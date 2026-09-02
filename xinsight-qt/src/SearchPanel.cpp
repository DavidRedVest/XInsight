#include "SearchPanel.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

using xinsight::core::search::SearchOptions;
using xinsight::core::search::SearchResult;

namespace {
constexpr int kPathRole = Qt::UserRole + 1;
constexpr int kLineRole = Qt::UserRole + 2;
constexpr int kColumnRole = Qt::UserRole + 3;
constexpr int kIsFileGroupRole = Qt::UserRole + 4;
} // namespace

SearchPanel::SearchPanel(QWidget *parent) : QWidget(parent) {
    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(4, 4, 4, 4);

    auto *queryRow = new QHBoxLayout();
    queryEdit_ = new QLineEdit(this);
    queryEdit_->setPlaceholderText(tr("Search project..."));
    connect(queryEdit_, &QLineEdit::returnPressed, this, &SearchPanel::onSearchTriggered);
    queryRow->addWidget(queryEdit_, 1);

    searchButton_ = new QPushButton(tr("Search"), this);
    connect(searchButton_, &QPushButton::clicked, this, &SearchPanel::onSearchTriggered);
    queryRow->addWidget(searchButton_);

    cancelButton_ = new QPushButton(tr("Cancel"), this);
    cancelButton_->setEnabled(false);
    connect(cancelButton_, &QPushButton::clicked, this, [this]() { emit cancelRequested(); });
    queryRow->addWidget(cancelButton_);

    rootLayout->addLayout(queryRow);

    auto *optionsRow = new QHBoxLayout();
    caseSensitiveCheck_ = new QCheckBox(tr("Case sensitive"), this);
    wholeWordCheck_ = new QCheckBox(tr("Whole word"), this);
    regexCheck_ = new QCheckBox(tr("Regex"), this);
    gitignoreCheck_ = new QCheckBox(tr("Respect .gitignore"), this);
    gitignoreCheck_->setChecked(true); // PRD 3.1 default
    optionsRow->addWidget(caseSensitiveCheck_);
    optionsRow->addWidget(wholeWordCheck_);
    optionsRow->addWidget(regexCheck_);
    optionsRow->addWidget(gitignoreCheck_);
    optionsRow->addStretch(1);
    rootLayout->addLayout(optionsRow);

    resultsTree_ = new QTreeWidget(this);
    resultsTree_->setHeaderHidden(true);
    resultsTree_->setColumnCount(1);
    connect(resultsTree_, &QTreeWidget::itemActivated, this, &SearchPanel::onItemActivated);
    rootLayout->addWidget(resultsTree_, 1);

    statusLabel_ = new QLabel(this);
    rootLayout->addWidget(statusLabel_);
}

void SearchPanel::focusQuery() {
    queryEdit_->setFocus();
    queryEdit_->selectAll();
}

void SearchPanel::onSearchTriggered() {
    QString query = queryEdit_->text();
    if (query.isEmpty()) return;

    clearResults();
    searchInProgress_ = true;
    searchButton_->setEnabled(false);
    cancelButton_->setEnabled(true);
    statusLabel_->setText(tr("Searching..."));

    SearchOptions options;
    options.caseSensitive = caseSensitiveCheck_->isChecked();
    options.wholeWord = wholeWordCheck_->isChecked();
    options.useRegex = regexCheck_->isChecked();
    options.respectGitignore = gitignoreCheck_->isChecked();

    emit searchRequested(query, options);
}

void SearchPanel::clearResults() {
    resultsTree_->clear();
    fileGroups_.clear();
    matchCount_ = 0;
    statusLabel_->clear();
}

QTreeWidgetItem *SearchPanel::fileGroupItem(const QString &path) {
    auto it = fileGroups_.find(path);
    if (it != fileGroups_.end()) return it.value();

    auto *item = new QTreeWidgetItem();
    item->setText(0, path);
    item->setData(0, kIsFileGroupRole, true);
    item->setExpanded(true);
    resultsTree_->addTopLevelItem(item);
    fileGroups_.insert(path, item);
    return item;
}

void SearchPanel::appendResults(const std::vector<SearchResult> &results) {
    for (const SearchResult &result : results) {
        QString path = QString::fromStdString(result.path);
        QTreeWidgetItem *group = fileGroupItem(path);

        int column = result.submatches.empty() ? 0 : static_cast<int>(result.submatches.front().startCol);

        auto *child = new QTreeWidgetItem();
        child->setText(0, tr("L%1: %2").arg(result.line).arg(QString::fromStdString(result.preview).trimmed()));
        child->setData(0, kPathRole, path);
        child->setData(0, kLineRole, static_cast<int>(result.line));
        child->setData(0, kColumnRole, column);
        group->addChild(child);

        ++matchCount_;
    }

    if (!results.empty()) {
        statusLabel_->setText(tr("%1 matches so far...").arg(matchCount_));
    }
}

void SearchPanel::searchFinished(size_t totalMatches, bool wasCancelled) {
    searchInProgress_ = false;
    searchButton_->setEnabled(true);
    cancelButton_->setEnabled(false);

    statusLabel_->setText(wasCancelled ? tr("Cancelled -- %1 matches found").arg(totalMatches)
                                        : tr("%1 matches").arg(totalMatches));
}

void SearchPanel::onItemActivated(QTreeWidgetItem *item, int /*column*/) {
    if (item == nullptr || item->data(0, kIsFileGroupRole).toBool()) return;

    emit resultActivated(item->data(0, kPathRole).toString(), item->data(0, kLineRole).toInt(),
                          item->data(0, kColumnRole).toInt());
}
