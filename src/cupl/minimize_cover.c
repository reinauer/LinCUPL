/*
 * minimize_cover.c - covers of the minimize phase (levels 2 and 3).
 *
 * Cube lists and cofactors, the complement of a cover, the sharp product
 * and intersection of covers, and unravelling cubes into their minterms of
 * the multiple-valued variables.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "minimize_int.h"

/* =========================================================================
 * Cube lists and cofactors (Espresso cofactor.c)
 * ========================================================================= */

/* T[1] points one past the terminator, so T[1] - T is the number of slots
 * in use (cubes + 3). */
void minimize_cubelist_set_end(cubelist_t T, pset *terminator)
{
    T[1] = (pset)(void *)(terminator + 1);
}

int minimize_cubelist_slots(cubelist_t T)
{
    return (int)((pset *)(void *)T[1] - T);
}

void minimize_cubelist_free(cubelist_t T)
{
    minimize_work_free(T[0]);
    minimize_work_free(T);
}

cubelist_t minimize_cubelist_alloc(int slots)
{
    return minimize_work_alloc((size_t)slots * sizeof(pset));
}

static pset *cubelist_add_cover(pset *pdest, pcover A)
{
    pset p, last;
    foreach_set(A, last, p) {
        *pdest++ = p;
    }
    return pdest;
}

cubelist_t minimize_cube1list(pcover A)
{
    cubelist_t T = minimize_cubelist_alloc(A->count + 3);
    T[0] = new_cube();
    pset *pdest = cubelist_add_cover(T + 2, A);
    *pdest = NULL;
    minimize_cubelist_set_end(T, pdest);
    return T;
}

cubelist_t minimize_cube2list(pcover A, pcover B)
{
    cubelist_t T = minimize_cubelist_alloc(A->count + B->count + 3);
    T[0] = new_cube();
    pset *pdest = cubelist_add_cover(T + 2, A);
    pdest = cubelist_add_cover(pdest, B);
    *pdest = NULL;
    minimize_cubelist_set_end(T, pdest);
    return T;
}

cubelist_t minimize_cube3list(pcover A, pcover B, pcover C)
{
    cubelist_t T = minimize_cubelist_alloc(A->count + B->count + C->count + 3);
    T[0] = new_cube();
    pset *pdest = cubelist_add_cover(T + 2, A);
    pdest = cubelist_add_cover(pdest, B);
    pdest = cubelist_add_cover(pdest, C);
    *pdest = NULL;
    minimize_cubelist_set_end(T, pdest);
    return T;
}

/* cofactor of T against the cube c */
cubelist_t minimize_cofactor(cubelist_t T, pset c)
{
    pset temp = minimize_cube.temp[0];
    cubelist_t Tc = minimize_cubelist_alloc(minimize_cubelist_slots(T));
    pset *pdest = Tc + 2;
    pset p;

    set_diff(temp, minimize_cube.fullset, c);
    Tc[0] = set_or(new_cube(), T[0], temp);
    for (pset *T1 = T + 2; (p = *T1) != NULL; T1++) {
        if (p != c && minimize_cdist0(p, c)) {
            *pdest++ = p;
        }
    }
    *pdest = NULL;
    minimize_cubelist_set_end(Tc, pdest);
    return Tc;
}

/* cofactor against a cube that is full except in var */
cubelist_t minimize_scofactor(cubelist_t T, pset c, int var)
{
    pset temp = minimize_cube.temp[1];
    const uint32_t *mask = minimize_cube.var_mask[var];
    int first = minimize_cube.first_word[var];
    int last = minimize_cube.last_word[var];
    cubelist_t Tc = minimize_cubelist_alloc(minimize_cubelist_slots(T));
    pset *pdest = Tc + 2;
    pset p;

    pset cof = new_cube();
    int i = LOOP(c);
    cof[0] = (uint32_t)i;
    Tc[0] = cof;
    do {
        cof[i] = (minimize_cube.fullset[i] & ~c[i]) | T[0][i];
    } while (--i > 0);

    for (i = first; i <= last; i++) {
        temp[i] = c[i] & mask[i];
    }
    for (pset *T1 = T + 2; (p = *T1) != NULL; T1++) {
        if (p != c) {
            for (i = first; i <= last; i++) {
                if ((temp[i] & p[i]) != 0) {
                    *pdest++ = p;
                    break;
                }
            }
        }
    }
    *pdest = NULL;
    minimize_cubelist_set_end(Tc, pdest);
    return Tc;
}

