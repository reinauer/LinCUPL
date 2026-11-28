/*
 * devlib.c - reading the device libraries (.dl), see devlib.h.
 */
#include "platform.h"
#include <string.h>
#include "devlib.h"

int dl_read_header(FILE *fp, uint8_t *header)
{
    if (fread(header, DL_HEADER_SIZE, 1, fp) == 0) {
        return DL_NO_HEADER;
    }
    if (rd16(header) != DL_MAGIC) {
        return DL_BAD_MAGIC;
    }
    if (rd16(header + 2) != DL_ACCESS_KEY) {
        return DL_BAD_KEY;
    }
    return DL_OK;
}

int dl_find_device(FILE *fp, const char *name, int match, uint8_t *entry)
{
    if (fseek(fp, DL_HEADER_SIZE, SEEK_SET) != 0) {
        return 0;
    }
    for (int i = 0; i < DL_MAX_ENTRIES; i++) {
        char entry_name[DL_ENTRY_NAME_SIZE + 1];
        if (fread(entry, DL_ENTRY_SIZE, 1, fp) == 0) {
            break;
        }
        memcpy(entry_name, entry, DL_ENTRY_NAME_SIZE);
        entry_name[DL_ENTRY_NAME_SIZE] = '\0';
        if ((entry[DL_ENTRY_FLAGS] & 1) != 0 &&
            (match == DL_IGNORE_CASE ? strcasecmp(name, entry_name) : strcmp(name, entry_name)) == 0) {
            return 1;
        }
    }
    return 0;
}

int dl_seek_image(FILE *fp, const uint8_t *entry)
{
    return fseek(fp, (long)dl_image_offset(entry), SEEK_SET) == 0;
}

size_t dl_read_image(FILE *fp, uint8_t *image, size_t size)
{
    size_t done = 0;
    while (done < size) {
        size_t chunk = size - done < DL_READ_BLOCK ? size - done : DL_READ_BLOCK;
        size_t got = fread(image + done, 1, chunk, fp);
        done += got;
        if (got < chunk) {
            break;
        }
    }
    return done;
}

/* A bounded view of fixed-width records in an image section. */
static uint8_t *image_records(uint8_t *image, size_t size, unsigned section,
                              unsigned width, unsigned *count)
{
    *count = 0;
    if (size < DL_SECTIONS * DL_SECTION_HEADER_SIZE || section >= DL_SECTIONS) {
        return NULL;
    }
    const uint8_t *header = image + section * DL_SECTION_HEADER_SIZE;
    size_t offset = rd32(header + 8);
    unsigned n = rd16(header + 4);
    if (rd16(header + 6) != width || offset > size || n > (size - offset) / width ||
        (n != 0 && offset < DL_SECTIONS * DL_SECTION_HEADER_SIZE)) {
        return NULL;
    }
    *count = n;
    return image + offset;
}

static void correct_vp8_enable_pin(uint8_t *image, size_t size)
{
    unsigned pin_count, count;
    const uint8_t *pins = image_records(image, size, 6, 16, &pin_count);
    uint8_t *links = image_records(image, size, 10, 13, &count);
    for (unsigned i = 0; i < count; i++) {
        uint8_t *c = links + 13 * i;
        if (rd16(c + 5) != 0x11) {
            continue;
        }
        /* Resolve the enable by its pin record's line, not the stale
         * package number in the registered-mode connection record. */
        for (unsigned j = 0; j < pin_count; j++) {
            const uint8_t *p = pins + 16 * j;
            if (rd16(p) == rd16(c) && p[5] == 9) {
                wr16(c + 2, rd16(p + 2));
                break;
            }
        }
    }
}

static void correct_v8h_complex_mode(uint8_t *image, size_t size)
{
    unsigned count;
    uint8_t *init = image_records(image, size, 9, 9, &count);
    int nonregistered = 0;
    for (unsigned i = 0; i < count; i++) {
        const uint8_t *r = init + 9 * i;
        if (rd32(r) == 2192 && rd32(r + 4) == 2192 && r[8] == 1) {
            nonregistered = 1;
        }
    }
    if (!nonregistered) {
        return;
    }
    for (unsigned i = 0; i < count; i++) {
        uint8_t *r = init + 9 * i;
        /* SG0/SG1 = 1/1 selects product-term output enable. The 1/0
         * setting instead selects simple outputs or dedicated inputs. */
        if (rd32(r) == 2193 && rd32(r + 4) == 2193 && r[8] == 0) {
            r[8] = 1;
        }
    }
}

