#include "xinsight/core/theme/Theme.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>

namespace xinsight::core::theme {

namespace {

int hexNibble(char c, bool &ok) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    ok = false;
    return 0;
}

std::string jsonColor(const Color &c) { return toHexString(c); }

std::optional<Color> jsonToColor(const nlohmann::json &j) {
    if (!j.is_string()) return std::nullopt;
    return parseHexColor(j.get<std::string>());
}

} // namespace

std::optional<Color> parseHexColor(std::string_view hex) {
    if (!hex.empty() && hex.front() == '#') hex.remove_prefix(1);
    if (hex.size() != 6) return std::nullopt;

    bool ok = true;
    auto byteAt = [&](size_t i) {
        int hi = hexNibble(hex[i], ok);
        int lo = hexNibble(hex[i + 1], ok);
        return static_cast<uint8_t>((hi << 4) | lo);
    };

    Color color;
    color.r = byteAt(0);
    color.g = byteAt(2);
    color.b = byteAt(4);
    if (!ok) return std::nullopt;
    return color;
}

std::string toHexString(const Color &color) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", color.r, color.g, color.b);
    return std::string(buf);
}

Color resolveTokenColor(const Theme &theme, std::string_view captureName) {
    std::string candidate(captureName);
    while (!candidate.empty()) {
        auto it = theme.tokenColors.find(candidate);
        if (it != theme.tokenColors.end()) return it->second;

        auto lastDot = candidate.find_last_of('.');
        if (lastDot == std::string::npos) break;
        candidate.resize(lastDot);
    }
    return theme.editor.foreground;
}

nlohmann::json themeToJson(const Theme &theme) {
    nlohmann::json j;
    j["name"] = theme.name;
    j["editor"] = {
        {"background", jsonColor(theme.editor.background)},
        {"foreground", jsonColor(theme.editor.foreground)},
        {"currentLine", jsonColor(theme.editor.currentLine)},
        {"selectionBackground", jsonColor(theme.editor.selectionBackground)},
        {"lineNumberForeground", jsonColor(theme.editor.lineNumberForeground)},
        {"lineNumberBackground", jsonColor(theme.editor.lineNumberBackground)},
    };
    nlohmann::json tokens = nlohmann::json::object();
    for (const auto &[capture, color] : theme.tokenColors) tokens[capture] = jsonColor(color);
    j["tokens"] = tokens;
    return j;
}

std::optional<Theme> themeFromJson(const nlohmann::json &json) {
    if (!json.is_object()) return std::nullopt;

    auto nameIt = json.find("name");
    if (nameIt == json.end() || !nameIt->is_string()) return std::nullopt;

    auto editorIt = json.find("editor");
    if (editorIt == json.end() || !editorIt->is_object()) return std::nullopt;

    Theme theme;
    theme.name = nameIt->get<std::string>();

    auto readRequired = [&](const char *key, Color &out) {
        auto it = editorIt->find(key);
        if (it == editorIt->end()) return false;
        auto color = jsonToColor(*it);
        if (!color) return false;
        out = *color;
        return true;
    };

    bool ok = true;
    ok &= readRequired("background", theme.editor.background);
    ok &= readRequired("foreground", theme.editor.foreground);
    ok &= readRequired("currentLine", theme.editor.currentLine);
    ok &= readRequired("selectionBackground", theme.editor.selectionBackground);
    ok &= readRequired("lineNumberForeground", theme.editor.lineNumberForeground);
    ok &= readRequired("lineNumberBackground", theme.editor.lineNumberBackground);
    if (!ok) return std::nullopt;

    auto tokensIt = json.find("tokens");
    if (tokensIt != json.end() && tokensIt->is_object()) {
        for (auto it = tokensIt->begin(); it != tokensIt->end(); ++it) {
            auto color = jsonToColor(it.value());
            if (color) theme.tokenColors.emplace(it.key(), *color);
        }
    }

    return theme;
}

