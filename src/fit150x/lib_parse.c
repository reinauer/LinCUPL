/*
 * lib_parse.c - parser of the gate library text (atmel.std) for the
 * ATF150x fitter, and the destructors of the library objects.  The reader
 * of the binary form of a library is in lib_bin.c.
 *
 * Syntax (keywords in any case; '#' starts a comment that ends with the
 * line; a real number is  [-] digits [. digits]):
 *
 *   library   = { gate | pgate | ugate | default | comment }
 *   default   = DEFAULT MAX_FANOUT real
 *   comment   = COMMENT '"' { word } '"'
 *   gate      = GATE int int name real [real] out '=' ( CONST0 | CONST1 | expr ) ';' pins
 *   expr      = term { '+' term }
 *   term      = factor { ( '*' | '.' ) factor }
 *   factor    = ident | '!' factor | '(' expr ')'
 *   pins      = PIN '*' phase timing            (all input pins alike)
 *             | PIN ident phase timing { PIN ident phase timing }
 *   phase     = INV | NONINV | UNKNOWN
 *   timing    = 6 reals (GATE, BUF) or 8 reals (the other PGATEs, UGATE)
 *   pgate     = PGATE int int kind ...
 *     kind TG DG LG JKG DMUXG LMUXG RSG RSL:
 *               name real clk clock ar ap ce oe data q qn z related
 *       clock = RECK | FECK | L0 | L1
 *       data  = d                (TG: may be '*'; DG, LG)
 *             | j k              (JKG, RSG, RSL; each may be '*' or '!'pin)
 *             | sel a b          (DMUXG, LMUXG)
 *       ar ap ce oe: '*', pin or '!'pin;  q qn z: '*' or pin
 *     kind THG ITHG:   name real in oe out related
 *     kind BUF:        buftype name real in out pad oe related
 *       buftype = I_BUF | O_BUF | BI_BUF | TS_BUF | CLK_BUF
 *   related   = { RELATED_OUTPUT_PIN pin real pins }
 *   ugate     = UGATE int int name real n pin*n m pin*m related
 *               [ UGATE_IS ( adder1 | adder2 ) { word } END_UGATE_IS ]
 *   name      = word | '"' word '"'
 */
#include "lib_priv.h"

/* keyword indices installed by lib_lex_setup */
enum {
    KW_GATE = 1, KW_PIN, KW_INV, KW_NONINV, KW_UNKNOWN, KW_CONST0, KW_CONST1,
    KW_PGATE, KW_TG, KW_DG, KW_LG, KW_JKG, KW_DMUXG, KW_LMUXG, KW_THG, KW_ITHG,
    KW_RSG, KW_BUF, KW_RECK, KW_FECK, KW_L0, KW_L1, KW_I_BUF, KW_O_BUF,
    KW_BI_BUF, KW_TS_BUF, KW_CLK_BUF, KW_UGATE, KW_RELATED_OUTPUT_PIN,
    KW_DEFAULT, KW_MAX_FANOUT, KW_COMMENT, KW_RSL
};

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static int g_timing_count;        /* reals per PIN line: 6 or 8 */
static int g_names_freed;         /* see lib_names_freed */
float      g_lib_default_fanout;

void lib_parse_reset(void)
{
    g_timing_count = 0;
    g_names_freed = 0;
    g_lib_default_fanout = 0.0f;
}

static FitLibExpr *lib_parse_or(FitList *names);
static FitFctTree *lib_expr_to_tree(FitLibExpr *e);

/* ------------------------------------------------------------------ */
/* Destructors                                                         */
/* ------------------------------------------------------------------ */

static void lib_free_if(void *p)
{
    if (p != NULL)
        free(p);
}

/* FitListEq: pointer identity; the same code as list_ptr_eq */

/* Releases the pins of a pin list (name and struct of each) and their
 * timing lists, each list once ("PIN *" makes all pins share one).  The
 * pin list itself is left to the caller.  Shared by lib_comb_free and
 * lib_outpin_free. */
static void lib_pins_free(FitList *pins)
{
    FitList *lists = list_create();

    for (int i = 0; i < LCOUNT(pins); i++) {
        FitLibPin *p = pins->items[i];

        if (p == NULL)
            continue;
        if (list_find(lists, p->timing, list_ptr_eq) == 0)
            list_add(lists, p->timing);
        free(p->name);
        free(p);
        pins->items[i] = NULL;
    }
    for (int i = 0; i < LCOUNT(lists); i++) {
        FitList *timing = lists->items[i];

        if (timing != NULL)
            list_free_all(&timing, util_free_ptr);
    }
    list_destroy(&lists);
}

/* FitListFree */
void lib_outpin_free(void **outpin_ref)
{
    FitLibOutPin *o = *outpin_ref;

    if (o == NULL)
        return;
    free(o->name);
    lib_pins_free(o->pins);         /* the list o->pins itself is not freed */
    free(o);
    *outpin_ref = NULL;
}

/* FitListFree */
static void lib_expr_free(void **expr_ref)
{
    FitLibExpr *e = *expr_ref;

    if (e == NULL)
        return;
    if (e->children != NULL)
        list_free_all(&e->children, lib_expr_free);
    free(e);
    *expr_ref = NULL;
}

static void lib_comb_free(void **cell_ref)
{
    FitLibComb *c = *cell_ref;
    void *expr;

    if (c == NULL)
        return;
    lib_pins_free(c->pins);         /* the list c->pins itself is not freed */
    if (c->tree != NULL)
        fcttree_free(&c->tree);     /* the names held by its ID nodes are not freed */
    expr = c->expr;
    lib_expr_free(&expr);
    free(c);
    *cell_ref = NULL;
}

