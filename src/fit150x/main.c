/*
 * main.c - main, command line and the job function of the ATF150x fitter.
 *
 * One program serves the three devices: it is installed as fit1502, fit1504
 * and fit1508 and selects the device from the name it was started under
 * (then from the value of -dev / -device, then defaults to the 1508).
 *
 * The flow: banner, argv -> one command string ("fit1508 <in> -name value
 * ..."; "-str NAME VALUE" becomes " -NAME VALUE"), command string ->
 * parameter list, then the job: check the parameters, open the report,
 * read the design, apply the options, load the gate library and map the
 * design onto it, run the fit driver.
 *
 * The command file "-c file" is not supported. EDIF and EQN pin assignments are
 * read by the input stage when "-pinfile" is specified.
 *
 * FITTERDIR is joined with '/'; without FITTERDIR the directory holding
 * atmel.std is searched for; the temporary log is created in the system's
 * temporary directory.
 */
#include "platform.h"
#include "banner.h"
#include "locate.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base_int.h"
#include "dev.h"
#include "fit.h"
#include "lib.h"
#include "min.h"
#include "syn.h"
#include "tt.h"

FitOptions g_opt;               /* the options of the run */
int g_fit_mode;              /* enum FitMode */
const char *g_family;
char *g_version;
char *g_command;
char *g_fitterdir;
FitList *g_preassign;
char *g_in_base;
char *g_jedec_file;
char *g_vector_file;
char *g_verilog_file;
char *g_time_file;
char *g_tech_name;
char *g_in_name;

static int   g_signal_count;
static int   g_job_entered;
static FILE *g_log_in;          /* never opened */
static FILE *g_log_out;         /* never opened */
static char *g_dir_copy_3;      /* third copy of the FITTERDIR prefix, never read */
static char *g_dir_copy_4;      /* fourth copy, never read */

void fit_drv_reset(void);      /* fit_drv.c: start state of the fit modules */

static void main_cleanup_exit(int reinstall);
static void main_install_signals(int enable);

static void not_supported(const char *what)
{
    fflush(stdout);
    fprintf(stderr, "fit150x: %s is not supported\n", what);
    exit(3);
}

/* Where atmel.std is when FITTERDIR does not say: the Fitters directory of
 * $LINCUPL_SHARE, the directory of the program, the usual places relative
 * to it, the system directories.  Returns a new string, "" if not found. */
static char *main_find_fitter_dir(void)
{
    static const char *const rel[] = {
        "", "/../share/lincupl/Fitters", "/share/lincupl/Fitters", "/../../share/lincupl/Fitters", "/../Fitters",
        "/../../../share/lincupl/Fitters", "/../../../../share/lincupl/Fitters"
    };
    static const char *const fixed[] = { "/usr/local/share/lincupl/Fitters", "/usr/share/lincupl/Fitters" };
    char cand[PATH_MAX], probe[PATH_MAX + 16], resolved[PATH_MAX], exe_dir[PATH_MAX];
    const char *share = getenv("LINCUPL_SHARE");

    if (share != NULL && share[0] != '\0') {
        snprintf(cand, sizeof(cand), "%s/Fitters", share);
        snprintf(probe, sizeof(probe), "%s/atmel.std", cand);
        if (lincupl_resolve_ci_path(probe, resolved, sizeof(resolved)) == 0)
            return util_strdup(cand);
    }
    if (lincupl_get_exe_dir(exe_dir, sizeof(exe_dir))) {
        for (size_t i = 0; i < sizeof(rel) / sizeof(rel[0]); i++) {
            snprintf(cand, sizeof(cand), "%s%s", exe_dir, rel[i]);
            snprintf(probe, sizeof(probe), "%s/atmel.std", cand);
            if (lincupl_resolve_ci_path(probe, resolved, sizeof(resolved)) == 0)
                return util_strdup(cand);
        }
    }
    for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
        snprintf(probe, sizeof(probe), "%s/atmel.std", fixed[i]);
        if (lincupl_resolve_ci_path(probe, resolved, sizeof(resolved)) == 0)
            return util_strdup(fixed[i]);
    }
    return util_strdup("");
}

/* the directory of the gate library ($FITTERDIR) and the
 * prefixes made from it, ending in '/'; temporary files go to $TMPDIR
 * (default /tmp). */
static int main_init_paths(void)
{
    const char *env = util_getenv("FITTERDIR");
    const char *tmp = getenv("TMPDIR");
    char prefix[PATH_MAX];

    if (env == NULL || env[0] == '\0')
        g_fitterdir = main_find_fitter_dir();
    else
        g_fitterdir = util_strdup(env);
    g_lib_prefix = util_strdup(g_fitterdir);
    g_dir_copy_3 = util_strdup(g_fitterdir);
    g_dir_copy_4 = util_strdup(g_fitterdir);
    util_str_append(&g_lib_prefix, "/");
    util_str_append(&g_dir_copy_3, "/");
    util_str_append(&g_dir_copy_4, "/");
#ifdef _WIN32
    if (tmp == NULL || tmp[0] == '\0')
        tmp = getenv("TEMP");
    if (tmp == NULL || tmp[0] == '\0')
        tmp = getenv("TMP");
    if (tmp == NULL || tmp[0] == '\0')
        tmp = ".";
#else
    if (tmp == NULL || tmp[0] == '\0')
        tmp = "/tmp";
#endif
    snprintf(prefix, sizeof(prefix), "%s/fit150x.%ld.", tmp, (long)getpid());
    g_tmp_prefix = util_strdup(prefix);
    return 0;
}

/* start of a run: exit code, message stream, paths, entity table. */
static void main_init(FILE *log_in, FILE *log_out)
{
    g_signal_count = 0;
    g_tree_gc = NULL;
    g_exit_code = 0;
    (void)log_in;
    if (log_out == NULL)
        log_out = stdout;
    g_msg = log_out;
    util_echo_set(log_out);
    main_init_paths();
    g_verbose = 0;
    fit_prep_nop();
    entity_table_init();
    util_timer_reset();
}

/* Technology options have not been parsed yet, so name only the family. */
static void main_print_banner(void)
{
    g_family = dev_family_string();
    g_version = util_strdup(PROGRAM_VERSION);
    fprintf(g_msg, "%s %s fit%s: ATF%s Fitter\n", PROGRAM_NAME, g_version, g_family, g_family);
    fprintf(g_msg, "%s\n", PROGRAM_COPYRIGHT);
}

/* index of `option` among argv[1], argv[3], argv[5] ... */
static int main_find_deb_option(int argc, char **argv, const char *option)
{
    for (int i = 1; i < argc; i += 2) {
        if (util_str_choice(argv[i], 1, option) != 0)
            return i;
    }
    return 0;
}

/* messages back to the console */
static void main_reset_msg(void)
{
    g_verbose = 1;
    if (g_msg != stdout)
        util_fclose(g_msg);
    g_msg = stdout;
    util_echo_off();
    util_echo_set(g_msg);
}

/* SIGINT prints "User Interruption" and returns to the job,
 * which ends the run with exit code 4; any other signal but SIGTERM is an
 * internal error and ends the program with status 7. */
static void main_signal_handler(int sig)
{
    signal(sig, main_signal_handler);
    main_reset_msg();
    if (sig == SIGINT) {
        g_exit_code = 4;
        fprintf(g_msg, "\nUser Interruption\n");
    } else {
        if (sig != SIGTERM) {
            g_exit_code = 7;
            fprintf(g_msg, "\n\nINTERNAL ERROR - Please contact your Hot-Line\n");
            exit(g_exit_code);
        }
        fflush(g_msg);
        g_exit_code = 0;
        main_cleanup_exit(1);
    }
    fflush(g_msg);
    g_signal_count++;
    if (g_signal_count < 0x32)
        longjmp(g_job_jmp, g_signal_count);
    fprintf(g_msg, "\nToo many internal errors or user interruptions. Exiting.\n");
    exit(g_exit_code);
}

/* removes the temporary files, closes the logs and exits with
 * g_exit_code. */
static void main_cleanup_exit(int reinstall)
{
    tmp_cleanup();
    if (g_log_in != NULL)
        util_fclose(g_log_in);
    if (g_log_out != NULL)
        util_fclose(g_log_out);
    util_final_cleanup();
    exit(g_exit_code);
    main_install_signals(reinstall);    /* not reached */
}

/* installs main_signal_handler for SIGSEGV, SIGABRT, SIGINT, SIGILL. */
static void main_install_signals(int enable)
{
    if (enable != 0) {
        signal(SIGSEGV, main_signal_handler);
        signal(SIGABRT, main_signal_handler);
        signal(SIGINT, main_signal_handler);
        signal(SIGILL, main_signal_handler);
    }
}

/* "-deb N" with N one of 0, 1, 2; the value is not used */
static int main_parse_debug_level(int argc, char **argv)
{
    int i = main_find_deb_option(argc, argv, "-deb");

    if (i == 0)
        return 0;
    if (i + 1 > argc || util_str_choice(argv[i + 1], 3, "0", "1", "2") == 0) {
        fprintf(stdout, "Argument expected for -deb is one out of 0, 1, 2\n");
        return 0;
    }
    return atoi(argv[i + 1]);
}

/* Help lines of the extended mode, each for some of the devices */
enum { D1502 = 1, D1504 = 2, D1508 = 4, DALL = 7 };

typedef struct {
    int devices;
    const char *text;
} HelpLine;

static void main_print_help(const HelpLine *lines, size_t n)
{
    int dev = util_str_icmp(g_family, "1502") == 0 ? D1502 : util_str_icmp(g_family, "1504") == 0 ? D1504 : D1508;

    for (size_t i = 0; i < n; i++) {
        if ((lines[i].devices & dev) != 0)
            fprintf(g_msg, "%s", lines[i].text);
    }
}

