/*
 * cbld - CUPL device library management program.
 *
 * Inspects .dl device libraries (-t short listing, -l long listing, -e
 * valid extensions, -u to name the library) and builds new ones (-b) from
 * a build file that copies devices out of existing libraries.
 */

#include "platform.h"
#include "banner.h"
#include "devlib.h"
#include "locate.h"
#include "strutil.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Section headers of a device image (the file layout is in devlib.h)
 * ------------------------------------------------------------------------- */
/* Packed 20-byte on-disk section header at the start of each device record */
typedef struct {
    uint8_t  ord;
    uint8_t  seg;
    uint16_t max;
    uint16_t cnt;
    uint16_t rsize;
    uint32_t roff;
    uint32_t disk_ptr1;
    uint32_t disk_ptr2;
} dl_sec_hdr_disk_t;

/* 64-bit safe in-memory section header keeping full-width void* pointers */
typedef struct {
    uint8_t  ord;
    uint8_t  seg;
    uint16_t max;
    uint16_t cnt;
    uint16_t rsize;
    uint32_t roff;
    void    *ptr1;
    void    *ptr2;
} dl_sec_hdr_mem_t;

/* -------------------------------------------------------------------------
 * In-memory library handle
 * ------------------------------------------------------------------------- */
typedef struct {
    FILE             *fp;
    uint8_t          *hdr_buf;                    /* 199 bytes */
    uint8_t          *dir_buf;                    /* 1000 * 26 bytes */
    uint8_t          *cur_dir_entry;
    uint8_t          *rec_buf;                    /* current device record */
    int32_t           field_1c;
    int32_t           field_20;
    int32_t           field_24;
    int32_t           field_28;
    int32_t           num_sections;
    dl_sec_hdr_mem_t  sections[DL_SECTIONS];  /* 64-bit safe section headers */
} dl_handle_t;

/* -------------------------------------------------------------------------
 * Build-file scanner state (one per open file)
 * ------------------------------------------------------------------------- */
typedef struct {
    char *filename;
    FILE *fp;
    int   line_num;
    int   col_num;
    int   max_tok_len;
    uint8_t eof_flag;
} scan_ctx_t;

#define MAX_SCAN_DEPTH 20

/* -------------------------------------------------------------------------
 * Global variables
 * ------------------------------------------------------------------------- */
static int          g_build_pass    = 0;
static int          g_err_count     = 0;
static uint16_t     g_flags         = 0;
static const char  *g_lib_path      = NULL;
static char        *g_dev_filter[16];          /* uses indices 3..15 */
static char         g_dev_filter_buf[16][64];
static uint8_t      g_dev_found[16];           /* uses indices 3..15 */

static dl_handle_t  g_list_lib;
static dl_handle_t  g_src_lib;

/* Target library build globals */
static FILE        *g_target_fp     = NULL;
static uint8_t     *g_target_hdr    = NULL;
static uint8_t     *g_target_dir    = NULL;
static uint8_t     *g_target_dir_cur = NULL;
static int          g_target_dev_cnt = 0;
static FILE        *g_target_hdr_fp = NULL;
static char        *g_target_path   = NULL;
static char        *g_src_path      = NULL;

static char         g_unused_buf128[128];      /* cleared, otherwise unused */
static char         g_unused_buf16[16];        /* cleared, otherwise unused */
static char         g_flag_chars[32];
static char         g_default_lib[PATH_MAX];
static char         g_lib_resolved[PATH_MAX];

static int         *g_line_num_ptr  = NULL;
static char         g_line_buf[300];
static scan_ctx_t   g_scan_stack[MAX_SCAN_DEPTH];
static int          g_scan_top      = -1;
static scan_ctx_t  *g_scan_cur      = NULL;
static char         g_scan_raw_mode = 0;

/* Error messages table */
static const char *const k_err_msgs[16] = {
    "program  error: ",                        /* 0 */
    "invalid syntax: ",                        /* 1 */
    "could not open:",                         /* 2 */
    "invalid flag: ",                          /* 3 */
    "invalid library access key for ",         /* 4 */
    "invalid library interface ",              /* 5 */
    "bad library file:  ",                     /* 6 */
    "device not in library: ",                 /* 7 */
    "inconsistent library access key ",        /* 8 */
    "inconsistent library rev ",               /* 9 */
    "too many devices for target library: ",   /* 10 */
    "device not in library: ",                 /* 11 */
    "no default library for ",                 /* 12 */
    "too many devices on command line",        /* 13 */
    "write error:  ",                          /* 14 */
    "out of memory:  "                         /* 15 */
};

/* Usage strings */
static const char *const k_usage_flags[] = {
    "-b   generate library using build file",
    "-e   list valid extensions",
    "-l   list long contents of library",
    "-t   list contents of library",
    "-u   use specified library for listings",
    NULL
};

/* Build file keywords */
static const char *const k_build_keywords[] = {
    "target",
    "source",
    NULL
};

/* Forward declarations */
static void fatal_error(int16_t code, const char *extra);
static void error_msg(int code, const char *extra);
static void note_msg(int code, const char *extra);
static void cleanup_target_on_error(void);

/* -------------------------------------------------------------------------
 * Memory allocation helpers
 * ------------------------------------------------------------------------- */
static void *cbld_calloc(size_t count, size_t size) {
    size_t total = count * size;
    void *p = malloc(total);
    if (p != NULL) {
        memset(p, 0, total);
    }
    return p;
}

/* -------------------------------------------------------------------------
 * Path manipulation helpers
 * ------------------------------------------------------------------------- */
/* dir_prefix + file name of base_path without directory and extension +
 * ext_suffix; '/', '\\' and ':' end a directory part */