/* every string but q is released */
static void lib_seq_free(void **cell_ref)
{
    FitLibSeq *s = *cell_ref;

    lib_free_if(s->name);
    lib_free_if(s->clk);
    lib_free_if(s->ar);
    lib_free_if(s->ap);
    lib_free_if(s->ce);
    lib_free_if(s->oe);
    lib_free_if(s->d);
    lib_free_if(s->j);
    lib_free_if(s->k);
    lib_free_if(s->mux_a);
    lib_free_if(s->mux_b);
    lib_free_if(s->mux_sel);
    lib_free_if(s->qn);
    lib_free_if(s->z);
    if (s->outs != NULL)
        list_free_all(&s->outs, lib_outpin_free);
    free(s);
    *cell_ref = NULL;
}

static void lib_buf_free(void **cell_ref)
{
    FitLibBuf *b = *cell_ref;

    lib_free_if(b->name);
    lib_free_if(b->out);
    lib_free_if(b->in);
    lib_free_if(b->oe);
    lib_free_if(b->pad);
    if (b->outs != NULL)
        list_free_all(&b->outs, lib_outpin_free);
    free(b);
    *cell_ref = NULL;
}

static void lib_ugate_free(void **cell_ref)
{
    FitLibUGate *u = *cell_ref;

    lib_free_if(u->name);
    if (u->inputs != NULL)
        list_free_all(&u->inputs, util_free_ptr);
    if (u->outputs != NULL)
        list_free_all(&u->outputs, util_free_ptr);
    if (u->outs != NULL)
        list_free_all(&u->outs, lib_outpin_free);
    free(u);
    *cell_ref = NULL;
}

/* FitListFree */
void lib_gate_free(void **gate_ref)
{
    FitLibGate *g = *gate_ref;

    if (g == NULL)
        return;
    if (g->cell != NULL) {
        if (g->kind == FIT_GATE_COMB)
            lib_comb_free(&g->cell);
        else if (g->kind == FIT_GATE_UNKNOWN)
            lib_ugate_free(&g->cell);
        else if (g->kind == FIT_GATE_BUF)
            lib_buf_free(&g->cell);
        else
            lib_seq_free(&g->cell);
    }
    free(g);
    *gate_ref = NULL;
}

void lib_header_free(FitLibHeader **header)
{
    FitLibHeader *h = *header;

    if (h == NULL)
        return;
    for (int i = 0; i < LCOUNT(h->l10); i++)
        free(h->l10->items[i]);
    if (h->l10 != NULL)
        list_destroy(&h->l10);
    for (int i = 0; i < LCOUNT(h->l20); i++)
        free(h->l20->items[i]);
    if (h->l20 != NULL)
        list_destroy(&h->l20);
    free(h->comment);
    free(h);
}

/* ------------------------------------------------------------------ */
/* Numbers and names                                                   */
/* ------------------------------------------------------------------ */

/* [-] digits [ . digits ].  The lexer delivers the two digit
 * strings as identifiers around a '.' token; they are put together again
 * and read with "%f".  On failure the error flag is set (no message). */
static int lib_parse_real(float *out)
{
    char *int_part, *frac_part, *buf;
    size_t size;
    int sign = 1;

    *out = 0.0f;
    if (g_lex_token == FIT_TOK_MINUS) {
        sign = -1;
        lex_next_token();
    }
    if (g_lex_token != FIT_TOK_IDENT || !util_is_digits(g_lex_text)) {
        g_lex_failed = 1;
        return 0;
    }
    int_part = util_strdup(g_lex_text);
    lex_next_token();
    if (g_lex_token == FIT_TOK_DOT) {
        lex_next_token();
        if (g_lex_token != FIT_TOK_IDENT || !util_is_digits(g_lex_text)) {
            g_lex_failed = 1;
            free(int_part);
            return 0;
        }
        frac_part = util_strdup(g_lex_text);
        lex_next_token();
    } else {
        frac_part = util_strdup("0");
    }
    /* at least 0x100 bytes, more for long digit strings */
    size = strlen(int_part) + strlen(frac_part) + 2;
    if (size < 0x100)
        size = 0x100;
    buf = util_calloc(size, 1);
    util_str_insert_front(frac_part, buf);
    util_str_insert_front(".", buf);
    util_str_insert_front(int_part, buf);
    sscanf(buf, "%f", out);
    free(int_part);
    free(frac_part);
    free(buf);
    *out = (float)sign * *out;
    return 1;
}

/* unsigned decimal integer; on failure the error flag is set */
static int lib_parse_int(int *out)
{
    char *s;

    *out = 0;
    if (g_lex_token != FIT_TOK_IDENT || !util_is_digits(g_lex_text)) {
        g_lex_failed = 1;
        return 0;
    }
    s = util_strdup(g_lex_text);
    lex_next_token();
    sscanf(s, "%d", out);
    free(s);
    return 1;
}

/* a name, optionally between double quotes; new string */
static char *lib_parse_name(void)
{
    int quoted = (g_lex_token == FIT_TOK_DQUOTE);
    char *name;

    if (quoted)
        lex_next_token();
    name = util_strdup(g_lex_text);
    lex_next_token();
    if (quoted) {
        if (g_lex_token != FIT_TOK_DQUOTE) {
            lex_error(1, "Name must be between '\"'", 0);
            return NULL;            /* name is not freed */
        }
        lex_next_token();
    }
    return name;
}

/* DEFAULT MAX_FANOUT real */
static void lib_parse_default(void)
{
    if (g_lex_token != KW_MAX_FANOUT) {
        lex_error(1, "DEFAULT VALUE NAME expected - analysis aborted", 0);
        g_lex_failed = 1;
        return;
    }
    lex_next_token();
    if (!lib_parse_real(&g_lib_default_fanout))
        lex_error(1, "DEFAULT Real VALUE  expected - analysis aborted", 0);
}

/* ------------------------------------------------------------------ */
/* GATE                                                                */
/* ------------------------------------------------------------------ */