/* count the zeros of every part and classify the variables */
void minimize_massive_count(cubelist_t T)
{
    int *count = minimize_cdata.part_zeros;
    const uint32_t *cof = T[0];
    pset p;

    for (int i = 0; i < minimize_cube.size; i++) {
        count[i] = 0;
    }
    for (pset *T1 = T + 2; (p = *T1) != NULL; T1++) {
        for (int i = LOOP(p); i > 0; i--) {
            int *cnt = count + (i - 1) * 32;
            for (uint32_t val = minimize_cube.fullset[i] & ~(p[i] | cof[i]); val != 0; val >>= 1, cnt++) {
                if (val & 1u) {
                    (*cnt)++;
                }
            }
        }
    }

    int best = -1, mostactive = 0, mostzero = 0, mostbalanced = 32000;
    minimize_cdata.vars_unate = minimize_cdata.vars_active = 0;
    for (int var = 0; var < minimize_cube.num_vars; var++) {
        int active, maxactive;
        if (var < minimize_cube.num_binary_vars) {
            int zeros0 = count[var * 2];
            int zeros1 = count[var * 2 + 1];
            active = (zeros0 > 0) + (zeros1 > 0);
            minimize_cdata.var_zeros[var] = zeros0 + zeros1;
            maxactive = zeros0 < zeros1 ? zeros1 : zeros0;
        } else {
            minimize_cdata.var_zeros[var] = 0;
            active = 0;
            maxactive = 0;
            for (int i = minimize_cube.first_part[var]; i <= minimize_cube.last_part[var]; i++) {
                minimize_cdata.var_zeros[var] += count[i];
                active += count[i] > 0;
                if (active > maxactive) {
                    maxactive = active;
                }
            }
        }
        if (active > mostactive) {
            best = var;
            mostactive = active;
            mostzero = minimize_cdata.var_zeros[var];
            mostbalanced = maxactive;
        } else if (active == mostactive) {
            if (minimize_cdata.var_zeros[var] > mostzero) {
                best = var;
                mostzero = minimize_cdata.var_zeros[var];
                mostbalanced = maxactive;
            } else if (minimize_cdata.var_zeros[var] == mostzero && maxactive < mostbalanced) {
                best = var;
                mostbalanced = maxactive;
            }
        }
        minimize_cdata.parts_active[var] = active;
        minimize_cdata.is_unate[var] = active == 1;
        minimize_cdata.vars_active += active > 0;
        minimize_cdata.vars_unate += active == 1;
    }
    minimize_cdata.best = best;
}

/* split the parts of the most binate variable into two halves */
int minimize_binate_split_select(cubelist_t T, pset cleft, pset cright)
{
    int best = minimize_cdata.best;
    int lastbit = minimize_cube.last_part[best];
    const uint32_t *cof = T[0];
    int halfbit = 0;
    int i;

    set_diff(cleft, minimize_cube.fullset, minimize_cube.var_mask[best]);
    set_diff(cright, minimize_cube.fullset, minimize_cube.var_mask[best]);
    for (i = minimize_cube.first_part[best]; i <= lastbit; i++) {
        if (!is_in_set(cof, i)) {
            halfbit++;
        }
    }
    for (i = minimize_cube.first_part[best], halfbit = halfbit / 2; halfbit > 0; i++) {
        if (!is_in_set(cof, i)) {
            halfbit--;
            set_insert(cleft, i);
        }
    }
    for (; i <= lastbit; i++) {
        if (!is_in_set(cof, i)) {
            set_insert(cright, i);
        }
    }
    return best;
}

/* the cube with the most parts */
pset minimize_largest_cube(cubelist_t T)
{
    pset temp = minimize_cube.temp[0];
    pset best = NULL, p;
    int most = -1;
    for (pset *T1 = T + 2; (p = *T1) != NULL; T1++) {
        int n = set_ord(set_or(temp, T[0], p));
        if (n > most) {
            most = n;
            best = p;
        }
    }
    if (best == NULL) {
        minimize_fatal_error(ERR_INTERNAL_NO_LARGEST_CUBE, "");
    }
    return best;
}

