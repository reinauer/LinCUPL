/*
 * jed.c - fuse map and JEDEC file of the ATF1502 / ATF1504 / ATF1508
 * fitter.
 *
 * Two halves:
 *   jed_build_fuse_map turns the placed and routed design into the fuse
 *   image g_jed_map: product term rows, the 24 configuration bits of every
 *   macrocell, the UIM multiplexer fuses, the global OE multiplexers,
 *   device configuration and UES bits.
 *   jed_write_file writes the image as <out>.jed.
 *
 * Device differences:
 *   - fuse addresses and NOTE texts: FitJedecLayout (dev_tables.c);
 *   - order of the sections and of the fuses inside a row: see
 *     jed_write_pt_rows, jed_write_uim_section, jed_write_global_oe,
 *     jed_cfg_area_block_up / _down, jed_literal_column, jed_map_uim;
 *   - one FitJedecMap serves all three devices.
 *
 * jed_write_debug_cfg (-str DEBUG on) shares the checksum state and is
 * kept here.
 *
 * The .jed is written with CR LF line ends.
 */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "base.h"
#include "dev.h"
#include "dev_priv.h"
#include "fit.h"
#include "jedsum.h"
#include "lib.h"
#include "place.h"
#include "syn.h"
#include "tt.h"

FitJedecMap *g_jed_map;
FitList *g_jed_global_slots;            /* the six global OE slots (created by
                                         place_create_global_slots) */

static FILE *g_jed_file;
static FitEntity *g_jed_ent;
static FitList *g_jed_slots;            /* placement list (FitSlot) */
static FitList *g_jed_uim;              /* routing lists (FitUimSlot per block) */
static FitList *g_jed_blocks;           /* per block with something in it: list of its used slots */
static FitList *g_jed_ctl_pin_slots;    /* used slots on global clock / clear / OE pins */
static char *g_jed_cfg;                 /* configuration string being built */
static int g_jed_gclk_pol[3];           /* -1 when global clock 1..3 is used inverted */
static int g_jed_gclr_pol;
static int g_jed_goe_pol[6];
static char g_jed_cfg_area[0xf00];      /* the interleaved configuration fuses as written */

static jedsum_fuses_t g_jed_sum;        /* fuse checksum */
static int g_jed_sum_on = 1;            /* (initial value 1) */

static int jed_set_pterm(FitFctTree *tree, int pt, FitSlot *slot);
static int jed_place_sum(FitSlot *slot, FitFctTree *tree, int use);

static int jed_count(const FitList *l)
{
    return l != NULL ? l->count : 0;
}

/* util_str_ieq as a list comparison. */
static int jed_name_eq(void *item, void *key)
{
    return util_str_ieq(item, key);
}

/* ------------------------------------------------------------------ */
/* Fuse image                                                          */
/* ------------------------------------------------------------------ */

static char *jed_ones(int n)
{
    char *s = util_calloc(1, (size_t)n + 1);
    memset(s, '1', (size_t)n);
    return s;
}

/* Allocates g_jed_map with every fuse intact ('1') and the default
 * macrocell configuration, preset by output_fast, mc_power and
 * open_collector when they are given without a signal list. */
static void jed_alloc_map(void)
{
    int mc, pt, b, i;

    g_jed_map = util_calloc(1, sizeof *g_jed_map);
    for (mc = 0; mc < g_dev->n_macrocells; mc++) {
        for (pt = 0; pt < FIT_PT_PER_MC; pt++) {
            g_jed_map->pt[mc][pt] = jed_ones(FIT_PT_FUSES);
            g_jed_map->pt_extra[mc][pt] = jed_ones(FIT_PT_EXTRA_FUSES);
        }
        g_jed_map->mc_cfg[mc] = util_strdup(FIT_MC_CFG_DEFAULT);
        /* the extended mode leaves fast output and open collector off in unused
         * macrocells */
        if (g_opt.output_fast != 0 && g_opt.output_fast_list == NULL && !FIT_EXT)
            g_jed_map->mc_cfg[mc][0x16] = '0';
        if (g_opt.mc_power != 0 && g_opt.mc_power_list == NULL)
            g_jed_map->mc_cfg[mc][8] = '1';
        if (g_opt.open_collector != 0 && g_opt.open_collector_list == NULL && !FIT_EXT)
            g_jed_map->mc_cfg[mc][0x14] = '1';
    }
    for (b = 0; b < g_dev->n_blocks; b++)
        for (i = 0; i < FIT_UIM_PER_BLOCK; i++)
            g_jed_map->uim[b][i] = jed_ones(g_dev->uim_width);
    for (i = 0; i < FIT_GLOBAL_SLOTS; i++)
        g_jed_map->global[i] = jed_ones(g_dev->uim_width);
    memset(g_jed_map->ues_bits, '1', sizeof g_jed_map->ues_bits);
}

/* ------------------------------------------------------------------ */
/* Lookups                                                             */
/* ------------------------------------------------------------------ */

/* new list of the block inputs of block `block` (1-based) that
 * carry `signal`. */
static FitList *jed_uim_of_signal(const char *signal, int block)
{
    FitList *out = list_create();
    int i, j;

    for (i = 0; i < jed_count(g_jed_uim); i++) {
        FitList *l = g_jed_uim->items[i];
        /* an empty list is skipped */
        if (jed_count(l) > 0 && ((FitUimSlot *)l->items[0])->block == block) {
            for (j = 0; j < l->count; j++) {
                FitUimSlot *u = l->items[j];
                if (util_str_icmp(u->signal, signal) == 0)
                    list_add(out, u);
            }
            break;
        }
    }
    return out;
}

/* List of g_jed_blocks whose slots are in block `block`, NULL if none. */
static FitList *jed_block_slots(int block)
{
    int i;

    for (i = 0; i < jed_count(g_jed_blocks); i++) {
        FitList *l = g_jed_blocks->items[i];
        if (((FitSlot *)l->items[0])->block == block)
            return l;
    }
    return NULL;
}

FitList *jed_cascade_slots(FitSlot *slot)
{
    FitList *block = jed_block_slots(slot->block);     /* may be NULL */
    FitList *out = list_create();
    const char *key;
    int i;

    if (util_str_icmp(slot->signal, FIT_FREE_NAME) != 0 && slot->fct_id != 0)
        key = slot->signal;
    else if (util_str_icmp(slot->buried, FIT_FREE_NAME) != 0)
        key = slot->buried;
    else
        key = NULL;     /* no signal: nothing matches */
    for (i = 0; i < jed_count(block); i++) {
        FitSlot *s = block->items[i];
        if (key != NULL && util_str_icmp(s->cascade, key) == 0)
            list_add(out, s);
    }
    return out;
}

/* slot of block `block` whose foldback signal is `name`. */
static FitSlot *jed_foldback_slot(const char *name, int block)
{
    FitList *l = jed_block_slots(block);
    int i;

    for (i = 0; i < jed_count(l); i++) {
        FitSlot *s = l->items[i];
        if (util_str_icmp(s->foldback, name) == 0)
            return s;
    }
    return NULL;
}

/* of the block inputs carrying a signal, the one to use for the
 * literal `name`.  A signal that enters a block twice is a registered pin
 * with separate pad and feedback: the node ending in 'p' serves the pad
 * signal, the one ending in 'b' the feedback. */
static FitUimSlot *jed_pick_uim(FitList *uims, const char *name)
{
    if (uims->count == 2) {
        FitUimSlot *u0 = uims->items[0];
        char *signal = NULL;
        FitSlot *s = place_slot_of_node(u0->sel_node, &signal);
        FitInstance *inst = NULL;
        int kind;

        if (s != NULL) {
            inst = s->inst;
            if (inst == NULL) {
                if (s->fct_id == 0)
                    inst = tt3_find_instance_by_input(g_jed_ent, s->buried, &kind);
                else
                    inst = tt3_find_instance_by_alias(g_jed_ent, s->signal, &kind);
            }
        }
        if (inst != NULL && inst->pin != NULL && inst->d != NULL) {
            size_t len = strlen(u0->sel_node);
            char last = len > 0 ? u0->sel_node[len - 1] : '\0';
            if (util_str_icmp(inst->pin, name) != 0)
                return last == 'b' ? u0 : uims->items[1];
            return last == 'p' ? u0 : uims->items[1];
        }
    }
    return uims->items[0];
}

/* the used slot called `name` if it sits on a global clock pin
 * (kind 3) or the global clear pin (kind 4), else NULL. */
static FitSlot *jed_ctl_pin_slot(const char *name, int kind)
{
    int i;

    for (i = 0; i < jed_count(g_jed_ctl_pin_slots); i++) {
        FitSlot *s = g_jed_ctl_pin_slots->items[i];
        if (util_str_icmp(s->signal, name) == 0) {
            FitList *pins;
            int pos;
            if (kind == 3)
                pins = dev_gclk_pin_list();
            else if (kind == 4)
                pins = dev_gclr_pin_list();
            else
                pins = list_create();
            pos = list_find(pins, FIT_INT2PTR(s->pin), list_ptr_eq);
            list_destroy(&pins);
            return pos != 0 ? s : NULL;
        }
    }
    return NULL;
}

