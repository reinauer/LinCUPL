/*
 * min_domlist.c - two-level minimisation of the ATF150x fitter on lists of
 * product terms ("DomList") with sets of variables ("VarSet").
 *
 * A product term is a FitPTerm: two bit vectors `bits` and `care` with one
 * bit per variable.  (bits, care) = (1,1) is the literal x, (0,0) the
 * literal !x, (0,1) "x does not occur"; (1,0) appears in intermediate
 * results for a variable that occurs in both forms.  A variable occurs in a
 * term where bits == care.
 *
 * The entry point of the CUPL flow is domlist_minimise_entity:
 * every function is flattened to a sum of products, then each term is
 * expanded (literals are dropped one at a time, highest variable first, as
 * long as the term stays inside the function), terms covered by an expanded
 * term are dropped, and finally terms covered by the rest of the list are
 * removed.  "Inside the function" is decided by domlist_includes, a
 * tautology check of the cofactor.
 */
#include <stdio.h>
#include <stdlib.h>

#include "min_priv.h"
#include "tt.h"

/* A list header that shares the item array of another list ("view");
 * only count and items are used. */
typedef struct DomView {
    int    count;
    void **items;
} DomView;

static int domlist_includes(void **items, int count, FitPTerm *pt, int nw);

/* the variable with this id (inputs, outputs, locals) */
static FitVariable *domlist_find_var(int id, FitEntity *e)
{
    FitVariable *v = var_list_find_id(e->inputs, id);

    if (v == NULL) {
        v = var_list_find_id(e->outputs, id);
        if (v == NULL)
            v = var_list_find_id(e->locals, id);
    }
    return v;
}

/* the variables a tree refers to, in order of first appearance */
static FitList *domlist_tree_vars(FitEntity *e, FitFctTree *tree)
{
    FitList *ids = list_create();
    FitList *vars = list_create();

    fcttree_collect_ids(tree, ids);
    for (int i = 0; i < min_count(ids); i++)
        list_add(vars, domlist_find_var(FIT_PTR2INT(ids->items[i]), e));
    list_destroy(&ids);
    return vars;
}

/* constant, variable, or the negation of one */
static int domlist_tree_is_literal(const FitFctTree *t)
{
    if (t->sel == FIT_SEL_CONST || t->sel == FIT_SEL_ID)
        return 1;
    if (t->sel == FIT_SEL_NOT) {
        const FitFctTree *c = t->u.children->items[0];

        if (c->sel == FIT_SEL_CONST || c->sel == FIT_SEL_ID)
            return 1;
    }
    return 0;
}

/* add the variables of a term to a set */
static void domlist_varset_add(unsigned *vs, const FitPTerm *pt, int nw)
{
    for (int k = 0; k < nw; k++)
        vs[k] |= ~(pt->care[k] ^ pt->bits[k]);
}

/* RemoveSpecifiedVar: clear `mask` in word `idx`; returns 1 when
 * the set is not empty afterwards */
static int domlist_varset_remove(unsigned *vs, unsigned mask, int idx, int nw)
{
    if (idx < 0) {
        fprintf(g_msg, "Problem in RemoveSpecifiedVar\n");
        return 0;               /* nothing is written in front of the set */
    }
    vs[idx] &= ~mask;
    if (vs[idx] != 0)
        return 1;
    for (int k = 0; k < nw; k++) {
        if (vs[k] != 0)
            return 1;
    }
    return 0;
}

/* RemovePTVars: remove the variables of a term from a set;
 * returns 1 when the set is not empty afterwards */
static int domlist_varset_remove_pterm(unsigned *vs, const FitPTerm *pt, int nw)
{
    unsigned any = 0;

    if (nw < 0)
        fprintf(g_msg, "Problem in RemovePTVars\n");
    for (int k = 0; k < nw; k++) {
        vs[k] &= pt->care[k] ^ pt->bits[k];
        any |= vs[k];
    }
    return any != 0;
}

/* lowest variable of the highest non-empty word of a set:
 * returns the word index (-1 for an empty set) and the bit in *mask */
