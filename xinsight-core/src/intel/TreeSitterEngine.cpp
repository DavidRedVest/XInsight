#include "xinsight/core/intel/TreeSitterEngine.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <functional>
#include <regex>
#include <stdexcept>
#include <string>

#include <tree_sitter/api.h>

#include "xinsight/core/queries/c_folds.h"
#include "xinsight/core/queries/c_highlights.h"
#include "xinsight/core/queries/c_references.h"
#include "xinsight/core/queries/c_tags.h"
#include "xinsight/core/queries/cpp_folds.h"
#include "xinsight/core/queries/cpp_highlights.h"
#include "xinsight/core/queries/cpp_references.h"
#include "xinsight/core/queries/cpp_tags.h"

extern "C" {
const TSLanguage *tree_sitter_c(void);
const TSLanguage *tree_sitter_cpp(void);
}

namespace xinsight::core::intel {

namespace {

const TSLanguage *languageHandle(Language lang) {
    return lang == Language::C ? tree_sitter_c() : tree_sitter_cpp();
}

std::string_view nodeText(TSNode node, std::string_view source) {
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    return source.substr(start, end - start);
}

TSQuery *compileQuery(const TSLanguage *lang, std::string_view source, const char *debugName) {
    uint32_t errorOffset = 0;
    TSQueryError errorType = TSQueryErrorNone;
    TSQuery *query = ts_query_new(lang, source.data(), static_cast<uint32_t>(source.size()), &errorOffset, &errorType);
    if (query == nullptr) {
        throw std::runtime_error(std::string("xinsight-core: failed to compile query '") + debugName +
                                  "' (error type " + std::to_string(static_cast<int>(errorType)) +
                                  " at byte offset " + std::to_string(errorOffset) + ")");
    }
    return query;
}

// A predicate argument is either a captured node (with its text) or a
// string literal from the query source.
struct PredArg {
    bool isCapture = false;
    TSNode node{};
    std::string_view text;
};

bool typeMatchesAny(TSNode node, const std::vector<PredArg> &literals, size_t from) {
    std::string_view type = ts_node_type(node);
    for (size_t i = from; i < literals.size(); ++i) {
        if (literals[i].text == type) return true;
    }
    return false;
}

bool evaluateOnePredicate(std::string_view name, const std::vector<PredArg> &args) {
    if (args.empty()) return true; // malformed; don't block the match

    if (name == "eq?") {
        return args.size() >= 2 && args[0].text == args[1].text;
    }
    if (name == "not-eq?") {
        return !(args.size() >= 2 && args[0].text == args[1].text);
    }
    if (name == "match?" || name == "not-match?") {
        if (args.size() < 2) return true;
        bool matched = false;
        try {
            std::regex re{std::string(args[1].text)};
            matched = std::regex_search(args[0].text.begin(), args[0].text.end(), re);
        } catch (const std::regex_error &) {
            matched = false;
        }
        return name == "match?" ? matched : !matched;
    }
    if (name == "any-of?" || name == "not-any-of?") {
        bool found = false;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[0].text == args[i].text) {
                found = true;
                break;
            }
        }
        return name == "any-of?" ? found : !found;
    }
    if (name == "has-ancestor?" || name == "not-has-ancestor?") {
        if (!args[0].isCapture) return true;
        bool found = false;
        TSNode ancestor = ts_node_parent(args[0].node);
        while (!ts_node_is_null(ancestor)) {
            if (typeMatchesAny(ancestor, args, 1)) {
                found = true;
                break;
            }
            ancestor = ts_node_parent(ancestor);
        }
        return name == "has-ancestor?" ? found : !found;
    }
    if (name == "has-parent?" || name == "not-has-parent?") {
        if (!args[0].isCapture) return true;
        TSNode parent = ts_node_parent(args[0].node);
        bool found = !ts_node_is_null(parent) && typeMatchesAny(parent, args, 1);
        return name == "has-parent?" ? found : !found;
    }
    // Unknown predicate: don't block the match on something we don't
    // understand rather than silently dropping otherwise-good captures.
    return true;
}

