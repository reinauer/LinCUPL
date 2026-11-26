/*
 * base_entity.c - the table of named entities, the library object kept in
 * it, constructors, copies and destructors of entity, function, netlist and
 * instance, and the lookups in an entity.
 *
 * Helpers needed only by the EQN / EDIF readers are not implemented.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base_int.h"
#include "lib.h"
#include "syn.h"
#include "tt.h"

/* ------------------------------------------------------------------ */
/* entity table                                                        */
/* ------------------------------------------------------------------ */

#define ENTITY_SLOTS 1500   /* slots 0..3 are reserved, 4.. hold named objects */

typedef struct EntitySlot {
    char  name[0x100];
    char  file[0x100];
    int   index;                /* 0..3 in the reserved slots */
    int   type;                 /* enum FitSlotType */
    void *object;               /* FitEntity or FitLibHandle */
} EntitySlot;

static EntitySlot g_entities[ENTITY_SLOTS];
static int g_entity_slots;                      /* slots searched; 0 until entity_table_init */

char *g_lib_prefix;
FitList *g_tree_gc;

/* marks every slot free; the reserved slots get their index */
void entity_table_init(void)
{
    for (int i = 0; i < 4; i++) {
        g_entities[i].name[0] = '\0';
        g_entities[i].file[0] = '\0';
        g_entities[i].type = FIT_ENT_FREE;
        g_entities[i].index = i;
    }
    g_entity_slots = ENTITY_SLOTS;
    for (int i = 4; i < ENTITY_SLOTS; i++) {
        g_entities[i].name[0] = '\0';
        g_entities[i].file[0] = '\0';
        g_entities[i].type = FIT_ENT_FREE;
        g_entities[i].object = NULL;
    }
}

/* slot of `name` (ignoring case) among the named slots; -1 and
 * *type = FIT_ENT_FREE if there is none */
int entity_table_find(const char *name, int *type)
{
    for (int i = 4; i < g_entity_slots; i++) {
        if (util_str_icmp(name, g_entities[i].name) == 0) {
            *type = g_entities[i].type;
            return i;
        }
    }
    *type = FIT_ENT_FREE;
    return -1;
}

/* first free named slot: 1 and *slot, or 0 if the table is full */
int entity_table_new_slot(int *slot)
{
    for (int i = 4; i < g_entity_slots; i++) {
        if (g_entities[i].type == FIT_ENT_FREE) {
            *slot = i;
            return 1;
        }
    }
    return 0;
}

/* releases the gate list of a "stdcell" library handle */
static void lib_handle_release(void **object)
{
    FitLibHandle *h = *object;

    if (h != NULL && h->kind == 1)
        list_free_all(&h->gates, lib_gate_free);
}

/* stores an object in a slot, releasing the entity or library the slot
 * held; name and file are cut to the size of the fields */
int entity_table_store(int slot, const char *name, const char *file, int type, void *object)
{
    EntitySlot *s;

    if (slot < 0 || slot >= ENTITY_SLOTS)
        return 0;
    s = &g_entities[slot];
    if (s->type == FIT_ENT_ENTITY) {
        FitEntity *old = s->object;

        entity_free(&old);
        s->object = old;
    } else if (s->type == FIT_ENT_LIBRARY) {
        lib_handle_release(&s->object);
    }
    strncpy(s->name, name, sizeof(s->name) - 1);
    s->name[sizeof(s->name) - 1] = '\0';
    strncpy(s->file, file, sizeof(s->file) - 1);
    s->file[sizeof(s->file) - 1] = '\0';
    s->type = type;
    s->object = object;
    return 1;
}

/* a new string naming a slot type */
static char *entity_type_name(int type)
{
    if (type == FIT_ENT_ENTITY)
        return util_strdup("FCT");
    if (type == FIT_ENT_LIBRARY)
        return util_strdup("LIB");
    return util_strdup("Unknown Type");
}

/* slot used when no name is given */
static int entity_default_slot(int type)
{
    if (type == FIT_ENT_ENTITY || type != FIT_ENT_LIBRARY)
        return 1;
    return 2;
}

/* looks up the object stored under `name` (NULL: the default slot of the
 * type).  1 with *object if the slot holds an object of `type`; else 0, with
 * a message when report != 0. */
int entity_table_read(const char *name, int type, int *found_type, void **object, int *slot, int report)
{
    if (type != FIT_ENT_ENTITY && type != FIT_ENT_LIBRARY) {
        if (report != 0)
            fprintf(g_msg, "ERROR - EntityReadFromSymbName (bad value for ExternalType)\n");
        return 0;
    }
    if (name == NULL) {
        *slot = entity_default_slot(type);
        *found_type = g_entities[*slot].type;
    } else {
        *slot = entity_table_find(name, found_type);
        if (*slot == -1) {
            if (report != 0)
                util_report_error(FIT_ERR_ENTITY_NOT_FOUND, name);
            return 0;
        }
    }
    if (*found_type != type) {
        if (report != 0) {
            char *tname = entity_type_name(g_entities[*slot].type);

            util_report_error(FIT_ERR_ENTITY_TYPE, tname);
            free(tname);
        }
        return 0;
    }
    *object = g_entities[*slot].object;
    return 1;
}

int entity_table_get(const char *name, int type, int *found_type, void **object, int *slot)
{
    return entity_table_read(name, type, found_type, object, slot, 1);
}

/* ------------------------------------------------------------------ */
/* the library object                                                  */
/* ------------------------------------------------------------------ */

