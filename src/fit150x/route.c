/*
 * route.c - the router of the ATF150x fitter: writing the UIM assignment
 * into the block inputs, and the repair of a failed routing by moving or
 * exchanging signals between macrocells and pins.
 *
 * Memory: an owned string of a slot that is overwritten here is not freed
 * (the slots' "--" strings are partly static, partly owned, so they cannot
 * be told apart).
 */
#include "place_priv.h"

/* Trace of the repair steps on stderr, compiled in with -DPLACE_TRACE only. */
#ifdef PLACE_TRACE
#define TRACE(...) fprintf(stderr, __VA_ARGS__)
#else
#define TRACE(...) ((void)0)
#endif

static int route_count_failed(FitList *failed);

/* Macrocells needed for n_pt product terms: ceil(n_pt / 5). */
int route_mc_for_terms(int n_pt)
{
    int n = n_pt / 5;

    if (n_pt % 5 != 0)
        n++;
    return n;
}

/* Body of route_commit and route_commit_reverse for one name of one
 * block.  Returns 1 when the name failed. */
static int route_commit_name(FitEntity *ent, FitList *block_cands, FitList *block_names,
                             int i, int block, FitList *failed_ids, int reverse)
{
    char *node = (char *)block_names->items[i];
    char *signal = NULL;
    char *src;
    FitList *c;
    FitUimSlot *u;

    place_slot_of_node(node, &signal);
    c = PLACE_LIST(block_cands, i);
    if (c == NULL || c->count != 1) {
        fprintf(g_msg, "Routage Fail for Pin : %s in Block %d\n", node, block + 1);
        g_fit_route_failed = 1;
        return 1;
    }
    u = (FitUimSlot *)c->items[0];
    if (util_str_icmp(u->signal, FIT_FREE_NAME) == 0) {
        u->sel_node = util_strdup(node);
        src = util_strdup(latch_signal_source(ent, signal));
        u->signal = src;
        if (g_equiv_verbose != 0)
            printf("Variable %s  routed  with UIM%d in Block %d\n", src, u->index, block + 1);
        return 0;
    }
    if (g_fit_report_failures != 0) {
        src = latch_signal_source(ent, signal);
        if (reverse)
            printf("Fail to route variable %s in Block %d\n", src, block + 1);
        else
            fprintf(g_fit_report, "Fail to route variable %s in Block %d\n", src, block + 1);
    }
    g_fit_route_failed = 1;
    list_add(failed_ids, FIT_INT2PTR(entity_var_id_by_name(ent, signal)));
    return 1;
}

static int route_commit_any(FitEntity *ent, FitList *cands, FitList *names, FitList *failed, int reverse)
{
    FitList *bc, *bn, *ids;
    int b, i, n_failed;

    for (b = 0; b < g_dev->n_blocks; b++) {
        bn = PLACE_LIST(names, b);
        bc = PLACE_LIST(cands, b);
        if (g_equiv_verbose != 0) {
            printf("#################################\n");
            printf("#####  Routing in Block %d  #####\n", b + 1);
            printf("#################################\n");
        }
        ids = list_create();
        n_failed = 0;
        if (PLACE_COUNT(bn) != PLACE_COUNT(bc)) {
            fprintf(g_msg, "Routing Failed ...\n");
            return 0;
        }
        if (reverse) {
            for (i = PLACE_COUNT(bc) - 1; i >= 0; i--)
                n_failed += route_commit_name(ent, bc, bn, i, b, ids, 1);
        } else {
            for (i = 0; i < PLACE_COUNT(bc); i++)
                n_failed += route_commit_name(ent, bc, bn, i, b, ids, 0);
        }
        if (g_equiv_verbose != 0)
            printf("%d Variable Fail sur %d  in Block %d\n", n_failed, PLACE_COUNT(bc), b + 1);
        list_add(failed, ids);
    }
    return g_dev->n_blocks;
}

int route_commit(FitEntity *ent, FitList *cands, FitList *names, FitList *failed)
{
    return route_commit_any(ent, cands, names, failed, 0);
}

/* route_commit with the names of each block taken last to first;
 * its "Fail to route variable" line goes to stdout. */
static int route_commit_reverse(FitEntity *ent, FitList *cands, FitList *names, FitList *failed)
{
    TRACE("route_commit_reverse\n");
    return route_commit_any(ent, cands, names, failed, 1);
}

/* Block (1-based) in which var is placed according to
 * g_fit_block_placed; 0 for an input that is no netlist signal or when not found. */
