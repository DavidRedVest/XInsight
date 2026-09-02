#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json_fwd.hpp>

// Data-driven theme structure (PRD 5.7 / P1 #14): every color the GUI paints
// an editor with -- both the fixed editor chrome (background, current line,
// selection, ...) and the tree-sitter capture-name -> color mapping that
// used to be hardcoded in xinsight-qt/src/HighlightStyles.cpp -- lives here
// as plain data, loadable from JSON. xinsight-qt must read colors through
// this structure rather than embedding QColor literals (CLAUDE.md: "every
// core feature... must work with zero configuration", and colors are no
// exception to "route config through core").
namespace xinsight::core::theme {

struct Color {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;

    bool operator==(const Color &) const = default;
};

// Parses a "#RRGGBB" (or "RRGGBB") hex string. Returns nullopt if malformed.
std::optional<Color> parseHexColor(std::string_view hex);
std::string toHexString(const Color &color);

struct EditorColors {
    Color background;
    Color foreground;
    Color currentLine;
    Color selectionBackground;
    Color lineNumberForeground;
    Color lineNumberBackground;
};

// A full theme: fixed editor chrome colors plus a capture-name -> color
// table for tree-sitter highlight captures (e.g. "keyword", "string.escape").
// Lookups against `tokenColors` should go through resolveTokenColor(), which
// implements the dot-prefix fallback (a capture with no dedicated entry
// falls back to its parent, e.g. "string.escape" -> "string").
struct Theme {
    std::string name;
    EditorColors editor;
    std::unordered_map<std::string, Color> tokenColors;
};

// Resolves `captureName` against `theme.tokenColors` by trying it verbatim,
// then progressively shorter dot-separated prefixes; falls back to
// `theme.editor.foreground` if nothing matches at all.
Color resolveTokenColor(const Theme &theme, std::string_view captureName);

// Reads a theme from a JSON file on disk (see docs/theme-schema or any
// builtin theme's toJson() output for the expected shape). Returns nullopt
// if the file can't be read or doesn't parse as a valid theme -- callers
// should treat that as "skip this theme", never as fatal.
std::optional<Theme> loadThemeFromFile(const std::filesystem::path &path);

nlohmann::json themeToJson(const Theme &theme);
// Returns nullopt if `json` is missing required fields or has malformed
// color strings.
std::optional<Theme> themeFromJson(const nlohmann::json &json);

// Built-in themes, always available with no file I/O (PRD P1 #14: v1 ships
// dark/light/eye-care out of the box).
Theme builtinDarkTheme();
Theme builtinLightTheme();
Theme builtinEyeCareTheme();

// Owns the set of available themes (the three built-ins plus any *.json
// theme files found in `userThemesDir`) and the currently-selected one,
// persisting the selection as the minimal "settings" store PRD 5.7 asks
// for (a single JSON file in the user config directory) -- there is
// nothing else to configure yet, so a dedicated generic settings class
// would be speculative; this is the one place that reads/writes it.
class ThemeManager {
public:
    // `userConfigDir` is the app's user-level config directory (e.g.
    // "~/Library/Application Support/XInsight"); custom themes are read
    // from "<userConfigDir>/themes/*.json" and the selected theme name is
    // persisted to "<userConfigDir>/settings.json". Neither needs to exist
    // yet -- ThemeManager creates/reads them lazily.
    explicit ThemeManager(std::filesystem::path userConfigDir);

    // Built-ins first (Dark, Light, Eye-care, in that order), then any
    // custom themes loaded from the user themes directory, sorted by name.
    const std::vector<Theme> &availableThemes() const { return availableThemes_; }

    const Theme &currentTheme() const;

    // Switches the current theme if `name` is found among availableThemes()
    // and persists the choice; no-op (returns false) otherwise.
    bool setCurrentTheme(const std::string &name);

private:
    void reloadAvailableThemes();
    void loadPersistedSelection();
    void persistSelection() const;

    std::filesystem::path userConfigDir_;
    std::vector<Theme> availableThemes_;
    std::size_t currentIndex_ = 0;
};

} // namespace xinsight::core::theme
