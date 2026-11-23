/*
 * sim_parse - the simulation input (.si) front end: scanner,
 * table-driven parser and the actions of every statement and $ directive.
 *
 * Part of the CUPL simulator (see sim.c).
 *
 * The grammar is a table of entries (k_grammar).  A rule is a run of
 * entries starting with an OP_RULE entry; `next` of an entry is the offset
 * (from the rule start) of the entries to try after it matched.  Tokens are
 * list nodes: kind = token code, u.text = spelling.
 */
#include "sim.h"

/* -------------------------------------------------------------------------
 * Tokens
 * ------------------------------------------------------------------------- */
enum {
    T_NAME = 0x03,
    T_PARTNO = 0x04,
    T_REV = 0x05,
    T_DATE = 0x06,
    T_DESIGNER = 0x07,
    T_COMPANY = 0x08,
    T_ASSY = 0x09,
    T_ORDER = 0x0a,
    T_BASE = 0x0b,
    T_VECTORS = 0x0c,
    T_IDENT = 0x0d,
    T_PRELOAD = 0x0e,
    T_MACRO = 0x0f,
    T_MEND = 0x10,
    T_HEX = 0x11,
    T_OCTAL = 0x12,
    T_DECIMAL = 0x13,
    T_COLON = 0x14,
    T_LIST = 0x15,
    T_SEMI = 0x16,
    T_EQUAL = 0x17,
    T_TEXT = 0x18,
    T_QUOTE = 0x19,
    T_DQUOTE = 0x1a,
    T_COMMA = 0x1b,
    T_DOT = 0x1c,
    T_WHILE = 0x1d,
    T_FOR = 0x1e,
    T_GREATER = 0x1f,
    T_LESS = 0x20,
    T_HASH = 0x21,
    T_REPEAT = 0x22,
    T_EOL = 0x23,
    T_EOF = 0x24,
    T_LPAREN = 0x25,
    T_RPAREN = 0x26,
    T_NUMBER = 0x32,
    T_IDENT_NUM = 0x33,
    T_SLASH_ERR = 0x34,
    T_BAD = 0x35,
    T_PUNCT = 0x36,
    T_CHAR = 0x37,
    T_NOT = 0x38,
    T_BLANK = 0x39,
    T_MSG = 0x3a,
    T_EXIT = 0x3b,
    T_BINARY = 0x3c,
    T_FIELD = 0x3d,
    T_IF = 0x3e,
    T_LBRACKET = 0x40,
    T_RBRACKET = 0x41,
    T_ORDER_TEXT = 0x43,
    T_ORDER_SPACE = 0x44,
    T_PERCENT = 0x46,
    T_DOLLAR = 0x47,
    T_STUCKH = 0x48,
    T_STUCKL = 0x49,
    T_LOCATION = 0x4d,
    T_TRACE = 0x4e,
    T_DEVICE = 0x4f,
    T_FORMAT = 0x50,
    T_SIMON = 0x51,
    T_SIMOFF = 0x52,
    T_ELSE = 0x54,
    T_ENDIF = 0x55,
    T_DO = 0x56,
    T_UNTIL = 0x57,
    T_VAR = 0x58,
    T_SET = 0x59,
    T_ENDF = 0x5a,
    T_PLUS = 0x5b,
    T_MINUS = 0x5c,
    T_STAR = 0x5d,
    T_SLASH = 0x5e,
    T_ENDW = 0x5f,
    T_COMP = 0x60,
    T_OUT = 0x61,
    T_AND = 0x62,
    T_CALL = 0x63
};

/* Parser operations (grammar_t.op) */
enum {
    OP_TRY = 0,         /* match the token, else try the next entry */
    OP_CALL = 1,        /* try the rule at `base`; on failure try the next entry */
    OP_PUSHBACK = 2,    /* give the token back and leave the rule successfully */
    OP_FAIL = 3,        /* leave the rule: no match */
    OP_ERROR = 4,       /* syntax error, leave the rule: no match */
    OP_ACCEPT = 5,      /* (unused) */
    OP_FIRST = 6,       /* first token of a rule: no match leaves the rule quietly */
    OP_MUST = 7,        /* no match: syntax error, skip the statement, leave the rule */
    OP_RESYNC = 8,      /* no match: syntax error, go on at the rule's first entry */
    OP_LAST = 9,        /* like OP_MUST; a match ends the rule */
    OP_RULE = 10,       /* rule header, never executed */
    OP_MUST_CALL = 11,  /* OP_CALL, but failure is a syntax error */
    OP_EXPECT = 12      /* no match: "missing symbol", go on with the next entry */
};

typedef void (*action_fn)(list_node_t *tok);

typedef struct {
    uint8_t token;      /* token to match */
    int16_t next;       /* entries to try after a match, from the rule start */
    int16_t base;       /* rule start; for OP_CALL the rule to call */
    int16_t op;         /* OP_* */
    action_fn action;   /* run with the matched token */
} grammar_t;

/* Keywords (compared in lower case; one-letter words never match) */
static const struct {
    const char *name;
    int token;
} k_keywords[] = {
    { "msg", T_MSG }, { "for", T_FOR }, { "repeat", T_REPEAT }, { "fld", T_FIELD },
    { "field", T_FIELD }, { "while", T_WHILE }, { "preload", T_PRELOAD }, { "name", T_NAME },
    { "partno", T_PARTNO }, { "rev", T_REV }, { "revision", T_REV }, { "date", T_DATE },
    { "designer", T_DESIGNER }, { "company", T_COMPANY }, { "assy", T_ASSY },
    { "assembly", T_ASSY }, { "device", T_DEVICE }, { "format", T_FORMAT }, { "order", T_ORDER },
    { "location", T_LOCATION }, { "loc", T_LOCATION }, { "base", T_BASE }, { "vectors", T_VECTORS },
    { "macro", T_MACRO }, { "mend", T_MEND }, { "hex", T_HEX }, { "octal", T_OCTAL },
    { "decimal", T_DECIMAL }, { "binary", T_BINARY }, { "exit", T_EXIT }, { "stuckh", T_STUCKH },
    { "stuckl", T_STUCKL }, { "trace", T_TRACE }, { "if", T_IF }, { "else", T_ELSE },
    { "endif", T_ENDIF }, { "do", T_DO }, { "until", T_UNTIL }, { "simoff", T_SIMOFF },
    { "simon", T_SIMON }, { "var", T_VAR }, { "set", T_SET }, { "endf", T_ENDF },
    { "endw", T_ENDW }, { "comp", T_COMP }, { "out", T_OUT }, { "call", T_CALL },
    { NULL, 0 }
};

typedef struct {
    const char *name;
    int token;
} char_token_t;

/* Test value characters */
static const char_token_t k_value_chars[] = {
    { "H", T_VAL_H }, { "L", T_VAL_L }, { "Z", T_VAL_Z }, { "X", T_VAL_X }, { "P", T_VAL_P },
    { "C", T_VAL_C }, { "K", T_VAL_K }, { "N", T_VAL_N }, { "0", T_VAL_0 }, { "1", T_VAL_1 },
    { "*", T_VAL_STAR }, { "R", T_VAL_R }, { NULL, 0 }
};

/* Punctuation */
static const char_token_t k_punctuation[] = {
    { ";", T_SEMI }, { "(", T_LPAREN }, { ")", T_RPAREN }, { ".", T_DOT }, { "'", T_QUOTE },
    { "\"", T_DQUOTE }, { ":", T_COLON }, { ",", T_COMMA }, { "!", T_NOT }, { "[", T_LBRACKET },
    { "]", T_RBRACKET }, { "=", T_EQUAL }, { "%", T_PERCENT }, { "<", T_LESS }, { ">", T_GREATER },
    { "#", T_HASH }, { "$", T_DOLLAR }, { "+", T_PLUS }, { "-", T_MINUS }, { "*", T_STAR },
    { "/", T_SLASH }, { "&", T_AND }, { NULL, 0 }
};

/* -------------------------------------------------------------------------
 * Limits
 * ------------------------------------------------------------------------- */
#define TOKEN_TEXT_SIZE 0x401
#define LINE_TEXT_MAX 0x400
#define MODE_STACK_SIZE 20
#define BLOCK_MAX 20                /* nesting depth of $IF / $DO / $WHILE / $FOR */
#define HEADER_LINES 15             /* lines of the heading above the vectors */
#define COND_VALUES_MAX 100         /* test values in one condition */
#define VAR_TEXT_SIZE 0x21
#define QUOTE_TEXT_SIZE 10

/* -------------------------------------------------------------------------
 * Scanner state
 * ------------------------------------------------------------------------- */
typedef int (*scan_fn)(char *text);

/* Scanner mode: the scanner to use and how to skip after an error */
typedef struct {
    char active;        /* 0: the parser runs no actions */
    scan_fn scan;
    scan_fn skip;       /* skips the rest of a statement after a syntax error */
} scan_mode_t;

static int scan_normal(char *text);
static int scan_normal_eol(char *text);
static int scan_value_char(char *text);
static int scan_to_semicolon(char *text);
static int scan_to_quote(char *text);
static int scan_to_eol(char *text);

static const scan_mode_t k_mode_none = { 0, NULL, NULL };
static const scan_mode_t k_mode_normal = { 1, scan_normal, scan_to_semicolon };
static const scan_mode_t k_mode_values = { 1, scan_value_char, scan_to_eol };
static const scan_mode_t k_mode_to_semicolon = { 1, scan_to_semicolon, scan_to_eol };
static const scan_mode_t k_mode_to_quote = { 1, scan_to_quote, scan_to_eol };
static const scan_mode_t k_mode_directive = { 1, scan_normal_eol, scan_to_eol };

static const scan_mode_t *g_modes[MODE_STACK_SIZE]; /* stack of scanner modes */
static const scan_mode_t **g_mode_top;          /* current mode */
static char g_tok_text[TOKEN_TEXT_SIZE + 1];    /* spelling of the token being scanned */
static char g_want_eol;                         /* scan_normal reports the next line end */
static list_t g_pushback;                       /* tokens to deliver before scanning */
static char g_stop_parsing;                     /* set by $EXIT */

/* Stand-in tokens (numbered 0 and 1) handed to an action that runs during
 * error recovery, when there is no real token. */
static list_node_t g_phantom[2];
static char g_phantom_text[2][VAR_TEXT_SIZE];

/* -------------------------------------------------------------------------
 * Statement state
 * ------------------------------------------------------------------------- */

/* A control structure being executed ($IF, $DO, $WHILE or $FOR). */
enum { BLOCK_IF = 0, BLOCK_DO = 1, BLOCK_WHILE = 2, BLOCK_FOR = 3 };
typedef struct block {
    uint8_t type;           /* BLOCK_* */
    int active;             /* statements inside are executed */
    struct block *parent;
    /* IF / DO / WHILE */
    int else_seen;
    int nvalues;            /* signals named in the condition */
    int16_t cond;           /* result of the condition */
    uint32_t compare;       /* constant the signals are compared with */
    long pos;               /* file position to loop back to */
    /* FOR */
    const char *var;        /* loop variable */
    uint16_t first;
    uint16_t last;
    uint16_t cur;
    int step;               /* 1 or -1 */
    long for_pos;           /* file position of the loop body */
} block_t;

/* A $VAR, a FIELD or an ORDER name usable in $SET / $COMP / conditions. */
typedef struct var {
    const char *name;
    list_t *members;
    int state;              /* 2 = value is a number, 1 = has other levels */
    char text[VAR_TEXT_SIZE];
    uint32_t value;
    struct var *next;
} var_t;

typedef struct macro {
    const char *name;
    long body_pos;          /* file position of the body */
    list_t *params;
    struct macro *next;
} macro_t;

/* A running $CALL */
typedef struct call {
    macro_t *macro;
    list_t args;
    long return_pos;        /* file position to continue at after $MEND */
    struct call *prev;      /* the enclosing call */
} call_t;

/* Node of a $COMP expression */
typedef struct expr {
    char op;                /* operator token, '2' (T_NUMBER) for a value */
    uint32_t value;
    struct expr *left;
    struct expr *right;
} expr_t;

typedef struct expr_save {
    expr_t *expr;
    struct expr_save *next;
} expr_save_t;

static block_t g_blocks[BLOCK_MAX];             /* open blocks, innermost last */
static block_t *g_block;                        /* innermost open block, NULL if none */
static block_t g_null_block;                    /* all zeroes; used by $ELSE without a block */
static int g_block_depth;
static uint32_t g_relation;                     /* 1 '=', 2 '#', 4 '<', 8 '>' */
static uint32_t g_const_value;                  /* constant of the condition */
static int g_have_const;                        /* a quoted constant follows */
static int g_const_is_levels;                   /* it was quoted with " (H/L) */
static block_t *g_cond_loop;                    /* $FOR variable named in a condition */
static char g_cond_values[COND_VALUES_MAX];     /* test values of the condition */

static var_t *g_vars;
static var_t *g_vars_tail;
static var_t *g_cur_var;                        /* variable being defined or set */
static var_t *g_comp_var;                       /* variable of the $COMP being read */
static expr_t *g_expr;                          /* expression being read */
static int g_expr_started;                      /* the $COMP has an operand or operator */
static expr_save_t *g_expr_stack;               /* expressions outside the open '(' */
static int g_paren_depth;
static int g_after_paren;                       /* g_expr is a closed parenthesis */

static macro_t *g_macros;
static macro_t *g_macros_tail;
static call_t *g_call;                          /* innermost running $CALL */
static int g_arg_pending;                       /* next text starts a new argument */
static list_node_t *g_strings_order_mark;       /* last interned string before ORDER */
static int g_strings_order_count;
static list_node_t *g_strings_mark;             /* last interned string before $COMP / $CALL */

static char *g_header_lines[HEADER_LINES];      /* heading above the vectors */
static int g_first_order;                       /* no ORDER statement read yet */
static uint16_t g_header_seen;                  /* header items found */
static int g_header_kind;                       /* token of the header item being read */
static char g_repeat_kind;                      /* T_REPEAT while a $REPEAT is set */
static char g_repeat_active;
static int g_repeat_step;
static int g_repeat_done;                       /* repetitions so far */
static int g_repeat_count;                      /* repetitions requested */

