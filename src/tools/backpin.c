/*
 * backpin - back-annotation of pin placements into a CUPL source file.
 *
 * Copies <source>.pld to a backup file, reads the pin placements the
 * fitter wrote (<source>.plc, or <source>.ppn for fitter type 4 devices),
 * and writes <source>.pld again from the backup with the PIN and PINNODE
 * statements carrying the placed pin numbers; NODE statements are copied as
 * they are.  The device's pin count and fitter type come from the device
 * library.  The new text is collected in memory and written over
 * <source>.pld only after the whole file has been read, so a statement
 * backpin cannot handle leaves the design unchanged.
 */

#include "banner.h"
#include "devlib.h"
#include "locate.h"
#include "strutil.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <io.h>
#define access_check _access
#ifndef R_OK
#define R_OK 4
#endif
#ifndef W_OK
#define W_OK 2
#endif
#else
#include <unistd.h>
#define access_check access
#endif

/* Token types returned by lex_token */
#define TOK_EOF     (-1)
#define TOK_NUMBER  0x100
#define TOK_IDENT   0x101
#define TOK_PIN     0x102
#define TOK_NODE    0x103
#define TOK_PINNODE 0x104
#define TOK_SEMI    0x105
#define TOK_CMT_BEG 0x106
#define TOK_CMT_END 0x107

/* Error and warning message table */
static const char *const k_error_msgs[] = {
    "program error:  ",                       /* 0  (0x00) */
    "could not open:  ",                      /* 1  (0x01) */
    "could not execute program:  ",           /* 2  (0x02) */
    "disk write error:  ",                    /* 3  (0x03) */
    "out of memory:  ",                       /* 4  (0x04) */
    "illegal character:  ",                   /* 5  (0x05) */
    "unexpected symbol: ",                    /* 6  (0x06) */
    "invalid flag: ",                         /* 7  (0x07) */
    "missing semi-colon on line above",       /* 8  (0x08) */
    "unexpected end-of-line ",                /* 9  (0x09) */
    "existing pin number has been changed: ", /* 10 (0x0a) */
    "pin number out of bounds for: ",         /* 11 (0x0b) */
    "could not find LIBCUPL in ENVIRONMENT",  /* 12 (0x0c) */
    "invalid library access key",             /* 13 (0x0d) */
    "invalid library interface",              /* 14 (0x0e) */
    "bad library file:  ",                    /* 15 (0x0f) */
    "device not in library:  ",               /* 16 (0x10) */
    "target device not specified ",           /* 17 (0x11) */
    "PGA pin numbers are not supported ",     /* 18 (0x12) */
    "the backup would overwrite the source: " /* 19 (0x13) */
};

/* Global state */
static uint8_t g_had_error = 0;
static const char *g_bak_arg = "&";
static const char *g_log_arg = "&";
static const char *g_device = "&";
static const char *g_source = "&";
static const char *g_lib_arg = "&";
static char g_device_buf[128];
static char g_lib_resolved[PATH_MAX];
static int g_line_num = 1;
static int g_collect_out = 0;           /* 1 while the new .pld text is collected */
static char *g_out_buf = NULL;
static size_t g_out_len = 0;
static size_t g_out_cap = 0;

static char g_cur_tok_str[1024] = "";
static int g_in_comment = 0;
static uint8_t g_is_mach = 0;

static char g_line_buf[4096];
static char g_bracket_rhs_buf[2048];

static uint8_t g_flag_log = 0;
static uint8_t g_flag_float = 0;
static uint8_t g_flag_pin_only = 0;

static FILE *g_fp_pld = NULL;
static char *g_pld_path = NULL;
static FILE *g_fp_plc = NULL;
static char *g_plc_path = NULL;
static FILE *g_fp_bak = NULL;
static char *g_bak_path = NULL;
static FILE *g_fp_in = NULL;
static FILE *g_fp_log = NULL;
static uint8_t g_read_mode = 0;         /* 1 = reading .plc/.ppn, 0 = updating .pld */
static char **g_pin_names = NULL;       /* 1000-element array */
static int g_err_cnt = 0;
static char g_prev_tok_str[1024] = "";
static int g_lookahead = 0;
static uint8_t g_fitter_type = 0;
static int g_dev_pin_cnt = 0;

/* Forward declarations */
static void fatal_error(int code, const char *arg, int line_no);

/* print error diagnostic to stdout (or g_fp_log if -l) */
static void print_error_msg(FILE *fp_default, int msg_idx, int err_code, int line_no, const char *arg) {
    FILE *out = (g_flag_log == 0) ? fp_default : g_fp_log;
    if (out == NULL) out = stdout;
    fprintf(out, "[%04d%c%c] ", err_code, 'b', 'p');
    if (line_no > 0) {
        fprintf(out, "line(%d) ", line_no);
    }
    fprintf(out, "%s%s\n", k_error_msgs[msg_idx], arg ? arg : "");
}

/* record and print error */
static void report_error(int err_code, const char *arg, int line_no) {
    g_err_cnt++;
    g_had_error = 1;
    int msg_idx = (err_code < 1000) ? err_code : 0;
    print_error_msg(stdout, msg_idx, err_code, line_no, arg);
}

/* fatal error and exit(1) */
static void fatal_error(int code, const char *arg, int line_no) {
    if (code >= 0) {
        report_error(code, arg, line_no);
    }
    exit(1);
}

