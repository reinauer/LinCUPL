/*
 * fit_prep.c - preparation of the netlist for the ATF150x fitter:
 * transformations of the netlist before and between fitter passes.
 *
 *   - XOR gates left by XOR detection are turned back into functions
 *     (fit_prep_expand_xor_gates);
 *   - foldback nodes: which local functions may stay as foldback terms
 *     (g_fit_foldback_ids), collapsing the others into their users
 *     (fit_prep_collapse_foldbacks);
 *   - control terms shared between registers get one "Com_Ctrl_<n>" node
 *     (fit_prep_share_ctrl_terms);
 *   - classification of clock / reset / preset / output-enable signals as
 *     global-pin candidates (fit_prep_classify_ctrl and its callers);
 *   - the macrocell plan of a pass (fit_prep_plan): minimisation,
 *     factorisation, splitting of wide functions into "FB_<n>" foldback
 *     nodes until the estimate fits the device.
 *
 * EDIF input (-str ifmt edif) is not supported (fit_prep_edif_keep_inputs).
 *
 * Lists are walked with index pairs (0-based index, 1-based position)
 * because the list package works with 1-based positions.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "fit_types.h"
#include "base.h"
#include "tt.h"
#include "lib.h"
#include "min.h"
#include "syn.h"
#include "place.h"
#include "dev.h"
#include "fit.h"
#include "fit_priv.h"

/* ------------------------------------------------------------------ */
/* File-scope state                                                     */
/* ------------------------------------------------------------------ */

static FitList *g_ctrl_inv_names;      /* names of control sources used inverted */
static FitList *g_ctrl_names;          /* names of control sources */
static FitList *g_ctrl_buffer_fcts;    /* FitFct: buffers between a pin and a control input */
static int      g_fb_substituted;      /* set by fit_prep_subst_foldback */
static int      g_split_estimate;      /* running macrocell estimate of fit_prep_split_wide */

void fit_prep_reset(void)
{
    g_ctrl_inv_names = NULL;
    g_ctrl_names = NULL;
    g_ctrl_buffer_fcts = NULL;
    g_fb_substituted = 0;
    g_split_estimate = 0;
}

/* ------------------------------------------------------------------ */
/* Callback adapters                                                    */
/* ------------------------------------------------------------------ */

static void *copy_var_cb(void *v)
{
    return var_copy((FitVariable *)v);
}

static void free_var_cb(void **ref)
{
    FitVariable *v = (FitVariable *)*ref;

    var_free(&v);
    *ref = v;
}

static void free_pterm_cb(void **ref)
{
    FitPTerm *p = (FitPTerm *)*ref;

    pterm_free(&p);
    *ref = p;
}

static void *copy_pterm_cb(void *p, void *nwords)
{
    return pterm_copy((FitPTerm *)p, FIT_PTR2INT(nwords));
}

int fit_str_ieq_cb(void *a, void *b)
{
    return util_str_ieq((const char *)a, (const char *)b);
}

/* Bounded copy into FitVariable.name. */
static void set_var_name(FitVariable *v, const char *name)
{
    size_t n = strlen(name);

    if (n >= FIT_NAME_MAX)
        n = FIT_NAME_MAX - 1;
    memcpy(v->name, name, n);
    v->name[n] = '\0';
}

/* The lists inputs + outputs + locals joined into one new list of copies of
 * the pointers. */
static FitList *all_vars_shallow(FitEntity *ent)
{
    FitList *vars = list_copy(ent->inputs);
    FitList *a = list_copy(ent->outputs);
    FitList *b = list_copy(ent->locals);

    list_append_destroy(vars, &a);
    list_append_destroy(vars, &b);
    return vars;
}

/* ------------------------------------------------------------------ */

void fit_prep_nop(void)
{
}

void fit_prep_hoist_children(FitEntity *ent, FitFctTree *tree)
{
    int pos;

    if (tree->u.children == NULL)
        return;
    for (pos = tree->u.children->count; pos != 0; pos--) {
        FitFctTree *child = (FitFctTree *)tree->u.children->items[pos - 1];
        FitFctTree *node;
        FitFct *fct;
        int id;

        list_del_shift(tree->u.children, pos);
        id = bdd_new_local_var(ent);
        fct = fct_new();
        fct->var_id = id;
        fct->tree[0] = child;
        ent->n_local++;
        list_add(ent->fcts, fct);
        node = fcttree_new();
        node->sel = FIT_SEL_ID;
        node->u.id = id;
        node->user = FIT_INT2PTR(1);
        list_add(tree->u.children, node);
    }
}

/* signal on the output pin (dir 0) of an unknown gate */
static char *fit_prep_gate_output(FitEntity *ent, FitUnknownGate *gate)
{
    int i;

    (void)ent;
    if (gate != NULL && gate->pins != NULL) {
        for (i = 0; i < LN(gate->pins); i++) {
            FitUnknownPin *pin = (FitUnknownPin *)gate->pins->items[i];

            if (pin->dir == 0)
                return pin->signal;
        }
    }
    return NULL;
}

/* target of the alias `name`, or name itself */
static char *fit_prep_alias_target(FitEntity *ent, char *name)
{
    int i;

    for (i = 0; i < LN(ent->net->aliases); i++) {
        FitAlias *al = (FitAlias *)ent->net->aliases->items[i];

        if (util_str_icmp(al->name, name) == 0 && al->target != NULL)
            return al->target;
    }
    return name;
}

/* a*!b + b*!a for the two inputs of an XOR gate */
static FitFctTree *fit_prep_gate_xor_tree(FitEntity *ent, FitUnknownGate *gate)
{
    FitList *pins, *ins;
    FitFctTree *sum, *t1, *t2, *n1, *n2;
    const char *a, *b;
    int i;

    if (gate == NULL)
        return NULL;
    pins = gate->pins;
    if (pins == NULL)
        return NULL;
    ins = list_create();
    for (i = 0; i < LN(pins); i++) {
        FitUnknownPin *pin = (FitUnknownPin *)pins->items[i];

        if (pin->dir == 1)
            list_add(ins, pin);
    }
    if (ins->count < 2)        /* fewer than two inputs: stop */
        fit_internal_error("fit_prep_gate_xor_tree: XOR gate with fewer than two inputs");
    a = ((FitUnknownPin *)ins->items[0])->signal;
    b = ((FitUnknownPin *)ins->items[1])->signal;

    sum = fcttree_new();
    sum->sel = FIT_SEL_OR;
    sum->u.children = list_create_sized(LN(pins));

    t1 = fcttree_new();
    t1->sel = FIT_SEL_AND;
    t1->u.children = list_create();
    list_add(t1->u.children, fcttree_new_id(entity_var_id(ent, a)));
    n1 = fcttree_new();
    n1->sel = FIT_SEL_NOT;
    n1->u.children = list_create();
    list_add(n1->u.children, fcttree_new_id(entity_var_id(ent, b)));
    list_add(t1->u.children, n1);

    t2 = fcttree_new();
    t2->sel = FIT_SEL_AND;
    t2->u.children = list_create();
    list_add(t2->u.children, fcttree_new_id(entity_var_id(ent, b)));
    n2 = fcttree_new();
    n2->sel = FIT_SEL_NOT;
    n2->u.children = list_create();
    list_add(n2->u.children, fcttree_new_id(entity_var_id(ent, a)));
    list_add(t2->u.children, n2);

    list_add(sum->u.children, t1);
    list_add(sum->u.children, t2);
    list_destroy(&ins);
    return sum;
}

/* replaces every unknown gate named XOR by a function */
static void fit_prep_expand_xor_gates(FitEntity *ent)
{
    int i = 0;
    int pos = 1;

    for (;;) {
        FitList *items = ent->net->items;
        FitNetItem *item;

        if (LN(items) <= i)
            break;
        item = (FitNetItem *)items->items[i];
        if (item->kind == FIT_GATE_UNKNOWN) {
            FitUnknownGate *gate = (FitUnknownGate *)item->cell;

            if (gate->i00 == 1 && strncasecmp(gate->model, "XOR", 3) == 0) {
                FitFctTree *tree = fit_prep_gate_xor_tree(ent, gate);

                if (tree != NULL) {
                    char *out = fit_prep_gate_output(ent, gate);
                    char *target = fit_prep_alias_target(ent, out);
                    int id = entity_var_id(ent, out);
                    FitFct *fct;

                    lib_move_variable(id, ent->inputs, ent->locals);
                    lib_move_variable(id, ent->outputs, ent->locals);
                    lib_alias_remove(ent->net, out);
                    fct = fct_new();
                    fct->var_id = id;
                    fct->tree[0] = tree;
                    list_add(ent->fcts, fct);
                    if (util_str_icmp(out, target) != 0) {
                        int id2 = entity_var_id(ent, out);

                        tt2_substitute_fct(ent, entity_find_fct(ent, id2));
                    }
                    list_del_shift(ent->net->items, pos);
                    free(gate);         /* the gate, not the item */
                    i--;
                    pos--;
                }
            }
        }
        i++;
        pos++;
    }
    lib_outputs_to_locals(ent);
}

/* 1 when the signal v is a buried node (a local, or an alias
 * entry without target that leads to one) */
int fit_prep_var_is_buried(FitEntity *ent, FitVariable *v)
{
    FitList *aliases;
    int k;

    if (entity_is_local(ent, v->id) != 0)
        return 1;
    if (ent->net == NULL || ent->net->aliases == NULL)
        return 0;
    aliases = ent->net->aliases;
    k = list_find(aliases, v->name, alias_name_eq);
    if (k != 0) {
        char *target = ((FitAlias *)aliases->items[k - 1])->target;

        if (target == NULL) {
            FitInstance *inst = netlist_find_instance(ent, v->name);
            char *src;
            int id;

            if (inst == NULL)
                return 1;
            src = inst->d;
            if (src == NULL)
                src = inst->d2;
            id = entity_var_id_by_name(ent, src);
            if (id != 0) {
                FitVariable *v2 = entity_var_by_id(ent, id);
                int id2 = netlist_alias_target_id(ent, v2->name);

                if (id2 != 0)
                    v2 = entity_var_by_id(ent, id2);
                k = list_find(ent->net->aliases, v2->name, alias_name_eq);
                if (k != 0 && ((FitAlias *)ent->net->aliases->items[k - 1])->target == NULL)
                    return 1;
            }
        } else {
            int id = entity_var_id_by_name(ent, target);
            FitVariable *v2 = entity_var_by_id(ent, id);

            k = list_find(ent->net->aliases, v2->name, alias_name_eq);
            if (k != 0)
                return 1;
        }
    }
    return 0;
}

/* moves the operands `sub` of `tree` into a new local function
 * and puts an ID node for it into tree */
