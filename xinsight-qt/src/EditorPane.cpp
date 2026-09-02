#include "EditorPane.h"

#include <QFileInfo>
#include <QMessageBox>

#include "EditorView.h"
#include "PathUtil.h"

using xinsight::core::intel::TreeSitterEngine;

EditorPane::EditorPane(TreeSitterEngine &engine, DocumentRegistry &registry,
                        xinsight::core::theme::ThemeManager &themeManager,
                        xinsight::core::intel::CodeIntelligence &codeIntelligence,
                        xinsight::core::context::ContextEngine &contextEngine, QWidget *parent)
    : QTabWidget(parent), engine_(engine), registry_(registry), themeManager_(themeManager),
      codeIntelligence_(codeIntelligence), contextEngine_(contextEngine) {
    setTabsClosable(true);
    setMovable(true);
    setDocumentMode(true);

    connect(this, &QTabWidget::tabCloseRequested, this, [this](int index) {
        if (!confirmCloseTab(index)) return;
        QWidget *w = widget(index);
        removeTab(index);
        delete w;
    });

    connect(this, &QTabWidget::currentChanged, this,
            [this](int index) { emit currentEditorChanged(qobject_cast<EditorView *>(widget(index))); });
}

bool EditorPane::confirmCloseTab(int index) {
    EditorView *view = editorAt(index);
    if (view == nullptr || !view->isDirty()) return true;

    setCurrentIndex(index);
    auto choice = QMessageBox::question(
        this, tr("Unsaved Changes"), tr("%1 has unsaved changes. Save before closing?").arg(view->displayName()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);

    if (choice == QMessageBox::Cancel) return false;
    if (choice == QMessageBox::Save) return view->save();
    return true; // Discard
}

void EditorPane::addEditorTab(EditorView *view, const QString &label) {
    addTab(view, label);
    setCurrentWidget(view);
    connect(view, &EditorView::tabLabelChanged, this, [this, view]() {
        int idx = indexOf(view);
        if (idx >= 0) setTabText(idx, view->displayName());
    });
}

EditorView *EditorPane::openFile(const QString &absolutePath) {
    QString canonical = QString::fromStdString(canonicalOf(absolutePath).string());
    for (int i = 0; i < count(); ++i) {
        EditorView *existing = editorAt(i);
        if (existing != nullptr && existing->filePath() == canonical) {
            setCurrentIndex(i);
            return existing;
        }
    }

    auto *view = new EditorView(engine_, registry_, themeManager_, codeIntelligence_, contextEngine_, this);
    if (!view->openFile(absolutePath)) {
        delete view;
        return nullptr;
    }

    addEditorTab(view, view->displayName());
    return view;
}

EditorView *EditorPane::newUntitledTab() {
    auto *view = new EditorView(engine_, registry_, themeManager_, codeIntelligence_, contextEngine_, this);
    view->newUntitled();
    addEditorTab(view, view->displayName());
    return view;
}

EditorView *EditorPane::currentEditor() const {
    return qobject_cast<EditorView *>(currentWidget());
}

EditorView *EditorPane::editorAt(int index) const {
    return qobject_cast<EditorView *>(widget(index));
}
