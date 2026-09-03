#include "MainWindow.h"

#include <QAction>
#include <QActionGroup>
#include <QDockWidget>
#include <QFileDialog>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QString>

#include "ClangdStatusView.h"
#include "ContextPaneView.h"
#include "EditorView.h"
#include "OutlineView.h"
#include "ProjectTreeView.h"
#include "SearchPanel.h"
#include "SplitManager.h"
#include "xinsight/core/Version.h"
#include "xinsight/core/encoding/TextCodec.h"
#include "xinsight/core/intel/SymbolIndex.h"
#include "xinsight/core/project/ProjectModel.h"

using xinsight::core::nav::NavigationLocation;
using xinsight::core::project::FileEntry;
using xinsight::core::search::SearchOptions;
using xinsight::core::search::SearchResult;

namespace {
std::filesystem::path userConfigDir() {
    // AppDataLocation, not AppConfigLocation: on macOS the latter maps to
    // ~/Library/Preferences, but PRD 5.7 specifies
    // ~/Library/Application Support/XInsight for global/user config.
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return std::filesystem::path(dir.toStdString());
}
} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), themeManager_(userConfigDir()),
      codeIntelligence_(treeSitterEngine_, std::make_shared<xinsight::core::intel::InMemorySymbolIndex>(),
                         uiDispatcher_),
      contextEngine_(codeIntelligence_, uiDispatcher_), projectModel_(uiDispatcher_), searchEngine_(uiDispatcher_) {
    setWindowTitle(QStringLiteral("XInsight %1")
                        .arg(QString::fromUtf8(xinsight::core::version().data(),
                                                static_cast<int>(xinsight::core::version().size()))));
    resize(1200, 800);

    splitManager_ = new SplitManager(treeSitterEngine_, documentRegistry_, themeManager_, codeIntelligence_,
                                      contextEngine_, this);
    setCentralWidget(splitManager_);

    projectTree_ = new ProjectTreeView(this);
    auto *projectDock = new QDockWidget(tr("Project"), this);
    projectDock->setWidget(projectTree_);
    projectDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::LeftDockWidgetArea, projectDock);

    outlineView_ = new OutlineView(this);
    auto *outlineDock = new QDockWidget(tr("Outline"), this);
    outlineDock->setWidget(outlineView_);
    outlineDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::RightDockWidgetArea, outlineDock);

    // PRD 2.1: ambient context pane, docked below the editor by default
    // (position is user-movable like any dock, per PRD 2.2).
    contextPaneView_ = new ContextPaneView(this);
    auto *contextDock = new QDockWidget(tr("Context"), this);
    contextDock->setWidget(contextPaneView_);
    contextDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::BottomDockWidgetArea, contextDock);

    searchPanel_ = new SearchPanel(this);
    searchDock_ = new QDockWidget(tr("Search"), this);
    searchDock_->setWidget(searchPanel_);
    searchDock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable);
    searchDock_->hide(); // stays out of the way until Cmd+Shift+F
    addDockWidget(Qt::BottomDockWidgetArea, searchDock_);

    // PRD 8.4's clangd config diagnostic panel: pure status display, docked
    // alongside Context by default since both are "ambient" panes.
    clangdStatusView_ = new ClangdStatusView(this);
    auto *clangdDock = new QDockWidget(tr("Clangd"), this);
    clangdDock->setWidget(clangdStatusView_);
    clangdDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::BottomDockWidgetArea, clangdDock);

    auto *fileMenu = menuBar()->addMenu(tr("&File"));

    auto *viewMenu = menuBar()->addMenu(tr("&View"));
    // toggleViewAction() is the dock's own checkable show/hide action: it
    // stays in sync when the user closes a dock via its title-bar close
    // button, so this is also how a closed Project/Outline/Search panel
    // gets reopened -- without it there's no way back in once closed.
    viewMenu->addAction(projectDock->toggleViewAction());
    viewMenu->addAction(outlineDock->toggleViewAction());
    viewMenu->addAction(contextDock->toggleViewAction());
    viewMenu->addAction(searchDock_->toggleViewAction());
    viewMenu->addAction(clangdDock->toggleViewAction());

    auto *openProjectAction = fileMenu->addAction(tr("&Open Project..."));
    openProjectAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+O")));
    connect(openProjectAction, &QAction::triggered, this, &MainWindow::openProject);

    auto *newFileAction = fileMenu->addAction(tr("&New File"));
    newFileAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+N")));
    connect(newFileAction, &QAction::triggered, this, [this]() { splitManager_->newUntitledInActivePane(); });

    fileMenu->addSeparator();

    auto *saveAction = fileMenu->addAction(tr("&Save"));
    saveAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+S")));
    connect(saveAction, &QAction::triggered, this, [this]() {
        // No transient "Saved" message: the dirty bullet disappearing from
        // the tab (and the persistent status line, via tabLabelChanged) is
        // the feedback -- a timed showMessage() here would clobber that
        // persistent status line and then leave the bar blank once it expires.
        if (activeEditor_ != nullptr) activeEditor_->save();
    });

    auto *saveAsAction = fileMenu->addAction(tr("Save &As..."));
    saveAsAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+S")));
    connect(saveAsAction, &QAction::triggered, this, [this]() {
        if (activeEditor_ == nullptr) return;
        QString path = QFileDialog::getSaveFileName(this, tr("Save As"));
        if (path.isEmpty()) return;
        activeEditor_->saveAs(path); // emits tabLabelChanged(), which refreshes the persistent status line
    });

    auto *windowMenu = menuBar()->addMenu(tr("&Window"));
    auto *splitRightAction = windowMenu->addAction(tr("Split Left/Right"));
    splitRightAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+\\")));
    connect(splitRightAction, &QAction::triggered, this,
            [this]() { splitManager_->splitActivePane(Qt::Horizontal); });

    auto *splitDownAction = windowMenu->addAction(tr("Split Top/Bottom"));
    splitDownAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+\\")));
    connect(splitDownAction, &QAction::triggered, this,
            [this]() { splitManager_->splitActivePane(Qt::Vertical); });

    auto *closePaneAction = windowMenu->addAction(tr("Close Pane"));
    closePaneAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+W")));
    connect(closePaneAction, &QAction::triggered, this, [this]() { splitManager_->closeActivePane(); });

    windowMenu->addSeparator();

    auto *goBackAction = windowMenu->addAction(tr("Go Back"));
    goBackAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+[")));
    connect(goBackAction, &QAction::triggered, this, &MainWindow::goBack);

    auto *goForwardAction = windowMenu->addAction(tr("Go Forward"));
    goForwardAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+]")));
    connect(goForwardAction, &QAction::triggered, this, &MainWindow::goForward);

    auto *themeMenu = menuBar()->addMenu(tr("&Theme"));
    auto *themeActionGroup = new QActionGroup(this);
    themeActionGroup->setExclusive(true);
    for (const auto &theme : themeManager_.availableThemes()) {
        QString themeName = QString::fromStdString(theme.name);
        auto *themeAction = themeMenu->addAction(themeName);
        themeAction->setCheckable(true);
        themeAction->setChecked(theme.name == themeManager_.currentTheme().name);
        themeActionGroup->addAction(themeAction);
        connect(themeAction, &QAction::triggered, this,
                [this, themeName]() { switchTheme(themeName.toStdString()); });
    }

    auto *searchMenu = menuBar()->addMenu(tr("&Search"));
    auto *findInFilesAction = searchMenu->addAction(tr("Find in Files..."));
    findInFilesAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+F")));
    connect(findInFilesAction, &QAction::triggered, this, [this]() {
        searchDock_->show();
        searchDock_->raise();
        searchPanel_->focusQuery();
    });

    searchMenu->addSeparator();

    auto *gotoDefAction = searchMenu->addAction(tr("Go to Definition"));
    gotoDefAction->setShortcut(QKeySequence(QStringLiteral("F12")));
    connect(gotoDefAction, &QAction::triggered, this, &MainWindow::jumpToDefinition);

    auto *findRefsAction = searchMenu->addAction(tr("Find References"));
    findRefsAction->setShortcut(QKeySequence(QStringLiteral("Shift+F12")));
    connect(findRefsAction, &QAction::triggered, this, &MainWindow::findReferencesAtCursor);

    auto *workspaceSymbolAction = searchMenu->addAction(tr("Go to Symbol in Workspace..."));
    workspaceSymbolAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+T")));
    // QsciScintilla's own key handling swallows plain Ctrl/Cmd+letter
    // combinations before they'd otherwise bubble up to a WindowShortcut
    // (the default context) when the editor has focus -- ApplicationShortcut
    // makes Qt's shortcut dispatch check this action first, regardless of
    // which child widget is focused.
    workspaceSymbolAction->setShortcutContext(Qt::ApplicationShortcut);
    connect(workspaceSymbolAction, &QAction::triggered, this, &MainWindow::promptWorkspaceSymbolSearch);

    connect(searchPanel_, &SearchPanel::searchRequested, this, [this](const QString &query, SearchOptions options) {
        if (projectModel_.rootPath().empty()) {
            statusBar()->showMessage(tr("Open a project before searching."), 5000);
            return;
        }
        searchEngine_.search(query.toStdString(), projectModel_.rootPath(), options);
    });
    connect(searchPanel_, &SearchPanel::cancelRequested, this, [this]() { searchEngine_.cancel(); });
    connect(searchPanel_, &SearchPanel::resultActivated, this,
            [this](const QString &path, int line, int column) { jumpTo(path, line, column); });

    searchEngine_.setOnResults([this](std::vector<SearchResult> batch) { searchPanel_->appendResults(batch); });
    searchEngine_.setOnComplete(
        [this](size_t total, bool cancelled) { searchPanel_->searchFinished(total, cancelled); });

    // No timeout on either message: both persist until replaced, matching
    // the rest of this file's "no transient showMessage() over a
    // persistent status line" convention -- updateStatusForActiveEditor()
    // on completion restores whatever the persistent line should say.
    codeIntelligence_.setOnIndexProgress([this](size_t done, size_t total) {
        statusBar()->showMessage(tr("Indexing symbols... %1/%2").arg(done).arg(total));
    });
    codeIntelligence_.setOnIndexComplete([this]() { updateStatusForActiveEditor(); });

    codeIntelligence_.setOnClangdStatusChanged([this](bool running) {
        if (running) {
            clangdStatusView_->showRunning();
        } else {
            clangdStatusView_->showFailed();
        }
    });

    contextEngine_.setOnContext(
        [this](xinsight::core::context::ContextResult result) { contextPaneView_->showResult(result); });
    connect(contextPaneView_, &ContextPaneView::drillDownRequested, this,
            [this](const QString &path, int line, int column) { jumpTo(path, line, column); });

    projectModel_.setOnScanComplete([this](std::vector<FileEntry> entries) {
        projectTree_->populate(entries);

        // PRD 5.3: build the workspace symbol index in the background as
        // soon as the file list is known -- entries are FileEntry's
        // (relative path + isDirectory + exceedsSizeLimit), so resolve to
        // absolute paths and drop anything CodeIntelligence shouldn't try
        // to parse (directories, oversized files; unrecognized extensions
        // are filtered by CodeIntelligence itself).
        std::vector<std::filesystem::path> files;
        for (const FileEntry &entry : entries) {
            if (entry.isDirectory || entry.exceedsSizeLimit) continue;
            files.push_back(projectModel_.rootPath() / entry.relativePath);
        }
        codeIntelligence_.indexProject(std::move(files));
    });

    connect(projectTree_, &ProjectTreeView::fileActivated, this, [this](const QString &relativePath) {
        QString absolutePath = QString::fromStdString(projectModel_.rootPath().string()) + QLatin1Char('/') +
                                relativePath;
        // On success, activeEditorChanged -> updateStatusForActiveEditor()
        // already puts up a persistent encoding/name status line; a timed
        // "Opened:" message here would just clobber it and then vanish
        // (QStatusBar has no "revert to previous permanent message").
        if (splitManager_->openFileInActivePane(absolutePath) == nullptr) {
            statusBar()->showMessage(tr("Could not open: %1").arg(relativePath), 5000);
        }
    });

    auto rescanProject = [this]() {
        if (!projectModel_.rootPath().empty()) projectModel_.openRoot(projectModel_.rootPath());
    };

    connect(projectTree_, &ProjectTreeView::newFileRequested, this, [this, rescanProject](const QString &parentDir) {
        bool ok = false;
        QString name = QInputDialog::getText(this, tr("New File"), tr("File name:"), QLineEdit::Normal, QString(), &ok);
        if (!ok || name.isEmpty()) return;

        std::filesystem::path target = projectModel_.rootPath();
        if (!parentDir.isEmpty()) target /= parentDir.toStdString();
        target /= name.toStdString();

        if (xinsight::core::project::createEmptyFile(target)) {
            rescanProject();
        } else {
            QMessageBox::warning(this, tr("New File"), tr("Could not create %1").arg(name));
        }
    });

    connect(projectTree_, &ProjectTreeView::newFolderRequested, this,
            [this, rescanProject](const QString &parentDir) {
                bool ok = false;
                QString name = QInputDialog::getText(this, tr("New Folder"), tr("Folder name:"), QLineEdit::Normal,
                                                       QString(), &ok);
                if (!ok || name.isEmpty()) return;

                std::filesystem::path target = projectModel_.rootPath();
                if (!parentDir.isEmpty()) target /= parentDir.toStdString();
                target /= name.toStdString();

                if (xinsight::core::project::createDirectory(target)) {
                    rescanProject();
                } else {
                    QMessageBox::warning(this, tr("New Folder"), tr("Could not create %1").arg(name));
                }
            });

    connect(splitManager_, &SplitManager::activeEditorChanged, this, &MainWindow::onActiveEditorChanged);

    connect(outlineView_, &OutlineView::symbolActivated, this, [this](int byteOffset) {
        if (activeEditor_ == nullptr) return;
        if (!activeEditor_->isUntitled()) {
            navigationEngine_.push(
                {activeEditor_->filePath().toStdString(), static_cast<uint32_t>(activeEditor_->currentByteOffset())});
        }
        activeEditor_->gotoByteOffset(byteOffset);
    });

    onActiveEditorChanged(splitManager_->activeEditor());

    statusBar()->showMessage(tr("Ready"));
}

