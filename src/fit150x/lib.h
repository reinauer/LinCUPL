/*
 * lib.h - public interface of the gate library modules of the ATF150x
 * fitter: techmap (lib_techmap.c), lib_load (lib_load.c) and lib_parse
 * (lib_parse.c, lib_bin.c).  They read the gate library (atmel.std) and
 * adapt the design's netlist to it ("extra wiring").
 *
 * Private declarations are in lib_priv.h.
 */
#ifndef FIT150X_LIB_H
#define FIT150X_LIB_H

#include "fit_types.h"
#include "base.h"

/* ------------------------------------------------------------------ */
/* Structures created by these modules: the gate library in memory     */
/* ------------------------------------------------------------------ */

/* Expression node of a GATE equation as parsed (lib_parse.c).  The parser
 * turns it into a FitFctTree with lib_expr_to_tree. */
enum FitLibExprOp {
    FIT_LIBEXPR_NOT    = 0,   /* text "!", one child */
    FIT_LIBEXPR_CONST1 = 1,   /* text "CONST1" */
    FIT_LIBEXPR_VAR    = 2,   /* text = pin name */
    FIT_LIBEXPR_AND    = 3,   /* text "*" */
    FIT_LIBEXPR_OR     = 4,   /* text "+" */
    FIT_LIBEXPR_CONST0 = 9    /* text "CONST0" */
};

typedef struct FitLibExpr {
    char     text[0x98];
    int      op;           /* enum FitLibExprOp */
    int      bin_word;     /* only read/written by the binary library reader */
    FitList *children;     /* FitLibExpr */
    int      bin_words[4]; /* only the binary library reader touches them */
} FitLibExpr;

/* Timing of one input pin towards a related output pin. */
typedef struct FitLibPin {
    char    *name;
    int      phase;     /* 0 INV, 1 NONINV, 2 UNKNOWN */
    FitList *timing;    /* float * items (6 for GATE and BUF, 8 for the
                           sequential gates); with "PIN *" all pins of the gate
                           share one list (owned by the first) */
} FitLibPin;

/* RELATED_OUTPUT_PIN section. */
typedef struct FitLibOutPin {
    char    *name;
    float    max_fanout;
    FitList *pins;        /* FitLibPin */
} FitLibOutPin;

/* Payload of a GATE (kind FIT_GATE_COMB). */
typedef struct FitLibComb {
    char        name[FIT_NAME_MAX];
    char        out_name[FIT_NAME_MAX]; /* left side of the equation */
    float       area;                   /* (0 for CONST0/CONST1 gates) */
    float       max_fanout;
    FitLibExpr *expr;
    FitList    *pins;                   /* FitLibPin; NULL for constants */
    FitFctTree *tree;                   /* the equation; FIT_SEL_ID nodes
                                           carry a char * pin name in u.children
                                           instead of an id (see
                                           lib_expr_to_tree) */
} FitLibComb;

/* Clock kinds of a sequential PGATE (FitLibSeq.clk_kind, FitInstance.clk_kind) */
enum FitLibClock {
    FIT_CLK_RECK = 1,   /* rising edge */
    FIT_CLK_FECK = 2,   /* falling edge */
    FIT_CLK_L0   = 3,   /* latch, library keyword L0 */
    FIT_CLK_L1   = 4    /* latch, library keyword L1 */
};

/* Payload of a sequential PGATE (kinds TG DG LG JKG DMUXG LMUXG THG ITHG
 * RSG RSL).  A pin that is absent ("*") is NULL.  The `_pol` fields are 1,
 * or -1 when the pin is written with a leading "!". */
typedef struct FitLibSeq {
    char    *name;
    float    area;
    char    *clk;
    int      clk_kind;  /* enum FitLibClock */
    char    *ar;        /* asynchronous reset */
    int      ar_pol;
    char    *ap;        /* asynchronous preset */
    int      ap_pol;
    char    *ce;        /* clock enable */
    int      ce_pol;
    char    *oe;        /* output enable */
    int      oe_pol;
    char    *d;         /* data input of TG DG LG THG ITHG */
    char    *j;         /* J (JKG), S (RSG, RSL) */
    int      j_pol;
    char    *k;         /* K / R */
    int      k_pol;
    char    *mux_a;     /* DMUXG, LMUXG */
    char    *mux_b;
    char    *mux_sel;
    char    *q;
    char    *qn;
    char    *z;         /* tri-state output */
    FitList *outs;      /* FitLibOutPin */
} FitLibSeq;

