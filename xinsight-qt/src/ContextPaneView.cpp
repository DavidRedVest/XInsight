#include "ContextPaneView.h"

#include <QEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextCursor>
#include <QVBoxLayout>
#include <filesystem>

using xinsight::core::context::ContextResult;

ContextPaneView::ContextPaneView(QWidget *parent) : QWidget(parent) {
    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(4, 4, 4, 4);

    auto *headerRow = new QHBoxLayout();
    headerLabel_ = new QLabel(tr("No context"), this);
    headerLabel_->setStyleSheet(QStringLiteral("font-weight: bold;"));
    headerRow->addWidget(headerLabel_, 1);

    prevButton_ = new QPushButton(QStringLiteral("<"), this);
    prevButton_->setFixedWidth(28);
    prevButton_->setEnabled(false);
    prevButton_->setToolTip(tr("Previous candidate"));
    connect(prevButton_, &QPushButton::clicked, this, [this]() {
        if (currentIndex_ > 0) showCandidate(currentIndex_ - 1);
    });
    headerRow->addWidget(prevButton_);

    candidateLabel_ = new QLabel(this);
    headerRow->addWidget(candidateLabel_);

    nextButton_ = new QPushButton(QStringLiteral(">"), this);
    nextButton_->setFixedWidth(28);
    nextButton_->setEnabled(false);
    nextButton_->setToolTip(tr("Next candidate"));
    connect(nextButton_, &QPushButton::clicked, this, [this]() {
        if (currentIndex_ + 1 < result_.candidates.size()) showCandidate(currentIndex_ + 1);
    });
    headerRow->addWidget(nextButton_);

    rootLayout->addLayout(headerRow);

    snippetView_ = new QPlainTextEdit(this);
    snippetView_->setReadOnly(true);
    snippetView_->setFont(QFont(QStringLiteral("Menlo"), 12));
    snippetView_->setLineWrapMode(QPlainTextEdit::NoWrap);
    snippetView_->viewport()->installEventFilter(this);
    rootLayout->addWidget(snippetView_, 1);
}

void ContextPaneView::showResult(const ContextResult &result) {
    result_ = result;
    currentIndex_ = 0;
    updateDisplay();
}

void ContextPaneView::updateDisplay() {
    if (result_.candidates.empty()) {
        headerLabel_->setText(result_.queriedName.empty()
                                   ? tr("No context")
                                   : tr("No definition for '%1'").arg(QString::fromStdString(result_.queriedName)));
        snippetView_->clear();
        candidateLabel_->clear();
        prevButton_->setEnabled(false);
        nextButton_->setEnabled(false);
        return;
    }
    showCandidate(0);
}

void ContextPaneView::showCandidate(size_t index) {
    if (index >= result_.candidates.size()) return;
    currentIndex_ = index;
    const auto &candidate = result_.candidates[index];

    QString fileName = QString::fromStdString(std::filesystem::path(candidate.file).filename().string());
    headerLabel_->setText(tr("%1:%2  %3")
                               .arg(fileName)
                               .arg(candidate.highlightRow + 1)
                               .arg(QString::fromStdString(candidate.signature).trimmed()));

    snippetView_->setPlainText(QString::fromStdString(candidate.snippet));

    // Highlight the definition line within the snippet.
    QTextCursor cursor(snippetView_->document());
    cursor.movePosition(QTextCursor::Start);
    cursor.movePosition(QTextCursor::Down, QTextCursor::MoveAnchor,
                         static_cast<int>(candidate.highlightRow - candidate.snippetStartRow));
    QTextEdit::ExtraSelection selection;
    selection.format.setBackground(QColor(255, 235, 59, 70));
    selection.format.setProperty(QTextFormat::FullWidthSelection, true);
    selection.cursor = cursor;
    snippetView_->setExtraSelections({selection});

    if (result_.candidates.size() > 1) {
        candidateLabel_->setText(tr("%1/%2").arg(index + 1).arg(result_.candidates.size()));
        prevButton_->setEnabled(index > 0);
        nextButton_->setEnabled(index + 1 < result_.candidates.size());
    } else {
        candidateLabel_->clear();
        prevButton_->setEnabled(false);
        nextButton_->setEnabled(false);
    }
}

bool ContextPaneView::eventFilter(QObject *watched, QEvent *event) {
    if (watched == snippetView_->viewport() && event->type() == QEvent::MouseButtonDblClick) {
        if (currentIndex_ < result_.candidates.size()) {
            const auto &candidate = result_.candidates[currentIndex_];
            emit drillDownRequested(QString::fromStdString(candidate.file), static_cast<int>(candidate.highlightRow) + 1,
                                     0);
        }
        return true;
    }
    return QWidget::eventFilter(watched, event);
}
