/*
 * expand_device.c - the device in the expand phase.
 *
 * Loads the target device from the device library, binds every symbol to
 * the device's pin functions and macrocell nodes, adds symbols for unused
 * node functions that need a default, checks usage conflicts and decides
 * the polarity in which each equation is implemented.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "expand_int.h"

/* -------------------------------------------------------------------------
 * State
 * ------------------------------------------------------------------------- */
FILE *expand_lib_file;
static uint8_t g_lib_header[DL_HEADER_SIZE];
uint8_t expand_lib_entry[DL_ENTRY_SIZE];
uint8_t *expand_dev;
size_t expand_dev_size;
static int g_pin_limit;
static int g_node_limit;
static int g_next_hidden;
static int g_next_node;
static symbol_t **g_node_owner;        /* first user of a numbered node */
static symbol_t **g_node_alias;
static int g_reg_outputs[4];           /* outputs counted by kind, see count_output() */
static int g_bank_ext[4];              /* function used by each bank, -1 if none yet */

/* Usage maps: 16-bit words, word 0 unused */
uint16_t *expand_conflict_map;
static uint8_t *g_line_owner;
static uint8_t *g_reg_count;
static uint16_t *g_pinfn_used;
static uint16_t *g_nodefn_used;
uint16_t *expand_nodefn_default;
static uint16_t *g_line_used;
static uint16_t *g_pin_output;
uint16_t *expand_pin_foreign;
size_t expand_conflict_words, expand_nodefn_words, expand_pin_words;
static size_t g_line_owner_size, g_reg_count_size, g_pinfn_words, g_line_words;

/* -------------------------------------------------------------------------
 * Device library (see devlib.h)
 * ------------------------------------------------------------------------- */

/* Open the device library (argv[2]) and check its header. */
void expand_open_library(void)
{
    const char *path = expand_args[2];
    expand_lib_file = expand_open_file(path, "rb");
    if (expand_lib_file == NULL) {
        expand_fatal_error(ERR_OPEN, path);
    }
    memset(g_lib_header, 0, sizeof(g_lib_header));
    switch (dl_read_header(expand_lib_file, g_lib_header)) {
    case DL_OK:
        break;
    case DL_BAD_KEY:
        expand_fatal_error(ERR_LIB_KEY, "");
        break;
    default:
        expand_fatal_error(ERR_BAD_LIBRARY, path);
        break;
    }
}

/* Read the image of the device found by dl_find_device(); some of it is
 * enough */
static int read_device_image(void)
{
    uint32_t length = dl_image_size(expand_lib_entry);
    if (!dl_seek_image(expand_lib_file, expand_lib_entry)) {
        return 0;
    }
    expand_dev = expand_alloc_tracked(length, "device library processing");
    expand_dev_size = length;
    return dl_read_device_image(expand_lib_file, expand_lib_entry, expand_dev, length) != 0;
}

/* Load a device from the library and set the pin and node limits.
 * Families 8 and 9 are virtual devices. */
void expand_load_device(const char *device)
{
    if (!dl_find_device(expand_lib_file, device, DL_MATCH_CASE, expand_lib_entry)) {
        expand_fatal_error(ERR_NO_DEVICE, device);
    }
    if (!read_device_image()) {
        expand_fatal_error(ERR_BAD_LIBRARY, device);
    }
    if (dev_u16(dev_info(INFO_VALID)) == 0) {
        expand_fatal_error(ERR_NOT_SUPPORTED, device);
    }
    unsigned family = dev_u8(dev_info(INFO_FAMILY));
    if (family == 8 || family == 9) {
        expand_mode = "1";
    }
    g_pin_limit = (int)(dev_u16(dev_info(INFO_PIN_COUNT)) + dev_u8(dev_info(INFO_EXTRA_A)) +
                        dev_u8(dev_info(INFO_EXTRA_B)) + dev_u8(dev_info(INFO_EXTRA_C)));
    g_node_limit = (int)dev_u8(dev_info(INFO_NODE_BASE)) + g_pin_limit + 1;
}

/* -------------------------------------------------------------------------
 * Binding symbols to the device
 * ------------------------------------------------------------------------- */
static int ext_is(const symbol_t *sym, const char *ext)
{
    return strncmp(sym->ext, ext, SYMBOL_EXT_SIZE) == 0;
}

static int same_name(const symbol_t *a, const symbol_t *b)
{
    return strncmp(a->name, b->name, SYMBOL_NAME_SIZE) == 0;
}

static int device_is(const char *name)
{
    return strcmp(expand_device_name, name) == 0;
}

/* Code of a symbol's extension, 0 without extension, -1 if the
 * device does not know it.  The extension is converted to lower case. */
static int extension_code(symbol_t *sym)
{
    char ext[SYMBOL_EXT_SIZE + 1];
    if (sym->ext[0] == '\0' || sym->ext[0] == ' ') {
        return 0;
    }
    for (int i = 0; i < SYMBOL_EXT_SIZE && sym->ext[i] != '\0'; i++) {
        if (sym->ext[i] >= 'A' && sym->ext[i] <= 'Z') {
            sym->ext[i] = (char)(sym->ext[i] - 'A' + 'a');
        }
    }
    snprintf(ext, sizeof(ext), "%.*s", SYMBOL_EXT_SIZE, sym->ext);
    long rec = (long)dev_u32(DEV_EXT_PTR);
    int count = (int)dev_u16(DEV_EXT_COUNT);
    for (int i = 0; i < count; i++, rec += EXT_SIZE) {
        char name[EXT_CODE + 1];
        dev_string(rec, name, sizeof(name));
        if (strcmp(ext, name) == 0) {
            return (int)dev_u8(rec + EXT_CODE);
        }
    }
    return dev_u8(dev_info(INFO_FAMILY)) == 9 ? 0 : -1;
}

