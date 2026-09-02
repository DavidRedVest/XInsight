#pragma once

#include <QColor>
#include <QString>

#include "xinsight/core/theme/Theme.h"

// Maps xinsight-core's tree-sitter highlight captures (e.g.
// "function.call", "keyword.conditional") to Scintilla style numbers, and
// resolves colors against a loaded xinsight::core::theme::Theme (CP7) via
// the same dot-prefix fallback contract that theme::resolveTokenColor()
// implements.
//
// Style-number budget: Scintilla reserves style numbers 32-39 for its own
// use (STYLE_DEFAULT, STYLE_LINENUMBER, ...); styleNumberForCapture() skips
// that range.
namespace xinsight::qt::highlights {

constexpr int kDefaultStyle = 0;

inline QColor toQColor(const xinsight::core::theme::Color &c) { return QColor(c.r, c.g, c.b); }

// Resolves a capture name to a color from `theme` by trying it verbatim,
// then progressively shorter dot-separated prefixes (e.g. "string.escape"
// falls back to "string" if there's no dedicated entry), falling back to
// the theme's editor foreground if nothing matches.
QColor colorForCapture(const xinsight::core::theme::Theme &theme, const QString &capture);

// Stable style-number assignment for a fixed, known set of capture names
// (see xinsight-core/query/{c,cpp}/highlights.scm). Returns kDefaultStyle
// for anything not in that set.
int styleNumberForCapture(const QString &capture);

// All capture names styleNumberForCapture() knows about; used to
// initialize every style's color once per editor widget.
const QList<QString> &knownCaptures();

} // namespace xinsight::qt::highlights
