/*
 * fusemap.h - CUPL output generation phase (last compiler phase, fusemap_run).
 *
 * Declarations shared by the modules of the phase.  See fusemap.c for an
 * overview.
 */
#ifndef CUPLC_NATIVE_H
#define CUPLC_NATIVE_H

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
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include "cfile.h"
#include "devlib.h"
#include "phase.h"

/* Everything with external linkage carries a cuplc_ prefix at link time so
 * that the phases can live in one executable; the source uses short names. */
#define g                   cuplc_state
#define status_printf       cuplc_status_printf
#define stream_printf       cuplc_stream_printf
#define trace_printf        cuplc_trace_printf
#define report_error        cuplc_report_error
#define report_warning      cuplc_report_warning
#define report_symbol_error cuplc_report_symbol_error
#define report_from_to_error cuplc_report_from_to_error
#define fatal_error         cuplc_fatal_error
#define abort_phase         cuplc_abort_phase
#define alloc_tracked       cuplc_alloc_tracked
#define alloc_zeroed        cuplc_alloc_zeroed
#define free_tracked        cuplc_free_tracked
#define dup_string          cuplc_dup_string
#define open_file           cuplc_open_file
#define close_file          cuplc_close_file
#define read_char           cuplc_read_char
#define remove_file         cuplc_remove_file
#define file_exists         cuplc_file_exists
#define check_write         cuplc_check_write
#define out_printf          cuplc_out_printf
#define out_putc            cuplc_out_putc
#define out_puts            cuplc_out_puts
#define make_path           cuplc_make_path
#define source_dir          cuplc_source_dir
#define creation_time_text  cuplc_creation_time_text
#define find_symbol         cuplc_find_symbol
#define find_node_variant   cuplc_find_node_variant
#define find_extension_pair cuplc_find_extension_pair
#define print_banner        cuplc_print_banner
#define print_banner_with   cuplc_print_banner_with
#define header_string       cuplc_header_string
#define set_fuse            cuplc_set_fuse
#define get_fuse            cuplc_get_fuse
#define terms_before_split  cuplc_terms_before_split
#define text_set_fuse       cuplc_text_set_fuse

/* -------------------------------------------------------------------------
 * Limits
 * ------------------------------------------------------------------------- */
#define SYM_NAME_SIZE CF_NAME_SIZE
#define SYM_EXT_SIZE CF_EXT_SIZE

/* -------------------------------------------------------------------------
 * Option flags, one bit per option letter
 * ------------------------------------------------------------------------- */
enum {
    OPT_LIST = 0x0002,        /* l, q: errors are also written to <design>.lst */
    OPT_DOC = 0x0004,         /* x: documentation file <design>.doc */
    OPT_FUSE_PLOT = 0x0008,   /* f: fuse plot appended to the .doc file */
    OPT_ABSOLUTE = 0x0010,    /* a, s: absolute file <design>.abs for the simulator */
    OPT_G = 0x0020,           /* g */
    OPT_PLA = 0x0040,         /* b, zq: Berkeley PLA file / fitter input */
    OPT_PDIF = 0x0080,        /* p: P-CAD PDIF symbol file */
    OPT_JEDEC = 0x0100,       /* j: JEDEC file */
    OPT_HL = 0x0200,          /* i: HL (Signetics IFL) download file */
    OPT_HEX = 0x0400,         /* h: ASCII-hex download file */
    OPT_NO_MERGE = 0x1000,    /* r: do not merge identical product terms */
    OPT_D = 0x2000,           /* d */
    OPT_N = 0x4000,           /* n */
    OPT_PALASM = 0x8000       /* c: PALASM .pds file */
};

/* -------------------------------------------------------------------------
 * Error numbers.  Numbers below 1000 index k_error_text; larger numbers are
 * internal consistency errors and print "program error".
 * ------------------------------------------------------------------------- */