static int g_base;                              /* T_HEX, T_OCTAL, T_DECIMAL or T_BINARY */
static char g_quote_kind;                       /* T_QUOTE or T_DQUOTE in a vector, else 0 */
static char g_quote_text[QUOTE_TEXT_SIZE];      /* digits between the quotes */
static char g_in_range;                         /* ".." seen in a list */
static char g_positive;                         /* cleared by '!' in front of a name */
static char g_field_positive;                   /* cleared by '!' in front of a FIELD list */
static const char *g_range_stem;                /* name stem of the range start ("a" of "a0..7") */
static int g_range_first;                       /* number of the range start */
static field_t *g_cur_field;                    /* FIELD being defined */

#define GRAMMAR_ENTRIES 322
static const grammar_t k_grammar[GRAMMAR_ENTRIES];
static list_node_t *next_token(void);

/* -------------------------------------------------------------------------
 * Scanner
 * ------------------------------------------------------------------------- */
int is_phantom_token(const list_node_t *tok)
{
    return tok == &g_phantom[0] || tok == &g_phantom[1];
}

static const char *tok_text(const list_node_t *tok)
{
    return tok->u.text != NULL ? tok->u.text : "";
}

static void line_put(int c)
{
    int col = g_source->col++;
    if (col >= 0 && col < LINE_TEXT_MAX) {
        g_line_text[col] = (char)c;
    }
}

static void bad_char(int c)
{
    char text[16];
    snprintf(text, sizeof(text), "'h'%x", (unsigned)c);
    report_error(ERR_BAD_CHAR, text);
}

static int is_valid_char(int c)
{
    return is_space(c) || is_print(c);
}

/* Read up to the terminator.  Line ends inside are kept in the
 * text. */
static int scan_to(char *text, int terminator)
{
    char *p = text;
    if (g_source == NULL) {
        fatal_error(0x3f9, "");
    }
    if (g_source->at_eof) {
        return T_EOF;
    }
    *p = '\0';
    for (;;) {
        int c = read_char(g_source->fp);
        if (c == EOF || c == '\n') {
            flush_source_line(g_line_text);
            if (c == EOF) {
                g_source->at_eof = 1;
                return T_EOF;
            }
        }
        if (!is_valid_char(c)) {
            bad_char(c);
        }
        if (c != '\n') {
            line_put(c);
        }
        if (c == terminator) {
            return T_SEMI;
        }
        if (p - text > 0x400) {
            fatal_error(ERR_STATEMENT_TOO_LONG, "");
        }
        *p++ = (char)c;
        *p = '\0';
    }
}

/* Scanners of the modes that read up to a terminator. */
static int scan_to_quote(char *text)
{
    return scan_to(text, '"');
}

static int scan_to_semicolon(char *text)
{
    return scan_to(text, ';');
}

static int scan_to_eol(char *text)
{
    int token = scan_to(text, '\n');
    return token == T_SEMI ? T_EOL : token;
}

/* After a '/': skip a comment.  Returns 0 if no '*' follows. */
static int skip_comment(void)
{
    int star = 0;
    int c = read_char(g_source->fp);
    if (c != '*') {
        ungetc(c, g_source->fp);
        return 0;
    }
    line_put('*');
    for (;;) {
        if (g_source->col > 0x400) {
            fatal_error(ERR_STATEMENT_TOO_LONG, "");
        }
        c = read_char(g_source->fp);
        if (c == EOF || c == '\n') {
            flush_source_line(g_line_text);
            if (c == EOF) {
                g_source->at_eof = 1;
                return 1;
            }
        } else if (!is_valid_char(c)) {
            bad_char(c);
            continue;
        } else {
            line_put(c);
        }
        if (c == '*') {
            star = 1;
        } else if (c == '/' && star) {
            return 1;
        } else {
            star = 0;
        }
    }
}

/* One character of a test vector. */
static int scan_value_char(char *text)
{
    for (;;) {
        int blank = 0;
        int c;
        if (g_source == NULL) {
            fatal_error(0x3f1, "");
        }
        if (g_source->at_eof) {
            return T_EOF;
        }
        for (;;) {
            c = read_char(g_source->fp);
            if (c == EOF || c == '\n') {
                flush_source_line(g_line_text);
                if (c == EOF) {
                    g_source->at_eof = 1;
                    return T_EOF;
                }
                return T_EOL;
            }
            if (is_space(c)) {
                line_put(c);
                blank = 1;
            } else if (is_valid_char(c)) {
                break;
            } else {
                bad_char(c);
            }
        }
        if (blank) {
            ungetc(c, g_source->fp);
            text[0] = ' ';
            text[1] = '\0';
            return T_BLANK;
        }
        line_put(c);
        if (c != '/') {
            text[0] = (char)c;
            text[1] = '\0';
            return T_CHAR;
        }
        if (!skip_comment()) {
            return T_SLASH_ERR;
        }
    }
}

/* Names, numbers and punctuation. */
static int scan_normal(char *text)
{
    char *p = text;
    int first_kind, last_kind;
    int c;
    for (;;) {
        if (g_source == NULL) {
            fatal_error(0x3f2, "");
        }
        if (g_source->at_eof) {
            return T_EOF;
        }
        for (;;) {
            c = read_char(g_source->fp);
            if (c == '\n' || c == EOF) {
                flush_source_line(g_line_text);
                if (c == EOF) {
                    g_source->at_eof = 1;
                    return T_EOF;
                }
                if (g_want_eol) {
                    g_want_eol = 0;
                    return T_EOL;
                }
            } else if (is_space(c)) {
                line_put(c);
            } else if (is_valid_char(c)) {
                break;
            } else {
                bad_char(c);
            }
        }
        line_put(c);
        if (c != '/') {
            *p++ = (char)c;
            *p = '\0';
        }
        if (is_alpha(c) || c == '_') {
            first_kind = last_kind = T_IDENT;
            break;
        }
        if (is_digit(c)) {
            first_kind = last_kind = T_NUMBER;
            break;
        }
        if (c != '/') {
            return T_PUNCT;
        }
        if (!skip_comment()) {
            p[0] = '/';
            p[1] = '\0';
            return T_PUNCT;
        }
    }
    for (;;) {
        c = read_char(g_source->fp);
        if (c != EOF) {
            if (!is_valid_char(c)) {
                bad_char(c);
            }
            if (is_alpha(c) || c == '_') {
                last_kind = T_IDENT;
            } else if (is_digit(c)) {
                last_kind = T_NUMBER;
            } else {
                ungetc(c, g_source->fp);
                break;
            }
            line_put(c);
            if (p - text < TOKEN_TEXT_SIZE - 1) {
                *p++ = (char)c;
            }
            if (first_kind == T_NUMBER && last_kind == T_IDENT) {
                first_kind = T_IDENT;
            }
            continue;
        }
        g_source->at_eof = 1;
        break;
    }
    *p = '\0';
    if (first_kind == T_IDENT) {
        return last_kind == T_NUMBER ? T_IDENT_NUM : T_IDENT;
    }
    return last_kind == T_NUMBER ? T_NUMBER : T_IDENT;
}

static int scan_normal_eol(char *text)
{
    g_want_eol = 1;
    return scan_normal(text);
}

/* Push a scanner mode (k_mode_none until the caller sets it). */
static void mode_push(void)
{
    if (g_mode_top == &g_modes[MODE_STACK_SIZE - 1]) {
        fatal_error(0x3f3, "parser");
    } else {
        g_mode_top++;
    }
    *g_mode_top = &k_mode_none;
}

static void mode_pop(void)
{
    if (g_mode_top == &g_modes[0]) {
        fatal_error(0x3f4, "parser");
    }
    g_mode_top--;
}

/* Open a .si file as the current source. */
static void open_source(const char *name)
{
    if (g_source == NULL) {
        g_source = &g_source_stack[0];
    } else {
        if (g_source == &g_source_stack[SOURCE_STACK_SIZE - 1]) {
            fatal_error(0x3e9, "");
        }
        g_source++;
    }
    /* binary: the loops return to positions taken with ftell, which are
     * exact only without line end translation; CR is skipped as a blank */
    g_source->fp = open_file(name, "rb");
    if (g_source->fp == NULL) {
        fatal_error(ERR_OPEN, name);
    }
    g_source->name = dup_string(name, "initialization");
    g_source->line = 1;
    g_line_ptr = &g_source->line;
    g_source->col = 0;
    g_source->at_eof = 0;
}

/* -------------------------------------------------------------------------
 * Parser
 * ------------------------------------------------------------------------- */

/* Table entry of a character (letters in upper case), NULL if none. */
static const char_token_t *find_char_token(const char_token_t *table, int c)
{
    if (is_lower(c)) {
        c = to_upper(c);
    }
    for (; table->name != NULL; table++) {
        if (table->name[0] == (char)c) {
            return table;
        }
    }
    return NULL;
}

/* Spelling of a token for "missing symbol". */
static const char *token_name(int token)
{
    for (int i = 0; k_keywords[i].name != NULL; i++) {
        if (k_keywords[i].token == token) {
            return k_keywords[i].name;
        }
    }
    for (int i = 0; k_punctuation[i].name != NULL; i++) {
        if (k_punctuation[i].token == token) {
            return k_punctuation[i].name;
        }
    }
    for (int i = 0; k_value_chars[i].name != NULL; i++) {
        if (k_value_chars[i].token == token) {
            return k_value_chars[i].name;
        }
    }
    return "something";
}

/* Next token, from the push-back list or the scanner. */
static list_node_t *next_token(void)
{
    list_node_t *tok;
    const char *text;
    const char_token_t *ct;
    int pushed_back = 0;
    int kind;
    if (!list_is_empty(&g_pushback)) {
        tok = list_pop_front(&g_pushback);
        kind = (int8_t)tok->kind;
        text = tok_text(tok);
        pushed_back = 1;
    } else {
        if ((*g_mode_top)->scan == NULL) {
            fatal_error(0x3f0, "parser");
        }
        kind = (*g_mode_top)->scan(g_tok_text);
        text = g_tok_text;
        tok = node_new();
    }
    switch (kind) {
    case T_BAD:
        report_error(ERR_SYNTAX, text);
        return tok;
    case T_IDENT:
        if (strlen(text) >= 2) {
            copy_lowercase(g_name_buf, text);
            for (int i = 0; k_keywords[i].name != NULL; i++) {
                if (strcmp(g_name_buf, k_keywords[i].name) == 0) {
                    tok->u.text = (char *)k_keywords[i].name;
                    tok->kind = (uint8_t)k_keywords[i].token;
                    return tok;
                }
            }
        }
        break;
    case T_SLASH_ERR:
        fatal_error(ERR_SYNTAX, text);
    case T_CHAR:
        ct = find_char_token(k_value_chars, text[0]);
        if (ct != NULL) {
            tok->u.text = (char *)ct->name;
            tok->kind = (uint8_t)ct->token;
            return tok;
        }
        /* fall through */
    case T_PUNCT:
        ct = find_char_token(k_punctuation, text[0]);
        if (ct != NULL) {
            tok->u.text = (char *)ct->name;
            tok->kind = (uint8_t)ct->token;
            return tok;
        }
        break;
    default:
        break;
    }
    tok->kind = (uint8_t)kind;
    if (!pushed_back) {
        tok->u.text = (char *)intern_string(text);
    }
    return tok;
}

static int actions_enabled(void)
{
    return (*g_mode_top)->active != 0;
}

/* Skip the rest of the statement after a syntax error. */
static void recover(const grammar_t *e, int matched)
{
    fflush(stdout);
    if ((*g_mode_top)->skip != NULL) {
        (*g_mode_top)->skip(g_name_buf);
    }
    if (e->action != NULL) {
        /* There is no real token: the action gets phantom token 0 or 1,
         * chosen by `matched`.  Actions that look at the token see that
         * number on purpose. */
        e->action(&g_phantom[matched ? 1 : 0]);
    }
    fflush(stdout);
}

/* Run the rule whose header (or calling entry) is `e`. */
static int parse_rule(const grammar_t *e, list_node_t *tok)
{
    int base;
    if (g_stop_parsing) {
        return 1;
    }
    base = e->base;
    if (e->action != NULL && actions_enabled()) {
        e->action(tok);
    }
    e = &k_grammar[e->next + base];
    for (;;) {
        int matched = 0;
        int resync = 0;
        int ok = 0;
        while (!ok) {
            if (e->token == T_TEXT &&
                (tok->kind == T_NUMBER || tok->kind == T_IDENT || tok->kind == T_IDENT_NUM)) {
                tok->kind = T_TEXT;
            }
            switch (e->op) {
            case OP_CALL:
            case OP_MUST_CALL:
                ok = parse_rule(&k_grammar[e->base], tok);
                matched = 0;
                break;
            case OP_PUSHBACK:
                list_append(tok, &g_pushback);
                return 1;
            case OP_FAIL:
                return 0;
            case OP_ERROR:
                report_error(ERR_SYNTAX, tok_text(tok));
                return 0;
            case OP_RULE:
                fatal_error(0x3ef, "parser");
            default:
                if (tok->kind == e->token) {
                    ok = 1;
                }
                matched = 1;
                break;
            }
            if (ok) {
                break;
            }
            switch (e->op) {
            case OP_ACCEPT:
                return 1;
            case OP_FIRST:
                return 0;
            case OP_MUST:
            case OP_LAST:
            case OP_MUST_CALL:
                report_error(ERR_SYNTAX, tok_text(tok));
                recover(e, matched);
                return 1;
            case OP_RESYNC:
                report_error(ERR_SYNTAX, tok_text(tok));
                ok = 1;
                resync = 1;
                break;
            case OP_EXPECT:
                report_error(ERR_MISSING_SYMBOL, token_name(e->token));
                e++;
                break;
            default:
                e++;
                break;
            }
        }
        if (e->action == NULL || !actions_enabled()) {
            if (matched && e->op != OP_LAST) {
                node_free(tok);
            }
        } else {
            e->action(tok);
        }
        if (g_stop_parsing) {
            return 1;
        }
        if (e->op == OP_LAST) {
            break;
        }
        tok = next_token();
        e = resync ? &k_grammar[base + 1] : &k_grammar[e->next + base];
    }
    return 1;
}

