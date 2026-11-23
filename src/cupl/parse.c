/*
 * parse - CUPL parser phase (second compiler phase, parse_run).
 *
 * The phase reads the preprocessed design (temp file CA<pid>, written by
 * preprocess) and writes the symbol table and one equation per assigned
 * variable to the temp file CB<pid> for expand:
 *
 *   1. Parsing.  A table-driven recursive parser (parse_grammar.c, the
 *      grammar is in parse_tables.h) reads the tokens of the lexer
 *      (parse_lex.c) and calls a semantic action for most rules
 *      (parse_actions.c).  The actions keep a stack of token lists ("work
 *      lists", parse_tokens.c) and enter variables in the symbol table
 *      (parse_symtab.c).  Expressions are stored as flat token lists on the
 *      variables they are assigned to.
 *   2. Lowering.  TABLE, function calls and range/match expressions
 *      ([a..b]:value) (parse_lower.c) and SEQUENCE/SEQUENCED/SEQUENCEJK/
 *      SEQUENCERS/SEQUENCET and CONDITION (parse_sequence.c) are not parsed
 *      into equations directly: their actions build ordinary CUPL
 *      statements as token lists and push them onto the front of the token
 *      queue, from where the parser reads them back before it continues
 *      with the file.  (APPEND statements, ".odel"/".fdel" pseudo operators
 *      and so on are used in the generated text.)
 *   3. Output (parse_output.c).  The symbols are flattened into a sorted
 *      array, numbered, and written to CB<pid> together with the header
 *      items and the expressions, with intermediate variables expanded in
 *      place and D/T/JK/SR register conversions (REGISTER_SELECT) applied.
 *
 * Optional outputs: <design>.lst (flag l) and <design>.oed (DECLARE and
 * PROPERTY statements).
 *
 * This file holds the error messages, the listing and the entry point;
 * status output, memory and files are the phase runtime of
 * src/shared/phase.c.  Declarations shared by the parse_*.c files are in
 * parse_int.h.
 *
 * Robustness:
 *   - Fixed-size buffers are bounds-checked.
 *   - Bytes of the 131-byte header record of CB<pid> that no header item
 *     sets are written as zero.
 *   - A cycle of three or more intermediate variables is reported as
 *     error 30 instead of recursing without end.
 *   - A number in a FIELD list is written to CB<pid> with the value 0.
 *
 * Status messages go to stdout.
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
#include "platform.h"
#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "locate.h"
#include "strutil.h"
#include "phase.h"
#include "cupl_phases.h"
#include "banner.h"
#include "parse_int.h"

/* message texts, indexed by error code */
static const char *const k_error_text[] = {
    "program error:  ",
    "could not open:  ",
    "invalid number:  ",
    "invalid library access key",
    "invalid library interface",
    "bad library file:  ",
    "device not in library:  ",
    "invalid syntax:  ",
    "too many errors",
    "missing symbol:  ",
    "vector too wide",
    "expression already assigned to:  ",
    "vector size mismatch:  ",
    "undefined function:  ",
    "variable already declared:  ",
    "out of memory:  ",
    "invalid number of function arguments:  ",
    "disk write error:  ",
    "intermediate var not assigned an expression:  ",
    "indexed and non-indexed vars in range or match expression",
    "index too large for range or match operation",
    "header item already declared",
    "missing header item(s)",
    "invalid range arguments:  ",
    "range or match number larger than variable list",
    "range minimization error",
    "invalid table statement",
    "invalid present state number",
    "invalid next state number",
    "invalid flip-flop type for sequence statement:  ",
    "intermediate dependent on itself:  ",
    "invalid minimization level:  ",
    "invalid next state:  'h'",
    "multiple asynchronous defaults for state:  'h'",
    "multiple synchronous defaults for state:  'h'",
    "multiple unconditional statements for state:  'h'",
    "device does not support synchronous state machines:  ",
    "duplicate present state:  'h'",
    "target device not specified",
    "line exceeds maximum length",
    "invalid or duplicate header name:  ",
    "don't care(s) not allowed for decimal number, treated as 0",
    "range or match list completely don't cared, decoded to 0",
    "invalid GROUP name:  ",
    "unexpected end-of-file",
    "reached end-of-file before ending comment",
    "invalid DeMorgan level ",
    "vector size mismatch in comparison vector: ",
    "fixed polarity device, reset DeMorgan level to 0: ",
    "unknown DECLARE entity: ",
    "invalid register type conversion: ",
    "invalid Register_Select level "
};

