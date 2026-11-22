/*
 * preprocess.c - CUPL preprocessor phase (first compiler phase, preprocess_run).
 *
 * The phase runs two passes over the design:
 *
 *   1. Macro pass (token level): <design>.pld -> <design>.mx
 *      Expands $MACRO/$MEND definitions and calls, $REPEAT/$REPEND loops,
 *      $IF/$ELSE/$IFEND, $INCLUDE, {expression} substitutions, FIELD lists
 *      inside macro expansions, PIN/PINNODE lists, and collects FUSE()
 *      statements into fuse.tmp.
 *
 *   2. Define pass (character level): <design>.mx -> CA<pid> temp file
 *      Handles the lowercase directives $define, $undef, $include, $ifdef,
 *      $ifndef, $else, $endif and substitutes defined names in the text.
 *
 * Fixed-size buffers are bounds-checked.  Status and error messages go to
 * stdout.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "locate.h"
#include "ascii.h"
#include "strutil.h"
#include "phase.h"
#include "cupl_phases.h"

/* -------------------------------------------------------------------------
 * Tokens of the macro pass
 * ------------------------------------------------------------------------- */
enum {
    TOK_EOF = -1,            /* end of file / end of captured text */
    TOK_END = 0,             /* end of a macro expansion string */
    /* single characters are returned as themselves */
    TOK_NUMBER = 0x100,
    TOK_IDENT = 0x101,
    TOK_INCLUDE = 0x102,     /* $INCLUDE */
    TOK_DEFINE = 0x103,      /* $DEFINE */
    TOK_IF = 0x105,          /* $IF */
    TOK_IFEND = 0x107,       /* $IFEND */
    TOK_ELSE = 0x108,        /* $ELSE */
    TOK_PIN = 0x109,
    TOK_PINNODE = 0x10a,
    TOK_FIELD = 0x10c,
    TOK_REPEAT = 0x10d,      /* $REPEAT */
    TOK_REPEND = 0x10e,      /* $REPEND */
    TOK_MACRO = 0x10f,       /* $MACRO */
    TOK_MEND = 0x110,        /* $MEND */
    TOK_POWER = 0x111,       /* ** */
    TOK_TABLE = 0x112,
    TOK_FUSE = 0x113,
    TOK_FUNCTION = 0x114,
    TOK_SEQUENCE = 0x115,
    TOK_SEQUENCED = 0x116,
    TOK_SEQUENCET = 0x117,
    TOK_SEQUENCEJK = 0x118,
    TOK_SEQUENCERS = 0x119,
    TOK_CONDITION = 0x11a,
    TOK_EQ = 0x11b,          /* == */
    TOK_NE = 0x11c,          /* != */
    TOK_GT = 0x11d,          /* >  */
    TOK_LT = 0x11e,          /* <  */
    TOK_GE = 0x11f,          /* >= */
    TOK_LE = 0x120,          /* <= */
    TOK_COMMENT_OPEN = 0x121,
    TOK_COMMENT_CLOSE = 0x122,
    TOK_PROPERTY = 0x123,
    TOK_INIT_TO = 0x124
};

/* Keyword table.  The first two entries are the only ones that are
 * recognised while a macro body is being recorded (see g_keyword_limit). */
static const struct {
    const char *name;
    int token;
} k_keywords[] = {
    { "$MACRO", TOK_MACRO },
    { "$MEND", TOK_MEND },
    { "$REPEAT", TOK_REPEAT },
    { "$REPEND", TOK_REPEND },
    { "$INCLUDE", TOK_INCLUDE },
    { "$IF", TOK_IF },
    { "$ELSE", TOK_ELSE },
    { "$IFEND", TOK_IFEND },
    { "FIELD", TOK_FIELD },
    { "TABLE", TOK_TABLE },
    { "$DEFINE", TOK_DEFINE },
    { "FUNCTION", TOK_FUNCTION },
    { "FUSE", TOK_FUSE },
    { "SEQUENCE", TOK_SEQUENCE },
    { "SEQUENCED", TOK_SEQUENCED },
    { "SEQUENCET", TOK_SEQUENCET },
    { "SEQUENCEJK", TOK_SEQUENCEJK },
    { "SEQUENCERS", TOK_SEQUENCERS },
    { "PROPERTY", TOK_PROPERTY },
    { "INIT_TO", TOK_INIT_TO },
    { "PIN", TOK_PIN },
    { "PINNODE", TOK_PINNODE },
    { "CONDITION", TOK_CONDITION },
    { NULL, 0 }
};

/* -------------------------------------------------------------------------
 * Error messages
 * ------------------------------------------------------------------------- */
enum {
    ERR_PROGRAM = 0,
    ERR_OPEN = 1,
    ERR_NO_LABEL = 3,
    ERR_ALREADY_DEFINED = 4,
    ERR_STRING = 5,
    ERR_ELSE_WITHOUT_IFDEF = 6,
    ERR_ENDIF_WITHOUT_IFDEF = 7,
    ERR_IFDEF_TOO_DEEP = 8,
    ERR_MISSING_ENDIF = 9,
    ERR_BAD_COMMAND = 10,
    ERR_DISK_WRITE = 11,
    ERR_OUT_OF_MEMORY = 12,
    ERR_ILLEGAL_CHAR = 13,
    ERR_UNEXPECTED_SYMBOL = 14,
    ERR_REPEAT_TOO_DEEP = 15,
    ERR_NO_MACRO_NAME = 17,
    ERR_PARAM_COUNT = 18,
    ERR_OUT_OF_RANGE = 19,
    ERR_STACK_OVERFLOW = 20,
    ERR_UNDEFINED_SYMBOL = 21,
    ERR_UNEXPECTED_EOF = 25,
    ERR_EOF_IN_COMMENT = 26,
    ERR_BAD_SYNTAX = 27,
    ERR_TOO_MANY_PARAMS = 28,
    /* Internal consistency errors; all are reported as "program error". */
    ERR_INTERNAL_QUEUE_SHORT = 1001,
    ERR_INTERNAL_ELSE_STATE = 1002,
    ERR_INTERNAL_IFDEF_UNDERFLOW = 1003,
    ERR_INTERNAL_TABLE_FULL = 1004,
    ERR_INTERNAL_QUEUE_EMPTY = 1005,
    ERR_INTERNAL_NO_FREE_NODE = 1006
};

/* Message texts, indexed by error code; codes beyond the table use entry 0. */
static const char *const k_error_text[] = {
    "program error:  ",
    "could not open:  ",
    "could not execute program:  ",
    "no label given for command",
    "already defined:  ",
    "string error",
    "$else without $ifdef",
    "$endif without $ifdef",
    "$ifdef nesting too deep",
    "missing $endif",
    "invalid preprocessor command:  $",
    "disk write error:  ",
    "out of memory:  ",
    "illegal character:  ",
    "unexpected symbol: ",
    "Repeat nesting too deep",
    "duplicate Macro function name: ",
    "missing Macro name",
    "incorrect number of parameters ",
    "out of range ",
    "internal stack overflow",
    "expression contains undefined symbol: ",
    "invalid library access key",
    "invalid library interface",
    "bad library file:  ",
    "unexpected end-of-file ",
    "reached end-of-file before ending comment ",
    "invalid syntax for preprocessor command: ",
    "too many defined parameters for macro (max 30): "
};
#define ERROR_TEXT_COUNT ((int)(sizeof(k_error_text) / sizeof(k_error_text[0])))

/* -------------------------------------------------------------------------
 * Limits
 * ------------------------------------------------------------------------- */
#define TOKEN_TEXT_SIZE 100
#define TEXT_BUF_SIZE 0x8000
#define MACRO_MAX_PARAMS 30
#define REPEAT_MAX_VARS 19
#define REPEAT_VALUES_MAX 0x10000  /* values of one $REPEAT variable */
#define EXPR_STACK_SIZE 99
#define FUSE_MAX 299
#define DEFINE_MAX 400
#define DEFINE_POOL_SIZE 16000
#define CHAR_QUEUE_NODES 260
#define IFDEF_MAX_DEPTH 12
#define WORD_BUF_SIZE 260

/* -------------------------------------------------------------------------
 * Phase-wide state
 * ------------------------------------------------------------------------- */
static void out_of_memory(const char *context);

static struct phase_rt g_rt = {
    .code_suffix = "cx",
    .messages = k_error_text,
    .message_count = ERROR_TEXT_COUNT,
    .text_size = 1024,
    .out_of_memory = out_of_memory,
};
static jmp_buf g_abort_jmp;
static const char *g_args[10];         /* copy of the phase argv */
static int g_error_count;
static int g_had_error;
static int g_keep_mx_file;             /* 'e' flag */
static char *g_mx_name;                /* <design>.mx */
static char *g_temp_name;              /* CA<pid> output of the define pass */

/* Directories searched for bare $include / $INCLUDE file names. */
static char g_pld_dir[PATH_MAX];
static char g_lib_dir[PATH_MAX];

/* -------------------------------------------------------------------------
 * Macro pass state
 * ------------------------------------------------------------------------- */
typedef struct macro {
    char *name;
    char *body;
    char *params[MACRO_MAX_PARAMS + 1]; /* NULL terminated */
    struct macro *next;
} macro_t;

typedef struct {
    char *name;
    int value;      /* value for the iteration being expanded */
    int *values;    /* list terminated by -1 */
} repeat_var_t;

typedef struct {
    int32_t address;
    int8_t value;
} fuse_entry_t;

static FILE *g_src;                      /* macro pass input */
static FILE *g_dst;                      /* macro pass output (.mx) */

static int g_tok;                        /* lookahead token */
static char g_tok_text[TOKEN_TEXT_SIZE]; /* text of the lookahead token */
static char g_prev_text[TOKEN_TEXT_SIZE];/* text of the token just consumed */
static int g_ws_count;                   /* blanks skipped before the lookahead */
static int g_line;                       /* source line number */
static int g_no_line_count;              /* reading an include or an expansion */
static int g_skip_ws;                    /* scanner drops blanks and newlines */
static int g_in_comment;
static int g_in_define_line;             /* on a $DEFINE line; no comment tokens */
static int g_keyword_limit;              /* 0 = all keywords, n = first n only */