/* print warning message */
static void warn_msg(int code, const char *arg) {
    FILE *out = (g_flag_log == 0) ? stdout : g_fp_log;
    if (out == NULL) out = stdout;
    fprintf(out, "[%04d%c%c] ", code, 'b', 'p');
    fprintf(out, "Warning: %s%s\n", k_error_msgs[code], arg ? arg : "");
    fflush(out);
}

/* print command-line usage */
static void print_usage(void) {
    static const char *const k_opts[] = {
        "-b <filename>    specify filename for backup of source file",
        "-p               place pin numbers only into source file",
        "-f               float all pins in source file",
        "-l <filename>    send all screen output to a log file",
        "-u <filename>    use specified library file",
        NULL
    };
    printf("%s\n", "usage: backpin [options] device source");
    printf("%s\n", "options:");
    for (int i = 0; k_opts[i] != NULL; i++) {
        printf("%10s%s\n", " ", k_opts[i]);
    }
    printf("\n%s\n", "device:  CUPL device mnemonic");
    printf("%s\n\n", "source:  CUPL source (PLD) file");
}

/* Parse the command line (without changing the caller's argv): options,
 * then the device and the source.  Returns 0 when the device or the source
 * is missing, so that the usage text is printed. */
static int parse_args(char **argv, int argc) {
    const char *words[2];
    int n_words = 0;
    int i = 1;

    g_flag_log = 0;
    g_flag_float = 0;
    g_flag_pin_only = 0;

    while (i < argc) {
        const char *arg = argv[i++];
        if (arg[0] != '-') {
            if (n_words < 2) {
                words[n_words] = arg;
            }
            n_words++;
            continue;
        }
        char opt = (char)tolower((unsigned char)arg[1]);
        switch (opt) {
        case '\0':
            break;
        case 'b':
            if (i < argc) g_bak_arg = argv[i];
            i++;
            break;
        case 'l':
            g_flag_log = 1;
            if (i < argc) g_log_arg = argv[i];
            i++;
            break;
        case 'f':
            g_flag_float = 1;
            break;
        case 'p':
            g_flag_pin_only = 1;
            break;
        case 'u':
            if (i < argc) g_lib_arg = argv[i];
            i++;
            break;
        default:
        {
            /* the option as given, its letter in lower case */
            char shown[256];
            snprintf(shown, sizeof(shown), "%c%s", opt, arg + 2);
            fatal_error(7, shown, 0);
        }
            break;
        }
    }

    if (n_words != 2) {
        return 0;
    }
    snprintf(g_device_buf, sizeof(g_device_buf), "%s", words[0]);
    lincupl_strlwr(g_device_buf);
    g_device = g_device_buf;
    g_source = words[1];
    return 1;
}

/* read next character from g_fp_in */
static int next_char(void) {
    return fgetc(g_fp_in);
}

/* unget character to g_fp_in */
static void unget_char(int ch) {
    if (ch != EOF) {
        ungetc(ch, g_fp_in);
    }
}

/* return 1 if all chars in s are digits */
static int is_all_digits(const char *s) {
    while (*s != '\0') {
        if (!isdigit((unsigned char)*s)) {
            return 0;
        }
        s++;
    }
    return 1;
}

/* append to a statement buffer; a statement too long for it stops backpin
 * (and leaves the design unchanged) rather than losing text */
static void append_str(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0 || !src) return;
    size_t dlen = strlen(dst);
    size_t slen = strlen(src);
    if (dlen + slen + 1 > cap) {
        fatal_error(4, "statement too long", g_line_num);
    }
    memcpy(dst + dlen, src, slen + 1);
}

/* lookup keyword ("PIN"=0x102, "NODE"=0x103, "PINNODE"=0x104) or return 0x101 */
static int classify_ident(const char *s) {
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", s);
    lincupl_strupr(buf);
    if (strcmp(buf, "PINNODE") == 0) return TOK_PINNODE;
    if (strcmp(buf, "NODE") == 0)    return TOK_NODE;
    if (strcmp(buf, "PIN") == 0)     return TOK_PIN;
    return TOK_IDENT;
}

/* lexical analyzer */
static int lex_token(void) {
    snprintf(g_prev_tok_str, sizeof(g_prev_tok_str), "%s", g_cur_tok_str);
    char *dst = g_cur_tok_str;
    char *dst_end = g_cur_tok_str + sizeof(g_cur_tok_str) - 1;
    int ch = next_char();
    if (ch == EOF) {
        return TOK_EOF;
    }
    if (ch == '\n') {
        g_line_num++;
        g_cur_tok_str[0] = (char)ch;
        g_cur_tok_str[1] = '\0';
        return '\n';
    }
    if (ch == '=') {
        g_cur_tok_str[0] = '=';
        g_cur_tok_str[1] = '\0';
        return '=';
    }
    if (isalnum((unsigned char)ch) || ch == '$' || ch == '_') {
        while (ch != EOF && (isalnum((unsigned char)ch) || ch == '$' || ch == '_')) {
            if (dst < dst_end) {
                *dst++ = (char)ch;
            }
            ch = next_char();
        }
        *dst = '\0';
        unget_char(ch);
        if (is_all_digits(g_cur_tok_str)) {
            return TOK_NUMBER;
        }
        return classify_ident(g_cur_tok_str);
    }
    if (ch == ';') {
        g_cur_tok_str[0] = ';';
        g_cur_tok_str[1] = '\0';
        return TOK_SEMI;
    }
    if (ch == '/') {
        int ch2 = next_char();
        g_cur_tok_str[0] = '/';
        if (ch2 == '*') {
            g_in_comment = 1;
            g_cur_tok_str[1] = '*';
            g_cur_tok_str[2] = '\0';
            return TOK_CMT_BEG;
        }
        unget_char(ch2);
        g_cur_tok_str[1] = '\0';
        return '/';
    }
    if (ch == '*') {
        int ch2 = next_char();
        g_cur_tok_str[0] = '*';
        if (ch2 == '/') {
            g_in_comment = 0;
            g_cur_tok_str[1] = '/';
            g_cur_tok_str[2] = '\0';
            return TOK_CMT_END;
        }
        unget_char(ch2);
        g_cur_tok_str[1] = '\0';
        return '*';
    }
    g_cur_tok_str[0] = (char)ch;
    g_cur_tok_str[1] = '\0';
    return ch;
}