static FitLibExpr *lib_expr_new(int op, const char *text)
{
    FitLibExpr *e = util_calloc(1, sizeof *e);

    e->op = op;
    /* truncated to the 0x98 bytes of text */
    strncpy(e->text, text, sizeof e->text - 1);
    e->text[sizeof e->text - 1] = '\0';
    return e;
}

/* ident | '!' factor | '(' expr ')' */
static FitLibExpr *lib_parse_factor(FitList *names)
{
    FitLibExpr *node = NULL;

    if (g_lex_token == FIT_TOK_EOF)
        return NULL;
    if (g_lex_token == FIT_TOK_IDENT) {
        node = lib_expr_new(FIT_LIBEXPR_VAR, g_lex_text);
        if (list_find(names, g_lex_text, lib_str_ieq) == 0)
            list_add(names, util_strdup(g_lex_text));
        node->children = NULL;
        lex_next_token();
    } else if (g_lex_token == FIT_TOK_BANG) {
        FitLibExpr *child;

        lex_next_token();
        child = lib_parse_factor(names);
        if (g_lex_failed != 0)
            return NULL;
        node = lib_expr_new(FIT_LIBEXPR_NOT, "!");
        node->children = list_create_sized(1);
        list_add(node->children, child);
    } else if (g_lex_token == FIT_TOK_LPAREN) {
        lex_next_token();
        node = lib_parse_or(names);
        lex_expect(FIT_TOK_RPAREN);
    } else {
        lex_error(1, "(, ! or variable expected", 0);
        return NULL;
    }
    if (g_lex_failed == 0)
        return node;
    free(node);
    return NULL;
}

/* Error exit of lib_parse_and / lib_parse_or when one of them finds the
 * error flag set after its first operand.  The first time within a GATE
 * nothing happens; the second time the job ends with the internal error
 * ("INTERNAL ERROR", exit status 7).  The counter is reset at the start
 * of each GATE.  This is deliberate: it fixes how a malformed equation in
 * the library ends the run. */
static void lib_names_freed(void)
{
    g_names_freed++;
    if (g_names_freed >= 2)
        lib_internal_error("pin name list freed twice");
}

/* factor { ('*' | '.') factor } */
static FitLibExpr *lib_parse_and(FitList *names)
{
    FitLibExpr *first, *node;

    if (g_lex_token == FIT_TOK_EOF)
        return NULL;
    first = lib_parse_factor(names);
    g_lex_failed |= (first == NULL);
    if (g_lex_token != FIT_TOK_DOT && g_lex_token != FIT_TOK_STAR) {
        if (g_lex_failed == 0)
            return first;
        lib_names_freed();
        return NULL;
    }
    node = NULL;
    if (g_lex_failed == 0) {
        node = lib_expr_new(FIT_LIBEXPR_AND, "*");
        node->children = list_create();
        list_add(node->children, first);
    }
    while (g_lex_token == FIT_TOK_DOT || g_lex_token == FIT_TOK_STAR) {
        FitLibExpr *next;

        lex_next_token();
        next = lib_parse_factor(names);
        g_lex_failed |= (next == NULL);
        if (g_lex_failed == 0)
            list_add(node->children, next);
    }
    if (g_lex_failed == 0)
        return node;
    if (node != NULL)
        free(node);             /* its operands are not freed */
    else
        lib_names_freed();
    return NULL;
}

/* term { '+' term } */
static FitLibExpr *lib_parse_or(FitList *names)
{
    FitLibExpr *first, *node;

    if (g_lex_token == FIT_TOK_EOF)
        return NULL;
    first = lib_parse_and(names);
    g_lex_failed |= (first == NULL);
    if (g_lex_token != FIT_TOK_PLUS) {
        if (g_lex_failed != 0) {
            lib_names_freed();
            return NULL;
        }
        return first;
    }
    node = NULL;
    if (g_lex_failed == 0) {
        node = lib_expr_new(FIT_LIBEXPR_OR, "+");
        node->children = list_create();
        list_add(node->children, first);
    }
    while (g_lex_token == FIT_TOK_PLUS) {
        FitLibExpr *next;

        lex_next_token();
        next = lib_parse_and(names);
        /* a null term (end of the file) is not appended; the ';' check
         * that follows then fails */
        if (g_lex_failed == 0 && next != NULL)
            list_add(node->children, next);
    }
    if (g_lex_failed != 0) {
        if (node != NULL)
            free(node);         /* its operands are not freed */
        else
            lib_names_freed();
        return NULL;
    }
    return node;
}

/* right side of a GATE equation up to and including the ';'.
 * Returns 0 for CONST0 / CONST1, 1 for an expression. */
static int lib_parse_equation(FitLibExpr **expr, FitList *names)
{
    *expr = NULL;
    if (g_lex_token != KW_CONST0 && g_lex_token != KW_CONST1) {
        *expr = lib_parse_or(names);
        lex_expect(FIT_TOK_SEMI);
        return 1;
    }
    if (g_lex_token == KW_CONST0)
        *expr = lib_expr_new(FIT_LIBEXPR_CONST0, "CONST0");
    else
        *expr = lib_expr_new(FIT_LIBEXPR_CONST1, "CONST1");
    (*expr)->children = NULL;
    lex_next_token();
    lex_expect(FIT_TOK_SEMI);
    return 0;
}

/* the expression as a function tree.  An ID node carries a copy
 * of the pin name where a design tree has the variable id. */
