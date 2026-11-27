/*
 * tt.h - public interface of the tt modules of the ATF150x fitter:
 *
 *   fct_flatten   sum-of-products expansion     prefix flatten_
 *   tt3_write     .tt3 writer, programmed logic prefix tt3_
 *   input_driver  reading the design            prefix input_
 *   tt2_read      .tt2 reader, entity <-> PLA   prefix tt2_
 *
 * The functions listed here are the ones other modules call; everything
 * else of these modules is static in src/fit150x/tt*.c.
 *
 * Conventions used by all of them: a FitList pointer may be NULL (an empty
 * list); list positions are 1-based; strings returned as "char *" are
 * allocated and owned by the caller unless stated.
 */
#ifndef FIT150X_TT_H
#define FIT150X_TT_H

#include <stdio.h>
#include "fit_types.h"

/* ------------------------------------------------------------------ */
/* fct_flatten                                                         */
/* ------------------------------------------------------------------ */

/* fold the constants of a tree in place: "!C" becomes the other
 * constant, constant operands of '*' and '+' are dropped or absorb the node,
 * a '*' or '+' left with one operand is replaced by that operand. */
void flatten_fold_tree(FitFctTree *tree);

/* flatten_fold_tree on tree[0] of every FitFct of the entity. */
void flatten_fold_entity(FitEntity *entity);

/* every output that is used inside some function gets a copy of
 * its variable appended to entity->inputs; n_in is set to the new count. */
void flatten_add_fed_back_outputs(FitEntity *entity);

/* the reverse: inputs that are also outputs (same id) are freed
 * and removed from entity->inputs (list_del_insert order); n_in updated. */
void flatten_drop_fed_back_outputs(FitEntity *entity);

/* calloc(1, 8): a pair of ints (index, weight). */
int *flatten_new_pair(void);

/* free(*ref); *ref = NULL.  Used all over the fitter as the
 * element destructor of lists of plain malloc blocks (FitListFree). */
void flatten_free_ref(void **ref);

/* turn every function of *entity into a sum of products.  The
 * entity is replaced by a rebuilt copy.  A function whose expansion would
 * exceed max_terms product terms is split with new local variables. */
void flatten_entity(FitEntity **entity, int max_terms);

/* replace variable id old_id by new_id in tree[0] of every
 * function of the entity. */
void flatten_replace_id(int old_id, int new_id, FitEntity *entity);

/* ------------------------------------------------------------------ */
/* tt3_write                                                           */
/* ------------------------------------------------------------------ */

/* first netlist instance (kinds other than 1, 6, 7, 10, 13) one
 * of whose inputs d, d2 or pin is `name`
 * (case-sensitive).  *kind receives its FitNetItem.kind, 0 if none. */
FitInstance *tt3_find_instance_by_input(FitEntity *entity, const char *name, int *kind);

/* look `name` up among the alias targets of the netlist
 * (case-insensitive) and continue with tt3_find_instance_by_input on the
 * alias name.  NULL if there is no such alias. */
FitInstance *tt3_find_instance_by_alias(FitEntity *entity, const char *name, int *kind);

/* FitNetItem.kind of the instance whose output q is
 * `name` (case-insensitive), -1 if none. */
int tt3_gate_kind_of_output(FitEntity *entity, const char *name);

/* the "<device> programmed logic:" section of the .fit report:
 * the equations of a copy of the entity in CUPL-like syntax. */
void tt3_print_programmed_logic(FILE *out, FitEntity *entity);

/* write <base>.tt3 (".tt3" is appended unless the name already
 * contains ".tt").  With `extra` non-NULL the "#$ PINS" and "#$ NODES"
 * lines are left out (the driver passes NULL). */
void tt3_write(const char *base, FitEntity *entity, char *extra);

/* ------------------------------------------------------------------ */
/* input_driver                                                        */
/* ------------------------------------------------------------------ */

/* read the design named by the first parameter of the list
 * (FitParam items; the job passes "i" first), format from parameter "ifmt"
 * (TT and EDIF are supported; EQN and the others end in "fit150x: ... is not
 * supported", exit 3), store it in the entity table under the name given
 * by parameter "d" (the job passes "Fct"; without it: slot 1, name "") and
 * call tt2_print_entity_stats.  Returns 1 on success; on failure 0 with
 * g_exit_code set (0x65 bad ifmt, 0xc9 file not found, 0x12d reader
 * failed).  the extended mode adds the PROPERTY lines of a .tt2 file to the job
 * parameters `job`. */
int input_read(FitList *params, FitList *job);
int edif_read(FILE *in, FitEntity **entity, FitList *params);

/* 1 if some function of the entity has a tree[2] that is
 * not the constant 0. */
int input_entity_has_dc(FitEntity *entity);

