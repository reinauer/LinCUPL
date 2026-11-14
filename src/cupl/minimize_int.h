/*
 * minimize_int.h - declarations shared by the parts of the minimize phase
 * (logic minimizer, minimize_run).
 *
 *   minimize.c          errors, memory and files (src/shared/phase.c)
 *                       and the driver
 *   minimize_io.c       device class, CM<pid> input and CC<pid> output,
 *                       frames, and the conversion of frames to covers and
 *                       back
 *   minimize_cube.c     set families, cube operations, sorting, containment
 *                       and the cube structure (Espresso set.c, setc.c,
 *                       contain.c, cubestr.c)
 *   minimize_cover.c    cube lists, cofactors, complement, sharp and
 *                       unravel (Espresso cofactor.c, compl.c, sharp.c,
 *                       cvrm.c)
 *   minimize_levels.c   tautology, minimum cover, irredundant cover, the
 *                       three minimization levels and verification
 *   minimize_espresso.c level 4: interface to Berkeley Espresso 2.5.1
 *
 * The element-level set operations are inline functions below.
 */
#ifndef LINCUPL_MINIMIZE_INT_H
#define LINCUPL_MINIMIZE_INT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "bits.h"
#include "cfile.h"
#include "devlib.h"
#include "phase.h"
#include "setsort.h"

/* -------------------------------------------------------------------------
 * Error messages
 * ------------------------------------------------------------------------- */
enum {
    ERR_PROGRAM = 0,
    ERR_OPEN = 1,
    ERR_INVALID_FILE = 3,
    ERR_OUT_OF_MEMORY = 4,
    ERR_DISK_WRITE = 5,
    ERR_LIB_ACCESS_KEY = 6,
    ERR_LIB_INTERFACE = 7,
    ERR_BAD_LIBRARY = 8,
    ERR_DEVICE_NOT_FOUND = 9,
    ERR_DISK_READ = 11,
    /* Internal consistency errors; all are reported as "program error". */
    ERR_INTERNAL_BAD_ROW = 1002,          /* 0x3ea */
    ERR_INTERNAL_NO_UNATE_VAR = 1004,     /* 0x3ec */
    ERR_INTERNAL_NO_LARGEST_CUBE = 1005,  /* 0x3ed */
    ERR_INTERNAL_NO_CUBE_SETUP = 1006,    /* 0x3ee */
    ERR_INTERNAL_NO_COVER = 1007,         /* 0x3ef */
    ERR_INTERNAL_UNRAVEL_TOO_BIG = 1008,  /* 0x3f0 */
    ERR_INTERNAL_VERIFY = 1009,           /* 0x3f1 */
    ERR_INTERNAL_ON_OFF_OVERLAP = 1010,   /* 0x3f2 */
    ERR_INTERNAL_EMPTY_ROW = 1011,        /* 0x3f3 */
    ERR_INTERNAL_ROW_NOT_COVERED = 1012,  /* 0x3f4 */
    ERR_INTERNAL_NO_COLUMN = 1013,        /* 0x3f5 */
    ERR_INTERNAL_EOL_IN_INPUTS = 1017,    /* 0x3f9 */
    ERR_INTERNAL_EOL_IN_OUTPUTS = 1019,   /* 0x3fb */
    ERR_INTERNAL_BAD_CHAR = 1020,         /* 0x3fc */
    ERR_INTERNAL_BAD_LEVEL = 1021,        /* 0x3fd */
    /* consistency report after a failed verification */
    ERR_CHECK_ON_DC_DISJOINT = 1024,      /* 0x400 */
    ERR_CHECK_ON_DC_OVERLAP = 1025,
    ERR_CHECK_ON_OFF_DISJOINT = 1026,
    ERR_CHECK_ON_OFF_OVERLAP = 1027,
    ERR_CHECK_DC_OFF_DISJOINT = 1028,
    ERR_CHECK_DC_OFF_OVERLAP = 1029,
    ERR_CHECK_COMPLETE = 1030,
    ERR_CHECK_INCOMPLETE = 1031,
    ERR_VERIFY_NOT_COVERED = 1032,        /* 0x408 */
    ERR_VERIFY_NOT_CONTAINED = 1033,      /* 0x409 */
    ERR_INTERNAL_LIB_STATUS = 1034,       /* 0x40a */
    ERR_INTERNAL_ESPRESSO = 1035          /* level 4: Espresso 2.5.1 stopped with an error */
};

