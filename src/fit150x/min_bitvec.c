/*
 * min_bitvec.c - bit vectors of the ATF150x fitter: operations on arrays
 * of 32-bit words.  Variable v (1-based) is bit (v-1)&31 of word (v-1)>>5.
 *
 * Equation input (-str ifmt eqn) is an unsupported option.
 */
#include <stdio.h>
#include <stdlib.h>

#include "bits.h"
#include "min_priv.h"

void bitvec_fill(unsigned *v, int n_words, unsigned value)
{
    for (int i = 0; i < n_words; i++)
        v[i] = value;
}

unsigned *bitvec_alloc_plus1(int n_words)
{
    return util_calloc((size_t)(n_words + 1), sizeof(unsigned));
}

unsigned *bitvec_new_filled(int n_words, unsigned value)
{
    unsigned *v = util_calloc((size_t)n_words, sizeof(unsigned));

    bitvec_fill(v, n_words, value);
    return v;
}

unsigned *bitvec_new_zero(int n_words)
{
    return bitvec_new_filled(n_words, 0);
}

unsigned *bitvec_copy_new(const unsigned *src, int n_words)
{
    unsigned *v = util_calloc((size_t)n_words, sizeof(unsigned));

    for (int i = n_words - 1; i >= 0; i--)
        v[i] = src[i];
    return v;
}

int bitvec_test(const unsigned *v, int var)
{
    unsigned pos = (unsigned)var - 1u;

    return ((1u << (pos & 0x1f)) & v[pos >> 5]) != 0;
}

int bitvec_count(const unsigned *v, int n_words)
{
    int n = 0;

    for (int i = n_words - 1; i >= 0; i--)
        n += lincupl_popcount32(v[i]);
    return n;
}

void bitvec_set(unsigned *v, int var)
{
    unsigned pos = (unsigned)var - 1u;

    v[pos >> 5] |= 1u << (pos & 0x1f);
}

void bitvec_clear(unsigned *v, int var)
{
    unsigned pos = (unsigned)var - 1u;

    v[pos >> 5] &= ~(1u << (pos & 0x1f));
}

unsigned *bitvec_xnor_new(const unsigned *a, const unsigned *b, int n_words)
{
    unsigned *v = bitvec_alloc_plus1(n_words);

    for (int i = 0; i < n_words; i++)
        v[i] = ~(a[i] ^ b[i]);
    return v;
}

int bitvec_not(const unsigned *a, unsigned *out, int n_words)
{
    int any = 0;

    for (int i = 0; i < n_words; i++) {
        unsigned w = ~a[i];

        out[i] = w;
        if (w != 0)
            any = 1;
    }
    return any;
}

int bitvec_and(const unsigned *a, const unsigned *b, unsigned *out, int n_words)
{
    int any = 0;

    for (int i = 0; i < n_words; i++) {
        unsigned w = a[i] & b[i];

        out[i] = w;
        if (w != 0)
            any = 1;
    }
    return any;
}

int bitvec_or(const unsigned *a, const unsigned *b, unsigned *out, int n_words)
{
    int any = 0;

    for (int i = 0; i < n_words; i++) {
        unsigned w = a[i] | b[i];

        out[i] = w;
        if (w != 0)
            any = 1;
    }
    return any;
}

int bitvec_nor(const unsigned *a, const unsigned *b, unsigned *out, int n_words)
{
    int any = 0;

    for (int i = 0; i < n_words; i++) {
        unsigned w = ~(a[i] | b[i]);

        out[i] = w;
        if (w != 0)
            any = 1;
    }
    return any;
}

void bitvec_xnor(const unsigned *a, const unsigned *b, unsigned *out, int n_words)
{
    for (int i = 0; i < n_words; i++)
        out[i] = ~(a[i] ^ b[i]);
}

/* ------------------------------------------------------------------ */
/* Entry points of unsupported options                                 */
/* ------------------------------------------------------------------ */

static void bitvec_unsupported(const char *what)
{
    fprintf(stderr, "fit150x: %s is not supported\n", what);
    exit(3);
}

/* -str ifmt eqn */
int bitvec_parse_eqn_cube(char *text, int *a, int *b, unsigned n)
{
    (void)text;
    (void)a;
    (void)b;
    (void)n;
    bitvec_unsupported("the equation input format (-str ifmt eqn)");
    return 0;
}