static FitFctTree *lib_expr_to_tree(FitLibExpr *e)
{
    FitFctTree *t = fcttree_new();

    switch (e->op) {
    case FIT_LIBEXPR_NOT:
        t->sel = FIT_SEL_NOT;
        t->u.children = list_create_sized(1);
        list_add(t->u.children, lib_expr_to_tree(e->children->items[0]));
        break;
    case FIT_LIBEXPR_CONST1:
        t->sel = FIT_SEL_CONST;
        t->u.value = 1;
        break;
    case FIT_LIBEXPR_VAR:
        t->sel = FIT_SEL_ID;
        t->u.children = (FitList *)(void *)util_strdup(e->text);
        break;
    case FIT_LIBEXPR_AND:
    case FIT_LIBEXPR_OR:
        t->sel = (e->op == FIT_LIBEXPR_AND) ? FIT_SEL_AND : FIT_SEL_OR;
        t->u.children = list_create_sized(LCOUNT(e->children));
        for (int i = 0; i < LCOUNT(e->children); i++)
            list_add(t->u.children, lib_expr_to_tree(e->children->items[i]));
        break;
    case FIT_LIBEXPR_CONST0:
        t->sel = FIT_SEL_CONST;
        t->u.value = 0;
        break;
    default:
        break;
    }
    return t;
}

/* g_timing_count real numbers as a list of float * */
static FitList *lib_parse_timing(void)
{
    FitList *l = list_create_sized(g_timing_count);

    for (int i = 0; i < g_timing_count; i++) {
        float v = 0.0f;
        float *item;

        if (!lib_parse_real(&v)) {
            lex_error(1, "Timing parameter is not real", 0);
            return NULL;            /* l is not freed */
        }
        item = util_calloc(1, sizeof *item);
        *item = v;
        list_add(l, item);
    }
    return l;
}

/* phase keyword of a PIN line: 1 and the phase, or 0 */
static int lib_pin_phase(int *phase)
{
    if (g_lex_token == KW_INV)
        *phase = 0;
    else if (g_lex_token == KW_UNKNOWN)
        *phase = 2;
    else if (g_lex_token == KW_NONINV)
        *phase = 1;
    else
        return 0;
    return 1;
}

static void *lib_strdup_item(void *s)      /* FitListCopy around util_strdup */
{
    return util_strdup(s);
}

/* the PIN lines of a gate or of one related output.  `names`
 * are the input pins that may appear.  mode 1 (GATE): every pin must have
 * its line; otherwise the lines stop at the first token that is not PIN.
 * Returns a list of FitLibPin, NULL after an error. */
static FitList *lib_parse_pins(FitList *names, int mode)
{
    FitList *rest = list_map_copy(names, lib_strdup_item);
    FitList *pins;
    FitList *timing;
    int phase = 0;
    int total, first, done;

    lex_expect(KW_PIN);
    if (g_lex_failed != 0)
        return NULL;
    pins = list_create_sized(LCOUNT(names));
    if (g_lex_token == FIT_TOK_STAR) {
        FitList *shared = NULL;

        lex_next_token();
        if (!lib_pin_phase(&phase)) {
            lex_error(1, "Unknown Pin Phase (INV, NONINV or UNKNOWN allowed)", 0);
            return NULL;
        }
        lex_next_token();
        timing = lib_parse_timing();
        if (g_lex_failed != 0)
            return NULL;
        for (int i = 0; i < LCOUNT(rest); i++) {
            FitLibPin *p = util_calloc(1, sizeof *p);

            p->phase = phase;
            p->name = util_strdup(rest->items[i]);
            if (i == 0) {
                shared = list_create_sized(LCOUNT(timing));
                for (int k = 0; k < LCOUNT(timing); k++) {
                    float *item = util_calloc(1, sizeof *item);

                    *item = *(float *)timing->items[k];
                    list_add(shared, item);
                }
            }
            p->timing = shared;
            list_add(pins, p);
        }
        if (timing != NULL)
            list_free_all(&timing, util_free_ptr);
        if (rest != NULL)
            list_free_all(&rest, util_free_ptr);
        return pins;
    }
    first = 1;
    total = LCOUNT(rest);
    for (done = 0; done < total; done++) {
        FitLibPin *p;
        int pos;

        if (!first) {
            if (g_lex_token != KW_PIN) {
                if (mode != 1)
                    break;
                lex_error(1, "PIN expected", 0);
                fprintf(g_msg, "\tThe following pins are not defined : ");
                util_print_str_list(g_msg, rest, " - ");
                fprintf(g_msg, "\n");
            }
            if (g_lex_failed != 0)
                return NULL;
            lex_next_token();
        } else {
            first = 0;
        }
        if (g_lex_token != FIT_TOK_IDENT) {
            lex_error(1, "Variable or * expected", 0);
            return NULL;
        }
        pos = list_find(rest, g_lex_text, lib_str_ieq);
        if (pos == 0) {
            lex_error_text("Undefined (or already declared) variable in Pin List : ", g_lex_text);
            return NULL;
        }
        lex_next_token();
        if (!lib_pin_phase(&phase)) {
            lex_error(1, "Unknown Pin Phase (INV, NONINV or UNKNOWN allowed)", 0);
            return NULL;
        }
        lex_next_token();
        timing = lib_parse_timing();
        if (g_lex_failed != 0)
            return NULL;
        p = util_calloc(1, sizeof *p);
        p->phase = phase;
        p->name = util_strdup(rest->items[pos - 1]);
        p->timing = timing;
        list_add(pins, p);
        free(rest->items[pos - 1]);
        list_del_insert(rest, pos);
    }
    if (rest != NULL)
        list_destroy(&rest);        /* names without a PIN line are not freed */
    return pins;
}

