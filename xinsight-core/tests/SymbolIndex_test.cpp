#include <doctest/doctest.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>

#include "xinsight/core/intel/SymbolIndex.h"

using namespace xinsight::core::intel;

namespace {

Symbol makeFunctionSymbol(std::string name) {
    Symbol s;
    s.name = std::move(name);
    s.kind = SymbolKind::Function;
    return s;
}

// Backend-agnostic interface tests (PRD 5.3.1): every assertion here goes
// through ISymbolIndex only, never InMemorySymbolIndex specifics. When a
// future SqliteSymbolIndex lands, add one line calling this same function
// with a factory for it -- if that doesn't compile and pass unchanged, the
// interface wasn't abstracted cleanly (PRD's own stated acceptance test).
void runInterfaceTests(const std::function<std::unique_ptr<ISymbolIndex>()> &makeIndex) {
    SUBCASE("fresh index has no definitions or references") {
        auto index = makeIndex();
        CHECK(index->findDefinitions("foo").empty());
        CHECK(index->findReferences("foo").empty());
        CHECK(index->searchSymbols("foo", 10).empty());
    }

    SUBCASE("updateFile populates definitions and references, queryable by exact name") {
        auto index = makeIndex();

        Symbol def = makeFunctionSymbol("compute_widget");
        def.startByte = 10;
        def.startRow = 1;
        def.startColumn = 4;

        IdentifierOccurrence ref;
        ref.name = "compute_widget";
        ref.startByte = 100;
        ref.startRow = 5;
        ref.startColumn = 2;

        index->updateFile("/proj/widget.c", {def}, {ref});

        auto defs = index->findDefinitions("compute_widget");
        REQUIRE(defs.size() == 1);
        CHECK(defs[0].file == "/proj/widget.c");
        CHECK(defs[0].kind == SymbolKind::Function);
        CHECK(defs[0].startByte == 10);
        CHECK(defs[0].startRow == 1);

        auto refs = index->findReferences("compute_widget");
        REQUIRE(refs.size() == 1);
        CHECK(refs[0].file == "/proj/widget.c");
        CHECK(refs[0].startByte == 100);
    }

    SUBCASE("same name defined in multiple files returns all candidates") {
        auto index = makeIndex();
        index->updateFile("/proj/a.c", {makeFunctionSymbol("init")}, {});
        index->updateFile("/proj/b.c", {makeFunctionSymbol("init")}, {});

        CHECK(index->findDefinitions("init").size() == 2);
    }

    SUBCASE("updateFile on an already-indexed file replaces its old entries") {
        auto index = makeIndex();
        index->updateFile("/proj/a.c", {makeFunctionSymbol("old_name")}, {});
        CHECK(index->findDefinitions("old_name").size() == 1);

        index->updateFile("/proj/a.c", {makeFunctionSymbol("new_name")}, {});

        CHECK(index->findDefinitions("old_name").empty()); // stale entry gone
        CHECK(index->findDefinitions("new_name").size() == 1);
    }

    SUBCASE("updateFile on one file doesn't disturb another file's same-named entries") {
        auto index = makeIndex();
        index->updateFile("/proj/a.c", {makeFunctionSymbol("shared")}, {});
        index->updateFile("/proj/b.c", {makeFunctionSymbol("shared")}, {});
        REQUIRE(index->findDefinitions("shared").size() == 2);

        index->updateFile("/proj/a.c", {makeFunctionSymbol("renamed")}, {});

        auto defs = index->findDefinitions("shared");
        REQUIRE(defs.size() == 1);
        CHECK(defs[0].file == "/proj/b.c");
    }

    SUBCASE("removeFile drops exactly that file's definitions and references") {
        auto index = makeIndex();
        index->updateFile("/proj/a.c", {makeFunctionSymbol("foo")}, {});
        index->updateFile("/proj/b.c", {makeFunctionSymbol("bar")}, {});

        index->removeFile("/proj/a.c");

        CHECK(index->findDefinitions("foo").empty());
        CHECK(index->findDefinitions("bar").size() == 1);
    }

    SUBCASE("removeFile on an unindexed file is a safe no-op") {
        auto index = makeIndex();
        index->removeFile("/proj/never-indexed.c");
        CHECK(index->findDefinitions("anything").empty());
    }

    SUBCASE("clear drops every file's entries") {
        auto index = makeIndex();
        index->updateFile("/proj/a.c", {makeFunctionSymbol("foo")}, {});
        REQUIRE(index->findDefinitions("foo").size() == 1);

        index->clear();
        CHECK(index->findDefinitions("foo").empty());
    }

    SUBCASE("searchSymbols matches substrings case-insensitively") {
        auto index = makeIndex();
        index->updateFile("/proj/a.c",
                           {makeFunctionSymbol("ComputeWidget"), makeFunctionSymbol("compute_helper"),
                            makeFunctionSymbol("unrelated")},
                           {});

        auto results = index->searchSymbols("COMPUTE", 10);
        REQUIRE(results.size() == 2);
        CHECK(std::any_of(results.begin(), results.end(), [](const auto &r) { return r.name == "ComputeWidget"; }));
        CHECK(std::any_of(results.begin(), results.end(), [](const auto &r) { return r.name == "compute_helper"; }));
    }

    SUBCASE("searchSymbols results are sorted by name for deterministic output") {
        auto index = makeIndex();
        index->updateFile(
            "/proj/a.c", {makeFunctionSymbol("zeta_func"), makeFunctionSymbol("alpha_func"), makeFunctionSymbol("mid_func")}, {});

        auto results = index->searchSymbols("func", 10);
        REQUIRE(results.size() == 3);
        CHECK(results[0].name == "alpha_func");
        CHECK(results[1].name == "mid_func");
        CHECK(results[2].name == "zeta_func");
    }

    SUBCASE("searchSymbols respects maxResults") {
        auto index = makeIndex();
        std::vector<Symbol> symbols;
        for (int i = 0; i < 5; ++i) symbols.push_back(makeFunctionSymbol("item_" + std::to_string(i)));
        index->updateFile("/proj/a.c", symbols, {});

        CHECK(index->searchSymbols("item", 3).size() == 3);
    }
}

} // namespace

TEST_CASE("InMemorySymbolIndex: passes the backend-agnostic ISymbolIndex interface suite") {
    runInterfaceTests([] { return std::make_unique<InMemorySymbolIndex>(); });
}
