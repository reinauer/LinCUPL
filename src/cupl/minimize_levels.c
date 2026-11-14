/*
 * minimize_levels.c - the minimization levels of the minimize phase.
 *
 * Tautology, the minimum cover of a 0/1 matrix and the irredundant cover
 * that level 2 (Quine-McCluskey) and level 3 (Presto) are built from, the
 * call of Berkeley Espresso 2.5.1 for level 4, and the verification of the
 * results against the input cover.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "minimize_espresso.h"
#include "minimize_int.h"

static int g_irred_columns;            /* candidates of select_minimal() */
static pset g_irred_fullset;           /* row template of ftautology() */

/* =========================================================================
 * Tautology (Espresso irred.c)
 * ========================================================================= */
static int tautology(cubelist_t T);

/* try to split the cover into two parts that share no variable */
static int taut_components(cubelist_t T)
{
    int ncubes = minimize_cubelist_slots(T) - 3;
    pset common = minimize_cube.temp[0];
    pset pc = minimize_cube.temp[6];
    pset cof = T[0];
    int count = 0;
    int change;
    pset p;

    for (pset *T1 = T + 2; (p = *T1) != NULL; T1++) {
        RESET(p, SET_COVERED);
    }
    set_or(common, cof, minimize_largest_cube(T));
    do {
        change = 0;
        for (pset *T1 = T + 2; (p = *T1) != NULL; T1++) {
            if (!TESTP(p, SET_COVERED) && minimize_ccommon(set_or(pc, p, cof), common)) {
                SET(p, SET_COVERED);
                set_and(common, common, pc);
                change = 1;
                count++;
            }
        }
    } while (change);
    if (count == ncubes) {
        return 2;
    }

    for (int covered = 1; covered >= 0; covered--) {
        cubelist_t part = minimize_cubelist_alloc(minimize_cubelist_slots(T));
        pset *pdest = part + 2;
        part[0] = set_copy(new_cube(), T[0]);
        for (pset *T1 = T + 2; (p = *T1) != NULL; T1++) {
            if (TESTP(p, SET_COVERED) == covered) {
                *pdest++ = p;
            }
        }
        *pdest = NULL;
        minimize_cubelist_set_end(part, pdest);
        if (covered) {
            if (tautology(part)) {
                minimize_cubelist_free(T);
                return 1;
            }
        } else {
            minimize_cubelist_free(T);
            return tautology(part);
        }
    }
    return 0;
}

/* returns 2 if the cover has to be split */
static int taut_special_cases(cubelist_t T)
{
    pset ceil = minimize_cube.temp[0];
    pset p;

    for (pset *T1 = T + 2; (p = *T1) != NULL; T1++) {
        if (minimize_full_row(p, T[0])) {
            minimize_cubelist_free(T);
            return 1;
        }
    }
    for (;;) {
        set_copy(ceil, T[0]);
        for (pset *T1 = T + 2; (p = *T1) != NULL; T1++) {
            set_or(ceil, ceil, p);
        }
        if (!setp_equal(ceil, minimize_cube.fullset)) {
            minimize_cubelist_free(T);
            return 0;
        }
        minimize_massive_count(T);
        if (minimize_cdata.vars_unate == minimize_cdata.vars_active) {
            minimize_cubelist_free(T);
            return 0;
        }
        if (minimize_cdata.vars_active == 1) {
            minimize_cubelist_free(T);
            return 1;
        }
        if (minimize_cdata.vars_unate == 0) {
            if ((double)minimize_cdata.var_zeros[minimize_cdata.best] < (double)(minimize_cubelist_slots(T) - 3) * 0.4) {
                return taut_components(T);
            }
            return 2;
        }
        /* only the cubes that are full in the unate variables matter */
        for (int var = 0; var < minimize_cube.num_vars; var++) {
            if (minimize_cdata.is_unate[var]) {
                pset *pdest = T + 2;
                for (pset *T1 = T + 2; (p = *T1) != NULL; T1++) {
                    if (setp_implies(minimize_cube.var_mask[var], set_or(ceil, p, T[0]))) {
                        *pdest++ = p;
                    }
                }
                *pdest = NULL;
                minimize_cubelist_set_end(T, pdest);
            }
        }
    }
}

