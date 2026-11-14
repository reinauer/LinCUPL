/*
 * lib_bin.c - reader of the binary form of a gate library for the ATF150x
 * fitter.
 *
 * A library file that starts with the two 32-bit words 0x30, 0x0d is a
 * dump of the structures lib_parse.c builds, written by a tool that is not
 * part of the fitter.  atmel.std is text, so none of this runs in the CUPL
 * flow, and no binary library is available to test it against; it has
 * never been run on a real file.
 *
 * Format (all words little-endian 32 bit; a string is a length word
 * followed by that many bytes, the terminating NUL included; length 0 is a
 * null pointer; a float is its 32-bit pattern):
 *
 *   file    = 0x30 0x0d  string(comment)  float(default fanout)  n  gate*n
 *   gate    = kind dont_touch dont_use cell
 *   cell 1  = string(name) string(out) float(area) float(fanout) expr pins tree
 *   cell 10 = string(name) float(area) adder strings(in) strings(out) outpins
 *   cell 13 = type float(area) string(name) string(in) string(out) string(pad)
 *             string(oe) oe_pol outpins
 *   other   = string(name) float(area) string(clk) clk_kind string(ar) ar_pol
 *             string(ap) ap_pol string(ce) ce_pol string(oe) oe_pol string(d)
 *             string(j) j_pol string(k) k_pol string(a) string(b) string(sel)
 *             string(q) string(qn) string(z) outpins
 *   expr    = string(text) op bin_word  n expr*n  bin_words[4]
 *   pins    = n shared pin*n       (shared 1: all pins use the first one's timing)
 *   pin     = string(name) phase  n float*n
 *   tree    = sel ( value | string(name) | n tree*n )     for 'C' | 'I' | others
 *   outpins = n outpin*n
 *   outpin  = string(name) float(max fanout) pins
 *   strings = n string*n
 */
#include "lib_priv.h"

/* one 32-bit word */
static int libbin_read_word(FILE *f, int *out)
{
    unsigned char b[4];

    if (fread(b, 4, 1, f) == 0)
        return 0;
    *out = (int)((unsigned)b[0] | ((unsigned)b[1] << 8) | ((unsigned)b[2] << 16)
                 | ((unsigned)b[3] << 24));
    return 1;
}

static int libbin_read_float(FILE *f, float *out)
{
    unsigned char b[4];
    unsigned bits;

    if (fread(b, 4, 1, f) == 0)
        return 0;
    bits = (unsigned)b[0] | ((unsigned)b[1] << 8) | ((unsigned)b[2] << 16) | ((unsigned)b[3] << 24);
    memcpy(out, &bits, sizeof *out);    /* IEEE single on every host this builds for */
    return 1;
}

/* length word and bytes; *out is NULL for length 0.  One byte more than
 * the length is allocated, so that the string always ends. */
static int libbin_read_string(FILE *f, char **out)
{
    int len;
    char *s;

    if (!libbin_read_word(f, &len))
        return 0;
    if (len == 0) {
        *out = NULL;
        return 1;
    }
    s = util_calloc((size_t)(unsigned)len + 1, 1);
    *out = s;
    if (fread(s, 1, (size_t)(unsigned)len, f) < (size_t)(unsigned)len) {
        free(s);
        *out = NULL;
        return 0;
    }
    return 1;
}

/* string into a fixed buffer, truncated to fit; an empty (null) string
 * leaves the buffer unchanged */
static int libbin_read_name(FILE *f, char *dst, size_t size)
{
    char *s = NULL;

    if (!libbin_read_string(f, &s))
        return 0;
    if (s != NULL) {
        strncpy(dst, s, size - 1);
        dst[size - 1] = '\0';
        free(s);
    }
    return 1;
}

static int libbin_read_expr_list(FILE *f, FitList **out);

static int libbin_read_expr(FILE *f, FitLibExpr **out)
{
    FitLibExpr *e = util_calloc(1, sizeof *e);

    *out = e;
    if (!libbin_read_name(f, e->text, sizeof e->text))
        return 0;
    if (!libbin_read_word(f, &e->op))
        return 0;
    if (!libbin_read_word(f, &e->bin_word))
        return 0;
    if (!libbin_read_expr_list(f, &e->children))
        return 0;
    for (int i = 0; i < 4; i++)
        if (!libbin_read_word(f, &e->bin_words[i]))
            return 0;
    return 1;
}

static int libbin_read_expr_list(FILE *f, FitList **out)
{
    int n;

    if (!libbin_read_word(f, &n))
        return 0;
    *out = list_create_sized(n);
    for (int i = 1; i <= n; i++) {
        FitLibExpr *e = NULL;

        if (!libbin_read_expr(f, &e))
            return 0;
        list_add(*out, e);
    }
    return 1;
}

