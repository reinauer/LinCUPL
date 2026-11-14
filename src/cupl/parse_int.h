/*
 * parse - declarations shared by the files of the CUPL parser phase
 * (parse.c, parse_*.c): tokens and lists, symbols and symbol tables, error
 * codes, limits, and the state and functions one file uses of another.
 */
#ifndef LINCUPL_PARSE_INT_H
#define LINCUPL_PARSE_INT_H

#include <stdint.h>
#include <stdio.h>
#include "ascii.h"

/* -------------------------------------------------------------------------
 * Tokens
 * ------------------------------------------------------------------------- */
enum {
    /* 0x00..0x4f: keywords and operators, see k_keywords */
    TOK_NOT = 0x00,
    TOK_OR = 0x02,
    TOK_XOR = 0x03,
    TOK_AND = 0x04,
    TOK_EQEQ = 0x05,
    TOK_QUOTE = 0x06,
    TOK_LPAREN = 0x07,
    TOK_RPAREN = 0x08,
    TOK_COMMA = 0x09,
    TOK_DOT = 0x0a,
    TOK_ESQ = 0x0d,          /* ".esq": end of a generated sequence */
    TOK_FDEL = 0x0e,         /* ".fdel": end of a generated IF block */
    TOK_FINI = 0x10,         /* ".fini": end of the last table in CB */
    TOK_NSQ = 0x11,          /* ".nsq": next state of a generated sequence */
    TOK_ODEL = 0x12,         /* ".odel": start of a generated IF condition */
    TOK_PEND = 0x14,         /* ".pend": end of an equation in CB */
    TOK_XEND = 0x15,         /* ".xend": end of a table in CB */
    TOK_COLON = 0x16,
    TOK_SEMI = 0x17,
    TOK_ASSIGN = 0x18,
    TOK_LBRACKET = 0x1a,
    TOK_RBRACKET = 0x1b,
    TOK_APPEND = 0x1c,
    TOK_ASSEMBLY = 0x1d,
    TOK_ASSY = 0x1e,
    TOK_RADIX_B = 0x1f,
    TOK_RADIX_D = 0x22,
    TOK_GLOBAL = 0x2f,
    TOK_RADIX_H = 0x32,
    TOK_LOC = 0x35,
    TOK_LOCATION = 0x36,
    TOK_NAME = 0x39,
    TOK_RADIX_O = 0x3d,
    TOK_REV = 0x43,
    TOK_REVISION = 0x45,
    TOK_LBRACE = 0x4e,
    TOK_RBRACE = 0x4f,

    TOK_IDENT = 0x51,        /* identifier */
    TOK_NUMBER = 0x52,       /* digits only */
    TOK_IDENT_NUM = 0x53,    /* identifier that ends in a digit */
    TOK_BAD = 0x54,
    TOK_UNKNOWN = 0x55,
    TOK_PUNCT = 0x56,        /* any other character, looked up in k_punctuation */
    TOK_ANY = 0x5a,          /* grammar only: any token but end of file */
    TOK_WORD = 0x5d,         /* grammar only: identifier or number */
    TOK_EXTENSION = 0x5e,    /* the name after '.', kept behind its variable */
    TOK_TEXT = 0x5f,         /* header text up to ';' */
    TOK_RANGE_FROM = 0x60,   /* first number of n..m in a value list */
    TOK_RANGE_TO = 0x61,
    TOK_LIST_NUMBER = 0x62,  /* number in a value list */
    TOK_EOL = 0x65,
    TOK_EOF = 0x66,
    TOK_GROUP = 0x67         /* grammar only: group header / sub-group call */
};

/* token flags */
#define TF_NEGATED 0x01
#define TF_SYMBOL 0x02       /* value is a symbol */
#define TF_NUMBER 0x10       /* value is a number (or, in expressions, a bit) */

typedef struct symbol symbol_t;

typedef struct {
    uint32_t value;
    uint32_t dontcare;       /* mask of don't-care bits */
} number_t;

/* A token: type, flags, a value that is a string, a symbol, a number or a
 * plain 0/1 depending on context, and the link. */
typedef struct token {
    uint8_t type;
    uint8_t flags;
    union {
        const char *text;
        symbol_t *sym;
        number_t *num;
        uintptr_t bit;
        void *any;
    } u;
    struct token *next;
} token_t;

typedef struct list {
    token_t *head;
    token_t *tail;
    int16_t count;
    uint16_t flags;          /* bit 0: bracket was not negated */
    struct list *next_free;
} list_t;

