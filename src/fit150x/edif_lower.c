/* Convert EDIF primitive instances to the fitter's Boolean functions and
 * register/pad netlist. No textual intermediate or external cell library
 * is needed for the built-in primitives. */
#include <ctype.h>
#include <errno.h>
#include "edif_priv.h"

static FitVariable *variable(EdRead *r, const char *name, int cls)
{
    FitVariable *v = var_new(0);
    v->id = ++r->next_id;
    snprintf(v->name, sizeof(v->name), "%s", name);
    list_add(cls == ED_INPUT ? r->entity->inputs :
             cls == ED_OUTPUT ? r->entity->outputs : r->entity->locals, v);
    return v;
}

static FitVariable *wire_var(EdRead *r, EdWire *w)
{
    char name[64];
    w = ed_root(w);
    if (!w->var) {
        snprintf(name, sizeof(name), "__edif_net_%d", w->number);
        w->var = variable(r, name, 0);
    }
    return w->var;
}

static void equation(EdRead *r, FitVariable *v, FitFctTree *tree)
{
    FitFct *f = fct_new();
    f->var_id = v->id;
    f->tree[0] = tree;
    list_add(r->entity->fcts, f);
}

static EdPort *find_pin(EdFrame *f, const char *name)
{
    for (EdPort *p = f->view->ports; p; p = p->next)
        if (util_str_ieq(p->name, name))
            return p;
    return NULL;
}

static EdWire *pin(EdRead *r, EdFrame *f, const char *name, int dir, int optional)
{
    EdPort *p;
    EdWire *w;
    if (!name)
        return NULL;
    p = find_pin(f, name);
    if (!p) {
        if (optional)
            return NULL;
        ed_error(r, f->node, "cell '%s' requires pin '%s'", f->view->model, name);
    }
    if (p->width != 1 || p->direction != dir)
        ed_error(r, p->node, "pin '%s' has the wrong width or direction for '%s'",
                 name, f->view->model);
    f->used[p->offset] = 1;
    w = ed_root(f->wires[p->offset]);
    if (!w->connected) {
        if (optional || dir == ED_OUTPUT)
            return NULL;
        ed_error(r, f->node, "unconnected input '%s' on '%s'", name, f->name);
    }
    return w;
}

static FitFctTree *input(EdRead *r, EdFrame *f, const char *name)
{
    EdWire *w = pin(r, f, name, ED_INPUT, 0);
    w->read = 1;
    return fcttree_new_id(wire_var(r, w)->id);
}

static void drive(EdRead *r, EdFrame *f, EdWire *w)
{
    if (++w->drivers > 1)
        ed_error(r, f ? f->node : NULL, "multiple drivers on net %d%s%s",
                 w->number, f ? " at " : "", f ? f->name : "");
}

static void output(EdRead *r, EdFrame *f, const char *name, FitFctTree *tree)
{
    EdWire *w = pin(r, f, name, ED_OUTPUT, 0);
    if (!w) {
        fcttree_free(&tree);
        return;
    }
    drive(r, f, w);
    equation(r, wire_var(r, w), tree);
}

static FitFctTree *binary(int op, FitFctTree *a, FitFctTree *b)
{
    return fcttree_new_binary(op, a, b);
}

static FitFctTree *mux(FitFctTree *s, FitFctTree *a, FitFctTree *b)
{
    return binary(FIT_SEL_OR,
                  binary(FIT_SEL_AND, fcttree_new_not(fcttree_copy(s, NULL)), a),
                  binary(FIT_SEL_AND, s, b));
}

/* Netlist inputs to a register/pad must be output functions (or physical
 * inputs), while its driven signals are aliased inputs. Keeping this
 * boundary explicit also breaks sequential feedback for cycle checking. */
static char *control_tree(EdRead *r, FitFctTree *tree)
{
    char name[64];
    FitVariable *v;
    snprintf(name, sizeof(name), "__edif_term_%d", r->next_id + 1);
    v = variable(r, name, ED_OUTPUT);
    equation(r, v, tree);
    lib_alias_add(r->entity->net, v->name, NULL);
    return util_strdup(v->name);
}