bool evaluatePredicates(const TSQuery *query, const TSQueryMatch &match, std::string_view source) {
    uint32_t stepCount = 0;
    const TSQueryPredicateStep *steps = ts_query_predicates_for_pattern(query, match.pattern_index, &stepCount);

    uint32_t i = 0;
    while (i < stepCount) {
        uint32_t start = i;
        while (i < stepCount && steps[i].type != TSQueryPredicateStepTypeDone) ++i;
        uint32_t end = i; // exclusive
        if (i < stepCount) ++i; // skip Done

        if (end <= start || steps[start].type != TSQueryPredicateStepTypeString) continue;

        uint32_t nameLen = 0;
        const char *namePtr = ts_query_string_value_for_id(query, steps[start].value_id, &nameLen);
        std::string_view predName(namePtr, nameLen);

        std::vector<PredArg> args;
        for (uint32_t k = start + 1; k < end; ++k) {
            if (steps[k].type == TSQueryPredicateStepTypeCapture) {
                for (uint16_t c = 0; c < match.capture_count; ++c) {
                    if (match.captures[c].index == steps[k].value_id) {
                        PredArg arg;
                        arg.isCapture = true;
                        arg.node = match.captures[c].node;
                        arg.text = nodeText(arg.node, source);
                        args.push_back(arg);
                        break;
                    }
                }
            } else {
                uint32_t len = 0;
                const char *p = ts_query_string_value_for_id(query, steps[k].value_id, &len);
                PredArg arg;
                arg.isCapture = false;
                arg.text = std::string_view(p, len);
                args.push_back(arg);
            }
        }

        if (!evaluateOnePredicate(predName, args)) return false;
    }
    return true;
}

template <typename Fn>
void forEachMatch(const TSQuery *query, TSNode root, std::string_view source, Fn &&callback) {
    TSQueryCursor *cursor = ts_query_cursor_new();
    ts_query_cursor_exec(cursor, query, root);
    TSQueryMatch match;
    while (ts_query_cursor_next_match(cursor, &match)) {
        if (!evaluatePredicates(query, match, source)) continue;
        callback(match);
    }
    ts_query_cursor_delete(cursor);
}

std::string_view captureName(const TSQuery *query, uint32_t index) {
    uint32_t len = 0;
    const char *p = ts_query_capture_name_for_id(query, index, &len);
    return std::string_view(p, len);
}

std::optional<SymbolKind> symbolKindForDefinitionCapture(std::string_view name) {
    // name looks like "definition.function", "definition.method", ...
    static constexpr std::pair<std::string_view, SymbolKind> kMap[] = {
        {"definition.function", SymbolKind::Function},
        {"definition.method", SymbolKind::Method},
        {"definition.class", SymbolKind::Class},
        {"definition.struct", SymbolKind::Struct},
        {"definition.enum", SymbolKind::Enum},
        {"definition.union", SymbolKind::Union},
        {"definition.namespace", SymbolKind::Namespace},
        {"definition.typedef", SymbolKind::Typedef},
        {"definition.macro", SymbolKind::Macro},
        {"definition.variable", SymbolKind::GlobalVariable},
    };
    for (const auto &[key, kind] : kMap) {
        if (key == name) return kind;
    }
    return std::nullopt;
}

// Wrapper declarator node types tree-sitter interposes between an
// identifier and its owning declaration/parameter_declaration/
// field_declaration (e.g. `*w` is a pointer_declarator wrapping the
// identifier `w`). Walking up through these is how identifierAtByteOffset
// finds "the declaration this identifier is the name of".
bool isDeclaratorWrapper(std::string_view type) {
    static constexpr std::string_view kWrappers[] = {
        "pointer_declarator", "array_declarator",   "init_declarator",
        "reference_declarator", "parenthesized_declarator", "attributed_declarator",
    };
    for (auto w : kWrappers) {
        if (w == type) return true;
    }
    return false;
}

