/*
 * jedec.c - fuse map and JEDEC file.
 *
 * The product terms of every OR array are placed row by row: each input of
 * a term blows one of the two fuses (true / complement column) the device
 * library gives for the pin; macrocell configuration fuses are set from the
 * node records.  Several devices need extra work: ep312/ep324 borrow product
 * terms from neighbouring macrocells, max5032 uses expander terms, v2500b
 * shares rows between the two halves of a macrocell, xl78c800 routes single
 * inputs through a matrix.
 */
#include "banner.h"
#include "fusemap.h"

struct cuplc_row_state row_state;

/* Macrocell of the ep312, ep324 and v2500b (tables in jedec_cells.h) */
typedef struct {
    uint16_t pin;
    uint16_t alt_pin;           /* second pin or node number of the cell */
    uint16_t neighbor[2];       /* cells terms can be borrowed from;
                                   v2500b uses [1] as a mark */
    uint16_t avail;             /* product terms available */
    uint16_t needed;            /* product terms of the cell's output */
    uint32_t row_fuse[4];       /* first fuse of each group of rows */
    int32_t row_terms[4];       /* product terms in each group */
    uint32_t sel_fuse[4];       /* selection fuses */
} cell_template_t;

#include "jedec_cells.h"
#include "jedsum.h"

#define MAX_CELLS 24
#define ROW_BUF 256
#define ROW_MAX 200             /* widest product-term row blow_term() accepts */

static cell_template_t s_cells[MAX_CELLS];   /* working copy of the cell table */
static int s_ncells;
static int32_t s_expanders[32][9];           /* working copy of k_max5_expanders */
static int s_expander_needed;

static FILE *s_jed_fp;
static char *s_jed_name;
static unsigned s_transmit_sum;
static jedsum_fuses_t s_fuse_sum;

void jed_reset(void)
{
    memset(&row_state, 0, sizeof(row_state));
    memset(s_cells, 0, sizeof(s_cells));
    s_ncells = 0;
    memcpy(s_expanders, k_max5_expanders, sizeof(s_expanders));
    s_expander_needed = 0;
    s_jed_fp = NULL;
    s_jed_name = NULL;
    s_transmit_sum = 0;
    jedsum_fuses_init(&s_fuse_sum);
}

/* -------------------------------------------------------------------------
 * Fuse access
 * ------------------------------------------------------------------------- */

/* Set one fuse of the map to 0 or 1 */
void set_fuse(uint32_t fuse, unsigned value)
{
    if (value > 1) {
        report_error(1005, "blowing fuse");
        value = 0;
    }
    if (g.fuses == NULL || (size_t)(fuse >> 3) >= g.fuses_size) {
        return;     /* fuse outside the map */
    }
    uint8_t *p = &g.fuses[fuse >> 3];
    *p = (uint8_t)((*p & ~(1u << (fuse & 7))) | (value << (fuse & 7)));
}

/* Value of one fuse; 0 outside the map */
int get_fuse(uint32_t fuse)
{
    if (g.fuses == NULL || (size_t)(fuse >> 3) >= g.fuses_size) {
        return 0;
    }
    return (g.fuses[fuse >> 3] >> (fuse & 7)) & 1;
}

/* Set one position of the '0'/'1' text image of a type 0xff device */
void text_set_fuse(uint32_t pos, unsigned value)
{
    if (g.pla_text != NULL && pos < 0x800) {
        g.pla_text[pos] = (char)(value + '0');
    }
}

/* Write a value into consecutive fuses, low bit first */
static void set_fuse_bits(uint32_t fuse, unsigned value, int count)
{
    for (int i = 0; i < count; i++) {
        set_fuse(fuse++, value & 1);
        value >>= 1;
    }
}

/* As above, but the fuses after the first one are found
 * (value >> 8) positions further on */
static void set_fuse_bits_split(uint32_t fuse, unsigned value, int count)
{
    set_fuse(fuse, value & 1);
    fuse += 1 + (value >> 8);
    value &= 0xff;
    for (int i = 0; i < ((count - 1) & 0xffff) && count != 1; i++) {
        value >>= 1;
        set_fuse(fuse++, value & 1);
    }
}

/* Has a product term already been written to the row starting at 'fuse'? */
static int row_was_written(uint32_t fuse)
{
    for (int i = 0; i < g.row_count; i++) {
        if (g.row_list[i] == fuse) {
            return 1;
        }
    }
    return 0;
}

/* Write the fuses of one product term and remember the row */
void write_fuse_row(const uint8_t *row, uint32_t fuse, int count)
{
    if (g.row_limit < g.row_count + 1) {
        fatal_error(1026, "");
    }
    g.row_list[g.row_count++] = fuse;
    for (int i = 0; i < count; i++) {
        set_fuse(fuse++, row[i] & 1);
    }
}

/* Read a row; returns 1 if every fuse still has the default value */
int read_fuse_row(uint8_t *row, uint32_t fuse, int count)
{
    int untouched = 1;
    for (int i = 0; i < count; i++) {
        row[i] = (uint8_t)get_fuse(fuse++);
        if (row[i] != attr8(ATTR_DEFAULT_FUSE)) {
            untouched = 0;
        }
    }
    return untouched;
}

/* -------------------------------------------------------------------------
 * max5032 expander product terms
 * ------------------------------------------------------------------------- */
enum { EXP_PIN = 0, EXP_ROW_A = 4, EXP_ROW_B = 5, EXP_NEEDED = 6, EXP_FUSE = 7 };

/* Entry of a pin in the expander table; an unknown pin is fatal */
static int expander_index(int pin)
{
    for (int i = 0; i < 32; i++) {
        if (s_expanders[i][EXP_PIN] == pin) {
            return i;
        }
    }
    fatal_error(1027, "corrupt pin number");
}

/* A pin used as an input gives up its two expander rows.  The
 * rows cleared are those of the entry sixteen places further on. */
static void max5_release(int pin)
{
    int i = expander_index(pin);
    if (i + 16 < 32) {
        s_expanders[i + 16][EXP_ROW_A] = 0;
        s_expanders[i + 16][EXP_ROW_B] = 0;
    }
    g.expander_free -= 2;
}

/* Record the number of product terms the output on 'pin' needs */
void max5_set_needed(int pin, int nterms)
{
    s_expander_needed = nterms;
    s_expanders[expander_index(pin)][EXP_NEEDED] = nterms;
}

