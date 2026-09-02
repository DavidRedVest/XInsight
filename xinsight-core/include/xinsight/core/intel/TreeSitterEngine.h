#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xinsight::core::intel {

enum class Language { C, Cpp };

// Best-effort extension -> Language mapping per PRD 5.6's C/C++ file set
// (.c .cc .cpp .cxx .c++ .h .hh .hpp .hxx .h++ .inl, case-insensitive).
// Headers default to the C++ grammar (a superset for the node types our
// queries use); nullopt for anything outside that set. `extension` may or
// may not include the leading '.'.
std::optional<Language> languageForExtension(std::string_view extension);

enum class SymbolKind {
    Function,
    Method,
    Class,
    Struct,
    Enum,
    Union,
    Namespace,
    Typedef,
    Macro,
    GlobalVariable,
};

struct Symbol {
    std::string name;
    SymbolKind kind;
    uint32_t startByte = 0;
    uint32_t endByte = 0;
    uint32_t startRow = 0;    // 0-based
    uint32_t startColumn = 0; // 0-based, in bytes
};

// One occurrence of an identifier-like token (definition or use site) --
// the raw material for the "标识符出现索引" PRD 3.2/5.3 describes for
// find-references: fuzzy/name-based, not scope-resolved.
struct IdentifierOccurrence {
    std::string name;
    uint32_t startByte = 0;
    uint32_t endByte = 0;
    uint32_t startRow = 0;    // 0-based
    uint32_t startColumn = 0; // 0-based, in bytes
};

struct HighlightSpan {
    uint32_t startByte = 0;
    uint32_t endByte = 0;
    std::string capture; // e.g. "keyword", "function.call" (no leading '@')
};

struct FoldRange {
    uint32_t startByte = 0;
    uint32_t endByte = 0;
    uint32_t startRow = 0;
    uint32_t endRow = 0;
};

// What to look up in the symbol index for the identifier under the cursor
// (PRD 2.1's ambient context pane). `lookupName` is either the identifier
// itself (already a type, function, macro, ...) or -- for a variable --
// its resolved declared type name, per PRD 2.1's "变量→类型解码": "光标落在
// 变量上时,不止显示变量声明,而是顺着声明找到其类型并显示该类型的定义".
struct CursorContext {
    std::string lookupName;
    bool isVariableType = false; // true when lookupName came from decoding a variable's type
};

// Mirrors tree-sitter's TSInputEdit. Byte offsets are UTF-8 byte offsets
// into the document; points are 0-based (row, column-in-bytes).
struct Edit {
    uint32_t startByte = 0;
    uint32_t oldEndByte = 0;
    uint32_t newEndByte = 0;
    uint32_t startRow = 0, startColumn = 0;
    uint32_t oldEndRow = 0, oldEndColumn = 0;
    uint32_t newEndRow = 0, newEndColumn = 0;
};

// Owns a parsed tree-sitter tree plus the source buffer it was parsed from.
// Move-only: wraps a tree-sitter tree handle that isn't safe to copy.
class ParsedDocument {
public:
    ParsedDocument();
    ParsedDocument(ParsedDocument&&) noexcept;
    ParsedDocument& operator=(ParsedDocument&&) noexcept;
    ParsedDocument(const ParsedDocument&) = delete;
    ParsedDocument& operator=(const ParsedDocument&) = delete;
    ~ParsedDocument();

    bool valid() const;
    Language language() const;
    std::string_view source() const;

private:
    friend class TreeSitterEngine;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The tree-sitter-backed default code intelligence layer (PRD 4.2 Layer A).
// Stateless aside from the compiled query cache: callers own ParsedDocument
// instances and pass them back in for incremental edits / queries.
class TreeSitterEngine {
public:
    TreeSitterEngine();
    ~TreeSitterEngine();
    TreeSitterEngine(const TreeSitterEngine&) = delete;
    TreeSitterEngine& operator=(const TreeSitterEngine&) = delete;

    ParsedDocument parse(Language language, std::string source) const;

    // Applies an incremental edit and reparses. `newSource` is the full
    // post-edit buffer (tree-sitter still needs it to re-lex changed
    // regions); only the edited byte range is re-lexed thanks to `edit`.
    void applyEdit(ParsedDocument& doc, const Edit& edit, std::string newSource) const;

    // Outline / workspace-symbol-index source: definitions extracted via
    // query/<lang>/tags.scm. Not persisted here -- callers build whatever
    // index they need on top (M1: per-file outline only).
    std::vector<Symbol> outline(const ParsedDocument& doc) const;

    // Find-references source (PRD 3.2/5.3): every identifier-like token in
    // the document via query/<lang>/references.scm -- definitions and uses
    // alike, fuzzy/name-based rather than scope-resolved. Never includes
    // text inside comments or string/char literals (those aren't lexed as
    // identifier nodes at all).
    std::vector<IdentifierOccurrence> references(const ParsedDocument& doc) const;

    std::vector<FoldRange> folds(const ParsedDocument& doc) const;

    // Resolves the identifier under `byteOffset` for the context pane
    // (PRD 2.1). A type name resolves to itself. A variable name -- at
    // its own declaration site, or a later use within the same
    // function/parameter scope -- resolves to its declared type's name
    // (e.g. `struct S *psvar` -> "S"), approximated syntactically (no
    // real scope resolution): nearest preceding same-name declaration
    // within the enclosing function wins. Falls back to the identifier's
    // own text when no type can be resolved (PRD's accepted "做不到时
    // 回落为显示变量声明本身" -- CodeIntelligence just won't find a
    // definition for an unresolvable local, which is an acceptable no-op
    // outcome, not an error). Returns nullopt when the cursor isn't on an
    // identifier-like token at all.
    std::optional<CursorContext> identifierAtByteOffset(const ParsedDocument& doc, uint32_t byteOffset) const;

    // For C++ documents, runs query/c/highlights.scm then
    // query/cpp/highlights.scm and concatenates results; later entries in
    // the returned vector are meant to win when a renderer applies them in
    // order over the same byte range (see query/cpp/highlights.scm's header
    // comment).
    std::vector<HighlightSpan> highlights(const ParsedDocument& doc) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace xinsight::core::intel
