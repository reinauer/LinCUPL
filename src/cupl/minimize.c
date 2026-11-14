/*
 * minimize - CUPL logic minimizer phase (minimize_run).
 *
 * The phase runs between expand and fusemap when one of the minimization
 * levels -m2 (Quine-McCluskey), -m3 (Presto) or -m4 (Espresso) was
 * requested.  It reads the product term file CM<pid> written by expand and
 * writes CC<pid> for fusemap:
 *
 *   file header (131 bytes), extra block, symbol table (64 bytes per symbol),
 *   then a sequence of frames.  A frame is one expression: a 12 byte header
 *   and one row per product term.  A row consists of three bit planes with
 *   one bit per symbol: the output plane (which symbol the term drives), the
 *   mask plane (which symbols appear in the term) and the value plane (their
 *   polarity).
 *
 * Frames of level 0 and 1 are copied.  Frames of level 2..4 are turned into
 * a multiple-valued cover (one two-part variable per symbol plus one output
 * variable with a part per symbol), minimized and written back.  For devices
 * of class 3 or 5 and with -ks all frames of a design are minimized together
 * so that product terms can be shared between outputs.
 *
 * Levels 2 and 3 use the minimizer in minimize_cube.c, minimize_cover.c and
 * minimize_levels.c, an early form of Berkeley Espresso-MV; the function
 * names are those of the Espresso sources (cofactor, complement, sf_contain,
 * tautology, irredundant, ...), with a minimize_ prefix where they are
 * shared between files.  Only what these levels need is present: the input
 * is always an on-set with a don't-care set, and there is no output phase
 * assignment, input variable pairing or exact minimum cover.  Level 4 runs
 * Berkeley Espresso 2.5.1 instead (see espresso_cover() in
 * minimize_levels.c and minimize_espresso.c), so its covers can differ from
 * what the algorithm of levels 2 and 3 would produce.  The results of
 * levels 3 and 4 are verified against the input cover.
 *
 * All allocations are zero filled and tracked: the working memory of the
 * minimizer is released after each frame, everything else when the phase
 * returns.
 *
 * Malformed input is caught: the flag string is not read past its end, the
 * row reader stops at the end of the frame, the row terminator is not
 * written in front of an empty frame, frames of different sizes are not
 * minimized together, the device class is not read outside the device
 * record.  Where the rows would lead outside a frame, the phase stops with
 * "[0000cm] program error:  product term sections".
 *
 * This file holds the error messages, the memory pools and the driver
 * (status output, memory and files are the phase runtime of
 * src/shared/phase.c); minimize_io.c reads and writes the files.
 *
 * Status text goes to stdout.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include "locate.h"
#include "strutil.h"
#include "cupl_phases.h"
#include "minimize_int.h"

/* -------------------------------------------------------------------------
 * Error messages
 * ------------------------------------------------------------------------- */

/* message texts, indexed by the error codes 0..11 */
static const char *const k_error_text[] = {
    "program error:  ",
    "could not open:  ",
    "could not execute program:  ",
    "invalid file:  ",
    "out of memory:  ",
    "disk write error:  ",
    "invalid library access key ",
    "invalid library interface ",
    "bad library file:  ",
    "device not in library:  ",
    "design too complex for this minimization level ",
    "disk read error, unexpected end of file:  "
};

/* -------------------------------------------------------------------------
 * Phase-wide state
 * ------------------------------------------------------------------------- */
static void out_of_memory(const char *context);

static struct phase_rt g_rt = {
    .code_suffix = "cm",
    .messages = k_error_text,
    .message_count = (int)(sizeof(k_error_text) / sizeof(k_error_text[0])),
    .text_size = 1024,
    .out_of_memory = out_of_memory,
};
static phase_pool_t g_work_allocs;     /* live for one frame */
static jmp_buf g_abort_jmp;            /* target of minimize_fatal_error() */
const char *minimize_args[10];
static int g_error_count;
static int g_list_errors;              /* 'l' flag: errors also go to the listing */
static int g_no_sharing;               /* 'r' flag */
static int g_quiet_format;             /* 'q' flag */
static int g_share_terms;              /* "ks" flag */
static FILE *g_list_file;              /* listing file (.lst) */
static char *g_list_name;

/* -------------------------------------------------------------------------
 * Errors (runtime in src/shared/phase.c)
 * ------------------------------------------------------------------------- */

