/*
 * synxor.c - xor_detect module of the ATF150x fitter: finds functions of
 * the form  (x1 ^ ... ^ xn) ^ g  and
 * moves the XOR into the macrocell's XOR gate.
 *
 * For every function named in the xor_synthesis list (all functions when
 * the list is empty) the inputs are grouped by how often they occur
 * positively and negatively; for each group, largest first, the BDD of the
 * function is built with the group's variables on top and walked: when the
 * group's variables lead to exactly one sub-function reached once directly
 * and once inverted, the function is rewritten as an "XOR" gate (netlist
 * item of kind 10) of two new nodes ATM_n.
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

static int      g_xor_count;        /* sum of the variable positions below inverted edges */
static FitList *g_xor_new_inputs;   /* ids of local functions replaced by an XOR gate output */
static FitList *g_xor_aliases;      /* FitAlias made for the gate pins */
static FitList *g_xor_new_outputs;  /* ids of locals that nothing reads any more */

static void *xor_copy_var(void *v)      /* var_copy as FitListCopy */
{
    return var_copy(v);
}

/* delete the function of variable id from entity->fcts.  The
 * item is released with free() only (its trees are not).  When no function
 * has this id the last one is deleted. */
static void xor_delete_fct(FitEntity *entity, int id)
{
    FitList *fcts = entity->fcts;
    int pos = 0;
    int i;

    for (i = 0; i < fcts->count; i++) {
        pos++;
        if (((FitFct *)fcts->items[i])->var_id == id)
            break;
    }
    list_del_shift_free(fcts, pos, util_free_ptr);
}

/* the variable with this id.  Inputs are looked for in inputs,
 * locals, outputs; outputs in outputs; locals in locals, outputs.
 * NULL when the id is unknown. */
static FitVariable *xor_var_by_id(FitEntity *entity, int id)
{
    FitList *l;
    int cls;
    int i;

    if (entity_is_input(entity, id))
        cls = 0;
    else if (entity_is_output(entity, id))
        cls = 1;
    else if (entity_is_local(entity, id))
        cls = 2;
    else
        return NULL;

    if (cls == 0) {
        l = entity->inputs;
        for (i = 0; i < LCOUNT(l); i++)
            if (((FitVariable *)l->items[i])->id == id)
                return l->items[i];
    }
    if (cls != 1) {
        l = entity->locals;
        for (i = 0; i < LCOUNT(l); i++)
            if (((FitVariable *)l->items[i])->id == id)
                return l->items[i];
    }
    l = entity->outputs;
    for (i = 0; i < LCOUNT(l); i++)
        if (((FitVariable *)l->items[i])->id == id)
            return l->items[i];
    return NULL;
}

/* name of a variable for messages and netlist strings */
static const char *xor_var_name(FitEntity *entity, int id)
{
    FitVariable *v = xor_var_by_id(entity, id);

    return v != NULL ? v->name : "";
}

static void xor_delete_fcts(FitEntity *entity, FitList *ids)
{
    int i;

    for (i = 0; i < LCOUNT(ids); i++)
        xor_delete_fct(entity, FIT_PTR2INT(ids->items[i]));
}

/* move the local variable id to the inputs (to_outputs == 0) or
 * to the outputs: a copy is appended there and the local is deleted.  When
 * no local has this id the last local is deleted. */
static void xor_move_local(FitEntity *entity, int id, int to_outputs)
{
    FitList *locals = entity->locals;
    int pos = 0;
    int i;

    for (i = 0; i < locals->count; i++) {
        FitVariable *v = locals->items[i];

        pos++;
        if (v->id == id) {
            list_add(to_outputs == 0 ? entity->inputs : entity->outputs, var_copy(v));
            break;
        }
    }
    list_del_shift_free(entity->locals, pos, util_free_ptr);
}

