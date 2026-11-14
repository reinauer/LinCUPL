/*
 * minimize_io.c - files and frames of the minimize phase.
 *
 * Reads the device class from the device library, copies the design
 * headers and symbol tables from CM<pid> to CC<pid>, reads and writes
 * frames, and turns the rows of the frames into the covers of a PLA and
 * the minimized covers back into rows.
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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "locate.h"
#include "minimize_int.h"

/* -------------------------------------------------------------------------
 * State
 * ------------------------------------------------------------------------- */
char minimize_cm_name[9];
static char g_cc_name[9];              /* CC<pid>, target of the pass-through rename */
static char g_out_buf[9];              /* CC<pid>, output */
const char *minimize_out_name;
static FILE *g_in;
static FILE *g_out;
static uint8_t g_header[CF_HEADER_SIZE]; /* header of the current design */
static const char *g_device;           /* device name from the file header */
static int g_designs_done;             /* design headers copied */

static frame_t *g_frames;              /* frames being minimized */

/* Row reader */
static int g_num_inputs;               /* symbols of the current frame */
static char *g_out_text;               /* output part of every row as text */
static frame_t *g_row_frame;           /* frame being read */
static int g_row_pos;                  /* next row of g_row_frame */
static int g_row_words;                /* words per row */
static int g_plane_words;              /* words per plane */
static int g_row_index;                /* rows consumed of the current section */
int minimize_split_state;
static int g_split_rows;               /* rows stored for the first section */
static int g_rows_left;
static int g_rows_done;
int minimize_shared_mode;

typedef struct {
    FILE *fp;
    uint8_t *hdr_buf;       /* library header */
    uint8_t *dir_entry;     /* directory entry of the device */
    uint8_t *rec_buf;       /* device record */
    size_t rec_size;
} dl_handle_t;

/* =========================================================================
 * Device library: only the device class is needed
 * ========================================================================= */

/* open the library and check its header; DL_OK if it is usable */
static int open_library(dl_handle_t *lib, const char *path)
{
    lib->fp = minimize_open_file(path, "rb");
    if (lib->fp == NULL) {
        return DL_CANNOT_OPEN;
    }
    lib->hdr_buf = minimize_alloc_tracked(DL_HEADER_SIZE, "initialization");
    return dl_read_header(lib->fp, lib->hdr_buf);
}

/* find the directory entry of a device */
static int find_device(dl_handle_t *lib, const char *name)
{
    lib->dir_entry = minimize_alloc_tracked(DL_ENTRY_SIZE + 1, "initialization");
    return dl_find_device(lib->fp, name, DL_MATCH_CASE, lib->dir_entry);
}

/* read the device record */
static int read_device(dl_handle_t *lib)
{
    if (!dl_seek_image(lib->fp, lib->dir_entry)) {
        return 0;
    }
    lib->rec_size = dl_image_size(lib->dir_entry);
    lib->rec_buf = minimize_alloc_tracked(lib->rec_size, "device library processing");
    return dl_first_block_read(dl_read_image(lib->fp, lib->rec_buf, lib->rec_size), lib->rec_size);
}

/* load the record of the design's device and return its class */
int minimize_load_device_class(void)
{
    dl_handle_t lib;
    memset(&lib, 0, sizeof(lib));

    switch (open_library(&lib, minimize_args[2])) {
    case DL_OK:
        break;
    case DL_CANNOT_OPEN:
        minimize_fatal_error(ERR_OPEN, "cupl.dl");
        break;
    case DL_NO_HEADER:
    case DL_BAD_MAGIC:
        minimize_fatal_error(ERR_BAD_LIBRARY, "cupl.dl");
        break;
    default:
        minimize_fatal_error(ERR_LIB_ACCESS_KEY, "");
        break;
    }
    if (!find_device(&lib, g_device)) {
        minimize_fatal_error(ERR_DEVICE_NOT_FOUND, g_device);
    }
    if (!read_device(&lib)) {
        minimize_fatal_error(ERR_BAD_LIBRARY, g_device);
    }
    minimize_close_file(lib.fp);

    /* the offsets are checked against the record; a class outside it is 0 */
    if (lib.rec_size < DL_INFO_OFFSET + 4) {
        return 0;
    }
    size_t info = rd32(lib.rec_buf + DL_INFO_OFFSET);
    if (info >= lib.rec_size || lib.rec_size - info <= DL_INFO_CLASS) {
        return 0;
    }
    return (int8_t)lib.rec_buf[info + DL_INFO_CLASS];
}

