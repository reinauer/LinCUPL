#include "code_editor.h"
#include "../common/cupl_highlighter.h"
#include "font_settings.h"

#include <QApplication>
#include <QPainter>
#include <QPalette>
#include <QResizeEvent>
#include <QTextBlock>
#include <QToolButton>
#include <QWheelEvent>

namespace {

class LineNumberArea : public QWidget {
public:
    explicit LineNumberArea(CodeEditor *editor)
        : QWidget(editor), m_editor(editor) {}

    QSize sizeHint() const override {
        return QSize(m_editor->lineNumberAreaWidth(), 0);
    }

protected:
    void paintEvent(QPaintEvent *event) override {
        m_editor->lineNumberAreaPaintEvent(event);
    }

private:
    CodeEditor *m_editor;
};

// The font new editors start with; empty until Options > Font... or the
// saved font settings set it.
QFont &standardFontStore() {
    static QFont font;
    return font;
}
bool &standardFontSet() {
    static bool set = false;
    return set;
}

} // namespace

void CodeEditor::setStandardFont(const QFont &font) {
    standardFontStore() = font;
    standardFontSet() = true;
}

QFont CodeEditor::standardFont() {
    if (standardFontSet()) {
        return standardFontStore();
    }
    return FontSettings::defaultEditorFont();
}

CodeEditor::CodeEditor(QWidget *parent)
    : QPlainTextEdit(parent) {
    m_lineNumberArea = new LineNumberArea(this);

    setBaseFont(standardFont());
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setTabStopDistance(fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4.0);

    connect(this, &CodeEditor::blockCountChanged,
            this, &CodeEditor::updateLineNumberAreaWidth);
    connect(this, &CodeEditor::updateRequest,
            this, &CodeEditor::updateLineNumberArea);
    connect(this, &CodeEditor::cursorPositionChanged,
            this, &CodeEditor::updateExtraSelections);

    updateLineNumberAreaWidth(0);
    updateExtraSelections();
}

void CodeEditor::setBaseFont(const QFont &font) {
    m_baseFont = font;
    applyZoomedFont();
}

void CodeEditor::zoomBy(int steps) {
    m_zoomSteps += steps;
    applyZoomedFont();
}

// The base font enlarged or reduced by one point (or pixel) per zoom step,
// never below 4.
void CodeEditor::applyZoomedFont() {
    QFont f = m_baseFont;
    if (f.pointSizeF() > 0) {
        const qreal size = qMax<qreal>(4.0, f.pointSizeF() + m_zoomSteps);
        m_zoomSteps = qRound(size - m_baseFont.pointSizeF());
        f.setPointSizeF(size);
    } else if (f.pixelSize() > 0) {
        const int size = qMax(4, f.pixelSize() + m_zoomSteps);
        m_zoomSteps = size - m_baseFont.pixelSize();
        f.setPixelSize(size);
    }
    setFont(f);
}

void CodeEditor::changeEvent(QEvent *event) {
    QPlainTextEdit::changeEvent(event);
    if (event->type() == QEvent::FontChange) {
        setTabStopDistance(fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4.0);
        updateLineNumberAreaWidth(0);
        const QRect cr = contentsRect();
        m_lineNumberArea->setGeometry(QRect(cr.left(), cr.top(), lineNumberAreaWidth(), cr.height()));
        m_lineNumberArea->update();
    }
}

// Ctrl+wheel zooms editable and read-only editors alike, in the same steps
// as View > Zoom In / Zoom Out.
void CodeEditor::wheelEvent(QWheelEvent *event) {
    if (event->modifiers() & Qt::ControlModifier) {
        const int delta = event->angleDelta().y();
        if (delta != 0) {
            zoomBy(delta > 0 ? 1 : -1);
        }
        event->accept();
        return;
    }
    QPlainTextEdit::wheelEvent(event);
}

void CodeEditor::setFilePath(const QString &path) {
    m_filePath = path;
    setHighlightLanguage(CuplHighlighter::languageForFile(path));
}

void CodeEditor::setCompanionOutput(bool isCompanion) {
    m_isCompanionOutput = isCompanion;
    setReadOnly(isCompanion);
}

void CodeEditor::setHighlightLanguage(CuplHighlighter::Language language) {
    if (language == CuplHighlighter::Language::None) {
        delete m_highlighter;
        m_highlighter = nullptr;
    } else if (m_highlighter == nullptr) {
        m_highlighter = new CuplHighlighter(document(), language);
    } else {
        m_highlighter->setLanguage(language);
    }
}

