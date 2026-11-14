/*
 * synfactor.c - factorize module of the ATF150x fitter: algebraic
 * factorisation of the functions of an entity.
 *
 * The functions are held as lists of product terms (FitPTerm) over the
 * inputs followed by the locals; every extracted divisor becomes a new
 * function appended to the list and a new variable (the next bit).  One
 * pass (factor_one_pass) runs 13 steps with a falling gain threshold; a
 * step extracts multi-term divisors (kernels, factor_kernel_loop), merges
 * the single-literal terms of a function into one new function, then
 * extracts common cubes (factor_cube_loop).
 *
 * Literal coding of a FitPTerm (bit of `bits`, bit of `care`):
 *   (0,1) variable absent, (1,1) positive literal, (0,0) negative literal.
 *
 * Unsupported options, refused by factor_entity:
 *   - mode 0 (third argument of factor_entity; the fitter always passes 1):
 *     the kernel loop without complement division.
 *   - mode 2 (parity / three-term functions kept apart).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fit_types.h"
#include "base.h"
#include "min.h"
#include "lib.h"
#include "tt.h"
#include "syn.h"

#define LCOUNT(l) ((l) != NULL ? (l)->count : 0)

/* A common cube and the terms that hold it (made by
 * factor_add_cube_candidate). */
typedef struct FactorCube {
    FitList  *terms;   /* FitPTerm, shared with the function lists */
    FitPTerm *cube;
    int       gain;
} FactorCube;

/* gain of a candidate: (entity, functions, kernel terms, co-kernel list)
 * or (entity, functions, terms holding the cube, cube) */
typedef int (*FactorGainFn)(FitEntity *e, FitList *funcs, FitList *terms, void *aux, int nwords);
/* choice among the candidates of highest gain: positions in `best` */
typedef int (*FactorChooseFn)(FitEntity *e, FitList *funcs, FitList *cands, FitList *best,
                              int nwords);

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

/* parameters of a run, set by factor_run_with */
static int g_fac_kernel_limit;          /* kernels taken per function */
static int g_fac_mode;
static FactorChooseFn g_fac_cube_choose;
static FactorGainFn   g_fac_kernel_gain;
static int g_fac_speed_mode;            /* set for the SPEED criterion; never read */
static FactorChooseFn g_fac_kernel_choose;
static int g_fac_criterion;
static FactorGainFn   g_fac_cube_gain;
static int g_fac_all_levels;            /* 0: level-0 kernels only */
static int g_fac_kind;
static int g_fac_min_literals;          /* smallest cube worth extracting */
static int g_fac_cube_limit;
static int g_fac_all_cubes;             /* 0: maximal cubes only */
static int g_fac_max_pass;

/* the function lists in work */
static int g_fac_nwords;                /* words per bit vector */
static int g_fac_nvars;                 /* variables in use */

/* kernel and cube search (factor_kernels_of_function,
 * factor_find_common_cubes) */
static int       g_kf_limit;
static FitList  *g_kf_tmp;              /* created and released by factor_find_common_cubes, otherwise unused */
static int       g_kf_nvars;
static FitList  *g_kf_hash;             /* buckets (lists of FactorKernel) */
static int       g_kf_fct;              /* index of the function searched */
static FitList  *g_kf_out;              /* FactorKernel or FactorCube */
static int       g_kf_nwords;
static int       g_kf_count;            /* candidates found so far */
static FitPTerm *g_kf_cube;             /* scratch cube */

/* thresholds of the 13 steps of a pass */
static const int g_fac_thresholds[13] = { 100, 90, 80, 70, 60, 50, 40, 30, 20, 14, 8, 1, 0 };

/* ------------------------------------------------------------------ */
/* list callbacks with the shapes the list package wants               */
/* ------------------------------------------------------------------ */

static void *fac_copy_pterm(void *p, void *nwords)      /* pterm_copy as FitListCopy2 */
{
    return pterm_copy(p, FIT_PTR2INT(nwords));
}

static void fac_free_pterm(void **slot)                 /* pterm_free as FitListFree */
{
    FitPTerm *p = *slot;

    pterm_free(&p);
    *slot = p;
}

static void fac_destroy_list(void **slot)               /* list_destroy as FitListFree */
{
    FitList *l = *slot;

    list_destroy(&l);
    *slot = l;
}

static void *fac_copy_var(void *v)                      /* var_copy as FitListCopy */
{
    return var_copy(v);
}

static void fac_free_var(void **slot)                   /* var_free as FitListFree */
{
    FitVariable *v = *slot;

    var_free(&v);
    *slot = v;
}

static int fac_nwords_for(int nvars)
{
    if (nvars == 0)
        return 1;
    return (int)(((unsigned)nvars - 1u) >> 5) + 1;
}

/* ------------------------------------------------------------------ */
/* product-term primitives                                             */
/* ------------------------------------------------------------------ */

/* dst = the literals a and b have in common */
static void factor_pterm_common(FitPTerm *a, FitPTerm *b, FitPTerm *dst, int nwords)
{
    while (nwords != 0) {
        nwords--;
        dst->bits[nwords] = a->bits[nwords] & b->bits[nwords];
        dst->care[nwords] = a->care[nwords] | b->care[nwords];
    }
}

/* 1 when every literal of b is a literal of a */
static int factor_pterm_divides(FitPTerm *a, FitPTerm *b, int nwords)
{
    int k;

    for (k = 0; k < nwords; k++)
        if ((b->care[k] & ~(a->care[k] & a->bits[k]) & b->bits[k]) != 0)
            return 0;
    for (k = 0; k < nwords; k++)
        if ((~(b->care[k] | b->bits[k]) & (a->bits[k] | a->care[k])) != 0)
            return 0;
    return 1;
}

/* dst = a without the literals of b */
static void factor_pterm_quotient(FitPTerm *a, FitPTerm *b, FitPTerm *dst, int nwords)
{
    while (nwords != 0) {
        unsigned u;

        nwords--;
        u = b->care[nwords] ^ b->bits[nwords];
        dst->bits[nwords] = a->bits[nwords] & u;
        dst->care[nwords] = a->care[nwords] | ~u;
    }
}

/* quotients of the terms that `cube` divides, appended to out */
static void factor_divide_by_cube(FitList *terms, FitPTerm *cube, FitList *out, int nwords)
{
    int i;

    for (i = 0; i < LCOUNT(terms); i++) {
        if (factor_pterm_divides(terms->items[i], cube, nwords) != 0) {
            FitPTerm *q = pterm_new_raw(nwords);

            factor_pterm_quotient(terms->items[i], cube, q, nwords);
            list_add(out, q);
        }
    }
}

static int factor_count_literals(FitList *terms, int nwords)
{
    int n = 0;
    int i;

    for (i = 0; i < LCOUNT(terms); i++)
        n += pterm_count_literals(terms->items[i], nwords);
    return n;
}

int factor_cover_state(FitList *pterms, int nwords)
{
    unsigned *one = bitvec_new_zero(nwords);
    unsigned *all = bitvec_new_zero(nwords);
    int i, n;

    for (i = 0; i < LCOUNT(pterms); i++) {
        FitPTerm *p = pterms->items[i];

        bitvec_xnor(p->bits, p->care, one, nwords);
        bitvec_or(all, one, all, nwords);
    }
    n = bitvec_count(all, nwords);
    free(one);
    free(all);
    return n;
}

/* ------------------------------------------------------------------ */
/* algebraic division                                                  */
/* ------------------------------------------------------------------ */

/* f / divisor by intersecting the quotients by each cube of the
 * divisor; the quotient terms are appended to out (nothing when empty) */
static void factor_weak_divide(FitList *f, FitList *divisor, FitList *out, int nwords)
{
    FitList *acc;
    int i;

    if (divisor == NULL || divisor->count == 0)
        return;
    acc = list_create();
    factor_divide_by_cube(f, divisor->items[0], acc, nwords);
    for (i = 1; i < divisor->count; i++) {
        FitList *tmp = list_create();
        FitList *both;

        factor_divide_by_cube(f, divisor->items[i], tmp, nwords);
        both = list_intersection_mapped(acc, tmp, pterm_equal, fac_copy_pterm,
                                        FIT_INT2PTR(nwords));
        list_free_all(&tmp, fac_free_pterm);
        list_free_all(&acc, fac_free_pterm);
        if (both == NULL)
            return;
        acc = both;
    }
    list_append_destroy(out, &acc);
}

static int factor_is_divisible(FitList *f, FitList *divisor, int nwords)
{
    FitList *q = list_create();
    int n;

    factor_weak_divide(f, divisor, q, nwords);
    n = q->count;
    list_free_all(&q, fac_free_pterm);
    return n != 0;
}

/* for each term of f, the literals it has in common with every
 * term of the divisor */