static int domlist_varset_last(const unsigned *vs, int nw, unsigned *mask)
{
    for (int k = nw - 1; k >= 0; k--) {
        if (vs[k] != 0) {
            *mask = ~(vs[k] - 1u) & vs[k];
            return k;
        }
    }
    return -1;
}

/* does the term have exactly one variable of the set?
 * 0: yes (*idx = its word, *bits / *care = that word of the term masked to
 * the set), -1: it has none, 1: it has more than one. */
static int domlist_pterm_univar(const FitPTerm *pt, const unsigned *vs, int nw,
                                unsigned *bits, unsigned *care, int *idx)
{
    int found = 0;

    *idx = -1;
    for (int k = 0; k < nw; k++) {
        unsigned w = ~(pt->bits[k] ^ pt->care[k]) & vs[k];

        if (w == 0)
            continue;
        if ((w & (w - 1u)) != 0)
            return 1;
        if (*idx >= 0)
            return 1;
        found = 1;
        *bits = vs[k] & pt->bits[k];
        *care = ~vs[k] | pt->care[k];
        *idx = k;
    }
    return found - 1;
}

/* 1 when the term has no variable of the set */
static int domlist_pterm_no_var_in(const FitPTerm *pt, const unsigned *vs, int nw)
{
    for (int k = 0; k < nw; k++) {
        if ((~(pt->care[k] ^ pt->bits[k]) & vs[k]) != 0)
            return 0;
    }
    return 1;
}

/* 1 when the term has a variable of the set */
static int domlist_pterm_has_var_in(const FitPTerm *pt, const unsigned *vs, int nw)
{
    for (int k = 0; k < nw; k++) {
        if ((~(pt->care[k] ^ pt->bits[k]) & vs[k]) != 0)
            return 1;
    }
    return 0;
}

/* is the term compatible with the cube (a, b)?  For every
 * variable of the cube (a == b) the term must not have the variable, or
 * have it with bits == a ^ x.  With n >= 1 the cube has n words; with
 * n <= 0 it is the single word -n of the term against a[0], b[0]. */
static int domlist_pterm_matches(const FitPTerm *pt, const unsigned *a, const unsigned *b,
                                 int n, unsigned x)
{
    if (n < 1) {
        unsigned u = pt->bits[-n];

        return ((pt->care[-n] ^ u) | (a[0] ^ u ^ x) | (b[0] ^ a[0])) == 0xffffffffu;
    }
    for (int k = 0; k < n; k++) {
        unsigned u = pt->bits[k];

        if (((pt->care[k] ^ u) | (u ^ a[k] ^ x) | (b[k] ^ a[k])) != 0xffffffffu)
            return 0;
    }
    return 1;
}

/* append to `out` the terms of the view that match (a, b).
 * `out` may share its item array with the view (count 0 at entry): the
 * selected terms then replace the view in place, which works because a term
 * is written to a slot that has already been read and ListAddElt only grows
 * a full list. */
static void domlist_select(const DomView *view, const unsigned *a, const unsigned *b, int n,
                           FitList *out, unsigned x)
{
    void **items = view->items;
    int count = view->count;

    for (int i = 0; i < count; i++) {
        FitPTerm *pt = items[i];

        if (domlist_pterm_matches(pt, a, b, n, x))
            list_add(out, pt);
    }
}

/* 1 when some term has exactly one variable of the set; the
 * outputs describe the last such term looked at */
static int domlist_find_univar(const DomView *view, const unsigned *vs, int nw,
                               unsigned *bits, unsigned *care, int *idx)
{
    for (int i = 0; i < view->count; i++) {
        if (domlist_pterm_univar(view->items[i], vs, nw, bits, care, idx) == 0)
            return 1;
    }
    return 0;
}

/* 1 when some term has no variable of the set left */
static int domlist_has_free_term(void **items, int count, const unsigned *vs, int nw)
{
    for (int i = 0; i < count; i++) {
        if (domlist_pterm_no_var_in(items[i], vs, nw))
            return 1;
    }
    return 0;
}

