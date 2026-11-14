/*
 * lib_techmap.c - techmap module of the ATF150x fitter: adapts the netlist
 * of the design to the gate library ("extra
 * wiring").  For every sequential or tri-state cell it looks for a library
 * gate with the same pins; where there is none it changes polarities of
 * control inputs, or rebuilds the cell from what the library has (T, JK,
 * RS and mux flip-flops from D flip-flops, clock enables as multiplexers,
 * missing set/reset as an extra "SBOX" cell, tri-state outputs as separate
 * tri-state cells), adding functions to the entity for the glue logic.
 *
 * With atmel.std and the CUPL flow about half of this file runs.
 *
 * The messages of this module go through techmap_message, which prints
 * only for a negative level; every caller passes 1 or 2, so nothing is
 * ever printed (except with LIB_TRACE).  The calls are kept as they are.
 *
 * The three option globals of the module, g_ts_flags, g_lib_strict_async
 * and g_lib_latch_mode, are never set to anything but zero (their only
 * writes are the resets in techmap_reset and lib_load_library).  The code
 * they guard is complete all the same.
 */
#include <signal.h>

#include "lib_priv.h"

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static FitList *g_inv_cache;      /* TechmapInvPair: signals already inverted */
static FitList *g_rebuilt;        /* names (not owned) of instances that got an SBOX */
int             g_lib_strict_async;
static unsigned g_ts_flags;       /* bit 0 / bit 1; never set */
static char    *g_tie_for_1;      /* name of the variable made by techmap_tie_constant(1) */
static char    *g_tie_for_0;      /* the same for any other value */
char           *g_lib_latch_mode; /* "comb", "no" or NULL; never set */
static int      g_no_ts_warned;

/* item of g_inv_cache */
typedef struct TechmapInvPair {
    char *orig;
    char *inverted;
} TechmapInvPair;

void techmap_reset(void)
{
    g_inv_cache = NULL;
    g_rebuilt = NULL;
    g_lib_strict_async = 0;
    g_ts_flags = 0;
    g_tie_for_1 = NULL;
    g_tie_for_0 = NULL;
    g_lib_latch_mode = NULL;
    g_no_ts_warned = 0;
}

/* Called where a pointer the code relies on is missing or a structure is
 * inconsistent.  Raises SIGSEGV, so that the signal handler installed by
 * main ends the job: "INTERNAL ERROR - Please contact your Hot-Line",
 * exit status 7.  `what` says what is wrong; it is not printed, so the
 * console shows only the handler's message. */
void lib_internal_error(const char *what)
{
    (void)what;
    raise(SIGSEGV);
    util_abort_job(99);     /* not reached with the handler installed */
}

/* util_strdup of a string that must be there; NULL is an internal error */
static char *dup_str(const char *s)
{
    if (s == NULL)
        lib_internal_error("null string");
    return util_strdup(s);
}

static FitVariable *need_var(FitVariable *v)
{
    if (v == NULL)
        lib_internal_error("variable not found");
    return v;
}

static FitNetItem *netitem_raw(int kind, void *cell)
{
    FitNetItem *it = util_calloc(1, sizeof *it);   /* calloc(1, 8) */

    it->kind = kind;
    it->cell = cell;
    return it;
}

static void techmap_set_polarity(FitEntity *ent, char **sig, int *pol, int want, int direct);
static int  techmap_map_instance(FitEntity *ent, FitNetItem *it, FitList *groups);
static FitFct *techmap_new_mux_fct(FitEntity *ent, char *a, char *b, char *sel, char *out);

/* ------------------------------------------------------------------ */

void techmap_add_wire_fct(FitEntity *ent, FitVariable *dst, FitVariable *src)
{
    LIB_TRACE_F("@wire %s %s\n", dst != NULL ? dst->name : "(null)", src != NULL ? src->name : "(null)");
    FitFct *f;

    if (dst == NULL || src == NULL)
        lib_internal_error("variable not found");
    if (ent->fcts == NULL)
        ent->fcts = list_create();
    f = fct_new();
    f->var_id = dst->id;
    f->tree[0] = fcttree_new_id(src->id);
    list_add(ent->fcts, f);
}

/* prints only for a negative level (never) */
void techmap_message(const char *fmt, const char *a, const char *b, int level)
{
#ifdef LIB_TRACE
    level = -1;
#endif
    if (level < 0)
        fprintf(g_msg, fmt, a != NULL ? a : "(null)", b != NULL ? b : "(null)");
}

/* 1 and the value if the function of `id` is a constant node.
 * An id without a function (0 for a signal that is not an output) is an
 * internal error. */
static int techmap_const_value_of(FitEntity *ent, int id, int *value)
{
    FitFctTree *t = entity_fct_tree(ent, id);

    if (t == NULL)
        lib_internal_error("signal without function");
    if (!tt2_tree_is_const(t) || t->sel != FIT_SEL_CONST)
        return 0;
    *value = t->u.value;
    return 1;
}

/* 1 if `name` is an output (not an input) whose function is the
 * constant that makes a control pin of polarity `pol` permanently
 * inactive: constant 1 with polarity -1, or constant 0 with polarity 1. */
static int techmap_ctrl_is_tied_off(FitEntity *ent, char *name, int pol)
{
    int value = 0;

    if (name != NULL && var_list_id_of_name(ent->inputs, name) == 0) {
        int id = var_list_id_of_name(ent->outputs, name);

        if (techmap_const_value_of(ent, id, &value)) {
            if (value == 1) {
                if (pol == -1)
                    return 1;
            } else if (value == 0 && pol == 1) {
                return 1;
            }
        }
    }
    return 0;
}

/* adds the function  to = from.  `to` stops being an input
 * (lib_input_to_output) if it is one. */
static void techmap_connect(FitEntity *ent, char *from, char *to)
{
    FitVariable *dst = lib_input_to_output(ent, to);
    FitVariable *src;

    if (dst == NULL)
        dst = entity_find_var(ent, to);
    src = var_list_find_name(ent->inputs, from);
    if (src == NULL)
        src = entity_find_var(ent, from);
    techmap_add_wire_fct(ent, dst, src);
}

/* the input `name` becomes an output defined as the negation of
 * a new input; returns the name of that new input (it lives in the
 * variable, not a copy), or NULL when `name` is not an input. */
static char *techmap_input_to_inverted(FitEntity *ent, char *name)
{
    FitVariable *v = lib_input_to_output(ent, name);
    FitVariable *nv;

    if (v == NULL)
        return NULL;
    nv = lib_new_variable(ent, 0, 1);
    techmap_add_wire_fct(ent, v, nv);
    tt2_invert_fct(ent, v->id);
    return nv->name;
}

/* removes the tri-state cell at index idx and wires its output
 * to its input (inverted for an inverting cell). */
static void techmap_remove_tristate(FitEntity *ent, FitNetItem *it, int idx)
{
    LIB_TRACE_F("@rm_tristate %s\n", LIB_TRACE_S(INS_NAME((FitInstance *)it->cell)));
    FitInstance *ins = it->cell;

    list_del_shift(ent->net->items, idx + 1);
    if (it->kind == FIT_GATE_ITH)
        INS_Q(ins) = techmap_input_to_inverted(ent, INS_Q(ins));   /* not a copy; the old string is lost */
    techmap_connect(ent, INS_D(ins), INS_Q(ins));
    lib_output_to_local(ent, INS_D(ins));
    lib_output_to_local(ent, INS_OE(ins));
    netitem_free(&it);
}

/* removes the tri-state pad buffer at index idx: pad = in. */
static void techmap_remove_ts_buffer(FitEntity *ent, FitNetItem *it, int idx)
{
    LIB_TRACE_F("@rm_tsbuf %s\n", LIB_TRACE_S(BUF_NAME((FitCell13 *)it->cell)));
    FitCell13 *c = it->cell;

    list_del_shift(ent->net->items, idx + 1);
    if (BUF_IN(c) != NULL)
        techmap_connect(ent, BUF_IN(c), BUF_PAD(c));
    lib_output_to_local(ent, BUF_IN(c));
    lib_output_to_local(ent, BUF_OE(c));
    netitem_free(&it);
}

/* drops tri-state cells and tri-state buffers whose enable is a
 * constant (techmap_ctrl_is_tied_off).  Returns how many. */