/* -------------------------------------------------------------------------
 * Limits and file format constants
 * ------------------------------------------------------------------------- */

/* CM and CC files: see cfile.h */
#define FRAME_LEVEL_MASK 7
#define ROW_NORMAL 1               /* header word of the mask plane */
#define ROW_SEPARATOR 2
#define ROW_SKIP 4

/* Device information record of the device library (see devlib.h) */
#define DL_INFO_CLASS 0x11

#define UNRAVEL_LIMIT 10000000
#define INTERSECT_CHUNK 500
#define MERGE_STACK_SIZE 34        /* stack depth of minimize_cb_sharp() */

/* -------------------------------------------------------------------------
 * Sets, set families and cubes (Espresso)
 * ------------------------------------------------------------------------- */

/* A set is an array of 32 bit words.  Word 0 holds the index of the last
 * word in its low 10 bits, flags in bits 10..15 and a sort key ("size") in
 * the upper 16 bits; element n is bit (n & 31) of word (n >> 5) + 1. */
typedef uint32_t *pset;

#define SET_LOOP_MASK 0x3ffu
#define SET_PRIME     0x8000u
#define SET_NONESSEN  0x4000u
#define SET_ACTIVE    0x2000u
#define SET_REDUND    0x1000u
#define SET_COVERED   0x0800u

typedef struct set_family {
    int wsize;              /* words per set */
    int sf_size;            /* elements per set */
    int capacity;           /* sets allocated */
    int count;              /* sets in use */
    int active_count;       /* sets with SET_ACTIVE */
    pset data;
    struct set_family *next; /* free list */
} set_family_t;
typedef set_family_t *pcover;

/* A cube list is { cofactor cube, end marker, cube, cube, ..., NULL }. */
typedef pset *cubelist_t;

typedef struct {
    pcover F, D, R;         /* on-set, don't-care set, off-set */
    int pla_type;           /* which of them the input describes */
} pla_t;
#define PLA_TYPE_F 1
#define PLA_TYPE_D 2
#define PLA_TYPE_R 4

/* description of the cube structure */
typedef struct {
    int size;               /* parts in a cube */
    int num_vars;
    int num_binary_vars;
    int *first_part;
    int *last_part;
    int *part_size;
    int *first_word;
    int *last_word;
    pset binary_mask;
    pset mv_mask;
    pset *var_mask;
    pset *temp;             /* CUBE_TEMP scratch cubes */
    pset fullset;
    pset emptyset;
    uint32_t inmask;        /* mask for the last word of the binary part */
    int inword;             /* last word of the binary part, -1 if none */
    int *sparse;            /* variable is to be made sparse */
} cube_struct_t;
#define CUBE_TEMP 10

/* result of minimize_massive_count() */
typedef struct {
    int *part_zeros;
    int *var_zeros;
    int *parts_active;
    int *is_unate;
    int vars_active;
    int vars_unate;
    int best;
} cdata_t;

/* comparison function of lincupl_sort_sets() */
typedef lincupl_set_compare_t set_compare_t;

/* -------------------------------------------------------------------------
 * Frames
 * ------------------------------------------------------------------------- */
/* A frame: the fields of its header (cf_frame_t), its rows and the
 * bookkeeping of this phase. */
typedef struct frame {
    uint16_t flags;         /* +0 */
    uint16_t field2;        /* +2 */
    uint16_t num_vars;      /* +4: symbols = bits per plane */
    uint16_t num_rows;      /* +6: product terms */
    uint16_t field8;        /* +8 */
    uint16_t field10;       /* +10 */
    uint16_t *data;         /* rows of row_words words: out, mask, val plane */
    int row_words;
    int total_words;
    int alloc_rows;         /* rows the buffer has room for */
    struct frame *next;
} frame_t;

/* -------------------------------------------------------------------------
 * State shared between the parts (each part resets what it defines)
 * ------------------------------------------------------------------------- */

/* minimize.c */
extern const char *minimize_args[10];   /* copy of the phase argv */

/* minimize_io.c */
extern char minimize_cm_name[9];        /* CM<pid>, input */
extern const char *minimize_out_name;   /* NULL until the output is opened */
/* A frame with a separator row is minimized in two sections. */
extern int minimize_split_state;        /* 0 none, 1 first section read, 2 first section
                                           stored, 3 reading the second section */
extern int minimize_shared_mode;        /* all frames are minimized together */