/* the short usage text of the extended mode; returns 0. */
static int main_usage_ext(void)
{
    static const HelpLine lines[] = {
        { DALL, " Options:\n" },
        { DALL, "   -help\n" },
        { DALL, "   -o output_file_name (for *.tt3 and *.jed)\n" },
        { D1508, "   -device package_type (PLCC84/TQFP100/PQFP100/PQFP160)\n" },
        { D1508, "   -tech tech_name (ATF1508AS/ATF1508ASV/ATF1508BE)\n" },
        { D1502, "   -device package_type (PLCC44/TQFP44)\n" },
        { D1502, "   -tech tech_name (ATF1502AS/ATF1502ASV/ATF1502BE)\n" },
        { D1504, "   -device package_type  (PLCC44/TQFP44/PLCC68/PLCC84/PQFP100/TQFP100)\n" },
        { D1504, "   -tech tech_name (ATF1504AS/ATF1504ASV/ATF1504BE)\n" },
        { DALL, "   -module module_name\n" },
        { DALL, "   -preassign TRY|keep|ignore (pin preassignment options)\n" },
        { DALL, "   -silent (no message on screen)\n" },
        { DALL, "   -h2 (advanced help option)\n" },
        { DALL, "   -has (advanced help option for AS)\n" },
        { DALL, "   -hbe (advanced help option for BE)\n" },
    };

    fprintf(g_msg, " Usage: fit%s [-i] input_file[.tt2] {options}\n", g_family);
    main_print_help(lines, sizeof(lines) / sizeof(lines[0]));
    return 0;
}

/* the -strategy options from Latch_Synthesis on, the same in every extended mode
 * help text; returns 0. */
static int main_usage_common_ext(void)
{
    static const char *const lines[] = {
        "   -strategy Latch_Synthesis [ON | off ]\n",
        "   -strategy Optimize [ON | off]\n",
        "   -strategy Cascade_Logic [ON | off |= pin_name1 ..pin_nameN]\n",
        "   -strategy Foldback_Logic [ON | off |= node_name1 ..node_nameN]\n",
        "   -strategy Soft_Buffer [on | OFF |= node_name1 ..node_nameN]\n",
        "   -strategy XOR_Synthesis [on | OFF |= pin_name1 ..pin_nameN]\n",
        "   -strategy Push_Gate [on | OFF]\n",
        "   -strategy Verilog_sim [sdf | Verilog | OFF]\n",
        "   -strategy Vhdl_sim [sdf | vhdl | OFF]\n",
        "   -strategy Out_Edif [on | OFF]\n",
        "   -strategy Global_Fold [node_name1 ..node_nameN]\n",
        "   -strategy Global_OE [node_name1 ..node_nameN]\n",
        "   -strategy OE_node [node_Number1..node_NumberN]\n",
        "   -strategy logic_doubling [on | OFF]\n",
        "   -strategy twoclock [clockname]\n",
        "   -strategy pinfile [file] \n"
    };

    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
        fprintf(g_msg, "%s", lines[i]);
    return 0;
}

/* the "-h2" text of the extended mode: the -strategy options of all technologies;
 * returns 0. */
static int main_usage_all_ext(void)
{
    static const HelpLine lines[] = {
        { DALL, "   -strategy c [command file name]\n" },
        { DALL, "   -strategy ifmt (input file format) [TT | edif | eqn]\n" },
        { DALL, "   -strategy lib (library file name for edif input)\n" },
        { DALL, "   -strategy MC_power = [   OFF |   on | = pin_name1 pin_name2...] \n" },
        { DALL, "   -strategy open_collector = [   OFF |   on  | = pin_name1 pin_name2...]\n" },
        { DALL, "   -strategy JTAG = [   off |   ON ] \n" },
        { DALL, "   -strategy power_reset = [   OFF |   on ] \n" },
        { DALL, "   -strategy pd1 [   OFF |   on ] (power down 1)\n" },
        { DALL, "   -strategy pd2 [   OFF |   on ] (power down 2)\n" },
        { DALL, "   -strategy TDI_pullup = [   OFF |   on ] \n" },
        { DALL, "   -strategy TMS_pullup = [   OFF |   on ] \n" },
        { DALL, "   -strategy DEBUG = [   on |   OFF ]\n" },
        { DALL, "   -strategy output_fast [ON | off | = pin_name1 pin_name2...]\n" },
        { DALL, "   -strategy pin_keep [ off | ON | = pin_name1 pin_name2...]\n" },
        { D1504 | D1508, "   -strategy no_tff [ OFF | on ] \n" },
        { DALL, "   -strategy ues [value ] (2 ASCII characters)\n" },
        { DALL, "   -strategy security [ OFF | on ]\n" },
        { DALL, "   -strategy tPD = [ 5 |15 | 7 | 10 | 20 | 25 ]\n" },
        { D1504 | D1508, "   -strategy voltage_level_A [ 1.5 | 1.8 | 2.5 | 3.3]\n" },
        { D1504 | D1508, "   -strategy voltage_level_B [ 1.5 | 1.8 | 2.5 | 3.3]\n" },
        { D1504 | D1508, "   -strategy SSTL_input [OFF | = pin_name1 pin_name2...]\n" },
        { D1502, "   -strategy voltage_level_A [ 1.8 | 2.5 | 3.3]\n" },
        { D1502, "   -strategy voltage_level_B [ 1.8 | 2.5 | 3.3]\n" },
        { DALL, "   -strategy fast_inlatch [ OFF | on | = pin_name1 pin_name2...]\n" },
        { DALL, "   -strategy schmitt_trigger [ OFF | = pin_name1 pin_name2...]\n" },
        { DALL, "   -strategy pull_up [ OFF | = pin_name1 pin_name2...]\n" },
        { DALL, "   -strategy unused_To_PinKeeper [ off | ON ] \n" },
        { DALL, "   -strategy pull_up_unused [ OFF | on]\n" },
        { DALL, "   -strategy unused_To_Ground [ OFF | on]\n" },
        { DALL, "   -strategy pull_down [ OFF | = dedicated_pin1 dedicated_pin2...]\n" },
        { DALL, "   -strategy GCLK_ITD = [ AUTO | on | off | GCLK1 | GCLK2 | GCLK3 ]\n" },
    };

    main_print_help(lines, sizeof(lines) / sizeof(lines[0]));
    return main_usage_common_ext();
}

/* the "-has" text of the extended mode: the -strategy options of AS devices; returns 0. */
static int main_usage_as_ext(void)
{
    static const HelpLine lines[] = {
        { DALL, "   -strategy c [command file name]\n" },
        { DALL, "   -strategy ifmt (input file format) [TT | edif | eqn]\n" },
        { DALL, "   -strategy lib (library file name for edif input)\n" },
        { DALL, "   -strategy MC_power = [   OFF |   on | = pin_name1 pin_name2...] \n" },
        { DALL, "   -strategy open_collector = [   OFF |   on  | = pin_name1 pin_name2...]\n" },
        { DALL, "   -strategy JTAG = [   off |   ON ] \n" },
        { DALL, "   -strategy power_reset = [   OFF |   on ] \n" },
        { DALL, "   -strategy pd1 [   OFF |   on ] (power down 1)\n" },
        { DALL, "   -strategy pd2 [   OFF |   on ] (power down 2)\n" },
        { DALL, "   -strategy TDI_pullup = [   OFF |   on ] \n" },
        { DALL, "   -strategy TMS_pullup = [   OFF |   on ] \n" },
        { DALL, "   -strategy DEBUG = [   on |   OFF ]\n" },
        { DALL, "   -strategy output_fast [ON | off | = pin_name1 pin_name2...]\n" },
        { DALL, "   -strategy pin_keep [ off | ON]\n" },
        { DALL, "   -strategy ues [value ] (2 ASCII characters)\n" },
        { DALL, "   -strategy security [ OFF | on ]\n" },
        { DALL, "   -strategy tPD = [ 15 | 7 | 10 | 20 | 25 ]\n" },
        { DALL, "   -strategy fast_inlatch [ OFF | on | = pin_name1 pin_name2...]\n" },
        { DALL, "   -strategy GCLK_ITD = [ AUTO | on | off | GCLK1 | GCLK2 | GCLK3 ]\n" },
    };

    main_print_help(lines, sizeof(lines) / sizeof(lines[0]));
    return main_usage_common_ext();
}

/* the "-hbe" text of the extended mode: the -strategy options of BE devices; returns 0. */
static int main_usage_be_ext(void)
{
    static const HelpLine lines[] = {
        { DALL, "   -strategy c [command file name]\n" },
        { DALL, "   -strategy ifmt (input file format) [TT | edif | eqn]\n" },
        { DALL, "   -strategy lib (library file name for edif input)\n" },
        { DALL, "   -strategy open_collector = [   OFF |   on  | = pin_name1 pin_name2...]\n" },
        { DALL, "   -strategy JTAG = [   off |   ON ] \n" },
        { DALL, "   -strategy pd1 [   OFF |   on ] (power down 1)\n" },
        { DALL, "   -strategy pd2 [   OFF |   on ] (power down 2)\n" },
        { DALL, "   -strategy TDI_pullup = [   OFF |   on ] \n" },
        { DALL, "   -strategy TMS_pullup = [   OFF |   on ] \n" },
        { DALL, "   -strategy DEBUG = [   on |   OFF ]\n" },
        { DALL, "   -strategy output_fast [on | OFF | = pin_name1 pin_name2...]\n" },
        { DALL, "   -strategy pin_keep [ off | = pin_name1 pin_name2...]\n" },
        { DALL, "   -strategy ues [value ] (2 ASCII characters)\n" },
        { DALL, "   -strategy security [ OFF | on ]\n" },
        { D1504 | D1508, "   -strategy no_tff [ OFF | on ] \n" },
        { DALL, "   -strategy tPD = [ 5 | 7 ]\n" },
        { D1504 | D1508, "   -strategy voltage_level_A [ 1.5 | 1.8 | 2.5 | 3.3]\n" },
        { D1504 | D1508, "   -strategy voltage_level_B [ 1.5 | 1.8 | 2.5 | 3.3]\n" },
        { D1504 | D1508, "   -strategy SSTL_input [OFF | = pin_name1 pin_name2...]\n" },
        { D1502, "   -strategy voltage_level_A [ 1.8 | 2.5 | 3.3]\n" },
        { D1502, "   -strategy voltage_level_B [ 1.8 | 2.5 | 3.3]\n" },
        { DALL, "   -strategy fast_inlatch [ OFF | on | = pin_name1 pin_name2...]\n" },
        { DALL, "   -strategy schmitt_trigger [ OFF | = pin_name1 pin_name2...]\n" },
        { DALL, "   -strategy pull_up [ OFF | = pin_name1 pin_name2...]\n" },
        { DALL, "   -strategy unused_To_PinKeeper [ off | ON ] \n" },
        { DALL, "   -strategy pull_up_unused [ OFF | on]\n" },
        { DALL, "   -strategy unused_To_Ground [ OFF | on]\n" },
        { DALL, "   -strategy pull_down [ OFF | = dedicated_pin1 dedicated_pin2...]\n" },
    };

    main_print_help(lines, sizeof(lines) / sizeof(lines[0]));
    return main_usage_common_ext();
}

