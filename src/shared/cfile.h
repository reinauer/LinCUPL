/*
 * cfile.h - the files the compiler phases hand on to each other, and the
 * absolute file for the simulator.
 *
 * parse writes CB<pid>, expand reads it and writes CM<pid>, minimize reads
 * CM<pid> and writes CC<pid>, fusemap reads CC<pid> and writes
 * <design>.abs, which csim reads.  The files hold one section per design:
 *
 *   header    CF_HEADER_SIZE bytes (fields below), followed by
 *             cf_header_extra() bytes of header strings
 *   symbols   one record of CF_SYMBOL_SIZE bytes per symbol; the records of
 *             the CB file are CF_CB_SYMBOL_SIZE bytes, they end before the
 *             pin index
 *   CB        the equations as token records (CF_TOKEN_SIZE bytes)
 *   others    frames: a frame header of CF_FRAME_SIZE bytes and its rows.
 *             A row describes a product term with three bit planes of
 *             cf_plane_words() 16-bit words each: the outputs the term
 *             drives, the variables it uses and the variables that are true.
 *             Bit i of a plane is bit (i & 15) of word 1 + (i >> 4); word 0
 *             holds flags.  A frame header with CF_FRAME_END_OF_DESIGN or
 *             CF_FRAME_END_OF_FILE ends a section or the file.
 *
 * Numbers are little-endian.  The phases keep their own structures; the
 * functions here convert records to and from them and do the file I/O.
 * The readers and writers return the result of the transfer and leave the
 * error handling to the caller.
 */
#ifndef LINCUPL_CFILE_H
#define LINCUPL_CFILE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "le.h"

#define CF_MAGIC_CB 0xd432
#define CF_MAGIC_CM 0x2fbe          /* CM and CC */
#define CF_MAGIC_ABS 0x5b25
#define CF_ABS_VERSION 5

/* Header */
#define CF_HEADER_SIZE 0x83
#define CF_HDR_MAGIC 0x00           /* u16 */
#define CF_HDR_VERSION 0x02         /* u16: CF_ABS_VERSION in the .abs file */
#define CF_HDR_SYMBOLS 0x04         /* u16: symbol records */
#define CF_HDR_FORMAT 0x08          /* ':' if no minimization was asked for, else
                                     * '.'; then the FORMAT text */
#define CF_HDR_DESIGN 0x48          /* design name */
#define CF_HDR_DEVICE 0x65          /* the device's directory entry in the
                                     * library (DL_ENTRY_SIZE bytes) */
#define CF_HDR_EXTRA 0x81           /* u16: size of the header strings */

/* Symbol record */
#define CF_SYMBOL_SIZE 0x40
#define CF_CB_SYMBOL_SIZE 0x34
#define CF_NAME_SIZE 0x20
#define CF_EXT_SIZE 10
#define CF_SYM_NAME 0x00
#define CF_SYM_EXT 0x20
#define CF_SYM_PIN 0x2a             /* u16 */
#define CF_SYM_TYPE 0x2c
#define CF_SYM_FLAGS 0x2d
#define CF_SYM_ATTR 0x2e            /* 4 bytes */
#define CF_SYM_POLARITY 0x32
#define CF_SYM_ATTR2 0x33
#define CF_SYM_PIN_INDEX 0x34       /* s16 */
#define CF_SYM_NODE_INDEX 0x36      /* s16 */
#define CF_SYM_OR_INDEX 0x38        /* s16 */
#define CF_SYM_EXT_CODE 0x3a        /* s16 */
#define CF_SYM_TERMS 0x3c           /* u32 */

typedef struct {
    char name[CF_NAME_SIZE];
    char ext[CF_EXT_SIZE];          /* extension */
    uint16_t pin;                   /* pin or node number */
    uint8_t type;
    uint8_t flags;
    uint8_t attr[4];
    uint8_t polarity;               /* DeMorgan level */
    uint8_t attr2;
    int16_t pin_index;              /* record in the device's pin section */
    int16_t node_index;             /* record in the device's node section */
    int16_t or_index;               /* node record of the OR array */
    int16_t ext_code;               /* extension number */
    uint32_t terms;                 /* .abs: non-zero if the symbol has terms */
} cf_symbol_t;

/* Token record of the CB file */
#define CF_TOKEN_SIZE 6

typedef struct {
    uint8_t type;
    uint8_t flags;
    uint32_t value;
} cf_token_t;

/* Frame header */
#define CF_FRAME_SIZE 12

enum {
    CF_FRAME_END_OF_FILE = 0x10,
    CF_FRAME_END_OF_DESIGN = 0x20,
    CF_FRAME_FIELD = 0x80           /* .abs: members of a FIELD */
};

typedef struct {
    uint16_t flags;                 /* +0x00: minimization level in bits 0-2,
                                     * CF_FRAME_xxx */
    uint16_t or_array;              /* +0x02 */
    uint16_t nvars;                 /* +0x04: symbols covered by a plane */
    uint16_t nrows;                 /* +0x06: product terms */
    uint16_t has_xor;               /* +0x08 */
    uint16_t max_terms;             /* +0x0a */
} cf_frame_t;

/* 16-bit words per bit plane of a frame over nvars symbols */
static inline int cf_plane_words(int nvars)
{
    return ((nvars - 1) >> 4) + 2;
}

static inline unsigned cf_header_extra(const uint8_t *header)
{
    return rd16(header + CF_HDR_EXTRA);
}

/* All return 1 on success, 0 on a short read or write */
int cf_read_header(FILE *fp, uint8_t *header);
int cf_write_header(FILE *fp, const uint8_t *header);

void cf_get_symbol(const uint8_t *rec, cf_symbol_t *sym);
void cf_put_symbol(uint8_t *rec, const cf_symbol_t *sym);
/* reads a record of `size` bytes, CF_SYMBOL_SIZE or CF_CB_SYMBOL_SIZE;
 * the fields behind a short record are 0 */
int cf_read_symbol(FILE *fp, cf_symbol_t *sym, size_t size);
int cf_write_symbol(FILE *fp, const cf_symbol_t *sym);

int cf_read_token(FILE *fp, cf_token_t *tok);
int cf_write_token(FILE *fp, const cf_token_t *tok);

int cf_read_frame(FILE *fp, cf_frame_t *frame);
int cf_write_frame(FILE *fp, const cf_frame_t *frame);

/* The rows of a frame as `count` 16-bit words; both return the number of
 * words transferred, which is less than `count` if the file ends or a
 * write fails */
size_t cf_read_words(FILE *fp, uint16_t *words, size_t count);
size_t cf_write_words(FILE *fp, const uint16_t *words, size_t count);

#endif /* LINCUPL_CFILE_H */
