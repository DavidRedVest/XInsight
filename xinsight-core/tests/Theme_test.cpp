#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <string>

#include "xinsight/core/theme/Theme.h"

using namespace xinsight::core::theme;
namespace fs = std::filesystem;

namespace {

class TempConfigDir {
public:
    TempConfigDir() {
        root_ = fs::temp_directory_path() /
                fs::path("xinsight-theme-test-" +
                         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root_);
    }
    ~TempConfigDir() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    void writeThemeFile(const std::string &relativeName, const std::string &content) {
        fs::path dir = root_ / "themes";
        fs::create_directories(dir);
        std::ofstream out(dir / relativeName, std::ios::binary);
        out << content;
    }

    const fs::path &root() const { return root_; }

private:
    fs::path root_;
};

} // namespace

TEST_CASE("parseHexColor: accepts with and without leading #") {
    auto a = parseHexColor("#1E90FF");
    REQUIRE(a.has_value());
    CHECK(a->r == 0x1E);
    CHECK(a->g == 0x90);
    CHECK(a->b == 0xFF);

    auto b = parseHexColor("1E90FF");
    REQUIRE(b.has_value());
    CHECK(*b == *a);
}

TEST_CASE("parseHexColor: rejects malformed input") {
    CHECK_FALSE(parseHexColor("#1E90F").has_value());   // too short
    CHECK_FALSE(parseHexColor("#1E90FFAA").has_value()); // too long
    CHECK_FALSE(parseHexColor("#GG90FF").has_value());   // invalid hex digit
    CHECK_FALSE(parseHexColor("").has_value());
}

TEST_CASE("toHexString: round-trips through parseHexColor") {
    Color c{0x0A, 0xBC, 0xDE};
    std::string hex = toHexString(c);
    CHECK(hex == "#0ABCDE");
    auto parsed = parseHexColor(hex);
    REQUIRE(parsed.has_value());
    CHECK(*parsed == c);
}

TEST_CASE("resolveTokenColor: exact match wins") {
    Theme theme = builtinDarkTheme();
    Color expected = theme.tokenColors.at("keyword");
    CHECK(resolveTokenColor(theme, "keyword") == expected);
}

TEST_CASE("resolveTokenColor: falls back through dot-separated prefixes") {
    Theme theme = builtinDarkTheme();
    // "string.escape" has no dedicated entry; must fall back to "string".
    CHECK(resolveTokenColor(theme, "string.escape") == theme.tokenColors.at("string"));
    CHECK(resolveTokenColor(theme, "keyword.conditional.ternary") == theme.tokenColors.at("keyword"));
}

TEST_CASE("resolveTokenColor: falls back to editor foreground when nothing matches") {
    Theme theme = builtinDarkTheme();
    CHECK(resolveTokenColor(theme, "totally.unknown.capture") == theme.editor.foreground);
}

TEST_CASE("themeToJson / themeFromJson: round-trips a built-in theme") {
    Theme original = builtinDarkTheme();
    auto json = themeToJson(original);
    auto restored = themeFromJson(json);
    REQUIRE(restored.has_value());
    CHECK(restored->name == original.name);
    CHECK(restored->editor.background == original.editor.background);
    CHECK(restored->editor.foreground == original.editor.foreground);
    CHECK(restored->tokenColors == original.tokenColors);
}

TEST_CASE("themeFromJson: rejects missing required fields") {
    nlohmann::json j = {{"name", "Broken"}}; // no "editor" object
    CHECK_FALSE(themeFromJson(j).has_value());

    nlohmann::json j2 = {{"name", "Broken2"},
                          {"editor", {{"background", "#000000"}}}}; // missing other editor fields
    CHECK_FALSE(themeFromJson(j2).has_value());
}

TEST_CASE("themeFromJson: rejects malformed color strings") {
    nlohmann::json j = {{"name", "Broken3"},
                         {"editor",
                          {{"background", "not-a-color"},
                           {"foreground", "#D4D4D4"},
                           {"currentLine", "#2A2A2A"},
                           {"selectionBackground", "#264F78"},
                           {"lineNumberForeground", "#858585"},
                           {"lineNumberBackground", "#252525"}}}};
    CHECK_FALSE(themeFromJson(j).has_value());
}

TEST_CASE("loadThemeFromFile: returns nullopt for a nonexistent file") {
    CHECK_FALSE(loadThemeFromFile("/nonexistent/path/theme.json").has_value());
}