/* Name of an extension code */
static int extension_name(int code, char *out, size_t size)
{
    long rec = (long)dev_u32(DEV_EXT_PTR);
    int count = (int)dev_u16(DEV_EXT_COUNT);
    for (int i = 0; i < count; i++, rec += EXT_SIZE) {
        if ((int)dev_u8(rec + EXT_CODE) == code) {
            dev_string(rec, out, size);
            return 1;
        }
    }
    return 0;
}

static int not_found(void)
{
    return is_virtual_device() ? 0 : -1;
}

/* Pin function of a pin for an extension, preferably the given
 * variant.  Pin numbers are deliberately compared modulo 256 here and below;
 * this decides which function is found for pin numbers above 255. */
static int find_pin_function(unsigned pin, int ext, int variant)
{
    int count = (int)dev_u16(DEV_PINFN_COUNT);
    for (int i = 0; i < count; i++) {
        if (dev_u16(dev_pinfn(i, PINFN_PIN)) == (pin & 0xff) &&
            dev_u8(dev_pinfn(i, PINFN_EXT)) == (unsigned)(ext & 0xff) &&
            dev_u8(dev_pinfn(i, PINFN_VARIANT)) == (unsigned)variant) {
            return i;
        }
    }
    for (int i = 0; i < count; i++) {
        if (dev_u16(dev_pinfn(i, PINFN_PIN)) == (pin & 0xff) &&
            dev_u8(dev_pinfn(i, PINFN_EXT)) == (unsigned)(ext & 0xff)) {
            return i;
        }
    }
    return not_found();
}

/* Pin function of a pin with the given variant, any extension */
static int find_pin_variant(unsigned pin, int variant)
{
    int count = (int)dev_u16(DEV_PINFN_COUNT);
    for (int i = 0; i < count; i++) {
        if (dev_u16(dev_pinfn(i, PINFN_PIN)) == (pin & 0xff) &&
            dev_u8(dev_pinfn(i, PINFN_VARIANT)) == (unsigned)(variant & 0xff)) {
            return i;
        }
    }
    return not_found();
}

/* Node function of a pin for an extension */
static int find_node_function(unsigned pin, int ext)
{
    int count = (int)dev_u16(DEV_NODEFN_COUNT);
    for (int i = 0; i < count; i++) {
        if (dev_u16(dev_nodefn(i, NODEFN_PIN)) == (pin & 0xff) &&
            dev_u8(dev_nodefn(i, NODEFN_EXT)) == (unsigned)(ext & 0xff)) {
            return i;
        }
    }
    return not_found();
}

/* As find_node_function(), but only functions with polarity 1; -1 if
 * there is none */
static int find_active_node_function(unsigned pin, int ext)
{
    int count = (int)dev_u16(DEV_NODEFN_COUNT);
    for (int i = 0; i < count; i++) {
        if (dev_u16(dev_nodefn(i, NODEFN_PIN)) == (pin & 0xff) &&
            dev_u8(dev_nodefn(i, NODEFN_EXT)) == (unsigned)(ext & 0xff) &&
            dev_u8(dev_nodefn(i, NODEFN_POLARITY)) == 1) {
            return i;
        }
    }
    return -1;
}

/* Node function with the given extension on a product term line */
static int find_line_function(int ext, unsigned line)
{
    int count = (int)dev_u16(DEV_NODEFN_COUNT);
    for (int i = 0; i < count; i++) {
        if (dev_u16(dev_nodefn(i, NODEFN_LINE)) == line &&
            dev_u8(dev_nodefn(i, NODEFN_EXT)) == (unsigned)ext) {
            return i;
        }
    }
    return not_found();
}

static int pin_has_d_extension(unsigned pin)
{
    for (int i = 0; i < expand_symbol_count; i++) {
        if (expand_symbols[i].pin == pin && ext_is(&expand_symbols[i], "d")) {
            return 1;
        }
    }
    return 0;
}

/* Another symbol of the same name has the .dq extension. */
static int has_dq_twin(const symbol_t *sym)
{
    for (int i = 0; i < expand_symbol_count; i++) {
        const symbol_t *s = &expand_symbols[i];
        if (same_name(s, sym) && (ext_is(s, "dq") || ext_is(s, "DQ"))) {
            return 1;
        }
    }
    return 0;
}

/* g20xv10 only.  For the plain output (extension code 1): no .oe
 * equation exists for the pin; otherwise: an .oemux equation exists. */
static int uses_alternate_output(unsigned pin, int ext_code)
{
    for (int i = 0; i < expand_symbol_count; i++) {
        const symbol_t *s = &expand_symbols[i];
        if (s->pin != pin) {
            continue;
        }
        if (ext_code == 1) {
            if (ext_is(s, "OE") || ext_is(s, "oe")) {
                return 0;
            }
        } else if (ext_is(s, "OEMUX") || ext_is(s, "oemux")) {
            return 1;
        }
    }
    return ext_code == 1;
}

