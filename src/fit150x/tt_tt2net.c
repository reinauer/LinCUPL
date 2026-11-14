/*
 * tt_tt2net.c - tt2_read, names with extensions: after the PLA has been
 * turned into functions, the signals called <node>.<ext> are grouped per
 * node and become netlist
 * instances (registers, latches, tri-states), XOR functions (.X1/.X2) or
 * plain signals; then the "+"/"-" and PINS/NODES declarations are applied.
 *
 * Extensions (case-insensitive, tt2_extension_code):
 *   .AP .PR .SP  preset        .AR .RE .SR  reset       .CE  clock enable
 *   .CLK .C .CP  clock         .D .REG  D input         .T   T input
 *   .J .K        JK inputs     .R .S    RS inputs       .L .G  latch input
 *   .LE .LH      latch enable  .OE  output enable       .PIN pad
 *   .Q .FB       feedback      .FC .LD  unknown (warning)
 */
#include "tt_priv.h"

static void tt2_dots_to_underscores(char *s)
{
    for (; *s != '\0'; s++)
        if (*s == '.')
            *s = '_';
}

static int tt2_name_has_dot(const char *s)
{
    return strchr(s, '.') != NULL;
}

/* *ext = the last '.' of the name (NULL if none), *base = new
 * string with what precedes it ("" if there is no dot).  Most callers
 * never release *base. */
static void tt2_split_extension(char *name, char **ext, char **base)
{
    size_t len = strlen(name);
    size_t i;

    *ext = NULL;
    *base = util_calloc(1, len + 1);
    for (i = 0; i < len; i++) {
        if (name[i] == '.') {
            *ext = name + i;
            memcpy(*base, name, i);
            (*base)[i] = '\0';
        }
    }
}

/* strcmp for an extension; the NULL extension of a name without a dot is
 * an internal error */
static int ext_is(const char *ext, const char *wanted)
{
    if (ext == NULL)
        tt_unreachable("ext_is: extension of a name without a dot");
    return strcmp(ext, wanted) == 0;
}

/* code of the extension of a name (after its last dot); an
 * unknown one gives 0 and "Warning - <.EXT> extension unknown" */
static int tt2_extension_code(const char *name)
{
    static const struct { const char *ext; int code; } table[] = {
        { ".AP", 1 },  { ".AR", 2 },   { ".CE", 3 },    { ".CLK", 4 }, { ".C", 4 },
        { ".CP", 5 },  { ".D", 6 },    { ".FB", 7 },    { ".FC", 8 },  { ".J", 9 },
        { ".K", 10 },  { ".LD", 11 },  { ".LE", 12 },   { ".L", 0x19 }, { ".G", 0x19 },
        { ".LH", 13 }, { ".OE", 14 },  { ".PIN", 15 },  { ".PR", 16 }, { ".Q", 17 },
        { ".R", 18 },  { ".RE", 19 },  { ".REG", 20 },  { ".S", 21 },  { ".SP", 22 },
        { ".SR", 23 }, { ".T", 24 }
    };
    const char *dot = strrchr(name, '.');
    char *ext;
    size_t i;
    int code = 0;

    if (dot == NULL)
        return 0;
    ext = util_strdup(dot);
    lincupl_strupr(ext);
    for (i = 0; i < sizeof table / sizeof table[0]; i++) {
        if (strcmp(ext, table[i].ext) == 0) {
            code = table[i].code;
            break;
        }
    }
    if (i == sizeof table / sizeof table[0])
        fprintf(g_msg, "Warning - %s extension unknown\n", ext);
    free(ext);
    return code;
}

/* the variables of `list` that have the base name `base`
 * (case-sensitive) and an extension other than `ext` */
static FitList *tt2_collect_siblings(FitList *list, const char *base, const char *ext)
{
    FitList *siblings = list_create();
    int n = tt_count(list);
    int i;

    for (i = 0; i < n; i++) {
        FitVariable *v = list->items[i];
        char *v_ext, *v_base;

        tt2_split_extension(v->name, &v_ext, &v_base);
        if (strcmp(v_base, base) == 0 && !ext_is(v_ext, ext))
            list_add(siblings, v);
        free(v_base);
    }
    return siblings;
}

