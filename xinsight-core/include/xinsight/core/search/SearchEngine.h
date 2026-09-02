#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "xinsight/core/IUiDispatcher.h"

namespace xinsight::core::search {

struct SearchOptions {
    bool caseSensitive = false;
    bool wholeWord = false;
    bool useRegex = false;
    // PRD 3.1: follows .gitignore by default, but must be toggleable off
    // (embedded projects often need to search into ignored directories).
    bool respectGitignore = true;
    // e.g. {"*.c", "*.h"}; passed through to ripgrep's --glob, one per entry.
    std::vector<std::string> globs;
};

struct SubMatch {
    uint32_t startCol = 0; // byte offset within `SearchResult::preview`
    uint32_t endCol = 0;
};

struct SearchResult {
    std::string path; // as reported by ripgrep (absolute, since we pass an absolute root)
    uint32_t line = 0; // 1-based, matches ripgrep's line_number
    std::string preview; // full line text, trailing newline stripped
    std::vector<SubMatch> submatches;
};

// The single entry point for text search (PRD 3.1/5.4): shells out to
// `rg --json`, parsing its newline-delimited JSON event stream
// incrementally (not buffering the whole run) so results can stream to the
// UI and a search can be cancelled mid-flight.
class SearchEngine {
public:
    using ResultsCallback = std::function<void(std::vector<SearchResult>)>; // batched, may fire many times
    using CompleteCallback = std::function<void(size_t totalMatches, bool wasCancelled)>;

    explicit SearchEngine(IUiDispatcher &dispatcher);
    ~SearchEngine();
    SearchEngine(const SearchEngine &) = delete;
    SearchEngine &operator=(const SearchEngine &) = delete;

    void setOnResults(ResultsCallback callback);
    void setOnComplete(CompleteCallback callback);

    // Cancels any in-flight search, then starts a new one.
    void search(std::string query, std::filesystem::path root, SearchOptions options = {});

    // Requests cancellation of an in-flight search; safe to call when idle.
    void cancel();

private:
    void joinSearchThread();

    IUiDispatcher &dispatcher_;
    std::thread searchThread_;
    std::atomic<bool> cancelRequested_{false};
    ResultsCallback onResults_;
    CompleteCallback onComplete_;
};

// Builds the `rg` argv (excluding the binary name) for the given query,
// root, and options. Exposed for unit testing without spawning a process.
std::vector<std::string> buildRipgrepArgs(const std::string &query, const std::filesystem::path &root,
                                           const SearchOptions &options);

// Parses one line of `rg --json` output. Returns nullopt for non-"match"
// event types (begin/end/summary/context) or malformed lines -- callers
// should just skip those, not treat them as errors.
std::optional<SearchResult> parseRipgrepMatchLine(const std::string &jsonLine);

} // namespace xinsight::core::search
