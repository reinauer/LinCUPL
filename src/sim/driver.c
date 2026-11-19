/*
 * driver.c - command line driver of the CUPL simulator.
 *
 * csim_parse_options reads the csim command line and finds the device
 * library; csim_driver_run decides from the library entry of the device
 * whether csima or vsima simulates the design and runs that phase with the
 * argument vector it expects.  With -f the design is the fuse map of a
 * JEDEC file, which csima simulates.
 */
#include "banner.h"
#include "locate.h"
#include "strutil.h"
#include "cupl_phases.h"
#include "cfile.h"
#include "devlib.h"

#include <ctype.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

/* Phase table; active 2 ends the table. */
struct sim_phase {
    unsigned char active;       /* 1 = run this phase */
    unsigned char active_init;
    const char *name;
    unsigned char id;
};

static struct sim_phase g_phases[4] = {
    {0, 0, "csim",  7},
    {1, 1, "simulate", 8},
    {0, 0, "simulate", 11},
    {2, 2, NULL,    2}
};

static const char * const g_err_msgs[15] = {
    "program  error:  ",
    "could not open:  ",
    "could not execute program:  ",
    "could not find PATH in ENVIRONMENT",
    "could not find LIBCUPL in ENVIRONMENT",
    "could not find program:  ",
    "insufficient memory to execute program:  ",
    "invalid flag:  ",
    "out of memory:  ",
    "file read error, unexpected end of file: ",
    "invalid library access key",
    "invalid library interface",
    "bad library file:  ",
    "device not in library:  ",
    "target device not specified"
};

/* state of the driver */
static short g_err_count = 0;
static jmp_buf g_fatal_jmp;                  /* driver_fatal jumps back to csim_driver_run */

static char g_pid_str[32];                /* "&" and a six-digit number */
static char g_flags_str[256];             /* "&" and the flag letters for the phase */
static time_t g_start_time = 0;

static char *g_na[9];                     /* argument vector of the phase */
static char g_lib_buf[PATH_MAX];
static char g_dev_arg_buf[128];
static char g_source_buf[PATH_MAX];
static char g_saved_cwd[PATH_MAX];
static char g_abs_lib_buf[PATH_MAX];
static char g_abs_work_buf[PATH_MAX];
static char g_short_src_buf[512];
static volatile int g_did_chdir = 0;

static int fprint_flushed(FILE *fp, const char *fmt, ...) {
    if (!fp) return 0;
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(fp, fmt, ap);
    va_end(ap);
    fflush(fp);
    return r;
}

static int print_flushed(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
    return r;
}

static void driver_error(int code, const char *detail) {
    g_err_count++;
    int msg_idx = (code < 1000 && code >= 0 && code < 15) ? code : 0;
    int disp_code = (code < 1000) ? code : 0;
    fprint_flushed(stdout, "[%04d%c%c] %s%s\n",
                  disp_code, 's', 'k',
                  g_err_msgs[msg_idx],
                  detail ? detail : "");
}

static void driver_fatal(short code, const char *detail) {
    if (code >= 0) {
        driver_error((int)code, detail);
    }
    time_t now = time(NULL);
    fprint_flushed(stdout, "Total time: %ld secs\n", (long)(now - g_start_time));
    longjmp(g_fatal_jmp, 1);
}

