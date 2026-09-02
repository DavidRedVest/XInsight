#include <doctest/doctest.h>

#include "support/ImmediateUiDispatcher.h"

TEST_CASE("ImmediateUiDispatcher runs posted work inline") {
    xinsight::core::testing::ImmediateUiDispatcher dispatcher;
    bool ran = false;
    dispatcher.post([&ran] { ran = true; });
    CHECK(ran);
}