static void xor_move_locals(FitEntity *entity, FitList *ids, int to_outputs)
{
    int i;

    for (i = 0; i < LCOUNT(ids); i++)
        xor_move_local(entity, FIT_PTR2INT(ids->items[i]), to_outputs);
}

/* number of positive occurrences of id (nothing below a NOT
 * counts) */
static unsigned xor_tree_count_pos(FitFctTree *t, int id)
{
    unsigned n = 0;
    int i;

    if (t->sel == FIT_SEL_ID)
        return id == t->u.id;
    if (t->sel == FIT_SEL_CONST)
        return 0;
    if (t->sel != FIT_SEL_NOT) {
        for (i = 0; i < t->u.children->count; i++)
            n += xor_tree_count_pos(t->u.children->items[i], id);
    }
    return n;
}

static int xor_count_pos_uses(FitEntity *entity, int id)
{
    int n = 0;
    int i;

    for (i = 0; i < LCOUNT(entity->fcts); i++)
        n += (int)xor_tree_count_pos(((FitFct *)entity->fcts->items[i])->tree[0], id);
    return n;
}

/* number of occurrences of !id (the operand of a NOT is taken to
 * be a variable) */
static unsigned xor_tree_count_neg(FitFctTree *t, int id)
{
    unsigned n = 0;
    int i;

    if (t->sel == FIT_SEL_NOT)
        return id == ((FitFctTree *)t->u.children->items[0])->u.id;
    if (t->sel == FIT_SEL_CONST || t->sel == FIT_SEL_ID)
        return 0;
    for (i = 0; i < t->u.children->count; i++)
        n += xor_tree_count_neg(t->u.children->items[i], id);
    return n;
}

static int xor_count_neg_uses(FitEntity *entity, int id)
{
    int n = 0;
    int i;

    for (i = 0; i < LCOUNT(entity->fcts); i++)
        n += (int)xor_tree_count_neg(((FitFct *)entity->fcts->items[i])->tree[0], id);
    return n;
}

/* 1-based positions of the lists of `lists` that hold both a
 * and b (lists of integers) */
static FitList *xor_find_count_pairs(FitList *lists, int a, int b)
{
    FitList *res = list_create();
    int i;

    for (i = 0; i < LCOUNT(lists); i++) {
        if (list_find(lists->items[i], FIT_INT2PTR(a), list_ptr_eq) != 0 &&
            list_find(lists->items[i], FIT_INT2PTR(b), list_ptr_eq) != 0)
            list_add(res, FIT_INT2PTR(i + 1));
    }
    return res;
}

/* over the AND terms of the sum `tree`, note which of the four
 * sign combinations of the variables a and b occur together in a term;
 * 0 as soon as two different combinations other than the two an XOR pair
 * may show have been seen. */
static int xor_pair_compatible(FitFctTree *tree, int a, int b)
{
    FitList *terms = tree->u.children;
    int seen_pb_na = 0, seen_pa_nb = 0, seen_pa_pb = 0, seen_na_nb = 0;
    int i, j;

    for (i = 0; i < terms->count; i++) {
        FitFctTree *term = terms->items[i];
        int pos_a = 0, neg_a = 0, pos_b = 0, neg_b = 0;

        if (term->sel != FIT_SEL_AND)
            continue;
        for (j = 0; j < term->u.children->count; j++) {
            FitFctTree *lit = term->u.children->items[j];

            if (lit->sel == FIT_SEL_NOT) {
                int id = ((FitFctTree *)lit->u.children->items[0])->u.id;

                if (a == id)
                    neg_a = 1;
                if (b == id)
                    neg_b = 1;
            } else if (lit->sel == FIT_SEL_ID) {
                if (a == lit->u.id)
                    pos_a = 1;
                if (b == lit->u.id)
                    pos_b = 1;
            }
        }
        if (pos_b && neg_a)
            seen_pb_na = 1;
        if (pos_a && neg_b)
            seen_pa_nb = 1;
        if (pos_b && pos_a)
            seen_pa_pb = 1;
        if (neg_b && neg_a)
            seen_na_nb = 1;
        if (seen_pb_na) {
            if (seen_pa_nb)
                return 0;
            if (seen_pa_pb)
                return 0;
        } else {
            if (seen_pa_nb && seen_na_nb)
                return 0;
            if (seen_pa_pb && seen_na_nb)
                return 0;
        }
        if (seen_pb_na && seen_na_nb)
            return 0;
        if (seen_pa_nb && seen_pa_pb)
            return 0;
    }
    return 1;
}

