/*
 * fit_types.h - data structures shared between the modules of the
 * ATF1502/ATF1504/ATF1508 fitter (fit1502, fit1504, fit1508).
 *
 * A field whose meaning is not established carries "guess" (meaning
 * inferred from a few uses) or "unknown" (only its width is known) in its
 * comment; such fields keep a provisional name made of a type letter and a
 * number (i30: an integer).  Nothing is written to disk as a raw struct.
 *
 * Conventions used by the fitter sources:
 *  - Types: FitCamelCase.
 *  - Functions: <module>_<verb>_<object>() in lower_snake_case (list_,
 *    util_, lex_, fcttree_, pterm_, entity_, var_, lib_, tt2_, tt3_, jed_,
 *    dev_, report_, fit_, place_, route_, alloc_, xor_, factor_, esp_ ...).
 *  - Globals: g_<name>; constants and enum members: FIT_UPPER_CASE.
 *  - Device dependence goes through const FitDeviceParams *g_dev and the
 *    device tables, never through #ifdef: one binary serves the three
 *    devices.
 *  - All state is reset at the start of a run and all memory released at
 *    the end.
 *  - List positions returned by the find functions and taken by the delete
 *    functions are 1-based, 0 means "not found"; macrocell, block and pin
 *    numbers are 1-based.
 */
#ifndef FIT150X_FIT_TYPES_H
#define FIT150X_FIT_TYPES_H

#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Limits                                                              */
/* ------------------------------------------------------------------ */

#define FIT_NAME_MAX           0x100  /* name buffers: FitParam, FitVariable */
#define FIT_MC_PER_BLOCK       16     /* macrocells per logic block */
#define FIT_PT_PER_MC          5      /* product terms per macrocell */
#define FIT_PT_FUSES           80     /* fuses of one product term row */
#define FIT_PT_EXTRA_FUSES     16     /* second fuse row per product term */
#define FIT_MC_CFG_BITS        24     /* macrocell configuration string, default below */
#define FIT_MC_CFG_DEFAULT     "000111000000000101100110"
#define FIT_UIM_PER_BLOCK      40     /* block inputs routed through the UIM */
#define FIT_GLOBAL_SLOTS       6      /* global signal slots */
#define FIT_FREE_NAME          "--"   /* name of an unused slot */
#define FIT_MAX_BLOCKS         8
#define FIT_MAX_MACROCELLS     128
#define FIT_FIRST_ROUTE_LIMIT  0x19   /* initial value of the pass limit g_fit_route_limit */
#define FIT_LAST_ROUTE_LIMIT   0x28   /* largest value tried */

/* ------------------------------------------------------------------ */
/* base_list.c: growable array of pointers                             */
/*   list_create_sized / list_create (capacity 16)                     */
/*   list_add appends (capacity doubles when full)                     */
/*   list_destroy frees array and header, not the elements             */
/*   list_free_all calls free_fn(&item) on each element first          */
/* ------------------------------------------------------------------ */

typedef struct FitList {
    int    capacity;   /* allocated slots */
    int    count;      /* used slots */
    void **items;
} FitList;

typedef int  (*FitListEq)(void *item, void *key);      /* nonzero = match */
typedef int  (*FitListCmp)(void *a, void *b);          /* <0, 0, >0 */
typedef void (*FitListFree)(void **item_ref);          /* frees *item_ref, sets it to NULL */
typedef void *(*FitListCopy)(void *item);

/* ------------------------------------------------------------------ */
/* base_util.c: command parameter ("-name value")                      */
/*   param_new creates one, param_get returns a copy of the value.     */
/*   The job's parameter list is built by param_parse_command from the */
/*   command string; the first two positional words get the names "i"  */
/*   and "o".                                                          */
/* ------------------------------------------------------------------ */

#define FIT_PARAM_VALUE_MAX    2000   /* value field of a FitParam; the legacy mode uses FIT_NAME_MAX of it */

typedef struct FitParam {
    char name[FIT_NAME_MAX];
    char value[FIT_PARAM_VALUE_MAX];
} FitParam;