/* the product of all terms restricted to the set: afterwards a
 * variable occurs in the result when it occurs in the terms in one form
 * only */
static FitPTerm *domlist_mono_forms(const DomView *view, int nw, const unsigned *vs)
{
    FitPTerm *f = pterm_new(nw);

    for (int i = 0; i < view->count; i++)
        pterm_and_into(f, view->items[i], nw);
    for (int k = nw - 1; k >= 0; k--) {
        f->bits[k] &= vs[k];
        f->care[k] |= ~vs[k];
    }
    return f;
}

/* the variable of the set to split on: the one that occurs in
 * the most terms, among those the one whose two forms are most balanced,
 * among those the first found (highest word, lowest bit).  Returns the word
 * index, the bit in *mask. */
static int domlist_best_var(const DomView *view, const unsigned *vs, int nw, unsigned *mask)
{
    int best_diff = view->count + 1;
    int best_sum = 0;
    int best_idx = 0;           /* only used as is for an empty set */
    unsigned *left = bitvec_copy_new(vs, nw);
    unsigned m;
    int idx = domlist_varset_last(left, nw, &m);

    while (idx >= 0) {
        int n0 = 0, n1 = 0, sum, diff;

        domlist_varset_remove(left, m, idx, nw);
        for (int i = 0; i < view->count; i++) {
            const FitPTerm *pt = view->items[i];
            unsigned b = pt->bits[idx];
            unsigned c = pt->care[idx];

            if ((m & b & c) != 0)
                n1++;
            else if ((m & ~(b | c)) != 0)
                n0++;
        }
        sum = n0 + n1;
        diff = n0 < n1 ? n1 - n0 : n0 - n1;
        if (best_sum < sum || (sum == best_sum && diff < best_diff)) {
            best_diff = diff;
            *mask = m;
            best_sum = sum;
            best_idx = idx;
        }
        idx = domlist_varset_last(left, nw, &m);
    }
    free(left);
    return best_idx;
}

/* do the terms cover everything, looking only at the variables
 * of the set `vs` (which is consumed)?
 *  - a term with a single variable of the set: keep the terms compatible
 *    with the other form of that variable and drop the variable; no term
 *    left: no; a term without variables left, or no variable left: yes;
 *  - variables that occur in one form only: keep the terms without them;
 *    fewer than four terms left, or no variable left: no;
 *  - otherwise split on domlist_best_var and check both halves, the one
 *    with fewer terms first. */
static int domlist_tautology(void **items, int count, int nw, unsigned *vs)
{
    FitList *out = list_create_sized(count);
    DomView view;
    unsigned ubits = 0, ucare = 0;
    int idx = 0;
    int result;

    view.count = count;
    view.items = items;

    while (domlist_find_univar(&view, vs, nw, &ubits, &ucare, &idx)) {
        domlist_select(&view, &ubits, &ucare, -idx, out, 0);
        if (out->count == 0) {
            list_destroy(&out);
            return 0;
        }
        if (!domlist_varset_remove(vs, ~(ubits ^ ucare), idx, nw) ||
            domlist_has_free_term(out->items, out->count, vs, nw)) {
            list_destroy(&out);
            return 1;
        }
        view.count = out->count;
        view.items = out->items;
        out->count = 0;
    }

    for (;;) {
        FitPTerm *forms = domlist_mono_forms(&view, nw, vs);

        if (!domlist_pterm_has_var_in(forms, vs, nw)) {
            FitList *zero, *first, *second;
            unsigned *vs2;

            pterm_free(&forms);
            idx = domlist_best_var(&view, vs, nw, &ubits);
            zero = list_create_sized(view.count);
            ucare = 0xffffffffu;
            domlist_select(&view, &ubits, &ucare, -idx, zero, 0);
            domlist_select(&view, &ubits, &ucare, -idx, out, 0xffffffffu);
            domlist_varset_remove(vs, ~(ubits ^ ucare), idx, nw);
            first = zero;
            second = out;
            if (out->count < zero->count) {
                first = out;
                second = zero;
            }
            vs2 = bitvec_copy_new(vs, nw);
            if (domlist_tautology(first->items, first->count, nw, vs) == 0 ||
                domlist_tautology(second->items, second->count, nw, vs2) == 0)
                result = 0;
            else
                result = 1;
            list_destroy(&second);
            list_destroy(&first);
            free(vs2);
            return result;
        }
        if (!domlist_varset_remove_pterm(vs, forms, nw)) {
            pterm_free(&forms);
            break;
        }
        domlist_select(&view, forms->bits, forms->care, nw, out, 0);
        pterm_free(&forms);
        if (out->count < 4)
            break;
        view.count = out->count;
        view.items = out->items;
        out->count = 0;
    }
    list_destroy(&out);
    return 0;
}

