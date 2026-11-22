/*
 * expand_tree.c - expression trees of the expand phase.
 *
 * Node and list cell pools, constant folding, DeMorgan (pushing negations
 * down to the variables), XOR expansion and multiplying AND over OR until
 * an equation is a sum of products.  The node layout and the 25 operand
 * forms of every operator are described in expand_int.h.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "expand_int.h"

/* -------------------------------------------------------------------------
 * Operand forms
 * ------------------------------------------------------------------------- */
const uint8_t expand_form_left[FORM_COUNT] = {
    K_NODE, K_NODE, K_NODE, K_VAR, K_NVAR, K_VAR, K_NVAR, K_VAR, K_NVAR,
    K_ZERO, K_ONE, K_ZERO, K_ONE, K_ZERO, K_ZERO, K_ONE, K_ONE,
    K_VAR, K_NVAR, K_VAR, K_NVAR, K_NODE, K_NODE, K_ZERO, K_ONE
};
const uint8_t expand_form_right[FORM_COUNT] = {
    K_NODE, K_VAR, K_NVAR, K_NODE, K_NODE, K_VAR, K_VAR, K_NVAR, K_NVAR,
    K_ZERO, K_ZERO, K_ONE, K_ONE, K_VAR, K_NVAR, K_VAR, K_NVAR,
    K_ZERO, K_ZERO, K_ONE, K_ONE, K_ZERO, K_ONE, K_NODE, K_NODE
};
/* form number for [left kind][right kind] */
static const uint8_t k_form_of[5][5] = {
    /* left N  */ { 0, 1, 2, 21, 22 },
    /* left V  */ { 3, 5, 7, 17, 19 },
    /* left V' */ { 4, 6, 8, 18, 20 },
    /* left 0  */ { 23, 13, 14, 9, 11 },
    /* left 1  */ { 24, 15, 16, 10, 12 }
};

/* -------------------------------------------------------------------------
 * State
 * ------------------------------------------------------------------------- */
static node_t *g_root;

/* Node and list cell pools */
static node_t *g_free_nodes;
static node_block_t *g_node_blocks;
static int g_live_nodes;
static cell_t *g_free_cells;

/* Sum-of-products expansion */
static cell_t *g_and_list;             /* AND nodes still to be multiplied out */
static cell_t *g_or_list;
static int g_or_count;
static int g_factor_is_true;
static node_t *g_products;
static node_t *g_products_tail;
static node_t *g_cur_head;
static node_t *g_cur_product;
static int g_product_count;

static void push_negation(node_t *n);

/* -------------------------------------------------------------------------
 * List cells
 * ------------------------------------------------------------------------- */

/* Allocate a block of POOL_BLOCK list cells as the free list. */
static void cell_pool_grow(void)
{
    cell_t *block = expand_alloc_tracked(POOL_BLOCK * sizeof(cell_t), "DeMorgan's expansion and");
    for (int i = 0; i < POOL_BLOCK; i++) {
        block[i].next = (i == POOL_BLOCK - 1) ? NULL : &block[i + 1];
    }
    g_free_cells = block;
}

static cell_t *cell_new(void)
{
    expand_progress_tick();
    if (g_free_cells == NULL) {
        cell_pool_grow();
    }
    cell_t *c = g_free_cells;
    g_free_cells = c->next;
    c->next = NULL;
    c->node = NULL;
    return c;
}

static void cell_free(cell_t *c)
{
    expand_progress_tick();
    c->next = g_free_cells;
    g_free_cells = c;
}

static void cell_list_free(cell_t *list)
{
    while (list != NULL) {
        cell_t *next = list->next;
        cell_free(list);
        list = next;
    }
}

static void cell_push(cell_t **list, node_t *n)
{
    cell_t *c = cell_new();
    c->node = n;
    c->next = *list;
    *list = c;
}

/* -------------------------------------------------------------------------
 * Node pool.  Freed nodes are reused last-in first-out and the whole pool is
 * rebuilt after each equation.  This order must not change: some paths look
 * at nodes after they were freed.
 * ------------------------------------------------------------------------- */

/* Chain the nodes of a block; returns the last one */
static node_t *thread_block(node_t *first, int count)
{
    node_t *last = first;
    for (int i = 0; i < count; i++) {
        last = &first[i];
        last->right.node = (i == count - 1) ? NULL : &first[i + 1];
    }
    return last;
}

/* Allocate another block of POOL_BLOCK nodes as the free list. */
static void node_pool_grow(void)
{
    node_t *first = expand_alloc_tracked(POOL_BLOCK * sizeof(node_t), "DeMorgan's expansion mem");
    node_block_t *block = expand_alloc_tracked(sizeof(*block), "Memory table");
    block->count = POOL_BLOCK;
    block->first = first;
    block->next = g_node_blocks;
    g_node_blocks = block;
    g_free_nodes = first;
    thread_block(first, POOL_BLOCK);
}

