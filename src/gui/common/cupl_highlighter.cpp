#include "cupl_highlighter.h"

#include <QFileInfo>
#include <QGuiApplication>
#include <QPalette>
#include <QRegularExpression>
#include <QSet>
#include <QStyleHints>

namespace {

// Block state bits.  -1 (no state yet) counts as 0.
enum : int {
    kInComment = 1,     // inside /* ... */
    kInVectors = 2,     // .si: after VECTORS:
    kInResults = 4      // .so: after the first ===== line
};

using WordSet = QSet<QString>;

const WordSet &headerWords() {
    static const WordSet s = {
        "name", "partno", "date", "rev", "revision", "designer", "company",
        "assy", "assembly", "loc", "location", "device", "format"
    };
    return s;
}

// The rest of the compiler's keyword table
const WordSet &pldKeywords() {
    static const WordSet s = {
        "append", "condition", "declare", "default", "demorgan", "exit", "extern",
        "field", "fld", "function", "fuse", "global", "group", "grp", "if", "init_to",
        "min", "msg", "next", "node", "out", "pin", "pinnode", "present", "property",
        "register_select", "sequence", "sequenced", "sequencejk", "sequencers",
        "sequencet", "table", "troff", "tron"
    };
    return s;
}

// Words after '$' in a design file (preprocessor and macro processor)
const WordSet &pldDirectives() {
    static const WordSet s = {
        "define", "undef", "include", "ifdef", "ifndef", "else", "endif",
        "macro", "mend", "repeat", "repend", "if", "ifend"
    };
    return s;
}

// The simulator's keyword table, without the header words
const WordSet &siKeywords() {
    static const WordSet s = {
        "msg", "for", "repeat", "fld", "field", "while", "preload", "order",
        "base", "vectors", "macro", "mend", "hex", "octal", "decimal", "binary",
        "exit", "stuckh", "stuckl", "trace", "if", "else", "endif", "do", "until",
        "simoff", "simon", "var", "set", "endf", "endw", "comp", "out", "call"
    };
    return s;
}

// Pin extensions of the device libraries
const WordSet &extensions() {
    static const WordSet s = {
        "iockmux", "apmux", "armux", "ckmux", "lemux", "oemux", "imux", "ioap",
        "ioar", "iock", "byp", "cnt", "dfb", "ffm", "int", "iod", "iol", "lfb",
        "obs", "tec", "tfb", "ap", "ar", "ca", "ce", "ck", "dq", "io", "le", "lq",
        "oe", "pr", "sp", "sr", "t1", "t2", "d", "j", "k", "l", "r", "s", "t"
    };
    return s;
}

bool isWordStart(QChar c) { return c.isLetter() || c == QLatin1Char('_'); }
bool isWordChar(QChar c) { return c.isLetterOrNumber() || c == QLatin1Char('_'); }

int wordEnd(const QString &text, int pos) {
    while (pos < text.size() && isWordChar(text[pos])) {
        ++pos;
    }
    return pos;
}

int skipSpaces(const QString &text, int pos) {
    while (pos < text.size() && text[pos].isSpace()) {
        ++pos;
    }
    return pos;
}

// 'b'0101, 'o'17, 'd'12, 'h'FF and don't-care digits (X): returns the end
// of the number, or -1 if no radix prefix starts at pos.
int radixNumberEnd(const QString &text, int pos) {
    if (pos + 2 >= text.size() || text[pos + 2] != QLatin1Char('\'')) {
        return -1;
    }
    if (!QStringLiteral("bBoOdDhH").contains(text[pos + 1])) {
        return -1;
    }
    int end = pos + 3;
    while (end < text.size() && (isWordChar(text[end]))) {
        ++end;
    }
    return end;
}

// {i} and {i+1} in $REPEAT and $MACRO bodies: a closing brace on the same
// line with only an expression between.  The braces of a TABLE block are
// not matched by this.
int substitutionEnd(const QString &text, int pos) {
    int end = pos + 1;
    bool any = false;
    while (end < text.size()) {
        const QChar c = text[end];
        if (c == QLatin1Char('}')) {
            return any ? end + 1 : -1;
        }
        if (!isWordChar(c) && !QStringLiteral("+-*/% ").contains(c)) {
            return -1;
        }
        any = any || isWordChar(c);
        ++end;
    }
    return -1;
}

}  // namespace