/* match expected token and advance lookahead */
static void match_token(int expected) {
    if (g_lookahead == expected) {
        g_lookahead = lex_token();
        return;
    }
    fatal_error(6, g_cur_tok_str, g_line_num);
}

/* add a string to the new .pld text while it is being collected */
static void emit_str(const char *s) {
    if (!g_collect_out) {
        return;
    }
    size_t n = strlen(s);
    if (g_out_len + n + 1 > g_out_cap) {
        size_t cap = g_out_cap ? g_out_cap : 0x8000;
        while (g_out_len + n + 1 > cap) {
            cap *= 2;
        }
        char *buf = (char *)realloc(g_out_buf, cap);
        if (buf == NULL) {
            fatal_error(4, "file buffer allocation", 0);
        }
        g_out_buf = buf;
        g_out_cap = cap;
    }
    memcpy(g_out_buf + g_out_len, s, n + 1);
    g_out_len += n;
}

/* is the lookahead blank space or the start of a comment? */
static int at_list_space(void) {
    return g_lookahead == ' ' || g_lookahead == '\t' || g_lookahead == '\r' ||
           g_lookahead == '\n' || g_lookahead == TOK_CMT_BEG;
}

/* copy blank space and comments inside a bracketed list to dst */
static void copy_list_space(char *dst, size_t cap) {
    while (at_list_space()) {
        if (g_lookahead == TOK_CMT_BEG) {
            while (g_lookahead != TOK_CMT_END && g_lookahead != TOK_EOF) {
                match_token(g_lookahead);
                append_str(dst, cap, g_prev_tok_str);
            }
        }
        match_token(g_lookahead);
        append_str(dst, cap, g_prev_tok_str);
    }
}

/* resolve library path from -u, LIBCUPL, LINCUPL_SHARE, or relative to binary */
static void resolve_lib_file(void) {
    if (strcmp(g_lib_arg, "&") == 0) {
        if (!lincupl_find_default_dl(g_lib_resolved, sizeof(g_lib_resolved))) {
            fatal_error(12, "", 0);
        }
        g_lib_arg = g_lib_resolved;
    } else {
        lincupl_resolve_explicit_dl(g_lib_arg, g_lib_resolved, sizeof(g_lib_resolved));
        g_lib_arg = g_lib_resolved;
    }
    if (access_check(g_lib_arg, R_OK) != 0) {
        fatal_error(1, g_lib_arg, 0);
    }
}

/* read device pin count and fitter type from .dl */
static int read_device_library(const char *dev_name, const char *lib_path) {
    FILE *fp = lincupl_fopen_ci(lib_path, "rb");
    if (fp == NULL) {
        fatal_error(1, g_lib_arg, 0);
    }

    uint8_t hdr[DL_HEADER_SIZE];
    switch (dl_read_header(fp, hdr)) {
    case DL_OK:
        break;
    case DL_BAD_KEY:
        fclose(fp);
        fatal_error(13, "", 0);
        break;
    default:
        fclose(fp);
        fatal_error(15, g_lib_arg, 0);
        break;
    }

    if (strcmp("&", dev_name) == 0) {
        fclose(fp);
        fatal_error(17, "", 0);
    }

    uint8_t dir_entry[DL_ENTRY_SIZE];
    if (!dl_find_device(fp, dev_name, DL_IGNORE_CASE, dir_entry)) {
        fclose(fp);
        fatal_error(16, dev_name, 0);
    }

    uint32_t rec_size = dl_image_size(dir_entry);
    if (rec_size < 0x48u || !dl_seek_image(fp, dir_entry)) {
        fclose(fp);
        fatal_error(15, dev_name, 0);
    }

    uint8_t *rec_buf = (uint8_t *)malloc(rec_size);
    if (rec_buf == NULL) {
        fclose(fp);
        fatal_error(4, "device library processing", 0);
    }

    if (dl_read_image(fp, rec_buf, rec_size) != rec_size) {
        free(rec_buf);
        fclose(fp);
        fatal_error(15, dev_name, 0);
    }
    fclose(fp);

    uint32_t sec3_roff = rd32(rec_buf + DL_INFO_OFFSET);
    if (sec3_roff > rec_size || rec_size - sec3_roff < 0x13u) {
        free(rec_buf);
        fatal_error(15, dev_name, 0);
    }
    int pin_cnt = (int)rec_buf[sec3_roff + 8];
    g_fitter_type = rec_buf[sec3_roff + 0x12];

    free(rec_buf);
    return pin_cnt;
}