/* Make every node free again */
static void node_pool_reset(void)
{
    if (g_node_blocks == NULL) {
        expand_fatal_error(ERR_OUT_OF_MEMORY, "Can't release a null block");
    }
    g_free_nodes = g_node_blocks->first;
    g_live_nodes = 0;
    for (node_block_t *b = g_node_blocks; b != NULL; b = b->next) {
        node_t *last = thread_block(b->first, b->count);
        if (b->next != NULL) {
            last->right.node = b->next->first;
        }
    }
}

/* Take a node from the free list, cleared. */
static node_t *node_new(void)
{
    expand_progress_tick();
    if (g_free_nodes == NULL) {
        node_pool_grow();
    }
    node_t *n = g_free_nodes;
    g_free_nodes = n->right.node;
    g_live_nodes++;
    n->type = 0;
    n->flags = 0;
    n->left.raw = 0;
    n->right.raw = 0;
    return n;
}

/* Put a node back at the front of the free list. */
static void node_free(node_t *n)
{
    expand_progress_tick();
    if (n != NULL) {
        n->right.raw = 0;
        n->right.node = g_free_nodes;
        g_free_nodes = n;
        g_live_nodes--;
    }
}

node_t *expand_new_node_of(int type, int flags)
{
    node_t *n = node_new();
    n->type = (uint8_t)type;
    n->flags = (uint8_t)flags;
    return n;
}

/* -------------------------------------------------------------------------
 * Tree primitives
 * ------------------------------------------------------------------------- */

/* Release a tree.  Only operands that are sub-trees are followed,
 * so the literal chains of product lists are left to node_pool_reset(). */
static void tree_free(node_t *root)
{
    node_t *parent = NULL;
    int side = 0;
    node_t *cur = root;
    while (cur != NULL) {
        if (has_left_node(cur) && cur->left.node != NULL) {
            side = 1;
            parent = cur;
            cur = cur->left.node;
        } else if (has_right_node(cur) && cur->right.node != NULL) {
            side = 2;
            parent = cur;
            cur = cur->right.node;
        } else {
            node_free(cur);
            if (side == 1 && parent != NULL) {
                parent->left.raw = 0;
            } else if (side == 2 && parent != NULL) {
                parent->right.raw = 0;
            }
            cur = (cur == root) ? NULL : root;
            parent = NULL;
        }
    }
}

/* Copy a tree.  The stored values of constant operands are not
 * copied; they are implied by the node type. */
static node_t *tree_copy(const node_t *n)
{
    if (n == NULL) {
        return NULL;
    }
    node_t *copy = expand_new_node_of(n->type, n->flags);
    if (op_of(n->type) != 0) {
        operand_kind_t lk = left_kind(n), rk = right_kind(n);
        if (rk == K_VAR || rk == K_NVAR) {
            copy->right = n->right;
        }
        if (lk == K_VAR || lk == K_NVAR) {
            copy->left = n->left;
        }
    } else if (n->type == NODE_VAR || n->type == NODE_CONST) {
        copy->right = n->right;
    }
    if (has_left_node(n)) {
        copy->left.node = tree_copy(n->left.node);
    }
    if (has_right_node(n)) {
        copy->right.node = tree_copy(n->right.node);
    }
    return copy;
}

/* Overwrite a node with another operator node, taking over its
 * operands.  A plain variable or constant source only passes on its type. */
static void node_assign(node_t *dst, const node_t *src)
{
    if (dst == NULL || src == NULL) {
        return;
    }
    dst->type = src->type;
    dst->flags = src->flags;
    if (op_of(src->type) != 0) {
        operand_kind_t lk = left_kind(src), rk = right_kind(src);
        if (rk == K_VAR || rk == K_NVAR || rk == K_NODE) {
            dst->right = src->right;
        }
        if (lk == K_VAR || lk == K_NVAR || lk == K_NODE) {
            dst->left = src->left;
        }
    } else if (src->type == NODE_NOT_VAR || src->type == NODE_NOT_NVAR || src->type == NODE_NOT) {
        dst->right = src->right;
    }
}

/* Store an operand in a node according to its kind. */
static void set_left(node_t *n, operand_kind_t kind, const node_t *source)
{
    if (kind == K_ZERO || kind == K_ONE) {
        n->left.raw = (kind == K_ONE);
    } else if (kind == K_NODE) {
        n->left.node = (node_t *)source;
    } else {
        n->left = source->right;
    }
}

static void set_right(node_t *n, operand_kind_t kind, const node_t *source)
{
    if (kind == K_ZERO || kind == K_ONE) {
        n->right.raw = (kind == K_ONE);
    } else if (kind == K_NODE) {
        n->right.node = (node_t *)source;
    } else {
        n->right = source->right;
    }
}

