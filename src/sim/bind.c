/*
 * sim_bind - binding of the design's symbols to the macrocell models
 * of the device: for every pin and node the model (cell type) is taken from
 * the device library, and the symbols that act as its clock, resets, presets
 * and output enable are looked up.
 *
 * Part of the CUPL simulator (see sim.c).
 */
#include "sim.h"

/* Roles of a signal towards a macrocell (the cell type codes of the device's
 * node records, also used in its connection table). */
enum {
    ROLE_OE_PIN = 2,
    ROLE_SP = 3,            /* synchronous preset */
    ROLE_AP = 5,            /* asynchronous preset */
    ROLE_SR = 6,            /* synchronous reset */
    ROLE_AR = 7,            /* asynchronous reset */
    ROLE_CLOCK = 0x0f,
    ROLE_OE = 0x11,         /* output enable product term */
    ROLE_AP_ALT = 0x12,
    ROLE_14 = 0x14,
    ROLE_1A = 0x1a,
    ROLE_LATCH = 0x1d,      /* latch enable */
    ROLE_LATCH_INV = 0x1e,
    ROLE_20 = 0x20,
    ROLE_CLOCK_INV = 0x27,
    ROLE_2F = 0x2f
};

/* Connection table of the device (section 10, 13-byte records) */
enum { DEV_SEC_CONNECTIONS = 10, CONN_SIZE = 13 };

/* Search state of the connection lookups */
typedef struct {
    int cur;            /* connection found for the wanted role */
    int own;            /* the cell's own connection entry */
    uint8_t type;       /* cell type (record byte +4) */
    uint16_t number;    /* cell number (record word +2) */
    uint16_t term;      /* record word +0 */
    uint16_t group;     /* connection word +7 of the own entry */
    uint16_t member;    /* connection word +9 of the own entry */
} conn_search_t;

static conn_search_t g_search;
int g_counter_cells[6];             /* the six counter cells of the f507 */

static const uint8_t *conn_rec(int i)
{
    static const uint8_t k_empty[CONN_SIZE];
    if (i < 0 || i >= dev_section_count(DEV_SEC_CONNECTIONS)) {
        return k_empty;
    }
    return dev_section(DEV_SEC_CONNECTIONS) + (size_t)i * CONN_SIZE;
}

/* Start a search from the symbol's node or pin record. */
static void search_from(const uint8_t *rec)
{
    g_search.cur = -1;
    g_search.own = -1;
    g_search.group = 0xffff;
    g_search.type = rec[4];
    g_search.number = (uint16_t)rd16(rec + 2);
    g_search.term = (uint16_t)rd16(rec);
}

/* Next connection entry describing the cell itself. */
static int next_own_connection(void)
{
    int count = dev_section_count(DEV_SEC_CONNECTIONS);
    for (int i = g_search.own < 0 ? 0 : g_search.own + 1; i < count; i++) {
        const uint8_t *c = conn_rec(i);
        if (g_search.number == rd16(c + 2) && g_search.type == c[5] && g_search.term == rd16(c)) {
            g_search.own = i;
            g_search.member = (uint16_t)rd16(c + 9);
            g_search.group = (uint16_t)rd16(c + 7);
            return i;
        }
    }
    return -1;
}

/* Next entry of the same group with the wanted role; one with the cell's
 * own number is preferred. */
static int next_role_connection(int role)
{
    int count = dev_section_count(DEV_SEC_CONNECTIONS);
    int first = -1;
    for (int i = g_search.cur < 0 ? 0 : g_search.cur + 1; i < count; i++) {
        const uint8_t *c = conn_rec(i);
        if (g_search.group == rd16(c + 7) && g_search.member != rd16(c + 9) && c[5] == role) {
            if (g_search.number == rd16(c + 2)) {
                g_search.cur = i;
                return i;
            }
            if (first < 0) {
                first = i;
            }
        }
    }
    if (first >= 0) {
        g_search.cur = first;
    }
    return first;
}

/* Same, for a given cell number. */
static int next_role_connection_numbered(int role, unsigned number)
{
    int count = dev_section_count(DEV_SEC_CONNECTIONS);
    for (int i = g_search.cur < 0 ? 0 : g_search.cur + 1; i < count; i++) {
        const uint8_t *c = conn_rec(i);
        if (g_search.group == rd16(c + 7) && g_search.member != rd16(c + 9) && rd16(c + 2) == number &&
            c[5] == role) {
            g_search.cur = i;
            return i;
        }
    }
    return -1;
}

/* The symbol bound to the node the found connection names. */
static int symbol_of_node_connection(int sym)
{
    const cell_t *self = &g_cells[sym];
    const uint8_t *c;
    if (!self->bound || g_search.cur < 0) {
        return -1;
    }
    c = conn_rec(g_search.cur);
    for (int i = 0; i < g_nsyms; i++) {
        const cell_t *other = &g_cells[i];
        if (other != self && other->bound && other->node_type == c[5] && other->number == rd16(c + 2) &&
            other->term == rd16(c)) {
            return i;
        }
    }
    /* Shared reset/preset rows have one node record per destination pin.
     * The equation may be attached to any of them; their array identifies
     * the physical control term even when this pin has no own equation. */
    if (c[5] == ROLE_AR || c[5] == ROLE_SP) {
        for (int i = 0; i < g_nsyms; i++) {
            const cell_t *other = &g_cells[i];
            if (other != self && other->bound && other->node_type == c[5] &&
                other->term == rd16(c)) {
                return i;
            }
        }
    }
    return -1;
}

/* The symbol bound to the pin the found connection names. */
static int symbol_of_pin_connection(int sym)
{
    const cell_t *self = &g_cells[sym];
    unsigned number;
    if (!self->bound || g_search.cur < 0) {
        return -1;
    }
    number = rd16(conn_rec(g_search.cur) + 2);
    for (int i = 0; i < g_nsyms; i++) {
        if (g_cells[i].bound && &g_cells[i] != self && g_cells[i].number == number) {
            return i;
        }
    }
    return -1;
}

static int symbol_of_connection(int sym)
{
    if (conn_rec(g_search.cur)[10] == 1) {
        return symbol_of_pin_connection(sym);
    }
    return symbol_of_node_connection(sym);
}

/* Clock and latch enable roles have a second entry for the other polarity. */
static int role_has_second_entry(int role)
{
    return role == ROLE_CLOCK || role == ROLE_CLOCK_INV || role == ROLE_LATCH || role == ROLE_LATCH_INV;
}

static int is_pin_register_type(int t, int with_35)
{
    return t == 0x17 || t == 0x18 || t == 0x33 || t == 0x34 || (with_35 && t == 0x35);
}

/* Common start of the lookups: choose the record the search starts from.
 * Returns 0 if the symbol is neither a pin nor a node. */
static int search_from_symbol(int sym, int with_35)
{
    const sym_t *s = &g_syms[sym];
    if (s->node_rec < 0 && s->pin_rec < 0) {
        return 0;
    }
    if (s->pin_rec >= 0 && is_pin_register_type(dev_pin(s->pin_rec)[4], with_35)) {
        search_from(dev_pin(s->pin_rec));
    } else if (s->node_rec >= 0) {
        search_from(dev_node(s->node_rec));
    } else {
        search_from(dev_pin(s->pin_rec));
    }
    return 1;
}

/* The symbol that plays `role` for the cell of `sym`, -1 if none. */
static int find_related_vsima(int sym, int role);

static int find_related(int sym, int role)
{
    int found;
    if (g_vsima) {
        return find_related_vsima(sym, role);
    }
    if (!search_from_symbol(sym, 0)) {
        return -1;
    }
    do {
        if (next_own_connection() < 0) {
            return -1;
        }
    } while (next_role_connection(role) < 0);
    found = symbol_of_connection(sym);
    if (role_has_second_entry(role) && found == -1) {
        g_search.cur++;
        found = symbol_of_connection(sym);
    }
    return found;
}

