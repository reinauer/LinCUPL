/*
 * sim_jedload - the design taken from a JEDEC fuse map instead of an
 * absolute file (option f): <design>.jed is decoded with the library entry
 * of its device into the symbols and product term rows that load_design()
 * would read from <design>.abs, so that the binder and the simulation run
 * on it unchanged.
 *
 * Supported are the GAL16V8 and GAL20V8 in their three modes and the
 * GAL22V10 (devices whose node records are OR arrays, combinatorial and D
 * register outputs, output enable terms and reset / preset terms).
 *
 * How the library describes the fuses (see also src/cupl/fusemap.h and
 * build_fuse_map() in src/cupl/jedec.c, which this reverses):
 *   - a pin record gives the two columns of a pin in every product term
 *     row: the fuse of the true and of the complement input;
 *   - a node record of type "OR array" gives the first fuse, the fuses per
 *     row and the number of rows of an array;
 *   - the other node records are what a pin can be: each names its array
 *     and the configuration fuses that select it (polarity, registered or
 *     combinatorial).  The record whose fuses match is the macrocell as
 *     programmed;
 *   - the connection table ties an output to the node records of its
 *     output enable, reset and preset terms, as the binder uses it.
 *
 * Part of the CUPL simulator (see sim.c).
 */
#include "sim.h"
#include "jedread.h"

/* Offsets in the records of the device (names as in src/cupl/fusemap.h) */
enum {
    HDR_PINS = 0x08,            /* u8 */
    HDR_TYPE = 0x11,            /* u8: 0 = AND array with fixed OR arrays */
    HDR_FUSES = 0x15,           /* u32 */
    HDR_ARRAYS = 0x27,          /* u16: size of the term table */
    HDR_UES_BITS = 0x3d,        /* u8 */
    HDR_UES_FUSE = 0x3e,        /* u32 */
    PIN_NUMBER = 0x02,          /* u16 */
    PIN_TRUE_COL = 0x07,        /* u8 */
    PIN_COMP_COL = 0x08,        /* u8 */
    NODE_ARRAY = 0x00,          /* u16 */
    NODE_NUMBER = 0x02,         /* u16 */
    NODE_TYPE = 0x04,           /* u8: NT_xxx */
    NODE_SEL_VALUE = 0x07,      /* u16 */
    NODE_CFG_VALUE = 0x09,      /* u8 */
    NODE_ROW_FUSE = 0x0a,       /* u32: first fuse of an array; of the select fuses otherwise */
    NODE_CFG_FUSE = 0x12,       /* u32 */
    NODE_ROW_WIDTH = 0x1b,      /* u8: fuses per row; number of select fuses otherwise */
    NODE_CFG_BITS = 0x1d,       /* u8 */
    NODE_MAX_TERMS = 0x1f,      /* u16 */
    SEC_INIT = 9,               /* 9-byte records: first fuse, last fuse (u32), value */
    INIT_SIZE = 9,
    SEC_CONNECTIONS = 10,       /* 13-byte records, see bind.c */
    CONN_SIZE = 13
};

/* Types of node records */
enum {
    NT_COMBINATORIAL = 0x00,
    NT_REGISTER = 0x01,
    NT_ENABLE = 0x02,           /* output enable term */
    NT_PRESET = 0x03,           /* synchronous preset term */
    NT_RESET = 0x07,            /* asynchronous reset term */
    NT_ARRAY = 0x0e
};

enum { PIN_KIND_REGISTER_FEEDBACK = 11, PIN_KIND_ENABLE = 9 };

/* Symbol kinds and flags as cupl writes them (src/shared/absdump.c) */
enum {
    KIND_PIN = 2, KIND_PIN_EXT = 5, KIND_CONST0 = 6, KIND_CONST1 = 7,
    SYMF_INPUT = 0x80, SYMF_OUTPUT = 0x40, SYMF_LOW = 0x10
};

#define MAX_PINS 255
#define MAX_COLUMNS 128             /* column pairs of a product term row */
#define NAME_SIZE 0x20

/* The devices a fuse map is taken for when neither csim nor the file names
 * one, by the number of fuses, in the order they are tried; ptd_fuse is the
 * first product term disable fuse.  The PLCC package of the GAL20V8 and the
 * GAL22V10 has 28 pins and counts them differently: its devices are for a
 * file that says so (QP28). */
static const struct {
    uint32_t fuses;
    uint8_t pins;               /* only for a file with this QP field; 0: for any */
    const char *device;
    uint32_t ptd_fuse;
} k_families[] = {
    { 2706, 28, "g20v8lcc", 2640 },
    { 2706, 28, "g20v8alcc", 2640 },
    { 5892, 28, "g22v10lcc", 0 },
    { 5828, 28, "g22v10lcc", 0 },
    { 2194, 0, "g16v8", 2128 },
    { 2194, 0, "g16v8a", 2128 },
    { 2706, 0, "g20v8", 2640 },
    { 2706, 0, "g20v8a", 2640 },    /* Atmel.dl has the simple mode only as g20v8as */
    { 5892, 0, "g22v10", 0 },
    { 5828, 0, "g22v10", 0 }        /* 22V10 without the 64 signature fuses */
};

typedef struct {
    int16_t in_rec;             /* pin record of the pin as an input, -1 none */
    int16_t reg_rec;            /* pin record of the register feedback, -1 none */
    int16_t column;             /* its column pair, -1 none */
    int16_t node;               /* node record of the macrocell as programmed, -1: no output */
    int16_t enable;             /* node records of the output enable, */
    int16_t reset;              /* reset */
    int16_t preset;             /* and preset term, -1 none */
    int16_t sym;                /* symbol of the pin */
    int16_t out_sym;            /* symbol the product terms drive */
    int16_t enable_sym;
    int16_t reset_sym;
    int16_t preset_sym;
    uint8_t named;              /* the name is from the .pld file */
    uint8_t low;                /* and declared active low (!name) */
    char name[NAME_SIZE];       /* P<number> unless named */
} jpin_t;

typedef struct {
    uint8_t true_col;
    uint8_t comp_col;
    uint16_t pin;
    uint8_t used;               /* some product term has the input */
} column_t;

static struct {
    uint8_t *fuses;             /* one byte per fuse */
    uint32_t nfuses;
    unsigned qp;                /* the QP field (number of pins), 0 if the file has none */
    char device[DL_ENTRY_NAME_SIZE + 1]; /* the device the file names, in lower case, "" if none */
    uint32_t ptd_fuse;          /* 0: the device has no product term disable fuses */
    int npins;
    jpin_t pins[MAX_PINS + 1];  /* by pin number */
    column_t columns[MAX_COLUMNS];
    int ncolumns;
    char *pld_name;             /* the .pld file pin names were taken from, NULL none */
} g_jed;

