/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, LinCUPL team
 *
 * Decode ATF150x routing and macrocells from their programmed fuse map.
 * The fitter's immutable package/routing tables describe the wiring;
 * all logic and register controls below are reconstructed from fuses.
 */
#include "sim.h"
#include "sim_seq.h"
#include <ctype.h>
#include "../fit150x/dev.h"

enum { J_MAX_NODES = 70000, J_MAX_DEPTH = 2048, J_MAX_EVENTS = 256 };
enum { J_CONST, J_WIRE, J_NOT, J_AND, J_OR, J_XOR, J_PAD, J_STATE, J_INVALID };
enum {
    C_OE0, C_OE1, C_OE2, C_OUTPUT, C_DATA, C_CLOCK, C_CASCADE,
    C_GCLK0, C_LOW_POWER, C_PT1, C_XOR_B, C_INVERT,
    C_PT4, C_PT2, C_PT5, C_CLEAR, C_PT3, C_PRESET,
    C_FEEDBACK, C_GCLK1, C_OPEN_DRAIN, C_PT_POWER, C_SLEW, C_STORAGE
};

typedef struct {
    int op, a, b;
    uint8_t value, color, used;
} jnode_t;

typedef struct {
    uint8_t cfg[24], q, previous_clock, used, toggle, invalid_storage;
    int pt[5], sum, cascade, foldback, feedback, logic;
    int state, data, clock, enable, clear, preset, output, oe, pin;
} jmacro_t;

typedef struct {
    const FitDeviceDesc *device;
    const FitPackageDesc *package;
    const uint8_t *fuses;
    uint8_t config[32], input_used[257], output_used[257], drive[257], keeper[257];
    uint8_t pd_pin[257], held_pins[257], held_nodes[257];
    uint8_t termination[257]; /* 0 none, 1 keeper, 2 pull-up, 3 pull-down */
    int power_down, be, jtag;
    int pin_sym[257], pin_mc[257], pad[257];
    int uim[8][40], goe[6], gclk[3], gclr, nnodes, sequential, evaluation_failed;
    jmacro_t mc[128];
    jnode_t *nodes;
} jdevice_t;

static jdevice_t *chip;

int jed150x_active(void) { return chip != NULL; }
void jed150x_reset(void) { chip = NULL; }

static void bad(const char *detail)
{
    fatal_error(ERR_JEDEC_DEVICE, detail);
}

static int node(int op, int a, int b)
{
    if (chip->nnodes == J_MAX_NODES) bad("ATF150x logic graph is too large");
    int n = chip->nnodes++;
    chip->nodes[n].op = op;
    chip->nodes[n].a = a;
    chip->nodes[n].b = b;
    return n;
}

static int invert(int a)
{
    if (a < 2) return 1 - a;
    if (chip->nodes[a].op == J_NOT) return chip->nodes[a].a;
    return node(J_NOT, a, 0);
}

static int gate(int op, int a, int b)
{
    if (op == J_AND) {
        if (!a || !b) return 0;
        if (a == 1) return b;
        if (b == 1 || a == b) return a;
    } else if (op == J_OR) {
        if (a == 1 || b == 1) return 1;
        if (!a) return b;
        if (!b || a == b) return a;
    } else {
        if (a == b) return 0;
        if (!a) return b;
        if (!b) return a;
        if (a == 1) return invert(b);
        if (b == 1) return invert(a);
    }
    return node(op, a, b);
}

static void wire(int n, int target)
{
    chip->nodes[n].op = J_WIRE;
    chip->nodes[n].a = target;
}

static int source(const char *name)
{
    const FitPackageDesc *p = chip->package;
    int mc;
    if (!strcmp(name, "GND")) return 0;
    if (!strcmp(name, "GCLK")) return chip->pad[p->gclk_pins[0]];
    if (!strcmp(name, "GCLR")) return chip->pad[p->gclr_pin];
    if (!strcmp(name, "OE1")) return chip->pad[p->oe_pins[0]];
    if (!strcmp(name, "OE2")) return chip->pad[p->oe_pins[1]];
    if (name[0] >= 'A' && name[0] <= 'H' && (mc = atoi(name + 1)) > 0 &&
        mc <= chip->device->params.n_macrocells) {
        const char *suffix = name + 1;
        while (*suffix >= '0' && *suffix <= '9') suffix++;
        if (!strcmp(suffix, "fb")) return chip->mc[mc - 1].feedback;
        if (!strcmp(suffix, "p")) {
            int pin = chip->mc[mc - 1].pin;
            return pin ? chip->pad[pin] : node(J_INVALID, 0, 0);
        }
    }
    return node(J_INVALID, 0, 0);
}

static int selection(int address, int reverse, const char *const *choices)
{
    int chosen = -1, width = chip->device->params.uim_width;
    for (int i = 0; i < width; i++) {
        if (!chip->fuses[address + i]) {
            if (chosen >= 0) return node(J_INVALID, address, 0);
            chosen = reverse ? width - 1 - i : i;
        }
    }
    /* An erased switch disconnects all sources and supplies logic low. */
    return chosen < 0 ? 0 : source(choices[chosen]);
}

