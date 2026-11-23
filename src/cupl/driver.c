/*
 * driver.c - compiler kernel
 *
 * cupl_parse_options reads the cupl command line; cupl_driver_run builds
 * the argument vector shared by all phases from it, runs the enabled phases
 * in order (preprocess, parse, expand, minimize, fusemap, the external
 * fitter, simulate) and removes their temporary files.
 */

#include "platform.h"
#include "banner.h"
#include "locate.h"
#include "runfit.h"
#include "strutil.h"
#include "cupl_phases.h"
#include "cfile.h"
#include "devlib.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Phase table entry: active (0 off, 1 on, 2 end of table), initial value of
 * active, phase name, extra flags passed to the phase, phase id */
typedef struct {
    char active;
    char init;
    const char *name;
    const char *arg;
    char id;
} cuplk_phase_t;

/* Phases in the order they run; the fitter and the simulator are switched
 * on by the options */
static cuplk_phase_t g_phases[9] = {
    { 0, 0, "cupl",   "",   1 }, /* [0] */
    { 1, 1, "preprocess", "",   2 }, /* [1] */
    { 1, 1, "parse",      "",   3 }, /* [2] */
    { 1, 1, "expand",     "",   4 }, /* [3] */
    { 1, 1, "minimize",   "",   6 }, /* [4] */
    { 1, 1, "fusemap",    "",   5 }, /* [5] */
    { 0, 0, "fitter",     "",   9 }, /* [6] */
    { 0, 0, "simulate",   "zl", 8 }, /* [7] */
    { 2, 2, NULL,     "",   0 }  /* [8] */
};

/* Kernel state; cupl_driver_run sets it up before each run */
static char g_flags_buf[64];              /* flags for the phases, "&..." (zero padded) */
static char g_pid_buf[16];                /* "&" + 6-digit id used in temp file names */
static int g_sim_virtual = 0;             /* simulate a virtual device (vsima mode) */
static char g_lib_buf[PATH_MAX];
static char g_dev_buf[128];
static char g_source_buf[PATH_MAX];
static char g_err_prefix_printed = 0;     /* blank line before the first message printed */

/* na[0..8]: argument vector passed to every phase */
static const char *g_na[9];
static const int g_na_argc = 8;

/* printf to stdout, flushed at once */
static int print_flushed(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int rc = vfprintf(stdout, fmt, ap);
    va_end(ap);
    fflush(stdout);
    return rc;
}

/* -------------------------------------------------------------------------
 * Command line
 * ------------------------------------------------------------------------- */

/* Is the single flag c (not the second letter of a k, m or z pair) in s? */
static int has_single_flag(const char *s, char c)
{
    for (size_t i = 0; s[i] != '\0'; i++) {
        if (s[i] == 'k' || s[i] == 'm' || s[i] == 'z') {
            if (s[i + 1] != '\0') i++;
            continue;
        }
        if (s[i] == c) return 1;
    }
    return 0;
}

/* Is the pair c1 c2 (k, m or z and its letter) in s? */
static int has_flag_pair(const char *s, char c1, char c2)
{
    for (size_t i = 0; s[i] != '\0'; i++) {
        if (s[i] == 'k' || s[i] == 'm' || s[i] == 'z') {
            if (s[i] == c1 && s[i + 1] == c2) return 1;
            if (s[i + 1] != '\0') i++;
        }
    }
    return 0;
}

/* Append a flag (c2 == 0) or a flag pair to opt->flags unless it is there
 * already; each flag is passed on once, in the order first given. */
static void add_flag(struct cupl_options *opt, char c1, char c2)
{
    size_t len = strlen(opt->flags);
    if (c2 == 0) {
        if (!has_single_flag(opt->flags + 1, c1) && len + 1 < sizeof(opt->flags)) {
            opt->flags[len] = c1;
            opt->flags[len + 1] = '\0';
        }
    } else if (!has_flag_pair(opt->flags + 1, c1, c2) && len + 2 < sizeof(opt->flags)) {
        opt->flags[len] = c1;
        opt->flags[len + 1] = c2;
        opt->flags[len + 2] = '\0';
    }
}

