/*
 * tt_tt2.c - tt2_read, first part: conversions between the functions of
 * an entity and a PLA (list of FitPTermIO), and the entity helpers the rest
 * of the fitter uses (statistics, cycle check, renaming of netlist
 * signals).
 */
#include "tt_priv.h"

/* largest depth of the functions of the entity; *depth is only
 * raised. */
static void tt2_entity_depth(FitEntity *entity, int *depth)
{
    FitList *depths = list_create_sized(tt2_next_var_id(entity));
    int n, i;

    depths->count = depths->capacity;
    for (i = 0; i < depths->count; i++)
        depths->items[i] = FIT_INT2PTR(-1);
    n = tt_count(entity->fcts);
    for (i = 0; i < n; i++) {
        FitFct *fct = entity->fcts->items[i];
        int id = fct->var_id;
        int known;

        if (id < 0 || id >= depths->count)
            tt_unreachable("tt2_entity_depth: function id outside the variable ids");
        known = FIT_PTR2INT(depths->items[id]);
        if (known == -1) {
            int d = 1;

            fcttree_depth_rec(entity, fct->tree[0], depths, &d, 1);
            depths->items[id] = FIT_INT2PTR(d);
            if (*depth < d)
                *depth = d;
        } else if (*depth < known) {
            *depth = known;
        }
    }
    list_destroy(&depths);
}

/* FitListEq3 comparing the input part of a FitPTermIO with a
 * FitPTerm */
static int tt2_pterm_io_equal(void *item, void *key, void *nwords)
{
    return pterm_equal(item, key, nwords);
}

FitList *tt2_pla_from_fct(FitEntity *entity, int which)
{
    FitList *pla = list_create();
    int n_nodes = entity->n_local + entity->n_out;
    int nw_all = tt_words(entity->n_in + n_nodes);
    int nw_out = tt_words(n_nodes);
    char lit = which == 2 ? '-' : '1';
    int k, i;

    for (k = 0; k < tt_count(entity->fcts); k++) {
        FitFct *fct = entity->fcts->items[k];
        FitFctTree *tree;
        FitList *pterms;
        int pos;

        pos = var_list_pos_of_id(entity->outputs, fct->var_id);
        if (pos == 0) {
            pos = var_list_pos_of_id(entity->locals, fct->var_id);
            if (pos == 0) {
                fprintf(g_msg, "In PLAFromFctNoSubstitute (SingleFct Id is neither OUT nor LOCAL).\n");
                return NULL;
            }
            pos += entity->n_out;
        }
        if (which == 1)
            tree = fct->tree[1];
        else if (which == 2)
            tree = fct->tree[2];
        else
            tree = fct->tree[0];
        pterms = fcttree_to_pterms3(tree, entity->outputs, entity->inputs, entity->locals);
        for (i = 0; i < tt_count(pterms); i++) {
            int found = list_find_x(pla, pterms->items[i], tt2_pterm_io_equal,
                                    FIT_INT2PTR(nw_all));
            FitPTermIO *io;

            if (found == 0) {
                FitPTerm *copy;

                io = util_calloc(1, sizeof *io);
                copy = pterm_copy(pterms->items[i], nw_all);
                io->in = *copy;
                free(copy);
                pterm_init_zero(&io->out, nw_out);
                pterm_set_literal(&io->out, pos, lit);
                list_add(pla, io);
            } else {
                io = pla->items[found - 1];
                pterm_set_literal(&io->out, pos, lit);
            }
        }
        list_free_all(&pterms, tt_cb_free_pterm);
    }
    return pla;
}

/* indices (0-based) of the first n output bits of a product
 * term that are '1' (ones != 0: bits and care set) or '-' (care set, bits
 * clear). */
static FitList *tt2_pterm_out_indices(const FitPTerm *out, unsigned n, int ones)
{
    FitList *indices = list_create();
    unsigned b;

    for (b = 0; b < n; b++) {
        unsigned mask = 1u << (b & 31);
        unsigned bits = out->bits[b >> 5] & mask;
        unsigned care = out->care[b >> 5] & mask;

        if (ones ? (bits != 0 && care != 0) : (care != 0 && bits == 0))
            list_add(indices, FIT_INT2PTR((int)b));
    }
    return indices;
}

