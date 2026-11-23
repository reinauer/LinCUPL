/*
 * locate.h - file names and data files
 *
 * Helpers shared by the driver, the kernel and the phases: slash
 * normalization, case-insensitive lookup and fopen(), the directory of the
 * running executable, and the search for the device library (.dl).
 */
#ifndef LINCUPL_LOCATE_H
#define LINCUPL_LOCATE_H

#include <limits.h>
#include <stddef.h>
#include <stdio.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Copy a path, turning every backslash into a slash. */
void lincupl_normalize_slashes(const char *in_path, char *out_path, size_t out_sz);

/* 1 if path names an existing regular file / directory. */
int lincupl_is_reg_file(const char *path);
int lincupl_is_dir(const char *path);

/*
 * Resolve `raw_path` case-insensitively component-by-component on POSIX
 * filesystems so designs and data files whose names differ in case from the
 * names used to refer to them (`TTL.M`, `ttl.m`, `FIT1500.M`, `Join2500.si`,
 * `Atmel.dl`) work identically on Linux and macOS.
 * Returns 0 on match (writing matched path to `resolved`), -1 if not found
 * (writing slash-normalized `raw_path` to `resolved`).
 */
int lincupl_resolve_ci_path(const char *raw_path, char *resolved, size_t resolved_sz);

/*
 * Portable fopen wrapper:
 * - Drops the 't' (text) mode letter ("rt" -> "r", "wt" -> "w").
 * - Normalizes backslashes to forward slashes.
 * - Falls back to case-insensitive path resolution when opening existing files.
 * - For write/create modes where parent directories have different casing on
 *   Linux, resolves the parent directory case-insensitively.
 */
FILE *lincupl_fopen_ci(const char *raw_path, const char *raw_mode);

/* Directory containing the running executable; 0 if it
 * cannot be determined. */
int lincupl_get_exe_dir(char *dir_out, size_t dir_sz);

/*
 * Resolve an explicit library path (e.g. from `-u <lib>` or `LIBCUPL=<lib>`)
 * case-insensitively on POSIX filesystems. Supports either a `.dl` file path
 * (e.g. `/path/to/atmel.dl` -> `/path/to/Atmel.dl`) or a directory containing
 * `lincupl.dl` / `Atmel.dl` / `cupl.dl`. If not found on disk, preserves slash-normalized
 * `raw_lib` so error diagnostics report the caller's path verbatim.
 */
int lincupl_resolve_explicit_dl(const char *raw_lib, char *out, size_t out_sz);

/*
 * Locate the default CUPL device library (.dl) on the host system:
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
int lincupl_find_default_dl(char *out, size_t out_sz);

/*
 * Keep the source name short for the phases: when na[5] contains a directory
 * prefix and is >= 80 chars, save CWD, absolutize na[2] (and na[1] if
 * active), chdir() into the resolved source directory, and rewrite na[5] to
 * its basename.  Returns 1 if the directory was changed; the caller changes
 * back to saved_cwd when the phases are done.
 */
int lincupl_enter_long_source_dir(const char **src_io, const char **lib_io, const char **work_io,
                                  char *saved_cwd, size_t saved_cwd_sz,
                                  char *abs_lib_buf, size_t abs_lib_sz,
                                  char *abs_work_buf, size_t abs_work_sz,
                                  char *short_src_buf, size_t short_src_sz);

#ifdef __cplusplus
}
#endif

#endif /* LINCUPL_LOCATE_H */