static int techmap_remove_inactive_tristates(FitEntity *ent)
{
    FitList *items;
    int removed = 0;
    int i = 0;

    if (ent->net == NULL)
        return 0;
    items = ent->net->items;
    while (i < LCOUNT(items)) {
        FitNetItem *it = items->items[i];

        if (it->kind == FIT_GATE_TH || it->kind == FIT_GATE_ITH) {
            FitInstance *ins = it->cell;

            if (techmap_ctrl_is_tied_off(ent, INS_OE(ins), INS_OE_POL(ins))) {
                techmap_message("Warning -  Removing tri-state of instance %s: tri-state pin is never active\n",
                                INS_NAME(ins), NULL, 1);
                techmap_remove_tristate(ent, it, i);
                removed++;
                continue;
            }
        } else if (it->kind == FIT_GATE_BUF && BUF_TYPE((FitCell13 *)it->cell) == FIT_BUF_TS) {
            FitCell13 *c = it->cell;

            if (techmap_ctrl_is_tied_off(ent, BUF_OE(c), BUF_OE_POL(c))) {
                techmap_message("Warning -  Removing buffer of instance %s: tri-state pin is never active\n",
                                BUF_NAME(c), NULL, 1);
                techmap_remove_ts_buffer(ent, it, i);
                removed++;
                continue;
            }
        }
        i++;
    }
    return removed;
}

/* FitListFree */
static void techmap_inv_pair_free(void **ref)
{
    TechmapInvPair *p = *ref;

    free(p->orig);
    free(p->inverted);
    free(p);
    *ref = NULL;
}

/* 1 if the library has a buffer like this cell; -1 if none but
 * some library buffer has the other enable polarity; else 0. */
static int techmap_match_buffer(FitCell13 *c, FitList *groups)
{
    FitLibKindGroup *g = lib_kind_group_get(groups, FIT_GATE_BUF);
    FitList *gl;
    int result = 0;

    if (g == NULL)
        return 0;
    gl = g->gates;
    for (int i = 0; i < LCOUNT(gl); i++) {
        FitLibBuf *b = ((FitLibGate *)gl->items[i])->cell;

        if (b->type == BUF_TYPE(c)
            && lib_same_presence(BUF_IN(c), b->in)
            && lib_ctrl_same(BUF_OE(c), BUF_OE_POL(c), b->oe, b->oe_pol)
            && lib_same_presence(BUF_OUT(c), b->out)
            && lib_same_presence(BUF_PAD(c), b->pad))
            return 1;
        if (BUF_OE(c) != NULL && BUF_OE_POL(c) != b->oe_pol)
            result = -1;
    }
    return result;
}

/* a buffer the library does not have becomes a plain wire (or
 * disappears). */
static void techmap_buffer_to_wire(FitEntity *ent, FitNetItem *it, int idx)
{
    LIB_TRACE_F("@buf2wire %s\n", LIB_TRACE_S(BUF_NAME((FitCell13 *)it->cell)));
    FitCell13 *c = it->cell;
    char *name;

    list_del_shift(ent->net->items, idx + 1);
    if (BUF_PAD(c) == NULL) {
        fprintf(g_msg, "Instance %s of buffer has no pad. This instance is removed.\n",
                BUF_NAME(c) != NULL ? BUF_NAME(c) : "(null)");
        /* The cell's `out` and `in` names are used before the cell is
         * released. */
        if (BUF_OUT(c) != NULL)
            lib_output_to_local(ent, BUF_OUT(c));
        if (BUF_IN(c) != NULL)
            lib_output_to_local(ent, BUF_IN(c));
        netitem_free(&it);
        return;
    }
    techmap_message("Info -  Target library has no buffers.\n\tReplacing buffer %s by a single wire.\n ",
                    BUF_NAME(c), NULL, 2);
    switch (BUF_TYPE(c)) {
    case FIT_BUF_O:
        techmap_connect(ent, BUF_IN(c), BUF_PAD(c));
        name = BUF_IN(c);
        break;
    case FIT_BUF_I:
    case FIT_BUF_CLK:
        techmap_connect(ent, BUF_PAD(c), BUF_OUT(c));
        name = BUF_OUT(c);
        break;
    default:
        netitem_free(&it);
        return;
    }
    lib_output_to_local(ent, name);
    netitem_free(&it);
}

/* the cell stops using the signal *ref: frees the string, lets
 * the variable become a local if nothing else needs it as an output, and
 * returns the variable's own name. */
static char *techmap_release_signal(FitEntity *ent, char **ref)
{
    LIB_TRACE_F("@release %s\n", LIB_TRACE_S(*ref));
    FitVariable *v = need_var(entity_find_var(ent, *ref));

    free(*ref);
    *ref = NULL;
    lib_output_to_local(ent, v->name);
    return v->name;
}

/* connects an unused control pin to a new constant output.  The
 * constant is 0 for value 1 and 1 otherwise; one variable is shared by all
 * pins asking for 1 and one by those asking for 0 (a request for -1 makes a
 * new variable every time and replaces the shared "0" one). */
static void techmap_tie_constant(FitEntity *ent, char **sig, int value)
{
    LIB_TRACE_F("@tie %d\n", value);
    FitVariable *nv;
    FitFctTree *tree;
    FitFct *f;

    if (value == 1) {
        if (g_tie_for_1 != NULL) {
            *sig = util_strdup(g_tie_for_1);
            return;
        }
    } else if (value == 0 && g_tie_for_0 != NULL) {
        *sig = util_strdup(g_tie_for_0);
        return;
    }
    nv = lib_new_variable(ent, 1, 1);
    tree = fcttree_new_const(value != 1);
    if (value == 1)
        g_tie_for_1 = nv->name;
    else
        g_tie_for_0 = nv->name;
    *sig = util_strdup(nv->name);
    f = fct_new();
    f->var_id = nv->id;
    f->tree[0] = tree;
    list_add(ent->fcts, f);
}

/* makes the control pin (*sig, *pol) have polarity `want`.  An
 * unconnected pin is tied to a constant; a connected pin of the other
 * polarity gets an inverted copy of its signal (one per signal, kept in
 * g_inv_cache). */
static void techmap_set_polarity(FitEntity *ent, char **sig, int *pol, int want, int direct)
{
    LIB_TRACE_F("@setpol %s %d %d %d\n", LIB_TRACE_S(*sig), *pol, want, direct);
    FitVariable *nv;
    TechmapInvPair *pair;
    char *old;
    int pos;

    if (*sig == NULL) {
        techmap_tie_constant(ent, sig, direct != 0 ? want : -want);
        *pol = want;
        return;
    }
    if (*pol == want)
        return;
    pos = list_find(g_inv_cache, *sig, tt2_name_ref_equal);
    if (pos != 0) {
        techmap_release_signal(ent, sig);
        *sig = util_strdup(((TechmapInvPair *)g_inv_cache->items[pos - 1])->inverted);
        *pol = want;
        return;
    }
    old = techmap_release_signal(ent, sig);
    nv = lib_new_variable(ent, 1, 1);
    pair = util_calloc(1, sizeof *pair);
    pair->orig = util_strdup(old);
    pair->inverted = util_strdup(nv->name);
    list_add(g_inv_cache, pair);
    *sig = util_strdup(nv->name);
    techmap_add_wire_fct(ent, nv, entity_find_var(ent, old));
    tt2_invert_fct(ent, nv->id);
    *pol = want;
}

/* if the pin is driven by an output that is just the negation of
 * an input without an alias, connects the pin to that input and flips the
 * polarity (for the clock: rising <-> falling, high <-> low level). */
static void techmap_absorb_inverter(FitEntity *ent, char **sig, int *pol, int is_clock)
{
    FitFctTree *t, *child;
    FitVariable *in;
    int id;

    if (*sig == NULL)
        return;
    id = var_list_id_of_name(ent->outputs, *sig);
    if (id == 0)
        return;
    t = entity_fct_tree(ent, id);
    if (t == NULL)
        lib_internal_error("output without function");
    if (t->sel != FIT_SEL_NOT)
        return;
    child = t->u.children->items[0];
    if (child->sel != FIT_SEL_ID)
        return;
    in = var_list_find_id(ent->inputs, child->u.id);
    if (in == NULL || lib_id_has_alias(child->u.id, ent))
        return;
    techmap_release_signal(ent, sig);
    *sig = util_strdup(in->name);
    if (!is_clock) {
        *pol = (*pol != 1) ? 1 : -1;
        return;
    }
    switch (*pol) {
    case FIT_CLK_RECK: *pol = FIT_CLK_FECK; break;
    case FIT_CLK_FECK: *pol = FIT_CLK_RECK; break;
    case FIT_CLK_L0:   *pol = FIT_CLK_L1;   break;
    case FIT_CLK_L1:   *pol = FIT_CLK_L0;   break;
    default: break;
    }
}