/* New AND / OR node over two trees.  Variables and constants are
 * folded into the node; the operand nodes themselves are left alone. */
static node_t *make_node(int op, node_t *left, node_t *right)
{
    node_t *n = node_new();
    if (op != OP_AND && op != OP_OR) {
        return n;
    }
    operand_kind_t lk = kind_of(left), rk = kind_of(right);
    n->type = (uint8_t)(op + k_form_of[lk][rk]);
    set_left(n, lk, left);
    set_right(n, rk, right);
    return n;
}

/* Give an operator node its operands while a tree is built.
 * Variables and constants are folded into the node and released; a sub-tree
 * operand is left for the caller to attach.  Constants are stored with
 * their effective value, including negation.  An AND/OR/XOR that receives
 * only one operand gets a "sub-tree, leaf" form whatever side it is on. */
void expand_fold_operands(node_t *right, node_t *left, node_t *op_node)
{
    int op = op_node->type;
    if (op == NODE_NOT) {
        if (left != NULL || right == NULL) {
            return;
        }
        switch (kind_of(right)) {
        case K_VAR: op_node->type = NODE_NOT_VAR; break;
        case K_NVAR: op_node->type = NODE_NOT_NVAR; break;
        case K_ONE: op_node->type = NODE_NOT_ONE; break;
        case K_ZERO: op_node->type = NODE_NOT_ZERO; break;
        default: return;
        }
        op_node->right = right->right;
        node_free(right);
        return;
    }
    if (op != OP_AND && op != OP_XOR && op != OP_OR) {
        return;
    }
    if (right == NULL && left == NULL) {
        return;
    }
    if (left == NULL || right == NULL) {
        node_t *only = left == NULL ? right : left;
        operand_kind_t k = kind_of(only);
        if (k == K_NODE) {
            return;
        }
        op_node->type = (uint8_t)(op + k_form_of[K_NODE][k]);
        if (left == NULL || k == K_ZERO) {
            set_right(op_node, k, only);
        } else {
            set_left(op_node, k, only);
        }
        node_free(only);
        return;
    }
    operand_kind_t lk = kind_of(left), rk = kind_of(right);
    if (is_leaf_kind(lk) && is_leaf_kind(rk)) {
        op_node->type = (uint8_t)(op + k_form_of[lk][rk]);
        set_right(op_node, rk, right);
        set_left(op_node, lk, left);
        node_free(right);
        node_free(left);
    } else if (is_leaf_kind(rk)) {
        op_node->type = (uint8_t)(op + k_form_of[K_NODE][rk]);
        set_right(op_node, rk, right);
        node_free(right);
    } else if (is_leaf_kind(lk)) {
        op_node->type = (uint8_t)(op + k_form_of[lk][K_NODE]);
        set_left(op_node, lk, left);
        node_free(left);
    }
}

/* Fold variables and constants that have become operands of AND
 * and OR nodes into those nodes, bottom up. */
void expand_fold_tree(node_t *n)
{
    if (n == NULL) {
        return;
    }
    expand_progress_tick();
    if (has_left_node(n)) {
        expand_fold_tree(n->left.node);
    }
    if (has_right_node(n)) {
        expand_fold_tree(n->right.node);
    }
    int op = op_of(n->type);
    if (op != OP_AND && op != OP_OR) {
        return;
    }
    int form = n->type - op;
    operand_kind_t lk = (operand_kind_t)expand_form_left[form];
    operand_kind_t rk = (operand_kind_t)expand_form_right[form];
    node_t *left = lk == K_NODE ? n->left.node : NULL;
    node_t *right = rk == K_NODE ? n->right.node : NULL;
    operand_kind_t new_lk = left != NULL ? kind_of(left) : lk;
    operand_kind_t new_rk = right != NULL ? kind_of(right) : rk;
    if (new_lk == lk && new_rk == rk) {
        return;
    }
    n->type = (uint8_t)(op + k_form_of[new_lk][new_rk]);
    if (left != NULL && new_lk != K_NODE) {
        set_left(n, new_lk, left);
        node_free(left);
    }
    if (right != NULL && new_rk != K_NODE) {
        set_right(n, new_rk, right);
        node_free(right);
    }
}

/* -------------------------------------------------------------------------
 * DeMorgan and XOR expansion
 * ------------------------------------------------------------------------- */

/* Form of the dual operator after both operands have been negated */
static const uint8_t k_negated_form[FORM_COUNT] = {
    0, 2, 1, 4, 3, 8, 7, 6, 5, 12, 11, 10, 9, 16, 15, 14, 13, 20, 19, 18, 17, 22, 21, 24, 23
};

/* Push negations down to the variables.  A negated AND becomes
 * the OR of its negated operands and the other way round; NOT nodes are
 * removed.  XOR nodes keep their flag for expand_xors(). */
