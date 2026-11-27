/*
 * base_lex.c - the table-driven scanner shared by the text readers of the
 * ATF150x fitter.
 *
 * A reader installs a keyword table and the comment characters, hands over
 * the input stream and calls lex_start; lex_next_token then leaves the
 * token code in g_lex_token and its text in g_lex_text / g_lex_long_text.
 *
 * EDIF and EQN use separate scanners with source locations and bounded
 * storage.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "base_int.h"
#include "dev.h"

#define LEX_MAX_KEYWORDS    100     /* size of the keyword table */
#define LEX_MAX_SEPARATORS  0x14
#define LEX_MAX_DELIMITERS  0x1e
#define LEX_MAX_EXTRA       0x1e
#define LEX_LONG_MAX        0xa00   /* characters kept of one word */

char g_lex_text[FIT_NAME_MAX + 1];      /* FIT_NAME_MAX characters and the terminating NUL */
char g_lex_long_text[LEX_LONG_MAX + 1];
int  g_lex_token;
int  g_lex_line;
int  g_lex_failed;
int  g_lex_error_count;
int  g_lex_eof;
char g_lex_char;
int  g_verbose;

static FILE *g_lex_in;
static FILE *g_lex_err;
static int   g_lex_warning_count;
static int   g_lex_max_warnings;
static int   g_lex_max_errors;
static char  g_lex_comment_start;
static char  g_lex_comment_end;

static int   g_lex_keyword_count;
static struct {
    int  code;
    char word[0x100];
} g_lex_keywords[LEX_MAX_KEYWORDS + 1]; /* entries 1..count */

static int  g_lex_separator_count;
static char g_lex_separators[LEX_MAX_SEPARATORS + 1];   /* entries 1..count; set but never consulted */
static int  g_lex_delimiter_count;
static char g_lex_delimiters[LEX_MAX_DELIMITERS + 1];   /* entries 1..count */
static int  g_lex_extra_count;
static char g_lex_extra[LEX_MAX_EXTRA + 1];         /* entries 1..count */

/* a message of the lexer; the extended mode also writes it into the report */
static void lex_print(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(g_lex_err, fmt, ap);
    va_end(ap);
    if (FIT_EXT && g_fit_report != NULL) {
        va_start(ap, fmt);
        vfprintf(g_fit_report, fmt, ap);
        va_end(ap);
    }
}

/* "Warning on line %d", only when g_verbose; at most g_lex_max_warnings
 * and none after an error */
static void lex_warning(const char *msg)
{
    if (g_verbose != 0) {
        g_lex_warning_count++;
        if (g_lex_warning_count < g_lex_max_warnings && g_lex_error_count == 0)
            lex_print("Warning on line %d : %s\n", g_lex_line, msg);
    }
}

static int lex_is_delimiter(char c)
{
    for (int i = 1; i <= g_lex_delimiter_count; i++) {
        if (g_lex_delimiters[i] == c)
            return 1;
    }
    return 0;
}

static int lex_is_extra_char(char c)
{
    for (int i = 1; i <= g_lex_extra_count; i++) {
        if (g_lex_extra[i] == c)
            return 1;
    }
    return 0;
}

static int lex_is_blank(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == (char)-1;
}

/* letters, digits, blanks, delimiters and the extra characters
 * may appear in the input */
static int lex_char_allowed(char c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
        return 1;
    if (c >= '0' && c <= '9')
        return 1;
    if (lex_is_blank(c))
        return 1;
    if (lex_is_delimiter(c))
        return 1;
    return lex_is_extra_char(c);
}

/* one character of the input; end of file reads as (char)-1 */
static char lex_getc(void)
{
    int c = file_getc_text(g_lex_in);
    char ch = (char)c;      /* EOF becomes (char)-1 */

    g_lex_line += ch == '\n';
    return ch;
}

/* read the next character into g_lex_char, skipping characters
 * that are not allowed (with a warning) and comments. */
static void lex_next_char(void)
{
    g_lex_char = lex_getc();
    while (!lex_char_allowed(g_lex_char)) {
        char msg[40];
        size_t n;

        strcpy(msg, "Skipping not allowed character near \"");
        n = strlen(msg);
        msg[n] = g_lex_char;
        msg[n + 1] = '\0';
        strcat(msg, "\"");
        lex_warning(msg);
        g_lex_char = lex_getc();
    }
    if (g_lex_char == g_lex_comment_start) {
        do {
            for (;;) {
                g_lex_char = lex_getc();
                g_lex_eof = g_lex_char == (char)-1;
                if (g_lex_char == g_lex_comment_end) {
                    if (g_lex_char != (char)-1)
                        g_lex_char = lex_getc();
                    break;
                }
                if (g_lex_char == (char)-1)
                    break;
            }
        } while (g_lex_char == g_lex_comment_start);
    }
    g_lex_eof = g_lex_char == (char)-1;
}