bool isDeclarationHolder(std::string_view type) {
    return type == "declaration" || type == "parameter_declaration" || type == "field_declaration";
}

// Extracts the plain type name text from a declaration's `type:` field
// node. struct/union/enum/class specifiers carry the name in their own
// `name:` field; a bare type_identifier (typedef'd name) already is the
// name; primitive/sized types have no separate name node, so their own
// text is used as a best-effort lookup key (harmless if it doesn't match
// anything in the symbol index -- PRD's accepted fallback).
std::optional<std::string> typeNameFromTypeField(TSNode typeNode, std::string_view source) {
    if (ts_node_is_null(typeNode)) return std::nullopt;
    std::string_view type = ts_node_type(typeNode);

    if (type == "type_identifier") return std::string(nodeText(typeNode, source));

    if (type == "struct_specifier" || type == "union_specifier" || type == "enum_specifier" ||
        type == "class_specifier") {
        TSNode nameNode = ts_node_child_by_field_name(typeNode, "name", 4);
        if (ts_node_is_null(nameNode)) return std::nullopt; // anonymous struct/union/enum
        return std::string(nodeText(nameNode, source));
    }

    // primitive_type, sized_type_specifier, template_type,
    // qualified_identifier, decltype, dependent_type, ... : no dedicated
    // name field, so just use the node's own text.
    return std::string(nodeText(typeNode, source));
}

// Case A: `node` is (possibly wrapped in pointer/array/init/... declarator
// nodes) the "declarator" of an enclosing declaration/parameter_declaration
// /field_declaration -- i.e. the cursor is on the variable's own
// declaration site. Returns that declaration's resolved type name.
std::optional<std::string> declaredTypeAtDeclarationSite(TSNode node, std::string_view source) {
    TSNode current = node;
    while (!ts_node_is_null(current)) {
        TSNode parent = ts_node_parent(current);
        if (ts_node_is_null(parent)) return std::nullopt;

        std::string_view parentType = ts_node_type(parent);
        if (isDeclarationHolder(parentType)) {
            // Multiple comma-separated declarators (`int a, *b;`) aren't
            // distinguished here -- child_by_field_name returns the first
            // one, so a match against a later declarator in the same
            // statement is missed. Accepted v1 limitation (rare style).
            TSNode declarator = ts_node_child_by_field_name(parent, "declarator", 10);
            if (!ts_node_is_null(declarator) && ts_node_eq(declarator, current)) {
                TSNode typeField = ts_node_child_by_field_name(parent, "type", 4);
                return typeNameFromTypeField(typeField, source);
            }
            return std::nullopt;
        }

        if (!isDeclaratorWrapper(parentType)) return std::nullopt;
        current = parent;
    }
    return std::nullopt;
}

// Case B: `node` is a use of `name` somewhere other than its declaration
// site. Walks up to the enclosing function_definition (covering both its
// parameter list and body) and searches its declarations for the nearest
// one (by start byte, preferring one that precedes `node`) whose
// declarator name matches, returning its resolved type name.
std::optional<std::string> declaredTypeFromEnclosingScope(TSNode node, std::string_view name,
                                                           std::string_view source) {
    TSNode scope = ts_node_parent(node);
    while (!ts_node_is_null(scope) && std::string_view(ts_node_type(scope)) != "function_definition") {
        scope = ts_node_parent(scope);
    }
    if (ts_node_is_null(scope)) return std::nullopt;

    uint32_t cursorStart = ts_node_start_byte(node);
    std::optional<std::string> best;
    uint32_t bestDistance = UINT32_MAX;
    bool bestPrecedes = false;

    std::function<void(TSNode)> visit = [&](TSNode n) {
        std::string_view type = ts_node_type(n);
        // Don't descend into a nested function body -- its locals aren't
        // in scope here. (Nested `function_definition` doesn't occur in
        // C; harmless guard for C++ member/lambda edge cases.)
        if (isDeclarationHolder(type)) {
            TSNode declarator = ts_node_child_by_field_name(n, "declarator", 10);
            if (!ts_node_is_null(declarator)) {
                TSNode innermost = declarator;
                while (isDeclaratorWrapper(ts_node_type(innermost))) {
                    TSNode inner = ts_node_child_by_field_name(innermost, "declarator", 10);
                    if (ts_node_is_null(inner)) break;
                    innermost = inner;
                }
                if (nodeText(innermost, source) == name) {
                    uint32_t declStart = ts_node_start_byte(n);
                    bool precedes = declStart <= cursorStart;
                    uint32_t distance = precedes ? cursorStart - declStart : declStart - cursorStart;
                    // Prefer any preceding declaration over any
                    // non-preceding one; within the same precedes-ness,
                    // prefer the closest.
                    bool better = (precedes && !bestPrecedes) || (precedes == bestPrecedes && distance < bestDistance);
                    if (better) {
                        TSNode typeField = ts_node_child_by_field_name(n, "type", 4);
                        best = typeNameFromTypeField(typeField, source);
                        bestDistance = distance;
                        bestPrecedes = precedes;
                    }
                }
            }
            return; // a declaration's internals (initializer expressions, ...) aren't more declarations
        }
        uint32_t count = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < count; ++i) visit(ts_node_named_child(n, i));
    };
    visit(scope);

    return best;
}

} // namespace

