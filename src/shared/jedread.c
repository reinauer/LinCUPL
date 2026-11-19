/*
 * jedread.c - reading the fuse map of a JEDEC file (see jedread.h)
 */
#include "jedread.h"
#include "jedsum.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define JED_STX 2
#define JED_ETX 3
#define JED_CTRL_Z 0x1a
#define JED_MAX_FILE (64UL * 1024 * 1024)
#define FUSE_UNSET 2                /* no L field has listed the fuse */

/* The map while it is read: it grows with the fuses listed */
typedef struct {
    unsigned char *state;           /* 0, 1 or FUSE_UNSET */
    unsigned long size;             /* allocated states */
    unsigned long used;             /* highest fuse listed + 1 */
} fuse_list_t;

static int jed_fail(char *err, size_t err_size, const char *fmt, ...)
{
    if (err != NULL && err_size != 0) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, err_size, fmt, ap);
        va_end(ap);
    }
    return 0;
}

static int is_blank(int c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v' || c == 0;
}

static int upper(int c)
{
    return (c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c;
}

/* The whole file in memory, NUL-terminated; NULL if it cannot be read */
static char *read_all(FILE *fp, size_t *len_out)
{
    size_t size = 0x4000, len = 0;
    char *text = malloc(size);
    while (text != NULL) {
        size_t got = fread(text + len, 1, size - len - 1, fp);
        len += got;
        if (got == 0) {
            break;
        }
        if (len + 1 >= size) {
            char *bigger;
            if (size >= JED_MAX_FILE) {
                free(text);
                return NULL;
            }
            size *= 2;
            bigger = realloc(text, size);
            if (bigger == NULL) {
                free(text);
                return NULL;
            }
            text = bigger;
        }
    }
    if (text != NULL) {
        if (ferror(fp)) {
            free(text);
            return NULL;
        }
        text[len] = '\0';
        *len_out = len;
    }
    return text;
}

/* Set the state of one fuse; 0 if memory runs out */
static int set_fuse(fuse_list_t *list, unsigned long fuse, int state)
{
    if (fuse >= list->size) {
        unsigned long size = list->size != 0 ? list->size : 0x2000;
        unsigned char *bigger;
        while (size <= fuse) {
            size *= 2;
        }
        bigger = realloc(list->state, size);
        if (bigger == NULL) {
            return 0;
        }
        memset(bigger + list->size, FUSE_UNSET, size - list->size);
        list->state = bigger;
        list->size = size;
    }
    list->state[fuse] = (unsigned char)state;
    if (fuse >= list->used) {
        list->used = fuse + 1;
    }
    return 1;
}

/* A decimal number at *p, after blanks; -1 if there is none or it is too
 * large for a fuse number */
static long read_number(const char **p, const char *end)
{
    const char *s = *p;
    unsigned long value = 0;
    int digits = 0;
    while (s < end && is_blank((unsigned char)*s)) {
        s++;
    }
    for (; s < end && *s >= '0' && *s <= '9'; s++, digits++) {
        if (value > JED_MAX_FUSES) {
            return -1;
        }
        value = value * 10 + (unsigned long)(*s - '0');
    }
    *p = s;
    return (digits == 0 || value > JED_MAX_FUSES) ? -1 : (long)value;
}

/* The L field "<fuse> <states>" */
static int read_fuse_list(const char *p, const char *end, fuse_list_t *list, char *err, size_t err_size)
{
    long fuse = read_number(&p, end);
    if (fuse < 0) {
        return jed_fail(err, err_size, "an L field has no valid fuse number");
    }
    for (; p < end; p++) {
        if (*p == '0' || *p == '1') {
            if ((unsigned long)fuse >= JED_MAX_FUSES) {
                return jed_fail(err, err_size, "an L field lists more than %lu fuses", JED_MAX_FUSES);
            }
            if (!set_fuse(list, (unsigned long)fuse, *p - '0')) {
                return jed_fail(err, err_size, "out of memory");
            }
            fuse++;
        } else if (!is_blank((unsigned char)*p)) {
            return jed_fail(err, err_size, "invalid character '%c' in the L field at fuse %ld",
                            (*p >= 0x20 && *p < 0x7f) ? *p : '?', fuse);
        }
    }
    return 1;
}

static unsigned fuse_checksum(const unsigned char *fuses, unsigned long nfuses)
{
    jedsum_fuses_t sum;
    jedsum_fuses_init(&sum);
    for (unsigned long i = 0; i < nfuses; i++) {
        jedsum_fuses_add(&sum, fuses[i]);
    }
    jedsum_fuses_flush(&sum);
    return jedsum_fuses_value(&sum);
}

void jed_free(jed_map_t *map)
{
    if (map != NULL) {
        free(map->fuses);
        free(map->design);
        memset(map, 0, sizeof(*map));
    }
}

int jed_read(FILE *fp, jed_map_t *map, char *err, size_t err_size)
{
    fuse_list_t list = { NULL, 0, 0 };
    size_t len = 0;
    char *text;
    const char *start, *end, *p;
    int default_state = -1;
    unsigned file_checksum = 0;
    int first = 1;
    int ok = 1;

    memset(map, 0, sizeof(*map));
    text = read_all(fp, &len);
    if (text == NULL) {
        return jed_fail(err, err_size, "the file cannot be read");
    }
    /* the fields stand between STX and ETX; Ctrl-Z ends a file without ETX */
    start = text;
    end = text + len;
    for (p = text; p < end; p++) {
        if (*p == JED_STX) {
            start = p + 1;
            map->has_stx = 1;
            break;
        }
    }
    for (p = start; p < end; p++) {
        if (*p == JED_ETX) {
            map->has_etx = 1;
            end = p;
        } else if (*p == JED_CTRL_Z) {
            end = p;
        }
    }

    for (p = start; ok && p < end; ) {
        const char *field = p;
        const char *stop = memchr(p, '*', (size_t)(end - p));
        if (stop == NULL) {
            stop = end;
        }
        p = stop < end ? stop + 1 : end;
        if (first) {
            /* the design specification */
            size_t n = (size_t)(stop - field);
            size_t out = 0;
            first = 0;
            map->design = malloc(n + 1);
            if (map->design == NULL) {
                ok = jed_fail(err, err_size, "out of memory");
                break;
            }
            for (size_t i = 0; i < n; i++) {
                if (field[i] != '\r' && field[i] != '\0') {
                    map->design[out++] = field[i];
                }
            }
            map->design[out] = '\0';
            continue;
        }
        while (field < stop && is_blank((unsigned char)*field)) {
            field++;
        }
        if (field == stop) {
            continue;
        }
        switch (upper((unsigned char)*field)) {
        case 'Q': {
            int which = field + 1 < stop ? upper((unsigned char)field[1]) : 0;
            const char *q = field + 2;
            long value;
            if (which != 'F' && which != 'P') {
                break;
            }
            value = read_number(&q, stop);
            if (value < 0) {
                ok = jed_fail(err, err_size, "the Q%c field has no valid number", which);
            } else if (which == 'F') {
                map->qf = (unsigned long)value;
            } else {
                map->pins = (unsigned)value;
            }
            break;
        }
        case 'F': {
            const char *q = field + 1;
            while (q < stop && is_blank((unsigned char)*q)) {
                q++;
            }
            if (q < stop && (*q == '0' || *q == '1')) {
                default_state = *q - '0';
            } else {
                ok = jed_fail(err, err_size, "the F field is neither 0 nor 1");
            }
            break;
        }
        case 'L':
            ok = read_fuse_list(field + 1, stop, &list, err, err_size);
            break;
        case 'C': {
            const char *q = field + 1;
            int digits = 0;
            unsigned value = 0;
            while (q < stop && is_blank((unsigned char)*q)) {
                q++;
            }
            for (; q < stop; q++, digits++) {
                int c = upper((unsigned char)*q);
                if (c >= '0' && c <= '9') {
                    value = value * 16 + (unsigned)(c - '0');
                } else if (c >= 'A' && c <= 'F') {
                    value = value * 16 + (unsigned)(c - 'A' + 10);
                } else {
                    break;
                }
            }
            if (digits == 0 || digits > 8) {
                ok = jed_fail(err, err_size, "the C field is no hexadecimal number");
            } else {
                file_checksum = value & 0xffff;
                map->has_checksum = 1;
            }
            break;
        }
        default:
            /* notes, security fuse, test vectors, ...: not needed */
            break;
        }
    }
    free(text);

    if (ok && !map->has_etx && !map->has_checksum) {
        ok = jed_fail(err, err_size, "it ends without ETX and without a fuse checksum (C field), so it is cut short");
    }
    if (ok) {
        map->nfuses = map->qf != 0 ? map->qf : list.used;
        if (map->nfuses == 0) {
            ok = jed_fail(err, err_size, "it has no fuses (no QF and no L field)");
        } else if (list.used > map->nfuses) {
            ok = jed_fail(err, err_size, "fuse %lu is listed, but QF says there are %lu fuses", list.used - 1, map->nfuses);
        }
    }
    if (ok) {
        map->fuses = malloc(map->nfuses);
        if (map->fuses == NULL) {
            ok = jed_fail(err, err_size, "out of memory");
        }
    }
    for (unsigned long i = 0; ok && i < map->nfuses; i++) {
        int state = i < list.used ? list.state[i] : FUSE_UNSET;
        if (state == FUSE_UNSET) {
            if (default_state < 0) {
                ok = jed_fail(err, err_size, "fuse %lu has no state: no L field lists it and there is no F field", i);
                break;
            }
            state = default_state;
        }
        map->fuses[i] = (unsigned char)state;
    }
    if (ok) {
        map->checksum = fuse_checksum(map->fuses, map->nfuses);
        if (map->has_checksum && map->checksum != file_checksum) {
            ok = jed_fail(err, err_size, "the fuse checksum is %04X, but the C field says %04X", map->checksum,
                          file_checksum);
        }
    }
    free(list.state);
    if (!ok) {
        jed_free(map);
    }
    return ok;
}