/* simplifications of a sequential cell before it is matched:
 * inverters in front of the control pins are absorbed, control pins tied
 * to their inactive level are disconnected, and a tri-state output whose
 * enable is tied becomes a plain Q output. */
static void techmap_prepare_instance(FitEntity *ent, FitNetItem *it)
{
    FitInstance *ins = it->cell;

    techmap_absorb_inverter(ent, &INS_CLK(ins), &INS_CLK_KIND(ins), 1);
    if (techmap_ctrl_is_tied_off(ent, INS_AP(ins), INS_AP_POL(ins)))
        techmap_release_signal(ent, &INS_AP(ins));
    techmap_absorb_inverter(ent, &INS_AP(ins), &INS_AP_POL(ins), 0);
    if (techmap_ctrl_is_tied_off(ent, INS_AR(ins), INS_AR_POL(ins)))
        techmap_release_signal(ent, &INS_AR(ins));
    techmap_absorb_inverter(ent, &INS_AR(ins), &INS_AR_POL(ins), 0);
    if (techmap_ctrl_is_tied_off(ent, INS_CE(ins), -INS_CE_POL(ins)))
        techmap_release_signal(ent, &INS_CE(ins));
    techmap_absorb_inverter(ent, &INS_CE(ins), &INS_CE_POL(ins), 0);
    if (techmap_ctrl_is_tied_off(ent, INS_OE(ins), INS_OE_POL(ins))
        && it->kind != FIT_GATE_TH && it->kind != FIT_GATE_ITH
        && INS_Z(ins) != NULL && INS_Q(ins) == NULL) {
        techmap_release_signal(ent, &INS_OE(ins));
        INS_Q(ins) = util_strdup(INS_Z(ins));
        free(INS_Z(ins));
        INS_Z(ins) = NULL;
    }
    techmap_absorb_inverter(ent, &INS_OE(ins), &INS_OE_POL(ins), 0);
}

/* penalty for one control pin of the cell against the gate's */
static int techmap_ctrl_cost(char *a, int a_pol, char *b, int b_pol, int missing)
{
    if (!lib_pin_available(a, b))
        return missing;
    if (b == NULL)
        return 0;
    if (a == NULL)
        return 5;
    return lib_ctrl_same(a, a_pol, b, b_pol) ? 0 : 0x14;
}

/* can the cell be turned into this gate, and at what cost */
static int techmap_gate_cost(int kind, FitInstance *ins, FitLibSeq *g, int *cost)
{
    int ik, gk, c;

    *cost = 0;
    if ((kind == FIT_GATE_DMUX || kind == FIT_GATE_LMUX) && INS_CE(ins) != NULL && g->ce == NULL)
        return 0;
    if (!lib_inputs_match(ins, g)) {
        if (g->d == NULL) {
            if (g->mux_a == NULL)
                return 0;
            if (g->mux_b == NULL)
                return 0;
            if (g->mux_sel == NULL)
                return 0;
        }
        *cost += 200;
    }
    if (INS_Q(ins) == NULL && INS_QN(ins) == NULL && INS_Z(ins) == NULL)
        return 0;
    if (!lib_same_presence(INS_CLK(ins), g->clk))
        return 0;
    ik = INS_CLK_KIND(ins);
    gk = g->clk_kind;
    if (ik == FIT_CLK_FECK || ik == FIT_CLK_RECK) {
        if (gk != FIT_CLK_FECK && gk != FIT_CLK_RECK)
            return 0;
    } else {
        if (gk == FIT_CLK_FECK)
            return 0;
        if (gk == FIT_CLK_RECK)
            return 0;
    }
    *cost += (ik != gk) ? 500 : 0;
    if (!((g_lib_strict_async == 0 || g->ap == NULL || g->ar == NULL
           || (INS_AP(ins) != NULL && INS_AR(ins) != NULL))
          && ((INS_AP(ins) == NULL && INS_AR(ins) == NULL) || g->ap != NULL || g->ar != NULL)))
        return 0;
    if (INS_Z(ins) != NULL) {
        if (INS_OE(ins) == NULL)
            return 0;
        if (g->q == NULL && g->qn == NULL)
            return 0;
        if (g->z == NULL) {
            if (g_ts_flags == 0)
                return 0;
            *cost += 200;
        }
    }
    *cost += !lib_pin_available(INS_Q(ins), g->q);
    *cost += !lib_pin_available(INS_QN(ins), g->qn);
    if (kind == FIT_GATE_JK || kind == FIT_GATE_RS) {
        *cost += techmap_ctrl_cost(INS_AP(ins), INS_AP_POL(ins), g->ap, g->ap_pol, 200);
        *cost += techmap_ctrl_cost(INS_AR(ins), INS_AR_POL(ins), g->ar, g->ar_pol, 200);
        *cost += techmap_ctrl_cost(INS_J(ins), INS_J_POL(ins), g->j, g->j_pol, 100);
        c = techmap_ctrl_cost(INS_K(ins), INS_K_POL(ins), g->k, g->k_pol, 100);
    } else {
        *cost += techmap_ctrl_cost(INS_AP(ins), INS_AP_POL(ins), g->ap, g->ap_pol, 200);
        c = techmap_ctrl_cost(INS_AR(ins), INS_AR_POL(ins), g->ar, g->ar_pol, 200);
    }
    *cost += c;
    *cost += techmap_ctrl_cost(INS_CE(ins), INS_CE_POL(ins), g->ce, g->ce_pol, 100);
    *cost += techmap_ctrl_cost(INS_OE(ins), INS_OE_POL(ins), g->oe, g->oe_pol, 100);
    return 1;
}

/* the cheapest gate the cell can be turned into (first of equal
 * cost), NULL if none */
static FitLibSeq *techmap_best_gate(FitNetItem *it, FitList *gl)
{
    FitInstance *ins = it->cell;
    FitLibSeq *best = NULL;
    int best_cost = 10000;
    int cost;

    for (int i = 0; i < LCOUNT(gl); i++) {
        FitLibSeq *g = ((FitLibGate *)gl->items[i])->cell;

        if (techmap_gate_cost(it->kind, ins, g, &cost) && cost < best_cost) {
            best_cost = cost;
            best = g;
        }
    }
    return best;
}

static void techmap_connect_open_input(FitEntity *ent, char *inst, const char *pin, char **ref)
{
    if (*ref == NULL) {
        FitVariable *nv;

        techmap_message("Warning -  Input %s of %s is not connected\n", pin, inst, 1);
        nv = lib_new_variable(ent, 0, 1);
        *ref = util_strdup(nv->name);
    }
}

/* unconnected data inputs get a new input variable */
static void techmap_connect_open_inputs(FitEntity *ent, FitNetItem *it)
{
    FitInstance *ins = it->cell;

    if (it->kind == FIT_GATE_DFF) {
        techmap_connect_open_input(ent, INS_NAME(ins), "D", &INS_D(ins));
    } else if (it->kind == FIT_GATE_DMUX || it->kind == FIT_GATE_LMUX) {
        techmap_connect_open_input(ent, INS_NAME(ins), "A", &INS_MUX_A(ins));
        techmap_connect_open_input(ent, INS_NAME(ins), "B", &INS_MUX_B(ins));
        techmap_connect_open_input(ent, INS_NAME(ins), "Sel", &INS_MUX_SEL(ins));
    }
}

/* appends  out = a <sel> b */
static void techmap_add_binary_fct(FitEntity *ent, int sel, char *out, char *a, char *b)
{
    int id_out = entity_var_id(ent, out);
    int id_a = entity_var_id(ent, a);
    int id_b = entity_var_id(ent, b);
    FitFct *f = fct_new();

    f->var_id = id_out;
    f->tree[0] = fcttree_new_binary_ids(sel, id_a, id_b);
    list_add(ent->fcts, f);
}

/* new function  out = b * sel + a * !sel */
static FitFct *techmap_new_mux_fct(FitEntity *ent, char *a, char *b, char *sel, char *out)
{
    int id_a = entity_var_id(ent, a);
    int id_b = entity_var_id(ent, b);
    int id_sel = entity_var_id(ent, sel);
    FitFctTree *t1 = fcttree_new_binary_ids(FIT_SEL_AND, id_b, id_sel);
    FitFctTree *nsel = fcttree_new_not(fcttree_new_id(id_sel));
    FitFctTree *t2 = fcttree_new_binary(FIT_SEL_AND, fcttree_new_id(id_a), nsel);
    FitFct *f = fct_new();

    f->var_id = entity_var_id(ent, out);
    f->tree[0] = fcttree_new_binary(FIT_SEL_OR, t1, t2);
    return f;
}

