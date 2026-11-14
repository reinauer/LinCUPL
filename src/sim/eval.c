/*
 * sim_eval - the macrocell models: what a register, latch, output
 * buffer or feedback path of the device does with the values computed from
 * the product terms.
 *
 * Part of the CUPL simulator (see sim.c).
 *
 * Every model is called as eval(sym, pass): pass 0 only propagates, pass 1
 * also applies the asynchronous controls, passes 2 and 3 (clock phases)
 * additionally let a clock edge load the register.  The result (used only
 * for node cells) is non-zero if the output depends on an unstable signal.
 *
 * Slots of the cell state are filled by the init functions in
 * bind.c; -1 means "no such signal".
 */
#include "sim.h"

/* column / row of a logic level in the tables below */
static const uint8_t k_level_index[8] = { 0, 0, 0, 1, 1, 0, 2, 1 };
/* two output enables combined (device header +0x2e = 2 / 3) */
static const uint8_t k_enable_mix_2[9] = { 3, 2, 6, 2, 2, 2, 6, 2, 6 };
static const uint8_t k_enable_mix_3[9] = { 2, 3, 6, 2, 2, 2, 2, 6, 6 };
/* output of a three-state buffer, [value][enable] */
static const uint8_t k_buffer[9] = { LV_HIGHZ, LV_LOW, LV_UNKNOWN, LV_HIGHZ, LV_HIGH, LV_UNKNOWN,
                                     LV_HIGHZ, LV_UNKNOWN, LV_UNKNOWN };

static int level_index(int level)
{
    return k_level_index[level & 7];
}

static int invert(int level)
{
    return (level >= 0 && level < 8) ? k_invert_level[level] : 0;
}

/* Is the control signal in slot value `sym` present and high? */
static int is_high(int sym)
{
    return sym >= 0 && g_sig_value[sym] == LV_HIGH;
}

static int device_polarity(void)
{
    return dev_header()[0x2f];
}

/* Did the clock just make its active edge? */
static int clock_edge(int clock, int polarity)
{
    if (clock < 0) {
        return 0;
    }
    if (polarity == 0x0f && g_sig_prev[clock] == LV_LOW && g_sig_value[clock] == LV_HIGH) {
        return 1;
    }
    if (polarity == 0x27 && g_sig_prev[clock] == LV_HIGH && g_sig_value[clock] == LV_LOW) {
        return 1;
    }
    return 0;
}

/* clock_edge, or the clock resting low. */
static int clock_edge_or_low(int clock, int polarity)
{
    if (clock < 0) {
        return 0;
    }
    if (clock_edge(clock, polarity)) {
        return 1;
    }
    return g_sig_prev[clock] == LV_LOW && g_sig_value[clock] == LV_LOW;
}

/* Is the latch transparent? */
static int latch_open(int enable, int polarity)
{
    int level;
    if (enable < 0) {
        return 0;
    }
    level = (int8_t)(g_sig_value[enable] == 0 ? g_vec_level[enable] : g_sig_value[enable]);
    if ((polarity == 0x1d || polarity == 0x0f) && level == LV_HIGH) {
        return 1;
    }
    if ((polarity == 0x1e || polarity == 0x27) && level == LV_LOW) {
        return 1;
    }
    return 0;
}

/* The value a register of `sym` loads from symbol `src`. */
static int register_input(int sym, int src)
{
    int level = (int8_t)g_sig_value[src];
    int polarity = device_polarity();
    if (g_cells[sym].true_out == 0) {
        return polarity == 1 ? invert(level) : level;
    }
    return polarity == 2 ? invert(level) : level;
}

/* The level the test vector applies to pin `number`. */
static int vector_level_of_pin(int sym, int number)
{
    if (g_vsima) {
        /* vsima: by name, the first column that gives a level */
        int level = LV_UNKNOWN;
        for (int i = 0; i < g_value_count && level == LV_UNKNOWN; i++) {
            const order_t *o = &g_order[i];
            const sym_t *s = &g_syms[o->sym_index];
            if (strcmp(g_syms[sym].name, s->name) == 0) {
                if (o->value == T_VAL_0) {
                    level = (s->flags & 0x10) ? LV_HIGH : LV_LOW;
                } else if (o->value == T_VAL_1) {
                    level = (s->flags & 0x10) ? LV_LOW : LV_HIGH;
                }
            }
        }
        return level;
    }
    for (int i = 0; i < g_value_count; i++) {
        const order_t *o = &g_order[i];
        if (o->pin == number) {
            if (o->value == T_VAL_0) {
                return o->positive ? LV_LOW : LV_HIGH;
            }
            if (o->value == T_VAL_1) {
                return o->positive ? LV_HIGH : LV_LOW;
            }
            return LV_UNKNOWN;
        }
    }
    return LV_UNKNOWN;
}

/* Combine two output enable signals with one of the k_enable_mix tables. */
static int mix_enables(const uint8_t *table, int second, int first)
{
    int a = first < 0 ? LV_UNKNOWN : (int8_t)g_sig_value[first];
    int b = second < 0 ? LV_UNKNOWN : (int8_t)g_sig_value[second];
    return table[level_index(b) * 3 + level_index(a)];
}

