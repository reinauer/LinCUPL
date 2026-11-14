/*
 * parse - strings, symbol tables and the device library.
 *
 * Names are interned, so that they can be compared by pointer.  Each
 * design (NAME header item) has a symbol table of its own; the variables
 * of one name form a tree ordered by extension.  Of the device library
 * only the directory entry of the device and a few fields of its
 * description are read.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parse_int.h"
#include "devlib.h"

static symbol_t *g_free_symbols;
static char *g_string_space;
static int g_string_used;
/* strings */
static list_t g_strings[HASH_SIZE];    /* interned strings by hash */
char g_parse_empty_ext[1];             /* extension of a symbol without one */

/* symbol tables */
symtab_t *g_parse_first_table;
static symtab_t *g_last_table;
static int g_table_count;
int g_parse_total_syms;
symtab_t *g_parse_table;               /* table being filled */
const char *g_parse_device_name;
static int g_device_loaded;            /* g_parse_device holds the named device */
device_t *g_parse_device;
static char g_virtual_one[2];          /* "1" */

void parse_symtab_reset(void)
{
    g_free_symbols = NULL;
    g_string_space = NULL;
    g_string_used = 0;
    memset(g_strings, 0, sizeof(g_strings));
    g_parse_empty_ext[0] = '\0';
    g_parse_first_table = NULL;
    g_last_table = NULL;
    g_table_count = 0;
    g_parse_total_syms = 0;
    g_parse_table = NULL;
    g_parse_device_name = NULL;
    g_device_loaded = 0;
    g_parse_device = NULL;
    strcpy(g_virtual_one, "1");
}

/* -------------------------------------------------------------------------
 * Strings
 * ------------------------------------------------------------------------- */

/* sum of the (signed) characters modulo HASH_SIZE */
static int hash_name(const char *name)
{
    int sum = 0;
    if (name != NULL) {
        for (; *name != '\0'; name++) {
            sum += (signed char)*name;
        }
    }
    sum %= HASH_SIZE;
    return sum < 0 ? sum + HASH_SIZE : sum;
}

/* `size` bytes for an interned string; a new block of STRING_CHUNK bytes
 * is started when the current one is full */
static char *string_space(int size)
{
    g_string_used += size;
    if (g_string_space == NULL || g_string_used > STRING_CHUNK) {
        g_string_space = parse_alloc_tracked(STRING_CHUNK, "string space");
        g_string_used = size;
    }
    g_string_space += size;
    return g_string_space - size;
}

/* the unique stored copy of a string; NULL for an empty string.
 * Strings are compared by pointer everywhere else. */
const char *parse_intern(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return NULL;
    }
    size_t size = strlen(text) + 1;
    if (size > MAX_INTERN_LEN) {
        parse_fatal_error(ERR_LINE_TOO_LONG, "");
    }
    list_t *bucket = &g_strings[hash_name(text)];
    for (token_t *entry = bucket->head; entry != NULL; entry = entry->next) {
        if (strcmp(text, entry->u.text) == 0) {
            return entry->u.text;
        }
    }
    token_t *entry = parse_token_alloc();
    char *copy = string_space((int)size);
    entry->u.text = copy;
    entry->type = (uint8_t)size;
    strcpy(copy, text);
    parse_list_append(entry, bucket);
    return copy;
}

void parse_copy_lower(char *dst, size_t dst_size, const char *src)
{
    size_t n = 0;
    for (; src != NULL && src[n] != '\0' && n + 1 < dst_size; n++) {
        dst[n] = (char)to_lower((unsigned char)src[n]);
    }
    dst[n] = '\0';
}

/* the number a name ends in, -1 if it does not end in a digit.
 * A "$demorg" suffix is ignored. */
int parse_name_index(const char *name)
{
    char buf[260];
    snprintf(buf, sizeof(buf), "%s", name != NULL ? name : "");
    char *cut = strstr(buf, "$demorg");
    if (cut != NULL) {
        *cut = '\0';
    }
    char *p = buf + strlen(buf);
    while (p > buf && is_digit((unsigned char)p[-1])) {
        p--;
    }
    if (!is_digit((unsigned char)*p)) {
        return -1;
    }
    return (int)strtol(p, NULL, 10);
}