/* the short usage text; returns 0. */
static int main_usage(void)
{
    if (FIT_EXT)
        return main_usage_ext();
    fprintf(g_msg, " Usage: fit%s [-i] input_file[.tt2] {options}\n", g_family);
    fprintf(g_msg, " Options:\n");
    fprintf(g_msg, "   -help\n");
    fprintf(g_msg, "   -o output_file_name (for *.tt3 and *.jed)\n");
    if (util_str_icmp(g_family, "1508") == 0)
        fprintf(g_msg, "   -device device_name (P1508C68/P1508C84/P1508T100/P1508Q100/P1508Q160)\n");
    else if (util_str_icmp(g_family, "1502") == 0)
        fprintf(g_msg, "   -device (or -dev) device_name (P1502C44/P1502T44)\n");
    else
        fprintf(g_msg, "   -device device_name (P1504C44/P1504T44/P1504C68/P1504C84/P1504Q100/P1504T100)\n");
    fprintf(g_msg, "   -module module_name\n");
    fprintf(g_msg, "   -preassign TRY|keep|ignore (pin preassignment options)\n");
    fprintf(g_msg, "   -silent (no message on screen)\n");
    fprintf(g_msg, "   -h2 (advanced help option)\n");
    return 0;
}

/* the "-h2" usage text of the -strategy options; returns 0. */
static int main_usage_advanced(void)
{
    static const char *const lines[] = {
        "   -strategy c [command file name]\n",
        "   -strategy ifmt (input file format) [TT2 | edif | eqn]\n",
        "   -strategy lib (library file name for edif input)\n",
        "   -strategy MC_power = [   OFF |   on | = pin_name1 pin_name2...] \n",
        "   -strategy open_collector = [   OFF |   on  | = pin_name1 pin_name2...]\n",
        "   -strategy JTAG = [   OFF |   on ] \n",
        "   -strategy power_reset = [   OFF |   on ] \n",
        "   -strategy pd1 [   OFF |   on | = pin_name ] (power down 1)\n",
        "   -strategy pd2 [   OFF |   on | = pin_name ] (power down 2)\n",
        "   -strategy TDI_pullup = [   OFF |   on ] \n",
        "   -strategy TMS_pullup = [   OFF |   on ] \n",
        "   -strategy DEBUG = [   on |   OFF ]\n",
        "   -strategy output_fast [on | OFF | = pin_name1 pin_name2...]\n",
        "   -strategy pin_keep [ off | ON | = pin_name1 pin_name2...]\n",
        "   -strategy security [ OFF | on ]\n",
        "   -strategy tPD = [ 15 | 7 | 10 | 12 | 25 ]\n",
        "   -strategy fast_inlatch [ OFF | on | = pin_name1 pin_name2...]\n",
        "   -strategy GCK_ITD = [ AUTO | on | off | GCK0 | GCK1 | GCK2 ]\n",
        "   -strategy Latch_Synthesis [ON | off ]\n",
        "   -strategy Optimize [ON | off]\n",
        "   -strategy Cascade_Logic [ON | off |= pin_name1 ..pin_nameN]\n",
        "   -strategy Foldback_Logic [ON | off |= node_name1 ..node_nameN]\n",
        "   -strategy Soft_Buffer [on | OFF |= node_name1 ..node_nameN]\n",
        "   -strategy XOR_Synthesis [on | OFF |= pin_name1 ..pin_nameN]\n",
        "   -strategy Push_Gate [on | OFF]\n",
        "   -strategy Verilog_sim [sdf | Verilog | OFF]\n",
        "   -strategy Vhdl_sim [sdf | vhdl | OFF]\n",
        "   -strategy Out_Edif [on | OFF]\n",
        "   -strategy Global_Fold [node_name1 ..node_nameN]\n",
        "   -strategy Global_OE [node_name1 ..node_nameN]\n",
        "   -strategy OE_node [node_Number1..node_NumberN]\n",
        "   -strategy logic_doubling [on | OFF]\n",
        "   -strategy twoclock [clockname]\n",
        "   -strategy pinfile [file] \n"
    };

    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
        fprintf(g_msg, "%s", lines[i]);
    return 0;
}

/* Preserve a single argv value when the command parser splits words. */
static void main_append_value(char **cmd, const char *value)
{
    int quote = value[0] != '"' && strpbrk(value, " \t\r\n") != NULL;

    if (quote)
        util_str_append(cmd, "\"");
    util_str_append(cmd, value);
    if (quote)
        util_str_append(cmd, "\"");
}

/* "-str NAME VALUE", "-str NAME = V1 V2 ...", "-str NAME =V1 V2
 * ..." or "-str NAME=VALUE" -> " -NAME VALUE" (several values joined by
 * commas) appended to the command string.  *pi is the index of NAME on
 * entry and of the last argument consumed on return. */
static int main_append_strategy(char **argv, int argc, int *pi, char **cmd)
{
    /* "NAME=VALUE" is split into one 24-byte block: the name in the
     * first 12 bytes, the value in the last 12.  A name longer than 11
     * characters runs into the value, which then overwrites its tail, and
     * the command string carries that mixed text.  This is deliberate:
     * the command string and the parameters taken from it depend on it.
     * An argument that does not fit the block at all is split at its
     * first and last '=' instead. */
    char both[24 + 1];
    char *name = both, *value = both + 12;
    const char *arg = argv[*pi];
    int seen = 0, overflow = 0;
    size_t vi = 0, len;

    if (arg == NULL) {
        /* "-str" was the last argument: stop as on a crash, with "INTERNAL
         * ERROR" (exit status 7) */
        main_signal_handler(SIGSEGV);
        return 0;
    }
    if (strstr(arg, "=") == NULL) {
        char *next = *pi + 1 <= argc ? argv[*pi + 1] : NULL;

        if (next == NULL) {
            /* no value after the name: stop as on a crash, with "INTERNAL
             * ERROR" (exit status 7) */
            main_signal_handler(SIGSEGV);
        }
        if (next[0] == '=') {
            int k;

            util_str_append(cmd, " -");
            util_str_append(cmd, argv[*pi]);
            util_str_append(cmd, " ");
            if (util_str_icmp(next, "=") != 0) {
                next[0] = ' ';
                (*pi)--;
            }
            if (*pi + 2 > argc || argv[*pi + 2] == NULL)
                main_signal_handler(SIGSEGV);
            util_str_append(cmd, argv[*pi + 2]);
            k = *pi + 3;
            *pi += 2;
            while (k <= argc && argv[k] != NULL && argv[k][0] != '-') {
                *pi = k;
                /* the extended mode adds no comma after a value that ends with one */
                if (!FIT_EXT || (*cmd)[strlen(*cmd) - 1] != ',')
                    util_str_append(cmd, ",");
                util_str_append(cmd, argv[*pi]);
                k = *pi + 1;
            }
        } else {
            if (next[0] == '-')
                return main_usage();
            util_str_append(cmd, " -");
            util_str_append(cmd, argv[*pi]);
            util_str_append(cmd, " ");
            main_append_value(cmd, next);
            (*pi)++;
        }
        return 1;
    }
    memset(both, 0, sizeof(both));
    len = strlen(arg);
    for (size_t u = 0; u <= len; u++) {
        char c = arg[u];

        if (c == '=') {
            seen = 1;
            vi = 0;
            if (u >= 24)
                overflow = 1;
            else
                name[u] = '\0';
        } else if (seen) {
            if (vi >= 12)
                overflow = 1;
            else
                value[vi] = c;
            vi++;
        } else if (u >= 24) {
            overflow = 1;
        } else {
            name[u] = c;
        }
    }
    util_str_append(cmd, " -");
    if (overflow) {
        /* the argument did not fit the block: name before the first '=',
         * value after the last one */
        char *copy = util_strdup(arg);

        *strchr(copy, '=') = '\0';
        util_str_append(cmd, copy);
        util_str_append(cmd, " ");
        main_append_value(cmd, strrchr(arg, '=') + 1);
        free(copy);
        return 1;
    }
    util_str_append(cmd, name);
    util_str_append(cmd, " ");
    main_append_value(cmd, value);
    return 1;
}

/* argv -> "fit<family> word -name value ...".  Returns 0 after
 * printing the usage (no arguments, -help, -h2, a lone option, a third
 * positional word, a strategy without value). */
