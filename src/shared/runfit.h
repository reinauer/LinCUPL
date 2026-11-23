/*
 * runfit.h - finding and starting an external fitter
 *
 * Used by the compiler kernel (the fitter phase) and by cfitr.
 */
#ifndef LINCUPL_RUNFIT_H
#define LINCUPL_RUNFIT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Results of runfit_spawn() other than an exit status */
#define RUNFIT_NOT_STARTED (-1) /* no program could be started */
#define RUNFIT_ABNORMAL    (-2) /* the program did not exit normally */

/* An executable file `name` (not a directory) in one of the directories of
 * $PATH; 1 with its path in out, else 0. */
int runfit_find_in_path(const char *name, char *out, size_t out_sz);

/*
 * Locate the fitter program `name`.  A native program <name> next to the
 * running executable or on PATH is preferred; otherwise a Windows fitter
 * <name>.exe / <name>.com is looked up in $LINCUPL_FITTERS, the current
 * directory and the Fitters directories near the device library lib_path,
 * to be run through Wine on POSIX hosts. Returns 1
 * for a native program, 2 when Wine is needed, 0 if nothing was found.
 */
int runfit_find_fitter(const char *name, const char *lib_path, char *out, size_t out_sz);

/*
 * Start a program and wait for it.  The names in progs (NULL-terminated)
 * are tried in turn; argv is the argument vector, passed as
 * given.  With fitter_dir set, the program finds FITTERDIR set to it unless
 * it is set already.  Returns the exit status of the program,
 * RUNFIT_NOT_STARTED or RUNFIT_ABNORMAL.
 */
int runfit_spawn(const char *const *progs, char *const argv[], const char *fitter_dir);

#ifdef __cplusplus
}
#endif

#endif /* LINCUPL_RUNFIT_H */
