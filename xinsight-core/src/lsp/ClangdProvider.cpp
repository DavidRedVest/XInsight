#include "xinsight/core/lsp/ClangdProvider.h"

#include <fstream>
#include <iterator>

#include <nlohmann/json.hpp>

#include "xinsight/core/encoding/TextCodec.h"

namespace xinsight::core::lsp {

namespace {

struct DecodedCodepoint {
    uint32_t codepoint;
    size_t byteLength;
};

DecodedCodepoint decodeUtf8At(std::string_view s, size_t pos) {
    unsigned char c = static_cast<unsigned char>(s[pos]);
    if (c < 0x80) return {c, 1};
    if ((c & 0xE0) == 0xC0 && pos + 1 < s.size()) {
        uint32_t cp = static_cast<uint32_t>((c & 0x1F) << 6 | (static_cast<unsigned char>(s[pos + 1]) & 0x3F));
        return {cp, 2};
    }
    if ((c & 0xF0) == 0xE0 && pos + 2 < s.size()) {
        uint32_t cp = static_cast<uint32_t>((c & 0x0F) << 12 | (static_cast<unsigned char>(s[pos + 1]) & 0x3F) << 6 |
                                             (static_cast<unsigned char>(s[pos + 2]) & 0x3F));
        return {cp, 3};
    }
    if ((c & 0xF8) == 0xF0 && pos + 3 < s.size()) {
        uint32_t cp = static_cast<uint32_t>((c & 0x07) << 18 | (static_cast<unsigned char>(s[pos + 1]) & 0x3F) << 12 |
                                             (static_cast<unsigned char>(s[pos + 2]) & 0x3F) << 6 |
                                             (static_cast<unsigned char>(s[pos + 3]) & 0x3F));
        return {cp, 4};
    }
    return {c, 1}; // malformed leading byte -- treat as a single opaque unit
}

std::string toFileUri(const std::string &path) { return "file://" + path; }

std::string fromFileUri(const std::string &uri) {
    const std::string prefix = "file://";
    if (uri.rfind(prefix, 0) == 0) return uri.substr(prefix.size());
    return uri;
}

std::optional<std::vector<uint8_t>> readFileBytes(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Reads just row `row` (0-based) of `file` from disk. Used only to
// convert a *response* position back to a UTF-8 byte column -- the
// request side uses the caller's live buffer text instead (see
// ClangdProvider.h), since the target of a definition/reference lookup
// is typically a *different* file than the one being edited, for which
// there is no live buffer to consult.
std::string readLineFromFile(const std::string &file, uint32_t row) {
    auto raw = readFileBytes(file);
    if (!raw) return {};
    auto decoded = xinsight::core::encoding::decode(*raw);

    size_t start = 0;
    uint32_t currentRow = 0;
    while (currentRow < row) {
        size_t nl = decoded.utf8Text.find('\n', start);
        if (nl == std::string::npos) return {};
        start = nl + 1;
        ++currentRow;
    }
    size_t end = decoded.utf8Text.find('\n', start);
    return decoded.utf8Text.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

} // namespace

uint32_t utf8ByteColumnToUtf16(std::string_view lineUtf8, uint32_t byteColumn) {
    uint32_t utf16Count = 0;
    size_t pos = 0;
    while (pos < lineUtf8.size() && pos < byteColumn) {
        DecodedCodepoint c = decodeUtf8At(lineUtf8, pos);
        utf16Count += (c.codepoint >= 0x10000) ? 2 : 1;
        pos += c.byteLength;
    }
    return utf16Count;
}

uint32_t utf16ColumnToUtf8ByteColumn(std::string_view lineUtf8, uint32_t utf16Column) {
    uint32_t utf16Count = 0;
    size_t pos = 0;
    while (pos < lineUtf8.size() && utf16Count < utf16Column) {
        DecodedCodepoint c = decodeUtf8At(lineUtf8, pos);
        utf16Count += (c.codepoint >= 0x10000) ? 2 : 1;
        pos += c.byteLength;
    }
    return static_cast<uint32_t>(pos);
}

ClangdProvider::ClangdProvider(LspClient &client) : client_(client) {
    client_.setOnNotification(
        [this](const std::string &method, const nlohmann::json &params) { handleNotification(method, params); });
}

void ClangdProvider::handleNotification(const std::string &method, const nlohmann::json &params) {
    if (method != "textDocument/publishDiagnostics") return;
    if (!params.contains("uri")) return;

    std::string file = fromFileUri(params["uri"].get<std::string>());
    std::lock_guard<std::mutex> lock(readyMutex_);
    readyFiles_.insert(file);
}

void ClangdProvider::didOpen(const std::string &file, const std::string &languageId, std::string utf8Text) {
    int version;
    {
        std::lock_guard<std::mutex> lock(versionMutex_);
        version = documentVersions_[file] = 1;
    }
    {
        std::lock_guard<std::mutex> lock(readyMutex_);
        readyFiles_.erase(file); // not ready again until a fresh diagnostics publish confirms it
    }

    nlohmann::json params = {
        {"textDocument",
         {{"uri", toFileUri(file)}, {"languageId", languageId}, {"version", version}, {"text", std::move(utf8Text)}}},
    };
    client_.sendNotification("textDocument/didOpen", std::move(params));
}

void ClangdProvider::didChange(const std::string &file, std::string utf8Text) {
    int version;
    {
        std::lock_guard<std::mutex> lock(versionMutex_);
        version = ++documentVersions_[file];
    }

    nlohmann::json params = {
        {"textDocument", {{"uri", toFileUri(file)}, {"version", version}}},
        {"contentChanges", nlohmann::json::array({nlohmann::json{{"text", std::move(utf8Text)}}})},
    };
    client_.sendNotification("textDocument/didChange", std::move(params));
}

void ClangdProvider::didSave(const std::string &file) {
    client_.sendNotification("textDocument/didSave", {{"textDocument", {{"uri", toFileUri(file)}}}});
}

bool ClangdProvider::isFileReady(const std::string &file) const {
    std::lock_guard<std::mutex> lock(readyMutex_);
    return readyFiles_.count(file) > 0;
}

std::vector<PreciseLocation> ClangdProvider::parseLocations(const nlohmann::json &result) const {
    std::vector<PreciseLocation> locations;

    auto addOne = [&](const nlohmann::json &item) {
        std::string uri;
        nlohmann::json range;
        if (item.contains("uri") && item.contains("range")) {
            uri = item["uri"].get<std::string>();
            range = item["range"];
        } else if (item.contains("targetUri")) {
            // A LocationLink (only sent if we advertised linkSupport,
            // which we don't -- handled anyway for robustness).
            uri = item["targetUri"].get<std::string>();
            range = item.contains("targetSelectionRange") ? item["targetSelectionRange"]
                                                            : item.value("targetRange", nlohmann::json::object());
        } else {
            return;
        }
        if (!range.contains("start")) return;

        std::string file = fromFileUri(uri);
        uint32_t row = range["start"].value("line", 0u);
        uint32_t utf16Column = range["start"].value("character", 0u);
        uint32_t byteColumn = utf16ColumnToUtf8ByteColumn(readLineFromFile(file, row), utf16Column);

        locations.push_back(PreciseLocation{std::move(file), row, byteColumn});
    };

    if (result.is_array()) {
        for (const auto &item : result) addOne(item);
    } else if (result.is_object()) {
        addOne(result);
    }
    return locations;
}

void ClangdProvider::findDefinition(const std::string &file, uint32_t row, std::string_view lineTextUtf8,
                                     uint32_t byteColumn, LocationsCallback callback) {
    nlohmann::json params = {
        {"textDocument", {{"uri", toFileUri(file)}}},
        {"position", {{"line", row}, {"character", utf8ByteColumnToUtf16(lineTextUtf8, byteColumn)}}},
    };
    client_.sendRequest("textDocument/definition", std::move(params),
                         [this, callback](std::optional<nlohmann::json> result, std::optional<nlohmann::json> error) {
                             if (error || !result) {
                                 callback({});
                                 return;
                             }
                             callback(parseLocations(*result));
                         });
}

void ClangdProvider::findReferences(const std::string &file, uint32_t row, std::string_view lineTextUtf8,
                                     uint32_t byteColumn, LocationsCallback callback) {
    nlohmann::json params = {
        {"textDocument", {{"uri", toFileUri(file)}}},
        {"position", {{"line", row}, {"character", utf8ByteColumnToUtf16(lineTextUtf8, byteColumn)}}},
        {"context", {{"includeDeclaration", true}}},
    };
    client_.sendRequest("textDocument/references", std::move(params),
                         [this, callback](std::optional<nlohmann::json> result, std::optional<nlohmann::json> error) {
                             if (error || !result) {
                                 callback({});
                                 return;
                             }
                             callback(parseLocations(*result));
                         });
}

void ClangdProvider::searchWorkspaceSymbols(std::string query, SymbolsCallback callback) {
    client_.sendRequest(
        "workspace/symbol", {{"query", std::move(query)}},
        [this, callback](std::optional<nlohmann::json> result, std::optional<nlohmann::json> error) {
            std::vector<WorkspaceSymbolMatch> matches;
            if (!error && result && result->is_array()) {
                for (const auto &item : *result) {
                    if (!item.contains("name") || !item.contains("location")) continue;
                    const nlohmann::json &loc = item["location"];
                    if (!loc.contains("uri") || !loc.contains("range") || !loc["range"].contains("start")) continue;

                    std::string file = fromFileUri(loc["uri"].get<std::string>());
                    uint32_t row = loc["range"]["start"].value("line", 0u);
                    uint32_t utf16Column = loc["range"]["start"].value("character", 0u);
                    uint32_t byteColumn = utf16ColumnToUtf8ByteColumn(readLineFromFile(file, row), utf16Column);

                    matches.push_back(WorkspaceSymbolMatch{item["name"].get<std::string>(), std::move(file), row, byteColumn});
                }
            }
            callback(std::move(matches));
        });
}

} // namespace xinsight::core::lsp
