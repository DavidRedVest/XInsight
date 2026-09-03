#include "EditorView.h"

#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFont>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <Scintilla.h>
#include <SciLexer.h>
#include <algorithm>
#include <vector>

#include "DocumentRegistry.h"
#include "HighlightStyles.h"
#include "PathUtil.h"

using namespace xinsight::core::intel;
namespace enc = xinsight::core::encoding;
namespace hl = xinsight::qt::highlights;
namespace theme = xinsight::core::theme;

EditorView::EditorView(TreeSitterEngine &engine, DocumentRegistry &registry, theme::ThemeManager &themeManager,
                        CodeIntelligence &codeIntelligence, xinsight::core::context::ContextEngine &contextEngine,
                        QWidget *parent)
    : QsciScintilla(parent), engine_(engine), registry_(registry), themeManager_(themeManager),
      codeIntelligence_(codeIntelligence), contextEngine_(contextEngine) {
    // Container lexing: we drive all styling/folding ourselves from
    // tree-sitter results rather than through a QsciLexer (PRD 4.3).
    SendScintilla(SCI_SETLEXER, static_cast<unsigned long>(SCLEX_CONTAINER));

    setFolding(QsciScintilla::BoxedTreeFoldStyle);
    setUtf8(true);

    // Scintilla's default key map binds Ctrl+T (which QScintilla maps
    // Cmd+T to on macOS) to SCI_LINETRANSPOSE ("swap current line with
    // previous"), silently swapping two lines instead of letting
    // MainWindow's "Go to Symbol in Workspace" QAction shortcut fire.
    // Freeing it here is a one-time keymap fix, not a per-theme style
    // concern, so it lives in the constructor rather than initStyles().
    SendScintilla(SCI_CLEARCMDKEY, static_cast<unsigned long>('T' | (SCMOD_CTRL << 16)));

    initStyles();

    connect(this, &QsciScintilla::textChanged, this, &EditorView::onTextChanged);
    connect(this, &QsciScintilla::modificationChanged, this, [this](bool) { emit tabLabelChanged(); });
    connect(this, &QsciScintilla::cursorPositionChanged, this, &EditorView::onCursorPositionChanged);
}

EditorView::~EditorView() {
    if (!filePath_.isEmpty()) registry_.release(std::filesystem::path(filePath_.toStdString()));
}

void EditorView::initStyles() {
    const theme::Theme &current = themeManager_.currentTheme();
    QFont font(QStringLiteral("Menlo"), 12);
    QColor foreground = hl::toQColor(current.editor.foreground);
    QColor background = hl::toQColor(current.editor.background);

    SendScintilla(SCI_STYLESETFONT, static_cast<unsigned long>(STYLE_DEFAULT), font.family().toUtf8().constData());
    SendScintilla(SCI_STYLESETSIZE, static_cast<unsigned long>(STYLE_DEFAULT), font.pointSize());
    SendScintilla(SCI_STYLESETFORE, static_cast<unsigned long>(STYLE_DEFAULT), foreground);
    SendScintilla(SCI_STYLESETBACK, static_cast<unsigned long>(STYLE_DEFAULT), background);
    SendScintilla(SCI_STYLECLEARALL);

    setColor(foreground);
    setPaper(background);
    setCaretForegroundColor(foreground);
    setCaretLineVisible(true);
    setCaretLineBackgroundColor(hl::toQColor(current.editor.currentLine));
    setSelectionBackgroundColor(hl::toQColor(current.editor.selectionBackground));
    setMarginsBackgroundColor(hl::toQColor(current.editor.lineNumberBackground));
    setMarginsForegroundColor(hl::toQColor(current.editor.lineNumberForeground));
    setMarginLineNumbers(0, true);
    setMarginWidth(0, QStringLiteral("00000"));

    SendScintilla(SCI_STYLESETFORE, static_cast<unsigned long>(hl::kDefaultStyle), foreground);
    for (const QString &capture : hl::knownCaptures()) {
        int style = hl::styleNumberForCapture(capture);
        SendScintilla(SCI_STYLESETFORE, static_cast<unsigned long>(style), hl::colorForCapture(current, capture));
    }
}

void EditorView::applyTheme() {
    initStyles();
    if (document_) applyHighlights();
}

