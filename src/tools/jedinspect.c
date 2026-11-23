/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, LinCUPL team
 *
 * Read-only inspection of JEDEC fuse maps using the shared reader.
 */
#include "platform.h"
#include "jedread.h"
#include "locate.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *out)
{
    fprintf(out, "Usage: jedinspect [--json] [--fuses START:COUNT]\n"
                 "                  [--diff OTHER] [--limit COUNT] FILE\n"
                 "Show fuse counts, checksums, framing and design text.\n"
                 "--diff compares fuse states, ignoring text and record order.\n"
                 "--limit bounds listed differences (default 64; 0: summary only).\n"
                 "The transmission checksum and non-fuse records are not checked.\n"
                 "Exit status: 0 success/equal, 1 different fuses, 2 error.\n");
}

static int number(const char *text, unsigned long *out, const char **end)
{
    char *stop;
    if (*text < '0' || *text > '9')
        return 0;
    errno = 0;
    *out = strtoul(text, &stop, 10);
    if (errno || *out > JED_MAX_FUSES)
        return 0;
    *end = stop;
    return 1;
}

static int read_map(const char *path, jed_map_t *map)
{
    char error[256];
    FILE *fp = lincupl_fopen_ci(path, "rb");
    int ok;
    if (!fp) {
        fprintf(stderr, "jedinspect: %s: %s\n", path, strerror(errno));
        return 0;
    }
    ok = jed_read(fp, map, error, sizeof(error));
    fclose(fp);
    if (!ok)
        fprintf(stderr, "jedinspect: %s: %s\n", path, error);
    return ok;
}

/* Escape bytes outside ASCII too: design text need not be UTF-8. */
static void json_string(const char *text)
{
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p == '"' || *p == '\\')
            printf("\\%c", *p);
        else if (*p < 32 || *p >= 127)
            printf("\\u%04x", *p);
        else
            putchar(*p);
    }
    putchar('"');
}

static int state(const jed_map_t *map, unsigned long i)
{
    return i < map->nfuses ? map->fuses[i] : -1;
}

static void design_text(const char *text)
{
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if ((*p < 32 && *p != '\n' && *p != '\t') || *p >= 127)
            printf("\\x%02x", *p);
        else
            putchar(*p);
    }
    putchar('\n');
}

int main(int argc, char **argv)
{
    lincupl_init_file_mode();
    const char *path = NULL, *other = NULL;
    unsigned long first = 0, count = 0, limit = 64, ones = 0, differences = 0;
    int json = 0, range = 0, options = 1, result = 2;
    jed_map_t map = {0}, rhs = {0};

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i], *end;
        if (options && strcmp(arg, "--") == 0) {
            options = 0;
        } else if (options && strcmp(arg, "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (options && strcmp(arg, "--json") == 0) {
            json = 1;
        } else if (options && strcmp(arg, "--diff") == 0 && i + 1 < argc && !other) {
            other = argv[++i];
        } else if (options && strcmp(arg, "--limit") == 0 && i + 1 < argc) {
            if (!number(argv[++i], &limit, &end) || *end)
                goto bad_usage;
        } else if (options && strcmp(arg, "--fuses") == 0 && i + 1 < argc && !range) {
            if (!number(argv[++i], &first, &end) || *end != ':' ||
                !number(end + 1, &count, &end) || *end || !count)
                goto bad_usage;
            range = 1;
        } else if ((options && arg[0] == '-') || path) {
            goto bad_usage;
        } else {
            path = arg;
        }
    }
    if (!path)
        goto bad_usage;
    if (!read_map(path, &map) || (other && !read_map(other, &rhs)))
        goto done;
    if (range && (first >= map.nfuses || count > map.nfuses - first)) {
        fprintf(stderr, "jedinspect: fuse range exceeds %lu fuses\n", map.nfuses);
        goto done;
    }
    for (unsigned long i = 0; i < map.nfuses; i++)
        ones += map.fuses[i];
    if (json) {
        printf("{\"file\":");
        json_string(path);
        printf(",\"fuses\":%lu,\"pins\":%u,\"zeros\":%lu,\"ones\":%lu,"
               "\"checksum\":\"%04X\",\"checksum_present\":%s,"
               "\"stx\":%s,\"etx\":%s,\"transmission_checksum_checked\":false,\"design\":",
               map.nfuses, map.pins, map.nfuses - ones, ones, map.checksum,
               map.has_checksum ? "true" : "false", map.has_stx ? "true" : "false",
               map.has_etx ? "true" : "false");
        json_string(map.design ? map.design : "");
    } else {
        printf("File: %s\nFuses: %lu (%lu zero, %lu one)\nPins (QP): %u\n"
               "Fuse checksum: %04X (%s)\nFraming: STX %s, ETX %s\n"
               "Transmission checksum: not checked\nDesign: ",
               path, map.nfuses, map.nfuses - ones, ones, map.pins, map.checksum,
               map.has_checksum ? "verified" : "computed; no C field",
               map.has_stx ? "present" : "absent", map.has_etx ? "present" : "absent");
        design_text(map.design ? map.design : "");
    }
    if (range) {
        if (json)
            printf(",\"range\":{\"start\":%lu,\"count\":%lu,\"bits\":\"", first, count);
        for (unsigned long i = 0; i < count; i++) {
            if (!json && i % 64 == 0)
                printf("L%06lu ", first + i);
            putchar('0' + map.fuses[first + i]);
            if (!json && (i % 64 == 63 || i + 1 == count))
                putchar('\n');
        }
        if (json)
            printf("\"}");
    }
    if (other) {
        unsigned long length = map.nfuses > rhs.nfuses ? map.nfuses : rhs.nfuses;
        if (json) {
            printf(",\"comparison\":{\"file\":");
            json_string(other);
            printf(",\"fuses\":%lu,\"differences\":[", rhs.nfuses);
        } else {
            printf("Compare: %s (%lu fuses)\n", other, rhs.nfuses);
        }
        for (unsigned long i = 0; i < length; i++) {
            int a = state(&map, i), b = state(&rhs, i);
            if (a == b)
                continue;
            if (differences < limit) {
                if (json) {
                    printf("%s{\"fuse\":%lu,\"left\":", differences ? "," : "", i);
                    if (a < 0) printf("null"); else printf("%d", a);
                    printf(",\"right\":");
                    if (b < 0) printf("null"); else printf("%d", b);
                    printf("}");
                } else {
                    printf("%lu: %c -> %c\n", i, a < 0 ? '-' : '0' + a, b < 0 ? '-' : '0' + b);
                }
            }
            differences++;
        }
        if (json)
            printf("],\"count\":%lu,\"truncated\":%s}", differences, differences > limit ? "true" : "false");
        else
            printf("Different fuses: %lu%s\n", differences, differences > limit ? " (listing limited)" : "");
    }
    if (json)
        printf("}\n");
    result = differences ? 1 : 0;
    if (fflush(stdout) == EOF) {
        fprintf(stderr, "jedinspect: output error\n");
        result = 2;
    }
done:
    jed_free(&map);
    jed_free(&rhs);
    return result;
bad_usage:
    usage(stderr);
    return 2;
}