static void lex_skip_blanks(void)
{
    while (lex_is_blank(g_lex_char) && g_lex_char != (char)-1)
        lex_next_char();
}

/* collect a word (up to the next blank or delimiter) */
static void lex_read_word(void)
{
    char *p = g_lex_long_text;
    int n = 0;
    int ended = 0;

    do {
        *p++ = g_lex_char;
        n++;
        lex_next_char();
        if (lex_is_blank(g_lex_char) || lex_is_delimiter(g_lex_char)) {
            ended = n < LEX_LONG_MAX;
            break;
        }
    } while (n < LEX_LONG_MAX);
    if (!ended) {
        if (!lex_is_blank(g_lex_char) && !lex_is_delimiter(g_lex_char)) {
            lex_warning("String truncation (more than 1600 characters !)");
            do {
                lex_next_char();
                if (lex_is_blank(g_lex_char))
                    break;
            } while (!lex_is_delimiter(g_lex_char));
        }
    }
    *p = '\0';
    if (n > 0xff)
        lex_warning("Long string detected (more than 160 characters !)");
    strncpy(g_lex_text, g_lex_long_text, FIT_NAME_MAX);
    g_lex_text[FIT_NAME_MAX] = '\0';
}

/* keyword code of g_lex_text (ignoring case), FIT_TOK_IDENT if none */
static int lex_keyword_code(void)
{
    for (int i = 1; i <= g_lex_keyword_count; i++) {
        if (util_str_icmp(g_lex_text, g_lex_keywords[i].word) == 0)
            return g_lex_keywords[i].code;
    }
    return FIT_TOK_IDENT;
}

/* spelling of a keyword or delimiter token, "" for any other code */
const char *lex_token_name(int tok)
{
    if (tok > 0 && tok <= g_lex_keyword_count)
        return g_lex_keywords[tok].word;
    switch (tok) {
    case FIT_TOK_COMMA:    return ",";
    case FIT_TOK_AMP:      return "&";
    case FIT_TOK_QUOTE:    return "'";
    case FIT_TOK_DQUOTE:   return "\"";
    case FIT_TOK_TILDE:    return "~";
    case FIT_TOK_QUESTION: return "?";
    case FIT_TOK_BANG:     return "!";
    case FIT_TOK_STARSTAR: return "**";
    case FIT_TOK_STAR:     return "*";
    case FIT_TOK_SEMI:     return ";";
    case FIT_TOK_COLON:    return ":";
    case FIT_TOK_ASSIGN:   return ":=";
    case FIT_TOK_EQEQ:     return "==";
    case FIT_TOK_GE:       return ">=";
    case FIT_TOK_GT:       return ">";
    case FIT_TOK_LE:       return "<=";
    case FIT_TOK_LT:       return "<";
    case FIT_TOK_EQUAL:    return "=";
    case FIT_TOK_RPAREN:   return ")";
    case FIT_TOK_LPAREN:   return "(";
    case FIT_TOK_CARET:    return "^";
    case FIT_TOK_DOT:      return ".";
    case FIT_TOK_MINUS:    return "-";
    case FIT_TOK_PLUS:     return "+";
    case FIT_TOK_SLASH:    return "/";
    case FIT_TOK_RBRACE:   return "}";
    case FIT_TOK_LBRACE:   return "{";
    case FIT_TOK_RBRACKET: return "]";
    case FIT_TOK_LBRACKET: return "[";
    default:               return "";
    }
}

/* if the current character is in the delimiter table and is one
 * of the known delimiters, set g_lex_token and consume it. */
