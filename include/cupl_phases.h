#ifndef CUPL_PHASES_H
#define CUPL_PHASES_H

#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The cupl command line, "cupl [-flags] [library] [device] file", as
 * cupl_parse_options leaves it for cupl_driver_run. */
struct cupl_options {
    char flags[64];             /* "&" and the option letters for the phases */
    int run_fitter;             /* -j, -zq or -zx: run the fitter the device names */
    int run_simulator;          /* -s or -w */
    int flag_j;                 /* -j, -n, -w: also passed to the simulator */
    int flag_n;
    int flag_w;
    char library[PATH_MAX];     /* device library */
    char device[128];           /* lower case; "" if none was given */
    char source[PATH_MAX];      /* design file */
};

enum {
    CUPL_OPTIONS_OK = 0,
    CUPL_OPTIONS_USAGE,         /* unknown flag or missing operand */
    CUPL_OPTIONS_NO_LIBRARY     /* no device library found */
};

/* The csim command line, "csim [-flags] [library] [device] file", as
 * csim_parse_options leaves it for csim_driver_run. */
struct csim_options {
    char flags[8];              /* "&" and the option letters for the simulator */
    char library[PATH_MAX];
    char device[128];           /* lower case; "" if none was given */
    char source[PATH_MAX];
};

enum {
    CSIM_OPTIONS_OK = 0,
    CSIM_OPTIONS_USAGE,         /* no arguments or too many operands */
    CSIM_OPTIONS_NO_LIBRARY,    /* no device library found */
    CSIM_OPTIONS_NO_SOURCE      /* no design file given */
};

/* Parse the command line of cupl / csim (argv[0] is the program) and find
 * the device library.  csim_parse_options prints "Unknown flag -x" for
 * flags it ignores.  Returns one of the *_OPTIONS_* codes. */
int cupl_parse_options(int argc, char **argv, struct cupl_options *opt);
int csim_parse_options(int argc, char **argv, struct csim_options *opt);

/* Run the compiler phases / the simulator.  The result is 0 on success. */
int cupl_driver_run(const struct cupl_options *opt);
int csim_driver_run(const struct csim_options *opt);

/* Entry points of the compiler phases and the simulators.  argv is the
 * argument vector the driver builds for each phase (phase name, work
 * directory, library, id, device, source file, flags, ...).  The result is
 * 0 on success. */
int preprocess_run(int argc, char **argv);
int parse_run(int argc, char **argv);
int expand_run(int argc, char **argv);
int minimize_run(int argc, char **argv);
int fusemap_run(int argc, char **argv);

/* The simulator; vsima 0 simulates the fuse map (csima), 1 the equations
 * of a virtual or fitted device (vsima). */
int sim_run(int vsima, int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* CUPL_PHASES_H */
