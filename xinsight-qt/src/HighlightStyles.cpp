#include "HighlightStyles.h"

#include <QHash>

namespace xinsight::qt::highlights {

namespace {

// Fixed order (alphabetical) so style-number assignment is deterministic
// across runs; must stay in sync with the capture set actually emitted by
// xinsight-core/query/{c,cpp}/highlights.scm.
const QList<QString> &captureOrder() {
    static const QList<QString> order = {
        "attribute",
        "boolean",
        "character",
        "comment",
        "comment.documentation",
        "constant",
        "constant.builtin",
        "constant.macro",
        "constructor",
        "function",
        "function.builtin",
        "function.call",
        "function.macro",
        "function.method",
        "function.method.call",
        "keyword",
        "keyword.conditional",
        "keyword.conditional.ternary",
        "keyword.coroutine",
        "keyword.directive",
        "keyword.directive.define",
        "keyword.exception",
        "keyword.import",
        "keyword.modifier",
        "keyword.operator",
        "keyword.repeat",
        "keyword.return",
        "keyword.type",
        "label",
        "module",
        "number",
        "operator",
        "property",
        "punctuation.bracket",
        "punctuation.delimiter",
        "punctuation.special",
        "string",
        "string.escape",
        "type",
        "type.builtin",
        "type.definition",
        "variable",
        "variable.builtin",
        "variable.member",
        "variable.parameter",
    };
    return order;
}

const QHash<QString, int> &captureStyleNumbers() {
    static const QHash<QString, int> table = [] {
        QHash<QString, int> result;
        int next = 1; // 0 is kDefaultStyle
        for (const QString &capture : captureOrder()) {
            if (next >= 32 && next <= 39) next = 40; // Scintilla-reserved range
            result.insert(capture, next);
            ++next;
        }
        return result;
    }();
    return table;
}

} // namespace

QColor colorForCapture(const xinsight::core::theme::Theme &theme, const QString &capture) {
    return toQColor(xinsight::core::theme::resolveTokenColor(theme, capture.toStdString()));
}

int styleNumberForCapture(const QString &capture) {
    return captureStyleNumbers().value(capture, kDefaultStyle);
}

const QList<QString> &knownCaptures() {
    return captureOrder();
}

} // namespace xinsight::qt::highlights