static void push_negation(node_t *n)
{
    for (;;) {
        if (n == NULL) {
            return;
        }
        if (n->type != NODE_NOT) {
            break;
        }
        node_t *operand = n->right.node;
        if (operand == NULL) {
            return; /* malformed tree: stop instead of looping */
        }
        if ((n->flags & NODE_NEGATED) == 0) {
            toggle_negation(operand);
        }
        node_assign(n, operand);
        node_free(operand);
    }
    int op = op_of(n->type);
    if (op == OP_AND || op == OP_OR) {
        if (n->flags & NODE_NEGATED) {
            n->flags &= (uint8_t)~NODE_NEGATED;
            int form = k_negated_form[n->type - op];
            n->type = (uint8_t)((op == OP_AND ? OP_OR : OP_AND) + form);
            switch (expand_form_left[form]) {
            case K_NODE: toggle_negation(n->left.node); break;
            case K_ZERO: n->left.raw = 0; break;
            case K_ONE: n->left.raw = 1; break;
            default: break;
            }
            switch (expand_form_right[form]) {
            case K_NODE: toggle_negation(n->right.node); break;
            case K_ZERO: n->right.raw = 0; break;
            case K_ONE: n->right.raw = 1; break;
            default: break;
            }
        }
    } else if (op == 0) {
        switch (n->type) {
        case NODE_VAR:
        case NODE_LIST:
        case NODE_CONST:
            break;
        case NODE_NOT_VAR:
            n->type = NODE_VAR;
            if ((n->flags & NODE_NEGATED) == 0) {
                n->flags = NODE_NEGATED;
            } else {
                n->flags &= (uint8_t)~NODE_NEGATED;
            }
            expand_fold_tree(g_root);
            break;
        case NODE_NOT_NVAR:
            n->type = NODE_VAR;
            if (n->flags & NODE_NEGATED) {
                n->flags = NODE_NEGATED;
            }
            expand_fold_tree(g_root);
            break;
        case NODE_NOT_ZERO:
            n->type = NODE_CONST;
            n->right.raw = (n->flags & NODE_NEGATED) == 0;
            n->flags = 0;
            expand_fold_tree(g_root);
            break;
        case NODE_NOT_ONE:
            n->type = NODE_CONST;
            n->right.raw = (n->flags & NODE_NEGATED) != 0;
            n->flags = 0;
            expand_fold_tree(g_root);
            break;
        default:
            expand_report_error(ERR_INTERNAL_NEGATE_TYPE, "");
            break;
        }
    }
    if (has_left_node(n)) {
        push_negation(n->left.node);
    }
    if (has_right_node(n)) {
        push_negation(n->right.node);
    }
}

/* Negate a tree */
static void negate_tree(node_t *n)
{
    if (n == NULL) {
        return;
    }
    toggle_negation(n);
    push_negation(n);
}

/* An operand of a node as a tree of its own; variables and constants get a
 * temporary node. */
static node_t *operand_as_tree(operand_kind_t kind, operand_t operand)
{
    node_t *n;
    switch (kind) {
    case K_NODE:
        return operand.node;
    case K_VAR:
    case K_NVAR:
        n = expand_new_node_of(NODE_VAR, kind == K_NVAR ? NODE_NEGATED : 0);
        n->right = operand;
        return n;
    default:
        n = expand_new_node_of(NODE_CONST, 0);
        n->right.raw = (kind == K_ONE);
        return n;
    }
}

static void free_if_leaf(node_t *n)
{
    if (n != NULL && (n->type == NODE_VAR || n->type == NODE_CONST)) {
        node_free(n);
    }
}

/* Rewrite  a $ b  as  a & !b # !a & b  (or, for a negated XOR,
 * a & b # !a & !b). */
static void expand_xor(node_t *n)
{
    operand_kind_t lk = left_kind(n), rk = right_kind(n);
    node_t *right = operand_as_tree(rk, n->right);
    node_t *left = operand_as_tree(lk, n->left);
    node_t *not_left = tree_copy(left);
    negate_tree(not_left);
    node_t *not_right = tree_copy(right);
    negate_tree(not_right);
    n->type = OP_OR;
    if ((n->flags & NODE_NEGATED) == 0) {
        n->left.node = make_node(OP_AND, left, not_right);
        n->right.node = make_node(OP_AND, not_left, right);
    } else {
        n->flags &= (uint8_t)~NODE_NEGATED;
        n->left.node = make_node(OP_AND, left, right);
        n->right.node = make_node(OP_AND, not_left, not_right);
    }
    free_if_leaf(right);
    free_if_leaf(left);
    free_if_leaf(not_right);
    free_if_leaf(not_left);
}

/* Expand every XOR in the tree.  An XOR the device implements is
 * kept; if it is negated the negation is moved to its left operand. */
