/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, LinCUPL team
 *
 * Stateful access to the same decoded arrays and cells used by csim.
 * Each model owns its allocation pool. Switching models only switches
 * immutable bindings and working buffers; every mutable evaluation byte
 * is restored from the supplied state before running a vector.
 */
#include "sim.h"
#include "seq.h"
#include "phase.h"

#define BYTE_ARRAYS(X) \
    X(g_sig_hiz) X(g_sig_value) X(g_sig_out) X(g_reg_state) \
    X(g_reg_prev) X(g_reg_q) X(g_sig_unstable) X(g_sig_prev) \
    X(g_pin_reg_input) X(g_vec_level) X(g_sig_is_clock) \
    X(g_sig_saved) X(g_reg_before) X(g_node_level) X(g_bypass_sym)
#define WORD_ARRAYS(X) \
    X(g_feedback_sym) X(g_fb_undefined) X(g_fb_symbols) \
    X(g_term_outputs) X(g_in_unknown) X(g_in_valid) X(g_in_high)
#define POINTERS(X) \
    BYTE_ARRAYS(X) WORD_ARRAYS(X) X(g_row_value) X(g_row_stable) \
    X(g_stuck) X(g_syms) X(g_terms) X(g_cells) X(g_order) X(g_dev_rec)
#define SCALARS(X) \
    X(g_nsyms) X(g_nsyms_file) X(g_nterms) X(g_total_rows) \
    X(g_row_words) X(g_plane_words) X(g_dev_has_class) \
    X(g_dev_is_isp1016) X(g_dev_is_7c33x) X(g_dev_is_78c800) \
    X(g_dev_is_7c330) X(g_order_count) X(g_value_count)

#define MAX_BLOCKS 24
#define MAX_STORAGE 256

typedef struct {
    phase_pool_t pool;
    sim_truth_t pins;
    unsigned char *initial;
    size_t state_size, image_size;
    struct { void *data; size_t size; } blocks[MAX_BLOCKS];
    unsigned nblocks, nstorage;
    struct { unsigned number, sym; size_t offset; } storage[MAX_STORAGE];
#define DECL_BYTE(n) uint8_t *n;
    BYTE_ARRAYS(DECL_BYTE)
#undef DECL_BYTE
#define DECL_WORD(n) uint16_t *n;
    WORD_ARRAYS(DECL_WORD)
#undef DECL_WORD
    uint8_t *g_row_value, *g_row_stable, *g_stuck, *g_dev_rec;
    sym_t *g_syms;
    term_entry_t *g_terms;
    cell_t *g_cells;
    order_t *g_order;
#define DECL_SCALAR(n) int n;
    SCALARS(DECL_SCALAR)
#undef DECL_SCALAR
    char device[28];
} legacy_seq_t;

static void activate(const legacy_seq_t *m)
{
    jed150x_reset();
#define RESTORE(n) n = m->n;
    POINTERS(RESTORE)
    SCALARS(RESTORE)
#undef RESTORE
    memcpy(g_device_name, m->device, sizeof(g_device_name));
    use_device_image(m->g_dev_rec, m->image_size);
    g_vsima = 0;
    g_options = OPT_FUSEMAP;
    g_sim_on = 1;
    g_trace_level = 0;
    g_vector_number = 0;
}

static void state_copy(legacy_seq_t *m, unsigned char *state, int save)
{
    size_t at = 1;
    for (unsigned i = 0; i < m->nblocks; i++) {
        if (save) memcpy(state + at, m->blocks[i].data, m->blocks[i].size);
        else memcpy(m->blocks[i].data, state + at, m->blocks[i].size);
        at += m->blocks[i].size;
    }
}

static void block(legacy_seq_t *m, void *data, size_t size)
{
    m->blocks[m->nblocks].data = data;
    m->blocks[m->nblocks++].size = size;
    m->state_size += size;
}

