/*
 * minimize_cube.c - sets, set families and cubes of the minimize phase
 * (levels 2 and 3).
 *
 * Set families with a free list of released headers, the cube operations
 * of the multiple-valued cube algebra, the sort the minimizer depends on,
 * containment and the description of the cube structure.  The
 * element-level set operations are inline functions in minimize_int.h.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "minimize_int.h"

cube_struct_t minimize_cube;
cdata_t minimize_cdata;
static set_family_t *g_sf_free;        /* released set family headers */

/* =========================================================================
 * Set families (Espresso set.c)
 * ========================================================================= */

/* The header of a released family is reused, so active_count
 * keeps its previous value. */
pcover minimize_sf_new(int num, int size)
{
    pcover A;
    if (g_sf_free != NULL) {
        A = g_sf_free;
        g_sf_free = A->next;
    } else {
        A = minimize_work_alloc(sizeof(*A));
    }
    A->sf_size = size;
    A->wsize = set_words(size);
    A->capacity = num;
    A->data = minimize_work_alloc((size_t)(num > 0 ? num : 0) * (size_t)A->wsize * sizeof(uint32_t));
    A->count = 0;
    return A;
}

pcover minimize_new_cover(int num)
{
    return minimize_sf_new(num, minimize_cube.size);
}

void minimize_sf_free(pcover A)
{
    minimize_work_free(A->data);
    A->data = NULL;
    A->next = g_sf_free;
    g_sf_free = A;
}

static pcover sf_copy(pcover R, pcover A)
{
    R->count = A->count;
    if (A->count > 0) {
        memcpy(R->data, A->data, (size_t)A->count * (size_t)A->wsize * sizeof(uint32_t));
    }
    return R;
}

pcover minimize_sf_save(pcover A)
{
    return sf_copy(minimize_sf_new(A->count, A->sf_size), A);
}

static pcover sf_join(pcover A, pcover B)
{
    pcover R = minimize_sf_new(A->count + B->count, A->sf_size);
    size_t a_words = (size_t)A->count * (size_t)A->wsize;
    size_t b_words = (size_t)B->count * (size_t)B->wsize;
    R->count = A->count + B->count;
    if (a_words > 0) {
        memcpy(R->data, A->data, a_words * sizeof(uint32_t));
    }
    if (b_words > 0) {
        memcpy(R->data + a_words, B->data, b_words * sizeof(uint32_t));
    }
    return R;
}

/* append B to A and release B */
pcover minimize_sf_append(pcover A, pcover B)
{
    if (A->capacity < A->count + B->count) {
        pcover R = sf_join(A, B);
        minimize_sf_free(A);
        A = R;
    } else {
        size_t b_words = (size_t)B->count * (size_t)B->wsize;
        if (b_words > 0) {
            memcpy(GETSET(A, A->count), B->data, b_words * sizeof(uint32_t));
        }
        A->count += B->count;
    }
    minimize_sf_free(B);
    return A;
}

/* Grows the family by one set at a time when it is full. */
pcover minimize_sf_addset(pcover A, const uint32_t *s)
{
    if (A->capacity <= A->count) {
        pcover R = sf_copy(minimize_sf_new(A->count + 1, A->sf_size), A);
        minimize_sf_free(A);
        A = R;
    }
    set_copy(GETSET(A, A->count++), s);
    return A;
}

pcover minimize_sf_active(pcover A)
{
    pset p, last;
    foreach_set(A, last, p) {
        SET(p, SET_ACTIVE);
    }
    A->active_count = A->count;
    return A;
}

/* drop the sets that are not active */
pcover minimize_sf_inactive(pcover A)
{
    pset p, last, pdest = A->data;
    foreach_set(A, last, p) {
        if (TESTP(p, SET_ACTIVE)) {
            if (pdest != p) {
                set_copy(pdest, p);
            }
            pdest += A->wsize;
        } else {
            A->count--;
        }
    }
    return A;
}

/* =========================================================================
 * Cube operations (Espresso setc.c)
 * ========================================================================= */

/* For every binary variable: bit 2v is set when the intersection in v is
 * empty; the odd bits are a by-product. */
static uint32_t empty_pairs(uint32_t x)
{
    return ~(x | (x >> 1));
}

/* p together with the cofactor cube covers everything */
int minimize_full_row(const uint32_t *p, const uint32_t *cof)
{
    int i = LOOP(p);
    do {
        if ((p[i] | cof[i]) != minimize_cube.fullset[i]) {
            return 0;
        }
    } while (--i > 0);
    return 1;
}

