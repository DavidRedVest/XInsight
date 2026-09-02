#include "xinsight/core/search/SearchEngine.h"

#include <array>

#include <nlohmann/json.hpp>
#include <reproc++/reproc.hpp>

namespace xinsight::core::search {

namespace {
constexpr size_t kBatchSize = 200; // flush to the UI callback every N matches
}

std::vector<std::string> buildRipgrepArgs(const std::string &query, const std::filesystem::path &root,
                                           const SearchOptions &options) {
    std::vector<std::string> args = {"rg", "--json"};

    if (!options.caseSensitive) args.push_back("--ignore-case");
    if (options.wholeWord) args.push_back("--word-regexp");
    if (!options.useRegex) args.push_back("--fixed-strings");
    if (!options.respectGitignore) args.push_back("--no-ignore");

    for (const std::string &glob : options.globs) {
        args.push_back("--glob");
        args.push_back(glob);
    }

    args.push_back("--"); // everything after this is positional, even if it looks like a flag
    args.push_back(query);
    args.push_back(root.string());

    return args;
}

std::optional<SearchResult> parseRipgrepMatchLine(const std::string &jsonLine) {
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(jsonLine);
    } catch (const nlohmann::json::exception &) {
        return std::nullopt;
    }

    if (!parsed.is_object() || parsed.value("type", "") != "match") return std::nullopt;

    const auto &data = parsed["data"];

    // Binary/non-UTF-8 lines: ripgrep emits `lines.bytes` (base64) instead
    // of `lines.text`. We only handle text search over text files (PRD 3.1
    // implies source files); skip those rather than mis-decoding them.
    if (!data.contains("lines") || !data["lines"].contains("text")) return std::nullopt;
    if (!data.contains("path") || !data["path"].contains("text")) return std::nullopt;

    SearchResult result;
    result.path = data["path"]["text"].get<std::string>();
    result.line = data.value("line_number", 0u);

    std::string text = data["lines"]["text"].get<std::string>();
    if (!text.empty() && text.back() == '\n') text.pop_back();
    result.preview = std::move(text);

    if (data.contains("submatches")) {
        for (const auto &sm : data["submatches"]) {
            SubMatch subMatch;
            subMatch.startCol = sm.value("start", 0u);
            subMatch.endCol = sm.value("end", 0u);
            result.submatches.push_back(subMatch);
        }
    }

    return result;
}

SearchEngine::SearchEngine(IUiDispatcher &dispatcher) : dispatcher_(dispatcher) {}

SearchEngine::~SearchEngine() {
    cancel();
    joinSearchThread();
}

void SearchEngine::joinSearchThread() {
    if (searchThread_.joinable()) searchThread_.join();
}

void SearchEngine::setOnResults(ResultsCallback callback) {
    onResults_ = std::move(callback);
}

void SearchEngine::setOnComplete(CompleteCallback callback) {
    onComplete_ = std::move(callback);
}

void SearchEngine::cancel() {
    cancelRequested_.store(true);
}

void SearchEngine::search(std::string query, std::filesystem::path root, SearchOptions options) {
    cancel();
    joinSearchThread();
    cancelRequested_.store(false);

    std::vector<std::string> args = buildRipgrepArgs(query, root, options);
    ResultsCallback resultsCallback = onResults_;
    CompleteCallback completeCallback = onComplete_;
    IUiDispatcher *dispatcher = &dispatcher_;
    std::atomic<bool> *cancelFlag = &cancelRequested_;

    searchThread_ = std::thread([args, resultsCallback, completeCallback, dispatcher, cancelFlag] {
        reproc::process process;
        std::error_code ec = process.start(args);

        size_t totalMatches = 0;
        bool cancelled = false;

        if (!ec) {
            std::string buffer;
            std::vector<SearchResult> pending;
            std::array<uint8_t, 8192> chunk{};

            auto flush = [&]() {
                if (pending.empty() || !resultsCallback) return;
                dispatcher->post(
                    [resultsCallback, batch = std::move(pending)]() mutable { resultsCallback(std::move(batch)); });
                pending.clear();
            };

            while (true) {
                if (cancelFlag->load()) {
                    cancelled = true;
                    process.kill();
                    break;
                }

                auto [bytesRead, readEc] = process.read(reproc::stream::out, chunk.data(), chunk.size());
                if (readEc) break; // EOF (process exited) or a real error either way stop reading

                buffer.append(reinterpret_cast<const char *>(chunk.data()), bytesRead);

                size_t newlinePos;
                while ((newlinePos = buffer.find('\n')) != std::string::npos) {
                    std::string line = buffer.substr(0, newlinePos);
                    buffer.erase(0, newlinePos + 1);

                    if (auto result = parseRipgrepMatchLine(line)) {
                        ++totalMatches;
                        pending.push_back(std::move(*result));
                        if (pending.size() >= kBatchSize) flush();
                    }
                }
            }

            flush();
            process.wait(reproc::milliseconds(2000));
        }

        if (completeCallback) {
            dispatcher->post([completeCallback, totalMatches, cancelled]() { completeCallback(totalMatches, cancelled); });
        }
    });
}

} // namespace xinsight::core::search