static int main_build_command(int argc, char **argv, char **cmd)
{
    if (argc < 2)
        return main_usage();
    *cmd = util_strdup("fit");
    util_str_append(cmd, g_family);
    if (argc == 2) {
        if (util_str_icmp(argv[1], "-help") == 0)
            return main_usage();
        if (FIT_EXT) {
            if (util_str_icmp(argv[1], "-h2") == 0)
                return main_usage_all_ext();
            if (util_str_icmp(argv[1], "-has") == 0)
                return main_usage_as_ext();
            if (util_str_icmp(argv[1], "-hbe") == 0)
                return main_usage_be_ext();
        } else if (util_str_icmp(argv[1], "-h2") == 0) {
            return main_usage_advanced();
        }
        if (argv[1][0] == '-')
            return main_usage();
    }
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (util_str_icmp(argv[i], "-strategy") == 0 || util_str_icmp(argv[i], "-str") == 0) {
                i++;
                if (main_append_strategy(argv, argc, &i, cmd) == 0)
                    return 0;
            } else {
                /* the extended mode gives -i and -lib all the words up to the next option */
                int many = FIT_EXT && (util_str_icmp(argv[i], "-i") == 0 || util_str_icmp(argv[i], "-lib") == 0);

                util_str_append(cmd, " ");
                util_str_append(cmd, argv[i]);
                if (i < argc - 1 && argv[i + 1][0] != '-') {
                    i++;
                    util_str_append(cmd, " ");
                    main_append_value(cmd, argv[i]);
                    while (many && i + 1 < argc && argv[i + 1][0] != '-') {
                        i++;
                        util_str_append(cmd, " ");
                        util_str_append(cmd, argv[i]);
                    }
                }
            }
        } else {
            if (i > 2)
                return main_usage();
            util_str_append(cmd, " ");
            main_append_value(cmd, argv[i]);
        }
    }
    return 1;
}

/* command string (without the program name) -> parameter list;
 * the first two parameters, when they are plain words, are named "i" and
 * "o". */
static int main_command_to_params(char *cmd, FitList **params)
{
    FitParam *p;

    if (param_parse_command(cmd, params) == 0) {
        if (*params != NULL)
            list_destroy(params);
        return 0;
    }
    if (*params == NULL || (*params)->count < 1)    /* cannot happen: main requires an argument */
        return 1;
    for (int i = 0; i < (*params)->count; i++) {
        FitParam *value = (*params)->items[i];
        size_t length = strlen(value->value);

        if (length >= 2 && value->value[0] == '"' &&
            value->value[length - 1] == '"') {
            memmove(value->value, value->value + 1, length - 2);
            value->value[length - 2] = '\0';
        }
    }
    p = (*params)->items[0];
    if (util_str_icmp(p->name, g_empty_str) == 0)
        strcpy(p->name, "i");
    if ((*params)->count > 1) {
        p = (*params)->items[1];
        if (util_str_icmp(p->name, g_empty_str) == 0)
            strcpy(p->name, "o");
    }
    return 1;
}

/* command string -> parameter list (the first word, the program name,
 * is dropped); reports a usage error and returns 0 when the string cannot
 * be parsed. */
static int main_parse_command(const char *command, FitList **params)
{
    char *copy = util_strdup(command);
    char *rest = copy;
    char *program = util_first_word(&rest);
    int ok;

    if (main_command_to_params(rest, params) == 0) {
        util_report_error(FIT_ERR_USAGE, program);
        free(program);
        ok = 0;
    } else {
        if (program != NULL)
            free(program);
        ok = 1;
    }
    free(copy);
    return ok;
}

/* index of `option` in argv, 0 if absent */
static int main_find_c_option(int argc, char **argv, const char *option)
{
    for (int i = 1; i < argc; i++) {
        if (util_str_choice(argv[i], 1, option) != 0)
            return i;
    }
    return 0;
}

/* a new string with what follows the last '.' of the file name,
 * NULL if the name has no '.'. */
static char *main_extension_of(const char *path)
{
    const char *sep = util_strrchr(path, '\\');
    char *ext;
    size_t n = 0, len;
    int seen = 0;

    if (sep != NULL)
        path = sep + 1;
    if (strstr(path, ".") == NULL)
        return NULL;
    len = strlen(path);
    ext = util_calloc(1, len + 4);
    for (size_t u = 0; u <= len; u++) {
        if (path[u] == '.') {
            seen = 1;
            n = 0;
        } else if (seen) {
            ext[n++] = path[u];
        }
    }
    return ext;
}

/* the path without ".ext"; the path itself if the file name has
 * no '.' */
static char *main_strip_extension(char *path)
{
    char *ext = main_extension_of(path);
    char *out;
    size_t keep;

    if (ext == NULL)
        return path;
    keep = strlen(path) - strlen(ext) - 1;
    out = util_calloc(1, keep + 1);
    memcpy(out, path, keep);
    out[keep] = '\0';
    free(ext);
    return out;
}

/* a register (or latch, tri-state, wire) of the netlist that has
 * a name of its own (inst_name): the node feeding it is renamed to that name if
 * no variable has the name yet. */
static void main_rename_register_nodes(FitEntity *e)
{
    for (int i = 0; i < (e->net->items == NULL ? 0 : e->net->items->count); i++) {
        FitNetItem *item = e->net->items->items[i];
        FitInstance *inst;
        FitVariable *v;
        FitAlias *alias;
        char **field;
        int pos;

        if (item->kind != FIT_GATE_DFF && item->kind != FIT_GATE_TFF && item->kind != FIT_GATE_TH &&
            item->kind != FIT_GATE_LATCH && item->kind != FIT_GATE_WIRE)
            continue;
        inst = item->cell;
        if (inst->inst_name == NULL)
            continue;
        if (inst->pin != NULL)
            field = &inst->pin;
        else if (inst->d2 != NULL)
            field = &inst->d2;
        else
            field = &inst->d;
        if (*field == NULL)         /* no such field */
            continue;
        v = entity_find_var(e, *field);
        if (v == NULL)              /* no such variable */
            continue;
        if (entity_var_is_node(e, v) == 0)
            continue;
        if (entity_var_id_by_name(e, inst->inst_name) != 0)
            continue;
        flatten_free_ref((void **)field);
        *field = util_strdup(inst->inst_name);
        pos = list_find(e->net->aliases, v->name, alias_name_eq);
        if (pos != 0) {             /* no alias of that name */
            alias = e->net->aliases->items[pos - 1];
            flatten_free_ref((void **)&alias->name);
            alias->name = util_strdup(inst->inst_name);
        }
        snprintf(v->name, sizeof(v->name), "%s", inst->inst_name);
    }
}

/* -i is required; crit, ifmt, tt and debug are validated and get
 * their defaults AREA, tt, OFF, OFF */
static int main_check_params(FitList *params)
{
    char *v = param_get(params, "i");

    if (v == NULL) {
        fprintf(g_msg, "%s\n", "-i parameter required ...");
        return 0;
    }
    flatten_free_ref((void **)&v);

    v = param_get(params, "crit");
    if (v == NULL) {
        list_add(params, param_new("crit", "AREA"));
    } else if (util_str_choice(v, 4, "AREA", "SPEED", "SPEED_AREA", "AREA_SPEED") == 0) {
        util_report_error(FIT_ERR_PARAM_VALUE, "allowed values for -crit are AREA/SPEED/SPEED_AREA/AREA_SPEED ");
        return 0;
    }
    flatten_free_ref((void **)&v);

    v = param_get(params, "ifmt");
    if (v == NULL) {
        list_add(params, param_new("ifmt", "tt"));
    } else if (util_str_choice(v, 3, "EQN", "TT", "EDIF") == 0) {
        util_report_error(FIT_ERR_PARAM_VALUE, "allowed values for -ifmt are EQN/TT/EDIF ");
        return 0;
    }
    flatten_free_ref((void **)&v);

    v = param_get(params, "tt");
    if (v == NULL) {
        list_add(params, param_new("tt", "OFF"));
    } else if (util_str_choice(v, 2, "ON", "OFF") == 0) {
        util_report_error(FIT_ERR_PARAM_VALUE, "allowed values for -tt are ON/OFF ");
        return 0;
    }
    flatten_free_ref((void **)&v);

    v = param_get(params, "debug");
    if (v == NULL) {
        list_add(params, param_new("debug", "OFF"));
    } else if (util_str_choice(v, 2, "ON", "OFF") == 0) {
        util_report_error(FIT_ERR_PARAM_VALUE, "allowed values for -debug are ON/OFF ");
        return 0;
    }
    flatten_free_ref((void **)&v);
    return 1;
}

/* 1 when the netlist holds a gate of unknown kind. */
static int main_has_unknown_gate(FitEntity *e)
{
    FitList *items = e->net->items;

    for (int i = 0; i < (items == NULL ? 0 : items->count); i++) {
        if (((FitNetItem *)items->items[i])->kind == FIT_GATE_UNKNOWN)
            return 1;
    }
    return 0;
}

/* the file name part of a path */
static char *main_input_name(char *path)
{
    const char *sep = util_strrchr(path, '\\');

    if (sep == NULL)
        return path;
    return path + (sep - path) + 1;
}

static int str_ieq_cb(void *a, void *b)
{
    return util_str_ieq(a, b);
}

/* the signals named by foldback_Logic, global_fold and
 * soft_buffer must survive entity_simplified_copy: their ids go on
 * g_keep_ids.  A named local that nothing uses, but whose "<name>_PIN"
 * twin is used, swaps names with the twin. */
static int main_mark_kept_signals(void)
{
    FitEntity *e;
    void *object;
    FitList *names;
    int type, slot;

    if (entity_table_get("Fct", FIT_ENT_ENTITY, &type, &object, &slot) == 0) {
        fprintf(g_msg, "Not possible to read an entity: ENTITY_IS_FCT.\n");
        g_exit_code = FIT_EXIT_NO_ENTITY;
        return 0;
    }
    e = object;
    names = list_union(g_opt.foldback_list, g_opt.global_fold, str_ieq_cb);
    names = list_union(names, g_opt.soft_buffer, str_ieq_cb);   /* the first union is not released */
    if (names != NULL && names->count != 0)
        g_keep_ids = list_create();
    for (int i = 0; i < (names == NULL ? 0 : names->count); i++) {
        char *name = names->items[i];
        FitVariable *v = entity_find_var(e, name);
        FitVariable *w;
        char *pin_name;

        if (v == NULL)
            continue;
        if (tt2_count_id_uses(e, v->id) != 0) {
            list_add(g_keep_ids, FIT_INT2PTR(v->id));
            continue;
        }
        pin_name = util_strdup(name);
        util_str_append(&pin_name, "_PIN");
        w = entity_find_var(e, pin_name);
        if (w != NULL && tt2_count_id_uses(e, w->id) != 0 && entity_is_local(e, v->id) != 0) {
            list_add(g_keep_ids, FIT_INT2PTR(w->id));
            if (latch_find_alias(w->name, e->net) == 0) {
                snprintf(w->name, sizeof(w->name), "%s", v->name);
                snprintf(v->name, sizeof(v->name), "%s", pin_name);
            }
        }
        free(pin_name);
    }
    return 1;
}

