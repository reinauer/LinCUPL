/*
 * tt_flatten.c - fct_flatten: turns the functions of an entity into sums
 * of products, splitting a function with new local variables when its
 * expansion would exceed a limit.
 */
#include "tt_priv.h"

int g_flatten_max_terms;
int g_flatten_limit_sums;

/* Returned instead of a list when the limit is exceeded. */
static FitList flatten_fail_marker;
#define FLATTEN_FAIL (&flatten_fail_marker)

static FitList *flatten_tree_to_pterms(FitFctTree *tree, FitList *vars);

/* Estimated number of product terms of a subtree, kept in FitFctTree.user
 * by flatten_split_tree. */
static int tree_weight(const FitFctTree *t) { return FIT_PTR2INT(t->user); }
static void tree_set_weight(FitFctTree *t, int w) { t->user = FIT_INT2PTR(w); }

void tt_unreachable(const char *what)
{
    fprintf(stderr, "fit150x: internal error: %s\n", what);
    fflush(stderr);
    g_exit_code = FIT_EXIT_MEMORY;
    exit(3);
}

void tt_not_supported(const char *what)
{
    fprintf(stderr, "fit150x: %s is not supported\n", what);
    fflush(stderr);
    exit(3);
}

void flatten_fold_tree(FitFctTree *tree)
{
    FitFctTree *child;
    FitList *kids;
    int absorbing;      /* constant that decides the node */
    int i;

    switch (tree->sel) {
    case FIT_SEL_NOT:
        child = tree->u.children->items[0];
        flatten_fold_tree(child);
        if (child->sel == FIT_SEL_CONST) {
            int value = child->u.value != 1;
            list_destroy(&tree->u.children);
            tree->sel = FIT_SEL_CONST;
            tree->u.value = value;
            fcttree_free(&child);
        }
        return;
    case FIT_SEL_AND:
    case FIT_SEL_OR:
        absorbing = tree->sel == FIT_SEL_AND ? 0 : 1;
        for (i = 0; i < tt_count(tree->u.children); i++) {
            child = tree->u.children->items[i];
            flatten_fold_tree(child);
            if (child->sel != FIT_SEL_CONST)
                continue;
            /* AND: a child other than 1 makes the node 0;
             * OR: a child other than 0 makes the node 1 */
            if (tree->sel == FIT_SEL_AND ? child->u.value != 1 : child->u.value != 0) {
                list_free_all(&tree->u.children, tt_cb_free_tree);
                tree->sel = FIT_SEL_CONST;
                tree->u.value = absorbing;
                return;
            }
            list_del_shift_free(tree->u.children, i + 1, tt_cb_free_tree);
            i--;
        }
        kids = tree->u.children;
        if (kids == NULL || kids->count == 0) {
            if (kids != NULL)
                list_destroy(&tree->u.children);
            tree->sel = FIT_SEL_CONST;
            tree->u.value = !absorbing;
            return;
        }
        if (kids->count == 1) {
            child = kids->items[0];
            list_destroy(&tree->u.children);
            tree->sel = child->sel;
            tree->u = child->u;
            free(child);
        }
        return;
    default:
        return;
    }
}

void flatten_fold_entity(FitEntity *entity)
{
    int i;

    for (i = 0; i < tt_count(entity->fcts); i++) {
        FitFct *fct = entity->fcts->items[i];
        flatten_fold_tree(fct->tree[0]);
    }
}

void flatten_add_fed_back_outputs(FitEntity *entity)
{
    int i;

    for (i = 0; i < tt_count(entity->outputs); i++) {
        FitVariable *out = entity->outputs->items[i];
        if (tt2_count_id_uses(entity, out->id) != 0)
            list_add(entity->inputs, var_copy(out));
    }
    entity->n_in = tt_count(entity->inputs);
}

void flatten_drop_fed_back_outputs(FitEntity *entity)
{
    int i;

    for (i = 0; i < tt_count(entity->inputs); i++) {
        FitVariable *in = entity->inputs->items[i];
        if (list_find(entity->outputs, in, var_same_name) != 0) {
            var_free(&in);
            list_del_insert(entity->inputs, i + 1);
            i--;
        }
    }
    entity->n_in = tt_count(entity->inputs);
}

int *flatten_new_pair(void)
{
    return util_calloc(1, 2 * sizeof(int));
}

void flatten_free_ref(void **ref)
{
    if (*ref != NULL) {
        free(*ref);
        *ref = NULL;
    }
}

