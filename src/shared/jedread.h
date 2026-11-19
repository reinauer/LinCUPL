/*
 * jedread.h - reading the fuse map of a JEDEC file (JESD3)
 *
 * A JEDEC file is a design specification (free text), followed by fields
 * that each end with '*'.  The first character of a field says what it is:
 *
 *   QF<n>            number of fuses
 *   QP<n>            number of pins
 *   F<0|1>           state of the fuses no L field lists
 *   L<n> <0|1>...    states of consecutive fuses, starting with fuse n
 *   C<hhhh>          fuse checksum (jedsum.h)
 *
 * The fields stand between STX and ETX; the transmission checksum follows
 * the ETX.  A fuse state of 0 is an intact link (the input is connected),
 * 1 a blown one.
 *
 * Files of several compilers and device programmers are accepted: field
 * letters in either case, any line ends, a file without STX (it is read
 * from its start) or without ETX (read to its end), a QF field that is
 * missing or 0 (the number of fuses is then the highest fuse listed + 1)
 * and fields this reader has no use for (N notes, G security fuse, P and V
 * test vectors and anything else), which are skipped.  The transmission
 * checksum is not looked at.
 *
 * What is checked: every fuse must have a state (from an L field or the F
 * field), no L field may reach beyond QF, a C field must match the fuse map,
 * and a file that has neither an ETX nor a C field is taken as cut short.
 */
#ifndef LINCUPL_JEDREAD_H
#define LINCUPL_JEDREAD_H

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JED_MAX_FUSES 4000000UL     /* a QF or fuse number above this is an error */

typedef struct {
    unsigned long nfuses;       /* number of fuses */
    unsigned char *fuses;       /* nfuses states, 0 or 1 */
    unsigned long qf;           /* the QF field, 0 if the file has none */
    unsigned pins;              /* the QP field, 0 if the file has none */
    int has_stx;
    int has_etx;
    int has_checksum;           /* the file has a C field */
    unsigned checksum;          /* fuse checksum of the map */
    char *design;               /* the design specification in front of the
                                 * first field, line ends as "\n" */
} jed_map_t;

/* Read a JEDEC file from its current position.  Returns 1 and the map in
 * *map (to be released with jed_free), or 0 and the reason as text in err. */
int jed_read(FILE *fp, jed_map_t *map, char *err, size_t err_size);
void jed_free(jed_map_t *map);

#ifdef __cplusplus
}
#endif

#endif /* LINCUPL_JEDREAD_H */
