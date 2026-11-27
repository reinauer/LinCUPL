/*
 * sim.h - shared declarations of the CUPL simulator (csima / vsima).
 *
 * csima and vsima are the same program; the places where they differ test
 * g_vsima.
 */
#ifndef SIM_NATIVE_H
#define SIM_NATIVE_H

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#include "platform.h"
#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ascii.h"
#include "cfile.h"
#include "devlib.h"
#include "phase.h"
#include "sim_truth.h"

#ifdef SIM_DEFINE_GLOBALS
#define SIM_EXTERN
#else
#define SIM_EXTERN extern
#endif

#if defined(__GNUC__) || defined(__clang__)
#define SIM_NORETURN __attribute__((noreturn))
#define SIM_PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#define SIM_NORETURN
#define SIM_PRINTF(a, b)
#endif

/* 1 while the simulator runs as vsima, 0 as csima. */
SIM_EXTERN int g_vsima;
/* Text that differs between csima and vsima (mostly the detail of internal errors). */
#define VTEXT(csima_text, vsima_text) (g_vsima ? (vsima_text) : (csima_text))

/* -------------------------------------------------------------------------
 * Logic values of a signal (per-symbol byte arrays, register outputs, ...)
 * ------------------------------------------------------------------------- */
enum {
    LV_NONE = 0,
    LV_LOW = 2,
    LV_HIGH = 3,
    LV_UNKNOWN = 6,    /* X */
    LV_HIGHZ = 7
};

/* -------------------------------------------------------------------------
 * Error numbers (index into the message table in sim.c; >= 1000 are internal
 * consistency checks, all printed as "program error:  ")
 * ------------------------------------------------------------------------- */
enum {
    ERR_PROGRAM = 0,
    ERR_OPEN = 1,                 /* could not open:   */
    ERR_BAD_NUMBER = 2,           /* invalid number:   */
    ERR_FILE_FORMAT = 3,          /* invalid file format:   */
    ERR_LIB_KEY = 4,              /* invalid library access key */
    ERR_LIB_INTERFACE = 5,        /* invalid library interface */
    ERR_BAD_LIBRARY = 6,          /* bad library file:   */
    ERR_NO_DEVICE = 7,            /* device not in library:   */
    ERR_OUTPUT_FORMAT = 8,        /* invalid output format:   */
    ERR_SYNTAX = 9,               /* invalid syntax:   */
    ERR_EXPECTING_DEVICE = 10,    /* expecting device:   */
    ERR_UNKNOWN_SYMBOL = 11,      /* unknown symbol:   */
    ERR_TOO_MANY_SYMBOLS = 12,    /* too many symbols */
    ERR_EXCESSIVE_VALUE = 13,     /* excessive test value:   */
    ERR_INSUFFICIENT_VALUES = 14, /* insufficient test values */
    ERR_FIELD_DEFINED = 15,       /* field already defined:   */
    ERR_TOO_MANY_ERRORS = 16,     /* too many errors */
    ERR_MISSING_SYMBOL = 17,      /* missing symbol:   */
    ERR_OUT_OF_MEMORY = 18,       /* out of memory:   */
    ERR_EXPECTED = 19,            /* " " (used for "user expected ...") */
    ERR_UNSTABLE = 20,            /* unstable output:   */
    ERR_BAD_TEST_VALUE = 21,      /* invalid test value:   */
    ERR_BAD_FAULT_ID = 22,        /* bad fault id:   */
    ERR_READ = 23,                /* could not read file:   */
    ERR_WRITE = 24,               /* could not write file:   */
    ERR_HEADER_INCONSISTENT = 25, /* inconsistent header information */
    ERR_HEADER_MISSING = 26,      /* missing header item(s) */
    ERR_OLD_ABS = 27,             /* old absolute file format:   */
    ERR_STATEMENT_TOO_LONG = 28,  /* statement too long */
    ERR_TRACE_LEVEL = 29,         /* invalid trace level:   */
    ERR_BAD_CHAR = 30,            /* invalid character:   */
    ERR_UNEXPECTED_EOF = 31,      /* disk read error, unexpected end of file:   */
    ERR_UNDEFINED_FEEDBACK = 32,  /* feedback usage of undefined output:  */
    ERR_NO_PIN_NUMBER = 33,       /* csima: pin number is undefined for:
                                     vsima: jedec vectors cannot be created with undefined pin numbers */
    ERR_REGISTER_TYPE = 34,       /* vsima only: possible undefined register type:  */
    /* option f: the design is taken from a JEDEC file (jedload.c) */
    ERR_JEDEC_FILE = 35,          /* invalid JEDEC file:   */
    ERR_JEDEC_NO_DEVICE = 36,     /* no device for JEDEC file:   */
    ERR_JEDEC_DEVICE = 37,        /* device cannot be simulated from a JEDEC file:   */
    ERR_JEDEC_MODE = 38,          /* mode fuses of the JEDEC file select another mode than device:   */
    ERR_JEDEC_DEAD_TERMS = 39     /* product terms without effect, the fuse map makes an input of pin:   */
};