/* The letters of one '-' argument (any case, '-' is skipped).  Returns 0
 * for an unknown flag. */
static int parse_flag_arg(struct cupl_options *opt, const char *p, int *has_u)
{
    for (; *p != '\0'; p++) {
        int c = tolower((unsigned char)*p);
        int next;
        switch (c) {
        case '-':
            break;
        case 'a': case 'b': case 'c': case 'd': case 'e': case 'f': case 'g':
        case 'h': case 'i': case 'l': case 'o': case 'p': case 'q': case 'r':
        case 't': case 'x':
            add_flag(opt, (char)c, 0);
            break;
        case 'j':
            add_flag(opt, 'j', 0);
            opt->flag_j = 1;
            opt->run_fitter = 1;
            break;
        case 'n':
            add_flag(opt, 'n', 0);
            opt->flag_n = 1;
            break;
        case 's':
            add_flag(opt, 's', 0);
            opt->run_simulator = 1;
            break;
        case 'w':
            /* -w passes 's' on, so that fusemap writes the .abs file the
             * simulator needs */
            add_flag(opt, 's', 0);
            opt->run_simulator = 1;
            opt->flag_w = 1;
            break;
        case 'u':
            *has_u = 1;
            break;
        case 'k':
            /* only -kb, -kd, -ks and -kx exist */
            next = tolower((unsigned char)p[1]);
            if (next != 'b' && next != 'd' && next != 's' && next != 'x') {
                return 0;
            }
            add_flag(opt, 'k', (char)next);
            p++;
            break;
        case 'm':
            /* -m alone is -m1 */
            next = (unsigned char)p[1];
            if (next >= '0' && next <= '4') {
                add_flag(opt, 'm', (char)next);
                p++;
            } else if (isdigit(next)) {
                return 0;
            } else {
                add_flag(opt, 'm', '1');
            }
            break;
        case 'y':
            /* -y1 .. -y8 are accepted and have no effect */
            p++;
            if (*p < '1' || *p > '8') {
                return 0;
            }
            break;
        case 'z':
            next = tolower((unsigned char)p[1]);
            if (next != 'q' && next != 'x') {
                return 0;
            }
            add_flag(opt, 'z', (char)next);
            opt->run_fitter = 1;
            p++;
            break;
        default:
            return 0;
        }
    }
    return 1;
}

int cupl_parse_options(int argc, char **argv, struct cupl_options *opt)
{
    int pos_args[4];
    int pos_count = 0;
    int has_u = 0;
    int have_lib;

    memset(opt, 0, sizeof(*opt));
    opt->flags[0] = '&';
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (!parse_flag_arg(opt, argv[i], &has_u)) {
                return CUPL_OPTIONS_USAGE;
            }
        } else if (pos_count < 4) {
            pos_args[pos_count++] = i;
        }
    }
    /* every phase runs in console mode */
    add_flag(opt, 'v', 0);

    /* operands: [library (with -u)] [device] file */
    int first = has_u ? 1 : 0;
    if (pos_count < first + 1) {
        return CUPL_OPTIONS_USAGE;
    }
    if (pos_count < first + 2) {
        snprintf(opt->source, sizeof(opt->source), "%s", argv[pos_args[first]]);
    } else {
        snprintf(opt->device, sizeof(opt->device), "%s", argv[pos_args[first]]);
        lincupl_strlwr(opt->device);
        snprintf(opt->source, sizeof(opt->source), "%s", argv[pos_args[first + 1]]);
    }
    if (has_u) {
        char raw_lib[PATH_MAX];
        snprintf(raw_lib, sizeof(raw_lib), "%s", argv[pos_args[0]]);
        have_lib = lincupl_resolve_explicit_dl(raw_lib, opt->library, sizeof(opt->library));
    } else {
        have_lib = lincupl_find_default_dl(opt->library, sizeof(opt->library));
    }
    return have_lib ? CUPL_OPTIONS_OK : CUPL_OPTIONS_NO_LIBRARY;
}

/* -------------------------------------------------------------------------
 * Simulator selection
 * ------------------------------------------------------------------------- */