/* GATE (the keyword has been read) */
static FitLibGate *lib_parse_gate(void)
{
    FitLibGate *g;
    FitLibComb *c;
    FitLibExpr *expr = NULL;
    FitList *names;
    FitList *pins = NULL;
    char out_name[FIT_NAME_MAX];
    char *name;
    float area = 0.0f, fanout = 0.0f;
    int dont_touch = 0, dont_use = 0;
    int is_expr;

    if (!lib_parse_int(&dont_touch)) {
        lex_error(2, "Don't Touch is not integer", FIT_TOK_IDENT, 0);
        return NULL;
    }
    if (!lib_parse_int(&dont_use)) {
        lex_error(2, "Don't Use is not integer", FIT_TOK_IDENT, 0);
        return NULL;
    }
    name = lib_parse_name();
    if (!lib_parse_real(&area)) {
        lex_error(2, "Area is not real", FIT_TOK_IDENT, 0);
        return NULL;
    }
    if (!lib_parse_real(&fanout)) {
        fanout = g_lib_default_fanout;
        g_lex_failed = 0;
    }
    names = list_create();
    g_names_freed = 0;
    util_str_copy_n(out_name, g_lex_text, sizeof out_name, 1);
    lex_next_token();
    if (g_lex_failed != 0)
        return NULL;
    lex_expect(FIT_TOK_EQUAL);
    if (g_lex_failed != 0)
        return NULL;
    is_expr = lib_parse_equation(&expr, names);
    if (g_lex_failed != 0)
        return NULL;
    if (is_expr) {
        g_timing_count = 6;
        pins = lib_parse_pins(names, 1);
        if (g_lex_failed != 0)
            return NULL;
    }
    /* A name that lost its closing quote is null here when the missing
     * second number has cleared the error flag again; that ends the job
     * with an internal error. */
    if (name == NULL || expr == NULL)
        lib_internal_error("gate without name");
    c = util_calloc(1, sizeof *c);
    util_str_copy_n(c->name, name, sizeof c->name, 0);
    util_str_copy_n(c->out_name, out_name, sizeof c->out_name, 0);
    c->area = is_expr ? area : 0.0f;
    c->max_fanout = fanout;
    c->expr = expr;
    c->tree = lib_expr_to_tree(expr);
    c->pins = pins;
    g = util_calloc(1, sizeof *g);
    g->kind = FIT_GATE_COMB;
    g->dont_touch = dont_touch;
    g->dont_use = dont_use;
    g->cell = c;
    list_destroy(&names);       /* neither the pin names nor `name` are freed */
    return g;
}

/* ------------------------------------------------------------------ */
/* PGATE                                                               */
/* ------------------------------------------------------------------ */

/* the current token as a pin name (new string in *ref).  With
 * check != 0 the name must not be in `seen` yet and is added to it (the
 * list shares the string). */
static int lib_take_pin_name(char **ref, int check, FitList *seen)
{
    *ref = util_strdup(g_lex_text);
    if (check != 0) {
        if (list_find(seen, *ref, lib_str_ieq) != 0) {
            lex_error(1, "Pin already defined or related", 0);
            return 0;
        }
        list_add(seen, *ref);
    }
    return 1;
}

/* a pin that may be absent ('*') and, if pol is given, may be
 * written with a leading '!' (polarity -1 instead of 1). */
static int lib_take_opt_pin(char **ref, int *pol, int check, FitList *seen)
{
    if (pol != NULL)
        *pol = 1;
    if (g_lex_token == FIT_TOK_BANG) {
        if (pol == NULL) {
            lex_error(2, "! not allowed for this Pin", FIT_TOK_IDENT, 0);
            return 0;
        }
        *pol = -1;
        lex_next_token();
        return lib_take_pin_name(ref, check, seen);
    }
    if (g_lex_token == FIT_TOK_STAR) {
        *ref = NULL;
        return 1;
    }
    return lib_take_pin_name(ref, check, seen);
}

/* { RELATED_OUTPUT_PIN pin real pins }.  Returns the list of
 * FitLibOutPin (empty if there is no section), NULL after an error. */
static FitList *lib_parse_related_outputs(FitList *inputs, FitList *outputs, int mode)
{
    FitList *seen = list_create();
    FitList *result = list_create();

    for (;;) {
        FitLibOutPin *o;
        FitList *pins;
        char *name = NULL;
        float fanout = 0.0f;
        int ok;

        if (g_lex_token != KW_RELATED_OUTPUT_PIN) {
            list_destroy(&seen);
            return result;
        }
        lex_next_token();
        ok = lib_take_pin_name(&name, 1, seen);
        if (list_find(outputs, name, lib_str_ieq) == 0) {
            lex_error(1, "Undefined OutPut Pin", 0);
            return NULL;
        }
        if (!ok)
            return NULL;
        lex_next_token();
        if (!lib_parse_real(&fanout)) {
            lex_error(1, "Max Fanout is not Real", 0);
            return NULL;
        }
        if (g_lex_token != KW_PIN) {
            lex_error(1, "PIN expected", 0);
            return NULL;
        }
        pins = lib_parse_pins(inputs, mode);
        if (g_lex_failed != 0)
            return NULL;
        o = util_calloc(1, sizeof *o);
        o->name = name;
        o->max_fanout = fanout;
        o->pins = pins;
        list_add(result, o);
    }
}

/* the common end of the PGATE and UGATE parsers: the related outputs as a
 * fresh list (list_map_copy with list_identity); NULL after an error */
static FitList *lib_take_related_outputs(FitList *inputs, FitList *outputs, int mode)
{
    FitList *parsed = lib_parse_related_outputs(inputs, outputs, mode);
    FitList *copy = list_map_copy(parsed, list_identity);

    if (parsed != NULL)
        list_destroy(&parsed);
    return copy;
}