/* Buffer types (FitLibBuf.type, FitCell13.buf_type) */
enum FitLibBufType {
    FIT_BUF_I   = 1,   /* I_BUF */
    FIT_BUF_O   = 2,   /* O_BUF */
    FIT_BUF_BI  = 3,   /* BI_BUF */
    FIT_BUF_TS  = 4,   /* TS_BUF */
    FIT_BUF_CLK = 5    /* CLK_BUF */
};

/* Payload of "PGATE .. BUF" (kind FIT_GATE_BUF). */
typedef struct FitLibBuf {
    int      type;      /* enum FitLibBufType */
    float    area;
    char    *name;
    char    *in;
    char    *out;
    char    *pad;
    char    *oe;
    int      oe_pol;
    FitList *outs;      /* FitLibOutPin */
} FitLibBuf;

/* Payload of a UGATE (kind FIT_GATE_UNKNOWN in a library). */
typedef struct FitLibUGate {
    char    *name;
    float    area;
    int      adder;     /* 0, or 1 / 2 for "UGATE_IS adder1 / adder2" */
    FitList *inputs;    /* char * */
    FitList *outputs;   /* char * */
    FitList *outs;      /* FitLibOutPin */
} FitLibUGate;

/* One gate of the library.  kind is an enum FitGateKind. */
typedef struct FitLibGate {
    int   kind;
    int   dont_touch;   /* first integer after GATE/PGATE/UGATE */
    int   dont_use;     /* second integer; gates with a nonzero value are
                           ignored by the mapper (lib_group_by_kind) */
    void *cell;         /* FitLibComb (kind 1), FitLibUGate (10),
                           FitLibBuf (13), FitLibSeq (all others) */
} FitLibGate;

/* Third result of lib_parse_file (created by lib_header_new); only
 * `comment` is ever filled by the text parser.  The other fields are
 * created with fixed contents and never read (unknown use). */
typedef struct FitLibHeader {
    char    *comment;   /* words of COMMENT "..." joined by blanks */
    FitList *l10;       /* empty list; items freed with free() */
    int      w14;       /* 3 */
    FitList *l20;       /* empty list; items freed with free() */
} FitLibHeader;

/* The library object itself (kind 1, file, gates, header) is
 * FitLibHandle in base.h: lib_open (base_entity.c) creates it from the two
 * results of lib_parse_file and stores it in the entity table with type 3;
 * its `info` member is the FitLibHeader. */

/* The gates of a library grouped by kind (built by lib_group_by_kind). */
typedef struct FitLibKindGroup {
    int      kind;
    int      count;    /* number of gates */
    FitList *gates;    /* FitLibGate, not owned */
} FitLibKindGroup;

/* ------------------------------------------------------------------ */
/* Functions called from other packages                                */
/* ------------------------------------------------------------------ */

/* Library stage of the job (called from main_fit_job) with the parameter
 * list {tech=atmel.std, s=Fct, tsbuf=ON}: takes the entity named by "s"
 * from the entity table, reads the library named by "tech" unless it is
 * already loaded under the symbolic name "//<tech>" (through lib_open,
 * which calls lib_parse_file), adapts the netlist to the library and
 * stores the result back under the same entity name.  Returns 1 on
 * success, 0 on failure. */
int lib_load_library(FitList *params);

/* Parser of a gate library file (text, or the binary form that starts with
 * the words 0x30, 0xd).  On success *gates is a list of FitLibGate and
 * *header a FitLibHeader, and 1 is returned; on failure both are released
 * and 0 is returned.  Called from lib_open. */
int lib_parse_file(const char *path, FitList **gates, FitLibHeader **header);

/* Releases a FitLibHeader (does not clear the pointer; NULL is allowed). */
void lib_header_free(FitLibHeader **header);

/* Destructor of one FitLibGate (FitListFree); used by lib_handle_release
 * to release a library. */
void lib_gate_free(void **gate_ref);

/* Appends the function "dst = src" (an ID node) to ent->fcts, creating the
 * list if needed.  Called from tt2_finish_instance. */