static const sim_truth_t *legacy_pins(const void *context)
{ return &((const legacy_seq_t *)context)->pins; }
static size_t legacy_size(const void *context)
{ return ((const legacy_seq_t *)context)->state_size; }
static void legacy_initial(void *context, unsigned char *state)
{
    legacy_seq_t *m = context;
    memcpy(state, m->initial, m->state_size);
}

static int legacy_step(void *context, const unsigned char *state,
                        const unsigned char *inputs, unsigned char *next,
                        unsigned char *outputs, char *error, size_t size)
{
    legacy_seq_t *m = context;
    activate(m);
    state_copy(m, (unsigned char *)state, 0);
    for (unsigned i = 0; i < m->pins.ninputs; i++) {
        unsigned p = m->pins.inputs[i];
        if (inputs[p] > 1) {
            snprintf(error, size, "P%u requires a binary input", p);
            return 0;
        }
        g_order[i].value = inputs[p] ? T_VAL_1 : T_VAL_0;
    }
    if (!sim_seq_vector(error, size)) return 0;
    memset(outputs, 0, SIM_TRUTH_PINS);
    for (unsigned i = 0; i < m->pins.noutputs; i++) {
        unsigned p = m->pins.outputs[i];
        int sym = g_order[m->pins.ninputs + i].sym_index;
        switch (g_sig_out[sym]) {
        case LV_LOW: outputs[p] = '0'; break;
        case LV_HIGH: outputs[p] = '1'; break;
        case LV_HIGHZ: outputs[p] = 'Z'; break;
        default:
            snprintf(error, size, "P%u is indeterminate in a reachable state", p);
            return 0;
        }
    }
    next[0] = 1;
    state_copy(m, next, 1);
    return 1;
}

static void legacy_free(void *context)
{
    legacy_seq_t *m = context;
    phase_pool_free_all(&m->pool);
    free(m->initial);
    free(m);
}
static unsigned legacy_storage_count(const void *context)
{ return ((const legacy_seq_t *)context)->nstorage; }
static int legacy_storage(const void *context, unsigned i, char *name,
                            size_t size, unsigned char *powerup)
{
    const legacy_seq_t *m = context;
    if (i >= m->nstorage) return 0;
    snprintf(name, size, "R%u", m->storage[i].number);
    int level = m->initial[m->storage[i].offset];
    *powerup = level == LV_LOW ? '0' : level == LV_HIGH ? '1' : 'X';
    return 1;
}
static int legacy_preload(void *context, unsigned char *state,
                            unsigned i, unsigned value)
{
    legacy_seq_t *m = context;
    if (state[0] || i >= m->nstorage) return 0;
    activate(m);
    state_copy(m, state, 0);
    unsigned sym = m->storage[i].sym;
    g_reg_state[sym] = g_reg_prev[sym] = g_reg_before[sym] = value ? LV_HIGH : LV_LOW;
    state_copy(m, state, 1);
    return 1;
}
static const sim_seq_ops_t legacy_ops = {
    legacy_pins, legacy_size, legacy_initial, legacy_step, legacy_free,
    legacy_storage_count, legacy_storage, legacy_preload
};

static int add_order(unsigned pin, int token)
{
    for (int i = 0; i < g_nsyms; i++) {
        if (g_syms[i].kind != 2 || g_syms[i].pin != pin) continue;
        sym_t *sym = find_symbol(g_syms[i].name);
        order_t *o = &g_order[g_order_count++];
        o->sym_index = (int16_t)(sym - g_syms);
        o->sym = sym;
        o->pin = (uint16_t)pin;
        o->width = 1;
        o->positive = 1;
        o->value = (int16_t)token;
        return 1;
    }
    return 0;
}

