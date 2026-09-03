#include "xinsight/core/lsp/LspClient.h"

#include <array>
#include <cctype>

#include <nlohmann/json.hpp>
#include <reproc++/reproc.hpp>

namespace xinsight::core::lsp {

std::vector<nlohmann::json> extractFrames(std::string &buffer) {
    std::vector<nlohmann::json> result;

    while (true) {
        size_t headerEnd = buffer.find("\r\n\r\n");
        if (headerEnd == std::string::npos) break; // header not fully received yet

        std::string header = buffer.substr(0, headerEnd);
        size_t clPos = header.find("Content-Length:");
        if (clPos == std::string::npos) {
            // No Content-Length in this header block -- can't recover a
            // frame boundary from it, so drop it and try to resync on
            // whatever follows.
            buffer.erase(0, headerEnd + 4);
            continue;
        }

        size_t numStart = clPos + std::string("Content-Length:").size();
        while (numStart < header.size() && (header[numStart] == ' ' || header[numStart] == '\t')) ++numStart;
        size_t numEnd = numStart;
        while (numEnd < header.size() && std::isdigit(static_cast<unsigned char>(header[numEnd]))) ++numEnd;
        if (numEnd == numStart) {
            buffer.erase(0, headerEnd + 4);
            continue;
        }

        long contentLength = std::stol(header.substr(numStart, numEnd - numStart));
        size_t bodyStart = headerEnd + 4;
        if (contentLength < 0 || buffer.size() < bodyStart + static_cast<size_t>(contentLength)) {
            break; // body not fully received yet
        }

        std::string body = buffer.substr(bodyStart, static_cast<size_t>(contentLength));
        buffer.erase(0, bodyStart + static_cast<size_t>(contentLength));

        try {
            result.push_back(nlohmann::json::parse(body));
        } catch (const nlohmann::json::parse_error &) {
            // Unparsable body: skip it (framing itself is still intact --
            // we consumed exactly Content-Length bytes) and keep going.
        }
    }

    return result;
}

std::string encodeFrame(const nlohmann::json &message) {
    std::string body = message.dump();
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

struct LspClient::Impl {
    reproc::process process;
};

LspClient::LspClient(IUiDispatcher &dispatcher) : dispatcher_(dispatcher), impl_(std::make_unique<Impl>()) {}

LspClient::~LspClient() { stop(); }

bool LspClient::isRunning() const { return running_.load(); }

void LspClient::setOnNotification(NotificationCallback callback) { onNotification_ = std::move(callback); }

void LspClient::start(std::vector<std::string> command, std::string rootUri, std::function<void(bool)> onReady) {
    stop(); // clean slate if already running

    reproc::options options;
    options.redirect.in.type = reproc::redirect::pipe;

    std::error_code ec = impl_->process.start(command, options);
    if (ec) {
        dispatcher_.post([onReady]() { onReady(false); });
        return;
    }

    running_.store(true);
    readThread_ = std::thread([this] { readLoop(); });

    nlohmann::json initParams = {
        {"processId", nullptr},
        {"rootUri", rootUri},
        {"capabilities", nlohmann::json::object()},
    };
    sendRequest("initialize", initParams,
                [this, onReady](std::optional<nlohmann::json> result, std::optional<nlohmann::json> error) {
                    if (error || !result) {
                        onReady(false);
                        return;
                    }
                    sendNotification("initialized", nlohmann::json::object());
                    onReady(true);
                });
}

void LspClient::stop() {
    if (!running_.exchange(false)) return;

    impl_->process.kill(); // unblocks readLoop_'s blocking read() on another thread
    if (readThread_.joinable()) readThread_.join();

    std::lock_guard<std::mutex> lock(pendingMutex_);
    pendingRequests_.clear();
}

void LspClient::readLoop() {
    std::string buffer;
    std::array<uint8_t, 8192> chunk{};

    while (running_.load()) {
        auto [bytesRead, ec] = impl_->process.read(reproc::stream::out, chunk.data(), chunk.size());
        if (ec) break; // EOF (process exited) or a real error either way stop reading

        buffer.append(reinterpret_cast<const char *>(chunk.data()), bytesRead);
        for (const nlohmann::json &frame : extractFrames(buffer)) dispatchIncoming(frame);
    }

    running_.store(false);
}

void LspClient::dispatchIncoming(const nlohmann::json &message) {
    bool hasId = message.contains("id") && !message["id"].is_null();
    bool hasMethod = message.contains("method");

    if (hasId && !hasMethod) {
        // A response to one of our own requests.
        int64_t id = message["id"].get<int64_t>();
        ResponseCallback callback;
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            auto it = pendingRequests_.find(id);
            if (it == pendingRequests_.end()) return; // stale/unknown id (e.g. we already stopped)
            callback = std::move(it->second);
            pendingRequests_.erase(it);
        }
        std::optional<nlohmann::json> result = message.contains("result") ? std::optional(message["result"]) : std::nullopt;
        std::optional<nlohmann::json> error = message.contains("error") ? std::optional(message["error"]) : std::nullopt;
        dispatcher_.post([callback, result, error]() { callback(result, error); });
        return;
    }

    if (hasMethod) {
        std::string method = message["method"].get<std::string>();
        nlohmann::json params = message.contains("params") ? message["params"] : nlohmann::json::object();

        if (hasId) {
            // A server->client request. This client implements no such
            // methods, but a strict server may block waiting for *some*
            // reply -- an empty-result ack is enough to unstick it.
            nlohmann::json response = {{"jsonrpc", "2.0"}, {"id", message["id"]}, {"result", nullptr}};
            writeMessage(response);
        }

        NotificationCallback callback = onNotification_;
        if (callback) dispatcher_.post([callback, method, params]() { callback(method, params); });
    }
}

void LspClient::writeMessage(const nlohmann::json &message) {
    std::string framed = encodeFrame(message);
    std::lock_guard<std::mutex> lock(writeMutex_);
    impl_->process.write(reinterpret_cast<const uint8_t *>(framed.data()), framed.size());
}

int64_t LspClient::sendRequest(std::string method, nlohmann::json params, ResponseCallback callback) {
    int64_t id = nextId_.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pendingRequests_[id] = std::move(callback);
    }
    nlohmann::json message = {{"jsonrpc", "2.0"}, {"id", id}, {"method", std::move(method)}, {"params", std::move(params)}};
    writeMessage(message);
    return id;
}

void LspClient::sendNotification(std::string method, nlohmann::json params) {
    nlohmann::json message = {{"jsonrpc", "2.0"}, {"method", std::move(method)}, {"params", std::move(params)}};
    writeMessage(message);
}

} // namespace xinsight::core::lsp
