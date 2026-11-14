/*
 * min_equiv.c - equivalence check of the ATF150x fitter: "Verifying
 * equivalence ..." of two entities.
 *
 * The two entities must have the same input names, output names and
 * netlist.  Then, output by output (or two outputs at a time), the cone of
 * logic of the output is copied out of each entity, the inputs of the first
 * cone are brought into the order of the second (which was sorted by a
 * weight computed from the functions), both cones are turned into BDDs
 * (min_bdd.c) and the BDDs are compared node by node.
 *
 * The messages of this module are printed only when g_equiv_verbose is
 * not 0.
 */
#include <stdio.h>
#include <stdlib.h>

#include "min_priv.h"
#include "lib.h"
#include "tt.h"

#define EQUIV_MAX_NODES 1000000

int g_equiv_verbose;

static FitEntity *g_cone_src;       /* entity the cone is cut from */
static FitEntity *g_cone_dst;       /* the cone */
static FitList   *g_cone_seen;      /* variables of the functions already copied */

static FitEntity *g_order_entity;
static FitList   *g_order_inputs;
static FitList   *g_order_outputs;
static FitList   *g_order_locals;

/* copy into the cone every function the tree depends on */
static void equiv_collect_support(FitFctTree *tree)
{
    for (;;) {
        FitVariable *v;
        int id, pos;

        if (tree->sel == FIT_SEL_CONST)
            return;
        if (tree->sel != FIT_SEL_ID) {
            for (int i = 0; i < min_count(tree->u.children); i++)
                equiv_collect_support(tree->u.children->items[i]);
            return;
        }
        id = tree->u.id;
        if (var_list_pos_of_id(g_cone_src->inputs, id) != 0)
            return;
        pos = var_list_pos_of_id(g_cone_src->locals, id);
        if (pos == 0) {
            pos = var_list_pos_of_id(g_cone_src->outputs, id);
            if (pos == 0)       /* not a variable of the entity */
                return;
            v = g_cone_src->outputs->items[pos - 1];
        } else {
            v = g_cone_src->locals->items[pos - 1];
        }
        if (list_find(g_cone_seen, v, list_ptr_eq) != 0)
            return;
        list_add(g_cone_seen, v);
        list_add(g_cone_dst->fcts, fct_copy(entity_find_fct(g_cone_src, v->id)));
        tree = entity_fct_tree(g_cone_src, v->id);
        if (tree == NULL)       /* no function */
            return;
    }
}

FitEntity *equiv_extract_cone(FitEntity *src, int out_idx, int out_idx2, int step)
{
    FitEntity *e = entity_new(NULL);
    FitVariable *v;
    FitFct *f1, *f2 = NULL;
    int second;

    e->n_local = 0;
    e->n_in = src->n_in;
    e->inputs = list_map_copy(src->inputs, min_copy_var_cb);
    e->outputs = list_create_sized(2);
    e->fcts = list_create_sized(min_count(src->fcts));
    e->net = NULL;
    e->i30 = 0;
    v = var_copy(src->outputs->items[out_idx]);
    list_add(e->outputs, v);
    f1 = entity_find_fct(src, v->id);
    list_add(e->fcts, fct_copy(f1));
    second = out_idx2 < min_count(src->outputs) && step != 1;
    if (second) {
        v = var_copy(src->outputs->items[out_idx2]);
        list_add(e->outputs, v);
        f2 = entity_find_fct(src, v->id);
        list_add(e->fcts, fct_copy(f2));
    }
    e->n_out = min_count(e->outputs);
    g_cone_src = src;
    g_cone_dst = e;
    g_cone_seen = list_create();
    equiv_collect_support(f1->tree[0]);
    if (second)
        equiv_collect_support(f2->tree[0]);
    e->locals = list_map_copy(g_cone_seen, min_copy_var_cb);
    e->n_local = min_count(e->locals);
    list_destroy(&g_cone_seen);
    return e;
}

/* FitListCmp on FitVariable.weight, unsigned */
static int equiv_cmp_var_weight(void *a, void *b)
{
    unsigned wa = (unsigned)((FitVariable *)a)->weight;
    unsigned wb = (unsigned)((FitVariable *)b)->weight;

    if (wb > wa)
        return -1;
    return wb < wa;
}

