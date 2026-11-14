/*
 * base_fcttree.c - the Boolean expression tree of the ATF150x fitter:
 * create, copy, free, conversion to product terms, cycle and depth
 * analysis, compaction.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base_int.h"
#include "min.h"
#include "tt.h"

FitList *g_depth_visited;       /* ids being visited by fcttree_depth_rec */

static int child_count(const FitFctTree *t)
{
    return t->u.children == NULL ? 0 : t->u.children->count;
}

static int has_children(int sel)
{
    return sel == FIT_SEL_AND || sel == FIT_SEL_OR || sel == FIT_SEL_NOT || sel == FIT_SEL_EQ ||
           sel == FIT_SEL_XOR;
}

FitFctTree *fcttree_new(void)
{
    FitFctTree *t = util_calloc(1, sizeof(*t));

    t->sel = FIT_SEL_EMPTY;
    t->u.children = NULL;
    t->user = NULL;
    return t;
}

/* append the items of *src to dst (src stays as it is) */
static void fcttree_append_children(FitList *dst, FitList **src)
{
    if (*src != NULL) {
        int count = (*src)->count;

        for (int i = 0; i < count; i++)
            list_add(dst, (*src)->items[i]);
    }
}

static void fcttree_free_deferred_cb(void **slot)
{
    FitFctTree *t = *slot;

    fcttree_free_deferred(&t);
}

/* release a tree.  The nodes are not freed here: each one is
 * marked (selector 0) and queued on g_tree_gc, so that a node reachable
 * twice is queued once; fcttree_gc_end frees the queue.  *t keeps its
 * value. */
void fcttree_free_deferred(FitFctTree **t)
{
    FitFctTree *n = *t;

    if (n == NULL)
        return;
    if (has_children(n->sel))
        list_free_all(&n->u.children, fcttree_free_deferred_cb);
    if (n->sel != 0) {
        n->sel = 0;
        if (g_tree_gc != NULL)
            list_add(g_tree_gc, n);
    }
}

/* frees a tree at once (fcttree_free_deferred inside its own collection);
 * *t = NULL */
void fcttree_free(FitFctTree **t)
{
    fcttree_gc_begin();
    fcttree_free_deferred(t);
    fcttree_gc_end();
    *t = NULL;
}

struct copy_ctx {
    FitListCopy user_copy;
};

static void *fcttree_copy_cb(void *item, void *extra)
{
    return fcttree_copy(item, ((struct copy_ctx *)extra)->user_copy);
}

/* deep copy of a tree; the items of a FIT_SEL_SET node are shared.
 * user_copy (may be NULL) is only passed down.  NULL for NULL. */
FitFctTree *fcttree_copy(FitFctTree *t, FitListCopy user_copy)
{
    FitFctTree *c;
    struct copy_ctx ctx;

    if (t == NULL)
        return NULL;
    c = fcttree_new();
    c->sel = t->sel;
    switch (t->sel) {
    case FIT_SEL_NOT:
    case FIT_SEL_AND:
    case FIT_SEL_OR:
    case FIT_SEL_EQ:
    case FIT_SEL_XOR:
        ctx.user_copy = user_copy;
        c->u.children = list_map_copy_x(t->u.children, fcttree_copy_cb, &ctx);
        break;
    case FIT_SEL_CONST:
        c->u.value = t->u.value;
        break;
    case FIT_SEL_ID:
        c->u.id = t->u.id;
        break;
    case FIT_SEL_SET:
        c->u.children = list_map_copy(t->u.children, list_identity);
        break;
    default:
        fprintf(g_msg, "In FctTreeCopy, illegal selector - result may be wrong.\n");
        c->u.value = 1;
        break;
    }
    if (user_copy != NULL)
        c->user = user_copy(t->user);
    return c;
}

/* fcttree_to_pterms over the concatenation of three variable lists */
FitList *fcttree_to_pterms3(FitFctTree *t, FitList *v1, FitList *v2, FitList *v3)
{
    FitList *vars = list_create();
    FitList *out;

    list_append(vars, v1);
    list_append(vars, v2);
    list_append(vars, v3);
    out = fcttree_to_pterms(t, vars);
    list_destroy(&vars);
    return out;
}

static void pterm_free_cb(void **p)
{
    FitPTerm *q = *p;

    pterm_free(&q);
    *p = q;
}

static FitList *single_term(FitPTerm *p)
{
    FitList *out = list_create();

    list_add(out, p);
    return out;
}

