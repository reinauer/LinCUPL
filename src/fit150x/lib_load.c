/*
 * lib_load.c - library stage of the ATF150x fitter: the stage itself
 * (lib_load_library), what happens to netlist items the library cannot
 * take, the comparison helpers the mapper uses on cells and gates, the
 * alias list of a netlist, and the handling of outputs that are read
 * inside the design.
 *
 * PALASM input is an unsupported option; lib_palasm_stage is only a stub.
 */
#include "lib_priv.h"

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static int g_model_rename_counter;
static int g_default_name_counter;

void lib_load_reset(void)
{
    g_model_rename_counter = 0;
    g_default_name_counter = 0;
}

void lib_reset(void)
{
    techmap_reset();
    lib_load_reset();
    lib_parse_reset();
}

/* FitListEq around util_str_ieq */
int lib_str_ieq(void *a, void *b)
{
    return util_str_ieq(a, b);
}

static char *lib_default_gate_name(void);
static FitUnknownGate *lib_cell13_to_unknown(FitNetItem *it);
static FitUnknownGate *lib_instance_to_unknown(FitNetItem *it);
static void lib_split_output(FitEntity *ent, FitFct *f);

/* ------------------------------------------------------------------ */
/* Unmapped items                                                      */
/* ------------------------------------------------------------------ */

/* pins of a library buffer as FitUnknownPin (name twice) */
static FitList *lib_buf_pin_list(FitLibBuf *b)
{
    FitList *l = list_create();

    lib_unknown_gate_add_pin(l, b->in, b->in, 1);
    lib_unknown_gate_add_pin(l, b->out, b->out, 0);
    lib_unknown_gate_add_pin(l, b->oe, b->oe, 1);
    if (b->type == FIT_BUF_I || b->type == FIT_BUF_CLK)
        lib_unknown_gate_add_pin(l, b->pad, b->pad, 1);
    else
        lib_unknown_gate_add_pin(l, b->pad, b->pad, 0);
    return l;
}

static FitList *lib_seq_pin_list(FitLibSeq *s)
{
    FitList *l = list_create();

    lib_unknown_gate_add_pin(l, s->clk, s->clk, 1);
    lib_unknown_gate_add_pin(l, s->ar, s->ar, 1);
    lib_unknown_gate_add_pin(l, s->ap, s->ap, 1);
    lib_unknown_gate_add_pin(l, s->ce, s->ce, 1);
    lib_unknown_gate_add_pin(l, s->oe, s->oe, 1);
    lib_unknown_gate_add_pin(l, s->d, s->d, 1);
    lib_unknown_gate_add_pin(l, s->mux_a, s->mux_a, 1);
    lib_unknown_gate_add_pin(l, s->mux_b, s->mux_b, 1);
    lib_unknown_gate_add_pin(l, s->mux_sel, s->mux_sel, 1);
    lib_unknown_gate_add_pin(l, s->q, s->q, 0);
    lib_unknown_gate_add_pin(l, s->qn, s->qn, 0);
    lib_unknown_gate_add_pin(l, s->z, s->z, 0);
    lib_unknown_gate_add_pin(l, s->j, s->j, 1);
    lib_unknown_gate_add_pin(l, s->k, s->k, 1);
    return l;
}

static FitList *lib_ugate_pin_list(FitLibUGate *u)
{
    FitList *l = list_create();

    if (u->inputs != NULL)
        for (int i = 0; i < LCOUNT(u->inputs); i++)
            lib_unknown_gate_add_pin(l, u->inputs->items[i], u->inputs->items[i], 1);
    if (u->outputs != NULL)
        for (int i = 0; i < LCOUNT(u->outputs); i++)
            lib_unknown_gate_add_pin(l, u->outputs->items[i], u->outputs->items[i], 0);
    return l;
}

static FitList *lib_comb_pin_list(FitLibComb *c)
{
    FitList *l = list_create();

    lib_unknown_gate_add_pin(l, g_empty_str, c->out_name, 0);
    if (c->pins == NULL)
        return l;
    for (int i = 0; i < LCOUNT(c->pins); i++) {
        FitLibPin *p = c->pins->items[i];

        lib_unknown_gate_add_pin(l, p->name, p->name, 1);
    }
    return l;
}

/* FitListEq: two FitUnknownPin with the same pin name */
static int lib_upin_same_name(void *a, void *b)
{
    return util_str_ieq(UPIN_NAME((FitUnknownPin *)a), UPIN_NAME((FitUnknownPin *)b));
}

/* FitListEq: same pin name and direction */
static int lib_upin_equal(void *a, void *b)
{
    FitUnknownPin *pa = a, *pb = b;

    if (!util_str_ieq(UPIN_NAME(pa), UPIN_NAME(pb)) || UPIN_DIR(pa) != UPIN_DIR(pb))
        return 0;
    return 1;
}

/* does the library gate have exactly the pins of the unknown
 * gate (names without regard to case, same direction)?  On success the
 * pin names of the unknown gate take the spelling of the library. */
