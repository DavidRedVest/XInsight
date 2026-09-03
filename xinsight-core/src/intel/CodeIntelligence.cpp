#include "xinsight/core/intel/CodeIntelligence.h"

#include <fstream>
#include <iterator>
#include <optional>

#include "xinsight/core/encoding/TextCodec.h"
#include "xinsight/core/lsp/ClangdProvider.h"
#include "xinsight/core/lsp/LspClient.h"

namespace xinsight::core::intel {

namespace {

constexpr size_t kIndexBatchSize = 20; // flush accumulated per-file results to the UI thread every N files

std::optional<std::vector<uint8_t>> readFileBytes(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

struct PendingFileIndex {
    std::string file;
    std::vector<Symbol> symbols;
    std::vector<IdentifierOccurrence> references;
};

} // namespace

CodeIntelligence::CodeIntelligence(TreeSitterEngine &engine, std::shared_ptr<ISymbolIndex> index,
                                    IUiDispatcher &dispatcher)
    : engine_(engine), index_(std::move(index)), dispatcher_(dispatcher) {}

CodeIntelligence::~CodeIntelligence() {
    // Explicit, not left to implicit member-destruction order: stop()
    // halts lspClient_'s read thread synchronously, guaranteeing no more
    // notification callbacks can fire (those capture clangdProvider_'s
    // `this`) before clangdProvider_ itself gets destroyed. Declaration
    // order alone would destroy clangdProvider_ *before* lspClient_ is
    // stopped, which is backwards -- the same class of bug fixed for
    // MainWindow/EditorView/DocumentRegistry earlier in this project.
    if (lspClient_) lspClient_->stop();

    cancelRequested_.store(true);
    joinIndexThread();
}

void CodeIntelligence::joinIndexThread() {
    if (indexThread_.joinable()) indexThread_.join();
}

void CodeIntelligence::setOnIndexProgress(ProgressCallback callback) { onProgress_ = std::move(callback); }

void CodeIntelligence::setOnIndexComplete(CompleteCallback callback) { onComplete_ = std::move(callback); }

void CodeIntelligence::indexProject(std::vector<std::filesystem::path> files) {
    cancelRequested_.store(true);
    joinIndexThread();
    cancelRequested_.store(false);

    TreeSitterEngine *engine = &engine_;
    IUiDispatcher *dispatcher = &dispatcher_;
    // Captured by value (not `this`) into both the thread and its posted
    // lambdas: CodeIntelligence's destructor cancels+joins this thread
    // before `index_` is destroyed, but a lambda already queued on the UI
    // thread when that happens must still be safe to run -- a shared_ptr
    // copy keeps the index object alive until every such lambda has run,
    // even past this CodeIntelligence's own lifetime (see ProjectModel's
    // identical "deliberately not capturing `this`" precedent).
    std::shared_ptr<ISymbolIndex> index = index_;
    std::atomic<bool> *cancelFlag = &cancelRequested_;
    ProgressCallback onProgress = onProgress_;
    CompleteCallback onComplete = onComplete_;

    indexThread_ = std::thread([files = std::move(files), engine, dispatcher, index, cancelFlag, onProgress,
                                 onComplete] {
        size_t total = files.size();
        size_t done = 0;
        std::vector<PendingFileIndex> pending;

        auto flush = [&]() {
            if (pending.empty()) return;
            dispatcher->post([index, batch = std::move(pending), done, total, onProgress]() mutable {
                for (auto &item : batch) {
                    index->updateFile(item.file, std::move(item.symbols), std::move(item.references));
                }
                if (onProgress) onProgress(done, total);
            });
            pending.clear();
        };

        for (const auto &path : files) {
            if (cancelFlag->load()) break;
            ++done;

            auto language = languageForExtension(path.extension().string());
            if (!language) continue;

            auto raw = readFileBytes(path);
            if (!raw) continue;

            auto decoded = xinsight::core::encoding::decode(*raw);
            ParsedDocument doc = engine->parse(*language, decoded.utf8Text);
            if (!doc.valid()) continue;

            PendingFileIndex item;
            item.file = path.string();
            item.symbols = engine->outline(doc);
            item.references = engine->references(doc);
            pending.push_back(std::move(item));

            if (pending.size() >= kIndexBatchSize) flush();
        }
        flush();

        if (!cancelFlag->load() && onComplete) {
            dispatcher->post([onComplete]() { onComplete(); });
        }
    });
}

void CodeIntelligence::updateFileIndex(const std::string &absolutePath, const ParsedDocument &doc) {
    if (!doc.valid()) return;
    index_->updateFile(absolutePath, engine_.outline(doc), engine_.references(doc));
}

void CodeIntelligence::removeFileFromIndex(const std::string &absolutePath) { index_->removeFile(absolutePath); }

std::vector<SymbolLocation> CodeIntelligence::findDefinition(const std::string &name) const {
    return index_->findDefinitions(name);
}

std::vector<ReferenceLocation> CodeIntelligence::findReferences(const std::string &name) const {
    return index_->findReferences(name);
}

std::vector<SymbolLocation> CodeIntelligence::searchWorkspaceSymbols(const std::string &query,
                                                                      size_t maxResults) const {
    return index_->searchSymbols(query, maxResults);
}

void CodeIntelligence::setOnClangdStatusChanged(ClangdStatusCallback callback) {
    onClangdStatusChanged_ = std::move(callback);
}

bool CodeIntelligence::isClangdConfigured() const { return clangdProvider_ != nullptr; }

void CodeIntelligence::configureClangd(std::optional<std::filesystem::path> compileCommandsDir,
                                        std::filesystem::path projectRoot) {
    // Tear down any existing instance first (also covers "switching to a
    // project without a compile db": compileCommandsDir == nullopt just
    // leaves both reset, and routing below falls back to tree-sitter).
    if (lspClient_) lspClient_->stop();
    clangdProvider_.reset();
    lspClient_.reset();

    if (!compileCommandsDir) return;

    lspClient_ = std::make_unique<lsp::LspClient>(dispatcher_);
    clangdProvider_ = std::make_unique<lsp::ClangdProvider>(*lspClient_);

    std::vector<std::string> command = {"clangd", "--compile-commands-dir=" + compileCommandsDir->string()};
    std::string rootUri = "file://" + projectRoot.string();

    // Copies onClangdStatusChanged_ by value, not `this` -- safe
    // regardless of this CodeIntelligence's lifetime (ProjectModel/
    // SearchEngine's established precedent). On failure, clangdProvider_
    // is deliberately left allocated-but-inert rather than reset here:
    // isFileReady() can never become true for a clangd that never
    // finished starting, so routing already falls back to tree-sitter
    // without needing to null anything out from inside this callback.
    lspClient_->start(command, rootUri, [callback = onClangdStatusChanged_](bool success) {
        if (callback) callback(success);
    });
}

void CodeIntelligence::notifyFileOpened(const std::string &absolutePath, const std::string &languageId,
                                         std::string utf8Text) {
    if (clangdProvider_) clangdProvider_->didOpen(absolutePath, languageId, std::move(utf8Text));
}

void CodeIntelligence::notifyFileChanged(const std::string &absolutePath, std::string utf8Text) {
    if (clangdProvider_) clangdProvider_->didChange(absolutePath, std::move(utf8Text));
}

void CodeIntelligence::notifyFileSaved(const std::string &absolutePath) {
    if (clangdProvider_) clangdProvider_->didSave(absolutePath);
}

namespace {
QueryLocation toQueryLocation(std::string name, const SymbolLocation &loc) {
    return QueryLocation{std::move(name), loc.file, loc.startRow, loc.startColumn, loc.kind};
}
QueryLocation toQueryLocation(std::string name, const ReferenceLocation &loc) {
    return QueryLocation{std::move(name), loc.file, loc.startRow, loc.startColumn, std::nullopt};
}
QueryLocation toQueryLocation(const lsp::PreciseLocation &loc, std::string name) {
    return QueryLocation{std::move(name), loc.file, loc.startRow, loc.startColumn, std::nullopt};
}
} // namespace

void CodeIntelligence::findDefinitionAsync(std::string name, std::string file, uint32_t row, std::string lineTextUtf8,
                                            uint32_t byteColumn, DefinitionCallback callback) {
    if (clangdProvider_ && clangdProvider_->isFileReady(file)) {
        clangdProvider_->findDefinition(
            file, row, lineTextUtf8, byteColumn, [name, callback](std::vector<lsp::PreciseLocation> locations) {
                DefinitionResult result;
                result.precise = true;
                result.candidates.reserve(locations.size());
                for (const auto &loc : locations) result.candidates.push_back(toQueryLocation(loc, name));
                callback(std::move(result));
            });
        return;
    }

    DefinitionResult result;
    result.precise = false;
    for (const SymbolLocation &loc : index_->findDefinitions(name)) result.candidates.push_back(toQueryLocation(name, loc));
    callback(std::move(result));
}

void CodeIntelligence::findReferencesAsync(std::string name, std::string file, uint32_t row, std::string lineTextUtf8,
                                            uint32_t byteColumn, ReferencesCallback callback) {
    if (clangdProvider_ && clangdProvider_->isFileReady(file)) {
        clangdProvider_->findReferences(
            file, row, lineTextUtf8, byteColumn, [name, callback](std::vector<lsp::PreciseLocation> locations) {
                ReferencesResult result;
                result.precise = true;
                result.candidates.reserve(locations.size());
                for (const auto &loc : locations) result.candidates.push_back(toQueryLocation(loc, name));
                callback(std::move(result));
            });
        return;
    }

    ReferencesResult result;
    result.precise = false;
    for (const ReferenceLocation &loc : index_->findReferences(name)) result.candidates.push_back(toQueryLocation(name, loc));
    callback(std::move(result));
}

void CodeIntelligence::searchWorkspaceSymbolsAsync(std::string query, size_t maxResults,
                                                    WorkspaceSymbolCallback callback) {
    // Workspace symbol search has no single "current file" to gate
    // readiness on -- route to clangd once it's running at all (any file
    // having published diagnostics means the server is alive and its
    // background index is at least underway; a partially-built index just
    // yields fewer results, which is consistent with PRD 5.2's "never
    // block, degrade gracefully" spirit rather than a correctness issue).
    if (clangdProvider_ && lspClient_ && lspClient_->isRunning()) {
        clangdProvider_->searchWorkspaceSymbols(
            query, [callback](std::vector<lsp::WorkspaceSymbolMatch> matches) {
                WorkspaceSymbolResult result;
                result.precise = true;
                result.candidates.reserve(matches.size());
                for (const auto &match : matches) {
                    result.candidates.push_back(QueryLocation{match.name, match.file, match.startRow, match.startColumn, std::nullopt});
                }
                callback(std::move(result));
            });
        return;
    }

    WorkspaceSymbolResult result;
    result.precise = false;
    for (const SymbolLocation &loc : index_->searchSymbols(query, maxResults)) {
        result.candidates.push_back(toQueryLocation(loc.name, loc));
    }
    callback(std::move(result));
}

} // namespace xinsight::core::intel