/* symbol types */
enum {
    SYM_FIELD = 1,
    SYM_PIN = 2,
    SYM_VARIABLE = 3,
    SYM_PIN_EXT = 5,         /* pin.ext */
    SYM_NODE = 9,
    SYM_NODE_EXT = 10,
    SYM_FUNCTION = 11,
    SYM_GROUP = 14
};

/* symbol flags */
#define SF_MIN_MASK 0x07     /* minimisation level */
#define SF_MIN_SET 0x08      /* level given with MIN */
#define SF_PIN_NEGATED 0x10  /* declared as !name */
#define SF_LHS_NEGATED 0x20  /* assigned as !name = ... */
#define SF_ASSIGNED 0x40
#define SF_USED 0x80

struct symbol {
    const char *name;
    const char *ext;
    int16_t pin;
    uint8_t type;
    uint8_t flags;
    uint16_t ord;            /* number in the CB file */
    uint8_t demorgan;        /* DEMORGAN level, 0x80 = given */
    uint16_t regtype;        /* extension code << 8 | REGISTER_SELECT */
    list_t *expr;            /* expression assigned to the variable, or the
                              * members of a FIELD/GROUP, or a function body */
    symbol_t *left;          /* other extensions of the same name */
    symbol_t *right;
    symbol_t *next;          /* hash chain of names */
};

#define HASH_SIZE 97

typedef struct {
    FILE *fp;
    uint8_t *header;         /* 199 bytes */
    uint8_t *entry;          /* directory entry of the device, 26 bytes */
    int have_entry;
    uint8_t *data;           /* device description */
    size_t data_size;
} device_t;

/* fields of the architecture block of a device (parse_dev_arch_byte) */
#define ARCH_PIN_COUNT 0x08
#define ARCH_CLASS 0x11          /* 8 and 9: virtual device */
#define ARCH_SEQ_NEXT_1 0x33
#define ARCH_SEQ_NEXT_2 0x34
#define ARCH_SEQ_NEXT_COUNT 0x35
#define ARCH_SEQ_INPUT_1 0x36
#define ARCH_SEQ_INPUT_2 0x37
#define ARCH_SEQ_INPUT_COUNT 0x38

typedef struct symtab {
    const char *name;        /* NAME header item */
    device_t *device;
    int16_t index;
    const char *format;      /* FORMAT header item */
    int16_t nsyms;
    symbol_t *hash[HASH_SIZE];
    list_t header;           /* header items, token type = keyword */
    struct symtab *next;
} symtab_t;

typedef struct {
    const char *name;
    int id;
} keyword_t;

typedef int (*action_fn)(token_t *tok);

/* the semantic actions, called by the parser as the grammar rules match
 * (parse_actions.c, parse_lower.c, parse_sequence.c) */
#define ACTION(name) int name(token_t *tok)
ACTION(act_header_keyword); ACTION(act_header_text); ACTION(act_header_name);
ACTION(act_header_device); ACTION(act_header_format); ACTION(act_modifier);
ACTION(act_assign_targets); ACTION(act_list_mode_off); ACTION(act_statement_no_expr);
ACTION(act_statement_end); ACTION(act_flush_call_results); ACTION(act_field_name);
ACTION(act_field_end); ACTION(act_pin_keyword); ACTION(act_work_pop);
ACTION(act_pin_declare); ACTION(act_node_declare); ACTION(act_pinnode_declare);
ACTION(act_sequence_begin); ACTION(act_sequenced_begin); ACTION(act_sequencejk_begin);
ACTION(act_sequencers_begin); ACTION(act_sequencet_begin); ACTION(act_sequence_vars);
ACTION(act_sequence_end); ACTION(act_list_mode_on); ACTION(act_if_begin);
ACTION(act_next_unconditional); ACTION(act_next_default); ACTION(act_out_unconditional);
ACTION(act_present_state); ACTION(act_next_conditional); ACTION(act_out_conditional);
ACTION(act_next_default_cond); ACTION(act_next_state); ACTION(act_transition_end);
ACTION(act_out_default_cond); ACTION(act_out_vars); ACTION(act_out_default);
ACTION(act_next_state_default); ACTION(act_seq_next_entry); ACTION(act_seq_done);
ACTION(act_seq_emit); ACTION(act_next_async_default); ACTION(act_out_async_default);
ACTION(act_next_async_cond); ACTION(act_out_async_cond); ACTION(act_function_name);
ACTION(act_function_params); ACTION(act_function_lbrace); ACTION(act_function_rbrace);
ACTION(act_function_ident); ACTION(act_function_token); ACTION(act_work_push);
ACTION(act_work_merge); ACTION(act_lbracket); ACTION(act_range_first);
ACTION(act_variable); ACTION(act_number_var); ACTION(act_call);
ACTION(act_extension); ACTION(act_range_last); ACTION(act_rbracket);
ACTION(act_radix_prefix); ACTION(act_number); ACTION(act_number_range);
ACTION(act_call_lparen); ACTION(act_call_args_done); ACTION(act_list_operator);
ACTION(act_range_match); ACTION(act_expr_token); ACTION(act_expr_operand);
ACTION(act_table_inputs); ACTION(act_table_outputs); ACTION(act_table_end);
ACTION(act_table_row_inputs); ACTION(act_table_row_outputs); ACTION(act_min_equals);
ACTION(act_min_level); ACTION(act_group_name); ACTION(act_group_end);
ACTION(act_declare_name); ACTION(act_declare_entity); ACTION(act_declare_vars);
ACTION(act_declare_init); ACTION(act_declare_end); ACTION(act_declare_lparen);
ACTION(act_declare_rparen); ACTION(act_declare_word); ACTION(act_property_name);
ACTION(act_property_end); ACTION(act_property_word); ACTION(act_condition_begin);
ACTION(act_demorgan_equals); ACTION(act_demorgan_level); ACTION(act_regsel_equals);
ACTION(act_regsel_level);
#undef ACTION