/* the lowest bit of the highest nonzero word of v */
static int bitvec_last_set(const unsigned *v, int nwords, int *word, unsigned *mask)
{
    int i = nwords - 1;

    while (i >= 0 && v[i] == 0)
        i--;
    if (i >= 0) {
        *mask = v[i];
        if (v[i] != 0) {
            *word = i;
            *mask = ~(*mask - 1u) & *mask;
            return 1;
        }
    }
    return 0;
}

/* the complement of one product term as a list of terms with a
 * single literal each (De Morgan) */
FitList *pterm_complement(FitPTerm *p, int nwords)
{
    FitList *out = list_create();
    unsigned *lits = bitvec_xnor_new(p->bits, p->care, nwords);
    int word;
    unsigned mask;

    while (bitvec_last_set(lits, nwords, &word, &mask) != 0) {
        FitPTerm *q = pterm_new(nwords);

        list_add(out, q);
        if ((p->bits[word] & mask) == 0)
            q->bits[word] |= mask;
        else
            q->care[word] &= ~mask;
        lits[word] &= ~mask;
    }
    free(lits);
    return out;
}

/* the complement of a sum of products: the product of the
 * complements of its terms */
static FitList *pterm_list_complement(FitList *pterms, int nwords)
{
    FitList *acc = list_create_sized(1);

    list_add(acc, pterm_new(nwords));
    for (int i = 0; i < (pterms == NULL ? 0 : pterms->count); i++) {
        FitList *c = pterm_complement(pterms->items[i], nwords);
        FitList *prod = pterm_list_and(acc, c, nwords);

        list_free_all(&acc, pterm_free_cb);
        list_free_all(&c, pterm_free_cb);
        acc = prod;
    }
    return acc;
}

/* the tree as a sum of products over the
 * variables of `vars` (list of FitVariable; position n is bit n-1).  An
 * empty list is the constant 0, NULL an error. */
FitList *fcttree_to_pterms(FitFctTree *t, FitList *vars)
{
    unsigned n;
    int nwords;
    FitList *acc, *sub, *out;

    if (t == NULL)
        return NULL;
    n = vars == NULL ? 0 : (unsigned)vars->count;
    nwords = n == 0 ? 1 : (int)((n - 1) >> 5) + 1;
    switch (t->sel) {
    case FIT_SEL_NOT:
        sub = fcttree_to_pterms(t->u.children->items[0], vars);
        if (sub == NULL || sub->count == 0) {
            list_destroy(&sub);
            return single_term(pterm_new(nwords));
        }
        out = pterm_list_complement(sub, nwords);
        list_free_all(&sub, pterm_free_cb);
        return out;
    case FIT_SEL_AND:
        acc = fcttree_to_pterms(t->u.children->items[0], vars);
        if (acc == NULL)
            return NULL;
        if (acc->count == 0)
            return acc;
        for (int i = 1; i < child_count(t); i++) {
            FitList *prod;
            int na, ns, np;

            sub = fcttree_to_pterms(t->u.children->items[i], vars);
            na = acc == NULL ? 0 : acc->count;
            ns = sub == NULL ? 0 : sub->count;
            prod = pterm_list_and(acc, sub, nwords);
            np = prod == NULL ? 0 : prod->count;
            if ((ns + na) * 2 < np) {
                FitList *reduced = domlist_minimise_pterms(prod, NULL, n, 0xfffffffe);

                list_free_all(&prod, pterm_free_cb);
                prod = reduced;
            }
            list_free_all(&acc, pterm_free_cb);
            list_free_all(&sub, pterm_free_cb);
            acc = prod;
            if (acc == NULL)
                return NULL;
            if (acc->count == 0)
                return acc;
        }
        return acc;
    case FIT_SEL_OR:
        out = list_create();
        for (int i = 0; i < child_count(t); i++) {
            sub = fcttree_to_pterms(t->u.children->items[i], vars);
            if (sub != NULL && sub->count != 0)
                list_append_destroy(out, &sub);
            else
                list_destroy(&sub);
        }
        return out;
    case FIT_SEL_CONST:
        if (t->u.value == 0)
            return list_create_sized(0);
        if (t->u.value == 1)
            return single_term(pterm_new(nwords));
        fprintf(g_msg, "In FctTreeToLPTerm (Complex constant).\n");
        fprintf(g_msg, "Result of the command will be incorrect - delete it.\n");
        fprintf(g_msg, "Replace complex variables by 0/1 and run the command again.\n");
        return NULL;
    case 'F':
        out = single_term(pterm_new_zero(nwords));
        fprintf(g_msg, "In FctTreeToLPTerm (FALSE found).\n");
        fprintf(g_msg, "Result of the command will be correct.\n");
        return out;
    case FIT_SEL_ID: {
        int pos = var_list_pos_of_id(vars, t->u.id);
        FitPTerm *p;
        unsigned word, mask;

        if (pos == 0) {
            fprintf(g_msg, "In FctTreeToLPTerm (Id is neither OUT nor IN nor LOCAL).\n");
            fprintf(g_msg, "Result of the command will be incorrect - delete it.\n");
            return NULL;
        }
        p = pterm_new(nwords);
        word = ((unsigned)pos - 1u) >> 5;
        mask = 1u << (((unsigned)pos - 1u) & 0x1f);
        p->bits[word] |= mask;
        p->care[word] |= mask;
        return single_term(p);
    }
    case FIT_SEL_SET:
        fprintf(g_msg, "In FctTreeToLPTerm (SIDF found).\n");
        fprintf(g_msg, "Result of the command will be incorrect - delete it.\n");
        fprintf(g_msg, "Replace structured variables by simple ones and run the command again.\n");
        return NULL;
    case FIT_SEL_EMPTY:
        out = single_term(pterm_new(nwords));
        fprintf(g_msg, "In FctTreeToLPTerm (TRUE found).\n");
        fprintf(g_msg, "Result of the command will be correct.\n");
        return out;
    default:
        fprintf(g_msg, "Illegal Selector in FctTree\n");
        return NULL;
    }
}