/* The two behaviours of the fitter.  The extended mode knows the BE and ASV
 * technologies, more options, a different .fit report and different
 * configuration bits; the legacy mode is the default.  g_fit_mode selects
 * one ($LINCUPL_FIT150X_MODE). */
enum FitMode {
    FIT_MODE_LEGACY = 0,
    FIT_MODE_EXTENDED = 1
};
#define FIT_EXT (g_fit_mode == FIT_MODE_EXTENDED)

/* Exit / error codes stored in g_exit_code and returned by main. */
enum FitExitCode {
    FIT_EXIT_OK              = 0,
    FIT_EXIT_MEMORY          = 3,      /* "Memory Full" (util_calloc) */
    FIT_EXIT_FILE_OPEN       = 5,      /* "Can not Open File" (util_fopen_checked) */
    FIT_EXIT_ABORT           = 6,      /* util_abort_job with code 0 */
    FIT_EXIT_NO_ENTITY       = 7,      /* "Not possible to read an entity" (main_fit_job) */
    FIT_EXIT_USAGE           = 0x65,   /* bad command line / parameter value */
    FIT_EXIT_NO_FIT          = 0x66,   /* "DESIGN FAIL TO FIT" (main_run_job) */
    FIT_EXIT_INPUT_OPEN      = 0xc9,   /* input file not found (input_read) */
    FIT_EXIT_INPUT_SYNTAX    = 0x12d,  /* reader failed (input_read) */
    FIT_EXIT_INPUT_FAILED    = 0x12e,  /* "Reading input file failed" (main_fit_job) */
    FIT_EXIT_FUSE_MAP        = 0x1fa,  /* "Fuse mapping fail" (fit_driver) */
    FIT_EXIT_JEDEC_WRITE     = 0x1fb   /* jed_write_file failed (fit_driver) */
};

/* longjmp values used with the job jmp_buf g_job_jmp */
enum FitJump {
    FIT_JUMP_ABORT     = 0,
    FIT_JUMP_FILE      = 0x32,
    FIT_JUMP_MEMORY    = 99
};

/* ------------------------------------------------------------------ */
/* base_lex.c: one global scanner shared by all text readers           */
/* ------------------------------------------------------------------ */

/* Token classes left in g_lex_token by lex_next_token.  Values
 * 1..max_keyword are the indices of the keyword table installed with
 * lex_set_keyword_table; the others below are the ones the tt2 property
 * handler tt2_property uses. */
enum FitLexToken {
    FIT_TOK_IDENT   = -100,   /* word that is not a keyword; text in g_lex_text */
    FIT_TOK_REAL    = -99     /* guess: tt2_read_var_list compares against -99 */
};

/* ------------------------------------------------------------------ */
/* base_fcttree.c: Boolean expression tree                             */
/*   fcttree_new (selector 'T'), fcttree_copy, fcttree_free_deferred   */
/*   (nodes are queued on a list that fcttree_gc_begin creates and     */
/*   fcttree_gc_end releases)                                          */
/* ------------------------------------------------------------------ */

/* The selector is an ASCII character. */
enum FitTreeSel {
    FIT_SEL_NOT   = '!',  /* 0x21 children: list of FitFctTree (one) */
    FIT_SEL_AND   = '*',  /* 0x2a children */
    FIT_SEL_OR    = '+',  /* 0x2b children */
    FIT_SEL_EQ    = '=',  /* 0x3d children (guess: equivalence / XNOR) */
    FIT_SEL_XOR   = '^',  /* 0x5e children */
    FIT_SEL_CONST = 'C',  /* 0x43 value in u.value */
    FIT_SEL_ID    = 'I',  /* 0x49 variable id in u.id (FitVariable.id) */
    FIT_SEL_SET   = 'S',  /* 0x53 u.children is a list of plain integers (guess: id set) */
    FIT_SEL_EMPTY = 'T'   /* 0x54 freshly created node */
};

typedef struct FitFctTree {
    int sel;                 /* enum FitTreeSel */
    union {
        FitList *children;   /* for ! * + = ^ (FitFctTree *) and S (integers) */
        int      id;         /* for I */
        int      value;      /* for C */
    } u;
    void *user;              /* copied through the callback of fcttree_copy; unknown use */
} FitFctTree;

