/*
 * absdump.h - the absolute file (<design>.abs) as readable text.
 *
 * The text shows the header, the symbol table, the product terms of every
 * OR array as rows of 0 / 1 / - per input variable, and the field lists,
 * decoded with the readers of cfile.h that csim uses as well.  Bytes whose
 * meaning is not known are shown as hex where they are; a short or broken
 * file gives what could be decoded and a line starting with "*** error:".
 *
 * With the device library the file was compiled for, the text also says
 * for each output whether csim complements its OR array (the polarity of
 * the output's node record), which the product terms alone do not show.
 */
#ifndef LINCUPL_ABSDUMP_H
#define LINCUPL_ABSDUMP_H

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Decode the .abs file at `path` (or the stream `fp`, read from its current
 * position to its end).  `lib_path` names the device library (.dl), or is
 * NULL or "" to decode without it.  Returns the text as a NUL-terminated
 * string to be released with free(), or NULL if memory runs out.
 * `*complete` (if not NULL) is set to 1 if the whole file was decoded and
 * 0 if an error line was written.
 */
char *absdump_file(const char *path, const char *lib_path, int *complete);
char *absdump_stream(FILE *fp, const char *lib_path, int *complete);

#ifdef __cplusplus
}
#endif

#endif /* LINCUPL_ABSDUMP_H */