/* double the last '_' of the name (shifting what follows it) */
static void tt2_insert_underscore(char *s)
{
    int len = (int)strlen(s);
    int i;

    if (len + 1 >= FIT_NAME_MAX)
        return;         /* no room */
    s[len + 1] = '\0';
    for (i = len - 1; i >= 0; i--) {
        s[i + 1] = s[i];
        if (s[i] == '_')
            break;
    }
}

/* appends item (creating the list if needed) and counts it */
static FitList *tt2_list_add_counted(FitList *list, void *item, int *count)
{
    if (list == NULL)
        list = list_create();
    list_add(list, item);
    (*count)++;
    return list;
}

/* removes the variable of v's name (if present) and uncounts it */
static FitList *tt2_list_del_counted(FitList *list, FitVariable *v, int *count)
{
    if (list != NULL) {
        int pos = list_find(list, v, var_same_name);

        if (pos != 0) {
            list_del_shift(list, pos);
            (*count)--;
        }
    }
    return list;
}

/* FitListEq of a FitVariable against a name */
static int tt2_var_has_name_i(void *var, void *name)
{
    if (var == NULL || name == NULL)
        return 0;
    return util_str_icmp(((FitVariable *)var)->name, name) == 0;
}

/* new plain variable with the next free id */
static FitVariable *tt2_new_named_var(const char *name)
{
    FitVariable *v = var_new(0);

    v->id = g_tt2_next_id++;
    util_str_copy_n(v->name, name, FIT_NAME_MAX, 0);
    return v;
}

static int net_has_alias(FitEntity *entity, const char *name)
{
    return list_find(entity->net->aliases, (void *)name, alias_name_eq) != 0;
}

/* connect a pin of the instance to a signal that the design
 * computes; if the signal is an output it gets an alias entry */
static void tt2_set_pin_out(char *name, char **slot, FitEntity *entity)
{
    *slot = util_strdup(name);
    if (var_list_id_of_name(entity->outputs, name) != 0 && !net_has_alias(entity, name))
        lib_alias_add(entity->net, name, NULL);
}

/* connect a pin of the instance to a signal the instance drives */
static void tt2_set_pin(char *name, char **slot, FitEntity *entity)
{
    *slot = util_strdup(name);
    if (!net_has_alias(entity, name))
        lib_alias_add(entity->net, name, NULL);
}

/* checks that the data and pad signals of an instance are inputs, moves a
 * pad without output enable to d and gives a tri-state without register its
 * data input; 0 after an error message */
static int tt2_check_instance_pins(int kind, FitInstance *inst, FitEntity *entity)
{
    if (inst->d != NULL && var_list_find_name(entity->inputs, inst->d) == NULL) {
        fprintf(g_msg, "Error -  Dot extension %s can not be output\n", inst->d);
        return 0;
    }
    if (inst->pin != NULL) {
        if (var_list_find_name(entity->inputs, inst->pin) == NULL) {
            fprintf(g_msg, "Error -  Dot extension %s can not be output\n", inst->pin);
            return 0;
        }
        if (inst->oe == NULL && inst->d == NULL) {
            inst->d = util_strdup(inst->pin);
            flatten_free_ref((void **)&inst->pin);
        }
    }
    if (kind == 0 && inst->oe != NULL && inst->q == NULL) {
        /* a tri-state without register: the node's own function becomes
         * the data input */
        FitVariable *out = var_list_find_name(entity->outputs, inst->inst_name);
        FitVariable *v;
        FitFct *fct;

        if (out == NULL) {
            fprintf(g_msg, "Error -  no output assciated with the node or pin %s\n", inst->inst_name);
            return 0;
        }
        fct = entity_find_fct(entity, out->id);
        if (fct == NULL)
            tt_unreachable("tt2_check_instance_pins: output without function");
        v = lib_new_variable(entity, 1, 0);
        fct->var_id = v->id;
        g_tt2_next_id++;
        inst->q = util_strdup(v->name);
        lib_alias_add(entity->net, v->name, NULL);
    }
    return 1;
}

