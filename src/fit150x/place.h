/*
 * place.h - public interface of the place modules of the ATF150x fitter:
 *
 *   placer          prefix place_   (src/fit150x/place.c)
 *   router          prefix route_   (src/fit150x/route.c)
 *   block partition prefix alloc_   (src/fit150x/alloc.c)
 *
 * The functions listed here are the ones other modules call.
 *
 * Conventions: a FitList pointer may be NULL (an empty list); list
 * positions are 1-based, 0 = not found; block numbers are 1-based in
 * FitSlot.block and 0-based wherever a block indexes one of the per-block
 * lists (said at each function).  Lists of variable ids hold the ids as
 * (void *)(intptr_t)id.
 *
 * FitSlot (fit_types.h) as these modules use it:
 *   signal    name of the signal on the pin ("--" = free)
 *   fct_id    FitVariable.id / FitFct.var_id of the function whose
 *             output drives the pin, 0 = none (a buried function is
 *             recorded only by its name in buried)
 *   cascade   "--" unless the macrocell's terms are taken by a
 *             neighbour (cascade); such a cell offers 0 terms
 *   buried    name of the buried (feedback) signal, "--" = none
 *   foldback  foldback signal of the macrocell ("--" none, "NA" ...);
 *             a foldback costs one of the five terms
 *   fn_kind   "reg" / "com" / "--"
 * Node names ("refs") are the strings of the UIM tables: <block letter>
 * <macrocell number> "p" (pin) or "fb" (feedback), e.g. "A13p", "G103fb",
 * and "GCLK", "GCLR", "OE1", "OE2" for the dedicated inputs.
 */
#ifndef FIT150X_PLACE_H
#define FIT150X_PLACE_H

#include "fit_types.h"

/* ------------------------------------------------------------------ */
/* Structures                                                          */
/* ------------------------------------------------------------------ */

/* Item of the pin preassignment list g_preassign (created outside the
 * place modules; only the two fields the place modules read). */
typedef struct FitPreassignItem {
    char *name;   /* signal name */
    int   pin;    /* pin number; 300+mc = foldback node, 600+mc = feedback node */
} FitPreassignItem;

/* ------------------------------------------------------------------ */
/* place.c                                                             */
/* ------------------------------------------------------------------ */

/* Expression of variable id: a fresh ID node (fcttree_new_id) when
 * id is not a local of ent or is not in g_fit_excluded_ids, else tree[0] of
 * its function.  Used by jed.c and tt_tt3.c. */
FitFctTree *place_fct_tree(FitEntity *ent, int id);

/* Number of product terms of fct: 1 when tree[0] is not a '+',
 * else its number of operands (0 for a '+' without list).  ent is unused.
 * Used by the fit driver. */
int place_count_terms(FitEntity *ent, FitFct *fct);

/* 1-based position in the preassignment list of the item whose
 * pin number is `pin`; 0 when pin is 0 or not found.  Used by the fit
 * driver. */
int place_preassign_index_of_pin(int pin);

/* The function that drives variable var: for a register or
 * tri-state output the function of the cell's q signal, else the
 * function of var itself.  NULL when there is none.  Used by the fit
 * driver. */
FitFct *place_fct_of_var(FitEntity *ent, FitVariable *var);

/* Free macrocells of block (1-based) able to take a function of
 * n_pt product terms: scans the block from its last macrocell down, and
 * for a cell that qualifies under `mode` (1, 5, 0x18: see place.c) collects
 * it and as many lower neighbours as are needed to reach n_pt terms.
 * mc != 0 restricts the search to that macrocell (1-based).  var_id is the
 * candidate function (tested against the Global_OE list).  Returns a new
 * list of FitSlot, empty when nothing fits.  Used by the fit driver. */
FitList *place_find_slots(FitEntity *ent, int block, int n_pt, int mode, int mc, int var_id);

/* Creates g_fit_slots: one FitSlot per macrocell (block, number,
 * pin of the package) followed by the dedicated inputs in the order OE2,
 * GCLK, GCLR, OE1, then reserves the JTAG and power-down pins
 * (report_reserve_jtag_slots, report_reserve_pd_slots).  Called from the
 * pass fit_drv_pass. */
void place_create_slots(char *package);