/* adds each variable id of the tree to ids, once */
void fcttree_collect_ids(FitFctTree *t, FitList *ids)
{
    if (t->sel == FIT_SEL_ID) {
        if (list_contains_ptr(ids, FIT_INT2PTR(t->u.id)) == 0)
            list_add(ids, FIT_INT2PTR(t->u.id));
    } else if (t->sel != FIT_SEL_CONST) {
        int count = child_count(t);

        for (int i = 0; i < count; i++)
            fcttree_collect_ids(t->u.children->items[i], ids);
    }
}

/* number of distinct variable ids in the tree */
int fcttree_count_ids(FitFctTree *t)
{
    FitList *ids = list_create();
    int n;

    fcttree_collect_ids(t, ids);
    n = ids == NULL ? 0 : ids->count;
    list_destroy(&ids);
    return n;
}

/* logic depth of a tree.  depths is indexed by variable id and
 * memoises the depth of the function defining each output or local (-1 =
 * not computed yet); ids on g_depth_visited are treated as inputs. */
void fcttree_depth_rec(FitEntity *e, FitFctTree *t, FitList *depths, int *max, int level)
{
    if (*max < level)
        *max = level;
    if (t == NULL) {
        *max = 0;
        return;
    }
    if (t->sel == FIT_SEL_ID) {
        int id = t->u.id;
        int visiting = list_find(g_depth_visited, FIT_INT2PTR(id), list_ptr_eq);

        if ((entity_is_local(e, id) != 0 || entity_is_output(e, id) != 0) && visiting == 0) {
            int d;

            if (id < 0 || id >= depths->capacity)
                return;
            d = FIT_PTR2INT(depths->items[id]);
            if (d == -1) {
                int sub = 0;

                if (g_depth_visited != NULL)
                    list_add(g_depth_visited, FIT_INT2PTR(id));
                fcttree_depth_rec(e, entity_fct_tree(e, id), depths, &sub, 1);
                depths->items[id] = FIT_INT2PTR(sub);
                if (*max < sub + level)
                    *max = sub + level;
            } else if (*max < d + level) {
                *max = d + level;
            }
        }
    } else if (t->sel != FIT_SEL_CONST) {
        int count = child_count(t);

        for (int i = 0; i < count; i++)
            fcttree_depth_rec(e, t->u.children->items[i], depths, max, level);
    }
}

/* logic depth of a tree of entity e, through the functions it uses */
void fcttree_depth(FitEntity *e, FitFctTree *t, int *depth)
{
    FitList *depths = list_create_sized(tt2_next_var_id(e));

    depths->count = depths->capacity;
    for (int i = 0; i < depths->count; i++)
        depths->items[i] = FIT_INT2PTR(-1);
    fcttree_depth_rec(e, t, depths, depth, 1);
    list_destroy(&depths);
}