void fit_prep_extract_operands(FitEntity *ent, FitFctTree *tree, FitList *sub, int flag)
{
    FitFct *fct = fct_new();
    int id = tt2_next_var_id(ent);
    FitList *outs, *vars, *locs, *created;
    FitFctTree *node;
    int i, n;

    fct->var_id = id;
    if (sub == NULL || sub->count != 1) {
        FitFctTree *t = fcttree_new();

        t->u.children = sub;
        t->sel = tree->sel;
        fct->tree[0] = t;
    } else {
        fct->tree[0] = (FitFctTree *)sub->items[0];
    }
    outs = list_map_copy(ent->outputs, copy_var_cb);
    vars = list_map_copy(ent->inputs, copy_var_cb);
    list_append_destroy(vars, &outs);
    list_destroy(&outs);
    locs = list_map_copy(ent->locals, copy_var_cb);
    list_append_destroy(vars, &locs);
    list_destroy(&locs);
    created = vars_new_generated(1, &id, 2, id, vars);
    list_free_all(&vars, free_var_cb);
    if (ent->locals == NULL)
        ent->locals = list_create();
    list_append_destroy(ent->locals, &created);
    ent->n_local++;
    list_destroy(&created);
    list_add(ent->fcts, fct);
    fct->flags = (flag == 1);

    /* The loop runs over the n slots the list had before it, while it
     * deletes from the list (list_del_insert moves the last item into
     * the hole).  A slot at or past the shrinking count still holds the
     * item moved out of it; that item is then looked up again with
     * list_find.  The order of the remaining items depends on this. */
    n = tree->u.children->count;
    for (i = 0; i < n; i++) {
        void *item;
        int k;

        if (i >= tree->u.children->capacity)
            break;
        item = tree->u.children->items[i];
        if (list_find(sub, item, list_ptr_eq) != 0) {
            k = list_find(tree->u.children, item, list_ptr_eq);
            if (k != 0)
                list_del_insert(tree->u.children, k);
        }
    }
    node = fcttree_new();
    node->sel = FIT_SEL_ID;
    node->user = tree;
    node->u.id = id - 1;
    list_add(tree->u.children, node);
}

/* rewrites *ref as a sum of products over the entity's variables */
void fit_prep_tree_to_sop(FitFctTree **ref, FitEntity *ent)
{
    FitList *vars, *tmp, *pterms;
    FitFctTree *t;

    if (*ref == NULL)
        return;
    vars = list_map_copy(ent->outputs, copy_var_cb);
    tmp = list_map_copy(ent->inputs, copy_var_cb);
    list_append_destroy(vars, &tmp);
    list_destroy(&tmp);
    tmp = list_map_copy(ent->locals, copy_var_cb);
    list_append_destroy(vars, &tmp);
    list_destroy(&tmp);

    t = *ref;
    if (t->sel == FIT_SEL_XOR) {
        /* The two operands are converted through local copies of the
         * pointers: a replaced operand is lost, one rewritten in place
         * stays. */
        FitFctTree *a = (FitFctTree *)t->u.children->items[0];
        FitFctTree *b = (FitFctTree *)t->u.children->items[1];

        fit_prep_tree_to_sop(&a, ent);
        fit_prep_tree_to_sop(&b, ent);
    } else {
        pterms = fcttree_to_pterms3(t, ent->outputs, ent->inputs, ent->locals);
        if (pterms == NULL || pterms->count < 2) {
            FitPTerm *p;

            if (pterms == NULL)     /* no product terms: stop */
                fit_internal_error("fit_prep_tree_to_sop: function without product terms");
            p = pterms->capacity > 0 ? (FitPTerm *)pterms->items[0] : NULL;
            *ref = fcttree_from_pterm(p, vars);
        } else {
            int i;

            (*ref)->sel = FIT_SEL_OR;
            (*ref)->u.children = list_create();
            for (i = 0; i < LN(pterms); i++)
                list_add((*ref)->u.children,
                         fcttree_from_pterm((FitPTerm *)pterms->items[i], vars));
        }
        list_free_all(&pterms, free_pterm_cb);
    }
    list_free_all(&vars, free_var_cb);
}

/* names -> ids; unknown names are reported */
int fit_prep_names_to_ids(FitEntity *ent, FitList *names, FitList **ids)
{
    int i;

    if (names == NULL || names->count == 0)
        return 0;
    *ids = list_create();
    for (i = 0; i < names->count; i++) {
        char *name = (char *)names->items[i];
        int id = entity_var_id_by_name(ent, name);

        if (id == 0)
            fprintf(g_msg, "## The variable name %s does not exist in the circuit.\n", name);
        else
            list_add(*ids, FIT_INT2PTR(id));
    }
    if (*ids == NULL || (*ids)->count == 0)
        list_destroy(ids);
    return 1;
}

/* EDIF input (-str ifmt edif): not supported */
void fit_prep_edif_keep_inputs(FitEntity *ent, int mode)
{
    (void)ent;
    (void)mode;
    fit_unsupported("ifmt edif");
}

/* 1 when fct can be a foldback term: a literal, a product of
 * literals, or a sum of literals (which is turned into the product of the
 * complements and the variable's inversion flag toggled) */
static int fit_prep_is_foldback_form(FitEntity *ent, FitFct *fct)
{
    FitFctTree *t = fct->tree[0];
    int sel = t->sel;
    FitList *l;
    int i;

    if (sel == FIT_SEL_ID || sel == FIT_SEL_NOT)
        return 1;
    if (sel != FIT_SEL_AND && sel != FIT_SEL_OR)
        return 0;
    l = t->u.children;
    for (i = 0; i < LN(l); i++) {
        FitFctTree *c = (FitFctTree *)l->items[i];

        if (c->sel != FIT_SEL_ID
            && (c->sel != FIT_SEL_NOT
                || ((FitFctTree *)c->u.children->items[0])->sel != FIT_SEL_ID))
            break;
    }
    if (LN(l) != i)
        return 0;
    if (sel == FIT_SEL_OR) {
        fct->tree[0] = fcttree_new_not(t);
        fit_prep_tree_to_sop(&fct->tree[0], ent);
        entity_toggle_inv(ent, fct->var_id);
    }
    return 1;
}

/* rebuilds g_fit_foldback_ids from the local functions that have
 * foldback form */
void fit_prep_find_foldbacks(FitEntity *ent)
{
    FitList *xor_ids;
    int i;

    if (g_fit_foldback_ids != NULL && g_fit_foldback_ids->count != 0)
        list_destroy(&g_fit_foldback_ids);
    g_fit_foldback_ids = list_create();
    xor_ids = place_xor_local_ids(ent);
    for (i = 0; i < LN(ent->fcts); i++) {
        FitFct *fct = (FitFct *)ent->fcts->items[i];

        if (entity_is_local(ent, fct->var_id) == 0)
            continue;
        if (list_find(xor_ids, FIT_INT2PTR(fct->var_id), list_ptr_eq) != 0)
            continue;
        if (list_find(g_opt.soft_buffer, FIT_INT2PTR(fct->var_id), list_ptr_eq) != 0
            && list_find(g_opt.foldback_list, FIT_INT2PTR(fct->var_id), list_ptr_eq) == 0)
            continue;
        if (fit_prep_is_foldback_form(ent, fct) != 0)
            list_add(g_fit_foldback_ids, FIT_INT2PTR(fct->var_id));
    }
}

/* substitutes the foldback function fb into *ref wherever it
 * keeps *ref a product of literals */
static int fit_prep_subst_foldback(FitFctTree **ref, FitFct *fb)
{
    FitFctTree *child;
    int i, pos;

    switch ((*ref)->sel) {
    default:
        return 0;
    case FIT_SEL_OR:
        for (pos = 1; ; pos++) {
            FitList *l = (*ref)->u.children;

            if (LN(l) <= pos - 1)
                break;
            child = (FitFctTree *)l->items[pos - 1];
            if (fit_prep_subst_foldback(&child, fb) != 0) {
                list_del_shift((*ref)->u.children, pos);
                list_insert_at((*ref)->u.children, child, pos);
            }
        }
        return 0;
    case FIT_SEL_ID:
        if ((*ref)->u.id == fb->var_id) {
            fcttree_substitute(ref, fb);
            g_fb_substituted = 1;
            return 1;
        }
        return 0;
    case FIT_SEL_AND:
        break;
    }
    for (i = 0; ; i++) {
        FitList *l = (*ref)->u.children;

        if (LN(l) <= i)
            return 0;
        child = (FitFctTree *)l->items[i];
        if (fit_prep_subst_foldback(&child, fb) != 0) {
            int sel = child->sel;

            if (sel == FIT_SEL_AND) {
                int k;

                list_del_shift((*ref)->u.children, i + 1);
                for (k = 0; k < LN(child->u.children); k++)
                    list_add((*ref)->u.children, child->u.children->items[k]);
                return 0;
            }
            if (sel == FIT_SEL_ID || sel == FIT_SEL_NOT) {
                list_del_shift((*ref)->u.children, i + 1);
                list_add((*ref)->u.children, child);
                return 0;
            }
        }
    }
}

/* gives the XOR function `user` a private copy of
 * the foldback function id */
static void fit_prep_dup_foldback(FitEntity *ent, FitFct *user, int id)
{
    FitFct *fb = entity_find_fct(ent, id);
    FitVariable *v = lib_new_variable(ent, 2, 0);
    FitFct *fct = fct_new();

    fct->var_id = v->id;
    fct->tree[0] = fcttree_copy(fb->tree[0], NULL);
    list_add(ent->fcts, fct);
    fcttree_replace_id(user->tree[0], fb->var_id, fct->var_id, FIT_SEL_ID);
}

/* number of uses of id in the other functions */
static int fit_prep_count_uses(FitEntity *ent, int id)
{
    int n = 0;
    int i;

    for (i = 0; i < LN(ent->fcts); i++) {
        FitFct *fct = (FitFct *)ent->fcts->items[i];

        if (fct->var_id != id)
            n += (int)fcttree_count_id(fct->tree[0], id);
    }
    return n;
}

/* substitutes the foldback candidates into their users where
 * that is free, and drops the ones nobody uses any more */
void fit_prep_collapse_foldbacks(FitEntity *ent)
{
    int i = 0;
    int pos = 1;

    fit_prep_find_foldbacks(ent);
    for (;;) {
        FitFct *fb;
        int id, j;

        if (LN(g_fit_foldback_ids) <= i)
            break;
        id = FIT_PTR2INT(g_fit_foldback_ids->items[i]);
        fb = entity_find_fct(ent, id);
        if (fb == NULL)         /* no function: stop */
            fit_internal_error("fit_prep_collapse_foldbacks: foldback id without function");
        for (j = 0; j < LN(ent->fcts); j++) {
            FitFct *fct = (FitFct *)ent->fcts->items[j];

            if (fct->var_id == fb->var_id)
                continue;
            if (list_find(g_opt.xor_list, FIT_INT2PTR(fct->var_id), list_ptr_eq) == 0) {
                fit_prep_subst_foldback(&fct->tree[0], fb);
                if (g_fb_substituted != 0) {
                    fct->tree[0] = domlist_minimise_tree(ent, fct->tree[0]);
                    g_fb_substituted = 0;
                }
            } else {
                FitList *ids = list_create();

                fcttree_collect_ids(fct->tree[0], ids);
                if (ids != NULL && ids->count == 2) {
                    int k;

                    for (k = 0; k < 2; k++) {
                        int in_id = FIT_PTR2INT(ids->items[k]);

                        if (list_find(g_fit_foldback_ids, FIT_INT2PTR(in_id), list_ptr_eq) != 0)
                            fit_prep_dup_foldback(ent, fct, in_id);
                    }
                    list_destroy(&ids);
                }
            }
        }
        if (fit_prep_count_uses(ent, id) == 0) {
            list_del_shift(g_fit_foldback_ids, pos);
            if (list_find(g_opt.foldback_list, FIT_INT2PTR(id), list_ptr_eq) != 0)
                printf("Warning : Some user foldbacks are collapsed\n");
            entity_remove_fct(ent, fb);
            i--;
            pos--;
        }
        i++;
        pos++;
    }
}