MainWindow::~MainWindow() {
    delete splitManager_;
    splitManager_ = nullptr;
}

void MainWindow::onActiveEditorChanged(EditorView *editor) {
    disconnect(activeEditorReparsedConnection_);
    disconnect(activeEditorLabelConnection_);
    disconnect(activeEditorGotoDefConnection_);
    activeEditor_ = editor;

    if (editor != nullptr) {
        activeEditorReparsedConnection_ = connect(editor, &EditorView::contentReparsed, this,
                                                   [this, editor]() { outlineView_->populate(editor->outline()); });
        activeEditorLabelConnection_ =
            connect(editor, &EditorView::tabLabelChanged, this, [this]() { updateStatusForActiveEditor(); });
        activeEditorGotoDefConnection_ =
            connect(editor, &EditorView::gotoDefinitionRequested, this, &MainWindow::jumpToDefinition);
        outlineView_->populate(editor->outline());
        // Switching panes/tabs doesn't itself move the cursor, but the
        // ambient context pane still needs to reflect wherever the newly
        // active editor's cursor already is (PRD 2.1).
        editor->updateContextForCursor();
    } else {
        outlineView_->populate({});
        contextEngine_.onCursorMoved(std::string(), std::string());
    }

    updateStatusForActiveEditor();
}

