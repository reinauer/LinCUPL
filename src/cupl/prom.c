/*
 * prom.c - download formats other than JEDEC: PROM bit map and ASCII-hex
 * file (.hex), Altera programmer object file (.pof) for the max5032.
 */
#include "banner.h"
#include "fusemap.h"

/* PROM map */
static uint16_t *s_words;           /* one word per address */
static int s_prom_built;
static int s_nwords;
static int s_width;                 /* data bits per word */
static int s_addr_bits;

/* POF */
static FILE *s_pof_fp;
static char *s_pof_name;
static unsigned s_crc;              /* CRC of the bytes written so far */

void dl_reset(void)
{
    s_words = NULL;
    s_prom_built = 0;
    s_nwords = s_width = s_addr_bits = 0;
    s_pof_fp = NULL;
    s_pof_name = NULL;
    s_crc = 0;
}

/* -------------------------------------------------------------------------
 * PROM
 * ------------------------------------------------------------------------- */

/* Every word starts with the bits of the active-low outputs set */
static unsigned init_prom_words(void)
{
    unsigned mask = 0;
    for (int i = 0; i < g.nsyms; i++) {
        const symbol_t *s = &g.syms[i];
        int inv = (s->flags & SYMF_INVERTED) != 0;
        int f20 = (s->flags & SYMF_20) != 0;
        if (s->node_index >= 0 && s->or_index >= 0 && inv != f20) {
            mask |= 1u << (node32(s->node_index, NODE_OR_FUSE) & 0x0f);
        }
    }
    mask &= 0xffff;
    for (int i = 0; i < s_nwords; i++) {
        s_words[i] = (uint16_t)mask;
    }
    return mask;
}

/* Number of addresses covered by the inputs a term leaves open,
 * minus one */
static unsigned free_address_count(unsigned free_mask, int bits)
{
    unsigned result = 0;
    unsigned bit = 1;
    for (int i = 0; i < bits; i++) {
        if (free_mask & 1) {
            result |= bit;
            bit <<= 1;
        }
        free_mask >>= 1;
    }
    return result & 0xffff;
}

/* Spread the bits of 'counter' over the address bits that the
 * term does not fix */
static unsigned make_address(int bits, unsigned counter, unsigned fixed_mask, unsigned fixed_value)
{
    unsigned result = 0;
    unsigned bit = 1;
    for (int i = 0; i < bits; i++) {
        if (!(bit & fixed_mask)) {
            if (counter & 1) {
                result |= bit;
            }
            counter >>= 1;
        }
        bit <<= 1;
    }
    return (result | fixed_value) & 0xffff;
}

/* Enter one product term in every word it selects */
static void add_prom_term(const uint16_t *out, const uint16_t *care, uint16_t *val, unsigned dflt)
{
    unsigned out_mask = 0;
    unsigned fixed_value = 0;
    unsigned fixed_mask = 0;
    if (term_bit(care, 0)) {
        return;
    }
    int always = term_bit(care, 1);
    for (int i = 2; i < g.nsyms; i++) {
        const symbol_t *s = &g.syms[i];
        if (!always && term_bit(care, i)) {
            if (s->pin_index < 0) {
                report_symbol_error(1021, s);
                continue;
            }
            unsigned bit = 1u << (pin8(s->pin_index, PIN_TRUE_COL) & 0x0f);
            int inv = (s->flags & SYMF_INVERTED) != 0;
            fixed_mask |= bit;
            if (term_bit(val, i) == inv) {
                term_clear_bit(val, i);
            } else {
                fixed_value |= bit;
                term_set_bit(val, i);
            }
        }
        if (term_bit(out, i)) {
            if (s->node_index < 0 || s->or_index < 0) {
                report_symbol_error(1022, s);
            } else {
                out_mask |= 1u << (node32(s->node_index, NODE_OR_FUSE) & 0x0f);
            }
        }
    }
    unsigned last = free_address_count(~fixed_mask & 0xffff, s_addr_bits);
    for (unsigned k = 0; k <= last; k++) {
        unsigned addr = always ? k : make_address(s_addr_bits, k, fixed_mask & 0xffff, fixed_value & 0xffff);
        if ((int)addr >= s_nwords) {
            continue;       /* address outside the map */
        }
        unsigned bit = 1;
        for (int b = 0; b < s_width; b++, bit <<= 1) {
            if (~dflt & bit & out_mask) {
                s_words[addr] |= (uint16_t)(1u << (b & 15));
            } else if (bit & out_mask & dflt) {
                s_words[addr] &= (uint16_t)~(1u << (b & 15));
            }
        }
    }
}