/* -------------------------------------------------------------------------
 * Runtime: output, errors, memory, files (sim.c)
 * ------------------------------------------------------------------------- */

/* printf to a stream.  Text for stdout is line buffered (complete lines
 * only) unless option v is set; anything else is written to the file. */
void stream_printf(FILE *fp, const char *fmt, ...) SIM_PRINTF(2, 3);

/* Report (code >= 0) and abandon the phase.  Never returns. */
SIM_NORETURN void fatal_error(int code, const char *detail);
/* Queue an error for the source line being read, marked at the current
 * column.  The queue is printed when the line is complete. */
void report_error(int code, const char *detail);
/* Same, at an explicit column. */
void report_error_at(int code, int column, const char *detail);
/* Queue a "Please note:" warning at the current column. */
void report_warning(int code, const char *detail);
/* Queue "user expected (<expected>) for <name>" (ERR_EXPECTED). */
void report_expected(int column, const char *expected, const char *name);
/* Queue "invalid test value:  <value> for <name>" (vsima). */
void report_bad_value(int column, const char *value, const char *name);
/* Print a warning immediately (not tied to a source line). */
void print_warning_now(int code, const char *detail);

/* Zero-filled tracked allocation; fatal_error(ERR_OUT_OF_MEMORY, context)
 * on failure.  Everything still allocated is released when the phase ends. */
void *alloc_tracked(size_t size, const char *context);
char *dup_string(const char *s, const char *context);
/* Release one block early (NULL is ignored). */
void free_tracked(void *p);

/* fopen with a case-insensitive file name lookup; "t" is dropped from the
 * mode.  Files are registered and closed when the phase ends. */
FILE *open_file(const char *path, const char *mode);
void close_file(FILE *fp);
/* Text-mode read: carriage returns are dropped and Ctrl-Z ends the file. */
int read_char(FILE *fp);
/* fputc / fputs that write "\n" as "\r\n" in *.jed text files.  For stdout
 * they write directly, bypassing the line buffer of stream_printf. */
int file_putc(int c, FILE *fp);
int file_puts(const char *s, FILE *fp);
void remove_file(const char *path);
extern jmp_buf g_abort_jmp;         /* fatal_error jumps back to sim_run */

/* Build "<dir><basename of name, cut at .pld><ext>" (tracked). */
char *make_file_name(const char *dir, const char *name, const char *ext);

/* -------------------------------------------------------------------------
 * Bit sets stored as arrays of 16-bit words (bit i = word i>>4, bit i&15)
 * ------------------------------------------------------------------------- */
static inline int bit_test(const uint16_t *words, int i)
{
    return (words[i >> 4] >> (i & 15)) & 1;
}
static inline void bit_set(uint16_t *words, int i)
{
    words[i >> 4] = (uint16_t)(words[i >> 4] | (1u << (i & 15)));
}
static inline void bit_clear(uint16_t *words, int i)
{
    words[i >> 4] = (uint16_t)(words[i >> 4] & ~(1u << (i & 15)));
}

/* -------------------------------------------------------------------------
 * Generic lists
 * ------------------------------------------------------------------------- */
struct sym;

/* List node; tokens of the parser are list nodes too (kind = token code). */
typedef struct list_node {
    uint8_t flags;
    uint8_t kind;
    union {
        void *ptr;
        char *text;
        struct sym *sym;
    } u;
    struct list_node *next;
} list_node_t;

/* List header */
typedef struct list {
    list_node_t *head;
    list_node_t *tail;
    int count;
} list_t;