static int route_block_of_var(FitEntity *ent, FitVariable *var)
{
    FitInstance *inst;
    int kind, id, b;

    if (entity_has_input_id(ent, var->id) == 0) {
        if (netlist_has_name(ent, var->name) == 0) {
            id = var->id;
        } else {
            inst = tt3_find_instance_by_alias(ent, var->name, &kind);
            if (inst == NULL)
                place_trap("route_block_of_var: no instance for the alias");
            id = entity_var_id_by_name(ent, inst->q);
        }
    } else {
        if (latch_find_alias(var->name, ent->net) == 0)
            return 0;
        inst = tt3_find_instance_by_input(ent, var->name, &kind);
        if (inst == NULL)
            id = kind;      /* 0 when no instance was found */
        else
            id = entity_var_id_by_name(ent, inst->q);
    }
    for (b = 0; b < g_dev->n_blocks; b++) {
        if (list_find(PLACE_LIST(g_fit_block_placed, b), FIT_INT2PTR(id), list_ptr_eq) != 0)
            return b + 1;
    }
    return 0;
}

/* Name of dedicated input slot i (index into g_fit_slots). */
static const char *route_dedicated_name(int i)
{
    if (i == g_dev->n_macrocells)
        return "OE2";
    if (i == g_dev->n_macrocells + 1)
        return "GCLK";
    if (i == g_dev->n_macrocells + 2)
        return "GCLR";
    return "OE1";
}

FitSlot *route_node_of_name(const char *name, char **node)
{
    FitSlot *slot;
    int i;

    for (i = 0; i < g_dev->n_macrocells; i++) {
        slot = PLACE_SLOT(i);
        if (util_str_icmp(slot->signal, name) == 0) {
            *node = place_node_name(slot, "p");
            return slot;
        }
        if (util_str_icmp(slot->buried, name) == 0) {
            *node = place_node_name(slot, "fb");
            return slot;
        }
    }
    for (i = g_dev->n_macrocells; i < g_dev->n_ded_inputs + g_dev->n_macrocells; i++) {
        slot = PLACE_SLOT(i);
        if (util_str_icmp(slot->signal, name) == 0) {
            *node = util_strdup(route_dedicated_name(i));
            return slot;
        }
    }
    return NULL;
}

/* Takes the ids of a and b (each may be NULL) out of every
 * block's list of failed ids. */
static void route_forget_failed(FitList *failed, FitVariable *a, FitVariable *b)
{
    int ida = a != NULL ? a->id : 0;
    int idb = b != NULL ? b->id : 0;
    int blk, k;

    for (blk = 0; blk < g_dev->n_blocks; blk++) {
        k = list_find(PLACE_LIST(failed, blk), FIT_INT2PTR(ida), list_ptr_eq);
        if (k != 0)
            list_del_shift(PLACE_LIST(failed, blk), k);
        k = list_find(PLACE_LIST(failed, blk), FIT_INT2PTR(idb), list_ptr_eq);
        if (k != 0)
            list_del_shift(PLACE_LIST(failed, blk), k);
    }
}

/* 1 when block (0-based) has a free block input that can select node. */
static int route_block_can_select(const char *node, int block)
{
    FitList *c = place_uim_free_candidates(node, block);
    int ok = c != NULL && c->count != 0;

    list_destroy(&c);
    return ok;
}

/* 1 when every block that needs variable id has a free block
 * input for `node`. */
static int route_node_reaches_users(const char *node, int id)
{
    int b;

    TRACE("route_node_reaches_users %.7s %d\n", node, id);
    for (b = 0; b < g_dev->n_blocks; b++) {
        if (list_find(PLACE_LIST(g_fit_block_fanin, b), FIT_INT2PTR(id), list_ptr_eq) != 0
            && !route_block_can_select(node, b))
            return 0;
    }
    return 1;
}

/* Frees every block input again. */
void route_clear_uim(void)
{
    FitList *l;
    FitUimSlot *u;
    int b, j;

    for (b = 0; b < g_dev->n_blocks; b++) {
        l = PLACE_LIST(g_fit_uim_slots, b);
        for (j = 0; j < PLACE_COUNT(l); j++) {
            u = (FitUimSlot *)l->items[j];
            if (util_str_icmp(u->signal, FIT_FREE_NAME) != 0) {
                free(u->signal);
                u->signal = util_strdup(FIT_FREE_NAME);
                free(u->sel_node);
                u->sel_node = util_strdup(FIT_FREE_NAME);
            }
        }
    }
}