static void configuration(void)
{
    static const int groups[6][4] = {
        {16, 12, 14, 11}, {9, 6, 13, 10}, {20, 18, 8, 21},
        {7, 19, 22, 5}, {23, 4, 3, 15}, {0, 1, 17, 2}
    };
    int blocks = chip->device->params.n_blocks;
    int small = chip->device->id == FIT_DEV_1502;
    if (chip->be) {
        const uint8_t *f = chip->fuses;
        int base = chip->device->jedec->cfg_bits_addr;
        memset(chip->config, 1, sizeof(chip->config));
        for (int i = 0; i < blocks * 16; i++) {
            jmacro_t *m = &chip->mc[i];
            int start = fit_be_macrocell_base(chip->device->id, i);
            for (int k = 0; k < 24; k++)
                m->cfg[k] = fit_be_cfg_offsets[k] < 0 ? 0 : f[start + fit_be_cfg_offsets[k]];
            if (!small) {
                m->toggle = f[start + 8] && !f[start + 9];
                m->invalid_storage = f[start + 8] && f[start + 9];
                if (m->toggle) m->cfg[C_STORAGE] = 1;
            }
            if (m->pin) {
                int term = f[start] + 2 * f[start + 1];
                chip->termination[m->pin] = term == 3 ? 1 : term == 1 ? 2 : 0;
                if (term == 2) bad("ATF150x BE pin selects an invalid termination");
            }
        }
        for (int i = 0; i < 6; i++)
            chip->config[27 - (small ? 5 - i : i)] = f[base + 1 + i];
        chip->config[21] = f[base + 7];
        for (int i = 0; i < 3; i++) {
            int c = 18 - i * 3, b = base + 8 + i * 3;
            chip->config[c + 2] = f[b];
            chip->config[c + 1] = f[b + 1];
            chip->config[c] = f[b + 2];
        }
        chip->config[29] = f[base + 34];
        chip->config[28] = f[base + 35];
        chip->jtag = f[base + 33];
        int inputs[4] = {chip->package->gclk_pins[0], chip->package->gclk_pins[1],
                         chip->package->oe_pins[0], chip->package->gclr_pin};
        for (int i = 0; i < 4; i++) {
            int term = f[base + 23 + i * 2] + 2 * f[base + 24 + i * 2];
            chip->termination[inputs[i]] = term == 3 ? 1 : term == 1 ? 3 : term == 2 ? 2 : 0;
        }
        return;
    }
    for (int half = 0; half < 2; half++) {
        int address = chip->device->jedec->cfg_half_addr[half];
        for (int group = 0; group < 6; group++) {
            for (int k = 0; k < blocks / 2; k++) {
                int block = half ? blocks - k - 1 : k;
                address += 16;
                for (int pair = 0; pair < 2; pair++) {
                    int backwards = pair ^ small;
                    for (int n = 0; n < 16; n++) {
                        int local = backwards ? 15 - n : n;
                        int swap = backwards ? !(local & 1) : local & 1;
                        uint8_t *c = chip->mc[block * 16 + local].cfg;
                        c[groups[group][pair * 2 + swap]] = chip->fuses[address++];
                        c[groups[group][pair * 2 + 1 - swap]] = chip->fuses[address++];
                    }
                }
            }
        }
    }
    for (int i = 0; i < 32; i++)
        chip->config[i] = chip->fuses[chip->device->jedec->cfg_bits_addr + 31 - i];
    chip->jtag = chip->fuses[chip->device->jedec->jtag_bits_addr + 1];
    for (int p = 1; p <= chip->package->n_pins; p++)
        if (chip->pin_mc[p] >= 0 && !chip->config[30]) chip->termination[p] = 1;
}

static void routing(void)
{
    const FitDeviceDesc *d = chip->device;
    int blocks = d->params.n_blocks, width = d->params.uim_width;
    for (int pair = 0; pair < blocks / 2; pair++) {
        int a = pair, b = blocks - pair - 1;
        int address = d->jedec->uim_addr[pair];
        if (d->id == FIT_DEV_1502) {
            for (int i = 39; i >= 0; i--) {
                chip->uim[a][i] = selection(address, 0, d->uim_sources + i * width);
                address += width;
                chip->uim[b][i] = selection(address, 0, d->uim_sources + i * width);
                address += width;
            }
        } else {
            for (int i = 0; i < 40; i += 2) {
                int targets[4][2] = {{a, i}, {b, i + 1}, {b, i}, {a, i + 1}};
                for (int k = 0; k < 4; k++) {
                    int index = targets[k][1];
                    chip->uim[targets[k][0]][index] = selection(address,
                        d->id == FIT_DEV_1508, d->uim_sources + index * width);
                    address += width;
                }
            }
        }
    }
    for (int i = 0; i < 6; i++) {
        int index = d->id == FIT_DEV_1502 ? 5 - i : i;
        int s = selection(d->jedec->global_oe_addr + i * width, 0,
                          d->oe_sources + index * width);
        chip->goe[index] = chip->config[27 - index] ? s : invert(s);
    }
    for (int i = 0; i < 3; i++) {
        int offset = 18 - i * 3;
        int value = chip->config[offset] * 2 + chip->config[offset + 1];
        int index = value == 3 ? 0 : value == 1 ? 1 : value == 0 ? 2 : -1;
        int pin = index < 0 ? 0 : chip->package->gclk_pins[index];
        int s = pin ? chip->pad[pin] : node(J_INVALID, 0, 0);
        chip->gclk[i] = chip->config[offset + 2] ? invert(s) : s;
    }
    chip->gclr = chip->pad[chip->package->gclr_pin];
    if (!chip->config[21]) chip->gclr = invert(chip->gclr);
}