/* Appends to ids the variable ids the expression tree depends
 * on, looking through foldback nodes (ids in g_fit_foldback_ids are
 * replaced by the inputs of their own function).  Used by the fit driver. */
void place_collect_inputs(FitEntity *ent, FitFctTree *tree, FitList *ids);

/* For a function of the form a*b + c*d ... recognised as an XOR
 * of two variables: collects the inputs of both operands into ids and
 * returns the number of product terms (operands of the second, else the
 * first, plus one; 2 when neither is a sum); 0 when fct has not that form.
 * Used by fit_drv_count_mc_terms. */
int place_xor_terms(FitEntity *ent, FitList *ids, FitFct *fct);

/* Slot for a pin or node number: 600+mc and 300+mc address the
 * macrocell mc, anything else is searched as a package pin among all
 * slots.  NULL when not found.  Used by jed.c and the fit driver. */
FitSlot *place_slot_of_pin(int pin);

/* Macrocell number (1-based) for a pin or node number, 0 when the
 * pin belongs to no macrocell. */
int place_macrocell_of_pin(int pin);

/* Block letter "A".."H" of a slot (anything but 1..7 gives "H").
 * Used by dev.c and the fit driver. */
const char *place_block_letter(FitSlot *slot);

/* For every block (0-based index in the result) the list of node
 * names (new strings) of the macrocell signals the block needs according
 * to g_fit_block_fanin.  Used by the fit driver. */
FitList *place_block_node_names(FitEntity *ent);

/* UIM assignment: names is a list (per block) of lists of node
 * names; the result is a list (per block) of lists (per name) of lists of
 * FitUimSlot candidates, reduced so that a name that could be routed has
 * exactly one candidate.  Used by the fit driver. */
FitList *place_assign_uim(FitList *names);

/* Slot named by a node name; *signal receives a new copy of the
 * signal behind it (the pin signal for "..p" and the dedicated inputs, the
 * feedback signal for "..fb" when the cell has one).  NULL for a NULL name.
 * Used by jed_pick_uim. */
FitSlot *place_slot_of_node(const char *node, char **signal);

/* Creates g_fit_uim_slots: per block FIT_UIM_PER_BLOCK FitUimSlot.
 * Called from the pass fit_drv_pass. */
void place_create_uim_slots(void);

/* Creates g_jed_global_slots: FIT_GLOBAL_SLOTS FitUimSlot.  Called
 * from the pass fit_drv_pass. */
void place_create_global_slots(void);

/* Returns a list of n_blocks lists: list k-1 holds the ids that
 * k blocks need (from g_fit_block_fanin); ids of the Global_OE list are
 * removed from g_fit_block_fanin on the way.  Called from the pass
 * fit_drv_pass. */
FitList *place_collect_block_signals(void);

/* Number of times variable id occurs in the functions of the
 * xor_synthesis list.  Used by fit_prep_filter_xor_list. */
int place_count_xor_uses(FitEntity *ent, int id);

/* Ids used by the functions of the xor_synthesis list that are
 * locals of ent and are either used at most twice in the entity or used
 * only by those functions; entries of the xor list without a function are
 * dropped from it.  Used by fit_prep.c and fit_drv_main.c. */
FitList *place_xor_local_ids(FitEntity *ent);

/* Removes the unused FitUimSlot ("--") from g_fit_uim_slots and
 * then the blocks left empty.  Called from the pass fit_drv_pass. */
void place_prune_uim_slots(void);

/* Slot (searched from the last dedicated input down) whose pin
 * signal is `signal`; *node receives its node name as a new string
 * ("OE2", "GCLK", "GCLR", "OE1" or <letter><mc>p).  NULL when not found.
 * Used by the fit driver. */
FitSlot *place_node_of_signal(const char *signal, char **node);

/* FitListEq: the first field of the item (FitSlot.signal,
 * FitPreassignItem.name) equals the key string, ignoring case. */
int place_name_eq(void *item, void *name);

/* ------------------------------------------------------------------ */
/* route.c                                                             */
/* ------------------------------------------------------------------ */