/* is the cube list a tautology?  Releases the list. */
static int tautology(cubelist_t T)
{
    int result = taut_special_cases(T);
    if (result != 2) {
        return result;
    }
    pset cr = new_cube();
    pset cl = new_cube();
    int best = minimize_binate_split_select(T, cl, cr);
    result = tautology(minimize_scofactor(T, cl, best)) && tautology(minimize_scofactor(T, cr, best));
    minimize_cubelist_free(T);
    minimize_work_free(cl);
    minimize_work_free(cr);
    return result;
}

/* c is covered by the cubes of T (T is kept) */
static int cube_is_covered(cubelist_t T, pset c)
{
    return tautology(minimize_cofactor(T, c));
}

/* Tautology that records which redundant cubes it needed: the result is a
 * family over the redundant cubes with one set per way of failing. */
static pcover ftautology(cubelist_t T);

/* returns 2 if the cover has to be split */
static int ftaut_special_cases(cubelist_t T, pcover *Tsave)
{
    pset temp = minimize_cube.temp[0];
    pset p;

    for (pset *T1 = T + 2; (p = *T1) != NULL; T1++) {
        if (!TESTP(p, SET_REDUND) && minimize_full_row(p, T[0])) {
            *Tsave = minimize_sf_new(0, g_irred_columns);
            minimize_cubelist_free(T);
            return 1;
        }
    }
    minimize_massive_count(T);
    if (minimize_cdata.vars_unate == minimize_cdata.vars_active) {
        *Tsave = minimize_sf_new(1, g_irred_columns);
        (*Tsave)->count = 1;
        pset row = set_copy((*Tsave)->data, g_irred_fullset);
        unsigned n = (unsigned)g_irred_columns;
        for (pset *T1 = T + 2; (p = *T1) != NULL; T1++) {
            if (TESTP(p, SET_REDUND) && minimize_full_row(p, T[0])) {
                n--;
                set_remove(row, (int)SIZE(p));
            }
        }
        put_size(row, n);
        minimize_cubelist_free(T);
        return 1;
    }
    if (minimize_cdata.vars_unate == 0) {
        return 2;
    }
    pset *pdest = T + 2;
    for (pset *T1 = T + 2; (p = *T1) != NULL; T1++) {
        int keep = 1;
        for (int var = 0; var < minimize_cube.num_vars && keep; var++) {
            if (minimize_cdata.is_unate[var]) {
                keep = setp_implies(minimize_cube.var_mask[var], set_or(temp, p, T[0]));
            }
        }
        if (keep) {
            *pdest++ = p;
        }
    }
    *pdest = NULL;
    minimize_cubelist_set_end(T, pdest);
    *Tsave = ftautology(T);
    return 1;
}

static pcover ftautology(cubelist_t T)
{
    pcover Tsave;
    if (ftaut_special_cases(T, &Tsave) == 2) {
        pset cr = new_cube();
        pset cl = new_cube();
        int best = minimize_binate_split_select(T, cl, cr);
        pcover right = ftautology(minimize_scofactor(T, cr, best));
        pcover left = ftautology(minimize_scofactor(T, cl, best));
        Tsave = minimize_sf_union(left, right);
        minimize_cubelist_free(T);
        minimize_work_free(cl);
        minimize_work_free(cr);
    }
    return Tsave;
}

/* ftautology() of T cofactored against c (T is kept) */
static pcover fcube_is_covered(cubelist_t T, pset c)
{
    return ftautology(minimize_cofactor(T, c));
}

/* =========================================================================
 * Minimum cover of a 0/1 matrix (rows = sets, columns = elements), used to
 * pick the redundant cubes to keep and the parts to lower.  Heuristic: an
 * independent set of rows gives a lower bound and a starting point for a
 * greedy column selection by weight.
 * ========================================================================= */

/* weight of each column over the active rows; a row with n
 * elements contributes 65536/n to each of its columns */
static int *column_weights(pcover A)
{
    int *weight = minimize_work_alloc((size_t)(A->sf_size > 0 ? A->sf_size : 0) * sizeof(int));
    pset p, last;
    foreach_set(A, last, p) {
        if (TESTP(p, SET_ACTIVE)) {
            int n = set_ord(p);
            if (n != 0) {
                set_adjcnt(p, weight, 0x10000 / n);
            }
        }
    }
    return weight;
}

/* the same, counting only the elements of a row that are in
 * the given column set */