/* remove CM<pid> and, once it was opened, the output file */
static void remove_temp_files(void)
{
    phase_remove(minimize_cm_name, 0);
    if (minimize_out_name != NULL) {
        phase_remove(minimize_out_name, 0);
    }
}

/* print one message: "[<code>cm] <text><detail>", with -q on stdout
 * "<listing>(0) : Error [<code>cm] :<text><detail>" */
static void print_error(FILE *stream, int code, const char *detail)
{
    if (g_rt.progress_column != 0 && stream == stdout) {
        fputc('\n', stdout);
        fflush(stdout);
        g_rt.progress_column = 0;
    }
    if (!g_quiet_format || stream == g_list_file) {
        phase_printf(&g_rt, stream, "[%04d%s] ", code, g_rt.code_suffix);
    } else {
        phase_printf(&g_rt, stream, "%s(%d) : Error [%04d%s] :",
                     g_list_name != NULL ? g_list_name : "(null)", 0, code, g_rt.code_suffix);
    }
    phase_printf(&g_rt, stream, "%s", phase_message(&g_rt, code));
    phase_printf(&g_rt, stream, "%s\n", detail);
}

/* report an error (on stdout, and in the listing with -l) and continue;
 * codes from 1000 on are printed as "program error" */
void minimize_report_error(int code, const char *detail)
{
    g_error_count++;
    print_error(stdout, code, detail);
    if (g_list_errors) {
        print_error(g_list_file, code, detail);
    }
}

/* report an error and abandon the phase */
void minimize_fatal_error(int code, const char *detail)
{
    if (code >= 0) {
        minimize_report_error(code, detail);
    }
    remove_temp_files();
    longjmp(g_abort_jmp, 1);
}

static void out_of_memory(const char *context)
{
    minimize_fatal_error(ERR_OUT_OF_MEMORY, context);
}

/* -------------------------------------------------------------------------
 * Memory and files
 * ------------------------------------------------------------------------- */

/* called for every working allocation; shows that the phase is
 * still alive by printing a mark every few seconds. */
void minimize_progress_tick(void)
{
    phase_progress_tick(&g_rt);
}

/* Allocation that lives until the phase returns. */
void *minimize_alloc_tracked(size_t size, const char *context)
{
    return phase_alloc(&g_rt, size, context);
}

char *minimize_dup_string(const char *s, const char *context)
{
    return phase_strdup(&g_rt, s, context);
}

/* working memory of the minimizer, released as a whole after each frame
 * (phase_pool_free_all of g_work_allocs). */
void *minimize_work_alloc(size_t size)
{
    minimize_progress_tick();
    return phase_pool_alloc(&g_rt, &g_work_allocs, size, "");
}

void minimize_work_free(void *p)
{
    phase_free(p);
}

/* fopen() with case-insensitive lookup; phase_end() closes what is still
 * open when the phase ends. */
FILE *minimize_open_file(const char *path, const char *mode)
{
    return phase_open(&g_rt, path, mode);
}

void minimize_close_file(FILE *fp)
{
    phase_close(&g_rt, fp);
}

/* =========================================================================
 * Driver
 * ========================================================================= */

/* Read, minimize and store the queued frames of one level. */
static int minimize_level(int level)
{
    pla_t *PLA = minimize_new_PLA(PLA_TYPE_F | PLA_TYPE_D);
    if (!minimize_read_pla(PLA, level)) {
        return 0;
    }
    minimize_pla(PLA, level);
    minimize_store_pla(PLA, level);
    minimize_free_PLA(PLA);
    return 1;
}

/* the next frame to process; designs after the first have their
 * header copied on the way.  A frame that is minimized in two sections is
 * returned twice. */
static int next_frame(frame_t *f)
{
    if (minimize_split_state != 0) {
        return 1;
    }
    for (;;) {
        int kind = minimize_read_frame(f);
        if (kind > 0) {
            minimize_begin_frame(f);
            return 1;
        }
        if (kind != 0) {
            return 0;
        }
        minimize_write_frame(f);
        minimize_copy_design_header();
    }
}

