/*
 * min_esp.c - the ATF150x fitter's own Espresso-style minimiser.
 *
 * It is a small, early version of Berkeley Espresso: the routine
 * `complement` (compl.c) with what it needs from set.c, setc.c, cofactor.c,
 * contain.c, unate.c and the BSD qsort that Espresso ships as ucbqsort.c.
 * Nothing else of Espresso is here: no expand / reduce / irredundant, no
 * covering routine.  The one caller is esp_complement_entity, which writes
 * an entity as a PLA, complements it and reads the result back.
 *
 * Function for function this is the source of Espresso 2.3 (release
 * 01/31/88); the Espresso names are kept with the prefix esp_.  It is
 * deliberately not the Berkeley Espresso 2.5.1 in third_party/espresso:
 * the fitter's output depends on the exact covers this version produces.
 * The differences from the 2.3 sources:
 *   - sf_new clears the set array it allocates;
 *   - the glue has its own copies of sf_new (without the free list of set
 *     family headers), sf_addset, set_copy, set_clear and set_or;
 *   - the debug output (debug_print, debug1_print, cprint, pc1, pc2 and
 *     the printf calls of binate_split_select and compl_merge) is left
 *     out, there being no way to switch it on.
 *
 * The cube structure, the covers and the strings built here are released
 * at the end of esp_complement_entity.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "bits.h"
#include "setsort.h"
#include "min_priv.h"
#include "tt.h"

typedef uint32_t *pset;
typedef uint32_t *pcube;

typedef struct EspSetFamily {       /* set_family_t */
    int       wsize;                /* words per set */
    int       sf_size;              /* elements per set */
    int       capacity;             /* sets allocated */
    int       count;                /* sets used */
    int       active_count;
    uint32_t *data;
    struct EspSetFamily *next;      /* free list link */
} EspSetFamily;
typedef EspSetFamily *pcover;

#define ESP_BPI             32
#define ESP_WHICH_WORD(e)   (((e) >> 5) + 1)
#define ESP_WHICH_BIT(e)    ((e) & 31)
#define ESP_SET_SIZE(size)  ((size) <= ESP_BPI ? 2 : ESP_WHICH_WORD((size) - 1) + 1)
#define ESP_LOOPINIT(size)  ((size) <= ESP_BPI ? 1 : ESP_WHICH_WORD((size) - 1))
#define ESP_LOOP(s)         ((int)((s)[0] & 0x03ff))
#define ESP_PUTLOOP(s, i)   ((s)[0] = ((s)[0] & ~0x03ffu) | (uint32_t)(i))
#define ESP_SIZE(s)         ((int)((s)[0] >> 16))
#define ESP_PUTSIZE(s, n)   ((s)[0] = ((s)[0] & 0xffffu) | ((uint32_t)(n) << 16))
#define ESP_ACTIVE          0x2000u
#define ESP_DISJOINT        0x55555555u
#define ESP_CUBE_TEMP       10
#define ESP_GETSET(A, i)    ((A)->data + (size_t)(A)->wsize * (size_t)(i))
#define ESP_CUBELISTSIZE(T) ((int)((pcube *)(T)[1] - (T)) - 3)
#define ESP_GETINPUT(c, pos) \
    (((c)[ESP_WHICH_WORD(2 * (pos))] >> ESP_WHICH_BIT(2 * (pos))) & 3u)

enum { ESP_TRUE = 1, ESP_MAYBE = 2 };
enum { ESP_USE_COMPL_LIFT = 0, ESP_USE_COMPL_LIFT_ONSET = 1, ESP_USE_COMPL_LIFT_ONSET_COMPLEX = 2 };

/* struct cube_struct */
static struct {
    int    size;
    int    num_vars;
    int    num_binary_vars;
    int   *first_part;
    int   *last_part;
    int   *part_size;
    int   *first_word;
    int   *last_word;
    pset   binary_mask;
    pset   mv_mask;
    pset  *var_mask;
    pset  *temp;
    pset   fullset;
    pset   emptyset;
    uint32_t inmask;
    int    inword;
    int   *sparse;
    int    num_mv_vars;
    int    output;
} cube;

/* struct cdata_struct */
static struct {
    int *part_zeros;
    int *var_zeros;
    int *parts_active;
    int *is_unate;
    int  vars_active;
    int  vars_unate;
    int  best;
} cdata;

static EspSetFamily *g_set_family_garbage;

/* ------------------------------------------------------------------ */
/* memory                                                              */
/* ------------------------------------------------------------------ */

static void esp_out_of_memory(long size)
{
    fflush(stdout);
    fprintf(stderr, "\nout of memory allocating %ld bytes\n", size);
    exit(1);
}

/* malloc; a request of less than one byte becomes four.  The
 * block is zero-filled (every use below initialises what it reads
 * anyway). */
static void *esp_malloc(long size)
{
    void *p;

    if (size < 1)
        size = 4;
    p = calloc(1, (size_t)size);
    if (p == NULL)
        esp_out_of_memory(size);
    return p;
}

static void *esp_realloc(void *old, long size)
{
    void *p;

    if (size < 1)
        size = 4;
    if (old == NULL)
        return esp_malloc(size);
    p = realloc(old, (size_t)size);
    if (p == NULL)
        esp_out_of_memory(size);
    return p;
}

static void esp_free(void *p)
{
    if (p != NULL)
        free(p);
}

static void esp_fatal(const char *s)
{
    fprintf(stderr, "espresso: %s\n", s);
    exit(1);
}

/* ------------------------------------------------------------------ */
/* set.c                                                               */
/* ------------------------------------------------------------------ */

static int esp_set_ord(pset a)
{
    int i, sum = 0;
    uint32_t val;

    for (i = ESP_LOOP(a); i > 0; i--) {
        if ((val = a[i]) != 0)
            sum += lincupl_popcount32(val);
    }
    return sum;
}

static pset esp_set_clear(pset r, int size)
{
    int i = ESP_LOOPINIT(size);

    r[0] = (uint32_t)i;
    do
        r[i] = 0;
    while (--i > 0);
    return r;
}

static pset esp_set_fill(pset r, int size)
{
    int i = ESP_LOOPINIT(size);

    r[0] = (uint32_t)i;
    r[i] = 0xffffffffu >> ((unsigned)(-size) & 0x1f);
    while (--i > 0)
        r[i] = 0xffffffffu;
    return r;
}

static pset esp_set_copy(pset r, pset a)
{
    int i = ESP_LOOP(a);

    do
        r[i] = a[i];
    while (--i >= 0);
    return r;
}

static pset esp_set_and(pset r, pset a, pset b)
{
    int i = ESP_LOOP(a);

    ESP_PUTLOOP(r, i);
    do
        r[i] = a[i] & b[i];
    while (--i > 0);
    return r;
}

static pset esp_set_or(pset r, pset a, pset b)
{
    int i = ESP_LOOP(a);

    ESP_PUTLOOP(r, i);
    do
        r[i] = a[i] | b[i];
    while (--i > 0);
    return r;
}

