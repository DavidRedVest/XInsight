#include "ClangdStatusView.h"

#include <QLabel>
#include <QVBoxLayout>

ClangdStatusView::ClangdStatusView(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);

    statusLabel_ = new QLabel(this);
    statusLabel_->setStyleSheet(QStringLiteral("font-weight: bold;"));
    layout->addWidget(statusLabel_);

    detailLabel_ = new QLabel(this);
    detailLabel_->setWordWrap(true);
    layout->addWidget(detailLabel_);
    layout->addStretch(1);

    showNotConfigured();
}

void ClangdStatusView::showNotConfigured() {
    statusLabel_->setText(tr("○ Fast mode (tree-sitter)"));
    detailLabel_->setText(
        tr("No compile_commands.json found for this project. XInsight works fully in fast mode "
           "(name-based, zero-config). Add -DCMAKE_EXPORT_COMPILE_COMMANDS=ON to your CMake build "
           "(or use bear/compiledb for a Makefile project) and reopen the project to upgrade jump-to-"
           "definition, find-references, and workspace symbol search to precise, compiler-accurate results."));
}

void ClangdStatusView::showStarting() {
    statusLabel_->setText(tr("○ Starting clangd..."));
    detailLabel_->setText(
        tr("Found a compile_commands.json. Launching clangd -- results stay in fast mode until it's ready."));
}

void ClangdStatusView::showRunning() {
    statusLabel_->setText(tr("● Precise mode (clangd)"));
    detailLabel_->setText(
        tr("clangd is running. Jump-to-definition, find-references, and workspace symbol search upgrade to "
           "precise, compiler-accurate results once each file's translation unit finishes indexing -- until "
           "then (and for anything clangd can't resolve), results stay in fast mode."));
}

void ClangdStatusView::showFailed() {
    statusLabel_->setText(tr("○ Fast mode (clangd failed to start)"));
    detailLabel_->setText(
        tr("A compile_commands.json was found, but clangd could not be started. XInsight continues to work "
           "fully in fast mode (tree-sitter, name-based)."));
}