static char *build_path(const char *dir_prefix, const char *base_path, const char *ext_suffix) {
    const char *slash = strrchr(base_path, '/');
    const char *bslash = strrchr(base_path, '\\');
    const char *colon = strrchr(base_path, ':');
    const char *base_in = base_path;
    if (bslash != NULL && (slash == NULL || bslash > slash)) {
        slash = bslash;
    }
    if (colon != NULL && (slash == NULL || colon > slash)) {
        slash = colon;
    }
    if (slash != NULL) {
        base_in = slash + 1;
    }

    const char *dot = strrchr(base_in, '.');
    size_t stem_len = (dot != NULL) ? (size_t)(dot - base_in) : strlen(base_in);

    size_t out_len = strlen(dir_prefix) + stem_len + strlen(ext_suffix) + 1;
    char *out = (char *)malloc(out_len);
    if (out == NULL) {
        fatal_error(15, "initialization");
    }
    strcpy(out, dir_prefix);
    memcpy(out + strlen(dir_prefix), base_in, stem_len);
    out[strlen(dir_prefix) + stem_len] = '\0';
    strcat(out, ext_suffix);
    return out;
}

/* raw_path (slashes normalised) with its extension replaced by ".dl" */
static char *build_dl_filepath(const char *raw_path) {
    char norm[PATH_MAX];
    lincupl_normalize_slashes(raw_path, norm, sizeof(norm));
    const char *slash = strrchr(norm, '/');
    const char *base = (slash != NULL) ? (slash + 1) : norm;
    const char *dot = strrchr(base, '.');
    size_t prefix_len = (dot != NULL) ? (size_t)(dot - norm) : strlen(norm);
    char *out = (char *)malloc(prefix_len + 4);
    if (out == NULL) {
        fatal_error(15, "initialization");
    }
    memcpy(out, norm, prefix_len);
    memcpy(out + prefix_len, ".dl", 4);
    return out;
}

/* -------------------------------------------------------------------------
 * Error / Warning reporting
 *
 * Messages look like "[NNNNba] line(N), <text> <extra>"; codes of 1000 and
 * more are internal errors and use message 0.
 * ------------------------------------------------------------------------- */
static void format_error(FILE *stream, const char *fmt_tail, int msg_idx, int err_num, const char *extra) {
    fprintf(stream, "[%04d%c%c] ", err_num, 'b', 'a');
    if (g_line_num_ptr != NULL) {
        fprintf(stream, "line(%d), ", *g_line_num_ptr);
    }
    fprintf(stream, "%s ", k_err_msgs[msg_idx]);
    fprintf(stream, fmt_tail, extra != NULL ? extra : "");
}

static void format_note(FILE *stream, const char *fmt_tail, int msg_idx, int err_num, const char *extra) {
    fprintf(stream, "[%04d%c%c] ", err_num, 'b', 'a');
    if (g_line_num_ptr != NULL) {
        fprintf(stream, "line(%d), ", *g_line_num_ptr);
    }
    fprintf(stream, "Please note: %s ", k_err_msgs[msg_idx]);
    fprintf(stream, fmt_tail, extra != NULL ? extra : "");
}

static void dispatch_diag(int code, const char *fmt_tail, int is_note, const char *extra) {
    int msg_idx;
    int err_num;
    if (code >= 1000) {
        msg_idx = 0;
        err_num = code;
    } else {
        msg_idx = code;
        err_num = code;
    }
    if (is_note) {
        format_note(stdout, fmt_tail, msg_idx, err_num, extra);
    } else {
        format_error(stdout, fmt_tail, msg_idx, err_num, extra);
    }
}

static void error_msg(int code, const char *extra) {
    g_err_count++;
    dispatch_diag(code, "%s\n", 0, extra);
}

static void note_msg(int code, const char *extra) {
    dispatch_diag(code, "%s\n", 1, extra);
}

static void cleanup_target_on_error(void) {
    if (g_target_path != NULL && g_target_fp != NULL) {
        fclose(g_target_fp);
        g_target_fp = NULL;
        remove(g_target_path);
    }
}

/* reports error `code` (none if negative), removes a partly written target
 * library and exits with status 1 */
static void fatal_error(int16_t code, const char *extra) {
    if (code >= 0) {
        error_msg((int)code, extra);
    }
    cleanup_target_on_error();
    exit(1);
}

/* -------------------------------------------------------------------------
 * .dl Library I/O functions
 * ------------------------------------------------------------------------- */
static int write_lib_header(uint8_t *hdr_buf, FILE *fp) {
    if (fwrite(hdr_buf, DL_HEADER_SIZE, 1, fp) == 1) {
        free(hdr_buf);
        return 1;
    }
    return 0;
}

/* opens a library and checks its header: DL_OK, DL_CANNOT_OPEN, 4 = bad
 * interface version, or a result of dl_read_header() */
static int open_and_check_lib(int16_t interface_ver, const char *path, dl_handle_t *lib) {
    lib->field_1c = -1;
    lib->field_20 = -1;
    lib->num_sections = DL_SECTIONS;
    lib->field_28 = 0;
    if (interface_ver != 1) {
        return 4;
    }
    if (path == NULL || path[0] == '\0') {
        return DL_CANNOT_OPEN;
    }
    lib->fp = lincupl_fopen_ci(path, "rb");
    if (lib->fp == NULL) {
        return DL_CANNOT_OPEN;
    }
    lib->hdr_buf = (uint8_t *)malloc(DL_HEADER_SIZE);
    if (lib->hdr_buf == NULL) {
        fatal_error(15, "device library initialization");
    }
    return dl_read_header(lib->fp, lib->hdr_buf);
}

static int write_lib_directory(const uint8_t *dir_buf, FILE *fp) {
    if (fwrite(dir_buf, DL_ENTRY_SIZE, DL_MAX_ENTRIES, fp) == DL_MAX_ENTRIES) {
        return 1;
    }
    return 0;
}

