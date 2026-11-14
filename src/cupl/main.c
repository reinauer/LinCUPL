/*
 * main.c - cupl
 *
 * Reads the command line (cupl_parse_options), prints the usage or the
 * "no library" message when it is not usable and otherwise runs the
 * compiler driver (cupl_driver_run).
 */

#include "banner.h"
#include "cupl_phases.h"

#include <stdio.h>

/* First lines of the usage and "no library" messages; the build date is
 * when this file was compiled. */
static void cupl_banner(void) {
    printf("\n%s %s cupl: Universal Compiler for Programmable Logic\n", PROGRAM_NAME, PROGRAM_VERSION);
    printf("%s\n", PROGRAM_COPYRIGHT);
    printf("Created %s %s\n\n", __DATE__, __TIME__);
}

static void cupl_usage(void) {
    cupl_banner();
    printf("USAGE: cupl [-flags] [library] [device] file\n");
    printf("FLAGS:\n");
    printf(" -a create absolute file               -p create PDIF file\n");
    printf(" -b create Berkeley PLA file           -qsubtle warnings OFF\n");
    printf(" -c create central fuse numbers        -r Do NOT expand Macro Products\n");
    printf(" -d append fuses in doc file           -sWINSIM follows CUPL\n");
    printf(" -e create expanded macro definition   -t append truth table in doc file\n");
    printf(" -f create Function file               -u use specified library\n");
    printf(" -g create Gates file                  -w simulate & generate waveform (.wo)\n");
    printf(" -h create ALL download files          -x create extended listing in doc file\n");
    printf(" -i create IFL file                    -zDeactivate unused global OR gates\n");
    printf(" -j create JEDEC file (default)     \n");
    printf(" -jx create JEDEC file with expanded listing\n");
    printf(" -kb Optimize product term usage (overrides the DEMORGAN statement in file)\n");
    printf(" -kd Demorganize all pins and pinnodes (overrides the DEMORGAN statement)\n");
    printf(" -ks Force product term sharing (enable group reduction)\n");
    printf(" -kx Do not expand XOR equations (virtual or fitter that supports XOR gates)\n");
    printf(" -l create listing file                \n");
    printf(" -m0 NO minimization                   -n use the Logic file Name for the Output\n");
    printf(" -m1 Quick minimization(default)       -o ONE Hot state machine Bit Encoding\n");
    printf(" -m2 Quine-McCluskey \n");
    printf(" -m3 Presto          \n");
    printf(" -m4 Espresso        \n");
    printf("Normal exit\n");
}

int main(int argc, char **argv) {
    static struct cupl_options opt;

    switch (cupl_parse_options(argc, argv, &opt)) {
    case CUPL_OPTIONS_USAGE:
        cupl_usage();
        return 1;
    case CUPL_OPTIONS_NO_LIBRARY:
        cupl_banner();
        printf("Could not find LIBCUPL in ENVIRONMENT\n");
        printf("Normal exit\n");
        return 1;
    default:
        break;
    }

    int rc = cupl_driver_run(&opt);
    printf("\nNormal exit\n");
    printf("Error Code = %d\n\n", rc);
    return rc;
}
