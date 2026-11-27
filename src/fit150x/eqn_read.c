/* Boolean equation input, lowered to the fitter's function trees. */
#include <ctype.h>
#include <stdarg.h>
#include "tt_priv.h"

enum { EQ_WORD = 256, EQ_MAX_SIGNALS = 4096, EQ_MAX_NODES = 65536,
       EQ_MAX_DEPTH = 128, EQ_MAX_BYTES = 4 * 1024 * 1024 };

typedef struct {
    char name[FIT_NAME_MAX + 1];
    int expression, input, output, read, color, line, pin, depth;
} EqSignal;

typedef struct {
    int op, a, b, depth;
} EqNode;

typedef struct {
    FILE *in;
    int ch, token, line, token_line, bytes, failed, nesting;
    int nsignals, nnodes, have_inputs, have_outputs, have_name;
    char word[FIT_NAME_MAX + 1];
    EqSignal *signals;
    EqNode *nodes;
} EqRead;

static void error(EqRead *r, const char *fmt, ...)
{
    va_list ap;
    if (r->failed) return;
    r->failed = 1;
    fprintf(g_msg, "EQN line %d: ", r->token_line);
    va_start(ap, fmt);
    vfprintf(g_msg, fmt, ap);
    va_end(ap);
    fputc('\n', g_msg);
}

static void character(EqRead *r)
{
    if (r->ch == '\n') r->line++;
    r->ch = fgetc(r->in);
    if (++r->bytes > EQ_MAX_BYTES) {
        error(r, "input exceeds %d bytes", EQ_MAX_BYTES);
        r->ch = EOF;
    }
}

static int name_char(int c)
{
    return c > 0 && (isalnum((unsigned char)c) ||
                       strchr("_.$[]", c) != NULL);
}

static void next(EqRead *r)
{
    int n = 0;
    if (r->failed) { r->token = EOF; return; }
    for (;;) {
        while (r->ch != EOF && isspace((unsigned char)r->ch)) character(r);
        if (r->ch != '#') break;
        while (r->ch != EOF && r->ch != '\n') character(r);
    }
    r->token_line = r->line;
    if (name_char(r->ch)) {
        do {
            if (n == FIT_NAME_MAX - 1) {
                error(r, "name exceeds %d characters", FIT_NAME_MAX - 1);
                r->token = EOF;
                return;
            }
            r->word[n++] = (char)r->ch;
            character(r);
        } while (name_char(r->ch));
        r->word[n] = '\0';
        r->token = EQ_WORD;
    } else {
        r->token = r->ch;
        if (r->ch != EOF) {
            if (!r->ch || !strchr("=;()*&+|^!~'", r->ch))
                error(r, "invalid character 0x%02x", (unsigned char)r->ch);
            character(r);
        }
    }
}

static void expect(EqRead *r, int token)
{
    if (r->token != token) error(r, "expected '%c'", token);
    else next(r);
}

static int signal_id(EqRead *r)
{
    if (r->token != EQ_WORD || !strcmp(r->word, "0") ||
        !strcmp(r->word, "1")) {
        error(r, "expected a signal name");
        return 0;
    }
    for (int i = 0; i < r->nsignals; i++)
        if (util_str_ieq(r->signals[i].name, r->word)) return i;
    if (r->nsignals == EQ_MAX_SIGNALS) {
        error(r, "too many signals (limit %d)", EQ_MAX_SIGNALS);
        return 0;
    }
    EqSignal *s = &r->signals[r->nsignals];
    strcpy(s->name, r->word);
    s->expression = -1;
    s->line = r->token_line;
    return r->nsignals++;
}

static int node(EqRead *r, int op, int a, int b)
{
    int depth = 1;
    if (r->failed) return 0;
    if (op != FIT_SEL_CONST && op != FIT_SEL_ID) {
        depth += r->nodes[a].depth;
        if (op != FIT_SEL_NOT && depth <= r->nodes[b].depth)
            depth = r->nodes[b].depth + 1;
    }
    if (depth > EQ_MAX_DEPTH || r->nnodes == EQ_MAX_NODES) {
        error(r, "expression complexity limit exceeded");
        return 0;
    }
    r->nodes[r->nnodes] = (EqNode){op, a, b, depth};
    return r->nnodes++;
}

static int expression(EqRead *r, int precedence);

static int unary(EqRead *r)
{
    int result = 0, token = r->token;
    if (r->failed) return 0;
    if (++r->nesting > EQ_MAX_DEPTH) {
        error(r, "expression nesting limit exceeded");
        r->nesting--;
        return 0;
    }
    if (token == '!' || token == '~') {
        next(r);
        result = unary(r);
        result = node(r, FIT_SEL_NOT, result, 0);
    } else if (token == '(') {
        next(r);
        result = expression(r, 0);
        expect(r, ')');
    } else if (token == EQ_WORD) {
        if (!strcmp(r->word, "0") || !strcmp(r->word, "1"))
            result = node(r, FIT_SEL_CONST, r->word[0] - '0', 0);
        else {
            int id = signal_id(r);
            r->signals[id].read = 1;
            result = node(r, FIT_SEL_ID, id, 0);
        }
        next(r);
    } else error(r, "expected an expression");
    while (!r->failed && r->token == '\'') {
        next(r);
        result = node(r, FIT_SEL_NOT, result, 0);
    }
    r->nesting--;
    return result;
}

