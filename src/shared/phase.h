/*
 * phase.h - runtime of the compiler phases and the simulator
 *
 * preprocess, parse, expand, minimize, fusemap and the simulator share one
 * runtime: status output, message texts, memory that is released when the
 * phase ends, files that are closed when the phase ends (also after an
 * error) and text-mode reading.  Each phase keeps one struct phase_rt,
 * fills in the fields that describe it and calls phase_begin() before and
 * phase_end() after a run.
 *
 * Status output: text for stdout is written at once in console mode (the
 * 'v' flag of every phase).  Otherwise only complete lines are printed; the
 * unterminated rest waits in status_line for the next newline, and text
 * never followed by a newline is not printed at all.
 *
 * Messages are numbered.  They are printed as "[<number><suffix>] <text>",
 * the number with four digits and the suffix naming the phase ("cx"
 * preprocess, "ca" parse, "cb" expand, "cm" minimize, "cc" fusemap, "sa"
 * the simulator).
 */
#ifndef LINCUPL_PHASE_H
#define LINCUPL_PHASE_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>

#if defined(__GNUC__) || defined(__clang__)
#define PHASE_PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#define PHASE_PRINTF(a, b)
#endif

#define PHASE_MAX_OPEN_FILES 32
#define PHASE_PROGRESS_INTERVAL 5   /* seconds between progress marks */
#define PHASE_PROGRESS_COLUMNS 70   /* progress marks per line */

/* A list of tracked memory blocks; each block starts with this link. */
typedef struct phase_block {
    struct phase_block *prev;
    struct phase_block *next;
} phase_pool_t;

struct phase_rt {
    /* Description of the phase */
    const char *code_suffix;            /* two letters after the message number */
    const char *const *messages;        /* message texts by number */
    int message_count;
    const char *unknown_message;        /* text for a number below 1000 outside the
                                         * table; NULL for entry 0 */
    size_t text_size;                   /* formatted text is cut to text_size - 1
                                         * characters (at most 2048) */
    void (*out_of_memory)(const char *context);  /* reports the error and
                                                  * abandons the phase */

    /* State, cleared by phase_begin() */
    int console;                        /* status text straight to stdout */
    char status_line[1024];             /* unterminated status text */
    int progress_column;                /* progress marks on the current line */
    time_t last_tick;                   /* time of the last progress mark */
    phase_pool_t allocs;                /* released by phase_end() */
    struct {
        FILE *fp;
        char crlf;                      /* text file named *.jed: "\n" is written as "\r\n" */
        char last_cr;                   /* the last character written was '\r' */
    } files[PHASE_MAX_OPEN_FILES];
};

/* Clear the state (not the description); phase_end() releases what is
 * still open or allocated. */
void phase_begin(struct phase_rt *rt);
void phase_end(struct phase_rt *rt);

/* ---- Output ------------------------------------------------------------ */

/* Formatted text to fp: stdout is the status output, other streams are
 * written with phase_puts().  Nothing is written to a NULL stream.  Returns
 * the length of the text (after cutting it to text_size - 1). */
int phase_vprintf(struct phase_rt *rt, FILE *fp, const char *fmt, va_list ap);
int phase_printf(struct phase_rt *rt, FILE *fp, const char *fmt, ...) PHASE_PRINTF(3, 4);

/* Trace text on the status output; in console mode a newline is added
 * unless the text is a single character. */
void phase_trace(struct phase_rt *rt, const char *text);

/* Called from long loops: every PHASE_PROGRESS_INTERVAL seconds a mark
 * ('.' and '_' alternating) is added to the status output, and a newline
 * after PHASE_PROGRESS_COLUMNS of them.  progress_column counts the marks
 * on the current line. */
void phase_progress_tick(struct phase_rt *rt);

/* Message text of a message number; numbers from 1000 on (internal errors)
 * have entry 0. */
const char *phase_message(const struct phase_rt *rt, int code);

/* ---- Memory ------------------------------------------------------------- */

/* Zero-filled block in rt->allocs or another pool.  When no memory is left
 * rt->out_of_memory(context) is called. */
void *phase_alloc(struct phase_rt *rt, size_t size, const char *context);
void *phase_pool_alloc(struct phase_rt *rt, phase_pool_t *pool, size_t size, const char *context);
char *phase_strdup(struct phase_rt *rt, const char *s, const char *context);
/* Release one block of any pool; NULL is ignored. */
void phase_free(void *p);
void phase_pool_free_all(phase_pool_t *pool);

/* Directory part of path including its separator ("" if none), and
 * dir + the design's file name without "./" and ".pld" + ext
 * (lincupl_design_file_name), both in rt->allocs. */
char *phase_dir_prefix(struct phase_rt *rt, const char *path);
char *phase_file_name(struct phase_rt *rt, const char *dir, const char *source, const char *ext);

/* Put the last six characters of the process id argument into a temp
 * file name after its two letters ("CA" + pid). */
void phase_temp_name(char *name, const char *pid);

/* ---- Files -------------------------------------------------------------- */

/* fopen with a case-insensitive name lookup (lincupl_fopen_ci), and the
 * file is registered.  NULL if it cannot be opened or all slots are used. */
FILE *phase_open(struct phase_rt *rt, const char *path, const char *mode);
/* Register a file the phase opened itself; with all slots used it is
 * closed and NULL is returned. */
FILE *phase_track(struct phase_rt *rt, FILE *fp, const char *path, const char *mode);
/* Close a registered file; other streams are left alone. */
void phase_close(struct phase_rt *rt, FILE *fp);
int phase_is_open(const struct phase_rt *rt, FILE *fp);
void phase_close_all(struct phase_rt *rt);

/* fputc / fputs that write "\n" as "\r\n" in registered *.jed text files.
 * EOF for a NULL stream or a write error. */
int phase_putc(struct phase_rt *rt, int c, FILE *fp);
int phase_puts(struct phase_rt *rt, const char *s, FILE *fp);

/* Text-mode read: carriage returns are dropped and Ctrl-Z ends the file.
 * EOF for a NULL stream. */
int phase_read_char(FILE *fp);

/* Delete a file; with any_case a file whose name differs only in case is
 * found too. */
void phase_remove(const char *path, int any_case);

#endif /* LINCUPL_PHASE_H */