/* complete an instance: make sure the node has an output
 * variable driven by the instance, set the polarity fields, and merge a
 * .PIN input into the feedback when there is no output enable.  Returns
 * the kind of the netlist item (0xc: a plain buffer, 8: tri-state without
 * register), -1 after an error. */
static int tt2_finish_instance(int kind, FitInstance *inst, FitEntity *entity)
{
    FitVariable *out, *src;
    int pos;

    if (!tt2_check_instance_pins(kind, inst, entity))
        return -1;
    if (tt2_find_node_decl(inst->inst_name) == 0) {
        fprintf(g_msg, "Syntax error ...\n");
        return -1;
    }
    if (inst->d == NULL && inst->pin == NULL) {
        FitVariable *v = lib_new_variable(entity, 0, 0);

        g_tt2_next_id++;
        if (inst->oe == NULL)
            inst->d = util_strdup(v->name);
        else
            inst->pin = util_strdup(v->name);
        lib_alias_add(entity->net, v->name, NULL);
    }

    pos = list_find(entity->inputs, inst->inst_name, var_has_name);
    if (pos == 0) {
        out = var_list_find_name(entity->outputs, inst->inst_name);
        if (out == NULL) {
            out = tt2_new_named_var(inst->inst_name);
            entity->outputs = tt2_list_add_counted(entity->outputs, out, &entity->n_out);
        }
    } else {
        out = entity->inputs->items[pos - 1];
        list_del_shift(entity->inputs, pos);
        pos = list_find(entity->outputs, inst->inst_name, var_has_name);
        if (pos == 0) {
            entity->outputs = tt2_list_add_counted(entity->outputs, out, &entity->n_out);
        } else if (pos <= tt_count(entity->inputs)) {
            /* The position found in the output list indexes the *input*
             * list.  Deliberate: the netlist generated from such a design
             * depends on it. */
            ((FitVariable *)entity->inputs->items[pos - 1])->id = out->id;
        }
    }

    src = var_list_find_name(entity->inputs, inst->pin != NULL ? inst->pin : inst->d);
    if (src == NULL)
        tt_unreachable("tt2_finish_instance: feedback signal of a node is not an input");
    techmap_add_wire_fct(entity, out, src);
    lib_alias_set_target(entity->net, src->name, out->name);

    if (inst->clk_kind == 0 && inst->clk != NULL)
        inst->clk_kind = 1;
    if (inst->ar != NULL)
        inst->ar_pol = 1;
    if (inst->ap != NULL)
        inst->ap_pol = 1;
    if (inst->ce != NULL)
        inst->ce_pol = 1;
    if (inst->q2 != NULL)
        inst->q2_pol = 1;
    if (inst->q3 != NULL)
        inst->q3_pol = 1;
    if (inst->oe != NULL)
        inst->oe_pol = 1;

    if (kind == 0) {
        kind = inst->oe != NULL ? 8 : 0xc;
        if (inst->q == NULL) {
            FitVariable *v = lib_new_variable(entity, 0, 1);

            g_tt2_next_id++;
            inst->q = util_strdup(v->name);
        }
        if (inst->d == NULL && inst->pin != NULL) {
            inst->d = util_strdup(inst->pin);
            flatten_free_ref((void **)&inst->pin);
        }
    }
    if (inst->d == NULL)
        return kind;

    if (inst->pin != NULL && inst->oe == NULL) {
        /* x.PIN and x.Q are the same signal when the pin cannot be
         * tri-stated: keep the feedback, drop the pin input */
        FitVariable *pin_var, *d_var;
        FitAlias *alias;
        int apos, dpos;

        pos = list_find(entity->inputs, inst->pin, var_has_name);
        if (pos == 0)
            tt_unreachable("tt2_finish_instance: .PIN signal is not an input");
        pin_var = entity->inputs->items[pos - 1];
        list_del_shift(entity->inputs, pos);
        d_var = var_list_find_name(entity->inputs, inst->d);
        if (d_var == NULL)
            tt_unreachable("tt2_finish_instance: feedback signal is not an input");
        flatten_replace_id(pin_var->id, d_var->id, entity);
        flatten_free_ref((void **)&inst->pin);
        apos = list_find(entity->net->aliases, pin_var->name, alias_name_eq);
        if (apos == 0)
            tt_unreachable("tt2_finish_instance: .PIN signal without alias entry");
        alias = entity->net->aliases->items[apos - 1];
        list_del_shift(entity->net->aliases, apos);
        dpos = list_find(entity->net->aliases, d_var->name, alias_name_eq);
        if (alias->target != NULL) {
            if (dpos == 0)
                tt_unreachable("tt2_finish_instance: feedback signal without alias entry");
            ((FitAlias *)entity->net->aliases->items[dpos - 1])->target = alias->target;
        }
    }
    return kind;
}