/* a and b intersect in the multiple-valued variable var */
static int mv_intersects(const uint32_t *a, const uint32_t *b, int var)
{
    const uint32_t *mask = minimize_cube.var_mask[var];
    for (int w = minimize_cube.first_word[var]; w <= minimize_cube.last_word[var]; w++) {
        if ((a[w] & b[w] & mask[w]) != 0) {
            return 1;
        }
    }
    return 0;
}

/* a and b intersect */
int minimize_cdist0(const uint32_t *a, const uint32_t *b)
{
    int last = minimize_cube.inword;
    if (last != -1) {
        if ((empty_pairs(a[last] & b[last]) & minimize_cube.inmask) != 0) {
            return 0;
        }
        for (int w = 1; w < last; w++) {
            if ((empty_pairs(a[w] & b[w]) & 0x55555555u) != 0) {
                return 0;
            }
        }
    }
    for (int var = minimize_cube.num_binary_vars; var < minimize_cube.num_vars; var++) {
        if (!mv_intersects(a, b, var)) {
            return 0;
        }
    }
    return 1;
}

/* a and b both depend on some common variable */
int minimize_ccommon(const uint32_t *a, const uint32_t *b)
{
    int last = minimize_cube.inword;
    if (last != -1) {
        if ((~(a[last] & (a[last] >> 1)) & ~(b[last] & (b[last] >> 1)) & minimize_cube.inmask) != 0) {
            return 1;
        }
        for (int w = 1; w < last; w++) {
            if ((~(a[w] & (a[w] >> 1)) & ~(b[w] & (b[w] >> 1)) & 0x55555555u) != 0) {
                return 1;
            }
        }
    }
    for (int var = minimize_cube.num_binary_vars; var < minimize_cube.num_vars; var++) {
        const uint32_t *mask = minimize_cube.var_mask[var];
        int first = minimize_cube.first_word[var];
        int lastw = minimize_cube.last_word[var];
        int a_active = 0;
        for (int w = first; w <= lastw && !a_active; w++) {
            a_active = (~a[w] & mask[w]) != 0;
        }
        if (a_active) {
            for (int w = first; w <= lastw; w++) {
                if ((~b[w] & mask[w]) != 0) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* larger sets first */
static int descend(pset *a, pset *b)
{
    const uint32_t *a1 = *a, *b1 = *b;
    if (SIZE(a1) > SIZE(b1)) {
        return -1;
    }
    if (SIZE(a1) < SIZE(b1)) {
        return 1;
    }
    int i = LOOP(a1);
    do {
        if (a1[i] > b1[i]) {
            return -1;
        }
        if (a1[i] < b1[i]) {
            return 1;
        }
    } while (--i > 0);
    return 0;
}

/* compare word by word from the last word down; larger first */
static int lex_order(pset *a, pset *b)
{
    const uint32_t *a1 = *a, *b1 = *b;
    int i = LOOP(a1);
    do {
        if (a1[i] > b1[i]) {
            return -1;
        }
        if (a1[i] < b1[i]) {
            return 1;
        }
    } while (--i > 0);
    return 0;
}

/* order ignoring the variable whose mask is in minimize_cube.temp[0] */
int minimize_d1_order(pset *a, pset *b)
{
    const uint32_t *a1 = *a, *b1 = *b, *ignore = minimize_cube.temp[0];
    int i = LOOP(a1);
    do {
        uint32_t x1 = a1[i] | ignore[i];
        uint32_t x2 = b1[i] | ignore[i];
        if (x1 > x2) {
            return -1;
        }
        if (x1 < x2) {
            return 1;
        }
    } while (--i > 0);
    return 0;
}

/* =========================================================================
 * Containment (Espresso contain.c)
 * ========================================================================= */

/* NULL terminated array of pointers to the sets of A */
pset *minimize_sf_list(pcover A)
{
    pset *A1 = minimize_work_alloc((size_t)(A->count + 1) * sizeof(pset));
    pset *pdest = A1;
    pset p, last;
    foreach_set(A, last, p) {
        *pdest++ = p;
    }
    *pdest = NULL;
    return A1;
}

/* the same, sorted, with the number of elements as size */
static pset *sf_sort(pcover A, set_compare_t compare)
{
    pset *A1 = minimize_work_alloc((size_t)(A->count + 1) * sizeof(pset));
    pset *pdest = A1;
    pset p, last;
    foreach_set(A, last, p) {
        put_size(p, (unsigned)set_ord(p));
        *pdest++ = p;
    }
    *pdest = NULL;
    lincupl_sort_sets(A1, A->count, compare);
    return A1;
}

/* new family from the first totcnt pointers; releases A1 */
static pcover sf_unlist(pset *A1, int totcnt, int size)
{
    pcover R = minimize_sf_new(totcnt, size);
    pset pdest = R->data;
    R->count = totcnt;
    for (pset *pa = A1; *pa != NULL; pa++) {
        set_copy(pdest, *pa);
        pdest += R->wsize;
    }
    minimize_work_free(A1);
    return R;
}

/* remove duplicates from a sorted list */
static int rm_equal(pset *A1, set_compare_t compare)
{
    pset *pdest = A1;
    if (*A1 != NULL) {
        pset *p;
        for (p = A1; p[1] != NULL; p++) {
            if (compare(p, p + 1) != 0) {
                *pdest++ = *p;
            }
        }
        *pdest++ = *p;
        *pdest = NULL;
    }
    return (int)(pdest - A1);
}

/* remove the sets contained in a larger one (list sorted by size) */
static int rm_contain(pset *A1)
{
    pset *pdest = A1, *pcheck = A1;
    uint32_t last_size = ~0u;
    pset a;
    for (pset *pa = A1; (a = *pa) != NULL; pa++) {
        if (SIZE(a) != last_size) {
            last_size = SIZE(a);
            pcheck = pdest;
        }
        int contained = 0;
        for (pset *pb = A1; pb != pcheck && !contained; pb++) {
            contained = setp_implies(a, *pb);
        }
        if (!contained) {
            *pdest++ = a;
        }
    }
    *pdest = NULL;
    return (int)(pdest - A1);
}

/* move the sets that are in both sorted lists to E1 */
static int rm2_equal(pset *A1, pset *B1, pset *E1, set_compare_t compare)
{
    pset *pda = A1, *pdb = B1, *pde = E1;
    while (*A1 != NULL && *B1 != NULL) {
        int order = compare(A1, B1);
        if (order == -1) {
            *pda++ = *A1++;
        } else if (order == 0) {
            *pde++ = *A1++;
            B1++;
        } else if (order == 1) {
            *pdb++ = *B1++;
        }
    }
    while (*A1 != NULL) {
        *pda++ = *A1++;
    }
    while (*B1 != NULL) {
        *pdb++ = *B1++;
    }
    *pda = *pdb = *pde = NULL;
    return (int)(pde - E1);
}

/* remove from A1 the sets contained in a larger set of B1 */
static int rm2_contain(pset *A1, pset *B1)
{
    pset *pdest = A1;
    pset a, b;
    for (pset *pa = A1; (a = *pa) != NULL; pa++) {
        int contained = 0;
        for (pset *pb = B1; (b = *pb) != NULL && SIZE(b) > SIZE(a) && !contained; pb++) {
            contained = setp_implies(a, b);
        }
        if (!contained) {
            *pdest++ = a;
        }
    }
    *pdest = NULL;
    return (int)(pdest - A1);
}

static int desc1(pset a, pset b)
{
    return descend(&a, &b);
}

/* merge three sorted lists into a new family; releases the lists */
static pcover sf_merge(pset *A1, pset *B1, pset *E1, int totcnt, int size)
{
    pcover R = minimize_sf_new(totcnt, size);
    pset pdest = R->data;
    pset *pa = A1, *pb = B1, *pe = E1;
    R->count = totcnt;
    for (;;) {
        pset a = *pa, b = *pb, e = *pe, take;
        if (a == NULL && b == NULL && e == NULL) {
            break;
        }
        if (a == NULL) {
            if (b == NULL || (e != NULL && desc1(e, b) < 0)) {
                take = e;
            } else {
                take = b;
            }
        } else if (b != NULL && desc1(b, a) < 0) {
            take = (e != NULL && desc1(e, b) < 0) ? e : b;
        } else if (e != NULL && desc1(e, a) < 0) {
            take = e;
        } else {
            take = a;
        }
        if (take == e) {
            pe++;
        } else if (take == b) {
            pb++;
        } else {
            pa++;
        }
        set_copy(pdest, take);
        pdest += R->wsize;
    }
    minimize_work_free(A1);
    minimize_work_free(B1);
    minimize_work_free(E1);
    return R;
}

/* remove duplicate and contained sets; releases A */
pcover minimize_sf_contain(pcover A)
{
    pset *A1 = sf_sort(A, descend);
    rm_equal(A1, descend);
    int cnt = rm_contain(A1);
    pcover R = sf_unlist(A1, cnt, A->sf_size);
    minimize_sf_free(A);
    return R;
}

/* union of two families that are sorted and free of containment */
pcover minimize_sf_union(pcover A, pcover B)
{
    pset *A1 = minimize_sf_list(A);
    pset *B1 = minimize_sf_list(B);
    int larger = A->count < B->count ? B->count : A->count;
    pset *E1 = minimize_work_alloc((size_t)(larger + 1) * sizeof(pset));
    int cnt = rm2_equal(A1, B1, E1, descend);
    cnt += rm2_contain(A1, B1);
    cnt += rm2_contain(B1, A1);
    pcover R = sf_merge(A1, B1, E1, cnt, A->sf_size);
    minimize_sf_free(A);
    minimize_sf_free(B);
    return R;
}

pcover minimize_lex_sort(pcover T)
{
    pcover R = sf_unlist(sf_sort(T, lex_order), T->count, T->sf_size);
    minimize_sf_free(T);
    return R;
}

pcover minimize_size_sort(pcover T)
{
    pcover R = sf_unlist(sf_sort(T, descend), T->count, T->sf_size);
    minimize_sf_free(T);
    return R;
}

/* =========================================================================
 * Cube structure (Espresso cubestr.c)
 * ========================================================================= */

/* derive the cube description from num_vars, num_binary_vars and
 * part_size[] */
void minimize_cube_setup(void)
{
    size_t nv = (size_t)minimize_cube.num_vars;
    minimize_cube.size = 0;
    minimize_cube.first_part = minimize_work_alloc(nv * sizeof(int));
    minimize_cube.last_part = minimize_work_alloc(nv * sizeof(int));
    minimize_cube.first_word = minimize_work_alloc(nv * sizeof(int));
    minimize_cube.last_word = minimize_work_alloc(nv * sizeof(int));
    for (int var = 0; var < minimize_cube.num_vars; var++) {
        if (var < minimize_cube.num_binary_vars) {
            minimize_cube.part_size[var] = 2;
        }
        minimize_cube.first_part[var] = minimize_cube.size;
        minimize_cube.first_word[var] = (minimize_cube.size >> 5) + 1;
        minimize_cube.size += abs(minimize_cube.part_size[var]);
        minimize_cube.last_part[var] = minimize_cube.size - 1;
        minimize_cube.last_word[var] = ((minimize_cube.size - 1) >> 5) + 1;
    }

    minimize_cube.var_mask = minimize_work_alloc(nv * sizeof(pset));
    minimize_cube.sparse = minimize_work_alloc(nv * sizeof(int));
    minimize_cube.binary_mask = new_cube();
    minimize_cube.mv_mask = new_cube();
    for (int var = 0; var < minimize_cube.num_vars; var++) {
        pset mask = minimize_cube.var_mask[var] = new_cube();
        for (int i = minimize_cube.first_part[var]; i <= minimize_cube.last_part[var]; i++) {
            set_insert(mask, i);
        }
        if (var < minimize_cube.num_binary_vars) {
            set_or(minimize_cube.binary_mask, minimize_cube.binary_mask, mask);
            minimize_cube.sparse[var] = 0;
        } else {
            set_or(minimize_cube.mv_mask, minimize_cube.mv_mask, mask);
            minimize_cube.sparse[var] = 1;
        }
    }
    if (minimize_cube.num_binary_vars == 0) {
        minimize_cube.inword = -1;
    } else {
        minimize_cube.inword = minimize_cube.last_word[minimize_cube.num_binary_vars - 1];
        minimize_cube.inmask = minimize_cube.binary_mask[minimize_cube.inword] & 0x55555555u;
    }

    minimize_cube.temp = minimize_work_alloc(CUBE_TEMP * sizeof(pset));
    for (int i = 0; i < CUBE_TEMP; i++) {
        minimize_cube.temp[i] = new_cube();
    }
    minimize_cube.fullset = set_fill(new_cube(), minimize_cube.size);
    minimize_cube.emptyset = new_cube();

    minimize_cdata.part_zeros = minimize_work_alloc((size_t)minimize_cube.size * sizeof(int));
    minimize_cdata.var_zeros = minimize_work_alloc(nv * sizeof(int));
    minimize_cdata.parts_active = minimize_work_alloc(nv * sizeof(int));
    minimize_cdata.is_unate = minimize_work_alloc(nv * sizeof(int));
}

/* Forget the released set family headers; their memory belongs to the
 * working memory of a frame. */
void minimize_sf_forget(void)
{
    g_sf_free = NULL;
}

/* Reset the cube structure for a new run of the phase. */
void minimize_cube_reset(void)
{
    g_sf_free = NULL;
    memset(&minimize_cube, 0, sizeof(minimize_cube));
    memset(&minimize_cdata, 0, sizeof(minimize_cdata));
}
