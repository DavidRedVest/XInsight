#include "OutlineView.h"

using xinsight::core::intel::Symbol;
using xinsight::core::intel::SymbolKind;

namespace {

constexpr int kByteOffsetRole = Qt::UserRole + 1;

QString kindLabel(SymbolKind kind) {
    switch (kind) {
    case SymbolKind::Function: return QStringLiteral("fn");
    case SymbolKind::Method: return QStringLiteral("method");
    case SymbolKind::Class: return QStringLiteral("class");
    case SymbolKind::Struct: return QStringLiteral("struct");
    case SymbolKind::Enum: return QStringLiteral("enum");
    case SymbolKind::Union: return QStringLiteral("union");
    case SymbolKind::Namespace: return QStringLiteral("namespace");
    case SymbolKind::Typedef: return QStringLiteral("typedef");
    case SymbolKind::Macro: return QStringLiteral("macro");
    case SymbolKind::GlobalVariable: return QStringLiteral("var");
    }
    return QStringLiteral("?");
}

} // namespace

OutlineView::OutlineView(QWidget *parent) : QListWidget(parent) {
    connect(this, &QListWidget::itemActivated, this, &OutlineView::onItemActivated);
}

void OutlineView::populate(const std::vector<Symbol> &symbols) {
    clear();
    for (const Symbol &symbol : symbols) {
        auto *item = new QListWidgetItem(QStringLiteral("[%1] %2 : %3")
                                              .arg(kindLabel(symbol.kind))
                                              .arg(QString::fromStdString(symbol.name))
                                              .arg(symbol.startRow + 1));
        item->setData(kByteOffsetRole, static_cast<int>(symbol.startByte));
        addItem(item);
    }
}

void OutlineView::onItemActivated(QListWidgetItem *item) {
    if (item == nullptr) return;
    emit symbolActivated(item->data(kByteOffsetRole).toInt());
}
