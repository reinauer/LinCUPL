/*
 * expand_int.h - declarations shared by the parts of the expand phase
 * (Boolean expansion, expand_run).
 *
 *   expand.c         messages, files and memory (src/shared/phase.c) and
 *                    the driver that walks the CB file section by section
 *   expand_input.c   CB file: section header, symbol table, and the parser
 *                    that turns an equation's token stream into a tree
 *   expand_device.c  device library and the binding of every symbol to the
 *                    device's pin and node functions; output polarity
 *   expand_tree.c    expression trees: node pools, constant folding,
 *                    DeMorgan, XOR expansion, multiplying out into a sum of
 *                    products
 *   expand_frame.c   frames: the sum of products as rows of bit maps, row
 *                    merging, and everything written to the CM file
 */
#ifndef LINCUPL_EXPAND_INT_H
#define LINCUPL_EXPAND_INT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "cfile.h"
#include "devlib.h"
#include "phase.h"

/* -------------------------------------------------------------------------
 * Error messages
 * ------------------------------------------------------------------------- */
enum {
    ERR_PROGRAM = 0,
    ERR_OPEN = 1,
    ERR_INVALID_FILE = 3,
    ERR_PARENTHESES = 4,
    ERR_LIB_KEY = 5,
    ERR_LIB_INTERFACE = 6,
    ERR_BAD_LIBRARY = 7,
    ERR_NO_DEVICE = 8,
    ERR_REDECLARED = 9,
    ERR_INVALID_OUTPUT = 10,
    ERR_UNKNOWN_EXTENSION = 11,
    ERR_INVALID_USAGE = 12,
    ERR_INVALID_OUTPUT_USAGE = 13,
    ERR_INVALID_INPUT = 14,
    ERR_NOT_SUPPORTED = 15,
    ERR_NO_EXPRESSION = 16,
    ERR_OUT_OF_MEMORY = 17,
    ERR_MISSING_FLIPFLOP = 18,
    ERR_DEMORGAN = 19,          /* warning */
    ERR_BANKED_MIX = 20,
    ERR_NO_EXPRESSION_ALLOWED = 21,
    ERR_INPUT_CONFLICT = 22,
    ERR_DISK_WRITE = 23,
    ERR_NO_SUCH_NODE = 24,
    ERR_EXCLUDED_OUTPUT = 25,
    ERR_DISK_READ = 26,
    ERR_INVALID_OUTPUT_PIN = 27,
    /* Internal consistency errors; all are reported as "program  error". */
    ERR_INTERNAL_LIB_OPEN = 1001,
    ERR_INTERNAL_NO_EXTRA_TABLE = 1002,
    ERR_INTERNAL_EXTRA_FULL = 1003,
    ERR_INTERNAL_EXTRA_NO_NODE = 1004,
    ERR_INTERNAL_EXTRA_NO_OUTPUT = 1006,
    ERR_INTERNAL_EXTRA_NO_EXT = 1007,
    ERR_INTERNAL_PRESET_NO_NODE = 1008,
    ERR_INTERNAL_PRESET_KIND = 1009,
    ERR_INTERNAL_EMIT_TYPE = 1010,
    ERR_INTERNAL_OP_UNDERFLOW = 1012,
    ERR_INTERNAL_OPERAND_UNDERFLOW = 1014,
    ERR_INTERNAL_OPERANDS_LEFT = 1015,
    ERR_INTERNAL_OPS_LEFT = 1016,
    ERR_INTERNAL_NO_TARGET = 1017,
    ERR_INTERNAL_BAD_TOKEN = 1018,
    ERR_INTERNAL_SYMBOL_COUNT = 1019,
    ERR_INTERNAL_NEGATE_TYPE = 1021,
    ERR_INTERNAL_ROW_FLAG = 1024,
    ERR_INTERNAL_NODE_RANGE = 1026,
    ERR_INTERNAL_FRAME_SIZE = 1027,
    ERR_INTERNAL_MERGE_BIT = 1028,
    ERR_INTERNAL_XOR_MERGE = 1030,
    ERR_INTERNAL_ALT_DEVICE = 1033
};