static FitList *factor_div_common_cubes(FitList *f, FitList *divisor, int nwords)
{
    FitList *res = list_create_sized(LCOUNT(f));
    FitList *tmp = list_create_sized(LCOUNT(divisor) * LCOUNT(f));
    int i, j;

    for (i = 0; i < LCOUNT(f); i++) {
        int k = LCOUNT(tmp);
        FitPTerm *acc;

        for (j = 0; j < LCOUNT(divisor); j++) {
            FitPTerm *p = util_calloc(1, sizeof *p);

            pterm_alloc(p, nwords);
            factor_pterm_common(f->items[i], divisor->items[j], p, nwords);
            list_add(tmp, p);
        }
        acc = k < LCOUNT(tmp) ? pterm_copy(tmp->items[k], nwords) : NULL;
        while (k < LCOUNT(tmp) - 1) {
            FitPTerm *n = pterm_and(acc, tmp->items[k + 1], nwords);

            pterm_free(&acc);
            k++;
            acc = n;
        }
        list_add(res, acc);
    }
    list_free_all(&tmp, fac_free_pterm);
    return res;
}

/* for each term of f, what is left of it after taking away the
 * divisor terms that divide it */
static FitList *factor_div_quotient_cubes(FitList *f, FitList *divisor, int nwords)
{
    FitList *res = list_create_sized(LCOUNT(f));
    FitList *tmp = list_create_sized(LCOUNT(divisor) * LCOUNT(f));
    int i, j;

    for (i = 0; i < LCOUNT(f); i++) {
        int found = 0;
        int k = LCOUNT(tmp);
        FitPTerm *src;

        for (j = 0; j < LCOUNT(divisor); j++) {
            if (factor_pterm_divides(f->items[i], divisor->items[j], nwords) != 0) {
                FitPTerm *p = util_calloc(1, sizeof *p);

                pterm_alloc(p, nwords);
                factor_pterm_quotient(f->items[i], divisor->items[j], p, nwords);
                list_add(tmp, p);
                found++;
            }
            if (factor_pterm_divides(divisor->items[j], f->items[i], nwords) != 0) {
                FitPTerm *p = util_calloc(1, sizeof *p);

                pterm_alloc(p, nwords);
                pterm_init_with_value(p, "-", nwords);
                list_add(tmp, p);
                found++;
            }
        }
        if (found == 0) {
            src = f->items[i];
        } else {
            /* the last quotient found is never folded in; deliberate, the
             * factored result depends on it */
            src = tmp->items[k];
            while (k < LCOUNT(tmp) - 1) {
                factor_pterm_common(src, tmp->items[k], src, nwords);
                k++;
            }
        }
        list_add(res, pterm_copy(src, nwords));
    }
    list_free_all(&tmp, fac_free_pterm);
    return res;
}

/* the terms of lists[0] that occur in every other list (copies) */
static FitList *factor_div_intersect_quotients(FitList *lists, int nwords)
{
    FitList *acc = list_map_copy_x(lists->items[0], fac_copy_pterm, FIT_INT2PTR(nwords));
    int i, j;

    for (i = 1; i < LCOUNT(lists); i++) {
        FitList *next = list_create();

        for (j = 0; j < LCOUNT(acc); j++) {
            if (list_find_x(lists->items[i], acc->items[j], pterm_equal, FIT_INT2PTR(nwords)) != 0)
                list_add(next, pterm_copy(acc->items[j], nwords));
        }
        list_free_all(&acc, fac_free_pterm);
        acc = list_map_copy_x(next, fac_copy_pterm, FIT_INT2PTR(nwords));
        list_free_all(&next, fac_free_pterm);
    }
    return acc;
}

void factor_divide(FitList *f, FitList *divisor, int nwords, FitList **quotient,
                   FitList **remainder, int boolean_division)
{
    FitList *common, *qcubes, *all, *per, *q, *prod, *rest;
    int total = 0;
    int i, j, k;

    if (boolean_division != 0) {
        fprintf(g_msg, "Boolean Division not implemented !\n");
        return;
    }
    if (!factor_is_divisible(f, divisor, nwords)) {
        *quotient = NULL;
        *remainder = f;
        return;
    }
    common = factor_div_common_cubes(f, divisor, nwords);
    qcubes = factor_div_quotient_cubes(f, divisor, nwords);
    all = list_create();
    per = list_create();
    for (i = 0; i < LCOUNT(divisor); i++) {
        FitList *l;

        k = LCOUNT(all);
        for (j = 0; j < LCOUNT(common); j++) {
            if (pterm_equal(divisor->items[i], common->items[j], FIT_INT2PTR(nwords)) != 0) {
                list_add(all, pterm_copy(qcubes->items[j], nwords));
                total++;
            }
        }
        l = list_create();
        for (; k < total; k++)
            list_add(l, pterm_copy(all->items[k], nwords));
        list_add(per, l);
    }
    list_free_all(&all, fac_free_pterm);
    list_free_all(&qcubes, fac_free_pterm);
    list_free_all(&common, fac_free_pterm);

    q = factor_div_intersect_quotients(per, nwords);
    *quotient = q;
    prod = pterm_list_and(divisor, q, nwords);
    *remainder = prod;
    rest = list_difference_x(f, prod, pterm_equal, FIT_INT2PTR(nwords));
    list_free_all(remainder, fac_free_pterm);
    *remainder = rest;

    for (i = 0; i < LCOUNT(per); i++) {
        FitList *l = per->items[i];

        list_free_all(&l, fac_free_pterm);
        per->items[i] = l;
    }
    list_free_all(&per, fac_destroy_list);
}

/* ------------------------------------------------------------------ */
/* candidates                                                          */
/* ------------------------------------------------------------------ */

/* FitListFree */
static void factor_free_cokernel(void **slot)
{
    FactorCokernel *c = *slot;

    if (c != NULL) {
        pterm_free(&c->cube);
        free(c);
        *slot = NULL;
    }
}

static void factor_free_kernel(FactorKernel **ref)
{
    FactorKernel *k = *ref;

    if (k != NULL) {
        if (k->terms != NULL)
            list_free_all(&k->terms, fac_free_pterm);
        if (k->cokernels != NULL)
            list_free_all(&k->cokernels, factor_free_cokernel);
        free(k);
        *ref = NULL;
    }
}

static void factor_free_cube(FactorCube *c)
{
    list_destroy(&c->terms);
    pterm_free(&c->cube);
    free(c);
}

static void factor_add_cube_candidate(FitList *terms, FitPTerm *cube)
{
    FactorCube *c = util_calloc(1, sizeof *c);

    c->terms = terms;
    c->cube = cube;
    list_add(g_kf_out, c);
}

/* enter the kernel `terms` found in function `fct` with
 * co-kernel `cube`.  A kernel already known (same terms, found through
 * the hash table) only gets a new co-kernel and `terms` is released. */
static void factor_add_kernel(FitList *terms, FitPTerm *cube, int fct)
{
    FactorCokernel *ck = util_calloc(1, sizeof *ck);
    FactorKernel *k;
    FitList *bucket;
    int nbuckets, h, i;

    ck->fct = fct;
    ck->cube = cube;
    nbuckets = LCOUNT(g_kf_hash);
    if (nbuckets == 0)     /* cannot happen: the table has 3 * (largest function) buckets */
        util_abort_job(0);
    h = (factor_cover_state(terms, g_kf_nwords) +
         (LCOUNT(terms) + factor_count_literals(terms, g_kf_nwords) * 10) * 2) % nbuckets;
    bucket = g_kf_hash->items[h];
    for (i = 0; i < LCOUNT(bucket); i++) {
        k = bucket->items[i];
        if (LCOUNT(terms) == LCOUNT(k->terms) &&
            list_equal_sets_x(k->terms, terms, pterm_equal, FIT_INT2PTR(g_kf_nwords)) != 0)
            break;
    }
    if (i != LCOUNT(bucket)) {
        k = bucket->items[i];
        list_add(k->cokernels, ck);
        list_free_all(&terms, fac_free_pterm);
        return;
    }
    k = util_calloc(1, sizeof *k);
    k->terms = terms;
    k->cokernels = list_create_sized(1);
    list_add(k->cokernels, ck);
    if (g_kf_hash->items[h] == NULL)
        g_kf_hash->items[h] = list_create_sized(1);
    list_add(g_kf_hash->items[h], k);
    list_add(g_kf_out, k);
}

/* ------------------------------------------------------------------ */
/* kernel search                                                       */
/* ------------------------------------------------------------------ */

/* g_kf_cube-style: cube = common literals of the terms that hold
 * literal `lit`; 1 when more than one term holds it */
static int factor_cube_of_literal(FitList *terms, unsigned lit, FitPTerm *cube)
{
    int n = 0;
    int i;

    for (i = 0; i < LCOUNT(terms); i++) {
        if (pterm_has_literal(terms->items[i], lit) != 0) {
            if (n == 0)
                pterm_init_with_value(cube, "?", g_kf_nwords);
            factor_pterm_common(cube, terms->items[i], cube, g_kf_nwords);
            n++;
        }
    }
    return n > 1;
}