static char *control(EdRead *r, EdFrame *f, const char *name, int optional)
{
    EdWire *w = pin(r, f, name, ED_INPUT, optional);
    if (!w)
        return NULL;
    w->read = 1;
    return control_tree(r, fcttree_new_id(wire_var(r, w)->id));
}

static char *state_output(EdRead *r, EdFrame *f, const char *name, int dir)
{
    EdWire *w = pin(r, f, name, dir, 1);
    FitVariable *v;
    if (!w)
        return NULL;
    drive(r, f, w);
    w->sequential = 1;
    v = wire_var(r, w);
    lib_move_variable(v->id, r->entity->locals, r->entity->inputs);
    lib_alias_add(r->entity->net, v->name, NULL);
    return util_strdup(v->name);
}

/* A lone complementary output can hold the inverted state directly:
 * complement D and exchange reset/preset. For both outputs, derive QN
 * from Q explicitly so register alias resolution cannot lose its polarity. */
static char *register_outputs(EdRead *r, EdFrame *f, FitInstance *inst, int kind,
                              const char *q, const char *qn)
{
    char *positive = state_output(r, f, q, ED_OUTPUT);
    EdWire *negative = pin(r, f, qn, ED_OUTPUT, 1);
    if (!positive && negative && !inst->pin &&
        (kind == FIT_GATE_DFF || kind == FIT_GATE_LATCH || kind == FIT_GATE_TFF)) {
        char *reset = inst->ar;
        int reset_pol = inst->ar_pol;
        if (kind != FIT_GATE_TFF) {
            FitFct *data = entity_find_fct(r->entity, entity_var_id(r->entity, inst->q));
            if (!data)
                ed_error(r, f->node, "register '%s' requires a data input", f->name);
            data->tree[0] = fcttree_new_not(data->tree[0]);
        }
        inst->ar = inst->ap;
        inst->ar_pol = inst->ap_pol;
        inst->ap = reset;
        inst->ap_pol = reset_pol;
        return state_output(r, f, qn, ED_OUTPUT);
    }
    if (negative) {
        if (!positive) {
            char name[64];
            FitVariable *v;
            snprintf(name, sizeof(name), "__edif_state_%d", r->next_id + 1);
            v = variable(r, name, ED_INPUT);
            lib_alias_add(r->entity->net, v->name, NULL);
            positive = util_strdup(v->name);
        }
        drive(r, f, negative);
        equation(r, wire_var(r, negative),
                 fcttree_new_not(fcttree_new_id(entity_var_id(r->entity, positive))));
    }
    return positive;
}

static FitInstance *instance(EdRead *r, EdFrame *f, int kind)
{
    FitNetItem *item = netitem_new();
    FitInstance *inst = instance_new();
    char name[64];
    item->kind = kind;
    item->cell = inst;
    snprintf(name, sizeof(name), "__edif_cell_%d", tt_count(r->entity->net->items));
    inst->inst_name = util_strdup(name);
    inst->name = util_strdup(f->view->model);
    inst->ar_pol = inst->ap_pol = inst->ce_pol = inst->oe_pol = 1;
    list_add(r->entity->net->items, item);
    return inst;
}

static const char *choice(EdRead *r, EdFrame *f, const char *a,
                          const char *b, const char *c, const char *d)
{
    const char *names[] = {a, b, c, d};
    const char *found = NULL;
    for (int i = 0; i < 4; i++) {
        if (!names[i] || !find_pin(f, names[i]))
            continue;
        if (found)
            ed_error(r, f->node, "ambiguous primitive pin names '%s' and '%s'", found, names[i]);
        found = names[i];
    }
    if (!found)
        ed_error(r, f->node, "missing primitive pin '%s' in '%s'", a, f->view->model);
    return found;
}