enum {
    ERR_PROGRAM = 0,
    ERR_OPEN = 1,
    ERR_INVALID_FILE = 2,
    ERR_LIB_ACCESS_KEY = 3,
    ERR_LIB_INTERFACE = 4,
    ERR_BAD_LIBRARY = 5,
    ERR_TOO_MANY_PTERMS = 6,
    ERR_DOWNLOAD_FORMAT = 7,
    ERR_PIN_NOT_INPUT = 8,
    ERR_NO_NAME = 9,
    ERR_DISK_WRITE = 10,
    ERR_OUT_OF_MEMORY = 11,
    ERR_DISK_READ = 12,
    ERR_PINNODE_CONFLICT = 13,
    ERR_UNKNOWN_EXTENSION = 14,
    ERR_LOCAL_FEEDBACK = 15,
    ERR_EXPANDER_PTERMS = 16,
    ERR_GLOBAL_FEEDBACK = 17,
    ERR_XILINX_SYMBOL = 18,
    ERR_XILINX_MAP = 19,
    ERR_MACRO_SYMBOL = 20,
    ERR_OED_FILE = 21,
    ERR_UNSUPPORTED_EXTENSION = 22,
    ERR_DECLARE_COUNT = 23,
    ERR_TOO_MANY_XOR = 24,
    ERR_NOT_SUPPORTED = 25
};

/* -------------------------------------------------------------------------
 * Intermediate file (CC<pid>, written by minimize; see cfile.h)
 * ------------------------------------------------------------------------- */

/* One product-term list.  On disk: a frame header followed by nterms terms.
 * A term is three groups of group_words 16-bit words:
 *   out  - which symbols the term drives (the first term's first set bit
 *          names the symbol the equation belongs to)
 *   care - symbols that appear in the product
 *   val  - symbols that appear uncomplemented
 * Word 0 of each group holds flags; the bit of symbol i is bit (i & 15) of
 * word 1 + (i >> 4). */
typedef struct equation {
    uint16_t flags;          /* header +0x00: EQ_xxx */
    uint16_t or_array;       /* header +0x02: OR array the list was assigned to */
    uint16_t nvars;          /* header +0x04: number of symbols covered by a group */
    uint16_t nterms;         /* header +0x06 */
    uint16_t w8;             /* header +0x08 */
    uint16_t max_terms;      /* header +0x0a */
    uint16_t *terms;         /* nterms * term_words words */
    int group_words;         /* term_words / 3 */
    int term_words;
    int total_words;         /* nterms * term_words as read */
    int symbol;              /* index of the owning symbol */
    struct equation *next;   /* next list sharing the same OR array */
} equation_t;

enum {
    EQ_END_OF_FILE = CF_FRAME_END_OF_FILE,
    EQ_END_OF_DEVICE = CF_FRAME_END_OF_DESIGN,
    EQ_ACTIVE_LOW = 0x1000
};

/* Flags in word 0 of a term's care group */
enum {
    TERM_MORE = 0x0001,
    TERM_SPLIT = 0x0002,     /* last term of the first half of a split array */
    TERM_DELETED = 0x0004
};

#define TERM_OUT(eq, t)  ((eq)->terms + (size_t)(t) * (size_t)(eq)->term_words)
#define TERM_CARE(eq, t) (TERM_OUT(eq, t) + (eq)->group_words)
#define TERM_VAL(eq, t)  (TERM_OUT(eq, t) + 2 * (eq)->group_words)

static inline int term_bit(const uint16_t *group, int i)
{
    return (group[1 + (i >> 4)] >> (i & 15)) & 1;
}
static inline void term_set_bit(uint16_t *group, int i)
{
    group[1 + (i >> 4)] = (uint16_t)(group[1 + (i >> 4)] | (1u << (i & 15)));
}
static inline void term_clear_bit(uint16_t *group, int i)
{
    group[1 + (i >> 4)] = (uint16_t)(group[1 + (i >> 4)] & ~(1u << (i & 15)));
}

/* Symbol table entry (cf_symbol_t on disk) */
typedef struct symbol {
    char name[SYM_NAME_SIZE + SYM_EXT_SIZE + 2]; /* +0x00 name, +0x20 extension (+ a guard) */
    uint16_t pin;            /* +0x2a: pin or node number */
    uint8_t type;            /* +0x2c: low nibble = SYM_xxx */
    uint8_t flags;           /* +0x2d: SYMF_xxx */
    uint8_t b2e[4];          /* +0x2e */
    uint8_t b32;             /* +0x32 */
    uint8_t b33;             /* +0x33 */
    int16_t pin_index;       /* +0x34: record in the device pin section, or -1 */
    int16_t node_index;      /* +0x36: record in the device node section, or -1 */
    int16_t or_index;        /* +0x38: node record of the OR array, or -1 */
    int16_t ext_code;        /* +0x3a: extension number */
    equation_t *eq;          /* product-term list (not in the file) */
} symbol_t;

#define SYM_EXT(s) ((s)->name + SYM_NAME_SIZE)

enum {
    SYMF_INVERTED = 0x10,
    SYMF_20 = 0x20,
    SYMF_40 = 0x40,
    SYMF_INPUT = 0x80
};