/* an "instance" that turned out to be a wire: q drives d */
static void tt2_instance_to_buffer(FitInstance *inst, FitEntity *entity)
{
    FitVariable *d_var = entity_find_var(entity, inst->d);
    FitVariable *q_var = entity_find_var(entity, inst->q);

    if (d_var == NULL || q_var == NULL)
        tt_unreachable("tt2_instance_to_buffer: buffer between unknown signals");
    if (list_find(entity->inputs, inst->d, tt2_var_has_name_i) != 0) {
        if (!tt2_net_uses_signal(entity, inst->d))
            entity->locals = tt2_list_add_counted(entity->locals, d_var, &entity->n_local);
        else
            entity->outputs = tt2_list_add_counted(entity->outputs, d_var, &entity->n_out);
        entity->inputs = tt2_list_del_counted(entity->inputs, d_var, &entity->n_in);
    }
    tt2_add_buffer_fct(entity, d_var, q_var);
    if (!tt2_net_uses_signal(entity, inst->q)
        && list_find(entity->outputs, q_var->name, tt2_var_has_name_i) != 0) {
        lib_alias_remove(entity->net, inst->q);
        entity->locals = tt2_list_add_counted(entity->locals, q_var, &entity->n_local);
        entity->outputs = tt2_list_del_counted(entity->outputs, q_var, &entity->n_out);
    }
    if (!tt2_net_uses_signal(entity, inst->d))
        lib_alias_remove(entity->net, inst->d);
}

static void ensure_netlist(FitEntity *entity)
{
    if (entity->net == NULL) {
        entity->net = netlist_new();
        entity->net->items = list_create();
    }
}

/* build the instance of node `base` from its signals `signals`
 * (all the dotted names with that base).  Each signal loses its dot
 * (a.b -> a_b, made unique) and is removed from `dotted`. */
