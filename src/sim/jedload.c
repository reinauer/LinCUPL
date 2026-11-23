/*
 * sim_jedload - the design taken from a JEDEC fuse map instead of an
 * absolute file (option f): <design>.jed is decoded with the library entry
 * of its device into the symbols and product term rows that load_design()
 * would read from <design>.abs, so that the binder and the simulation run
 * on it unchanged.
 *
 * Supported are the GAL16V8 and GAL20V8 in their three modes and the
 * GAL22V10, ATF750 and ATF2500. ATF row sharing and clock routing are
 * decoded here into the existing combinational, D and T cell models.
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
#include "strutil.h"

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
    NODE_PAIR_FUSE = 0x16,      /* u32: control-input selection */
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
    NT_ARRAY = 0x0e,
    NT_CLOCK = 0x0f,
    NT_TOGGLE = 0x1b,
    NT_CLOCK_ENABLE = 0x1f,
    NT_CLOCK_SELECT = 0x2a
};

enum { PIN_KIND_REGISTER_FEEDBACK = 11, PIN_KIND_ENABLE = 9 };

/* Symbol kinds and flags as cupl writes them (src/shared/absdump.c) */
enum {
    KIND_PIN = 2, KIND_PIN_EXT = 5, KIND_CONST0 = 6, KIND_CONST1 = 7,
    KIND_NODE = 9, KIND_NODE_EXT = 10,
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
    { 5828, 0, "g22v10", 0 },       /* 22V10 without the 64 signature fuses */
    { 14394, 28, "v750lcc", 0 },
    { 14435, 28, "v750blcc", 0 },
    { 14504, 28, "v750clcc", 0 },
    { 14504, 28, "v750cextlcc", 0 },
    { 14504, 28, "v750cppklcc", 0 },
    { 14504, 28, "v750cextppklcc", 0 },
    { 71648, 44, "v2500lcc", 0 },
    { 71745, 44, "v2500blcc", 0 },
    { 71816, 44, "v2500clcc", 0 },
    { 71816, 44, "v2500cppklcc", 0 },
    { 14394, 0, "v750", 0 },
    { 14435, 0, "v750b", 0 },
    { 14504, 0, "v750c", 0 },
    { 14504, 0, "v750cext", 0 },
    { 14504, 0, "v750cppk", 0 },
    { 14504, 0, "v750cextppk", 0 },
    { 71648, 0, "v2500", 0 },
    { 71745, 0, "v2500b", 0 },
    { 71816, 0, "v2500c", 0 },
    { 71816, 0, "v2500cppk", 0 }
};

typedef struct {
    int16_t in_rec;             /* pin record of the pin as an input, -1 none */
    int16_t reg_rec;            /* pin record of the register feedback, -1 none */
    int16_t column;             /* its column pair, -1 none */
    int16_t node;               /* node record of the macrocell as programmed, -1: no output */
    int16_t array;              /* OR array of the macrocell, -1: the pin has none */
    int16_t off_enable;         /* output enable term of a macrocell that is turned off, -1 none */
    int16_t enable;             /* node records of the output enable, */
    int16_t reset;              /* reset */
    int16_t preset;             /* and preset term, -1 none */
    int16_t sym;                /* symbol of the pin */
    int16_t out_sym;            /* symbol the product terms drive */
    int16_t enable_sym;
    int16_t reset_sym;
    int16_t preset_sym;
    int16_t clock, clock_sym, clock_select, clock_select_sym;
    int16_t io_rec, io_sym;     /* separate physical input of an ATF output */
    int16_t alias;              /* same flip-flop as this pin, or -1 */
    uint8_t named;              /* the name is from the .pld file */
    uint8_t low;                /* and declared active low (!name) */
    char name[NAME_SIZE];       /* P<number> unless named */
} jpin_t;

typedef struct {
    uint8_t true_col;
    uint8_t comp_col;
    uint16_t pin;
    uint8_t used;               /* some product term has the input */
    int16_t sym;
    uint8_t inverted;
} column_t;

static struct {
    uint8_t *fuses;             /* one byte per fuse */
    uint32_t nfuses;
    unsigned qp;                /* the QP field (number of pins), 0 if the file has none */
    char device[DL_ENTRY_NAME_SIZE + 1]; /* the device the file names, in lower case, "" if none */
    uint32_t ptd_fuse;          /* 0: the device has no product term disable fuses */
    int npins;
    int nsignals;              /* includes the numbered buried registers */
    int atf;                   /* 750 or 2500, zero for GAL devices */
    uint16_t row_mask[512];    /* selected sum-term rows, indexed by array */
    uint8_t gated_clock[512];  /* AND the product clock with the global pin */
    jpin_t pins[MAX_PINS + 1];  /* by pin number */
    column_t columns[MAX_COLUMNS];
    int ncolumns;
    char *pld_name;             /* the .pld file pin names were taken from, NULL none */
} g_jed;

/* What was read from <design>.pld (see "Pin names from the design file") */
#define PLD_MAX_TEXT 0x100000       /* characters of the text that are looked at */
#define PLD_MAX_LIST 64             /* entries of a pin or name list */
#define PLD_MAX_PINS 512            /* pin declarations */
#define PLD_MAX_DEFINES 256         /* names defined with $DEFINE */
#define PLD_MAX_DEFINE_NAME 63      /* characters of such a name */
#define PLD_MAX_NESTING 16          /* $IFDEF levels, $INCLUDE files inside each other */

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

typedef struct {
    char *name;
    char *value;
    int length;                 /* of the name */
    uint8_t defined;            /* 0 after $UNDEF */
} pld_define_t;