static int product(int mc, int term)
{
    const FitDeviceDesc *d = chip->device;
    int down = !(mc & 1);
    if (d->id != FIT_DEV_1502) down = !down;
    int address = d->jedec->pt_addr[mc] + (down ? 4 - term : term) * 96;
    const uint8_t *f = chip->fuses + address;
    if (chip->mc[mc].cfg[C_PT_POWER]) {
        for (int i = 0; i < 96; i++)
            if (f[i]) return node(J_INVALID, address, 0);
        return 0;
    }
    /* Contradictory literal pairs force an unused row low, independently
     * of its otherwise unconfigured input switches. */
    for (int i = 0; i < 40; i++)
        if (!f[16 + i * 2] && !f[17 + i * 2]) return 0;
    int result = 1;
    for (int i = 0; i < 16; i++)
        if (!f[i]) result = gate(J_AND, result, chip->mc[(mc / 16) * 16 + i].foldback);
    for (int i = 0; i < 40; i++) {
        int positive = 16 + i * 2;
        if (d->id == FIT_DEV_1502 && (i & 1)) positive ^= 1;
        int s = chip->uim[mc / 16][i];
        if (!f[positive]) result = gate(J_AND, result, s);
        if (!f[positive ^ 1]) result = gate(J_AND, result, invert(s));
    }
    return result;
}

static void macrocell(int index)
{
    jmacro_t *m = &chip->mc[index];
    const uint8_t *c = m->cfg;
    static const int selects[5] = {C_PT1, C_PT2, C_PT3, C_PT4, C_PT5};
    int sum = index % 8 ? chip->mc[index - 1].cascade : 0;
    for (int i = 0; i < 5; i++) {
        m->pt[i] = product(index, i);
        if (!c[selects[i]]) sum = gate(J_OR, sum, m->pt[i]);
    }
    wire(m->sum, sum);
    wire(m->cascade, c[C_CASCADE] ? m->sum : 0);
    int y1 = c[C_PT1] ? m->pt[0] : 1;
    int y2 = c[C_PT2] ? m->pt[1] : 1;
    int rare = c[C_CASCADE] && !c[C_XOR_B] && c[C_INVERT];
    wire(m->foldback, rare ? 1 : c[C_PT1] ? invert(m->pt[0]) : 1);
    int a = c[C_CASCADE] ? y2 : m->sum;
    int b = c[C_XOR_B] ? invert(m->state) :
            !c[C_INVERT] ? 1 : c[C_CASCADE] ? y1 : y2;
    int logic = gate(J_XOR, a, b);
    if (!c[C_INVERT]) logic = invert(logic);
    wire(m->logic, logic);
    m->data = c[C_DATA] ? m->logic : c[C_OUTPUT] ? m->pt[1] :
              m->pin ? chip->pad[m->pin] : node(J_INVALID, 0, 0);
    m->output = c[C_OUTPUT] ? m->logic : m->state;
    wire(m->feedback, c[C_FEEDBACK] ? m->logic : m->state);
    m->clear = gate(J_OR, c[C_PT3] ? m->pt[2] : 0, c[C_CLEAR] ? 0 : chip->gclr);
    m->preset = c[C_PT5] && c[C_PRESET] ? m->pt[4] : 0;
    int clock = c[C_GCLK1] ? (c[C_GCLK0] ? 0 : 2) : 1;
    m->clock = c[C_CLOCK] ? (c[C_PT4] ? m->pt[3] : 1) :
               (!c[C_GCLK1] && c[C_GCLK0]) ? node(J_INVALID, 0, 0) : chip->gclk[clock];
    m->enable = !c[C_CLOCK] && c[C_PT4] ? m->pt[3] : 1;
    int oe = c[C_OE0] * 4 + c[C_OE1] * 2 + c[C_OE2];
    m->oe = !oe ? 0 : oe < 7 ? chip->goe[oe - 1] : c[C_PRESET] ? 1 : m->pt[4];
    if (oe == 7 && !c[C_PRESET] && !c[C_PT5]) m->oe = node(J_INVALID, 0, 0);
}

static void reachable(int n, int depth)
{
    jnode_t *v = &chip->nodes[n];
    if (v->used) return;
    if (depth > J_MAX_DEPTH) bad("ATF150x combinational routing is too deep");
    v->used = 1;
    switch (v->op) {
    case J_INVALID: bad("ATF150x active routing selects an invalid or unbonded source"); break;
    case J_PAD:
        if (chip->pd_pin[v->a])
            bad("ATF150x power-down pin is also routed as a logic input");
        chip->input_used[v->a] = 1;
        break;
    case J_STATE: {
        jmacro_t *m = &chip->mc[v->a];
        if (m->invalid_storage) bad("ATF150x BE macrocell selects an invalid storage mode");
        m->used = chip->sequential = 1;
        reachable(m->data, depth + 1);
        reachable(m->clock, depth + 1);
        reachable(m->enable, depth + 1);
        reachable(m->clear, depth + 1);
        reachable(m->preset, depth + 1);
        break;
    }
    case J_CONST: break;
    default:
        reachable(v->a, depth + 1);
        if (v->op != J_WIRE && v->op != J_NOT) reachable(v->b, depth + 1);
    }
}