/* the name without the number it ends in */
void parse_name_prefix(char *out, size_t out_size, const char *name)
{
    char buf[260];
    snprintf(buf, sizeof(buf), "%s", name != NULL ? name : "");
    char *cut = strstr(buf, "$demorg");
    if (cut != NULL) {
        *cut = '\0';
    }
    size_t n = strlen(buf);
    while (n > 0 && is_digit((unsigned char)buf[n - 1])) {
        n--;
    }
    buf[n] = '\0';
    snprintf(out, out_size, "%s", buf);
}

/* compare two names; names with the same prefix are ordered by
 * their number, then by length ("a1" < "a01" < "a2"). */
int parse_compare_names(const char *a, const char *b)
{
    if (a == NULL) {
        a = "";
    }
    if (b == NULL) {
        b = "";
    }
    int index_a = parse_name_index(a);
    int index_b = parse_name_index(b);
    if (index_a < 0 || index_b < 0) {
        return strcmp(a, b);
    }
    char prefix_a[260];
    char prefix_b[260];
    parse_name_prefix(prefix_a, sizeof(prefix_a), a);
    parse_name_prefix(prefix_b, sizeof(prefix_b), b);
    int order = strcmp(prefix_a, prefix_b);
    if (order != 0) {
        return order;
    }
    if (index_a != index_b) {
        return index_a < index_b ? -1 : 1;
    }
    size_t len_a = strlen(a);
    size_t len_b = strlen(b);
    if (len_a != len_b) {
        return len_a < len_b ? -1 : 1;
    }
    return 0;
}

/* cut a name longer than `limit` to limit - 1 characters */
void parse_truncate_name(char *name, size_t limit)
{
    if (strlen(name) > limit) {
        name[limit - 1] = '\0';
    }
}

/* prefix + number, at most 32 characters */
void parse_make_indexed_name(char *out, const char *prefix, unsigned number)
{
    snprintf(out, NAME_BUF_SIZE, "%s%d", or_null(prefix), (int)number);
    parse_truncate_name(out, 0x21);
}

/* =========================================================================
 * Symbol tables
 * ========================================================================= */

/* a symbol from the pool; free symbols are chained through `left` */
static symbol_t *symbol_alloc(void)
{
    if (g_free_symbols == NULL) {
        symbol_t *chunk = parse_alloc_tracked(SYMBOL_CHUNK * sizeof(*chunk), "symbol table");
        for (int i = 0; i < SYMBOL_CHUNK - 1; i++) {
            chunk[i].left = &chunk[i + 1];
        }
        g_free_symbols = chunk;
    }
    symbol_t *sym = g_free_symbols;
    g_free_symbols = sym->left;
    sym->expr = NULL;
    sym->left = NULL;
    sym->right = NULL;
    sym->next = NULL;
    return sym;
}

/* look a variable up by name and extension */
static symbol_t *find_symbol(const char *name, const char *ext, const symtab_t *table)
{
    char lower[260];
    if (ext == NULL) {
        ext = g_parse_empty_ext;
    }
    symbol_t *sym = table->hash[hash_name(name)];
    while (sym != NULL && parse_compare_names(name, sym->name) != 0) {
        sym = sym->next;
    }
    parse_copy_lower(lower, sizeof(lower), ext);
    while (sym != NULL) {
        int order = strcmp(lower, sym->ext);
        if (order == 0) {
            break;
        }
        sym = (order < 0) ? sym->left : sym->right;
    }
    return sym;
}

symbol_t *parse_find_symbol_of_type(const char *name, const char *ext, int type, const symtab_t *table)
{
    symbol_t *sym = find_symbol(name, ext, table);
    return (sym != NULL && sym->type == type) ? sym : NULL;
}

/* name.ext takes pin number, kind and polarity from the first
 * variable entered under that name */
static void inherit_from_base(symbol_t *sym, const symbol_t *base)
{
    sym->pin = base->pin;
    if (base->type == SYM_NODE || base->type == SYM_NODE_EXT) {
        sym->type = SYM_NODE_EXT;
    } else if (base->type == SYM_PIN || base->type == SYM_PIN_EXT) {
        sym->type = SYM_PIN_EXT;
    }
    if (base->flags & SF_PIN_NEGATED) {
        sym->flags |= SF_PIN_NEGATED;
    }
}

/* find a variable or enter it.  With `may_exist` clear, finding
 * it is an error ("variable already declared").  Variables of one name form
 * a binary tree ordered by extension, hanging off the hash chain entry. */