/* the library gate has no clock enable.  A D flip-flop or latch
 * becomes a mux flip-flop / mux latch selecting between the data and its
 * own output; other cells get the multiplexer as a function in front of
 * the data input. */
static void techmap_ce_to_mux(FitEntity *ent, FitNetItem *it, int gate_has_q)
{
    FitInstance *ins = it->cell;
    FitVariable *nv;
    char *feedback = NULL;

    techmap_message("Info -  Target library has no clock enable on flip flops.\n"
                    "\tCreating mux for Clock Enable %s on instance %s\n",
                    INS_CE(ins), INS_NAME(ins), 2);
    if (INS_Q(ins) == NULL && INS_QN(ins) == NULL) {
        nv = lib_new_variable(ent, 0, 1);
        if (!gate_has_q)
            INS_QN(ins) = util_strdup(nv->name);
        else
            INS_Q(ins) = util_strdup(nv->name);
    }
    if (INS_Q(ins) != NULL) {
        feedback = INS_Q(ins);
    } else if (INS_QN(ins) != NULL) {
        nv = lib_new_variable(ent, 2, 0);
        techmap_add_wire_fct(ent, nv, entity_find_var(ent, INS_QN(ins)));
        tt2_invert_fct(ent, nv->id);
        feedback = nv->name;
    }
    if (it->kind == FIT_GATE_DFF) {
        it->kind = FIT_GATE_DMUX;
    } else if (it->kind == FIT_GATE_LATCH) {
        it->kind = FIT_GATE_LMUX;
    } else {
        FitFct *f;

        nv = lib_new_variable(ent, 1, 1);
        if (INS_CE_POL(ins) == 1)
            f = techmap_new_mux_fct(ent, feedback, INS_D(ins), INS_CE(ins), nv->name);
        else
            f = techmap_new_mux_fct(ent, INS_D(ins), feedback, INS_CE(ins), nv->name);
        list_add(ent->fcts, f);
        techmap_release_signal(ent, &INS_D(ins));
        techmap_release_signal(ent, &INS_CE(ins));
        INS_D(ins) = util_strdup(nv->name);
        return;
    }
    INS_MUX_SEL(ins) = INS_CE(ins);
    INS_CE(ins) = NULL;
    if (INS_CE_POL(ins) == 1) {
        INS_MUX_A(ins) = dup_str(feedback);
        INS_MUX_B(ins) = INS_D(ins);
        INS_D(ins) = NULL;
        return;
    }
    INS_MUX_A(ins) = INS_D(ins);
    INS_MUX_B(ins) = dup_str(feedback);
    INS_D(ins) = NULL;
}

/* clock enable of a JK or RS flip-flop: J and K are gated with
 * the enable */
static void techmap_ce_to_jk_gating(FitEntity *ent, FitInstance *ins)
{
    FitVariable *nv;
    char *ce, *j, *k;

    techmap_message("Info -  Target library has no clock enable on flip flops.\n"
                    "\tCreating mux for Clock Enable %s on instance %s\n",
                    INS_CE(ins), INS_NAME(ins), 2);
    ce = techmap_release_signal(ent, &INS_CE(ins));
    j = techmap_release_signal(ent, &INS_J(ins));
    k = techmap_release_signal(ent, &INS_K(ins));
    INS_CE(ins) = NULL;
    if (INS_CE_POL(ins) == -1) {
        FitVariable *v = var_list_find_name(ent->outputs, ce);

        if (v == NULL) {
            nv = lib_new_variable(ent, 2, 0);
            techmap_add_wire_fct(ent, nv, entity_find_var(ent, ce));
            tt2_invert_fct(ent, nv->id);
            ce = nv->name;
        } else {
            tt2_invert_fct(ent, v->id);
        }
    }
    nv = lib_new_variable(ent, 1, 1);
    techmap_add_binary_fct(ent, FIT_SEL_AND, nv->name, j, ce);
    INS_J(ins) = util_strdup(nv->name);
    nv = lib_new_variable(ent, 1, 1);
    techmap_add_binary_fct(ent, FIT_SEL_AND, nv->name, k, ce);
    INS_K(ins) = util_strdup(nv->name);
}

/* the signal *ref is replaced by a new output that is its negation */
static void techmap_invert_input(FitEntity *ent, char **ref)
{
    char *old = techmap_release_signal(ent, ref);
    FitVariable *nv = lib_new_variable(ent, 1, 1);

    techmap_add_wire_fct(ent, nv, entity_find_var(ent, old));
    tt2_invert_fct(ent, nv->id);
    *ref = util_strdup(nv->name);
}

/* the gate has the other one of set / reset: the cell is turned
 * upside down (Q <-> QN, data inverted, set <-> reset). */
static void techmap_swap_q_qn(FitEntity *ent, FitNetItem *it)
{
    FitInstance *ins = it->cell;
    char *s;
    int p;

    s = INS_Q(ins);
    INS_Q(ins) = INS_QN(ins);
    INS_QN(ins) = s;
    switch (it->kind) {
    case FIT_GATE_DFF:
    case FIT_GATE_LATCH:
        techmap_invert_input(ent, &INS_D(ins));
        break;
    case FIT_GATE_JK:
        s = INS_K(ins);
        INS_K(ins) = INS_J(ins);
        INS_J(ins) = s;
        p = INS_J_POL(ins);
        INS_J_POL(ins) = INS_K_POL(ins);
        INS_K_POL(ins) = p;
        break;
    case FIT_GATE_DMUX:
    case FIT_GATE_LMUX:
        techmap_invert_input(ent, &INS_MUX_A(ins));
        techmap_invert_input(ent, &INS_MUX_B(ins));
        break;
    case FIT_GATE_RS:
        INS_J_POL(ins) = -INS_J_POL(ins);
        INS_K_POL(ins) = -INS_K_POL(ins);
        break;
    default:
        break;
    }
    s = INS_AR(ins);
    INS_AR(ins) = INS_AP(ins);
    INS_AP(ins) = s;
    p = INS_AP_POL(ins);
    INS_AP_POL(ins) = INS_AR_POL(ins);
    INS_AR_POL(ins) = p;
}

/* the extra cell of techmap_make_sbox: a flip-flop or latch with
 * the clock, enables and the one asynchronous input of `ins` (moved to the
 * other pin), whose data input is a constant. */
static FitInstance *techmap_new_sbox_instance(FitEntity *ent, FitInstance *ins, int constant,
                                              unsigned outs)
{
    FitVariable *nv = lib_new_variable(ent, 1, 1);
    FitInstance *s;
    FitFct *f = fct_new();

    f->var_id = nv->id;
    f->tree[0] = fcttree_new_const(constant);
    list_add(ent->fcts, f);
    s = util_calloc(1, sizeof *s);    /* not instance_new: polarities start as 0, not -1 */
    if (INS_NAME(ins) != NULL) {
        char *suffix;

        INS_NAME(s) = util_strdup(INS_NAME(ins));
        suffix = util_str_with_index("_", LCOUNT(ent->net->items) + 1);
        util_str_append(&INS_NAME(s), suffix);
        free(suffix);
    }
    INS_D(s) = util_strdup(nv->name);
    if (outs & 2) {
        nv = lib_new_variable(ent, 0, 1);
        INS_Q(s) = util_strdup(nv->name);
    }
    if (outs & 1) {
        nv = lib_new_variable(ent, 0, 1);
        INS_QN(s) = util_strdup(nv->name);
    }
    if (INS_CE(ins) != NULL)
        INS_CE(s) = util_strdup(INS_CE(ins));
    if (INS_OE(ins) != NULL)
        INS_OE(s) = util_strdup(INS_OE(ins));
    if (INS_CLK(ins) == NULL) {
        INS_CLK(s) = NULL;
    } else {
        INS_CLK(s) = util_strdup(INS_CLK(ins));
        INS_CLK_KIND(s) = INS_CLK_KIND(ins);
    }
    if (constant == 0) {
        INS_AP(s) = dup_str(INS_AR(ins));
        INS_AP_POL(s) = INS_AR_POL(ins);
    } else {
        INS_AR(s) = dup_str(INS_AP(ins));
        INS_AR_POL(s) = INS_AP_POL(ins);
    }
    list_add_unique(g_rebuilt, INS_NAME(ins), lib_str_ieq);
    return s;
}

/* the gate lacks the reset (or set) the cell has.  A second
 * cell, set by that signal, is added and the outputs are combined:
 * Q = Q' * QN(sbox) for a reset (+ for a set), QN = QN' + Q(sbox). */
