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

#include "EditorView.h"
#include "OutlineView.h"
#include "ProjectTreeView.h"
#include "SearchPanel.h"
#include "SplitManager.h"
#include "xinsight/core/Version.h"
#include "xinsight/core/encoding/TextCodec.h"
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
    : QMainWindow(parent), themeManager_(userConfigDir()), projectModel_(uiDispatcher_), searchEngine_(uiDispatcher_) {
    setWindowTitle(QStringLiteral("XInsight %1")
                        .arg(QString::fromUtf8(xinsight::core::version().data(),
                                                static_cast<int>(xinsight::core::version().size()))));
    resize(1200, 800);

    splitManager_ = new SplitManager(treeSitterEngine_, documentRegistry_, themeManager_, this);
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

    searchPanel_ = new SearchPanel(this);
    auto *searchDock = new QDockWidget(tr("Search"), this);
    searchDock->setWidget(searchPanel_);
    searchDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable);
    searchDock->hide(); // stays out of the way until Cmd+Shift+F
    addDockWidget(Qt::BottomDockWidgetArea, searchDock);

    auto *fileMenu = menuBar()->addMenu(tr("&File"));

    auto *viewMenu = menuBar()->addMenu(tr("&View"));
    // toggleViewAction() is the dock's own checkable show/hide action: it
    // stays in sync when the user closes a dock via its title-bar close
    // button, so this is also how a closed Project/Outline/Search panel
    // gets reopened -- without it there's no way back in once closed.
    viewMenu->addAction(projectDock->toggleViewAction());
    viewMenu->addAction(outlineDock->toggleViewAction());
    viewMenu->addAction(searchDock->toggleViewAction());

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
    connect(findInFilesAction, &QAction::triggered, this, [searchDock, this]() {
        searchDock->show();
        searchDock->raise();
        searchPanel_->focusQuery();
    });

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

    projectModel_.setOnScanComplete([this](std::vector<FileEntry> entries) { projectTree_->populate(entries); });

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
    activeEditor_ = editor;

    if (editor != nullptr) {
        activeEditorReparsedConnection_ = connect(editor, &EditorView::contentReparsed, this,
                                                   [this, editor]() { outlineView_->populate(editor->outline()); });
        activeEditorLabelConnection_ =
            connect(editor, &EditorView::tabLabelChanged, this, [this]() { updateStatusForActiveEditor(); });
        outlineView_->populate(editor->outline());
    } else {
        outlineView_->populate({});
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
    projectModel_.openRoot(dir.toStdString());
}