/* For every block the node names of the pin signals without
 * logic (inputs) the block needs: macrocell pins in macrocell order, then
 * the dedicated inputs. */
FitList *route_block_pin_names(FitEntity *ent)
{
    FitList *res = list_create();
    FitSlot *slot;
    char *node;
    int i, b, id;

    for (b = 0; b < g_dev->n_blocks; b++)
        list_add(res, list_create());
    for (i = 0; i < g_dev->n_ded_inputs + g_dev->n_macrocells; i++) {
        slot = PLACE_SLOT(i);
        if (util_str_icmp(slot->signal, FIT_FREE_NAME) == 0 || slot->fct_id != 0)
            continue;
        if (i >= g_dev->n_macrocells && i <= g_dev->n_macrocells + 3)
            node = util_strdup(route_dedicated_name(i));
        else
            node = place_node_name(slot, "p");
        id = entity_var_id_by_name(ent, slot->signal);
        for (b = 0; b < g_dev->n_blocks; b++) {
            if (list_find(PLACE_LIST(g_fit_block_fanin, b), FIT_INT2PTR(id), list_ptr_eq) != 0)
                list_add(PLACE_LIST(res, b), node);
        }
    }
    return res;
}

/* Test of route_find_free_cell / route_find_free_pin: can the users of
 * id be reached from the node <slot><suffix>? */
static int route_slot_reaches_users(FitSlot *slot, const char *suffix, int id)
{
    char *node = place_node_name(slot, suffix);
    int ok = route_node_reaches_users(node, id);

    free(node);
    return ok;
}

/* A macrocell of block (1-based) without logic, buried signal or
 * lent terms to which the function with source id src_id can move, tested
 * through its pin node (mode 1) or feedback node; scanned from the top of
 * the block.
 * mode 1 (output): the pin must exist and be free.  mode 5 (node): first
 * cells without pin or whose pin is taken; then, as for any other mode,
 * every such cell. */
static FitSlot *route_find_free_cell(int block, int n_pt, int mode, int src_id)
{
    int hi = block * 16 - 1;
    int lo = block * 16 - 16;
    FitSlot *slot;
    int i;

    (void)n_pt;
    for (i = hi; i >= lo; i--) {
        slot = PLACE_SLOT(i);
        if (slot->fct_id != 0
            || util_str_icmp(slot->buried, FIT_FREE_NAME) != 0
            || util_str_icmp(slot->cascade, FIT_FREE_NAME) != 0)
            continue;
        if (mode == 1) {
            if (util_str_icmp(slot->signal, FIT_FREE_NAME) != 0)
                continue;
            if (slot->pin == 0)
                continue;
        } else if (mode == 5 && slot->pin != 0) {
            if (util_str_icmp(slot->signal, FIT_FREE_NAME) == 0)
                continue;
        }
        if (route_slot_reaches_users(slot, mode == 1 ? "p" : "fb", src_id))
            return slot;
    }
    if (mode != 1) {
        for (i = hi; i >= lo; i--) {
            slot = PLACE_SLOT(i);
            if (slot->fct_id == 0
                && util_str_icmp(slot->buried, FIT_FREE_NAME) == 0
                && util_str_icmp(slot->cascade, FIT_FREE_NAME) == 0) {
                if (route_slot_reaches_users(slot, "fb", src_id))
                    return slot;
            }
        }
    }
    return NULL;
}

/* A free pin from which the users of input id can be reached:
 * macrocell pins without buried signal, then any free macrocell pin, then
 * the dedicated inputs. */
static FitSlot *route_find_free_pin(int id)
{
    FitSlot *slot;
    int i;

    for (i = 0; i < g_dev->n_macrocells; i++) {
        slot = PLACE_SLOT(i);
        if (slot->pin != 0
            && util_str_icmp(slot->signal, FIT_FREE_NAME) == 0
            && util_str_icmp(slot->buried, FIT_FREE_NAME) == 0) {
            if (route_slot_reaches_users(slot, "p", id))
                return slot;
        }
    }
    for (i = 0; i < g_dev->n_macrocells; i++) {
        slot = PLACE_SLOT(i);
        if (slot->pin != 0 && util_str_icmp(slot->signal, FIT_FREE_NAME) == 0) {
            if (route_slot_reaches_users(slot, "p", id))
                return slot;
        }
    }
    for (i = g_dev->n_macrocells; i < g_dev->n_ded_inputs + g_dev->n_macrocells; i++) {
        slot = PLACE_SLOT(i);
        if (slot->pin != 0 && util_str_icmp(slot->signal, FIT_FREE_NAME) == 0) {
            if (route_node_reaches_users(route_dedicated_name(i), id))
                return slot;
        }
    }
    return NULL;
}

