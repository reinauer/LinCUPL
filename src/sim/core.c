/*
 * sim_core - the simulation proper: evaluation of the product terms
 * of the compiled design against the input vector, the phases of one test
 * vector (set inputs, settle, clock, settle) and the comparison with the
 * expected values.
 *
 * Part of the CUPL simulator (see sim.c).
 */
#include "sim.h"

#define MAX_SETTLE_PASSES 20

/* value token for a logic level (index LV_*) */
static const int16_t k_level_token[9] = { -1, -1, T_VAL_L, T_VAL_H, -1, -1, T_VAL_X, T_VAL_Z, 0 };
/* the opposite logic level */
const uint16_t k_invert_level[8] = { 0, 0, LV_HIGH, LV_LOW, 0, 0, LV_UNKNOWN, LV_HIGHZ };

static int g_preload;               /* the vector is a register preload */
static int g_preload_pin;           /* preload enable pin is active */
static uint32_t g_rand_seed;        /* state of the random generator (test value R) */

/* -------------------------------------------------------------------------
 * Input vector
 * ------------------------------------------------------------------------- */

/* Store LV_LOW, LV_HIGH or LV_UNKNOWN for symbol `index` in the three
 * planes of the input vector. */
void set_input_value(int value, int index)
{
    switch (value) {
    case LV_LOW:
        bit_clear(g_in_unknown, index);
        bit_set(g_in_valid, index);
        bit_clear(g_in_high, index);
        break;
    case LV_HIGH:
        bit_clear(g_in_unknown, index);
        bit_set(g_in_valid, index);
        bit_set(g_in_high, index);
        break;
    default:
        if (value != LV_UNKNOWN) {
            fatal_error(0x404, "");
        }
        bit_set(g_in_unknown, index);
        bit_set(g_in_valid, index);
        bit_clear(g_in_high, index);
        break;
    }
}

void set_input_from_reg(int src, int index)
{
    set_input_value((int8_t)g_reg_q[src], index);
}

static void set_input_unknown(int index)
{
    set_input_value(LV_UNKNOWN, index);
}

/* Level of symbol `index` in the input vector; `invert` swaps LV_LOW and
 * LV_HIGH. */
int get_input_value(int index, int invert)
{
    if (bit_test(g_in_unknown, index)) {
        return LV_UNKNOWN;
    }
    if (!bit_test(g_in_valid, index)) {
        return LV_UNKNOWN;
    }
    if (bit_test(g_in_high, index)) {
        return invert ? LV_LOW : LV_HIGH;
    }
    return invert ? LV_HIGH : LV_LOW;
}

/* Set the value of a symbol that is a pin of a kind that can be driven
 * from outside. */
static void set_pin_value(int sym_index, int value)
{
    const sym_t *s = &g_syms[sym_index];
    if (s->pin_rec < 0) {
        return;
    }
    if (g_vsima && g_dev_has_class) {
        g_sig_value[sym_index] = (uint8_t)value;
        return;
    }
    switch (dev_pin(s->pin_rec)[PIN_KIND]) {
    case 5: case 9: case 10: case 12: case 13: case 14: case 15: case 16:
    case 18: case 22: case 23: case 24:
        g_sig_value[sym_index] = (uint8_t)value;
        break;
    default:
        break;
    }
}

/* -------------------------------------------------------------------------
 * Allocation and reset
 * ------------------------------------------------------------------------- */

/* Initial state of the simulation core's globals. */
void sim_reset(void)
{
    jed150x_reset();
    g_preload = 0;
    g_preload_pin = 0;
    g_rand_seed = 1;
    g_cells = NULL;
    g_stuck = NULL;
}

int vector_is_preload(void)
{
    return g_preload;
}

int cell_kind(int sym_index)
{
    return g_cells[sym_index].kind;
}