static FitFct *new_sum_fct(int id)
{
    FitFct *fct = fct_new();

    fct->var_id = id;
    fct->tree[0] = fcttree_new();
    fct->tree[0]->sel = FIT_SEL_OR;
    fct->tree[0]->u.children = list_create();
    fct->tree[2] = fcttree_new();
    fct->tree[2]->sel = FIT_SEL_OR;
    fct->tree[2]->u.children = list_create();
    return fct;
}

void tt2_fct_from_pla(FitList *pla, FitEntity *entity)
{
    int n_local = entity->n_local;
    int n_out = entity->n_out;
    unsigned n = (unsigned)(n_local + n_out);
    FitList *vars;
    int i, k;

    vars = list_map_copy(entity->outputs, tt_cb_copy_var);
    list_append_mapped(vars, entity->inputs, tt_cb_copy_var);
    list_append_mapped(vars, entity->locals, tt_cb_copy_var);

    entity->fcts = list_create_sized((int)n);
    for (i = 0; i < n_out; i++) {
        FitVariable *v = entity->outputs->items[i];
        list_add(entity->fcts, new_sum_fct(v->id));
    }
    for (i = 0; i < n_local; i++) {
        FitVariable *v = entity->locals->items[i];
        list_add(entity->fcts, new_sum_fct(v->id));
    }

    for (k = 0; k < tt_count(pla); k++) {
        FitPTermIO *io = pla->items[k];
        FitFctTree *term = fcttree_from_pterm(&io->in, vars);
        FitList *ones = tt2_pterm_out_indices(&io->out, n, 1);
        FitList *dcs = tt2_pterm_out_indices(&io->out, n, 0);

        for (i = 0; i < tt_count(ones); i++) {
            FitFct *fct = entity->fcts->items[FIT_PTR2INT(ones->items[i])];
            list_add(fct->tree[0]->u.children, fcttree_copy(term, NULL));
        }
        for (i = 0; i < tt_count(dcs); i++) {
            FitFct *fct = entity->fcts->items[FIT_PTR2INT(dcs->items[i])];
            list_add(fct->tree[2]->u.children, fcttree_copy(term, NULL));
        }
        list_destroy(&ones);
        list_destroy(&dcs);
        fcttree_free(&term);
    }

    /* an empty sum is the constant 0 */
    for (i = 0; i < tt_count(entity->fcts); i++) {
        FitFct *fct = entity->fcts->items[i];
        int t;

        for (t = 0; t <= 2; t += 2) {
            FitFctTree *tree = fct->tree[t];
            if (tree->u.children == NULL || tree->u.children->count == 0) {
                tree->sel = FIT_SEL_CONST;
                list_destroy(&tree->u.children);
                tree->u.value = 0;
            }
        }
    }
    list_free_all(&vars, tt_cb_free_var);
}

static int max_id_of(FitList *vars)
{
    FitVariable *v;

    if (vars == NULL || vars->count == 0)
        return 0;
    v = list_extreme(vars, 0, var_cmp_id);
    return v->id;
}

int tt2_next_var_id(FitEntity *entity)
{
    int in = max_id_of(entity->inputs);
    int local = max_id_of(entity->locals);
    int out = max_id_of(entity->outputs);

    if (local <= out && in <= out)
        return out + 1;
    if (in < local || in < out)
        in = local;
    return in + 1;
}

char *tt2_ids_to_string(FitEntity *entity, FitList *ids)
{
    char *s = NULL;
    int i;

    for (i = 0; i < tt_count(ids); i++) {
        char *name = entity_var_name(entity, FIT_PTR2INT(ids->items[i]));

        util_str_append(&s, name);
        free(name);
        if (i != tt_count(ids) - 1)
            util_str_append(&s, " -> ");
    }
    return s;
}

