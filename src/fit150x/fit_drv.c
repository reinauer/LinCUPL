/*
 * fit_drv.c - the fit driver of the ATF150x fitter, first part: the
 * globals of the fit modules, the product-term estimate of a function, and
 * one fitter pass (fit_drv_pass) with the stages it runs itself: check of
 * the pin preassignments, global pins, fast input latches, placement of
 * the functions of each block into macrocells, placement of the input
 * pins.  The partition into blocks, the search for macrocells and the
 * routing are the place modules' (place.h).
 *
 * The driver, the transformations it runs before the first pass and the
 * .pin / .io writers are in fit_drv_main.c.
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
/* Globals (declared in fit.h)                                          */
/* ------------------------------------------------------------------ */

int g_fit_route_failed;
int g_fit_place_failed;
int g_fit_retry_count;
int g_fit_plan_done;
int g_fit_report_failures;
int g_fit_preassign_used = 1;

FitList *g_fit_input_ids;       /* ids of the inputs placed by fit_drv_place_inputs */
FitList *g_fit_block_fcts;      /* per block: FitFct assigned by the partition */
FitList *g_fit_block_fanin;     /* per block: ids of the signals the block needs */
FitList *g_fit_excluded_ids;
FitList *g_fit_uim_slots;
FitList *g_fit_block_groups;
FitList *g_fit_block_placed;    /* per block: ids of the functions placed */
FitList *g_fit_doubling_fcts;   /* FitFct the partition left for fit_drv_place_leftovers */
FitList *g_fit_slots;
FitList *g_fit_global_names;
FitList *g_fit_block_foldbacks; /* per block: foldback ids */
int      g_fit_route_limit;
FitList *g_fit_must_global;     /* names of the signals that must be on a global pin */
FitList *g_fit_foldback_ids;
FitList *g_fit_work_fcts;       /* FitFct to place (alloc_build_fct_list) */

int      g_fit_hoist_mode;
FitList *g_fit_hoisted_ids;

/* The fast-inlatch list: names (char *) as given by the property until a
 * pass converts it, then FitFastInlatch items. */
#define g_fast_inlatch (g_opt.fast_inlatch_list)

/* 1 once a pass has converted the list to FitFastInlatch items.  Before
 * that (in the preparation, and in a pass when the fast_inlatch flag is
 * off) the lookups fit_drv_fast_inlatch_index / fit_drv_fast_inlatch_node
 * and fit_drv_place_fct still take each item as two ints: the first eight
 * bytes of the name string, little-endian, with zero bytes behind the end
 * of a short name.  fast_inlatch_word() computes that value without
 * reading behind the string. */
static int g_fast_inlatch_pairs;

/* word 0 (in_id) or 1 (node_id) of item i of the fast-inlatch list */
static int fast_inlatch_word(int i, int word)
{
    void *item = g_fast_inlatch->items[i];
    const unsigned char *name;
    size_t len, k;
    unsigned v = 0;

    if (g_fast_inlatch_pairs != 0) {
        FitFastInlatch *fi = (FitFastInlatch *)item;

        return word == 0 ? fi->in_id : fi->node_id;
    }
    name = (const unsigned char *)item;
    len = strlen((const char *)name);
    for (k = 0; k < 4; k++) {
        size_t off = (size_t)word * 4 + k;

        if (off < len)
            v |= (unsigned)name[off] << (8 * k);
    }
    return (int)v;
}

void fit_drv_reset(void)
{
    g_fit_route_failed = 0;
    g_fit_place_failed = 0;
    g_fit_retry_count = 0;
    g_fit_plan_done = 0;
    g_fit_report_failures = 0;
    g_fit_preassign_used = 1;
    g_fit_input_ids = g_fit_block_fcts = g_fit_block_fanin = g_fit_excluded_ids = NULL;
    g_fit_uim_slots = g_fit_block_groups = g_fit_block_placed = g_fit_doubling_fcts = NULL;
    g_fit_slots = g_fit_global_names = g_fit_block_foldbacks = g_fit_must_global = NULL;
    g_fit_foldback_ids = g_fit_work_fcts = NULL;
    g_fit_route_limit = 0;
    g_fit_hoist_mode = 0;
    g_fit_hoisted_ids = NULL;
    g_fast_inlatch_pairs = 0;
    fit_prep_reset();
}

void fit_unsupported(const char *what)
{
    fflush(stdout);
    fprintf(stderr, "fit150x: %s is not supported\n", what);
    exit(3);
}