/* What was read from <design>.pld (see "Pin names from the design file") */
#define PLD_MAX_TEXT 0x100000       /* characters of the file that are looked at */
#define PLD_MAX_LIST 64             /* entries of a pin or name list */
#define PLD_MAX_PINS 512            /* pin declarations */

typedef struct {
    char name[NAME_SIZE];
    uint8_t low;                /* !name */
} pld_name_t;

typedef struct {
    int number;
    pld_name_t name;
} pld_pin_t;

typedef struct pld_field {
    char name[NAME_SIZE];
    int count;
    pld_name_t *members;
    struct pld_field *next;
} pld_field_t;

static struct {
    char *file;                 /* name of the file, NULL if there is none */
    char device[DL_ENTRY_NAME_SIZE + 1]; /* its DEVICE in lower case, "" if none */
    pld_pin_t *pins;            /* pin declarations in the order of the file */
    int npins;
    pld_field_t *fields;        /* FIELD statements, sorted by name */
    int use_names;              /* the simulated device has these pin numbers */
} g_pld;

/* -------------------------------------------------------------------------
 * Fuses and device records
 * ------------------------------------------------------------------------- */
static int fuse(uint32_t n)
{
    return n < g_jed.nfuses ? g_jed.fuses[n] : 0;
}

/* The value of `count` fuses from `first` on, lowest fuse in bit 0 */
static unsigned fuse_bits(uint32_t first, int count)
{
    unsigned value = 0;
    for (int i = 0; i < count && i < 16; i++) {
        value |= (unsigned)fuse(first + (uint32_t)i) << i;
    }
    return value;
}

static int node_count(void)
{
    return dev_section_count(DEV_SEC_NODES);
}

/* Node record of type NT_ARRAY for array `array`, NULL if there is none */
static const uint8_t *array_record(int array)
{
    for (int i = 0; i < node_count(); i++) {
        const uint8_t *n = dev_node(i);
        if (n[NODE_TYPE] == NT_ARRAY && (int)rd16(n + NODE_ARRAY) == array) {
            return n;
        }
    }
    return NULL;
}

/* Is the row starting at `first` always false?  It is if its product term
 * disable fuse is intact, or if both fuses of an input are intact (the
 * input and its complement are both in the product). */