static pset esp_set_diff(pset r, pset a, pset b)
{
    int i = ESP_LOOP(a);

    ESP_PUTLOOP(r, i);
    do
        r[i] = a[i] & ~b[i];
    while (--i > 0);
    return r;
}

/* r = (a & mask) | (b & ~mask) */
static pset esp_set_merge(pset r, pset a, pset b, pset mask)
{
    int i = ESP_LOOP(a);

    ESP_PUTLOOP(r, i);
    do
        r[i] = (a[i] & mask[i]) | (b[i] & ~mask[i]);
    while (--i > 0);
    return r;
}

static int esp_setp_equal(pset a, pset b)
{
    int i = ESP_LOOP(a);

    do
        if (a[i] != b[i])
            return 0;
    while (--i > 0);
    return 1;
}

static int esp_setp_disjoint(pset a, pset b)
{
    int i = ESP_LOOP(a);

    do
        if (a[i] & b[i])
            return 0;
    while (--i > 0);
    return 1;
}

/* a implies b: no element of a is missing in b (INLINEsetp_implies) */
static int esp_setp_implies(pset a, pset b)
{
    int i = ESP_LOOP(a);

    do
        if (a[i] & ~b[i])
            return 0;
    while (--i > 0);
    return 1;
}

static void esp_set_insert(pset s, int e)
{
    s[ESP_WHICH_WORD(e)] |= 1u << ESP_WHICH_BIT(e);
}

static void esp_set_remove(pset s, int e)
{
    s[ESP_WHICH_WORD(e)] &= ~(1u << ESP_WHICH_BIT(e));
}

static int esp_is_in_set(pset s, int e)
{
    return (s[ESP_WHICH_WORD(e)] & (1u << ESP_WHICH_BIT(e))) != 0;
}

static pset esp_set_new(int size)
{
    return esp_set_clear(esp_malloc((long)ESP_SET_SIZE(size) * 4), size);
}

static pcube esp_new_cube(void)
{
    return esp_set_new(cube.size);
}

/* sf_new without the free list of headers (the copy used by the
 * glue).  The set array is cleared. */
static pcover esp_sf_new_plain(int num, int size)
{
    pcover A = esp_malloc((long)sizeof(*A));
    long words;

    A->sf_size = size;
    A->wsize = ESP_SET_SIZE(size);
    A->capacity = num;
    words = (long)A->wsize * num;
    A->data = esp_malloc(words * 4);
    for (long i = 0; i < words; i++)
        A->data[i] = 0;
    A->count = 0;
    A->active_count = 0;
    A->next = NULL;
    return A;
}

/* sf_new; the set array is cleared (not in Espresso 2.3) */
static pcover esp_sf_new(int num, int size)
{
    pcover A;
    long words;

    if (g_set_family_garbage == NULL) {
        A = esp_malloc((long)sizeof(*A));
    } else {
        A = g_set_family_garbage;
        g_set_family_garbage = A->next;
    }
    A->sf_size = size;
    A->wsize = ESP_SET_SIZE(size);
    A->capacity = num;
    words = (long)A->wsize * num;
    A->data = esp_malloc(words * 4);
    for (long i = 0; i < words; i++)
        A->data[i] = 0;
    A->count = 0;
    A->active_count = 0;
    return A;
}

/* the set array is released, the header goes to the free list */
static void esp_sf_free(pcover A)
{
    esp_free(A->data);
    A->data = NULL;
    A->next = g_set_family_garbage;
    g_set_family_garbage = A;
}

static void esp_intcpy(uint32_t *d, const uint32_t *s, long n)
{
    for (long i = 0; i < n; i++)
        d[i] = s[i];
}

static pcover esp_sf_append(pcover A, pcover B)
{
    long asize = (long)A->count * A->wsize;
    long bsize = (long)B->count * B->wsize;

    if (A->sf_size != B->sf_size)
        esp_fatal("sf_append: sf_size mismatch");
    A->capacity = A->count + B->count;
    A->data = esp_realloc(A->data, (long)A->capacity * A->wsize * 4);
    esp_intcpy(A->data + asize, B->data, bsize);
    A->count += B->count;
    A->active_count += B->active_count;
    esp_sf_free(B);
    return A;
}

static pcover esp_sf_addset(pcover A, pset s)
{
    pset p;

    if (A->count >= A->capacity) {
        A->capacity = A->capacity + A->capacity / 2 + 1;
        A->data = esp_realloc(A->data, (long)A->capacity * A->wsize * 4);
    }
    p = ESP_GETSET(A, A->count++);
    esp_set_copy(p, s);
    return A;
}

/* for every column of r the sum over the sets of A that have it
 * of 1024 / (set_ord - 1) */
static int *esp_sf_count_restricted(pcover A, pset r)
{
    int *count = esp_malloc((long)A->sf_size * 4);
    pset p, last;

    for (int i = A->sf_size - 1; i >= 0; i--)
        count[i] = 0;
    for (p = A->data, last = p + (size_t)A->count * (size_t)A->wsize; p < last; p += A->wsize) {
        int ord = esp_set_ord(p);

        for (int i = ESP_LOOP(p); i > 0;) {
            uint32_t val = p[i] & r[i];
            int base = --i << 5;

            for (; val != 0; base++, val >>= 1) {
                if (val & 1)
                    count[base] += ord - 1 != 0 ? 1024 / (ord - 1) : 0;    /* never 0: callers have ord >= 2 */
            }
        }
    }
    return count;
}

typedef int (*EspCompare)(pset *a, pset *b);

/* ------------------------------------------------------------------ */
/* setc.c, contain.c                                                   */
/* ------------------------------------------------------------------ */

static int esp_full_row(pcube p, pcube cof)
{
    int i = ESP_LOOP(p);

    do
        if ((p[i] | cof[i]) != cube.fullset[i])
            return 0;
    while (--i > 0);
    return 1;
}

/* 1 when the cubes intersect */
static int esp_cdist0(pcube a, pcube b)
{
    int w, last, var;
    uint32_t x;

    if ((last = cube.inword) != -1) {
        x = a[last] & b[last];
        if (~(x | x >> 1) & cube.inmask)
            return 0;
        for (w = 1; w < last; w++) {
            x = a[w] & b[w];
            if (~(x | x >> 1) & ESP_DISJOINT)
                return 0;
        }
    }
    for (var = cube.num_binary_vars; var < cube.num_vars; var++) {
        pcube mask = cube.var_mask[var];
        int hit = 0;

        last = cube.last_word[var];
        for (w = cube.first_word[var]; w <= last; w++) {
            if (a[w] & b[w] & mask[w]) {
                hit = 1;
                break;
            }
        }
        if (!hit)
            return 0;
    }
    return 1;
}

