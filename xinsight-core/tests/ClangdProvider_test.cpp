#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <nlohmann/json.hpp>
#include <thread>

#include "support/ImmediateUiDispatcher.h"
#include "xinsight/core/lsp/ClangdProvider.h"

using namespace xinsight::core::lsp;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------
// utf8ByteColumnToUtf16 / utf16ColumnToUtf8ByteColumn: pure, no process.
// ---------------------------------------------------------------------

TEST_CASE("utf8ByteColumnToUtf16: pure-ASCII text has identical byte and UTF-16 columns") {
    std::string_view line = "int compute_widget(int value) {";
    CHECK(utf8ByteColumnToUtf16(line, 0) == 0);
    CHECK(utf8ByteColumnToUtf16(line, 4) == 4);
    CHECK(utf8ByteColumnToUtf16(line, static_cast<uint32_t>(line.size())) == line.size());
}

TEST_CASE("utf8ByteColumnToUtf16: multi-byte UTF-8 (Chinese) collapses to 1 UTF-16 unit per character") {
    // "护眼" is 2 codepoints, 3 UTF-8 bytes each (6 bytes total), but only
    // 2 UTF-16 code units (both in the BMP) -- exactly the PRD 2.3
    // Chinese-comment scenario this conversion exists for.
    std::string line = "护眼 mode";
    CHECK(utf8ByteColumnToUtf16(line, 0) == 0);
    CHECK(utf8ByteColumnToUtf16(line, 3) == 1);  // after "护" (3 bytes -> 1 UTF-16 unit)
    CHECK(utf8ByteColumnToUtf16(line, 6) == 2);  // after "护眼" (6 bytes -> 2 UTF-16 units)
    CHECK(utf8ByteColumnToUtf16(line, 7) == 3);  // after "护眼 " (space is 1 byte -> 1 more unit)
}

TEST_CASE("utf16ColumnToUtf8ByteColumn: inverse of utf8ByteColumnToUtf16 for Chinese text") {
    std::string line = "护眼 mode";
    CHECK(utf16ColumnToUtf8ByteColumn(line, 0) == 0);
    CHECK(utf16ColumnToUtf8ByteColumn(line, 1) == 3);
    CHECK(utf16ColumnToUtf8ByteColumn(line, 2) == 6);
    CHECK(utf16ColumnToUtf8ByteColumn(line, 3) == 7);
}

TEST_CASE("utf8/utf16 column conversion: round-trips for mixed ASCII + CJK content") {
    std::string line = "// 计算 widget 的值 compute_widget(w)";
    uint32_t afterComment = utf8ByteColumnToUtf16(line, 2); // after "//"
    CHECK(utf16ColumnToUtf8ByteColumn(line, afterComment) == 2);

    auto fullLen = static_cast<uint32_t>(line.size());
    uint32_t fullUtf16Len = utf8ByteColumnToUtf16(line, fullLen);
    CHECK(utf16ColumnToUtf8ByteColumn(line, fullUtf16Len) == fullLen);
}

TEST_CASE("utf8ByteColumnToUtf16: an out-of-range byteColumn clamps to the line's full length") {
    std::string_view line = "abc";
    CHECK(utf8ByteColumnToUtf16(line, 1000) == 3);
}

// ---------------------------------------------------------------------
// Real end-to-end tests against /usr/bin/clangd, using a proper
// compile_commands.json so clangd has a real compilation database (not
// just its "fallback" heuristic mode).
// ---------------------------------------------------------------------