int jed_tree_var_id(const FitFctTree *tree)
{
    while (tree->sel == FIT_SEL_NOT)
        tree = tree->u.children->items[0];
    if (tree->sel == FIT_SEL_ID)
        return tree->u.id;
    return 0;
}

int jed_children_same_sel(const FitFctTree *tree)
{
    FitFctTree *a = tree->u.children->items[0];
    FitFctTree *b = tree->u.children->items[1];
    return a->sel == b->sel;
}

/* global OE slot carrying `name`. */
static FitUimSlot *jed_global_oe_of(const char *name)
{
    int i;

    for (i = 0; i < jed_count(g_jed_global_slots); i++) {
        FitUimSlot *g = g_jed_global_slots->items[i];
        if (util_str_icmp(g->signal, name) == 0)
            return g;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Product terms                                                       */
/* ------------------------------------------------------------------ */

/* Column of the literal of block input `index` in a product term row:
 * 2 * index + (negated); the ATF1502 swaps the two columns of the odd
 * inputs. */
static int jed_literal_column(int index, int negated)
{
    if (g_dev_desc->id == FIT_DEV_1502 && (index % 2) != 0)
        return index * 2 + (negated ? 0 : 1);
    return index * 2 + (negated ? 1 : 0);
}

/* Clears the fuses of the literals of `tree` (an AND of possibly negated
 * variables) in product term `pt` (0..4) of the macrocell of `slot`.  A
 * NOT node marks its child through FitFctTree.user. */
static int jed_set_pterm(FitFctTree *tree, int pt, FitSlot *slot)
{
    int mc = slot->macrocell - 1;

    if (tree->sel == FIT_SEL_NOT) {
        FitFctTree *child = tree->u.children->items[0];
        child->user = FIT_INT2PTR(1);
        return jed_set_pterm(child, pt, slot);
    }
    if (tree->sel == FIT_SEL_AND) {
        int i;
        for (i = 0; i < jed_count(tree->u.children); i++)
            if (jed_set_pterm(tree->u.children->items[i], pt, slot) == 0)
                return 0;
        return 1;
    }
    if (tree->sel != FIT_SEL_ID)
        return 1;
    {
        int cls;
        char *name = lib_var_name_by_id(tree->u.id, g_jed_ent, &cls);
        char *src = latch_signal_source(g_jed_ent, name);
        FitList *uims = jed_uim_of_signal(src, slot->block);
        FitSlot *fb;

        if (mc < 0 || mc >= g_dev->n_macrocells || pt < 0 || pt >= FIT_PT_PER_MC)
            return 0;   /* cannot happen with slots from the placement list */
        if (uims->count != 0) {
            FitUimSlot *u = jed_pick_uim(uims, name);
            int col = jed_literal_column(u->index, tree->user == FIT_INT2PTR(1));
            if (col >= 0 && col < FIT_PT_FUSES)
                g_jed_map->pt[mc][pt][col] = '0';
            tree->user = NULL;
            list_destroy(&uims);
            return 1;
        }
        list_destroy(&uims);
        fb = jed_foldback_slot(src, slot->block);
        if (fb != NULL) {
            int col = fb->block * -0x10 + 0xf + fb->macrocell;
            if (col >= 0 && col < FIT_PT_EXTRA_FUSES)
                g_jed_map->pt_extra[mc][pt][col] = '0';
            return 1;
        }
        fprintf(g_fit_report, "ERROR : Variable %s not routed on the block %d\n", src, slot->block);
        g_exit_code = 0x1f6;
    }
    return 0;
}

/* takes a free product term of the slot for `use`.  Each of the
 * five terms can serve logic (1) or one special purpose.  Returns the term
 * number 1..5, 0 when none is free. */
static int jed_take_pterm(FitSlot *slot, int use)
{
    if (slot->pt_use[0] == 0 && (use == 1 || use == 2)) {
        slot->pt_use[0] = use;
        return 1;
    }
    if (slot->pt_use[1] == 0 && (use == 0x18 || use == 1 || use == 0x13)) {
        slot->pt_use[1] = use;
        return 2;
    }
    if (slot->pt_use[2] == 0 && (use == 1 || use == 4)) {
        slot->pt_use[2] = use;
        return 3;
    }
    if (slot->pt_use[3] == 0 && (use == 1 || use == 3 || use == 7)) {
        slot->pt_use[3] = use;
        return 4;
    }
    if (slot->pt_use[4] == 0 && (use == 1 || use == 5 || use == 6)) {
        slot->pt_use[4] = use;
        return 5;
    }
    if (FIT_EXT && use == 7 && slot->pt_use[3] == 3) {
        fprintf(g_fit_report, "ERROR : Pterm can not be set because of\n");
        fprintf(g_fit_report, "conflict between pt clock & enable in MC%d\n", slot->macrocell);
        return 0;
    }
    fprintf(g_fit_report, "ERROR : Pterm can not be set\n");
    return 0;
}

/* records a constant or global source for clock (3), reset (4)
 * or output enable (6). */
static int jed_set_source(FitSlot *slot, int kind, int source)
{
    if (kind == 3)
        slot->clk_src = source;
    else if (kind == 4)
        slot->ar_src = source;
    else if (kind == 6)
        slot->oe_src = source;
    return 1;
}

/* notes that a global control signal is used inverted by `inst`. */
static void jed_note_polarity(FitInstance *inst, int source)
{
    if (source >= 8 && source <= 10) {
        if (inst->clk_kind == 2 || inst->clk_kind == 3)
            g_jed_gclk_pol[source - 8] = -1;
    } else if (source >= 0xb && source <= 0x10) {
        if (inst->oe_pol == -1)
            g_jed_goe_pol[source - 0xb] = -1;
    } else if (source == 4) {
        if (inst->ar_pol == -1)
            g_jed_gclr_pol = -1;
    }
}

/* 1 when `name` is an input of the entity that is not an alias
 * of the netlist. */
static int jed_is_plain_input(FitEntity *ent, char *name)
{
    if (list_find(ent->inputs, name, var_has_name) == 0)
        return 0;
    return latch_find_alias(name, ent->net) == 0;
}

/* polarity the cell wants for a control signal: -1 inverted. */
static int jed_control_polarity(const FitInstance *inst, int kind)
{
    if (kind == 3) {
        if (inst->clk_kind == 2 || inst->clk_kind == 3)
            return -1;
    } else if (kind == 6) {
        if (inst->oe_pol == -1)
            return -1;
    } else if (kind == 4) {
        if (inst->ar_pol == -1)
            return -1;
    } else if (kind == 5) {
        if (inst->ap_pol == -1)
            return -1;
    } else if (kind == 7 && inst->ce_pol == -1) {
        return -1;
    }
    return 1;
}

/* gives the control signal `name` a product term of its own. */
static int jed_control_pterm(FitSlot *slot, char *name, int kind, int negate)
{
    FitFctTree *tree;
    int pt = jed_take_pterm(slot, kind);
    int id;

    if (pt == 0)
        return 0;
    id = entity_var_id_by_name(g_jed_ent, name);
    if (jed_control_polarity(slot->inst, kind) == 1 && negate == 0)
        tree = fcttree_new_id(id);
    else
        tree = fcttree_new_not(fcttree_new_id(id));
    return jed_set_pterm(tree, pt - 1, slot) != 0;
}

/* index of the first global clock pin other than `self` whose
 * signal is not one of the global signals (g_fit_global_names), 0 when all are
 * taken (or when that pin is the first). */
static int jed_free_clock_pin(FitList *pins, int self)
{
    int j;

    for (j = 0; j < jed_count(pins); j++) {
        if (j != self) {
            FitSlot *s = place_slot_of_pin(FIT_PTR2INT(pins->items[j]));
            if (s == NULL || list_find(g_fit_global_names, s->signal, jed_name_eq) == 0)
                break;
        }
    }
    return j != jed_count(pins) ? j : 0;
}

/* a clock named in the twoclock property is used in both
 * polarities: reserves the mode words of the parameter block for it.
 * Returns the clock number to use, the result of jed_free_clock_pin. */
static int jed_twoclock(FitList *pins, int index)
{
    int other = jed_free_clock_pin(pins, index);

    if (index == 0) {
        if (other == 1)
            dev_params_set_clock_mode(1, 1);
        else
            dev_params_set_clock_mode(2, 1);
    } else if (index == 1) {
        if (other == 0)
            dev_params_set_clock_mode(0, 2);
        else
            dev_params_set_clock_mode(2, 2);
    } else if (index == 2) {
        if (other == 0)
            dev_params_set_clock_mode(0, 3);
        else
            dev_params_set_clock_mode(1, 3);
    }
    return other;
}

/* clock source code (8..10) for a slot on a global clock pin. */
static int jed_clock_source(FitSlot *gs, int kind, int inverted)
{
    int result = 0;

    if (kind == 3) {
        FitList *pins = dev_gclk_pin_list();
        int i;
        for (i = 0; i < pins->count; i++) {
            if (gs->pin == FIT_PTR2INT(pins->items[i])) {
                if (inverted != 0
                    && list_find(g_opt.twoclock, gs->signal, jed_name_eq) != 0)
                    i = jed_twoclock(pins, i);
                result = i + 8;
                break;
            }
        }
        list_destroy(&pins);
    }
    return result;
}

/* control signal `name` of the cell in `slot` is an input of the
 * design: use the global clock / clear / OE network when the signal is on
 * it, else a product term. */
static int jed_control_from_input(FitSlot *slot, char *name, int kind, int negate)
{
    FitInstance *inst = slot->inst;

    if (kind == 3) {
        int inverted = (negate == -1 || inst->clk_kind == 2 || inst->clk_kind == 3) ? 1 : 0;
        FitSlot *gs = jed_ctl_pin_slot(name, 3);
        if (gs != NULL) {
            slot->clk_src = jed_clock_source(gs, 3, inverted);
            if (negate != 0) {
                if (inst->clk_kind == 1)
                    inst->clk_kind = 2;
                else if (inst->clk_kind == 4)
                    inst->clk_kind = 3;
            }
            jed_note_polarity(inst, slot->clk_src);
            if (negate == 0)
                return 1;
            if (inst->clk_kind == 2)
                inst->clk_kind = 1;
            else if (inst->clk_kind == 3)
                inst->clk_kind = 4;
            return 1;
        }
    } else if (kind == 4) {
        if (jed_ctl_pin_slot(name, 4) != NULL) {
            slot->ar_src = 4;
            jed_note_polarity(inst, 4);
            return 1;
        }
    } else if (kind == 6) {
        FitUimSlot *g = jed_global_oe_of(name);
        if (g != NULL) {
            slot->oe_src = g->index + 0xb;
            jed_note_polarity(inst, slot->oe_src);
            return 1;
        }
    } else if (kind != 5 && kind != 7) {
        return 1;
    }
    return jed_control_pterm(slot, name, kind, negate) != 0;
}

/* control signal `name` of the cell in `slot` is a function of
 * the design. */
static int jed_control_from_fct(FitSlot *slot, char *name, int kind)
{
    int id = entity_var_id_by_name(g_jed_ent, name);
    FitFct *fct = entity_find_fct(g_jed_ent, id);
    FitFctTree *t = fct->tree[0];
    int negated = 0;
    int pt;

    if (t->sel == FIT_SEL_CONST)
        return jed_set_source(slot, kind, t->u.value == 1 ? 0x11 : 0x12) != 0;

    if (t->sel == FIT_SEL_OR) {
        if (kind == 6) {
            FitUimSlot *g = jed_global_oe_of(name);
            if (g != NULL) {
                slot->oe_src = g->index + 0xb;
                return 1;
            }
        }
        pt = jed_take_pterm(slot, kind);
        if (pt == 0)
            return 0;
        /* a sum cannot be a control term: the term gets the signal itself */
        return jed_set_pterm(fcttree_new_id(fct->var_id), pt - 1, slot) != 0;
    }

    if (kind == 6) {
        FitUimSlot *g = jed_global_oe_of(name);
        if (g == NULL) {
            int id2 = 0;
            if (t->sel == FIT_SEL_ID) {
                id2 = t->u.id;
            } else if (t->sel == FIT_SEL_NOT) {
                negated = 1;
                id2 = ((FitFctTree *)t->u.children->items[0])->u.id;
            }
            if (t->sel == FIT_SEL_ID || t->sel == FIT_SEL_NOT)
                g = jed_global_oe_of(entity_var_by_id(g_jed_ent, id2)->name);
        }
        if (g != NULL && !jed_is_plain_input(g_jed_ent, g->signal)) {
            FitInstance *inst = slot->inst;
            slot->oe_src = g->index + 0xb;
            if (negated != 0 && inst->oe_pol == 1)
                inst->oe_pol = -1;
            jed_note_polarity(inst, slot->oe_src);
            if (negated != 0 && inst->oe_pol == -1)
                inst->oe_pol = 1;
            return 1;
        }
    } else if (kind == 3) {
        int id2 = 0;
        if (t->sel == FIT_SEL_ID) {
            id2 = t->u.id;
        } else if (t->sel == FIT_SEL_NOT) {
            negated = -1;
            id2 = ((FitFctTree *)t->u.children->items[0])->u.id;
        }
        if (t->sel == FIT_SEL_ID || t->sel == FIT_SEL_NOT) {
            char *src = latch_signal_source(g_jed_ent, entity_var_by_id(g_jed_ent, id2)->name);
            if (list_find(g_jed_ent->outputs, src, var_has_name) != 0
                && jed_control_from_input(slot, src, 3, negated) != 0)
                return 1;
        }
    }
    pt = jed_take_pterm(slot, kind);
    if (pt == 0)
        return 0;
    return jed_set_pterm(fct->tree[0], pt - 1, slot) != 0;
}

/* the control signal `name` of kind `kind` (3 clock, 4 reset, 5 preset,
 * 6 OE, 7 clock enable) of the cell in `slot`, from an input pin or from
 * the signal's function.  1 for a NULL name. */
static int jed_map_control(FitSlot *slot, char *name, int kind)
{
    if (name == NULL)
        return 1;
    if (list_find(g_jed_ent->inputs, name, var_has_name) == 0)
        return jed_control_from_fct(slot, name, kind) != 0;
    return jed_control_from_input(slot, name, kind, 0) != 0;
}

/* clock, reset, preset, clock enable and output enable of the
 * cell of the slot. */
static int jed_map_controls(FitSlot *slot)
{
    FitInstance *inst = slot->inst;

    return jed_map_control(slot, inst->clk, 3) != 0
        && jed_map_control(slot, inst->ar, 4) != 0
        && jed_map_control(slot, inst->ap, 5) != 0
        && jed_map_control(slot, inst->ce, 7) != 0
        && jed_map_control(slot, inst->oe, 6) != 0;
}

/* puts the function `tree` into the product terms of `slot`:
 * one term for a product (marked `use`); a sum takes one logic term per
 * product, first in the neighbours that lend their terms to the slot
 * (four each, five when the neighbour has no foldback), then in the slot. */
static int jed_place_sum(FitSlot *slot, FitFctTree *tree, int use)
{
    FitList *lenders;
    int n = 0;
    int n_terms;
    int pt;
    int j;

    if (tree->sel != FIT_SEL_OR) {
        pt = jed_take_pterm(slot, use);
        if (pt == 0)
            return 0;
        return jed_set_pterm(tree, pt - 1, slot) != 0;
    }
    n_terms = jed_count(tree->u.children);
    lenders = jed_cascade_slots(slot);
    for (j = 0; j < lenders->count; j++) {
        FitSlot *c = lenders->items[j];
        int cap;
        int k = 0;
        if (util_str_icmp(c->foldback, "NA") == 0)
            cap = 5;
        else
            cap = util_str_icmp(c->foldback, FIT_FREE_NAME) == 0 ? 5 : 4;
        do {
            if (n >= n_terms)
                break;      /* a sum without terms ends here */
            pt = jed_take_pterm(c, 1);
            if (pt == 0)
                return 0;
            if (jed_set_pterm(tree->u.children->items[n], pt - 1, c) == 0)
                return 0;
            n++;
        } while (n < n_terms && ++k < cap);
    }
    while (n < n_terms) {
        pt = jed_take_pterm(slot, 1);
        if (pt == 0)
            return 0;
        if (jed_set_pterm(tree->u.children->items[n], pt - 1, slot) == 0)
            return 0;
        n++;
    }
    list_destroy(&lenders);
    return 1;
}

/* function of the form a * b' + a' * b (or the XNOR form with
 * equal polarities) for a macrocell with the XOR gate: one operand goes to
 * the XOR term (0x13), the other to the sum. */
static int jed_place_xor(FitSlot *slot, FitFct *fct)
{
    FitFctTree *t = fct->tree[0];
    FitFctTree *prod, *e1, *e2;
    int id1, id2;

    if (t->sel != FIT_SEL_OR || jed_count(t->u.children) != 2)
        return 0;
    prod = t->u.children->items[0];
    if (prod->sel != FIT_SEL_AND || jed_count(prod->u.children) != 2)
        return 0;
    id1 = jed_tree_var_id(prod->u.children->items[0]);
    id2 = jed_tree_var_id(prod->u.children->items[1]);
    if (id1 == 0 || id2 == 0)
        return 0;
    e1 = place_fct_tree(g_jed_ent, id1);
    e2 = place_fct_tree(g_jed_ent, id2);
    if (!jed_children_same_sel(prod)) {
        if (e2->sel == FIT_SEL_OR) {
            if (e1->sel == FIT_SEL_OR) {
                fprintf(g_fit_report, "Incorrect xor form\n");
                return 0;
            }
            return jed_place_sum(slot, e1, 0x13) != 0 && jed_place_sum(slot, e2, 1) != 0;
        }
        if (jed_place_sum(slot, e2, 0x13) == 0)
            return 0;
        return jed_place_sum(slot, e1, 1) != 0;
    }
    if (e2->sel != FIT_SEL_ID) {
        if (e1->sel != FIT_SEL_ID) {
            fprintf(g_fit_report, "Incorrect xnor form\n");
            return 0;
        }
        if (jed_place_sum(slot, fcttree_new_not(e1), 0x13) == 0)
            return 0;
        return jed_place_sum(slot, e2, 1) != 0;
    }
    if (jed_place_sum(slot, fcttree_new_not(e2), 0x13) == 0)
        return 0;
    return jed_place_sum(slot, e1, 1) != 0;
}

/* 1 when nothing is placed in the slot. */
static int jed_slot_is_empty(const FitSlot *slot)
{
    if (util_str_icmp(slot->signal, FIT_FREE_NAME) != 0)
        return 0;
    if (util_str_icmp(slot->buried, FIT_FREE_NAME) != 0)
        return 0;
    if (util_str_icmp(slot->foldback, FIT_FREE_NAME) != 0 && util_str_icmp(slot->foldback, "NA") != 0)
        return 0;
    return util_str_icmp(slot->cascade, FIT_FREE_NAME) == 0;
}

/* product terms and control sources of one slot. */
static int jed_map_slot(FitSlot *slot)
{
    FitInstance *inst = NULL;
    int have_inst = 0;
    FitFct *fct;
    int kind;
    int id;

    if (util_str_icmp(slot->signal, FIT_FREE_NAME) != 0)
        jed_set_source(slot, 6, slot->fct_id == 0 ? 0x12 : 0x11);
    if (util_str_icmp(slot->fn_kind, "reg") == 0) {
        if (slot->fct_id != 0) {
            inst = tt3_find_instance_by_alias(g_jed_ent, slot->signal, &kind);
            have_inst = 1;
            if (inst != NULL) {
                slot->inst = inst;
                slot->cell_kind = kind;
                if (!jed_map_controls(slot))
                    return 0;
            }
        }
        if (util_str_icmp(slot->buried, FIT_FREE_NAME) != 0) {
            inst = tt3_find_instance_by_input(g_jed_ent, slot->buried, &kind);
            have_inst = 1;
            if (inst != NULL) {
                slot->inst = inst;
                slot->cell_kind = kind;
                if (!jed_map_controls(slot))
                    return 0;
            }
        }
    }
    if (util_str_icmp(slot->foldback, FIT_FREE_NAME) != 0 && util_str_icmp(slot->foldback, "NA") != 0) {
        int pt;
        id = entity_var_id_by_name(g_jed_ent, slot->foldback);
        fct = entity_find_fct(g_jed_ent, id);
        if (fct->tree[0]->sel == FIT_SEL_OR) {
            fprintf(g_fit_report, "Bad foldback form\n");
            return 0;
        }
        pt = jed_take_pterm(slot, 2);
        if (pt == 0)
            return 0;
        if (jed_set_pterm(fct->tree[0], pt - 1, slot) == 0)
            return 0;
    }
    if (util_str_icmp(slot->cascade, FIT_FREE_NAME) != 0)
        return 1;       /* the terms of this macrocell belong to a neighbour */
    if (util_str_icmp(slot->fn_kind, "reg") == 0) {
        /* The output name is read through the last instance looked up
         * above; without one the slot cannot be mapped. */
        if (!have_inst || inst == NULL)
            return 0;
        id = entity_var_id_by_name(g_jed_ent, inst->q);
        fct = entity_find_fct(g_jed_ent, id);
    } else if (slot->fct_id != 0) {
        fct = entity_find_fct(g_jed_ent, slot->fct_id);
    } else {
        if (util_str_icmp(slot->buried, FIT_FREE_NAME) == 0)
            return 1;
        id = entity_var_id_by_name(g_jed_ent, slot->buried);
        fct = entity_find_fct(g_jed_ent, id);
    }
    if (fct == NULL)
        return 0;
    /* the extended mode also maps a constant function when the macrocell has a
     * buried signal beside it */
    if (tt2_tree_is_const(fct->tree[0]) == 0
        || (FIT_EXT && slot->fct_id != 0 && util_str_icmp(slot->buried, FIT_FREE_NAME) != 0)) {
        slot->polarity = entity_var_by_id(g_jed_ent, fct->var_id)->polarity;
        if (list_find(g_opt.xor_list, FIT_INT2PTR(fct->var_id), list_ptr_eq) != 0)
            return jed_place_xor(slot, fct) != 0;
        if (fit_drv_fast_inlatch_index(fct->var_id) == 0) {
            if (slot->fct_id != 0 && util_str_icmp(slot->buried, FIT_FREE_NAME) != 0) {
                if (jed_place_sum(slot, fct->tree[0], 0x18) == 0)
                    return 0;
                fct = entity_find_fct(g_jed_ent, slot->fct_id);
                slot->fb_pol = entity_var_by_id(g_jed_ent, fct->var_id)->polarity;
            }
            return jed_place_sum(slot, fct->tree[0], 1) != 0;
        }
    }
    return 1;
}

/* product terms and control sources of every used slot, block by block. */
static int jed_map_all_slots(void)
{
    int i, j;

    for (i = 0; i < jed_count(g_jed_blocks); i++) {
        FitList *l = g_jed_blocks->items[i];
        for (j = 0; j < jed_count(l); j++)
            if (!jed_map_slot(l->items[j]))
                return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Macrocell configuration bits                                        */
/* ------------------------------------------------------------------ */

/* the cell is a latch. */
static int jed_cell_is_latch(const FitSlot *slot)
{
    if (util_str_icmp(slot->fn_kind, "reg") == 0) {
        FitNetItem *it = slot->inst != NULL ? netlist_find_item(g_jed_ent, slot->inst->q) : NULL;
        return it != NULL && it->kind == FIT_GATE_LATCH;
    }
    return 0;
}

/* the function in the macrocell is the constant 1. */
static int jed_fct_is_one(const FitSlot *slot)
{
    FitFct *fct;
    int id;

    if (util_str_icmp(slot->fn_kind, "reg") == 0 && slot->pt_use[1] != 0x18) {
        if (slot->inst == NULL)
            return 0;   /* no cell */
        id = entity_var_id_by_name(g_jed_ent, slot->inst->q);
    } else if (slot->fct_id != 0) {
        id = slot->fct_id;
    } else {
        if (util_str_icmp(slot->buried, FIT_FREE_NAME) == 0)
            return 0;
        id = entity_var_id_by_name(g_jed_ent, slot->buried);
    }
    fct = entity_find_fct(g_jed_ent, id);
    if (fct == NULL)
        return 0;       /* no function */
    return fct->tree[0]->sel == FIT_SEL_CONST && fct->tree[0]->u.value == 1;
}

static int jed_has_foldback(const FitSlot *slot)
{
    return util_str_icmp(slot->foldback, FIT_FREE_NAME) != 0 && util_str_icmp(slot->foldback, "NA") != 0;
}

/* the macrocell has logic of its own: a function that is not the
 * constant 1 and whose terms are not lent to a neighbour */
static int jed_has_logic(const FitSlot *slot)
{
    if (util_str_icmp(slot->buried, FIT_FREE_NAME) == 0 && slot->fct_id == 0)
        return 0;
    if (util_str_icmp(slot->cascade, FIT_FREE_NAME) != 0)
        return 0;
    return !jed_fct_is_one(slot);
}

/* the function in the macrocell is in the xor_synthesis list (or its
 * second term is used as XOR) */
static int jed_uses_xor(const FitSlot *slot)
{
    int id;

    if (slot->pt_use[1] == 0x13)
        return 1;
    id = slot->fct_id;
    if (id == 0) {
        if (util_str_icmp(slot->buried, FIT_FREE_NAME) == 0)
            return 0;
        id = entity_var_id_by_name(g_jed_ent, slot->buried);
    }
    return list_find(g_opt.xor_list, FIT_INT2PTR(id), list_ptr_eq) != 0;
}

/* the function in the macrocell is used inverted */
static int jed_is_inverted(const FitSlot *slot)
{
    if (slot->polarity == -1)
        return 1;
    return slot->pt_use[1] == 0x18 && slot->fb_pol == -1;
}

/* the slot is a fast-inlatch input pin */
static int jed_is_fast_inlatch_pin(const FitSlot *slot)
{
    if (util_str_icmp(slot->signal, FIT_FREE_NAME) != 0 && slot->fct_id == 0
        && util_str_icmp(slot->fn_kind, "reg") == 0) {
        int id = entity_var_id_by_name(g_jed_ent, slot->signal);
        if (fit_drv_fast_inlatch_node(id) != 0)
            return 1;
    }
    return 0;
}

/* the macrocell holds a register whose output is not a tri-state */
static int jed_is_registered_output(const FitSlot *slot)
{
    FitNetItem *it;

    if (util_str_icmp(slot->fn_kind, "com") == 0)
        return 0;
    if (util_str_icmp(slot->buried, FIT_FREE_NAME) == 0 && slot->fct_id == 0)
        return 0;
    if (slot->pt_use[1] == 0x18)
        return 0;
    it = slot->inst != NULL ? netlist_find_item(g_jed_ent, slot->inst->q) : NULL;
    if (it == NULL)
        return 0;       /* no netlist item */
    return it->kind != FIT_GATE_TH;
}

/* the slot carries a signal with a function */
static int jed_slot_has_function(const FitSlot *slot)
{
    return util_str_icmp(slot->signal, FIT_FREE_NAME) != 0 && slot->fct_id != 0;
}

static int jed_option_applies(int flag, FitList *names, const FitSlot *slot)
{
    if (flag == 0)
        return 0;
    if (names != NULL && list_find(names, slot->signal, jed_name_eq) == 0)
        return 0;
    return 1;
}

/* the 24 configuration bits of a macrocell into g_jed_cfg. */
static int jed_build_cfg(const FitSlot *slot)
{
    static const char defaults[] = "111111100000000101100110";
    char *c = util_strdup("xxxxxxxxxxxxxxxxxxxxxxxx");
    int i;

    g_jed_cfg = c;
    if (jed_has_logic(slot) || jed_uses_xor(slot))
        c[6] = '0';
    else if (jed_fct_is_one(slot))
        c[6] = '1';

    if (jed_has_foldback(slot))
        c[9] = '1';
    else if (slot->pt_use[0] == 1)
        c[9] = '0';

    if (slot->inst != NULL && slot->cell_kind == 2 && slot->polarity == -1) {
        c[10] = '1';
        c[11] = '1';
    } else if (slot->inst != NULL && slot->cell_kind == 2 && slot->polarity == 1) {
        c[10] = '1';
        c[11] = '0';
    } else if (jed_is_inverted(slot) || jed_uses_xor(slot)) {
        c[10] = '0';
        c[11] = '1';
    } else {
        c[10] = '0';
        c[11] = '0';
    }

    if (jed_uses_xor(slot))
        c[13] = '1';
    else if (slot->pt_use[1] == 1 || jed_fct_is_one(slot))
        c[13] = '0';

    if (jed_is_fast_inlatch_pin(slot)) {
        c[3] = '0';
        c[4] = '0';
    } else if (util_str_icmp(slot->fn_kind, "reg") == 0 && slot->pt_use[1] != 0x18) {
        c[4] = '1';
    } else if (slot->pt_use[1] == 0x18) {
        c[3] = '1';
        c[4] = '0';
    }
    if (util_str_icmp(slot->signal, FIT_FREE_NAME) != 0 && slot->fct_id != 0)
        c[3] = jed_is_registered_output(slot) ? '0' : '1';
    if (slot->pt_use[1] == 1 && c[4] == '0')
        c[3] = '0';

    c[12] = (slot->pt_use[3] == 1) ? '0' : '1';
    if (slot->pt_use[3] == 3) {
        c[5] = '1';
        c[12] = '1';
    } else if (slot->clk_src == 8) {
        c[5] = '0';
        c[7] = '1';
        c[19] = '1';
    } else if (slot->clk_src == 9) {
        c[5] = '0';
        c[19] = '0';
    } else if (slot->clk_src == 10) {
        c[5] = '0';
        c[7] = '0';
        c[19] = '1';
    } else if (slot->clk_src == 0x11) {
        c[5] = '1';
        c[12] = '0';
    }
    if (slot->pt_use[3] == 7) {
        c[5] = '0';
        c[12] = '1';
    } else if (slot->clk_src == 8 || slot->clk_src == 9 || slot->clk_src == 10) {
        c[12] = '0';
    }

    c[16] = (slot->pt_use[2] == 1) ? '0' : '1';
    if (slot->ar_src == 4)
        c[15] = '0';
    else if (slot->ar_src == 0 || slot->pt_use[2] == 4)
        c[15] = '1';

    c[14] = (slot->pt_use[4] == 1) ? '0' : '1';
    if (slot->pt_use[4] == 5) {
        c[14] = '1';
        c[17] = '1';
    } else {
        c[17] = '0';
    }

    if (slot->oe_src == 0x12 || (slot->oe_src >= 0xb && slot->oe_src <= 0x11)) {
        int code = (slot->oe_src == 0x12) ? 0 : slot->oe_src - 0xa;   /* 0xb..0x11 -> 1..7 */
        c[0] = (code & 4) ? '1' : '0';
        c[1] = (code & 2) ? '1' : '0';
        c[2] = (code & 1) ? '1' : '0';
        if (slot->oe_src == 0x11)
            c[17] = '1';
    }
    if (slot->pt_use[4] == 6) {
        c[0] = '1';
        c[1] = '1';
        c[2] = '1';
        c[14] = '1';
        c[17] = '0';
    }

    c[18] = (!jed_is_registered_output(slot) && slot->pt_use[1] != 0x18) ? '1' : '0';
    c[23] = jed_cell_is_latch(slot) ? '0' : '1';
    c[8] = jed_option_applies(g_opt.mc_power, g_opt.mc_power_list, slot) ? '1' : '0';
    /* the extended mode makes only macrocells with a function open collector or fast */
    c[20] = jed_option_applies(g_opt.open_collector, g_opt.open_collector_list, slot)
            && (!FIT_EXT || jed_slot_has_function(slot)) ? '1' : '0';
    c[21] = (slot->pt_use[0] == 0 && slot->pt_use[1] == 0 && slot->pt_use[2] == 0
             && slot->pt_use[3] == 0 && slot->pt_use[4] == 0) ? '1' : '0';
    c[22] = jed_option_applies(g_opt.output_fast, g_opt.output_fast_list, slot)
            && (!FIT_EXT || jed_slot_has_function(slot)) ? '0' : '1';

    for (i = 0; i < FIT_MC_CFG_BITS; i++)
        if (c[i] == 'x')
            c[i] = defaults[i];
    return 1;
}

/* configuration string of every used slot, block by block, into
 * g_jed_map->mc_cfg. */
static int jed_map_all_cfg(void)
{
    int i, j;

    for (i = 0; i < jed_count(g_jed_blocks); i++) {
        FitList *l = g_jed_blocks->items[i];
        for (j = 0; j < jed_count(l); j++) {
            FitSlot *slot = l->items[j];
            if (!jed_build_cfg(slot))
                return 0;
            if (slot->macrocell >= 1 && slot->macrocell <= g_dev->n_macrocells) {
                free(g_jed_map->mc_cfg[slot->macrocell - 1]);
                g_jed_map->mc_cfg[slot->macrocell - 1] = util_strdup(g_jed_cfg);
            }
            free(g_jed_cfg);
            g_jed_cfg = NULL;
        }
    }
    return 1;
}

/* The configuration fuses are stored as pairs of columns over the sixteen
 * macrocells of a block, alternately walking the block upwards and
 * downwards.  jed_cfg_area_block_up / jed_cfg_area_block_down are the two
 * walks; the ATF1502 swaps them. */
static void jed_cfg_area_put(int *pos, char c)
{
    int i = *pos - g_dev_desc->jedec->mc_cfg_addr;

    if (i >= 0 && i < (int)sizeof g_jed_cfg_area)
        g_jed_cfg_area[i] = c;
    *pos += 1;
}

/* Upwards from the first macrocell of block `block` (1-based): an odd
 * macrocell gives bit x then bit y, an even one y then x. */
static void jed_cfg_area_block_up(int block, int *pos, int x, int y)
{
    int mc = block * 16 - 15;
    int n;

    for (n = 0; n < 16; n++, mc++) {
        const char *cfg = g_jed_map->mc_cfg[mc - 1];
        if (mc % 2 == 0) {
            jed_cfg_area_put(pos, cfg[y]);
            jed_cfg_area_put(pos, cfg[x]);
        } else {
            jed_cfg_area_put(pos, cfg[x]);
            jed_cfg_area_put(pos, cfg[y]);
        }
    }
}

/* Downwards from the last macrocell of the block: an even macrocell gives
 * bit x then bit y, an odd one y then x. */
static void jed_cfg_area_block_down(int block, int *pos, int x, int y)
{
    int mc = block * 16;
    int n;

    for (n = 0; n < 16; n++, mc--) {
        const char *cfg = g_jed_map->mc_cfg[mc - 1];
        if (mc % 2 == 0) {
            jed_cfg_area_put(pos, cfg[x]);
            jed_cfg_area_put(pos, cfg[y]);
        } else {
            jed_cfg_area_put(pos, cfg[y]);
            jed_cfg_area_put(pos, cfg[x]);
        }
    }
}

/* one group of four configuration bits for half of the blocks. */
static void jed_cfg_area_group(int half, int *pos, int a, int b, int c, int d)
{
    int n = g_dev->n_blocks;
    int block;

    if (half == 0) {
        for (block = 1; block <= n / 2; block++) {
            *pos += 0x10;
            if (g_dev_desc->id == FIT_DEV_1502) {
                jed_cfg_area_block_down(block, pos, a, b);
                jed_cfg_area_block_up(block, pos, c, d);
            } else {
                jed_cfg_area_block_up(block, pos, a, b);
                jed_cfg_area_block_down(block, pos, c, d);
            }
        }
    } else {
        for (block = n; block > n / 2; block--) {
            *pos += 0x10;
            if (g_dev_desc->id == FIT_DEV_1502) {
                jed_cfg_area_block_down(block, pos, a, b);
                jed_cfg_area_block_up(block, pos, c, d);
            } else {
                jed_cfg_area_block_up(block, pos, a, b);
                jed_cfg_area_block_down(block, pos, c, d);
            }
        }
    }
}

/* Lays the configuration bits of every macrocell out in g_jed_cfg_area:
 * two halves, six groups of four bits each. */
static void jed_build_cfg_area(void)
{
    int half;

    memset(g_jed_cfg_area, '1', (size_t)g_dev_desc->jedec->mc_cfg_fuses);
    for (half = 0; half < 2; half++) {
        int pos = g_dev_desc->jedec->cfg_half_addr[half];
        jed_cfg_area_group(half, &pos, 0x10, 0xc, 0xe, 0xb);
        jed_cfg_area_group(half, &pos, 9, 6, 0xd, 10);
        jed_cfg_area_group(half, &pos, 0x14, 0x12, 8, 0x15);
        jed_cfg_area_group(half, &pos, 7, 0x13, 0x16, 5);
        jed_cfg_area_group(half, &pos, 0x17, 4, 3, 0xf);
        jed_cfg_area_group(half, &pos, 0, 1, 0x11, 2);
    }
}

/* ------------------------------------------------------------------ */
/* UIM, global OE, device bits                                         */
/* ------------------------------------------------------------------ */

/* the fuse of the selected input of every routed block input
 * multiplexer.  The ATF1504 keeps the fuses of a multiplexer in reverse
 * order. */
static int jed_map_uim(void)
{
    int w = g_dev->uim_width;
    int i, j;

    for (i = 0; i < jed_count(g_jed_uim); i++) {
        FitList *l = g_jed_uim->items[i];
        for (j = 0; j < jed_count(l); j++) {
            FitUimSlot *u = l->items[j];
            int k = dev_uim_input_of(u);
            if (k == -1) {
                fprintf(g_fit_report, "ERROR : UIM not FOUND\n");
                return 0;
            }
            if (u->block >= 1 && u->block <= g_dev->n_blocks)
                g_jed_map->uim[u->block - 1][u->index][g_dev_desc->id == FIT_DEV_1504 ? w - 1 - k : k] = '0';
        }
    }
    return 1;
}

/* the fuse of the selected input of every used global OE multiplexer. */
static int jed_map_global_oe(void)
{
    int i;

    for (i = 0; i < FIT_GLOBAL_SLOTS; i++) {
        FitUimSlot *g = g_jed_global_slots->items[i];
        if (util_str_icmp(g->signal, FIT_FREE_NAME) != 0 && g->signal != NULL) {
            int k = dev_oe_input_of(g);
            if (k == -1) {
                fprintf(g_fit_report, "ERROR : OE UIM not FOUND\n");
                return 0;
            }
            g_jed_map->global[g->index][k] = '0';
        }
    }
    return 1;
}

static void jed_mode_bits(char *two, int mode)
{
    if (mode == 3) {
        two[0] = '0';
        two[1] = '0';
    } else if (mode == 2) {
        two[0] = '0';
        two[1] = '1';
    } else if (mode == 1) {
        two[0] = '1';
        two[1] = '1';
    }
}

/* the 32 device configuration bits. */
static int jed_map_device_bits(void)
{
    char *b = g_jed_map->cfg_bits;
    int i;

    for (i = 0; i < 6; i++)
        b[i] = '1';
    b[6] = g_opt.power_reset != 0 ? '0' : '1';
    b[7] = (g_opt.tdi_pullup != 0 && g_opt.jtag != 0) ? '0' : '1';
    b[8] = (g_opt.tms_pullup != 0 && g_opt.jtag != 0) ? '0' : '1';
    b[9] = g_opt.gck_itd[2] != 0 ? '0' : '1';
    b[10] = g_opt.gck_itd[1] != 0 ? '0' : '1';
    b[11] = g_opt.gck_itd[0] != 0 ? '0' : '1';
    jed_mode_bits(&b[12], g_dev->gclk2_mode);
    b[14] = g_jed_gclk_pol[2] == -1 ? '1' : '0';
    jed_mode_bits(&b[15], g_dev->gclk1_mode);
    b[17] = g_jed_gclk_pol[1] == -1 ? '1' : '0';
    jed_mode_bits(&b[18], g_dev->gclk0_mode);
    b[20] = g_jed_gclk_pol[0] == -1 ? '1' : '0';
    b[21] = g_jed_gclr_pol == -1 ? '0' : '1';
    for (i = 0; i < 6; i++)
        b[22 + i] = g_jed_goe_pol[5 - i] == -1 ? '0' : '1';
    b[28] = g_opt.pd2 != 0 ? '0' : '1';
    b[29] = g_opt.pd1 != 0 ? '0' : '1';
    b[30] = g_opt.pin_keep == 0 ? '1' : '0';
    b[31] = '0';
    return 1;
}

/* the two UES characters, most significant bit first. */
static int jed_map_ues(void)
{
    int i, k;

    if (g_opt.ues == NULL)
        return 1;
    for (i = 0; i < 2; i++) {
        /* an empty string has no second character: 0 */
        int c = (i == 1 && g_opt.ues[0] == '\0') ? 0 : (signed char)g_opt.ues[i];
        for (k = i * 8 + 7; k >= i * 8; k--) {
            g_jed_map->ues_bits[k] = (c & 1) ? '1' : '0';
            c >>= 1;
        }
    }
    return 1;
}

/* g_jed_blocks: for every block with used slots the list of them. */
static void jed_collect_blocks(void)
{
    int block, i;

    g_jed_blocks = list_create();
    for (block = 1; block <= g_dev->n_blocks; block++) {
        FitList *l = list_create();
        for (i = 0; i < jed_count(g_jed_slots); i++) {
            FitSlot *s = g_jed_slots->items[i];
            if (!jed_slot_is_empty(s) && s->block == block)
                list_add(l, s);
        }
        if (l->count == 0)
            list_destroy(&l);
        else
            list_add(g_jed_blocks, l);
    }
}

/* g_jed_ctl_pin_slots: the used slots on global clock, clear and OE
 * pins. */
static void jed_collect_ctl_pin_slots(void)
{
    FitList *clk = dev_gclk_pin_list();
    FitList *clr = dev_gclr_pin_list();
    FitList *oe = dev_oe_pin_list();
    FitList *a = list_union(clr, clk, list_ptr_eq);
    FitList *pins = list_union(a, oe, list_ptr_eq);
    int i, j;

    g_jed_ctl_pin_slots = list_create();
    for (i = 0; i < jed_count(pins); i++) {
        for (j = 0; j < jed_count(g_jed_slots); j++) {
            FitSlot *s = g_jed_slots->items[j];
            if (strcmp(s->signal, FIT_FREE_NAME) != 0 && s->pin == FIT_PTR2INT(pins->items[i]))
                list_add(g_jed_ctl_pin_slots, s);
        }
    }
    if (pins != NULL)
        list_destroy(&pins);
    if (a != NULL)
        list_destroy(&a);
    list_destroy(&clk);
    list_destroy(&clr);
    list_destroy(&oe);
}

int jed_build_fuse_map(FitEntity *ent, FitList *slots, FitList *uim)
{
    int i;

    g_jed_ent = ent;
    g_jed_uim = uim;
    g_jed_slots = slots;
    jed_collect_blocks();
    jed_collect_ctl_pin_slots();
    jed_alloc_map();
    g_jed_gclr_pol = 1;
    for (i = 0; i < 3; i++)
        g_jed_gclk_pol[i] = 1;
    for (i = 0; i < 6; i++)
        g_jed_goe_pol[i] = 1;
    if (!jed_map_all_slots())
        return 0;
    if (!jed_map_all_cfg())
        return 0;
    jed_build_cfg_area();
    if (!jed_map_uim())
        return 0;
    if (!jed_map_global_oe())
        return 0;
    if (!jed_map_device_bits())
        return 0;
    return jed_map_ues() != 0;
}

/* ------------------------------------------------------------------ */
/* JEDEC file                                                          */
/* ------------------------------------------------------------------ */

/* fprintf to the .jed with the line ends of a text-mode stream. */
static void jed_printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    int n, i;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n >= (int)sizeof buf)
        n = (int)sizeof buf - 1;
    for (i = 0; i < n; i++) {
        if (buf[i] == '\n')
            fputc('\r', g_jed_file);
        fputc(buf[i], g_jed_file);
    }
}

/* writes one fuse and adds it to the checksum (as not blown while
 * g_jed_sum_on is clear). */
static void jed_put_fuse(char c)
{
    jed_printf("%c", c);
    jedsum_fuses_add(&g_jed_sum, c == '1' && g_jed_sum_on != 0);
}

/* header.  QP is the number in the package name. */
static void jed_write_header(void)
{
    time_t now;
    char digits[8];
    size_t len = strlen(g_dev_package);
    size_t i;

    time(&now);
    memset(digits, 0, sizeof digits);
    for (i = 4; i < len && i - 4 < sizeof digits - 1; i++)
        digits[i - 4] = g_dev_package[i];
    jed_printf("%c", 2);
    jed_printf("JEDEC file for: %s\n", g_dev_desc->jedec->part);
    jed_printf("Created on: %s*\n", ctime(&now));
    if (g_opt.security == 0)
        jed_printf("QF%d* QP%d * F0*\n", g_dev_desc->jedec->n_fuses, atoi(digits));
    else
        jed_printf("QF%d* QP%d * F0* G1*\n", g_dev_desc->jedec->n_fuses, atoi(digits));
    jed_printf("NOTE\n");
    for (i = 0; i < 0x32; i++)
        jed_printf(i % 10 == 0 ? "0" : " ");
    jed_printf("*\n");
}

/* one product term: the 16 foldback fuses and the 80 input
 * fuses.  A term with all fuses intact is left out (the default is F0!). */
static void jed_write_pterm(int *addr, int mc, int pt)
{
    const char *extra = g_jed_map->pt_extra[mc - 1][pt];
    const char *row = g_jed_map->pt[mc - 1][pt];
    int i;

    if (strchr(extra, '0') == NULL && strchr(row, '0') == NULL) {
        *addr += 0x60;
        return;
    }
    jed_printf("L%d\n", *addr);
    for (i = 0; i < FIT_PT_EXTRA_FUSES; i++)
        jed_put_fuse(extra[i]);
    jed_printf("\n");
    for (i = 0; i < FIT_PT_FUSES; i++) {
        jed_put_fuse(row[i]);
        if (i == 0x27)
            jed_printf("\n");
    }
    *addr += 0x60;
    jed_printf("*  NOTE PT %d of MC %d *\n", pt + 1, mc);
}

/* the five terms of a macrocell; every other macrocell has them in
 * reverse order (the ATF1502 starts with the other one). */
static void jed_write_mc_pterms(int mc)
{
    int addr = g_dev_desc->jedec->pt_addr[mc - 1];
    int down = (mc % 2 == 0);
    int pt;

    if (g_dev_desc->id == FIT_DEV_1502)
        down = !down;
    if (down) {
        for (pt = 4; pt >= 0; pt--)
            jed_write_pterm(&addr, mc, pt);
    } else {
        for (pt = 0; pt < 5; pt++)
            jed_write_pterm(&addr, mc, pt);
    }
}

/* product terms of all macrocells.  The ATF1508 takes the blocks in the
 * order A B C D H G F E, the ATF1504 A B D C, each from its last macrocell
 * down; the ATF1502 takes the blocks in order, each from its first
 * macrocell up. */
static void jed_write_pt_rows(void)
{
    static const int order_1508[8] = { 1, 2, 3, 4, 8, 7, 6, 5 };
    static const int order_1504[4] = { 1, 2, 4, 3 };
    int i, n;

    if (g_dev_desc->id == FIT_DEV_1502) {
        for (i = 0; i < g_dev->n_blocks; i++)
            for (n = 1; n <= 16; n++)
                jed_write_mc_pterms(i * 16 + n);
        return;
    }
    for (i = 0; i < g_dev->n_blocks; i++) {
        int block = (g_dev_desc->id == FIT_DEV_1508) ? order_1508[i] : order_1504[i];
        for (n = 0; n < 16; n++)
            jed_write_mc_pterms(block * 16 - n);
    }
}

/* (option -str DEBUG on): the configuration strings as comments. */
static void jed_write_debug_cfg(void)
{
    int mc, i;

    jed_printf("\n");
    jed_printf("NOTE config for DEBUG\n");
    jed_printf("N        0         0         0 *\n");
    for (mc = 1; mc <= g_dev->n_macrocells; mc++) {
        jed_printf("N MC%d\t", mc);
        for (i = 0; i < FIT_MC_CFG_BITS; i++)
            jed_put_fuse(g_jed_map->mc_cfg[mc - 1][i]);
        jed_printf("*\n");
    }
}

/* comment after a row of configuration fuses. */
static void jed_write_cfg_note(int addr)
{
    const FitJedecLayout *lay = g_dev_desc->jedec;
    int i;

    for (i = 0; i < lay->n_notes; i++) {
        if (lay->notes[i].addr == addr) {
            jed_printf("%s", lay->notes[i].text);
            return;
        }
    }
}

/* the macrocell configuration area, with a NOTE line after every row. */
static void jed_write_cfg_area(void)
{
    const FitJedecLayout *lay = g_dev_desc->jedec;
    int addr = lay->mc_cfg_addr;
    int col = 0;
    int i;

    jed_printf("\nNOTE macrocell configurations");
    jed_printf("\n        0         0         0         0 *\n");
    for (i = 0; i < lay->mc_cfg_fuses; i++) {
        if (col == 0 || col == 0x10 || col == 0x30)
            jed_printf("L%d ", addr);
        jed_put_fuse(g_jed_cfg_area[i]);
        col++;
        if (col == 0x10) {
            jed_printf("*\n");
            addr += 0x10;
        } else if (col == 0x30) {
            jed_printf("*");
            jed_write_cfg_note(addr);
            addr += 0x20;
        } else if (col == 0x50) {
            jed_printf("*");
            jed_write_cfg_note(addr);
            addr += 0x20;
            col = 0;
        }
    }
}

static void jed_write_mux(int *addr, int block, int mux, int descending)
{
    const char *fuses = g_jed_map->uim[block][mux];
    int w = g_dev->uim_width;
    int k;

    jed_printf("L%d ", *addr);
    if (descending) {
        for (k = w - 1; k >= 0; k--)
            jed_put_fuse(fuses[k]);
    } else {
        for (k = 0; k < w; k++)
            jed_put_fuse(fuses[k]);
    }
    jed_printf("*  NOTE Mux-%d of block %c*\n", mux, 'A' + block);
    *addr += w;
}

/* the multiplexers of the block pair (a, b), 0-based.  The ATF1508 and
 * ATF1504 interleave them as a[i] b[i+1] b[i] a[i+1] for i = 0, 2, .. 38
 * with the fuses of a multiplexer written last to first; the ATF1502
 * writes a[i] b[i] for i = 39 .. 0 with the fuses first to last. */
static void jed_write_uim_section(int pair, int a, int b)
{
    int addr = g_dev_desc->jedec->uim_addr[pair];
    int i;

    jed_printf("\nNOTE   UIM for block %c and %c *\n", 'A' + a, 'A' + b);
    jed_printf("NOTE         0         0         0*\n");
    if (g_dev_desc->id == FIT_DEV_1502) {
        for (i = FIT_UIM_PER_BLOCK - 1; i > 0; i -= 2) {
            jed_write_mux(&addr, a, i, 0);
            jed_write_mux(&addr, b, i, 0);
            jed_write_mux(&addr, a, i - 1, 0);
            jed_write_mux(&addr, b, i - 1, 0);
        }
        return;
    }
    for (i = 0; i < FIT_UIM_PER_BLOCK; i += 2) {
        jed_write_mux(&addr, a, i, 1);
        jed_write_mux(&addr, b, i + 1, 1);
        jed_write_mux(&addr, b, i, 1);
        jed_write_mux(&addr, a, i + 1, 1);
    }
}

/* block pairs (0, B-1), (1, B-2), ... */
static void jed_write_uim(void)
{
    int i;

    for (i = 0; i < g_dev->n_blocks / 2; i++)
        jed_write_uim_section(i, i, g_dev->n_blocks - 1 - i);
}

/* the six global OE multiplexers; the ATF1502 writes them last to
 * first. */
static void jed_write_global_oe(void)
{
    int w = g_dev->uim_width;
    int i, k;

    jed_printf("\nNOTE  6 global OE \n");
    jed_printf("0         0         0*\n");
    jed_printf("L%d\n", g_dev_desc->jedec->global_oe_addr);
    for (i = 0; i < FIT_GLOBAL_SLOTS; i++) {
        const char *fuses = g_jed_map->global[g_dev_desc->id == FIT_DEV_1502 ? FIT_GLOBAL_SLOTS - 1 - i : i];
        for (k = 0; k < w; k++)
            jed_put_fuse(fuses[k]);
        jed_printf("\n");
    }
    jed_printf("*\n");
}

static void jed_write_device_bits(void)
{
    int i;

    jed_printf("\nNOTE  device configuration bits*\n");
    jed_printf("NOTE   0         0         0*\n");
    jed_printf("L%d ", g_dev_desc->jedec->cfg_bits_addr);
    for (i = 0x1f; i >= 0; i--)
        jed_put_fuse(g_jed_map->cfg_bits[i]);
    jed_printf("*\n");
}

static void jed_write_jtag_bits(void)
{
    jed_printf("\nNOTE  Special Purpose Bits (JTAG) *\n");
    jed_printf("L%d ", g_dev_desc->jedec->jtag_bits_addr);
    g_jed_map->jtag_bits[0] = g_opt.jtag != 0 ? '1' : '0';
    jed_put_fuse('1');
    jed_put_fuse(g_jed_map->jtag_bits[0]);
    jed_put_fuse('1');
    jed_put_fuse('1');
    jed_printf("*\n");
}

static void jed_write_ues_bits(void)
{
    int i;

    jed_printf("\nNOTE  UES bits*\n");
    jed_printf("L%d ", g_dev_desc->jedec->ues_bits_addr);
    for (i = 0; i < 0x10; i++)
        jed_put_fuse(g_jed_map->ues_bits[i]);
    jed_printf("*\n");
}

static void jed_write_reserved_bits(void)
{
    int i;

    jed_printf("\nNOTE  Reserved bits *\n");
    jed_printf("L%d ", g_dev_desc->jedec->reserved_addr);
    /* the fuses up to QF: two on the ATF1508, six on the ATF1504 and
     * ATF1502 */
    for (i = g_dev_desc->jedec->reserved_addr; i < g_dev_desc->jedec->n_fuses; i++)
        jed_put_fuse('0');
    jed_printf("*\n\n");
}

/* fuse checksum, four hex digits, upper case.  A partly filled last
 * byte is not counted. */
static void jed_write_checksum(void)
{
    jed_printf("C%04X*\n", jedsum_fuses_value(&g_jed_sum));
}

/* Test vector pin map of a package name: the device's own packages first,
 * then the maps for package names of other devices. */
static const char *jed_vector_map(const char *package)
{
    int i;

    for (i = 0; i < g_dev_desc->n_packages; i++)
        if (util_str_icmp(package, g_dev_desc->packages[i].name) == 0)
            return g_dev_desc->packages[i].vector_map;
    for (i = 0; i < g_dev_desc->n_vector_maps; i++)
        if (util_str_icmp(package, g_dev_desc->vector_maps[i].package) == 0)
            return g_dev_desc->vector_maps[i].map;
    return NULL;
}

#define JED_VEC_NAMES   200     /* 200 signal names, 200 columns */
#define JED_VEC_TOKEN   192     /* its token buffer */

/* Reads the next word of the vector file as fscanf("%s") does into a
 * buffer of JED_VEC_TOKEN bytes; a longer word is cut there.  Returns 0
 * at the end of the file. */
static int jed_vec_token(FILE *f, char *tok)
{
    int c = fgetc(f);
    int n = 0;

    while (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f')
        c = fgetc(f);
    if (c == EOF || c == 0x1a)
        return 0;
    while (c != EOF && c != 0x1a && c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\v' && c != '\f') {
        if (n < JED_VEC_TOKEN - 1)
            tok[n++] = (char)c;
        c = fgetc(f);
    }
    tok[n] = '\0';
    return 1;
}

/* Appends the test vectors of <out_base>.tmv, if that file exists.
 * Format of the file as this function reads it: words; a word starting
 * with 'S' and ending with ';' opens a table; then the column names up to
 * "$" with "->" between inputs and outputs; then one word per vector up to
 * "$" (input characters, two separator characters, output characters);
 * then two more words. */
static void jed_append_vectors(const char *out_base, const char *package, FILE *jed)
{
    const char *map = jed_vector_map(package);
    char *tmv_name = util_strdup(out_base);
    int sig_pin[JED_VEC_NAMES];
    const char *sig_name[JED_VEC_NAMES];
    char *col_name[2 * JED_VEC_NAMES];
    int col_pin[2 * JED_VEC_NAMES];
    int col_src[2 * JED_VEC_NAMES];
    char tok[JED_VEC_TOKEN];
    char **vectors = NULL;
    int n_vectors = 0, cap_vectors = 0;
    int n_sig = 0;
    int n_inputs = 0;
    size_t map_len = map != NULL ? strlen(map) : 0;
    FILE *f;
    int i;

    util_str_append(&tmv_name, ".tmv");
    for (i = 0; i < jed_count(g_jed_slots); i++) {
        FitSlot *s = g_jed_slots->items[i];
        if (util_str_icmp(s->signal, FIT_FREE_NAME) != 0 && n_sig < JED_VEC_NAMES) {
            sig_pin[n_sig] = s->pin;
            sig_name[n_sig] = s->signal;
            n_sig++;
        }
    }
    f = fopen(tmv_name, "r");
    free(tmv_name);
    if (f == NULL)
        return;
    if (map == NULL) {
        /* no pin map for this package: no vectors */
        fclose(f);
        return;
    }
    if (jed_vec_token(f, tok)) {
        int more = 1;
        while (more) {
            int n_cols = 0, n_extra = 0;
            int c;

            if (!(tok[0] == 'S' && tok[strlen(tok) - 1] == ';')) {
                if (!jed_vec_token(f, tok))
                    more = 0;
                else
                    continue;
            }
            jed_vec_token(f, tok);
            while (util_str_icmp("$", tok) != 0) {
                if (util_str_icmp("->", tok) == 0) {
                    n_inputs = n_cols;
                } else if (n_cols < JED_VEC_NAMES) {
                    col_name[n_cols] = util_strdup(tok);
                    n_cols++;
                }
                if (!jed_vec_token(f, tok))
                    break;      /* truncated file */
            }
            for (c = 0; c < n_cols; c++) {
                int found = 0;
                int s;
                for (s = 0; s < n_sig; s++) {
                    if (util_str_icmp(col_name[c], sig_name[s]) == 0) {
                        if (found) {
                            /* the signal is on a second pin: an extra column that
                             * repeats column c */
                            if (n_cols + n_extra < 2 * JED_VEC_NAMES) {
                                col_pin[n_cols + n_extra] = sig_pin[s];
                                col_src[n_cols + n_extra] = c;
                                n_extra++;
                            }
                        } else {
                            col_pin[c] = sig_pin[s];
                        }
                        found = 1;
                    }
                }
                if (!found)
                    col_pin[c] = 999;
            }
            jed_vec_token(f, tok);
            while (util_str_icmp("$", tok) != 0) {
                char *v = util_strdup(map);
                size_t tok_len = strlen(tok);

                for (c = 0; c < n_inputs; c++)
                    if (col_pin[c] != 999 && (size_t)c < tok_len
                        && col_pin[c] >= 1 && (size_t)col_pin[c] <= map_len)
                        v[col_pin[c] - 1] = tok[c];
                for (c = n_cols; c < n_cols + n_extra; c++)
                    if (col_pin[c] != 999 && (size_t)col_src[c] < tok_len
                        && col_pin[c] >= 1 && (size_t)col_pin[c] <= map_len)
                        v[col_pin[c] - 1] = tok[col_src[c]];
                for (c = n_inputs + 2; c < n_cols + 2; c++) {
                    int pin = col_pin[c - 2];
                    if (pin != 999 && pin >= 1 && (size_t)pin <= map_len && v[pin - 1] == 'X'
                        && (size_t)c < tok_len) {
                        char ch = tok[c];
                        if (ch == '0' || ch == 'L')
                            v[pin - 1] = 'L';
                        else if (ch == '1' || ch == 'H')
                            v[pin - 1] = 'H';
                        else if (ch == 'Z')
                            v[pin - 1] = 'Z';
                    }
                }
                if (n_vectors == cap_vectors) {
                    cap_vectors = cap_vectors != 0 ? cap_vectors * 2 : 256;
                    vectors = util_realloc(vectors, (size_t)cap_vectors * sizeof *vectors);
                }
                vectors[n_vectors++] = v;
                if (!jed_vec_token(f, tok))
                    break;
            }
            for (c = 0; c < n_cols; c++)
                free(col_name[c]);
            jed_vec_token(f, tok);
            if (!jed_vec_token(f, tok))
                more = 0;
        }
    }
    fclose(f);
    if (jed == NULL) {
        fprintf(g_msg, "Unable to open a jedec file\n");
    } else {
        if (n_vectors != 0)
            jed_printf("QV%d* X0*\n", n_vectors);
        for (i = 0; i < n_vectors; i++) {
            if (i < 9)
                jed_printf("V000%d ", i + 1);
            else if (i < 99)
                jed_printf("V00%d ", i + 1);
            else
                jed_printf("V0%d ", i + 1);
            /* a vector can be longer than jed_printf's buffer */
            fputs(vectors[i], g_jed_file);
            jed_printf("*\n");
        }
    }
    for (i = 0; i < n_vectors; i++)
        free(vectors[i]);
    free(vectors);
}

int jed_write_file(const char *out_base)
{
    char *name = util_strdup(out_base);

    util_str_append(&name, ".jed");
    jedsum_fuses_init(&g_jed_sum);
    g_jed_sum_on = 1;
    g_jed_file = file_open_write(name);
    free(name);
    if (g_jed_file == NULL)
        return 0;                       /* the file cannot be created */
    jed_write_header();
    jed_write_pt_rows();
    g_jed_sum_on = 0;
    if (g_opt.debug != 0)
        jed_write_debug_cfg();
    g_jed_sum_on = 1;
    jed_write_cfg_area();
    jed_write_uim();
    jed_write_global_oe();
    jed_write_device_bits();
    jed_write_jtag_bits();
    jed_write_ues_bits();
    jed_write_reserved_bits();
    jed_write_checksum();
    jed_append_vectors(out_base, g_dev_package, g_jed_file);
    jed_printf("%c", 3);
    jed_printf("0000\n");
    fflush(g_jed_file);
    return 1;
}