static int expression(EqRead *r, int precedence)
{
    int left = unary(r);
    while (!r->failed) {
        int token = r->token, level, op, implicit = 0;
        if (token == '+' || token == '|') { level = 1; op = FIT_SEL_OR; }
        else if (token == '^') { level = 2; op = FIT_SEL_XOR; }
        else if (token == '*' || token == '&') { level = 3; op = FIT_SEL_AND; }
        else if (token == EQ_WORD || token == '(' || token == '!' || token == '~') {
            level = 3; op = FIT_SEL_AND; implicit = 1;
        } else break;
        if (level <= precedence) break;
        if (!implicit) next(r);
        int right = expression(r, level);
        left = node(r, op, left, right);
    }
    return left;
}

static void declaration(EqRead *r, int output)
{
    int *seen = output ? &r->have_outputs : &r->have_inputs;
    if (*seen) error(r, "duplicate %s declaration", output ? "OUTORDER" : "INORDER");
    *seen = 1;
    next(r);
    expect(r, '=');
    while (!r->failed && r->token != ';') {
        int id = signal_id(r);
        EqSignal *s = &r->signals[id];
        if (s->input || s->output)
            error(r, "signal '%s' occurs in more than one port declaration", s->name);
        if (output) s->output = 1;
        else s->input = 1;
        next(r);
    }
    expect(r, ';');
}

static void read_equations(EqRead *r)
{
    next(r);
    while (!r->failed && r->token != EOF) {
        if (r->token != EQ_WORD) { error(r, "expected a statement"); break; }
        if (util_str_ieq(r->word, "INORDER")) declaration(r, 0);
        else if (util_str_ieq(r->word, "OUTORDER")) declaration(r, 1);
        else if (util_str_ieq(r->word, "NAME")) {
            if (r->have_name++) error(r, "duplicate NAME declaration");
            next(r);
            expect(r, '=');
            if (r->token != EQ_WORD) error(r, "expected a design name");
            next(r);
            expect(r, ';');
        } else {
            int id = signal_id(r);
            if (r->signals[id].expression >= 0)
                error(r, "multiple equations for '%s'", r->signals[id].name);
            next(r);
            expect(r, '=');
            r->signals[id].expression = expression(r, 0);
            expect(r, ';');
        }
    }
    if (ferror(r->in)) error(r, "cannot read input");
}

/* Check the equation graph before handing it to transformations which
 * assume acyclic functions. The bound also limits later expansion depth. */
static int visit_signal(EqRead *r, int id, int depth);

static int visit_node(EqRead *r, int id, int depth)
{
    EqNode *n = &r->nodes[id];
    if (r->failed) return 0;
    if (depth > EQ_MAX_DEPTH) { error(r, "logic depth limit exceeded"); return 0; }
    if (n->op == FIT_SEL_ID) return visit_signal(r, n->a, depth + 1) + 1;
    if (n->op != FIT_SEL_CONST) {
        int a = visit_node(r, n->a, depth + 1);
        int b = n->op == FIT_SEL_NOT ? 0 : visit_node(r, n->b, depth + 1);
        return (a > b ? a : b) + 1;
    }
    return 1;
}

static int visit_signal(EqRead *r, int id, int depth)
{
    EqSignal *s = &r->signals[id];
    if (s->input || r->failed) return 0;
    if (s->color == 2) return s->depth;
    r->token_line = s->line;
    if (s->color == 1) { error(r, "combinational cycle at '%s'", s->name); return 0; }
    s->color = 1;
    s->depth = visit_node(r, s->expression, depth);
    if (s->depth > EQ_MAX_DEPTH) error(r, "logic depth limit exceeded");
    s->color = 2;
    return s->depth;
}

static FitFctTree *lower(EqRead *r, int id)
{
    EqNode *n = &r->nodes[id];
    if (n->op == FIT_SEL_CONST) return fcttree_new_const(n->a);
    if (n->op == FIT_SEL_ID) return fcttree_new_id(n->a + 1);
    if (n->op == FIT_SEL_NOT) return fcttree_new_not(lower(r, n->a));
    return fcttree_new_binary(n->op, lower(r, n->a), lower(r, n->b));
}