/* "a,b,c" -> new list of new strings in *out */
static void main_split_list(FitList **out, const char *text)
{
    char word[0x100];
    size_t n = 0, len = strlen(text);

    *out = list_create();
    for (size_t u = 0; u <= len; u++) {
        char c = text[u];

        if (c == ',' || c == '\0') {
            word[n] = '\0';
            list_add(*out, util_strdup(word));
            n = 0;
        } else if (n < sizeof(word) - 1) {      /* longer words are cut */
            word[n++] = c;
        }
    }
}

static void set_package(const char *name)
{
    g_dev_package = util_strdup(name);
}

/* device name ("P1508C84") -> package name ("PLCC84") in
 * g_dev_package; a name that is not a package of this device gives a
 * warning and the default (PQFP100 on the 1508, PLCC44 on the others). */
static void main_device_package(void)
{
    static const struct {
        const char *suffix, *package;
    } map[] = {
        { "T44", "TQFP44" }, { "C44", "PLCC44" }, { "C68", "PLCC68" }, { "C84", "PLCC84" },
        { "Q100", "PQFP100" }, { "T100", "TQFP100" }, { "Q160", "PQFP160" }
    };

    lincupl_strupr(g_dev_package);
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strstr(g_dev_package, map[i].suffix) != NULL) {
            set_package(map[i].package);
            break;
        }
    }
    if (util_str_choice(g_dev_package, 5, "PLCC68", "PLCC84", "PQFP100", "TQFP100", "PQFP160") == 0 &&
        util_str_icmp(g_family, "1508") == 0) {
        set_package("PQFP100");
        fprintf(g_msg, "Wrong device name, Defaulting to P1508Q100\n");
        return;
    }
    if (util_str_choice(g_dev_package, 2, "PLCC44", "TQFP44") != 0 || util_str_icmp(g_family, "1502") != 0) {
        if (util_str_choice(g_dev_package, 6, "PLCC68", "PLCC84", "PQFP100", "TQFP100", "PLCC44", "TQFP44") != 0)
            return;
        if (util_str_icmp(g_family, "1504") != 0)
            return;
    }
    set_package("PLCC44");
    fprintf(g_msg, "Wrong device name, Defaulting to PLCC44\n");
}

/* Extended mode: device name or package name -> package name in g_dev_package; a
 * name that is not a package of the device and technology gives a warning
 * and the default (BE devices come in TQFP packages only) */
static void main_device_package_ext(void)
{
    static const struct {
        const char *code, *package;
    } map[] = {
        { "T44", "TQFP44" }, { "C44", "PLCC44" }, { "C68", "PLCC68" }, { "C84", "PLCC84" },
        { "Q100", "PQFP100" }, { "T100", "TQFP100" }, { "Q160", "PQFP160" }
    };
    const char *def = NULL, *msg = NULL;
    int is1508 = util_str_icmp(g_family, "1508") == 0;
    int is1502 = util_str_icmp(g_family, "1502") == 0;
    int is1504 = util_str_icmp(g_family, "1504") == 0;

    lincupl_strupr(g_dev_package);
    lincupl_strupr(g_tech_name);
    if (strstr(g_tech_name, "BE") != NULL)
        g_opt.be = 1;
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strstr(g_dev_package, map[i].code) != NULL || strstr(g_dev_package, map[i].package) != NULL) {
            set_package(map[i].package);
            break;
        }
    }
    if (util_str_choice(g_dev_package, 5, "PLCC68", "PLCC84", "PQFP100", "TQFP100", "PQFP160") == 0 && is1508 &&
        g_opt.be == 0) {
        def = "PQFP100";
        msg = "Wrong device name, Defaulting to P1508Q100\n";
    } else if (util_str_choice(g_dev_package, 1, "TQFP100") == 0 && is1508 && g_opt.be != 0) {
        def = "TQFP100";
        msg = "Wrong device name, Defaulting to TQFP100\n";
    } else if (util_str_choice(g_dev_package, 2, "PLCC44", "TQFP44") == 0 && is1502 && g_opt.be == 0) {
        def = "PLCC44";
        msg = "Wrong device name, Defaulting to PLCC44\n";
    } else if (util_str_choice(g_dev_package, 1, "TQFP44") == 0 && is1502 && g_opt.be != 0) {
        def = "TQFP44";
        msg = "Wrong device name, Defaulting to TQFP44\n";
    } else if (util_str_choice(g_dev_package, 6, "PLCC68", "PLCC84", "PQFP100", "TQFP100", "PLCC44", "TQFP44") == 0 &&
               is1504 && g_opt.be == 0) {
        def = "PLCC44";
        msg = "Wrong device name, Defaulting to PLCC44\n";
    } else if (util_str_choice(g_dev_package, 2, "TQFP100", "TQFP44") == 0 && is1504 && g_opt.be != 0) {
        def = "TQFP44";
        msg = "Wrong device name, Defaulting to TQFP44\n";
    }
    if (def != NULL) {
        set_package(def);
        fprintf(g_msg, "%s", msg);
    }
}

/* Extended mode: the name the technology is taken from (-device, -dev, else "AS"),
 * -tech, the package */
static void main_tech_options(FitList *params)
{
    char *v = param_get(params, "device");

    if (v == NULL)
        v = param_get(params, "dev");
    if (v != NULL)
        g_tech_name = v;
    else if (g_tech_name == NULL)
        g_tech_name = util_strdup("AS");
    v = param_get(params, "tech");
    if (v != NULL) {
        lincupl_strupr(v);
        if (strstr(v, "BE") != NULL)
            g_opt.be = 1;
        if (strstr(v, "ASV") != NULL)
            g_opt.asv = 1;
    }
    v = param_get(params, "device");
    if (v == NULL)
        v = param_get(params, "dev");
    if (v == NULL && g_dev_package == NULL) {
        if (g_opt.be == 0)
            v = util_strdup(util_str_icmp(g_family, "1508") == 0 ? "PQFP100" : "PLCC44");
        else
            v = util_strdup(util_str_icmp(g_family, "1508") == 0 ? "TQFP100" : "TQFP44");
    }
    if (v != NULL)
        g_dev_package = v;
    main_device_package_ext();
    g_opt.device_package = g_dev_package;
}

/* the fitting options of the command line.  They are applied
 * after the design has been read, on top of what its PROPERTY lines set. */
static void main_fit_options(FitList *params)
{
    char *v;

    v = param_get(params, "Global_OE");
    if (v != NULL && util_str_icmp(v, "OFF") != 0 && util_str_icmp(v, "ON") != 0) {
        main_split_list(&g_opt.global_oe, v);
        free(v);
        v = param_get(params, "OE_node");
        if (v != NULL) {
            /* the n-th number of OE_node is the pin of "<n-th name>_OE" */
            char number[0x100];
            size_t n = 0, len = strlen(v);
            int k = 0;

            for (size_t u = 0; u <= len; u++) {
                char c = v[u];

                if (c == ',' || c == '\0') {
                    struct { char *name; int pin; int is_pin; } *item;
                    char name[0x100];
                    int pin;

                    number[n] = '\0';
                    n = 0;
                    pin = atoi(number);
                    k++;
                    if ((g_opt.global_oe == NULL ? 0 : g_opt.global_oe->count) < k)
                        break;
                    snprintf(name, sizeof(name), "%s_OE", (char *)g_opt.global_oe->items[k - 1]);
                    item = util_calloc(1, sizeof(*item));
                    item->pin = pin;
                    item->name = util_strdup(name);
                    list_add(g_preassign, item);
                } else if (n < sizeof(number) - 1) {    /* longer numbers are cut */
                    number[n++] = c;
                }
            }
        }
    }
    flatten_free_ref((void **)&v);

    v = param_get(params, "cascade_logic");
    if (v != NULL && util_str_icmp(v, "OFF") != 0) {
        g_opt.cascade_logic = 1;
        if (util_str_icmp(v, "ON") != 0)
            main_split_list(&g_opt.cascade_list, v);
    }
    flatten_free_ref((void **)&v);

    v = param_get(params, "foldback_Logic");
    if (FIT_EXT) {
        /* the extended mode starts with foldback logic off */
        if (v != NULL && util_str_icmp(v, "OFF") != 0) {
            g_opt.foldback = 1;
            if (util_str_icmp(v, "ON") != 0)
                main_split_list(&g_opt.foldback_list, v);
        }
    } else if (v != NULL && util_str_icmp(v, "OFF") != 0 && util_str_icmp(v, "ON") != 0) {
        main_split_list(&g_opt.foldback_list, v);
    }
    flatten_free_ref((void **)&v);

    v = param_get(params, "global_fold");
    if (v != NULL && util_str_icmp(v, "OFF") != 0 && util_str_icmp(v, "ON") != 0)
        main_split_list(&g_opt.global_fold, v);
    flatten_free_ref((void **)&v);

    v = param_get(params, "xor_synthesis");
    if (v != NULL && util_str_icmp(v, "OFF") != 0) {
        if (util_str_icmp(v, "ON") == 0)
            g_opt.xor_synthesis = 1;
        else
            main_split_list(&g_opt.xor_list, v);
    }
    flatten_free_ref((void **)&v);

    v = param_get(params, "soft_buffer");
    if (v != NULL && util_str_icmp(v, "OFF") != 0 && util_str_icmp(v, "ON") != 0)
        main_split_list(&g_opt.soft_buffer, v);
    free(v);

    v = param_get(params, "fast_inlatch");
    if (v != NULL && util_str_icmp(v, "OFF") != 0) {
        g_opt.fast_inlatch = 1;
        if (util_str_icmp(v, "ON") == 0)
            g_opt.fast_inlatch_list = NULL;
        else
            main_split_list(&g_opt.fast_inlatch_list, v);
    }
    free(v);

    v = param_get(params, "push_gate");
    if (v != NULL && util_str_icmp(v, "OFF") == 0)
        g_opt.push_gate = 0;
    free(v);

    v = param_get(params, "preassign");
    if (v == NULL)
        v = param_get(params, "pre");
    if (v != NULL) {
        if (util_str_icmp(v, "keep") == 0)
            g_opt.preassign = FIT_PREASSIGN_KEEP;
        else
            g_opt.preassign = util_str_icmp(v, "try") != 0 ? FIT_PREASSIGN_IGNORE : FIT_PREASSIGN_TRY;
        flatten_free_ref((void **)&v);
    }

    v = param_get(params, "optimize");
    if (v != NULL && util_str_icmp(v, "off") == 0)
        g_opt.optimize = 0;
    free(v);

    v = param_get(params, "Latch_Synthesis");
    if (v != NULL && util_str_icmp(v, "ON") == 0)
        g_opt.latch_synthesis = 1;
    free(v);

    if (FIT_EXT) {
        main_tech_options(params);
        return;
    }
    v = param_get(params, "device");
    if (v == NULL)
        v = param_get(params, "dev");
    if (v == NULL) {
        if (g_dev_package == NULL) {
            g_dev_package = util_strdup(util_str_icmp(g_family, "1508") == 0 ? "PQFP100" : "PLCC44");
            main_device_package();
        } else {
            main_device_package();
        }
    } else {
        g_dev_package = v;
        main_device_package();
    }
    g_opt.device_package = g_dev_package;
}

