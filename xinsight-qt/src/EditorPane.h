#pragma once

#include <QTabWidget>

#include "xinsight/core/context/ContextEngine.h"
#include "xinsight/core/intel/CodeIntelligence.h"
#include "xinsight/core/intel/TreeSitterEngine.h"
#include "xinsight/core/theme/Theme.h"

class EditorView;
class DocumentRegistry;

// A single split "cell" (PRD 2.2): a tab group of EditorViews. Multiple
// EditorPanes are arranged into a QSplitter tree by SplitManager.
class EditorPane final : public QTabWidget {
    Q_OBJECT

public:
    explicit EditorPane(xinsight::core::intel::TreeSitterEngine &engine, DocumentRegistry &registry,
                         xinsight::core::theme::ThemeManager &themeManager,
                         xinsight::core::intel::CodeIntelligence &codeIntelligence,
                         xinsight::core::context::ContextEngine &contextEngine, QWidget *parent = nullptr);

    // Opens `absolutePath` and makes it current. If this pane already has a
    // tab open on that exact path, switches to it instead of creating a
    // duplicate -- otherwise creates a new tab. Returns nullptr (and adds
    // no tab) if the file couldn't be opened.
    EditorView *openFile(const QString &absolutePath);

    // Adds a new untitled ("新建文件", PRD 2.3) tab and makes it current.
    EditorView *newUntitledTab();

    EditorView *currentEditor() const;
    EditorView *editorAt(int index) const;

signals:
    // Emitted whenever the current tab changes, including to nullptr when
    // the pane becomes empty (its last tab was closed).
    void currentEditorChanged(EditorView *editor);

private:
    void addEditorTab(EditorView *view, const QString &label);
    // Prompts to save if dirty; returns false if the user cancelled (the
    // tab must stay open in that case).
    bool confirmCloseTab(int index);

    xinsight::core::intel::TreeSitterEngine &engine_;
    DocumentRegistry &registry_;
    xinsight::core::theme::ThemeManager &themeManager_;
    xinsight::core::intel::CodeIntelligence &codeIntelligence_;
    xinsight::core::context::ContextEngine &contextEngine_;
};
