/*
 * expand - CUPL Boolean expansion phase (third compiler phase, expand_run).
 *
 * The phase reads the intermediate file CB<pid> written by parse (a header,
 * the symbol table and every equation as a stream of infix tokens) and writes
 * CM<pid> for minimize.  For every design section in the file it
 *
 *   1. looks the target device up in the device library and binds every
 *      symbol to the device's pin functions and macrocell nodes
 *      (expand_device.c),
 *   2. parses each equation into an expression tree (expand_input.c),
 *   3. applies the output polarity, pushes negations down to the variables
 *      (DeMorgan), rewrites XOR as AND/OR unless the output has an XOR gate,
 *      and multiplies AND over OR until the tree is a sum of products
 *      (expand_tree.c),
 *   4. writes the sum of products as a "frame": one row of three bit maps
 *      (outputs, variables used, variables true) per product term.  When the
 *      symbol asks for minimisation, identical, covered and adjacent rows
 *      are merged while the frame is built (expand_frame.c).
 *
 * The real minimisers (Quine-McCluskey, Presto, Espresso) live in minimize;
 * the -m level only reaches this phase as a per-symbol on/off switch.
 *
 * This file holds the messages of the phase and the driver; status output,
 * memory and files are the phase runtime of src/shared/phase.c.
 *
 * Fixed-size buffers are bounds-checked, and reads outside the device image
 * or through null pointers (possible with malformed input) yield 0.  Status
 * and error messages go to stdout.
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
#include "expand_int.h"

/* -------------------------------------------------------------------------
 * Error messages
 * ------------------------------------------------------------------------- */

/* Message texts, indexed by error code; codes from 1000 up use entry 0. */
static const char *const k_error_text[] = {
    "program  error:  ",
    "could not open:  ",
    "could not execute program:  ",
    "invalid file:  ",
    "missing or mismatched parentheses:  ",
    "invalid library access key",
    "invalid library interface",
    "bad library file:  ",
    "device not in library:  ",
    "redeclared:  ",
    "invalid output:  ",
    "unknown extension:  ",
    "invalid usage:  ",
    "invalid output extension or usage:  ",
    "invalid input:  ",
    "device not yet fully supported:  ",
    "no expression assigned to:  ",
    "out of memory:  ",
    "missing flip-flop expression for:  ",
    "DeMorgan's theorem invoked for:  ",
    "invalid mix of banked outputs:  ",
    "no expression allowed for:  ",
    "conflicting input architectures:  ",
    "disk write error:  ",
    "output defined for node which does not exist:  ",
    "output mutually excluded by previous output:  ",
    "disk read error, unexpected end of file:  ",
    "Invalid output pin: "
};
#define ERROR_TEXT_COUNT ((int)(sizeof(k_error_text) / sizeof(k_error_text[0])))

/* -------------------------------------------------------------------------
 * Phase-wide state
 * ------------------------------------------------------------------------- */
static void out_of_memory(const char *context);

static struct phase_rt g_rt = {
    .code_suffix = "cb",
    .messages = k_error_text,
    .message_count = ERROR_TEXT_COUNT,
    .unknown_message = "",
    .text_size = 1024,
    .out_of_memory = out_of_memory,
};
static jmp_buf g_abort_jmp;
const char *expand_args[10];
const char *expand_mode;
int expand_error_count;
static int g_listing;                  /* 'l' flag, errors are copied to the .lst file */
static int g_long_messages;            /* 'q' flag, "file(line) : Error" format */
int expand_keep_xor;
static FILE *g_list_file;
static char *g_list_name;
static int g_line_known;               /* equations are being read; messages show expand_line */

/* Intermediate files */
FILE *expand_in;
FILE *expand_out;
char expand_in_name[16];
char expand_out_name[16];
uint8_t expand_header[CF_HEADER_SIZE];
uint8_t *expand_header_extra;
char *expand_device_name;

/* Symbols */
symbol_t *expand_symbols;
int expand_symbol_count;
int expand_symbol_alloc;
symbol_t *expand_extra_symbols;
int expand_extra_count;
int expand_extra_max;