/* name of a library gate: where it is depends on the kind of payload */
static const char *lib_gate_name(FitLibGate *g, int *known)
{
    *known = 1;
    switch (g->kind) {
    case 1:
        return (const char *)g->cell;               /* FitLibComb.name, an array */
    case 0xd:
        return ((FitLibBuf *)g->cell)->name;
    case 0xc:
        *known = 0;
        return NULL;
    default:
        if (g->kind < 1 || g->kind > 0xe) {
            *known = 0;
            return NULL;
        }
        return *(char **)g->cell;                   /* first member of FitLibSeq / FitLibUGate */
    }
}

/* list callback: 1 if the library gate is called `name` (ignoring case) */
int lib_gate_has_name(void *gate, char *name)
{
    int known;
    const char *gname = lib_gate_name(gate, &known);

    if (!known)
        return 0;
    return util_str_ieq(name, gname);
}

static int str_ieq_cb(void *a, void *b)
{
    return util_str_ieq(a, b);
}

/* "Warning : gate %s defined %d times" for every gate name that
 * occurs more than once in the library */
static void lib_warn_duplicate_gates(FitList *gates)
{
    FitList *names = list_create();
    FitList *seen, *counts;

    for (int i = 0; i < (gates == NULL ? 0 : gates->count); i++) {
        FitLibGate *g = gates->items[i];

        if (g->kind == 1)
            list_add(names, g->cell);
        else if (g->kind == 0xd)
            list_add(names, ((FitLibBuf *)g->cell)->name);
        else
            list_add(names, *(char **)g->cell);
    }
    seen = list_create();
    counts = list_create();
    for (int i = 0; i < names->count; i++) {
        char *name = names->items[i];

        if (list_find(seen, name, str_ieq_cb) == 0) {
            int n = 0;

            for (int j = 0; j < names->count; j++) {
                if (util_str_icmp(name, names->items[j]) == 0)
                    n++;
            }
            if (n > 1) {
                list_add(seen, name);
                list_add(counts, FIT_INT2PTR(n));
            }
        }
    }
    for (int i = 0; i < seen->count; i++)
        fprintf(g_msg, "  Warning : gate %s defined %d times\n", (char *)seen->items[i], FIT_PTR2INT(counts->items[i]));
    list_destroy(&names);
    list_destroy(&seen);
    list_destroy(&counts);
}

/* open the library `file` (as given, then with the library
 * prefix), parse it and register it in the entity table under `name` (slot
 * 2 when name is NULL).  fmt NULL means "stdcell", the only format there is
 * a reader for. */
int lib_open(const char *file, const char *name, char *fmt, FitLibHandle **out)
{
    char *path, *fmt_copy = NULL;
    FILE *f;
    FitList *gates = NULL;
    FitLibHeader *header = NULL;
    FitLibHandle *h;
    int slot, type, k;

    if (fmt == NULL) {
        util_str_append(&fmt_copy, "stdcell");
        fmt = fmt_copy;
    }
    path = util_strdup(file);
    f = file_open_read(path);
    if (f == NULL) {
        free(path);
        path = util_strdup(g_lib_prefix);
        util_str_append(&path, file);
        f = file_open_read(path);
        if (f == NULL) {
            if (util_str_ieq(file, "atmel.std") && util_str_ieq(fmt, "stdcell")) {
                gates = lib_builtin_atf150x();
            } else {
                util_report_error(FIT_ERR_FILE_NOT_FOUND, file);
                return 0;
            }
        }
    }
    k = util_str_choice(fmt, 3, "PAL", "MACH", "stdcell");
    if (k == 1 || k == 2) {
        /* there are no readers for these formats */
        return 0;
    }
    if (k != 3) {
        util_report_error(FIT_ERR_PARAM_VALUE, "allowed values for -ofmt are PAL/MACH/stdcell");
        return 0;
    }
    if (f != NULL) {
        util_fclose(f);
        f = NULL;
        if (lib_parse_file(path, &gates, &header) == 0)
            return 0;
    }
    if (name == NULL) {
        slot = 2;
        name = "";
    } else {
        slot = entity_table_find(name, &type);
        if (slot == -1 && entity_table_new_slot(&slot) == 0)
            return 0;
    }
    h = util_calloc(1, sizeof(*h));
    *out = h;
    h->file = util_strdup(path);
    switch (util_str_choice(fmt, 4, "PAL", "MACH", "stdcell", "xilinx")) {
    case 1:
        h->kind = 2;
        h->gates = gates;
        break;
    case 2:
        h->kind = 5;
        h->gates = gates;
        break;
    case 3:
        h->kind = 1;
        h->gates = gates;
        h->info = header;
        break;
    case 4:
        printf("\aNot implemented\n");
        return 0;
    default:
        fprintf(g_msg, "\aSomething went wrong in LibInterfReadLib");
        return 0;
    }
    entity_table_store(slot, name, path, FIT_ENT_LIBRARY, h);
    if (h->kind == 1) {
        if (header != NULL && header->comment != NULL)
            fprintf(g_msg, "%s\n", header->comment);
        lib_warn_duplicate_gates(h->gates);
    }
    if (f != NULL)
        util_fclose(f);
    return 1;
}

/* ------------------------------------------------------------------ */
/* constructors and copies                                             */
/* ------------------------------------------------------------------ */

FitFct *fct_new(void)
{
    FitFct *f = util_calloc(1, sizeof(*f));

    f->tree[0] = NULL;
    f->tree[1] = NULL;
    f->tree[2] = NULL;
    f->reach = NULL;
    f->flags = 0;
    return f;
}

/* deep copy of a function (its three trees); NULL for NULL */
FitFct *fct_copy(FitFct *f)
{
    FitFct *c;

    if (f == NULL)
        return NULL;
    c = fct_new();
    c->var_id = f->var_id;
    c->tree[0] = fcttree_copy(f->tree[0], NULL);
    c->tree[1] = fcttree_copy(f->tree[1], NULL);
    c->tree[2] = fcttree_copy(f->tree[2], NULL);
    return c;
}