/* FitListCmp on FitFct.flags (depth, then weight), unsigned */
static int equiv_cmp_fct_weight(void *a, void *b)
{
    unsigned wa = (unsigned)((FitFct *)a)->flags;
    unsigned wb = (unsigned)((FitFct *)b)->flags;

    if (wb > wa)
        return -1;
    return wb < wa;
}

static void equiv_unknown_variable(void)
{
    fprintf(stderr, "** Unknown variable in PropagateWeight\n");
    util_abort_job(0);
}

/* FitFct.flags of every function below `tree` becomes the length
 * of the longest chain of functions from an output down to it */
static void equiv_propagate_depth(FitFctTree *tree, int depth)
{
    while (tree->sel != FIT_SEL_CONST) {
        FitFct *f;

        if (tree->sel != FIT_SEL_ID) {
            for (int i = 0; i < min_count(tree->u.children); i++)
                equiv_propagate_depth(tree->u.children->items[i], depth);
            return;
        }
        if (var_list_pos_of_id(g_order_inputs, tree->u.id) != 0)
            return;
        if (var_list_pos_of_id(g_order_locals, tree->u.id) == 0 &&
            var_list_pos_of_id(g_order_outputs, tree->u.id) == 0) {
            equiv_unknown_variable();
            return;
        }
        f = entity_find_fct(g_order_entity, tree->u.id);
        if (f == NULL || depth <= f->flags)     /* NULL: no function */
            return;
        f->flags = depth;
        tree = f->tree[0];
        depth++;
    }
}

/* add weight + 1 to every input and every function the tree
 * refers to directly */
static void equiv_propagate_weight(FitFctTree *tree, int weight)
{
    if (tree->sel == FIT_SEL_CONST)
        return;
    if (tree->sel == FIT_SEL_ID) {
        int pos = var_list_pos_of_id(g_order_inputs, tree->u.id);
        FitFct *f;

        if (pos != 0) {
            FitVariable *v = g_order_inputs->items[pos - 1];

            v->weight = (int)((unsigned)v->weight + (unsigned)weight + 1u);
            return;
        }
        if (var_list_pos_of_id(g_order_locals, tree->u.id) == 0 &&
            var_list_pos_of_id(g_order_outputs, tree->u.id) == 0) {
            equiv_unknown_variable();
            return;
        }
        f = entity_find_fct(g_order_entity, tree->u.id);
        if (f != NULL)          /* NULL: no function */
            f->flags = (int)((unsigned)f->flags + (unsigned)weight + 1u);
        return;
    }
    for (int i = 0; i < min_count(tree->u.children); i++)
        equiv_propagate_weight(tree->u.children->items[i], weight);
}

/* sort the functions of the entity by depth (deepest first) and
 * its inputs by weight (lightest first).  The input order becomes the
 * variable order of the BDDs. */
static void equiv_order_entity(FitEntity *e)
{
    g_order_inputs = e->inputs;
    g_order_locals = e->locals;
    g_order_outputs = e->outputs;
    g_order_entity = e;
    for (int i = 0; i < min_count(g_order_inputs); i++)
        ((FitVariable *)g_order_inputs->items[i])->weight = 0;
    for (int i = 0; i < min_count(e->fcts); i++)
        ((FitFct *)e->fcts->items[i])->flags = 0;
    for (int i = 0; i < min_count(e->outputs); i++) {
        FitVariable *v = e->outputs->items[i];
        FitFctTree *t = entity_fct_tree(e, v->id);

        if (t != NULL)          /* NULL: no function */
            equiv_propagate_depth(t, 1);
    }
    list_sort(e->fcts, 1, equiv_cmp_fct_weight);
    for (int i = 0; i < min_count(e->fcts); i++)
        ((FitFct *)e->fcts->items[i])->flags = 0;
    for (int i = 0; i < min_count(e->fcts); i++) {
        FitFct *f = e->fcts->items[i];

        equiv_propagate_weight(f->tree[0], f->flags);
    }
    list_sort(g_order_inputs, 0, equiv_cmp_var_weight);
}

/* Replace *vars by a list holding, for each variable of `order`, the
 * variable of *vars with the same name. */
static void equiv_reorder_vars(FitList **vars, FitList *order)
{
    FitList *old = *vars;

    *vars = list_create_sized(min_count(order));
    for (int i = 0; i < min_count(order); i++) {
        FitVariable *v = order->items[i];

        list_add(*vars, var_list_find_name(old, v->name));
    }
    list_destroy(&old);
}