CuplHighlighter::CuplHighlighter(QTextDocument *parent, Language language, Theme theme)
    : QSyntaxHighlighter(parent), m_language(language), m_theme(theme) {
    buildFormats();
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this]() {
        if (m_theme == Theme::Auto) {
            refreshPalette();
        }
    });
#endif
}

CuplHighlighter::Language CuplHighlighter::languageForFile(const QString &filePath) {
    if (filePath.isEmpty()) {
        return Language::Pld;
    }
    const QString ext = QFileInfo(filePath).suffix().toLower();
    if (ext == QLatin1String("si")) {
        return Language::Si;
    }
    if (ext == QLatin1String("so")) {
        return Language::SiListing;
    }
    if (ext == QLatin1String("lst")) {
        return Language::PldListing;
    }
    static const QStringList pld = {
        QStringLiteral("pld"), QStringLiteral("m"), QStringLiteral("mac"),
        QStringLiteral("doc"), QStringLiteral("sim"), QStringLiteral("def")
    };
    return pld.contains(ext) ? Language::Pld : Language::None;
}

void CuplHighlighter::setLanguage(Language language) {
    if (language != m_language) {
        m_language = language;
        rehighlight();
    }
}

void CuplHighlighter::refreshPalette() {
    buildFormats();
    rehighlight();
}

void CuplHighlighter::buildFormats() {
    bool dark = m_theme == Theme::Dark;
    if (m_theme == Theme::Auto) {
        dark = QGuiApplication::palette().color(QPalette::Base).lightness() < 128;
    }
    auto fmt = [dark](QColor light, QColor darkColor, int weight = QFont::Normal,
                      bool italic = false) {
        QTextCharFormat f;
        f.setForeground(dark ? darkColor : light);
        f.setFontWeight(weight);
        f.setFontItalic(italic);
        return f;
    };

    m_header = fmt(QColor(0, 102, 153), QColor(86, 182, 194), QFont::Bold);
    m_keyword = fmt(QColor(128, 0, 144), QColor(198, 120, 221), QFont::Bold);
    m_directive = fmt(QColor(163, 73, 0), QColor(229, 192, 123), QFont::Bold);
    m_extension = fmt(QColor(0, 92, 197), QColor(97, 175, 239), QFont::DemiBold);
    m_logic = fmt(QColor(196, 26, 22), QColor(224, 108, 117), QFont::Bold);
    m_assign = fmt(QColor(90, 90, 90), QColor(171, 178, 191), QFont::Bold);
    m_number = fmt(QColor(152, 104, 1), QColor(209, 154, 102));
    m_string = fmt(QColor(28, 125, 54), QColor(152, 195, 121));
    m_comment = fmt(QColor(106, 115, 125), QColor(127, 132, 142), QFont::Normal, true);
    m_lineNumber = fmt(QColor(140, 140, 140), QColor(110, 118, 129));
    m_error = fmt(QColor(200, 0, 0), QColor(248, 113, 113), QFont::Bold);
    m_vecInput = fmt(QColor(0, 92, 197), QColor(97, 175, 239), QFont::Bold);
    m_vecOutput = fmt(QColor(28, 125, 54), QColor(152, 195, 121), QFont::Bold);
    m_vecClock = fmt(QColor(163, 73, 0), QColor(229, 192, 123), QFont::Bold);
    m_vecOther = fmt(QColor(128, 0, 144), QColor(198, 120, 221));
}

// From pos (inside a comment) to the end of the comment or line.
int CuplHighlighter::scanComment(const QString &text, int pos, int state) {
    const int close = text.indexOf(QStringLiteral("*/"), pos);
    if (close < 0) {
        setFormat(pos, text.size() - pos, m_comment);
        return state | kInComment;
    }
    setFormat(pos, close + 2 - pos, m_comment);
    return state & ~kInComment;
}