std::optional<Theme> loadThemeFromFile(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;

    nlohmann::json j;
    try {
        in >> j;
    } catch (const nlohmann::json::parse_error &) {
        return std::nullopt;
    }
    return themeFromJson(j);
}

namespace {

std::unordered_map<std::string, Color> makeTokenColors(
    std::initializer_list<std::pair<const char *, Color>> entries) {
    std::unordered_map<std::string, Color> result;
    for (const auto &[capture, color] : entries) result.emplace(capture, color);
    return result;
}

} // namespace

Theme builtinDarkTheme() {
    Theme theme;
    theme.name = "Dark";
    theme.editor = EditorColors{
        .background = {0x1E, 0x1E, 0x1E},
        .foreground = {0xD4, 0xD4, 0xD4},
        .currentLine = {0x2A, 0x2A, 0x2A},
        .selectionBackground = {0x26, 0x4F, 0x78},
        .lineNumberForeground = {0x85, 0x85, 0x85},
        .lineNumberBackground = {0x25, 0x25, 0x25},
    };
    theme.tokenColors = makeTokenColors({
        {"comment", {0x6A, 0x99, 0x55}},
        {"string", {0xCE, 0x91, 0x78}},
        {"character", {0xCE, 0x91, 0x78}},
        {"number", {0xB5, 0xCE, 0xA8}},
        {"boolean", {0x56, 0x9C, 0xD6}},
        {"keyword", {0x56, 0x9C, 0xD6}},
        {"attribute", {0x56, 0x9C, 0xD6}},
        {"type", {0x4E, 0xC9, 0xB0}},
        {"module", {0x4E, 0xC9, 0xB0}},
        {"constructor", {0x4E, 0xC9, 0xB0}},
        {"function", {0xDC, 0xDC, 0xAA}},
        {"variable", {0x9C, 0xDC, 0xFE}},
        {"property", {0x9C, 0xDC, 0xFE}},
        {"label", {0x9C, 0xDC, 0xFE}},
        {"constant", {0x4F, 0xC1, 0xFF}},
        {"punctuation", {0xD4, 0xD4, 0xD4}},
        {"operator", {0xD4, 0xD4, 0xD4}},
    });
    return theme;
}

Theme builtinLightTheme() {
    Theme theme;
    theme.name = "Light";
    theme.editor = EditorColors{
        .background = {0xFF, 0xFF, 0xFF},
        .foreground = {0x00, 0x00, 0x00},
        .currentLine = {0xF5, 0xF5, 0xF5},
        .selectionBackground = {0xAD, 0xD6, 0xFF},
        .lineNumberForeground = {0x23, 0x78, 0x93},
        .lineNumberBackground = {0xF5, 0xF5, 0xF5},
    };
    theme.tokenColors = makeTokenColors({
        {"comment", {0x00, 0x80, 0x00}},
        {"string", {0xA3, 0x15, 0x15}},
        {"character", {0xA3, 0x15, 0x15}},
        {"number", {0x09, 0x86, 0x58}},
        {"boolean", {0x00, 0x00, 0xFF}},
        {"keyword", {0x00, 0x00, 0xFF}},
        {"attribute", {0x00, 0x00, 0xFF}},
        {"type", {0x26, 0x7F, 0x99}},
        {"module", {0x26, 0x7F, 0x99}},
        {"constructor", {0x26, 0x7F, 0x99}},
        {"function", {0x79, 0x5E, 0x26}},
        {"variable", {0x00, 0x10, 0x80}},
        {"property", {0x00, 0x10, 0x80}},
        {"label", {0x00, 0x10, 0x80}},
        {"constant", {0x00, 0x70, 0xC1}},
        {"punctuation", {0x00, 0x00, 0x00}},
        {"operator", {0x00, 0x00, 0x00}},
    });
    return theme;
}