/* -------------------------------------------------------------------------
 * Limits and file layouts
 * ------------------------------------------------------------------------- */
#define POOL_BLOCK 1000            /* nodes / list cells allocated at a time */
#define STACK_STEP 200             /* growth step of the two parser stacks */
#define SYMBOL_NAME_SIZE CF_NAME_SIZE
#define SYMBOL_EXT_SIZE CF_EXT_SIZE

/* Token types of the CB file */
enum {
    TOK_OR = 2,
    TOK_XOR = 3,
    TOK_AND = 4,
    TOK_OPEN = 7,              /* '(' ; flag bit 0: the parenthesis is negated */
    TOK_CLOSE = 8,
    TOK_END_FILE = 0x10,
    TOK_END_EQUATION = 0x14,
    TOK_END_SECTION = 0x15,    /* another design section follows */
    TOK_VARIABLE = 0x51,
    TOK_CONSTANT = 0x52,
    TOK_CONSTANT2 = 0x62,
    TOK_LINE = 99
};
#define TOKEN_FLAG_NEGATED 0x01
#define TOKEN_FLAG_TARGET 0x08

/* Frame markers of the CM file */
#define FRAME_END_SECTION 0x20
#define FRAME_END_FILE 0x10
#define FRAME_INVERTED 0x1000

/* Symbol kinds (low nibble of symbol.kind) */
enum {
    SYM_FIELD = 1,
    SYM_PIN = 2,
    SYM_INTERMEDIATE = 3,
    SYM_END = 4,
    SYM_PINNODE = 5,
    SYM_GENERATED = 8,         /* added by this phase for unused node functions */
    SYM_NODE = 9,
    SYM_HIDDEN_NODE = 10,
    SYM_FIELD2 = 14
};

/* Symbol flags */
#define SYMF_MIN_LEVEL 0x07    /* minimisation level requested for the symbol */
#define SYMF_PIN_LOW 0x10      /* pin declared active low */
#define SYMF_EQN_LOW 0x20      /* equation written for the negated name */
#define SYMF_OUTPUT 0x40       /* symbol has an equation */
#define SYMF_INPUT 0x80        /* symbol is used in an equation */

/* symbol.polarity */
enum { POL_KEEP = 0, POL_INVERTED = 1, POL_FIXED = 2 };

/* A symbol.  The hex numbers are the offsets of the fields in a symbol
 * record of the CM file; the records of the CB file end before pin_fn. */
typedef struct symbol {
    char name[SYMBOL_NAME_SIZE];   /* 0x00 */
    char ext[SYMBOL_EXT_SIZE];     /* 0x20: extension (d, oe, ar, ...) */
    uint16_t pin;                  /* 0x2a: pin / node number; high nibble set: unnumbered node */
    uint8_t kind;                  /* 0x2c */
    uint8_t flags;                 /* 0x2d */
    uint8_t attr[4];               /* 0x2e */
    uint8_t polarity;              /* 0x32 */
    uint8_t attr2;                 /* 0x33 */
    int16_t pin_fn;                /* 0x34: index into the device's pin function table */
    int16_t node_fn;               /* 0x36: index into the device's node function table */
    int16_t out_fn;                /* 0x38: node function of the macrocell output */
    int16_t ext_code;              /* 0x3a: code of the extension */
    uint8_t pad[4];                /* 0x3c */
} symbol_t;

/* -------------------------------------------------------------------------
 * Expression trees
 *
 * A node is an operator with two operands.  Operands that are variables or
 * constants are folded into the node, so every operator comes in 25 forms,
 * numbered from the operator's base type:
 *
 *    +0 N,N    +5 V,V     +9 0,0    +13 0,V    +17 V,0    +21 N,0
 *    +1 N,V    +6 V',V   +10 1,0    +14 0,V'   +18 V',0   +22 N,1
 *    +2 N,V'   +7 V,V'   +11 0,1    +15 1,V    +19 V,1    +23 0,N
 *    +3 V,N    +8 V',V'  +12 1,1    +16 1,V'   +20 V',1   +24 1,N
 *    +4 V',N
 *
 * (left,right; N = sub-tree, V = variable, V' = negated variable).
 * ------------------------------------------------------------------------- */