int CuplHighlighter::scanSource(const QString &text, int pos, int state, bool si) {
    const int n = text.size();
    bool afterDirective = false;    // the rest of the line is not vectors

    if (state & kInComment) {
        state = scanComment(text, pos, state);
        if (state & kInComment) {
            return state;
        }
        pos = text.indexOf(QStringLiteral("*/"), pos) + 2;
    }

    while (pos < n) {
        if ((state & kInVectors) && !afterDirective) {
            return scanVectors(text, pos, state);
        }
        const QChar c = text[pos];
        const QChar next = pos + 1 < n ? text[pos + 1] : QChar();

        if (c.isSpace()) {
            ++pos;
            continue;
        }
        if (c == QLatin1Char('/') && next == QLatin1Char('*')) {
            state = scanComment(text, pos, state);
            if (state & kInComment) {
                return state;
            }
            pos = text.indexOf(QStringLiteral("*/"), pos + 2) + 2;
            continue;
        }

        int end = pos + 1;
        const QTextCharFormat *format = nullptr;

        if (c == QLatin1Char('"')) {
            const int close = text.indexOf(QLatin1Char('"'), pos + 1);
            end = close < 0 ? n : close + 1;
            format = &m_string;
        } else if (c == QLatin1Char('\'')) {
            const int radixEnd = radixNumberEnd(text, pos);
            if (radixEnd > 0) {
                end = radixEnd;
                format = &m_number;
            }
        } else if (c == QLatin1Char('$')) {
            const int wordStart = pos + 1;
            const int wEnd = wordEnd(text, wordStart);
            const QString word = text.mid(wordStart, wEnd - wordStart).toLower();
            if (si ? siKeywords().contains(word) : pldDirectives().contains(word)) {
                end = wEnd;
                format = &m_directive;
                afterDirective = true;
            } else if (!si) {
                format = &m_logic;      // XOR
            }
        } else if (c == QLatin1Char('{')) {
            const int subEnd = substitutionEnd(text, pos);
            if (subEnd > 0) {
                end = subEnd;
                format = &m_directive;
            }
        } else if (c == QLatin1Char('.')) {
            if (next == QLatin1Char('.')) {
                end = pos + 2;
                format = &m_assign;     // range
            } else if (pos > 0 && (isWordChar(text[pos - 1]) ||
                                   QStringLiteral("])}").contains(text[pos - 1])) &&
                       isWordStart(next)) {
                const int wEnd = wordEnd(text, pos + 1);
                if (extensions().contains(text.mid(pos + 1, wEnd - pos - 1).toLower())) {
                    format = &m_extension;
                }
                end = wEnd;
            }
        } else if (isWordStart(c)) {
            end = wordEnd(text, pos);
            const QString word = text.mid(pos, end - pos).toLower();
            if (headerWords().contains(word)) {
                format = &m_header;
            } else if (si ? siKeywords().contains(word) : pldKeywords().contains(word)) {
                format = &m_keyword;
                if (si && word == QLatin1String("vectors")) {
                    // The vectors start after the colon.
                    const int colon = skipSpaces(text, end);
                    setFormat(pos, end - pos, m_keyword);
                    if (colon < n && text[colon] == QLatin1Char(':')) {
                        setFormat(colon, 1, m_assign);
                        end = colon + 1;
                    }
                    state |= kInVectors;
                    afterDirective = false;
                    pos = end;
                    continue;
                }
            }
        } else if (c.isDigit()) {
            end = wordEnd(text, pos);           // hex digits by default
            format = &m_number;
        } else if (c == QLatin1Char('!') || c == QLatin1Char('&') || c == QLatin1Char('#')) {
            format = &m_logic;
        } else if (c == QLatin1Char('=')) {
            end = (next == QLatin1Char('>') || next == QLatin1Char('=')) ? pos + 2 : pos + 1;
            format = &m_assign;
        } else if (c == QLatin1Char(':')) {
            if (!si && (next == QLatin1Char('&') || next == QLatin1Char('#') || next == QLatin1Char('$'))) {
                end = pos + 2;
                format = &m_logic;      // bit field reduction
            } else {
                format = &m_assign;     // equality, or the colon after ORDER
            }
        } else if (si && c == QLatin1Char('%') && next.isDigit()) {
            end = wordEnd(text, pos + 1);
            format = &m_number;         // ORDER spacing
        }

        if (format != nullptr) {
            setFormat(pos, end - pos, *format);
        }
        pos = end;
    }
    return state;
}