int equiv_verify_entities(FitEntity *a, FitEntity *b, int step)
{
    FitList *diff;
    int unverified = 0;
    int i;

    if (g_equiv_verbose)
        fprintf(g_msg, "Verifying equivalence ...\n");
    if (!list_equal_sets(a->inputs, b->inputs, var_same_name)) {
        if (g_equiv_verbose)
            fprintf(g_msg, "Non equivalent systems (input lists differ).\n");
        return 0;
    }
    if (!list_equal_sets(a->outputs, b->outputs, var_same_name)) {
        if (g_equiv_verbose)
            fprintf(g_msg, "Non equivalent systems (output lists differ).\n");
        return 0;
    }
    if ((lib_netlist_find_wire(a->net) != 0 || lib_netlist_find_wire(b->net) != 0) &&
        g_equiv_verbose)
        fprintf(g_msg, "Loop(s) exist(s) in function, EquivFct may be incorrect !\n");
    if (!lib_netlist_equal(a->net, b->net)) {
        if (g_equiv_verbose)
            fprintf(g_msg, "Non equivalent systems (extra wirings differ).\n");
        return 0;
    }

    tt2_collapse_locals(a, 0);
    tt2_collapse_locals(b, 0);
    equiv_reorder_vars(&a->outputs, b->outputs);
    diff = list_create_sized(min_count(a->outputs));

    i = 0;
    while (i < min_count(a->outputs)) {
        FitVariable *v1 = a->outputs->items[i];
        FitVariable *v2 = NULL;
        FitEntity *ea, *eb;
        FitList *roots_a = NULL, *blocks_a = NULL, *unique_a = NULL;
        FitList *roots_b = NULL, *blocks_b = NULL, *unique_b = NULL;
        FitBddNode *term_a, *term_b;
        int count_a = 0, count_b = 0;

        if (i < min_count(a->outputs) - 1 && step != 1)
            v2 = a->outputs->items[i + 1];
        ea = equiv_extract_cone(a, i, i + 1, step);
        eb = equiv_extract_cone(b, i, i + 1, step);
        equiv_order_entity(eb);
        if (!list_equal_sets(ea->inputs, eb->inputs, var_same_name) && step == 1) {
            entity_free(&ea);
            entity_free(&eb);
            /* the variable itself, not its name, is entered here; the
             * list is printed with %s below */
            list_add(diff, v1);
            i++;
            continue;
        }
        equiv_reorder_vars(&ea->inputs, eb->inputs);

        bdd_build_entity_500(ea, &roots_a, &blocks_a, &unique_a, &term_a, &count_a,
                             EQUIV_MAX_NODES);
        if (count_a > 0) {
            bdd_build_entity_500(eb, &roots_b, &blocks_b, &unique_b, &term_b, &count_b,
                                 EQUIV_MAX_NODES);
            if (count_b > 0) {
                FitList *d = bdd_compare_roots(roots_a, roots_b, v1, v2);

                list_append_destroy(diff, &d);
            }
        }
        list_free_all(&roots_a, min_free_block_cb);
        bdd_free_blocks(blocks_a);
        bdd_free_unique_table(unique_a);
        if (count_a > 0) {
            list_free_all(&roots_b, min_free_block_cb);
            bdd_free_blocks(blocks_b);
            bdd_free_unique_table(unique_b);
        }
        if (count_a < 0 || count_b < 0) {
            if (g_equiv_verbose)
                fprintf(g_msg, "-> Can't verify functions %s.\n", v1->name);
            if (v2 != NULL && g_equiv_verbose)
                fprintf(g_msg, ", %s.\n", v2->name);
            if (g_equiv_verbose)
                fprintf(g_msg, "\n");
            unverified = 1;
        }
        entity_free(&ea);
        entity_free(&eb);
        i += step;
    }

    if (diff == NULL || diff->count < 1) {
        if (unverified && g_equiv_verbose)
            fprintf(g_msg, "Equivalent systems (but some functions can't be checked).\n");
        list_destroy(&diff);
        return 1;
    }
    if (g_equiv_verbose) {
        fprintf(g_msg, "Non equivalent systems !!!\n");
        fprintf(g_msg, "The following functions are not equivalent :\n");
        for (int k = 0; k < min_count(diff); k++)
            fprintf(g_msg, "%s \n", (const char *)diff->items[k]);
        fprintf(g_msg, "\n");
    }
    list_destroy(&diff);
    return 0;
}