static int lib_model_pins_match(FitUnknownGate *ug, FitLibGate *gate)
{
    FitList *pins = ug->pins;
    FitList *gate_pins;
    FitList *common;
    int n, total;

    if (pins == NULL || pins->count == 0)
        return 1;
    if (gate->kind == FIT_GATE_COMB)
        gate_pins = lib_comb_pin_list(gate->cell);
    else if (gate->kind == FIT_GATE_UNKNOWN)
        gate_pins = lib_ugate_pin_list(gate->cell);
    else if (gate->kind == FIT_GATE_BUF)
        gate_pins = lib_buf_pin_list(gate->cell);
    else
        gate_pins = lib_seq_pin_list(gate->cell);
    common = list_intersection(gate_pins, ug->pins, lib_upin_equal);
    if (common == NULL)
        return 0;                       /* gate_pins is not released */
    n = common->count;
    list_destroy(&common);
    total = LCOUNT(ug->pins);
    if (n == total) {
        for (int i = 0; i < pins->count; i++) {
            FitUnknownPin *p = pins->items[i];
            int pos = list_find(gate_pins, p, lib_upin_equal);
            FitUnknownPin *gp;

            if (pos == 0)               /* not in the gate: nothing to rename */
                continue;
            gp = gate_pins->items[pos - 1];
            if (strcmp(UPIN_NAME(p), UPIN_NAME(gp)) != 0)
                strcpy(UPIN_NAME(p), UPIN_NAME(gp));    /* same length: equal but for case */
        }
    }
    list_free_all(&gate_pins, util_free_ptr);   /* the pins' strings are not freed */
    return n == total;
}

/* looks an unknown gate up in the library.  Returns 1 if the
 * library has a gate of that name (or the model is an "_ASYLMSOURCE_"
 * one), else 0. */
static int lib_check_model(FitNetItem *it, FitLibHandle *lib, int check_pins)
{
    LIB_TRACE_F("@model %s\n", LIB_TRACE_S(UG_MODEL((FitUnknownGate *)it->cell)));
    FitList *gates = lib->gates;
    FitUnknownGate *ug = it->cell;
    FitLibGate *g = NULL;
    int found = 0;

    if (UG_MODEL(ug) != NULL && strncasecmp(UG_MODEL(ug), "_ASYLMSOURCE_", 0xd) == 0)
        return 1;
    if (gates == NULL) {
        techmap_message("Warning -  Model %s is not in destination library :\n"
                        "           Instance %s will be a black box in the resulting netlist\n",
                        UG_MODEL(ug), UG_INST(ug), 1);
        return 0;
    }
    if (UG_MODEL(ug) == NULL) {
        UG_MODEL(ug) = lib_default_gate_name();
        return 0;
    }
    for (int i = 0; i < gates->count && !found; i++) {
        g = gates->items[i];
        if (lib_gate_has_name(g, UG_MODEL(ug)))
            found = 1;
    }
    if (check_pins) {
        if (!found) {
            techmap_message("Warning -  Model %s is not in destination library :\n"
                            "           Instance %s will be a black box in the resulting netlist\n",
                            UG_MODEL(ug), UG_INST(ug), 1);
        } else if (!lib_model_pins_match(ug, g)) {
            techmap_message("Warning -  Model %s has not the same characteristics in destination library\n",
                            UG_MODEL(ug), NULL, 1);
            g_model_rename_counter++;
            UG_MODEL(ug) = util_str_with_index(UG_MODEL(ug), g_model_rename_counter);   /* old name not freed */
            techmap_message("Warning -  Model name has been changed to %s\n", UG_MODEL(ug), NULL, 1);
            return 0;
        }
    }
    return found;
}

/* Prints the gates that could not be migrated and the instances that
 * could not be mapped (singular or plural form). */
static void lib_report_unmapped(FitList *gates, FitList *instances)
{
    char *s;

    if (gates != NULL && gates->count != 0) {
        if (gates->count == 1) {
            s = util_strdup(gates->items[0]);
            fprintf(g_msg, "Unable to migrate the gate: %s\n", s);
        } else {
            s = util_join_wrapped(gates, ",", "", 0x50);
            fprintf(g_msg, "Unable to migrate the following gates:\n%s\n", s);
        }
        free(s);
    }
    if (instances != NULL && instances->count != 0) {
        if (instances->count != 1) {
            s = util_join_wrapped(instances, ",", "", 0x50);
            fprintf(g_msg, "Unable to map the following instances:\n%s\n", s);
        } else {
            s = util_strdup(instances->items[0]);
            fprintf(g_msg, "Unable to map the instance: %s\n", s);
        }
        free(s);
    }
}

/* every item the mapper could not place becomes an unknown gate
 * (a black box with named pins) and is looked up in the library by name;
 * the instances are reported.  *out is always `ent`. */