Theme builtinEyeCareTheme() {
    Theme theme;
    theme.name = "Eye-care";
    theme.editor = EditorColors{
        .background = {0xC7, 0xED, 0xCC},
        .foreground = {0x2C, 0x3E, 0x32},
        .currentLine = {0xB8, 0xDE, 0xBE},
        .selectionBackground = {0xA0, 0xD0, 0xAA},
        .lineNumberForeground = {0x4F, 0x6B, 0x57},
        .lineNumberBackground = {0xBF, 0xE5, 0xC5},
    };
    // Colors below are all >=4.5:1 luminance-contrast against the
    // background (#C7EDCC) -- the original set skewed too close in both
    // hue and luminance to the light-green background (some as low as
    // ~2.8:1), making code hard to distinguish from the page. Darkened and
    // desaturated relative to the Dark/Light themes' accents to keep the
    // "eye-care" low-glare feel while staying legible.
    theme.tokenColors = makeTokenColors({
        {"comment", {0x4F, 0x6B, 0x57}},
        {"string", {0x8B, 0x5A, 0x2B}},
        {"character", {0x8B, 0x5A, 0x2B}},
        {"number", {0x3D, 0x6B, 0x4F}},
        {"boolean", {0x1F, 0x4E, 0x79}},
        {"keyword", {0x1F, 0x4E, 0x79}},
        {"attribute", {0x1F, 0x4E, 0x79}},
        {"type", {0x2F, 0x6B, 0x5E}},
        {"module", {0x2F, 0x6B, 0x5E}},
        {"constructor", {0x2F, 0x6B, 0x5E}},
        {"function", {0x6B, 0x5A, 0x1F}},
        {"variable", {0x1F, 0x3A, 0x52}},
        {"property", {0x1F, 0x3A, 0x52}},
        {"label", {0x1F, 0x3A, 0x52}},
        {"constant", {0x2A, 0x5A, 0x78}},
        {"punctuation", {0x2C, 0x3E, 0x32}},
        {"operator", {0x2C, 0x3E, 0x32}},
    });
    return theme;
}

ThemeManager::ThemeManager(std::filesystem::path userConfigDir) : userConfigDir_(std::move(userConfigDir)) {
    reloadAvailableThemes();
    loadPersistedSelection();
}

void ThemeManager::reloadAvailableThemes() {
    availableThemes_.clear();
    availableThemes_.push_back(builtinDarkTheme());
    availableThemes_.push_back(builtinLightTheme());
    availableThemes_.push_back(builtinEyeCareTheme());

    std::vector<Theme> custom;
    std::error_code ec;
    std::filesystem::path themesDir = userConfigDir_ / "themes";
    if (std::filesystem::is_directory(themesDir, ec)) {
        for (const auto &entry : std::filesystem::directory_iterator(themesDir, ec)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".json") continue;
            if (auto theme = loadThemeFromFile(entry.path())) custom.push_back(std::move(*theme));
        }
    }
    std::sort(custom.begin(), custom.end(), [](const Theme &a, const Theme &b) { return a.name < b.name; });
    for (auto &theme : custom) availableThemes_.push_back(std::move(theme));
}

void ThemeManager::loadPersistedSelection() {
    std::ifstream in(userConfigDir_ / "settings.json", std::ios::binary);
    if (!in) return;

    nlohmann::json j;
    try {
        in >> j;
    } catch (const nlohmann::json::parse_error &) {
        return;
    }
    auto it = j.find("theme");
    if (it != j.end() && it->is_string()) setCurrentTheme(it->get<std::string>());
}

void ThemeManager::persistSelection() const {
    std::error_code ec;
    std::filesystem::create_directories(userConfigDir_, ec);
    if (ec) return;

    std::ofstream out(userConfigDir_ / "settings.json", std::ios::binary | std::ios::trunc);
    if (!out) return;
    nlohmann::json j;
    j["theme"] = currentTheme().name;
    out << j.dump(2);
}

const Theme &ThemeManager::currentTheme() const { return availableThemes_[currentIndex_]; }

bool ThemeManager::setCurrentTheme(const std::string &name) {
    for (std::size_t i = 0; i < availableThemes_.size(); ++i) {
        if (availableThemes_[i].name == name) {
            currentIndex_ = i;
            persistSelection();
            return true;
        }
    }
    return false;
}

} // namespace xinsight::core::theme