static void techmap_make_sbox(FitEntity *ent, FitNetItem *it, FitLibSeq *g)
{
    FitInstance *ins = it->cell;
    FitInstance *sbox;
    FitVariable *new_q = NULL, *new_qn = NULL, *old_q = NULL, *old_qn = NULL;
    FitNetItem *item;
    char *sig, *old;
    unsigned outs = 0;
    int is_reset;

    sig = INS_AP(ins);
    if (sig == NULL || g->ap != NULL) {
        sig = INS_AR(ins);
        is_reset = 1;
    } else {
        is_reset = 0;
    }
    techmap_message("Info -  Creating instance %s_SBOX to replace %s pin\n", INS_NAME(ins), sig, 2);
    if (INS_Q(ins) == NULL && INS_QN(ins) == NULL) {
        FitVariable *nv = lib_new_variable(ent, 0, 1);

        INS_Q(ins) = util_strdup(nv->name);
    }
    if (INS_Q(ins) != NULL) {
        new_q = lib_new_variable(ent, 0, 1);
        old = INS_Q(ins);
        outs = 1;
        INS_Q(ins) = util_strdup(new_q->name);
        old_q = lib_input_to_output(ent, old);
        free(old);
    }
    if (INS_QN(ins) != NULL) {
        new_qn = lib_new_variable(ent, 0, 1);
        old = INS_QN(ins);
        outs |= 2;
        INS_QN(ins) = util_strdup(new_qn->name);
        old_qn = lib_input_to_output(ent, old);
        free(old);
    }
    item = netitem_raw((it->kind == FIT_GATE_LATCH || it->kind == FIT_GATE_LMUX)
                       ? FIT_GATE_LATCH : FIT_GATE_DFF, NULL);
    sbox = techmap_new_sbox_instance(ent, ins, !is_reset, outs);
    item->cell = sbox;
    list_add(ent->net->items, item);
    if (is_reset) {
        if (INS_AR(ins) != NULL)
            free(INS_AR(ins));
        INS_AR(ins) = NULL;
    } else {
        if (INS_AP(ins) != NULL)
            free(INS_AP(ins));
        INS_AP(ins) = NULL;
    }
    if (outs & 1)
        techmap_add_binary_fct(ent, is_reset ? FIT_SEL_AND : FIT_SEL_OR,
                               need_var(old_q)->name, new_q->name, INS_QN(sbox));
    if (outs & 2)
        techmap_add_binary_fct(ent, is_reset ? FIT_SEL_OR : FIT_SEL_AND,
                               need_var(old_qn)->name, new_qn->name, INS_Q(sbox));
}

/* the gate has no tri-state output: the Z output of the cell
 * moves to a separate tri-state cell (inverting if it has to hang on QN). */
static void techmap_make_thbox(FitEntity *ent, FitNetItem *it, int gate_has_q)
{
    FitInstance *ins = it->cell;
    FitInstance *th;
    FitNetItem *item;
    FitVariable *src;
    char *name;

    techmap_message("Info -  Target library has no tri-state outputs on flip flops.\n"
                    "\tCreating Tri-state instance for output %s on instance %s\n",
                    INS_Z(ins), INS_NAME(ins), 2);
    th = util_calloc(1, sizeof *th);
    item = netitem_raw(gate_has_q ? FIT_GATE_TH : FIT_GATE_ITH, th);
    if (INS_NAME(ins) != NULL) {
        INS_NAME(th) = util_strdup(INS_NAME(ins));
        util_str_append(&INS_NAME(th), "_THBOX");
    }
    INS_OE_POL(th) = INS_OE_POL(ins);
    INS_OE(th) = dup_str(INS_OE(ins));
    free(INS_OE(ins));
    INS_OE(ins) = NULL;
    INS_Q(th) = dup_str(INS_Z(ins));
    name = INS_Q(ins);
    if ((name == NULL || !gate_has_q) && (name = INS_QN(ins), name == NULL || gate_has_q)) {
        src = lib_new_variable(ent, 0, 1);
        if (!gate_has_q)
            INS_QN(ins) = util_strdup(src->name);
        else
            INS_Q(ins) = util_strdup(src->name);
    } else {
        src = need_var(entity_find_var(ent, name));
    }
    free(INS_Z(ins));
    INS_Z(ins) = NULL;
    INS_D(th) = util_strdup(src->name);
    list_add(ent->net->items, item);
}

/* changes the cell so that it fits the gate chosen by
 * techmap_best_gate.  Returns nonzero if something was changed (the
 * caller then matches again), 0 if nothing was or the cell needs a
 * tri-state output the library cannot give. */
static int techmap_fit_to_gate(FitEntity *ent, FitNetItem *it, FitLibSeq *g)
{
    LIB_TRACE_F("@fit %s %s\n", LIB_TRACE_S(INS_NAME((FitInstance *)it->cell)), LIB_TRACE_S(g->name));
    FitInstance *ins = it->cell;
    unsigned char changes;

    changes = !lib_inputs_match(ins, g);
    if (changes)
        techmap_connect_open_inputs(ent, it);
    if (INS_CLK(ins) != NULL && g->clk != NULL) {
        techmap_set_polarity(ent, &INS_CLK(ins), &INS_CLK_KIND(ins), g->clk_kind, 1);
        changes++;
    }
    if (INS_OE(ins) != NULL && g->oe == NULL) {
        if (g_ts_flags != 0) {
            if (INS_Z(ins) == NULL) {
                techmap_message("Warning -  No high impedance output on instance %s. "
                                "Output enable pin %s is removed\n",
                                INS_NAME(ins), INS_OE(ins), 1);
                free(INS_OE(ins));
                INS_OE(ins) = NULL;
            } else {
                techmap_make_thbox(ent, it, g->q != NULL);
                changes++;
            }
        } else {
            if (g_no_ts_warned++ != 0)
                techmap_message("Warning -  No tri-state gates in target library. \n", NULL, NULL, 1);
            return 0;
        }
    }
    if (INS_CE(ins) != NULL && g->ce == NULL) {
        if (it->kind == FIT_GATE_JK || it->kind == FIT_GATE_RS)
            techmap_ce_to_jk_gating(ent, ins);
        else
            techmap_ce_to_mux(ent, it, g->q != NULL);
        changes++;
    }
    if (((INS_AR(ins) != NULL && g->ar == NULL) || (INS_AP(ins) != NULL && g->ap == NULL))
        && (INS_Q(ins) != NULL || INS_QN(ins) != NULL)) {
        if ((g->ap == NULL && g->ar == NULL) || (INS_AR(ins) != NULL && INS_AP(ins) != NULL))
            techmap_make_sbox(ent, it, g);
        else
            techmap_swap_q_qn(ent, it);
        changes++;
    }
    return changes != 0;
}

/* gives the control pins of the cell the polarities of the gate */
static void techmap_set_control_polarities(FitEntity *ent, FitInstance *ins, FitLibSeq *g)
{
    if (g->ar != NULL)
        techmap_set_polarity(ent, &INS_AR(ins), &INS_AR_POL(ins), g->ar_pol, 1);
    if (g->ap != NULL)
        techmap_set_polarity(ent, &INS_AP(ins), &INS_AP_POL(ins), g->ap_pol, 1);
    if (g->ce != NULL)
        techmap_set_polarity(ent, &INS_CE(ins), &INS_CE_POL(ins), g->ce_pol, 0);
    if (g->oe != NULL)
        techmap_set_polarity(ent, &INS_OE(ins), &INS_OE_POL(ins), g->oe_pol, 1);
    if (g->j != NULL)
        techmap_set_polarity(ent, &INS_J(ins), &INS_J_POL(ins), g->j_pol, 1);
    if (g->k != NULL)
        techmap_set_polarity(ent, &INS_K(ins), &INS_K_POL(ins), g->k_pol, 1);
}

/* the gate has all pins of the cell but other polarities, or
 * only one of Q / QN: control signals are inverted, and an output the gate
 * lacks is derived from the one it has. */