static void expand_xors(node_t *n)
{
    if (n == NULL) {
        return;
    }
    if (op_of(n->type) == OP_XOR) {
        if ((n->flags & NODE_KEEP_XOR) == 0) {
            expand_xor(n);
        } else if (n->flags & NODE_NEGATED) {
            n->flags &= (uint8_t)~NODE_NEGATED;
            int form = n->type - OP_XOR;
            operand_kind_t lk = (operand_kind_t)expand_form_left[form];
            operand_kind_t rk = (operand_kind_t)expand_form_right[form];
            switch (lk) {
            case K_NODE: negate_tree(n->left.node); break;
            case K_VAR: lk = K_NVAR; break;
            case K_NVAR: lk = K_VAR; break;
            case K_ZERO: lk = K_ONE; n->left.raw = 1; break;
            case K_ONE: lk = K_ZERO; n->left.raw = 0; break;
            }
            n->type = (uint8_t)(OP_XOR + k_form_of[lk][rk]);
        }
    }
    if (has_left_node(n)) {
        expand_xors(n->left.node);
    }
    if (has_right_node(n)) {
        expand_xors(n->right.node);
    }
}

/* -------------------------------------------------------------------------
 * Sum of products
 *
 * An AND node that has an OR below it is multiplied out.  The work list is a
 * list of products; each product is a header node (type OP_OR, flag = 1 when
 * the product is dropped) whose `left` chain holds its literals (NODE_VAR)
 * and whose `right` links the next product.  A product whose first literal
 * was turned into NODE_CONST is false.
 * ------------------------------------------------------------------------- */

/* Start a new product at the end of the current list */
static void new_product(void)
{
    g_product_count++;
    node_t *p = expand_new_node_of(OP_OR, 0);
    if (g_cur_head == NULL) {
        g_cur_head = p;
    } else {
        g_cur_product->right.node = p;
    }
    g_cur_product = p;
}

/* The current product contains a contradiction or a 0 */
static void mark_product_false(void)
{
    node_t *first = g_cur_product->left.node;
    if (first == NULL) {
        node_t *t = expand_new_node_of(NODE_CONST, 0);
        t->left.node = g_cur_product->left.node;
        g_cur_product->left.node = t;
    } else {
        first->type = NODE_CONST;
        first->flags = 0;
        first->right.raw = 0;
    }
}

/* 1 if the literal is already in the current product (or the
 * product is false), 2 if its complement is, else 0 */
static int find_literal(const node_t *lit)
{
    const node_t *l = g_cur_product->left.node;
    if (l == NULL) {
        return 0;
    }
    if (l->type == NODE_CONST) {
        return 1;
    }
    for (; l != NULL; l = l->left.node) {
        if (l->right.raw == lit->right.raw) {
            if (l->flags == lit->flags) {
                return 1;
            }
            if ((l->flags == 1 && lit->flags == 0) || (l->flags == 0 && lit->flags == 1)) {
                return 2;
            }
        }
    }
    return 0;
}

/* AND a variable into the current product.  The node made for a literal
 * that turns out not to be needed is not released. */
static void add_literal(operand_t variable, int negated)
{
    node_t *first = g_cur_product->left.node;
    if (first != NULL && first->type == NODE_CONST) {
        return;
    }
    node_t *lit = expand_new_node_of(NODE_VAR, negated ? 1 : 0);
    lit->right = variable;
    int found = find_literal(lit);
    if (found == 0) {
        lit->left.node = g_cur_product->left.node;
        g_cur_product->left.node = lit;
    } else if (found == 2) {
        mark_product_false();
    }
}

/* One operand of a node that is a variable or a constant, ANDed into the
 * current product. */
static void add_operand(const node_t *n, int right_side)
{
    operand_kind_t k = right_side ? right_kind(n) : left_kind(n);
    operand_t o = right_side ? n->right : n->left;
    switch (k) {
    case K_VAR: add_literal(o, 0); break;
    case K_NVAR: add_literal(o, 1); break;
    case K_ZERO: mark_product_false(); break;
    default: break;
    }
}

/* AND every literal of a tree of AND nodes into the current
 * product.  A node with a 0 operand makes the product false once. */
static void add_and_literals(const node_t *n)
{
    if (n == NULL) {
        return;
    }
    if (has_right_node(n)) {
        add_and_literals(n->right.node);
    }
    if (has_left_node(n)) {
        add_and_literals(n->left.node);
    }
    if (op_of(n->type) != OP_AND) {
        return;
    }
    operand_kind_t lk = left_kind(n), rk = right_kind(n);
    if (lk == K_ZERO || rk == K_ZERO) {
        mark_product_false();
        return;
    }
    add_operand(n, 1);
    add_operand(n, 0);
}