/* Build the PROM contents (one word per address) from the product terms */
void build_prom_map(void)
{
    s_width = (int)(attr8(0x0a) % 0x11);
    s_nwords = (int)attr16(ATTR_NROWS);
    s_addr_bits = (int)attr8(0x09);
    s_words = alloc_zeroed((size_t)s_nwords + 1, sizeof(uint16_t), "creating rom bit map");
    s_prom_built = 1;
    unsigned dflt = init_prom_words();
    for (int i = 0; i < g.nor_arrays; i++) {
        or_array_t *oa = &g.or_arrays[i];
        if (oa->sym == NULL || oa->sym->or_index < 0 || oa->nterms <= 0) {
            continue;
        }
        for (equation_t *eq = oa->eq; eq != NULL; eq = eq->next) {
            for (int t = 0; t < eq->nterms; t++) {
                if (!(TERM_CARE(eq, t)[0] & TERM_DELETED)) {
                    add_prom_term(TERM_OUT(eq, t), TERM_CARE(eq, t), TERM_VAL(eq, t), dflt);
                }
            }
        }
    }
}

/* PROM contents in the fuse plot, four words per line */
void write_prom_plot(FILE *fp)
{
    if (!s_prom_built) {
        report_error(1023, "");
        return;
    }
    int index = 0;
    for (int row = 0; row < s_nwords / 4; row++) {
        stream_printf(fp, "%04x  ", index);
        for (int k = 0; k < 4; k++, index++) {
            unsigned word = s_words[index];
            unsigned bit = 1u << ((s_width - 1) & 0x1f);
            for (int b = 0; b < s_width; b++, bit >>= 1) {
                out_putc((word & bit) ? '1' : '0', fp);
            }
            out_putc(' ', fp);
            out_putc(' ', fp);
        }
        out_putc('\n', fp);
    }
}

static void put_upper(FILE *fp, const char *fmt, unsigned value)
{
    char text[64];
    snprintf(text, sizeof(text), fmt, value);
    for (char *p = text; *p != '\0'; p++) {
        if (*p >= 'a' && *p <= 'z') {
            *p = (char)(*p - 'a' + 'A');
        }
    }
    stream_printf(fp, "%s", text);
}

/* ASCII-hex download file */
void write_hex(void)
{
    if (!s_prom_built) {
        report_error(1024, "");
        return;
    }
    char *name;
    if (!(g.options & OPT_N)) {
        if (strcmp(g.design_name, "no_name") == 0) {
            report_warning(ERR_NO_NAME, "hex");
        }
        name = make_path(source_dir(g.design_name), g.design_name, ".hex");
    } else {
        name = make_path(source_dir(g.args[5]), g.args[5], ".hex");
    }
    FILE *fp = open_file(name, "wt");
    if (fp == NULL) {
        fatal_error(ERR_OPEN, name);
    }
    unsigned sum = 0;
    int index = 0;
    stream_printf(fp, "%c\n", 2);
    check_write(fp, name);
    for (int row = 0; row < s_nwords / 16; row++) {
        put_upper(fp, "$A%04x,", (unsigned)index);
        for (int k = 0; k < 16; k++, index++) {
            unsigned word = s_words[index];
            switch (s_width) {
            case 8:
                put_upper(fp, "%02x ", word & 0xff);
                break;
            case 0x0c:
                put_upper(fp, "%03x ", word & 0xfff);
                break;
            case 0x10:
                put_upper(fp, "%04x ", word);
                break;
            default:
                put_upper(fp, "%x ", word & 0x0f);
                break;
            }
            sum = (sum + word) & 0xffff;
        }
        stream_printf(fp, "\n");
    }
    stream_printf(fp, "%c\n", 3);
    put_upper(fp, "$S%04x,", sum);
    for (int i = 0; i < 0x28; i++) {
        out_putc(' ', fp);
    }
    check_write(fp, name);
    close_file(fp);
}

