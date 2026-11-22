/*
 * sim - CUPL functional simulator, shared by the csima and vsima
 * phases (sim_run(0) / sim_run(1)).
 *
 * csima and vsima run the same program; the places where they differ test
 * g_vsima.  This file holds error reporting, lists and string helpers and
 * the program flow; status output, memory and files are the phase runtime
 * of src/shared/phase.c.
 *
 * Source files:
 *   sim.c        error reporting, lists, strings, program flow
 *   load.c   .abs file and device library
 *   jedload.c  the design from a JEDEC fuse map instead (option f)
 *   parse.c  .si scanner, parser and statement actions
 *   core.c   product term evaluation, phases of a test vector
 *   bind.c   binding symbols to macrocell models
 *   eval.c   macrocell models
 *   out.c    listing / waveform / trace output
 *   jedec.c  test vectors in the JEDEC file
 *
 * Fixed-size buffers are bounds-checked.  Status messages go to stdout.
 */
#define SIM_DEFINE_GLOBALS
#include "banner.h"
#include "sim.h"
#include <time.h>
#include <unistd.h>
#include "locate.h"
#include "strutil.h"

/* -------------------------------------------------------------------------
 * Runtime state
 * ------------------------------------------------------------------------- */
#define MAX_QUEUED_ERRORS 30        /* messages per source line */
#define MARKER_LINE_SIZE 0x400

typedef struct {
    int16_t severity;   /* 0 error, 1 warning, 2 "user expected" */
    int16_t code;
    const char *text1;
    const char *text2;
} queued_error_t;

/* Message texts, indexed by error number (ERR_*). */
static const char *const k_error_text[] = {
    "program error:  ",
    "could not open:  ",
    "invalid number:  ",
    "invalid file format:  ",
    "invalid library access key",
    "invalid library interface",
    "bad library file:  ",
    "device not in library:  ",
    "invalid output format:  ",
    "invalid syntax:  ",
    "expecting device:  ",
    "unknown symbol:  ",
    "too many symbols",
    "excessive test value:  ",
    "insufficient test values",
    "field already defined:  ",
    "too many errors",
    "missing symbol:  ",
    "out of memory:  ",
    " ",
    "unstable output:  ",
    "invalid test value:  ",
    "bad fault id:  ",
    "could not read file:  ",
    "could not write file:  ",
    "inconsistent header information",
    "missing header item(s)",
    "old absolute file format:  ",
    "statement too long",
    "invalid trace level:  ",
    "invalid character:  ",
    "disk read error, unexpected end of file:  ",
    "feedback usage of undefined output: ",
    "pin number is undefined for: ",
    NULL
};
static const char *const k_error_text_33_vsima = "jedec vectors cannot be created with undefined pin numbers: ";
static const char *const k_error_text_34_vsima = "possible undefined register type: ";

static void out_of_memory(const char *context)
{
    fatal_error(ERR_OUT_OF_MEMORY, context);
}

static struct phase_rt g_rt = {
    .code_suffix = "sa",
    .messages = k_error_text,
    .message_count = ERR_NO_PIN_NUMBER + 1,
    .unknown_message = "",
    .text_size = 2048,
    .out_of_memory = out_of_memory,
};
jmp_buf g_abort_jmp;                           /* fatal_error jumps back to sim_run */

static queued_error_t g_queued[MAX_QUEUED_ERRORS]; /* messages of the current line */
static int g_queued_count;
static char g_marker_line[MARKER_LINE_SIZE + 1]; /* '^' under each error column */

/* -------------------------------------------------------------------------
 * Output, memory and files (runtime in src/shared/phase.c)
 * ------------------------------------------------------------------------- */

/* fputc, except that "\n" is written as "\r\n" in *.jed text files. */
int file_putc(int c, FILE *fp)
{
    return phase_putc(&g_rt, c, fp);
}

int file_puts(const char *s, FILE *fp)
{
    return phase_puts(&g_rt, s, fp);
}

/* printf to a stream.  Text for stdout is line buffered unless the 'v'
 * option is set. */
void stream_printf(FILE *fp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    phase_vprintf(&g_rt, fp, fmt, ap);
    va_end(ap);
}

void *alloc_tracked(size_t size, const char *context)
{
    return phase_alloc(&g_rt, size, context);
}