static int read_lib_directory(dl_handle_t *lib) {
    lib->dir_buf = (uint8_t *)cbld_calloc(DL_MAX_ENTRIES, DL_ENTRY_SIZE);
    if (lib->dir_buf == NULL) {
        fatal_error(15, "device library processing");
    }
    if (fread(lib->dir_buf, DL_ENTRY_SIZE, DL_MAX_ENTRIES, lib->fp) == DL_MAX_ENTRIES) {
        return 1;
    }
    return 0;
}

/* decodes the 13 packed section headers at the start of the device record */
static void unpack_sec_headers(dl_handle_t *lib) {
    if (lib->rec_buf == NULL) {
        return;
    }
    for (int i = 0; i < DL_SECTIONS; i++) {
        const uint8_t *p = lib->rec_buf + (i * 20);
        lib->sections[i].ord   = p[0];
        lib->sections[i].seg   = p[1];
        lib->sections[i].max   = rd16(p + 2);
        lib->sections[i].cnt   = rd16(p + 4);
        lib->sections[i].rsize = rd16(p + 6);
        lib->sections[i].roff  = rd32(p + 8);
        lib->sections[i].ptr1  = NULL;
        lib->sections[i].ptr2  = NULL;
    }
}

/* seek to cur_dir_entry offset and read device record */
static int read_device_record_seek(dl_handle_t *lib) {
    lib->field_24 = 0;
    uint32_t rec_size = dl_image_size(lib->cur_dir_entry);
    if (!dl_seek_image(lib->fp, lib->cur_dir_entry)) {
        return 0;
    }
    lib->rec_buf = (uint8_t *)malloc(rec_size ? rec_size : 1);
    if (lib->rec_buf == NULL) {
        fatal_error(15, "device library processing");
    }
    size_t got = dl_read_image(lib->fp, lib->rec_buf, rec_size);
    unpack_sec_headers(lib);
    return (got == rec_size && ferror(lib->fp) == 0) ? 1 : 0;
}

/* find device in directory (the name is lower-cased in place) */
static int find_device_in_dir(char *dev_name, dl_handle_t *lib) {
    lincupl_strlwr(dev_name);
    uint8_t *entry = lib->dir_buf;
    for (int i = 0; i < DL_MAX_ENTRIES; i++, entry += DL_ENTRY_SIZE) {
        if ((entry[DL_ENTRY_FLAGS] & 1u) != 0 && strcmp(dev_name, (const char *)entry) == 0) {
            lib->cur_dir_entry = entry;
            return 1;
        }
    }
    return 0;
}

/* write device record in pass 1 and free buffer */
static int write_device_record(uint8_t *buf, uint32_t size, FILE *fp) {
    uint8_t *orig_buf = buf;
    int ok = 1;
    if (g_build_pass != 0) {
        uint32_t rem = size;
        while (rem > 0) {
            size_t n = fwrite(buf, 1, rem, fp);
            if (n == 0 || ferror(fp)) {
                ok = 0;
                break;
            }
            buf += n;
            rem -= (uint32_t)n;
        }
    }
    if (orig_buf != NULL) {
        free(orig_buf);
    }
    return ok;
}

/* open library or exit with corresponding error */
static void open_lib_or_die(dl_handle_t *lib, const char *path) {
    int rc = open_and_check_lib(1, path, lib);
    switch (rc) {
    case DL_OK:
        break;
    case DL_CANNOT_OPEN:
        fatal_error(2, path);
        break;
    case DL_NO_HEADER:
    case DL_BAD_MAGIC:
        fatal_error(6, path);
        break;
    case 4:
        fatal_error(5, "");
        break;
    case DL_BAD_KEY:
        fatal_error(4, path);
        break;
    default:
        fatal_error(1003, "");
        break;
    }
}

/* -------------------------------------------------------------------------
 * Library build functions (-b)
 *
 * The build file is processed twice.  Pass 0 creates the target library and
 * writes its header and directory; pass 1 appends the device records.
 * ------------------------------------------------------------------------- */
/* starts a target library: its name becomes <target>.dl and the header is
 * prepared */
static void init_target_lib(const char *target_name) {
    if (g_target_path != NULL) {
        free(g_target_path);
    }
    g_target_path = build_dl_filepath(target_name);
    if (g_build_pass == 0) {
        g_target_hdr_fp = lincupl_fopen_ci(g_target_path, "wb");
        if (g_target_hdr_fp == NULL) {
            fatal_error(2, g_target_path);
        }
        fclose(g_target_hdr_fp);
    } else {
        g_target_fp = lincupl_fopen_ci(g_target_path, "ab");
        if (g_target_fp == NULL) {
            fatal_error(2, g_target_path);
        }
    }
    g_target_dir = (uint8_t *)cbld_calloc(DL_MAX_ENTRIES, DL_ENTRY_SIZE);
    if (g_target_dir == NULL) {
        fatal_error(15, "building target library");
    }
    g_target_hdr = (uint8_t *)malloc(DL_HEADER_SIZE);
    if (g_target_hdr == NULL) {
        fatal_error(15, "building target library");
    }
    memset(g_target_hdr, 0, DL_HEADER_SIZE);
    g_target_hdr[0x15] = 0;
    g_target_hdr[0x26] = 0;
    wr16(g_target_hdr + 2, 0);
    char *lib_base = build_path("", target_name, "");
    snprintf((char *)(g_target_hdr + 4), 17, "%s", lib_base);
    free(lib_base);
    g_target_dev_cnt = 0;
    g_target_dir_cur = g_target_dir;
}

static void reset_build_state(void) {
    g_target_fp      = NULL;
    g_target_dir     = NULL;
    g_target_dev_cnt = 0;
    g_target_dir_cur = NULL;
    g_src_lib.dir_buf       = NULL;
    g_src_lib.cur_dir_entry = NULL;
    g_target_path    = NULL;
    g_src_path       = NULL;
    g_target_hdr_fp  = NULL;
}