/* =========================================================================
 * Product term files
 * ========================================================================= */

static void check_output(void)
{
    fflush(g_out);
    if (ferror(g_out)) {
        minimize_fatal_error(ERR_DISK_WRITE, minimize_out_name);
    }
}

/* open CM<pid> */
static void open_input(void)
{
    phase_temp_name(minimize_cm_name, minimize_args[3]);
    g_in = minimize_open_file(minimize_cm_name, "rb");
    if (g_in == NULL) {
        minimize_fatal_error(ERR_OPEN, minimize_cm_name);
    }
}

/* create CC<pid> */
static void open_output(void)
{
    minimize_out_name = g_out_buf;
    phase_temp_name(g_out_buf, minimize_args[3]);
    g_out = minimize_open_file(minimize_out_name, "wb");
    if (g_out == NULL) {
        minimize_fatal_error(ERR_OPEN, minimize_out_name);
    }
}

/* read the file header and the block that follows it */
static uint8_t *read_file_header(void)
{
    uint8_t *extra = NULL;
    if (!cf_read_header(g_in, g_header)) {
        minimize_fatal_error(ERR_DISK_READ, minimize_cm_name);
    } else if (rd16(g_header + CF_HDR_MAGIC) != CF_MAGIC_CM) {
        minimize_fatal_error(ERR_INVALID_FILE, minimize_cm_name);
    }
    unsigned extra_size = cf_header_extra(g_header);
    if (extra_size != 0) {
        extra = minimize_alloc_tracked(extra_size, "symbol table");
        if (fread(extra, extra_size, 1, g_in) == 0) {
            minimize_fatal_error(ERR_DISK_READ, minimize_cm_name);
        }
    }
    return extra;
}

/* nothing to minimize; hand the file to fusemap under its name */
static void pass_through(void)
{
    char from[PATH_MAX], to[PATH_MAX];
    minimize_close_file(g_in);
    g_in = NULL;
    phase_temp_name(g_cc_name, minimize_args[3]);
    lincupl_normalize_slashes(minimize_cm_name, from, sizeof(from));
    lincupl_normalize_slashes(g_cc_name, to, sizeof(to));
    if (rename(from, to) != 0) {
        unlink(to);
        if (rename(from, to) != 0) {
            minimize_report_error(ERR_OPEN, g_cc_name);
        }
    }
}

/* 0 if the file was passed through unchanged */
static int check_input(void)
{
    uint8_t *extra = read_file_header();
    phase_free(extra);
    if (g_designs_done == 0 && g_header[CF_HDR_FORMAT] == ':') {
        pass_through();
        return 0;
    }
    fseek(g_in, 0, SEEK_SET);
    return 1;
}

/* copy the header, the block
 * after it and the symbol table of a design to the output */
void minimize_copy_design_header(void)
{
    uint8_t *extra = read_file_header();
    unsigned extra_size = cf_header_extra(g_header);
    if (!cf_write_header(g_out, g_header)) {
        minimize_fatal_error(ERR_DISK_WRITE, minimize_out_name);
    }
    if (extra_size != 0 && fwrite(extra, extra_size, 1, g_out) == 0) {
        minimize_fatal_error(ERR_DISK_WRITE, minimize_out_name);
    }
    check_output();

    char device[CF_HEADER_SIZE - CF_HDR_DEVICE + 1];
    memcpy(device, g_header + CF_HDR_DEVICE, CF_HEADER_SIZE - CF_HDR_DEVICE);
    device[CF_HEADER_SIZE - CF_HDR_DEVICE] = '\0';
    g_device = minimize_dup_string(device, "symbol table");
    phase_free(extra);

    /* the symbol records are copied as they are */
    unsigned symbols = rd16(g_header + CF_HDR_SYMBOLS);
    uint8_t *table = minimize_alloc_tracked((size_t)symbols * CF_SYMBOL_SIZE, "symbol table");
    if (symbols == 0 || fread(table, CF_SYMBOL_SIZE, symbols, g_in) == 0) {
        minimize_fatal_error(ERR_DISK_READ, minimize_cm_name);
    }
    if (fwrite(table, CF_SYMBOL_SIZE, symbols, g_out) == 0) {
        minimize_fatal_error(ERR_DISK_WRITE, minimize_out_name);
    }
    check_output();
    phase_free(table);
    g_designs_done++;
}