/* -------------------------------------------------------------------------
 * Device library (.dl).  The layout of the file, its directory and the
 * section headers of a device record is described in devlib.h.  The record
 * is kept as read and accessed through the little-endian helpers, since the
 * phase also patches it in place.  Offsets below are byte offsets within a
 * record.
 * ------------------------------------------------------------------------- */

enum {
    SEC_ATTR = 3,       /* 1 x 152: device attributes */
    SEC_4 = 4,          /* n x 21 */
    SEC_5 = 5,          /* n x 40 */
    SEC_PIN = 6,        /* n x 16: input columns of the AND array */
    SEC_NODE = 7,       /* n x 39: product-term rows and macrocell fuses */
    SEC_8 = 8,          /* n x 27 */
    SEC_INIT = 9,       /* n x 9: fuse ranges with a non-default initial value */
    SEC_EXT_PAIR = 10,  /* n x 13: macrocell configurations by extension pair */
    SEC_OUTPUT = 11,    /* n x 24: fitter command line items */
    SEC_12 = 12         /* n x 22 */
};

static inline unsigned rd8(const uint8_t *p) { return p[0]; }

/* Device attributes (section 3) */
enum {
    ATTR_NPINS = 0x08,          /* u8 */
    ATTR_NNODES = 0x0c,         /* u16 */
    ATTR_NEXTRA1 = 0x0e,        /* u8 */
    ATTR_NEXTRA2 = 0x0f,        /* u8 */
    ATTR_NEXTRA3 = 0x10,        /* u8 */
    ATTR_TYPE = 0x11,           /* u8: DEVT_xxx */
    ATTR_PLA_FORMAT = 0x12,     /* u8: 0 = none, else fitter input format */
    ATTR_DEFAULT_FUSE = 0x13,   /* u8 */
    ATTR_NFUSES = 0x15,         /* u32 */
    ATTR_W19 = 0x19,            /* u16 */
    ATTR_NROWS = 0x1d,          /* u16: number of product-term rows */
    ATTR_NOR_ARRAYS = 0x27,     /* u16 */
    ATTR_DOWNLOAD = 0x2b,       /* u8: DL_xxx */
    ATTR_B30 = 0x30,            /* u8 */
    ATTR_UES_BITS = 0x3d,       /* u8 */
    ATTR_UES_FUSE = 0x3e,       /* u32 */
    ATTR_FITTER = 0x43,         /* char[] */
    ATTR_FITTER_ARG = 0x50,     /* char[] */
    ATTR_PART_NAME = 0x84       /* char[] */
};

enum {
    DEVT_PAL = 0,
    DEVT_1 = 1,
    DEVT_2 = 2,
    DEVT_IFL = 3,
    DEVT_PROM = 4,
    DEVT_5 = 5,
    DEVT_7 = 7,
    DEVT_8 = 8,
    DEVT_VIRTUAL = 9
};

/* Pin records (section 6) */
enum {
    PIN_W0 = 0x00,              /* u16 */
    PIN_NUMBER = 0x02,          /* u16 */
    PIN_EXT = 0x04,             /* u8: extension number of the feedback path */
    PIN_KIND = 0x05,            /* u8 */
    PIN_B6 = 0x06,              /* u8 */
    PIN_TRUE_COL = 0x07,        /* u8: column of the true fuse in a row */
    PIN_COMP_COL = 0x08,        /* u8: column of the complement fuse */
    PIN_CFG_VALUE = 0x0a,       /* u8 */
    PIN_CFG_BITS = 0x0b,        /* u8 */
    PIN_CFG_FUSE = 0x0c,        /* u32 */
    PIN_REC_SIZE = 0x10
};

/* Node records (section 7) */
enum {
    NODE_ARRAY = 0x00,          /* u16: OR array number */
    NODE_NUMBER = 0x02,         /* u16: pin or node number */
    NODE_EXT = 0x04,            /* u8: extension number */
    NODE_KIND = 0x05,           /* u8 */
    NODE_INVERT = 0x06,         /* u8 */
    NODE_SEL_VALUE = 0x07,      /* u16 */
    NODE_CFG_VALUE = 0x09,      /* u8 */
    NODE_ROW_FUSE = 0x0a,       /* u32: first fuse of the first product term */
    NODE_OR_FUSE = 0x0e,        /* u32 */
    NODE_CFG_FUSE = 0x12,       /* u32 */
    NODE_PAIR_FUSE = 0x16,      /* u32 */
    NODE_ROW_WIDTH = 0x1b,      /* u8: AND-array fuses per product term */
    NODE_OR_WIDTH = 0x1c,       /* u8: OR-array fuses per product term */
    NODE_CFG_BITS = 0x1d,       /* u8 */
    NODE_PAIR_BITS = 0x1e,      /* u8 */
    NODE_MAX_TERMS = 0x1f,      /* u16 */
    NODE_SPLIT_TERMS = 0x21,    /* u8: terms in the first half of a split array */
    NODE_B24 = 0x24,            /* u8 */
    NODE_B25 = 0x25,            /* u8 */
    NODE_B26 = 0x26,            /* u8 */
    NODE_REC_SIZE = 0x27
};