/* never seen running: distance 0, 1 or 2 (2 = two or more) */
static int esp_cdist01(pset a, pset b)
{
    int dist = 0;
    int w, last, var;
    uint32_t x;

    if ((last = cube.inword) != -1) {
        x = a[last] & b[last];
        if ((x = ~(x | x >> 1) & cube.inmask) != 0) {
            if ((dist = lincupl_popcount32(x)) > 1)
                return 2;
        }
        for (w = 1; w < last; w++) {
            x = a[w] & b[w];
            if ((x = ~(x | x >> 1) & ESP_DISJOINT) != 0) {
                if (dist == 1 || (dist += lincupl_popcount32(x)) > 1)
                    return 2;
            }
        }
    }
    for (var = cube.num_binary_vars; var < cube.num_vars; var++) {
        pcube mask = cube.var_mask[var];
        int hit = 0;

        last = cube.last_word[var];
        for (w = cube.first_word[var]; w <= last; w++) {
            if (a[w] & b[w] & mask[w]) {
                hit = 1;
                break;
            }
        }
        if (!hit && ++dist > 1)
            return 2;
    }
    return dist;
}

/* never seen running */
static pcube esp_force_lower(pcube xlower, pcube a, pcube b)
{
    int w, last, var;
    uint32_t x;

    if ((last = cube.inword) != -1) {
        x = a[last] & b[last];
        if ((x = ~(x | x >> 1) & cube.inmask) != 0)
            xlower[last] |= (x | (x << 1)) & a[last];
        for (w = 1; w < last; w++) {
            x = a[w] & b[w];
            if ((x = ~(x | x >> 1) & ESP_DISJOINT) != 0)
                xlower[w] |= (x | (x << 1)) & a[w];
        }
    }
    for (var = cube.num_binary_vars; var < cube.num_vars; var++) {
        pcube mask = cube.var_mask[var];
        int hit = 0;

        last = cube.last_word[var];
        for (w = cube.first_word[var]; w <= last; w++) {
            if (a[w] & b[w] & mask[w]) {
                hit = 1;
                break;
            }
        }
        if (!hit) {
            for (w = cube.first_word[var]; w <= last; w++)
                xlower[w] |= a[w] & mask[w];
        }
    }
    return xlower;
}

/* ascending set size, then ascending words from the last down */
static int esp_ascend(pset *a, pset *b)
{
    pset a1 = *a, b1 = *b;
    int i;

    if (ESP_SIZE(a1) > ESP_SIZE(b1))
        return 1;
    if (ESP_SIZE(a1) < ESP_SIZE(b1))
        return -1;
    i = ESP_LOOP(a1);
    do {
        if (a1[i] > b1[i])
            return 1;
        if (a1[i] < b1[i])
            return -1;
    } while (--i > 0);
    return 0;
}

/* order for the distance-1 merge: the words of the cubes with the
 * splitting variable (cube.temp[0]) filled in, from the last down;
 * descending */
static int esp_d1_order(pset *a, pset *b)
{
    pset a1 = *a, b1 = *b, c1 = cube.temp[0];
    int i = ESP_LOOP(a1);
    uint32_t x1, x2;

    do {
        if ((x1 = a1[i] | c1[i]) > (x2 = b1[i] | c1[i]))
            return -1;
        if (x1 < x2)
            return 1;
    } while (--i > 0);
    return 0;
}

static pset *esp_sf_sort(pcover A, EspCompare compare)
{
    pset *A1 = esp_malloc((long)(A->count + 1) * (long)sizeof(pset));
    pset *pdest = A1;
    pset p, last;

    for (p = A->data, last = p + (size_t)A->count * (size_t)A->wsize; p < last; p += A->wsize) {
        ESP_PUTSIZE(p, esp_set_ord(p));
        *pdest++ = p;
    }
    *pdest = NULL;
    lincupl_sort_sets(A1, A->count, compare);
    return A1;
}

static pset *esp_sf_list(pcover A)
{
    pset *A1 = esp_malloc((long)(A->count + 1) * (long)sizeof(pset));
    pset *pdest = A1;
    pset p, last;

    for (p = A->data, last = p + (size_t)A->count * (size_t)A->wsize; p < last; p += A->wsize)
        *pdest++ = p;
    *pdest = NULL;
    return A1;
}

static pcover esp_sf_unlist(pset *A1, int totcnt, int size)
{
    pcover R = esp_sf_new(totcnt, size);
    pset pr, p, *pa;

    R->count = totcnt;
    for (pr = R->data, pa = A1; (p = *pa++) != NULL; pr += R->wsize)
        esp_set_copy(pr, p);
    esp_free(A1);
    return R;
}

static int esp_rm_equal(pset *A1, EspCompare compare)
{
    pset *p, *pdest = A1;

    if (*A1 != NULL) {
        for (p = A1 + 1; *p != NULL; p++) {
            if (compare(p, p - 1) != 0)
                *pdest++ = *(p - 1);
        }
        *pdest++ = *(p - 1);
        *pdest = NULL;
    }
    return (int)(pdest - A1);
}

/* drop every set that contains a smaller set kept before */
static int esp_rm_rev_contain(pset *A1)
{
    pset *pa, *pb, *pcheck = A1, a, b;
    pset *pdest = A1;
    int last_size = -1;

    for (pa = A1; (a = *pa++) != NULL;) {
        int contains = 0;

        if (ESP_SIZE(a) != last_size) {
            last_size = ESP_SIZE(a);
            pcheck = pdest;
        }
        for (pb = A1; pb != pcheck;) {
            b = *pb++;
            if (esp_setp_implies(b, a)) {
                contains = 1;
                break;
            }
        }
        if (!contains)
            *pdest++ = a;
    }
    *pdest = NULL;
    return (int)(pdest - A1);
}

static pcover esp_sf_rev_contain(pcover A)
{
    pset *A1 = esp_sf_sort(A, esp_ascend);
    int cnt;
    pcover R;

    (void)esp_rm_equal(A1, esp_ascend);
    cnt = esp_rm_rev_contain(A1);
    R = esp_sf_unlist(A1, cnt, A->sf_size);
    esp_sf_free(A);
    return R;
}

/* ------------------------------------------------------------------ */
/* cofactor.c                                                          */
/* ------------------------------------------------------------------ */

static void esp_free_cubelist(pcube *T)
{
    esp_free(T[0]);
    esp_free(T);
}

/* the cubes of T that have a part of variable `var` in common
 * with c; the cofactor cube gets everything outside c */
static pcube *esp_scofactor(pcube *T, pcube c, int var)
{
    pcube *Tc, *Tc_save;
    pcube p, mask = cube.temp[1], *T1;
    int first = cube.first_word[var], last = cube.last_word[var];
    int listlen = ESP_CUBELISTSIZE(T) + 5;

    Tc_save = Tc = esp_malloc((long)listlen * (long)sizeof(pcube));
    *Tc++ = esp_set_or(esp_new_cube(), T[0], esp_set_diff(mask, cube.fullset, c));
    Tc++;
    (void)esp_set_and(mask, cube.var_mask[var], c);
    for (T1 = T + 2; (p = *T1++) != NULL;) {
        if (p != c) {
            int i = first;

            do {
                if (p[i] & mask[i]) {
                    *Tc++ = p;
                    break;
                }
            } while (++i <= last);
        }
    }
    *Tc++ = NULL;
    Tc_save[1] = (pcube)Tc;
    return Tc_save;
}

