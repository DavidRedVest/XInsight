#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "xinsight/core/Version.h"

TEST_CASE("version() returns the compiled-in version string") {
    CHECK(xinsight::core::version() == xinsight::core::kVersion);
    CHECK_FALSE(xinsight::core::version().empty());
}