struct ParsedDocument::Impl {
    Language language = Language::C;
    std::string source;
    TSTree *tree = nullptr;

    ~Impl() {
        if (tree != nullptr) ts_tree_delete(tree);
    }
};

ParsedDocument::ParsedDocument() = default;
ParsedDocument::ParsedDocument(ParsedDocument &&) noexcept = default;
ParsedDocument &ParsedDocument::operator=(ParsedDocument &&) noexcept = default;
ParsedDocument::~ParsedDocument() = default;

bool ParsedDocument::valid() const {
    return impl_ != nullptr && impl_->tree != nullptr;
}

Language ParsedDocument::language() const {
    return impl_->language;
}

std::string_view ParsedDocument::source() const {
    return impl_->source;
}

struct TreeSitterEngine::Impl {
    TSParser *parser = ts_parser_new();

    TSQuery *cHighlightsForC = nullptr;
    TSQuery *cHighlightsForCpp = nullptr;
    TSQuery *cppHighlightsForCpp = nullptr;
    TSQuery *cTags = nullptr;
    TSQuery *cFolds = nullptr;
    TSQuery *cReferences = nullptr;
    TSQuery *cppTags = nullptr;
    TSQuery *cppFolds = nullptr;
    TSQuery *cppReferences = nullptr;

    Impl() {
        cHighlightsForC = compileQuery(tree_sitter_c(), queries::kCHighlights, "c/highlights.scm (C)");
        cHighlightsForCpp = compileQuery(tree_sitter_cpp(), queries::kCHighlights, "c/highlights.scm (C++)");
        cppHighlightsForCpp = compileQuery(tree_sitter_cpp(), queries::kCppHighlights, "cpp/highlights.scm");
        cTags = compileQuery(tree_sitter_c(), queries::kCTags, "c/tags.scm");
        cFolds = compileQuery(tree_sitter_c(), queries::kCFolds, "c/folds.scm");
        cReferences = compileQuery(tree_sitter_c(), queries::kCReferences, "c/references.scm");
        cppTags = compileQuery(tree_sitter_cpp(), queries::kCppTags, "cpp/tags.scm");
        cppFolds = compileQuery(tree_sitter_cpp(), queries::kCppFolds, "cpp/folds.scm");
        cppReferences = compileQuery(tree_sitter_cpp(), queries::kCppReferences, "cpp/references.scm");
    }

    ~Impl() {
        ts_query_delete(cHighlightsForC);
        ts_query_delete(cHighlightsForCpp);
        ts_query_delete(cppHighlightsForCpp);
        ts_query_delete(cTags);
        ts_query_delete(cFolds);
        ts_query_delete(cReferences);
        ts_query_delete(cppTags);
        ts_query_delete(cppFolds);
        ts_query_delete(cppReferences);
        ts_parser_delete(parser);
    }
};