symbol_t *parse_enter_symbol(const char *name, const char *ext, int may_exist, symtab_t *table)
{
    char lower[260];
    if (ext == NULL) {
        ext = g_parse_empty_ext;
    }
    int bucket = hash_name(name);
    symbol_t *root = table->hash[bucket];
    while (root != NULL && parse_compare_names(name, root->name) != 0) {
        root = root->next;
    }
    symbol_t *sym;
    if (root != NULL) {
        symbol_t *at = root;
        symbol_t *parent = root;
        int order = 0;
        parse_copy_lower(lower, sizeof(lower), ext);
        while (at != NULL) {
            order = strcmp(lower, at->ext);
            parent = at;
            if (order == 0) {
                if (!may_exist) {
                    parse_error_symbol(ERR_ALREADY_DECLARED, at);
                }
                return at;
            }
            at = (order < 0) ? at->left : at->right;
        }
        sym = symbol_alloc();
        if (order < 0) {
            parent->left = sym;
        } else {
            parent->right = sym;
        }
    } else {
        sym = symbol_alloc();
        sym->next = table->hash[bucket];
        table->hash[bucket] = sym;
    }
    table->nsyms++;
    g_parse_total_syms++;
    sym->name = name;
    /* the stored extension text itself is converted to lower case */
    for (char *p = (char *)(uintptr_t)ext; *p != '\0'; p++) {
        if (is_upper((unsigned char)*p)) {
            *p = (char)to_lower((unsigned char)*p);
        }
    }
    sym->ext = ext;
    sym->type = SYM_VARIABLE;
    if (root != NULL) {
        inherit_from_base(sym, root);
    }
    return sym;
}

/* a new, empty symbol table at the end of the list of tables */
symtab_t *parse_new_table(const char *name)
{
    symtab_t *table = parse_alloc_tracked(sizeof(*table), "symbol table");
    table->index = (int16_t)g_table_count++;
    table->name = name;
    if (g_last_table == NULL) {
        g_parse_first_table = table;
    } else {
        g_last_table->next = table;
    }
    g_last_table = table;
    return table;
}

/* the table whose NAME is `name` (interned, compared by pointer) */
symtab_t *parse_find_table(const char *name)
{
    for (symtab_t *table = g_parse_first_table; table != NULL; table = table->next) {
        if (table->name == name) {
            return table;
        }
    }
    return NULL;
}

/* =========================================================================
 * Device library
 *
 * Only what this phase needs is read: the directory entry of the device and
 * a few fields of its description.
 * ========================================================================= */
/* bounds-checked access to the device description */
static uint32_t dev_byte(const device_t *dev, uint32_t offset)
{
    return (dev->data != NULL && offset < dev->data_size) ? dev->data[offset] : 0;
}

static uint32_t dev_word(const device_t *dev, uint32_t offset)
{
    return dev_byte(dev, offset) | dev_byte(dev, offset + 1) << 8;
}

static uint32_t dev_long(const device_t *dev, uint32_t offset)
{
    return dev_word(dev, offset) | dev_word(dev, offset + 2) << 16;
}

/* fields of the architecture block, whose offset is at 0x44 */
uint32_t parse_dev_arch_byte(const device_t *dev, uint32_t field)
{
    return dev_byte(dev, dev_long(dev, 0x44) + field);
}

const char *parse_virtual_flag(void)
{
    return g_parse_virtual_device != NULL ? g_parse_virtual_device : "";
}

/* name of the extension with the given code; "d" on a virtual
 * device if the device does not have it */
const char *parse_dev_ext_name(int code, const device_t *dev)
{
    if (code == 0) {
        return NULL;
    }
    uint32_t table = dev_long(dev, 0x58);
    uint32_t count = dev_word(dev, 0x54);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t entry = table + i * 0x15;
        if ((uint32_t)code == dev_byte(dev, entry + 0x14)) {
            return entry + 0x14 <= dev->data_size ? (const char *)dev->data + entry : NULL;
        }
    }
    return parse_virtual_flag()[0] == '1' ? "d" : NULL;
}

/* code of an extension name on this device: 0 for none, -1 if the
 * device does not have it */
