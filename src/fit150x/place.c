/*
 * place.c - the placer of the ATF150x fitter: the placement slots
 * (macrocells and dedicated inputs), the search for free macrocells, node
 * names, and the assignment of block inputs to UIM multiplexers.
 *
 * The code is the same for the three devices and reads the device through
 * g_dev.
 */
#include "place_priv.h"

/* ------------------------------------------------------------------ */
/* Globals defined by the place modules                                */
/* ------------------------------------------------------------------ */

int      g_alloc_free_pins;
FitList *g_route_cands;
FitList *g_route_failed_ids;
FitList *g_route_names;
int      g_alloc_mc_per_block;
int      g_alloc_grouped;
FitList *g_place_seen_ids;
int      g_alloc_max_wide;
int      g_alloc_swap_depth;
FitList *g_alloc_preassigned_fcts;
int      g_route_progress = 1;

static char place_free_name[] = FIT_FREE_NAME;

void place_trap(const char *what)
{
    fflush(stdout);
    fprintf(stderr, "fit150x: internal error: %s\n", what);
    exit(3);
}

static void *copy_list_cb(void *list)
{
    return list_copy((FitList *)list);
}

static void destroy_list_cb(void **ref)
{
    FitList *l = (FitList *)*ref;

    list_destroy(&l);
    *ref = NULL;
}

FitFctTree *place_fct_tree(FitEntity *ent, int id)
{
    FitFct *fct;

    if (list_find(ent->locals, FIT_INT2PTR(id), var_id_eq) == 0)
        return fcttree_new_id(id);
    if (list_find(g_fit_excluded_ids, FIT_INT2PTR(id), list_ptr_eq) == 0)
        return fcttree_new_id(id);
    fct = entity_find_fct(ent, id);
    if (fct == NULL)
        place_trap("place_fct_tree: variable without function");
    return fct->tree[0];
}

int place_count_terms(FitEntity *ent, FitFct *fct)
{
    FitFctTree *tree = fct->tree[0];

    (void)ent;
    if (tree->sel != FIT_SEL_OR)
        return 1;
    if (tree->u.children == NULL)
        return 0;
    return tree->u.children->count;
}

/* Product terms a macrocell can still offer: 0 when it holds a
 * function, lends its terms (cascade) or has a feedback signal; 4 when it has a
 * foldback, else 5. */
int place_slot_free_terms(FitSlot *slot)
{
    int n = 5;

    if (slot->fct_id == 0
        && util_str_icmp(slot->cascade, FIT_FREE_NAME) == 0
        && util_str_icmp(slot->buried, FIT_FREE_NAME) == 0) {
        if (util_str_icmp(slot->foldback, FIT_FREE_NAME) != 0)
            n = 4;
        return n;
    }
    return 0;
}

/* 1 when the signal preassigned to `pin` is an input of ent. */
static int place_preassigned_pin_is_input(FitEntity *ent, int pin)
{
    FitPreassignItem *item;
    int i, id;

    if (pin == 0)
        return 0;
    for (i = 0; ; i++) {
        if (i >= PLACE_COUNT(g_preassign))
            return 0;
        item = (FitPreassignItem *)g_preassign->items[i];
        if (item->pin == pin)
            break;
    }
    id = entity_var_id_by_name(ent, item->name);
    if (id != 0)
        return entity_has_input_id(ent, id) != 0;
    return 0;
}

int place_preassign_index_of_pin(int pin)
{
    int i;

    if (pin != 0) {
        for (i = 0; i < PLACE_COUNT(g_preassign); i++) {
            if (((FitPreassignItem *)g_preassign->items[i])->pin == pin)
                return i + 1;
        }
    }
    return 0;
}

FitFct *place_fct_of_var(FitEntity *ent, FitVariable *var)
{
    char *name = var->name;
    FitInstance *inst;
    int kind, id;

    if (netlist_has_name(ent, name) != 0) {
        inst = tt3_find_instance_by_alias(ent, name, &kind);
        if (inst == NULL)
            place_trap("place_fct_of_var: no instance for the alias");
        id = entity_var_id_by_name(ent, inst->q);
        return entity_find_fct(ent, id);
    }
    if (latch_find_alias(name, ent->net) != 0) {
        inst = tt3_find_instance_by_input(ent, name, &kind);
        if (inst != NULL) {
            id = entity_var_id_by_name(ent, inst->q);
            return entity_find_fct(ent, id);
        }
    }
    return entity_find_fct(ent, var->id);
}

/* 1 when some signal is preassigned to the feedback node of
 * macrocell mc (pin number 600 + mc). */
