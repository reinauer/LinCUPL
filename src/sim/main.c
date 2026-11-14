/*
 * csim - command line front end of the CUPL simulator.
 *
 * Reads "csim -flags [library] [device] filename" (csim_parse_options),
 * prints the usage or the "no library" message when it is not usable and
 * otherwise runs the simulator driver (csim_driver_run).
 */
#include "cupl_phases.h"

#include <stdio.h>
#include <stdlib.h>

static void csim_usage(void) {
    printf("Usage: csim -flags <library.dl> filename\n");
    printf("Where -flags:\n");
    printf("  -j Attach test vectors to jedec file\n");
    printf("  -n download filename=PLD filename\n");
    printf("  -l listing file (generate error listing in .so file)\n");
    printf("  -w simulate and generate waveform (.wo and .so files)\n");
    printf("  -u Use specified library (overrides Environment Variable)\n");
    exit(0);
}

int main(int argc, char **argv) {
    static struct csim_options opt;

    switch (csim_parse_options(argc, argv, &opt)) {
    case CSIM_OPTIONS_USAGE:
        csim_usage();
        break;
    case CSIM_OPTIONS_NO_LIBRARY:
        printf("            Unable to read environment variable: LIBCUPL  \n");
        printf("  Please set this variable to point to the directory where the CUPL library \n");
        printf("  is installed or specify the library with the -u flag. \n");
        printf("         (e.g. csim -u %s/cupl.dl design). \n", LINCUPL_DATADIR);
        return 1;
    case CSIM_OPTIONS_NO_SOURCE:
        printf("Missing input file name\n\n");
        csim_usage();
        break;
    default:
        break;
    }
    return csim_driver_run(&opt);
}
