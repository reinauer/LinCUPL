/*
 * fit_drv_main.c - the fit driver of the ATF150x fitter, second part: the
 * driver fit_driver with its pass loop and fallback strategy, the
 * transformations of the netlist it runs before the first pass, the retry
 * helper that cuts wide functions, the pass messages, and the .pin and .io
 * writers.  One pass itself is fit_drv_pass in fit_drv.c.
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

static int is_reg_kind(int kind)        /* D-FF, T-FF, tri-state, latch */
{
    return kind == 3 || kind == 2 || kind == 8 || kind == 4;
}

/* ------------------------------------------------------------------ */
/* Before the first pass                                                */
/* ------------------------------------------------------------------ */

/* Only one TwoClock signal is applied: warns when more are given and
 * shortens the list. */
static void fit_drv_check_twoclock(void)
{
    int i = 1;

    if (g_opt.twoclock == NULL || g_opt.twoclock->count <= 1)
        return;
    fprintf(g_msg, " ## Warning : only ONE TwoClock is applied \n");
    fprintf(g_fit_report, " ## Warning : only ONE TwoClock is applied \n");
    /* deletes positions 2, 3, ... of the shrinking list: with three names
     * the third survives */
    while (i < LN(g_opt.twoclock)) {
        i++;
        list_del_shift(g_opt.twoclock, i);
    }
}

/* is id the output of a register or tri-state? */
static int fit_drv_is_reg_output(FitEntity *ent, int id)
{
    int i;

    if (ent->net == NULL || ent->net->items == NULL)
        return 0;
    for (i = 0; i < LN(ent->net->items); i++) {
        FitNetItem *item = (FitNetItem *)ent->net->items->items[i];

        if (is_reg_kind(item->kind)
            && entity_find_var(ent, ((FitInstance *)item->cell)->q)->id == id)
            return 1;
    }
    return 0;
}

/* an output that only copies or inverts a register output takes
 * the register's place (the two functions exchange their variables) */
static void fit_drv_swap_reg_buffers(FitEntity *ent)
{
    int i = 0;

    while (i < LN(ent->fcts)) {
        FitFct *fct = (FitFct *)ent->fcts->items[i];
        FitVariable *v = entity_var_by_id(ent, fct->var_id);

        if (latch_find_alias(v->name, ent->net) == 0) {
            FitFctTree *t = fct->tree[0];

            if (t->sel == FIT_SEL_NOT) {
                FitFctTree *c = (FitFctTree *)t->u.children->items[0];

                if (c->sel == FIT_SEL_ID && fit_drv_is_reg_output(ent, c->u.id) != 0) {
                    int src = c->u.id;
                    FitFct *reg;
                    FitFctTree *copy;

                    flatten_replace_id(src, fct->var_id, ent);
                    entity_toggle_inv(ent, fct->var_id);
                    reg = entity_find_fct(ent, src);
                    if (reg == NULL)    /* no register: stop */
                        fit_internal_error("fit_drv_swap_reg_buffers: register output without function");
                    copy = fcttree_copy(reg->tree[0], NULL);
                    reg->var_id = fct->var_id;
                    reg->tree[0] = fcttree_copy(fcttree_new_not(copy), NULL);
                    fct->var_id = src;
                    fct->tree[0] = fcttree_new_not(fct->tree[0]);
                }
            } else if (t->sel == FIT_SEL_ID && fit_drv_is_reg_output(ent, t->u.id) != 0) {
                int src = t->u.id;
                FitFct *reg;

                flatten_replace_id(src, fct->var_id, ent);
                reg = entity_find_fct(ent, src);
                if (reg == NULL)        /* no register: stop */
                    fit_internal_error("fit_drv_swap_reg_buffers: register output without function");
                reg->var_id = fct->var_id;
                fct->var_id = src;
            }
        }
        i++;
    }
}

/* the q signal of a cell must not have a function: an existing
 * one is substituted into its users; a q that is an input pin is replaced
 * by a new variable that copies it */