TreeSitterEngine::TreeSitterEngine() : impl_(std::make_unique<Impl>()) {}
TreeSitterEngine::~TreeSitterEngine() = default;

ParsedDocument TreeSitterEngine::parse(Language language, std::string source) const {
    ParsedDocument doc;
    doc.impl_ = std::make_unique<ParsedDocument::Impl>();
    doc.impl_->language = language;
    doc.impl_->source = std::move(source);

    ts_parser_set_language(impl_->parser, languageHandle(language));
    doc.impl_->tree = ts_parser_parse_string(impl_->parser, nullptr, doc.impl_->source.data(),
                                              static_cast<uint32_t>(doc.impl_->source.size()));
    return doc;
}

void TreeSitterEngine::applyEdit(ParsedDocument &doc, const Edit &edit, std::string newSource) const {
    TSInputEdit tsEdit{};
    tsEdit.start_byte = edit.startByte;
    tsEdit.old_end_byte = edit.oldEndByte;
    tsEdit.new_end_byte = edit.newEndByte;
    tsEdit.start_point = TSPoint{edit.startRow, edit.startColumn};
    tsEdit.old_end_point = TSPoint{edit.oldEndRow, edit.oldEndColumn};
    tsEdit.new_end_point = TSPoint{edit.newEndRow, edit.newEndColumn};

    ts_tree_edit(doc.impl_->tree, &tsEdit);
    doc.impl_->source = std::move(newSource);

    ts_parser_set_language(impl_->parser, languageHandle(doc.impl_->language));
    TSTree *newTree = ts_parser_parse_string(impl_->parser, doc.impl_->tree, doc.impl_->source.data(),
                                              static_cast<uint32_t>(doc.impl_->source.size()));
    ts_tree_delete(doc.impl_->tree);
    doc.impl_->tree = newTree;
}

std::vector<Symbol> TreeSitterEngine::outline(const ParsedDocument &doc) const {
    std::vector<Symbol> result;
    if (!doc.valid()) return result;

    const TSQuery *query = doc.impl_->language == Language::C ? impl_->cTags : impl_->cppTags;
    TSNode root = ts_tree_root_node(doc.impl_->tree);

    forEachMatch(query, root, doc.impl_->source, [&](const TSQueryMatch &match) {
        std::optional<TSNode> nameNode;
        std::optional<SymbolKind> kind;
        for (uint16_t c = 0; c < match.capture_count; ++c) {
            std::string_view name = captureName(query, match.captures[c].index);
            if (name == "name") {
                nameNode = match.captures[c].node;
            } else if (auto k = symbolKindForDefinitionCapture(name)) {
                kind = k;
            }
        }
        if (!nameNode || !kind) return;

        Symbol symbol;
        symbol.name = std::string(nodeText(*nameNode, doc.impl_->source));
        symbol.kind = *kind;
        symbol.startByte = ts_node_start_byte(*nameNode);
        symbol.endByte = ts_node_end_byte(*nameNode);
        TSPoint start = ts_node_start_point(*nameNode);
        symbol.startRow = start.row;
        symbol.startColumn = start.column;
        result.push_back(std::move(symbol));
    });

    return result;
}

std::vector<IdentifierOccurrence> TreeSitterEngine::references(const ParsedDocument &doc) const {
    std::vector<IdentifierOccurrence> result;
    if (!doc.valid()) return result;

    const TSQuery *query = doc.impl_->language == Language::C ? impl_->cReferences : impl_->cppReferences;
    TSNode root = ts_tree_root_node(doc.impl_->tree);

    forEachMatch(query, root, doc.impl_->source, [&](const TSQueryMatch &match) {
        for (uint16_t c = 0; c < match.capture_count; ++c) {
            if (captureName(query, match.captures[c].index) != "reference") continue;
            TSNode node = match.captures[c].node;

            IdentifierOccurrence occurrence;
            occurrence.name = std::string(nodeText(node, doc.impl_->source));
            occurrence.startByte = ts_node_start_byte(node);
            occurrence.endByte = ts_node_end_byte(node);
            TSPoint start = ts_node_start_point(node);
            occurrence.startRow = start.row;
            occurrence.startColumn = start.column;
            result.push_back(std::move(occurrence));
        }
    });

    return result;
}

