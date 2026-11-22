/*
 * fusemap.c - CUPL output generation phase (last compiler phase, fusemap_run).
 *
 * The phase reads the intermediate file CC<pid> written by cuplm (a header,
 * the symbol table and one product-term list per output), loads the record
 * of the target device from the device library, assigns the product-term
 * lists to the OR arrays of the device and writes the requested files:
 *
 *   fusemap.c       messages, output (src/shared/phase.c), intermediate
 *                        file, device library, OR arrays, driver
 *   jedec.c   fuse map and JEDEC file (.jed)
 *   atv5000.c   product-term placement of the ATV5000, EP1800 check
 *   doc.c   documentation file (.doc): expanded product terms,
 *                        symbol table, fuse plot, chip diagram; HL file (.hl)
 *   abs.c   absolute file for the simulator (.abs), .sim file
 *   pla.c   Berkeley PLA files (.pla, .tt2), fitter input
 *   prom.c    PROM map, ASCII-hex file (.hex), POF file (.pof)
 *   netlist.c   PALASM (.pds), PDIF symbol (.pdf), EDIF (.edf)
 *
 * Notes on the implementation:
 *   - The polarity of an output is taken from its product-term list as cuplb
 *     wrote it, and feedback pins are resolved after the DeMorgan forms have
 *     been chosen; see select_output_nodes() and rebuild_output_table().
 *   - Fixed-size buffers and tables are bounds-checked; fuses outside the
 *     fuse map are ignored.
 *   - Status messages go to stdout.
 *   - There is no Xilinx XNF / OED netlist writer; the 'zx' option only
 *     selects the fitter command file and, for devices other than "ga...",
 *     a warning.
 */
#include "fusemap.h"
#include "locate.h"
#include "strutil.h"
#include "cupl_phases.h"

struct cuplc_state g;

/* Message texts, indexed by error number (ERR_xxx) */
static const char *const k_error_text[] = {
    "program error:  ",
    "could not open:  ",
    "invalid file:  ",
    "invalid library access key",
    "invalid library interface",
    "bad library file:  ",
    "excessive number of product terms:  ",
    "invalid download format:  ",
    "pin can not be used as input:  ",
    "header name undefined, using no_name",
    "disk write error:  ",
    "out of memory:  ",
    "disk read error, unexpected end of file:  ",
    "conflicting usage of pinnode:  ",
    "unknown extension encountered:  ",
    "invalid local feedback ",
    "exceeded number of expander product terms",
    "global feedback in local product term: ",
    "couldn't find XILINX symbol: ",
    "couldn't map CUPL symbol to XILINX symbol: ",
    "couldn't find CUPL macro symbol: ",
    "Error found in OED file  ",
    "unsupported extension: ",
    "incorrect number of variables in DECLARE statement: ",
    "too many XOR gates defined for output: ",
    "feature not supported by this device: "
};
#define ERROR_TEXT_COUNT ((int)(sizeof(k_error_text) / sizeof(k_error_text[0])))

/* -------------------------------------------------------------------------
 * Runtime (src/shared/phase.c)
 * ------------------------------------------------------------------------- */
static void out_of_memory(const char *context)
{
    fatal_error(ERR_OUT_OF_MEMORY, context);
}

static struct phase_rt g_rt = {
    .code_suffix = "cc",
    .messages = k_error_text,
    .message_count = ERROR_TEXT_COUNT,
    .text_size = 2048,
    .out_of_memory = out_of_memory,
};

/* Formatted output to a file or the status output; returns the number of
 * characters written */
int stream_printf(FILE *fp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int len = phase_vprintf(&g_rt, fp, fmt, ap);
    va_end(ap);
    return len;
}

/* stream_printf() to the status output */
void status_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    phase_vprintf(&g_rt, stdout, fmt, ap);
    va_end(ap);
}

/* Diagnostic trace output.  In console mode a newline is added, unless the
 * text is a single character. */
void trace_printf(const char *fmt, ...)
{
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    phase_trace(&g_rt, text);
}

void *alloc_tracked(size_t size, const char *context)
{
    return phase_alloc(&g_rt, size, context);
}

void *alloc_zeroed(size_t count, size_t size, const char *context)
{
    return phase_alloc(&g_rt, count * size, context);
}

void free_tracked(void *p)
{
    phase_free(p);
}

char *dup_string(const char *s, const char *context)
{
    return phase_strdup(&g_rt, s, context);
}

/* Open a file, looking up existing files without regard to case.  Text files
 * named *.jed are written with CR LF line ends; all other files are written
 * as they are. */
FILE *open_file(const char *path, const char *mode)
{
    return phase_open(&g_rt, path, mode);
}

void close_file(FILE *fp)
{
    phase_close(&g_rt, fp);
}

void out_putc(int c, FILE *fp)
{
    phase_putc(&g_rt, c, fp);
}

void out_puts(const char *s, FILE *fp)
{
    phase_puts(&g_rt, s, fp);
}