/* Current equation */
symbol_t *expand_cur_sym;
unsigned expand_cur_index;
node_t *expand_expr;
int expand_line;
int expand_invert_output;
int expand_min_level;

/* -------------------------------------------------------------------------
 * Files, errors and memory (runtime in src/shared/phase.c)
 * ------------------------------------------------------------------------- */

/* Called from the inner loops; shows a sign of life every five
 * seconds. */
void expand_progress_tick(void)
{
    phase_progress_tick(&g_rt);
}

FILE *expand_open_file(const char *path, const char *mode)
{
    return phase_open(&g_rt, path, mode);
}

/* Close and delete the CB<pid> input file. */
static void remove_input_file(void)
{
    if (expand_in != NULL) {
        phase_close(&g_rt, expand_in);
    }
    phase_remove(expand_in_name, 0);
}

/* The output file is closed but deliberately not deleted, so CM<pid> stays
 * behind after a fatal error. */
static void close_output_file(void)
{
    if (expand_out != NULL) {
        phase_close(&g_rt, expand_out);
    }
}

static const char *text_or_null(const char *s)
{
    return s != NULL ? s : "(null)";
}

/* One error or warning message on one stream.
 * `detail` and `ext` fill the "%s\n" or "%s.%s\n" tail. */
static void print_message(FILE *fp, int warning, int code, const char *detail, const char *ext, int pin)
{
    const char *text = phase_message(&g_rt, code);
    if (g_rt.progress_column != 0 && fp == stdout) {
        phase_printf(&g_rt, stdout, "\n");
        g_rt.progress_column = 0;
    }
    if (!g_long_messages || fp == g_list_file) {
        phase_printf(&g_rt, fp, "[%04d%s] ", code, g_rt.code_suffix);
        if (warning) {
            phase_printf(&g_rt, fp, "Please note: %s", text);
        }
    } else {
        int line = g_line_known ? expand_line : 0;
        if (warning) {
            phase_printf(&g_rt, fp, "%s(%d) : Warning [%04d%s] : %s ", text_or_null(g_list_name), line,
                         code, g_rt.code_suffix, text);
        } else {
            phase_printf(&g_rt, fp, "%s(%d) : Error [%04d%s] :", text_or_null(g_list_name), line, code,
                         g_rt.code_suffix);
        }
    }
    if (!warning) {
        if (pin >= 0) {
            phase_printf(&g_rt, fp, "pin/node %d ", pin);
        }
        phase_printf(&g_rt, fp, "%s", text);
    }
    if (ext != NULL) {
        phase_printf(&g_rt, fp, "%s.%s\n", text_or_null(detail), ext);
    } else {
        phase_printf(&g_rt, fp, "%s\n", text_or_null(detail));
    }
    fflush(fp);
}

/* Message on stdout and, with -l, in the listing */
static void emit_message(int warning, int code, const char *detail, const char *ext, int pin)
{
    print_message(stdout, warning, code, detail, ext, pin);
    if (g_listing) {
        print_message(g_list_file, warning, code, detail, ext, pin);
    }
}

/* Report an error and continue */
void expand_report_error(int code, const char *detail)
{
    expand_error_count++;
    emit_message(0, code, detail, NULL, -1);
}

/* Report an error (unless code < 0) and abandon the phase */
void expand_fatal_error(int code, const char *detail)
{
    if (code >= 0) {
        expand_report_error(code, detail);
    }
    remove_input_file();
    close_output_file();
    longjmp(g_abort_jmp, 1);
}

static void out_of_memory(const char *context)
{
    expand_fatal_error(ERR_OUT_OF_MEMORY, context);
}

static int symbol_has_ext(const symbol_t *sym)
{
    return sym->ext[0] != '\0' && sym->ext[0] != ' ';
}

static void symbol_message(int warning, int code, const symbol_t *sym, int pin)
{
    char name[SYMBOL_NAME_SIZE + 1];
    char ext[SYMBOL_EXT_SIZE + 1];
    symbol_name(sym, name, sizeof(name));
    snprintf(ext, sizeof(ext), "%.*s", SYMBOL_EXT_SIZE, sym->ext);
    emit_message(warning, code, name, symbol_has_ext(sym) ? ext : NULL, pin);
}