/* Inspect .dl library to check whether device uses vsima (dev_info[0x11] == 9) or csima */
static int cuplk_device_uses_vsima(const char *dev_name, const char *lib_path)
{
    FILE *fp;
    uint8_t hdr[DL_HEADER_SIZE];
    uint8_t entry[DL_ENTRY_SIZE];
    uint8_t *sec_data = NULL;
    uint32_t sec_size = 0;

    if (!dev_name || !dev_name[0] || !lib_path || !lib_path[0]) return 0;
    fp = lincupl_fopen_ci(lib_path, "rb");
    if (!fp) return 0;
    if (dl_read_header(fp, hdr) == DL_OK && dl_find_device(fp, dev_name, DL_IGNORE_CASE, entry)) {
        sec_size = dl_image_size(entry);
        if (sec_size >= 0x48u && dl_seek_image(fp, entry)) {
            sec_data = (uint8_t *)malloc(sec_size);
        }
        if (sec_data && dl_read_device_image(fp, entry, sec_data, sec_size) != sec_size) {
            free(sec_data);
            sec_data = NULL;
        }
    }
    fclose(fp);
    if (!sec_data) return 0;
    uint32_t sub_off = rd32(sec_data + DL_INFO_OFFSET);
    int is_vsima = 0;
    if (sub_off + 0x12u <= sec_size && sec_data[sub_off + 0x11u] == '\t') {
        is_vsima = 1;
    }
    free(sec_data);
    return is_vsima;
}

/* select the simulator mode for virtual devices or the normal one before
 * running the simulator phase: from the device
 * on the command line, else from the device recorded in the design's .abs */
static void cuplk_select_simulator(void)
{
    int use_vsima = 0;
    if (strcmp(g_na[4], "&") != 0) {
        use_vsima = cuplk_device_uses_vsima(g_na[4], g_na[2]);
    } else if (g_na[5] && strcmp(g_na[5], "&") != 0) {
        char norm[PATH_MAX];
        lincupl_normalize_slashes(g_na[5], norm, sizeof(norm));
        const char *slash = strrchr(norm, '/');
        const char *fname = slash ? (slash + 1) : norm;
        const char *dot = strrchr(fname, '.');
        size_t blen = dot ? (size_t)(dot - fname) : strlen(fname);
        char stem[256];
        if (blen >= sizeof(stem)) blen = sizeof(stem) - 1;
        memcpy(stem, fname, blen);
        stem[blen] = '\0';

        char abs_cand[PATH_MAX];
        FILE *fp = NULL;
        if (slash) {
            size_t dlen = (size_t)(slash - norm);
            snprintf(abs_cand, sizeof(abs_cand), "%.*s/%s.abs", (int)dlen, norm, stem);
            fp = lincupl_fopen_ci(abs_cand, "rb");
        }
        if (!fp) {
            snprintf(abs_cand, sizeof(abs_cand), "%s.abs", stem);
            fp = lincupl_fopen_ci(abs_cand, "rb");
        }
        if (fp) {
            uint8_t abs_hdr[CF_HEADER_SIZE];
            if (cf_read_header(fp, abs_hdr)) {
                abs_hdr[CF_HEADER_SIZE - 1] = '\0';
                const char *dev_in_abs = (const char *)(abs_hdr + CF_HDR_DEVICE);
                use_vsima = cuplk_device_uses_vsima(dev_in_abs, g_na[2]);
            }
            fclose(fp);
        }
    }
    g_sim_virtual = use_vsima;
}

/* -------------------------------------------------------------------------
 * Fitter phase
 *
 * For devices that need an external fitter (ATF150x and other CPLDs), fusemap
 * leaves a two-line command file "runfit.$$$":
 *
 *     <pla format> <device> <fitter program> <entry point or ~>
 *     <arguments for the fitter>
 *
 * e.g. "14 f1500a fit1500 ~" / " MUX.tt2 -CUPL -dev f1500a".  The kernel runs
 * that program in the design's directory; the fitter reads the .tt2 file and
 * writes the .jed, .fit and .plc files.
 * ------------------------------------------------------------------------- */