/* ------------------------------------------------------------------ */
/* base_pterm.c: product term as two bit vectors                       */
/*   pterm_new_raw / pterm_new (one part), ptermio_new (input+output), */
/*   pterm_set_literal, pterm_build, pterm_to_string                   */
/* ------------------------------------------------------------------ */

typedef struct FitPTerm {
    unsigned *bits;   /* bit (var-1): 1 for literal '1' (pterm_set_literal) */
    unsigned *care;   /* bit (var-1): guess - set unless the literal is '-' */
} FitPTerm;

typedef struct FitPTermIO {      /* ptermio_new */
    FitPTerm in;      /* input part */
    FitPTerm out;     /* output part */
} FitPTermIO;

/* ------------------------------------------------------------------ */
/* base_var.c: a signal of an entity                                   */
/*   var_new, var_copy, var_free                                       */
/* ------------------------------------------------------------------ */

typedef struct FitVarExt {       /* present when has_ext == 1 */
    int w[4];                    /* unknown */
} FitVarExt;

typedef struct FitVariable {
    char       kind;                 /* argument of var_new; 1 and 2 get an attribute list */
    char       name[FIT_NAME_MAX];
    int        cls;                  /* 0 at creation; tt3_name_register_inputs stores 2
                                        (guess: class in/out/local) */
    int        id;                   /* number used by FIT_SEL_ID nodes and FitFct.var_id */
    int        polarity;             /* 1 at creation; copied with the variable (guess: active level) */
    FitList   *attrs;                /* kinds 1 and 2 only; items freed with free() (guess: strings) */
    int        has_ext;
    FitVarExt *ext;
    int        pin;                  /* -1 at creation (guess: assigned pin or node number) */
    int        aux;                  /* -1 at creation; unknown */
    int        weight;               /* ordering weight of an input in the equivalence check
                                        (min_equiv.c); never touched by create/copy */
} FitVariable;

/* ------------------------------------------------------------------ */
/* base_entity.c: design in memory                                     */
/* ------------------------------------------------------------------ */

/* One function of the entity: output variable and its expressions.
 * fct_new, fct_copy, fct_free */
typedef struct FitFct {
    int         var_id;   /* FitVariable.id of the defined signal */
    FitFctTree *tree[3];  /* guess - on-set, don't-care set, off-set */
    union {               /* one slot with two users; zeroed by fct_new, not copied by fct_copy */
        int      flags;   /* fan-in count stored and sorted on by the partition
                             (route_fct_weight_cmp) */
        FitList *reach;   /* ids reachable from the function, memoised by the cycle check
                             (tt2_check_cycles clears and releases it, fcttree_find_cycle
                             fills it) */
    };
} FitFct;

/* Kind tag of a netlist item (FitNetItem.kind).  Only the values tested in
 * the code are listed; other values below 13 use the FitInstance payload. */
enum FitGateKind {
    FIT_GATE_TFF      = 2,   /* output written as <name>.T  (tt3_name_register_inputs) */
    FIT_GATE_LATCH    = 4,   /* output written as <name>.L, enable .LE/.LH */
    FIT_GATE_DMUX     = 6,   /* "DMUX_GATE not supported" (netlist_find_instance) */
    FIT_GATE_LMUX     = 7,   /* "LMUX_GATE not supported" */
    FIT_GATE_TH       = 8,   /* output written as <name>.TH */
    FIT_GATE_ITH      = 9,   /* output written as <name>.TH */
    FIT_GATE_UNKNOWN  = 10,  /* payload FitUnknownGate, "UNKNOWN_GATE in NetList" */
    FIT_GATE_CELL13   = 13,  /* payload FitCell13 */
    /* every other kind that reaches tt3_name_register_inputs is written as
     * <name>.D */