static void lib_migrate_unmapped(FitEntity *ent, FitList *unmapped, FitLibHandle *lib,
                                 int check_pins, FitEntity **out)
{
    LIB_TRACE_F("@migrate\n");
    FitList *instances;
    FitList *gates;

    if (unmapped == NULL) {
        *out = ent;
        return;
    }
    instances = list_create();
    gates = list_create();          /* stays empty */
    for (int i = 0; i < unmapped->count; i++) {
        FitNetItem *it = unmapped->items[i];
        FitUnknownGate *ug;
        char *inst;

        if (it->kind != FIT_GATE_UNKNOWN) {
            if (it->kind == FIT_GATE_BUF) {
                FitCell13 *c = it->cell;

                ug = lib_cell13_to_unknown(it);
                inst = UG_INST(ug);
                if (list_find(instances, inst, lib_str_ieq) == 0)
                    list_add(instances, util_strdup_or_null(inst));
                it->kind = FIT_GATE_UNKNOWN;
                cell13_free(&c);
            } else {
                ug = lib_instance_to_unknown(it);
                inst = UG_INST(ug);
                if (list_find(instances, inst, lib_str_ieq) == 0)
                    list_add(instances, util_strdup_or_null(inst));
                it->kind = FIT_GATE_UNKNOWN;
                util_free_ptr(&it->cell);
            }
            it->cell = ug;
        }
        lib_check_model(it, lib, check_pins);
    }
    *out = ent;
    lib_report_unmapped(gates, instances);
    list_free_all(&gates, util_free_ptr);
    list_free_all(&instances, util_free_ptr);
}

/* ------------------------------------------------------------------ */
/* The library stage                                                   */
/* ------------------------------------------------------------------ */

int lib_load_library(FitList *params)
{
    FitList *unmapped = NULL;
    FitEntity *ent = NULL, *work, *mapped = NULL, *result;
    FitLibHandle *lib = NULL;
    void *obj = NULL;
    char *ent_name, *tech, *file, *sym;
    int found_type = 0, slot = 0, lib_type = 0, lib_slot = 0;

    g_lib_strict_async = 0;
    ent_name = param_get(params, "s");
    if (!entity_table_get(ent_name, FIT_ENT_ENTITY, &found_type, &obj, &slot)) {
        if (ent_name != NULL)
            free(ent_name);
        return 0;
    }
    ent = obj;
    work = entity_simplified_copy(ent);
    tech = param_get(params, "tech");
    if (tech == NULL) {
        fprintf(g_msg, "Error -  Library file name or symbolic name is not defined\n");
        return 0;
    }
    file = util_strdup(tech);
    sym = NULL;
    util_str_append(&sym, "//");
    util_str_append(&sym, file);
    obj = NULL;
    if (!entity_table_read(sym, FIT_ENT_LIBRARY, &lib_type, &obj, &lib_slot, 0)) {
        fprintf(g_msg, "Reading library ...\n");
        if (!lib_open(file, sym, "stdcell", &lib)) {
            free(file);
            free(sym);
            return 0;
        }
    } else {
        lib = obj;
    }
    free(file);
    free(tech);
    free(sym);
    unmapped = list_create();
    techmap_adapt_netlist(work, lib, unmapped);
    lib_migrate_unmapped(work, unmapped, lib, 1, &mapped);
    list_destroy(&unmapped);
    result = entity_simplified_copy(mapped);
    entity_free(&mapped);
    if (ent_name == NULL)
        ent_name = util_strdup(g_empty_str);
    entity_table_store(slot, ent_name, ent_name, FIT_ENT_ENTITY, result);
    free(ent_name);
    if (g_lib_latch_mode != NULL)
        free(g_lib_latch_mode);
    g_lib_latch_mode = NULL;
    return 1;
}

/* PALASM input: unsupported option */
void lib_palasm_stage(FitEntity *ent)
{
    (void)ent;
    fprintf(stderr, "fit150x: ifmt PALASM is not supported\n");
    exit(3);
}

/* ------------------------------------------------------------------ */
/* Variables                                                           */
/* ------------------------------------------------------------------ */

/* FitListEq */
int lib_eq_second_string(void *item, void *key)
{
    return util_str_ieq(((char **)item)[1], key);
}

char *lib_var_name_by_id(int id, FitEntity *ent, int *cls)
{
    FitVariable *v;

    v = var_list_find_id(ent->inputs, id);
    if (v != NULL) {
        *cls = 0;
        return v->name;
    }
    v = var_list_find_id(ent->outputs, id);
    if (v != NULL) {
        *cls = 1;
        return v->name;
    }
    v = var_list_find_id(ent->locals, id);
    if (v == NULL)
        return NULL;
    *cls = 2;
    return v->name;
}

/* 1 if the variable has an entry in the alias list */
int lib_id_has_alias(int id, FitEntity *ent)
{
    char *name;
    int cls = 0;

    if (ent == NULL)
        return 0;
    if (ent->net == NULL)
        return 0;
    name = lib_var_name_by_id(id, ent, &cls);
    return list_find(ent->net->aliases, name, alias_name_eq) != 0;
}

/* FitListEq: the first word of the item equals the key */
int lib_eq_first_word(void *item, void *key)
{
    return *(int *)item == FIT_PTR2INT(key);
}

void lib_move_variable(int id, FitList *from, FitList *to)
{
    LIB_TRACE_F("@move %d\n", id);
    FitVariable *v = var_list_find_id(from, id);

    if (v != NULL) {
        list_add(to, v);
        list_del_shift(from, list_find(from, v, var_same_name));
    }
}

