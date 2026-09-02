#include "ProjectTreeView.h"

#include <QFileInfo>
#include <QHash>
#include <QIcon>
#include <QMenu>
#include <QStyle>

using xinsight::core::project::FileEntry;

namespace {
constexpr int kRelativePathRole = Qt::UserRole + 1;
constexpr int kIsDirectoryRole = Qt::UserRole + 2;
} // namespace

ProjectTreeView::ProjectTreeView(QWidget *parent) : QTreeWidget(parent) {
    setHeaderHidden(true);
    setColumnCount(1);
    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QTreeWidget::itemActivated, this, &ProjectTreeView::onItemActivated);
    connect(this, &QTreeWidget::customContextMenuRequested, this, &ProjectTreeView::onContextMenuRequested);
}

void ProjectTreeView::populate(const std::vector<FileEntry> &entries) {
    clear();

    // scanDirectory() (xinsight-core) guarantees entries are sorted with
    // each directory preceding its own descendants, so a single pass with
    // a path->item map (no back-patching) is enough to build the tree.
    QHash<QString, QTreeWidgetItem *> pathToItem;

    const QIcon dirIcon = style()->standardIcon(QStyle::SP_DirIcon);
    const QIcon fileIcon = style()->standardIcon(QStyle::SP_FileIcon);

    for (const auto &entry : entries) {
        QString relativePath = QString::fromStdString(entry.relativePath.generic_string());
        QString name = QString::fromStdString(entry.relativePath.filename().generic_string());

        QString parentPath = QString::fromStdString(entry.relativePath.parent_path().generic_string());
        QTreeWidgetItem *parentItem = parentPath.isEmpty() ? nullptr : pathToItem.value(parentPath, nullptr);

        auto *item = new QTreeWidgetItem();
        item->setText(0, name);
        item->setIcon(0, entry.isDirectory ? dirIcon : fileIcon);
        item->setData(0, kRelativePathRole, relativePath);
        item->setData(0, kIsDirectoryRole, entry.isDirectory);
        if (entry.exceedsSizeLimit) {
            item->setToolTip(0, tr("File exceeds the indexable size limit; opened read-only, unparsed."));
        }

        if (parentItem != nullptr) {
            parentItem->addChild(item);
        } else {
            addTopLevelItem(item);
        }

        if (entry.isDirectory) {
            pathToItem.insert(relativePath, item);
        }
    }
}

void ProjectTreeView::onItemActivated(QTreeWidgetItem *item, int /*column*/) {
    if (item == nullptr) return;
    if (item->data(0, kIsDirectoryRole).toBool()) return;
    emit fileActivated(item->data(0, kRelativePathRole).toString());
}

void ProjectTreeView::onContextMenuRequested(const QPoint &pos) {
    QTreeWidgetItem *item = itemAt(pos);

    QString parentRelativeDir;
    if (item != nullptr) {
        parentRelativeDir = item->data(0, kIsDirectoryRole).toBool()
                                 ? item->data(0, kRelativePathRole).toString()
                                 : QFileInfo(item->data(0, kRelativePathRole).toString()).path();
        if (parentRelativeDir == QStringLiteral(".")) parentRelativeDir.clear();
    }

    QMenu menu(this);
    QAction *newFileAction = menu.addAction(tr("New File..."));
    QAction *newFolderAction = menu.addAction(tr("New Folder..."));

    QAction *chosen = menu.exec(viewport()->mapToGlobal(pos));
    if (chosen == newFileAction) {
        emit newFileRequested(parentRelativeDir);
    } else if (chosen == newFolderAction) {
        emit newFolderRequested(parentRelativeDir);
    }
}