enum {
    OP_AND = 0x01,
    OP_XOR = 0x1a,
    OP_OR = 0x33,
    NODE_VAR = 0x4c,           /* right = symbol, flag bit 0 = negated */
    NODE_LIST = 0x4d,          /* member of a FIELD list: left = next, right = symbol */
    NODE_NOT = 0x4f,           /* right = operand */
    NODE_NOT_VAR = 0x50,       /* NOT applied to a variable (right = symbol) */
    NODE_NOT_NVAR = 0x51,      /* NOT applied to a negated variable */
    NODE_NOT_ZERO = 0x52,
    NODE_NOT_ONE = 0x53,
    NODE_CONST = 0x54          /* right = 0 or 1, flag bit 0 inverts it */
};
#define FORM_COUNT 25
#define NODE_NEGATED 0x01
#define NODE_KEEP_XOR 0x20     /* the device implements this XOR; do not expand it */

typedef enum { K_NODE, K_VAR, K_NVAR, K_ZERO, K_ONE } operand_kind_t;

struct node;
typedef union {
    struct node *node;
    symbol_t *sym;
    uintptr_t raw;             /* constants are stored as 0 / 1 */
} operand_t;

typedef struct node {
    uint8_t type;
    uint8_t flags;
    operand_t left;
    operand_t right;           /* also links free nodes */
} node_t;

/* A list cell */
typedef struct cell {
    node_t *node;
    struct cell *next;
} cell_t;

typedef struct node_block {
    node_t *first;
    int count;
    struct node_block *next;
} node_block_t;

/* Operator stack entry of the expression parser */
typedef struct {
    int priority;
    node_t *node;
    uint8_t arity;
} op_entry_t;

/* -------------------------------------------------------------------------
 * Device library
 * ------------------------------------------------------------------------- */

/* Offsets in the device image */
#define DEV_INFO_PTR 0x44          /* offset of the device description */
#define DEV_EXT_COUNT 0x54         /* extension table: 0x15-byte records */
#define DEV_EXT_PTR 0x58
#define DEV_PINFN_COUNT 0x7c       /* pin function table: 0x10-byte records */
#define DEV_PINFN_PTR 0x80
#define DEV_NODEFN_COUNT 0x90      /* node function table: 0x27-byte records */
#define DEV_NODEFN_PTR 0x94
#define DEV_LINK_COUNT 0xcc        /* node link table: 0xd-byte records */
#define DEV_LINK_PTR 0xd0
#define DEV_ALT_COUNT 0xf4         /* alternative device table: 0x16-byte records */
#define DEV_ALT_PTR 0xf8

#define EXT_SIZE 0x15
#define EXT_CODE 0x14
#define PINFN_SIZE 0x10
#define NODEFN_SIZE 0x27
#define LINK_SIZE 0x0d
#define ALT_SIZE 0x16

/* Device description fields (relative to DEV_INFO_PTR) */
#define INFO_VALID 0x00            /* u16, 0 = device not supported */
#define INFO_NODE_BASE 0x08        /* u8  */
#define INFO_PIN_COUNT 0x0c        /* u16 */
#define INFO_EXTRA_A 0x0e          /* u8  */
#define INFO_EXTRA_B 0x0f          /* u8  */
#define INFO_EXTRA_C 0x10          /* u8  */
#define INFO_FAMILY 0x11           /* u8: 8 and 9 are the virtual devices */
#define INFO_DEFAULT_EXT 0x1c      /* u8  */
#define INFO_MAX_TERMS 0x25        /* u16: product terms a frame starts with */
#define INFO_TERM_LINES 0x27       /* u16 */
#define INFO_INPUT_LINES 0x29      /* u16 */
#define INFO_EXTRA_SYMBOLS 0x2c    /* u16 */

/* Pin function record */
#define PINFN_LINE 0x00            /* u16 */
#define PINFN_PIN 0x02             /* u16 */
#define PINFN_EXT 0x04             /* u8  */
#define PINFN_TYPE 0x05            /* u8  */
#define PINFN_VARIANT 0x06         /* u8  */
#define PINFN_ARCH_A 0x07          /* u8  */
#define PINFN_ARCH_B 0x08          /* u8  */