/* Pass `value` through the output buffer of `sym`, which is enabled by a
 * pin, by a product term or by a second enable. */
static int buffer_output(int sym, int value, int oe_pin, int oe_term, int oe_second)
{
    int mode = -1;
    int first = -1;
    int enable;
    (void)sym;
    if (oe_pin >= 0) {
        first = oe_pin;
        mode = 0;
    } else if (oe_term >= 0) {
        first = oe_term;
        mode = 1;
    }
    if (oe_second >= 0 && !g_vsima) {
        mode = 2;
    }
    switch (mode) {
    case -1:
        enable = LV_HIGH;
        break;
    case 0: {
        const sym_t *s = &g_syms[first];
        if (g_vsima) {
            /* vsima: an enable tied to ground or VCC */
            if (s->pin_rec >= 0 && g_dev_has_class && strcmp(s->name, "_gnd") == 0) {
                enable = LV_LOW;
            } else if (s->pin_rec >= 0 && g_dev_has_class && strcmp(s->name, "_vcc") == 0) {
                enable = LV_HIGH;
            } else if (s->pin_rec >= 0 && !g_dev_has_class && dev_pin(s->pin_rec)[PIN_KIND] == 2) {
                enable = LV_LOW;
            } else if (s->pin_rec >= 0 && !g_dev_has_class && dev_pin(s->pin_rec)[PIN_KIND] == 3) {
                enable = LV_HIGH;
            } else {
                enable = (int8_t)g_sig_value[first];
            }
        } else if (s->pin_rec < 0) {
            enable = (int8_t)g_sig_value[first];
            if (node_kind_default(dev_node(s->node_rec)[5]) == 0) {
                enable = invert(enable);
            }
        } else if (dev_pin(s->pin_rec)[PIN_KIND] == 2) {
            enable = LV_LOW;
        } else if (dev_pin(s->pin_rec)[PIN_KIND] == 3) {
            enable = LV_HIGH;
        } else {
            enable = (int8_t)g_sig_value[first];
        }
        break;
    }
    case 1:
        enable = (int8_t)g_vec_level[first];
        if (g_vsima) {
            if (g_syms[first].pin_rec >= 0 && (g_syms[first].flags & 0x10)) {
                enable = invert(enable);
            }
        } else if (dev_header()[0x2e] != 4) {
            enable = invert(enable);
        }
        break;
    default:
        if (dev_header()[0x2e] == 2) {
            enable = mix_enables(k_enable_mix_2, oe_second, first);
        } else if (dev_header()[0x2e] == 3) {
            enable = mix_enables(k_enable_mix_3, oe_second, first);
        } else {
            enable = LV_HIGH;
            report_error(0x40b, "");
        }
        break;
    }
    return (int8_t)k_buffer[level_index(enable) + level_index(value) * 3];
}

/* Drive the pin of node `sym` with `value` (the state of its register if
 * `registered`), and feed the result back into the array. */
static int drive_output(int sym, int registered, int value, int oe_pin, int oe_term, int oe_second)
{
    const cell_t *c = &g_cells[sym];
    const sym_t *s = &g_syms[sym];
    int polarity = device_polarity();
    int out, pin_level, kind, feedback = 0, skip = 0;
    if (value < 0) {
        return 0;
    }
    g_node_level[sym] = (uint8_t)value;
    if (g_vsima && dev_header()[0x12] == 10 && c->node_type == 0x1b && c->true_out == 0) {
        out = invert(value);
    } else if (c->true_out == 0) {
        out = (polarity == 0 || !registered || polarity == 2) ? invert(value) : value;
    } else {
        out = (polarity == 2 && registered) ? invert(value) : value;
    }
    pin_level = buffer_output(sym, out, oe_pin, oe_term, oe_second);
    if (s->pin_rec < 0) {
        kind = -1;
    } else if (!g_vsima || (!g_dev_has_class && s->pin != 0)) {
        kind = dev_pin(s->pin_rec)[PIN_KIND];
    } else if (c->node_type == 0 || c->node_type == 0x25 || c->node_type == 0x33 || c->node_type == 0x34) {
        kind = 7;
    } else {
        kind = 0x0b;
    }
    if (g_feedback_sym[sym] == 0) {
        return pin_level;
    }
    if (kind == 7 || kind == 0x10) {
        if (pin_level != LV_HIGHZ) {
            feedback = pin_level;
        } else if (g_vec_level[sym] == LV_HIGH || g_vec_level[sym] == LV_LOW) {
            skip = 1;
        } else {
            feedback = registered ? LV_HIGH : LV_UNKNOWN;
        }
    } else if (kind == 0x0b || kind == 0x17) {
        if (c->node_type == 0) {
            if (pin_level != LV_HIGHZ) {
                feedback = pin_level;
            } else if (g_order[sym].value == T_VAL_Z) {     /* deliberately indexed by symbol */
                feedback = registered ? LV_HIGH : LV_UNKNOWN;
            } else {
                skip = 1;
            }
        } else if (c->node_type == 0x0d) {
            feedback = LV_UNKNOWN;
        } else {
            feedback = out;
        }
    } else {
        feedback = (out == 0) ? LV_UNKNOWN : out;
    }
    if (!skip) {
        if (c->true_fb != 1 && (c->node_type != 0x26 || dev_header()[0x30] != 1)) {
            feedback = invert(feedback);
        }
        set_input_value(feedback, (int16_t)g_feedback_sym[sym]);
    }
    return pin_level;
}