static int tt2_make_instance(FitList *signals, char *base, FitEntity *entity, FitList *dotted)
{
    FitNetItem *item = netitem_new();
    FitInstance *inst = instance_new();
    int n, k, kind;

    inst->inst_name = base;
    item->cell = inst;
    item->kind = 0;
    if (entity->net == NULL)
        ensure_netlist(entity);
    else if (entity->net->items == NULL)
        entity->net->items = list_create();

    n = tt_count(signals);
    for (k = 0; k < n; k++) {
        FitVariable *v = signals->items[k];
        char *name = v->name;
        int code = tt2_extension_code(name);
        FitVariable *copy = var_copy(v);

        tt2_dots_to_underscores(copy->name);
        while (list_find(entity->inputs, copy, var_same_name) != 0
               || list_find(entity->outputs, copy, var_same_name) != 0
               || list_find(entity->locals, copy, var_same_name) != 0)
            tt2_insert_underscore(copy->name);
        strcpy(name, copy->name);
        var_free(&copy);

        switch (code) {
        case 1:                 /* .AP */
            tt2_set_pin_out(name, &inst->ap, entity);
            break;
        case 2:                 /* .AR */
            tt2_set_pin_out(name, &inst->ar, entity);
            break;
        case 3:                 /* .CE */
            tt2_set_pin_out(name, &inst->ce, entity);
            break;
        case 4:                 /* .CLK .C */
        case 5:                 /* .CP */
            tt2_set_pin_out(name, &inst->clk, entity);
            break;
        case 6:                 /* .D */
        case 0x14:              /* .REG */
            tt2_set_pin_out(name, &inst->q, entity);
            if (item->kind == 0)
                item->kind = FIT_GATE_DFF;
            break;
        case 7:                 /* .FB */
            tt2_set_pin(name, &inst->d, entity);
            break;
        case 9:                 /* .J */
            tt2_set_pin_out(name, &inst->q2, entity);
            item->kind = FIT_GATE_JK;
            break;
        case 10:                /* .K */
            tt2_set_pin_out(name, &inst->q3, entity);
            item->kind = FIT_GATE_JK;
            break;
        case 0xc:               /* .LE */
            tt2_set_pin_out(name, &inst->clk, entity);
            inst->clk_kind = g_opt.cupl == 0 ? 3 : 4;
            item->kind = FIT_GATE_LATCH;
            break;
        case 0xd:               /* .LH */
            tt2_set_pin_out(name, &inst->clk, entity);
            inst->clk_kind = 4;
            item->kind = FIT_GATE_LATCH;
            break;
        case 0xe:               /* .OE */
            tt2_set_pin_out(name, &inst->oe, entity);
            break;
        case 0xf:               /* .PIN */
            tt2_set_pin(name, &inst->pin, entity);
            break;
        case 0x10:              /* .PR */
        case 0x16:              /* .SP */
            fprintf(g_msg, "Warning -  PRESET %s will be used as asynchronous PRESET\n", name);
            tt2_set_pin_out(name, &inst->ap, entity);
            break;
        case 0x11:              /* .Q */
            tt2_set_pin(name, &inst->d, entity);
            item->kind = FIT_GATE_DFF;
            break;
        case 0x12:              /* .R */
            tt2_set_pin_out(name, &inst->q2, entity);
            item->kind = FIT_GATE_RS;
            break;
        case 0x13:              /* .RE */
        case 0x17:              /* .SR */
            fprintf(g_msg, "Warning -  RESET %s will be used as asynchronous RESET\n", name);
            tt2_set_pin_out(name, &inst->ar, entity);
            break;
        case 0x15:              /* .S */
            tt2_set_pin_out(name, &inst->q3, entity);
            item->kind = FIT_GATE_RS;
            break;
        case 0x18:              /* .T */
            tt2_set_pin_out(name, &inst->q, entity);
            item->kind = FIT_GATE_TFF;
            break;
        case 0x19:              /* .L .G */
            tt2_set_pin_out(name, &inst->q, entity);
            if (item->kind == 0)
                item->kind = FIT_GATE_LATCH;
            break;
        default:                /* .FC .LD */
            fprintf(g_msg, "Warning - %s extension unknown\n", name);
            break;
        }
        list_del_shift(dotted, list_find(dotted, v, var_same_name));
    }

    if (item->kind == 0
        && (inst->clk != NULL || inst->ar != NULL || inst->ap != NULL || inst->ce != NULL))
        item->kind = FIT_GATE_DFF;
    switch (item->kind) {
    case FIT_GATE_TFF:
    case FIT_GATE_DFF:
        if (inst->clk == NULL) {
            fprintf(g_msg, "Error - Node %s is registered and there is no .CLK in .olb\n",
                    inst->inst_name);
            return 0;
        }
        if (inst->q == NULL) {
            fprintf(g_msg, "Error - Node %s is registered and there is no .D or .T in .olb\n",
                    inst->inst_name);
            return 0;
        }
        break;
    case FIT_GATE_LATCH:
        if (inst->clk == NULL) {
            fprintf(g_msg, "Error - Node %s is latched and there is no .LE or .LH in .olb\n",
                    inst->inst_name);
            return 0;
        }
        break;
    case FIT_GATE_JK:
        if (inst->q3 == NULL) {
            fprintf(g_msg, FIT_EXT ? " Error - Node %s is on a JK flip flop and there is no .K in .olb\n"
                                     : "Error - Node %s is on a JK flip flop and there is no .K in .olb\n",
                    inst->inst_name);
            return 0;
        }
        if (inst->q2 == NULL) {
            fprintf(g_msg, "Error - Node %s is on a JK flip flop and there is no .J in .olb\n",
                    inst->inst_name);
            return 0;
        }
        break;
    case FIT_GATE_RS:
        if (inst->q3 == NULL) {
            fprintf(g_msg, "Error - Node %s is on a RS flip flop and there is no .S in .olb\n",
                    inst->inst_name);
            return 0;
        }
        if (inst->q2 == NULL) {
            fprintf(g_msg, "Error - Node %s is on a RS flip flop and there is no .R in .olb\n",
                    inst->inst_name);
            return 0;
        }
        break;
    default:
        break;
    }

    kind = tt2_finish_instance(item->kind, inst, entity);
    item->kind = kind;
    if (kind != 0) {
        if (kind == -1)
            return 0;
        if (kind == FIT_GATE_WIRE) {
            tt2_instance_to_buffer(inst, entity);
            free(item);
            free(inst);
            return 1;
        }
        list_add(entity->net->items, item);
    }
    return 1;
}