static int g_capturing;                  /* emit() appends to g_capture_buf */
static char g_capture_buf[TEXT_BUF_SIZE];/* recorded macro / repeat body */
static int g_read_capture;               /* scanner reads g_capture_rd */
static const char *g_capture_rd;
static char g_work_buf[TEXT_BUF_SIZE];   /* macro expansion work text */
static int g_read_expansion;             /* scanner reads g_expansion_rd */
static const char *g_expansion_rd;
static int g_in_macro_expansion;
static int g_suppress_output;            /* never becomes nonzero */

static int g_in_table;                   /* after TABLE/SEQUENCE/..., '{' is literal */
static int g_brace_in_define;            /* a $DEFINE line contained '{' */
static int g_radix;                      /* 1 hex, 2 binary, 3 octal ('h' prefix seen) */
static int g_pin_counter;                /* counted but never used */

static macro_t *g_macros;
/* Lookahead saved across a macro call.  These are single global slots, so
 * nested calls share them. */
static int g_saved_tok;
static char g_saved_prev_text[TOKEN_TEXT_SIZE];
static char g_saved_tok_text[TOKEN_TEXT_SIZE];

static int g_repeat_depth;               /* -1 outside $REPEAT */
static int g_repeat_var_count;
static repeat_var_t g_repeat_vars[REPEAT_MAX_VARS + 1]; /* 1-based */
static int *g_repeat_values_wr;
static int *g_repeat_values_end;

static int g_expr_stack[EXPR_STACK_SIZE];
static int g_expr_sp;

static fuse_entry_t g_fuses[FUSE_MAX];
static int g_fuse_count;

/* -------------------------------------------------------------------------
 * Define pass state
 * ------------------------------------------------------------------------- */
typedef struct define_entry {
    const char *name;
    int name_len;
    int defined;                 /* 0 after $undef */
    const char *value;
    struct define_entry *next;   /* list ordered by descending name length */
} define_entry_t;

typedef struct char_node {
    char c;
    struct char_node *next;
} char_node_t;

typedef struct {
    char kind;          /* 1 = $ifdef/$ifndef part, 4 = $else part */
    char started_skip;  /* this level is the one that switched skipping on */
    char skipping;      /* text at this level is discarded */
} ifdef_level_t;

static FILE *g_pp_out;
static int g_pp_out_line;                      /* lines written so far */
static int g_pp_eof;                           /* directive hit end of file */
static int g_pp_eol;                           /* directive consumed its newline */
static char g_word[WORD_BUF_SIZE];
static char g_command[WORD_BUF_SIZE];

static define_entry_t g_defines[DEFINE_MAX];
static int g_define_count;
static define_entry_t *g_define_list;
static int g_max_define_len;
static char g_define_pool[DEFINE_POOL_SIZE + 2];
static char *g_define_pool_ptr;

/* Sliding window of pending output characters, as long as the longest
 * defined name, in which defined names are searched. */
static char_node_t g_queue_nodes[CHAR_QUEUE_NODES];
static char_node_t *g_queue_head;
static char_node_t *g_queue_tail;
static char_node_t *g_queue_free;
static int g_queue_len;

static ifdef_level_t g_ifdef_stack[IFDEF_MAX_DEPTH + 1];
static int g_ifdef_depth;
static int g_ifdef_skipping;

static void expand_tokens(void);
static void parse_expression(void);
static void preprocess_stream(FILE *in);

/* -------------------------------------------------------------------------
 * Errors and files (runtime in src/shared/phase.c)
 * ------------------------------------------------------------------------- */

/* Delete the output file of the define pass, if it was started. */
static void remove_temp_file(void)
{
    if (g_temp_name != NULL) {
        phase_remove(g_temp_name, 1);
    }
    g_temp_name = NULL;
}

/* Print one message: "[NNNNcx] line(N) text detail". */
static void print_error(int code, int line, const char *detail)
{
    phase_printf(&g_rt, stdout, "[%04d%s] ", code, g_rt.code_suffix);
    if (line > 0) {
        phase_printf(&g_rt, stdout, "line(%d) ", line);
    }
    phase_printf(&g_rt, stdout, "%s%s\n", phase_message(&g_rt, code), detail);
    fflush(stdout);
}

/* Report an error and continue */
static void report_error(int code, const char *detail, int line)
{
    g_error_count++;
    g_had_error = 1;
    print_error(code, line, detail);
}

/* Report an error and abandon the phase */
static void fatal_error(int code, const char *detail, int line)
{
    if (code >= 0) {
        report_error(code, detail, line);
    }
    remove_temp_file();
    fflush(g_dst);
    longjmp(g_abort_jmp, 1);
}

static void out_of_memory(const char *context)
{
    fatal_error(ERR_OUT_OF_MEMORY, context, 0);
}

/* Report a character that may not appear in the source, in hex. */
static void illegal_char_error(int code, int ch, int line)
{
    char detail[16];
    snprintf(detail, sizeof(detail), "'h'%x", (unsigned)(ch & 0xff));
    report_error(code, detail, line);
}

/* Open a file with a case-insensitive name lookup.  Bare names opened for
 * reading are also looked up next to the design and in the library / macro
 * directories.  Open files are tracked so that an abort can close them. */
static FILE *open_file(const char *path, const char *mode)
{
    FILE *fp = lincupl_fopen_ci(path, mode);
    if (fp == NULL && mode[0] == 'r') {
        char norm[PATH_MAX];
        lincupl_normalize_slashes(path, norm, sizeof(norm));
        if (strchr(norm, '/') == NULL) {
            char dirs[6][PATH_MAX + 32];
            int ndirs = 0;
            if (g_pld_dir[0]) {
                snprintf(dirs[ndirs++], sizeof(dirs[0]), "%s", g_pld_dir);
            }
            if (g_lib_dir[0]) {
                snprintf(dirs[ndirs++], sizeof(dirs[0]), "%s", g_lib_dir);
                snprintf(dirs[ndirs++], sizeof(dirs[0]), "%s/Macro", g_lib_dir);
                snprintf(dirs[ndirs++], sizeof(dirs[0]), "%s/../WinCupl/Macro", g_lib_dir);
                snprintf(dirs[ndirs++], sizeof(dirs[0]), "%s/../WinCupl/Fitters", g_lib_dir);
                snprintf(dirs[ndirs++], sizeof(dirs[0]), "%s/../Examples/Atmel", g_lib_dir);
            }
            for (int i = 0; i < ndirs && fp == NULL; i++) {
                char candidate[2 * PATH_MAX + 64];
                char resolved[PATH_MAX];
                snprintf(candidate, sizeof(candidate), "%s/%s", dirs[i], norm);
                if (lincupl_resolve_ci_path(candidate, resolved, sizeof(resolved)) == 0) {
                    fp = fopen(resolved, "r");
                }
            }
        }
    }
    return phase_track(&g_rt, fp, path, mode);
}

static void append_text(char *dst, size_t cap, const char *src)
{
    size_t used = strlen(dst);
    if (used + 1 < cap) {
        snprintf(dst + used, cap - used, "%s", src);
    }
}

/* Append to one of the large text buffers; overflow is a fatal error. */
static void append_big(char *dst, const char *src)
{
    if (strlen(dst) + strlen(src) + 1 > TEXT_BUF_SIZE) {
        fatal_error(ERR_STRING, "", 0);
    }
    strcat(dst, src);
}

static void copy_token_text(char *dst, const char *src)
{
    snprintf(dst, TOKEN_TEXT_SIZE, "%s", src);
}

/* =========================================================================
 * Macro pass: scanner
 * ========================================================================= */

/* Next character from the active source */
static int next_char(void)
{
    if (g_read_capture) {
        return (signed char)*g_capture_rd++;
    }
    if (g_read_expansion) {
        return (signed char)*g_expansion_rd++;
    }
    int c = phase_read_char(g_src);
    if (c > 0x7f) {
        char detail[48];
        snprintf(detail, sizeof(detail), "char %c : ASCII code:%d", c, c);
        fatal_error(ERR_ILLEGAL_CHAR, detail, g_line);
    }
    return c;
}

/* Push back the character last returned by next_char(). */
static void unread_char(int c)
{
    if (g_read_capture) {
        g_capture_rd--;
    } else if (g_read_expansion) {
        g_expansion_rd--;
    } else {
        ungetc(c, g_src);
    }
}

static int is_all_digits(const char *s)
{
    for (; *s != '\0'; s++) {
        if (!is_digit((unsigned char)*s)) {
            return 0;
        }
    }
    return 1;
}

/* Token of a keyword (case-insensitive) among the first g_keyword_limit
 * entries (all if 0), TOK_IDENT for anything else. */
static int lookup_keyword(const char *text)
{
    char upper[TOKEN_TEXT_SIZE];
    size_t n = 0;
    for (; text[n] != '\0' && n + 1 < sizeof(upper); n++) {
        upper[n] = (char)to_upper((unsigned char)text[n]);
    }
    upper[n] = '\0';
    for (int i = 0; k_keywords[i].name != NULL && (i < g_keyword_limit || g_keyword_limit == 0); i++) {
        if (strcmp(k_keywords[i].name, upper) == 0) {
            return k_keywords[i].token;
        }
    }
    return TOK_IDENT;
}

static void set_tok_text(int c0, int c1)
{
    g_tok_text[0] = (char)c0;
    g_tok_text[1] = (char)c1;
    g_tok_text[2] = '\0';
}

/* Second character of a two-character operator, or put it back. */
static int scan_pair(int second, int token_if_pair, int token_otherwise)
{
    int c = next_char();
    if (c == second) {
        g_tok_text[1] = (char)second;
        g_tok_text[2] = '\0';
        return token_if_pair;
    }
    unread_char(c);
    g_tok_text[1] = '\0';
    return token_otherwise;
}

/* Operator starting with c: *, **, =, ==, >, >=, <, <= */
static int scan_operator(int c)
{
    g_tok_text[0] = (char)c;
    switch (c) {
    case '*': return scan_pair('*', TOK_POWER, '*');
    case '=': return scan_pair('=', TOK_EQ, '=');
    case '>': return scan_pair('=', TOK_GE, TOK_GT);
    case '<': return scan_pair('=', TOK_LE, TOK_LT);
    default:
        fatal_error(ERR_UNEXPECTED_SYMBOL, g_prev_text, g_line);
        return 0;
    }
}