/* -------------------------------------------------------------------------
 * POF (max5032)
 * ------------------------------------------------------------------------- */

/* order of the fuse columns of a macrocell */
static const uint8_t k_pof_columns[32] = {
    1, 0, 2, 3, 7, 6, 4, 5, 12, 13, 15, 14, 10, 11, 9, 8,
    25, 24, 26, 27, 31, 30, 28, 29, 20, 21, 23, 22, 18, 19, 17, 16
};
/* first column of each of the four bytes of a row */
static const uint8_t k_pof_first[4] = { 1, 9, 25, 17 };
/* bit of the byte that receives each of eight consecutive columns */
static const uint8_t k_pof_bits[8] = { 0x40, 0x80, 0x20, 0x10, 0x01, 0x02, 0x08, 0x04 };
/* the seven fuse packets: two header words, rows, columns, two more
 * header words (bit 0 of the last one doubles the data size) */
static const uint16_t k_pof_packets[7][6] = {
    { 0, 0, 40, 128, 255, 0 },
    { 0, 160, 40, 8, 255, 0 },
    { 0, 173, 40, 1, 255, 0 },
    { 0, 176, 40, 8, 255, 0 },
    { 0, 189, 40, 1, 255, 0 },
    { 40, 255, 1, 1, 15, 0 },
    { 255, 254, 1, 1, 255, 0 }
};

/* CRC-16 (CCITT, reflected) over the bytes written */
static void pof_crc(unsigned byte)
{
    for (int i = 0; i < 8; i++) {
        unsigned x = byte ^ s_crc;
        s_crc >>= 1;
        if (x & 1) {
            s_crc ^= 0x8408;
        }
        byte >>= 1;
    }
}

static void pof_put(const uint8_t *data, size_t count, int with_crc)
{
    for (size_t i = 0; i < count; i++) {
        if (fputc(data[i], s_pof_fp) == EOF) {
            fatal_error(ERR_DISK_WRITE, s_pof_name);
        }
        if (with_crc) {
            pof_crc(data[i]);
        }
    }
}

/* Packet with a text: tag, length including the NUL, text */
static void pof_text_packet(unsigned tag, const char *text)
{
    uint8_t buf[6 + 260];
    size_t len = strlen(text);
    if (len > 256) {
        len = 256;
    }
    memset(buf, 0, sizeof(buf));
    wr16(buf, tag);
    wr32(buf + 2, (uint32_t)len + 1);
    memcpy(buf + 6, text, len);
    pof_put(buf, len + 7, 1);
}

/* Creator, device and design header packets */
static void pof_write_texts(void)
{
    char text[600];
    char when[64];
    snprintf(text, sizeof(text), "Created by %s %s", PROGRAM_NAME, PROGRAM_VERSION);
    snprintf(when, sizeof(when), " on %s", creation_time_text());
    if (when[0] != '\0') {
        when[strlen(when) - 1] = '\0';      /* drop the line feed of ctime() */
    }
    snprintf(text + strlen(text), sizeof(text) - strlen(text), "%s", when);
    pof_text_packet(1, text);
    pof_text_packet(2, "EPM5032");
    static const char *const labels[8] = {
        "Name", "Partno", "Revision", "Date", "Designer", "Company", "Assembly", "Location"
    };
    text[0] = '\0';
    for (int i = 0; i < 8; i++) {
        char item[400];
        snprintf(item, sizeof(item), "%-12s%s | ", labels[i], header_string(i));
        if (strlen(text) + strlen(item) < 0x100) {
            strcat(text, item);
        }
    }
    pof_text_packet(3, text);
}

/* Eight fuses of one row of the AND array as a byte */
static unsigned pof_array_byte(int first, int row, int col)
{
    unsigned value = 0;
    for (int j = 0; j < 8; j++) {
        if (get_fuse((uint32_t)k_pof_columns[(first + j) & 31] * 0x5a0 + (uint32_t)row * 0x90 + (uint32_t)col) == 1) {
            value |= k_pof_bits[j];
        }
    }
    return value;
}

/* Data of a packet of AND-array fuses: rows/4 groups of four rows, 'cols'
 * bytes each, starting at fuse column 'offset' */