/* Build path from dir, base_name (stripped of ext/dir), and ext */
static char *make_path(const char *dir_part, const char *name_part, const char *ext_part) {
    size_t nlen = strlen(name_part);
    char *tmp = (char *)malloc(nlen + 1);
    if (!tmp) {
        driver_fatal(8, "initialization");
    }
    char *dst = tmp;
    for (const char *p = name_part; *p != '.' && *p != '\0'; p++) {
        *dst++ = *p;
    }
    *dst = '\0';

    const char *sep = lincupl_path_sep(tmp);
    const char *base = sep != NULL ? sep + 1 : tmp;

    size_t out_sz = strlen(dir_part) + strlen(base) + strlen(ext_part) + 4;
    char *out = (char *)malloc(out_sz);
    if (!out) {
        free(tmp);
        driver_fatal(8, "initialization");
    }
    out[0] = '\0';
    if (strlen(dir_part) != 0) {
        strcpy(out, dir_part);
        const char *last_bs = strrchr(dir_part, '\\');
        const char *last_fs = strrchr(dir_part, '/');
        const char *last_sep = last_bs;
        if (last_fs && (!last_sep || last_fs > last_sep)) last_sep = last_fs;
        if (!last_sep || *(last_sep + 1) != '\0') {
            strcat(out, "/");
        }
    }
    strcat(out, base);
    strcat(out, ext_part);
    free(tmp);
    return out;
}

/* Extract directory prefix (including trailing slash) or "" */
static char *get_dir(const char *path) {
    size_t len = lincupl_path_dir_len(path);
    if (len == 0) {
        return "";
    }
    char *out = (char *)malloc(len + 1);
    if (!out) {
        driver_fatal(8, "initialization");
    }
    memcpy(out, path, len);
    out[len] = '\0';
    return out;
}

/*
 * Look up `dev_name` in the .dl library and copy the 152-byte header record
 * of its entry (section_data + *(int32_t *)(section_data + 0x44)) into
 * `out_buf`.  Returns 4 on success; otherwise 5 (cannot open), 7 (wrong
 * access key), 9 (device "unknown"), 10 (not in the library) or 12 (bad
 * library file).
 */
static int lookup_device_in_lib(uint8_t *out_buf, const char *dev_name, const char *lib_path) {
    FILE *fp = lincupl_fopen_ci(lib_path, "rb");
    if (!fp) {
        return 5;
    }

    uint8_t hdr[DL_HEADER_SIZE];
    switch (dl_read_header(fp, hdr)) {
    case DL_OK:
        break;
    case DL_BAD_KEY:
        fclose(fp);
        return 7;
    default:
        fclose(fp);
        return 12;
    }

    if (strcmp("unknown", dev_name) == 0) {
        fclose(fp);
        return 9;
    }

    uint8_t entry[DL_ENTRY_SIZE];
    if (!dl_find_device(fp, dev_name, DL_IGNORE_CASE, entry)) {
        fclose(fp);
        return 10;
    }

    uint32_t sec_size = dl_image_size(entry);
    if (sec_size < 0x48u || !dl_seek_image(fp, entry)) {
        fclose(fp);
        return 12;
    }

    uint8_t *sec_data = (uint8_t *)malloc(sec_size);
    if (!sec_data) {
        fclose(fp);
        driver_fatal(8, "device library processing");
    }
    size_t got = dl_read_image(fp, sec_data, sec_size);
    fclose(fp);
    if (got != sec_size) {
        free(sec_data);
        return 12;
    }

    uint32_t sub_off = rd32(sec_data + DL_INFO_OFFSET);
    if (sub_off > sec_size || sec_size - sub_off < 0x26u * 4u) {
        free(sec_data);
        return 12;
    }
    memcpy(out_buf, sec_data + sub_off, 0x26u * 4u);
    free(sec_data);
    return 4;
}

/* Read the header of the .abs file, take the device name from it and look
 * it up in the library.  Returns 0 if the device is simulated by
 * vsima (header byte +0x11 is 9), 1 for csima, 2 / 3 if the .abs file
 * cannot be opened / read, else the error code of the library lookup. */
static int check_abs(const char *abs_path, const char *lib_path) {
    FILE *fp = lincupl_fopen_ci(abs_path, "rb");
    if (!fp) {
        return 2;
    }
    uint8_t abs_hdr[CF_HEADER_SIZE];
    if (!cf_read_header(fp, abs_hdr)) {
        fclose(fp);
        return 3;
    }
    abs_hdr[CF_HEADER_SIZE - 1] = '\0';
    const char *dev_in_abs = (const char *)(abs_hdr + CF_HDR_DEVICE);
    uint8_t dev_info[0x26 * 4];
    memset(dev_info, 0, sizeof(dev_info));
    int rc = lookup_device_in_lib(dev_info, dev_in_abs, lib_path);
    fclose(fp);
    if (rc == 4) {
        if (dev_info[0x11] == '\t') {
            return 0;
        }
        return 1;
    }
    return rc;
}

