/*
 * absdump.c - the absolute file as readable text, see absdump.h.
 *
 * The meaning of the fields comes from the writer (cupl's fusemap phase,
 * src/cupl/abs.c, with the symbol fields set by parse and expand) and the
 * reader (csim, src/sim/load.c and the evaluation in src/sim/core.c and
 * bind.c).  The file is read with the cfile.h readers csim uses.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "absdump.h"
#include "cfile.h"
#include "devlib.h"
#include "locate.h"

/* -------------------------------------------------------------------------
 * Growing text buffer
 * ------------------------------------------------------------------------- */
#if defined(__GNUC__) || defined(__clang__)
#define ABS_PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#define ABS_PRINTF(a, b)
#endif

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    int oom;
} text_t;

static void put(text_t *t, const char *fmt, ...) ABS_PRINTF(2, 3);

static void put(text_t *t, const char *fmt, ...)
{
    va_list ap;
    char small[256];
    int n;
    if (t->oom) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(small, sizeof(small), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if (t->len + (size_t)n + 1 > t->cap) {
        size_t cap = t->cap != 0 ? t->cap : 4096;
        char *p;
        while (t->len + (size_t)n + 1 > cap) {
            cap *= 2;
        }
        p = realloc(t->buf, cap);
        if (p == NULL) {
            t->oom = 1;
            return;
        }
        t->buf = p;
        t->cap = cap;
    }
    if ((size_t)n < sizeof(small)) {
        memcpy(t->buf + t->len, small, (size_t)n + 1);
    } else {
        va_start(ap, fmt);
        vsnprintf(t->buf + t->len, (size_t)n + 1, fmt, ap);
        va_end(ap);
    }
    t->len += (size_t)n;
}

static void put_spaces(text_t *t, int n)
{
    while (n-- > 0) {
        put(t, " ");
    }
}

/* -------------------------------------------------------------------------
 * State of one decoding
 * ------------------------------------------------------------------------- */
#define HEX_LIMIT 512               /* bytes of unknown data shown at most */

typedef struct {
    FILE *fp;
    long base;                      /* stream position of the file start */
    long size;                      /* bytes from base to the end */
    text_t out;
    int errors;
    /* symbol table of the current section */
    cf_symbol_t *syms;
    int nsyms;                      /* records read */
    int declared_nsyms;             /* count in the header */
    char (*labels)[CF_NAME_SIZE + CF_EXT_SIZE + 4];
    int row_words;                  /* words per row of the first record, -1 */
    int seen_terms;                 /* "Product terms" heading written */
    int seen_fields;                /* "Field lists" heading written */
    /* device library entry */
    const char *lib_path;
    uint8_t *image;
    size_t image_size;
    int vsima;                      /* csim simulates it as vsima */
} dump_t;

static long position(dump_t *d)
{
    long pos = ftell(d->fp);
    return pos < 0 ? d->size : pos - d->base;
}

static long remaining(dump_t *d)
{
    long left = d->size - position(d);
    return left < 0 ? 0 : left;
}

static void error_line(dump_t *d, const char *fmt, ...) ABS_PRINTF(2, 3);

static void error_line(dump_t *d, const char *fmt, ...)
{
    char text[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    put(&d->out, "*** error: %s\n", text);
    d->errors++;
}

/* Hex lines of `n` bytes that start at file offset `offset` */
static void put_hex(dump_t *d, const char *indent, long offset, const uint8_t *bytes, size_t n)
{
    size_t shown = n < HEX_LIMIT ? n : HEX_LIMIT;
    for (size_t off = 0; off < shown; off += 16) {
        put(&d->out, "%s%08lx ", indent, (unsigned long)(offset + (long)off));
        for (size_t i = 0; i < 16; i++) {
            if (off + i < shown) {
                put(&d->out, " %02x", bytes[off + i]);
            } else {
                put(&d->out, "   ");
            }
        }
        put(&d->out, "  |");
        for (size_t i = 0; i < 16 && off + i < shown; i++) {
            unsigned c = bytes[off + i];
            put(&d->out, "%c", c >= 0x20 && c < 0x7f ? (int)c : '.');
        }
        put(&d->out, "|\n");
    }
    if (n > shown) {
        put(&d->out, "%s... %lu more bytes\n", indent, (unsigned long)(n - shown));
    }
}

/* Read up to `n` bytes and show them as hex (the rest of a short item) */
static void put_rest_hex(dump_t *d, const char *indent, size_t n)
{
    long offset = position(d);
    uint8_t *bytes;
    size_t got;
    if (n == 0) {
        return;
    }
    bytes = malloc(n);
    if (bytes == NULL) {
        return;
    }
    got = fread(bytes, 1, n, d->fp);
    put_hex(d, indent, offset, bytes, got);
    free(bytes);
}

/* Text of a fixed-size name field: up to its NUL, unprintable bytes as '?' */
static void field_text(char *dst, const char *src, size_t size)
{
    size_t i;
    for (i = 0; i < size && src[i] != '\0'; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
    }
    dst[i] = '\0';
}

static int bit(const uint16_t *plane, int i)
{
    return (plane[1 + (i >> 4)] >> (i & 15)) & 1;
}

/* Name of symbol i as used in the terms: name or name.ext; the constants
 * (symbols 0 and 1, kinds 6 and 7) as <0> and <1> */
static const char *label(dump_t *d, int i)
{
    static char other[24];
    if (i >= 0 && i < d->nsyms) {
        return d->labels[i];
    }
    snprintf(other, sizeof(other), "#%d", i);
    return other;
}

/* A variable of a product term: name, !name, or a constant */
static void put_variable(text_t *t, dump_t *d, int i, int positive)
{
    unsigned kind = (i >= 0 && i < d->nsyms) ? (d->syms[i].type & 0x0f) : 0;
    if (kind == 6) {
        put(t, positive ? "0" : "1");
    } else if (kind == 7) {
        put(t, positive ? "1" : "0");
    } else {
        put(t, "%s%s", positive ? "" : "!", label(d, i));
    }
}

/* -------------------------------------------------------------------------
 * Device library
 * ------------------------------------------------------------------------- */

/* Start of section n of the device image and its record count; NULL if
 * the image does not have it */
static const uint8_t *dev_section(dump_t *d, int n, int *count)
{
    uint32_t off;
    if (d->image == NULL || (size_t)(n + 1) * DL_SECTION_HEADER_SIZE > d->image_size) {
        return NULL;
    }
    off = rd32(d->image + n * DL_SECTION_HEADER_SIZE + 8);
    if (count != NULL) {
        *count = (int)rd16(d->image + n * DL_SECTION_HEADER_SIZE + 4);
    }
    return off < d->image_size ? d->image + off : NULL;
}

/* Record i of `size` bytes in section n, NULL if outside the image */
static const uint8_t *dev_record(dump_t *d, int n, int i, int size)
{
    const uint8_t *base = dev_section(d, n, NULL);
    if (base == NULL || i < 0 ||
        (size_t)(base - d->image) + (size_t)(i + 1) * (size_t)size > d->image_size) {
        return NULL;
    }
    return base + (size_t)i * (size_t)size;
}

#define DEV_SEC_HEADER 3            /* device information, one record */
#define DEV_SEC_NODES 7             /* node records */
#define DEV_NODE_REC_SIZE 39
#define DEV_HDR_CLASS 0x11          /* 9: simulated by vsima */
#define DEV_HDR_OR_ARRAYS 0x27      /* u16: entries of csim's term table */
#define DEV_NODE_TERM 0             /* u16: OR array the node takes */
#define DEV_NODE_CELL 4             /* cell type (macrocell model) */
#define DEV_NODE_KIND 5             /* output polarity, see node_true_out */

/* csim's cell.true_out for node records of kinds 0..7: 0 if the output
 * complements the OR array (bind.c, node_kind_default) */
static int node_true_out(unsigned kind)
{
    static const int8_t k_true_out[8] = { 0, 0, 1, 1, 1, 0, 0, 1 };
    return kind < 8 ? k_true_out[kind] : -1;
}

static const uint8_t *dev_header(dump_t *d)
{
    static const uint8_t k_zero[0x98];
    const uint8_t *h = dev_record(d, DEV_SEC_HEADER, 0, (int)sizeof(k_zero));
    return h != NULL ? h : k_zero;
}

/* Load the image of the device named in the header; the reason it is not
 * used goes into the text */
static void load_device(dump_t *d, const uint8_t *hdr)
{
    char name[DL_ENTRY_NAME_SIZE + 1];
    uint8_t lib_header[DL_HEADER_SIZE];
    uint8_t entry[DL_ENTRY_SIZE];
    FILE *fp;
    uint32_t size;
    const uint8_t *abs_entry = hdr + CF_HDR_DEVICE;
    free(d->image);
    d->image = NULL;
    d->image_size = 0;
    d->vsima = 0;
    if (d->lib_path == NULL || d->lib_path[0] == '\0') {
        put(&d->out, "  library        none given: the output polarity of the OR arrays is not shown\n");
        return;
    }
    field_text(name, (const char *)abs_entry, DL_ENTRY_NAME_SIZE);
    fp = lincupl_fopen_ci(d->lib_path, "rb");
    if (fp == NULL) {
        put(&d->out, "  library        %s cannot be opened: the output polarity is not shown\n", d->lib_path);
        return;
    }
    if (dl_read_header(fp, lib_header) != DL_OK) {
        put(&d->out, "  library        %s is not a device library: the output polarity is not shown\n",
            d->lib_path);
        fclose(fp);
        return;
    }
    if (!dl_find_device(fp, name, DL_MATCH_CASE, entry)) {
        put(&d->out, "  library        %s has no device %s: the output polarity is not shown\n", d->lib_path, name);
        fclose(fp);
        return;
    }
    size = dl_image_size(entry);
    if (size >= 0x1000000 || size < DL_SECTIONS * DL_SECTION_HEADER_SIZE || !dl_seek_image(fp, entry) ||
        (d->image = malloc(size)) == NULL) {
        put(&d->out, "  library        %s: the entry of %s cannot be read\n", d->lib_path, name);
        fclose(fp);
        return;
    }
    d->image_size = dl_read_device_image(fp, entry, d->image, size);
    fclose(fp);
    if (d->image_size < DL_SECTIONS * DL_SECTION_HEADER_SIZE) {
        put(&d->out, "  library        %s: the entry of %s is cut short\n", d->lib_path, name);
        free(d->image);
        d->image = NULL;
        d->image_size = 0;
        return;
    }
    d->vsima = dev_header(d)[DEV_HDR_CLASS] == 9;
    put(&d->out, "  library        %s, entry %s (%u bytes), simulated by %s\n", d->lib_path, name,
        (unsigned)size, d->vsima ? "vsima" : "csima");
    if (size != dl_image_size(abs_entry) || dl_image_offset(entry) != dl_image_offset(abs_entry)) {
        put(&d->out, "                 the file was compiled with another entry (%u bytes at 0x%x),\n"
                     "                 so the output polarity shown may not be the one compiled for\n",
            (unsigned)dl_image_size(abs_entry), (unsigned)dl_image_offset(abs_entry));
    }
}

/* -------------------------------------------------------------------------
 * Header
 * ------------------------------------------------------------------------- */

/* Non-zero bytes of a header range that is not otherwise decoded */
static void put_unknown_header(dump_t *d, const uint8_t *hdr, int from, int to)
{
    int any = 0;
    for (int i = from; i < to; i++) {
        any |= hdr[i];
    }
    if (any) {
        put(&d->out, "  unidentified   bytes 0x%02x..0x%02x:", from, to - 1);
        for (int i = from; i < to; i++) {
            put(&d->out, " %02x", hdr[i]);
        }
        put(&d->out, "\n");
    }
}

/* Bytes behind the NUL of a text field, if any is not zero */
static void put_text_tail(dump_t *d, const uint8_t *hdr, int from, int to)
{
    int end = from;
    while (end < to && hdr[end] != 0) {
        end++;
    }
    put_unknown_header(d, hdr, end, to);
}

/* The header strings, in the order csim compares them with the header of
 * the simulation input */
static void put_header_strings(dump_t *d, const char *extra, size_t size)
{
    static const char *const k_items[8] = {
        "name", "partno", "revision", "date", "designer", "company", "assembly", "location"
    };
    size_t pos = 0;
    int n = 0;
    while (pos < size) {
        const char *s = extra + pos;
        const char *nul = memchr(s, '\0', size - pos);
        size_t len = nul != NULL ? (size_t)(nul - s) : size - pos;
        char text[80];
        size_t shown = len < sizeof(text) - 1 ? len : sizeof(text) - 1;
        for (size_t i = 0; i < shown; i++) {
            unsigned char c = (unsigned char)s[i];
            text[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
        }
        text[shown] = '\0';
        if (n < 8) {
            put(&d->out, "    %-12s %s%s\n", k_items[n], text, shown < len ? "..." : "");
        } else {
            put(&d->out, "    string %-5d %s%s\n", n + 1, text, shown < len ? "..." : "");
        }
        if (pos + len >= size) {
            put(&d->out, "                 (the last string has no terminating NUL)\n");
        }
        pos += len + 1;
        n++;
    }
    for (; n < 8; n++) {
        put(&d->out, "    %-12s (missing)\n", k_items[n]);
    }
}

/* The header and the symbol records.  Returns 1 if all were read, 2 if the
 * file ends inside the symbol table, 0 if nothing more can be decoded */
static int dump_header(dump_t *d)
{
    uint8_t hdr[CF_HEADER_SIZE];
    char text[CF_HEADER_SIZE];
    long offset = position(d);
    unsigned magic, version, extra;
    if (remaining(d) < CF_HEADER_SIZE) {
        error_line(d, "the file ends inside the header (%ld of %d bytes at offset 0x%lx)", remaining(d),
                   CF_HEADER_SIZE, offset);
        put_rest_hex(d, "  ", (size_t)remaining(d));
        return 0;
    }
    if (!cf_read_header(d->fp, hdr)) {
        error_line(d, "the header cannot be read");
        return 0;
    }
    magic = rd16(hdr + CF_HDR_MAGIC);
    version = rd16(hdr + CF_HDR_VERSION);
    put(&d->out, "Header (offset 0x%lx)\n", offset);
    if (magic != CF_MAGIC_ABS) {
        const char *what = magic == CF_MAGIC_CB ? " (the parser's intermediate file)"
                           : magic == CF_MAGIC_CM ? " (an intermediate file of expand or minimize)" : "";
        error_line(d, "magic number 0x%04x%s, an absolute file has 0x%04x", magic, what, CF_MAGIC_ABS);
        put_hex(d, "  ", offset, hdr, sizeof(hdr));
        return 0;
    }
    put(&d->out, "  magic          0x%04x (absolute file), version %u\n", magic, version);
    if (version != CF_ABS_VERSION) {
        error_line(d, "version %u: csim reads version %d only", version, CF_ABS_VERSION);
    }
    put_unknown_header(d, hdr, 0x06, 0x08);
    field_text(text, (const char *)hdr + CF_HDR_DESIGN, CF_HDR_DEVICE - CF_HDR_DESIGN);
    put(&d->out, "  design         %s\n", text);
    put_text_tail(d, hdr, CF_HDR_DESIGN, CF_HDR_DEVICE);
    field_text(text, (const char *)hdr + CF_HDR_DEVICE, DL_ENTRY_NAME_SIZE);
    put(&d->out, "  device         %s (library entry: flags 0x%02x, %u bytes at offset 0x%x)\n", text,
        hdr[CF_HDR_DEVICE + DL_ENTRY_FLAGS], (unsigned)dl_image_size(hdr + CF_HDR_DEVICE),
        (unsigned)dl_image_offset(hdr + CF_HDR_DEVICE));
    put_text_tail(d, hdr, CF_HDR_DEVICE, CF_HDR_DEVICE + DL_ENTRY_FLAGS);
    put_unknown_header(d, hdr, CF_HDR_DEVICE + DL_ENTRY_SIZE, CF_HDR_EXTRA);
    if (hdr[CF_HDR_FORMAT] == 0) {
        put(&d->out, "  format         not recorded (cupl leaves it empty in the .abs file)\n");
    } else {
        field_text(text, (const char *)hdr + CF_HDR_FORMAT + 1, CF_HDR_DESIGN - CF_HDR_FORMAT - 1);
        put(&d->out, "  format         '%c' (%s)%s%s\n", hdr[CF_HDR_FORMAT],
            hdr[CF_HDR_FORMAT] == ':' ? "no minimization asked for"
            : hdr[CF_HDR_FORMAT] == '.' ? "minimized" : "unknown indicator",
            text[0] != '\0' ? ", FORMAT " : "", text);
    }
    put_text_tail(d, hdr, CF_HDR_FORMAT, CF_HDR_DESIGN);
    put(&d->out, "  symbols        %u\n", rd16(hdr + CF_HDR_SYMBOLS));
    extra = cf_header_extra(hdr);
    if (extra != 0) {
        size_t got;
        char *strings = malloc(extra);
        if (strings == NULL) {
            return 0;
        }
        got = fread(strings, 1, extra, d->fp);
        put(&d->out, "  header strings %u bytes\n", extra);
        put_header_strings(d, strings, got);
        free(strings);
        if (got < extra) {
            error_line(d, "the file ends inside the header strings (%lu of %u bytes)", (unsigned long)got, extra);
            return 0;
        }
    } else {
        put(&d->out, "  header strings none\n");
    }
    load_device(d, hdr);
    put(&d->out, "\n");

    /* the symbol table follows */
    {
        unsigned nsyms = rd16(hdr + CF_HDR_SYMBOLS);
        free(d->syms);
        free(d->labels);
        d->nsyms = 0;
        d->syms = calloc(nsyms + 1, sizeof(*d->syms));
        d->labels = calloc(nsyms + 1, sizeof(*d->labels));
        if (d->syms == NULL || d->labels == NULL) {
            return 0;
        }
        d->declared_nsyms = (int)nsyms;
        for (unsigned i = 0; i < nsyms; i++) {
            if (remaining(d) < CF_SYMBOL_SIZE) {
                return 2;
            }
            if (!cf_read_symbol(d->fp, &d->syms[i], CF_SYMBOL_SIZE)) {
                error_line(d, "symbol %u cannot be read", i);
                return 0;
            }
            d->nsyms = (int)i + 1;
        }
    }
    return 1;
}

/* -------------------------------------------------------------------------
 * Symbol table
 * ------------------------------------------------------------------------- */
static const char *kind_name(unsigned kind)
{
    switch (kind) {
    case 1: return "field";
    case 2: return "pin";
    case 3: return "var";
    case 4: return "end";
    case 5: return "pin.ext";
    case 6: return "const 0";
    case 7: return "const 1";
    case 8: return "generated";
    case 9: return "node";
    case 10: return "node.ext";
    case 11: return "function";
    case 14: return "group";
    default: return NULL;
    }
}

static void put_index(text_t *t, int width, int value)
{
    if (value == -1) {
        put(t, "%*s", width, "-");
    } else {
        put(t, "%*d", width, value);
    }
}

static void kind_text(char *buf, size_t size, unsigned type)
{
    const char *name = kind_name(type & 0x0f);
    if (name != NULL && (type & 0xf0) == 0) {
        snprintf(buf, size, "%s", name);
    } else if (name != NULL) {
        snprintf(buf, size, "%s 0x%02x", name, type);
    } else {
        snprintf(buf, size, "0x%02x", type);
    }
}

static void flags_text(char *buf, size_t size, unsigned flags)
{
    int n = snprintf(buf, size, "0x%02x", flags);
    if (flags & 0x80) n += snprintf(buf + n, size - (size_t)n, " in");
    if (flags & 0x40) n += snprintf(buf + n, size - (size_t)n, " out");
    if (flags & 0x10) n += snprintf(buf + n, size - (size_t)n, " pin-low");
    if (flags & 0x20) n += snprintf(buf + n, size - (size_t)n, " eqn-low");
    if (flags & 0x07) n += snprintf(buf + n, size - (size_t)n, " m%u", flags & 0x07);
    if (flags & 0x08) snprintf(buf + n, size - (size_t)n, " min");
}

static void dump_symbols(dump_t *d)
{
    int name_width = 4;
    int kind_width = 4;
    int flags_width = 5;
    for (int i = 0; i < d->nsyms; i++) {
        const cf_symbol_t *s = &d->syms[i];
        char name[CF_NAME_SIZE + 1];
        char ext[CF_EXT_SIZE + 1];
        char text[48];
        unsigned kind = s->type & 0x0f;
        field_text(name, s->name, CF_NAME_SIZE);
        field_text(ext, s->ext, CF_EXT_SIZE);
        if (i < 2 && name[0] == '\0' && (kind == 6 || kind == 7)) {
            snprintf(d->labels[i], sizeof(d->labels[i]), "<%c>", kind == 6 ? '0' : '1');
        } else if (ext[0] != '\0') {
            snprintf(d->labels[i], sizeof(d->labels[i]), "%s.%s", name, ext);
        } else {
            snprintf(d->labels[i], sizeof(d->labels[i]), "%s", name);
        }
        if ((int)strlen(d->labels[i]) > name_width) {
            name_width = (int)strlen(d->labels[i]);
        }
        kind_text(text, sizeof(text), s->type);
        if ((int)strlen(text) > kind_width) {
            kind_width = (int)strlen(text);
        }
        flags_text(text, sizeof(text), s->flags);
        if ((int)strlen(text) > flags_width) {
            flags_width = (int)strlen(text);
        }
    }
    if (d->nsyms < d->declared_nsyms) {
        put(&d->out, "Symbols (%d of %d)\n", d->nsyms, d->declared_nsyms);
    } else {
        put(&d->out, "Symbols (%d)\n", d->nsyms);
    }
    put(&d->out, "  idx  %-*s    pin  %-*s  %-*s  pol    regsel  pin-rec node-rec or-rec  ext  terms  cb#\n",
        name_width, "name", kind_width, "kind", flags_width, "flags");
    for (int i = 0; i < d->nsyms; i++) {
        const cf_symbol_t *s = &d->syms[i];
        char kind[24];
        char flags[48];
        char pol[8];
        char regsel[8];
        char terms[16];
        kind_text(kind, sizeof(kind), s->type);
        flags_text(flags, sizeof(flags), s->flags);
    switch (s->polarity) {
        case 0: snprintf(pol, sizeof(pol), "-"); break;
        case 1: snprintf(pol, sizeof(pol), "compl"); break;
        case 2: snprintf(pol, sizeof(pol), "twin"); break;
        default: snprintf(pol, sizeof(pol), "0x%02x", s->polarity); break;
        }
        switch (s->attr2) {
        case 0: snprintf(regsel, sizeof(regsel), "-"); break;
        case 1: snprintf(regsel, sizeof(regsel), "D"); break;
        case 2: snprintf(regsel, sizeof(regsel), "T"); break;
        case 3: snprintf(regsel, sizeof(regsel), "JK"); break;
        case 4: snprintf(regsel, sizeof(regsel), "SR"); break;
        case 5: snprintf(regsel, sizeof(regsel), "D/T"); break;
        default: snprintf(regsel, sizeof(regsel), "0x%02x", s->attr2); break;
        }
        if (s->terms == 0) {
            snprintf(terms, sizeof(terms), "-");
        } else if (s->terms == 1) {
            snprintf(terms, sizeof(terms), "yes");
        } else {
            snprintf(terms, sizeof(terms), "0x%x", (unsigned)s->terms);
        }
        put(&d->out, "  %3d  %-*s  %5u  %-*s  %-*s  %-5s  %-6s", i, name_width, d->labels[i], s->pin, kind_width, kind,
            flags_width, flags, pol, regsel);
        put_index(&d->out, 8, s->pin_index);
        put_index(&d->out, 9, s->node_index);
        put_index(&d->out, 7, s->or_index);
        put(&d->out, "  %3d  %-5s  %u", s->ext_code, terms, rd16(s->attr));
        if (s->attr[2] != 0 || s->attr[3] != 0) {
            put(&d->out, " (attr 2..3: %02x %02x)", s->attr[2], s->attr[3]);
        }
        put(&d->out, "\n");
    }
    put(&d->out,
        "  kind:   pin, pin.ext (pin with extension), node, node.ext, var (intermediate\n"
        "          variable), field, group, const 0/1, generated (node function added by cupl)\n"
        "  flags:  in = used in an equation, out = has an equation, pin-low = declared\n"
        "          active low (!name), eqn-low = equation written for !name, mN = minimization\n"
        "          level N, min = level given with MIN\n"
        "  pol:    compl = equation complemented for the output's polarity, twin = DeMorgan\n"
        "          or D/T alternative (not listed in the .sim file)\n"
        "  regsel: REGISTER_SELECT type; pin-rec, node-rec, or-rec: records in the device's\n"
        "          pin and node sections (or-rec: node of the OR array), - = none;\n"
        "          ext: extension number; terms: has product terms; cb#: number in the\n"
        "          parser's output\n\n");
}

/* -------------------------------------------------------------------------
 * Records: OR arrays and field lists
 * ------------------------------------------------------------------------- */

/* Frame flags as words */
static void put_frame_flags(text_t *t, unsigned flags)
{
    unsigned known = 0x0007 | CF_FRAME_FIELD | 0x1000;
    put(t, "flags 0x%04x (min level %u", flags, flags & 7);
    if (flags & 0x1000) {
        put(t, ", 0x1000 output not complemented");
    }
    if (flags & ~known) {
        put(t, ", unidentified 0x%04x", flags & ~known);
    }
    put(t, ")");
}

/* Does csim complement the OR array of output `s`?  1 yes, 0 no, -1 not
 * known; `why` says what decides it */
static int output_complemented(dump_t *d, int s, char *why, size_t why_size, unsigned or_array)
{
    const cf_symbol_t *sym = &d->syms[s];
    const uint8_t *node;
    unsigned kind;
    int true_out;
    why[0] = '\0';
    if (d->image == NULL) {
        return -1;
    }
    if (d->vsima) {
        int inverted = ((sym->flags & 0x20) != 0) != ((sym->flags & 0x10) != 0);
        snprintf(why, why_size, "vsima: the flags pin-low and eqn-low %s", inverted ? "differ" : "agree");
        return inverted;
    }
    if (sym->node_index < 0) {
        snprintf(why, why_size, "no node record");
        return -1;
    }
    node = dev_record(d, DEV_SEC_NODES, sym->node_index, DEV_NODE_REC_SIZE);
    if (node == NULL) {
        snprintf(why, why_size, "node record %d is outside the device", sym->node_index);
        return -1;
    }
    kind = node[DEV_NODE_KIND];
    true_out = node_true_out(kind);
    if (rd16(node + DEV_NODE_TERM) != or_array) {
        snprintf(why, why_size, "node record %d: cell type 0x%02x, kind %u, takes OR array %u", sym->node_index,
                 node[DEV_NODE_CELL], kind, rd16(node + DEV_NODE_TERM));
    } else {
        snprintf(why, why_size, "node record %d: cell type 0x%02x, kind %u", sym->node_index, node[DEV_NODE_CELL],
                 kind);
    }
    return true_out < 0 ? -1 : !true_out;
}

/* The sum of products of the rows that drive output `s`, as csim combines
 * them: the rows are ORed; a row with flag 0x0002 ends a first group, which
 * is XORed with the rows after it */
static void put_equation(dump_t *d, const cf_frame_t *f, const uint16_t *w, int nrows, int s, int complemented)
{
    int pw = cf_plane_words(f->nvars);
    int rw = 3 * pw;
    int split_after = -1;
    int first = 1;
    int last_row = -1;
    for (int r = 0; r < nrows; r++) {
        const uint16_t *out = w + (size_t)r * (size_t)rw;
        if (bit(out, s)) {
            last_row = r;
            if (split_after < 0 && (out[pw] & 0x0002)) {
                split_after = r;
            }
        }
    }
    if (split_after == last_row) {
        split_after = -1;           /* nothing follows the first group */
    }
    put(&d->out, "    %s = %s", label(d, s), complemented == 1 ? "!(" : "");
    if (split_after >= 0) {
        put(&d->out, "(");
    }
    for (int r = 0; r < nrows; r++) {
        const uint16_t *out = w + (size_t)r * (size_t)rw;
        const uint16_t *care = out + pw;
        const uint16_t *val = care + pw;
        int any = 0;
        if (!bit(out, s)) {
            continue;
        }
        if (!first) {
            put(&d->out, " # ");
        }
        first = 0;
        for (int i = 0; i < f->nvars; i++) {
            if (bit(care, i)) {
                if (any) {
                    put(&d->out, " & ");
                }
                put_variable(&d->out, d, i, bit(val, i));
                any = 1;
            }
        }
        if (!any) {
            put(&d->out, "1");
        }
        if (r == split_after) {
            put(&d->out, ") $ (");
            first = 1;
        }
    }
    if (first && split_after < 0) {
        put(&d->out, "0");
    }
    if (split_after >= 0) {
        put(&d->out, ")");
    }
    put(&d->out, "%s\n", complemented == 1 ? ")" : "");
}

static void dump_or_array(dump_t *d, const cf_frame_t *f, const uint16_t *w, int nrows)
{
    int pw = cf_plane_words(f->nvars);
    int rw = 3 * pw;
    int nvars = f->nvars;
    int ndriven = 0;
    char *used;
    char *driven;
    if (!d->seen_terms) {
        put(&d->out, "Product terms\n"
                     "  One row per product term: 1 = the variable is true, 0 = complemented,\n"
                     "  - = not used.  Only the variables some row uses are columns.  The terms\n"
                     "  are ORed; the equation below the output says what csim makes of them.\n"
                     "  Equations are in pin levels: for a pin declared active low (pin-low)\n"
                     "  the level is the complement of the signal.  Flag 0x1000: cupl chose\n"
                     "  the node that takes the OR array as it is, where the output has one\n"
                     "  that does and one that complements it.\n\n");
        d->seen_terms = 1;
    }
    if (f->max_terms != 0) {
        put(&d->out, "  OR array %u: %u of at most %u terms, ", f->or_array, f->nrows, f->max_terms);
    } else {
        put(&d->out, "  OR array %u: %u terms, ", f->or_array, f->nrows);
    }
    put_frame_flags(&d->out, f->flags);
    put(&d->out, ", %u variables", nvars);
    if (f->has_xor != 0) {
        put(&d->out, ", word 8: 0x%04x", f->has_xor);
    }
    put(&d->out, "\n");
    if (d->image != NULL && !d->vsima) {
        unsigned count = rd16(dev_header(d) + DEV_HDR_OR_ARRAYS);
        if (f->or_array >= count) {
            put(&d->out, "    the device has %u OR arrays: csim reports an error for this one\n", count);
        }
    }
    used = calloc((size_t)nvars + 1, 1);
    driven = calloc((size_t)nvars + 1, 1);
    if (used == NULL || driven == NULL) {
        free(used);
        free(driven);
        return;
    }
    for (int r = 0; r < nrows; r++) {
        const uint16_t *out = w + (size_t)r * (size_t)rw;
        for (int i = 0; i < nvars; i++) {
            used[i] |= (char)bit(out + pw, i);
            if (bit(out, i) && !driven[i]) {
                driven[i] = 1;
                ndriven++;
            }
        }
    }
    if (ndriven == 0) {
        put(&d->out, "    drives no symbol\n");
    }
    for (int s = 0; s < nvars; s++) {
        char why[96];
        int complemented;
        if (!driven[s]) {
            continue;
        }
        if (s >= d->nsyms) {
            put(&d->out, "    drives #%d, which is not in the symbol table\n", s);
            put_equation(d, f, w, nrows, s, -1);
            continue;
        }
        complemented = output_complemented(d, s, why, sizeof(why), f->or_array);
        put(&d->out, "    drives %s (pin %u%s)", label(d, s), d->syms[s].pin,
            (d->syms[s].flags & 0x10) ? ", declared active low" : "");
        if (complemented == 1) {
            put(&d->out, ": csim complements the OR array (%s)\n", why);
        } else if (complemented == 0) {
            put(&d->out, ": csim takes the OR array as it is (%s)\n", why);
        } else if (why[0] != '\0') {
            put(&d->out, ": output polarity not known (%s)\n", why);
        } else {
            put(&d->out, "\n");
        }
        put_equation(d, f, w, nrows, s, complemented);
    }

    /* the rows */
    {
        int width[1024];
        int ncols = 0;
        int cols[1024];
        for (int i = 0; i < nvars && ncols < 1024; i++) {
            if (used[i]) {
                int len = (int)strlen(label(d, i));
                cols[ncols] = i;
                width[ncols] = len < 1 ? 1 : len;
                ncols++;
            }
        }
        put(&d->out, "    term ");
        for (int c = 0; c < ncols; c++) {
            put(&d->out, " %-*s", width[c], label(d, cols[c]));
        }
        put(&d->out, "\n");
        for (int r = 0; r < nrows; r++) {
            const uint16_t *out = w + (size_t)r * (size_t)rw;
            const uint16_t *care = out + pw;
            const uint16_t *val = care + pw;
            int stray = 0;
            int beyond = 0;
            put(&d->out, "    %4d ", r + 1);
            for (int c = 0; c < ncols; c++) {
                int i = cols[c];
                put(&d->out, " %c", bit(care, i) ? (bit(val, i) ? '1' : '0') : '-');
                if (c + 1 < ncols) {
                    put_spaces(&d->out, width[c] - 1);
                }
            }
            if (ndriven > 1) {
                const char *sep = "  -> ";
                for (int i = 0; i < nvars; i++) {
                    if (bit(out, i)) {
                        put(&d->out, "%s%s", sep, label(d, i));
                        sep = ", ";
                    }
                }
            }
            if (care[0] & 0x0002) {
                put(&d->out, "  | end of the first XOR group");
            }
            if (care[0] & 0x0004) {
                put(&d->out, "  (marked deleted, csim evaluates it all the same)");
            }
            if (out[0] != 0 || (care[0] & ~0x0007u) != 0 || val[0] != 0) {
                put(&d->out, "  [word 0: 0x%04x 0x%04x 0x%04x]", out[0], care[0], val[0]);
            }
            for (int i = 0; i < nvars; i++) {
                stray |= bit(val, i) && !bit(care, i);
            }
            for (int i = nvars; i < (pw - 1) * 16; i++) {
                beyond |= bit(out, i) | bit(care, i) | bit(val, i);
            }
            if (stray) {
                put(&d->out, "  [true bits of unused variables:");
                for (int i = 0; i < nvars; i++) {
                    if (bit(val, i) && !bit(care, i)) {
                        put(&d->out, " %s", label(d, i));
                    }
                }
                put(&d->out, "]");
            }
            if (beyond) {
                put(&d->out, "  [bits beyond the %d variables set]", nvars);
            }
            put(&d->out, "\n");
        }
        if (ncols == 1024) {
            put(&d->out, "    (only the first 1024 variables are shown)\n");
        }
    }
    put(&d->out, "\n");
    free(used);
    free(driven);
}

/* A field list: the field symbol is the first bit of plane 0 in the first
 * row; each row names members in plane 1, and plane 2 says which of them
 * are listed true (csim, field_members) */
static void dump_field(dump_t *d, const cf_frame_t *f, const uint16_t *w, int nrows)
{
    int pw = cf_plane_words(f->nvars);
    int rw = 3 * pw;
    int field = -1;
    const char *sep = "";
    if (!d->seen_fields) {
        put(&d->out, "Field lists\n");
        d->seen_fields = 1;
    }
    for (int i = 0; nrows > 0 && i < f->nvars; i++) {
        if (bit(w, i)) {
            field = i;
            break;
        }
    }
    if (field < 0) {
        put(&d->out, "  FIELD (none: no field symbol in the first row; csim reports an unreferenced field) = ");
    } else {
        put(&d->out, "  FIELD %s = ", label(d, field));
    }
    for (int r = 0; r < nrows; r++) {
        const uint16_t *members = w + (size_t)r * (size_t)rw + pw;
        const uint16_t *marks = members + pw;
        for (int i = 0; i < f->nvars; i++) {
            if (bit(members, i)) {
                put(&d->out, "%s%s%s", sep, bit(marks, i) ? "" : "!", label(d, i));
                sep = ", ";
            }
        }
    }
    put(&d->out, "\n    record: ");
    put_frame_flags(&d->out, f->flags);
    put(&d->out, ", word 2: %u, %u variables, %u rows, word 8: 0x%04x, word 10: %u\n", f->or_array, f->nvars,
        f->nrows, f->has_xor, f->max_terms);
}

/* The records up to the end record; returns 1 if it was found */
static int dump_records(dump_t *d)
{
    d->row_words = -1;
    d->seen_terms = 0;
    d->seen_fields = 0;
    for (;;) {
        cf_frame_t f;
        long offset = position(d);
        size_t words;
        size_t got;
        uint16_t *w;
        int rw;
        int rows;
        if (remaining(d) < CF_FRAME_SIZE) {
            error_line(d, "the file ends before the end record (offset 0x%lx, %ld bytes left)", offset,
                       remaining(d));
            put_rest_hex(d, "  ", (size_t)remaining(d));
            return 0;
        }
        if (!cf_read_frame(d->fp, &f)) {
            error_line(d, "the record at offset 0x%lx cannot be read", offset);
            return 0;
        }
        if (f.flags & (CF_FRAME_END_OF_FILE | CF_FRAME_END_OF_DESIGN)) {
            put(&d->out, "End record (offset 0x%lx): flags 0x%04x (%s)", offset, f.flags,
                (f.flags & CF_FRAME_END_OF_DESIGN) ? "end of design" : "end of file");
            put(&d->out, "; its other words are left over from the last record: %u %u %u 0x%04x %u\n", f.or_array,
                f.nvars, f.nrows, f.has_xor, f.max_terms);
            return 1;
        }
        rw = 3 * cf_plane_words(f.nvars);
        if (d->row_words < 0) {
            d->row_words = rw;
        } else if (rw != d->row_words) {
            error_line(d, "the record at offset 0x%lx has rows of %d words, the first record %d "
                       "(csim stops: pt width mismatch)", offset, rw, d->row_words);
        }
        words = (size_t)f.nrows * (size_t)rw;
        if (words * 2 > (size_t)remaining(d)) {
            words = (size_t)remaining(d) / 2;
        }
        w = calloc(words + 1, sizeof(uint16_t));
        if (w == NULL) {
            return 0;
        }
        got = cf_read_words(d->fp, w, words);
        rows = (int)(got / (size_t)rw);
        if (f.flags & CF_FRAME_FIELD) {
            dump_field(d, &f, w, rows);
        } else {
            dump_or_array(d, &f, w, rows);
        }
        if (f.nrows == 0) {
            put(&d->out, "    a record without rows: csima takes it for the end of the file\n\n");
        }
        if (rows < f.nrows) {
            error_line(d, "the file ends inside the record at offset 0x%lx: %d of %u rows", offset, rows, f.nrows);
            if (got % (size_t)rw != 0) {
                put(&d->out, "  incomplete row %d:", rows + 1);
                for (size_t i = got - got % (size_t)rw; i < got; i++) {
                    put(&d->out, " %04x", w[i]);
                }
                put(&d->out, "\n");
            }
            free(w);
            put_rest_hex(d, "  ", (size_t)remaining(d));
            return 0;
        }
        free(w);
    }
}

/* -------------------------------------------------------------------------
 * Entry points
 * ------------------------------------------------------------------------- */
char *absdump_stream(FILE *fp, const char *lib_path, int *complete)
{
    dump_t d;
    int section = 0;
    memset(&d, 0, sizeof(d));
    d.fp = fp;
    d.lib_path = lib_path;
    d.base = ftell(fp);
    if (d.base >= 0 && fseek(fp, 0, SEEK_END) == 0) {
        d.size = ftell(fp) - d.base;
        fseek(fp, d.base, SEEK_SET);
    } else {
        d.base = 0;
        d.size = 0x7fffffff;
    }
    for (;;) {
        uint8_t next[2];
        long pos;
        int status;
        if (section > 0) {
            put(&d.out, "\nNext design section (offset 0x%lx)\n\n", position(&d));
        }
        section++;
        status = dump_header(&d);
        if (status == 0) {
            break;
        }
        dump_symbols(&d);
        if (status == 2) {
            error_line(&d, "the file ends inside the symbol table: %d of %d symbols, %ld bytes left", d.nsyms,
                       d.declared_nsyms, remaining(&d));
            put_rest_hex(&d, "  ", (size_t)remaining(&d));
            break;
        }
        if (!dump_records(&d)) {
            break;
        }
        if (remaining(&d) == 0) {
            break;
        }
        pos = position(&d);
        if (fread(next, 1, 2, fp) == 2 && rd16(next) == CF_MAGIC_ABS) {
            fseek(fp, d.base + pos, SEEK_SET);
            continue;
        }
        fseek(fp, d.base + pos, SEEK_SET);
        put(&d.out, "Bytes after the end record (%ld), not read by csim:\n", remaining(&d));
        put_rest_hex(&d, "  ", (size_t)remaining(&d));
        break;
    }
    free(d.syms);
    free(d.labels);
    free(d.image);
    if (complete != NULL) {
        *complete = d.errors == 0 && !d.out.oom;
    }
    if (d.out.oom) {
        free(d.out.buf);
        return NULL;
    }
    if (d.out.buf == NULL) {
        put(&d.out, "%s", "");
        if (d.out.oom) {
            return NULL;
        }
    }
    return d.out.buf;
}

char *absdump_file(const char *path, const char *lib_path, int *complete)
{
    FILE *fp = lincupl_fopen_ci(path, "rb");
    char *text;
    if (fp == NULL) {
        size_t size = strlen(path) + 64;
        text = malloc(size);
        if (text != NULL) {
            snprintf(text, size, "*** error: %s cannot be opened\n", path);
        }
        if (complete != NULL) {
            *complete = 0;
        }
        return text;
    }
    text = absdump_stream(fp, lib_path, complete);
    fclose(fp);
    return text;
}
