#pragma once

#include <QCloseEvent>
#include <QMainWindow>

#include "DocumentRegistry.h"
#include "QtUiDispatcher.h"
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

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

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

    // Declaration order matters: uiDispatcher_ must outlive/precede
    // projectModel_/searchEngine_, which hold a reference to it.
    QtUiDispatcher uiDispatcher_;
    xinsight::core::intel::TreeSitterEngine treeSitterEngine_;
    DocumentRegistry documentRegistry_;
    xinsight::core::theme::ThemeManager themeManager_;
    xinsight::core::project::ProjectModel projectModel_;
    xinsight::core::search::SearchEngine searchEngine_;
    xinsight::core::nav::NavigationEngine navigationEngine_;
    ProjectTreeView *projectTree_ = nullptr;
    OutlineView *outlineView_ = nullptr;
    SplitManager *splitManager_ = nullptr;
    SearchPanel *searchPanel_ = nullptr;
    EditorView *activeEditor_ = nullptr;
    QMetaObject::Connection activeEditorReparsedConnection_;
    QMetaObject::Connection activeEditorLabelConnection_;
};
