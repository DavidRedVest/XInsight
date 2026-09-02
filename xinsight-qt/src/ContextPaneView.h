#pragma once

#include <QWidget>

#include "xinsight/core/context/ContextEngine.h"

class QLabel;
class QPlainTextEdit;
class QPushButton;

// PRD 2.1's ambient context pane: pure rendering of whatever ContextEngine
// last resolved -- MainWindow wires ContextEngine::setOnContext(...) to
// showResult(), and this widget's drillDownRequested signal back through
// MainWindow's jumpTo()/NavigationEngine (PRD 5.4/5.5: GUI only renders
// core data, never queries the index directly).
class ContextPaneView final : public QWidget {
    Q_OBJECT

public:
    explicit ContextPaneView(QWidget *parent = nullptr);

    void showResult(const xinsight::core::context::ContextResult &result);

signals:
    // Emitted on double-click in the snippet (PRD 2.1 "点击进入
    // (drill-down)", matching SearchPanel's existing double-click-to-jump
    // convention). `column` is always 0 (byte column of the definition
    // line's start) -- good enough to land on the right line.
    void drillDownRequested(QString absolutePath, int line, int column);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void updateDisplay();
    void showCandidate(size_t index);

    QLabel *headerLabel_ = nullptr;
    QPlainTextEdit *snippetView_ = nullptr;
    QLabel *candidateLabel_ = nullptr;
    QPushButton *prevButton_ = nullptr;
    QPushButton *nextButton_ = nullptr;

    xinsight::core::context::ContextResult result_;
    size_t currentIndex_ = 0;
};