/* "OFF" | "ON" | "a,b,c": the extended mode's flag-and-list options; returns 1 and
 * sets *list (NULL for ON) unless the value is absent or OFF */
static int main_flag_list(FitList *params, const char *name, FitList **list)
{
    char *v = param_get(params, name);
    int on = v != NULL && util_str_icmp(v, "OFF") != 0;

    if (on) {
        if (util_str_icmp(v, "ON") == 0)
            *list = NULL;
        else
            main_split_list(list, v);
    }
    free(v);
    return on;
}

/* Extended mode: an option that only BE devices know; warns otherwise, or when
 * one of the conflicting options is set already */
static void main_be_only(FitList *params, const char *name, int *flag, int other1, int other2, const char *conflict)
{
    char *v = param_get(params, name);

    if (v != NULL && util_str_icmp(v, "OFF") != 0) {
        if (g_opt.be == 0)
            fprintf(g_msg, "Warning : %s parameter only valid for BE devices\n", name);
        else if (other1 != 0 || other2 != 0)
            fprintf(g_msg, "Warning : %s parameter cannot be set while %s is also set\n", name, conflict);
        else
            *flag = 1;
    }
    free(v);
}

/* "3.3" 3, "2.5" 2, "1.8" 1, anything else 0 (1.5 V) */
static void main_voltage_level(FitList *params, const char *name, int *level)
{
    char *v = param_get(params, name);

    if (v != NULL) {
        if (util_str_icmp(v, "3.3") == 0)
            *level = 3;
        else if (util_str_icmp(v, "2.5") == 0)
            *level = 2;
        else
            *level = util_str_icmp(v, "1.8") == 0;
    }
    free(v);
}

/* Extended mode: a pin may not be schmitt triggered and an SSTL input */
static void main_check_schmitt_sstl(void)
{
    if (g_opt.schmitt_trigger == 0 || g_opt.sstl_input == 0)
        return;
    if (g_opt.schmitt_list == NULL && g_opt.sstl_list == NULL) {
        fprintf(g_msg, "Warning: schmitt trigger and SSTL input cannot be both enabled.\n");
        fprintf(g_msg, "SSTL Input will be disabled\n.");
        g_opt.sstl_input = 0;
        return;
    }
    for (int i = 0; i < (g_opt.schmitt_list == NULL ? 0 : g_opt.schmitt_list->count); i++) {
        char *name = g_opt.schmitt_list->items[i];
        int pos = list_find(g_opt.sstl_list, name, str_ieq_cb);

        if (pos != 0) {
            fprintf(g_msg, "Warning: Pin name: %s cannot be schmitt triggered and SSTL input.\n", name);
            fprintf(g_msg, "Only schmitt trigger is enabled.\n");
            list_del_shift(g_opt.sstl_list, pos);
        }
    }
}

/* Extended mode: the pin keeper wins over pull up and pull down, pull down over
 * pull up */
static void main_check_pin_pulls(void)
{
    if (g_opt.pin_keep != 0 && g_opt.pin_keep_list == NULL) {
        for (int i = 0; i < (g_opt.pull_down_list == NULL ? 0 : g_opt.pull_down_list->count); i++) {
            fprintf(g_msg, "Warning: Pin name: %s cannot be pulled down and to pin keeper at the same time.\n",
                    (char *)g_opt.pull_down_list->items[i]);
            fprintf(g_msg, "Only pin keeper is enabled.\n");
        }
        g_opt.pull_down_list = list_create();
        for (int i = 0; i < (g_opt.pull_up_list == NULL ? 0 : g_opt.pull_up_list->count); i++) {
            fprintf(g_msg, "Warning: Pin name: %s cannot be pulled up and to pin keeper at the same time.\n",
                    (char *)g_opt.pull_up_list->items[i]);
            fprintf(g_msg, "Only pin keeper is enabled.\n");
        }
        g_opt.pull_up_list = list_create();
    }
    for (int i = 0; i < (g_opt.pin_keep_list == NULL ? 0 : g_opt.pin_keep_list->count); i++) {
        char *name = g_opt.pin_keep_list->items[i];
        int pos = list_find(g_opt.pull_down_list, name, str_ieq_cb);

        if (pos != 0) {
            fprintf(g_msg, "Warning: Pin name: %s cannot be pulled down and set to pin keeper at the same time.\n", name);
            fprintf(g_msg, "Only pin keeper is enabled.\n");
            list_del_shift(g_opt.pull_down_list, pos);
        }
        pos = list_find(g_opt.pull_up_list, name, str_ieq_cb);
        if (pos != 0) {
            fprintf(g_msg, "Warning: Pin name: %s cannot be pulled up and set to pin keeper at the same time.\n", name);
            fprintf(g_msg, "Only pin keeper is enabled.\n");
            list_del_shift(g_opt.pull_up_list, pos);
        }
    }
    for (int i = 0; i < (g_opt.pull_down_list == NULL ? 0 : g_opt.pull_down_list->count); i++) {
        char *name = g_opt.pull_down_list->items[i];
        int pos = list_find(g_opt.pull_up_list, name, str_ieq_cb);

        if (pos != 0) {
            fprintf(g_msg, "Warning: Pin name: %s cannot be pulled down and up at the same time.\n", name);
            fprintf(g_msg, "Only pull down is enabled.\n");
            list_del_shift(g_opt.pull_up_list, pos);
        }
    }
}