/* The asynchronous part shared by most registers: reset, preset, and an
 * optional further reset.  Returns 1 if one of them was active. */
static int async_controls(uint8_t *state, int reset, int preset, int extra_reset, int *unstable)
{
    if (is_high(reset)) {
        *state = LV_LOW;
        if (unstable != NULL) {
            *unstable = (int8_t)g_sig_unstable[reset];
        }
        return 1;
    }
    if (is_high(preset)) {
        *state = LV_HIGH;
        if (unstable != NULL) {
            *unstable = (int8_t)g_sig_unstable[preset];
        }
        return 1;
    }
    if (is_high(extra_reset)) {
        *state = LV_LOW;
        if (unstable != NULL) {
            *unstable = (int8_t)g_sig_unstable[extra_reset];
        }
        return 1;
    }
    return 0;
}

/* The synchronous overrides checked at a clock edge, in priority order.
 * Returns 1 if one of them set the state. */
static int clocked_controls(uint8_t *state, int reset, int preset, int sync_preset, int sync_reset,
                            int extra_reset)
{
    if (is_high(reset)) {
        *state = LV_LOW;
    } else if (is_high(preset)) {
        *state = LV_HIGH;
    } else if (is_high(sync_preset)) {
        *state = LV_HIGH;
    } else if (is_high(sync_reset)) {
        *state = LV_LOW;
    } else if (is_high(extra_reset)) {
        *state = LV_LOW;
    } else {
        return 0;
    }
    return 1;
}

/* vsima: reset and preset active together leave the register undefined. */
static int both_high_vsima(int reset, int preset)
{
    return g_vsima && is_high(reset) && is_high(preset);
}

static void check_node_type(int sym, int a, int b, int c, int code, const char *name)
{
    int t = g_cells[sym].node_type;
    if (t != a && t != b && t != c) {
        fatal_error(code, name);
    }
}

static void check_pin_type(int sym, int type, int code, const char *name)
{
    if (g_cells[sym].pin_type != type) {
        fatal_error(code, name);
    }
}

/* combinational output (node types 0, 0x0d, 0x30) */
int eval_combinational(int sym, int pass)
{
    const int *s = g_cells[sym].node_state->s;
    if (pass == 2) {
        return 0;
    }
    check_node_type(sym, 0, 0x30, 0x0d, 0x408, VTEXT("", "combinatorial output evaluation"));
    g_sig_out[sym] = (uint8_t)drive_output(sym, 0, (int8_t)g_sig_value[s[0]], s[1], s[2], s[3]);
    return (int8_t)g_sig_unstable[sym];
}

/* D register at an input pin (pin type 0x17) */
int eval_input_register(int sym, int pass)
{
    const int *s = g_cells[sym].pin_state->s;
    uint8_t *q = &g_reg_q[s[1]];
    check_pin_type(sym, 0x17, 0x407, VTEXT("di", "d-type input evaluation"));
    if (g_vsima && clock_edge(s[10], s[12])) {
        *q = g_pin_reg_input[s[0]];
    }
    if (both_high_vsima(s[6], s[7])) {
        *q = LV_UNKNOWN;
    } else if (g_dev_is_7c33x && is_high(s[6]) && is_high(s[7])) {
        *q = g_pin_reg_input[s[0]];
    } else if (!clock_edge(s[10], s[12])) {
        if (pass != 0) {
            async_controls(q, s[6], s[7], -1, NULL);
        }
    } else if (pass > 1) {
        if (!clocked_controls(q, s[6], s[7], s[9], s[8], s[13])) {
            *q = g_pin_reg_input[s[0]];
        }
    }
    set_input_from_reg(s[1], s[0]);
    return 0;
}

/* latch at an input pin (pin type 0x18) */
int eval_input_latch(int sym, int pass)
{
    const int *s = g_cells[sym].pin_state->s;
    uint8_t *q = &g_reg_q[s[1]];
    check_pin_type(sym, 0x18, 0x407, VTEXT("dl", "l-type input evaluation"));
    if (both_high_vsima(s[6], s[7])) {
        *q = LV_UNKNOWN;
    } else if (g_dev_is_7c33x && is_high(s[6]) && is_high(s[7])) {
        *q = g_pin_reg_input[s[0]];
    } else if (!latch_open(s[10], s[11])) {
        if (pass != 0) {
            async_controls(q, s[6], s[7], -1, NULL);
        }
    } else if (pass > 1) {
        if (!clocked_controls(q, s[6], s[7], s[9], s[8], -1)) {
            *q = g_pin_reg_input[s[0]];
        }
    }
    set_input_from_reg(s[1], s[0]);
    return 0;
}

