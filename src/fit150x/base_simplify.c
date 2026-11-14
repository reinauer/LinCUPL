/*
 * base_simplify.c - entity_simplified_copy: a copy of an entity in which
 * the functions that are only another signal (or its complement), a
 * constant, or a duplicate of another function have been removed and their
 * uses redirected.  Called from the library stage, the fit preparation and
 * the XOR detection.
 *
 * Signals whose id is on g_keep_ids (set up by main from the foldback,
 * global_fold and soft_buffer options) are never removed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base_int.h"
#include "lib.h"
#include "tt.h"

FitList *g_keep_ids;

static int child_count(const FitFctTree *t)
{
    return t->u.children == NULL ? 0 : t->u.children->count;
}

/* structural equality of two trees (operands in the same order) */
static unsigned fcttree_equal(FitFctTree *a, FitFctTree *b)
{
    unsigned same = 1;
    int n;

    if (a->sel != b->sel)
        return 0;
    if (a->sel == FIT_SEL_ID || a->sel == FIT_SEL_CONST)
        return a->u.id == b->u.id;
    n = child_count(a);
    if (n != child_count(b))
        return 0;
    for (int i = 0; i < n && same != 0; i++)
        same = fcttree_equal(a->u.children->items[i], b->u.children->items[i]);
    return same;
}

/* is f "local" or "!local"?  *target = the function defining
 * that local, *inv = 1 for the complement. */
static int fct_is_local_alias(FitEntity *e, FitFct *f, FitFct **target, int *inv)
{
    FitFctTree *t = f->tree[0];

    if (t->sel == FIT_SEL_ID && entity_is_local(e, t->u.id) != 0) {
        *target = entity_find_fct(e, t->u.id);
        if (*target == NULL)
            return 0;
        *inv = 0;
        return 1;
    }
    if (t->sel == FIT_SEL_NOT) {
        FitFctTree *c = t->u.children->items[0];

        if (c->sel == FIT_SEL_ID && entity_is_local(e, c->u.id) != 0) {
            *target = entity_find_fct(e, c->u.id);
            if (*target == NULL)
                return 0;
            *inv = 1;
            return 1;
        }
    }
    *target = NULL;
    return 0;
}

/* is f an input or output, or the complement of one? */
static int fct_is_io_alias(FitEntity *e, FitFct *f, int *id, int *inv)
{
    FitFctTree *t = f->tree[0];
    FitFctTree *c;

    if (t->sel == FIT_SEL_ID && (entity_is_input(e, t->u.id) != 0 || entity_is_output(e, t->u.id) != 0)) {
        *id = t->u.id;
        *inv = 0;
        return 1;
    }
    if (t->sel != FIT_SEL_NOT)
        return 0;
    c = t->u.children->items[0];
    if (c->sel != FIT_SEL_ID)
        return 0;
    if (entity_is_input(e, c->u.id) == 0 && entity_is_output(e, c->u.id) == 0)
        return 0;
    *id = c->u.id;
    *inv = 1;
    return 1;
}

/* is f a constant or the complement of one? */
static int fct_is_const(FitFct *f, int *value, int *inv)
{
    FitFctTree *t = f->tree[0];

    if (t->sel == FIT_SEL_CONST) {
        *value = t->u.value;
        *inv = 0;
        return 1;
    }
    if (t->sel == FIT_SEL_NOT) {
        FitFctTree *c = t->u.children->items[0];

        if (c->sel == FIT_SEL_CONST) {
            *value = c->u.value;
            *inv = 1;
            return 1;
        }
    }
    return 0;
}

/* f takes over the tree of target (shared), or its complement
 * with the NOT pushed down */
static void fct_take_tree(FitFct *f, FitFct *target, int inv)
{
    FitFctTree *neg;

    if (inv == 0) {
        f->tree[0] = target->tree[0];
        return;
    }
    neg = fcttree_new();
    neg->u.children = list_create();
    neg->sel = FIT_SEL_NOT;
    list_add(neg->u.children, target->tree[0]);
    neg->user = util_calloc(1, 2 * sizeof(void *));
    f->tree[0] = fcttree_propagate_inv(neg, 0);
}