/* inlines and removes the local functions that are direct
 * operands of tree */
static void fit_prep_inline_operands(FitEntity *ent, FitFctTree *tree)
{
    int i;

    if (tree->u.children == NULL)
        return;
    i = tree->u.children->count;
    while (i != 0) {
        FitFctTree *c;

        i--;
        if (i >= LN(tree->u.children))      /* the list shrank: skip */
            continue;
        c = (FitFctTree *)tree->u.children->items[i];
        if (c->sel == FIT_SEL_ID) {
            int id = c->u.id;

            if (entity_is_local(ent, id) != 0) {
                FitFct *fct = entity_find_fct(ent, id);

                tt2_substitute_fct(ent, fct);
                entity_remove_fct(ent, fct);
            }
        }
    }
}

/* the functions flatten split up (g_fit_hoisted_ids) are
 * implemented complemented */
static void fit_prep_complement_hoisted(FitEntity *ent)
{
    int i;

    for (i = 0; i < LN(g_fit_hoisted_ids); i++) {
        FitFct *fct = entity_find_fct(ent, FIT_PTR2INT(g_fit_hoisted_ids->items[i]));

        if (fct != NULL) {
            FitFctTree **ref = &fct->tree[0];

            fit_prep_inline_operands(ent, fct->tree[0]);
            *ref = fcttree_new_not(*ref);
            *ref = fcttree_propagate_inv(*ref, 0);
            fit_prep_tree_to_sop(ref, ent);
            entity_var_by_id(ent, fct->var_id)->polarity = -1;
        }
    }
}

/* 1 when the two entities have the same numbers of inputs, outputs and
 * functions; 0 when both are NULL. */
static int fit_prep_same_shape(FitEntity *a, FitEntity *b)
{
    int n;

    if (a == NULL && b == NULL)
        return 0;
    if (a->n_in != b->n_in)
        return 0;
    if (a->n_out != b->n_out)
        return 0;
    n = LN(a->fcts);
    if (b->fcts == NULL)
        return n == 0;
    return n == b->fcts->count;
}

/* the working entity: minimised copy in which each sum is
 * replaced by its complement when that has fewer product terms */
FitEntity *fit_prep_working_copy(FitEntity *ent)
{
    FitEntity *e, *comp;
    int i = 0;

    if (g_opt.ifmt_edif == 0)
        g_fit_hoist_mode = 1;
    e = domlist_minimise_entity(ent, 1, 1, 0x32);
    g_fit_hoist_mode = 0;
    fit_prep_complement_hoisted(e);
    comp = esp_complement_entity(e);
    if (fit_prep_same_shape(e, comp) == 0) {
        entity_free(&comp);
        return e;
    }
    for (;;) {
        FitFct *fct;

        if (LN(e->fcts) <= i) {
            for (i = 0; i < LN(e->locals); i++) {
                FitVariable *v = (FitVariable *)e->locals->items[i];

                if (v->polarity != 1) {
                    v->polarity = 1;
                    entity_toggle_inv(e, entity_var_id_by_name(e, v->name));
                }
            }
            entity_free(&comp);
            return e;
        }
        fct = (FitFct *)e->fcts->items[i];
        if (fct->tree[0]->sel == FIT_SEL_OR) {
            FitFct *cf = entity_find_fct(comp, fct->var_id);
            int sel = cf->tree[0]->sel;
            FitVariable *v;

            if (sel == FIT_SEL_OR
                && LN(fct->tree[0]->u.children) <= LN(cf->tree[0]->u.children))
                goto next;
            if (sel == FIT_SEL_ID || sel == FIT_SEL_NOT) {
                entity_free(&comp);
                return e;
            }
            v = entity_var_by_id(e, fct->var_id);
            if (cf->tree[0]->sel == FIT_SEL_AND
                && esp_signal_is_control(v->name, e) != 0)
                goto next;
            fct->tree[0] = fcttree_copy(cf->tree[0], NULL);
            v = entity_var_by_id(e, fct->var_id);
            v->polarity = (v->polarity != 1) ? 1 : -1;
        }
next:
        i++;
    }
}

/* inlines constants and the locals whose substitution grows the
 * design by less than `gain` literals */
static int fit_prep_inline_small(FitEntity *ent, int gain)
{
    FitList *xor_inputs = list_create();
    int changed = 0;
    int i;

    for (i = 0; i < LN(g_opt.xor_list); i++) {
        FitFct *fct = entity_find_fct(ent, FIT_PTR2INT(g_opt.xor_list->items[i]));

        fcttree_collect_ids(fct->tree[0], xor_inputs);
    }
    i = 0;
    for (;;) {
#define CUR ((FitFct *)ent->fcts->items[i])
        if (LN(ent->fcts) <= i)
            break;
        if (list_find(g_opt.xor_list, FIT_INT2PTR(CUR->var_id), list_ptr_eq) == 0
            && list_find(xor_inputs, FIT_INT2PTR(CUR->var_id), list_ptr_eq) == 0) {
            if (entity_is_local(ent, CUR->var_id) == 0) {
                if (entity_is_output(ent, CUR->var_id) != 0
                    && entity_fct_tree(ent, CUR->var_id)->sel == FIT_SEL_CONST) {
                    tt2_substitute_fct(ent, CUR);
                    changed = 1;
                }
            } else {
                int uses = tt2_count_id_uses(ent, CUR->var_id);
                int leaves = fcttree_count_leaves(CUR->tree[0]);

                if ((leaves - 1) * uses - leaves < gain) {
                    changed = 1;
                    tt2_substitute_fct(ent, CUR);
                    entity_remove_fct(ent, CUR);
                    continue;
                }
            }
        }
#undef CUR
        i++;
    }
    list_destroy(&xor_inputs);
    if (changed != 0)
        ent->i04 = 0;
    return 1;
}

/* nonzero when tree is a literal.  A node that is neither ID,
 * AND, OR nor NOT gives its selector (nonzero). */
static unsigned fit_prep_is_literal(FitEntity *ent, FitFctTree *tree)
{
    unsigned sel = (unsigned)tree->sel;

    (void)ent;
    if (sel == FIT_SEL_ID)
        return 1;
    if (sel == FIT_SEL_AND || sel == FIT_SEL_OR)
        return 0;
    if (sel == FIT_SEL_NOT)
        return ((FitFctTree *)tree->u.children->items[0])->sel == FIT_SEL_ID;
    return sel;
}

/* Control input of a register by the kind code used throughout
 * (fit_prep_find_same_ctrl, fit_drv_count_ctrl_terms): 3 clock, 4 reset,
 * 5 preset, 6 output enable, 7 clock enable. */
static char *ctrl_signal(FitInstance *inst, int kind)
{
    switch (kind) {
    case 3: return inst->clk;
    case 4: return inst->ar;
    case 5: return inst->ap;
    case 6: return inst->oe;
    case 7: return inst->ce;
    }
    return NULL;
}

/* a later register whose control function of the same kind is
 * equivalent to fct */
static FitFct *fit_prep_find_same_ctrl(FitEntity *ent, FitInstance *inst, FitFct *fct,
                                       int index, int kind, int global_oe)
{
    FitList *ids, *ids2;
    int j;

    (void)inst;
    if (fit_prep_is_literal(ent, fct->tree[0]) != 0)
        return NULL;
    ids = list_create();
    fcttree_collect_ids(fct->tree[0], ids);
    for (j = index + 1; j < LN(ent->net->items); j++) {
        FitNetItem *item = (FitNetItem *)ent->net->items->items[j];
        FitVariable *v;
        FitFct *fct2;
        char *name;

        /* Unknown gates and kind 13 cells are not register cells. */
        if (item->kind == FIT_GATE_UNKNOWN || item->kind == FIT_GATE_CELL13)
            continue;
        name = ctrl_signal((FitInstance *)item->cell, kind);
        if (name == NULL)
            continue;
        v = entity_find_var(ent, name);
        if (entity_has_input_id(ent, v->id) != 0
            || (esp_var_is_simple_control(ent, v) != 0 && global_oe == 0))
            continue;
        ids2 = list_create();
        fct2 = entity_find_fct(ent, v->id);
        fcttree_collect_ids(fct2->tree[0], ids2);
        if (list_equal_sets(ids, ids2, list_ptr_eq) != 0) {
            if (latch_trees_equivalent(ent, fct->tree[0], fct2->tree[0]) != 0) {
                if (ids2 != NULL && ids2->count != 0)
                    list_destroy(&ids2);
                list_destroy(&ids);
                return fct2;
            }
            if (ids2 != NULL && ids2->count != 0)
                list_destroy(&ids2);
            /* ids is destroyed here; the following items are compared
             * against the empty (NULL) list */
            list_destroy(&ids);
            continue;
        }
        list_destroy(&ids2);
    }
    list_destroy(&ids);
    return NULL;
}

/* puts the control function `name` of register `inst` into a
 * group of equivalent control functions */
static void fit_prep_group_ctrl(FitEntity *ent, FitInstance *inst, char *name, int index,
                                int kind, FitList *groups)
{
    int global_oe = (g_opt.global_oe != NULL && g_opt.global_oe->count != 0 && kind == 6);
    FitVariable *v = entity_find_var(ent, name);
    FitFct *fct, *twin;
    FitList *group = NULL;
    int i;

    if (entity_has_input_id(ent, v->id) != 0)
        return;
    if (esp_var_is_simple_control(ent, v) != 0 && global_oe == 0)
        return;
    fct = entity_find_fct(ent, v->id);
    twin = fit_prep_find_same_ctrl(ent, inst, fct, index, kind, global_oe);
    if (twin == NULL)
        return;
    for (i = 0; i < LN(groups); i++) {
        FitFct *head;

        group = (FitList *)groups->items[i];
        head = entity_find_fct(ent, FIT_PTR2INT(group->items[0]));
        if (latch_trees_equivalent(ent, twin->tree[0], head->tree[0]) != 0) {
            if (list_find(group, FIT_INT2PTR(fct->var_id), list_ptr_eq) == 0)
                list_add(group, FIT_INT2PTR(fct->var_id));
            if (list_find(group, FIT_INT2PTR(twin->var_id), list_ptr_eq) == 0)
                list_add(group, FIT_INT2PTR(twin->var_id));
            break;
        }
    }
    if (groups == NULL || groups->count == 0 || groups->count <= i) {
        group = list_create();
        if (list_find(group, FIT_INT2PTR(fct->var_id), list_ptr_eq) == 0)
            list_add(group, FIT_INT2PTR(fct->var_id));
        if (list_find(group, FIT_INT2PTR(twin->var_id), list_ptr_eq) == 0)
            list_add(group, FIT_INT2PTR(twin->var_id));
        list_add(groups, group);
    }
}

/* one "Com_Ctrl_<id>" node per group; the members become
 * buffers of it */