static int place_feedback_is_preassigned(int mc)
{
    int i;

    if (mc != 0) {
        for (i = 0; i < PLACE_COUNT(g_preassign); i++) {
            if (((FitPreassignItem *)g_preassign->items[i])->pin == mc + 600)
                return 1;
        }
    }
    return 0;
}

/* 1 when the pin of the macrocell does not stand in the way of
 * burying a function in it: no pin, or a pin that is neither free nor
 * owned by logic, or a pin whose preassigned signal is an input. */
static int place_slot_pin_usable(FitEntity *ent, FitSlot *slot)
{
    if (slot->pin == 0)
        return 1;
    if (util_str_icmp(slot->signal, FIT_FREE_NAME) == 0 || slot->fct_id != 0)
        return place_preassigned_pin_is_input(ent, slot->pin) != 0;
    return 1;
}

/* 0 when the output preassigned to the pin of slot needs so many
 * product terms that n_pt more do not fit into the macrocell. */
static int place_slot_fits_with_pin_owner(FitEntity *ent, FitSlot *slot, int n_pt)
{
    FitPreassignItem *item;
    FitVariable *var;
    FitFct *fct;
    FitList *tmp;
    int k, n;

    k = place_preassign_index_of_pin(slot->pin);
    if (k != 0) {
        item = (FitPreassignItem *)g_preassign->items[k - 1];
        k = list_find(ent->outputs, item->name, var_has_name);
        if (k == 0)
            return 0;
        fct = place_fct_of_var(ent, (FitVariable *)ent->outputs->items[k - 1]);
        if (fct == NULL)
            place_trap("place_slot_fits_with_pin_owner: variable without function");
        var = entity_var_by_id(ent, fct->var_id);
        tmp = list_create();
        n = fit_drv_count_mc_terms(ent, fct, var, tmp);
        list_destroy(&tmp);
        if (5 < n + n_pt)
            return 0;
    }
    return 1;
}

FitList *place_find_slots(FitEntity *ent, int block, int n_pt, int mode, int mc, int var_id)
{
    FitList *result = list_create();
    int lo = block * 16 - 16;
    int hi = block * 16 - 1;
    int i, j, avail, more;
    FitSlot *slot, *next;

    for (i = hi; i >= lo; i--) {
        if (mc != 0 && i != mc - 1)
            continue;
        slot = PLACE_SLOT(i);
        if (slot->block != block)
            continue;

        if (mode == 1) {
            FitVariable *owner = slot->sstl_input ? entity_var_by_id(ent, var_id) : NULL;
            int own_sstl_pad = owner != NULL && util_str_ieq(slot->signal, owner->name);
            if (slot->pin == 0 ||
                (util_str_icmp(slot->signal, FIT_FREE_NAME) != 0 && !own_sstl_pad))
                continue;
            goto check_preassign;
        } else if (mode == 5) {
            if (n_pt < 6) {
                if (!place_slot_pin_usable(ent, slot))
                    continue;
                if (mc == 0 && place_feedback_is_preassigned(slot->macrocell) != 0)
                    continue;
            } else {
                goto check_preassign;
            }
        } else if (mode == 0x18) {
            if (slot->pin == 0
                || (mc == 0 && place_feedback_is_preassigned(slot->macrocell) != 0)
                || util_str_icmp(slot->buried, FIT_FREE_NAME) != 0
                || place_slot_fits_with_pin_owner(ent, slot, n_pt) == 0)
                continue;
        } else {
check_preassign:
            if (g_fit_preassign_used != 0 && mc == 0) {
                if (place_preassign_index_of_pin(slot->pin) != 0)
                    continue;
                if (place_feedback_is_preassigned(slot->macrocell) != 0)
                    continue;
            }
        }

        avail = place_slot_free_terms(slot);
        if (list_find(g_opt.global_oe, FIT_INT2PTR(var_id), list_ptr_eq) != 0
            && dev_find_global_oe(slot, 0, mode) == 0)
            continue;
        if (avail == 0)
            continue;

        list_add(result, slot);
        if (n_pt <= avail)
            return result;
        j = i - 1;
        if (j >= lo) {
            for (;;) {
                next = PLACE_SLOT(j);
                more = place_slot_free_terms(next);
                if (more == 0 || next->macrocell % 8 == 0) {
                    list_destroy(&result);
                    result = list_create();
                    break;
                }
                list_add(result, next);
                avail += more;
                if (n_pt <= avail)
                    return result;
                j--;
                if (j < lo)
                    break;
            }
            if (j > lo)
                continue;
        }
        /* the chain ran into the lower end of the block */
        if (result == NULL || result->count == 0)
            return result;
        list_destroy(&result);
        return list_create();
    }
    return result;
}

