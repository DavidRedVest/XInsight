#pragma once

#include <QTreeWidget>
#include <vector>

#include "xinsight/core/project/ProjectModel.h"

// Renders a xinsight::core::project::FileEntry list as a tree. Pure
// rendering, per PRD 5.4/5.5: this widget never scans the filesystem or
// touches ProjectModel itself -- MainWindow owns the core-side scan and
// pushes results in via populate(); this view only asks (via signals) and
// MainWindow performs the actual filesystem mutation + rescan.
class ProjectTreeView final : public QTreeWidget {
    Q_OBJECT

public:
    explicit ProjectTreeView(QWidget *parent = nullptr);

    void populate(const std::vector<xinsight::core::project::FileEntry> &entries);

signals:
    // Emitted with a path relative to the current project root, using
    // forward slashes, when a file (not directory) item is double-clicked.
    void fileActivated(const QString &relativePath);

    // PRD 2.3 "新建文件/文件夹(在文件树右键或菜单)". `parentRelativeDir` is
    // the directory to create inside, relative to the project root (empty
    // string means the root itself).
    void newFileRequested(const QString &parentRelativeDir);
    void newFolderRequested(const QString &parentRelativeDir);

private:
    void onItemActivated(QTreeWidgetItem *item, int column);
    void onContextMenuRequested(const QPoint &pos);
};
