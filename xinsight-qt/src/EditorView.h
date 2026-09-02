#pragma once

#include <Qsci/qsciscintilla.h>
#include <filesystem>
#include <optional>
#include <vector>

#include "xinsight/core/encoding/TextCodec.h"
#include "xinsight/core/intel/CodeIntelligence.h"
#include "xinsight/core/intel/TreeSitterEngine.h"
#include "xinsight/core/theme/Theme.h"

class DocumentRegistry;
class QFileSystemWatcher;

// A single tree-sitter-backed editor view. Wraps QsciScintilla and drives
// its styling/folding directly from xinsight-core's TreeSitterEngine
// (PRD 4.3: "高亮由 tree-sitter 驱动... GUI 不直调 tree-sitter" -- this class
// is the one place in xinsight-qt allowed to do so, since TreeSitterEngine
// itself has no GUI dependency and EditorView is exactly the adapter layer
// PRD 5 describes).
//
// Bound to at most one file for its lifetime (or none, for an untitled
// buffer): openFile()/newUntitled() is called exactly once, right after
// construction. If the file is already open in another EditorView, this
// one attaches to the same shared QsciDocument (PRD 8.7) via `registry`.
class EditorView final : public QsciScintilla {
    Q_OBJECT

public:
    // `engine`, `registry`, `themeManager` and `codeIntelligence` must
    // outlive this EditorView; all are shared across all editor views
    // (query compilation happens once, same-file views share one document,
    // all views repaint from the same current theme, and every view's
    // successful save feeds the same workspace symbol index, respectively).
    explicit EditorView(xinsight::core::intel::TreeSitterEngine &engine, DocumentRegistry &registry,
                         xinsight::core::theme::ThemeManager &themeManager,
                         xinsight::core::intel::CodeIntelligence &codeIntelligence, QWidget *parent = nullptr);
    ~EditorView() override;

    // Re-applies themeManager's current theme to this view's chrome and
    // highlight colors, then re-highlights the current content. Called by
    // MainWindow on every open EditorView when the user switches themes.
    void applyTheme();

    // Detects encoding/line-ending, loads `absolutePath` (attaching to an
    // already-open shared document if another view has this file open),
    // and does an initial parse + highlight + fold. Returns false if the
    // file couldn't be read or its extension isn't recognized as C/C++.
    bool openFile(const QString &absolutePath);

    // Sets up an empty, path-less buffer (PRD 2.3 "新建文件"); save() will
    // prompt for a location the first time.
    void newUntitled();

    bool isUntitled() const { return filePath_.isEmpty(); }
    const QString &filePath() const { return filePath_; }
    QString displayName() const;

    bool isDirty() const { return isModified(); }

    // Writes the current buffer back using the file's original
    // encoding/line-ending (PRD 2.3: never silently re-encode). Prompts
    // for a location first if isUntitled(). Returns false if the user
    // cancelled the prompt or the write failed.
    bool save();
    bool saveAs(const QString &absolutePath);

    const std::vector<xinsight::core::intel::Symbol> &outline() const { return outline_; }

    // Moves the cursor to a tree-sitter byte offset (as reported in
    // Symbol::startByte) and scrolls it into view.
    void gotoByteOffset(int byteOffset);

    // Moves the cursor to a 1-based line and a byte offset within that
    // line (ripgrep's submatch column convention) and scrolls it into view.
    void gotoLineAndColumn(int line1Based, int byteColumn);

    // Current cursor byte offset, for recording a NavigationEngine
    // "jumping from here" location before jumping elsewhere.
    int currentByteOffset() const;

    // The identifier-like word the cursor is currently inside/adjacent to
    // (Scintilla's own word-boundary notion), or an empty string. Source
    // for F12/Shift+F12's "look this up" target.
    QString wordUnderCursor() const;

    xinsight::core::encoding::TextEncoding encoding() const { return encoding_; }
    xinsight::core::encoding::LineEnding lineEnding() const { return lineEnding_; }

signals:
    // Fired after a reparse completes and outline()/highlights/folds are
    // up to date. No payload (Qt cross-thread-safe primitive types only);
    // listeners pull fresh state via outline().
    void contentReparsed();

    // Mirrors QsciScintilla::modificationChanged, plus fires once for the
    // path/title change on a successful save-as -- EditorPane listens to
    // this single signal to keep a tab's label (name + dirty marker) current.
    void tabLabelChanged();

private slots:
    void onTextChanged();
    void onExternalFileChanged(const QString &path);

private:
    void initStyles();
    void reparseAndRestyle();
    void applyHighlights();
    void applyFolds();
    void applyOutline();
    bool writeToDisk(const std::filesystem::path &path);
    bool loadFromDisk(const std::filesystem::path &canonicalPath);
    bool performReload();
    void watchCurrentFile();

    xinsight::core::intel::TreeSitterEngine &engine_;
    DocumentRegistry &registry_;
    xinsight::core::theme::ThemeManager &themeManager_;
    xinsight::core::intel::CodeIntelligence &codeIntelligence_;
    std::optional<xinsight::core::intel::ParsedDocument> document_;
    std::vector<xinsight::core::intel::Symbol> outline_;
    QString filePath_; // empty iff untitled
    xinsight::core::encoding::TextEncoding encoding_ = xinsight::core::encoding::TextEncoding::Utf8;
    xinsight::core::encoding::LineEnding lineEnding_ = xinsight::core::encoding::LineEnding::Lf;
    bool hadBom_ = false;
    bool suppressTextChanged_ = false;
    bool justSaved_ = false;
    QFileSystemWatcher *watcher_ = nullptr;
};
