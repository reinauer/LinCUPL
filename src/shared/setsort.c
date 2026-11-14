/*
 * setsort.c - the sort of the Espresso minimizers (see setsort.h)
 */
#include "setsort.h"

#include <stddef.h>
#include <string.h>

typedef uint32_t *set_t;

#define SORT_THRESH 4       /* below this an insertion sort is used */
#define SORT_MTHRESH 6      /* from this size on the median of three is taken */

static void swap_sets(set_t *a, set_t *b)
{
    set_t t = *a;
    *a = *b;
    *b = t;
}

/* quicksort of [base, max), down to parts smaller than SORT_THRESH */
static void sort_partition(set_t *base, set_t *max, lincupl_set_compare_t compare)
{
    ptrdiff_t lo = max - base;
    ptrdiff_t hi;
    do {
        set_t *mid, *i, *j, *jj, *tmp;
        mid = i = base + (lo >> 1);
        if (lo >= SORT_MTHRESH) {
            /* the median of the first, middle and last element */
            jj = base;
            j = compare(jj, i) > 0 ? jj : i;
            tmp = max - 1;
            if (compare(j, tmp) > 0) {
                j = (j == jj) ? i : jj;
                if (compare(j, tmp) < 0) {
                    j = tmp;
                }
            }
            if (j != i) {
                swap_sets(i, j);
            }
        }
        for (i = base, j = max - 1;;) {
            while (i < mid && compare(i, mid) <= 0) {
                i++;
            }
            int found = 0;
            while (j > mid) {
                if (compare(mid, j) <= 0) {
                    j--;
                    continue;
                }
                tmp = i + 1;
                if (i == mid) {
                    mid = jj = j;
                } else {
                    jj = j;
                    j--;
                }
                found = 1;
                break;
            }
            if (!found) {
                if (i == mid) {
                    break;
                }
                jj = mid;
                tmp = mid = i;
                j--;
            }
            swap_sets(i, jj);
            i = tmp;
        }
        /* the smaller part by recursion, the larger one by iteration */
        j = mid;
        i = mid + 1;
        lo = j - base;
        hi = max - i;
        if (lo <= hi) {
            if (lo >= SORT_THRESH) {
                sort_partition(base, j, compare);
            }
            base = i;
            lo = hi;
        } else {
            if (hi >= SORT_THRESH) {
                sort_partition(i, max, compare);
            }
            max = j;
        }
    } while (lo >= SORT_THRESH);
}

void lincupl_sort_sets(uint32_t **base, int n, lincupl_set_compare_t compare)
{
    if (n <= 1) {
        return;
    }
    set_t *max = base + n;
    set_t *hi;
    if (n >= SORT_THRESH) {
        sort_partition(base, max, compare);
        hi = base + SORT_THRESH;
    } else {
        hi = max;
    }
    /* the smallest element is among the first few; make it a sentinel */
    set_t *smallest = base;
    for (set_t *lo = base + 1; lo < hi; lo++) {
        if (compare(smallest, lo) > 0) {
            smallest = lo;
        }
    }
    if (smallest != base) {
        swap_sets(smallest, base);
    }
    for (set_t *min = base + 1; min < max; min++) {
        hi = min - 1;
        while (compare(hi, min) > 0 && hi > base) {
            hi--;
        }
        if (compare(hi, min) <= 0) {
            hi++;
        }
        if (hi != min) {
            set_t moved = *min;
            memmove(hi + 1, hi, (size_t)(min - hi) * sizeof(*hi));
            *hi = moved;
        }
    }
}