static int is_word_char(int c)
{
    return is_alnum(c) || c == '$' || c == '_' || c == '@';
}

/* Read the next token into g_tok_text; the previous token's text
 * moves to g_prev_text. */
static int scan_token(void)
{
    int c;

    g_ws_count = 0;
    copy_token_text(g_prev_text, g_tok_text);

    for (;;) {
        for (;;) {
            c = next_char();
            if (g_read_capture && c == 0) {
                return TOK_EOF;
            }
            if (!g_skip_ws || (c != ' ' && c != '\t')) {
                break;
            }
            g_ws_count++;
        }
        if (c != '\n') {
            break;
        }
        if (!g_no_line_count) {
            g_line++;
        }
        set_tok_text(c, 0);
        if (!g_skip_ws) {
            return '\n';
        }
    }

    if (is_word_char(c)) {
        size_t n = 0;
        while (is_word_char(c)) {
            if (n + 1 < TOKEN_TEXT_SIZE) {
                g_tok_text[n++] = (char)c;
            }
            c = next_char();
        }
        g_tok_text[n] = '\0';
        unread_char(c);
        if (is_all_digits(g_tok_text)) {
            return TOK_NUMBER;
        }
        return lookup_keyword(g_tok_text);
    }

    if (!g_in_comment && (c == '*' || c == '=' || c == '>' || c == '<')) {
        set_tok_text(c, 0);
        return scan_operator(c);
    }

    if (!g_in_define_line && c == '/') {
        g_tok_text[0] = (char)c;
        if (scan_pair('*', TOK_COMMENT_OPEN, '/') == TOK_COMMENT_OPEN) {
            g_in_comment = 1;
            return TOK_COMMENT_OPEN;
        }
        return '/';
    }

    if (!g_in_define_line && c == '*') {
        /* only reached inside a comment */
        g_tok_text[0] = (char)c;
        if (scan_pair('/', TOK_COMMENT_CLOSE, '*') == TOK_COMMENT_CLOSE) {
            g_in_comment = 0;
            return TOK_COMMENT_CLOSE;
        }
        return '*';
    }

    set_tok_text(c, 0);
    return c;
}

/* Consume the lookahead token, which must be `expected` */
static void expect(int expected)
{
    if (g_tok == expected) {
        g_tok = scan_token();
    } else if (g_tok == TOK_EOF) {
        fatal_error(ERR_UNEXPECTED_EOF, "", g_line);
    } else {
        fatal_error(ERR_UNEXPECTED_SYMBOL, g_prev_text, g_line);
    }
}

/* Write text to the .mx file, or to the body being recorded */
static void emit(const char *text)
{
    if (g_capturing) {
        append_big(g_capture_buf, text);
    } else if (!g_suppress_output) {
        fputs(text, g_dst);
    }
}

static void expect_and_emit(int expected)
{
    expect(expected);
    emit(g_prev_text);
}

/* Some messages deliberately report the line number truncated to a signed
 * byte (line 200 is reported as -56). */
static int line_as_byte(void)
{
    return (signed char)g_line;
}

/* Discard a comment.  The line reported for an unterminated comment is the
 * one where the file ended; some callers report it truncated to a byte. */
static void skip_comment(int byte_line)
{
    expect(TOK_COMMENT_OPEN);
    while (g_tok != TOK_COMMENT_CLOSE && g_tok != TOK_EOF) {
        expect(g_tok);
    }
    if (g_tok == TOK_COMMENT_CLOSE) {
        expect(TOK_COMMENT_CLOSE);
    } else {
        fatal_error(ERR_EOF_IN_COMMENT, "", byte_line ? line_as_byte() : g_line);
    }
}

/* Copy a comment through to the output unchanged. */
static void copy_comment(int byte_line)
{
    expect_and_emit(TOK_COMMENT_OPEN);
    while (g_tok != TOK_COMMENT_CLOSE && g_tok != TOK_EOF) {
        expect_and_emit(g_tok);
    }
    if (g_tok == TOK_COMMENT_CLOSE) {
        expect_and_emit(TOK_COMMENT_CLOSE);
    } else {
        fatal_error(ERR_EOF_IN_COMMENT, "", byte_line ? line_as_byte() : g_line);
    }
}

static int at_line_end(void)
{
    return g_tok == '\n' || g_tok == TOK_EOF || g_tok == TOK_END;
}

/* =========================================================================
 * Macro pass: {expression} evaluation
 * ========================================================================= */

static void expr_push(int value)
{
    if (g_expr_sp >= EXPR_STACK_SIZE) {
        fatal_error(ERR_STACK_OVERFLOW, "", 0);
    }
    g_expr_stack[g_expr_sp++] = value;
}

static int expr_pop(void)
{
    if (g_expr_sp == 0) {
        fatal_error(ERR_STACK_OVERFLOW, "", 0);
    }
    return g_expr_stack[--g_expr_sp];
}

/* Index of a $REPEAT variable, 0 if unknown */
static int find_repeat_var(const char *name)
{
    for (int i = g_repeat_var_count; i >= 1; i--) {
        if (strcmp(name, g_repeat_vars[i].name) == 0) {
            return i;
        }
    }
    return 0;
}

static int wrap_mul(int a, int b)
{
    return (int)((uint32_t)a * (uint32_t)b);
}

/* ( expr ) | number | repeat variable | log<base>( expr ) */
static void parse_primary(void)
{
    if (g_tok == '(') {
        expect('(');
        parse_expression();
        expect(')');
    } else if (g_tok == TOK_NUMBER) {
        expect(TOK_NUMBER);
        expr_push((int)strtol(g_prev_text, NULL, 10));
    } else if (g_tok == TOK_IDENT) {
        expect(TOK_IDENT);
        if (strncmp(g_prev_text, "log", 3) != 0 && strncmp(g_prev_text, "LOG", 3) != 0) {
            int var = find_repeat_var(g_prev_text);
            if (var == 0) {
                fatal_error(ERR_UNDEFINED_SYMBOL, g_prev_text, 0);
            }
            expr_push(g_repeat_vars[var].value);
            return;
        }
        /* log2, log8, log10, log16: number of digits needed for the value */
        char base_char = g_prev_text[3];
        expect('(');
        parse_expression();
        expect(')');
        int value = expr_pop();
        int base = (base_char == '1') ? 16 : (base_char == '2') ? 2 : (base_char == '8') ? 8 : 10;
        int power = 1;
        int digits = 0;
        if (value > 1) {
            do {
                power = wrap_mul(power, base);
                digits++;
            } while (power < value);
        }
        expr_push(digits);
    } else {
        fatal_error(ERR_UNEXPECTED_SYMBOL, g_prev_text, line_as_byte());
    }
}

/* primary [ ** primary ] */
static void parse_power(void)
{
    parse_primary();
    if (g_tok == TOK_POWER) {
        expect(TOK_POWER);
        parse_primary();
        int exponent = expr_pop();
        int base = expr_pop();
        int result = 1;
        for (int i = 0; i < exponent; i++) {
            result = wrap_mul(result, base);
        }
        expr_push(result);
    }
}

/* power { (*|/|%) power } */
static void parse_term(void)
{
    parse_power();
    while (g_tok == '*' || g_tok == '/' || g_tok == '%') {
        int op = g_tok;
        expect(g_tok);
        parse_power();
        int rhs = expr_pop();
        int lhs = expr_pop();
        int result;
        if (op == '*') {
            result = wrap_mul(lhs, rhs);
        } else if (rhs == 0 || (lhs == INT_MIN && rhs == -1)) {
            /* division by zero or overflow: quotient 0, remainder the dividend */
            result = (op == '/') ? 0 : lhs;
        } else {
            result = (op == '/') ? lhs / rhs : lhs % rhs;
        }
        expr_push(result);
    }
}

/* term { (+|-) term } */
static void parse_additive(void)
{
    parse_term();
    while (g_tok == '+' || g_tok == '-') {
        int op = g_tok;
        expect(g_tok);
        parse_term();
        uint32_t rhs = (uint32_t)expr_pop();
        uint32_t lhs = (uint32_t)expr_pop();
        expr_push((int)(op == '+' ? lhs + rhs : lhs - rhs));
    }
}

/* Value (1 or 0) of a comparison operator token applied to two values */
static int compare(int op, int lhs, int rhs)
{
    switch (op) {
    case TOK_EQ: return lhs == rhs;
    case TOK_NE: return lhs != rhs;
    case TOK_GT: return lhs > rhs;
    case TOK_LT: return lhs < rhs;
    case TOK_GE: return lhs >= rhs;
    case TOK_LE: return lhs <= rhs;
    default:
        fatal_error(ERR_UNEXPECTED_SYMBOL, "invalid operator", 0);
        return 0;
    }
}

/* additive { (==|!=|>|<|>=|<=) additive }; result on the stack */
static void parse_expression(void)
{
    parse_additive();
    while (g_tok >= TOK_EQ && g_tok <= TOK_LE) {
        int op = g_tok;
        expect(g_tok);
        parse_additive();
        int rhs = expr_pop();
        int lhs = expr_pop();
        expr_push(compare(op, lhs, rhs));
    }
}

/* Low 16 bits in binary without leading zeros */
static void format_binary(char *dst, int value)
{
    if (value == 0) {
        strcpy(dst, "0");
        return;
    }
    int started = 0;
    for (int bit = 0; bit < 16; bit++) {
        int set = ((uint32_t)value << bit) & 0x8000u;
        if (set) {
            started = 1;
        }
        if (started) {
            *dst++ = set ? '1' : '0';
        }
    }
    *dst = '\0';
}

/* Emit the value of the expression just parsed, in the radix selected by a
 * preceding 'h' / 'b' / 'o' prefix (decimal otherwise). */