    /* The rest of the enumeration, from the gate library parser (lib_parse.c:
     * lib_parse_file, lib_parse_pgate; library keyword in parentheses).  The
     * same values tag the gates of a library (FitLibGate.kind, lib.h). */
    FIT_GATE_COMB     = 1,   /* (GATE) combinational gate; library only: payload FitLibComb */
    FIT_GATE_DFF      = 3,   /* (DG) D flip-flop */
    FIT_GATE_JK       = 5,   /* (JKG) JK flip-flop */
    FIT_GATE_RS       = 11,  /* (RSG) RS flip-flop */
    FIT_GATE_WIRE     = 12,  /* no library keyword: feed-through made by lib_split_output and
                                techmap_latch_to_comb, payload FitInstance with only inst_name,
                                q (driven signal) and d (source signal) set */
    FIT_GATE_BUF      = 13,  /* (BUF) pad buffer = FIT_GATE_CELL13; FitCell13.buf_type is an
                                enum FitLibBufType */
    FIT_GATE_RSL      = 14   /* (RSL) RS latch; the mapper turns it into FIT_GATE_LATCH */
    /* TFF is (TG), LATCH (LG), DMUX (DMUXG), LMUX (LMUXG), TH (THG), ITH (ITHG),
     * UNKNOWN (UGATE). */
};

/* Sequential / tri-state cell.  instance_new, instance_copy; freed with a
 * plain free of the block (strings are not freed there). */
typedef struct FitInstance {
    char *name;         /* name of the gate (model) the cell is an instance of */
    char *inst_name;    /* instance name */
    char *clk;          /* clock / latch enable signal */
    int   clk_kind;     /* enum FitLibClock; tt3_name_register_inputs tests for 2 and 3 */
    char *ar;           /* asynchronous reset signal -> <name>.AR */
    int   ar_pol;       /* -1 at creation */
    char *ap;           /* asynchronous preset signal -> <name>.AP */
    int   ap_pol;       /* -1 at creation */
    char *ce;           /* clock enable signal -> <name>.CE */
    int   ce_pol;       /* -1 at creation */
    char *oe;           /* output enable signal -> <name>.OE */
    int   oe_pol;       /* -1 at creation */
    char *q;            /* output signal name (key of tt3_gate_kind_of_output,
                           netlist_find_instance) */
    char *q2;           /* second output name searched by netlist_find_instance;
                           J / S input in the library stage */
    int   q2_pol;       /* polarity of the J / S input (library stage) */
    char *q3;           /* third output name searched by netlist_find_instance;
                           K / R input in the library stage */
    int   q3_pol;       /* polarity of the K / R input (library stage) */
    char *mux_a;        /* multiplexer inputs and select (DMUX / LMUX cells) */
    char *mux_b;
    char *mux_sel;
    char *d;            /* data input signal (entity_var_is_node tries pin, d, d2) */
    char *d2;           /* alternative data input */
    char *pin;          /* pad signal -> <name>.PIN */
} FitInstance;

/* Pad buffer.  cell13_copy, cell13_free */
typedef struct FitCell13 {
    char *model;        /* name of the buffer gate the cell is an instance of */
    char *inst_name;    /* instance name */
    int   buf_type;     /* enum FitLibBufType; 1 at creation */
    char *in;
    char *out;
    char *pad;
    char *oe;
    int   oe_pol;       /* -1 at creation */
} FitCell13;

/* connection of an unknown gate; unknown_pin_copy */
typedef struct FitUnknownPin {
    int   dir;          /* 1 input, 0 output */
    char *name;         /* pin name */
    char *signal;       /* connected signal */
} FitUnknownPin;

/* unknown_gate_new, unknown_gate_copy, unknown_gate_free */
typedef struct FitUnknownGate {
    int      i00;          /* unknown; 1 for the XOR gates xor_rewrite_function makes */
    char    *model;        /* model (gate) name */
    char    *inst_name;    /* instance name */
    FitList *pins;         /* FitUnknownPin */
} FitUnknownGate;

/* netitem_copy, netitem_free */
typedef struct FitNetItem {
    int   kind;     /* enum FitGateKind */
    void *cell;     /* FitInstance, FitUnknownGate (kind 10) or FitCell13 (kind 13) */
} FitNetItem;

/* alias_copy, alias_free */
typedef struct FitAlias {
    char *name;
    char *target;   /* may be NULL */
} FitAlias;

/* netlist_new, netlist_copy, netlist_free */
typedef struct FitNetlist {
    FitList *items;    /* FitNetItem */
    FitList *aliases;  /* FitAlias */
} FitNetlist;

