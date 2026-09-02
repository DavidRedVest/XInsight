#include <algorithm>
#include <chrono>
#include <cstdio>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <future>

#include "support/ImmediateUiDispatcher.h"
#include "xinsight/core/project/ProjectModel.h"

using namespace xinsight::core::project;
namespace fs = std::filesystem;

namespace {

// RAII temp directory populated with a fixture tree matching PRD 5.6's
// inclusion-rule test surface: mixed extensions, a skip-listed dir, a
// build-* prefix dir, a nested source dir, and an oversized file.
class TempProjectDir {
public:
    TempProjectDir() {
        root_ = fs::temp_directory_path() /
                fs::path("xinsight-projectmodel-test-" + std::to_string(std::chrono::steady_clock::now()
                                                                              .time_since_epoch()
                                                                              .count()));
        fs::create_directories(root_);
    }

    ~TempProjectDir() {
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

bool contains(const std::vector<FileEntry> &entries, const std::string &relative) {
    return std::any_of(entries.begin(), entries.end(),
                        [&](const FileEntry &e) { return e.relativePath.generic_string() == relative; });
}

} // namespace

TEST_CASE("scanDirectory includes only allow-listed C/C++ extensions") {
    TempProjectDir dir;
    dir.writeFile("main.c", "int main(void) { return 0; }\n");
    dir.writeFile("widget.hpp", "#pragma once\n");
    dir.writeFile("README.md", "not code\n");
    dir.writeFile("image.png", "\x89PNG fake\n");

    auto entries = scanDirectory(dir.root());

    CHECK(contains(entries, "main.c"));
    CHECK(contains(entries, "widget.hpp"));
    CHECK_FALSE(contains(entries, "README.md"));
    CHECK_FALSE(contains(entries, "image.png"));
}

TEST_CASE("scanDirectory skips default-excluded directories but still recurses into normal ones") {
    TempProjectDir dir;
    dir.writeFile(".git/HEAD", "ref: refs/heads/main\n");
    dir.writeFile("build/generated.c", "int g(void) { return 0; }\n");
    dir.writeFile("build-debug/generated.c", "int g(void) { return 0; }\n");
    dir.writeFile("node_modules/pkg/index.c", "int p(void) { return 0; }\n");
    dir.writeFile("src/lib/core.c", "int core(void) { return 0; }\n");

    auto entries = scanDirectory(dir.root());

    CHECK_FALSE(contains(entries, ".git/HEAD"));
    CHECK_FALSE(contains(entries, "build/generated.c"));
    CHECK_FALSE(contains(entries, "build-debug/generated.c"));
    CHECK_FALSE(contains(entries, "node_modules/pkg/index.c"));
    CHECK(contains(entries, "src/lib/core.c"));
    CHECK(contains(entries, "src"));
    CHECK(contains(entries, "src/lib"));
}

TEST_CASE("scanDirectory marks files over the size threshold but still includes them") {
    TempProjectDir dir;
    FileInclusionRules rules;
    rules.maxIndexableFileSizeBytes = 16; // tiny, to trigger deterministically without a huge fixture

    dir.writeFile("small.c", "int a;\n");            // <= 16 bytes
    dir.writeFile("big.c", "int a_very_long_name;\n"); // > 16 bytes

    auto entries = scanDirectory(dir.root(), rules);

    auto small = std::find_if(entries.begin(), entries.end(),
                               [](const FileEntry &e) { return e.relativePath.generic_string() == "small.c"; });
    auto big = std::find_if(entries.begin(), entries.end(),
                             [](const FileEntry &e) { return e.relativePath.generic_string() == "big.c"; });

    REQUIRE(small != entries.end());
    REQUIRE(big != entries.end());
    CHECK_FALSE(small->exceedsSizeLimit);
    CHECK(big->exceedsSizeLimit);
}

TEST_CASE("scanDirectory rejects an extension-matched file that is actually binary") {
    TempProjectDir dir;
    fs::path p = dir.root() / "blob.h";
    std::ofstream out(p, std::ios::binary);
    char bytes[] = {'#', 'i', 'f', '\0', 'x', 'y', 'z'};
    out.write(bytes, sizeof(bytes));
    out.close();

    auto entries = scanDirectory(dir.root());
    CHECK_FALSE(contains(entries, "blob.h"));
}

TEST_CASE("scanDirectory extension matching is case-insensitive") {
    TempProjectDir dir;
    dir.writeFile("Legacy.C", "int f(void) { return 0; }\n");
    dir.writeFile("Widget.HPP", "#pragma once\n");

    auto entries = scanDirectory(dir.root());
    CHECK(contains(entries, "Legacy.C"));
    CHECK(contains(entries, "Widget.HPP"));
}

TEST_CASE("scanDirectory does not follow symlinked directories by default") {
    TempProjectDir dir;
    dir.writeFile("real/inside.c", "int inside(void) { return 0; }\n");

    std::error_code ec;
    fs::create_directory_symlink(dir.root() / "real", dir.root() / "link", ec);
    if (ec) return; // symlink creation unsupported/denied in this environment; skip rather than fail spuriously

    auto entries = scanDirectory(dir.root());
    CHECK(contains(entries, "real/inside.c"));
    CHECK_FALSE(contains(entries, "link/inside.c"));
}

TEST_CASE("scanDirectory results are sorted with parent directories before their children") {
    TempProjectDir dir;
    dir.writeFile("a/b/c.c", "int c(void) { return 0; }\n");
    dir.writeFile("a/z.c", "int z(void) { return 0; }\n");

    auto entries = scanDirectory(dir.root());

    auto indexOf = [&](const std::string &relative) {
        return std::distance(entries.begin(), std::find_if(entries.begin(), entries.end(), [&](const FileEntry &e) {
                                  return e.relativePath.generic_string() == relative;
                              }));
    };

    CHECK(indexOf("a") < indexOf("a/b"));
    CHECK(indexOf("a/b") < indexOf("a/b/c.c"));
    CHECK(indexOf("a") < indexOf("a/z.c"));
}

TEST_CASE("ProjectModel::openRoot scans in the background and delivers results via IUiDispatcher") {
    TempProjectDir dir;
    dir.writeFile("main.c", "int main(void) { return 0; }\n");

    xinsight::core::testing::ImmediateUiDispatcher dispatcher;
    ProjectModel model(dispatcher);

    std::promise<std::vector<FileEntry>> resultPromise;
    std::future<std::vector<FileEntry>> resultFuture = resultPromise.get_future();
    model.setOnScanComplete(
        [&resultPromise](std::vector<FileEntry> entries) { resultPromise.set_value(std::move(entries)); });

    model.openRoot(dir.root());

    auto status = resultFuture.wait_for(std::chrono::seconds(5));
    REQUIRE(status == std::future_status::ready);

    auto entries = resultFuture.get();
    CHECK(contains(entries, "main.c"));
    CHECK(model.rootPath() == dir.root());
}

TEST_CASE("createEmptyFile creates an empty, readable file") {
    TempProjectDir dir;
    fs::path target = dir.root() / "new_file.c";

    CHECK(createEmptyFile(target));
    CHECK(fs::exists(target));
    CHECK(fs::file_size(target) == 0);
}

TEST_CASE("createEmptyFile refuses to overwrite an existing file") {
    TempProjectDir dir;
    dir.writeFile("existing.c", "int x;\n");

    CHECK_FALSE(createEmptyFile(dir.root() / "existing.c"));
    // Original content must survive the refused call.
    std::ifstream in(dir.root() / "existing.c");
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(content == "int x;\n");
}

TEST_CASE("createEmptyFile fails if the parent directory doesn't exist") {
    TempProjectDir dir;
    CHECK_FALSE(createEmptyFile(dir.root() / "no_such_dir" / "file.c"));
}

TEST_CASE("createDirectory creates a new directory") {
    TempProjectDir dir;
    fs::path target = dir.root() / "new_dir";

    CHECK(createDirectory(target));
    CHECK(fs::is_directory(target));
}

TEST_CASE("createDirectory refuses to clobber an existing path") {
    TempProjectDir dir;
    dir.writeFile("existing/marker.c", "int x;\n");

    CHECK_FALSE(createDirectory(dir.root() / "existing"));
    CHECK(fs::exists(dir.root() / "existing" / "marker.c")); // untouched
}