static void *fct_copy_cb(void *f)
{
    return fct_copy(f);
}

FitNetlist *netlist_new(void)
{
    return util_calloc(1, sizeof(FitNetlist));
}

/* an 8-byte record, used for netlist items ... */
FitNetItem *netitem_new(void)
{
    return util_calloc(1, sizeof(FitNetItem));
}

/* ... and for aliases */
FitAlias *alias_new(void)
{
    return util_calloc(1, sizeof(FitAlias));
}

FitUnknownGate *unknown_gate_new(void)
{
    return util_calloc(1, sizeof(FitUnknownGate));
}

FitUnknownPin *unknown_pin_new(void)
{
    return util_calloc(1, sizeof(FitUnknownPin));
}

/* new instance; the four polarities start at -1 (none) */
FitInstance *instance_new(void)
{
    FitInstance *i = util_calloc(1, sizeof(*i));

    i->oe_pol = -1;
    i->ar_pol = -1;
    i->ap_pol = -1;
    i->ce_pol = -1;
    return i;
}

/* new empty entity with a copy of name (which may be NULL) */
FitEntity *entity_new(const char *name)
{
    FitEntity *e = util_calloc(1, sizeof(*e));

    e->name = name == NULL ? NULL : util_strdup(name);
    return e;
}

/* deep copy of an instance (strings duplicated) */
static FitInstance *instance_copy(FitInstance *i)
{
    FitInstance *c;

    if (i == NULL)
        return NULL;
    c = util_calloc(1, sizeof(*c));
    c->name = util_strdup_or_null(i->name);
    c->inst_name = util_strdup_or_null(i->inst_name);
    c->clk = util_strdup_or_null(i->clk);
    c->clk_kind = i->clk_kind;
    c->ar = util_strdup_or_null(i->ar);
    c->ar_pol = i->ar_pol;
    c->ap = util_strdup_or_null(i->ap);
    c->ap_pol = i->ap_pol;
    c->ce = util_strdup_or_null(i->ce);
    c->ce_pol = i->ce_pol;
    c->oe = util_strdup_or_null(i->oe);
    c->oe_pol = i->oe_pol;
    c->q = util_strdup_or_null(i->q);
    c->q2 = util_strdup_or_null(i->q2);
    c->q2_pol = i->q2_pol;
    c->q3 = util_strdup_or_null(i->q3);
    c->q3_pol = i->q3_pol;
    c->mux_a = util_strdup_or_null(i->mux_a);
    c->mux_b = util_strdup_or_null(i->mux_b);
    c->mux_sel = util_strdup_or_null(i->mux_sel);
    c->d = util_strdup_or_null(i->d);
    c->d2 = util_strdup_or_null(i->d2);
    c->pin = util_strdup_or_null(i->pin);
    return c;
}

static FitCell13 *cell13_copy(FitCell13 *s)
{
    FitCell13 *c;

    if (s == NULL)
        return NULL;
    c = util_calloc(1, sizeof(*c));
    c->model = util_strdup_or_null(s->model);
    c->inst_name = util_strdup_or_null(s->inst_name);
    c->in = util_strdup_or_null(s->in);
    c->oe = util_strdup_or_null(s->oe);
    c->oe_pol = s->oe_pol;
    c->pad = util_strdup_or_null(s->pad);
    c->out = util_strdup_or_null(s->out);
    c->buf_type = s->buf_type;
    return c;
}

static void *unknown_pin_copy(void *item)
{
    FitUnknownPin *p = item, *c;

    if (p == NULL)
        return NULL;
    c = util_calloc(1, sizeof(*c));
    c->dir = p->dir;
    c->name = util_strdup_or_null(p->name);
    c->signal = util_strdup_or_null(p->signal);
    return c;
}

static FitUnknownGate *unknown_gate_copy(FitUnknownGate *g)
{
    FitUnknownGate *c;

    if (g == NULL)
        return NULL;
    c = util_calloc(1, sizeof(*c));
    c->i00 = g->i00;
    c->model = util_strdup_or_null(g->model);
    c->inst_name = util_strdup_or_null(g->inst_name);
    c->pins = list_map_copy(g->pins, unknown_pin_copy);
    return c;
}

/* deep copy of a netlist item, by the kind of its cell */
static void *netitem_copy(void *item)
{
    FitNetItem *it = item, *c;

    if (it == NULL)
        return NULL;
    c = util_calloc(1, sizeof(*c));
    c->kind = it->kind;
    if (it->kind == FIT_GATE_UNKNOWN)
        c->cell = unknown_gate_copy(it->cell);
    else if (it->kind == FIT_GATE_CELL13)
        c->cell = cell13_copy(it->cell);
    else
        c->cell = instance_copy(it->cell);
    return c;
}

static void *alias_copy(void *item)
{
    FitAlias *a = item, *c;

    if (a == NULL)
        return NULL;
    c = util_calloc(1, sizeof(*c));
    c->name = util_strdup_or_null(a->name);
    c->target = util_strdup_or_null(a->target);
    return c;
}

FitNetlist *netlist_copy(FitNetlist *n)
{
    FitNetlist *c;

    if (n == NULL)
        return NULL;
    c = util_calloc(1, sizeof(*c));
    c->items = list_map_copy(n->items, netitem_copy);
    c->aliases = list_map_copy(n->aliases, alias_copy);
    return c;
}

static void *var_copy_cb(void *v)
{
    return var_copy(v);
}

