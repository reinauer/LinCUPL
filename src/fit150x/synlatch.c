/*
 * synlatch.c - latch_synth module of the ATF150x fitter: the netlist
 * utilities the fitter uses, and the entry of the latch detection.
 *
 * latch_synthesis (-str Latch_Synthesis ON) turns feed-throughs whose
 * function holds the output while an enable is off into latches.
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
#include "dev.h"

#define LCOUNT(l) ((l) != NULL ? (l)->count : 0)

FitList *g_latch_tristate_names;

/* a new variable (kind 0) named SF<id> with the next free id of
 * the entity.  It is not entered into the entity. */
static FitVariable *latch_new_sf_var(FitEntity *entity)
{
    char buf[36];
    int id = tt2_next_var_id(entity);
    FitVariable *v = var_new(0);

    snprintf(buf, sizeof buf, "SF%d", id);
    strcpy(v->name, buf);
    v->id = id;
    return v;
}

int latch_trees_equivalent(FitEntity *entity, FitFctTree *a, FitFctTree *b)
{
    FitEntity *e1 = entity_new(NULL);
    FitEntity *e2 = entity_new(NULL);
    FitFct *f;
    FitVariable *v;
    FitList *ids1, *ids2;
    int i;

    e1->outputs = list_create_sized(1);
    e2->outputs = list_create_sized(1);
    e1->n_local = 0;
    e2->n_local = 0;
    e1->locals = NULL;
    e2->locals = NULL;
    e1->fcts = list_create_sized(1);
    e2->fcts = list_create_sized(1);
    e1->net = NULL;
    e2->i30 = 0;       /* net is cleared only in e1, i30 only in e2 */

    f = fct_new();
    v = latch_new_sf_var(entity);
    list_add(e1->outputs, v);
    f->var_id = v->id;
    f->tree[0] = fcttree_copy(a, NULL);
    list_add(e1->fcts, f);

    f = fct_new();
    v = latch_new_sf_var(entity);
    list_add(e2->outputs, v);
    f->var_id = v->id;
    f->tree[0] = fcttree_copy(b, NULL);
    list_add(e2->fcts, f);

    ids1 = list_create();
    fcttree_collect_ids(a, ids1);
    e1->inputs = list_create_sized(LCOUNT(ids1));
    ids2 = list_create();
    fcttree_collect_ids(b, ids2);
    e2->inputs = list_create_sized(LCOUNT(ids2));

    for (i = 0; i < LCOUNT(ids1); i++)
        list_add(e1->inputs, entity_var_by_id(entity, FIT_PTR2INT(ids1->items[i])));
    e1->n_in = LCOUNT(e1->inputs);
    for (i = 0; i < LCOUNT(ids2); i++)
        list_add(e2->inputs, entity_var_by_id(entity, FIT_PTR2INT(ids2->items[i])));
    e2->n_in = LCOUNT(e2->inputs);

    list_destroy(&ids1);
    list_destroy(&ids2);
    /* e1 and e2 are not released (their inputs are the variables of
     * `entity` itself, so entity_free could not be used). */
    return equiv_verify_entities(e1, e2, 1);
}

/* the complement of a copy of t: the operand of a negation, or the
 * negation of t */
static FitFctTree *latch_complement(FitFctTree *t)
{
    if (t == NULL)
        return NULL;
    if (t->sel == FIT_SEL_NOT)
        return fcttree_copy(t->u.children->items[0], NULL);
    return fcttree_new_not(fcttree_copy(t, NULL));
}

/* the enable literal of a product of two factors, one of them the
 * variable `id`: the other factor.  NULL when the product has another
 * shape or contains the complement of `id`. */
static FitFctTree *latch_product_enable(FitFctTree *t, int id)
{
    FitFctTree *enable = NULL;
    int found = 0;
    int i;

    if (t->sel != FIT_SEL_AND || t->u.children == NULL || t->u.children->count != 2)
        return NULL;
    for (i = 0; i < 2; i++) {
        FitFctTree *c = t->u.children->items[i];

        if (c->sel == FIT_SEL_NOT) {
            FitFctTree *inner = c->u.children->items[0];

            if (inner->sel != FIT_SEL_ID || inner->u.id == id)
                return NULL;
            enable = c;
        } else if (c->sel == FIT_SEL_ID && c->u.id == id) {
            found = id;
        } else {
            enable = c;
        }
    }
    if (enable == NULL || found == 0)
        return NULL;
    return enable;
}

/* the enable literals of the products of the sum fct, one per product
 * that passes the data input `id`; NULL when the function is no sum */
static FitList *latch_enables(FitFct *fct, int id)
{
    FitFctTree *t = fct->tree[0];
    FitList *res;
    int i;

    if (t->sel != FIT_SEL_OR)
        return NULL;
    res = list_create();
    for (i = 0; i < LCOUNT(t->u.children); i++) {
        FitFctTree *e = latch_product_enable(t->u.children->items[i], id);

        if (e != NULL)
            list_add(res, e);
    }
    return res;
}