void fit_internal_error(const char *what)
{
    fflush(stdout);
    fprintf(stderr, "fit150x: internal error: %s\n", what);
    exit(3);
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                        */
/* ------------------------------------------------------------------ */

static void free_str(char **s)          /* flatten_free_ref on a string */
{
    void *p = *s;

    flatten_free_ref(&p);
    *s = (char *)p;
}

static int is_free_name(const char *s)
{
    return util_str_icmp(s, FIT_FREE_NAME) == 0;
}

static FitSlot *slot_at(int index)
{
    if (index < 0 || index >= LN(g_fit_slots))
        fit_internal_error("slot_at: slot index out of range");
    return (FitSlot *)g_fit_slots->items[index];
}

static FitList *block_list(FitList *per_block, int index)
{
    if (index < 0 || index >= LN(per_block))
        fit_internal_error("block_list: block index out of range");
    return (FitList *)per_block->items[index];
}

static FitPreassignItem *preassign_at(int pos)
{
    return (FitPreassignItem *)g_preassign->items[pos - 1];
}

/* "<block letter><macrocell>p", the node name of a macrocell pin */
static char *pin_node_name(FitSlot *slot)
{
    char *s = (char *)util_calloc(1, 16);

    snprintf(s, 16, "%s%d%s", place_block_letter(slot), slot->macrocell, "p");
    return s;
}

/* ------------------------------------------------------------------ */
/* Groups of functions that share foldbacks (for alloc_partition)      */
/* ------------------------------------------------------------------ */

/* the functions of g_fit_block_groups that use foldback id;
 * their flags field is set to their number of foldbacks */
static FitList *fit_drv_fcts_using_foldback(FitEntity *ent, int id)
{
    FitList *res = list_create();
    int i;

    for (i = 0; i < LN(g_fit_block_groups); i++) {
        FitFct *fct = (FitFct *)g_fit_block_groups->items[i];
        FitList *fbs = alloc_fct_foldbacks(ent, fct);

        if (list_find(fbs, FIT_INT2PTR(id), list_ptr_eq) != 0) {
            fct->flags = LN(fbs);
            list_add(res, fct);
        }
        list_destroy(&fbs);
    }
    return res;
}

/* for every foldback id the list of functions of g_fit_block_groups that
 * use it (empty lists left out). */
static FitList *fit_drv_foldback_user_lists(FitEntity *ent)
{
    FitList *res = list_create();
    int i;

    for (i = 0; i < LN(g_fit_foldback_ids); i++) {
        FitList *l = fit_drv_fcts_using_foldback(ent, FIT_PTR2INT(g_fit_foldback_ids->items[i]));

        if (l != NULL && l->count != 0)
            list_add(res, l);
    }
    return res;
}

/* takes the first list and merges into it every other list that
 * intersects it, until nothing changes; sorted by flags */
static FitList *fit_drv_merge_group(FitEntity *ent, FitList *lists)
{
    FitList *group = (FitList *)lists->items[0];
    int stable;

    (void)ent;
    list_del_shift(lists, 1);
    do {
        int pos = 1;
        int i = 0;

        stable = 1;
        while (i < LN(lists)) {
            FitList *cur = (FitList *)lists->items[i];

            if (list_intersection(cur, group, alloc_first_int_eq) != NULL) {
                group = list_union(group, cur, alloc_first_int_eq);
                list_del_shift(lists, pos);
                i--;
                pos--;
                stable = 0;
            }
            i++;
            pos++;
        }
    } while (stable == 0);
    list_sort(group, 0, route_fct_weight_cmp);
    return group;
}

void fit_drv_build_groups(FitEntity *ent)
{
    FitList *lists = fit_drv_foldback_user_lists(ent);
    FitList *groups = list_create();

    while (lists != NULL && lists->count != 0)
        list_add(groups, fit_drv_merge_group(ent, lists));
    g_fit_block_groups = groups;
}

/* ------------------------------------------------------------------ */
/* Product-term estimate                                                */
/* ------------------------------------------------------------------ */

/* does the input `name` sit on a pin of the kind (3 GCLK,
 * 4 GCLR, 6 OE)? */
static int fit_drv_on_global_pin(const char *name, int kind)
{
    char *node = NULL;
    FitSlot *slot = route_node_of_name(name, &node);
    FitList *pins;

    free_str(&node);
    if (slot == NULL)
        return 0;
    if (kind == 3)
        pins = dev_gclk_pin_list();
    else if (kind == 4)
        pins = dev_gclr_pin_list();
    else if (kind == 6)
        pins = dev_oe_pin_list();
    else
        return 0;
    return list_find(pins, FIT_INT2PTR(slot->pin), list_ptr_eq) != 0;
}

/* a clock whose source is preassigned to a GCLK pin is global */
static int fit_drv_clock_preassigned_global(FitEntity *ent, int kind, int id)
{
    char *src;
    int k;

    if (kind != 3 || g_fit_preassign_used == 0)
        return 0;
    src = latch_signal_source(ent, entity_var_by_id(ent, id)->name);
    if (list_find(ent->inputs, src, var_has_name) != 0)
        return 0;
    k = list_find(g_preassign, src, place_name_eq);
    if (k == 0)
        return 0;
    if (list_find(dev_gclk_pin_list(), FIT_INT2PTR(preassign_at(k)->pin), list_ptr_eq) == 0)
        return 0;
    if (list_find(g_fit_global_names, src, fit_str_ieq_cb) == 0)
        list_add(g_fit_global_names, src);
    return 1;
}

/* 1 when the control signal `name` of kind `kind` takes a
 * product term; its inputs are added to seen */
static int fit_drv_ctrl_needs_term(FitEntity *ent, FitList *seen, char *name,
                                   int can_be_global, int kind)
{
    FitFctTree *t;
    int id;

    if (name == NULL)
        return 0;
    id = entity_var_id_by_name(ent, name);
    if (list_find(ent->inputs, name, var_has_name) != 0) {
        if (can_be_global != 0
            && list_find(g_fit_global_names, name, fit_str_ieq_cb) != 0
            && fit_drv_on_global_pin(name, kind) != 0)
            return 0;
        if (list_find(seen, FIT_INT2PTR(id), list_ptr_eq) == 0)
            list_add(seen, FIT_INT2PTR(entity_var_id_by_name(ent, name)));
        return 1;
    }
    if (entity_find_fct(ent, id) == NULL)   /* no function: stop */
        fit_internal_error("fit_drv_ctrl_needs_term: control signal without function");
    t = entity_find_fct(ent, id)->tree[0];
    if (can_be_global != 0) {
        if (t->sel == FIT_SEL_ID)
            id = t->u.id;
        else if (t->sel == FIT_SEL_NOT)
            id = ((FitFctTree *)t->u.children->items[0])->u.id;
        else
            goto count;
        if (list_find(g_opt.global_oe, FIT_INT2PTR(id), list_ptr_eq) != 0)
            return 0;
        if (fit_drv_clock_preassigned_global(ent, kind, id) != 0)
            return 0;
    }
count:
    if (t->sel == FIT_SEL_OR) {
        if (list_find(seen, FIT_INT2PTR(id), list_ptr_eq) == 0)
            list_add(seen, FIT_INT2PTR(id));
    } else {
        if (t->sel == FIT_SEL_CONST)
            return 0;
        place_collect_inputs(ent, t, seen);
    }
    return 1;
}

int fit_drv_count_ctrl_terms(FitEntity *ent, FitList *seen, char *name)
{
    FitInstance *inst = netlist_find_instance(ent, name);
    int a, b, c, d, e;

    if (inst == NULL)
        return 0;
    a = fit_drv_ctrl_needs_term(ent, seen, inst->clk, 1, 3);
    b = fit_drv_ctrl_needs_term(ent, seen, inst->ar, 1, 4);
    c = fit_drv_ctrl_needs_term(ent, seen, inst->oe, 1, 6);
    d = fit_drv_ctrl_needs_term(ent, seen, inst->ap, 0, 5);
    e = fit_drv_ctrl_needs_term(ent, seen, inst->ce, 0, 7);
    return e + a + b + c + d;
}

int fit_drv_count_mc_terms(FitEntity *ent, FitFct *fct, FitVariable *var, FitList *inputs)
{
    int n;

    if (list_find(g_opt.xor_list, FIT_INT2PTR(var->id), list_ptr_eq) == 0) {
        if (fit_drv_fast_inlatch_index(var->id) == 0)
            place_collect_inputs(ent, fct->tree[0], inputs);
        n = place_count_terms(ent, fct);
    } else {
        n = place_xor_terms(ent, inputs, fct);
        if (n == 0)
            return 0;
    }
    if (latch_find_alias(var->name, ent->net) == 0)
        return n;
    return fit_drv_count_ctrl_terms(ent, inputs, var->name) + n;
}

int fit_drv_count_borrowed(FitEntity *ent, int n_mc, int n_pt, FitVariable *var)
{
    int n;

    if (n_pt % 5 == 0)
        return 0;
    if (n_pt < 5)
        return 1;
    n = n_mc * 5 - n_pt;
    if (n_mc < n)
        n = n_mc;
    else if (n != n_mc)
        return n;
    if (list_find(g_opt.xor_list, FIT_INT2PTR(var->id), list_ptr_eq) != 0) {
        FitList *seen = list_create();

        if (fit_drv_count_ctrl_terms(ent, seen, var->name) == 3)
            n--;
        list_destroy(&seen);
    }
    return n;
}

int fit_drv_is_single_term_reg(FitEntity *ent, FitFct *fct, FitVariable *var, int n_pt)
{
    FitNetItem *item;

    if (n_pt != 5)
        return 0;
    if (place_count_terms(ent, fct) != 1)
        return 0;
    if (latch_find_alias(var->name, ent->net) == 0)
        return 0;
    item = netlist_find_item(ent, var->name);
    if (item == NULL)       /* no netlist item: stop */
        fit_internal_error("fit_drv_is_single_term_reg: register output without cell");
    if (item->kind == FIT_GATE_TFF)
        return 0;
    if (item->cell == NULL)
        return 0;
    return ((FitInstance *)item->cell)->oe == NULL;
}

int fit_drv_fast_inlatch_index(int node_id)
{
    int i;

    for (i = 0; i < LN(g_fast_inlatch); i++)
        if (fast_inlatch_word(i, 1) == node_id)
            return i + 1;
    return 0;
}

int fit_drv_fast_inlatch_node(int in_id)
{
    int i;

    for (i = 0; i < LN(g_fast_inlatch); i++)
        if (fast_inlatch_word(i, 0) == in_id)
            return fast_inlatch_word(i, 1);
    return 0;
}

/* The driver puts the saved list (names, or NULL) back before each pass. */
void fit_drv_restore_fast_inlatch(FitList *saved)
{
    list_destroy(&g_fast_inlatch);
    g_fast_inlatch = list_copy(saved);
    g_fast_inlatch_pairs = 0;
}

/* ------------------------------------------------------------------ */
/* Input pins                                                           */
/* ------------------------------------------------------------------ */

/* node name of a dedicated input slot */
static void fit_drv_ded_node_name(FitSlot *slot, char **node)
{
    FitList *gclk = dev_gclk_pin_list();
    FitList *gclr = dev_gclr_pin_list();
    FitList *oe = dev_oe_pin_list();
    int k = list_find(oe, FIT_INT2PTR(slot->pin), list_ptr_eq);

    if (k == 0) {
        if (list_find(gclk, FIT_INT2PTR(slot->pin), list_ptr_eq) != 0) {
            *node = util_strdup("GCLK");
            return;
        }
        if (list_find(gclr, FIT_INT2PTR(slot->pin), list_ptr_eq) != 0)
            *node = util_strdup("GCLR");
        return;
    }
    if (k == 1)
        *node = util_strdup("OE1");
    else
        *node = util_strdup("OE2");
}

/* is `name` (a buried signal) a fast-inlatch node? */
static int fit_drv_is_fast_inlatch_signal(FitEntity *ent, char *name)
{
    FitInstance *inst;

    if (g_opt.fast_inlatch == 0)
        return 0;
    if (is_free_name(name))
        return 0;
    inst = lib_instance_by_output(ent, name);
    if (inst == NULL)
        return 0;
    return fit_drv_fast_inlatch_index(entity_var_id_by_name(ent, inst->q)) != 0;
}

/* a free pin for an input: macrocells with nothing in them,
 * then macrocells whose buried signal is not a fast-inlatch node, then the
 * dedicated inputs; pins someone is preassigned to are skipped */
static FitSlot *fit_drv_free_input_slot(FitEntity *ent, char **node)
{
    int n_mc = g_dev->n_macrocells;
    int i;

    for (i = 0; i < n_mc; i++) {
        FitSlot *slot = slot_at(i);

        if (slot->pin != 0 && is_free_name(slot->signal) && is_free_name(slot->buried)
            && place_preassign_index_of_pin(slot->pin) == 0) {
            *node = pin_node_name(slot);
            return slot;
        }
    }
    for (i = 0; i < n_mc; i++) {
        FitSlot *slot = slot_at(i);

        if (slot->pin != 0 && is_free_name(slot->signal)
            && place_preassign_index_of_pin(slot->pin) == 0
            && fit_drv_is_fast_inlatch_signal(ent, slot->buried) == 0) {
            *node = pin_node_name(slot);
            return slot;
        }
    }
    for (i = n_mc; i < g_dev->n_ded_inputs + n_mc; i++) {
        FitSlot *slot = slot_at(i);

        if (slot->pin != 0 && is_free_name(slot->signal)
            && place_preassign_index_of_pin(slot->pin) == 0) {
            const char *name;

            if (i == n_mc)
                name = "OE2";
            else if (i == n_mc + 1)
                name = "GCLK";
            else if (i == n_mc + 2)
                name = "GCLR";
            else
                name = "OE1";
            *node = util_strdup(name);
            return slot;
        }
    }
    return NULL;
}

/* SSTL inputs have their pad selected before output placement. */
static int fit_drv_sstl_name(const char *name)
{
    return g_opt.sstl_input && (g_opt.sstl_list == NULL ||
        list_find(g_opt.sstl_list, (void *)name, fit_str_ieq_cb) != 0);
}

/* gives input id a pin; *node receives the node name */
static FitSlot *fit_drv_place_input(FitEntity *ent, int id, char **node)
{
    FitVariable *v = entity_var_by_id(ent, id);
    FitSlot *slot;
    int k, node_id;

    if (fit_drv_sstl_name(v->name)) {
        for (int i = 0; i < g_dev->n_macrocells; i++) {
            slot = slot_at(i);
            if (util_str_ieq(slot->signal, v->name)) {
                *node = pin_node_name(slot);
                return slot;
            }
        }
    }
    if (g_fit_preassign_used != 0) {
        k = list_find(g_preassign, v->name, place_name_eq);
        if (k != 0) {
            slot = place_slot_of_pin(preassign_at(k)->pin);
            if (slot == NULL) {
                fprintf(g_msg, "Pin number does not exist\n");
                return NULL;
            }
            if (slot->block == 0)
                fit_drv_ded_node_name(slot, node);
            else
                *node = pin_node_name(slot);
            goto take;
        }
    }
    node_id = fit_drv_fast_inlatch_node(id);
    if (node_id != 0) {
        FitVariable *nv = entity_var_by_id(ent, node_id);

        slot = route_node_of_name(latch_signal_source(ent, nv->name), node);
        free_str(node);
        if (slot == NULL)   /* no slot: stop */
            fit_internal_error("fit_drv_place_input: fast-inlatch node not placed");
        *node = pin_node_name(slot);
        return slot;
    }
    slot = fit_drv_free_input_slot(ent, node);
take:
    if (slot == NULL)
        return NULL;
    /* the extended mode also accepts the slot when the input is there already */
    if (FIT_EXT && util_str_icmp(slot->signal, v->name) == 0)
        return slot;
    if (is_free_name(slot->signal)) {
        slot->signal = util_strdup(v->name);
        return slot;
    }
    return NULL;
}

/* places the input pins, blocks from the last to the first;
 * returns per block the node names of the inputs the block needs */
static FitList *fit_drv_place_inputs(FitEntity *ent, FitList *block_signals)
{
    FitList *res;
    char *node = NULL;
    int b, k;

    g_fit_input_ids = list_create();
    res = list_create();
    for (b = 0; b < g_dev->n_blocks; b++)
        list_add(res, list_create());
    for (b = g_dev->n_blocks - 1; b >= 0; b--) {
        FitList *sig = block_list(block_signals, b);
        int i = 0;
        int pos = 1;

        while (i < LN(sig)) {
            int id = FIT_PTR2INT(sig->items[i]);

            if (list_find(g_opt.global_oe, FIT_INT2PTR(id), list_ptr_eq) != 0) {
                list_del_shift(sig, pos);
                continue;
            }
            if (entity_has_input_id(ent, id) != 0) {
                FitVariable *v = entity_var_by_id(ent, id);
                char *name = v->name;

                if (latch_find_alias(name, ent->net) == 0) {
                    FitSlot *slot;

                    if (list_find(g_fit_global_names, name, fit_str_ieq_cb) == 0) {
                        slot = fit_drv_place_input(ent, id, &node);
                        list_add(g_fit_input_ids, FIT_INT2PTR(id));
                    } else {
                        slot = place_node_of_signal(name, &node);
                    }
                    if (slot == NULL) {
                        fprintf(g_fit_report, "Variable : %s can not be placed\n", name);
                        g_fit_place_failed = 1;
                    } else {
                        if (g_equiv_verbose != 0)
                            fprintf(g_msg, "Variable : %s  placed at MC%d\n", name,
                                    slot->macrocell);
                        for (k = 0; k < g_dev->n_blocks; k++)
                            if (list_find(block_list(g_fit_block_fanin, k), FIT_INT2PTR(id),
                                          list_ptr_eq) != 0)
                                list_add(block_list(res, k), node);
                    }
                }
            }
            i++;
            pos++;
        }
    }
    return res;
}

/* is the preassignment `item` respected? */
static int fit_drv_preassign_respected(FitList *slots, FitPreassignItem *item)
{
    int i;

    for (i = 0; i < LN(slots); i++) {
        FitSlot *slot = (FitSlot *)slots->items[i];

        if (strcmp(slot->signal, item->name) == 0)
            return slot->pin == item->pin;
        if (strcmp(slot->buried, item->name) == 0) {
            int index = item->pin - 601;

            if (index < 0 || index >= LN(g_fit_slots))  /* no such macrocell */
                return 0;
            return ((FitSlot *)g_fit_slots->items[index])->macrocell == item->pin - 600;
        }
    }
    return 0;
}

/* Checks that every preassigned signal of the design sits on its pin or
 * node; prints "Pin assign not respected" and returns 0 otherwise.  Used
 * only with verbose messages. */
int fit_drv_verify_preassign(FitEntity *ent, FitList *slots)
{
    int i;

    for (i = 0; i < LN(g_preassign); i++) {
        FitPreassignItem *item = (FitPreassignItem *)g_preassign->items[i];

        if (entity_find_var(ent, item->name) != NULL
            && fit_drv_preassign_respected(slots, item) == 0) {
            printf("\nPin assign not respected for (%s; %d)\n", item->name, item->pin);
            return 0;
        }
    }
    return 1;
}

/* input pins, UIM assignment and routing */
static int fit_drv_assign_inputs_and_route(FitEntity *ent, FitList *block_signals)
{
    FitList *in_nodes = fit_drv_place_inputs(ent, block_signals);
    FitList *mc_nodes = place_block_node_names(ent);
    FitList *names = list_create();
    FitList *cands, *failed;
    int b;

    for (b = 0; b < g_dev->n_blocks; b++)
        list_add(names, list_union(block_list(mc_nodes, b), block_list(in_nodes, b),
                                   fit_str_ieq_cb));
    cands = place_assign_uim(names);
    failed = list_create();
    route_commit(ent, cands, names, failed);
    if (g_equiv_verbose != 0) {
        printf("\nVerification avant backtracking\n");
        fit_drv_verify_preassign(ent, g_fit_slots);
    }
    if (g_fit_place_failed == 0 && g_fit_route_failed != 0)
        route_repair(ent, cands, failed, names);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Placement of functions into macrocells                               */
/* ------------------------------------------------------------------ */

/* places the function of var into a macrocell of block
 * (1-based).  Returns 1 when placed. */
static int fit_drv_place_fct(FitEntity *ent, FitVariable *var, int block)
{
    FitFct *fct = entity_find_fct(ent, var->id);
    int mode = entity_var_is_node(ent, var) != 0 ? 5 : 1;
    char *src = latch_signal_source(ent, var->name);
    int src_id = entity_var_id_by_name(ent, src);
    int mc_pre = 0;
    int b, top, k;

    k = list_find(g_preassign, src, place_name_eq);
    if (k != 0)
        mc_pre = place_macrocell_of_pin(preassign_at(k)->pin);
    for (b = 1, top = 16; b <= g_dev->n_blocks; b++, top += 16) {
        FitList *ids, *slots;
        FitSlot *slot;
        int n_pt, fi, full, j;

        if (block != b || (mc_pre != 0 && mc_pre > top))
            continue;
        ids = list_copy(block_list(g_fit_block_fanin, b - 1));
        n_pt = fit_drv_count_mc_terms(ent, fct, var, ids);
        fi = fit_drv_fast_inlatch_index(var->id);
        if (fi != 0) {
            slots = place_find_slots(ent, b, n_pt, 1, mc_pre, src_id);
        } else if (g_opt.logic_doubling == 0
                   || fit_drv_is_single_term_reg(ent, fct, var, mode) == 0) {
            slots = place_find_slots(ent, b, n_pt, mode, mc_pre, src_id);
        } else {
            slots = place_find_slots(ent, b, n_pt, 0x18, mc_pre, src_id);
            if (LN(slots) == 0)
                slots = place_find_slots(ent, b, n_pt, 1, mc_pre, src_id);
            if (LN(slots) == 0)
                slots = place_find_slots(ent, b, n_pt, 5, mc_pre, src_id);
        }
        if (LN(slots) == 0) {
            if (mode != 5)
                continue;
            slots = place_find_slots(ent, b, n_pt, 1, mc_pre, src_id);
            if (LN(slots) == 0)
                continue;
        }
        slot = (FitSlot *)slots->items[0];
        if (mode == 1) {
            slot->fct_id = fct->var_id;
            slot->signal = util_strdup(src);
        } else {
            slot->buried = util_strdup(src);
            if (fi != 0) {
                FitVariable *in = entity_var_by_id(ent, fast_inlatch_word(fi - 1, 0));

                if (in == NULL)     /* no input: stop */
                    fit_internal_error("fit_drv_place_fct: fast-inlatch input not in the design");
                slot->signal = util_strdup(in->name);
            }
        }
        if (list_find(g_opt.global_oe, FIT_INT2PTR(src_id), list_ptr_eq) != 0) {
            /* the extended mode falls back to the pin's multiplexer input */
            if (dev_find_global_oe(slot, 1, mode) == 0 && FIT_EXT)
                dev_find_global_oe(slot, 1, 1);
            list_add(g_fit_global_names, src);
        }
        slot->fn_kind = util_strdup(netlist_find_instance(ent, var->name) != NULL ? "reg" : "com");
        full = fit_drv_count_borrowed(ent, slots->count, n_pt, var);
        if (slots->count == full)
            full--;
        else
            slot->foldback = util_strdup("NA");
        if (g_equiv_verbose != 0)
            printf("Equation :  %s placed at macrocell : MC%d", src, slot->macrocell);
        for (j = slots->count - 1; j > 0; j--) {
            slot = (FitSlot *)slots->items[j];
            slot->cascade = util_strdup(src);
            slot->fn_kind = util_strdup("com");
            if (g_equiv_verbose != 0)
                printf(" MC%d", slot->macrocell);
            if (j == full)
                full--;
            else
                slot->foldback = util_strdup("NA");
        }
        if (g_equiv_verbose != 0)
            printf("\n");
        fit_drv_count_mc_terms(ent, fct, var, block_list(g_fit_block_fanin, slot->block - 1));
        list_add(block_list(g_fit_block_placed, slot->block - 1), FIT_INT2PTR(var->id));
        return 1;
    }
    return 0;
}

/* foldback id takes the highest free foldback slot of block
 * (0-based) */
static int fit_drv_place_foldback(FitEntity *ent, int id, int block)
{
    int i;

    for (i = block * 16 + 15; i >= block * 16; i--) {
        FitSlot *slot = slot_at(i);

        if (is_free_name(slot->foldback)) {
            slot->foldback = util_strdup(entity_var_by_id(ent, id)->name);
            return 1;
        }
    }
    return 0;
}

/* places the foldbacks block (0-based) needs */
static void fit_drv_place_block_foldbacks(FitEntity *ent, int block)
{
    int i;

    for (i = 0; i < LN(g_fit_foldback_ids); i++) {
        int id = FIT_PTR2INT(g_fit_foldback_ids->items[i]);

        if (list_find(block_list(g_fit_block_foldbacks, block), FIT_INT2PTR(id), list_ptr_eq) != 0
            && fit_drv_place_foldback(ent, id, block) == 0) {
            fprintf(g_fit_report, "ERROR: can not place a foldback  %s in block %d\n",
                    entity_var_by_id(ent, id)->name, block + 1);
            g_fit_place_failed = 1;
        }
    }
}

/* orders the functions of a block: pin functions, then node
 * functions, each ascending by number of product terms */
static void fit_drv_sort_block(FitEntity *ent, FitList *fcts)
{
    FitList *inputs = list_create();
    FitList *nodes = list_create();
    int i = 0;
    int pos = 1;

    while (i < LN(fcts)) {
        FitFct *fct = (FitFct *)fcts->items[i];
        FitVariable *v = entity_var_by_id(ent, fct->var_id);

        fct->flags = fit_drv_count_mc_terms(ent, fct, v, inputs);
        if (entity_var_is_node(ent, v) != 0) {
            list_add(nodes, fct);
            list_del_shift(fcts, pos);
            i--;
            pos--;
        }
        i++;
        pos++;
    }
    list_sort(fcts, 0, route_fct_weight_cmp);
    list_sort(nodes, 0, route_fct_weight_cmp);
    list_append_destroy(fcts, &nodes);
}

/* places the functions of every block */
static int fit_drv_place_blocks(FitEntity *ent)
{
    int b;

    g_fit_block_placed = list_create();
    for (b = 0; b < g_dev->n_blocks; b++)
        list_add(g_fit_block_placed, list_create());
    for (b = 0; b < g_dev->n_blocks; b++) {
        FitList *fcts = block_list(g_fit_block_fcts, b);
        int i = 0;
        int pos = 1;

        fit_drv_sort_block(ent, fcts);
        while (i < LN(g_opt.global_oe)) {
            FitVariable *v = entity_var_by_id(ent, FIT_PTR2INT(g_opt.global_oe->items[i]));
            FitFct *fct;
            int k;

            if (v == NULL) {
                list_del_shift(g_opt.global_oe, pos);
                continue;
            }
            fct = place_fct_of_var(ent, v);
            if (fct == NULL) {
                list_del_shift(g_opt.global_oe, pos);
                continue;
            }
            k = list_find(fcts, fct, alloc_first_int_eq);
            if (k != 0) {
                list_del_shift(fcts, k);
                list_insert_at(fcts, fct, 1);
            }
            i++;
            pos++;
        }
        for (i = 0; i < LN(fcts); i++) {
            FitVariable *v = entity_var_by_id(ent, ((FitFct *)fcts->items[i])->var_id);

            if (fit_drv_place_fct(ent, v, b + 1) == 0) {
                if (g_fit_report_failures != 0)
                    fprintf(g_fit_report, "Fail to place fonction : %s\n", v->name);
                g_fit_place_failed = 1;
            }
        }
        if (g_opt.logic_doubling == 0)
            fit_drv_place_block_foldbacks(ent, b);
    }
    return g_fit_place_failed == 0;
}

/* logic doubling: puts the combinational function fct on the
 * pin of a macrocell that holds a buried one-term register without output
 * enable */
static int fit_drv_double_up(FitEntity *ent, FitFct *fct)
{
    FitVariable *v;
    char *src;
    int k = 0;
    int i;

    if (list_find(g_opt.xor_list, FIT_INT2PTR(fct->var_id), list_ptr_eq) != 0)
        return 0;
    v = entity_var_by_id(ent, fct->var_id);
    src = latch_signal_source(ent, v->name);
    for (i = 0; i < g_dev->n_macrocells; i++) {
        FitSlot *slot;
        FitInstance *inst;
        FitVariable *rv;
        FitFct *rf;
        FitList *ids;
        int kind = 0;
        int n_reg, n_fct;

        if (k != 0)
            return 0;
        if (g_fit_preassign_used == 0) {
            slot = slot_at(i);
        } else {
            k = list_find(g_preassign, src, place_name_eq);
            if (k == 0) {
                slot = slot_at(i);
            } else {
                slot = place_slot_of_pin(preassign_at(k)->pin);
                if (slot == NULL)
                    return 0;
            }
        }
        if (slot->pin == 0 || slot->polarity == -1 || !is_free_name(slot->signal)
            || is_free_name(slot->buried) || util_str_icmp(slot->fn_kind, "com") == 0)
            continue;
        inst = tt3_find_instance_by_input(ent, slot->buried, &kind);
        if (inst == NULL || (kind != 3 && kind != 4) || inst->oe != NULL)
            continue;
        rv = entity_find_var(ent, slot->buried);
        rf = place_fct_of_var(ent, rv);
        if (place_count_terms(ent, rf) != 1)
            continue;
        ids = list_copy(block_list(g_fit_block_fanin, slot->block - 1));
        n_reg = fit_drv_count_mc_terms(ent, rf, entity_var_by_id(ent, rf->var_id), ids);
        n_fct = fit_drv_count_mc_terms(ent, fct, v, ids);
        if (n_reg + n_fct >= 6)
            continue;
        if (!is_free_name(slot->foldback) && util_str_icmp(slot->foldback, "NA") != 0
            && n_reg + n_fct >= 5)
            continue;
        if (g_fit_route_limit < LN(ids))
            return 0;
        if (list_find(g_opt.global_oe, FIT_INT2PTR(v->id), list_ptr_eq) != 0) {
            if (dev_find_global_oe(slot, 0, 1) == 0)
                continue;
            list_add(g_fit_global_names, src);
        }
        slot->signal = util_strdup(src);
        slot->fct_id = fct->var_id;
        if (list_find(g_opt.global_oe, FIT_INT2PTR(v->id), list_ptr_eq) != 0)
            dev_find_global_oe(slot, 1, 1);
        if (n_fct + n_reg == 5)
            slot->foldback = util_strdup("NA");
        g_fit_block_fanin->items[slot->block - 1] = list_copy(ids);
        alloc_merge_block_foldbacks(alloc_fct_foldbacks(ent, fct), slot->block - 1, 1);
        return 1;
    }
    return 0;
}

/* with logic doubling, places the functions the partition left
 * in g_fit_doubling_fcts, raising the route limit up to 40; then the foldbacks */
static int fit_drv_place_leftovers(FitEntity *ent)
{
    FitList *left = g_fit_doubling_fcts;
    int b;

    if (g_opt.logic_doubling == 0)
        return 1;
    for (; g_fit_route_limit < 0x29; g_fit_route_limit++) {
        int pos = 1;
        int i = 0;

        while (i < LN(left)) {
            FitFct *fct = (FitFct *)left->items[i];

            if (fit_drv_double_up(ent, fct) == 0) {
                FitVariable *v = entity_var_by_id(ent, fct->var_id);

                for (b = 0; b < g_dev->n_blocks; b++) {
                    FitList *ids = list_copy(block_list(g_fit_block_fanin, b));

                    fit_drv_count_mc_terms(ent, fct, v, ids);
                    if (LN(ids) <= g_fit_route_limit
                        && fit_drv_place_fct(ent, v, b + 1) != 0) {
                        g_fit_block_fanin->items[b] = list_copy(ids);
                        list_del_shift(g_fit_doubling_fcts, pos);
                        alloc_merge_block_foldbacks(alloc_fct_foldbacks(ent, fct), b, 1);
                        i--;
                        pos--;
                        break;
                    }
                }
            } else {
                list_del_shift(g_fit_doubling_fcts, pos);
                i--;
                pos--;
            }
            i++;
            pos++;
            left = g_fit_doubling_fcts;
        }
        if (left == NULL || left->count == 0)
            break;
    }
    for (b = 0; b < g_dev->n_blocks; b++)
        fit_drv_place_block_foldbacks(ent, b);
    left = g_fit_doubling_fcts;
    if ((left == NULL || left->count == 0) && g_fit_place_failed == 0)
        return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Preassignments, global pins, fast input latches                      */
/* ------------------------------------------------------------------ */

/* is *pin a pin or node number of the device?  On the ATF1502 the
 * numbers 45..76 are mapped to 301..332 and 77..108 to 601..632 first. */
static int fit_drv_pin_exists(int *pin)
{
    int mc = 0;
    int p, i;

    if (util_str_icmp(g_family, "1502") == 0 && *pin < 0x6d) {
        if (*pin >= 0x4d)
            *pin += 0x20c;
        else if (*pin >= 0x2d)
            *pin += 0x100;
    }
    p = *pin;
    if (p >= 600)
        mc = p - 600;
    else if (p > 299)
        mc = p - 300;
    for (i = 0; i < g_dev->n_ded_inputs + g_dev->n_macrocells; i++) {
        FitSlot *slot = slot_at(i);

        if (mc == 0) {
            if (slot->pin == p)
                return 1;
        } else if (slot->macrocell == mc) {
            return 1;
        }
    }
    return 0;
}

/* checks the pin preassignments; drops those of unknown names
 * and, at the end, the foldback placements (300..599) */
static int fit_drv_check_preassign(FitEntity *ent)
{
    FitList *seen = list_create();
    FitList *jtag = NULL;
    FitList *pd = NULL;
    int i = 0;
    int pos = 1;

    if (g_opt.jtag != 0)
        jtag = dev_jtag_pin_list();
    if (g_opt.pd1 != 0 || g_opt.pd2 != 0)
        pd = dev_pd_pin_list();
    while (i < LN(g_preassign)) {
        FitPreassignItem *item = (FitPreassignItem *)g_preassign->items[i];

        if (entity_find_var(ent, item->name) == NULL) {
            list_del_shift(g_preassign, pos);
            continue;
        }
        if (fit_drv_pin_exists(&item->pin) == 0) {
            fprintf(g_fit_report, FIT_EXT ? "## ERROR : Bad user pin assignment : %d\n"
                                            : "## ERROR : Bad user pin assignement : %d\n", item->pin);
            return 0;
        }
        if (list_find(seen, FIT_INT2PTR(item->pin), list_ptr_eq) != 0) {
            fprintf(g_fit_report, "## ERROR : Pin used more than once : %d\n", item->pin);
            return 0;
        }
        list_add(seen, FIT_INT2PTR(item->pin));
        if (g_opt.jtag != 0
            && list_find(jtag, FIT_INT2PTR(item->pin), list_ptr_eq) != 0) {
            fprintf(g_fit_report, FIT_EXT ? "## ERROR : Pin assigned to JTAG pin : %d\n"
                                            : "## ERROR : Pin assigned to jtag's pin : %d\n", item->pin);
            return 0;
        }
        if (g_opt.pd1 != 0 && LN(pd) > 0 && FIT_PTR2INT(pd->items[0]) == item->pin) {
            fprintf(g_fit_report, "## ERROR : Pin assigned to power down pin 1: %d\n",
                    item->pin);
            return 0;
        }
        if (g_opt.pd2 != 0 && LN(pd) > 1 && FIT_PTR2INT(pd->items[1]) == item->pin) {
            fprintf(g_fit_report, "## ERROR : Pin assigned to power down pin 2: %d\n",
                    item->pin);
            return 0;
        }
        i++;
        pos++;
    }
    i = 0;
    pos = 1;
    while (i < LN(g_preassign)) {
        int pin = ((FitPreassignItem *)g_preassign->items[i])->pin;

        if (pin > 299 && pin < 600) {
            list_del_shift(g_preassign, pos);
            continue;
        }
        i++;
        pos++;
    }
    list_destroy(&seen);
    return 1;
}

/* Reserve VREF only for banks containing an SSTL input. Supplies are
 * external: these constraints select legal pads, not voltage fuses. */
static int fit_drv_sstl_pad_ok(FitSlot *slot)
{
    if (slot == NULL || slot->pin <= 0 || slot->macrocell <= 0 ||
        !is_free_name(slot->signal))
        return 0;
    int bank = (slot->macrocell - 1) >= g_dev->n_macrocells / 2;
    int voltage = bank ? g_opt.voltage_b : g_opt.voltage_a;
    int refmc = fit_be_vref_macrocell(g_dev_desc->id, bank);
    if (voltage < 2 || refmc < 0 || slot->macrocell == refmc + 1)
        return 0;
    FitSlot *ref = slot_at(refmc);
    return ref->pin > 0 && place_preassign_index_of_pin(ref->pin) == 0 &&
        (is_free_name(ref->signal) ||
         util_str_ieq(ref->signal, bank ? "VREFB" : "VREFA"));
}

static int fit_drv_place_sstl(FitEntity *ent)
{
    if (!g_opt.sstl_input)
        return 1;
    if (!g_opt.be || g_dev_desc->id == FIT_DEV_1502) {
        fprintf(g_msg, "SSTL inputs require ATF1504BE or ATF1508BE\n");
        return 0;
    }
    FitList *pads = list_create();
    for (int i = 0; i < LN(ent->inputs); i++) {
        FitVariable *v = ent->inputs->items[i];
        if (util_str_ieq(latch_signal_source(ent, v->name), v->name))
            list_add(pads, v);
    }
    for (int i = 0; ent->net != NULL && i < LN(ent->outputs); i++) {
        FitVariable *v = ent->outputs->items[i];
        if (!util_str_ieq(latch_signal_source(ent, v->name), v->name))
            continue;
        int kind = tt3_gate_kind_of_output(ent, v->name);
        FitInstance *inst = netlist_find_instance(ent, v->name);
        if (inst == NULL && ent->net != NULL)
            inst = tt3_find_instance_by_alias(ent, v->name, &kind);
        if (inst != NULL && (inst->pin != NULL ||
            ((kind == FIT_GATE_TH || kind == FIT_GATE_ITH) && inst->d != NULL)))
            list_add(pads, v);
    }
    for (int i = 0; i < LN(g_opt.sstl_list); i++) {
        char *name = g_opt.sstl_list->items[i];
        if (list_find(pads, name, var_has_name) == 0) {
            fprintf(g_msg, "SSTL input is not an input signal: %s\n", name);
            list_destroy(&pads);
            return 0;
        }
    }
    /* Place constrained inputs first so automatic choices cannot consume
     * a bank's reference pad before its explicitly selected input. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < LN(pads); i++) {
            FitVariable *v = pads->items[i];
            if (!fit_drv_sstl_name(v->name))
                continue;
            int k = g_fit_preassign_used ?
                list_find(g_preassign, v->name, place_name_eq) : 0;
            if ((k == 0) != (pass != 0))
                continue;
            FitSlot *slot = k ? place_slot_of_pin(preassign_at(k)->pin) : NULL;
            /* ON applies to I/O pads; dedicated constrained inputs stay CMOS. */
            if (slot != NULL && slot->macrocell == 0 && g_opt.sstl_list == NULL)
                continue;
            if (g_opt.schmitt_trigger && (g_opt.schmitt_list == NULL ||
                list_find(g_opt.schmitt_list, v->name, fit_str_ieq_cb))) {
                fprintf(g_msg, "SSTL and Schmitt trigger conflict on input %s\n", v->name);
                list_destroy(&pads);
                return 0;
            }
            if (k == 0) {
                for (int mc = 0; mc < g_dev->n_macrocells; mc++) {
                    FitSlot *candidate = slot_at(mc);
                    if (place_preassign_index_of_pin(candidate->pin) == 0 &&
                        fit_drv_sstl_pad_ok(candidate)) {
                        slot = candidate;
                        break;
                    }
                }
            }
            if (!fit_drv_sstl_pad_ok(slot)) {
                fprintf(g_msg, "SSTL input %s needs an I/O pad in a 2.5 or 3.3 V bank "
                        "with an available VREF pin\n", v->name);
                list_destroy(&pads);
                return 0;
            }
            int bank = (slot->macrocell - 1) >= g_dev->n_macrocells / 2;
            FitSlot *ref = slot_at(fit_be_vref_macrocell(g_dev_desc->id, bank));
            ref->signal = bank ? "VREFB" : "VREFA";
            ref->reserved_pad = 1;
            slot->signal = util_strdup(v->name);
            slot->sstl_input = 1;
            if (k == 0 && entity_is_output(ent, v->id)) {
                /* The output half of a bidirectional pad must use the
                 * same macrocell selected for its SSTL input buffer. */
                FitPreassignItem *item = util_calloc(1, sizeof *item);
                item->name = util_strdup(v->name);
                item->pin = slot->pin;
                if (g_preassign == NULL)
                    g_preassign = list_create();
                list_add(g_preassign, item);
                g_fit_preassign_used = 1;
            }
        }
    }
    list_destroy(&pads);
    return 1;
}

/* gives signal v a transparent latch (constant enable) of its
 * own, so that it sits in a macrocell as a registered node */
static void fit_drv_add_latch(FitEntity *ent, FitVariable *v)
{
    FitNetItem *item;
    FitInstance *inst;
    FitVariable *nv;
    FitFct *fct;
    FitFctTree *one;

    if (ent->net == NULL)
        ent->net = netlist_new();
    if (ent->net->items == NULL) {
        ent->net->items = list_create();
        ent->net->aliases = list_create();
    }
    item = netitem_new();
    item->kind = FIT_GATE_LATCH;
    inst = instance_new();
    inst->inst_name = util_strdup(v->name);
    item->cell = inst;
    list_add(ent->net->items, item);

    nv = lib_new_variable(ent, 1, 1);
    fct = fct_new();
    fct->var_id = nv->id;
    one = fcttree_new();
    one->sel = FIT_SEL_CONST;
    one->u.value = 1;
    fct->tree[0] = one;
    list_add(ent->fcts, fct);
    inst->clk = util_strdup(nv->name);
    inst->clk_kind = 4;

    nv = lib_new_variable(ent, 1, 1);
    fct = entity_find_fct(ent, v->id);
    if (fct == NULL)        /* no function: stop */
        fit_internal_error("fit_drv_add_latch: Global_OE signal without function");
    fct->var_id = nv->id;
    inst->q = util_strdup(nv->name);
    if (entity_is_local(ent, v->id) == 0) {
        nv = lib_new_variable(ent, 0, 1);
        inst->d = util_strdup(nv->name);
        lib_alias_set_target(ent->net, nv->name, v->name);
        fct = fct_new();
        fct->var_id = v->id;
        fct->tree[0] = fcttree_new_id(nv->id);
        list_add(ent->fcts, fct);
        return;
    }
    inst->d = util_strdup(v->name);
    lib_move_variable(v->id, ent->locals, ent->inputs);
    lib_alias_add(ent->net, v->name, NULL);
}

/* the signals of g_opt.global_fold that are neither inputs nor outputs
 * on a pin that reaches a global-OE multiplexer get a latch */
static void fit_drv_place_global_fold(FitEntity *ent)
{
    int i;

    for (i = 0; i < LN(g_opt.global_fold); i++) {
        int id = FIT_PTR2INT(g_opt.global_fold->items[i]);
        FitVariable *v = entity_var_by_id(ent, id);
        int k;

        if (v == NULL)
            continue;
        if (list_find(ent->inputs, v->name, var_has_name) != 0)
            continue;
        if (list_find(ent->outputs, v->name, var_has_name) == 0) {
            fit_drv_add_latch(ent, v);
            continue;
        }
        k = list_find(g_preassign, v->name, place_name_eq);
        if (k != 0
            && dev_find_global_oe(place_slot_of_pin(preassign_at(k)->pin), 0, 1) == 0) {
            FitVariable *nv = lib_new_variable(ent, 2, 0);
            FitFct *old = entity_find_fct(ent, id);
            FitFct *fct = fct_new();

            if (old == NULL)    /* no function: stop */
                fit_internal_error("fit_drv_place_global_fold: Global_OE signal without function");
            fct->var_id = v->id;
            fct->tree[0] = fcttree_new_id(nv->id);
            v->id = nv->id;
            old->var_id = nv->id;
            nv->id = fct->var_id;
            list_add(ent->fcts, fct);
            fit_drv_add_latch(ent, nv);
        }
    }
}

/* Compares name with every {input id, node id} pair read as text: the
 * eight bytes of the two ids, little-endian, as a string (case ignored).
 * This is the comparison the fitter applies to the converted list; which
 * names count as fast-inlatch inputs depends on it. */
static int fast_inlatch_has_name(const char *name)
{
    int i, j;

    for (i = 0; i < LN(g_fast_inlatch); i++) {
        FitFastInlatch *fi = (FitFastInlatch *)g_fast_inlatch->items[i];
        unsigned a = (unsigned)fi->in_id;
        unsigned b = (unsigned)fi->node_id;
        char text[9];

        for (j = 0; j < 4; j++) {
            text[j] = (char)((a >> (8 * j)) & 0xffu);
            text[4 + j] = (char)((b >> (8 * j)) & 0xffu);
        }
        text[8] = '\0';
        if (util_str_ieq(text, name) != 0)
            return 1;
    }
    return 0;
}

static FitFastInlatch *new_fast_inlatch(int in_id, int node_id)
{
    FitFastInlatch *fi = (FitFastInlatch *)util_calloc(1, sizeof *fi);

    fi->in_id = in_id;
    fi->node_id = node_id;
    return fi;
}

static void add_node_preassign(FitEntity *ent, FitVariable *node, int mc)
{
    FitPreassignItem *item = (FitPreassignItem *)util_calloc(1, sizeof *item);

    item->pin = mc + 600;
    item->name = latch_signal_source(ent, node->name);
    list_add(g_preassign, item);
}

/* fast_inlatch without a list: every buried register fed
 * directly by an input pin */
static void fit_drv_find_fast_inlatch(FitEntity *ent)
{
    int i;

    g_fast_inlatch = list_create();
    g_fast_inlatch_pairs = 1;
    for (i = 0; i < LN(ent->net->items); i++) {
        FitNetItem *item = (FitNetItem *)ent->net->items->items[i];
        FitVariable *v, *in;
        FitFct *fct;
        int k;

        if (item->kind != 3 && item->kind != 2 && item->kind != 4)
            continue;
        v = entity_find_var(ent, ((FitInstance *)item->cell)->q);
        if (entity_var_is_node(ent, v) == 0)
            continue;
        fct = entity_find_fct(ent, v->id);
        if (fct->tree[0]->sel != FIT_SEL_ID)
            continue;
        in = entity_var_by_id(ent, fct->tree[0]->u.id);
        if (list_find(ent->inputs, FIT_INT2PTR(in->id), var_id_eq) == 0)
            continue;
        if (latch_find_alias(in->name, ent->net) != 0 || fast_inlatch_has_name(in->name))
            continue;
        k = list_find(g_preassign, in->name, place_name_eq);
        if (k != 0) {
            FitSlot *slot = place_slot_of_pin(preassign_at(k)->pin);
            int k2;

            if (slot == NULL)   /* no slot: stop */
                fit_internal_error("fit_drv_find_fast_inlatch: preassigned pin without slot");
            if (slot->macrocell == 0)
                continue;
            k2 = list_find(g_preassign, latch_signal_source(ent, v->name), place_name_eq);
            if (k2 == 0)
                add_node_preassign(ent, v, slot->macrocell);
            else if (slot->macrocell != preassign_at(k2)->pin - 600)
                continue;
        }
        list_add(g_fast_inlatch, new_fast_inlatch(in->id, v->id));
    }
}

/* the fast-inlatch entry for the named input, or NULL */
static FitFastInlatch *fit_drv_fast_inlatch_of(FitEntity *ent, FitVariable *in)
{
    int i;

    for (i = 0; i < LN(ent->net->items); i++) {
        FitNetItem *item = (FitNetItem *)ent->net->items->items[i];
        FitVariable *v;
        FitFct *fct;
        int k;

        if (item->kind != 3 && item->kind != 2 && item->kind != 4)
            continue;
        v = entity_find_var(ent, ((FitInstance *)item->cell)->q);
        if (entity_var_is_node(ent, v) == 0)
            continue;
        fct = entity_find_fct(ent, v->id);
        if (fct->tree[0]->sel != FIT_SEL_ID || fct->tree[0]->u.id != in->id)
            continue;
        k = list_find(g_preassign, in->name, place_name_eq);
        if (k != 0) {
            FitSlot *slot = place_slot_of_pin(preassign_at(k)->pin);
            int k2;

            if (slot == NULL)   /* no slot: stop */
                fit_internal_error("fit_drv_fast_inlatch_of: preassigned pin without slot");
            k2 = list_find(g_preassign, latch_signal_source(ent, v->name), place_name_eq);
            if (k2 == 0) {
                add_node_preassign(ent, v, slot->macrocell);
            } else if (slot->macrocell != preassign_at(k2)->pin - 600) {
                fprintf(g_fit_report, "Input %s can not be fast in latch\n", in->name);
                continue;
            }
        }
        return new_fast_inlatch(in->id, v->id);
    }
    return NULL;
}

/* fast_inlatch with a list of input names */
static void fit_drv_redo_fast_inlatch(FitEntity *ent)
{
    FitList *names = list_copy(g_fast_inlatch);
    int i;

    list_destroy(&g_fast_inlatch);
    g_fast_inlatch = list_create();
    g_fast_inlatch_pairs = 1;
    for (i = 0; i < LN(names); i++) {
        FitVariable *in = entity_find_var(ent, (char *)names->items[i]);
        FitFastInlatch *fi;

        if (in == NULL)     /* no input: stop */
            fit_internal_error("fit_drv_redo_fast_inlatch: fast_inlatch name not in the design");
        fi = fit_drv_fast_inlatch_of(ent, in);
        if (fi != NULL)
            list_add(g_fast_inlatch, fi);
    }
    list_destroy(&names);
}

/* puts the input `name` on a free pin of `pins` (kind 3 clock,
 * 4 clear, 6 output enable) */
static int fit_drv_take_global_pin(char *name, int kind, FitList *pins)
{
    FitSlot *slot;
    int k = 0;
    int i;

    if (fit_drv_sstl_name(name)) {
        for (i = 0; i < g_dev->n_macrocells; i++) {
            slot = slot_at(i);
            if (util_str_ieq(slot->signal, name)) {
                if (list_find(pins, FIT_INT2PTR(slot->pin), list_ptr_eq) == 0)
                    return 0;
                return kind != 6 || dev_assign_global_oe(slot) != 0;
            }
        }
    }
    if (g_fit_preassign_used != 0)
        k = list_find(g_preassign, name, place_name_eq);
    if (k == 0) {
        for (i = g_dev->n_ded_inputs - 1 + g_dev->n_macrocells; i >= 0; i--) {
            slot = slot_at(i);
            if (is_free_name(slot->signal)
                && list_find(pins, FIT_INT2PTR(slot->pin), list_ptr_eq) != 0
                && place_preassign_index_of_pin(slot->pin) == 0) {
                slot->signal = util_strdup(name);
                if (kind != 6)
                    return 1;
                if (dev_assign_global_oe(slot) != 0)
                    return 1;
                free_str(&slot->signal);
                slot->signal = util_strdup(FIT_FREE_NAME);
            }
        }
        return 0;
    }
    if (list_find(pins, FIT_INT2PTR(preassign_at(k)->pin), list_ptr_eq) != 0
        && (slot = place_slot_of_pin(preassign_at(k)->pin)) != NULL
        && is_free_name(slot->signal)) {
        slot->signal = util_strdup(name);
        if (kind != 6 || dev_assign_global_oe(slot) != 0)
            return 1;
        free_str(&slot->signal);
        slot->signal = util_strdup(FIT_FREE_NAME);
    }
    return 0;
}

/* global clock, clear and output-enable pins.  Two rounds over
 * the registers: first only the signals of g_fit_must_global, then all. */
static int fit_drv_assign_global_pins(FitEntity *ent)
{
    FitList *gclk, *gclr, *oe;
    int n_clk = 0, n_clr = 0, n_oe = 0;
    int must = 0;
    int i;

    g_fit_global_names = list_create();
    gclk = dev_gclk_pin_list();
    gclr = dev_gclr_pin_list();
    oe = dev_oe_pin_list();
    if (g_opt.twoclock != NULL && g_opt.twoclock->count != 0) {
        char *tc = (char *)g_opt.twoclock->items[0];

        if (list_find(ent->inputs, tc, var_has_name) != 0
            && fit_drv_take_global_pin(tc, 3, gclk) == 0)
            return 0;
        list_add(g_fit_global_names, tc);
        n_clk = 2;
    }
    if (g_fit_must_global != NULL)
        must = g_fit_must_global->count;
    for (;;) {
        for (i = 0; i < LN(ent->net->items); i++) {
            FitNetItem *item = (FitNetItem *)ent->net->items->items[i];
            FitInstance *inst;
            int kind = item->kind;

            if (kind != 3 && kind != 8 && kind != 4 && kind != 2)
                continue;
            inst = (FitInstance *)item->cell;

            if (list_find(ent->inputs, inst->clk, var_has_name) != 0
                && latch_find_alias(inst->clk, ent->net) == 0 && n_clk < 3
                && list_find(g_fit_global_names, inst->clk, fit_str_ieq_cb) == 0
                && (must == 0 || list_find(g_fit_must_global, inst->clk, fit_str_ieq_cb) != 0)) {
                if (fit_drv_take_global_pin(inst->clk, 3, gclk) == 0) {
                    if (must != 0) {
                        fprintf(g_fit_report, "%s must be placed on global pin\n", inst->clk);
                        g_exit_code = 0x12e;
                        return 0;
                    }
                } else {
                    list_add(g_fit_global_names, inst->clk);
                    n_clk++;
                }
            }

            if (list_find(ent->inputs, inst->ar, var_has_name) != 0
                && latch_find_alias(inst->ar, ent->net) == 0 && n_clr < 1
                && list_find(g_fit_global_names, inst->ar, fit_str_ieq_cb) == 0
                && (must == 0 || list_find(g_fit_must_global, inst->ar, fit_str_ieq_cb) != 0)
                && fit_drv_take_global_pin(inst->ar, 4, gclr) != 0) {
                list_add(g_fit_global_names, inst->ar);
                n_clr++;
            }

            if (list_find(ent->inputs, inst->oe, var_has_name) != 0
                && latch_find_alias(inst->oe, ent->net) == 0 && n_oe < 5
                && list_find(g_fit_global_names, inst->oe, fit_str_ieq_cb) == 0
                && (must == 0 || list_find(g_fit_must_global, inst->oe, fit_str_ieq_cb) != 0)) {
                if (fit_drv_take_global_pin(inst->oe, 6, oe) != 0) {
                    list_add(g_fit_global_names, inst->oe);
                    n_oe++;
                } else if (must != 0) {
                    fprintf(g_fit_report, "%s must be placed on global pin\n", inst->oe);
                    g_exit_code = 0x12e;
                    return 0;
                }
            }
        }
        if (must == 0)
            return 1;
        must = 0;
    }
}

/* ------------------------------------------------------------------ */
/* One pass                                                             */
/* ------------------------------------------------------------------ */

/* one attempt.  Returns 1 when the design is placed and routed;
 * otherwise *result says why (enum FitPassResult; -2 bad preassignment). */
int fit_drv_pass(FitEntity *ent, FitList *used, int *result)
{
    int n_mc = 0;
    int limit, b;

    g_fit_block_fanin = list_create();
    g_fit_excluded_ids = NULL;
    for (b = 0; b < g_dev->n_blocks; b++)
        list_add(g_fit_block_fanin, list_create());
    if (g_dev_package == NULL)
        g_dev_package = util_strdup("PLCC44");
    place_create_slots(g_dev_package);
    place_create_uim_slots();
    place_create_global_slots();
    if (fit_drv_check_preassign(ent) == 0 || fit_drv_place_sstl(ent) == 0) {
        g_exit_code = 0x12e;
        *result = -2;
        return 0;
    }
    fit_drv_place_global_fold(ent);
    if (g_opt.fast_inlatch != 0) {
        if (g_fast_inlatch == NULL)
            fit_drv_find_fast_inlatch(ent);
        else
            fit_drv_redo_fast_inlatch(ent);
    }
    if (fit_drv_assign_global_pins(ent) == 0) {
        *result = 2;
        g_exit_code = 0x1f8;
        return 0;
    }
    fit_prep_plan(ent, &used, &n_mc);
    limit = g_fit_route_limit;
    if (g_dev->n_macrocells < n_mc) {
        if (g_opt.logic_doubling != 0)
            fprintf(g_fit_report, "# Aborting : Design is large, it contains %d Logic Cell\n",
                    n_mc);
        *result = -1;
        g_exit_code = 0x192;
        return 0;
    }
    if (alloc_partition_simple(ent, n_mc) == 0) {
        g_fit_route_limit = limit;
        if (alloc_partition(ent, n_mc) == 0) {
            *result = 1;
            g_exit_code = 0x1f7;
            g_fit_place_failed = 1;
            return 0;
        }
    }
    if (fit_drv_place_blocks(ent) == 0) {
        *result = 2;
        g_exit_code = 0x1f8;
        return 0;
    }
    if (fit_drv_place_leftovers(ent) == 0) {
        *result = 2;
        g_exit_code = 0x1f8;
        return 0;
    }
    fit_drv_assign_inputs_and_route(ent, place_collect_block_signals());
    if (g_fit_route_failed != 0) {
        g_exit_code = 0x1f5;
        *result = 3;
    }
    place_prune_uim_slots();
    if (g_fit_route_failed == 0 && g_fit_place_failed == 0)
        return 1;
    return 0;
}