/* Extension pair records (section 10) */
enum {
    PAIR_W0 = 0x00,             /* u16 */
    PAIR_NUMBER = 0x02,         /* u16 */
    PAIR_EXT = 0x05,            /* u16 */
    PAIR_GROUP = 0x07,          /* u16 */
    PAIR_SELECT = 0x09,         /* u8 */
    PAIR_VALUE = 0x0c,          /* u8 */
    PAIR_REC_SIZE = 0x0d
};

typedef struct {
    FILE *fp;
    uint8_t *header;            /* 199 bytes */
    const uint8_t *dir_entry;   /* 26 bytes, inside the intermediate header */
    uint8_t *rec;               /* device record */
    size_t rec_size;
} device_lib_t;

/* Entry of the OR-array table */
typedef struct {
    int nterms;                 /* product terms needed by all lists */
    equation_t *eq;             /* first list */
    symbol_t *sym;              /* symbol of the first list */
} or_array_t;

/* -------------------------------------------------------------------------
 * Phase state.  One object, cleared at the start of fusemap_run.
 * ------------------------------------------------------------------------- */

struct cuplc_state {
    jmp_buf abort_jmp;              /* target of abort_phase() */
    const char *args[10];           /* copy of the phase argv */
    int error_count;
    unsigned options;               /* OPT_xxx */
    int quiet_format;               /* 'q' flag, compiler-style messages */
    int opt_edif;                   /* 't' flag */
    int opt_xnf;                    /* 'zx' flag */
    int opt_fitter;                 /* 'zq' flag */
    int opt_share;                  /* 'ks' flag */
    const int *line_ptr;            /* source line for messages (always NULL) */
    long start_time;

    FILE *lst_fp;                   /* <design>.lst, appended */
    char *lst_name;
    FILE *doc_fp;
    char *doc_name;
    FILE *abs_fp;
    char *abs_name;

    /* intermediate file */
    char temp_name[16];             /* "CC" + pid */
    FILE *temp_fp;
    uint8_t hdr[CF_HEADER_SIZE];
    char *hdr_strings;              /* Name, Partno, ... NUL separated */
    size_t hdr_strings_len;
    int nsyms;
    symbol_t *syms;
    char *device_name;              /* points into hdr */
    char *design_name;              /* points into hdr */
    char *format_text;              /* points into hdr: download formats wanted by the source */

    device_lib_t lib;

    /* devices that need special handling, selected by name */
    int dev_f7024;
    int dev_xl78c800;
    int dev_ep312;                  /* ep312 / ep324 */
    int dev_ep1800;
    int dev_max5;
    int dev_v5000;
    int dev_v2500b;
    int dev_p22v10s;

    /* OR arrays */
    int nor_arrays;
    or_array_t *or_arrays;
    int too_many_terms;
    int merged_terms;
    int or_terms_used;
    int expander_free;

    /* fuse map */
    int map_built;
    int is_prom_like;               /* device type 0xff (p14r21) */
    uint8_t *fuses;                 /* one bit per fuse */
    size_t fuses_size;
    symbol_t **input_sym_by_pin;
    symbol_t **node_sym_by_pin;
    symbol_t **output_sym_by_pin;
    int sym_by_pin_count;
    char *pla_text;                 /* '0'/'1' image used for device type 0xff */
    int row_limit;
    uint32_t *row_list;             /* first fuses of the rows written so far */
    int row_count;
};

extern struct cuplc_state g;

/* -------------------------------------------------------------------------
 * Device record access
 * ------------------------------------------------------------------------- */