/* print a fitter error as "[<code>ck] <msg><detail>" */
static void cuplk_fitter_error(int code, const char *msg, const char *detail)
{
    if (!g_err_prefix_printed) {
        print_flushed("\n\r");
        g_err_prefix_printed = 1;
    }
    print_flushed("[%04d%c%c] %s%s\n\r", code, 'c', 'k', msg, detail);
}

/* read one line without its newline; -1 at end of file */
static int cuplk_read_line(FILE *fp, char *buf, int size)
{
    int c = 0;
    int n = 0;
    while (c != '\n' && c != EOF && n < size - 1) {
        c = fgetc(fp);
        if (c != EOF && c != '\n' && c != '\r') {
            buf[n++] = (char)c;
        }
    }
    buf[n] = '\0';
    return c == EOF ? -1 : n;
}

#define CUPLK_FITTER_MAX_ARGS 64

/* run the fitter named in runfit.$$$ (see above) and wait for it; sets
 * *phase_ret to 1 if the program cannot be found or started */
static int cuplk_run_fitter(int *phase_ret)
{
    char head[256];
    char args[512];
    char device[64];
    char fitter[128];
    char entry[128];
    int pla_format = 0;

    *phase_ret = 0;
    FILE *fp = lincupl_fopen_ci("runfit.$$$", "rt");
    if (fp == NULL) {
        return 0;
    }
    cuplk_read_line(fp, head, sizeof(head));
    cuplk_read_line(fp, args, sizeof(args));
    fclose(fp);
    unlink("runfit.$$$");

    device[0] = fitter[0] = entry[0] = '\0';
    sscanf(head, "%d %63s %127s %127s", &pla_format, device, fitter, entry);
    if (pla_format < 2 || pla_format > 15 || fitter[0] == '\0' || strcmp(fitter, ".") == 0) {
        return 0;
    }

    /* The library names "find1502" etc. for the ATF1502/1504/1508: a small
     * launcher (find150x.exe) whose only job is to locate fit150x.exe on PATH
     * and start it with the same arguments.  That lookup is done here. */
    if (strncmp(fitter, "find", 4) == 0) {
        memmove(fitter + 3, fitter + 4, strlen(fitter + 4) + 1);
        fitter[1] = 'i';
        fitter[2] = 't';
    }

    char program[PATH_MAX];
    char wine_cmd[PATH_MAX];
    char *argv[CUPLK_FITTER_MAX_ARGS + 8];
    int argc = 0;

    int kind = runfit_find_fitter(fitter, g_na[2], program, sizeof(program));
    if (kind == 2) {
        /* a Windows fitter needs Wine: $LINCUPL_WINE (may carry options) or wine on PATH */
        const char *env_wine = getenv("LINCUPL_WINE");
        if (env_wine != NULL && env_wine[0] != '\0') {
            snprintf(wine_cmd, sizeof(wine_cmd), "%s", env_wine);
            for (char *tok = strtok(wine_cmd, " "); tok != NULL && argc < 6; tok = strtok(NULL, " ")) {
                argv[argc++] = tok;
            }
        } else if (runfit_find_in_path("wine", wine_cmd, sizeof(wine_cmd))) {
            argv[argc++] = wine_cmd;
        } else {
            kind = 0;
        }
    }
    if (kind == 0) {
        cuplk_fitter_error(5, "could not find program:  ", fitter);
        *phase_ret = 1;
        return 0;
    }
    argv[argc++] = program;
    for (char *tok = strtok(args, " "); tok != NULL && argc < CUPLK_FITTER_MAX_ARGS; tok = strtok(NULL, " ")) {
        argv[argc++] = tok;
    }
    argv[argc] = NULL;

    print_flushed("%s\n\r", fitter);
    fflush(stdout);
    time_t start = time(NULL);
    /* the Atmel fitters read atmel.std from $FITTERDIR */
    char fitter_dir[PATH_MAX];
    const char *fitter_dir_env = NULL;
    if (kind == 2) {
        snprintf(fitter_dir, sizeof(fitter_dir), "%s", program);
        char *sep = strrchr(fitter_dir, '/');
        if (sep != NULL) {
            *sep = '\0';
            fitter_dir_env = fitter_dir;
        }
    }
    const char *progs[] = { argv[0], NULL };
    int rc = runfit_spawn(progs, argv, fitter_dir_env);
    /* exit status 127 counts as a failure to start, as from a shell that
     * did not find the program */
    if (rc == RUNFIT_NOT_STARTED || rc == 127) {
        cuplk_fitter_error(2, "could not execute program:  ", fitter);
        *phase_ret = 1;
        return 0;
    }
    print_flushed("time: %ld secs\n\r", (long)(time(NULL) - start));
    /* a fitter ends with a non-zero status when the design does not fit or
     * it could not read its input; the reason is in its own messages */
    if (rc == RUNFIT_ABNORMAL) {
        cuplk_fitter_error(11, "Fatal fitter error during processing", "");
        *phase_ret = 1;
    } else if (rc != 0) {
        cuplk_fitter_error(10, "Fitter could not fit design", "");
        *phase_ret = 1;
    }
    return 0;
}