/* D flip-flop (node type 0x15) */
int eval_d(int sym, int pass)
{
    const int *s = g_cells[sym].node_state->s;
    uint8_t *state = &g_reg_state[s[1]];
    int unstable = 0;
    int edge = clock_edge(s[10], s[12]);
    check_node_type(sym, 0x15, 0x15, 0x15, 0x407, VTEXT("dt", "d-type output evaluation"));
    if ((!edge && pass != 0) || (edge && pass > 1)) {
        if (is_high(s[6]) && is_high(s[7])) {
            /* reset and preset together make the register transparent */
            *state = (uint8_t)register_input(sym, s[0]);
            unstable = (int8_t)g_sig_unstable[s[0]];
        } else if (!edge) {
            if (!async_controls(state, s[6], s[7], g_vsima ? -1 : s[13], &unstable)) {
                unstable = (int8_t)g_sig_unstable[s[1]];
            }
        } else if (is_high(s[6])) {
            *state = LV_LOW;
        } else if (is_high(s[7])) {
            *state = LV_HIGH;
        } else if (!g_vsima && is_high(s[13])) {
            *state = LV_HIGH;       /* deliberate: the same signal resets when there is no edge */
        } else {
            *state = (uint8_t)register_input(sym, s[0]);
        }
    }
    g_sig_out[sym] = (uint8_t)drive_output(sym, 1, (int8_t)*state, s[2], s[3], s[4]);
    return unstable;
}

/* D flip-flop with optional second clock (node type 1) */
int eval_d_feedback(int sym, int pass)
{
    const int *s = g_cells[sym].node_state->s;
    uint8_t *state = &g_reg_state[s[1]];
    int unstable = 0;
    check_node_type(sym, 1, 1, 1, 0x407, VTEXT("df", "d-type output evaluation"));
    if (both_high_vsima(s[6], s[7])) {
        *state = LV_UNKNOWN;
    } else if (g_dev_is_7c33x && is_high(s[6]) && is_high(s[7])) {
        *state = (uint8_t)register_input(sym, s[0]);
    } else {
        int edge;
        g_reg_before[s[1]] = *state;
        if (s[11] == -1 || g_vsima) {
            edge = clock_edge(s[10], s[12]);
        } else {
            edge = (clock_edge(s[10], s[12]) & clock_edge_or_low(s[11], s[12])) |
                   (clock_edge_or_low(s[10], s[12]) & clock_edge(s[11], s[12]));
        }
        if (!edge) {
            if (pass != 0 && !async_controls(state, s[6], s[7], s[13], &unstable)) {
                unstable = (int8_t)g_sig_unstable[s[1]];
            }
        } else if (pass > 1) {
            if (!clocked_controls(state, s[6], s[7], s[9], s[8], g_vsima ? -1 : s[13])) {
                *state = (uint8_t)register_input(sym, s[0]);
            }
        }
    }
    if (g_bypass_sym[sym] != 0 && g_sig_value[g_bypass_sym[sym]] == LV_HIGH) {
        /* register bypass selected */
        *state = (uint8_t)register_input(sym, s[0]);
    }
    g_sig_out[sym] = (uint8_t)drive_output(sym, 1, (int8_t)*state, s[2], s[3], -1);
    return unstable;
}

/* D flip-flop with clock enable (node type 0x16) */
int eval_d_enable(int sym, int pass)
{
    const int *s = g_cells[sym].node_state->s;
    const cell_t *c = &g_cells[sym];
    const uint8_t *hdr = dev_header();
    uint8_t *state = &g_reg_state[s[1]];
    int swapped = hdr[0x2f] == 2 && (int16_t)rd16(hdr + 8) < (int16_t)c->number;   /* csima only */
    int unstable = 0;
    int edge;
    check_node_type(sym, 0x16, 0x16, 0x16, 0x407, VTEXT("de", "de-type output evaluation"));
    if (g_vsima) {
        swapped = 0;
    }
    if (both_high_vsima(s[7], s[8])) {
        *state = LV_UNKNOWN;
        g_sig_out[sym] = (uint8_t)drive_output(sym, 1, (int8_t)*state, s[3], s[4], -1);
        return 0;
    }
    g_reg_before[s[1]] = *state;
    edge = clock_edge(s[11], s[12]);
    if (!edge) {
        if (pass != 0) {
            if (is_high(s[7])) {
                *state = swapped ? LV_HIGH : LV_LOW;
                unstable = (int8_t)g_sig_unstable[s[7]];
            } else if (is_high(s[8])) {
                *state = swapped ? LV_LOW : LV_HIGH;
                unstable = (int8_t)g_sig_unstable[s[8]];
            } else if (!g_vsima && is_high(s[13])) {
                *state = LV_LOW;
                unstable = (int8_t)g_sig_unstable[s[13]];
            } else {
                unstable = (int8_t)g_sig_unstable[s[1]];
            }
        }
    } else if (pass > 1) {
        if (is_high(s[7])) {
            *state = (swapped && c->true_out == 0) ? LV_HIGH : LV_LOW;
        } else if (is_high(s[8])) {
            *state = (swapped && c->true_out == 0) ? LV_LOW : LV_HIGH;
        } else if (is_high(s[10])) {
            *state = LV_HIGH;
        } else if (is_high(s[9])) {
            *state = LV_LOW;
        } else if (!g_vsima && is_high(s[13])) {
            *state = LV_LOW;
        } else if (s[2] == -1) {
            *state = (uint8_t)register_input(sym, s[0]);
        } else if (g_sig_value[s[2]] != LV_LOW) {
            if (g_sig_value[s[2]] == LV_HIGH) {
                *state = (uint8_t)register_input(sym, s[0]);
            } else if (g_vsima && g_sig_value[s[2]] != LV_UNKNOWN) {
                /* vsima: only an unknown enable makes the state unknown */
            } else if (*state == LV_LOW && g_reg_state[s[0]] == LV_LOW) {
                *state = LV_LOW;
            } else {
                *state = LV_UNKNOWN;
            }
        }
    }
    g_sig_out[sym] = (uint8_t)drive_output(sym, 1, (int8_t)*state, s[3], s[4], -1);
    return unstable;
}