sim_seq_model_t *sim_legacy_seq_copy(char *error, size_t size)
{
    g_order = alloc_tracked(((size_t)g_nsyms + 2) * sizeof(*g_order), "sequential model");
    legacy_seq_t *m = calloc(1, sizeof(*m));
    if (!m) goto memory;
    sim_seq_model_t *result = sim_seq_create(m, &legacy_ops);
    if (!result) { free(m); goto memory; }
    jedec_truth_pins(&m->pins);
    /* Dedicated clock pins may have no product-term column in use. */
    for (int i = 0; i < g_nsyms; i++) {
        unsigned p = g_syms[i].pin;
        if (!g_sig_is_clock[i] || !p || p > m->pins.npins ||
            m->pins.role[p] != SIM_PIN_INPUT) continue;
        unsigned j;
        for (j = 0; j < m->pins.ninputs; j++) if (m->pins.inputs[j] == p) break;
        if (j == m->pins.ninputs) m->pins.inputs[m->pins.ninputs++] = p;
    }
    g_order_count = 0;
    for (unsigned i = 0; i < m->pins.ninputs; i++)
        if (!add_order(m->pins.inputs[i], T_VAL_0)) goto pin_error;
    for (unsigned i = 0; i < m->pins.noutputs; i++)
        if (!add_order(m->pins.outputs[i], T_VAL_STAR)) goto pin_error;
    g_value_count = g_order_count;
    /* Unknown clock history suppresses an artificial edge in the first
     * vector, while asynchronous controls still settle normally. */
    memset(g_sig_saved, LV_UNKNOWN, g_nsyms);
    memset(g_sig_prev, LV_UNKNOWN, g_nsyms);
    jedec_design_bound();
#define CAPTURE(n) m->n = n;
    POINTERS(CAPTURE)
    SCALARS(CAPTURE)
#undef CAPTURE
    memcpy(m->device, g_device_name, sizeof(m->device));
    m->image_size = device_image_size();
    m->state_size = 1;
#define ADD_BYTE(n) block(m, n, g_nsyms);
    BYTE_ARRAYS(ADD_BYTE)
#undef ADD_BYTE
    block(m, g_row_value, g_total_rows);
    block(m, g_row_stable, g_total_rows);
    block(m, g_stuck, g_total_rows);
    block(m, g_in_unknown, ((size_t)g_row_words + 16) * sizeof(uint16_t));
    block(m, g_fb_undefined, ((size_t)g_plane_words + 8) * sizeof(uint16_t));
    block(m, g_fb_symbols, ((size_t)g_plane_words + 8) * sizeof(uint16_t));
    block(m, g_term_outputs, ((size_t)g_plane_words + 8) * sizeof(uint16_t));
    for (int i = 0; i < g_nsyms; i++) {
        const cell_t *c = &g_cells[i];
        if (!c->is_node || !c->node_state ||
            (c->node_type != 1 && c->node_type != 0x1b &&
             c->node_type != 0x15 && c->node_type != 0x16)) continue;
        unsigned sym = (unsigned)c->node_state->s[1];
        if (sym >= g_nsyms) {
            snprintf(error, size, "invalid register storage binding");
            sim_seq_free(result);
            return NULL;
        }
        unsigned j;
        for (j = 0; j < m->nstorage; j++) if (m->storage[j].sym == sym) break;
        if (j < m->nstorage) continue;
        if (m->nstorage == MAX_STORAGE) {
            snprintf(error, size, "too many register storage elements");
            sim_seq_free(result);
            return NULL;
        }
        m->storage[j].number = g_syms[i].pin;
        m->storage[j].sym = sym;
        size_t offset = 1;
        for (unsigned b = 0; b < m->nblocks; b++) {
            if (m->blocks[b].data == g_reg_state) break;
            offset += m->blocks[b].size;
        }
        m->storage[j].offset = offset + sym;
        m->nstorage++;
    }
    m->initial = malloc(m->state_size);
    if (!m->initial) { sim_seq_free(result); goto memory; }
    m->initial[0] = 0;
    state_copy(m, m->initial, 1);
    sim_seq_take_allocations(&m->pool);
    return result;
pin_error:
    snprintf(error, size, "physical pin has no simulation symbol");
    sim_seq_free(result);
    return NULL;
memory:
    snprintf(error, size, "out of memory for sequential model");
    return NULL;
}