static void buffer(EdRead *r, EdFrame *f, const char *in, const char *oe,
                   const char *out, const char *pad)
{
    FitNetItem *item = netitem_new();
    FitCell13 *b = util_calloc(1, sizeof(*b));
    char name[64];
    item->kind = FIT_GATE_BUF;
    item->cell = b;
    b->buf_type = pad ? FIT_BUF_BI : FIT_BUF_TS;
    b->model = util_strdup(f->view->model);
    snprintf(name, sizeof(name), "__edif_buf_%d", tt_count(r->entity->net->items));
    b->inst_name = util_strdup(name);
    b->in = control(r, f, in, 0);
    b->oe = control(r, f, oe, 0);
    b->oe_pol = 1;
    b->pad = state_output(r, f, pad ? pad : out, pad ? ED_INOUT : ED_OUTPUT);
    if (pad)
        b->out = state_output(r, f, out, ED_OUTPUT);
    if (pad && !b->out)
        b->buf_type = FIT_BUF_TS;
    if (!b->pad)
        ed_error(r, f->node, "unconnected tristate pad on '%s'", f->name);
    list_add(r->entity->net->items, item);
}

static int pn(int c) { return c == 'P' || c == 'N'; }
static int polarity(int c) { return c == 'P' ? 1 : -1; }

static int yosys_register(EdRead *r, EdFrame *f, const char *model)
{
    static const struct { const char *name; int kind, format; } types[] = {
        { "$_DFF_", FIT_GATE_DFF, 0 },
        { "$_DFFE_", FIT_GATE_DFF, 1 },
        { "$_DFFSR_", FIT_GATE_DFF, 2 },
        { "$_DFFSRE_", FIT_GATE_DFF, 3 },
        { "$_DLATCH_", FIT_GATE_LATCH, 0 },
        { "$_DLATCHSR_", FIT_GATE_LATCH, 2 },
        { "$_SDFF_", FIT_GATE_DFF, 4 },
        { "$_SDFFE_", FIT_GATE_DFF, 5 },
        { "$_SDFFCE_", FIT_GATE_DFF, 6 }
    };
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        size_t len = strlen(types[i].name), n;
        const char *s;
        int format = types[i].format, enable = 0, reset = 0, separate = 0;
        FitInstance *inst;
        if (strncmp(model, types[i].name, len))
            continue;
        s = model + len;
        n = strlen(s);
        if (!n || s[n - 1] != '_')
            break;
        n--;
        if (format == 0 && n == 3) reset = 1;
        else if (format == 1) { enable = 1; reset = n == 4; }
        else if (format == 2 || format == 3) { separate = 1; enable = format == 3; }
        else if (format >= 4) { reset = 1; enable = format != 4; }
        if (n != (size_t)(1 + 2 * (reset || separate) + enable) || !pn(s[0]) ||
            (reset && (!pn(s[1]) || (s[2] != '0' && s[2] != '1'))) ||
            (separate && (!pn(s[1]) || !pn(s[2]))) || (enable && !pn(s[n - 1])))
            break;
        inst = instance(r, f, types[i].kind);
        inst->clk = control(r, f, types[i].kind == FIT_GATE_LATCH ? "E" : "C", 0);
        inst->clk_kind = types[i].kind == FIT_GATE_LATCH ?
                         (s[0] == 'P' ? FIT_CLK_L1 : FIT_CLK_L0) :
                         (s[0] == 'P' ? FIT_CLK_RECK : FIT_CLK_FECK);
        inst->d = state_output(r, f, "Q", ED_OUTPUT);
        if (!inst->d)
            ed_error(r, f->node, "register '%s' has no connected output", f->name);
        if (format >= 4) {
            FitFctTree *d = input(r, f, "D"), *rst = input(r, f, "R");
            if (s[1] == 'N') rst = fcttree_new_not(rst);
            if (format == 5) {
                FitFctTree *en = input(r, f, "E");
                EdWire *q = pin(r, f, "Q", ED_OUTPUT, 0);
                if (s[n - 1] == 'N') en = fcttree_new_not(en);
                d = mux(en, fcttree_new_id(wire_var(r, q)->id), d);
            }
            inst->q = control_tree(r, mux(rst, d, fcttree_new_const(s[2] == '1')));
        } else {
            inst->q = control(r, f, "D", 0);
            if (reset) {
                char **signal = s[2] == '1' ? &inst->ap : &inst->ar;
                *signal = control(r, f, "R", 0);
                if (s[2] == '1') inst->ap_pol = polarity(s[1]);
                else inst->ar_pol = polarity(s[1]);
            }
            if (separate) {
                inst->ap = control(r, f, "S", 0);
                inst->ar = control(r, f, "R", 0);
                inst->ap_pol = polarity(s[1]);
                inst->ar_pol = polarity(s[2]);
            }
        }
        if (enable && format != 5) {
            inst->ce = control(r, f, "E", 0);
            inst->ce_pol = polarity(s[n - 1]);
        }
        return 1;
    }
    return 0;
}