/* the same, also giving the list of those terms in *sub.
 * 0 (and no list) when one term or all terms hold the literal. */
static int factor_cube_of_literal_list(FitList *terms, unsigned lit, FitPTerm *cube,
                                       FitList **sub)
{
    int n = 0;
    int i;

    for (i = 0; i < LCOUNT(terms); i++) {
        if (pterm_has_literal(terms->items[i], lit) != 0) {
            if (n == 0) {
                pterm_init_with_value(cube, "?", g_kf_nwords);
                *sub = list_create_sized(0);
            }
            factor_pterm_common(cube, terms->items[i], cube, g_kf_nwords);
            n++;
            list_add(*sub, terms->items[i]);
        }
    }
    if (n != 1 && n != LCOUNT(terms))
        return n != 0;
    if (n != 0)      /* *sub exists only when n != 0; terms is never empty */
        list_destroy(sub);
    return 0;
}

/* 1 when the cube has no literal on a variable before the one
 * of literal `lit` (and one at or after it in the same word) */
static int factor_cube_is_new_at(int lit, FitPTerm *cube)
{
    int half = lit / 2;
    unsigned last = (unsigned)half >> 5;
    unsigned w;

    for (w = 0; w < last; w++)
        if (~(cube->care[w] ^ cube->bits[w]) != 0)
            return 0;
    return !(util_lowest_set_bit(~(cube->care[last] ^ cube->bits[last])) <
             ((unsigned)half & 0x1fu) + 1);
}

/* kernels of level 0 of `terms` (a cube-free quotient reached
 * with co-kernel `cube`), looking at the literals from `start` on.
 * Takes ownership of terms and cube. */
static void factor_kernels_rec_level0(FitList *terms, int start, FitPTerm *cube)
{
    int leaf = 1;
    int l;

    for (l = 0; l < start; l++) {
        if (factor_cube_of_literal(terms, (unsigned)l, g_kf_cube)) {
            leaf = 0;
            break;
        }
    }
    for (l = start; l < g_kf_nvars * 2; l++) {
        if (!factor_cube_of_literal(terms, (unsigned)l, g_kf_cube))
            continue;
        leaf = 0;
        if (factor_cube_is_new_at(l, g_kf_cube)) {
            FitList *q = list_create();
            FitPTerm *c;

            factor_divide_by_cube(terms, g_kf_cube, q, g_kf_nwords);
            c = pterm_copy(cube, g_kf_nwords);
            pterm_and_to(c, g_kf_cube, c, g_kf_nwords);
            factor_kernels_rec_level0(q, (l - l % 2) + 2, c);
            if (g_kf_limit < g_kf_count)
                goto discard;
        }
    }
    if (leaf && g_kf_count <= g_kf_limit) {
        int before = LCOUNT(g_kf_out);

        factor_add_kernel(terms, cube, g_kf_fct);
        if (LCOUNT(g_kf_out) != before)
            g_kf_count++;
        return;
    }
discard:
    list_free_all(&terms, fac_free_pterm);
    pterm_free(&cube);
}

/* kernels of every level */
static void factor_kernels_rec(FitList *terms, int start, FitPTerm *cube)
{
    int before;
    int l;

    for (l = start; l < g_kf_nvars * 2; l++) {
        if (factor_cube_of_literal(terms, (unsigned)l, g_kf_cube) &&
            factor_cube_is_new_at(l, g_kf_cube)) {
            FitList *q = list_create_sized(2);
            FitPTerm *c;

            factor_divide_by_cube(terms, g_kf_cube, q, g_kf_nwords);
            c = pterm_copy(cube, g_kf_nwords);
            pterm_and_to(c, g_kf_cube, c, g_kf_nwords);
            factor_kernels_rec(q, (l - l % 2) + 2, c);
            if (g_kf_limit < g_kf_count) {
                list_free_all(&terms, fac_free_pterm);
                pterm_free(&cube);
                return;
            }
        }
    }
    before = LCOUNT(g_kf_out);
    factor_add_kernel(terms, cube, g_kf_fct);
    if (LCOUNT(g_kf_out) != before)
        g_kf_count++;
}

static void factor_kernels_of_function(int all_levels, FitList *f, int fct, FitList *kernels,
                                       FitList *hash, int limit, int nvars, int nwords)
{
    FitList *q;
    FitPTerm *c;
    int i;

    g_kf_out = kernels;
    g_kf_hash = hash;
    g_kf_limit = limit;
    g_kf_nwords = nwords;
    g_kf_count = 0;
    g_kf_nvars = nvars;
    g_kf_fct = fct;
    g_kf_cube = pterm_new_raw(nwords);
    pterm_init_with_value(g_kf_cube, "?", g_kf_nwords);
    for (i = 0; i < LCOUNT(f); i++)
        factor_pterm_common(g_kf_cube, f->items[i], g_kf_cube, g_kf_nwords);
    q = list_create_sized(0);
    factor_divide_by_cube(f, g_kf_cube, q, g_kf_nwords);
    c = pterm_copy(g_kf_cube, g_kf_nwords);
    if (all_levels == 0)
        factor_kernels_rec_level0(q, 0, c);
    else
        factor_kernels_rec(q, 0, c);
    pterm_free(&g_kf_cube);
}

void factor_find_kernels(FitList *funcs, int nvars, int all_levels, int limit,
                         FitList **kernels, FitList **hash, FitList *indices)
{
    int i;

    if (*kernels == NULL || (*kernels)->count == 0) {
        int largest = 0;
        FitList *h;

        for (i = 0; i < LCOUNT(funcs); i++) {
            int n = LCOUNT((FitList *)funcs->items[i]);

            if (largest < n)
                largest = n;
        }
        h = list_create_sized(largest * 3);
        *hash = h;
        h->count = largest * 3;
    }
    for (i = 0; i < LCOUNT(indices); i++) {
        int idx = FIT_PTR2INT(indices->items[i]);
        FitList *f = funcs->items[idx];

        if (f != NULL && f->count > 1)
            factor_kernels_of_function(all_levels, f, idx, *kernels, *hash, limit, nvars,
                                       fac_nwords_for(nvars));
    }
}

/* ------------------------------------------------------------------ */
/* common cube search                                                  */
/* ------------------------------------------------------------------ */

static void factor_terms_containing(FitList *terms, FitPTerm *cube, FitList *out)
{
    int i;

    for (i = 0; i < LCOUNT(terms); i++)
        if (pterm_covers(terms->items[i], cube, g_kf_nwords) != 0)
            list_add(out, terms->items[i]);
}

/* the cube found for literal `lit` adds no literal before that
 * literal's variable to `parent`, and the variable itself is new */
static int factor_cube_extends(int lit, FitPTerm *cube, FitPTerm *parent)
{
    int half = lit / 2;
    int i = 0;
    int v;

    for (v = 1; i < half; i++, v++) {
        if (bitvec_test(cube->care, v) == bitvec_test(cube->bits, v)) {
            if (bitvec_test(parent->care, v) != bitvec_test(parent->bits, v))
                return 0;
        }
    }
    return bitvec_test(parent->care, i + 1) != bitvec_test(parent->bits, i + 1);
}

/* cubes common to at least two terms that cannot be extended
 * (maximal for their set of terms).  Takes ownership of terms and cube. */
static void factor_cubes_rec_maximal(FitList *terms, int start, FitPTerm *cube)
{
    FitList *sub = NULL;
    int maximal = 1;
    int l;

    if (start < g_kf_nvars * 2) {
        for (l = start; l < g_kf_nvars * 2; l++) {
            if (factor_cube_of_literal_list(terms, (unsigned)l, g_kf_cube, &sub) &&
                factor_cube_extends(l, g_kf_cube, cube)) {
                FitPTerm *c;

                maximal = 0;
                c = pterm_copy(g_kf_cube, g_kf_nwords);
                factor_cubes_rec_maximal(sub, (l - l % 2) + 2, c);
                if (g_kf_limit < g_kf_count)
                    goto discard;
            }
        }
        if (!maximal)
            goto discard;
    }
    for (l = 0; l < start; l++) {
        if (factor_cube_of_literal_list(terms, (unsigned)l, g_kf_cube, &sub)) {
            list_destroy(&sub);
            goto discard;
        }
    }
    if (maximal) {
        factor_add_cube_candidate(terms, cube);
        g_kf_count++;
        return;
    }
discard:
    list_destroy(&terms);
    pterm_free(&cube);
}

/* every cube of two or more literals common to at least two
 * terms */
