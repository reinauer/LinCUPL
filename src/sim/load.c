/*
 * sim_load - reading the absolute file (.abs) and the device library
 * entry, and the symbol / field tables built from them.
 *
 * Part of the CUPL simulator (see sim.c).
 */
#include "sim.h"
#include "strutil.h"

static FILE *g_abs_file;
static char *g_abs_name;
static size_t g_dev_rec_size;

/* The open device library */
static struct {
    FILE *fp;
    uint8_t *header;
    uint8_t *entry;                 /* directory entry buffer */
    const char *name;               /* device to look up, then the entry found */
} g_lib;

/* -------------------------------------------------------------------------
 * Device library
 * ------------------------------------------------------------------------- */
const uint8_t *dev_section(int n)
{
    static const uint8_t k_empty[64];
    uint32_t off;
    if (g_dev_rec == NULL || n < 0 || n >= DL_SECTIONS) {
        return k_empty;
    }
    off = rd32(g_dev_rec + n * DL_SECTION_HEADER_SIZE + 8);
    return off < g_dev_rec_size ? g_dev_rec + off : k_empty;
}

int dev_section_count(int n)
{
    return g_dev_rec == NULL ? 0 : (int)rd16(g_dev_rec + n * DL_SECTION_HEADER_SIZE + 4);
}

const uint8_t *dev_header(void)
{
    return dev_section(DEV_SEC_HEADER);
}

/* Record `i` of `size` bytes in section n.  Indexes outside the device
 * record yield a record of zeroes. */
static const uint8_t *dev_record(int n, int i, int size)
{
    static const uint8_t k_empty[64];
    const uint8_t *base = dev_section(n);
    if (i < 0 || g_dev_rec == NULL || base < g_dev_rec ||
        (size_t)(base - g_dev_rec) + (size_t)(i + 1) * (size_t)size > g_dev_rec_size) {
        return k_empty;
    }
    return base + (size_t)i * (size_t)size;
}

const uint8_t *dev_pin(int i)
{
    return dev_record(DEV_SEC_PINS, i, DEV_PIN_REC_SIZE);
}

const uint8_t *dev_node(int i)
{
    return dev_record(DEV_SEC_NODES, i, DEV_NODE_REC_SIZE);
}

/* Load the library entry named g_lib.name into g_dev_rec. */
static void load_device(void)
{
    const char *lib_path = g_args[2];
    const char *wanted = g_lib.name;
    uint32_t size;
    size_t got;

    g_lib.fp = open_file(lib_path, "rb");
    if (g_lib.fp == NULL) {
        fatal_error(ERR_OPEN, lib_path);
    }
    g_lib.header = alloc_tracked(DL_HEADER_SIZE, "initialization");
    switch (dl_read_header(g_lib.fp, g_lib.header)) {
    case DL_OK:
        break;
    case DL_BAD_KEY:
        fatal_error(ERR_LIB_KEY, "");
        break;
    default:
        fatal_error(ERR_BAD_LIBRARY, lib_path);
        break;
    }
    g_lib.entry = alloc_tracked(DL_ENTRY_SIZE, "initialization");
    if (!dl_find_device(g_lib.fp, wanted, DL_MATCH_CASE, g_lib.entry)) {
        fatal_error(ERR_NO_DEVICE, wanted);
    }
    g_lib.entry[DL_ENTRY_NAME_SIZE] = '\0';
    g_lib.name = (const char *)g_lib.entry;
    /* read the device record */
    size = dl_image_size(g_lib.entry);
    got = 0;
    if (dl_seek_image(g_lib.fp, g_lib.entry) && size < 0x1000000) {
        g_dev_rec_size = size;
        g_dev_rec = alloc_tracked((size_t)size + 0x200, "device library processing");
        got = dl_read_image(g_lib.fp, g_dev_rec, size);
    }
    if (got == 0 || size < DL_SECTIONS * DL_SECTION_HEADER_SIZE) {
        fatal_error(ERR_BAD_LIBRARY, g_lib.name);
    }
    if (strcmp(g_lib.name, "p7c330") == 0) {
        g_dev_is_7c330 = 1;
    }
    /* Header byte +0x12: 1 = no JEDEC file can be produced for this device. */
    if (dev_header()[0x12] == 1) {
        g_options &= (uint16_t)~OPT_JEDEC;
    }
    g_dev_has_class = dev_header()[0x12] >= 1;
    close_file(g_lib.fp);
    g_lib.fp = NULL;
}

void load_device_entry(const char *name)
{
    g_lib.name = name;
    load_device();
}

/* Does the library have an entry `name`?  (0 also if it cannot be read;
 * load_device_entry() then reports why.) */