/* SR (node type 8) and JK (node type 10) flip-flops;
 * slot 0 is S / J, slot 1 is R / K. */
static int eval_sr_jk(int sym, int pass, int jk)
{
    const int *s = g_cells[sym].node_state->s;
    uint8_t *state = &g_reg_state[s[2]];
    int unstable = 0;
    int edge;
    if (both_high_vsima(s[6], s[7])) {
        *state = LV_UNKNOWN;
        g_sig_out[sym] = (uint8_t)drive_output(sym, 0, (int8_t)*state, s[3], s[4], -1);
        return 0;
    }
    if (g_vsima) {
        g_reg_before[s[2]] = *state;
    }
    edge = clock_edge(s[10], s[11]);
    if (!edge) {
        if (pass != 0 && !async_controls(state, s[6], s[7], -1, &unstable)) {
            unstable = (int8_t)g_sig_unstable[s[2]];
        }
    } else if (pass > 1 && !clocked_controls(state, s[6], s[7], s[9], s[8], -1)) {
        int set = (int8_t)g_sig_value[s[0]];
        int reset = (int8_t)g_sig_value[s[1]];
        if (set == LV_LOW) {
            if (reset == LV_HIGH) {
                *state = LV_LOW;
            } else if (reset == LV_UNKNOWN && *state != LV_LOW) {
                *state = LV_UNKNOWN;
            }
        } else if (set == LV_HIGH) {
            if (reset == LV_LOW) {
                *state = LV_HIGH;
            } else if (!jk) {
                if (reset == LV_HIGH || reset == LV_UNKNOWN) {
                    *state = LV_UNKNOWN;
                }
            } else if (reset == LV_HIGH) {
                *state = (uint8_t)invert((int8_t)*state);
            } else if (reset == LV_UNKNOWN) {
                *state = (*state == LV_LOW) ? LV_HIGH : LV_UNKNOWN;
            }
        } else if (set == LV_UNKNOWN) {
            if (reset == LV_LOW) {
                if (*state != LV_HIGH) {
                    *state = LV_UNKNOWN;
                }
            } else if (!jk) {
                if (reset == LV_HIGH || reset == LV_UNKNOWN) {
                    *state = LV_UNKNOWN;
                }
            } else if (reset == LV_HIGH) {
                *state = (*state == LV_HIGH) ? LV_LOW : LV_UNKNOWN;
            } else if (reset == LV_UNKNOWN) {
                *state = LV_UNKNOWN;
            }
        }
    }
    g_sig_out[sym] = (uint8_t)drive_output(sym, 0, (int8_t)*state, s[3], s[4], -1);
    return unstable;
}

int eval_sr(int sym, int pass)
{
    check_node_type(sym, 8, 8, 8, 0x407, VTEXT("sr", "sr-type output evaluation"));
    return eval_sr_jk(sym, pass, 0);
}

int eval_jk(int sym, int pass)
{
    check_node_type(sym, 10, 10, 10, 0x407, VTEXT("jk", "jk-type output evaluation"));
    return eval_sr_jk(sym, pass, 1);
}

/* The next state of a T register: toggle if `t` is high. */
static int toggle(int t, int state)
{
    if (t == LV_HIGH) {
        if (state == LV_LOW) {
            return LV_HIGH;
        }
        if (state == LV_HIGH) {
            return LV_LOW;
        }
        return state;
    }
    if (t == LV_UNKNOWN) {
        return LV_UNKNOWN;
    }
    return state;
}

/* Polarity correction applied to the state of a T register after it
 * toggled. */
static int toggle_polarity(int sym, int state)
{
    int polarity = device_polarity();
    if (state == LV_UNKNOWN || g_vsima) {
        return state;
    }
    if ((g_cells[sym].true_out == 0 && polarity == 1) || (g_cells[sym].true_out != 0 && polarity == 2)) {
        return invert(state);
    }
    return state;
}