/* -------------------------------------------------------------------------
 * Runtime state
 * ------------------------------------------------------------------------- */
static void out_of_memory(const char *context);

static struct phase_rt g_rt = {
    .code_suffix = "ca",
    .messages = k_error_text,
    .message_count = (int)(sizeof(k_error_text) / sizeof(k_error_text[0])),
    .unknown_message = "(null)",
    .text_size = 1024,
    .out_of_memory = out_of_memory,
};
static jmp_buf g_abort_jmp;            /* parse_fatal_error() returns to the driver */
const char *g_parse_args[10];          /* copy of the phase arguments */

/* options */
static int g_listing;                  /* flag l */
int g_parse_min_level;                 /* flag m */
static int g_quiet_format;             /* flag q, "file(line) : Error" messages */
int g_parse_demorgan_mode;             /* flag kb = 2, kd = 1 */
int g_parse_one_hot;                   /* flag o, one state bit per state */
static char g_quiet_name[32];          /* source name for flag q */

/* errors and listing */
static int g_error_count;
static int g_pending_count;
static struct {
    int kind;                          /* 0 error, 1 warning, 2 error about a symbol */
    int code;
    const char *text;
    const char *ext;
} g_pending[MAX_PENDING_ERRORS];
static int g_listing_extra;            /* message lines written to the listing */
static char *g_listing_name;
static FILE *g_listing_fp;
static int g_listing_page;
static time_t g_listing_time;

/* temp files */
static char *g_temp_name;              /* CA<pid> */
char *g_parse_out_name;                /* CB<pid> */

static int g_parsing;                 /* fatal errors list the line first */

/* -------------------------------------------------------------------------
 * Output, files and memory (runtime in src/shared/phase.c)
 * ------------------------------------------------------------------------- */

/* diagnostic trace output */
void parse_trace_puts(const char *text)
{
    phase_trace(&g_rt, text);
}

/* formatted output to the status output (fp == stdout) or a file */
static void out_printf(FILE *fp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    phase_vprintf(&g_rt, fp, fmt, ap);
    va_end(ap);
}

static void out_of_memory(const char *context)
{
    parse_fatal_error(ERR_OUT_OF_MEMORY, context);
}

void *parse_alloc_tracked(size_t size, const char *context)
{
    return phase_alloc(&g_rt, size, context);
}

char *parse_dup_string(const char *s, const char *context)
{
    return phase_strdup(&g_rt, s, context);
}

FILE *parse_open_file(const char *path, const char *mode)
{
    return phase_open(&g_rt, path, mode);
}

void parse_close_file(FILE *fp)
{
    phase_close(&g_rt, fp);
}

/* Close all open files except the listing and the .oed file, which stay
 * open across the end of the parse. */
static void close_work_files(void)
{
    for (int i = 0; i < PHASE_MAX_OPEN_FILES; i++) {
        FILE *fp = g_rt.files[i].fp;
        if (fp != NULL && !(g_listing && fp == g_listing_fp) && fp != g_parse_oed_fp) {
            phase_close(&g_rt, fp);
        }
    }
}

/* Text-mode read of an open file (EOF for any other stream) */
int parse_read_char(FILE *fp)
{
    if (!phase_is_open(&g_rt, fp)) {
        return EOF;
    }
    return phase_read_char(fp);
}

void parse_unread_char(int c, FILE *fp)
{
    if (c != EOF && phase_is_open(&g_rt, fp)) {
        ungetc(c & 0xff, fp);
    }
}

/* close the work files and delete the input temp file */
static void remove_temp_file(void)
{
    if (g_temp_name != NULL) {
        close_work_files();
        phase_remove(g_temp_name, 0);
    }
    g_temp_name = NULL;
    g_parse_out_name = NULL;
}