void techmap_add_wire_fct(FitEntity *ent, FitVariable *dst, FitVariable *src);

/* Name of the variable with this id: searches inputs, outputs, locals and
 * sets *cls to 0, 1, 2 accordingly.  Returns NULL (and leaves *cls alone)
 * when there is none.  Also used by base_simplify.c and jed.c. */
char *lib_var_name_by_id(int id, FitEntity *ent, int *cls);

/* Moves the variable with this id from list `from` to the end of
 * list `to` (nothing happens when `from` does not hold it). */
void lib_move_variable(int id, FitList *from, FitList *to);

/* In the first tree of every FitFct of the list, replaces ID
 * nodes for old_id by new_id (descends through ! * + ^ only). */
void lib_fcts_replace_id(FitList *fcts, int old_id, int new_id);

/* Separates outputs that are read inside the design: for every output that
 * takes part in a combinational loop through the functions, or (through
 * lib_split_output) for each variable so found, a new output <name>_OO_OO_
 * takes over the pin and a FIT_GATE_WIRE item connects the two.  Also
 * creates the netlist lists if missing and recounts n_in/n_out/n_local.
 * Returns 1.  Called from tt2_delete_unused_cycles and from the mapper. */
int lib_split_fed_back_outputs(FitEntity *ent);

/* 1 when two netlists hold the same items (as sets, compared field by
 * field for presence of the pins and equal polarities).  Used by
 * min_equiv.c. */
int lib_netlist_equal(FitNetlist *a, FitNetlist *b);

/* 1-based position of the first FIT_GATE_WIRE item of the netlist, 0 if
 * none (or net is NULL).  Used by min_equiv.c. */
int lib_netlist_find_wire(FitNetlist *net);

/* Sets the target of the alias `name` to a copy of target (or NULL).  The
 * old target string is not freed. */
void lib_alias_set_target(FitNetlist *net, char *name, char *target);

/* Removes the alias `name` from the list (the item is not freed). */
void lib_alias_remove(FitNetlist *net, char *name);

/* Appends an alias {copy of name, copy of target}; either may be
 * NULL.  Creates the alias list if needed.  Nothing happens for net NULL. */
void lib_alias_add(FitNetlist *net, char *name, char *target);

/* If `name` is not an input, has an alias entry and tt2_net_uses_signal
 * says it is not used as an output, moves it from the outputs to the
 * locals and drops its alias.  Called from tt2_apply_node_decls. */
void lib_output_to_local(FitEntity *ent, char *name);

/* The same for every alias entry of the entity.  Called from
 * fit_prep_expand_xor_gates. */
void lib_outputs_to_locals(FitEntity *ent);

/* New variable with a generated name (var_new_unique) appended to the
 * inputs (cls 0), outputs (1) or locals (2), counters updated; with
 * add_alias nonzero it also gets an alias entry without target. */
FitVariable *lib_new_variable(FitEntity *ent, int cls, int add_alias);

/* Appends a pin {dir, copy of pin_name, copy of signal} to the pin list of
 * an unknown gate; nothing happens when signal is NULL.  Called from
 * xor_rewrite_function. */
void lib_unknown_gate_add_pin(FitList *pins, char *signal, char *pin_name, int dir);

/* The sequential or tri-state cell of the netlist whose Q
 * (FitInstance.d) or QN (FitInstance.d2) signal is `name`; NULL if none.  Items
 * of kind 1, 6, 7, 10 and 13 are skipped. */
FitInstance *lib_instance_by_output(FitEntity *ent, char *name);

/* FitListEq: item is a pointer to a record whose second word is a string;
 * equal to key (case-sensitive). */
int lib_eq_second_string(void *item, void *key);

/* FitListEq: first word of the item equals key.  Used by
 * lib_netlist_find_wire. */
int lib_eq_first_word(void *item, void *key);

/* Post-processing of an entity read in PALASM format, an unsupported
 * option (main_check_params does not accept that format, so nothing
 * reaches it): reports "fit150x: ifmt PALASM is not supported" on stderr
 * and exits with status 3. */
void lib_palasm_stage(FitEntity *ent);

/* Resets the file-scope state of the three modules to zero; called at the
 * start of each run. */
void lib_reset(void);

#endif /* FIT150X_LIB_H */