void MainWindow::updateStatusForActiveEditor() {
    if (activeEditor_ == nullptr) {
        statusBar()->clearMessage();
        return;
    }

    QString encodingName = QString::fromUtf8(xinsight::core::encoding::encodingName(activeEditor_->encoding()).data());
    QString lineEndingName =
        activeEditor_->lineEnding() == xinsight::core::encoding::LineEnding::CrLf ? tr("CRLF") : tr("LF");

    statusBar()->showMessage(
        tr("%1  |  %2  |  %3").arg(activeEditor_->displayName(), encodingName, lineEndingName));
}

void MainWindow::switchTheme(const std::string &themeName) {
    if (!themeManager_.setCurrentTheme(themeName)) return;
    for (EditorView *editor : splitManager_->allEditors()) editor->applyTheme();
}

namespace {

QString symbolKindLabel(xinsight::core::intel::SymbolKind kind) {
    using xinsight::core::intel::SymbolKind;
    switch (kind) {
    case SymbolKind::Function:
        return QStringLiteral("function");
    case SymbolKind::Method:
        return QStringLiteral("method");
    case SymbolKind::Class:
        return QStringLiteral("class");
    case SymbolKind::Struct:
        return QStringLiteral("struct");
    case SymbolKind::Enum:
        return QStringLiteral("enum");
    case SymbolKind::Union:
        return QStringLiteral("union");
    case SymbolKind::Namespace:
        return QStringLiteral("namespace");
    case SymbolKind::Typedef:
        return QStringLiteral("typedef");
    case SymbolKind::Macro:
        return QStringLiteral("macro");
    case SymbolKind::GlobalVariable:
        return QStringLiteral("variable");
    }
    return QString();
}

// Converts an async-routing symbol/reference hit into the same SearchResult
// shape ripgrep results use (PRD 3.3's unified panel model), so
// SearchPanel's existing grouping/jump rendering can be reused verbatim for
// definitions/references/workspace-symbol results too -- a single
// SubMatch carrying the name's own byte range doubles as the jump column,
// since there's no ripgrep-style surrounding line text to highlight within.
// Carries a leading "[precise]"/"[fast]" marker (PRD 5.2/8.3: "UI 用一个小标识展示
// 当前是精确还是快速模式") since QueryLocation candidates can come from
// either engine and the panel doesn't otherwise distinguish them.
SearchResult toSearchResult(const xinsight::core::intel::QueryLocation &loc, bool precise) {
    SearchResult result;
    result.path = loc.file;
    result.line = loc.startRow + 1;
    QString marker = precise ? QStringLiteral("[precise] ") : QStringLiteral("[fast] ");
    QString kindSuffix = loc.kind ? QStringLiteral(" (%1)").arg(symbolKindLabel(*loc.kind)) : QString();
    result.preview = marker.toStdString() + loc.name + kindSuffix.toStdString();
    result.submatches.push_back({loc.startColumn, loc.startColumn + static_cast<uint32_t>(loc.name.size())});
    return result;
}

} // namespace

