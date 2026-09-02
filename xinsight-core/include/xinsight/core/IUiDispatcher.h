#pragma once

#include <functional>

namespace xinsight::core {

// The single crossing point from core-owned threads (I/O, background
// indexing, ...) back to the UI thread. Core code must never block waiting
// on a result in the UI thread, and must never assume anything about which
// thread it is running on except through this interface. See PRD 5.1.
struct IUiDispatcher {
    virtual void post(std::function<void()> fn) = 0;
    virtual ~IUiDispatcher() = default;
};

} // namespace xinsight::core