/* deal with one dotted name and all its siblings */
static int tt2_build_instance(FitVariable *v, FitList *dotted, FitEntity *entity)
{
    char *ext, *base;
    FitList *signals;

    tt2_split_extension(v->name, &ext, &base);
    if (tt2_extension_code(v->name) == 0) {
        tt2_dots_to_underscores(v->name);
        list_del_shift(dotted, list_find(dotted, v, var_same_name));
        return 1;
    }
    signals = tt2_collect_siblings(dotted, base, ext);
    list_add(signals, v);
    if (!tt2_make_instance(signals, base, entity, dotted))
        return 0;
    list_destroy(&signals);
    return 1;
}

int tt2_build_instances(FitEntity *entity, FitList *dotted)
{
    while (dotted != NULL && dotted->count != 0) {
        if (!tt2_build_instance(dotted->items[dotted->count - 1], dotted, entity))
            return 0;
    }
    return 1;
}

/* 1 if none of the signals is a clock, latch enable or output
 * enable (exact, upper-case extensions) */
static int tt2_only_pin_feedback(FitList *signals)
{
    int i;

    for (i = 0; i < tt_count(signals); i++) {
        FitVariable *v = signals->items[i];
        char *ext, *base;
        int control;

        tt2_split_extension(v->name, &ext, &base);
        free(base);
        control = ext_is(ext, ".C") || ext_is(ext, ".CLK") || ext_is(ext, ".LE")
                  || ext_is(ext, ".LH") || ext_is(ext, ".OE");
        if (control)
            return 0;
    }
    return 1;
}

/* x.PIN of a node without clock, latch enable or output enable
 * is the signal x itself.  Returns 1 if v was removed from `dotted`. */
static int tt2_merge_pin_input(FitVariable *v, FitList *dotted, FitEntity *entity)
{
    char *ext, *base;
    FitList *signals;
    FitVariable *plain;

    tt2_split_extension(v->name, &ext, &base);
    if (!ext_is(ext, ".PIN"))
        return 0;
    signals = tt2_collect_siblings(dotted, base, ext);
    list_add(signals, v);
    if (!tt2_only_pin_feedback(signals)) {
        list_destroy(&signals);
        return 0;
    }
    plain = entity_find_var(entity, base);
    if (plain == NULL) {
        list_del_shift(dotted, list_find(dotted, v, var_same_name));
        strcpy(v->name, base);
    } else {
        flatten_replace_id(v->id, plain->id, entity);
        list_del_shift(entity->inputs, list_find(entity->inputs, v, var_same_name));
        list_del_shift(dotted, list_find(dotted, v, var_same_name));
    }
    list_destroy(&signals);
    return 1;
}

void tt2_merge_pin_inputs(FitEntity *entity, FitList *dotted)
{
    int i;

    for (i = 0; i < tt_count(dotted); i++)
        if (tt2_merge_pin_input(dotted->items[i], dotted, entity))
            i--;
    ensure_netlist(entity);
}