int tt2_check_cycles(FitEntity *entity, char **cycle)
{
    FitList *path, *seen;
    char buf[0x100];
    int i;

    if (entity == NULL)
        return 1;
    path = list_create();
    seen = list_create();
    for (i = 0; i < tt_count(entity->fcts); i++)
        ((FitFct *)entity->fcts->items[i])->reach = NULL;

    for (i = 0; i < tt_count(entity->fcts); i++) {
        FitFct *fct = entity->fcts->items[i];
        FitFctTree *tree = fct->tree[0];

        if (tree == NULL)
            tree = fct->tree[1];
        if (tree == NULL) {
            char *name;

            strcpy(buf, "ERROR : Incorrect Fct structure (variable ");
            name = entity_var_name(entity, fct->var_id);
            util_str_cat_n(buf, name, 0x100);
            util_str_cat_n(buf, ") has no  associated equation", 0x100);
            fprintf(g_msg, "%s.\n", buf);
            strcpy(buf, "  Cycle detection aborted - Results may be incorrect");
            fprintf(g_msg, "%s.\n", buf);
            list_destroy(&path);
            list_destroy(&seen);
            return 0;
        }
        list_add(path, FIT_INT2PTR(fct->var_id));
        if (fct->reach == NULL) {
            if (fcttree_find_cycle(entity, tree, path, seen, cycle) != 0) {
                list_destroy(&path);
                list_destroy(&seen);
                return 0;
            }
            fct->reach = list_map_copy(seen, list_identity);
        }
        seen->count = 0;
        path->count = 0;
    }
    list_destroy(&path);
    list_destroy(&seen);
    for (i = 0; i < tt_count(entity->fcts); i++)
        list_destroy(&((FitFct *)entity->fcts->items[i])->reach);
    return 1;
}

int tt2_tree_is_const(FitFctTree *tree)
{
    if (tree->sel == FIT_SEL_CONST)
        return 1;
    if (tree->sel == FIT_SEL_NOT
        && ((FitFctTree *)tree->u.children->items[0])->sel == FIT_SEL_CONST)
        return 1;
    return 0;
}

int tt2_count_id_uses(FitEntity *entity, int id)
{
    int total = 0;
    int i;

    for (i = 0; i < tt_count(entity->fcts); i++) {
        FitFct *fct = entity->fcts->items[i];
        total += (int)fcttree_count_id(fct->tree[0], id);
    }
    return total;
}

int tt2_entity_complexity(FitEntity *entity)
{
    int total = 0;
    int i;

    for (i = 0; i < tt_count(entity->fcts); i++) {
        FitFct *fct = entity->fcts->items[i];
        total += fcttree_count_leaves(fct->tree[0]);
    }
    return total;
}

void tt2_substitute_fct(FitEntity *entity, FitFct *fct)
{
    int n = tt_count(entity->fcts);     /* fixed before the first call */
    int i;

    for (i = 0; i < n; i++) {
        FitFct *f = entity->fcts->items[i];
        fcttree_substitute(&f->tree[0], fct);
    }
}

int tt2_collapse_locals(FitEntity *entity, int limit)
{
    int changed = 0;
    int i = 0;

    while (i < tt_count(entity->fcts)) {
        FitFct *fct = entity->fcts->items[i];

        if (entity_is_local(entity, fct->var_id)) {
            int uses = tt2_count_id_uses(entity, fct->var_id);
            int leaves = fcttree_count_leaves(fct->tree[0]);

            if ((leaves - 1) * uses - leaves < limit) {
                changed = 1;
                tt2_substitute_fct(entity, fct);
                entity_remove_fct(entity, fct);
                continue;       /* the same position holds the next function */
            }
        } else if (entity_is_output(entity, fct->var_id)) {
            FitFctTree *tree = entity_fct_tree(entity, fct->var_id);

            if (tree->sel == FIT_SEL_CONST) {
                tt2_substitute_fct(entity, fct);
                changed = 1;
            }
        }
        i++;
    }
    if (changed)
        entity->i04 = 0;
    return 1;
}

void tt2_compact_entity(FitEntity *entity)
{
    int n, i;

    if (entity->fcts == NULL)
        return;
    n = entity->fcts->count;
    for (i = 0; i < n; i++) {
        FitFct *fct = entity->fcts->items[i];
        fcttree_compact(&fct->tree[0]);
    }
}

/* message for the status of tt2_convert_pla_type */
void tt2_report_set_conflict(FILE *out, int status)
{
    switch (status) {
    case 1:
        fprintf(out, "Warning: ON-SET and DC-SET intersect\n");
        break;
    case 2:
        fprintf(out, "Warning: OFF-SET and DC-SET intersect\n");
        break;
    case 100:
        fprintf(out, "Error: ON-SET and OFF-SET intersect\n");
        break;
    case 0x65:
        fprintf(out, "Error: OFF-SET and DC-SET intersect\n");
        break;
    case 0x66:
        fprintf(out, "Error: ON-SET and DC-SET intersect\n");
        break;
    default:
        break;
    }
}

