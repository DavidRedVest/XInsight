#pragma once

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "xinsight/core/IUiDispatcher.h"
#include "xinsight/core/intel/SymbolIndex.h"
#include "xinsight/core/intel/TreeSitterEngine.h"

namespace xinsight::core::intel {

// The single entry point for definition/reference/workspace-symbol queries
// (PRD 5.1/5.4/5.5): GUI and other core modules must never call
// TreeSitterEngine's outline()/references() or an ISymbolIndex directly for
// these purposes, only through here. M2 wires only the tree-sitter+index
// path (results are never "not found" -- tree-sitter always has *an*
// answer); M4 adds an optional clangd branch behind this same API without
// changing callers (PRD 5.2's routing table).
class CodeIntelligence {
public:
    using ProgressCallback = std::function<void(size_t filesIndexed, size_t totalFiles)>;
    using CompleteCallback = std::function<void()>;

    // `engine` and `dispatcher` must outlive this CodeIntelligence.
    CodeIntelligence(TreeSitterEngine &engine, std::shared_ptr<ISymbolIndex> index, IUiDispatcher &dispatcher);
    ~CodeIntelligence();
    CodeIntelligence(const CodeIntelligence &) = delete;
    CodeIntelligence &operator=(const CodeIntelligence &) = delete;

    void setOnIndexProgress(ProgressCallback callback);
    void setOnIndexComplete(CompleteCallback callback);

    // Cancels any in-flight scan, then background-reindexes exactly
    // `files` (absolute paths). Files not in this list are left alone --
    // callers pass the project's full file list for an initial scan; a
    // narrower list just re-verifies those files without wiping anything
    // else. Files with an unrecognized extension or that fail to read are
    // silently skipped (never surfaced as an indexing error -- PRD 5.2's
    // "never report not-found" spirit extends to indexing robustness).
    void indexProject(std::vector<std::filesystem::path> files);

    // Re-derives symbols/references for `absolutePath` from `doc` (already
    // parsed by the caller -- EditorView, the one GUI class allowed to
    // touch TreeSitterEngine directly) and updates the index synchronously
    // on the calling thread. PRD 8.7: index updates are deferred to save,
    // not run on every keystroke -- callers are expected to call this only
    // after a successful save.
    void updateFileIndex(const std::string &absolutePath, const ParsedDocument &doc);

    // Drops `absolutePath`'s entries (e.g. file deleted externally).
    void removeFileFromIndex(const std::string &absolutePath);

    // `precise` is always false in M2 (no clangd yet); kept on the result
    // types so M4 can start setting it without another interface change.
    std::vector<SymbolLocation> findDefinition(const std::string &name) const;
    std::vector<ReferenceLocation> findReferences(const std::string &name) const;
    std::vector<SymbolLocation> searchWorkspaceSymbols(const std::string &query, size_t maxResults = 200) const;

private:
    void joinIndexThread();

    TreeSitterEngine &engine_;
    std::shared_ptr<ISymbolIndex> index_;
    IUiDispatcher &dispatcher_;
    std::thread indexThread_;
    std::atomic<bool> cancelRequested_{false};
    ProgressCallback onProgress_;
    CompleteCallback onComplete_;
};

} // namespace xinsight::core::intel