static void pin_constraints(EqRead *r, FitList *params)
{
    char *option = param_get(params, "pinfile"), *path;
    FILE *file;
    if (!option) return;
    if (*option && !util_str_ieq(option, "ON")) path = util_strdup(option);
    else {
        path = util_strdup(g_opt.out_base);
        util_str_append(&path, ".pin");
    }
    free(option);
    file = fopen(path, "r");
    if (!file) error(r, "cannot open pin file '%s'", path);
    else {
        char line[4096];
        r->token_line = 0;
        while (!r->failed && fgets(line, sizeof(line), file)) {
            char *name = line, *colon, *end;
            long number;
            int id = -1;
            r->token_line++;
            if (!strchr(line, '\n') && !feof(file)) {
                error(r, "pin file line is too long");
                break;
            }
            while (isspace((unsigned char)*name)) name++;
            if (!*name || *name == '#' || !strncmp(name, "//", 2) ||
                !strncasecmp(name, "CHIP ", 5)) continue;
            if (*name == '"') {
                name++;
                end = strchr(name, '"');
                if (!end) { error(r, "unterminated pin name"); break; }
                *end++ = '\0';
                while (isspace((unsigned char)*end)) end++;
                colon = *end == ':' ? end : NULL;
            } else {
                colon = strchr(name, ':');
                if (colon) {
                    *colon = '\0';
                    end = colon;
                    while (end > name && isspace((unsigned char)end[-1])) *--end = '\0';
                }
            }
            if (!colon) { error(r, "expected 'signal : pin' in '%s'", path); break; }
            number = dev_parse_pin(params, colon + 1, &end);
            while (isspace((unsigned char)*end)) end++;
            if (!number ||
                (*end && *end != '#' && strncmp(end, "//", 2))) {
                error(r, "invalid pin assignment in '%s'", path);
                break;
            }
            for (int i = 0; i < r->nsignals; i++)
                if (util_str_ieq(name, r->signals[i].name)) id = i;
            if (id < 0 || (!r->signals[id].input && !r->signals[id].output)) {
                error(r, "pin assignment names unknown port '%s'", name);
                break;
            }
            if (r->signals[id].pin && r->signals[id].pin != number)
                error(r, "conflicting assignments for '%s'", name);
            for (int i = 0; i < r->nsignals; i++)
                if (i != id && r->signals[i].pin == number)
                    error(r, "pin %ld assigned to both '%s' and '%s'",
                          number, name, r->signals[i].name);
            r->signals[id].pin = (int)number;
        }
        if (ferror(file)) error(r, "cannot read pin file '%s'", path);
        fclose(file);
        if (!r->failed) {
            g_opt.pinfile = 1;
            g_opt.pin_filename = util_strdup(path);
        }
    }
    free(path);
}

int eqn_read(FILE *in, FitEntity **entity, FitList *params)
{
    EqRead r = {0};
    r.in = in;
    r.line = r.token_line = 1;
    r.signals = util_calloc(EQ_MAX_SIGNALS, sizeof(*r.signals));
    r.nodes = util_calloc(EQ_MAX_NODES, sizeof(*r.nodes));
    character(&r);
    read_equations(&r);
    for (int i = 0; !r.failed && i < r.nsignals; i++) {
        EqSignal *s = &r.signals[i];
        r.token_line = s->line;
        if (!r.have_inputs && !s->output && s->expression < 0) s->input = 1;
        if (!r.have_outputs && s->expression >= 0 && !s->read) s->output = 1;
        if (s->input && s->expression >= 0)
            error(&r, "input '%s' also has an equation", s->name);
        else if (!s->input && s->expression < 0)
            error(&r, "undefined signal '%s'", s->name);
    }
    for (int i = 0; !r.failed && i < r.nsignals; i++) visit_signal(&r, i, 0);
    int noutputs = 0;
    for (int i = 0; i < r.nsignals; i++) noutputs += r.signals[i].output;
    if (!r.failed && !noutputs) error(&r, "design has no outputs");
    if (!r.failed) pin_constraints(&r, params);
    if (!r.failed) {
        FitEntity *e = entity_new(NULL);
        e->inputs = list_create();
        e->outputs = list_create();
        e->locals = list_create();
        e->fcts = list_create();
        e->net = netlist_new();
        e->net->items = list_create();
        for (int i = 0; i < r.nsignals; i++) {
            EqSignal *s = &r.signals[i];
            FitVariable *v = var_new(0);
            v->id = i + 1;
            strcpy(v->name, s->name);
            list_add(s->input ? e->inputs : s->output ? e->outputs : e->locals, v);
            if (s->pin) {
                FitPreassignItem *p = util_calloc(1, sizeof(*p));
                p->name = util_strdup(s->name);
                p->pin = s->pin;
                list_add(g_preassign, p);
            }
            if (s->expression >= 0) {
                FitFct *f = fct_new();
                f->var_id = v->id;
                f->tree[0] = lower(&r, s->expression);
                list_add(e->fcts, f);
            }
        }
        e->n_in = e->inputs->count;
        e->n_out = e->outputs->count;
        e->n_local = e->locals->count;
        *entity = e;
    }
    free(r.signals);
    free(r.nodes);
    return !r.failed;
}
