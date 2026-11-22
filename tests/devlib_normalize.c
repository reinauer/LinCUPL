/* Synthetic device records: no installed device library is needed. */
#include "devlib.h"
#include <stdio.h>
#include <string.h>

#define IMAGE_SIZE 512
#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static int read_image(const uint8_t *input, size_t size, const char *device,
                      int raw, uint8_t *output)
{
    uint8_t entry[DL_ENTRY_SIZE] = { 0 };
    memcpy(entry, device, strlen(device));
    FILE *fp = tmpfile();
    CHECK(fp != NULL);
    CHECK(fwrite(input, 1, size, fp) == size);
    rewind(fp);
    size_t got = raw ? dl_read_image(fp, output, size)
                     : dl_read_device_image(fp, entry, output, size);
    fclose(fp);
    CHECK(got == size);
    return 0;
}

static void section(uint8_t *image, unsigned id, unsigned count,
                    unsigned width, unsigned offset)
{
    uint8_t *h = image + id * DL_SECTION_HEADER_SIZE;
    wr16(h + 4, count);
    wr16(h + 6, width);
    wr32(h + 8, offset);
}

static int check_buried_d_polarity(void)
{
    uint8_t input[IMAGE_SIZE] = { 0 }, fixed[IMAGE_SIZE], again[IMAGE_SIZE];
    section(input, 3, 1, 152, 260);
    input[268] = 24;
    section(input, 7, 2, 39, 412);
    wr16(input + 414, 25);
    input[416] = 1;
    input[417] = 4;
    wr16(input + 453, 23);
    input[455] = 1;
    input[456] = 4;
    CHECK(read_image(input, sizeof(input), "v750", 0, fixed) == 0);
    CHECK(fixed[417] == 2);
    CHECK(fixed[456] == 4);
    CHECK(read_image(fixed, sizeof(fixed), "v750", 0, again) == 0);
    CHECK(memcmp(fixed, again, sizeof(fixed)) == 0);
    CHECK(read_image(input, sizeof(input), "v2500", 0, fixed) == 0);
    CHECK(memcmp(input, fixed, sizeof(input)) == 0);
    input[416] = 27; /* T data does not invert when Q is complemented. */
    CHECK(read_image(input, sizeof(input), "v750", 0, fixed) == 0);
    CHECK(memcmp(input, fixed, sizeof(input)) == 0);
    return 0;
}

static int check_pal26_feedback(void)
{
    uint8_t input[IMAGE_SIZE] = { 0 }, fixed[IMAGE_SIZE], again[IMAGE_SIZE];
    section(input, 6, 2, 16, 260);
    wr16(input + 262, 17);
    input[264] = 0x25;
    wr32(input + 272, 7835);
    wr16(input + 278, 17);
    input[280] = 0x22;
    wr32(input + 288, 7845);
    CHECK(read_image(input, sizeof(input), "p26v12", 0, fixed) == 0);
    CHECK(rd32(fixed + 272) == 7845);
    CHECK(read_image(fixed, sizeof(fixed), "p26v12", 0, again) == 0);
    CHECK(memcmp(fixed, again, sizeof(fixed)) == 0);
    CHECK(read_image(input, sizeof(input), "p22v10", 0, fixed) == 0);
    CHECK(memcmp(input, fixed, sizeof(input)) == 0);
    /* No matching DFB pin means there is no authoritative replacement. */
    wr16(input + 278, 18);
    CHECK(read_image(input, sizeof(input), "p26v12", 0, fixed) == 0);
    CHECK(memcmp(input, fixed, sizeof(input)) == 0);
    return 0;
}

static void initializer(uint8_t *record, unsigned first, unsigned last,
                        unsigned value)
{
    wr32(record, first);
    wr32(record + 4, last);
    record[8] = (uint8_t)value;
}

int main(void)
{
    CHECK(check_pal26_feedback() == 0);
    uint8_t original[IMAGE_SIZE] = { 0 }, fixed[IMAGE_SIZE], again[IMAGE_SIZE];
    section(original, 7, 1, 39, 260);
    section(original, 9, 3, 9, 299);
    wr16(original + 262, 19);
    original[264] = 14;
    wr32(original + 270, 2452);
    initializer(original + 299, 100, 110, 1);
    initializer(original + 308, 2640, 2703, 1);
    initializer(original + 317, 3860, 3860, 0);

    CHECK(read_image(original, sizeof(original), "g24v10ms", 0, fixed) == 0);
    CHECK(rd32(fixed + 270) == 2352);
    CHECK(rd16(fixed + 9 * DL_SECTION_HEADER_SIZE + 4) == 2);
    CHECK(memcmp(fixed + 299, original + 299, 9) == 0);
    CHECK(memcmp(fixed + 308, original + 317, 9) == 0);
    CHECK(read_image(fixed, sizeof(fixed), "g24v10ms", 0, again) == 0);
    CHECK(memcmp(fixed, again, sizeof(fixed)) == 0);

    CHECK(read_image(original, sizeof(original), "g24v10ms", 1, again) == 0);
    CHECK(memcmp(original, again, sizeof(original)) == 0);
    CHECK(read_image(original, sizeof(original), "g24v10ma", 0, again) == 0);
    CHECK(memcmp(original, again, sizeof(original)) == 0);

    CHECK(check_buried_d_polarity() == 0);

    /* Cut-off records must not be interpreted or written past the bytes read. */
    memset(again, 0xa5, sizeof(again));
    CHECK(read_image(original, 270, "g24v10ms", 0, again) == 0);
    CHECK(memcmp(original, again, 270) == 0);
    for (size_t i = 270; i < sizeof(again); i++) CHECK(again[i] == 0xa5);
    memset(again, 0xa5, sizeof(again));
    CHECK(read_image(original, 100, "g24v10ms", 0, again) == 0);
    CHECK(memcmp(original, again, 100) == 0);
    for (size_t i = 100; i < sizeof(again); i++) CHECK(again[i] == 0xa5);

    puts("device normalization: passed");
    return 0;
}