/* TG DG LG JKG DMUXG LMUXG RSG RSL */
static FitLibSeq *lib_parse_seq(int kind)
{
    FitLibSeq *s = util_calloc(1, sizeof *s);
    FitList *inputs = list_create();
    FitList *outputs = list_create();
    float area = 0.0f;

    s->name = lib_parse_name();
    if (!lib_parse_real(&area)) {
        lex_error(1, "Area is not real", 0);
        return NULL;
    }
    s->area = area;
    if (!lib_take_pin_name(&s->clk, 1, inputs))
        return NULL;
    lex_next_token();
    switch (g_lex_token) {
    case KW_RECK: s->clk_kind = FIT_CLK_RECK; break;
    case KW_FECK: s->clk_kind = FIT_CLK_FECK; break;
    case KW_L1:   s->clk_kind = FIT_CLK_L1;   break;
    case KW_L0:   s->clk_kind = FIT_CLK_L0;   break;
    default:
        lex_error(1, "Type of clock expected", 0);
        return NULL;
    }
    lex_next_token();
    if (!lib_take_opt_pin(&s->ar, &s->ar_pol, 1, inputs))
        return NULL;
    lex_next_token();
    if (!lib_take_opt_pin(&s->ap, &s->ap_pol, 1, inputs))
        return NULL;
    lex_next_token();
    if (!lib_take_opt_pin(&s->ce, &s->ce_pol, 1, inputs))
        return NULL;
    lex_next_token();
    if (!lib_take_opt_pin(&s->oe, &s->oe_pol, 1, inputs))
        return NULL;
    lex_next_token();
    switch (kind) {
    case FIT_GATE_TFF:
        if (!lib_take_opt_pin(&s->d, NULL, 1, inputs))
            return NULL;
        break;
    case FIT_GATE_DFF:
    case FIT_GATE_LATCH:
        if (!lib_take_pin_name(&s->d, 1, inputs))
            return NULL;
        break;
    case FIT_GATE_JK:
    case FIT_GATE_RS:
    case FIT_GATE_RSL:
        if (!lib_take_opt_pin(&s->j, &s->j_pol, 1, inputs))
            return NULL;
        lex_next_token();
        if (!lib_take_opt_pin(&s->k, &s->k_pol, 1, inputs))
            return NULL;
        break;
    case FIT_GATE_DMUX:
    case FIT_GATE_LMUX:
        if (!lib_take_pin_name(&s->mux_sel, 1, inputs))
            return NULL;
        lex_next_token();
        if (!lib_take_pin_name(&s->mux_a, 1, inputs))
            return NULL;
        lex_next_token();
        if (!lib_take_pin_name(&s->mux_b, 1, inputs))
            return NULL;
        break;
    default:
        break;
    }
    lex_next_token();
    if (!lib_take_opt_pin(&s->q, NULL, 1, outputs))
        return NULL;
    lex_next_token();
    if (!lib_take_opt_pin(&s->qn, NULL, 1, outputs))
        return NULL;
    lex_next_token();
    if (!lib_take_opt_pin(&s->z, NULL, 1, outputs))
        return NULL;
    lex_next_token();
    g_timing_count = 8;
    s->outs = lib_take_related_outputs(inputs, outputs, kind);
    if (s->outs == NULL)
        return NULL;
    list_destroy(&inputs);
    list_destroy(&outputs);
    return s;
}

/* THG, ITHG:  name real in oe out */
static FitLibSeq *lib_parse_th(void)
{
    FitLibSeq *s = util_calloc(1, sizeof *s);
    FitList *inputs = list_create();
    FitList *outputs = list_create();
    float area = 0.0f;

    if (!lib_take_pin_name(&s->name, 0, inputs))
        return NULL;
    lex_next_token();
    if (!lib_parse_real(&area)) {
        lex_error(1, "Area is not real", 0);
        return NULL;
    }
    s->area = area;
    if (!lib_take_pin_name(&s->d, 1, inputs))
        return NULL;
    lex_next_token();
    if (!lib_take_opt_pin(&s->oe, &s->oe_pol, 1, inputs))
        return NULL;
    lex_next_token();
    if (!lib_take_pin_name(&s->q, 1, outputs))
        return NULL;
    lex_next_token();
    g_timing_count = 8;
    s->outs = lib_take_related_outputs(inputs, outputs, FIT_GATE_TH);
    if (s->outs == NULL)
        return NULL;
    list_destroy(&inputs);
    list_destroy(&outputs);
    return s;
}

/* buffer type keyword; *pad_is_output says which side the pad
 * pin belongs to */
static int lib_buf_type(int *type, int *pad_is_output, const char *msg)
{
    switch (g_lex_token) {
    case KW_I_BUF:   *type = FIT_BUF_I;   *pad_is_output = 0; return 1;
    case KW_O_BUF:   *type = FIT_BUF_O;   *pad_is_output = 1; return 1;
    case KW_BI_BUF:  *type = FIT_BUF_BI;  *pad_is_output = 1; return 1;
    case KW_TS_BUF:  *type = FIT_BUF_TS;  *pad_is_output = 1; return 1;
    case KW_CLK_BUF: *type = FIT_BUF_CLK; *pad_is_output = 0; return 1;
    default:
        lex_error(1, msg, 0);
        return 0;
    }
}

/* BUF:  buftype name real in out pad oe */
static FitLibBuf *lib_parse_buf(void)
{
    FitLibBuf *b = util_calloc(1, sizeof *b);
    FitList *inputs, *outputs;
    float area = 0.0f;
    int pad_is_output = 0;

    if (!lib_buf_type(&b->type, &pad_is_output, "Type Of Buffer expected"))
        return NULL;
    lex_next_token();
    if (!lib_take_pin_name(&b->name, 0, NULL))
        return NULL;
    inputs = list_create();
    outputs = list_create();
    lex_next_token();
    if (!lib_parse_real(&area)) {
        lex_error(1, "Area is not real", 0);
        return NULL;
    }
    b->area = area;
    if (!lib_take_opt_pin(&b->in, NULL, 1, inputs))
        return NULL;
    lex_next_token();
    if (!lib_take_opt_pin(&b->out, NULL, 1, outputs))
        return NULL;
    lex_next_token();
    if (!lib_take_opt_pin(&b->pad, NULL, 1, pad_is_output ? outputs : inputs))
        return NULL;
    lex_next_token();
    if (!lib_take_opt_pin(&b->oe, &b->oe_pol, 1, inputs))
        return NULL;
    lex_next_token();
    g_timing_count = 6;
    b->outs = lib_take_related_outputs(inputs, outputs, FIT_GATE_BUF);
    if (b->outs == NULL)
        return NULL;
    list_destroy(&inputs);
    list_destroy(&outputs);
    return b;
}