/* 1 when the term `pt` is covered by the terms of the list
 * (which may contain pt's own slot only if it is not pt) */
static int domlist_includes(void **items, int count, FitPTerm *pt, int nw)
{
    FitList *sel = list_create_sized(count);
    DomView view;
    int result;

    view.count = count;
    view.items = items;
    domlist_select(&view, pt->bits, pt->care, nw, sel, 0xffffffffu);
    if (sel->count == 0) {
        result = 0;
    } else {
        unsigned *vs = bitvec_new_zero(nw);

        for (int i = 0; i < sel->count; i++)
            domlist_varset_add(vs, sel->items[i], nw);
        if (!domlist_varset_remove_pterm(vs, pt, nw))
            result = 1;
        else if (domlist_has_free_term(sel->items, sel->count, vs, nw))
            result = 1;
        else
            result = domlist_tautology(sel->items, sel->count, nw, vs);
        free(vs);
    }
    list_destroy(&sel);
    return result;
}

/* copy of a term with every literal removed that can go: the
 * variables are tried from the highest word down, lowest bit first; a
 * literal is dropped when the term with that literal inverted is covered by
 * the list. */
static FitPTerm *domlist_expand_pterm(FitPTerm *pt, void **items, int count, int nw)
{
    FitPTerm *c = pterm_copy(pt, nw);
    unsigned *vs = bitvec_new_zero(nw);
    unsigned m;
    int idx;

    domlist_varset_add(vs, c, nw);
    idx = domlist_varset_last(vs, nw, &m);
    while (idx >= 0) {
        domlist_varset_remove(vs, m, idx, nw);
        c->bits[idx] ^= m;
        c->care[idx] ^= m;
        if (domlist_includes(items, count, c, nw) == 0) {
            c->bits[idx] ^= m;
            c->care[idx] ^= m;
        } else {
            c->bits[idx] &= ~m;
            c->care[idx] |= m;
        }
        idx = domlist_varset_last(vs, nw, &m);
    }
    free(vs);
    return c;
}

/* remove, from the last term down to position `start`, every
 * term covered by the others.  A term that stays is taken out and appended
 * again, so the list ends up rotated (ListDelInsert moves the last term
 * into the hole). */
static void domlist_remove_redundant(FitList *l, int nw, int start, int free_terms)
{
    int n = min_count(l);

    for (int i = n - 1; i >= start; i--, n--) {
        FitPTerm *pt = l->items[i];

        list_del_insert(l, n);
        if (domlist_includes(l->items, l->count, pt, nw) == 0) {
            l->items[l->count] = pt;
            l->count++;
        } else if (free_terms) {
            pterm_free(&pt);
        }
    }
}

/* FitListEq3 for list_find_x: the key term against a list term */
static int domlist_covers_cb(void *item, void *key, void *nw)
{
    return pterm_covers(key, item, FIT_PTR2INT(nw));
}

/* consensus of two terms: defined when they conflict in exactly
 * one variable; that variable is dropped from the product.  NULL
 * otherwise. */