char *dup_string(const char *s, const char *context)
{
    return phase_strdup(&g_rt, s, context);
}

void free_tracked(void *p)
{
    phase_free(p);
}

/* Existing files are opened under the name found by a case-insensitive
 * lookup; a file that is not there is only created, never read. */
FILE *open_file(const char *path, const char *mode_in)
{
    char mode[8];
    char resolved[PATH_MAX];
    size_t n = 0;
    FILE *fp;
    for (size_t i = 0; mode_in[i] != '\0' && n + 1 < sizeof(mode); i++) {
        if (mode_in[i] != 't') {
            mode[n++] = mode_in[i];
        }
    }
    mode[n] = '\0';
    if (lincupl_resolve_ci_path(path, resolved, sizeof(resolved)) == 0) {
        fp = fopen(resolved, mode);
    } else if (mode[0] == 'r') {
        fp = NULL;
    } else {
        fp = lincupl_fopen_ci(path, mode_in);
    }
    return phase_track(&g_rt, fp, path, mode);
}

void close_file(FILE *fp)
{
    phase_close(&g_rt, fp);
}

void remove_file(const char *path)
{
    phase_remove(path, 1);
}

int read_char(FILE *fp)
{
    return phase_read_char(fp);
}

/* "<dir><base name of name without "./", cut at .pld><ext>" in tracked
 * memory. */
char *make_file_name(const char *dir, const char *name, const char *ext)
{
    return phase_file_name(&g_rt, dir, name, ext);
}

/* -------------------------------------------------------------------------
 * Error reporting
 * ------------------------------------------------------------------------- */

static const char *error_text(int code)
{
    switch (code) {
    case ERR_JEDEC_FILE: return "invalid JEDEC file:  ";
    case ERR_JEDEC_NO_DEVICE: return "no device for JEDEC file:  ";
    case ERR_JEDEC_DEVICE: return "device cannot be simulated from a JEDEC file:  ";
    case ERR_JEDEC_MODE: return "mode fuses of the JEDEC file select another mode than device:  ";
    case ERR_JEDEC_DEAD_TERMS: return "product terms without effect, the fuse map makes an input of pin:  ";
    default: break;
    }
    if (g_vsima && code == ERR_NO_PIN_NUMBER) {
        return k_error_text_33_vsima;
    }
    if (g_vsima && code == ERR_REGISTER_TYPE) {
        return k_error_text_34_vsima;
    }
    return phase_message(&g_rt, code);
}

/* Print one message.  `wave` selects the .wo file format. */
static void print_error(int wave, FILE *fp, int code, int severity, const char *text1, const char *text2)
{
    if (!wave) {
        stream_printf(fp, "[%04d%s] ", code, g_rt.code_suffix);
    } else {
        stream_printf(fp, "#E %04d ", code);
    }
    fflush(fp);
    if (severity == 2) {
        const char *a = text1 ? text1 : "(null)";
        const char *b = text2 ? text2 : "(null)";
        if (wave) {
            stream_printf(fp, "%s %s\n", a, b);
        } else if (g_vsima && code == ERR_BAD_TEST_VALUE) {
            /* vsima: "invalid test value:  <value> for <name>" */
            stream_printf(fp, "%s%s for %s\n", error_text(code), a, b);
        } else {
            stream_printf(fp, "user expected (%s) for %s\n", a, b);
        }
        fflush(fp);
        return;
    }
    if (!wave) {
        if (severity == 1) {
            stream_printf(fp, "Please note: ");
            fflush(fp);
        }
        stream_printf(fp, "%s", error_text(code));
    }
    if (text1 != NULL) {
        stream_printf(fp, "%s", text1);
    }
    if (text2 != NULL) {
        stream_printf(fp, "%s", text2);
    }
    file_putc('\n', fp);
    fflush(fp);
}

/* Print the queued messages of the current line under a marker line. */
void print_queued_errors(FILE *fp, int wave)
{
    if (g_queued_count == 0 || fp == NULL) {
        return;
    }
    /* cut the marker line after its last '^' */
    {
        int i;
        for (i = MARKER_LINE_SIZE - 1; i >= 0 && g_marker_line[i] != '^'; i--) {
        }
        g_marker_line[i + 1] = '\0';
    }
    if (!wave) {
        stream_printf(fp, "      %s\n", g_marker_line);
    }
    for (int i = 0; i < g_queued_count; i++) {
        print_error(wave, fp, g_queued[i].code, g_queued[i].severity, g_queued[i].text1, g_queued[i].text2);
    }
    file_putc('\n', fp);
    fflush(fp);
}