void CodeEditor::setDiagnosticLines(const QSet<int> &errorLines, const QSet<int> &warningLines) {
    m_errorLines = errorLines;
    m_warningLines = warningLines;
    updateExtraSelections();
    m_lineNumberArea->update();
}

void CodeEditor::clearDiagnostics() {
    m_errorLines.clear();
    m_warningLines.clear();
    updateExtraSelections();
    m_lineNumberArea->update();
}

void CodeEditor::gotoLineColumn(int line1Based, int column1Based) {
    const int targetBlockIdx = qMax(0, line1Based - 1);
    QTextBlock block = document()->findBlockByNumber(targetBlockIdx);
    if (!block.isValid()) {
        block = document()->lastBlock();
    }
    if (!block.isValid()) {
        return;
    }

    QTextCursor cur(block);
    const int colOffset = qBound(0, column1Based - 1, block.length() > 0 ? block.length() - 1 : 0);
    cur.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor, colOffset);
    setTextCursor(cur);
    centerCursor();
    setFocus();
}

int CodeEditor::lineNumberAreaWidth() const {
    int digits = 1;
    int maxBlocks = qMax(1, blockCount());
    while (maxBlocks >= 10) {
        maxBlocks /= 10;
        ++digits;
    }
    digits = qMax(digits, 3);
    return 18 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
}

void CodeEditor::updateLineNumberAreaWidth(int /*newBlockCount*/) {
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void CodeEditor::updateLineNumberArea(const QRect &rect, int dy) {
    if (dy != 0) {
        m_lineNumberArea->scroll(0, dy);
    } else {
        m_lineNumberArea->update(0, rect.y(), m_lineNumberArea->width(), rect.height());
    }

    if (rect.contains(viewport()->rect())) {
        updateLineNumberAreaWidth(0);
    }
}

void CodeEditor::resizeEvent(QResizeEvent *event) {
    QPlainTextEdit::resizeEvent(event);
    const QRect cr = contentsRect();
    m_lineNumberArea->setGeometry(QRect(cr.left(), cr.top(), lineNumberAreaWidth(), cr.height()));
    placeRawButton();
}

void CodeEditor::setAlternateViews(const QString &decoded, const QString &raw) {
    m_decodedText = decoded;
    m_rawText = raw;
    if (m_rawButton == nullptr) {
        m_rawButton = new QToolButton(this);
        m_rawButton->setText(QStringLiteral("Raw"));
        m_rawButton->setCheckable(true);
        m_rawButton->setAutoRaise(false);
        m_rawButton->setFocusPolicy(Qt::NoFocus);
        m_rawButton->setToolTip(QStringLiteral("Show the bytes of the file as hex instead of the decoded view"));
        m_rawButton->setAccessibleName(QStringLiteral("Raw"));
        connect(m_rawButton, &QToolButton::toggled, this, [this](bool raw) { showView(raw); });
        // the viewport shrinks when a scroll bar appears; keep the button
        // in its top right corner
        viewport()->installEventFilter(this);
        m_rawButton->show();
    }
    showView(m_rawButton->isChecked());
    placeRawButton();
}

bool CodeEditor::showsRaw() const {
    return m_rawButton != nullptr && m_rawButton->isChecked();
}

void CodeEditor::setShowRaw(bool raw) {
    if (m_rawButton != nullptr) {
        m_rawButton->setChecked(raw);
    }
}

void CodeEditor::showView(bool raw) {
    setPlainText(raw ? m_rawText : m_decodedText);
    document()->setModified(false);
}

void CodeEditor::placeRawButton() {
    if (m_rawButton == nullptr) {
        return;
    }
    m_rawButton->adjustSize();
    const QRect vp = viewport()->geometry();
    m_rawButton->move(vp.right() - m_rawButton->width() - 4, vp.top() + 4);
    m_rawButton->raise();
}

bool CodeEditor::eventFilter(QObject *watched, QEvent *event) {
    if (watched == viewport() && event->type() == QEvent::Resize) {
        placeRawButton();
    }
    return QPlainTextEdit::eventFilter(watched, event);
}

void CodeEditor::updateExtraSelections() {
    QList<QTextEdit::ExtraSelection> selections;
    const QColor baseBg = palette().color(QPalette::Base);
    const bool dark = baseBg.lightness() < 128;

    // Highlight current line
    {
        QTextEdit::ExtraSelection curSel;
        curSel.format.setBackground(dark ? QColor(45, 50, 60) : QColor(238, 244, 252));
        curSel.format.setProperty(QTextFormat::FullWidthSelection, true);
        curSel.cursor = textCursor();
        curSel.cursor.clearSelection();
        selections.append(curSel);
    }

    // Highlight warning lines
    for (int ln : m_warningLines) {
        if (m_errorLines.contains(ln)) {
            continue;
        }
        const QTextBlock blk = document()->findBlockByNumber(ln - 1);
        if (blk.isValid()) {
            QTextEdit::ExtraSelection sel;
            sel.format.setBackground(dark ? QColor(80, 65, 25) : QColor(255, 246, 209));
            sel.format.setProperty(QTextFormat::FullWidthSelection, true);
            sel.cursor = QTextCursor(blk);
            selections.append(sel);
        }
    }

    // Highlight error lines
    for (int ln : m_errorLines) {
        const QTextBlock blk = document()->findBlockByNumber(ln - 1);
        if (blk.isValid()) {
            QTextEdit::ExtraSelection sel;
            sel.format.setBackground(dark ? QColor(90, 35, 38) : QColor(255, 226, 226));
            sel.format.setProperty(QTextFormat::FullWidthSelection, true);
            sel.cursor = QTextCursor(blk);
            selections.append(sel);
        }
    }

    setExtraSelections(selections);
}

void CodeEditor::lineNumberAreaPaintEvent(QPaintEvent *event) {
    QPainter painter(m_lineNumberArea);
    const QColor baseBg = palette().color(QPalette::Base);
    const bool dark = baseBg.lightness() < 128;
    const QColor gutterBg = dark ? baseBg.lighter(115) : baseBg.darker(104);
    const QColor borderCol = dark ? baseBg.lighter(140) : baseBg.darker(116);
    const QColor textCol = dark ? QColor(150, 158, 170) : QColor(110, 118, 129);
    const QColor activeTextCol = palette().color(QPalette::Text);

    painter.fillRect(event->rect(), gutterBg);
    painter.setPen(borderCol);
    painter.drawLine(m_lineNumberArea->width() - 1, event->rect().top(),
                     m_lineNumberArea->width() - 1, event->rect().bottom());

    QTextBlock block = firstVisibleBlock();
    int blockNumber = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());
    const int curBlock = textCursor().blockNumber();

    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            const int lineNo = blockNumber + 1;
            if (m_errorLines.contains(lineNo)) {
                painter.setBrush(QColor(220, 53, 69));
                painter.setPen(Qt::NoPen);
                painter.drawEllipse(4, top + (fontMetrics().height() - 7) / 2, 7, 7);
            } else if (m_warningLines.contains(lineNo)) {
                painter.setBrush(QColor(230, 162, 0));
                painter.setPen(Qt::NoPen);
                painter.drawEllipse(4, top + (fontMetrics().height() - 7) / 2, 7, 7);
            }

            painter.setPen(blockNumber == curBlock ? activeTextCol : textCol);
            QFont f = font();
            f.setBold(blockNumber == curBlock);
            painter.setFont(f);
            painter.drawText(0, top, m_lineNumberArea->width() - 6, fontMetrics().height(),
                             Qt::AlignRight | Qt::AlignVCenter, QString::number(lineNo));
        }

        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++blockNumber;
    }
}