/* Power-up state of registers and outputs. */
static void init_levels(void)
{
    jed150x_powerup();
    const uint8_t *hdr = dev_header();
    int level;
    switch (hdr[0x31]) {
    case 1: level = LV_LOW; break;
    case 2: level = LV_HIGH; break;
    default: level = LV_UNKNOWN; break;
    }
    if (g_vsima) {
        uint8_t *prev = g_reg_prev;
        for (int i = 0; i < g_nsyms; i++) {
            if (hdr[0x2f] == 2 && g_cells[i].true_out == 0) {
                g_reg_state[i] = (uint8_t)k_invert_level[level];
            } else {
                g_reg_state[i] = (uint8_t)level;
                *prev++ = (uint8_t)level;   /* deliberately not indexed by the symbol */
            }
        }
        if (design_has_registers()) {
            update_cells(0);
        }
    } else {
        for (int i = 0; i < g_nsyms; i++) {
            if ((int16_t)rd16(hdr + 8) < (int16_t)g_cells[i].number && hdr[0x2f] == 2 && g_cells[i].true_out != 0) {
                g_reg_state[i] = (uint8_t)k_invert_level[level];
            } else {
                g_reg_state[i] = (uint8_t)level;
            }
            g_reg_prev[i] = (uint8_t)level;
        }
        if (hdr[0x1b] != 0) {
            update_cells(0);
        }
    }
    for (int i = 0; i < g_nsyms; i++) {
        int t = g_cells[i].pin_type;
        g_reg_q[i] = (uint8_t)((t == 0x17 || t == 0x18) ? level : LV_UNKNOWN);
    }
    for (int i = 0; i < g_nsyms; i++) {
        int t = g_cells[i].pin_type;
        if (t == 0x33 || t == 0x34) {
            g_reg_state[i] = (uint8_t)level;
        }
    }
    for (int i = 0; i < g_nsyms; i++) {
        switch (g_cells[i].node_type) {
        case 1: case 0x15: case 0x16: case 0x1b: case 8: case 9: case 10: case 11: case 0x1c:
            g_reg_before[i] = (uint8_t)level;
            break;
        default:
            g_reg_before[i] = 0;
            break;
        }
    }
}

/* Clear the per-row and per-symbol values of the previous vector. */
static void clear_vector_state(void)
{
    memset(g_row_value, 0, (size_t)(g_total_rows & 0xffff));
    memset(g_row_stable, 0, (size_t)(g_total_rows & 0xffff));
    memset(g_sig_value, 0, g_nsyms);
    memset(g_sig_hiz, 0, g_nsyms);
    memset(g_sig_out, 0, g_nsyms);
}

/* Back to the power-up state. */
void reset_simulation(void)
{
    memset(g_in_unknown, 0, (size_t)g_row_words * sizeof(uint16_t));
    init_levels();
    clear_vector_state();
    if (g_vsima && (g_options & OPT_JEDEC)) {
        jedec_find_power_pins();
    }
}

/* Allocate the per-row and per-symbol state, bind the symbols to their
 * macrocell models and set the power-up levels. */
void sim_allocate(void)
{
    static const char *const what = "initialization";
    size_t n = g_nsyms;
    size_t rows = (size_t)g_total_rows;
    g_row_value = alloc_tracked(rows, what);
    g_row_stable = alloc_tracked(rows, what);
    g_stuck = alloc_tracked(rows, what);
    g_sig_out = alloc_tracked(n, what);
    g_sig_value = alloc_tracked(n, what);
    g_sig_prev = alloc_tracked(n, what);
    g_reg_state = alloc_tracked(n, what);
    g_reg_prev = alloc_tracked(n, what);
    g_reg_q = alloc_tracked(n, what);
    g_pin_reg_input = alloc_tracked(n, what);
    g_sig_hiz = alloc_tracked(n, what);
    g_sig_unstable = alloc_tracked(n, what);
    g_reg_before = alloc_tracked(n, what);
    g_vec_level = alloc_tracked(n, what);
    g_feedback_sym = alloc_tracked(n * sizeof(uint16_t), what);
    g_sig_is_clock = alloc_tracked(n, what);
    g_sig_saved = alloc_tracked(n, what);
    g_node_level = alloc_tracked(n, what);
    g_bypass_sym = alloc_tracked(n, what);
    memset(g_sig_saved, LV_LOW, n);
    memset(g_vec_level, LV_UNKNOWN, n);
    /* The three planes of the input vector are one block; a few accesses
     * land just behind it, hence the slack. */
    g_in_unknown = alloc_tracked(((size_t)g_row_words + 16) * sizeof(uint16_t), what);
    g_in_valid = g_in_unknown + g_plane_words;
    g_in_high = g_in_valid + g_plane_words;
    g_fb_undefined = alloc_tracked(((size_t)g_plane_words + 8) * sizeof(uint16_t), what);
    g_fb_symbols = alloc_tracked(((size_t)g_plane_words + 8) * sizeof(uint16_t), what);
    g_term_outputs = alloc_tracked(((size_t)g_plane_words + 8) * sizeof(uint16_t), what);
    g_cells = alloc_tracked(n * sizeof(cell_t), what);
    bind_cells();
    pair_feedback_symbols();
    init_levels();
    memset(g_pin_reg_input, LV_UNKNOWN, n);
    if (g_vsima) {
        warn_missing_registers();
    }
}

/* -------------------------------------------------------------------------
 * Macrocell dispatch
 * ------------------------------------------------------------------------- */

/* Compute the output of a symbol that is a node of the device.  Returns
 * non-zero if the output changed and is unstable. */
