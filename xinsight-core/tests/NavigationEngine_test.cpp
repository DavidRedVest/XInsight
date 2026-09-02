#include <doctest/doctest.h>

#include "xinsight/core/nav/NavigationEngine.h"

using namespace xinsight::core::nav;

TEST_CASE("NavigationEngine: fresh instance can't go back or forward") {
    NavigationEngine nav;
    CHECK_FALSE(nav.canGoBack());
    CHECK_FALSE(nav.canGoForward());
    CHECK_FALSE(nav.goBack({"a.c", 0}).has_value());
    CHECK_FALSE(nav.goForward({"a.c", 0}).has_value());
}

TEST_CASE("NavigationEngine: push then goBack returns to the pushed location") {
    NavigationEngine nav;
    nav.push({"a.c", 10});

    REQUIRE(nav.canGoBack());
    auto back = nav.goBack({"b.c", 20});
    REQUIRE(back.has_value());
    CHECK(back->absolutePath == "a.c");
    CHECK(back->byteOffset == 10);
}

TEST_CASE("NavigationEngine: goBack then goForward retraces correctly") {
    NavigationEngine nav;
    nav.push({"a.c", 10});

    auto back = nav.goBack({"b.c", 20});
    REQUIRE(back.has_value());
    CHECK(*back == NavigationLocation{"a.c", 10});

    REQUIRE(nav.canGoForward());
    auto forward = nav.goForward({"a.c", 10});
    REQUIRE(forward.has_value());
    CHECK(*forward == NavigationLocation{"b.c", 20});
}

TEST_CASE("NavigationEngine: multiple pushes form a proper stack") {
    NavigationEngine nav;
    nav.push({"a.c", 1});
    nav.push({"b.c", 2});
    nav.push({"c.c", 3});

    auto back1 = nav.goBack({"d.c", 4});
    REQUIRE(back1.has_value());
    CHECK(*back1 == NavigationLocation{"c.c", 3});

    auto back2 = nav.goBack({"d.c", 4});
    REQUIRE(back2.has_value());
    CHECK(*back2 == NavigationLocation{"b.c", 2});

    auto back3 = nav.goBack({"d.c", 4});
    REQUIRE(back3.has_value());
    CHECK(*back3 == NavigationLocation{"a.c", 1});

    CHECK_FALSE(nav.canGoBack());
}

TEST_CASE("NavigationEngine: a fresh push after going back clears the forward stack") {
    NavigationEngine nav;
    nav.push({"a.c", 1});
    nav.goBack({"b.c", 2}); // now at a.c, b.c is forward-reachable

    REQUIRE(nav.canGoForward());
    nav.push({"a.c", 1}); // simulates jumping away from a.c to somewhere new

    CHECK_FALSE(nav.canGoForward());
}