/* -------------------------------------------------------------------------
 * Error reporting
 * ------------------------------------------------------------------------- */

/* one message line */
static void print_message(FILE *fp, int code, int kind, const char *text, const char *ext)
{
    if (g_quiet_format && !g_listing) {
        out_printf(fp, "%s(%d) : %s [%04d%s] :", g_quiet_name, g_parse_in_open ? g_parse_in_line : 0,
                   kind == 1 ? "Warning" : "Error", code, g_rt.code_suffix);
    } else {
        out_printf(fp, "[%04d%s] ", code, g_rt.code_suffix);
        if (kind == 1) {
            out_printf(fp, "Please note: ");
        }
    }
    out_printf(fp, "%s", phase_message(&g_rt, code));
    if (text != NULL) {
        out_printf(fp, "%s", text);
    }
    if (ext != NULL) {
        out_printf(fp, ".%s", ext);
    }
    out_printf(fp, "\n");
    fflush(fp);
}

/* the marker line and the messages queued for the current line */
static void print_pending(FILE *fp)
{
    if (g_pending_count == 0) {
        return;
    }
    /* cut the marker line behind the last '^' */
    int i = LINE_MAX_CHARS;
    while (i > 0 && g_parse_marker[i] != '^') {
        i--;
    }
    if (i > 0 && i < LINE_MAX_CHARS) {
        g_parse_marker[i + 1] = '\0';
    } else if (i == 0) {
        /* a marker in the first column is lost */
        g_parse_marker[0] = '\0';
    }
    if (!g_quiet_format || fp == g_listing_fp) {
        out_printf(fp, "    %s\n", g_parse_marker);
    }
    for (i = 0; i < g_pending_count; i++) {
        print_message(fp, g_pending[i].code, g_pending[i].kind, g_pending[i].text, g_pending[i].ext);
    }
    fflush(fp);
}

static void clear_marker(void)
{
    memset(g_parse_marker, ' ', LINE_MAX_CHARS);
    g_parse_marker[LINE_MAX_CHARS] = '\0';
}

/* queue a message for the current line */
static void queue_message(int code, int kind, const char *text, const char *ext)
{
    int col = g_parse_in_open ? g_parse_in_col - 1 : -1;
    if (kind != 1) {
        g_error_count++;
    }
    g_pending_count++;
    if (col < 0) {
        col = 0;
    }
    g_parse_marker[col] = '^';
    g_pending[g_pending_count - 1].code = code;
    g_pending[g_pending_count - 1].kind = kind;
    g_pending[g_pending_count - 1].text = parse_intern(text);
    g_pending[g_pending_count - 1].ext = parse_intern(ext);
    if (g_pending_count >= MAX_PENDING_ERRORS) {
        print_pending(stdout);
        if (g_listing) {
            print_pending(g_listing_fp);
        }
        parse_fatal_error(ERR_TOO_MANY, "");
    }
}

void parse_error_at(int code, const char *text)
{
    queue_message(code, 0, text, NULL);
}

void parse_warning_at(int code, const char *text)
{
    queue_message(code, 1, text, NULL);
}

/* queue an error that names a symbol (name.ext) */
void parse_error_symbol(int code, const symbol_t *sym)
{
    if (sym == NULL) {
        queue_message(code, 2, "", "");
    } else {
        queue_message(code, 2, sym->name, sym->ext);
    }
}

/* messages printed at once (output pass) */
void parse_report_now(int code, int kind, const char *text, const char *ext)
{
    if (kind != 1) {
        g_error_count++;
    }
    print_message(stdout, code, kind, text, ext);
    if (g_listing) {
        print_message(g_listing_fp, code, kind, text, ext);
        g_listing_extra++;
    }
}

/* report an error and abandon the phase.  A negative code only
 * abandons the phase. */
void parse_fatal_error(int code, const char *detail)
{
    if (g_parsing) {
        parse_end_of_line();
    }
    if (code >= 0) {
        print_message(stdout, code, 0, detail, NULL);
        if (g_listing) {
            print_message(g_listing_fp, code, 0, detail, NULL);
            g_listing_extra++;
            fflush(g_listing_fp);
        }
    }
    if (g_listing) {
        parse_close_file(g_listing_fp);
    }
    remove_temp_file();
    parse_close_file(g_parse_in);
    longjmp(g_abort_jmp, 1);
}