/* every leaf `id` becomes "!x", x being a new leaf with selector
 * new_sel and value new_value.  Returns the number of replacements. */
static int fcttree_replace_id_inv(FitFctTree *t, int id, int new_value, int new_sel)
{
    int n = 0;

    if (t->sel == FIT_SEL_ID) {
        if (t->u.id == id) {
            FitFctTree *leaf = fcttree_new();
            void **back;

            leaf->sel = new_sel;
            leaf->u.id = new_value;
            back = util_calloc(1, 2 * sizeof(void *));
            back[0] = t;
            leaf->user = back;
            t->u.children = list_create();
            t->sel = FIT_SEL_NOT;
            list_add(t->u.children, leaf);
            return 1;
        }
    } else if (t->sel != FIT_SEL_CONST) {
        for (int i = 0, count = child_count(t); i < count; i++)
            n += fcttree_replace_id_inv(t->u.children->items[i], id, new_value, new_sel);
    }
    return n;
}

/* every leaf `id` becomes a leaf with selector new_sel and value
 * new_value.  Returns the number of replacements. */
int fcttree_replace_id(FitFctTree *t, int id, int new_value, int new_sel)
{
    int n = 0;

    if (t->sel == FIT_SEL_ID) {
        if (t->u.id == id) {
            t->u.id = new_value;
            t->sel = new_sel;
            return 1;
        }
    } else if (t->sel != FIT_SEL_CONST) {
        for (int i = 0, count = child_count(t); i < count; i++)
            n += fcttree_replace_id(t->u.children->items[i], id, new_value, new_sel);
    }
    return n;
}

static void list_destroy_cb(void **l)
{
    FitList *p = *l;

    list_destroy(&p);
    *l = p;
}

static void var_free_cb(void **v)
{
    FitVariable *p = *v;

    var_free(&p);
    *v = p;
}

/* delete the locals no function uses any more, with their
 * functions, until nothing changes. */
static void entity_drop_unused_locals(FitEntity *e)
{
    FitList *uses = list_create_sized(e->fcts == NULL ? 0 : e->fcts->count);
    int removed;

    do {
        int li = 0, lpos = 1;

        removed = 0;
        if (e->locals == NULL || e->locals->count < 1)
            break;
        do {
            int used = 0, deleted = 0;
            int id = ((FitVariable *)e->locals->items[li])->id;

            if (uses == NULL || uses->count == 0) {
                for (int i = 0; i < (e->fcts == NULL ? 0 : e->fcts->count); i++) {
                    FitList *ids = list_create();
                    FitFct *f = e->fcts->items[i];

                    if (f->tree[0] != NULL)
                        fcttree_collect_ids(f->tree[0], ids);
                    list_add(uses, ids);
                }
            }
            for (int i = 0; i < (e->fcts == NULL ? 0 : e->fcts->count) && !used; i++) {
                if (list_find(uses->items[i], FIT_INT2PTR(id), list_ptr_eq) != 0)
                    used = 1;
            }
            if (!used && e->fcts != NULL && e->fcts->count > 0) {
                int k = 1, more;

                /* after a deletion the function that moved into slot k is
                 * not looked at: k advances all the same */
                do {
                    FitFct *f = e->fcts->items[k - 1];

                    if (f->var_id == id) {
                        deleted = 1;
                        list_del_shift(e->fcts, k);
                        list_del_shift_free(uses, k, list_destroy_cb);
                        free(f);
                    }
                    more = k < e->fcts->count;
                    k++;
                } while (more);
                if (deleted) {
                    list_del_shift_free(e->locals, lpos, var_free_cb);
                    removed++;
                    li--;
                    lpos--;
                }
            }
            li++;
            lpos++;
        } while (li < (e->locals == NULL ? 0 : e->locals->count));
    } while (removed != 0);
    if (uses != NULL)
        list_free_all(&uses, list_destroy_cb);
}

/* replace the signal of `victim` by the signal of `keep`
 * (complemented if inv) in every function and in the netlist, then remove
 * victim and its variable. */
