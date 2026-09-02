#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace xinsight::core::nav {

// A file + byte-offset location, the unit NavigationEngine's stacks are
// made of. Deliberately doesn't carry line/column -- callers (GUI) resolve
// that from the editor at jump time.
struct NavigationLocation {
    std::string absolutePath;
    uint32_t byteOffset = 0;

    bool operator==(const NavigationLocation &other) const {
        return absolutePath == other.absolutePath && byteOffset == other.byteOffset;
    }
};

// The single exit point for jump/back-forward stack state (PRD 5.1/5.4).
// Pure data structure, no I/O -- GUI performs the actual file-open/cursor
// move; this only tracks where "back"/"forward" (Cmd+[ / Cmd+]) should go.
//
// Standard browser-style semantics: push() records where you're jumping
// FROM (making it reachable via back) and discards the forward history,
// since a fresh jump abandons whatever "future" you could have redone.
class NavigationEngine {
public:
    void push(NavigationLocation from);

    bool canGoBack() const { return !backStack_.empty(); }
    bool canGoForward() const { return !forwardStack_.empty(); }

    // `current` is where the caller is right now (pushed onto the opposite
    // stack so the trip can be retraced). Returns nullopt if there's
    // nowhere to go.
    std::optional<NavigationLocation> goBack(NavigationLocation current);
    std::optional<NavigationLocation> goForward(NavigationLocation current);

private:
    std::vector<NavigationLocation> backStack_;
    std::vector<NavigationLocation> forwardStack_;
};

} // namespace xinsight::core::nav