static void device_view(void)
{
    int count = chip->package->n_pins;
    size_t header = DL_SECTIONS * DL_SECTION_HEADER_SIZE;
    size_t pins = header + 152, size = pins + count * DEV_PIN_REC_SIZE;
    uint8_t *record = alloc_tracked(size, "ATF150x device");
    wr32(record + DEV_SEC_HEADER * DL_SECTION_HEADER_SIZE + 8, (uint32_t)header);
    wr16(record + DEV_SEC_HEADER * DL_SECTION_HEADER_SIZE + 4, 1);
    wr32(record + DEV_SEC_PINS * DL_SECTION_HEADER_SIZE + 8, (uint32_t)pins);
    wr16(record + DEV_SEC_PINS * DL_SECTION_HEADER_SIZE + 4, (uint16_t)count);
    record[header + 8] = (uint8_t)count;
    wr16(record + header + 0x19, chip->sequential ? 1 : 0);
    for (int p = 1; p <= count; p++) {
        uint8_t *pin = record + pins + (p - 1) * DEV_PIN_REC_SIZE;
        wr16(pin + 2, (uint16_t)p);
        if (chip->pad[p]) pin[5] = chip->pin_mc[p] >= 0 ? 16 : 12;
    }
    use_device_image(record, size);
}

static const FitDeviceDesc *device_for_fuses(unsigned nfuses)
{
    for (int i = 0; i < 3; i++) {
        const FitJedecLayout *be = fit_be_jedec_layout(g_dev_descs[i].id);
        if ((unsigned)g_dev_descs[i].jedec->n_fuses == nfuses ||
            (be && (unsigned)be->n_fuses == nfuses))
            return &g_dev_descs[i];
    }
    return NULL;
}

static const FitPackageDesc *package_for_name(const FitDeviceDesc *d, const char *name)
{
    if (!d || !name || !*name) return NULL;
    for (int i = 0; i < d->n_packages; i++) {
        char canonical[80];
        const FitPackageDesc *p = &d->packages[i];
        snprintf(canonical, sizeof(canonical), "f%.4s%s", d->jedec->part + 3, p->name);
        if (!strcasecmp(name, canonical) || !strcasecmp(name, p->dev_name)) return p;
        snprintf(canonical, sizeof(canonical), "f%.4sisp%s", d->jedec->part + 3, p->name);
        if (!strcasecmp(name, canonical)) return p;
    }
    return NULL;
}

int jed150x_name_matches(const char *name, unsigned nfuses, unsigned pins)
{
    const FitPackageDesc *p = package_for_name(device_for_fuses(nfuses), name);
    return p && (!pins || pins == (unsigned)p->n_pins);
}

int jed150x_same_package(const char *name)
{
    return !*name || package_for_name(chip->device, name) == chip->package;
}

int jed150x_ball_pin(const char *name)
{
    if (!chip || !chip->package->balls) return 0;
    if (!strncasecmp(name, "BALL_", 5)) name += 5;
    else if ((name[0] == 'P' || name[0] == 'p') && isdigit((unsigned char)name[1]))
        return 0; /* P<n> remains the numeric JEDEC vector position. */
    for (int p = 1; p <= chip->package->n_pins; p++)
        if (!strcasecmp(name, chip->package->balls[p - 1].name)) return p;
    return 0;
}