/* Copy a list of products */
static node_t *copy_products(const node_t *list)
{
    node_t *head = NULL, *tail = NULL;
    for (; list != NULL; list = list->right.node) {
        node_t *first = NULL, *prev = NULL;
        for (const node_t *q = list; q != NULL; q = q->left.node) {
            node_t *c = expand_new_node_of(q->type, q->flags);
            if (q->type == NODE_VAR || q->type == NODE_CONST) {
                c->right = q->right;
            }
            if (first == NULL) {
                first = c;
            } else {
                c->left = prev->left;
                prev->left.node = c;
            }
            prev = c;
        }
        if (head == NULL) {
            head = first;
        } else {
            tail->right.node = first;
            first->right.raw = 0;
        }
        tail = first;
    }
    return head;
}

/* List the OR nodes of a tree; notes whether one of them has a
 * constant 1 operand, which makes the factor true */
static void collect_or_nodes(node_t *n)
{
    if (n == NULL || kind_of(n) == K_VAR || n->type == NODE_CONST) {
        return;
    }
    if (op_of(n->type) == OP_OR) {
        g_or_count++;
        if (left_kind(n) == K_ONE || right_kind(n) == K_ONE) {
            g_factor_is_true = 1;
        }
        cell_push(&g_or_list, n);
    }
    if (has_right_node(n)) {
        collect_or_nodes(n->right.node);
    }
    if (has_left_node(n)) {
        collect_or_nodes(n->left.node);
    }
}

static int operand_is_and(operand_t o)
{
    int type = operand_type(o);
    return type != 0 && type < OP_XOR;
}

/* One term of an OR node, ANDed into `repeat` consecutive products starting
 * with a new one (first factor, repeat < 0) or with the current one. */
typedef enum { TERM_RIGHT, TERM_LEFT, TERM_RIGHT_TREE, TERM_LEFT_TREE, TERM_FALSE } term_t;

static void apply_term(const node_t *or_node, term_t term, int repeat)
{
    int count = repeat < 0 ? 1 : repeat;
    for (int i = 0; i < count; i++) {
        if (repeat < 0) {
            new_product();
        }
        if (g_cur_product == NULL) {
            return;
        }
        switch (term) {
        case TERM_RIGHT: add_operand(or_node, 1); break;
        case TERM_LEFT: add_operand(or_node, 0); break;
        case TERM_RIGHT_TREE: add_and_literals(or_node->right.node); break;
        case TERM_LEFT_TREE: add_and_literals(or_node->left.node); break;
        case TERM_FALSE: mark_product_false(); break;
        }
        if (repeat >= 0) {
            g_cur_product = g_cur_product->right.node;
        }
    }
}

/* The terms the OR nodes on g_or_list contribute (shared by first_factor()
 * and multiply_factor()).  OR nodes with a constant 1 operand never get
 * here.  A 0 term only counts when multiplying; for the first factor it is
 * used when nothing else has produced a product. */
static void apply_or_terms(int repeat)
{
    while (g_or_list != NULL) {
        cell_t *cell = g_or_list;
        const node_t *o = cell->node;
        if (op_of(o->type) == OP_OR) {
            operand_kind_t lk = left_kind(o), rk = right_kind(o);
            int zero_terms = (lk == K_ZERO) + (rk == K_ZERO);
            if (rk == K_VAR || rk == K_NVAR) {
                apply_term(o, TERM_RIGHT, repeat);
            }
            if (lk == K_NODE && operand_is_and(o->left)) {
                apply_term(o, TERM_LEFT_TREE, repeat);
            }
            if (lk == K_VAR || lk == K_NVAR) {
                apply_term(o, TERM_LEFT, repeat);
            }
            if (rk == K_NODE && operand_is_and(o->right)) {
                apply_term(o, TERM_RIGHT_TREE, repeat);
            }
            if (repeat >= 0) {
                for (int i = 0; i < zero_terms; i++) {
                    apply_term(o, TERM_FALSE, repeat);
                }
            } else if (zero_terms != 0 && g_cur_head == NULL &&
                       (lk == K_ZERO || lk == K_NODE) && (rk == K_ZERO || rk == K_NODE)) {
                apply_term(o, TERM_FALSE, repeat);
            }
        }
        g_or_list = cell->next;
        cell_free(cell);
    }
}

/* Turn the first factor of an AND into the product list */
static void first_factor(node_t *factor)
{
    g_factor_is_true = 0;
    g_cur_head = NULL;
    g_cur_product = NULL;
    g_or_list = NULL;
    collect_or_nodes(factor);
    if (g_factor_is_true) {
        cell_list_free(g_or_list);
        return;
    }
    if (g_or_list == NULL) {
        new_product();
        add_and_literals(factor);
    } else {
        apply_or_terms(-1);
    }
    g_products = g_cur_head;
    g_products_tail = g_cur_product;
}