static int builtin(EdRead *r, EdFrame *f)
{
    const char *m = f->view->model, *a, *out;
    FitFctTree *tree;
    static const struct { const char *name; int op, invert; } gates[] = {
        { "AND", FIT_SEL_AND, 0 }, { "NAND", FIT_SEL_AND, 1 },
        { "OR", FIT_SEL_OR, 0 }, { "NOR", FIT_SEL_OR, 1 },
        { "XOR", FIT_SEL_XOR, 0 }, { "XNOR", FIT_SEL_XOR, 1 }
    };
    if (util_str_ieq(m, "GND") || util_str_ieq(m, "VCC")) {
        out = choice(r, f, "Q", "Y", "G", "P");
        output(r, f, out, fcttree_new_const(util_str_ieq(m, "VCC")));
        return 1;
    }
    if (util_str_ieq(m, "BUF") || util_str_ieq(m, "INBUF") ||
        util_str_ieq(m, "OUTBUF") || util_str_ieq(m, "IBUF") ||
        util_str_ieq(m, "INV") || util_str_ieq(m, "NOT") ||
        strcmp(m, "$_BUF_") == 0 || strcmp(m, "$_NOT_") == 0) {
        a = choice(r, f, "A", "A1", "I", NULL);
        out = choice(r, f, "Q", "Y", "QN", "Z");
        tree = input(r, f, a);
        if (util_str_ieq(m, "INV") || util_str_ieq(m, "NOT") || !strcmp(m, "$_NOT_"))
            tree = fcttree_new_not(tree);
        output(r, f, out, tree);
        return 1;
    }
    for (size_t i = 0; i < sizeof(gates) / sizeof(gates[0]); i++) {
        char generic[32];
        const char *suffix;
        int count, inverted = 0, generic_match;
        snprintf(generic, sizeof(generic), "$_%s_", gates[i].name);
        generic_match = !strcmp(m, generic);
        if (!generic_match && strncasecmp(m, gates[i].name, strlen(gates[i].name)))
            continue;
        suffix = generic_match ? "2" : m + strlen(gates[i].name);
        count = 0;
        while (isdigit((unsigned char)*suffix)) {
            count = count * 10 + *suffix++ - '0';
            if (count > 32) return 0;
        }
        if (!count && gates[i].op == FIT_SEL_XOR) count = 2;
        if (*suffix == 'I' || *suffix == 'i') {
            suffix++;
            while (isdigit((unsigned char)*suffix)) {
                inverted = inverted * 10 + *suffix++ - '0';
                if (inverted > 32) return 0;
            }
        }
        if (*suffix || count < 2 || inverted > count)
            return 0;
        tree = NULL;
        for (int n = 0; n < count; n++) {
            char name[24];
            FitFctTree *term;
            if (find_pin(f, "A0")) snprintf(name, sizeof(name), "A%d", n);
            else snprintf(name, sizeof(name), "%c%s", 'A' + n,
                          n >= count - inverted ? "N" : "");
            term = input(r, f, name);
            if (n >= count - inverted) term = fcttree_new_not(term);
            tree = tree ? binary(gates[i].op, tree, term) : term;
        }
        if (gates[i].invert) tree = fcttree_new_not(tree);
        out = choice(r, f, "Q", "QN", "Y", "Z0");
        output(r, f, out, tree);
        return 1;
    }
    if (!strcmp(m, "$_MUX_") || util_str_ieq(m, "MUX2") ||
        util_str_ieq(m, "MUX4") || util_str_ieq(m, "MUX8")) {
        if (!strcmp(m, "$_MUX_")) {
            tree = mux(input(r, f, "S"), input(r, f, "A"), input(r, f, "B"));
            output(r, f, "Y", tree);
        } else {
            int n = m[3] - '0', level = 0;
            FitFctTree *terms[8];
            for (int j = 0; j < n; j++) {
                char name[8];
                snprintf(name, sizeof(name), "M%d", j);
                terms[j] = input(r, f, name);
            }
            for (; n > 1; n /= 2, level++) {
                char name[8];
                snprintf(name, sizeof(name), "S%d", level);
                for (int j = 0; j < n / 2; j++)
                    terms[j] = mux(input(r, f, name), terms[2*j], terms[2*j+1]);
            }
            output(r, f, "Q", terms[0]);
        }
        return 1;
    }
    if (!strcmp(m, "$_ANDNOT_") || !strcmp(m, "$_ORNOT_")) {
        output(r, f, "Y", binary(!strcmp(m, "$_ANDNOT_") ? FIT_SEL_AND : FIT_SEL_OR,
                                input(r, f, "A"), fcttree_new_not(input(r, f, "B"))));
        return 1;
    }
    if (!strcmp(m, "$_NMUX_")) {
        output(r, f, "Y", fcttree_new_not(mux(input(r, f, "S"), input(r, f, "A"), input(r, f, "B"))));
        return 1;
    }
    if (!strcmp(m, "$_AOI3_") || !strcmp(m, "$_OAI3_") ||
        !strcmp(m, "$_AOI4_") || !strcmp(m, "$_OAI4_")) {
        int op = m[2] == 'A' ? FIT_SEL_AND : FIT_SEL_OR;
        int outer = op == FIT_SEL_AND ? FIT_SEL_OR : FIT_SEL_AND;
        FitFctTree *left = binary(op, input(r, f, "A"), input(r, f, "B"));
        FitFctTree *right = input(r, f, "C");
        if (m[5] == '4')
            right = binary(op, right, input(r, f, "D"));
        output(r, f, "Y", fcttree_new_not(binary(outer, left, right)));
        return 1;
    }
    if (!strcmp(m, "$_TBUF_")) {
        buffer(r, f, "A", "E", "Y", NULL);
        return 1;
    }
    if (util_str_ieq(m, "TRI") || util_str_ieq(m, "BIBUF")) {
        buffer(r, f, "A", "EN", "Q", util_str_ieq(m, "BIBUF") ? "PAD" : NULL);
        return 1;
    }
    if (yosys_register(r, f, m))
        return 1;
    if (util_str_ieq(m, "DFF") || util_str_ieq(m, "DFFE") ||
        util_str_ieq(m, "DFFAR") || util_str_ieq(m, "DFFAS") ||
        util_str_ieq(m, "DFFARS") || util_str_ieq(m, "DFFEARS") ||
        util_str_ieq(m, "LATCH")) {
        int latch = util_str_ieq(m, "LATCH");
        FitInstance *inst = instance(r, f, latch ? FIT_GATE_LATCH : FIT_GATE_DFF);
        inst->clk = control(r, f, latch ? "EN" : "CLK", 0);
        inst->clk_kind = latch ? FIT_CLK_L1 : FIT_CLK_RECK;
        inst->q = control(r, f, "D", 0);
        if (latch || util_str_ieq(m, "DFFAR") || util_str_ieq(m, "DFFARS") || util_str_ieq(m, "DFFEARS"))
            inst->ar = control(r, f, "AR", 1);
        if (latch || util_str_ieq(m, "DFFAS") || util_str_ieq(m, "DFFARS") || util_str_ieq(m, "DFFEARS"))
            inst->ap = control(r, f, "AS", 1);
        if (util_str_ieq(m, "DFFE") || util_str_ieq(m, "DFFEARS"))
            inst->ce = control(r, f, "CE", 1);
        inst->d = register_outputs(r, f, inst, latch ? FIT_GATE_LATCH : FIT_GATE_DFF, "Q", "QN");
        if (!inst->d)
            ed_error(r, f->node, "register '%s' has no connected output", f->name);
        return 1;
    }
    return 0;
}

