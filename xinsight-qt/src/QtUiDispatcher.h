#pragma once

#include "xinsight/core/IUiDispatcher.h"

// The one Qt-side implementation of IUiDispatcher, per PRD 5.1. Core threads
// call post() to marshal a callback onto the Qt UI thread; nothing else in
// xinsight-qt should reach into core's threading directly.
class QtUiDispatcher final : public xinsight::core::IUiDispatcher {
public:
    void post(std::function<void()> fn) override;
};