/* Writes the UIM assignment into the FitUimSlots: cands is the
 * result of place_assign_uim, names the list it was made from; for every
 * block a list of the ids that could not be routed is appended to failed.
 * Sets g_fit_route_failed on any failure.  Returns n_blocks, or 0 after
 * "Routing Failed ..." (list lengths differ).  Used by the fit driver. */
int route_commit(FitEntity *ent, FitList *cands, FitList *names, FitList *failed);

/* Slot holding variable `name` as pin signal or feedback signal
 * (macrocells), or as pin signal (dedicated inputs); *node receives its
 * node name (new string; <letter><mc>p or ..fb, or the dedicated input's
 * name).  NULL when not found.  Used by report.c and the fit driver. */
FitSlot *route_node_of_name(const char *name, char **node);

/* Repairs a failed routing by moving and swapping signals between
 * macrocells and routing again, until g_fit_route_failed is clear or the
 * attempts are used up.  cands / failed / names as in route_commit.  The
 * final lists are left in g_route_cands, g_route_failed_ids, g_route_names.
 * Used by the fit driver. */
void route_repair(FitEntity *ent, FitList *cands, FitList *failed, FitList *names);

/* FitListCmp on FitFct.flags (the fan-in count stored by
 * alloc): 1, 0, -1.  Used by fit_prep.c and the fit driver. */
int route_fct_weight_cmp(void *a, void *b);

/* ------------------------------------------------------------------ */
/* alloc.c                                                             */
/* ------------------------------------------------------------------ */

/* Builds g_fit_work_fcts, the list of functions to place (outputs
 * that are no register inputs, then locals), sorted by descending fan-in
 * (FitFct.flags); exclude lists ids to leave out (copied to
 * g_fit_excluded_ids and taken out of g_fit_foldback_ids).  Returns the
 * number of macrocells the functions need.  Used by fit_prep.c. */
int alloc_build_fct_list(FitEntity *ent, FitList *exclude);

/* ids: foldback ids one function needs; block: 0-based.  Ids the
 * block already has (g_fit_block_foldbacks) are deleted from ids; with
 * add != 0 the others are appended to the block's list.  Used by the fit
 * driver. */
void alloc_merge_block_foldbacks(FitList *ids, int block, int add);

/* Ids of the foldback nodes (g_fit_foldback_ids) the function
 * fct depends on, transitively.  New list.  Used by the fit driver. */
FitList *alloc_fct_foldbacks(FitEntity *ent, FitFct *fct);

/* Prints the 90 % logic warning when n_mc macrocells exceed it;
 * then, unless preassignments are honoured (returns 0 at once), partitions
 * the functions of g_fit_work_fcts into blocks (g_fit_block_fcts,
 * g_fit_block_foldbacks), raising g_fit_route_limit until all are placed.
 * 1 = done, 0 = limit reached.  Called from the pass fit_drv_pass. */
int alloc_partition_simple(FitEntity *ent, int n_mc);

/* FitListEq: the first int of both items is equal (FitFct.var_id
 * of two functions).  Used by the fit driver and alloc.c. */
int alloc_first_int_eq(void *a, void *b);

/* Block partition with pin preassignments and block groups.
 * 1 = done, 0 = failed.  Called from the pass fit_drv_pass. */
int alloc_partition(FitEntity *ent, int n_mc);

/* ------------------------------------------------------------------ */
/* Globals defined by the place modules (place.c)                      */
/* ------------------------------------------------------------------ */

extern int      g_alloc_free_pins;     /* I/O pins left in the block being filled */
extern FitList *g_route_cands;         /* see route_repair */
extern FitList *g_route_failed_ids;
extern FitList *g_route_names;
extern int      g_alloc_mc_per_block;  /* written by the partition, never read */
extern int      g_alloc_grouped;       /* 1 while the partition works by block groups */
extern FitList *g_place_seen_ids;      /* work list of place_collect_inputs */
extern int      g_alloc_max_wide;      /* limit of multi-macrocell functions per block (6 or 8) */
extern int      g_alloc_swap_depth;    /* recursion counter of alloc_reroute_name */
extern FitList *g_alloc_preassigned_fcts; /* functions without pin or preassigned
                                             (alloc_split_pin_bound) */
extern int      g_route_progress;      /* initially 1; set when a repair step changed something */

#endif /* FIT150X_PLACE_H */
