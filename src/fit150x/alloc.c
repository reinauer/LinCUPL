/*
 * alloc.c - the block partition of the ATF150x fitter: the list of
 * functions to place, their partition into logic blocks under the fan-in,
 * macrocell, foldback and pin limits, and the balancing of the UIM
 * clusters when routing fails.
 */
#include "place_priv.h"

/* Trace on stderr, compiled in with -DPLACE_TRACE only. */
#ifdef PLACE_TRACE
#define TRACE(...) fprintf(stderr, __VA_ARGS__)
#else
#define TRACE(...) ((void)0)
#endif

/* Largest number of node names a UIM cluster should hold: 20 on the
 * ATF1508, 10 on the ATF1504 and ATF1502. */
static int alloc_cluster_limit(void)
{
    return g_dev->n_macrocells == 128 ? 20 : 10;
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

/* Replaces the list in slot i of `lists` by an empty one. */
static void alloc_reset_list(FitList *lists, int i)
{
    FitList *l = PLACE_LIST(lists, i);

    list_destroy(&l);
    lists->items[i] = list_create();
}

/* fit_drv_count_mc_terms for fct with a throw-away input list; *fanin
 * receives the number of inputs when not NULL. */
static int alloc_fct_terms(FitEntity *ent, FitFct *fct, FitVariable *var, int *fanin)
{
    FitList *tmp = list_create();
    int n = fit_drv_count_mc_terms(ent, fct, var, tmp);

    if (fanin != NULL)
        *fanin = PLACE_COUNT(tmp);
    list_destroy(&tmp);
    return n;
}

static FitVariable *alloc_var_of(FitEntity *ent, FitFct *fct)
{
    FitVariable *var = entity_var_by_id(ent, fct->var_id);

    if (var == NULL)
        place_trap("alloc_var_of: function without variable");
    return var;
}

/* 1 when var is not a netlist signal, or is the output of a
 * tri-state cell (kind 8) and not a node. */
int alloc_is_plain_node(FitEntity *ent, FitVariable *var)
{
    FitNetItem *item;

    if (latch_find_alias(var->name, ent->net) == 0)
        return 1;
    if (entity_var_is_node(ent, var) != 0)
        return 0;
    item = netlist_find_item(ent, var->name);
    if (item == NULL)
        return 0;
    return item->kind == FIT_GATE_TH;
}

/* Stores the fan-in of every function of the work list in its
 * flags and sorts the list by it, largest first. */
static void alloc_sort_by_fanin(FitEntity *ent)
{
    FitFct *fct;
    int i, fanin;

    for (i = 0; i < PLACE_COUNT(g_fit_work_fcts); i++) {
        fct = (FitFct *)g_fit_work_fcts->items[i];
        alloc_fct_terms(ent, fct, alloc_var_of(ent, fct), &fanin);
        fct->flags = fanin;
    }
    list_sort(g_fit_work_fcts, 0, route_fct_weight_cmp);
}

/* Macrocells that occur more than once among the preassigned
 * pins (pin and feedback node of the same cell). */
static FitList *alloc_shared_preassign_mcs(void)
{
    FitList *seen = list_create();
    FitList *res = list_create();
    int i, mc;

    for (i = 0; i < PLACE_COUNT(g_preassign); i++) {
        mc = place_macrocell_of_pin(((FitPreassignItem *)g_preassign->items[i])->pin);
        if (mc != 0) {
            if (list_find(seen, FIT_INT2PTR(mc), list_ptr_eq) != 0)
                list_add(res, FIT_INT2PTR(mc));
            else
                list_add(seen, FIT_INT2PTR(mc));
        }
    }
    list_destroy(&seen);
    return res;
}

/* Logic doubling: moves functions from g_fit_doubling_fcts back to the
 * work list until fewer than (logic_doubling - 1) are left, skipping those
 * preassigned to a macrocell that is preassigned twice.  Returns the
 * macrocells the moved functions need; resets logic_doubling to 1. */
static int alloc_place_doubled(FitEntity *ent)
{
    FitList *saved, *shared;
    FitFct *fct;
    FitVariable *var;
    int added = 0, pos = 1, k, mc;

#ifdef PLACE_TRACE
    for (k = 0; k < PLACE_COUNT(g_preassign); k++)
        TRACE("alloc_place_doubled preassign %s %d\n", ((FitPreassignItem *)g_preassign->items[k])->name,
              ((FitPreassignItem *)g_preassign->items[k])->pin);
    TRACE("alloc_place_doubled count %d doubling %d\n", PLACE_COUNT(g_fit_doubling_fcts), g_opt.logic_doubling);
#endif
    saved = g_fit_work_fcts;
    g_fit_work_fcts = g_fit_doubling_fcts;
    alloc_sort_by_fanin(ent);
    g_fit_work_fcts = saved;
    shared = alloc_shared_preassign_mcs();
    for (;;) {
        if (PLACE_COUNT(g_fit_doubling_fcts) <= g_opt.logic_doubling - 1)
            break;
        if (pos > PLACE_COUNT(g_fit_doubling_fcts)) {
            /* every function left is skipped: stop */
            TRACE("alloc_place_doubled overrun pos %d count %d doubling %d\n", pos, PLACE_COUNT(g_fit_doubling_fcts),
                  g_opt.logic_doubling);
            break;
        }
        fct = (FitFct *)g_fit_doubling_fcts->items[pos - 1];
        var = alloc_var_of(ent, fct);
        k = list_find(g_preassign, latch_signal_source(ent, var->name), place_name_eq);
        if (k != 0) {
            mc = place_macrocell_of_pin(((FitPreassignItem *)g_preassign->items[k - 1])->pin);
            if (list_find(shared, FIT_INT2PTR(mc), list_ptr_eq) != 0) {
                pos++;
                continue;
            }
        }
        list_add(g_fit_work_fcts, fct);
        list_del_shift(g_fit_doubling_fcts, pos);
        added += route_mc_for_terms(alloc_fct_terms(ent, fct, var, NULL));
        if (PLACE_COUNT(g_fit_doubling_fcts) < pos)
            break;
    }
    g_opt.logic_doubling = 1;
    return added;
}

int alloc_build_fct_list(FitEntity *ent, FitList *exclude)
{
    FitVariable *var;
    FitFct *fct;
    int total = 0;
    int i, k, n, fanin;

    if (g_fit_work_fcts != NULL)
        list_destroy(&g_fit_work_fcts);
    g_fit_work_fcts = list_create();
    if (g_opt.logic_doubling != 0) {
        if (g_fit_doubling_fcts != NULL)
            list_destroy(&g_fit_doubling_fcts);
        g_fit_doubling_fcts = list_create();
    }
    if (g_fit_excluded_ids != NULL)
        list_destroy(&g_fit_excluded_ids);
    g_fit_excluded_ids = list_copy(exclude);
    for (i = 0; i < PLACE_COUNT(exclude); i++) {
        k = list_find(g_fit_foldback_ids, exclude->items[i], list_ptr_eq);
        if (k != 0)
            list_del_shift(g_fit_foldback_ids, k);
    }

    for (i = 0; i < PLACE_COUNT(ent->outputs); i++) {
        var = (FitVariable *)ent->outputs->items[i];
        if (netlist_has_name(ent, var->name) != 0
            || esp_var_is_simple_control(ent, var) != 0
            || list_find(exclude, FIT_INT2PTR(var->id), list_ptr_eq) != 0)
            continue;
        fct = entity_find_fct(ent, var->id);
        if (fct == NULL)
            place_trap("alloc_build_fct_list: variable without function");
        if (g_opt.logic_doubling != 0) {
            if (!alloc_is_plain_node(ent, var)) {
                if (place_count_terms(ent, fct) == 1 && entity_var_is_node(ent, var) != 0)
                    g_opt.logic_doubling++;
            } else if (place_count_terms(ent, fct) <= 4) {
                list_add(g_fit_doubling_fcts, fct);
                continue;
            }
        }
        n = alloc_fct_terms(ent, fct, var, &fanin);
        total += route_mc_for_terms(n);
        fct->flags = fanin;
        list_add(g_fit_work_fcts, fct);
    }

    for (i = 0; i < PLACE_COUNT(ent->locals); i++) {
        var = (FitVariable *)ent->locals->items[i];
        if (list_find(g_fit_foldback_ids, FIT_INT2PTR(var->id), list_ptr_eq) != 0
            || list_find(exclude, FIT_INT2PTR(var->id), list_ptr_eq) != 0)
            continue;
        fct = entity_find_fct(ent, var->id);
        if (fct == NULL)
            place_trap("alloc_build_fct_list: variable without function");
        n = alloc_fct_terms(ent, fct, var, &fanin);
        total += route_mc_for_terms(n);
        fct->flags = fanin;
        list_add(g_fit_work_fcts, fct);
    }

    list_sort(g_fit_work_fcts, 0, route_fct_weight_cmp);
    if (g_opt.logic_doubling == 0)
        return total;
    return total + alloc_place_doubled(ent);
}

/* Number of inputs fct shares with the id set `set`:
 * |inputs| - |set + inputs| + |set|. */
static int alloc_shared_fanin(FitEntity *ent, FitFct *fct, FitList *set)
{
    FitList *inputs = list_create();
    FitList *both;
    int n;

    fit_drv_count_mc_terms(ent, fct, alloc_var_of(ent, fct), inputs);
    both = list_union(set, inputs, list_ptr_eq);
    n = PLACE_COUNT(inputs) - PLACE_COUNT(both) + PLACE_COUNT(set);
    list_destroy(&inputs);
    list_destroy(&both);
    return n;
}

/* Sorts the work list by the number of inputs shared with
 * `set` (the inputs of the block being filled), largest first. */
static void alloc_sort_by_shared_fanin(FitEntity *ent, FitList *set)
{
    FitFct *fct;
    int i;

    for (i = 0; i < PLACE_COUNT(g_fit_work_fcts); i++) {
        fct = (FitFct *)g_fit_work_fcts->items[i];
        fct->flags = alloc_shared_fanin(ent, fct, set);
    }
    list_sort(g_fit_work_fcts, 0, route_fct_weight_cmp);
}

/* Number of macrocells of block (0-based) that have a package
 * pin which is neither a JTAG pin in use nor a power-down pin in use. */
static int alloc_block_io_pins(int block)
{
    FitList *jtag = NULL, *pd = NULL;
    int pd_on = g_opt.pd1 != 0 || g_opt.pd2 != 0;
    int k, pin, n = 0;

    if (g_opt.jtag != 0)
        jtag = dev_jtag_pin_list();
    if (pd_on)
        pd = dev_pd_pin_list();
    for (k = 0; k < 16; k++) {
        pin = dev_pin_of_macrocell(g_dev_package, block * 16 + k);
        if (pin == 0)
            continue;
        if (g_opt.jtag != 0 && list_find(jtag, FIT_INT2PTR(pin), list_ptr_eq) != 0)
            continue;
        if (pd_on && list_find(pd, FIT_INT2PTR(pin), list_ptr_eq) != 0)
            continue;
        n++;
    }
    list_destroy(&jtag);
    list_destroy(&pd);
    return n;
}

/* FitListEq: pin number of a preassignment item. */
static int alloc_preassign_pin_eq(void *item, void *pin)
{
    return ((FitPreassignItem *)item)->pin == FIT_PTR2INT(pin);
}

/* 1 when pin / node number `pin` lies in block (1-based), on a
 * macrocell with a package pin; for a feedback node (>= 600) only when
 * the macrocell's pin is not preassigned too. */
static int alloc_pin_in_block(int pin, int block)
{
    FitSlot *slot = place_slot_of_pin(pin);

    if (slot == NULL)
        place_trap("alloc_pin_in_block: no macrocell for the pin");
    if (slot->block != block)
        return 0;
    if (slot->pin == 0)
        return 0;
    if (599 < pin
        && list_find(g_preassign, FIT_INT2PTR(slot->pin), alloc_preassign_pin_eq) != 0)
        return 0;
    return 1;
}

/* I/O pins of block (1-based) left for functions: its usable
 * pins minus the inputs preassigned to pins of the block. */
static int alloc_block_free_pins(FitEntity *ent, int block)
{
    FitPreassignItem *item;
    int n = alloc_block_io_pins(block - 1);
    int i, id;

    if (g_fit_preassign_used != 0) {
        for (i = 0; i < PLACE_COUNT(g_preassign); i++) {
            item = (FitPreassignItem *)g_preassign->items[i];
            id = entity_var_id_by_name(ent, item->name);
            if (id != 0 && entity_has_input_id(ent, id) != 0
                && item->pin < 300 && alloc_pin_in_block(item->pin, block) != 0)
                n--;
        }
    }
    return n;
}

/* Written to g_alloc_mc_per_block, which nothing reads. */
static int alloc_mc_per_block(int n_mc)
{
    int n;

    if (g_dev->n_macrocells <= n_mc)
        return 0x10;
    n = n_mc / g_dev->n_blocks;
    if (n % g_dev->n_blocks != 0)
        n++;
    return n;
}

/* Limit of multi-macrocell functions per block. */
static int alloc_wide_limit(void)
{
    if (util_str_icmp(g_dev_package, "PLCC44") == 0
        || util_str_icmp(g_dev_package, "PLCC68") == 0)
        return 6;
    return 8;
}

void alloc_merge_block_foldbacks(FitList *ids, int block, int add)
{
    FitList *have;
    void *id;
    int i = 0, pos = 1;

    while (i < PLACE_COUNT(ids)) {
        id = ids->items[i];
        have = PLACE_LIST(g_fit_block_foldbacks, block);
        if (list_find(have, id, list_ptr_eq) == 0) {
            if (add != 0)
                list_add(have, id);
            i++;
            pos++;
        } else {
            list_del_shift(ids, pos);
        }
    }
}

FitList *alloc_fct_foldbacks(FitEntity *ent, FitFct *fct)
{
    FitList *saved, *inputs, *res;
    FitFct *f;
    void *id;
    int i, j, n, done;

    /* the inputs of fct without looking through foldbacks */
    saved = g_fit_foldback_ids;
    g_fit_foldback_ids = list_create();
    inputs = list_create();
    res = list_create();
    fit_drv_count_mc_terms(ent, fct, alloc_var_of(ent, fct), inputs);
    list_destroy(&g_fit_foldback_ids);
    g_fit_foldback_ids = saved;

    for (i = 0; i < PLACE_COUNT(g_fit_foldback_ids); i++) {
        id = g_fit_foldback_ids->items[i];
        if (list_find(inputs, id, list_ptr_eq) != 0)
            list_add(res, id);
    }
    list_destroy(&inputs);
    do {
        done = 1;
        n = PLACE_COUNT(res);
        for (j = 0; j < n; j++) {
            f = entity_find_fct(ent, PLACE_ID(res, j));
            if (f == NULL)
                place_trap("alloc_fct_foldbacks: variable without function");
            for (i = 0; i < PLACE_COUNT(g_fit_foldback_ids); i++) {
                id = g_fit_foldback_ids->items[i];
                if (list_find(res, id, list_ptr_eq) == 0
                    && fcttree_count_id(f->tree[0], FIT_PTR2INT(id)) != 0) {
                    done = 0;
                    list_add(res, id);
                }
            }
        }
    } while (!done);
    return res;
}

/* Tail of alloc_fill_block_simple and alloc_fill_block: on PLCC44, a
 * block other than the first that ends with six multi-macrocell functions
 * lowers the limit to five and exchanges its function list with the first
 * block's. */
static void alloc_plcc44_exchange(int block, int wide)
{
    FitList *mine, *first;

    if (util_str_icmp(g_dev_package, "PLCC44") != 0 || wide != 6)
        return;
    g_alloc_max_wide = 5;
    if (block == 0)
        return;
    mine = list_copy(PLACE_LIST(g_fit_block_fcts, block));
    first = PLACE_LIST(g_fit_block_fcts, block);
    list_destroy(&first);
    g_fit_block_fcts->items[block] = list_copy(PLACE_LIST(g_fit_block_fcts, 0));
    first = PLACE_LIST(g_fit_block_fcts, 0);
    list_destroy(&first);
    g_fit_block_fcts->items[0] = mine;
}

/* 1 when var needs an I/O pin for the simple partition: it is no node, or
 * it is a node with a fast-inlatch entry. */
static int alloc_needs_pin_simple(FitEntity *ent, FitVariable *var)
{
    if (entity_var_is_node(ent, var) == 0)
        return 1;
    return fit_drv_fast_inlatch_index(var->id) != 0;
}

/* Fills block (0-based) from the work list: the first function
 * is the seed, the others are taken in list order while the limits hold. */
static void alloc_fill_block_simple(FitEntity *ent, int block)
{
    FitList *inputs, *trial, *fb;
    FitFct *fct;
    FitVariable *var;
    int wide = 0;
    int n_pt, mc, total, room, pins, pin, need, idx, pos;

    g_alloc_free_pins = alloc_block_io_pins(block);
    if (g_fit_work_fcts == NULL || g_fit_work_fcts->count == 0)
        return;

    fct = (FitFct *)g_fit_work_fcts->items[0];
    inputs = list_create();
    var = alloc_var_of(ent, fct);
    n_pt = fit_drv_count_mc_terms(ent, fct, var, inputs);
    if (g_fit_route_limit < PLACE_COUNT(inputs))
        g_fit_route_limit = PLACE_COUNT(inputs);
    total = route_mc_for_terms(n_pt);
    need = total - fit_drv_count_borrowed(ent, total, n_pt, var);
    fb = alloc_fct_foldbacks(ent, fct);
    alloc_merge_block_foldbacks(fb, block, 1);
    room = 0x10 - PLACE_COUNT(fb) - need;
    if (room < 0)
        return;
    if (1 < total)
        wide = 1;
    pins = alloc_needs_pin_simple(ent, var);
    list_add(PLACE_LIST(g_fit_block_fcts, block), fct);
    list_del_shift(g_fit_work_fcts, 1);
    if (g_alloc_grouped == 0)
        alloc_sort_by_shared_fanin(ent, inputs);

    pos = 1;
    for (idx = 0; idx < PLACE_COUNT(g_fit_work_fcts) && total != 0x10; idx++, pos++) {
        fct = (FitFct *)g_fit_work_fcts->items[idx];
        trial = list_copy(inputs);
        var = alloc_var_of(ent, fct);
        n_pt = fit_drv_count_mc_terms(ent, fct, var, trial);
        mc = route_mc_for_terms(n_pt);
        need = mc - fit_drv_count_borrowed(ent, mc, n_pt, var);
        fb = alloc_fct_foldbacks(ent, fct);
        alloc_merge_block_foldbacks(fb, block, 0);
        need += PLACE_COUNT(fb);
        pin = alloc_needs_pin_simple(ent, var);
        if (g_fit_route_limit < PLACE_COUNT(trial)
            || 0x10 < total + mc
            || g_alloc_max_wide < (1 < mc) + wide
            || room - need < 0
            || g_alloc_free_pins < pin + pins) {
            list_destroy(&fb);
            list_destroy(&trial);
            continue;
        }
        alloc_merge_block_foldbacks(fb, block, 1);
        list_destroy(&fb);
        list_destroy(&trial);
        room -= need;
        total += mc;
        pins += pin;
        fit_drv_count_mc_terms(ent, fct, var, inputs);
        list_del_shift(g_fit_work_fcts, pos);
        idx--;
        pos--;
        if (1 < mc) {
            wide++;
            list_insert_at(PLACE_LIST(g_fit_block_fcts, block), fct, 1);
        } else {
            list_add(PLACE_LIST(g_fit_block_fcts, block), fct);
        }
    }
    alloc_plcc44_exchange(block, wide);
}

int alloc_partition_simple(FitEntity *ent, int n_mc)
{
    FitList *saved;
    int b, wide_limit;

    /* n_mc / n_macrocells * 100 > 90, exact in integers because
     * n_macrocells is a power of two */
    if (n_mc * 100 > 90 * g_dev->n_macrocells) {
        if (g_fit_retry_count == 0)
            fprintf(g_fit_report, "Info: %s uses 90%% of the logic resources in device %s\n",
                    g_opt.out_base, g_dev_package);
        fprintf(g_fit_report, "  If you wish to have more pins available for future logic change\n");
        fprintf(g_fit_report, "  Atmel recommends using a larger device.\n");
    }
    if (g_fit_preassign_used != 0)
        return 0;
    g_alloc_mc_per_block = alloc_mc_per_block(n_mc);
    g_alloc_max_wide = alloc_wide_limit();
    g_fit_block_fcts = list_create();
    g_fit_block_foldbacks = list_create();
    for (b = 0; b < g_dev->n_blocks; b++) {
        list_add(g_fit_block_foldbacks, list_create());
        list_add(g_fit_block_fcts, list_create());
    }
    saved = list_copy(g_fit_work_fcts);
    wide_limit = g_alloc_max_wide;
    for (;;) {
        g_alloc_max_wide = wide_limit;
        for (b = 0; b < g_dev->n_blocks; b++) {
            alloc_sort_by_fanin(ent);
            alloc_fill_block_simple(ent, b);
        }
        if (g_fit_work_fcts == NULL || g_fit_work_fcts->count == 0)
            return 1;
        if (0x28 <= g_fit_route_limit) {
            list_destroy(&g_fit_work_fcts);
            g_fit_work_fcts = saved;
            list_free_all(&g_fit_block_fcts, destroy_list_cb);
            list_free_all(&g_fit_block_foldbacks, destroy_list_cb);
            return 0;
        }
        list_destroy(&g_fit_work_fcts);
        g_fit_work_fcts = list_copy(saved);
        g_fit_route_limit++;
        for (b = 0; b < g_dev->n_blocks; b++) {
            alloc_reset_list(g_fit_block_fcts, b);
            alloc_reset_list(g_fit_block_foldbacks, b);
        }
    }
}

/* All block inputs of block (0-based), used or not, whose
 * multiplexer can select `node`. */
static FitList *alloc_uim_candidates(const char *node, int block)
{
    FitList *res = list_create();
    FitList *l = PLACE_LIST(g_fit_uim_slots, block);
    FitUimSlot *u;
    int j, k;

    for (j = 0; j < PLACE_COUNT(l); j++) {
        u = (FitUimSlot *)l->items[j];
        for (k = 0; k < g_dev->uim_width; k++) {
            if (dev_uim_selects(u, node, k)) {
                list_add(res, u);
                break;
            }
        }
    }
    return res;
}

/* For every node name of `names` its list from alloc_uim_candidates. */
static FitList *alloc_uim_candidate_lists(FitList *names, int block)
{
    FitList *res = list_create();
    FitList *c;
    char *node;
    int i, j;

    for (i = 0; i < PLACE_COUNT(names); i++) {
        node = (char *)names->items[i];
        c = alloc_uim_candidates(node, block);
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

/* Takes one cluster off the front of the parallel lists cands /
 * names: the first name and every name whose candidates overlap, directly
 * or through others, with the block inputs collected so far.  Returns the
 * block inputs of the cluster; *taken receives its names. */
static FitList *alloc_take_cluster(FitList *cands, FitList *names, FitList **taken)
{
    FitList *set = list_create();
    FitList *l, *common;
    void *name;
    int i, j, pos, done;

    *taken = list_create();
    l = PLACE_LIST(cands, 0);
    for (j = 0; j < PLACE_COUNT(l); j++)
        list_add(set, l->items[j]);
    list_add(*taken, names->items[0]);
    list_del_shift(cands, 1);
    list_del_shift(names, 1);
    do {
        done = 1;
        i = 0;
        pos = 1;
        while (i < PLACE_COUNT(cands)) {
            name = names->items[i];
            l = PLACE_LIST(cands, i);
            common = list_intersection(l, set, place_uim_index_eq);
            if (common != NULL) {
                list_destroy(&common);
                done = 0;
                for (j = 0; j < PLACE_COUNT(l); j++) {
                    if (list_find(set, l->items[j], place_uim_index_eq) == 0)
                        list_add(set, l->items[j]);
                }
                list_add(*taken, name);
                list_del_shift(cands, pos);
                list_del_shift(names, pos);
                i--;
                pos--;
            }
            i++;
            pos++;
        }
    } while (!done);
    return set;
}

/* Splits the node names of block (0-based) into clusters (see
 * alloc_take_cluster) and returns the list of the clusters' name lists.
 * *failed_ids receives a new list: the variable ids of the last cluster
 * that has more names than block inputs, empty when there is none. */
FitList *alloc_block_clusters(FitEntity *ent, FitList *names, int block, FitList **failed_ids)
{
    FitList *sets = list_create();
    FitList *clusters = list_create();
    FitList *work = list_copy(names);
    FitList *cands = alloc_uim_candidate_lists(work, block);
    FitList *set, *taken, *over = NULL;
    char *signal;
    int i;

    while (cands != NULL && cands->count != 0) {
        set = alloc_take_cluster(cands, work, &taken);
        if (PLACE_COUNT(set) < PLACE_COUNT(taken))
            over = taken;
        list_add(sets, set);
        list_add(clusters, taken);
    }
    *failed_ids = list_create();
    if (over != NULL) {
        for (i = 0; i < PLACE_COUNT(over); i++) {
            place_slot_of_node((char *)over->items[i], &signal);
            list_add(*failed_ids, FIT_INT2PTR(entity_var_id_by_name(ent, signal)));
        }
    }
    return clusters;
}

/* 1 when `name` is preassigned to a macrocell whose pin is not
 * preassigned as well (a node preassigned to a feedback node whose pin is
 * therefore lost). */
static int alloc_preassigned_blocks_pin(char *name)
{
    FitSlot *slot;
    int k;

    k = list_find(g_preassign, name, place_name_eq);
    if (k != 0) {
        slot = place_slot_of_pin(((FitPreassignItem *)g_preassign->items[k - 1])->pin);
        if (slot == NULL)
            place_trap("alloc_preassigned_blocks_pin: no macrocell for the pin");
        if (slot->pin != 0)
            return list_find(g_preassign, FIT_INT2PTR(slot->pin), alloc_preassign_pin_eq) == 0;
    }
    return 0;
}

/* State of block (0-based) from the functions already assigned
 * to it.  Returns the number of functions that need a pin. */
static int alloc_block_usage(FitEntity *ent, int block, FitList **inputs, int *total,
                             int *wide, int *room, int *doubled)
{
    FitList *l = PLACE_LIST(g_fit_block_fcts, block);
    FitFct *fct;
    FitVariable *var;
    int used = 0, pins = 0;
    int i, n_pt, mc, node;

    *doubled = 0;
    *wide = 0;
    *total = 0;
    *inputs = list_create();
    for (i = 0; i < PLACE_COUNT(l); i++) {
        fct = (FitFct *)l->items[i];
        var = alloc_var_of(ent, fct);
        n_pt = fit_drv_count_mc_terms(ent, fct, var, *inputs);
        mc = route_mc_for_terms(n_pt);
        used += mc - fit_drv_count_borrowed(ent, mc, n_pt, var);
        *total += mc;
        if (1 < mc)
            (*wide)++;
        node = entity_var_is_node(ent, var);
        pins += node == 0;
        if (node != 0 && g_opt.logic_doubling != 0
            && fit_drv_is_single_term_reg(ent, fct, var, 5) != 0) {
            (*total)--;
            (*doubled)++;
        }
    }
    *room = 0x10 - PLACE_COUNT(PLACE_LIST(g_fit_block_foldbacks, block)) - used;
    return pins;
}

/* Function behind a preassigned name: of the q signal of the
 * register it is an input of or (lib_instance_by_output) an output of,
 * else its own. */
static FitFct *alloc_fct_of_name(FitEntity *ent, char *name)
{
    FitInstance *inst;
    int kind;

    inst = tt3_find_instance_by_alias(ent, name, &kind);
    if (inst == NULL)
        inst = lib_instance_by_output(ent, name);
    if (inst != NULL)
        name = inst->q;
    return entity_find_fct(ent, entity_var_id_by_name(ent, name));
}

/* Macrocells needed by the doubled functions (g_fit_doubling_fcts) that
 * are preassigned into block (1-based). */
static int alloc_block_doubled_mcs(FitEntity *ent, int block)
{
    FitPreassignItem *item;
    FitFct *fct;
    int i, n = 0;

    for (i = 0; i < PLACE_COUNT(g_preassign); i++) {
        item = (FitPreassignItem *)g_preassign->items[i];
        if (alloc_pin_in_block(item->pin, block) == 0)
            continue;
        if (entity_var_id_by_name(ent, item->name) == 0)
            continue;
        fct = alloc_fct_of_name(ent, item->name);
        if (fct == NULL)
            continue;
        if (list_find(g_fit_doubling_fcts, fct, alloc_first_int_eq) == 0)
            continue;
        n += route_mc_for_terms(alloc_fct_terms(ent, fct, alloc_var_of(ent, fct), NULL));
    }
    return n;
}

/* Adds functions of the work list to block (0-based) on top of
 * what g_fit_block_fcts already holds for it.  While g_equiv_verbose is nonzero
 * and preassignments are honoured, the reason a function was refused is
 * left in g_equiv_verbose: the fan-in when above 40, 2 macrocells, 3
 * foldbacks, 4 multi-macrocell functions, 5 pins. */
static void alloc_fill_block(FitEntity *ent, int block)
{
    FitList *inputs, *trial, *fb;
    FitFct *fct;
    FitVariable *var;
    int total, wide, room, doubled, pins, pins0, reserved;
    int n_pt, mc, need, pin, idx, pos;

    if (g_fit_work_fcts == NULL || g_fit_work_fcts->count == 0)
        return;
    pins = alloc_block_usage(ent, block, &inputs, &total, &wide, &room, &doubled);
    pins0 = pins;
    g_alloc_free_pins = alloc_block_free_pins(ent, block + 1);
    reserved = alloc_block_doubled_mcs(ent, block + 1);
    total += reserved < doubled ? doubled : reserved;
    if (g_alloc_grouped == 0)
        alloc_sort_by_shared_fanin(ent, inputs);

    pos = 1;
    for (idx = 0; ; idx++, pos++) {
        if (idx >= PLACE_COUNT(g_fit_work_fcts))
            goto done;
        if (total == 0x10 && g_opt.logic_doubling == 0)
            break;
        fct = (FitFct *)g_fit_work_fcts->items[idx];
        trial = list_copy(inputs);
        var = alloc_var_of(ent, fct);
        n_pt = fit_drv_count_mc_terms(ent, fct, var, trial);
        mc = route_mc_for_terms(n_pt);
        need = mc - fit_drv_count_borrowed(ent, mc, n_pt, var);
        fb = alloc_fct_foldbacks(ent, fct);
        alloc_merge_block_foldbacks(fb, block, 0);
        need += PLACE_COUNT(fb);
        if (entity_var_is_node(ent, var) == 0) {
            pin = 1;
        } else {
            pin = alloc_preassigned_blocks_pin(latch_signal_source(ent, var->name));
            if (pin == 0 && fit_drv_fast_inlatch_index(var->id) != 0)
                pin = 1;
            if (g_opt.logic_doubling != 0
                && fit_drv_is_single_term_reg(ent, fct, var, 5) != 0
                && doubled < reserved) {
                doubled++;
                mc--;
            }
        }
        if (g_fit_route_limit < PLACE_COUNT(trial)
            || 0x10 < total + mc
            || room - need < 0
            || g_alloc_max_wide < (1 < mc) + wide
            || g_alloc_free_pins < pins + pin) {
            if (g_equiv_verbose != 0 && g_fit_preassign_used != 0) {
                if (trial != NULL && 0x28 < trial->count)
                    g_equiv_verbose = trial->count;
                else if (0x10 < total + mc)
                    g_equiv_verbose = 2;
                else if (room - need < 0)
                    g_equiv_verbose = 3;
                else if (g_alloc_max_wide < (1 < mc) + wide)
                    g_equiv_verbose = 4;
                else if (g_alloc_free_pins < pin + pins)
                    g_equiv_verbose = 5;
            }
            list_destroy(&fb);
            list_destroy(&trial);
            continue;
        }
        pins += pin;
        alloc_merge_block_foldbacks(fb, block, 1);
        list_destroy(&fb);
        list_destroy(&trial);
        total += mc;
        room -= need;
        fit_drv_count_mc_terms(ent, fct, var, inputs);
        list_del_shift(g_fit_work_fcts, pos);
        idx--;
        pos--;
        if (mc < 2) {
            list_add(PLACE_LIST(g_fit_block_fcts, block), fct);
        } else {
            wide++;
            list_insert_at(PLACE_LIST(g_fit_block_fcts, block), fct, pins0 + 1);
        }
    }
    if (g_equiv_verbose != 0 && g_fit_preassign_used != 0
        && g_fit_work_fcts != NULL && g_fit_work_fcts->count != 0)
        g_equiv_verbose = 2;
done:
    alloc_plcc44_exchange(block, wide);
}

int alloc_first_int_eq(void *a, void *b)
{
    return *(int *)a == *(int *)b;
}

/* Block letter for a 1-based block number.  Its result is used
 * by the three failure messages of alloc_place_preassigned. */
static const char *alloc_block_letter(int block)
{
    switch (block) {
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

/* Distributes the work list over all blocks with alloc_fill_block,
 * starting again from `work` with the fan-in limit raised by one and the
 * block lists restored from base_block_fcts / base_block_foldbacks while
 * functions are left over.  0 when the limit has reached 40. */
static int alloc_distribute(FitEntity *ent, FitList *work, FitList *base_block_fcts, FitList *base_block_foldbacks)
{
    int b;

    for (;;) {
        for (b = 0; b < g_dev->n_blocks; b++) {
            alloc_sort_by_fanin(ent);
            alloc_fill_block(ent, b);
        }
        if (g_fit_work_fcts == NULL || g_fit_work_fcts->count == 0)
            return 1;
        if (0x27 < g_fit_route_limit)
            return 0;
        list_destroy(&g_fit_work_fcts);
        g_fit_work_fcts = list_copy(work);
        g_fit_route_limit++;
        list_free_all(&g_fit_block_fcts, destroy_list_cb);
        list_free_all(&g_fit_block_foldbacks, destroy_list_cb);
        g_fit_block_fcts = list_map_copy(base_block_fcts, copy_list_cb);
        g_fit_block_foldbacks = list_map_copy(base_block_foldbacks, copy_list_cb);
    }
}

/* Puts the functions with a preassigned pin into the block of
 * their pin, checks that every block can hold them, then distributes the
 * rest of the work list.  0 on failure (reason in the .fit report). */
static int alloc_place_preassigned(FitEntity *ent)
{
    FitList *per_block, *rest, *base_block_fcts, *base_block_foldbacks;
    FitPreassignItem *item;
    FitFct *fct;
    int i, b, k, block, saved_limit, ok;

    per_block = list_map_copy(g_fit_block_fcts, copy_list_cb);
    for (i = 0; i < PLACE_COUNT(g_preassign); i++) {
        item = (FitPreassignItem *)g_preassign->items[i];
        if (entity_var_id_by_name(ent, item->name) == 0)
            continue;
        fct = alloc_fct_of_name(ent, item->name);
        if (fct == NULL)
            continue;
        block = place_block_of_pin(item->pin);
        if (block == 0) {
            fprintf(g_fit_report, "Bad assignment for output : %s\n", item->name);
            return 0;
        }
        if (g_opt.logic_doubling == 0
            || list_find(g_fit_doubling_fcts, fct, alloc_first_int_eq) == 0)
            list_add(PLACE_LIST(per_block, block - 1), fct);
        k = list_find(g_fit_work_fcts, fct, alloc_first_int_eq);
        if (k != 0)
            list_del_shift(g_fit_work_fcts, k);
    }
    rest = list_copy(g_fit_work_fcts);
    saved_limit = g_fit_route_limit;
    g_fit_route_limit = 0x28;
    g_equiv_verbose = 1;
    for (b = 0; b < g_dev->n_blocks; b++) {
        g_fit_work_fcts = PLACE_LIST(per_block, b);
        alloc_fill_block(ent, b);
        if (g_fit_work_fcts != NULL && g_fit_work_fcts->count != 0) {
            if (0x28 < g_equiv_verbose)
                fprintf(g_fit_report, "Unable to Keep Fanin of %d on Block %s\n",
                        g_equiv_verbose, alloc_block_letter(b + 1));
            else if (g_equiv_verbose == 2)
                fprintf(g_fit_report, "More than 16 MCs on Block %s\n", alloc_block_letter(b + 1));
            else if (g_equiv_verbose == 3)
                fprintf(g_fit_report, "Unable to place all foldbacks on Block %s\n",
                        alloc_block_letter(b + 1));
            g_equiv_verbose = 0;
            return 0;
        }
    }
    g_equiv_verbose = 0;
    g_fit_route_limit = saved_limit;
    g_fit_work_fcts = list_copy(rest);
    if (g_fit_work_fcts != NULL && g_fit_work_fcts->count != 0) {
        base_block_fcts = list_map_copy(g_fit_block_fcts, copy_list_cb);
        base_block_foldbacks = list_map_copy(g_fit_block_foldbacks, copy_list_cb);
        rest = list_copy(g_fit_work_fcts);
        ok = alloc_distribute(ent, rest, base_block_fcts, base_block_foldbacks);
        if (!ok) {
            list_destroy(&g_fit_block_fcts);
            list_destroy(&g_fit_block_foldbacks);
            list_destroy(&g_fit_work_fcts);
            list_destroy(&base_block_fcts);
            list_destroy(&rest);
            return 0;
        }
    }
    return 1;
}

/* Moves the functions that use foldbacks from the work list to
 * g_fit_block_groups (here a flat list of FitFct). */
static void alloc_split_foldback_users(FitEntity *ent)
{
    FitFct *fct;
    FitList *fb;
    int i = 0, pos = 1;

    if (g_fit_block_groups != NULL)
        list_destroy(&g_fit_block_groups);
    g_fit_block_groups = list_create();
    while (i < PLACE_COUNT(g_fit_work_fcts)) {
        fct = (FitFct *)g_fit_work_fcts->items[i];
        fb = alloc_fct_foldbacks(ent, fct);
        if (fb != NULL && fb->count != 0) {
            list_add(g_fit_block_groups, fct);
            list_del_shift(g_fit_work_fcts, pos);
            i--;
            pos--;
        }
        list_destroy(&fb);
        i++;
        pos++;
    }
}

/* Moves the functions that need a pin (no node) and the nodes
 * whose source signal is preassigned from the work list to g_alloc_preassigned_fcts. */
static void alloc_split_pin_bound(FitEntity *ent)
{
    FitFct *fct;
    FitVariable *var;
    int i = 0, pos = 1;

    if (g_alloc_preassigned_fcts != NULL)
        list_destroy(&g_alloc_preassigned_fcts);
    g_alloc_preassigned_fcts = list_create();
    while (i < PLACE_COUNT(g_fit_work_fcts)) {
        fct = (FitFct *)g_fit_work_fcts->items[i];
        var = alloc_var_of(ent, fct);
        if (entity_var_is_node(ent, var) == 0
            || list_find(g_preassign, latch_signal_source(ent, var->name), place_name_eq) != 0) {
            list_add(g_alloc_preassigned_fcts, fct);
            list_del_shift(g_fit_work_fcts, pos);
            i--;
            pos--;
        }
        i++;
        pos++;
    }
}

int alloc_partition(FitEntity *ent, int n_mc)
{
    FitList *saved, *first, *base_block_fcts, *base_block_foldbacks;
    int b, i, ok;

    g_alloc_mc_per_block = alloc_mc_per_block(n_mc);
    g_alloc_max_wide = alloc_wide_limit();
    g_fit_block_fcts = list_create();
    g_fit_block_foldbacks = list_create();
    g_alloc_preassigned_fcts = list_create();
    for (b = 0; b < g_dev->n_blocks; b++) {
        list_add(g_fit_block_fcts, list_create());
        list_add(g_fit_block_foldbacks, list_create());
    }
    alloc_split_foldback_users(ent);
    alloc_split_pin_bound(ent);
    saved = g_fit_work_fcts;
    first = list_union(g_alloc_preassigned_fcts, g_fit_block_groups, alloc_first_int_eq);
    g_fit_work_fcts = list_copy(first);
    g_alloc_grouped = 0;

    if (g_fit_preassign_used != 0) {
        if (alloc_place_preassigned(ent) == 0)
            return 0;
    } else {
        for (;;) {
            if (g_alloc_grouped == 0) {
                for (b = 0; b < g_dev->n_blocks; b++) {
                    alloc_sort_by_fanin(ent);
                    alloc_fill_block_simple(ent, b);
                }
            } else {
                /* one group per block first; `first` now collects what the
                 * groups leave over */
                first = NULL;
                for (i = 0; i < PLACE_COUNT(g_fit_block_groups) && i < g_dev->n_blocks; i++) {
                    g_fit_work_fcts = list_copy(PLACE_LIST(g_fit_block_groups, i));
                    alloc_fill_block_simple(ent, i);
                    first = list_union(first, g_fit_work_fcts, alloc_first_int_eq);
                }
                if (i < PLACE_COUNT(g_fit_block_groups)) {
                    g_fit_work_fcts = list_copy(first);
                    for (; i < PLACE_COUNT(g_fit_block_groups); i++)
                        g_fit_work_fcts = list_union(PLACE_LIST(g_fit_block_groups, i), g_fit_work_fcts,
                                                alloc_first_int_eq);
                    g_fit_work_fcts = list_union(g_alloc_preassigned_fcts, g_fit_work_fcts, alloc_first_int_eq);
                }
                if (PLACE_COUNT(g_fit_block_groups) <= g_dev->n_blocks) {
                    g_fit_work_fcts = list_union(g_alloc_preassigned_fcts, g_fit_work_fcts, alloc_first_int_eq);
                    g_fit_work_fcts = list_union(first, g_fit_work_fcts, alloc_first_int_eq);
                }
                for (b = 0; b < g_dev->n_blocks; b++) {
                    alloc_sort_by_fanin(ent);
                    alloc_fill_block(ent, b);
                }
            }
            if (g_fit_work_fcts == NULL || g_fit_work_fcts->count == 0)
                break;
            if (g_fit_route_limit == 0x28) {
                if (g_alloc_grouped != 0)
                    goto fail;
                fit_drv_build_groups(ent);
                g_alloc_grouped = 1;
                g_fit_route_limit = 0x18;
            } else if (0x27 < g_fit_route_limit) {
                goto fail;
            }
            list_destroy(&g_fit_work_fcts);
            g_fit_work_fcts = list_copy(first);
            g_fit_route_limit++;
            for (b = 0; b < g_dev->n_blocks; b++) {
                alloc_reset_list(g_fit_block_foldbacks, b);
                alloc_reset_list(g_fit_block_fcts, b);
            }
        }
    }

    /* the buried nodes that were set aside */
    base_block_fcts = list_map_copy(g_fit_block_fcts, copy_list_cb);
    base_block_foldbacks = list_map_copy(g_fit_block_foldbacks, copy_list_cb);
    g_fit_work_fcts = list_copy(saved);
    ok = alloc_distribute(ent, saved, base_block_fcts, base_block_foldbacks);
    if (!ok) {
        list_destroy(&g_fit_block_fcts);
        list_destroy(&g_fit_block_foldbacks);
    }
    list_destroy(&g_fit_work_fcts);
    list_destroy(&base_block_fcts);
    list_destroy(&saved);
    list_destroy(&first);
    return ok;

fail:
    list_free_all(&g_fit_block_foldbacks, destroy_list_cb);
    list_free_all(&g_fit_block_fcts, destroy_list_cb);
    list_destroy(&g_fit_work_fcts);
    list_destroy(&saved);
    list_destroy(&first);
    return 0;
}

/* Copy of the per-block cluster lists down to the name lists. */
static FitList *alloc_copy_clusters(FitList *all)
{
    FitList *res = list_create();
    FitList *bl, *copy;
    int b, i;

    for (b = 0; b < PLACE_COUNT(all); b++) {
        bl = PLACE_LIST(all, b);
        copy = list_create();
        for (i = 0; i < PLACE_COUNT(bl); i++)
            list_add(copy, list_copy(PLACE_LIST(bl, i)));
        list_add(res, copy);
    }
    return res;
}

/* Frees the per-block cluster lists of *all and the list itself. */
static void alloc_free_clusters(FitList **all)
{
    FitList *bl;
    int b;

    for (b = 0; b < PLACE_COUNT(*all); b++) {
        bl = PLACE_LIST(*all, b);
        list_free_all(&bl, destroy_list_cb);
    }
    list_destroy(all);
}

/* Sum over the clusters of the names (consumed) of block
 * (0-based) of (names - block inputs) where positive. */
static int alloc_excess_block(FitList *names, int block)
{
    FitList *cands = alloc_uim_candidate_lists(names, block);
    FitList *set, *taken;
    int n = 0;

    while (cands != NULL && cands->count != 0) {
        set = alloc_take_cluster(cands, names, &taken);
        if (PLACE_COUNT(set) < PLACE_COUNT(taken))
            n += PLACE_COUNT(taken) - PLACE_COUNT(set);
        list_destroy(&set);
        list_destroy(&taken);
    }
    list_destroy(&cands);
    return n;
}

/* Total excess of all blocks. */
static int alloc_excess_total(FitList *all)
{
    FitList *bl, *names;
    int b, i, total = 0;

    for (b = 0; b < PLACE_COUNT(all); b++) {
        bl = PLACE_LIST(all, b);
        names = list_create();
        for (i = 0; i < PLACE_COUNT(bl); i++)
            list_append(names, PLACE_LIST(bl, i));
        total += alloc_excess_block(names, b);
        list_destroy(&names);
    }
    return total;
}

/* Sum over all clusters of the names above the cluster limit. */
static int alloc_cluster_overflow(FitList *all)
{
    FitList *bl;
    int limit = alloc_cluster_limit();
    int b, i, n, total = 0;

    for (b = 0; b < PLACE_COUNT(all); b++) {
        bl = PLACE_LIST(all, b);
        for (i = 0; i < PLACE_COUNT(bl); i++) {
            n = PLACE_COUNT(PLACE_LIST(bl, i));
            if (limit < n)
                total += n - limit;
        }
    }
    return total;
}

/* Records in every block's clusters that the signals on nodes
 * `name` (cluster cl) and `other` (cluster k) changed places. */
static void alloc_swap_names(FitList *all, char *name, char *other, int cl, int k)
{
    FitList *a, *b;
    int blk, ia, ib;

    for (blk = 0; blk < PLACE_COUNT(all); blk++) {
        a = PLACE_LIST(PLACE_LIST(all, blk), cl);
        b = PLACE_LIST(PLACE_LIST(all, blk), k);
        ia = list_find(a, name, place_str_ieq);
        ib = list_find(b, other, place_str_ieq);
        if (ia != 0) {
            if (ib == 0) {
                list_del_shift(a, ia);
                list_add(b, other);
            }
        } else if (ib != 0) {
            list_del_shift(b, ib);
            list_add(a, name);
        }
    }
}

/* 1 when exchanging the two nodes lowers the cluster overflow
 * (or, when it removes it, leaves an excess below the old overflow). */
static int alloc_swap_improves(FitList *all, char *name, char *other, int cl, int k)
{
    FitList *copy = alloc_copy_clusters(all);
    int before, after;

    TRACE("alloc_swap_improves %.7s %.7s\n", name, other);
    alloc_swap_names(copy, name, other, cl, k);
    before = alloc_cluster_overflow(all);
    after = alloc_cluster_overflow(copy);
    if (after == 0)
        after = alloc_excess_total(copy);
    alloc_free_clusters(&copy);
    return after < before;
}

/* 1 when pin is a JTAG pin in use or a power-down pin in use. */
static int alloc_pin_is_reserved(int pin)
{
    FitList *l;
    int r;

    if (g_opt.jtag != 0) {
        l = dev_jtag_pin_list();
        r = list_find(l, FIT_INT2PTR(pin), list_ptr_eq);
        list_destroy(&l);
        if (r != 0)
            return 1;
    }
    if (g_opt.pd1 != 0 || g_opt.pd2 != 0) {
        l = dev_pd_pin_list();
        r = (g_opt.pd1 != 0 && FIT_PTR2INT(l->items[0]) == pin)
            || (g_opt.pd2 != 0 && FIT_PTR2INT(l->items[1]) == pin);
        list_destroy(&l);
        if (r)
            return 1;
    }
    return 0;
}

/* Slot of a node name; the signal copy place_slot_of_node returns is not
 * wanted. */
static FitSlot *alloc_slot(const char *node)
{
    char *signal = NULL;
    FitSlot *slot = place_slot_of_node(node, &signal);

    free(signal);
    return slot;
}

/* 1 when the macrocell has a foldback that costs a product term
 * (foldback neither "--" nor "NA"). */
static int alloc_foldback_term(FitSlot *slot)
{
    return util_str_icmp(slot->foldback, FIT_FREE_NAME) != 0
           && util_str_icmp(slot->foldback, "NA") != 0;
}

/* Function of the signal called `name` (a slot's buried / signal). */
static FitFct *alloc_fct_of_signal(FitEntity *ent, char *name)
{
    FitVariable *var = entity_find_var(ent, name);
    FitFct *fct;

    if (var == NULL)
        place_trap("alloc_fct_of_signal: signal not in the design");
    fct = place_fct_of_var(ent, var);
    if (fct == NULL)
        place_trap("alloc_fct_of_signal: signal without function");
    return fct;
}

/* Exchanges the buried signals of two macrocells. */
static int alloc_swap_feedback(FitEntity *ent, char *name, char *other)
{
    FitSlot *a = alloc_slot(name);
    FitSlot *b = alloc_slot(other);
    char *a1c = a->foldback, *a20 = a->fn_kind, *a18 = a->buried;

    TRACE("alloc_swap_feedback %.7s %.7s\n", name, other);
    (void)ent;
    if ((a->fct_id != 0 && util_str_icmp(a18, FIT_FREE_NAME) != 0)
        || (b->fct_id != 0 && util_str_icmp(b->buried, FIT_FREE_NAME) != 0)) {
        a->buried = b->buried;
        b->buried = a18;
        a->fn_kind = util_strdup(b->fn_kind);
        b->fn_kind = a20;
        return 1;
    }
    a->buried = b->buried;
    b->buried = a18;
    if (util_str_icmp(a->buried, FIT_FREE_NAME) == 0) {
        a->fn_kind = util_strdup(FIT_FREE_NAME);
    } else if (util_str_icmp(b->buried, FIT_FREE_NAME) == 0) {
        b->fn_kind = util_strdup(FIT_FREE_NAME);
    } else {
        a->fn_kind = util_strdup(b->fn_kind);
        b->fn_kind = a20;
    }
    a->foldback = util_strdup(b->foldback);
    b->foldback = a1c;
    return 1;
}

/* Exchanges the pin signals (and pin functions) of two
 * macrocells; refused for JTAG / power-down pins. */
static int alloc_swap_pins(FitEntity *ent, char *name, char *other)
{
    FitSlot *a = alloc_slot(name);
    FitSlot *b = alloc_slot(other);
    char *a20, *a1c, *asig;
    int ai;

    TRACE("alloc_swap_pins %.7s %.7s\n", name, other);
    (void)ent;
    if (alloc_pin_is_reserved(a->pin) != 0)
        return 0;
    if (alloc_pin_is_reserved(b->pin) != 0)
        return 0;
    a20 = a->fn_kind;
    a1c = a->foldback;
    ai = a->fct_id;
    asig = a->signal;
    if (ai == 0) {
        if (b->fct_id == 0) {
            a->signal = b->signal;
            b->signal = asig;
            return 1;
        }
    } else if (util_str_icmp(b->buried, FIT_FREE_NAME) != 0) {
        goto with_buried;
    }
    if (b->fct_id == 0 || util_str_icmp(a->buried, FIT_FREE_NAME) == 0) {
        a->fct_id = b->fct_id;
        b->fct_id = ai;
        a->signal = b->signal;
        b->signal = asig;
        a->fn_kind = util_strdup(b->fn_kind);
        b->fn_kind = a20;
        a->foldback = util_strdup(b->foldback);
        b->foldback = a1c;
        return 1;
    }
with_buried:
    a->fct_id = b->fct_id;
    b->fct_id = ai;
    a->signal = b->signal;
    b->signal = asig;
    if (util_str_icmp(a->buried, FIT_FREE_NAME) == 0) {
        a->fn_kind = util_strdup("com");
        return 1;
    }
    if (util_str_icmp(b->buried, FIT_FREE_NAME) == 0)
        b->fn_kind = util_strdup("com");
    return 1;
}

/* The buried functions of both cells fit into five terms next
 * to the cell's foldback. */
static int alloc_swap_terms_ok(FitEntity *ent, FitSlot *a, FitSlot *b)
{
    int ea = alloc_foldback_term(a);
    int eb = alloc_foldback_term(b);

    if (util_str_icmp(a->buried, FIT_FREE_NAME) != 0
        && 5 < route_fct_terms(ent, alloc_fct_of_signal(ent, a->buried)) + ea)
        return 0;
    if (util_str_icmp(b->buried, FIT_FREE_NAME) != 0
        && 5 < route_fct_terms(ent, alloc_fct_of_signal(ent, b->buried)) + eb)
        return 0;
    return 1;
}

/* One direction of alloc_swap_terms_ok_doubled: can the buried function
 * of `from` share a macrocell with the pin function of `to`? */
static int alloc_doubled_terms_ok(FitEntity *ent, FitSlot *to, FitSlot *from, int extra)
{
    FitVariable *var;
    FitFct *fct, *buried;
    int n;

    if (to->fct_id == 0) {
        if (util_str_icmp(from->buried, FIT_FREE_NAME) != 0
            && 5 < route_fct_terms(ent, alloc_fct_of_signal(ent, from->buried)) + extra)
            return 0;
        return 1;
    }
    var = entity_var_by_id(ent, to->fct_id);
    if (var == NULL)
        place_trap("alloc_doubled_terms_ok: no variable with this id");
    if (!alloc_is_plain_node(ent, var))
        return 0;
    fct = entity_find_fct(ent, var->id);
    if (fct == NULL)
        place_trap("alloc_doubled_terms_ok: variable without function");
    n = route_fct_terms(ent, fct);
    if (util_str_icmp(from->buried, FIT_FREE_NAME) != 0) {
        buried = alloc_fct_of_signal(ent, from->buried);
        if (fit_drv_is_single_term_reg(ent, buried, alloc_var_of(ent, buried), 5) == 0)
            return 0;
        if (5 < route_fct_terms(ent, buried) + extra + n)
            return 0;
    }
    return 1;
}

/* Term check of an exchange under logic doubling. */
static int alloc_swap_terms_ok_doubled(FitEntity *ent, FitSlot *a, FitSlot *b)
{
    int ea = alloc_foldback_term(a);
    int eb = alloc_foldback_term(b);

    if (!alloc_doubled_terms_ok(ent, a, b, ea))
        return 0;
    if (!alloc_doubled_terms_ok(ent, b, a, eb))
        return 0;
    return 1;
}

/* Common tail of alloc_can_swap_feedback and alloc_can_swap_pins: with
 * fast_inlatch, the buried functions of the two cells must not be
 * fast-inlatch nodes. */
static int alloc_swap_inlatch_ok(FitEntity *ent, FitSlot *a, FitSlot *b)
{
    if (g_opt.fast_inlatch == 0)
        return 1;
    if (util_str_icmp(a->buried, FIT_FREE_NAME) != 0
        && fit_drv_fast_inlatch_index(alloc_fct_of_signal(ent, a->buried)->var_id) != 0)
        return 0;
    if (util_str_icmp(b->buried, FIT_FREE_NAME) != 0
        && fit_drv_fast_inlatch_index(alloc_fct_of_signal(ent, b->buried)->var_id) != 0)
        return 0;
    return 1;
}

/* May the buried signals of the two nodes change places? */
static int alloc_can_swap_feedback(FitEntity *ent, char *name, char *other)
{
    FitSlot *a, *b;

    TRACE("alloc_can_swap_feedback %.7s %.7s\n", name, other);
    if (g_opt.logic_doubling == 0) {
        a = alloc_slot(name);
        if (a->fct_id != 0)
            return 0;
        b = alloc_slot(other);
        if (b->fct_id != 0)
            return 0;
        if (alloc_swap_terms_ok(ent, a, b) == 0)
            return 0;
        if (util_str_icmp(a->cascade, FIT_FREE_NAME) != 0)
            return 0;
        if (util_str_icmp(b->cascade, FIT_FREE_NAME) != 0)
            return 0;
    } else {
        a = alloc_slot(name);
        b = alloc_slot(other);
        if (util_str_icmp(a->cascade, FIT_FREE_NAME) != 0)
            return 0;
        if (util_str_icmp(b->cascade, FIT_FREE_NAME) != 0)
            return 0;
        if (alloc_swap_terms_ok_doubled(ent, a, b) == 0)
            return 0;
    }
    if (list_find(g_fit_global_names, a->buried, place_str_ieq) != 0)
        return 0;
    if (list_find(g_fit_global_names, b->buried, place_str_ieq) != 0)
        return 0;
    if (route_is_preassigned(ent, a->buried))
        return 0;
    if (route_is_preassigned(ent, b->buried))
        return 0;
    return alloc_swap_inlatch_ok(ent, a, b);
}

/* The pin functions of both cells fit into five terms next to
 * the cell's foldback. */
static int alloc_swap_terms_ok_pins(FitEntity *ent, FitSlot *a, FitSlot *b)
{
    FitFct *fct;
    int ea = alloc_foldback_term(a);
    int eb = alloc_foldback_term(b);

    if (a->fct_id != 0) {
        fct = entity_find_fct(ent, a->fct_id);
        if (fct == NULL)
            place_trap("alloc_swap_terms_ok_pins: variable without function");
        if (5 < route_fct_terms(ent, fct) + ea)
            return 0;
    }
    if (b->fct_id != 0) {
        fct = entity_find_fct(ent, b->fct_id);
        if (fct == NULL)
            place_trap("alloc_swap_terms_ok_pins: variable without function");
        if (5 < route_fct_terms(ent, fct) + eb)
            return 0;
    }
    return 1;
}

/* May the pin signals of the two nodes change places? */
static int alloc_can_swap_pins(FitEntity *ent, char *name, char *other)
{
    FitSlot *a, *b;

    TRACE("alloc_can_swap_pins %.7s %.7s\n", name, other);
    a = alloc_slot(name);
    b = alloc_slot(other);
    if (g_opt.logic_doubling == 0) {
        if (b->pin == 0)
            return 0;
        if (a->fct_id != 0 && util_str_icmp(b->buried, FIT_FREE_NAME) != 0)
            return 0;
        if (b->fct_id != 0 && util_str_icmp(a->buried, FIT_FREE_NAME) != 0)
            return 0;
        if (alloc_swap_terms_ok_pins(ent, a, b) == 0)
            return 0;
        if (a->fct_id != 0 || b->fct_id != 0) {
            if (util_str_icmp(a->cascade, FIT_FREE_NAME) != 0)
                return 0;
            if (util_str_icmp(b->cascade, FIT_FREE_NAME) != 0)
                return 0;
        }
    } else {
        if (alloc_swap_terms_ok_doubled(ent, a, b) == 0)
            return 0;
    }
    if (list_find(g_fit_global_names, a->signal, place_str_ieq) != 0)
        return 0;
    if (list_find(g_fit_global_names, b->signal, place_str_ieq) != 0)
        return 0;
    if (route_is_preassigned(ent, a->signal))
        return 0;
    if (route_is_preassigned(ent, b->signal))
        return 0;
    return alloc_swap_inlatch_ok(ent, a, b);
}

/* Tries to move the signal on node `name` (cluster cl of block,
 * 0-based) to a node of another, smaller cluster's class that the block
 * does not use yet: same block letter, same kind (pin / feedback). */
static int alloc_try_move_name(FitEntity *ent, char *name, FitList *all, FitList *nodes,
                               int cl, int block)
{
    FitList *bl = PLACE_LIST(all, block);
    FitList *other_cluster, *class_nodes;
    char *cand;
    size_t len = strlen(name);
    int feedback, k, j, ok;

    TRACE("alloc_try_move_name %.7s -\n", name);
    if (len == 0)
        return 0;
    feedback = name[len - 1] == 'b';
    for (k = 0; k < PLACE_COUNT(nodes); k++) {
        if (k == cl)
            continue;
        other_cluster = PLACE_LIST(bl, k);
        if (PLACE_COUNT(other_cluster) >= alloc_cluster_limit())
            continue;
        class_nodes = PLACE_LIST(nodes, k);
        for (j = 0; j < PLACE_COUNT(class_nodes); j++) {
            cand = (char *)class_nodes->items[j];
            if (cand[0] == '\0' || cand[strlen(cand) - 1] != name[len - 1] || name[0] != cand[0])
                continue;
            if (list_find(other_cluster, cand, place_str_ieq) != 0)
                continue;
            ok = feedback ? alloc_can_swap_feedback(ent, name, cand)
                          : alloc_can_swap_pins(ent, name, cand);
            if (ok == 0 || !alloc_swap_improves(all, name, cand, cl, k))
                continue;
            ok = feedback ? alloc_swap_feedback(ent, name, cand)
                          : alloc_swap_pins(ent, name, cand);
            if (ok != 0) {
                alloc_swap_names(all, name, cand, cl, k);
                return 1;
            }
        }
    }
    return 0;
}

/* Moves names out of cluster cl of block until it is within the
 * cluster limit or no name can be moved. */
static void alloc_relieve_cluster(FitEntity *ent, FitList *all, FitList *nodes, int cl, int block)
{
    FitList *c = PLACE_LIST(PLACE_LIST(all, block), cl);
    int i;

    TRACE("alloc_relieve_cluster %d %d\n", cl, block);
    for (i = 0; i < PLACE_COUNT(c); i++) {
        if (alloc_try_move_name(ent, (char *)c->items[i], all, nodes, cl, block) != 0) {
            if (PLACE_COUNT(c) <= alloc_cluster_limit())
                return;
            i--;
        }
    }
}

/* A free block input of block that can select node. */
static FitUimSlot *alloc_free_candidate(const char *node, int block)
{
    FitList *l = alloc_uim_candidates(node, block);
    FitUimSlot *u, *found = NULL;
    int i;

    TRACE("alloc_free_candidate %.7s %d\n", node, block);
    for (i = 0; i < PLACE_COUNT(l); i++) {
        u = (FitUimSlot *)l->items[i];
        if (util_str_icmp(u->signal, FIT_FREE_NAME) == 0) {
            found = u;
            break;
        }
    }
    list_destroy(&l);
    return found;
}

/* Finds a block input for node by pushing the node that holds
 * one of its candidates to another input, recursively (at most 50 calls
 * in all). */
static int alloc_reroute_name(char *node, int block)
{
    FitList *l;
    FitUimSlot *u, *spare;
    char *old, *signal;
    int i;

    TRACE("alloc_reroute_name %.7s %d\n", node, block);
    g_alloc_swap_depth++;
    if (g_alloc_swap_depth >= 0x33)
        return 0;
    l = alloc_uim_candidates(node, block);
    for (i = 0; i < PLACE_COUNT(l); i++) {
        u = (FitUimSlot *)l->items[i];
        old = u->sel_node;
        spare = alloc_free_candidate(old, block);
        if (spare != NULL) {
            spare->sel_node = util_strdup(old);
            spare->signal = util_strdup(u->signal);
            u->sel_node = util_strdup(node);
            place_slot_of_node(node, &signal);
            u->signal = signal;
            return 1;
        }
    }
    for (i = 0; i < PLACE_COUNT(l); i++) {
        u = (FitUimSlot *)l->items[i];
        if (alloc_reroute_name(u->sel_node, block) != 0) {
            u->sel_node = node;
            place_slot_of_node(node, &signal);
            u->signal = signal;
            return 1;
        }
    }
    return 0;
}

/* Names of a block whose decided block input is already taken
 * by an earlier name. */
static FitList *alloc_doubly_routed(FitList *cands, FitList *names)
{
    FitList *res = list_create();
    FitList *seen = list_create();
    FitList *c;
    FitUimSlot *u;
    int i;

    for (i = 0; i < PLACE_COUNT(cands); i++) {
        c = PLACE_LIST(cands, i);
        if (c == NULL || c->count == 0)
            continue;       /* no candidate */
        u = (FitUimSlot *)c->items[0];
        if (util_str_icmp(u->signal, FIT_FREE_NAME) != 0) {
            if (list_find(seen, u, place_uim_index_eq) != 0)
                list_add(res, names->items[i]);
            else
                list_add(seen, u);
        }
    }
    list_destroy(&seen);
    return res;
}

/* Reroutes, one by one, the names of a block whose candidate input is
 * taken twice (alloc_reroute_name); sets g_fit_route_failed for each one
 * that cannot be moved. */
static void alloc_reroute_block(FitList *cands, FitList *names, int block)
{
    FitList *l = alloc_doubly_routed(cands, names);
    int i = 0, pos = 1;

    while (i < PLACE_COUNT(l)) {
        if (alloc_reroute_name((char *)l->items[i], block) == 0) {
            i++;
            g_fit_route_failed = 1;
            pos++;
        } else {
            list_del_shift(l, pos);
        }
    }
}

/* alloc_reroute_block for every block, g_fit_route_failed cleared first. */
static void alloc_reroute_all(FitList *cands, FitList *names)
{
    int b;

    TRACE("alloc_reroute_all\n");
    g_fit_route_failed = 0;
    for (b = 0; b < g_dev->n_blocks; b++)
        alloc_reroute_block(PLACE_LIST(cands, b), PLACE_LIST(names, b), b);
}

/* Index of the first cluster above the cluster limit. */
static int alloc_find_big_cluster(FitList *clusters, int *index)
{
    int i;

    for (i = 0; i < PLACE_COUNT(clusters); i++) {
        if (alloc_cluster_limit() < PLACE_COUNT(PLACE_LIST(clusters, i))) {
            *index = i;
            return 1;
        }
    }
    return 0;
}

/* Merges the clusters of one block into one cluster per node
 * class (a cluster belongs to the class that holds its first name).  NULL
 * when a class has no cluster. */
static FitList *alloc_merge_by_class(FitList *clusters, FitList *nodes)
{
    FitList *res = list_create();
    FitList *class_nodes, *merged, *c;
    int k, i, pos;

    for (k = 0; k != PLACE_COUNT(nodes); k++) {
        merged = NULL;
        class_nodes = PLACE_LIST(nodes, k);
        i = 0;
        pos = 1;
        while (i < PLACE_COUNT(clusters)) {
            c = PLACE_LIST(clusters, i);
            if (PLACE_COUNT(c) != 0
                && list_find(class_nodes, c->items[0], place_str_ieq) != 0) {
                if (merged != NULL)
                    c = list_union(c, merged, place_str_ieq);
                list_del_shift(clusters, pos);
                i--;
                pos--;
                merged = c;
            }
            i++;
            pos++;
        }
        if (merged == NULL)
            return NULL;
        list_add(res, merged);
    }
    return res;
}

/* alloc_merge_by_class for every block; 0 when one fails,
 * else the number of blocks. */
static int alloc_merge_all_by_class(FitList *all, FitList *nodes)
{
    FitList *merged;
    int pos;

    TRACE("alloc_merge_all_by_class\n");
    for (pos = 1; pos - 1 < PLACE_COUNT(all); pos++) {
        merged = alloc_merge_by_class(PLACE_LIST(all, pos - 1), nodes);
        if (merged == NULL)
            return 0;
        list_del_shift(all, pos);
        list_insert_at(all, merged, pos);
    }
    return PLACE_COUNT(all);
}

/* Balances the UIM clusters by moving signals between
 * macrocells, then routes everything again.  names: per block the node
 * names.  Returns 1 when the routing succeeded, 0 when it failed or the
 * clusters could not be formed.  Leaves the new lists in g_route_*. */
int alloc_rebalance(FitEntity *ent, FitList *names)
{
    FitList *all = list_create();
    FitList *nodes = dev_uim_node_lists();
    FitList *dummy, *macro_names, *pin_names, *cands, *failed;
    int b, cl, excess, n, saved_doubling;

    for (b = 0; b < PLACE_COUNT(names); b++)
        list_add(all, alloc_block_clusters(ent, PLACE_LIST(names, b), b, &dummy));
    if (alloc_merge_all_by_class(all, nodes) == 0)
        return 0;
    excess = alloc_excess_total(all);
    saved_doubling = g_opt.logic_doubling;
    g_opt.logic_doubling = 0;
    while (excess != 0) {
        for (b = 0; b < PLACE_COUNT(all); b++) {
            if (alloc_find_big_cluster(PLACE_LIST(all, b), &cl) != 0)
                alloc_relieve_cluster(ent, all, nodes, cl, b);
        }
        n = alloc_excess_total(all);
        if (excess <= n) {
            excess = n;
            if (g_opt.logic_doubling != 0)
                break;
            g_opt.logic_doubling = 1;
        } else {
            excess = n;
        }
    }
    g_fit_route_failed = 0;
    g_opt.logic_doubling = saved_doubling;
    route_clear_uim();
    macro_names = place_block_node_names(ent);
    pin_names = route_block_pin_names(ent);
    names = list_create();
    for (b = 0; b < g_dev->n_blocks; b++)
        list_add(names, list_union(PLACE_LIST(macro_names, b), PLACE_LIST(pin_names, b), place_str_ieq));
    all = list_create();
    for (b = 0; b < PLACE_COUNT(names); b++)
        list_add(all, alloc_block_clusters(ent, PLACE_LIST(names, b), b, &dummy));
    excess = alloc_excess_total(all);
    cands = place_assign_uim(names);
    failed = list_create();
    route_commit(ent, cands, names, failed);
    if (g_fit_route_failed != 0 && excess == 0)
        alloc_reroute_all(cands, names);
    g_route_names = names;
    g_route_cands = cands;
    g_route_failed_ids = failed;
    return g_fit_route_failed == 0;
}
