#include "xinsight/core/intel/SymbolIndex.h"

#include <algorithm>
#include <cctype>

namespace xinsight::core::intel {

namespace {
std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}
} // namespace

void InMemorySymbolIndex::removeFile(const std::string &file) {
    auto it = filesIndexed_.find(file);
    if (it == filesIndexed_.end()) return;

    for (const std::string &name : it->second.definitionNames) {
        auto bucketIt = definitionsByName_.find(name);
        if (bucketIt == definitionsByName_.end()) continue;
        auto &bucket = bucketIt->second;
        bucket.erase(std::remove_if(bucket.begin(), bucket.end(),
                                     [&](const SymbolLocation &loc) { return loc.file == file; }),
                     bucket.end());
        if (bucket.empty()) definitionsByName_.erase(bucketIt);
    }

    for (const std::string &name : it->second.referenceNames) {
        auto bucketIt = referencesByName_.find(name);
        if (bucketIt == referencesByName_.end()) continue;
        auto &bucket = bucketIt->second;
        bucket.erase(std::remove_if(bucket.begin(), bucket.end(),
                                     [&](const ReferenceLocation &loc) { return loc.file == file; }),
                     bucket.end());
        if (bucket.empty()) referencesByName_.erase(bucketIt);
    }

    filesIndexed_.erase(it);
}

void InMemorySymbolIndex::updateFile(const std::string &file, std::vector<Symbol> symbols,
                                      std::vector<IdentifierOccurrence> references) {
    removeFile(file); // clear any prior entries for this file first

    FileEntry entry;
    entry.definitionNames.reserve(symbols.size());
    entry.referenceNames.reserve(references.size());

    for (Symbol &symbol : symbols) {
        SymbolLocation loc;
        loc.name = symbol.name;
        loc.kind = symbol.kind;
        loc.file = file;
        loc.startByte = symbol.startByte;
        loc.startRow = symbol.startRow;
        loc.startColumn = symbol.startColumn;
        entry.definitionNames.push_back(symbol.name);
        definitionsByName_[symbol.name].push_back(std::move(loc));
    }

    for (IdentifierOccurrence &ref : references) {
        ReferenceLocation loc;
        loc.name = ref.name;
        loc.file = file;
        loc.startByte = ref.startByte;
        loc.startRow = ref.startRow;
        loc.startColumn = ref.startColumn;
        entry.referenceNames.push_back(ref.name);
        referencesByName_[ref.name].push_back(std::move(loc));
    }

    filesIndexed_[file] = std::move(entry);
}

void InMemorySymbolIndex::clear() {
    definitionsByName_.clear();
    referencesByName_.clear();
    filesIndexed_.clear();
}

std::vector<SymbolLocation> InMemorySymbolIndex::findDefinitions(const std::string &name) const {
    auto it = definitionsByName_.find(name);
    if (it == definitionsByName_.end()) return {};
    return it->second;
}

std::vector<ReferenceLocation> InMemorySymbolIndex::findReferences(const std::string &name) const {
    auto it = referencesByName_.find(name);
    if (it == referencesByName_.end()) return {};
    return it->second;
}

std::vector<SymbolLocation> InMemorySymbolIndex::searchSymbols(const std::string &query, size_t maxResults) const {
    std::string lowerQuery = toLower(query);
    std::vector<SymbolLocation> matches;
    for (const auto &[name, locations] : definitionsByName_) {
        if (toLower(name).find(lowerQuery) == std::string::npos) continue;
        matches.insert(matches.end(), locations.begin(), locations.end());
    }

    std::sort(matches.begin(), matches.end(), [](const SymbolLocation &a, const SymbolLocation &b) {
        if (a.name != b.name) return a.name < b.name;
        return a.file < b.file;
    });
    if (matches.size() > maxResults) matches.resize(maxResults);
    return matches;
}

} // namespace xinsight::core::intel