static int *column_weights_within(pcover A, pset columns)
{
    int *weight = minimize_work_alloc((size_t)(A->sf_size > 0 ? A->sf_size : 0) * sizeof(int));
    pset p, last;
    foreach_set(A, last, p) {
        if (TESTP(p, SET_ACTIVE)) {
            int n = set_dist(p, columns);
            if (n != 0) {
                set_adjcnt(p, weight, 0x10000 / n);
            }
        }
    }
    return weight;
}

/* add the heaviest column (of within, if given) to cover and
 * retire the rows it covers */
static void select_column(pcover A, int *weight, pset cover, pset within)
{
    int best = 0, column = -1;
    pset p, last;

    for (int i = 0; i < A->sf_size; i++) {
        if ((within == NULL || is_in_set(within, i)) && weight[i] > best) {
            best = weight[i];
            column = i;
        }
    }
    if (column == -1) {
        minimize_fatal_error(ERR_INTERNAL_NO_COLUMN, "");
    }
    set_insert(cover, column);
    foreach_set(A, last, p) {
        if (TESTP(p, SET_ACTIVE) && is_in_set(p, column)) {
            set_adjcnt(p, weight, -0x10000 / set_ord(p));
            if (within != NULL && TESTP(p, SET_PRIME)) {
                set_diff(within, within, p);
            }
            A->active_count--;
            RESET(p, SET_ACTIVE);
        }
    }
}

/* graph over the active rows with an edge between disjoint rows */
static pcover disjoint_graph(pcover A)
{
    int n = A->active_count;
    pcover G = minimize_sf_new(n, n);
    pset last = A->data + (size_t)A->count * (size_t)A->wsize;
    pset p = A->data;

    G->count = n;
    for (int i = 0; i < n; i++) {
        set_clear(GETSET(G, i), n);
    }
    for (int i = 0; i < n - 1 && p < last; p += A->wsize) {
        if (TESTP(p, SET_ACTIVE)) {
            int j = i + 1;
            for (pset q = p + A->wsize; j < n && q < last; q += A->wsize) {
                if (TESTP(q, SET_ACTIVE)) {
                    if (setp_disjoint(p, q)) {
                        set_insert(GETSET(G, i), j);
                        set_insert(GETSET(G, j), i);
                    }
                    j++;
                }
            }
            i++;
        }
    }
    return G;
}

/* greedy maximal set of pairwise disjoint rows */
static void independent_rows(pcover G, pset indep)
{
    int n = G->sf_size;
    pset candidates = set_fill(set_new(n), n);
    pset tied = set_new(n);
    pset temp = set_new(n);
    pset p;
    int i;

    minimize_sf_active(G);
    set_clear(indep, n);
    while (G->active_count != 0) {
        /* rows with the most edges to the remaining candidates */
        int most = -1;
        for (i = 0, p = G->data; i < G->count; i++, p += G->wsize) {
            if (TESTP(p, SET_ACTIVE)) {
                int degree = set_dist(p, candidates);
                if (degree > most) {
                    set_clear(tied, n);
                    set_insert(tied, i);
                    most = degree;
                } else if (degree == most) {
                    set_insert(tied, i);
                }
            }
        }
        /* among those the one with the most edges to the others */
        int pick = -1;
        most = -1;
        for (i = 0, p = G->data; i < G->count; i++, p += G->wsize) {
            if (TESTP(p, SET_ACTIVE) && is_in_set(tied, i)) {
                int degree = set_dist(p, tied);
                if (degree > most) {
                    pick = i;
                    most = degree;
                }
            }
        }
        set_insert(indep, pick);
        set_remove(candidates, pick);
        G->active_count--;
        RESET(GETSET(G, pick), SET_ACTIVE);
        /* rows that are not disjoint from all chosen rows drop out */
        for (i = 0, p = G->data; i < G->count; i++, p += G->wsize) {
            if (TESTP(p, SET_ACTIVE) && !setp_empty(set_diff(temp, indep, p))) {
                G->active_count--;
                RESET(p, SET_ACTIVE);
                set_remove(candidates, i);
            }
        }
    }
    minimize_work_free(candidates);
    minimize_work_free(tied);
    minimize_work_free(temp);
}

/* cover the independent rows among the active rows, one column
 * per independent row */