/* the first .X1 or .X2 signal of the list */
static FitVariable *tt2_find_xor_sibling(FitList *signals)
{
    int i;

    for (i = 0; i < tt_count(signals); i++) {
        FitVariable *v = signals->items[i];
        char *ext, *base;

        tt2_split_extension(v->name, &ext, &base);
        free(base);
        if (ext_is(ext, ".X1") || ext_is(ext, ".X2"))
            return v;
    }
    return NULL;
}

/* x.X1 and x.X2 become locals x_X1 and x_X2, and x (or x.D when
 * the node has further signals) the output x_X1 $ x_X2 */
static FitVariable *tt2_make_xor(FitVariable *a, FitVariable *b, const char *base,
                                 FitEntity *entity, int with_others)
{
    FitVariable *result;
    FitFct *fct;
    int pos;

    tt2_dots_to_underscores(a->name);
    tt2_dots_to_underscores(b->name);
    if (entity->locals == NULL)
        entity->locals = list_create();
    if (list_find(entity->inputs, a, var_same_name) == 0) {
        lib_move_variable(a->id, entity->outputs, entity->locals);
        lib_move_variable(b->id, entity->outputs, entity->locals);
        entity->n_out -= 2;
    } else {
        lib_move_variable(a->id, entity->inputs, entity->locals);
        lib_move_variable(b->id, entity->inputs, entity->locals);
        entity->n_in -= 2;
    }
    entity->n_local += 2;

    result = tt2_new_named_var(base);
    if (with_others)
        util_str_cat_n(result->name, ".D", FIT_NAME_MAX);   /* sprintf "%s.D" onto itself */
    pos = list_find(entity->inputs, result, var_same_name);
    if (pos == 0) {
        entity->outputs = tt2_list_add_counted(entity->outputs, result, &entity->n_out);
    } else {
        var_free(&result);
        result = entity->inputs->items[pos - 1];
        lib_move_variable(result->id, entity->inputs, entity->outputs);
    }
    fct = fct_new();
    fct->var_id = result->id;
    fct->tree[0] = fcttree_new_binary_ids(FIT_SEL_XOR, a->id, b->id);
    list_add(entity->fcts, fct);
    return result;
}

static void xor_warning(FitVariable *v, const char *base)
{
    fprintf(g_msg, "Warning - XOR %s with bad number of entry not created\n", base);
    tt2_dots_to_underscores(v->name);
}

/* pairs the .X1 / .X2 signal v with its partner into an XOR; removes both
 * from `dotted` */
static void tt2_build_xor(FitVariable *v, FitList *dotted, FitEntity *entity)
{
    char *ext, *base, *other_ext, *other_base;
    const char *wanted;
    FitList *signals;
    FitVariable *other;
    int with_others = 0;

    tt2_split_extension(v->name, &ext, &base);
    if (ext_is(ext, ".X1")) {
        wanted = ".X2";
    } else if (ext_is(ext, ".X2")) {
        wanted = ".X1";
    } else {
        list_del_shift(dotted, list_find(dotted, v, var_same_name));
        return;
    }
    signals = tt2_collect_siblings(dotted, base, ext);
    other = tt2_find_xor_sibling(signals);
    if (other == NULL) {
        xor_warning(v, base);
        list_del_shift(dotted, list_find(dotted, v, var_same_name));
        return;
    }
    if (signals != NULL && signals->count > 1)
        with_others = 1;
    list_destroy(&signals);
    tt2_split_extension(other->name, &other_ext, &other_base);
    if (ext_is(other_ext, wanted)) {
        FitVariable *result = tt2_make_xor(v, other, other_base, entity, with_others);

        if (tt2_name_has_dot(result->name))
            list_add(dotted, result);
    } else {
        xor_warning(v, other_base);
    }
    list_del_shift(dotted, list_find(dotted, v, var_same_name));
    list_del_shift(dotted, list_find(dotted, other, var_same_name));
}

void tt2_build_xors(FitEntity *entity, FitList *dotted)
{
    while (dotted != NULL && dotted->count != 0)
        tt2_build_xor(dotted->items[dotted->count - 1], dotted, entity);
}