int device_in_library(const char *name)
{
    uint8_t header[DL_HEADER_SIZE];
    uint8_t entry[DL_ENTRY_SIZE];
    FILE *fp = open_file(g_args[2], "rb");
    int found;
    if (fp == NULL) {
        return 0;
    }
    found = dl_read_header(fp, header) == DL_OK && dl_find_device(fp, name, DL_MATCH_CASE, entry);
    close_file(fp);
    return found;
}

/* Is `abs_device` listed as an alias in the library entry `device`? */
static int device_has_alias(const char *device, const char *abs_device)
{
    const uint8_t *alias;
    int count;
    g_lib.name = device;
    load_device();
    count = dev_section_count(DEV_SEC_ALIASES);
    alias = dev_section(DEV_SEC_ALIASES);
    for (int i = 0; i < count; i++, alias += 0x16) {
        if (strncmp((const char *)alias, abs_device, 0x16) == 0) {
            return 1;
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Absolute file
 * ------------------------------------------------------------------------- */

void close_abs_file(void)
{
    close_file(g_abs_file);
    g_abs_file = NULL;
}

static void abs_read(void *dst, size_t size, size_t count)
{
    if (size == 0 || count == 0 || fread(dst, size, count, g_abs_file) == 0) {
        fatal_error(ERR_UNEXPECTED_EOF, g_abs_name);
    }
}

/* Open the .abs file and read its header. */
static void read_abs_header(void)
{
    uint8_t header[CF_HEADER_SIZE];
    unsigned extra;
    if (g_vsima) {
        /* vsima: the name is built from the source argument, without a
         * trailing .si (as for the other devices, the test file may be
         * named with or without its extension) */
        char *dir = dup_string(g_args[5], "initialization");
        dir[lincupl_path_dir_len(dir)] = '\0';
        char *source = dup_string(g_args[5], "initialization");
        size_t len = strlen(source);
        if (len >= 3 && strcasecmp(source + len - 3, ".si") == 0) {
            source[len - 3] = '\0';
        }
        g_abs_name = make_file_name(dir, source, ".abs");
    } else {
        g_abs_name = make_file_name(g_dir_prefix, g_base_name, ".abs");
    }
    g_abs_file = open_file(g_abs_name, "rb");
    if (g_abs_file == NULL) {
        fatal_error(ERR_OPEN, g_abs_name);
    }
    if (!cf_read_header(g_abs_file, header)) {
        fatal_error(ERR_UNEXPECTED_EOF, g_abs_name);
    }
    if (rd16(header + CF_HDR_MAGIC) != CF_MAGIC_ABS) {
        fatal_error(ERR_FILE_FORMAT, g_abs_name);
    }
    if (rd16(header + CF_HDR_VERSION) != CF_ABS_VERSION) {
        fatal_error(ERR_OLD_ABS, g_abs_name);
    }
    g_nsyms = (uint16_t)rd16(header + CF_HDR_SYMBOLS);
    memcpy(g_device_name, header + CF_HDR_DEVICE, 0x1c);
    g_device_name[sizeof(g_device_name) - 1] = '\0';
    memcpy(g_abs_jedec_name, header + CF_HDR_DESIGN, 0x1d);
    g_abs_jedec_name[0x1d] = '\0';
    extra = cf_header_extra(header);
    if (extra != 0) {
        g_abs_extra = alloc_tracked(extra + 1, "symbol table");
        abs_read(g_abs_extra, extra, 1);
    }
}

/* Read the symbol table.  csima rejects symbols of kind 2, 5, 9 or 10
 * without a pin number; vsima instead adjusts pins and pin records (_CLOCK
 * is pin 1, _COE pin 2). */
static void read_symbols(void)
{
    cf_symbol_t rec;
    g_nsyms_file = g_nsyms;
    /* Room for the entries vsima appends is not needed: it only renumbers. */
    g_syms = alloc_tracked(((size_t)g_nsyms + 1) * sizeof(sym_t), "symbol table");
    if (g_nsyms == 0) {
        fatal_error(ERR_UNEXPECTED_EOF, g_abs_name);
    }
    for (int i = 0; i < g_nsyms; i++) {
        sym_t *s = &g_syms[i];
        if (!cf_read_symbol(g_abs_file, &rec, CF_SYMBOL_SIZE)) {
            if (i == 0) {
                fatal_error(ERR_UNEXPECTED_EOF, g_abs_name);
            }
            memset(&rec, 0, sizeof(rec));
        }
        memcpy(s->name, rec.name, sizeof(s->name));
        s->name[sizeof(s->name) - 1] = '\0';
        memcpy(s->type, rec.ext, sizeof(s->type));
        s->type[sizeof(s->type) - 1] = '\0';
        s->pin = rec.pin;
        s->kind = rec.type;
        s->flags = rec.flags;
        s->pin_rec = rec.pin_index;
        s->node_rec = rec.node_index;
        s->file_3c = rec.terms;
        s->frame = NULL;
    }
    for (int i = 0; i < g_nsyms; i++) {
        sym_t *s = &g_syms[i];
        if (!g_vsima) {
            if (s->pin == 0 && (s->kind == 2 || s->kind == 5 || s->kind == 9 || s->kind == 10)) {
                fatal_error(ERR_NO_PIN_NUMBER, s->name);
            }
            continue;
        }
        /* The note is printed once, for the first pin without a number,
         * which turns the JEDEC vectors off.  The original printed it for
         * every symbol up to that pin (for all of them when every pin has
         * a number and the vectors are written), with the name glued to
         * the text (cupl_bug.pdf #50a). */
        if ((g_options & OPT_JEDEC) && s->pin == 0 && (s->kind == 2 || s->kind == 5)) {
            g_options &= (uint16_t)~OPT_JEDEC;
            print_warning_now(ERR_NO_PIN_NUMBER, s->name);
        }
        if (strcmp(s->name, "_CLOCK") == 0) {
            s->pin = 1;
        } else if (strcmp(s->name, "_COE") == 0) {
            s->pin = 2;
        }
        if (s->pin == 0 && (s->node_rec == -1 || (s->flags & 0x80))) {
            s->pin_rec = 0;
        }
        if (s->pin_rec >= 0 && s->node_rec == -1) {
            s->flags &= 0xef;
        }
    }
}

/* Read the next frame.  Returns NULL at the end marker. */
static frame_t *read_frame(void)
{
    cf_frame_t hdr;
    frame_t *f = alloc_tracked(sizeof(*f), "initialization");
    int plane_words;
    if (!cf_read_frame(g_abs_file, &hdr)) {
        fatal_error(ERR_UNEXPECTED_EOF, g_abs_name);
    }
    f->flags = (uint8_t)hdr.flags;
    f->b1 = (uint8_t)(hdr.flags >> 8);
    f->index = hdr.or_array;
    f->nsyms = hdr.nvars;
    f->nrows = hdr.nrows;
    f->w8 = hdr.has_xor;
    f->wa = hdr.max_terms;
    if (f->flags & (CF_FRAME_END_OF_FILE | CF_FRAME_END_OF_DESIGN)) {
        return NULL;
    }
    plane_words = cf_plane_words(f->nsyms);
    f->row_words = plane_words * 3;
    f->total_words = f->nrows * f->row_words;
    if (f->nrows == 0) {
        if (!g_vsima) {
            /* csima reads zero bytes and takes that for end of file */
            fatal_error(ERR_UNEXPECTED_EOF, g_abs_name);
        }
        return f;
    }
    f->rows = alloc_tracked((size_t)f->total_words * 2 + 2, "frame allocation");
    f->byte_size = f->total_words * 2 + CF_FRAME_SIZE;
    f->plane0 = f->rows;
    f->plane1 = f->plane0 + plane_words;
    f->plane2 = f->plane1 + plane_words;
    /* a short frame is taken as far as it goes */
    if (cf_read_words(g_abs_file, f->rows, (size_t)f->total_words) == 0) {
        fatal_error(ERR_UNEXPECTED_EOF, g_abs_name);
    }
    return f;
}

/* First symbol >= start whose bit is set in the plane, or -1. */
int first_symbol_in_plane(int start, int nsyms, const uint16_t *plane)
{
    if (plane == NULL) {
        return -1;
    }
    for (int i = start; i < nsyms; i++) {
        if (bit_test(plane + 1, i)) {
            return i;
        }
    }
    return -1;
}

/* Read all frames and build the term table; field frames are attached to
 * their field symbol. */
static void read_frames(void)
{
    frame_t *f;
    if (g_vsima) {
        g_nterms = 1;
        for (int i = 0; i < g_nsyms_file; i++) {
            if (g_syms[i].file_3c != 0) {
                g_nterms++;
            }
        }
    } else {
        g_nterms = (int)rd16(dev_header() + 0x27);
    }
    g_terms = alloc_tracked((size_t)g_nterms * sizeof(term_entry_t), "initialization");
    g_total_rows = 0;
    g_row_words = -1;
    while ((f = read_frame()) != NULL) {
        if (g_row_words < 0) {
            g_row_words = f->row_words;
            g_plane_words = (uint16_t)(g_row_words / 3);
        } else if (g_row_words != f->row_words) {
            fatal_error(0x3fc, VTEXT("", "pt width mismatch"));
        }
        if (!(f->flags & CF_FRAME_FIELD)) {
            if ((int)f->index < g_nterms) {
                g_terms[f->index].frame = f;
                g_terms[f->index].nrows = f->nrows;
                g_terms[f->index].first_row = g_total_rows;
                g_total_rows += f->nrows;
            } else if (g_vsima) {
                char text[40];
                snprintf(text, sizeof(text), "wrong cell number: %d", (int)f->index);
                report_error(0x3eb, text);
            } else {
                report_error(0x3eb, "");
            }
        } else {
            int member = first_symbol_in_plane(0, f->nsyms, f->rows);
            if (member < 0 || member >= g_nsyms) {
                report_error(0x3ec, "unreferenced field");
            } else {
                g_syms[member].frame = f;
            }
        }
    }
    if (g_vsima) {
        /* Symbols that are neither pins nor nodes get pin record numbers
         * following the real ones. */
        g_next_pin_rec = g_nsyms_file;
        for (int i = 0; i < g_nsyms; i++) {
            if (!(g_syms[i].flags & 0xc0)) {
                g_syms[i].flags |= 0x80;
                g_syms[i].pin_rec = (int16_t)g_next_pin_rec++;
            }
        }
    }
}

/* Read the .abs file, check its device against the phase argument (csima
 * also accepts an alias of the library entry) and load the device. */
void load_design(void)
{
    read_abs_header();
    read_symbols();
    g_lib.name = g_device_name;
    if (!g_vsima) {
        if (strncmp(g_device_name, "p1016", 5) == 0 || strncmp(g_device_name, "p1020", 5) == 0) {
            g_dev_is_isp1016 = 1;
        } else if (strcmp(g_device_name, "p7c331") == 0 || strcmp(g_device_name, "p7c330") == 0 ||
                   strncmp(g_device_name, "mach", 4) == 0) {
            g_dev_is_7c33x = 1;
        } else if (strcmp(g_device_name, "xl78c800") == 0) {
            g_dev_is_78c800 = 1;
        }
    }
    if (g_args[4][0] != '&' && strcmp(g_args[4], g_device_name) != 0) {
        if (g_vsima || !device_has_alias(g_args[4], g_device_name)) {
            fatal_error(ERR_EXPECTING_DEVICE, g_device_name);
        }
        g_lib.name = g_device_name;
    }
    load_device();
    read_frames();
}

/* -------------------------------------------------------------------------
 * Symbols and fields
 * ------------------------------------------------------------------------- */

field_t *find_field(const char *name)
{
    for (field_t *f = g_fields; f != NULL; f = f->next) {
        if (strcmp(f->name, name) == 0) {
            return f;
        }
    }
    return NULL;
}

/* Append a new field to g_fields; a name already defined is an error and
 * returns the existing field. */
field_t *define_field(const char *name)
{
    field_t *f = find_field(name);
    if (f != NULL) {
        report_error(ERR_FIELD_DEFINED, name);
        return f;
    }
    f = alloc_tracked(sizeof(*f), "symbol table");
    if (g_fields == NULL) {
        g_fields = f;
    } else {
        g_fields_tail->next = f;
    }
    g_fields_tail = f;
    f->name = name;
    f->members = NULL;
    return f;
}

/* The members of a field, from the rows of its frame.  Each row names the
 * member symbols in plane 1; a bit in plane 2 sets flag bit 0 of the
 * member's node. */
static list_t *field_members(const sym_t *field_sym)
{
    const frame_t *f = field_sym->frame;
    list_t *list;
    if (f == NULL) {
        fatal_error(0x40c, "");
    }
    list = list_new();
    for (int row = 0; row < f->nrows; row++) {
        const uint16_t *base = f->rows + (size_t)row * (size_t)g_row_words;
        const uint16_t *members = base + g_plane_words;
        const uint16_t *marks = members + g_plane_words;
        for (int i = 0; i < g_nsyms; i++) {
            if (bit_test(members + 1, i)) {
                list_node_t *node = node_new();
                node->u.sym = &g_syms[i];
                if (bit_test(marks + 1, i)) {
                    node->flags |= 1;
                }
                node->kind = 0xd;
                list_append(node, list);
            }
        }
    }
    return list;
}

/* Define a field for every field symbol (kind 1) of the .abs file. */
void build_fields(void)
{
    for (int i = 0; i < g_nsyms; i++) {
        if (g_syms[i].kind == 1) {
            field_t *f = define_field(g_syms[i].name);
            f->members = field_members(&g_syms[i]);
        }
    }
}