static void fit_prep_make_ctrl_nodes(FitEntity *ent, FitList *groups)
{
    int in_xor = 0;         /* not reset between groups */
    int gi;

    for (gi = 0; gi < LN(groups); gi++) {
        FitList *group = (FitList *)groups->items[gi];
        int id = tt2_next_var_id(ent);
        FitVariable *v = var_new(0);
        FitFct *first, *fct;
        FitFctTree *idnode;
        int j;

        set_var_name(v, util_str_with_index("Com_Ctrl_", id));
        v->id = id;
        list_add(ent->locals, v);
        ent->n_local = LN(ent->locals);
        first = entity_find_fct(ent, FIT_PTR2INT(group->items[0]));
        fct = fct_new();
        fct->var_id = id;
        fct->tree[0] = fcttree_copy(first->tree[0], NULL);
        idnode = fcttree_new_id(id);
        for (j = 0; j < LN(ent->fcts); j++) {
            FitFct *member = (FitFct *)ent->fcts->items[j];

            if (list_find(group, FIT_INT2PTR(member->var_id), list_ptr_eq) != 0) {
                in_xor = list_find(g_opt.xor_list, FIT_INT2PTR(member->var_id), list_ptr_eq);
                if (in_xor != 0)
                    list_del_shift(g_opt.xor_list, in_xor);
                member->tree[0] = fcttree_copy(idnode, NULL);
            }
        }
        list_add(ent->fcts, fct);
        if (in_xor != 0)
            list_add(g_opt.xor_list, FIT_INT2PTR(fct->var_id));
    }
}

/* shares equivalent control functions (clock, reset, preset,
 * clock enable, output enable) between registers */
void fit_prep_share_ctrl_terms(FitEntity *ent)
{
    FitList *g[5];
    static const int kinds[5] = { 3, 4, 5, 7, 6 };
    int i, k;

    if (ent->net == NULL || ent->net->items == NULL)
        return;
    for (k = 0; k < 5; k++)
        g[k] = list_create();
    for (i = 0; i < LN(ent->net->items); i++) {
        FitNetItem *item = (FitNetItem *)ent->net->items->items[i];
        int kind = item->kind;

        if (kind == 3 || kind == 8 || kind == 4 || kind == 2) {
            FitInstance *inst = (FitInstance *)item->cell;

            for (k = 0; k < 5; k++) {
                char *name = ctrl_signal(inst, kinds[k]);

                if (name != NULL)
                    fit_prep_group_ctrl(ent, inst, name, i, kinds[k], g[k]);
            }
        }
    }
    for (k = 0; k < 5; k++)
        if (g[k] != NULL && g[k]->count != 0)
            fit_prep_make_ctrl_nodes(ent, g[k]);
    for (k = 0; k < 5; k++)
        if (g[k] != NULL)
            list_destroy(&g[k]);
}

/* a control function that is a sum of literals becomes the
 * complement of a new foldback node "FB_<id>"; returns the node's id */
static int fit_prep_ctrl_sum_to_foldback(FitEntity *ent, FitFct *fct)
{
    FitVariable *v;
    FitFct *nf;
    FitFctTree *t;
    int i, pos, id;

    if (fct->tree[0]->sel != FIT_SEL_OR)
        return 0;
    for (i = 0; i < LN(fct->tree[0]->u.children); i++)
        if (fit_prep_is_literal(ent, (FitFctTree *)fct->tree[0]->u.children->items[i]) == 0)
            return 0;
    for (pos = 1; ; pos++) {
        FitList *l = fct->tree[0]->u.children;

        if (LN(l) <= pos - 1)
            break;
        t = fcttree_new_not((FitFctTree *)l->items[pos - 1]);
        fit_prep_tree_to_sop(&t, ent);
        list_del_shift(fct->tree[0]->u.children, pos);
        list_insert_at(fct->tree[0]->u.children, t, pos);
    }
    id = tt2_next_var_id(ent);
    v = var_new(0);
    set_var_name(v, util_str_with_index("FB_", id));
    v->id = id;
    list_add(ent->locals, v);
    nf = fct_new();
    t = fcttree_copy(fct->tree[0], NULL);
    nf->tree[0] = t;
    t->sel = FIT_SEL_AND;
    nf->var_id = v->id;
    fct->tree[0] = fcttree_new_not(fcttree_new_id(v->id));
    list_add(ent->fcts, nf);
    list_add(g_fit_foldback_ids, FIT_INT2PTR(nf->var_id));
    return v->id;
}

/* re-normalises every function that uses id */
static void fit_prep_renormalise_users(FitEntity *ent, int id)
{
    int i;

    for (i = 0; i < LN(ent->fcts); i++) {
        FitFct *fct = (FitFct *)ent->fcts->items[i];
        FitList *ids = list_create();

        fcttree_collect_ids(fct->tree[0], ids);
        if (list_find(ids, FIT_INT2PTR(id), list_ptr_eq) != 0)
            fit_prep_tree_to_sop(&fct->tree[0], ent);
        list_destroy(&ids);
    }
}

/* the five control inputs of one register */
static void fit_prep_ctrl_sums_of_instance(FitEntity *ent, FitInstance *inst)
{
    static const int kinds[5] = { 3, 4, 5, 7, 6 };
    int k;

    for (k = 0; k < 5; k++) {
        char *name = ctrl_signal(inst, kinds[k]);
        FitVariable *v;

        if (name == NULL)
            continue;
        v = entity_find_var(ent, name);
        if (entity_has_input_id(ent, v->id) == 0) {
            FitFct *fct = entity_find_fct(ent, v->id);
            int id = fit_prep_ctrl_sum_to_foldback(ent, fct);

            if (id != 0) {
                tt2_substitute_fct(ent, fct);
                fit_prep_renormalise_users(ent, id);
            }
        }
    }
}

/* control functions that are sums of literals go through a
 * foldback node, so that the control term is a single product term */
static void fit_prep_ctrl_sums_to_foldbacks(FitEntity *ent)
{
    int i;

    if (ent->net == NULL || ent->net->items == NULL)
        return;
    for (i = 0; i < LN(ent->net->items); i++) {
        FitNetItem *item = (FitNetItem *)ent->net->items->items[i];
        int kind = item->kind;

        if (kind == 3 || kind == 2 || kind == 8 || kind == 4)
            fit_prep_ctrl_sums_of_instance(ent, (FitInstance *)item->cell);
    }
}

/* implements fct through foldback nodes.  The function (or, with
 * use_alt, its complement alt) is taken as a sum; each product that is not
 * a literal becomes a foldback node FB_<n>, and the sum becomes the product
 * of the complements.  Returns the number of foldback nodes added. */
static int fit_prep_split_into_foldbacks(FitEntity *ent, FitFct *fct, FitFctTree *alt,
                                         int use_alt, int keep_fct)
{
    FitFctTree **ref;
    FitVariable *v;
    FitFct *nf;
    int n_literals = 0;
    int before, base, i, pos, id;

    if (g_fit_foldback_ids == NULL)
        g_fit_foldback_ids = list_create();
    before = LN(g_fit_foldback_ids);
    if (use_alt == 1)
        fct->tree[0] = alt;
    ref = &fct->tree[0];
    fit_prep_tree_to_sop(ref, ent);
    base = tt2_next_var_id(ent);
    (*ref)->sel = FIT_SEL_AND;
    i = 0;
    pos = 1;
    for (;;) {
        FitList *l = (*ref)->u.children;
        FitFctTree *c, *t;

        if (LN(l) <= i)
            break;
        c = (FitFctTree *)l->items[i];
        fit_prep_tree_to_sop(&c, ent);
        if (fit_prep_is_literal(ent, c) == 0) {
            v = var_new(0);
            id = base - 1 + pos;
            set_var_name(v, util_str_with_index("FB_", id));
            v->id = id;
            list_add(ent->locals, v);
            nf = fct_new();
            nf->var_id = id;
            nf->tree[0] = fcttree_copy(c, NULL);
            list_add(ent->fcts, nf);
            list_add(g_fit_foldback_ids, FIT_INT2PTR(nf->var_id));
            t = fcttree_new_not(fcttree_new_id(nf->var_id));
            list_del_shift((*ref)->u.children, pos);
            list_insert_at((*ref)->u.children, t, pos);
        } else {
            n_literals++;
            t = fcttree_new_not(c);
            fit_prep_tree_to_sop(&t, ent);
            list_del_shift((*ref)->u.children, pos);
            list_insert_at((*ref)->u.children, t, pos);
        }
        i++;
        pos++;
    }
    if (use_alt == 0) {
        if (keep_fct == 0) {
            v = var_new(0);
            id = i + base;
            set_var_name(v, util_str_with_index("FB_", id));
            v->id = id;
            list_add(ent->locals, v);
            nf = fct_new();
            nf->var_id = id;
            nf->tree[0] = *ref;
            list_add(ent->fcts, nf);
            list_add(g_fit_foldback_ids, FIT_INT2PTR(nf->var_id));
            *ref = fcttree_new_not(fcttree_new_id(nf->var_id));
        } else {
            entity_toggle_inv(ent, fct->var_id);
        }
    }
    if (n_literals == LN((*ref)->u.children) || keep_fct == 1)
        list_add(g_fit_foldback_ids, FIT_INT2PTR(fct->var_id));
    ent->n_local = LN(ent->locals);
    return LN(g_fit_foldback_ids) - before;
}

/* is the complement of tree at most one non-literal product
 * larger than tree?  Then *out is the minimised complement. */
static int fit_prep_complement_is_smaller(FitEntity *ent, FitFctTree *tree, FitFctTree **out)
{
    FitFctTree *plain = fcttree_copy(tree, NULL);
    int n_plain = LN(plain->u.children);
    FitFctTree *neg = fcttree_new_not(fcttree_copy(tree, NULL));
    FitFctTree *min, *c;
    int n_neg, i;

    fit_prep_tree_to_sop(&neg, ent);
    min = domlist_minimise_tree(ent, neg);
    n_neg = LN(min->u.children);
    for (i = 0; i < LN(min->u.children); i++) {
        c = (FitFctTree *)min->u.children->items[i];
        fit_prep_tree_to_sop(&c, ent);
        if (fit_prep_is_literal(ent, c) != 0)
            n_neg--;
    }
    for (i = 0; i < LN(plain->u.children); i++) {
        c = (FitFctTree *)plain->u.children->items[i];
        fit_prep_tree_to_sop(&c, ent);
        if (fit_prep_is_literal(ent, c) != 0)
            n_plain--;
    }
    if (n_plain + 1 < n_neg) {
        *out = NULL;
        return 0;
    }
    *out = fcttree_copy(min, NULL);
    return 1;
}

/* splits fct into foldback nodes in the polarity with fewer product
 * terms. */
static void fit_prep_split_best_polarity(FitEntity *ent, FitFct *fct, int keep_fct)
{
    FitFctTree *alt;

    if (fit_prep_complement_is_smaller(ent, fct->tree[0], &alt) == 0)
        fit_prep_split_into_foldbacks(ent, fct, alt, 0, keep_fct);
    else
        fit_prep_split_into_foldbacks(ent, fct, alt, 1, keep_fct);
}

/* clears FitFct.flags of every function. */
static void fit_prep_clear_fct_flags(FitEntity *ent)
{
    int i;

    for (i = 0; i < LN(ent->fcts); i++)
        ((FitFct *)ent->fcts->items[i])->flags = 0;
}