list_node_t *node_new(void);
list_t *list_new(void);
void node_free(list_node_t *node);
/* Stand-in token used during error recovery (static, never freed). */
int is_phantom_token(const list_node_t *tok);
void list_free_header(list_t *list);
void list_push_front(list_node_t *node, list_t *list);
int list_is_empty(const list_t *list);
void list_append(list_node_t *node, list_t *list);
void list_free_nodes(list_t *list);
list_node_t *list_pop_front(list_t *list);
/* Append src to dst; both headers then describe the joined list. */
void list_concat(list_t *dst, list_t *src);
list_t *list_clone(const list_t *src);
/* Push one node per bit of value (LSB first, so the MSB ends up at the
 * head); kind is kind_one for a 1 bit, kind_zero for a 0 bit. */
void list_push_bits(list_t *list, unsigned value, int kind_zero, int kind_one);
/* Push nodes of `kind` at the front until the list has `count` nodes. */
void list_pad_front(list_t *list, int count, int kind);
/* Text nodes <stem><n> for n from first (excluded) to last (included). */
list_t *list_name_range(const char *stem, int first, int last, int kind, int flags);
void list_reset(list_t *list);
void list_copy_header(list_t *dst, const list_t *src);

/* Stack of list headers used by the parser actions. */
#define LIST_STACK_SIZE 20
SIM_EXTERN list_t g_list_stack[LIST_STACK_SIZE];
SIM_EXTERN list_t *g_list_top;                       /* top entry of g_list_stack */
void list_stack_pop(void);
void list_stack_push(void);

/* Return the shared copy of a string (NULL for NULL or ""). */
const char *intern_string(const char *s);

/* String helpers */
int trailing_number(const char *s);                  /* -1 if none */
void strip_trailing_number(char *dst, const char *src);
void make_indexed_name(char *dst, const char *stem, int index);
int parse_number(char *text, int base_token);        /* base_token: HEX, OCTAL, DECIMAL or BINARY */
void copy_lowercase(char *dst, const char *src);

/* -------------------------------------------------------------------------
 * Absolute file (.abs) contents
 * ------------------------------------------------------------------------- */

/* A block of product terms ("frame") read from the .abs file, with its
 * header (cf_frame_t; offsets below).  Each row consists of three bit planes
 * of plane_words 16-bit words; bit i of a plane is bit_test(plane + 1, i). */
typedef struct frame {
    uint8_t flags;          /* +0x00: 0x80 field definition, 0x20/0x10 end of file */
    uint8_t b1;             /* +0x01 */
    uint16_t index;         /* +0x02: output (term table) number */
    uint16_t nsyms;         /* +0x04: number of symbols covered by a plane */
    uint16_t nrows;         /* +0x06: number of rows (product terms) */
    uint16_t w8;            /* +0x08 */
    uint16_t wa;            /* +0x0a */
    uint16_t *rows;         /* nrows * row_words words */
    uint16_t *plane2;       /* rows + 2 * plane_words */
    uint16_t *plane1;       /* rows + plane_words */
    uint16_t *plane0;       /* rows */
    int byte_size;          /* size in the file, header included */
    int row_words;          /* 3 * plane_words */
    int total_words;
} frame_t;

/* Symbol table entry (cf_symbol_t in the .abs file; offsets below). */
typedef struct sym {
    char name[0x20];        /* +0x00 */
    char type[10];          /* +0x20: node type name ("io", "byp", ...) */
    uint16_t pin;           /* +0x2a: pin number, 0 = none */
    uint8_t kind;           /* +0x2c */
    uint8_t flags;          /* +0x2d: 0x80, 0x40, 0x20, 0x10 ... */
    int16_t pin_rec;        /* +0x34: index of the device's pin record, < 0 none */
    int16_t node_rec;       /* +0x36: index of the device's node record, < 0 none */
    uint32_t file_3c;       /* +0x3c as stored in the file (vsima: non-zero = has terms) */
    frame_t *frame;         /* field definition frame (kind 1), not in the file */
} sym_t;

SIM_EXTERN sym_t *g_syms;           /* symbol table */
SIM_EXTERN uint16_t g_nsyms;        /* number of symbols (abs header +4) */
SIM_EXTERN char g_device_name[28];  /* device name from the .abs header */
SIM_EXTERN int g_nsyms_file;       /* number of symbols in the file */
SIM_EXTERN int g_dev_has_class;     /* device header byte +0x12 is non-zero */
SIM_EXTERN char *g_abs_extra;       /* variable part of the .abs header */
/* Header items (bit 0 NAME ... bit 7 LOCATION) that g_abs_extra has and the
 * .si file is compared with: all of them, unless the design comes from a
 * JEDEC file that does not carry them (option f). */