/* open input and output and copy the first design header; 0 if the input
 * was passed through */
int minimize_open_files(void)
{
    open_input();
    if (!check_input()) {
        return 0;
    }
    open_output();
    minimize_copy_design_header();
    return 1;
}

/* close both files and remove the input */
void minimize_close_files(void)
{
    char path[PATH_MAX];
    minimize_close_file(g_in);
    g_in = NULL;
    check_output();
    minimize_close_file(g_out);
    g_out = NULL;
    lincupl_normalize_slashes(minimize_cm_name, path, sizeof(path));
    unlink(path);
}

/* -------------------------------------------------------------------------
 * Frames
 * ------------------------------------------------------------------------- */
static uint16_t *row_out(const frame_t *f, int row)
{
    return f->data + (size_t)row * (size_t)f->row_words;
}

static uint16_t *row_mask(const frame_t *f, int row)
{
    return row_out(f, row) + f->row_words / 3;
}

static uint16_t *row_val(const frame_t *f, int row)
{
    return row_out(f, row) + 2 * (f->row_words / 3);
}

static int plane_bit(const uint16_t *plane, int i)
{
    return (plane[1 + (i >> 4)] >> (i & 15)) & 1;
}

static void plane_set(uint16_t *plane, int i)
{
    plane[1 + (i >> 4)] |= (uint16_t)(1u << (i & 15));
}

/* allocate the rows of a frame from its header */
static void alloc_frame_rows(frame_t *f)
{
    int plane_words = cf_plane_words(f->num_vars);
    f->row_words = plane_words * 3;
    f->total_words = (int)f->num_rows * f->row_words;
    f->alloc_rows = f->num_rows;
    phase_free(f->data);
    f->data = minimize_alloc_tracked((size_t)f->total_words * sizeof(uint16_t), "frame allocation");
}

/* read the next frame; 1 = frame, 0 = end of a design,
 * -1 = end of the file */
int minimize_read_frame(frame_t *f)
{
    cf_frame_t header;
    if (!cf_read_frame(g_in, &header)) {
        minimize_fatal_error(ERR_DISK_READ, minimize_cm_name);
    }
    f->flags = header.flags;
    f->field2 = header.or_array;
    f->num_vars = header.nvars;
    f->num_rows = header.nrows;
    f->field8 = header.has_xor;
    f->field10 = header.max_terms;
    if (f->flags & CF_FRAME_END_OF_DESIGN) {
        return 0;
    }
    if (f->flags & CF_FRAME_END_OF_FILE) {
        return -1;
    }
    alloc_frame_rows(f);
    /* a short frame is taken as far as it goes */
    if (cf_read_words(g_in, f->data, (size_t)f->total_words) == 0) {
        minimize_fatal_error(ERR_DISK_READ, minimize_cm_name);
    }
    return 1;
}

/* A frame without rows cannot be written: writing nothing
 * counts as a failed write. */
void minimize_write_frame(frame_t *f)
{
    cf_frame_t header = { f->flags, f->field2, f->num_vars, f->num_rows, f->field8, f->field10 };
    if (!cf_write_frame(g_out, &header)) {
        minimize_fatal_error(ERR_DISK_WRITE, minimize_out_name);
    }
    if ((f->flags & (CF_FRAME_END_OF_DESIGN | CF_FRAME_END_OF_FILE)) == 0) {
        f->total_words = (int)f->num_rows * f->row_words;
        if (cf_write_words(g_out, f->data, (size_t)f->total_words) == 0) {
            minimize_fatal_error(ERR_DISK_WRITE, minimize_out_name);
        }
        check_output();
    }
}