/* run one compiler/simulator phase; returns 1 for an unknown phase id */
static int cuplk_dispatch_phase(
    cuplk_phase_t *phase,
    const char *override_name,
    const char *override_proc,
    const char *arg_str,
    int argc,
    char **argv,
    int unused1,
    int *phase_ret,
    int unused2)
{
    (void)override_proc;
    (void)arg_str;
    (void)unused1;
    (void)unused2;

    switch (phase->id) {
    case 2:
        *phase_ret = preprocess_run(argc, argv);
        return 0;
    case 3:
        *phase_ret = parse_run(argc, argv);
        return 0;
    case 4:
        *phase_ret = expand_run(argc, argv);
        return 0;
    case 5:
        *phase_ret = fusemap_run(argc, argv);
        return 0;
    case 6:
        *phase_ret = minimize_run(argc, argv);
        return 0;
    case 8:
        if (g_sim_virtual) {
            *phase_ret = sim_run(1, argc, argv);
        } else {
            *phase_ret = sim_run(0, argc, argv);
        }
        return 0;
    case 9: /* fitter */
        if (override_name && override_name[0] != '&') {
            *phase_ret = system(override_name);
        } else {
            *phase_ret = 0;
        }
        return 0;
    default:
        return 1;
    }
}

/* cupl_driver_run - compiler kernel entry point.  Returns the sum of the
 * phase results. */