static void factor_cubes_rec(FitList *terms, int start, FitPTerm *cube)
{
    FitList *sub = NULL;
    int l;

    if (pterm_count_literals(cube, g_kf_nwords) > 1) {
        factor_add_cube_candidate(terms, cube);
        g_kf_count++;
        if (g_kf_limit < g_kf_count)
            return;
    }
    for (l = start; l < g_kf_nvars * 2; l++) {
        if (factor_cube_of_literal_list(terms, (unsigned)l, g_kf_cube, &sub) &&
            factor_cube_extends(l, g_kf_cube, cube)) {
            FitPTerm *c = pterm_copy(g_kf_cube, g_kf_nwords);

            factor_cubes_rec(sub, (l - l % 2) + 2, c);
        }
    }
    if (pterm_count_literals(cube, g_kf_nwords) < 2) {
        pterm_free(&cube);
        list_destroy(&terms);
    }
}

/* the common cubes of the terms of `flat`, appended to out with
 * their gain */
static void factor_find_common_cubes(int all_cubes, FitList *flat, FitList *out, int limit,
                                     int nvars, int nwords, FactorGainFn gain, FitEntity *e,
                                     FitList *funcs)
{
    FitList *l;
    FitPTerm *c;
    int i;

    g_kf_out = list_create_sized(0);
    g_kf_count = 0;
    g_kf_limit = limit;
    g_kf_nwords = nwords;
    g_kf_nvars = nvars;
    g_kf_cube = pterm_new_raw(nwords);
    pterm_init_with_value(g_kf_cube, "?", g_kf_nwords);
    g_kf_tmp = list_create_sized(nvars);
    for (i = 0; i < LCOUNT(flat); i++)
        factor_pterm_common(g_kf_cube, flat->items[i], g_kf_cube, g_kf_nwords);
    l = list_create();
    factor_terms_containing(flat, g_kf_cube, l);
    c = pterm_copy(g_kf_cube, g_kf_nwords);
    if (all_cubes == 0)
        factor_cubes_rec_maximal(l, 0, c);
    else
        factor_cubes_rec(l, 0, c);
    pterm_free(&g_kf_cube);
    for (i = 0; i < LCOUNT(g_kf_out); i++) {
        FactorCube *cand = g_kf_out->items[i];

        cand->gain = gain(e, funcs, cand->terms, cand->cube, g_kf_nwords);
    }
    list_append_destroy(out, &g_kf_out);
    list_free_all(&g_kf_tmp, fac_destroy_list);
}

static void factor_find_common_cubes_all(FitList *flat, int nvars, int all_cubes, int limit,
                                         FitList **cubes, FactorGainFn gain, FitEntity *e,
                                         FitList *funcs)
{
    factor_find_common_cubes(all_cubes, flat, *cubes, limit, nvars, fac_nwords_for(nvars), gain,
                             e, funcs);
}

/* ------------------------------------------------------------------ */
/* gain and selection callbacks                                        */
/* ------------------------------------------------------------------ */

/* 1 when the cube has a literal on one of the variables created
 * by the factorisation (those after the inputs of e) */
static int factor_cube_uses_new_var(FitPTerm *cube, FitEntity *e, FitList *funcs)
{
    int v = LCOUNT(e->inputs);
    int end = (LCOUNT(funcs) - LCOUNT(e->outputs)) + v;

    while (v < end) {
        v++;
        if (bitvec_test(cube->bits, v) == bitvec_test(cube->care, v))
            return 1;
    }
    return 0;
}

static int factor_terms_use_new_var(FitList *terms, FitEntity *e, FitList *funcs)
{
    int i;

    for (i = 0; i < LCOUNT(terms); i++)
        if (factor_cube_uses_new_var(terms->items[i], e, funcs) != 0)
            return 1;
    return 0;
}

/* remove the literals on those variables from the cube */
static void factor_clear_new_vars(FitPTerm *cube, FitEntity *e, FitList *funcs)
{
    int v = LCOUNT(e->inputs);
    int end = (LCOUNT(funcs) - LCOUNT(e->outputs)) + v;

    while (v < end) {
        v++;
        if (bitvec_test(cube->bits, v) == bitvec_test(cube->care, v)) {
            bitvec_clear(cube->bits, v);
            bitvec_set(cube->care, v);
        }
    }
}

/* literals saved by extracting a kernel of n terms with these co-kernels */
static int factor_kernel_saving(FitList *terms, FitList *cokernels, int nwords)
{
    int nterms = LCOUNT(terms);
    int ncok = LCOUNT(cokernels);
    int sum = 0;
    int i;

    for (i = 0; i < ncok; i++)
        sum += pterm_count_literals(((FactorCokernel *)cokernels->items[i])->cube, nwords) *
               (nterms - 1);
    return (ncok - 1) * nterms + sum;
}

/* also used for the Xilinx target */
static int factor_kernel_gain_area(FitEntity *e, FitList *funcs, FitList *terms, void *cokernels,
                                   int nwords)
{
    (void)e;
    (void)funcs;
    return factor_kernel_saving(terms, cokernels, nwords);
}

/* no gain for a kernel that uses a new variable
 * (it would add a level) */
static int factor_kernel_gain_speed(FitEntity *e, FitList *funcs, FitList *terms, void *cokernels,
                                    int nwords)
{
    if (factor_terms_use_new_var(terms, e, funcs) != 0)
        return 0;
    return factor_kernel_saving(terms, cokernels, nwords);
}

/* the distinct function indices of a co-kernel list */
static FitList *factor_kernel_fct_indices(FitList *cokernels)
{
    FitList *res = list_create_sized(1);
    FitList *l = list_copy(cokernels);

    while (l != NULL && l->count != 0) {
        int fct = ((FactorCokernel *)l->items[0])->fct;
        int i = 0;
        int pos = 1;

        list_add(res, FIT_INT2PTR(fct));
        list_del_insert(l, 1);
        while (i < LCOUNT(l)) {
            if (((FactorCokernel *)l->items[i])->fct == fct) {
                list_del_insert(l, pos);
                i--;
                pos--;
            }
            i++;
            pos++;
        }
    }
    list_destroy(&l);
    return res;
}

/* among the best kernels, the last one that occurs in the
 * fewest functions */
static int factor_kernel_choose(FitEntity *e, FitList *funcs, FitList *cands, FitList *best,
                                int nwords)
{
    int fewest = 10000;
    int choice = 0;    /* always set below: best is never empty */
    int i;

    (void)e;
    (void)funcs;
    (void)nwords;
    if (best != NULL && best->count == 1)
        return FIT_PTR2INT(best->items[0]);
    for (i = 0; i < LCOUNT(best); i++) {
        int idx = FIT_PTR2INT(best->items[i]);
        FitList *fcts = factor_kernel_fct_indices(((FactorKernel *)cands->items[idx])->cokernels);

        if (LCOUNT(fcts) <= fewest) {
            choice = idx;
            fewest = LCOUNT(fcts);
        }
        list_destroy(&fcts);
    }
    return choice;
}

/* gain of a common cube, area criterion (factor_cube_gain_clb is the same
 * without the minimum size test) */
static int factor_cube_gain_area(FitEntity *e, FitList *funcs, FitList *terms, void *cube,
                                 int nwords)
{
    int nterms = LCOUNT(terms);
    int nlit;

    (void)e;
    (void)funcs;
    if (nterms == 1)
        return 0;
    nlit = pterm_count_literals(cube, nwords);
    if (nlit < g_fac_min_literals)
        return 0;
    return (nterms - 1) * nlit - nterms + 1;
}

static int factor_cube_gain_speed_big(FitEntity *e, FitList *funcs, FitList *terms, void *cube,
                                      int nwords)
{
    int nterms = LCOUNT(terms);
    int nlit;

    if (nterms == 1)
        return 0;
    nlit = pterm_count_literals(cube, nwords);
    if (nlit < g_fac_min_literals)
        return 0;
    if (factor_cube_uses_new_var(cube, e, funcs) != 0)
        return 0;
    return (nterms - 1) * nlit - nterms + 1;
}

static int factor_cube_gain_clb(FitEntity *e, FitList *funcs, FitList *terms, void *cube,
                                int nwords)
{
    int nterms = LCOUNT(terms);

    (void)e;
    (void)funcs;
    if (nterms == 1)
        return 0;
    return pterm_count_literals(cube, nwords) * (nterms - 1) - nterms + 1;
}

/* the literals on new variables are taken out of the cube first */
static int factor_cube_gain_speed(FitEntity *e, FitList *funcs, FitList *terms, void *cube,
                                  int nwords)
{
    int nterms = LCOUNT(terms);

    if (nterms == 1)
        return 0;
    factor_clear_new_vars(cube, e, funcs);
    return pterm_count_literals(cube, nwords) * (nterms - 1) - nterms + 1;
}

static int factor_count_ptr(void *item, FitList *l)
{
    int n = 0;
    int i;

    if (l == NULL)
        return 0;
    for (i = 0; i < l->count; i++)
        if (item == l->items[i])
            n++;
    return n;
}

/* among the best cubes, the first whose
 * terms are shared with the fewest other candidates */