SIM_EXTERN uint8_t g_header_items;

/* Entry of the term table g_terms, indexed by frame->index. */
typedef struct term_entry {
    frame_t *frame;
    int nrows;
    int first_row;          /* running total of rows before this entry */
} term_entry_t;

SIM_EXTERN term_entry_t *g_terms;
SIM_EXTERN int g_nterms;
SIM_EXTERN int g_total_rows;
SIM_EXTERN int g_row_words;         /* words per row (3 planes), -1 until known */
SIM_EXTERN uint16_t g_plane_words;  /* g_row_words / 3 */

/* A named FIELD (list g_fields). */
typedef struct field {
    const char *name;       /* points at the symbol name */
    list_t *members;        /* nodes with u.sym, kind 0xd, flags bit 0 */
    struct field *next;
} field_t;

SIM_EXTERN field_t *g_fields;
SIM_EXTERN field_t *g_fields_tail;
field_t *find_field(const char *name);
field_t *define_field(const char *name);
/* The symbol an ORDER name refers to. */
sym_t *find_symbol(const char *name);

/* -------------------------------------------------------------------------
 * Device library record (loaded by load_design)
 * ------------------------------------------------------------------------- */
enum {
    DEV_SEC_HEADER = 3,     /* one 152-byte record */
    DEV_SEC_PINS = 6,       /* 16-byte records */
    DEV_SEC_NODES = 7,      /* 39-byte records */
    DEV_SEC_ALIASES = 12    /* 22-byte names */
};
#define DEV_PIN_REC_SIZE 16
#define DEV_NODE_REC_SIZE 39

SIM_EXTERN uint8_t *g_dev_rec;      /* the device record */
void use_device_image(uint8_t *image, size_t size);
int jed150x_load(const uint8_t *fuses, unsigned nfuses, unsigned pins,
                const char *name, const char *description);
int jed150x_name_matches(const char *name, unsigned nfuses, unsigned pins);
int jed150x_same_package(const char *name);
int jed150x_active(void);
int jed150x_sequential(void);
void jed150x_reset(void);
void jed150x_symbols(void);
void jed150x_bind(void);
void jed150x_powerup(void);
void jed150x_step(int pass);
void jed150x_truth_pins(sim_truth_t *table);
/* Start of section n inside the device record: rec + u32(rec + n*20 + 8). */
const uint8_t *dev_section(int n);
/* Number of records in section n: u16(rec + n*20 + 4). */
int dev_section_count(int n);
/* The single header record (section 3); e.g. byte +0x11, +0x12, word +0x19. */
const uint8_t *dev_header(void);
/* Pin record i (section 6): word +0, +2; byte +4 (cell type), +5, ... */
const uint8_t *dev_pin(int i);
/* Node record i (section 7): word +0, +2; byte +4 (cell type), +5, +6, +0x25 ... */
const uint8_t *dev_node(int i);

SIM_EXTERN int g_dev_is_isp1016;    /* device name starts with p1016 / p1020 */
SIM_EXTERN char g_dev_is_7c33x;     /* p7c330, p7c331 or mach* */
SIM_EXTERN char g_dev_is_78c800;    /* xl78c800 */
SIM_EXTERN int g_dev_is_7c330;      /* library entry name is p7c330 */

/* -------------------------------------------------------------------------
 * Phase arguments and options
 * ------------------------------------------------------------------------- */
SIM_EXTERN const char *g_args[9];   /* copy of the phase argv */
enum {
    OPT_LISTING = 0x02,     /* l / w: write .so and .wo */
    OPT_FUSEMAP = 0x04,     /* f: simulate <design>.jed, there is no .abs file */
    OPT_JEDEC = 0x08,       /* j: append test vectors to the .jed file */
    OPT_N = 0x10,           /* n */
    OPT_CONSOLE = 0x80      /* v */
};
SIM_EXTERN uint16_t g_options;      /* OPT_* */
SIM_EXTERN FILE *g_list_file;       /* <design>.so */
SIM_EXTERN char *g_list_name;
SIM_EXTERN FILE *g_wave_file;       /* <design>.wo */
SIM_EXTERN char *g_wave_name;
SIM_EXTERN int g_error_count;
SIM_EXTERN char *g_dir_prefix;      /* directory part of the source name */
SIM_EXTERN char g_base_name[260];    /* source name without directory and extension */
SIM_EXTERN char g_name_buf[0x402];  /* scratch buffer for built names and output lines */