/* appends a copy of a source directory entry to the target directory,
 * placing its record after the previous one (at most 999 devices) */
static void add_dir_entry_to_target(const uint8_t *src_entry) {
    if (g_target_dev_cnt >= 999) {
        fatal_error(10, g_target_path);
    }
    uint8_t *dst = g_target_dir_cur;
    if (g_target_dev_cnt == 0) {
        wr32(dst + 0x16, DL_DATA_START);
    } else {
        dst = g_target_dir_cur + DL_ENTRY_SIZE;
        uint32_t prev_off  = rd32(g_target_dir_cur + DL_ENTRY_OFFSET);
        uint32_t prev_size = rd32(g_target_dir_cur + DL_ENTRY_LENGTH);
        wr32(dst + 0x16, prev_off + prev_size);
        g_target_dir_cur = dst;
    }
    strcpy((char *)dst, (const char *)src_entry);
    dst[DL_ENTRY_FLAGS] = src_entry[DL_ENTRY_FLAGS];
    wr32(dst + 0x12, rd32(src_entry + DL_ENTRY_LENGTH));
    g_target_dev_cnt++;
}

/* opens a source library and takes its access key, revision strings and
 * magic number into the target header (with a note when they differ) */
static void open_source_lib(const char *src_name) {
    if (g_src_path != NULL) {
        free(g_src_path);
    }
    g_src_path = build_dl_filepath(src_name);
    if (g_target_hdr == NULL) {
        fatal_error(1007, NULL);
    }
    open_lib_or_die(&g_src_lib, g_src_path);
    uint16_t tgt_key = rd16(g_target_hdr + 2);
    uint16_t src_key = rd16(g_src_lib.hdr_buf + 2);
    if (tgt_key != 0 && tgt_key != src_key) {
        note_msg(8, "");
    }
    wr16(g_target_hdr + 2, src_key);
    if (g_target_hdr[0x15] != '\0') {
        if (strcmp((const char *)(g_src_lib.hdr_buf + 0x15),
                   (const char *)(g_target_hdr + 0x15)) != 0) {
            note_msg(9, "");
        }
    }
    strcpy((char *)(g_target_hdr + 0x15), (const char *)(g_src_lib.hdr_buf + 0x15));
    strcpy((char *)(g_target_hdr + 0x26), (const char *)(g_src_lib.hdr_buf + 0x26));
    wr16(g_target_hdr, rd16(g_src_lib.hdr_buf));
    if (!read_lib_directory(&g_src_lib)) {
        fatal_error(6, g_src_path);
    }
}

static void close_source_lib(void) {
    if (g_src_lib.hdr_buf != NULL) {
        free(g_src_lib.hdr_buf);
        g_src_lib.hdr_buf = NULL;
    }
    if (g_src_lib.dir_buf != NULL) {
        free(g_src_lib.dir_buf);
        g_src_lib.dir_buf = NULL;
    }
    if (g_src_lib.fp != NULL) {
        fclose(g_src_lib.fp);
        g_src_lib.fp = NULL;
    }
}

/* copies one device from the source library to the target */
static void copy_single_device(char *dev_name) {
    if (!find_device_in_dir(dev_name, &g_src_lib)) {
        error_msg(11, dev_name);
        return;
    }
    read_device_record_seek(&g_src_lib);
    add_dir_entry_to_target(g_src_lib.cur_dir_entry);
    uint32_t rec_size = rd32(g_src_lib.cur_dir_entry + DL_ENTRY_LENGTH);
    if (!write_device_record(g_src_lib.rec_buf, rec_size, g_target_fp)) {
        fatal_error(14, g_target_path);
    }
    g_src_lib.rec_buf = NULL;
}

/* copies every device of the source library ("*" in the build file) */
static void copy_all_devices(void) {
    g_src_lib.cur_dir_entry = g_src_lib.dir_buf;
    for (int i = 0; i < DL_MAX_ENTRIES; i++) {
        if ((g_src_lib.cur_dir_entry[DL_ENTRY_FLAGS] & 1u) != 0) {
            read_device_record_seek(&g_src_lib);
            add_dir_entry_to_target(g_src_lib.cur_dir_entry);
            uint32_t rec_size = rd32(g_src_lib.cur_dir_entry + DL_ENTRY_LENGTH);
            if (!write_device_record(g_src_lib.rec_buf, rec_size, g_target_fp)) {
                fatal_error(14, g_target_path);
            }
            g_src_lib.rec_buf = NULL;
        }
        g_src_lib.cur_dir_entry += DL_ENTRY_SIZE;
    }
}

/* finishes the current target library: pass 0 writes its header and
 * directory, pass 1 closes the record file */
static void flush_target_lib(void) {
    if (g_target_dev_cnt != 0 && g_target_dir != NULL) {
        if (g_build_pass == 0) {
            g_target_hdr_fp = lincupl_fopen_ci(g_target_path, "ab");
            if (g_target_hdr_fp == NULL) {
                fatal_error(2, g_target_path);
            }
            if (!write_lib_header(g_target_hdr, g_target_hdr_fp)) {
                fatal_error(14, g_target_path);
            }
            g_target_hdr = NULL;
            if (!write_lib_directory(g_target_dir, g_target_hdr_fp)) {
                fatal_error(14, g_target_path);
            }
            fclose(g_target_hdr_fp);
        } else if (g_target_hdr != NULL) {
            free(g_target_hdr);
            g_target_hdr = NULL;
        }
        close_source_lib();
        if (g_build_pass != 0 && g_target_fp != NULL) {
            fclose(g_target_fp);
            g_target_fp = NULL;
        }
        free(g_target_dir);
        g_target_dir     = NULL;
        g_target_dev_cnt = 0;
    }
}