int jed150x_load(const uint8_t *fuses, unsigned nfuses, unsigned pins,
                const char *name, const char *description)
{
    const FitDeviceDesc *d = device_for_fuses(nfuses);
    const FitPackageDesc *p = NULL;
    if (!d) return 0;
    int be = nfuses != (unsigned)d->jedec->n_fuses;
    char be_name[16];
    snprintf(be_name, sizeof(be_name), "ATF%.4sBE", d->jedec->part + 3);
    for (const char *s = description; s && *s; s++)
        if (!strncasecmp(s, be_name, strlen(be_name)) && !be)
            bad("ATF150x BE-series description disagrees with QF fuse count");
    char family[8];
    snprintf(family, sizeof(family), "%.4s", d->jedec->part + 3);
    if (name && *name) {
        if (!strstr(name, family)) bad("ATF150x device name disagrees with QF fuse count");
        p = package_for_name(d, name);
        if (!p) bad("unknown ATF150x package; select an f1502/f1504/f1508 package mnemonic");
    } else {
        int candidates = 0;
        for (int i = 0; i < d->n_packages; i++)
            if ((unsigned)d->packages[i].n_pins == pins) { p = &d->packages[i]; candidates++; }
        if (candidates != 1) bad("ATF150x package is ambiguous; specify the device explicitly");
    }
    if (pins && pins != (unsigned)p->n_pins) bad("ATF150x package disagrees with QP pin count");
    if (!be && p->id == FIT_PKG_CBGA132)
        bad("ATF1508 CBGA132 requires a BE-series fuse map");
    chip = alloc_tracked(sizeof(*chip), "ATF150x decoder");
    chip->nodes = alloc_tracked(J_MAX_NODES * sizeof(*chip->nodes), "ATF150x logic");
    chip->device = d; chip->package = p; chip->fuses = fuses; chip->be = be;
    node(J_CONST, LV_LOW, 0); node(J_CONST, LV_HIGH, 0);
    for (int pin = 0; pin <= 256; pin++) chip->pin_mc[pin] = chip->pin_sym[pin] = -1;
    for (int i = 0; i < d->params.n_macrocells + d->params.n_ded_inputs; i++) {
        int pin = p->rows[i].pin;
        if (pin) chip->pad[pin] = node(J_PAD, pin, 0);
        if (i < d->params.n_macrocells) {
            jmacro_t *m = &chip->mc[i];
            m->pin = pin;
            if (pin) chip->pin_mc[pin] = i;
            m->sum = node(J_WIRE, 0, 0);
            m->cascade = node(J_WIRE, 0, 0);
            m->foldback = node(J_WIRE, 0, 0);
            m->feedback = node(J_WIRE, 0, 0);
            m->logic = node(J_WIRE, 0, 0);
            m->state = node(J_STATE, i, 0);
        }
    }
    configuration();
    for (int i = 0; i < 2; i++) {
        if (chip->config[29 - i]) continue;
        int pin = p->pd_pins[i];
        if (!pin || !chip->pad[pin]) bad("ATF150x power-down pin is not bonded");
        chip->pd_pin[pin] = chip->input_used[pin] = 1;
        chip->power_down = chip->sequential = 1;
    }
    routing();
    for (int i = 0; i < d->params.n_macrocells; i++) macrocell(i);
    for (int i = 0; i < d->params.n_macrocells; i++) {
        jmacro_t *m = &chip->mc[i];
        int jtag = 0;
        if (chip->jtag)
            for (int k = 0; k < 4; k++) if (m->pin == p->jtag_pins[k]) jtag = 1;
        if (m->pin && m->oe != 0 && !jtag && !chip->pd_pin[m->pin]) {
            chip->output_used[m->pin] = 1;
            reachable(m->output, 0);
            reachable(m->oe, 0);
        }
    }
    snprintf(g_device_name, sizeof(g_device_name), "f%s%s", family, p->name);
    for (char *s = g_device_name; *s; s++) *s = (char)tolower((unsigned char)*s);
    device_view();
    return 1;
}

void jed150x_symbols(void)
{
    for (int i = 2; i < g_nsyms; i++) {
        int p = g_syms[i].pin;
        if (p < 1 || p > chip->package->n_pins) continue;
        chip->pin_sym[p] = i;
        g_syms[i].flags &= 0x10;
        if (chip->output_used[p]) g_syms[i].flags |= 0x40;
        if (chip->input_used[p]) g_syms[i].flags |= 0x80;
    }
}

void jed150x_bind(void)
{
    for (int i = 0; i < g_nsyms; i++) {
        cell_t *c = &g_cells[i];
        int p = g_syms[i].pin;
        c->number = (uint16_t)p;
        c->clock_enable = -1;
        c->true_out = c->true_fb = 1;
        c->node_type = -1;
        c->bound = 1;
        c->kind = p > 0 && chip->output_used[p] ? 3 : 1;
        g_feedback_sym[i] = (uint16_t)i;
    }
}

static void clear_cache(void);
static void observe(uint8_t *pins, uint8_t *nodes);

void jed150x_powerup(void)
{
    if (!chip) return;
    memset(chip->keeper, LV_UNKNOWN, sizeof(chip->keeper));
    memset(chip->drive, LV_HIGHZ, sizeof(chip->drive));
    for (int i = 0; i < chip->device->params.n_macrocells; i++) {
        chip->mc[i].q = chip->mc[i].cfg[C_INVERT] ? LV_HIGH : LV_LOW;
        chip->mc[i].previous_clock = LV_UNKNOWN;
    }
    clear_cache();
    observe(chip->held_pins, chip->held_nodes);
}

static int not_level(int a)
{
    return a == LV_LOW ? LV_HIGH : a == LV_HIGH ? LV_LOW : LV_UNKNOWN;
}

static int value(int id, int depth);

static int output_value(jmacro_t *m, int depth)
{
    int enable = value(m->oe, depth + 1);
    if (enable == LV_LOW) return LV_HIGHZ;
    if (enable != LV_HIGH) return LV_UNKNOWN;
    int result = value(m->output, depth + 1);
    return m->cfg[C_OPEN_DRAIN] && result == LV_HIGH ? LV_HIGHZ : result;
}