/* allocate 1000-element pin placement array */
static void alloc_pin_placements(void) {
    g_pin_names = (char **)malloc(1000 * sizeof(char *));
    if (g_pin_names == NULL) {
        fatal_error(4, "allocating pin placements", 0);
    }
    for (int i = 0; i < 1000; i++) {
        g_pin_names[i] = NULL;
    }
}

/* store signal name at pin_idx */
static void store_pin_placement(const char *sig_name, int pin_idx) {
    if (pin_idx > 999 || pin_idx < 0) {
        warn_msg(11, sig_name);
        return;
    }
    size_t len = strlen(sig_name);
    g_pin_names[pin_idx] = (char *)malloc(len + 1);
    if (g_pin_names[pin_idx] == NULL) {
        fatal_error(4, "allocating pin placement name", 0);
    }
    strcpy(g_pin_names[pin_idx], sig_name);
}

/* free pin placement array */
static void free_pin_placements(void) {
    if (g_pin_names != NULL) {
        for (int i = 0; i < 1000; i++) {
            if (g_pin_names[i] != NULL) {
                free(g_pin_names[i]);
            }
        }
        free(g_pin_names);
        g_pin_names = NULL;
    }
}

/* lookup pin number by signal name (case-insensitive) */
static int lookup_pin_by_name(const char *sig_name) {
    for (int i = 0; i < 1000; i++) {
        if (g_pin_names[i] != NULL && strcasecmp(sig_name, g_pin_names[i]) == 0) {
            return i;
        }
    }
    return 0;
}

/* open log file */
static void open_log_file(void) {
    g_fp_log = lincupl_fopen_ci(g_log_arg, "w");
    if (g_fp_log == NULL) {
        fatal_error(1, g_log_arg, 0);
    }
}

/* printf to stdout (or log file when -l is active) */
static void out_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (g_flag_log == 0) {
        vfprintf(stdout, fmt, ap);
        fflush(stdout);
    } else {
        vfprintf(g_fp_log, fmt, ap);
        fflush(g_fp_log);
    }
    va_end(ap);
}

/* extract directory prefix (including trailing separator) or "" */
static char *extract_dir(const char *path) {
    size_t len = lincupl_path_dir_len(path);
    if (len == 0) {
        return "";
    }
    char *dir = (char *)malloc(len + 1);
    if (dir == NULL) {
        fatal_error(4, "initialization", 0);
    }
    memcpy(dir, path, len);
    dir[len] = '\0';
    return dir;
}

/* build path from dir + stem(source without "./" and .pld) + ext */
static char *make_filepath(const char *dir, const char *src, const char *ext) {
    if (src[0] == '.' && (src[1] == '/' || src[1] == '\\')) {
        src += 2;
    }
    size_t len = lincupl_design_file_name(NULL, 0, dir, src, ext);
    char *res = (char *)malloc(len + 1);
    if (res == NULL) {
        fatal_error(4, "initialization", 0);
    }
    lincupl_design_file_name(res, len + 1, dir, src, ext);
    return res;
}

static char *resolve_existing_filepath(const char *dir, const char *src, const char *ext) {
    char *cand = make_filepath(dir, src, ext);
    char resolved[PATH_MAX];
    if (lincupl_resolve_ci_path(cand, resolved, sizeof(resolved)) == 0) {
        free(cand);
        char *dup = (char *)malloc(strlen(resolved) + 1);
        if (dup == NULL) {
            fatal_error(4, "initialization", 0);
        }
        strcpy(dup, resolved);
        return dup;
    }
    return cand;
}

/* Is path the file open as fp?  (A -b name that is the source itself would
 * empty it when the backup is created.) */
