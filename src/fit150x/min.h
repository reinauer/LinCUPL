/*
 * min.h - public interface of the min modules of the ATF150x fitter,
 * two-level minimisation:
 *   min_bdd.c      a BDD package (nodes from pools, unique table, computed
 *                  table) and the operations on those BDDs
 *   min_equiv.c    equivalence check of two entities
 *   min_domlist.c  minimisation of product-term lists
 *   min_bitvec.c   bit vectors
 *   min_esp.c      the fitter's own Espresso-style minimiser
 *
 * Only the functions other modules call are declared here.
 */
#ifndef FIT150X_MIN_H
#define FIT150X_MIN_H

#include "fit_types.h"

/* The BDD types FitBddNode, FitBddBucket, FitBddRoot, FitBddInfo and the
 * marks FIT_BDD_DIRECT / FIT_BDD_INVERTED are at the end of fit_types.h. */

/* Puts the file-scope state of these modules back to what it is when the
 * program starts (g_equiv_verbose = 0, no pools, no Espresso cube
 * structure) and releases what they still hold.  To be called at the
 * start of a run when several runs share a process. */
void min_reset(void);

/* ------------------------------------------------------------------ */
/* min_bdd.c: BDDs of the functions of an entity                       */
/* ------------------------------------------------------------------ */

/* build the BDD of every output of `entity` (pool blocks of 100
 * cells, at most 5,000,000 apply steps).  *roots: list of FitBddRoot, one per
 * output, in output order; *node_blocks: list of the node pool blocks (free
 * with bdd_free_blocks); *unique: the unique table, list of FitBddBucket
 * (free with bdd_free_unique_table); *terminal: the constant node;
 * *n_nodes: number of nodes reachable from the outputs (counting the
 * terminal), -1 when it exceeds max_nodes, -2 when a function could not be
 * built. */
void bdd_build_entity_100(FitEntity *entity, FitList **roots, FitList **node_blocks,
                          FitList **unique, FitBddNode **terminal, int *n_nodes,
                          int max_nodes);

/* the same with the step limit and the pool block size given. */
void bdd_build_entity_ex(FitEntity *entity, FitList **roots, FitList **node_blocks,
                         FitList **unique, FitBddNode **terminal, int *n_nodes,
                         int max_nodes, int step_limit, int block_cells);

/* ------------------------------------------------------------------ */
/* min_bdd.c: operations on BDDs                                       */
/* ------------------------------------------------------------------ */

/* free a unique table (the buckets and their lists, not the nodes). */
void bdd_free_unique_table(FitList *unique);

/* free a list of malloc'ed blocks and the list. */
void bdd_free_blocks(FitList *blocks);

/* free the FitBddInfo blocks made by bdd_annotate (200 records
 * per block, each with a parents list) and the list. */
void bdd_free_info_blocks(FitList *info_blocks);

/* append a new local variable to entity->locals; its id is the
 * next free id (tt2_next_var_id) and its name is "SF" followed by that
 * id.  Returns the id. */
int bdd_new_local_var(FitEntity *entity);

/* build a new entity from BDD roots: inputs and outputs copied
 * from `src`, one function per root as a multiplexer tree, shared nodes
 * turned into new local variables.  NULL on error. */
FitEntity *bdd_to_entity(FitList *roots, FitEntity *src);

/* attach a FitBddInfo (parents list, level) to every node
 * reachable from `roots`; *info_blocks receives the list of record blocks
 * (free with bdd_free_info_blocks). */
void bdd_annotate(FitList *roots, FitList **info_blocks, FitBddNode *terminal);

/* list of lists of nodes by level (index = FitBddInfo.level,
 * 0..level of the terminal), in the order a depth-first walk from the roots
 * meets them. */
FitList *bdd_nodes_by_level(FitList *roots, FitBddNode *terminal);

/* make a copy of `node` with both edge marks swapped,
 * enter it into the unique table `unique` and into the parents
 * lists of its children; its FitBddInfo is a copy of the node's with
 * `parents` as parents list.  With parents == NULL the copy is appended to
 * `top_nodes`, otherwise its level is 1 + the highest level of the parents
 * and it is appended to by_level[that level]. */
FitBddNode *bdd_clone_inverted(FitBddNode *node, FitList *parents, FitList *top_nodes,
                               FitList *unique, FitList *node_blocks,
                               FitList *info_blocks, FitList *by_level);

/* split the nodes of `parents` that point to `node` into those
 * that do so through a direct edge (*direct) and through an inverted edge
 * (*inverted); a node pointing to it with both edges is entered twice. */
void bdd_split_parents(FitBddNode *node, FitList *parents, FitList **direct,
                       FitList **inverted);

/* ------------------------------------------------------------------ */
/* min_equiv.c: equivalence check                                      */
/* ------------------------------------------------------------------ */