static FitPTerm *domlist_consensus(const FitPTerm *a, const FitPTerm *b, int nw)
{
    FitPTerm *r = pterm_new_raw(nw);
    int found = 0;

    for (int k = 0; k < nw; k++) {
        unsigned t;

        r->bits[k] = b->bits[k] | a->bits[k];
        r->care[k] = a->care[k] & b->care[k];
        t = ~r->care[k] & r->bits[k];
        if (t != 0) {
            unsigned c = r->care[k];

            if (found || (t & (t - 1u)) != 0) {
                found = 0;
                break;
            }
            r->care[k] = r->bits[k] | c;
            r->bits[k] = r->bits[k] & c;
            found = 1;
        }
    }
    if (!found)
        pterm_free(&r);
    return r;
}

/* the term has the literal !var */
static int domlist_pterm_var_is_zero(const FitPTerm *pt, int var)
{
    return !bitvec_test(pt->bits, var) && !bitvec_test(pt->care, var);
}

/* never seen running: for every variable, from the last to the
 * first, add the expanded consensus of each pair of terms that have the
 * variable in opposite forms, unless a term without the variable already
 * covers it. */
static void domlist_add_consensus(FitList *l, unsigned n_vars)
{
    int nw = min_words(n_vars);
    int view_count = min_count(l);

    for (unsigned var = n_vars; var != 0; var--) {
        FitList *off = list_create_sized(min_count(l));
        FitList *on = list_create_sized(min_count(l));
        FitList *dc = list_create_sized(min_count(l));
        int n_on;

        for (int i = 0, n = min_count(l); i < n; i++) {
            FitPTerm *pt = l->items[i];
            FitList *dst;

            /* the term has the literal var */
            if (bitvec_test(pt->bits, (int)var))
                dst = on;
            else if (domlist_pterm_var_is_zero(pt, (int)var))
                dst = off;
            else
                dst = dc;
            dst->items[dst->count] = pt;
            dst->count++;
        }
        n_on = on->count;
        for (int i = 0; i < n_on; i++) {
            int n_off = off->count;

            for (int j = 0; j < n_off; j++) {
                FitPTerm *c = domlist_consensus(off->items[j], on->items[i], nw);

                if (c == NULL)
                    continue;
                if (list_find_x(dc, c, domlist_covers_cb, FIT_INT2PTR(nw)) == 0) {
                    /* the context is the list as it was at entry */
                    FitPTerm *e = domlist_expand_pterm(c, l->items, view_count, nw);

                    list_add(l, e);
                    list_add(dc, l->items[l->count - 1]);
                }
                pterm_free(&c);
            }
        }
        list_destroy(&dc);
        list_destroy(&on);
        list_destroy(&off);
    }
}

FitList *domlist_minimise_pterms(FitList *pterms, FitList *fixed, unsigned n_vars,
                                 unsigned flags)
{
    FitList *l;
    int n_fixed = 0;
    int nw;

    if (fixed == NULL) {
        l = list_copy(pterms);
    } else {
        n_fixed = fixed->count;
        l = list_copy(fixed);
        list_append_mapped(l, pterms, list_identity);
    }
    nw = min_words(n_vars);
    if (l == NULL)              /* pterms NULL without fixed terms */
        return NULL;

    for (int p = n_fixed; p < l->count; p++) {
        int n = l->count;

        l->items[p] = domlist_expand_pterm(l->items[p], l->items, l->count, nw);
        for (int q = p + 1; q < n; q++) {
            if (pterm_covers(l->items[q], l->items[p], nw))
                l->items[q] = NULL;
        }
        list_compact(l);
    }
    if ((flags | 0xfffffff5u) != 0xffffffffu)
        domlist_add_consensus(l, n_vars);
    if ((flags | 0xfffffffbu) != 0xffffffffu)
        domlist_remove_redundant(l, nw, n_fixed, 1);
    for (int i = 0; i < n_fixed; i++)
        l->items[i] = NULL;
    list_compact(l);
    return l;
}