static int lex_read_delimiter(void)
{
    int one, two = 0;
    char second = '\0';

    if (!lex_is_delimiter(g_lex_char))
        return 0;
    switch (g_lex_char) {
    case '!':  one = FIT_TOK_BANG; break;
    case '"':  one = FIT_TOK_DQUOTE; break;
    case '&':  one = FIT_TOK_AMP; break;
    case '\'': one = FIT_TOK_QUOTE; break;
    case '(':  one = FIT_TOK_LPAREN; break;
    case ')':  one = FIT_TOK_RPAREN; break;
    case '*':  one = FIT_TOK_STAR; second = '*'; two = FIT_TOK_STARSTAR; break;
    case '+':  one = FIT_TOK_PLUS; break;
    case ',':  one = FIT_TOK_COMMA; break;
    case '-':  one = FIT_TOK_MINUS; break;
    case '.':  one = FIT_TOK_DOT; break;
    case '/':  one = FIT_TOK_SLASH; break;
    case ':':  one = FIT_TOK_COLON; second = '='; two = FIT_TOK_ASSIGN; break;
    case ';':  one = FIT_TOK_SEMI; break;
    case '<':  one = FIT_TOK_LT; second = '='; two = FIT_TOK_LE; break;
    case '=':  one = FIT_TOK_EQUAL; second = '='; two = FIT_TOK_EQEQ; break;
    case '>':  one = FIT_TOK_GT; second = '='; two = FIT_TOK_GE; break;
    case '?':  one = FIT_TOK_QUESTION; break;
    case '[':  one = FIT_TOK_LBRACKET; break;
    case ']':  one = FIT_TOK_RBRACKET; break;
    case '^':  one = FIT_TOK_CARET; break;
    case '{':  one = FIT_TOK_LBRACE; break;
    case '}':  one = FIT_TOK_RBRACE; break;
    case '~':  one = FIT_TOK_TILDE; break;
    default:
        return 0;
    }
    if (second == '\0') {
        g_lex_token = one;
        lex_next_char();
        return 1;
    }
    lex_next_char();
    if (g_lex_char == second) {
        g_lex_token = two;
        lex_next_char();
        return 1;
    }
    g_lex_token = one;
    return 1;
}

/* reads the next token: a delimiter, a keyword or an identifier, or
 * FIT_TOK_EOF at the end of the input */
void lex_next_token(void)
{
    if (g_lex_eof == 0) {
        lex_skip_blanks();
        if (g_lex_eof == 0) {
            const char *name;

            if (lex_read_delimiter() == 0) {
                lex_read_word();
                g_lex_token = lex_keyword_code();
                return;
            }
            name = lex_token_name(g_lex_token);
            strcpy(g_lex_text, name);
            strcpy(g_lex_long_text, name);
            return;
        }
    }
    g_lex_token = FIT_TOK_EOF;
    g_lex_text[0] = (char)0xff;
}

/* reports "Error on line %d : <msg><arg>" and counts the error */
void lex_error_text(const char *msg, const char *arg)
{
    g_lex_error_count++;
    g_lex_failed = 1;
    if (g_lex_error_count < g_lex_max_errors)
        lex_print("Error on line %d : %s%s\n", g_lex_line, msg, arg);
    if (g_lex_error_count == g_lex_max_errors)
        lex_print("Error on line %d (too many errors)\n", g_lex_line);
}

/* report msg; when mode != 0 and tokens are given (a list ended
 * by 0), skip input until the current token is one of them or the end of
 * the file. */
void lex_error(int mode, const char *msg, int tok, ...)
{
    g_lex_error_count++;
    g_lex_failed = 1;
    if (g_lex_error_count < g_lex_max_errors)
        lex_print("Error on line %d : %s\n", g_lex_line, msg);
    if (g_lex_error_count == g_lex_max_errors)
        lex_print("Error on line %d (too many errors)\n", g_lex_line);
    if (mode != 0 && tok != 0) {
        FitList *stop = list_create();
        va_list ap;

        va_start(ap, tok);
        do {
            list_add(stop, FIT_INT2PTR(tok));
            tok = va_arg(ap, int);
        } while (tok != 0);
        va_end(ap);
        list_add(stop, FIT_INT2PTR(FIT_TOK_EOF));
        while (list_find(stop, FIT_INT2PTR(g_lex_token), list_ptr_eq) == 0)
            lex_next_token();
        list_destroy(&stop);
    }
}

/* lex_error without the message: count an error and, when mode
 * != 0 and tokens are given (a list ended by 0), skip input until the
 * current token is one of them or the end of the file. */
void lex_error_skip(int mode, int tok, ...)
{
    g_lex_error_count++;
    g_lex_failed = 1;
    if (mode != 0 && tok != 0) {
        FitList *stop = list_create();
        va_list ap;

        va_start(ap, tok);
        do {
            list_add(stop, FIT_INT2PTR(tok));
            tok = va_arg(ap, int);
        } while (tok != 0);
        va_end(ap);
        list_add(stop, FIT_INT2PTR(FIT_TOK_EOF));
        while (list_find(stop, FIT_INT2PTR(g_lex_token), list_ptr_eq) == 0)
            lex_next_token();
        list_destroy(&stop);
    }
}

/* n pairs (index, word) follow; the indices
 * must be 1, 2, ..., n. */