/* T flip-flop (node type 0x1b) */
int eval_t(int sym, int pass)
{
    const int *s = g_cells[sym].node_state->s;
    uint8_t *state = &g_reg_state[s[1]];
    int unstable = 0;
    int edge;
    int extra = g_vsima ? -1 : s[12];
    check_node_type(sym, 0x1b, 0x1b, 0x1b, 0x407, VTEXT("tf", "t-type output evaluation"));
    if (both_high_vsima(s[6], s[7])) {
        *state = LV_UNKNOWN;
        g_sig_out[sym] = (uint8_t)drive_output(sym, 1, (int8_t)*state, s[2], s[3], -1);
        return 0;
    }
    g_reg_before[s[1]] = *state;
    edge = clock_edge(s[10], s[11]);
    if (!edge) {
        if (pass != 0 && !async_controls(state, s[6], s[7], extra, &unstable)) {
            unstable = (int8_t)g_sig_unstable[s[1]];
        }
    } else if (pass > 1 && !clocked_controls(state, s[6], s[7], s[9], s[8], extra)) {
        *state = (uint8_t)toggle((int8_t)g_sig_value[s[0]], (int8_t)*state);
        *state = (uint8_t)toggle_polarity(sym, (int8_t)*state);
    }
    g_sig_out[sym] = (uint8_t)drive_output(sym, 1, (int8_t)*state, s[2], s[3], -1);
    return unstable;
}

/* latch (node type 0x1c) */
int eval_latch(int sym, int pass)
{
    const int *s = g_cells[sym].node_state->s;
    uint8_t *state = &g_reg_state[s[1]];
    int unstable = 0;
    int open;
    check_node_type(sym, 0x1c, 0x1c, 0x1c, 0x407, VTEXT("lt", "l-type output evaluation"));
    if (both_high_vsima(s[6], s[7])) {
        *state = LV_UNKNOWN;
        g_sig_out[sym] = (uint8_t)drive_output(sym, 1, (int8_t)*state, s[2], s[3], -1);
        return 0;
    }
    g_reg_before[s[1]] = *state;
    open = latch_open(s[10], s[11]);
    if (pass != 0) {
        if (!open) {
            if (!async_controls(state, s[6], s[7], s[12], &unstable)) {
                unstable = (int8_t)g_sig_unstable[s[1]];
            }
        } else if (!clocked_controls(state, s[6], s[7], s[9], s[8], g_vsima ? -1 : s[12])) {
            *state = (uint8_t)register_input(sym, s[0]);
        }
    }
    g_sig_out[sym] = (uint8_t)drive_output(sym, 1, (int8_t)*state, s[2], s[3], s[4]);
    return unstable;
}

/* internal feedback (pin type 0x26) */
int eval_internal_feedback(int sym, int pass)
{
    const int *s = g_cells[sym].pin_state->s;
    (void)pass;
    check_pin_type(sym, 0x26, 0x408, VTEXT("", "internal feedback evaluation"));
    if (g_vsima) {
        /* vsima: corrects the polarity of an inverted output */
        int node = find_feedback_node(sym);
        int polarity = device_polarity();
        int level = (int8_t)g_sig_value[s[0]];
        if (node >= 0 && g_cells[node].true_out == 0 && (polarity == 2 || polarity == 1) && dev_header()[0x30] == 0) {
            level = invert(level);
        }
        set_input_value(level, sym);
    } else {
        set_input_value((int8_t)g_sig_value[s[0]], sym);
    }
    g_sig_out[s[0]] = g_sig_value[s[0]];
    return (int8_t)g_sig_unstable[sym];
}

/* Feed the level at an I/O pin back: when the buffer is off, the level the
 * test vector applies to the pin. */
static void feed_back_pin(int sym, int pin_level)
{
    if (pin_level == LV_HIGHZ) {
        int given = vector_level_of_pin(sym, g_cells[sym].number);
        set_input_value((given == LV_HIGH || given == LV_LOW) ? given : LV_UNKNOWN, sym);
    } else {
        set_input_value(pin_level, sym);
    }
}

/* I/O pin (pin type 0x25) */
int eval_io(int sym, int pass)
{
    const int *s = g_cells[sym].pin_state->s;
    int node = find_feedback_node(sym);
    int true_out = node >= 0 ? g_cells[node].true_out : 0;
    int polarity = device_polarity();
    int value, pin_level;
    (void)pass;
    check_pin_type(sym, 0x25, 0x408, VTEXT("", "pin feedback evaluation"));
    if (node != -1 && g_cells[node].node_type != 0) {
        value = (int8_t)(g_vsima ? g_sig_out[s[0]] : g_reg_state[s[0]]);
        if (true_out == 0) {
            if (polarity == 0 || polarity == 2) {
                value = invert(value);
            }
        } else if (polarity == 2) {
            value = invert(value);
        }
    } else {
        value = (int8_t)g_sig_value[s[0]];
        if (true_out == 0) {
            value = invert(value);
        }
    }
    pin_level = buffer_output(sym, value, s[1], s[2], s[3]);
    feed_back_pin(sym, pin_level);
    g_sig_out[sym] = (uint8_t)pin_level;
    return 0;
}

/* register in the feedback path (pin types
 * 0x22 D, 0x23 latch, 0x24 T).  If the node itself is that kind of
 * register, its state is used. */