/* does tree mention the variable id? */
static int fit_prep_tree_uses(FitEntity *ent, int id, FitFctTree *tree)
{
    int i;

    switch (tree->sel) {
    case FIT_SEL_NOT:
    case FIT_SEL_AND:
    case FIT_SEL_OR:
    case FIT_SEL_EQ:
    case FIT_SEL_XOR:
        for (i = 0; i < LN(tree->u.children); i++)
            if (fit_prep_tree_uses(ent, id, (FitFctTree *)tree->u.children->items[i]) != 0)
                return 1;
        break;
    case FIT_SEL_ID:
        return id == tree->u.id;
    default:
        break;
    }
    return 0;
}

/* does fct depend on its own output? */
static int fit_prep_fct_is_recursive(FitEntity *ent, FitFct *fct)
{
    return fit_prep_tree_uses(ent, fct->var_id, fct->tree[0]);
}

/* new foldback node "FB_<id>" = product of `operands` (the list
 * is used, not copied) */
static FitFct *fit_prep_new_foldback(FitEntity *ent, FitList *operands)
{
    FitFctTree *t = fcttree_new();
    FitVariable *v;
    FitFct *fct;

    t->sel = FIT_SEL_AND;
    t->u.children = operands;
    v = lib_new_variable(ent, 2, 0);
    snprintf(v->name, FIT_NAME_MAX, "%s_%d", "FB", v->id);
    fct = fct_new();
    fct->var_id = v->id;
    fct->tree[0] = t;
    list_add(ent->fcts, fct);
    list_add(g_fit_foldback_ids, FIT_INT2PTR(fct->var_id));
    return fct;
}

/* 1 when the quotient is not a constant */
static int fit_prep_quotient_usable(FitEntity *ent, FitList *quotient)
{
    FitList *vars = all_vars_shallow(ent);
    FitFctTree *t = fcttree_from_pterms(quotient, vars);

    list_destroy(&vars);
    return tt2_tree_is_const(t) == 0;
}

/* for a sum, moves its literals (complemented) into a new foldback node;
 * returns the rewritten copy or NULL */
static FitFctTree *fit_prep_hoist_literals(FitEntity *ent, FitFct *fct)
{
    FitList *lits = list_create();
    FitFctTree *copy, *c, *t;
    int i, pos;

    if (fct->tree[0]->sel != FIT_SEL_OR)
        return NULL;
    copy = fcttree_copy(fct->tree[0], NULL);
    i = 0;
    pos = 1;
    for (;;) {
        if (LN(copy->u.children) <= i)
            break;
        c = (FitFctTree *)copy->u.children->items[i];
        fit_prep_tree_to_sop(&c, ent);
        if (fit_prep_is_literal(ent, c) != 0) {
            t = fcttree_new_not(c);
            fit_prep_tree_to_sop(&t, ent);
            list_del_shift(copy->u.children, pos);
            list_add(lits, t);
            i--;
            pos--;
        }
        i++;
        pos++;
    }
    if (lits != NULL && lits->count > 1) {
        FitFct *fb = fit_prep_new_foldback(ent, lits);

        t = fcttree_new_not(fcttree_new_id(fb->var_id));
        list_add(copy->u.children, t);
        return copy;
    }
    fcttree_free_deferred(&copy);
    list_destroy(&lits);
    return NULL;
}

/* rewrites fct as quotient * D + remainder with the divisor D in a new
 * node */
static int fit_prep_apply_division(FitEntity *ent, FitFct *fct, FitList *quotient,
                                   FitList *divisor, FitList *remainder)
{
    FitList *vars = all_vars_shallow(ent);
    FitFctTree *dt, *qt, *rt, *dnode = NULL;
    FitVariable *v;
    FitFct *nf;
    int is_sum = 0;
    int hoisted = 0;

    dt = domlist_minimise_tree(ent, fcttree_from_pterms(divisor, vars));
    if (dt->sel == FIT_SEL_CONST) {
        list_destroy(&vars);
        return 0;
    }
    v = lib_new_variable(ent, 2, 0);
    nf = fct_new();
    nf->var_id = v->id;
    nf->tree[0] = dt;
    if (dt->sel == FIT_SEL_OR)
        is_sum = 1;
    if (fit_prep_is_foldback_form(ent, nf) == 0) {
        dnode = fit_prep_hoist_literals(ent, nf);
        if (dnode == NULL) {
            list_del_shift_free(ent->locals, LN(ent->locals), free_var_cb);
            fct_free(&nf);
            return 0;
        }
        hoisted = 1;
        list_del_shift_free(ent->locals, LN(ent->locals) - 1, free_var_cb);
        fct_free(&nf);
    } else {
        list_add(g_fit_foldback_ids, FIT_INT2PTR(nf->var_id));
        list_add(ent->fcts, nf);
    }
    qt = fcttree_from_pterms(quotient, vars);
    rt = fcttree_from_pterms(remainder, vars);
    if (hoisted == 0) {
        if (is_sum == 1)
            dnode = fcttree_new_not(fcttree_new_id(v->id));
        else
            dnode = fcttree_new_id(v->id);
    }
    fct->tree[0] = fcttree_new_binary(FIT_SEL_OR, fcttree_new_binary(FIT_SEL_AND, qt, dnode), rt);
    fit_prep_tree_to_sop(&fct->tree[0], ent);
    list_destroy(&vars);
    return 1;
}

/* one algebraic division of fct by its best kernel */
static int fit_prep_divide_once(FitEntity *ent, FitFct *fct)
{
    FitFctTree *tree = fct->tree[0];
    FitList *vars = all_vars_shallow(ent);
    int nvars = LN(vars);
    int nwords = nvars != 0 ? (int)(((unsigned)nvars - 1u) >> 5) + 1 : 1;
    FitList *pterms, *funcs, *kernels, *hash, *indices;
    FitList *quotient = NULL, *remainder = NULL, *divisor;
    FitFctTree *best;
    int best_cost, k;

    pterms = fcttree_to_pterms3(tree, ent->inputs, ent->outputs, ent->locals);
    funcs = list_create();
    list_add(funcs, pterms);
    kernels = list_create();
    hash = list_create();
    indices = list_create();
    list_add(indices, FIT_INT2PTR(0));
    factor_find_kernels(funcs, nvars, 0, 1, &kernels, &hash, indices);
    list_destroy(&funcs);
    list_destroy(&hash);
    list_destroy(&indices);
    if (kernels == NULL || kernels->count == 0) {
        list_destroy(&kernels);
        return 0;
    }
    divisor = *(FitList **)kernels->items[0];
    factor_divide(pterms, divisor, nwords, &quotient, &remainder, 0);
    if (remainder == NULL || remainder->count == 0) {
        if (LN(pterms) == LN(divisor)) {
            if (fit_prep_quotient_usable(ent, quotient) == 0)
                return 0;
        }
    }
    best = domlist_minimise_tree(ent, fcttree_from_pterms(divisor, vars));
    best_cost = LN(remainder) + LN(quotient);
    for (k = 1; k < LN(kernels); k++) {
        FitList *d = *(FitList **)kernels->items[k];
        FitList *q = NULL, *r = NULL;
        int nq, nr;

        factor_divide(pterms, d, nwords, &q, &r, 0);
        nq = LN(q);
        nr = LN(r);
        if (nr + nq < best_cost || best->sel == FIT_SEL_CONST) {
            quotient = list_map_copy_x(q, copy_pterm_cb, FIT_INT2PTR(nwords));
            divisor = list_map_copy_x(d, copy_pterm_cb, FIT_INT2PTR(nwords));
            remainder = list_map_copy_x(r, copy_pterm_cb, FIT_INT2PTR(nwords));
            best = domlist_minimise_tree(ent, fcttree_from_pterms(divisor, vars));
            best_cost = nr + nq;
        }
    }
    list_destroy(&vars);
    list_destroy(&kernels);
    return fit_prep_apply_division(ent, fct, quotient, divisor, remainder) != 0;
}

/* divides fct until it has at most `limit` product terms */
static void fit_prep_divide_down(FitEntity *ent, FitFct *fct, int n_terms, int limit)
{
    int k = LN(fct->tree[0]->u.children);
    int extra = n_terms - k;
    int ok;

    do {
        if (k + extra <= limit)
            return;
        ok = fit_prep_divide_once(ent, fct);
        k = LN(fct->tree[0]->u.children);
    } while (ok != 0);
}

/* factorises the functions that need more than one macrocell */
static void fit_prep_divide_wide(FitEntity *ent)
{
    FitList *inputs;
    int i;

    fit_prep_clear_fct_flags(ent);
    inputs = list_create();
    for (i = 0; i < LN(g_fit_work_fcts); i++) {
        FitFct *fct = (FitFct *)g_fit_work_fcts->items[i];
        FitVariable *v = entity_var_by_id(ent, fct->var_id);
        int n;

        if (fit_prep_fct_is_recursive(ent, fct) != 0)
            continue;
        n = fit_drv_count_mc_terms(ent, fct, v, inputs);
        if (n <= 5)
            continue;
        if (list_find(g_opt.xor_list, FIT_INT2PTR(v->id), list_ptr_eq) != 0) {
            FitFct *part = fit_drv_xor_wide_operand(ent, fct);
            FitList *l = part->tree[0]->u.children;

            n = (n - LN(l)) - 1;
            fit_prep_divide_down(ent, part, LN(l), 4 - n);
            continue;
        }
        fit_prep_divide_down(ent, fct, n, 5);
    }
}

/* reduces fct to at most `limit` product terms by moving
 * literals and products into foldback nodes; returns the estimated number
 * of macrocells saved */
static int fit_prep_split_fct(FitEntity *ent, FitFct *fct, int limit)
{
    FitFctTree **ref = &fct->tree[0];
    FitFctTree *saved, *c, *t;
    FitList *lits;
    FitFct *fb;
    int i, pos, n, r;

    if ((*ref)->u.children != NULL && (*ref)->u.children->count == 1) {
        r = 0;
        if (entity_is_local(ent, fct->var_id) != 0) {
            /* A local function of one product is added to
             * g_fit_foldback_ids only while that list is empty; with
             * entries in it there is no comparator to search it with, and
             * the fitter stops with an internal error. */
            if (LN(g_fit_foldback_ids) != 0)
                fit_internal_error("fit_prep_split_fct: list search without comparator");
            list_add(g_fit_foldback_ids, FIT_INT2PTR(fct->var_id));
            r = 1;
        }
        return r;
    }
    lits = list_create();
    saved = fcttree_copy(*ref, NULL);
    i = 0;
    pos = 1;
    for (;;) {
        if (LN((*ref)->u.children) <= i)
            break;
        c = (FitFctTree *)(*ref)->u.children->items[i];
        fit_prep_tree_to_sop(&c, ent);
        if (fit_prep_is_literal(ent, c) != 0) {
            t = fcttree_new_not(c);
            fit_prep_tree_to_sop(&t, ent);
            list_del_shift((*ref)->u.children, pos);
            list_add(lits, t);
            i--;
            pos--;
        }
        i++;
        pos++;
    }
    n = LN((*ref)->u.children);
    if (n == 0) {
        if (entity_is_local(ent, fct->var_id) == 0) {
            fb = fit_prep_new_foldback(ent, lits);
            *ref = fcttree_new_not(fcttree_new_id(fb->var_id));
            r = LN(lits) / 5 - 1;
        } else {
            (*ref)->sel = FIT_SEL_AND;
            (*ref)->u.children = lits;
            entity_toggle_inv(ent, fct->var_id);
            list_add(g_fit_foldback_ids, FIT_INT2PTR(fct->var_id));
            r = LN(lits) / 5;
        }
        if (LN(lits) % 5 != 0)
            r++;
        fcttree_free_deferred(&saved);
        return r;
    }
    if (limit < LN(lits) + n) {
        while (LN(lits) < limit) {
            if (LN((*ref)->u.children) == 0)    /* no product left: stop */
                fit_internal_error("fit_prep_split_fct: no product left to move");
            c = (FitFctTree *)(*ref)->u.children->items[0];
            fb = fit_prep_new_foldback(ent, c->u.children);
            t = fcttree_new_not(fcttree_new_id(fb->var_id));
            list_del_shift((*ref)->u.children, 1);
            list_add(lits, t);
        }
        fb = fit_prep_new_foldback(ent, lits);
        t = fcttree_new_not(fcttree_new_id(fb->var_id));
        list_add((*ref)->u.children, t);
        fcttree_free_deferred(&saved);
        return 1;
    }
    fcttree_free_deferred(ref);
    *ref = saved;
    if (entity_is_local(ent, fct->var_id) != 0) {
        fit_prep_split_best_polarity(ent, fct, 1);
        return 1;
    }
    if (esp_signal_is_control(entity_var_by_id(ent, fct->var_id)->name, ent) == 0)
        return 0;
    fit_prep_split_best_polarity(ent, fct, 0);
    return 1;
}

