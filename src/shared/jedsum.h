/*
 * jedsum.h - JEDEC file checksums
 *
 * The fuse checksum (the C field) is the 16-bit sum of the fuse map taken
 * as 8-bit bytes, the lowest-numbered fuse of each byte in bit 0.
 *
 * The transmission checksum after the ETX is the 16-bit sum of every
 * character from the STX to the ETX, both included, taken as 7 bits; a line
 * end counts as CR LF.
 */
#ifndef LINCUPL_JEDSUM_H
#define LINCUPL_JEDSUM_H

#ifdef __cplusplus
extern "C" {
#endif

/* Fuse checksum being computed; fuses are added in fuse number order */
typedef struct {
    unsigned sum;
    unsigned byte;      /* the byte being collected */
    unsigned bits;      /* fuses in it */
} jedsum_fuses_t;

void jedsum_fuses_init(jedsum_fuses_t *fs);
void jedsum_fuses_add(jedsum_fuses_t *fs, int blown);
/* Add a partly filled last byte (as if padded with fuses that are not
 * blown) */
void jedsum_fuses_flush(jedsum_fuses_t *fs);
/* The fuse checksum, 0..0xffff */
unsigned jedsum_fuses_value(const jedsum_fuses_t *fs);

/* Transmission checksum sum with character c / every character of text
 * added, 0..0xffff */
unsigned jedsum_char(unsigned sum, int c);
unsigned jedsum_text(unsigned sum, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* LINCUPL_JEDSUM_H */
