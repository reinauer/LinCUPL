/*
 * base_pterm.c - product terms as pairs of bit vectors.
 *
 * A literal of variable n (1-based) is bit n-1 of the two vectors:
 *
 *     bits care   character   meaning
 *      0    1          -      variable absent from the product
 *      1    1          1      variable
 *      0    0          0      complemented variable
 *      1    0        ~ (?)    contradiction / "unknown" (pterm_build takes
 *                             '?' and '~'; the output part of a PLA line
 *                             uses it as a fourth value)
 *
 * The word operations themselves (fill, count, xnor ...) are in min.h.
 * Helpers needed only by the equation reader are not implemented.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base_int.h"
#include "min.h"

/* sets the literal of variable var (1-based) to c: '0', '1' or '-' */
void pterm_set_literal(FitPTerm *p, int var, char c)
{
    unsigned word = ((unsigned)var - 1u) >> 5;
    unsigned mask = 1u << (((unsigned)var - 1u) & 0x1f);

    if (c == '-' || c == '0')
        p->bits[word] &= ~mask;
    else if (c == '1')
        p->bits[word] |= mask;
    if (c != '-') {
        if (c == '0') {
            p->care[word] &= ~mask;
            return;
        }
        if (c != '1')
            return;
    }
    p->care[word] |= mask;
}

/* mask of the positions of p holding the literal value c */
static void pterm_extract(FitPTerm *p, unsigned *out, int nwords, char c)
{
    if (p == NULL)
        return;
    switch (c) {
    case '-':
        /* the first result is overwritten, so '-' gives the mask of '1'
         * (no caller asks for '-') */
        bitvec_not(p->bits, out, nwords);
        bitvec_and(p->bits, p->care, out, nwords);
        return;
    case '0':
        bitvec_nor(p->bits, p->care, out, nwords);
        return;
    case '1':
        bitvec_and(p->bits, p->care, out, nwords);
        return;
    case '?':
        bitvec_xnor(p->bits, p->care, out, nwords);
        return;
    default:
        return;
    }
}

/* union over the terms of the positions that carry a literal */
unsigned *pterm_list_support(FitList *pterms, int nwords)
{
    unsigned *one, *all;

    if (pterms == NULL)
        return NULL;
    one = util_calloc((size_t)(nwords > 0 ? nwords : 1), 4);
    all = util_calloc((size_t)(nwords > 0 ? nwords : 1), 4);
    for (int i = 0; i < pterms->count; i++) {
        pterm_extract(pterms->items[i], one, nwords, '?');
        for (int w = 0; w < nwords; w++)
            all[w] |= one[w];
    }
    free(one);
    return all;
}

/* 1 if bits and care differ in every position of every word
 * (no variable appears in the product) */
int pterm_is_tautology(FitPTerm *p, int nwords)
{
    for (int i = nwords - 1; i >= 0; i--) {
        if ((p->care[i] ^ p->bits[i]) != 0xffffffffu)
            return 0;
    }
    return 1;
}

int pterm_bit(FitPTerm *p, int var)
{
    return bitvec_test(p->bits, var);
}

/* allocates both vectors (nwords + 1 words each) */
void pterm_alloc(FitPTerm *p, int nwords)
{
    p->bits = bitvec_alloc_plus1(nwords);
    p->care = bitvec_alloc_plus1(nwords);
}

FitPTerm *pterm_new_raw(int nwords)
{
    FitPTerm *p = util_calloc(1, sizeof(*p));

    pterm_alloc(p, nwords);
    return p;
}

/* allocates the vectors; bits 0, care all ones: no literal */
void pterm_init_dc(FitPTerm *p, int nwords)
{
    pterm_alloc(p, nwords);
    bitvec_fill(p->bits, nwords, 0);
    bitvec_fill(p->care, nwords, 0xffffffffu);
}

FitPTerm *pterm_new(int nwords)
{
    FitPTerm *p = util_calloc(1, sizeof(*p));

    pterm_init_dc(p, nwords);
    return p;
}