/* Moves input var to another free pin. */
static int route_move_input(FitEntity *ent, FitVariable *var, int block, FitList *failed)
{
    FitSlot *dst, *cur;
    char *node;

    TRACE("route_move_input\n");
    (void)ent;
    (void)block;
    dst = route_find_free_pin(var->id);
    if (dst == NULL)
        return 0;
    cur = route_node_of_name(var->name, &node);
    if (cur == NULL)
        place_trap("route_move_input: signal not placed in a macrocell");
    cur->signal = util_strdup(FIT_FREE_NAME);
    dst->signal = util_strdup(var->name);
    route_forget_failed(failed, var, NULL);
    return 1;
}

/* Exchanges the pins of inputs a (which failed in `block`,
 * 0-based) and b, if b is not needed by that block and both stay routable
 * in all blocks that need only one of them. */
static int route_swap_inputs(FitEntity *ent, FitVariable *a, FitVariable *b, FitList *names, int block)
{
    FitSlot *slot_a, *slot_b;
    char *node_a, *node_b;
    int bl;

    TRACE("route_swap_inputs\n");
    (void)ent;
    (void)names;
    if (list_find(PLACE_LIST(g_fit_block_fanin, block), FIT_INT2PTR(b->id), list_ptr_eq) != 0)
        return 0;
    slot_b = route_node_of_name(b->name, &node_b);
    if (slot_b == NULL)
        place_trap("route_swap_inputs: signal not placed in a macrocell");
    if (!route_block_can_select(node_b, block))
        return 0;
    for (bl = 0; bl < g_dev->n_blocks; bl++) {
        if (bl != block
            && list_find(PLACE_LIST(g_fit_block_fanin, bl), FIT_INT2PTR(a->id), list_ptr_eq) != 0
            && list_find(PLACE_LIST(g_fit_block_fanin, bl), FIT_INT2PTR(b->id), list_ptr_eq) == 0) {
            if (!route_block_can_select(node_b, bl))
                return 0;
        }
    }
    slot_a = route_node_of_name(a->name, &node_a);
    if (slot_a == NULL)
        place_trap("route_swap_inputs: signal not placed in a macrocell");
    for (bl = 0; bl < g_dev->n_blocks; bl++) {
        if (list_find(PLACE_LIST(g_fit_block_fanin, bl), FIT_INT2PTR(b->id), list_ptr_eq) != 0
            && list_find(PLACE_LIST(g_fit_block_fanin, bl), FIT_INT2PTR(a->id), list_ptr_eq) == 0) {
            if (!route_block_can_select(node_a, bl))
                return 0;
        }
    }
    slot_a->signal = util_strdup(b->name);
    slot_b->signal = util_strdup(a->name);
    return 1;
}

/* 1 when preassignments are honoured and the source of `name`
 * is preassigned. */
int route_is_preassigned(FitEntity *ent, char *name)
{
    if (g_fit_preassign_used == 0)
        return 0;
    return list_find(g_preassign, latch_signal_source(ent, name), place_name_eq) != 0;
}

/* Tries to exchange failed input var with one of the inputs of
 * g_fit_input_ids. */
static int route_try_swap_input(FitEntity *ent, FitList *failed, FitVariable *var, FitList *names, int block)
{
    FitVariable *other;
    int i, id;

    TRACE("route_try_swap_input\n");
    for (i = 0; i < PLACE_COUNT(g_fit_input_ids); i++) {
        id = PLACE_ID(g_fit_input_ids, i);
        if (id == var->id)
            continue;
        if (list_find(PLACE_LIST(failed, block), FIT_INT2PTR(id), list_ptr_eq) != 0)
            continue;
        other = entity_var_by_id(ent, id);
        if (other == NULL)
            place_trap("route_try_swap_input: no variable with this id");
        if (list_find(g_fit_global_names, other->name, place_str_ieq) == 0
            && !route_is_preassigned(ent, other->name)
            && route_swap_inputs(ent, var, other, names, block) != 0) {
            route_forget_failed(failed, var, other);
            return 1;
        }
    }
    return 0;
}

/* Product terms of fct in a macrocell: terms of its expression
 * (or of the XOR form for the xor_synthesis functions) plus the control
 * terms of its register.  Returns 0 for an xor function without the XOR
 * form. */
