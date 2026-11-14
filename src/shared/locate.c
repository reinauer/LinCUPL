/*
 * locate.c - file names and data files
 *
 * Case-insensitive path lookup, the directory of the running executable and
 * the search for the device library (see locate.h).
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

#include "locate.h"

#include <dirent.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

/* Copy a path, turning every backslash into a slash. */
void lincupl_normalize_slashes(const char *in_path, char *out_path, size_t out_sz) {
    size_t i = 0;
    if (!out_path || out_sz == 0) return;
    if (!in_path) {
        out_path[0] = '\0';
        return;
    }
    while (in_path[i] && i + 1 < out_sz) {
        out_path[i] = (in_path[i] == '\\') ? '/' : in_path[i];
        i++;
    }
    out_path[i] = '\0';
}

int lincupl_is_reg_file(const char *path) {
    struct stat st;
    if (!path || !path[0]) return 0;
    return (stat(path, &st) == 0 && S_ISREG(st.st_mode)) ? 1 : 0;
}

int lincupl_is_dir(const char *path) {
    struct stat st;
    if (!path || !path[0]) return 0;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) ? 1 : 0;
}

/*
 * Resolve `raw_path` case-insensitively component-by-component on POSIX
 * filesystems so designs and data files whose names differ in case from the
 * names used to refer to them (`TTL.M`, `ttl.m`, `FIT1500.M`, `Join2500.si`,
 * `Atmel.dl`) work identically on Linux and macOS.
 * Returns 0 on match (writing matched path to `resolved`), -1 if not found
 * (writing slash-normalized `raw_path` to `resolved`).
 */
int lincupl_resolve_ci_path(const char *raw_path, char *resolved, size_t resolved_sz) {
    char norm[PATH_MAX];
    char cur[PATH_MAX];
    const char *p;

    if (!resolved || resolved_sz == 0) return -1;
    if (!raw_path || !raw_path[0]) {
        resolved[0] = '\0';
        return -1;
    }

    lincupl_normalize_slashes(raw_path, norm, sizeof(norm));
    if (access(norm, F_OK) == 0) {
        snprintf(resolved, resolved_sz, "%s", norm);
        return 0;
    }

    p = norm;
    if (*p == '/') {
        strcpy(cur, "/");
        while (*p == '/') p++;
    } else {
        cur[0] = '\0';
    }

    while (*p) {
        char seg[512];
        char cand[PATH_MAX];
        size_t slen = 0;
        while (p[slen] && p[slen] != '/') slen++;
        if (slen >= sizeof(seg)) {
            snprintf(resolved, resolved_sz, "%s", norm);
            return -1;
        }
        memcpy(seg, p, slen);
        seg[slen] = '\0';
        p += slen;
        while (*p == '/') p++;

        if (cur[0] == '\0') {
            snprintf(cand, sizeof(cand), "%s", seg);
        } else if (strcmp(cur, "/") == 0) {
            snprintf(cand, sizeof(cand), "/%s", seg);
        } else {
            snprintf(cand, sizeof(cand), "%s/%s", cur, seg);
        }

        if (strcmp(seg, ".") == 0 || strcmp(seg, "..") == 0 || access(cand, F_OK) == 0) {
            snprintf(cur, sizeof(cur), "%s", cand);
            continue;
        }

        {
            const char *dir_to_open = cur[0] ? cur : ".";
            DIR *d = opendir(dir_to_open);
            struct dirent *de;
            int found = 0;
            if (!d) {
                snprintf(resolved, resolved_sz, "%s", norm);
                return -1;
            }
            while ((de = readdir(d)) != NULL) {
                if (strcasecmp(de->d_name, seg) == 0) {
                    if (cur[0] == '\0') {
                        snprintf(cur, sizeof(cur), "%s", de->d_name);
                    } else if (strcmp(cur, "/") == 0) {
                        snprintf(cur, sizeof(cur), "/%s", de->d_name);
                    } else {
                        size_t clen = strlen(cur);
                        snprintf(cur + clen, sizeof(cur) - clen, "/%s", de->d_name);
                    }
                    found = 1;
                    break;
                }
            }
            closedir(d);
            if (!found) {
                snprintf(resolved, resolved_sz, "%s", norm);
                return -1;
            }
        }
    }

    if (cur[0] && access(cur, F_OK) == 0) {
        snprintf(resolved, resolved_sz, "%s", cur);
        return 0;
    }
    snprintf(resolved, resolved_sz, "%s", norm);
    return -1;
}