/* The text of the .pld file with the directives carried out */
static struct {
    char *text;
    size_t length;
    pld_define_t defines[PLD_MAX_DEFINES];
    int ndefines;
    int longest;                /* length of the longest defined name */
    char pending[PLD_MAX_DEFINE_NAME + 1]; /* characters not yet written: a name may start here */
    int npending;
    int nesting;                /* $IFDEF levels */
    uint8_t outer[PLD_MAX_NESTING + 1]; /* per level: the text around it is read, */
    uint8_t taken[PLD_MAX_NESTING + 1]; /* its condition holds */
    int reading;                /* the text is read (no level skips it) */
} g_pp;

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

static int atf_family(void)
{
    uint32_t count = rd32(dev_header() + HDR_FUSES);
    if (count == 14394 || count == 14435 || count == 14504)
        return 750;
    if (count == 71648 || count == 71745 || count == 71816)
        return 2500;
    return 0;
}

static int registered_node(int node)
{
    return node >= 0 && (dev_node(node)[NODE_TYPE] == NT_REGISTER ||
                         dev_node(node)[NODE_TYPE] == NT_TOGGLE);
}

static int row_selected(int array, int row)
{
    return !g_jed.atf || (array < 512 && row < 16 &&
                          (g_jed.row_mask[array] & (1u << row)));
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
    if (g_jed.atf) {
        for (int c = 0; c + 1 < width; c += 2) {
            if (!fuse(first + c) && !fuse(first + c + 1))
                return 1;
        }
    }
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
            if (row_selected(array, r) && !row_is_false(first + (uint32_t)(r * width), width)) {
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
    int atf = atf_family();
    for (int i = 0; i < count; i++, init += INIT_SIZE) {
        uint32_t first = rd32(init);
        /* ATF2500 initializers also contain writable macrocell defaults.
         * Only the C revisions' feedback and pin-keeper options select
         * library variants. Signature and reserved bits do not. */
        if (atf && !((atf == 750 && (first == 14499 || first == 14500)) ||
                     (atf == 2500 && first == 71745)))
            continue;
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
    int ok = hdr[HDR_TYPE] == 0;   /* the pin count, one byte, is <= MAX_PINS */
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
        case NT_CLOCK: case NT_TOGGLE: case NT_CLOCK_ENABLE: case NT_CLOCK_SELECT:
            ok = atf_family() != 0;
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
static void choose_device(const char *jed_name, int strict)
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
            found = try_device(k_families[i].device, jed_name, entry, &code, text, sizeof(text)) &&
                    mode_fuses_match();
            if (!found && first_code < 0) {
                first_code = code;
                snprintf(first_text, sizeof(first_text), "%s", text);
            }
        }
        if (!found && first_code < 0) {
            snprintf(text, sizeof(text), "%s has %lu fuses (supported families: GAL16V8, GAL20V8, GAL22V10, ATF750, ATF2500)",
                     jed_name, (unsigned long)g_jed.nfuses);
            fatal_error(ERR_JEDEC_NO_DEVICE, text);
        }
        if (!found) {
            fatal_error(first_code, first_text);
        }
        g_pld.use_names = g_pld.device[0] == '\0';
    }
    if (!mode_fuses_match()) {
        if (strict) {
            fatal_error(ERR_JEDEC_MODE, entry);
        }
        print_warning_now(ERR_JEDEC_MODE, entry);
    }
    if (strict && g_jed.qp && g_jed.qp != dev_header()[HDR_PINS]) {
        snprintf(text, sizeof(text), "%s has QP%u, device %s has %u pins",
                 jed_name, g_jed.qp, entry, dev_header()[HDR_PINS]);
        fatal_error(ERR_JEDEC_FILE, text);
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
    g_jed.nsignals = g_jed.npins;
    g_jed.atf = atf_family();
    memset(g_jed.row_mask, 0xff, sizeof(g_jed.row_mask));
    g_jed.ncolumns = 0;
    for (int p = 0; p <= MAX_PINS; p++) {
        jpin_t *pin = &g_jed.pins[p];
        memset(pin, 0, sizeof(*pin));
        pin->in_rec = pin->reg_rec = pin->column = pin->node = pin->array = pin->off_enable = -1;
        pin->enable = pin->reset = pin->preset = -1;
        pin->sym = pin->out_sym = pin->enable_sym = pin->reset_sym = pin->preset_sym = -1;
        pin->clock = pin->clock_sym = pin->clock_select = pin->clock_select_sym = -1;
        pin->io_rec = pin->io_sym = pin->alias = -1;
        snprintf(pin->name, sizeof(pin->name), "P%d", p);
    }
    for (int i = 0; i < count; i++) {
        const uint8_t *rec = dev_pin(i);
        int number = (int)rd16(rec + PIN_NUMBER);
        jpin_t *pin;
        if (number < 1 || number > (g_jed.atf ? MAX_PINS : g_jed.npins)) {
            continue;
        }
        if (number > g_jed.nsignals)
            g_jed.nsignals = number;
        pin = &g_jed.pins[number];
        if (g_jed.atf && rec[4] == 37)
            pin->io_rec = (int16_t)i;
        if (g_jed.atf && rec[4] != 0)
            continue;
        if (rec[PIN_KIND] == PIN_KIND_REGISTER_FEEDBACK || (g_jed.atf && rec[PIN_KIND] == 23)) {
            if (pin->reg_rec < 0) {
                pin->reg_rec = (int16_t)i;
            }
        } else if (pin->in_rec < 0) {
            pin->in_rec = (int16_t)i;
        }
        if (rec[PIN_TRUE_COL] != rec[PIN_COMP_COL] && (g_jed.atf || pin->column < 0)) {
            column_t *c;
            int found = -1;
            for (int k = 0; k < g_jed.ncolumns; k++) {
                if ((g_jed.columns[k].true_col == rec[PIN_TRUE_COL] &&
                     g_jed.columns[k].comp_col == rec[PIN_COMP_COL]) ||
                    (g_jed.columns[k].true_col == rec[PIN_COMP_COL] &&
                     g_jed.columns[k].comp_col == rec[PIN_TRUE_COL])) {
                    found = k;
                    break;
                }
            }
            if (g_jed.atf && found >= 0) {
                pin->column = (int16_t)found;
                continue;
            }
            if (g_jed.ncolumns >= MAX_COLUMNS) {
                fatal_error(ERR_JEDEC_DEVICE, g_device_name);
            }
            c = &g_jed.columns[g_jed.ncolumns];
            c->true_col = rec[PIN_TRUE_COL];
            c->comp_col = rec[PIN_COMP_COL];
            c->pin = (uint16_t)number;
            c->used = 0;
            c->sym = -1;
            pin->column = (int16_t)g_jed.ncolumns++;
        }
    }
}

/* Every fuse of a row must be the input of a pin; and mark the inputs that
 * some product term uses. */
static void scan_arrays(void)
{
    uint8_t covered[256] = { 0 };
    for (int c = 0; c < g_jed.ncolumns; c++) {
        covered[g_jed.columns[c].true_col] = 1;
        covered[g_jed.columns[c].comp_col] = 1;
    }
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
        if (g_jed.atf && (rd16(a + NODE_ARRAY) >= 512 || terms > 16))
            fatal_error(ERR_JEDEC_DEVICE, "invalid ATF array geometry");
        if (g_jed.atf && width == 1)  /* a clock-selection descriptor */
            continue;
        if (!g_jed.atf && width != 2 * g_jed.ncolumns) {
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
            for (int c = 0; g_jed.atf && c < width; c++) {
                if (!covered[c] && !fuse(row + (uint32_t)c))
                    fatal_error(ERR_JEDEC_DEVICE, "device library lacks a used array input");
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
        if (pin->array < 0) {
            pin->array = (int16_t)rd16(n + NODE_ARRAY);
        }
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
            pin->off_enable = pin->enable;
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

/* ATF macrocells have a pin input, two register feedback inputs and two
 * overlapping sums. The numbered alias of the first register must share
 * its state with the registered pin. Keep the library's cell models and
 * connection records, and select the physical rows feeding each sum.
 * Routing follows the Logic Options in Microchip DS20006641 (ATF750C)
 * and the output logic diagrams in Atmel 0777 (ATF2500C). */
static int atf_node(int number, int type)
{
    for (int i = 0; i < node_count(); i++) {
        const uint8_t *n = dev_node(i);
        if ((int)rd16(n + NODE_NUMBER) == number && n[NODE_TYPE] == type)
            return i;
    }
    return -1;
}

static int node_config_matches(const uint8_t *n)
{
    return !n[NODE_CFG_BITS] ||
           fuse_bits(rd32(n + NODE_CFG_FUSE), n[NODE_CFG_BITS]) == n[NODE_CFG_VALUE];
}

static void configure_atf(void)
{
    /* Register records contain routing selectors as well as D/T bits.
     * Routing a sum away from the pin never removes the flip-flop. */
    for (int p = 1; p <= g_jed.nsignals; p++) {
        jpin_t *pin = &g_jed.pins[p];
        for (int i = 0; i < node_count(); i++) {
            const uint8_t *n = dev_node(i);
            int type = n[NODE_TYPE];
            unsigned value, wanted, mask;
            if ((int)rd16(n + NODE_NUMBER) != p ||
                (type != NT_COMBINATORIAL && type != NT_REGISTER && type != NT_TOGGLE))
                continue;
            if (p > g_jed.npins && type == NT_COMBINATORIAL)
                continue;
            if (!node_config_matches(n))
                continue;
            value = fuse_bits(rd32(n + NODE_ROW_FUSE), n[NODE_ROW_WIDTH]);
            wanted = rd16(n + NODE_SEL_VALUE);
            mask = (1u << n[NODE_ROW_WIDTH]) - 1;
            if (p <= g_jed.npins) {
                if (g_jed.atf == 750)
                    mask &= ~2u;  /* independent or combined sums */
                else if (n[NODE_ROW_WIDTH] == 4)
                    mask &= ~3u;  /* ATF2500's shared rows */
            } else if (g_jed.atf == 2500 || rd32(n + NODE_ROW_FUSE) < 14394) {
                mask = 0;
            }
            if ((value & mask) != (wanted & mask))
                continue;
            /* D records of the physical pin do not constrain its T bit. */
            if (pin->node < 0 || type == NT_TOGGLE) {
                pin->node = (int16_t)i;
                pin->array = (int16_t)rd16(n + NODE_ARRAY);
            }
        }
    }
    for (int p = 1; p <= g_jed.npins; p++) {
        jpin_t *pin = &g_jed.pins[p];
        const uint8_t *n, *a;
        unsigned mode, mask;
        int alias = -1, buried = -1;
        uint32_t config;
        if (pin->node < 0)
            continue;
        n = dev_node(pin->node);
        a = array_record(pin->array);
        config = rd32(n + NODE_ROW_FUSE);
        if (g_jed.atf == 2500 && n[NODE_ROW_WIDTH] == 2)
            config -= 2;
        mode = fuse_bits(config, g_jed.atf == 750 ? 3 : 4);
        for (int q = g_jed.npins + 1; q <= g_jed.nsignals; q++) {
            jpin_t *other = &g_jed.pins[q];
            const uint8_t *b = array_record(other->array);
            if (other->node < 0 || !b)
                continue;
            if (pin->reg_rec >= 0 && other->column >= 0 &&
                (g_jed.columns[other->column].true_col == dev_pin(pin->reg_rec)[PIN_TRUE_COL] ||
                 g_jed.columns[other->column].comp_col == dev_pin(pin->reg_rec)[PIN_TRUE_COL]))
                alias = q;
            else if (rd32(b + NODE_ROW_FUSE) == rd32(a + NODE_ROW_FUSE) +
                     (uint32_t)(a[NODE_ROW_WIDTH] * (g_jed.atf == 750 ? rd16(a + NODE_MAX_TERMS) / 2 : 8)))
                buried = q;
        }
        if (g_jed.atf == 750) {
            int count = rd16(a + NODE_MAX_TERMS);
            mask = (1u << ((mode & 2) ? count / 2 : count)) - 1;
        } else {
            unsigned routing = mode & 3;
            mask = routing == 2 ? 0xfff : routing == 1 ? 0x00f : 0x0ff;
            if (routing == 3) {
                /* S0=S1=1 feeds D/T2 back to the array. S5 also selects
                 * whether a combinational pin includes its four rows. */
                int t = buried < 0 ? -1 : atf_node(buried, NT_TOGGLE);
                int s5 = t >= 0 && fuse(rd32(dev_node(t) + NODE_CFG_FUSE));
                mask = (mode & 4) ? (s5 ? 0xf0f : 0x00f) : 0x0ff;
                if (buried >= 0) {
                    /* This is a private, normalized device image. The
                     * compiler entry only offers registered equations
                     * for this node, but the hardware has a bypass. */
                    uint8_t *b = g_dev_rec + (dev_node(g_jed.pins[buried].node) - g_dev_rec);
                    b[NODE_TYPE] = NT_COMBINATORIAL;
                }
            }
        }
        g_jed.row_mask[pin->array] = (uint16_t)mask;
        pin->enable = (int16_t)related_node(pin->node, NT_ENABLE);
        if (alias >= 0 && registered_node(pin->node)) {
            jpin_t *other = &g_jed.pins[alias];
            if (pin->enable >= 0 && array_rows(rd16(dev_node(pin->enable))) == 0) {
                /* A buried use of FF1 can use the pin's larger sum. */
                other->array = pin->array;
                pin->node = -1;
                pin->enable = -1;
            } else {
                other->alias = (int16_t)p;
                other->node = -1;
            }
        }
    }
    for (int p = 1; p <= g_jed.nsignals; p++) {
        jpin_t *pin = &g_jed.pins[p];
        if (pin->node < 0)
            continue;
        if (registered_node(pin->node)) {
            pin->reset = (int16_t)related_node(pin->node, NT_RESET);
            pin->preset = (int16_t)related_node(pin->node, NT_PRESET);
            pin->clock = (int16_t)atf_node(p, NT_CLOCK);
            if (g_jed.atf == 2500) {
                int ce = atf_node(p, NT_CLOCK_ENABLE);
                if (ce >= 0 && fuse_bits(rd32(dev_node(ce) + NODE_ROW_FUSE), 1))
                    g_jed.gated_clock[rd16(dev_node(ce) + NODE_ARRAY)] = 1;
            } else {
                int ck = atf_node(p, NT_CLOCK_SELECT);
                if (ck >= 0 && fuse(rd32(dev_node(ck) + NODE_PAIR_FUSE)))
                    pin->clock_select = (int16_t)ck;
            }
        }
        if (p <= g_jed.npins && pin->enable >= 0 &&
            !array_rows(rd16(dev_node(pin->enable))) && !registered_node(pin->node)) {
            pin->node = pin->enable = -1;
        }
    }
}

/* -------------------------------------------------------------------------
 * Symbols and product terms
 * ------------------------------------------------------------------------- */
static int compare_names(const char *a, const char *b);

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
 *   - an input: the pin; on a pin with a macrocell that the .pld file
 *     declares, also the output enable term of the macrocell, which is
 *     always false (cupl writes it for a declared pin: $TRACE lists it);
 *   - a combinatorial output: the pin, bound to its node record as well,
 *     and its output enable term (extension oe);
 *   - a registered output: the pin with the pin record of the register
 *     feedback, the register (extension d) and its reset (ar), output
 *     enable (oe) and preset (sp) terms.
 * A pin is flagged as an input if a product term uses it.  The pins come
 * in the order of their names, as cupl sorts its symbols: $TRACE lists
 * the outputs of a term (the registers of a common reset) in this order. */
static void build_symbols(void)
{
    int order[MAX_PINS + 1];
    g_syms = alloc_tracked(((size_t)g_jed.nsignals * 9 + 3) * sizeof(sym_t), "symbol table");
    g_nsyms = 0;
    add_symbol("", "", 0, KIND_CONST0, 0, -1, -1);
    add_symbol("", "", 0, KIND_CONST1, 0, -1, -1);
    for (int i = 0; i < g_jed.nsignals; i++) {
        int k = i;
        for (; k > 0 && compare_names(g_jed.pins[order[k - 1]].name, g_jed.pins[i + 1].name) > 0; k--) {
            order[k] = order[k - 1];
        }
        order[k] = i + 1;
    }
    for (int i = 0; i < g_jed.nsignals; i++) {
        int p = order[i];
        jpin_t *pin = &g_jed.pins[p];
        int low = pin->low ? SYMF_LOW : 0;
        int kind = p > g_jed.npins ? KIND_NODE : KIND_PIN;
        int ext_kind = p > g_jed.npins ? KIND_NODE_EXT : KIND_PIN_EXT;
        int used = (pin->column >= 0 && g_jed.columns[pin->column].used) ? SYMF_INPUT : 0;
        if (pin->alias >= 0)
            continue;
        if (g_jed.atf && registered_node(pin->node))
            used = SYMF_INPUT;
        if (pin->node < 0) {
            pin->sym = (int16_t)add_symbol(pin->name, "", p, kind, used | low, pin->in_rec, -1);
            if (pin->named && pin->off_enable >= 0) {
                pin->enable = pin->off_enable;
                pin->enable_sym = (int16_t)add_symbol(pin->name, "oe", p, ext_kind, SYMF_OUTPUT, -1, pin->enable);
            }
            continue;
        }
        if (registered_node(pin->node)) {
            pin->sym = (int16_t)add_symbol(pin->name, "", p, kind, used | low,
                                           pin->reg_rec >= 0 ? pin->reg_rec : pin->in_rec, pin->node);
            pin->out_sym = (int16_t)add_symbol(pin->name,
                dev_node(pin->node)[NODE_TYPE] == NT_TOGGLE ? "t" : "d",
                p, ext_kind, SYMF_OUTPUT | low, -1, pin->node);
            if (g_jed.atf && p <= g_jed.npins && pin->io_rec >= 0)
                pin->io_sym = (int16_t)add_symbol(pin->name, "io", p, ext_kind,
                    SYMF_INPUT, pin->io_rec, pin->node);
        } else {
            pin->sym = (int16_t)add_symbol(pin->name, "", p, kind, SYMF_OUTPUT | used | low, pin->in_rec,
                                           pin->node);
            pin->out_sym = pin->sym;
        }
        if (pin->reset >= 0) {
            pin->reset_sym = (int16_t)add_symbol(pin->name, "ar", p, ext_kind, SYMF_OUTPUT, -1, pin->reset);
        }
        if (pin->enable >= 0) {
            pin->enable_sym = (int16_t)add_symbol(pin->name, "oe", p, ext_kind, SYMF_OUTPUT, -1, pin->enable);
        }
        if (pin->preset >= 0) {
            pin->preset_sym = (int16_t)add_symbol(pin->name, "sp", p, ext_kind, SYMF_OUTPUT, -1, pin->preset);
        }
        if (pin->clock >= 0) {
            pin->clock_sym = (int16_t)add_symbol(pin->name,
                dev_node(pin->clock)[NODE_TYPE] == NT_CLOCK_ENABLE ? "ce" : "ck",
                p, ext_kind, SYMF_OUTPUT, -1, pin->clock);
        }
        if (pin->clock_select >= 0) {
            pin->clock_select_sym = (int16_t)add_symbol(pin->name, "ckmux", p,
                ext_kind, SYMF_OUTPUT, -1, pin->clock_select);
        }
    }
    for (int p = 1; p <= g_jed.nsignals; p++) {
        jpin_t *pin = &g_jed.pins[p];
        if (pin->alias >= 0)
            pin->sym = g_jed.pins[pin->alias].sym;
        for (int c = 0; c < g_jed.ncolumns; c++) {
            column_t *col = &g_jed.columns[c];
            if (g_jed.atf) {
                if (pin->in_rec >= 0 &&
                    (dev_pin(pin->in_rec)[PIN_TRUE_COL] == col->true_col ||
                     dev_pin(pin->in_rec)[PIN_TRUE_COL] == col->comp_col)) {
                    col->sym = pin->io_sym >= 0 ? pin->io_sym : pin->sym;
                    col->inverted = dev_pin(pin->in_rec)[PIN_TRUE_COL] != col->true_col;
                }
                if (pin->alias >= 0)
                    continue;
                if (pin->reg_rec >= 0 &&
                    (dev_pin(pin->reg_rec)[PIN_TRUE_COL] == col->true_col ||
                     dev_pin(pin->reg_rec)[PIN_TRUE_COL] == col->comp_col) &&
                    (p > g_jed.npins || registered_node(pin->node))) {
                    col->sym = pin->sym;
                    col->inverted = dev_pin(pin->reg_rec)[PIN_TRUE_COL] != col->true_col;
                    /* ATF750 feedback taps Q before the output inverter.
                     * The shared register model publishes feedback with
                     * output polarity applied; undo it for the array. */
                    if (g_jed.atf == 750 && p <= g_jed.npins &&
                        dev_node(pin->node)[5] == 6)
                        col->inverted = !col->inverted;
                }
            } else if (col->pin == p) {
                col->sym = pin->sym;
            }
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
    for (int p = 1; p <= g_jed.nsignals; p++) {
        const jpin_t *pin = &g_jed.pins[p];
        const int16_t node[6] = { pin->node, pin->enable, pin->reset, pin->preset, pin->clock, pin->clock_select };
        const int16_t sym[6] = { pin->out_sym, pin->enable_sym, pin->reset_sym, pin->preset_sym,
                                 pin->clock_sym, pin->clock_select_sym };
        for (int k = 0; k < 6; k++) {
            int target = k == 0 && g_jed.atf ? pin->array : (int)rd16(dev_node(node[k]) + NODE_ARRAY);
            if (node[k] >= 0 && sym[k] >= 0 && target == array) {
                if (outputs != NULL) {
                    bit_set(outputs + 1, sym[k]);
                }
                n++;
            }
        }
    }
    return n;
}

static int global_clock_symbol(void)
{
    for (int p = 1; p <= g_jed.npins; p++) {
        int rec = g_jed.pins[p].in_rec;
        if (rec >= 0 && (dev_pin(rec)[PIN_KIND] == 5 || dev_pin(rec)[PIN_KIND] == 12))
            return g_jed.pins[p].sym;
    }
    fatal_error(ERR_JEDEC_DEVICE, "device library lacks a global clock pin");
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
    int clock_select = g_jed.atf && width == 1;
    int rows = clock_select ? 1 : array_rows(array);
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
        if (clock_select) {
            int clock = global_clock_symbol();
            bit_set(used + 1, clock);
            bit_set(high + 1, clock);
            continue;
        }
        if (r + 1 < f->nrows) {
            used[0] |= 1;           /* another term follows */
        }
        if (rows == 0 || !row_selected(array, r) || row_is_false(row, width)) {
            bit_set(used + 1, 0);
            bit_set(high + 1, 0);
            continue;
        }
        inputs = 0;
        for (int c = 0; c < g_jed.ncolumns; c++) {
            const column_t *col = &g_jed.columns[c];
            int sym = col->sym;
            if (sym < 0)
                fatal_error(ERR_JEDEC_DEVICE, "unbound array input");
            if (fuse(row + col->true_col) == 0) {
                bit_set(used + 1, sym);
                if (!col->inverted)
                    bit_set(high + 1, sym);
                inputs++;
            } else if (fuse(row + col->comp_col) == 0) {
                bit_set(used + 1, sym);
                if (col->inverted)
                    bit_set(high + 1, sym);
                inputs++;
            }
        }
        if (inputs == 0) {
            bit_set(used + 1, 1);
            bit_set(high + 1, 1);
        }
        if (g_jed.atf && g_jed.gated_clock[array]) {
            int clock = global_clock_symbol();
            if (bit_test(used + 1, clock) && !bit_test(high + 1, clock)) {
                /* The row contains !CLK as well as the clock gate. */
                bit_set(used + 1, 0);
                bit_set(high + 1, 0);
            } else {
                bit_set(used + 1, clock);
                bit_set(high + 1, clock);
            }
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
 * whose pin numbers these are.  Everything else in the file is skipped.
 *
 * The declarations are read from the text as the define pass of cupl's
 * preprocessor leaves it (src/cupl/preprocess.c): $INCLUDE files are read
 * in place, names defined with $DEFINE are replaced by their text, and
 * $IFDEF / $IFNDEF / $ELSE / $ENDIF choose what is read.  The macro pass
 * is not carried out: declarations that $REPEAT or $MACRO make are not
 * found.
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
    if (strcasecmp(word, "pin") == 0 || strcasecmp(word, "pinnode") == 0) {
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
    static const char *const k_words[5] = { "pin", "pinnode", "field", "fld", "device" };
    skip_blanks(&s);
    for (int i = 0; i < 5; i++) {
        size_t len = strlen(k_words[i]);
        if (strncasecmp(s, k_words[i], len) == 0 && !is_alnum(s[len]) && s[len] != '_') {
            return 1;
        }
    }
    return 0;
}

/* -- the define pass ------------------------------------------------------
 * As in cupl, character by character and with its comments in the text: a
 * defined name is replaced wherever it stands and no letter follows it,
 * the longest name first; its text is not looked at again.  Characters
 * wait in `pending` as long as a name could start with them.  So a name of
 * the greatest length is replaced whatever follows it, and what is pending
 * in front of a directive line is written as it is: a name that stands
 * there, closer to the end of its line than the longest name is long, is
 * not replaced. */
static void pp_write(int c)
{
    if (g_pp.length < PLD_MAX_TEXT) {
        g_pp.text[g_pp.length++] = (char)c;
    }
}

/* The defined name at the front of the pending characters, NULL if none */
static const pld_define_t *pp_match(void)
{
    const pld_define_t *best = NULL;
    for (int i = 0; i < g_pp.ndefines; i++) {
        const pld_define_t *d = &g_pp.defines[i];
        if (!d->defined || d->length > g_pp.npending || (best != NULL && d->length <= best->length) ||
            memcmp(g_pp.pending, d->name, (size_t)d->length) != 0) {
            continue;
        }
        if (d->length == g_pp.npending || !is_alpha((unsigned char)g_pp.pending[d->length])) {
            best = d;
        }
    }
    return best;
}

static void pp_drop(int count)
{
    memmove(g_pp.pending, g_pp.pending + count, (size_t)(g_pp.npending - count));
    g_pp.npending -= count;
}

static void pp_replace(const pld_define_t *d)
{
    pp_drop(d->length);
    for (const char *v = d->value; *v != '\0'; v++) {
        pp_write(*v);
    }
}

/* One character of the text */
static void pp_put(int c)
{
    const pld_define_t *d;
    g_pp.pending[g_pp.npending++] = (char)c;
    if (g_pp.npending > g_pp.longest) {
        pp_write(g_pp.pending[0]);
        pp_drop(1);
    }
    if (g_pp.npending >= g_pp.longest && (d = pp_match()) != NULL) {
        pp_replace(d);
    }
}

/* Write what is pending: at the end of a file names are still replaced,
 * in front of a directive line they are not. */
static void pp_flush(int replacing)
{
    while (g_pp.npending > 0) {
        const pld_define_t *d = replacing ? pp_match() : NULL;
        if (d != NULL) {
            pp_replace(d);
        } else {
            pp_write(g_pp.pending[0]);
            pp_drop(1);
        }
    }
}

static pld_define_t *pp_find(const char *name)
{
    for (int i = 0; i < g_pp.ndefines; i++) {
        if (strcmp(g_pp.defines[i].name, name) == 0) {
            return &g_pp.defines[i];
        }
    }
    return NULL;
}

/* The next word of a directive line (anything up to a blank), "" at its end */
static void pp_word(const char **s, char *out, size_t size)
{
    size_t len = 0;
    while (**s == ' ' || **s == '\t') {
        (*s)++;
    }
    for (; **s != '\0' && **s != ' ' && **s != '\t'; (*s)++) {
        if (len + 1 < size) {
            out[len++] = **s;
        }
    }
    out[len] = '\0';
}

/* $DEFINE name text: the text is the rest of the line without a comment
 * and the blanks in front of it.  A name that is defined keeps its text. */
static void pp_define(const char *s)
{
    char name[PLD_MAX_DEFINE_NAME + 2];
    pld_define_t *d;
    size_t len;
    pp_word(&s, name, sizeof(name));
    if (name[0] == '\0' || strlen(name) > PLD_MAX_DEFINE_NAME) {
        return;
    }
    d = pp_find(name);
    if (d != NULL && d->defined) {
        return;
    }
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    len = strstr(s, "/*") != NULL ? (size_t)(strstr(s, "/*") - s) : strlen(s);
    for (; len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t'); len--) {
    }
    if (d == NULL) {
        if (g_pp.ndefines >= PLD_MAX_DEFINES) {
            return;
        }
        d = &g_pp.defines[g_pp.ndefines++];
        d->name = dup_string(name, "initialization");
        d->length = (int)strlen(name);
        d->defined = 1;
        /* as in cupl, only a new name changes how far ahead is looked */
        g_pp.longest = 0;
        for (int i = 0; i < g_pp.ndefines; i++) {
            if (g_pp.defines[i].defined && g_pp.defines[i].length > g_pp.longest) {
                g_pp.longest = g_pp.defines[i].length;
            }
        }
    }
    d->defined = 1;
    d->value = alloc_tracked(len + 1, "initialization");
    memcpy(d->value, s, len);
    d->value[len] = '\0';
}

static void pp_read_file(FILE *fp, int depth);

/* $INCLUDE file: the file is looked for as cupl does, next to the design
 * and in the directory of the device library and its Macro directory. */
static void pp_include(const char *s, int depth)
{
    static const char *const k_sub[3] = { "", "", "Macro/" };
    char name[260];
    const char *dirs[3];
    char *lib_dir = dup_string(g_args[2] != NULL ? g_args[2] : "", "initialization");
    char *slash = strrchr(lib_dir, '/');
    FILE *fp;
    pp_word(&s, name, sizeof(name));
    if (name[0] == '\0' || depth >= PLD_MAX_NESTING) {
        return;
    }
    if (slash != NULL) {
        slash[1] = '\0';
    } else {
        lib_dir[0] = '\0';
    }
    dirs[0] = g_dir_prefix;
    dirs[1] = dirs[2] = lib_dir;
    fp = open_file(name, "rt");
    for (int i = 0; fp == NULL && i < 3 && strchr(name, '/') == NULL && strchr(name, '\\') == NULL; i++) {
        size_t size = strlen(dirs[i]) + strlen(k_sub[i]) + strlen(name) + 1;
        char *path = alloc_tracked(size, "initialization");
        snprintf(path, size, "%s%s%s", dirs[i], k_sub[i], name);
        if (dirs[i][0] != '\0') {
            fp = open_file(path, "rt");
        }
        free_tracked(path);
    }
    free_tracked(lib_dir);
    if (fp != NULL) {
        pp_read_file(fp, depth + 1);
    }
}

/* A line that starts with '$'; s is what follows the '$'.  The directives
 * of the macro pass are left out; for the define pass an $INCLUDE file is
 * already part of the text. */
static void pp_directive(const char *s, int depth)
{
    static const char *const k_define_pass[6] = { "ifdef", "ifndef", "else", "endif", "define", "undef" };
    char word[16], name[PLD_MAX_DEFINE_NAME + 2];
    pld_define_t *d;
    pp_word(&s, word, sizeof(word));
    copy_lowercase(word, word);
    for (int i = 0; i < 6; i++) {
        if (strcmp(word, k_define_pass[i]) == 0) {
            pp_flush(0);
        }
    }
    if (strcmp(word, "ifdef") == 0 || strcmp(word, "ifndef") == 0) {
        pp_word(&s, name, sizeof(name));
        d = pp_find(name);
        if (g_pp.nesting < PLD_MAX_NESTING) {
            g_pp.nesting++;
            g_pp.outer[g_pp.nesting] = (uint8_t)g_pp.reading;
            g_pp.taken[g_pp.nesting] = (uint8_t)((d != NULL && d->defined) == (word[2] == 'd'));
            g_pp.reading = g_pp.reading && g_pp.taken[g_pp.nesting];
        }
    } else if (strcmp(word, "else") == 0) {
        if (g_pp.nesting > 0) {
            g_pp.reading = g_pp.outer[g_pp.nesting] && !g_pp.taken[g_pp.nesting];
        }
    } else if (strcmp(word, "endif") == 0) {
        if (g_pp.nesting > 0) {
            g_pp.reading = g_pp.outer[g_pp.nesting--];
        }
    } else if (!g_pp.reading) {
        return;
    } else if (strcmp(word, "define") == 0) {
        pp_define(s);
    } else if (strcmp(word, "undef") == 0) {
        pp_word(&s, name, sizeof(name));
        d = pp_find(name);
        if (d != NULL) {
            d->defined = 0;
        }
    } else if (strcmp(word, "include") == 0) {
        pp_include(s, depth);
    }
}

/* One file: a line that starts with '$' is a directive, the other lines
 * are text. */
static void pp_read_file(FILE *fp, int depth)
{
    size_t size = 256, len = 0;
    char *line = alloc_tracked(size, "initialization");
    const char *start;
    int c;
    do {
        c = read_char(fp);
        if (c != '\n' && c != EOF) {
            if (len + 2 > size) {
                char *grown = alloc_tracked(size * 2, "initialization");
                memcpy(grown, line, len);
                free_tracked(line);
                line = grown;
                size *= 2;
            }
            line[len++] = (char)(c == '\0' || c == '\r' ? ' ' : c);
            continue;
        }
        line[len] = '\0';
        for (start = line; *start == ' ' || *start == '\t'; start++) {
        }
        if (*start == '$') {
            pp_directive(start + 1, depth);
        } else if (g_pp.reading) {
            for (size_t i = 0; i < len; i++) {
                pp_put(line[i]);
            }
            pp_put('\n');
        }
        len = 0;
    } while (c != EOF);
    free_tracked(line);
    close_file(fp);
}

/* Blank out the comments of the text */
static void blank_comments(char *text)
{
    int in_comment = 0;
    for (char *s = text; *s != '\0'; s++) {
        if (!in_comment && s[0] == '/' && s[1] == '*') {
            in_comment = 1;
            *s++ = ' ';
            *s = ' ';
        } else if (in_comment && s[0] == '*' && s[1] == '/') {
            in_comment = 0;
            *s++ = ' ';
            *s = ' ';
        } else if (in_comment && *s != '\n') {
            *s = ' ';
        }
    }
}

/* Read the declarations of <design>.pld, if the file is there. */
static void read_pld(void)
{
    char *name = make_file_name(g_dir_prefix, g_base_name, ".pld");
    FILE *fp = open_file(name, "rt");
    char *text;
    memset(&g_pld, 0, sizeof(g_pld));
    if (fp == NULL) {
        return;
    }
    g_pld.file = name;
    g_pld.pins = alloc_tracked(PLD_MAX_PINS * sizeof(pld_pin_t), "initialization");
    memset(&g_pp, 0, sizeof(g_pp));
    g_pp.reading = 1;
    text = g_pp.text = alloc_tracked(PLD_MAX_TEXT + 2, "initialization");
    pp_read_file(fp, 0);
    pp_flush(1);
    text[g_pp.length] = '\0';
    blank_comments(text);
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
    for (int p = 1; p <= g_jed.nsignals; p++) {
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
        if (decl->number < 1 || decl->number > g_jed.nsignals || g_jed.pins[decl->number].named) {
            continue;
        }
        for (int p = 1; p <= g_jed.nsignals; p++) {
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
    for (int p = 1; p <= g_jed.nsignals; p++) {
        jpin_t *pin = &g_jed.pins[p];
        for (int q = 1; q <= g_jed.nsignals && !pin->named; q++) {
            if (g_jed.pins[q].named && strcmp(g_jed.pins[q].name, pin->name) == 0) {
                snprintf(pin->name, sizeof(pin->name), "PIN%d", p);
                break;
            }
        }
    }
}

/* A macrocell whose fuses make it an input does not drive its pin: product
 * terms it has are without effect.  cupl writes such a map for a design
 * that is forced into the simple mode of a GAL16V8 or GAL20V8 and reads an
 * output back: the compiled design then simulates as written, the device
 * does what the map says. */
static void warn_dead_terms(void)
{
    for (int p = 1; p <= g_jed.npins; p++) {
        const jpin_t *pin = &g_jed.pins[p];
        char text[NAME_SIZE + 16];
        if (pin->node >= 0 || pin->array < 0 || array_rows(pin->array) == 0) {
            continue;
        }
        if (pin->named) {
            snprintf(text, sizeof(text), "%d (%s)", p, pin->name);
        } else {
            snprintf(text, sizeof(text), "%d", p);
        }
        print_warning_now(ERR_JEDEC_DEAD_TERMS, text);
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
            int is = pin->node < 0 ? 0 : (registered_node(pin->node) ? 2 : 1);
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
    int numbered = name[0] == 'P' && name[1] >= '1' && name[1] <= '9';
    if (numbered) {
        for (const char *p = name + 1; *p != '\0'; p++) {
            if (*p < '0' || *p > '9' || number > MAX_PINS)
                return NULL;
            number = number * 10 + (*p - '0');
        }
    } else {
        number = pin_by_name(name);
    }
    if (number < 1 || number > g_jed.nsignals)
        return NULL;
    if (g_jed.pins[number].alias >= 0) {
        const jpin_t *pin = &g_jed.pins[g_jed.pins[number].alias];
        /* An ORDER alias observes the same FF before the pin's polarity
         * buffer. Translate the expectation; do not create another FF. */
        *inverted = pin->low ^ (!numbered && g_jed.pins[number].low) ^
                    (dev_node(pin->node)[5] == 6);
        return &g_syms[pin->out_sym];
    }
    if (!numbered || !g_jed.pins[number].named)
        return NULL;
    *inverted = g_jed.pins[number].low;
    return find_symbol(g_jed.pins[number].name);
}

/* Only the interactive simulator uses adjacent source files for names
 * and prints the pin summary. */
void load_jedec_file(const char *jed_name, int source_names)
{
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
    memset(&g_pld, 0, sizeof(g_pld));
    if (source_names) {
        read_pld();
    }
    choose_device(jed_name, !source_names);
    /* option j adds the vectors to the file that was read */
    lincupl_strcpy_trunc(g_abs_jedec_name, sizeof(g_abs_jedec_name), g_base_name);
    g_options |= OPT_N;
    read_header_items(design);
    collect_pins();
    scan_arrays();
    if (g_jed.atf)
        configure_atf();
    else
        configure_macrocells();
    name_pins();
    if (!g_jed.atf)
        warn_dead_terms();
    build_symbols();
    build_frames();
    define_pld_fields();
    if (source_names) {
        print_summary(jed_name);
    }
}

/* Build the design from <design>.jed: the counterpart of load_design(). */
void load_design_jedec(void)
{
    load_jedec_file(make_file_name(g_dir_prefix, g_base_name, ".jed"), 1);
}

/* Describe the physical pins of the map already decoded above. */
void jedec_truth_pins(sim_truth_t *table)
{
    table->npins = (unsigned)g_jed.npins;
    snprintf(table->device, sizeof(table->device), "%s", g_device_name);
    for (int p = 1; p <= g_jed.npins; p++) {
        const jpin_t *pin = &g_jed.pins[p];
        if (pin->node >= 0) {
            table->role[p] = registered_node(pin->node) ?
                             SIM_PIN_REGISTER : SIM_PIN_OUTPUT;
            table->outputs[table->noutputs++] = (unsigned)p;
        } else if (pin->in_rec >= 0) {
            int kind = dev_pin(pin->in_rec)[PIN_KIND];
            if (kind == 2 || kind == 3 || (pin->column < 0 && kind == 0)) {
                continue;
            }
            table->role[p] = SIM_PIN_INPUT;
            if ((pin->column >= 0 && g_jed.columns[pin->column].used) || kind == PIN_KIND_ENABLE) {
                table->inputs[table->ninputs++] = (unsigned)p;
            }
        }
    }
}
