/* SPDX-License-Identifier: BSD-2-Clause */
/* Expose the real normalized reader to device-compiler regression tests. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "devlib.h"

int main(int argc, char **argv)
{
    uint8_t header[DL_HEADER_SIZE], entry[DL_ENTRY_SIZE];
    int by_index = argc == 5 && strcmp(argv[2], "--index") == 0;
    if (argc != 4 && !by_index)
        return 2;
    FILE *input = fopen(argv[1], "rb");
    if (input == NULL)
        return 1;
    int found = 0;
    if (dl_read_header(input, header) == DL_OK) {
        if (by_index) {
            char *end;
            long index = strtol(argv[3], &end, 10);
            if (*argv[3] != '\0' && *end == '\0' && index >= 0 && index < DL_MAX_ENTRIES &&
                fseek(input, DL_HEADER_SIZE + index * DL_ENTRY_SIZE, SEEK_SET) == 0 &&
                fread(entry, 1, sizeof(entry), input) == sizeof(entry))
                found = (entry[DL_ENTRY_FLAGS] & 1) != 0;
        } else {
            found = dl_find_device(input, argv[2], DL_MATCH_CASE, entry);
        }
    }
    if (!found || !dl_seek_image(input, entry)) {
        fclose(input);
        return 1;
    }
    size_t size = dl_image_size(entry);
    uint8_t *image = malloc(size);
    if (image == NULL) {
        fclose(input);
        return 1;
    }
    size_t got = dl_read_device_image(input, entry, image, size);
    fclose(input);
    int status = 1;
    if (got == size) {
        FILE *output = fopen(argv[by_index ? 4 : 3], "wb");
        if (output != NULL) {
            int complete = fwrite(image, 1, size, output) == size;
            status = fclose(output) == 0 && complete ? 0 : 1;
        }
    }
    free(image);
    return status;
}