/* Error naming a symbol */
void expand_report_symbol_error(int code, const symbol_t *sym)
{
    expand_error_count++;
    symbol_message(0, code, sym, -1);
}

/* Error naming a symbol and its pin */
void expand_report_pin_error(int code, const symbol_t *sym)
{
    expand_error_count++;
    symbol_message(0, code, sym, sym->pin);
}

/* Warning naming a symbol */
void expand_report_symbol_warning(int code, const symbol_t *sym)
{
    symbol_message(1, code, sym, -1);
}

/* Zero-filled block that is released when the phase returns. */
void *expand_alloc_tracked(size_t size, const char *context)
{
    return phase_alloc(&g_rt, size, context);
}

/* -------------------------------------------------------------------------
 * Phase driver
 * ------------------------------------------------------------------------- */

/* All equations of a design section.  Returns what
 * expand_read_equation() returned last. */
static int process_equations(void)
{
    int more;
    expand_stacks_alloc();
    while ((more = expand_read_equation()) > 1) {
        if (!is_field_symbol(expand_cur_sym)) {
            expand_apply_polarity();
            expand_min_level = expand_cur_sym->flags & SYMF_MIN_LEVEL;
            expand_equation();
        }
        unsigned type = dev_u8(dev_pinfn(expand_cur_sym->pin_fn, PINFN_TYPE));
        if (type == 2 || type == 3) {
            char name[SYMBOL_NAME_SIZE + 1];
            symbol_name(expand_cur_sym, name, sizeof(name));
            expand_report_error(ERR_INVALID_OUTPUT_PIN, name);
        }
        expand_fold_tree(expand_expr);
        expand_build_frame(expand_expr, 0);
        expand_tree_release(expand_expr);
        expand_expr = NULL;
    }
    return more;
}

/* Frames of the generated symbols */
static void write_preset_frames(void)
{
    if (expand_extra_max == 0) {
        return;
    }
    expand_expr = NULL;
    for (int i = 0; i < expand_extra_count; i++) {
        expand_cur_sym = &expand_extra_symbols[i];
        expand_cur_index = (unsigned)(i + expand_symbol_count);
        expand_apply_polarity();
        expand_build_frame(NULL, 1);
    }
}

/* Convert the whole CB file: every design section is bound to its device
 * and its symbols and frames are written to CM<pid>. */
static void process_file(void)
{
    char alt_name[ALT_SIZE + 1];
    int more;
    expand_open_library();
    phase_temp_name(expand_out_name, expand_args[3]);
    expand_out = expand_open_file(expand_out_name, "wb");
    if (expand_out == NULL) {
        expand_fatal_error(ERR_OPEN, expand_out_name);
    }
    do {
        expand_read_section_header();
        for (;;) {
            expand_load_device(expand_device_name);
            expand_alloc_usage_maps();
            if (!expand_bind_symbols(alt_name, sizeof(alt_name)) || strcmp(alt_name, expand_device_name) == 0) {
                break;
            }
            snprintf(expand_device_name, CF_HDR_EXTRA - CF_HDR_DEVICE, "%s", alt_name);
        }
        expand_write_section_header();
        expand_set_polarity_states();
        expand_write_symbols();
        expand_expr = NULL;
        g_line_known = 1;
        more = process_equations();
        write_preset_frames();
        if (more) {
            expand_write_marker(FRAME_END_SECTION);
        }
    } while (more);
    phase_close(&g_rt, expand_lib_file);
    expand_lib_file = NULL;
    expand_write_marker(FRAME_END_FILE);
    if (fflush(expand_out) != 0) {
        expand_fatal_error(ERR_DISK_WRITE, expand_out_name);
    }
    phase_close(&g_rt, expand_out);
    expand_out = NULL;
    phase_close(&g_rt, expand_in);
    expand_in = NULL;
}