/* minimize the frames of CM<pid> into CC<pid> */
static void minimize_design(void)
{
    frame_t frame;
    frame_t *f;

    if (!minimize_open_files()) {
        return;
    }
    memset(&frame, 0, sizeof(frame));
    int device_class = minimize_load_device_class();

    if ((device_class == 3 || device_class == 5 || g_share_terms) && !g_no_sharing) {
        /* product terms may be shared: minimize all frames together */
        minimize_shared_mode = 1;
        while (next_frame(&frame)) {
            minimize_queue_frame(&frame);
        }
        for (int level = 2; level <= 4; level++) {
            minimize_level(level);
        }
        while ((f = minimize_dequeue_frame()) != NULL) {
            minimize_write_frame(f);
            minimize_free_frame(f);
        }
        phase_pool_free_all(&g_work_allocs);
    } else {
        while (next_frame(&frame)) {
            int level = frame.flags & FRAME_LEVEL_MASK;
            if (level < 2) {
                minimize_write_frame(&frame);
                continue;
            }
            minimize_sf_forget();
            if (minimize_split_state == 0) {
                minimize_queue_frame(&frame);
            }
            if (minimize_level(level)) {
                if (minimize_split_state == 0) {
                    f = minimize_dequeue_frame();
                    minimize_write_frame(f);
                    minimize_free_frame(f);
                }
                phase_pool_free_all(&g_work_allocs);
            }
        }
    }
    minimize_write_frame(&frame);
    minimize_close_files();
}

/* option letters.  'k' takes the next letter as its argument,
 * 'z' skips one letter and 'v' two. */
static void parse_flags(const char *flags)
{
    while (*flags != '\0') {
        int skip = 0;
        switch (*flags) {
        case 'k':
            flags++;
            if (*flags == 's') {
                g_share_terms = 1;
            }
            break;
        case 'l':
            g_list_errors = 1;
            break;
        case 'q':
            g_quiet_format = 1;
            break;
        case 'r':
            g_no_sharing = 1;
            break;
        case 'v':
            g_rt.console = 1;
            skip = 2;
            break;
        case 'z':
            skip = 1;
            break;
        default:
            break;
        }
        /* never skip past the end of the string */
        for (skip++; skip > 0 && *flags != '\0'; skip--) {
            flags++;
        }
    }
}

/* open the listing file to append the error messages to */
static void open_listing(const char *design)
{
    const char *dir = phase_dir_prefix(&g_rt, minimize_args[5]);
    char *path = phase_file_name(&g_rt, dir, design, ".lst");
    g_list_name = phase_file_name(&g_rt, dir, design, ".lst");
    g_list_file = minimize_open_file(path, "at");
    if (g_list_file == NULL) {
        g_list_errors = 0;
        minimize_report_error(ERR_OPEN, path);
        return;
    }
    phase_printf(&g_rt, g_list_file, "\n");
}

/* parse the flags, minimize the design and close the listing; abandons
 * the phase if any error was reported */
static void run_phase(int argc)
{
    g_rt.last_tick = time(NULL);
    minimize_out_name = NULL;
    parse_flags(argc > 6 ? minimize_args[6] : "");
    if (g_list_errors) {
        open_listing(minimize_args[5]);
    }
    minimize_design();
    if (g_list_errors) {
        minimize_close_file(g_list_file);
        g_list_file = NULL;
        g_list_errors = 0;
    }
    if (g_rt.progress_column != 0) {
        phase_trace(&g_rt, "\n");
    }
    if (g_error_count != 0) {
        minimize_fatal_error(-1, "");
    }
}

/* reset the phase-wide state before a run */
static void reset_state(void)
{
    memset(minimize_args, 0, sizeof(minimize_args));
    phase_begin(&g_rt);
    g_error_count = 0;
    g_list_errors = 0;
    g_no_sharing = 0;
    g_quiet_format = 0;
    g_share_terms = 0;
    g_list_file = NULL;
    g_list_name = NULL;
    minimize_io_reset();
    minimize_cube_reset();
    minimize_levels_reset();
}

/* Phase entry point; argv is the argument vector built by the driver
 * (see driver.c).  Returns 0 on success, 1 after an error. */
int minimize_run(int argc, char **argv)
{
    volatile int result = 1;

    reset_state();
    for (int i = 0; i < argc && i < 10; i++) {
        minimize_args[i] = (argv != NULL && argv[i] != NULL) ? argv[i] : "";
    }

    if (argc >= 6 && setjmp(g_abort_jmp) == 0) {
        run_phase(argc);
        result = 0;
    }

    phase_pool_free_all(&g_work_allocs);
    phase_end(&g_rt);
    return result;
}