static void emit_expression_value(void)
{
    int value = expr_pop();
    if (g_radix == 1) {
        snprintf(g_prev_text, sizeof(g_prev_text), "%x", (unsigned)value);
    } else if (g_radix == 3) {
        snprintf(g_prev_text, sizeof(g_prev_text), "%o", (unsigned)value);
    } else if (g_radix == 2) {
        format_binary(g_prev_text, value);
    } else {
        snprintf(g_prev_text, sizeof(g_prev_text), "%d", value);
    }
    g_radix = 0;
    emit(g_prev_text);
}

/* Replace "{ expr }" by its value */
static void eval_braces(void)
{
    int saved_skip_ws = g_skip_ws;
    g_skip_ws = 1;
    expect('{');
    parse_expression();
    emit_expression_value();
    g_skip_ws = saved_skip_ws;
    expect('}');
}

/* As eval_braces, after the '{' has been consumed */
static void eval_braces_body(void)
{
    g_skip_ws = 1;
    parse_expression();
    emit_expression_value();
    g_skip_ws = 0;
}

/* Radix selected by a number prefix letter */
static int radix_of_prefix(const char *text)
{
    if (strcmp(text, "H") == 0 || strcmp(text, "h") == 0) return 1;
    if (strcmp(text, "B") == 0 || strcmp(text, "b") == 0) return 2;
    if (strcmp(text, "O") == 0 || strcmp(text, "o") == 0) return 3;
    return 0;
}

/* Copy a number prefix such as 'h' and remember its radix */
static void emit_number_prefix(void)
{
    expect('\'');
    emit("'");
    if (g_tok == TOK_IDENT) {
        g_radix = radix_of_prefix(g_tok_text);
        if (g_radix != 0) {
            expect_and_emit(TOK_IDENT);
            expect('\'');
            emit("'");
        }
    }
}

/* =========================================================================
 * Macro pass: statements
 * ========================================================================= */

/* FUSE(address, value) - collected for fuse.tmp */
static void parse_fuse(void)
{
    g_skip_ws = 1;
    expect(TOK_FUSE);
    expect('(');
    expect(TOK_NUMBER);
    int address = (int)strtol(g_prev_text, NULL, 10);
    expect(',');
    expect(TOK_NUMBER);
    int value = (int)strtol(g_prev_text, NULL, 10);
    expect(')');
    if (g_fuse_count >= FUSE_MAX) {
        fatal_error(ERR_OUT_OF_RANGE, "", 0);
    }
    g_fuses[g_fuse_count].address = address;
    g_fuses[g_fuse_count].value = (int8_t)value;
    g_fuse_count++;
    g_skip_ws = 0;
}

/* Write the FUSE() statements to fuse.tmp: per statement a 32-bit
 * little-endian address and one value byte. */
static void write_fuse_file(void)
{
    FILE *fp = open_file("fuse.tmp", "w");
    if (fp == NULL) {
        fatal_error(ERR_OPEN, "temporary fuse file", 0);
    }
    for (int i = 0; i < g_fuse_count; i++) {
        uint32_t address = (uint32_t)g_fuses[i].address;
        uint8_t record[5] = {
            (uint8_t)address, (uint8_t)(address >> 8), (uint8_t)(address >> 16),
            (uint8_t)(address >> 24), (uint8_t)g_fuses[i].value
        };
        fwrite(record, 4, 1, fp);
        fwrite(record + 4, 1, 1, fp);
    }
    phase_close(&g_rt, fp);
}

/* $IF(expr) ... [$ELSE ...] $IFEND */
static void parse_if(void)
{
    g_skip_ws = 1;
    expect(TOK_IF);
    expect('(');
    parse_expression();
    g_skip_ws = 0;
    expect(')');
    int condition = expr_pop();

    for (int else_part = 0; else_part < 2; else_part++) {
        int keep = else_part ? (condition == 0) : (condition != 0);
        while (g_tok != TOK_IFEND && g_tok != TOK_EOF && g_tok != TOK_END &&
               (else_part || g_tok != TOK_ELSE)) {
            if (!keep) {
                expect(g_tok);
                continue;
            }
            if (g_tok == '\'') {
                emit_number_prefix();
            }
            if (g_tok == '{' && !g_capturing) {
                eval_braces();
            } else {
                expect_and_emit(g_tok);
            }
        }
        if (g_tok != TOK_ELSE) {
            break;
        }
        expect(TOK_ELSE);
    }

    expect(TOK_IFEND);
    while (!at_line_end()) {
        expect(g_tok);
    }
    if (g_tok == '\n') {
        expect('\n');
    }
}

/* Expand another source file in place */
static void include_file(const char *path)
{
    FILE *saved_src = g_src;
    int saved_tok = g_tok;
    char saved_tok_text[TOKEN_TEXT_SIZE];
    char saved_prev_text[TOKEN_TEXT_SIZE];
    copy_token_text(saved_tok_text, g_tok_text);
    copy_token_text(saved_prev_text, g_prev_text);

    g_src = open_file(path, "r");
    if (g_src == NULL) {
        fatal_error(ERR_OPEN, path, 0);
    }
    g_no_line_count = 1;
    expand_tokens();
    g_no_line_count = 0;
    phase_close(&g_rt, g_src);

    g_src = saved_src;
    g_tok = saved_tok;
    copy_token_text(g_tok_text, saved_tok_text);
    copy_token_text(g_prev_text, saved_prev_text);
}

/* $INCLUDE file */
static void parse_include(void)
{
    char path[PATH_MAX];
    int saved_suppress = g_suppress_output;
    g_suppress_output = 0;

    g_skip_ws = 1;
    expect(TOK_INCLUDE);
    g_skip_ws = 0;
    path[0] = '\0';
    expect(g_tok);
    append_text(path, sizeof(path), g_prev_text);
    while (g_tok != '\n' && g_tok != TOK_EOF) {
        expect(g_tok);
        append_text(path, sizeof(path), g_prev_text);
    }
    expect('\n');

    char *cut = strchr(path, ' ');
    if (cut != NULL) {
        *cut = '\0';
    }
    cut = strchr(path, '/');
    if (cut != NULL && cut[1] == '*') {
        *cut = '\0';
    }
    include_file(path);
    if (saved_suppress) {
        emit("\n");
    }
    g_suppress_output = saved_suppress;
}

/* $DEFINE line - copied through for the define pass */
static void parse_define_line(void)
{
    int saved_suppress = g_suppress_output;
    g_suppress_output = 0;
    g_in_define_line = 1;
    expect_and_emit(TOK_DEFINE);
    while (!at_line_end()) {
        expect(g_tok);
        if (strcmp(g_prev_text, "{") == 0) {
            g_brace_in_define = 1;
        }
        emit(g_prev_text);
    }
    if (g_tok == '\n') {
        expect('\n');
        emit("\n");
    }
    g_in_define_line = 0;
    g_suppress_output = saved_suppress;
}

/* Number formed by the trailing digits of a name ("A12" -> 12). */
static int trailing_number(const char *text)
{
    size_t start = strlen(text);
    while (start > 0 && is_digit((unsigned char)text[start - 1])) {
        start--;
    }
    return (int)strtol(text + start, NULL, 10);
}

/* name[..end] {, name[..end]} ]  - returns the number of pins.
 * The list text is rebuilt in pin_list. */
static int parse_pin_list(char *pin_list, size_t cap)
{
    int count = 1;
    int first = 0;
    int last = 0;
    size_t len = strlen(g_tok_text);
    int last_char = len ? (unsigned char)g_tok_text[len - 1] : 0;

    if (!is_alpha(last_char) && last_char != '_') {
        first = trailing_number(g_tok_text);
    }
    append_text(pin_list, cap, g_tok_text);
    expect(TOK_IDENT);

    if (g_tok == '.') {
        expect('.');
        expect('.');
        append_text(pin_list, cap, "..");
        if (g_tok == TOK_NUMBER) {
            last = (int)strtol(g_tok_text, NULL, 10);
            expect(TOK_NUMBER);
        } else if (g_tok == TOK_IDENT) {
            last = trailing_number(g_tok_text);
            expect(TOK_IDENT);
        } else {
            fatal_error(ERR_UNEXPECTED_SYMBOL, g_tok_text, 0);
        }
        append_text(pin_list, cap, g_prev_text);
        count = (first < last) ? (last - first + 1) : (first - last + 1);
    }

    if (g_tok == ',') {
        append_text(pin_list, cap, ",");
        expect(',');
        count += parse_pin_list(pin_list, cap);
    } else if (g_tok == ']') {
        append_text(pin_list, cap, "]");
    } else {
        fatal_error(ERR_UNEXPECTED_SYMBOL, g_tok_text, 0);
    }
    return count;
}

/* The keyword of a PIN / PINNODE declaration and, when it has no number
 * ("PIN = name" or "PIN = [list]"), the placeholder number 0. */
static void parse_pin_head(void)
{
    g_skip_ws = 1;
    expect_and_emit(g_tok == TOK_PIN ? TOK_PIN : TOK_PINNODE);

    if (g_tok == '=') {
        expect('=');
        if (g_tok == '[') {
            char pin_list[1024];
            expect('[');
            strcpy(pin_list, "= [");
            emit(" [");
            emit("0");
            int count = parse_pin_list(pin_list, sizeof(pin_list));
            if (count > 1) {
                g_pin_counter += count - 1;
                emit("..");
                emit("0");
            }
            g_pin_counter++;
            emit("] ");
            emit(pin_list);
            g_skip_ws = 0;
            while (g_tok != ';') {
                expect(g_tok);
            }
        } else {
            emit("  ");
            emit("0");
            emit("  = ");
        }
    } else {
        for (int i = 0; i < g_ws_count; i++) {
            emit(" ");
        }
    }
    g_skip_ws = 0;
}

/* PIN / PINNODE declaration and the rest of its line.  A further PIN or
 * PINNODE statement on the line gets its placeholder number as well; the
 * original copied the rest of the line as it was, so a second
 * "PIN = name;" on one line was a syntax error. */
