#include "xinsight/core/nav/NavigationEngine.h"

namespace xinsight::core::nav {

void NavigationEngine::push(NavigationLocation from) {
    backStack_.push_back(std::move(from));
    forwardStack_.clear();
}

std::optional<NavigationLocation> NavigationEngine::goBack(NavigationLocation current) {
    if (backStack_.empty()) return std::nullopt;

    forwardStack_.push_back(std::move(current));
    NavigationLocation target = backStack_.back();
    backStack_.pop_back();
    return target;
}

std::optional<NavigationLocation> NavigationEngine::goForward(NavigationLocation current) {
    if (forwardStack_.empty()) return std::nullopt;

    backStack_.push_back(std::move(current));
    NavigationLocation target = forwardStack_.back();
    forwardStack_.pop_back();
    return target;
}

} // namespace xinsight::core::nav