/* -------------------------------------------------------------------------
 * Error messages
 * ------------------------------------------------------------------------- */
enum {
    ERR_PROGRAM = 0,
    ERR_OPEN = 1,
    ERR_BAD_NUMBER = 2,
    ERR_LIB_KEY = 3,
    ERR_LIB_INTERFACE = 4,
    ERR_BAD_LIBRARY = 5,
    ERR_NO_DEVICE = 6,
    ERR_SYNTAX = 7,
    ERR_TOO_MANY = 8,
    ERR_MISSING_SYMBOL = 9,
    ERR_ALREADY_ASSIGNED = 11,
    ERR_SIZE_MISMATCH = 12,
    ERR_UNDEFINED_FUNCTION = 13,
    ERR_ALREADY_DECLARED = 14,
    ERR_OUT_OF_MEMORY = 15,
    ERR_ARG_COUNT = 16,
    ERR_DISK_WRITE = 17,
    ERR_NOT_ASSIGNED = 18,
    ERR_MIXED_INDEX = 19,
    ERR_INDEX_TOO_LARGE = 20,
    ERR_HEADER_TWICE = 21,
    ERR_MISSING_HEADER = 22,
    ERR_RANGE_ARGS = 23,
    ERR_RANGE_MIN = 25,
    ERR_BAD_TABLE = 26,
    ERR_PRESENT_STATE = 27,
    ERR_NEXT_STATE = 28,
    ERR_FLIPFLOP_TYPE = 29,
    ERR_SELF_DEPENDENT = 30,
    ERR_MIN_LEVEL = 31,
    ERR_UNKNOWN_NEXT_STATE = 32,
    ERR_MULTI_ASYNC_DEFAULT = 33,
    ERR_MULTI_SYNC_DEFAULT = 34,
    ERR_MULTI_UNCONDITIONAL = 35,
    ERR_NO_STATE_MACHINE = 36,
    ERR_DUPLICATE_STATE = 37,
    ERR_NO_TARGET = 38,
    ERR_LINE_TOO_LONG = 39,
    ERR_HEADER_NAME = 40,
    ERR_DECIMAL_DONTCARE = 41,
    ERR_ALL_DONTCARE = 42,
    ERR_EOF_IN_COMMENT = 45,
    ERR_DEMORGAN_LEVEL = 46,
    ERR_COMPARE_SIZE = 47,
    ERR_FIXED_POLARITY = 48,
    ERR_DECLARE_ENTITY = 49,
    ERR_REGISTER_TYPE = 50,
    ERR_REGSEL_LEVEL = 51
    /* codes from 1000 are internal consistency checks; they are printed
     * with their number and the text "program error" */
};

/* -------------------------------------------------------------------------
 * Limits.  Several of them decide when an error is reported ("line exceeds
 * maximum length", "too many errors", internal errors for the stacks).
 * ------------------------------------------------------------------------- */