static int eval_feedback_register(int sym, int pass, int pin_type, int node_type, const char *name)
{
    int node = find_feedback_node(sym);
    const cell_t *nc = &g_cells[node >= 0 ? node : sym];
    const int *s;
    check_pin_type(sym, pin_type, 0x407, name);
    if (nc->node_type == node_type) {
        s = nc->node_state->s;
    } else {
        uint8_t *state;
        int active;
        s = g_cells[sym].pin_state->s;
        state = &g_reg_state[s[1]];
        if (pin_type == 0x22) {
            active = clock_edge(s[10], s[12]);
        } else if (pin_type == 0x23) {
            active = latch_open(s[10], s[11]);
        } else {
            active = clock_edge(s[10], s[11]);
        }
        if (!active) {
            if (pass != 0) {
                async_controls(state, s[6], s[7], pin_type == 0x24 ? s[12] : -1, NULL);
            }
        } else if (pin_type == 0x22 ? pass > 1 : pass != 0) {
            if (!clocked_controls(state, s[6], s[7], s[9], s[8], (pin_type == 0x24 && !g_vsima) ? s[12] : -1)) {
                if (pin_type == 0x24) {
                    *state = (uint8_t)toggle((int8_t)g_sig_value[s[0]], (int8_t)*state);
                    *state = (uint8_t)toggle_polarity(sym, (int8_t)*state);
                } else if (nc->true_out == 0) {
                    *state = (uint8_t)invert((int8_t)g_sig_value[s[0]]);
                } else {
                    *state = g_sig_value[s[0]];
                }
            }
        }
    }
    if (nc->true_fb == 1) {
        g_reg_q[s[1]] = g_reg_state[s[1]];
    } else {
        g_reg_q[s[1]] = (uint8_t)invert((int8_t)g_reg_state[s[1]]);
    }
    if (pin_type == 0x22 && !g_vsima && strcmp(g_device_name, "v750") == 0) {
        g_reg_q[s[1]] = (g_reg_q[s[1]] == LV_LOW) ? LV_HIGH : LV_LOW;
    }
    set_input_from_reg(s[1], sym);
    if (pin_type == 0x22) {
        g_sig_out[sym] = LV_LOW;
    }
    return 0;
}

int eval_feedback_d(int sym, int pass)
{
    return eval_feedback_register(sym, pass, 0x22, 1, VTEXT("dfb", "d-type feedback evaluation"));
}

int eval_feedback_latch(int sym, int pass)
{
    return eval_feedback_register(sym, pass, 0x23, 0x1c, VTEXT("lfb", "l-type feedback evaluation"));
}

int eval_feedback_t(int sym, int pass)
{
    return eval_feedback_register(sym, pass, 0x24, 0x1b, VTEXT("tfb", "t-type feedback evaluation"));
}

/* counter stage of the f507 (pin type 0x2e; csima only) */
int eval_counter(int sym, int pass)
{
    const int *s = g_cells[sym].pin_state->s;
    uint8_t *state = &g_reg_state[s[0]];
    uint8_t before = g_reg_prev[s[0]];
    (void)pass;
    check_pin_type(sym, 0x2e, 0x407, "cnt2");
    if (!clock_edge(s[5], s[6])) {
        *state = before;
    } else if (is_high(s[1]) || (s[2] >= 0 && g_reg_prev[s[2]] == LV_HIGH)) {
        *state = LV_LOW;
    } else if (is_high(s[3]) || (s[4] >= 0 && g_reg_prev[s[4]] == LV_HIGH)) {
        /* held: the state is left as it is */
    } else {
        int carry = 1;
        for (int i = s[8] - 1; i >= 0; i--) {
            if (g_reg_prev[g_counter_cells[i]] == LV_LOW) {
                carry = 0;
                break;
            }
        }
        if (!carry) {
            *state = before;
        } else if (before == LV_LOW) {
            *state = LV_HIGH;
        } else if (before == LV_HIGH) {
            *state = LV_LOW;
        } else {
            fatal_error(0x407, "cnt_q");
        }
    }
    g_reg_q[s[0]] = *state;
    set_input_from_reg(s[0], sym);
    return 0;
}

/* The value an I/O register loads at its clock edge. */
static int io_register_input(int sym, const int *s, int state)
{
    int polarity = device_polarity();
    if (buffer_output(sym, state, s[2], s[3], s[4]) == LV_HIGHZ) {
        return g_vec_level[s[0]] == 0 ? LV_UNKNOWN : (int8_t)g_vec_level[s[0]];
    }
    if (g_vsima) {
        /* vsima */
        int v = (int8_t)((int8_t)g_reg_before[s[0]] < 1 ? g_sig_value[s[0]] : g_reg_before[s[0]]);
        return g_cells[s[0]].true_out == 0 ? invert(v) : v;
    }
    if ((int8_t)g_reg_before[s[0]] < 1) {
        return (int8_t)g_sig_out[s[0]];
    }
    if (g_cells[s[0]].true_out == 0) {
        int v = (int8_t)g_reg_before[s[1]];
        return (polarity == 0 || polarity == 2) ? invert(v) : v;
    }
    return polarity == 2 ? (int8_t)g_reg_before[s[0]] : invert((int8_t)g_reg_before[s[0]]);
}