static void parse_pin(void)
{
    int saved_suppress = g_suppress_output;
    int in_comment = 0;
    int statement_start = 0;
    g_suppress_output = 0;
    parse_pin_head();

    while (!at_line_end()) {
        if (!in_comment && statement_start && (g_tok == TOK_PIN || g_tok == TOK_PINNODE)) {
            parse_pin_head();
            statement_start = 0;
            continue;
        }
        if (g_tok == TOK_COMMENT_OPEN) {
            in_comment = 1;
        } else if (g_tok == TOK_COMMENT_CLOSE) {
            in_comment = 0;
        } else if (!in_comment && g_tok != ' ' && g_tok != '\t') {
            statement_start = (g_tok == ';');
        }
        expect_and_emit(g_tok);
    }
    if (g_tok == '\n') {
        expect('\n');
        emit("\n");
    }
    g_skip_ws = 0;
    g_suppress_output = saved_suppress;
}

/* FIELD list member name, optionally followed by {expression} */
static void parse_field_name(void)
{
    expect_and_emit(TOK_IDENT);
    if (g_tok == '{') {
        eval_braces();
    }
}

/* End of a FIELD list range: {expression}, number or name */
static void parse_field_range_end(void)
{
    if (g_tok == '{') {
        eval_braces();
    } else if (g_tok == TOK_NUMBER) {
        expect_and_emit(TOK_NUMBER);
    } else {
        parse_field_name();
    }
}

/* FIELD list member: name or name..end */
static void parse_field_item(void)
{
    parse_field_name();
    if (g_tok == '.') {
        expect('.');
        expect('.');
        emit("..");
        parse_field_range_end();
    }
}

/* FIELD name = [list] inside a macro expansion */
static void parse_field(void)
{
    int saved_skip_ws = g_skip_ws;
    g_skip_ws = 1;
    expect_and_emit(TOK_FIELD);
    for (int i = 0; i < g_ws_count; i++) {
        emit(" ");
    }
    expect_and_emit(TOK_IDENT);
    expect('=');
    expect('[');
    emit(" = [");
    parse_field_item();
    while (g_tok == ',') {
        expect(',');
        emit(", ");
        parse_field_item();
    }
    expect(']');
    emit("];\n");
    g_skip_ws = saved_skip_ws;
}

/* -------------------------------------------------------------------------
 * $REPEAT
 * ------------------------------------------------------------------------- */

/* Add a $REPEAT variable with an empty value list. */
static void declare_repeat_var(const char *name)
{
    if (find_repeat_var(name) != 0 || g_repeat_var_count >= REPEAT_MAX_VARS) {
        fatal_error(ERR_INTERNAL_TABLE_FULL, "symbol table", 0);
    }
    repeat_var_t *var = &g_repeat_vars[++g_repeat_var_count];
    var->name = phase_strdup(&g_rt, name, "in allocating symbol table");
    var->values = phase_alloc(&g_rt, (REPEAT_VALUES_MAX + 1) * sizeof(int), "in macro expanding");
    g_repeat_values_wr = var->values;
    g_repeat_values_end = var->values + REPEAT_VALUES_MAX;
    *g_repeat_values_wr = -1;
}

static void add_repeat_value(int value)
{
    if (g_repeat_values_wr >= g_repeat_values_end) {
        fatal_error(ERR_OUT_OF_RANGE, "for Repeat statement", 0);
    }
    *g_repeat_values_wr++ = value;
    *g_repeat_values_wr = -1;
}

/* Bound of a $REPEAT range: an expression, optionally in braces */
static int parse_repeat_bound(void)
{
    if (g_tok == '{') {
        expect('{');
        parse_expression();
        expect('}');
    } else {
        parse_expression();
    }
    return expr_pop();
}

/* Value or first..last (ascending or descending) */
static void parse_repeat_range(void)
{
    int first = parse_repeat_bound();
    int last = first;
    if (g_tok == '.') {
        expect('.');
        expect('.');
        last = parse_repeat_bound();
    }
    int64_t span = (int64_t)last - (int64_t)first;
    if (span > 0xffff || span < -0xffff) {
        fatal_error(ERR_OUT_OF_RANGE, "for Repeat statement", 0);
    }
    if (first < last) {
        for (int v = first; v <= last; v++) {
            add_repeat_value(v);
        }
    } else {
        for (int v = first; v >= last; v--) {
            add_repeat_value(v);
        }
    }
}

static void skip_comment_if_any(void)
{
    if (g_tok == TOK_COMMENT_OPEN) {
        skip_comment(1);
    }
}

/* $REPEAT var = [list]; the caller then records the body */
static void begin_repeat(void)
{
    g_skip_ws = 1;
    g_repeat_depth++;
    expect(TOK_REPEAT);
    skip_comment_if_any();
    expect(TOK_IDENT);
    declare_repeat_var(g_prev_text);
    skip_comment_if_any();
    expect('=');
    skip_comment_if_any();
    expect('[');
    parse_repeat_range();
    while (g_tok == ',') {
        expect(',');
        parse_repeat_range();
    }
    g_skip_ws = 0;
    skip_comment_if_any();
    expect(']');
    while (g_tok != '\n') {
        if (g_tok == '\t' || g_tok == ' ') {
            expect(g_tok);
        } else if (g_tok == TOK_COMMENT_OPEN) {
            skip_comment(1);
        } else {
            fatal_error(ERR_BAD_SYNTAX, " $REPEAT ", line_as_byte());
        }
    }
    expect('\n');
    if (g_repeat_depth == 0) {
        g_capture_buf[0] = '\0';
    }
}

/* The identifier just scanned from a recorded body names one of the
 * functions of {expressions} (log2, log8, log10, log16) and is followed by
 * its '(' */
static int at_function_call(void)
{
    if (g_tok != TOK_IDENT ||
        (strncmp(g_tok_text, "log", 3) != 0 && strncmp(g_tok_text, "LOG", 3) != 0)) {
        return 0;
    }
    const char *p = g_capture_rd;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return *p == '(';
}

/* Expand the recorded $REPEAT body once with the current values */
static void expand_repeat_body(void)
{
    g_capture_rd = g_capture_buf;
    g_read_capture = 1;
    g_tok = scan_token();

    for (;;) {
        int saved_skip_ws = g_skip_ws;
        if (g_tok == TOK_EOF) {
            g_read_capture = 0;
            return;
        }
        if (g_tok == '{') {
            g_skip_ws = 1;
            expect('{');
            if (g_tok == TOK_NUMBER || g_tok == '(' ||
                (g_tok == TOK_IDENT && find_repeat_var(g_tok_text) > 0) || at_function_call()) {
                eval_braces_body();
                expect('}');
                g_skip_ws = saved_skip_ws;
            } else {
                /* not an expression: keep the brace (blank skipping stays on) */
                emit("{");
            }
        } else if (g_tok == '\'') {
            emit_number_prefix();
        } else if (g_tok == TOK_IF) {
            parse_if();
        } else if (g_tok == TOK_FUSE) {
            parse_fuse();
            expect(';');
        } else if (g_tok == TOK_NUMBER) {
            expect_and_emit(g_tok);
        } else if (g_tok == TOK_IDENT) {
            g_radix = 0;
            expect_and_emit(g_tok);
        } else if (g_tok == TOK_COMMENT_OPEN) {
            copy_comment(1);
        } else {
            expect(g_tok);
            g_radix = 0;
            emit(g_prev_text);
        }
    }
}

/* $REPEND - expand the body for every value (for two nested
 * $REPEATs, for every combination of values) */
static void end_repeat(void)
{
    g_repeat_depth--;
    if (g_repeat_depth >= 0) {
        return;
    }
    g_capturing = 0;
    if (g_repeat_var_count > 2) {
        fatal_error(ERR_REPEAT_TOO_DEEP, " ", line_as_byte());
    }

    int saved_no_line_count = g_no_line_count;
    int saved_tok = g_tok;
    char saved_tok_text[TOKEN_TEXT_SIZE];
    char saved_prev_text[TOKEN_TEXT_SIZE];
    g_no_line_count = 1;
    copy_token_text(saved_tok_text, g_tok_text);
    copy_token_text(saved_prev_text, g_prev_text);

    for (const int *outer = g_repeat_vars[1].values; outer != NULL && *outer != -1; outer++) {
        g_repeat_vars[1].value = *outer;
        if (g_repeat_var_count == 2) {
            for (const int *inner = g_repeat_vars[2].values; *inner != -1; inner++) {
                g_repeat_vars[2].value = *inner;
                expand_repeat_body();
            }
        } else {
            expand_repeat_body();
        }
    }

    g_no_line_count = saved_no_line_count;
    g_tok = saved_tok;
    copy_token_text(g_tok_text, saved_tok_text);
    copy_token_text(g_prev_text, saved_prev_text);
    g_repeat_var_count = 0;
}

/* -------------------------------------------------------------------------
 * $MACRO
 * ------------------------------------------------------------------------- */

static macro_t *find_macro(const char *name)
{
    for (macro_t *m = g_macros; m != NULL; m = m->next) {
        if (strcmp(m->name, name) == 0) {
            return m;
        }
    }
    return NULL;
}

/* $MACRO name param...; the caller then records the body */
static void begin_macro(void)
{
    g_keyword_limit = 2;
    expect(TOK_MACRO);
    while (g_tok != TOK_IDENT && g_tok != TOK_EOF && g_tok != TOK_END && g_tok != TOK_COMMENT_OPEN) {
        expect(g_tok);
    }
    if (g_tok == TOK_COMMENT_OPEN) {
        skip_comment(0);
    }
    if (g_tok != TOK_IDENT) {
        fatal_error(ERR_NO_MACRO_NAME, "", 0);
    }
    expect(TOK_IDENT);

    macro_t *m = phase_alloc(&g_rt, sizeof(*m), "expanding macro");
    memset(m, 0, sizeof(*m));
    m->name = phase_strdup(&g_rt, g_prev_text, "expanding macro");

    int nparams = 0;
    while (!at_line_end()) {
        while (g_tok != TOK_IDENT && g_tok != TOK_EOF && g_tok != '\n' && g_tok != TOK_COMMENT_OPEN) {
            expect(g_tok);
        }
        if (g_tok == TOK_IDENT) {
            if (nparams >= MACRO_MAX_PARAMS) {
                fatal_error(ERR_TOO_MANY_PARAMS, m->name, 0);
            }
            expect(TOK_IDENT);
            m->params[nparams++] = phase_strdup(&g_rt, g_prev_text, "expanding macro");
        } else if (g_tok == TOK_COMMENT_OPEN) {
            skip_comment(0);
        }
    }
    if (g_tok == '\n') {
        expect('\n');
    }
    m->params[nparams] = NULL;

    m->next = g_macros;
    g_macros = m;
    g_capture_buf[0] = '\0';
}