/* n floats as a list of float * */
static int libbin_read_floats(FILE *f, FitList **out)
{
    int n;

    if (!libbin_read_word(f, &n))
        return 0;
    *out = list_create_sized(n);
    for (int i = 1; i <= n; i++) {
        float *item = util_calloc(1, sizeof *item);

        if (!libbin_read_float(f, item)) {
            free(item);
            return 0;
        }
        list_add(*out, item);
    }
    return 1;
}

static int libbin_read_pin(FILE *f, FitLibPin **out)
{
    FitLibPin *p = util_calloc(1, sizeof *p);

    *out = p;
    if (!libbin_read_string(f, &p->name))
        return 0;
    if (!libbin_read_word(f, &p->phase))
        return 0;
    return libbin_read_floats(f, &p->timing);
}

static int libbin_read_pins(FILE *f, FitList **out)
{
    FitList *first_timing = NULL;
    int n, shared;

    if (!libbin_read_word(f, &n))
        return 0;
    *out = list_create_sized(n);
    if (!libbin_read_word(f, &shared))
        return 0;
    for (int i = 1; i <= n; i++) {
        FitLibPin *p = NULL;

        if (!libbin_read_pin(f, &p))
            return 0;
        list_add(*out, p);
        if (i == 1) {
            if (shared == 1)
                first_timing = p->timing;
        } else if (shared == 1) {
            if (p->timing != NULL)
                list_free_all(&p->timing, util_free_ptr);
            p->timing = first_timing;
        }
    }
    return 1;
}

static int libbin_read_tree_list(FILE *f, FitList **out);

static int libbin_read_tree(FILE *f, FitFctTree **out)
{
    FitFctTree *t = util_calloc(1, sizeof *t);

    *out = t;
    if (!libbin_read_word(f, &t->sel))
        return 0;
    if (t->sel == FIT_SEL_CONST) {
        int value;

        if (!libbin_read_word(f, &value))
            return 0;
        t->u.value = value;
        return 1;
    }
    if (t->sel == FIT_SEL_ID) {
        char *name = NULL;

        if (!libbin_read_string(f, &name))
            return 0;
        t->u.children = (FitList *)(void *)name;    /* pin name, as in lib_expr_to_tree */
        return 1;
    }
    return libbin_read_tree_list(f, &t->u.children);
}

static int libbin_read_tree_list(FILE *f, FitList **out)
{
    int n;

    if (!libbin_read_word(f, &n))
        return 0;
    *out = list_create_sized(n);
    for (int i = 1; i <= n; i++) {
        FitFctTree *t = NULL;

        if (!libbin_read_tree(f, &t))
            return 0;
        list_add(*out, t);
    }
    return 1;
}

static int libbin_read_comb(FILE *f, void **out)
{
    FitLibComb *c = util_calloc(1, sizeof *c);

    *out = c;
    if (!libbin_read_name(f, c->name, sizeof c->name))
        return 0;
    if (!libbin_read_name(f, c->out_name, sizeof c->out_name))
        return 0;
    if (!libbin_read_float(f, &c->area))
        return 0;
    if (!libbin_read_float(f, &c->max_fanout))
        return 0;
    if (!libbin_read_expr(f, &c->expr))
        return 0;
    if (!libbin_read_pins(f, &c->pins))
        return 0;
    return libbin_read_tree(f, &c->tree);
}

static int libbin_read_outpin(FILE *f, FitLibOutPin **out)
{
    FitLibOutPin *o = util_calloc(1, sizeof *o);

    *out = o;
    if (!libbin_read_string(f, &o->name))
        return 0;
    if (!libbin_read_float(f, &o->max_fanout))
        return 0;
    return libbin_read_pins(f, &o->pins);
}

static int libbin_read_outpins(FILE *f, FitList **out)
{
    int n;

    if (!libbin_read_word(f, &n))
        return 0;
    *out = list_create_sized(n);
    for (int i = 1; i <= n; i++) {
        FitLibOutPin *o = NULL;

        if (!libbin_read_outpin(f, &o))
            return 0;
        list_add(*out, o);
    }
    return 1;
}

static int libbin_read_buf(FILE *f, void **out)
{
    FitLibBuf *b = util_calloc(1, sizeof *b);

    *out = b;
    if (!libbin_read_word(f, &b->type))
        return 0;
    if (!libbin_read_float(f, &b->area))
        return 0;
    if (!libbin_read_string(f, &b->name))
        return 0;
    if (!libbin_read_string(f, &b->in))
        return 0;
    if (!libbin_read_string(f, &b->out))
        return 0;
    if (!libbin_read_string(f, &b->pad))
        return 0;
    if (!libbin_read_string(f, &b->oe))
        return 0;
    if (!libbin_read_word(f, &b->oe_pol))
        return 0;
    return libbin_read_outpins(f, &b->outs);
}