int route_fct_terms(FitEntity *ent, FitFct *fct)
{
    FitList *ids = list_create();
    FitVariable *var;
    int n, extra = 0;

    if (list_find(g_opt.xor_list, FIT_INT2PTR(fct->var_id), list_ptr_eq) == 0) {
        place_collect_inputs(ent, fct->tree[0], ids);
        n = place_count_terms(ent, fct);
    } else {
        n = place_xor_terms(ent, ids, fct);
        if (n == 0) {
            list_destroy(&ids);
            return 0;
        }
    }
    var = entity_var_by_id(ent, fct->var_id);
    if (var == NULL)
        place_trap("route_fct_terms: no variable with this id");
    if (latch_find_alias(var->name, ent->net) != 0)
        extra = fit_drv_count_ctrl_terms(ent, ids, var->name);
    list_destroy(&ids);
    return n + extra;
}

/* Id of the signal that enters the UIM for var: var itself, or
 * for a register output the register's d (else pad) input. */
static int route_source_id(FitEntity *ent, FitVariable *var)
{
    FitInstance *inst;
    int kind;

    if (netlist_has_name(ent, var->name) == 0)
        return var->id;
    inst = tt3_find_instance_by_alias(ent, var->name, &kind);
    if (inst == NULL)
        place_trap("route_source_id: no instance for the alias");
    if (inst->d == NULL)
        return entity_var_id_by_name(ent, inst->pin);
    return entity_var_id_by_name(ent, inst->d);
}

/* Moves the single-macrocell function of var to a free
 * macrocell of its block. */
static int route_move_node(FitEntity *ent, FitList *failed, FitVariable *var)
{
    FitFct *fct;
    FitSlot *dst, *cur;
    char *node, *old_foldback;
    int n_pt, block, is_node, src;

    TRACE("route_move_node\n");
    fct = place_fct_of_var(ent, var);
    if (fct == NULL)
        place_trap("route_move_node: variable without function");
    n_pt = route_fct_terms(ent, fct);
    if (1 < route_mc_for_terms(n_pt))
        return 0;
    block = route_block_of_var(ent, var);
    if (block == 0)
        return 0;
    is_node = entity_var_is_node(ent, var);
    src = route_source_id(ent, var);
    dst = route_find_free_cell(block, n_pt, is_node != 0 ? 5 : 1, src);
    if (dst == NULL)
        return 0;
    cur = route_node_of_name(var->name, &node);
    if (cur == NULL)
        place_trap("route_move_node: signal not placed in a macrocell");
    old_foldback = dst->foldback;
    if (cur->fct_id == 0) {
        cur->buried = util_strdup(FIT_FREE_NAME);
        dst->buried = util_strdup(var->name);
    } else {
        dst->fct_id = cur->fct_id;
        cur->fct_id = 0;
        cur->signal = util_strdup(FIT_FREE_NAME);
        dst->signal = util_strdup(var->name);
    }
    dst->fn_kind = util_strdup(cur->fn_kind);
    cur->fn_kind = util_strdup(FIT_FREE_NAME);
    dst->foldback = util_strdup(cur->foldback);
    cur->foldback = util_strdup(old_foldback);
    route_forget_failed(failed, var, NULL);
    return 1;
}

/* Exchanges the macrocells of the single-macrocell functions a
 * (failed in `block`, 0-based) and b. */
