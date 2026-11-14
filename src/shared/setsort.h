/*
 * setsort.h - the sort of the Espresso minimizers (compiler -m4 and fitter).
 */
#ifndef LINCUPL_SETSORT_H
#define LINCUPL_SETSORT_H

#include <stdint.h>

typedef int (*lincupl_set_compare_t)(uint32_t **a, uint32_t **b);

/* Sorts n set pointers with the 4.3BSD qsort algorithm that Espresso
 * ships (quicksort down to small parts, then an insertion sort).  Like any
 * quicksort it does not keep equal elements in their order, and the order it
 * leaves them in decides which cubes survive, so the minimized covers depend
 * on this algorithm and not only on the comparison: the C library's qsort
 * cannot replace it. */
void lincupl_sort_sets(uint32_t **base, int n, lincupl_set_compare_t compare);

#endif
