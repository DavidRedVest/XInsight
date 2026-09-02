#pragma once

#include <QWidget>
#include <vector>

#include "xinsight/core/intel/CodeIntelligence.h"
#include "xinsight/core/intel/TreeSitterEngine.h"
#include "xinsight/core/theme/Theme.h"

class QVBoxLayout;
class QSplitter;
class EditorPane;
class EditorView;
class DocumentRegistry;

// Owns a nested QSplitter tree of EditorPane "cells" (PRD 2.2). Each cell
// is an independent tab group; splitting/closing rearranges the tree while
// preserving every other cell's widget identity (no rebuild-from-scratch).
//
// "Active pane" tracks whichever EditorPane last had keyboard focus
// anywhere inside it (including inside its current EditorView), via
// QApplication::focusChanged -- this is what Cmd+\ / Cmd+Shift+\ / Cmd+W
// and outline/navigation wiring operate on.
class SplitManager final : public QWidget {
    Q_OBJECT

public:
    explicit SplitManager(xinsight::core::intel::TreeSitterEngine &engine, DocumentRegistry &registry,
                           xinsight::core::theme::ThemeManager &themeManager,
                           xinsight::core::intel::CodeIntelligence &codeIntelligence, QWidget *parent = nullptr);

    // May be null if the active pane has no tabs open.
    EditorView *activeEditor() const;

    // Opens `absolutePath` as a new tab in the active pane (creating an
    // initial pane first if none exists yet).
    EditorView *openFileInActivePane(const QString &absolutePath);

    // PRD 2.3 "新建文件": adds an untitled tab to the active pane.
    EditorView *newUntitledInActivePane();

    void splitActivePane(Qt::Orientation orientation);
    void closeActivePane();

    // All EditorViews across every pane, for "prompt to save everything
    // dirty" on window close.
    std::vector<EditorView *> allEditors() const;

signals:
    // Emitted whenever the active pane or its current tab changes.
    void activeEditorChanged(EditorView *editor);

private:
    EditorPane *createPane();
    void setActivePane(EditorPane *pane);
    void onFocusChanged(QWidget *now);
    static EditorPane *findAncestorPane(QWidget *w);
    static EditorPane *findFirstPane(QWidget *w);
    static void collectPanes(QWidget *w, std::vector<EditorPane *> &out);

    xinsight::core::intel::TreeSitterEngine &engine_;
    DocumentRegistry &registry_;
    xinsight::core::theme::ThemeManager &themeManager_;
    xinsight::core::intel::CodeIntelligence &codeIntelligence_;
    QVBoxLayout *layout_ = nullptr;
    QWidget *root_ = nullptr;
    EditorPane *activePane_ = nullptr;
};