static void close_list_lib(void) {
    if (g_list_lib.fp != NULL) {
        fclose(g_list_lib.fp);
        g_list_lib.fp = NULL;
    }
    if (g_list_lib.hdr_buf != NULL) {
        free(g_list_lib.hdr_buf);
        g_list_lib.hdr_buf = NULL;
    }
    if (g_list_lib.dir_buf != NULL) {
        free(g_list_lib.dir_buf);
        g_list_lib.dir_buf = NULL;
    }
}

/* -------------------------------------------------------------------------
 * Listing functions (-t, -l, -e)
 * ------------------------------------------------------------------------- */
static void print_lib_rev_banner(dl_handle_t *lib) {
    printf("%s  rev:%s\n\n", g_lib_path, (const char *)(lib->hdr_buf + 0x15));
}

static void print_long_header(void) {
    printf("%-12s  %-3s   %-4s  %-5s  %-6s\n", "Device", "Rev", "Pins", "Fuses", "Pterms");
    printf("------------  ---   ----  -----  ------\n");
}

static void print_long_device_row(const char *dev_name, const uint8_t *sec3_data) {
    uint16_t rev    = rd16(sec3_data + 0x00);
    uint8_t  pins   = sec3_data[0x08];
    uint32_t fuses  = rd32(sec3_data + 0x15);
    uint16_t pterms = rd16(sec3_data + 0x1d);
    printf("%-12s   %02d   %3d   %5lu   %4u\n",
           dev_name, (int)rev, (int)pins, (unsigned long)fuses, (unsigned int)pterms);
}

static void print_ext_header(void) {
    printf("%-12s  %s\n", "Device", "Extensions");
    printf("------------  ----------------------\n");
}

static void print_ext_device_row(const char *dev_name) {
    uint32_t roff = rd32(g_list_lib.rec_buf + 0x58);
    uint16_t cnt  = rd16(g_list_lib.rec_buf + 0x54);
    const char *ext_ptr = (const char *)(g_list_lib.rec_buf + roff);
    printf("%-12s   ", dev_name);
    for (int i = 0; i < (int)cnt; i++) {
        char buf[32];
        strcpy(buf, ext_ptr);
        lincupl_strupr(buf);
        printf("%s ", buf);
        ext_ptr += 0x15;
    }
    printf("\n");
}

/* -t list contents of library */
static void list_devices_short(void) {
    int has_filter = (g_dev_filter[3] != NULL) ? 1 : 0;
    int idx = 0;

    open_lib_or_die(&g_list_lib, g_lib_path);
    if (!read_lib_directory(&g_list_lib)) {
        fatal_error(6, g_lib_path);
    }
    print_lib_rev_banner(&g_list_lib);

    uint8_t *entry = g_list_lib.dir_buf;
    if (has_filter) {
        for (int i = 3; i < 16 && g_dev_filter[i] != NULL; i++) {
            g_dev_found[i] = 0;
        }
    }

    for (;;) {
        int col = 0;
        do {
            while (idx < DL_MAX_ENTRIES && (entry[DL_ENTRY_FLAGS] & 1u) == 0) {
                entry += DL_ENTRY_SIZE;
                idx++;
            }
            if (idx >= DL_MAX_ENTRIES) {
                close_list_lib();
                break;
            }
            if (!has_filter) {
                printf("%-12s  ", (const char *)entry);
                col++;
            } else {
                for (int i = 3; i < 16 && g_dev_filter[i] != NULL; i++) {
                    if (strcmp((const char *)entry, g_dev_filter[i]) == 0) {
                        g_dev_found[i] = 1;
                        printf("%-12s  ", (const char *)entry);
                        col++;
                    }
                }
            }
            entry += DL_ENTRY_SIZE;
            idx++;
        } while (col < 5);

        printf("\n");
        if (idx >= DL_MAX_ENTRIES) {
            if (has_filter) {
                for (int i = 3; i < 16 && g_dev_filter[i] != NULL; i++) {
                    if (g_dev_found[i] == 0) {
                        note_msg(7, g_dev_filter[i]);
                    }
                }
            }
            return;
        }
    }
}

/* -l list long contents of library */
static void list_devices_long(void) {
    int has_filter = (g_dev_filter[3] != NULL) ? 1 : 0;

    open_lib_or_die(&g_list_lib, g_lib_path);
    if (!read_lib_directory(&g_list_lib)) {
        fatal_error(6, g_lib_path);
    }
    print_lib_rev_banner(&g_list_lib);
    g_list_lib.cur_dir_entry = g_list_lib.dir_buf;
    print_long_header();

    if (has_filter) {
        for (int i = 3; i < 16 && g_dev_filter[i] != NULL; i++) {
            g_dev_found[i] = 0;
        }
    }

    for (int idx = 0; idx < DL_MAX_ENTRIES; idx++) {
        if (rd32(g_list_lib.cur_dir_entry + DL_ENTRY_LENGTH) == 0) {
            break;
        }
        if (!has_filter) {
            if ((g_list_lib.cur_dir_entry[DL_ENTRY_FLAGS] & 1u) != 0) {
                read_device_record_seek(&g_list_lib);
                uint32_t sec3_off = rd32(g_list_lib.rec_buf + DL_INFO_OFFSET);
                print_long_device_row((const char *)g_list_lib.cur_dir_entry,
                                      g_list_lib.rec_buf + sec3_off);
                free(g_list_lib.rec_buf);
                g_list_lib.rec_buf = NULL;
            }
        } else {
            for (int i = 3; i < 16 && g_dev_filter[i] != NULL; i++) {
                if ((g_list_lib.cur_dir_entry[DL_ENTRY_FLAGS] & 1u) != 0 &&
                    strcmp((const char *)g_list_lib.cur_dir_entry, g_dev_filter[i]) == 0) {
                    read_device_record_seek(&g_list_lib);
                    g_dev_found[i] = 1;
                    uint32_t sec3_off = rd32(g_list_lib.rec_buf + DL_INFO_OFFSET);
                    print_long_device_row((const char *)g_list_lib.cur_dir_entry,
                                          g_list_lib.rec_buf + sec3_off);
                    free(g_list_lib.rec_buf);
                    g_list_lib.rec_buf = NULL;
                }
            }
        }
        g_list_lib.cur_dir_entry += DL_ENTRY_SIZE;
    }

    if (has_filter) {
        printf("\n");
        for (int i = 3; i < 16 && g_dev_filter[i] != NULL; i++) {
            if (g_dev_found[i] == 0) {
                note_msg(7, g_dev_filter[i]);
            }
        }
    }
    close_list_lib();
}