/* minimize_cube.c */
extern cube_struct_t minimize_cube;
extern cdata_t minimize_cdata;

/* -------------------------------------------------------------------------
 * Functions
 * ------------------------------------------------------------------------- */

/* minimize.c: errors, memory, files */
void minimize_report_error(int code, const char *detail);
void minimize_fatal_error(int code, const char *detail);
void minimize_progress_tick(void);
void *minimize_alloc_tracked(size_t size, const char *context);
char *minimize_dup_string(const char *s, const char *context);
void *minimize_work_alloc(size_t size);
void minimize_work_free(void *p);
FILE *minimize_open_file(const char *path, const char *mode);
void minimize_close_file(FILE *fp);

/* minimize_io.c */
void minimize_io_reset(void);
int minimize_load_device_class(void);
int minimize_open_files(void);
void minimize_close_files(void);
void minimize_copy_design_header(void);
int minimize_read_frame(frame_t *f);
void minimize_write_frame(frame_t *f);
void minimize_queue_frame(const frame_t *f);
frame_t *minimize_dequeue_frame(void);
void minimize_free_frame(frame_t *f);
void minimize_begin_frame(frame_t *f);
pla_t *minimize_new_PLA(int pla_type);
void minimize_free_PLA(pla_t *PLA);
int minimize_read_pla(pla_t *PLA, int level);
void minimize_store_pla(pla_t *PLA, int level);

/* minimize_cube.c */
void minimize_cube_reset(void);
void minimize_sf_forget(void);
pcover minimize_sf_new(int num, int size);
pcover minimize_new_cover(int num);
void minimize_sf_free(pcover A);
pcover minimize_sf_save(pcover A);
pcover minimize_sf_append(pcover A, pcover B);
pcover minimize_sf_addset(pcover A, const uint32_t *s);
pcover minimize_sf_active(pcover A);
pcover minimize_sf_inactive(pcover A);
int minimize_full_row(const uint32_t *p, const uint32_t *cof);
int minimize_cdist0(const uint32_t *a, const uint32_t *b);
int minimize_ccommon(const uint32_t *a, const uint32_t *b);
int minimize_d1_order(pset *a, pset *b);
pset *minimize_sf_list(pcover A);
pcover minimize_sf_contain(pcover A);
pcover minimize_sf_union(pcover A, pcover B);
pcover minimize_lex_sort(pcover T);
pcover minimize_size_sort(pcover T);
void minimize_cube_setup(void);

/* minimize_cover.c */
void minimize_cubelist_set_end(cubelist_t T, pset *terminator);
int minimize_cubelist_slots(cubelist_t T);
void minimize_cubelist_free(cubelist_t T);
cubelist_t minimize_cubelist_alloc(int slots);
cubelist_t minimize_cube1list(pcover A);
cubelist_t minimize_cube2list(pcover A, pcover B);
cubelist_t minimize_cube3list(pcover A, pcover B, pcover C);
cubelist_t minimize_cofactor(cubelist_t T, pset c);
cubelist_t minimize_scofactor(cubelist_t T, pset c, int var);
void minimize_massive_count(cubelist_t T);
int minimize_binate_split_select(cubelist_t T, pset cleft, pset cright);
pset minimize_largest_cube(cubelist_t T);
pcover minimize_complement(cubelist_t T);
pcover minimize_cv_intersect(pcover A, pcover B);
pcover minimize_cb_sharp(pset c, pcover T);
pcover minimize_unravel(pcover B, int start);

/* minimize_levels.c */
void minimize_levels_reset(void);
void minimize_pla(pla_t *PLA, int level);

/* =========================================================================
 * Sets (Espresso set.c)
 * ========================================================================= */

#define LOOP(p) ((int)((p)[0] & SET_LOOP_MASK))
#define SIZE(p) ((p)[0] >> 16)
#define TESTP(p, flag) (((p)[0] & (flag)) != 0)
#define SET(p, flag) ((p)[0] |= (flag))
#define RESET(p, flag) ((p)[0] &= ~(uint32_t)(flag))
#define GETSET(A, i) ((A)->data + (size_t)(A)->wsize * (size_t)(i))
#define foreach_set(A, last, p) \
    for ((p) = (A)->data, (last) = (p) + (size_t)(A)->count * (size_t)(A)->wsize; (p) < (last); (p) += (A)->wsize)