/* The pin polarity does not apply to these functions */
static void clear_pin_polarity(symbol_t *sym)
{
    unsigned polarity = dev_u8(dev_nodefn(sym->node_fn, NODEFN_POLARITY));
    if (polarity == 4 || polarity == 5) {
        sym->flags &= (uint8_t)~SYMF_PIN_LOW;
    }
    if (is_virtual_device()) {
        int code = sym->ext_code;
        if (code == 2 || code == 3 || (code >= 5 && code <= 7) || (code >= 0x2a && code <= 0x2c) ||
            code == 0x43) {
            sym->flags &= (uint8_t)~SYMF_PIN_LOW;
        }
    }
}

/* Look up the pin function, node function and output function
 * of a symbol */
static void bind_functions(symbol_t *sym)
{
    int has_d = pin_has_d_extension(sym->pin);
    sym->pin_fn = (int16_t)find_pin_function(sym->pin, sym->ext_code, has_d);
    sym->node_fn = (int16_t)find_node_function(sym->pin, sym->ext_code);
    if (sym->node_fn > 0 && dev_u8(dev_nodefn(sym->node_fn, NODEFN_SHARING)) == 4 &&
        has_dq_twin(sym)) {
        sym->node_fn = (int16_t)(sym->node_fn + (int)dev_u8(dev_nodefn(sym->node_fn, NODEFN_ALT_STEP)));
    }
    if (sym->node_fn >= 0) {
        sym->out_fn = (int16_t)find_line_function(0x0e, dev_u16(dev_nodefn(sym->node_fn, NODEFN_LINE)));
        if (strncmp(expand_device_name, "g20xv10", 7) == 0 &&
            dev_u8(dev_nodefn(sym->node_fn, NODEFN_ACTIVE)) != 0 &&
            uses_alternate_output(dev_u16(dev_nodefn(sym->node_fn, NODEFN_PIN)),
                                  (int8_t)sym->ext_code)) {
            sym->out_fn++;
            sym->node_fn = (int16_t)find_active_node_function(sym->pin, sym->ext_code);
        }
        if (sym->out_fn >= 0) {
            clear_pin_polarity(sym);
        }
    }
    if (sym->pin_fn < 0 && sym->node_fn < 0) {
        expand_report_pin_error(ERR_INVALID_USAGE, sym);
        expand_fatal_error(-1, "");
    }
}

/* Bind a pin. */
static void bind_pin(symbol_t *sym)
{
    sym->ext_code = (int16_t)extension_code(sym);
    if (sym->ext_code < 0) {
        expand_report_symbol_error(ERR_UNKNOWN_EXTENSION, sym);
    }
    bind_functions(sym);
}

static symbol_t *node_slot(symbol_t **map, unsigned index)
{
    return (int)index <= g_pin_limit ? map[index] : NULL;
}

/* Bind a node.  One declared without a number waits for the hidden node of
 * the same index to get one. */
static void bind_node(symbol_t *sym)
{
    sym->ext_code = (int16_t)extension_code(sym);
    if (sym->ext_code < 0) {
        expand_report_symbol_error(ERR_UNKNOWN_EXTENSION, sym);
        return;
    }
    unsigned index = sym->pin & 0xfff;
    if ((sym->pin & 0xf000) == 0) {
        bind_functions(sym);
        return;
    }
    if ((int)index > g_pin_limit || g_node_owner[index] != NULL) {
        expand_report_symbol_error(ERR_NO_SUCH_NODE, sym);
        return;
    }
    if (g_node_alias[index] == NULL) {
        g_node_owner[index] = sym;
        return;
    }
    sym->pin = g_node_alias[index]->pin;
    bind_functions(sym);
}

/* Bind a hidden node.  One without a number gets the next free one. */
static void bind_hidden_node(symbol_t *sym)
{
    sym->ext_code = (int16_t)extension_code(sym);
    if (sym->ext_code < 0) {
        expand_report_symbol_error(ERR_UNKNOWN_EXTENSION, sym);
    }
    if ((sym->pin & 0xf000) == 0) {
        bind_functions(sym);
        return;
    }
    unsigned index = sym->pin & 0xfff;
    uint16_t number;
    symbol_t *alias = node_slot(g_node_alias, index);
    if (alias == NULL) {
        if (sym->ext_code == 0x0d) {
            number = (uint16_t)g_next_hidden++;
        } else if (dev_u8(dev_info(INFO_EXTRA_A)) == 0) {
            number = (uint16_t)(index + (unsigned)g_next_node);
        } else {
            number = (uint16_t)g_next_node++;
        }
        for (int i = 0; i < expand_symbol_count; i++) {
            if (expand_symbols[i].pin == number) {
                number++;
                i = -1;
            }
        }
    } else {
        number = alias->pin;
    }
    sym->pin = number;
    if ((int)index <= g_pin_limit) {
        g_node_alias[index] = sym;
    }
    bind_functions(sym);
    if ((int)index > g_pin_limit) {
        expand_report_symbol_error(ERR_INTERNAL_NODE_RANGE, sym);
    } else if (g_node_owner[index] != NULL && g_node_owner[index]->pin != sym->pin) {
        g_node_owner[index]->pin = sym->pin;
        bind_functions(g_node_owner[index]);
    }
}

/* Count the outputs by kind for the choice of an alternative
 * device */