static void techmap_change_controls(FitEntity *ent, FitNetItem *it, FitLibSeq *g)
{
    LIB_TRACE_F("@change %s %s\n", LIB_TRACE_S(INS_NAME((FitInstance *)it->cell)), LIB_TRACE_S(g->name));
    FitInstance *ins = it->cell;
    FitVariable *v, *other;
    char *q = INS_Q(ins);
    char *qn, *moved, *other_name;

    techmap_message("Info -  Changing control inputs of instance %s\n", INS_NAME(ins), g->name, 2);
    techmap_set_control_polarities(ent, ins, g);
    q = INS_Q(ins);
    if (q == NULL || g->q != NULL) {
        qn = INS_QN(ins);
        if (qn == NULL)
            return;
        if (g->qn != NULL)
            return;
        if (q == NULL) {
            INS_QN(ins) = NULL;
            INS_Q(ins) = dup_str(techmap_input_to_inverted(ent, qn));
            return;                 /* the old QN string is not freed */
        }
        moved = util_strdup(qn);
        INS_QN(ins) = NULL;
        v = lib_input_to_output(ent, moved);
        other_name = INS_Q(ins);
    } else {
        if (INS_QN(ins) == NULL) {
            INS_Q(ins) = NULL;
            INS_QN(ins) = dup_str(techmap_input_to_inverted(ent, q));
            free(q);
            return;
        }
        moved = util_strdup(q);
        INS_Q(ins) = NULL;
        v = lib_input_to_output(ent, moved);
        other_name = INS_QN(ins);
    }
    other = var_list_find_name(ent->inputs, other_name);
    techmap_add_wire_fct(ent, v, other);
    tt2_invert_fct(ent, v->id);
    free(moved);
}

/* can the gate (which has all the pins) serve, and how many
 * polarity changes does it need */
static int techmap_polarity_cost(FitInstance *ins, FitLibSeq *g, int *cost)
{
    *cost = 0;
    if (g_lib_strict_async != 0 && g->ap != NULL && g->ar != NULL
        && (INS_AP(ins) == NULL || INS_AR(ins) == NULL))
        return 0;
    if (INS_Z(ins) != NULL && g->z == NULL)
        return 0;
    *cost += !lib_same_presence(INS_Q(ins), g->q);
    *cost += !lib_same_presence(INS_QN(ins), g->qn);
    *cost += techmap_ctrl_cost(INS_AP(ins), INS_AP_POL(ins), g->ap, g->ap_pol, 0);
    *cost += techmap_ctrl_cost(INS_AR(ins), INS_AR_POL(ins), g->ar, g->ar_pol, 0);
    *cost += techmap_ctrl_cost(INS_OE(ins), INS_OE_POL(ins), g->oe, g->oe_pol, 0);
    *cost += techmap_ctrl_cost(INS_CE(ins), INS_CE_POL(ins), g->ce, g->ce_pol, 0);
    *cost += techmap_ctrl_cost(INS_J(ins), INS_J_POL(ins), g->j, g->j_pol, 0);
    *cost += techmap_ctrl_cost(INS_K(ins), INS_K_POL(ins), g->k, g->k_pol, 0);
    return 1;
}

/* the cheapest gate with the same inputs that has every control
 * pin the cell uses (first of equal cost), NULL if none */
static FitLibSeq *techmap_best_gate_same_pins(FitNetItem *it, FitList *gl)
{
    FitInstance *ins = it->cell;
    FitLibSeq *best = NULL;
    int best_cost = 10000;
    int cost = 0;

    for (int i = 0; i < LCOUNT(gl); i++) {
        FitLibSeq *g = ((FitLibGate *)gl->items[i])->cell;

        if (lib_inputs_match(ins, g) && lib_controls_available(ins, g)
            && techmap_polarity_cost(ins, g, &cost) && cost < best_cost) {
            best_cost = cost;
            best = g;
        }
    }
    return best;
}

/* 1 if some gate has exactly the pins and polarities of the cell */
static int techmap_has_exact_gate(FitNetItem *it, FitList *gl)
{
    FitInstance *ins = it->cell;

    for (int i = 0; i < LCOUNT(gl); i++) {
        FitLibSeq *g = ((FitLibGate *)gl->items[i])->cell;

        if (lib_inputs_match(ins, g) && lib_controls_same(ins, g) && lib_outputs_available(ins, g))
            return 1;
    }
    return 0;
}

/* is there a D flip-flop with an asynchronous input (if needed)
 * and an output enable (if the cell has a tri-state output) */
static int techmap_dff_available(FitList *gl, int need_async, char *z)
{
    for (int i = 0; i < LCOUNT(gl); i++) {
        FitLibSeq *g = ((FitLibGate *)gl->items[i])->cell;

        if ((need_async == 0 || g->ar != NULL || g->ap != NULL)
            && (z == NULL || g->oe != NULL || g_ts_flags != 0))
            return 1;
    }
    return 0;
}

/* T flip-flop -> D flip-flop fed by its own QN, enabled by T */
static void techmap_tff_to_dff(FitEntity *ent, FitNetItem *it)
{
    FitInstance *ins = it->cell;

    it->kind = FIT_GATE_DFF;
    if (INS_D(ins) != NULL) {
        INS_CE(ins) = INS_D(ins);
        INS_CE_POL(ins) = 1;
    }
    if (INS_QN(ins) == NULL) {
        FitVariable *nv = lib_new_variable(ent, 0, 1);

        INS_QN(ins) = util_strdup(nv->name);
    }
    INS_D(ins) = util_strdup(INS_QN(ins));
}

/* JK flip-flop -> mux flip-flop selecting J or !K by its output */
static void techmap_jk_to_dmux(FitEntity *ent, FitNetItem *it)
{
    FitInstance *ins = it->cell;
    char *k = INS_K(ins);
    char *j = INS_J(ins);

    INS_J(ins) = NULL;
    INS_K(ins) = NULL;
    if (INS_Q(ins) == NULL && INS_QN(ins) == NULL) {
        FitVariable *nv = lib_new_variable(ent, 0, 1);

        INS_Q(ins) = util_strdup(nv->name);
    }
    it->kind = FIT_GATE_DMUX;
    if (INS_Q(ins) != NULL) {
        INS_MUX_A(ins) = j;
        INS_MUX_B(ins) = k;
        INS_MUX_SEL(ins) = util_strdup(INS_Q(ins));
        techmap_set_polarity(ent, &INS_MUX_A(ins), &INS_J_POL(ins), 1, 0);
        techmap_set_polarity(ent, &INS_MUX_B(ins), &INS_K_POL(ins), -1, 0);
        return;
    }
    INS_MUX_A(ins) = k;
    INS_MUX_B(ins) = j;
    INS_MUX_SEL(ins) = dup_str(INS_QN(ins));
    techmap_set_polarity(ent, &INS_MUX_A(ins), &INS_K_POL(ins), -1, 0);
    techmap_set_polarity(ent, &INS_MUX_B(ins), &INS_J_POL(ins), 1, 0);
}

/* RS flip-flop / latch -> D cell with  D = (R + Q) * S  on the
 * signals after S has been made active low and R active high */
static void techmap_rs_to_dff(FitEntity *ent, FitNetItem *it)
{
    FitInstance *ins = it->cell;
    FitVariable *nv;
    FitFctTree *t1, *t2;
    FitFct *f;
    char *s, *r;
    int id_s, id_r, id_q;

    techmap_set_polarity(ent, &INS_J(ins), &INS_J_POL(ins), -1, 0);
    techmap_set_polarity(ent, &INS_K(ins), &INS_K_POL(ins), 1, 0);
    s = techmap_release_signal(ent, &INS_J(ins));
    r = techmap_release_signal(ent, &INS_K(ins));
    if (INS_Q(ins) == NULL) {
        nv = lib_new_variable(ent, 0, 1);
        INS_Q(ins) = util_strdup(nv->name);
    }
    it->kind = FIT_GATE_DFF;
    id_s = entity_var_id(ent, s);
    id_r = entity_var_id(ent, r);
    id_q = entity_var_id(ent, INS_Q(ins));
    nv = lib_new_variable(ent, 1, 1);
    INS_D(ins) = util_strdup(nv->name);
    t1 = fcttree_new_binary_ids(FIT_SEL_OR, id_r, id_q);
    t2 = fcttree_new_id(id_s);
    t1 = fcttree_new_binary(FIT_SEL_AND, t1, t2);
    f = fct_new();
    f->var_id = entity_var_id(ent, nv->name);
    f->tree[0] = t1;
    list_add(ent->fcts, f);
}

/* mux flip-flop / latch -> plain one with the multiplexer as a
 * function in front of D */
static void techmap_mux_ff_to_ff(FitEntity *ent, FitNetItem *it)
{
    FitInstance *ins = it->cell;
    FitVariable *nv;
    char *a, *b, *sel;

    if (it->kind == FIT_GATE_DMUX)
        it->kind = FIT_GATE_DFF;
    else if (it->kind == FIT_GATE_LMUX)
        it->kind = FIT_GATE_LATCH;
    a = techmap_release_signal(ent, &INS_MUX_A(ins));
    b = techmap_release_signal(ent, &INS_MUX_B(ins));
    sel = techmap_release_signal(ent, &INS_MUX_SEL(ins));
    nv = lib_new_variable(ent, 1, 1);
    INS_D(ins) = util_strdup(nv->name);
    list_add(ent->fcts, techmap_new_mux_fct(ent, a, b, sel, nv->name));
}