/* Node function record */
#define NODEFN_LINE 0x00           /* u16 */
#define NODEFN_PIN 0x02            /* u16 */
#define NODEFN_EXT 0x04            /* u8  */
#define NODEFN_POLARITY 0x05       /* u8  */
#define NODEFN_GROUP 0x0e          /* u32 */
#define NODEFN_TERMS 0x1f          /* u16 */
#define NODEFN_XOR 0x21            /* u8  */
#define NODEFN_DEFAULT 0x22        /* u8  */
#define NODEFN_HAS_DEFAULT 0x23    /* u8  */
#define NODEFN_SHARING 0x24        /* u8  */
#define NODEFN_ACTIVE 0x25         /* u8  */
#define NODEFN_ALT_STEP 0x26       /* u8  */

/* Node link record */
#define LINK_LINE 0x00             /* u16 */
#define LINK_PIN 0x02              /* u16 */
#define LINK_EXT 0x05              /* u16 */
#define LINK_GROUP 0x07            /* u16 */
#define LINK_MEMBER 0x09           /* u8  */
#define LINK_CLASS 0x0b            /* u8  */

/* Alternative device record */
#define ALT_CONDITION 0x14
#define ALT_PRIORITY 0x15

/* Iterator over the node link table */
typedef struct {
    int member;        /* current member of the group, -1 before the first */
    int group_start;   /* link record of the node function itself */
    uint8_t ext;       /* extension, pin and line of the node function */
    uint16_t pin;
    uint16_t line;
    uint8_t link_class;
    uint16_t group;    /* group of the current link record */
    uint8_t self;      /* member number of the node function in that group */
} link_iter_t;

/* A frame: the product terms of one equation.  The hex numbers are the
 * offsets of the fields in the frame header of the CM file. */
typedef struct {
    uint16_t flags;        /* 0x00 */
    uint16_t reserved;     /* 0x02 */
    uint16_t var_count;    /* 0x04 */
    uint16_t row_count;    /* 0x06 */
    uint16_t has_xor;      /* 0x08 */
    uint16_t max_terms;    /* 0x0a */
    uint16_t *rows;
    uint16_t *cur_true;    /* maps of the row being built */
    uint16_t *cur_used;
    uint16_t *cur_out;
    uint32_t row_words;    /* three maps */
    uint32_t total_words;
} frame_t;

/* Row flag bits kept in word 0 of the "used" map */
#define ROW_MORE 0x0001        /* another row follows */
#define ROW_XOR_SPLIT 0x0002   /* last row of the first XOR operand */
#define ROW_BAD 0x0004
/* Bits 0 and 1 of word 1 of the "used"/"true" maps stand for the constants */
#define BIT_FALSE 0
#define BIT_TRUE 1

/* Result of comparing two rows */
typedef struct {
    uint8_t keep_first;
    int diff_bit;
    uint8_t relation;      /* 0 none, 1 equal, 2 one covers the other, 3 adjacent */
} row_compare_t;

/* -------------------------------------------------------------------------
 * State shared between the parts (each part resets what it defines)
 * ------------------------------------------------------------------------- */

/* expand.c: options */
extern const char *expand_args[10];     /* copy of the phase argv */
extern const char *expand_mode;         /* argv[7]: "1" for virtual devices */
extern int expand_error_count;
extern int expand_keep_xor;             /* 'kx' flag */

/* expand.c: intermediate files */
extern FILE *expand_in;
extern FILE *expand_out;
extern char expand_in_name[16];         /* "CBXXXXXX" */
extern char expand_out_name[16];        /* "CMXXXXXX" */
extern uint8_t expand_header[CF_HEADER_SIZE];
extern uint8_t *expand_header_extra;
extern char *expand_device_name;        /* inside expand_header */

/* expand.c: symbols */
extern symbol_t *expand_symbols;
extern int expand_symbol_count;
extern int expand_symbol_alloc;
extern symbol_t *expand_extra_symbols;  /* generated for unused node functions */
extern int expand_extra_count;
extern int expand_extra_max;