static frame_t *clone_frame(const frame_t *f)
{
    frame_t *copy = minimize_alloc_tracked(sizeof(*copy), "multiple out minimization");
    *copy = *f;
    copy->next = NULL;
    copy->alloc_rows = f->row_words > 0 ? f->total_words / f->row_words : 0;
    copy->data = minimize_alloc_tracked((size_t)f->total_words * sizeof(uint16_t), "multiple output min");
    if (f->total_words > 0) {
        memcpy(copy->data, f->data, (size_t)f->total_words * sizeof(uint16_t));
    }
    return copy;
}

/* append a copy of the frame to the list of frames to minimize */
void minimize_queue_frame(const frame_t *f)
{
    frame_t *copy = clone_frame(f);
    frame_t **link = &g_frames;
    while (*link != NULL) {
        link = &(*link)->next;
    }
    *link = copy;
}

frame_t *minimize_dequeue_frame(void)
{
    frame_t *f = g_frames;
    if (f != NULL) {
        g_frames = f->next;
    }
    return f;
}

void minimize_free_frame(frame_t *f)
{
    phase_free(f->data);
    phase_free(f);
}

/* resize the frame to the given number of rows; the output
 * plane of the last row is taken from the first row */
static void resize_frame(frame_t *f, int rows)
{
    int plane_words = f->row_words / 3;
    size_t row_bytes = (size_t)f->row_words * sizeof(uint16_t);
    uint16_t *data = minimize_alloc_tracked((size_t)rows * row_bytes, "multiple output min");
    int keep = f->alloc_rows < rows ? f->alloc_rows : rows;
    if (keep > 0) {
        memcpy(data, f->data, (size_t)keep * row_bytes);
    }
    phase_free(f->data);
    f->data = data;
    f->alloc_rows = rows;
    memmove(row_out(f, rows - 1), row_out(f, 0), (size_t)plane_words * sizeof(uint16_t));
}

/* -------------------------------------------------------------------------
 * Rows as text: "<inputs> <outputs>\n" with one character per symbol
 * ------------------------------------------------------------------------- */

/* start reading the rows of a frame.
 * The output part of all rows is that of the first row. */
void minimize_begin_frame(frame_t *f)
{
    g_row_frame = f;
    g_row_pos = 0;
    g_row_words = f->row_words;
    g_plane_words = f->row_words / 3;
    g_row_index = 0;
    g_num_inputs = f->num_vars;
    phase_free(g_out_text);
    g_out_text = minimize_alloc_tracked((size_t)g_num_inputs + 1, "product term buffer");
    for (int i = 0; i < g_num_inputs; i++) {
        g_out_text[i] = (f->alloc_rows > 0 && plane_bit(row_out(f, 0), i)) ? '1' : '0';
    }
    g_out_text[g_num_inputs] = '\0';
}

/* text of the current row of the frame being read */
static void format_row(char *text)
{
    const uint16_t *mask = row_mask(g_row_frame, g_row_pos);
    const uint16_t *val = row_val(g_row_frame, g_row_pos);
    for (int i = 0; i < g_num_inputs; i++) {
        text[i] = !plane_bit(mask, i) ? '-' : plane_bit(val, i) ? '1' : '0';
    }
    text[g_num_inputs] = ' ';
    strcpy(text + g_num_inputs + 1, g_out_text);
    text[2 * g_num_inputs + 1] = '\n';
    text[2 * g_num_inputs + 2] = '\0';
}

/* text of the next row of the frame; 0 at the end of the frame
 * or of its current section */
static int next_row(char *text, frame_t *f)
{
    int skip_first = 0;

    if (minimize_split_state == 2) {
        skip_first = g_split_rows < g_rows_done;
        f->num_rows = (uint16_t)g_rows_left;
        g_rows_left = 0;
    } else if (minimize_split_state == 1) {
        return 0;
    }
    while ((int)f->num_rows > g_row_index) {
        int emitted = 0;
        if (g_row_pos >= g_row_frame->alloc_rows) {
            /* the row count says there are more rows than the frame holds */
            minimize_fatal_error(ERR_PROGRAM, "product term sections");
        }
        int type = row_mask(g_row_frame, g_row_pos)[0];
        if (type == ROW_SKIP) {
            /* not passed on, but counted */
        } else if (minimize_split_state == 2) {
            /* skipping the first section, which is not counted */
            g_row_index--;
            if (type == ROW_SEPARATOR) {
                if (skip_first) {
                    skip_first = 0;
                } else {
                    minimize_split_state = 3;
                }
            }
        } else {
            format_row(text);
            emitted = 1;
            if (type == ROW_SEPARATOR && minimize_split_state != 3) {
                minimize_split_state = 1;
            }
        }
        g_row_pos++;
        g_row_index++;
        g_rows_left = (int)f->num_rows - g_row_index;
        g_rows_done = g_row_index;
        if (emitted) {
            return 1;
        }
    }
    return 0;
}