/* 1 when id is compatible with one member of the group */
static int xor_group_compatible(FitFctTree *tree, FitList *group, int id)
{
    int i;

    for (i = 0; i < LCOUNT(group); i++)
        if (xor_pair_compatible(tree, FIT_PTR2INT(group->items[i]), id) != 0)
            return 1;
    return 0;
}

/* new list with copies of the lists of `lists`, largest first
 * (the first of equal size first); `lists` is emptied. */
static FitList *xor_sort_groups_by_size(FitList *lists)
{
    FitList *res = list_create();
    int best = 0;   /* always set below because no group is empty */

    while (lists != NULL && lists->count != 0) {
        int best_size = 0;
        int i;
        FitList *g;

        for (i = 0; i < lists->count; i++) {
            int n = LCOUNT((FitList *)lists->items[i]);

            if (best_size < n) {
                best = i;
                best_size = n;
            }
        }
        if (best >= lists->count)
            best = 0;
        g = lists->items[best];
        list_add(res, list_map_copy(g, list_identity));
        list_del_shift(lists, best + 1);
    }
    return res;
}

/* AND node over the nodes of `children` (shared, not copied) */
static FitFctTree *xor_new_and_node(FitList *children)
{
    FitFctTree *n = fcttree_new();

    n->sel = FIT_SEL_AND;
    n->u.children = list_map_copy(children, list_identity);
    return n;
}

/* new variable ATM_<id> appended to the inputs (to_outputs == 0)
 * or the outputs */
static void xor_add_atm_var(FitEntity *entity, int id, int to_outputs)
{
    char buf[256];
    FitVariable *v = var_new(0);

    snprintf(buf, sizeof buf, "ATM_%d", id);
    strcpy(v->name, buf);
    v->id = id;
    if (to_outputs == 0)
        list_add(entity->inputs, v);
    else
        list_add(entity->outputs, v);
}

/* the list of by_level that holds `node` (first match); the last list
 * if none holds it. */
static FitList *xor_level_list_of(FitList *by_level, FitBddNode *node)
{
    int i;

    for (i = 0; i < by_level->count; i++)
        if (list_find(by_level->items[i], node, list_ptr_eq) != 0)
            return by_level->items[i];
    return by_level->count > 0 ? by_level->items[by_level->count - 1] : NULL;
}

/* a child that is reached by both direct and inverted
 * edges is split in two, so that below this point every node is reached
 * with one polarity only. */
static void xor_split_child(FitBddNode *child, int mark, FitList *unique, FitList *blocks,
                            FitList *infos, FitList *by_level)
{
    FitList *level = xor_level_list_of(by_level, child);
    FitList *parents = child->info->parents;
    FitList *direct = NULL, *inverted = NULL;

    if (parents != NULL && parents->count > 1) {
        bdd_split_parents(child, parents, &direct, &inverted);
        if (direct != NULL && direct->count != 0 && inverted != NULL && inverted->count != 0)
            bdd_clone_inverted(child, mark == FIT_BDD_DIRECT ? inverted : direct, level,
                               unique, blocks, infos, by_level);
    }
}

/* walk the BDD from `node` through the variables 1..width and
 * collect the nodes below them (`nodes`, with the mark of the edge that
 * leads there in `marks`).  A node reached a second time with the other
 * mark is entered again and remembered in `dups`.  g_xor_count adds up the
 * variable positions of the nodes entered through an inverted edge. */