/* bring a PLA as read (output characters 1 0 - ~ with the
 * meaning the .type gives them) into the form the fitter uses: '1' =
 * ON-set, '-' = DC-set.  type[o-1] is '0'..'5' for the kinds f, r, fd, fr,
 * dr, fdr of output o.  Returns the new list of FitPTermIO, or NULL with
 * *status = 100, 0x65 or 0x66 when the sets contradict each other;
 * *status 1 or 2 is a warning. */
FitList *tt2_convert_pla_type(FitList *pla, int n_total, int n_out, const char *type,
                              int *status)
{
    int nw = tt_words(n_total);
    int nw_out = tt_words(n_out);
    FitList *result = list_create();
    FitList *on, *off, *dc, *cover;
    int o;

    *status = 0;
    for (o = 1; o <= n_out; o++) {
        switch (type[o - 1]) {
        case '0':
            on = ptermio_select_output(pla, o, '1');
            ptermio_merge_output(on, result, o, nw, nw_out, '1');
            list_destroy(&on);
            break;
        case '1':
            off = ptermio_select_output(pla, o, '0');
            cover = domlist_merge_covers(off, NULL, n_total);
            ptermio_merge_output(cover, result, o, nw, nw_out, '1');
            list_destroy(&off);
            list_free_all(&cover, tt_cb_free_pterm);
            break;
        case '2':
            on = ptermio_select_output(pla, o, '1');
            dc = ptermio_select_output(pla, o, '-');
            if (pterm_lists_disjoint(on, dc, nw) == 0)
                *status = 1;
            ptermio_merge_output(on, result, o, nw, nw_out, '1');
            ptermio_merge_output(dc, result, o, nw, nw_out, '-');
            list_destroy(&on);
            list_destroy(&dc);
            break;
        case '3':
            on = ptermio_select_output(pla, o, '1');
            off = ptermio_select_output(pla, o, '0');
            if (pterm_lists_disjoint(on, off, nw) == 0) {
                *status = 100;
                list_destroy(&on);
                list_destroy(&off);
                list_free_all(&result, tt_cb_free_ptermio);
                return NULL;
            }
            cover = domlist_merge_covers(off, NULL, n_total);
            if (on != NULL && on->count != 0)
                ptermio_merge_output(on, result, o, nw, nw_out, '1');
            else if (off != NULL && off->count != 0)
                ptermio_merge_output(cover, result, o, nw, nw_out, '1');
            list_destroy(&on);
            list_destroy(&off);
            list_free_all(&cover, tt_cb_free_pterm);
            break;
        case '4':
            off = ptermio_select_output(pla, o, '0');
            dc = ptermio_select_output(pla, o, '-');
            if (pterm_lists_disjoint(off, dc, nw) == 0)
                *status = 2;
            cover = domlist_merge_covers(off, dc, n_total);
            ptermio_merge_output(dc, result, o, nw, nw_out, '-');
            ptermio_merge_output(cover, result, o, nw, nw_out, '1');
            list_destroy(&off);
            list_destroy(&dc);
            list_free_all(&cover, tt_cb_free_pterm);
            break;
        case '5':
            on = ptermio_select_output(pla, o, '1');
            off = ptermio_select_output(pla, o, '0');
            dc = ptermio_select_output(pla, o, '-');
            if (pterm_lists_disjoint(off, dc, nw) == 0)
                *status = 0x65;
            else if (pterm_lists_disjoint(off, on, nw) == 0)
                *status = 100;
            else if (pterm_lists_disjoint(dc, on, nw) == 0)
                *status = 0x66;
            if (*status != 0) {
                list_destroy(&on);
                list_destroy(&off);
                list_destroy(&dc);
                list_free_all(&result, tt_cb_free_ptermio);
                return NULL;
            }
            ptermio_merge_output(on, result, o, nw, nw_out, '1');
            ptermio_merge_output(off, result, o, nw, nw_out, '0');
            ptermio_merge_output(dc, result, o, nw, nw_out, '-');
            list_destroy(&off);
            list_destroy(&on);
            list_destroy(&dc);
            break;
        default:
            break;
        }
    }
    return result;
}

