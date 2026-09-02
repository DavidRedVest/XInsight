#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "xinsight/core/IUiDispatcher.h"
#include "xinsight/core/intel/CodeIntelligence.h"

namespace xinsight::core::context {

// One candidate definition to render in the ambient context pane.
struct ContextCandidate {
    std::string file;
    std::string signature;  // the definition line's own source text
    std::string snippet;    // multi-line source text around the definition
    uint32_t snippetStartRow = 0; // 0-based row in `file` where snippet begins
    uint32_t highlightRow = 0;    // 0-based row in `file` of the definition itself
    xinsight::core::intel::SymbolKind kind;
};

struct ContextResult {
    std::string queriedName; // empty when the cursor isn't on a resolvable identifier
    // Ranked same-file > same-directory > rest-of-project (PRD 2.1);
    // candidates[0] is the default selection. Empty when nothing resolves
    // (unknown/local identifier, or queriedName itself is empty) -- the
    // GUI just shows nothing, never an error.
    std::vector<ContextCandidate> candidates;
};

// PRD 2.1's ambient context pane, core half: "光标→防抖(150-250ms)→查索引→
// 发出源码区间". Debounces cursor-move notifications on its own background
// worker thread (per CLAUDE.md: core has its own I/O threads, never
// borrows the GUI event loop) and cancels any request superseded by a
// newer one before its debounce window elapses. Always answers from
// CodeIntelligence's always-on tree-sitter layer -- never blocks on
// clangd, never reports "not found" as an error (just fewer/no
// candidates).
class ContextEngine {
public:
    using ContextCallback = std::function<void(ContextResult)>;

    // `codeIntelligence` and `dispatcher` must outlive this ContextEngine.
    ContextEngine(xinsight::core::intel::CodeIntelligence &codeIntelligence, IUiDispatcher &dispatcher);
    ~ContextEngine();
    ContextEngine(const ContextEngine &) = delete;
    ContextEngine &operator=(const ContextEngine &) = delete;

    void setOnContext(ContextCallback callback);

    // Called on every cursor move. `identifierName` is the lookup name
    // already resolved by TreeSitterEngine::identifierAtByteOffset (empty
    // if the cursor isn't on a resolvable identifier -- clears any
    // pending request and immediately fires an empty ContextResult so the
    // pane clears without waiting out a debounce window it doesn't need).
    // `currentFile` (the file the cursor is in) ranks candidates per PRD
    // 2.1 (same-file > same-directory > rest); its directory is derived
    // internally.
    void onCursorMoved(std::string identifierName, std::string currentFile);

private:
    struct Request {
        std::string name;
        std::string file;
    };

    void workerLoop();
    ContextResult resolve(const Request &request) const;

    xinsight::core::intel::CodeIntelligence &codeIntelligence_;
    IUiDispatcher &dispatcher_;
    ContextCallback onContext_;

    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopRequested_ = false;
    uint64_t generation_ = 0; // bumped on every onCursorMoved (requests and cancellations alike)
    std::optional<Request> activeRequest_;
};

} // namespace xinsight::core::context