/* Like find_related, but the second connection with that role. */
static int find_related_second(int sym, int role)
{
    const sym_t *s = &g_syms[sym];
    int found;
    if (s->node_rec < 0 && s->pin_rec < 0) {
        return -1;
    }
    if (s->node_rec >= 0) {
        search_from(dev_node(s->node_rec));
    } else {
        search_from(dev_pin(s->pin_rec));
    }
    do {
        if (next_own_connection() < 0) {
            break;
        }
    } while (next_role_connection(role) < 0);
    g_search.cur++;
    do {
        if (next_own_connection() < 0) {
            return -1;
        }
    } while (next_role_connection(role) < 0);
    found = symbol_of_connection(sym);
    if (role_has_second_entry(role) && found == -1) {
        g_search.cur++;
        found = symbol_of_connection(sym);
    }
    return found;
}

/* Like find_related, starting from the pin record only. */
static int find_related_of_pin(int sym, int role)
{
    const sym_t *s = &g_syms[sym];
    int found;
    if (g_vsima) {
        return find_related_vsima(sym, role);
    }
    if (s->pin_rec < 0) {
        return -1;
    }
    search_from(dev_pin(s->pin_rec));
    do {
        if (next_own_connection() < 0) {
            return -1;
        }
    } while (next_role_connection(role) < 0);
    found = symbol_of_connection(sym);
    if (role == 0x3a && found == -1) {     /* I/O clock: try the second entry */
        g_search.cur++;
        found = symbol_of_connection(sym);
    }
    return found;
}

/* The symbol with pin `number` that plays `role` for the cell. */
static int find_related_pin(int sym, int role, unsigned number)
{
    if (!search_from_symbol(sym, 1)) {
        return -1;
    }
    do {
        if (next_own_connection() < 0) {
            return -1;
        }
    } while (next_role_connection_numbered(role, number) < 0);
    return symbol_of_connection(sym);
}

/* -------------------------------------------------------------------------
 * vsima: devices without macrocell description
 *
 * vsima does not use the device's node records and connection table.
 * The model of a symbol is named by its extension (x.d, x.ck, x.ar ...), and
 * the signals that control a cell are the symbols with the same name and
 * the matching extension.
 * ------------------------------------------------------------------------- */
typedef struct {
    const char *name;
    int code;
} name_code_t;

/* cell type / role for a symbol's extension */
static const name_code_t k_extension_types[] = {
    { "", 0x00 }, { "d", 0x01 }, { "l", 0x1c }, { "j", 0x0a }, { "k", 0x0b }, { "s", 0x08 },
    { "r", 0x09 }, { "t", 0x1b }, { "dq", 0x17 }, { "lq", 0x18 }, { "ap", 0x05 }, { "ar", 0x07 },
    { "sp", 0x03 }, { "sr", 0x06 }, { "ck", 0x0f }, { "oe", 0x02 }, { "ca", 0x0d }, { "pr", 0x10 },
    { "ce", 0x1f }, { "le", 0x1d }, { "obs", 0x20 }, { "byp", 0x13 }, { "dfb", 0x22 },
    { "lfb", 0x23 }, { "tfb", 0x24 }, { "io", 0x25 }, { "int", 0x26 }, { "ckmux", 0x2a },
    { "oemux", 0x2b }, { "tec", 0x2d }, { "imux", 0x30 }, { "t1", 0x31 }, { "t2", 0x32 },
    { "iod", 0x33 }, { "iol", 0x34 }, { "iot", 0x35 }, { "iock", 0x3a }, { "ioar", 0x36 },
    { "ioap", 0x37 }, { "iosr", 0x38 }, { "iosp", 0x39 }, { "armux", 0x3e }, { "apmux", 0x3f },
    { "lemux", 0x2c }, { NULL, 0 }
};

/* the global signals */
static const name_code_t k_global_signals[] = {
    { "_CLOCK", 5 }, { "_GND", 2 }, { "_VCC", 3 }, { "_VEE", 4 }, { "_COE", 9 }, { NULL, 0 }
};

/* Code of `name` in a name table (case-insensitive), 0 if not found. */
static int lookup_name_code(const char *name, const name_code_t *table)
{
    if (name[0] != '\0') {
        for (; table->name != NULL; table++) {
            if (strcasecmp(table->name, name) == 0) {
                return table->code;
            }
        }
    }
    return 0;
}

int sym_type_code(const sym_t *s)
{
    return lookup_name_code(s->type, k_extension_types);
}

/* Give a symbol used as a control input a pin record number of its own. */
static void assign_pin_rec(sym_t *s)
{
    if (s->pin_rec < 0) {
        s->pin_rec = (int16_t)g_next_pin_rec++;
    }
}

/* The symbol that plays `role` for the cell of `sym`: the one with the same
 * name and that extension, else a global signal. */
static int find_related_vsima(int sym, int role)
{
    const char *global = NULL;
    int wanted;
    for (int i = 0; i < g_nsyms; i++) {
        if (i != sym && strcmp(g_syms[sym].name, g_syms[i].name) == 0 && sym_type_code(&g_syms[i]) == role) {
            return i;
        }
    }
    if (role == ROLE_CLOCK || role == 0x3a) {
        wanted = 5;
    } else if (role == ROLE_OE) {
        wanted = 9;
    } else {
        return -1;
    }
    for (const name_code_t *g = k_global_signals; g->name != NULL; g++) {
        if (g->code == wanted) {
            global = g->name;
            break;
        }
    }
    if (global != NULL) {
        for (int i = 0; i < g_nsyms; i++) {
            sym_t *s = &g_syms[i];
            if ((s->flags & 0x80) && strcasecmp(global, s->name) == 0) {
                assign_pin_rec(s);
                if (g_cells[i].kind == 0) {
                    g_cells[i].kind = 1;
                }
                return i;
            }
        }
    }
    return -1;
}

static int same_name_node(const sym_t *other, const sym_t *self)
{
    return strcmp(other->name, self->name) == 0 && other->node_rec >= 0 && (other->flags & 0x40);
}

/* A node symbol with the same name and the given extension. */
static int find_node_of_type_vsima(int sym, int type, int self_is_pin)
{
    const sym_t *self = &g_syms[sym];
    if ((self_is_pin ? self->pin_rec : self->node_rec) >= 0) {
        for (int i = 0; i < g_nsyms; i++) {
            if (same_name_node(&g_syms[i], self) && sym_type_code(&g_syms[i]) == type) {
                return i;
            }
        }
    }
    return -1;
}

/* Does the design give the signal of symbol `sym` an equation (sym itself
 * or one of its extensions)? */