/* a cube as row text */
static char *format_cube(pset c, char *text)
{
    static const char in_chars[] = "?01-";
    int n = 0;
    for (int var = 0; var < minimize_cube.num_binary_vars; var++) {
        text[n++] = in_chars[is_in_set(c, var * 2 + 1) * 2 + is_in_set(c, var * 2)];
    }
    for (int var = minimize_cube.num_binary_vars; var < minimize_cube.num_vars; var++) {
        text[n++] = ' ';
        for (int i = minimize_cube.first_part[var]; i <= minimize_cube.last_part[var]; i++) {
            text[n++] = is_in_set(c, i) ? '1' : '0';
        }
    }
    text[n] = '\0';
    return text;
}

/* row text to the three planes of a row.  A term
 * without input literals is stored as symbol 1 if it has an output and as
 * symbol 0 otherwise. */
static void parse_row(const char *text, uint16_t *out, uint16_t *mask, uint16_t *val)
{
    const char *in_text = text;
    const char *out_text = text + g_num_inputs + 1;
    int any_in = 0, any_out = 0;

    for (int i = 0; i < g_plane_words; i++) {
        out[i] = mask[i] = val[i] = 0;
    }
    mask[0] |= ROW_NORMAL;
    for (int i = 0; i < g_num_inputs; i++) {
        if (in_text[i] == '0') {
            plane_set(mask, i);
            any_in = 1;
        } else if (in_text[i] == '1') {
            plane_set(mask, i);
            plane_set(val, i);
            any_in = 1;
        }
        if (out_text[i] == '1') {
            plane_set(out, i);
            any_out = 1;
        }
    }
    if (!any_in) {
        mask[1] |= any_out ? 2 : 1;
        val[1] |= any_out ? 2 : 1;
    }
}

/* some of the first n symbols is set in both planes */
static int planes_intersect(const uint16_t *a, const uint16_t *b, int n)
{
    for (int i = 0; i < n; i++) {
        if (plane_bit(a, i) && plane_bit(b, i)) {
            return 1;
        }
    }
    return 0;
}

/* =========================================================================
 * Frames <-> covers (Espresso cvrin.c, cvrout.c)
 * ========================================================================= */

pla_t *minimize_new_PLA(int pla_type)
{
    pla_t *PLA = minimize_work_alloc(sizeof(*PLA));
    PLA->F = PLA->D = PLA->R = NULL;
    PLA->pla_type = pla_type;
    return PLA;
}

/* releases only the PLA record; its covers are working memory and go
 * with phase_pool_free_all() */
void minimize_free_PLA(pla_t *PLA)
{
    minimize_work_free(PLA);
}

/* add one row to the covers of the PLA.  This is the PLA reader
 * of Espresso working on the text made by format_row(): blanks, tabs and
 * '|' are skipped, '2' stands for '-', and in the output part '4' stands
 * for '1', '3' for '0' and '~' for nothing.  The other characters matter
 * only when the frame has a different number of symbols than the cube
 * structure was set up for. */