static void count_output(const symbol_t *sym)
{
    if (sym->node_fn < 0 || (sym->flags & SYMF_OUTPUT) == 0) {
        return;
    }
    switch (dev_u8(dev_nodefn(sym->node_fn, NODEFN_EXT))) {
    case 0:
        if (sym->pin_fn >= 0 && (sym->flags & SYMF_INPUT) != 0) {
            g_reg_outputs[2]++;
        }
        break;
    case 1: g_reg_outputs[0]++; break;
    case 2: g_reg_outputs[1]++; break;
    case 0x2b: g_reg_outputs[3]++; break;
    default: break;
    }
}

/* Record which device resources the symbol occupies */
static void mark_usage(symbol_t *sym)
{
    if (sym->pin_fn >= 0 && sym->pin != 0 && dev_u8(dev_info(INFO_FAMILY)) != 9 &&
        sym->polarity != POL_FIXED && sym->ext[0] == '\0') {
        if (!map_test(g_pinfn_used, g_pinfn_words, sym->pin_fn)) {
            map_set(g_pinfn_used, g_pinfn_words, sym->pin_fn);
        } else {
            sym->pin_fn = -1;
            expand_report_pin_error(ERR_REDECLARED, sym);
        }
    }
    if (sym->pin_fn >= 0 && !is_virtual_device() && (sym->flags & SYMF_INPUT) != 0) {
        unsigned line = dev_u16(dev_pinfn(sym->pin_fn, PINFN_LINE));
        if (line < g_line_owner_size) {
            unsigned owner = g_line_owner[line];
            if (owner == 0 || owner == (unsigned)sym->pin_fn || device_is("p7c330") ||
                device_is("p7c331") ||
                dev_u8(dev_pinfn((int)owner, PINFN_ARCH_A)) != dev_u8(dev_pinfn(sym->pin_fn, PINFN_ARCH_A)) ||
                dev_u8(dev_pinfn((int)owner, PINFN_ARCH_B)) != dev_u8(dev_pinfn(sym->pin_fn, PINFN_ARCH_B))) {
                g_line_owner[line] = (uint8_t)sym->pin_fn;
            }
        }
    }
    if (sym->node_fn >= 0 && !is_virtual_device() && sym->polarity != POL_FIXED) {
        if (!map_test(g_nodefn_used, expand_nodefn_words, sym->node_fn)) {
            map_set(g_nodefn_used, expand_nodefn_words, sym->node_fn);
        } else {
            sym->node_fn = -1;
            expand_report_pin_error(ERR_REDECLARED, sym);
        }
        map_set(g_line_used, g_line_words, (int)dev_u16(dev_nodefn(sym->node_fn, NODEFN_LINE)));
        if (sym->flags & SYMF_OUTPUT) {
            map_set(g_pin_output, expand_pin_words, sym->pin);
            unsigned ext = dev_u8(dev_nodefn(sym->node_fn, NODEFN_EXT));
            if (ext >= 8 && ext < 12 && sym->pin < g_reg_count_size) {
                g_reg_count[sym->pin]++;
            }
            unsigned default_ext = dev_u8(dev_info(INFO_DEFAULT_EXT));
            if (default_ext != 0 && dev_u8(dev_nodefn(sym->node_fn, NODEFN_ACTIVE)) != 0 &&
                default_ext != ext) {
                map_set(expand_pin_foreign, expand_pin_words, sym->pin);
            }
        }
    }
}

/* The two halves of a flip-flop (codes 8..11: J/K, S/R) must both
 * be given */
static void check_flipflop_pairs(void)
{
    for (int i = 0; i < expand_symbol_count; i++) {
        const symbol_t *sym = &expand_symbols[i];
        if (sym->node_fn < 0 || (sym->flags & SYMF_OUTPUT) == 0) {
            continue;
        }
        unsigned ext = dev_u8(dev_nodefn(sym->node_fn, NODEFN_EXT));
        if (ext >= 8 && ext < 12 && sym->pin < g_reg_count_size && g_reg_count[sym->pin] == 1) {
            char name[SYMBOL_NAME_SIZE + 1];
            symbol_name(sym, name, sizeof(name));
            expand_report_error(ERR_MISSING_FLIPFLOP, name);
        }
    }
}

/* The device asks for another library entry depending on the
 * kinds of outputs used.  Returns its name or NULL. */
static int pick_alternative_device(char *name, size_t size)
{
    long best = -1;
    unsigned priority = 0;
    long rec = (long)dev_u32(DEV_ALT_PTR);
    int count = (int)dev_u16(DEV_ALT_COUNT);
    for (int i = 0; i < count; i++, rec += ALT_SIZE) {
        int applies = 0;
        switch (dev_u8(rec + ALT_CONDITION)) {
        case 1: applies = g_reg_outputs[0] > 0; break;
        case 2: applies = g_reg_outputs[1] > 0; break;
        case 3: applies = g_reg_outputs[2] > 0; break;
        case 4: applies = 1; break;
        case 5: applies = g_reg_outputs[3] > 0; break;
        default: expand_fatal_error(ERR_INTERNAL_ALT_DEVICE, "");
        }
        if (applies && priority <= dev_u8(rec + ALT_PRIORITY)) {
            priority = dev_u8(rec + ALT_PRIORITY);
            best = rec;
        }
    }
    if (best < 0) {
        return 0;
    }
    dev_string(best, name, size);
    return 1;
}

/* First symbol on the same product term line and in the same
 * group as `sym` */
