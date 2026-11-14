/*
 * tt_tt3.c - tt3_write: the fitted design as text.  Two outputs: the "<device> programmed logic:" equations of the
 * .fit report and the .tt3 file (a PLA whose "#$ PINS"/"#$ NODES" lines
 * carry the pin and node numbers the fitter assigned).  Both start from a
 * copy of the entity in which the netlist instances have been turned back
 * into named signals: <node>.D/.T/.L/.TH for the data input, <node>.C
 * (.LE/.LH) .AR .AP .OE .CE for the control terms, <node>.Q/.PIN for the
 * feedbacks.
 */
#include "banner.h"
#include "tt_priv.h"

int g_tt3_n_pterms;
int g_tt3_n_nodes;
int g_tt3_n_pins;
int g_tt3_n_out;
int g_tt3_n_in;

/* would be set while the product terms of a second entity are written
 * (they are printed with '0' instead of '1').  tt3_write never has a
 * second entity, so the flag stays 0. */
static int g_tt3_second_pla;

/* ------------------------------------------------------------------ */
/* Equations for the report                                            */
/* ------------------------------------------------------------------ */

/* a message of tt3_print_tree; the extended mode also writes it into the report */
static void tt3_message(const char *text)
{
    fputs(text, g_msg);
    if (FIT_EXT && g_fit_report != NULL)
        fputs(text, g_fit_report);
}

/* writes a tree in infix form with the given operator strings (its
 * messages call it FctTreeCopy) */
static void tt3_print_tree(FILE *out, FitEntity *entity, FitFctTree *tree, const char *or_sep,
                           const char *and_sep, const char *not_str)
{
    char *name;
    int i;

    while (tree != NULL) {
        switch (tree->sel) {
        case FIT_SEL_NOT:
            fprintf(out, "%s", not_str);
            tree = tree->u.children->items[0];
            continue;
        case FIT_SEL_AND:
        case FIT_SEL_OR:
        case FIT_SEL_EQ:
        case FIT_SEL_XOR:
            fprintf(out, "(");
            tt3_print_tree(out, entity, tree->u.children->items[0], or_sep, and_sep, not_str);
            for (i = 1; i < tt_count(tree->u.children); i++) {
                switch (tree->sel) {
                case FIT_SEL_AND:
                    fprintf(out, "%s", and_sep);
                    break;
                case FIT_SEL_OR:
                    fprintf(out, "%s", or_sep);
                    break;
                case FIT_SEL_EQ:
                    fprintf(out, "==");
                    break;
                default:
                    fprintf(out, "$");
                    break;
                }
                tt3_print_tree(out, entity, tree->u.children->items[i], or_sep, and_sep,
                               not_str);
            }
            fprintf(out, ")");
            return;
        case FIT_SEL_CONST:
            fprintf(out, "%d", tree->u.value);
            return;
        case 'F':
            tt3_message("In FctTreeCopy, old-fashioned selector.\n");
            fprintf(out, "0 ");
            return;
        case FIT_SEL_ID:
            name = entity_var_name(entity, tree->u.id);
            if (g_opt.open_collector == 0
                || list_find(g_opt.open_collector_list, name, tt_cb_str_ieq) == 0)
                fprintf(out, "%s", name);
            else
                fprintf(out, "%s.PIN", name);
            free(name);
            return;
        case FIT_SEL_SET:
            return;
        case FIT_SEL_EMPTY:
            tt3_message("In FctTreeCopy, old-fashioned selector.\n");
            fprintf(out, "1 ");
            return;
        default:
            tt3_message("Illegal Selector in FctTree\n");
            return;
        }
    }
}

/* a function recognised by tt3_fct_is_xor, written "a $ b" */
static void tt3_print_xor(FILE *out, FitEntity *entity, FitFctTree *tree)
{
    FitFctTree *first = tree->u.children->items[0];
    int a = jed_tree_var_id(first->u.children->items[0]);
    int b = jed_tree_var_id(first->u.children->items[1]);
    char *name_a, *name_b;

    if (a == 0 || b == 0)
        return;
    name_a = entity_var_name(entity, a);    /* neither name is released */
    name_b = entity_var_name(entity, b);
    if (jed_children_same_sel(first)) {
        FitFctTree *t = place_fct_tree(entity, b);

        if (t->sel == FIT_SEL_ID)
            fprintf(out, "%s $ !%s", name_a, name_b);
        else
            fprintf(out, "!%s $ %s", name_a, name_b);
    } else {
        fprintf(out, "%s $ %s", name_a, name_b);
    }
}

/* with xor_synthesis on, is the function (x*y + z*w) over
 * exactly two variables */
