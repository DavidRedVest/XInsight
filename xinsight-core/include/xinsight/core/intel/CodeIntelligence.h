#pragma once

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "xinsight/core/IUiDispatcher.h"
#include "xinsight/core/intel/SymbolIndex.h"
#include "xinsight/core/intel/TreeSitterEngine.h"

namespace xinsight::core::lsp {
class LspClient;
class ClangdProvider;
} // namespace xinsight::core::lsp

namespace xinsight::core::intel {

// One candidate returned by the *async* routing methods below -- shaped
// uniformly regardless of whether it came from tree-sitter or clangd.
// `kind` is absent for clangd-sourced candidates: clangd's plain
// `Location` responses (textDocument/definition/references) don't carry
// symbol-kind info, only workspace/symbol does, and even then GUI display
// is a presentation concern this struct just carries data for.
struct QueryLocation {
    std::string name;
    std::string file;
    uint32_t startRow = 0;
    uint32_t startColumn = 0;
    std::optional<SymbolKind> kind;
};

// `precise` is PRD 5.2/8.3's "每个结果带 precise 标记": true when this
// came from clangd (compile-db configured and the TU was ready at query
// time), false when it's tree-sitter's fuzzy, always-available answer.
struct DefinitionResult {
    std::vector<QueryLocation> candidates;
    bool precise = false;
};
struct ReferencesResult {
    std::vector<QueryLocation> candidates;
    bool precise = false;
};
struct WorkspaceSymbolResult {
    std::vector<QueryLocation> candidates;
    bool precise = false;
};

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

    // Synchronous, tree-sitter-only (never clangd): the API ContextEngine
    // uses (PRD 5.2 -- the context pane always answers immediately from
    // tree-sitter and never waits on clangd, "prefer precise" being a
    // future opt-in this v1 doesn't implement).
    std::vector<SymbolLocation> findDefinition(const std::string &name) const;
    std::vector<ReferenceLocation> findReferences(const std::string &name) const;
    std::vector<SymbolLocation> searchWorkspaceSymbols(const std::string &query, size_t maxResults = 200) const;

    // Configures the optional clangd overlay (PRD 4.2 Layer B). Pass
    // nullopt to disable (or to fall back after a project without a
    // compile db is opened) -- routing then stays tree-sitter-only, same
    // as M2/M3, no regression. `compileCommandsDir` is the directory
    // *containing* compile_commands.json. Starting clangd is asynchronous
    // and best-effort: if it fails to launch or the handshake fails,
    // routing silently keeps using tree-sitter (PRD 5.2's "降级铁律") --
    // `onStatusChanged`, if set, is notified either way.
    using ClangdStatusCallback = std::function<void(bool running)>;
    void setOnClangdStatusChanged(ClangdStatusCallback callback);
    void configureClangd(std::optional<std::filesystem::path> compileCommandsDir, std::filesystem::path projectRoot);
    bool isClangdConfigured() const;

    // Keeps the optional clangd overlay's view of a buffer in sync (PRD
    // 8.7). No-ops when clangd isn't configured.
    void notifyFileOpened(const std::string &absolutePath, const std::string &languageId, std::string utf8Text);
    void notifyFileChanged(const std::string &absolutePath, std::string utf8Text);
    void notifyFileSaved(const std::string &absolutePath);

    using DefinitionCallback = std::function<void(DefinitionResult)>;
    using ReferencesCallback = std::function<void(ReferencesResult)>;
    using WorkspaceSymbolCallback = std::function<void(WorkspaceSymbolResult)>;

    // Async routing per PRD 5.2: if clangd is configured and has this
    // file's TU ready, fires `callback` *asynchronously* (later, via the
    // dispatcher) with clangd's precise=true answer; otherwise fires it
    // *immediately* (synchronously, before returning) with tree-sitter's
    // precise=false answer -- callers must not assume either timing.
    // Never blocks on clangd, never regresses vs. the tree-sitter-only
    // path. `row`/`byteColumn` locate the cursor in `file` using this
    // codebase's usual convention; `lineTextUtf8` is that row's current
    // text (needed only for the clangd path's UTF-16 position).
    void findDefinitionAsync(std::string name, std::string file, uint32_t row, std::string lineTextUtf8,
                              uint32_t byteColumn, DefinitionCallback callback);
    void findReferencesAsync(std::string name, std::string file, uint32_t row, std::string lineTextUtf8,
                              uint32_t byteColumn, ReferencesCallback callback);
    void searchWorkspaceSymbolsAsync(std::string query, size_t maxResults, WorkspaceSymbolCallback callback);

private:
    void joinIndexThread();

    TreeSitterEngine &engine_;
    std::shared_ptr<ISymbolIndex> index_;
    IUiDispatcher &dispatcher_;
    std::thread indexThread_;
    std::atomic<bool> cancelRequested_{false};
    ProgressCallback onProgress_;
    CompleteCallback onComplete_;

    // Declaration order matters: clangdProvider_ holds a reference into
    // *lspClient_, so lspClient_ must outlive it. (Destruction safety
    // beyond mere ordering is handled explicitly in ~CodeIntelligence()
    // -- see CodeIntelligence.cpp.)
    std::unique_ptr<lsp::LspClient> lspClient_;
    std::unique_ptr<lsp::ClangdProvider> clangdProvider_;
    ClangdStatusCallback onClangdStatusChanged_;
};

} // namespace xinsight::core::intel
