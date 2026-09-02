#include "xinsight/core/intel/CodeIntelligence.h"

#include <fstream>
#include <iterator>
#include <optional>

#include "xinsight/core/encoding/TextCodec.h"

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

} // namespace xinsight::core::intel