/* -e list valid extensions */
static void list_devices_ext(void) {
    int has_filter = (g_dev_filter[3] != NULL) ? 1 : 0;

    open_lib_or_die(&g_list_lib, g_lib_path);
    if (!read_lib_directory(&g_list_lib)) {
        fatal_error(6, g_lib_path);
    }
    print_lib_rev_banner(&g_list_lib);
    g_list_lib.cur_dir_entry = g_list_lib.dir_buf;
    print_ext_header();

    if (has_filter) {
        for (int i = 3; i < 16 && g_dev_filter[i] != NULL; i++) {
            g_dev_found[i] = 0;
        }
    }

    for (int idx = 0; idx < DL_MAX_ENTRIES; idx++) {
        if (!has_filter) {
            if ((g_list_lib.cur_dir_entry[DL_ENTRY_FLAGS] & 1u) != 0) {
                read_device_record_seek(&g_list_lib);
                print_ext_device_row((const char *)g_list_lib.cur_dir_entry);
                free(g_list_lib.rec_buf);
                g_list_lib.rec_buf = NULL;
            }
        } else {
            for (int i = 3; i < 16 && g_dev_filter[i] != NULL; i++) {
                if ((g_list_lib.cur_dir_entry[DL_ENTRY_FLAGS] & 1u) != 0 &&
                    strcmp((const char *)g_list_lib.cur_dir_entry, g_dev_filter[i]) == 0) {
                    read_device_record_seek(&g_list_lib);
                    g_dev_found[i] = 1;
                    print_ext_device_row((const char *)g_list_lib.cur_dir_entry);
                    free(g_list_lib.rec_buf);
                    g_list_lib.rec_buf = NULL;
                }
            }
        }
        g_list_lib.cur_dir_entry += DL_ENTRY_SIZE;
    }

    if (has_filter) {
        printf("\n");
        for (int i = 3; i < 16 && g_dev_filter[i] != NULL; i++) {
            if (g_dev_found[i] == 0) {
                note_msg(7, g_dev_filter[i]);
            }
        }
    }
    close_list_lib();
}

/* -------------------------------------------------------------------------
 * Build file lexical analyzer & parser
 *
 * A build file is a sequence of
 *     target <library>  source <library>  <device> | *  ...  [source ...]
 * with C-style block comments.
 * ------------------------------------------------------------------------- */
static void push_scan_ctx(void) {
    if (g_scan_cur == NULL) {
        g_scan_top = 0;
        g_scan_cur = &g_scan_stack[0];
        return;
    }
    g_scan_top++;
    if (g_scan_top >= MAX_SCAN_DEPTH) {
        fatal_error(1001, "");
    }
    g_scan_cur = &g_scan_stack[g_scan_top];
}

static void open_scan_file(const char *path, int max_tok_len) {
    if (path == NULL || path[0] == '\0') {
        fatal_error(2, "");
    }
    push_scan_ctx();
    g_scan_cur->fp = lincupl_fopen_ci(path, "r");
    if (g_scan_cur->fp == NULL) {
        fatal_error(2, path);
    }
    g_scan_cur->filename = (char *)malloc(strlen(path) + 1);
    if (g_scan_cur->filename == NULL) {
        fatal_error(15, "initialization");
    }
    strcpy(g_scan_cur->filename, path);
    g_scan_cur->line_num    = 1;
    g_line_num_ptr          = &g_scan_cur->line_num;
    g_scan_cur->col_num     = 0;
    g_scan_cur->eof_flag    = 0;
    g_scan_cur->max_tok_len = max_tok_len;
}

static int skip_block_comment(void) {
    int c = fgetc(g_scan_cur->fp);
    if (c == '*') {
        if (g_scan_cur->col_num < (int)sizeof(g_line_buf) - 1) {
            g_line_buf[g_scan_cur->col_num] = '*';
        }
        g_scan_cur->col_num++;
        int prev_star = 0;
        for (;;) {
            for (;;) {
                c = fgetc(g_scan_cur->fp);
                if (c == EOF || c == '\n') {
                    g_scan_cur->line_num++;
                    g_scan_cur->col_num = 0;
                    if (c == EOF) {
                        g_scan_cur->eof_flag = 1;
                        return 1;
                    }
                } else {
                    if (g_scan_cur->col_num < (int)sizeof(g_line_buf) - 1) {
                        g_line_buf[g_scan_cur->col_num] = (char)c;
                    }
                    g_scan_cur->col_num++;
                }
                if (c != '*') {
                    break;
                }
                prev_star = 1;
            }
            if (c == '/' && prev_star) {
                break;
            }
            prev_star = 0;
        }
        return 1;
    }
    return 0;
}

/* next token of the build file into out_tok.  Returns 1 for a number, 2 for
 * a word (or any blank-separated text in raw mode), 3 for a word ending in
 * a digit, 4 for '*', 5 for a lone '/', 6 at the end of the file, 0 for
 * any other character. */