void MainWindow::showSearchResults(const QString &statusText, std::vector<SearchResult> results) {
    searchDock_->show();
    searchDock_->raise();
    searchPanel_->clearResults();
    searchPanel_->appendResults(results);
    searchPanel_->searchFinished(results.size(), false);
    statusBar()->showMessage(statusText, 5000);
}

void MainWindow::jumpToDefinition() {
    if (activeEditor_ == nullptr) return;
    QString word = activeEditor_->wordUnderCursor();
    if (word.isEmpty()) return;

    EditorView::CursorLocation cursor = activeEditor_->currentCursorLocation();
    codeIntelligence_.findDefinitionAsync(
        word.toStdString(), activeEditor_->filePath().toStdString(), static_cast<uint32_t>(cursor.row),
        cursor.lineText.toStdString(), static_cast<uint32_t>(cursor.byteColumn),
        [this, word](xinsight::core::intel::DefinitionResult result) {
            if (result.candidates.empty()) {
                statusBar()->showMessage(tr("No definition found for '%1'").arg(word), 5000);
                return;
            }
            if (result.candidates.size() == 1) {
                const auto &loc = result.candidates[0];
                jumpTo(QString::fromStdString(loc.file), static_cast<int>(loc.startRow) + 1,
                       static_cast<int>(loc.startColumn));
                return;
            }

            std::vector<SearchResult> results;
            results.reserve(result.candidates.size());
            for (const auto &loc : result.candidates) results.push_back(toSearchResult(loc, result.precise));
            showSearchResults(tr("%1 definitions of '%2'").arg(result.candidates.size()).arg(word),
                               std::move(results));
        });
}

