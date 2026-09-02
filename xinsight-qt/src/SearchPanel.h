#pragma once

#include <QHash>
#include <QWidget>
#include <vector>

#include "xinsight/core/search/SearchEngine.h"

class QLineEdit;
class QCheckBox;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
class QLabel;

// Unified text-search results panel (PRD 3.1/3.3). Pure rendering + intent
// forwarding: never touches ripgrep itself -- MainWindow owns the
// SearchEngine and pushes results in via appendResults()/searchFinished().
class SearchPanel final : public QWidget {
    Q_OBJECT

public:
    explicit SearchPanel(QWidget *parent = nullptr);

    void focusQuery();
    void clearResults();
    void appendResults(const std::vector<xinsight::core::search::SearchResult> &results);
    void searchFinished(size_t totalMatches, bool wasCancelled);

signals:
    void searchRequested(QString query, xinsight::core::search::SearchOptions options);
    void cancelRequested();
    // `startByte` is an absolute byte offset into the file (already
    // resolved from ripgrep's line+column by the caller wiring this up).
    void resultActivated(QString absolutePath, int line, int column);

private:
    void onSearchTriggered();
    void onItemActivated(QTreeWidgetItem *item, int column);
    QTreeWidgetItem *fileGroupItem(const QString &path);

    QLineEdit *queryEdit_ = nullptr;
    QCheckBox *caseSensitiveCheck_ = nullptr;
    QCheckBox *wholeWordCheck_ = nullptr;
    QCheckBox *regexCheck_ = nullptr;
    QCheckBox *gitignoreCheck_ = nullptr;
    QPushButton *searchButton_ = nullptr;
    QPushButton *cancelButton_ = nullptr;
    QTreeWidget *resultsTree_ = nullptr;
    QLabel *statusLabel_ = nullptr;

    QHash<QString, QTreeWidgetItem *> fileGroups_;
    size_t matchCount_ = 0;
    bool searchInProgress_ = false;
};