static inline unsigned sec_count(int sec) { return rd16(g.lib.rec + (size_t)sec * DL_SECTION_HEADER_SIZE + 4); }
static inline unsigned sec_rec_size(int sec) { return rd16(g.lib.rec + (size_t)sec * DL_SECTION_HEADER_SIZE + 6); }
static inline uint8_t *sec_data(int sec) { return g.lib.rec + rd32(g.lib.rec + (size_t)sec * DL_SECTION_HEADER_SIZE + 8); }

static inline uint8_t *attr_ptr(int off) { return sec_data(SEC_ATTR) + off; }
static inline unsigned attr8(int off) { return rd8(attr_ptr(off)); }
static inline unsigned attr16(int off) { return rd16(attr_ptr(off)); }
static inline uint32_t attr32(int off) { return rd32(attr_ptr(off)); }

static inline uint8_t *pin_rec(int i) { return sec_data(SEC_PIN) + (ptrdiff_t)i * PIN_REC_SIZE; }
static inline unsigned pin8(int i, int off) { return rd8(pin_rec(i) + off); }
static inline unsigned pin16(int i, int off) { return rd16(pin_rec(i) + off); }
static inline uint32_t pin32(int i, int off) { return rd32(pin_rec(i) + off); }

static inline uint8_t *node_rec(int i) { return sec_data(SEC_NODE) + (ptrdiff_t)i * NODE_REC_SIZE; }
static inline unsigned node8(int i, int off) { return rd8(node_rec(i) + off); }
static inline unsigned node16(int i, int off) { return rd16(node_rec(i) + off); }
static inline uint32_t node32(int i, int off) { return rd32(node_rec(i) + off); }

static inline uint8_t *pair_rec(int i) { return sec_data(SEC_EXT_PAIR) + (ptrdiff_t)i * PAIR_REC_SIZE; }

static inline int npins(void) { return (int)sec_count(SEC_PIN); }
static inline int nnodes(void) { return (int)sec_count(SEC_NODE); }
static inline int device_type(void) { return (int)attr8(ATTR_TYPE); }

/* Number of pin/node numbers of the device: pins + nodes + extras */
static inline int pin_number_limit(void)
{
    return (int)(attr16(ATTR_NNODES) + attr8(ATTR_NPINS) + attr8(ATTR_NEXTRA1) + attr8(ATTR_NEXTRA2)
                 + attr8(ATTR_NEXTRA3));
}

static inline int sym_index(const symbol_t *s) { return (int)(s - g.syms); }

/* -------------------------------------------------------------------------
 * Runtime (fusemap.c)
 * ------------------------------------------------------------------------- */
#if defined(__GNUC__) || defined(__clang__)
#define PRINTF_LIKE(a, b) __attribute__((format(printf, a, b)))
#define NORETURN __attribute__((noreturn))
#else
#define PRINTF_LIKE(a, b)
#define NORETURN
#endif

void status_printf(const char *fmt, ...) PRINTF_LIKE(1, 2);
int stream_printf(FILE *fp, const char *fmt, ...) PRINTF_LIKE(2, 3);
void trace_printf(const char *fmt, ...) PRINTF_LIKE(1, 2);
void report_error(int code, const char *detail);
void report_warning(int code, const char *detail);
void report_symbol_error(int code, const symbol_t *sym);
void report_from_to_error(int code, const symbol_t *from, const symbol_t *to);
NORETURN void fatal_error(int code, const char *detail);
NORETURN void abort_phase(void);

void *alloc_tracked(size_t size, const char *context);
void *alloc_zeroed(size_t count, size_t size, const char *context);
void free_tracked(void *p);
char *dup_string(const char *s, const char *context);

FILE *open_file(const char *path, const char *mode);
void close_file(FILE *fp);
int read_char(FILE *fp);
void remove_file(const char *path);
int file_exists(const char *path);
void check_write(FILE *fp, const char *name);    /* fflush; "disk write error" when it failed */

/* Output to a file opened with open_file().  JEDEC files get CR LF line ends. */
void out_printf(FILE *fp, const char *fmt, ...) PRINTF_LIKE(2, 3);
void out_putc(int c, FILE *fp);
void out_puts(const char *s, FILE *fp);

char *source_dir(const char *path);
char *make_path(const char *dir, const char *path, const char *ext);
const char *creation_time_text(void);                                 /* ctime(&start_time) */

symbol_t *find_symbol(const char *name, const char *ext);
int find_node_variant(int kind, const symbol_t *sym);
int find_extension_pair(const symbol_t *sym, unsigned number, unsigned ext);

void set_fuse(uint32_t fuse, unsigned value);
int get_fuse(uint32_t fuse);
void text_set_fuse(uint32_t pos, unsigned value);

