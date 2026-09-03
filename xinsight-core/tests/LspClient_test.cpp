#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <nlohmann/json.hpp>

#include "support/ImmediateUiDispatcher.h"
#include "xinsight/core/lsp/LspClient.h"

using namespace xinsight::core::lsp;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------
// extractFrames()/encodeFrame(): pure, no process spawned (PRD 8.4:
// "帧解析器必须单测").
// ---------------------------------------------------------------------

TEST_CASE("extractFrames: a single complete frame") {
    std::string body = "{\"a\":1,\"b\":2}";
    std::string buffer = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    auto frames = extractFrames(buffer);
    REQUIRE(frames.size() == 1);
    CHECK(frames[0]["a"] == 1);
    CHECK(frames[0]["b"] == 2);
    CHECK(buffer.empty()); // fully consumed
}

TEST_CASE("extractFrames: two frames arriving in one read (pipelined)") {
    std::string first = "{\"n\":1}";
    std::string second = "{\"n\":2}";
    std::string buffer = "Content-Length: " + std::to_string(first.size()) + "\r\n\r\n" + first +
                          "Content-Length: " + std::to_string(second.size()) + "\r\n\r\n" + second;

    auto frames = extractFrames(buffer);
    REQUIRE(frames.size() == 2);
    CHECK(frames[0]["n"] == 1);
    CHECK(frames[1]["n"] == 2);
    CHECK(buffer.empty());
}

TEST_CASE("extractFrames: a header split across two reads yields nothing until complete") {
    std::string buffer = "Content-Length: 7\r\n\r"; // missing the final \n
    auto frames = extractFrames(buffer);
    CHECK(frames.empty());
    CHECK(buffer == "Content-Length: 7\r\n\r"); // untouched, waiting for more

    buffer += "\n{\"x\":9}";
    frames = extractFrames(buffer);
    REQUIRE(frames.size() == 1);
    CHECK(frames[0]["x"] == 9);
}

TEST_CASE("extractFrames: a body split across two reads yields nothing until complete") {
    std::string body = "{\"value\":\"hello world\"}";
    std::string header = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";

    std::string buffer = header + body.substr(0, 10); // partial body
    auto frames = extractFrames(buffer);
    CHECK(frames.empty());
    CHECK(buffer == header + body.substr(0, 10)); // untouched

    buffer += body.substr(10); // rest arrives
    frames = extractFrames(buffer);
    REQUIRE(frames.size() == 1);
    CHECK(frames[0]["value"] == "hello world");
}

TEST_CASE("extractFrames: an unparsable body is skipped without corrupting subsequent framing") {
    std::string badBody = "{not valid json";
    std::string goodBody = "{\"ok\":true}";
    std::string buffer = "Content-Length: " + std::to_string(badBody.size()) + "\r\n\r\n" + badBody +
                          "Content-Length: " + std::to_string(goodBody.size()) + "\r\n\r\n" + goodBody;

    auto frames = extractFrames(buffer);
    REQUIRE(frames.size() == 1); // the malformed one is silently dropped
    CHECK(frames[0]["ok"] == true);
    CHECK(buffer.empty());
}

TEST_CASE("extractFrames: a header with no Content-Length is dropped, not left to jam framing") {
    std::string goodBody = "{\"ok\":true}";
    std::string buffer =
        "X-Something: else\r\n\r\n" + std::string("garbage-not-consulted") +
        "Content-Length: " + std::to_string(goodBody.size()) + "\r\n\r\n" + goodBody;
    // Note: the bogus first "header" has no length info, so extractFrames
    // can only resynchronize at the next \r\n\r\n it finds -- this test
    // documents that behavior rather than asserting perfect recovery of
    // the "garbage-not-consulted" bytes (which get swept up as part of
    // resyncing on the next header boundary).
    auto frames = extractFrames(buffer);
    REQUIRE(frames.size() == 1);
    CHECK(frames[0]["ok"] == true);
}

TEST_CASE("encodeFrame: round-trips through extractFrames") {
    nlohmann::json message = {{"jsonrpc", "2.0"}, {"id", 42}, {"method", "textDocument/definition"}};
    std::string framed = encodeFrame(message);

    auto frames = extractFrames(framed);
    REQUIRE(frames.size() == 1);
    CHECK(frames[0]["id"] == 42);
    CHECK(frames[0]["method"] == "textDocument/definition");
}