/* deep copy of an entity: variables, functions and netlist */
FitEntity *entity_copy(FitEntity *e)
{
    FitEntity *c;

    if (e == NULL)
        return NULL;
    c = entity_new(e->name);
    c->i04 = e->i04;
    if (e->lib_file != NULL)
        c->lib_file = util_strdup(e->lib_file);
    c->n_in = e->n_in;
    c->n_out = e->n_out;
    c->n_local = e->n_local;
    c->inputs = list_map_copy(e->inputs, var_copy_cb);
    c->outputs = list_map_copy(e->outputs, var_copy_cb);
    c->locals = list_map_copy(e->locals, var_copy_cb);
    c->fcts = list_map_copy(e->fcts, fct_copy_cb);
    c->net = netlist_copy(e->net);
    c->i2c = e->i2c;
    return c;
}

/* ------------------------------------------------------------------ */
/* destructors                                                         */
/* ------------------------------------------------------------------ */

/* release the tree nodes queued since fcttree_gc_begin */
void fcttree_gc_end(void)
{
    if (g_tree_gc == NULL)
        return;
    for (int i = 0; i < g_tree_gc->count; i++)
        free(g_tree_gc->items[i]);
    list_destroy(&g_tree_gc);
    g_tree_gc = NULL;
}

/* frees a function; its trees are queued on g_tree_gc, so the call needs
 * fcttree_gc_begin / fcttree_gc_end around it */
void fct_free(FitFct **f)
{
    if (*f != NULL) {
        fcttree_free_deferred(&(*f)->tree[0]);
        fcttree_free_deferred(&(*f)->tree[1]);
        fcttree_free_deferred(&(*f)->tree[2]);
        free(*f);
        *f = NULL;
    }
}

static void fct_free_cb(void **f)
{
    FitFct *p = *f;

    fct_free(&p);
    *f = p;
}

/* starts collecting released tree nodes on g_tree_gc */
void fcttree_gc_begin(void)
{
    if (g_tree_gc == NULL)
        g_tree_gc = list_create();
}

static void unknown_gate_free(void **g)
{
    FitUnknownGate *ug = *g;

    if (ug != NULL) {
        /* the pins are released with free(): their strings stay allocated */
        list_free_all(&ug->pins, util_free_ptr);
        free(ug);
        *g = NULL;
    }
}

void cell13_free(FitCell13 **c)
{
    FitCell13 *p = *c;

    if (p != NULL) {
        if (p->model != NULL)
            free(p->model);
        if (p->inst_name != NULL)
            free(p->inst_name);
        if (p->in != NULL)
            free(p->in);
        if (p->out != NULL)
            free(p->out);
        if (p->pad != NULL)
            free(p->pad);
        if (p->oe != NULL)
            free(p->oe);
        free(p);
        *c = NULL;
    }
}

/* frees a netlist item and its cell */
void netitem_free(FitNetItem **it)
{
    FitNetItem *p = *it;

    if (p != NULL) {
        if (p->kind == FIT_GATE_UNKNOWN) {
            unknown_gate_free(&p->cell);
        } else if (p->kind == FIT_GATE_CELL13) {
            FitCell13 *c = p->cell;

            cell13_free(&c);
            p->cell = NULL;
        } else {
            /* a FitInstance is released as one block; its strings stay */
            util_free_ptr(&p->cell);
        }
        free(p);
        *it = NULL;
    }
}

static void netitem_free_cb(void **it)
{
    FitNetItem *p = *it;

    netitem_free(&p);
    *it = p;
}

void alias_free(FitAlias **a)
{
    FitAlias *p = *a;

    if (p != NULL) {
        free(p->name);
        if (p->target != NULL)
            free(p->target);
        free(p);
        *a = NULL;
    }
}

static void alias_free_cb(void **a)
{
    FitAlias *p = *a;

    alias_free(&p);
    *a = p;
}

static void netlist_free(FitNetlist **n)
{
    if (*n != NULL) {
        list_free_all(&(*n)->items, netitem_free_cb);
        list_free_all(&(*n)->aliases, alias_free_cb);
        free(*n);
        *n = NULL;
    }
}

static void var_free_cb(void **v)
{
    FitVariable *p = *v;

    var_free(&p);
    *v = p;
}

/* frees an entity with its variables, functions and netlist */
void entity_free(FitEntity **e)
{
    FitEntity *p = *e;

    if (p != NULL) {
        if (p->name != NULL)
            free(p->name);
        list_free_all(&p->inputs, var_free_cb);
        list_free_all(&p->outputs, var_free_cb);
        list_free_all(&p->locals, var_free_cb);
        fcttree_gc_begin();
        list_free_all(&p->fcts, fct_free_cb);
        fcttree_gc_end();
        netlist_free(&p->net);
        free(p);
        *e = NULL;
    }
}

/* releases the contents of an entity, keeps the struct */
void entity_clear(FitEntity *e)
{
    if (e != NULL) {
        if (e->name != NULL)
            free(e->name);
        list_free_all(&e->inputs, var_free_cb);
        list_free_all(&e->outputs, var_free_cb);
        list_free_all(&e->locals, var_free_cb);
        fcttree_gc_begin();
        list_free_all(&e->fcts, fct_free_cb);
        fcttree_gc_end();
        netlist_free(&e->net);
    }
}

/* copies the fields of src into dst; the lists and strings are shared */
void entity_shallow_assign(FitEntity *src, FitEntity *dst)
{
    if (src != NULL && dst != NULL) {
        dst->name = src->name;
        dst->n_in = src->n_in;
        dst->n_out = src->n_out;
        dst->n_local = src->n_local;
        dst->inputs = src->inputs;
        dst->outputs = src->outputs;
        dst->locals = src->locals;
        dst->fcts = src->fcts;
        dst->i04 = src->i04;
        dst->lib_file = src->lib_file;
        dst->net = src->net;
        dst->i30 = src->i30;
    }
}