/* count the zeros of every column of the cube list and choose
 * the splitting variable (cdata.best): most active parts, then most zeros,
 * then the smallest larger half; the first such variable. */
static void esp_massive_count(pcube *T)
{
    int *count = cdata.part_zeros;
    pcube *T1, p, cof = T[0], full = cube.fullset;
    int var, i, lastbit, active, maxactive;
    int best = -1, mostactive = 0, mostzero = 0, mostbalanced = 32000;

    for (i = cube.size - 1; i >= 0; i--)
        count[i] = 0;
    for (T1 = T + 2; (p = *T1++) != NULL;) {
        for (i = ESP_LOOP(p); i > 0; i--) {
            uint32_t val = full[i] & ~(p[i] | cof[i]);
            int *cnt = count + ((i - 1) << 5);

            for (int b = 0; val != 0; b++, val >>= 1) {
                if (val & 1)
                    cnt[b]++;
            }
        }
    }

    cdata.vars_unate = cdata.vars_active = 0;
    cdata.best = -1;
    for (var = 0; var < cube.num_vars; var++) {
        if (var < cube.num_binary_vars) {
            i = count[var * 2];
            lastbit = count[var * 2 + 1];
            active = (i > 0) + (lastbit > 0);
            cdata.var_zeros[var] = i + lastbit;
            maxactive = i > lastbit ? i : lastbit;
        } else {
            maxactive = active = cdata.var_zeros[var] = 0;
            lastbit = cube.last_part[var];
            for (i = cube.first_part[var]; i <= lastbit; i++) {
                cdata.var_zeros[var] += count[i];
                active += (count[i] > 0);
                if (active > maxactive)
                    maxactive = active;
            }
        }
        if (active > mostactive) {
            best = var;
            mostactive = active;
            mostzero = cdata.var_zeros[best];
            mostbalanced = maxactive;
        } else if (active == mostactive) {
            if (cdata.var_zeros[var] > mostzero) {
                best = var;
                mostzero = cdata.var_zeros[best];
                mostbalanced = maxactive;
            } else if (cdata.var_zeros[var] == mostzero) {
                if (maxactive < mostbalanced) {
                    best = var;
                    mostbalanced = maxactive;
                }
            }
        }
        cdata.parts_active[var] = active;
        cdata.is_unate[var] = (active == 1);
        cdata.vars_active += (active > 0);
        cdata.vars_unate += (active == 1);
    }
    if (cube.num_vars > 0)
        cdata.best = best;
}

/* cleft gets the first half of the parts of the best variable
 * that are not yet in the cofactor cube, cright the rest */
static int esp_binate_split_select(pcube *T, pcube cleft, pcube cright)
{
    int best = cdata.best;
    int i, lastbit = cube.last_part[best], halfbit = 0;
    pcube cof = T[0];

    (void)esp_set_diff(cleft, cube.fullset, cube.var_mask[best]);
    (void)esp_set_diff(cright, cube.fullset, cube.var_mask[best]);
    for (i = cube.first_part[best]; i <= lastbit; i++) {
        if (!esp_is_in_set(cof, i))
            halfbit++;
    }
    for (i = cube.first_part[best], halfbit = halfbit / 2; halfbit > 0; i++) {
        if (!esp_is_in_set(cof, i)) {
            halfbit--;
            esp_set_insert(cleft, i);
        }
    }
    for (; i <= lastbit; i++) {
        if (!esp_is_in_set(cof, i))
            esp_set_insert(cright, i);
    }
    return best;
}

static pcube *esp_cube1list(pcover A)
{
    pcube *list = esp_malloc((long)(A->count + 3) * (long)sizeof(pcube));
    pcube *plist = list;
    pcube p, last;

    *plist++ = esp_new_cube();
    plist++;
    for (p = A->data, last = p + (size_t)A->count * (size_t)A->wsize; p < last; p += A->wsize)
        *plist++ = p;
    *plist++ = NULL;
    list[1] = (pcube)plist;
    return list;
}

static pcover esp_cubeunlist(pcube *A1)
{
    pcube p, pdest, cof = A1[0];
    pcover A = esp_sf_new(ESP_CUBELISTSIZE(A1), cube.size);

    for (int i = 2; (p = A1[i]) != NULL; i++) {
        pdest = ESP_GETSET(A, i - 2);
        esp_set_or(pdest, p, cof);
    }
    A->count = ESP_CUBELISTSIZE(A1);
    return A;
}

/* ------------------------------------------------------------------ */
/* unate.c                                                             */
/* ------------------------------------------------------------------ */

/* one column per active part of the (unate) cube list: a set
 * has the column where the cube has a zero */
static pcover esp_map_cover_to_unate(pcube *T)
{
    pcover A = esp_sf_new(ESP_CUBELISTSIZE(T), cdata.vars_unate);
    pcube p, pA, *T1;
    int ncol = 0;

    A->count = ESP_CUBELISTSIZE(T);
    pA = A->data;
    for (int i = 0; i < A->count; i++, pA += A->wsize)
        (void)esp_set_clear(pA, A->sf_size);

    for (int i = 0; i < cube.size; i++) {
        if (cdata.part_zeros[i] > 0) {
            if (ncol > cdata.vars_unate) {
                /* assert(ncol <= cdata.vars_unate); the expression printed
                 * is just "c" */
                fprintf(stderr, "Assertion failed: %s\n", "c");
                exit(-1);
            }
            pA = A->data;
            for (T1 = T + 2; (p = *T1++) != NULL;) {
                if (!esp_is_in_set(p, i))
                    esp_set_insert(pA, ncol);
                pA += A->wsize;
            }
            ncol++;
        }
    }
    return A;
}

static pcover esp_map_unate_to_cover(pcover A)
{
    pcover B = esp_sf_new(A->count, cube.size);
    int *unate = esp_malloc((long)cube.num_vars * 4);
    int nunate = 0;
    pcube p, pB, last;

    B->count = A->count;
    for (int var = 0; var < cube.num_vars; var++) {
        if (cdata.is_unate[var])
            unate[nunate++] = var;
    }
    pB = B->data;
    for (p = A->data, last = p + (size_t)A->count * (size_t)A->wsize; p < last; p += A->wsize) {
        esp_set_fill(pB, cube.size);
        for (int ncol = 0; ncol < nunate; ncol++) {
            if (esp_is_in_set(p, ncol)) {
                int lp = cube.last_part[unate[ncol]];

                for (int i = cube.first_part[unate[ncol]]; i <= lp; i++) {
                    if (cdata.part_zeros[i] == 0)
                        esp_set_remove(pB, i);
                }
            }
        }
        pB += B->wsize;
    }
    esp_free(unate);
    return B;
}

