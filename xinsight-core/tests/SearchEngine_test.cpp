#include <algorithm>
#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <future>

#include "support/ImmediateUiDispatcher.h"
#include "xinsight/core/search/SearchEngine.h"

using namespace xinsight::core::search;
namespace fs = std::filesystem;

namespace {

class TempSearchDir {
public:
    TempSearchDir() {
        root_ = fs::temp_directory_path() /
                fs::path("xinsight-searchengine-test-" +
                         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root_);
    }
    ~TempSearchDir() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    void writeFile(const std::string &relative, std::string_view content) {
        fs::path p = root_ / relative;
        fs::create_directories(p.parent_path());
        std::ofstream out(p, std::ios::binary);
        out << content;
    }

    const fs::path &root() const { return root_; }

private:
    fs::path root_;
};

} // namespace

TEST_CASE("buildRipgrepArgs: default options map to the expected flags") {
    auto args = buildRipgrepArgs("needle", "/some/root", SearchOptions{});

    CHECK(std::find(args.begin(), args.end(), "--json") != args.end());
    CHECK(std::find(args.begin(), args.end(), "--ignore-case") != args.end()); // caseSensitive=false by default
    CHECK(std::find(args.begin(), args.end(), "--fixed-strings") != args.end()); // useRegex=false by default
    CHECK(std::find(args.begin(), args.end(), "--word-regexp") == args.end());
    CHECK(std::find(args.begin(), args.end(), "--no-ignore") == args.end());
    CHECK(args.back() == "/some/root");
    CHECK(args[args.size() - 2] == "needle");
}

TEST_CASE("buildRipgrepArgs: caseSensitive suppresses --ignore-case") {
    SearchOptions opts;
    opts.caseSensitive = true;
    auto args = buildRipgrepArgs("needle", "/root", opts);
    CHECK(std::find(args.begin(), args.end(), "--ignore-case") == args.end());
}

TEST_CASE("buildRipgrepArgs: wholeWord adds --word-regexp") {
    SearchOptions opts;
    opts.wholeWord = true;
    auto args = buildRipgrepArgs("needle", "/root", opts);
    CHECK(std::find(args.begin(), args.end(), "--word-regexp") != args.end());
}

TEST_CASE("buildRipgrepArgs: useRegex suppresses --fixed-strings") {
    SearchOptions opts;
    opts.useRegex = true;
    auto args = buildRipgrepArgs("a.*b", "/root", opts);
    CHECK(std::find(args.begin(), args.end(), "--fixed-strings") == args.end());
}

TEST_CASE("buildRipgrepArgs: respectGitignore=false adds --no-ignore") {
    SearchOptions opts;
    opts.respectGitignore = false;
    auto args = buildRipgrepArgs("needle", "/root", opts);
    CHECK(std::find(args.begin(), args.end(), "--no-ignore") != args.end());
}

TEST_CASE("buildRipgrepArgs: globs are passed as repeated --glob pairs") {
    SearchOptions opts;
    opts.globs = {"*.c", "*.h"};
    auto args = buildRipgrepArgs("needle", "/root", opts);

    int globCount = 0;
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "--glob") ++globCount;
    }
    CHECK(globCount == 2);
    CHECK(std::find(args.begin(), args.end(), "*.c") != args.end());
    CHECK(std::find(args.begin(), args.end(), "*.h") != args.end());
}

TEST_CASE("parseRipgrepMatchLine: parses a real match event") {
    // Captured verbatim from `rg --json world` against a real fixture.
    std::string line =
        R"({"type":"match","data":{"path":{"text":"/tmp/a.txt"},"lines":{"text":"hello world\n"},)"
        R"("line_number":1,"absolute_offset":0,"submatches":[{"match":{"text":"world"},"start":6,"end":11}]}})";

    auto result = parseRipgrepMatchLine(line);
    REQUIRE(result.has_value());
    CHECK(result->path == "/tmp/a.txt");
    CHECK(result->line == 1);
    CHECK(result->preview == "hello world"); // trailing \n stripped
    REQUIRE(result->submatches.size() == 1);
    CHECK(result->submatches[0].startCol == 6);
    CHECK(result->submatches[0].endCol == 11);
}