int lex_set_keyword_table(int n, ...)
{
    va_list ap;
    int ok = 1;

    g_lex_keyword_count = n;
    if (n > LEX_MAX_KEYWORDS) {     /* extra entries are ignored */
        g_lex_keyword_count = LEX_MAX_KEYWORDS;
        n = LEX_MAX_KEYWORDS;
    }
    va_start(ap, n);
    for (int i = 1; i <= n; i++) {
        int code = va_arg(ap, int);
        const char *word;

        g_lex_keywords[i].code = code;
        if (code != i) {
            fprintf(g_msg, "Error in LexSetKeyWordTable : Bad Keyword DomList, indices must range from 1 to max_keyword\n");
            ok = 0;
            break;
        }
        word = va_arg(ap, const char *);
        strncpy(g_lex_keywords[i].word, word, sizeof(g_lex_keywords[i].word) - 1);
        g_lex_keywords[i].word[sizeof(g_lex_keywords[i].word) - 1] = '\0';
    }
    va_end(ap);
    return ok;
}

/* the following n characters (as int) become the separator table */
int lex_set_separator_table(int n, ...)
{
    va_list ap;

    if (n > LEX_MAX_SEPARATORS) {
        fprintf(g_msg, "Error in LexSetSeparatorTable : Too Much separators\n");
        return 0;
    }
    g_lex_separator_count = n;
    va_start(ap, n);
    for (int i = 1; i <= n; i++)
        g_lex_separators[i] = (char)va_arg(ap, int);
    va_end(ap);
    return 1;
}

/* the following n characters (as int) become the delimiter table */
int lex_set_delimiter_table(int n, ...)
{
    va_list ap;

    if (n > LEX_MAX_DELIMITERS) {
        fprintf(g_msg, "Error in LexSetDelimitorTable : Too Much delimitors\n");
        return 0;
    }
    g_lex_delimiter_count = n;
    va_start(ap, n);
    for (int i = 1; i <= n; i++)
        g_lex_delimiters[i] = (char)va_arg(ap, int);
    va_end(ap);
    return 1;
}

/* the following n characters (as int) are allowed in words besides
 * letters and digits */
int lex_set_extra_char_table(int n, ...)
{
    va_list ap;

    if (n > LEX_MAX_EXTRA) {
        fprintf(g_msg, "Error in LexSetExtraCharTable : Too Much extra characters\n");
        return 0;
    }
    g_lex_extra_count = n;
    va_start(ap, n);
    for (int i = 1; i <= n; i++)
        g_lex_extra[i] = (char)va_arg(ap, int);
    va_end(ap);
    return 1;
}

static int lex_default_delimiters(void)
{
    lex_set_delimiter_table(0x17, '[', ']', '{', '}', '+', '-', '.', '^', ')', '(', ':', ';', '!', '?', '~',
                            '=', '<', '>', '*', ',', '"', '\'', '/');
    return 1;
}

static int lex_default_separators(void)
{
    lex_set_separator_table(4, ' ', '\t', '\n', -1);
    return 1;
}

static int lex_default_extra_chars(void)
{
    lex_set_extra_char_table(6, '%', '_', -1, '#', '$', '&');
    return 1;
}

int lex_set_default_tables(void)
{
    lex_default_extra_chars();
    lex_default_separators();
    lex_default_delimiters();
    return 1;
}

void lex_set_streams(FILE *in, FILE *err)
{
    g_lex_err = err;
    g_lex_in = in;
}

/* line 1, counters cleared, warning and error limits 20; reads the first
 * character */
void lex_start(void)
{
    g_lex_line = 1;
    g_lex_warning_count = 0;
    g_lex_max_warnings = 0x14;
    g_lex_error_count = 0;
    g_lex_max_errors = 0x14;
    g_lex_failed = 0;
    lex_next_char();
}

void lex_set_comment_chars(char start, char end)
{
    g_lex_comment_start = start;
    g_lex_comment_end = end;
}

/* consumes tok if it is the current token, else "<tok> expected" */
void lex_expect(int tok)
{
    char msg[0x100];

    if (g_lex_token == tok) {
        lex_next_token();
        return;
    }
    snprintf(msg, sizeof(msg), "%s expected", lex_token_name(tok));
    lex_error(2, msg, g_lex_token, 0);
}

/* reset the statics of this file at the start of a run */
void base_lex_reset(void)
{
    g_lex_text[0] = '\0';
    g_lex_long_text[0] = '\0';
    g_lex_token = 0;
    g_lex_line = 0;
    g_lex_failed = 0;
    g_lex_error_count = 0;
    g_lex_eof = 0;
    g_lex_char = '\0';
    g_verbose = 0;
    g_lex_in = NULL;
    g_lex_err = NULL;
    g_lex_comment_start = '\0';
    g_lex_comment_end = '\0';
    g_lex_keyword_count = 0;
    g_lex_separator_count = 0;
    g_lex_delimiter_count = 0;
    g_lex_extra_count = 0;
    (void)g_lex_separators;
}