/* remove the function f from the entity together with the
 * variable it defines (a local if the id is one, otherwise an output).  The
 * FitFct block is freed, its trees are not. */
void entity_remove_fct(FitEntity *e, FitFct *f)
{
    FitList *vars;
    int pos = 0, found = 0;

    if (entity_is_local(e, f->var_id) == 0) {
        vars = e->outputs;
        e->n_out--;
    } else {
        vars = e->locals;
        e->n_local--;
    }
    /* pos ends as the position of the variable, or as the count when the
     * id is not in the list (the last variable is deleted then) */
    for (int i = 0; i < vars->count && !found; i++) {
        found = ((FitVariable *)vars->items[i])->id == f->var_id;
        pos++;
    }
    list_del_shift_free(vars, pos, var_free_cb);
    pos = 0;
    found = 0;
    for (int i = 0; i < e->fcts->count && !found; i++) {
        found = e->fcts->items[i] == (void *)f;
        pos++;
    }
    list_del_shift_free(e->fcts, pos, util_free_ptr);
}

/* a copy of the tree in which every NOT has
 * been pushed down to the variables and constants (invert != 0 asks for
 * the complement). */
FitFctTree *fcttree_propagate_inv(FitFctTree *t, unsigned invert)
{
    FitFctTree *out;

    for (;;) {
        if (t == NULL)
            return NULL;
        if (t->sel != FIT_SEL_NOT)
            break;
        t = t->u.children->items[0];
        invert = invert == 0;
    }
    out = fcttree_new();
    out->u.children = list_create_sized(1);
    switch (t->sel) {
    default:
        util_report_error(FIT_ERR_INTERNAL, "in FctTreePropagateInv");
        return NULL;
    case FIT_SEL_AND:
    case FIT_SEL_OR:
        if (invert == 0)
            out->sel = t->sel;
        else
            out->sel = t->sel == FIT_SEL_AND ? FIT_SEL_OR : FIT_SEL_AND;
        for (int i = 0; i < (t->u.children == NULL ? 0 : t->u.children->count); i++)
            list_add(out->u.children, fcttree_propagate_inv(t->u.children->items[i], invert != 0));
        break;
    case FIT_SEL_CONST:
        if (t->u.value != 0 && t->u.value != 1) {
            util_report_error(FIT_ERR_INTERNAL, "in FctTreeToLPTerm (remove complex constant from specification)");
            return NULL;
        }
        out->sel = FIT_SEL_CONST;
        list_destroy(&out->u.children);
        out->u.value = invert == 0 ? t->u.value : t->u.value == 0;
        return out;
    case FIT_SEL_ID:
        if (invert != 0) {
            FitFctTree *leaf;

            out->sel = FIT_SEL_NOT;
            leaf = fcttree_new();
            leaf->sel = FIT_SEL_ID;
            leaf->u.id = t->u.id;
            list_add(out->u.children, leaf);
            return out;
        }
        out->sel = FIT_SEL_ID;
        list_destroy(&out->u.children);
        out->u.id = t->u.id;
        break;
    case FIT_SEL_XOR:
        if (invert == 0) {
            out->sel = FIT_SEL_XOR;
            for (int i = 0; i < (t->u.children == NULL ? 0 : t->u.children->count); i++)
                list_add(out->u.children, fcttree_propagate_inv(t->u.children->items[i], 0));
        } else {
            FitFctTree *x;

            out->sel = FIT_SEL_NOT;
            x = fcttree_new();
            x->sel = FIT_SEL_XOR;
            x->u.children = list_create_sized(t->u.children == NULL ? 0 : t->u.children->count);
            list_add(out->u.children, x);
            for (int i = 0; i < (t->u.children == NULL ? 0 : t->u.children->count); i++)
                list_add(x->u.children, fcttree_propagate_inv(t->u.children->items[i], 0));
        }
        break;
    }
    return out;
}

/* copy of a function with the NOTs of its trees pushed down;
 * NULL (and nothing kept) if one of the trees cannot be converted. */
static void *fct_copy_inv_propagated(void *item)
{
    FitFct *f = item;
    FitFct *c = fct_new();

    c->var_id = f->var_id;
    for (int i = 0; i < 3; i++) {
        if (f->tree[i] != NULL) {
            c->tree[i] = fcttree_propagate_inv(f->tree[i], 0);
            if (c->tree[i] == NULL) {
                fct_free(&c);
                return NULL;
            }
        }
    }
    return c;
}

/* entity_copy with fct_copy_inv_propagated for the functions
 * (i2c is not copied) */
FitEntity *entity_copy_inv_propagated(FitEntity *e)
{
    FitEntity *c;

    if (e == NULL)
        return NULL;
    c = entity_new(e->name);
    c->i04 = e->i04;
    if (e->lib_file != NULL)
        c->lib_file = util_strdup(e->lib_file);
    c->n_in = e->n_in;
    c->n_out = e->n_out;
    c->n_local = e->n_local;
    c->inputs = list_map_copy(e->inputs, var_copy_cb);
    c->outputs = list_map_copy(e->outputs, var_copy_cb);
    c->locals = list_map_copy(e->locals, var_copy_cb);
    c->fcts = list_map_copy(e->fcts, fct_copy_inv_propagated);
    c->net = netlist_copy(e->net);
    return c;
}

/* the product of the literals of p over the
 * variables of `vars` (bit n-1 belongs to vars item n); a single literal is
 * returned without the AND node, a term without literals as the constant 1. */