/* The device library must be readable */
static void verify_lib(void) {
    FILE *fp = lincupl_fopen_ci(g_na[2], "rb");
    if (!fp) {
        driver_fatal(1, g_na[2]);
    }
    fclose(fp);
}

/* Set up the argument vector of the phase and determine whether csima or
 * vsima is needed */
static void init_args(const struct csim_options *opt) {
    memset(g_flags_str, 0, sizeof(g_flags_str));
    snprintf(g_flags_str, sizeof(g_flags_str), "%s", opt->flags);
    snprintf(g_pid_str, sizeof(g_pid_str), "&%06u", 0u);
    snprintf(g_lib_buf, sizeof(g_lib_buf), "%s", opt->library);
    snprintf(g_dev_arg_buf, sizeof(g_dev_arg_buf), "%s", opt->device[0] != '\0' ? opt->device : "&");
    snprintf(g_source_buf, sizeof(g_source_buf), "%s", opt->source);
    g_na[2] = g_lib_buf;
    g_na[3] = g_pid_str;
    g_na[4] = g_dev_arg_buf;
    g_na[5] = g_source_buf;
    g_na[6] = g_flags_str;
    verify_lib();
    if (strchr(g_flags_str + 1, 'f') != NULL) {
        /* -f: the design is the fuse map of <name>.jed.  There is no .abs
         * file to take the device from; the simulator (csima) finds the
         * device from the fuse map or takes the one given here. */
        return;
    }
    /* Check if device is "&" (read from .abs) or specified explicitly */
    if (strcmp(g_na[4], "&") == 0) {
        char base_name[256];
        lincupl_split_path(g_na[5], NULL, 0, base_name, sizeof(base_name), NULL, 0);
        char *dir_prefix = get_dir(g_na[5]);
        char *abs_path = make_path(dir_prefix, base_name, ".abs");
        int rc = check_abs(abs_path, g_na[2]);
        if (rc == 2 && dir_prefix[0] != '\0') {
            char *cwd_abs = make_path("", base_name, ".abs");
            int rc2 = check_abs(cwd_abs, g_na[2]);
            if (rc2 != 2) {
                free(abs_path);
                abs_path = cwd_abs;
                rc = rc2;
            } else {
                free(cwd_abs);
            }
        }
        if (dir_prefix[0] != '\0') {
            free(dir_prefix);
        }
        switch (rc) {
            case 1:
                break;
            case 2:
                driver_fatal(1, abs_path);
                break;
            case 3:
            case 6:
                driver_fatal(9, abs_path);
                break;
            case 5:
                driver_fatal(1, g_na[2]);
                break;
            case 7:
                driver_fatal(10, NULL);
                break;
            case 8:
                driver_fatal(11, NULL);
                break;
            case 10:
                driver_fatal(13, g_na[4]);
                break;
            case 12:
                driver_fatal(12, g_na[2]);
                break;
            case 9:
                driver_fatal(14, NULL);
                break;
            case 0:
                g_phases[1].active = 0;
                g_phases[2].active = 1;
                break;
            default:
                driver_fatal(0, "undefined error");
                break;
        }
        free(abs_path);
    } else {
        uint8_t dev_info[0x26 * 4];
        memset(dev_info, 0, sizeof(dev_info));
        int rc = lookup_device_in_lib(dev_info, g_na[4], g_na[2]);
        if (rc == 4) {
            if (dev_info[0x11] == '\t') {
                g_phases[1].active = 0;
                g_phases[2].active = 1;
            }
        } else {
            switch (rc) {
                case 3:
                    driver_fatal(9, g_na[2]);
                    break;
                case 2:
                case 5:
                    driver_fatal(1, g_na[2]);
                    break;
                case 7:
                    driver_fatal(10, NULL);
                    break;
                case 8:
                    driver_fatal(11, NULL);
                    break;
                case 10:
                    driver_fatal(13, g_na[4]);
                    break;
                case 12:
                    driver_fatal(12, g_na[2]);
                    break;
                case 9:
                    driver_fatal(14, NULL);
                    break;
                default:
                    break;
            }
        }
    }
}