#define LINE_MAX_CHARS 0x100       /* g_parse_line, g_parse_marker: 257 bytes each */
#define MAX_PENDING_ERRORS 30      /* messages queued for one source line */
#define WORK_STACK_DEPTH 30
#define MODE_STACK_DEPTH 20
#define TOKEN_TEXT_SIZE 0x103
#define NAME_BUF_SIZE 0x101
#define STRING_CHUNK 2000
#define TOKEN_CHUNK 1000
#define LIST_CHUNK 1000
#define SYMBOL_CHUNK 55
#define MAX_INTERN_LEN 0xff
#define LHS_CHUNK 50
#define STATE_CHUNK 50
#define TERM_CHUNK 50
#define DECLARE_MAX 10             /* entity classes read from declare.def */
#define MAX_EXPAND_DEPTH 10000     /* cycle guard, see emit_expression */

/* the phase arguments, see parse_run */
#define g_parse_work_dir g_parse_args[1]
#define g_parse_library_path g_parse_args[2]
#define g_parse_pid_text g_parse_args[3]
#define g_parse_device_arg g_parse_args[4]
#define g_parse_source_path g_parse_args[5]
#define g_parse_virtual_device g_parse_args[7]     /* "1" for a virtual device */

static inline const char *text_or_empty(const char *s)
{
    return s != NULL ? s : "";
}

static inline const char *or_null(const char *s)
{
    return s != NULL ? s : "(null)";
}

/* -------------------------------------------------------------------------
 * Shared state and functions, by the file that defines them
 * ------------------------------------------------------------------------- */

/* parse.c: messages, memory, files, listing and entry point */
extern const char *g_parse_args[10];   /* copy of the phase arguments */
extern int g_parse_min_level;          /* flag m */
extern int g_parse_demorgan_mode;      /* flag kb = 2, kd = 1 */
extern int g_parse_one_hot;            /* flag o, one state bit per state */
extern char *g_parse_out_name;         /* CB<pid> */
void parse_trace_puts(const char *text);
void *parse_alloc_tracked(size_t size, const char *context);
char *parse_dup_string(const char *s, const char *context);
FILE *parse_open_file(const char *path, const char *mode);
void parse_close_file(FILE *fp);
int parse_read_char(FILE *fp);
void parse_unread_char(int c, FILE *fp);
void parse_error_at(int code, const char *text);
void parse_warning_at(int code, const char *text);
void parse_error_symbol(int code, const symbol_t *sym);
void parse_report_now(int code, int kind, const char *text, const char *ext);
void parse_fatal_error(int code, const char *detail);
char *parse_directory_prefix(const char *path);
char *parse_make_file_name(const char *dir, const char *source, const char *ext);
void parse_end_of_line(void);

/* parse_tokens.c: tokens, token lists, the work list stack, numbers */
extern list_t *g_parse_work;           /* top of the stack */
extern int g_parse_work_level;
void parse_tokens_reset(void);
token_t *parse_token_alloc(void);
void parse_token_free(token_t *tok);
void parse_list_init(list_t *list);
list_t *parse_list_alloc(void);
void parse_list_free(list_t *list);
list_t *parse_list_clone_header(const list_t *list);
void parse_list_push_front(token_t *tok, list_t *list);
void parse_list_append(token_t *tok, list_t *list);
void parse_list_clear(list_t *list);
void parse_list_destroy(list_t *list);
token_t *parse_list_pop_front(list_t *list);
token_t *parse_list_pop_back(list_t *list);
void parse_list_concat(list_t *dst, list_t *src);
token_t *parse_token_copy(const token_t *tok);
list_t *parse_list_copy(const list_t *list);
token_t *parse_token_new_op(int id);
token_t *parse_token_new_text(const char *text, int type);
void parse_append_op(list_t *list, int id);
void parse_list_set_negated(list_t *list, int negated);
void parse_list_insert_between(const token_t *model, list_t *list);
void parse_list_insert_missing(const token_t *model, int type, list_t *list);
void parse_list_insert_where(const token_t *model, int first, int second, int unless, list_t *list);
token_t *parse_list_nth_variable(const list_t *list, int n);
token_t *parse_list_find_type(const list_t *list, int type);
void parse_list_replace_type(int from, int to, list_t *list);
void parse_work_reset(void);
void parse_work_pop(void);
void parse_work_push(void);
number_t *parse_number_alloc(void);
number_t *parse_number_text(const char *text, int radix);
const char *parse_format_hex(uint32_t value);
int parse_number_radix(const number_t *num);
int parse_compare_numbers(const number_t *a, const number_t *b);
token_t *parse_token_new_number_text(const token_t *tok, int type);
token_t *parse_token_new_hex(uint32_t value, int type);
void parse_append_hex_number(uint32_t value, list_t *list);
void parse_append_number(const number_t *num, list_t *list);