/* split for a unate cover: one part of a variable in which the
 * largest cube is not full against the rest */
static int unate_split_select(cubelist_t T, pset cleft, pset cright)
{
    pset temp = minimize_cube.temp[0];
    int best = -1, mostzero = 0;
    int i;

    set_or(temp, T[0], minimize_largest_cube(T));
    for (int var = 0; var < minimize_cube.num_vars; var++) {
        if (!setp_implies(minimize_cube.var_mask[var], temp) && minimize_cdata.var_zeros[var] > mostzero) {
            best = var;
            mostzero = minimize_cdata.var_zeros[var];
        }
    }
    if (best == -1) {
        minimize_fatal_error(ERR_INTERNAL_NO_UNATE_VAR, "");
    }
    set_copy(cleft, minimize_cube.fullset);
    set_diff(cright, minimize_cube.fullset, minimize_cube.var_mask[best]);
    for (i = minimize_cube.first_part[best]; minimize_cdata.part_zeros[i] < 1; i++) {
    }
    set_insert(cright, i);
    set_remove(cleft, i);
    return best;
}

/* =========================================================================
 * Complement (Espresso compl.c)
 * ========================================================================= */
pcover minimize_complement(cubelist_t T);

/* complement of a single cube */
static pcover compl_cube(pset p)
{
    pset diff = minimize_cube.temp[7];
    pcover R = minimize_new_cover(minimize_cube.num_vars);
    for (int var = 0; var < minimize_cube.num_vars; var++) {
        if (!setp_implies(minimize_cube.var_mask[var], p)) {
            pset pdest = GETSET(R, R->count++);
            set_and(diff, p, minimize_cube.var_mask[var]);
            set_diff(pdest, minimize_cube.fullset, diff);
        }
    }
    return R;
}

/* merge the cubes of the two halves that differ only in the
 * splitting variable */
static void compl_d1merge(pset *L1, pset *R1)
{
    pset pl = *L1, pr = *R1;
    while (pl != NULL && pr != NULL) {
        int order = minimize_d1_order(L1, R1);
        if (order == 1) {
            pr = *(++R1);
        } else if (order == 0) {
            RESET(pr, SET_ACTIVE);
            set_or(pl, pl, pr);
            pr = *(++R1);
        } else if (order == -1) {
            pl = *(++L1);
        }
    }
}

/* raise the active cubes of one half into the other half of the
 * splitting variable where a cube of the other half allows it */
static void compl_lift(pset *A1, pset *B1, pset bcube, int var)
{
    pset lift = minimize_cube.temp[4];
    pset nomask = minimize_cube.temp[5];
    pset liftor = minimize_cube.temp[6];
    pset a, b;

    set_and(liftor, bcube, minimize_cube.var_mask[var]);
    set_diff(nomask, minimize_cube.fullset, minimize_cube.var_mask[var]);
    for (; (a = *A1) != NULL; A1++) {
        if (!TESTP(a, SET_ACTIVE)) {
            continue;
        }
        set_and(lift, a, nomask);
        set_or(lift, lift, liftor);
        for (pset *B = B1; (b = *B) != NULL; B++) {
            if (setp_implies(lift, b)) {
                set_or(a, a, liftor);
                SET(a, SET_ACTIVE);
                break;
            }
        }
    }
}

/* combine the complements of the two cofactors.  Only the cubes
 * of the right half are marked active before lifting. */