std::vector<FoldRange> TreeSitterEngine::folds(const ParsedDocument &doc) const {
    std::vector<FoldRange> result;
    if (!doc.valid()) return result;

    const TSQuery *query = doc.impl_->language == Language::C ? impl_->cFolds : impl_->cppFolds;
    TSNode root = ts_tree_root_node(doc.impl_->tree);

    forEachMatch(query, root, doc.impl_->source, [&](const TSQueryMatch &match) {
        for (uint16_t c = 0; c < match.capture_count; ++c) {
            if (captureName(query, match.captures[c].index) != "fold") continue;
            TSNode node = match.captures[c].node;
            FoldRange range;
            range.startByte = ts_node_start_byte(node);
            range.endByte = ts_node_end_byte(node);
            range.startRow = ts_node_start_point(node).row;
            range.endRow = ts_node_end_point(node).row;
            if (range.endRow > range.startRow) result.push_back(range);
        }
    });

    return result;
}

std::optional<CursorContext> TreeSitterEngine::identifierAtByteOffset(const ParsedDocument &doc,
                                                                       uint32_t byteOffset) const {
    if (!doc.valid()) return std::nullopt;

    TSNode root = ts_tree_root_node(doc.impl_->tree);
    TSNode node = ts_node_descendant_for_byte_range(root, byteOffset, byteOffset);
    if (ts_node_is_null(node)) return std::nullopt;

    std::string_view type = ts_node_type(node);
    std::string_view source = doc.impl_->source;

    if (type == "type_identifier") {
        return CursorContext{std::string(nodeText(node, source)), false};
    }
    if (type != "identifier") return std::nullopt;

    std::string name(nodeText(node, source));

    if (auto declSiteType = declaredTypeAtDeclarationSite(node, source)) {
        return CursorContext{*declSiteType, true};
    }
    if (auto scopeType = declaredTypeFromEnclosingScope(node, name, source)) {
        return CursorContext{*scopeType, true};
    }

    return CursorContext{name, false};
}

std::vector<HighlightSpan> TreeSitterEngine::highlights(const ParsedDocument &doc) const {
    std::vector<HighlightSpan> result;
    if (!doc.valid()) return result;

    TSNode root = ts_tree_root_node(doc.impl_->tree);

    auto collect = [&](const TSQuery *query) {
        forEachMatch(query, root, doc.impl_->source, [&](const TSQueryMatch &match) {
            for (uint16_t c = 0; c < match.capture_count; ++c) {
                std::string_view name = captureName(query, match.captures[c].index);
                if (name.empty() || name[0] == '_') continue; // internal predicate-binding capture
                TSNode node = match.captures[c].node;
                result.push_back(HighlightSpan{ts_node_start_byte(node), ts_node_end_byte(node), std::string(name)});
            }
        });
    };

    if (doc.impl_->language == Language::C) {
        collect(impl_->cHighlightsForC);
    } else {
        collect(impl_->cHighlightsForCpp);
        collect(impl_->cppHighlightsForCpp);
    }

    return result;
}

std::optional<Language> languageForExtension(std::string_view extension) {
    std::string ext(extension);
    if (!ext.empty() && ext.front() == '.') ext.erase(0, 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return std::tolower(ch); });

    if (ext == "c") return Language::C;

    static constexpr std::array<std::string_view, 9> kCppExtensions = {
        "cc", "cpp", "cxx", "c++", "h", "hh", "hpp", "hxx", "h++",
    };
    for (auto candidate : kCppExtensions) {
        if (ext == candidate) return Language::Cpp;
    }
    if (ext == "inl") return Language::Cpp;

    return std::nullopt;
}

} // namespace xinsight::core::intel