/* FitListCmp on the weight (second int) of two pairs */
static int flatten_cmp_pair_weight(void *a, void *b)
{
    int wa = ((int *)a)[1];
    int wb = ((int *)b)[1];

    if (wa > wb)
        return 1;
    return wa >= wb ? 0 : -1;
}

/* complement of a sum of products (De Morgan, multiplied out).
 * FLATTEN_FAIL when an intermediate result has more than
 * g_flatten_max_terms terms. */
static FitList *flatten_complement_pterms(FitList *pterms, int nwords)
{
    FitList *acc = list_create_sized(1);
    FitList *prod = NULL;
    FitList *comp;
    int i;

    list_add(acc, pterm_new(nwords));
    for (i = 0; i < tt_count(pterms); i++) {
        comp = pterm_complement(pterms->items[i], nwords);
        prod = pterm_list_and(acc, comp, nwords);
        if (tt_count(prod) > g_flatten_max_terms) {
            list_free_all(&acc, tt_cb_free_pterm);
            list_free_all(&prod, tt_cb_free_pterm);
            list_free_all(&comp, tt_cb_free_pterm);
            return FLATTEN_FAIL;
        }
        list_free_all(&acc, tt_cb_free_pterm);
        list_free_all(&comp, tt_cb_free_pterm);
        acc = prod;
    }
    /* The only caller never passes an empty list. */
    return acc;
}

static FitList *single_pterm_list(FitPTerm *p)
{
    FitList *l = list_create();

    list_add(l, p);
    return l;
}

/* the list of FitPTerm of a tree over the variables `vars` (bit position =
 * position in the list).  NULL for an unusable tree, FLATTEN_FAIL when the
 * limit is exceeded.  Its messages call it "FctTreeToLPTerm". */
static FitList *flatten_tree_to_pterms(FitFctTree *tree, FitList *vars)
{
    FitList *acc, *part, *prod;
    FitPTerm *p;
    int n_vars, nwords, i, pos;

    if (tree == NULL)
        return NULL;
    n_vars = tt_count(vars);
    nwords = tt_words(n_vars);

    switch (tree->sel) {
    case FIT_SEL_NOT:
        part = flatten_tree_to_pterms(tree->u.children->items[0], vars);
        if (part == FLATTEN_FAIL)
            return FLATTEN_FAIL;
        if (part == NULL || part->count == 0) {
            if (part != NULL)
                list_destroy(&part);
            return single_pterm_list(pterm_new(nwords));
        }
        acc = flatten_complement_pterms(part, nwords);
        list_free_all(&part, tt_cb_free_pterm);
        return acc;

    case FIT_SEL_AND:
        acc = flatten_tree_to_pterms(tree->u.children->items[0], vars);
        if (acc == FLATTEN_FAIL)
            return FLATTEN_FAIL;
        if (acc == NULL || acc->count == 0)
            return acc;
        for (i = 1; i < tt_count(tree->u.children); i++) {
            int n_acc, n_part;

            part = flatten_tree_to_pterms(tree->u.children->items[i], vars);
            if (part == FLATTEN_FAIL) {
                list_free_all(&acc, tt_cb_free_pterm);
                return FLATTEN_FAIL;
            }
            n_acc = tt_count(acc);
            n_part = tt_count(part);
            prod = pterm_list_and(acc, part, nwords);
            if (tt_count(prod) > g_flatten_max_terms) {
                list_free_all(&prod, tt_cb_free_pterm);
                list_free_all(&acc, tt_cb_free_pterm);
                list_free_all(&part, tt_cb_free_pterm);
                return FLATTEN_FAIL;
            }
            if ((n_part + n_acc) * 2 < tt_count(prod)) {
                FitList *reduced = domlist_minimise_pterms(prod, NULL, (unsigned)n_vars,
                                                           0xfffffffeu);
                list_free_all(&prod, tt_cb_free_pterm);
                prod = reduced;
            }
            list_free_all(&acc, tt_cb_free_pterm);
            list_free_all(&part, tt_cb_free_pterm);
            acc = prod;
            if (acc == NULL)
                return NULL;
            if (acc->count == 0)
                return acc;
        }
        return acc;

    case FIT_SEL_OR:
        acc = list_create();
        for (i = 0; i < tt_count(tree->u.children); i++) {
            part = flatten_tree_to_pterms(tree->u.children->items[i], vars);
            if (part == FLATTEN_FAIL) {
                list_free_all(&acc, tt_cb_free_pterm);
                return FLATTEN_FAIL;
            }
            if (part != NULL && part->count != 0)
                list_append_destroy(acc, &part);
            else if (part != NULL)
                list_destroy(&part);
            if (g_flatten_limit_sums != 0 && tt_count(acc) > g_flatten_max_terms) {
                list_free_all(&acc, tt_cb_free_pterm);
                return FLATTEN_FAIL;
            }
        }
        return acc;

    case FIT_SEL_CONST:
        if (tree->u.value == 0)
            return list_create_sized(0);
        if (tree->u.value == 1)
            return single_pterm_list(pterm_new(nwords));
        fprintf(g_msg, "ERROR in FctTreeToLPTerm (Complex constant)\n");
        fprintf(g_msg, "  Result of the command will be incorrect - delete it\n");
        fprintf(g_msg, "  Replace complex variables by 0/1 and run the command again\n");
        return NULL;

    case 'F':
        acc = single_pterm_list(pterm_new_zero(nwords));
        fprintf(g_msg, "Warning in FctTreeToLPTerm (FALSE found)\n");
        fprintf(g_msg, "  Result of the command will be correct\n");
        return acc;

    case FIT_SEL_ID:
        pos = var_list_pos_of_id(vars, tree->u.id);
        if (pos == 0) {
            fprintf(g_msg, "ERROR in FctTreeToLPTerm (Id is neither OUT nor IN nor LOCAL)\n");
            fprintf(g_msg, "  Result of the command will be incorrect - delete it\n");
            return NULL;
        }
        p = pterm_new(nwords);
        p->bits[(unsigned)(pos - 1) >> 5] |= 1u << ((unsigned)(pos - 1) & 31);
        p->care[(unsigned)(pos - 1) >> 5] |= 1u << ((unsigned)(pos - 1) & 31);
        return single_pterm_list(p);

    case FIT_SEL_SET:
        fprintf(g_msg, "ERROR in FctTreeToLPTerm (SIDF found)\n");
        fprintf(g_msg, "  Result of the command will be incorrect - delete it\n");
        fprintf(g_msg, " Replace structured variables by simple ones and run the command again\n");
        return NULL;

    case FIT_SEL_EMPTY:
        acc = single_pterm_list(pterm_new(nwords));
        fprintf(g_msg, "Warning in FctTreeToLPTerm (TRUE found)\n");
        fprintf(g_msg, "Result of the command will be correct\n");
        return acc;

    default:
        return NULL;
    }
}