/* look for a combinational cycle through the tree.  path holds
 * the ids of the functions being expanded, seen collects the ids met;
 * FitFct.reach memoises what a function reaches.  Returns nonzero when a
 * cycle was found (*cycle = its description) or the check had to stop. */
unsigned fcttree_find_cycle(FitEntity *e, FitFctTree *t, FitList *path, FitList *seen, char **cycle)
{
    char msg[0x100];

    switch (t->sel) {
    case FIT_SEL_NOT:
    case FIT_SEL_AND:
    case FIT_SEL_OR:
    case FIT_SEL_EQ:
    case FIT_SEL_XOR:
        for (int i = 0; i < child_count(t); i++) {
            if (fcttree_find_cycle(e, t->u.children->items[i], path, seen, cycle) != 0)
                return 1;
        }
        return 0;
    case FIT_SEL_CONST:
    case 'F':
    case FIT_SEL_EMPTY:
        return 0;
    case FIT_SEL_ID: {
        FitFct *f = NULL;
        FitFctTree *tree;
        FitList *newpath;

        for (int i = 0; i < (e->fcts == NULL ? 0 : e->fcts->count); i++) {
            FitFct *g = e->fcts->items[i];

            if (g->var_id == t->u.id) {
                f = g;
                break;
            }
        }
        if (f == NULL)
            return 0;
        tree = f->tree[0];
        if (tree == NULL) {
            if (f->tree[1] == NULL) {
                char *name = entity_var_name(e, f->var_id);

                strcpy(msg, "Incorrect Fct structure (variable ");
                util_str_cat_n(msg, name, 0x100);
                util_str_cat_n(msg, ") has no  associated equation", 0x100);
                free(name);
                fprintf(g_msg, "%s.\n", msg);
                fprintf(g_msg, "Cycle detection aborted - Results may be incorrect.\n");
                return 1;
            }
            tree = f->tree[1];
        }
        list_add(seen, FIT_INT2PTR(f->var_id));
        newpath = list_map_copy(path, list_identity);
        if (newpath == NULL)        /* path was NULL */
            newpath = list_create();
        list_add(newpath, FIT_INT2PTR(f->var_id));
        if (path != NULL && list_contains_ptr(path, FIT_INT2PTR(f->var_id)) != 0) {
            *cycle = tt2_ids_to_string(e, newpath);
            list_destroy(&newpath);
            return 1;
        }
        if (f->reach == NULL) {
            FitList *reach = list_create();
            unsigned r = fcttree_find_cycle(e, tree, newpath, reach, cycle);

            f->reach = reach;
            list_destroy(&newpath);
            return r;
        } else {
            FitList *common = list_intersection(newpath, f->reach, list_ptr_eq);
            unsigned found = common != NULL;

            if (found) {
                list_append_mapped(newpath, f->reach, list_identity);
                *cycle = tt2_ids_to_string(e, newpath);
                list_destroy(&common);
            }
            list_destroy(&newpath);
            return found;
        }
    }
    case FIT_SEL_SET:
        strcpy(msg, "Incorrect Fct structure (complex variable identified)");
        fprintf(g_msg, "%s.\n", msg);
        fprintf(g_msg, "Cycle detection aborted - Results may be incorrect.\n");
        return 1;
    default:
        return 1;
    }
}

/* number of leaves with variable id */
unsigned fcttree_count_id(FitFctTree *t, int id)
{
    unsigned n = 0;

    if (t->sel == FIT_SEL_ID)
        return id == t->u.id;
    if (t->sel == FIT_SEL_CONST)
        return 0;
    for (int i = 0, count = child_count(t); i < count; i++)
        n += fcttree_count_id(t->u.children->items[i], id);
    return n;
}

/* number of variable leaves */
int fcttree_count_leaves(FitFctTree *t)
{
    int n = 0;

    if (t->sel == FIT_SEL_ID)
        return 1;
    if (t->sel == FIT_SEL_CONST)
        return 0;
    for (int i = 0, count = child_count(t); i < count; i++)
        n += fcttree_count_leaves(t->u.children->items[i]);
    return n;
}

/* every leaf f->var_id becomes f->tree[0] itself (the subtree is
 * shared between all the places, not copied) */
void fcttree_substitute(FitFctTree **t, FitFct *f)
{
    FitFctTree *n = *t;

    if (n->sel == FIT_SEL_ID) {
        if (f->var_id == n->u.id)
            *t = f->tree[0];
    } else if (n->sel != FIT_SEL_CONST) {
        for (int i = 0, count = child_count(n); i < count; i++) {
            FitFctTree *child = n->u.children->items[i];

            fcttree_substitute(&child, f);
            n->u.children->items[i] = child;
        }
    }
}