static int route_swap_nodes(FitEntity *ent, FitVariable *a, FitVariable *b, FitList *names, int block)
{
    FitFct *fa, *fb;
    FitSlot *slot_a, *slot_b;
    char *node_a, *node_b, *s20a, *s1ca, *s1cb;
    int na, nb, src_a, src_b, bl, i04a;

    TRACE("route_swap_nodes\n");
    (void)names;
    fa = place_fct_of_var(ent, a);
    fb = place_fct_of_var(ent, b);
    if (fa == NULL || fb == NULL)
        place_trap("route_swap_nodes: variable without function");
    if (g_opt.fast_inlatch != 0 && fit_drv_fast_inlatch_index(fb->var_id) != 0)
        return 0;
    na = route_fct_terms(ent, fa);
    nb = route_fct_terms(ent, fb);
    na = route_mc_for_terms(na);
    nb = route_mc_for_terms(nb);
    if (1 < na)
        return 0;
    if (na != nb)
        return 0;
    src_a = route_source_id(ent, a);
    src_b = route_source_id(ent, b);
    if (list_find(PLACE_LIST(g_fit_block_fanin, block), FIT_INT2PTR(src_b), list_ptr_eq) != 0)
        return 0;
    slot_b = route_node_of_name(b->name, &node_b);
    if (slot_b == NULL)
        place_trap("route_swap_nodes: signal not placed in a macrocell");
    if (g_opt.logic_doubling != 0 && slot_b->fct_id != 0
        && util_str_icmp(slot_b->buried, FIT_FREE_NAME) != 0)
        return 0;
    if (!route_block_can_select(node_b, block))
        return 0;
    for (bl = 0; bl < g_dev->n_blocks; bl++) {
        if (bl != block
            && list_find(PLACE_LIST(g_fit_block_fanin, bl), FIT_INT2PTR(src_a), list_ptr_eq) != 0
            && list_find(PLACE_LIST(g_fit_block_fanin, bl), FIT_INT2PTR(src_b), list_ptr_eq) == 0) {
            if (!route_block_can_select(node_b, bl))
                return 0;
        }
    }
    slot_a = route_node_of_name(a->name, &node_a);
    if (slot_a == NULL)
        place_trap("route_swap_nodes: signal not placed in a macrocell");
    for (bl = 0; bl < g_dev->n_blocks; bl++) {
        if (list_find(PLACE_LIST(g_fit_block_fanin, bl), FIT_INT2PTR(src_b), list_ptr_eq) != 0
            && list_find(PLACE_LIST(g_fit_block_fanin, bl), FIT_INT2PTR(src_a), list_ptr_eq) == 0) {
            if (!route_block_can_select(node_a, bl))
                return 0;
        }
    }
    i04a = slot_a->fct_id;
    s20a = slot_a->fn_kind;
    s1ca = slot_a->foldback;
    if (i04a == 0) {
        slot_a->buried = util_strdup(b->name);
        slot_b->buried = util_strdup(a->name);
    } else {
        slot_a->fct_id = slot_b->fct_id;
        slot_b->fct_id = i04a;
        slot_a->signal = util_strdup(b->name);
        slot_b->signal = util_strdup(a->name);
    }
    slot_a->fn_kind = util_strdup(slot_b->fn_kind);
    slot_b->fn_kind = util_strdup(s20a);
    s1cb = slot_b->foldback;
    slot_a->foldback = util_strdup(s1cb);
    slot_b->foldback = util_strdup(s1ca);
    return 1;
}

/* Tries to exchange failed function var with another function
 * of its own block that is of the same kind (node / output). */
static int route_try_swap_node(FitEntity *ent, FitList *failed, FitVariable *var, int block, FitList *names)
{
    FitVariable *other;
    FitList *l;
    char *src;
    int is_node, blk, i;

    TRACE("route_try_swap_node\n");
    is_node = entity_var_is_node(ent, var);
    blk = route_block_of_var(ent, var);
    if (blk == 0)
        return 0;
    l = PLACE_LIST(g_fit_block_placed, blk - 1);
    for (i = 0; i < PLACE_COUNT(l); i++) {
        other = entity_var_by_id(ent, PLACE_ID(l, i));
        if (other == NULL)
            place_trap("route_try_swap_node: no variable with this id");
        src = latch_signal_source(ent, other->name);
        if (util_str_icmp(src, other->name) != 0) {
            other = entity_find_var(ent, src);
            if (other == NULL)
                place_trap("route_try_swap_node: signal not in the design");
        }
        if (other->id == var->id)
            continue;
        if (entity_var_is_node(ent, other) != is_node)
            continue;
        if (list_find(PLACE_LIST(failed, block), FIT_INT2PTR(other->id), list_ptr_eq) != 0)
            continue;
        if (route_is_preassigned(ent, other->name))
            continue;
        if (list_find(g_fit_global_names, other->name, place_str_ieq) != 0)
            continue;
        if (route_swap_nodes(ent, var, other, names, block) != 0) {
            route_forget_failed(failed, var, other);
            return 1;
        }
    }
    return 0;
}

/* Tries to cure the failed signal id of `block` (0-based) by
 * moving it or exchanging it with another signal. */
