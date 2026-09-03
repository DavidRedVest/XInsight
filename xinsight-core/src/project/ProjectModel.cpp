#include "xinsight/core/project/ProjectModel.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <system_error>

namespace xinsight::core::project {

namespace {

std::string lowercase(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool isSkippedDirName(const std::string &name, const FileInclusionRules &rules) {
    for (const auto &skip : rules.skipDirNames) {
        if (name == skip) return true;
    }
    for (const auto &prefix : rules.skipDirPrefixes) {
        if (name.size() >= prefix.size() && name.compare(0, prefix.size(), prefix) == 0) return true;
    }
    return false;
}

bool hasIncludedExtension(const std::filesystem::path &path, const FileInclusionRules &rules) {
    std::string ext = path.extension().string();
    if (ext.empty()) return false;
    if (ext.front() == '.') ext.erase(0, 1);
    ext = lowercase(ext);
    return std::find(rules.includedExtensions.begin(), rules.includedExtensions.end(), ext) !=
           rules.includedExtensions.end();
}

// Defensive fallback for PRD 5.6's "skip obvious binaries by content" --
// most of the filtering already happens via the extension allowlist, this
// just guards against a mis-tagged file (e.g. a generated blob named .h).
bool looksBinary(const std::filesystem::path &path) {
    std::FILE *f = std::fopen(path.string().c_str(), "rb");
    if (f == nullptr) return false; // can't read it; let the caller's later open() report the real error
    std::array<char, 8192> buffer{};
    size_t n = std::fread(buffer.data(), 1, buffer.size(), f);
    std::fclose(f);
    return std::find(buffer.data(), buffer.data() + n, '\0') != buffer.data() + n;
}

void scanRecursive(const std::filesystem::path &root, const std::filesystem::path &relativeDir,
                    const FileInclusionRules &rules, std::vector<FileEntry> &out) {
    std::error_code ec;
    std::filesystem::path absoluteDir = root / relativeDir;

    for (const auto &entry : std::filesystem::directory_iterator(
             absoluteDir, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (ec) break;

        const std::filesystem::path relativePath = relativeDir.empty() ? entry.path().filename()
                                                                         : relativeDir / entry.path().filename();

        bool isSymlink = entry.is_symlink(ec);
        bool isDir = entry.is_directory(ec);

        if (isSymlink && isDir && !rules.followSymlinkDirs) {
            continue; // PRD 5.6 default: don't follow symlinked directories
        }

        if (isDir) {
            std::string name = entry.path().filename().string();
            if (isSkippedDirName(name, rules)) continue;

            out.push_back(FileEntry{relativePath, /*isDirectory=*/true, 0, false});
            scanRecursive(root, relativePath, rules, out);
            continue;
        }

        bool isRegularFile = entry.is_regular_file(ec);
        if (!isRegularFile || ec) continue;

        if (!hasIncludedExtension(entry.path(), rules)) continue;
        if (looksBinary(entry.path())) continue;

        uint64_t size = entry.file_size(ec);
        if (ec) size = 0;

        FileEntry fileEntry;
        fileEntry.relativePath = relativePath;
        fileEntry.isDirectory = false;
        fileEntry.sizeBytes = size;
        fileEntry.exceedsSizeLimit = size > rules.maxIndexableFileSizeBytes;
        out.push_back(std::move(fileEntry));
    }
}

} // namespace

std::vector<FileEntry> scanDirectory(const std::filesystem::path &root, const FileInclusionRules &rules) {
    std::vector<FileEntry> result;

    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec) || ec) return result;

    scanRecursive(root, std::filesystem::path{}, rules, result);

    std::sort(result.begin(), result.end(),
              [](const FileEntry &a, const FileEntry &b) { return a.relativePath < b.relativePath; });

    return result;
}

bool createEmptyFile(const std::filesystem::path &path) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) return false;
    if (!std::filesystem::is_directory(path.parent_path(), ec) || ec) return false;

    std::ofstream out(path, std::ios::binary);
    return out.good();
}

bool createDirectory(const std::filesystem::path &path) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) return false;
    if (!std::filesystem::is_directory(path.parent_path(), ec) || ec) return false;

    return std::filesystem::create_directory(path, ec) && !ec;
}

std::optional<std::filesystem::path> findCompileCommandsDir(const std::filesystem::path &root) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(root / "compile_commands.json", ec)) return root;

    static constexpr std::array<const char *, 4> kConventionalBuildDirs = {
        "build",
        "out",
        "cmake-build-debug",
        "cmake-build-release",
    };
    for (const char *dirName : kConventionalBuildDirs) {
        std::filesystem::path candidate = root / dirName;
        if (std::filesystem::is_regular_file(candidate / "compile_commands.json", ec)) return candidate;
    }

    return std::nullopt;
}

ProjectModel::ProjectModel(IUiDispatcher &dispatcher) : dispatcher_(dispatcher) {}

ProjectModel::~ProjectModel() {
    joinScanThread();
}

void ProjectModel::joinScanThread() {
    if (scanThread_.joinable()) scanThread_.join();
}

void ProjectModel::setOnScanComplete(ScanCompleteCallback callback) {
    onScanComplete_ = std::move(callback);
}

void ProjectModel::openRoot(std::filesystem::path root, FileInclusionRules rules) {
    joinScanThread();
    root_ = std::move(root);

    // Deliberately not capturing `this`: the posted UI-thread callback may
    // run after this ProjectModel is destroyed (IUiDispatcher::post queues
    // onto the UI event loop, which can outlive us), so the background
    // thread and the posted lambda only carry copies of what they need.
    std::filesystem::path rootCopy = root_;
    FileInclusionRules rulesCopy = rules;
    ScanCompleteCallback callback = onScanComplete_;
    IUiDispatcher *dispatcher = &dispatcher_;

    scanThread_ = std::thread([rootCopy, rulesCopy, callback, dispatcher] {
        std::vector<FileEntry> entries = scanDirectory(rootCopy, rulesCopy);
        if (!callback) return;
        dispatcher->post([callback, entries = std::move(entries)]() mutable { callback(std::move(entries)); });
    });
}

} // namespace xinsight::core::project
