#include <algorithm>
#include <doctest/doctest.h>

#include "xinsight/core/intel/TreeSitterEngine.h"

using namespace xinsight::core::intel;

namespace {

bool hasSymbol(const std::vector<Symbol> &symbols, std::string_view name, SymbolKind kind) {
    return std::any_of(symbols.begin(), symbols.end(),
                        [&](const Symbol &s) { return s.name == name && s.kind == kind; });
}

bool hasSymbolNamed(const std::vector<Symbol> &symbols, std::string_view name) {
    return std::any_of(symbols.begin(), symbols.end(), [&](const Symbol &s) { return s.name == name; });
}

bool hasHighlight(const std::vector<HighlightSpan> &spans, std::string_view capture) {
    return std::any_of(spans.begin(), spans.end(), [&](const HighlightSpan &s) { return s.capture == capture; });
}

} // namespace

TEST_CASE("languageForExtension follows PRD 5.6's C/C++ extension set") {
    CHECK(languageForExtension(".c") == Language::C);
    CHECK(languageForExtension("c") == Language::C);
    CHECK(languageForExtension(".cpp") == Language::Cpp);
    CHECK(languageForExtension(".CC") == Language::Cpp);
    CHECK(languageForExtension(".h") == Language::Cpp);
    CHECK(languageForExtension(".hpp") == Language::Cpp);
    CHECK(languageForExtension(".inl") == Language::Cpp);
    CHECK(languageForExtension(".py") == std::nullopt);
}

TEST_CASE("C: outline extracts definitions and excludes locals") {
    TreeSitterEngine engine;
    std::string source = R"(
#define MAX_SIZE 128
#define SQUARE(x) ((x) * (x))

struct Point { int x; int y; };
enum Color { RED, GREEN, BLUE };
union Value { int i; float f; };
typedef struct Point PointAlias;

int g_counter = 0;
static const char *g_name = "hello";

int add(int a, int b) {
    int local = a + b;
    return local;
}

struct Point *make_point(int x, int y) {
    struct Point *p = 0;
    return p;
}
)";

    ParsedDocument doc = engine.parse(Language::C, source);
    REQUIRE(doc.valid());

    auto symbols = engine.outline(doc);

    CHECK(hasSymbol(symbols, "MAX_SIZE", SymbolKind::Macro));
    CHECK(hasSymbol(symbols, "SQUARE", SymbolKind::Macro));
    CHECK(hasSymbol(symbols, "Point", SymbolKind::Struct));
    CHECK(hasSymbol(symbols, "Color", SymbolKind::Enum));
    CHECK(hasSymbol(symbols, "Value", SymbolKind::Union));
    CHECK(hasSymbol(symbols, "PointAlias", SymbolKind::Typedef));
    CHECK(hasSymbol(symbols, "g_counter", SymbolKind::GlobalVariable));
    CHECK(hasSymbol(symbols, "g_name", SymbolKind::GlobalVariable));
    CHECK(hasSymbol(symbols, "add", SymbolKind::Function));
    CHECK(hasSymbol(symbols, "make_point", SymbolKind::Function));

    // Locals must not leak into the (file-scope) outline.
    CHECK_FALSE(hasSymbolNamed(symbols, "local"));
    CHECK_FALSE(hasSymbolNamed(symbols, "p"));
}

TEST_CASE("C: K&R-style function definitions are still recognized") {
    // function_definition's node shape is identical for K&R style (the
    // old-style parameter declarations are just extra `declaration`
    // children our tags.scm pattern doesn't look at), so this should work
    // without any special-casing -- verified against tree-sitter-c's
    // node-types.json before writing tags.scm.
    TreeSitterEngine engine;
    std::string source = R"(
int add(a, b)
    int a;
    int b;
{
    return a + b;
}
)";

    ParsedDocument doc = engine.parse(Language::C, source);
    REQUIRE(doc.valid());

    auto symbols = engine.outline(doc);
    CHECK(hasSymbol(symbols, "add", SymbolKind::Function));
}

TEST_CASE("C: function pointer globals are a known, documented gap") {
    // Not one of the shapes tags.scm's global-variable patterns cover.
    // This test documents the current limitation rather than asserting
    // (incorrectly) that it works -- the engine must still not crash on it.
    TreeSitterEngine engine;
    std::string source = "void (*g_callback)(int);\n";

    ParsedDocument doc = engine.parse(Language::C, source);
    REQUIRE(doc.valid());

    auto symbols = engine.outline(doc);
    CHECK_FALSE(hasSymbolNamed(symbols, "g_callback"));
}

TEST_CASE("C++: classes, methods, namespaces, constructors") {
    TreeSitterEngine engine;
    std::string source = R"(
namespace outer {

class Widget {
public:
    Widget();
    void render();

private:
    int m_size;
};

Widget::Widget() : m_size(0) {}

void Widget::render() {
    m_size += 1;
}

} // namespace outer

struct Config {
    int flags;
};
)";

    ParsedDocument doc = engine.parse(Language::Cpp, source);
    REQUIRE(doc.valid());

    auto symbols = engine.outline(doc);

    CHECK(hasSymbol(symbols, "outer", SymbolKind::Namespace));
    CHECK(hasSymbol(symbols, "Widget", SymbolKind::Class));
    CHECK(hasSymbol(symbols, "Config", SymbolKind::Struct));
    CHECK(hasSymbol(symbols, "render", SymbolKind::Method));
    // Out-of-line constructor definition: Widget::Widget().
    CHECK(hasSymbolNamed(symbols, "Widget"));
}