/* entity_new, entity_copy, entity_free, entity_clear, entity_shallow_assign.
 * Registered by name in the entity table (entity_table_find,
 * entity_table_store, entity_table_get); the design is entity "Fct". */
typedef struct FitEntity {
    char       *name;
    int         i04;       /* unknown */
    char       *lib_file;  /* library file the netlist was adapted to (techmap_adapt_netlist
                              skips the entity when it is the library's file) */
    int         n_in;      /* guess: number of inputs */
    int         n_out;     /* number of outputs (tt3_name_register_inputs increments it
                              with `outputs`) */
    int         n_local;   /* guess: number of internal variables */
    FitList    *inputs;    /* FitVariable (entity_has_input_id searches it by id) */
    FitList    *outputs;   /* FitVariable */
    FitList    *locals;    /* FitVariable */
    FitList    *fcts;      /* FitFct */
    FitNetlist *net;
    int         i2c;       /* unknown */
    int         i30;       /* unknown */
} FitEntity;

/* ------------------------------------------------------------------ */
/* dev.c, dev_tables.c: device description                             */
/* ------------------------------------------------------------------ */

enum FitDevice { FIT_DEV_1502 = 0, FIT_DEV_1504 = 1, FIT_DEV_1508 = 2 };

/* The numeric parameters of a device; the only ones the
 * device-independent code reads. */
typedef struct FitDeviceParams {
    int n_blocks;       /* 8 / 4 / 2 for 1508 / 1504 / 1502 */
    int n_macrocells;   /* 128 / 64 / 32 */
    int n_ded_inputs;   /* 4 dedicated input pins */
    int uim_width;      /* fuses per UIM multiplexer: 27 / 9 / 5 */
    int uim_max_cands;  /* 5 / 3 / 3: largest number of candidates of a node name
                           place_uim_pick decides by trial */
    int gclk0_mode;     /* mode bits of global clock 0 in the device configuration:
                           1; written 2 or 3 by jed_map_device_bits */
    int gclk1_mode;     /* the same for global clock 1: 2 */
    int gclk2_mode;     /* the same for global clock 2: 3 */
    const char *family; /* "1508" / "1504" / "1502" (dev_family_string) */
} FitDeviceParams;

/* Packages; the names are the strings used throughout the program. */
enum FitPackage {
    FIT_PKG_PLCC44,   /* 1502, 1504 */
    FIT_PKG_TQFP44,   /* 1502, 1504 */
    FIT_PKG_PLCC68,   /* 1504, 1508 */
    FIT_PKG_PLCC84,   /* 1504, 1508 */
    FIT_PKG_PQFP100,  /* 1504, 1508 */
    FIT_PKG_TQFP100,  /* 1504, 1508 */
    FIT_PKG_PQFP160,  /* 1508 */
    FIT_PKG_CBGA132   /* 1508BE */
};

/* ------------------------------------------------------------------ */
/* Fitter state (fit driver, placer, router, partition)                */
/* ------------------------------------------------------------------ */

/* One placement slot: a macrocell or a dedicated input pin.
 * place_create_slots makes n_macrocells + n_ded_inputs of them in the list
 * g_fit_slots. */