FitFctTree *fcttree_from_pterm(FitPTerm *p, FitList *vars)
{
    unsigned n = vars == NULL ? 0 : (unsigned)vars->count;
    FitFctTree *out = fcttree_new();
    int nwords = n == 0 ? 1 : (int)((n - 1) >> 5) + 1;

    if (pterm_is_tautology(p, nwords) != 0) {
        out->sel = FIT_SEL_CONST;
        out->u.value = 1;
        return out;
    }
    out->u.children = list_create();
    out->sel = FIT_SEL_AND;
    for (unsigned i = 0; i < n; i++) {
        unsigned word = i >> 5;
        unsigned mask = 1u << (i & 0x1f);
        int id = ((FitVariable *)vars->items[i])->id;

        if ((p->bits[word] & mask) == 0) {
            if ((p->care[word] & mask) == 0) {
                FitFctTree *neg = fcttree_new();
                FitFctTree *leaf = fcttree_new();

                leaf->sel = FIT_SEL_ID;
                leaf->u.id = id;
                neg->sel = FIT_SEL_NOT;
                neg->u.children = list_create_sized(1);
                list_add(neg->u.children, leaf);
                list_add(out->u.children, neg);
            }
        } else if ((p->care[word] & mask) == 0) {
            printf("Function FctTreeFromPTerm : Error in PTerm Value\n");
        } else {
            FitFctTree *leaf = fcttree_new();

            leaf->sel = FIT_SEL_ID;
            leaf->u.id = id;
            list_add(out->u.children, leaf);
        }
    }
    if (out->u.children != NULL && out->u.children->count == 1) {
        FitFctTree *only = out->u.children->items[0];

        list_destroy(&out->u.children);
        free(out);
        return only;
    }
    return out;
}

/* the sum of the products of a list of FitPTerm; the constant 0
 * for an empty list. */
FitFctTree *fcttree_from_pterms(FitList *pterms, FitList *vars)
{
    FitFctTree *out;

    if (pterms == NULL || pterms->count == 0) {
        out = fcttree_new();
        out->u.value = 0;
        out->sel = FIT_SEL_CONST;
        return out;
    }
    if (pterms->count == 1)
        return fcttree_from_pterm(pterms->items[0], vars);
    out = fcttree_new();
    out->sel = FIT_SEL_OR;
    out->u.children = list_create_sized(pterms->count);
    for (int i = 0; i < pterms->count; i++)
        list_add(out->u.children, fcttree_from_pterm(pterms->items[i], vars));
    return out;
}

/* a new string with the name of variable id; a generated name
 * when the entity is NULL, the id negative or the variable unknown. */
char *entity_var_name(FitEntity *e, int id)
{
    FitList *vars;
    char *name;
    int kind;

    if (e == NULL || id < 0)
        return util_gen_name(2, id);
    name = util_calloc(1, 0x100);
    if (entity_is_input(e, id) != 0) {
        kind = 0;
    } else if (entity_is_output(e, id) != 0) {
        kind = 1;
    } else if (entity_is_local(e, id) != 0) {
        kind = 2;
    } else {
        free(name);
        return util_gen_name(2, id);
    }
    vars = kind == 0 ? e->inputs : kind == 1 ? e->outputs : e->locals;
    for (int i = 0; i < (vars == NULL ? 0 : vars->count); i++) {
        FitVariable *v = vars->items[i];

        if (v->id == id) {
            strcpy(name, v->name);
            return name;
        }
    }
    free(name);
    /* not reached in practice: the membership test above succeeded.  An
     * input is named XXI_, an output XXO_; a missing local list gives XXO_
     * too, a local not found XXL_. */
    if (kind == 0)
        return util_gen_name(0, id);
    if (kind == 1)
        return util_gen_name(1, id);
    return util_gen_name(vars == NULL ? 1 : 2, id);
}

int entity_is_output(FitEntity *e, int id)
{
    int found = 0;

    if (e->outputs == NULL)
        return 0;
    for (int i = 0; i < e->outputs->count; i++) {
        if (((FitVariable *)e->outputs->items[i])->id == id)
            found = 1;
    }
    return found;
}

int entity_is_input(FitEntity *e, int id)
{
    int found = 0;

    if (e->inputs == NULL)
        return 0;
    for (int i = 0; i < e->inputs->count; i++) {
        if (((FitVariable *)e->inputs->items[i])->id == id)
            found = 1;
    }
    return found;
}

int entity_is_local(FitEntity *e, int id)
{
    if (e->locals != NULL) {
        for (int i = 0; i < e->locals->count; i++) {
            if (((FitVariable *)e->locals->items[i])->id == id)
                return 1;
        }
    }
    return 0;
}

/* tree[0] of the function defining id, or NULL */
FitFctTree *entity_fct_tree(FitEntity *e, int id)
{
    FitFct *f = entity_find_fct(e, id);

    return f == NULL ? NULL : f->tree[0];
}

/* the function defining variable id, or NULL */
FitFct *entity_find_fct(FitEntity *e, int id)
{
    if (e->fcts == NULL)
        return NULL;
    for (int i = 0; i < e->fcts->count; i++) {
        FitFct *f = e->fcts->items[i];

        if (f->var_id == id)
            return f;
    }
    return NULL;
}

/* n_in / n_out / n_local from the sizes of the lists */
void entity_update_counts(FitEntity *e)
{
    if (e != NULL) {
        e->n_in = e->inputs == NULL ? 0 : e->inputs->count;
        e->n_local = e->locals == NULL ? 0 : e->locals->count;
        e->n_out = e->outputs == NULL ? 0 : e->outputs->count;
    }
}

/* ------------------------------------------------------------------ */
/* lookups                                                             */
/* ------------------------------------------------------------------ */

int entity_has_input_id(FitEntity *e, int id)
{
    for (int i = 0; i < (e->inputs == NULL ? 0 : e->inputs->count); i++) {
        if (((FitVariable *)e->inputs->items[i])->id == id)
            return 1;
    }
    return 0;
}