/* Package pin of a dedicated input: kind 8 / 4 / 0xb take the
 * first, 9 / 0xc the second, anything else the third pin of their kind. */
static int place_dedicated_pin(int kind)
{
    if (kind == 8 || kind == 4 || kind == 0xb)
        return dev_special_pin(kind, 0);
    if (kind == 9 || kind == 0xc)
        return dev_special_pin(kind, 1);
    return dev_special_pin(kind, 2);
}

void place_create_slots(char *package)
{
    FitSlot *slot;
    FitList *pins;
    int i, b, kind;

    g_fit_slots = list_create();
    for (i = 0; i < g_dev->n_macrocells; ) {
        slot = (FitSlot *)util_calloc(1, sizeof *slot);
        for (b = 0; b < g_dev->n_blocks; b++) {
            if (i < (b + 1) * 16) {
                slot->block = b + 1;
                break;
            }
        }
        slot->pin = dev_pin_of_macrocell(package, i);
        i++;
        slot->macrocell = i;
        slot->cascade = util_strdup(FIT_FREE_NAME);
        slot->signal = place_free_name;
        slot->fn_kind = place_free_name;
        slot->buried = place_free_name;
        slot->foldback = place_free_name;
        slot->polarity = 1;
        list_add(g_fit_slots, slot);
    }
    for (i = 0; i < g_dev->n_ded_inputs; i++) {
        slot = (FitSlot *)util_calloc(1, sizeof *slot);
        slot->block = 0;
        slot->macrocell = 0;
        slot->cascade = util_strdup(FIT_FREE_NAME);
        slot->signal = place_free_name;
        slot->fn_kind = place_free_name;
        slot->buried = place_free_name;
        slot->foldback = place_free_name;
        if (i == 0)
            kind = 9;
        else if (i == 1)
            kind = 8;
        else if (i == 2)
            kind = 4;
        else
            kind = 0xb;
        slot->pin = place_dedicated_pin(kind);
        list_add(g_fit_slots, slot);
    }
    if (g_opt.jtag != 0) {
        pins = dev_jtag_pin_list();
        report_reserve_jtag_slots(g_fit_slots, pins);
    }
    if (g_opt.pd1 != 0 || g_opt.pd2 != 0) {
        pins = dev_pd_pin_list();
        report_reserve_pd_slots(g_fit_slots, pins);
    }
}

/* recursive part of place_collect_inputs */
static void place_collect_inputs_rec(FitEntity *ent, FitFctTree *tree, FitList *ids)
{
    int i, n, id;
    FitList *children;

    for (;;) {
        if (tree->sel != FIT_SEL_ID) {
            if (tree->sel != FIT_SEL_CONST) {
                children = tree->u.children;
                n = children->count;
                for (i = 0; i < n; i++)
                    place_collect_inputs_rec(ent, (FitFctTree *)children->items[i], ids);
            }
            return;
        }
        id = tree->u.id;
        if (list_contains_ptr(ids, FIT_INT2PTR(id)) != 0)
            return;
        if (list_contains_ptr(g_fit_foldback_ids, FIT_INT2PTR(id)) == 0) {
            list_add(ids, FIT_INT2PTR(id));
            return;
        }
        if (list_contains_ptr(g_place_seen_ids, FIT_INT2PTR(id)) != 0)
            return;
        list_add(g_place_seen_ids, FIT_INT2PTR(id));
        tree = entity_fct_tree(ent, id);
        if (tree == NULL)
            place_trap("place_collect_inputs_rec: function without a tree");
    }
}

void place_collect_inputs(FitEntity *ent, FitFctTree *tree, FitList *ids)
{
    g_place_seen_ids = list_create();
    place_collect_inputs_rec(ent, tree, ids);
    list_destroy(&g_place_seen_ids);
}