static void xor_walk_bdd(FitBddNode *node, int width, FitBddNode *terminal, FitList *nodes,
                         FitList *marks, FitList *dups, FitList *unique, FitList *blocks,
                         FitList *infos, FitList *by_level)
{
    int pos;

    if (node == terminal || width == 0) {
        list_add(nodes, node);
        list_add(marks, FIT_INT2PTR(FIT_BDD_DIRECT));
        return;
    }

    /* the lo edge */
    if (node->lo == terminal) {
        pos = list_find(nodes, terminal, list_ptr_eq);
        if (pos == 0) {
            list_add(nodes, node->lo);
            list_add(marks, FIT_INT2PTR(node->lo_mark));
            if (node->lo_mark != FIT_BDD_DIRECT)
                g_xor_count += node->lo->var;
            goto second;
        }
        if (FIT_PTR2INT(marks->items[pos - 1]) == node->lo_mark ||
            list_find(dups, node->lo, list_ptr_eq) != 0)
            goto second;
        list_add(nodes, node->lo);
        list_add(marks, FIT_INT2PTR(node->lo_mark));
        list_add(dups, node->lo);
    } else {
        xor_split_child(node->lo, node->lo_mark, unique, blocks, infos, by_level);
        if (node->lo->var <= width) {
            xor_walk_bdd(node->lo, width, terminal, nodes, marks, dups, unique, blocks, infos,
                         by_level);
            goto second;
        }
        pos = list_find(nodes, node->lo, list_ptr_eq);
        if (pos == 0) {
            list_add(nodes, node->lo);
            list_add(marks, FIT_INT2PTR(node->lo_mark));
        } else {
            if (FIT_PTR2INT(marks->items[pos - 1]) == node->lo_mark ||
                list_find(dups, node->lo, list_ptr_eq) != 0)
                goto second;
            list_add(nodes, node->lo);
            list_add(marks, FIT_INT2PTR(node->lo_mark));
            list_add(dups, node->lo);
        }
    }
    if (node->lo_mark != FIT_BDD_DIRECT)
        g_xor_count += node->lo->var;

second:
    /* the hi edge.  The count tests below look at the mark of the other
     * edge (lo_mark) in three of the four places.  This is
     * deliberate: which XORs are found depends on it. */
    if (node->hi == terminal) {
        pos = list_find(nodes, node->hi, list_ptr_eq);
        if (pos == 0) {
            list_add(nodes, node->hi);
            list_add(marks, FIT_INT2PTR(node->hi_mark));
            if (node->lo_mark == FIT_BDD_DIRECT)
                return;
            g_xor_count += node->hi->var;
            return;
        }
        if (FIT_PTR2INT(marks->items[pos - 1]) == node->hi_mark)
            return;
        if (list_find(dups, node->hi, list_ptr_eq) != 0)
            return;
        list_add(nodes, node->hi);
        list_add(marks, FIT_INT2PTR(node->hi_mark));
        list_add(dups, node->hi);
    } else {
        xor_split_child(node->hi, node->hi_mark, unique, blocks, infos, by_level);
        if (node->hi->var <= width) {
            xor_walk_bdd(node->hi, width, terminal, nodes, marks, dups, unique, blocks, infos,
                         by_level);
            return;
        }
        pos = list_find(nodes, node->hi, list_ptr_eq);
        if (pos != 0) {
            if (FIT_PTR2INT(marks->items[pos - 1]) == node->hi_mark)
                return;
            if (list_find(dups, node->hi, list_ptr_eq) != 0)
                return;
            list_add(nodes, node->hi);
            list_add(marks, FIT_INT2PTR(node->hi_mark));
            list_add(dups, node->hi);
            if (node->hi_mark == FIT_BDD_DIRECT)
                return;
            g_xor_count += node->hi->var;
            return;
        }
        list_add(nodes, node->hi);
        list_add(marks, FIT_INT2PTR(node->hi_mark));
    }
    if (node->lo_mark == FIT_BDD_DIRECT)
        return;
    g_xor_count += node->hi->var;
}

