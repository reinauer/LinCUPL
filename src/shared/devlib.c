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