static void cover_independent_rows(pcover A, pset cover, pset indep, int find_rows, int static_weights)
{
    pset choice = set_new(A->sf_size);
    pset p, last;
    int i = 0;

    if (find_rows) {
        pcover G = disjoint_graph(A);
        independent_rows(G, indep);
        minimize_sf_free(G);
    }
    foreach_set(A, last, p) {
        if (TESTP(p, SET_ACTIVE)) {
            if (is_in_set(indep, i)) {
                SET(p, SET_PRIME);
                set_or(choice, choice, p);
            } else {
                RESET(p, SET_PRIME);
            }
            i++;
        }
    }
    if (static_weights) {
        int *weight = column_weights(A);
        while (!setp_empty(choice)) {
            select_column(A, weight, cover, choice);
        }
        minimize_work_free(weight);
    } else {
        while (!setp_empty(choice)) {
            int *weight = column_weights_within(A, choice);
            select_column(A, weight, cover, choice);
            minimize_work_free(weight);
        }
    }
    minimize_work_free(choice);
}

/* keep the columns that are the only one covering some row and
 * cover the remaining rows again */
static void make_cover_irredundant(pcover A, pset cover)
{
    pset temp = set_new(A->sf_size);
    pset needed = set_new(A->sf_size);
    pset p, last;

    foreach_set(A, last, p) {
        if (set_ord(set_and(temp, p, cover)) == 1) {
            set_or(needed, needed, temp);
        }
    }
    if (!setp_empty(set_diff(temp, cover, needed))) {
        A->active_count = 0;
        foreach_set(A, last, p) {
            if (setp_disjoint(p, needed)) {
                A->active_count++;
                SET(p, SET_ACTIVE);
            }
        }
        int *weight = column_weights(A);
        while (A->active_count != 0) {
            select_column(A, weight, needed, NULL);
        }
        minimize_work_free(weight);
    }
    set_copy(cover, needed);
    minimize_work_free(temp);
    minimize_work_free(needed);
}

/* a small set of columns that covers every row of A; *bound is
 * the number of independent rows found */
static pset minimum_cover(pcover A, int *bound)
{
    pset indep = set_new(A->count);
    pset indep_rest = set_new(A->count);
    pset best = set_new(A->sf_size);
    pset cover = set_new(A->sf_size);
    pset p, last;

    if (A->count > 0) {
        foreach_set(A, last, p) {
            if (setp_empty(p)) {
                minimize_fatal_error(ERR_INTERNAL_EMPTY_ROW, "");
            }
        }
        /* plain greedy cover */
        int *weight = column_weights(minimize_sf_active(A));
        while (A->active_count != 0) {
            select_column(A, weight, best, NULL);
        }
        minimize_work_free(weight);

        /* independent rows first, static weights */
        cover_independent_rows(minimize_sf_active(A), cover, indep, 1, 1);
        while (A->active_count != 0) {
            cover_independent_rows(A, cover, indep_rest, 1, 1);
        }
        make_cover_irredundant(A, cover);
        if (set_ord(cover) < set_ord(best)) {
            set_copy(best, cover);
        }
        if (set_ord(indep) != set_ord(best)) {
            /* bound not reached: once more with weights recomputed */
            set_clear(cover, A->sf_size);
            cover_independent_rows(minimize_sf_active(A), cover, indep, 0, 0);
            while (A->active_count != 0) {
                cover_independent_rows(A, cover, indep_rest, 1, 0);
            }
            make_cover_irredundant(A, cover);
            if (set_ord(cover) < set_ord(best)) {
                set_copy(best, cover);
            }
        }
    }
    foreach_set(A, last, p) {
        if (setp_disjoint(p, best)) {
            minimize_fatal_error(ERR_INTERNAL_ROW_NOT_COVERED, "");
        }
    }
    *bound = set_ord(indep);
    minimize_work_free(indep);
    minimize_work_free(indep_rest);
    minimize_work_free(cover);
    return best;
}

/* =========================================================================
 * Irredundant cover (Espresso irred.c)
 * ========================================================================= */

/* list of the cubes of both lists with room for all of them */
static cubelist_t cubelist_join(cubelist_t F1, cubelist_t D1)
{
    cubelist_t R = minimize_cubelist_alloc(minimize_cubelist_slots(F1) + minimize_cubelist_slots(D1));
    pset *pdest = R + 2;
    R[0] = set_copy(new_cube(), F1[0]);
    for (pset *T1 = F1 + 2; *T1 != NULL; T1++) {
        *pdest++ = *T1;
    }
    for (pset *T1 = D1 + 2; *T1 != NULL; T1++) {
        *pdest++ = *T1;
    }
    *pdest = NULL;
    minimize_cubelist_set_end(R, pdest);
    return R;
}