static int tt3_fct_is_xor(FitEntity *entity, FitFct *fct)
{
    FitFctTree *tree, *c;
    FitList *ids;
    int two;

    (void)entity;
    /* the extended mode also writes the functions of the xor list as xor */
    if (g_opt.xor_synthesis == 0
        && (!FIT_EXT || list_find(g_opt.xor_list, FIT_INT2PTR(fct->var_id), list_ptr_eq) == 0))
        return 0;
    tree = fct->tree[0];
    if (tree->sel != FIT_SEL_OR || tt_count(tree->u.children) != 2)
        return 0;
    c = tree->u.children->items[0];
    if (c->sel != FIT_SEL_AND || tt_count(c->u.children) != 2)
        return 0;
    c = tree->u.children->items[1];
    if (c->sel != FIT_SEL_AND || tt_count(c->u.children) != 2)
        return 0;
    ids = list_create();
    fcttree_collect_ids(tree, ids);
    two = ids->count == 2;
    list_destroy(&ids);
    return two;
}

static void print_assignment(FILE *out, FitEntity *entity, const char *fmt, int lhs_id,
                             int rhs_id)
{
    char *name = entity_var_name(entity, lhs_id);

    fprintf(out, fmt, name);
    free(name);
    name = entity_var_name(entity, rhs_id);
    fprintf(out, "%s;  \n", name);
    free(name);
}

/* one function as an equation, preceded by the control terms the
 * variable carries (FitVariable pin, aux and its FitVarExt).
 * mode -1 writes a '!' in front of the left-hand side. */
static void tt3_print_fct(FILE *out, FitEntity *entity, FitFct *fct, int mode)
{
    FitVariable *v;
    char *name;
    int self;

    if (fct == NULL)
        return;
    if (entity_is_local(entity, fct->var_id))
        v = var_list_find_id(entity->locals, fct->var_id);
    else if (entity_is_output(entity, fct->var_id))
        v = var_list_find_id(entity->outputs, fct->var_id);
    else
        v = NULL;
    if (v == NULL)
        tt_unreachable("tt3_print_fct: function of a variable that is neither output nor local");

    self = v->aux == -1 ? fct->var_id : v->aux;
    if (v->pin != -1) {
        if (v->aux == -1)
            print_assignment(out, entity, "%s.OE = ", fct->var_id, v->pin);
        else
            print_assignment(out, entity, "%s.oe = ", v->aux, v->pin);
    }
    if (v->has_ext == 1) {
        if (v->ext->w[3] != -1)
            print_assignment(out, entity, "%s.CE = ", self, v->ext->w[3]);
        if (v->ext->w[0] != -1)
            print_assignment(out, entity, "%s.CK = ", self, v->ext->w[0]);
        if (v->ext->w[1] != -1)
            print_assignment(out, entity, "%s.AR = ", self, v->ext->w[1]);
        if (v->ext->w[2] != -1)
            print_assignment(out, entity, "%s.AP = ", self, v->ext->w[2]);
    }
    if (mode == -1)
        fprintf(out, "!");
    if (v->has_ext == 1) {
        name = entity_var_name(entity, v->aux);
        fprintf(out, "%s.D = ", name);
        free(name);
        fprintf(out, "\n");
    } else {
        name = entity_var_name(entity, self);
        fprintf(out, "%s = ", name);
        free(name);
    }
    if (!tt3_fct_is_xor(entity, fct))
        tt3_print_tree(out, entity, fct->tree[0], "\n\t# ", " & ", "!");
    else
        tt3_print_xor(out, entity, fct->tree[0]);
    fprintf(out, ";\n");
}

/* ------------------------------------------------------------------ */
/* Netlist lookups                                                     */
/* ------------------------------------------------------------------ */

static int is_register_kind(int kind)
{
    return kind != 0xd && kind != 1 && kind != 6 && kind != 7 && kind != 10;
}

FitInstance *tt3_find_instance_by_input(FitEntity *entity, const char *name, int *kind)
{
    int i;

    *kind = 0;
    if (entity == NULL || entity->net == NULL || entity->net->items == NULL)
        return NULL;
    for (i = 0; i < tt_count(entity->net->items); i++) {
        FitNetItem *item = entity->net->items->items[i];
        FitInstance *inst;

        if (!is_register_kind(item->kind))
            continue;
        inst = item->cell;
        if ((inst->d != NULL && util_str_ieq(name, inst->d))
            || (inst->d2 != NULL && util_str_ieq(name, inst->d2))
            || (inst->pin != NULL && util_str_ieq(name, inst->pin))) {
            *kind = item->kind;
            return inst;
        }
    }
    return NULL;
}

FitInstance *tt3_find_instance_by_alias(FitEntity *entity, const char *name, int *kind)
{
    FitList *aliases = entity->net->aliases;
    FitAlias *alias = NULL;
    int i;

    for (i = 0; i < tt_count(aliases); i++) {
        alias = aliases->items[i];
        if (alias->target != NULL && util_str_icmp(alias->target, name) == 0)
            break;
    }
    if (i == tt_count(aliases))
        return NULL;
    return tt3_find_instance_by_input(entity, alias->name, kind);
}

