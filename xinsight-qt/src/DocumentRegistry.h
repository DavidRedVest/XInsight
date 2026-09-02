#pragma once

#include <Qsci/qscidocument.h>
#include <filesystem>
#include <map>

#include "xinsight/core/encoding/TextCodec.h"

// PRD 8.7: "多个分屏格子打开同一文件时，编辑与 dirty 状态需要一致（共享同一
// 文档缓冲，不要各存一份）". QsciDocument is implicitly-shared and designed
// exactly for this (multiple QsciScintilla views attached to one document
// share its text buffer, undo stack, and modified/save-point state). This
// registry is the map from canonical file path -> that shared document (plus
// the encoding metadata decode() produced, which is a property of the file's
// content, not of any one view onto it).
//
// Refcounted so the entry (and the underlying QsciDocument) is dropped once
// every EditorView showing that file has closed -- reopening the file after
// that re-reads from disk rather than resurrecting stale state.
class DocumentRegistry {
public:
    struct Entry {
        QsciDocument document;
        xinsight::core::encoding::TextEncoding encoding;
        xinsight::core::encoding::LineEnding lineEnding;
        bool hadBom = false;
    };

    // Returns the existing entry for `canonicalPath` and bumps its
    // refcount, or nullptr if the file isn't currently open anywhere.
    Entry *acquireExisting(const std::filesystem::path &canonicalPath);

    // Registers a freshly-loaded file (refcount starts at 1). `canonicalPath`
    // must not already be registered.
    Entry &registerNew(const std::filesystem::path &canonicalPath, QsciDocument document,
                        xinsight::core::encoding::TextEncoding encoding, xinsight::core::encoding::LineEnding lineEnding,
                        bool hadBom);

    // Decrements the refcount for `canonicalPath`; erases the entry once it
    // reaches zero. Safe to call with a path that isn't registered (no-op).
    void release(const std::filesystem::path &canonicalPath);

private:
    struct Slot {
        Entry entry;
        int refCount = 0;
    };

    std::map<std::filesystem::path, Slot> slots_;
};