static void entity_merge_fct(FitEntity *e, FitFct *victim, FitFct *keep, int inv)
{
    int vid = victim->var_id;
    int cls = 0;
    char *old_name, *new_name;

    for (int i = 0; i < (e->fcts == NULL ? 0 : e->fcts->count); i++) {
        FitFct *g = e->fcts->items[i];
        FitFctTree *tree = g->tree[0] != NULL ? g->tree[0] : g->tree[1];

        if (tree == NULL)
            continue;
        if (inv == 0)
            fcttree_replace_id(tree, vid, keep->var_id, FIT_SEL_ID);
        else
            fcttree_replace_id_inv(tree, vid, keep->var_id, FIT_SEL_ID);
    }
    old_name = util_strdup_or_null(lib_var_name_by_id(victim->var_id, e, &cls));
    new_name = util_strdup_or_null(lib_var_name_by_id(keep->var_id, e, &cls));
    if (old_name != NULL && new_name != NULL)
        tt2_rename_net_signal(e, old_name, new_name, 1);
    free(old_name);
    free(new_name);
    entity_remove_fct(e, victim);
}

static int trees_equal(FitFct *f, FitFct *g)
{
    if (f->tree[0] != NULL && g->tree[0] != NULL && fcttree_equal(f->tree[0], g->tree[0]) != 0)
        return 1;
    if (f->tree[1] != NULL && g->tree[1] != NULL && fcttree_equal(f->tree[1], g->tree[1]) != 0)
        return 1;
    return 0;
}

/* one pass merging functions with identical trees.  Only
 * functions of fewer than four variables are compared with the functions
 * after them; of two equal ones a removable local is the one to go. */
static void entity_merge_equal_pass(FitEntity *e, int *merged)
{
    for (int i = 0; i < e->fcts->count; i++) {
        FitFct *f = e->fcts->items[i];
        int keep_f = list_find(g_keep_ids, FIT_INT2PTR(f->var_id), list_ptr_eq);
        FitList *ids = list_create();
        int few;

        if (f->tree[0] != NULL)
            fcttree_collect_ids(f->tree[0], ids);
        few = ids == NULL || ids->count < 4;
        list_destroy(&ids);
        if (!few)
            continue;
        for (int j = i + 1; j < e->fcts->count; j++) {
            FitFct *g = e->fcts->items[j];
            int keep_g = list_find(g_keep_ids, FIT_INT2PTR(g->var_id), list_ptr_eq);

            if (entity_is_local(e, g->var_id) == 0 || keep_g != 0) {
                if (entity_is_local(e, f->var_id) != 0 && keep_f == 0 && trees_equal(f, g)) {
                    entity_merge_fct(e, f, g, 0);
                    (*merged)++;
                    f = e->fcts->items[i];
                    keep_f = list_find(g_keep_ids, FIT_INT2PTR(f->var_id), list_ptr_eq);
                    j = i;
                }
            } else if (trees_equal(f, g)) {
                entity_merge_fct(e, g, f, 0);
                (*merged)++;
                j--;
            }
        }
    }
}

/* entity_merge_equal_pass until a pass merges nothing */
static void entity_merge_equal(FitEntity *e)
{
    int merged;

    do {
        merged = 0;
        entity_merge_equal_pass(e, &merged);
    } while (merged != 0);
}

/* f is "id" or "!id" of an input or output: use that signal
 * wherever f's signal is used */
static void entity_replace_by_id(FitEntity *e, FitFct *f, int id, int inv)
{
    for (int i = 0; i < e->fcts->count; i++) {
        FitFct *g = e->fcts->items[i];

        if (inv == 0)
            fcttree_replace_id(g->tree[0], f->var_id, id, FIT_SEL_ID);
        else
            fcttree_replace_id_inv(g->tree[0], f->var_id, id, FIT_SEL_ID);
    }
}

/* f is a constant: put the constant wherever f's signal is used
 * and fold the constants of the functions that changed */
static void entity_replace_by_const(FitEntity *e, FitFct *f, int value, int inv)
{
    for (int i = 0; i < e->fcts->count; i++) {
        FitFct *g = e->fcts->items[i];
        int n;

        if (inv == 0)
            n = fcttree_replace_id(g->tree[0], f->var_id, value, FIT_SEL_CONST);
        else
            n = fcttree_replace_id_inv(g->tree[0], f->var_id, value, FIT_SEL_CONST);
        if (n != 0)
            flatten_fold_tree(g->tree[0]);
    }
}