static void clear_marker_line(void)
{
    memset(g_marker_line, ' ', MARKER_LINE_SIZE);
    g_marker_line[MARKER_LINE_SIZE] = '\0';
}

void clear_queued_errors(void)
{
    clear_marker_line();
    g_queued_count = 0;
}

int queued_error_count(void)
{
    return g_queued_count;
}

/* Queue a message for the current source line and mark its column.  Errors
 * (not warnings) count towards g_error_count; a full queue is fatal. */
static void queue_error(int code, int column, int severity, const char *text1, const char *text2)
{
    if (severity != 1) {
        g_error_count++;
    }
    g_queued_count++;
    if (column < 0) {
        column = 0;
    }
    if (column < MARKER_LINE_SIZE) {
        g_marker_line[column] = '^';
    }
    g_queued[g_queued_count - 1].code = (int16_t)code;
    g_queued[g_queued_count - 1].severity = (int16_t)severity;
    g_queued[g_queued_count - 1].text1 = intern_string(text1);
    g_queued[g_queued_count - 1].text2 = intern_string(text2);
    if (g_queued_count >= MAX_QUEUED_ERRORS) {
        print_queued_errors(stdout, 0);
        if (g_options & OPT_LISTING) {
            print_queued_errors(g_list_file, 0);
            print_queued_errors(g_wave_file, 1);
        }
        fatal_error(ERR_TOO_MANY_ERRORS, "");
    }
}

static int current_column(void)
{
    return g_source != NULL ? g_source->col - 1 : -1;
}

void report_error(int code, const char *detail)
{
    queue_error(code, current_column(), 0, detail, NULL);
}

void report_error_at(int code, int column, const char *detail)
{
    queue_error(code, column, 0, detail, NULL);
}

void report_warning(int code, const char *detail)
{
    queue_error(code, current_column(), 1, detail, NULL);
}

void report_expected(int column, const char *expected, const char *name)
{
    queue_error(ERR_EXPECTED, column, 2, expected, name);
}

/* Queue "invalid test value:  <value> for <name>" (vsima only). */
void report_bad_value(int column, const char *value, const char *name)
{
    queue_error(ERR_BAD_TEST_VALUE, column, 2, value, name);
}

/* Print a warning on stdout and in the listing right away. */
void print_warning_now(int code, const char *detail)
{
    print_error(0, stdout, code, 1, detail, NULL);
    fflush(stdout);
    if (g_options & OPT_LISTING) {
        print_error(0, g_list_file, code, 1, detail, NULL);
        fflush(g_list_file);
    }
}

void close_abs_file(void);

/* List the pending source line, print the error (if code >= 0) on stdout
 * and in the listing, close the files and jump back to sim_run. */
void fatal_error(int code, const char *detail)
{
    if (g_line_pending) {
        g_line_pending = 0;     /* a failure inside the flush must not recurse */
        flush_source_line(g_line_text);
    }
    if (code >= 0) {
        print_error(0, stdout, code, 0, detail, NULL);
        fflush(stdout);
        if ((g_options & OPT_LISTING) && g_list_file != NULL) {
            print_error(0, g_list_file, code, 0, detail, NULL);
            fflush(g_list_file);
        }
    }
    if (g_options & OPT_LISTING) {
        close_file(g_list_file);
        close_file(g_wave_file);
    }
    close_abs_file();
    fflush(NULL);
    longjmp(g_abort_jmp, 1);
}

/* Does the (lower-cased) line contain "order" followed by ':'? */
static int is_order_line(const char *line)
{
    char lower[MARKER_LINE_SIZE + 2];
    const char *p;
    snprintf(lower, sizeof(lower), "%s", line);
    for (char *q = lower; *q != '\0'; q++) {
        *q = (char)(is_upper(*q) ? to_lower(*q) : *q);
    }
    p = strstr(lower, "order");
    if (p != NULL) {
        for (p += 5; *p != '\0'; p++) {
            if (*p == ':') {
                return 1;
            }
            if (!is_space(*p)) {
                return 0;
            }
        }
    }
    return 0;
}