int place_xor_terms(FitEntity *ent, FitList *ids, FitFct *fct)
{
    FitFctTree *tree = fct->tree[0];
    FitFctTree *first, *t1, *t2;
    FitList *l;
    int id1, id2;

    if (tree->sel != FIT_SEL_OR)
        return 0;
    l = tree->u.children;
    if (l == NULL || l->count != 2)
        return 0;
    first = (FitFctTree *)l->items[0];
    if (first->sel != FIT_SEL_AND)
        return 0;
    l = first->u.children;
    if (l == NULL || l->count != 2)
        return 0;
    id1 = jed_tree_var_id((FitFctTree *)l->items[0]);
    id2 = jed_tree_var_id((FitFctTree *)first->u.children->items[1]);
    if (id1 == 0 || id2 == 0)
        return 0;
    t1 = place_fct_tree(ent, id1);
    t2 = place_fct_tree(ent, id2);
    place_collect_inputs(ent, t1, ids);
    place_collect_inputs(ent, t2, ids);
    if (t2->sel == FIT_SEL_OR) {
        if (t2->u.children == NULL)
            return 1;
        return t2->u.children->count + 1;
    }
    if (t1->sel != FIT_SEL_OR)
        return 2;
    if (t1->u.children == NULL)
        return 1;
    return t1->u.children->count + 1;
}

FitSlot *place_slot_of_pin(int pin)
{
    int i, k, n;

    if (pin >= 300) {
        k = pin < 600 ? pin - 300 : pin - 600;
        if (k != 0) {
            if (k > PLACE_COUNT(g_fit_slots))
                return NULL;      /* no such macrocell */
            return PLACE_SLOT(k - 1);
        }
    }
    n = g_dev->n_ded_inputs + g_dev->n_macrocells;
    for (i = 0; i < n; i++) {
        if (PLACE_SLOT(i)->pin == pin)
            return PLACE_SLOT(i);
    }
    return NULL;
}

/* Block (1-based) of a pin or node number; 0 when it is no
 * macrocell pin. */
int place_block_of_pin(int pin)
{
    int i, k;

    if (pin >= 300) {
        k = pin < 600 ? pin - 300 : pin - 600;
        if (k != 0) {
            if (k > PLACE_COUNT(g_fit_slots))
                return 0;      /* no such macrocell */
            return PLACE_SLOT(k - 1)->block;
        }
    }
    for (i = 0; i < g_dev->n_macrocells; i++) {
        if (PLACE_SLOT(i)->pin == pin)
            return PLACE_SLOT(i)->block;
    }
    return 0;
}

int place_macrocell_of_pin(int pin)
{
    int i, k;

    if (pin >= 300) {
        k = pin < 600 ? pin - 300 : pin - 600;
        if (k != 0) {
            if (k > PLACE_COUNT(g_fit_slots))
                return 0;      /* no such macrocell */
            return PLACE_SLOT(k - 1)->macrocell;
        }
    }
    for (i = 0; i < g_dev->n_macrocells; i++) {
        if (PLACE_SLOT(i)->pin == pin)
            return PLACE_SLOT(i)->macrocell;
    }
    return 0;
}

const char *place_block_letter(FitSlot *slot)
{
    switch (slot->block) {
    case 1: return "A";
    case 2: return "B";
    case 3: return "C";
    case 4: return "D";
    case 5: return "E";
    case 6: return "F";
    case 7: return "G";
    default: return "H";
    }
}

/* New string "<letter><macrocell><suffix>" for a macrocell slot. */
char *place_node_name(FitSlot *slot, const char *suffix)
{
    char *name = (char *)util_calloc(1, PLACE_NODE_NAME_SIZE);

    snprintf(name, PLACE_NODE_NAME_SIZE, "%s%d%s", place_block_letter(slot), slot->macrocell, suffix);
    return name;
}

static int place_ids_have_name(FitEntity *ent, FitList *ids, const char *name)
{
    return list_find(ids, FIT_INT2PTR(entity_var_id_by_name(ent, name)), list_ptr_eq) != 0;
}

/* Node names of the macrocell signals whose ids are in `ids`
 * (the ids one block needs), in macrocell order: the feedback node of a
 * buried signal, the pin of a combinatorial output, and for a registered
 * output without buried signal the nodes of the register's inputs. */