/* latch -> D flip-flop on the same enable with a multiplexer
 * behind it that passes the data while the latch would be transparent */
static void techmap_latch_to_dff(FitEntity *ent, FitNetItem *it)
{
    FitInstance *ins = it->cell;
    FitVariable *nv, *out;
    FitFct *f;
    char *old_q;

    it->kind = FIT_GATE_DFF;
    if (INS_Q(ins) == NULL) {
        nv = lib_new_variable(ent, 0, 1);
        INS_Q(ins) = util_strdup(nv->name);
    }
    old_q = INS_Q(ins);                       /* not freed */
    nv = lib_new_variable(ent, 0, 1);
    INS_Q(ins) = util_strdup(nv->name);
    out = need_var(lib_input_to_output(ent, old_q));
    if (INS_CLK_KIND(ins) == FIT_CLK_L0) {
        f = techmap_new_mux_fct(ent, INS_D(ins), INS_Q(ins), INS_CLK(ins), out->name);
        INS_CLK_KIND(ins) = FIT_CLK_RECK;
    } else {
        f = techmap_new_mux_fct(ent, INS_Q(ins), INS_D(ins), INS_CLK(ins), out->name);
        INS_CLK_KIND(ins) = FIT_CLK_FECK;
    }
    list_add(ent->fcts, f);
}

/* latch -> combinational loop (only with g_lib_latch_mode
 * "comb", which nothing sets):
 *   Q = fb * !(en * !D) [* !AR] + en * D [* !AR] [+ AP]
 * and the item becomes a FIT_GATE_WIRE from Q to the new input fb.
 * `en` and the reset term appear in two places of the tree; the second
 * use is a copy, so that the tree owns its nodes. */
static int techmap_latch_to_comb(FitEntity *ent, FitNetItem *it)
{
    FitInstance *ins = it->cell;
    FitVariable *nv, *fb;
    FitFctTree *en, *hold, *pass, *t;
    FitFct *f;
    char *q;
    int id_q, id_d, id_c;

    if (INS_OE(ins) != NULL) {
        if (g_ts_flags != 0) {
            if (INS_Z(ins) == NULL) {
                techmap_message("Warning -  No high impedance output on instance %s. "
                                "Output enable pin %s is removed\n",
                                INS_NAME(ins), INS_OE(ins), 1);
                free(INS_OE(ins));
                INS_OE(ins) = NULL;
            } else {
                techmap_make_thbox(ent, it, 1);
            }
        } else {
            techmap_message("Warning -  No tri-state gates in target library. \n", NULL, NULL, 1);
            return 0;
        }
    }
    if (INS_Q(ins) == NULL) {
        if (INS_QN(ins) == NULL) {
            nv = lib_new_variable(ent, 0, 1);
            INS_QN(ins) = util_strdup(nv->name);
        }
        q = techmap_input_to_inverted(ent, INS_QN(ins));
        techmap_release_signal(ent, &INS_QN(ins));
        if (q == NULL)
            lib_internal_error("null string");
    } else {
        q = INS_Q(ins);
    }
    id_q = entity_var_id(ent, q);
    id_d = entity_var_id(ent, INS_D(ins));
    id_c = entity_var_id(ent, INS_CLK(ins));
    techmap_release_signal(ent, &INS_D(ins));
    techmap_release_signal(ent, &INS_CLK(ins));
    INS_D(ins) = util_strdup(q);
    fb = lib_new_variable(ent, 0, 1);
    INS_Q(ins) = util_strdup(fb->name);       /* the old Q string is not freed */
    if (INS_CLK_KIND(ins) == FIT_CLK_L1)
        en = fcttree_new_id(id_c);
    else
        en = fcttree_new_not(fcttree_new_id(id_c));
    if (INS_CE(ins) != NULL) {
        FitFctTree *ce = fcttree_new_id(entity_var_id(ent, INS_CE(ins)));

        if (INS_CE_POL(ins) != 1)
            ce = fcttree_new_not(ce);
        en = fcttree_new_binary(FIT_SEL_AND, en, ce);
        techmap_release_signal(ent, &INS_CE(ins));
    }
    hold = fcttree_new_binary(FIT_SEL_AND, en, fcttree_new_not(fcttree_new_id(id_d)));
    pass = fcttree_new_binary(FIT_SEL_AND, fcttree_copy(en, NULL), fcttree_new_id(id_d));
    t = fcttree_new_binary(FIT_SEL_AND, fcttree_new_id(fb->id), fcttree_new_not(hold));
    if (INS_AR(ins) != NULL) {
        FitFctTree *r = fcttree_new_id(entity_var_id(ent, INS_AR(ins)));

        if (INS_AR_POL(ins) == 1)
            r = fcttree_new_not(r);
        techmap_release_signal(ent, &INS_AR(ins));
        pass = fcttree_new_binary(FIT_SEL_AND, pass, r);
        t = fcttree_new_binary(FIT_SEL_AND, t, fcttree_copy(r, NULL));
    }
    t = fcttree_new_binary(FIT_SEL_OR, t, pass);
    if (INS_AP(ins) != NULL) {
        FitFctTree *p = fcttree_new_id(entity_var_id(ent, INS_AP(ins)));

        if (INS_AP_POL(ins) == -1)
            p = fcttree_new_not(p);
        techmap_release_signal(ent, &INS_AP(ins));
        t = fcttree_new_binary(FIT_SEL_OR, t, p);
    }
    f = fct_new();
    f->var_id = id_q;
    f->tree[0] = t;
    it->kind = FIT_GATE_WIRE;
    list_add(ent->fcts, f);
    return 1;
}

/* the D flip-flops of the library (kind 3); a library without any is an
 * internal error */
static FitList *techmap_dff_gates(FitList *groups)
{
    FitLibKindGroup *g = lib_kind_group_find(groups, FIT_GATE_DFF);

    if (g == NULL)
        lib_internal_error("library without D flip-flops");
    return g->gates;
}

/* shared by the cases of techmap_map_instance: the D input of a tri-state
 * cell is inverted (kinds 8 <-> 9) */
static void techmap_invert_th_input(FitEntity *ent, FitInstance *ins)
{
    int id = var_list_id_of_name(ent->outputs, INS_D(ins));

    if (id == 0) {
        char *old = techmap_release_signal(ent, &INS_D(ins));
        FitVariable *nv = lib_new_variable(ent, 1, 1);

        techmap_add_wire_fct(ent, nv, entity_find_var(ent, old));
        tt2_invert_fct(ent, nv->id);
        INS_D(ins) = util_strdup(nv->name);
    } else {
        tt2_invert_fct(ent, id);
    }
}

/* finds or makes a library gate for one sequential or tri-state
 * cell.  Returns 1 if the cell is mapped, 0 if not (the caller then puts
 * it on the list of unmapped items). */
