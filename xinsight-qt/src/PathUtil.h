#pragma once

#include <QString>
#include <filesystem>

// Shared by EditorView (which stores filePath() in this canonical form) and
// EditorPane (which needs to compare an incoming path against already-open
// tabs using that same form) -- must stay a single definition so the two
// never silently drift into different notions of "same file".
inline std::filesystem::path canonicalOf(const QString &absolutePath) {
    std::error_code ec;
    std::filesystem::path resolved = std::filesystem::weakly_canonical(absolutePath.toStdString(), ec);
    return ec ? std::filesystem::path(absolutePath.toStdString()) : resolved;
}