/* replaces the variable `id` in t by the constant `value` */
static void latch_set_constant(FitFctTree *t, int id, int value)
{
    int i;

    for (;;) {
        switch (t->sel) {
        case FIT_SEL_NOT:
            t = t->u.children->items[0];
            continue;
        case FIT_SEL_AND:
        case FIT_SEL_OR:
            for (i = 0; i < LCOUNT(t->u.children); i++)
                latch_set_constant(t->u.children->items[i], id, value);
            return;
        case FIT_SEL_ID:
            if (t->u.id == id) {
                t->sel = FIT_SEL_CONST;
                t->u.value = value;
            }
            return;
        default:
            return;
        }
    }
}

/* The feed-through `it` becomes a latch when its function has the form
 * (data & enable) + (!enable & rest): the function becomes rest and a new
 * variable carrying the latch enable is added.  Returns 1 when it was
 * converted. */
static int latch_convert(FitEntity *entity, FitNetItem *it)
{
    FitInstance *inst = it->cell;
    int q_id = entity_var_id_by_name(entity, inst->q);
    int d_id = entity_var_id_by_name(entity, inst->d);
    FitFct *fct = entity_find_fct(entity, q_id);
    FitFctTree *work, *hold, *off, *on, *whole;
    FitList *enables;
    FitVariable *v;
    FitFct *ef;
    char *src;
    int i;

    fct->tree[0] = domlist_minimise_tree(entity, fct->tree[0]);
    work = fcttree_copy(fct->tree[0], NULL);
    enables = latch_enables(fct, d_id);
    if (enables == NULL || enables->count == 0)
        return 0;
    /* the function with every enable off */
    for (i = 0; i < enables->count; i++) {
        FitFctTree *e = enables->items[i];

        if (e->sel != FIT_SEL_ID)
            latch_set_constant(work, ((FitFctTree *)e->u.children->items[0])->u.id, 1);
        else
            latch_set_constant(work, e->u.id, 0);
    }
    hold = domlist_minimise_tree(entity, work);
    if (enables->count < 2) {
        off = latch_complement(enables->items[0]);
    } else {
        off = fcttree_new();
        off->sel = FIT_SEL_AND;
        off->u.children = list_create_sized(enables->count);
        for (i = 0; i < enables->count; i++)
            list_add(off->u.children, latch_complement(enables->items[i]));
    }
    on = latch_complement(off);
    whole = fcttree_new_binary(FIT_SEL_OR,
                               fcttree_new_binary(FIT_SEL_AND, fcttree_new_id(d_id), on),
                               fcttree_new_binary(FIT_SEL_AND, off, hold));
    if (latch_trees_equivalent(entity, fct->tree[0], whole) == 0)
        return 0;

    it->kind = FIT_GATE_LATCH;
    fcttree_free_deferred(&fct->tree[0]);
    fct->tree[0] = hold;
    v = lib_new_variable(entity, 1, 1);
    ef = fct_new();
    ef->var_id = v->id;
    ef->tree[0] = off;
    list_add(entity->fcts, ef);
    inst->clk = util_strdup(v->name);
    inst->clk_kind = 4;
    src = latch_signal_source(entity, inst->d);
    if (util_str_icmp(src, inst->d) != 0)
        lib_alias_set_target(entity->net, inst->d, src);
    return 1;
}

void latch_synthesis(FitEntity **entity)
{
    FitEntity *e = *entity;
    int i;

    if (e->net == NULL)
        return;
    for (i = 0; i < LCOUNT(e->net->items); i++) {
        FitNetItem *it = e->net->items->items[i];

        if (it->kind == FIT_GATE_WIRE && latch_convert(e, it) != 0) {
            fprintf(g_msg, "LATCH FOUND ... \n");
            if (FIT_EXT && g_fit_report != NULL)
                fprintf(g_fit_report, "LATCH FOUND ... \n");
        }
    }
}

int latch_find_buffer_of(FitEntity *entity, int id)
{
    FitList *fcts = entity->fcts;
    int i;

    for (i = 0; i < LCOUNT(fcts); i++) {
        FitFct *f = fcts->items[i];
        FitFctTree *t = f->tree[0];

        if (t->sel == FIT_SEL_ID && t->u.id == id)
            return f->var_id;
    }
    return 0;
}

int latch_find_alias(const char *name, FitNetlist *net)
{
    if (net == NULL)
        return 0;
    if (net->aliases == NULL)
        return 0;
    {
        /* the comparison does not modify the name; the list package takes void * keys */
        union { const char *c; void *v; } key;

        key.c = name;
        return list_find(net->aliases, key.v, tt2_name_ref_equal);
    }
}

char *latch_signal_source(FitEntity *entity, char *name)
{
    FitNetlist *net = entity->net;
    FitInstance *inst;
    FitAlias *al;
    char *src;
    int kind;
    int pos;

    if (net == NULL)
        return name;
    if (latch_find_alias(name, net) == 0)
        return name;
    inst = netlist_find_instance(entity, name);
    if (inst == NULL) {
        inst = tt3_find_instance_by_input(entity, name, &kind);
        if (inst == NULL)
            return name;
    }
    src = inst->pin;
    if (src == NULL) {
        src = inst->d;
        if (src == NULL)
            src = inst->d2;
    }
    pos = list_find(net->aliases, src, alias_name_eq);
    if (pos == 0)
        return src;   /* src has no alias entry */
    al = net->aliases->items[pos - 1];
    if (al->target != NULL && latch_find_alias(al->target, net) == 0)
        return al->target;
    return src;
}