static int route_fix_signal(FitEntity *ent, FitList *failed, int id, int block, FitList *names)
{
    FitVariable *var = entity_var_by_id(ent, id);
    FitFct *fct;
    FitSlot *slot;
    char *node;

    TRACE("route_fix_signal %d %d\n", id, block);
    if (var == NULL)
        place_trap("route_fix_signal: no variable with this id");
    if (list_find(g_fit_global_names, var->name, place_str_ieq) != 0)
        return 0;
    if (route_is_preassigned(ent, var->name))
        return 0;
    if (entity_has_input_id(ent, var->id) != 0
        && latch_find_alias(var->name, ent->net) == 0) {
        if (fit_drv_fast_inlatch_node(var->id) != 0)
            return 0;
        if (route_move_input(ent, var, block, failed) != 0)
            return 1;
        return route_try_swap_input(ent, failed, var, names, block) != 0;
    }
    if (g_opt.fast_inlatch != 0) {
        fct = place_fct_of_var(ent, var);
        if (fct == NULL)
            place_trap("route_fix_signal: variable without function");
        if (fit_drv_fast_inlatch_index(fct->var_id) != 0)
            return 0;
    }
    if (g_opt.logic_doubling != 0) {
        slot = route_node_of_name(var->name, &node);
        if (slot == NULL)
            place_trap("route_fix_signal: signal not placed in a macrocell");
        if (slot->fct_id != 0 && util_str_icmp(slot->buried, FIT_FREE_NAME) != 0)
            return 0;
    }
    if (route_move_node(ent, failed, var) != 0)
        return 1;
    return route_try_swap_node(ent, failed, var, block, names) != 0;
}

/* Works on the failed ids of one block (0-based); on retries
 * (g_fit_retry_count != 0) on the ids of the block's overfull cluster
 * instead.  Sets g_route_progress after the first cure. */
static void route_fix_block(FitEntity *ent, FitList *failed, int block, FitList *names)
{
    FitList *ids = PLACE_LIST(failed, block);
    int i;

    if (g_fit_retry_count != 0 && ids != NULL && ids->count != 0)
        alloc_block_clusters(ent, PLACE_LIST(names, block), block, &ids);
    for (i = 0; i < PLACE_COUNT(ids); i++) {
        if (route_fix_signal(ent, failed, PLACE_ID(ids, i), block, names) != 0) {
            g_route_progress = 1;
            return;
        }
    }
}

/* One repair step. */
static void route_repair_step(FitEntity *ent, FitList *cands, FitList *failed, FitList *names)
{
    FitList *macro_names, *pin_names;
    int b;

    g_route_progress = 0;
    TRACE("route_repair_step\n");
    for (b = 0; b < g_dev->n_blocks; b++) {
        route_fix_block(ent, failed, b, names);
        if (g_route_progress != 0)
            goto reroute;
    }
    list_destroy(&failed);
    failed = list_create();
    route_clear_uim();
    route_commit_reverse(ent, cands, names, failed);
    for (b = 0; b < g_dev->n_blocks; b++) {
        route_fix_block(ent, failed, b, names);
        if (g_route_progress != 0)
            goto reroute;
    }
    g_route_failed_ids = failed;
    g_route_names = names;
    g_route_cands = cands;
    return;

reroute:
    g_fit_route_failed = 0;
    route_clear_uim();
    macro_names = place_block_node_names(ent);
    pin_names = route_block_pin_names(ent);
    names = list_create();
    for (b = 0; b < g_dev->n_blocks; b++)
        list_add(names, list_union(PLACE_LIST(macro_names, b), PLACE_LIST(pin_names, b), place_str_ieq));
    cands = place_assign_uim(names);
    failed = list_create();
    route_commit(ent, cands, names, failed);
    g_route_cands = cands;
    g_route_failed_ids = failed;
    g_route_names = names;
}

/* For an output var (not open collector, no buried signal in its
 * cell, a register only without output enable) whose pin node cannot be
 * selected by `block` (0-based): use the feedback node of its macrocell
 * instead when the block can select that, replacing the name in the
 * block's name list.  Rarely needed on the 160-pin ATF1508. */