static FitFctTree *library_tree(EdRead *r, EdFrame *f, FitLibExpr *expr)
{
    FitFctTree *tree = NULL;
    if (!expr) ed_error(r, f->node, "missing library equation");
    switch (expr->op) {
    case FIT_LIBEXPR_CONST0: return fcttree_new_const(0);
    case FIT_LIBEXPR_CONST1: return fcttree_new_const(1);
    case FIT_LIBEXPR_VAR: return input(r, f, expr->text);
    case FIT_LIBEXPR_NOT:
        return fcttree_new_not(library_tree(r, f, expr->children->items[0]));
    case FIT_LIBEXPR_AND: case FIT_LIBEXPR_OR:
        for (int i = 0; i < tt_count(expr->children); i++) {
            FitFctTree *child = library_tree(r, f, expr->children->items[i]);
            tree = tree ? binary(expr->op == FIT_LIBEXPR_AND ? FIT_SEL_AND : FIT_SEL_OR,
                                 tree, child) : child;
        }
        return tree;
    default: ed_error(r, f->node, "unsupported library equation");
    }
    return NULL;
}

static int library_cell(EdRead *r, EdFrame *f)
{
    for (int i = 0; i < tt_count(r->library); i++) {
        FitLibGate *g = r->library->items[i];
        const char *name = g->kind == FIT_GATE_COMB ? ((FitLibComb *)g->cell)->name :
                           g->kind == FIT_GATE_BUF ? ((FitLibBuf *)g->cell)->name :
                           g->kind == FIT_GATE_UNKNOWN ? ((FitLibUGate *)g->cell)->name :
                           ((FitLibSeq *)g->cell)->name;
        if (!util_str_ieq(name, f->view->model))
            continue;
        if (g->kind == FIT_GATE_COMB) {
            FitLibComb *c = g->cell;
            output(r, f, c->out_name, library_tree(r, f, c->expr));
        } else if (g->kind == FIT_GATE_BUF) {
            FitLibBuf *b = g->cell;
            if (b->type == FIT_BUF_I || b->type == FIT_BUF_CLK)
                output(r, f, b->out, input(r, f, b->pad));
            else if (b->type == FIT_BUF_O)
                output(r, f, b->pad, input(r, f, b->in));
            else {
                buffer(r, f, b->in, b->oe, b->type == FIT_BUF_BI ? b->out : b->pad,
                       b->type == FIT_BUF_BI ? b->pad : NULL);
                ((FitCell13 *)((FitNetItem *)r->entity->net->items->items[
                    r->entity->net->items->count - 1])->cell)->oe_pol = b->oe_pol;
            }
        } else if (g->kind == FIT_GATE_UNKNOWN) {
            ed_error(r, f->node, "library cell '%s' has no logic definition", name);
        } else {
            FitLibSeq *s = g->cell;
            FitInstance *inst = instance(r, f, g->kind);
            inst->clk = control(r, f, s->clk, 0);
            inst->clk_kind = s->clk_kind;
            inst->q = control(r, f, s->d, 0);
            inst->ar = control(r, f, s->ar, 1); inst->ar_pol = s->ar_pol;
            inst->ap = control(r, f, s->ap, 1); inst->ap_pol = s->ap_pol;
            inst->ce = control(r, f, s->ce, 1); inst->ce_pol = s->ce_pol;
            inst->oe = control(r, f, s->oe, 1); inst->oe_pol = s->oe_pol;
            inst->q2 = control(r, f, s->j, 0); inst->q2_pol = s->j_pol;
            inst->q3 = control(r, f, s->k, 0); inst->q3_pol = s->k_pol;
            inst->mux_a = control(r, f, s->mux_a, 0);
            inst->mux_b = control(r, f, s->mux_b, 0);
            inst->mux_sel = control(r, f, s->mux_sel, 0);
            inst->pin = state_output(r, f, s->z, ED_OUTPUT);
            inst->d = register_outputs(r, f, inst, g->kind, s->q, s->qn);
            if (!inst->d && !inst->d2 && !inst->pin)
                ed_error(r, f->node, "cell '%s' has no connected output", f->name);
        }
        return 1;
    }
    return 0;
}

