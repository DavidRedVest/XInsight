#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "xinsight/core/intel/TreeSitterEngine.h"

// Backend-agnostic workspace symbol index (PRD 5.3/5.3.1): the equivalent
// of Source Insight's symbol database, built from TreeSitterEngine's
// per-file Symbol/IdentifierOccurrence extraction. Upper layers
// (CodeIntelligence) depend only on ISymbolIndex, never on a concrete
// backend -- v1 ships only InMemorySymbolIndex; a SqliteSymbolIndex is
// explicitly deferred (not a v1 goal, see PRD 5.3.1) but must be able to
// reuse tests/SymbolIndex_test.cpp's interface-level suite unchanged.
namespace xinsight::core::intel {

struct SymbolLocation {
    std::string name;
    SymbolKind kind;
    std::string file; // absolute path, as passed to updateFile()
    uint32_t startByte = 0;
    uint32_t startRow = 0;    // 0-based
    uint32_t startColumn = 0; // 0-based, in bytes
    // Enclosing scope name (class/namespace/function), when known. Always
    // empty in v1: query/{c,cpp}/tags.scm doesn't track nesting containers
    // yet. Kept in the data model now (PRD 5.3's record shape names it)
    // so adding that later doesn't touch this interface again.
    std::string container;
};

struct ReferenceLocation {
    std::string name;
    std::string file;
    uint32_t startByte = 0;
    uint32_t startRow = 0;
    uint32_t startColumn = 0;
};

class ISymbolIndex {
public:
    virtual ~ISymbolIndex() = default;

    // Replaces all definitions/references previously recorded for `file`
    // with `symbols`/`references` -- whole-file granularity, matching
    // tree-sitter's per-file re-parse granularity (simplest correct
    // semantics for v1; see PRD 8.7 for the edit->reparse->index closure
    // this backs). `file` must be an absolute, canonical path used
    // consistently by all callers -- it's both the map key and the value
    // returned in query results.
    virtual void updateFile(const std::string &file, std::vector<Symbol> symbols,
                             std::vector<IdentifierOccurrence> references) = 0;

    // Drops all entries previously recorded for `file` (e.g. file
    // deleted/excluded from the project). No-op if `file` isn't indexed.
    virtual void removeFile(const std::string &file) = 0;

    virtual void clear() = 0;

    // Exact-name definition lookup (jump-to-definition candidates). May
    // return multiple hits -- same-name symbols across different files are
    // expected and valid (PRD's "fuzzy, always works" lookup, not
    // scope-resolved); candidate ranking is a GUI/context-engine concern.
    virtual std::vector<SymbolLocation> findDefinitions(const std::string &name) const = 0;

    // Exact-name identifier-occurrence lookup (fuzzy find-references).
    virtual std::vector<ReferenceLocation> findReferences(const std::string &name) const = 0;

    // Case-insensitive substring match over all known definition names,
    // for workspace symbol search (Cmd+T). Results are sorted by
    // (name, file) for deterministic output; capped at `maxResults`.
    virtual std::vector<SymbolLocation> searchSymbols(const std::string &query, size_t maxResults) const = 0;
};

// v1's only ISymbolIndex backend (PRD 5.3.1): plain in-memory hash maps,
// rebuilt on every app start via a full project scan. No persistence --
// that's explicitly deferred to a future SqliteSymbolIndex, not a v1 goal.
class InMemorySymbolIndex final : public ISymbolIndex {
public:
    void updateFile(const std::string &file, std::vector<Symbol> symbols,
                     std::vector<IdentifierOccurrence> references) override;
    void removeFile(const std::string &file) override;
    void clear() override;
    std::vector<SymbolLocation> findDefinitions(const std::string &name) const override;
    std::vector<ReferenceLocation> findReferences(const std::string &name) const override;
    std::vector<SymbolLocation> searchSymbols(const std::string &query, size_t maxResults) const override;

private:
    // Names this file contributed, so removeFile()/updateFile() can find
    // and erase exactly this file's entries out of the by-name buckets
    // below without touching other files' same-named entries.
    struct FileEntry {
        std::vector<std::string> definitionNames;
        std::vector<std::string> referenceNames;
    };

    std::unordered_map<std::string, std::vector<SymbolLocation>> definitionsByName_;
    std::unordered_map<std::string, std::vector<ReferenceLocation>> referencesByName_;
    std::unordered_map<std::string, FileEntry> filesIndexed_;
};

} // namespace xinsight::core::intel
