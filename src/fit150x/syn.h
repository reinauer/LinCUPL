/*
 * syn.h - public interface of the synthesis transforms of the ATF150x
 * fitter:
 *   xor_detect   (synxor.c)
 *   factorize    (synfactor.c)
 *   latch_synth  (synlatch.c)
 *
 * Only the functions that other modules call are declared here.
 */
#ifndef FIT150X_SYN_H
#define FIT150X_SYN_H

#include "fit_types.h"

/* ------------------------------------------------------------------ */
/* xor_detect                                                          */
/* ------------------------------------------------------------------ */

/* XOR detection over the functions named in the option list
 * g_opt.xor_list (when that list is empty every function is tried).
 * Prints "   A XOR is detected for the OUTPUT/PIN \t%s" on the message
 * stream for each function rewritten, adds ATM_%d nodes and an "XOR"
 * gate (netlist item of kind 10) per detected function, replaces *entity
 * by the rewritten copy and replaces xor_list by the list of the variable
 * ids for which an XOR was found.  Called once, from fit_prep_detect_xor. */
void xor_detect_entity(FitEntity **entity);

/* ------------------------------------------------------------------ */
/* factorize                                                           */
/* ------------------------------------------------------------------ */

/* Target technology (second argument of factor_entity). The fitter passes
 * FACTOR_TARGET_STANDARD only. */
enum FactorTarget {
    FACTOR_TARGET_STANDARD   = 1,   /* "Factorization for : STANDARD CELLS." */
    FACTOR_TARGET_XILINX     = 3,
    FACTOR_TARGET_ACTEL      = 4,
    FACTOR_TARGET_QUICKLOGIC = 7,
    FACTOR_TARGET_ALTERA     = 9,
    FACTOR_TARGET_LITERALS   = 10
};

/* factorize_entity.  Returns a new entity (the caller keeps
 * `entity`); the netlist of the result is a copy of entity->net.
 *   target     enum FactorTarget
 *   criterion  0 = speed, otherwise area
 *   mode       1 in the fitter (fit_prep_reduce).  0 and 2 are
 *              unsupported options: the call reports it and exits with
 *              status 3.
 *   limit      -1 means 6000; the fitter passes 1
 *   verbose    nonzero prints the "Factorization for : ..." and PASS lines
 *              on the message stream (the fitter passes 0) */
FitEntity *factor_entity(FitEntity *entity, int target, int criterion,
                         int mode, int limit, int verbose);

/* Algebraic division of the product-term list `f` by the list
 * `divisor` (FitPTerm items, `nwords` words per bit vector).
 * Not divisible: *quotient = NULL and *remainder = f itself (not a copy).
 * Otherwise *quotient and *remainder are new lists of new product terms.
 * `boolean_division` nonzero only prints "Boolean Division not
 * implemented !" and returns without touching the outputs.
 * Called from fit_prep_divide_once. */
void factor_divide(FitList *f, FitList *divisor, int nwords,
                   FitList **quotient, FitList **remainder,
                   int boolean_division);

/* One candidate divisor found by factor_find_kernels (allocated
 * with calloc by factor_add_kernel). */
typedef struct FactorKernel {
    FitList *terms;      /* FitPTerm: the kernel (owned) */
    FitList *cokernels;  /* FactorCokernel: where it was found */
} FactorKernel;

typedef struct FactorCokernel {      /* see factor_add_kernel */
    int       fct;       /* index of the function in the list */
    FitPTerm *cube;      /* co-kernel cube (owned): function = cube * kernel + ... */
} FactorCokernel;

/* Collect the kernels of the functions funcs[indices[i]]
 * (lists of FitPTerm; indices holds plain integers; functions with fewer
 * than two terms are skipped) into *kernels (FactorKernel items,
 * appended; *kernels must be a list).  *hash is the bucket table used to
 * find equal kernels (list of lists of FactorKernel, NULL for an empty
 * bucket); a new one with 3 * (largest function size) buckets is stored
 * there when *kernels is empty.  nvars = number of variables (bits) in
 * use; all_levels 0 = kernels of level 0 only (what the fitter passes),
 * nonzero = kernels of every level; the search of one function stops
 * when more than `limit` kernels have been entered for it.
 * Called from fit_prep_divide_once. */
void factor_find_kernels(FitList *funcs, int nvars, int all_levels, int limit,
                         FitList **kernels, FitList **hash, FitList *indices);

/* Number of variables that occur as a literal in at least one
 * term of the list (FitPTerm of nwords words). */
int factor_cover_state(FitList *pterms, int nwords);

/* ------------------------------------------------------------------ */
/* latch_synth: latch detection and netlist utilities                  */
/* ------------------------------------------------------------------ */

/* Builds two one-output entities (outputs named SF%d) from the
 * trees a and b over the variables of `entity` and returns the result of
 * the equivalence check equiv_verify_entities(e1, e2, 1).  The two
 * temporary entities are not freed.  Called from fit_prep.c. */
int latch_trees_equivalent(FitEntity *entity, FitFctTree *a, FitFctTree *b);

/* Latch detection, runs only with -str Latch_Synthesis ON, an
 * unsupported option: reports "fit150x: Latch_Synthesis is not supported"
 * and exits with status 3. */
void latch_synthesis(FitEntity **entity);

/* var_id of the first function of `entity` whose expression is
 * the bare variable `id` (a buffer of it); 0 when there is none.
 * Called from lib_split_output. */
int latch_find_buffer_of(FitEntity *entity, int id);

/* 1-based position of the alias called `name` in net->aliases
 * (comparison tt2_name_ref_equal), 0 when absent or when net / the list
 * is NULL.
 * Used all over the fitter to ask "is this a netlist signal". */
int latch_find_alias(const char *name, FitNetlist *net);

/* Follows a signal back through the cell that drives it:
 * returns `name` itself when it is not a netlist signal or has no driving
 * instance, otherwise the pad / data input of that instance, or the alias
 * target of that input when the target is not a netlist signal.  The
 * result is a pointer into the entity, not a copy. */
char *latch_signal_source(FitEntity *entity, char *name);

/* Removes the netlist items of kind 12 (buffers), rewiring the
 * function, variables and aliases they connect.  Called from fit_driver
 * and from tt3_name_register_inputs. */
void latch_remove_buffers(FitEntity *entity);

/* Bit set (one bit per list position, calloc'ed, the caller
 * frees it) of the variables of `vars` whose polarity is -1; NULL when
 * vars is NULL.  Called from tt3_write_pla. */
unsigned *latch_inverted_set(FitList *vars);

/* Fix-up after reading: netlist items of kind 13 with buf_type == 3
 * or 4 become tri-state cells (kind 8).  Called from main_fit_job. */
void latch_fixup_entity(FitEntity *entity);

/* names (not owned) of the kind-13 cells with buf_type == 3 seen by
 * latch_fixup_entity; nothing in the fitter reads it. */
extern FitList *g_latch_tristate_names;

#endif /* FIT150X_SYN_H */
