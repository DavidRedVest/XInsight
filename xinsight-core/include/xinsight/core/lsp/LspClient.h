#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "xinsight/core/IUiDispatcher.h"

namespace xinsight::core::lsp {

// Parses zero or more complete `Content-Length: N\r\n\r\n<json>` frames out
// of `buffer`, appending each parsed JSON body to the result and erasing
// the consumed bytes from `buffer` in place -- so a frame split across
// multiple reads (a header cut in half, or a body cut mid-stream) just
// leaves its partial bytes in `buffer` for the next call to resume from.
// A malformed header or an unparsable JSON body is skipped rather than
// left to corrupt the rest of the stream's framing -- LSP servers don't
// reissue malformed messages, so there's nothing to recover by keeping it.
// PRD 8.4: "帧解析器必须单测" -- exposed specifically so this can be tested
// without spawning a real language server.
std::vector<nlohmann::json> extractFrames(std::string &buffer);

// Encodes one JSON-RPC message as a Content-Length-framed byte string
// ready to write to the server's stdin.
std::string encodeFrame(const nlohmann::json &message);

// The single entry point for clangd communication (PRD 5.1/5.4/8.4):
// spawns the server, drives the initialize/initialized handshake, and
// exposes request/notification send plus response/notification dispatch.
// Owns its own read thread (core has its own I/O threads, never borrows
// the GUI event loop, per PRD 5.1) -- every callback fires via
// IUiDispatcher::post on the UI thread, never inline on the read thread.
class LspClient {
public:
    // `result` is set on success, `error` (the LSP error object) on
    // failure; exactly one of the two is set.
    using ResponseCallback =
        std::function<void(std::optional<nlohmann::json> result, std::optional<nlohmann::json> error)>;
    // Server-initiated notifications (e.g. textDocument/publishDiagnostics,
    // $/progress) and server-initiated requests alike (the latter are
    // auto-acked with an empty result first -- see LspClient.cpp -- since
    // this client implements no server->client request methods).
    using NotificationCallback = std::function<void(const std::string &method, const nlohmann::json &params)>;

    explicit LspClient(IUiDispatcher &dispatcher);
    ~LspClient();
    LspClient(const LspClient &) = delete;
    LspClient &operator=(const LspClient &) = delete;

    // Spawns `command` (e.g. {"clangd", "--compile-commands-dir=..."})
    // and performs the initialize/initialized handshake against
    // `rootUri` (a file:// URI). `onReady(success)` fires exactly once
    // (via dispatcher) once the handshake completes or fails; `success`
    // is false if the process couldn't be spawned or the server returned
    // an error from `initialize`. Calling start() while already running
    // stops the previous instance first.
    void start(std::vector<std::string> command, std::string rootUri, std::function<void(bool)> onReady);

    // Kills the server and joins the read thread. Any requests still
    // awaiting a response are dropped without their callback firing.
    // Safe to call when not running.
    void stop();

    bool isRunning() const;

    void setOnNotification(NotificationCallback callback);

    // Sends a request; `callback` fires (via dispatcher) with the
    // response when it arrives. Returns the assigned request id.
    int64_t sendRequest(std::string method, nlohmann::json params, ResponseCallback callback);

    void sendNotification(std::string method, nlohmann::json params);

private:
    void readLoop();
    void dispatchIncoming(const nlohmann::json &message);
    void writeMessage(const nlohmann::json &message);

    IUiDispatcher &dispatcher_;

    struct Impl; // hides reproc::process from this public header
    std::unique_ptr<Impl> impl_;

    std::thread readThread_;
    std::atomic<bool> running_{false};
    std::mutex writeMutex_; // serializes writes to the child's stdin
    std::mutex pendingMutex_; // guards pendingRequests_
    std::atomic<int64_t> nextId_{1};
    std::map<int64_t, ResponseCallback> pendingRequests_;
    NotificationCallback onNotification_;
};

} // namespace xinsight::core::lsp