void out_printf(FILE *fp, const char *fmt, ...)
{
    char text[16384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    phase_puts(&g_rt, text, fp);
}

int read_char(FILE *fp)
{
    return phase_read_char(fp);
}

void remove_file(const char *path)
{
    phase_remove(path, 1);
}

int file_exists(const char *path)
{
    char resolved[PATH_MAX];
    return lincupl_resolve_ci_path(path, resolved, sizeof(resolved)) == 0;
}

void check_write(FILE *fp, const char *name)
{
    fflush(fp);
    if (ferror(fp)) {
        fatal_error(ERR_DISK_WRITE, name);
    }
}

/* Directory part of a path including the separator, or "" */
char *source_dir(const char *path)
{
    return phase_dir_prefix(&g_rt, path);
}

/* dir + base name of path (without "./" and a trailing ".pld") + ext */
char *make_path(const char *dir, const char *path, const char *ext)
{
    return phase_file_name(&g_rt, dir, path, ext);
}

/* ctime() of the phase start time, including its newline */
const char *creation_time_text(void)
{
    static char text[32];
    time_t t = (time_t)g.start_time;
    const char *s = ctime(&t);
    if (s != NULL) {
        snprintf(text, sizeof(text), "%s", s);
    } else {
        strcpy(text, "Thu Jan 01 00:00:00 1970\n");
    }
    return text;
}

/* -------------------------------------------------------------------------
 * Error reporting
 * ------------------------------------------------------------------------- */

/* Remove the intermediate file and a left-over <design>.oed */
static void remove_temp_files(void)
{
    char oed[PATH_MAX];
    remove_file(g.temp_name);
    snprintf(oed, sizeof(oed) - 4, "%s", g.args[5] != NULL ? g.args[5] : "");
    char *dot = strrchr(oed, '.');
    if (dot != NULL) {
        *dot = '\0';
    }
    strcat(oed, ".oed");
    if (file_exists(oed)) {
        remove_file(oed);
    }
}

/* An absolute file that was being written is not left behind */
static void remove_abs_file(void)
{
    if ((g.options & OPT_ABSOLUTE) && g.abs_name != NULL) {
        remove_file(g.abs_name);
    }
}

/* Print an error message: "[nnnncc] text" or, with the 'q' option on the
 * console, "<file>(line) : Error [nnnncc] :text" */
static void print_error(FILE *fp, const char *fmt, int code, const char *a, const char *b)
{
    if (!g.quiet_format || fp == g.lst_fp) {
        stream_printf(fp, "[%04d%s] ", code, g_rt.code_suffix);
        if (g.line_ptr != NULL) {
            stream_printf(fp, "line(%d), ", *g.line_ptr);
        }
    } else {
        stream_printf(fp, "%s(%d) : Error [%04d%s] :", g.lst_name, g.line_ptr != NULL ? *g.line_ptr : 0, code,
                      g_rt.code_suffix);
    }
    stream_printf(fp, "%s", phase_message(&g_rt, code));
    stream_printf(fp, fmt, a, b);
}

/* Print a warning in the same two formats as print_error() */
static void print_warning(FILE *fp, const char *fmt, int code, const char *a, const char *b)
{
    if (!g.quiet_format || fp == g.lst_fp) {
        stream_printf(fp, "[%04d%s] ", code, g_rt.code_suffix);
        if (g.line_ptr != NULL) {
            stream_printf(fp, "line(%d), ", *g.line_ptr);
        }
        stream_printf(fp, "Please note: %s", phase_message(&g_rt, code));
    } else {
        stream_printf(fp, "%s(%d) : Warning [%04d%s] : %s", g.lst_name, g.line_ptr != NULL ? *g.line_ptr : 0,
                      code, g_rt.code_suffix, phase_message(&g_rt, code));
    }
    stream_printf(fp, fmt, a, b);
}

/* Print a message on the console and, with the 'l' option, in the
 * listing file. */
static void emit_message(int code, const char *fmt,
                         void (*print)(FILE *, const char *, int, const char *, const char *),
                         const char *a, const char *b)
{
    print(stdout, fmt, code, a, b);
    if (g.options & OPT_LIST) {
        print(g.lst_fp, fmt, code, a, b);
    }
}

/* Report an error and continue */
void report_error(int code, const char *detail)
{
    g.error_count++;
    emit_message(code, "%s\n", print_error, detail, "");
}

/* Report a warning; it does not count as an error */
void report_warning(int code, const char *detail)
{
    emit_message(code, "%s\n", print_warning, detail, "");
}

/* Error naming a symbol, with its extension if it has one */
void report_symbol_error(int code, const symbol_t *sym)
{
    g.error_count++;
    if (SYM_EXT(sym)[0] == ' ' || SYM_EXT(sym)[0] == '\0') {
        emit_message(code, "%s\n", print_error, sym->name, "");
    } else {
        emit_message(code, "%s.%s\n", print_error, sym->name, SYM_EXT(sym));
    }
}

static void append_symbol_name(char *dst, size_t cap, const symbol_t *sym)
{
    size_t used = strlen(dst);
    snprintf(dst + used, cap - used, "%s", sym->name);
    if (SYM_EXT(sym)[0] != ' ' && SYM_EXT(sym)[0] != '\0') {
        used = strlen(dst);
        snprintf(dst + used, cap - used, ".%s", SYM_EXT(sym));
    }
}

/* Error naming a signal path "from a to b" */
void report_from_to_error(int code, const symbol_t *from, const symbol_t *to)
{
    char text[160];
    g.error_count++;
    strcpy(text, "from ");
    append_symbol_name(text, sizeof(text), from);
    snprintf(text + strlen(text), sizeof(text) - strlen(text), " to ");
    append_symbol_name(text, sizeof(text), to);
    emit_message(code, "%s\n", print_error, text, "");
}

/* Report an error (unless code is negative) and abandon the phase */
void fatal_error(int code, const char *detail)
{
    if ((int16_t)code >= 0) {
        report_error((int16_t)code, detail);
    }
    abort_phase();
}

/* Remove the temporary and partial output files and return to fusemap_run() */
void abort_phase(void)
{
    remove_temp_files();
    remove_abs_file();
    longjmp(g.abort_jmp, 1);
}

/* -------------------------------------------------------------------------
 * Intermediate file
 * ------------------------------------------------------------------------- */

/* The file is named "CC" + the last six characters of
 * the process id argument */
static FILE *open_intermediate(void)
{
    phase_temp_name(g.temp_name, g.args[3] != NULL ? g.args[3] : "");
    FILE *fp = open_file(g.temp_name, "rb");
    if (fp == NULL) {
        fatal_error(ERR_OPEN, g.temp_name);
    }
    return fp;
}

/* Read the fixed header and the strings that follow it.  Returns 0 if the
 * magic number is wrong. */
static int read_header(void)
{
    if (!cf_read_header(g.temp_fp, g.hdr)) {
        fatal_error(ERR_DISK_READ, g.temp_name);
    }
    if (rd16(g.hdr + CF_HDR_MAGIC) != CF_MAGIC_CM) {
        report_error(ERR_INVALID_FILE, "intermediate file");
        return 0;
    }
    size_t extra = cf_header_extra(g.hdr);
    if (extra != 0) {
        g.hdr_strings = alloc_tracked(extra + 1, "symbol table");
        g.hdr_strings_len = extra;
        if (fread(g.hdr_strings, extra, 1, g.temp_fp) != 1) {
            fatal_error(ERR_DISK_READ, g.temp_name);
        }
    }
    return 1;
}

/* Read the header and the symbol table of the next design */
static void load_design(void)
{
    if (!read_header()) {
        fatal_error(-1, "");
    }
    g.nsyms = (int)rd16(g.hdr + CF_HDR_SYMBOLS);
    g.device_name = (char *)g.hdr + CF_HDR_DEVICE;
    g.design_name = (char *)g.hdr + CF_HDR_DESIGN;
    g.hdr[CF_HDR_DEVICE - 1] = '\0';
    for (char *p = g.design_name; *p != '\0'; p++) {
        if (*p == ' ' || (*p >= '\t' && *p <= '\r')) {
            *p = '\0';
            break;
        }
    }
    g.format_text = (char *)g.hdr + CF_HDR_FORMAT;
    g.hdr[CF_HDR_DESIGN - 1] = '\0';
    g.syms = alloc_zeroed((size_t)g.nsyms + 1, sizeof(symbol_t), "symbol table");
    for (int i = 0; i < g.nsyms; i++) {
        cf_symbol_t rec;
        symbol_t *s = &g.syms[i];
        if (!cf_read_symbol(g.temp_fp, &rec, CF_SYMBOL_SIZE)) {
            fatal_error(ERR_DISK_READ, g.temp_name);
        }
        memcpy(s->name, rec.name, SYM_NAME_SIZE);
        memcpy(SYM_EXT(s), rec.ext, SYM_EXT_SIZE);
        s->pin = rec.pin;
        s->type = rec.type;
        s->flags = rec.flags;
        memcpy(s->b2e, rec.attr, 4);
        s->b32 = rec.polarity;
        s->b33 = rec.attr2;
        s->pin_index = rec.pin_index;
        s->node_index = rec.node_index;
        s->or_index = rec.or_index;
        s->ext_code = rec.ext_code;
        s->eq = NULL;
    }
    for (int i = 0; i < g.nsyms; i++) {
        if (g.syms[i].b32 != 0) {
            g.syms[i].flags ^= SYMF_20;
        }
    }
}

/* Read one product-term list.  Returns 1 if a list was
 * read, 0 at the end of the design and -1 at the end of the file. */
static int read_equation(equation_t *eq)
{
    cf_frame_t head;
    if (!cf_read_frame(g.temp_fp, &head)) {
        fatal_error(ERR_DISK_READ, g.temp_name);
    }
    eq->flags = head.flags;
    eq->or_array = head.or_array;
    eq->nvars = head.nvars;
    eq->nterms = head.nrows;
    eq->w8 = head.has_xor;
    eq->max_terms = head.max_terms;
    if (eq->flags & EQ_END_OF_DEVICE) {
        return 0;
    }
    if (eq->flags & EQ_END_OF_FILE) {
        return -1;
    }
    eq->group_words = cf_plane_words(eq->nvars);
    eq->term_words = eq->group_words * 3;
    eq->total_words = eq->nterms * eq->term_words;
    /* one spare term so that an empty list still has a first term to look at */
    eq->terms = alloc_zeroed((size_t)eq->total_words + (size_t)eq->term_words, sizeof(uint16_t), "frame allocation");
    if (cf_read_words(g.temp_fp, eq->terms, (size_t)eq->total_words) != (size_t)eq->total_words) {
        fatal_error(ERR_DISK_READ, g.temp_name);
    }
    return 1;
}

/* First symbol at or after 'first' whose bit is set in a group */
static int first_set_bit(int first, int nvars, const uint16_t *group)
{
    for (int i = first; i < nvars; i++) {
        if (term_bit(group, i)) {
            return i;
        }
    }
    return -1;
}

/* Look up a symbol by name and extension (first two skipped) */
symbol_t *find_symbol(const char *name, const char *ext)
{
    for (int i = 2; i < g.nsyms; i++) {
        symbol_t *s = &g.syms[i];
        if (strcmp(s->name, name) == 0 && strcmp(SYM_EXT(s), ext) == 0 && s->b32 == 0) {
            return s;
        }
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Device library
 * ------------------------------------------------------------------------- */

/* Open the device library and check its header: DL_OK if it is usable,
 * DL_CANNOT_OPEN or a result of dl_read_header() */
static int open_library(const char *path)
{
    g.lib.fp = open_file(path, "rb");
    if (g.lib.fp == NULL) {
        return DL_CANNOT_OPEN;
    }
    g.lib.header = alloc_tracked(DL_HEADER_SIZE, "initialization");
    return dl_read_header(g.lib.fp, g.lib.header);
}

/* Open the device library named on the command line; any problem is fatal */
static void open_device_library(void)
{
    g.lib.rec = NULL;
    g.lib.header = NULL;
    switch (open_library(g.args[2] != NULL ? g.args[2] : "")) {
    case DL_OK:
        break;
    case DL_CANNOT_OPEN:
        fatal_error(ERR_OPEN, "cupl.dl");
    case DL_NO_HEADER:
    case DL_BAD_MAGIC:
        fatal_error(ERR_BAD_LIBRARY, "cupl.dl");
    case DL_BAD_KEY:
        fatal_error(ERR_LIB_ACCESS_KEY, "");
    default:
        fatal_error(1001, "");
    }
}

/* Read the record the directory entry points at */
static int read_device_record(void)
{
    if (g.lib.fp == NULL) {
        return 0;
    }
    uint32_t size = dl_image_size(g.lib.dir_entry);
    if (!dl_seek_image(g.lib.fp, g.lib.dir_entry)) {
        return 0;
    }
    /* The section offsets are not checked; the spare space after the record
     * keeps a short or damaged record from being read past its end. */
    g.lib.rec_size = size;
    g.lib.rec = alloc_tracked((size_t)size + 0x10000, "device library processing");
    return dl_read_device_image(g.lib.fp, g.lib.dir_entry, g.lib.rec, size) > 0;
}

/* Load the record of the target device and close the library.  Devices of
 * type 8 and virtual devices skip the product-term limit check (args[7]). */
static void load_device(void)
{
    if (!read_device_record()) {
        fatal_error(ERR_BAD_LIBRARY, (const char *)g.lib.dir_entry);
    }
    if (device_type() == DEVT_8 || device_type() == DEVT_VIRTUAL) {
        g.args[7] = "1";
    }
    close_file(g.lib.fp);
    g.lib.fp = NULL;
}

/* Node record with the same array, number and extension as the
 * symbol's node but of the given kind */
int find_node_variant(int kind, const symbol_t *sym)
{
    const uint8_t *own = node_rec(sym->node_index);
    for (int i = 0; i < nnodes(); i++) {
        const uint8_t *n = node_rec(i);
        if (rd16(n + NODE_ARRAY) == rd16(own + NODE_ARRAY) && rd16(n + NODE_NUMBER) == rd16(own + NODE_NUMBER)
            && rd8(n + NODE_KIND) == (unsigned)kind && rd8(n + NODE_EXT) == rd8(own + NODE_EXT)) {
            return i;
        }
    }
    return -1;
}

/* State of a search through the extension pair section */
typedef struct {
    int second;         /* last record found by next_other_pair() */
    int first;          /* last record found by next_own_pair() */
    unsigned ext;       /* extension, number and array of the symbol */
    unsigned number;
    unsigned array;
    unsigned group;     /* group and selection of the record 'first' */
    unsigned select;
} pair_search_t;

/* Next record describing the symbol's own macrocell input */
static int next_own_pair(pair_search_t *st)
{
    int count = (int)sec_count(SEC_EXT_PAIR);
    for (int i = st->first < 0 ? 0 : st->first + 1; i < count; i++) {
        const uint8_t *p = pair_rec(i);
        if (rd16(p + PAIR_NUMBER) == st->number && rd16(p + PAIR_EXT) == st->ext && rd16(p + PAIR_W0) == st->array) {
            st->first = i;
            st->select = rd8(p + PAIR_SELECT);
            st->group = rd16(p + PAIR_GROUP);
            return i;
        }
    }
    return -1;
}

/* Next record of the same group, other selection, that names the
 * wanted source */
static int next_other_pair(pair_search_t *st, unsigned ext, unsigned number)
{
    int count = (int)sec_count(SEC_EXT_PAIR);
    for (int i = st->second < 0 ? 0 : st->second + 1; i < count; i++) {
        const uint8_t *p = pair_rec(i);
        if (rd16(p + PAIR_GROUP) == st->group && rd8(p + PAIR_SELECT) != st->select && rd16(p + PAIR_NUMBER) == number
            && rd16(p + PAIR_EXT) == ext) {
            st->second = i;
            return i;
        }
    }
    return -1;
}

/* Find the extension pair record that connects a macrocell input
 * of 'sym' to the signal (number, ext).  Returns the record index or -1. */
int find_extension_pair(const symbol_t *sym, unsigned number, unsigned ext)
{
    pair_search_t st;
    const uint8_t *src = NULL;
    if (sym == NULL || (sym->node_index < 0 && sym->pin_index < 0)) {
        return -1;
    }
    memset(&st, 0, sizeof(st));
    st.first = -1;
    st.second = -1;
    st.group = 0xffff;
    if (sym->pin_index >= 0) {
        unsigned e = pin8(sym->pin_index, PIN_EXT);
        if (e == 0x33 || e == 0x34 || e == 0x17 || e == 0x18) {
            src = pin_rec(sym->pin_index);
        }
    }
    if (src == NULL) {
        if (sym->node_index >= 0) {
            src = node_rec(sym->node_index);
        } else if (sym->pin_index >= 0) {
            src = pin_rec(sym->pin_index);
        }
    }
    if (src != NULL) {
        /* pin and node records start with the same three fields */
        st.ext = rd8(src + 4);
        st.number = rd16(src + 2);
        st.array = rd16(src);
    }
    for (;;) {
        if (next_own_pair(&st) < 0) {
            return -1;
        }
        if (next_other_pair(&st, ext, number) >= 0) {
            return st.second;
        }
    }
}

/* -------------------------------------------------------------------------
 * Symbol preparation
 * ------------------------------------------------------------------------- */

/* A pin symbol without an equation whose pin also has an output (pin kinds
 * 7, 0x0b, 0x10, 0x17) gets the node of that output.  Pins of kind 0x0b
 * without a usable node cannot be inputs. */
static void check_feedback_pins(void)
{
    for (int i = 0; i < g.nsyms; i++) {
        symbol_t *s = &g.syms[i];
        if (s->pin_index >= 0 && s->eq == NULL && g.output_sym_by_pin[s->pin] != NULL) {
            unsigned kind = pin8(s->pin_index, PIN_KIND);
            if (kind == 7 || kind == 0x10 || kind == 0x0b || kind == 0x17) {
                s->node_index = g.output_sym_by_pin[s->pin]->node_index;
            }
        }
        if (s->pin_index >= 0 && (s->node_index < 0 || node8(s->node_index, NODE_B24) == 2) && (s->flags & SYMF_INPUT)
            && pin8(s->pin_index, PIN_KIND) == 0x0b) {
            report_error(ERR_PIN_NOT_INPUT, s->name);
        }
    }
}

/* Pick the node record that matches the polarity of each output and build
 * the table of output symbols by pin number.
 *
 * Symbols with b32 != 0 (DeMorgan candidates) use the same mapping as all
 * others, because cuplb stores the final polarity in the list's flag word.
 * The feedback pins are checked only after the DeMorgan forms have been
 * chosen, see rebuild_output_table(). */
static void select_output_nodes(void)
{
    int limit = pin_number_limit();
    g.sym_by_pin_count = limit + 1;
    g.output_sym_by_pin = alloc_zeroed((size_t)limit + 1 + 0x10000, sizeof(symbol_t *), "creating bit map");
    for (int i = 0; i < g.nsyms; i++) {
        symbol_t *s = &g.syms[i];
        if (s->node_index < 0 || s->eq == NULL) {
            continue;
        }
        int active_low = (s->eq->flags & EQ_ACTIVE_LOW) != 0;
        unsigned kind = node8(s->node_index, NODE_KIND);
        if (kind == 3) {
            report_error(1015, "");
        } else if (kind == 6) {
            if (active_low) {
                s->node_index = (int16_t)find_node_variant(7, s);
            }
        } else if (kind == 7 && !active_low) {
            s->node_index = (int16_t)find_node_variant(6, s);
        }
        if (s->node_index >= 0 && s->eq != NULL && node8(s->node_index, NODE_B25) != 0) {
            g.output_sym_by_pin[s->pin] = s;
        }
    }
}

/* Once the DeMorgan forms are chosen, rebuild the table from the surviving
 * lists and only then propagate the node of an output to its feedback pin. */
static void rebuild_output_table(void)
{
    int limit = pin_number_limit();
    for (int i = 0; i <= limit; i++) {
        g.output_sym_by_pin[i] = NULL;
    }
    for (int i = 0; i < g.nsyms; i++) {
        symbol_t *s = &g.syms[i];
        if (s->node_index >= 0 && s->eq != NULL && node8(s->node_index, NODE_B25) != 0 && (int)s->pin <= limit) {
            g.output_sym_by_pin[s->pin] = s;
        }
    }
    check_feedback_pins();
}

/* Exchange the names and the b32/b33 marks of two symbols */
static void swap_symbol_names(symbol_t *a, symbol_t *b)
{
    char name[SYM_NAME_SIZE + SYM_EXT_SIZE + 2];
    /* only the name up to its NUL is copied, which runs into the extension
     * when the name fills its field */
    size_t na = strlen(a->name) + 1;
    size_t nb = strlen(b->name) + 1;
    memcpy(name, a->name, na);
    memcpy(a->name, b->name, nb);
    memcpy(b->name, name, na);
    uint8_t t = a->b32;
    a->b32 = b->b32;
    b->b32 = t;
    t = a->b33;
    a->b33 = b->b33;
    b->b33 = t;
}

static void retire_symbol(symbol_t *s, int drop_eq)
{
    s->type = (uint8_t)((s->type & 0xf0) | 3);
    s->or_index = -1;
    if (drop_eq) {
        s->eq = NULL;
    }
}

/* Of an output and its DeMorgan twin (name$demorg) keep the one with fewer
 * product terms; turn the other into an intermediate variable.
 *
 * The walk keeps a position and an iteration count separately.  After an
 * exchange the position continues behind the twin, so when the twin is not
 * the immediately preceding entry some entries are visited twice and the
 * same number of entries at the end of the table is never looked at.  This
 * is deliberate: which form survives, and so the generated files, depends
 * on it. */
static void choose_demorgan_forms(void)
{
    int pos = 2;
    for (int count = 2; count < g.nsyms && pos < g.nsyms; count++, pos++) {
        symbol_t *s = &g.syms[pos];
        if (s->b32 != 2) {
            continue;
        }
        char name[SYM_NAME_SIZE + SYM_EXT_SIZE + 2];
        snprintf(name, sizeof(name), "%s", s->name);
        char *dollar = strrchr(name, '$');
        if (dollar != NULL && strcmp(dollar, "$demorg") == 0) {
            *dollar = '\0';
        }
        symbol_t *twin = find_symbol(name, SYM_EXT(s));
        if (twin != NULL && s->eq != NULL && twin->eq != NULL) {
            if (s->eq->nterms < twin->eq->nterms) {
                swap_symbol_names(twin, s);
                retire_symbol(twin, twin->b33 != 0);
                pos = sym_index(twin) + 1;
            } else {
                retire_symbol(s, s->b33 != 0);
            }
        }
    }
    pos = 2;
    for (int count = 2; count < g.nsyms && pos < g.nsyms; count++, pos++) {
        symbol_t *s = &g.syms[pos];
        if (s->b33 != 5 || s->b32 == 2) {
            continue;
        }
        symbol_t *twin = find_symbol(s->name, SYM_EXT(s)[0] == 'd' ? "t" : "d");
        if (twin == NULL || s->eq == NULL || twin->eq == NULL) {
            continue;
        }
        if (s->eq->nterms < twin->eq->nterms) {
            retire_symbol(twin, 1);
            twin->b32 = 2;
            twin->b33 = 5;
            s->b32 = 0;
            s->b33 = 0;
        } else {
            swap_symbol_names(twin, s);
            retire_symbol(s, 1);
            s->b32 = 2;
            s->b33 = 5;
            twin->b32 = 0;
            twin->b33 = 0;
            pos = sym_index(twin);
        }
    }
    for (int i = 2; i < g.nsyms; i++) {
        symbol_t *s = &g.syms[i];
        if (strrchr(s->name, '$') != NULL) {
            retire_symbol(s, 1);
        }
    }
}

/* -------------------------------------------------------------------------
 * OR arrays
 * ------------------------------------------------------------------------- */

/* Hang every product-term list on the OR array of its output */
static void assign_or_arrays(void)
{
    g.nor_arrays = (int)attr16(ATTR_NOR_ARRAYS);
    g.or_arrays = alloc_zeroed((size_t)g.nor_arrays + 1, sizeof(or_array_t), "creating bit map");
    for (int i = 0; i < g.nsyms; i++) {
        symbol_t *s = &g.syms[i];
        if (s->or_index < 0 || s->eq == NULL) {
            continue;
        }
        s->eq->next = NULL;
        s->eq->symbol = i;
        unsigned array = node16(s->or_index, NODE_ARRAY);
        if ((int)array >= g.nor_arrays) {
            report_error(1014, "");
            continue;
        }
        or_array_t *oa = &g.or_arrays[array];
        s->eq->or_array = (uint16_t)array;
        if (oa->eq == NULL) {
            oa->eq = s->eq;
            oa->nterms = s->eq->nterms;
            oa->sym = s;
        }
        /* only the next list on the same array is chained to this one */
        for (int j = i + 1; j < g.nsyms; j++) {
            symbol_t *t = &g.syms[j];
            if (t->or_index >= 0 && t->eq != NULL && node16(t->or_index, NODE_ARRAY) == array) {
                s->eq->next = t->eq;
                t->eq->next = NULL;
                t->eq->or_array = (uint16_t)array;
                oa->nterms += t->eq->nterms;
                break;
            }
        }
    }
}

/* Two product terms have the same inputs */
static int same_product(const uint16_t *care_a, const uint16_t *val_a, const uint16_t *care_b,
                        const uint16_t *val_b, int group_words)
{
    if ((care_b[0] & TERM_DELETED) || (care_a[0] & TERM_DELETED)) {
        return 0;
    }
    for (int i = 1; i < group_words; i++) {
        if (care_a[i] != care_b[i] || val_a[i] != val_b[i]) {
            return 0;
        }
    }
    return 1;
}

static void or_groups(uint16_t *dst, const uint16_t *src, int group_words)
{
    for (int i = 1; i < group_words; i++) {
        dst[i] |= src[i];
    }
}

static void copy_term(uint16_t *dst, const uint16_t *src, int term_words)
{
    memmove(dst, src, (size_t)term_words * sizeof(uint16_t));
}

/* Remove the terms of 'b' that also occur in 'a'; the copy in 'a'
 * then drives both outputs.  A removed term is replaced by the last term of
 * 'b', whose outputs are also added to the term of 'a' (kept as is). */
static void merge_two_lists(equation_t *a, equation_t *b, or_array_t *oa)
{
    int gw = a->group_words;
    int tw = a->term_words;
    for (int i = 0; i < a->nterms; i++) {
        if (TERM_CARE(a, i)[0] & TERM_DELETED) {
            continue;
        }
        if (b->nterms == 0) {
            return;
        }
        uint16_t *cur = b->terms;
        uint16_t *last = b->terms + (size_t)(b->nterms - 1) * (size_t)tw;
        for (int j = 0; j < b->nterms; j++) {
            if (same_product(TERM_CARE(a, i), TERM_VAL(a, i), cur + gw, cur + 2 * gw, gw)) {
                oa->nterms--;
                or_groups(TERM_OUT(a, i), cur, gw);
                copy_term(cur, last, tw);
                cur[0] |= 1;
                last -= tw;
                b->nterms--;
                if (b->nterms != 0) {
                    last[gw] = 0;
                }
                or_groups(TERM_OUT(a, i), cur, gw);
                if (node8(oa->sym->or_index, NODE_OR_WIDTH) != 0) {
                    g.merged_terms++;
                }
            }
            if (b->nterms == 0) {
                break;
            }
            cur += tw;
        }
    }
    if (node8(oa->sym->or_index, NODE_OR_WIDTH) != 0) {
        g.or_terms_used = oa->nterms;
    }
}

/* Merge identical product terms of lists that share an OR array */
static void merge_shared_terms(void)
{
    g.merged_terms = 0;
    if (device_type() == DEVT_1) {
        return;
    }
    for (int i = 0; i < g.nor_arrays; i++) {
        for (equation_t *a = g.or_arrays[i].eq; a != NULL && a->next != NULL; a = a->next) {
            if (a->nterms == 0) {
                continue;
            }
            for (equation_t *b = a->next; b != NULL; b = b->next) {
                if (b->nterms != 0) {
                    merge_two_lists(a, b, &g.or_arrays[i]);
                }
            }
        }
    }
}

/* OR array whose symbol has the given pin number and no
 * extension (or the .d extension) */
static or_array_t *find_array_by_pin(int pin)
{
    for (int i = 0; i < g.nor_arrays; i++) {
        symbol_t *s = g.or_arrays[i].sym;
        if (s == NULL) {
            /* an unused array has no symbol */
            continue;
        }
        if (s->pin == pin && (s->ext_code == 1 || s->ext_code == 0)) {
            return &g.or_arrays[i];
        }
    }
    return NULL;
}

/* Report conflicting use of a pinnode when the partner pin of the array's
 * node (NODE_B26) has an OR array of its own with extension code 1 */
static void check_shared_node(or_array_t *oa, unsigned b24)
{
    unsigned partner = node8(oa->sym->or_index, NODE_B26);
    if (partner != 0) {
        or_array_t *other = find_array_by_pin((int)partner);
        if (other != NULL && (b24 & 0x0c) == 0x0c && other->sym->ext_code == 1) {
            report_symbol_error(ERR_PINNODE_CONFLICT, oa->sym);
        }
    }
}

/* Number of terms before the one that ends the first half.  The scan does
 * not move past a deleted term; the placement of split arrays depends on
 * it. */
int terms_before_split(const equation_t *eq)
{
    int pos = 0;
    for (int i = 0; i < eq->nterms; i++) {
        unsigned flags = TERM_CARE(eq, pos)[0];
        if (!(flags & TERM_DELETED)) {
            if (flags & TERM_SPLIT) {
                return i;
            }
            pos++;
        }
    }
    return eq->nterms;
}

/* Exchange the two halves of a list for a split OR array.  The scans do not
 * move past a deleted term, as in terms_before_split(). */
static void swap_halves(equation_t *eq)
{
    int gw = eq->group_words;
    int tw = eq->term_words;
    uint16_t *tmp = alloc_zeroed((size_t)eq->total_words + (size_t)tw * 2, sizeof(uint16_t), "frame allocation");
    int seen = 0;
    int src = 0;
    int dst = 0;
    for (int i = 0; i < eq->nterms; i++) {
        unsigned flags = TERM_CARE(eq, src)[0];
        if (flags & TERM_DELETED) {
            continue;
        }
        if (seen) {
            copy_term(tmp + (size_t)dst * tw, TERM_OUT(eq, src), tw);
            tmp[(size_t)dst * tw + gw] |= TERM_MORE;
            dst++;
        }
        if (flags & TERM_SPLIT) {
            seen = 1;
        }
        src++;
    }
    if (seen && dst > 0) {
        uint16_t *care = tmp + (size_t)(dst - 1) * tw + gw;
        *care = (uint16_t)((*care & ~TERM_MORE) | TERM_SPLIT);
    }
    src = 0;
    int i;
    for (i = 0; i < eq->nterms; i++) {
        unsigned flags = TERM_CARE(eq, src)[0];
        if (flags & TERM_DELETED) {
            continue;
        }
        copy_term(tmp + (size_t)dst * tw, TERM_OUT(eq, src), tw);
        tmp[(size_t)dst * tw + gw] |= TERM_MORE;
        if (flags & TERM_SPLIT) {
            break;
        }
        dst++;
        src++;
    }
    if (i < eq->nterms) {
        tmp[(size_t)dst * tw + gw] &= (uint16_t)~TERM_SPLIT;
    }
    int pos = 0;
    for (i = 0; i < eq->nterms; i++) {
        if (!(tmp[(size_t)pos * tw + gw] & TERM_DELETED)) {
            copy_term(TERM_OUT(eq, pos), tmp + (size_t)pos * tw, tw);
            pos++;
        }
    }
    free_tracked(tmp);
}

/* A split OR array holds 'split' terms in its first half; swap the halves
 * if only the other order fits, else report too many product terms */
static void check_split_array(or_array_t *oa, int split)
{
    equation_t *eq = oa->eq;
    if (eq == NULL) {
        return;
    }
    int second = (int)node16(oa->sym->or_index, NODE_MAX_TERMS) - split;
    int count[2] = { 0, 0 };
    int half = 0;
    int pos = 0;
    for (int i = 0; i < eq->nterms; i++, pos++) {
        unsigned flags = TERM_CARE(eq, pos)[0];
        if (flags & TERM_DELETED) {
            continue;
        }
        count[half]++;
        if (flags & TERM_SPLIT) {
            half++;
        }
        if (half == 2) {
            report_error(1013, "");
            half = 1;
        }
    }
    if (split < count[0] || second < count[1]) {
        if (second < count[0] || split < count[1]) {
            if (g.dev_max5 != 1) {
                report_symbol_error(ERR_TOO_MANY_PTERMS, oa->sym);
            }
        } else {
            swap_halves(eq);
        }
    }
}

/* Check that every OR array can hold its product terms */
static void check_or_arrays(void)
{
    g.too_many_terms = 0;
    for (int i = 0; i < g.nor_arrays; i++) {
        or_array_t *oa = &g.or_arrays[i];
        if (oa->sym != NULL && oa->sym->or_index >= 0) {
            unsigned b24 = node8(oa->sym->or_index, NODE_B24);
            if ((b24 & 8) == 8) {
                check_shared_node(oa, b24);
            }
        }
    }
    for (int i = 0; i < g.nor_arrays; i++) {
        or_array_t *oa = &g.or_arrays[i];
        if (oa->sym == NULL || oa->sym->or_index < 0) {
            continue;
        }
        int split = (int)node8(oa->sym->or_index, NODE_SPLIT_TERMS);
        if (split != 0) {
            check_split_array(oa, split);
        }
        unsigned max_terms = node16(oa->sym->or_index, NODE_MAX_TERMS);
        if (device_type() != DEVT_PROM && max_terms != 0 && g.args[7][0] != '1' && (int)max_terms < oa->nterms) {
            if (g.dev_max5 == 1) {
                max5_set_needed(oa->sym->pin, oa->nterms);
            } else {
                if (device_type() == DEVT_IFL) {
                    report_error(ERR_TOO_MANY_PTERMS, "OR Arrays");
                } else {
                    report_symbol_error(ERR_TOO_MANY_PTERMS, oa->sym);
                }
                g.too_many_terms = 1;
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * Driver
 * ------------------------------------------------------------------------- */

/* Set the options from the option letters of the command line */
static void parse_flags(const char *p)
{
    for (; *p != '\0'; p++) {
        switch (*p) {
        case 'a':
        case 's':
            g.options |= OPT_ABSOLUTE;
            break;
        case 'b':
            g.options |= OPT_PLA;
            break;
        case 'c':
            g.options |= OPT_PALASM;
            break;
        case 'd':
            g.options |= OPT_D;
            break;
        case 'f':
            g.options |= OPT_FUSE_PLOT;
            break;
        case 'g':
            g.options |= OPT_G;
            break;
        case 'h':
            g.options |= OPT_HEX;
            break;
        case 'i':
            g.options |= OPT_HL;
            break;
        case 'j':
            g.options |= OPT_JEDEC;
            break;
        case 'k':
            p++;
            if (*p == 's') {
                g.opt_share = 1;
            }
            break;
        case 'n':
            g.options |= OPT_N;
            break;
        case 'p':
            g.options |= OPT_PDIF;
            break;
        case 'q':
            g.quiet_format = 1;
            g.options |= OPT_LIST;
            break;
        case 'l':
            g.options |= OPT_LIST;
            break;
        case 'r':
            g.options |= OPT_NO_MERGE;
            break;
        case 't':
            g.opt_edif = 1;
            break;
        case 'v':
            g_rt.console = 1;
            break;
        case 'x':
            g.options |= OPT_DOC;
            break;
        case 'z':
            p++;
            if (*p == 'q') {
                g.options |= OPT_PLA;
                g.opt_fitter = 1;
            } else if (*p == 'x') {
                g.opt_xnf = 1;
            }
            break;
        default:
            break;
        }
        if (*p == '\0') {
            break;
        }
    }
}

/* The listing file written by the earlier phases is appended to */
static void open_listing(const char *source)
{
    char *name = make_path(source_dir(g.args[5]), source, ".lst");
    g.lst_name = make_path(source_dir(g.args[5]), source, ".lst");
    g.lst_fp = open_file(name, "at");
    if (g.lst_fp == NULL) {
        g.options &= ~(unsigned)OPT_LIST;
        fatal_error(ERR_OPEN, name);
    }
    stream_printf(g.lst_fp, "\n");
    check_write(g.lst_fp, name);
}

/* Download formats requested by the source's FORMAT statement */
static void add_requested_formats(const char *p)
{
    for (; *p != '\0'; p++) {
        if (*p == 'h') {
            g.options |= OPT_HEX;
        } else if (*p == 'i') {
            g.options |= OPT_HL;
        } else if (*p == 'j') {
            g.options |= OPT_JEDEC;
        }
    }
}

/* Does the device support the download format?  Warns if not. */
static int format_supported(unsigned format)
{
    unsigned kind = attr8(ATTR_DOWNLOAD);
    if (format == OPT_JEDEC) {
        if (kind == 1 || kind == 4 || kind == 5) {
            return 1;
        }
        report_warning(ERR_DOWNLOAD_FORMAT, "jedec");
    } else if (format == OPT_HL) {
        if (kind == 2 || kind == 5) {
            return 1;
        }
        report_warning(ERR_DOWNLOAD_FORMAT, "hl");
    } else if (format == OPT_HEX) {
        if (kind == 3 || kind == 4) {
            return 1;
        }
        report_warning(ERR_DOWNLOAD_FORMAT, "ascii-hex");
    } else {
        report_error(1012, "");
    }
    return 0;
}

/* File arguments of the fitter command line */
static void write_fitter_files(FILE *fp)
{
    unsigned count = sec_count(SEC_OUTPUT);
    char *base = make_path(source_dir(g.args[5]), g.args[5], "");
    const char *part = (const char *)attr_ptr(ATTR_PART_NAME);
    const uint8_t *rec = sec_data(SEC_OUTPUT);
    for (unsigned i = 0; i < count; i++, rec += 0x18) {
        switch (rec[0]) {
        case 1:
            out_printf(fp, " %s%s", base, ".pla");
            break;
        case 2:
            out_printf(fp, " %s%s", base, (const char *)rec + 1);
            break;
        case 3:
            out_printf(fp, " %s%s", base, ".jed");
            break;
        case 4:
            out_printf(fp, " %s", part[0] != '\0' ? part : g.device_name);
            break;
        case 5:
            out_printf(fp, " %s", (const char *)rec + 1);
            break;
        case 6:
            out_printf(fp, " %s", g.args[1]);
            break;
        default:
            return;
        }
    }
}

/* Command file runfit.$$$ read by the kernel to start the fitter */
static void write_runfit(unsigned pla_format)
{
    FILE *fp = open_file("runfit.$$$", "wt");
    if (fp == NULL) {
        fatal_error(ERR_OPEN, "runfit.$$$");
    }
    const char *part = (const char *)attr_ptr(ATTR_PART_NAME);
    const char *fitter = (const char *)attr_ptr(ATTR_FITTER);
    const char *fitter_arg = (const char *)attr_ptr(ATTR_FITTER_ARG);
    out_printf(fp, "%u ", pla_format);
    out_printf(fp, "%s ", part[0] != '\0' ? part : g.device_name);
    if (fitter[0] == '\0') {
        out_printf(fp, ". ");
    } else {
        out_printf(fp, "%s ", fitter);
    }
    if (fitter_arg[0] == '\0') {
        out_printf(fp, "~ ");
    } else {
        out_printf(fp, "%s ", fitter_arg);
    }
    out_printf(fp, "\n");
    write_fitter_files(fp);
    close_file(fp);
}

/* Set the dev_xxx flags of devices that need special handling */
static void select_device_quirks(void)
{
    const char *name = g.device_name;
    g.dev_f7024 = g.dev_xl78c800 = g.dev_ep312 = g.dev_ep1800 = 0;
    g.dev_max5 = g.dev_v5000 = g.dev_v2500b = g.dev_p22v10s = 0;
    if (strcmp(name, "f7024") == 0) {
        g.dev_f7024 = 1;
    } else if (strcmp(name, "xl78c800") == 0) {
        g.dev_xl78c800 = 1;
    } else if (strncmp(name, "ep312", 5) == 0) {
        g.dev_ep312 = 1;
        select_cell_table(CELLS_EP312, strcmp(name, "ep312lcc") == 0);
    } else if (strncmp(name, "ep324", 5) == 0) {
        g.dev_ep312 = 1;
        select_cell_table(CELLS_EP324, strcmp(name, "ep324lcc") == 0);
    } else if (strcmp(name, "ep1800") == 0) {
        g.dev_ep1800 = 1;
    } else if (strncmp(name, "max5", 4) == 0) {
        g.dev_max5 = 1;
        g.expander_free = (int)attr16(ATTR_W19) * 2;
    } else if (strcmp(name, "v5000") == 0) {
        g.dev_v5000 = 1;
    } else if (strcmp(name, "v2500b") == 0 || strcmp(name, "v2500c") == 0) {
        g.dev_v2500b = 1;
        select_cell_table(CELLS_V2500B, 0);
    } else if (strcmp(name, "v2500blcc") == 0 || strcmp(name, "v2500clcc") == 0) {
        g.dev_v2500b = 1;
        select_cell_table(CELLS_V2500B, 1);
    } else if (strncmp(name, "p22v10s", 7) == 0) {
        g.dev_p22v10s = 1;
    }
}

/* Process one design of the intermediate file.  Returns 0 if
 * another design follows, -1 at the end of the file. */
static int process_device(void)
{
    g.map_built = 0;
    load_design();
    g.lib.dir_entry = (const uint8_t *)g.device_name;
    load_device();
    unsigned type = attr8(ATTR_TYPE);
    if (g.options & OPT_ABSOLUTE) {
        abs_open();
        sim_open();
    }
    if (attr8(ATTR_TYPE) == 0xff) {
        *attr_ptr(ATTR_TYPE) = 0;
        g.is_prom_like = 1;
    }
    int more;
    for (;;) {
        equation_t *eq = alloc_zeroed(1, sizeof(*eq), "creating bit map");
        more = read_equation(eq);
        if (more <= 0) {
            break;
        }
        int owner = first_set_bit(0, eq->nvars, TERM_OUT(eq, 0));
        if (owner < 0 || owner >= g.nsyms) {
            fatal_error(1011, "");
        }
        g.syms[owner].eq = eq;
    }
    select_device_quirks();
    select_output_nodes();
    choose_demorgan_forms();
    rebuild_output_table();
    assign_or_arrays();
    if (g.dev_ep312) {
        ep312_assign_terms();
    }
    if (g.dev_v2500b) {
        v2500b_assign_alias_rows();
    }
    if (g.options & OPT_DOC) {
        doc_write_symbols();
    }
    unsigned pla_format = attr8(ATTR_PLA_FORMAT);
    if (g.options & OPT_PALASM) {
        write_palasm();
    }
    if (g.opt_edif) {
        write_edif();
    }
    if (g.opt_xnf && strncmp(g.device_name, "ga", 2) != 0) {
        report_warning(ERR_DOWNLOAD_FORMAT, "xnf");
    }
    if (!(g.options & OPT_NO_MERGE) && type != DEVT_VIRTUAL) {
        merge_shared_terms();
    }
    if (g.dev_v5000 && v5000_check_feedback()) {
        abort_phase();
    }
    if (g.options & OPT_DOC) {
        doc_write_totals();
    }
    if (type != DEVT_VIRTUAL) {
        if (type != DEVT_5) {
            check_or_arrays();
        }
        add_requested_formats(g.format_text);
    }
    if ((g.options & OPT_JEDEC) || g.opt_xnf || g.opt_fitter) {
        write_runfit(pla_format);
    }
    if ((g.options & OPT_PLA) || (pla_format > 1 && (g.options & OPT_JEDEC))) {
        if (pla_format == 0) {
            write_pla();
        } else {
            write_fitter_pla((int)pla_format);
        }
    }
    if (g.options & OPT_PDIF) {
        write_pdif();
    }
    if (!g.too_many_terms && type != DEVT_VIRTUAL && type != DEVT_8) {
        if (type == DEVT_PROM) {
            build_prom_map();
        }
        if ((g.options & OPT_JEDEC) && format_supported(OPT_JEDEC)) {
            if (g.dev_max5) {
                write_pof();
            } else {
                write_jedec();
            }
        }
        if ((g.options & OPT_HEX) && format_supported(OPT_HEX)) {
            write_hex();
        }
    }
    if (!g.too_many_terms) {
        if (g.options & OPT_ABSOLUTE) {
            abs_write();
            abs_end(more == 0 ? 0x20 : 0x10);
            sim_write();
        }
        if ((g.options & OPT_HL) && format_supported(OPT_HL)) {
            write_fuse_plot(".hl", 1);
        }
        if ((g.options & OPT_FUSE_PLOT) && pla_format == 0) {
            write_fuse_plot(".doc", 9);
        }
    }
    return more;
}

/* Process all designs of the intermediate file */
static void process_file(void)
{
    open_device_library();
    g.fuses = NULL;
    g.temp_fp = open_intermediate();
    g.syms = NULL;
    if (g.options & OPT_DOC) {
        doc_open();
    }
    while (process_device() >= 0) {
    }
    close_file(g.temp_fp);
    g.temp_fp = NULL;
    close_plot_file();
    if (g.options & OPT_ABSOLUTE) {
        check_write(g.abs_fp, g.abs_name);
        close_file(g.abs_fp);
        g.abs_fp = NULL;
    }
}

/* Parse the options, open the listing file and process the intermediate
 * file.  Returns 0 on success; errors end in fatal_error(). */
static int run_phase(char **argv)
{
    g.abs_fp = NULL;
    g.line_ptr = NULL;
    parse_flags(argv[6] != NULL ? argv[6] : "");
    for (int i = 0; i < 8 && argv[i] != NULL; i++) {
        g.args[i] = argv[i];
    }
    if (g.options & OPT_LIST) {
        open_listing(g.args[5]);
    }
    g.start_time = (long)time(NULL);
    process_file();
    if (g.options & OPT_LIST) {
        fflush(g.lst_fp);
        if (ferror(g.lst_fp)) {
            fatal_error(ERR_DISK_WRITE, make_path(source_dir(g.args[5]), g.args[5], ".lst"));
        }
        close_file(g.lst_fp);
        g.lst_fp = NULL;
        g.options &= ~(unsigned)OPT_LIST;
    }
    if (g.error_count != 0) {
        fatal_error(-1, "");
    }
    remove_temp_files();
    return 0;
}

/* Reset the state of the output modules */
void modules_reset(void)
{
    jed_reset();
    dev_reset();
    doc_reset();
    abs_reset();
    pla_reset();
    dl_reset();
    net_reset();
}

/* Entry point of the phase, called by the kernel with the phase arguments
 * Returns 0 on success, 1 on error. */
int fusemap_run(int argc, char **argv)
{
    static char *safe_argv[11];
    int rc;
    memset(&g, 0, sizeof(g));
    phase_begin(&g_rt);
    strcpy(g.temp_name, "CCXXXXXX");
    for (int i = 0; i < 10; i++) {
        safe_argv[i] = (i < argc && argv != NULL) ? argv[i] : NULL;
    }
    safe_argv[10] = NULL;
    for (int i = 0; i < 8; i++) {
        g.args[i] = "";
    }
    modules_reset();
    if (setjmp(g.abort_jmp) == 0) {
        if (argc < 3) {
            fatal_error(-1, "");
        }
        rc = run_phase(safe_argv);
        fflush(NULL);
    } else {
        rc = 1;
    }
    phase_end(&g_rt);
    return rc;
}
