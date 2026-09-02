#pragma once

#include <QListWidget>
#include <vector>

#include "xinsight/core/intel/TreeSitterEngine.h"

// Flat, line-ordered outline list for the current editor's symbols. Pure
// rendering (PRD 5.4/5.5) -- EditorView already sorts by startByte, this
// widget just renders what it's given.
class OutlineView final : public QListWidget {
    Q_OBJECT

public:
    explicit OutlineView(QWidget *parent = nullptr);

    void populate(const std::vector<xinsight::core::intel::Symbol> &symbols);

signals:
    void symbolActivated(int byteOffset);

private:
    void onItemActivated(QListWidgetItem *item);
};