int tt2_print_entity_stats(FILE *unused, FitEntity *entity)
{
    int complexity = tt2_entity_complexity(entity);
    int depth = 0;
    int ios = 0;

    (void)unused;
    tt2_entity_depth(entity, &depth);
    if (entity->net != NULL && entity->net->aliases != NULL)
        ios = -entity->net->aliases->count;
    ios += tt_count(entity->inputs) + tt_count(entity->outputs);
    if (g_verbose != 0)
        fprintf(g_msg, "\tIOs        = %-4d\tINodes     = %-4d \tDepth      = %-3d\tComplexity = %-6d\n",
                ios, tt_count(entity->locals), depth, complexity);
    return 1;
}

/* replaces *s by a copy of new_name if it equals old_name (ignoring case) */
static void tt2_rename_string(char **s, const char *old_name, const char *new_name)
{
    if (*s != NULL && util_str_ieq(*s, old_name)) {
        free(*s);
        *s = util_strdup(new_name);
    }
}

void tt2_rename_net_signal(FitEntity *entity, const char *old_name, const char *new_name,
                           int aliases_too)
{
    int i, j;

    if (entity == NULL || entity->net == NULL || entity->net->items == NULL)
        return;
    for (i = 0; i < tt_count(entity->net->items); i++) {
        FitNetItem *item = entity->net->items->items[i];

        if (item->kind == FIT_GATE_UNKNOWN) {
            FitUnknownGate *gate = item->cell;

            for (j = 0; j < tt_count(gate->pins); j++) {
                FitUnknownPin *pin = gate->pins->items[j];
                tt2_rename_string(&pin->signal, old_name, new_name);
            }
        } else if (item->kind == FIT_GATE_CELL13) {
            FitCell13 *cell = item->cell;

            tt2_rename_string(&cell->pad, old_name, new_name);
            tt2_rename_string(&cell->in, old_name, new_name);
            tt2_rename_string(&cell->out, old_name, new_name);
            tt2_rename_string(&cell->oe, old_name, new_name);
        } else {
            FitInstance *inst = item->cell;

            tt2_rename_string(&inst->clk, old_name, new_name);
            tt2_rename_string(&inst->ar, old_name, new_name);
            tt2_rename_string(&inst->ap, old_name, new_name);
            tt2_rename_string(&inst->ce, old_name, new_name);
            tt2_rename_string(&inst->oe, old_name, new_name);
            tt2_rename_string(&inst->q, old_name, new_name);
            tt2_rename_string(&inst->q2, old_name, new_name);
            tt2_rename_string(&inst->q3, old_name, new_name);
            tt2_rename_string(&inst->mux_a, old_name, new_name);
            tt2_rename_string(&inst->mux_b, old_name, new_name);
            tt2_rename_string(&inst->mux_sel, old_name, new_name);
            tt2_rename_string(&inst->d, old_name, new_name);
            tt2_rename_string(&inst->d2, old_name, new_name);
            tt2_rename_string(&inst->pin, old_name, new_name);
        }
    }
    if (aliases_too != 0) {
        for (i = 0; i < tt_count(entity->net->aliases); i++) {
            FitAlias *alias = entity->net->aliases->items[i];

            tt2_rename_string(&alias->name, old_name, new_name);
            tt2_rename_string(&alias->target, old_name, new_name);
        }
    }
}

void tt2_add_buffer_fct(FitEntity *entity, FitVariable *out, FitVariable *in)
{
    FitFct *fct;

    if (entity->fcts == NULL)
        entity->fcts = list_create();
    fct = fct_new();
    fct->var_id = out->id;
    fct->tree[0] = fcttree_new_id(in->id);
    list_add(entity->fcts, fct);
}

