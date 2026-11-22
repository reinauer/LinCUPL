/*
 * devlib.c - reading the device libraries (.dl), see devlib.h.
 */
#include <string.h>
#include <strings.h>
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

size_t dl_read_device_image(FILE *fp, const uint8_t *entry, uint8_t *image, size_t size)
{
    size_t got = dl_read_image(fp, image, size);
    char name[DL_ENTRY_NAME_SIZE + 1];
    memcpy(name, entry, DL_ENTRY_NAME_SIZE);
    name[DL_ENTRY_NAME_SIZE] = '\0';
    if (strcasecmp(name, "g24v10ms") == 0) {
        correct_gal24v10_rows(image, got);
    }
    return got;
}