TEST_CASE("parseRipgrepMatchLine: non-match event types are ignored") {
    CHECK_FALSE(parseRipgrepMatchLine(R"({"type":"begin","data":{"path":{"text":"/tmp/a.txt"}}})").has_value());
    CHECK_FALSE(parseRipgrepMatchLine(R"({"type":"end","data":{}})").has_value());
    CHECK_FALSE(parseRipgrepMatchLine(R"({"type":"summary","data":{}})").has_value());
}

TEST_CASE("parseRipgrepMatchLine: malformed JSON doesn't throw, just yields nullopt") {
    CHECK_FALSE(parseRipgrepMatchLine("not json at all").has_value());
    CHECK_FALSE(parseRipgrepMatchLine("").has_value());
    CHECK_FALSE(parseRipgrepMatchLine(R"({"type":"match")").has_value()); // truncated
}

TEST_CASE("parseRipgrepMatchLine: binary matches (lines.bytes instead of lines.text) are skipped") {
    std::string line = R"({"type":"match","data":{"path":{"text":"/tmp/bin"},)"
                        R"("lines":{"bytes":"AAAA"},"line_number":1,"submatches":[]}})";
    CHECK_FALSE(parseRipgrepMatchLine(line).has_value());
}

TEST_CASE("SearchEngine: finds matches in a real directory and reports completion") {
    TempSearchDir dir;
    dir.writeFile("a.c", "int needle_value = 1;\nint other = 2;\n");
    dir.writeFile("b.c", "// no match here\n");
    dir.writeFile("sub/c.c", "int needle_again = 3;\n");

    xinsight::core::testing::ImmediateUiDispatcher dispatcher;
    SearchEngine engine(dispatcher);

    std::vector<SearchResult> allResults;
    std::promise<size_t> completePromise;
    std::future<size_t> completeFuture = completePromise.get_future();

    engine.setOnResults([&allResults](std::vector<SearchResult> batch) {
        allResults.insert(allResults.end(), std::make_move_iterator(batch.begin()),
                           std::make_move_iterator(batch.end()));
    });
    engine.setOnComplete([&completePromise](size_t total, bool cancelled) {
        CHECK_FALSE(cancelled);
        completePromise.set_value(total);
    });

    engine.search("needle", dir.root());

    auto status = completeFuture.wait_for(std::chrono::seconds(10));
    REQUIRE(status == std::future_status::ready);
    CHECK(completeFuture.get() == 2);
    REQUIRE(allResults.size() == 2);

    bool foundA = std::any_of(allResults.begin(), allResults.end(),
                               [](const SearchResult &r) { return r.preview.find("needle_value") != std::string::npos; });
    bool foundC = std::any_of(allResults.begin(), allResults.end(),
                               [](const SearchResult &r) { return r.preview.find("needle_again") != std::string::npos; });
    CHECK(foundA);
    CHECK(foundC);
}

TEST_CASE("SearchEngine: case-insensitive by default") {
    TempSearchDir dir;
    dir.writeFile("a.c", "int NEEDLE = 1;\n");

    xinsight::core::testing::ImmediateUiDispatcher dispatcher;
    SearchEngine engine(dispatcher);

    std::promise<size_t> completePromise;
    auto completeFuture = completePromise.get_future();
    engine.setOnComplete([&completePromise](size_t total, bool) { completePromise.set_value(total); });

    engine.search("needle", dir.root());

    auto status = completeFuture.wait_for(std::chrono::seconds(10));
    REQUIRE(status == std::future_status::ready);
    CHECK(completeFuture.get() == 1);
}

TEST_CASE("SearchEngine: respectGitignore=false finds matches inside a gitignored directory") {
    TempSearchDir dir;
    dir.writeFile(".gitignore", "ignored_dir/\n");
    dir.writeFile("ignored_dir/hidden.c", "int needle_in_ignored = 1;\n");

    xinsight::core::testing::ImmediateUiDispatcher dispatcher;
    SearchEngine engine(dispatcher);

    std::promise<size_t> completePromise;
    auto completeFuture = completePromise.get_future();
    engine.setOnComplete([&completePromise](size_t total, bool) { completePromise.set_value(total); });

    SearchOptions opts;
    opts.respectGitignore = false;
    engine.search("needle", dir.root(), opts);

    auto status = completeFuture.wait_for(std::chrono::seconds(10));
    REQUIRE(status == std::future_status::ready);
    CHECK(completeFuture.get() == 1);
}