static void fit_drv_fix_reg_outputs(FitEntity *ent)
{
    int i;

    for (i = 0; i < LN(ent->net->items); i++) {
        FitNetItem *item = (FitNetItem *)ent->net->items->items[i];
        FitInstance *inst;
        FitFct *fct;
        int id;

        /* unknown gates and kind 13 cells are not register cells */
        if (item->kind == FIT_GATE_UNKNOWN || item->kind == FIT_GATE_CELL13)
            continue;
        inst = (FitInstance *)item->cell;
        id = entity_var_id_by_name(ent, inst->q);
        if (id == 0)
            continue;
        fct = entity_find_fct(ent, id);
        if (fct != NULL) {
            tt2_substitute_fct(ent, fct);
        } else if (entity_has_input_id(ent, id) != 0) {
            FitVariable *nv = lib_new_variable(ent, 1, 1);

            fct = fct_new();
            fct->var_id = nv->id;
            fct->tree[0] = fcttree_new_id(id);
            list_add(ent->fcts, fct);
            inst->q = util_strdup(nv->name);
        }
    }
}

/* substitutes the function of the control signal `name` into its users
 * unless it is a sum. */
static void fit_drv_inline_ctrl_product(FitEntity *ent, char *name)
{
    int id = entity_var_id_by_name(ent, name);

    if (id != 0) {
        FitFct *fct = entity_find_fct(ent, id);

        if (fct != NULL && fct->tree[0]->sel != FIT_SEL_OR)
            tt2_substitute_fct(ent, fct);
    }
}

/* control functions that are not sums are substituted into
 * their users */
void fit_drv_inline_ctrl_products(FitEntity *ent)
{
    int i;

    for (i = 0; i < LN(ent->net->items); i++) {
        FitNetItem *item = (FitNetItem *)ent->net->items->items[i];
        FitInstance *inst;

        /* unknown gates and kind 13 cells are not register cells */
        if (item->kind == FIT_GATE_UNKNOWN || item->kind == FIT_GATE_CELL13)
            continue;
        inst = (FitInstance *)item->cell;
        if (inst->clk != NULL)
            fit_drv_inline_ctrl_product(ent, inst->clk);
        if (inst->oe != NULL)
            fit_drv_inline_ctrl_product(ent, inst->oe);
        if (inst->ar != NULL)
            fit_drv_inline_ctrl_product(ent, inst->ar);
        if (inst->ap != NULL)
            fit_drv_inline_ctrl_product(ent, inst->ap);
        if (inst->ce != NULL)
            fit_drv_inline_ctrl_product(ent, inst->ce);
    }
}

/* the register whose data or pad signal is variable id */
static FitInstance *fit_drv_reg_fed_by(FitEntity *ent, int id, int *kind)
{
    FitVariable *v;
    int i;

    if (ent->net == NULL || ent->net->items == NULL)
        return NULL;
    v = entity_var_by_id(ent, id);
    for (i = 0; i < LN(ent->net->items); i++) {
        FitNetItem *item = (FitNetItem *)ent->net->items->items[i];

        if (is_reg_kind(item->kind)) {
            FitInstance *inst = (FitInstance *)item->cell;

            if (util_str_icmp(v->name, inst->d) == 0
                || util_str_icmp(v->name, inst->pin) == 0) {
                *kind = item->kind;
                return inst;
            }
        }
    }
    return NULL;
}

/* push_gate.  An output pin that is the inversion of the buried
 * signal of a register becomes that signal's pin: the inversion moves in
 * front of the register (D complemented, reset and preset exchanged). */
static void fit_drv_push_gates(FitEntity *ent)
{
    int i;

    for (i = 0; i < LN(ent->fcts); i++) {
        FitFct *fct = (FitFct *)ent->fcts->items[i];
        char *name = entity_var_by_id(ent, fct->var_id)->name;
        FitFctTree *c;
        FitInstance *inst;
        FitAlias *alias;
        FitVariable *qv;
        int src, k;
        int kind = 0;
        char *s;
        int p;

        if (latch_find_alias(name, ent->net) != 0 || fct->tree[0]->sel != FIT_SEL_NOT)
            continue;
        if (entity_is_local(ent, fct->var_id) != 0)
            continue;
        c = (FitFctTree *)fct->tree[0]->u.children->items[0];
        if (c->sel != FIT_SEL_ID)
            continue;
        src = c->u.id;
        inst = fit_drv_reg_fed_by(ent, src, &kind);
        if (inst == NULL)
            continue;
        k = list_find(ent->net->aliases, inst->d, alias_name_eq);
        if (k == 0)         /* no alias entry: stop */
            fit_internal_error("fit_drv_push_gates: register input without alias entry");
        alias = (FitAlias *)ent->net->aliases->items[k - 1];
        if (alias->target != NULL)
            continue;
        /* a preassigned output stays where it is; the extended mode still moves it
         * when it names the register instance itself */
        if (g_fit_preassign_used != 0
            && ((list_find(g_preassign, name, place_name_eq) != 0
                 && (!FIT_EXT || util_str_icmp(inst->inst_name, name) != 0))
                || list_find(g_preassign, alias->name, place_name_eq) != 0))
            continue;
        alias->target = util_strdup(name);
        entity_toggle_inv(ent, src);
        qv = entity_find_var(ent, inst->q);
        if (kind != 2) {
            FitFct *qf = entity_find_fct(ent, qv->id);

            if (tt2_tree_is_const(qf->tree[0]) == 0)
                qv->polarity = (qv->polarity != -1) ? -1 : 1;
            else
                qf->tree[0] = fcttree_new_not(qf->tree[0]);
        }
        s = inst->ar;
        p = inst->ar_pol;
        inst->ar = inst->ap;
        inst->ap = s;
        inst->ar_pol = inst->ap_pol;
        inst->ap_pol = p;
    }
}