typedef struct DepthPair {
    int index;      /* position of the function in e->fcts */
    int depth;
} DepthPair;

/* list comparator on DepthPair.depth */
static int depth_pair_cmp(void *a, void *b)
{
    int x = ((DepthPair *)a)->depth;
    int y = ((DepthPair *)b)->depth;

    if (x == y)
        return 0;
    return x >= y ? 1 : -1;
}

/* (index, logic depth) of every function, sorted by depth */
static FitList *entity_fct_depths(FitEntity *e)
{
    FitList *out = list_create_sized(e->fcts == NULL ? 0 : e->fcts->count);

    for (int i = 0; i < (e->fcts == NULL ? 0 : e->fcts->count); i++) {
        DepthPair *p;
        int depth = 1;

        fcttree_depth(e, ((FitFct *)e->fcts->items[i])->tree[0], &depth);
        p = util_malloc(sizeof(*p));
        p->index = i;
        p->depth = depth;
        list_add(out, p);
    }
    list_sort(out, 1, depth_pair_cmp);
    return out;
}

/* reorder e->fcts by increasing logic depth */
static void entity_sort_fcts_by_depth(FitEntity *e)
{
    FitList *sorted = list_create_sized(e->fcts == NULL ? 0 : e->fcts->count);
    FitList *order = entity_fct_depths(e);

    for (int i = 0; i < (order == NULL ? 0 : order->count); i++)
        list_add(sorted, e->fcts->items[((DepthPair *)order->items[i])->index]);
    list_destroy(&e->fcts);
    e->fcts = sorted;
    list_free_all(&order, util_free_ptr);
}

/* remove the functions that are just another signal, its
 * complement or a constant.  *count = number of changes. */
static void entity_remove_aliases(FitEntity *e, int *count)
{
    int i = 0;

    entity_sort_fcts_by_depth(e);
    *count = 0;
    for (;;) {
        FitFct *f, *target;
        int id, inv;

        if (i >= (e->fcts == NULL ? 0 : e->fcts->count)) {
            e->n_local = e->locals == NULL ? 0 : e->locals->count;
            return;
        }
        f = e->fcts->items[i];
        if (fct_is_local_alias(e, f, &target, &inv) != 0) {
            if (list_find(g_keep_ids, FIT_INT2PTR(target->var_id), list_ptr_eq) == 0) {
                /* the local disappears: f gets its equation */
                fct_take_tree(f, target, inv);
                entity_merge_fct(e, target, f, inv);
                (*count)++;
                continue;
            }
            if (list_find(g_keep_ids, FIT_INT2PTR(f->var_id), list_ptr_eq) == 0 &&
                entity_is_local(e, f->var_id) != 0) {
                /* the local has to stay: f disappears if it is a local itself */
                entity_merge_fct(e, f, target, inv);
                continue;
            }
        } else if (fct_is_io_alias(e, f, &id, &inv) != 0) {
            if (list_find(g_keep_ids, FIT_INT2PTR(f->var_id), list_ptr_eq) == 0) {
                entity_replace_by_id(e, f, id, inv);
                (*count)++;
            }
        } else if (fct_is_const(f, &id, &inv) != 0) {
            entity_replace_by_const(e, f, id, inv);
            (*count)++;
        }
        i++;
    }
}

/* copy with the NOTs pushed down, constants folded, aliases of other
 * signals removed, equal functions merged (below 1000 functions) and unused
 * locals dropped */
FitEntity *entity_simplified_copy(FitEntity *e)
{
    FitEntity *c;
    int changes;

    e->n_local = e->locals == NULL ? 0 : e->locals->count;
    c = entity_copy_inv_propagated(e);
    flatten_fold_entity(c);
    tt2_compact_entity(c);
    entity_remove_aliases(c, &changes);
    flatten_fold_entity(c);
    if (c->fcts == NULL || c->fcts->count < 1000)
        entity_merge_equal(c);
    entity_drop_unused_locals(c);
    c->n_local = c->locals == NULL ? 0 : c->locals->count;
    return c;
}