TEST_CASE("loadThemeFromFile: returns nullopt for invalid JSON") {
    TempConfigDir dir;
    dir.writeThemeFile("bad.json", "{not valid json");
    CHECK_FALSE(loadThemeFromFile(dir.root() / "themes" / "bad.json").has_value());
}

TEST_CASE("loadThemeFromFile: loads a well-formed custom theme") {
    TempConfigDir dir;
    dir.writeThemeFile("custom.json", R"({
        "name": "Custom",
        "editor": {
            "background": "#000000",
            "foreground": "#FFFFFF",
            "currentLine": "#111111",
            "selectionBackground": "#222222",
            "lineNumberForeground": "#333333",
            "lineNumberBackground": "#444444"
        },
        "tokens": {
            "keyword": "#FF0000"
        }
    })");

    auto theme = loadThemeFromFile(dir.root() / "themes" / "custom.json");
    REQUIRE(theme.has_value());
    CHECK(theme->name == "Custom");
    CHECK(theme->editor.background == Color{0x00, 0x00, 0x00});
    CHECK(theme->tokenColors.at("keyword") == Color{0xFF, 0x00, 0x00});
}

TEST_CASE("builtin themes: dark, light and eye-care all have distinct names and full palettes") {
    Theme dark = builtinDarkTheme();
    Theme light = builtinLightTheme();
    Theme eyeCare = builtinEyeCareTheme();

    CHECK(dark.name != light.name);
    CHECK(dark.name != eyeCare.name);
    CHECK(light.name != eyeCare.name);

    for (const Theme *theme : {&dark, &light, &eyeCare}) {
        CHECK_FALSE(theme->tokenColors.empty());
        CHECK(theme->tokenColors.count("keyword") == 1);
        CHECK(theme->tokenColors.count("string") == 1);
        CHECK(theme->tokenColors.count("comment") == 1);
    }
}

TEST_CASE("ThemeManager: available themes include the three built-ins by default") {
    TempConfigDir dir;
    ThemeManager manager(dir.root());

    const auto &themes = manager.availableThemes();
    REQUIRE(themes.size() == 3);
    CHECK(themes[0].name == "Dark");
    CHECK(themes[1].name == "Light");
    CHECK(themes[2].name == "Eye-care");
    CHECK(manager.currentTheme().name == "Dark"); // default selection
}

TEST_CASE("ThemeManager: setCurrentTheme switches and persists the selection") {
    TempConfigDir dir;
    {
        ThemeManager manager(dir.root());
        REQUIRE(manager.setCurrentTheme("Light"));
        CHECK(manager.currentTheme().name == "Light");
    }
    {
        // A fresh ThemeManager over the same config dir should restore the choice.
        ThemeManager manager(dir.root());
        CHECK(manager.currentTheme().name == "Light");
    }
}

TEST_CASE("ThemeManager: setCurrentTheme with an unknown name is a no-op") {
    TempConfigDir dir;
    ThemeManager manager(dir.root());
    CHECK_FALSE(manager.setCurrentTheme("Does Not Exist"));
    CHECK(manager.currentTheme().name == "Dark");
}

TEST_CASE("ThemeManager: picks up custom theme files from the user themes directory") {
    TempConfigDir dir;
    dir.writeThemeFile("mytheme.json", R"({
        "name": "MyCustom",
        "editor": {
            "background": "#010101",
            "foreground": "#FEFEFE",
            "currentLine": "#020202",
            "selectionBackground": "#030303",
            "lineNumberForeground": "#040404",
            "lineNumberBackground": "#050505"
        },
        "tokens": {}
    })");

    ThemeManager manager(dir.root());
    const auto &themes = manager.availableThemes();
    REQUIRE(themes.size() == 4);
    CHECK(themes[3].name == "MyCustom");
    REQUIRE(manager.setCurrentTheme("MyCustom"));
    CHECK(manager.currentTheme().editor.background == Color{0x01, 0x01, 0x01});
}

TEST_CASE("ThemeManager: ignores malformed theme files in the user themes directory") {
    TempConfigDir dir;
    dir.writeThemeFile("broken.json", "{not valid json");

    ThemeManager manager(dir.root());
    CHECK(manager.availableThemes().size() == 3); // broken file silently skipped
}
