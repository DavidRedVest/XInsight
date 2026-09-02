#include "xinsight/core/context/ContextEngine.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "xinsight/core/encoding/TextCodec.h"

namespace xinsight::core::context {

namespace {

using xinsight::core::intel::SymbolLocation;

constexpr auto kDebounceWindow = std::chrono::milliseconds(200); // PRD 2.1: 150-250ms
constexpr int kContextLinesBefore = 2;
constexpr int kFallbackLinesAfter = 8; // for symbols with no brace body to bound the snippet
constexpr uint32_t kMaxBraceScanLines = 2000; // defensive bound against a mismatched-brace scan runaway

// PRD 2.1: "窗格显示...函数体或定义行 ± 若干行" -- kinds whose definition has
// a `{ ... }` body worth showing in full, rather than a fixed-size window
// that could truncate mid-function.
bool hasBracedBody(xinsight::core::intel::SymbolKind kind) {
    using xinsight::core::intel::SymbolKind;
    switch (kind) {
    case SymbolKind::Function:
    case SymbolKind::Method:
    case SymbolKind::Struct:
    case SymbolKind::Union:
    case SymbolKind::Enum:
    case SymbolKind::Class:
    case SymbolKind::Namespace:
        return true;
    case SymbolKind::Typedef:
    case SymbolKind::Macro:
    case SymbolKind::GlobalVariable:
        return false;
    }
    return false;
}

// Finds the line (0-based) of the closing brace matching the first '{'
// found at or after `searchFromLine` -- a lightweight text-based counter,
// not a real parse (doesn't special-case braces inside string/char
// literals or comments). A rare-miss approximation is an accepted
// trade-off here per PRD 2.1's own "近似实现" framing for this pane;
// missing a match just falls back to the fixed-window snippet below.
std::optional<uint32_t> findMatchingBraceLine(const std::vector<std::string_view> &lines, uint32_t searchFromLine) {
    uint32_t depth = 0;
    bool opened = false;
    uint32_t scanLimit = std::min<uint32_t>(searchFromLine + kMaxBraceScanLines, static_cast<uint32_t>(lines.size()));
    for (uint32_t row = searchFromLine; row < scanLimit; ++row) {
        for (char c : lines[row]) {
            if (c == '{') {
                ++depth;
                opened = true;
            } else if (c == '}') {
                if (depth > 0) --depth;
                if (opened && depth == 0) return row;
            }
        }
    }
    return std::nullopt;
}

std::optional<std::vector<uint8_t>> readFileBytes(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::vector<std::string_view> splitLines(std::string_view text) {
    std::vector<std::string_view> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t newline = text.find('\n', start);
        if (newline == std::string_view::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, newline - start));
        start = newline + 1;
    }
    return lines;
}

// same-file (0) > same-directory (1) > rest-of-project (2), PRD 2.1.
int rankOf(const SymbolLocation &candidate, const std::string &currentFile, const std::string &currentDir) {
    if (candidate.file == currentFile) return 0;
    if (std::filesystem::path(candidate.file).parent_path() == currentDir) return 1;
    return 2;
}

} // namespace

ContextEngine::ContextEngine(xinsight::core::intel::CodeIntelligence &codeIntelligence, IUiDispatcher &dispatcher)
    : codeIntelligence_(codeIntelligence), dispatcher_(dispatcher) {
    worker_ = std::thread([this] { workerLoop(); });
}

ContextEngine::~ContextEngine() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopRequested_ = true;
    }
    cv_.notify_one();
    if (worker_.joinable()) worker_.join();
}

void ContextEngine::setOnContext(ContextCallback callback) { onContext_ = std::move(callback); }

void ContextEngine::onCursorMoved(std::string identifierName, std::string currentFile) {
    std::unique_lock<std::mutex> lock(mutex_);
    ++generation_;

    if (identifierName.empty()) {
        activeRequest_.reset();
        lock.unlock();
        cv_.notify_one();
        if (onContext_) dispatcher_.post([cb = onContext_]() { cb(ContextResult{}); });
        return;
    }

    activeRequest_ = Request{std::move(identifierName), std::move(currentFile)};
    lock.unlock();
    cv_.notify_one();
}

void ContextEngine::workerLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        cv_.wait(lock, [&] { return stopRequested_ || activeRequest_.has_value(); });
        if (stopRequested_) return;

        uint64_t myGeneration = generation_;
        // Returns true iff woken because the predicate became true (a
        // newer request or a cancellation bumped generation_, or we're
        // stopping) before the debounce window elapsed; false on a clean
        // timeout, meaning nothing superseded this request in time.
        bool supersededOrStopped =
            cv_.wait_for(lock, kDebounceWindow, [&] { return stopRequested_ || generation_ != myGeneration; });
        if (stopRequested_) return;
        if (supersededOrStopped) continue; // loop back; wait() will pick up whatever's current now

        Request request = *activeRequest_;
        lock.unlock();

        ContextResult result = resolve(request);
        if (onContext_) {
            dispatcher_.post([cb = onContext_, result = std::move(result)]() mutable { cb(std::move(result)); });
        }

        lock.lock();
        // Only clear if still the same request -- a newer one may have
        // already replaced it while resolve() was running.
        if (generation_ == myGeneration) activeRequest_.reset();
    }
}

ContextResult ContextEngine::resolve(const Request &request) const {
    ContextResult result;
    result.queriedName = request.name;

    std::vector<SymbolLocation> defs = codeIntelligence_.findDefinition(request.name);
    if (defs.empty()) return result;

    std::string currentDir = std::filesystem::path(request.file).parent_path().string();
    std::stable_sort(defs.begin(), defs.end(), [&](const SymbolLocation &a, const SymbolLocation &b) {
        return rankOf(a, request.file, currentDir) < rankOf(b, request.file, currentDir);
    });

    for (const SymbolLocation &def : defs) {
        auto raw = readFileBytes(def.file);
        if (!raw) continue;
        auto decoded = xinsight::core::encoding::decode(*raw);
        std::vector<std::string_view> lines = splitLines(decoded.utf8Text);
        if (def.startRow >= lines.size()) continue;

        uint32_t lastLine = static_cast<uint32_t>(lines.size()) - 1;
        uint32_t startLine = def.startRow > kContextLinesBefore ? def.startRow - kContextLinesBefore : 0;

        uint32_t endLine;
        if (hasBracedBody(def.kind)) {
            auto closingLine = findMatchingBraceLine(lines, def.startRow);
            endLine = closingLine ? std::min(*closingLine, lastLine)
                                   : std::min<uint32_t>(def.startRow + kFallbackLinesAfter, lastLine);
        } else {
            endLine = std::min<uint32_t>(def.startRow + kFallbackLinesAfter, lastLine);
        }

        std::string snippet;
        for (uint32_t i = startLine; i <= endLine; ++i) {
            snippet.append(lines[i]);
            snippet.push_back('\n');
        }

        ContextCandidate candidate;
        candidate.file = def.file;
        candidate.signature = std::string(lines[def.startRow]);
        candidate.snippet = std::move(snippet);
        candidate.snippetStartRow = startLine;
        candidate.highlightRow = def.startRow;
        candidate.kind = def.kind;
        result.candidates.push_back(std::move(candidate));
    }

    return result;
}

} // namespace xinsight::core::context