int cupl_driver_run(const struct cupl_options *opt)
{
    char local_arg_buf[1024];
    int phase_ret = 0;
    int dispatch_err = 0;
    int total_status = 0;
    int failed_phase = 0;

    time_t start_time = time(NULL);

    for (int i = 0; g_phases[i].init != 2; i++) {
        g_phases[i].active = g_phases[i].init;
    }
    if (opt->run_fitter) {
        g_phases[6].active = 1;
    }
    if (opt->run_simulator) {
        g_phases[7].active = 1;
    }
    g_sim_virtual = 0;
    g_err_prefix_printed = 0;

    memset(g_flags_buf, 0, sizeof(g_flags_buf));
    snprintf(g_flags_buf, sizeof(g_flags_buf), "%s", opt->flags);
    snprintf(g_pid_buf, sizeof(g_pid_buf), "&%06d", 0);
    snprintf(g_lib_buf, sizeof(g_lib_buf), "%s", opt->library);
    snprintf(g_dev_buf, sizeof(g_dev_buf), "%s", opt->device);
    snprintf(g_source_buf, sizeof(g_source_buf), "%s", opt->source);
    g_na[0] = "cupl";
    g_na[1] = "&";
    g_na[2] = g_lib_buf;
    g_na[3] = g_pid_buf;
    g_na[4] = g_dev_buf[0] != '\0' ? g_dev_buf : "&";
    g_na[5] = g_source_buf;
    g_na[6] = g_flags_buf;
    g_na[7] = "~";
    g_na[8] = NULL;

    print_flushed("\n\r%s %s cupl: Universal Compiler for Programmable Logic\n\r", PROGRAM_NAME, PROGRAM_VERSION);
    print_flushed("%s\n\r\n\r", PROGRAM_COPYRIGHT);

    char saved_cwd[PATH_MAX];
    char abs_lib_buf[PATH_MAX];
    char abs_work_buf[PATH_MAX];
    char short_src_buf[512];
    int did_chdir = lincupl_enter_long_source_dir(
        &g_na[5],
        &g_na[2],
        NULL,
        saved_cwd, sizeof(saved_cwd),
        abs_lib_buf, sizeof(abs_lib_buf),
        abs_work_buf, sizeof(abs_work_buf),
        short_src_buf, sizeof(short_src_buf));

    cuplk_phase_t *phase = &g_phases[0];
    while (phase->active != 2 && dispatch_err == 0 && phase_ret == 0) {
        if (phase->active == 1) {
            if (phase->id == 8) {
                cuplk_select_simulator();
            }
            g_na[0] = phase->name;
            local_arg_buf[0] = '\0';

            if (phase->arg != NULL && phase->arg[0] != '\0') {
                /* the simulator gets j, n and w and its own flags */
                memset(g_flags_buf, 0, sizeof(g_flags_buf));
                snprintf(g_flags_buf, sizeof(g_flags_buf), "&%s%s%s%sv", opt->flag_j ? "j" : "",
                         opt->flag_n ? "n" : "", opt->flag_w ? "w" : "", phase->arg);
                g_na[6] = g_flags_buf;
            }

            for (int i = 1; i < g_na_argc; i++) {
                size_t cur_len = strlen(local_arg_buf);
                snprintf(local_arg_buf + cur_len, sizeof(local_arg_buf) - cur_len, "%s ", g_na[i]);
            }

            if (phase->id != 9) {
                print_flushed("%s\n\r", phase->name);
            }
            time_t phase_start = time(NULL);
            if (phase->id == 9) {
                dispatch_err = cuplk_run_fitter(&phase_ret);
            } else {
                dispatch_err = cuplk_dispatch_phase(
                    phase, phase->name, phase->name,
                    local_arg_buf, g_na_argc, (char **)g_na, 0, &phase_ret, 0);
            }
            time_t phase_end = time(NULL);
            if (phase->id != 9) {
                print_flushed("time: %ld secs\n\r", (long)(phase_end - phase_start));
            }
            total_status += phase_ret + dispatch_err;
            if (phase_ret != 0 || dispatch_err != 0) {
                failed_phase = phase->id;
            }
#ifdef LINCUPL_PHASE_TEST
            /* tests/phasediff: stop after the named phase and keep its files */
            {
                const char *stop_after = getenv("LINCUPL_STOP_AFTER");
                if (stop_after != NULL && strcmp(stop_after, phase->name) == 0) {
                    if (did_chdir && chdir(saved_cwd) != 0) {
                        /* best-effort restore */
                    }
                    return total_status;
                }
            }
#endif
        }
        phase++;
    }

    /* Clean up temporary intermediate files CA..CZ<pid> (incl. CM<pid>) and runfit.$$$ */
    unlink("runfit.$$$");
    for (int i = 0; i < 26; i++) {
        char tmp_path[PATH_MAX];
        snprintf(tmp_path, sizeof(tmp_path), "C%c%s", i + 'A', g_pid_buf + 1);
        unlink(tmp_path);
    }

    if (did_chdir) {
        if (chdir(saved_cwd) != 0) {
            /* best-effort restore */
        }
    }

    time_t end_time = time(NULL);
    if (failed_phase == 0) {
        print_flushed("\n\rTotal elapsed time: %ld sec\n\r", (long)(end_time - start_time));
    } else {
        /* a failed compile ends with a message that names its kind: errors
         * in the fuse map, in the simulation, or anything else, which stops
         * the compile earlier */
        const char *msg = failed_phase == 5   ? "CUPL errors encountered!"
                          : failed_phase == 8 ? "CSIM errors encountered!"
                                              : "Fatal CUPL errors encountered ... program aborted!";
        print_flushed("Total time: %ld secs\n\r", (long)(end_time - start_time));
        print_flushed("\n\r%s", msg);
    }
    return total_status;
}