static int scan_next_token(char *out_tok, size_t out_cap, char raw_mode) {
    for (;;) {
        if (g_scan_cur == NULL) {
            fatal_error(1002, "");
        }
        if (g_scan_cur->eof_flag != 0) {
            return 6;
        }

        int c;
        for (;;) {
            for (;;) {
                c = fgetc(g_scan_cur->fp);
                if (c != '\n' && c != EOF) {
                    break;
                }
                g_scan_cur->line_num++;
                g_scan_cur->col_num = 0;
                if (c == EOF) {
                    g_scan_cur->eof_flag = 1;
                    return 6;
                }
            }
            if (!isspace((unsigned char)c)) {
                break;
            }
            if (g_scan_cur->col_num < (int)sizeof(g_line_buf) - 1) {
                g_line_buf[g_scan_cur->col_num] = (char)c;
            }
            g_scan_cur->col_num++;
        }

        if (g_scan_cur->col_num < (int)sizeof(g_line_buf) - 1) {
            g_line_buf[g_scan_cur->col_num] = (char)c;
        }
        g_scan_cur->col_num++;

        size_t len = 0;
        if (raw_mode != 0) {
            if (len + 1 < out_cap) {
                out_tok[len++] = (char)c;
            }
            for (;;) {
                c = fgetc(g_scan_cur->fp);
                if (c == EOF || isspace((unsigned char)c)) {
                    break;
                }
                if (g_scan_cur->col_num < (int)sizeof(g_line_buf) - 1) {
                    g_line_buf[g_scan_cur->col_num] = (char)c;
                }
                g_scan_cur->col_num++;
                if (len + 1 < out_cap) {
                    out_tok[len++] = (char)c;
                }
            }
            out_tok[len] = '\0';
            if (c != EOF) {
                ungetc(c, g_scan_cur->fp);
            }
            return 2;
        }

        if (c != '/') {
            if (len + 1 < out_cap) {
                out_tok[len++] = (char)c;
            }
            out_tok[len] = '\0';
        }

        char first_cls = 0;
        char last_cls  = 0;
        if (isalpha((unsigned char)c) || c == '_' || c == '.') {
            first_cls = 2;
        } else if (isdigit((unsigned char)c)) {
            first_cls = 1;
        } else {
            if (strchr("*", c) != NULL) {
                return 4;
            }
            if (c != '/') {
                return 0;
            }
            if (!skip_block_comment()) {
                return 5;
            }
            continue;
        }

        for (;;) {
            c = fgetc(g_scan_cur->fp);
            if (c == EOF) {
                g_scan_cur->eof_flag = 1;
                break;
            }
            if (isalpha((unsigned char)c) || c == '_' || c == '.') {
                last_cls = 2;
            } else if (isdigit((unsigned char)c)) {
                last_cls = 1;
            } else {
                ungetc(c, g_scan_cur->fp);
                break;
            }
            if (g_scan_cur->col_num < (int)sizeof(g_line_buf) - 1) {
                g_line_buf[g_scan_cur->col_num] = (char)c;
            }
            g_scan_cur->col_num++;
            if (len + 1 < out_cap) {
                out_tok[len++] = (char)c;
            }
            if (first_cls == 1 && last_cls == 2) {
                first_cls = 2;
            }
        }
        out_tok[len] = '\0';
        if (first_cls == 2) {
            return (last_cls == 1) ? 3 : 2;
        } else if (first_cls == 1) {
            return (last_cls == 1) ? 1 : 2;
        }
        return 0;
    }
}

static int lookup_build_keyword(char *tok) {
    lincupl_strlwr(tok);
    for (int i = 0; k_build_keywords[i] != NULL; i++) {
        if (strcmp(tok, k_build_keywords[i]) == 0) {
            return i;
        }
    }
    return -1;
}

/* one pass over the build file g_lib_path */
static void process_build_file(void) {
    char tok[PATH_MAX];
    int state = 0;
    int next_state = 0;
    int tok_kind = 0;

    open_scan_file(g_lib_path, 40);
    for (;;) {
        int lex = scan_next_token(tok, sizeof(tok), g_scan_raw_mode);
        if (lex == 6) {
            break;
        }
        if (lex < 2) {
            fatal_error(1, tok);
        } else if (lex == 2 || lex == 3) {
            int kw = lookup_build_keyword(tok);
            tok_kind = (kw < 0) ? 0 : (kw + 2);
        } else if (lex == 4) {
            if (tok[0] == '*') {
                tok_kind = 1;
            } else {
                fatal_error(1, tok);
            }
        } else if (lex == 5) {
            fatal_error(1, tok);
        } else {
            fatal_error(1004, "");
        }

        switch (state) {
        case 0:
            if (tok_kind != 2) {
                fatal_error(1, tok);
            }
            next_state = 1;
            g_scan_raw_mode = 1;
            flush_target_lib();
            break;
        case 1:
            if (tok_kind != 0) {
                fatal_error(1, tok);
            }
            next_state = 2;
            g_scan_raw_mode = 0;
            init_target_lib(tok);
            break;
        case 2:
            if (tok_kind != 3) {
                fatal_error(1, tok);
            }
            next_state = 3;
            g_scan_raw_mode = 1;
            close_source_lib();
            break;
        case 3:
            if (tok_kind != 0) {
                fatal_error(1, tok);
            }
            next_state = 4;
            g_scan_raw_mode = 0;
            open_source_lib(tok);
            break;
        case 4:
            if (tok_kind == 0) {
                next_state = 4;
                copy_single_device(tok);
            } else if (tok_kind == 1) {
                next_state = 4;
                copy_all_devices();
            } else if (tok_kind == 2) {
                next_state = 1;
                g_scan_raw_mode = 1;
                flush_target_lib();
            } else if (tok_kind == 3) {
                next_state = 3;
                g_scan_raw_mode = 1;
                close_source_lib();
            } else {
                fatal_error(1, tok);
            }
            break;
        default:
            fatal_error(1005, "");
            break;
        }
        state = next_state;
    }
    flush_target_lib();
    while (g_scan_top >= 0) {
        if (g_scan_stack[g_scan_top].fp != NULL) {
            fclose(g_scan_stack[g_scan_top].fp);
            g_scan_stack[g_scan_top].fp = NULL;
        }
        if (g_scan_stack[g_scan_top].filename != NULL) {
            free(g_scan_stack[g_scan_top].filename);
            g_scan_stack[g_scan_top].filename = NULL;
        }
        g_scan_top--;
    }
    g_scan_cur = NULL;
    g_line_num_ptr = NULL;
    g_scan_raw_mode = 0;
}