/* the inputs, then the outputs, whose names contain a dot; NULL
 * if there are none */
FitList *tt2_collect_dotted(FitEntity *entity)
{
    FitList *dotted = NULL;
    FitList *lists[2];
    int l, i;

    lists[0] = entity->inputs;
    lists[1] = entity->outputs;
    for (l = 0; l < 2; l++) {
        for (i = 0; i < tt_count(lists[l]); i++) {
            FitVariable *v = lists[l]->items[i];

            if (!tt2_name_has_dot(v->name))
                continue;
            if (dotted == NULL)
                dotted = list_create();
            list_add(dotted, v);
        }
    }
    return dotted;
}

/* a signal declared active low ("name-").  An input keeps its
 * pin but every use reads a new variable that is its complement; an
 * output's function is inverted. */
static void tt2_apply_active_low(FitEntity *entity, char *name)
{
    FitVariable *in, *out;

    tt2_dots_to_underscores(name);
    in = var_list_find_name(entity->inputs, name);
    if (in != NULL) {
        FitVariable *inv = var_new_unique(entity);

        inv->id = in->id;
        in->id = g_tt2_next_id++;
        tt2_add_buffer_fct(entity, inv, in);
        tt2_invert_fct(entity, inv->id);
        if (!tt2_net_uses_signal(entity, in->name)) {
            entity->locals = tt2_list_add_counted(entity->locals, inv, &entity->n_local);
            return;
        }
        entity->outputs = tt2_list_add_counted(entity->outputs, inv, &entity->n_out);
        tt2_rename_net_signal(entity, name, inv->name, 0);
        if (!net_has_alias(entity, inv->name))
            lib_alias_add(entity->net, inv->name, NULL);
    }
    out = var_list_find_name(entity->outputs, name);
    if (out != NULL)
        tt2_invert_fct(entity, out->id);
}

/* apply the declarations collected while reading (names with
 * "+"/"-", "#$ PINS", "#$ NODES").  A name that is both input and output
 * and not connected to an instance is one signal: the output takes over
 * the input's id (a node additionally becomes a local).  Returns 0 after
 * an error. */
int tt2_apply_node_decls(FitEntity *entity)
{
    int ok = 1;
    int n, i;

    if (g_tt2_node_decls == NULL)
        return 1;
    n = g_tt2_node_decls->count;
    for (i = 0; i < n; i++) {
        TtNodeDecl *decl = g_tt2_node_decls->items[i];
        FitVariable *in = var_list_find_name(entity->inputs, decl->name);
        FitVariable *out = in != NULL ? var_list_find_name(entity->outputs, decl->name) : NULL;

        if (in != NULL && out != NULL) {
            FitFct *fct;

            if (tt2_net_uses_signal(entity, decl->name)) {
                fprintf(g_msg, "Error - Variable %s can not be in .ilb and .olb\n", decl->name);
                ok = 0;
                continue;
            }
            list_del_shift(entity->inputs, list_find(entity->inputs, in, var_same_name));
            fct = entity_find_fct(entity, out->id);
            if (fct == NULL)
                tt_unreachable("tt2_apply_node_decls: output without function");
            fct->var_id = in->id;
            out->id = in->id;
            var_free(&in);
            if (decl->declared == 0) {
                list_del_shift(entity->outputs,
                               list_find(entity->outputs, out, var_same_name));
                list_add(entity->locals, out);
                continue;
            }
        }
        if (decl->active == 2)
            tt2_apply_active_low(entity, decl->name);
        if (decl->declared == 0 && ok) {
            if (entity->net != NULL && !net_has_alias(entity, decl->name))
                lib_alias_add(entity->net, decl->name, NULL);
            if (var_list_find_name(entity->inputs, decl->name) != NULL) {
                fprintf(g_msg, "Error : Node %s Has no equation\n", decl->name);
                return 0;
            }
            lib_output_to_local(entity, decl->name);
        }
    }
    if (g_tt2_node_decls != NULL)
        list_destroy(&g_tt2_node_decls);
    return ok;
}