/* the sets of A that do not have column `pick` */
static pcover esp_abs_covered(pcover A, int pick)
{
    pcover B = esp_sf_new(A->count, A->sf_size);
    pset p, last, pdest = B->data;

    for (p = A->data, last = p + (size_t)A->count * (size_t)A->wsize; p < last; p += A->wsize) {
        if (!esp_is_in_set(p, pick)) {
            esp_set_copy(pdest, p);
            B->count++;
            pdest += B->wsize;
        }
    }
    return B;
}

/* the sets of A that have no column of pick_set */
static pcover esp_abs_covered_many(pcover A, pset pick_set)
{
    pcover B = esp_sf_new(A->count, A->sf_size);
    pset p, last, pdest = B->data;

    for (p = A->data, last = p + (size_t)A->count * (size_t)A->wsize; p < last; p += A->wsize) {
        if (esp_setp_disjoint(p, pick_set)) {
            esp_set_copy(pdest, p);
            B->count++;
            pdest += B->wsize;
        }
    }
    return B;
}

/* the column of `restrict` with the largest weighted count; the
 * first one among equals */
static int esp_abs_select_restricted(pcover A, pset restrict_set)
{
    int *count = esp_sf_count_restricted(A, restrict_set);
    int best_var = -1, best_count = 0;

    for (int i = 0; i < A->sf_size; i++) {
        if (count[i] > best_count) {
            best_var = i;
            best_count = count[i];
        }
    }
    esp_free(count);
    if (best_var == -1)
        esp_fatal("abs_select_restricted: should not have best_var == -1");
    return best_var;
}

/* all minimal covers of the rows of A (SIZE of every set must
 * hold its number of elements).  Disposes of A. */
static pcover esp_unate_complement(pcover A)
{
    pcover Abar;
    pset p, p1, restrict_set;
    int i, max_i, min_set_ord;

    if (A->count == 0) {
        int size = A->sf_size;

        esp_sf_free(A);
        Abar = esp_sf_new(1, size);
        (void)esp_set_clear(ESP_GETSET(Abar, Abar->count++), size);
    } else if (A->count == 1) {
        p = A->data;
        Abar = esp_sf_new(A->sf_size, A->sf_size);
        for (i = 0; i < A->sf_size; i++) {
            if (esp_is_in_set(p, i)) {
                p1 = esp_set_clear(ESP_GETSET(Abar, Abar->count++), A->sf_size);
                esp_set_insert(p1, i);
            }
        }
        esp_sf_free(A);
    } else {
        /* the columns of the smallest sets */
        restrict_set = esp_set_new(A->sf_size);
        min_set_ord = A->sf_size + 1;
        for (i = 0, p = A->data; i < A->count; i++, p += A->wsize) {
            if (ESP_SIZE(p) < min_set_ord) {
                esp_set_copy(restrict_set, p);
                min_set_ord = ESP_SIZE(p);
            } else if (ESP_SIZE(p) == min_set_ord) {
                esp_set_or(restrict_set, restrict_set, p);
            }
        }
        if (min_set_ord == 0) {
            A->count = 0;
            Abar = A;
        } else if (min_set_ord == 1) {
            /* essential columns */
            Abar = esp_unate_complement(esp_abs_covered_many(A, restrict_set));
            esp_sf_free(A);
            for (i = 0, p = Abar->data; i < Abar->count; i++, p += Abar->wsize)
                esp_set_or(p, p, restrict_set);
        } else {
            max_i = esp_abs_select_restricted(A, restrict_set);
            /* covers of the rows without max_i, with max_i added */
            Abar = esp_unate_complement(esp_abs_covered(A, max_i));
            for (i = 0, p = Abar->data; i < Abar->count; i++, p += Abar->wsize)
                esp_set_insert(p, max_i);
            /* covers of all rows without using max_i */
            for (i = 0, p = A->data; i < A->count; i++, p += A->wsize) {
                if (esp_is_in_set(p, max_i)) {
                    esp_set_remove(p, max_i);
                    ESP_PUTSIZE(p, ESP_SIZE(p) - 1);
                }
            }
            Abar = esp_sf_append(Abar, esp_unate_complement(A));
        }
        esp_free(restrict_set);
    }
    return Abar;
}

static pcover esp_unate_compl(pcover A)
{
    pset p, last;

    for (p = A->data, last = p + (size_t)A->count * (size_t)A->wsize; p < last; p += A->wsize)
        ESP_PUTSIZE(p, esp_set_ord(p));
    A = esp_unate_complement(A);
    A = esp_sf_rev_contain(A);
    return A;
}

/* ------------------------------------------------------------------ */
/* compl.c                                                             */
/* ------------------------------------------------------------------ */

static pcover esp_complement(pcube *T);

/* complement of a single cube (De Morgan) */
static pcover esp_compl_cube(pcube p)
{
    pcube diff = cube.temp[7], pdest, mask, full = cube.fullset;
    pcover R = esp_sf_new(cube.num_vars, cube.size);

    esp_set_diff(diff, full, p);
    for (int var = 0; var < cube.num_vars; var++) {
        mask = cube.var_mask[var];
        if (!esp_setp_disjoint(diff, mask)) {
            pdest = ESP_GETSET(R, R->count++);
            esp_set_merge(pdest, diff, full, mask);
        }
    }
    return R;
}

static int esp_compl_special_cases(pcube *T, pcover *Tbar)
{
    pcube *T1, p, ceil, cof = T[0];
    pcover A, ceil_compl;

    /* no cubes in the cover */
    if (T[2] == NULL) {
        *Tbar = esp_sf_addset(esp_sf_new(1, cube.size), cube.fullset);
        esp_free_cubelist(T);
        return ESP_TRUE;
    }
    /* a single cube */
    if (T[3] == NULL) {
        *Tbar = esp_compl_cube(esp_set_or(cof, cof, T[2]));
        esp_free_cubelist(T);
        return ESP_TRUE;
    }
    /* a row of all 1's: the complement is empty */
    for (T1 = T + 2; (p = *T1++) != NULL;) {
        if (esp_full_row(p, cof)) {
            *Tbar = esp_sf_new(0, cube.size);
            esp_free_cubelist(T);
            return ESP_TRUE;
        }
    }
    /* a column of all 0's can be factored out */
    ceil = esp_set_copy(esp_malloc((long)ESP_SET_SIZE(ESP_LOOP(cof) << 5) * 4), cof);
    for (T1 = T + 2; (p = *T1++) != NULL;) {
        int i = ESP_LOOP(ceil);

        do
            ceil[i] |= p[i];
        while (--i > 0);
    }
    if (!esp_setp_equal(ceil, cube.fullset)) {
        ceil_compl = esp_compl_cube(ceil);
        (void)esp_set_or(cof, cof, esp_set_diff(ceil, cube.fullset, ceil));
        esp_free(ceil);
        *Tbar = esp_sf_append(esp_complement(T), ceil_compl);
        return ESP_TRUE;
    }
    esp_free(ceil);

    esp_massive_count(T);

    /* a single active variable not factored out above: tautology */
    if (cdata.vars_active == 1) {
        *Tbar = esp_sf_new(0, cube.size);
        esp_free_cubelist(T);
        return ESP_TRUE;
    }
    /* unate cover */
    if (cdata.vars_unate == cdata.vars_active) {
        A = esp_map_cover_to_unate(T);
        esp_free_cubelist(T);
        A = esp_unate_compl(A);
        *Tbar = esp_map_unate_to_cover(A);
        esp_sf_free(A);
        return ESP_TRUE;
    }
    return ESP_MAYBE;
}