/* the device options of the command line, the extended mode */
static void main_device_options_ext(FitList *params)
{
    char *v;

    v = param_get(params, "security");
    if (v != NULL && util_str_icmp(v, "ON") == 0)
        g_opt.security = 1;
    free(v);
    if (main_flag_list(params, "pin_keep", &g_opt.pin_keep_list))
        g_opt.pin_keep = 1;
    v = param_get(params, "jtag");
    if (v != NULL && util_str_icmp(v, "OFF") == 0)
        g_opt.jtag = 0;
    free(v);
    v = param_get(params, "power_reset");
    if (v != NULL && util_str_icmp(v, "ON") == 0)
        g_opt.power_reset = 1;
    free(v);
    v = param_get(params, "pd1");
    if (v != NULL && util_str_icmp(v, "ON") == 0)
        g_opt.pd1 = 1;
    free(v);
    v = param_get(params, "pd2");
    if (v != NULL && util_str_icmp(v, "ON") == 0)
        g_opt.pd2 = 1;
    free(v);
    v = param_get(params, "TDI_pullup");
    if (v != NULL && util_str_icmp(v, "ON") == 0)
        g_opt.tdi_pullup = 1;
    free(v);
    v = param_get(params, "TMS_pullup");
    if (v != NULL && util_str_icmp(v, "ON") == 0)
        g_opt.tms_pullup = 1;
    free(v);
    v = param_get(params, "POR");
    if (v != NULL && util_str_icmp(v, "one") == 0)
        g_opt.por = 1;
    free(v);

    /* GCLKn of the extended mode are GCK0..GCK2; BE devices have no input transition
     * detection */
    v = g_opt.be == 0 ? param_get(params, "GCLK_ITD") : NULL;
    if (v != NULL) {
        if (util_str_icmp(v, "OFF") == 0) {
            g_opt.gck_itd[0] = g_opt.gck_itd[1] = g_opt.gck_itd[2] = 0;
        } else if (util_str_icmp(v, "GCLK1") == 0) {
            g_opt.gck_itd[2] = 0;
            g_opt.gck_itd[1] = 0;
        } else if (util_str_icmp(v, "GCLK2") == 0) {
            g_opt.gck_itd[2] = 0;
            g_opt.gck_itd[0] = 0;
        } else if (util_str_icmp(v, "GCLK3") == 0) {
            g_opt.gck_itd[0] = 0;
            g_opt.gck_itd[1] = 0;
        }
    }
    free(v);

    /* output_fast is on by default */
    v = param_get(params, "output_fast");
    if (v != NULL && util_str_icmp(v, "OFF") == 0)
        g_opt.output_fast = 0;
    else if (v == NULL || util_str_icmp(v, "ON") == 0)
        g_opt.output_fast_list = NULL;
    else
        main_split_list(&g_opt.output_fast_list, v);
    free(v);
    if (main_flag_list(params, "mc_power", &g_opt.mc_power_list))
        g_opt.mc_power = 1;
    if (main_flag_list(params, "open_collector", &g_opt.open_collector_list))
        g_opt.open_collector = 1;
    if (main_flag_list(params, "schmitt_trigger", &g_opt.schmitt_list))
        g_opt.schmitt_trigger = 1;
    v = param_get(params, "pull_up");
    if (v != NULL && util_str_icmp(v, "OFF") != 0)
        main_split_list(&g_opt.pull_up_list, v);
    free(v);
    v = param_get(params, "pull_down");
    if (v != NULL && util_str_icmp(v, "OFF") != 0)
        main_split_list(&g_opt.pull_down_list, v);
    free(v);
    v = param_get(params, "no_tff");
    if (v != NULL && util_str_icmp(v, "OFF") != 0) {
        if (g_opt.be == 0)
            fprintf(g_msg, "Warning : no_tff parameter only valid for BE devices\n");
        else
            g_opt.tff_ok = 0;
    }
    free(v);
    main_be_only(params, "pull_up_unused", &g_opt.pull_up_unused, g_opt.unused_to_ground, g_opt.unused_to_pinkeeper,
                 "unused_To_Ground/unused_To_PinKeeper");
    main_be_only(params, "unused_To_Ground", &g_opt.unused_to_ground, g_opt.pull_up_unused, g_opt.unused_to_pinkeeper,
                 "pull_up_unused/unused_To_PinKeeper");
    main_be_only(params, "unused_To_PinKeeper", &g_opt.unused_to_pinkeeper, g_opt.pull_up_unused,
                 g_opt.unused_to_ground, "pull_up_unused/unused_To_Ground");

    g_opt.ues = param_get(params, "ues");

    /* tPD -> speed grade code: AS 7 (25 ns), 6 (20), 5 (15), 4 (10), 3
     * (7); ASV 9 (20), 8 (other); BE 1 (7), 0 (5) */
    v = param_get(params, "tPD");
    if (v != NULL) {
        g_opt.tpd_text = v;
        g_opt.tpd = atoi(v);
        if (g_opt.asv != 0) {
            g_opt.tpd = (g_opt.tpd == 20) + 8;
        } else if (g_opt.be != 0) {
            if (g_opt.tpd == 7) {
                g_opt.tpd = 1;
            } else {
                if (g_opt.tpd != 5) {
                    fprintf(g_msg, "Warning : BE devices only support tpd -5 or -7\n");
                    fprintf(g_msg, "Warning : tpd value set to: -5\n");
                    fprintf(g_fit_report, "Warning : BE devices only support tpd -5 or -7, tpd value set to: -5 \n");
                }
                g_opt.tpd = 0;
            }
        } else {
            if (g_opt.tpd == 25)
                g_opt.tpd = 7;
            else if (g_opt.tpd == 20)
                g_opt.tpd = 6;
            else if (g_opt.tpd == 15)
                g_opt.tpd = 5;
            else
                g_opt.tpd = (g_opt.tpd == 10) + 3;
        }
    }
    if (g_opt.be != 0) {
        main_voltage_level(params, "voltage_level_A", &g_opt.voltage_a);
        main_voltage_level(params, "voltage_level_B", &g_opt.voltage_b);
    }
    v = param_get(params, "SSTL_input");
    if (v != NULL && util_str_icmp(v, "OFF") != 0) {
        if (g_opt.be == 0 || util_str_icmp(g_family, "1502") == 0) {
            fprintf(g_msg, "Warning : SSTL_input parameter only valid for 1504/08BE devices\n");
        } else if (g_opt.voltage_a > 1 || g_opt.voltage_b > 1) {
            g_opt.sstl_input = 1;
            if (util_str_icmp(v, "ON") != 0)
                main_split_list(&g_opt.sstl_list, v);
        } else {
            fprintf(g_msg, "Warning : SSTL_input parameter only supported for voltage levels 2.5 or 3.3\n");
        }
    }
    free(v);
    main_check_schmitt_sstl();
    main_check_pin_pulls();

    v = param_get(params, "Vhdl_sim");
    if (v == NULL)
        v = param_get(params, "Out_sim");
    if (v != NULL)
        g_opt.vhdl_sim = 2 - (util_str_icmp(v, "VHDL") != 0);
    free(v);
    v = param_get(params, "Verilog_Sim");
    if (v != NULL)
        g_opt.verilog_sim = 2 - (util_str_icmp(v, "Verilog") != 0);
    free(v);
    v = param_get(params, "out_edif");
    if (v != NULL && util_str_icmp(v, "on") == 0)
        g_opt.out_edif = 1;
    free(v);

    g_jedec_file = param_get(params, "jedec_file");
    g_vector_file = param_get(params, "vector_file");
    g_verilog_file = param_get(params, "verilog_file");
    g_time_file = param_get(params, "time_file");
}

/* the device options of the command line */
static void main_device_options(FitList *params)
{
    char *v;

    if (FIT_EXT) {
        main_device_options_ext(params);
        return;
    }
    v = param_get(params, "security");
    if (v != NULL && util_str_icmp(v, "ON") == 0)
        g_opt.security = 1;
    free(v);
    v = param_get(params, "pin_keep");
    if (v != NULL && util_str_icmp(v, "OFF") == 0)
        g_opt.pin_keep = 0;
    free(v);
    v = param_get(params, "jtag");
    if (v != NULL && util_str_icmp(v, "OFF") == 0)
        g_opt.jtag = 0;
    free(v);
    v = param_get(params, "power_reset");
    if (v != NULL && util_str_icmp(v, "ON") == 0)
        g_opt.power_reset = 1;
    free(v);
    v = param_get(params, "pd1");
    if (v != NULL && util_str_icmp(v, "ON") == 0)
        g_opt.pd1 = 1;
    free(v);
    v = param_get(params, "pd2");
    if (v != NULL && util_str_icmp(v, "ON") == 0)
        g_opt.pd2 = 1;
    free(v);
    v = param_get(params, "TDI_pullup");
    if (v != NULL && util_str_icmp(v, "ON") == 0)
        g_opt.tdi_pullup = 1;
    free(v);
    v = param_get(params, "TMS_pullup");
    if (v != NULL && util_str_icmp(v, "ON") == 0)
        g_opt.tms_pullup = 1;
    free(v);

    v = param_get(params, "GCK_ITD");
    if (v != NULL) {
        if (util_str_icmp(v, "OFF") == 0) {
            g_opt.gck_itd[2] = 0;
            g_opt.gck_itd[1] = 0;
            g_opt.gck_itd[0] = 0;
        } else if (util_str_icmp(v, "GCK0") == 0) {
            g_opt.gck_itd[2] = 0;
            g_opt.gck_itd[1] = 0;
        } else if (util_str_icmp(v, "GCK1") == 0) {
            g_opt.gck_itd[2] = 0;
            g_opt.gck_itd[0] = 0;
        } else if (util_str_icmp(v, "GCK2") == 0) {
            g_opt.gck_itd[0] = 0;
            g_opt.gck_itd[1] = 0;
        }
    }
    free(v);

    v = param_get(params, "output_fast");
    if (v != NULL && util_str_icmp(v, "OFF") != 0) {
        g_opt.output_fast = 1;
        if (util_str_icmp(v, "ON") == 0)
            g_opt.output_fast_list = NULL;
        else
            main_split_list(&g_opt.output_fast_list, v);
    }
    free(v);
    v = param_get(params, "mc_power");
    if (v != NULL && util_str_icmp(v, "OFF") != 0) {
        g_opt.mc_power = 1;
        if (util_str_icmp(v, "ON") == 0)
            g_opt.mc_power_list = NULL;
        else
            main_split_list(&g_opt.mc_power_list, v);
    }
    free(v);
    v = param_get(params, "open_collector");
    if (v != NULL && util_str_icmp(v, "OFF") != 0) {
        g_opt.open_collector = 1;
        if (util_str_icmp(v, "ON") == 0)
            g_opt.open_collector_list = NULL;
        else
            main_split_list(&g_opt.open_collector_list, v);
    }
    free(v);

    g_opt.ues = param_get(params, "ues");

    v = param_get(params, "tPD");
    if (v != NULL) {
        int tpd = atoi(v);

        if (tpd == 0x19)
            g_opt.tpd = 4;
        else if (tpd == 0x14)
            g_opt.tpd = 3;
        else if (tpd == 0xf)
            g_opt.tpd = 2;
        else
            g_opt.tpd = tpd == 10;
    }
    free(v);

    v = param_get(params, "Vhdl_sim");
    if (v == NULL)
        v = param_get(params, "Out_sim");
    if (v != NULL)
        g_opt.vhdl_sim = 2 - (util_str_icmp(v, "VHDL") != 0);
    free(v);
    v = param_get(params, "Verilog_Sim");
    if (v != NULL)
        g_opt.verilog_sim = 2 - (util_str_icmp(v, "Verilog") != 0);
    free(v);
    v = param_get(params, "out_edif");
    if (v != NULL && util_str_icmp(v, "on") == 0)
        g_opt.out_edif = 1;
    free(v);

    g_jedec_file = param_get(params, "jedec_file");
    g_vector_file = param_get(params, "vector_file");
    g_verilog_file = param_get(params, "verilog_file");
    g_time_file = param_get(params, "time_file");
}

/* -logic_doubling, -twoclock */
static void main_logic_options(FitList *params)
{
    char *v;

    v = param_get(params, "logic_doubling");
    if (v != NULL && util_str_icmp(v, "ON") == 0)
        g_opt.logic_doubling = 1;
    flatten_free_ref((void **)&v);
    g_opt.twoclock = list_create();
    v = param_get(params, "twoclock");
    if (v != NULL)
        list_add(g_opt.twoclock, v);
}