/* the property lists are converted from names to variable ids */
static void fit_drv_names_to_ids(FitEntity *ent)
{
    FitList **lists[5];
    int k;

    lists[0] = &g_opt.global_fold;
    lists[1] = &g_opt.cascade_list;
    lists[2] = &g_opt.foldback_list;
    lists[3] = &g_opt.xor_list;
    lists[4] = &g_opt.soft_buffer;
    for (k = 0; k < 5; k++) {
        if (*lists[k] != NULL) {
            FitList *names = list_copy(*lists[k]);

            fit_prep_names_to_ids(ent, names, lists[k]);
            list_destroy(&names);
        }
    }
}

/* user foldbacks that are plain copies or inversions are
 * implemented complemented */
static void fit_drv_invert_user_foldbacks(FitEntity *ent)
{
    int i;

    for (i = 0; i < LN(ent->locals); i++) {
        FitVariable *v = (FitVariable *)ent->locals->items[i];

        if (list_find(g_opt.foldback_list, FIT_INT2PTR(v->id), list_ptr_eq) != 0) {
            FitFct *fct = entity_find_fct(ent, v->id);
            int sel = fct->tree[0]->sel;

            if (sel == FIT_SEL_ID || sel == FIT_SEL_NOT) {
                fct->tree[0] = fcttree_new_not(fct->tree[0]);
                fit_prep_tree_to_sop(&fct->tree[0], ent);
                entity_toggle_inv(ent, v->id);
            }
        }
    }
}

/* for the Global_OE entry `name` (a register): id of the source
 * of the register's output enable, 0 when there is none.  A preassignment
 * of "<name>_OE" is renamed to the source. */
static int fit_drv_global_oe_source(FitEntity *ent, char *name)
{
    char buf[FIT_NAME_MAX];
    int i;

    for (i = 0; i < LN(ent->net->items); i++) {
        FitNetItem *item = (FitNetItem *)ent->net->items->items[i];
        FitInstance *inst;
        FitFct *fct;
        FitVariable *v;
        int src = 0;        /* stays 0 when the output enable is neither a
                               signal nor its inversion */
        int k;

        if (!is_reg_kind(item->kind))
            continue;
        inst = (FitInstance *)item->cell;
        if (util_str_icmp(inst->inst_name, name) != 0)
            continue;
        if (inst->oe == NULL)
            return 0;
        fct = entity_find_fct(ent, entity_var_id_by_name(ent, inst->oe));
        if (fct == NULL)
            return 0;
        if (fct->tree[0]->sel == FIT_SEL_ID)
            src = fct->tree[0]->u.id;
        if (fct->tree[0]->sel == FIT_SEL_NOT)
            src = ((FitFctTree *)fct->tree[0]->u.children->items[0])->u.id;
        v = entity_var_by_id(ent, src);
        snprintf(buf, sizeof buf, "%s_%s", name, "OE");
        k = list_find(g_preassign, buf, place_name_eq);
        if (k != 0 && v != NULL) {
            FitPreassignItem *pre = (FitPreassignItem *)g_preassign->items[k - 1];
            void *old = pre->name;

            flatten_free_ref(&old);
            pre->name = util_strdup(v->name);
        }
        return src;
    }
    return 0;
}

/* the Global_OE list (register names) becomes a list of the ids
 * of the output-enable sources, which are also appended to
 * g_opt.global_fold */
static void fit_drv_resolve_global_oe(FitEntity *ent)
{
    int i = 0;
    int pos = 1;

    while (i < LN(g_opt.global_oe)) {
        int id = fit_drv_global_oe_source(ent, (char *)g_opt.global_oe->items[i]);

        list_del_shift(g_opt.global_oe, pos);
        if (id == 0)
            continue;
        list_insert_at(g_opt.global_oe, FIT_INT2PTR(id), pos);
        if (g_opt.global_fold == NULL)
            g_opt.global_fold = list_create();
        list_add(g_opt.global_fold, FIT_INT2PTR(id));
        i++;
        pos++;
    }
}

