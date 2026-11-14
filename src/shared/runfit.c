/*
 * runfit.c - finding and starting an external fitter (see runfit.h)
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifdef __APPLE__
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#endif

#include "runfit.h"
#include "locate.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

int runfit_find_in_path(const char *name, char *out, size_t out_sz)
{
    const char *path = getenv("PATH");
    while (path != NULL && *path != '\0') {
        const char *end = strchr(path, ':');
        size_t len = end ? (size_t)(end - path) : strlen(path);
        if (len > 0 && len + strlen(name) + 2 < out_sz) {
            snprintf(out, out_sz, "%.*s/%s", (int)len, path, name);
            if (access(out, X_OK) == 0 && !lincupl_is_dir(out)) {
                return 1;
            }
        }
        path = end ? end + 1 : NULL;
    }
    return 0;
}

int runfit_find_fitter(const char *name, const char *lib_path, char *out, size_t out_sz)
{
    char dir[PATH_MAX];
    char cand[PATH_MAX + 512];

    if (lincupl_get_exe_dir(dir, sizeof(dir))) {
        snprintf(cand, sizeof(cand), "%s/%s", dir, name);
        if (access(cand, X_OK) == 0 && !lincupl_is_dir(cand)) {
            snprintf(out, out_sz, "%s", cand);
            return 1;
        }
    }
    if (runfit_find_in_path(name, out, out_sz)) {
        return 1;
    }

    char lib_dir[PATH_MAX];
    lincupl_normalize_slashes(lib_path ? lib_path : "", lib_dir, sizeof(lib_dir));
    char *slash = strrchr(lib_dir, '/');
    if (slash != NULL) {
        *slash = '\0';
    } else {
        strcpy(lib_dir, ".");
    }
    const char *env_dir = getenv("LINCUPL_FITTERS");
    static const char *const k_ext[] = { ".exe", ".com", NULL };
    for (int where = 0; where < 4; where++) {
        for (int e = 0; k_ext[e] != NULL; e++) {
            switch (where) {
            case 0:
                if (env_dir == NULL || env_dir[0] == '\0') continue;
                snprintf(cand, sizeof(cand), "%s/%s%s", env_dir, name, k_ext[e]);
                break;
            case 1:
                snprintf(cand, sizeof(cand), "%s%s", name, k_ext[e]);
                break;
            case 2:
                snprintf(cand, sizeof(cand), "%s/Fitters/%s%s", lib_dir, name, k_ext[e]);
                break;
            default:
                snprintf(cand, sizeof(cand), "%s/../WinCupl/Fitters/%s%s", lib_dir, name, k_ext[e]);
                break;
            }
            if (lincupl_resolve_ci_path(cand, out, out_sz) == 0 && !lincupl_is_dir(out)) {
                return 2;
            }
        }
    }
    return 0;
}

int runfit_spawn(const char *const *progs, char *const argv[], const char *fitter_dir)
{
    /* the child reports a failed exec through this pipe; a successful exec
     * closes it */
    int err_pipe[2];
    if (pipe(err_pipe) != 0) {
        return RUNFIT_NOT_STARTED;
    }
    fcntl(err_pipe[1], F_SETFD, FD_CLOEXEC);

    pid_t pid = fork();
    if (pid < 0) {
        close(err_pipe[0]);
        close(err_pipe[1]);
        return RUNFIT_NOT_STARTED;
    }
    if (pid == 0) {
        close(err_pipe[0]);
        if (fitter_dir != NULL) {
            setenv("FITTERDIR", fitter_dir, 0);
        }
        for (int i = 0; progs[i] != NULL; i++) {
            execvp(progs[i], argv);
        }
        int err = errno;
        ssize_t wr = write(err_pipe[1], &err, sizeof(err));
        (void)wr;
        close(err_pipe[1]);
        _exit(127);
    }

    close(err_pipe[1]);
    int child_err = 0;
    ssize_t n = read(err_pipe[0], &child_err, sizeof(child_err));
    close(err_pipe[0]);

    int status = 0;
    pid_t done;
    while ((done = waitpid(pid, &status, 0)) < 0 && errno == EINTR) {
    }
    if (n > 0) {
        return RUNFIT_NOT_STARTED;
    }
    if (done < 0 || !WIFEXITED(status)) {
        return RUNFIT_ABNORMAL;
    }
    return WEXITSTATUS(status);
}