static int eval_node_cell(int sym, int pass)
{
    const cell_t *c = &g_cells[sym];
    const cell_type_t *type;
    uint8_t before;
    int r;
    if (!c->is_node) {
        return 0;
    }
    before = g_sig_out[sym];
    if (c->node_type >= CELL_TYPE_COUNT) {
        return 0;
    }
    type = &cell_types()[c->node_type < 0 ? 0 : c->node_type];
    if (type->state_size < 1 || type->eval == NULL) {
        g_sig_out[sym] = g_sig_value[sym];
        r = (int8_t)g_sig_unstable[sym];
    } else {
        r = (int8_t)type->eval(sym, pass);
    }
    if (r != 0 && g_sig_out[sym] == before && g_sig_out[sym] != LV_UNKNOWN) {
        r = 0;
    }
    return r;
}

/* A node without a stable value: mark the symbol it feeds as undefined. */
static void mark_undefined_feedback(int sym, int error)
{
    int target = (int16_t)g_feedback_sym[sym];
    if (target >= 0 && bit_test(g_fb_symbols, target)) {
        if (target == 0) {
            report_error(error, "");
        }
        bit_set(g_fb_undefined, target);
    }
}

/* Run the node model of every symbol (eval_node_cell); for a result < 1
 * the fed-back symbol is passed to mark_undefined_feedback. */
void update_cells(int pass)
{
    if (jed150x_active()) { jed150x_step(pass); return; }
    for (int i = 0; i < g_nsyms; i++) {
        if (eval_node_cell(i, pass) < 1) {
            mark_undefined_feedback(i, 0x40a);
        }
    }
}

/* Run the models of the pin cells: counters only, all, or all but the D
 * registers. */
enum { PIN_CELLS_COUNTERS, PIN_CELLS_ALL, PIN_CELLS_NOT_D };
static void eval_pin_cells(int which, int pass)
{
    for (int i = 0; i < g_nsyms; i++) {
        int t = g_cells[i].pin_type;
        const cell_type_t *type;
        if (t <= 0 || t >= CELL_TYPE_COUNT) {
            continue;
        }
        if (which == PIN_CELLS_COUNTERS && t != 0x2e) {
            continue;
        }
        if (which == PIN_CELLS_NOT_D && (t == 0x17 || t == 0x18)) {
            continue;
        }
        type = &cell_types()[t];
        if (type->eval != NULL) {
            type->eval(i, pass);
        }
    }
}

/* -------------------------------------------------------------------------
 * Product terms
 * ------------------------------------------------------------------------- */

/* Evaluate all rows of one output against the input vector.  Symbols 0 and
 * 1 are the constants; word 0 of each plane is a header. */
static void eval_rows(const term_entry_t *t)
{
    const uint16_t *plane0 = t->frame->rows;
    const uint16_t *plane1 = plane0 + g_plane_words;
    const uint16_t *plane2 = plane1 + g_plane_words;
    int row = t->first_row;
    memset(g_term_outputs, 0, g_plane_words * sizeof(uint16_t));
    for (int r = 0; r < t->nrows; r++, row++) {
        int forced = 0;
        int n_true = 0, n_unknown = 0, n_false = 0, n_fb_bad = 0, n_fb_all = 0;
        if (g_stuck[row] != 0) {
            g_row_value[row] = g_stuck[row];
            g_row_stable[row] = 1;
            forced = 1;
        } else if (((plane1[1] & 1) && !(plane2[1] & 1)) || ((plane1[1] & 2) && (plane2[1] & 2))) {
            g_row_value[row] = LV_HIGH;
            g_row_stable[row] = 1;
            forced = 1;
        } else if ((plane1[1] & 1) || (plane1[1] & 2)) {
            g_row_value[row] = LV_LOW;
            g_row_stable[row] = 1;
            forced = 1;
        }
        for (int w = 1; w < g_plane_words; w++) {
            uint16_t used = plane1[w];
            uint16_t fb = g_fb_symbols[w - 1];
            uint16_t undefined = g_fb_undefined[w - 1];
            uint16_t unknown = (uint16_t)(((g_in_valid[w - 1] ^ used) | g_in_unknown[w - 1]) & used);
            uint16_t match = (uint16_t)(~(g_in_high[w - 1] ^ plane2[w]) & used & ~unknown);
            uint16_t need;
            if (used == match) {
                n_true++;
            } else if ((uint16_t)(match | unknown) == used) {
                n_unknown++;
            } else {
                n_false++;
            }
            need = (uint16_t)((~fb & used) | (undefined & used & fb));
            if (((match | unknown) & need) != need) {
                n_fb_bad++;
            }
            if ((used & fb) == (undefined & used & fb)) {
                n_fb_all++;
            }
            g_term_outputs[w - 1] |= plane0[w];
        }
        if (!forced) {
            if (n_false != 0) {
                g_row_value[row] = LV_LOW;
            } else if (n_true == g_plane_words - 1) {
                g_row_value[row] = LV_HIGH;
            } else if (n_unknown == 0) {
                report_error(0x3fd, "");
            } else {
                g_row_value[row] = LV_UNKNOWN;
            }
            if (n_fb_bad != 0) {
                g_row_stable[row] = 1;
            } else {
                g_row_stable[row] = (n_fb_all == g_plane_words - 1);
            }
        }
        if (g_trace_level == 3) {
            print_trace_line(plane0, plane1, plane2, (int8_t)g_row_value[row], 0);
        }
        plane0 += g_row_words;
        plane1 += g_row_words;
        plane2 += g_row_words;
    }
}