static int factor_cube_choose(FitEntity *e, FitList *funcs, FitList *cands, FitList *best,
                              int nwords)
{
    FitList *all;
    int least = 1000000;
    int choice = 0;    /* stays 0 when nothing is below 1000000 */
    int i, j;

    (void)e;
    (void)funcs;
    (void)nwords;
    if (best != NULL && best->count == 1)
        return FIT_PTR2INT(best->items[0]);
    all = list_create();
    for (i = 0; i < LCOUNT(cands); i++) {
        FitList *terms = ((FactorCube *)cands->items[i])->terms;

        for (j = 0; j < LCOUNT(terms); j++)
            list_add(all, terms->items[j]);
    }
    for (i = 0; i < LCOUNT(best); i++) {
        int idx = FIT_PTR2INT(best->items[i]);
        FitList *terms = ((FactorCube *)cands->items[idx])->terms;
        int shared = 0;

        for (j = 0; j < LCOUNT(terms); j++)
            shared += factor_count_ptr(terms->items[j], all) - 1;
        if (shared < least) {
            choice = idx;
            least = shared;
        }
    }
    list_destroy(&all);
    return choice;
}

/* position in cands of the candidate to extract, -1 when none
 * has a gain above the threshold */
static int factor_select_best(FitEntity *e, FitList *funcs, FitList *cands, int is_cube,
                              int threshold, FactorGainFn gain, FactorChooseFn choose)
{
    FitList *best = list_create_sized(0);
    int highest = 0;
    int i, r;

    list_add(best, FIT_INT2PTR(-1));
    /* threshold -1 belongs to mode 2 (parity functions only), an
     * unsupported option that factor_entity does not let in */
    for (i = 0; i < LCOUNT(cands); i++) {
        int g;

        if (is_cube) {
            g = ((FactorCube *)cands->items[i])->gain;
        } else {
            FactorKernel *k = cands->items[i];

            g = gain(e, funcs, k->terms, k->cokernels, g_fac_nwords);
        }
        if (threshold < g && highest <= g) {
            if (highest < g)
                best->count = 0;
            list_add(best, FIT_INT2PTR(i));
            highest = g;
        }
    }
    r = choose(e, funcs, cands, best, g_fac_nwords);
    list_destroy(&best);
    return r;
}

/* ------------------------------------------------------------------ */
/* growing the bit vectors when a variable is added                    */
/* ------------------------------------------------------------------ */

static void fac_resize_pterm(FitPTerm *p, unsigned old_words, int new_words)
{
    FitPTerm *n = pterm_copy_words(old_words, new_words, p);

    pterm_release(p);
    p->bits = n->bits;
    p->care = n->care;
    free(n);
}

static void factor_resize_pterm_list(FitList *terms, unsigned old_words, int new_words)
{
    int i;

    for (i = 0; i < LCOUNT(terms); i++)
        fac_resize_pterm(terms->items[i], old_words, new_words);
}

/* after the variable count has become nvars */
static void factor_resize_functions(FitList *funcs, int nvars)
{
    unsigned old_words = nvars == 1 ? 1u : (((unsigned)nvars - 2u) >> 5) + 1u;
    unsigned new_words = (unsigned)fac_nwords_for(nvars);
    int i;

    if (old_words == new_words)
        return;
    for (i = 0; i < LCOUNT(funcs); i++)
        factor_resize_pterm_list(funcs->items[i], old_words, (int)new_words);
}

/* the same for the candidates: kernels (terms and co-kernel
 * cubes) or cubes (the cube only, its terms belong to the functions) */
static void factor_resize_candidates(FitList *cands, int nvars, int resize_terms, int is_cube)
{
    unsigned old_words = nvars == 1 ? 1u : (((unsigned)nvars - 2u) >> 5) + 1u;
    unsigned new_words = (unsigned)fac_nwords_for(nvars);
    int i, j;

    if (old_words == new_words)
        return;
    for (i = 0; i < LCOUNT(cands); i++) {
        if (is_cube) {
            FactorCube *c = cands->items[i];

            if (resize_terms)
                factor_resize_pterm_list(c->terms, old_words, (int)new_words);
            fac_resize_pterm(c->cube, old_words, (int)new_words);
        } else {
            FactorKernel *k = cands->items[i];

            if (resize_terms)
                factor_resize_pterm_list(k->terms, old_words, (int)new_words);
            for (j = 0; j < LCOUNT(k->cokernels); j++)
                fac_resize_pterm(((FactorCokernel *)k->cokernels->items[j])->cube, old_words,
                                 (int)new_words);
        }
    }
}

/* one more variable: used by the kernel, single-literal and cube steps */
static void fac_add_variable(void)
{
    int nvars = g_fac_nvars + 1;

    if (nvars == 0)
        g_fac_nwords = 1;
    else
        g_fac_nwords = (int)((unsigned)g_fac_nvars >> 5) + 1;
    g_fac_nvars = nvars;
}

/* ------------------------------------------------------------------ */
/* kernel extraction                                                   */
/* ------------------------------------------------------------------ */

/* drop the co-kernels that belong to the functions of
 * `changed`; a kernel left without co-kernel is taken out of cands and
 * appended to `removed` (the caller frees it after it has taken it out
 * of the hash table). */
static FitList *factor_drop_cokernels_of(FitList *cands, FitList *changed, FitList *removed)
{
    int i = 0;

    while (i < LCOUNT(cands)) {
        FactorKernel *k = cands->items[i];
        FitList *cl = k->cokernels;
        int j = 0;

        while (j < LCOUNT(cl)) {
            void *ck = cl->items[j];

            if (list_contains_ptr(changed, FIT_INT2PTR(((FactorCokernel *)ck)->fct)) != 0) {
                factor_free_cokernel(&ck);
                list_del_insert(cl, j + 1);
            } else {
                j++;
            }
        }
        if (cl == NULL || cl->count == 0) {
            list_add(removed, k);
            list_del_insert(cands, i + 1);
        } else {
            i++;
        }
    }
    return cands;
}

/* extract kernels while one has a gain above the threshold
 * `pass`.  Every function is divided by the kernel and, when the kernel
 * has fewer than four terms, what is left by the kernel's complement.
 * idx (owned) lists the functions whose kernels are to be searched. */
static void factor_kernel_loop(FitEntity *e, FitList *funcs, FitList *idx, FitList **kernels,
                               FitList **hash, int pass, int *changed)
{
    *changed = 0;
    for (;;) {
        FitList *kern, *compl = NULL, *removed;
        int have_compl;
        int sel, n, k, i, j;

        factor_find_kernels(funcs, g_fac_nvars, g_fac_all_levels, g_fac_kernel_limit, kernels,
                            hash, idx);
        sel = factor_select_best(e, funcs, *kernels, 0, pass, g_fac_kernel_gain,
                                 g_fac_kernel_choose);
        if (sel == -1) {
            list_destroy(&idx);
            return;
        }
        *changed = 1;
        kern = ((FactorKernel *)(*kernels)->items[sel])->terms;
        if (g_fac_kind == 3 && g_fac_criterion == 0)
            n = LCOUNT(e->outputs);
        else
            n = LCOUNT(funcs);

        fac_add_variable();
        list_add(funcs, list_map_copy_x(kern, fac_copy_pterm, FIT_INT2PTR(g_fac_nwords)));
        factor_resize_functions(funcs, g_fac_nvars);
        factor_resize_candidates(*kernels, g_fac_nvars, 1, 0);

        if (kern == NULL || kern->count < 4) {
            unsigned *support = pterm_list_support(kern, g_fac_nwords);
            FitList *cover = domlist_factor_cover(kern, g_fac_nwords, support);

            free(support);
            compl = domlist_minimise_pterms(cover, NULL, (unsigned)g_fac_nvars, 0);
            list_free_all(&cover, fac_free_pterm);
            have_compl = 1;
        } else {
            have_compl = 0;
        }

        idx->count = 0;
        for (k = 0; k < n; k++) {
            FitList *q = NULL, *rem = NULL, *cq = NULL, *crem = NULL;
            FitList *newf, *compl_q = NULL;

            /* constant functions are left alone */
            if (k < LCOUNT(e->fcts) &&
                ((FitFct *)e->fcts->items[k])->tree[0]->sel == FIT_SEL_CONST)
                continue;

            factor_divide(funcs->items[k], kern, g_fac_nwords, &q, &rem, 0);
            for (i = 0; i < LCOUNT(q); i++) {
                FitPTerm *t = q->items[i];

                bitvec_set(t->bits, g_fac_nvars);
                bitvec_set(t->care, g_fac_nvars);
            }
            newf = list_map_copy_x(q, fac_copy_pterm, FIT_INT2PTR(g_fac_nwords));
            list_destroy(&q);

            if (rem == NULL || rem->count == 0 || !have_compl) {
                crem = NULL;
            } else {
                factor_divide(rem, compl, g_fac_nwords, &cq, &crem, 0);
                if (cq != NULL) {
                    for (j = 0; j < LCOUNT(cq); j++) {
                        FitPTerm *t = cq->items[j];

                        bitvec_clear(t->bits, g_fac_nvars);
                        bitvec_clear(t->care, g_fac_nvars);
                    }
                    compl_q = list_map_copy_x(cq, fac_copy_pterm, FIT_INT2PTR(g_fac_nwords));
                    list_destroy(&cq);
                }
            }

            if (newf == NULL) {
                if (compl_q == NULL)
                    continue;
                list_append_destroy(compl_q, &crem);
                funcs->items[k] = compl_q;
            } else if (compl_q == NULL) {
                list_append_destroy(newf, &rem);
                funcs->items[k] = newf;
            } else {
                list_append_destroy(newf, &compl_q);
                list_append_destroy(newf, &crem);
                funcs->items[k] = newf;
            }
            list_add(idx, FIT_INT2PTR(k));
        }

        if (have_compl)
            list_free_all(&compl, fac_free_pterm);

        removed = list_create_sized(1);
        *kernels = factor_drop_cokernels_of(*kernels, idx, removed);
        for (i = 0; i < LCOUNT(removed); i++) {
            for (j = 0; j < LCOUNT(*hash); j++) {
                FitList *bucket = (*hash)->items[j];
                int p;

                if (bucket == NULL)
                    continue;
                for (p = 0; p < bucket->count; p++) {
                    if (bucket->items[p] == removed->items[i]) {
                        list_del_insert(bucket, p + 1);
                        break;
                    }
                }
            }
        }
        for (i = 0; i < LCOUNT(removed); i++) {
            FactorKernel *dead = removed->items[i];

            factor_free_kernel(&dead);
        }
        list_destroy(&removed);

        if (g_fac_kind != 3 || g_fac_criterion != 0)
            list_add(idx, FIT_INT2PTR(funcs != NULL ? funcs->count - 1 : -1));
    }
}