int terms_before_split(const equation_t *eq);
void print_banner(FILE *fp);
void print_banner_with(FILE *fp, const char *prefix);
const char *header_string(int index);

/* -------------------------------------------------------------------------
 * Modules
 * ------------------------------------------------------------------------- */
#define modules_reset       cuplc_modules_reset
#define select_cell_table   cuplc_select_cell_table
#define ep312_assign_terms  cuplc_ep312_assign_terms
#define max5_set_needed     cuplc_max5_set_needed
#define build_fuse_map      cuplc_build_fuse_map
#define write_jedec         cuplc_write_jedec
#define jed_reset           cuplc_jed_reset
#define row_state           cuplc_row_state
#define write_fuse_row      cuplc_write_fuse_row
#define read_fuse_row       cuplc_read_fuse_row
#define check_macrocell     cuplc_check_macrocell
#define find_node_by_ext    cuplc_find_node_by_ext
#define input_inverted      cuplc_input_inverted
#define v5000_build         cuplc_v5000_build
#define ep1800_check_array  cuplc_ep1800_check_array
#define v5000_check_feedback cuplc_v5000_check_feedback
#define dev_reset           cuplc_dev_reset
#define doc_open            cuplc_doc_open
#define doc_write_symbols   cuplc_doc_write_symbols
#define doc_write_totals    cuplc_doc_write_totals
#define doc_reset           cuplc_doc_reset
#define write_chip_diagram  cuplc_write_chip_diagram
#define write_palasm        cuplc_write_palasm
#define write_edif          cuplc_write_edif
#define write_pla           cuplc_write_pla
#define write_fitter_pla    cuplc_write_fitter_pla
#define write_pdif          cuplc_write_pdif
#define build_prom_map      cuplc_build_prom_map
#define write_pof           cuplc_write_pof
#define write_hex           cuplc_write_hex
#define write_prom_plot     cuplc_write_prom_plot
#define abs_open            cuplc_abs_open
#define abs_write           cuplc_abs_write
#define abs_end             cuplc_abs_end
#define sim_open            cuplc_sim_open
#define sim_write           cuplc_sim_write
#define write_fuse_plot     cuplc_write_fuse_plot
#define close_plot_file     cuplc_close_plot_file
#define pla_reset           cuplc_pla_reset
#define net_reset           cuplc_net_reset
#define dl_reset            cuplc_dl_reset
#define abs_reset           cuplc_abs_reset

void modules_reset(void);

/* Position of the product term being placed */
struct cuplc_row_state {
    int and_width;      /* AND-array fuses per term */
    int or_width;       /* OR-array fuses per term */
    uint32_t or_fuse;
    uint32_t fuse;      /* first fuse of the current term */
    uint32_t second;    /* first fuse of the second half of a split array */
    int stride;
    int placed;
    int text_mode;
};
extern struct cuplc_row_state row_state;

/* jedec.c */
enum { CELLS_EP312, CELLS_EP324, CELLS_V2500B };
void jed_reset(void);
void select_cell_table(int which, int lcc);
void ep312_assign_terms(void);
void max5_set_needed(int pin, int nterms);
void build_fuse_map(void);
void write_jedec(void);
void write_fuse_row(const uint8_t *row, uint32_t fuse, int count);
int read_fuse_row(uint8_t *row, uint32_t fuse, int count);
void check_macrocell(int symbol);
int find_node_by_ext(int symbol, unsigned ext);
int input_inverted(int symbol, int pin_index);

/* atv5000.c: device specific placement */
void dev_reset(void);
void v5000_build(void);
int ep1800_check_array(or_array_t *oa);
int v5000_check_feedback(void);

/* doc.c */
void doc_reset(void);
void doc_open(void);
void doc_write_symbols(void);
void doc_write_totals(void);
void write_chip_diagram(FILE *fp);
void write_fuse_plot(const char *ext, int kind);
void close_plot_file(void);

/* abs.c */
void abs_reset(void);
void abs_open(void);
void abs_write(void);
void abs_end(int code);
void sim_open(void);
void sim_write(void);

/* pla.c */
void pla_reset(void);
void write_pla(void);
void write_fitter_pla(int format);

/* prom.c */
void dl_reset(void);
void build_prom_map(void);
void write_pof(void);
void write_hex(void);
void write_prom_plot(FILE *fp);

/* netlist.c */
void net_reset(void);
void write_palasm(void);
void write_edif(void);
void write_pdif(void);

#endif /* CUPLC_NATIVE_H */