/* list callback: FitVariable.id against FIT_INT2PTR(id) */
int var_id_eq(void *var, void *id)
{
    return ((FitVariable *)var)->id == FIT_PTR2INT(id);
}

/* list callback: FitFct.var_id (the first int of the item) against an id */
int fct_id_eq(void *fct, void *id)
{
    return *(int *)fct == FIT_PTR2INT(id);
}

/* the netlist item whose cell drives
 * `out_name` on one of its three outputs */
static FitNetItem *netlist_search(FitEntity *e, const char *out_name)
{
    if (e == NULL || e->net == NULL || e->net->items == NULL)
        return NULL;
    for (int i = 0; i < (e->net->items == NULL ? 0 : e->net->items->count); i++) {
        FitNetItem *it = e->net->items->items[i];

        if (it->kind == FIT_GATE_UNKNOWN) {
            fprintf(g_msg, "UNKNOWN_GATE in NetList\n");
        } else if (it->kind == FIT_GATE_DMUX) {
            fprintf(g_msg, "DMUX_GATE not supported\n");
        } else if (it->kind == FIT_GATE_LMUX) {
            fprintf(g_msg, "LMUX_GATE not supported\n");
        } else if (it->kind != FIT_GATE_CELL13) {
            FitInstance *inst = it->cell;

            if (util_str_ieq(out_name, inst->q) != 0 || util_str_ieq(out_name, inst->q2) != 0 ||
                util_str_ieq(out_name, inst->q3) != 0)
                return it;
        }
    }
    return NULL;
}

FitInstance *netlist_find_instance(FitEntity *e, const char *out_name)
{
    FitNetItem *it = netlist_search(e, out_name);

    return it == NULL ? NULL : it->cell;
}

FitNetItem *netlist_find_item(FitEntity *e, const char *out_name)
{
    return netlist_search(e, out_name);
}

/* id of the variable called `name` (ignoring case), searched in the locals,
 * outputs and inputs; 0 if none */
int entity_var_id_by_name(FitEntity *e, const char *name)
{
    FitList *lists[3];

    if (name == NULL)
        return 0;
    lists[0] = e->locals;
    lists[1] = e->outputs;
    lists[2] = e->inputs;
    for (int k = 0; k < 3; k++) {
        int count = lists[k] == NULL ? 0 : lists[k]->count;

        for (int i = 0; i < count; i++) {
            FitVariable *v = lists[k]->items[i];

            if (util_str_ieq(v->name, name) != 0)
                return v->id;
        }
    }
    return 0;
}

/* the variable with this id among the locals, outputs and inputs */
FitVariable *entity_var_by_id(FitEntity *e, int id)
{
    FitList *lists[3];

    lists[0] = e->locals;
    lists[1] = e->outputs;
    lists[2] = e->inputs;
    for (int k = 0; k < 3; k++) {
        int count = lists[k] == NULL ? 0 : lists[k]->count;

        for (int i = 0; i < count; i++) {
            FitVariable *v = lists[k]->items[i];

            if (v->id == id)
                return v;
        }
    }
    return NULL;
}

/* 1 if name is not an alias itself but the target of one */
int netlist_has_name(FitEntity *e, const char *name)
{
    FitNetlist *net = e->net;

    if (net != NULL && net->items != NULL) {
        if (latch_find_alias((char *)(uintptr_t)name, net) == 0 && net->aliases != NULL) {
            for (int i = 0; i < net->aliases->count; i++) {
                FitAlias *a = net->aliases->items[i];

                if (util_str_ieq(name, a->target) != 0)
                    return 1;
            }
        }
    }
    return 0;
}

/* id of the variable the alias `name` stands for, 0 if there is
 * no such alias or it has no target */
int netlist_alias_target_id(FitEntity *e, const char *name)
{
    FitNetlist *net = e->net;

    if (net != NULL && net->items != NULL && net->aliases != NULL) {
        for (int i = 0; i < net->aliases->count; i++) {
            FitAlias *a = net->aliases->items[i];

            if (util_str_ieq(name, a->name) != 0) {
                if (a->target == NULL)
                    return 0;
                return entity_var_id_by_name(e, a->target);
            }
        }
    }
    return 0;
}

/* list callback: the first char * of the item against a name (ignoring
 * case) */
int alias_name_eq(void *alias, void *name)
{
    return util_str_ieq(*(char **)alias, name);
}

/* number of functions whose tree[0] uses variable id */
int entity_count_fct_using(FitEntity *e, int id)
{
    int n = 0;

    for (int i = 0; i < (e->fcts == NULL ? 0 : e->fcts->count); i++) {
        FitFct *f = e->fcts->items[i];

        if (fcttree_count_id(f->tree[0], id) != 0)
            n++;
    }
    return n;
}

/* 1 for a local variable, and for a variable whose alias (or the
 * alias of the signal feeding its register) leads to an untargeted alias. */
int entity_var_is_node(FitEntity *e, FitVariable *v)
{
    FitList *aliases;
    FitVariable *w;
    int pos;

    if (entity_is_local(e, v->id) != 0)
        return 1;
    if (e->net == NULL || e->net->aliases == NULL)
        return 0;
    aliases = e->net->aliases;
    pos = list_find(aliases, v->name, alias_name_eq);
    if (pos == 0)
        return 0;
    if (((FitAlias *)aliases->items[pos - 1])->target == NULL) {
        FitInstance *inst = netlist_find_instance(e, v->name);
        const char *src;
        int id;

        if (inst == NULL)
            return 1;
        src = inst->pin;
        if (src == NULL) {
            src = inst->d;
            if (src == NULL)
                src = inst->d2;
        }
        w = entity_var_by_id(e, entity_var_id_by_name(e, src));
        if (w == NULL)
            return 0;
        id = netlist_alias_target_id(e, w->name);
        if (id != 0) {
            w = entity_var_by_id(e, id);
            if (w == NULL)
                return 0;
        }
        pos = list_find(e->net->aliases, w->name, alias_name_eq);
        if (pos != 0 && ((FitAlias *)e->net->aliases->items[pos - 1])->target == NULL)
            return 1;
    } else {
        w = entity_var_by_id(e, entity_var_id_by_name(e, ((FitAlias *)aliases->items[pos - 1])->target));
        if (w == NULL)
            return 0;
        if (list_find(e->net->aliases, w->name, alias_name_eq) != 0)
            return 1;
    }
    return 0;
}