int tt3_gate_kind_of_output(FitEntity *entity, const char *name)
{
    int i;

    for (i = 0; i < tt_count(entity->net->items); i++) {
        FitNetItem *item = entity->net->items->items[i];
        FitInstance *inst = item->cell;

        /* an unknown gate or a pad buffer has no output q */
        if (item->kind == FIT_GATE_UNKNOWN || item->kind == FIT_GATE_CELL13)
            continue;
        if (util_str_icmp(inst->q, name) == 0)
            return item->kind;
    }
    return -1;
}

/* first register instance whose pad is `name` (ignoring case), NULL if
 * none */
static FitInstance *tt3_find_instance_by_pin(FitEntity *entity, const char *name)
{
    int i;

    if (entity == NULL || entity->net == NULL || entity->net->items == NULL)
        return NULL;
    for (i = 0; i < tt_count(entity->net->items); i++) {
        FitNetItem *item = entity->net->items->items[i];
        FitInstance *inst;

        if (!is_register_kind(item->kind))
            continue;
        inst = item->cell;
        if (inst->pin != NULL && util_str_ieq(name, inst->pin))
            return inst;
    }
    return NULL;
}

/* 1 if `name` has no alias entry of its own but is the target
 * of one */
static int tt3_is_alias_target(FitEntity *entity, const char *name)
{
    int i;

    if (latch_find_alias((char *)name, entity->net) != 0)
        return 0;
    for (i = 0; i < tt_count(entity->net->aliases); i++) {
        FitAlias *alias = entity->net->aliases->items[i];

        if (alias->target != NULL && util_str_icmp(alias->target, name) == 0)
            return 1;
    }
    return 0;
}

/* 1 if some function defines v */
static int tt3_var_has_fct(FitEntity *entity, FitVariable *v)
{
    int i;

    for (i = 0; i < tt_count(entity->fcts); i++)
        if (((FitFct *)entity->fcts->items[i])->var_id == v->id)
            return 1;
    return 0;
}

/* does the feedback of this output come from the pad (the
 * output is open collector) */
static int tt3_is_open_collector(FitEntity *orig, FitVariable *v)
{
    if (g_opt.open_collector == 0 || entity_var_is_node(orig, v))
        return 0;
    if (g_opt.open_collector_list == NULL
        || list_find(g_opt.open_collector_list, v->name, tt_cb_str_ieq) != 0)
        return 1;
    return 0;
}

/* delete the alias entries called `name` and clear the targets
 * that are `name` */