/* refill R with the relatively essential cubes of F1 and D1 */
static void cubelist_essential(cubelist_t F1, cubelist_t D1, cubelist_t R)
{
    pset *pdest = R + 2;
    set_copy(R[0], F1[0]);
    for (pset *T1 = F1 + 2; *T1 != NULL; T1++) {
        if (!TESTP(*T1, SET_NONESSEN)) {
            *pdest++ = *T1;
        }
    }
    for (pset *T1 = D1 + 2; *T1 != NULL; T1++) {
        *pdest++ = *T1;
    }
    *pdest = NULL;
    minimize_cubelist_set_end(R, pdest);
}

/* of the partially redundant cubes (active in F1) keep a minimum
 * set that together with R still covers all of them */
static int select_minimal(cubelist_t F1, cubelist_t R, int *count)
{
    pset *pdest;
    pset p;
    int n = 0;

    for (pdest = R + 2; (p = *pdest) != NULL; pdest++) {
        RESET(p, SET_REDUND);
    }
    /* number the candidates and add them to the list */
    for (pset *T1 = F1 + 2; (p = *T1) != NULL; T1++) {
        if (TESTP(p, SET_ACTIVE)) {
            put_size(p, (unsigned)n);
            SET(p, SET_REDUND);
            *pdest++ = p;
            n++;
        }
    }
    g_irred_columns = n;
    *pdest = NULL;
    minimize_cubelist_set_end(R, pdest);

    /* for each candidate the sets of candidates that could cover it */
    pcover table = minimize_sf_new(0, n);
    g_irred_fullset = set_new(n);
    n = 0;
    for (pset *T1 = F1 + 2; (p = *T1) != NULL; T1++) {
        if (TESTP(p, SET_ACTIVE)) {
            set_fill(g_irred_fullset, g_irred_columns);
            set_remove(g_irred_fullset, n);
            table = minimize_sf_append(table, fcube_is_covered(R, p));
            RESET(p, SET_REDUND);
            n++;
        }
    }
    table = minimize_sf_contain(table);
    set_fill(g_irred_fullset, g_irred_columns);
    pset last;
    foreach_set(table, last, p) {
        set_diff(p, g_irred_fullset, p);
    }
    int bound;
    pset keep = minimum_cover(table, &bound);

    n = 0;
    for (pset *T1 = F1 + 2; (p = *T1) != NULL; T1++) {
        if (!TESTP(p, SET_NONESSEN)) {
            SET(p, SET_ACTIVE);
        } else if (TESTP(p, SET_ACTIVE)) {
            if (!is_in_set(keep, n)) {
                RESET(p, SET_ACTIVE);
            }
            n++;
        }
    }
    *count = set_ord(keep);
    minimize_sf_free(table);
    minimize_work_free(keep);
    minimize_work_free(g_irred_fullset);
    g_irred_fullset = NULL;
    return bound;
}

/* leave active the cubes of F1 that form an irredundant cover
 * together with the don't-care cubes D1 */
static void mark_irredundant(cubelist_t F1, cubelist_t D1)
{
    int count = 0;
    cubelist_t R = cubelist_join(F1, D1);
    pset p;

    /* relatively essential: not covered by all the other cubes */
    for (pset *T1 = F1 + 2; (p = *T1) != NULL; T1++) {
        if (cube_is_covered(R, p)) {
            SET(p, SET_ACTIVE | SET_NONESSEN);
        } else {
            RESET(p, SET_ACTIVE | SET_NONESSEN);
        }
    }
    /* totally redundant: covered by the relatively essential cubes alone */
    cubelist_essential(F1, D1, R);
    for (pset *T1 = F1 + 2; (p = *T1) != NULL; T1++) {
        if (TESTP(p, SET_ACTIVE) && cube_is_covered(R, p)) {
            RESET(p, SET_ACTIVE);
        }
    }
    select_minimal(F1, R, &count);
    minimize_cubelist_free(R);
}

/* remove from F the cubes not needed for an irredundant cover with D */
static pcover irredundant(pcover F, pcover D)
{
    cubelist_t D1 = minimize_cube1list(D);
    cubelist_t F1 = minimize_cube1list(F);
    mark_irredundant(F1, D1);
    minimize_cubelist_free(F1);
    minimize_cubelist_free(D1);
    return minimize_sf_inactive(F);
}

/* =========================================================================
 * The three minimization levels
 * ========================================================================= */