static int route_use_feedback_node(FitEntity *ent, FitVariable *var, int block, FitList *block_names)
{
    FitSlot *slot;
    FitInstance *inst;
    char *node, *fb_node;
    int src, kind, k;

    TRACE("route_use_feedback_node %d\n", block);
    slot = route_node_of_name(var->name, &node);
    if (slot == NULL)
        place_trap("route_use_feedback_node: signal not placed in a macrocell");
    if (!(g_opt.open_collector == 0
          || (g_opt.open_collector_list != NULL
              && list_find(g_opt.open_collector_list, slot->signal, place_str_ieq) == 0)))
        return 0;
    if (!(util_str_icmp(slot->buried, FIT_FREE_NAME) == 0 || slot->fct_id == 0))
        return 0;
    src = route_source_id(ent, var);
    if (slot->fct_id == 0)
        return 0;
    if (util_str_icmp(slot->fn_kind, "reg") == 0) {
        inst = tt3_find_instance_by_alias(ent, slot->signal, &kind);
        if (inst == NULL)
            place_trap("route_use_feedback_node: no instance for the alias");
        if (inst->oe != NULL)
            return 0;
    }
    if (list_find(PLACE_LIST(g_fit_block_fanin, block), FIT_INT2PTR(src), list_ptr_eq) == 0)
        return 0;
    if (route_block_can_select(node, block))
        return 0;
    fb_node = place_node_name(slot, slot->fct_id != 0 ? "fb" : "p");
    if (route_block_can_select(fb_node, block)) {
        k = list_find(block_names, node, place_str_ieq);
        if (k != 0) {
            list_del_shift(block_names, k);
            list_insert_at(block_names, fb_node, k);
            return 1;
        }
    }
    return 0;
}

/* For the ids of `block` that still fail after alloc_block_clusters:
 * moves one of them to the feedback node of its macrocell and clusters
 * again, at most eleven rounds. */
static void route_fix_block_by_feedback(FitEntity *ent, FitList *ids, FitList *block_names, int block)
{
    FitVariable *var;
    int n = 0, i;

    TRACE("route_fix_block_by_feedback %d\n", block);
    if (ids == NULL || ids->count == 0)
        return;
    for (;;) {
        alloc_block_clusters(ent, block_names, block, &ids);
        if (ids == NULL || ids->count == 0)
            return;
        if (10 < n)
            return;
        n++;
        for (i = 0; i < PLACE_COUNT(ids); i++) {
            var = entity_var_by_id(ent, PLACE_ID(ids, i));
            if (var == NULL)
                place_trap("route_fix_block_by_feedback: no variable with this id");
            if (route_use_feedback_node(ent, var, block, block_names) != 0)
                break;
        }
    }
}

/* Last resort after alloc_rebalance failed.  The routing lists g_route_*
 * are not updated. */
static void route_retry_with_feedback(FitEntity *ent, FitList *failed, FitList *names)
{
    FitList *cands, *new_failed;
    int b;

    TRACE("route_retry_with_feedback\n");
    g_fit_report_failures = 1;
    for (b = 0; b < g_dev->n_blocks; b++)
        route_fix_block_by_feedback(ent, PLACE_LIST(failed, b), PLACE_LIST(names, b), b);
    route_clear_uim();
    cands = place_assign_uim(names);
    new_failed = list_create();
    route_commit(ent, cands, names, new_failed);
    g_fit_report_failures = 0;
}

/* Total number of failed ids. */
static int route_count_failed(FitList *failed)
{
    int i, n = 0;

    for (i = 0; i < PLACE_COUNT(failed); i++) {
        if (failed->items[i] != NULL)
            n += PLACE_LIST(failed, i)->count;
    }
    return n;
}

void route_repair(FitEntity *ent, FitList *cands, FitList *failed, FitList *names)
{
    int saved_retry = g_fit_retry_count;
    int patience = 10;
    int best, n;

    g_route_cands = cands;
    g_route_failed_ids = failed;
    g_route_names = names;
    g_route_progress = 1;
    g_fit_retry_count = 0;
    best = route_count_failed(failed);
    while (g_fit_route_failed != 0 && g_route_progress != 0) {
        route_repair_step(ent, g_route_cands, g_route_failed_ids, g_route_names);
        n = route_count_failed(g_route_failed_ids);
        if (n < best) {
            best = n;
            patience = 10;
        } else {
            patience--;
            if (patience == 0)
                break;
        }
    }
    g_route_progress = 1;
    for (g_fit_retry_count = 1;
         g_fit_route_failed != 0 && g_route_progress != 0 && g_fit_retry_count < 0xb;
         g_fit_retry_count++)
        route_repair_step(ent, g_route_cands, g_route_failed_ids, g_route_names);
    g_fit_retry_count = saved_retry;
    if (g_fit_route_failed != 0) {
        TRACE("route_repair\n");
        if (alloc_rebalance(ent, g_route_names) == 0) {
            g_fit_route_failed = 0;
            route_retry_with_feedback(ent, g_route_failed_ids, g_route_names);
        }
    }
}

int route_fct_weight_cmp(void *a, void *b)
{
    int wa = ((FitFct *)a)->flags;
    int wb = ((FitFct *)b)->flags;

    if (wa > wb)
        return 1;
    return wa >= wb ? 0 : -1;
}