typedef struct FitSlot {
    char *signal;     /* placed signal name, FIT_FREE_NAME when empty (not owned at creation) */
    int   fct_id;     /* FitVariable.id of the function whose output drives the pin; 0 = none
                         (placer, router, partition) */
    int   block;      /* 1..n_blocks, 0 for dedicated inputs */
    int   macrocell;  /* 1..n_macrocells, 0 for dedicated inputs */
    int   pin;        /* package pin (dev_pin_of_macrocell) or dedicated pin (place_dedicated_pin) */
    char *cascade;    /* owned copy of FIT_FREE_NAME at creation; not "--" = the cell's terms are lent
                         to a neighbour (cascade): it offers no terms (place_slot_free_terms) */
    char *buried;     /* FIT_FREE_NAME at creation; name of the buried (feedback node) signal */
    char *foldback;   /* FIT_FREE_NAME at creation; foldback signal of the cell (costs one term), or "NA" */
    char *fn_kind;    /* FIT_FREE_NAME at creation; "reg" / "com": kind of the function in the cell */
    int   reserved_pad; /* pad reserved for JTAG, power down or VREF */
    int   sstl_input;  /* selected I/O input buffer, including bidirectional pads */
    int   polarity;   /* 1 for macrocells at creation; the fuse mapper stores the polarity of the
                         variable placed here (jed_map_slot) */
    /* The fields from cell_kind on are written by the fuse mapper
     * (jed_map_slot) and read by the report (report.c). */
    int   cell_kind;  /* enum FitGateKind of the cell in the macrocell (2 T, 4 latch ...) */
    int   fb_pol;     /* polarity of the buried function when the cell has two functions */
    int   pt_use[5];  /* use of the five product terms: 0 free, 1 logic, 2 foldback,
                         0x13 xor, 0x18 second function, 3 clock, 4 reset, 5 preset, 6 OE, 7 clock enable */
    int   clk_src;    /* clock source: 8, 9, 10 global clock 1..3; 0x11 / 0x12 constant */
    int   ar_src;     /* reset source: 4 global clear; 0x11 / 0x12 constant */
    int   oe_src;     /* OE source: 0xb..0x10 global OE 1..6, 0x11 always on, 0x12 off */
    FitInstance *inst; /* the register / tri-state cell placed here */
} FitSlot;

/* One block input of the UIM (list of lists g_fit_uim_slots, created by
 * place_create_uim_slots) or one global signal slot (created by
 * place_create_global_slots). */
typedef struct FitUimSlot {
    int   block;      /* 0-based block index */
    int   name_pos;   /* 1-based position of the routed node name in the block's name list
                         (place_uim_take_singles) */
    int   index;      /* 0..39 within the block */
    char *signal;     /* FIT_FREE_NAME when unused (owned) */
    int   sel_is_pin; /* 1 when sel_node is a pin node ("...p"); set by the report */
    char *sel_node;   /* FIT_FREE_NAME at creation (owned); node name the multiplexer selects
                         (route_commit) */
    int   i18;        /* 6 once the input is the decided candidate of a name
                         (place_uim_take_singles, place_uim_pick); never read */
} FitUimSlot;

/* Preassign property, -preassign / PROPERTY ATMEL preassign */
enum FitPreassign {
    FIT_PREASSIGN_KEEP   = 1,
    FIT_PREASSIGN_TRY    = 2,
    FIT_PREASSIGN_IGNORE = 3
};

/* Reason code a fitting pass hands back to the driver (third argument of
 * fit_drv_pass, tested by fit_driver).  Only the values the driver tests. */
enum FitPassResult {
    FIT_PASS_OK          = 0,
    FIT_PASS_RETRY_1     = 1,   /* with 2: retried through fit_drv_relax at most twice */
    FIT_PASS_RETRY_2     = 2,
    FIT_PASS_ROUTE_LIMIT = 3,   /* raise g_fit_route_limit (0x19..0x28) and try again */
    FIT_PASS_SWITCHED    = 4,   /* set by the driver after fit_drv_drop_node_preassign */
    FIT_PASS_RESTART     = 5,   /* set by the driver: preassign TRY dropped, start over */
    FIT_PASS_GIVE_UP     = 6,   /* set by the driver */
    FIT_PASS_TOO_BIG     = -1   /* turn logic doubling on and restart */
};

/* ------------------------------------------------------------------ */
/* jed.c: fuse image                                                   */
/*   Allocated by jed_alloc_map.  Every fuse row is a string of        */
/*   '0'/'1'.                                                          */
/* ------------------------------------------------------------------ */