static void tt3_drop_alias(FitList *aliases, const char *name)
{
    int i = 0;

    while (i < tt_count(aliases)) {
        FitAlias *alias = aliases->items[i];

        if (util_str_icmp(alias->name, name) == 0) {
            list_del_shift(aliases, i + 1);
        } else {
            if (util_str_icmp(alias->target, name) == 0)
                flatten_free_ref((void **)&alias->target);
            i++;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Instances back to named signals                                     */
/* ------------------------------------------------------------------ */

/* bring the functions and variables in line with the alias
 * table: the function of an alias target that is itself not a netlist
 * signal is dropped; a register output without target is given the id of
 * the signal the register drives. */
static void tt3_resolve_aliases(FitEntity *entity)
{
    FitNetlist *net = entity->net;
    int i;

    if (net == NULL || net->items == NULL || net->aliases == NULL)
        return;
    for (i = 0; i < tt_count(net->aliases); i++) {
        FitAlias *alias = net->aliases->items[i];
        int pos;

        if (alias->target == NULL || latch_find_alias(alias->target, entity->net) != 0)
            continue;
        pos = list_find(entity->fcts,
                        FIT_INT2PTR(entity_var_id_by_name(entity, alias->target)), fct_id_eq);
        if (pos != 0) {
            FitFct *fct = entity->fcts->items[pos - 1];

            list_del_shift(entity->fcts, pos);
            fct_free(&fct);
        }
    }
    for (i = 0; i < tt_count(net->aliases); i++) {
        FitAlias *alias = net->aliases->items[i];
        FitAlias *driven;
        FitInstance *inst;
        FitVariable *va, *vb;
        FitFct *fct;
        const char *signal;
        int pos, id, new_id;

        if (alias->target != NULL)
            continue;
        inst = netlist_find_instance(entity, alias->name);
        if (inst == NULL)
            continue;
        signal = inst->pin;
        if (signal == NULL)
            signal = inst->d2;
        if (signal == NULL)
            signal = inst->d;
        pos = list_find(entity->net->aliases, (void *)signal, alias_name_eq);
        if (pos == 0)
            tt_unreachable("tt3_resolve_aliases: register feedback without alias entry");
        driven = entity->net->aliases->items[pos - 1];
        if (driven->target == NULL || latch_find_alias(driven->target, entity->net) != 0) {
            new_id = entity_var_id_by_name(entity, driven->name);
            lib_move_variable(new_id, entity->inputs, entity->outputs);
        } else {
            new_id = entity_var_id_by_name(entity, driven->target);
        }
        id = entity_var_id_by_name(entity, alias->name);
        fct = entity_find_fct(entity, id);
        va = entity_var_by_id(entity, id);
        vb = entity_var_by_id(entity, new_id);
        if (fct == NULL || va == NULL || vb == NULL)
            tt_unreachable("tt3_resolve_aliases: register output without function or variable");
        fct->var_id = new_id;
        vb->polarity = va->polarity;
        list_del_shift(entity->outputs, list_find(entity->outputs, FIT_INT2PTR(id), var_id_eq));
    }
}

/* after the renaming, put every variable into the list it
 * belongs to.  `orig` is the entity as it was before. */
static void tt3_sort_variables(FitEntity *entity, FitEntity *orig)
{
    int i;

    if (entity->net == NULL)
        return;

    /* inputs nobody reads that are netlist signals are released */
    for (i = 0; i < tt_count(entity->inputs); i++) {
        FitVariable *v = entity->inputs->items[i];

        if (tt2_count_id_uses(entity, v->id) != 0)
            continue;
        if (list_find(entity->net->aliases, v->name, alias_name_eq) == 0)
            continue;
        var_free(&v);
        list_del_insert(entity->inputs, i + 1);
        entity->n_in = tt_count(entity->inputs);
        i--;
    }

    /* outputs: nodes become locals, netlist signals disappear, an output
     * without function is an input if it is read at all */
    i = 0;
    while (i < tt_count(entity->outputs)) {
        FitVariable *v = entity->outputs->items[i];

        if (entity_var_is_node(orig, v)) {
            list_del_shift(entity->outputs, i + 1);
            list_add(entity->locals, v);
        } else if (latch_find_alias(v->name, entity->net) != 0) {
            list_del_shift(entity->outputs, i + 1);
            tt3_drop_alias(entity->net->aliases, v->name);
        } else if (!tt3_var_has_fct(entity, v)) {
            list_del_shift(entity->outputs, i + 1);
            if (tt2_count_id_uses(entity, v->id) != 0)
                list_add(entity->inputs, v);
        } else {
            i++;
        }
    }

    /* inputs: the same; an input that has a function is an output.  (After
     * such a move the input that slid into its place is skipped.
     * Deliberate: the variable lists of the .tt3 file depend on it.) */
    i = 0;
    while (i < tt_count(entity->inputs)) {
        FitVariable *v = entity->inputs->items[i];

        if (entity_var_is_node(orig, v)) {
            list_del_shift(entity->inputs, i + 1);
            list_add(entity->locals, v);
        } else if (latch_find_alias(v->name, entity->net) != 0) {
            list_del_shift(entity->inputs, i + 1);
            tt3_drop_alias(entity->net->aliases, v->name);
        } else {
            if (tt3_var_has_fct(entity, v)) {
                list_del_shift(entity->inputs, i + 1);
                list_add(entity->outputs, v);
            }
            i++;
        }
    }

    /* inputs no function uses are dropped */
    i = 0;
    while (i < tt_count(entity->inputs)) {
        FitVariable *v = entity->inputs->items[i];

        if (entity_count_fct_using(entity, v->id) == 0)
            list_del_shift(entity->inputs, i + 1);
        else
            i++;
    }
}

/* the new variable <fmt % base> with this id, placed among the locals if
 * the node `var` is one in `orig`, else among the outputs */
static void add_named_var(FitEntity *entity, FitEntity *orig, FitVariable *var, int id,
                          int polarity, const char *fmt, const char *base)
{
    FitVariable *nv = var_new(0);

    nv->id = id;
    nv->polarity = polarity;
    snprintf(nv->name, sizeof nv->name, fmt, base);
    if (!entity_var_is_node(orig, var)) {
        list_add(entity->outputs, nv);
        entity->n_out++;
    } else {
        list_add(entity->locals, nv);
        entity->n_local++;
    }
}

/* one control input of an instance (clock, reset, preset, output enable,
 * clock enable) as the function <base><ext> */
static void add_control_fct(FitEntity *entity, FitEntity *orig, FitVariable *var,
                            const char *signal, int negate, const char *fmt, const char *base)
{
    FitVariable *sv;
    FitFct *fct;
    int id;

    if (signal == NULL)
        return;
    sv = entity_find_var(entity, signal);
    if (sv == NULL)
        tt_unreachable("add_control_fct: control signal of an instance is not a variable");
    id = tt2_next_var_id(entity);
    fct = fct_new();
    fct->var_id = id;
    if (list_find(entity->inputs, FIT_INT2PTR(sv->id), var_id_eq) == 0) {
        /* a product term: its function moves to the new name */
        FitFct *src = entity_find_fct(entity, sv->id);

        if (src == NULL)
            tt_unreachable("add_control_fct: control signal without function");
        fct->tree[0] = fcttree_copy(src->tree[0], NULL);
        sv->cls = 2;
        flatten_replace_id(sv->id, id, entity);
    } else if (negate) {
        fct->tree[0] = fcttree_new_not(fcttree_new_id(sv->id));
    } else {
        fct->tree[0] = fcttree_new_id(sv->id);
    }
    add_named_var(entity, orig, var, id, sv->polarity, fmt, base);
    list_add(entity->fcts, fct);
}

/* for every function whose variable belongs to a netlist
 * instance, rename the function to <node>.D (.T, .L, .TH), add functions
 * for the control terms and rename the feedback variables. */
static void tt3_name_register_inputs(FitEntity *entity)
{
    FitEntity *orig;
    char base[FIT_NAME_MAX];
    int k, i;

    if (entity->net == NULL)
        return;
    orig = entity_copy(entity);     /* never released */
    latch_remove_buffers(entity);
    tt3_resolve_aliases(entity);

    /* the functions added on the way are visited too */
    for (k = 0; k < tt_count(entity->fcts); k++) {
        FitFct *fct = entity->fcts->items[k];
        FitVariable *var = entity_var_by_id(entity, fct->var_id);
        FitInstance *inst;
        const char *clock_fmt;
        int kind;

        if (var == NULL)
            tt_unreachable("tt3_name_register_inputs: function without variable");
        if (tt3_is_alias_target(entity, var->name)) {
            inst = tt3_find_instance_by_alias(entity, var->name, &kind);
            if (inst == NULL)
                tt_unreachable("tt3_name_register_inputs: alias target without instance");
        } else {
            if (list_find(entity->net->aliases, var->name, alias_name_eq) == 0)
                continue;
            inst = lib_instance_by_output(entity, var->name);
            if (inst == NULL)
                inst = tt3_find_instance_by_pin(entity, var->name);
            if (inst == NULL)
                continue;
        }
        snprintf(base, sizeof base, "%s", var->name);

        kind = tt3_gate_kind_of_output(entity, inst->q);
        if (kind != 0) {
            int id = tt2_next_var_id(entity);
            const char *fmt;

            fct->var_id = id;
            if (kind == FIT_GATE_TFF)
                fmt = "%s.T";
            else if (kind == FIT_GATE_LATCH)
                fmt = "%s.L";
            else if (kind == FIT_GATE_TH || kind == FIT_GATE_ITH)
                fmt = "%s.TH";
            else
                fmt = "%s.D";
            add_named_var(entity, orig, var, id, var->polarity, fmt, base);
        }

        if (kind == FIT_GATE_LATCH)
            clock_fmt = g_opt.cupl == 0 ? "%s.LH" : "%s.LE";
        else
            clock_fmt = "%s.C";
        add_control_fct(entity, orig, var, inst->clk, inst->clk_kind == 2 || inst->clk_kind == 3,
                        clock_fmt, base);
        add_control_fct(entity, orig, var, inst->ar, inst->ar_pol == -1, "%s.AR", base);
        add_control_fct(entity, orig, var, inst->ap, inst->ap_pol == -1, "%s.AP", base);
        add_control_fct(entity, orig, var, inst->oe, inst->oe_pol == -1, "%s.OE", base);
        add_control_fct(entity, orig, var, inst->ce, inst->ce_pol == -1, "%s.CE", base);

        if (inst->d != NULL) {
            FitVariable *dv = entity_var_by_id(entity, entity_var_id(entity, inst->d));

            if (dv == NULL)
                tt_unreachable("tt3_name_register_inputs: feedback of an instance is not a variable");
            if (kind == FIT_GATE_TH || kind == FIT_GATE_ITH || tt3_is_open_collector(orig, var))
                snprintf(dv->name, sizeof dv->name, "%s.PIN", base);
            else
                snprintf(dv->name, sizeof dv->name, "%s.Q", base);
        }
        if (inst->pin != NULL) {
            FitVariable *pv = entity_var_by_id(entity, entity_var_id(entity, inst->pin));

            if (pv == NULL)
                tt_unreachable("tt3_name_register_inputs: pad of an instance is not a variable");
            snprintf(pv->name, sizeof pv->name, "%s.PIN", base);
        }
    }

    /* the outputs whose function moved to a control term */
    i = 0;
    while (i < tt_count(entity->outputs)) {
        FitVariable *v = entity->outputs->items[i];

        if (v->cls == 2) {
            int pos = list_find(entity->fcts, FIT_INT2PTR(v->id), fct_id_eq);

            if (pos != 0)
                list_del_shift(entity->fcts, pos);
            list_del_shift(entity->outputs, i + 1);
        } else {
            i++;
        }
    }
    tt3_sort_variables(entity, orig);
}

/* an output <x>.TH (tri-state without register) is written as
 * <x>; the output <x> itself is removed */
static void tt3_strip_th_outputs(FitEntity *entity)
{
    char plain[FIT_NAME_MAX];
    int k;

    for (k = 0; k < tt_count(entity->outputs); k++) {
        FitVariable *v = entity->outputs->items[k];
        size_t len;
        int pos;

        if (strstr(v->name, ".TH") == NULL)
            continue;
        len = strlen(v->name);
        if (len < 3)
            continue;
        snprintf(plain, sizeof plain, "%s", v->name);
        plain[len - 3] = '\0';
        pos = list_find(entity->outputs, plain, var_has_name);
        if (pos != 0)
            list_del_shift(entity->outputs, pos);
        v->name[len - 3] = '\0';
    }
}

void tt3_print_programmed_logic(FILE *out, FitEntity *entity)
{
    FitEntity *copy = entity_copy(entity);
    int i;

    if (copy == NULL)
        return;
    tt3_name_register_inputs(copy);
    tt3_strip_th_outputs(copy);
    fprintf(out, "\n");
    fprintf(out, "%s programmed logic:\n", g_dev_package);
    fprintf(out, "-----------------------------------\n");
    for (i = 0; i < tt_count(g_fit_foldback_ids); i++)
        entity_toggle_inv(copy, FIT_PTR2INT(g_fit_foldback_ids->items[i]));
    for (i = 0; i < tt_count(copy->fcts); i++) {
        FitFct *fct = copy->fcts->items[i];
        int folded = list_find(g_fit_foldback_ids, FIT_INT2PTR(fct->var_id), list_ptr_eq);
        FitVariable *v = entity_var_by_id(copy, fct->var_id);

        if (v == NULL)
            tt_unreachable("tt3_print_programmed_logic: function without variable");
        tt3_print_fct(out, copy, fct, v->polarity == 1 && folded == 0 ? 1 : -1);
        fprintf(out, "\n");
    }
    entity_free(&copy);
}

/* ------------------------------------------------------------------ */
/* The .tt3 file                                                       */
/* ------------------------------------------------------------------ */

/* pin of the slot that holds `name`, 0 if none */
static int tt3_pin_of_signal(const char *name)
{
    int i;

    for (i = 0; i < tt_count(g_fit_slots); i++) {
        FitSlot *slot = g_fit_slots->items[i];

        if (slot->pin != 0 && util_str_icmp(name, slot->signal) == 0)
            return slot->pin;
    }
    return 0;
}

/* feedback node number (600 + macrocell) of the slot whose
 * buried (feedback) signal is `name`, 0 if none */
static int tt3_node_of_signal(const char *name)
{
    int i;

    for (i = 0; i < tt_count(g_fit_slots); i++) {
        FitSlot *slot = g_fit_slots->items[i];

        if (util_str_icmp(name, slot->buried) == 0)
            return slot->macrocell + 600;
    }
    return 0;
}

/* is this the name of a signal that stands for its node in the PINS /
 * NODES line: the data input of a register (.D, .T, .L but not .LE/.LH)
 * or a name without extension */
static int names_a_node(const char *name)
{
    if (strstr(name, ".D") != NULL || strstr(name, ".T") != NULL)
        return 1;
    if (strstr(name, ".L") != NULL && strstr(name, ".LE") == NULL
        && strstr(name, ".LH") == NULL)
        return 1;
    return strstr(name, ".") == NULL;
}

/* copy of the variable under the name without extension */
static FitVariable *copy_with_base_name(FitVariable *v)
{
    FitVariable *copy = var_copy(v);
    char *base = util_base_name(v->name);

    snprintf(copy->name, sizeof copy->name, "%s", base);
    free(base);
    return copy;
}

/* position (0-based) of the first variable of the list whose name without
 * extension is `name`; the count of the list if there is none */
static int find_by_base_name(FitList *vars, const char *name)
{
    int j;

    for (j = 0; j < tt_count(vars); j++) {
        char *base = util_base_name(((FitVariable *)vars->items[j])->name);
        int same = util_str_ieq(base, name);

        free(base);
        if (same)
            break;
    }
    return j;
}

/* sort the variables of the renamed entity for the PLA:
 *   *pins       copies for the "#$ PINS" line
 *   *nodes      copies for the "#$ NODES" line
 *   *extra_in   variables that become additional inputs (.ilb)
 *   *extra_out  locals that become additional outputs (.ob) */
static void tt3_classify_variables(FitEntity *entity, FitList **pins, FitList **extra_in,
                                   FitList **extra_out, FitList **nodes)
{
    static const char *const out_ext[] = { ".D", ".T", ".C", ".AR", ".OE", ".CE", ".AP", ".L" };
    int i;
    size_t x;

    *extra_in = list_create();
    *extra_out = list_create();
    *pins = list_create();
    *nodes = list_create();

    for (i = 0; i < tt_count(entity->locals); i++) {
        FitVariable *v = entity->locals->items[i];
        int is_out = 0;

        for (x = 0; x < sizeof out_ext / sizeof out_ext[0]; x++)
            if (strstr(v->name, out_ext[x]) != NULL)
                is_out = 1;
        list_add(is_out ? *extra_out : *extra_in, v);
    }
    for (i = 0; i < tt_count(*extra_in); i++) {
        FitVariable *v = (*extra_in)->items[i];

        if (find_by_base_name(*extra_out, v->name) == tt_count(*extra_out))
            list_add(*extra_out, v);
    }

    for (i = 0; i < tt_count(entity->outputs); i++) {
        FitVariable *v = entity->outputs->items[i];

        if (names_a_node(v->name))
            list_add(*pins, copy_with_base_name(v));
    }
    for (i = 0; i < tt_count(entity->inputs); i++) {
        FitVariable *v = entity->inputs->items[i];

        if (strstr(v->name, ".PIN") != NULL || strstr(v->name, ".Q") != NULL)
            continue;
        if (find_by_base_name(entity->outputs, v->name) == tt_count(entity->outputs))
            list_add(*pins, v);
    }
    for (i = 0; i < tt_count(entity->locals); i++) {
        FitVariable *v = entity->locals->items[i];

        if (names_a_node(v->name))
            list_add(*nodes, copy_with_base_name(v));
    }

    /* outputs and locals that are read by a function are inputs as well */
    for (i = 0; i < tt_count(entity->outputs); i++) {
        FitVariable *v = entity->outputs->items[i];

        if (strstr(v->name, ".") == NULL && entity_count_fct_using(entity, v->id) != 0)
            list_add(*extra_in, var_copy(v));
    }
    for (i = 0; i < tt_count(entity->outputs); i++) {
        FitVariable *v = entity->outputs->items[i];

        if (strstr(v->name, ".") != NULL && entity_count_fct_using(entity, v->id) != 0)
            list_add(*extra_in, var_copy(v));
    }
    for (i = 0; i < tt_count(entity->locals); i++) {
        FitVariable *v = entity->locals->items[i];

        if (strstr(v->name, ".") != NULL && entity_count_fct_using(entity, v->id) != 0)
            list_add(*extra_in, var_copy(v));
    }
}

/* the output part of a product term as text: '1' (or '0' for an
 * output whose bit is set in `inverted`), '-' and '~' */
static void tt3_pterm_out_to_string(const FitPTerm *p, unsigned n, char *text, int warn,
                                    const unsigned *inverted)
{
    int bad = 0;
    unsigned b;

    for (b = 0; b < n; b++) {
        unsigned mask = 1u << (b & 31);
        unsigned bits = p->bits[b >> 5] & mask;
        unsigned care = p->care[b >> 5] & mask;

        if (bits == 0) {
            text[b] = care == 0 ? '~' : '-';
        } else if (care == 0) {
            text[b] = '~';
            bad = 1;
        } else if (g_tt3_second_pla == 0 && (inverted == NULL || (inverted[b >> 5] & mask) == 0)) {
            text[b] = '1';
        } else {
            text[b] = '0';
        }
    }
    text[n] = '\0';
    if (bad && warn)
        fprintf(g_msg, "Function PTermToString : Error in PTerm Value\n");
}

static void tt3_print_pterm_out(FILE *out, const FitPTerm *p, unsigned n,
                                const unsigned *inverted)
{
    char *text = util_calloc(1, (size_t)n + 1);

    tt3_pterm_out_to_string(p, n, text, 1, inverted);
    fprintf(out, "%s", text);
    free(text);
}

/* one PLA line: input part, tab, output part */
static void tt3_print_pterm(FILE *out, FitPTermIO *io, unsigned n_in, unsigned n_out,
                            const unsigned *inverted)
{
    pterm_print(out, &io->in, n_in);
    fprintf(out, "\t");
    tt3_print_pterm_out(out, &io->out, n_out, inverted);
}

/* the PLA of the renamed entity.  The terms of a second entity could
 * follow those of the first; tt3_write never has one (see
 * g_tt3_second_pla), so that part is not implemented.  With `extra`
 * non-NULL the PINS and NODES lines are left out. */
static void tt3_write_pla(FILE *out, FitEntity *entity, char *extra)
{
    FitList *pins, *extra_in, *extra_out, *nodes;
    FitList *in_copies, *out_copies, *pla, *dc;
    unsigned *inverted;
    char *row, *in_row;
    int n_nodes, n_pins, i;

    tt3_classify_variables(entity, &pins, &extra_in, &extra_out, &nodes);
    n_nodes = tt_count(nodes);
    n_pins = tt_count(pins);
    in_copies = list_map_copy(extra_in, tt_cb_copy_var);
    out_copies = list_map_copy(extra_out, tt_cb_copy_var);
    list_append_destroy(entity->inputs, &in_copies);
    list_append_destroy(entity->outputs, &out_copies);
    list_destroy(&entity->locals);
    entity->n_in = tt_count(entity->inputs);
    entity->n_out = tt_count(entity->outputs);
    entity->n_local = 0;
    tt3_strip_th_outputs(entity);
    inverted = latch_inverted_set(entity->outputs);

    if (extra == NULL) {
        fprintf(out, "#$ PINS %d ", n_pins);
        for (i = 0; i < tt_count(pins); i++) {
            FitVariable *v = pins->items[i];
            fprintf(out, "%s:%d ", v->name, tt3_pin_of_signal(v->name));
        }
        fprintf(out, "\n");
        fprintf(out, "#$ NODES %d ", n_nodes);
        for (i = 0; i < tt_count(nodes); i++) {
            FitVariable *v = nodes->items[i];
            int node = tt3_node_of_signal(v->name);

            if (node == 0)
                fprintf(out, "%s ", v->name);
            else
                fprintf(out, "%s:%d ", v->name, node);
        }
        fprintf(out, "\n");
    }
    fprintf(out, ".type fr\n");
    fprintf(out, ".i %d\n", entity->n_in);
    fprintf(out, ".o %d\n", entity->n_out);
    if (entity->inputs != NULL && entity->inputs->count != 0) {
        fprintf(out, ".ilb ");
        list_print(out, entity->inputs, tt_cb_print_var, " ");
        fprintf(out, "\n");
    }
    if (entity->outputs != NULL && entity->outputs->count != 0) {
        fprintf(out, ".ob ");
        list_print(out, entity->outputs, tt_cb_print_var, " ");
        fprintf(out, "\n");
    }

    pla = tt2_pla_from_fct(entity, 0);
    dc = tt2_pla_from_fct(entity, 2);
    if (pla == NULL)
        tt_unreachable("tt3_write_pla: function of a variable that is not an output");
    for (i = 0; i < tt_count(dc); i++)
        list_add(pla, dc->items[i]);
    list_destroy(&dc);

    /* The input part covers outputs and inputs; cut it down to the inputs. */
    row = util_calloc(1, (size_t)entity->n_in + 1 + (size_t)entity->n_out);
    in_row = util_calloc(1, (size_t)entity->n_in + 1);
    for (i = 0; i < tt_count(pla); i++) {
        FitPTermIO *io = pla->items[i];
        FitPTerm *p;

        pterm_to_string(&io->in, (unsigned)(entity->n_in + entity->n_out), row, 1);
        strcpy(in_row, row + entity->n_out);
        p = pterm_new(tt_words(entity->n_in));
        pterm_build(p, in_row, (unsigned)entity->n_in);
        pterm_release(&io->in);
        io->in = *p;
        free(p);
    }
    list_destroy(&extra_in);
    list_destroy(&extra_out);
    list_destroy(&pins);        /* `nodes` is not released */

    g_tt3_n_in = entity->n_in;
    g_tt3_n_out = entity->n_out;
    g_tt3_n_nodes = n_nodes;
    g_tt3_n_pins = n_pins;
    g_tt3_n_pterms = tt_count(pla);

    fprintf(out, ".p %d\n", tt_count(pla));
    for (i = 0; i < tt_count(pla); i++) {
        tt3_print_pterm(out, pla->items[i], (unsigned)entity->n_in, (unsigned)entity->n_out,
                        inverted);
        fprintf(out, "\n");
    }
    fprintf(out, ".end\n");
    list_free_all(&pla, tt_cb_free_ptermio);
    free(row);
    free(in_row);
    free(inverted);
}

void tt3_write(const char *base, FitEntity *entity, char *extra)
{
    FitEntity *copy;
    char *file_name, *module;
    FILE *out;

    if (strstr(base, ".tt") == NULL) {
        char *with_ext = util_strdup(base);

        util_str_append(&with_ext, ".tt");
        file_name = util_str_with_index(with_ext, 3);
        free(with_ext);
    } else {
        file_name = util_strdup(base);
    }
    copy = entity_copy(entity);
    netlist_absorb_tristates(copy);
    out = file_open_write(file_name);
    free(file_name);
    if (out == NULL) {
        entity_free(&copy);
        return;
    }
    fprintf(out, g_opt.cupl != 0 ? "#$ TOOL CUPL %s %s %s\n" : "#$ TOOL ABEL %s %s %s\n",
            PROGRAM_NAME, g_family, g_version);
    tt3_name_register_inputs(copy);
    module = util_base_name(base);
    fprintf(out, "#$ MODULE %s\n", module);
    free(module);
    module = util_base_name(base);
    fprintf(out, "#$ JEDECFILE %s\n", module);
    free(module);
    tt3_write_pla(out, copy, extra);
    entity_free(&copy);
    util_fclose(out);
}