/* annotate the tree with the number of product terms its
 * expansion would have (FitFctTree.user) and, where an AND node exceeds
 * g_flatten_max_terms, move groups of its factors into new local functions
 * of the entity.  Returns the tree; NULL for a node other than ! * + C I. */
static FitFctTree *flatten_split_tree(FitEntity *entity, FitFctTree *tree)
{
    FitList *pairs, *groups, *first, *weights, *group;
    FitFctTree *child, *node;
    int *pair;
    int i, k, j, w, total, over;

    switch (tree->sel) {
    case FIT_SEL_NOT:
    case FIT_SEL_CONST:
    case FIT_SEL_ID:
        tree_set_weight(tree, 1);
        return tree;
    case FIT_SEL_OR:
        total = 0;
        for (i = 0; i < tt_count(tree->u.children); i++) {
            child = flatten_split_tree(entity, tree->u.children->items[i]);
            if (child == NULL)
                tt_unreachable("flatten_split_tree: XOR or structured operand while splitting a sum");
            total = (int)((unsigned)total + (unsigned)tree_weight(child));
            tree->u.children->items[i] = child;
        }
        tree_set_weight(tree, total);
        return tree;
    case FIT_SEL_AND:
        break;
    default:
        return NULL;
    }

    total = 1;
    over = 0;
    pairs = list_create_sized(tt_count(tree->u.children));
    for (i = 0; i < tt_count(tree->u.children); i++) {
        child = flatten_split_tree(entity, tree->u.children->items[i]);
        if (child == NULL)
            tt_unreachable("flatten_split_tree: XOR or structured operand while splitting a product");
        tree->u.children->items[i] = child;
        total = tt_mul(total, tree_weight(child));
        pair = flatten_new_pair();
        pair[0] = i;
        pair[1] = tree_weight(child);
        list_add(pairs, pair);
        if (total > g_flatten_max_terms && !over)
            over = 1;
    }
    if (!over) {
        tree_set_weight(tree, total);
        return tree;
    }

    /* Distribute the factors, lightest first, over groups whose product of
     * weights stays within the limit.  Group 0 takes the factors of weight
     * 1, group 1 is the first real group. */
    list_sort(pairs, 0, flatten_cmp_pair_weight);
    groups = list_create();
    first = list_create();
    list_add(groups, first);
    weights = list_create();
    list_add(weights, FIT_INT2PTR(1));
    for (k = 0; k < tt_count(pairs); k++) {
        pair = pairs->items[k];
        child = tree->u.children->items[pair[0]];
        if (pair[1] == 1) {
            list_add(first, child);
            continue;
        }
        w = 0;
        for (j = 1; j < tt_count(groups); j++) {
            w = tt_mul(FIT_PTR2INT(weights->items[j]), pair[1]);
            if (w <= g_flatten_max_terms)
                break;
        }
        if (j < tt_count(groups)) {
            list_add(groups->items[j], child);
            weights->items[j] = FIT_INT2PTR(w);
        } else {
            group = list_create();
            list_add(groups, group);
            list_add(group, child);
            list_add(weights, FIT_INT2PTR(pair[1]));
        }
    }
    list_free_all(&pairs, flatten_free_ref);
    list_destroy(&weights);

    /* Factors of weight 1 and the first group stay; every further group
     * becomes a local function. */
    tree->u.children->count = 0;
    for (i = 0; i < tt_count(first); i++)
        list_add(tree->u.children, first->items[i]);
    /* There may be no second group. */
    group = groups->count > 1 ? groups->items[1] : NULL;
    for (i = 0; i < tt_count(group); i++)
        list_add(tree->u.children, group->items[i]);
    for (k = 2; k < tt_count(groups); k++) {
        FitFct *fct;
        int id;

        group = groups->items[k];
        id = bdd_new_local_var(entity);
        fct = fct_new();
        fct->var_id = id;
        if (group == NULL || group->count != 1) {
            node = fcttree_new();
            node->sel = FIT_SEL_AND;
            node->u.children = list_create_sized(tt_count(group));
            for (i = 0; i < tt_count(group); i++)
                list_add(node->u.children, group->items[i]);
            fct->tree[0] = node;
        } else {
            fct->tree[0] = group->items[0];
        }
        entity->n_local++;
        list_add(entity->fcts, fct);
        node = fcttree_new();
        node->sel = FIT_SEL_ID;
        node->u.id = id;
        tree_set_weight(node, 1);
        list_add(tree->u.children, node);
    }
    list_free_all(&groups, tt_cb_free_list);

    total = 1;
    for (i = 0; i < tt_count(tree->u.children); i++)
        total = tt_mul(total, tree_weight(tree->u.children->items[i]));
    tree_set_weight(tree, total);
    return tree;
}