/* ------------------------------------------------------------------ */
/* tt2_read: entity <-> PLA conversions and entity helpers             */
/* ------------------------------------------------------------------ */

/* list of FitPTermIO built from tree
 * `which` of every function (0: tree[0], 1: tree[1], 2: tree[2], whose
 * output bits get '-').  NULL after an error message. */
FitList *tt2_pla_from_fct(FitEntity *entity, int which);

/* the reverse: replace entity->fcts by functions built from a
 * list of FitPTermIO (tree[0] and tree[2] of each output and local). */
void tt2_fct_from_pla(FitList *pla, FitEntity *entity);

/* 1 + the largest variable id of the entity. */
int tt2_next_var_id(FitEntity *entity);

/* names of a list of variable ids joined with a separator. */
char *tt2_ids_to_string(FitEntity *entity, FitList *ids);

/* combinational cycle check.  Returns 1 if there is none; 0
 * otherwise, with *cycle (if the check got that far) describing it. */
int tt2_check_cycles(FitEntity *entity, char **cycle);

/* 1 if the tree is a constant or the negation of a constant. */
int tt2_tree_is_const(FitFctTree *tree);

/* number of references to variable id in tree[0] of all functions. */
int tt2_count_id_uses(FitEntity *entity, int id);

/* sum of the tree sizes (fcttree_count_leaves) of tree[0] of all functions:
 * the "Complexity" of the statistics line. */
int tt2_entity_complexity(FitEntity *entity);

/* substitute function `fct` into tree[0] of every function
 * (fcttree_substitute on each). */
void tt2_substitute_fct(FitEntity *entity, FitFct *fct);

/* collapse local functions whose substitution costs less than
 * `limit`; constants are always propagated.  Returns 1. */
int tt2_collapse_locals(FitEntity *entity, int limit);

/* fcttree_compact on tree[0] of every function. */
void tt2_compact_entity(FitEntity *entity);

/* the line "\tIOs        = %-4d\tINodes     = %-4d \tDepth
 *      = %-3d\tComplexity = %-6d\n" on g_msg, only when g_verbose
 * is nonzero, which it is not in the CUPL flow.  IOs = inputs +
 * outputs - alias entries, INodes = locals.  Returns 1. */
int tt2_print_entity_stats(FILE *unused, FitEntity *entity);

/* rename a signal in every instance of the netlist (and in the
 * aliases if aliases_too is nonzero). */
void tt2_rename_net_signal(FitEntity *entity, const char *old_name, const char *new_name,
                           int aliases_too);

/* append the function  out = in  (a single FIT_SEL_ID node). */
void tt2_add_buffer_fct(FitEntity *entity, FitVariable *out, FitVariable *in);

/* 1 if some instance of the netlist has a pin connected to `name`. */
int tt2_net_uses_signal(FitEntity *entity, const char *name);

/* turn a cycle description of tt2_check_cycles back into a list
 * of ids of local variables; NULL if one of the names is not a local. */
FitList *tt2_cycle_to_ids(FitEntity *entity, const char *cycle);

/* delete the functions and local variables of a cycle (the list
 * of tt2_cycle_to_ids; it is consumed down to its unmatched entries). */
void tt2_delete_cycle(FitEntity *entity, FitList *ids);

/* "Info -  The following Local unused cycle is deleted :" */
void tt2_delete_unused_cycles(FitEntity *entity);

/* replace tree[0] of the function defining `id` by its negation
 * (fcttree_new_not). */
void tt2_invert_fct(FitEntity *entity, int id);

/* list comparator: item is a "char **" (for example a FitAlias),
 * key a string; nonzero if *item equals key (case-sensitive). */
int tt2_name_ref_equal(void *item, void *key);

/* the PLA (.tt2) reader.  Returns 1 and the new entity in
 * *entity, or 0 after a syntax error.  `job`: see input_read. */
int tt2_read(FILE *in, FitEntity **entity, FitList *job);

/* ------------------------------------------------------------------ */
/* Globals of these modules that other packages use                    */
/* ------------------------------------------------------------------ */

/* limit on the number of product terms while flattening (second
 * argument of flatten_entity). */
extern int g_flatten_max_terms;

/* when nonzero the limit is also applied to the terms collected
 * for an OR node.  Only an EDIF reader would set it; always 0 here. */
extern int g_flatten_limit_sums;

/* figures of the last PLA written by tt3_write, read by the report:
 * product terms, "#$ NODES" count, "#$ PINS"
 * count, .o and .i. */
extern int g_tt3_n_pterms;
extern int g_tt3_n_nodes;
extern int g_tt3_n_pins;
extern int g_tt3_n_out;
extern int g_tt3_n_in;

#endif /* FIT150X_TT_H */