static int has_equation(int sym)
{
    for (int i = 0; i < g_nsyms; i++) {
        if ((g_syms[i].flags & 0x40) && strcmp(g_syms[i].name, g_syms[sym].name) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Follow a clock that is a node with a single-input product term back to
 * the pin that drives it. */
static void trace_clock_vsima(int *clock, int *polarity)
{
    sym_t *s = &g_syms[*clock];
    if (s->flags & 0x80) {
        *polarity = (strcmp(s->name, "_CLOCK") == 0 && (s->flags & 0x10)) ? ROLE_CLOCK_INV : ROLE_CLOCK;
        if (s->pin == 0) {
            s->pin = 1;
        }
        return;
    }
    {
        const frame_t *f = (s->node_rec >= 0 && s->node_rec < g_nterms) ? g_terms[s->node_rec].frame : NULL;
        const uint16_t *used, *inverted = NULL;
        int plane_words, found = 0, count = 0;
        sym_t *t;
        if (f == NULL || f->rows == NULL) {
            return;
        }
        plane_words = f->row_words / 3;
        used = f->rows + plane_words;
        for (int row = 0; row < f->nrows; row++, used += f->row_words) {
            for (int i = 0; i < g_nsyms; i++) {
                if (bit_test(used + 1, i)) {
                    inverted = used + plane_words;
                    found = i;
                    if (++count > 1) {
                        return;
                    }
                }
            }
        }
        if (inverted == NULL) {
            return;     /* the term has no input */
        }
        t = &g_syms[found];
        if (has_equation(found)) {
            /* The clock is an output of the design, e.g. a register
             * clocking the next one of a ripple counter.  Its pin symbol
             * is never driven; the clock term itself follows it.  The
             * original took the pin and never clocked the register. */
            return;
        }
        if (t->pin == 0) {
            t->pin = 1;
        } else if (!g_dev_has_class) {
            return;
        }
        assign_pin_rec(t);
        *polarity = bit_test(inverted + 1, found) ? ROLE_CLOCK : ROLE_CLOCK_INV;
        *clock = found;
    }
}

/* -------------------------------------------------------------------------
 * Lookups among the symbols
 * ------------------------------------------------------------------------- */
static int node_term(const sym_t *s)
{
    return (int)rd16(dev_node(s->node_rec));
}

static int same_pin_node(const sym_t *other, const sym_t *self)
{
    return other->pin == self->pin && other->node_rec >= 0 && (other->flags & 0x40);
}

/* The node symbol on the same pin and output whose node record is marked
 * as the one fed back. */
int find_feedback_node(int sym)
{
    const sym_t *self = &g_syms[sym];
    if (g_vsima) {
        /* vsima: the node with the same name that has a model */
        for (int i = 0; i < g_nsyms; i++) {
            if (same_name_node(&g_syms[i], self) && cell_types()[g_cells[i].node_type & 0x7f].init != NULL) {
                return i;
            }
        }
        return -1;
    }
    if (self->node_rec >= 0) {
        for (int i = 0; i < g_nsyms; i++) {
            const sym_t *s = &g_syms[i];
            if (same_pin_node(s, self) && node_term(s) == node_term(self) && dev_node(s->node_rec)[0x25] != 0) {
                return i;
            }
        }
    }
    return -1;
}

/* A node symbol on the same pin with the given cell type. */
static int find_node_of_type(int sym, int type)
{
    const sym_t *self = &g_syms[sym];
    if (g_vsima) {
        return find_node_of_type_vsima(sym, type, 0);
    }
    if (self->node_rec >= 0) {
        for (int i = 0; i < g_nsyms; i++) {
            const sym_t *s = &g_syms[i];
            if (same_pin_node(s, self) && dev_node(s->node_rec)[4] == type) {
                return i;
            }
        }
    }
    return -1;
}

/* Same, but (unless the device header says 3) belonging to another output
 * than the symbol itself. */
static int find_other_node(int sym, int type)
{
    const sym_t *self = &g_syms[sym];
    if (g_vsima) {
        return find_node_of_type_vsima(sym, type, 0);
    }
    if (self->node_rec >= 0) {
        for (int i = 0; i < g_nsyms; i++) {
            const sym_t *s = &g_syms[i];
            if (same_pin_node(s, self) && dev_node(s->node_rec)[4] == type) {
                if (dev_header()[0x11] == 3 || node_term(s) != node_term(self)) {
                    return i;
                }
            }
        }
    }
    return -1;
}

/* Same as find_node_of_type for a symbol that is a pin. */
static int find_node_by_pin(int sym, int type)
{
    const sym_t *self = &g_syms[sym];
    if (g_vsima) {
        return find_node_of_type_vsima(sym, type, 1);
    }
    if (self->pin_rec >= 0) {
        for (int i = 0; i < g_nsyms; i++) {
            const sym_t *s = &g_syms[i];
            if (same_pin_node(s, self) && dev_node(s->node_rec)[4] == type) {
                return i;
            }
        }
    }
    return -1;
}

/* The pin symbol with the same pin number and the given type. */
int find_feedback_symbol(int sym, int type)
{
    const sym_t *self = &g_syms[sym];
    if (g_vsima) {
        /* vsima: same name instead of same pin number */
        if (self->node_rec >= 0) {
            for (int i = 0; i < g_nsyms; i++) {
                const sym_t *s = &g_syms[i];
                if (strcmp(s->name, self->name) == 0 && s->pin_rec >= 0 && sym_type_code(s) == type) {
                    return i;
                }
            }
        }
        return -1;
    }
    if (self->node_rec >= 0) {
        for (int i = 0; i < g_nsyms; i++) {
            const sym_t *s = &g_syms[i];
            if (s->pin == self->pin && s->pin_rec >= 0 && dev_pin(s->pin_rec)[4] == type) {
                return i;
            }
        }
    }
    return -1;
}

/* The single input of the product term of node symbol `term_sym` (first
 * symbol used by its first row); *inverted tells its polarity. */
static int term_input(int term_sym, int *inverted)
{
    const sym_t *s = &g_syms[term_sym];
    int term = g_vsima ? s->node_rec : node_term(s);
    const frame_t *f;
    const uint16_t *used;
    if (term < 0 || term >= g_nterms || (f = g_terms[term].frame) == NULL || f->rows == NULL) {
        return -1;
    }
    used = f->rows + f->row_words / 3;
    for (int i = 0; i < g_nsyms; i++) {
        if (bit_test(used + 1, i)) {
            *inverted = bit_test(used + f->row_words / 3 + 1, i);
            return i;
        }
    }
    return -1;
}

/* The clock (latch enable) of `sym` when it is given by a product term;
 * *role receives the polarity. */
static int control_from_term(int sym, int term_sym, int *role, int role_true, int role_inverted)
{
    int inverted = 0;
    int input;
    *role = -1;
    input = term_input(term_sym, &inverted);
    if (input < 0) {
        return -1;
    }
    *role = inverted ? role_true : role_inverted;
    if (g_vsima && has_equation(input)) {
        /* An output of the design, such as a register clocking the next
         * one of a ripple counter: its pin symbol is never driven, the
         * term follows it.  The original took the pin, and the register
         * never saw an edge. */
        *role = role_true;
        return term_sym;
    }
    if (g_vsima) {
        /* vsima: the input itself is the control */
        if (g_syms[input].pin == 0) {
            g_syms[input].pin = (role_true == ROLE_CLOCK) ? 1 : 3;
        }
        assign_pin_rec(&g_syms[input]);
        return input;
    }
    return find_related_pin(sym, *role, g_syms[input].pin);
}

static int clock_from_term(int sym, int term_sym, int *role)
{
    return control_from_term(sym, term_sym, role, ROLE_CLOCK, ROLE_CLOCK_INV);
}

static int latch_from_term(int sym, int term_sym, int *role)
{
    return control_from_term(sym, term_sym, role, ROLE_LATCH, ROLE_LATCH_INV);
}

/* A control signal given by a product term whose input is a pin: the pin's
 * kind selects the role. */
static int pin_control_from_term(int sym, int term_sym, int *role, int default_role, int pass_role)
{
    int inverted = 0;
    int input = term_input(term_sym, &inverted);
    int kind, wanted;
    *role = -1;
    if (input < 0) {
        return -1;
    }
    if (g_vsima) {
        /* vsima: ground and supply are told by name */
        int code = lookup_name_code(g_syms[input].name, k_global_signals);
        *role = code == 2 ? 0x28 : (code == 3 ? 0x29 : default_role);
        if (pass_role) {
            if (g_syms[input].pin == 0) {
                g_syms[input].pin = 2;
            }
            assign_pin_rec(&g_syms[input]);
        }
        return input;
    }
    kind = dev_pin(g_syms[input].pin_rec)[PIN_KIND];
    if (kind == 2) {
        *role = 0x28;
        wanted = pass_role ? 0x28 : 2;
    } else if (kind == 3) {
        *role = 0x29;
        wanted = pass_role ? 0x29 : 3;
    } else {
        *role = default_role;
        wanted = default_role;
    }
    return find_related_pin(sym, wanted, g_syms[input].pin);
}

/* Let the symbol use the node record of the same output and cell number
 * that has the given type. */
static void retarget_node(int sym, int type)
{
    sym_t *s = &g_syms[sym];
    const uint8_t *own = dev_node(s->node_rec);
    int count = dev_section_count(DEV_SEC_NODES);
    if (g_vsima) {
        /* vsima: the node record of the same-named node with that extension */
        for (int i = 0; i < g_nsyms; i++) {
            const sym_t *o = &g_syms[i];
            if ((o->flags & 0x40) && strcmp(o->name, s->name) == 0 && sym_type_code(o) == type) {
                s->node_rec = o->node_rec;
                return;
            }
        }
        return;
    }
    for (int i = 0; i < count; i++) {
        const uint8_t *n = dev_node(i);
        if (rd16(n) == rd16(own) && rd16(n + 2) == rd16(own + 2) && n[4] == type) {
            s->node_rec = (int16_t)i;
            return;
        }
    }
}

/* The symbol an ORDER name refers to.  Of several symbols with the same
 * name, a pin with a cell model (unless it is of type "io") or a fed-back
 * node (other than types 9 and 0x0b) is preferred. */
static sym_t *find_symbol_csima(const char *name)
{
    for (int i = 0; i < g_nsyms; i++) {
        sym_t *s = &g_syms[i];
        if (strcmp(name, s->name) != 0) {
            continue;
        }
        if (s->pin_rec >= 0 && (s->flags & 0x80) && dev_pin(s->pin_rec)[4] != 0 && strcmp(s->type, "io") != 0) {
            return s;
        }
        if (s->node_rec >= 0 && (s->flags & 0x40)) {
            const uint8_t *node = dev_node(s->node_rec);
            if (node[0x25] != 0 && node[4] != 9 && node[4] != 0x0b) {
                return s;
            }
        }
    }
    for (int i = 0; i < g_nsyms; i++) {
        if (strcmp(name, g_syms[i].name) == 0) {
            return &g_syms[i];
        }
    }
    return NULL;
}

/* Same for vsima, with the cell types taken from the extensions. */
static sym_t *find_symbol_vsima(const char *name)
{
    for (int i = 0; i < g_nsyms; i++) {
        sym_t *s = &g_syms[i];
        int code;
        if (strcmp(name, s->name) != 0) {
            continue;
        }
        code = sym_type_code(s);
        if (s->pin_rec >= 0 && (s->flags & 0x80) && code != 0 && strcmp(s->type, "io") != 0) {
            return s;
        }
        if (s->node_rec >= 0 && (s->flags & 0x40) && cell_types()[g_cells[i].node_type & 0x7f].init != NULL &&
            code != 9 && code != 0x0b) {
            return s;
        }
    }
    for (int i = 0; i < g_nsyms; i++) {
        if (strcmp(name, g_syms[i].name) == 0) {
            return &g_syms[i];
        }
    }
    return NULL;
}

sym_t *find_symbol(const char *name)
{
    return g_vsima ? find_symbol_vsima(name) : find_symbol_csima(name);
}

/* For every fed-back node the symbol through which the feedback reaches
 * the product terms. */
void pair_feedback_symbols(void)
{
    if (g_vsima) {
        for (int i = 0; i < g_nsyms; i++) {
            const sym_t *s = &g_syms[i];
            g_feedback_sym[i] = 0;
            if (s->node_rec >= 0 && (s->flags & 0x40) && cell_types()[g_cells[i].node_type & 0x7f].init != NULL) {
                for (int j = 2; j < g_nsyms; j++) {
                    const sym_t *p = &g_syms[j];
                    int type = g_dev_has_class ? g_cells[j].pin_type : dev_pin(p->pin_rec)[4];
                    if ((p->flags & 0x80) && strcmp(s->name, p->name) == 0 && type == 0) {
                        g_feedback_sym[i] = (uint16_t)j;
                    }
                }
                if (g_feedback_sym[i] == 0) {
                    g_feedback_sym[i] = (uint16_t)i;
                }
            }
        }
        return;
    }
    for (int i = 0; i < g_nsyms; i++) {
        const sym_t *s = &g_syms[i];
        g_feedback_sym[i] = 0;
        if (s->node_rec >= 0 && (s->flags & 0x40) && dev_node(s->node_rec)[0x25] != 0) {
            for (int j = 0; j < g_nsyms; j++) {
                const sym_t *p = &g_syms[j];
                if (p->flags & 0x80) {
                    int kind = dev_pin(p->pin_rec)[PIN_KIND];
                    if ((kind == 7 || kind == 0x10 || kind == 0x0b || kind == 0x17) &&
                        strcmp(s->name, p->name) == 0 && dev_pin(p->pin_rec)[4] == 0) {
                        g_feedback_sym[i] = (uint16_t)j;
                    }
                }
            }
            if (g_feedback_sym[i] == 0) {
                g_feedback_sym[i] = (uint16_t)i;
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * Cell initialisation
 * ------------------------------------------------------------------------- */
static int *node_slots(int sym)
{
    return g_cells[sym].node_state->s;
}

static int *pin_slots(int sym)
{
    return g_cells[sym].pin_state->s;
}

/* Output enable: either a product term node (type 0x2b) decides, or the
 * device's fixed connections. */
static void bind_output_enable(int sym, int owner, int *oe_pin, int *oe_term)
{
    int node = find_other_node(owner, 0x2b);
    if (node < 0) {
        *oe_pin = find_related(owner, ROLE_OE_PIN);
        *oe_term = find_related(owner, ROLE_OE);
        /* PEEL's registered function shares the combinational OE term,
         * but its connection table only links that term to the latter. */
        if (*oe_pin < 0 && *oe_term < 0 && strncmp(g_device_name, "f18v8z", 6) == 0) {
            *oe_pin = find_node_of_type(owner, ROLE_OE_PIN);
        }
    } else {
        int role;
        int found = pin_control_from_term(sym, node, &role, ROLE_OE, 1);
        if (role == ROLE_OE) {
            *oe_term = found;
            *oe_pin = -1;
        } else {
            *oe_term = -1;
            *oe_pin = found;
        }
    }
}

/* Clock from a product term node (type 0x2a) or the fixed connection. */
static void bind_clock(int sym, int *clock, int *polarity)
{
    int node = find_other_node(sym, 0x2a);
    if (node < 0) {
        *clock = find_related(sym, ROLE_CLOCK);
        *polarity = ROLE_CLOCK;
        if (g_vsima && *clock > 0) {
            trace_clock_vsima(clock, polarity);
        }
    } else {
        *clock = clock_from_term(sym, node, polarity);
    }
}

/* Polarity of a fixed clock / latch enable: inverted if it comes through a
 * node whose record says so. */
static int fixed_polarity(int control, int role_true, int role_inverted)
{
    const sym_t *s;
    if (control < 0) {
        return role_true;
    }
    s = &g_syms[control];
    if (s->node_rec < 1 || dev_node(s->node_rec)[5] != 6) {
        return role_true;
    }
    return role_inverted;
}

static void mark_clock(int clock)
{
    if (clock > 0) {
        g_sig_is_clock[clock] = 1;
    }
}

/* combinational output (node types 0, 0x0d, 0x30) */
static void init_combinational(int sym)
{
    int *s = node_slots(sym);
    int t = g_cells[sym].node_type;
    if (t != 0 && t != 0x30 && t != 0x0d) {
        fatal_error(0x405, VTEXT("", "non-register primative initialization"));
    }
    s[0] = sym;
    bind_output_enable(sym, sym, &s[1], &s[2]);
    s[3] = g_vsima ? -1 : find_related(sym, ROLE_14);
    s[4] = find_related(sym, ROLE_20);
}

/* A reset / preset given by a product term node of type `node_type`.  The
 * lookup is always made with the default role ROLE_AR (reset), so a preset
 * given this way is never bound.  This is deliberate; the simulation results
 * depend on it. */
static void bind_term_control(int sym, int node_type, int role, int *slot)
{
    int node = find_other_node(sym, node_type);
    if (node < 0) {
        if (find_other_node(sym, role) >= 0) {
            *slot = find_related(sym, role);
        }
    } else {
        int got;
        int found = pin_control_from_term(sym, node, &got, ROLE_AR, 0);
        *slot = (got == role) ? found : -1;
    }
}

/* JK flip-flop (node type 10) */
static void init_jk(int sym)
{
    int *s = node_slots(sym);
    if (g_cells[sym].node_type != 10) {
        fatal_error(0x406, VTEXT("jk", "jk initialization"));
    }
    s[0] = sym;
    s[1] = find_node_of_type(sym, 0x0b);
    if (s[1] < 0) {
        fatal_error(0x40f, VTEXT("", "jk initialization"));
    }
    s[2] = sym;
    bind_output_enable(sym, sym, &s[3], &s[4]);
    s[5] = find_related(sym, ROLE_20);
    bind_term_control(sym, 0x3e, ROLE_AR, &s[6]);
    bind_term_control(sym, 0x3f, ROLE_AP, &s[7]);
    s[8] = find_related(sym, ROLE_SR);
    s[9] = find_related(sym, ROLE_SP);
    bind_clock(sym, &s[10], &s[11]);
    mark_clock(s[10]);
}

/* SR flip-flop (node type 8) */
static void init_sr(int sym)
{
    int *s = node_slots(sym);
    if (g_cells[sym].node_type != 8) {
        fatal_error(0x406, VTEXT("sr", "sr initialization"));
    }
    s[0] = sym;
    s[1] = find_node_of_type(sym, 9);
    if (s[1] < 0) {
        fatal_error(0x40e, VTEXT("sr", "sr initialization"));
    }
    s[2] = sym;
    bind_output_enable(sym, sym, &s[3], &s[4]);
    s[5] = find_related(sym, ROLE_20);
    s[8] = find_related(sym, ROLE_SR);
    s[6] = find_related(sym, ROLE_AR);
    s[9] = find_related(sym, ROLE_SP);
    s[7] = find_related(sym, ROLE_AP);
    s[4] = -1;
    if (g_pin_and_node_resets && s[7] < 0) {
        s[7] = find_related(sym, ROLE_AP_ALT);
    } else if (g_pin_and_node_resets && s[6] < 0) {
        s[6] = find_related(sym, ROLE_AP_ALT);
    } else {
        s[4] = find_related(sym, ROLE_OE);
    }
    bind_clock(sym, &s[10], &s[11]);
    mark_clock(s[10]);
}

/* D register at an input pin (pin type 0x17) */
static void init_input_register(int sym)
{
    int *s = pin_slots(sym);
    int node;
    if (g_cells[sym].pin_type != 0x17) {
        fatal_error(0x406, VTEXT("di", "d-type input initialization"));
    }
    s[0] = sym;
    s[1] = sym;
    node = find_node_by_pin(sym, 0x2a);
    if (node < 0) {
        s[10] = find_related(sym, ROLE_CLOCK);
        s[12] = ROLE_CLOCK;
        if (g_vsima && s[10] > 0) {
            trace_clock_vsima(&s[10], &s[12]);
        }
    } else {
        s[10] = clock_from_term(sym, node, &s[12]);
    }
    mark_clock(s[10]);
    s[8] = find_related(sym, ROLE_SR);
    s[6] = find_related(sym, ROLE_AR);
    s[9] = find_related(sym, ROLE_SP);
    s[7] = find_related(sym, ROLE_AP);
}

/* latch at an input pin (pin type 0x18) */
static void init_input_latch(int sym)
{
    int *s = pin_slots(sym);
    int node;
    if (g_cells[sym].pin_type != 0x18) {
        fatal_error(0x406, VTEXT("lq", "lq initialization"));
    }
    s[0] = sym;
    s[1] = sym;
    node = find_node_by_pin(sym, 0x2c);
    if (node < 0) {
        s[10] = find_related(sym, ROLE_LATCH);
        if (s[10] < 0) {
            s[10] = find_related(sym, ROLE_CLOCK);
            s[11] = ROLE_CLOCK;
        } else {
            s[11] = ROLE_LATCH;
        }
    } else {
        s[10] = latch_from_term(sym, node, &s[11]);
    }
    s[8] = find_related(sym, ROLE_SR);
    s[6] = find_related(sym, ROLE_AR);
    s[9] = find_related(sym, ROLE_SP);
    s[7] = find_related(sym, ROLE_AP);
}

/* The part shared by the registers with slots 2..9. */
static void bind_register_controls(int sym, int *s)
{
    bind_output_enable(sym, sym, &s[2], &s[3]);
    s[4] = g_vsima ? -1 : find_related(sym, ROLE_14);
    s[5] = find_related(sym, ROLE_20);
    s[8] = find_related(sym, ROLE_SR);
    s[6] = find_related(sym, ROLE_AR);
    s[9] = find_related(sym, ROLE_SP);
    s[7] = find_related(sym, ROLE_AP);
}

/* D flip-flop (node types 1 and 0x15) */
static void init_d(int sym)
{
    int *s = node_slots(sym);
    int t = g_cells[sym].node_type;
    int node = -1;
    if (t != 1 && t != 0x15) {
        fatal_error(0x406, VTEXT("d", "d-type output initialization"));
    }
    s[0] = sym;
    s[1] = sym;
    bind_register_controls(sym, s);
    if (g_vsima) {
        s[11] = -1;
        s[13] = -1;
        bind_clock(sym, &s[10], &s[12]);
        mark_clock(s[10]);
        return;
    }
    s[11] = g_dev_is_isp1016 ? 0 : -1;
    s[13] = find_related(sym, ROLE_1A);
    if (s[13] == -1) {
        s[13] = find_related(sym, ROLE_2F);
    }
    if (!g_dev_is_7c330) {
        node = find_other_node(sym, 0x2a);
    }
    if (node >= 0) {
        s[10] = clock_from_term(sym, node, &s[12]);
    } else {
        s[10] = find_related(sym, ROLE_CLOCK);
        s[12] = fixed_polarity(s[10], ROLE_CLOCK, ROLE_CLOCK_INV);
        if (s[11] == 0) {
            s[11] = find_related_second(sym, ROLE_CLOCK);
        }
    }
    mark_clock(s[10]);
    if (s[7] < 0) {
        s[7] = find_related(sym, ROLE_AP_ALT);
    }
}

/* D flip-flop with clock enable (node type 0x16) */
static void init_d_enable(int sym)
{
    int *s = node_slots(sym);
    if (g_cells[sym].node_type != 0x16) {
        fatal_error(0x406, VTEXT("de", "de-type output initialization"));
    }
    s[0] = sym;
    s[1] = sym;
    s[2] = find_node_of_type(sym, 0x1f);
    if (s[2] < 0) {
        s[2] = -1;
    }
    bind_output_enable(sym, sym, &s[3], &s[4]);
    s[5] = g_vsima ? -1 : find_related(sym, ROLE_14);
    s[6] = find_related(sym, ROLE_20);
    s[9] = find_related(sym, ROLE_SR);
    s[7] = find_related(sym, ROLE_AR);
    s[10] = find_related(sym, ROLE_SP);
    s[8] = find_related(sym, ROLE_AP);
    s[13] = g_vsima ? -1 : find_related(sym, ROLE_1A);
    bind_clock(sym, &s[11], &s[12]);
    mark_clock(s[11]);
    if (!g_vsima && s[8] < 0) {
        s[8] = find_related(sym, ROLE_AP_ALT);
    }
}

/* T flip-flop (node type 0x1b) */
static void init_t(int sym)
{
    int *s = node_slots(sym);
    if (g_cells[sym].node_type != 0x1b) {
        fatal_error(0x406, VTEXT("t", "t-type output initialization"));
    }
    s[0] = sym;
    s[1] = sym;
    bind_register_controls(sym, s);
    s[12] = g_vsima ? -1 : find_related(sym, ROLE_1A);
    bind_clock(sym, &s[10], &s[11]);
    mark_clock(s[10]);
    if (!g_vsima && s[7] < 0) {
        s[7] = find_related(sym, ROLE_AP_ALT);
    }
}

/* Latch enable from a product term node (type `node_type`) or fixed. */
static void bind_latch_enable(int sym, int node_type, int *enable, int *polarity)
{
    int node = find_other_node(sym, node_type);
    if (node >= 0) {
        *enable = latch_from_term(sym, node, polarity);
        return;
    }
    *enable = find_related(sym, ROLE_LATCH);
    if (*enable < 0) {
        *enable = find_related(sym, ROLE_CLOCK);
        *polarity = ROLE_CLOCK;
    } else if (g_vsima) {
        *polarity = (g_syms[*enable].flags & 0x10) ? ROLE_LATCH_INV : ROLE_LATCH;
    } else {
        *polarity = fixed_polarity(*enable, ROLE_LATCH, ROLE_LATCH_INV);
    }
}

/* latch (node type 0x1c) */
static void init_latch(int sym)
{
    int *s = node_slots(sym);
    if (g_cells[sym].node_type != 0x1c) {
        fatal_error(0x406, VTEXT("L", "L-type output initialization"));
    }
    s[0] = sym;
    s[1] = sym;
    bind_register_controls(sym, s);
    s[12] = g_vsima ? -1 : find_related(sym, ROLE_1A);
    bind_latch_enable(sym, 0x2c, &s[10], &s[11]);
    if (!g_vsima && s[7] < 0) {
        s[7] = find_related(sym, ROLE_AP_ALT);
    }
}

static int feedback_node_or_die(int sym)
{
    int node = find_feedback_node(sym);
    if (node < 0) {
        fatal_error(ERR_UNDEFINED_FEEDBACK, g_syms[sym].name);
    }
    return node;
}

/* internal feedback (pin type 0x26) */
static void init_internal_feedback(int sym)
{
    int *s = pin_slots(sym);
    int node;
    if (g_cells[sym].pin_type != 0x26) {
        fatal_error(0x405, VTEXT("int", "internal feedback initialization"));
    }
    node = feedback_node_or_die(sym);
    s[0] = node_slots(node)[0];
    s[1] = -1;
    s[2] = -1;
    s[3] = -1;
}

/* The symbol whose value an I/O pin shows: the node itself or, for a
 * registered node, its output. */
static int io_source(int sym, int node)
{
    int t = g_cells[node].node_type;
    int source;
    if (t == 0) {
        return node_slots(node)[0];
    }
    /* SR and JK registers keep their state symbol in slot 2; csima always
     * takes slot 1. */
    source = node_slots(node)[(g_vsima && (t == 8 || t == 9 || t == 10 || t == 11)) ? 2 : 1];
    retarget_node(sym, 0);
    return source;
}

/* I/O pin (pin type 0x25) */
static void init_io(int sym)
{
    int *s = pin_slots(sym);
    int node;
    if (g_cells[sym].pin_type != 0x25) {
        fatal_error(0x405, VTEXT("io", "pin feedback initialization"));
    }
    node = feedback_node_or_die(sym);
    if (g_cells[node].node_type == 0) {
        const int *n = node_slots(node);
        s[0] = n[0];
        s[1] = n[1];
        s[2] = n[2];
        s[3] = g_vsima ? -1 : n[3];
    } else {
        s[0] = io_source(sym, node);
        s[1] = find_related(sym, ROLE_OE_PIN);
        s[2] = find_related(sym, ROLE_OE);
        s[3] = g_vsima ? -1 : find_related(sym, ROLE_14);
    }
    g_cells[node].kind = 3;
    g_cells[node].fed_back = 1;
    if (g_vsima) {
        g_cells[node].true_fb = 1;
    }
}

/* register in the feedback path (pin types
 * 0x22 D, 0x23 latch, 0x24 T) */
static void init_feedback_register(int sym, int pin_type, int node_type, const char *name)
{
    int *s = pin_slots(sym);
    int node;
    if (g_cells[sym].pin_type != pin_type) {
        fatal_error(0x405, name);
    }
    node = feedback_node_or_die(sym);
    if (g_cells[node].node_type != node_type) {
        int term;
        int polarity_slot = (pin_type == 0x22) ? 12 : 11;
        s[0] = node_slots(node)[0];
        s[1] = sym;
        retarget_node(sym, node_type);
        s[8] = find_related(sym, ROLE_SR);
        s[6] = find_related(sym, ROLE_AR);
        s[9] = find_related(sym, ROLE_SP);
        s[7] = find_related(sym, ROLE_AP);
        term = find_other_node(sym, (g_vsima && pin_type == 0x23) ? 0x2c : 0x2a);
        if (term >= 0) {
            if (g_vsima && pin_type == 0x23) {
                s[10] = latch_from_term(sym, term, &s[polarity_slot]);
            } else {
                s[10] = clock_from_term(sym, term, &s[polarity_slot]);
            }
        } else if (pin_type == 0x23) {
            s[10] = find_related(sym, ROLE_LATCH);
            if (s[10] < 0) {
                s[10] = find_related(sym, ROLE_CLOCK);
                s[11] = ROLE_CLOCK;
            } else {
                s[11] = ROLE_LATCH;
            }
        } else {
            s[10] = find_related(sym, ROLE_CLOCK);
            s[polarity_slot] = ROLE_CLOCK;
            if (g_vsima && pin_type == 0x22 && s[10] > 0) {
                trace_clock_vsima(&s[10], &s[polarity_slot]);
            }
        }
    }
    if (pin_type == 0x22) {
        mark_clock(s[10]);
        s[13] = -1;
    } else {
        s[12] = -1;
    }
    s[2] = -1;
    s[3] = -1;
    s[4] = -1;
}

static void init_feedback_d(int sym)
{
    init_feedback_register(sym, 0x22, 1, VTEXT("dfb", "d-type feedback initialization"));
}

static void init_feedback_latch(int sym)
{
    init_feedback_register(sym, 0x23, 0x1c, VTEXT("lfb", "L-type feedback initialization"));
}

static void init_feedback_t(int sym)
{
    init_feedback_register(sym, 0x24, 0x1b, VTEXT("tfb", "t-type feedback initialization"));
}

/* The symbol bound to cell number `number` (an SR register if
 * `need_sr`). */
static int find_cell_number(int number, int need_sr)
{
    for (int i = 0; i < g_nsyms; i++) {
        if (g_cells[i].number == number && (!need_sr || g_cells[i].node_type == 8)) {
            return i;
        }
    }
    return -1;
}

/* counter of the f507 (pin type 0x2e; csima only) */
static void init_counter(int sym)
{
    int *s = pin_slots(sym);
    int node, index;
    if (g_cells[sym].pin_type != 0x2e) {
        fatal_error(0x406, "cnt_1");
    }
    if (strcmp(g_device_name, "f507") != 0) {
        fatal_error(0x406, "f507");
    }
    s[0] = sym;
    s[1] = find_cell_number(0x27, 0);
    s[2] = find_cell_number(0x28, 1);
    s[3] = find_cell_number(0x29, 0);
    s[4] = find_cell_number(0x2a, 1);
    s[7] = LV_UNKNOWN;
    node = find_node_by_pin(sym, 0x2a);
    if (node < 0) {
        s[5] = find_related(sym, ROLE_CLOCK);
        s[6] = ROLE_CLOCK;
    } else {
        s[5] = clock_from_term(sym, node, &s[6]);
    }
    index = (int)g_cells[sym].number - 0x21;
    if (index < 0 || index > 5) {
        fatal_error(0x406, "pn");
    }
    s[8] = index;
    g_counter_cells[index] = sym;
}

/* Common part of the registered I/O pins (types 0x33, 0x34, 0x35). */
static int bind_io_register(int sym, int *s)
{
    int node = feedback_node_or_die(sym);
    s[0] = io_source(sym, node);
    return node;
}

/* I/O pin with D register (pin type 0x33) */
static void init_io_d(int sym)
{
    int *s = pin_slots(sym);
    int node, term;
    if (g_cells[sym].pin_type != 0x33) {
        fatal_error(0x405, VTEXT("iod", "iod feedback initialization"));
    }
    s[0] = sym;
    s[1] = sym;
    node = bind_io_register(sym, s);
    bind_output_enable(sym, s[0], &s[2], &s[3]);
    s[4] = g_vsima ? -1 : find_related(sym, ROLE_14);
    s[7] = find_related_of_pin(sym, 0x38);
    s[5] = find_related_of_pin(sym, 0x36);
    s[8] = find_related_of_pin(sym, 0x39);
    s[6] = find_related_of_pin(sym, 0x37);
    if (g_vsima) {
        s[11] = -1;
        s[10] = ROLE_CLOCK;
        s[9] = find_related(sym, 0x3a);
        if (s[9] > 0) {
            trace_clock_vsima(&s[9], &s[10]);
            mark_clock(s[9]);
        }
        g_cells[node].kind = 3;
        g_cells[node].fed_back = 1;
        return;
    }
    s[11] = find_related(sym, ROLE_1A);
    term = find_node_by_pin(sym, 0x2a);
    if (term < 0) {
        term = find_other_node(sym, 0x2a);
    }
    if (term < 0) {
        s[9] = find_related_of_pin(sym, ROLE_CLOCK);
        s[10] = fixed_polarity(s[9], ROLE_CLOCK, ROLE_CLOCK_INV);
    } else {
        s[9] = clock_from_term(sym, term, &s[10]);
    }
    if (s[9] < 0) {
        s[9] = find_related_of_pin(sym, 0x3a);
        s[10] = ROLE_CLOCK;
    }
    if (term < 0) {
        mark_clock(s[9]);
    }
    if (s[6] < 0) {
        s[6] = find_related(sym, ROLE_AP_ALT);
    }
    g_cells[node].kind = 3;
    g_cells[node].fed_back = 1;
}

/* I/O pin with T register (pin type 0x35) */
static void init_io_t(int sym)
{
    int *s = pin_slots(sym);
    int node, term;
    if (g_cells[sym].pin_type != 0x35) {
        fatal_error(0x405, VTEXT("iot", "iot feedback initialization"));
    }
    s[0] = sym;
    node = bind_io_register(sym, s);
    bind_output_enable(sym, sym, &s[2], &s[3]);
    s[4] = g_vsima ? -1 : find_related(sym, ROLE_14);
    s[7] = find_related(sym, ROLE_SR);
    s[5] = find_related(sym, ROLE_AR);
    s[8] = find_related(sym, ROLE_SP);
    s[6] = find_related(sym, ROLE_AP);
    if (g_vsima) {
        s[11] = -1;
        s[9] = find_related(sym, 0x3a);
        s[10] = ROLE_CLOCK;
        if (s[9] > 0) {
            trace_clock_vsima(&s[9], &s[10]);
            mark_clock(s[9]);
        }
        g_cells[node].kind = 3;
        g_cells[node].fed_back = 1;
        return;
    }
    s[11] = find_related(sym, ROLE_1A);
    term = find_other_node(sym, 0x2a);
    if (term < 0) {
        s[9] = find_related(sym, ROLE_CLOCK);
        s[10] = fixed_polarity(s[9], ROLE_CLOCK, ROLE_CLOCK_INV);
    } else {
        s[9] = clock_from_term(sym, term, &s[10]);
    }
    mark_clock(s[9]);
    if (s[6] < 0) {
        s[6] = find_related(sym, ROLE_AP_ALT);
    }
    g_cells[node].kind = 3;
    g_cells[node].fed_back = 1;
}

/* I/O pin with latch (pin type 0x34) */
static void init_io_latch(int sym)
{
    int *s = pin_slots(sym);
    if (g_cells[sym].pin_type != 0x34) {
        fatal_error(0x405, VTEXT("iol", "iol feedback initialization"));
    }
    int node;
    s[1] = sym;
    node = bind_io_register(sym, s);
    bind_output_enable(sym, sym, &s[2], &s[3]);
    s[4] = g_vsima ? -1 : find_related(sym, ROLE_14);
    s[7] = find_related(sym, ROLE_SR);
    s[5] = find_related(sym, ROLE_AR);
    s[8] = find_related(sym, ROLE_SP);
    s[6] = find_related(sym, ROLE_AP);
    if (g_vsima) {
        int term = find_other_node(sym, 0x2c);
        s[11] = -1;
        if (term >= 0) {
            s[9] = latch_from_term(sym, term, &s[10]);
        } else {
            s[9] = find_related(sym, ROLE_LATCH);
            if (s[9] < 0) {
                s[9] = find_related(sym, 0x3a);
                s[10] = ROLE_CLOCK;
                if (s[9] >= 0) {
                    trace_clock_vsima(&s[9], &s[10]);
                }
            } else {
                s[10] = ROLE_LATCH;
            }
        }
        g_cells[node].kind = 3;
        g_cells[node].fed_back = 1;
        return;
    }
    s[11] = find_related(sym, ROLE_1A);
    bind_latch_enable(sym, 0x2c, &s[9], &s[10]);
    if (s[6] < 0) {
        s[6] = find_related(sym, ROLE_AP_ALT);
    }
}

/* -------------------------------------------------------------------------
 * Cell type table and binder
 * ------------------------------------------------------------------------- */

/* {state size, init, eval} per cell type (csima) */
static const cell_type_t k_cell_types_csima[CELL_TYPE_COUNT] = {
    [0x00] = { 20, init_combinational, eval_combinational },
    [0x01] = { 56, init_d, eval_d_feedback },
    [0x08] = { 48, init_sr, eval_sr },
    [0x0a] = { 48, init_jk, eval_jk },
    [0x0d] = { 20, init_combinational, eval_combinational },
    [0x15] = { 56, init_d, eval_d },
    [0x16] = { 56, init_d_enable, eval_d_enable },
    [0x17] = { 56, init_input_register, eval_input_register },
    [0x18] = { 52, init_input_latch, eval_input_latch },
    [0x1b] = { 52, init_t, eval_t },
    [0x1c] = { 52, init_latch, eval_latch },
    [0x22] = { 56, init_feedback_d, eval_feedback_d },
    [0x23] = { 52, init_feedback_latch, eval_feedback_latch },
    [0x24] = { 52, init_feedback_t, eval_feedback_t },
    [0x25] = { 20, init_io, eval_io },
    [0x26] = { 20, init_internal_feedback, eval_internal_feedback },
    [0x2e] = { 36, init_counter, eval_counter },
    [0x30] = { 20, init_combinational, eval_combinational },
    [0x33] = { 48, init_io_d, eval_io_d },
    [0x34] = { 48, init_io_latch, eval_io_latch },
    [0x35] = { 48, init_io_t, eval_io_t }
};

/* vsima: pin type 0x0c has no model */
static void init_none(int sym)
{
    if (g_cells[sym].pin_type != 0x0c) {
        fatal_error(0x405, "flip-flop mode initialization");
    }
}

/* {state size, init, eval} per cell type (vsima) */
static const cell_type_t k_cell_types_vsima[CELL_TYPE_COUNT] = {
    [0x00] = { 20, init_combinational, eval_combinational },
    [0x01] = { 56, init_d, eval_d_feedback },
    [0x08] = { 48, init_sr, eval_sr },
    [0x0a] = { 48, init_jk, eval_jk },
    [0x0c] = { 56, init_none, eval_none },
    [0x0d] = { 20, init_combinational, eval_combinational },
    [0x15] = { 56, init_d, eval_d },
    [0x16] = { 56, init_d_enable, eval_d_enable },
    [0x17] = { 56, init_input_register, eval_input_register },
    [0x18] = { 52, init_input_latch, eval_input_latch },
    [0x1b] = { 52, init_t, eval_t },
    [0x1c] = { 52, init_latch, eval_latch },
    [0x22] = { 56, init_feedback_d, eval_feedback_d },
    [0x23] = { 52, init_feedback_latch, eval_feedback_latch },
    [0x24] = { 52, init_feedback_t, eval_feedback_t },
    [0x25] = { 20, init_io, eval_io },
    [0x26] = { 20, init_internal_feedback, eval_internal_feedback },
    [0x2e] = { 36, NULL, NULL },
    [0x30] = { 20, init_combinational, eval_combinational },
    [0x33] = { 48, init_io_d, eval_io_d },
    [0x34] = { 48, init_io_latch, eval_io_latch },
    [0x35] = { 48, init_io_t, eval_io_t }
};

const cell_type_t *cell_types(void)
{
    return g_vsima ? k_cell_types_vsima : k_cell_types_csima;
}

/* The symbol with this name and type "byp" (0 if none). */
static int find_bypass_symbol(const char *name)
{
    for (int i = 0; i < g_nsyms; i++) {
        if (strcmp(name, g_syms[i].name) == 0 && strcmp(g_syms[i].type, "byp") == 0) {
            return i;
        }
    }
    return 0;
}

static void run_init(int type, int sym)
{
    if (type > 0x43) {
        fatal_error(g_vsima ? 0x3ff : 0x402, VTEXT("", "module MACINIT1"));
    }
    if (cell_types()[type].init != NULL) {
        cell_types()[type].init(sym);
    }
}

/* One round of cell initialisation over all symbols.  `pin_from` is the
 * lowest pin record index treated as a pin (0 in the first round, 1 in the
 * second). */
static void init_all_cells(int pin_from)
{
    for (int i = 0; i < g_nsyms; i++) {
        const sym_t *s = &g_syms[i];
        if (g_vsima) {
            /* vsima: the types were put into the cells by the binder */
            if ((s->flags & 0x80) && s->pin_rec >= pin_from && g_cells[i].pin_type != 0) {
                run_init(g_cells[i].pin_type, i);
            } else if (s->node_rec >= 0 && (!(s->flags & 0x80) || (s->flags & 0x40)) &&
                       (pin_from == 0 || (s->flags & 0x40))) {
                run_init(g_cells[i].node_type, i);
            }
            continue;
        }
        if (s->pin_rec >= pin_from) {
            int t = dev_pin(s->pin_rec)[4];
            if (t > 0x43) {
                fatal_error(0x402, "");
            }
            if (t != 0) {
                run_init(t, i);
                continue;
            }
        }
        if (s->node_rec >= 0 && (!(s->flags & 0x80) || (s->flags & 0x40))) {
            run_init(dev_node(s->node_rec)[4], i);
        }
    }
}

/* Default of cell.true_out by the kind byte of a node record.  Only the
 * first 8 entries are real defaults; the values for kinds 8..31 look
 * arbitrary but are deliberate, as the results for such node records
 * depend on them. */
static const int8_t k_node_kind_default[2][32] = {
    { 0, 0, 1, 1, 1, 0, 0, 1, 0x14, 0, 0, 0, (int8_t)0x88, (int8_t)0xc9, 0x40, 0,
      (int8_t)0xaf, 0x7b, 0x40, 0, 0x38, 0, 0, 0, 0x6e, (int8_t)0xd1, 0x40, 0, (int8_t)0x92, (int8_t)0x85, 0x40, 0 },
    { 0, 0, 1, 1, 1, 0, 0, 1, 0x14, 0, 0, 0, (int8_t)0xe4, (int8_t)0xbd, 0x40, 0,
      (int8_t)0xf0, (int8_t)0x8e, 0x40, 0, 0x38, 0, 0, 0, 0x1e, (int8_t)0xc6, 0x40, 0, 0x6f, (int8_t)0x80, 0x40, 0 }
};

int node_kind_default(int kind)
{
    return k_node_kind_default[g_vsima ? 1 : 0][kind & 31];
}

/* vsima: a "d" register with a clock enable (x.ce) is type 0x16. */
static int d_register_type(const sym_t *s)
{
    for (int i = 0; i < g_nsyms; i++) {
        if (strcmp(s->name, g_syms[i].name) == 0 && strcasecmp(g_syms[i].type, "ce") == 0) {
            return 0x16;
        }
    }
    return 1;
}

/* bind_cells for vsima: the cell types come from the symbols' extensions. */
static void bind_cells_vsima(void)
{
    memset(g_bypass_sym, 0, g_nsyms);
    for (int i = 0; i < g_nsyms; i++) {
        const sym_t *s = &g_syms[i];
        cell_t *c = &g_cells[i];
        int polarity_default;
        c->kind = 0;
        c->bound = 0;
        c->is_node = 0;
        c->pin_type = 0;
        c->node_state = alloc_tracked(sizeof(cell_state_t), "initialization");
        c->pin_state = alloc_tracked(sizeof(cell_state_t), "initialization");
        if (s->pin_rec >= 0) {
            int t = sym_type_code(s);
            if (t > 0x43) {
                fatal_error(0x3fc, "module MACINIT1");
            }
            c->kind = 1;
            c->bound = 1;
            c->pin_type = (int16_t)t;
            c->true_out = 1;
            c->true_fb = 1;
            c->number = s->pin;
            if ((s->flags & 0x80) && !(s->flags & 0x40)) {
                continue;
            }
            if (s->node_rec >= 0) {
                c->kind = 3;
                c->fed_back = 1;
                bit_set(g_fb_symbols, i);
            }
        }
        /* A variable is no cell.  REGISTER_SELECT leaves the input of a
         * two-input flip-flop that it does not convert as the variable
         * ".<name>" with its extension (".q.s"); the original made it a
         * flip-flop node of q and stopped with "sr initialization". */
        if (s->node_rec < 0 || s->kind == 3) {
            continue;
        }
        {
            int t = sym_type_code(s);
            if (c->kind != 3) {
                c->kind = 2;
            }
            if (t > 0x43) {
                fatal_error(0x3fd, "module MACINIT1");
            }
            if (t == 1) {
                t = d_register_type(s);
            }
            c->bound = 1;
            c->node_type = (int16_t)t;
            c->number = s->pin;
            c->term = (uint16_t)s->node_rec;
            polarity_default = (!(s->flags & 0x20) || (s->flags & 0x10)) && ((s->flags & 0x20) || !(s->flags & 0x10));
            if (polarity_default) {
                c->true_out = 1;
                c->true_fb = 1;
            } else {
                c->true_out = 0;
                /* registers invert in their model */
                if (t == 1 || t == 0x1b || t == 0x1c || t == 10 || t == 11 || t == 8 || t == 9) {
                    c->true_fb = 0;
                } else {
                    c->true_fb = (s->flags & 0x10) ? 0 : 1;
                }
            }
            if (s->flags & 0x40) {
                c->is_node = 1;
            }
            if (strcmp(s->type, "byp") == 0) {
                g_bypass_sym[find_bypass_symbol(s->name)] = (uint8_t)i;
            }
        }
    }
    g_pin_and_node_resets = 0;
    init_all_cells(0);
    init_all_cells(1);
}

/* Fill g_cells from the device records and run the cell models'
 * initialisation (twice, so that cells can refer to each other). */
void bind_cells(void)
{
    int has_node_5_or_7 = 0;
    int has_pin_kind_e = 0;
    if (g_vsima) {
        bind_cells_vsima();
        return;
    }
    memset(g_bypass_sym, 0, g_nsyms);
    for (int i = 0; i < g_nsyms; i++) {
        const sym_t *s = &g_syms[i];
        cell_t *c = &g_cells[i];
        int bind_node = 1;
        c->kind = 0;
        c->bound = 0;
        c->is_node = 0;
        c->node_state = alloc_tracked(sizeof(cell_state_t), "initialization");
        c->pin_state = alloc_tracked(sizeof(cell_state_t), "initialization");
        if (s->pin_rec >= 0) {
            const uint8_t *pin = dev_pin(s->pin_rec);
            int t = pin[4];
            if (pin[PIN_KIND] == 0x0e) {
                has_pin_kind_e = 1;
            }
            if (t > 0x43) {
                fatal_error(0x402, "");
            }
            c->kind = 1;
            c->bound = 1;
            c->pin_type = (int16_t)t;
            c->number = (uint16_t)rd16(pin + 2);
            bind_node = 0;
            if (!(s->flags & 0x80) || (s->flags & 0x40)) {
                switch (pin[PIN_KIND]) {
                case 7: case 0x0b: case 0x10: case 0x17:
                    c->kind = 3;
                    c->fed_back = 1;
                    bit_set(g_fb_symbols, i);
                    break;
                default:
                    break;
                }
                bind_node = 1;
            }
        }
        if (bind_node && s->node_rec >= 0) {
            const uint8_t *node = dev_node(s->node_rec);
            int t = node[4];
            if (c->kind != 3) {
                c->kind = 2;
            }
            if (t > 0x43) {
                fatal_error(0x402, "");
            }
            if (t == 5 || t == 7) {
                has_node_5_or_7 = 1;
            }
            c->bound = 1;
            c->node_type = (int16_t)t;
            c->number = (uint16_t)rd16(node + 2);
            c->term = (uint16_t)rd16(node);
            if (g_args[7][0] == '1') {
                /* mode selected by the 8th phase argument ("1": virtual device) */
                if (((s->flags & 0x20) && !(s->flags & 0x10)) || (!(s->flags & 0x20) && (s->flags & 0x10))) {
                    c->true_out = 0;
                    c->true_fb = (t == 0x1b) ? 0 : 1;
                } else {
                    c->true_out = 1;
                    c->true_fb = 1;
                }
            } else {
                c->true_out = (int16_t)node_kind_default(node[5]);
                c->true_fb = (node[6] == 0) ? 1 : 0;
            }
            if (s->flags & 0x40) {
                c->is_node = 1;
            }
            if (strcmp(s->type, "byp") == 0) {
                g_bypass_sym[find_bypass_symbol(s->name)] = (uint8_t)i;
            }
        }
    }
    g_pin_and_node_resets = (has_node_5_or_7 && has_pin_kind_e);
    init_all_cells(0);
    init_all_cells(1);
}