static void lib_tree_replace_id(FitFctTree *t, int old_id, int new_id)
{
    switch (t->sel) {
    case FIT_SEL_NOT:
    case FIT_SEL_AND:
    case FIT_SEL_OR:
    case FIT_SEL_XOR:
        for (int i = 0; i < LCOUNT(t->u.children); i++)
            lib_tree_replace_id(t->u.children->items[i], old_id, new_id);
        break;
    case FIT_SEL_ID:
        if (t->u.id == old_id)
            t->u.id = new_id;
        break;
    default:
        break;
    }
}

void lib_fcts_replace_id(FitList *fcts, int old_id, int new_id)
{
    for (int i = 0; i < LCOUNT(fcts); i++)
        lib_tree_replace_id(((FitFct *)fcts->items[i])->tree[0], old_id, new_id);
}

/* ------------------------------------------------------------------ */
/* Outputs that are read inside the design                             */
/* ------------------------------------------------------------------ */

/* 1 for a node the walk stops at: a constant, a set, an empty
 * node, 'F', or a variable without a function */
static int lib_tree_is_leaf_end(FitFctTree *t, FitEntity *ent)
{
    switch (t->sel) {
    case FIT_SEL_CONST:
    case 'F':
    case FIT_SEL_SET:
    case FIT_SEL_EMPTY:
        return 1;
    case FIT_SEL_ID:
        return entity_find_fct(ent, t->u.id) == NULL;
    default:
        return 0;
    }
}

/* depth-first walk through the functions.  `path` holds the
 * variables being expanded, `done` those already seen; a variable met
 * again while on the path goes to `hits`.
 * After a variable has been expanded, position count-1 (1-based) is
 * removed from the path, that is the entry *before* the one just added,
 * or nothing when the path has one entry.  This is deliberate: it decides
 * which outputs are split, and so the netlist the later stages see. */
static void lib_find_loops(FitFctTree *t, FitList *path, FitList *hits, FitList *done,
                           FitEntity *ent)
{
    int id;

    if (lib_tree_is_leaf_end(t, ent))
        return;
    if (t->sel != FIT_SEL_ID) {
        for (int i = 0; i < LCOUNT(t->u.children); i++)
            lib_find_loops(t->u.children->items[i], path, hits, done, ent);
        return;
    }
    id = t->u.id;
    if (list_find(done, FIT_INT2PTR(id), list_ptr_eq) != 0)
        return;
    if (list_find(path, FIT_INT2PTR(id), list_ptr_eq) == 0) {
        list_add(path, FIT_INT2PTR(id));
        lib_find_loops(entity_find_fct(ent, id)->tree[0], path, hits, done, ent);
        list_del_shift(path, LCOUNT(path) - 1);
        if (list_find(done, FIT_INT2PTR(id), list_ptr_eq) == 0)
            list_add(done, FIT_INT2PTR(id));
    } else {
        list_add(hits, FIT_INT2PTR(id));
        list_add(done, FIT_INT2PTR(id));
    }
}

int lib_split_fed_back_outputs(FitEntity *ent)
{
    FitList *path, *hits, *done;

    if (ent == NULL)
        return 1;
    if (ent->net == NULL)
        ent->net = util_calloc(1, sizeof *ent->net);
    if (ent->net->items == NULL)
        ent->net->items = list_create();
    if (ent->net->aliases == NULL)
        ent->net->aliases = list_create();
    path = list_create();
    hits = list_create();
    done = list_create();
    for (int i = 0; i < LCOUNT(ent->fcts); i++) {
        FitFct *f = ent->fcts->items[i];

        if (list_find(done, FIT_INT2PTR(f->var_id), list_ptr_eq) != 0)
            continue;
        if (!entity_is_output(ent, f->var_id))
            continue;
        list_add(path, FIT_INT2PTR(f->var_id));
        lib_find_loops(f->tree[0], path, hits, done, ent);
        list_del_shift(path, LCOUNT(path) - 1);
        if (list_find(done, FIT_INT2PTR(f->var_id), list_ptr_eq) == 0)
            list_add(done, FIT_INT2PTR(f->var_id));
    }
    for (int i = 0; i < LCOUNT(hits); i++) {
        FitFct *f = entity_find_fct(ent, FIT_PTR2INT(hits->items[i]));

        if (f == NULL)      /* cannot happen: a hit is a variable with a function */
            lib_internal_error("function not found");
        lib_split_output(ent, f);
    }
    list_destroy(&done);
    list_destroy(&hits);
    list_destroy(&path);
    ent->n_in = LCOUNT(ent->inputs);
    ent->n_out = LCOUNT(ent->outputs);
    ent->n_local = LCOUNT(ent->locals);
    return 1;
}

/* the function `f` (of an output or local V that takes part in a
 * loop) is given to a new output V_OO_OO_; a FIT_GATE_WIRE item says that
 * V_OO_OO_ drives V.  If V has an alias entry or is a local it becomes an
 * input; otherwise a new input V_II_II_ takes V's id (so that the functions
 * reading V now read the input) and V becomes a buffer of it. */
