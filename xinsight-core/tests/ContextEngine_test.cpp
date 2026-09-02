#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <thread>

#include "support/ImmediateUiDispatcher.h"
#include "xinsight/core/context/ContextEngine.h"
#include "xinsight/core/intel/CodeIntelligence.h"
#include "xinsight/core/intel/SymbolIndex.h"

using namespace xinsight::core::context;
using namespace xinsight::core::intel;
namespace fs = std::filesystem;

namespace {

class TempContextDir {
public:
    TempContextDir() {
        root_ = fs::temp_directory_path() /
                fs::path("xinsight-contextengine-test-" +
                         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root_);
    }
    ~TempContextDir() {
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

// Runs indexProject synchronously (blocks until CodeIntelligence's own
// background scan completes) so tests can rely on the index being fully
// populated before exercising ContextEngine.
void indexAndWait(CodeIntelligence &intel, std::vector<fs::path> files) {
    std::promise<void> complete;
    auto future = complete.get_future();
    intel.setOnIndexComplete([&]() { complete.set_value(); });
    intel.indexProject(std::move(files));
    REQUIRE(future.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
}

} // namespace

TEST_CASE("ContextEngine: empty identifier clears immediately without waiting out the debounce") {
    TreeSitterEngine engine;
    xinsight::core::testing::ImmediateUiDispatcher dispatcher;
    CodeIntelligence intel(engine, std::make_shared<InMemorySymbolIndex>(), dispatcher);
    ContextEngine context(intel, dispatcher);

    std::promise<ContextResult> received;
    auto future = received.get_future();
    context.setOnContext([&](ContextResult result) { received.set_value(std::move(result)); });

    auto start = std::chrono::steady_clock::now();
    context.onCursorMoved("", "/proj/a.c");

    REQUIRE(future.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready);
    auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed < std::chrono::milliseconds(150)); // well under PRD's 150-250ms debounce window

    ContextResult result = future.get();
    CHECK(result.queriedName.empty());
    CHECK(result.candidates.empty());
}

TEST_CASE("ContextEngine: resolves a real definition after the debounce window") {
    TempContextDir dir;
    fs::path fileA = dir.writeFile("a.c", "int compute_widget(int value) {\n    return value * 2;\n}\n");

    TreeSitterEngine engine;
    xinsight::core::testing::ImmediateUiDispatcher dispatcher;
    CodeIntelligence intel(engine, std::make_shared<InMemorySymbolIndex>(), dispatcher);
    indexAndWait(intel, {fileA});

    ContextEngine context(intel, dispatcher);
    std::promise<ContextResult> received;
    auto future = received.get_future();
    context.setOnContext([&](ContextResult result) { received.set_value(std::move(result)); });

    context.onCursorMoved("compute_widget", fileA.string());

    REQUIRE(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    ContextResult result = future.get();

    CHECK(result.queriedName == "compute_widget");
    REQUIRE(result.candidates.size() == 1);
    CHECK(result.candidates[0].file == fileA.string());
    CHECK(result.candidates[0].signature.find("compute_widget") != std::string::npos);
    CHECK(result.candidates[0].snippet.find("return value * 2") != std::string::npos);
    CHECK(result.candidates[0].kind == SymbolKind::Function);
}

TEST_CASE("ContextEngine: a later cursor move before the debounce elapses supersedes the earlier one") {
    TempContextDir dir;
    fs::path fileA = dir.writeFile("a.c", "int foo(void) { return 1; }\nint bar(void) { return 2; }\n");

    TreeSitterEngine engine;
    xinsight::core::testing::ImmediateUiDispatcher dispatcher;
    CodeIntelligence intel(engine, std::make_shared<InMemorySymbolIndex>(), dispatcher);
    indexAndWait(intel, {fileA});

    ContextEngine context(intel, dispatcher);
    std::mutex resultsMutex;
    std::vector<ContextResult> results;
    context.setOnContext([&](ContextResult result) {
        std::lock_guard<std::mutex> lock(resultsMutex);
        results.push_back(std::move(result));
    });

    context.onCursorMoved("foo", fileA.string());
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // well inside the 200ms debounce window
    context.onCursorMoved("bar", fileA.string());

    // Give the (now-single) pending request time to resolve, plus margin.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::lock_guard<std::mutex> lock(resultsMutex);
    REQUIRE(results.size() == 1); // "foo" must never have fired
    CHECK(results[0].queriedName == "bar");
    REQUIRE(results[0].candidates.size() == 1);
    CHECK(results[0].candidates[0].signature.find("bar") != std::string::npos);
}

TEST_CASE("ContextEngine: candidates rank same-file > same-directory > rest-of-project") {
    TempContextDir dir;
    fs::path sameFile = dir.writeFile("main.c", "int shared(void) { return 0; }\nint caller(void) { return shared(); }\n");
    fs::path sameDirFile = dir.writeFile("helper.c", "int shared(void) { return 1; }\n");
    fs::path otherDirFile = dir.writeFile("sub/other.c", "int shared(void) { return 2; }\n");

    TreeSitterEngine engine;
    xinsight::core::testing::ImmediateUiDispatcher dispatcher;
    CodeIntelligence intel(engine, std::make_shared<InMemorySymbolIndex>(), dispatcher);
    indexAndWait(intel, {sameFile, sameDirFile, otherDirFile});

    ContextEngine context(intel, dispatcher);
    std::promise<ContextResult> received;
    auto future = received.get_future();
    context.setOnContext([&](ContextResult result) { received.set_value(std::move(result)); });

    context.onCursorMoved("shared", sameFile.string());

    REQUIRE(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    ContextResult result = future.get();

    REQUIRE(result.candidates.size() == 3);
    CHECK(result.candidates[0].file == sameFile.string());
    CHECK(result.candidates[1].file == sameDirFile.string());
    CHECK(result.candidates[2].file == otherDirFile.string());
}

TEST_CASE("ContextEngine: unresolvable identifier yields an empty (not error) result") {
    TempContextDir dir;
    fs::path fileA = dir.writeFile("a.c", "int foo(void) { return 1; }\n");

    TreeSitterEngine engine;
    xinsight::core::testing::ImmediateUiDispatcher dispatcher;
    CodeIntelligence intel(engine, std::make_shared<InMemorySymbolIndex>(), dispatcher);
    indexAndWait(intel, {fileA});

    ContextEngine context(intel, dispatcher);
    std::promise<ContextResult> received;
    auto future = received.get_future();
    context.setOnContext([&](ContextResult result) { received.set_value(std::move(result)); });

    context.onCursorMoved("totally_unknown_name", fileA.string());

    REQUIRE(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    ContextResult result = future.get();
    CHECK(result.queriedName == "totally_unknown_name");
    CHECK(result.candidates.empty());
}