/* parse_symtab.c: strings, symbol tables, device library */
extern char g_parse_empty_ext[1];      /* extension of a symbol without one */
extern symtab_t *g_parse_first_table;
extern int g_parse_total_syms;
extern symtab_t *g_parse_table;        /* table being filled */
extern const char *g_parse_device_name;
extern device_t *g_parse_device;
void parse_symtab_reset(void);
const char *parse_intern(const char *text);
void parse_copy_lower(char *dst, size_t dst_size, const char *src);
int parse_name_index(const char *name);
void parse_name_prefix(char *out, size_t out_size, const char *name);
int parse_compare_names(const char *a, const char *b);
void parse_truncate_name(char *name, size_t limit);
void parse_make_indexed_name(char *out, const char *prefix, unsigned number);
symbol_t *parse_find_symbol_of_type(const char *name, const char *ext, int type, const symtab_t *table);
symbol_t *parse_enter_symbol(const char *name, const char *ext, int may_exist, symtab_t *table);
symtab_t *parse_new_table(const char *name);
symtab_t *parse_find_table(const char *name);
uint32_t parse_dev_arch_byte(const device_t *dev, uint32_t field);
const char *parse_virtual_flag(void);
const char *parse_dev_ext_name(int code, const device_t *dev);
int parse_dev_ext_code(const char *ext, const device_t *dev);
int parse_dev_pin_index(int pin, int ext_code, const device_t *dev);
uint32_t parse_dev_pin_kind(const device_t *dev, int index);
const char *parse_device_entry_name(const device_t *dev);
void parse_require_device(symtab_t *table);
void parse_select_device_from_args(void);

/* parse_lex.c: lexer */
extern char g_parse_line[LINE_MAX_CHARS + 1];   /* text of the current line */
extern char g_parse_marker[LINE_MAX_CHARS + 1]; /* '^' under the error columns */
extern FILE *g_parse_in;
extern int g_parse_in_open;
extern int g_parse_in_line;            /* current line number */
extern int g_parse_in_col;             /* characters of the line read so far */
extern list_t g_parse_queue;           /* tokens to read before the file */
void parse_lex_reset(void);
void parse_lex_open(const char *name);
void parse_lex_header_text_mode(void);
void parse_lex_recover(char *text);
int parse_actions_enabled(void);
void parse_push_mode(void);
void parse_pop_mode(void);
token_t *parse_next_token(void);
const char *parse_token_text(const token_t *tok);

/* parse_grammar.c: keyword lookup, parser */
extern int g_parse_last_token_type;
void parse_grammar_reset(void);
const char *parse_keyword_name(int id);
const keyword_t *parse_find_keyword(const char *text);
const keyword_t *parse_find_in_table(const keyword_t *table, int count, const char *text);
const keyword_t *parse_find_punctuation(const char *text);
void parse_grammar_run(void);

/* parse_actions.c: semantic actions (the act_* functions are declared above) */
extern char g_parse_name_buf[NAME_BUF_SIZE]; /* scratch for generated names */
extern int g_parse_expand_fields;      /* field names stand for their members */
extern int g_parse_list_mode;          /* numbers are list members */
extern int g_parse_radix;              /* TOK_RADIX_x */
extern int g_parse_brace_depth;
extern symbol_t *g_parse_def_symbol;   /* FIELD/GROUP/FUNCTION being defined */
extern list_t g_parse_numbers;         /* numbers of the statement */
extern list_t g_parse_call_text;       /* body of the call being expanded */
extern symbol_t **g_parse_lhs_syms;
extern int g_parse_max_min_level;
extern FILE *g_parse_oed_fp;
void parse_actions_reset(void);
void parse_actions_begin(void);
const char *parse_variable_name(const token_t *tok);
void parse_grow_targets(void);
int parse_is_variable_token(const token_t *tok);
void parse_run_statement_end(void);

/* parse_lower.c: function calls, range/match, TABLE */
void parse_lower_reset(void);
void parse_queue_prepend(list_t *list);
void parse_append_bracket_vars(list_t *list, const list_t *vars);

/* parse_sequence.c: SEQUENCE and CONDITION */
extern int g_parse_in_sequence;
void parse_sequence_reset(void);

/* parse_output.c: CB<pid> output */
void parse_output_reset(void);
void parse_write_output(void);

#endif