static void lib_split_output(FitEntity *ent, FitFct *f)
{
    LIB_TRACE_F("@split %d\n", f->var_id);
    FitVariable *v, *nv, *src;
    FitInstance *wire;
    FitNetItem *item;
    char *name;
    int is_out = entity_is_output(ent, f->var_id);

    if (!is_out)
        v = var_list_find_id(ent->locals, f->var_id);
    else
        v = var_list_find_id(ent->outputs, f->var_id);
    if (v == NULL)
        lib_internal_error("variable not found");
    nv = var_new(0);
    name = v->name;
    var_set_unique_name(ent, nv, name, "_OO_OO_");
    nv->id = tt2_next_var_id(ent);
    list_add(ent->outputs, nv);
    lib_alias_add(ent->net, nv->name, NULL);
    f->var_id = nv->id;
    if (list_find(ent->net->aliases, name, alias_name_eq) != 0 || !is_out) {
        int buf_id;

        lib_move_variable(v->id, is_out ? ent->outputs : ent->locals, ent->inputs);
        lib_alias_add(ent->net, name, NULL);
        buf_id = latch_find_buffer_of(ent, nv->id);
        if (buf_id != 0) {
            FitVariable *bv = entity_var_by_id(ent, buf_id);

            if (bv == NULL)
                lib_internal_error("variable not found");
            lib_alias_add(ent->net, nv->name, bv->name);
        }
        src = v;
    } else {
        FitVariable *iv = var_new(0);

        var_set_unique_name(ent, iv, name, "_II_II_");
        iv->id = v->id;
        list_add(ent->inputs, iv);
        lib_alias_add(ent->net, iv->name, name);
        v->id = tt2_next_var_id(ent);
        tt2_add_buffer_fct(ent, v, iv);
        src = iv;
    }
    f->var_id = nv->id;
    wire = util_calloc(1, sizeof *wire);
    INS_D(wire) = util_strdup(nv->name);
    INS_Q(wire) = util_strdup(src->name);
    INS_NAME(wire) = util_strdup(name);
    item = util_calloc(1, sizeof *item);
    item->kind = FIT_GATE_WIRE;
    item->cell = wire;
    list_add(ent->net->items, item);
}

/* ------------------------------------------------------------------ */
/* Comparison of netlists                                              */
/* ------------------------------------------------------------------ */

static int lib_instance_equal(FitInstance *a, FitInstance *b)
{
    if (!lib_same_presence(INS_CLK(a), INS_CLK(b)))
        return 0;
    if (!lib_same_presence(INS_AR(a), INS_AR(b)))
        return 0;
    if (!lib_same_presence(INS_AP(a), INS_AP(b)))
        return 0;
    if (!lib_same_presence(INS_CE(a), INS_CE(b)))
        return 0;
    if (!lib_same_presence(INS_OE(a), INS_OE(b)))
        return 0;
    if (!lib_same_presence(INS_D(a), INS_D(b)))
        return 0;
    if (!lib_same_presence(INS_J(a), INS_J(b)))
        return 0;
    if (!lib_same_presence(INS_K(a), INS_K(b)))
        return 0;
    if (!lib_same_presence(INS_MUX_A(a), INS_MUX_A(b)))
        return 0;
    if (!lib_same_presence(INS_MUX_B(a), INS_MUX_B(b)))
        return 0;
    if (!lib_same_presence(INS_MUX_SEL(a), INS_MUX_SEL(b)))
        return 0;
    if (!lib_same_presence(INS_Q(a), INS_Q(b)))
        return 0;
    if (!lib_same_presence(INS_QN(a), INS_QN(b)))
        return 0;
    if (!lib_same_presence(INS_Z(a), INS_Z(b)))
        return 0;
    if (INS_CLK_KIND(a) != INS_CLK_KIND(b))
        return 0;
    if (INS_AR_POL(a) != INS_AR_POL(b))
        return 0;
    if (INS_AP_POL(a) != INS_AP_POL(b))
        return 0;
    if (INS_OE_POL(a) != INS_OE_POL(b))
        return 0;
    return INS_CE_POL(a) == INS_CE_POL(b);
}

static int lib_unknown_gate_equal(FitUnknownGate *a, FitUnknownGate *b)
{
    if (!util_str_ieq(UG_MODEL(a), UG_MODEL(b)))
        return 0;
    return list_equal_sets(a->pins, b->pins, lib_upin_same_name) != 0;
}

static int lib_cell13_equal(FitCell13 *a, FitCell13 *b)
{
    if (BUF_TYPE(a) != BUF_TYPE(b))
        return 0;
    if (!lib_same_presence(BUF_IN(a), BUF_IN(b)))
        return 0;
    if (!lib_same_presence(BUF_OUT(a), BUF_OUT(b)))
        return 0;
    if (!lib_same_presence(BUF_PAD(a), BUF_PAD(b)))
        return 0;
    if (!lib_same_presence(BUF_OE(a), BUF_OE(b)))
        return 0;
    return BUF_OE_POL(a) == BUF_OE_POL(b);
}

/* FitListEq over FitNetItem */
static int lib_netitem_equal(void *a, void *b)
{
    FitNetItem *ia = a, *ib = b;

    if (ia->kind != ib->kind)
        return 0;
    if (ia->kind == FIT_GATE_UNKNOWN)
        return lib_unknown_gate_equal(ia->cell, ib->cell);
    if (ia->kind == FIT_GATE_BUF)
        return lib_cell13_equal(ia->cell, ib->cell);
    return lib_instance_equal(ia->cell, ib->cell);
}

