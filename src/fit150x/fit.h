/*
 * fit.h - public interface of the fit modules of the ATF150x fitter: the
 * preparation of the netlist for fitting (fit_prep.c) and the fit driver
 * (fit_drv.c, fit_drv_main.c), which runs the placement passes and writes
 * the results.
 */
#ifndef FIT150X_FIT_H
#define FIT150X_FIT_H

#include "fit_types.h"

/* ------------------------------------------------------------------ */
/* Structures created by these modules                                 */
/* ------------------------------------------------------------------ */

/* Item of g_fit_fast_inlatch once fit_drv_find_fast_inlatch has converted
 * the property's names: the input and the node that latches it. */
typedef struct FitFastInlatch {
    int in_id;      /* FitVariable.id of the input pin (key of fit_drv_fast_inlatch_node) */
    int node_id;    /* FitVariable.id of the registered node (key of fit_drv_fast_inlatch_index) */
} FitFastInlatch;

/* ------------------------------------------------------------------ */
/* Functions called from other packages                                */
/* ------------------------------------------------------------------ */

/* The fit driver: strategy section of the report, preparation of
 * the netlist, the pass loop, fuse map, report, .jed, .tt3, .pin, .io.
 * ent is the mapped entity, out_base the output file name without
 * extension, preassign an enum FitPreassign, latch_synthesis the
 * -Latch_Synthesis flag.  Returns 1 when the design fits, else 0
 * (g_exit_code says why).  Called from main_fit_job (main.c). */
int fit_driver(FitEntity *ent, char *out_base, int preassign, int do_latch_synthesis);

/* Empty function; kept as a hook for the callers in main.c. */
void fit_prep_nop(void);

/* For every child of tree (last to first): move the child into a
 * new local function of ent (new variable from bdd_new_local_var) and put
 * an ID node for that variable in its place.  Called from entity_flatten
 * (tt_flatten.c). */
void fit_prep_hoist_children(FitEntity *ent, FitFctTree *tree);

/* EDIF input (-str ifmt edif) only, which is not supported: reports
 * "fit150x: ifmt edif is not supported" and exits with 3. */
void fit_prep_edif_keep_inputs(FitEntity *ent, int mode);

/* Builds the list g_fit_block_groups from the entity; used by the block
 * partition (alloc.c). */
void fit_drv_build_groups(FitEntity *ent);

/* Number of control product terms (clock 3, reset 4, output
 * enable 6, preset 5, clock enable 7) the register whose output is `name`
 * needs in its macrocell; the signals found are appended to `seen`.
 * 0 when `name` is not a register output.  Used by the router (route.c). */
int fit_drv_count_ctrl_terms(FitEntity *ent, FitList *seen, char *name);

/* Number of product terms the function fct of variable var takes
 * in a macrocell, control terms included; input ids are collected in
 * `inputs`.  Used by the placer and the partition (place.c, alloc.c). */
int fit_drv_count_mc_terms(FitEntity *ent, FitFct *fct, FitVariable *var, FitList *inputs);

/* Number of macrocells' worth of spare terms: from the macrocell
 * count n_mc and product-term count n_pt of a function, how many
 * neighbouring macrocells have to lend terms.  Used by alloc.c. */
int fit_drv_count_borrowed(FitEntity *ent, int n_mc, int n_pt, FitVariable *var);

/* 1 when var (with n_pt == 5 product terms) is a register that
 * needs no output enable and whose function is a single term.  Used by
 * the block partition (alloc.c). */
int fit_drv_is_single_term_reg(FitEntity *ent, FitFct *fct, FitVariable *var, int n_pt);

/* 1-based position in g_fit_fast_inlatch of the entry whose
 * node_id is id, 0 when there is none.  Used by jed.c, route.c, alloc.c. */
int fit_drv_fast_inlatch_index(int node_id);

/* node_id of the fast-inlatch entry whose in_id is id, else 0.
 * Used by jed.c and route.c. */
int fit_drv_fast_inlatch_node(int in_id);

/* ------------------------------------------------------------------ */
/* Globals defined by the fit modules (in fit_drv.c)                    */
/*                                                                    */
/* Shared by the preparation, the driver, the placer, the router and   */
/* the partition.  The lists are rebuilt for every pass.               */
/* ------------------------------------------------------------------ */

extern int g_fit_route_failed;        /* cleared before every pass; set when routing fails */
extern int g_fit_place_failed;        /* cleared before every pass */
extern int g_fit_retry_count;         /* retries through fit_drv_relax (at most 2) */
extern int g_fit_plan_done;           /* set once the macrocell plan fit_prep_plan has run */
extern int g_fit_report_failures;     /* set while route_retry_with_feedback runs:
                                         failures are reported */
extern int g_fit_preassign_used;      /* nonzero while pin preassignments are honoured;
                                         initially 1 */

extern FitList *g_fit_input_ids;      /* ids of the input signals placed */
extern FitList *g_fit_block_fcts;     /* per block: FitFct assigned by the partition */
extern FitList *g_fit_block_fanin;    /* per block: ids of the signals the block needs
                                         from the UIM */
extern FitList *g_fit_excluded_ids;   /* ids not to be placed as macrocells */
extern FitList *g_fit_uim_slots;      /* per block: list of FitUimSlot, created by
                                         place_create_uim_slots */
extern FitList *g_fit_block_groups;   /* groups (lists of FitFct) of functions that
                                         share foldbacks */
extern FitList *g_fit_block_placed;   /* per block: ids of the signals placed in it */
extern FitList *g_fit_doubling_fcts;  /* with logic doubling: FitFct set aside by the
                                         partition, placed at the end */
extern FitList *g_fit_slots;          /* list of FitSlot, created by place_create_slots */
extern FitList *g_fit_global_names;   /* names (char *) of the signals on global pins */
extern FitList *g_fit_block_foldbacks; /* per block: ids of the foldbacks it implements */
extern int      g_fit_route_limit;    /* FIT_FIRST_ROUTE_LIMIT..FIT_LAST_ROUTE_LIMIT */
extern FitList *g_fit_must_global;    /* names of the signals that must be on a global pin */
extern FitList *g_fit_foldback_ids;   /* ids of the functions kept as foldback nodes */
extern FitList *g_fit_work_fcts;      /* FitFct still to be assigned to a block, sorted
                                         by descending fan-in */

/* Shared with entity_flatten (tt_flatten.c), defined by the fit modules. */
extern int      g_fit_hoist_mode;     /* 1 while fit_prep_working_copy has the entity flattened */
extern FitList *g_fit_hoisted_ids;    /* var ids of the functions flatten handed to
                                         fit_prep_hoist_children; read by
                                         fit_prep_complement_hoisted */

#endif /* FIT150X_FIT_H */