/* Take the next free expander row; 0 if none is left */
static uint32_t max5_take_row(void)
{
    for (int i = 0; i < 32; i++) {
        if (s_expanders[i][EXP_ROW_A] != 0) {
            uint32_t row = (uint32_t)s_expanders[i][EXP_ROW_A];
            s_expanders[i][EXP_ROW_A] = 0;
            g.expander_free--;
            uint32_t fuse = (uint32_t)s_expanders[i][EXP_FUSE];
            if ((int)attr8(ATTR_NPINS) < s_expanders[i][EXP_PIN]) {
                for (int k = 0; k < row_state.stride; k++) {
                    set_fuse(fuse++, 1);
                }
            }
            return row;
        }
        if (s_expanders[i][EXP_ROW_B] != 0) {
            uint32_t row = (uint32_t)s_expanders[i][EXP_ROW_B];
            s_expanders[i][EXP_ROW_B] = 0;
            g.expander_free--;
            return row;
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * ep312 / ep324 / v2500b macrocells
 * ------------------------------------------------------------------------- */

/* Select the cell table of the device; the LCC packages use other pin
 * numbers */
void select_cell_table(int which, int lcc)
{
    const cell_template_t *src;
    const uint16_t (*pins)[4];
    switch (which) {
    case CELLS_EP312:
        src = k_ep312_cells;
        pins = k_ep312lcc_pins;
        s_ncells = 12;
        break;
    case CELLS_EP324:
        src = k_ep324_cells;
        pins = k_ep324lcc_pins;
        s_ncells = 24;
        break;
    default:
        src = k_v2500b_cells;
        pins = k_v2500blcc_pins;
        s_ncells = 24;
        break;
    }
    memcpy(s_cells, src, (size_t)s_ncells * sizeof(s_cells[0]));
    if (lcc) {
        for (int i = 0; i < s_ncells; i++) {
            s_cells[i].pin = pins[i][0];
            s_cells[i].alt_pin = pins[i][1];
            s_cells[i].neighbor[0] = pins[i][2];
            s_cells[i].neighbor[1] = pins[i][3];
        }
    }
}

/* Cell with the given pin number, else with the given second number */
static cell_template_t *find_cell(unsigned pin)
{
    for (int i = 0; i < s_ncells; i++) {
        if (s_cells[i].pin == pin) {
            return &s_cells[i];
        }
    }
    for (int i = 0; i < s_ncells; i++) {
        if (s_cells[i].alt_pin == pin) {
            return &s_cells[i];
        }
    }
    return NULL;
}

/* Borrow four product terms from a neighbour that needs at most
 * four itself */
static void borrow_terms(cell_template_t *cell, unsigned neighbor_pin)
{
    cell_template_t *from = find_cell(neighbor_pin);
    if (from == NULL || from->needed >= 5) {
        return;
    }
    from->avail = (uint16_t)(from->avail - 4);
    cell->avail = (uint16_t)(cell->avail + 4);
    if (cell->neighbor[0] == from->pin) {
        cell->row_fuse[3] = from->row_fuse[2];
        cell->row_terms[3] = from->row_terms[2];
        from->row_terms[2] = 0;
    } else {
        cell->row_fuse[0] = from->row_fuse[1];
        cell->row_terms[0] = from->row_terms[1];
        from->row_terms[1] -= 4;
    }
}

/* Let every cell that has too few product terms borrow from its neighbours */
static void borrow_all(void)
{
    for (int i = 0; i < s_ncells; i++) {
        if (s_cells[i].avail < s_cells[i].needed) {
            borrow_terms(&s_cells[i], s_cells[i].neighbor[0]);
        }
        if (s_cells[i].avail < s_cells[i].needed) {
            borrow_terms(&s_cells[i], s_cells[i].neighbor[1]);
        }
    }
}

static int array_is_cell_output(const or_array_t *oa)
{
    return oa->sym != NULL && oa->sym->or_index >= 0 && oa->nterms > 0 && node8(oa->sym->node_index, NODE_B25) != 0;
}

/* Distribute the product terms of the ep312 / ep324 */
void ep312_assign_terms(void)
{
    for (int i = 0; i < g.nor_arrays; i++) {
        or_array_t *oa = &g.or_arrays[i];
        if (array_is_cell_output(oa)) {
            cell_template_t *c = find_cell(node16(oa->sym->node_index, NODE_NUMBER));
            if (c != NULL) {
                c->needed = (uint16_t)oa->nterms;
            }
        }
    }
    borrow_all();
    for (int i = 0; i < g.nor_arrays; i++) {
        or_array_t *oa = &g.or_arrays[i];
        if (array_is_cell_output(oa)) {
            cell_template_t *c = find_cell(node16(oa->sym->node_index, NODE_NUMBER));
            if (c != NULL) {
                wr16(node_rec(oa->sym->or_index) + NODE_MAX_TERMS, c->avail);
            }
        }
    }
    for (int i = 2; i < g.nsyms; i++) {
        symbol_t *s = &g.syms[i];
        unsigned type = s->type & 0x0f;
        if (s->eq != NULL && (type == 2 || type == 5 || type == 9 || type == 10) && s->eq->max_terms > 2) {
            cell_template_t *c = find_cell(s->pin);
            if (c != NULL) {
                s->eq->max_terms = c->avail;
            }
        }
    }
}

/* v2500b row sharing between the two flip-flops of a macrocell */
static void v2500b_fix_cells(void)
{
    for (int i = 0; i < g.nor_arrays; i++) {
        or_array_t *oa = &g.or_arrays[i];
        if (!array_is_cell_output(oa) || oa->sym->ext_code != 0) {
            continue;
        }
        unsigned number = node16(oa->sym->node_index, NODE_NUMBER);
        for (int c = 0; c < s_ncells; c++) {
            if (s_cells[c].pin == number && oa->nterms < 9) {
                for (int j = 0; j < g.nor_arrays; j++) {
                    symbol_t *t = g.or_arrays[j].sym;
                    if (t != NULL && s_cells[c].alt_pin == t->pin && t->ext_code == 0) {
                        set_fuse(s_cells[c].sel_fuse[1], 1);
                    }
                }
            }
            if (s_cells[c].pin == number && oa->nterms < 5) {
                for (int j = 0; j < g.nor_arrays; j++) {
                    symbol_t *t = g.or_arrays[j].sym;
                    if (t != NULL && s_cells[c].alt_pin == t->pin && t->ext_code == 0) {
                        set_fuse(s_cells[c].sel_fuse[1], 0);
                    }
                }
            }
            if (s_cells[c].pin == number && oa->nterms > 8) {
                set_fuse(s_cells[c].row_fuse[1], 1);
            }
        }
    }
    for (int i = 0; i < g.nor_arrays; i++) {
        or_array_t *oa = &g.or_arrays[i];
        if (!array_is_cell_output(oa) || (oa->sym->ext_code != 1 && oa->sym->ext_code != 0x1b)) {
            continue;
        }
        unsigned number = node16(oa->sym->node_index, NODE_NUMBER);
        for (int c = 0; c < s_ncells; c++) {
            if (s_cells[c].pin == number) {
                for (int j = 0; j < g.nor_arrays; j++) {
                    symbol_t *t = g.or_arrays[j].sym;
                    if (t != NULL && s_cells[c].alt_pin == t->pin && t->ext_code == 0) {
                        set_fuse(s_cells[c].row_fuse[1], 1);
                        set_fuse(s_cells[c].sel_fuse[1], 0);
                    }
                }
            }
            if (s_cells[c].pin == number && oa->nterms > 8) {
                set_fuse(s_cells[c].row_fuse[1], 1);
            }
        }
    }
    for (int i = 0; i < g.nor_arrays; i++) {
        or_array_t *oa = &g.or_arrays[i];
        if (oa->sym == NULL || oa->sym->or_index < 0 || oa->nterms <= 0) {
            continue;
        }
        unsigned number = node16(oa->sym->node_index, NODE_NUMBER);
        if (number != 0 && (oa->sym->ext_code == 1 || oa->sym->ext_code == 0x1b)) {
            for (int c = 0; c < s_ncells; c++) {
                if (s_cells[c].pin == number) {
                    s_cells[c].neighbor[1] = 1;
                }
            }
        }
    }
    for (int c = 0; c < s_ncells; c++) {
        if (s_cells[c].neighbor[1] == 0) {
            set_fuse(s_cells[c].row_fuse[2], 1);
        }
    }
}

/* -------------------------------------------------------------------------
 * Macrocell configuration
 * ------------------------------------------------------------------------- */

/* Configuration that depends on what drives a macrocell input
 * (clock, enable, ...): the source is the first input of the symbol's first
 * product term, or the pin itself for some extensions. */
static void set_pair_bits(uint32_t fuse, const symbol_t *sym, int count)
{
    unsigned e = node8(sym->node_index, NODE_EXT);
    int by_term = e == '*' || e == '+' || e == ',' || e == ';' || e == '<' || e == '>' || e == '?' || e == 'C'
                  || e == '0';
    unsigned ext = 0;
    unsigned number = 0;
    int found = 0;
    for (int i = 0; i < g.nsyms; i++) {
        if (by_term) {
            if (sym->eq == NULL || !term_bit(TERM_CARE(sym->eq, 0), i)) {
                continue;
            }
            if (found) {
                break;
            }
            int v = term_bit(TERM_VAL(sym->eq, 0), i);
            switch (pin8(g.syms[i].pin_index, PIN_KIND)) {
            case 0:
            case 7:
            case 0x0b:
                ext = 0x17;
                break;
            case 2:
                ext = 0x28;
                break;
            case 3:
                ext = 0x29;
                break;
            case 5:
            case 0x0c:
            case 0x14:
            case 0x15:
            case 0x16:
                if (e == '*' || e == 'C') {
                    ext = v ? 0x0f : 0x27;
                } else if (!v && e == ',') {
                    ext = 0x1e;
                } else {
                    ext = 0x1d;
                }
                break;
            case 9:
            case 0x0d:
                ext = 0x11;
                break;
            case 0x0e:
                if (e == '>') {
                    ext = 7;
                } else if (e == '?') {
                    ext = 5;
                } else if (e == '+') {
                    ext = 0x11;
                }
                break;
            default:
                break;
            }
            number = g.syms[i].pin;
            if ((e == '*' || e == 'C') && attr8(ATTR_NPINS) < number) {
                ext = 0;
            }
            if (e == ';' || e == '<') {
                ext = 0;
            }
            if (g.dev_f7024 && e == '*' && attr8(ATTR_NPINS) < sym->pin) {
                if (sym->pin < 0x47 && number == 0x0d) {
                    set_fuse(0x35d0, 1);
                } else if (sym->pin > 0x46 && number == 0x0d) {
                    set_fuse(0x35d4, 1);
                }
            }
            found = 1;
        } else if (e == 2 || e == 0x0f || e == 0x17) {
            ext = e;
            number = sym->pin;
            break;
        }
    }
    symbol_t *input = g.input_sym_by_pin[sym->pin];
    int rec = find_extension_pair(g.node_sym_by_pin[sym->pin], number, ext);
    if (rec == -1) {
        rec = find_extension_pair(input, number, ext);
    }
    if (rec != -1) {
        set_fuse_bits(fuse, rd8(pair_rec(rec) + PAIR_VALUE), count);
    }
}

/* Set the configuration fuses of the macrocell a list belongs to */
void check_macrocell(int symbol)
{
    const symbol_t *s = &g.syms[symbol];
    if (s->node_index < 0) {
        report_symbol_error(1007, s);
        return;
    }
    const uint8_t *n = node_rec(s->node_index);
    if (n[NODE_ROW_WIDTH] != 0) {
        if (rd16(n + NODE_SEL_VALUE) < 0x100) {
            set_fuse_bits(rd32(n + NODE_ROW_FUSE), rd16(n + NODE_SEL_VALUE), n[NODE_ROW_WIDTH]);
        } else {
            set_fuse_bits_split(rd32(n + NODE_ROW_FUSE), rd16(n + NODE_SEL_VALUE), n[NODE_ROW_WIDTH]);
        }
    }
    if (n[NODE_CFG_BITS] != 0) {
        set_fuse_bits(rd32(n + NODE_CFG_FUSE), n[NODE_CFG_VALUE], n[NODE_CFG_BITS]);
    }
    if (n[NODE_PAIR_BITS] != 0) {
        set_pair_bits(rd32(n + NODE_PAIR_FUSE), s, n[NODE_PAIR_BITS]);
    }
}

/* Does a symbol on the pin have extension code 1 or 0x16? */
static int pin_has_registered_output(unsigned pin)
{
    for (int i = 0; i < g.nsyms; i++) {
        if (g.syms[i].pin == pin && (g.syms[i].ext_code == 1 || g.syms[i].ext_code == 0x16)) {
            return 1;
        }
    }
    return 0;
}

/* Configuration fuses of input pins */
static void set_input_pin_fuses(void)
{
    if (strcmp(g.device_name, "p18v8") == 0) {
        /* An unused output register must be bypassed when its pin is an
         * input. Keep its OE row false and select the combinational mux. */
        for (int i = 0; i < g.nsyms; i++) {
            const symbol_t *s = &g.syms[i];
            if (s->pin_index < 0 || !(s->flags & SYMF_INPUT) || s->ext_code != 0 ||
                g.output_sym_by_pin[s->pin] != NULL) {
                continue;
            }
            for (int n = 0; n < nnodes(); n++) {
                if (node16(n, NODE_NUMBER) == s->pin && node8(n, NODE_EXT) == 0 &&
                    node8(n, NODE_KIND) == 6) {
                    set_fuse_bits(node32(n, NODE_ROW_FUSE), node16(n, NODE_SEL_VALUE),
                                  (int)node8(n, NODE_ROW_WIDTH));
                    break;
                }
            }
        }
    }
    if (device_type() == DEVT_1) {
        return;
    }
    for (int i = 0; i < g.nsyms; i++) {
        const symbol_t *s = &g.syms[i];
        if (s->pin_index < 0 || !(s->flags & SYMF_INPUT) || pin8(s->pin_index, PIN_CFG_BITS) == 0) {
            continue;
        }
        unsigned value = pin8(s->pin_index, PIN_CFG_VALUE);
        if (pin8(s->pin_index, PIN_EXT) == 0x25 && pin8(s->pin_index, PIN_B6) == 0xff
            && pin8(s->pin_index, PIN_CFG_BITS) < 3 && !pin_has_registered_output(s->pin)) {
            value = value == 1 ? 0 : 1;
        }
        set_fuse_bits(pin32(s->pin_index, PIN_CFG_FUSE), value, (int)pin8(s->pin_index, PIN_CFG_BITS));
    }
}

/* -------------------------------------------------------------------------
 * Product terms
 * ------------------------------------------------------------------------- */

/* Node record of the symbol's macrocell with another extension;
 * the kind may be the opposite polarity */
int find_node_by_ext(int symbol, unsigned ext)
{
    const uint8_t *own = node_rec(g.syms[symbol].node_index);
    unsigned array = own[NODE_ARRAY];
    unsigned number = own[NODE_NUMBER];
    unsigned kind = own[NODE_KIND];
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < nnodes(); i++) {
            const uint8_t *n = node_rec(i);
            if (rd16(n + NODE_ARRAY) == array && rd16(n + NODE_NUMBER) == number && n[NODE_KIND] == kind
                && n[NODE_EXT] == ext) {
                return i;
            }
        }
        if (kind == 1) {
            kind = 6;
        } else if (kind == 2) {
            kind = 7;
        } else if (kind == 6) {
            kind = 1;
        } else if (kind == 7) {
            kind = 2;
        }
    }
    return -1;
}

/* Does the device see input 'symbol' inverted?  Used by all routines that
 * blow the fuses of a product term. */
int input_inverted(int symbol, int pin_index)
{
    const symbol_t *s = &g.syms[symbol];
    int node = s->node_index;
    int inv = (s->flags & SYMF_INVERTED) != 0;
    if (node >= 0 && node8(node, NODE_INVERT) != 0) {
        inv = !inv;
    }
    switch (pin8(pin_index, PIN_EXT)) {
    case 0x22:
        if (node >= 0 && node8(find_node_by_ext(symbol, 1), NODE_INVERT) != 0) {
            inv = !inv;
        }
        break;
    case 0x23:
        if (node >= 0 && node8(find_node_by_ext(symbol, 0x1c), NODE_INVERT) != 0) {
            inv = !inv;
        }
        break;
    case 0x24:
        if (node >= 0 && node8(find_node_by_ext(symbol, 0x1b), NODE_INVERT) != 0) {
            inv = !inv;
        }
        break;
    case 0x25:
        if (node >= 0 && node8(node, NODE_INVERT) != 0) {
            inv = !inv;
        }
        break;
    case 0x26:
        if (attr8(ATTR_B30) == 1) {
            inv = 0;
        }
        break;
    default:
        break;
    }
    return inv;
}

/* The term is the constant 0 */
static int term_is_false(const uint16_t *care, const uint16_t *val)
{
    return (term_bit(care, 0) && term_bit(val, 0)) || (term_bit(care, 1) && !term_bit(val, 1));
}

static int term_is_constant(const uint16_t *care)
{
    return term_bit(care, 0) || term_bit(care, 1);
}

static void put_row(uint8_t *row, unsigned col, unsigned value)
{
    if (row_state.text_mode) {
        text_set_fuse(col + row_state.fuse, value);
    } else {
        row[col] = (uint8_t)value;
    }
}

/* Blow the fuses of one product term at row_state.fuse.  'alt'
 * selects the last pin record of a pin instead of the symbol's own. */
static void blow_term(const uint16_t *care, uint16_t *val, int alt, int owner_index)
{
    uint8_t row[ROW_BUF];
    uint8_t old[ROW_BUF];
    const symbol_t *owner = &g.syms[owner_index];
    int stride = row_state.stride;
    if (stride > ROW_MAX) {
        fatal_error(1025, "");
    }
    memset(row, 1, sizeof(row));
    if (row_state.fuse > attr32(ATTR_NFUSES)) {
        return;
    }
    int untouched = read_fuse_row(old, row_state.fuse, stride);
    if (term_is_false(care, val)) {
        uint32_t f = row_state.fuse;
        for (int i = 0; i < stride; i++, f++) {
            if (row_state.text_mode) {
                text_set_fuse(f, 0);
            } else {
                set_fuse(f, 0);
            }
        }
        return;
    }
    if (term_is_constant(care)) {
        write_fuse_row(row, row_state.fuse, stride);
        return;
    }
    for (int i = 0; i < g.nsyms; i++) {
        if (!term_bit(care, i)) {
            continue;
        }
        const symbol_t *s = &g.syms[i];
        if (s->pin_index < 0) {
            report_symbol_error(1008, s);
            continue;
        }
        int pi = s->pin_index;
        if (alt) {
            unsigned number = pin16(pi, PIN_NUMBER);
            for (pi = npins() - 1; pi >= 0; pi--) {
                if (pin16(pi, PIN_NUMBER) == number) {
                    break;
                }
            }
        }
        int inv = input_inverted(i, pi);
        unsigned e = pin8(pi, PIN_EXT);
        if (g.dev_max5 && ((e == 0 && pin8(pi, PIN_KIND) == 7) || e == 0x25)) {
            max5_release((int)pin16(pi, PIN_NUMBER));
        }
        const uint8_t *p = pin_rec(pi);
        if ((stride < p[PIN_COMP_COL] || stride < p[PIN_TRUE_COL]) && g.is_prom_like) {
            /* the input has no column in this row: the row is given up */
            if (row_state.text_mode) {
                text_set_fuse(row_state.fuse + (uint32_t)stride, 0);
            }
            row_state.fuse -= (uint32_t)stride;
            return;
        }
        if (s->node_index >= 0 && node8(s->node_index, NODE_EXT) == 0x0d) {
            put_row(row, p[PIN_TRUE_COL], 0);
        } else if (term_bit(val, i) == inv) {
            put_row(row, p[PIN_TRUE_COL], 1);
            put_row(row, p[PIN_COMP_COL], 0);
            term_clear_bit(val, i);
        } else {
            put_row(row, p[PIN_TRUE_COL], 0);
            put_row(row, p[PIN_COMP_COL], 1);
            term_set_bit(val, i);
        }
    }
    if (g.args[7][0] != '1' && device_type() != DEVT_IFL && !untouched && row_was_written(row_state.fuse)
        && memcmp(row, old, (size_t)stride) != 0) {
        report_symbol_error(ERR_PINNODE_CONFLICT, owner);
    }
    write_fuse_row(row, row_state.fuse, stride);
}

/* Connect the term to the OR arrays of the outputs it drives */
static void blow_or_term(const uint16_t *out)
{
    int type = device_type();
    uint32_t clear = 0;
    uint32_t base = 0;
    if (type == DEVT_PAL) {
        return;
    }
    if (type == DEVT_1 || type == DEVT_2) {
        clear = base = row_state.or_fuse;
    } else if (type == DEVT_IFL || type == DEVT_7 || type == DEVT_8) {
        clear = row_state.fuse + (uint32_t)row_state.and_width;
        base = row_state.fuse;
    }
    for (int i = 0; i < row_state.or_width; i++) {
        set_fuse(clear++, 1);
    }
    for (int i = 0; i < g.nsyms; i++) {
        if (!term_bit(out, i)) {
            continue;
        }
        const symbol_t *s = &g.syms[i];
        if (s->or_index < 0) {
            report_symbol_error(1009, s);
        } else if (node8(s->or_index, NODE_OR_WIDTH) != 0) {
            set_fuse(node32(s->node_index, NODE_OR_FUSE) + base, 0);
        }
    }
}

/* Set all fuses of the row at row_state.fuse to 1 */
static void blow_unused_row(void)
{
    uint32_t f = row_state.fuse;
    for (int i = 0; i < row_state.stride; i++) {
        set_fuse(f++, 1);
    }
}

/* max5032 list with four or more terms: the first two terms use
 * the macrocell's own rows, the others expander rows */
static void max5_blow_list(equation_t *eq)
{
    if (g.expander_free + 1 < s_expander_needed) {
        fatal_error(ERR_EXPANDER_PTERMS, "");
    }
    for (int t = 0; t < eq->nterms; t++) {
        if (TERM_CARE(eq, t)[0] & TERM_DELETED) {
            continue;
        }
        row_state.placed++;
        blow_term(TERM_CARE(eq, t), TERM_VAL(eq, t), 0, eq->symbol);
        blow_or_term(TERM_OUT(eq, t));
        if (t < 2) {
            row_state.fuse += (uint32_t)row_state.stride;
        } else {
            row_state.fuse = max5_take_row();
            if (row_state.fuse == 0) {
                fatal_error(ERR_EXPANDER_PTERMS, "");
            }
        }
    }
    row_state.fuse = max5_take_row();
    /* One more term is placed after the last one, in a further expander
     * row: the spare zero term that read_equation() allocates behind every
     * list.  It uses up the row but leaves it unprogrammed. */
    if (!(TERM_CARE(eq, eq->nterms)[0] & TERM_DELETED)) {
        blow_term(TERM_CARE(eq, eq->nterms), TERM_VAL(eq, eq->nterms), 0, eq->symbol);
    }
}

/* ---- xl78c800 ---- */

/* True-fuse column of the first pin record with the given number, or -1 */
static int pin_true_column(unsigned number)
{
    for (int i = 0; i < npins(); i++) {
        if (pin16(i, PIN_NUMBER) == number) {
            return (int)pin8(i, PIN_TRUE_COL);
        }
    }
    return -1;
}

/* Find a free (or identical) row of a buried node for a product
 * term; returns the column of that node, or -1 */
static int xl_place_row(const uint8_t *row, int count, unsigned own_number)
{
    uint8_t old[ROW_BUF];
    for (int a = 0; a < nnodes(); a++) {
        const uint8_t *na = node_rec(a);
        if (!(attr8(ATTR_NPINS) < rd16(na + NODE_NUMBER) && na[NODE_EXT] == 0 && na[NODE_B25] != 0 && na[0x23] != 0
              && rd16(na + NODE_NUMBER) != own_number)) {
            continue;
        }
        for (int b = 0; b < nnodes(); b++) {
            const uint8_t *nb = node_rec(b);
            if (a == b || rd16(na + NODE_ARRAY) != rd16(nb + NODE_ARRAY)) {
                continue;
            }
            uint32_t fuse = rd32(nb + NODE_ROW_FUSE);
            if (read_fuse_row(old, fuse, count)) {
                write_fuse_row(row, fuse, count);
                return pin_true_column(rd16(na + NODE_NUMBER));
            }
            if (memcmp(row, old, (size_t)count) == 0) {
                return pin_true_column(rd16(na + NODE_NUMBER));
            }
        }
    }
    return -1;
}

/* Bring the value bit of input i to "true column" form: afterwards the bit
 * is set when the complement fuse is the one to keep. */
static void normalise_val(uint16_t *val, int i, int inv)
{
    if (term_bit(val, i) == inv) {
        term_clear_bit(val, i);
    } else {
        term_set_bit(val, i);
    }
}

/* xl78c800 product term.  A term with one input needs no row; the
 * column of that input is returned.  Other terms are placed in a row of a
 * buried node whose column is returned. */
static int xl_blow_term(const uint16_t *care, uint16_t *val, int owner_index)
{
    uint8_t row[ROW_BUF];
    const symbol_t *owner = &g.syms[owner_index];
    int stride = row_state.stride;
    int ncare = 0;
    int col = 0;
    for (int i = 0; i < g.nsyms; i++) {
        ncare += term_bit(care, i);
    }
    if (stride > ROW_MAX) {
        fatal_error(1025, "");
    }
    memset(row, 1, sizeof(row));
    if (term_is_false(care, val)) {
        uint32_t f = row_state.fuse;
        for (int i = 0; i < stride; i++) {
            set_fuse(f++, 0);
        }
        return 0;
    }
    if (term_is_constant(care)) {
        write_fuse_row(row, row_state.fuse, stride);
        return 0;
    }
    for (int i = 0; i < g.nsyms; i++) {
        if (!term_bit(care, i)) {
            continue;
        }
        const symbol_t *s = &g.syms[i];
        if (s->pin_index < 0) {
            report_symbol_error(1008, s);
            continue;
        }
        int inv = input_inverted(i, s->pin_index);
        const uint8_t *p = pin_rec(s->pin_index);
        if (s->node_index >= 0 && node8(s->node_index, NODE_EXT) == 0x0d) {
            row[p[PIN_TRUE_COL]] = 0;
            col = p[PIN_TRUE_COL];
            continue;
        }
        normalise_val(val, i, inv);
        if (ncare < 2) {
            if (ncare == 1) {
                col = term_bit(val, i) == inv ? p[PIN_COMP_COL] : p[PIN_TRUE_COL];
            }
        } else {
            inv = !inv;
            if (term_bit(val, i) == inv) {
                row[p[PIN_TRUE_COL]] = 1;
                row[p[PIN_COMP_COL]] = 0;
                col = p[PIN_COMP_COL];
            } else {
                row[p[PIN_TRUE_COL]] = 0;
                row[p[PIN_COMP_COL]] = 1;
                col = p[PIN_TRUE_COL];
            }
        }
    }
    if (ncare != 1) {
        col = xl_place_row(row, stride, node16(owner->node_index, NODE_NUMBER));
        if (col < 0) {
            report_symbol_error(ERR_TOO_MANY_PTERMS, owner);
            col = 0;
        }
    }
    return col;
}

/* Node record of the same macrocell input with the other kind */
static int other_kind_node(int node)
{
    const uint8_t *own = node_rec(node);
    for (int i = 0; i < nnodes(); i++) {
        const uint8_t *n = node_rec(i);
        if (rd16(n + NODE_ARRAY) == rd16(own + NODE_ARRAY) && rd16(n + NODE_NUMBER) == rd16(own + NODE_NUMBER)
            && n[NODE_EXT] == own[NODE_EXT] && n[NODE_KIND] != own[NODE_KIND]) {
            return i;
        }
    }
    return -1;
}

/* xl78c800 single product term of a kind 6/7 node, placed in the
 * node's own row */
static void xl_blow_single(const uint16_t *care, uint16_t *val, int owner_index)
{
    uint8_t row[ROW_BUF];
    const symbol_t *owner = &g.syms[owner_index];
    int stride = row_state.stride;
    if (stride > ROW_MAX) {
        fatal_error(1025, "");
    }
    if (node8(owner->node_index, NODE_ROW_WIDTH) != 0) {
        int other = other_kind_node(owner->node_index);
        if (other >= 0 && node8(other, NODE_ROW_WIDTH) != 0) {
            const uint8_t *n = node_rec(other);
            if (rd16(n + NODE_SEL_VALUE) < 0x100) {
                set_fuse_bits(rd32(n + NODE_ROW_FUSE), rd16(n + NODE_SEL_VALUE), n[NODE_ROW_WIDTH]);
            } else {
                set_fuse_bits_split(rd32(n + NODE_ROW_FUSE), rd16(n + NODE_SEL_VALUE), n[NODE_ROW_WIDTH]);
            }
        }
    }
    memset(row, 1, sizeof(row));
    if (term_is_false(care, val)) {
        uint32_t f = row_state.fuse;
        for (int i = 0; i < stride; i++) {
            set_fuse(f++, 0);
        }
        return;
    }
    if (!term_is_constant(care)) {
        for (int i = 0; i < g.nsyms; i++) {
            if (!term_bit(care, i)) {
                continue;
            }
            const symbol_t *s = &g.syms[i];
            if (s->pin_index < 0) {
                report_symbol_error(1008, s);
                continue;
            }
            int inv = input_inverted(i, s->pin_index);
            const uint8_t *p = pin_rec(s->pin_index);
            if (rd16(p + PIN_NUMBER) < 0x23) {
                if (s->node_index >= 0 && node8(s->node_index, NODE_EXT) == 0x0d) {
                    row[p[PIN_TRUE_COL]] = 0;
                    continue;
                }
                normalise_val(val, i, inv);
            } else {
                term_clear_bit(val, i);
            }
            inv = !inv;
            if (term_bit(val, i) == inv) {
                row[p[PIN_TRUE_COL]] = 1;
                row[p[PIN_COMP_COL]] = 0;
            } else {
                row[p[PIN_COMP_COL]] = 1;
                row[p[PIN_TRUE_COL]] = 0;
            }
        }
    }
    write_fuse_row(row, row_state.fuse, stride);
}

/* Place all product terms of an OR array */
static void blow_array(or_array_t *oa)
{
    int orn = oa->sym->or_index;
    int split = (int)node8(orn, NODE_SPLIT_TERMS);
    uint32_t xl_base = 0;
    cell_template_t *cell = NULL;
    int part = 0;
    row_state.and_width = (int)node8(orn, NODE_ROW_WIDTH);
    if (row_state.and_width == 0) {
        return;
    }
    row_state.or_width = (int)node8(orn, NODE_OR_WIDTH);
    row_state.stride = row_state.and_width + row_state.or_width;
    row_state.fuse = node32(orn, NODE_ROW_FUSE);
    row_state.second = (uint32_t)(row_state.stride * split) + row_state.fuse;
    row_state.or_fuse = 0;
    if (g.dev_xl78c800) {
        uint32_t f = node32(orn, NODE_ROW_FUSE);
        for (int i = 0; i < row_state.and_width; i++) {
            set_fuse(f++, 1);
        }
        xl_base = node32(orn, NODE_ROW_FUSE);
    }
    if (g.is_prom_like) {
        row_state.second = row_state.fuse;
    }
    switch (device_type()) {
    case DEVT_PAL:
        row_state.stride = row_state.and_width;
        break;
    case DEVT_1:
    case DEVT_2:
        row_state.stride = row_state.and_width;
        row_state.or_fuse = node32(orn, NODE_OR_FUSE);
        break;
    case DEVT_IFL:
    case DEVT_7:
    case DEVT_8:
        row_state.stride = row_state.and_width + row_state.or_width;
        break;
    default:
        break;
    }
    row_state.placed = 0;
    for (equation_t *eq = oa->eq; eq != NULL; eq = eq->next) {
        check_macrocell(eq->symbol);
        if (eq->nterms == 0) {
            continue;
        }
        int alt = 0;
        if (strncmp("f502", g.device_name, 4) == 0 && node8(orn, NODE_B24) == 3) {
            alt = 1;
        } else if (node8(orn, NODE_B24) == 3) {
            row_state.placed++;
            continue;
        }
        if (split != 0 && split < terms_before_split(eq)) {
            row_state.fuse = row_state.second;
        }
        int in_cell = g.dev_ep312 && node8(oa->sym->node_index, NODE_B25) != 0;
        if (in_cell) {
            cell = find_cell(node16(oa->sym->node_index, NODE_NUMBER));
            if (cell == NULL) {
                in_cell = 0;
            }
        }
        if (in_cell) {
            for (int j = 0; j < 4; j++) {
                if (cell->row_terms[j] == 0) {
                    set_fuse(cell->sel_fuse[j], 1);
                }
            }
            for (part = 0; part < 3 && cell->row_terms[part] == 0; part++) {
            }
            row_state.fuse = cell->row_fuse[part];
            set_fuse(cell->sel_fuse[part], 0);
        }
        if (g.dev_max5 && oa->nterms >= 4) {
            max5_blow_list(eq);
            continue;
        }
        for (int t = 0; t < eq->nterms; t++) {
            uint16_t *care = TERM_CARE(eq, t);
            if (!(care[0] & TERM_DELETED)) {
                row_state.placed++;
                if (!g.dev_xl78c800) {
                    blow_term(care, TERM_VAL(eq, t), alt, eq->symbol);
                } else if (eq->nterms == 1 && (node8(oa->sym->node_index, NODE_KIND) == 7
                                               || node8(oa->sym->node_index, NODE_KIND) == 6)) {
                    xl_blow_single(care, TERM_VAL(eq, t), eq->symbol);
                } else {
                    int col = xl_blow_term(care, TERM_VAL(eq, t), eq->symbol);
                    if (xl_base < attr32(ATTR_NFUSES)) {
                        set_fuse(((uint32_t)col & 0xffff) + xl_base, 0);
                    }
                }
                blow_or_term(TERM_OUT(eq, t));
                row_state.fuse += (uint32_t)row_state.stride;
                row_state.or_fuse += (uint32_t)row_state.or_width;
                if (in_cell && --cell->row_terms[part] == 0) {
                    if (part < 3) {
                        part++;
                    }
                    row_state.fuse = cell->row_fuse[part];
                    if (t < eq->nterms - 1) {
                        set_fuse(cell->sel_fuse[part], 0);
                    }
                }
            }
            if ((care[0] & TERM_SPLIT) && !g.is_prom_like) {
                row_state.fuse = row_state.second;
            }
        }
    }
    if (oa->nterms != row_state.placed) {
        fatal_error(1006, "");
    }
    if (row_state.or_width != 0 && (g.options & OPT_D) && device_type() == DEVT_IFL) {
        for (int i = oa->nterms; i < (int)node16(orn, NODE_MAX_TERMS); i++) {
            blow_unused_row();
            row_state.fuse += (uint32_t)row_state.stride;
        }
    }
}

/* -------------------------------------------------------------------------
 * Fuse map
 * ------------------------------------------------------------------------- */

/* Allocate the map and set the default and initial fuse values */
static void init_fuse_map(void)
{
    uint32_t nfuses = attr32(ATTR_NFUSES);
    unsigned dflt = attr8(ATTR_DEFAULT_FUSE);
    if (dflt > 1) {
        report_error(1004, "invalid default JEDEC value");
        dflt = 0;
    }
    int limit = pin_number_limit();
    g.input_sym_by_pin = alloc_zeroed((size_t)limit + 1 + 0x10000, sizeof(symbol_t *), "creating jedec map");
    g.node_sym_by_pin = alloc_zeroed((size_t)limit + 1 + 0x10000, sizeof(symbol_t *), "creating jedec map");
    g.fuses_size = (size_t)(nfuses >> 3) + 2;
    g.fuses = alloc_zeroed(g.fuses_size, 1, "creating jedec map");
    g.row_limit = (int)attr16(ATTR_NROWS);
    g.row_list = alloc_zeroed((size_t)g.row_limit + 1, sizeof(uint32_t), "creating jedec map");
    g.row_count = 0;
    if (g.is_prom_like) {
        g.pla_text = alloc_tracked(0x800, "creating prom map");
        for (uint32_t i = 0; i < 0x700; i++) {
            text_set_fuse(i, dflt);
        }
    }
    for (uint32_t f = 0; f < nfuses; f++) {
        set_fuse(f, dflt);
    }
    unsigned nranges = sec_count(SEC_INIT);
    const uint8_t *r = sec_data(SEC_INIT);
    for (unsigned i = 0; i < nranges; i++, r += 9) {
        uint32_t first = rd32(r);
        uint32_t last = rd32(r + 4);
        unsigned value = r[8];
        if (value < 2 && last < nfuses && first < nfuses) {
            for (uint32_t f = first; f <= last; f++) {
                set_fuse(f, value);
            }
        } else {
            report_error(1005, "invalid initial JEDEC value");
        }
    }
    unsigned ues_bits = attr8(ATTR_UES_BITS);
    if (ues_bits != 0) {
        /* the user signature is taken from the Partno header field */
        int nbytes = (int)ues_bits >> 3;
        uint32_t fuse = attr32(ATTR_UES_FUSE);
        if (nfuses <= fuse || nfuses <= (uint32_t)nbytes + fuse - 1) {
            report_error(1005, "invalid UES JEDEC number");
            return;
        }
        const char *p = g.hdr_strings != NULL ? g.hdr_strings + strlen(g.hdr_strings) + 1 : "";
        if (g.hdr_strings != NULL && p >= g.hdr_strings + g.hdr_strings_len) {
            p = "";
        }
        for (int i = 0; i < nbytes && *p != '\0'; i++, p++) {
            for (int bit = 7; bit >= 0; bit--) {
                set_fuse(fuse++, (unsigned)(*p >> bit) & 1);
            }
        }
    }
    for (int i = 0; i < g.nsyms; i++) {
        symbol_t *s = &g.syms[i];
        if (s->node_index > 0 && node8(s->node_index, NODE_B25) == 1) {
            g.node_sym_by_pin[s->pin] = s;
        }
        if (s->pin_index > 0 && pin8(s->pin_index, PIN_KIND) == 0) {
            g.input_sym_by_pin[s->pin] = s;
        }
    }
}

/* Read one row of the type 0xff text image as a 7-bit value and
 * the mask of the bits that are programmed */
static void text_row_pattern(unsigned *value, unsigned *mask, int row)
{
    const char *p = g.pla_text + row * 14 + 13;
    *value = 0;
    *mask = 0;
    for (int i = 0; i < 7; i++, p -= 2) {
        char hi = p[0];
        char lo = p[-1];
        int bit = -1;
        if (lo == '0' && hi == '1') {
            bit = 1;
        } else if (lo == '1' && hi == '0') {
            bit = 0;
        }
        *mask <<= 1;
        *value <<= 1;
        if (bit >= 0) {
            *mask |= 1;
            *value |= (unsigned)bit;
        }
    }
}

/* Expand the text image into the PROM-style array of an output */
static void expand_text_rows(uint32_t base)
{
    for (int row = 0; row < 0x80; row++) {
        unsigned value, mask;
        text_row_pattern(&value, &mask, row);
        if (mask == 0) {
            continue;
        }
        for (unsigned addr = 0; addr < 0x80; addr++) {
            if ((addr & mask) == value) {
                set_fuse(addr * 0x15 + base, 1);
            }
        }
    }
}

static int array_in_use(const or_array_t *oa)
{
    return oa->sym != NULL && oa->sym->or_index >= 0 && oa->nterms > 0;
}

/* xl78c800: arrays of buried nodes are placed before the others */
static int xl_is_buried(const or_array_t *oa)
{
    int node = oa->sym->node_index;
    unsigned e = node8(node, NODE_EXT);
    return (e == '@' || e == 0) && node8(node, NODE_B25) != 0;
}

/* Build the fuse map of the device (once per design) */
void build_fuse_map(void)
{
    if (g.map_built || device_type() == DEVT_VIRTUAL || device_type() == DEVT_8) {
        return;
    }
    init_fuse_map();
    if (g.dev_xl78c800) {
        for (int i = 0; i < g.nor_arrays; i++) {
            or_array_t *oa = &g.or_arrays[i];
            if (array_in_use(oa) && xl_is_buried(oa) && attr8(ATTR_NPINS) < node16(oa->sym->node_index, NODE_NUMBER)) {
                blow_array(oa);
            }
        }
    }
    if (g.dev_ep1800) {
        int bad = 0;
        for (int i = 0; i < g.nor_arrays; i++) {
            or_array_t *oa = &g.or_arrays[i];
            if (array_in_use(oa) && ep1800_check_array(oa) == 1) {
                bad++;
            }
        }
        if (bad != 0) {
            abort_phase();
        }
    }
    if (g.dev_v5000) {
        v5000_build();
    }
    for (int i = 0; i < g.nor_arrays; i++) {
        or_array_t *oa = &g.or_arrays[i];
        uint32_t text_base = 0;
        if (g.dev_xl78c800 && array_in_use(oa) && xl_is_buried(oa)) {
            continue;
        }
        int text_array = g.is_prom_like && oa->sym != NULL && oa->sym->or_index >= 0
                         && node8(oa->sym->or_index, NODE_B24) == 0xff;
        if (text_array) {
            for (uint32_t k = 0; k < 0x700; k++) {
                text_set_fuse(k, 0);
            }
            text_base = node32(oa->sym->or_index, NODE_OR_FUSE);
            row_state.text_mode = 1;
        }
        if (!g.dev_v5000 && array_in_use(oa)) {
            blow_array(oa);
        }
        if (text_array) {
            expand_text_rows(text_base);
            row_state.text_mode = 0;
        }
    }
    set_input_pin_fuses();
    if (g.dev_v2500b) {
        v2500b_fix_cells();
    }
    /* fuses given with FUSE() statements, collected by cuplx */
    FILE *fp = open_file("fuse.tmp", "r");
    if (fp != NULL) {
        uint8_t rec[5];
        while (fread(rec, 4, 1, fp) != 0) {
            rec[4] = 0;
            if (fread(rec + 4, 1, 1, fp) == 0) {
                rec[4] = 0;
            }
            set_fuse(rd32(rec), rec[4]);
            trace_printf("WARNING: Fuse %lu is blown to %x\n", (unsigned long)rd32(rec), (unsigned)rec[4]);
        }
        close_file(fp);
        remove_file("fuse.tmp");
    }
    g.map_built = 1;
}

/* -------------------------------------------------------------------------
 * JEDEC file
 * ------------------------------------------------------------------------- */
static const char *const k_header_labels[8] = {
    "Name", "Partno", "Revision", "Date", "Designer", "Company", "Assembly", "Location"
};

const char *header_string(int index)
{
    const char *p = g.hdr_strings;
    if (p == NULL) {
        return "";
    }
    const char *end = p + g.hdr_strings_len;
    for (int i = 0; i < index && p < end; i++) {
        p += strlen(p) + 1;
    }
    return p < end ? p : "";
}

static void jed_text(const char *text)
{
    s_transmit_sum = jedsum_text(s_transmit_sum, text);
    out_puts(text, s_jed_fp);
}

/* Formatted JEDEC field, converted to upper case */
static void jed_field(const char *fmt, unsigned long value)
{
    char text[128];
    snprintf(text, sizeof(text), fmt, value);
    for (char *p = text; *p != '\0'; p++) {
        if (*p >= 'a' && *p <= 'z') {
            *p = (char)(*p - 'a' + 'A');
        }
    }
    jed_text(text);
}

static void banner_lines(void (*emit)(const char *), FILE *fp, const char *prefix, const char *first_fmt)
{
    char text[600];
    char line[512];
    snprintf(line, sizeof(line), first_fmt, PROGRAM_NAME, PROGRAM_VERSION);
    snprintf(text, sizeof(text), "%s%s", line[0] == '\n' ? "" : prefix, line);
    emit != NULL ? emit(text) : out_puts(text, fp);
    snprintf(text, sizeof(text), "%s%-16s%s  Library %s-%d-%d\n", prefix, "Device", (const char *)g.lib.dir_entry,
             (const char *)g.lib.header + 0x15, (int)rd16(g.lib.header + 2), (int)attr16(0));
    emit != NULL ? emit(text) : out_puts(text, fp);
    snprintf(text, sizeof(text), "%s%-16s%s", prefix, "Created", creation_time_text());
    emit != NULL ? emit(text) : out_puts(text, fp);
    for (int i = 0; i < 8; i++) {
        snprintf(text, sizeof(text), "%s%-16s%s\n", prefix, k_header_labels[i], header_string(i));
        emit != NULL ? emit(text) : out_puts(text, fp);
    }
}

/* Design header as written to the .doc and other files */
void print_banner(FILE *fp)
{
    banner_lines(NULL, fp, "", "%-16s%s\n");
}

/* The same header as a comment block, each line starting with 'prefix' */
void print_banner_with(FILE *fp, const char *prefix)
{
    banner_lines(NULL, fp, prefix, "%-16s%s\n");
}

/* STX, the design header and the QP, QF, G and F fields */
static void write_jedec_header(void)
{
    out_putc(2, s_jed_fp);
    s_transmit_sum = jedsum_char(s_transmit_sum, 2);
    banner_lines(jed_text, NULL, "", "\n%-16s%s\n");
    jed_field("*QP%u \n", attr8(ATTR_NPINS));
    jed_field("*QF%lu \n", attr32(ATTR_NFUSES));
    jed_field((g.options & OPT_G) ? "*G1 \n" : "*G0 \n", 0);
    jed_field("*F%1d \n", 0);
}

/* First blown fuse at or after 'fuse'; count + 1 if there is none */
static uint32_t next_blown_fuse(uint32_t fuse, uint32_t count)
{
    for (;; fuse++) {
        if (count < fuse) {
            return count + 1;
        }
        if (get_fuse(fuse) == 1) {
            return fuse;
        }
    }
}

/* One L field: "*Lnnnnn " and up to 'width' fuses */
static void write_fuse_line(uint32_t fuse, int width, uint32_t count)
{
    char text[32];
    snprintf(text, sizeof(text), "*L%05lu ", (unsigned long)fuse);
    jed_text(text);
    for (int i = 0; i < width && fuse <= count - 1; i++, fuse++) {
        int c = get_fuse(fuse) + '0';
        out_putc(c, s_jed_fp);
        s_transmit_sum = jedsum_char(s_transmit_sum, c);
    }
    out_putc('\n', s_jed_fp);
    s_transmit_sum = jedsum_char(s_transmit_sum, '\n');
}

/* 32 fuses per line; lines without a blown fuse are left out */
static void write_fuse_lines(void)
{
    uint32_t count = attr32(ATTR_NFUSES);
    uint32_t fuse = 0;
    while (fuse < count) {
        uint32_t next = next_blown_fuse(fuse, count);
        if (count <= next) {
            return;
        }
        fuse += ((next - fuse) >> 5) * 0x20;
        write_fuse_line(fuse, 0x20, count);
        fuse += 0x20;
    }
}

/* Fuse checksum over the whole fuse map */
static void add_fuse_sum(uint32_t count)
{
    for (uint32_t f = 0; f < count; f++) {
        jedsum_fuses_add(&s_fuse_sum, get_fuse(f) == 1);
    }
    jedsum_fuses_flush(&s_fuse_sum);
}

/* Fuse lines, fuse checksum, ETX and transmission checksum */
static void write_jedec_fuses(void)
{
    write_fuse_lines();
    add_fuse_sum(attr32(ATTR_NFUSES));
    jed_field("*C%04x\n*", jedsum_fuses_value(&s_fuse_sum));
    out_putc(3, s_jed_fp);
    s_transmit_sum = jedsum_char(s_transmit_sum, 3);
    if (g.options & OPT_LIST) {
        out_printf(g.lst_fp, "Jedec Fuse Checksum       (%04x)\n", jedsum_fuses_value(&s_fuse_sum));
        out_printf(g.lst_fp, "Jedec Transmit Checksum   (%04x)\n", s_transmit_sum);
    }
    jed_field("%04x", s_transmit_sum);
}

/* Create the .jed file, named after the design unless the 'n' option is
 * given */
static void open_jedec(void)
{
    if (!(g.options & OPT_N)) {
        if (strcmp(g.design_name, "no_name") == 0) {
            report_warning(ERR_NO_NAME, "jed");
        }
        s_jed_name = make_path(source_dir(g.args[5]), g.design_name, ".jed");
    } else {
        s_jed_name = make_path(source_dir(g.args[5]), g.args[5], ".jed");
    }
    s_jed_fp = open_file(s_jed_name, "wt");
    if (s_jed_fp == NULL) {
        fatal_error(ERR_OPEN, s_jed_name);
    }
}

/* Write the JEDEC file of the current design */
void write_jedec(void)
{
    if (device_type() == DEVT_VIRTUAL) {
        return;
    }
    open_jedec();
    write_jedec_header();
    check_write(s_jed_fp, s_jed_name);
    build_fuse_map();
    write_jedec_fuses();
    check_write(s_jed_fp, s_jed_name);
    close_file(s_jed_fp);
    s_jed_fp = NULL;
}
