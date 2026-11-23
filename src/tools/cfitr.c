/*
 * cfitr - CUPL fitter runner.
 *
 *     cfitr <status-file> <program> [args...]
 *
 * Runs <program> with the given arguments (the program name itself is
 * passed as argv[0]; backslashes in it become slashes), waits for it and
 * writes its exit status as a 4-byte native-endian integer to
 * <status-file>.  The status is -1 if the program could not be started or
 * did not exit normally.  The exit status of cfitr is the same value.
 */

#include "platform.h"
#include "runfit.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Runs prog with params and waits for it.  The name is tried as given
 * (a name without a directory first in the current directory, then in
 * PATH), and the same again without a ".exe" suffix.  Returns the exit
 * status, or -1 if no attempt could be started or the child did not exit
 * normally. */
static int32_t run_child(const char *prog, char **params) {
    char local_path[520];
    char stripped[512];
    char local_stripped[520];
    const char *progs[5];
    int n = 0;

    if (strchr(prog, '/') == NULL) {
        snprintf(local_path, sizeof(local_path), "./%s", prog);
        progs[n++] = local_path;
    }
    progs[n++] = prog;

    size_t len = strlen(prog);
    if (len > 4 && strcasecmp(prog + len - 4, ".exe") == 0 && len - 4 < sizeof(stripped)) {
        memcpy(stripped, prog, len - 4);
        stripped[len - 4] = '\0';
        if (strchr(stripped, '/') == NULL) {
            snprintf(local_stripped, sizeof(local_stripped), "./%s", stripped);
            progs[n++] = local_stripped;
        }
        progs[n++] = stripped;
    }
    progs[n] = NULL;

    int rc = runfit_spawn(progs, params, NULL);
    return rc < 0 ? -1 : (int32_t)rc;
}

int main(int argc, char **argv) {
    lincupl_init_file_mode();
    if (argc < 3) {
        return 1;
    }

    const char *status_file = argv[1];
    char prog[512];
    size_t pi = 0;
    while (argv[2][pi] && pi + 1 < sizeof(prog)) {
        prog[pi] = (argv[2][pi] == '\\') ? '/' : argv[2][pi];
        pi++;
    }
    prog[pi] = '\0';

    char **params = (char **)calloc((size_t)argc, sizeof(char *));
    if (params == NULL) {
        return 1;
    }

    for (int i = 2; i < argc; i++) {
        params[i - 2] = argv[i];
    }
    params[argc - 2] = NULL;

    int32_t ret = run_child(prog, params);

    FILE *out_fp = fopen(status_file, "wb");
    if (out_fp != NULL) {
        fwrite(&ret, 4, 1, out_fp);
        fclose(out_fp);
    }

    free(params);
    return (int)ret;
}