/* while the macrocell estimate n_mc exceeds the device, splits
 * the functions whose term count modulo 5 is 1, 2, 3, 4 and then 0 */
static void fit_prep_split_wide(FitEntity *ent, int n_mc)
{
    FitList *inputs;
    int round = 1;
    int r, j;

    fit_prep_clear_fct_flags(ent);
    inputs = list_create();
    g_split_estimate = n_mc;
    if (n_mc <= g_dev->n_macrocells)
        goto done;
again:
    if (round >= 5)
        goto done;
    r = round;
    for (j = 0; j < LN(g_fit_work_fcts); j++) {
        FitFct *fct = (FitFct *)g_fit_work_fcts->items[j];
        FitVariable *v = entity_var_by_id(ent, fct->var_id);

        if (fit_prep_fct_is_recursive(ent, fct) == 0) {
            int n = fit_drv_count_mc_terms(ent, fct, v, inputs);
            int rem = r % 5;

            r = round;
            if (n % 5 == rem
                && (n > 5 || entity_is_local(ent, v->id) != 0
                    || esp_signal_is_control(v->name, ent) != 0)
                && list_find(g_opt.xor_list, FIT_INT2PTR(v->id), list_ptr_eq) == 0) {
                if (round == 0)
                    round = 5;
                g_split_estimate -= fit_prep_split_fct(ent, fct, round + 1);
                r = round;
                if (g_split_estimate <= g_dev->n_macrocells)
                    break;
                if (g_dev->n_macrocells <= LN(g_fit_foldback_ids))
                    break;
            }
        }
    }
    if (r == 0)
        goto done;
    round = r + 1;
    if (round == 5) {
        if (g_split_estimate <= g_dev->n_macrocells)
            goto done;
        round = 0;
    }
    if (g_split_estimate <= g_dev->n_macrocells)
        goto done;
    goto again;
done:
    fit_prep_collapse_foldbacks(ent);
}

/* is `name` the output of a kind 2 (T flip-flop) cell? */
static int fit_prep_is_tff_output(FitEntity *ent, const char *name)
{
    int i;

    if (ent->net == NULL || ent->net->items == NULL)
        return 0;
    for (i = 0; i < LN(ent->net->items); i++) {
        FitNetItem *item = (FitNetItem *)ent->net->items->items[i];

        if (item->kind == FIT_GATE_TFF
            && util_str_icmp(name, ((FitInstance *)item->cell)->q) == 0)
            return 1;
    }
    return 0;
}

/* candidates for XOR detection when xor_synthesis is on without
 * a list: the plain combinational outputs, and the locals that need more
 * than one macrocell or exactly two product terms */
static FitList *fit_prep_xor_candidates(FitEntity *ent)
{
    FitList *res = list_create();
    FitList *inputs;
    int i;

    g_fit_global_names = list_create();
    for (i = 0; i < ent->n_out; i++) {
        FitVariable *v = (FitVariable *)ent->outputs->items[i];

        if (fit_prep_is_tff_output(ent, v->name) == 0
            && netlist_has_name(ent, v->name) == 0
            && esp_var_is_simple_control(ent, v) == 0
            && list_find(g_opt.xor_list, FIT_INT2PTR(v->id), list_ptr_eq) == 0) {
            FitFct *fct = entity_find_fct(ent, v->id);

            inputs = list_create();
            fit_drv_count_mc_terms(ent, fct, v, inputs);
            list_destroy(&inputs);
            list_add(res, FIT_INT2PTR(fct->var_id));
        }
    }
    for (i = 0; i < ent->n_local; i++) {
        FitVariable *v = (FitVariable *)ent->locals->items[i];

        if (list_find(g_opt.xor_list, FIT_INT2PTR(v->id), list_ptr_eq) == 0) {
            FitFct *fct = entity_find_fct(ent, v->id);
            int n;

            inputs = list_create();
            n = fit_drv_count_mc_terms(ent, fct, v, inputs);
            list_destroy(&inputs);
            if (n > 5 || n == 2)
                list_add(res, FIT_INT2PTR(fct->var_id));
        }
    }
    list_destroy(&g_fit_global_names);
    return res;
}

/* XOR detection for the listed outputs, or (n_listed == 0, flag
 * xor_synthesis) for the candidates of fit_prep_xor_candidates */
void fit_prep_detect_xor(FitEntity *ent, int n_listed)
{
    FitList *saved = NULL;
    FitEntity *work;

    if (n_listed == 0) {
        FitList *cand = fit_prep_xor_candidates(ent);

        if (cand == NULL || cand->count == 0)
            return;
        saved = g_opt.xor_list;
        g_opt.xor_list = cand;
    }
    fprintf(g_msg, "Xor Detection ...\n");
    work = entity_copy(ent);
    xor_detect_entity(&work);
    entity_update_counts(work);
    entity_clear(ent);
    entity_shallow_assign(work, ent);
    if (g_opt.xor_list != NULL && g_opt.xor_list->count != 0) {
        fit_prep_expand_xor_gates(ent);
        work = entity_simplified_copy(ent);
        if (FIT_EXT) {
            /* the extended mode also propagates the inversions of the simplified
             * entity */
            FitEntity *inv = entity_copy_inv_propagated(work);

            entity_clear(ent);
            entity_shallow_assign(inv, ent);
            entity_free(&work);
        } else {
            entity_clear(ent);
            entity_shallow_assign(work, ent);
        }
    }
    if (n_listed == 0) {
        FitList *all = list_union(g_opt.xor_list, saved, list_ptr_eq);

        list_destroy(&g_opt.xor_list);
        list_destroy(&saved);
        g_opt.xor_list = all;
    }
}

/* classifies the control signal *name_ref of a register.
 * When the signal is an output that only buffers (pol 1) or inverts
 * (pol -1) a source, returns 1 and
 *   - if the source is an input pin: replaces *name_ref by the source name
 *     (so the register is fed from the pin directly) and remembers the
 *     buffer function for removal;
 *   - if the source is a buried node: *pol = 2.
 * The source name is entered into g_ctrl_names / g_ctrl_inv_names, from
 * which the list of global-pin candidates (g_fit_must_global) is made.
 * The tests and the insertions use the two lists asymmetrically; this is
 * deliberate, the choice of global pins depends on it. */
static int fit_prep_classify_ctrl(FitEntity *ent, char **name_ref, int *pol)
{
    FitFct *fct;
    FitFctTree *t;
    FitVariable *v;
    FitList *lst;
    char *nm;
    int id, src;

    if (*name_ref == NULL)
        return 0;
    id = var_list_id_of_name(ent->outputs, *name_ref);
    if (id == 0)
        return 0;
    *pol = 1;
    fct = entity_find_fct(ent, id);
    t = fct->tree[0];
    if (t->sel == FIT_SEL_ID) {
        int buried;

        src = t->u.id;
        v = entity_var_by_id(ent, src);
        nm = v->name;
        if (latch_find_alias(nm, ent->net) == 0) {
            if (list_find(g_ctrl_inv_names, nm, fit_str_ieq_cb) != 0
                && list_find(g_opt.twoclock, nm, fit_str_ieq_cb) == 0)
                return 0;
            if (entity_has_input_id(ent, src) != 0) {
                *name_ref = util_strdup(nm);
                if (list_find(g_ctrl_buffer_fcts, FIT_INT2PTR(id), fct_id_eq) == 0)
                    list_add(g_ctrl_buffer_fcts, fct);
                lst = g_ctrl_names;
                goto tail;
            }
            buried = fit_prep_var_is_buried(ent, v);
        } else {
            if (list_find(ent->outputs, FIT_INT2PTR(src), var_id_eq) != 0)
                goto is_output;
            buried = fit_prep_var_is_buried(ent, v);
        }
        if (buried != 0)
            *pol = 2;
        if (list_find(g_ctrl_names, nm, fit_str_ieq_cb) == 0)
            list_add(g_ctrl_names, nm);
        return 1;
    }
    if (t->sel != FIT_SEL_NOT)
        return 0;
    if (((FitFctTree *)t->u.children->items[0])->sel != FIT_SEL_ID)
        return 0;
    src = ((FitFctTree *)t->u.children->items[0])->u.id;
    v = entity_var_by_id(ent, src);
    nm = v->name;
    if (latch_find_alias(nm, ent->net) == 0) {
        if (list_find(g_ctrl_names, nm, fit_str_ieq_cb) != 0
            && list_find(g_opt.twoclock, nm, fit_str_ieq_cb) == 0)
            return 0;
        if (entity_has_input_id(ent, src) == 0) {
            if (fit_prep_var_is_buried(ent, v) != 0)
                *pol = 2;
            if (list_find(g_ctrl_inv_names, nm, fit_str_ieq_cb) == 0)
                list_add(g_ctrl_names, nm);
            return 1;
        }
        *name_ref = util_strdup(nm);
        *pol = -1;
        if (list_find(g_ctrl_buffer_fcts, FIT_INT2PTR(id), fct_id_eq) == 0)
            list_add(g_ctrl_buffer_fcts, fct);
        if (list_find(g_ctrl_inv_names, nm, fit_str_ieq_cb) == 0)
            list_add(g_ctrl_inv_names, nm);
        return 1;
    }
    if (list_find(ent->outputs, FIT_INT2PTR(src), var_id_eq) != 0) {
is_output:
        if (esp_var_is_simple_control(ent, v) != 0)
            tt2_substitute_fct(ent, entity_find_fct(ent, src));
        return 0;
    }
    if (fit_prep_var_is_buried(ent, v) != 0)
        *pol = 2;
    lst = g_ctrl_inv_names;
tail:
    if (list_find(lst, nm, fit_str_ieq_cb) == 0)
        list_add(g_ctrl_names, nm);
    return 1;
}

/* a conflict of the control terms of a register; the extended mode writes it to the
 * report as well */
static void fit_prep_conflict(const char *fmt, const char *name)
{
    fprintf(g_msg, fmt, name);
    if (FIT_EXT && g_fit_report != NULL)
        fprintf(g_fit_report, fmt, name);
}