/* message level; this module prints its messages when it is not
 * 0.  Written by alloc.c and read by other stages of the fitter as
 * well. */
extern int g_equiv_verbose;

/* "Verifying equivalence ...": 1 when the two entities have the
 * same inputs, outputs and netlist and the BDDs of corresponding outputs are
 * equal; `step` is 1 (one output at a time) or 2 (pairs of outputs). */
int equiv_verify_entities(FitEntity *a, FitEntity *b, int step);

/* ------------------------------------------------------------------ */
/* min_domlist.c: minimisation of product-term lists                   */
/* ------------------------------------------------------------------ */

/* never seen running: rebuild a product-term list from the
 * list `pterms` restricted to the variables in `varset`; returns a new list
 * of new FitPTerm. */
FitList *domlist_factor_cover(FitList *pterms, int n_words, unsigned *varset);

/* copy of `entity` with every function flattened to a sum of
 * products (entity_flatten with `depth`) and minimised term by
 * term.  Called with (entity, 1, 1, 0x32); with use_local != 1 it
 * prints "La on aurait utilise la min globale". */
FitEntity *domlist_minimise_entity(FitEntity *entity, int use_local, int unused,
                                   int depth);

/* minimised copy of one function tree of `entity` (NULL for
 * NULL). */
FitFctTree *domlist_minimise_tree(FitEntity *entity, FitFctTree *tree);

/* never seen running: union of two product-term lists made
 * irredundant through domlist_factor_cover; n_vars is the number of
 * variables. */
FitList *domlist_merge_covers(FitList *a, FitList *b, int n_vars);

/* minimise a list of product terms (FitPTerm of n_vars
 * variables): returns a new list holding the terms of `fixed` (may be NULL;
 * they are only used as don't-care context and are removed from the result)
 * followed by the reduced terms of `pterms`.  flags is a mask of bits to
 * skip: the callers pass 0xfffffffb (run the final redundancy removal, not
 * the consensus step). */
FitList *domlist_minimise_pterms(FitList *pterms, FitList *fixed, unsigned n_vars,
                                 unsigned flags);

/* ------------------------------------------------------------------ */
/* min_bitvec.c: arrays of 32-bit words, variable v (1-based) is       */
/* bit (v-1)&31 of word (v-1)>>5                                       */
/* ------------------------------------------------------------------ */

/* every word = value */
void bitvec_fill(unsigned *v, int n_words, unsigned value);
/* calloc of n_words+1 words */
unsigned *bitvec_alloc_plus1(int n_words);
/* calloc of n_words words, all 0 */
unsigned *bitvec_new_zero(int n_words);
/* bit of variable `var` */
int bitvec_test(const unsigned *v, int var);
/* number of 1 bits */
int bitvec_count(const unsigned *v, int n_words);
void bitvec_set(unsigned *v, int var);
void bitvec_clear(unsigned *v, int var);
/* new vector (bitvec_alloc_plus1) = ~(a ^ b) */
unsigned *bitvec_xnor_new(const unsigned *a, const unsigned *b, int n_words);
/* out = ~a; returns 1 when the result is not all 0 */
int bitvec_not(const unsigned *a, unsigned *out, int n_words);
/* out = a & b; returns 1 when the result is not all 0 */
int bitvec_and(const unsigned *a, const unsigned *b, unsigned *out, int n_words);
/* out = a | b; returns 1 when the result is not all 0 */
int bitvec_or(const unsigned *a, const unsigned *b, unsigned *out, int n_words);
/* out = ~(a | b); returns 1 when the result is not all 0 */
int bitvec_nor(const unsigned *a, const unsigned *b, unsigned *out, int n_words);
/* out = ~(a ^ b) */
void bitvec_xnor(const unsigned *a, const unsigned *b, unsigned *out, int n_words);

/* Unsupported equation input, exits with status 3. */
int bitvec_parse_eqn_cube(char *text, int *a, int *b, unsigned n);

/* ------------------------------------------------------------------ */
/* min_esp.c: Espresso-style minimiser                                 */
/* ------------------------------------------------------------------ */

/* new entity whose functions are the complements of the
 * functions of `entity`: the entity is written as a PLA, complemented with
 * the Espresso-style `complement` of min_esp.c, read back and minimised with
 * domlist_minimise_entity(e, 1, 1, 0x32). */
FitEntity *esp_complement_entity(FitEntity *entity);

/* 1 when the signal `name` is a control input (clk, ar, ap, oe or ce of
 * the FitInstance) of a netlist item of kind 2, 3, 4 or 8 of `entity`. */
int esp_signal_is_control(const char *name, FitEntity *entity);

/* 1 when `var` is such a control signal and its function is not
 * a sum of two or more terms. */
int esp_var_is_simple_control(FitEntity *entity, FitVariable *var);

#endif /* FIT150X_MIN_H */
