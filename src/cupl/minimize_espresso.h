/*
 * minimize_espresso - minimization level 4 of minimize: Berkeley Espresso 2.5.1
 * (third_party/espresso) behind an interface that does not expose its types
 * or global variables.
 */
#ifndef MINIMIZE_ESPRESSO_H
#define MINIMIZE_ESPRESSO_H

#include <stdint.h>

/* A cover in the set layout that minimize uses (and Espresso itself): a cube is
 * set_words 32 bit words; word 0 is not looked at; element n is bit (n & 31)
 * of word (n >> 5) + 1.  Input i is the pair of elements 2i (the input may
 * be 0) and 2i + 1 (it may be 1); output k is element 2 * num_inputs + k. */
typedef struct {
    int num_inputs;
    int num_outputs;
    int set_words;
    const uint32_t *on_set;         /* on_count cubes */
    int on_count;
    const uint32_t *dc_set;         /* dc_count cubes; may be NULL if there are none */
    int dc_count;
    void (*tick)(void);             /* called while Espresso works, or NULL */
} minimize_espresso_problem_t;

enum {
    MINIMIZE_ESPRESSO_OK = 0,
    MINIMIZE_ESPRESSO_FAILED = 1,      /* Espresso stopped with an internal error */
    MINIMIZE_ESPRESSO_NO_MEMORY = 2
};

/* Minimize the on-set against the don't-care set.  On success *result is a
 * calloc()ed array of *result_count cubes in the layout above (word 0 is 0),
 * which the caller releases with free().  Nothing of Espresso's state
 * survives the call, whether it succeeds or not. */
int minimize_espresso_minimize(const minimize_espresso_problem_t *problem, uint32_t **result, int *result_count);

/* Test support: the largest number of memory blocks that Espresso still held
 * after a successful minimization had released everything it knew of
 * (0 unless the library leaks). */
long minimize_espresso_leaked_blocks(void);

#endif