/* Take the output enable of the tri-state buffer th into the register reg:
 * the common part of the two branches of netlist_absorb_tristates.  Returns
 * 0 if the alias of the register's data input is missing. */
static int absorb_into(FitEntity *e, FitInstance *reg, FitInstance *th, const char *pad)
{
    const char *key = reg->d != NULL ? reg->d : reg->d2;
    FitAlias *alias;
    int pos;

    reg->oe = util_strdup_or_null(th->oe);
    reg->oe_pol = th->oe_pol;
    reg->pin = util_strdup_or_null(pad);
    pos = list_find(e->net->aliases, (void *)(uintptr_t)key, alias_name_eq);
    if (pos == 0)
        return 0;
    alias = e->net->aliases->items[pos - 1];
    flatten_free_ref((void **)&alias->target);
    return 1;
}

/* a tri-state buffer (netlist item of kind 8) that follows a
 * register is folded into the register: the register gets the buffer's
 * enable and pad, the buffer leaves the netlist. */
void netlist_absorb_tristates(FitEntity *e)
{
    if (e == NULL || e->net == NULL || e->net->items == NULL)
        return;
    for (int i = 0; i < (e->net->items == NULL ? 0 : e->net->items->count); i++) {
        FitNetItem *item = e->net->items->items[i];
        FitInstance *th, *reg;
        int pos;

        if (item->kind != FIT_GATE_TH)
            continue;
        th = item->cell;
        pos = list_find(e->outputs, th->q, var_has_name);
        if (pos == 0) {
            const char *pad;

            if (latch_find_alias(th->q, e->net) == 0)
                continue;
            reg = lib_instance_by_output(e, th->q);
            if (reg == NULL || reg->oe != NULL || reg->clk == NULL)
                continue;
            pad = reg->d == NULL ? th->pin : th->d;
            if (reg->ap != NULL && list_find(e->inputs, th->oe, var_has_name) == 0)
                continue;
            if (absorb_into(e, reg, th, pad) == 0)
                continue;
            list_del_shift(e->net->items, i + 1);
            i--;
            free(th);
            free(item);
        } else {
            FitVariable *outv = e->outputs->items[pos - 1];
            FitFct *f = entity_find_fct(e, outv->id);
            FitVariable *src;
            FitAlias *alias;
            const char *pad;

            if (f == NULL || f->tree[0] == NULL)
                continue;
            if (f->tree[0]->sel != FIT_SEL_ID)
                continue;
            src = entity_var_by_id(e, f->tree[0]->u.id);
            if (src == NULL)
                continue;
            reg = lib_instance_by_output(e, src->name);
            if (reg == NULL || reg->oe != NULL || reg->clk == NULL)
                continue;
            pad = th->d != NULL ? th->d : th->pin;
            if (reg->ap != NULL && list_find(e->inputs, th->oe, var_has_name) == 0)
                continue;
            if (absorb_into(e, reg, th, pad) == 0)
                continue;
            list_del_shift(e->net->items, i + 1);
            i--;
            pos = list_find(e->net->aliases, outv->name, alias_name_eq);
            if (pos != 0) {
                alias = e->net->aliases->items[pos - 1];
                list_del_shift(e->net->aliases, pos);
                alias_free(&alias);
            }
            entity_remove_fct(e, f);
            free(th);
            free(item);
        }
    }
}

/* turn every leaf `id` into "!id" and every "!x" whose operand
 * carries the value id into the plain leaf. */
static void fcttree_toggle_inv(FitFctTree *t, int id)
{
    switch (t->sel) {
    case FIT_SEL_NOT: {
        FitFctTree *child = t->u.children->items[0];

        /* the operand's selector is not looked at, only its id/value word;
         * the child list of the NOT node is not released */
        if (child->u.id == id) {
            t->sel = FIT_SEL_ID;
            t->u.id = id;
            free(child);
        }
        break;
    }
    case FIT_SEL_AND:
    case FIT_SEL_OR:
        for (int i = 0; i < (t->u.children == NULL ? 0 : t->u.children->count); i++)
            fcttree_toggle_inv(t->u.children->items[i], id);
        break;
    case FIT_SEL_ID:
        if (t->u.id == id) {
            FitFctTree *leaf;

            t->sel = FIT_SEL_NOT;
            t->u.children = list_create_sized(1);
            leaf = fcttree_new();
            leaf->sel = FIT_SEL_ID;
            leaf->u.id = id;
            list_add(t->u.children, leaf);
        }
        break;
    default:
        break;
    }
}

/* id <-> !id in tree[0] of every function */
void entity_toggle_inv(FitEntity *e, int id)
{
    for (int i = 0; i < (e->fcts == NULL ? 0 : e->fcts->count); i++)
        fcttree_toggle_inv(((FitFct *)e->fcts->items[i])->tree[0], id);
}

/* reset the statics of this file at the start of a run */
void base_entity_reset(void)
{
    g_entity_slots = 0;
    g_tree_gc = NULL;
    g_lib_prefix = NULL;
}