static void check_write(FILE *fp, const char *name)
{
    fflush(fp);
    if (ferror(fp)) {
        fatal_error(ERR_WRITE, name);
    }
}

/* List the FIELD definitions of the design ahead of the ORDER line. */
static void list_fields(void)
{
    char *text;
    if (g_fields == NULL) {
        return;
    }
    text = alloc_tracked(0x400, "field initialization");
    for (field_t *f = g_fields; f != NULL; f = f->next) {
        snprintf(text, 0x400, "%s = [", f->name);
        for (list_node_t *n = f->members->head; n != NULL; n = n->next) {
            size_t used = strlen(text);
            snprintf(text + used, 0x400 - used, "%s%s", n->u.sym->name, n == f->members->tail ? "]" : ",");
        }
        {
            size_t used = strlen(text);
            snprintf(text + used, 0x400 - used, ";");
        }
        stream_printf(g_list_file, "%4d: FIELD %s\n", g_source->line++, text);
        check_write(g_list_file, g_list_name);
        stream_printf(g_wave_file, "#F %s\n", text);
        check_write(g_wave_file, g_wave_name);
    }
    stream_printf(g_list_file, "%4d:\n", g_source->line++);
    check_write(g_list_file, g_list_name);
    stream_printf(g_wave_file, "\n");
    check_write(g_wave_file, g_wave_name);
    free_tracked(text);
}

/* Write the current source line (cut at the read column) to the listing and
 * the waveform file, print its queued messages, and advance to the next
 * line.  The FIELD definitions are listed just before the ORDER line. */
void flush_source_line(char *line)
{
    if (g_source == NULL) {
        return;
    }
    if (g_source->col >= 0 && g_source->col <= MARKER_LINE_SIZE) {
        line[g_source->col] = '\0';
    }
    if (g_fields_unlisted && is_order_line(line) && (g_options & OPT_LISTING)) {
        list_fields();
        g_fields_unlisted = 0;
    }
    if (!g_listing_off || (!g_vector_running && g_queued_count != 0)) {
        if (g_options & OPT_LISTING) {
            stream_printf(g_list_file, "%4d: %s\n", g_source->line, line);
            check_write(g_list_file, g_list_name);
            stream_printf(g_wave_file, "#H %s\n", line);
            check_write(g_wave_file, g_wave_name);
        }
        if (g_queued_count != 0) {
            stream_printf(stdout, "%4d: %s\n", g_source->line, line);
            print_queued_errors(stdout, 0);
            if (g_options & OPT_LISTING) {
                print_queued_errors(g_list_file, 0);
                print_queued_errors(g_wave_file, 1);
            }
            clear_marker_line();
            g_queued_count = 0;
        }
    }
    if (g_options & OPT_LISTING) {
        fflush(g_list_file);
        fflush(g_wave_file);
    }
    g_source->col = 0;
    g_source->line++;
}

/* -------------------------------------------------------------------------
 * Lists
 * ------------------------------------------------------------------------- */

list_node_t *node_new(void)
{
    return alloc_tracked(sizeof(list_node_t), "expanding expression");
}

list_t *list_new(void)
{
    return alloc_tracked(sizeof(list_t), "expanding expression");
}

/* Free a node; the phantom token is static and stays. */
void node_free(list_node_t *node)
{
    if (is_phantom_token(node)) {
        return;
    }
    free_tracked(node);
}

void list_free_header(list_t *list)
{
    free_tracked(list);
}

void list_push_front(list_node_t *node, list_t *list)
{
    if (list == NULL) {
        return;
    }
    if (list->head == NULL) {
        list->head = list->tail = node;
        node->next = NULL;
    } else {
        node->next = list->head;
        list->head = node;
    }
    list->count++;
}

int list_is_empty(const list_t *list)
{
    return list != NULL && list->head == NULL;
}

void list_append(list_node_t *node, list_t *list)
{
    if (list == NULL) {
        return;
    }
    if (list->head == NULL) {
        list->head = list->tail = node;
    } else {
        list->tail->next = node;
        list->tail = node;
    }
    node->next = NULL;
    list->count++;
}

/* Free all nodes and empty the list; a count that does not match the
 * number of nodes is an internal error. */
void list_free_nodes(list_t *list)
{
    list_node_t *node = list->head;
    while (node != NULL) {
        list_node_t *next = node->next;
        node_free(node);
        list->count--;
        node = next;
    }
    list->head = list->tail = NULL;
    if (list->count != 0) {
        fatal_error(0x3ee, VTEXT("", "list processing"));
    }
}