int expand_find_line_sharer(const symbol_t *sym)
{
    for (int i = 0; i < expand_symbol_count; i++) {
        const symbol_t *s = &expand_symbols[i];
        if (s->node_fn >= 0 &&
            dev_u16(dev_nodefn(s->node_fn, NODEFN_LINE)) == dev_u16(dev_nodefn(sym->node_fn, NODEFN_LINE)) &&
            dev_u32(dev_nodefn(s->node_fn, NODEFN_GROUP)) == dev_u16(dev_nodefn(sym->node_fn, NODEFN_GROUP))) {
            return i;
        }
    }
    return -1;
}

/* First symbol with the same function on the same pin */
static int find_function_user(const symbol_t *sym)
{
    for (int i = 0; i < expand_symbol_count; i++) {
        const symbol_t *s = &expand_symbols[i];
        if (s->node_fn >= 0 &&
            dev_u16(dev_nodefn(s->node_fn, NODEFN_PIN)) == dev_u8(dev_nodefn(sym->node_fn, NODEFN_PIN)) &&
            dev_u8(dev_nodefn(s->node_fn, NODEFN_EXT)) == dev_u8(dev_nodefn(sym->node_fn, NODEFN_EXT)) &&
            dev_u32(dev_nodefn(s->node_fn, NODEFN_GROUP)) == dev_u16(dev_nodefn(sym->node_fn, NODEFN_GROUP))) {
            return i;
        }
    }
    return -1;
}

/* Start an iteration over the link records of a node function. */
static void link_iter_init(link_iter_t *it, int node_fn, int link_class)
{
    it->group_start = -1;
    it->member = -1;
    it->group = 0xffff;
    it->self = 0;
    it->ext = (uint8_t)dev_u8(dev_nodefn(node_fn, NODEFN_EXT));
    it->pin = (uint16_t)dev_u16(dev_nodefn(node_fn, NODEFN_PIN));
    it->line = (uint16_t)dev_u16(dev_nodefn(node_fn, NODEFN_LINE));
    it->link_class = (uint8_t)link_class;
}

/* Next link record of the node function itself */
static int link_iter_next_group(link_iter_t *it)
{
    int count = (int)dev_u16(DEV_LINK_COUNT);
    for (int i = it->group_start < 0 ? 0 : it->group_start + 1; i < count; i++) {
        if (it->pin == dev_u16(dev_link(i, LINK_PIN)) &&
            it->link_class == dev_u8(dev_link(i, LINK_CLASS)) &&
            it->ext == dev_u16(dev_link(i, LINK_EXT)) &&
            it->line == dev_u16(dev_link(i, LINK_LINE))) {
            it->group_start = i;
            it->self = (uint8_t)dev_u8(dev_link(i, LINK_MEMBER));
            it->group = (uint16_t)dev_u16(dev_link(i, LINK_GROUP));
            return i;
        }
    }
    return -1;
}

/* Next other member of the current group */
static int link_iter_next_member(link_iter_t *it)
{
    int count = (int)dev_u16(DEV_LINK_COUNT);
    for (int i = it->member < 0 ? 0 : it->member + 1; i < count; i++) {
        if (it->group == dev_u16(dev_link(i, LINK_GROUP)) &&
            it->self != dev_u8(dev_link(i, LINK_MEMBER)) &&
            it->link_class == dev_u8(dev_link(i, LINK_CLASS)) &&
            it->pin == dev_u16(dev_link(i, LINK_PIN))) {
            it->member = i;
            return i;
        }
    }
    return -1;
}

/* Node function a link record stands for */
static int find_linked_function(int link)
{
    int count = (int)dev_u16(DEV_NODEFN_COUNT);
    for (int i = 0; i < count; i++) {
        if (dev_u8(dev_nodefn(i, NODEFN_DEFAULT)) != 0 &&
            dev_u16(dev_link(link, LINK_LINE)) == dev_u16(dev_nodefn(i, NODEFN_LINE)) &&
            dev_u16(dev_link(link, LINK_PIN)) == dev_u16(dev_nodefn(i, NODEFN_PIN)) &&
            dev_u16(dev_link(link, LINK_EXT)) == dev_u8(dev_nodefn(i, NODEFN_EXT))) {
            return i;
        }
    }
    return -1;
}

/* One of the four banks whose outputs must all use the same function */
static void check_bank(int bank, const symbol_t *extra)
{
    int ext = (int)dev_u8(dev_nodefn(extra->node_fn, NODEFN_EXT));
    if (g_bank_ext[bank] < 0) {
        g_bank_ext[bank] = ext;
    } else if (ext != g_bank_ext[bank]) {
        expand_report_symbol_error(ERR_BANKED_MIX, extra);
    }
}

/* A node function linked to an output has no equation of its
 * own.  Depending on its default rule it is checked, or a symbol is
 * generated for it so that it gets a preset frame. */