// Test vectors: one character per signal, quoted bus values, comments and
// '$' directives that run to the end of the line.
int CuplHighlighter::scanVectors(const QString &text, int pos, int state) {
    const int n = text.size();
    while (pos < n) {
        const QChar c = text[pos];
        if (c == QLatin1Char('/') && pos + 1 < n && text[pos + 1] == QLatin1Char('*')) {
            state = scanComment(text, pos, state);
            if (state & kInComment) {
                return state;
            }
            pos = text.indexOf(QStringLiteral("*/"), pos + 2) + 2;
            continue;
        }
        if (c == QLatin1Char('$')) {
            // A directive; the rest of the line is not vectors.
            return scanSource(text, pos, state & ~kInVectors, true) | kInVectors;
        }
        if (c == QLatin1Char('\'') || c == QLatin1Char('"')) {
            const int close = text.indexOf(c, pos + 1);
            const int end = close < 0 ? n : close + 1;
            setFormat(pos, end - pos, c == QLatin1Char('\'') ? m_vecInput : m_vecOutput);
            pos = end;
            continue;
        }
        switch (c.toUpper().unicode()) {
        case '0': case '1':
            setFormat(pos, 1, m_vecInput);
            break;
        case 'H': case 'L':
            setFormat(pos, 1, m_vecOutput);
            break;
        case 'C': case 'K': case 'P':
            setFormat(pos, 1, m_vecClock);
            break;
        case 'X': case 'Z': case 'N': case '*':
            setFormat(pos, 1, m_vecOther);
            break;
        default:
            break;
        }
        ++pos;
    }
    return state;
}

// .lst and .so: numbered source lines, banners, messages and, in a .so,
// the simulation results.
void CuplHighlighter::highlightListing(const QString &text, int state, bool si) {
    static const QRegularExpression numbered(QStringLiteral("^\\s*(\\d+):"));
    static const QRegularExpression message(QStringLiteral("^\\s*\\[\\d{4}[a-z]{2}\\]"));
    static const QRegularExpression marker(QStringLiteral("^\\s*\\^[\\^ ]*$"));
    static const QRegularExpression result(QStringLiteral("^\\d{4}:"));

    if (message.match(text).hasMatch() || marker.match(text).hasMatch()) {
        setFormat(0, text.size(), m_error);
        setCurrentBlockState(state);
        return;
    }
    if (si && text.startsWith(QStringLiteral("==="))) {
        setFormat(0, text.size(), m_lineNumber);
        setCurrentBlockState((state | kInResults) & ~kInComment);
        return;
    }
    if (state & kInResults) {
        if (result.match(text).hasMatch()) {
            setFormat(0, 5, m_lineNumber);
            scanVectors(text, 5, 0);
        } else {
            // signal name heading between the ===== lines, $MSG output
            setFormat(0, text.size(), m_header);
        }
        setCurrentBlockState(state);
        return;
    }
    const QRegularExpressionMatch m = numbered.match(text);
    if (m.hasMatch()) {
        const int start = m.capturedEnd();
        setFormat(0, start, m_lineNumber);
        setCurrentBlockState(scanSource(text, start, state, si));
        return;
    }
    // banner, page headings
    setFormat(0, text.size(), m_lineNumber);
    setCurrentBlockState(state);
}

void CuplHighlighter::highlightBlock(const QString &text) {
    int state = previousBlockState();
    if (state < 0) {
        state = 0;
    }
    switch (m_language) {
    case Language::None:
        setCurrentBlockState(0);
        break;
    case Language::Pld:
        setCurrentBlockState(scanSource(text, 0, state, false));
        break;
    case Language::Si:
        setCurrentBlockState(scanSource(text, 0, state, true));
        break;
    case Language::PldListing:
        highlightListing(text, state, false);
        break;
    case Language::SiListing:
        highlightListing(text, state, true);
        break;
    }
}
