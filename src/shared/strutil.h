/*
 * strutil.h - string and file name helpers
 *
 * Case conversion of names, and the file name rules shared by the compiler
 * phases, the simulator and the tools: where the directory part of a name
 * ends, splitting a name into directory, base name and extension, and the
 * name of an output file built from the design's source name.
 *
 * A directory part may end in '/', '\' or a drive colon.
 */
#ifndef LINCUPL_STRUTIL_H
#define LINCUPL_STRUTIL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Convert s to lower / upper case in place (ASCII letters only); NULL is
 * ignored. */
void lincupl_strlwr(char *s);
void lincupl_strupr(char *s);

/* Copy src to dst, truncated to size - 1 characters, always terminated
 * (unless size is 0).  Returns the length of src, so a result >= size
 * means src was truncated. */
size_t lincupl_strcpy_trunc(char *dst, size_t size, const char *src);

/* End of the directory part of path: the last '/', else the last '\', else
 * the last ':'; NULL if there is none. */
const char *lincupl_path_sep(const char *path);

/* Like lincupl_path_sep, but the later of the last '/' and the last '\'. */
const char *lincupl_path_sep_last(const char *path);

/* Length of the directory part of path including its separator
 * (lincupl_path_sep), 0 if there is none. */
size_t lincupl_path_dir_len(const char *path);

/* Split path into directory part (with its separator, lincupl_path_sep_last),
 * base name and extension (from the last '.' of the file name on, with the
 * dot).  Each part is truncated to its buffer size; a NULL buffer is not
 * written. */
void lincupl_split_path(const char *path, char *dir, size_t dir_size, char *base, size_t base_size,
                        char *ext, size_t ext_size);

/* name without a leading "./": a name that starts with a dot loses its first
 * two characters (a lone "." just the dot). */
const char *lincupl_skip_dot_dir(const char *name);

/* Output file name of a design: dir, then the file name of source without
 * its directory part (lincupl_path_sep) and without a trailing ".pld" (any
 * case), then ext.  Written to out like snprintf(); returns the length of
 * the whole name. */
size_t lincupl_design_file_name(char *out, size_t out_size, const char *dir, const char *source,
                                const char *ext);

#ifdef __cplusplus
}
#endif

#endif /* LINCUPL_STRUTIL_H */