static FitList *place_node_names_for_ids(FitEntity *ent, FitList *ids)
{
    FitList *names = list_create();
    FitSlot *slot;
    FitInstance *inst;
    const char *name;
    int i, kind;

    for (i = 0; i < g_dev->n_macrocells; i++) {
        slot = PLACE_SLOT(i);
        if (util_str_icmp(slot->buried, FIT_FREE_NAME) != 0
            && place_ids_have_name(ent, ids, latch_signal_source(ent, slot->buried))) {
            list_add(names, place_node_name(slot, "fb"));
            if (slot->fct_id == 0)
                continue;
        }
        if (slot->pin == 0
            || util_str_icmp(slot->signal, FIT_FREE_NAME) == 0
            || slot->fct_id == 0)
            continue;
        if (util_str_icmp(slot->fn_kind, "com") == 0
            || util_str_icmp(slot->buried, FIT_FREE_NAME) != 0) {
            inst = tt3_find_instance_by_alias(ent, slot->signal, &kind);
            if (inst == NULL) {
                name = slot->signal;
            } else {
                name = inst->d;
                if (name == NULL)
                    name = inst->pin;
            }
            if (place_ids_have_name(ent, ids, name))
                list_add(names, place_node_name(slot, "p"));
        } else {
            inst = tt3_find_instance_by_alias(ent, slot->signal, &kind);
            if (inst == NULL)
                place_trap("place_node_names_for_ids: no instance for the alias");
            if (inst->d2 != NULL && place_ids_have_name(ent, ids, inst->d2))
                list_add(names, place_node_name(slot, inst->pin != NULL ? "fb" : "p"));
            if (inst->d != NULL && place_ids_have_name(ent, ids, inst->d))
                list_add(names, place_node_name(slot, inst->pin != NULL ? "fb" : "p"));
            if (inst->pin != NULL && place_ids_have_name(ent, ids, inst->pin))
                list_add(names, place_node_name(slot, "p"));
        }
    }
    return names;
}

FitList *place_block_node_names(FitEntity *ent)
{
    FitList *res = list_create();
    int b;

    for (b = 0; b < g_dev->n_blocks; b++)
        list_add(res, place_node_names_for_ids(ent, PLACE_LIST(g_fit_block_fanin, b)));
    return res;
}

/* The unused block inputs of block (0-based) whose multiplexer
 * can select `node`. */
FitList *place_uim_free_candidates(const char *node, int block)
{
    FitList *res = list_create();
    FitList *l = PLACE_LIST(g_fit_uim_slots, block);
    FitUimSlot *u;
    int j, k;

    for (j = 0; j < PLACE_COUNT(l); j++) {
        u = (FitUimSlot *)l->items[j];
        if (util_str_icmp(u->signal, FIT_FREE_NAME) != 0)
            continue;
        for (k = 0; k < g_dev->uim_width; k++) {
            if (dev_uim_selects(u, node, k)) {
                list_add(res, u);
                break;
            }
        }
    }
    return res;
}

/* For every node name of `names` its list of free candidates. */
static FitList *place_uim_candidate_lists(FitList *names, int block)
{
    FitList *res = list_create();
    FitList *c;
    char *node;
    int i, j;

    for (i = 0; i < PLACE_COUNT(names); i++) {
        node = (char *)names->items[i];
        c = place_uim_free_candidates(node, block);
        list_add(res, c);
        if (g_equiv_verbose != 0) {
            printf("Pin : %s ===>", node);
            for (j = 0; j < PLACE_COUNT(c); j++)
                printf(" M%d", ((FitUimSlot *)c->items[j])->index);
            printf("\n");
        }
    }
    return res;
}

/* FitListEq: two FitUimSlot have the same index. */
int place_uim_index_eq(void *a, void *b)
{
    return ((FitUimSlot *)a)->index == ((FitUimSlot *)b)->index;
}

/* Rebuilds `chosen` as the block inputs that are the only
 * candidate of a name, marking each (i18 = 6, name_pos = 1-based position of
 * the name).  Returns 1 when there is any single-candidate name. */
static int place_uim_take_singles(FitList *chosen, FitList *cands)
{
    FitList *l;
    FitUimSlot *u;
    int i, any = 0;

    while (chosen != NULL && chosen->count != 0)
        list_del_shift(chosen, 1);
    for (i = 0; i < PLACE_COUNT(cands); i++) {
        l = PLACE_LIST(cands, i);
        if (l != NULL && l->count == 1) {
            any = 1;
            u = (FitUimSlot *)l->items[0];
            if (list_find(chosen, u, place_uim_index_eq) == 0) {
                u->i18 = 6;
                u->name_pos = i + 1;
                list_add(chosen, u);
            }
        }
    }
    return any;
}

/* Removes the chosen block inputs from the candidate lists that
 * still have more than one entry.  Returns 1 when something was removed.
 * After a removal the scan skips the entry that moved into its place. */
static int place_uim_drop_taken(FitList *chosen, FitList *cands)
{
    FitList *l;
    int i, j, any = 0;

    for (i = 0; i < PLACE_COUNT(cands); i++) {
        l = PLACE_LIST(cands, i);
        if (l != NULL && 1 < l->count) {
            for (j = 0; j < l->count; j++) {
                if (list_find(chosen, l->items[j], place_uim_index_eq) != 0) {
                    any = 1;
                    list_del_shift(l, j + 1);
                }
            }
        }
    }
    return any;
}