/* Level 4: the cover is minimized by Berkeley Espresso 2.5.1
 * (minimize_espresso.c, third_party/espresso), not by the routines of this
 * file, so the result can differ from what the algorithm of levels 2 and 3
 * would produce.  The off-set that minimize_read_pla() computed is not passed on;
 * Espresso derives its own from the on-set and the don't-care set.  The
 * caller verifies the result against the input cover. */
static pcover espresso_cover(pcover F, pcover D)
{
    minimize_espresso_problem_t problem;
    uint32_t *cubes = NULL;
    int count = 0;

    problem.num_inputs = minimize_cube.num_binary_vars;
    problem.num_outputs = minimize_cube.part_size[minimize_cube.num_vars - 1];
    problem.set_words = F->wsize;
    problem.on_set = F->data;
    problem.on_count = F->count;
    problem.dc_set = D->data;
    problem.dc_count = D->count;
    problem.tick = minimize_progress_tick;

    int status = minimize_espresso_minimize(&problem, &cubes, &count);
    if (status == MINIMIZE_ESPRESSO_NO_MEMORY) {
        minimize_fatal_error(ERR_OUT_OF_MEMORY, "espresso");
    }
    if (status != MINIMIZE_ESPRESSO_OK) {
        minimize_fatal_error(ERR_INTERNAL_ESPRESSO, "espresso");
    }

    pcover M = minimize_new_cover(count);
    pset c = new_cube();
    for (int i = 0; i < count; i++) {
        /* word 0 (size and flags) is minimize's own */
        memcpy(c + 1, cubes + (size_t)i * (size_t)F->wsize + 1, (size_t)(F->wsize - 1) * sizeof(uint32_t));
        M = minimize_sf_addset(M, c);
    }
    minimize_work_free(c);
    free(cubes);
    minimize_sf_free(F);
    return M;
}

/* level 3, first half: add input parts to each cube as long as
 * the cube stays inside the cover; drop the cubes that end up contained */
static int presto_expand(pla_t *PLA, pcover D)
{
    int changed = 0, removed = 0;
    int last_input_part = minimize_cube.last_part[minimize_cube.num_binary_vars - 1];
    pset c = new_cube();
    pcover F = minimize_size_sort(PLA->F);
    pset p, q, last;

    minimize_sf_active(F);
    cubelist_t FD = minimize_cube2list(F, D);
    foreach_set(F, last, p) {
        if (TESTP(p, SET_ACTIVE) && !TESTP(p, SET_PRIME)) {
            int raised = 0;
            set_copy(c, p);
            for (int i = 0; i <= last_input_part; i++) {
                if (!is_in_set(c, i)) {
                    set_insert(c, i);
                    if (cube_is_covered(FD, c)) {
                        raised = 1;
                    } else {
                        set_remove(c, i);
                    }
                }
            }
            if (raised) {
                set_copy(p, c);
                for (q = p + F->wsize; q < last; q += F->wsize) {
                    if (setp_implies(q, p)) {
                        removed = 1;
                        F->active_count--;
                        RESET(q, SET_ACTIVE);
                    }
                }
                changed = 1;
            }
            SET(p, SET_PRIME);
        }
    }
    minimize_cubelist_free(FD);
    if (removed) {
        F = minimize_sf_inactive(F);
    }
    minimize_work_free(c);
    PLA->F = F;
    return changed;
}

/* level 3, second half: take away from each cube the outputs
 * that the rest of the cover provides */
static int presto_reduce(pla_t *PLA, pcover D)
{
    int changed = 0, removed = 0;
    int out = minimize_cube.num_vars - 1;
    pset whole = minimize_cube.temp[7];
    pset temp = minimize_cube.temp[8];
    pcover F = minimize_size_sort(PLA->F);
    pset p, last;

    minimize_sf_active(F);
    cubelist_t FD = minimize_cube2list(F, D);
    foreach_set(F, last, p) {
        int lowered = 0;
        set_copy(whole, p);
        for (int i = minimize_cube.first_part[out]; i <= minimize_cube.last_part[out]; i++) {
            if (is_in_set(p, i)) {
                set_remove(p, i);
                if (cube_is_covered(FD, whole)) {
                    lowered = 1;
                } else {
                    set_insert(p, i);
                }
            }
        }
        if (lowered) {
            changed = 1;
            RESET(p, SET_PRIME);
            if (setp_empty(set_and(temp, p, minimize_cube.var_mask[out]))) {
                removed = 1;
                F->active_count--;
                RESET(p, SET_ACTIVE);
            }
        }
    }
    minimize_cubelist_free(FD);
    if (removed) {
        F = minimize_sf_inactive(F);
    }
    PLA->F = F;
    return changed;
}