static int value(int id, int depth)
{
    jnode_t *n = &chip->nodes[id];
    int a, b, result = LV_UNKNOWN;
    if (n->color == 2) return n->value;
    if (n->color == 1) return LV_UNKNOWN;
    if (depth > J_MAX_DEPTH) {
        chip->evaluation_failed = 1;
        return LV_UNKNOWN;
    }
    n->color = 1;
    switch (n->op) {
    case J_CONST: result = n->a; break;
    case J_STATE: result = chip->mc[n->a].q; break;
    case J_PAD: {
        int p = n->a, external = chip->drive[p];
        int mc = chip->pin_mc[p];
        result = mc >= 0 && chip->output_used[p] ?
                 output_value(&chip->mc[mc], depth + 1) : LV_HIGHZ;
        if (result == LV_HIGHZ) {
            result = external;
            if (result == LV_HIGHZ)
                result = chip->termination[p] == 1 ? chip->keeper[p] :
                         chip->termination[p] == 2 ? LV_HIGH :
                         chip->termination[p] == 3 ? LV_LOW : LV_UNKNOWN;
        }
        else if ((external == LV_LOW || external == LV_HIGH) && external != result)
            result = LV_UNKNOWN;
        break;
    }
    case J_WIRE: result = value(n->a, depth + 1); break;
    case J_NOT: result = not_level(value(n->a, depth + 1)); break;
    case J_AND:
    case J_OR:
    case J_XOR:
        a = value(n->a, depth + 1);
        b = value(n->b, depth + 1);
        if (n->op == J_AND) {
            if (a == LV_LOW || b == LV_LOW) result = LV_LOW;
            else if (a == LV_HIGH && b == LV_HIGH) result = LV_HIGH;
        } else if (n->op == J_OR) {
            if (a == LV_HIGH || b == LV_HIGH) result = LV_HIGH;
            else if (a == LV_LOW && b == LV_LOW) result = LV_LOW;
        } else if ((a == LV_LOW || a == LV_HIGH) && (b == LV_LOW || b == LV_HIGH))
            result = a == b ? LV_LOW : LV_HIGH;
        break;
    default: break;
    }
    n->value = (uint8_t)result;
    n->color = 2;
    return result;
}

static void clear_cache(void)
{
    for (int i = 0; i < chip->nnodes; i++) chip->nodes[i].color = 0;
}

/* Snapshot all D/clock/control values before changing any register.
 * Derived clocks are then reevaluated until no further event is pending. */
static int advance(int pass)
{
    uint8_t next_state[128];
    chip->evaluation_failed = 0;
    for (int event = 0; event < J_MAX_EVENTS; event++) {
        int changed = 0;
        clear_cache();
        for (int i = 0; i < chip->device->params.n_macrocells; i++) {
            jmacro_t *m = &chip->mc[i];
            int result = m->q;
            if (m->used) {
                int reset = value(m->clear, 0), preset = value(m->preset, 0);
                int clock = value(m->clock, 0), enable = value(m->enable, 0);
                int data = value(m->data, 0);
                if (m->toggle)
                    data = data == LV_LOW ? m->q : data == LV_HIGH ? not_level(m->q) : LV_UNKNOWN;
                if (reset == LV_HIGH && preset == LV_LOW) result = LV_LOW;
                else if (preset == LV_HIGH && reset == LV_LOW) result = LV_HIGH;
                else if (reset != LV_LOW || preset != LV_LOW) result = LV_UNKNOWN;
                else if (!m->cfg[C_STORAGE]) {
                    if (clock == LV_HIGH && enable == LV_HIGH) result = data;
                    else if (clock != LV_LOW && enable != LV_LOW && data != result)
                        result = LV_UNKNOWN;
                } else if (pass > 1 && clock != LV_LOW && m->previous_clock != LV_HIGH) {
                    if (clock == LV_HIGH && m->previous_clock == LV_LOW && enable == LV_HIGH)
                        result = data;
                    else if (enable != LV_LOW && data != result) result = LV_UNKNOWN;
                }
                if (pass > 1 || m->previous_clock == LV_UNKNOWN)
                    m->previous_clock = (uint8_t)clock;
            }
            next_state[i] = (uint8_t)result;
            if (result != m->q) changed = 1;
        }
        for (int i = 0; i < chip->device->params.n_macrocells; i++) chip->mc[i].q = next_state[i];
        if (!changed) break;
        if (event == J_MAX_EVENTS - 1) return 0;
    }
    clear_cache();
    return !chip->evaluation_failed;
}

static void observe(uint8_t *pins, uint8_t *nodes)
{
    for (int p = 1; p <= chip->package->n_pins; p++) {
        int mc = chip->pin_mc[p];
        int result = chip->pad[p] ? value(chip->pad[p], 0) : LV_UNKNOWN;
        if (result == LV_LOW || result == LV_HIGH) chip->keeper[p] = (uint8_t)result;
        nodes[p] = (uint8_t)result;
        if (chip->output_used[p]) {
            jmacro_t *m = &chip->mc[mc];
            nodes[p] = (uint8_t)value(m->output, 0);
            result = output_value(m, 0);
        }
        pins[p] = (uint8_t)result;
    }
}

/* Power-down blocks every ordinary input and retains both the output
 * value and its enable. Restoring inputs on wake-up lets the normal
 * event engine see transitions from the last active clock levels. */