static void correct_v8h_simple_input(uint8_t *image, size_t size)
{
    unsigned count;
    uint8_t *pins = image_records(image, size, 6, 16, &count);
    for (unsigned i = 0; i < count; i++) {
        uint8_t *p = pins + 16 * i;
        /* The unlatched input on line 16 names ground pin 20 instead
         * of pin 12. Require the matching LQ route before changing it. */
        if (rd16(p) != 16 || rd16(p + 2) != 20 || p[4] != 0 || p[5] != 0 ||
            p[7] != 30 || p[8] != 31 || p[10] != 1 || p[11] != 1 ||
            rd32(p + 12) != 2209) {
            continue;
        }
        for (unsigned j = 0; j < count; j++) {
            const uint8_t *q = pins + 16 * j;
            if (rd16(q) == rd16(p) && rd16(q + 2) == 12 &&
                q[4] == 0x18 && q[5] == 0 && q[10] == 0 &&
                memcmp(q + 6, p + 6, 4) == 0 &&
                memcmp(q + 11, p + 11, 5) == 0) {
                wr16(p + 2, rd16(q + 2));
                break;
            }
        }
    }
}

static void correct_atf750_feedback(uint8_t *image, size_t size)
{
    unsigned count;
    uint8_t *pins = image_records(image, size, 6, 16, &count);
    for (unsigned i = 0; i < count; i++) {
        uint8_t *p = pins + 16 * i;
        /* TFB names a feedback path. Only the D/T equation may select
         * the register type; a read must not turn a D register into T. */
        if (p[4] == 0x24) {
            p[11] = 0;
        }
    }
}

static void correct_2500_buried_functions(uint8_t *image, size_t size)
{
    unsigned count;
    const uint8_t *attrs = image_records(image, size, 3, 152, &count);
    if (count != 1) {
        return;
    }
    unsigned npins = attrs[8];
    uint8_t *nodes = image_records(image, size, 7, 39, &count);
    for (unsigned i = 0; i < count;) {
        uint8_t *n = nodes + 39 * i;
        unsigned number = rd16(n + 2);
        /* The first 24 internal nodes are buried registers. Their data
         * sum has no combinational bypass; output-register aliases follow. */
        if (number > npins && number <= npins + 24 && n[4] == 0) {
            memmove(n, n + 39, (count - i - 1) * 39);
            wr16(image + 7 * DL_SECTION_HEADER_SIZE + 4, --count);
        } else {
            i++;
        }
    }
}

static void correct_gal24v10_rows(uint8_t *image, size_t size)
{
    enum { NODE_SECTION = 7, NODE_SIZE = 39, INIT_SECTION = 9, INIT_SIZE = 9 };
    unsigned count;
    uint8_t *nodes = image_records(image, size, NODE_SECTION, NODE_SIZE, &count);
    for (unsigned i = 0; i < count; i++) {
        uint8_t *n = nodes + i * NODE_SIZE;
        /* Pin 19 has eight 48-fuse rows at 2304. Combinational mode
         * reserves the first for OE, so data starts at 2352. */
        if (rd16(n + 2) == 19 && n[4] == 14 && rd32(n + 10) == 2452) {
            wr32(n + 10, 2352);
        }
        /* The combinational array loses one data row to output enable.
         * Registered arrays on the same pin retain all eight rows. */
        if (n[4] == 14 && rd16(n + 2) >= 16 && rd16(n + 2) <= 19 &&
            rd16(n + 31) == 8) {
            for (unsigned j = 0; j < count; j++) {
                const uint8_t *output = nodes + j * NODE_SIZE;
                if (output[4] == 0 && rd16(output) == rd16(n) &&
                    rd16(output + 2) == rd16(n + 2)) {
                    wr16(n + 31, 7);
                    break;
                }
            }
        }
    }
    uint8_t *initializers = image_records(image, size, INIT_SECTION, INIT_SIZE, &count);
    for (unsigned i = 0; i < count;) {
        uint8_t *r = initializers + i * INIT_SIZE;
        /* This GAL20V8 product-term-disable range is inside the GAL24V10
         * AND array. Removing it leaves unused rows at constant false. */
        if (rd32(r) == 2640 && rd32(r + 4) == 2703 && r[8] == 1) {
            memmove(r, r + INIT_SIZE, (count - i - 1) * INIT_SIZE);
            wr16(image + INIT_SECTION * DL_SECTION_HEADER_SIZE + 4, --count);
        } else {
            i++;
        }
    }
}