static int libbin_read_strings(FILE *f, FitList **out)
{
    int n;

    if (!libbin_read_word(f, &n))
        return 0;
    *out = list_create_sized(n);
    for (int i = 1; i <= n; i++) {
        char *s = NULL;

        if (!libbin_read_string(f, &s))
            return 0;
        list_add(*out, s);
    }
    return 1;
}

static int libbin_read_ugate(FILE *f, void **out)
{
    FitLibUGate *u = util_calloc(1, sizeof *u);

    *out = u;
    if (!libbin_read_string(f, &u->name))
        return 0;
    if (!libbin_read_float(f, &u->area))
        return 0;
    if (!libbin_read_word(f, &u->adder))
        return 0;
    if (!libbin_read_strings(f, &u->inputs))
        return 0;
    if (!libbin_read_strings(f, &u->outputs))
        return 0;
    return libbin_read_outpins(f, &u->outs);
}

static int libbin_read_seq(FILE *f, void **out)
{
    FitLibSeq *s = util_calloc(1, sizeof *s);

    *out = s;
    if (!libbin_read_string(f, &s->name))
        return 0;
    if (!libbin_read_float(f, &s->area))
        return 0;
    if (!libbin_read_string(f, &s->clk))
        return 0;
    if (!libbin_read_word(f, &s->clk_kind))
        return 0;
    if (!libbin_read_string(f, &s->ar))
        return 0;
    if (!libbin_read_word(f, &s->ar_pol))
        return 0;
    if (!libbin_read_string(f, &s->ap))
        return 0;
    if (!libbin_read_word(f, &s->ap_pol))
        return 0;
    if (!libbin_read_string(f, &s->ce))
        return 0;
    if (!libbin_read_word(f, &s->ce_pol))
        return 0;
    if (!libbin_read_string(f, &s->oe))
        return 0;
    if (!libbin_read_word(f, &s->oe_pol))
        return 0;
    if (!libbin_read_string(f, &s->d))
        return 0;
    if (!libbin_read_string(f, &s->j))
        return 0;
    if (!libbin_read_word(f, &s->j_pol))
        return 0;
    if (!libbin_read_string(f, &s->k))
        return 0;
    if (!libbin_read_word(f, &s->k_pol))
        return 0;
    if (!libbin_read_string(f, &s->mux_a))
        return 0;
    if (!libbin_read_string(f, &s->mux_b))
        return 0;
    if (!libbin_read_string(f, &s->mux_sel))
        return 0;
    if (!libbin_read_string(f, &s->q))
        return 0;
    if (!libbin_read_string(f, &s->qn))
        return 0;
    if (!libbin_read_string(f, &s->z))
        return 0;
    return libbin_read_outpins(f, &s->outs);
}

static int libbin_read_gate(FILE *f, FitLibGate **out)
{
    FitLibGate *g = util_calloc(1, sizeof *g);

    *out = g;
    if (!libbin_read_word(f, &g->kind))
        return 0;
    if (!libbin_read_word(f, &g->dont_touch))
        return 0;
    if (!libbin_read_word(f, &g->dont_use))
        return 0;
    if (g->kind == FIT_GATE_COMB)
        return libbin_read_comb(f, &g->cell);
    if (g->kind == FIT_GATE_UNKNOWN)
        return libbin_read_ugate(f, &g->cell);
    if (g->kind == FIT_GATE_BUF)
        return libbin_read_buf(f, &g->cell);
    return libbin_read_seq(f, &g->cell);
}

/* 1 if the file starts with the words 0x30, 0x0d (they are consumed) */
int libbin_is_binary(FILE *f)
{
    int w;

    if (libbin_read_word(f, &w) && w == 0x30 && libbin_read_word(f, &w) && w == 0xd)
        return 1;
    return 0;
}

/* The rest of a binary library after the two magic words.  A
 * gate that fails to read is not added to the list (and not freed). */
int libbin_read_library(FILE *f, FitList **gates, FitLibHeader **header)
{
    FitLibHeader *h = util_calloc(1, sizeof *h);
    float fanout;
    int n;

    *header = h;
    if (!libbin_read_string(f, &h->comment))
        return 0;
    if (!libbin_read_float(f, &fanout))
        return 0;
    g_lib_default_fanout = fanout;
    if (!libbin_read_word(f, &n))
        return 0;
    *gates = list_create_sized(n);
    for (int i = 1; i <= n; i++) {
        FitLibGate *g = NULL;

        if (!libbin_read_gate(f, &g))
            return 0;
        list_add(*gates, g);
    }
    return 1;
}