/* $MEND - the recorded text becomes the macro body */
static void end_macro(void)
{
    g_macros->body = phase_strdup(&g_rt, g_capture_buf, "loading macro ");
    g_keyword_limit = 0;
    expect(TOK_MEND);
    while (g_tok == '\n') {
        expect('\n');
    }
}

/* Replace every occurrence of one parameter in g_work_buf by its argument.
 * The argument NC (no connect) removes the whole statement when the
 * parameter starts it, and reads as constant 'b'1 elsewhere. */
static void substitute_macro_param(const char *param, const char *arg)
{
    int at_statement_start = 1;

    g_capture_rd = g_work_buf;
    g_capture_buf[0] = '\0';
    g_tok = scan_token();
    while (g_tok != TOK_EOF) {
        if (g_tok == TOK_IDENT && strcmp(g_tok_text, param) == 0) {
            expect(g_tok);
            if (strcmp(arg, "NC") != 0) {
                emit(arg);
                at_statement_start = 0;
            } else if (!at_statement_start) {
                emit("'b'1");
            } else {
                while (g_tok != ';') {
                    expect(g_tok);
                }
                expect(';');
                expect(g_tok);
            }
        } else {
            if (g_tok == TOK_IDENT || g_tok == TOK_NUMBER) {
                at_statement_start = 0;
            } else if (g_tok == ';') {
                at_statement_start = 1;
            } else if (g_tok != '\n') {
                at_statement_start = 0;
            }
            expect_and_emit(g_tok);
        }
    }
    snprintf(g_work_buf, sizeof(g_work_buf), "%s", g_capture_buf);
}

/* name(arg, ...); - expand a macro call */
static void expand_macro(const macro_t *m)
{
    char *args[MACRO_MAX_PARAMS];
    int nargs = 0;

    g_skip_ws = 1;
    int saved_no_line_count = g_no_line_count;
    g_no_line_count = 1;
    expect(TOK_IDENT);
    expect('(');
    while (g_tok != ')') {
        g_work_buf[0] = '\0';
        while (g_tok != ',' && g_tok != ')') {
            expect(g_tok);
            append_big(g_work_buf, g_prev_text);
        }
        if (m->params[nargs] == NULL) {
            fatal_error(ERR_PARAM_COUNT, "for Macro call", g_line);
        }
        args[nargs++] = phase_strdup(&g_rt, g_work_buf, "expanding macro ");
        if (g_tok == ',') {
            expect(',');
        }
    }
    if (m->params[nargs] != NULL) {
        fatal_error(ERR_PARAM_COUNT, "for Macro call", g_line);
    }
    expect(')');

    snprintf(g_work_buf, sizeof(g_work_buf), "%s", m->body);
    g_skip_ws = 0;
    g_capturing = 1;
    g_saved_tok = g_tok;
    copy_token_text(g_saved_prev_text, g_prev_text);
    copy_token_text(g_saved_tok_text, g_tok_text);
    g_keyword_limit = 2;
    g_read_capture = 1;
    for (int i = 0; m->params[i] != NULL; i++) {
        substitute_macro_param(m->params[i], args[i]);
    }
    g_capturing = 0;
    g_keyword_limit = 0;
    g_read_capture = 0;

    /* run the substituted body through the normal token expansion */
    g_read_expansion = 1;
    g_expansion_rd = g_work_buf;
    g_in_macro_expansion = 1;
    expand_tokens();
    g_in_macro_expansion = 0;
    g_read_expansion = 0;

    g_no_line_count = saved_no_line_count;
    g_tok = g_saved_tok;
    copy_token_text(g_prev_text, g_saved_prev_text);
    copy_token_text(g_tok_text, g_saved_tok_text);
    expect(';');
}

/* -------------------------------------------------------------------------
 * Macro pass main loop
 * ------------------------------------------------------------------------- */

/* Copy tokens from the active source to the output, handling the
 * preprocessor statements.  Returns at the end of the source. */
static void expand_tokens(void)
{
    g_tok = scan_token();
    for (;;) {
        switch (g_tok) {
        case TOK_EOF:
            if (g_capturing) {
                fatal_error(ERR_UNEXPECTED_EOF, "", g_line);
            }
            return;
        case TOK_END:
            return;

        case TOK_REPEAT:
            begin_repeat();
            g_capturing = 1;
            continue;
        case TOK_REPEND:
            end_repeat();
            expect(TOK_REPEND);
            expect('\n');
            continue;
        case TOK_INCLUDE:
            parse_include();
            continue;
        case TOK_PIN:
        case TOK_PINNODE:
            parse_pin();
            continue;
        case '\'':
            emit_number_prefix();
            continue;
        case TOK_FUSE:
            if (g_capturing) {
                break;
            }
            parse_fuse();
            expect(';');
            continue;
        case TOK_MACRO:
            g_capturing = 1;
            begin_macro();
            continue;
        case TOK_DEFINE:
            parse_define_line();
            continue;
        case TOK_MEND:
            g_capturing = 0;
            end_macro();
            continue;

        case '{':
            /* "{expr}" is evaluated unless it opens a TABLE/SEQUENCE/... body,
             * a body is being recorded, or a $DEFINE used a brace. */
            if (!g_in_table && !g_capturing && (!g_brace_in_define || g_in_macro_expansion)) {
                eval_braces();
            } else {
                expect_and_emit('{');
                g_in_table = 0;
            }
            continue;
        case TOK_TABLE:
        case TOK_SEQUENCE:
        case TOK_FUNCTION:
        case TOK_SEQUENCED:
        case TOK_SEQUENCERS:
        case TOK_SEQUENCEJK:
        case TOK_SEQUENCET:
        case TOK_CONDITION:
        case TOK_PROPERTY:
        case TOK_INIT_TO:
            expect_and_emit(g_tok);
            g_in_table = 1;
            continue;

        case TOK_COMMENT_OPEN:
            copy_comment(0);
            continue;
        case TOK_IF:
            if (g_capturing) {
                break;
            }
            parse_if();
            continue;
        case TOK_FIELD:
            if (!g_in_macro_expansion) {
                break;
            }
            parse_field();
            expect(';');
            continue;

        case TOK_IDENT: {
            const macro_t *m = find_macro(g_tok_text);
            if (m != NULL) {
                expand_macro(m);
                continue;
            }
            break;
        }
        default:
            break;
        }
        expect_and_emit(g_tok);
    }
}

/* Macro pass: expand `in` into `out`, then write fuse.tmp if there were
 * FUSE() statements. */
static void run_macro_pass(FILE *in, FILE *out)
{
    g_capture_buf[0] = '\0';
    g_src = in;
    g_dst = out;
    expand_tokens();
    if (g_fuse_count > 0) {
        write_fuse_file();
    }
}

/* =========================================================================
 * Define pass
 * ========================================================================= */

static void pp_check_write(void)
{
    fflush(g_pp_out);
    if (ferror(g_pp_out)) {
        fatal_error(ERR_DISK_WRITE, g_temp_name, 0);
    }
}

static void pp_put_char(int c)
{
    if (c != 0) {
        fputc(c, g_pp_out);
    }
}

/* Empty the queue of pending characters and put all nodes on the free list. */
static void queue_init(void)
{
    g_queue_head = NULL;
    g_queue_tail = NULL;
    g_queue_len = 0;
    for (int i = 0; i < CHAR_QUEUE_NODES; i++) {
        g_queue_nodes[i].c = 0;
        g_queue_nodes[i].next = (i + 1 < CHAR_QUEUE_NODES) ? &g_queue_nodes[i + 1] : NULL;
    }
    g_queue_free = &g_queue_nodes[0];
}

/* Append a character to the queue of pending characters. */
static void queue_push(int c)
{
    if (g_queue_free == NULL) {
        fatal_error(ERR_INTERNAL_NO_FREE_NODE, "", 0);
    }
    char_node_t *node = g_queue_free;
    g_queue_free = node->next;
    node->c = (char)c;
    node->next = NULL;
    g_queue_len++;
    if (g_queue_head == NULL) {
        g_queue_head = node;
    } else {
        g_queue_tail->next = node;
    }
    g_queue_tail = node;
}

/* Oldest pending character, 0 if none */
static int queue_pop(void)
{
    char_node_t *node = g_queue_head;
    if (node == NULL) {
        g_queue_len = 0;
        return 0;
    }
    g_queue_len--;
    g_queue_head = node->next;
    if (g_queue_head == NULL) {
        g_queue_tail = NULL;
    }
    node->next = g_queue_free;
    g_queue_free = node;
    return node->c;
}

/* Write one pending character, counting output lines. */
static void pp_write_pending(int c)
{
    if (c == '\n') {
        fputc('\n', g_pp_out);
        g_pp_out_line++;
    } else {
        pp_put_char(c);
    }
}

/* Define table entry of a name (defined or not), NULL if there is none */
static define_entry_t *find_define(const char *name)
{
    for (int i = 0; i < g_define_count; i++) {
        if (strcmp(g_defines[i].name, name) == 0) {
            return &g_defines[i];
        }
    }
    return NULL;
}

/* Next free define table entry, cleared */
static define_entry_t *alloc_define(void)
{
    if (g_define_count == DEFINE_MAX) {
        fatal_error(ERR_INTERNAL_TABLE_FULL, "", 0);
    }
    define_entry_t *entry = &g_defines[g_define_count++];
    entry->name = "";
    entry->value = "";
    entry->defined = 0;
    entry->name_len = 0;
    entry->next = NULL;
    return entry;
}

static void pool_put(char c)
{
    if (g_define_pool_ptr > &g_define_pool[DEFINE_POOL_SIZE - 1]) {
        fatal_error(ERR_STRING, "", 0);
    }
    *g_define_pool_ptr++ = c;
}