namespace {

class TempClangdProject {
public:
    TempClangdProject() {
        root_ = fs::temp_directory_path() /
                fs::path("xinsight-clangdprovider-test-" +
                         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root_);
    }
    ~TempClangdProject() {
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

    void writeCompileCommands(const std::vector<fs::path> &sourceFiles) {
        nlohmann::json entries = nlohmann::json::array();
        for (const fs::path &file : sourceFiles) {
            entries.push_back({
                {"directory", root_.string()},
                {"command", "cc -c " + file.string()},
                {"file", file.string()},
            });
        }
        std::ofstream out(root_ / "compile_commands.json", std::ios::binary);
        out << entries.dump(2);
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

// Sends didOpen and polls isFileReady() until publishDiagnostics arrives
// or the timeout elapses.
bool waitUntilReady(ClangdProvider &provider, const std::string &file, std::chrono::seconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (provider.isFileReady(file)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return provider.isFileReady(file);
}

} // namespace

TEST_CASE("ClangdProvider: findDefinition resolves a real cross-file jump via real clangd") {
    if (!clangdAvailable()) {
        MESSAGE("clangd not available on PATH -- skipping real-server test");
        return;
    }

    TempClangdProject project;
    fs::path header = project.writeFile("widget.h", "int compute_widget(int value);\n");
    fs::path source = project.writeFile("main.c", "#include \"widget.h\"\n"
                                                   "// 计算函数的调用\n"
                                                   "int main(void) {\n"
                                                   "    int result = compute_widget(5);\n"
                                                   "    return result;\n"
                                                   "}\n");
    fs::path widgetImpl =
        project.writeFile("widget.c", "#include \"widget.h\"\n"
                                       "int compute_widget(int value) {\n"
                                       "    return value * 2;\n"
                                       "}\n");
    project.writeCompileCommands({source, widgetImpl});

    xinsight::core::testing::ImmediateUiDispatcher dispatcher;
    LspClient client(dispatcher);
    ClangdProvider provider(client);

    std::promise<bool> ready;
    auto readyFuture = ready.get_future();
    client.start({"clangd", "--compile-commands-dir=" + project.root().string()}, "file://" + project.root().string(),
                 [&](bool ok) { ready.set_value(ok); });
    REQUIRE(readyFuture.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    REQUIRE(readyFuture.get());

    std::ifstream in(source, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    provider.didOpen(source.string(), "c", text);

    // Also open widget.c explicitly, matching real app usage (the user
    // would have both files open in tabs) -- this gives clangd an
    // explicit ASTWorker/preamble for widget.c immediately, rather than
    // relying solely on compile_commands.json's background cross-TU
    // indexing, whose completion isn't signaled by any per-file
    // diagnostic and was observed to occasionally still be in flight
    // well past 20s under load (a real clangd characteristic, not a bug
    // in this wrapper -- see git history for the flakier polling version
    // of this test this replaced).
    std::ifstream widgetIn(widgetImpl, std::ios::binary);
    std::string widgetText((std::istreambuf_iterator<char>(widgetIn)), std::istreambuf_iterator<char>());
    provider.didOpen(widgetImpl.string(), "c", widgetText);

    REQUIRE(waitUntilReady(provider, source.string(), std::chrono::seconds(20)));
    REQUIRE(waitUntilReady(provider, widgetImpl.string(), std::chrono::seconds(20)));

    // Line 3 (0-based) is `    int result = compute_widget(5);` -- the
    // call site. Byte column of "compute_widget" on that line:
    std::string line4 = "    int result = compute_widget(5);";
    uint32_t byteColumn = static_cast<uint32_t>(line4.find("compute_widget"));

    std::promise<std::vector<PreciseLocation>> defPromise;
    auto defFuture = defPromise.get_future();
    provider.findDefinition(source.string(), 3, line4, byteColumn,
                             [&](std::vector<PreciseLocation> found) { defPromise.set_value(std::move(found)); });
    REQUIRE(defFuture.wait_for(std::chrono::seconds(15)) == std::future_status::ready);
    std::vector<PreciseLocation> locations = defFuture.get();

    REQUIRE_FALSE(locations.empty());
    // fs::canonical, not a plain string compare: clangd resolves macOS's
    // /var -> /private/var symlink in its URIs, but fs::temp_directory_path()
    // doesn't -- both name the same file, just spelled differently.
    CHECK(fs::canonical(locations[0].file) == fs::canonical(widgetImpl));
    CHECK(locations[0].startRow == 1); // `int compute_widget(int value) {` is line 1 (0-based)

    client.stop();
}

TEST_CASE("ClangdProvider: workspace symbol search returns real results via real clangd") {
    if (!clangdAvailable()) {
        MESSAGE("clangd not available on PATH -- skipping real-server test");
        return;
    }

    TempClangdProject project;
    fs::path source = project.writeFile("widget.c", "int compute_widget_unique_xyz(int value) {\n    return value;\n}\n");
    project.writeCompileCommands({source});

    xinsight::core::testing::ImmediateUiDispatcher dispatcher;
    LspClient client(dispatcher);
    ClangdProvider provider(client);

    std::promise<bool> ready;
    auto readyFuture = ready.get_future();
    client.start({"clangd", "--compile-commands-dir=" + project.root().string()}, "file://" + project.root().string(),
                 [&](bool ok) { ready.set_value(ok); });
    REQUIRE(readyFuture.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    REQUIRE(readyFuture.get());

    std::ifstream in(source, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    provider.didOpen(source.string(), "c", text);
    REQUIRE(waitUntilReady(provider, source.string(), std::chrono::seconds(20)));

    std::promise<std::vector<WorkspaceSymbolMatch>> symbolsPromise;
    auto symbolsFuture = symbolsPromise.get_future();
    provider.searchWorkspaceSymbols("compute_widget_unique_xyz",
                                     [&](std::vector<WorkspaceSymbolMatch> matches) { symbolsPromise.set_value(std::move(matches)); });

    REQUIRE(symbolsFuture.wait_for(std::chrono::seconds(15)) == std::future_status::ready);
    std::vector<WorkspaceSymbolMatch> matches = symbolsFuture.get();

    bool found = false;
    for (const auto &match : matches) {
        if (match.name == "compute_widget_unique_xyz") found = true;
    }
    CHECK(found);

    client.stop();
}
