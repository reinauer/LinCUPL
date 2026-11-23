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

#include "platform.h"
#include "runfit.h"
#include "locate.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#endif

static int executable_file(const char *path)
{
#ifdef _WIN32
    return lincupl_is_reg_file(path);
#else
    return access(path, X_OK) == 0 && !lincupl_is_dir(path);
#endif
}

int runfit_find_in_path(const char *name, char *out, size_t out_sz)
{
    const char *path = getenv("PATH");
    while (path != NULL && *path != '\0') {
        const char *end = strchr(path, LINCUPL_PATH_SEPARATOR);
        size_t len = end ? (size_t)(end - path) : strlen(path);
        if (len > 0 && len + strlen(name) + sizeof(LINCUPL_EXE_SUFFIX) + 1 < out_sz) {
            snprintf(out, out_sz, "%.*s/%s%s", (int)len, path, name, LINCUPL_EXE_SUFFIX);
            if (executable_file(out)) {
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
        snprintf(cand, sizeof(cand), "%s/%s%s", dir, name, LINCUPL_EXE_SUFFIX);
        if (executable_file(cand)) {
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
#ifdef _WIN32
                return 1;
#else
                return 2;
#endif
            }
        }
    }
    return 0;
}

#ifdef _WIN32
/* Quote each argument using the Windows C runtime's backslash/quote rules. */
static char *command_line(char *const argv[])
{
    size_t capacity = 1;
    for (size_t i = 0; argv[i]; i++) {
        size_t n = strlen(argv[i]);
        if (n > 32767 || capacity + 2 * n + 3 > 32767)
            return NULL;
        capacity += 2 * n + 3;
    }
    char *line = malloc(capacity);
    if (!line) return NULL;
    char *out = line;
    for (size_t i = 0; argv[i]; i++) {
        const char *p = argv[i];
        if (i) *out++ = ' ';
        *out++ = '"';
        for (;;) {
            size_t slashes = 0;
            while (*p == '\\') { slashes++; p++; }
            if (*p == '"' || *p == '\0') slashes *= 2;
            while (slashes--) *out++ = '\\';
            if (*p == '\0') break;
            if (*p == '"') *out++ = '\\';
            *out++ = *p++;
        }
        *out++ = '"';
    }
    *out = '\0';
    return line;
}

/* Copy the inherited environment, inserting FITTERDIR in name order. */
static char *fitter_environment(const char *directory)
{
    char *inherited = GetEnvironmentStringsA();
    if (!inherited) return NULL;
    size_t size = strlen(directory) + sizeof("FITTERDIR=") + 1;
    for (const char *p = inherited; *p; p += strlen(p) + 1)
        size += strlen(p) + 1;
    char *env = malloc(size);
    if (env) {
        char *out = env;
        int inserted = 0;
        for (const char *p = inherited; *p; p += strlen(p) + 1) {
            if (!inserted && strcasecmp(p, "FITTERDIR=") > 0) {
                out += sprintf(out, "FITTERDIR=%s", directory) + 1;
                inserted = 1;
            }
            size_t n = strlen(p) + 1;
            memcpy(out, p, n);
            out += n;
        }
        if (!inserted) out += sprintf(out, "FITTERDIR=%s", directory) + 1;
        *out = '\0';
    }
    FreeEnvironmentStringsA(inherited);
    return env;
}

int runfit_spawn(const char *const *progs, char *const argv[], const char *fitter_dir)
{
    char *env = NULL;
    int result = RUNFIT_NOT_STARTED;
    if (fitter_dir && !getenv("FITTERDIR")) {
        env = fitter_environment(fitter_dir);
        if (!env) return result;
    }
    for (size_t i = 0; progs[i]; i++) {
        STARTUPINFOA startup = {0};
        PROCESS_INFORMATION process;
        char program[PATH_MAX];
        DWORD length = SearchPathA(NULL, progs[i], ".exe",
                                   sizeof(program), program, NULL);
        if (!length || length >= sizeof(program)) continue;
        char *line = command_line(argv);
        if (!line) break;
        startup.cb = sizeof(startup);
        BOOL started = CreateProcessA(program, line, NULL, NULL, TRUE,
                                      0, env, NULL, &startup, &process);
        free(line);
        if (!started) continue;
        DWORD status;
        if (WaitForSingleObject(process.hProcess, INFINITE) != WAIT_OBJECT_0 ||
            !GetExitCodeProcess(process.hProcess, &status) || status > INT_MAX)
            result = RUNFIT_ABNORMAL;
        else
            result = (int)status;
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        break;
    }
    free(env);
    return result;
}
#else
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

#endif