/* -------------------------------------------------------------------------
 * Simulation input (.si) reading state, shared by lexer / parser / listing
 * ------------------------------------------------------------------------- */

/* Entry of the source stack: an open .si file. */
typedef struct source {
    char *name;
    FILE *fp;
    int line;
    int col;
    char at_eof;
} source_t;

#define SOURCE_STACK_SIZE 20
SIM_EXTERN source_t g_source_stack[SOURCE_STACK_SIZE];
SIM_EXTERN source_t *g_source;      /* current source, NULL if none */
SIM_EXTERN int *g_line_ptr;         /* line counter used in messages */
SIM_EXTERN char g_line_pending;     /* g_line_text holds an unlisted line */
SIM_EXTERN char g_line_text[0x401]; /* text of the source line being read */
SIM_EXTERN int g_fields_unlisted;   /* FIELDs not yet written to the listing */
SIM_EXTERN char g_listing_off;      /* source lines are not listed */
SIM_EXTERN char g_vector_running;   /* a test vector is being simulated */
/* Terminate and list the current source line with its queued errors, then
 * start the next line. */
void flush_source_line(char *line);

/* -------------------------------------------------------------------------
 * Per-symbol simulation state (allocated by sim_allocate, g_nsyms entries each)
 * ------------------------------------------------------------------------- */
SIM_EXTERN uint8_t *g_row_value;    /* per row: LV_LOW / LV_HIGH / LV_UNKNOWN */
SIM_EXTERN uint8_t *g_row_stable;   /* per row */
SIM_EXTERN uint8_t *g_sig_hiz;      /* T_VAL_Z if the vector leaves the pin floating */
SIM_EXTERN uint8_t *g_sig_value;    /* current value of each symbol (LV_*) */
SIM_EXTERN uint8_t *g_sig_out;      /* level seen at the pin, 0 = not computed */
SIM_EXTERN uint8_t *g_reg_state;    /* state of the register / node */
SIM_EXTERN uint8_t *g_reg_prev;     /* g_reg_state of the previous pass */
SIM_EXTERN uint8_t *g_reg_q;        /* output of a pin register */
SIM_EXTERN uint8_t *g_sig_unstable; /* the value depends on an unstable signal */
SIM_EXTERN uint8_t *g_sig_prev;     /* g_sig_value of the previous pass */
SIM_EXTERN uint8_t *g_pin_reg_input; /* value given for a pin register */
SIM_EXTERN uint8_t *g_vec_level;    /* level given in the test vector */
SIM_EXTERN uint8_t *g_sig_is_clock; /* 1 if the symbol clocks a register */
SIM_EXTERN uint8_t *g_sig_saved;    /* g_sig_value at the end of the last vector */
SIM_EXTERN uint8_t *g_reg_before;   /* register state before the evaluation */
SIM_EXTERN uint8_t *g_node_level;   /* level in front of the output buffer */
SIM_EXTERN uint8_t *g_bypass_sym;   /* symbol (.byp) that bypasses the register */
SIM_EXTERN uint16_t *g_feedback_sym; /* per symbol, index of the symbol it feeds */
SIM_EXTERN uint16_t *g_fb_undefined; /* bit set: feedback symbols without a defined value */
SIM_EXTERN uint16_t *g_fb_symbols;  /* bit set: feedback symbols */
SIM_EXTERN uint16_t *g_term_outputs; /* bit set: symbols driven by the output being evaluated */

/* Input vector the product terms are matched against: three bit planes of
 * g_plane_words words each, bit i = symbol i. */
SIM_EXTERN uint16_t *g_in_unknown;  /* 1 = value unknown */
SIM_EXTERN uint16_t *g_in_valid;
SIM_EXTERN uint16_t *g_in_high;
/* Store logic value (LV_LOW/LV_HIGH/LV_UNKNOWN) for symbol index. */
void set_input_value(int value, int index);
/* set_input_value(g_reg_q[src], index). */
void set_input_from_reg(int src, int index);
/* Read it back; `invert` swaps LV_LOW and LV_HIGH. */
int get_input_value(int index, int invert);

/* -------------------------------------------------------------------------
 * Macrocell models
 * ------------------------------------------------------------------------- */