/* Multiply the product list by another factor */
static void multiply_factor(node_t *factor)
{
    g_factor_is_true = 0;
    g_or_count = 0;
    g_or_list = NULL;
    collect_or_nodes(factor);
    if (g_factor_is_true) {
        cell_list_free(g_or_list);
        return;
    }
    if (g_or_list == NULL) {
        g_cur_head = g_products;
        g_cur_product = g_products;
        for (int i = 0; i < g_product_count; i++) {
            add_and_literals(factor);
            g_cur_product = g_cur_product->right.node;
        }
        return;
    }
    /* one copy of the list for every further term of the factor */
    node_t *source = g_products;
    for (int i = 0; i < g_or_count; i++) {
        g_cur_head = copy_products(source);
        node_t *last = g_cur_head;
        while (last->right.node != NULL) {
            last = last->right.node;
        }
        g_products_tail->right.node = g_cur_head;
        g_products_tail = last;
        source = g_cur_head;
    }
    g_product_count *= g_or_count + 1;
    g_cur_product = g_products;
    g_cur_head = g_products;
    apply_or_terms(g_product_count / (g_or_count + 1));
}

/* Every literal of product a also occurs in product b */
static int product_subset(const node_t *a, const node_t *b)
{
    int found = 1;
    expand_progress_tick();
    for (const node_t *la = a->left.node; la != NULL && found; la = la->left.node) {
        found = 0;
        for (const node_t *lb = b->left.node; lb != NULL && !found; lb = lb->left.node) {
            if (la->right.raw == lb->right.raw && la->flags == lb->flags) {
                found = 1;
            }
        }
    }
    return found;
}

/* Drop false products and products covered by another one */
static void absorb_products(void)
{
    for (node_t *p = g_products; p != NULL; p = p->right.node) {
        if (operand_type(p->left) == NODE_CONST) {
            p->flags = 1;
        } else if (p->flags == 0) {
            node_t *q = g_products;
            while (q != NULL) {
                if (q->flags != 0 || q == p) {
                    q = q->right.node;
                } else if (product_subset(p, q)) {
                    q->flags = 1;
                    q = q->right.node;
                } else if (product_subset(q, p)) {
                    p->flags = 1;
                    q = NULL;
                } else {
                    q = q->right.node;
                }
            }
        }
    }
}

/* The literals of a product as a tree of AND nodes */
static node_t *product_to_tree(const node_t *literal)
{
    node_t *tree = NULL;
    int first = 1;
    for (; literal != NULL; literal = literal->left.node) {
        node_t *v = expand_new_node_of(NODE_VAR, literal->flags);
        v->right = literal->right;
        if (first) {
            first = 0;
            tree = v;
        } else {
            node_t *a = expand_new_node_of(OP_AND, 0);
            a->right.node = tree;
            a->left.node = v;
            expand_fold_operands(tree, v, a);
            tree = a;
        }
    }
    return tree;
}

/* The remaining products as a tree of OR nodes.  Products without
 * literals are skipped. */
static node_t *products_to_tree(void)
{
    node_t *tree = NULL;
    for (const node_t *p = g_products; p != NULL; p = p->right.node) {
        if (p->left.node == NULL || p->flags != 0 || p->left.node->type == NODE_CONST) {
            continue;
        }
        node_t *t = product_to_tree(p->left.node);
        if (tree == NULL) {
            tree = t;
        } else {
            node_t *o = expand_new_node_of(OP_OR, 0);
            o->right.node = tree;
            o->left.node = t;
            expand_fold_operands(tree, t, o);
            tree = o;
        }
    }
    if (tree == NULL) {
        tree = expand_new_node_of(NODE_CONST, 0);
    }
    return tree;
}

/* The products of a single variable operand of the AND being multiplied */
static void variable_factor(const node_t *n, int right_side, int first)
{
    if (first) {
        g_cur_head = NULL;
        g_cur_product = NULL;
        new_product();
        add_operand(n, right_side);
        g_products = g_cur_head;
        g_products_tail = g_cur_product;
    } else {
        g_cur_product = g_products;
        for (int i = 0; i < g_product_count; i++) {
            add_operand(n, right_side);
            g_cur_product = g_cur_product->right.node;
        }
    }
}

/* Replace an AND node whose operands contain ORs by the
 * equivalent sum of products */