int lib_netlist_equal(FitNetlist *a, FitNetlist *b)
{
    if (a == NULL) {
        a = b;
        if (b == NULL)
            return 1;
    } else if (b != NULL) {
        FitList *ia = a->items;

        if (ia == NULL || ia->count == 0) {
            if (b->items == NULL)
                return 1;
            if (b->items->count == 0)
                return 1;
        }
        if (LCOUNT(ia) == LCOUNT(b->items))
            return list_equal_sets(ia, b->items, lib_netitem_equal);
        return 0;
    }
    if (a->items == NULL || a->items->count == 0)
        return 1;
    return 0;
}

int lib_netlist_find_wire(FitNetlist *net)
{
    if (net == NULL)
        return 0;
    return list_find(net->items, FIT_INT2PTR(FIT_GATE_WIRE), lib_eq_first_word);
}

/* ------------------------------------------------------------------ */
/* Aliases                                                             */
/* ------------------------------------------------------------------ */

void lib_alias_set_target(FitNetlist *net, char *name, char *target)
{
    LIB_TRACE_F("@alias_target %s %s\n", LIB_TRACE_S(name), LIB_TRACE_S(target));
    FitList *aliases;
    int pos;

    if (net == NULL)
        return;
    aliases = net->aliases;
    if (aliases == NULL)
        return;
    pos = list_find(aliases, name, alias_name_eq);
    if (pos != 0) {
        FitAlias *a = aliases->items[pos - 1];

        if (target != NULL)
            a->target = util_strdup(target);    /* the old target is not freed */
        else
            a->target = NULL;
    }
}

void lib_alias_remove(FitNetlist *net, char *name)
{
    LIB_TRACE_F("@alias_remove %s\n", LIB_TRACE_S(name));
    int pos;

    if (net == NULL || net->aliases == NULL)
        return;
    pos = list_find(net->aliases, name, alias_name_eq);
    if (pos != 0)
        list_del_shift(net->aliases, pos);
}

void lib_alias_add(FitNetlist *net, char *name, char *target)
{
    LIB_TRACE_F("@alias_add %s %s\n", LIB_TRACE_S(name), LIB_TRACE_S(target));
    FitAlias *a;

    if (net == NULL)
        return;
    a = util_calloc(1, sizeof *a);      /* calloc(1, 8) */
    a->name = name != NULL ? util_strdup(name) : NULL;
    a->target = target != NULL ? util_strdup(target) : NULL;
    if (net->aliases == NULL)
        net->aliases = list_create();
    list_add(net->aliases, a);
}

void lib_output_to_local(FitEntity *ent, char *name)
{
    LIB_TRACE_F("@out2local %s\n", LIB_TRACE_S(name));
    int id;

    if (var_list_find_name(ent->inputs, name) != NULL)
        return;
    id = entity_var_id(ent, name);
    if (ent->net == NULL)
        return;
    if (list_find(ent->net->aliases, name, alias_name_eq) == 0)
        return;
    if (tt2_net_uses_signal(ent, name))
        return;
    lib_move_variable(id, ent->outputs, ent->locals);
    lib_alias_remove(ent->net, name);
    ent->n_local++;
    ent->n_out--;
}

void lib_outputs_to_locals(FitEntity *ent)
{
    int i = 0;

    if (ent->net == NULL)
        lib_internal_error("entity without netlist");
    for (;;) {
        FitList *aliases = ent->net->aliases;
        char *name;

        if (i >= LCOUNT(aliases))
            break;
        name = ((FitAlias *)aliases->items[i])->name;
        if (var_list_find_name(ent->inputs, name) == NULL && !tt2_net_uses_signal(ent, name)) {
            int id = entity_var_id(ent, name);

            lib_move_variable(id, ent->outputs, ent->locals);
            lib_alias_remove(ent->net, name);
            i--;
            ent->n_local++;
            ent->n_out--;
        }
        i++;
    }
}

FitVariable *lib_new_variable(FitEntity *ent, int cls, int add_alias)
{
    LIB_TRACE_F("@newvar %d %d\n", cls, add_alias);
    FitVariable *v = var_new_unique(ent);

    if (cls == 0) {
        list_add(ent->inputs, v);
        ent->n_in++;
    } else if (cls == 1) {
        list_add(ent->outputs, v);
        ent->n_out++;
    } else if (cls == 2) {
        list_add(ent->locals, v);
        ent->n_local++;
    }
    if (add_alias != 0)
        lib_alias_add(ent->net, v->name, NULL);
    return v;
}

/* an input that a cell is about to drive stops being an input:
 * it becomes a local if it has an alias entry and no cell reads it (the
 * alias entry goes), otherwise an output.  Returns the variable, or NULL
 * when `name` is not an input. */
FitVariable *lib_input_to_output(FitEntity *ent, char *name)
{
    LIB_TRACE_F("@in2out %s\n", LIB_TRACE_S(name));
    FitVariable *v = var_list_find_name(ent->inputs, name);

    if (v == NULL)
        return NULL;
    list_del_shift(ent->inputs, list_find(ent->inputs, v, var_same_name));
    ent->n_in--;
    if (ent->net == NULL)
        lib_internal_error("entity without netlist");
    if (list_find(ent->net->aliases, name, alias_name_eq) != 0
        && !tt2_net_uses_signal(ent, name)) {
        list_add(ent->locals, v);
        lib_alias_remove(ent->net, v->name);
        ent->n_local++;
        return v;
    }
    list_add(ent->outputs, v);
    ent->n_out++;
    return v;
}

