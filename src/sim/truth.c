/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, LinCUPL team
 *
 * Exhaustively drive a decoded combinational map through csim. A new
 * power-up state for every row prevents one row from hiding undefined
 * behavior in the next. Registered designs and output feedback are
 * rejected instead of treating a finite simulation as a proof.
 */
#include "sim.h"

static int fail(char *error, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(error, size, fmt, ap);
    va_end(ap);
    return 0;
}

static int pin_symbol(unsigned pin)
{
    for (int i = 0; i < g_nsyms; i++) {
        if (g_syms[i].kind == 2 && g_syms[i].pin == pin)
            return i;
    }
    return -1;
}

static int order_pin(unsigned pin, int token)
{
    int sym = pin_symbol(pin);
    order_t *o = &g_order[g_order_count];
    if (sym < 0)
        return 0;
    o->sym_index = sym;
    o->sym = &g_syms[sym];
    o->positive = 1;
    o->value = (int16_t)token;
    g_order_count++;
    return 1;
}

int build_truth_table(unsigned max_inputs, sim_truth_t *table, char *error, size_t size)
{
    jedec_truth_pins(table);
    if (jed150x_sequential() || sim_power_down_symbol() >= 0)
        return fail(error, size, "stateful design; use --sequential for reachable-state equivalence");
    for (unsigned i = 0; i < table->noutputs; i++) {
        unsigned pin = table->outputs[i];
        int sym = pin_symbol(pin);
        if (table->role[pin] == SIM_PIN_REGISTER)
            return fail(error, size, "P%u is registered; use --sequential for reachable-state equivalence", pin);
        if (sym < 0)
            return fail(error, size, "P%u has no simulation symbol", pin);
        if (g_syms[sym].flags & 0x80)
            return fail(error, size, "P%u feeds the array; use --sequential to compare output feedback", pin);
    }
    for (int i = 0; i < g_nsyms; i++) {
        if (g_syms[i].pin > table->npins && g_cells[i].is_node &&
            (g_cells[i].node_type == 1 || g_cells[i].node_type == 0x1b))
            return fail(error, size, "buried registers are present; use --sequential for reachable-state equivalence");
    }
    if (table->ninputs > max_inputs)
        return fail(error, size, "%u used inputs exceed the limit of %u", table->ninputs, max_inputs);
    table->nrows = UINT32_C(1) << table->ninputs;
    table->values = malloc((size_t)table->nrows * table->noutputs + 1);
    if (!table->values)
        return fail(error, size, "out of memory for truth table");

    g_order = alloc_tracked((g_nsyms + 2) * sizeof(order_t), "truth table");
    g_order_count = 0;
    for (unsigned i = 0; i < table->ninputs; i++) {
        if (!order_pin(table->inputs[i], T_VAL_0))
            return fail(error, size, "P%u has no simulation symbol", table->inputs[i]);
    }
    for (unsigned i = 0; i < table->noutputs; i++) {
        if (!order_pin(table->outputs[i], T_VAL_STAR))
            return fail(error, size, "P%u has no simulation symbol", table->outputs[i]);
    }
    g_value_count = g_order_count;
    g_sim_on = 1;
    g_trace_level = 0;
    for (uint32_t row = 0; row < table->nrows; row++) {
        reset_simulation();
        memset(g_vec_level, LV_UNKNOWN, g_nsyms);
        memset(g_sig_saved, LV_LOW, g_nsyms);
        jedec_design_bound();
        for (unsigned i = 0; i < table->ninputs; i++)
            g_order[i].value = (row & (UINT32_C(1) << i)) ? T_VAL_1 : T_VAL_0;
        simulate_vector();
        if (g_error_count)
            return fail(error, size, "simulation failed at input assignment %u", row);
        for (unsigned i = 0; i < table->noutputs; i++) {
            int sym = g_order[table->ninputs + i].sym_index;
            int level = g_sig_out[sym];
            unsigned char value;
            switch (level) {
            case LV_LOW: value = '0'; break;
            case LV_HIGH: value = '1'; break;
            case LV_HIGHZ: value = 'Z'; break;
            default:
                return fail(error, size, "P%u is indeterminate at input assignment %u",
                            table->outputs[i], row);
            }
            table->values[(size_t)row * table->noutputs + i] = value;
        }
    }
    return 1;
}

void sim_truth_free(sim_truth_t *table)
{
    free(table->values);
    memset(table, 0, sizeof(*table));
}