static void distribute(node_t *n)
{
    int is_false = 0;
    g_products = NULL;
    g_products_tail = NULL;
    g_product_count = 0;
    if (op_of(n->type) == OP_AND) {
        operand_kind_t lk = left_kind(n), rk = right_kind(n);
        if (lk == K_ZERO || rk == K_ZERO) {
            is_false = (lk == K_NODE || rk == K_NODE);
        } else if (lk == K_NODE && rk == K_NODE) {
            first_factor(n->left.node);
            tree_free(n->left.node);
            if (g_products == NULL) {
                first_factor(n->right.node);
            } else {
                multiply_factor(n->right.node);
            }
            tree_free(n->right.node);
        } else if (lk == K_NODE && (rk == K_VAR || rk == K_NVAR)) {
            first_factor(n->left.node);
            tree_free(n->left.node);
            variable_factor(n, 1, g_products == NULL);
        } else if (rk == K_NODE && (lk == K_VAR || lk == K_NVAR)) {
            variable_factor(n, 0, 1);
            if (g_cur_head == NULL) {
                first_factor(n->right.node);
            } else {
                multiply_factor(n->right.node);
            }
            tree_free(n->right.node);
        } else if (lk == K_NODE && rk == K_ONE) {
            first_factor(n->left.node);
            tree_free(n->left.node);
        } else if (rk == K_NODE && lk == K_ONE) {
            first_factor(n->right.node);
            tree_free(n->right.node);
        }
    }
    if (is_false) {
        if (has_right_node(n)) {
            tree_free(n->right.node);
        }
        if (has_left_node(n)) {
            tree_free(n->left.node);
        }
        n->type = NODE_CONST;
        n->flags = 0;
        n->right.raw = 0;
    } else if (g_products == NULL) {
        n->type = NODE_CONST;
        n->flags = 0;
        n->right.raw = 1;
    } else {
        absorb_products();
        node_t *result = products_to_tree();
        tree_free(g_products);
        if (result->type == NODE_VAR || result->type == NODE_CONST) {
            n->type = result->type;
            n->flags = result->flags;
            n->right = result->right;
        } else {
            node_assign(n, result);
        }
        node_free(result);
    }
}

/* List the AND nodes that have a sub-tree operand, parents before
 * their operands, so that the list is worked off from the leaves upwards */
static void collect_and_nodes(node_t *n)
{
    if (n == NULL || kind_of(n) == K_VAR || n->type == NODE_CONST) {
        return;
    }
    if (is_op_with_node(n->type, OP_AND)) {
        cell_push(&g_and_list, n);
    }
    if (has_right_node(n)) {
        collect_and_nodes(n->right.node);
    }
    if (has_left_node(n)) {
        collect_and_nodes(n->left.node);
    }
}

/* The right (right_side != 0) or left operand is a sub-tree with an OR at
 * its top */
static int operand_is_or(const node_t *n, int right_side)
{
    if (right_side ? !has_right_node(n) : !has_left_node(n)) {
        return 0;
    }
    return op_of(operand_type(right_side ? n->right : n->left)) == OP_OR;
}

/* -------------------------------------------------------------------------
 * Expanding one equation
 * ------------------------------------------------------------------------- */

/* An XOR at the top of the equation is kept when the macrocell
 * has an XOR gate (virtual devices: when -kx was given) */
static void mark_kept_xor(node_t *expr)
{
    if (expand_cur_sym->out_fn < 0) {
        return;
    }
    if (is_virtual_device()) {
        if (!expand_keep_xor) {
            return;
        }
    } else if (dev_u8(dev_nodefn(expand_cur_sym->out_fn, NODEFN_XOR)) == 0) {
        return;
    }
    if (expr != NULL && op_of(expr->type) == OP_XOR) {
        expr->flags |= NODE_KEEP_XOR;
    }
}

/* DeMorgan, XOR expansion and multiplication into a sum of
 * products */
void expand_equation(void)
{
    g_root = expand_expr;
    push_negation(expand_expr);
    mark_kept_xor(expand_expr);
    expand_xors(expand_expr);
    collect_and_nodes(expand_expr);
    while (g_and_list != NULL) {
        cell_t *cell = g_and_list;
        node_t *n = cell->node;
        if (is_op_with_node(n->type, OP_AND) &&
            (operand_is_or(n, 1) || operand_is_or(n, 0))) {
            expand_fold_tree(n);
            distribute(n);
        } else {
            g_and_list = cell->next;
            cell_free(cell);
        }
    }
}

/* Release the tree of an equation; the node pool is rebuilt when nodes
 * are left over. */
void expand_tree_release(node_t *expr)
{
    tree_free(expr);
    if (g_live_nodes != 0) {
        node_pool_reset();
    }
}

/* Reset the tree state for a new run of the phase. */
void expand_tree_reset(void)
{
    g_root = NULL;
    g_free_nodes = NULL;
    g_node_blocks = NULL;
    g_live_nodes = 0;
    g_free_cells = NULL;

    g_and_list = NULL;
    g_or_list = NULL;
    g_or_count = 0;
    g_factor_is_true = 0;
    g_products = NULL;
    g_products_tail = NULL;
    g_cur_head = NULL;
    g_cur_product = NULL;
    g_product_count = 0;
}