/* OR the rows that drive `sym`.  A row whose plane 1 header has bit 1 set
 * ends the first group; two groups are XORed. */
static void combine_rows(const term_entry_t *t, int sym)
{
    const uint16_t *plane0 = t->frame->rows;
    const uint16_t *plane1 = plane0 + g_plane_words;
    int n_unknown[2] = { 0, 0 }, n_high[2] = { 0, 0 }, n_low[2] = { 0, 0 };
    int n_high_unstable[2] = { 0, 0 }, n_unstable[2] = { 0, 0 }, value[2] = { 0, 0 };
    int group = 0;
    int unstable = 0;
    int row = t->first_row;
    for (int r = 0; r < t->nrows; r++, row++) {
        if (bit_test(plane0 + 1, sym)) {
            if (g_row_value[row] == LV_LOW) {
                n_low[group]++;
            } else if (g_row_value[row] == LV_HIGH) {
                n_high[group]++;
                if (!g_row_stable[row]) {
                    n_high_unstable[group]++;
                }
            } else {
                n_unknown[group]++;
            }
            if (!g_row_stable[row]) {
                n_unstable[group]++;
            }
            if ((plane1[0] & 2) && ++group > 1) {
                fatal_error(0x3ff, "");
            }
        }
        plane0 += g_row_words;
        plane1 += g_row_words;
    }
    (void)n_low;
    for (int k = 0; k <= group; k++) {
        value[k] = n_high[k] != 0 ? LV_HIGH : (n_unknown[k] != 0 ? LV_UNKNOWN : LV_LOW);
        if (value[k] == LV_HIGH || n_unstable[k] < 1) {
            if (n_high[k] == n_high_unstable[k] && n_high_unstable[k] > 0) {
                unstable = 1;
            }
        } else {
            unstable = 1;
        }
    }
    if (group < 1) {
        g_sig_value[sym] = (uint8_t)value[0];
    } else if (value[0] == LV_LOW) {
        g_sig_value[sym] = (uint8_t)value[1];
    } else if (value[0] == LV_UNKNOWN) {
        g_sig_value[sym] = LV_UNKNOWN;
    } else {
        g_sig_value[sym] = (uint8_t)k_invert_level[value[1]];
    }
    g_sig_unstable[sym] = (uint8_t)unstable;
}

/* Values of the symbols driven by one output. */
static int eval_outputs(const term_entry_t *t, int pass)
{
    int changed = 0;
    for (int i = 0; i < g_nsyms; i++) {
        if (bit_test(g_term_outputs, i)) {
            combine_rows(t, i);
            if (eval_node_cell(i, pass) < 1) {
                mark_undefined_feedback(i, 0x409);
            } else {
                changed = 1;
            }
        }
    }
    return changed;
}

/* Report the outputs that did not settle. */
static void report_unstable(void)
{
    for (int i = 0; i < g_value_count; i++) {
        if (g_sig_unstable[g_order[i].sym_index] != 0) {
            report_error_at(ERR_UNSTABLE, g_order[i].text_col, g_order[i].sym->name);
        }
    }
}

/* Evaluate the whole design until nothing changes any more (at most
 * MAX_SETTLE_PASSES times). */
static void settle(int pass)
{
    if (jed150x_active()) { jed150x_step(pass); return; }
    int twice = dev_header()[0x11] == 3;
    memset(g_fb_undefined, 0, g_plane_words * sizeof(uint16_t));
    for (int iteration = 0;; ) {
        int changed = 0;
        for (int round = 0; round < (twice ? 2 : 1); round++) {
            for (int i = 0; i < g_nterms; i++) {
                const term_entry_t *t = &g_terms[i];
                if (t->frame != NULL && t->nrows != 0) {
                    eval_rows(t);
                    if (eval_outputs(t, pass)) {
                        changed = 1;
                    }
                }
            }
        }
        if (!changed) {
            break;
        }
        compare_results(0);
        print_vector(0, 1);
        if (++iteration >= MAX_SETTLE_PASSES) {
            int errors = g_error_count;
            report_unstable();
            if (g_error_count == errors) report_error(ERR_UNSTABLE, "internal feedback");
            return;
        }
    }
}