/* first check of the registers: a register with preset and
 * output enable, or with clock and clock enable, needs one of the two from
 * a pin or node.  Creates the classification lists and g_fit_must_global.
 * Returns 0 after an error message. */
int fit_prep_check_ctrl_conflicts(FitEntity *ent)
{
    FitNetlist *net;
    int i = 0;
    int pol = 0;

    g_ctrl_buffer_fcts = list_create();
    g_ctrl_inv_names = list_create();
    g_ctrl_names = list_create();
    net = ent->net;
    if (net == NULL)
        return 1;
    for (;;) {
        FitNetItem *item;
        FitInstance *inst;
        int kind, id;

        if (LN(net->items) <= i)
            break;
        item = (FitNetItem *)net->items->items[i];
        kind = item->kind;
        if (kind != 3 && kind != 2 && kind != 4) {
            i++;
            continue;
        }
        inst = (FitInstance *)item->cell;
        if (inst->ap != NULL && inst->oe != NULL) {
            if (fit_prep_classify_ctrl(ent, &inst->oe, &pol) == 0) {
                id = var_list_id_of_name(ent->outputs, inst->oe);
                if (id != 0
                    && tt2_tree_is_const(entity_find_fct(ent, id)->tree[0]) == 0) {
                    fit_prep_conflict(FIT_EXT ? "# ERROR : Conflict between Set an Output Enable for PTerm use"
                                                  " for signal : %s.\n"
                                                : "# ERROR : Conflict between Set an Output Enable for Ptrem use"
                                                  " for signal : %s.\n", inst->inst_name);
                    return 0;
                }
            } else {
                inst->oe_pol = pol;
            }
        }
        if (inst->clk == NULL || inst->ce == NULL) {
            i++;
            continue;
        }
        if (fit_prep_classify_ctrl(ent, &inst->clk, &pol) == 0) {
            id = var_list_id_of_name(ent->outputs, inst->clk);
            if (id == 0) {
                i++;
                continue;
            }
            if (tt2_tree_is_const(entity_find_fct(ent, id)->tree[0]) == 0) {
                fit_prep_conflict(FIT_EXT ? "# Warning : Conflict between Clock and Clock Enable for PTerm use"
                                              " for signal : %s.\n"
                                            : "# ERROR : Conflict between Clock an Clock Enable for Ptrem use"
                                              " for signal : %s.\n", inst->inst_name);
                return 0;
            }
            i++;
        } else {
            if (pol != -1) {
                if (pol == 2) {
                    fit_prep_conflict(FIT_EXT ? "# ERROR : Conflict between Clock and Clock Enable for PTerm"
                                                  " use for signal : %s.\n"
                                                : "# ERROR : Conflict between Clock an Clock Enable for Ptrem"
                                                  " use for signal : %s.\n", inst->inst_name);
                    return 0;
                }
                i++;
                continue;
            }
            inst->clk_kind = (item->kind == 3) ? 2 : 3;
            i++;
        }
    }
    g_fit_must_global = list_union(g_ctrl_inv_names, g_ctrl_names, fit_str_ieq_cb);
    return 1;
}

/* classifies clock, reset, output enable and preset of every
 * register, then removes the buffer functions that are no longer needed */
void fit_prep_classify_registers(FitEntity *ent)
{
    int i;
    int pol = 0;

    if (ent == NULL || ent->net == NULL || ent->net->items == NULL)
        return;
    for (i = 0; i < LN(ent->net->items); i++) {
        FitNetItem *item = (FitNetItem *)ent->net->items->items[i];
        int kind = item->kind;

        if (kind == 3 || kind == 2 || kind == 8 || kind == 4) {
            FitInstance *inst = (FitInstance *)item->cell;

            if (fit_prep_classify_ctrl(ent, &inst->clk, &pol) != 0 && pol == -1)
                inst->clk_kind = (item->kind == 3) ? 2 : 3;
            if (fit_prep_classify_ctrl(ent, &inst->ar, &pol) != 0)
                inst->ar_pol = pol;
            if (fit_prep_classify_ctrl(ent, &inst->oe, &pol) != 0)
                inst->oe_pol = pol;
            if (fit_prep_classify_ctrl(ent, &inst->ap, &pol) != 0)
                inst->ap_pol = pol;
        }
    }
    for (i = 0; i < LN(g_ctrl_buffer_fcts); i++) {
        FitFct *fct = (FitFct *)g_ctrl_buffer_fcts->items[i];
        FitVariable *v = var_list_find_id(ent->outputs, fct->var_id);

        if (esp_signal_is_control(v->name, ent) == 0) {
            lib_alias_remove(ent->net, v->name);
            entity_remove_fct(ent, fct);
        }
    }
    list_destroy(&g_ctrl_buffer_fcts);
    list_destroy(&g_ctrl_inv_names);
    list_destroy(&g_ctrl_names);
}

/* has fct the form (a*b) + (c*d)? */
static int fit_prep_is_xor_form(FitEntity *ent, FitFct *fct)
{
    FitFctTree *t = fct->tree[0];
    FitFctTree *c;
    FitList *l;

    (void)ent;
    if (t->sel != FIT_SEL_OR)
        return 0;
    l = t->u.children;
    if (l == NULL || l->count != 2)
        return 0;
    c = (FitFctTree *)l->items[0];
    if (c->sel != FIT_SEL_AND)
        return 0;
    if (c->u.children == NULL || c->u.children->count != 2)
        return 0;
    c = (FitFctTree *)l->items[1];
    if (c->sel != FIT_SEL_AND)
        return 0;
    if (c->u.children == NULL || c->u.children->count != 2)
        return 0;
    return 1;
}

/* drops from the XOR list the functions that use id */
static void fit_prep_drop_xor_users(FitEntity *ent, int id)
{
    int i = 0;
    int pos = 1;

    for (;;) {
        FitFct *fct;

        if (LN(g_opt.xor_list) <= i)
            break;
        fct = entity_find_fct(ent, FIT_PTR2INT(g_opt.xor_list->items[i]));
        if (fcttree_count_id(fct->tree[0], id) != 0) {
            list_del_shift(g_opt.xor_list, pos);
            i--;
            pos--;
        }
        i++;
        pos++;
    }
}

/* keeps in the XOR list only functions that still have XOR form
 * and whose operands are not used elsewhere */
static void fit_prep_filter_xor_list(FitEntity *ent)
{
    FitList *ids;
    int i = 0;
    int pos = 1;
    int j;

    for (;;) {
        FitFct *fct;

        if (LN(g_opt.xor_list) <= i)
            break;
        fct = entity_find_fct(ent, FIT_PTR2INT(g_opt.xor_list->items[i]));
        if (fct == NULL)        /* no function: stop */
            fit_internal_error("fit_prep_filter_xor_list: XOR output without function");
        if (fit_prep_is_xor_form(ent, fct) == 0) {
            list_del_shift(g_opt.xor_list, pos);
            i--;
            pos--;
        }
        i++;
        pos++;
    }
    ids = place_xor_local_ids(ent);
    for (j = 0; j < LN(ids); j++) {
        int id = FIT_PTR2INT(ids->items[j]);
        int uses = tt2_count_id_uses(ent, id);

        if (uses > 2 && place_count_xor_uses(ent, id) < uses)
            fit_prep_drop_xor_users(ent, id);
    }
}

/* of two versions of the design keeps the one that needs fewer
 * macrocells (a when strictly fewer) and frees the other */
static FitEntity *fit_prep_pick_smaller(FitEntity *a, FitEntity *b)
{
    FitList *xor_start = list_copy(g_opt.xor_list);
    FitList *xor_a, *fb_a;
    int n_a, n_b;

    fit_prep_filter_xor_list(a);
    fit_prep_find_foldbacks(a);
    n_a = alloc_build_fct_list(a, place_xor_local_ids(a));
    xor_a = g_opt.xor_list;
    fb_a = list_copy(g_fit_foldback_ids);
    g_opt.xor_list = list_copy(xor_start);
    fit_prep_filter_xor_list(b);
    fit_prep_find_foldbacks(b);
    n_b = alloc_build_fct_list(b, place_xor_local_ids(b));
    if (n_a < n_b) {
        entity_free(&b);
        g_fit_foldback_ids = fb_a;
        g_opt.xor_list = xor_a;
        return a;
    }
    entity_free(&a);
    return b;
}

/* minimised copy of ent; when it needs more macrocells than the
 * device has, the factorised version is tried and the smaller one kept */
static FitEntity *fit_prep_reduce(FitEntity *ent, int criterion, int target)
{
    FitList *xor_start = list_copy(g_opt.xor_list);
    FitEntity *work = entity_copy(ent);
    FitEntity *min, *fact, *fmin;

    fit_prep_inline_small(work, 1);
    min = domlist_minimise_entity(work, 1, 1, 0x32);
    entity_free(&work);
    fit_prep_filter_xor_list(min);
    fit_prep_collapse_foldbacks(min);
    if (g_dev->n_macrocells < alloc_build_fct_list(min, place_xor_local_ids(min))) {
        fact = factor_entity(min, target, criterion, 1, 1, 0);
        fit_drv_inline_ctrl_products(fact);
        fmin = domlist_minimise_entity(fact, 1, 1, 0x32);
        entity_free(&fact);
        fit_prep_filter_xor_list(fmin);
        fit_prep_collapse_foldbacks(fmin);
        g_opt.xor_list = list_copy(xor_start);
        return fit_prep_pick_smaller(fmin, min);
    }
    return min;
}

/* the better of the reduced design and the design as it is */
static FitEntity *fit_prep_optimise(FitEntity *ent, int criterion)
{
    FitList *xor_start = list_copy(g_opt.xor_list);
    FitEntity *reduced, *best;
    char *cycle = NULL;

    if (tt2_check_cycles(ent, &cycle) == 0) {
        reduced = entity_copy(ent);
        fit_prep_collapse_foldbacks(reduced);
    } else {
        reduced = fit_prep_reduce(ent, criterion, 1);
    }
    g_opt.xor_list = list_copy(xor_start);
    best = fit_prep_pick_smaller(reduced, entity_copy(ent));
    fit_prep_collapse_foldbacks(best);
    return best;
}

/* orders the user's foldback list by logic depth and reports
 * names that have no function */
static void fit_prep_sort_user_foldbacks(FitEntity *ent)
{
    FitList *fcts;
    int i;

    if (g_opt.foldback_list == NULL || g_opt.foldback_list->count == 0)
        return;
    fcts = list_create();
    for (i = 0; i < LN(g_opt.foldback_list); i++) {
        int depth = 1;
        int id = FIT_PTR2INT(g_opt.foldback_list->items[i]);
        FitFct *fct = entity_find_fct(ent, id);

        if (fct == NULL) {
            FitVariable *v = entity_var_by_id(ent, id);

            if (v != NULL)
                fprintf(g_fit_report, "Warning : %s is not a foldback.\n", v->name);
            continue;
        }
        g_depth_visited = list_create();
        list_add(g_depth_visited, FIT_INT2PTR(fct->var_id));
        fcttree_depth(ent, fct->tree[0], &depth);
        list_destroy(&g_depth_visited);
        fct->flags = depth;
        list_add(fcts, fct);
    }
    list_sort(fcts, 0, route_fct_weight_cmp);
    list_destroy(&g_opt.foldback_list);
    g_opt.foldback_list = list_create();
    for (i = 0; i < LN(fcts); i++)
        list_add(g_opt.foldback_list, FIT_INT2PTR(((FitFct *)fcts->items[i])->var_id));
    list_destroy(&fcts);
}

