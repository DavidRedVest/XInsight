#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "xinsight/core/lsp/LspClient.h"

namespace xinsight::core::lsp {

// Converts a UTF-8 byte offset within `lineUtf8` to an LSP UTF-16
// code-unit offset -- LSP positions are always UTF-16 code units per
// spec, never bytes or codepoints, which matters for non-ASCII content
// (this project's own target of Chinese-commented embedded C, PRD 2.3).
// Codepoints outside the BMP contribute 2 UTF-16 units each (surrogate
// pairs). Malformed UTF-8 bytes are treated as one opaque unit each --
// best-effort, never throws.
uint32_t utf8ByteColumnToUtf16(std::string_view lineUtf8, uint32_t byteColumn);

// Inverse of utf8ByteColumnToUtf16.
uint32_t utf16ColumnToUtf8ByteColumn(std::string_view lineUtf8, uint32_t utf16Column);

// A precise location clangd resolved, translated back into this
// codebase's usual 0-based-row / UTF-8-byte-column convention (matching
// xinsight::core::intel::SymbolLocation/ReferenceLocation's shape, so
// CodeIntelligence can present both tree-sitter and clangd results
// uniformly to callers).
struct PreciseLocation {
    std::string file;
    uint32_t startRow = 0;
    uint32_t startColumn = 0;
};

struct WorkspaceSymbolMatch {
    std::string name;
    std::string file;
    uint32_t startRow = 0;
    uint32_t startColumn = 0;
};

// PRD 5.4's "ClangdProvider / LspClient: 与 clangd 通信唯一出口". Wraps
// LspClient with clangd's specific request shapes and the byte<->UTF-16
// column conversion LSP positions require, and tracks per-file readiness
// from publishDiagnostics as CodeIntelligence's proxy for "该 TU 已完成
//索引" (PRD 5.2). Never called directly by GUI -- only CodeIntelligence
// talks to this (PRD 5.5).
class ClangdProvider {
public:
    using LocationsCallback = std::function<void(std::vector<PreciseLocation>)>;
    using SymbolsCallback = std::function<void(std::vector<WorkspaceSymbolMatch>)>;

    // `client` must outlive this ClangdProvider. Takes over `client`'s
    // notification slot (PRD 5.4: this is the sole owner of the LspClient
    // it wraps).
    explicit ClangdProvider(LspClient &client);

    // Notifies clangd a file was opened/edited/saved so its view of the
    // buffer stays current (PRD 8.7). `utf8Text` is the full current
    // buffer content -- whole-document sync (TextDocumentSyncKind.Full),
    // not incremental deltas; a deliberate v1 simplification clangd fully
    // supports.
    void didOpen(const std::string &file, const std::string &languageId, std::string utf8Text);
    void didChange(const std::string &file, std::string utf8Text);
    void didSave(const std::string &file);

    // True once at least one publishDiagnostics has arrived for `file`
    // since its most recent didOpen -- this provider's proxy for "clangd
    // has indexed this TU enough to answer precisely" (PRD 5.2).
    bool isFileReady(const std::string &file) const;

    // `row`/`byteColumn` locate the cursor using this codebase's usual
    // convention; `lineTextUtf8` is that row's current text (the caller
    // already has the live buffer -- reading it back from disk here
    // would risk staleness against unsaved edits), used only to compute
    // the UTF-16 position LSP requires.
    void findDefinition(const std::string &file, uint32_t row, std::string_view lineTextUtf8, uint32_t byteColumn,
                         LocationsCallback callback);
    void findReferences(const std::string &file, uint32_t row, std::string_view lineTextUtf8, uint32_t byteColumn,
                         LocationsCallback callback);
    void searchWorkspaceSymbols(std::string query, SymbolsCallback callback);

private:
    void handleNotification(const std::string &method, const nlohmann::json &params);
    std::vector<PreciseLocation> parseLocations(const nlohmann::json &result) const;

    LspClient &client_;
    mutable std::mutex readyMutex_;
    std::set<std::string> readyFiles_;
    mutable std::mutex versionMutex_;
    std::map<std::string, int> documentVersions_;
};

} // namespace xinsight::core::lsp