/* distance-1 merge in the splitting variable: equal cubes of
 * the two sorted arrays are merged into the left one */
static void esp_compl_d1merge(pcube *L1, pcube *R1)
{
    pcube pl, pr;

    for (pl = *L1, pr = *R1; pl != NULL && pr != NULL;) {
        switch (esp_d1_order(L1, R1)) {
        case 1:
            pr = *(++R1);
            break;
        case -1:
            pl = *(++L1);
            break;
        case 0:
            pr[0] &= ~ESP_ACTIVE;
            {
                int i = ESP_LOOP(pl);

                do
                    pl[i] |= pr[i];
                while (--i > 0);
            }
            pr = *(++R1);
            break;
        }
    }
}

/* raise the splitting variable of a cube of A1 completely when
 * the raised cube is contained in a single cube of the other branch */
static void esp_compl_lift(pcube *A1, pcube *B1, pcube bcube, int var)
{
    pcube a, b, *B2, lift = cube.temp[4], liftor = cube.temp[5];
    pcube mask = cube.var_mask[var];

    (void)esp_set_and(liftor, bcube, mask);
    for (; (a = *A1++) != NULL;) {
        if (a[0] & ESP_ACTIVE) {
            (void)esp_set_merge(lift, bcube, a, mask);
            for (B2 = B1; (b = *B2++) != NULL;) {
                if (!esp_setp_implies(lift, b))
                    continue;
                {
                    int i = ESP_LOOP(a);

                    do
                        a[i] |= liftor[i];
                    while (--i > 0);
                }
                break;
            }
        }
    }
}

/* raise the splitting variable of a cube of A1 when the raised
 * cube does not intersect the original ON-set T */
static void esp_compl_lift_onset(pcube *A1, pcover T, pcube bcube, int var)
{
    pcube a, last, p, lift = cube.temp[4], mask = cube.var_mask[var];

    for (; (a = *A1++) != NULL;) {
        if (a[0] & ESP_ACTIVE) {
            int hit = 0;

            esp_set_and(lift, bcube, mask);
            esp_set_or(lift, a, lift);
            for (p = T->data, last = p + (size_t)T->count * (size_t)T->wsize; p < last;
                 p += T->wsize) {
                if (esp_cdist0(p, lift)) {
                    hit = 1;
                    break;
                }
            }
            if (!hit) {
                esp_set_copy(a, lift);
                a[0] |= ESP_ACTIVE;
            }
        }
    }
}

/* never seen running: complement never asks for this kind of
 * lifting.  The work cube is freed after the loop. */
static void esp_compl_lift_onset_complex(pcube *A1, pcover T, int var)
{
    pcube last, p, a, xlower = esp_new_cube();

    for (; (a = *A1++) != NULL;) {
        if (a[0] & ESP_ACTIVE) {
            (void)esp_set_clear(xlower, cube.size);
            for (p = T->data, last = p + (size_t)T->count * (size_t)T->wsize; p < last;
                 p += T->wsize) {
                int dist = esp_cdist01(p, a);

                if (dist < 2) {
                    if (dist == 0)
                        esp_fatal("compl: ON-set and OFF-set are not orthogonal");
                    else
                        (void)esp_force_lower(xlower, p, a);
                }
            }
            (void)esp_set_diff(xlower, cube.var_mask[var], xlower);
            (void)esp_set_or(a, a, xlower);
        }
    }
    esp_free(xlower);
}

/* intersect the two half complements with their cofactor cubes,
 * merge cubes that are equal outside the splitting variable, lift, and
 * join the two covers (left cubes first) */
static pcover esp_compl_merge(pcube *T1, pcover L, pcover R, pcube cl, pcube cr, int var,
                              int lifting)
{
    pcube p, last, pt;
    pcover T, Tbar;
    pcube *L1, *R1;

    for (p = L->data, last = p + (size_t)L->count * (size_t)L->wsize; p < last; p += L->wsize) {
        int i = ESP_LOOP(p);

        do
            p[i] &= cl[i];
        while (--i > 0);
        p[0] |= ESP_ACTIVE;
    }
    for (p = R->data, last = p + (size_t)R->count * (size_t)R->wsize; p < last; p += R->wsize) {
        int i = ESP_LOOP(p);

        do
            p[i] &= cr[i];
        while (--i > 0);
        p[0] |= ESP_ACTIVE;
    }

    (void)esp_set_copy(cube.temp[0], cube.var_mask[var]);
    L1 = esp_sf_list(L);
    lincupl_sort_sets(L1, L->count, esp_d1_order);
    R1 = esp_sf_list(R);
    lincupl_sort_sets(R1, R->count, esp_d1_order);

    esp_compl_d1merge(L1, R1);

    switch (lifting) {
    case ESP_USE_COMPL_LIFT_ONSET:
        T = esp_cubeunlist(T1);
        esp_compl_lift_onset(L1, T, cr, var);
        esp_compl_lift_onset(R1, T, cl, var);
        esp_sf_free(T);
        break;
    case ESP_USE_COMPL_LIFT_ONSET_COMPLEX:
        T = esp_cubeunlist(T1);
        esp_compl_lift_onset_complex(L1, T, var);
        esp_compl_lift_onset_complex(R1, T, var);
        esp_sf_free(T);
        break;
    case ESP_USE_COMPL_LIFT:
        esp_compl_lift(L1, R1, cr, var);
        esp_compl_lift(R1, L1, cl, var);
        break;
    default:
        break;
    }
    esp_free(L1);
    esp_free(R1);

    Tbar = esp_sf_new(L->count + R->count, cube.size);
    pt = Tbar->data;
    for (p = L->data, last = p + (size_t)L->count * (size_t)L->wsize; p < last; p += L->wsize) {
        esp_set_copy(pt, p);
        Tbar->count++;
        pt += Tbar->wsize;
    }
    for (p = R->data, last = p + (size_t)R->count * (size_t)R->wsize; p < last; p += R->wsize) {
        if (p[0] & ESP_ACTIVE) {
            esp_set_copy(pt, p);
            Tbar->count++;
            pt += Tbar->wsize;
        }
    }
    esp_sf_free(L);
    esp_sf_free(R);
    return Tbar;
}

