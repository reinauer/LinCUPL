/*
 * phase.c - runtime of the compiler phases and the simulator (see phase.h)
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "phase.h"
#include "locate.h"
#include "strutil.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define PHASE_TEXT_MAX 2048

void phase_begin(struct phase_rt *rt)
{
    rt->console = 0;
    rt->status_line[0] = '\0';
    rt->progress_column = 0;
    rt->last_tick = 0;
    rt->allocs.prev = rt->allocs.next = NULL;
    memset(rt->files, 0, sizeof(rt->files));
}

void phase_end(struct phase_rt *rt)
{
    phase_close_all(rt);
    phase_pool_free_all(&rt->allocs);
}

/* -------------------------------------------------------------------------
 * Output
 * ------------------------------------------------------------------------- */

static size_t text_size(const struct phase_rt *rt)
{
    return (rt->text_size == 0 || rt->text_size > PHASE_TEXT_MAX) ? PHASE_TEXT_MAX : rt->text_size;
}

/* Print the complete lines of text (which is modified) after the pending
 * status line and keep the unterminated rest. */
static void status_add_lines(struct phase_rt *rt, char *text)
{
    char *nl;
    while ((nl = strchr(text, '\n')) != NULL) {
        *nl = '\0';
        printf("%s%s\n", rt->status_line, text);
        fflush(stdout);
        rt->status_line[0] = '\0';
        text = nl + 1;
    }
    size_t used = strlen(rt->status_line);
    snprintf(rt->status_line + used, sizeof(rt->status_line) - used, "%s", text);
}

int phase_vprintf(struct phase_rt *rt, FILE *fp, const char *fmt, va_list ap)
{
    char text[PHASE_TEXT_MAX];
    size_t size = text_size(rt);
    int len = vsnprintf(text, size, fmt, ap);
    if (len >= (int)size) {
        len = (int)size - 1;
    }
    if (fp == NULL) {
        return len;
    }
    if (fp != stdout) {
        phase_puts(rt, text, fp);
    } else if (rt->console) {
        fputs(text, stdout);
        fflush(stdout);
    } else {
        status_add_lines(rt, text);
    }
    return len;
}

int phase_printf(struct phase_rt *rt, FILE *fp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int len = phase_vprintf(rt, fp, fmt, ap);
    va_end(ap);
    return len;
}

void phase_trace(struct phase_rt *rt, const char *text)
{
    if (rt->console) {
        if (strlen(text) == 1) {
            fputc(text[0], stdout);
        } else {
            puts(text);
        }
        fflush(stdout);
    } else {
        char copy[PHASE_TEXT_MAX];
        snprintf(copy, text_size(rt), "%s", text);
        status_add_lines(rt, copy);
    }
}

void phase_progress_tick(struct phase_rt *rt)
{
    time_t now = time(NULL);
    if (now - rt->last_tick > PHASE_PROGRESS_INTERVAL) {
        phase_trace(rt, rt->progress_column % 2 == 0 ? "." : "_");
        if (rt->progress_column < PHASE_PROGRESS_COLUMNS) {
            rt->progress_column++;
        } else {
            phase_trace(rt, "\n");
            rt->progress_column = 0;
        }
        rt->last_tick = now;
    }
}

const char *phase_message(const struct phase_rt *rt, int code)
{
    if (code >= 1000) {
        return rt->messages[0];
    }
    if (code >= 0 && code < rt->message_count) {
        return rt->messages[code];
    }
    return rt->unknown_message != NULL ? rt->unknown_message : rt->messages[0];
}

/* -------------------------------------------------------------------------
 * Memory
 * ------------------------------------------------------------------------- */

void *phase_pool_alloc(struct phase_rt *rt, phase_pool_t *pool, size_t size, const char *context)
{
    phase_pool_t *block = calloc(1, sizeof(*block) + (size ? size : 1));
    if (block == NULL) {
        rt->out_of_memory(context);
        return NULL;
    }
    if (pool->next == NULL) {
        pool->next = pool->prev = pool;
    }
    block->next = pool->next;
    block->prev = pool;
    pool->next->prev = block;
    pool->next = block;
    return block + 1;
}

void *phase_alloc(struct phase_rt *rt, size_t size, const char *context)
{
    return phase_pool_alloc(rt, &rt->allocs, size, context);
}

char *phase_strdup(struct phase_rt *rt, const char *s, const char *context)
{
    char *copy = phase_alloc(rt, strlen(s) + 1, context);
    strcpy(copy, s);
    return copy;
}

void phase_free(void *p)
{
    if (p != NULL) {
        phase_pool_t *block = (phase_pool_t *)p - 1;
        block->prev->next = block->next;
        block->next->prev = block->prev;
        free(block);
    }
}