static void assign_pin(EdRead *r, const char *name, int pin_number)
{
    EdTop *found = NULL;
    for (EdTop *p = r->ports; p; p = p->next) {
        if (!strcmp(p->name, name) || util_str_ieq(p->mapped, name)) {
            if (found && found != p)
                ed_error(r, NULL, "ambiguous pin name '%s'", name);
            found = p;
        }
    }
    if (!found)
        ed_error(r, NULL, "pin assignment names unknown port '%s'", name);
    if (pin_number <= 0 || pin_number > 256)
        ed_error(r, found->node, "invalid pin number %d for '%s'", pin_number, name);
    if (found->pin && found->pin != pin_number)
        ed_error(r, found->node, "conflicting assignments for '%s'", name);
    for (EdTop *p = r->ports; p; p = p->next)
        if (p != found && p->pin == pin_number)
            ed_error(r, found->node, "pin %d assigned to both '%s' and '%s'", pin_number, p->name, name);
    found->pin = pin_number;
}

static void constraints(EdRead *r, FitList *params)
{
    char *option = param_get(params, "pinfile"), *input_name = param_get(params, "i");
    const char *base = g_opt.out_base ? g_opt.out_base : input_name;
    char *path = ed_alloc(r, strlen(base) + (option ? strlen(option) : 0) + 8);
    FILE *file;
    int enabled = option != NULL;
    int required = enabled && *option && !util_str_ieq(option, "ON");
    if (required) strcpy(path, option);
    else {
        char *dot, *slash;
        strcpy(path, base);
        dot = strrchr(path, '.');
        slash = strrchr(path, '/');
        if (!g_opt.out_base && dot && (!slash || dot > slash)) *dot = '\0';
        strcat(path, ".pin");
    }
    free(input_name);
    free(option);
    for (EdTop *p = r->ports; p; p = p->next) {
        for (EdNode *n = ed_arg(p->node, 2); n; n = n->next) {
            EdNode *value;
            const char *name;
            char *end;
            long number;
            if (!ed_is(n, "property")) continue;
            name = ed_name(r, ed_arg(n, 1), 1);
            if (!util_str_ieq(name, "LOC") && !util_str_ieq(name, "PIN") &&
                !util_str_ieq(name, "PIN_NUMBER")) continue;
            value = ed_arg(ed_arg(n, 2), 1);
            if (!value || !value->text)
                ed_error(r, n, "pin location must be a scalar integer or string");
            name = value->text;
            if (*name == 'P' || *name == 'p') name++;
            errno = 0;
            number = strtol(name, &end, 10);
            if (errno || !*name || *end || number < 1 || number > 256)
                ed_error(r, n, "invalid pin location '%s'", value->text);
            assign_pin(r, p->name, (int)number);
        }
    }
    file = enabled ? fopen(path, "r") : NULL;
    if (!file) {
        if (enabled)
            ed_error(r, NULL, "cannot open pin file '%s'", path);
    } else {
        char line[4096];
        r->pin_file = file;
        while (fgets(line, sizeof(line), file)) {
            char *s = line, *colon, *end, *name;
            long number;
            while (isspace((unsigned char)*s)) s++;
            if (!*s || *s == '#' || !strncmp(s, "//", 2) || !strncasecmp(s, "CHIP ", 5)) continue;
            name = s;
            if (*name == '"') {
                name++;
                end = strchr(name, '"');
                if (!end)
                    ed_error(r, NULL, "unterminated pin name in '%s'", path);
                *end++ = '\0';
                while (isspace((unsigned char)*end)) end++;
                colon = *end == ':' ? end : NULL;
            } else {
                colon = strchr(s, ':');
                if (colon) {
                    *colon = '\0';
                    end = colon;
                    while (end > s && isspace((unsigned char)end[-1])) *--end = '\0';
                }
            }
            if (!colon)
                ed_error(r, NULL, "expected 'signal : pin' in '%s'", path);
            errno = 0;
            number = strtol(colon + 1, &end, 10);
            while (isspace((unsigned char)*end)) end++;
            if (errno || (*end && *end != '#' && strncmp(end, "//", 2)) || number < 1 || number > 256) {
                ed_error(r, NULL, "invalid pin assignment in '%s'", path);
            }
            assign_pin(r, name, (int)number);
        }
        if (ferror(file)) ed_error(r, NULL, "cannot read pin file '%s'", path);
        fclose(file);
        r->pin_file = NULL;
        g_opt.pinfile = 1;
        g_opt.pin_filename = util_strdup(path);
    }
    for (EdTop *p = r->ports; p; p = p->next) {
        if (p->pin) {
            FitPreassignItem *item = util_calloc(1, sizeof(*item) + sizeof(int));
            item->name = util_strdup(p->mapped);
            item->pin = p->pin;
            list_add(g_preassign, item);
        }
    }
}