/* where two functions have the same sum of products, replace the
 * later one by a reference to the earlier (unless the earlier is an output,
 * or merely a copy of another output or local).  Returns 1 if something
 * was replaced. */
static int flatten_share_equal_fcts(FitEntity *entity)
{
    FitList *plists = list_create_sized(tt_count(entity->fcts));
    int n_vars = tt_count(entity->outputs) + tt_count(entity->inputs) + tt_count(entity->locals);
    int nwords = tt_words(n_vars);
    int changed = 0;
    int i, j;

    for (i = 0; i < tt_count(entity->fcts); i++) {
        FitFct *fct = entity->fcts->items[i];
        list_add(plists, fcttree_to_pterms3(fct->tree[0], entity->outputs, entity->inputs,
                                            entity->locals));
    }
    for (i = 0; i < tt_count(entity->fcts); i++) {
        FitFct *fct = entity->fcts->items[i];
        FitList *mine;

        if (var_list_pos_of_id(entity->outputs, fct->var_id) != 0)
            continue;
        if (fct->tree[0]->sel == FIT_SEL_ID
            && (var_list_pos_of_id(entity->locals, fct->tree[0]->u.id) != 0
                || var_list_pos_of_id(entity->outputs, fct->tree[0]->u.id) != 0))
            continue;
        mine = plists->items[i];
        for (j = i + 1; j < tt_count(entity->fcts); j++) {
            FitFct *other = entity->fcts->items[j];
            FitList *theirs = plists->items[j];
            FitFctTree *ref;

            if (tt_count(mine) != tt_count(theirs))
                continue;
            if (!list_equal_sets_x(mine, theirs, pterm_equal, FIT_INT2PTR(nwords)))
                continue;
            list_free_all(&theirs, tt_cb_free_pterm);
            fcttree_free(&other->tree[0]);
            ref = fcttree_new();
            ref->sel = FIT_SEL_ID;
            ref->u.id = fct->var_id;
            other->tree[0] = ref;
            plists->items[j] = fcttree_to_pterms3(ref, entity->outputs, entity->inputs,
                                                  entity->locals);
            changed = 1;
        }
    }
    for (i = 0; i < tt_count(entity->fcts); i++) {
        FitList *l = plists->items[i];
        list_free_all(&l, tt_cb_free_pterm);
    }
    list_destroy(&plists);
    return changed;
}