void MainWindow::findReferencesAtCursor() {
    if (activeEditor_ == nullptr) return;
    QString word = activeEditor_->wordUnderCursor();
    if (word.isEmpty()) return;

    EditorView::CursorLocation cursor = activeEditor_->currentCursorLocation();
    codeIntelligence_.findReferencesAsync(
        word.toStdString(), activeEditor_->filePath().toStdString(), static_cast<uint32_t>(cursor.row),
        cursor.lineText.toStdString(), static_cast<uint32_t>(cursor.byteColumn),
        [this, word](xinsight::core::intel::ReferencesResult result) {
            if (result.candidates.empty()) {
                statusBar()->showMessage(tr("No references found for '%1'").arg(word), 5000);
                return;
            }

            std::vector<SearchResult> results;
            results.reserve(result.candidates.size());
            for (const auto &loc : result.candidates) results.push_back(toSearchResult(loc, result.precise));
            showSearchResults(tr("%1 references to '%2'").arg(result.candidates.size()).arg(word),
                               std::move(results));
        });
}

void MainWindow::promptWorkspaceSymbolSearch() {
    // v1 simplification (documented, not a live-filtering quick-open
    // palette): a single blocking prompt, same QInputDialog pattern
    // already used for New File/New Folder. Results still land in the
    // shared search panel (PRD 3.3's actual requirement), just entered via
    // a plain dialog rather than a dedicated command-palette widget.
    bool ok = false;
    QString query = QInputDialog::getText(this, tr("Go to Symbol in Workspace"), tr("Symbol name:"),
                                           QLineEdit::Normal, QString(), &ok);
    if (!ok || query.isEmpty()) return;

    codeIntelligence_.searchWorkspaceSymbolsAsync(
        query.toStdString(), 200, [this, query](xinsight::core::intel::WorkspaceSymbolResult result) {
            if (result.candidates.empty()) {
                statusBar()->showMessage(tr("No symbols matching '%1'").arg(query), 5000);
                return;
            }

            std::vector<SearchResult> converted;
            converted.reserve(result.candidates.size());
            for (const auto &loc : result.candidates) converted.push_back(toSearchResult(loc, result.precise));
            showSearchResults(tr("%1 symbols matching '%2'").arg(result.candidates.size()).arg(query),
                               std::move(converted));
        });
}