/* allocates the vectors, both 0 */
void pterm_init_zero(FitPTerm *p, int nwords)
{
    pterm_alloc(p, nwords);
    bitvec_fill(p->bits, nwords, 0);
    bitvec_fill(p->care, nwords, 0);
}

FitPTerm *pterm_new_zero(int nwords)
{
    FitPTerm *p = util_calloc(1, sizeof(*p));

    pterm_init_zero(p, nwords);
    return p;
}

/* PLA line: inputs without literals, outputs all 0 */
FitPTermIO *ptermio_new(int nwords_in, int nwords_out)
{
    FitPTermIO *p = util_calloc(1, sizeof(*p));

    pterm_init_dc(&p->in, nwords_in);
    pterm_init_zero(&p->out, nwords_out);
    return p;
}

/* sets the first n positions from the characters 0 1 - ? ~ of s */
void pterm_build(FitPTerm *p, const char *s, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        unsigned word = i >> 5;
        unsigned mask = 1u << (i & 0x1f);

        switch (s[i]) {
        case '-':
            p->bits[word] &= ~mask;
            p->care[word] |= mask;
            break;
        case '0':
            p->bits[word] &= ~mask;
            p->care[word] &= ~mask;
            break;
        case '1':
            p->bits[word] |= mask;
            p->care[word] |= mask;
            break;
        case '?':
        case '~':
            p->bits[word] |= mask;
            p->care[word] &= ~mask;
            break;
        default:
            fprintf(g_msg, "ERROR  : Function PTermBuild : An error has been detected in the string: forbidden character\n");
            break;
        }
    }
}

void ptermio_build(FitPTermIO *p, const char *in, const char *out, unsigned n_in, unsigned n_out)
{
    pterm_build(&p->in, in, n_in);
    pterm_build(&p->out, out, n_out);
}

/* list callback: both vectors equal over nwords (FIT_INT2PTR) words */
int pterm_equal(void *a, void *b, void *nwords)
{
    FitPTerm *x = a, *y = b;
    size_t size = (size_t)FIT_PTR2INT(nwords) * 4;

    if (memcmp(x->bits, y->bits, size) != 0)
        return 0;
    return memcmp(x->care, y->care, size) == 0;
}

/* 1 if a covers b (every minterm of b is one of a) */
int pterm_covers(FitPTerm *a, FitPTerm *b, int nwords)
{
    for (int i = nwords - 1; i >= 0; i--) {
        if ((~a->bits[i] & b->bits[i]) != 0 || (~b->care[i] & a->care[i]) != 0)
            return 0;
    }
    return 1;
}

/* n characters and a NUL into out */
void pterm_to_string(FitPTerm *p, unsigned n, char *out, int warn)
{
    int bad = 0;

    for (unsigned i = 0; i < n; i++) {
        unsigned word = i >> 5;
        unsigned mask = 1u << (i & 0x1f);

        if ((p->bits[word] & mask) == 0) {
            out[i] = (p->care[word] & mask) == 0 ? '0' : '-';
        } else if ((p->care[word] & mask) == 0) {
            out[i] = '~';
            bad = 1;
        } else {
            out[i] = '1';
        }
    }
    out[n] = '\0';
    if (bad != 0 && warn != 0)
        fprintf(g_msg, "Function PTermToString : Error in PTerm Value\n");
}

/* a new term of nwords words whose first nwords_copy words are
 * those of src */
FitPTerm *pterm_copy_words(unsigned nwords_copy, int nwords, FitPTerm *src)
{
    FitPTerm *p = pterm_new(nwords);

    if (nwords_copy > (unsigned)nwords + 1u)
        nwords_copy = (unsigned)nwords + 1u;
    memcpy(p->bits, src->bits, (size_t)nwords_copy * 4);
    memcpy(p->care, src->care, (size_t)nwords_copy * 4);
    return p;
}

/* prints the first n positions as characters */
void pterm_print(FILE *out, FitPTerm *p, unsigned n)
{
    char *text = util_calloc((size_t)n + 1, 1);

    pterm_to_string(p, n, text, 1);
    fprintf(out, "%s", text);
    free(text);
}