/* rewrite the single function of `work` (a sum of products in
 * which the variables of `group` form an XOR with the rest) as
 *     ATM_a = AND of the group literals of the first term that has them all
 *     ATM_b = OR of the terms that hold exactly one group literal, without it
 *     out   = XOR gate (ATM_a, ATM_b)
 * `entity` is the design the function comes from; the new variables are
 * added to both. */
static void xor_rewrite_function(FitEntity *entity, FitEntity *work, FitList *group)
{
    FitNetlist *net;
    FitFctTree *tree, *and_node, *or_node;
    FitList *terms;
    FitFct *f;
    FitUnknownGate *gate;
    FitAlias *al;
    FitNetItem *item;
    char *s, *name, *indexed;
    int is_output = 0;
    int and_done = 0;
    int orig_id = 0;
    int out_id, id1, id2;
    int i, j;

    net = util_calloc(1, sizeof *net);
    net->items = list_create();
    net->aliases = list_create();
    work->net = netlist_copy(net);

    tree = ((FitFct *)work->fcts->items[0])->tree[0];
    and_node = fcttree_new();
    and_node->sel = FIT_SEL_AND;
    and_node->u.children = list_create();
    or_node = fcttree_new();
    or_node->sel = FIT_SEL_OR;
    or_node->u.children = list_create();

    terms = tree->u.children;
    for (i = 0; i < terms->count; i++) {
        FitFctTree *term = terms->items[i];
        FitList *rest = list_create();
        FitList *grp = list_create();
        /* every term is expected to be a product here; anything else is
         * taken as a term without literals */
        FitList *lits = term->sel == FIT_SEL_AND ? term->u.children : NULL;

        for (j = 0; j < LCOUNT(lits); j++) {
            FitFctTree *lit = lits->items[j];
            FitFctTree *add;
            FitList *dest = grp;

            if (lit->sel == FIT_SEL_NOT) {
                FitFctTree *child = lit->u.children->items[0];

                if (list_find(group, FIT_INT2PTR(child->u.id), list_ptr_eq) == 0) {
                    add = lit;
                    dest = rest;
                } else if (group != NULL && group->count > 1) {
                    add = lit;
                } else {
                    add = child;
                }
            } else if (lit->sel == FIT_SEL_ID) {
                if (list_find(group, FIT_INT2PTR(lit->u.id), list_ptr_eq) != 0) {
                    if (group == NULL || group->count != 1) {
                        add = lit;
                    } else {
                        if (rest != NULL && rest->count != 0)
                            list_destroy(&rest);
                        break;
                    }
                } else {
                    add = lit;
                    dest = rest;
                }
            } else {
                continue;
            }
            list_add(dest, add);
        }

        if (LCOUNT(lits) - LCOUNT(rest) == 1)
            list_add(or_node->u.children, xor_new_and_node(rest));
        if (!and_done && LCOUNT(grp) == LCOUNT(group)) {
            and_node->u.children = list_map_copy(grp, list_identity);
            and_done = 1;
        }
        /* rest and grp are not released */
    }

    out_id = ((FitFct *)work->fcts->items[0])->var_id;
    if (entity_is_local(entity, out_id) == 0) {
        is_output = 1;
        orig_id = out_id;
        out_id = tt2_next_var_id(entity);
        xor_add_atm_var(entity, out_id, 0);
        xor_add_atm_var(work, out_id, 0);
        ((FitFct *)work->fcts->items[0])->tree[0] = fcttree_new_id(out_id);
    } else {
        work->fcts = list_create();
        list_add(g_xor_new_inputs, FIT_INT2PTR(out_id));
    }

    f = fct_new();
    id1 = tt2_next_var_id(entity);
    xor_add_atm_var(entity, id1, 1);
    xor_add_atm_var(work, id1, 1);
    f->var_id = id1;
    f->tree[0] = and_node;
    list_add(work->fcts, f);

    f = fct_new();
    id2 = tt2_next_var_id(entity);
    xor_add_atm_var(entity, id2, 1);
    xor_add_atm_var(work, id2, 1);
    f->var_id = id2;
    f->tree[0] = or_node;
    list_add(work->fcts, f);

    work->n_local = LCOUNT(work->locals);

    gate = util_calloc(1, sizeof *gate);
    gate->pins = list_create();
    s = util_strdup("XOR");
    gate->model = s;
    name = util_strdup(s);
    util_str_append(&name, "_");
    indexed = util_str_with_index(name, out_id);
    gate->inst_name = util_strdup(indexed);

    s = util_strdup(xor_var_name(entity, id1));
    lib_unknown_gate_add_pin(gate->pins, s, "I_1", 1);
    al = alias_new();
    al->name = util_strdup(s);
    list_add(g_xor_aliases, al);

    s = util_strdup(xor_var_name(entity, id2));
    lib_unknown_gate_add_pin(gate->pins, s, "I_2", 1);
    al = alias_new();
    al->name = util_strdup(s);
    list_add(g_xor_aliases, al);

    s = util_strdup(xor_var_name(entity, out_id));
    lib_unknown_gate_add_pin(gate->pins, s, "O", 0);
    al = alias_new();
    al->name = util_strdup(s);
    if (is_output)
        al->target = util_strdup(xor_var_name(entity, orig_id));
    list_add(g_xor_aliases, al);

    gate->i00 = 1;
    item = util_calloc(1, sizeof *item);
    item->kind = FIT_GATE_UNKNOWN;
    item->cell = gate;
    list_add(work->net->items, item);
}