static pcover compl_merge(pcover L, pcover R, pset cl, pset cr, int var)
{
    pset p, last;
    foreach_set(L, last, p) {
        set_and(p, p, cl);
    }
    foreach_set(R, last, p) {
        set_and(p, p, cr);
        SET(p, SET_ACTIVE);
    }

    set_copy(minimize_cube.temp[0], minimize_cube.var_mask[var]);
    pset *L1 = minimize_sf_list(L);
    pset *R1 = minimize_sf_list(R);
    lincupl_sort_sets(L1, L->count, minimize_d1_order);
    lincupl_sort_sets(R1, R->count, minimize_d1_order);
    compl_d1merge(L1, R1);
    compl_lift(L1, R1, cr, var);
    compl_lift(R1, L1, cl, var);
    minimize_work_free(L1);
    minimize_work_free(R1);

    pcover Tbar = minimize_new_cover(L->count + R->count);
    pset pdest = Tbar->data;
    foreach_set(L, last, p) {
        set_copy(pdest, p);
        Tbar->count++;
        pdest += Tbar->wsize;
    }
    foreach_set(R, last, p) {
        if (TESTP(p, SET_ACTIVE)) {
            set_copy(pdest, p);
            Tbar->count++;
            pdest += Tbar->wsize;
        }
    }
    minimize_sf_free(L);
    minimize_sf_free(R);
    minimize_work_free(cl);
    minimize_work_free(cr);
    return Tbar;
}

/* returns 2 if the cover has to be split */
static int compl_special_cases(cubelist_t T, pcover *Tbar)
{
    pset cof = T[0];
    pset temp = minimize_cube.temp[0];
    pset ceil = minimize_cube.temp[1];
    pset p;

    if (T[2] == NULL) {
        /* empty cover: the complement is the universe */
        *Tbar = minimize_sf_addset(minimize_new_cover(1), minimize_cube.fullset);
    } else if (T[3] == NULL) {
        *Tbar = compl_cube(set_or(temp, cof, T[2]));
    } else {
        for (pset *T1 = T + 2; (p = *T1) != NULL; T1++) {
            if (minimize_full_row(p, cof)) {
                *Tbar = minimize_new_cover(0);
                minimize_cubelist_free(T);
                return 1;
            }
        }
        set_copy(ceil, cof);
        for (pset *T1 = T + 2; (p = *T1) != NULL; T1++) {
            set_or(ceil, ceil, p);
        }
        if (!setp_equal(ceil, minimize_cube.fullset)) {
            /* a part no cube has: factor it out */
            set_or(T[0], T[0], set_diff(temp, minimize_cube.fullset, ceil));
            pcover ceil_compl = compl_cube(ceil);
            *Tbar = minimize_sf_append(minimize_complement(T), ceil_compl);
            return 1;
        }
        minimize_massive_count(T);
        if (minimize_cdata.vars_active != 1) {
            return 2;
        }
        *Tbar = minimize_new_cover(0);
    }
    minimize_cubelist_free(T);
    return 1;
}

/* complement of a cube list; releases the list */
pcover minimize_complement(cubelist_t T)
{
    pcover Tbar;
    if (compl_special_cases(T, &Tbar) == 2) {
        pset cr = new_cube();
        pset cl = new_cube();
        int best;
        if (minimize_cdata.vars_unate == minimize_cdata.vars_active) {
            best = unate_split_select(T, cl, cr);
        } else {
            best = minimize_binate_split_select(T, cl, cr);
        }
        pcover Tr = minimize_complement(minimize_scofactor(T, cr, best));
        pcover Tl = minimize_complement(minimize_scofactor(T, cl, best));
        Tbar = compl_merge(Tl, Tr, cl, cr, best);
        minimize_cubelist_free(T);
    }
    return Tbar;
}

/* =========================================================================
 * Sharp and intersection (Espresso sharp.c)
 * ========================================================================= */

/* all pairwise intersections, without contained cubes */
pcover minimize_cv_intersect(pcover A, pcover B)
{
    pcover Save = NULL;
    pcover T = minimize_new_cover(INTERSECT_CHUNK);
    pset pt = T->data;
    pset pi, pj, lasti, lastj;

    foreach_set(A, lasti, pi) {
        foreach_set(B, lastj, pj) {
            if (minimize_cdist0(pi, pj)) {
                set_and(pt, pi, pj);
                if (++T->count >= T->capacity) {
                    T = minimize_sf_contain(T);
                    Save = Save == NULL ? T : minimize_sf_union(Save, T);
                    T = minimize_new_cover(INTERSECT_CHUNK);
                    pt = T->data;
                } else {
                    pt += T->wsize;
                }
            }
        }
    }
    T = minimize_sf_contain(T);
    return Save == NULL ? T : minimize_sf_union(Save, T);
}