TEST_CASE("Two independently parsed documents don't cross-contaminate") {
    // Stand-in for the "cross-file same-name symbol" fixture from PRD 10;
    // real cross-file resolution is ISymbolIndex's job (M2), but the engine
    // itself must at least keep separate documents' trees fully separate.
    TreeSitterEngine engine;
    ParsedDocument a = engine.parse(Language::C, "int helper(void) { return 1; }\n");
    ParsedDocument b = engine.parse(Language::C, "int helper(void) { return 2; }\n");

    REQUIRE(a.valid());
    REQUIRE(b.valid());
    CHECK(hasSymbol(engine.outline(a), "helper", SymbolKind::Function));
    CHECK(hasSymbol(engine.outline(b), "helper", SymbolKind::Function));
}

TEST_CASE("C: folds cover struct and function bodies") {
    TreeSitterEngine engine;
    std::string source = "struct S {\n    int a;\n    int b;\n};\n\nint f(void) {\n    return 1;\n}\n";

    ParsedDocument doc = engine.parse(Language::C, source);
    REQUIRE(doc.valid());

    auto ranges = engine.folds(doc);
    CHECK(ranges.size() >= 2);
    for (const auto &r : ranges) {
        CHECK(r.endRow > r.startRow);
    }
}

TEST_CASE("C: highlights produce keyword/string/comment/number spans") {
    TreeSitterEngine engine;
    std::string source = "// a comment\nint f(void) {\n    if (1) {\n        return 42;\n    }\n    return 0;\n}\n";

    ParsedDocument doc = engine.parse(Language::C, source);
    REQUIRE(doc.valid());

    auto spans = engine.highlights(doc);
    CHECK(hasHighlight(spans, "comment"));
    CHECK(hasHighlight(spans, "keyword.conditional"));
    CHECK(hasHighlight(spans, "keyword.return"));
    CHECK(hasHighlight(spans, "number"));
    for (const auto &s : spans) {
        CHECK(s.startByte <= s.endByte);
        CHECK(s.endByte <= source.size());
    }
}

TEST_CASE("C: #match? predicate correctly gates SCREAMING_CASE constant detection") {
    // Directly exercises evaluateOnePredicate's #match? branch, both the
    // true and false side, rather than just "the query compiled fine".
    TreeSitterEngine engine;
    std::string source = "int x = MAX_RETRIES;\nint y = lower_case_name;\n";

    ParsedDocument doc = engine.parse(Language::C, source);
    REQUIRE(doc.valid());

    auto spans = engine.highlights(doc);

    bool maxRetriesIsConstant = std::any_of(spans.begin(), spans.end(), [&](const HighlightSpan &s) {
        return s.capture == "constant" && source.substr(s.startByte, s.endByte - s.startByte) == "MAX_RETRIES";
    });
    bool lowerCaseIsConstant = std::any_of(spans.begin(), spans.end(), [&](const HighlightSpan &s) {
        return s.capture == "constant" && source.substr(s.startByte, s.endByte - s.startByte) == "lower_case_name";
    });

    CHECK(maxRetriesIsConstant);
    CHECK_FALSE(lowerCaseIsConstant);
}

TEST_CASE("Cpp: highlights layer c/highlights.scm under cpp/highlights.scm") {
    TreeSitterEngine engine;
    std::string source = "class Foo {\npublic:\n    void bar();\n};\n";

    ParsedDocument doc = engine.parse(Language::Cpp, source);
    REQUIRE(doc.valid());

    auto spans = engine.highlights(doc);
    CHECK(hasHighlight(spans, "keyword.type"));   // "class", from cpp/highlights.scm
    CHECK(hasHighlight(spans, "keyword.modifier")); // "public", from cpp/highlights.scm
    CHECK(hasHighlight(spans, "punctuation.bracket")); // "{"/"}", from c/highlights.scm
}

TEST_CASE("C: incremental edit shifts subsequent symbol positions correctly") {
    TreeSitterEngine engine;
    std::string source = "int foo(void) {\n    return 1;\n}\n";

    ParsedDocument doc = engine.parse(Language::C, source);
    REQUIRE(doc.valid());

    auto before = engine.outline(doc);
    REQUIRE(hasSymbolNamed(before, "foo"));
    auto fooBefore = std::find_if(before.begin(), before.end(), [](const Symbol &s) { return s.name == "foo"; });
    CHECK(fooBefore->startRow == 0);

    // Insert a comment line before `int foo...`, pushing it down one line.
    std::string insertion = "// header comment\n";
    Edit edit;
    edit.startByte = 0;
    edit.oldEndByte = 0;
    edit.newEndByte = static_cast<uint32_t>(insertion.size());
    edit.startRow = 0;
    edit.startColumn = 0;
    edit.oldEndRow = 0;
    edit.oldEndColumn = 0;
    edit.newEndRow = 1;
    edit.newEndColumn = 0;

    std::string newSource = insertion + source;
    engine.applyEdit(doc, edit, newSource);

    auto after = engine.outline(doc);
    REQUIRE(hasSymbolNamed(after, "foo"));
    auto fooAfter = std::find_if(after.begin(), after.end(), [](const Symbol &s) { return s.name == "foo"; });
    CHECK(fooAfter->startRow == 1);
    CHECK(std::string(newSource.substr(fooAfter->startByte, 3)) == "foo");
}