static void read_cube(pla_t *PLA, const char *line)
{
    pset cf = minimize_cube.temp[0], cr = minimize_cube.temp[1], cd = minimize_cube.temp[2];
    int savef = 0, saved = 0, saver = 0;
    int pos = 0;

    set_clear(cf, minimize_cube.size);
    for (int var = 0; var < minimize_cube.num_binary_vars; var++, pos++) {
        switch (line[pos]) {
        case '-':
        case '2':
            set_insert(cf, var * 2 + 1);
            set_insert(cf, var * 2);
            break;
        case '0':
            set_insert(cf, var * 2);
            break;
        case '1':
            set_insert(cf, var * 2 + 1);
            break;
        case ' ':
        case '\t':
        case '|':
            var--;
            break;
        case '\n':
            minimize_fatal_error(ERR_INTERNAL_EOL_IN_INPUTS, "");
            break;
        default:
            minimize_fatal_error(ERR_INTERNAL_BAD_CHAR, "");
        }
    }
    set_copy(cr, cf);
    set_copy(cd, cf);

    int out = minimize_cube.num_vars - 1;
    for (int i = minimize_cube.first_part[out]; i <= minimize_cube.last_part[out]; i++, pos++) {
        switch (line[pos]) {
        case '1':
        case '4':
            if (PLA->pla_type & PLA_TYPE_F) {
                set_insert(cf, i);
                savef = 1;
            }
            break;
        case '0':
        case '3':
            if (PLA->pla_type & PLA_TYPE_R) {
                set_insert(cr, i);
                saver = 1;
            }
            break;
        case '-':
        case '2':
            if (PLA->pla_type & PLA_TYPE_D) {
                set_insert(cd, i);
                saved = 1;
            }
            break;
        case '~':
            break;
        case ' ':
        case '\t':
        case '|':
            i--;
            break;
        case '\n':
            minimize_fatal_error(ERR_INTERNAL_EOL_IN_OUTPUTS, "");
            break;
        default:
            minimize_fatal_error(ERR_INTERNAL_BAD_CHAR, "");
        }
    }
    if (savef) {
        PLA->F = minimize_sf_addset(PLA->F, cf);
    }
    if (saved) {
        PLA->D = minimize_sf_addset(PLA->D, cd);
    }
    if (saver) {
        PLA->R = minimize_sf_addset(PLA->R, cr);
    }
}

/* set up the cube structure for the current frame size and read
 * the rows of all queued frames of the given level; 0 if there is none */
static int parse_pla(pla_t *PLA, int level)
{
    int found = 0;

    minimize_cube.num_binary_vars = g_num_inputs;
    minimize_cube.num_vars = minimize_cube.num_binary_vars + 1;
    minimize_cube.part_size = minimize_work_alloc((size_t)minimize_cube.num_vars * sizeof(int));
    /* the line has room for the cube structure and for the widest frame */
    size_t line_size = (size_t)minimize_cube.num_vars * 2 + 1;
    for (frame_t *f = g_frames; f != NULL; f = f->next) {
        if ((size_t)f->num_vars * 2 + 3 > line_size) {
            line_size = (size_t)f->num_vars * 2 + 3;
        }
    }
    char *line = minimize_alloc_tracked(line_size, "allocating PLA structure");
    minimize_cube.part_size[minimize_cube.num_vars - 1] = minimize_cube.num_binary_vars;
    minimize_cube_setup();

    if (PLA->F == NULL) {
        PLA->F = minimize_new_cover(10);
        PLA->R = minimize_new_cover(10);
        PLA->D = minimize_new_cover(10);
    }
    for (frame_t *f = g_frames; f != NULL; f = f->next) {
        if ((f->flags & FRAME_LEVEL_MASK) == level) {
            found = 1;
            minimize_begin_frame(f);
            while (next_row(line, f)) {
                read_cube(PLA, line);
            }
        }
    }
    phase_free(line);
    return found;
}

/* read the on-set and derive the off-set as its complement */
int minimize_read_pla(pla_t *PLA, int level)
{
    if (!parse_pla(PLA, level)) {
        return 0;
    }
    for (int var = 0; var < minimize_cube.num_vars; var++) {
        minimize_cube.part_size[var] = abs(minimize_cube.part_size[var]);
    }
    if (PLA->F == NULL) {
        minimize_fatal_error(ERR_INTERNAL_NO_COVER, "");
    }
    minimize_sf_free(PLA->R);
    PLA->R = minimize_complement(minimize_cube2list(PLA->F, PLA->D));
    return 1;
}

/* write the minimized cover back into the queued
 * frames of the given level.  Each frame receives the cubes that drive its
 * output. */
