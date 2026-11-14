/*
 * tt_priv.h - private to the sources of the tt modules (src/fit150x/tt*.c):
 * the headers of the other packages, callbacks with the generic list signatures, and
 * what the tt*.c files share among themselves.
 */
#ifndef FIT150X_TT_PRIV_H
#define FIT150X_TT_PRIV_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fit_types.h"
#include "base.h"
#include "min.h"
#include "lib.h"
#include "syn.h"
#include "fit.h"
#include "place.h"
#include "dev.h"
#include "tt.h"

/* Number of items of a list that may be NULL. */
static inline int tt_count(const FitList *l) { return l != NULL ? l->count : 0; }

/* Words needed for n bits, computed the same way everywhere:
 * 1 for n == 0, else ((n - 1) >> 5) + 1 in unsigned arithmetic. */
static inline int tt_words(int n)
{
    return n == 0 ? 1 : (int)(((unsigned)n - 1u) >> 5) + 1;
}

/* a * b in 32-bit unsigned arithmetic (wraps instead of overflowing) */
static inline int tt_mul(int a, int b)
{
    return (int)((unsigned)a * (unsigned)b);
}

/* ---- list callbacks with the generic signatures -------------------- */

static inline void tt_cb_free_tree(void **ref)
{
    FitFctTree *t = *ref;
    fcttree_free(&t);
    *ref = NULL;
}

static inline void tt_cb_free_pterm(void **ref)
{
    FitPTerm *p = *ref;
    pterm_free(&p);
    *ref = NULL;
}

static inline void tt_cb_free_ptermio(void **ref)
{
    FitPTermIO *p = *ref;
    ptermio_free(&p);
    *ref = NULL;
}

static inline void tt_cb_free_var(void **ref)
{
    FitVariable *v = *ref;
    var_free(&v);
    *ref = NULL;
}

static inline void tt_cb_free_list(void **ref)
{
    FitList *l = *ref;
    list_destroy(&l);
    *ref = NULL;
}

static inline void *tt_cb_copy_var(void *item)
{
    return var_copy(item);
}

static inline void tt_cb_print_var(FILE *out, void *item)
{
    var_print(out, item);
}

static inline int tt_cb_str_ieq(void *a, void *b)
{
    return util_str_ieq(a, b);
}

/* ---- shared between the tt*.c files --------------------------------- */

/* Called on a path that would read through a NULL or dangling pointer:
 * "fit150x: internal error: <what>" on stderr, exit status 3.  `what`
 * names the function and the condition. */
void tt_unreachable(const char *what);

/* Stub of a function that exists only for an option the CUPL flow does not
 * pass: "fit150x: <what> is not supported" on stderr, exit status 3. */
void tt_not_supported(const char *what);

/* tt_tt2.c */
FitList *tt2_convert_pla_type(FitList *pla, int n_total, int n_out, const char *type,
                              int *status);
void tt2_report_set_conflict(FILE *out, int status);

/* tt_tt2net.c */
int  tt2_build_instances(FitEntity *entity, FitList *dotted);
void tt2_build_xors(FitEntity *entity, FitList *dotted);
void tt2_merge_pin_inputs(FitEntity *entity, FitList *dotted);
FitList *tt2_collect_dotted(FitEntity *entity);
int  tt2_apply_node_decls(FitEntity *entity);
int  tt2_find_node_decl(const char *name);

/* tt_tt2prop.c */
void tt2_property(void);
void tt2_property_param(FitList *job);

/* tt_tt2read.c: state of one read */
typedef struct TtNodeDecl {
    char *name;
    int   declared;              /* 1; set from the PINS (1) / NODES (0) line */
    int   active;                /* 1; 2 when the name carries a '-' suffix */
} TtNodeDecl;

extern int      g_tt2_next_id;       /* next free variable id */
extern FitList *g_tt2_node_decls;    /* TtNodeDecl */

#endif /* FIT150X_TT_PRIV_H */