/* -------------------------------------------------------------------------
 * Helpers of the actions
 * ------------------------------------------------------------------------- */

/* Is the statement being read executed (not inside a false $IF / finished
 * $WHILE or an inactive block)? */
static int block_active(void)
{
    if (g_block == NULL) {
        return 1;
    }
    if (!g_block->active) {
        return 0;
    }
    if ((g_block->type == BLOCK_IF || g_block->type == BLOCK_WHILE) && g_block->cond == 0) {
        return 0;
    }
    return 1;
}

/* Open a nested block; it is executed only if the enclosing one is. */
static void push_block(int type)
{
    block_t *b;
    if (g_block_depth >= BLOCK_MAX) {
        fatal_error(ERR_OUT_OF_MEMORY, "too many nested statements");
    }
    b = &g_blocks[g_block_depth++];
    b->type = (uint8_t)type;
    if (g_block == NULL) {
        b->parent = NULL;
        b->active = 1;
    } else {
        if (!g_block->active) {
            b->active = 0;
        } else if (g_block->type == BLOCK_IF || g_block->type == BLOCK_WHILE) {
            b->active = g_block->cond;
        } else {
            b->active = 1;
        }
        b->parent = g_block;
    }
    g_block = b;
}

static void init_condition(void)
{
    g_block->else_seen = 0;
    g_block->nvalues = 0;
    g_block->cond = 1;
    g_block->compare = 0;
    g_block->pos = 0;
    g_cond_loop = NULL;
}

static void pop_block(void)
{
    if (g_block == NULL) {
        fatal_error(0x3ea, "parser actions");
    }
    if (g_block_depth == 0) {
        g_block = g_block->parent;
        fatal_error(0x3eb, "parser actions");
    }
    g_block_depth--;
    g_block = g_block->parent;
}

static int radix_of(int base_token)
{
    switch (base_token) {
    case T_HEX: return 16;
    case T_OCTAL: return 8;
    case T_DECIMAL: return 10;
    case T_BINARY: return 2;
    default: fatal_error(0x3ec, "parser actions");
    }
}

/* Read an unsigned decimal number, whatever the base in effect.  Returns 0
 * if there is none. */
static int scan_unsigned(const char *text, unsigned long *value)
{
    unsigned v;
    if (sscanf(text, "%u", &v) != 1) {
        return 0;
    }
    *value = v;
    return 1;
}