int tt2_net_uses_signal(FitEntity *entity, const char *name)
{
    int i, j;

    if (entity == NULL || entity->net == NULL || entity->net->items == NULL)
        return 0;
    for (i = 0; i < tt_count(entity->net->items); i++) {
        FitNetItem *item = entity->net->items->items[i];

        switch (item->kind) {
        case 2: case 3: case 4: case 5: case 6: case 7: case 8: case 9:
        case 0xb: case 0xc: {
            FitInstance *inst = item->cell;

            if (util_str_ieq(inst->clk, name) || util_str_ieq(inst->ar, name)
                || util_str_ieq(inst->ap, name) || util_str_ieq(inst->ce, name)
                || util_str_ieq(inst->oe, name) || util_str_ieq(inst->q, name)
                || util_str_ieq(inst->q2, name) || util_str_ieq(inst->q3, name)
                || util_str_ieq(inst->mux_a, name) || util_str_ieq(inst->mux_b, name)
                || util_str_ieq(inst->mux_sel, name) || util_str_ieq(inst->d, name)
                || util_str_ieq(inst->d2, name) || util_str_ieq(inst->pin, name))
                return 1;
            break;
        }
        case FIT_GATE_UNKNOWN: {
            FitUnknownGate *gate = item->cell;

            for (j = 0; j < tt_count(gate->pins); j++) {
                FitUnknownPin *pin = gate->pins->items[j];
                if (util_str_ieq(pin->signal, name))
                    return 1;
            }
            break;
        }
        case FIT_GATE_CELL13: {
            FitCell13 *cell = item->cell;

            if (util_str_ieq(cell->pad, name) || util_str_ieq(cell->in, name)
                || util_str_ieq(cell->out, name) || util_str_ieq(cell->oe, name))
                return 1;
            break;
        }
        default:
            break;
        }
    }
    return 0;
}

/* the names of a cycle description "a -> b -> c" as ids of
 * local variables */
FitList *tt2_cycle_to_ids(FitEntity *entity, const char *cycle)
{
    FitList *ids = list_create();
    char name[0x100];
    size_t len = strlen(cycle);
    size_t i;
    int n = 0;
    int id;

    for (i = 0; i < len; i++) {
        if (cycle[i] == ' ') {
            name[n] = '\0';
            id = var_list_id_of_name(entity->locals, name);
            if (id == 0) {
                list_destroy(&ids);
                return NULL;
            }
            list_add(ids, FIT_INT2PTR(id));
            i += 3;         /* skip "-> " */
            n = 0;
        } else if (n < (int)sizeof name - 1) {
            name[n++] = cycle[i];
        }
    }
    name[n] = '\0';
    id = var_list_id_of_name(entity->locals, name);
    if (id == 0) {
        list_destroy(&ids);
        return NULL;
    }
    list_add(ids, FIT_INT2PTR(id));
    return ids;
}

void tt2_delete_cycle(FitEntity *entity, FitList *ids)
{
    int i, j;

    /* the description names the first variable twice */
    list_del_insert(ids, 1);
    for (i = 0; i < tt_count(entity->fcts); i++) {
        FitFct *fct = entity->fcts->items[i];
        int pos;

        for (j = 0; j < tt_count(ids); j++)
            if (fct->var_id == FIT_PTR2INT(ids->items[j]))
                break;
        if (j >= tt_count(ids))
            continue;
        pos = list_find(entity->locals, FIT_INT2PTR(fct->var_id), var_id_eq);
        if (pos != 0)
            list_del_insert(entity->locals, pos);
        fct_free(&fct);
        list_del_insert(entity->fcts, i + 1);
        list_del_insert(ids, j + 1);
        i--;
    }
}

void tt2_delete_unused_cycles(FitEntity *entity)
{
    char *cycle;
    FitList *ids;

    if (lib_split_fed_back_outputs(entity) == 0)
        return;
    for (;;) {
        cycle = NULL;
        if (tt2_check_cycles(entity, &cycle) != 0)
            break;
        if (cycle == NULL)
            tt_unreachable("tt2_delete_unused_cycles: cycle check failed without a cycle");
        /* A cycle that is not made of locals only is found again and
         * again, and the loop does not end.  This is a known limitation,
         * left as it is. */
        ids = tt2_cycle_to_ids(entity, cycle);
        if (ids != NULL) {
            fprintf(g_msg, "Info -  The following Local unused cycle is deleted :\n");
            fprintf(g_msg, " %s\n", cycle);
            free(cycle);
            tt2_delete_cycle(entity, ids);
        }
    }
}

void tt2_invert_fct(FitEntity *entity, int id)
{
    FitFct *fct = entity_find_fct(entity, id);

    if (fct == NULL)
        tt_unreachable("tt2_invert_fct: no function for the variable to invert");
    fct->tree[0] = fcttree_new_not(fct->tree[0]);
}