bool CodeEditor::findNextText(const QString &needle, bool caseSensitive, bool wholeWords, bool backward) {
    if (needle.isEmpty()) {
        return false;
    }
    QTextDocument::FindFlags flags;
    if (caseSensitive) flags |= QTextDocument::FindCaseSensitively;
    if (wholeWords) flags |= QTextDocument::FindWholeWords;
    if (backward) flags |= QTextDocument::FindBackward;

    if (find(needle, flags)) {
        return true;
    }

    // Wrap around once
    QTextCursor saved = textCursor();
    QTextCursor wrapCursor(document());
    if (backward) {
        wrapCursor.movePosition(QTextCursor::End);
    } else {
        wrapCursor.movePosition(QTextCursor::Start);
    }
    setTextCursor(wrapCursor);
    if (find(needle, flags)) {
        return true;
    }
    setTextCursor(saved);
    return false;
}

int CodeEditor::replaceAllText(const QString &needle, const QString &replacement,
                               bool caseSensitive, bool wholeWords) {
    if (needle.isEmpty() || isReadOnly()) {
        return 0;
    }
    QTextDocument::FindFlags flags;
    if (caseSensitive) flags |= QTextDocument::FindCaseSensitively;
    if (wholeWords) flags |= QTextDocument::FindWholeWords;

    QTextCursor cursor(document());
    cursor.movePosition(QTextCursor::Start);

    int count = 0;
    cursor.beginEditBlock();
    while (true) {
        cursor = document()->find(needle, cursor, flags);
        if (cursor.isNull()) {
            break;
        }
        cursor.insertText(replacement);
        ++count;
    }
    cursor.endEditBlock();
    return count;
}