list_node_t *list_pop_front(list_t *list)
{
    list_node_t *node;
    if (list == NULL || list->head == NULL) {
        return NULL;
    }
    node = list->head;
    if (list->head == list->tail) {
        list->head = list->tail = NULL;
    } else {
        list->head = node->next;
    }
    list->count--;
    return node;
}

/* Append src to dst; afterwards both headers describe the joined list. */
void list_concat(list_t *dst, list_t *src)
{
    if (dst == NULL || src == NULL) {
        return;
    }
    if (dst->head == NULL) {
        *dst = *src;
    } else if (src->head != NULL) {
        dst->tail->next = src->head;
        dst->tail = src->tail;
        dst->count += src->count;
    }
    *src = *dst;
    if (src->tail != NULL) {    /* both lists may be empty */
        src->tail->next = NULL;
    }
}

/* New list with copies of the nodes of src (the data is shared). */
list_t *list_clone(const list_t *src)
{
    list_t *copy;
    if (src == NULL) {
        return NULL;
    }
    copy = list_new();
    for (const list_node_t *n = src->head; n != NULL; n = n->next) {
        list_node_t *c = node_new();
        c->flags = n->flags;
        c->kind = n->kind;
        c->u = n->u;
        list_append(c, copy);
    }
    return copy;
}

/* Push one node per bit of value, LSB first, so the MSB ends up at the head.
 * Leading zero bits produce no node. */
void list_push_bits(list_t *list, unsigned value, int kind_zero, int kind_one)
{
    int count = 0;
    if (list == NULL) {
        return;
    }
    for (; value != 0; value >>= 1) {
        list_node_t *node;
        if (count > 31) {
            fatal_error(0x401, VTEXT("", "list processing"));
        }
        node = node_new();
        node->kind = (uint8_t)((value & 1) ? kind_one : kind_zero);
        list_push_front(node, list);
        count++;
    }
}

/* Push nodes of the given kind at the front until the list has count nodes. */
void list_pad_front(list_t *list, int count, int kind)
{
    if (list == NULL) {
        return;
    }
    while (list->count < count) {
        list_node_t *node = node_new();
        node->kind = (uint8_t)kind;
        list_push_front(node, list);
    }
}

/* Text nodes <stem><n> for n running from first towards last, excluding
 * first itself and including last. */
list_t *list_name_range(const char *stem, int first, int last, int kind, int flags)
{
    int step = (last - first >= 0) ? 1 : -1;
    int index = first;
    list_t *list = list_new();
    do {
        list_node_t *node;
        index += step;
        make_indexed_name(g_name_buf, stem, index);
        node = node_new();
        node->u.text = (char *)intern_string(g_name_buf);
        node->kind = (uint8_t)kind;
        node->flags = (uint8_t)flags;
        list_append(node, list);
    } while (index != last);
    return list;
}

void list_reset(list_t *list)
{
    if (list != NULL) {
        list->head = list->tail = NULL;
        list->count = 0;
    }
}

void list_copy_header(list_t *dst, const list_t *src)
{
    if (dst != NULL && src != NULL) {
        *dst = *src;
    }
}

/* Drop the top entry of g_list_stack. */
void list_stack_pop(void)
{
    if (g_list_top == &g_list_stack[0]) {
        fatal_error(0x3f6, VTEXT("", "list processing"));
    }
    g_list_top--;
}

/* Push an empty list header onto g_list_stack. */
void list_stack_push(void)
{
    if (g_list_top == &g_list_stack[LIST_STACK_SIZE - 1]) {
        fatal_error(0x3f7, VTEXT("", "list processing"));
    }
    g_list_top++;
    list_reset(g_list_top);
}

/* -------------------------------------------------------------------------
 * Strings
 * ------------------------------------------------------------------------- */

/* Return the shared copy of s (NULL for NULL or ""). */
const char *intern_string(const char *s)
{
    list_node_t *node;
    if (s == NULL || s[0] == '\0') {
        return NULL;
    }
    for (node = g_strings.head; node != NULL; node = node->next) {
        if (strcmp(s, node->u.text) == 0) {
            return node->u.text;
        }
    }
    node = node_new();
    node->u.text = dup_string(s, "expanding expression");
    list_append(node, &g_strings);
    return node->u.text;
}