TEST_CASE("encodeFrame: Content-Length matches the UTF-8 byte length, not character count") {
    // A non-ASCII value ("护眼", 6 UTF-8 bytes for 2 characters) -- if
    // Content-Length were computed in characters instead of bytes, a real
    // server would misframe the next message.
    nlohmann::json message = {{"jsonrpc", "2.0"}, {"method", "test"}, {"params", {{"note", "护眼"}}}};
    std::string framed = encodeFrame(message);

    size_t headerEnd = framed.find("\r\n\r\n");
    REQUIRE(headerEnd != std::string::npos);
    std::string body = framed.substr(headerEnd + 4);
    CHECK(framed.find("Content-Length: " + std::to_string(body.size())) == 0);
}

// ---------------------------------------------------------------------
// Real end-to-end tests against /usr/bin/clangd (Apple's Xcode-bundled
// clangd). Skipped gracefully if clangd isn't on PATH, matching this
// codebase's "prefer the real thing over mocks" convention (see
// SearchEngine_test.cpp's real `rg` subprocess tests).
// ---------------------------------------------------------------------

namespace {

class TempClangdDir {
public:
    TempClangdDir() {
        root_ = fs::temp_directory_path() /
                fs::path("xinsight-lspclient-test-" +
                         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root_);
    }
    ~TempClangdDir() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    fs::path writeFile(const std::string &relative, std::string_view content) {
        fs::path p = root_ / relative;
        fs::create_directories(p.parent_path());
        std::ofstream out(p, std::ios::binary);
        out << content;
        return p;
    }

    const fs::path &root() const { return root_; }

private:
    fs::path root_;
};

bool clangdAvailable() {
    static const bool available = [] {
        xinsight::core::testing::ImmediateUiDispatcher dispatcher;
        LspClient client(dispatcher);
        std::promise<bool> ready;
        auto future = ready.get_future();
        client.start({"clangd"}, "file:///tmp", [&](bool ok) { ready.set_value(ok); });
        bool ok = future.wait_for(std::chrono::seconds(10)) == std::future_status::ready && future.get();
        client.stop();
        return ok;
    }();
    return available;
}

} // namespace

TEST_CASE("LspClient: initialize handshake against real clangd") {
    if (!clangdAvailable()) {
        MESSAGE("clangd not available on PATH -- skipping real-server test");
        return;
    }

    TempClangdDir dir;
    xinsight::core::testing::ImmediateUiDispatcher dispatcher;
    LspClient client(dispatcher);

    std::promise<bool> ready;
    auto future = ready.get_future();
    client.start({"clangd"}, "file://" + dir.root().string(), [&](bool ok) { ready.set_value(ok); });

    REQUIRE(future.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    CHECK(future.get());
    CHECK(client.isRunning());

    client.stop();
    CHECK_FALSE(client.isRunning());
}

TEST_CASE("LspClient: didOpen + textDocument/documentSymbol returns real symbols from clangd") {
    if (!clangdAvailable()) {
        MESSAGE("clangd not available on PATH -- skipping real-server test");
        return;
    }

    TempClangdDir dir;
    fs::path file = dir.writeFile("widget.c", "int compute_widget(int value) {\n    return value * 2;\n}\n");

    xinsight::core::testing::ImmediateUiDispatcher dispatcher;
    LspClient client(dispatcher);

    std::promise<bool> ready;
    auto readyFuture = ready.get_future();
    client.start({"clangd"}, "file://" + dir.root().string(), [&](bool ok) { ready.set_value(ok); });
    REQUIRE(readyFuture.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    REQUIRE(readyFuture.get());

    std::string uri = "file://" + file.string();
    std::ifstream in(file, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    client.sendNotification("textDocument/didOpen", {{"textDocument",
                                                        {{"uri", uri}, {"languageId", "c"}, {"version", 1}, {"text", text}}}});

    std::promise<nlohmann::json> symbolsPromise;
    auto symbolsFuture = symbolsPromise.get_future();
    client.sendRequest("textDocument/documentSymbol", {{"textDocument", {{"uri", uri}}}},
                        [&](std::optional<nlohmann::json> result, std::optional<nlohmann::json> error) {
                            symbolsPromise.set_value(error ? nlohmann::json(nullptr) : result.value_or(nlohmann::json(nullptr)));
                        });

    REQUIRE(symbolsFuture.wait_for(std::chrono::seconds(15)) == std::future_status::ready);
    nlohmann::json symbols = symbolsFuture.get();
    REQUIRE(symbols.is_array());

    bool foundComputeWidget = false;
    for (const auto &symbol : symbols) {
        if (symbol.contains("name") && symbol["name"] == "compute_widget") foundComputeWidget = true;
    }
    CHECK(foundComputeWidget);

    client.stop();
}