/* with logic doubling, a T flip-flop without
 * clock enable whose output is buried is rebuilt as a D flip-flop with
 * its own output as clock enable and the inverted data input */
static void fit_prep_tff_to_dff(FitEntity *ent, FitNetItem *item)
{
    FitInstance *inst = (FitInstance *)item->cell;
    FitVariable *v;
    FitFct *fct;
    int id;

    if (inst->d == NULL)
        return;
    if (inst->q != NULL) {
        inst->ce = inst->q;
        inst->ce_pol = 1;
    }
    id = entity_var_id(ent, inst->d);
    v = lib_new_variable(ent, 1, 1);
    inst->q = util_strdup(v->name);
    fct = fct_new();
    fct->var_id = v->id;
    fct->tree[0] = fcttree_new_not(fcttree_new_id(id));
    list_add(ent->fcts, fct);
    item->kind = 3;
}

/* first step of the driver; only does something with logic
 * doubling on */
void fit_prep_normalise(FitEntity *ent)
{
    int i;

    if (g_opt.logic_doubling == 0 || ent == NULL || ent->net == NULL
        || ent->net->items == NULL)
        return;
    for (i = 0; i < LN(ent->net->items); i++) {
        FitNetItem *item = (FitNetItem *)ent->net->items->items[i];

        if (item->kind == FIT_GATE_TFF && ((FitInstance *)item->cell)->ce == NULL) {
            FitVariable *v = entity_find_var(ent, ((FitInstance *)item->cell)->q);

            if (fit_prep_var_is_buried(ent, v) != 0
                && entity_find_fct(ent, v->id)->tree[0]->sel != FIT_SEL_OR)
                fit_prep_tff_to_dff(ent, item);
        }
    }
}

/* makes the soft-buffer (and user foldback) nodes look like
 * outputs so that the reduction keeps them */
static void fit_prep_expose_soft_nodes(FitEntity *ent)
{
    int i, pos;

    if (g_opt.foldback_list != NULL) {
        fit_prep_collapse_foldbacks(ent);
        i = 0;
        pos = 1;
        for (;;) {
            int id;

            if (LN(g_opt.foldback_list) <= i)
                break;
            id = FIT_PTR2INT(g_opt.foldback_list->items[i]);
            if (list_find(g_fit_foldback_ids, FIT_INT2PTR(id), list_ptr_eq) == 0) {
                FitVariable *v = entity_var_by_id(ent, id);

                if (v != NULL)
                    fprintf(g_fit_report, "## Warning : %s is not foldback\n", v->name);
                list_del_shift(g_opt.foldback_list, pos);
                i--;
                pos--;
            }
            i++;
            pos++;
        }
        g_opt.soft_buffer = list_union(g_opt.foldback_list, g_opt.soft_buffer, list_ptr_eq);
    }
    i = 0;
    pos = 1;
    for (;;) {
        FitVariable *v;

        if (LN(g_opt.soft_buffer) <= i)
            return;
        v = entity_var_by_id(ent, FIT_PTR2INT(g_opt.soft_buffer->items[i]));
        if (v != NULL) {
            if (list_find(ent->locals, FIT_INT2PTR(v->id), var_id_eq) == 0) {
                list_del_shift(g_opt.soft_buffer, pos);
                continue;
            }
            lib_move_variable(v->id, ent->locals, ent->outputs);
            ent->n_out++;
            ent->n_local--;
            if (list_find(ent->inputs, FIT_INT2PTR(v->id), var_id_eq) == 0) {
                list_add(ent->inputs, var_copy(v));
                ent->n_in++;
            }
        }
        i++;
        pos++;
    }
}

/* undoes fit_prep_expose_soft_nodes */
static void fit_prep_hide_soft_nodes(FitEntity *ent)
{
    int i;

    for (i = 0; i < LN(g_opt.soft_buffer); i++) {
        FitVariable *v = entity_var_by_id(ent, FIT_PTR2INT(g_opt.soft_buffer->items[i]));

        if (v != NULL) {
            int k;

            lib_move_variable(v->id, ent->outputs, ent->locals);
            ent->n_out--;
            ent->n_local++;
            k = list_find(ent->inputs, FIT_INT2PTR(v->id), var_id_eq);
            if (k != 0) {
                list_del_shift(ent->inputs, k);
                ent->n_in--;
            }
        }
    }
    if (g_opt.foldback_list != NULL)
        fit_prep_collapse_foldbacks(ent);
}

/* a control signal must be a sum of products of its own; a
 * product goes into a foldback node, anything else is inlined */
static void fit_prep_fix_ctrl_outputs(FitEntity *ent)
{
    int need_min = 0;
    int need_collapse = 0;
    int i;

    for (i = 0; i < LN(ent->outputs); i++) {
        FitVariable *v = (FitVariable *)ent->outputs->items[i];
        FitFct *fct;

        if (esp_signal_is_control(v->name, ent) == 0)
            continue;
        fct = entity_find_fct(ent, v->id);
        if (fct->tree[0]->sel == FIT_SEL_OR)
            continue;
        if (fct->tree[0]->sel == FIT_SEL_AND) {
            FitVariable *nv = lib_new_variable(ent, 2, 0);
            FitFct *nf = fct_new();

            need_collapse = 1;
            nf->var_id = nv->id;
            nf->tree[0] = fcttree_copy(fct->tree[0], NULL);
            fct->tree[0] = fcttree_new_id(nv->id);
            list_add(ent->fcts, nf);
            ent->n_local = LN(ent->locals);
            list_add(g_fit_foldback_ids, FIT_INT2PTR(nf->var_id));
            flatten_replace_id(v->id, nv->id, ent);
            continue;
        }
        need_min = 1;
        tt2_substitute_fct(ent, fct);
    }
    if (need_min != 0) {
        FitEntity *min = domlist_minimise_entity(ent, 1, 1, 0x32);

        entity_clear(ent);
        entity_shallow_assign(min, ent);
    }
    if (need_collapse != 0)
        fit_prep_collapse_foldbacks(ent);
}

/* does the product-of-sums skeleton of tree mention id? */
static int fit_prep_tree_has_operand(FitFctTree *tree, int id)
{
    int i;

    switch (tree->sel) {
    default:
        return 0;
    case FIT_SEL_AND:
    case FIT_SEL_OR:
        for (i = 0; i < LN(tree->u.children); i++)
            if (fit_prep_tree_has_operand((FitFctTree *)tree->u.children->items[i], id) != 0)
                return 1;
        return 0;
    case FIT_SEL_ID:
        return tree->u.id == id;
    }
}

/* is id used uncomplemented by some function? */
static int fit_prep_used_plain(FitEntity *ent, int id)
{
    int i;

    for (i = 0; i < LN(ent->fcts); i++)
        if (fit_prep_tree_has_operand(((FitFct *)ent->fcts->items[i])->tree[0], id) != 0)
            return 1;
    return 0;
}

/* user foldbacks that are plain buffers or inverters are
 * complemented when somebody uses them uncomplemented */
static void fit_prep_fix_user_foldbacks(FitEntity *ent)
{
    int changed = 0;
    int i = 0;
    int pos = 1;

    fit_prep_sort_user_foldbacks(ent);
    for (;;) {
        FitFct *fct;
        int id;

        if (LN(g_opt.foldback_list) <= i)
            break;
        id = FIT_PTR2INT(g_opt.foldback_list->items[i]);
        if (list_find(g_fit_foldback_ids, FIT_INT2PTR(id), list_ptr_eq) == 0) {
            fct = entity_find_fct(ent, id);
            if (fct == NULL) {
                list_del_shift(g_opt.foldback_list, pos);
                continue;
            }
            if ((fct->tree[0]->sel == FIT_SEL_ID || fct->tree[0]->sel == FIT_SEL_NOT)
                && fit_prep_used_plain(ent, id) != 0) {
                fct->tree[0] = fcttree_new_not(fct->tree[0]);
                fit_prep_tree_to_sop(&fct->tree[0], ent);
                entity_toggle_inv(ent, fct->var_id);
                changed = 1;
            }
        }
        i++;
        pos++;
    }
    if (changed != 0)
        fit_prep_collapse_foldbacks(ent);
}

/* the macrocell plan of a pass.  *used receives the ids of the
 * XOR-operand locals (place_xor_local_ids), *n_mc the macrocell estimate.
 * With optimize on and an estimate above the device, the design is reduced
 * (once per run: g_fit_plan_done), then wide functions are divided and
 * split into foldback nodes; if that is not enough the same is tried from
 * the state before, with the other factorisation criterion. */
void fit_prep_plan(FitEntity *ent, FitList **used, int *n_mc)
{
    FitEntity *saved, *best;
    int n, n1;

    fit_prep_fix_user_foldbacks(ent);
    fit_prep_fix_ctrl_outputs(ent);
    if (g_opt.foldback_list != NULL)
        fit_prep_collapse_foldbacks(ent);
    *used = place_xor_local_ids(ent);
    n = alloc_build_fct_list(ent, *used);
    if (g_opt.optimize == 0) {
        *n_mc = n;
        return;
    }
    if (g_fit_plan_done != 0 && n <= g_dev->n_macrocells) {
        *n_mc = n;
        return;
    }
    saved = entity_copy(ent);
    if (g_fit_plan_done == 0) {
        g_fit_plan_done = 1;
        fit_prep_expose_soft_nodes(ent);
        best = fit_prep_optimise(ent, 1);
        entity_clear(ent);
        entity_shallow_assign(best, ent);
        fit_prep_ctrl_sums_to_foldbacks(ent);
        *used = place_xor_local_ids(ent);
        fit_prep_hide_soft_nodes(ent);
    }
    n = alloc_build_fct_list(ent, *used);
    if (n <= g_dev->n_macrocells) {
        *n_mc = n;
        return;
    }
    fit_prep_expose_soft_nodes(ent);
    fit_prep_divide_wide(ent);
    n = alloc_build_fct_list(ent, *used);
    fit_prep_split_wide(ent, n);
    fit_prep_hide_soft_nodes(ent);
    *used = place_xor_local_ids(ent);
    n1 = alloc_build_fct_list(ent, *used);
    if (n1 <= g_dev->n_macrocells) {
        *n_mc = n1;
        return;
    }
    fit_prep_expose_soft_nodes(saved);
    saved = fit_prep_optimise(saved, 0);
    entity_clear(ent);
    entity_shallow_assign(saved, ent);
    fit_prep_ctrl_sums_to_foldbacks(ent);
    *used = place_xor_local_ids(ent);
    fit_prep_hide_soft_nodes(ent);
    n = alloc_build_fct_list(ent, *used);
    if (n <= g_dev->n_macrocells) {
        *n_mc = n;
        return;
    }
    fit_prep_expose_soft_nodes(ent);
    fit_prep_divide_wide(ent);
    n = alloc_build_fct_list(ent, *used);
    fit_prep_split_wide(ent, n);
    fit_prep_hide_soft_nodes(ent);
    *used = place_xor_local_ids(ent);
    n = alloc_build_fct_list(ent, *used);
    if (g_dev->n_macrocells < n && n1 < n) {
        *n_mc = n1;
        return;
    }
    *n_mc = n;
}