/* -------------------------------------------------------------------------
 * One test vector
 * ------------------------------------------------------------------------- */

static void copy_values_to_results(void)
{
    for (int i = 0; i < g_order_count; i++) {
        g_order[i].result = g_order[i].value;
    }
}

/* $TRACE 4: show the values in front of the output buffers. */
static void results_before_buffers(void)
{
    for (int i = 0; i < g_order_count; i++) {
        int level = (int8_t)g_node_level[g_order[i].sym_index];
        g_order[i].result = (level == 0) ? 0 : k_level_token[level & 7];
    }
}

/* Turn the simulated levels into result tokens and, if `check`, compare
 * them with the expected values. */
void compare_results(int check)
{
    for (int i = 0; i < g_order_count; i++) {
        order_t *o = &g_order[i];
        int level = (int8_t)(o->node_level ? g_node_level[o->sym_index] : g_sig_out[o->sym_index]);
        int token;
        if (level == 0) {
            o->result = o->value;
            continue;
        }
        if (level >= 9) {
            report_error(0x400, VTEXT("", "module CONVERT"));
            continue;
        }
        token = k_level_token[level];
        if (!o->positive) {
            if (token == T_VAL_H) {
                token = T_VAL_L;
            } else if (token == T_VAL_L) {
                token = T_VAL_H;
            }
        }
        if (token == T_VAL_Z &&
            (o->value == T_VAL_0 || o->value == T_VAL_1 || o->value == T_VAL_X || o->value == T_VAL_Z)) {
            o->result = o->value;
            continue;
        }
        if (check && o->value != token && o->value != T_VAL_STAR && o->value != T_VAL_N) {
            char expected[2] = { (char)value_token_char((int8_t)o->value), '\0' };
            report_expected(o->text_col, expected[0] != '\0' ? expected : NULL, o->sym->name);
        }
        o->result = (int16_t)((o->value == T_VAL_N) ? T_VAL_N : token);
    }
}

/* Linear congruential generator for test value R (state * 0x015a4e35 + 1,
 * result bits 16..30).  The sequence is fixed so that R vectors are
 * reproducible. */
static int lcg_rand(void)
{
    g_rand_seed = g_rand_seed * 0x015a4e35u + 1u;
    return (int)((g_rand_seed >> 16) & 0x7fff);
}

static int pin_kind(const sym_t *s)
{
    return dev_pin(s->pin_rec)[PIN_KIND];
}

static void bad_test_value(const order_t *o, int index)
{
    char text[2] = { (char)value_token_char((int8_t)o->value), '\0' };
    if (g_vsima) {
        report_bad_value(o->text_col, text[0] != '\0' ? text : NULL, o->sym->name);
    } else {
        report_error_at(ERR_BAD_TEST_VALUE, o->text_col, text[0] != '\0' ? text : NULL);
    }
    set_input_unknown(index);
}

/* ATV5000 ABS terms retain logical variable polarity. Pin and clock
 * levels remain physical; only the equation input plane needs conversion. */
static void set_vector_input_value(int value, int sym)
{
    if (strcmp(g_device_name, "v5000") == 0 && (g_syms[sym].flags & 0x10)) {
        value = k_invert_level[value & 7];
    }
    set_input_value(value, sym);
}