static void factor_extract_kernels(FitEntity *e, FitList **funcs, int pass, int *changed,
                                   FitList **kernels, FitList **hash)
{
    FitList *idx;
    int n, i;

    if (g_fac_kind == 3 && g_fac_criterion == 0)
        n = LCOUNT(e->outputs);
    else if (*funcs != NULL && (*funcs)->count != 0)
        n = (*funcs)->count;
    else
        n = LCOUNT(e->fcts);

    idx = list_create();
    if (*kernels == NULL || (*kernels)->count == 0) {
        for (i = 0; i < n; i++)
            list_add(idx, FIT_INT2PTR(i));
    }
    if (*funcs == NULL || (*funcs)->count == 0) {
        for (i = 0; i < LCOUNT(e->fcts); i++)
            list_add(*funcs, fcttree_to_pterms3(((FitFct *)e->fcts->items[i])->tree[0],
                                                e->inputs, NULL, e->locals));
    }
    factor_kernel_loop(e, *funcs, idx, kernels, hash, pass, changed);
}

/* ------------------------------------------------------------------ */
/* single literals                                                     */
/* ------------------------------------------------------------------ */

/* the product of the complements of the single-literal terms */
static FitPTerm *factor_common_literals(FitList *terms)
{
    FitPTerm *p = util_calloc(1, sizeof *p);
    int i, v;

    pterm_init_dc(p, g_fac_nwords);
    for (i = 0; i < LCOUNT(terms); i++) {
        FitPTerm *t = terms->items[i];

        for (v = 1; v <= g_fac_nvars; v++) {
            int a = bitvec_test(t->bits, v);
            int b = bitvec_test(t->care, v);

            if (a == b) {
                if (a == 0) {
                    bitvec_set(p->bits, v);
                    bitvec_set(p->care, v);
                } else {
                    bitvec_clear(p->bits, v);
                    bitvec_clear(p->care, v);
                }
            }
        }
    }
    return p;
}

/* a function with two or more single-literal terms
 *   f = a + b + rest   becomes   f = !n + rest,  n = !a * !b  */
static void factor_merge_single_literals(FitList *funcs, int *changed)
{
    int i = 0;

    *changed = 0;
    while (i < LCOUNT(funcs)) {
        FitList *f = funcs->items[i];
        FitList *singles = list_create_sized(0);
        int found = 0;
        int j = 0;

        while (j < LCOUNT(f)) {
            FitPTerm *t = f->items[j];

            if (pterm_count_literals(t, g_fac_nwords) == 1) {
                found++;
                list_add(singles, t);
                list_del_insert(f, j + 1);
            } else {
                j++;
            }
        }
        if (found < 2) {
            if (found == 1)
                list_add(f, singles->items[0]);
            list_destroy(&singles);
        } else {
            FitPTerm *node = factor_common_literals(singles);
            FitList *nf = list_create_sized(1);
            FitPTerm *t;

            *changed = 1;
            list_add(nf, node);
            list_add(funcs, nf);
            fac_add_variable();
            factor_resize_functions(funcs, g_fac_nvars);
            t = util_calloc(1, sizeof *t);
            pterm_init_dc(t, g_fac_nwords);
            bitvec_clear(t->bits, g_fac_nvars);
            bitvec_clear(t->care, g_fac_nvars);
            list_add(f, t);
            list_free_all(&singles, fac_free_pterm);
        }
        i++;
    }
}

/* ------------------------------------------------------------------ */
/* common cube extraction                                              */
/* ------------------------------------------------------------------ */

/* replace the literals of `cube` in term t by the new variable
 * (the last one) when t holds the cube */
static void factor_substitute_cube(FitPTerm *t, FitPTerm *cube, int *done)
{
    int v;

    *done = 0;
    if (pterm_covers(t, cube, g_fac_nwords) == 0)
        return;
    for (v = 1; v <= g_fac_nvars - 1; v++) {
        if (bitvec_test(cube->bits, v) == bitvec_test(cube->care, v)) {
            bitvec_clear(t->bits, v);
            bitvec_set(t->care, v);
        }
    }
    bitvec_set(t->bits, g_fac_nvars);
    bitvec_set(t->care, g_fac_nvars);
    *done = 1;
}

/* after candidate `sel` has been chosen: take it out of the
 * list, and bring the others up to date: a candidate whose cube holds the
 * chosen cube gets the new variable instead; from one that shares a
 * variable (same sign) with it the terms holding the chosen cube are
 * removed; candidates left with fewer than two terms are dropped; the
 * gain of the changed ones is computed again.  `changed` stays set from
 * one candidate to the next until a candidate is dropped (deliberate:
 * which gains are computed again affects the choices). */
static void factor_update_cube_candidates(FitList *cands, int sel, FactorGainFn gain,
                                          FitEntity *e, FitList *funcs)
{
    FitPTerm *chosen = ((FactorCube *)cands->items[sel])->cube;
    int changed = 0;
    int i = 0;
    int pos = 1;

    list_del_insert(cands, sel + 1);
    while (i < LCOUNT(cands)) {
        FactorCube *c = cands->items[i];
        FitList *terms = c->terms;
        int done, v, j;

        factor_substitute_cube(c->cube, chosen, &done);
        if (done) {
            i++;
            pos++;
            changed = 1;
            c->gain = gain(e, funcs, c->terms, c->cube, g_fac_nwords);
            continue;
        }
        for (v = 1; v <= g_fac_nvars; v++) {
            if (bitvec_test(c->cube->bits, v) == bitvec_test(chosen->bits, v) &&
                bitvec_test(c->cube->care, v) == bitvec_test(chosen->care, v)) {
                j = 0;
                while (j < LCOUNT(terms)) {
                    if (pterm_covers(terms->items[j], chosen, g_fac_nwords) != 0) {
                        list_del_insert(terms, j + 1);
                        changed = 1;
                    } else {
                        j++;
                    }
                }
                break;
            }
        }
        if (terms == NULL || terms->count <= 1) {
            changed = 0;
            list_del_insert(cands, pos);
            list_destroy(&c->terms);
            pterm_free(&c->cube);
            free(c);
            continue;
        }
        i++;
        pos++;
        if (changed)
            c->gain = gain(e, funcs, c->terms, c->cube, g_fac_nwords);
    }
}

static void factor_cube_loop(FitEntity *e, FitList **flat, FitList **funcs, FitList **cubes,
                             int pass, int *changed)
{
    *changed = 0;
    for (;;) {
        FactorCube *c;
        FitList *terms, *nf;
        FitPTerm *cube;
        int sel, i, done;

        if (*cubes == NULL || (*cubes)->count == 0)
            factor_find_common_cubes_all(*flat, g_fac_nvars, g_fac_all_cubes, g_fac_cube_limit,
                                         cubes, g_fac_cube_gain, e, *funcs);
        sel = factor_select_best(e, *funcs, *cubes, 1, pass, g_fac_cube_gain, g_fac_cube_choose);
        if (sel == -1)
            break;
        *changed = 1;
        c = (*cubes)->items[sel];
        terms = c->terms;
        cube = c->cube;
        nf = list_create_sized(1);
        list_add(nf, pterm_copy(cube, g_fac_nwords));
        list_add(*funcs, nf);
        fac_add_variable();
        factor_resize_functions(*funcs, g_fac_nvars);
        factor_resize_candidates(*cubes, g_fac_nvars, 0, 1);
        factor_update_cube_candidates(*cubes, sel, g_fac_cube_gain, e, *funcs);
        for (i = 0; i < LCOUNT(terms); i++)
            factor_substitute_cube(terms->items[i], cube, &done);
        list_destroy(&c->terms);
        pterm_free(&cube);
        free(c);
    }
}