/*
 * Portable fopen wrapper:
 * - Drops the 't' (text) mode letter ("rt" -> "r", "wt" -> "w").
 * - Normalizes backslashes to forward slashes.
 * - Falls back to case-insensitive path resolution when opening existing files.
 * - For write/create modes where parent directories have different casing on
 *   Linux, resolves the parent directory case-insensitively.
 */
FILE *lincupl_fopen_ci(const char *raw_path, const char *raw_mode) {
    char norm[PATH_MAX];
    char mode[16];
    size_t mi = 0;
    FILE *fp;

    if (!raw_path || !raw_mode) return NULL;
    lincupl_normalize_slashes(raw_path, norm, sizeof(norm));
    for (size_t i = 0; raw_mode[i] && mi + 1 < sizeof(mode); i++) {
        if (raw_mode[i] != 't') mode[mi++] = raw_mode[i];
    }
    mode[mi] = '\0';

    fp = fopen(norm, mode);
    if (fp) return fp;

    if (mode[0] == 'r' || mode[0] == 'a' || strchr(mode, '+') != NULL) {
        char resolved[PATH_MAX];
        if (lincupl_resolve_ci_path(norm, resolved, sizeof(resolved)) == 0) {
            fp = fopen(resolved, mode);
            if (fp) return fp;
        }
    }

    if (mode[0] == 'w' || mode[0] == 'a') {
        const char *slash = strrchr(norm, '/');
        if (slash && slash != norm) {
            char parent_raw[PATH_MAX];
            char parent_res[PATH_MAX];
            size_t dlen = (size_t)(slash - norm);
            if (dlen < sizeof(parent_raw)) {
                memcpy(parent_raw, norm, dlen);
                parent_raw[dlen] = '\0';
                if (lincupl_resolve_ci_path(parent_raw, parent_res, sizeof(parent_res)) == 0) {
                    char full_out[PATH_MAX];
                    snprintf(full_out, sizeof(full_out), "%s/%s", parent_res, slash + 1);
                    fp = fopen(full_out, mode);
                }
            }
        }
    }
    return fp;
}

/* Determine directory containing the running executable on macOS and Linux. */
int lincupl_get_exe_dir(char *dir_out, size_t dir_sz) {
    char raw_exe[PATH_MAX];
    char real_exe[PATH_MAX];
    const char *use_path = NULL;
    const char *slash;

    if (!dir_out || dir_sz == 0) return 0;
    dir_out[0] = '\0';

#if defined(__APPLE__)
    {
        uint32_t sz = (uint32_t)sizeof(raw_exe);
        if (_NSGetExecutablePath(raw_exe, &sz) == 0) {
            if (realpath(raw_exe, real_exe) != NULL) {
                use_path = real_exe;
            } else {
                use_path = raw_exe;
            }
        }
    }
#elif defined(__linux__)
    {
        ssize_t n = readlink("/proc/self/exe", raw_exe, sizeof(raw_exe) - 1);
        if (n > 0) {
            raw_exe[n] = '\0';
            if (realpath(raw_exe, real_exe) != NULL) {
                use_path = real_exe;
            } else {
                use_path = raw_exe;
            }
        }
    }
#endif

    if (!use_path || !use_path[0]) return 0;
    slash = strrchr(use_path, '/');
    if (!slash) return 0;
    if (slash == use_path) {
        snprintf(dir_out, dir_sz, "/");
    } else {
        size_t dlen = (size_t)(slash - use_path);
        if (dlen >= dir_sz) dlen = dir_sz - 1;
        memcpy(dir_out, use_path, dlen);
        dir_out[dlen] = '\0';
    }
    return 1;
}

/* Check `dir` for lincupl.dl (generated at build time: the devices of
 * Atmel.dl, then those only cupl.dl has), then Atmel.dl, then cupl.dl. */
static int lincupl_probe_dl_in_dir(const char *dir, char *out, size_t out_sz) {
    static const char *const names[] = { "lincupl.dl", "Atmel.dl", "cupl.dl" };
    char dir_res[PATH_MAX];
    if (!dir || !dir[0]) return 0;
    if (lincupl_resolve_ci_path(dir, dir_res, sizeof(dir_res)) != 0 ||
        !lincupl_is_dir(dir_res)) {
        return 0;
    }
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char cand[PATH_MAX];
        char resolved[PATH_MAX];
        snprintf(cand, sizeof(cand), "%s/%s", dir_res, names[i]);
        if (lincupl_resolve_ci_path(cand, resolved, sizeof(resolved)) == 0 &&
            lincupl_is_reg_file(resolved)) {
            snprintf(out, out_sz, "%s", resolved);
            return 1;
        }
    }
    return 0;
}