/* a # b as a cover */
static pcover sharp(pset a, pset b)
{
    pset diff = minimize_cube.temp[0];
    pset part = minimize_cube.temp[1];
    pset rest = minimize_cube.temp[2];
    pcover R = minimize_new_cover(minimize_cube.num_vars);

    if (!minimize_cdist0(a, b)) {
        R = minimize_sf_addset(R, a);
    } else {
        set_diff(diff, a, b);
        for (int var = 0; var < minimize_cube.num_vars; var++) {
            if (!setp_empty(set_and(part, diff, minimize_cube.var_mask[var]))) {
                set_diff(rest, a, minimize_cube.var_mask[var]);
                set_or(GETSET(R, R->count++), part, rest);
            }
        }
    }
    return R;
}

/* c # T, intersecting the single sharps pairwise like a binary
 * counter so that the intermediate covers stay small.
 *
 * The bottom entry of the stack is the cover {c} with the same level as
 * the first sharp, so the result of all intersections ends up there. */
pcover minimize_cb_sharp(pset c, pcover T)
{
    struct {
        pcover cover;
        int level;
    } stack[MERGE_STACK_SIZE];
    int depth = 0;
    int n = 1;

    for (int count = T->count; count != 0; count >>= 1) {
        depth++;
    }
    stack[0].cover = minimize_sf_addset(minimize_new_cover(1), c);
    stack[0].level = depth;

    pset q, last;
    pset final = T->data + (size_t)T->wsize * (size_t)(T->count > 0 ? T->count - 1 : 0);
    foreach_set(T, last, q) {
        stack[n].cover = sharp(c, q);
        stack[n].level = depth;
        n++;
        while (n > 1 && (stack[n - 1].level == stack[n - 2].level || q == final)) {
            pcover merged = minimize_cv_intersect(stack[n - 1].cover, stack[n - 2].cover);
            int level = (stack[n - 1].level < stack[n - 2].level ? stack[n - 1].level
                                                                  : stack[n - 2].level) - 1;
            minimize_sf_free(stack[n - 2].cover);
            minimize_sf_free(stack[n - 1].cover);
            stack[n - 2].cover = merged;
            stack[n - 2].level = level;
            n--;
        }
    }
    return stack[0].cover;
}

/* =========================================================================
 * Cover utilities (Espresso cvrm.c, cvrmisc.c)
 * ========================================================================= */

/* split a cube into cubes that have a single part in each
 * variable from start on */
static pcover unravel_cube(pset c, int start)
{
    pset startbase = minimize_cube.temp[0];
    int expansion = 1;
    pset p, last;

    set_copy(startbase, minimize_cube.emptyset);
    for (int var = 0; var < minimize_cube.num_vars; var++) {
        int n = set_dist(c, minimize_cube.var_mask[var]);
        if (n < 2 || var < start) {
            set_or(startbase, startbase, minimize_cube.var_mask[var]);
        } else {
            expansion *= n;
            if (expansion > UNRAVEL_LIMIT) {
                minimize_fatal_error(ERR_INTERNAL_UNRAVEL_TOO_BIG, "");
            }
        }
    }
    set_and(startbase, c, startbase);

    pcover B = minimize_new_cover(expansion);
    B->count = expansion;
    foreach_set(B, last, p) {
        set_copy(p, startbase);
    }

    int size = expansion;
    for (int var = start; var < minimize_cube.num_vars; var++) {
        int n = set_dist(c, minimize_cube.var_mask[var]);
        if (n > 1) {
            int stride = size;
            int offset = 0;
            size /= n;
            for (int i = minimize_cube.first_part[var]; i <= minimize_cube.last_part[var]; i++) {
                if (is_in_set(c, i)) {
                    for (int j = offset; j < expansion; j += stride) {
                        for (int k = 0; k < size; k++) {
                            set_insert(GETSET(B, j + k), i);
                        }
                    }
                    offset += size;
                }
            }
        }
    }
    return B;
}

/* unravel every cube of B; releases B */
pcover minimize_unravel(pcover B, int start)
{
    pcover B1 = minimize_new_cover(B->count);
    pset p, last;
    foreach_set(B, last, p) {
        B1 = minimize_sf_append(B1, unravel_cube(p, start));
    }
    minimize_sf_free(B);
    return B1;
}