/* With -l the messages are also appended to <design>.lst */
static void open_listing(const char *source)
{
    char *name = phase_file_name(&g_rt, phase_dir_prefix(&g_rt, expand_args[5]), source, ".lst");
    g_list_name = phase_file_name(&g_rt, phase_dir_prefix(&g_rt, expand_args[5]), source, ".lst");
    g_list_file = expand_open_file(name, "at");
    if (g_list_file == NULL) {
        g_listing = 0;
        expand_report_error(ERR_OPEN, name);
        return;
    }
    phase_printf(&g_rt, g_list_file, "\n");
    if (fflush(g_list_file) != 0) {
        expand_fatal_error(ERR_DISK_WRITE, name);
    }
}

/* Option letters.  'k', 'v' and 'z' swallow the following
 * letter. */
static void parse_flags(const char *p)
{
    while (*p != '\0') {
        switch (*p) {
        case 'k':
            p++;
            if (*p == 'x') {
                expand_keep_xor = 1;
            }
            break;
        case 'l':
            g_listing = 1;
            break;
        case 'q':
            g_long_messages = 1;
            break;
        case 'v':
            g_rt.console = 1;
            p++;
            break;
        case 'z':
            p++;
            break;
        default:
            break;
        }
        if (*p != '\0') {
            p++;
        }
    }
}

/* Set every phase variable to its initial value, so that the phase can run
 * more than once in one process. */
static void reset_state(void)
{
    memset(expand_args, 0, sizeof(expand_args));
    expand_mode = NULL;
    phase_begin(&g_rt);
    expand_error_count = 0;
    g_listing = 0;
    g_long_messages = 0;
    expand_keep_xor = 0;
    g_list_file = NULL;
    g_list_name = NULL;
    g_line_known = 0;

    expand_in = NULL;
    expand_out = NULL;
    strcpy(expand_in_name, "CBXXXXXX");
    strcpy(expand_out_name, "CMXXXXXX");
    memset(expand_header, 0, sizeof(expand_header));
    expand_header_extra = NULL;
    expand_device_name = (char *)expand_header + CF_HDR_DEVICE;

    expand_symbols = NULL;
    expand_symbol_count = 0;
    expand_symbol_alloc = 0;
    expand_extra_symbols = NULL;
    expand_extra_count = 0;
    expand_extra_max = 0;

    expand_cur_sym = NULL;
    expand_cur_index = 0;
    expand_expr = NULL;
    expand_line = 0;
    expand_invert_output = 0;
    expand_min_level = 0;

    expand_input_reset();
    expand_device_reset();
    expand_tree_reset();
    expand_frame_reset();
}

/* Run the phase: options, listing file, conversion of CB<pid> into
 * CM<pid>.  Any error abandons the phase; on success CB<pid> is deleted. */
static void run_phase(void)
{
    g_rt.last_tick = time(NULL);
    parse_flags(expand_args[6]);
    if (g_listing) {
        open_listing(expand_args[5]);
    }
    process_file();
    if (g_listing) {
        if (fflush(g_list_file) != 0) {
            expand_fatal_error(ERR_DISK_WRITE, phase_file_name(&g_rt, phase_dir_prefix(&g_rt, expand_args[5]), expand_args[5], ".lst"));
        }
        phase_close(&g_rt, g_list_file);
        g_list_file = NULL;
        g_listing = 0;
    }
    g_line_known = 0;
    if (g_rt.progress_column != 0) {
        phase_printf(&g_rt, stdout, "\n");
    }
    if (expand_error_count != 0) {
        expand_fatal_error(-1, "");
    }
    remove_input_file();
}

/* Phase entry point.  argv[2] is the device library, argv[3] the process
 * id, argv[5] the design file, argv[6] the option letters and argv[7] the
 * mode ("1" for virtual devices).  Returns 0 on success. */
int expand_run(int argc, char **argv)
{
    volatile int result = 1;

    reset_state();
    for (int i = 0; i < 10; i++) {
        expand_args[i] = (i < argc && argv != NULL && argv[i] != NULL) ? argv[i] : "";
    }
    expand_mode = expand_args[7];

    if (setjmp(g_abort_jmp) == 0) {
        if (argc < 3) {
            expand_fatal_error(-1, "");
        }
        run_phase();
        result = 0;
    }

    fflush(stdout);
    phase_end(&g_rt);
    return result;
}