/* Tries block input u for the name at position pos (1-based) on
 * a copy of the candidate lists and returns the number of names that would
 * end up sharing a block input (0 = no conflict). */
static int place_uim_trial(FitList *cands, FitUimSlot *u, int pos)
{
    FitList *chosen, *seen, *l;
    void *x;
    int i, conflicts = 0;

    cands = list_map_copy(cands, copy_list_cb);
    chosen = list_create();
    l = PLACE_LIST(cands, pos - 1);
    while (l != NULL && l->count != 0)
        list_del_shift(l, 1);
    if (l == NULL)
        place_trap("place_uim_trial: candidate list missing");
    list_add(l, u);
    u->i18 = 6;
    list_add(chosen, u);
    if (place_uim_drop_taken(chosen, cands) == 0) {
        list_destroy(&chosen);
        list_free_all(&cands, destroy_list_cb);
        return 0;
    }
    for (i = 0; i < PLACE_COUNT(cands); i++) {
        l = PLACE_LIST(cands, i);
        if (i != pos - 1 && l != NULL && l->count == 1) {
            x = l->items[0];
            if (list_find(chosen, x, place_uim_index_eq) != 0)
                conflicts++;
            else
                list_add(chosen, x);
        }
    }
    if (conflicts != 0) {
        list_destroy(&chosen);
        list_free_all(&cands, destroy_list_cb);
        return conflicts;
    }
    do {
        place_uim_take_singles(chosen, cands);
    } while (place_uim_drop_taken(chosen, cands) != 0);
    seen = list_create();
    list_add(seen, u);
    for (i = 0; i < PLACE_COUNT(cands); i++) {
        l = PLACE_LIST(cands, i);
        if (i != pos - 1 && l != NULL && l->count == 1) {
            x = l->items[0];
            if (list_find(seen, x, place_uim_index_eq) != 0)
                conflicts++;
            else
                list_add(seen, x);
        }
    }
    list_destroy(&chosen);
    list_destroy(&seen);
    list_free_all(&cands, destroy_list_cb);
    return conflicts;
}

/* Number of candidate lists with more than one entry that
 * contain u; *pos receives the 1-based position of the last of them. */
static int place_uim_count_lists(FitList *cands, FitUimSlot *u, int *pos)
{
    FitList *l;
    int i, n = 0;

    *pos = 0;
    for (i = 0; i < PLACE_COUNT(cands); i++) {
        l = PLACE_LIST(cands, i);
        if (l != NULL && 1 < l->count) {
            if (list_find(l, u, place_uim_index_eq) != 0) {
                n++;
                *pos = i + 1;
            }
        }
    }
    return n;
}

/* First block input of `all` that is not chosen yet and that
 * exactly one undecided name can use. */
static FitUimSlot *place_uim_find_unique(FitList *cands, FitList *all, FitList *chosen, int *pos)
{
    FitUimSlot *u;
    int i;

    for (i = 0; i < PLACE_COUNT(all); i++) {
        u = (FitUimSlot *)all->items[i];
        if (list_find(chosen, u, place_uim_index_eq) == 0) {
            if (place_uim_count_lists(cands, u, pos) == 1)
                return u;
        }
    }
    return NULL;
}

/* Decides one more name: a block input only that name can use,
 * else, among the names with the fewest candidates (2 up to the device
 * parameter uim_max_cands), the candidate whose trial gives the fewest conflicts.
 * Returns 0 when nothing is left to decide. */
static int place_uim_pick(FitList *all, FitList *chosen, FitList *cands)
{
    FitUimSlot *u;
    FitList *l;
    int want = 2;
    int pos, i, j, best, r;

    u = place_uim_find_unique(cands, all, chosen, &pos);
    if (u != NULL) {
        l = PLACE_LIST(cands, pos - 1);
        while (l != NULL && l->count != 0)
            list_del_shift(l, 1);
        u->i18 = 6;
        /* name_pos is set by place_uim_take_singles before anything reads it. */
        u->name_pos = pos;
        list_add(l, u);
        list_add(chosen, u);
        return 1;
    }
    for (;;) {
        for (i = 0; i < PLACE_COUNT(cands); i++) {
            l = PLACE_LIST(cands, i);
            if (PLACE_COUNT(l) != want)
                continue;
            best = 1000;
            pos = 1000;
            for (j = 0; j < PLACE_COUNT(l); j++) {
                u = (FitUimSlot *)l->items[j];
                r = place_uim_trial(cands, u, i + 1);
                if (r < best) {
                    pos = j + 1;
                    best = r;
                    if (r == 0)
                        break;
                }
                u->i18 = 0;
            }
            if (pos != 1000) {
                u = (FitUimSlot *)l->items[pos - 1];
                while (l != NULL && l->count != 0)
                    list_del_shift(l, 1);
                u->i18 = 6;
                u->name_pos = i + 1;
                list_add(l, u);
                list_add(chosen, u);
                return 1;
            }
        }
        if (want == g_dev->uim_max_cands)
            return 0;
        want++;
    }
}