FitFctTree *domlist_minimise_tree(FitEntity *entity, FitFctTree *tree)
{
    FitList *vars, *pterms, *minimal;
    FitFctTree *result;

    if (tree == NULL)
        return NULL;
    vars = domlist_tree_vars(entity, tree);
    pterms = fcttree_to_pterms3(tree, NULL, vars, NULL);
    minimal = domlist_minimise_pterms(pterms, NULL, (unsigned)min_count(vars), 0xfffffffbu);
    list_free_all(&pterms, min_free_pterm_cb);
    result = fcttree_from_pterms(minimal, vars);
    list_free_all(&minimal, min_free_pterm_cb);
    list_destroy(&vars);
    return result;
}

/* minimise tree[0], tree[2] and tree[1] (in this order) of every
 * function that is not just a literal */
static void domlist_minimise_fcts(FitEntity *e)
{
    static const int order[3] = { 0, 2, 1 };

    if (e == NULL)
        return;
    for (int i = 0; i < min_count(e->fcts); i++) {
        FitFct *f = e->fcts->items[i];

        for (int k = 0; k < 3; k++) {
            FitFctTree **slot = &f->tree[order[k]];

            if (*slot != NULL && !domlist_tree_is_literal(*slot)) {
                FitFctTree *t = domlist_minimise_tree(e, *slot);

                fcttree_free(slot);
                *slot = t;
            }
        }
    }
}

FitEntity *domlist_minimise_entity(FitEntity *entity, int use_local, int unused, int depth)
{
    FitEntity *e = entity_copy_inv_propagated(entity);

    (void)unused;
    flatten_entity(&e, depth);
    domlist_minimise_fcts(e);
    tt2_compact_entity(e);
    if (input_entity_has_dc(e) != 0 || use_local != 1)
        printf("La on aurait utilise la min globale\n");
    return e;
}

/* one step of domlist_same_forms */
static void domlist_same_forms_step(FitPTerm *f, const FitPTerm *pt, int nw)
{
    for (int k = 0; k < nw; k++) {
        unsigned b = f->bits[k];

        f->bits[k] = (pt->care[k] ^ f->care[k]) | pt->bits[k] | b;
        f->care[k] = f->care[k] & ~(pt->bits[k] ^ b) & pt->care[k];
    }
}

/* the literals common to all terms (in the same form),
 * restricted to the set */
static FitPTerm *domlist_same_forms(const DomView *view, int nw, const unsigned *vs)
{
    FitPTerm *f = pterm_copy(view->items[0], nw);

    for (int i = 0; i < view->count; i++)
        domlist_same_forms_step(f, view->items[i], nw);
    for (int k = 0; k < nw; k++) {
        f->bits[k] &= vs[k];
        f->care[k] |= ~vs[k];
    }
    return f;
}

/* never seen running; called from factor_kernel_loop (synfactor.c) and
 * from domlist_merge_covers.  Returns a new list of new terms; `vs` is
 * consumed. */