/* complement of the cube list T, which is disposed of */
static pcover esp_complement(pcube *T)
{
    pcube cl, cr;
    int best;
    pcover Tbar, Tl, Tr;
    int lifting;

    if (esp_compl_special_cases(T, &Tbar) == ESP_MAYBE) {
        cl = esp_new_cube();
        cr = esp_new_cube();
        best = esp_binate_split_select(T, cl, cr);
        Tl = esp_complement(esp_scofactor(T, cl, best));
        Tr = esp_complement(esp_scofactor(T, cr, best));
        if ((int)((unsigned)ESP_CUBELISTSIZE(T) * (unsigned)(Tr->count + Tl->count)) <
            (int)((unsigned)Tl->count * (unsigned)Tr->count))
            lifting = ESP_USE_COMPL_LIFT_ONSET;
        else
            lifting = ESP_USE_COMPL_LIFT;
        Tbar = esp_compl_merge(T, Tl, Tr, cl, cr, best, lifting);
        esp_free(cl);
        esp_free(cr);
        esp_free_cubelist(T);
    }
    return Tbar;
}

/* ------------------------------------------------------------------ */
/* the fitter's side                                                   */
/* ------------------------------------------------------------------ */

/* release what esp_cube_setup allocated */
static void esp_cube_release(void)
{
    if (cube.var_mask != NULL) {
        for (int i = 0; i < cube.num_vars; i++)
            esp_free(cube.var_mask[i]);
    }
    if (cube.temp != NULL) {
        for (int i = 0; i < ESP_CUBE_TEMP; i++)
            esp_free(cube.temp[i]);
    }
    esp_free(cube.first_part);
    esp_free(cube.last_part);
    esp_free(cube.part_size);
    esp_free(cube.first_word);
    esp_free(cube.last_word);
    esp_free(cube.binary_mask);
    esp_free(cube.mv_mask);
    esp_free(cube.var_mask);
    esp_free(cube.temp);
    esp_free(cube.fullset);
    esp_free(cube.emptyset);
    esp_free(cube.sparse);
    esp_free(cdata.part_zeros);
    esp_free(cdata.var_zeros);
    esp_free(cdata.parts_active);
    esp_free(cdata.is_unate);
    cube.first_part = cube.last_part = cube.part_size = NULL;
    cube.first_word = cube.last_word = cube.sparse = NULL;
    cube.binary_mask = cube.mv_mask = cube.fullset = cube.emptyset = NULL;
    cube.var_mask = cube.temp = NULL;
    cdata.part_zeros = cdata.var_zeros = cdata.parts_active = cdata.is_unate = NULL;
    while (g_set_family_garbage != NULL) {
        EspSetFamily *next = g_set_family_garbage->next;

        free(g_set_family_garbage);
        g_set_family_garbage = next;
    }
}

void min_esp_reset(void)
{
    esp_cube_release();
}

/* the cube structure for the entity seen as a PLA: one binary
 * variable per input, local and output of the entity, and one
 * multiple-valued variable with one part per local and output (Espresso's
 * cube_setup follows the first lines). */
static void esp_cube_setup(FitEntity *entity)
{
    int n_in = min_count(entity->inputs);
    int n_local = min_count(entity->locals);
    int n_out = min_count(entity->outputs);
    int var;

    cube.num_binary_vars = n_out + n_local + n_in;
    cube.num_vars = cube.num_binary_vars + 1;
    cube.part_size = esp_malloc((long)cube.num_vars * 4);
    cube.part_size[cube.num_vars - 1] = n_local + n_out;

    if (cube.num_binary_vars < 0 || cube.num_vars < cube.num_binary_vars)
        fprintf(g_msg, FIT_EXT ? "Error : in Fct->ChLInNames or Fct->ChLOutNames, cube size is silly\n"
                                 : "## Error in Fct->ChLInNames or Fct->ChLOutNames, cube size is silly\n");
    cube.num_mv_vars = cube.num_vars - cube.num_binary_vars;
    cube.output = cube.num_mv_vars > 0 ? cube.num_vars - 1 : -1;

    cube.size = 0;
    cube.first_part = esp_malloc((long)cube.num_vars * 4);
    cube.last_part = esp_malloc((long)cube.num_vars * 4);
    cube.first_word = esp_malloc((long)cube.num_vars * 4);
    cube.last_word = esp_malloc((long)cube.num_vars * 4);
    for (var = 0; var < cube.num_vars; var++) {
        int size;

        if (var < cube.num_binary_vars)
            cube.part_size[var] = 2;
        cube.first_part[var] = cube.size;
        cube.first_word[var] = ESP_WHICH_WORD(cube.size);
        size = cube.part_size[var];
        cube.size += size < 0 ? -size : size;
        cube.last_part[var] = cube.size - 1;
        cube.last_word[var] = ESP_WHICH_WORD(cube.size - 1);
    }

    cube.var_mask = esp_malloc((long)cube.num_vars * (long)sizeof(pset));
    cube.sparse = esp_malloc((long)cube.num_vars * 4);
    cube.binary_mask = esp_new_cube();
    cube.mv_mask = esp_new_cube();
    for (var = 0; var < cube.num_vars; var++) {
        pset mask = cube.var_mask[var] = esp_new_cube();

        for (int i = cube.first_part[var]; i <= cube.last_part[var]; i++)
            esp_set_insert(mask, i);
        if (var < cube.num_binary_vars) {
            (void)esp_set_or(cube.binary_mask, cube.binary_mask, mask);
            cube.sparse[var] = 0;
        } else {
            (void)esp_set_or(cube.mv_mask, cube.mv_mask, mask);
            cube.sparse[var] = 1;
        }
    }
    if (cube.num_binary_vars == 0) {
        cube.inword = -1;
    } else {
        cube.inword = cube.last_word[cube.num_binary_vars - 1];
        cube.inmask = cube.binary_mask[cube.inword] & ESP_DISJOINT;
    }

    cube.temp = esp_malloc((long)ESP_CUBE_TEMP * (long)sizeof(pset));
    for (int i = 0; i < ESP_CUBE_TEMP; i++)
        cube.temp[i] = esp_new_cube();
    cube.fullset = esp_set_fill(esp_new_cube(), cube.size);
    cube.emptyset = esp_new_cube();

    cdata.part_zeros = esp_malloc((long)cube.size * 4);
    cdata.var_zeros = esp_malloc((long)cube.num_vars * 4);
    cdata.parts_active = esp_malloc((long)cube.num_vars * 4);
    cdata.is_unate = esp_malloc((long)cube.num_vars * 4);

    for (var = 0; var < cube.num_vars; var++) {
        int size = cube.part_size[var];

        cube.part_size[var] = size < 0 ? -size : size;
    }
}

/* every FitPTermIO of the PLA as two strings (input part, output
 * part), appended alternately to the result */
static FitList *esp_pla_to_strings(FitEntity *entity, FitList *pla)
{
    int n_in = min_count(entity->inputs) + min_count(entity->locals) + min_count(entity->outputs);
    int n_out = min_count(entity->locals) + min_count(entity->outputs);
    FitList *strings = list_create();

    for (int i = 0; i < min_count(pla); i++) {
        char *in = util_calloc(1, (size_t)n_in + 1);
        char *out = util_calloc(1, (size_t)n_out + 1);
        FitPTermIO *p = pla->items[i];

        pterm_to_string(&p->in, (unsigned)n_in, in, 1);
        pterm_to_string(&p->out, (unsigned)n_out, out, 1);
        list_add(strings, in);
        list_add(strings, out);
    }
    return strings;
}

