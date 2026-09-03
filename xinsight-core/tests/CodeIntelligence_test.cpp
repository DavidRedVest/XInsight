#include <algorithm>
#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <nlohmann/json.hpp>
#include <thread>

#include "support/ImmediateUiDispatcher.h"
#include "xinsight/core/intel/CodeIntelligence.h"
#include "xinsight/core/lsp/LspClient.h"

using namespace xinsight::core::intel;
namespace fs = std::filesystem;

namespace {

class TempIndexDir {
public:
    TempIndexDir() {
        root_ = fs::temp_directory_path() /
                fs::path("xinsight-codeintel-test-" +
                         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root_);
    }
    ~TempIndexDir() {
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

} // namespace

TEST_CASE("updateFileIndex populates definitions and references queryable by name") {
    TreeSitterEngine engine;
    xinsight::core::testing::ImmediateUiDispatcher dispatcher;
    CodeIntelligence intel(engine, std::make_shared<InMemorySymbolIndex>(), dispatcher);

    std::string source = "int compute_widget(int value) {\n    return value * 2;\n}\n";
    ParsedDocument doc = engine.parse(Language::C, source);
    REQUIRE(doc.valid());

    intel.updateFileIndex("/proj/widget.c", doc);

    auto defs = intel.findDefinition("compute_widget");
    REQUIRE(defs.size() == 1);
    CHECK(defs[0].file == "/proj/widget.c");
    CHECK(defs[0].kind == SymbolKind::Function);

    auto refs = intel.findReferences("value");
    CHECK(refs.size() >= 2); // parameter + use inside the function body
}

TEST_CASE("updateFileIndex after an incremental edit reflects the new (shifted) position") {
    TreeSitterEngine engine;
    xinsight::core::testing::ImmediateUiDispatcher dispatcher;
    CodeIntelligence intel(engine, std::make_shared<InMemorySymbolIndex>(), dispatcher);

    std::string source = "int foo(void) {\n    return 1;\n}\n";
    ParsedDocument doc = engine.parse(Language::C, source);
    REQUIRE(doc.valid());

    intel.updateFileIndex("/proj/foo.c", doc);
    auto before = intel.findDefinition("foo");
    REQUIRE(before.size() == 1);
    CHECK(before[0].startRow == 0);

    // Insert a comment line before `int foo...`, pushing it down one line --
    // this is PRD 8.7's edit -> reparse -> index-slice-update closure.
    std::string insertion = "// header comment\n";
    Edit edit;
    edit.startByte = 0;
    edit.oldEndByte = 0;
    edit.newEndByte = static_cast<uint32_t>(insertion.size());
    edit.startRow = 0;
    edit.startColumn = 0;
    edit.oldEndRow = 0;
    edit.oldEndColumn = 0;
    edit.newEndRow = 1;
    edit.newEndColumn = 0;

    std::string newSource = insertion + source;
    engine.applyEdit(doc, edit, newSource);

    intel.updateFileIndex("/proj/foo.c", doc);
    auto after = intel.findDefinition("foo");
    REQUIRE(after.size() == 1); // replaced, not duplicated
    CHECK(after[0].startRow == 1);
    CHECK(newSource.substr(after[0].startByte, 3) == "foo");
}

TEST_CASE("removeFileFromIndex clears a file's definitions") {
    TreeSitterEngine engine;
    xinsight::core::testing::ImmediateUiDispatcher dispatcher;
    CodeIntelligence intel(engine, std::make_shared<InMemorySymbolIndex>(), dispatcher);

    ParsedDocument doc = engine.parse(Language::C, "int foo(void) { return 1; }\n");
    REQUIRE(doc.valid());
    intel.updateFileIndex("/proj/foo.c", doc);
    REQUIRE(intel.findDefinition("foo").size() == 1);

    intel.removeFileFromIndex("/proj/foo.c");
    CHECK(intel.findDefinition("foo").empty());
}

TEST_CASE("searchWorkspaceSymbols reflects indexed definitions") {
    TreeSitterEngine engine;
    xinsight::core::testing::ImmediateUiDispatcher dispatcher;
    CodeIntelligence intel(engine, std::make_shared<InMemorySymbolIndex>(), dispatcher);

    ParsedDocument doc = engine.parse(Language::C, "int compute_widget(void) { return 1; }\n");
    REQUIRE(doc.valid());
    intel.updateFileIndex("/proj/widget.c", doc);

    auto results = intel.searchWorkspaceSymbols("widget");
    REQUIRE(results.size() == 1);
    CHECK(results[0].name == "compute_widget");
}

TEST_CASE("indexProject scans real files in the background and reports progress + completion") {
    TempIndexDir dir;
    dir.writeFile("a.c", "int alpha_func(void) { return 1; }\n");
    dir.writeFile("sub/b.c", "int beta_func(void) { return alpha_func(); }\n");
    dir.writeFile("notes.txt", "alpha_func is mentioned here but this isn't C\n");

    TreeSitterEngine engine;
    xinsight::core::testing::ImmediateUiDispatcher dispatcher;
    CodeIntelligence intel(engine, std::make_shared<InMemorySymbolIndex>(), dispatcher);

    std::promise<void> completePromise;
    auto completeFuture = completePromise.get_future();
    size_t lastDone = 0, lastTotal = 0;
    intel.setOnIndexProgress([&](size_t done, size_t total) {
        lastDone = done;
        lastTotal = total;
    });
    intel.setOnIndexComplete([&]() { completePromise.set_value(); });

    intel.indexProject({dir.root() / "a.c", dir.root() / "sub" / "b.c", dir.root() / "notes.txt"});

    auto status = completeFuture.wait_for(std::chrono::seconds(10));
    REQUIRE(status == std::future_status::ready);

    CHECK(lastTotal == 3);
    CHECK(lastDone == 3);

    auto alphaDefs = intel.findDefinition("alpha_func");
    REQUIRE(alphaDefs.size() == 1);
    CHECK(alphaDefs[0].file == (dir.root() / "a.c").string());

    auto betaDefs = intel.findDefinition("beta_func");
    REQUIRE(betaDefs.size() == 1);
    CHECK(betaDefs[0].file == (dir.root() / "sub" / "b.c").string());

    // beta_func's body calls alpha_func -- a reference site beyond a.c's
    // own definition, proving cross-file reference indexing works.
    auto alphaRefs = intel.findReferences("alpha_func");
    CHECK(alphaRefs.size() == 2);

    // notes.txt has no recognized language extension, so despite
    // containing the text "alpha_func" it must never have been parsed --
    // its mention must not appear as an extra reference.
    bool anyFromNotes = std::any_of(alphaRefs.begin(), alphaRefs.end(),
                                     [&](const auto &r) { return r.file == (dir.root() / "notes.txt").string(); });
    CHECK_FALSE(anyFromNotes);
}

TEST_CASE("indexProject leaves previously indexed files alone when given a narrower list") {
    TempIndexDir dir;
    fs::path fileA = dir.writeFile("a.c", "int alpha_func(void) { return 1; }\n");
    fs::path fileB = dir.writeFile("b.c", "int beta_func(void) { return 2; }\n");

    TreeSitterEngine engine;
    xinsight::core::testing::ImmediateUiDispatcher dispatcher;
    CodeIntelligence intel(engine, std::make_shared<InMemorySymbolIndex>(), dispatcher);

    {
        std::promise<void> firstComplete;
        auto firstFuture = firstComplete.get_future();
        intel.setOnIndexComplete([&]() { firstComplete.set_value(); });
        intel.indexProject({fileA, fileB});
        REQUIRE(firstFuture.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    }
    REQUIRE(intel.findDefinition("alpha_func").size() == 1);
    REQUIRE(intel.findDefinition("beta_func").size() == 1);

    {
        std::promise<void> secondComplete;
        auto secondFuture = secondComplete.get_future();
        intel.setOnIndexComplete([&]() { secondComplete.set_value(); });
        intel.indexProject({fileA}); // b.c intentionally omitted
        REQUIRE(secondFuture.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    }

    CHECK(intel.findDefinition("alpha_func").size() == 1); // re-verified, still present
    CHECK(intel.findDefinition("beta_func").size() == 1);  // untouched, not wiped
}

// ---------------------------------------------------------------------
// Async routing (PRD 5.2): tree-sitter fallback when clangd isn't
// configured, and real precise routing once it is (against real clangd).
// ---------------------------------------------------------------------

TEST_CASE("findDefinitionAsync: fires synchronously with precise=false when clangd isn't configured") {
    TreeSitterEngine engine;
    xinsight::core::testing::ImmediateUiDispatcher dispatcher;
    CodeIntelligence intel(engine, std::make_shared<InMemorySymbolIndex>(), dispatcher);

    ParsedDocument doc = engine.parse(Language::C, "int compute_widget(void) { return 1; }\n");
    REQUIRE(doc.valid());
    intel.updateFileIndex("/proj/widget.c", doc);

    bool firedSynchronously = false;
    DefinitionResult result;
    intel.findDefinitionAsync("compute_widget", "/proj/main.c", 0, "", 0, [&](DefinitionResult r) {
        result = std::move(r);
        firedSynchronously = true;
    });

    CHECK(firedSynchronously); // no clangd configured -> immediate, not deferred
    CHECK_FALSE(result.precise);
    REQUIRE(result.candidates.size() == 1);
    CHECK(result.candidates[0].file == "/proj/widget.c");
    CHECK(result.candidates[0].name == "compute_widget");
}

TEST_CASE("findReferencesAsync/searchWorkspaceSymbolsAsync: also fall back synchronously without clangd") {
    TreeSitterEngine engine;
    xinsight::core::testing::ImmediateUiDispatcher dispatcher;
    CodeIntelligence intel(engine, std::make_shared<InMemorySymbolIndex>(), dispatcher);

    ParsedDocument doc = engine.parse(Language::C, "int compute_widget(void) { return compute_widget(); }\n");
    REQUIRE(doc.valid());
    intel.updateFileIndex("/proj/widget.c", doc);

    bool refsFired = false;
    intel.findReferencesAsync("compute_widget", "/proj/main.c", 0, "", 0, [&](ReferencesResult r) {
        CHECK_FALSE(r.precise);
        CHECK(r.candidates.size() == 2); // definition + the recursive call
        refsFired = true;
    });
    CHECK(refsFired);

    bool symbolsFired = false;
    intel.searchWorkspaceSymbolsAsync("widget", 200, [&](WorkspaceSymbolResult r) {
        CHECK_FALSE(r.precise);
        REQUIRE(r.candidates.size() == 1);
        CHECK(r.candidates[0].name == "compute_widget");
        symbolsFired = true;
    });
    CHECK(symbolsFired);
}

TEST_CASE("notifyFileOpened/Changed/Saved: safe no-ops when clangd isn't configured") {
    TreeSitterEngine engine;
    xinsight::core::testing::ImmediateUiDispatcher dispatcher;
    CodeIntelligence intel(engine, std::make_shared<InMemorySymbolIndex>(), dispatcher);

    CHECK_FALSE(intel.isClangdConfigured());
    intel.notifyFileOpened("/proj/main.c", "c", "int main(void) { return 0; }\n");
    intel.notifyFileChanged("/proj/main.c", "int main(void) { return 1; }\n");
    intel.notifyFileSaved("/proj/main.c");
    // No crash, no hang -- that's the whole assertion.
}

namespace {

bool clangdAvailable() {
    static const bool available = [] {
        xinsight::core::testing::ImmediateUiDispatcher dispatcher;
        xinsight::core::lsp::LspClient client(dispatcher);
        std::promise<bool> ready;
        auto future = ready.get_future();
        client.start({"clangd"}, "file:///tmp", [&](bool ok) { ready.set_value(ok); });
        bool ok = future.wait_for(std::chrono::seconds(10)) == std::future_status::ready && future.get();
        client.stop();
        return ok;
    }();
    return available;
}

void writeCompileCommands(const fs::path &root, const std::vector<fs::path> &sourceFiles) {
    nlohmann::json entries = nlohmann::json::array();
    for (const fs::path &file : sourceFiles) {
        entries.push_back({
            {"directory", root.string()},
            {"command", "cc -c " + file.string()},
            {"file", file.string()},
        });
    }
    std::ofstream out(root / "compile_commands.json", std::ios::binary);
    out << entries.dump(2);
}

} // namespace

TEST_CASE("findDefinitionAsync: routes to real clangd (precise=true) once the TU is ready") {
    if (!clangdAvailable()) {
        MESSAGE("clangd not available on PATH -- skipping real-server test");
        return;
    }

    TempIndexDir dir;
    fs::path fileA = dir.writeFile("a.c", "int compute_widget(int value) {\n    return value * 2;\n}\n"
                                           "int main(void) {\n    int r = compute_widget(3);\n    return r;\n}\n");
    writeCompileCommands(dir.root(), {fileA});

    TreeSitterEngine engine;
    xinsight::core::testing::ImmediateUiDispatcher dispatcher;
    CodeIntelligence intel(engine, std::make_shared<InMemorySymbolIndex>(), dispatcher);

    // Before configureClangd(): must behave exactly like M2/M3 (no
    // regression) -- populate the tree-sitter index the usual way first.
    {
        std::promise<void> indexComplete;
        auto indexFuture = indexComplete.get_future();
        intel.setOnIndexComplete([&]() { indexComplete.set_value(); });
        intel.indexProject({fileA});
        REQUIRE(indexFuture.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    }
    auto fallback = intel.findDefinition("compute_widget");
    REQUIRE(fallback.size() == 1);

    std::promise<bool> clangdReady;
    auto clangdReadyFuture = clangdReady.get_future();
    intel.setOnClangdStatusChanged([&](bool running) { clangdReady.set_value(running); });
    intel.configureClangd(dir.root(), dir.root());
    REQUIRE(clangdReadyFuture.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    REQUIRE(clangdReadyFuture.get());
    REQUIRE(intel.isClangdConfigured());

    std::ifstream in(fileA, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    intel.notifyFileOpened(fileA.string(), "c", text);

    // Poll findDefinitionAsync until clangd's TU is ready (isFileReady()
    // isn't directly observable through CodeIntelligence's public API, so
    // just retry the actual call -- it transparently falls back to
    // tree-sitter, precise=false, until then).
    DefinitionResult result;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
        std::string line5 = "    int r = compute_widget(3);";
        uint32_t col = static_cast<uint32_t>(line5.find("compute_widget"));

        std::promise<DefinitionResult> resultPromise;
        auto resultFuture = resultPromise.get_future();
        intel.findDefinitionAsync("compute_widget", fileA.string(), 4, line5, col,
                                   [&](DefinitionResult r) { resultPromise.set_value(std::move(r)); });
        REQUIRE(resultFuture.wait_for(std::chrono::seconds(15)) == std::future_status::ready);
        result = resultFuture.get();

        if (result.precise) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    CHECK(result.precise);
    REQUIRE_FALSE(result.candidates.empty());
    CHECK(fs::canonical(result.candidates[0].file) == fs::canonical(fileA));
    CHECK(result.candidates[0].startRow == 0); // `int compute_widget(int value) {`
}