static inline int set_words(int size)
{
    return size < 1 ? 2 : ((size - 1) >> 5) + 2;
}

static inline int is_in_set(const uint32_t *p, int e)
{
    return (int)((p[(e >> 5) + 1] >> (e & 31)) & 1u);
}

static inline void set_insert(pset p, int e)
{
    p[(e >> 5) + 1] |= 1u << (e & 31);
}

static inline void set_remove(pset p, int e)
{
    p[(e >> 5) + 1] &= ~(1u << (e & 31));
}

static inline void put_size(pset p, unsigned size)
{
    p[0] = (p[0] & 0xffffu) | (size << 16);
}

/* number of elements */
static inline int set_ord(const uint32_t *a)
{
    int sum = 0;
    for (int i = LOOP(a); i > 0; i--) {
        sum += lincupl_popcount32(a[i]);
    }
    return sum;
}

/* number of elements of the intersection */
static inline int set_dist(const uint32_t *a, const uint32_t *b)
{
    int sum = 0;
    for (int i = LOOP(a); i > 0; i--) {
        sum += lincupl_popcount32(a[i] & b[i]);
    }
    return sum;
}

static inline pset set_clear(pset r, int size)
{
    int i = set_words(size) - 1;
    r[0] = (uint32_t)i;
    do {
        r[i] = 0;
    } while (--i > 0);
    return r;
}

static inline pset set_fill(pset r, int size)
{
    int i = set_words(size) - 1;
    r[0] = (uint32_t)i;
    do {
        r[i] = ~0u;
    } while (--i > 0);
    for (int e = size < 0 ? 0 : size; e < LOOP(r) * 32; e++) {
        set_remove(r, e);
    }
    return r;
}

/* copies the header word as well */
static inline pset set_copy(pset r, const uint32_t *a)
{
    for (int i = LOOP(a); i >= 0; i--) {
        r[i] = a[i];
    }
    return r;
}

static inline void set_loop(pset r, int loop)
{
    r[0] = (r[0] & ~SET_LOOP_MASK) | (uint32_t)loop;
}

static inline pset set_and(pset r, const uint32_t *a, const uint32_t *b)
{
    int i = LOOP(a);
    set_loop(r, i);
    do {
        r[i] = a[i] & b[i];
    } while (--i > 0);
    return r;
}

static inline pset set_or(pset r, const uint32_t *a, const uint32_t *b)
{
    int i = LOOP(a);
    set_loop(r, i);
    do {
        r[i] = a[i] | b[i];
    } while (--i > 0);
    return r;
}

static inline pset set_diff(pset r, const uint32_t *a, const uint32_t *b)
{
    int i = LOOP(a);
    set_loop(r, i);
    do {
        r[i] = a[i] & ~b[i];
    } while (--i > 0);
    return r;
}

static inline int setp_empty(const uint32_t *a)
{
    int i = LOOP(a);
    do {
        if (a[i] != 0) {
            return 0;
        }
    } while (--i > 0);
    return 1;
}

static inline int setp_equal(const uint32_t *a, const uint32_t *b)
{
    int i = LOOP(a);
    do {
        if (a[i] != b[i]) {
            return 0;
        }
    } while (--i > 0);
    return 1;
}

static inline int setp_disjoint(const uint32_t *a, const uint32_t *b)
{
    int i = LOOP(a);
    do {
        if ((a[i] & b[i]) != 0) {
            return 0;
        }
    } while (--i > 0);
    return 1;
}

/* a is a subset of b */
static inline int setp_implies(const uint32_t *a, const uint32_t *b)
{
    int i = LOOP(a);
    do {
        if ((a[i] & ~b[i]) != 0) {
            return 0;
        }
    } while (--i > 0);
    return 1;
}

static inline pset set_new(int size)
{
    return set_clear(minimize_work_alloc((size_t)set_words(size) * sizeof(uint32_t)), size);
}

static inline pset new_cube(void)
{
    return set_new(minimize_cube.size);
}

/* add weight to count[] for every element of a */
static inline void set_adjcnt(const uint32_t *a, int *count, int weight)
{
    for (int i = LOOP(a); i > 0; i--) {
        int *cnt = count + (i - 1) * 32;
        for (uint32_t val = a[i]; val != 0; val >>= 1, cnt++) {
            if (val & 1u) {
                *cnt += weight;
            }
        }
    }
}

#endif /* LINCUPL_MINIMIZE_INT_H */