int parse_dev_ext_code(const char *ext, const device_t *dev)
{
    if (ext == NULL || ext[0] == '\0' || ext[0] == ' ' || parse_dev_arch_byte(dev, ARCH_CLASS) == 9) {
        return 0;
    }
    char lower[32];
    parse_copy_lower(lower, sizeof(lower), ext);
    uint32_t table = dev_long(dev, 0x58);
    uint32_t count = dev_word(dev, 0x54);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t entry = table + i * 0x15;
        if (entry + 0x15 > dev->data_size) {
            break;
        }
        if (strncmp(lower, (const char *)dev->data + entry, 0x14) == 0) {
            return (int)dev_byte(dev, entry + 0x14);
        }
    }
    return -1;
}

/* index of the pin/extension combination in the device's pin
 * table, -1 if there is none */
int parse_dev_pin_index(int pin, int ext_code, const device_t *dev)
{
    if (pin == 0 || parse_dev_arch_byte(dev, ARCH_CLASS) == 9) {
        return 0;
    }
    uint32_t table = dev_long(dev, 0x94);
    uint32_t count = dev_word(dev, 0x90);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t entry = table + i * 0x27;
        if ((uint32_t)pin == dev_word(dev, entry + 2) && (uint8_t)ext_code == dev_byte(dev, entry + 4)) {
            return (int)i;
        }
    }
    return parse_virtual_flag()[0] == '1' ? 0 : -1;
}

uint32_t parse_dev_pin_kind(const device_t *dev, int index)
{
    return dev_byte(dev, dev_long(dev, 0x94) + (uint32_t)index * 0x27 + 5);
}

/* read the directory entry and the description of a device from the
 * library */
static void load_device(device_t *dev, const char *name)
{
    g_parse_device = dev;
    dev->fp = parse_open_file(g_parse_library_path, "rb");
    if (dev->fp == NULL) {
        parse_fatal_error(ERR_OPEN, g_parse_library_path);
    }
    dev->header = parse_alloc_tracked(DL_HEADER_SIZE, "initialization");
    switch (dl_read_header(dev->fp, dev->header)) {
    case DL_OK:
        break;
    case DL_BAD_KEY:
        parse_fatal_error(ERR_LIB_KEY, "");
        break;
    default:
        parse_fatal_error(ERR_BAD_LIBRARY, g_parse_library_path);
        break;
    }
    if (strcmp("unknown", name) == 0) {
        parse_fatal_error(ERR_NO_TARGET, "");
    }

    dev->entry = parse_alloc_tracked(DL_ENTRY_SIZE + 1, "initialization");
    if (!dl_find_device(dev->fp, name, DL_MATCH_CASE, dev->entry)) {
        parse_fatal_error(ERR_NO_DEVICE, name);
    }
    dev->have_entry = 1;

    int loaded = 0;
    if (dl_seek_image(dev->fp, dev->entry)) {
        dev->data_size = dl_image_size(dev->entry);
        dev->data = parse_alloc_tracked(dev->data_size, "device library processing");
        loaded = dl_first_block_read(dl_read_image(dev->fp, dev->data, dev->data_size), dev->data_size);
    }
    if (!loaded) {
        parse_fatal_error(ERR_BAD_LIBRARY, name);
    }
    uint32_t class = parse_dev_arch_byte(dev, ARCH_CLASS);
    if (class == 9 || class == 8) {
        g_parse_virtual_device = g_virtual_one;
    }
    parse_close_file(dev->fp);
}

const char *parse_device_entry_name(const device_t *dev)
{
    return (dev != NULL && dev->have_entry) ? (const char *)dev->entry : NULL;
}

/* make sure the device of `table` is loaded */
void parse_require_device(symtab_t *table)
{
    if (g_device_loaded) {
        return;
    }
    symtab_t *other = g_parse_first_table;
    for (; other != NULL; other = other->next) {
        const char *name = parse_device_entry_name(other->device);
        if (name != NULL && parse_compare_names(g_parse_device_name, name) == 0) {
            break;
        }
    }
    if (other == NULL) {
        table->device = parse_alloc_tracked(sizeof(device_t), "initialization");
        load_device(table->device, g_parse_device_name);
    } else {
        table->device = other->device;
    }
    g_device_loaded = 1;
}

/* the device named on the command line, if any */
void parse_select_device_from_args(void)
{
    g_parse_device_name = (g_parse_device_arg[0] == '&') ? "unknown" : g_parse_device_arg;
    g_device_loaded = 0;
}

