/*
 * strutil.c - string and file name helpers (see strutil.h)
 */
#include "platform.h"
#include "strutil.h"

#include <stdio.h>
#include <string.h>

size_t lincupl_strcpy_trunc(char *dst, size_t size, const char *src)
{
    size_t len = strlen(src);
    if (size != 0) {
        size_t n = len < size - 1 ? len : size - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return len;
}

void lincupl_strlwr(char *s)
{
    if (s == NULL) {
        return;
    }
    for (; *s != '\0'; s++) {
        if (*s >= 'A' && *s <= 'Z') {
            *s = (char)(*s - 'A' + 'a');
        }
    }
}

void lincupl_strupr(char *s)
{
    if (s == NULL) {
        return;
    }
    for (; *s != '\0'; s++) {
        if (*s >= 'a' && *s <= 'z') {
            *s = (char)(*s - 'a' + 'A');
        }
    }
}

const char *lincupl_path_sep(const char *path)
{
    const char *sep = strrchr(path, '/');
    if (sep == NULL) {
        sep = strrchr(path, '\\');
    }
    if (sep == NULL) {
        sep = strrchr(path, ':');
    }
    return sep;
}

const char *lincupl_path_sep_last(const char *path)
{
    const char *sep = strrchr(path, '\\');
    const char *slash = strrchr(path, '/');
    if (slash != NULL && (sep == NULL || slash > sep)) {
        sep = slash;
    }
    if (sep == NULL) {
        sep = strrchr(path, ':');
    }
    return sep;
}

size_t lincupl_path_dir_len(const char *path)
{
    const char *sep = lincupl_path_sep(path);
    return sep != NULL ? (size_t)(sep - path) + 1 : 0;
}

/* first n characters of s into out (of size out_size), truncated */
static void copy_part(char *out, size_t out_size, const char *s, size_t n)
{
    if (out == NULL || out_size == 0) {
        return;
    }
    if (n >= out_size) {
        n = out_size - 1;
    }
    memcpy(out, s, n);
    out[n] = '\0';
}

void lincupl_split_path(const char *path, char *dir, size_t dir_size, char *base, size_t base_size,
                        char *ext, size_t ext_size)
{
    const char *sep = lincupl_path_sep_last(path);
    const char *name = sep != NULL ? sep + 1 : path;
    const char *dot = strrchr(name, '.');
    size_t base_len = dot != NULL ? (size_t)(dot - name) : strlen(name);

    copy_part(dir, dir_size, path, (size_t)(name - path));
    copy_part(base, base_size, name, base_len);
    copy_part(ext, ext_size, name + base_len, strlen(name + base_len));
}

const char *lincupl_skip_dot_dir(const char *name)
{
    if (name[0] == '.') {
        name += name[1] != '\0' ? 2 : 1;
    }
    return name;
}

/* index after the last c in the first n characters of s, 0 if there is none */
static size_t after_last(const char *s, size_t n, char c)
{
    while (n > 0 && s[n - 1] != c) {
        n--;
    }
    return n;
}

size_t lincupl_design_file_name(char *out, size_t out_size, const char *dir, const char *source,
                                const char *ext)
{
    size_t len = strlen(source);
    /* the base name ends where a trailing ".pld" starts */
    if (len >= 4 && strcasecmp(source + len - 4, ".pld") == 0) {
        len -= 4;
    }
    /* and starts after the last separator in front of that */
    size_t start = after_last(source, len, '/');
    if (start == 0) {
        start = after_last(source, len, '\\');
    }
    if (start == 0) {
        start = after_last(source, len, ':');
    }
    int n = snprintf(out, out_size, "%s%.*s%s", dir, (int)(len - start), source + start, ext);
    return n < 0 ? 0 : (size_t)n;
}