/* try to find an XOR in the one-function entity *work.
 * Returns 1 and replaces *work by the rewritten entity when one is found. */
static int xor_try_function(FitEntity *entity, FitEntity **work)
{
    FitList *groups = list_create();   /* lists of variable ids */
    FitList *counts = list_create();   /* per group: {positive uses, negative uses} */
    FitEntity *m;
    FitFctTree *tree;
    int nterms;
    int k, gi, j;

    m = entity_copy(domlist_minimise_entity(*work, 1, 1, 0x32));
    if (m->n_in <= 1)
        return 0;

    tree = ((FitFct *)m->fcts->items[0])->tree[0];
    /* a constant or a single variable has no child list */
    if (tree->sel == FIT_SEL_ID || tree->sel == FIT_SEL_CONST)
        nterms = 0;
    else
        nterms = LCOUNT(tree->u.children);

    for (k = 0; k < LCOUNT(m->inputs); k++) {
        int id = ((FitVariable *)m->inputs->items[k])->id;
        int np = xor_count_pos_uses(m, id);
        int nn = xor_count_neg_uses(m, id);
        FitList *sel;
        FitList *tmp;
        int found = 0;

        sel = xor_find_count_pairs(counts, np, nn);
        for (j = 0; j < LCOUNT(sel); j++) {
            int g = FIT_PTR2INT(sel->items[j]);

            if (xor_group_compatible(((FitFct *)m->fcts->items[0])->tree[0],
                                     groups->items[g - 1], id) != 0) {
                list_add(groups->items[g - 1], FIT_INT2PTR(id));
                found = 1;
                break;
            }
        }
        list_destroy(&sel);
        if (found)
            continue;

        tmp = list_create();
        list_add(tmp, FIT_INT2PTR(np));
        list_add(tmp, FIT_INT2PTR(nn));
        list_add(counts, list_map_copy(tmp, list_identity));
        list_destroy(&tmp);
        tmp = list_create();
        list_add(tmp, FIT_INT2PTR(id));
        list_add(groups, list_map_copy(tmp, list_identity));
        list_destroy(&tmp);
    }

    if (groups != NULL && groups->count != 0) {
        FitList *keep = list_create();
        int i;

        for (i = 0; i < LCOUNT(groups); i++) {
            FitList *pr = counts->items[i];
            int a = FIT_PTR2INT(pr->items[0]);
            int b = FIT_PTR2INT(pr->items[1]);
            FitList *g = groups->items[i];
            int n = LCOUNT(g);

            if (n * b + a == nterms || n * a + b == nterms)
                list_add(keep, list_map_copy(g, list_identity));
        }
        list_destroy(&groups);
        groups = list_map_copy(keep, list_identity);
        list_destroy(&keep);
    }
    {
        FitList *sorted = xor_sort_groups_by_size(groups);

        groups = list_map_copy(sorted, list_identity);
    }

    for (gi = 0; gi < LCOUNT(groups); gi++) {
        FitList *group = groups->items[gi];
        FitList *roots = NULL, *blocks = NULL, *unique = NULL, *infos = NULL;
        FitList *by_level, *nodes, *marks, *dups;
        FitBddNode *terminal = NULL;
        int n_nodes = 0;

        /* the group's variables become the first inputs (the last of the
         * group first) */
        for (j = 0; j < LCOUNT(group); j++) {
            int p = var_list_pos_of_id(m->inputs, FIT_PTR2INT(group->items[j]));

            if (p != 0) {
                FitVariable *v = var_copy(m->inputs->items[p - 1]);

                list_del_shift(m->inputs, p);
                list_insert_at(m->inputs, v, 1);
            }
        }
        bdd_build_entity_100(m, &roots, &blocks, &unique, &terminal, &n_nodes, 100000);
        if (n_nodes < 0) {
            bdd_free_blocks(blocks);
            bdd_free_unique_table(unique);
            /* too many nodes: no XOR for this function */
            return 0;
        }
        bdd_annotate(roots, &infos, terminal);
        by_level = bdd_nodes_by_level(roots, terminal);
        nodes = list_create();
        marks = list_create();
        dups = list_create();
        xor_walk_bdd(((FitBddRoot *)roots->items[0])->node, LCOUNT(group), terminal, nodes,
                     marks, dups, unique, blocks, infos, by_level);
        if (nodes != NULL && nodes->count == 2 && marks != NULL && marks->count == 2) {
            if (LCOUNT(group) == g_xor_count - 1 && nodes->items[0] == nodes->items[1]) {
                xor_rewrite_function(entity, m, group);
                *work = entity_copy(domlist_minimise_entity(m, 1, 1, 0x32));
                return 1;
            }
        }
        /* the BDD, the level lists and the three lists are not released */
    }
    return 0;
}