/* -------------------------------------------------------------------------
 * CLI Parsing & Main Entry Point
 * ------------------------------------------------------------------------- */
static void print_usage(void) {
    printf("\n%s\n", "usage: cbld [-flags] [ build | library ] [ devices ]");
    printf("%s\n", "flags:");
    for (int i = 0; k_usage_flags[i] != NULL; i++) {
        printf("%10s%s\n", " ", k_usage_flags[i]);
    }
}

static void init_globals(void) {
    memset(g_unused_buf128, 0, sizeof(g_unused_buf128));
    memset(g_unused_buf16, 0, sizeof(g_unused_buf16));
    memset(g_flag_chars, 0, sizeof(g_flag_chars));
    memset(g_default_lib, 0, sizeof(g_default_lib));
    memset(g_dev_filter, 0, sizeof(g_dev_filter));
}

static void parse_flag_string(const char *s) {
    size_t idx = strlen(g_flag_chars);
    for (; *s != '\0'; s++) {
        char c = (char)tolower((unsigned char)*s);
        if (idx + 1 < sizeof(g_flag_chars)) {
            g_flag_chars[idx++] = c;
            g_flag_chars[idx] = '\0';
        }
        if (c == 'b') {
            g_flags |= 0x01u;
        } else if (c == 't') {
            g_flags |= 0x02u;
        } else if (c == 'u') {
            g_flags |= 0x04u;
        } else if (c == 'l') {
            g_flags |= 0x08u;
        } else if (c == 'm') {
            g_flags |= 0x10u;
        } else if (c == 'e') {
            g_flags |= 0x20u;
        } else {
            char bad[2] = { *s, '\0' };
            fatal_error(3, bad);
        }
    }
}

/* collects the flags of all "-..." arguments; the argument after the last
 * flag is the build file (-b) or, with -u, the library; the rest name the
 * devices to list */
static void parse_cli(int argc, char **argv) {
    g_flag_chars[0] = '\0';
    g_unused_buf16[0] = '\0';
    g_flags = 0;

    int last_flag_idx = 0;
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            if (argv[i][0] == '-') {
                parse_flag_string(argv[i] + 1);
                last_flag_idx = i;
            }
        }
        if ((g_flags & 0x01u) != 0) {
            if (last_flag_idx + 1 < argc) {
                g_lib_path = argv[last_flag_idx + 1];
            }
        } else {
            if ((g_flags & 0x04u) != 0) {
                last_flag_idx++;
                if (last_flag_idx < argc) {
                    lincupl_resolve_ci_path(argv[last_flag_idx], g_lib_resolved, sizeof(g_lib_resolved));
                    g_lib_path = g_lib_resolved;
                }
            }
            int rem = (argc - last_flag_idx) - 1;
            if (rem > 0) {
                int slot = 3;
                while (slot < 16 && last_flag_idx < argc - 1) {
                    last_flag_idx++;
                    snprintf(g_dev_filter_buf[slot], sizeof(g_dev_filter_buf[slot]), "%s", argv[last_flag_idx]);
                    lincupl_strlwr(g_dev_filter_buf[slot]);
                    g_dev_filter[slot] = g_dev_filter_buf[slot];
                    slot++;
                }
                if (last_flag_idx < argc - 1) {
                    note_msg(13, "");
                    printf("\n");
                }
            }
        }
    }
}

/* the default device library when -u is not given; exits if there is none */
static void resolve_default_lib(void) {
    if (lincupl_find_default_dl(g_default_lib, sizeof(g_default_lib))) {
        g_lib_path = g_default_lib;
    } else {
        const char *env = getenv("LIBCUPL");
        if (env != NULL && env[0] != '\0') {
            fatal_error(2, env);
        }
        fatal_error(12, "cbld");
    }
    if (access(g_lib_path, R_OK) != 0) {
        fatal_error(2, g_lib_path);
    }
}

int main(int argc, char **argv) {
    lincupl_init_file_mode();
    init_globals();

    printf("%s %s cbld: CUPL Device Library Management Program\n%s\n",
           PROGRAM_NAME, PROGRAM_VERSION, PROGRAM_COPYRIGHT);

    if (argc == 1) {
        print_usage();
        return 0;
    }

    parse_cli(argc, argv);

    if ((g_flags & 0x04u) == 0 &&
        ((g_flags & 0x02u) != 0 || (g_flags & 0x20u) != 0 ||
         (g_flags & 0x08u) != 0 || (g_flags & 0x10u) != 0)) {
        resolve_default_lib();
    }

    if ((g_flags & 0x01u) != 0) {
        g_build_pass = 0;
        reset_build_state();
        process_build_file();
        g_build_pass = 1;
        reset_build_state();
        process_build_file();
        if (g_target_path != NULL) {
            free(g_target_path);
            g_target_path = NULL;
        }
        if (g_src_path != NULL) {
            free(g_src_path);
            g_src_path = NULL;
        }
    } else if ((g_flags & 0x08u) != 0) {
        list_devices_long();
    } else if ((g_flags & 0x20u) != 0) {
        list_devices_ext();
    } else if ((g_flags & 0x02u) != 0) {
        list_devices_short();
    } else {
        print_usage();
    }

    return (g_err_count != 0) ? 1 : 0;
}