static int same_file(FILE *fp, const char *path) {
    struct stat a, b;
    char resolved[PATH_MAX];
    if (lincupl_resolve_ci_path(path, resolved, sizeof(resolved)) == 0) {
        path = resolved;
    }
    if (fstat(fileno(fp), &a) != 0 || stat(path, &b) != 0) {
        return 0;
    }
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

/* The backup is the file named with -b, or else <source>.bak. */
static char *make_backup_filepath(const char *dir, const char *src) {
    if (strcmp(g_bak_arg, "&") != 0) {
        char *res = (char *)malloc(strlen(g_bak_arg) + 1);
        if (res == NULL) {
            fatal_error(4, "initialization", 0);
        }
        strcpy(res, g_bak_arg);
        return res;
    }
    return make_filepath(dir, src, ".bak");
}

/* copy source .pld file to .bak file */
static void create_backup(const char *dir, const char *src) {
    uint8_t *buf = (uint8_t *)malloc(0x7fff);
    if (buf == NULL) {
        fatal_error(4, "file buffer allocation", 0);
    }
    char *pld_path = resolve_existing_filepath(dir, src, ".pld");
    FILE *fp_pld = lincupl_fopen_ci(pld_path, "r");
    if (fp_pld == NULL) {
        fatal_error(1, pld_path, 0);
    }
    char *bak_path = make_backup_filepath(dir, src);
    if (same_file(fp_pld, bak_path)) {
        fclose(fp_pld);
        fatal_error(19, bak_path, 0);
    }
    FILE *fp_bak = lincupl_fopen_ci(bak_path, "w");
    if (fp_bak == NULL) {
        fclose(fp_pld);
        fatal_error(1, bak_path, 0);
    }
    size_t n;
    while ((n = fread(buf, 1, 0x7fff, fp_pld)) > 0) {
        fwrite(buf, 1, n, fp_bak);
    }
    if (ferror(fp_bak)) {
        fclose(fp_pld);
        fclose(fp_bak);
        fatal_error(3, bak_path, 0);
    }
    fclose(fp_pld);
    fclose(fp_bak);
    free(bak_path);
    free(pld_path);
    free(buf);
}

/* parse PIN or NODE line in .plc file */
static void parse_plc_pin_or_node(void) {
    int is_node = 0;
    int pin_idx = 0;

    if (g_lookahead == TOK_PIN) {
        match_token(TOK_PIN);
    } else {
        is_node = 1;
        match_token(TOK_NODE);
    }

    while (g_lookahead != TOK_NUMBER && g_lookahead != '\n' && g_lookahead != TOK_EOF) {
        match_token(g_lookahead);
    }
    if (g_lookahead == '\n') {
        fatal_error(9, "", g_line_num);
    }
    match_token(TOK_NUMBER);

    if (is_node && g_is_mach != 0) {
        pin_idx = atoi(g_prev_tok_str) + g_dev_pin_cnt - 1;
    } else {
        pin_idx = atoi(g_prev_tok_str);
    }

    while (g_lookahead != TOK_IDENT && g_lookahead != '\n' && g_lookahead != TOK_EOF) {
        match_token(g_lookahead);
    }
    if (g_lookahead == '\n') {
        fatal_error(9, "", g_line_num);
    }
    match_token(TOK_IDENT);
    store_pin_placement(g_prev_tok_str, pin_idx);

    while (g_lookahead != '\n' && g_lookahead != TOK_EOF && g_lookahead != ';') {
        match_token(g_lookahead);
    }
    if (g_lookahead == '\n') {
        match_token('\n');
    }
}

/* parse signal placement line in .ppn file (fitter_type == 4) */
static void parse_ppn_line(void) {
    char sig_name[1024];
    int pin_idx = 0;

    match_token(TOK_IDENT);
    snprintf(sig_name, sizeof(sig_name), "%s", g_prev_tok_str);

    while (g_lookahead != TOK_IDENT && g_lookahead != '\n' && g_lookahead != TOK_EOF) {
        match_token(g_lookahead);
    }
    match_token(TOK_IDENT);

    if (g_lookahead == '\n' || g_lookahead == TOK_EOF) {
        fatal_error(9, "", g_line_num);
    }

    while (g_lookahead != TOK_NUMBER && g_lookahead != TOK_IDENT &&
           g_lookahead != '\n' && g_lookahead != TOK_EOF) {
        match_token(g_lookahead);
    }
    if (g_lookahead == '\n' || g_lookahead == TOK_EOF) {
        fatal_error(9, "", g_line_num);
    }
    if (g_lookahead == TOK_IDENT) {
        fatal_error(18, "", 0);
    } else {
        match_token(TOK_NUMBER);
        pin_idx = atoi(g_prev_tok_str);
    }

    store_pin_placement(sig_name, pin_idx);

    while (g_lookahead != '\n' && g_lookahead != TOK_EOF) {
        match_token(g_lookahead);
    }
    if (g_lookahead == '\n') {
        match_token('\n');
    }
}

/* set when a name in a bracketed list has no placed pin */
static int g_list_unplaced = 0;

/* placed pin of a name in a bracketed list, 0 when it has none */
static int list_pin(const char *sig_name) {
    int pin = lookup_pin_by_name(sig_name);
    if (pin == 0) {
        g_list_unplaced = 1;
    }
    return pin;
}

/* expand bracketed signal list [sig0..sig3, ...] in .pld */
static int expand_bracket_signals(void) {
    char sig_buf[1060];
    char prefix_buf[1024];
    char num_str[32];
    int count = 1;
    int start_num = 0;
    int end_num = 0;

    copy_list_space(g_bracket_rhs_buf, sizeof(g_bracket_rhs_buf));
    if (g_lookahead == '!') {
        /* an active-low name (or range) in the list */
        match_token('!');
        append_str(g_bracket_rhs_buf, sizeof(g_bracket_rhs_buf), g_prev_tok_str);
        copy_list_space(g_bracket_rhs_buf, sizeof(g_bracket_rhs_buf));
    }
    if (g_lookahead != TOK_IDENT) {
        fatal_error(6, g_cur_tok_str, g_line_num);
    }
    int idx = (int)strlen(g_cur_tok_str) - 1;
    if (idx >= 0 && (isalpha((unsigned char)g_cur_tok_str[idx]) || g_cur_tok_str[idx] == '_')) {
        match_token(TOK_IDENT);
        int pin = list_pin(g_prev_tok_str);
        snprintf(num_str, sizeof(num_str), "%d", pin);
        append_str(g_line_buf, sizeof(g_line_buf), num_str);
        append_str(g_bracket_rhs_buf, sizeof(g_bracket_rhs_buf), g_prev_tok_str);
    } else {
        snprintf(prefix_buf, sizeof(prefix_buf), "%s", g_cur_tok_str);
        while (idx >= 0 && isdigit((unsigned char)g_cur_tok_str[idx])) {
            idx--;
        }
        idx++;
        start_num = atoi(g_cur_tok_str + idx);
        if ((size_t)idx < sizeof(prefix_buf)) {
            prefix_buf[idx] = '\0';
        }
        match_token(TOK_IDENT);
        int pin = list_pin(g_prev_tok_str);
        snprintf(num_str, sizeof(num_str), "%d", pin);
        append_str(g_line_buf, sizeof(g_line_buf), num_str);
        append_str(g_bracket_rhs_buf, sizeof(g_bracket_rhs_buf), g_prev_tok_str);
    }

    copy_list_space(g_bracket_rhs_buf, sizeof(g_bracket_rhs_buf));
    if (g_lookahead == '.') {
        match_token('.');
        match_token('.');
        append_str(g_bracket_rhs_buf, sizeof(g_bracket_rhs_buf), "..");
        copy_list_space(g_bracket_rhs_buf, sizeof(g_bracket_rhs_buf));
        if (g_lookahead == TOK_NUMBER) {
            end_num = atoi(g_cur_tok_str);
            match_token(TOK_NUMBER);
            append_str(g_bracket_rhs_buf, sizeof(g_bracket_rhs_buf), g_prev_tok_str);
        } else if (g_lookahead == TOK_IDENT) {
            int r_idx = (int)strlen(g_cur_tok_str) - 1;
            while (r_idx >= 0 && isdigit((unsigned char)g_cur_tok_str[r_idx])) {
                r_idx--;
            }
            r_idx++;
            end_num = atoi(g_cur_tok_str + r_idx);
            append_str(g_bracket_rhs_buf, sizeof(g_bracket_rhs_buf), g_cur_tok_str);
            match_token(TOK_IDENT);
        } else {
            fatal_error(6, g_cur_tok_str, 0);
        }

        int step;
        if (start_num < end_num) {
            step = 1;
            count = (end_num - start_num) + 1;
        } else {
            step = -1;
            count = (start_num - end_num) + 1;
        }

        copy_list_space(g_bracket_rhs_buf, sizeof(g_bracket_rhs_buf));
        int cur = start_num;
        while (cur != end_num) {
            cur += step;
            snprintf(num_str, sizeof(num_str), "%d", cur);
            snprintf(sig_buf, sizeof(sig_buf), "%s%s", prefix_buf, num_str);
            int pin = list_pin(sig_buf);
            snprintf(num_str, sizeof(num_str), "%d", pin);
            append_str(g_line_buf, sizeof(g_line_buf), ",");
            append_str(g_line_buf, sizeof(g_line_buf), num_str);
        }
    }

    if (g_lookahead == ',') {
        match_token(',');
        append_str(g_line_buf, sizeof(g_line_buf), g_prev_tok_str);
        append_str(g_bracket_rhs_buf, sizeof(g_bracket_rhs_buf), g_prev_tok_str);
        int more = expand_bracket_signals();
        count += more;
    } else if (g_lookahead == ']') {
        match_token(']');
        append_str(g_line_buf, sizeof(g_line_buf), "]");
        append_str(g_bracket_rhs_buf, sizeof(g_bracket_rhs_buf), g_prev_tok_str);
    } else {
        fatal_error(6, g_cur_tok_str, 0);
    }

    return count;
}

/* Rewrite the bracketed list on the right of a PIN or PINNODE statement
 * whose start (" = " or " = !") is in g_bracket_rhs_buf: the pin numbers of
 * its names go in brackets on the left.  When a name has no placed pin (the
 * pins are floated with -f, PINNODE numbers are left out with -p, or the
 * fitter did not place the name) the statement is written without numbers,
 * as a list cannot leave single pins out. */
static void rewrite_pin_list(void) {
    size_t lhs_len = strlen(g_line_buf);
    match_token('[');
    append_str(g_line_buf, sizeof(g_line_buf), " [");
    append_str(g_bracket_rhs_buf, sizeof(g_bracket_rhs_buf), "[");
    g_list_unplaced = 0;
    expand_bracket_signals();
    if (g_list_unplaced) {
        g_line_buf[lhs_len] = '\0';
    }
    append_str(g_line_buf, sizeof(g_line_buf), g_bracket_rhs_buf);
}

/* rewrite PIN or PINNODE statement in .pld file */
static void rewrite_pld_pin_stmt(void) {
    int no_old_pin = 1;
    int spaces_before = 0;
    int spaces_after = 0;
    int is_neg = 0;
    int is_pin = 0;
    int old_pin = 0;
    char pin_str[32];

    if (g_lookahead == TOK_PIN) {
        match_token(TOK_PIN);
        is_pin = 1;
    } else {
        match_token(TOK_PINNODE);
    }

    snprintf(g_line_buf, sizeof(g_line_buf), "%s", g_prev_tok_str);

    while (g_lookahead != '=' && g_lookahead != '\n' && g_lookahead != TOK_EOF) {
        if (g_lookahead == TOK_NUMBER) {
            match_token(TOK_NUMBER);
            old_pin = atoi(g_prev_tok_str);
            no_old_pin = 0;
        } else {
            match_token(g_lookahead);
            old_pin = 0;
            if (no_old_pin) {
                spaces_before++;
            } else {
                spaces_after++;
            }
        }
    }

    if (g_lookahead == '\n') {
        fatal_error(9, "", g_line_num);
    }
    if (g_lookahead == '=') {
        match_token('=');
    } else {
        fatal_error(6, g_cur_tok_str, g_line_num);
    }

    while (g_lookahead != TOK_IDENT && g_lookahead != '[' &&
           g_lookahead != '!' && g_lookahead != '\n' && g_lookahead != TOK_EOF) {
        match_token(g_lookahead);
    }

    if (g_lookahead == '!') {
        match_token('!');
        is_neg = 1;
    }

    if (g_lookahead == TOK_IDENT) {
        match_token(TOK_IDENT);
        if (g_flag_float == 0) {
            int new_pin = lookup_pin_by_name(g_prev_tok_str);
            if (new_pin == 0) {
                snprintf(pin_str, sizeof(pin_str), "%c", ' ');
            } else {
                if (g_flag_pin_only == 0) {
                    snprintf(pin_str, sizeof(pin_str), "%d", new_pin);
                } else {
                    snprintf(pin_str, sizeof(pin_str), "%c", ' ');
                }
                if (g_flag_pin_only != 0 && is_pin) {
                    snprintf(pin_str, sizeof(pin_str), "%d", new_pin);
                }
                if (g_flag_pin_only != 0 && !is_pin) {
                    snprintf(pin_str, sizeof(pin_str), "%c", ' ');
                }
                if (new_pin != old_pin && old_pin != 0) {
                    warn_msg(10, g_prev_tok_str);
                }
            }

            if (no_old_pin) {
                spaces_before -= (int)strlen(pin_str) + 1;
                spaces_after = 1;
            }
            if (spaces_before < 1) {
                spaces_before = 1;
            }
            for (int i = 0; i < spaces_before; i++) {
                append_str(g_line_buf, sizeof(g_line_buf), " ");
            }
            append_str(g_line_buf, sizeof(g_line_buf), pin_str);
            for (int i = 0; i < spaces_after; i++) {
                append_str(g_line_buf, sizeof(g_line_buf), " ");
            }
            append_str(g_line_buf, sizeof(g_line_buf), "= ");
            if (is_neg) {
                append_str(g_line_buf, sizeof(g_line_buf), "!");
            }
            append_str(g_line_buf, sizeof(g_line_buf), g_prev_tok_str);
        } else {
            for (int i = 0; i < spaces_before + spaces_after; i++) {
                append_str(g_line_buf, sizeof(g_line_buf), " ");
            }
            append_str(g_line_buf, sizeof(g_line_buf), " = ");
            if (is_neg) {
                append_str(g_line_buf, sizeof(g_line_buf), "!");
            }
            append_str(g_line_buf, sizeof(g_line_buf), g_prev_tok_str);
        }
    } else if (is_neg && (g_lookahead == ' ' || g_lookahead == '\t' || g_lookahead == '[')) {
        /* blanks between ! and [ are kept */
        snprintf(g_bracket_rhs_buf, sizeof(g_bracket_rhs_buf), "%s", " = !");
        while (g_lookahead == ' ' || g_lookahead == '\t') {
            match_token(g_lookahead);
            append_str(g_bracket_rhs_buf, sizeof(g_bracket_rhs_buf), g_prev_tok_str);
        }
        rewrite_pin_list();
    } else if (g_lookahead == '[') {
        snprintf(g_bracket_rhs_buf, sizeof(g_bracket_rhs_buf), "%s", " = ");
        rewrite_pin_list();
    } else {
        fatal_error(6, g_cur_tok_str, g_line_num);
    }

    while (g_lookahead != TOK_SEMI && g_lookahead != '\n' && g_lookahead != TOK_EOF) {
        match_token(g_lookahead);
        append_str(g_line_buf, sizeof(g_line_buf), g_prev_tok_str);
    }

    if (g_lookahead == TOK_SEMI) {
        match_token(TOK_SEMI);
        append_str(g_line_buf, sizeof(g_line_buf), g_prev_tok_str);
    } else {
        fatal_error(8, "", g_line_num);
    }

    if (g_lookahead == '\n') {
        match_token('\n');
        append_str(g_line_buf, sizeof(g_line_buf), "\n");
    }

    emit_str(g_line_buf);
}

/* copy a NODE statement of the .pld unchanged up to and including its
 * semicolon; NODE statements carry no pin numbers */
static void copy_node_stmt(void) {
    while (g_lookahead != TOK_SEMI && g_lookahead != TOK_EOF) {
        if (g_lookahead == TOK_CMT_BEG) {
            while (g_lookahead != TOK_CMT_END && g_lookahead != TOK_EOF) {
                match_token(g_lookahead);
                emit_str(g_prev_tok_str);
            }
        }
        if (g_lookahead == TOK_EOF) {
            break;
        }
        match_token(g_lookahead);
        emit_str(g_prev_tok_str);
    }
    if (g_lookahead == TOK_SEMI) {
        match_token(TOK_SEMI);
        emit_str(g_prev_tok_str);
    }
}

/* process input file (.plc/.ppn or .bak -> .pld) */
static void process_file(void) {
    g_line_num = 1;
    g_lookahead = lex_token();
    while (g_lookahead != TOK_EOF) {
        if (g_read_mode == 0 && g_lookahead == TOK_NODE) {
            copy_node_stmt();
        } else if (g_lookahead == TOK_PIN || g_lookahead == TOK_NODE || g_lookahead == TOK_PINNODE) {
            if (g_read_mode == 0) {
                rewrite_pld_pin_stmt();
            } else {
                parse_plc_pin_or_node();
            }
        } else if (g_lookahead == TOK_SEMI) {
            match_token(TOK_SEMI);
            emit_str(g_prev_tok_str);
            while (g_lookahead != '\n' && g_lookahead != TOK_EOF) {
                match_token(g_lookahead);
                emit_str(g_prev_tok_str);
            }
            if (g_lookahead == '\n') {
                match_token('\n');
                emit_str(g_prev_tok_str);
            }
        } else if (g_lookahead == TOK_CMT_BEG) {
            match_token(TOK_CMT_BEG);
            emit_str(g_prev_tok_str);
            while (g_lookahead != TOK_CMT_END && g_lookahead != TOK_EOF) {
                match_token(g_lookahead);
                emit_str(g_prev_tok_str);
            }
            match_token(TOK_CMT_END);
            emit_str(g_prev_tok_str);
        } else if (g_read_mode != 0 && g_fitter_type == 4 && g_lookahead == TOK_IDENT) {
            parse_ppn_line();
        } else {
            match_token(g_lookahead);
            emit_str(g_prev_tok_str);
        }
    }
}

int main(int argc, char **argv) {
    g_had_error = 0;
    if (parse_args(argv, argc) == 0) {
        print_usage();
        exit(0);
    }

    if (g_flag_log == 1) {
        open_log_file();
    }

    out_printf("%s %s backpin: BACKannotation of PIN placements\n%s\n\n",
               PROGRAM_NAME, PROGRAM_VERSION, PROGRAM_COPYRIGHT);

    char *dir = extract_dir(g_source);
    create_backup(dir, g_source);
    if (dir[0] != '\0') free(dir);

    if (g_flag_float == 0) {
        resolve_lib_file();
        g_dev_pin_cnt = read_device_library(g_device, g_lib_arg);
        if (strncmp(g_device, "mach", 4) == 0) {
            g_is_mach = 1;
        }
        char *plc_dir = extract_dir(g_source);
        if (g_fitter_type == 4) {
            g_plc_path = resolve_existing_filepath(plc_dir, g_source, ".ppn");
        } else {
            g_plc_path = resolve_existing_filepath(plc_dir, g_source, ".plc");
        }
        if (plc_dir[0] != '\0') free(plc_dir);

        g_fp_plc = lincupl_fopen_ci(g_plc_path, "r");
        if (g_fp_plc == NULL) {
            fatal_error(1, g_plc_path, 0);
        }
        alloc_pin_placements();
        g_fp_in = g_fp_plc;
        g_read_mode = 1;
        out_printf("\nReading pin placement file: %s\n", g_plc_path);
        process_file();
        fclose(g_fp_plc);
        g_fp_plc = NULL;

        if (g_flag_pin_only == 1) {
            for (int i = g_dev_pin_cnt; i < 1000; i++) {
                if (g_pin_names[i] != NULL) {
                    free(g_pin_names[i]);
                    g_pin_names[i] = NULL;
                }
            }
        }
    } else {
        alloc_pin_placements();
    }

    char *pld_dir = extract_dir(g_source);
    g_pld_path = resolve_existing_filepath(pld_dir, g_source, ".pld");
    if (pld_dir[0] != '\0') free(pld_dir);

    /* the .pld is only written once the new text is complete */
    if (access_check(g_pld_path, W_OK) != 0) {
        fatal_error(1, g_pld_path, 0);
    }

    char *bak_dir = extract_dir(g_source);
    g_bak_path = make_backup_filepath(bak_dir, g_source);
    if (bak_dir[0] != '\0') free(bak_dir);

    g_fp_bak = lincupl_fopen_ci(g_bak_path, "r");
    if (g_fp_bak == NULL) {
        fatal_error(1, g_bak_path, 0);
    }

    g_fp_in = g_fp_bak;
    g_collect_out = 1;
    g_read_mode = 0;
    out_printf("\nPlacing pin numbers in file: %s\n\n", g_pld_path);
    process_file();
    fclose(g_fp_bak);

    g_fp_pld = lincupl_fopen_ci(g_pld_path, "w");
    if (g_fp_pld == NULL) {
        fatal_error(1, g_pld_path, 0);
    }
    if (g_out_len > 0) {
        fwrite(g_out_buf, 1, g_out_len, g_fp_pld);
    }
    if (ferror(g_fp_pld) || fclose(g_fp_pld) != 0) {
        fatal_error(3, g_pld_path, 0);
    }
    free(g_out_buf);

    if (g_plc_path != NULL) free(g_plc_path);
    if (g_pld_path != NULL) free(g_pld_path);
    if (g_bak_path != NULL) free(g_bak_path);
    free_pin_placements();

    if (g_flag_log == 1 && g_fp_log != NULL) {
        fclose(g_fp_log);
    }
    return (g_had_error != 0) ? 1 : 0;
}