static void correct_buried_d_polarity(uint8_t *image, size_t size)
{
    unsigned count;
    const uint8_t *attrs = image_records(image, size, 3, 152, &count);
    if (count != 1) {
        return;
    }
    unsigned npins = attrs[8];
    uint8_t *nodes = image_records(image, size, 7, 39, &count);
    for (unsigned i = 0; i < count; i++) {
        uint8_t *n = nodes + 39 * i;
        /* Buried D registers have fixed positive polarity, not the
         * polarity-neutral behavior used for control terms. */
        if (rd16(n + 2) > npins && n[4] == 1 && n[5] == 4) {
            n[5] = 2;
        }
    }
}

static void correct_atf750_alias_columns(uint8_t *image, size_t size)
{
    unsigned count, pin_count;
    const uint8_t *attrs = image_records(image, size, 3, 152, &count);
    if (count != 1) {
        return;
    }
    unsigned npins = attrs[8];
    const uint8_t *nodes = image_records(image, size, 7, 39, &count);
    uint8_t *pins = image_records(image, size, 6, 16, &pin_count);
    for (unsigned i = 0; i < count; i++) {
        const uint8_t *alias = nodes + 39 * i;
        if (rd16(alias + 2) <= npins || alias[4] != 1) {
            continue;
        }
        for (unsigned j = 0; j < count; j++) {
            const uint8_t *parent = nodes + 39 * j;
            if (rd16(parent) != rd16(alias) || rd16(parent + 2) > npins ||
                parent[4] != 1 || parent[5] != 7) {
                continue;
            }
            for (unsigned k = 0; k < pin_count; k++) {
                const uint8_t *source = pins + 16 * k;
                if (rd16(source + 2) != rd16(parent + 2) || source[4] != 0 || source[6] != 1) {
                    continue;
                }
                /* A register alias has fixed polarity. Derive its columns
                 * from the positive physical output's register feedback. */
                unsigned flip = (alias[6] != 0) != (parent[6] != 0);
                for (unsigned p = 0; p < pin_count; p++) {
                    uint8_t *target = pins + 16 * p;
                    if (rd16(target + 2) == rd16(alias + 2) && target[4] == 0 &&
                        target[5] == 0x17 && target[6] == 0) {
                        target[7] = source[7 + flip];
                        target[8] = source[8 - flip];
                    }
                }
            }
        }
    }
}

static void correct_pal26_feedback(uint8_t *image, size_t size)
{
    unsigned count;
    uint8_t *pins = image_records(image, size, 6, 16, &count);
    for (unsigned i = 0; i < count; i++) {
        uint8_t *io = pins + 16 * i;
        if (io[4] != 0x25 || rd16(io + 2) < 15 || rd16(io + 2) > 20) {
            continue;
        }
        /* IO and DFB select the same macrocell feedback mux. The IO
         * records for these six pins point ten fuses before that mux. */
        for (unsigned j = 0; j < count; j++) {
            const uint8_t *dfb = pins + 16 * j;
            if (dfb[4] == 0x22 && rd16(io + 2) == rd16(dfb + 2) &&
                rd32(io + 12) + 10 == rd32(dfb + 12)) {
                wr32(io + 12, rd32(dfb + 12));
                break;
            }
        }
    }
}