static void flatten_replace_id_in_tree(int old_id, int new_id, FitFctTree *tree)
{
    int i;

    if (tree->sel == FIT_SEL_CONST)
        return;
    if (tree->sel == FIT_SEL_ID) {
        if (tree->u.id == old_id)
            tree->u.id = new_id;
        return;
    }
    for (i = 0; i < tt_count(tree->u.children); i++)
        flatten_replace_id_in_tree(old_id, new_id, tree->u.children->items[i]);
}

void flatten_replace_id(int old_id, int new_id, FitEntity *entity)
{
    int i;

    for (i = 0; i < tt_count(entity->fcts); i++) {
        FitFct *fct = entity->fcts->items[i];
        flatten_replace_id_in_tree(old_id, new_id, fct->tree[0]);
    }
}

/* remove the local functions that are a plain copy of another
 * local or output, renaming their uses.  (The function object and its
 * variable are not freed.) */
static void flatten_drop_alias_locals(FitEntity *entity)
{
    int i;

    for (i = 0; i < tt_count(entity->fcts); i++) {
        FitFct *fct = entity->fcts->items[i];
        int target;

        if (var_list_pos_of_id(entity->outputs, fct->var_id) != 0)
            continue;
        if (fct->tree[0]->sel != FIT_SEL_ID)
            continue;
        target = fct->tree[0]->u.id;
        if (var_list_pos_of_id(entity->locals, target) == 0
            && var_list_pos_of_id(entity->outputs, target) == 0)
            continue;
        flatten_replace_id(fct->var_id, target, entity);
        list_del_shift(entity->fcts, i + 1);
        i--;
        list_del_insert(entity->locals, var_list_pos_of_id(entity->locals, fct->var_id));
        entity->n_local--;
    }
}

/* flatten_share_equal_fcts and flatten_drop_alias_locals until nothing
 * is shared any more */
static void flatten_share_fcts(FitEntity **entity)
{
    while (flatten_share_equal_fcts(*entity) != 0)
        flatten_drop_alias_locals(*entity);
}

void flatten_entity(FitEntity **entity, int max_terms)
{
    FitEntity *copy;
    FitList *vars, *pterms;
    int changed = 0;
    int n, i;

    if (g_fit_hoist_mode != 0)
        g_fit_hoisted_ids = list_create();
    g_flatten_max_terms = max_terms;

    copy = entity_copy(*entity);
    entity_free(entity);
    flatten_fold_entity(copy);
    *entity = entity_copy_inv_propagated(copy);
    entity_free(&copy);
    tt2_compact_entity(*entity);
    flatten_add_fed_back_outputs(*entity);

    vars = list_create();
    n = tt_count((*entity)->fcts);
    for (i = 0; i < n; i++) {
        FitFct *fct;

        list_append(vars, (*entity)->inputs);
        list_append(vars, (*entity)->locals);
        fct = (*entity)->fcts->items[i];
        pterms = flatten_tree_to_pterms(fct->tree[0], vars);
        if (pterms == FLATTEN_FAIL) {
            if (g_fit_hoist_mode == 0) {
                fct->tree[0] = flatten_split_tree(*entity, fct->tree[0]);
            } else {
                fit_prep_hoist_children(*entity, fct->tree[0]);
                list_add(g_fit_hoisted_ids, FIT_INT2PTR(fct->var_id));
            }
            changed = 1;
        } else {
            list_free_all(&pterms, tt_cb_free_pterm);
        }
        vars->count = 0;
    }
    list_destroy(&vars);
    if (changed)
        flatten_share_fcts(entity);
    flatten_drop_fed_back_outputs(*entity);
}
