/*
 * jedsum.c - JEDEC file checksums (see jedsum.h)
 */
#include "jedsum.h"

void jedsum_fuses_init(jedsum_fuses_t *fs)
{
    fs->sum = 0;
    fs->byte = 0;
    fs->bits = 0;
}

void jedsum_fuses_add(jedsum_fuses_t *fs, int blown)
{
    if (blown) {
        fs->byte |= 1u << fs->bits;
    }
    if (++fs->bits == 8) {
        jedsum_fuses_flush(fs);
    }
}

void jedsum_fuses_flush(jedsum_fuses_t *fs)
{
    fs->sum = (fs->sum + fs->byte) & 0xffff;
    fs->byte = 0;
    fs->bits = 0;
}

unsigned jedsum_fuses_value(const jedsum_fuses_t *fs)
{
    return fs->sum;
}

unsigned jedsum_char(unsigned sum, int c)
{
    return (sum + (c == '\n' ? 0x0d + 0x0a : (unsigned)c & 0x7f)) & 0xffff;
}

unsigned jedsum_text(unsigned sum, const char *text)
{
    for (; *text != '\0'; text++) {
        sum = jedsum_char(sum, *text);
    }
    return sum;
}
