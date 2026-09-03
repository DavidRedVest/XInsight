#pragma once

#include <QWidget>

class QLabel;

// PRD P0 #10 / 8.4's "clangd 配置诊断" panel: pure rendering of whatever
// status MainWindow tells it -- never touches CodeIntelligence or clangd
// directly (PRD 5.4/5.5). Explains, in both directions, that XInsight
// works fully without clangd (fast/tree-sitter mode) and what configuring
// a compile_commands.json upgrades.
class ClangdStatusView final : public QWidget {
    Q_OBJECT

public:
    explicit ClangdStatusView(QWidget *parent = nullptr);

    void showNotConfigured();
    void showStarting();
    void showRunning();
    void showFailed();

private:
    QLabel *statusLabel_ = nullptr;
    QLabel *detailLabel_ = nullptr;
};