typedef struct FitJedecMap {
    char *pt[FIT_MAX_MACROCELLS][FIT_PT_PER_MC];        /* 80 fuses each, all '1' at start */
    char *pt_extra[FIT_MAX_MACROCELLS][FIT_PT_PER_MC];  /* 16 fuses each, all '1' at start */
    char *mc_cfg[FIT_MAX_MACROCELLS];                   /* FIT_MC_CFG_DEFAULT; [8], [20], [22] are
                                                           preset by mc_power, open_collector,
                                                           output_fast when given without a list */
    char *uim[FIT_MAX_BLOCKS][FIT_UIM_PER_BLOCK];       /* uim_width fuses each, all '1' */
    char *global[FIT_GLOBAL_SLOTS];                     /* uim_width fuses each, all '1'; written out
                                                           under "NOTE  6 global OE" (jed_write_global_oe) */
    char  ues_bits[16];                                 /* all '1' at start; "NOTE  UES bits"
                                                           (jed_write_ues_bits), filled by jed_map_ues */
    char  cfg_bits[32];                                 /* "NOTE  device configuration bits"
                                                           (jed_write_device_bits) */
    char  jtag_bits[4];                                 /* "NOTE  Special Purpose Bits (JTAG)"
                                                           (jed_write_jtag_bits) */
} FitJedecMap;

/* ------------------------------------------------------------------ */
/* Options: set from the command line (main_fit_options,              */
/* main_device_options, main_fit_job) and from "#$ PROPERTY ATMEL"     */
/* lines of the .tt2 (tt2_property)                                    */
/* ------------------------------------------------------------------ */

typedef struct FitOptions {
    int cupl;                 /* -CUPL given */
    int ifmt_edif;            /* -str ifmt edif */
    int debug;                /* -str DEBUG ON */
    int latch_synthesis;      /* -str Latch_Synthesis ON (default 0) */
    int verilog_sim;          /* 0 off, 1 Verilog, 2 other value (sdf) */
    int vhdl_sim;             /* 0 off, 1 VHDL, 2 other value (sdf) */
    int out_edif;
    int cascade_logic;        /* 1 when cascade_logic is given with a value other than OFF
                                           (also for a signal list, kept in cascade_list) */
    int xor_synthesis;        /* xor_synthesis ON for all outputs */
    int fast_inlatch;
    int tpd;                  /* speed grade code from tPD */
    int mc_power;
    int open_collector;
    int power_reset;
    int pd1;
    int pd2;
    int tdi_pullup;
    int tms_pullup;
    int output_fast;
    int security;
    int logic_doubling;       /* also switched on by the driver */
    int preassign;            /* enum FitPreassign, set to its default at start-up */
    int optimize;             /* 1 unless optimize off */
    int foldback;
    int push_gate;            /* 1 unless push_gate off */
    int jtag;                 /* 1 unless JTAG OFF */
    int gck_itd[3];           /* GCK0..GCK2 */
    int pin_keep;             /* 1 unless pin_keep OFF */
    char    *ues;
    char    *device_package;  /* package name, e.g. "PLCC84" */
    char    *out_base;        /* output file name without extension */
    FitList *soft_buffer;     /* signal names (char *) */
    FitList *mc_power_list;
    FitList *xor_list;
    FitList *twoclock;
    FitList *cascade_list;
    FitList *fast_inlatch_list;
    FitList *global_oe;
    FitList *output_fast_list;
    FitList *global_fold;
    FitList *open_collector_list;
    FitList *foldback_list;
    /* the extended mode only */
    int be;                   /* -tech names a BE device */
    int asv;                  /* -tech names an ASV device */
    int por;                  /* -POR one */
    int schmitt_trigger;      /* schmitt_trigger given (BE) */
    int sstl_input;           /* SSTL_input given (1504/1508 BE, 2.5 or 3.3 V) */
    int pull_up_unused;       /* the three treatments of unused pins (BE) */
    int unused_to_ground;
    int unused_to_pinkeeper;
    int tff_ok;               /* 1 unless no_tff (BE) */
    int voltage_a;            /* I/O voltage of bank A, B: 0 1.5 V, 1 1.8 V, */
    int voltage_b;            /* 2 2.5 V, 3 3.3 V (BE) */
    int pinfile;              /* -pinfile given */
    char *pin_filename;       /* resolved input constraint file, or NULL */
    char    *tpd_text;        /* value of tPD, NULL without */
    FitList *pin_keep_list;
    FitList *schmitt_list;
    FitList *sstl_list;
    FitList *pull_up_list;
    FitList *pull_down_list;
} FitOptions;

/* ------------------------------------------------------------------ */
/* Streams shared by the modules                                       */
/* ------------------------------------------------------------------ */