/* Apply the test values of the vector to the inputs. */
static void apply_vector(void)
{
    g_preload = 0;
    g_preload_pin = 0;
    for (int i = 0; i < g_value_count; i++) {
        const order_t *o = &g_order[i];
        const sym_t *s = &g_syms[o->sym_index];
        int kind = pin_kind(s);
        if (o->value == T_VAL_P) {
            g_preload = 1;
            break;
        }
        if (g_vsima) {
            continue;
        }
        if (kind == 0x11 &&
            ((o->value == T_VAL_K && o->positive == 1) || (o->value == T_VAL_C && o->positive == 0) ||
             (o->value == T_VAL_0 && o->positive == 1) || (o->value == T_VAL_1 && o->positive == 0))) {
            g_preload = 1;
        }
        if (kind == 0x12 &&
            ((o->value == T_VAL_0 && o->positive == 1) || (o->value == T_VAL_1 && o->positive == 0))) {
            g_preload_pin = 1;
        }
        if (kind == 0x18 &&
            ((o->value == T_VAL_0 && o->positive == 0) || (o->value == T_VAL_1 && o->positive == 1))) {
            g_preload_pin = 1;
        }
    }
    for (int i = 0; i < g_value_count; i++) {
        order_t *o = &g_order[i];
        int sym = o->sym_index;
        const sym_t *s = &g_syms[sym];
        int no_pin_cell = (g_cells[sym].pin_type == 0);
        int cell = g_cells[sym].kind;
        int value = (int8_t)o->value;
        int kind, fb;
        if (!g_preload && (o->positive != 1 || (g_vsima && g_cells[sym].true_out == 0))) {
            switch (value) {
            case T_VAL_1: value = T_VAL_0; break;
            case T_VAL_0: value = T_VAL_1; break;
            case T_VAL_C: value = T_VAL_K; break;
            case T_VAL_K: value = T_VAL_C; break;
            default: break;
            }
        }
        if (value == T_VAL_R) {
            value = (lcg_rand() & 1) ? T_VAL_1 : T_VAL_0;
            if (g_vsima) {
                o->value = (int16_t)value;
            }
        }
        kind = g_vsima ? 0 : pin_kind(s);
        switch (value) {
        case T_VAL_H:
        case T_VAL_L:
            g_vec_level[sym] = (value == T_VAL_H) ? LV_HIGH : LV_LOW;
            if (no_pin_cell && g_preload) {
                bad_test_value(o, sym);
            }
            break;
        case T_VAL_Z:
            g_sig_hiz[sym] = T_VAL_Z;
            g_vec_level[sym] = LV_UNKNOWN;
            break;
        case T_VAL_X:
            g_vec_level[sym] = LV_UNKNOWN;
            if (!no_pin_cell) {
                g_pin_reg_input[sym] = LV_UNKNOWN;
                if (!g_preload_pin) {
                    break;
                }
            }
            if (g_preload) {
                g_reg_state[sym] = LV_UNKNOWN;
                if (!g_preload_pin) {
                    break;
                }
            }
            fb = (int16_t)find_feedback_symbol(sym, 0x25);
            if (fb > 0) {
                set_input_unknown(fb);
                set_pin_value(fb, LV_UNKNOWN);
            }
            set_input_unknown(sym);
            set_pin_value(sym, LV_UNKNOWN);
            break;
        case T_VAL_P:
        case T_VAL_C:
        case T_VAL_K:
            if (g_vsima) {
                if (s->pin_rec < 0) {
                    bad_test_value(o, (int8_t)o->value);
                } else if (!g_preload) {
                    set_pin_value(sym, value == T_VAL_C ? LV_LOW : LV_HIGH);
                }
                set_vector_input_value(value == T_VAL_C ? LV_LOW : LV_HIGH, sym);
                break;
            }
            if (s->pin_rec < 0 ||
                (kind != 5 && kind != 0x10 && kind != 0x17 && kind != 0xc && kind != 0x16 && kind != 0x11)) {
                bad_test_value(o, (int8_t)o->value);
            } else if (kind == 0x11 &&
                       ((o->positive == 1 && value != T_VAL_K) || (o->positive == 0 && value != T_VAL_C))) {
                bad_test_value(o, (int8_t)o->value);
            } else if (!g_preload) {
                set_pin_value(sym, value == T_VAL_C ? LV_LOW : LV_HIGH);
            } else if (value == T_VAL_C && kind == 0x11) {
                value = T_VAL_K;
                o->value = T_VAL_K;
            }
            if (kind == 0xc || kind == 0x16 || kind == 0x10 || kind == 0x17 || kind == 0 || kind == 0x11) {
                set_vector_input_value(value == T_VAL_C ? LV_LOW : LV_HIGH, sym);
            }
            break;
        case T_VAL_N:
        case T_VAL_STAR:
            if (cell != 3 && cell != 2) {
                bad_test_value(o, sym);
            }
            g_vec_level[sym] = g_reg_state[sym];
            break;
        case T_VAL_0:
            g_vec_level[sym] = LV_LOW;
            if (!no_pin_cell) {
                g_pin_reg_input[sym] = LV_LOW;
                break;
            }
            if (g_preload) {
                g_reg_state[sym] = (dev_header()[0x42] == 1) ? LV_LOW : LV_HIGH;
                break;
            }
            if (cell != 3 && cell != 1) {
                bad_test_value(o, (int8_t)o->value);
            }
            fb = (int16_t)find_feedback_symbol(sym, 0x25);
            if (fb < 1) {
                set_vector_input_value(LV_LOW, sym);
                set_pin_value(sym, kind == 0x12 ? LV_HIGH : LV_LOW);
            } else {
                set_vector_input_value(LV_LOW, fb);
                set_pin_value(fb, LV_LOW);
            }
            break;
        case T_VAL_1:
            g_vec_level[sym] = LV_HIGH;
            if (!no_pin_cell) {
                g_pin_reg_input[sym] = LV_HIGH;
                break;
            }
            if (g_preload) {
                g_reg_state[sym] = (dev_header()[0x42] == 1) ? LV_HIGH : LV_LOW;
                if (!g_preload_pin) {
                    break;
                }
            }
            if (cell != 3 && cell != 1 && !g_preload) {
                bad_test_value(o, (int8_t)o->value);
            }
            fb = (int16_t)find_feedback_symbol(sym, 0x25);
            if (fb < 1) {
                set_vector_input_value(LV_HIGH, sym);
                set_pin_value(sym, kind == 0x12 ? LV_LOW : LV_HIGH);
            } else {
                set_vector_input_value(LV_HIGH, fb);
                set_pin_value(fb, LV_HIGH);
            }
            break;
        default:
            fatal_error(0x3fe, VTEXT("", "module CONVERT"));
        }
    }
}