/* pin count and number of registers against the device */
static int fit_drv_check_limits(FitEntity *ent)
{
    int n = 0;
    int i;

    if (dev_check_pin_count(ent) == 0)
        return 0;
    for (i = 0; i < LN(ent->net->items); i++) {
        int kind = ((FitNetItem *)ent->net->items->items[i])->kind;

        if (kind == 3 || kind == 2 || kind == 4)
            n++;
    }
    if (g_dev->n_macrocells < n) {
        fprintf(g_fit_report, "# ERROR : Design has %d flip flop, latch or tristate.\n", n);
        fprintf(g_msg, "# ERROR : Design has %d flip flop, latch or tristate.\n", n);
        g_exit_code = 0x193;
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Retry: cutting wide functions                                        */
/* ------------------------------------------------------------------ */

/* moves operands of tree, `chunk` (at first 5) at a time, into
 * new local functions until tree has at most max_ops operands and at most
 * `limit` inputs */
static void fit_drv_cut_tree(FitEntity *ent, FitFctTree *tree, int max_ops, int limit)
{
    int chunk = 5;
    int done = 0;
    int first = 1;
    int n, cnt, k;
    FitList *sub;

top:
    do {
        if (LN(tree->u.children) <= max_ops)
            return;
        sub = list_create();
        if (LN(tree->u.children) < chunk)
            chunk = LN(tree->u.children);
        for (k = chunk; k > 0; k--) {
            list_add(sub, tree->u.children->items[0]);
            list_del_shift(tree->u.children, 1);
        }
        fit_prep_extract_operands(ent, tree, sub, 1);
        done++;
        n = LN(tree->u.children);
    } while (chunk < n - done);
    if (n <= max_ops) {
        if (first != 0)
            cnt = fcttree_count_ids(tree);
        else
            cnt = n;
        if (cnt <= limit)
            return;
        chunk = LN(tree->u.children) - done;
        max_ops = LN(tree->u.children) - 1;
        done = -1;
        first = 0;
        goto top;
    }
    if (first != 0) {
        FitList *all = list_create();
        int j;

        for (j = 0; j < LN(tree->u.children) - done; j++) {
            FitList *ids = list_create();

            fcttree_collect_ids((FitFctTree *)tree->u.children->items[j], ids);
            all = list_union(all, ids, list_ptr_eq);
            list_destroy(&ids);
        }
        cnt = LN(all);
        list_destroy(&all);
    } else {
        cnt = n - done;
    }
    n = LN(tree->u.children);
    if (limit < (cnt - n) + done + chunk || done < max_ops)
        chunk = n - done;
    done = -1;
    first = 0;
    goto top;
}

/* function of variable id, NULL when id is an input */
static FitFct *fit_drv_fct_unless_input(FitEntity *ent, int id)
{
    if (list_find(ent->inputs, FIT_INT2PTR(id), var_id_eq) == 0)
        return entity_find_fct(ent, id);
    return NULL;
}

/* for an XOR function (a*b) + (c*d): the function of a or of b
 * if it is a sum, else NULL */
FitFct *fit_drv_xor_wide_operand(FitEntity *ent, FitFct *fct)
{
    FitFctTree *t = fct->tree[0];
    FitFctTree *prod;
    FitFct *f;

    if (t->sel != FIT_SEL_OR || t->u.children == NULL || t->u.children->count != 2)
        return NULL;
    prod = (FitFctTree *)t->u.children->items[0];
    if (prod->sel != FIT_SEL_AND || prod->u.children == NULL || prod->u.children->count != 2)
        return NULL;
    f = fit_drv_fct_unless_input(ent, jed_tree_var_id((FitFctTree *)prod->u.children->items[0]));
    if (f != NULL && f->tree[0]->sel == FIT_SEL_OR)
        return f;
    f = fit_drv_fct_unless_input(ent, jed_tree_var_id((FitFctTree *)prod->u.children->items[1]));
    if (f != NULL && f->tree[0]->sel == FIT_SEL_OR)
        return f;
    return NULL;
}

/* retry after a grouping or placement failure: every function
 * that needs more than one macrocell is cut into pieces that fit one */
static void fit_drv_relax(FitEntity *ent, FitList *used, int limit)
{
    FitList *inputs = list_create();
    int n = LN(ent->fcts);
    int i;

    for (i = 0; i < n; i++) {
        FitFct *fct = (FitFct *)ent->fcts->items[i];
        FitVariable *v;
        FitFctTree *tree;
        int sel, n_terms, n_ctrl, max_ops;

        if (fct == NULL || fct->tree[0] == NULL)
            continue;
        sel = fct->tree[0]->sel;
        if (sel == FIT_SEL_CONST || sel == FIT_SEL_ID || sel == FIT_SEL_SET)
            continue;
        if (list_find(used, FIT_INT2PTR(fct->var_id), list_ptr_eq) != 0)
            continue;
        v = entity_var_by_id(ent, fct->var_id);
        n_terms = fit_drv_count_mc_terms(ent, fct, v, inputs);
        if (n_terms <= 5)
            continue;
        if (latch_find_alias(v->name, ent->net) == 0)
            n_ctrl = 0;
        else
            n_ctrl = fit_drv_count_ctrl_terms(ent, inputs, v->name);
        if (list_find(g_opt.xor_list, FIT_INT2PTR(fct->var_id), list_ptr_eq) == 0) {
            max_ops = 5 - n_ctrl;
            if (max_ops < 1)
                max_ops = 1;
            if (n_terms % 5 == 0 && g_fit_retry_count == 0)
                continue;
            tree = fct->tree[0];
        } else {
            FitFct *part;

            max_ops = 4 - n_ctrl;
            if (max_ops < 1)
                max_ops = 1;
            part = fit_drv_xor_wide_operand(ent, fct);
            if (n_terms % 5 == 0 && g_fit_retry_count == 0)
                continue;
            if (part == NULL)   /* no function: stop */
                fit_internal_error("fit_drv_relax: XOR function without a sum operand");
            tree = part->tree[0];
        }
        fit_drv_cut_tree(ent, tree, max_ops, limit);
    }
}

/* ------------------------------------------------------------------ */
/* Pass messages and fallbacks                                          */
/* ------------------------------------------------------------------ */

/* "Fitter_Pass <n>, Preassign = ..." line with the reason for the pass,
 * on the message stream and in the report. */
static void fit_drv_print_pass_header(int pass, int reason)
{
    const char *pre = g_fit_preassign_used != 0 ? "KEEP" : "IGNORE";
    const char *what = NULL;

    if (reason == 3)
        return;
    fprintf(g_msg, "---------------------------------------------------------\n");
    fprintf(g_msg, " Fitter_Pass %d, Preassign = %s,", pass, pre);
    fprintf(g_fit_report, "---------------------------------------------------------\n");
    fprintf(g_fit_report, " Fitter_Pass %d, Preassign = %s,", pass, pre);
    if (reason == 0) {
        what = g_opt.logic_doubling == 0 ? " LOGIC_DOUBLING : OFF \n" : " LOGIC_DOUBLING : ON \n";
    } else if (reason == 1 || reason == 2) {
        if (g_fit_retry_count == 1)
            what = " CASCADE_LOGIC : (TRY) \n";
        else if (g_fit_retry_count == 2)
            what = " CASCADE_LOGIC : (OFF) \n";
    } else if (reason == -1) {
        what = g_fit_plan_done == 0 ? " LOGIC_DOUBLING : ON \n" : " OPTIMIZE : ON \n";
    } else if (reason == 4) {
        what = " NODE ASSIGN : OFF \n";
    } else if (reason == 5) {
        what = g_opt.logic_doubling != 0 ? " LOGIC_DOUBLING : ON \n" : " LOGIC_DOUBLING : OFF \n";
    } else if (reason == 6) {
        what = " LOGIC_DOUBLING : ON \n";
    }
    if (what != NULL) {
        fprintf(g_msg, "%s", what);
        fprintf(g_fit_report, "%s", what);
    }
    fprintf(g_msg, " ... \n");
    fprintf(g_fit_report, " ... \n");
}

/* the warning or error line for the result of a failed pass, on the
 * message stream and in the report. */
static void fit_drv_print_failure(int result)
{
    const char *text = NULL;

    if (result == -2)
        text = FIT_EXT ? " ## ERROR : Bad user pin assignment \n" : " ## ERROR : Bad user pin assignement \n";
    else if (result == -1)
        text = g_opt.logic_doubling == 0
                   ? " ## Warning : Design is large \n"
                   : " ## Warning : Even with doubling, design is still large \n";
    else if (result == 1)
        text = " ## Warning : Grouping fail \n";
    else if (result == 2)
        text = " ## Warning : Placement fail \n";
    else if (result == 3 && g_fit_route_limit > 0x27)
        text = " ## Warning : Routing fail \n";
    if (text != NULL) {
        fprintf(g_msg, "%s", text);
        fprintf(g_fit_report, "%s", text);
    }
}

/* fallback "NODE ASSIGN : OFF": the preassignments of unknown
 * names and of buried nodes are dropped */
static void fit_drv_drop_node_preassign(FitEntity *ent)
{
    int i = 0;
    int pos = 1;

    while (i < LN(g_preassign)) {
        FitVariable *v = entity_find_var(ent, ((FitPreassignItem *)g_preassign->items[i])->name);

        if (v == NULL || entity_var_is_node(ent, v) != 0) {
            list_del_shift(g_preassign, pos);
            continue;
        }
        i++;
        pos++;
    }
}

/* block numbers of the UIM slots become 1-based for the fuse
 * map and the report */
static void fit_drv_number_uim_blocks(FitList *uim)
{
    int b, i;

    for (b = 0; b < LN(uim); b++) {
        FitList *l = (FitList *)uim->items[b];

        for (i = 0; i < LN(l); i++)
            ((FitUimSlot *)l->items[i])->block++;
    }
}

/* ------------------------------------------------------------------ */
/* .pin and .io                                                         */
/* ------------------------------------------------------------------ */

/* writes <out>.pin: the pin of every placed signal and the node of every
 * buried one. */
static void fit_drv_write_pin_file(void)
{
    char *path = util_strdup(g_opt.out_base);
    void *p;
    FILE *f;
    int i;

    util_str_append(&path, ".pin");
    f = fopen(path, "w");
    if (f == NULL) {
        fprintf(g_msg, "Can not open %s file\n", path);
        return;
    }
    fprintf(f, "CHIP \"%s\" ASSIGNED TO AN %s\n", g_opt.out_base, g_dev_package);
    for (i = 0; i < LN(g_fit_slots); i++) {
        FitSlot *slot = (FitSlot *)g_fit_slots->items[i];

        if (util_str_icmp(slot->signal, FIT_FREE_NAME) != 0) {
            char number[16];
            fprintf(f, "%s\t\t\t\t\t: %s\n", slot->signal,
                    dev_pin_name(slot->pin, number, sizeof(number)));
        }
        if (util_str_icmp(slot->buried, FIT_FREE_NAME) != 0)
            fprintf(f, "%s\t\t\t\t\t: %d\n", slot->buried, slot->macrocell + 600);
    }
    fclose(f);
    p = path;
    flatten_free_ref(&p);
}

/* writes <out>.io: INPUT_PIN / OUTPUT_PIN / NODE_NUM of every placed
 * signal. */
static void fit_drv_write_io_file(void)
{
    char *path = util_strdup(g_opt.out_base);
    void *p;
    FILE *f;
    int i;

    util_str_append(&path, ".io");
    f = fopen(path, "w");
    if (f == NULL) {
        fprintf(g_msg, "Can not open %s file\n", path);
        return;
    }
    fprintf(f, "CHIP \"%s\"\n", g_opt.out_base);
    fprintf(f, "BEGIN\n");
    fprintf(f, "\n    DEVICE = \"%s\";\n", g_dev_package);
    for (i = 0; i < LN(g_fit_slots); i++) {
        FitSlot *slot = (FitSlot *)g_fit_slots->items[i];

        if (util_str_icmp(slot->signal, FIT_FREE_NAME) != 0) {
            char number[16];
            const char *pin = dev_pin_name(slot->pin, number, sizeof(number));
            fprintf(f, "    \"%s\"", slot->signal);
            report_put_blanks(f, 0x28 - (int)strlen(slot->signal));
            if (slot->fct_id == 0)
                fprintf(f, ": INPUT_PIN = %s ;\n", pin);
            else
                fprintf(f, ": OUTPUT_PIN = %s ;\n", pin);
        }
        if (util_str_icmp(slot->buried, FIT_FREE_NAME) != 0) {
            fprintf(f, "    \"%s\"", slot->buried);
            report_put_blanks(f, 0x28 - (int)strlen(slot->buried));
            fprintf(f, ": NODE_NUM = %d ;\n", slot->macrocell + 600);
        }
    }
    fprintf(f, "END;\n");
    fclose(f);
    p = path;
    flatten_free_ref(&p);
}

/* .vho / .vo / .sdo / .edn writers behind -str Vhdl_sim, Verilog_sim,
 * Out_Edif: not supported */
static void fit_drv_write_sim_outputs(const char *out_base, FitEntity *ent)
{
    (void)out_base;
    (void)ent;
    fit_unsupported("Vhdl_sim / Verilog_sim / Out_Edif");
}

/* Extended mode: a register node whose data function is an inverted product
 * takes the product itself; the register's data input is inverted
 * instead, and reset and preset are exchanged. */
static void fit_drv_uninvert_node_products(FitEntity *ent)
{
    int i;

    for (i = 0; i < LN(ent->fcts); i++) {
        FitFct *fct = (FitFct *)ent->fcts->items[i];
        FitVariable *v;
        FitInstance *inst;
        char *s;
        int p;

        if (fct->tree[0]->sel != FIT_SEL_AND)
            continue;
        v = entity_var_by_id(ent, fct->var_id);
        if (v->polarity != -1 || latch_find_alias(v->name, ent->net) == 0)
            continue;
        inst = netlist_find_instance(ent, v->name);
        if (inst == NULL || entity_var_is_node(ent, v) == 0)
            continue;
        v->polarity = 1;
        entity_toggle_inv(ent, entity_find_var(ent, inst->d)->id);
        s = inst->ar;
        p = inst->ar_pol;
        inst->ar = inst->ap;
        inst->ar_pol = inst->ap_pol;
        inst->ap = s;
        inst->ap_pol = p;
    }
}

/* Extended mode: an input on a dedicated pin cannot be SSTL; it is taken off
 * the SSTL list with a warning */
static void fit_drv_check_sstl_inputs(void)
{
    int i;

    for (i = 0; i < LN(g_fit_global_names); i++) {
        char *name = g_fit_global_names->items[i];
        char *node;
        int k = list_find(g_opt.sstl_list, name, fit_str_ieq_cb);

        if (k != 0 && place_node_of_signal(name, &node)->block == 0) {
            fprintf(g_fit_report, "Warning: I/O %s is a dedicated input, it cannot support SSTL\n", name);
            list_del_shift(g_opt.sstl_list, k);
        }
    }
}

/* ------------------------------------------------------------------ */
/* The driver                                                           */
/* ------------------------------------------------------------------ */

int fit_driver(FitEntity *ent, char *out_base, int preassign, int do_latch_synthesis)
{
    FitEntity *work, *saved;
    FitList *used, *saved_xor, *saved_inlatch, *slots, *uim;
    int pass = 1;
    int result = 0;
    int switched = 0;
    int n_xor;

    fit_drv_check_twoclock();
    report_write_strategy(util_base_name(out_base), g_fit_report);
    fit_prep_normalise(ent);
    g_fit_foldback_ids = list_create();
    fit_drv_swap_reg_buffers(ent);
    fit_drv_fix_reg_outputs(ent);
    if (fit_prep_check_ctrl_conflicts(ent) == 0) {
        fprintf(g_msg, " Design does not fit \n");
        return 0;
    }
    fit_prep_classify_registers(ent);
    fit_prep_share_ctrl_terms(ent);
    fit_drv_inline_ctrl_products(ent);
    if (g_opt.push_gate != 0)
        fit_drv_push_gates(ent);
    work = fit_prep_working_copy(ent);
    if (FIT_EXT)
        fit_drv_uninvert_node_products(work);
    fit_drv_names_to_ids(work);
    n_xor = LN(g_opt.xor_list);
    /* the extended mode always merges the tri-state buffers into their registers */
    if (FIT_EXT || preassign != g_fit_preassign_used)
        netlist_absorb_tristates(work);
    if (g_opt.xor_synthesis != 0 || n_xor != 0)
        fit_prep_detect_xor(work, n_xor);
    used = place_xor_local_ids(work);
    if (do_latch_synthesis != 0)
        latch_synthesis(&work);
    latch_remove_buffers(work);
    fit_drv_invert_user_foldbacks(work);
    if (preassign == FIT_PREASSIGN_IGNORE) {
        list_destroy(&g_preassign);
        g_preassign = list_create();
        g_fit_preassign_used = 0;
    }
    saved_xor = list_copy(g_opt.xor_list);
    saved_inlatch = list_copy(g_opt.fast_inlatch_list);
    fit_drv_resolve_global_oe(work);
    if (fit_drv_check_limits(work) == 0) {
        fprintf(g_msg, " Design does not fit \n");
        return 0;
    }
    g_fit_route_limit = FIT_FIRST_ROUTE_LIMIT;
    saved = entity_copy(work);
    fit_drv_print_pass_header(1, result);
    if (fit_drv_pass(work, used, &result) != 0)
        goto finish;

    for (;;) {
        fit_drv_print_failure(result);
        if ((result == 1 || result == 2) && g_fit_retry_count < 2
            && g_opt.cascade_logic == 0) {
            fit_drv_relax(work, used, FIT_LAST_ROUTE_LIMIT);
            g_fit_retry_count++;
            g_fit_route_limit = FIT_FIRST_ROUTE_LIMIT;
            pass++;
        } else if (result == 3 && g_fit_route_limit < FIT_LAST_ROUTE_LIMIT) {
            g_fit_route_limit++;
        } else if (result == -1) {
            if (g_opt.logic_doubling == 0) {
                g_opt.logic_doubling = 1;
                entity_free(&work);
                g_fit_plan_done = 0;
                work = entity_copy(saved);
                list_destroy(&g_fit_foldback_ids);
                g_fit_foldback_ids = list_create();
                g_opt.xor_list = list_copy(saved_xor);
                pass++;
            } else if (g_opt.optimize != 0) {
                goto finish;
            } else if (preassign != g_fit_preassign_used) {
                g_opt.optimize = 1;
                pass++;
            } else {
                fprintf(g_msg, "Please try with optimize : ON ...\n");
                goto finish;
            }
        } else if (g_fit_preassign_used != 0 && switched == 0) {
            switched = 1;
            g_fit_route_limit = FIT_FIRST_ROUTE_LIMIT;
            fit_drv_drop_node_preassign(work);
            result = 4;
            pass++;
        } else if (preassign == FIT_PREASSIGN_TRY && g_fit_preassign_used != 0) {
            list_destroy(&g_preassign);
            g_preassign = list_create();
            list_destroy(&g_fit_foldback_ids);
            g_fit_foldback_ids = list_create();
            g_fit_retry_count = 0;
            g_fit_preassign_used = 0;
            g_fit_route_limit = FIT_FIRST_ROUTE_LIMIT;
            entity_free(&work);
            g_fit_plan_done = 0;
            work = entity_copy(saved);
            g_opt.xor_list = list_copy(saved_xor);
            result = 5;
            pass = 1;
        } else {
            result = 6;
            if (g_opt.logic_doubling != 0) {
                if (g_opt.optimize == 0)
                    fprintf(g_msg, " Please try with optimize : ON ...\n");
                else if (g_opt.cascade_logic != 0)
                    fprintf(g_msg, " Please try with cascade_logic : OFF ...\n");
                goto finish;
            }
            g_opt.logic_doubling = 1;
            pass++;
        }
        fit_drv_print_pass_header(pass, result);
        g_fit_place_failed = 0;
        g_fit_route_failed = 0;
        result = 0;
        fit_drv_restore_fast_inlatch(saved_inlatch);
        g_exit_code = 0;
        if (fit_drv_pass(work, used, &result) != 0)
            break;
    }

finish:
    slots = g_fit_slots;
    uim = g_fit_uim_slots;
    fit_drv_number_uim_blocks(uim);
    if (g_equiv_verbose != 0) {
        printf("\nVerification apres backtracking\n");
        fit_drv_verify_preassign(work, g_fit_slots);
    }
    if (FIT_EXT)
        fit_drv_check_sstl_inputs();
    if (result == 0) {
        if (jed_build_fuse_map(work, slots, uim) == 0) {
            g_exit_code = 0x1fa;
            g_report_fits = 0;
            report_write_result(work, out_base, slots, uim);
            fprintf(g_msg, " Fuse mapping fail \n");
            fprintf(g_msg, " Design does not fit \n");
            return 0;
        }
    } else {
        g_report_fits = 0;
    }
    report_write_result(work, out_base, slots, uim);
    if (g_report_fits == 0)
        fprintf(g_msg, " Design does not fit \n");
    if (result != 0)
        return 0;
    if (jed_write_file(out_base) == 0) {
        fprintf(g_msg, " Design does not fit \n");
        g_exit_code = 0x1fb;
        return 0;
    }
    tt3_write(out_base, work, NULL);
    fit_drv_write_pin_file();
    fit_drv_write_io_file();
    if (g_opt.vhdl_sim != 0 || g_opt.verilog_sim != 0 || g_opt.out_edif != 0)
        fit_drv_write_sim_outputs(out_base, work);
    fprintf(g_msg, " Design fits successfully\n");
    return 1;
}