/* the cover of the PLA given as string pairs.  Input characters:
 * '0' '1', '-' or '2' (both), '?' (neither); output characters '1' or '4'
 * set the part, '0' '-' '2' '3' '~' leave it clear. */
static pcover esp_strings_to_cover(FitList *strings)
{
    pcover F = esp_sf_new_plain(10, cube.size);

    for (int k = 0; k < min_count(strings); k += 2) {
        pcube c = esp_new_cube();
        const char *in = strings->items[k];
        const char *out = strings->items[k + 1];
        int v, b;

        for (v = 0; v < cube.num_binary_vars; v++) {
            switch (in[v]) {
            case '-':
            case '2':
                esp_set_insert(c, 2 * v + 1);
                esp_set_insert(c, 2 * v);
                break;
            case '0':
                esp_set_insert(c, 2 * v);
                break;
            case '1':
                esp_set_insert(c, 2 * v + 1);
                break;
            case '?':
                break;
            default:
                fprintf(g_msg, "## Error in transformation cube to input string\n");
                break;
            }
        }
        for (b = v * 2; b <= cube.size - 1; b++) {
            switch (out[b - v * 2]) {
            case '-':
            case '0':
            case '2':
            case '3':
            case '~':
                break;
            case '1':
            case '4':
                esp_set_insert(c, b);
                break;
            default:
                fprintf(g_msg, "## Error in transformation cube to output string\n");
                break;
            }
        }
        F = esp_sf_addset(F, c);
        esp_free(c);
    }
    return F;
}

/* the reverse: two strings per cube.  An input with neither
 * part set gives a NUL character, which ends the string early. */
static FitList *esp_cover_to_strings(pcover R)
{
    static const char in_chars[4] = { '?', '0', '1', '-' };
    FitList *strings = list_create();
    pcube p, last;

    for (p = R->data, last = p + (size_t)R->count * (size_t)R->wsize; p < last; p += R->wsize) {
        char *in = util_calloc(1, (size_t)cube.num_binary_vars + 1);
        int first = cube.first_part[cube.output];
        int lastpart = cube.last_part[cube.output];
        char *out;

        for (int v = 0; v < cube.num_binary_vars; v++) {
            char ch = in_chars[ESP_GETINPUT(p, v)];

            if (ch == '-' || ch == '0' || ch == '1')
                in[v] = ch;
        }
        out = util_calloc(1, (size_t)(lastpart - first + 2));
        for (int b = first; b <= lastpart; b++)
            out[b - first] = esp_is_in_set(p, b) ? '1' : '0';
        list_add(strings, in);
        list_add(strings, out);
    }
    return strings;
}

/* string pairs back to a list of FitPTermIO; the strings are
 * freed */
static FitList *esp_strings_to_pla(FitEntity *entity, FitList *strings)
{
    unsigned n_in = (unsigned)(min_count(entity->inputs) + min_count(entity->locals) +
                               min_count(entity->outputs));
    unsigned n_out = (unsigned)(min_count(entity->locals) + min_count(entity->outputs));
    int nw_in = min_words(n_in);
    int nw_out = min_words(n_out);
    FitList *pla = list_create();

    for (int k = 0; k < min_count(strings); k += 2) {
        char *in = strings->items[k];
        char *out = strings->items[k + 1];
        FitPTermIO *p = ptermio_new(nw_in, nw_out);

        ptermio_build(p, in, out, n_in, n_out);
        list_add(pla, p);
        free(in);
        free(out);
    }
    return pla;
}

/* entity with the variables of `entity` and the functions of
 * the PLA */
static FitEntity *esp_pla_to_entity(FitEntity *entity, FitList *pla)
{
    FitEntity *e = entity_new(NULL);

    e->n_in = entity->n_in;
    e->n_out = entity->n_out;
    e->n_local = entity->n_local;
    e->inputs = list_map_copy(entity->inputs, min_copy_var_cb);
    e->outputs = list_map_copy(entity->outputs, min_copy_var_cb);
    e->locals = list_map_copy(entity->locals, min_copy_var_cb);
    tt2_fct_from_pla(pla, e);
    tt2_delete_unused_cycles(e);
    return e;
}

FitEntity *esp_complement_entity(FitEntity *entity)
{
    FitList *pla = tt2_pla_from_fct(entity, 0);
    FitList *strings = esp_pla_to_strings(entity, pla);
    FitList *compl_strings, *compl_pla;
    FitEntity *compl_entity, *result;
    pcover F, R;

    esp_cube_release();
    esp_cube_setup(entity);
    F = esp_strings_to_cover(strings);
    R = esp_complement(esp_cube1list(F));
    compl_strings = esp_cover_to_strings(R);
    compl_pla = esp_strings_to_pla(entity, compl_strings);
    compl_entity = esp_pla_to_entity(entity, compl_pla);

    /* release the terms, the strings, the covers and the cube structure */
    if (pla != NULL)
        list_free_all(&pla, min_free_ptermio_cb);
    list_free_all(&strings, min_free_block_cb);
    list_destroy(&compl_strings);
    list_free_all(&compl_pla, min_free_ptermio_cb);
    esp_sf_free(F);
    esp_sf_free(R);
    esp_cube_release();

    result = domlist_minimise_entity(compl_entity, 1, 1, 0x32);
    entity_free(&compl_entity);
    return result;
}

int esp_signal_is_control(const char *name, FitEntity *entity)
{
    FitList *items;

    if (entity->net == NULL || entity->net->items == NULL)
        return 0;
    items = entity->net->items;
    for (int i = 0; i < min_count(items); i++) {
        FitNetItem *it = items->items[i];

        if (it->kind == 3 || it->kind == 2 || it->kind == 8 || it->kind == 4) {
            FitInstance *c = it->cell;

            if (c->clk != NULL && util_str_icmp(name, c->clk) == 0)
                return 1;
            if (c->ar != NULL && util_str_icmp(name, c->ar) == 0)
                return 1;
            if (c->ap != NULL && util_str_icmp(name, c->ap) == 0)
                return 1;
            if (c->oe != NULL && util_str_icmp(name, c->oe) == 0)
                return 1;
            if (c->ce != NULL && util_str_icmp(name, c->ce) == 0)
                return 1;
        }
    }
    return 0;
}

int esp_var_is_simple_control(FitEntity *entity, FitVariable *var)
{
    FitFct *f;
    FitFctTree *t;

    if (!esp_signal_is_control(var->name, entity))
        return 0;
    f = entity_find_fct(entity, var->id);
    t = f->tree[0];
    if (t->sel == FIT_SEL_OR && t->u.children != NULL && t->u.children->count > 1)
        return 0;
    return 1;
}