static void correct_5000_latch_selector(uint8_t *image, size_t size)
{
    unsigned count;
    uint8_t *pins = image_records(image, size, 6, 16, &count);
    for (unsigned i = 0; i < count; i++) {
        uint8_t *iol = pins + 16 * i;
        if (rd16(iol + 2) != 25 || iol[4] != 0x34) {
            continue;
        }
        /* IOL and LQ select the same input latch. The pin 25 IOL record
         * instead names the sum-C join fuse of a different macrocell. */
        for (unsigned j = 0; j < count; j++) {
            const uint8_t *lq = pins + 16 * j;
            if (lq[4] == 0x18 && rd16(lq) == rd16(iol) &&
                rd16(lq + 2) == rd16(iol + 2) && lq[11] == iol[11]) {
                wr32(iol + 12, rd32(lq + 12));
                break;
            }
        }
    }
}

static void correct_peel_output_enable(uint8_t *image, size_t size)
{
    unsigned count;
    uint8_t *nodes = image_records(image, size, 7, 39, &count);
    for (unsigned i = 0; i < count; i++) {
        uint8_t *n = nodes + 39 * i;
        /* Generate a constant-true OE term only when no equation supplies
         * it. Explicit output-enable equations keep their own value. */
        if (n[4] == 2 && n[34] == 0) {
            n[34] = 2;
        }
    }
}

static void correct_ecl_reset_polarity(uint8_t *image, size_t size)
{
    unsigned count;
    uint8_t *pins = image_records(image, size, 6, 16, &count);
    for (unsigned i = 0; i < count; i++) {
        uint8_t *p = pins + 16 * i;
        /* LCC pin 2 is the same active-high master reset as DIP pin 1. */
        if (rd16(p + 2) == 2 && p[5] == 0x12) {
            p[5] = 0x18;
        }
    }
    uint8_t *links = image_records(image, size, 10, 13, &count);
    for (unsigned i = 0; i < count; i++) {
        uint8_t *c = links + 13 * i;
        if (rd16(c + 2) == 2 && rd16(c + 5) == 0x1a) {
            wr16(c + 5, 0x2f);
        }
    }
}

size_t dl_read_device_image(FILE *fp, const uint8_t *entry, uint8_t *image, size_t size)
{
    size_t got = dl_read_image(fp, image, size);
    char name[DL_ENTRY_NAME_SIZE + 1];
    memcpy(name, entry, DL_ENTRY_NAME_SIZE);
    name[DL_ENTRY_NAME_SIZE] = '\0';
    if (strcasecmp(name, "v2500b") == 0 || strcasecmp(name, "v2500blcc") == 0 ||
        strcasecmp(name, "v2500c") == 0 || strcasecmp(name, "v2500clcc") == 0) {
        correct_2500_buried_functions(image, got);
    }
    if (strncasecmp(name, "v750c", 5) == 0) {
        correct_atf750_feedback(image, got);
        correct_atf750_alias_columns(image, got);
    }
    if (strcasecmp(name, "g16vp8ms") == 0) {
        correct_vp8_enable_pin(image, got);
    }
    if (strcasecmp(name, "g16v8hslcc") == 0) {
        correct_v8h_simple_input(image, got);
    }
    if (strcasecmp(name, "g16v8hma") == 0 || strcasecmp(name, "g16v8hmalcc") == 0) {
        correct_v8h_complex_mode(image, got);
    }
    if (strcasecmp(name, "g24v10ms") == 0) {
        correct_gal24v10_rows(image, got);
    } else if (strcasecmp(name, "f18v8z") == 0 || strcasecmp(name, "f18v8zd") == 0 ||
               strcasecmp(name, "f18v8zs") == 0) {
        correct_peel_output_enable(image, got);
    } else if (strcasecmp(name, "v5000") == 0) {
        correct_5000_latch_selector(image, got);
    } else if (strcasecmp(name, "p26v12") == 0) {
        correct_pal26_feedback(image, got);
    }
    if (strncasecmp(name, "v750", 4) == 0 || strcasecmp(name, "v2500lcc") == 0) {
        correct_buried_d_polarity(image, got);
    }
    if (strcasecmp(name, "p1016rd4lcc") == 0 || strcasecmp(name, "p1016rd8lcc") == 0) {
        correct_ecl_reset_polarity(image, got);
    }
    return got;
}