static void factor_extract_cubes(FitEntity *e, int pass, FitList **flat, FitList **funcs,
                                 FitList **cubes, int *changed)
{
    int n, i;

    if (g_fac_kind == 3 && g_fac_criterion == 0)
        n = LCOUNT(e->outputs);
    else
        n = LCOUNT(*funcs);

    if (*flat == NULL || (*flat)->count == 0) {
        if (*funcs == NULL || (*funcs)->count == 0) {
            for (i = 0; i < LCOUNT(e->fcts); i++)
                list_add(*funcs, fcttree_to_pterms3(((FitFct *)e->fcts->items[i])->tree[0],
                                                    e->inputs, NULL, e->locals));
            if (g_fac_kind == 3 && g_fac_criterion == 0)
                n = LCOUNT(e->outputs);
            else
                n = LCOUNT(e->fcts);
        }
        for (i = 0; i < n; i++)
            list_append(*flat, (*funcs)->items[i]);
    }
    if (*flat != NULL && (*flat)->count != 0)
        factor_cube_loop(e, flat, funcs, cubes, pass, changed);
}

/* ------------------------------------------------------------------ */
/* passes                                                              */
/* ------------------------------------------------------------------ */

/* one step of a pass at gain threshold `pass` */
static void factor_step(FitEntity *e, FitList **funcs, FitList **flat, FitList **kernels,
                        FitList **hash, FitList **cubes, int *kernel_changed,
                        int *merge_changed, int *cube_changed, int pass)
{
    int merge = 0;
    int i;

    if (*merge_changed != 0 || *cube_changed != 0) {
        if (*kernels != NULL) {
            for (i = 0; i < LCOUNT(*kernels); i++) {
                FactorKernel *k = (*kernels)->items[i];

                factor_free_kernel(&k);
            }
            list_destroy(kernels);
        }
        if (*hash != NULL)
            list_free_all(hash, fac_destroy_list);
        *kernels = list_create();
    }
    factor_extract_kernels(e, funcs, pass, kernel_changed, kernels, hash);

    switch (g_fac_kind) {
    case 3:
        merge = g_fac_criterion != 0;
        break;
    case 1:
    case 4:
    case 7:
    case 10:
    case 11:
        merge = 1;
        break;
    default:
        break;
    }
    if (merge && (*kernel_changed != 0 || *cube_changed != 0))
        factor_merge_single_literals(*funcs, merge_changed);

    if (*merge_changed != 0 || *kernel_changed != 0) {
        for (i = 0; i < LCOUNT(*cubes); i++)
            factor_free_cube((*cubes)->items[i]);
        (*cubes)->count = 0;
        (*flat)->count = 0;
    }
    factor_extract_cubes(e, pass, flat, funcs, cubes, cube_changed);
}

/* entity like e whose functions are the product-term lists of
 * funcs; the lists beyond the functions of e become new locals */
static FitEntity *factor_build_entity(FitEntity *e, FitList *funcs)
{
    FitEntity *n = entity_new(e->name);
    FitList *vars, *lv;
    int extra, i;

    n->n_in = e->n_in;
    n->n_out = e->n_out;
    n->n_local = e->n_local;
    n->inputs = list_map_copy(e->inputs, fac_copy_var);
    n->outputs = list_map_copy(e->outputs, fac_copy_var);
    n->locals = list_map_copy(e->locals, fac_copy_var);
    n->fcts = list_create();
    n->net = NULL;
    n->i30 = 0;
    for (i = 0; i < LCOUNT(e->fcts); i++) {
        FitFct *f = fct_new();

        f->var_id = ((FitFct *)e->fcts->items[i])->var_id;
        list_add(n->fcts, f);
    }
    extra = LCOUNT(funcs) - LCOUNT(e->fcts);
    for (; extra > 0; extra--) {
        FitVariable *v = var_new_unique(n);
        FitFct *f;

        list_add(n->locals, v);
        f = fct_new();
        f->var_id = v->id;
        list_add(n->fcts, f);
    }
    n->n_local = LCOUNT(n->locals);
    vars = list_map_copy(n->inputs, fac_copy_var);
    lv = list_map_copy(n->locals, fac_copy_var);
    list_append_destroy(vars, &lv);
    for (i = 0; i < LCOUNT(n->fcts); i++)
        ((FitFct *)n->fcts->items[i])->tree[0] = fcttree_from_pterms(funcs->items[i], vars);
    list_free_all(&vars, fac_free_var);
    return n;
}

/* one pass over the entity; returns the factored entity */
static FitEntity *factor_one_pass(FitEntity *e)
{
    FitList *kernels = list_create();
    FitList *cubes = list_create();
    FitList *flat = list_create();
    FitList *funcs = list_create();
    FitList *hash = NULL;       /* set by the first kernel search */
    FitEntity *r;
    int kernel_changed = 0, merge_changed = 0, cube_changed = 0;
    int i;

    g_fac_nvars = LCOUNT(e->inputs) + LCOUNT(e->locals);
    g_fac_nwords = fac_nwords_for(g_fac_nvars);

    for (i = 0; i < 13; i++)
        factor_step(e, &funcs, &flat, &kernels, &hash, &cubes, &kernel_changed, &merge_changed,
                    &cube_changed, g_fac_thresholds[i]);

    for (i = 0; i < LCOUNT(kernels); i++) {
        FactorKernel *k = kernels->items[i];

        factor_free_kernel(&k);
    }
    list_destroy(&kernels);
    if (hash != NULL)
        list_free_all(&hash, fac_destroy_list);
    for (i = 0; i < LCOUNT(cubes); i++)
        factor_free_cube(cubes->items[i]);
    list_destroy(&cubes);
    list_destroy(&flat);

    r = factor_build_entity(e, funcs);
    for (i = 0; i < LCOUNT(funcs); i++) {
        FitList *f = funcs->items[i];

        list_free_all(&f, fac_free_pterm);
        funcs->items[i] = f;
    }
    list_free_all(&funcs, fac_destroy_list);

    tt2_collapse_locals(r, g_fac_mode != 2);
    g_fac_nvars = LCOUNT(r->inputs) + LCOUNT(r->locals);
    g_fac_nwords = fac_nwords_for(g_fac_nvars);
    r->n_local = LCOUNT(r->locals);
    return r;
}

/* the entity rebuilt from its BDDs (multiplexer trees), or a
 * copy of it when the BDDs get too large */
static FitEntity *factor_bdd_resynthesis(FitEntity *e)
{
    FitList *roots = NULL, *blocks = NULL, *unique = NULL, *infos = NULL;
    FitBddNode *terminal = NULL;
    FitEntity *r;
    int n_nodes = 0;

    bdd_build_entity_ex(e, &roots, &blocks, &unique, &terminal, &n_nodes, 0x32, 10000, 500);
    if (n_nodes < 0) {
        bdd_free_blocks(blocks);
        list_free_all(&roots, util_free_ptr);
        bdd_free_unique_table(unique);
        return entity_copy(e);
    }
    bdd_free_unique_table(unique);
    bdd_annotate(roots, &infos, terminal);
    r = bdd_to_entity(roots, e);
    bdd_free_info_blocks(infos);
    bdd_free_blocks(blocks);
    list_free_all(&roots, util_free_ptr);
    return r;
}

/* prev < cx * 1.01 with the product of the integer and the double
 * constant 1.01 (0x3FF028F5C28F5C29) not rounded to double before the
 * comparison, as with extended precision arithmetic.  Deliberate: the
 * decision to run another pass depends on it at the boundary.  Done in
 * integers: 1.01 = (2^52 + 0x28F5C28F5C29) / 2^52. */
static int fac_less_than_101_percent(int prev, int cx)
{
    unsigned long long m_hi = 0x28F5C28F5C29ull >> 20;
    unsigned long long m_lo = 0x28F5C28F5C29ull & 0xFFFFFull;
    unsigned long long a, b, s, q;
    int frac;

    if (cx < 0 || prev < 0)
        return (double)prev < (double)cx * 1.01;
    a = (unsigned long long)cx * m_hi;
    b = (unsigned long long)cx * m_lo;
    s = a + (b >> 20);
    q = (unsigned long long)cx + (s >> 32);
    frac = (s & 0xFFFFFFFFull) != 0 || (b & 0xFFFFFull) != 0;
    if ((unsigned long long)prev < q)
        return 1;
    return (unsigned long long)prev == q && frac;
}