/* I/O pin with D register (pin type 0x33) or latch
 * (pin type 0x34) */
static int eval_io_register(int sym, int pass, int latch)
{
    const int *s = g_cells[sym].pin_state->s;
    int active = 0;
    int value, pin_level;
    if (both_high_vsima(s[5], s[6])) {
        value = LV_UNKNOWN;
    } else if (g_dev_is_7c33x && is_high(s[5]) && is_high(s[6])) {
        value = register_input(sym, s[0]);
    } else {
        value = (int8_t)g_reg_state[s[1]];
        active = latch ? latch_open(s[9], s[10]) : clock_edge(s[9], s[10]);
        if (!active) {
            if (pass != 0) {
                if (is_high(s[5])) {
                    value = LV_LOW;
                } else if (is_high(s[6])) {
                    value = LV_HIGH;
                }
            }
        } else if (pass > 1) {
            if (is_high(s[5])) {
                value = LV_LOW;
            } else if (is_high(s[6])) {
                value = LV_HIGH;
            } else if (is_high(s[8])) {
                value = LV_HIGH;
            } else if (is_high(s[7])) {
                value = LV_LOW;
            } else if (is_high(s[11])) {
                value = LV_LOW;
            } else {
                value = io_register_input(sym, s, value);
            }
        }
    }
    if (g_vsima) {
        /* vsima has no output buffer here */
        set_input_value(value, sym);
        g_reg_state[s[1]] = (uint8_t)value;
        g_sig_out[sym] = (uint8_t)value;
        return 0;
    }
    pin_level = buffer_output(sym, value, s[2], s[3], s[4]);
    if (pin_level == LV_HIGHZ) {
        int given = vector_level_of_pin(sym, g_cells[sym].number);
        if (given != LV_HIGH && given != LV_LOW) {
            set_input_value(LV_UNKNOWN, sym);
        } else if (!active || pass == 0) {
            set_input_value((int8_t)g_reg_state[s[1]], sym);
        } else {
            set_input_value(given, sym);
            g_reg_state[s[1]] = (uint8_t)given;
        }
    } else {
        set_input_value(pin_level, sym);
        g_reg_state[s[1]] = (uint8_t)pin_level;
    }
    g_sig_out[sym] = (uint8_t)pin_level;
    return 0;
}

int eval_io_d(int sym, int pass)
{
    check_pin_type(sym, 0x33, 0x407, VTEXT("iod", "iod feedback evaluation"));
    return eval_io_register(sym, pass, 0);
}

int eval_io_latch(int sym, int pass)
{
    check_pin_type(sym, 0x34, 0x407, VTEXT("iol", "iol feedback evaluation"));
    return eval_io_register(sym, pass, 1);
}

/* I/O pin with T register (pin type 0x35) */
int eval_io_t(int sym, int pass)
{
    const int *s = g_cells[sym].pin_state->s;
    int value;
    check_pin_type(sym, 0x35, 0x407, VTEXT("iot", "iot feedback evaluation"));
    if (both_high_vsima(s[5], s[6])) {
        value = LV_UNKNOWN;
    } else if (g_dev_is_7c33x && is_high(s[5]) && is_high(s[6])) {
        value = register_input(sym, s[0]);
    } else {
        int edge;
        value = (int8_t)g_reg_state[s[1]];
        edge = clock_edge(s[9], s[10]);
        if (!edge) {
            if (pass != 0) {
                if (is_high(s[5])) {
                    value = LV_LOW;
                } else if (is_high(s[6])) {
                    value = LV_HIGH;
                }
            }
        } else if (pass > 1) {
            int t = (int8_t)g_sig_value[s[0]];
            if (g_vsima && (int8_t)g_reg_before[s[0]] >= 1) {
                t = (int8_t)g_reg_before[s[0]];
            }
            if (g_cells[s[0]].true_out == 0) {
                t = invert(t);
            }
            t = buffer_output(sym, t, s[2], s[3], s[4]);
            if (t == LV_HIGHZ) {
                t = (int8_t)g_vec_level[s[0]];
            }
            value = t;
            if (t == LV_LOW) {
                if (g_reg_state[s[1]] == LV_HIGH || g_reg_state[s[1]] == LV_UNKNOWN) {
                    value = (int8_t)g_reg_state[s[1]];
                }
            } else if (t == LV_HIGH) {
                value = toggle(LV_HIGH, (int8_t)g_reg_state[s[1]]);
                if (value != LV_LOW && value != LV_HIGH && value != LV_UNKNOWN) {
                    value = LV_HIGH;
                }
            }
        }
    }
    set_input_value(value, sym);
    g_reg_state[s[1]] = (uint8_t)value;
    g_sig_out[sym] = (uint8_t)value;
    return 0;
}

/* vsima: pin type 0x0c has no model */
int eval_none(int sym, int pass)
{
    (void)pass;
    check_pin_type(sym, 0x0c, 0x407, "flip-flop mode evaluation");
    return 0;
}