/* Union (by index) of all candidate lists. */
static FitList *place_uim_union(FitList *cands)
{
    FitList *all, *next;
    int i;

    /* no candidate lists: NULL */
    all = PLACE_COUNT(cands) > 0 ? list_copy(PLACE_LIST(cands, 0)) : NULL;
    for (i = 1; i < PLACE_COUNT(cands); i++) {
        next = list_union(all, PLACE_LIST(cands, i), place_uim_index_eq);
        list_destroy(&all);
        all = next;
    }
    return all;
}

/* UIM assignment of one block (0-based). */
static FitList *place_assign_uim_block(FitList *names, int block)
{
    FitList *chosen = list_create();
    FitList *cands = place_uim_candidate_lists(names, block);
    FitList *all = place_uim_union(cands);

    do {
        place_uim_take_singles(chosen, cands);
    } while (place_uim_drop_taken(chosen, cands) != 0);
    while (place_uim_pick(all, chosen, cands) != 0) {
        do {
            place_uim_take_singles(chosen, cands);
        } while (place_uim_drop_taken(chosen, cands) != 0);
    }
    list_destroy(&chosen);
    list_destroy(&all);
    return cands;
}

FitList *place_assign_uim(FitList *names)
{
    FitList *res = list_create();
    int b;

    for (b = 0; b < g_dev->n_blocks; b++)
        list_add(res, place_assign_uim_block(PLACE_LIST(names, b), b));
    return res;
}

FitSlot *place_slot_of_node(const char *node, char **signal)
{
    FitSlot *slot;
    char digits[4];
    size_t len, i;
    int mc;

    if (node == NULL)
        return NULL;
    if (util_str_icmp(node, "OE2") == 0) {
        slot = PLACE_SLOT(g_dev->n_macrocells);
    } else if (util_str_icmp(node, "GCLK") == 0) {
        slot = PLACE_SLOT(g_dev->n_macrocells + 1);
    } else if (util_str_icmp(node, "GCLR") == 0) {
        slot = PLACE_SLOT(g_dev->n_macrocells + 2);
    } else if (util_str_icmp(node, "OE1") == 0) {
        slot = PLACE_SLOT(g_dev->n_macrocells + 3);
    } else {
        /* "<letter><number>p" or "<letter><number>fb" */
        len = strlen(node);
        memset(digits, 0, sizeof digits);
        for (i = 1; i < len; i++) {
            if (node[i] == 'f' || node[i] == 'p')
                break;
            if (i - 1 < sizeof digits - 1)
                digits[i - 1] = node[i];
        }
        mc = atoi(digits);
        if (mc < 1 || mc > g_dev->n_macrocells + g_dev->n_ded_inputs)
            place_trap("place_slot_of_node: macrocell number out of range");
        slot = PLACE_SLOT(mc - 1);
        if (i < len && node[i] == 'f'
            && util_str_icmp(slot->buried, FIT_FREE_NAME) != 0) {
            *signal = util_strdup(slot->buried);
            return slot;
        }
        *signal = util_strdup(slot->signal);
        return slot;
    }
    *signal = util_strdup(slot->signal);
    return slot;
}

void place_create_uim_slots(void)
{
    FitList *l;
    FitUimSlot *u;
    int b, j;

    g_fit_uim_slots = list_create();
    for (b = 0; b < g_dev->n_blocks; b++) {
        l = list_create();
        for (j = 0; j < FIT_UIM_PER_BLOCK; j++) {
            u = (FitUimSlot *)util_calloc(1, sizeof *u);
            u->block = b;
            u->index = j;
            u->signal = util_strdup(FIT_FREE_NAME);
            u->sel_node = util_strdup(FIT_FREE_NAME);
            list_add(l, u);
        }
        list_add(g_fit_uim_slots, l);
    }
}

void place_create_global_slots(void)
{
    FitUimSlot *u;
    int j;

    g_jed_global_slots = list_create();
    for (j = FIT_GLOBAL_SLOTS; j != 0; j--) {
        u = (FitUimSlot *)util_calloc(1, sizeof *u);
        u->signal = util_strdup(FIT_FREE_NAME);
        u->sel_node = util_strdup(FIT_FREE_NAME);
        list_add(g_jed_global_slots, u);
    }
}