void MainWindow::jumpTo(const QString &absolutePath, int line, int column) {
    if (activeEditor_ != nullptr && !activeEditor_->isUntitled()) {
        navigationEngine_.push(
            {activeEditor_->filePath().toStdString(), static_cast<uint32_t>(activeEditor_->currentByteOffset())});
    }

    EditorView *target = activeEditor_;
    if (target == nullptr || target->filePath() != absolutePath) {
        target = splitManager_->openFileInActivePane(absolutePath);
    }
    if (target == nullptr) return;

    target->gotoLineAndColumn(line, column);
}

namespace {
void applyNavigationLocation(SplitManager &splitManager, EditorView *activeEditor,
                              const std::optional<NavigationLocation> &target) {
    if (!target) return;

    EditorView *editor = activeEditor;
    if (editor == nullptr || editor->filePath().toStdString() != target->absolutePath) {
        editor = splitManager.openFileInActivePane(QString::fromStdString(target->absolutePath));
    }
    if (editor != nullptr) editor->gotoByteOffset(static_cast<int>(target->byteOffset));
}
} // namespace

void MainWindow::goBack() {
    if (activeEditor_ == nullptr) return;
    NavigationLocation current{activeEditor_->filePath().toStdString(),
                                static_cast<uint32_t>(activeEditor_->currentByteOffset())};
    applyNavigationLocation(*splitManager_, activeEditor_, navigationEngine_.goBack(current));
}

void MainWindow::goForward() {
    if (activeEditor_ == nullptr) return;
    NavigationLocation current{activeEditor_->filePath().toStdString(),
                                static_cast<uint32_t>(activeEditor_->currentByteOffset())};
    applyNavigationLocation(*splitManager_, activeEditor_, navigationEngine_.goForward(current));
}

bool MainWindow::confirmCloseAllDirtyEditors() {
    for (EditorView *editor : splitManager_->allEditors()) {
        if (!editor->isDirty()) continue;

        auto choice =
            QMessageBox::question(this, tr("Unsaved Changes"),
                                   tr("%1 has unsaved changes. Save before closing?").arg(editor->displayName()),
                                   QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);

        if (choice == QMessageBox::Cancel) return false;
        if (choice == QMessageBox::Save && !editor->save()) return false;
    }
    return true;
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (confirmCloseAllDirtyEditors()) {
        event->accept();
    } else {
        event->ignore();
    }
}

void MainWindow::openProject() {
    QString dir = QFileDialog::getExistingDirectory(this, tr("Open Project Directory"));
    if (dir.isEmpty()) return;

    statusBar()->showMessage(tr("Scanning %1...").arg(dir));
    std::filesystem::path root(dir.toStdString());
    projectModel_.openRoot(root);

    // PRD 5.2/8.4: clangd is opt-in and best-effort -- only start it when a
    // compile_commands.json is actually found, and tear down any previous
    // project's instance either way so routing never silently keeps using a
    // stale/wrong project's clangd.
    auto compileCommandsDir = xinsight::core::project::findCompileCommandsDir(root);
    if (compileCommandsDir) {
        clangdStatusView_->showStarting();
    } else {
        clangdStatusView_->showNotConfigured();
    }
    codeIntelligence_.configureClangd(compileCommandsDir, root);
}