/* an AND / OR with one operand is replaced by
 * the operand, and an operand with the same operator as its parent is
 * merged into the parent (its operands are appended at the end). */
void fcttree_compact(FitFctTree **t)
{
    FitFctTree *n = *t;
    int sel = n->sel;
    int count, pos;

    if (sel == FIT_SEL_ID || sel == FIT_SEL_CONST)
        return;
    if (sel != FIT_SEL_AND && sel != FIT_SEL_OR) {
        if (sel == FIT_SEL_NOT) {
            FitFctTree *child = n->u.children->items[0];

            fcttree_compact(&child);
            n->u.children->items[0] = child;
            return;
        }
        if (sel != FIT_SEL_EQ && sel != FIT_SEL_XOR) {
            fprintf(g_msg, "Unknown operator in FctCompactFctTree.\n");
            return;
        }
        for (int i = 0; i < child_count(*t); i++) {
            FitFctTree *child = (*t)->u.children->items[i];

            fcttree_compact(&child);
            (*t)->u.children->items[i] = child;
        }
        return;
    }
    if (n->u.children != NULL && n->u.children->count == 1) {
        FitFctTree *only = n->u.children->items[0];

        fcttree_compact(&only);
        list_destroy(&(*t)->u.children);
        free(*t);
        *t = only;
        return;
    }
    count = child_count(n);
    pos = 1;
    for (int i = 0; i < count; i++, pos++) {
        FitFctTree *child = (*t)->u.children->items[i];

        fcttree_compact(&child);
        n = *t;
        if (n->sel == child->sel) {
            fcttree_append_children(n->u.children, &child->u.children);
            /* The slot is deleted also when the recursive call replaced
             * the operand (a single-operand node), so a freed node never
             * stays in the list. */
            list_del_shift((*t)->u.children, pos);
            count--;
            i--;
            pos--;
        } else {
            n->u.children->items[i] = child;
        }
    }
}

FitFctTree *fcttree_new_id(int id)
{
    FitFctTree *t = fcttree_new();

    t->sel = FIT_SEL_ID;
    t->u.id = id;
    return t;
}

FitFctTree *fcttree_new_const(int value)
{
    FitFctTree *t = fcttree_new();

    t->sel = FIT_SEL_CONST;
    t->u.value = value;
    return t;
}

/* new NOT node over child; NULL for NULL */
FitFctTree *fcttree_new_not(FitFctTree *child)
{
    FitFctTree *t;

    if (child == NULL)
        return NULL;
    t = fcttree_new();
    t->sel = FIT_SEL_NOT;
    t->u.children = list_create_sized(1);
    list_add(t->u.children, child);
    return t;
}

/* "a sel b".  XOR and equivalence are expanded:
 * a ^ b = !a*b + !b*a and a = b = !a*!b + a*b, the negated operands being
 * copies. */
FitFctTree *fcttree_new_binary(int sel, FitFctTree *a, FitFctTree *b)
{
    FitFctTree *first, *second, *t;

    if (a == NULL || b == NULL)
        return NULL;
    if (sel == FIT_SEL_XOR) {
        FitFctTree *na = fcttree_copy(fcttree_new_not(a), NULL);
        FitFctTree *nb = fcttree_copy(fcttree_new_not(b), NULL);

        first = fcttree_new_binary(FIT_SEL_AND, na, b);
        second = fcttree_new_binary(FIT_SEL_AND, nb, a);
    } else if (sel == FIT_SEL_EQ) {
        FitFctTree *na = fcttree_copy(fcttree_new_not(a), NULL);
        FitFctTree *nb = fcttree_copy(fcttree_new_not(b), NULL);

        first = fcttree_new_binary(FIT_SEL_AND, na, nb);
        second = fcttree_new_binary(FIT_SEL_AND, a, b);
    } else {
        t = fcttree_new();
        t->sel = sel;
        t->u.children = list_create_sized(2);
        list_add(t->u.children, a);
        list_add(t->u.children, b);
        return t;
    }
    if (first == NULL)
        return NULL;
    return fcttree_new_binary(FIT_SEL_OR, first, second);
}

FitFctTree *fcttree_new_binary_ids(int sel, int id_a, int id_b)
{
    FitFctTree *a = fcttree_new_id(id_a);
    FitFctTree *b = fcttree_new_id(id_b);

    return fcttree_new_binary(sel, a, b);
}
