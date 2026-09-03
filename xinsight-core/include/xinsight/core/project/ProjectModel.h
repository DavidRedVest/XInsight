#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "xinsight/core/IUiDispatcher.h"

namespace xinsight::core::project {

// File-inclusion defaults per PRD 5.6. All fields are meant to be
// user-configurable; these are just the shipped v1 defaults, not hardcoded
// policy -- callers may pass a modified copy to ProjectModel::openRoot.
struct FileInclusionRules {
    // Lowercase, no leading '.'.
    std::vector<std::string> includedExtensions = {
        "c", "cc", "cpp", "cxx", "c++", "h", "hh", "hpp", "hxx", "h++", "inl",
    };

    std::vector<std::string> skipDirNames = {
        ".git", ".svn", ".hg", "build", "out", "node_modules", ".cache",
    };

    // Simple prefix match (covers PRD 5.6's `build-*` / `cmake-build-*`).
    std::vector<std::string> skipDirPrefixes = {
        "build-", "cmake-build-",
    };

    // PRD 5.6: don't follow symlinked directories by default (avoids
    // cycles/duplicate indexing); this is a setting, not a hard rule.
    bool followSymlinkDirs = false;

    // PRD 5.6: files over this size are still shown (read-only) in the file
    // tree but skipped for parsing/indexing. 5 MB default.
    uint64_t maxIndexableFileSizeBytes = 5ull * 1024 * 1024;
};

struct FileEntry {
    // Relative to the scanned root, forward-slash separated regardless of
    // platform (for stable cross-platform comparison/sorting/testing).
    std::filesystem::path relativePath;
    bool isDirectory = false;
    uint64_t sizeBytes = 0;
    // Only meaningful when !isDirectory: file is over maxIndexableFileSizeBytes.
    bool exceedsSizeLimit = false;
};

// Synchronous, side-effect-free directory scan per `rules`. Pure enough to
// unit-test directly; ProjectModel below is a thin background-thread +
// IUiDispatcher wrapper around this. Results are sorted by relativePath,
// which guarantees (lexicographic prefix ordering) that a directory entry
// always precedes its own descendants -- callers building a tree
// incrementally can rely on parents appearing before children.
std::vector<FileEntry> scanDirectory(const std::filesystem::path &root, const FileInclusionRules &rules = {});

// PRD 2.3 "新建文件/文件夹". Both fail (return false) if `path` already
// exists or its parent doesn't -- callers are expected to have already
// resolved a target path (e.g. via a save dialog), not to silently
// overwrite or auto-create intermediate directories.
bool createEmptyFile(const std::filesystem::path &path);
bool createDirectory(const std::filesystem::path &path);

// Locates compile_commands.json for `root` (PRD 5.4: ProjectModel owns
// "compile db 定位"; PRD 8.4: CMake typically drops it at the build
// directory's top level via -DCMAKE_EXPORT_COMPILE_COMMANDS=ON). Checks
// `root` itself, then a handful of conventional build-directory names
// within it. Returns the *directory containing* compile_commands.json
// (what clangd's --compile-commands-dir wants), or nullopt if none of
// those locations has one. Deliberately shallow (no recursive search):
// PRD 5.6 already excludes build directories from the file tree/index,
// so this is the one place that deliberately looks *into* them anyway,
// and a handful of conventional names covers the common cases without
// walking the whole tree hunting for it.
std::optional<std::filesystem::path> findCompileCommandsDir(const std::filesystem::path &root);

// Owns background scanning of a project root and marshals results back to
// the UI thread via IUiDispatcher. Holds no file tree state itself --
// callers own whatever structure they build from the delivered FileEntry
// list (PRD 5.4: ProjectModel is core-owned "文件树数据"; the GUI only
// renders it).
class ProjectModel {
public:
    explicit ProjectModel(IUiDispatcher &dispatcher);
    ~ProjectModel();
    ProjectModel(const ProjectModel &) = delete;
    ProjectModel &operator=(const ProjectModel &) = delete;

    using ScanCompleteCallback = std::function<void(std::vector<FileEntry>)>;

    // Replaces the callback used by subsequent openRoot() calls. Not safe
    // to call while a scan is in flight from a different thread than the
    // one driving openRoot()/the destructor.
    void setOnScanComplete(ScanCompleteCallback callback);

    // Cancels/joins any in-flight scan, then starts a new background scan
    // of `root`. On completion, the registered callback (a snapshot taken
    // at call time, not `this`) is invoked via the dispatcher.
    void openRoot(std::filesystem::path root, FileInclusionRules rules = {});

    const std::filesystem::path &rootPath() const { return root_; }

private:
    void joinScanThread();

    IUiDispatcher &dispatcher_;
    std::filesystem::path root_;
    std::thread scanThread_;
    ScanCompleteCallback onScanComplete_;
};

} // namespace xinsight::core::project