/* Value of the digits at the end of s, -1 if s does not end in a digit. */
int trailing_number(const char *s)
{
    const char *end = s + strlen(s);
    const char *p = end;
    while (p > s && is_digit(p[-1])) {
        p--;
    }
    if (p == end) {
        return -1;
    }
    return (int)strtol(p, NULL, 10);
}

/* Copy src without the digits at its end. */
void strip_trailing_number(char *dst, const char *src)
{
    const char *p = src + strlen(src);
    while (p > src && is_digit(p[-1])) {
        p--;
    }
    while (src < p) {
        *dst++ = *src++;
    }
    *dst = '\0';
}

/* <stem><index>, at most 32 characters. */
void make_indexed_name(char *dst, const char *stem, int index)
{
    char digits[40];
    int room;
    snprintf(digits, sizeof(digits), "%d", index);
    strcpy(dst, stem);
    room = 0x20 - (int)strlen(stem);
    if (room > 0) {
        strncat(dst, digits, (size_t)room);
    }
    dst[0x20] = '\0';
}

/* Digits of `text` in the given base; letters are upper-cased in place. */
static int parse_digits(char *text, unsigned base)
{
    int value = 0;
    for (char *p = text; *p != '\0'; p++) {
        unsigned digit;
        if (is_alpha(*p)) {
            if (is_lower(*p)) {
                *p = (char)to_upper(*p);
            }
            digit = (unsigned)((int)*p - 0x37);
        } else if (is_digit(*p)) {
            digit = (unsigned)((int)*p - '0');
        } else {
            digit = base + 1;
        }
        if (digit >= base) {
            report_error(ERR_BAD_NUMBER, text);
            digit = 0;
        }
        value = (int)((unsigned)value * base + digit);
    }
    return value;
}

/* base_token is the token code of HEX (0x11), OCTAL (0x12), DECIMAL (0x13)
 * or BINARY (0x3c). */
int parse_number(char *text, int base_token)
{
    if (text[0] == '\0') {
        return 0;
    }
    switch (base_token) {
    case 0x11: return parse_digits(text, 16);
    case 0x12: return parse_digits(text, 8);
    case 0x13: return parse_digits(text, 10);
    case 0x3c: return parse_digits(text, 2);
    default:
        report_error(0x3f8, VTEXT("", "wrong base token"));
        return 0;
    }
}

void copy_lowercase(char *dst, const char *src)
{
    for (; *src != '\0'; src++) {
        *dst++ = (char)(is_upper(*src) ? to_lower(*src) : *src);
    }
    *dst = '\0';
}

/* -------------------------------------------------------------------------
 * Program flow
 * ------------------------------------------------------------------------- */

/* Heading of the listing and waveform files. */
static void write_listing_header(void)
{
    time_t now = time(NULL);
    stream_printf(g_list_file, "%s %s csim: CUPL Simulation Program\n%s\n", PROGRAM_NAME, PROGRAM_VERSION,
                  PROGRAM_COPYRIGHT);
    check_write(g_list_file, g_list_name);
    stream_printf(g_list_file, "CREATED %s\n", ctime(&now));
    check_write(g_list_file, g_list_name);
    stream_printf(g_list_file, "LISTING FOR SIMULATION FILE: %s%s\n\n", g_base_name, ".si");
    check_write(g_list_file, g_list_name);
    stream_printf(g_wave_file, "#WAVEFORM\n");
    fflush(g_wave_file);
}

/* Create <design>.so and <design>.wo and write their headings. */
static void open_listing_files(void)
{
    g_list_name = make_file_name(g_dir_prefix, g_base_name, ".so");
    g_wave_name = make_file_name(g_dir_prefix, g_base_name, ".wo");
    g_list_file = open_file(g_list_name, "wt");
    if (g_list_file == NULL) {
        fatal_error(ERR_OPEN, g_list_name);
    }
    g_wave_file = open_file(g_wave_name, "wt");
    if (g_wave_file == NULL) {
        fatal_error(ERR_OPEN, g_wave_name);
    }
    write_listing_header();
}