static void pof_array_packet(uint8_t *data, unsigned rows, unsigned cols, int offset)
{
    for (unsigned r = 0; r < rows >> 2; r++) {
        for (unsigned c = 0; c < 4; c++) {
            for (unsigned k = 0; k < cols; k++) {
                data[r * 4 * cols + c * cols + k] = (uint8_t)pof_array_byte(k_pof_first[c], (int)r, (int)k + offset);
            }
        }
    }
    data[rows * cols] = 0;
}

/* Data of a packet of macrocell fuses: 'rows' bytes, each made of eight
 * fuses 0x50 apart, starting at 'base' */
static void pof_cell_packet(uint8_t *data, unsigned rows, uint32_t base)
{
    for (unsigned r = 0; r < rows >> 2; r++, base += 2) {
        for (unsigned c = 0; c < 4; c++) {
            uint32_t fuse = c * 0x14 + base;
            unsigned value = 0;
            for (int j = 0; j < 8; j++, fuse += 0x50) {
                if (get_fuse(fuse)) {
                    value |= 1u << j;
                }
            }
            data[r * 4 + c] = (uint8_t)value;
        }
    }
    data[rows] = 0;
}

/* The fuse packets */
static void pof_write_fuses(void)
{
    for (int p = 0; p < 7; p++) {
        const uint16_t *t = k_pof_packets[p];
        unsigned rows = t[2];
        unsigned cols = t[3];
        uint8_t head[0x12];
        size_t size = (size_t)rows * cols;
        wr16(head, 7);
        wr32(head + 2, (uint32_t)size + 0x0c);
        wr16(head + 6, t[0]);
        wr16(head + 8, t[1]);
        wr16(head + 10, rows);
        wr16(head + 12, cols);
        wr16(head + 14, t[4]);
        wr16(head + 16, t[5]);
        uint8_t *data = alloc_zeroed(size * 2 + 16, 1, "creating electrical packets");
        switch (p) {
        case 0:
            pof_array_packet(data, rows, cols, 0);
            break;
        case 1:
            pof_array_packet(data, rows, cols, 0x80);
            break;
        case 2:
            pof_cell_packet(data, rows, 0xb400);
            break;
        case 3:
            pof_array_packet(data, rows, cols, 0x88);
            break;
        case 4:
            pof_cell_packet(data, rows, 0xb401);
            break;
        case 5: {
            unsigned value = 0;
            for (int j = 0; j < 4; j++) {
                if (get_fuse(0xb680 + (uint32_t)j)) {
                    value |= 1u << j;
                }
            }
            data[0] = (uint8_t)value;
            data[1] = 0;
            break;
        }
        default:
            data[0] = 0xff;
            data[1] = 0;
            break;
        }
        pof_put(head, sizeof(head), 1);
        pof_put(data, (t[5] & 1) ? size * 2 : size, 1);
        free_tracked(data);
    }
}

/* Programmer object file (<design>.pof) of the max5032 */
void write_pof(void)
{
    uint8_t rec[12];
    build_fuse_map();
    s_crc = 0xffff;
    s_pof_name = make_path(source_dir(g.args[5]), g.args[5], ".pof");
    s_pof_fp = open_file(s_pof_name, "wt");
    if (s_pof_fp == NULL) {
        fatal_error(ERR_OPEN, s_pof_name);
    }
    /* file header */
    memset(rec, 0, sizeof(rec));
    memcpy(rec, "POF", 4);
    wr16(rec + 6, 1);
    wr32(rec + 8, 0x0c);
    pof_put(rec, 12, 1);
    pof_write_texts();
    /* security packet */
    memset(rec, 0, sizeof(rec));
    wr16(rec, 5);
    wr32(rec + 2, 2);
    wr16(rec + 6, (g.options & OPT_G) ? 1 : 0);
    pof_put(rec, 8, 1);
    pof_write_fuses();
    /* checksum packet */
    memset(rec, 0, sizeof(rec));
    wr16(rec, 8);
    wr32(rec + 2, 2);
    wr16(rec + 6, ~s_crc & 0xffff);
    pof_put(rec, 8, 0);
    close_file(s_pof_fp);
    s_pof_fp = NULL;
}