/* -------------------------------------------------------------------------
 * Listing
 * ------------------------------------------------------------------------- */

/* file name without directory and extension */
static void base_name(const char *path, char *out, size_t out_size)
{
    lincupl_split_path(path, NULL, 0, out, out_size, NULL, 0);
}

/* directory part of a path including the separator */
char *parse_directory_prefix(const char *path)
{
    return phase_dir_prefix(&g_rt, path);
}

/* dir + base name of the source without "./" and ".pld" + ext */
char *parse_make_file_name(const char *dir, const char *source, const char *ext)
{
    return phase_file_name(&g_rt, dir, source, ext);
}

/* page header of the listing */
static void listing_page_header(void)
{
    char base[36];
    if (g_listing_page > 1) {
        out_printf(g_listing_fp, "\f\n");
    }
    base_name(g_parse_source_path, base, sizeof(base));
    out_printf(g_listing_fp, "LISTING FOR LOGIC DESCRIPTION FILE: %s%s", base, ".pld");
    for (int i = 0; i < 0x48 - ((int)strlen(base) + 0x2b); i++) {
        fputc(' ', g_listing_fp);
    }
    out_printf(g_listing_fp, "Page %d\n\n", g_listing_page++);
    out_printf(g_listing_fp, "%s %s cupl: %s\n%s\n", PROGRAM_NAME, PROGRAM_VERSION,
               "Universal Compiler for Programmable Logic", PROGRAM_COPYRIGHT);
    out_printf(g_listing_fp, "Created %s\n", ctime(&g_listing_time));
    fflush(g_listing_fp);
    if (ferror(g_listing_fp)) {
        parse_fatal_error(ERR_DISK_WRITE, g_listing_name);
    }
}

/* flag l */
static void open_listing(void)
{
    g_listing_page = 1;
    g_listing_name = parse_make_file_name(parse_directory_prefix(g_parse_source_path), g_parse_source_path,
                                          ".lst");
    g_listing_fp = parse_open_file(g_listing_name, "wt");
    if (g_listing_fp == NULL) {
        parse_fatal_error(ERR_OPEN, g_listing_name);
    }
    g_listing_time = time(NULL);
    listing_page_header();
}

/* a source line is complete: list it and print its messages */
void parse_end_of_line(void)
{
    if (!g_parse_in_open) {
        return;
    }
    g_parse_line[g_parse_in_col] = '\0';
    if (g_listing) {
        if (g_pending_count != 0) {
            g_listing_extra += g_pending_count + 1;
        }
        if ((g_parse_in_line + g_listing_extra) % 54 == 0) {
            listing_page_header();
        }
        out_printf(g_listing_fp, "%3d:%s\n", g_parse_in_line, g_parse_line);
    }
    if (g_pending_count != 0) {
        if (!g_quiet_format) {
            out_printf(stdout, "%3d:%s\n", g_parse_in_line, g_parse_line);
        }
        print_pending(stdout);
        if (g_listing) {
            print_pending(g_listing_fp);
        }
        clear_marker();
        g_pending_count = 0;
    }
    if (g_listing) {
        fflush(g_listing_fp);
    }
    g_parse_in_col = 0;
    g_parse_in_line++;
}

/* =========================================================================
 * Phase driver
 * ========================================================================= */

/* option letters */
static void parse_flags(const char *p)
{
    for (; *p != '\0'; p++) {
        switch (*p) {
        case 'k':
            if (p[1] == '\0') {
                return;
            }
            p++;
            if (*p == 'b') {
                g_parse_demorgan_mode = 2;
            } else if (*p == 'd') {
                g_parse_demorgan_mode = 1;
            }
            break;
        case 'l':
            g_listing = 1;
            open_listing();
            break;
        case 'm':
            if (p[1] == '\0') {
                parse_report_now(ERR_MIN_LEVEL, 0, NULL, NULL);
                return;
            }
            p++;
            if (*p >= '0' && *p <= '4') {
                g_parse_min_level = *p - '0';
            } else {
                parse_report_now(ERR_MIN_LEVEL, 0, p, NULL);
            }
            break;
        case 'o':
            g_parse_one_hot = 1;
            break;
        case 'q':
            g_quiet_format = 1;
            base_name(g_parse_source_path, g_quiet_name, sizeof(g_quiet_name) - 4);
            strcat(g_quiet_name, ".pld");
            break;
        case 'v':
            g_rt.console = 1;
            break;
        case 'z':
            if (p[1] == '\0') {
                return;
            }
            p++;
            break;
        default:
            break;
        }
    }
}