void pterm_release(FitPTerm *p)
{
    free(p->bits);
    p->bits = NULL;
    free(p->care);
    p->care = NULL;
}

void pterm_free(FitPTerm **p)
{
    if (*p != NULL) {
        pterm_release(*p);
        free(*p);
        *p = NULL;
    }
}

static void ptermio_release(FitPTermIO *p)
{
    pterm_release(&p->in);
    pterm_release(&p->out);
}

void ptermio_free(FitPTermIO **p)
{
    if (*p == NULL)
        return;
    ptermio_release(*p);
    free(*p);
    *p = NULL;
}

/* every position gets the literal value *c */
int pterm_init_with_value(FitPTerm *p, const char *c, int nwords)
{
    switch (*c) {
    case '-':
        bitvec_fill(p->bits, nwords, 0);
        bitvec_fill(p->care, nwords, 0xffffffffu);
        return 1;
    case '0':
        bitvec_fill(p->bits, nwords, 0);
        bitvec_fill(p->care, nwords, 0);
        return 1;
    case '1':
        bitvec_fill(p->bits, nwords, 0xffffffffu);
        bitvec_fill(p->care, nwords, 0xffffffffu);
        return 1;
    case '?':
        bitvec_fill(p->bits, nwords, 0xffffffffu);
        bitvec_fill(p->care, nwords, 0);
        return 1;
    default:
        printf("Function PTermInitWithValue : An error ");
        printf("has been detected in the string : ");
        printf("forbidden character\n");
        return 0;
    }
}

/* lit = 2 * position for the plain literal, 2 * position + 1
 * for the complemented one; nonzero if the term has that literal */
unsigned pterm_has_literal(FitPTerm *p, unsigned lit)
{
    unsigned pos = (unsigned)((int)lit >> 1);
    unsigned word = pos >> 5;
    unsigned mask = 1u << (pos & 0x1f);

    if ((lit & 1) == 0)
        return p->care[word] & p->bits[word] & mask;
    if ((p->bits[word] & mask) == 0 && (p->care[word] & mask) == 0)
        return 1;
    return 0;
}

/* bits * 2 + care of position index (0-based) */
int pterm_literal_code(FitPTerm *p, unsigned index)
{
    unsigned word = index >> 5;
    unsigned shift = index & 0x1f;

    return (int)((p->care[word] >> shift) & 1) + (int)((p->bits[word] >> shift) & 1) * 2;
}

/* '0', '-', '1' or '?' for position index (0-based) */
static char pterm_literal_char(FitPTerm *p, unsigned index)
{
    unsigned word = index >> 5;
    unsigned shift = index & 0x1f;
    unsigned care = (p->care[word] >> shift) & 1;

    if (((p->bits[word] >> shift) & 1) == 0)
        return care != 0 ? '-' : '0';
    return care != 0 ? '1' : '?';
}

/* copy of p; NULL for NULL */
FitPTerm *pterm_copy(FitPTerm *p, int nwords)
{
    FitPTerm *c;

    if (p == NULL)
        return NULL;
    c = pterm_new_raw(nwords);
    for (int i = nwords - 1; i >= 0; i--) {
        c->bits[i] = p->bits[i];
        c->care[i] = p->care[i];
    }
    return c;
}

/* number of variables that appear in the product */
int pterm_count_literals(FitPTerm *p, int nwords)
{
    int n = 0;

    if (p == NULL)
        return 0;
    for (int i = nwords - 1; i >= 0; i--) {
        unsigned w = ~(p->bits[i] ^ p->care[i]);

        n += bitvec_count(&w, 1);
    }
    return n;
}

/* the terms of a PLA (list of FitPTermIO) whose output `out`
 * (1-based) has the value c */
FitList *ptermio_select_output(FitList *pterms, int out, char c)
{
    FitList *sel = list_create();

    for (int i = 0; i < (pterms == NULL ? 0 : pterms->count); i++) {
        FitPTermIO *p = pterms->items[i];

        if (pterm_literal_char(&p->out, (unsigned)out - 1u) == c)
            list_add(sel, p);
    }
    return sel;
}