static int techmap_map_instance(FitEntity *ent, FitNetItem *it, FitList *groups)
{
    LIB_TRACE_F("@map %d %s\n", it->kind, LIB_TRACE_S(INS_NAME((FitInstance *)it->cell)));
    for (;;) {
        FitLibKindGroup *grp = lib_kind_group_find(groups, it->kind);
        FitInstance *ins;
        int need_async;

        if (grp != NULL) {
            FitList *gl = grp->gates;
            FitLibSeq *g;

            if (techmap_has_exact_gate(it, gl))
                return 1;
            g = techmap_best_gate_same_pins(it, gl);
            if (g != NULL) {
                techmap_change_controls(ent, it, g);
                return 1;
            }
            g = techmap_best_gate(it, gl);
            if (g != NULL && techmap_fit_to_gate(ent, it, g))
                continue;
        }
        ins = it->cell;
        need_async = (INS_AR(ins) != NULL || INS_AP(ins) != NULL);
        switch (it->kind) {
        case FIT_GATE_TFF:
            if (!techmap_dff_available(techmap_dff_gates(groups), need_async, INS_Z(ins)))
                return 0;
            techmap_message("Info -  Target library has no mux flip flops.\n"
                            "\tMoving Mux gate of instance name %s to Latch or D Gate\n",
                            INS_NAME(ins), NULL, 2);
            techmap_tff_to_dff(ent, it);
            techmap_map_instance(ent, it, groups);
            return 1;
        case FIT_GATE_LATCH:
            if (g_lib_latch_mode != NULL) {
                if (util_str_ieq(g_lib_latch_mode, "comb"))
                    return techmap_latch_to_comb(ent, it);
                if (util_str_ieq(g_lib_latch_mode, "no"))
                    goto no_cell;
            }
            if (!techmap_dff_available(techmap_dff_gates(groups), need_async, INS_Z(ins)))
                return 0;
            techmap_latch_to_dff(ent, it);
            techmap_map_instance(ent, it, groups);
            return 1;
        case FIT_GATE_JK:
            if (!techmap_dff_available(techmap_dff_gates(groups), need_async, INS_Z(ins)))
                return 0;
            techmap_message("Info -  Target library has no jk flip flops.\n"
                            "\tMoving jk gate of instance name %s to mux flip flop\n",
                            INS_NAME(ins), NULL, 2);
            techmap_jk_to_dmux(ent, it);
            techmap_map_instance(ent, it, groups);
            return 1;
        case FIT_GATE_DMUX:
        case FIT_GATE_LMUX:
            if (!techmap_dff_available(techmap_dff_gates(groups), need_async, INS_Z(ins)))
                return 0;
            techmap_message("Info -  Target library has no mux flip flops.\n"
                            "\tMoving Mux gate of instance name %s to Latch or D Gate\n",
                            INS_NAME(ins), NULL, 2);
            techmap_mux_ff_to_ff(ent, it);
            techmap_map_instance(ent, it, groups);
            return 1;
        case FIT_GATE_TH:
            if ((g_ts_flags & 1) == 0)
                goto no_cell;
            techmap_invert_th_input(ent, ins);
            it->kind = FIT_GATE_ITH;
            continue;
        case FIT_GATE_ITH:
            if ((g_ts_flags & 2) == 0)
                goto no_cell;
            techmap_invert_th_input(ent, ins);
            it->kind = FIT_GATE_TH;
            continue;
        case FIT_GATE_RS:
            if (!techmap_dff_available(techmap_dff_gates(groups), need_async, INS_Z(ins)))
                return 0;
            techmap_rs_to_dff(ent, it);
            techmap_map_instance(ent, it, groups);
            return 1;
        case FIT_GATE_RSL:
            if (!techmap_dff_available(techmap_dff_gates(groups), need_async, INS_Z(ins)))
                return 0;
            techmap_rs_to_dff(ent, it);
            techmap_map_instance(ent, it, groups);
            it->kind = FIT_GATE_LATCH;
            return 1;
        default:
        no_cell:
            techmap_message("Warning -  Unable to find a cell in destination library for instance %s\n",
                            INS_NAME(ins), NULL, 1);
            return 0;
        }
    }
}

/* bit 0 if the library has a BI_BUF, bit 1 if it has a TS_BUF.
 * (The caller throws the result away, so g_ts_flags stays 0.) */
static unsigned techmap_buffer_flags(FitList *groups)
{
    FitLibKindGroup *g = lib_kind_group_find(groups, FIT_GATE_BUF);
    unsigned flags = 0;

    if (g == NULL)
        return 0;
    for (int i = 0; i < LCOUNT(g->gates); i++) {
        int type = ((FitLibBuf *)((FitLibGate *)g->gates->items[i])->cell)->type;

        if (type == FIT_BUF_BI)
            flags |= 1;
        if (type == FIT_BUF_TS)
            flags |= 2;
    }
    return flags;
}

/* an alias keeps its target only if the target is an output
 * whose function is just the aliased variable itself. */
static void techmap_drop_stale_alias_targets(FitEntity *ent)
{
    FitList *aliases;

    if (ent->net == NULL)
        return;
    aliases = ent->net->aliases;
    for (int i = 0; i < LCOUNT(aliases); i++) {
        FitAlias *a = aliases->items[i];

        if (a->target == NULL)
            continue;
        if (var_list_id_of_name(ent->inputs, a->target) == 0
            && var_list_id_of_name(ent->locals, a->target) == 0) {
            int id = var_list_id_of_name(ent->outputs, a->target);

            if (id != 0) {
                FitFct *f = entity_find_fct(ent, id);

                if (f == NULL || f->tree[0] == NULL)
                    lib_internal_error("output without function");
                if (f->tree[0]->sel == FIT_SEL_ID) {
                    int cls = 0;
                    char *name = lib_var_name_by_id(f->tree[0]->u.id, ent, &cls);

                    if (util_str_ieq(name, a->name))
                        continue;
                }
            }
        }
        free(a->target);
        a->target = NULL;
    }
}

/* separates fed-back outputs, then deletes local cycles until
 * the cycle check passes.  A cycle that is not made of locals only cannot
 * be deleted; that is an internal error. */
static void techmap_finish(FitEntity *ent)
{
    LIB_TRACE_F("@finish\n");
    if (!lib_split_fed_back_outputs(ent))
        return;
    for (;;) {
        char *cycle = NULL;
        FitList *ids;

        if (tt2_check_cycles(ent, &cycle))
            break;
        ids = tt2_cycle_to_ids(ent, cycle);
        if (ids == NULL)
            lib_internal_error("combinational cycle that is not local");
        free(cycle);
        tt2_delete_cycle(ent, ids);
    }
}

/* the mapping of one entity onto a library.  Items the library
 * cannot take (unknown gates, bidirectional and tri-state buffers without
 * a match, cells without a gate) are appended to `unmapped`.  Always
 * returns 1. */
int techmap_adapt_netlist(FitEntity *ent, FitLibHandle *lib, FitList *unmapped)
{
    FitList *groups;
    FitList *items;
    int i;

    if (ent == NULL || util_str_icmp(ent->lib_file, lib->file) == 0
        || ent->net == NULL || ent->net->items == NULL)
        return 1;
    if (ent->locals == NULL)
        ent->locals = list_create_sized(1);
    if (ent->outputs == NULL)
        ent->outputs = list_create_sized(1);
    if (ent->inputs == NULL)
        ent->inputs = list_create_sized(1);
    g_rebuilt = list_create();
    g_inv_cache = list_create();
    g_tie_for_1 = NULL;
    g_tie_for_0 = NULL;
    groups = lib_group_by_kind(lib->gates);
    items = ent->net->items;
    techmap_remove_inactive_tristates(ent);
    (void)techmap_buffer_flags(groups);
    i = 0;
    while (i < LCOUNT(items)) {
        FitNetItem *it = items->items[i];

        if (it->kind == FIT_GATE_UNKNOWN) {
            list_add(unmapped, it);
        } else if (it->kind == FIT_GATE_WIRE) {
            /* nothing */
        } else if (it->kind == FIT_GATE_BUF) {
            FitCell13 *c = it->cell;
            int type = BUF_TYPE(c);
            int m = techmap_match_buffer(c, groups);

            if (m == 0) {
                if (type == FIT_BUF_TS) {
                    techmap_message("Warning -  No tri-state buffer for instance %s\n",
                                    BUF_NAME(c), NULL, 1);
                    list_add(unmapped, it);
                } else if (type == FIT_BUF_BI) {
                    techmap_message("Warning -  No bidirectional buffer for instance %s\n",
                                    BUF_NAME(c), NULL, 1);
                    list_add(unmapped, it);
                } else {
                    techmap_buffer_to_wire(ent, it, i);
                    continue;           /* the next item has moved to index i */
                }
            } else if (m == -1 && BUF_OE(c) != NULL) {
                techmap_set_polarity(ent, &BUF_OE(c), &BUF_OE_POL(c), -BUF_OE_POL(c), 1);
            }
        } else {
            techmap_prepare_instance(ent, it);
            if (!techmap_map_instance(ent, it, groups))
                list_add(unmapped, it);
        }
        i++;
    }
    techmap_drop_stale_alias_targets(ent);
    if (g_rebuilt != NULL && g_rebuilt->count != 0) {
        char *names;

        if (g_rebuilt->count == 1) {
            names = dup_str(g_rebuilt->items[0]);
            fprintf(g_msg, "The following gate has been rebuilt from primitives: %s\n", names);
        } else {
            names = util_join_wrapped(g_rebuilt, ",", "", 0x50);
            fprintf(g_msg, "The following gates have been rebuilt from primitives:\n%s\n", names);
        }
        free(names);
    }
    list_free_all(&groups, lib_kind_group_free);
    list_free_all(&g_inv_cache, techmap_inv_pair_free);
    list_destroy(&g_rebuilt);
    techmap_finish(ent);
    return 1;
}