typedef struct FitStreams {
    FILE *msg;    /* current message stream: stdout, or a temporary log
                     while msg_redirect_to has redirected it */
    FILE *fit;    /* the <out>.fit report, opened by report_open */
    FILE *jed;    /* the <out>.jed file while jed_write_file writes it */
} FitStreams;

/* ------------------------------------------------------------------ */
/* min_bdd.c: BDDs with marked edges (the min modules, min.h)          */
/*   The cells are BDD nodes (bdd_node_alloc) and computed-table       */
/*   entries (bdd_cache_entry_alloc, private to min_bdd.c).            */
/*   bdd_build (wrappers                                               */
/*   bdd_build_entity_500, bdd_build_entity_100, bdd_build_entity_ex), */
/*   bdd_apply, bdd_find_or_add.                                       */
/* ------------------------------------------------------------------ */

/* Edge marks: every edge and every root carries one. */
#define FIT_BDD_DIRECT    0x44   /* 'D' */
#define FIT_BDD_INVERTED  0x49   /* 'I' the function at the other end is complemented */

/* FitBddNode.state values */
enum FitBddState {
    FIT_BDD_ST_FRESH    = 0,     /* bdd_node_init */
    FIT_BDD_ST_COUNTED  = 1,     /* reached from an output (bdd_count_nodes); also the terminal */
    FIT_BDD_ST_ANNOTATED = 3,    /* has a FitBddInfo (bdd_annotate_node, bdd_annotate) */
    FIT_BDD_ST_LEVELLED = 4,     /* entered into the by-level lists (bdd_level_walk) */
    FIT_BDD_ST_SHARED   = 7,     /* turned into a local variable by bdd_node_to_tree */
    FIT_BDD_ST_DEPTH    = 10,    /* FitBddInfo.level computed (bdd_node_level) */
    FIT_BDD_ST_KEEP     = 11     /* 0xb: input variable node, or part of a local function (bdd_mark_keep) */
};

struct FitBddInfo;

typedef struct FitBddNode {
    int   var;                   /* 1-based position of the variable in entity->inputs; 0 in the terminal */
    int   hi_mark;               /* FIT_BDD_DIRECT / FIT_BDD_INVERTED: edge taken when the variable is 1 */
    struct FitBddNode *hi;       /* NULL in the terminal (test with FIT_BDD_IS_TERMINAL) */
    int   lo_mark;               /* edge taken when the variable is 0 */
    struct FitBddNode *lo;       /* NULL in the terminal */
    int   state;                 /* enum FitBddState */
    int   refs;                  /* number of times bdd_count_nodes reached the node */
    struct FitBddInfo *info;     /* set by bdd_annotate */
    int   bucket_pos;            /* index in its unique-table bucket when created */
    unsigned addr;               /* stand-in address of the node, used instead of its real address by
                                    the hash functions of the unique and computed tables (min_bdd.c) */
} FitBddNode;

#define FIT_BDD_IS_TERMINAL(n) ((n)->hi == NULL)

typedef struct FitBddBucket {    /* bdd_unique_create: one slot of the unique table */
    FitList *nodes;              /* FitBddNode */
    int      kept;               /* nodes[0..kept-1] survive the garbage collection bdd_collect */
} FitBddBucket;

typedef struct FitBddRoot {      /* bdd_out_roots_create: BDD of one output (or local) */
    int         mark;            /* FIT_BDD_DIRECT / FIT_BDD_INVERTED */
    FitBddNode *node;            /* NULL until built */
    char        name[FIT_NAME_MAX]; /* name of the variable */
} FitBddRoot;

typedef struct FitBddInfo {      /* bdd_info_alloc: per-node record made by bdd_annotate */
    int      w0;                 /* 0; unknown */
    int      w4;                 /* 0; unknown */
    int      level;              /* 0 for a node without parents, else 1 + highest level of its parents */
    FitList *parents;            /* FitBddNode: nodes with an edge to this one */
    char     kind;               /* 'N' (0x4e), or 'O' (0x4f) for a node an output points to */
} FitBddInfo;

#endif /* FIT150X_FIT_TYPES_H */