void phase_pool_free_all(phase_pool_t *pool)
{
    while (pool->next != NULL && pool->next != pool) {
        phase_free(pool->next + 1);
    }
    pool->next = pool->prev = NULL;
}

char *phase_dir_prefix(struct phase_rt *rt, const char *path)
{
    size_t len = lincupl_path_dir_len(path);
    char *dir = phase_alloc(rt, len + 1, "initialization");
    memcpy(dir, path, len);
    dir[len] = '\0';
    return dir;
}

char *phase_file_name(struct phase_rt *rt, const char *dir, const char *source, const char *ext)
{
    source = lincupl_skip_dot_dir(source);
    size_t len = lincupl_design_file_name(NULL, 0, dir, source, ext);
    char *name = phase_alloc(rt, len + 1, "initialization");
    lincupl_design_file_name(name, len + 1, dir, source, ext);
    return name;
}

void phase_temp_name(char *name, const char *pid)
{
    size_t len = strlen(pid);
    if (len > 6) {
        pid += len - 6;
        len = 6;
    }
    memcpy(name + 2, pid, len);
}

/* -------------------------------------------------------------------------
 * Files
 * ------------------------------------------------------------------------- */

static int file_slot(const struct phase_rt *rt, FILE *fp)
{
    if (fp == NULL) {
        return -1;
    }
    for (int i = 0; i < PHASE_MAX_OPEN_FILES; i++) {
        if (rt->files[i].fp == fp) {
            return i;
        }
    }
    return -1;
}

FILE *phase_track(struct phase_rt *rt, FILE *fp, const char *path, const char *mode)
{
    if (fp == NULL) {
        return NULL;
    }
    for (int i = 0; i < PHASE_MAX_OPEN_FILES; i++) {
        if (rt->files[i].fp == NULL) {
            size_t len = strlen(path);
            rt->files[i].fp = fp;
            rt->files[i].crlf = strchr(mode, 'b') == NULL && len >= 4 && strcasecmp(path + len - 4, ".jed") == 0;
            rt->files[i].last_cr = 0;
            return fp;
        }
    }
    fclose(fp);
    return NULL;
}

FILE *phase_open(struct phase_rt *rt, const char *path, const char *mode)
{
    return phase_track(rt, lincupl_fopen_ci(path, mode), path, mode);
}

void phase_close(struct phase_rt *rt, FILE *fp)
{
    int slot = file_slot(rt, fp);
    if (slot >= 0) {
        rt->files[slot].fp = NULL;
        fclose(fp);
    }
}

int phase_is_open(const struct phase_rt *rt, FILE *fp)
{
    return file_slot(rt, fp) >= 0;
}

void phase_close_all(struct phase_rt *rt)
{
    for (int i = 0; i < PHASE_MAX_OPEN_FILES; i++) {
        if (rt->files[i].fp != NULL) {
            fclose(rt->files[i].fp);
            rt->files[i].fp = NULL;
        }
    }
}

int phase_putc(struct phase_rt *rt, int c, FILE *fp)
{
    if (fp == NULL) {
        return EOF;
    }
    int slot = file_slot(rt, fp);
    c &= 0xff;
    if (slot >= 0 && rt->files[slot].crlf) {
        if (c == '\n' && !rt->files[slot].last_cr && fputc('\r', fp) == EOF) {
            return EOF;
        }
        rt->files[slot].last_cr = (c == '\r');
    }
    return fputc(c, fp);
}

int phase_puts(struct phase_rt *rt, const char *s, FILE *fp)
{
    if (fp == NULL) {
        return EOF;
    }
    int slot = file_slot(rt, fp);
    if (slot >= 0 && rt->files[slot].crlf) {
        for (; *s != '\0'; s++) {
            if (phase_putc(rt, (unsigned char)*s, fp) == EOF) {
                return EOF;
            }
        }
        return 0;
    }
    return fputs(s, fp) == EOF ? EOF : 0;
}

int phase_read_char(FILE *fp)
{
    int c;
    if (fp == NULL) {
        return EOF;
    }
    do {
        c = fgetc(fp);
    } while (c == '\r');
    if (c == 0x1a) {
        fseek(fp, 0, SEEK_END);
        c = fgetc(fp);
    }
    return c;
}

void phase_remove(const char *path, int any_case)
{
    char name[PATH_MAX];
    if (any_case) {
        lincupl_resolve_ci_path(path, name, sizeof(name));
    } else {
        lincupl_normalize_slashes(path, name, sizeof(name));
    }
    unlink(name);
}