/* a message that ends the job; the extended mode writes it to the report as well */
static void main_job_error(const char *text)
{
    fputs(text, g_msg);
    if (FIT_EXT && g_fit_report != NULL)
        fputs(text, g_fit_report);
}

/* the whole run.  Returns nonzero when the design fits. */
static int main_fit_job(FitList *params)
{
    FitList *sub;
    FitEntity *ent;
    void *object;
    char *v, *base;
    int type, slot;

    g_dev_package = NULL;
    g_tech_name = NULL;
    v = param_get(params, "cupl");
    if (v != NULL)
        g_opt.cupl = 1;
    free(v);
    v = param_get(params, "ifmt");
    if (util_str_icmp(v, "edif") == 0)
        g_opt.ifmt_edif = 1;
    free(v);
    g_opt.cascade_list = NULL;
    g_opt.xor_list = NULL;
    g_opt.soft_buffer = NULL;
    g_opt.foldback_list = NULL;
    g_opt.global_fold = NULL;
    g_opt.global_oe = NULL;
    g_opt.fast_inlatch_list = NULL;
    g_opt.open_collector_list = NULL;
    g_opt.mc_power_list = NULL;
    g_opt.output_fast_list = NULL;
    g_opt.pin_keep_list = NULL;
    g_opt.pull_down_list = NULL;
    g_opt.pull_up_list = NULL;
    g_opt.schmitt_list = NULL;
    if (main_check_params(params) == 0) {
        g_exit_code = FIT_EXIT_USAGE;
        return 0;
    }

    /* output base name: -o, else the input name; both without extension */
    v = param_get(params, "o");
    if (v == NULL)
        v = param_get(params, "i");
    base = main_strip_extension(v);
    g_opt.out_base = util_strdup(base);
    report_open(base);
    v = param_get(params, "i");
    g_in_name = main_input_name(v);
    g_in_base = main_strip_extension(g_in_name);
    free(param_get(params, "crit"));    /* fetched and dropped */

    v = param_get(params, "DEBUG");
    if (util_str_icmp(v, "ON") == 0) {
        g_opt.debug = 1;
        fprintf(g_msg, "Mode debug ... \n");
    }
    flatten_free_ref((void **)&v);
    /* the extended mode reads the PROPERTY lines of the design into the parameters,
     * so these two are taken after the design */
    if (!FIT_EXT)
        main_logic_options(params);
    g_preassign = list_create();

    /* read the design; it is registered as entity "Fct" */
    sub = list_create_sized(7);
    param_copy_into(params, sub, "i");
    param_copy_into(params, sub, "ifmt");
    param_copy_into(params, sub, "lib");
    param_copy_into(params, sub, "c");
    param_copy_into(params, sub, "ff");
    param_copy_into(params, sub, "ffsense");
    list_add(sub, param_new("d", "Fct"));
    if (input_read(sub, params) == 0) {
        main_job_error("Reading input file failed ...\n");
        if (g_exit_code == 0)
            g_exit_code = FIT_EXIT_INPUT_FAILED;
        return 0;
    }
    list_free_all(&sub, util_free_ptr);
    if (FIT_EXT)
        main_logic_options(params);

    v = param_get(params, "pinfile");
    if (v != NULL) {
        g_opt.pinfile = 1;
        if (g_opt.pin_filename == NULL)
            not_supported("-pinfile");
    }
    free(v);

    /* options, then the gate library and the mapping onto it */
    sub = list_create_sized(4);
    list_add(sub, param_new("tech", "atmel.std"));
    list_add(sub, param_new("s", "Fct"));
    list_add(sub, param_new("tsbuf", "ON"));
    main_fit_options(params);
    main_device_options(params);
    if (main_mark_kept_signals() == 0)
        return 0;
    v = tmp_log_name();
    msg_redirect_to(v);
    flatten_free_ref((void **)&v);
    if (lib_load_library(sub) == 0) {
        msg_restore();
        main_job_error("Checking Extra wiring failed ...\n");
        return 0;
    }
    msg_restore();
    list_free_all(&sub, util_free_ptr);

    if (entity_table_get("Fct", FIT_ENT_ENTITY, &type, &object, &slot) == 0) {
        main_job_error("Not possible to read an entity: ENTITY_IS_FCT.\n");
        g_exit_code = FIT_EXIT_NO_ENTITY;
        return 0;
    }
    ent = object;
    main_rename_register_nodes(ent);
    if (main_has_unknown_gate(ent) != 0) {
        main_job_error("UNKNOWN_GATE not supported.\n");
        return 0;
    }
    latch_fixup_entity(ent);
    return fit_driver(ent, base, g_opt.preassign, g_opt.latch_synthesis) != 0;
}

/* run the job once.  Fatal errors and signals longjmp back to
 * the setjmp here; the second return ends the function at once. */
static void main_run_job(const char *command)
{
    static FitList *params;     /* static: its value must survive the longjmp */
    int entered;
    char *silent;
    int r;

    setjmp(g_job_jmp);
    entered = g_job_entered;
    g_job_entered++;
    if (entered != 0)
        return;
    params = NULL;
    util_job_init();
    r = main_parse_command(command, &params);
    if (r == 0)
        g_exit_code = FIT_EXIT_USAGE;
    util_timer_update();
    silent = param_get(params, "silent");
    if (silent != NULL) {
        char *log = tmp_log_name();

        free(silent);
        msg_redirect_to(log);
        flatten_free_ref((void **)&log);
    }
    if (main_fit_job(params) == 0) {
        if (g_exit_code == 0)
            g_exit_code = FIT_EXIT_NO_FIT;
    } else {
        g_exit_code = 0;
    }
    util_timer_update();
    util_timer_print(g_msg);
    fprintf(g_msg, " \n");
    silent = param_get(params, "silent");
    if (silent != NULL) {
        free(silent);
        msg_restore();
        if (g_exit_code == FIT_EXIT_NO_FIT) {
            fprintf(g_msg, "DESIGN FAIL TO FIT\n");
        } else {
            fprintf(g_msg, "DESIGN FITS\n");
            fprintf(g_msg, " Output files: %s.tt3, %s.fit, %s.jed\n", g_opt.out_base, g_opt.out_base, g_opt.out_base);
        }
    }
    list_free_all(&params, util_free_ptr);
}

/* the mode to run in: $LINCUPL_FIT150X_MODE, "extended" for the extended
 * mode; unset, empty or anything else gives the legacy mode */
static void main_select_mode(void)
{
    const char *v = getenv("LINCUPL_FIT150X_MODE");

    g_fit_mode = v != NULL && strcmp(v, "extended") == 0 ? FIT_MODE_EXTENDED : FIT_MODE_LEGACY;
}

/* everything a run starts from */
static void main_reset_state(void)
{
    static const FitOptions initial = {
        .preassign = FIT_PREASSIGN_TRY,
        .optimize = 1,
        .foldback = 1,
        .push_gate = 1,
        .jtag = 1,
        .gck_itd = { 1, 1, 1 },
        .pin_keep = 1
    };
    /* Extended mode: foldback logic and the pin keeper off, fast outputs on */
    static const FitOptions initial_ext = {
        .preassign = FIT_PREASSIGN_TRY,
        .optimize = 1,
        .push_gate = 1,
        .jtag = 1,
        .gck_itd = { 1, 1, 1 },
        .output_fast = 1,
        .voltage_a = 1,
        .voltage_b = 1,
        .tff_ok = 1
    };

    base_util_reset();
    base_list_reset();
    base_lex_reset();
    base_entity_reset();
    lib_reset();
    min_reset();
    fit_drv_reset();
    g_opt = FIT_EXT ? initial_ext : initial;
    g_family = NULL;
    g_version = NULL;
    g_command = NULL;
    g_fitterdir = NULL;
    g_preassign = NULL;
    g_keep_ids = NULL;
    g_depth_visited = NULL;
    g_in_base = NULL;
    g_in_name = NULL;
    g_tech_name = NULL;
    g_jedec_file = NULL;
    g_vector_file = NULL;
    g_verilog_file = NULL;
    g_time_file = NULL;
    g_signal_count = 0;
    g_job_entered = 0;
    g_log_in = NULL;
    g_log_out = NULL;
}

/* the device this run is for: from the program name, else from -dev */
static void main_select_device(int argc, char **argv)
{
    if (argc > 0 && argv[0] != NULL && dev_select_by_name(argv[0]) != 0)
        return;
    for (int i = 1; i + 1 < argc; i++) {
        if ((util_str_icmp(argv[i], "-dev") == 0 || util_str_icmp(argv[i], "-device") == 0) &&
            dev_select_by_name(argv[i + 1]) != 0)
            return;
    }
    dev_select(FIT_DEV_1508);
}

/* the fitter's main: selects the device, prints the banner, builds the
 * command string from argv and runs the job.  Returns the exit code. */
int fit150x_main(int argc, char **argv)
{
    char *command = NULL;

    main_select_mode();
    main_reset_state();
    main_select_device(argc, argv);
    g_open_files = list_create();
    g_msg = stdout;
    main_init(g_log_in, g_log_out);
    (void)main_parse_debug_level(argc, argv);
    fit_prep_nop();     /* called with the debug level, which it ignores */
    main_print_banner();
    main_install_signals(1);
    if (argc >= 3 && main_find_c_option(argc, argv, "-c") != 0)
        not_supported("-c file");
    if (main_build_command(argc, argv, &command) == 0)
        return FIT_EXIT_USAGE;
    g_command = util_strdup(command);
    main_run_job(command);
    tmp_cleanup();
    if (g_log_in != NULL)
        util_fclose(g_log_in);
    if (g_log_out != NULL)
        util_fclose(g_log_out);
    util_final_cleanup();
    fflush(stdout);
    return g_exit_code;
}

int main(int argc, char **argv)
{
    lincupl_init_file_mode();
    return fit150x_main(argc, argv);
}