static void add_linked_function(const symbol_t *sym, const link_iter_t *it)
{
    if (expand_extra_count >= expand_extra_max) {
        expand_report_error(ERR_INTERNAL_EXTRA_FULL, "");
        return;
    }
    if (sym->out_fn < 0) {
        expand_report_error(ERR_INTERNAL_EXTRA_NO_NODE, "");
        return;
    }
    if (sym->polarity == POL_FIXED) {
        return;
    }
    int fn;
    if (dev_u16(dev_link(it->member, LINK_EXT)) == 0x13 && dev_u16(dev_link(it->member, LINK_PIN)) == 0) {
        fn = sym->node_fn;
    } else {
        fn = find_linked_function(it->member);
        if (fn < 0) {
            return;
        }
    }

    symbol_t *extra = &expand_extra_symbols[expand_extra_count];
    extra->pin = sym->pin;
    extra->kind = SYM_GENERATED;
    extra->flags = sym->flags & SYMF_OUTPUT;
    snprintf(extra->name, SYMBOL_NAME_SIZE, "%.*s", SYMBOL_NAME_SIZE - 1, sym->name);
    extra->pin_fn = -1;
    extra->out_fn = -1;
    extra->node_fn = (int16_t)fn;
    extra->ext_code = (int16_t)dev_u8(dev_nodefn(fn, NODEFN_EXT));
    extra->out_fn = (int16_t)find_line_function(0x0e, dev_u16(dev_nodefn(fn, NODEFN_LINE)));
    if (extra->out_fn < 0) {
        expand_report_symbol_error(ERR_INTERNAL_EXTRA_NO_OUTPUT, sym);
    }
    char ext[EXT_CODE + 1] = "";
    if (extension_name(extra->ext_code, ext, sizeof(ext))) {
        snprintf(extra->ext, SYMBOL_EXT_SIZE, "%s", ext);
    } else if (extra->ext_code == 0) {
        extra->ext[0] = '\0';
    } else {
        expand_report_symbol_error(ERR_INTERNAL_EXTRA_NO_EXT, sym);
    }
    if (strcmp(ext, "oe") == 0) {
        /* an .oemux equation takes the place of the output enable */
        for (int i = 0; i < expand_symbol_count; i++) {
            if (same_name(extra, &expand_symbols[i]) && ext_is(&expand_symbols[i], "oemux")) {
                return;
            }
        }
    }

    switch (dev_u8(dev_nodefn(fn, NODEFN_DEFAULT))) {
    case 1:
    case 2:
        if (find_function_user(extra) >= 0) {
            return;
        }
        if (dev_u8(dev_nodefn(fn, NODEFN_SHARING)) == 1 && expand_find_line_sharer(extra) >= 0) {
            return;
        }
        break;
    case 3:
        if (find_function_user(extra) < 0) {
            expand_report_symbol_warning(ERR_NO_EXPRESSION, extra);
        }
        return;
    case 4:
    case 5:
    case 6:
    case 7:
        check_bank((int)dev_u8(dev_nodefn(fn, NODEFN_DEFAULT)) - 4, extra);
        return;
    case 8:
        if (!map_test(g_nodefn_used, expand_nodefn_words, fn)) {
            expand_report_symbol_error(ERR_NO_EXPRESSION, extra);
        }
        return;
    case 9:
        if (dev_u8(dev_nodefn(sym->node_fn, NODEFN_EXT)) == 0x15) {
            if (map_test(g_nodefn_used, expand_nodefn_words, fn)) {
                return;
            }
        } else {
            if (map_test(g_nodefn_used, expand_nodefn_words, fn)) {
                expand_report_symbol_error(ERR_NO_EXPRESSION_ALLOWED, extra);
                return;
            }
            map_set(expand_nodefn_default, expand_nodefn_words, fn);
        }
        break;
    default:
        break;
    }
    expand_extra_count++;
}

/* Generate symbols for the node functions that go with the
 * outputs in use (output enables, resets, ...) but have no equation */
static void add_linked_functions(void)
{
    expand_extra_max = (int)dev_u16(dev_info(INFO_EXTRA_SYMBOLS)) + 1;
    expand_extra_symbols = expand_alloc_tracked((size_t)(expand_extra_max + 1) * sizeof(symbol_t), "symbol table");
    expand_extra_count = 0;
    for (int i = 0; i < expand_symbol_count; i++) {
        const symbol_t *sym = &expand_symbols[i];
        int is_output = (sym->flags & SYMF_OUTPUT) != 0;
        int is_input = (sym->flags & SYMF_INPUT) != 0;
        if (sym->node_fn < 0 || dev_u8(dev_nodefn(sym->node_fn, NODEFN_HAS_DEFAULT)) == 0) {
            continue;
        }
        if (!is_output && (device_is("ep1800") || !is_input)) {
            continue;
        }
        if (is_input && !is_output && map_test(g_pin_output, expand_pin_words, sym->pin)) {
            continue;
        }
        link_iter_t it;
        link_iter_init(&it, sym->node_fn, 1);
        while (link_iter_next_group(&it) >= 0) {
            it.member = -1;
            while (link_iter_next_member(&it) >= 0) {
                add_linked_function(sym, &it);
            }
        }
    }
}

/* Check exclusive modes whose descriptions use separate overlapping arrays
 * or a bank selector shared by several physical cells. */