/* Run the simulator, in the mode for virtual devices for phase 11; returns 1
 * for any other phase */
static int invoke_phase(struct sim_phase *phase, int *phase_rc_out) {
    if (phase->id == 8) {
        *phase_rc_out = sim_run(0, 8, g_na);
        return 0;
    } else if (phase->id == 11) {
        *phase_rc_out = sim_run(1, 8, g_na);
        return 0;
    }
    return 1;
}

/* Run active simulator phases */
static int run_phases(void) {
    int status = 0;

    for (int i = 0; g_phases[i].active != 2; i++) {
        if (g_phases[i].active == 1) {
            g_na[0] = (char *)g_phases[i].name;
            print_flushed("%s\n", g_phases[i].name);

            time_t phase_start = time(NULL);
            int phase_rc = 0;
            status = invoke_phase(&g_phases[i], &phase_rc);
            time_t phase_end = time(NULL);
            print_flushed("time: %d secs\n", (int)(phase_end - phase_start));

            if (status == 0) {
                if (phase_rc == 1) {
                    driver_fatal(-1, NULL);
                }
            } else if (status == 1) {
                driver_fatal(2, g_phases[i].name);
            }
        }
    }
    return status;
}

/* -------------------------------------------------------------------------
 * Command line
 * ------------------------------------------------------------------------- */

int csim_parse_options(int argc, char **argv, struct csim_options *opt)
{
    char letters[64];               /* flag letters as given, at most 63 */
    size_t nletters = 0;
    int pos_args[4];
    int pos_count = 0;
    int has_u = 0;
    char lib[PATH_MAX];
    int have_lib;

    memset(opt, 0, sizeof(*opt));
    lib[0] = '\0';
    letters[0] = '\0';
    if (argc < 2) {
        return CSIM_OPTIONS_USAGE;
    }

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            if (pos_count < 4) {
                pos_args[pos_count++] = i;
            }
            continue;
        }
        /* the first 255 characters of the argument; unknown letters are
         * reported and skipped */
        char arg[256];
        strncpy(arg, argv[i] + 1, sizeof(arg) - 1);
        arg[sizeof(arg) - 1] = '\0';
        for (char *s = arg; *s != '\0'; s++) {
            if (isupper((unsigned char)*s)) {
                *s = (char)tolower((unsigned char)*s);
            }
            int c = (unsigned char)*s;
            if (c == '-') {
                continue;
            } else if (c == 'j' || c == 'l' || c == 'n' || c == 'w' || c == 'd' || c == 'v' || c == 'f') {
                if (nletters + 1 < sizeof(letters)) {
                    letters[nletters++] = *s;
                    letters[nletters] = '\0';
                }
            } else if (c == 'u') {
                has_u = 1;
            } else {
                printf("Unknown flag -%c\n", (char)c);
            }
        }
    }

    /* operands: [library (with -u)] [device] file */
    if (!has_u) {
        if (pos_count == 1) {
            snprintf(opt->source, sizeof(opt->source), "%s", argv[pos_args[0]]);
        } else if (pos_count == 2) {
            snprintf(opt->device, sizeof(opt->device), "%s", argv[pos_args[0]]);
            snprintf(opt->source, sizeof(opt->source), "%s", argv[pos_args[1]]);
        } else if (pos_count > 2) {
            return CSIM_OPTIONS_USAGE;
        }
    } else if (pos_count > 1) {
        snprintf(lib, sizeof(lib), "%s", argv[pos_args[0]]);
        if (pos_count == 2) {
            snprintf(opt->source, sizeof(opt->source), "%s", argv[pos_args[1]]);
        } else if (pos_count == 3) {
            snprintf(opt->device, sizeof(opt->device), "%s", argv[pos_args[1]]);
            snprintf(opt->source, sizeof(opt->source), "%s", argv[pos_args[2]]);
        } else {
            return CSIM_OPTIONS_USAGE;
        }
    }
    lincupl_strlwr(opt->device);

    /* f, j, l, n, v and w are passed on once each, in the order first given,
     * and v in any case: the simulator always runs in console mode.  -d
     * (show the .so file in the waveform viewer) is not passed on. */
    opt->flags[0] = '&';
    for (size_t i = 0; i <= nletters; i++) {
        char c = i < nletters ? letters[i] : 'v';
        if (c != 'd' && strchr(opt->flags + 1, c) == NULL) {
            size_t len = strlen(opt->flags);
            opt->flags[len] = c;
            opt->flags[len + 1] = '\0';
        }
    }

    if (has_u) {
        have_lib = lincupl_resolve_explicit_dl(lib, opt->library, sizeof(opt->library));
    } else {
        have_lib = lincupl_find_default_dl(opt->library, sizeof(opt->library));
    }
    if (!have_lib) {
        return CSIM_OPTIONS_NO_LIBRARY;
    }
    if (opt->source[0] == '\0') {
        return CSIM_OPTIONS_NO_SOURCE;
    }
    return CSIM_OPTIONS_OK;
}