/* Drive the clock pins to their level before (phase 2) or after (phase 3)
 * the active edge. */
static int drive_clocks(int phase)
{
    int any = 0;
    for (int i = 0; i < g_value_count; i++) {
        const order_t *o = &g_order[i];
        int sym = o->sym_index;
        const sym_t *s = &g_syms[sym];
        int kind;
        int value = o->value;
        if ((strcmp(g_device_name, "v5000") == 0 || jed150x_active()) && !o->positive) {
            if (value == T_VAL_C) value = T_VAL_K;
            else if (value == T_VAL_K) value = T_VAL_C;
        }
        if (s->pin_rec < 0) {
            continue;
        }
        if (g_vsima) {
            if ((value == T_VAL_C && phase == 3) || (value == T_VAL_K && phase == 2)) {
                g_sig_value[sym] = LV_LOW;
            } else if ((value == T_VAL_C && phase == 2) || (value == T_VAL_K && phase == 3)) {
                g_sig_value[sym] = LV_HIGH;
                if (value == T_VAL_C) {
                    g_sig_prev[sym] = LV_LOW;
                }
            }
            any = 1;
            continue;
        }
        kind = pin_kind(s);
        if (kind != 5 && kind != 0xc && kind != 0x16 && kind != 0x10 && kind != 0x17) {
            continue;
        }
        if ((value == T_VAL_C && phase == 3) || (value == T_VAL_K && phase == 2)) {
            g_sig_value[sym] = LV_LOW;
            if (kind != 5) {
                set_vector_input_value(LV_LOW, sym);
            }
        } else if ((value == T_VAL_C && phase == 2) || (value == T_VAL_K && phase == 3)) {
            g_sig_value[sym] = LV_HIGH;
            if (value == T_VAL_C && phase == 2 && g_sig_prev[sym] == LV_UNKNOWN) {
                g_sig_prev[sym] = LV_LOW;
            }
            if (kind != 5) {
                set_vector_input_value(LV_HIGH, sym);
            }
        }
        any = 1;
    }
    return any;
}

static void remember_levels(void)
{
    memcpy(g_sig_prev, g_sig_value, g_nsyms);
    memcpy(g_reg_prev, g_reg_state, g_nsyms);
}

/* Is a clock at its rising edge? */
static int clock_edge_pending(void)
{
    for (int i = 0; i < g_nsyms; i++) {
        if (g_sig_is_clock[i] == 1 && g_sig_value[i] == LV_HIGH && g_sig_prev[i] == LV_LOW) {
            return 1;
        }
    }
    return 0;
}

static int tracing_steps(void)
{
    return g_trace_level > 1 && g_trace_level < 4;
}

/* One clock phase of simulate_vector: drive the clocks, then settle until
 * no clock edge is pending. */
static void clock_phase(int phase, int loop_pass)
{
    drive_clocks(phase);
    update_cells(phase);
    eval_pin_cells(PIN_CELLS_ALL, phase);
    remember_levels();
    settle(1);
    for (int iteration = 0; clock_edge_pending(); iteration++) {
        if (iteration == MAX_SETTLE_PASSES) {
            report_error(ERR_UNSTABLE, "clock feedback");
            return;
        }
        update_cells(phase);
        eval_pin_cells(PIN_CELLS_ALL, phase);
        remember_levels();
        settle(loop_pass);
    }
}