static void check_output_modes(void)
{
    for (int i = 0; i < expand_symbol_count; i++) {
        const symbol_t *comb = &expand_symbols[i];
        if (comb->node_fn < 0 || comb->ext_code != 0 || !(comb->flags & SYMF_OUTPUT) ||
            comb->polarity == POL_FIXED) {
            continue;
        }
        for (int j = 0; j < expand_symbol_count; j++) {
            const symbol_t *reg = &expand_symbols[j];
            if (reg->pin == comb->pin && reg->node_fn >= 0 && reg->ext_code == 1 &&
                (reg->flags & SYMF_OUTPUT) && reg->polarity != POL_FIXED) {
                int split_arrays = dev_u8(dev_nodefn(comb->node_fn, NODEFN_SHARING)) == 1 &&
                    dev_u8(dev_nodefn(reg->node_fn, NODEFN_SHARING)) == 1 &&
                    dev_u16(dev_nodefn(reg->node_fn, NODEFN_LINE)) !=
                        dev_u16(dev_nodefn(comb->node_fn, NODEFN_LINE));
                unsigned bank = dev_u8(dev_nodefn(comb->node_fn, NODEFN_DEFAULT));
                int banked_modes = bank >= 4 && bank <= 7 &&
                    dev_u8(dev_nodefn(reg->node_fn, NODEFN_DEFAULT)) == bank;
                if (split_arrays || banked_modes) {
                    expand_report_pin_error(ERR_EXCLUDED_OUTPUT, reg);
                }
            }
        }
    }
    if (expand_error_count > 0) {
        expand_fatal_error(-1, "");
    }
}

/* Allocate the usage maps for the current device and symbol table. */
void expand_alloc_usage_maps(void)
{
    expand_conflict_words = (size_t)expand_symbol_count / 16 + 3;
    expand_conflict_map = expand_alloc_tracked(expand_conflict_words * 2, "bit maps");
    g_line_owner_size = dev_u16(dev_info(INFO_INPUT_LINES)) + 1;
    g_line_owner = expand_alloc_tracked(g_line_owner_size, "bit maps");
    g_reg_count_size = (size_t)g_node_limit + 1;
    g_reg_count = expand_alloc_tracked(g_reg_count_size, "bit maps");
    g_pinfn_words = dev_u16(DEV_PINFN_COUNT) / 16 + 3;
    g_pinfn_used = expand_alloc_tracked(g_pinfn_words * 2, "bit maps");
    expand_nodefn_words = dev_u16(DEV_NODEFN_COUNT) / 16 + 3;
    g_nodefn_used = expand_alloc_tracked(expand_nodefn_words * 2, "bit maps");
    expand_nodefn_default = expand_alloc_tracked(expand_nodefn_words * 2, "bit maps");
    g_line_words = dev_u16(dev_info(INFO_TERM_LINES)) / 16 + 3;
    g_line_used = expand_alloc_tracked(g_line_words * 2, "bit maps");
    expand_pin_words = (size_t)g_node_limit / 16 + 3;
    g_pin_output = expand_alloc_tracked(expand_pin_words * 2, "bit maps");
    expand_pin_foreign = expand_alloc_tracked(expand_pin_words * 2, "bit maps");
}

/* Bind every symbol to the device.  Returns 1 with the name of another
 * library entry if the design has to be bound again to that one. */
int expand_bind_symbols(char *alt_name, size_t alt_size)
{
    g_next_hidden = (int)(dev_u16(dev_info(INFO_PIN_COUNT)) + dev_u8(dev_info(INFO_NODE_BASE)) + 1);
    g_next_node = (int)dev_u8(dev_info(INFO_NODE_BASE)) + 1;
    memset(g_reg_outputs, 0, sizeof(g_reg_outputs));
    g_node_owner = expand_alloc_tracked((size_t)(g_pin_limit + 1) * sizeof(*g_node_owner), "node map");
    g_node_alias = expand_alloc_tracked((size_t)(g_pin_limit + 1) * sizeof(*g_node_alias), "node map");

    for (int i = 0; i < expand_symbol_count; i++) {
        symbol_t *sym = &expand_symbols[i];
        switch (sym->kind) {
        case SYM_PIN:
        case SYM_PINNODE:
            if ((int)sym->pin >= g_node_limit) {
                expand_report_pin_error(ERR_INVALID_USAGE, sym);
                expand_fatal_error(-1, "");
            }
            bind_pin(sym);
            break;
        case SYM_NODE:
            bind_node(sym);
            break;
        case SYM_HIDDEN_NODE:
            bind_hidden_node(sym);
            break;
        default:
            sym->node_fn = -1;
            sym->out_fn = -1;
            sym->pin_fn = -1;
            break;
        }
        if (sym->pin & 0xf000) {
            sym->pin = 0;
        }
        count_output(sym);
    }
    for (int i = 0; i < expand_symbol_count; i++) {
        symbol_t *sym = &expand_symbols[i];
        if (sym->kind == SYM_PIN || sym->kind == SYM_NODE) {
            /* the other extensions of the same name follow the symbol */
            for (int j = i + 1; j < expand_symbol_alloc && same_name(sym, &expand_symbols[j]); j++) {
                int fn = find_pin_variant(sym->pin, expand_symbols[j].ext_code);
                if (fn > 0) {
                    sym->pin_fn = (int16_t)fn;
                }
            }
        }
        mark_usage(sym);
    }
    if (expand_error_count > 0) {
        expand_fatal_error(-1, "");
    }
    check_flipflop_pairs();
    if (pick_alternative_device(alt_name, alt_size)) {
        return 1;
    }
    check_output_modes();
    add_linked_functions();
    return 0;
}

/* The symbol is a flip-flop input or a plain output */
static int is_logic_output(const symbol_t *sym)
{
    static const char *const exts[] = { "d", "t", "l", "j", "k", "r", "s" };
    int kind = sym->kind & 0x0f;
    if (kind != SYM_PIN && kind != SYM_PINNODE && kind != SYM_NODE && kind != SYM_HIDDEN_NODE) {
        return 0;
    }
    if (sym->ext[0] == '\0') {
        return 1;
    }
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        if (ext_is(sym, exts[i])) {
            return 1;
        }
    }
    return 0;
}