/* expand.c: current equation */
extern symbol_t *expand_cur_sym;
extern unsigned expand_cur_index;
extern node_t *expand_expr;
extern int expand_line;
extern int expand_invert_output;
extern int expand_min_level;

/* expand_tree.c: operand kinds of the 25 operator forms */
extern const uint8_t expand_form_left[FORM_COUNT];
extern const uint8_t expand_form_right[FORM_COUNT];

/* expand_device.c: device library and usage maps */
extern FILE *expand_lib_file;
extern uint8_t expand_lib_entry[DL_ENTRY_SIZE];
extern uint8_t *expand_dev;
extern size_t expand_dev_size;
extern uint16_t *expand_conflict_map;
extern uint16_t *expand_nodefn_default;
extern uint16_t *expand_pin_foreign;
extern size_t expand_conflict_words, expand_nodefn_words, expand_pin_words;

/* -------------------------------------------------------------------------
 * Bit maps: arrays of 16-bit words whose word 0 is a header, bit i lives in
 * word 1 + i / 16.
 * ------------------------------------------------------------------------- */
static inline int map_test(const uint16_t *map, size_t words, int bit)
{
    int word = (bit >> 4) + 1;
    if (map == NULL || word < 0 || (size_t)word >= words) {
        return 0;
    }
    return (map[word] >> (bit & 15)) & 1;
}

static inline void map_set(uint16_t *map, size_t words, int bit)
{
    int word = (bit >> 4) + 1;
    if (map != NULL && word >= 0 && (size_t)word < words) {
        map[word] |= (uint16_t)(1u << (bit & 15));
    }
}

static inline void symbol_name(const symbol_t *sym, char *out, size_t size)
{
    snprintf(out, size, "%.*s", SYMBOL_NAME_SIZE, sym->name);
}

/* -------------------------------------------------------------------------
 * Device image access.  Offsets are checked against the size of the image;
 * bytes outside it read as 0.  A table index of -1 deliberately reads the
 * bytes in front of the table.
 * ------------------------------------------------------------------------- */
static inline unsigned dev_u8(long offset)
{
    if (expand_dev == NULL || offset < 0 || (size_t)offset >= expand_dev_size) {
        return 0;
    }
    return expand_dev[offset];
}

static inline unsigned dev_u16(long offset)
{
    return dev_u8(offset) | (dev_u8(offset + 1) << 8);
}

static inline uint32_t dev_u32(long offset)
{
    return (uint32_t)dev_u16(offset) | ((uint32_t)dev_u16(offset + 2) << 16);
}

static inline long dev_info(int field)
{
    return (long)dev_u32(DEV_INFO_PTR) + field;
}

static inline long dev_pinfn(int index, int field)
{
    return (long)dev_u32(DEV_PINFN_PTR) + (long)index * PINFN_SIZE + field;
}

static inline long dev_nodefn(int index, int field)
{
    return (long)dev_u32(DEV_NODEFN_PTR) + (long)index * NODEFN_SIZE + field;
}

static inline long dev_link(int index, int field)
{
    return (long)dev_u32(DEV_LINK_PTR) + (long)index * LINK_SIZE + field;
}

/* NUL-terminated text inside the device image */
static inline void dev_string(long offset, char *out, size_t size)
{
    size_t n = 0;
    for (; n + 1 < size; n++) {
        unsigned c = dev_u8(offset + (long)n);
        if (c == 0) {
            break;
        }
        out[n] = (char)c;
    }
    out[n] = '\0';
}

static inline int is_virtual_device(void)
{
    return expand_mode != NULL && expand_mode[0] == '1';
}

/* -------------------------------------------------------------------------
 * Node classification
 * ------------------------------------------------------------------------- */

/* Operator of a node type (OP_AND, OP_XOR, OP_OR) or 0 */
static inline int op_of(int type)
{
    if (type >= OP_AND && type < OP_AND + FORM_COUNT) {
        return OP_AND;
    }
    if (type >= OP_XOR && type < OP_XOR + FORM_COUNT) {
        return OP_XOR;
    }
    if (type >= OP_OR && type < OP_OR + FORM_COUNT) {
        return OP_OR;
    }
    return 0;
}

