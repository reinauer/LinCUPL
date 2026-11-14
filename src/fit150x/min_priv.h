/*
 * min_priv.h - declarations shared between the files of the min modules
 * (src/fit150x/min*.c) that are not part of its public interface (min.h).
 */
#ifndef FIT150X_MIN_PRIV_H
#define FIT150X_MIN_PRIV_H

#include "base.h"
#include "min.h"

/* Number of items of a list that may be NULL. */
static inline int min_count(const FitList *l)
{
    return l != NULL ? l->count : 0;
}

/* Number of 32-bit words for n variables: 1 for n == 0. */
static inline int min_words(unsigned n)
{
    return n == 0 ? 1 : (int)((n - 1) >> 5) + 1;
}

/* FitListFree shapes for the typed destructors of base.h */
void min_free_pterm_cb(void **item);     /* pterm_free */
void min_free_ptermio_cb(void **item);   /* ptermio_free */
void min_free_block_cb(void **item);     /* util_free_ptr */
void *min_copy_var_cb(void *item);       /* var_copy */

/* min_bdd.c */
/* bdd_build_entity_ex with 5,000,000 steps and blocks of 500 */
void bdd_build_entity_500(FitEntity *entity, FitList **roots, FitList **node_blocks,
                          FitList **unique, FitBddNode **terminal, int *n_nodes,
                          int max_nodes);
/* 1 when the two marked BDDs have the same shape and marks */
int bdd_equal(FitBddNode *a, int a_mark, FitBddNode *b, int b_mark);
/* list of the names (char *, not owned) of the first two roots
 * that differ between the two root lists */
FitList *bdd_compare_roots(FitList *roots_a, FitList *roots_b, FitVariable *first,
                           FitVariable *second);

/* min_equiv.c */
/* new entity holding output out_idx of src (and out_idx2 when
 * step != 1 and it exists) with the local functions they depend on */
FitEntity *equiv_extract_cone(FitEntity *src, int out_idx, int out_idx2, int step);

/* min_esp.c: release the cube structure and the free list of set family
 * headers (part of min_reset) */
void min_esp_reset(void);

/* min_bitvec.c */
unsigned *bitvec_new_filled(int n_words, unsigned value);
unsigned *bitvec_copy_new(const unsigned *src, int n_words);

#endif /* FIT150X_MIN_PRIV_H */
