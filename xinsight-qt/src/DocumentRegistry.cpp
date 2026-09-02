#include "DocumentRegistry.h"

DocumentRegistry::Entry *DocumentRegistry::acquireExisting(const std::filesystem::path &canonicalPath) {
    auto it = slots_.find(canonicalPath);
    if (it == slots_.end()) return nullptr;
    ++it->second.refCount;
    return &it->second.entry;
}

DocumentRegistry::Entry &DocumentRegistry::registerNew(const std::filesystem::path &canonicalPath,
                                                         QsciDocument document,
                                                         xinsight::core::encoding::TextEncoding encoding,
                                                         xinsight::core::encoding::LineEnding lineEnding, bool hadBom) {
    Slot slot;
    slot.entry.document = document;
    slot.entry.encoding = encoding;
    slot.entry.lineEnding = lineEnding;
    slot.entry.hadBom = hadBom;
    slot.refCount = 1;

    auto [it, inserted] = slots_.insert_or_assign(canonicalPath, std::move(slot));
    return it->second.entry;
}

void DocumentRegistry::release(const std::filesystem::path &canonicalPath) {
    auto it = slots_.find(canonicalPath);
    if (it == slots_.end()) return;
    if (--it->second.refCount <= 0) slots_.erase(it);
}