void latch_remove_buffers(FitEntity *entity)
{
    FitNetlist *net = entity->net;
    int i, pos;

    if (net == NULL)
        return;
    pos = 1;
    for (i = 0; i < LCOUNT(net->items); i++, pos++) {
        FitNetItem *it = net->items->items[i];
        FitInstance *inst;
        FitVariable *vq, *vs;
        FitFct *f;
        char *src;
        int src_id, q_id, d_id, p;

        if (it->kind != FIT_GATE_WIRE)
            continue;
        inst = it->cell;
        src = latch_signal_source(entity, inst->d);
        src_id = entity_var_id_by_name(entity, src);
        p = list_find(entity->fcts, FIT_INT2PTR(src_id), fct_id_eq);
        if (p != 0) {
            f = entity->fcts->items[p - 1];
            list_del_shift(entity->fcts, p);
            fct_free(&f);
        }
        q_id = entity_var_id_by_name(entity, inst->q);
        f = entity_find_fct(entity, q_id);
        f->var_id = src_id;
        vq = entity_var_by_id(entity, q_id);
        vs = entity_var_by_id(entity, src_id);
        vs->polarity = vq->polarity;
        d_id = entity_var_id_by_name(entity, inst->d);
        if (util_str_icmp(inst->d, src) == 0) {
            lib_move_variable(d_id, entity->inputs, entity->locals);
            if (vs->polarity == -1) {
                vs->polarity = 1;
                entity_toggle_inv(entity, vs->id);
            }
        } else {
            lib_fcts_replace_id(entity->fcts, d_id, src_id);
            p = list_find(entity->inputs, inst->d, var_has_name);
            list_del_shift(entity->inputs, p);
        }
        p = list_find(net->aliases, inst->d, alias_name_eq);
        list_del_shift(net->aliases, p);
        p = list_find(net->aliases, inst->q, alias_name_eq);
        list_del_shift(net->aliases, p);
        p = list_find(entity->outputs, inst->q, var_has_name);
        list_del_shift(entity->outputs, p);
        list_del_shift(net->items, pos);
        i--;
        pos--;
        entity->n_in = LCOUNT(entity->inputs);
        entity->n_out = LCOUNT(entity->outputs);
        entity->n_local = LCOUNT(entity->locals);
    }
}

unsigned *latch_inverted_set(FitList *vars)
{
    unsigned *set;
    int nwords, i;

    if (vars == NULL)
        return NULL;
    if (vars->count == 0)
        nwords = 1;
    else
        nwords = (int)(((unsigned)vars->count - 1u) >> 5) + 1;
    set = util_calloc((size_t)nwords, sizeof *set);
    for (i = 0; i < vars->count; i++) {
        FitVariable *v = vars->items[i];

        if (v->polarity == -1)
            set[i >> 5] |= 1u << (i & 31);
    }
    return set;
}

void latch_fixup_entity(FitEntity *entity)
{
    FitNetlist *net = entity->net;
    int i;

    if (net == NULL)
        return;
    g_latch_tristate_names = list_create();
    for (i = 0; i < LCOUNT(net->items); i++) {
        FitNetItem *it = net->items->items[i];
        FitCell13 *cell;
        FitInstance *inst;

        if (it->kind != FIT_GATE_CELL13)
            continue;
        cell = it->cell;
        if (cell->buf_type != 3 && cell->buf_type != 4)
            continue;
        inst = util_calloc(1, sizeof *inst);   /* not instance_new: polarities stay 0 */
        it->kind = FIT_GATE_TH;
        it->cell = inst;
        if (cell->inst_name != NULL)
            inst->inst_name = util_strdup(cell->inst_name);
        inst->q = util_strdup(cell->in);
        inst->d = util_strdup(cell->pad);
        inst->oe = util_strdup(cell->oe);
        inst->oe_pol = cell->oe_pol;
        if (cell->buf_type == 3) {
            int d_id, pad_id, p;

            list_add(g_latch_tristate_names, cell->inst_name);
            d_id = entity_var_id_by_name(entity, cell->pad);
            pad_id = entity_var_id_by_name(entity, cell->out);
            p = list_find(entity->inputs, FIT_INT2PTR(pad_id), var_id_eq);
            if (p != 0) {
                list_del_shift(entity->inputs, p);
                entity->n_in--;
            }
            lib_fcts_replace_id(entity->fcts, pad_id, d_id);
            p = list_find(entity->net->aliases, cell->out, alias_name_eq);
            list_del_shift(entity->net->aliases, p);
        }
        /* the item is now of kind 8 and needs no second look; the
         * strings of the old cell are not freed */
        free(cell);
    }
}