/* Copy a string into the pool; returns its length */
static int pool_add(const char *s)
{
    int len = 0;
    for (; *s != '\0'; s++) {
        pool_put(*s);
        len++;
    }
    *g_define_pool_ptr++ = '\0';
    return len;
}

static int longest_defined_name(void)
{
    int longest = 0;
    for (int i = 0; i < g_define_count; i++) {
        if (g_defines[i].defined && g_defines[i].name_len > longest) {
            longest = g_defines[i].name_len;
        }
    }
    return longest;
}

/* Keep the search list ordered longest name first */
static void insert_define_by_length(define_entry_t *entry)
{
    define_entry_t **link = &g_define_list;
    while (*link != NULL && (*link)->name_len >= entry->name_len) {
        link = &(*link)->next;
    }
    entry->next = *link;
    *link = entry;
}

/* Defined name at the front of the pending characters, if it is
 * not followed by a letter */
static define_entry_t *match_define(void)
{
    for (define_entry_t *entry = g_define_list; entry != NULL; entry = entry->next) {
        if (entry->name_len > g_queue_len || !entry->defined) {
            continue;
        }
        const char *s = entry->name;
        const char_node_t *node = g_queue_head;
        for (; *s != '\0'; s++) {
            if (node == NULL) {
                report_error(ERR_INTERNAL_QUEUE_SHORT, "", 0);
                break;
            }
            if (node->c != *s) {
                break;
            }
            node = node->next;
        }
        if (*s == '\0' && !is_alpha(node ? (unsigned char)node->c : 0)) {
            return entry;
        }
    }
    return NULL;
}

/* Drop the matched name and write its value instead */
static void substitute_define(const define_entry_t *entry)
{
    for (const char *s = entry->name; *s != '\0'; s++) {
        queue_pop();
    }
    fputs(entry->value, g_pp_out);
}

/* Write out everything pending, still substituting names */
static void flush_pending_substituting(void)
{
    while (g_queue_len > 0) {
        define_entry_t *entry = match_define();
        if (entry != NULL) {
            substitute_define(entry);
        } else {
            pp_write_pending(queue_pop());
        }
    }
}

/* Write out everything pending unchanged */
static void flush_pending(void)
{
    while (g_queue_len > 0) {
        pp_write_pending(queue_pop());
    }
}

/* -------------------------------------------------------------------------
 * $ifdef / $ifndef / $else / $endif nesting
 * ------------------------------------------------------------------------- */

/* Back to the outermost $ifdef level, nothing skipped */
static void ifdef_init(void)
{
    g_ifdef_depth = 0;
    memset(&g_ifdef_stack[0], 0, sizeof(g_ifdef_stack[0]));
    g_ifdef_skipping = 0;
}

static ifdef_level_t *ifdef_top(void)
{
    return &g_ifdef_stack[g_ifdef_depth];
}

/* Enter a nesting level; too deep a nesting is reported and ignored. */
static void ifdef_push(int skipping, int kind, int started_skip, int line)
{
    if (g_ifdef_depth == IFDEF_MAX_DEPTH) {
        report_error(ERR_IFDEF_TOO_DEEP, "", line);
        return;
    }
    g_ifdef_depth++;
    ifdef_top()->skipping = (char)skipping;
    ifdef_top()->kind = (char)kind;
    ifdef_top()->started_skip = (char)started_skip;
}

static void ifdef_pop(int line)
{
    if (g_ifdef_depth == 0) {
        report_error(ERR_INTERNAL_IFDEF_UNDERFLOW, "", line);
        return;
    }
    g_ifdef_depth--;
}

/* At the end of a file every $ifdef must have been closed. */
static void ifdef_check_closed(void)
{
    if (g_ifdef_depth != 0) {
        report_error(ERR_MISSING_ENDIF, "", 0);
    }
}

/* Enter a level whose text is skipped, which also starts the skipping. */
static void ifdef_push_skipping(int kind, int line)
{
    ifdef_push(1, kind, 1, line);
    g_ifdef_skipping = 1;
}

/* -------------------------------------------------------------------------
 * Directives
 * ------------------------------------------------------------------------- */

/* Next blank-delimited word of a directive line */
static char *read_word(FILE *in)
{
    size_t n = 0;
    int c;
    do {
        c = phase_read_char(in);
    } while (c == ' ' || c == '\t');
    while (c != ' ' && c != '\t' && c != EOF && c != '\n') {
        if (c < 0x21 || c > 0x7e) {
            phase_trace(&g_rt, "case 4\n");
            illegal_char_error(ERR_ILLEGAL_CHAR, c, g_pp_out_line);
        } else if (n + 1 < sizeof(g_word)) {
            g_word[n++] = (char)c;
        }
        c = phase_read_char(in);
    }
    g_word[n] = '\0';
    if (c == EOF) {
        g_pp_eof = 1;
    }
    if (c == '\n') {
        g_pp_eol = 1;
    }
    return g_word;
}

/* Read a directive's operand and echo it; NULL (after an error) if missing. */
static const char *read_operand(FILE *in, const char *echo_format, int line)
{
    const char *word = read_word(in);
    if (*word == '\0') {
        report_error(ERR_NO_LABEL, "", line);
        return NULL;
    }
    fprintf(g_pp_out, echo_format, word);
    pp_check_write();
    return word;
}

/* $ifdef (when_defined = 1) and $ifndef (when_defined = 0) */
static void directive_ifdef(FILE *in, int line, int when_defined)
{
    const char *name = read_operand(in, " %s ", line);
    if (name == NULL) {
        return;
    }
    if (g_ifdef_skipping) {
        ifdef_push(1, 1, 0, line);
        return;
    }
    const define_entry_t *entry = find_define(name);
    int is_defined = entry != NULL && entry->defined;
    if (is_defined == when_defined) {
        ifdef_push(0, 1, 0, line);
    } else {
        ifdef_push_skipping(1, line);
    }
}

/* $else: switch skipping for the current level unless an outer level skips */
static void directive_else(int line)
{
    const ifdef_level_t *top = ifdef_top();
    if (top->kind != 1) {
        report_error(ERR_ELSE_WITHOUT_IFDEF, "", line);
    } else if (top->started_skip == 0 && g_ifdef_skipping) {
        /* inside an outer skipped region: stay skipped */
        ifdef_pop(line);
        ifdef_push(1, 4, 0, line);
    } else if (top->started_skip == 1 && g_ifdef_skipping) {
        g_ifdef_skipping = 0;
        ifdef_pop(line);
        ifdef_push(0, 4, 0, line);
    } else if (top->started_skip != 0) {
        report_error(ERR_INTERNAL_ELSE_STATE, "", line);
    } else if (top->skipping == 0) {
        g_ifdef_skipping = 1;
        ifdef_pop(line);
        ifdef_push(1, 4, 1, line);
    } else {
        ifdef_pop(line);
        ifdef_push(0, 4, 0, line);
    }
}

/* $endif: leave the level and end the skipping it started */
static void directive_endif(int line)
{
    const ifdef_level_t *top = ifdef_top();
    if (top->kind != 1 && top->kind != 4) {
        fatal_error(ERR_ENDIF_WITHOUT_IFDEF, "", line);
    }
    if (top->started_skip == 1) {
        g_ifdef_skipping = 0;
    }
    ifdef_pop(line);
}

/* $define name value.  The rest of the line is the value; a
 * trailing comment and the blanks before it are not part of it. */
static void directive_define(FILE *in, int line)
{
    const char *name = read_word(in);
    fprintf(g_pp_out, " %s ", name);
    pp_check_write();
    if (*name == '\0') {
        report_error(ERR_NO_LABEL, "", line);
        return;
    }
    define_entry_t *entry = find_define(name);
    if (entry == NULL) {
        entry = alloc_define();
        entry->name = g_define_pool_ptr;
        entry->defined = 1;
        entry->name_len = pool_add(name);
        g_max_define_len = longest_defined_name();
        insert_define_by_length(entry);
    } else if (entry->defined == 1) {
        report_error(ERR_ALREADY_DEFINED, name, line);
        return;
    }
    entry->defined = 1;
    char *value = g_define_pool_ptr;
    entry->value = value;

    /* read_word consumed the delimiter, which may already be the newline. */
    if (g_pp_eol || g_pp_eof) {
        *g_define_pool_ptr++ = '\0';
        fputc('\n', g_pp_out);
        return;
    }

    int c;
    do {
        c = phase_read_char(in);
    } while (c == ' ' || c == '\t');
    ungetc(c, in);

    int comment_found = 0;
    for (;;) {
        c = phase_read_char(in);
        if (c == '\n' || c == EOF) {
            break;
        }
        if (!is_space(c) && !is_print(c)) {
            phase_trace(&g_rt, "case 3");
            illegal_char_error(ERR_ILLEGAL_CHAR, c, line);
            continue;
        }
        pp_put_char(c);
        if (comment_found) {
            continue;
        }
        if (g_define_pool_ptr > value + 1 && c == '*' && g_define_pool_ptr[-1] == '/') {
            /* drop the '/' and the blanks before the comment */
            *--g_define_pool_ptr = '\0';
            while (g_define_pool_ptr > value &&
                   (g_define_pool_ptr[-1] == ' ' || g_define_pool_ptr[-1] == '\t')) {
                *--g_define_pool_ptr = '\0';
            }
            if (g_define_pool_ptr == value) {
                g_define_pool_ptr++;
            }
            comment_found = 1;
        } else {
            pool_put((char)c);
        }
    }
    *g_define_pool_ptr++ = '\0';

    if (c == '\n') {
        fputc('\n', g_pp_out);
        g_pp_eol = 1;
    }
    if (c == EOF) {
        g_pp_eof = 1;
    }
}

/* $undef name: the name is no longer defined */
static void directive_undef(FILE *in, int line)
{
    const char *name = read_operand(in, " %s ", line);
    if (name != NULL) {
        define_entry_t *entry = find_define(name);
        if (entry != NULL) {
            entry->defined = 0;
        }
    }
}