/* Working data of one macrocell model instance: up to 14 ints (symbol
 * indices, polarities).  The meaning of a slot depends on the cell type. */
#define CELL_STATE_SLOTS 14
typedef struct cell_state {
    int s[CELL_STATE_SLOTS];
} cell_state_t;

/* Per-symbol binding to the device's macrocell models. */
typedef struct cell {
    int16_t node_type;      /* cell type of the node record */
    int16_t pin_type;       /* cell type of the pin record */
    uint16_t number;        /* pin / cell number */
    uint16_t term;          /* output (term table) number */
    int16_t true_out;       /* 0 = the output is inverted */
    int16_t true_fb;        /* 1 = fed back without inversion */
    int16_t fed_back;
    int clock_enable;      /* CE equation symbol, or -1 for unconditional clocking */
    cell_state_t *node_state;
    cell_state_t *pin_state;
    uint8_t kind;           /* 0 none, 1 pin, 2 node, 3 pin with fed-back node */
    uint8_t bound;          /* has a pin or node record */
    uint8_t is_node;        /* symbol is a node (flags 0x40) */
} cell_t;

SIM_EXTERN cell_t *g_cells;

/* Macrocell model table, indexed by cell type 0 .. 0x43. */
#define CELL_TYPE_COUNT 0x44
typedef void (*cell_init_fn)(int sym_index);
typedef int (*cell_eval_fn)(int sym_index, int pass);
typedef struct cell_type {
    int state_size;         /* bytes of cell_state used (20 .. 56), 0 = no model */
    cell_init_fn init;
    cell_eval_fn eval;
} cell_type_t;
/* Returns the table of the running variant. */
const cell_type_t *cell_types(void);

/* -------------------------------------------------------------------------
 * ORDER columns and test vectors (parser actions, vector output, core)
 * ------------------------------------------------------------------------- */

/* One column of the ORDER statement. */
typedef struct order {
    field_t *field;         /* the FIELD this column starts, if width > 1 */
    uint16_t pin;
    uint16_t column;        /* own index */
    int16_t sym_index;
    sym_t *sym;
    uint8_t positive;       /* listed with the polarity it was declared with */
    uint8_t node_level;     /* observe a register alias before the pin buffer */
    int16_t result;         /* value token computed by the simulation */
    int16_t value;          /* value token given in the test vector */
    int16_t width;          /* columns of the field starting here (1 for a single name) */
    int16_t text_col;       /* position of the column in the printed vector */
    list_t before;          /* texts / spacings printed in front of the column */
} order_t;

SIM_EXTERN order_t *g_order;        /* g_nsyms + 2 entries */
SIM_EXTERN int g_order_count;
SIM_EXTERN int g_value_count;       /* values read for the current vector */
SIM_EXTERN int g_vector_number;     /* vectors simulated so far */
SIM_EXTERN int g_trace_level;       /* $TRACE 0 .. 4 */
SIM_EXTERN char g_sim_on;           /* cleared by $SIMOFF */
SIM_EXTERN uint8_t *g_stuck;        /* per row, LV_LOW / LV_HIGH for a stuck-at fault */
SIM_EXTERN list_t g_strings;        /* interned strings */
SIM_EXTERN char g_out_text[0x402];  /* vector text without the inserted texts */
SIM_EXTERN int g_out_pos;           /* write position in g_name_buf */
SIM_EXTERN int g_out_text_pos;      /* write position in g_out_text */

/* Value tokens of a test vector (also stored in order_t.value / .result). */
enum {
    T_VAL_H = 0x27, T_VAL_L = 0x28, T_VAL_Z = 0x29, T_VAL_X = 0x2a, T_VAL_P = 0x2b,
    T_VAL_C = 0x2c, T_VAL_K = 0x2d, T_VAL_N = 0x2e, T_VAL_0 = 0x2f, T_VAL_1 = 0x30,
    T_VAL_STAR = 0x31, T_VAL_R = 0x53
};
/* The character of a value token, 0 if it is none. */
int value_token_char(int token);