/* Main driver routine: banner, arguments, phases, total time */
static int driver_main(const struct csim_options *opt) {
    print_flushed("%s %s csim: CUPL Simulation Program\n", PROGRAM_NAME, PROGRAM_VERSION);
    print_flushed("%s\n\n", PROGRAM_COPYRIGHT);

    g_start_time = time(NULL);

    for (int i = 0; g_phases[i].active_init != 2; i++) {
        g_phases[i].active = g_phases[i].active_init;
    }

    init_args(opt);

    {
        const char *src_p = g_na[5];
        const char *lib_p = g_na[2];
        const char *work_p = g_na[1];
        if (lincupl_enter_long_source_dir(
                &src_p,
                &lib_p,
                (work_p && strcmp(work_p, "&") != 0) ? &work_p : NULL,
                g_saved_cwd, sizeof(g_saved_cwd),
                g_abs_lib_buf, sizeof(g_abs_lib_buf),
                g_abs_work_buf, sizeof(g_abs_work_buf),
                g_short_src_buf, sizeof(g_short_src_buf))) {
            g_na[5] = (char *)src_p;
            g_na[2] = (char *)lib_p;
            g_na[1] = (char *)work_p;
            g_did_chdir = 1;
        }
    }

    int rc = run_phases();
    time_t now = time(NULL);
    print_flushed("total time: %d secs\n", (int)(now - g_start_time));
    return rc;
}

/* Entry point.  Returns 0 on success. */
int csim_driver_run(const struct csim_options *opt) {
    g_err_count = 0;
    g_did_chdir = 0;

    static char amp_strs[8][4];
    for (int i = 0; i < 8; i++) {
        strcpy(amp_strs[i], "&");
        g_na[i] = amp_strs[i];
    }
    g_na[8] = NULL;

    memset(g_pid_str, 0, sizeof(g_pid_str));
    memset(g_flags_str, 0, sizeof(g_flags_str));

    if (setjmp(g_fatal_jmp) != 0) {
        if (g_did_chdir) {
            if (chdir(g_saved_cwd) != 0) {
                /* best-effort restore */
            }
            g_did_chdir = 0;
        }
        return 1;
    }

    int rc = driver_main(opt);
    if (g_did_chdir) {
        if (chdir(g_saved_cwd) != 0) {
            /* best-effort restore */
        }
        g_did_chdir = 0;
    }
    return rc;
}