void minimize_store_pla(pla_t *PLA, int level)
{
    pcover F = PLA->F;
    size_t row_bytes = (size_t)g_row_words * sizeof(uint16_t);
    uint16_t *rows = minimize_alloc_tracked((size_t)F->count * row_bytes, "multiple output min");
    /* room for the cube and for what parse_row() reads of it */
    char *text = minimize_alloc_tracked((size_t)(minimize_cube.num_binary_vars + minimize_cube.num_vars + 1) + 2 * (size_t)g_num_inputs + 2,
                               "creating internal PLA representation");
    pset p, last;
    int i = 0;

    foreach_set(F, last, p) {
        uint16_t *row = rows + (size_t)i * (size_t)g_row_words;
        parse_row(format_cube(p, text), row, row + g_plane_words, row + 2 * g_plane_words);
        i++;
    }
    phase_free(text);

    for (frame_t *f = g_frames; f != NULL; f = f->next) {
        if ((f->flags & FRAME_LEVEL_MASK) != level) {
            continue;
        }
        int n = 0;                      /* rows the frame has now */
        int pos = 0;                    /* row the next cube goes to */
        if (f->row_words != g_row_words) {
            /* the rows were laid out for a frame of another size */
            minimize_fatal_error(ERR_PROGRAM, "product term sections");
        }
        if (minimize_split_state == 3) {
            /* second section: continue after the separator row, counting
             * from the rows stored for the first section */
            while (pos < f->alloc_rows && row_mask(f, pos)[0] != ROW_SEPARATOR) {
                pos++;
            }
            if (pos >= f->alloc_rows) {
                minimize_fatal_error(ERR_PROGRAM, "product term sections");
            }
            pos++;
            n = g_split_rows;
            g_split_rows = 0;
            minimize_split_state = 0;
        }
        for (i = 0; i < F->count; i++) {
            const uint16_t *row = rows + (size_t)i * (size_t)g_row_words;
            if (planes_intersect(row_out(f, 0), row, g_num_inputs)) {
                if (n >= (int)f->num_rows) {
                    resize_frame(f, n + 1);
                    pos = n;
                }
                if (pos >= f->alloc_rows) {
                    minimize_fatal_error(ERR_PROGRAM, "product term sections");
                }
                memcpy(row_mask(f, pos), row + g_plane_words, (size_t)g_plane_words * sizeof(uint16_t));
                memcpy(row_val(f, pos), row + 2 * g_plane_words, (size_t)g_plane_words * sizeof(uint16_t));
                pos++;
                n++;
            }
        }
        if (pos == 0 && (minimize_split_state == 1 || (minimize_split_state == 2 && !minimize_shared_mode))) {
            /* no row was stored: the separator would have to be marked in
             * front of the frame (first section), or the frame would be
             * read again and again (second section) */
            minimize_fatal_error(ERR_PROGRAM, "product term sections");
        }
        if (pos > 0) {
            /* the header word of the last row; skipped when no row was
             * stored */
            row_mask(f, pos - 1)[0] = 0;
        }
        if (minimize_split_state == 1) {
            row_mask(f, pos - 1)[0] |= ROW_SEPARATOR;
            g_split_rows = n;
            minimize_split_state = 2;
        }
        f->num_rows = (uint16_t)n;
        f->total_words = n * g_row_words;
    }
    phase_free(rows);
}

/* Reset the file and row reader state for a new run of the phase. */
void minimize_io_reset(void)
{
    strcpy(minimize_cm_name, "CMXXXXXX");
    strcpy(g_cc_name, "CCXXXXXX");
    strcpy(g_out_buf, "CCXXXXXX");
    minimize_out_name = NULL;
    g_in = NULL;
    g_out = NULL;
    memset(g_header, 0, sizeof(g_header));
    g_device = "";
    g_designs_done = 0;
    g_frames = NULL;
    g_num_inputs = 0;
    g_out_text = NULL;
    g_row_frame = NULL;
    g_row_pos = 0;
    g_row_words = 0;
    g_plane_words = 0;
    g_row_index = 0;
    minimize_split_state = 0;
    g_split_rows = 0;
    g_rows_left = 0;
    g_rows_done = 0;
    minimize_shared_mode = 0;
}