bool EditorView::loadFromDisk(const std::filesystem::path &canonicalPath) {
    QFile file(QString::fromStdString(canonicalPath.string()));
    if (!file.open(QIODevice::ReadOnly)) return false;

    QByteArray bytes = file.readAll();
    std::vector<uint8_t> raw(bytes.begin(), bytes.end());
    enc::DecodeResult decoded = enc::decode(raw);

    encoding_ = decoded.encoding;
    lineEnding_ = decoded.lineEnding;
    hadBom_ = decoded.hadBom;

    suppressTextChanged_ = true;
    setText(QString::fromStdString(decoded.utf8Text));
    suppressTextChanged_ = false;
    SendScintilla(SCI_SETSAVEPOINT); // freshly loaded content is never dirty

    return true;
}

bool EditorView::performReload() {
    if (!document_) return false;
    Language language = document_->language();
    if (!loadFromDisk(std::filesystem::path(filePath_.toStdString()))) return false;
    document_ = engine_.parse(language, text().toStdString());
    reparseAndRestyle();
    return true;
}

bool EditorView::openFile(const QString &absolutePath) {
    QFileInfo info(absolutePath);
    auto language = languageForExtension(info.suffix().toStdString());
    if (!language) return false;

    std::filesystem::path canonicalPath = canonicalOf(absolutePath);

    if (DocumentRegistry::Entry *existing = registry_.acquireExisting(canonicalPath)) {
        setDocument(existing->document);
        encoding_ = existing->encoding;
        lineEnding_ = existing->lineEnding;
        hadBom_ = existing->hadBom;
    } else {
        if (!loadFromDisk(canonicalPath)) return false;
        registry_.registerNew(canonicalPath, document(), encoding_, lineEnding_, hadBom_);
    }

    filePath_ = QString::fromStdString(canonicalPath.string());
    document_ = engine_.parse(*language, text().toStdString());
    reparseAndRestyle();
    watchCurrentFile();

    // No-op when clangd isn't configured (PRD 8.7's buffer-sync obligation
    // only applies once the optional overlay is active).
    codeIntelligence_.notifyFileOpened(filePath_.toStdString(), *language == Language::C ? "c" : "cpp",
                                        text().toStdString());
    return true;
}

void EditorView::newUntitled() {
    filePath_.clear();
    encoding_ = enc::TextEncoding::Utf8;
    lineEnding_ = enc::LineEnding::Lf;
    hadBom_ = false;
    document_ = engine_.parse(Language::Cpp, std::string());
    reparseAndRestyle();
}

QString EditorView::displayName() const {
    QString name = isUntitled() ? tr("untitled") : QFileInfo(filePath_).fileName();
    if (isDirty()) name += QStringLiteral(" •");
    return name;
}

bool EditorView::writeToDisk(const std::filesystem::path &path) {
    std::vector<uint8_t> raw = enc::encode(text().toStdString(), encoding_, lineEnding_, hadBom_);

    QFile file(QString::fromStdString(path.string()));
    if (!file.open(QIODevice::WriteOnly)) return false;

    justSaved_ = true;
    qint64 written = file.write(reinterpret_cast<const char *>(raw.data()), static_cast<qint64>(raw.size()));
    file.close();
    if (written != static_cast<qint64>(raw.size())) {
        justSaved_ = false;
        return false;
    }

    SendScintilla(SCI_SETSAVEPOINT);

    // PRD 8.7: index updates are deferred to save, not run on every
    // keystroke. `document_` is already current -- every edit feeds
    // tree-sitter's incremental reparse live, for highlighting -- so no
    // extra reparse is needed here, just handing the already-fresh parse
    // to the index.
    if (document_) codeIntelligence_.updateFileIndex(path.string(), *document_);

    // Same deferred-to-save cadence for the optional clangd overlay (no-op
    // if it isn't configured): didChange brings its buffer view current,
    // then didSave lets it re-run any save-triggered diagnostics.
    codeIntelligence_.notifyFileChanged(path.string(), text().toStdString());
    codeIntelligence_.notifyFileSaved(path.string());

    return true;
}

bool EditorView::save() {
    if (isUntitled()) {
        QString path = QFileDialog::getSaveFileName(this, tr("Save As"));
        if (path.isEmpty()) return false;
        return saveAs(path);
    }
    return writeToDisk(std::filesystem::path(filePath_.toStdString()));
}

bool EditorView::saveAs(const QString &absolutePath) {
    std::filesystem::path canonicalPath = canonicalOf(absolutePath);
    if (!writeToDisk(canonicalPath)) return false;

    if (!filePath_.isEmpty()) registry_.release(std::filesystem::path(filePath_.toStdString()));
    // Simplification (documented, not covered by PRD's acceptance
    // criteria): if another pane already had this exact path open, this
    // orphans that registration rather than merging documents. Save As
    // repoints only *this* view.
    registry_.registerNew(canonicalPath, document(), encoding_, lineEnding_, hadBom_);

    filePath_ = QString::fromStdString(canonicalPath.string());
    watchCurrentFile();
    emit tabLabelChanged();
    return true;
}