/* Index of the ORDER column of a name, -1 if none. */
static int find_order(const char *name)
{
    for (int i = 0; i < g_order_count; i++) {
        if (strcmp(g_order[i].sym->name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* Does a list entry with these flags mean the positive level of
 * ORDER column idx? */
static int order_polarity(int idx, int flags)
{
    const order_t *o = &g_order[idx];
    int b = (g_syms[o->sym_index].flags & 0x10) == 0;
    if (!o->positive) {
        b = !b;
    }
    return ((flags & 1) != 0) == b;
}

/* The opposite level of a value token unless `positive`. */
static int invert_value(int token, int positive)
{
    if (positive) {
        return token;
    }
    switch (token) {
    case T_VAL_H: return T_VAL_L;
    case T_VAL_L: return T_VAL_H;
    case T_VAL_C: return T_VAL_K;
    case T_VAL_K: return T_VAL_C;
    case T_VAL_0: return T_VAL_1;
    case T_VAL_1: return T_VAL_0;
    default: return token;
    }
}

static var_t *find_var(const char *name)
{
    for (var_t *v = g_vars; v != NULL; v = v->next) {
        if (v->name != NULL && strcmp(v->name, name) == 0) {
            return v;
        }
    }
    return NULL;
}

/* Append an empty variable to g_vars. */
static var_t *new_var(void)
{
    var_t *v = alloc_tracked(sizeof(*v), "allocating var space");
    v->members = alloc_tracked(sizeof(list_t), "allocating var space");
    if (g_vars == NULL) {
        g_vars = v;
    } else {
        g_vars_tail->next = v;
    }
    g_vars_tail = v;
    return v;
}

/* The $FOR block whose loop variable has this name. */
static block_t *find_loop_var(const char *name)
{
    for (block_t *b = g_block; b != NULL; b = b->parent) {
        if (b->type == BLOCK_FOR && b->var != NULL && strcmp(b->var, name) == 0) {
            return b;
        }
    }
    return NULL;
}

static macro_t *find_macro(const char *name)
{
    for (macro_t *m = g_macros; m != NULL; m = m->next) {
        if (strcmp(m->name != NULL ? m->name : "", name) == 0) {
            return m;
        }
    }
    return NULL;
}

/* The argument of the running macro call given for parameter
 * `name`, NULL if there is no such parameter. */
static list_node_t *macro_arg(const char *name)
{
    list_node_t *arg = g_call->args.head;
    for (list_node_t *param = g_call->macro->params->head; param != NULL; param = param->next) {
        if (strcmp(tok_text(param), name) == 0) {
            return arg;
        }
        if (arg != NULL) {
            arg = arg->next;
        }
    }
    return NULL;
}

/* Replace a token that names a macro parameter by the argument.  Returns 1
 * if it was replaced. */
static int substitute_arg(list_node_t *tok)
{
    list_node_t *arg;
    if (g_call == NULL) {
        return 0;
    }
    arg = macro_arg(tok_text(tok));
    if (arg == NULL) {
        return 0;
    }
    tok->u.text = arg->u.text;
    tok->kind = arg->kind;
    return 1;
}

/* Drop the strings interned since the mark was taken (new ORDER, end of
 * $COMP, end of a $CALL).  The strings themselves stay valid. */
static void release_strings(list_node_t *mark)
{
    if (mark != NULL) {
        g_strings.tail = mark;
        mark->next = NULL;
    }
}

/* Turn a number between quotes into one value token per bit. */
static void push_number_bits(list_t *list, int base_token, int quote, char *text, int width)
{
    const char_token_t *ct;
    int bits, total, code;
    switch (base_token) {
    case T_HEX: bits = 4; break;
    case T_OCTAL: bits = 3; break;
    case T_DECIMAL: bits = 4; break;
    case T_BINARY: bits = 1; break;
    default: fatal_error(0x3fa, VTEXT("", "module SUP"));
    }
    total = (width < 2) ? (int)strlen(text) * bits : width;
    ct = find_char_token(k_value_chars, text[0]);
    if (ct == NULL || ct->token == T_VAL_0 || ct->token == T_VAL_1 || ct->token == T_VAL_C) {
        code = -1;
    } else {
        code = ct->token;
    }
    if (code < 0) {
        int zero, one;
        if (quote == T_QUOTE) {
            zero = T_VAL_0;
            one = T_VAL_1;
        } else if (quote == T_DQUOTE) {
            zero = T_VAL_L;
            one = T_VAL_H;
        } else {
            fatal_error(ERR_SYNTAX, "bad number");
        }
        list_push_bits(list, (unsigned)parse_number(text, base_token), zero, one);
        list_pad_front(list, total, zero);
    } else {
        list_pad_front(list, total, code);
    }
}

/* Compare two texts ignoring white space. */
static int differ_ignoring_blanks(const char *a, const char *b)
{
    for (;;) {
        while (*a != '\0' && is_space(*a)) {
            a++;
        }
        while (*b != '\0' && is_space(*b)) {
            b++;
        }
        if (*a != *b) {
            return 1;
        }
        if (*a == '\0') {
            return 0;
        }
        a++;
        b++;
    }
}

/* -------------------------------------------------------------------------
 * Actions: header items, ORDER, FIELD, BASE, lists
 * ------------------------------------------------------------------------- */

/* Small actions: return to the previous scanner mode / just drop the token. */
static void act_pop_mode(list_node_t *tok)
{
    node_free(tok);
    mode_pop();
}

static void act_free_token(list_node_t *tok)
{
    node_free(tok);
}

/* NAME, PARTNO, ... : the rest up to ';' is the item's text. */
static void act_header_item(list_node_t *tok)
{
    g_header_kind = (int8_t)tok->kind;
    mode_push();
    *g_mode_top = &k_mode_to_semicolon;
}

/* Compare the item with the one compiled into the .abs file. */
static void act_header_text(list_node_t *tok)
{
    int index;
    switch (g_header_kind) {
    case T_NAME: index = 0; g_header_seen |= 0x01; break;
    case T_PARTNO: index = 1; g_header_seen |= 0x02; break;
    case T_REV: index = 2; g_header_seen |= 0x04; break;
    case T_DATE: index = 3; g_header_seen |= 0x08; break;
    case T_DESIGNER: index = 4; g_header_seen |= 0x10; break;
    case T_COMPANY: index = 5; g_header_seen |= 0x20; break;
    case T_ASSY: index = 6; g_header_seen |= 0x40; break;
    case T_LOCATION: index = 7; g_header_seen |= 0x80; break;
    case T_DEVICE:
    case T_FORMAT: index = -1; break;
    default: index = 0; break;
    }
    if (index >= 0 && (g_header_items & (1u << index))) {
        const char *item = g_abs_extra != NULL ? g_abs_extra : "";
        for (int i = 0; i < index; i++) {
            item += strlen(item) + 1;
        }
        if (differ_ignoring_blanks(tok_text(tok), item)) {
            report_warning(ERR_HEADER_INCONSISTENT, "");
        }
    }
    act_pop_mode(tok);
}

/* Warn if a header item is missing. */
static void check_header_items(void)
{
    if (g_header_items == 0) {
        return;     /* a JEDEC file without header items: none are expected */
    }
    for (int i = 0; i < 8; i++) {
        if (!(g_header_seen & (1u << i))) {
            print_warning_now(ERR_HEADER_MISSING, " ");
            return;
        }
    }
}

/* Forget the previous ORDER and everything that
 * depended on it. */
static void reset_order_state(void)
{
    for (int i = 0; i < g_nsyms; i++) {
        order_t *o = &g_order[i];
        o->positive = 0;
        o->pin = 0;
        o->sym = NULL;
        o->column = 0;
        o->text_col = 0;
        o->sym_index = 0;
        o->width = 0;
        list_reset(&o->before);
        if (g_vsima) {
            o->result = 0;
            o->value = 0;
        }
    }
    if (g_strings_order_mark == NULL) {
        g_strings_order_mark = g_strings.tail;
        g_strings_order_count = g_strings.count;
    } else {
        release_strings(g_strings_order_mark);
        g_strings.count = g_strings_order_count;
    }
    g_macros = g_macros_tail = NULL;
    g_vars = g_vars_tail = NULL;
    g_block = NULL;
    g_call = NULL;
    list_reset(&g_pushback);
    memset(g_header_lines, 0, sizeof(g_header_lines));
}

/* ORDER */
static void act_order_begin(list_node_t *tok)
{
    node_free(tok);
    g_listing_off = 0;
    if (g_block_depth > 0) {
        fatal_error(ERR_SYNTAX, "misplaced ORDER command");
    }
    if (g_strings_order_mark == NULL) {
        check_header_items();
    }
    if (g_vsima) {
        if (!g_first_order) {
            reset_simulation();
        }
        g_first_order = 0;
    }
    reset_order_state();
    g_order_count = 0;
    g_order[0].width = 0;
}

/* A name of the ORDER list. */
static void act_order_name(list_node_t *tok)
{
    field_t *field = find_field(tok_text(tok));
    if (field == NULL) {
        sym_t *sym;
        order_t *o;
        int i;
        int node_level = 0;
        if ((int)g_nsyms < g_order_count) {
            fatal_error(ERR_TOO_MANY_SYMBOLS, "");
        }
        sym = find_symbol(tok_text(tok));
        if (sym == NULL && (g_options & OPT_FUSEMAP)) {
            /* P<n> for a pin that has a name from the .pld file: the level
             * of the pin, whatever polarity the name was declared with */
            int inverted = 0;
            sym = jedec_pin_symbol(tok_text(tok), &inverted, &node_level);
            if (sym != NULL && inverted) {
                tok->flags ^= 1;
            }
        }
        if (sym == NULL) {
            fatal_error(ERR_UNKNOWN_SYMBOL, tok_text(tok));
        }
        i = g_order_count++;
        o = &g_order[i];
        o->positive = ((sym->flags & 0x10) == 0) == ((tok->flags & 1) != 0);
        o->node_level = (uint8_t)node_level;
        o->pin = sym->pin;
        o->sym = sym;
        o->column = (uint16_t)i;
        o->text_col = 0;
        o->sym_index = (int16_t)(sym - g_syms);
        if (o->width == 0) {
            o->width = 1;
        }
        tok->flags &= 0xfb;
        g_order[g_order_count].width = 0;
        list_reset(&g_order[g_order_count].before);
        if (!g_vsima) {
            if (g_first_order) {
                reset_simulation();
            }
            g_first_order = 0;
        }
    } else {
        list_t *members = list_clone(field->members);
        if (members != NULL) {
            g_order[g_order_count].width = (int16_t)members->count;
            g_order[g_order_count].field = field;
            list_concat(members, &g_pushback);
            list_free_header(members);
        }
    }
}

/* A quoted text or %n spacing in the ORDER list. */
static void act_order_text(list_node_t *tok)
{
    list_append(tok, &g_order[g_order_count].before);
}

/* Build the heading printed above the vectors: a line of '=',
 * then the names written downwards (fields across), bottom aligned, with
 * '!' above inverted columns. */
static void build_vector_header(void)
{
    const order_t *last = &g_order[g_order_count > 0 ? g_order_count - 1 : 0];
    int width = last->text_col + last->width;
    int size = width + 10;
    int nlines = 1;
    order_t *o = g_order;
    if (g_order_count <= 0) {
        /* Without ORDER columns the heading is only the line of '='. */
        width = 0;
        size = 10;
    }
    for (int i = 0; i < 2; i++) {
        g_header_lines[i] = alloc_tracked((size_t)size, "generating vector header");
        memset(g_header_lines[i], i == 0 ? '=' : ' ', (size_t)(width + 9));
    }
    if (g_order_count <= 0) {
        return;
    }
    for (;;) {
        int row = 1;
        int index = (int)(o - g_order);
        int positive = (g_syms[o->sym_index].flags & 0x10) == 0;
        const char *name;
        int len;
        if (!o->positive) {
            positive = !positive;
        }
        if (!positive) {
            g_header_lines[1][o->text_col + o->width / 2 + 6] = '!';
            row = 2;
        }
        name = (o->width < 2) ? o->sym->name : o->field->name;
        len = o->width;
        if (len > 1 && strlen(name) > (size_t)len) {
            int room;
            if (index + o->width < g_order_count) {
                room = o[o->width].text_col - o->text_col - 1;
            } else {
                room = size - o->text_col - 7;
            }
            if ((int)strlen(name) <= room) {
                len = (int)strlen(name);
            }
        }
        for (; *name != '\0' && row < HEADER_LINES; row++) {
            int indent;
            char *dst;
            if (nlines < row) {
                g_header_lines[row] = alloc_tracked((size_t)size, "generating vector header");
                memset(g_header_lines[row], ' ', (size_t)(width + 9));
                nlines++;
            }
            indent = (len - (int)strlen(name)) / 2;
            if (indent < 0) {
                indent = 0;
            }
            dst = g_header_lines[row] + o->text_col + indent + 6;
            for (int k = 0; k < len; k++) {
                if (dst - g_header_lines[row] < size - 1) {
                    *dst = *name;
                }
                name++;
                if (*name == '\0') {
                    break;
                }
                dst++;
            }
        }
        if (index + o->width >= g_order_count) {
            break;
        }
        o += o->width;
    }
    /* Move every column down until its last line is filled. */
    o = g_order;
    for (;;) {
        int index = (int)(o - g_order);
        int at = o->text_col + (o->width - 1) / 2 + 6;
        int len = o->width;
        if (len > 1) {
            if (index + o->width < g_order_count) {
                len = o[o->width].text_col - o->text_col - 1;
            } else {
                len = size - o->text_col - 7;
            }
        }
        while (g_header_lines[nlines][at] == ' ') {
            if (nlines < 2) {
                break;      /* an empty column cannot be moved down */
            }
            for (int k = 0; k < len; k++) {
                int col = o->text_col + k + 6;
                if (col >= size - 1) {
                    break;
                }
                for (int r = nlines; r > 1; r--) {
                    g_header_lines[r][col] = g_header_lines[r - 1][col];
                }
                g_header_lines[1][col] = ' ';
            }
        }
        if (index + o->width >= g_order_count) {
            break;
        }
        o += o->width;
    }
}

/* Make every FIELD usable as a variable. */
static void fields_to_vars(void)
{
    for (field_t *f = g_fields; f != NULL; f = f->next) {
        var_t *v = new_var();
        list_t *members;
        v->name = intern_string(f->name);
        v->state = 0;
        members = list_clone(f->members);
        if (members == NULL) {
            break;
        }
        list_concat(members, v->members);
    }
}

/* Make every ORDER name usable as a one-bit variable. */
static void order_to_vars(void)
{
    for (int i = 0; i < g_order_count; i++) {
        var_t *v = new_var();
        list_node_t *node;
        v->name = intern_string(g_order[i].sym->name);
        v->state = 0;
        node = alloc_tracked(sizeof(*node), "allocating variable list");
        node->u.text = (char *)v->name;
        node->kind = T_IDENT;
        node->flags |= 1;
        list_append(node, v->members);
    }
}

/* End of the ORDER statement. */
static void act_order_end(list_node_t *tok)
{
    (void)tok;
    print_vector(1, -1);        /* only to lay out the columns */
    build_vector_header();
    fields_to_vars();
    order_to_vars();
}

/* BASE */
static void act_base(list_node_t *tok)
{
    g_base = (int8_t)tok->kind;
}

/* Start collecting a list (list_stack_push as an action). */
static void act_list_begin(list_node_t *tok)
{
    (void)tok;
    list_stack_push();
}

/* Hand the collected list to the parser as tokens. */
static void act_list_to_pushback(list_node_t *tok)
{
    (void)tok;
    list_concat(g_list_top, &g_pushback);
    list_stack_pop();
}

/* '!' in front of a name */
static void act_negate(list_node_t *tok)
{
    g_positive = 0;
    node_free(tok);
}

/* ".." */
static void act_range(list_node_t *tok)
{
    (void)tok;
    g_in_range = 1;
}

/* A name (or number) in a list. */
static void act_list_name(list_node_t *tok)
{
    if (g_positive == g_field_positive) {
        tok->flags |= 1;
    }
    tok->flags |= 4;
    tok->kind = T_IDENT;
    list_append(tok, g_list_top);
    g_positive = 1;
}

/* A name ending in a number; the second one of "a0..7" expands
 * the range. */
static void act_list_numbered_name(list_node_t *tok)
{
    int positive = (g_positive == g_field_positive);
    int number = trailing_number(tok_text(tok));
    if (number < 0) {
        report_error(ERR_BAD_NUMBER, tok_text(tok));
        number = 0;
    }
    if (!g_in_range) {
        strip_trailing_number(g_name_buf, tok_text(tok));
        g_range_stem = intern_string(g_name_buf);
        g_range_first = number;
        tok->kind = T_IDENT_NUM;
        tok->flags = (uint8_t)(positive | 4);
        list_append(tok, g_list_top);
    } else {
        int flags = g_list_top->tail != NULL ? g_list_top->tail->flags : 0;
        list_t *range = list_name_range(g_range_stem != NULL ? g_range_stem : "", g_range_first, number,
                                        T_IDENT_NUM, flags);
        list_concat(g_list_top, range);
        list_free_header(range);
        g_in_range = 0;
    }
    g_positive = 1;
}

/* '"' in a list: read the text up to the closing quote. */
static void act_text_begin(list_node_t *tok)
{
    (void)tok;
    mode_push();
    *g_mode_top = &k_mode_to_quote;
}

/* The text read up to the closing quote: append it to the list. */
static void act_list_text(list_node_t *tok)
{
    mode_pop();
    tok->flags |= 4;
    tok->kind = T_ORDER_TEXT;
    list_append(tok, g_list_top);
    g_positive = 1;
}

/* %n */
static void act_list_spacing(list_node_t *tok)
{
    tok->flags |= 4;
    tok->kind = T_ORDER_SPACE;
    list_append(tok, g_list_top);
    g_positive = 1;
}

/* FIELD name */
static void act_field_name(list_node_t *tok)
{
    list_stack_push();
    g_cur_field = define_field(tok_text(tok));
}

static void act_field_negate(list_node_t *tok)
{
    g_field_positive = 0;
    node_free(tok);
}

/* End of a FIELD list: the collected names become the members. */
static void act_field_end(list_node_t *tok)
{
    (void)tok;
    g_cur_field->members = list_new();
    list_copy_header(g_cur_field->members, g_list_top);
    g_cur_field = NULL;
    list_stack_pop();
}

/* ']' */
static void act_field_bracket(list_node_t *tok)
{
    node_free(tok);
    g_field_positive = 1;
    g_positive = 1;
}

/* $MSG: printed only where the statement is executed, not in a branch of
 * an $IF that is not taken */
static void act_msg_end(list_node_t *tok)
{
    (void)tok;
    if (block_active()) {
        print_message(g_list_top);
    }
    list_free_nodes(g_list_top);
    list_stack_pop();
}

/* $EXIT: stops the simulation where the statement is executed */
static void act_exit_end(list_node_t *tok)
{
    if (block_active()) {
        g_stop_parsing = 1;
    }
    act_msg_end(tok);
}

/* $TRACE n */
static void act_trace(list_node_t *tok)
{
    int level = (int)strtol(tok_text(tok), NULL, 10);
    if (level > 4) {
        report_error(ERR_TRACE_LEVEL, tok_text(tok));
        level = 0;
    }
    g_trace_level = level;
    node_free(tok);
}

static void act_simoff(list_node_t *tok)
{
    g_sim_on = 0;
    node_free(tok);
}

static void act_simon(list_node_t *tok)
{
    g_sim_on = 1;
    node_free(tok);
}

/* STUCKH n / STUCKL n (csima only; vsima ignores them) */
static void set_stuck(list_node_t *tok, int level)
{
    unsigned long id = 0;
    int row;
    scan_unsigned(tok_text(tok), &id);
    row = stuck_fault_row(id);
    if (row < 0) {
        report_error(ERR_BAD_FAULT_ID, tok_text(tok));
    } else {
        g_stuck[row] = (uint8_t)level;
    }
    node_free(tok);
}

static void act_stuck_low(list_node_t *tok)
{
    if (!g_vsima) {
        set_stuck(tok, LV_LOW);
    }
}

static void act_stuck_high(list_node_t *tok)
{
    if (!g_vsima) {
        set_stuck(tok, LV_HIGH);
    }
}

/* -------------------------------------------------------------------------
 * Actions: VECTORS
 * ------------------------------------------------------------------------- */

/* Switch the scanner to test vector characters. */
static void enter_value_mode(void)
{
    mode_push();
    g_listing_off = 1;
    *g_mode_top = &k_mode_values;
}

/* "VECTORS:" - print the heading. */
static void act_vectors_begin(list_node_t *tok)
{
    (void)tok;
    g_listing_off = 1;
    if (g_options & OPT_LISTING) {
        stream_printf(g_list_file, "\f\n");
        for (int i = 0; i < HEADER_LINES && g_header_lines[i] != NULL; i++) {
            stream_printf(g_list_file, "%s\n", g_header_lines[i]);
        }
        stream_printf(g_list_file, "%s\n", g_header_lines[0] != NULL ? g_header_lines[0] : "(null)");
        fflush(g_list_file);
    }
    enter_value_mode();
}

/* One test value. */
static void act_value(list_node_t *tok)
{
    if (g_value_count < g_order_count) {
        if (block_active()) {
            g_order[g_value_count].value = (int8_t)tok->kind;
            g_value_count++;
        }
        node_free(tok);
    } else {
        report_error(ERR_EXCESSIVE_VALUE, tok_text(tok));
        node_free(tok);
    }
}

/* Is the vector to be applied again ($REPEAT)? */
static int repeat_again(void)
{
    if (!g_repeat_active) {
        return 0;
    }
    if (g_repeat_kind != T_REPEAT) {
        g_repeat_active = 0;
    } else {
        g_repeat_done += g_repeat_step;
        if (g_repeat_count <= g_repeat_done) {
            g_repeat_active = 0;
        }
    }
    return g_repeat_active;
}

/* End of a vector line: simulate it. */
static void act_vector_end(list_node_t *tok)
{
    if (g_value_count == 0 || !block_active()) {
        return;
    }
    if (g_value_count != g_order_count) {
        fatal_error(ERR_INSUFFICIENT_VALUES, "");
    }
    g_vector_running = 1;
    do {
        simulate_vector();
        print_vector(0, -1);
        if (g_options & OPT_JEDEC) {
            jedec_vector();
        }
        g_vector_number++;
    } while (repeat_again());
    g_quote_kind = 0;
    g_value_count = 0;
    g_vector_running = 0;
    node_free(tok);
}

/* $REPEAT n */
static void act_repeat_count(list_node_t *tok)
{
    g_repeat_kind = T_REPEAT;
    g_repeat_done = 0;
    g_repeat_step = 1;
    g_repeat_active = 1;
    g_repeat_count = (int)strtol(tok_text(tok), NULL, 10);
    node_free(tok);
}

/* '$' in the vectors: a directive up to the end of the line. */
static void act_directive_begin(list_node_t *tok)
{
    (void)tok;
    mode_push();
    *g_mode_top = &k_mode_directive;
}

/* ' or " in a vector: a number follows. */
static void act_quote_begin(list_node_t *tok)
{
    g_quote_kind = (char)tok->kind;
    g_quote_text[0] = '\0';
    node_free(tok);
    mode_pop();
}

/* A character that ends the directive part: read it again as a
 * test value. */
static void act_unget_char(list_node_t *tok)
{
    ungetc((unsigned char)tok_text(tok)[0], g_source->fp);
    g_source->col--;
    node_free(tok);
    mode_pop();
}

/* The number between quotes becomes value tokens. */
static void act_quoted_number(list_node_t *tok)
{
    static char empty[2];
    list_stack_push();
    /* The digits are upper-cased in place. */
    push_number_bits(g_list_top, g_base, g_quote_kind, tok->u.text != NULL ? tok->u.text : empty,
                     g_order[g_value_count].width);
    g_quote_kind = 0;
}

/* Closing quote. */
static void act_quote_end(list_node_t *tok)
{
    (void)tok;
    if (g_quote_text[0] != '\0') {
        list_node_t *node = node_new();
        node->u.text = g_quote_text;
        node->kind = T_TEXT;
        act_quoted_number(node);
        node_free(node);
    }
    enter_value_mode();
    act_list_to_pushback(NULL);
}

/* '*' between quotes */
static void act_quoted_star(list_node_t *tok)
{
    size_t used = strlen(g_quote_text);
    if (used + 1 < sizeof(g_quote_text)) {
        g_quote_text[used] = '*';
        g_quote_text[used + 1] = '\0';
    }
    node_free(tok);
}

/* -------------------------------------------------------------------------
 * Actions: $IF, $DO, $WHILE, $FOR
 * ------------------------------------------------------------------------- */

/* $IF: open a block and start a condition. */
static void act_if_begin(list_node_t *tok)
{
    g_have_const = 0;
    push_block(BLOCK_IF);
    init_condition();
    g_relation = 0;
    g_const_value = 0;
    node_free(tok);
}

/* = # < > */
static void act_relation(list_node_t *tok)
{
    switch (tok->kind) {
    case T_EQUAL: g_relation |= 1; break;
    case T_GREATER: g_relation |= 8; break;
    case T_LESS: g_relation |= 4; break;
    case T_HASH: g_relation |= 2; break;
    default: break;
    }
    node_free(tok);
}

/* ' or " in front of a constant */
static void act_const_quote(list_node_t *tok)
{
    g_have_const = 1;
    g_const_is_levels = (tok->kind == T_DQUOTE);
    node_free(tok);
}

/* The constant of a condition. */
static void act_cond_number(list_node_t *tok)
{
    if (g_cond_loop != NULL) {
        fatal_error(ERR_SYNTAX, "unsigned decimal value expected");
    }
    g_block->compare = (uint32_t)strtoul(tok_text(tok), NULL, radix_of(g_base));
    node_free(tok);
}

/* A name in a condition: a signal, a variable (expanded into its
 * members) or a $FOR variable. */
static void act_cond_operand(list_node_t *tok)
{
    var_t *var;
    if (g_call != NULL) {
        list_node_t *arg = macro_arg(tok_text(tok));
        if (arg != NULL) {
            tok->u.text = arg->u.text;
            if (arg->kind != T_IDENT && arg->kind != T_IDENT_NUM) {
                fatal_error(ERR_SYNTAX, "wrong value for argument in macro call");
            }
            tok->kind = arg->kind;
        }
    }
    var = find_var(tok_text(tok));
    if (var == NULL) {
        g_cond_loop = find_loop_var(tok_text(tok));
        if (g_cond_loop == NULL) {
            fatal_error(ERR_UNKNOWN_SYMBOL, tok_text(tok));
        }
    } else {
        int idx = find_order(tok_text(tok));
        if (idx < 0) {
            list_t *members = list_clone(var->members);
            if (members == NULL) {
                fatal_error(0x3e9, "parser actions");
            }
            if (!(tok->flags & 1)) {
                for (list_node_t *n = members->head; n != NULL; n = n->next) {
                    n->flags ^= 1;
                }
            }
            list_concat(members, &g_pushback);
            list_free_header(members);
            node_free(tok);
            return;
        }
        g_block->nvalues++;
        if (g_block->nvalues >= 1 && g_block->nvalues <= COND_VALUES_MAX) {
            g_cond_values[g_block->nvalues - 1] =
                (char)invert_value(g_order[idx].result, order_polarity(idx, tok->flags));
        }
    }
    node_free(tok);
}

/* Evaluate "signals <relation> constant". */
static void eval_condition(void)
{
    block_t *b = g_block;
    int nvalues = b->nvalues < COND_VALUES_MAX ? b->nvalues : COND_VALUES_MAX;
    if (!g_have_const) {
        return;
    }
    g_const_value = 0;
    for (int i = 0; i < nvalues; i++) {
        char c = g_cond_values[i];
        g_const_value *= 2;
        if (c == T_VAL_H || c == T_VAL_1) {
            g_const_value += 1;
        } else if (c != T_VAL_L && c != T_VAL_0) {
            fatal_error(ERR_SYNTAX, "bad value in condition");
        }
    }
    b->cond = 0;
    switch (g_relation) {
    case 1: b->cond = (g_const_value == b->compare); break;
    case 2: b->cond = (g_const_value != b->compare); break;
    case 4: b->cond = (g_const_value < b->compare); break;
    case 5: b->cond = (g_const_value <= b->compare); break;
    case 8: b->cond = (g_const_value > b->compare); break;
    case 9: b->cond = (g_const_value >= b->compare); break;
    default: fatal_error(ERR_SYNTAX, "condition");
    }
}

/* Compare the $FOR variable with a number. */
static int compare_loop_var(const char *text)
{
    unsigned long parsed;
    unsigned value, cur;
    if (!scan_unsigned(text, &parsed)) {
        fatal_error(ERR_SYNTAX, "bad value in condition");
    }
    value = (unsigned)parsed & 0xffffu;
    cur = g_cond_loop->cur;
    switch (g_relation) {
    case 1: return cur == value;
    case 2: return cur != value;
    case 4: return cur < value;
    case 5: return cur <= value;
    case 8: return value < cur;
    case 9: return value <= cur;
    default: fatal_error(ERR_SYNTAX, "bad condition");
    }
}

/* The unquoted right side of a condition: test values to compare
 * the signals with, or a number for a $FOR variable. */
static void act_cond_text(list_node_t *tok)
{
    block_t *b;
    const char *text;
    if (substitute_arg(tok) && (tok->kind == T_QUOTE || tok->kind == T_DQUOTE)) {
        g_have_const = 1;
        if (tok->kind == T_DQUOTE) {
            g_const_is_levels = 1;
        }
        act_cond_number(tok);
        return;
    }
    b = g_block;
    text = tok_text(tok);
    if (g_cond_loop != NULL) {
        b->cond = (int16_t)compare_loop_var(text);
        node_free(tok);
        return;
    }
    if ((int)strlen(text) != b->nvalues) {
        fatal_error(ERR_SYNTAX, "bad field width in condition");
    }
    b->cond = 1;
    for (int i = 0; i < b->nvalues && i < COND_VALUES_MAX; i++) {
        int code;
        switch (text[i]) {
        case '0': code = T_VAL_0; break;
        case '1': code = T_VAL_1; break;
        case 'C': code = T_VAL_C; break;
        case 'H': code = T_VAL_H; break;
        case 'K': code = T_VAL_K; break;
        case 'L': code = T_VAL_L; break;
        case 'N': code = T_VAL_N; break;
        case 'P': code = T_VAL_P; break;
        case 'X': code = T_VAL_X; break;
        case 'Z': code = T_VAL_Z; break;
        default: {
            char bad[2] = { text[i], '\0' };
            fatal_error(ERR_BAD_TEST_VALUE, bad);
        }
        }
        if (g_relation == 1) {
            if (g_cond_values[i] != code) {
                b->cond = 0;
                node_free(tok);
                return;
            }
        } else if (g_relation == 2) {
            if (g_cond_values[i] != code) {
                node_free(tok);
                return;
            }
        } else {
            fatal_error(ERR_SYNTAX, "bad condition");
        }
    }
    if (g_relation == 2) {
        b->cond = 0;
    }
    node_free(tok);
}

/* eval_condition as the action at the ':' of $IF */
static void act_if_cond(list_node_t *tok)
{
    (void)tok;
    eval_condition();
}

/* $ELSE: invert the condition of the open $IF. */
static void act_else(list_node_t *tok)
{
    /* Without an open block $ELSE works on g_null_block, which is all
     * zeroes, so a first stray $ELSE passes silently.  This is deliberate. */
    block_t *b = g_block != NULL ? g_block : &g_null_block;
    (void)tok;
    if (b->type != BLOCK_IF || b->else_seen) {
        fatal_error(ERR_SYNTAX, "missplaced ELSE");
    }
    b->cond = (b->cond != 1);
    b->else_seen = 1;
}

static void act_endif(list_node_t *tok)
{
    (void)tok;
    if (g_block != NULL && g_block->type != BLOCK_IF) {
        fatal_error(ERR_SYNTAX, "missplaced ENDIF");
    }
    pop_block();
}

/* $DO */
static void act_do(list_node_t *tok)
{
    (void)tok;
    push_block(BLOCK_DO);
    init_condition();
    g_block->pos = ftell(g_source->fp);
}

/* $UNTIL */
static void act_until_begin(list_node_t *tok)
{
    if (g_block == NULL || g_block->type != BLOCK_DO) {
        fatal_error(ERR_SYNTAX, "misplaced UNTIL");
    }
    g_relation = 0;
    g_const_value = 0;
    g_block->nvalues = 0;
    g_block->compare = 0;
    g_block->cond = 0;
    node_free(tok);
}

/* End of $UNTIL: loop back to the $DO while the condition is false. */
static void act_until_end(list_node_t *tok)
{
    (void)tok;
    if (g_block == NULL || g_block->type != BLOCK_DO) {
        fatal_error(0x406, "parser actions");
    }
    if (!g_block->active) {
        g_block->cond = 1;
    } else {
        eval_condition();
    }
    if (g_block->cond == 0) {
        fseek(g_source->fp, g_block->pos, SEEK_SET);
    } else {
        pop_block();
    }
}

/* $WHILE; the loop jumps back to the '$'. */
static void act_while_begin(list_node_t *tok)
{
    node_free(tok);
    push_block(BLOCK_WHILE);
    init_condition();
    g_relation = 0;
    g_const_value = 0;
    g_block->cond = 0;
    g_block->pos = ftell(g_source->fp) - 6;
}

static void act_while_cond(list_node_t *tok)
{
    (void)tok;
    eval_condition();
}

/* $ENDW */
static void act_endw(list_node_t *tok)
{
    if (g_block == NULL || g_block->type != BLOCK_WHILE) {
        fatal_error(ERR_SYNTAX, "misplaced ENDW");
    }
    if (!g_block->active) {
        g_block->cond = 0;
    }
    if (g_block->cond == 1) {
        fseek(g_source->fp, g_block->pos, SEEK_SET);
    }
    pop_block();
    list_append(tok, &g_pushback);
}

/* $FOR */
static void act_for_begin(list_node_t *tok)
{
    push_block(BLOCK_FOR);
    g_block->var = NULL;
    node_free(tok);
}

static void act_for_var(list_node_t *tok)
{
    substitute_arg(tok);
    if (find_loop_var(tok_text(tok)) != NULL) {
        fatal_error(ERR_SYNTAX, "loop index variable already in use");
    }
    g_block->var = tok->u.text;
    node_free(tok);
}

static void act_for_start(list_node_t *tok)
{
    unsigned long v;
    substitute_arg(tok);
    if (scan_unsigned(tok_text(tok), &v)) {
        g_block->first = (uint16_t)v;
    }
    g_block->cur = g_block->first;
    node_free(tok);
}

static void act_for_end(list_node_t *tok)
{
    unsigned long v;
    substitute_arg(tok);
    if (scan_unsigned(tok_text(tok), &v)) {
        g_block->last = (uint16_t)v;
    }
    g_block->step = (g_block->first < g_block->last) ? 1 : -1;
    node_free(tok);
}

static void act_for_body(list_node_t *tok)
{
    (void)tok;
    g_block->for_pos = ftell(g_source->fp);
}

/* $ENDF */
static void act_endf(list_node_t *tok)
{
    (void)tok;
    if (g_block == NULL || g_block->type != BLOCK_FOR) {
        fatal_error(ERR_SYNTAX, "misplaced ENDF");
    }
    if (!g_block->active) {
        g_block->cur = g_block->last;
    }
    if (g_block->cur == g_block->last) {
        pop_block();
    } else {
        g_block->cur = (uint16_t)(g_block->cur + (unsigned)g_block->step);
        fseek(g_source->fp, g_block->for_pos, SEEK_SET);
    }
}

/* -------------------------------------------------------------------------
 * Actions: $VAR, $SET, $COMP, $OUT
 * ------------------------------------------------------------------------- */

/* $VAR name */
static void act_var_name(list_node_t *tok)
{
    const char *name = tok->u.text;
    var_t *v;
    list_stack_push();
    if (g_vars == NULL) {
        fatal_error(ERR_SYNTAX, "misplaced variable declaration");
    }
    if (find_var(tok_text(tok)) != NULL) {
        fatal_error(ERR_SYNTAX, "variable already defined");
    }
    node_free(tok);
    v = new_var();
    v->name = name;
    v->state = 0;
}

/* The members of the new variable. */
static void act_var_members(list_node_t *tok)
{
    (void)tok;
    if (g_vars_tail == NULL) {
        fatal_error(0x3ed, "parser actions");
    }
    for (list_node_t *n = g_list_top->head; n != NULL; n = n->next) {
        var_t *v = find_var(tok_text(n));
        if (v == NULL) {
            if (n->kind != T_ORDER_TEXT) {
                fatal_error(ERR_UNKNOWN_SYMBOL, tok_text(n));
            }
        } else {
            list_t *members = list_clone(v->members);
            if (members != NULL) {
                if (!(n->flags & 1)) {
                    for (list_node_t *m = members->head; m != NULL; m = m->next) {
                        m->flags ^= 1;
                    }
                }
                list_concat(g_vars_tail->members, members);
                list_free_header(members);
            }
        }
    }
    list_stack_pop();
}

/* Give the ORDER column named by a member the value `ch`. */
static void set_member_value(const list_node_t *member, int ch)
{
    int idx = find_order(tok_text(member));
    int positive, code;
    if (idx < 0) {
        fatal_error(0x3ee, "parser actions");
    }
    positive = order_polarity(idx, member->flags);
    switch (ch) {
    case '*': code = T_VAL_STAR; break;
    case '0': code = invert_value(T_VAL_0, positive); break;
    case '1': code = invert_value(T_VAL_1, positive); break;
    case 'C': code = invert_value(T_VAL_C, positive); break;
    case 'H': code = invert_value(T_VAL_H, positive); break;
    case 'K': code = invert_value(T_VAL_K, positive); break;
    case 'L': code = invert_value(T_VAL_L, positive); break;
    case 'N': code = T_VAL_N; break;
    case 'P': code = T_VAL_P; break;
    case 'R': code = T_VAL_R; break;
    case 'X': code = T_VAL_X; break;
    case 'Z': code = T_VAL_Z; break;
    default: fatal_error(0x3ef, "parser actions");
    }
    g_order[idx].value = (int16_t)code;
}

/* Write the variable's number into its members as test values. */
static void apply_var_value(var_t *v)
{
    int n = v->members->count;
    uint32_t value = v->value;
    const list_node_t *member = v->members->head;
    if (n > VAR_TEXT_SIZE - 1) {
        n = VAR_TEXT_SIZE - 1;
    }
    v->text[n] = '\0';
    for (int i = n - 1; i >= 0; i--) {
        v->text[i] = (char)('0' + (value & 1));
        value /= 2;
    }
    for (char *p = v->text; *p != '\0' && member != NULL; p++, member = member->next) {
        if (g_const_is_levels) {
            *p = (*p == '0') ? 'L' : 'H';
        }
        set_member_value(member, *p);
    }
}

/* Compute the variable's number from the values of its members. */
static void read_var_value(var_t *v)
{
    const list_node_t *member = v->members->head;
    v->value = 0;
    for (int i = 0; i < v->members->count && member != NULL; i++, member = member->next) {
        int idx = find_order(tok_text(member));
        int positive, code;
        if (idx < 0) {
            fatal_error(0x3f0, "parser actions");
        }
        positive = order_polarity(idx, member->flags);
        code = invert_value(g_order[idx].value, positive);
        if (code == T_VAL_STAR) {
            code = invert_value(g_order[idx].result, positive);
        }
        v->value *= 2;
        if (code == T_VAL_H || code == T_VAL_1) {
            v->value += 1;
        } else if (code != T_VAL_L && code != T_VAL_0) {
            return;
        }
    }
    v->state = 2;
}

/* The variable a $SET / $COMP assigns to. */
static void set_target(list_node_t *tok)
{
    substitute_arg(tok);
    g_cur_var = find_var(tok_text(tok));
    if (g_cur_var == NULL) {
        fatal_error(ERR_UNKNOWN_SYMBOL, tok_text(tok));
    }
    g_cur_var->text[0] = '\0';
}

static void act_set_target(list_node_t *tok)
{
    set_target(tok);
    node_free(tok);
}

/* $SET name = 'number' */
static void act_set_number(list_node_t *tok)
{
    if (!block_active()) {
        node_free(tok);
        return;
    }
    g_cur_var->value = (uint32_t)strtoul(tok_text(tok), NULL, radix_of(g_base));
    g_cur_var->state = 2;
    node_free(tok);
    apply_var_value(g_cur_var);
    g_const_is_levels = 0;
    g_have_const = 0;
}

/* $SET name = values */
static void act_set_text(list_node_t *tok)
{
    if (substitute_arg(tok) && (tok->kind == T_QUOTE || tok->kind == T_DQUOTE)) {
        g_have_const = 1;
        if (tok->kind == T_DQUOTE) {
            g_const_is_levels = 1;
        }
        act_set_number(tok);
        return;
    }
    {
        size_t used = strlen(g_cur_var->text);
        snprintf(g_cur_var->text + used, sizeof(g_cur_var->text) - used, "%s", tok_text(tok));
    }
    node_free(tok);
}

/* End of $SET: apply the collected test values. */
static void act_set_end(list_node_t *tok)
{
    char message[80];
    var_t *v = g_cur_var;
    const list_node_t *member;
    if (g_call != NULL && g_have_const) {
        g_have_const = 0;
        g_const_is_levels = 0;
        node_free(tok);
        return;
    }
    if (!block_active()) {
        node_free(tok);
        return;
    }
    if ((int)strlen(v->text) != v->members->count) {
        snprintf(message, sizeof(message), "bad width for constant: %s", v->text);
        fatal_error(ERR_SYNTAX, message);
    }
    member = v->members->head;
    g_const_value = 0;
    v->state = 2;
    for (const char *p = v->text; *p != '\0'; p++) {
        if (strchr("01CKPXNHLZR*", *p) == NULL) {
            snprintf(message, sizeof(message), "bad value in set constant: %c", *p);
            fatal_error(ERR_SYNTAX, message);
        }
        g_const_value *= 2;
        if (*p == '1' || *p == 'H') {
            g_const_value += 1;
        } else if (*p != '0' && *p != 'L') {
            v->state = 1;
        }
        set_member_value(member, *p);
        member = member->next;
    }
    if (v->state == 2) {
        v->value = g_const_value;
    }
    node_free(tok);
}

/* Binding strength of an operator (a lower number binds tighter). */
static int operator_priority(int op)
{
    switch (op) {
    case T_NOT: return 0;
    case T_AND: return 1;
    case T_HASH:
    case T_DOLLAR: return 2;
    case T_STAR:
    case T_SLASH: return 3;
    case T_PLUS:
    case T_MINUS: return 4;
    default: return -1;
    }
}

static void expr_append_right(expr_t *e, expr_t *operand)
{
    while (e->right != NULL) {
        e = e->right;
    }
    e->right = operand;
}

/* Add a number or an operator to the expression being read. */
static void expr_add(uint32_t value, int op)
{
    expr_t *n;
    g_expr_started = 1;
    n = alloc_tracked(sizeof(*n), "allocating operation table");
    n->op = (char)op;
    n->value = value;
    if (g_expr == NULL) {
        g_expr = n;
    } else if (n->op == T_NUMBER) {
        expr_append_right(g_expr, n);
    } else {
        if (!g_after_paren && operator_priority(n->op) < operator_priority(g_expr->op)) {
            /* Binds tighter: take over the right operand */
            n->left = g_expr->right;
            g_expr->right = n;
            return;
        }
        n->left = g_expr;
        g_expr = n;
        g_after_paren = 0;
    }
}

/* Value of an expression tree; arithmetic wraps at 32 bits. */
static uint32_t expr_eval(const expr_t *e)
{
    uint32_t l, r;
    if (e == NULL) {
        return 0;
    }
    switch (e->op) {
    case T_NUMBER:
        return e->value;
    case T_MINUS:
        l = expr_eval(e->left);
        return l - expr_eval(e->right);
    case T_PLUS:
        l = expr_eval(e->left);
        return l + expr_eval(e->right);
    case T_STAR:
        l = expr_eval(e->left);
        return l * expr_eval(e->right);
    case T_SLASH:
        r = expr_eval(e->right);
        if (r == 0) {
            fatal_error(ERR_SYNTAX, "attempt to divide by zero");
        }
        return expr_eval(e->left) / r;
    case T_HASH:
        r = expr_eval(e->right);
        return expr_eval(e->left) | r;
    case T_NOT:
        r = expr_eval(e->right);
        (void)expr_eval(e->left);
        return ~r;
    case T_DOLLAR:
        r = expr_eval(e->right);
        return expr_eval(e->left) ^ r;
    case T_AND:
        r = expr_eval(e->right);
        return expr_eval(e->left) & r;
    default:
        fatal_error(0x3f1, "parser actions");
    }
}

/* $COMP name */
static void act_comp_target(list_node_t *tok)
{
    set_target(tok);
    g_comp_var = g_cur_var;
    node_free(tok);
    g_expr_started = 0;
    g_expr = NULL;
    g_expr_stack = NULL;
    g_after_paren = 0;
    g_strings_mark = g_strings.tail;
}

static void act_comp_number(list_node_t *tok)
{
    int radix = g_have_const ? radix_of(g_base) : 10;
    uint32_t value = (uint32_t)strtoul(tok_text(tok), NULL, radix);
    node_free(tok);
    expr_add(value, T_NUMBER);
}

/* A name in the expression: a $FOR variable or a variable whose
 * current value is taken. */
static void act_comp_operand(list_node_t *tok)
{
    block_t *loop;
    if (substitute_arg(tok)) {
        if (tok->kind == T_NUMBER) {
            act_comp_number(tok);
            return;
        }
        if (tok->kind == T_QUOTE || tok->kind == T_DQUOTE) {
            g_have_const = 1;
            if (tok->kind == T_DQUOTE) {
                g_const_is_levels = 1;
            }
            act_comp_number(tok);
            g_have_const = 0;
            g_const_is_levels = 0;
            return;
        }
    }
    loop = find_loop_var(tok_text(tok));
    if (loop == NULL) {
        set_target(tok);
        read_var_value(g_cur_var);
        if (g_cur_var->state != 2) {
            fatal_error(ERR_SYNTAX, "operands should have a valid integer value");
        }
        node_free(tok);
        expr_add(g_cur_var->value, T_NUMBER);
    } else {
        node_free(tok);
        expr_add(loop->cur, T_NUMBER);
    }
}

static void act_comp_operator(list_node_t *tok)
{
    int op = (int8_t)tok->kind;
    node_free(tok);
    expr_add(0, op);
}

/* '(' */
static void act_comp_lparen(list_node_t *tok)
{
    node_free(tok);
    g_paren_depth++;
    if (g_expr != NULL) {
        expr_save_t *save = alloc_tracked(sizeof(*save), "allocating operation table");
        save->expr = g_expr;
        save->next = g_expr_stack;
        g_expr_stack = save;
        g_expr = NULL;
    }
}

/* ')' */
static void act_comp_rparen(list_node_t *tok)
{
    expr_t *inner;
    node_free(tok);
    if (g_paren_depth == 0) {
        fatal_error(ERR_SYNTAX, "misplaced right parenthesis in expresion");
    }
    inner = g_expr;
    g_paren_depth--;
    /* back to the expression outside the parentheses */
    if (g_expr_stack == NULL) {
        g_expr = NULL;
    } else {
        expr_save_t *save = g_expr_stack;
        g_expr = save->expr;
        g_expr_stack = save->next;
        free_tracked(save);
    }
    if (g_expr == NULL) {
        g_after_paren = 1;
        g_expr = inner;
        return;
    }
    if (inner != NULL) {
        expr_append_right(g_expr, inner);
    }
}

/* End of $COMP: evaluate and store. */
static void act_comp_end(list_node_t *tok)
{
    (void)tok;
    if (block_active()) {
        int idx, kind;
        if (g_expr == NULL) {
            fatal_error(0x3f3, "parser actions");
        }
        if (g_paren_depth > 0) {
            fatal_error(ERR_SYNTAX, "missing right parentheses");
        }
        g_comp_var->value = expr_eval(g_expr);
        g_const_is_levels = 0;
        idx = g_comp_var->members->head != NULL ? find_order(tok_text(g_comp_var->members->head)) : -1;
        if (idx < 0) {
            fatal_error(0x3f4, "parser actions");
        }
        kind = cell_kind(g_order[idx].sym_index);
        if (kind == 3 || kind == 2) {
            g_const_is_levels = 1;
        }
        apply_var_value(g_comp_var);
    }
    if (!g_expr_started) {
        fatal_error(0x3f5, "parser actions");
    }
    release_strings(g_strings_mark);
}

/* $OUT: simulate with the current values. */
static void act_out(list_node_t *tok)
{
    if (!block_active()) {
        return;
    }
    g_value_count = g_order_count;
    act_vector_end(tok);
    for (int i = 0; i < g_order_count; i++) {
        if (g_order[i].value != T_VAL_STAR) {
            g_order[i].value = g_order[i].result;
        }
    }
}

/* -------------------------------------------------------------------------
 * Actions: $MACRO, $CALL, $MEND
 * ------------------------------------------------------------------------- */

/* $MACRO: append a new, unnamed macro to g_macros. */
static void act_macro_begin(list_node_t *tok)
{
    macro_t *m;
    node_free(tok);
    m = alloc_tracked(sizeof(*m), "allocating macro space");
    m->params = alloc_tracked(sizeof(list_t), "allocating macro space");
    if (g_macros == NULL) {
        g_macros = m;
    } else {
        g_macros_tail->next = m;
    }
    g_macros_tail = m;
}

static void act_macro_name(list_node_t *tok)
{
    if (find_macro(tok_text(tok)) != NULL) {
        fatal_error(ERR_SYNTAX, "macro already defined");
    }
    g_macros_tail->name = tok->u.text;
    node_free(tok);
}

static void act_macro_param(list_node_t *tok)
{
    list_append(tok, g_macros_tail->params);
}

/* Remember where the body starts and skip it. */
static void act_macro_body(list_node_t *tok)
{
    g_macros_tail->body_pos = ftell(g_source->fp);
    do {
        node_free(tok);
        tok = next_token();
        if (tok->kind == T_MACRO) {
            fatal_error(ERR_SYNTAX, "misplaced macro definition");
        }
        if (tok->kind == T_EOF) {
            fatal_error(ERR_SYNTAX, "unexpected end of file");
        }
    } while (tok->kind != T_MEND);
    list_append(tok, &g_pushback);
}

/* $CALL name */
static void act_call_name(list_node_t *tok)
{
    macro_t *m = find_macro(tok_text(tok));
    call_t *call;
    if (m == NULL) {
        fatal_error(ERR_SYNTAX, "undefined macro");
    }
    node_free(tok);
    g_strings_mark = g_strings.tail;
    call = alloc_tracked(sizeof(*call), "allocating macro call");
    call->prev = g_call;
    g_call = call;
    call->macro = m;
    g_have_const = 0;
    g_const_is_levels = 0;
}

/* '(' or ',' */
static void act_call_separator(list_node_t *tok)
{
    if (tok->kind == T_COMMA && g_arg_pending) {
        fatal_error(ERR_SYNTAX, "misplaced comma in macro call");
    }
    g_arg_pending = 1;
    node_free(tok);
}

/* An unquoted argument (or a further piece of it). */
static void act_call_text(list_node_t *tok)
{
    tok->kind = T_TEXT;
    if (!g_arg_pending) {
        if (g_call->args.tail != NULL) {
            char *arg = g_call->args.tail->u.text;
            size_t used = strlen(arg);
            snprintf(arg + used, VAR_TEXT_SIZE - used, "%s", tok_text(tok));
        }
        node_free(tok);
    } else {
        char *copy = alloc_tracked(VAR_TEXT_SIZE, "macro call");
        snprintf(copy, VAR_TEXT_SIZE, "%s", tok_text(tok));
        tok->u.text = copy;
        list_append(tok, &g_call->args);
        g_arg_pending = 0;
    }
}

static void act_call_quote(list_node_t *tok)
{
    g_have_const = 1;
    if (tok->kind == T_DQUOTE) {
        g_const_is_levels = 1;
    }
    node_free(tok);
}

/* A quoted argument */
static void act_call_const(list_node_t *tok)
{
    if (g_have_const) {
        tok->kind = g_const_is_levels ? T_DQUOTE : T_QUOTE;
        g_have_const = 0;
        g_const_is_levels = 0;
    }
    list_append(tok, &g_call->args);
    g_arg_pending = 0;
}

/* ')' */
static void act_call_rparen(list_node_t *tok)
{
    if (g_call->macro->params->count != g_call->args.count) {
        fatal_error(ERR_SYNTAX, "wrong number of arguments in macro call");
    }
    node_free(tok);
}

/* An argument that names a parameter of an enclosing call stands
 * for that call's argument. */
static void resolve_outer_arg(list_node_t *arg)
{
    call_t *saved = g_call;
    for (g_call = g_call->prev; g_call != NULL; g_call = g_call->prev) {
        list_node_t *outer = macro_arg(tok_text(arg));
        if (outer == NULL) {
            break;
        }
        arg->u.text = outer->u.text;
        arg->kind = outer->kind;
    }
    g_call = saved;
}

/* End of $CALL: continue reading at the macro body. */
static void act_call_end(list_node_t *tok)
{
    (void)tok;
    if (!block_active()) {
        return;
    }
    /* digit-only arguments are numbers; others may name an outer argument */
    for (list_node_t *arg = g_call->args.head; arg != NULL; arg = arg->next) {
        if (arg->kind == T_TEXT) {
            const char *p = tok_text(arg);
            while (is_digit(*p)) {
                p++;
            }
            if (*p == '\0') {
                arg->kind = T_NUMBER;
            } else {
                resolve_outer_arg(arg);
            }
        }
    }
    g_call->return_pos = ftell(g_source->fp);
    fseek(g_source->fp, g_call->macro->body_pos, SEEK_SET);
}

/* $MEND reached while running a call: return. */
static void act_mend(list_node_t *tok)
{
    (void)tok;
    if (g_call == NULL) {
        fatal_error(0x3f8, "parser actions");
    }
    fseek(g_source->fp, g_call->return_pos, SEEK_SET);
    g_call = g_call->prev;
    release_strings(g_strings_mark);
}

/* -------------------------------------------------------------------------
 * Grammar
 * ------------------------------------------------------------------------- */
static const grammar_t k_grammar[GRAMMAR_ENTRIES] = {
    /* 0: statement */
    { 0, 1, 0, OP_TRY, NULL },
    { T_MSG, 1, 157, OP_CALL, NULL },
    { T_NAME, 21, 0, OP_TRY, act_header_item },
    { T_PARTNO, 21, 0, OP_TRY, act_header_item },
    { T_REV, 21, 0, OP_TRY, act_header_item },
    { T_DATE, 21, 0, OP_TRY, act_header_item },
    { T_DESIGNER, 21, 0, OP_TRY, act_header_item },
    { T_COMPANY, 21, 0, OP_TRY, act_header_item },
    { T_ASSY, 21, 0, OP_TRY, act_header_item },
    { T_LOCATION, 21, 0, OP_TRY, act_header_item },
    { T_DEVICE, 21, 0, OP_TRY, act_header_item },
    { T_FORMAT, 21, 0, OP_TRY, act_header_item },
    { T_EXIT, 1, 161, OP_CALL, NULL },
    { T_ORDER, 1, 34, OP_CALL, NULL },
    { T_BASE, 1, 50, OP_CALL, NULL },
    { T_VECTORS, 1, 107, OP_CALL, NULL },
    { T_FIELD, 1, 192, OP_CALL, NULL },
    { T_STUCKH, 1, 206, OP_CALL, NULL },
    { T_VAR, 1, 249, OP_CALL, NULL },
    { T_EOF, 1, 0, OP_LAST, NULL },
    { 0, 1, 0, OP_ERROR, NULL },
    { T_SEMI, 1, 0, OP_RESYNC, act_header_text },
    /* 22: $MACRO name(params); ... $MEND; */
    { 0, 1, 22, OP_RULE, NULL },
    { T_MACRO, 2, 0, OP_FIRST, act_macro_begin },
    { T_IDENT, 4, 0, OP_TRY, act_macro_name },
    { T_IDENT_NUM, 4, 0, OP_MUST, act_macro_name },
    { T_LPAREN, 5, 0, OP_MUST, NULL },
    { T_RPAREN, 9, 0, OP_TRY, NULL },
    { T_IDENT, 7, 0, OP_MUST, act_macro_param },
    { T_COMMA, 6, 0, OP_TRY, NULL },
    { T_RPAREN, 9, 0, OP_MUST, NULL },
    { T_SEMI, 10, 0, OP_MUST, act_macro_body },
    { T_MEND, 11, 0, OP_MUST, NULL },
    { T_SEMI, 0, 0, OP_LAST, NULL },
    /* 34: ORDER: list; */
    { 0, 1, 34, OP_RULE, NULL },
    { T_ORDER, 2, 0, OP_FIRST, act_order_begin },
    { T_COLON, 3, 0, OP_EXPECT, NULL },
    { T_LIST, 4, 138, OP_MUST_CALL, act_list_to_pushback },
    { T_IDENT_NUM, 4, 0, OP_TRY, act_order_name },
    { T_IDENT, 4, 0, OP_TRY, act_order_name },
    { T_ORDER_TEXT, 4, 0, OP_TRY, act_order_text },
    { T_ORDER_SPACE, 4, 0, OP_TRY, act_order_text },
    { T_SEMI, 0, 0, OP_LAST, act_order_end },
    /* 43: PRELOAD */
    { 0, 1, 43, OP_RULE, NULL },
    { T_PRELOAD, 2, 0, OP_FIRST, NULL },
    { T_SEMI, 0, 0, OP_LAST, NULL },
    { T_LIST, 3, 138, OP_MUST_CALL, NULL },
    { T_EQUAL, 4, 0, OP_MUST, NULL },
    { T_TEXT, 0, 0, OP_MUST, NULL },
    { T_SEMI, 0, 0, OP_LAST, NULL },
    /* 50: BASE: radix; */
    { 0, 1, 50, OP_RULE, NULL },
    { T_BASE, 2, 0, OP_FIRST, NULL },
    { T_COLON, 3, 0, OP_EXPECT, NULL },
    { T_HEX, 7, 0, OP_TRY, act_base },
    { T_OCTAL, 7, 0, OP_TRY, act_base },
    { T_DECIMAL, 7, 0, OP_TRY, act_base },
    { T_BINARY, 7, 0, OP_MUST, act_base },
    { T_SEMI, 0, 0, OP_LAST, NULL },
    /* 58: $FOR var = first..last: */
    { 0, 1, 58, OP_RULE, NULL },
    { T_FOR, 2, 0, OP_FIRST, act_for_begin },
    { T_IDENT, 3, 0, OP_MUST, act_for_var },
    { T_EQUAL, 4, 0, OP_EXPECT, NULL },
    { T_TEXT, 5, 0, OP_MUST, act_for_start },
    { T_DOT, 6, 0, OP_MUST, NULL },
    { T_DOT, 7, 0, OP_MUST, NULL },
    { T_TEXT, 8, 0, OP_MUST, act_for_end },
    { T_COLON, 0, 0, OP_LAST, act_for_body },
    /* 67: $WHILE condition: */
    { 0, 1, 67, OP_RULE, NULL },
    { T_WHILE, 2, 0, OP_FIRST, act_while_begin },
    { T_LIST, 3, 138, OP_MUST_CALL, act_list_to_pushback },
    { T_IDENT_NUM, 3, 0, OP_TRY, act_cond_operand },
    { T_IDENT, 3, 0, OP_TRY, act_cond_operand },
    { T_GREATER, 9, 0, OP_TRY, act_relation },
    { T_LESS, 9, 0, OP_TRY, act_relation },
    { T_HASH, 9, 0, OP_TRY, act_relation },
    { T_EQUAL, 10, 0, OP_MUST, act_relation },
    { T_EQUAL, 10, 0, OP_TRY, act_relation },
    { T_QUOTE, 13, 0, OP_TRY, act_const_quote },
    { T_DQUOTE, 15, 0, OP_TRY, act_const_quote },
    { T_TEXT, 17, 0, OP_MUST, act_cond_text },
    { T_TEXT, 14, 0, OP_EXPECT, act_cond_number },
    { T_QUOTE, 17, 0, OP_MUST, act_free_token },
    { T_TEXT, 16, 0, OP_EXPECT, act_cond_number },
    { T_DQUOTE, 17, 0, OP_MUST, act_free_token },
    { T_COLON, 0, 0, OP_LAST, act_while_cond },
    /* 85: $IF condition: */
    { 0, 1, 85, OP_RULE, NULL },
    { T_IF, 2, 0, OP_FIRST, act_if_begin },
    { T_LIST, 3, 138, OP_MUST_CALL, act_list_to_pushback },
    { T_IDENT_NUM, 3, 0, OP_TRY, act_cond_operand },
    { T_IDENT, 3, 0, OP_TRY, act_cond_operand },
    { T_GREATER, 9, 0, OP_TRY, act_relation },
    { T_LESS, 9, 0, OP_TRY, act_relation },
    { T_HASH, 9, 0, OP_TRY, act_relation },
    { T_EQUAL, 10, 0, OP_MUST, act_relation },
    { T_EQUAL, 10, 0, OP_TRY, act_relation },
    { T_QUOTE, 13, 0, OP_TRY, act_const_quote },
    { T_DQUOTE, 15, 0, OP_TRY, act_const_quote },
    { T_TEXT, 17, 0, OP_MUST, act_cond_text },
    { T_TEXT, 14, 0, OP_EXPECT, act_cond_number },
    { T_QUOTE, 17, 0, OP_MUST, act_free_token },
    { T_TEXT, 16, 0, OP_EXPECT, act_cond_number },
    { T_DQUOTE, 17, 0, OP_MUST, act_free_token },
    { T_COLON, 0, 0, OP_LAST, act_if_cond },
    /* 103: $REPEAT count; */
    { 0, 1, 103, OP_RULE, NULL },
    { T_REPEAT, 2, 0, OP_FIRST, NULL },
    { T_NUMBER, 3, 0, OP_MUST, act_repeat_count },
    { T_SEMI, 0, 0, OP_LAST, NULL },
    /* 107: VECTORS: and the test vectors */
    { 0, 1, 107, OP_RULE, NULL },
    { T_VECTORS, 2, 0, OP_FIRST, NULL },
    { T_COLON, 3, 0, OP_MUST, act_vectors_begin },
    { T_BLANK, 3, 0, OP_TRY, NULL },
    { T_VAL_X, 3, 0, OP_TRY, act_value },
    { T_VAL_0, 3, 0, OP_TRY, act_value },
    { T_VAL_1, 3, 0, OP_TRY, act_value },
    { T_VAL_H, 3, 0, OP_TRY, act_value },
    { T_VAL_L, 3, 0, OP_TRY, act_value },
    { T_VAL_Z, 3, 0, OP_TRY, act_value },
    { T_VAL_C, 3, 0, OP_TRY, act_value },
    { T_VAL_P, 3, 0, OP_TRY, act_value },
    { T_VAL_N, 3, 0, OP_TRY, act_value },
    { T_VAL_K, 3, 0, OP_TRY, act_value },
    { T_VAL_STAR, 3, 0, OP_TRY, act_value },
    { T_VAL_R, 3, 0, OP_TRY, act_value },
    { T_EOL, 3, 0, OP_TRY, act_vector_end },
    { T_DOLLAR, 22, 0, OP_TRY, act_directive_begin },
    { T_QUOTE, 26, 0, OP_TRY, act_quote_begin },
    { T_DQUOTE, 28, 0, OP_TRY, act_quote_begin },
    { T_CHAR, 21, 0, OP_TRY, act_unget_char },
    { 0, 0, 0, OP_PUSHBACK, NULL },
    { 0, 23, 165, OP_CALL, NULL },
    { T_SEMI, 25, 0, OP_TRY, NULL },
    { T_COLON, 25, 0, OP_TRY, NULL },
    { T_EOL, 3, 0, OP_TRY, act_pop_mode },
    { T_TEXT, 27, 0, OP_RESYNC, act_quoted_number },
    { T_QUOTE, 3, 0, OP_RESYNC, act_quote_end },
    { T_TEXT, 30, 0, OP_TRY, act_quoted_number },
    { T_STAR, 29, 0, OP_TRY, act_quoted_star },
    { T_DQUOTE, 3, 0, OP_RESYNC, act_quote_end },
    /* 138: list of names, texts and spacings */
    { 0, 1, 138, OP_RULE, act_list_begin },
    { T_NOT, 2, 0, OP_TRY, act_negate },
    { T_IDENT_NUM, 9, 0, OP_TRY, act_list_numbered_name },
    { T_IDENT, 10, 0, OP_TRY, act_list_name },
    { T_NUMBER, 10, 0, OP_TRY, act_list_name },
    { T_DQUOTE, 17, 0, OP_TRY, act_text_begin },
    { T_PERCENT, 18, 0, OP_TRY, NULL },
    { T_SEMI, 18, 0, OP_PUSHBACK, NULL },
    { 0, 1, 0, OP_ERROR, NULL },
    { T_DOT, 16, 0, OP_TRY, NULL },
    { T_COMMA, 1, 0, OP_TRY, NULL },
    { 0, 10, 201, OP_CALL, NULL },
    { 0, 0, 0, OP_PUSHBACK, NULL },
    { T_NOT, 14, 0, OP_TRY, act_negate },
    { T_NUMBER, 10, 0, OP_TRY, act_list_numbered_name },
    { T_IDENT_NUM, 10, 0, OP_MUST, act_list_numbered_name },
    { T_DOT, 13, 0, OP_MUST, act_range },
    { T_SEMI, 10, 0, OP_MUST, act_list_text },
    { T_NUMBER, 10, 0, OP_MUST, act_list_spacing },
    /* 157: $MSG list; */
    { 0, 1, 157, OP_RULE, NULL },
    { T_MSG, 2, 0, OP_FIRST, NULL },
    { T_LIST, 3, 138, OP_MUST_CALL, act_msg_end },
    { T_SEMI, 0, 0, OP_LAST, NULL },
    /* 161: $EXIT list; */
    { 0, 1, 161, OP_RULE, NULL },
    { T_EXIT, 2, 0, OP_FIRST, NULL },
    { T_LIST, 3, 138, OP_MUST_CALL, NULL },
    { T_SEMI, 0, 0, OP_LAST, act_exit_end },
    /* 165: directive after $ */
    { 0, 1, 165, OP_RULE, NULL },
    { 0, 26, 300, OP_CALL, NULL },
    { 0, 26, 256, OP_CALL, NULL },
    { 0, 26, 271, OP_CALL, NULL },
    { 0, 26, 297, OP_CALL, NULL },
    { 0, 26, 58, OP_CALL, NULL },
    { 0, 26, 294, OP_CALL, NULL },
    { 0, 26, 67, OP_CALL, NULL },
    { 0, 26, 290, OP_CALL, NULL },
    { 0, 26, 85, OP_CALL, NULL },
    { 0, 26, 222, OP_CALL, NULL },
    { 0, 26, 225, OP_CALL, NULL },
    { 0, 26, 228, OP_CALL, NULL },
    { 0, 26, 231, OP_CALL, NULL },
    { 0, 26, 103, OP_CALL, NULL },
    { 0, 26, 22, OP_CALL, NULL },
    { 0, 26, 319, OP_CALL, NULL },
    { 0, 26, 157, OP_CALL, NULL },
    { 0, 26, 161, OP_CALL, NULL },
    { 0, 26, 212, OP_CALL, NULL },
    { 0, 26, 216, OP_CALL, NULL },
    { 0, 26, 219, OP_CALL, NULL },
    { 0, 26, 34, OP_CALL, NULL },
    { 0, 26, 50, OP_CALL, NULL },
    { 0, 26, 107, OP_CALL, NULL },
    { 0, 26, 43, OP_CALL, NULL },
    { 0, 0, 0, OP_PUSHBACK, NULL },
    /* 192: FIELD name = [list]; */
    { 0, 1, 192, OP_RULE, NULL },
    { T_FIELD, 2, 0, OP_FIRST, NULL },
    { T_IDENT, 3, 0, OP_MUST, act_field_name },
    { T_EQUAL, 4, 0, OP_EXPECT, NULL },
    { T_NOT, 5, 0, OP_TRY, act_field_negate },
    { T_LBRACKET, 6, 0, OP_EXPECT, NULL },
    { T_LIST, 7, 138, OP_MUST_CALL, act_field_end },
    { T_RBRACKET, 8, 0, OP_EXPECT, act_field_bracket },
    { T_SEMI, 0, 0, OP_LAST, NULL },
    /* 201: ( list ) */
    { 0, 1, 201, OP_RULE, NULL },
    { T_LPAREN, 2, 0, OP_FIRST, NULL },
    { 0, 3, 138, OP_CALL, NULL },
    { T_RPAREN, 4, 0, OP_EXPECT, NULL },
    { 0, 0, 0, OP_PUSHBACK, NULL },
    /* 206: STUCKH / STUCKL id; */
    { 0, 1, 206, OP_RULE, NULL },
    { T_STUCKL, 3, 0, OP_TRY, NULL },
    { T_STUCKH, 4, 0, OP_FIRST, NULL },
    { T_NUMBER, 5, 0, OP_EXPECT, act_stuck_low },
    { T_NUMBER, 5, 0, OP_EXPECT, act_stuck_high },
    { T_SEMI, 0, 0, OP_LAST, NULL },
    /* 212: $TRACE level; */
    { 0, 1, 212, OP_RULE, NULL },
    { T_TRACE, 2, 0, OP_FIRST, NULL },
    { T_NUMBER, 3, 0, OP_EXPECT, act_trace },
    { T_SEMI, 0, 0, OP_LAST, NULL },
    /* 216: $SIMOFF; */
    { 0, 1, 216, OP_RULE, NULL },
    { T_SIMOFF, 2, 0, OP_FIRST, act_simoff },
    { T_SEMI, 0, 0, OP_LAST, NULL },
    /* 219: $SIMON; */
    { 0, 1, 219, OP_RULE, NULL },
    { T_SIMON, 2, 0, OP_FIRST, act_simon },
    { T_SEMI, 0, 0, OP_LAST, NULL },
    /* 222: $ELSE: */
    { 0, 1, 222, OP_RULE, NULL },
    { T_ELSE, 2, 0, OP_FIRST, act_else },
    { T_COLON, 0, 0, OP_LAST, NULL },
    /* 225: $ENDIF; */
    { 0, 1, 225, OP_RULE, NULL },
    { T_ENDIF, 2, 0, OP_FIRST, act_endif },
    { T_SEMI, 0, 0, OP_LAST, NULL },
    /* 228: $DO: */
    { 0, 1, 228, OP_RULE, NULL },
    { T_DO, 2, 0, OP_FIRST, NULL },
    { T_COLON, 0, 0, OP_LAST, act_do },
    /* 231: $UNTIL condition; */
    { 0, 1, 231, OP_RULE, NULL },
    { T_UNTIL, 2, 0, OP_FIRST, act_until_begin },
    { T_LIST, 3, 138, OP_MUST_CALL, act_list_to_pushback },
    { T_IDENT_NUM, 3, 0, OP_TRY, act_cond_operand },
    { T_IDENT, 3, 0, OP_TRY, act_cond_operand },
    { T_GREATER, 9, 0, OP_TRY, act_relation },
    { T_LESS, 9, 0, OP_TRY, act_relation },
    { T_HASH, 9, 0, OP_TRY, act_relation },
    { T_EQUAL, 10, 0, OP_MUST, act_relation },
    { T_EQUAL, 10, 0, OP_TRY, act_relation },
    { T_QUOTE, 13, 0, OP_TRY, act_const_quote },
    { T_DQUOTE, 15, 0, OP_TRY, act_const_quote },
    { T_TEXT, 17, 0, OP_MUST, act_cond_text },
    { T_TEXT, 14, 0, OP_EXPECT, act_cond_number },
    { T_QUOTE, 17, 0, OP_MUST, act_free_token },
    { T_TEXT, 16, 0, OP_EXPECT, act_cond_number },
    { T_DQUOTE, 17, 0, OP_MUST, act_free_token },
    { T_SEMI, 0, 0, OP_LAST, act_until_end },
    /* 249: $VAR name = list; */
    { 0, 1, 249, OP_RULE, NULL },
    { T_VAR, 2, 0, OP_FIRST, NULL },
    { T_IDENT, 4, 0, OP_TRY, act_var_name },
    { T_IDENT_NUM, 4, 0, OP_MUST, act_var_name },
    { T_EQUAL, 5, 0, OP_EXPECT, NULL },
    { T_LIST, 6, 138, OP_MUST_CALL, act_var_members },
    { T_SEMI, 0, 0, OP_LAST, NULL },
    /* 256: $SET name = value; */
    { 0, 1, 256, OP_RULE, NULL },
    { T_SET, 2, 0, OP_FIRST, NULL },
    { T_IDENT, 4, 0, OP_TRY, act_set_target },
    { T_IDENT_NUM, 4, 0, OP_MUST, act_set_target },
    { T_EQUAL, 5, 0, OP_MUST, NULL },
    { T_QUOTE, 10, 0, OP_TRY, act_const_quote },
    { T_DQUOTE, 12, 0, OP_TRY, act_const_quote },
    { T_STAR, 7, 0, OP_TRY, act_set_text },
    { T_TEXT, 7, 0, OP_TRY, act_set_text },
    { T_SEMI, 0, 0, OP_LAST, act_set_end },
    { T_TEXT, 11, 0, OP_EXPECT, act_set_number },
    { T_QUOTE, 14, 0, OP_MUST, NULL },
    { T_TEXT, 13, 0, OP_EXPECT, act_set_number },
    { T_DQUOTE, 14, 0, OP_MUST, NULL },
    { T_SEMI, 0, 0, OP_LAST, NULL },
    /* 271: $COMP name = expression; */
    { 0, 1, 271, OP_RULE, NULL },
    { T_COMP, 2, 0, OP_FIRST, NULL },
    { T_IDENT, 4, 0, OP_TRY, act_comp_target },
    { T_IDENT_NUM, 4, 0, OP_FIRST, act_comp_target },
    { T_EQUAL, 5, 0, OP_MUST, NULL },
    { T_NOT, 6, 0, OP_TRY, act_comp_operator },
    { T_IDENT, 10, 0, OP_TRY, act_comp_operand },
    { T_IDENT_NUM, 10, 0, OP_TRY, act_comp_operand },
    { T_NUMBER, 10, 0, OP_TRY, act_comp_number },
    { T_LPAREN, 5, 0, OP_MUST, act_comp_lparen },
    { T_RPAREN, 11, 0, OP_TRY, act_comp_rparen },
    { T_PLUS, 5, 0, OP_TRY, act_comp_operator },
    { T_MINUS, 5, 0, OP_TRY, act_comp_operator },
    { T_STAR, 5, 0, OP_TRY, act_comp_operator },
    { T_SLASH, 5, 0, OP_TRY, act_comp_operator },
    { T_HASH, 5, 0, OP_TRY, act_comp_operator },
    { T_DOLLAR, 5, 0, OP_TRY, act_comp_operator },
    { T_AND, 5, 0, OP_TRY, act_comp_operator },
    { T_SEMI, 0, 0, OP_LAST, act_comp_end },
    /* 290: $ENDW */
    { 0, 1, 290, OP_RULE, NULL },
    { T_ENDW, 2, 0, OP_FIRST, NULL },
    { T_SEMI, 3, 0, OP_MUST, NULL },
    { T_EOL, 0, 0, OP_LAST, act_endw },
    /* 294: $ENDF; */
    { 0, 1, 294, OP_RULE, NULL },
    { T_ENDF, 2, 0, OP_FIRST, NULL },
    { T_SEMI, 0, 0, OP_LAST, act_endf },
    /* 297: $OUT; */
    { 0, 1, 297, OP_RULE, NULL },
    { T_OUT, 2, 0, OP_FIRST, act_out },
    { T_SEMI, 0, 0, OP_LAST, NULL },
    /* 300: $CALL name(args); */
    { 0, 1, 300, OP_RULE, NULL },
    { T_CALL, 2, 0, OP_FIRST, NULL },
    { T_IDENT, 4, 0, OP_TRY, act_call_name },
    { T_IDENT_NUM, 4, 0, OP_MUST, act_call_name },
    { T_LPAREN, 5, 0, OP_MUST, act_call_separator },
    { T_RPAREN, 18, 0, OP_TRY, act_call_rparen },
    { T_QUOTE, 12, 0, OP_TRY, act_call_quote },
    { T_DQUOTE, 14, 0, OP_TRY, act_call_quote },
    { T_STAR, 8, 0, OP_TRY, act_call_text },
    { T_TEXT, 8, 0, OP_TRY, act_call_text },
    { T_COMMA, 6, 0, OP_TRY, act_call_separator },
    { T_RPAREN, 18, 0, OP_MUST, act_call_rparen },
    { T_TEXT, 13, 0, OP_EXPECT, act_call_const },
    { T_QUOTE, 16, 0, OP_MUST, NULL },
    { T_TEXT, 15, 0, OP_EXPECT, act_call_const },
    { T_DQUOTE, 16, 0, OP_MUST, NULL },
    { T_COMMA, 6, 0, OP_TRY, act_call_separator },
    { T_RPAREN, 18, 0, OP_MUST, act_call_rparen },
    { T_SEMI, 0, 0, OP_LAST, act_call_end },
    /* 319: $MEND; */
    { 0, 1, 319, OP_RULE, NULL },
    { T_MEND, 2, 0, OP_FIRST, NULL },
    { T_SEMI, 0, 0, OP_LAST, act_mend },
};

/* -------------------------------------------------------------------------
 * Entry points
 * ------------------------------------------------------------------------- */

/* Initial state of the parser's globals. */
void parse_reset(void)
{
    memset(g_modes, 0, sizeof(g_modes));
    g_mode_top = &g_modes[0];
    g_tok_text[0] = '\0';
    g_want_eol = 0;
    list_reset(&g_pushback);
    g_stop_parsing = 0;
    memset(g_phantom, 0, sizeof(g_phantom));
    memset(g_phantom_text, 0, sizeof(g_phantom_text));
    g_phantom[0].u.text = g_phantom_text[0];
    g_phantom[1].u.text = g_phantom_text[1];
    memset(g_blocks, 0, sizeof(g_blocks));
    g_block = NULL;
    memset(&g_null_block, 0, sizeof(g_null_block));
    g_block_depth = 0;
    g_relation = 0;
    g_const_value = 0;
    g_have_const = 0;
    g_const_is_levels = 0;
    g_cond_loop = NULL;
    memset(g_cond_values, 0, sizeof(g_cond_values));
    g_vars = g_vars_tail = g_cur_var = g_comp_var = NULL;
    g_expr = NULL;
    g_expr_started = 0;
    g_expr_stack = NULL;
    g_paren_depth = 0;
    g_after_paren = 0;
    g_macros = g_macros_tail = NULL;
    g_call = NULL;
    g_arg_pending = 0;
    g_strings_order_mark = NULL;
    g_strings_order_count = 0;
    g_strings_mark = NULL;
    memset(g_header_lines, 0, sizeof(g_header_lines));
    g_first_order = 1;
    g_header_seen = 0;
    g_header_kind = 0;
    g_repeat_kind = 0;
    g_repeat_active = 0;
    g_repeat_step = g_repeat_done = g_repeat_count = 0;
    g_listing_off = 0;
    g_vector_running = 0;
    g_quote_text[0] = '\0';
    g_range_stem = NULL;
    g_range_first = 0;
    g_cur_field = NULL;
    g_order = NULL;
    g_order_count = 0;
    g_base = T_HEX;
    g_trace_level = 0;
    g_value_count = 0;
    g_quote_kind = 0;
    g_vector_number = 0;
    g_field_positive = 1;
    g_positive = 1;
    g_in_range = 0;
    g_sim_on = 1;
}

/* Read and execute <design>.si */
void parse_simulation_file(void)
{
    const char *name;
    g_line_ptr = NULL;
    g_order = alloc_tracked(((size_t)g_nsyms + 2) * sizeof(order_t), "initialization");
    name = make_file_name(g_dir_prefix, g_base_name, ".si");
    *g_mode_top = &k_mode_normal;
    open_source(name);
    g_line_pending = 1;
    g_stop_parsing = 0;
    parse_rule(&k_grammar[0], next_token());
    close_file(g_source->fp);
    g_source->fp = NULL;
}