/* repeat passes while they reduce the literal count */
static FitEntity *factor_passes(FitEntity *e, int print_result, int first_pass, int verbose)
{
    FitEntity *cur, *saved, *r, *res;
    int go_on = 1;
    int orig_cx, best, prev, bdd_cx, cx;
    int pass;

    orig_cx = tt2_entity_complexity(e);
    cur = entity_copy_inv_propagated(e);
    if (cur->locals == NULL)
        cur->locals = list_create();
    best = tt2_entity_complexity(cur);
    prev = best;
    if (g_fac_criterion == 1) {
        FitEntity *b = factor_bdd_resynthesis(e);

        bdd_cx = tt2_entity_complexity(b);
        entity_free(&b);
    } else {
        bdd_cx = 1000000;
    }

    pass = first_pass;
    while (pass <= g_fac_max_pass) {
        saved = entity_copy(cur);
        if (pass != first_pass) {
            FitEntity *m = domlist_minimise_entity(cur, 1, 1, 0x32);

            entity_free(&cur);
            cur = m;
        }
        g_fac_nvars = LCOUNT(cur->inputs) + LCOUNT(cur->locals);
        g_fac_nwords = fac_nwords_for(g_fac_nvars);
        if (verbose != 0 && g_verbose != 0)
            fprintf(g_msg, "PASS %d ...\n", pass);
        flatten_add_fed_back_outputs(cur);
        r = factor_one_pass(cur);
        flatten_drop_fed_back_outputs(r);
        flatten_drop_fed_back_outputs(cur);
        res = entity_copy_inv_propagated(r);
        entity_free(&r);
        cx = tt2_entity_complexity(res);
        if (cx < best) {
            if (verbose != 0 && print_result != 0 && g_verbose != 0)
                fprintf(g_msg, "Result of PASS %d : %d LITERALS.\n", pass, cx);
            if (fac_less_than_101_percent(prev, cx))
                go_on = 0;
            best = cx;
            prev = cx;
            entity_free(&cur);
            entity_free(&saved);
            cur = res;
            pass++;
        } else {
            entity_free(&cur);
            entity_free(&res);
            cur = saved;
            go_on = 0;
            if (verbose != 0 && print_result != 0 && g_verbose != 0) {
                fprintf(g_msg, "Result of PASS %d : %d LITERALS.\n", pass, best);
                pass++;
            }
        }
        if (cx > 3000 || go_on == 0)
            break;
    }

    if (bdd_cx < best) {
        if (verbose != 0 && g_verbose != 0)
            fprintf(g_msg, "PASS %d ...\n", pass);
        r = factor_bdd_resynthesis(e);
        entity_free(&cur);
        best = bdd_cx;
        cur = r;
        if (verbose != 0 && print_result != 0 && g_verbose != 0)
            fprintf(g_msg, "Result of PASS %d : %d LITERALS.\n", pass, best);
    }
    if (orig_cx < best) {
        entity_free(&cur);
        return entity_copy(e);
    }
    return cur;
}

static FitEntity *factor_run_with(FitEntity *e, int print_result, int first_pass, int verbose,
                                  int kind, int criterion, int mode, int kernel_limit,
                                  int cube_limit, int all_levels, int all_cubes, int min_literals,
                                  int max_pass, FactorGainFn kernel_gain,
                                  FactorChooseFn kernel_choose, FactorGainFn cube_gain,
                                  FactorChooseFn cube_choose)
{
    g_fac_kind = kind;
    g_fac_criterion = criterion;
    g_fac_mode = mode;
    g_fac_kernel_limit = kernel_limit;
    g_fac_cube_limit = cube_limit;
    g_fac_all_levels = all_levels;
    g_fac_all_cubes = all_cubes;
    g_fac_min_literals = min_literals;
    g_fac_max_pass = max_pass;
    g_fac_kernel_gain = kernel_gain;
    g_fac_kernel_choose = kernel_choose;
    g_fac_cube_gain = cube_gain;
    g_fac_cube_choose = cube_choose;
    return factor_passes(e, print_result, first_pass, verbose);
}

FitEntity *factor_entity(FitEntity *entity, int target, int criterion, int mode, int limit,
                         int verbose)
{
    FitEntity *r, *a, *b;
    int all_levels;
    int say = verbose != 0 && g_verbose != 0;
    int ca, cb;

    if (limit == -1)
        limit = 6000;
    if (mode != 1) {
        fprintf(stderr, "fit150x: factorisation mode %d is not supported\n", mode);
        exit(3);
    }
    all_levels = 0;     /* 1 for the modes other than 1 */

    if (target == FACTOR_TARGET_STANDARD) {
        if (say)
            fprintf(g_msg, "Factorization for : STANDARD CELLS.\n");
        if (criterion == 0) {
            if (say)
                fprintf(g_msg, "                   : Criterion = SPEED.\n");
            if (tt2_entity_complexity(entity) < 2000) {
                r = factor_run_with(entity, 1, 1, verbose, 3, 0, mode, limit, limit, all_levels,
                                    1, 0, 3, factor_kernel_gain_speed, factor_kernel_choose,
                                    factor_cube_gain_speed, factor_cube_choose);
                g_fac_kind = 1;
            } else {
                r = factor_run_with(entity, 1, 1, verbose, 1, 0, mode, limit, limit, all_levels,
                                    0, 3, 3, factor_kernel_gain_speed, factor_kernel_choose,
                                    factor_cube_gain_speed_big, factor_cube_choose);
            }
        } else {
            if (say)
                fprintf(g_msg,
                        "                   : Criterion = AREA (after place and route)\n");
            a = factor_run_with(entity, 0, 0, verbose, 1, criterion, mode, limit, limit,
                                all_levels, 0, 3, 0, factor_kernel_gain_area,
                                factor_kernel_choose, factor_cube_gain_area, factor_cube_choose);
            r = a;
            ca = tt2_entity_complexity(a);
            if (ca < 500) {
                b = factor_run_with(entity, 0, 1, verbose, 1, criterion, mode, limit, limit,
                                    all_levels, 1, 1, 10, factor_kernel_gain_area,
                                    factor_kernel_choose, factor_cube_gain_area,
                                    factor_cube_choose);
                cb = tt2_entity_complexity(b);
                if (ca - cb > 0x5a || cb < 300) {
                    entity_free(&a);
                    r = b;
                } else {
                    entity_free(&b);
                }
            }
        }
    } else if (target == FACTOR_TARGET_XILINX) {
        if (say)
            fprintf(g_msg, "Factorization for : XILINX.\n");
        if (criterion == 0) {
            if (say)
                fprintf(g_msg, "                   : Criterion = SPEED.\n");
            g_fac_speed_mode = 1;
            r = factor_run_with(entity, 1, 1, verbose, 3, 0, mode, limit, limit, all_levels, 1, 1,
                                3, factor_kernel_gain_speed, factor_kernel_choose,
                                factor_cube_gain_speed, factor_cube_choose);
        } else {
            if (say)
                fprintf(g_msg, "                   : Criterion = #CLBs.\n");
            r = factor_run_with(entity, 1, 1, verbose, 3, criterion, mode, limit, limit,
                                all_levels, 1, 1, 10, factor_kernel_gain_area,
                                factor_kernel_choose, factor_cube_gain_clb, factor_cube_choose);
        }
    } else if (target == FACTOR_TARGET_LITERALS) {
        if (say)
            fprintf(g_msg, "Factorization for : LITERALS.\n");
        r = factor_run_with(entity, 1, 1, verbose, 10, criterion, mode, limit, limit, all_levels,
                            1, 1, 10, factor_kernel_gain_area, factor_kernel_choose,
                            factor_cube_gain_area, factor_cube_choose);
    } else {
        if (say) {
            if (target == FACTOR_TARGET_ACTEL)
                fprintf(g_msg, "Factorization for : ACTEL.\n");
            else if (target == FACTOR_TARGET_QUICKLOGIC)
                fprintf(g_msg, "Factorization for : QUICKLOGIC.\n");
            else
                fprintf(g_msg, "Factorization for : %s.\n",
                        target == FACTOR_TARGET_ALTERA ? "ALTERA" : "GENERIC");
        }
        if (criterion == 0) {
            if (say)
                fprintf(g_msg, "                   : Criterion = SPEED.\n");
            g_fac_speed_mode = 1;
            r = factor_run_with(entity, 1, 1, verbose, 3, 0, mode, limit, limit, all_levels, 1, 1,
                                3, factor_kernel_gain_speed, factor_kernel_choose,
                                factor_cube_gain_speed, factor_cube_choose);
            g_fac_kind = 4;
        } else {
            if (say)
                fprintf(g_msg, "                   : Criterion = AREA.\n");
            r = factor_run_with(entity, 1, 1, verbose, 4, criterion, mode, limit, limit,
                                all_levels, 1, 1, 10, factor_kernel_gain_area,
                                factor_kernel_choose, factor_cube_gain_area, factor_cube_choose);
        }
    }
    r->net = netlist_copy(entity->net);
    return r;
}