static int transition(const uint8_t *drive, int pass, uint8_t *pins, uint8_t *nodes)
{
    int pd = LV_LOW;
    for (int p = 1; p <= chip->package->n_pins; p++) {
        if (!chip->pd_pin[p]) continue;
        if (drive[p] == LV_HIGH) { pd = LV_HIGH; break; }
        if (drive[p] != LV_LOW) pd = LV_UNKNOWN;
    }
    if (pd != LV_LOW) {
        if (pd == LV_UNKNOWN) {
            for (int i = 0; i < chip->device->params.n_macrocells; i++) {
                chip->mc[i].q = LV_UNKNOWN;
                chip->mc[i].previous_clock = LV_UNKNOWN;
            }
            memset(chip->held_pins, LV_UNKNOWN, sizeof(chip->held_pins));
            memset(chip->held_nodes, LV_UNKNOWN, sizeof(chip->held_nodes));
        }
        memcpy(pins, chip->held_pins, sizeof(chip->held_pins));
        memcpy(nodes, chip->held_nodes, sizeof(chip->held_nodes));
        for (int p = 1; p <= chip->package->n_pins; p++) {
            if (!chip->pad[p]) continue;
            /* Pin hold circuits remain active independently of the
             * sleeping logic. Preserve externally driven keeper levels
             * for the next active evaluation, including tri-state I/O. */
            int level = chip->output_used[p] ? chip->held_pins[p] : LV_HIGHZ;
            if (level == LV_HIGHZ) {
                level = drive[p];
                if (level == LV_HIGHZ)
                    level = chip->termination[p] == 1 ? chip->keeper[p] :
                            chip->termination[p] == 2 ? LV_HIGH :
                            chip->termination[p] == 3 ? LV_LOW : LV_UNKNOWN;
            } else if ((drive[p] == LV_LOW || drive[p] == LV_HIGH) && drive[p] != level) {
                level = LV_UNKNOWN;
            }
            if (level == LV_LOW || level == LV_HIGH) chip->keeper[p] = (uint8_t)level;
            if (!chip->output_used[p]) pins[p] = nodes[p] = (uint8_t)level;
        }
        return 1;
    }
    memcpy(chip->drive, drive, sizeof(chip->drive));
    if (!advance(pass)) return 0;
    observe(pins, nodes);
    memcpy(chip->held_pins, pins, sizeof(chip->held_pins));
    memcpy(chip->held_nodes, nodes, sizeof(chip->held_nodes));
    return !chip->evaluation_failed;
}

void jed150x_step(int pass)
{
    uint8_t pins[257] = {0}, nodes[257] = {0}, drive[257];
    if (!chip) return;
    memset(drive, LV_HIGHZ, sizeof(drive));
    for (int i = 0; i < g_value_count; i++) {
        const order_t *o = &g_order[i];
        int p = o->sym->pin;
        if (p < 1 || p > chip->package->n_pins) continue;
        if (pass == 0 && chip->pin_mc[p] >= 0 && chip->mc[chip->pin_mc[p]].used) {
            jmacro_t *m = &chip->mc[chip->pin_mc[p]];
            if (o->value == T_VAL_0 || o->value == T_VAL_1 || o->value == T_VAL_X)
                m->q = o->value == T_VAL_X ? LV_UNKNOWN :
                       o->value == T_VAL_1 ? LV_HIGH : LV_LOW;
            continue;
        }
        if (pass == 0 && chip->output_used[p] &&
            (o->value == T_VAL_0 || o->value == T_VAL_1))
            bad("ATF150x preload value targets a macrocell without active storage");
        if (o->value == T_VAL_C || o->value == T_VAL_K)
            drive[p] = g_sig_value[o->sym_index];
        else if (o->value == T_VAL_0 || o->value == T_VAL_1 || o->value == T_VAL_R)
            drive[p] = g_vec_level[o->sym_index];
        else if (o->value == T_VAL_X)
            drive[p] = LV_UNKNOWN;
    }
    if (pass == 0) {
        /* P loads storage directly. It neither clocks registers nor
         * applies asynchronous controls. Establish clock history after
         * all loads so a changed buried state cannot create a pulse. */
        memcpy(chip->drive, drive, sizeof(chip->drive));
        clear_cache();
        for (int i = 0; i < chip->device->params.n_macrocells; i++)
            if (chip->mc[i].used)
                chip->mc[i].previous_clock = (uint8_t)value(chip->mc[i].clock, 0);
        observe(pins, nodes);
        memcpy(chip->held_pins, pins, sizeof(chip->held_pins));
        memcpy(chip->held_nodes, nodes, sizeof(chip->held_nodes));
    } else if (!transition(drive, pass, pins, nodes)) {
        bad("ATF150x register/control feedback did not settle");
    }
    if (chip->evaluation_failed) bad("ATF150x combinational routing is too deep");
    for (int p = 1; p <= chip->package->n_pins; p++) {
        int sym = chip->pin_sym[p];
        if (sym < 0) continue;
        g_node_level[sym] = nodes[p];
        g_sig_out[sym] = chip->output_used[p] ? pins[p] : LV_NONE;
        g_sig_value[sym] = pins[p];
    }
}

int jed150x_sequential(void) { return chip && chip->sequential; }

void jed150x_truth_pins(sim_truth_t *table)
{
    table->npins = (unsigned)chip->package->n_pins;
    snprintf(table->device, sizeof(table->device), "%s", g_device_name);
    for (unsigned p = 1; p <= table->npins; p++) {
        if (chip->output_used[p]) {
            table->role[p] = SIM_PIN_OUTPUT;
            table->outputs[table->noutputs++] = p;
        } else if (chip->pad[p]) {
            table->role[p] = SIM_PIN_INPUT;
            if (chip->input_used[p]) table->inputs[table->ninputs++] = p;
        }
    }
}