/* $include file: run the define pass over the file in place */
static void directive_include(FILE *in, int line)
{
    const char *name = read_word(in);
    int saved_eof = g_pp_eof;
    int saved_eol = g_pp_eol;
    if (*name == '\0') {
        report_error(ERR_NO_LABEL, "", line);
        return;
    }
    fprintf(g_pp_out, " %s\n", name);
    pp_check_write();
    g_pp_out_line++;

    FILE *included = open_file(name, "rt");
    if (included == NULL) {
        report_error(ERR_OPEN, name, line);
        return;
    }
    pool_add(name);
    preprocess_stream(included);
    g_pp_eof = saved_eof;
    g_pp_eol = saved_eol;
}

/* A line starting with '$' */
static void process_directive(FILE *in, int line)
{
    int is_define = 0;
    int c = 0;

    const char *word = read_word(in);
    fputs(word, g_pp_out);
    pp_check_write();

    size_t n = 0;
    for (; word[n] != '\0'; n++) {
        g_command[n] = (char)to_lower((unsigned char)word[n]);
    }
    g_command[n] = '\0';

    if (g_command[0] == '\0') {
        report_error(ERR_BAD_COMMAND, g_command, line);
    } else if (strcmp(g_command, "ifdef") == 0) {
        directive_ifdef(in, line, 1);
    } else if (strcmp(g_command, "ifndef") == 0) {
        directive_ifdef(in, line, 0);
    } else if (strcmp(g_command, "else") == 0) {
        directive_else(line);
    } else if (strcmp(g_command, "endif") == 0) {
        directive_endif(line);
    } else if (ifdef_top()->skipping == 0) {
        if (strcmp(g_command, "define") == 0) {
            directive_define(in, line);
            is_define = 1;
        } else if (strcmp(g_command, "undef") == 0) {
            directive_undef(in, line);
        } else if (strcmp(g_command, "include") == 0) {
            directive_include(in, line);
        } else {
            report_error(ERR_BAD_COMMAND, g_command, line);
        }
    }

    /* copy the rest of the directive line */
    if (!g_pp_eof && !g_pp_eol) {
        for (;;) {
            c = phase_read_char(in);
            if (c == '\n' || c == EOF) {
                break;
            }
            if (!is_space(c) && !is_print(c)) {
                illegal_char_error(ERR_ILLEGAL_CHAR, c, line);
            } else {
                pp_put_char(c);
            }
        }
    }
    g_pp_eol = 1;
    if (!is_define) {
        fputc('\n', g_pp_out);
    }
    g_pp_out_line++;
    if (c == EOF) {
        g_pp_eof = 1;
    }
}

/* Run the define pass over one file (closes `in`) */
static void preprocess_stream(FILE *in)
{
    int line = 1;
    int column = 1;
    int c;

    while ((c = phase_read_char(in)) != EOF) {
        if (!is_space(c) && !is_print(c)) {
            illegal_char_error(ERR_ILLEGAL_CHAR, c, g_pp_out_line);
            continue;
        }
        g_pp_eof = 0;
        g_pp_eol = 0;

        if (c == '$' && column == 1) {
            flush_pending();
            fputc('$', g_pp_out);
            process_directive(in, line);
            if (g_pp_eof) {
                /* the file ended on a directive line */
                g_pp_eof = 0;
                g_pp_eol = 0;
                flush_pending();
                ifdef_check_closed();
                fputc('\n', g_pp_out);
                g_pp_out_line++;
                phase_close(&g_rt, in);
                pp_check_write();
                return;
            }
            line++;
            column = 1;
            g_pp_eof = 0;
            g_pp_eol = 0;
        } else if (ifdef_top()->skipping != 1) {
            queue_push(c);
            column++;
            if (g_queue_len > g_max_define_len) {
                int out = queue_pop();
                if (out == 0) {
                    fatal_error(ERR_INTERNAL_QUEUE_EMPTY, "", 0);
                }
                pp_write_pending(out);
            }
            if (g_queue_len >= g_max_define_len) {
                define_entry_t *entry = match_define();
                if (entry != NULL) {
                    substitute_define(entry);
                }
            }
            if (c == '\n') {
                line++;
                column = 1;
            }
        }
    }

    flush_pending_substituting();
    fputc('\n', g_pp_out);
    g_pp_out_line++;
    pp_check_write();
    ifdef_check_closed();
    phase_close(&g_rt, in);
}

/* =========================================================================
 * Phase driver
 * ========================================================================= */

/* Option letters.  'e' keeps the .mx file; 'e' and 'v' select
 * console output; 'e', 'v', 'k' and 'z' also swallow the following letter. */
static void parse_flags(const char *flags)
{
    for (const char *p = flags; *p != '\0'; p++) {
        if (*p == 'e') {
            g_keep_mx_file = 1;
        }
        if (*p == 'e' || *p == 'v') {
            g_rt.console = 1;
        }
        if ((*p == 'e' || *p == 'v' || *p == 'k' || *p == 'z') && p[1] != '\0') {
            p++;
        }
    }
}

/* Set every phase variable to its initial value, so that the phase can run
 * more than once in one process. */
static void reset_state(void)
{
    memset(g_args, 0, sizeof(g_args));
    phase_begin(&g_rt);
    g_error_count = 0;
    g_had_error = 0;
    g_keep_mx_file = 0;
    g_mx_name = NULL;
    g_temp_name = NULL;

    g_src = NULL;
    g_dst = NULL;
    g_tok = 0;
    g_tok_text[0] = '\0';
    g_prev_text[0] = '\0';
    g_ws_count = 0;
    g_line = 1;
    g_no_line_count = 0;
    g_skip_ws = 0;
    g_in_comment = 0;
    g_in_define_line = 0;
    g_keyword_limit = 0;
    g_capturing = 0;
    g_capture_buf[0] = '\0';
    g_read_capture = 0;
    g_capture_rd = NULL;
    g_work_buf[0] = '\0';
    g_read_expansion = 0;
    g_expansion_rd = NULL;
    g_in_macro_expansion = 0;
    g_suppress_output = 0;
    g_in_table = 0;
    g_brace_in_define = 0;
    g_radix = 0;
    g_pin_counter = 1;
    g_macros = NULL;
    g_saved_tok = 0;
    g_saved_prev_text[0] = '\0';
    g_saved_tok_text[0] = '\0';
    g_repeat_depth = -1;
    g_repeat_var_count = 0;
    memset(g_repeat_vars, 0, sizeof(g_repeat_vars));
    g_repeat_values_wr = NULL;
    g_repeat_values_end = NULL;
    g_expr_sp = 0;
    g_fuse_count = 0;

    g_pp_out = NULL;
    g_pp_out_line = 0;
    g_pp_eof = 0;
    g_pp_eol = 0;
    g_word[0] = '\0';
    g_command[0] = '\0';
    g_define_count = 0;
    g_define_list = NULL;
    g_max_define_len = 0;
    g_define_pool_ptr = g_define_pool;
    queue_init();
    ifdef_init();
}

/* Directory of `path` without the trailing separator ("" if it has none). */
static void directory_of(const char *path, char *dir, size_t dir_size, const char *if_none)
{
    char norm[PATH_MAX];
    lincupl_normalize_slashes(path, norm, sizeof(norm));
    char *slash = strrchr(norm, '/');
    if (slash == NULL) {
        snprintf(dir, dir_size, "%s", if_none);
    } else {
        *slash = '\0';
        snprintf(dir, dir_size, "%s", norm[0] ? norm : "/");
    }
}

/* Run both passes: <design>.pld -> <design>.mx -> CA<pid>.  The .mx file is
 * deleted unless 'e' was given; any error abandons the phase. */
static void run_phase(void)
{
    const char *source = g_args[5];
    parse_flags(g_args[6]);

    char *pld_name = phase_file_name(&g_rt, phase_dir_prefix(&g_rt, source), source, ".pld");
    FILE *pld = open_file(pld_name, "rt");
    if (pld == NULL) {
        fatal_error(ERR_OPEN, pld_name, 0);
    }
    g_mx_name = phase_file_name(&g_rt, phase_dir_prefix(&g_rt, source), source, ".mx");
    FILE *mx = open_file(g_mx_name, "wt");
    if (mx == NULL) {
        fatal_error(ERR_OPEN, g_mx_name, 0);
    }
    run_macro_pass(pld, mx);
    phase_close(&g_rt, mx);
    phase_close(&g_rt, pld);
    g_src = NULL;
    g_dst = NULL;

    mx = open_file(g_mx_name, "rt");
    if (mx == NULL) {
        fatal_error(ERR_OPEN, g_mx_name, 0);
    }

    /* Temp file name CA + last six characters of the process id */
    g_temp_name = phase_strdup(&g_rt, "CAXXXXXX", "");
    phase_temp_name(g_temp_name, g_args[3]);

    g_pp_out = open_file(g_temp_name, "wt");
    if (g_pp_out == NULL) {
        fatal_error(ERR_OPEN, g_temp_name, 0);
    }
    queue_init();
    ifdef_init();
    preprocess_stream(mx);
    pp_check_write();
    phase_close(&g_rt, g_pp_out);

    if (!g_keep_mx_file) {
        phase_remove(g_mx_name, 1);
    }
    if (g_had_error) {
        remove_temp_file();
        longjmp(g_abort_jmp, 1);
    }
}

/* Phase entry point.  argv[2] is the device library (its directory is
 * searched for include files), argv[3] the process id, argv[5] the design
 * file and argv[6] the option letters.  Returns 0 on success. */
int preprocess_run(int argc, char **argv)
{
    volatile int result = 1;

    reset_state();
    g_pld_dir[0] = '\0';
    g_lib_dir[0] = '\0';
    for (int i = 0; i < argc && i < 10; i++) {
        g_args[i] = (argv != NULL && argv[i] != NULL) ? argv[i] : "";
    }
    if (argc > 5 && strcmp(g_args[5], "&") != 0) {
        directory_of(g_args[5], g_pld_dir, sizeof(g_pld_dir), "");
    }
    if (argc > 2 && strcmp(g_args[2], "&") != 0) {
        directory_of(g_args[2], g_lib_dir, sizeof(g_lib_dir), ".");
    }

    if (argc >= 7 && setjmp(g_abort_jmp) == 0) {
        run_phase();
        result = 0;
    }

    phase_end(&g_rt);
    return result;
}
