#pragma once

#include "xinsight/core/IUiDispatcher.h"

namespace xinsight::core::testing {

// Synchronous IUiDispatcher for headless tests: runs posted work inline
// instead of marshaling to a real UI event loop, which doesn't exist here.
struct ImmediateUiDispatcher final : IUiDispatcher {
    void post(std::function<void()> fn) override {
        fn();
    }
};

} // namespace xinsight::core::testing