/* Level 2: all prime implicants as the complement of the
 * off-set, then an irredundant subset */
static void quine_mccluskey(pla_t *PLA)
{
    minimize_sf_free(PLA->F);
    PLA->R = minimize_lex_sort(minimize_sf_contain(minimize_unravel(PLA->R, minimize_cube.num_vars - 1)));
    PLA->F = minimize_cb_sharp(minimize_cube.fullset, PLA->R);
    PLA->F = irredundant(PLA->F, PLA->D);
}

/* =========================================================================
 * Verification (Espresso verify.c)
 * ========================================================================= */

/* F must lie inside Fold + Dold and cover Fold; 1 on error */
static int verify(pcover F, pcover Fold, pcover Dold)
{
    int error = 0;
    pset p, last;

    cubelist_t FD = minimize_cube2list(Fold, Dold);
    foreach_set(F, last, p) {
        if (!cube_is_covered(FD, p)) {
            error = 1;
            minimize_report_error(ERR_VERIFY_NOT_COVERED, "");
            break;
        }
    }
    minimize_cubelist_free(FD);

    FD = minimize_cube2list(F, Dold);
    foreach_set(Fold, last, p) {
        if (!cube_is_covered(FD, p)) {
            error = 1;
            minimize_report_error(ERR_VERIFY_NOT_CONTAINED, "");
            break;
        }
    }
    minimize_cubelist_free(FD);
    return error;
}

/* report how on-set, don't-care set and off-set relate */
static void check_consistency(pla_t *PLA)
{
    pcover T;

    T = minimize_cv_intersect(PLA->F, PLA->D);
    minimize_report_error(T->count != 0 ? ERR_CHECK_ON_DC_OVERLAP : ERR_CHECK_ON_DC_DISJOINT, "");
    minimize_sf_free(T);
    T = minimize_cv_intersect(PLA->F, PLA->R);
    minimize_report_error(T->count != 0 ? ERR_CHECK_ON_OFF_OVERLAP : ERR_CHECK_ON_OFF_DISJOINT, "");
    minimize_sf_free(T);
    T = minimize_cv_intersect(PLA->D, PLA->R);
    minimize_report_error(T->count != 0 ? ERR_CHECK_DC_OFF_OVERLAP : ERR_CHECK_DC_OFF_DISJOINT, "");
    minimize_sf_free(T);

    if (tautology(minimize_cube3list(PLA->F, PLA->D, PLA->R))) {
        minimize_report_error(ERR_CHECK_COMPLETE, "");
    } else {
        T = minimize_complement(minimize_cube3list(PLA->F, PLA->D, PLA->R));
        minimize_report_error(ERR_CHECK_INCOMPLETE, "");
        minimize_sf_free(T);
    }
}

/* =========================================================================
 * Minimizing a PLA
 * ========================================================================= */

/* minimize the covers of a PLA at the given level */
void minimize_pla(pla_t *PLA, int level)
{
    int error = 0;

    minimize_sf_forget();

    if (level == 4) {
        pcover Fold = minimize_sf_save(PLA->F);
        PLA->F = espresso_cover(PLA->F, PLA->D);
        error = verify(PLA->F, Fold, PLA->D);
        if (error) {
            PLA->F = Fold;
            check_consistency(PLA);
        }
    } else if (level == 3) {
        pcover Fold = minimize_sf_save(PLA->F);
        presto_expand(PLA, PLA->D);
        while (presto_reduce(PLA, PLA->D) && presto_expand(PLA, PLA->D)) {
        }
        error = verify(PLA->F, Fold, PLA->D);
    } else if (level == 2) {
        quine_mccluskey(PLA);
    } else if (level != 0 && level != 1) {
        minimize_fatal_error(ERR_INTERNAL_BAD_LEVEL, "unknown minimization level");
    }
    if (error) {
        minimize_fatal_error(ERR_INTERNAL_VERIFY, "verification");
    }
}

/* Reset the state of the irredundant cover for a new run of the phase. */
void minimize_levels_reset(void)
{
    g_irred_columns = 0;
    g_irred_fullset = NULL;
}