/* Retain a decoded graph after the compiler phase releases its transient
 * allocations. Graph evaluation and state changes use the same engine as
 * csim; traversal of reachable paired states lives in jedcompare. */
struct sim_seq_model {
    jdevice_t device;
    sim_truth_t pins;
};

sim_seq_model_t *jed150x_seq_copy(char *error, size_t size)
{
    for (int p = 1; p <= chip->package->n_pins; p++) {
        if (chip->input_used[p] && chip->output_used[p]) {
            snprintf(error, size, "P%d uses physical output feedback; sequential comparison requires driven inputs", p);
            return NULL;
        }
    }
    sim_seq_model_t *model = calloc(1, sizeof(*model));
    if (!model) {
        snprintf(error, size, "out of memory for sequential model");
        return NULL;
    }
    model->device = *chip;
    size_t bytes = (size_t)chip->nnodes * sizeof(*chip->nodes);
    model->device.nodes = malloc(bytes);
    if (!model->device.nodes) {
        free(model);
        snprintf(error, size, "out of memory for sequential logic graph");
        return NULL;
    }
    memcpy(model->device.nodes, chip->nodes, bytes);
    model->device.fuses = NULL;
    jed150x_truth_pins(&model->pins);
    return model;
}

const sim_truth_t *sim_seq_pins(const sim_seq_model_t *model)
{
    return &model->pins;
}

size_t sim_seq_state_size(const sim_seq_model_t *model)
{
    size_t size = 1;  /* whether initial input settling has happened */
    for (int i = 0; i < model->device.device->params.n_macrocells; i++)
        if (model->device.mc[i].used) size += 2;
    if (model->device.power_down)
        for (int p = 1; p <= model->device.package->n_pins; p++)
            if (model->device.output_used[p]) size += 2;
    return size;
}

static void save_state(unsigned char *state, int initialized)
{
    *state++ = (unsigned char)initialized;
    for (int i = 0; i < chip->device->params.n_macrocells; i++) {
        if (!chip->mc[i].used) continue;
        *state++ = chip->mc[i].q;
        *state++ = chip->mc[i].previous_clock;
    }
    if (chip->power_down)
        for (int p = 1; p <= chip->package->n_pins; p++) {
            if (!chip->output_used[p]) continue;
            *state++ = chip->held_pins[p];
            *state++ = chip->held_nodes[p];
        }
}

void sim_seq_initial(sim_seq_model_t *model, unsigned char *state)
{
    jdevice_t *saved = chip;
    chip = &model->device;
    jed150x_powerup();
    save_state(state, 0);
    chip = saved;
}

int sim_seq_step(sim_seq_model_t *model, const unsigned char *state,
                 const unsigned char *inputs, unsigned char *next,
                 unsigned char *outputs, char *error, size_t size)
{
    jdevice_t *saved = chip;
    int success = 0;
    state++;  /* startup marker */
    uint8_t pins[257] = {0}, nodes[257] = {0}, drive[257];
    chip = &model->device;
    for (int i = 0; i < chip->device->params.n_macrocells; i++) {
        if (!chip->mc[i].used) continue;
        chip->mc[i].q = *state++;
        chip->mc[i].previous_clock = *state++;
    }
    if (chip->power_down)
        for (int p = 1; p <= chip->package->n_pins; p++) {
            if (!chip->output_used[p]) continue;
            chip->held_pins[p] = *state++;
            chip->held_nodes[p] = *state++;
        }
    memset(drive, LV_HIGHZ, sizeof(drive));
    memset(chip->keeper, LV_UNKNOWN, sizeof(chip->keeper));
    for (unsigned i = 0; i < model->pins.ninputs; i++) {
        unsigned p = model->pins.inputs[i];
        if (inputs[p] > 1) {
            snprintf(error, size, "P%u requires a binary input", p);
            goto done;
        }
        drive[p] = inputs[p] ? LV_HIGH : LV_LOW;
    }
    if (!transition(drive, 1, pins, nodes) || !transition(drive, 2, pins, nodes)) {
        snprintf(error, size, "register/control feedback did not settle");
        goto done;
    }
    if (chip->evaluation_failed) {
        snprintf(error, size, "combinational routing is too deep");
        goto done;
    }
    memset(outputs, 0, SIM_TRUTH_PINS);
    for (unsigned i = 0; i < model->pins.noutputs; i++) {
        unsigned p = model->pins.outputs[i];
        switch (pins[p]) {
        case LV_LOW: outputs[p] = '0'; break;
        case LV_HIGH: outputs[p] = '1'; break;
        case LV_HIGHZ: outputs[p] = 'Z'; break;
        default:
            snprintf(error, size, "P%u is indeterminate in a reachable state", p);
            goto done;
        }
    }
    save_state(next, 1);
    success = 1;
done:
    chip = saved;
    return success;
}

void sim_seq_free(sim_seq_model_t *model)
{
    if (model) {
        free(model->device.nodes);
        free(model);
    }
}
