/*
 * cfile.c - intermediate files of the compiler phases and the absolute
 * file, see cfile.h.
 */
#include <string.h>
#include "cfile.h"

#define WORD_BLOCK 256              /* words converted at a time */

int cf_read_header(FILE *fp, uint8_t *header)
{
    return fread(header, CF_HEADER_SIZE, 1, fp) == 1;
}

int cf_write_header(FILE *fp, const uint8_t *header)
{
    return fwrite(header, CF_HEADER_SIZE, 1, fp) == 1;
}

void cf_get_symbol(const uint8_t *rec, cf_symbol_t *sym)
{
    memcpy(sym->name, rec + CF_SYM_NAME, CF_NAME_SIZE);
    memcpy(sym->ext, rec + CF_SYM_EXT, CF_EXT_SIZE);
    sym->pin = (uint16_t)rd16(rec + CF_SYM_PIN);
    sym->type = rec[CF_SYM_TYPE];
    sym->flags = rec[CF_SYM_FLAGS];
    memcpy(sym->attr, rec + CF_SYM_ATTR, sizeof(sym->attr));
    sym->polarity = rec[CF_SYM_POLARITY];
    sym->attr2 = rec[CF_SYM_ATTR2];
    sym->pin_index = (int16_t)rd16s(rec + CF_SYM_PIN_INDEX);
    sym->node_index = (int16_t)rd16s(rec + CF_SYM_NODE_INDEX);
    sym->or_index = (int16_t)rd16s(rec + CF_SYM_OR_INDEX);
    sym->ext_code = (int16_t)rd16s(rec + CF_SYM_EXT_CODE);
    sym->terms = rd32(rec + CF_SYM_TERMS);
}

void cf_put_symbol(uint8_t *rec, const cf_symbol_t *sym)
{
    memcpy(rec + CF_SYM_NAME, sym->name, CF_NAME_SIZE);
    memcpy(rec + CF_SYM_EXT, sym->ext, CF_EXT_SIZE);
    wr16(rec + CF_SYM_PIN, sym->pin);
    rec[CF_SYM_TYPE] = sym->type;
    rec[CF_SYM_FLAGS] = sym->flags;
    memcpy(rec + CF_SYM_ATTR, sym->attr, sizeof(sym->attr));
    rec[CF_SYM_POLARITY] = sym->polarity;
    rec[CF_SYM_ATTR2] = sym->attr2;
    wr16(rec + CF_SYM_PIN_INDEX, (uint16_t)sym->pin_index);
    wr16(rec + CF_SYM_NODE_INDEX, (uint16_t)sym->node_index);
    wr16(rec + CF_SYM_OR_INDEX, (uint16_t)sym->or_index);
    wr16(rec + CF_SYM_EXT_CODE, (uint16_t)sym->ext_code);
    wr32(rec + CF_SYM_TERMS, sym->terms);
}

int cf_read_symbol(FILE *fp, cf_symbol_t *sym, size_t size)
{
    uint8_t rec[CF_SYMBOL_SIZE];
    memset(rec, 0, sizeof(rec));
    if (size > sizeof(rec) || fread(rec, size, 1, fp) != 1) {
        return 0;
    }
    cf_get_symbol(rec, sym);
    return 1;
}

int cf_write_symbol(FILE *fp, const cf_symbol_t *sym)
{
    uint8_t rec[CF_SYMBOL_SIZE];
    cf_put_symbol(rec, sym);
    return fwrite(rec, sizeof(rec), 1, fp) == 1;
}

int cf_read_token(FILE *fp, cf_token_t *tok)
{
    uint8_t rec[CF_TOKEN_SIZE];
    if (fread(rec, sizeof(rec), 1, fp) != 1) {
        return 0;
    }
    tok->type = rec[0];
    tok->flags = rec[1];
    tok->value = rd32(rec + 2);
    return 1;
}

int cf_write_token(FILE *fp, const cf_token_t *tok)
{
    uint8_t rec[CF_TOKEN_SIZE];
    rec[0] = tok->type;
    rec[1] = tok->flags;
    wr32(rec + 2, tok->value);
    return fwrite(rec, sizeof(rec), 1, fp) == 1;
}

int cf_read_frame(FILE *fp, cf_frame_t *frame)
{
    uint8_t rec[CF_FRAME_SIZE];
    if (fread(rec, sizeof(rec), 1, fp) != 1) {
        return 0;
    }
    frame->flags = (uint16_t)rd16(rec);
    frame->or_array = (uint16_t)rd16(rec + 2);
    frame->nvars = (uint16_t)rd16(rec + 4);
    frame->nrows = (uint16_t)rd16(rec + 6);
    frame->has_xor = (uint16_t)rd16(rec + 8);
    frame->max_terms = (uint16_t)rd16(rec + 10);
    return 1;
}

int cf_write_frame(FILE *fp, const cf_frame_t *frame)
{
    uint8_t rec[CF_FRAME_SIZE];
    wr16(rec, frame->flags);
    wr16(rec + 2, frame->or_array);
    wr16(rec + 4, frame->nvars);
    wr16(rec + 6, frame->nrows);
    wr16(rec + 8, frame->has_xor);
    wr16(rec + 10, frame->max_terms);
    return fwrite(rec, sizeof(rec), 1, fp) == 1;
}

size_t cf_read_words(FILE *fp, uint16_t *words, size_t count)
{
    uint8_t buf[2 * WORD_BLOCK];
    size_t done = 0;
    while (done < count) {
        size_t chunk = count - done < WORD_BLOCK ? count - done : WORD_BLOCK;
        size_t got = fread(buf, 2, chunk, fp);
        for (size_t i = 0; i < got; i++) {
            words[done + i] = (uint16_t)rd16(buf + 2 * i);
        }
        done += got;
        if (got < chunk) {
            break;
        }
    }
    return done;
}

size_t cf_write_words(FILE *fp, const uint16_t *words, size_t count)
{
    uint8_t buf[2 * WORD_BLOCK];
    size_t done = 0;
    while (done < count) {
        size_t chunk = count - done < WORD_BLOCK ? count - done : WORD_BLOCK;
        for (size_t i = 0; i < chunk; i++) {
            wr16(buf + 2 * i, words[done + i]);
        }
        size_t put = fwrite(buf, 2, chunk, fp);
        done += put;
        if (put < chunk) {
            break;
        }
    }
    return done;
}