/*
 * Resolve an explicit library path (e.g. from `-u <lib>` or `LIBCUPL=<lib>`)
 * case-insensitively on POSIX filesystems. Supports either a `.dl` file path
 * (e.g. `/path/to/atmel.dl` -> `/path/to/Atmel.dl`) or a directory containing
 * `lincupl.dl` / `Atmel.dl` / `cupl.dl`. If not found on disk, preserves slash-normalized
 * `raw_lib` so error diagnostics report the caller's path verbatim.
 */
int lincupl_resolve_explicit_dl(const char *raw_lib, char *out, size_t out_sz) {
    char resolved[PATH_MAX];
    if (!out || out_sz == 0) return 0;
    if (!raw_lib || !raw_lib[0]) {
        out[0] = '\0';
        return 0;
    }
    if (lincupl_resolve_ci_path(raw_lib, resolved, sizeof(resolved)) == 0) {
        if (lincupl_is_reg_file(resolved)) {
            snprintf(out, out_sz, "%s", resolved);
            return 1;
        }
        if (lincupl_is_dir(resolved) && lincupl_probe_dl_in_dir(resolved, out, out_sz)) {
            return 1;
        }
    }
    lincupl_normalize_slashes(raw_lib, out, out_sz);
    return 1;
}

/*
 * Locate the default CUPL device library (.dl) portably across macOS & Linux:
 *   1. LIBCUPL environment variable (file path or directory)
 *   2. LINCUPL_SHARE environment variable (directory or file path)
 *   3. Relative to executable path (../share/lincupl, share/lincupl,
 *      ../../share/lincupl, ../Shared)
 *   4. Relative to working directory (., ../share/lincupl, share/lincupl,
 *      ../../share/lincupl, ../Shared)
 *   5. The installed data directory (LINCUPL_DATADIR, from CMake), then the
 *      standard system share prefixes (/usr/local/share/lincupl, /usr/share/lincupl)
 * When probing a directory, prefers lincupl.dl (Atmel.dl's 85 devices, then
 * the 575 that only cupl.dl has), then Atmel.dl, then cupl.dl.
 * Returns 1 when an existing library (or explicit LIBCUPL) is found, 0 otherwise.
 */
int lincupl_find_default_dl(char *out, size_t out_sz) {
    const char *env_lib;
    const char *env_share;
    char exe_dir[PATH_MAX];

    if (!out || out_sz == 0) return 0;

    env_lib = getenv("LIBCUPL");
    if (env_lib && env_lib[0]) {
        return lincupl_resolve_explicit_dl(env_lib, out, out_sz);
    }

    env_share = getenv("LINCUPL_SHARE");
    if (env_share && env_share[0]) {
        char resolved[PATH_MAX];
        if (lincupl_resolve_ci_path(env_share, resolved, sizeof(resolved)) == 0) {
            if (lincupl_is_dir(resolved) && lincupl_probe_dl_in_dir(resolved, out, out_sz)) {
                return 1;
            }
            if (lincupl_is_reg_file(resolved)) {
                snprintf(out, out_sz, "%s", resolved);
                return 1;
            }
        }
    }

    if (lincupl_get_exe_dir(exe_dir, sizeof(exe_dir))) {
        static const char *const rel_dirs[] = {
            "",
            "/../share/lincupl",
            "/share/lincupl",
            "/../../share/lincupl",
            "/../Shared"
        };
        for (size_t i = 0; i < sizeof(rel_dirs) / sizeof(rel_dirs[0]); i++) {
            char dir_cand[PATH_MAX];
            snprintf(dir_cand, sizeof(dir_cand), "%s%s", exe_dir, rel_dirs[i]);
            if (lincupl_probe_dl_in_dir(dir_cand, out, out_sz)) {
                return 1;
            }
        }
    }

    {
        static const char *const cwd_dirs[] = {
            ".",
            "../share/lincupl",
            "share/lincupl",
            "../../share/lincupl",
            "../Shared",
#ifdef LINCUPL_DATADIR
            LINCUPL_DATADIR,
#endif
            "/usr/local/share/lincupl",
            "/usr/share/lincupl"
        };
        for (size_t i = 0; i < sizeof(cwd_dirs) / sizeof(cwd_dirs[0]); i++) {
            if (lincupl_probe_dl_in_dir(cwd_dirs[i], out, out_sz)) {
                return 1;
            }
        }
    }

    snprintf(out, out_sz, "Atmel.dl");
    return 0;
}