/* PGATE (the keyword has been read) */
static FitLibGate *lib_parse_pgate(void)
{
    FitLibGate *g;
    void *cell;
    int dont_touch = 0, dont_use = 0;
    int kind;

    if (!lib_parse_int(&dont_touch)) {
        lex_error(1, "Don't Touch is not integer", 0);
        return NULL;
    }
    if (!lib_parse_int(&dont_use)) {
        lex_error(1, "Don't Use is not integer", 0);
        return NULL;
    }
    switch (g_lex_token) {
    case KW_TG:    kind = FIT_GATE_TFF;   break;
    case KW_DG:    kind = FIT_GATE_DFF;   break;
    case KW_LG:    kind = FIT_GATE_LATCH; break;
    case KW_JKG:   kind = FIT_GATE_JK;    break;
    case KW_DMUXG: kind = FIT_GATE_DMUX;  break;
    case KW_LMUXG: kind = FIT_GATE_LMUX;  break;
    case KW_THG:   kind = FIT_GATE_TH;    break;
    case KW_ITHG:  kind = FIT_GATE_ITH;   break;
    case KW_RSG:   kind = FIT_GATE_RS;    break;
    case KW_BUF:   kind = FIT_GATE_BUF;   break;
    case KW_RSL:   kind = FIT_GATE_RSL;   break;
    default:
        lex_error(1, "Type of predefined gate expected", 0);
        return NULL;
    }
    lex_next_token();
    if (kind == FIT_GATE_BUF)
        cell = lib_parse_buf();
    else if (kind == FIT_GATE_TH || kind == FIT_GATE_ITH)
        cell = lib_parse_th();
    else
        cell = lib_parse_seq(kind);
    if (cell == NULL)
        return NULL;
    g = util_calloc(1, sizeof *g);
    g->kind = kind;
    g->dont_touch = dont_touch;
    g->dont_use = dont_use;
    g->cell = cell;
    return g;
}

/* ------------------------------------------------------------------ */
/* UGATE                                                               */
/* ------------------------------------------------------------------ */

/* n pin*n */
static FitList *lib_parse_pin_list(void)
{
    FitList *l;
    int n = 0;

    if (g_lex_token != FIT_TOK_IDENT)
        return NULL;
    if (!lib_parse_int(&n)) {
        lex_error(1, "Number of Pin is not integer", 0);
        return NULL;
    }
    l = list_create_sized(n);
    for (int i = 0; i < n; i++) {
        char *name = NULL;

        if (!lib_take_pin_name(&name, 0, NULL))
            return NULL;
        list_add(l, name);
        lex_next_token();
    }
    return l;
}

/* the words of a UGATE_IS section must be the inputs followed by
 * the outputs of the gate */
static int lib_ugate_is_matches(FitLibUGate *u, FitList *words)
{
    FitList *all;
    int i;

    if (u == NULL)
        return 0;
    all = list_copy(u->inputs);
    list_append(all, u->outputs);
    if (list_equal_sets(all, words, lib_str_ieq)) {
        i = 0;
        while (i < LCOUNT(all) && i < LCOUNT(words)
               && util_str_icmp(all->items[i], words->items[i]) == 0)
            i++;
        if (i == LCOUNT(all)) {
            list_destroy(&all);
            return 1;
        }
    }
    lex_error(1, "Mismatch between UGATE_IS section and pin declaration", 0);
    list_destroy(&all);
    return 0;
}

/* UGATE (the keyword has been read) */
static FitLibGate *lib_parse_ugate(void)
{
    FitLibUGate *u = util_calloc(1, sizeof *u);
    FitLibGate *g;
    FitList *inputs, *outputs, *outs;
    float area = 0.0f;
    int dont_touch = 0, dont_use = 0;

    if (!lib_parse_int(&dont_touch)) {
        lex_error(1, "Don't Touch is not integer", 0);
        return NULL;
    }
    if (!lib_parse_int(&dont_use)) {
        lex_error(1, "Don't Use is not integer", 0);
        return NULL;
    }
    u->name = lib_parse_name();
    if (!lib_parse_real(&area)) {
        lex_error(1, "Area is not real", 0);
        return NULL;
    }
    u->area = area;
    inputs = lib_parse_pin_list();
    outputs = lib_parse_pin_list();
    if (inputs == NULL || outputs == NULL)
        return NULL;
    u->inputs = list_map_copy(inputs, lib_strdup_item);
    u->outputs = outputs;
    g_timing_count = 8;
    outs = lib_parse_related_outputs(inputs, outputs, FIT_GATE_UNKNOWN);
    if (outs == NULL)
        return NULL;
    u->outs = outs;
    u->adder = 0;
    if (util_str_icmp(g_lex_text, "UGATE_IS") == 0) {
        FitList *words = list_create();

        lex_next_token();
        if (util_str_icmp(g_lex_text, "adder1") != 0 && util_str_icmp(g_lex_text, "adder2") != 0) {
            lex_error(1, "UGATE_IS must be adder1 or adder2", 0);
            return NULL;
        }
        if (util_str_icmp(g_lex_text, "adder1") == 0)
            u->adder = 1;
        if (util_str_icmp(g_lex_text, "adder2") == 0)
            u->adder = 2;
        lex_next_token();
        while (util_str_icmp(g_lex_text, "END_UGATE_IS") != 0) {
            /* the end of the file inside the section is reported as an
             * out-of-memory error; deliberate, it fixes the exit status
             * for such a library */
            if (g_lex_token == FIT_TOK_EOF) {
                util_report_error(FIT_ERR_MEMORY, g_empty_str);
                util_abort_job(FIT_EXIT_MEMORY);
            }
            list_add(words, util_strdup(g_lex_text));
            lex_next_token();
        }
        lex_next_token();
        if (!lib_ugate_is_matches(u, words)) {
            lex_error(1, "incorrect UGATE_IS specification", 0);
            return NULL;
        }
        list_free_all(&words, util_free_ptr);
    }
    g = util_calloc(1, sizeof *g);
    g->kind = FIT_GATE_UNKNOWN;
    g->dont_touch = dont_touch;
    g->dont_use = dont_use;
    g->cell = u;
    return g;
}