void EditorView::watchCurrentFile() {
    if (watcher_ == nullptr) {
        watcher_ = new QFileSystemWatcher(this);
        connect(watcher_, &QFileSystemWatcher::fileChanged, this, &EditorView::onExternalFileChanged);
    }
    if (!watcher_->files().isEmpty()) watcher_->removePaths(watcher_->files());
    if (!filePath_.isEmpty()) watcher_->addPath(filePath_);
}

void EditorView::onExternalFileChanged(const QString & /*path*/) {
    if (justSaved_) {
        justSaved_ = false;
        watchCurrentFile(); // re-arm: some save patterns (replace-on-write) drop the watch
        return;
    }

    if (!isDirty()) {
        auto choice = QMessageBox::question(this, tr("File Changed"),
                                             tr("%1 was changed outside XInsight. Reload it?").arg(displayName()),
                                             QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (choice == QMessageBox::Yes) performReload();
        watchCurrentFile();
        return;
    }

    QMessageBox box(this);
    box.setWindowTitle(tr("File Changed"));
    box.setText(tr("%1 was changed outside XInsight, and you have unsaved changes here.").arg(displayName()));
    box.addButton(tr("Keep Local"), QMessageBox::RejectRole);
    QAbstractButton *discardLocal = box.addButton(tr("Discard Local (Reload)"), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.exec();

    if (box.clickedButton() == discardLocal) performReload();
    watchCurrentFile();
}

void EditorView::onTextChanged() {
    if (suppressTextChanged_ || !document_) return;

    // M1 does a full reparse per keystroke rather than TreeSitterEngine's
    // incremental applyEdit() -- that needs precise byte/line/col offsets
    // from Scintilla's modification signals. Tree-sitter full reparse is
    // fast enough that this is a legitimate starting point, not just a
    // placeholder.
    document_ = engine_.parse(document_->language(), text().toStdString());
    reparseAndRestyle();
}

void EditorView::reparseAndRestyle() {
    if (!document_ || !document_->valid()) return;
    applyHighlights();
    applyFolds();
    applyOutline();
    emit contentReparsed();
}

void EditorView::applyHighlights() {
    std::string_view source = document_->source();
    std::vector<HighlightSpan> spans = engine_.highlights(*document_);

    std::vector<unsigned char> styleOfByte(source.size(), static_cast<unsigned char>(hl::kDefaultStyle));

    // Spans are appended in query-evaluation order (c/highlights.scm then
    // cpp/highlights.scm for C++ files); later entries win on overlapping
    // byte ranges, so a plain forward overwrite gives the right result.
    for (const HighlightSpan &span : spans) {
        int style = hl::styleNumberForCapture(QString::fromStdString(span.capture));
        for (uint32_t b = span.startByte; b < span.endByte && b < styleOfByte.size(); ++b) {
            styleOfByte[b] = static_cast<unsigned char>(style);
        }
    }

    // Run-length encode so we issue one Scintilla call per contiguous run
    // of the same style rather than one per byte.
    size_t i = 0;
    while (i < styleOfByte.size()) {
        size_t runStart = i;
        unsigned char style = styleOfByte[i];
        while (i < styleOfByte.size() && styleOfByte[i] == style) ++i;

        SendScintilla(SCI_STARTSTYLING, static_cast<unsigned long>(runStart), 0L);
        SendScintilla(SCI_SETSTYLING, static_cast<unsigned long>(i - runStart), static_cast<long>(style));
    }
}

void EditorView::applyFolds() {
    int lineCount = lines();
    if (lineCount <= 0) return;

    std::vector<int> depthDelta(static_cast<size_t>(lineCount) + 1, 0);
    std::vector<bool> isHeader(static_cast<size_t>(lineCount), false);

    for (const FoldRange &range : engine_.folds(*document_)) {
        int start = static_cast<int>(range.startRow);
        int end = static_cast<int>(range.endRow);
        if (start < 0 || start >= lineCount) continue;
        end = std::min(end, lineCount - 1);

        // Body lines are start+1..end inclusive; the header line itself
        // (start) stays at the surrounding depth and just gets the header
        // flag, per Scintilla's fold-level convention.
        depthDelta[start + 1] += 1;
        if (end + 1 <= lineCount) depthDelta[end + 1] -= 1;
        isHeader[static_cast<size_t>(start)] = true;
    }

    int depth = 0;
    for (int line = 0; line < lineCount; ++line) {
        depth += depthDelta[static_cast<size_t>(line)];
        int level = SC_FOLDLEVELBASE + depth;
        if (isHeader[static_cast<size_t>(line)]) level |= SC_FOLDLEVELHEADERFLAG;
        SendScintilla(SCI_SETFOLDLEVEL, static_cast<unsigned long>(line), static_cast<long>(level));
    }
}

void EditorView::applyOutline() {
    outline_ = engine_.outline(*document_);
    std::sort(outline_.begin(), outline_.end(),
              [](const Symbol &a, const Symbol &b) { return a.startByte < b.startByte; });
}

void EditorView::gotoByteOffset(int byteOffset) {
    SendScintilla(SCI_GOTOPOS, static_cast<unsigned long>(byteOffset), 0L);
    setFocus();
}

void EditorView::gotoLineAndColumn(int line1Based, int byteColumn) {
    long linePos = SendScintilla(SCI_POSITIONFROMLINE, static_cast<unsigned long>(std::max(0, line1Based - 1)));
    gotoByteOffset(static_cast<int>(linePos) + byteColumn);
}

int EditorView::currentByteOffset() const {
    return static_cast<int>(SendScintilla(SCI_GETCURRENTPOS));
}

EditorView::CursorLocation EditorView::currentCursorLocation() const {
    long pos = SendScintilla(SCI_GETCURRENTPOS);
    long row = SendScintilla(SCI_LINEFROMPOSITION, static_cast<unsigned long>(pos));
    long linePos = SendScintilla(SCI_POSITIONFROMLINE, static_cast<unsigned long>(row));

    QString lineText = text(static_cast<int>(row));
    while (lineText.endsWith(QLatin1Char('\n')) || lineText.endsWith(QLatin1Char('\r'))) lineText.chop(1);

    return CursorLocation{static_cast<int>(row), static_cast<int>(pos - linePos), lineText};
}

QString EditorView::wordUnderCursor() const {
    int line = 0, index = 0;
    getCursorPosition(&line, &index);
    return wordAtLineIndex(line, index);
}

void EditorView::mousePressEvent(QMouseEvent *event) {
    // Cmd+click-to-jump (Windows/Linux IDE convention is Ctrl+click; Qt
    // maps Qt::ControlModifier to the physical Cmd key on macOS, so this
    // one check is already "Cmd" here without any #ifdef). Moves the caret
    // onto the clicked word first, then defers to MainWindow's existing
    // F12 handler via the signal -- it reads the word/position back off
    // this same caret, so no jump-to-definition logic is duplicated here.
    if (event->button() == Qt::LeftButton && (event->modifiers() & Qt::ControlModifier)) {
        QPoint point = event->position().toPoint();
        if (!wordAtPoint(point).isEmpty()) {
            long pos = SendScintilla(SCI_POSITIONFROMPOINT, static_cast<long>(point.x()), static_cast<long>(point.y()));
            if (pos >= 0) {
                SendScintilla(SCI_GOTOPOS, static_cast<unsigned long>(pos));
                setFocus();
                emit gotoDefinitionRequested();
                return;
            }
        }
    }
    QsciScintilla::mousePressEvent(event);
}

void EditorView::mouseMoveEvent(QMouseEvent *event) {
    QsciScintilla::mouseMoveEvent(event);

    // Hover affordance while Cmd is held, matching wordAtPoint()'s same
    // "is there actually a clickable identifier here" check mousePressEvent
    // uses -- so the cursor only changes where a click would actually jump.
    bool overWord = (event->modifiers() & Qt::ControlModifier) &&
                     !wordAtPoint(event->position().toPoint()).isEmpty();
    if (overWord) {
        viewport()->setCursor(Qt::PointingHandCursor);
    } else {
        viewport()->unsetCursor();
    }
}

void EditorView::onCursorPositionChanged(int /*line*/, int /*index*/) { updateContextForCursor(); }

void EditorView::updateContextForCursor() {
    if (!document_) {
        contextEngine_.onCursorMoved(std::string(), std::string());
        return;
    }

    auto ctx = engine_.identifierAtByteOffset(*document_, static_cast<uint32_t>(currentByteOffset()));
    contextEngine_.onCursorMoved(ctx ? ctx->lookupName : std::string(), filePath_.toStdString());
}