extern const uint16_t k_invert_level[8];  /* the opposite logic level */
enum { PIN_KIND = 5 };              /* byte +5 of a device pin record */
void update_cells(int pass);        /* run the node models of all symbols */
/* Simulated levels to result tokens; `check` compares them with the vector. */
void compare_results(int check);
int design_has_registers(void);     /* vsima: is anything clocked? */
void warn_missing_registers(void);  /* vsima: clock without register */
void bind_cells(void);              /* bind the symbols to the macrocell models */
void pair_feedback_symbols(void);   /* fill g_feedback_sym */
/* The pin symbol with the same pin number as sym_index and the given type. */
int find_feedback_symbol(int sym_index, int node_type);
void jedec_find_power_pins(void);   /* mark the supply pins for the V fields */
int vector_is_preload(void);        /* the current vector is a register preload */
SIM_EXTERN char g_abs_jedec_name[0x1e]; /* output file name from the .abs header */
int find_feedback_node(int sym_index); /* the node symbol marked as fed back */
int sym_type_code(const sym_t *s);   /* vsima: cell type code of the symbol's extension */
SIM_EXTERN int g_next_pin_rec;      /* vsima: next free pin record number */
int node_kind_default(int kind);    /* default cell.true_out by node record kind */
extern int g_counter_cells[6];      /* the six counter cells of the f507 */
/* Set by bind_cells: the device has nodes of type 5 or 7 and a pin of
 * kind 0x0e. */
SIM_EXTERN int g_pin_and_node_resets;
/* Macrocell models (eval.c) */
int eval_combinational(int sym, int pass);
int eval_input_register(int sym, int pass);
int eval_input_latch(int sym, int pass);
int eval_d(int sym, int pass);
int eval_d_feedback(int sym, int pass);
int eval_d_enable(int sym, int pass);
int eval_sr(int sym, int pass);
int eval_jk(int sym, int pass);
int eval_t(int sym, int pass);
int eval_latch(int sym, int pass);
int eval_internal_feedback(int sym, int pass);
int eval_io(int sym, int pass);
int eval_feedback_d(int sym, int pass);
int eval_feedback_latch(int sym, int pass);
int eval_feedback_t(int sym, int pass);
int eval_counter(int sym, int pass);
int eval_io_d(int sym, int pass);
int eval_io_latch(int sym, int pass);
int eval_io_t(int sym, int pass);
int eval_none(int sym, int pass);
/* cell_t.kind of a symbol */
int cell_kind(int sym_index);
void simulate_vector(void);         /* simulate the values in g_order[].value */
void reset_simulation(void);        /* back to the power-up state */
int stuck_fault_row(unsigned long id); /* product term row of a fault id (csima only) */
void jedec_vector(void);            /* V field of the vector just simulated */
void print_vector(int quiet, int pass);
void print_trace_title(const char *text);
void print_trace_line(const uint16_t *a, const uint16_t *b, const uint16_t *c, int value, int regs);
void print_message(list_t *list);   /* $MSG / $EXIT text */
void print_queued_errors(FILE *fp, int wave);
void clear_queued_errors(void);
int queued_error_count(void);

/* -------------------------------------------------------------------------
 * Program flow
 * ------------------------------------------------------------------------- */
int sim_run(int vsima, int argc, char **argv);   /* entry point, see cupl_phases.h */
void load_design(void);             /* read .abs and the device library entry */
void load_device_entry(const char *name); /* load the library entry `name` into g_dev_rec */
int device_in_library(const char *name);  /* does the library have the entry? */
/* Option f (jedload.c): build the design from <design>.jed and the library
 * entry of its device, in place of load_design(). */
void load_design_jedec(void);
void load_jedec_file(const char *path, int source_names);
void jedec_truth_pins(sim_truth_t *table);
int build_truth_table(unsigned max_inputs, sim_truth_t *table, char *error, size_t error_size);
/* After sim_allocate(): an output enable pin that no vector drives enables. */
void jedec_design_bound(void);
/* Resolve numbered pins and register aliases in ORDER. Set *inverted for
 * declared polarity and *node_level when observing a shared register before
 * its output buffer. Return NULL if the name cannot be resolved. */
sym_t *jedec_pin_symbol(const char *name, int *inverted, int *node_level);
void close_abs_file(void);
void build_fields(void);            /* define the FIELDs of the .abs file */
void sim_reset(void);               /* initial state of the simulation core */
void sim_allocate(void);            /* allocate and bind the simulation state */
void parse_reset(void);             /* initial state of the parser */
void parse_simulation_file(void);   /* read and execute <design>.si */
void jedec_reset(void);
void jedec_begin(void);             /* start the vectors in the JEDEC file */
void jedec_finish(void);            /* finish the JEDEC file */

#endif /* SIM_NATIVE_H */