/* ------------------------------------------------------------------ */
/* The file                                                            */
/* ------------------------------------------------------------------ */

static FitLibHeader *lib_header_new(void)
{
    FitLibHeader *h = util_calloc(1, sizeof *h);

    h->w14 = 3;
    h->l10 = list_create();
    h->l20 = list_create();
    return h;
}

/* keyword table, default delimiters, comments from '#' to the
 * end of the line, default fanout 999 */
static void lib_lex_setup(void)
{
    lex_set_keyword_table(0x21,
                          KW_GATE, "GATE", KW_PIN, "PIN", KW_INV, "INV", KW_NONINV, "NONINV",
                          KW_UNKNOWN, "UNKNOWN", KW_CONST0, "CONST0", KW_CONST1, "CONST1",
                          KW_PGATE, "PGATE", KW_TG, "TG", KW_DG, "DG", KW_LG, "LG",
                          KW_JKG, "JKG", KW_DMUXG, "DMUXG", KW_LMUXG, "LMUXG", KW_THG, "THG",
                          KW_ITHG, "ITHG", KW_RSG, "RSG", KW_BUF, "BUF", KW_RECK, "RECK",
                          KW_FECK, "FECK", KW_L0, "L0", KW_L1, "L1", KW_I_BUF, "I_BUF",
                          KW_O_BUF, "O_BUF", KW_BI_BUF, "BI_BUF", KW_TS_BUF, "TS_BUF",
                          KW_CLK_BUF, "CLK_BUF", KW_UGATE, "UGATE",
                          KW_RELATED_OUTPUT_PIN, "RELATED_OUTPUT_PIN", KW_DEFAULT, "DEFAULT",
                          KW_MAX_FANOUT, "MAX_FANOUT", KW_COMMENT, "COMMENT", KW_RSL, "RSL");
    lex_set_default_tables();
    lex_set_comment_chars('#', '\n');
    g_lib_default_fanout = 999.0f;
}

/* the words of COMMENT "..." joined by single blanks */
static char *lib_parse_comment(void)
{
    char *text = NULL;

    lex_expect(FIT_TOK_DQUOTE);
    while (g_lex_token != FIT_TOK_DQUOTE) {
        /* the end of the file inside the comment is reported as an
         * out-of-memory error (see lib_parse_ugate) */
        if (g_lex_token == FIT_TOK_EOF) {
            util_report_error(FIT_ERR_MEMORY, g_empty_str);
            util_abort_job(FIT_EXIT_MEMORY);
        }
        if (text == NULL) {
            text = util_strdup(g_lex_text);
        } else {
            util_str_append(&text, " ");
            util_str_append(&text, g_lex_text);
        }
        lex_next_token();
    }
    lex_expect(FIT_TOK_DQUOTE);
    return text;
}

int lib_parse_file(const char *path, FitList **gates, FitLibHeader **header)
{
    FILE *f;

    if (path == NULL)
        return 0;
    f = util_fopen(path, "rb");
    if (f == NULL) {
        util_report_error(FIT_ERR_FILE_NOT_FOUND, path);
        return 0;
    }
    if (libbin_is_binary(f)) {
        *gates = NULL;
        *header = NULL;
        if (libbin_read_library(f, gates, header)) {
            util_fclose(f);
            return 1;
        }
        /* When the file ends before the gate count, no list has been
         * stored yet; that ends the job with an internal error. */
        if (*gates == NULL)
            lib_internal_error("gate list not set");
        goto failed;
    }
    util_fclose(f);
    f = util_fopen(path, "r");
    if (f == NULL) {
        util_report_error(FIT_ERR_FILE_NOT_FOUND, path);
        return 0;
    }
    lex_set_streams(f, g_msg);
    lib_lex_setup();
    lex_start();
    lex_next_token();
    if (g_lex_token == FIT_TOK_EOF) {
        util_fclose(f);
        return 0;
    }
    *gates = list_create();
    *header = lib_header_new();
    do {
        FitLibGate *g;

        if (g_lex_eof != 0 || g_lex_failed != 0) {
            util_fclose(f);
            return 1;
        }
        switch (g_lex_token) {
        case KW_GATE:
            lex_next_token();
            g = lib_parse_gate();
            break;
        case KW_PGATE:
            lex_next_token();
            g = lib_parse_pgate();
            break;
        case KW_UGATE:
            lex_next_token();
            g = lib_parse_ugate();
            break;
        case KW_DEFAULT:
            lex_next_token();
            lib_parse_default();
            continue;
        case KW_COMMENT:
            lex_next_token();
            (*header)->comment = lib_parse_comment();
            continue;
        default:
            lex_error(1, "GATE, PGATE or UGATE expected - analysis aborted", 0);
            g_lex_failed = 1;
            goto failed;
        }
        if (g_lex_failed != 0)
            goto failed;
        if (g == NULL) {
            /* A UGATE whose pin count is not a word returns nothing without
             * an error; the library is rejected. */
            g_lex_failed = 1;
            goto failed;
        }
        list_add(*gates, g);
    } while (g_lex_failed == 0);
failed:
    if (*gates != NULL)
        list_free_all(gates, lib_gate_free);
    lib_header_free(header);
    *header = NULL;
    util_fclose(f);
    return 0;
}