/* ------------------------------------------------------------------ */
/* Comparison of a cell with a library gate                            */
/* ------------------------------------------------------------------ */

/* a pin the cell does not use is always available */
int lib_pin_available(char *want, char *have)
{
    if (want == NULL)
        return 1;
    return lib_same_presence(want, have) != 0;
}

/* both absent, or both present with the same polarity */
int lib_ctrl_same(char *a, int a_pol, char *b, int b_pol)
{
    if (a == NULL)
        return b == NULL;
    if (b == NULL)
        return 0;
    return a_pol == b_pol;
}

int lib_same_presence(char *a, char *b)
{
    if (a == NULL) {
        if (b == NULL)
            return 1;
    } else if (b != NULL) {
        return 1;
    }
    return 0;
}

/* same data inputs (mux inputs and D) */
int lib_inputs_match(FitInstance *ins, FitLibSeq *gate)
{
    return lib_same_presence(INS_MUX_A(ins), gate->mux_a)
        && lib_same_presence(INS_MUX_B(ins), gate->mux_b)
        && lib_same_presence(INS_MUX_SEL(ins), gate->mux_sel)
        && lib_same_presence(INS_D(ins), gate->d);
}

int lib_outputs_available(FitInstance *ins, FitLibSeq *gate)
{
    return lib_pin_available(INS_Q(ins), gate->q)
        && lib_pin_available(INS_QN(ins), gate->qn)
        && lib_pin_available(INS_Z(ins), gate->z);
}

int lib_controls_same(FitInstance *ins, FitLibSeq *gate)
{
    return lib_ctrl_same(INS_CLK(ins), INS_CLK_KIND(ins), gate->clk, gate->clk_kind)
        && lib_ctrl_same(INS_AR(ins), INS_AR_POL(ins), gate->ar, gate->ar_pol)
        && lib_ctrl_same(INS_AP(ins), INS_AP_POL(ins), gate->ap, gate->ap_pol)
        && lib_ctrl_same(INS_CE(ins), INS_CE_POL(ins), gate->ce, gate->ce_pol)
        && lib_ctrl_same(INS_OE(ins), INS_OE_POL(ins), gate->oe, gate->oe_pol)
        && lib_ctrl_same(INS_J(ins), INS_J_POL(ins), gate->j, gate->j_pol)
        && lib_ctrl_same(INS_K(ins), INS_K_POL(ins), gate->k, gate->k_pol);
}

int lib_controls_available(FitInstance *ins, FitLibSeq *gate)
{
    return lib_ctrl_same(INS_CLK(ins), INS_CLK_KIND(ins), gate->clk, gate->clk_kind)
        && lib_pin_available(INS_AR(ins), gate->ar)
        && lib_pin_available(INS_AP(ins), gate->ap)
        && lib_pin_available(INS_CE(ins), gate->ce)
        && lib_pin_available(INS_OE(ins), gate->oe)
        && lib_pin_available(INS_J(ins), gate->j)
        && lib_pin_available(INS_K(ins), gate->k);
}

/* ------------------------------------------------------------------ */
/* Unknown gates                                                       */
/* ------------------------------------------------------------------ */

void lib_unknown_gate_add_pin(FitList *pins, char *signal, char *pin_name, int dir)
{
    FitUnknownPin *p;

    if (signal == NULL)
        return;
    p = unknown_pin_new();
    UPIN_SIGNAL(p) = util_strdup(signal);
    UPIN_NAME(p) = util_strdup(pin_name);
    UPIN_DIR(p) = dir;
    list_add(pins, p);
}

FitInstance *lib_instance_by_output(FitEntity *ent, char *name)
{
    if (ent == NULL || ent->net == NULL || ent->net->items == NULL)
        return NULL;
    for (int i = 0; i < LCOUNT(ent->net->items); i++) {
        FitNetItem *it = ent->net->items->items[i];
        FitInstance *ins;

        if (it->kind == FIT_GATE_BUF || it->kind == FIT_GATE_COMB || it->kind == FIT_GATE_DMUX
            || it->kind == FIT_GATE_LMUX || it->kind == FIT_GATE_UNKNOWN)
            continue;
        ins = it->cell;
        if (INS_Q(ins) != NULL && util_str_ieq(name, INS_Q(ins)))
            return ins;
        if (INS_QN(ins) != NULL && util_str_ieq(name, INS_QN(ins)))
            return ins;
    }
    return NULL;
}

/* new string "GATE_DEFAULT_NAME_<n>" */
static char *lib_default_gate_name(void)
{
    g_default_name_counter++;
    return util_str_with_index("GATE_DEFAULT_NAME_", g_default_name_counter);
}