/* Put every global into its initial state. */
static void reset_globals(void)
{
    phase_begin(&g_rt);
    memset(g_queued, 0, sizeof(g_queued));
    g_queued_count = 0;
    memset(&g_strings, 0, sizeof(g_strings));
    g_error_count = 0;
    g_options = 0;
    g_list_file = g_wave_file = NULL;
    g_list_name = g_wave_name = NULL;
    g_dir_prefix = NULL;
    g_base_name[0] = '\0';
    g_fields = g_fields_tail = NULL;
    g_fields_unlisted = 1;
    g_line_pending = 0;
    g_line_ptr = NULL;
    g_source = NULL;
    memset(g_list_stack, 0, sizeof(g_list_stack));
    g_list_top = &g_list_stack[0];
    g_syms = NULL;
    g_nsyms = 0;
    g_abs_extra = NULL;
    g_header_items = 0xff;
    g_terms = NULL;
    g_nterms = g_total_rows = 0;
    g_row_words = -1;
    g_plane_words = 0;
    g_dev_rec = NULL;
    g_dev_is_isp1016 = 0;
    g_dev_is_7c33x = 0;
    g_dev_is_78c800 = 0;
    g_dev_is_7c330 = 0;
    g_dev_has_class = 0;
    clear_marker_line();
    sim_reset();
    parse_reset();
    jedec_reset();
}

/* Run the phase: read the options, open the listing files, load the design,
 * simulate the .si file and write the JEDEC test vectors. */
static void sim_main(void)
{
    g_line_ptr = NULL;
    /* option letters */
    for (const char *p = g_args[6]; *p != '\0'; p++) {
        switch (*p) {
        case 'j': g_options |= OPT_JEDEC; break;
        case 'l':
        case 'w': g_options |= OPT_LISTING; break;
        case 'n': g_options |= OPT_N; break;
        case 'f': g_options |= OPT_FUSEMAP; break;
        case 'v': g_options |= OPT_CONSOLE; g_rt.console = 1; break;
        default: break;
        }
    }
    g_dir_prefix = alloc_tracked(strlen(g_args[5]) + 1, "initialization");
    lincupl_split_path(g_args[5], g_dir_prefix, strlen(g_args[5]) + 1, g_base_name, sizeof(g_base_name), NULL, 0);
    if (g_options & OPT_LISTING) {
        open_listing_files();
    }
    clear_marker_line();
    if ((g_options & OPT_FUSEMAP) && !g_vsima) {
        load_design_jedec();
    } else {
        g_options &= (uint16_t)~OPT_FUSEMAP;
        load_design();
    }
    build_fields();
    sim_allocate();
    if (g_options & OPT_FUSEMAP) {
        jedec_design_bound();
    }
    if (g_options & OPT_JEDEC) {
        jedec_begin();
    }
    parse_simulation_file();
    if (g_options & OPT_JEDEC) {
        jedec_finish();
    }
    g_line_ptr = NULL;
    if (g_vsima ? g_error_count > 0 : g_error_count != 0) {
        fatal_error(-1, "");
    }
    if (g_options & OPT_LISTING) {
        /* Each of these two messages names the other file.  This is
         * deliberate; the message texts are kept as they are. */
        fflush(g_list_file);
        if (ferror(g_list_file)) {
            fatal_error(ERR_WRITE, make_file_name(g_dir_prefix, g_base_name, ".wo"));
        }
        close_file(g_list_file);
        fflush(g_wave_file);
        if (ferror(g_wave_file)) {
            fatal_error(ERR_WRITE, make_file_name(g_dir_prefix, g_base_name, ".so"));
        }
        close_file(g_wave_file);
    }
}

/* Entry point of both phases; vsima selects the variant.  Returns 0 on
 * success, 1 after an error.  All files and memory are released. */
int sim_run(int vsima, int argc, char **argv)
{
    int result = 1;
    g_vsima = vsima;
    reset_globals();
    for (int i = 0; i < 9; i++) {
        g_args[i] = (i < argc && argv != NULL && argv[i] != NULL) ? argv[i] : (i < 7 && i < argc ? "" : NULL);
    }
    if (setjmp(g_abort_jmp) == 0) {
        if (argc < 3) {
            fatal_error(-1, "");
        }
        for (int i = 0; i < 8; i++) {
            if (g_args[i] == NULL) {
                g_args[i] = "";
            }
        }
        sim_main();
        fflush(NULL);
        result = 0;
    }
    phase_end(&g_rt);
    return result;
}