void xor_detect_entity(FitEntity **entity)
{
    FitEntity *e;
    FitList *found_ids, *new_fcts, *new_items;
    int i, j;

    if ((*entity)->net->items == NULL)
        (*entity)->net->items = list_create();
    if ((*entity)->net->aliases == NULL)
        (*entity)->net->aliases = list_create();
    g_xor_aliases = list_create();
    g_xor_new_inputs = list_create();
    g_xor_new_outputs = list_create();
    found_ids = list_create();
    e = entity_copy(domlist_minimise_entity(*entity, 1, 1, 0x32));
    new_fcts = list_create();
    new_items = list_create();

    if (g_opt.xor_list == NULL || g_opt.xor_list->count == 0) {
        g_opt.xor_list = list_create();
        for (i = 0; i < LCOUNT(e->fcts); i++) {
            FitFct *c = fct_copy(e->fcts->items[i]);

            list_add(g_opt.xor_list, FIT_INT2PTR(c->var_id));
            fct_free(&c);
        }
    }

    for (i = 0; i < LCOUNT(e->fcts); i++) {
        FitFct *fc = fct_copy(e->fcts->items[i]);
        FitEntity *w;
        FitList *ids, *in_vars, *out_vars;
        int id;

        if (list_find(g_opt.xor_list, FIT_INT2PTR(fc->var_id), list_ptr_eq) == 0) {
            fct_free(&fc);
            continue;
        }
        id = fc->var_id;
        g_xor_count = 0;
        w = entity_new(NULL);
        ids = list_create();
        in_vars = list_create();
        out_vars = list_create();
        fcttree_collect_ids(fc->tree[0], ids);
        for (j = 0; j < LCOUNT(ids); j++)
            list_add(in_vars, xor_var_by_id(e, FIT_PTR2INT(ids->items[j])));
        list_add(out_vars, xor_var_by_id(e, fc->var_id));
        w->inputs = list_map_copy(in_vars, xor_copy_var);
        w->n_in = LCOUNT(w->inputs);
        w->outputs = list_map_copy(out_vars, xor_copy_var);
        w->n_out = LCOUNT(w->outputs);
        w->locals = list_create();
        w->n_local = 0;
        w->fcts = list_create();
        list_add(w->fcts, fc);

        if (xor_try_function(e, &w) != 0) {
            list_add(found_ids, FIT_INT2PTR(id));
            if (entity_is_local(e, id) == 0)
                fprintf(g_msg, "   A XOR is detected for the OUTPUT \t%s\n", xor_var_name(e, id));
            else
                fprintf(g_msg, "   A XOR is detected for the PIN \t%s\n", xor_var_name(e, id));
            for (j = 0; j < LCOUNT(w->fcts); j++)
                list_add(new_fcts, fct_copy(w->fcts->items[j]));
            list_append_destroy(new_items, &w->net->items);
        }
        entity_free(&w);
        list_destroy(&ids);
        list_destroy(&in_vars);
        list_destroy(&out_vars);
    }

    if (new_fcts != NULL && new_fcts->count != 0) {
        xor_delete_fcts(e, found_ids);
        if (g_xor_new_inputs != NULL && g_xor_new_inputs->count != 0) {
            xor_move_locals(e, g_xor_new_inputs, 0);
            list_destroy(&g_xor_new_inputs);
        }
        list_append_destroy(e->fcts, &new_fcts);
        list_append_destroy(e->net->items, &new_items);
        list_append_destroy(e->net->aliases, &g_xor_aliases);
        *entity = entity_copy(e);   /* the entity it replaces is not released */
        list_destroy(&new_fcts);
        entity_free(&e);

        g_xor_aliases = list_create();
        for (i = 0; i < LCOUNT((*entity)->fcts); i++) {
            int id = ((FitFct *)(*entity)->fcts->items[i])->var_id;

            if (entity_is_local(*entity, id) != 0 && tt2_count_id_uses(*entity, id) == 0) {
                FitAlias *al;
                char *s;

                list_add(g_xor_new_outputs, FIT_INT2PTR(id));
                s = util_strdup(xor_var_name(*entity, id));
                al = alias_new();
                al->name = util_strdup(s);
                list_add(g_xor_aliases, al);
            }
        }
        if (g_xor_new_outputs != NULL && g_xor_new_outputs->count != 0) {
            xor_move_locals(*entity, g_xor_new_outputs, 1);
            list_destroy(&g_xor_new_outputs);
        }
        if (g_xor_aliases != NULL && g_xor_aliases->count != 0)
            list_append_destroy((*entity)->net->aliases, &g_xor_aliases);
    }

    g_opt.xor_list = list_map_copy(found_ids, list_identity);
    (*entity)->n_local = LCOUNT((*entity)->locals);
    (*entity)->n_out = LCOUNT((*entity)->outputs);
    (*entity)->n_in = LCOUNT((*entity)->inputs);
    /* the result of this call is dropped; it is made for its side effects
     * (n_local of *entity, generated names) */
    (void)entity_simplified_copy(*entity);
    if (found_ids != NULL && found_ids->count != 0)
        list_destroy(&found_ids);
}
