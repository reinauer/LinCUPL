#pragma once

#include <QList>
#include <QPlainTextEdit>
#include <QSet>
#include <QWidget>

#include "../common/cupl_highlighter.h"

class QToolButton;

class CodeEditor : public QPlainTextEdit {
    Q_OBJECT

public:
    explicit CodeEditor(QWidget *parent = nullptr);

    // Also picks the highlighting from the file's extension.
    void setFilePath(const QString &path);
    QString filePath() const { return m_filePath; }

    void setCompanionOutput(bool isCompanion);
    bool isCompanionOutput() const { return m_isCompanionOutput; }

    void setHighlightLanguage(CuplHighlighter::Language language);

    // Font of editors created from now on (Options > Font...).
    static void setStandardFont(const QFont &font);
    static QFont standardFont();

    // Font chosen for this editor; zooming is applied on top of it, so a
    // new base font keeps the editor's zoom steps.
    void setBaseFont(const QFont &font);
    QFont baseFont() const { return m_baseFont; }
    void zoomBy(int steps);
    int zoomSteps() const { return m_zoomSteps; }
    bool isCuplHighlightingEnabled() const { return m_highlighter != nullptr; }

    void setDiagnosticLines(const QSet<int> &errorLines, const QSet<int> &warningLines);
    void clearDiagnostics();

    void gotoLineColumn(int line1Based, int column1Based = 1);
    int lineNumberAreaWidth() const;
    void lineNumberAreaPaintEvent(QPaintEvent *event);

    bool findNextText(const QString &needle, bool caseSensitive, bool wholeWords, bool backward = false);
    int replaceAllText(const QString &needle, const QString &replacement, bool caseSensitive, bool wholeWords);

    // A read-only file shown in two forms (the .abs file: decoded, and its
    // bytes as hex).  A checkable "Raw" button in the top right corner
    // switches between them; setting the texts again keeps the form shown.
    void setAlternateViews(const QString &decoded, const QString &raw);
    bool hasAlternateViews() const { return m_rawButton != nullptr; }
    bool showsRaw() const;
    void setShowRaw(bool raw);
    QToolButton *rawButton() const { return m_rawButton; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private slots:
    void updateLineNumberAreaWidth(int newBlockCount);
    void updateExtraSelections();
    void updateLineNumberArea(const QRect &rect, int dy);

private:
    void applyZoomedFont();
    void placeRawButton();
    void showView(bool raw);

    QToolButton *m_rawButton = nullptr;
    QString m_decodedText;
    QString m_rawText;

    QWidget *m_lineNumberArea = nullptr;
    QFont m_baseFont;
    int m_zoomSteps = 0;
    CuplHighlighter *m_highlighter = nullptr;
    QString m_filePath;
    bool m_isCompanionOutput = false;
    QSet<int> m_errorLines;
    QSet<int> m_warningLines;
};