static int ptermio_in_equal(void *item, void *key, void *nwords)
{
    return pterm_equal(key, item, nwords);
}

/* add the products of `pterms` (list of FitPTerm) to the PLA
 * `into` (list of FitPTermIO) as terms of output `out` with value c; a
 * product the PLA already has only gets the output set. */
void ptermio_merge_output(FitList *pterms, FitList *into, int out, int nwords_in, int nwords_out, char c)
{
    for (int i = 0; i < (pterms == NULL ? 0 : pterms->count); i++) {
        int pos = list_find_x(into, pterms->items[i], ptermio_in_equal, FIT_INT2PTR(nwords_in));

        if (pos == 0) {
            FitPTermIO *io = util_calloc(1, sizeof(*io));
            FitPTerm *part = pterm_copy(pterms->items[i], nwords_in);

            io->in = *part;
            free(part);
            part = pterm_new_zero(nwords_out);
            pterm_set_literal(part, out, c);
            io->out = *part;
            free(part);
            list_add(into, io);
        } else {
            pterm_set_literal(&((FitPTermIO *)into->items[pos - 1])->out, out, c);
        }
    }
}

/* 0 if some position has a bit set in a or b where not both terms
 * care, else 1 */
static int pterm_intersects(FitPTerm *a, FitPTerm *b, int nwords)
{
    for (int i = nwords - 1; i >= 0; i--) {
        if ((~(a->care[i] & b->care[i]) & (b->bits[i] | a->bits[i])) != 0)
            return 0;
    }
    return 1;
}

/* 0 if pterm_intersects holds for a term of a and a term of b, else 1 */
int pterm_lists_disjoint(FitList *a, FitList *b, int nwords)
{
    if (a != NULL && a->count != 0 && b != NULL && b->count != 0) {
        for (int i = 0; i < a->count; i++) {
            for (int j = 0; j < b->count; j++) {
                if (pterm_intersects(a->items[i], b->items[j], nwords) != 0)
                    return 0;
            }
        }
    }
    return 1;
}

/* dst = dst AND src, in place, without the emptiness test */
void pterm_and_into(FitPTerm *dst, FitPTerm *src, int nwords)
{
    for (int i = 0; i < nwords; i++) {
        dst->bits[i] |= src->bits[i];
        dst->care[i] &= src->care[i];
    }
}

/* the product of two terms, NULL if it is empty (a variable
 * would be needed in both polarities) */
FitPTerm *pterm_and(FitPTerm *a, FitPTerm *b, int nwords)
{
    FitPTerm *p = pterm_new_raw(nwords);

    for (int i = nwords - 1; i >= 0; i--) {
        p->bits[i] = a->bits[i] | b->bits[i];
        p->care[i] = a->care[i] & b->care[i];
        if ((p->bits[i] & ~p->care[i]) != 0) {
            pterm_free(&p);
            return NULL;
        }
    }
    return p;
}

/* all nonempty products of a term of a with a term of b, each
 * one once */
FitList *pterm_list_and(FitList *a, FitList *b, int nwords)
{
    FitList *out = list_create();

    if (a == NULL || a->count == 0 || b == NULL || b->count == 0)
        return out;
    for (int i = 0; i < a->count; i++) {
        for (int j = 0; j < b->count; j++) {
            FitPTerm *p = pterm_and(a->items[i], b->items[j], nwords);

            if (p == NULL)
                continue;
            if (list_find_x(out, p, pterm_equal, FIT_INT2PTR(nwords)) != 0)
                pterm_free(&p);
            else
                list_add(out, p);
        }
    }
    return out;
}

/* dst = a AND b, without the emptiness test */
void pterm_and_to(FitPTerm *a, FitPTerm *b, FitPTerm *dst, int nwords)
{
    for (int i = nwords - 1; i >= 0; i--) {
        dst->bits[i] = a->bits[i] | b->bits[i];
        dst->care[i] = a->care[i] & b->care[i];
    }
}