/* How the device treats the polarity of a node function: 0 active low,
 * 1 active high, 2 programmable */
static int polarity_mode(int node_fn)
{
    switch (dev_u8(dev_nodefn(node_fn, NODEFN_POLARITY))) {
    case 1:
    case 5:
        return 0;
    case 3:
    case 6:
    case 7:
        return 2;
    default:
        return 1;
    }
}

/* Note for the later phases which outputs with a fixed
 * polarity get their equation inverted */
void expand_set_polarity_states(void)
{
    for (int i = 0; i < expand_symbol_count; i++) {
        symbol_t *sym = &expand_symbols[i];
        if (!is_logic_output(sym) || sym->node_fn < 0 || is_virtual_device()) {
            continue;
        }
        int mode = polarity_mode(sym->node_fn);
        if (mode == 2) {
            continue;
        }
        int inverted = 0;
        if (sym->out_fn >= 0) {
            int pin_high = (sym->flags & SYMF_PIN_LOW) == 0;
            int eqn_high = (sym->flags & SYMF_EQN_LOW) == 0;
            inverted = ((eqn_high != pin_high) == mode);
        }
        sym->polarity = inverted ? POL_INVERTED : POL_KEEP;
    }
}

/* Bring the equation of the current symbol to the polarity the
 * device needs.  For a programmable polarity expand_invert_output tells whether
 * the output is to be inverted. */
void expand_apply_polarity(void)
{
    symbol_t *sym = expand_cur_sym;
    int programmable = 0;
    if ((sym->kind & 0x0f) == SYM_INTERMEDIATE && (sym->flags & SYMF_EQN_LOW) != 0) {
        toggle_negation(expand_expr);
    }
    if (sym->node_fn < 0 || sym->out_fn < 0) {
        return;
    }
    int mode = polarity_mode(sym->node_fn);
    int active;
    if (mode == 2) {
        programmable = 1;
    }
    if (is_virtual_device()) {
        programmable = 1;
        active = is_logic_output(sym);
        mode = active ? 2 : 1;
    } else {
        active = (int)dev_u8(dev_nodefn(sym->node_fn, NODEFN_ACTIVE));
    }
    int pin_high = (sym->flags & SYMF_PIN_LOW) == 0;
    int eqn_high = (sym->flags & SYMF_EQN_LOW) == 0;
    expand_invert_output = 0;
    if (mode == 2) {
        int plain = 1;
        if (expand_expr != NULL && expand_expr->type == NODE_NOT) {
            plain = 0;
        }
        if (is_virtual_device()) {
            plain = 1;
        }
        /* A DeMorgan twin (-kb / -kd) is always complemented below, so the
         * inversion flag has to say so even when the equation does not
         * start with a NOT. */
        if ((sym->polarity == POL_INVERTED || sym->polarity == POL_FIXED) && programmable &&
            expand_expr != NULL) {
            plain = 0;
        }
        mode = active == 0 ? eqn_high : (eqn_high == pin_high);
        mode = (mode == plain);
        expand_invert_output = mode;
    }
    unsigned ext = dev_u8(dev_nodefn(sym->node_fn, NODEFN_EXT));
    if (ext >= 8 && ext < 12 && (mode ^ pin_high ^ eqn_high) == 0) {
        expand_report_symbol_warning(ERR_DEMORGAN, sym);
    }
    if (sym->polarity != POL_KEEP && programmable) {
        if (sym->polarity == POL_INVERTED || sym->polarity == POL_FIXED) {
            toggle_negation(expand_expr);
        }
    } else if (active == 0) {
        if ((eqn_high ^ mode) != 0) {
            toggle_negation(expand_expr);
            sym->polarity = POL_INVERTED;
        } else {
            sym->polarity = POL_KEEP;
        }
    } else if ((eqn_high ^ pin_high ^ mode) == 0) {
        toggle_negation(expand_expr);
        sym->polarity = POL_INVERTED;
    }
}

/* Reset the device state for a new run of the phase. */
void expand_device_reset(void)
{
    expand_lib_file = NULL;
    memset(g_lib_header, 0, sizeof(g_lib_header));
    memset(expand_lib_entry, 0, sizeof(expand_lib_entry));
    expand_dev = NULL;
    expand_dev_size = 0;
    g_pin_limit = 0;
    g_node_limit = 0;
    g_next_hidden = 0;
    g_next_node = 0;
    g_node_owner = NULL;
    g_node_alias = NULL;
    memset(g_reg_outputs, 0, sizeof(g_reg_outputs));
    for (int i = 0; i < 4; i++) {
        g_bank_ext[i] = -1;
    }
    expand_conflict_map = NULL;
    g_line_owner = NULL;
    g_reg_count = NULL;
    g_pinfn_used = NULL;
    g_nodefn_used = NULL;
    expand_nodefn_default = NULL;
    g_line_used = NULL;
    g_pin_output = NULL;
    expand_pin_foreign = NULL;
    expand_conflict_words = g_line_owner_size = g_reg_count_size = g_pinfn_words = 0;
    expand_nodefn_words = g_line_words = expand_pin_words = 0;
}