/* Number of blocks that need variable id. */
static int place_count_blocks_needing(int id)
{
    int b, n = 0;

    for (b = 0; b < g_dev->n_blocks; b++) {
        if (list_find(PLACE_LIST(g_fit_block_fanin, b), FIT_INT2PTR(id), list_ptr_eq) != 0)
            n++;
    }
    return n;
}

FitList *place_collect_block_signals(void)
{
    FitList *res = list_create();
    FitList *l, *t;
    void *id;
    int b, j, pos, n;

    for (b = 0; b < g_dev->n_blocks; b++)
        list_add(res, list_create());
    for (b = 0; b < g_dev->n_blocks; b++) {
        j = 0;
        pos = 1;
        l = PLACE_LIST(g_fit_block_fanin, b);
        while (j < PLACE_COUNT(l)) {
            id = l->items[j];
            if (list_find(g_opt.global_oe, id, list_ptr_eq) == 0) {
                n = place_count_blocks_needing(FIT_PTR2INT(id));
                t = PLACE_LIST(res, n - 1);
                if (list_find(t, id, list_ptr_eq) == 0)
                    list_add(t, id);
                j++;
                pos++;
            } else {
                list_del_shift(l, pos);
            }
        }
    }
    return res;
}

int place_count_xor_uses(FitEntity *ent, int id)
{
    FitFct *fct;
    int i, n = 0;

    for (i = 0; i < PLACE_COUNT(g_opt.xor_list); i++) {
        fct = entity_find_fct(ent, PLACE_ID(g_opt.xor_list, i));
        if (fct == NULL)
            place_trap("place_count_xor_uses: variable without function");
        n += (int)fcttree_count_id(fct->tree[0], id);
    }
    return n;
}

FitList *place_xor_local_ids(FitEntity *ent)
{
    FitList *res = list_create();
    FitFct *fct;
    int j, pos, id, n;

    j = 0;
    pos = 1;
    while (j < PLACE_COUNT(g_opt.xor_list)) {
        fct = entity_find_fct(ent, PLACE_ID(g_opt.xor_list, j));
        if (fct == NULL) {
            list_del_shift(g_opt.xor_list, pos);
        } else {
            fcttree_collect_ids(fct->tree[0], res);
            j++;
            pos++;
        }
    }
    j = 0;
    pos = 1;
    while (j < PLACE_COUNT(res)) {
        id = PLACE_ID(res, j);
        if (list_find(ent->locals, FIT_INT2PTR(id), var_id_eq) == 0
            || ((n = tt2_count_id_uses(ent, id)) > 2
                && place_count_xor_uses(ent, id) < n)) {
            list_del_shift(res, pos);
            j--;
            pos--;
        }
        j++;
        pos++;
    }
    return res;
}

void place_prune_uim_slots(void)
{
    FitList *l;
    int b, bpos, j, pos;

    b = 0;
    bpos = 1;
    while (b < PLACE_COUNT(g_fit_uim_slots)) {
        l = PLACE_LIST(g_fit_uim_slots, b);
        j = 0;
        pos = 1;
        while (j < PLACE_COUNT(l)) {
            if (util_str_icmp(((FitUimSlot *)l->items[j])->signal, FIT_FREE_NAME) == 0) {
                list_del_shift(l, pos);
                j--;
                pos--;
            }
            j++;
            pos++;
        }
        if (l == NULL || l->count == 0) {
            list_del_shift(g_fit_uim_slots, bpos);
            b--;
            bpos--;
        }
        b++;
        bpos++;
    }
}

FitSlot *place_node_of_signal(const char *signal, char **node)
{
    FitSlot *slot = NULL;
    int i;

    for (i = g_dev->n_macrocells + 3; i >= 0; i--) {
        slot = PLACE_SLOT(i);
        if (util_str_icmp(slot->signal, signal) == 0)
            break;
    }
    if (i < 0)
        return NULL;
    if (i == g_dev->n_macrocells)
        *node = util_strdup("OE2");
    else if (i == g_dev->n_macrocells + 1)
        *node = util_strdup("GCLK");
    else if (i == g_dev->n_macrocells + 2)
        *node = util_strdup("GCLR");
    else if (i == g_dev->n_macrocells + 3)
        *node = util_strdup("OE1");
    else
        *node = place_node_name(slot, "p");
    return slot;
}

int place_name_eq(void *item, void *name)
{
    return util_str_icmp(*(char **)item, (const char *)name) == 0;
}
