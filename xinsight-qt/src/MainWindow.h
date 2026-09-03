#pragma once

#include <QCloseEvent>
#include <QMainWindow>

#include "DocumentRegistry.h"
#include "QtUiDispatcher.h"
#include "xinsight/core/context/ContextEngine.h"
#include "xinsight/core/intel/CodeIntelligence.h"
#include "xinsight/core/intel/TreeSitterEngine.h"
#include "xinsight/core/nav/NavigationEngine.h"
#include "xinsight/core/project/ProjectModel.h"
#include "xinsight/core/search/SearchEngine.h"
#include "xinsight/core/theme/Theme.h"

class ProjectTreeView;
class OutlineView;
class EditorView;
class SplitManager;
class SearchPanel;
class ContextPaneView;
class ClangdStatusView;
class QDockWidget;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    // Explicit (not compiler-generated): splitManager_ owns EditorViews
    // that hold references to treeSitterEngine_/documentRegistry_/
    // themeManager_/codeIntelligence_/contextEngine_ below. Those are
    // plain members, destroyed by the
    // implicit part of this destructor right after its body runs -- but
    // Qt only tears down child widgets (deleting the EditorViews) later,
    // inside ~QWidget(), which runs *after* that. Left alone, EditorView's
    // destructor dereferences an already-destroyed DocumentRegistry&
    // (SIGSEGV on quit). Deleting splitManager_ here, before the body
    // returns, forces EditorView destruction while those refs are still
    // valid.
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void openProject();

private:
    void onActiveEditorChanged(EditorView *editor);
    void updateStatusForActiveEditor();
    // Returns false (and leaves the window open) if the user cancelled
    // when prompted about a dirty editor.
    bool confirmCloseAllDirtyEditors();

    // Opens `absolutePath` (if not already the active file) and moves the
    // cursor to line/column (1-based line, byte offset within the line),
    // recording the jump with navigationEngine_ first so Cmd+[ / Cmd+]
    // can retrace it (PRD 5.1: NavigationEngine is the single exit point
    // for jump/back-forward state).
    void jumpTo(const QString &absolutePath, int line, int column);
    void goBack();
    void goForward();

    // Switches the active theme and re-paints every open EditorView with it
    // (PRD 5.7: theme selection is global, not per-view).
    void switchTheme(const std::string &themeName);

    // F12 / Shift+F12 / Cmd+T (PRD 3.2): all three go through
    // codeIntelligence_ only, never TreeSitterEngine/ISymbolIndex directly.
    void jumpToDefinition();
    void findReferencesAtCursor();
    void promptWorkspaceSymbolSearch();

    // Renders `results` into searchPanel_/searchDock_ (PRD 3.3: text
    // search, find-references, and workspace symbol search share one
    // dockable results panel) and shows/raises the dock.
    void showSearchResults(const QString &statusText, std::vector<xinsight::core::search::SearchResult> results);

    // Declaration order matters: uiDispatcher_ must outlive/precede
    // projectModel_/searchEngine_/codeIntelligence_, which hold a
    // reference to it; codeIntelligence_ must likewise be declared (and
    // thus destroyed) before treeSitterEngine_, whose reference it holds;
    // contextEngine_ must be declared (destroyed) after codeIntelligence_,
    // whose reference *it* holds.
    QtUiDispatcher uiDispatcher_;
    xinsight::core::intel::TreeSitterEngine treeSitterEngine_;
    DocumentRegistry documentRegistry_;
    xinsight::core::theme::ThemeManager themeManager_;
    xinsight::core::intel::CodeIntelligence codeIntelligence_;
    xinsight::core::context::ContextEngine contextEngine_;
    xinsight::core::project::ProjectModel projectModel_;
    xinsight::core::search::SearchEngine searchEngine_;
    xinsight::core::nav::NavigationEngine navigationEngine_;
    ProjectTreeView *projectTree_ = nullptr;
    OutlineView *outlineView_ = nullptr;
    SplitManager *splitManager_ = nullptr;
    SearchPanel *searchPanel_ = nullptr;
    QDockWidget *searchDock_ = nullptr;
    ContextPaneView *contextPaneView_ = nullptr;
    ClangdStatusView *clangdStatusView_ = nullptr;
    EditorView *activeEditor_ = nullptr;
    QMetaObject::Connection activeEditorReparsedConnection_;
    QMetaObject::Connection activeEditorLabelConnection_;
    QMetaObject::Connection activeEditorGotoDefConnection_;
};