static inline operand_kind_t left_kind(const node_t *n)
{
    return (operand_kind_t)expand_form_left[n->type - op_of(n->type)];
}

static inline operand_kind_t right_kind(const node_t *n)
{
    return (operand_kind_t)expand_form_right[n->type - op_of(n->type)];
}

/* Left operand is a sub-tree */
static inline int has_left_node(const node_t *n)
{
    return op_of(n->type) != 0 && left_kind(n) == K_NODE;
}

/* Right operand is a sub-tree */
static inline int has_right_node(const node_t *n)
{
    if (n->type == NODE_NOT) {
        return 1;
    }
    return op_of(n->type) != 0 && right_kind(n) == K_NODE;
}

/* `op` with at least one sub-tree operand */
static inline int is_op_with_node(int type, int op)
{
    if (op_of(type) != op) {
        return 0;
    }
    int form = type - op;
    return form <= 4 || form >= 21;
}

/* Type of the node an operand points to; 0 when the operand holds a small
 * value (a constant) instead of a pointer. */
static inline int operand_type(operand_t o)
{
    if (o.raw < 0x10000) {
        return 0;
    }
    return *(const uint8_t *)o.node;
}

/* What a tree is when used as an operand.  A constant node whose value is
 * neither 0 nor 1 counts as a sub-tree. */
static inline operand_kind_t kind_of(const node_t *n)
{
    if (n == NULL) {
        return K_NODE;
    }
    int negated = (n->flags & NODE_NEGATED) != 0;
    if (n->type == NODE_VAR) {
        return negated ? K_NVAR : K_VAR;
    }
    if (n->type == NODE_CONST) {
        if (n->right.raw == (uintptr_t)negated) {
            return K_ZERO;
        }
        if (n->right.raw == (uintptr_t)!negated) {
            return K_ONE;
        }
    }
    return K_NODE;
}

static inline int is_leaf_kind(operand_kind_t k)
{
    return k != K_NODE;
}

static inline void toggle_negation(node_t *n)
{
    if (n != NULL) {
        n->flags ^= NODE_NEGATED;
    }
}

static inline int is_field_symbol(const symbol_t *sym)
{
    int kind = sym->kind & 0x0f;
    return kind == SYM_FIELD || kind == SYM_FIELD2;
}

/* -------------------------------------------------------------------------
 * Functions
 * ------------------------------------------------------------------------- */

/* expand.c: messages, files, memory */
void expand_progress_tick(void);
FILE *expand_open_file(const char *path, const char *mode);
void expand_report_error(int code, const char *detail);
void expand_fatal_error(int code, const char *detail);
void expand_report_symbol_error(int code, const symbol_t *sym);
void expand_report_pin_error(int code, const symbol_t *sym);
void expand_report_symbol_warning(int code, const symbol_t *sym);
void *expand_alloc_tracked(size_t size, const char *context);

/* expand_input.c */
void expand_input_reset(void);
void expand_read_section_header(void);
void expand_stacks_alloc(void);
int expand_read_equation(void);

/* expand_device.c */
void expand_device_reset(void);
void expand_open_library(void);
void expand_load_device(const char *device);
void expand_alloc_usage_maps(void);
int expand_bind_symbols(char *alt_name, size_t alt_size);
int expand_find_line_sharer(const symbol_t *sym);
void expand_set_polarity_states(void);
void expand_apply_polarity(void);

/* expand_tree.c */
void expand_tree_reset(void);
node_t *expand_new_node_of(int type, int flags);
void expand_fold_operands(node_t *right, node_t *left, node_t *op_node);
void expand_fold_tree(node_t *n);
void expand_equation(void);
void expand_tree_release(node_t *expr);

/* expand_frame.c */
void expand_frame_reset(void);
void expand_write_section_header(void);
void expand_write_symbols(void);
void expand_write_marker(unsigned kind);
void expand_build_frame(const node_t *expr, int preset);

#endif /* LINCUPL_EXPAND_INT_H */
