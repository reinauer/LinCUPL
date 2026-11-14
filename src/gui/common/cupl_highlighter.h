#pragma once

// Syntax highlighting for CUPL design files (.pld, macro files), simulator
// input files (.si) and the listings made from them (.lst, .so), shared by
// LinCUPL and LinSim.
//
// Each line is scanned once, left to right, so a word is coloured by what
// it is in context: keywords inside comments or strings stay comments or
// strings, '$' is a directive only when a known directive name follows,
// '.ext' is a pin extension only directly after a signal name, and in a
// .si file everything after VECTORS: is coloured as test vectors.  The
// word lists are those of the compiler (src/cupl/parse_tables.h,
// src/cupl/preprocess.c), the simulator (src/sim/parse.c) and the pin
// extensions defined in the Atmel.dl and cupl.dl device libraries.

#include <QString>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>

class CuplHighlighter : public QSyntaxHighlighter {
public:
    enum class Language {
        None,       // not a CUPL file
        Pld,        // design and macro files, .doc and .sim files
        Si,         // simulator input
        PldListing, // compiler listing (.lst)
        SiListing   // simulator listing (.so)
    };

    enum class Theme {
        Auto,       // follow the application palette
        Light,
        Dark        // for views that always draw on a dark background
    };

    explicit CuplHighlighter(QTextDocument *parent, Language language = Language::Pld,
                             Theme theme = Theme::Auto);

    // The language a file is highlighted as, from its extension; a file
    // without a name is taken to be a new design.
    static Language languageForFile(const QString &filePath);

    void setLanguage(Language language);
    Language language() const { return m_language; }

    // Pick the colours again, e.g. after the palette changed.
    void refreshPalette();

protected:
    void highlightBlock(const QString &text) override;

private:
    void buildFormats();
    int scanSource(const QString &text, int pos, int state, bool si);
    int scanVectors(const QString &text, int pos, int state);
    int scanComment(const QString &text, int pos, int state);
    void highlightListing(const QString &text, int state, bool si);

    Language m_language;
    Theme m_theme;

    QTextCharFormat m_header;       // Name, Partno, Device ...
    QTextCharFormat m_keyword;      // PIN, FIELD, SEQUENCE, ORDER ...
    QTextCharFormat m_directive;    // $DEFINE, $REPEAT, $MSG ... and {i} substitutions
    QTextCharFormat m_extension;    // .d .oe .ar ...
    QTextCharFormat m_logic;        // ! & # $
    QTextCharFormat m_assign;       // = : => ..
    QTextCharFormat m_number;       // 'h'FF, 12
    QTextCharFormat m_string;       // "..."
    QTextCharFormat m_comment;      // /* ... */
    QTextCharFormat m_lineNumber;   // listing line numbers and banners
    QTextCharFormat m_error;        // [0019sa] messages and ^ markers
    QTextCharFormat m_vecInput;     // 0 1 and '..' in test vectors
    QTextCharFormat m_vecOutput;    // L H and ".."
    QTextCharFormat m_vecClock;     // C K P
    QTextCharFormat m_vecOther;     // X Z N *
};
