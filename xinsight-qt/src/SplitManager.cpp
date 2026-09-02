#include "SplitManager.h"

#include <QApplication>
#include <QSplitter>
#include <QVBoxLayout>

#include "EditorPane.h"
#include "EditorView.h"

using xinsight::core::intel::TreeSitterEngine;

SplitManager::SplitManager(TreeSitterEngine &engine, DocumentRegistry &registry,
                            xinsight::core::theme::ThemeManager &themeManager,
                            xinsight::core::intel::CodeIntelligence &codeIntelligence,
                            xinsight::core::context::ContextEngine &contextEngine, QWidget *parent)
    : QWidget(parent), engine_(engine), registry_(registry), themeManager_(themeManager),
      codeIntelligence_(codeIntelligence), contextEngine_(contextEngine) {
    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);

    root_ = createPane();
    layout_->addWidget(root_);
    setActivePane(qobject_cast<EditorPane *>(root_));

    connect(qApp, &QApplication::focusChanged, this,
            [this](QWidget * /*old*/, QWidget *now) { onFocusChanged(now); });
}

EditorPane *SplitManager::createPane() {
    auto *pane = new EditorPane(engine_, registry_, themeManager_, codeIntelligence_, contextEngine_, this);
    connect(pane, &EditorPane::currentEditorChanged, this, [this, pane](EditorView *editor) {
        if (pane == activePane_) emit activeEditorChanged(editor);
    });
    return pane;
}

void SplitManager::setActivePane(EditorPane *pane) {
    if (pane == activePane_) return;
    activePane_ = pane;
    emit activeEditorChanged(pane != nullptr ? pane->currentEditor() : nullptr);
}

void SplitManager::onFocusChanged(QWidget *now) {
    EditorPane *pane = findAncestorPane(now);
    if (pane != nullptr) setActivePane(pane);
}

EditorPane *SplitManager::findAncestorPane(QWidget *w) {
    while (w != nullptr) {
        if (auto *pane = qobject_cast<EditorPane *>(w)) return pane;
        w = w->parentWidget();
    }
    return nullptr;
}

EditorPane *SplitManager::findFirstPane(QWidget *w) {
    if (w == nullptr) return nullptr;
    if (auto *pane = qobject_cast<EditorPane *>(w)) return pane;
    if (auto *splitter = qobject_cast<QSplitter *>(w)) {
        for (int i = 0; i < splitter->count(); ++i) {
            if (EditorPane *found = findFirstPane(splitter->widget(i))) return found;
        }
    }
    return nullptr;
}

EditorView *SplitManager::activeEditor() const {
    return activePane_ != nullptr ? activePane_->currentEditor() : nullptr;
}

EditorView *SplitManager::openFileInActivePane(const QString &absolutePath) {
    if (activePane_ == nullptr) setActivePane(findFirstPane(root_));
    if (activePane_ == nullptr) return nullptr;
    return activePane_->openFile(absolutePath);
}

EditorView *SplitManager::newUntitledInActivePane() {
    if (activePane_ == nullptr) setActivePane(findFirstPane(root_));
    if (activePane_ == nullptr) return nullptr;
    return activePane_->newUntitledTab();
}

void SplitManager::collectPanes(QWidget *w, std::vector<EditorPane *> &out) {
    if (w == nullptr) return;
    if (auto *pane = qobject_cast<EditorPane *>(w)) {
        out.push_back(pane);
        return;
    }
    if (auto *splitter = qobject_cast<QSplitter *>(w)) {
        for (int i = 0; i < splitter->count(); ++i) collectPanes(splitter->widget(i), out);
    }
}

std::vector<EditorView *> SplitManager::allEditors() const {
    std::vector<EditorPane *> panes;
    collectPanes(root_, panes);

    std::vector<EditorView *> editors;
    for (EditorPane *pane : panes) {
        for (int i = 0; i < pane->count(); ++i) {
            if (EditorView *view = pane->editorAt(i)) editors.push_back(view);
        }
    }
    return editors;
}

void SplitManager::splitActivePane(Qt::Orientation orientation) {
    if (activePane_ == nullptr) return;

    QWidget *current = activePane_;
    QWidget *parent = current->parentWidget();
    auto *parentSplitter = qobject_cast<QSplitter *>(parent);

    EditorPane *newPane = createPane();
    if (EditorView *sourceEditor = activePane_->currentEditor()) {
        newPane->openFile(sourceEditor->filePath());
    }

    if (parentSplitter != nullptr && parentSplitter->orientation() == orientation) {
        int idx = parentSplitter->indexOf(current);
        parentSplitter->insertWidget(idx + 1, newPane);
    } else if (parentSplitter != nullptr) {
        int idx = parentSplitter->indexOf(current);
        auto *newSplitter = new QSplitter(orientation);
        newSplitter->addWidget(current); // reparents `current` out of parentSplitter
        newSplitter->addWidget(newPane);
        parentSplitter->insertWidget(idx, newSplitter);
    } else {
        // `current` is the sole root; no splitter exists yet.
        layout_->removeWidget(current);
        auto *newSplitter = new QSplitter(orientation);
        newSplitter->addWidget(current);
        newSplitter->addWidget(newPane);
        layout_->addWidget(newSplitter);
        root_ = newSplitter;
    }

    setActivePane(newPane);
    newPane->setFocus();
}

void SplitManager::closeActivePane() {
    if (activePane_ == nullptr) return;

    EditorPane *pane = activePane_;
    auto *parentSplitter = qobject_cast<QSplitter *>(pane->parentWidget());
    if (parentSplitter == nullptr) return; // refuse to close the last remaining pane

    delete pane;

    if (parentSplitter->count() == 1) {
        QWidget *remaining = parentSplitter->widget(0);
        auto *grandSplitter = qobject_cast<QSplitter *>(parentSplitter->parentWidget());

        if (grandSplitter != nullptr) {
            int idx = grandSplitter->indexOf(parentSplitter);
            remaining->setParent(nullptr);
            grandSplitter->insertWidget(idx, remaining);
        } else {
            layout_->removeWidget(parentSplitter);
            remaining->setParent(nullptr);
            layout_->addWidget(remaining);
            root_ = remaining;
        }
        parentSplitter->deleteLater();
    }

    EditorPane *newActive = findFirstPane(root_);
    setActivePane(newActive);
    if (newActive != nullptr) newActive->setFocus();
}