static int row_is_false(uint32_t first, int width)
{
    if (g_jed.ptd_fuse != 0 && width > 0 && fuse(g_jed.ptd_fuse + first / (uint32_t)width) == 0) {
        return 1;
    }
    for (int i = 0; i < g_jed.ncolumns; i++) {
        const column_t *c = &g_jed.columns[i];
        if (c->true_col < width && c->comp_col < width && fuse(first + c->true_col) == 0 &&
            fuse(first + c->comp_col) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Number of rows of `array` up to its last row that is not always false */
static int array_rows(int array)
{
    const uint8_t *a = array_record(array);
    int rows = 0;
    if (a != NULL) {
        uint32_t first = rd32(a + NODE_ROW_FUSE);
        int width = a[NODE_ROW_WIDTH];
        int terms = (int)rd16(a + NODE_MAX_TERMS);
        for (int r = 0; r < terms; r++) {
            if (!row_is_false(first + (uint32_t)(r * width), width)) {
                rows = r + 1;
            }
        }
    }
    return rows;
}

/* The node record of type `type` that the connection table relates to the
 * output node `node` (same pin, same group, other member), -1 if none. */
static int related_node(int node, int type)
{
    const uint8_t *own = dev_node(node);
    const uint8_t *conn = dev_section(SEC_CONNECTIONS);
    int count = dev_section_count(SEC_CONNECTIONS);
    unsigned number = rd16(own + NODE_NUMBER);
    for (int i = 0; i < count; i++) {
        const uint8_t *c = conn + (size_t)i * CONN_SIZE;
        if (rd16(c + 2) != number || c[5] != own[NODE_TYPE] || rd16(c) != rd16(own + NODE_ARRAY)) {
            continue;
        }
        for (int j = 0; j < count; j++) {
            const uint8_t *r = conn + (size_t)j * CONN_SIZE;
            if (rd16(r + 7) != rd16(c + 7) || rd16(r + 9) == rd16(c + 9) || r[5] != type || r[10] == 1 ||
                rd16(r + 2) != number) {
                continue;
            }
            for (int k = 0; k < node_count(); k++) {
                const uint8_t *n = dev_node(k);
                if (n[NODE_TYPE] == type && rd16(n + NODE_NUMBER) == number && rd16(n + NODE_ARRAY) == rd16(r)) {
                    return k;
                }
            }
        }
    }
    return -1;
}

/* -------------------------------------------------------------------------
 * The device
 * ------------------------------------------------------------------------- */

/* Do the fuses agree with the single fuses the library entry fixes?  These
 * are the mode fuses (SYN and AC0 of the GAL16V8 and GAL20V8): the entry
 * of each mode gives them the values that select it. */
static int mode_fuses_match(void)
{
    const uint8_t *init = dev_section(SEC_INIT);
    int count = dev_section_count(SEC_INIT);
    for (int i = 0; i < count; i++, init += INIT_SIZE) {
        uint32_t first = rd32(init);
        if (first == rd32(init + 4) && first < g_jed.nfuses && fuse(first) != init[8]) {
            return 0;
        }
    }
    return 1;
}

/* Can the loaded library entry be decoded here, and is the fuse map one of
 * this device?  A map may leave out a signature at its end.  Returns 0 and
 * the error if not. */
static int check_device(const char *device, const char *jed_name, int *code, char *text, size_t text_size)
{
    const uint8_t *hdr = dev_header();
    uint32_t fuses = rd32(hdr + HDR_FUSES);
    int known = 0;
    int ok = hdr[HDR_TYPE] == 0 && hdr[HDR_PINS] <= MAX_PINS;
    g_jed.ptd_fuse = 0;
    for (size_t i = 0; i < sizeof(k_families) / sizeof(k_families[0]); i++) {
        if (k_families[i].fuses == fuses) {
            known = 1;
            g_jed.ptd_fuse = k_families[i].ptd_fuse;
        }
    }
    for (int i = 0; ok && i < node_count(); i++) {
        switch (dev_node(i)[NODE_TYPE]) {
        case NT_COMBINATORIAL: case NT_REGISTER: case NT_ENABLE: case NT_PRESET: case NT_RESET: case NT_ARRAY:
            break;
        default:
            ok = 0;
            break;
        }
    }
    if (!ok || !known) {
        *code = ERR_JEDEC_DEVICE;
        snprintf(text, text_size, "%s", device);
        return 0;
    }
    if (g_jed.nfuses != fuses &&
        !(hdr[HDR_UES_BITS] != 0 && g_jed.nfuses == rd32(hdr + HDR_UES_FUSE) &&
          g_jed.nfuses + hdr[HDR_UES_BITS] == fuses)) {
        *code = ERR_JEDEC_FILE;
        snprintf(text, text_size, "%s has %lu fuses, device %s has %lu", jed_name, (unsigned long)g_jed.nfuses,
                 device, (unsigned long)fuses);
        return 0;
    }
    return 1;
}

/* Load the library entry `device` into g_dev_rec, or, if it lists entries
 * for its modes (g16v8: g16v8ms, g16v8ma and g16v8s), the one of them whose
 * mode fuses the map has.  Returns 1 and the name of the loaded entry in
 * `entry`, or 0 and the error if the map cannot be simulated with it. */
static int try_device(const char *device, const char *jed_name, char *entry, int *code, char *text, size_t text_size)
{
    int alternatives;
    if (strlen(device) > DL_ENTRY_NAME_SIZE || !device_in_library(device)) {
        *code = ERR_NO_DEVICE;
        snprintf(text, text_size, "%s", device);
        return 0;
    }
    snprintf(entry, DL_ENTRY_NAME_SIZE + 1, "%s", device);
    load_device_entry(device);
    alternatives = dev_section_count(DEV_SEC_ALIASES);
    if (alternatives > 0) {
        char (*names)[0x17] = alloc_tracked((size_t)alternatives * sizeof(*names), "device library processing");
        int found = 0;
        for (int i = 0; i < alternatives; i++) {
            memcpy(names[i], dev_section(DEV_SEC_ALIASES) + (size_t)i * 0x16, 0x16);
            names[i][0x16] = '\0';
        }
        for (int i = 0; i < alternatives && !found; i++) {
            if (names[i][0] == '\0' || strlen(names[i]) > DL_ENTRY_NAME_SIZE || !device_in_library(names[i])) {
                continue;
            }
            load_device_entry(names[i]);
            if (g_jed.nfuses <= rd32(dev_header() + HDR_FUSES) && mode_fuses_match()) {
                snprintf(entry, DL_ENTRY_NAME_SIZE + 1, "%s", names[i]);
                found = 1;
            }
        }
        free_tracked(names);
        if (!found) {
            *code = ERR_JEDEC_FILE;
            snprintf(text, text_size, "the mode fuses of %s select no mode of device %s", jed_name, device);
            return 0;
        }
    }
    return check_device(entry, jed_name, code, text, text_size);
}

/* Choose the device the fuse map is simulated as and load its library
 * entry: the device given to csim; else the device of the .pld file, if
 * there is one and the map can be a map of it; else the device the JEDEC
 * file names, on the same condition; else the device that has this number
 * of fuses and, if the file says how many, of pins.  The names of the .pld
 * file are for the pin numbers of its device (a PLCC package counts them
 * differently), so they are used only with a device that has as many
 * pins. */
static void choose_device(const char *jed_name)
{
    char entry[DL_ENTRY_NAME_SIZE + 1] = "";
    char text[200] = "";
    int code = 0;
    int pld_pins = 0;           /* pins of the .pld file's device, 0 if the map is not for it */
    if (g_pld.device[0] != '\0' && try_device(g_pld.device, jed_name, entry, &code, text, sizeof(text)) &&
        mode_fuses_match()) {
        pld_pins = dev_header()[HDR_PINS];
    }
    if (g_args[4][0] != '&' && g_args[4][0] != '\0') {
        if (!try_device(g_args[4], jed_name, entry, &code, text, sizeof(text))) {
            fatal_error(code, text);
        }
        g_pld.use_names = g_pld.device[0] == '\0' || pld_pins == dev_header()[HDR_PINS];
    } else if (pld_pins != 0) {
        g_pld.use_names = 1;    /* its entry is the one loaded */
    } else {
        char first_text[200] = "";
        int first_code = -1;
        int found = g_jed.device[0] != '\0' &&
                    try_device(g_jed.device, jed_name, entry, &code, text, sizeof(text)) && mode_fuses_match();
        for (size_t i = 0; i < sizeof(k_families) / sizeof(k_families[0]) && !found; i++) {
            if (k_families[i].fuses != g_jed.nfuses || (k_families[i].pins != 0 && k_families[i].pins != g_jed.qp)) {
                continue;
            }
            found = try_device(k_families[i].device, jed_name, entry, &code, text, sizeof(text));
            if (!found && first_code < 0) {
                first_code = code;
                snprintf(first_text, sizeof(first_text), "%s", text);
            }
        }
        if (!found && first_code < 0) {
            snprintf(text, sizeof(text), "%s has %lu fuses (GAL16V8: 2194, GAL20V8: 2706, GAL22V10: 5892 or 5828)",
                     jed_name, (unsigned long)g_jed.nfuses);
            fatal_error(ERR_JEDEC_NO_DEVICE, text);
        }
        if (!found) {
            fatal_error(first_code, first_text);
        }
        g_pld.use_names = g_pld.device[0] == '\0';
    }
    if (!mode_fuses_match()) {
        print_warning_now(ERR_JEDEC_MODE, entry);
    }
    snprintf(g_device_name, sizeof(g_device_name), "%s", entry);
}

/* -------------------------------------------------------------------------
 * Pins and macrocells
 * ------------------------------------------------------------------------- */

/* Pin records and column pairs of every pin; default names */
static void collect_pins(void)
{
    int count = dev_section_count(DEV_SEC_PINS);
    g_jed.npins = dev_header()[HDR_PINS];
    g_jed.ncolumns = 0;
    for (int p = 0; p <= MAX_PINS; p++) {
        jpin_t *pin = &g_jed.pins[p];
        memset(pin, 0, sizeof(*pin));
        pin->in_rec = pin->reg_rec = pin->column = pin->node = -1;
        pin->enable = pin->reset = pin->preset = -1;
        pin->sym = pin->out_sym = pin->enable_sym = pin->reset_sym = pin->preset_sym = -1;
        snprintf(pin->name, sizeof(pin->name), "P%d", p);
    }
    for (int i = 0; i < count; i++) {
        const uint8_t *rec = dev_pin(i);
        int number = (int)rd16(rec + PIN_NUMBER);
        jpin_t *pin;
        if (number < 1 || number > g_jed.npins) {
            continue;
        }
        pin = &g_jed.pins[number];
        if (rec[PIN_KIND] == PIN_KIND_REGISTER_FEEDBACK) {
            if (pin->reg_rec < 0) {
                pin->reg_rec = (int16_t)i;
            }
        } else if (pin->in_rec < 0) {
            pin->in_rec = (int16_t)i;
        }
        if (rec[PIN_TRUE_COL] != rec[PIN_COMP_COL] && pin->column < 0) {
            column_t *c;
            if (g_jed.ncolumns >= MAX_COLUMNS) {
                fatal_error(ERR_JEDEC_DEVICE, g_device_name);
            }
            c = &g_jed.columns[g_jed.ncolumns];
            c->true_col = rec[PIN_TRUE_COL];
            c->comp_col = rec[PIN_COMP_COL];
            c->pin = (uint16_t)number;
            c->used = 0;
            pin->column = (int16_t)g_jed.ncolumns++;
        }
    }
}

/* Every fuse of a row must be the input of a pin; and mark the inputs that
 * some product term uses. */
static void scan_arrays(void)
{
    for (int i = 0; i < node_count(); i++) {
        const uint8_t *a = dev_node(i);
        uint32_t first;
        int width, terms;
        if (a[NODE_TYPE] != NT_ARRAY) {
            continue;
        }
        first = rd32(a + NODE_ROW_FUSE);
        width = a[NODE_ROW_WIDTH];
        terms = (int)rd16(a + NODE_MAX_TERMS);
        if (width != 2 * g_jed.ncolumns) {
            fatal_error(ERR_JEDEC_DEVICE, g_device_name);
        }
        for (int c = 0; c < g_jed.ncolumns; c++) {
            if (g_jed.columns[c].true_col >= width || g_jed.columns[c].comp_col >= width) {
                fatal_error(ERR_JEDEC_DEVICE, g_device_name);
            }
        }
        for (int r = 0; r < terms; r++) {
            uint32_t row = first + (uint32_t)(r * width);
            if (row_is_false(row, width)) {
                continue;
            }
            for (int c = 0; c < g_jed.ncolumns; c++) {
                if (fuse(row + g_jed.columns[c].true_col) == 0 || fuse(row + g_jed.columns[c].comp_col) == 0) {
                    g_jed.columns[c].used = 1;
                }
            }
        }
    }
}

/* What the fuses make of every macrocell.  A pin is an output if the
 * select and configuration fuses of one of its node records have the
 * values of that record.  A macrocell without a product term whose output
 * enable term is always false is an input (it is an unused macrocell, or
 * one that a compiler turned off for a pin used as an input), unless it is
 * a register that product terms read back. */
static void configure_macrocells(void)
{
    for (int i = 0; i < node_count(); i++) {
        const uint8_t *n = dev_node(i);
        int number = (int)rd16(n + NODE_NUMBER);
        jpin_t *pin;
        if ((n[NODE_TYPE] != NT_COMBINATORIAL && n[NODE_TYPE] != NT_REGISTER) || number < 1 ||
            number > g_jed.npins || (n[NODE_ROW_WIDTH] == 0 && n[NODE_CFG_BITS] == 0)) {
            continue;
        }
        pin = &g_jed.pins[number];
        if (pin->node >= 0) {
            continue;
        }
        if (n[NODE_ROW_WIDTH] != 0 &&
            fuse_bits(rd32(n + NODE_ROW_FUSE), n[NODE_ROW_WIDTH]) != rd16(n + NODE_SEL_VALUE)) {
            continue;
        }
        if (n[NODE_CFG_BITS] != 0 && fuse_bits(rd32(n + NODE_CFG_FUSE), n[NODE_CFG_BITS]) != n[NODE_CFG_VALUE]) {
            continue;
        }
        pin->node = (int16_t)i;
    }
    for (int p = 1; p <= g_jed.npins; p++) {
        jpin_t *pin = &g_jed.pins[p];
        const uint8_t *n;
        int registered;
        if (pin->node < 0) {
            continue;
        }
        n = dev_node(pin->node);
        registered = n[NODE_TYPE] == NT_REGISTER;
        pin->enable = (int16_t)related_node(pin->node, NT_ENABLE);
        if (array_rows((int)rd16(n + NODE_ARRAY)) == 0 && pin->enable >= 0 &&
            array_rows((int)rd16(dev_node(pin->enable) + NODE_ARRAY)) == 0 &&
            !(registered && pin->column >= 0 && g_jed.columns[pin->column].used)) {
            pin->node = pin->enable = -1;
            continue;
        }
        if (registered) {
            /* a reset or preset term that is always false is left out, as
             * in a design that has none */
            pin->reset = (int16_t)related_node(pin->node, NT_RESET);
            if (pin->reset >= 0 && array_rows((int)rd16(dev_node(pin->reset) + NODE_ARRAY)) == 0) {
                pin->reset = -1;
            }
            pin->preset = (int16_t)related_node(pin->node, NT_PRESET);
            if (pin->preset >= 0 && array_rows((int)rd16(dev_node(pin->preset) + NODE_ARRAY)) == 0) {
                pin->preset = -1;
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * Symbols and product terms
 * ------------------------------------------------------------------------- */
static int add_symbol(const char *name, const char *type, int pin, int kind, int flags, int pin_rec, int node_rec)
{
    sym_t *s = &g_syms[g_nsyms];
    snprintf(s->name, sizeof(s->name), "%s", name);
    snprintf(s->type, sizeof(s->type), "%s", type);
    s->pin = (uint16_t)pin;
    s->kind = (uint8_t)kind;
    s->flags = (uint8_t)flags;
    s->pin_rec = (int16_t)pin_rec;
    s->node_rec = (int16_t)node_rec;
    s->file_3c = 0;
    s->frame = NULL;
    return g_nsyms++;
}

/* The symbol table as cupl writes it for such a design: the two constants,
 * then for every pin
 *   - an input: the pin;
 *   - a combinatorial output: the pin, bound to its node record as well,
 *     and its output enable term (extension oe);
 *   - a registered output: the pin with the pin record of the register
 *     feedback, the register (extension d) and its reset (ar), output
 *     enable (oe) and preset (sp) terms.
 * A pin is flagged as an input if a product term uses it. */
static void build_symbols(void)
{
    g_syms = alloc_tracked(((size_t)g_jed.npins * 5 + 3) * sizeof(sym_t), "symbol table");
    g_nsyms = 0;
    add_symbol("", "", 0, KIND_CONST0, 0, -1, -1);
    add_symbol("", "", 0, KIND_CONST1, 0, -1, -1);
    for (int p = 1; p <= g_jed.npins; p++) {
        jpin_t *pin = &g_jed.pins[p];
        int low = pin->low ? SYMF_LOW : 0;
        int used = (pin->column >= 0 && g_jed.columns[pin->column].used) ? SYMF_INPUT : 0;
        if (pin->node < 0) {
            pin->sym = (int16_t)add_symbol(pin->name, "", p, KIND_PIN, used | low, pin->in_rec, -1);
            continue;
        }
        if (dev_node(pin->node)[NODE_TYPE] == NT_REGISTER) {
            pin->sym = (int16_t)add_symbol(pin->name, "", p, KIND_PIN, used | low,
                                           pin->reg_rec >= 0 ? pin->reg_rec : pin->in_rec, pin->node);
            pin->out_sym = (int16_t)add_symbol(pin->name, "d", p, KIND_PIN_EXT, SYMF_OUTPUT | low, -1, pin->node);
        } else {
            pin->sym = (int16_t)add_symbol(pin->name, "", p, KIND_PIN, SYMF_OUTPUT | used | low, pin->in_rec,
                                           pin->node);
            pin->out_sym = pin->sym;
        }
        if (pin->reset >= 0) {
            pin->reset_sym = (int16_t)add_symbol(pin->name, "ar", p, KIND_PIN_EXT, SYMF_OUTPUT, -1, pin->reset);
        }
        if (pin->enable >= 0) {
            pin->enable_sym = (int16_t)add_symbol(pin->name, "oe", p, KIND_PIN_EXT, SYMF_OUTPUT, -1, pin->enable);
        }
        if (pin->preset >= 0) {
            pin->preset_sym = (int16_t)add_symbol(pin->name, "sp", p, KIND_PIN_EXT, SYMF_OUTPUT, -1, pin->preset);
        }
    }
    g_nsyms_file = g_nsyms;
    g_plane_words = (uint16_t)cf_plane_words(g_nsyms);
    g_row_words = g_plane_words * 3;
}

/* Mark in `outputs` (a bit plane) the symbols that array `array` drives.
 * Returns their number. */
static int array_outputs(int array, uint16_t *outputs)
{
    int n = 0;
    for (int p = 1; p <= g_jed.npins; p++) {
        const jpin_t *pin = &g_jed.pins[p];
        const int16_t node[4] = { pin->node, pin->enable, pin->reset, pin->preset };
        const int16_t sym[4] = { pin->out_sym, pin->enable_sym, pin->reset_sym, pin->preset_sym };
        for (int k = 0; k < 4; k++) {
            if (node[k] >= 0 && sym[k] >= 0 && (int)rd16(dev_node(node[k]) + NODE_ARRAY) == array) {
                if (outputs != NULL) {
                    bit_set(outputs + 1, sym[k]);
                }
                n++;
            }
        }
    }
    return n;
}

/* The rows of one array as a frame.  Rows are kept in their place up to the
 * last one that is not always false, so that a row is found from its first
 * fuse (STUCKH / STUCKL); an array without such a row is one false row.
 * In a row, a symbol is used if one of the two fuses of its pin is intact,
 * and true if that is the fuse of the true input.  A row that is always
 * false uses the constant 0 and a row without an input (always true) the
 * constant 1, as in the rows cupl writes. */
static frame_t *build_frame(int array)
{
    const uint8_t *a = array_record(array);
    frame_t *f = alloc_tracked(sizeof(*f), "initialization");
    uint32_t first = rd32(a + NODE_ROW_FUSE);
    int width = a[NODE_ROW_WIDTH];
    int rows = array_rows(array);
    f->index = (uint16_t)array;
    f->nsyms = g_nsyms;
    f->nrows = (uint16_t)(rows > 0 ? rows : 1);
    f->wa = (uint16_t)rd16(a + NODE_MAX_TERMS);
    f->row_words = g_row_words;
    f->total_words = f->nrows * f->row_words;
    f->rows = alloc_tracked((size_t)f->total_words * 2 + 2, "frame allocation");
    f->byte_size = f->total_words * 2 + CF_FRAME_SIZE;
    f->plane0 = f->rows;
    f->plane1 = f->plane0 + g_plane_words;
    f->plane2 = f->plane1 + g_plane_words;
    for (int r = 0; r < f->nrows; r++) {
        uint16_t *out = f->rows + (size_t)r * (size_t)g_row_words;
        uint16_t *used = out + g_plane_words;
        uint16_t *high = used + g_plane_words;
        uint32_t row = first + (uint32_t)(r * width);
        int inputs;
        array_outputs(array, out);
        if (r + 1 < f->nrows) {
            used[0] |= 1;           /* another term follows */
        }
        if (rows == 0 || row_is_false(row, width)) {
            bit_set(used + 1, 0);
            bit_set(high + 1, 0);
            continue;
        }
        inputs = 0;
        for (int c = 0; c < g_jed.ncolumns; c++) {
            const column_t *col = &g_jed.columns[c];
            int sym = g_jed.pins[col->pin].sym;
            if (fuse(row + col->true_col) == 0) {
                bit_set(used + 1, sym);
                bit_set(high + 1, sym);
                inputs++;
            } else if (fuse(row + col->comp_col) == 0) {
                bit_set(used + 1, sym);
                inputs++;
            }
        }
        if (inputs == 0) {
            bit_set(used + 1, 1);
            bit_set(high + 1, 1);
        }
    }
    return f;
}

/* The term table: a frame for every array that drives a symbol */
static void build_frames(void)
{
    g_nterms = (int)rd16(dev_header() + HDR_ARRAYS);
    g_terms = alloc_tracked(((size_t)g_nterms + 1) * sizeof(term_entry_t), "initialization");
    g_total_rows = 0;
    for (int array = 0; array < g_nterms; array++) {
        if (array_record(array) != NULL && array_outputs(array, NULL) > 0) {
            frame_t *f = build_frame(array);
            g_terms[array].frame = f;
            g_terms[array].nrows = f->nrows;
            g_terms[array].first_row = g_total_rows;
            g_total_rows += f->nrows;
        }
    }
}

/* -------------------------------------------------------------------------
 * Header items
 * ------------------------------------------------------------------------- */

/* The header items of the design (NAME, PARTNO, ...) as csim compares them
 * with those of the .si file.  cupl writes them into the design
 * specification of the JEDEC file, one "Keyword   text" line each; items
 * that are not there are not compared. */
static void read_header_items(const char *design)
{
    static const char *const k_items[8] = { "name", "partno", "revision", "date", "designer", "company",
                                            "assembly", "location" };
    const char *value[8] = { NULL };
    size_t length[8] = { 0 };
    size_t size = 8;
    char *out;
    g_header_items = 0;
    for (const char *line = design; line != NULL && *line != '\0'; ) {
        const char *end = strchr(line, '\n');
        size_t len = end != NULL ? (size_t)(end - line) : strlen(line);
        for (int i = 0; i < 8; i++) {
            size_t klen = strlen(k_items[i]);
            if (len > klen && strncasecmp(line, k_items[i], klen) == 0 && (line[klen] == ' ' || line[klen] == '\t') &&
                value[i] == NULL) {
                const char *v = line + klen;
                size_t vlen = len - klen;
                while (vlen > 0 && (*v == ' ' || *v == '\t')) {
                    v++;
                    vlen--;
                }
                while (vlen > 0 && (v[vlen - 1] == ' ' || v[vlen - 1] == '\t')) {
                    vlen--;
                }
                value[i] = v;
                length[i] = vlen;
                size += vlen;
                g_header_items |= (uint8_t)(1u << i);
            }
        }
        line = end != NULL ? end + 1 : NULL;
    }
    out = g_abs_extra = alloc_tracked(size + 1, "symbol table");
    for (int i = 0; i < 8; i++) {
        if (value[i] != NULL) {
            memcpy(out, value[i], length[i]);
        }
        out += length[i] + 1;
    }
}

/* The device a JEDEC file names in its design specification, as cupl
 * writes it there ("Device   g22v10lcc  Library DLIB-h-40-2"), in lower
 * case; "" if it names none. */
static void read_header_device(const char *design)
{
    g_jed.device[0] = '\0';
    for (const char *line = design; line != NULL && *line != '\0'; ) {
        const char *end = strchr(line, '\n');
        const char *s = line;
        size_t len = 0;
        while (*s == ' ' || *s == '\t') {
            s++;
        }
        if (strncasecmp(s, "device", 6) == 0 && (s[6] == ' ' || s[6] == '\t' || s[6] == ':')) {
            for (s += 6; *s == ' ' || *s == '\t' || *s == ':'; s++) {
            }
            while (is_alnum(s[len]) || s[len] == '_') {
                len++;
            }
            if (len > 0 && len <= DL_ENTRY_NAME_SIZE) {
                char word[DL_ENTRY_NAME_SIZE + 1];
                memcpy(word, s, len);
                word[len] = '\0';
                copy_lowercase(g_jed.device, word);
                return;
            }
        }
        line = end != NULL ? end + 1 : NULL;
    }
}

/* -------------------------------------------------------------------------
 * Pin names from the design file
 *
 * A fuse map has no names.  If <design>.pld is there, the pins take the
 * names of its simple pin declarations
 *     PIN <number> = [!]<name>;
 *     PIN [<number>, <number>..<number>] = [!][<name>, <name><n>..<m>];
 * with the polarity csim gives a compiled design: a vector value of 1 for
 * a pin declared as !name is a low level.  FIELD <name> = [<names>];
 * statements over such pins define the fields, and DEVICE names the device
 * whose pin numbers these are.  Everything else in the file is skipped:
 * nothing is preprocessed, so names made with $DEFINE or declared in an
 * $INCLUDE file are not found.
 * ------------------------------------------------------------------------- */
static void skip_blanks(const char **s)
{
    while (is_space(**s)) {
        (*s)++;
    }
}

static int take(const char **s, int c)
{
    skip_blanks(s);
    if (**s != c) {
        return 0;
    }
    (*s)++;
    return 1;
}

static int take_dots(const char **s)
{
    skip_blanks(s);
    if ((*s)[0] != '.' || (*s)[1] != '.') {
        return 0;
    }
    *s += 2;
    return 1;
}

/* A name or keyword: letters, digits and '_', not digits only */
static int read_word(const char **s, char *out)
{
    size_t len = 0;
    int letters = 0;
    skip_blanks(s);
    while (is_alnum((*s)[len]) || (*s)[len] == '_') {
        letters |= !is_digit((*s)[len]);
        len++;
    }
    if (len == 0 || len >= NAME_SIZE || !letters) {
        return 0;
    }
    memcpy(out, *s, len);
    out[len] = '\0';
    *s += len;
    return 1;
}

static int read_decimal(const char **s, int *value)
{
    int v = 0, digits = 0;
    skip_blanks(s);
    for (; is_digit(**s) && digits < 6; (*s)++, digits++) {
        v = v * 10 + (**s - '0');
    }
    *value = v;
    return digits > 0 && !is_alnum(**s) && **s != '_';
}

/* "<n>" or "[<n>, <n>..<m>, ...]"; returns the number of pins, -1 if it
 * is something else */
static int read_pin_list(const char **s, int *pins)
{
    int n = 0;
    if (!take(s, '[')) {
        return read_decimal(s, &pins[0]) ? 1 : -1;
    }
    do {
        int a, b;
        if (!read_decimal(s, &a)) {
            return -1;
        }
        b = a;
        if (take_dots(s) && !read_decimal(s, &b)) {
            return -1;
        }
        for (int v = a;; v += (b >= a ? 1 : -1)) {
            if (n >= PLD_MAX_LIST) {
                return -1;
            }
            pins[n++] = v;
            if (v == b) {
                break;
            }
        }
    } while (take(s, ','));
    return take(s, ']') ? n : -1;
}

/* "[!]<name>" or "[!][<name>, <name><n>..<m>, ...]", where <name><n>..<m>
 * (or ..<name><m>) stands for the names numbered n to m; returns the
 * number of names, -1 if it is something else */
static int read_name_list(const char **s, pld_name_t *names)
{
    int all_low = take(s, '!');
    int n = 0;
    if (!take(s, '[')) {
        names[0].low = (uint8_t)all_low;
        return read_word(s, names[0].name) ? 1 : -1;
    }
    do {
        int low = take(s, '!') ^ all_low;
        char word[NAME_SIZE], last[NAME_SIZE];
        size_t len, stem;
        int a, b;
        if (!read_word(s, word)) {
            return -1;
        }
        if (!take_dots(s)) {
            if (n >= PLD_MAX_LIST) {
                return -1;
            }
            snprintf(names[n].name, NAME_SIZE, "%s", word);
            names[n++].low = (uint8_t)low;
            continue;
        }
        len = strlen(word);
        for (stem = len; stem > 0 && is_digit(word[stem - 1]); stem--) {
        }
        if (stem == len || len - stem > 5 || (word[stem] == '0' && len - stem > 1)) {
            return -1;      /* no number, or one written with leading zeroes */
        }
        a = atoi(word + stem);
        skip_blanks(s);
        if (is_digit(**s)) {
            if (!read_decimal(s, &b)) {
                return -1;
            }
        } else {
            size_t i;
            if (!read_word(s, last) || strncmp(last, word, stem) != 0 || last[stem] == '\0' ||
                strlen(last) - stem > 5) {
                return -1;
            }
            for (i = stem; is_digit(last[i]); i++) {
            }
            if (last[i] != '\0') {
                return -1;
            }
            b = atoi(last + stem);
        }
        for (int v = a;; v += (b >= a ? 1 : -1)) {
            if (n >= PLD_MAX_LIST || snprintf(names[n].name, NAME_SIZE, "%.*s%d", (int)stem, word, v) >= NAME_SIZE) {
                return -1;
            }
            names[n++].low = (uint8_t)low;
            if (v == b) {
                break;
            }
        }
    } while (take(s, ','));
    return take(s, ']') ? n : -1;
}

/* The order cupl sorts names in: by the text in front of a trailing
 * number, then by the number ("q2" < "q10") */
static int compare_names(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b), sa = la, sb = lb;
    int order;
    while (sa > 0 && is_digit(a[sa - 1])) {
        sa--;
    }
    while (sb > 0 && is_digit(b[sb - 1])) {
        sb--;
    }
    if (sa == la || sb == lb) {
        return strcmp(a, b);
    }
    order = strncmp(a, b, sa < sb ? sa : sb);
    if (order == 0 && sa != sb) {
        order = sa < sb ? -1 : 1;
    }
    if (order == 0 && atol(a + sa) != atol(b + sb)) {
        order = atol(a + sa) < atol(b + sb) ? -1 : 1;
    }
    if (order == 0 && la != lb) {
        order = la < lb ? -1 : 1;
    }
    return order;
}

/* One statement (the text in front of a ';') that starts with PIN, FIELD
 * or DEVICE; anything that is not of the forms above is left alone. */
static void read_pld_statement(const char *s)
{
    char word[NAME_SIZE];
    pld_name_t names[PLD_MAX_LIST];
    int pins[PLD_MAX_LIST];
    int npins, nnames;
    if (!read_word(&s, word)) {
        return;
    }
    if (strcasecmp(word, "pin") == 0) {
        npins = read_pin_list(&s, pins);
        if (npins < 1 || !take(&s, '=')) {
            return;
        }
        nnames = read_name_list(&s, names);
        skip_blanks(&s);
        if (nnames != npins || *s != '\0') {
            return;
        }
        for (int i = 0; i < npins && g_pld.npins < PLD_MAX_PINS; i++) {
            g_pld.pins[g_pld.npins].number = pins[i];
            g_pld.pins[g_pld.npins++].name = names[i];
        }
    } else if (strcasecmp(word, "field") == 0 || strcasecmp(word, "fld") == 0) {
        pld_field_t *field, **link;
        if (!read_word(&s, word) || !take(&s, '=')) {
            return;
        }
        nnames = read_name_list(&s, names);
        skip_blanks(&s);
        if (nnames < 1 || *s != '\0') {
            return;
        }
        field = alloc_tracked(sizeof(*field), "field initialization");
        snprintf(field->name, sizeof(field->name), "%s", word);
        field->count = nnames;
        field->members = alloc_tracked((size_t)nnames * sizeof(pld_name_t), "field initialization");
        memcpy(field->members, names, (size_t)nnames * sizeof(pld_name_t));
        for (link = &g_pld.fields; *link != NULL && compare_names((*link)->name, word) <= 0; link = &(*link)->next) {
        }
        field->next = *link;
        *link = field;
    } else if (strcasecmp(word, "device") == 0 && g_pld.device[0] == '\0') {
        if (read_word(&s, word) && strlen(word) <= DL_ENTRY_NAME_SIZE) {
            copy_lowercase(g_pld.device, word);
        }
    }
}

/* Does a PIN, FIELD or DEVICE statement start at s (the start of a line)? */
static int is_declaration(const char *s)
{
    static const char *const k_words[4] = { "pin", "field", "fld", "device" };
    skip_blanks(&s);
    for (int i = 0; i < 4; i++) {
        size_t len = strlen(k_words[i]);
        if (strncasecmp(s, k_words[i], len) == 0 && !is_alnum(s[len]) && s[len] != '_') {
            return 1;
        }
    }
    return 0;
}

/* Read the declarations of <design>.pld, if the file is there. */
static void read_pld(void)
{
    char *name = make_file_name(g_dir_prefix, g_base_name, ".pld");
    FILE *fp = open_file(name, "rt");
    char *text;
    size_t len = 0;
    int c, prev = 0, in_comment = 0, line_start = 1, skip_line = 0;
    memset(&g_pld, 0, sizeof(g_pld));
    if (fp == NULL) {
        return;
    }
    g_pld.file = name;
    g_pld.pins = alloc_tracked(PLD_MAX_PINS * sizeof(pld_pin_t), "initialization");
    /* the text without comments and without preprocessor lines ($...) */
    text = alloc_tracked(PLD_MAX_TEXT + 2, "initialization");
    while ((c = read_char(fp)) != EOF && len < PLD_MAX_TEXT) {
        if (in_comment) {
            if (prev == '*' && c == '/') {
                in_comment = 0;
                c = ' ';
            }
            prev = c;
            continue;
        }
        if (prev == '/' && c == '*') {
            in_comment = 1;
            text[len - 1] = ' ';
            prev = 0;
            continue;
        }
        if (c == '\n') {
            line_start = 1;
            skip_line = 0;
        } else if (line_start && !is_space(c)) {
            line_start = 0;
            skip_line = (c == '$');
        }
        text[len++] = (char)((skip_line || c == '\0') ? ' ' : c);
        prev = c;
    }
    text[len] = '\0';
    close_file(fp);
    /* statements end with ';'; a declaration also starts at the start of
     * a line, whatever stands in front of it */
    for (char *s = text; *s != '\0'; ) {
        char *end = strchr(s, ';');
        char *start = s;
        if (end == NULL) {
            break;
        }
        *end = '\0';
        for (char *p = s; p != NULL && *p != '\0'; ) {
            if (is_declaration(p)) {
                start = p;
            }
            p = strchr(p, '\n');
            if (p != NULL) {
                p++;
            }
        }
        if (is_declaration(start)) {
            read_pld_statement(start);
        }
        s = end + 1;
    }
    free_tracked(text);
}

static int pin_by_name(const char *name)
{
    for (int p = 1; p <= g_jed.npins; p++) {
        if (strcmp(g_jed.pins[p].name, name) == 0) {
            return p;
        }
    }
    return 0;
}

/* Give the pins the names of the .pld file.  The first declaration of a
 * pin counts, and a name another pin has is not taken.  A pin without a
 * declaration keeps its default name P<n>, unless the file gives that name
 * to another pin: then it is PIN<n>. */
static void name_pins(void)
{
    if (!g_pld.use_names) {
        if (g_pld.npins > 0) {
            stream_printf(stdout, "pin names of %s are not used: they are for device %s\n", g_pld.file, g_pld.device);
        }
        return;
    }
    for (int i = 0; i < g_pld.npins; i++) {
        const pld_pin_t *decl = &g_pld.pins[i];
        int taken = 0;
        jpin_t *pin;
        if (decl->number < 1 || decl->number > g_jed.npins || g_jed.pins[decl->number].named) {
            continue;
        }
        for (int p = 1; p <= g_jed.npins; p++) {
            taken |= g_jed.pins[p].named && strcmp(g_jed.pins[p].name, decl->name.name) == 0;
        }
        if (taken) {
            continue;
        }
        pin = &g_jed.pins[decl->number];
        snprintf(pin->name, sizeof(pin->name), "%s", decl->name.name);
        pin->low = decl->name.low;
        pin->named = 1;
        g_jed.pld_name = g_pld.file;
    }
    for (int p = 1; p <= g_jed.npins; p++) {
        jpin_t *pin = &g_jed.pins[p];
        for (int q = 1; q <= g_jed.npins && !pin->named; q++) {
            if (g_jed.pins[q].named && strcmp(g_jed.pins[q].name, pin->name) == 0) {
                snprintf(pin->name, sizeof(pin->name), "PIN%d", p);
                break;
            }
        }
    }
}

/* Define the fields of the .pld file whose members are all named pins. */
static void define_pld_fields(void)
{
    for (const pld_field_t *f = g_pld.use_names ? g_pld.fields : NULL; f != NULL; f = f->next) {
        field_t *field;
        int ok = pin_by_name(f->name) == 0 && find_field(f->name) == NULL;
        for (int i = 0; ok && i < f->count; i++) {
            int p = pin_by_name(f->members[i].name);
            ok = p != 0 && g_jed.pins[p].named && !f->members[i].low;
        }
        if (!ok) {
            continue;
        }
        field = define_field(dup_string(f->name, "field initialization"));
        field->members = list_new();
        for (int i = 0; i < f->count; i++) {
            list_node_t *node = node_new();
            node->u.sym = &g_syms[g_jed.pins[pin_by_name(f->members[i].name)].sym];
            node->kind = 0xd;
            node->flags |= 1;
            list_append(node, field->members);
        }
    }
}

/* -------------------------------------------------------------------------
 * Entry points
 * ------------------------------------------------------------------------- */

/* Tell on the console what the fuse map was taken for: the device, and
 * which pins are inputs and outputs as the macrocells are programmed. */
static void print_summary(const char *jed_name)
{
    static const char *const k_title[3] = { "inputs:", "combinatorial outputs:", "registered outputs:" };
    stream_printf(stdout, "fuse map %s: %lu fuses, simulated as device %s\n", jed_name, (unsigned long)g_jed.nfuses,
                  g_device_name);
    if (g_jed.pld_name != NULL) {
        stream_printf(stdout, "pin names from %s\n", g_jed.pld_name);
    }
    for (int what = 0; what < 3; what++) {
        int any = 0;
        for (int p = 1; p <= g_jed.npins; p++) {
            const jpin_t *pin = &g_jed.pins[p];
            int is = pin->node < 0 ? 0 : (dev_node(pin->node)[NODE_TYPE] == NT_REGISTER ? 2 : 1);
            if (is != what) {
                continue;
            }
            if (what == 0) {
                /* not the supply pins and pins without a way into the array */
                int kind = dev_pin(pin->in_rec)[PIN_KIND];
                if (pin->in_rec < 0 || kind == 2 || kind == 3 || (pin->column < 0 && kind == 0)) {
                    continue;
                }
            }
            if (!any) {
                stream_printf(stdout, "  %-23s", k_title[what]);
                any = 1;
            }
            if (pin->named) {
                stream_printf(stdout, " %d=%s%s", p, pin->low ? "!" : "", pin->name);
            } else {
                stream_printf(stdout, " %d", p);
            }
        }
        if (any) {
            stream_printf(stdout, "\n");
        }
    }
}

/* An output enable pin enables the outputs unless a vector drives it:
 * a design that does not declare the pin has its outputs enabled, and so
 * has a fuse map simulated without the pin in the ORDER list. */
void jedec_design_bound(void)
{
    int active = dev_header()[0x2e] == 4 ? LV_HIGH : LV_LOW;
    for (int i = 0; i < g_nsyms; i++) {
        if (g_syms[i].pin_rec >= 0 && dev_pin(g_syms[i].pin_rec)[PIN_KIND] == PIN_KIND_ENABLE) {
            g_vec_level[i] = (uint8_t)active;
        }
    }
}

sym_t *jedec_pin_symbol(const char *name, int *inverted)
{
    int number = 0;
    if (name[0] != 'P' || name[1] < '1' || name[1] > '9') {
        return NULL;
    }
    for (const char *p = name + 1; *p != '\0'; p++) {
        if (*p < '0' || *p > '9' || number > MAX_PINS) {
            return NULL;
        }
        number = number * 10 + (*p - '0');
    }
    if (number > g_jed.npins || !g_jed.pins[number].named) {
        return NULL;
    }
    *inverted = g_jed.pins[number].low;
    return find_symbol(g_jed.pins[number].name);
}

/* Build the design from <design>.jed: the counterpart of load_design(). */
void load_design_jedec(void)
{
    char *jed_name = make_file_name(g_dir_prefix, g_base_name, ".jed");
    char reason[200];
    char text[400];
    jed_map_t map;
    char *design;
    FILE *fp;

    memset(&g_jed, 0, sizeof(g_jed));
    fp = open_file(jed_name, "rb");
    if (fp == NULL) {
        fatal_error(ERR_OPEN, jed_name);
    }
    if (!jed_read(fp, &map, reason, sizeof(reason))) {
        close_file(fp);
        snprintf(text, sizeof(text), "%s: %s", jed_name, reason);
        fatal_error(ERR_JEDEC_FILE, text);
    }
    close_file(fp);
    /* into tracked memory, so that an error from here on releases it */
    g_jed.nfuses = (uint32_t)map.nfuses;
    g_jed.qp = map.pins;
    g_jed.fuses = alloc_tracked(map.nfuses + 1, "initialization");
    memcpy(g_jed.fuses, map.fuses, map.nfuses);
    design = dup_string(map.design != NULL ? map.design : "", "initialization");
    jed_free(&map);

    read_header_device(design);
    read_pld();
    choose_device(jed_name);
    /* option j adds the vectors to the file that was read */
    snprintf(g_abs_jedec_name, sizeof(g_abs_jedec_name), "%s", g_base_name);
    g_options |= OPT_N;
    read_header_items(design);
    collect_pins();
    scan_arrays();
    configure_macrocells();
    name_pins();
    build_symbols();
    build_frames();
    define_pld_fields();
    print_summary(jed_name);
}