/* Simulate the vector whose values are in g_order[].value. */
void simulate_vector(void)
{
    clear_vector_state();
    apply_vector();
    if (g_sim_on) {
        int clocked;
        if (g_preload) {
            copy_values_to_results();
            update_cells(0);
            if (tracing_steps()) {
                print_trace_title("****** Preload ******");
            }
            return;
        }
        clocked = g_vsima ? design_has_registers() : rd16(dev_header() + 0x19) != 0;
        if (tracing_steps() && clocked) {
            print_trace_title("****** Before Clock ******");
        }
        memset(g_sig_out, 0, g_nsyms);
        if (!g_vsima) {
            eval_pin_cells(PIN_CELLS_COUNTERS, 0);
        }
        settle(1);
        eval_pin_cells(PIN_CELLS_NOT_D, 1);
        settle(1);
        remember_levels();
        if (g_trace_level == 2) {
            compare_results(0);
            print_vector(0, 1);
        }
        if (g_vsima ? design_has_registers() : clocked) {
            if (tracing_steps()) {
                print_trace_title("****** At Clock ******");
            }
            if (!g_vsima) {
                /* clocks start from the level they ended at */
                for (int i = 0; i < g_nsyms; i++) {
                    if (g_sig_is_clock[i] == 1) {
                        g_sig_prev[i] = g_sig_saved[i];
                    }
                }
            }
            eval_pin_cells(PIN_CELLS_ALL, 0);
            settle(1);
            if (g_vsima) {
                for (int i = 0; i < g_nsyms; i++) {
                    if (g_sig_is_clock[i] == 1) {
                        g_sig_prev[i] = g_sig_saved[i];
                    }
                }
            }
            clock_phase(2, g_vsima ? 2 : 1);
            if (tracing_steps()) {
                print_trace_line(NULL, NULL, NULL, 0, 1);
            }
            memset(g_sig_out, 0, g_nsyms);
            if (tracing_steps()) {
                print_trace_title("****** After Clock ******");
            }
            if (g_vsima) {
                settle(3);
            }
            clock_phase(3, 1);
            memcpy(g_sig_saved, g_sig_value, g_nsyms);
            if (tracing_steps()) {
                compare_results(0);
                print_vector(0, 1);
            }
        }
    }
    if (g_trace_level == 4) {
        print_trace_title("\n****** before output buffer ******");
        results_before_buffers();
        print_vector(0, 1);
    }
    compare_results(1);
}

/* -------------------------------------------------------------------------
 * Stuck-at faults (csima only)
 * ------------------------------------------------------------------------- */

/* First fuse number and fuses per row of output `term`. */
static long term_first_fuse(int term, long *step)
{
    int count = dev_section_count(DEV_SEC_NODES);
    for (int i = 0; i < count; i++) {
        const uint8_t *node = dev_node(i);
        if ((int)rd16(node) == term && node[4] == 0x0e) {
            *step = node[0x1b];
            return (long)(int32_t)rd32(node + 10);
        }
    }
    *step = 0;
    return -1;
}

/* The product term row a fault id (fuse number) belongs to, -1 if none. */
int stuck_fault_row(unsigned long id)
{
    for (int i = 0; i < g_nterms; i++) {
        const term_entry_t *t = &g_terms[i];
        if (t->frame != NULL && t->nrows != 0) {
            long step = 0;
            long fuse = term_first_fuse(i, &step);
            for (int r = 0; r < t->nrows; r++) {
                if (fuse == (long)(int32_t)id) {
                    return t->first_row + r;
                }
                fuse += step;
            }
        }
    }
    return -1;
}


/* -------------------------------------------------------------------------
 * vsima only
 * ------------------------------------------------------------------------- */
static int is_register_extension(int code)
{
    switch (code) {
    case 0x01: case 0x1b: case 0x1c: case 0x16: case 0x17: case 0x18: case 0x0a: case 0x0b:
    case 0x08: case 0x09: case 0x31: case 0x32: case 0x22: case 0x24: case 0x23: case 0x33:
    case 0x35: case 0x34:
        return 1;
    default:
        return 0;
    }
}

/* Does the design have anything that is clocked? */
int design_has_registers(void)
{
    if (!g_dev_has_class) {
        return rd16(dev_header() + 0x19) != 0;
    }
    for (int i = 0; i < g_nsyms; i++) {
        if (is_register_extension(sym_type_code(&g_syms[i]))) {
            return 1;
        }
    }
    return 0;
}

/* Warn about names that have a clock (.ck, .ce, .ckmux,
 * .iock, .lemux) but no register.  The loop counts from 2 but starts at
 * the first symbol, so the last two symbols are never looked at. */
void warn_missing_registers(void)
{
    char name[80] = "";
    int has_register = 0;
    int has_clock = 0;
    for (int i = 0; i + 2 < g_nsyms; i++) {
        const sym_t *s = &g_syms[i];
        int code;
        if (s->kind == 3) {
            /* a variable, such as the ".q.s" REGISTER_SELECT leaves
             * between q.d and q.ck; it would split q's symbols */
            continue;
        }
        if (strcmp(s->name, name) != 0) {
            if (has_clock && !has_register) {
                print_warning_now(ERR_REGISTER_TYPE, name);
            }
            snprintf(name, sizeof(name), "%s", s->name);
            has_register = 0;
            has_clock = 0;
        }
        code = sym_type_code(s);
        if (is_register_extension(code)) {
            has_register = 1;
        }
        if (code == 0x0f || code == 0x1f || code == 0x2a || code == 0x3a || code == 0x2c) {
            has_clock = 1;
        }
    }
    if (has_clock && !has_register) {
        print_warning_now(ERR_REGISTER_TYPE, name);
    }
}