/* parse the preprocessed design */
static void parse_design(void)
{
    g_temp_name = parse_dup_string("CAXXXXXX", "");
    phase_temp_name(g_temp_name, g_parse_pid_text);
    parse_lex_open(g_temp_name);

    g_parse_table = parse_new_table("no_name");
    parse_actions_begin();

    clear_marker();
    parse_select_device_from_args();
    g_parsing = 1;
    parse_grammar_run();
    remove_temp_file();
}

/* give all phase state its initial value, so that the phase can run
 * more than once in one process */
static void reset_state(void)
{
    memset(g_parse_args, 0, sizeof(g_parse_args));
    phase_begin(&g_rt);
    g_listing = 0;
    g_parse_min_level = 1;
    g_quiet_format = 0;
    g_parse_demorgan_mode = 0;
    g_parse_one_hot = 0;
    g_quiet_name[0] = '\0';
    g_error_count = 0;
    g_pending_count = 0;
    g_listing_extra = 0;
    g_listing_name = NULL;
    g_listing_fp = NULL;
    g_listing_page = 0;
    g_listing_time = 0;
    g_temp_name = NULL;
    g_parse_out_name = NULL;
    g_parsing = 0;

    parse_lex_reset();
    parse_tokens_reset();
    parse_symtab_reset();
    parse_grammar_reset();
    parse_actions_reset();
    parse_lower_reset();
    parse_sequence_reset();
    parse_output_reset();
}

/* parse the design, write CB<pid> if there were no errors, close the
 * listing and the .oed file */
static void run_phase(void)
{
    parse_grow_targets();
    parse_flags(g_parse_args[6]);
    parse_design();
    if (g_error_count == 0) {
        parse_require_device(g_parse_table);
        if (g_error_count == 0) {
            parse_write_output();
        }
    }
    if (g_error_count != 0) {
        parse_fatal_error(-1, "");
    }
    if (g_listing) {
        fflush(g_listing_fp);
        if (ferror(g_listing_fp)) {
            parse_fatal_error(ERR_DISK_WRITE,
                        parse_make_file_name(parse_directory_prefix(g_parse_source_path),
                                             g_parse_source_path, ".lst"));
        }
        parse_close_file(g_listing_fp);
    }
    parse_close_file(g_parse_in);
    if (g_parse_oed_fp != NULL) {
        fflush(g_parse_oed_fp);
        parse_close_file(g_parse_oed_fp);
        g_parse_oed_fp = NULL;
    }
}

/* Entry point of the phase.  argv: [1] work directory, [2] device library,
 * [3] process id for the temp file names, [4] device ('&' for none),
 * [5] source file, [6] option letters, [7] "1" for a virtual device.
 * Returns 0 on success, 1 on error. */
int parse_run(int argc, char **argv)
{
    volatile int result = 1;

    reset_state();
    /* missing arguments among the first seven become empty strings */
    for (int i = 0; i < argc && i < 8; i++) {
        if (argv == NULL || argv[i] == NULL) {
            if (i >= 7) {
                break;
            }
            g_parse_args[i] = "";
        } else {
            g_parse_args[i] = argv[i];
        }
    }
    for (int i = 0; i < 7; i++) {
        if (g_parse_args[i] == NULL) {
            g_parse_args[i] = "";
        }
    }

    if (setjmp(g_abort_jmp) == 0) {
        if (argc < 3) {
            parse_fatal_error(-1, "");
        }
        run_phase();
        result = 0;
    }

    phase_end(&g_rt);
    return result;
}