/* cwd + '/' + rel (and a trailing '/' if asked for), truncated to out_sz */
static void lincupl_join_cwd_path(const char *cwd, const char *rel,
                                  int ensure_trailing_slash,
                                  char *out, size_t out_sz) {
    size_t clen, rlen, pos = 0;
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!cwd || !rel) return;
    clen = strlen(cwd);
    rlen = strlen(rel);
    if (clen + 1 < out_sz) {
        memcpy(out, cwd, clen);
        pos = clen;
    }
    if (pos > 0 && out[pos - 1] != '/' && pos + 1 < out_sz) {
        out[pos++] = '/';
    }
    if (rlen > 0) {
        size_t avail = (out_sz - 1 > pos) ? (out_sz - 1 - pos) : 0;
        size_t n = (rlen < avail) ? rlen : avail;
        memcpy(out + pos, rel, n);
        pos += n;
    }
    if (ensure_trailing_slash && pos > 0 && out[pos - 1] != '/' && pos + 1 < out_sz) {
        out[pos++] = '/';
    }
    out[pos] = '\0';
}

/*
 * Keep the source name short for the phases: when na[5] contains a directory
 * prefix and is >= 80 chars, save CWD, absolutize na[2] (and na[1] if
 * active), chdir() into the resolved source directory, and rewrite na[5] to
 * its basename.  Returns 1 if the directory was changed; the caller changes
 * back to saved_cwd when the phases are done.
 */
int lincupl_enter_long_source_dir(
    const char **src_io,
    const char **lib_io,
    const char **work_io,
    char *saved_cwd, size_t saved_cwd_sz,
    char *abs_lib_buf, size_t abs_lib_sz,
    char *abs_work_buf, size_t abs_work_sz,
    char *short_src_buf, size_t short_src_sz) {
    char norm_src[PATH_MAX];
    char dir_raw[PATH_MAX];
    char dir_res[PATH_MAX];
    const char *slash;
    size_t dlen;
    int have_abs_lib = 0;
    int have_abs_work = 0;

    if (!src_io || !*src_io || strcmp(*src_io, "&") == 0) return 0;
    lincupl_normalize_slashes(*src_io, norm_src, sizeof(norm_src));
    slash = strrchr(norm_src, '/');
    if (!slash || slash[1] == '\0' || strlen(norm_src) < 80) return 0;

    if (!getcwd(saved_cwd, saved_cwd_sz)) return 0;

    dlen = (size_t)(slash - norm_src);
    if (dlen == 0) {
        strcpy(dir_raw, "/");
    } else if (dlen < sizeof(dir_raw)) {
        memcpy(dir_raw, norm_src, dlen);
        dir_raw[dlen] = '\0';
    } else {
        return 0;
    }

    lincupl_resolve_ci_path(dir_raw, dir_res, sizeof(dir_res));
    if (!lincupl_is_dir(dir_res)) return 0;

    if (lib_io && *lib_io && strcmp(*lib_io, "&") != 0 && abs_lib_buf && abs_lib_sz > 0) {
        char res_lib[PATH_MAX];
        char real_lib[PATH_MAX];
        lincupl_resolve_ci_path(*lib_io, res_lib, sizeof(res_lib));
        if (realpath(res_lib, real_lib) != NULL) {
            snprintf(abs_lib_buf, abs_lib_sz, "%s", real_lib);
        } else if (res_lib[0] == '/') {
            snprintf(abs_lib_buf, abs_lib_sz, "%s", res_lib);
        } else {
            lincupl_join_cwd_path(saved_cwd, res_lib, 0, abs_lib_buf, abs_lib_sz);
        }
        have_abs_lib = 1;
    }

    if (work_io && *work_io && strcmp(*work_io, "&") != 0 && abs_work_buf && abs_work_sz > 0) {
        char norm_w[PATH_MAX];
        lincupl_normalize_slashes(*work_io, norm_w, sizeof(norm_w));
        if (norm_w[0] == '/') {
            snprintf(abs_work_buf, abs_work_sz, "%s", norm_w);
        } else {
            lincupl_join_cwd_path(saved_cwd, norm_w, 1, abs_work_buf, abs_work_sz);
        }
        have_abs_work = 1;
    }

    if (chdir(dir_res) != 0) return 0;

    if (have_abs_lib) *lib_io = abs_lib_buf;
    if (have_abs_work) *work_io = abs_work_buf;
    snprintf(short_src_buf, short_src_sz, "%s", slash + 1);
    *src_io = short_src_buf;
    return 1;
}
