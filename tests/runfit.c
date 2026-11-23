#include "runfit.h"
#include "locate.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { \
    if (!(expr)) { fprintf(stderr, "failed: %s\n", #expr); return 1; } \
} while (0)

int main(int argc, char **argv)
{
    const char *inherited = getenv("FITTERDIR");
    const char *directory = inherited ? inherited : "support directory";
    if (argc > 1) {
        CHECK(argc == 8);
        CHECK(strcmp(argv[1], "child") == 0);
        CHECK(strcmp(argv[2], "") == 0);
        CHECK(strcmp(argv[3], "two words") == 0);
        CHECK(strcmp(argv[4], "a\"b") == 0);
        CHECK(strcmp(argv[5], "path with spaces\\") == 0);
        CHECK(strcmp(argv[6], "slash\\\"quote") == 0);
        CHECK(inherited && strcmp(inherited, argv[7]) == 0);
        return 127;
    }

    char program[PATH_MAX];
    CHECK(runfit_find_fitter("fitter test", NULL, program, sizeof(program)) == 1);
    CHECK(runfit_find_in_path("fitter test", program, sizeof(program)) == 1);
    const char *progs[] = {"no-such-lincupl-fitter", program, NULL};
    char *args[] = {program, "child", "", "two words", "a\"b",
                    "path with spaces\\", "slash\\\"quote", (char *)directory, NULL};
    CHECK(runfit_spawn(progs, args, "support directory") == 127);
    progs[1] = "fitter test";
    CHECK(runfit_spawn(progs, args, "support directory") == 127);
    CHECK(inherited ? strcmp(getenv("FITTERDIR"), inherited) == 0
                    : getenv("FITTERDIR") == NULL);
    progs[1] = NULL;
    CHECK(runfit_spawn(progs, args, NULL) == RUNFIT_NOT_STARTED);
    return 0;
}