/* black box for a pad buffer; pins D, Y, E, PAD */
static FitUnknownGate *lib_cell13_to_unknown(FitNetItem *it)
{
    FitCell13 *c = it->cell;
    FitUnknownGate *ug = unknown_gate_new();

    if (BUF_MODEL(c) == NULL)
        UG_MODEL(ug) = lib_default_gate_name();
    else
        UG_MODEL(ug) = util_strdup(BUF_MODEL(c));
    UG_INST(ug) = util_strdup_or_null(BUF_NAME(c));
    ug->pins = list_create();
    lib_unknown_gate_add_pin(ug->pins, BUF_IN(c), "D", 1);
    lib_unknown_gate_add_pin(ug->pins, BUF_OUT(c), "Y", 0);
    lib_unknown_gate_add_pin(ug->pins, BUF_OE(c), "E", 1);
    if (BUF_TYPE(c) == FIT_BUF_I || BUF_TYPE(c) == FIT_BUF_CLK)
        lib_unknown_gate_add_pin(ug->pins, BUF_PAD(c), "PAD", 1);
    else
        lib_unknown_gate_add_pin(ug->pins, BUF_PAD(c), "PAD", 0);
    return ug;
}

/* black box for a sequential or tri-state cell */
static FitUnknownGate *lib_instance_to_unknown(FitNetItem *it)
{
    FitInstance *ins = it->cell;
    FitUnknownGate *ug = unknown_gate_new();

    if (INS_MODEL(ins) == NULL)
        UG_MODEL(ug) = lib_default_gate_name();
    else
        UG_MODEL(ug) = util_strdup(INS_MODEL(ins));
    UG_INST(ug) = util_strdup_or_null(INS_NAME(ins));
    ug->pins = list_create();
    lib_unknown_gate_add_pin(ug->pins, INS_CLK(ins), "CLK", 1);
    lib_unknown_gate_add_pin(ug->pins, INS_AR(ins), "RST", 1);
    lib_unknown_gate_add_pin(ug->pins, INS_AP(ins), "SET", 1);
    lib_unknown_gate_add_pin(ug->pins, INS_CE(ins), "E", 1);
    lib_unknown_gate_add_pin(ug->pins, INS_OE(ins), "OE", 1);
    lib_unknown_gate_add_pin(ug->pins, INS_D(ins), "D", 1);
    lib_unknown_gate_add_pin(ug->pins, INS_MUX_A(ins), "A", 1);
    lib_unknown_gate_add_pin(ug->pins, INS_MUX_B(ins), "B", 1);
    lib_unknown_gate_add_pin(ug->pins, INS_MUX_SEL(ins), "SEL", 1);
    lib_unknown_gate_add_pin(ug->pins, INS_Q(ins), "Q", 0);
    lib_unknown_gate_add_pin(ug->pins, INS_QN(ins), "QN", 0);
    lib_unknown_gate_add_pin(ug->pins, INS_Z(ins), "Z", 0);
    if (it->kind == FIT_GATE_JK || it->kind == FIT_GATE_RS) {
        lib_unknown_gate_add_pin(ug->pins, INS_J(ins), "J", 1);
        lib_unknown_gate_add_pin(ug->pins, INS_K(ins), "K", 1);
    }
    return ug;
}

/* ------------------------------------------------------------------ */
/* The gates of a library by kind                                      */
/* ------------------------------------------------------------------ */

/* FitListEq */
static int lib_kind_group_eq(void *item, void *key)
{
    return ((FitLibKindGroup *)item)->kind == FIT_PTR2INT(key);
}

FitLibKindGroup *lib_kind_group_find(FitList *groups, int kind)
{
    int pos = list_find(groups, FIT_INT2PTR(kind), lib_kind_group_eq);

    if (pos == 0)
        return NULL;
    return groups->items[pos - 1];
}

static FitLibKindGroup *lib_kind_group_new(int kind)
{
    FitLibKindGroup *g = util_calloc(1, sizeof *g);

    g->kind = kind;
    g->count = 0;
    g->gates = list_create();
    return g;
}

/* the group of a kind, created (empty) if the list has none */
FitLibKindGroup *lib_kind_group_get(FitList *groups, int kind)
{
    FitLibKindGroup *g = lib_kind_group_find(groups, kind);

    if (g == NULL) {
        if (groups == NULL)
            lib_internal_error("library without gates");
        g = lib_kind_group_new(kind);
        list_add(groups, g);
    }
    return g;
}

/* the usable gates (dont_use == 0) grouped by kind, in library
 * order; NULL for a null list */
FitList *lib_group_by_kind(FitList *gates)
{
    FitList *groups;

    if (gates == NULL)
        return NULL;
    groups = list_create();
    for (int i = 0; i < gates->count; i++) {
        FitLibGate *g = gates->items[i];

        if (g->dont_use == 0) {
            FitLibKindGroup *grp = lib_kind_group_get(groups, g->kind);

            list_add(grp->gates, g);
            grp->count = LCOUNT(grp->gates);
        }
    }
    return groups;
}

/* FitListFree.  The loop releases the gates beyond `count`;
 * there never are any, because count always equals the list length. */
void lib_kind_group_free(void **group_ref)
{
    FitLibKindGroup *g = *group_ref;
    int n = LCOUNT(g->gates);

    for (int i = g->count; i < n; i++) {
        lib_gate_free(&g->gates->items[i]);
        g->gates->count--;
    }
    list_destroy(&g->gates);
    free(g);
}