FitList *domlist_factor_cover(FitList *pterms, int n_words, unsigned *varset)
{
    int nw = n_words;
    unsigned *vs = varset;
    FitList *out = list_create_sized(min_count(pterms));
    FitPTerm *lits = NULL;      /* the literals of the single-variable terms met so far */
    DomView view;
    unsigned ubits = 0, ucare = 0;
    int idx = 0;
    int covered = 0;
    FitList *result;

    view.count = min_count(pterms);
    view.items = pterms != NULL ? pterms->items : NULL;

    while (domlist_find_univar(&view, vs, nw, &ubits, &ucare, &idx)) {
        if (lits == NULL)
            lits = pterm_new(nw);
        if (ubits == 0) {
            lits->bits[idx] |= ~ucare;
            lits->care[idx] |= ~ucare;
        } else {
            lits->bits[idx] &= ~ubits;
            lits->care[idx] &= ~ubits;
        }
        domlist_select(&view, &ubits, &ucare, -idx, out, 0);
        if (out->count == 0)
            goto single;
        if (!domlist_varset_remove(vs, ~(ubits ^ ucare), idx, nw) ||
            domlist_has_free_term(out->items, out->count, vs, nw)) {
            covered = 1;
            goto single;
        }
        view.count = out->count;
        view.items = out->items;
        out->count = 0;
    }

    {
        FitPTerm *forms;
        FitList *common;

        if (view.count == 0) {  /* empty list: a new blank term */
            forms = pterm_new(nw);
        } else {
            forms = domlist_same_forms(&view, nw, vs);
        }
        common = pterm_complement(forms, nw);
        domlist_varset_remove_pterm(vs, forms, nw);
        pterm_free(&forms);

        if (!domlist_has_free_term(view.items, view.count, vs, nw)) {
            FitList *zero = list_create_sized(view.count);
            FitList *r0, *r1;
            unsigned *vs2;

            idx = domlist_best_var(&view, vs, nw, &ubits);
            ucare = 0xffffffffu;
            domlist_select(&view, &ubits, &ucare, -idx, zero, 0);
            domlist_select(&view, &ubits, &ucare, -idx, out, 0xffffffffu);
            domlist_varset_remove(vs, ~(ubits ^ ucare), idx, nw);
            vs2 = bitvec_copy_new(vs, nw);
            r0 = domlist_factor_cover(zero, nw, vs2);
            free(vs2);
            r1 = domlist_factor_cover(out, nw, vs);
            list_destroy(&out);
            list_destroy(&zero);
            if (min_count(r0) == 0 && min_count(r1) == 0) {
                list_destroy(&r1);
            } else {
                int n0 = min_count(r0);

                /* terms found in both halves do not depend on the variable */
                for (int i = 0; i < n0; i++) {
                    FitPTerm *p = r0->items[i];
                    int dup = 0;
                    int pos;

                    while ((pos = list_find_x(r1, p, pterm_equal, FIT_INT2PTR(nw))) > 0) {
                        FitPTerm *twin = r1->items[pos - 1];

                        list_del_insert(r1, pos);
                        pterm_free(&twin);
                        dup++;
                    }
                    if (dup != 0) {
                        if (dup > 1)
                            fprintf(g_msg, "Replicates %d.\n", dup);
                        list_add(common, p);
                        r0->items[i] = NULL;
                    }
                }
                list_compact(r0);
                list_compact(r1);
                for (int i = 0; i < min_count(r0); i++) {
                    FitPTerm *p = r0->items[i];

                    p->bits[idx] &= ~ubits;
                    p->care[idx] &= ~ubits;
                }
                for (int i = 0; i < min_count(r1); i++) {
                    FitPTerm *p = r1->items[i];

                    p->bits[idx] |= ubits;
                    p->care[idx] |= ubits;
                }
                list_append_destroy(r0, &r1);
            }
            list_append_destroy(r0, &common);
            result = r0;
        } else {
            result = common;
            list_destroy(&out);
        }
        if (lits != NULL) {
            for (int i = 0; i < min_count(result); i++)
                pterm_and_into(result->items[i], lits, nw);
            pterm_free(&lits);
        }
        return result;
    }

single:
    list_destroy(&out);
    result = list_create_sized(1);
    if (covered) {
        pterm_free(&lits);
    } else {
        result->items[result->count] = lits;
        result->count++;
    }
    return result;
}

/* never seen running; called from tt2_convert_pla_type (tt_tt2.c) */
FitList *domlist_merge_covers(FitList *a, FitList *b, int n_vars)
{
    int nw = min_words((unsigned)n_vars);
    FitList *all, *result;
    unsigned *vs;

    if (min_count(a) == 0 && min_count(b) == 0) {
        result = list_create_sized(1);
        list_add(result, pterm_new(nw));
        return result;
    }
    all = list_create_sized(min_count(a) + min_count(b));
    list_append_mapped(all, a, list_identity);
    list_append_mapped(all, b, list_identity);
    vs = bitvec_new_zero(nw);
    for (int i = 0; i < all->count; i++)
        domlist_varset_add(vs, all->items[i], nw);
    result = domlist_factor_cover(all, nw, vs);
    list_destroy(&all);
    free(vs);
    return result;
}