void ed_lower(EdRead *r, FitList *params)
{
    char *library = param_get(params, "lib"), *cycle = NULL;
    r->entity = entity_new(NULL);
    r->entity->inputs = list_create();
    r->entity->outputs = list_create();
    r->entity->locals = list_create();
    r->entity->fcts = list_create();
    r->entity->net = netlist_new();
    r->entity->net->items = list_create();
    if (library) {
        int ok = lib_parse_file(library, &r->library, &r->lib_header);
        free(library);
        if (!ok) ed_error(r, NULL, "cannot read primitive library");
    }
    for (EdTop *p = r->ports; p; p = p->next) {
        size_t i = 0;
        /* Keep ordinary port and bus names in reports. Dots are reserved
         * by later register naming, and whitespace cannot occur in TT3. */
        for (const unsigned char *s = (const unsigned char *)p->name; *s; s++) {
            int ordinary = isalnum(*s) || *s == '_' || *s == '[' || *s == ']';
            if (i + (ordinary ? 1 : 4) >= sizeof(p->mapped))
                ed_error(r, p->node, "port name is too long");
            if (ordinary) p->mapped[i++] = (char)*s;
            else { snprintf(p->mapped + i, sizeof(p->mapped) - i, "_%02x_", *s); i += 4; }
        }
        p->mapped[i] = '\0';
        if (!i || !strncasecmp(p->mapped, "__edif_", 7))
            ed_error(r, p->node, "empty or reserved port name '%s'", p->name);
        for (EdTop *old = r->ports; old != p; old = old->next)
            if (util_str_ieq(old->mapped, p->mapped))
                ed_error(r, p->node, "port names '%s' and '%s' collide in the fitter", old->name, p->name);
        if (p->direction == ED_INPUT) {
            drive(r, NULL, p->wire);
            equation(r, wire_var(r, p->wire), fcttree_new_id(variable(r, p->mapped, ED_INPUT)->id));
        } else if (p->direction == ED_INOUT) {
            p->wire->pad = 1;
        }
    }
    constraints(r, params);
    for (EdFrame *f = r->leaves; f; f = f->leaf_next) {
        if (!library_cell(r, f) && !builtin(r, f))
            ed_error(r, f->node, "unknown primitive '%s' at '%s'; supply its definition with -lib",
                     f->view->model, f->name);
        for (EdPort *p = f->view->ports; p; p = p->next)
            for (int i = 0; i < p->width; i++)
                if (!f->used[p->offset + i])
                    ed_error(r, p->node, "unsupported pin '%s' on primitive '%s'", p->name, f->view->model);
    }
    for (EdTop *p = r->ports; p; p = p->next) {
        if (p->direction != ED_INPUT) {
            if (p->direction == ED_INOUT && !p->wire->drivers) {
                drive(r, NULL, p->wire);
                equation(r, wire_var(r, p->wire),
                         fcttree_new_id(variable(r, p->mapped, ED_INPUT)->id));
                continue;
            }
            FitVariable *v = variable(r, p->mapped, ED_OUTPUT);
            p->wire->read = 1;
            equation(r, v, fcttree_new_id(wire_var(r, p->wire)->id));
            if (p->wire->sequential)
                lib_alias_set_target(r->entity->net, p->wire->var->name, v->name);
        }
    }
    for (int i = 0; i < r->wires->count; i++) {
        EdWire *w = r->wires->items[i];
        if (w->parent == w && w->read && !w->drivers)
            ed_error(r, NULL, "undriven net %d", w->number);
    }
    r->entity->n_in = tt_count(r->entity->inputs);
    r->entity->n_out = tt_count(r->entity->outputs);
    r->entity->n_local = tt_count(r->entity->locals);
    if (!tt2_check_cycles(r->entity, &cycle)) {
        char *copy = ed_string(r, cycle ? cycle : "unknown cycle");
        free(cycle);
        ed_error(r, NULL, "combinational cycle: %s", copy);
    }
}
