/*
 * min_bdd.c - BDD package of the ATF150x fitter and the operations on its
 * BDDs.
 *
 * The package proper: every edge carries a mark, FIT_BDD_DIRECT ('D') or
 * FIT_BDD_INVERTED ('I'); a function is a pair (node, mark).  There is one
 * terminal node; (terminal, 'D') is the constant 1 and (terminal, 'I') the
 * constant 0.  The variable order is the order of entity->inputs, the first
 * input on top.  Nodes come from pools of `block_cells` cells and are kept
 * in a hash table (the "unique table", a list of FitBddBucket); the AND/OR
 * results are remembered in a second hash table (the "computed table") whose
 * slots hold at most five entries.  The package builds the BDDs of all
 * outputs of an entity (bdd_build), collecting unused nodes on the way,
 * and hands the root list, the node blocks and the unique table to the
 * caller.
 *
 * The operations on the result: conversion back to an entity of
 * multiplexer trees (bdd_to_entity), parent lists and levels for every
 * node (bdd_annotate, bdd_nodes_by_level), node duplication
 * (bdd_clone_inverted), structural comparison (bdd_equal).
 *
 * Both hash functions take the addresses of the two child nodes:
 * ((lo >> 1) ^ hi) >> 2 modulo the table size.  Every node gets a
 * stand-in address (FitBddNode.addr: blocks 0x24 bytes per cell, starting
 * at BDD_FIRST_ADDR) and that is hashed, not the real pointer.  The BDDs
 * built do not depend on the hash values: a node is found in whatever slot
 * it hashes to, and a computed-table miss only repeats work whose results
 * still exist.  What does depend on them is the number of apply steps,
 * which is compared with the step limit (5,000,000 from the standard entry
 * points).  The stand-in addresses are deliberate: with real pointers that
 * number would follow the host's heap layout, and a function that comes
 * close to the limit could be given up in one run and finished in another.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "min_priv.h"
#include "tt.h"

#define BDD_CELL_SIZE     0x24u         /* stand-in size of a node */
#define BDD_FIRST_ADDR    0x00a40000u   /* stand-in address of the first node block */
#define BDD_INFO_PER_BLOCK 200
#define BDD_CACHE_SLOT_MAX 5            /* entries per computed-table slot */
#define BDD_CACHE_BLOCKS_MAX 0x1e       /* clear the computed table beyond this many entry blocks */
#define BDD_COLLECT_STEP   0x1e         /* collect after an output that added more nodes than this */

/* computed-table entry */
typedef struct BddCacheEntry {
    int         op;        /* '*' or '+' */
    FitBddNode *a;
    int         a_mark;
    FitBddNode *b;
    int         b_mark;
    FitBddNode *res;
    int         res_mark;
} BddCacheEntry;

/* ---- state of the build ---- */
static int      g_bdd_steps;          /* apply calls so far */
static int      g_bdd_step_limit;
static int      g_bdd_block_cells;    /* cells per pool block */
static FitEntity *g_bdd_entity;
static int      g_bdd_n_in;
static int      g_bdd_n_local;
static int      g_bdd_n_out;
static FitList *g_bdd_free_nodes;     /* nodes released by bdd_collect */
static FitList *g_bdd_var_lists;      /* per input: the counted nodes of that variable */
static int     *g_bdd_perm;           /* perm[i] = i */
static int     *g_bdd_inv;            /* inv[i] = i + 1 */
static int      g_bdd_unique_size;
static int      g_bdd_cache_size;
static FitList *g_bdd_cache_blocks;   /* blocks of BddCacheEntry */
static BddCacheEntry *g_bdd_cache_cur;
static BddCacheEntry *g_bdd_cache_end;
static FitList *g_bdd_node_blocks;    /* blocks of FitBddNode */
static FitBddNode *g_bdd_node_cur;
static FitBddNode *g_bdd_node_end;
static FitList *g_bdd_unique;         /* list of FitBddBucket */
static FitList *g_bdd_cache;          /* list of lists of BddCacheEntry */
static FitList *g_bdd_var_nodes;      /* item i (1-based): the node of input i */
static FitList *g_bdd_local_roots;    /* FitBddRoot per local */
static FitList *g_bdd_out_roots;      /* FitBddRoot per output */
static int      g_bdd_count;          /* nodes reached from the outputs, -1, -2 */
static unsigned g_bdd_next_addr;      /* stand-in address of the next node */

/* ---- state of the operations on BDDs ---- */
static FitBddInfo *g_info_cur;
static int      g_info_left;          /* end of the block: records left in the block */
static FitList *g_info_blocks;
static FitList *g_shared_nodes;       /* nodes turned into locals by bdd_to_entity */

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

/* See min.h.  The lists and blocks a finished build
 * handed out belong to its caller; what is cleared here are the pointers the
 * modules keep between calls. */
void min_reset(void)
{
    g_equiv_verbose = 0;
    g_bdd_steps = 0;
    g_bdd_step_limit = 0;
    g_bdd_block_cells = 0;
    g_bdd_entity = NULL;
    g_bdd_n_in = g_bdd_n_local = g_bdd_n_out = 0;
    g_bdd_free_nodes = NULL;
    g_bdd_var_lists = NULL;
    g_bdd_perm = NULL;
    g_bdd_inv = NULL;
    g_bdd_unique_size = g_bdd_cache_size = 0;
    g_bdd_cache_blocks = NULL;
    g_bdd_cache_cur = g_bdd_cache_end = NULL;
    g_bdd_node_blocks = NULL;
    g_bdd_node_cur = g_bdd_node_end = NULL;
    g_bdd_unique = NULL;
    g_bdd_cache = NULL;
    g_bdd_var_nodes = NULL;
    g_bdd_local_roots = NULL;
    g_bdd_out_roots = NULL;
    g_bdd_count = 0;
    g_bdd_next_addr = 0;
    g_info_cur = NULL;
    g_info_left = 0;
    g_info_blocks = NULL;
    g_shared_nodes = NULL;
    min_esp_reset();
}

void min_free_pterm_cb(void **item)
{
    FitPTerm *p = *item;

    pterm_free(&p);
    *item = NULL;
}

void min_free_ptermio_cb(void **item)
{
    FitPTermIO *p = *item;

    ptermio_free(&p);
    *item = NULL;
}

void min_free_block_cb(void **item)
{
    free(*item);
    *item = NULL;
}

void *min_copy_var_cb(void *item)
{
    return var_copy(item);
}

static void bdd_destroy_list_cb(void **item)   /* list_destroy as FitListFree */
{
    FitList *l = *item;

    list_destroy(&l);
    *item = NULL;
}

static int bdd_flip(int mark)
{
    return mark == FIT_BDD_DIRECT ? FIT_BDD_INVERTED : FIT_BDD_DIRECT;
}

static FitBddNode *bdd_terminal(void)
{
    FitBddBucket *b = g_bdd_unique->items[0];

    return b->nodes->items[0];
}

/* ((lo >> 1) ^ hi) >> 2: the hash of both tables */
static int bdd_hash(const FitBddNode *hi, const FitBddNode *lo, int size)
{
    return (int)(((lo->addr >> 1) ^ hi->addr) >> 2) % size;
}

static int bdd_is_terminal(const FitBddNode *n)
{
    return FIT_BDD_IS_TERMINAL(n);
}

/* a node whose two edges lead to the terminal */
static int bdd_is_single_var(const FitBddNode *n)
{
    if (bdd_is_terminal(n) || n->hi != n->lo || !bdd_is_terminal(n->hi))
        return 0;
    return 1;
}

static void bdd_node_init(FitBddNode *n, int bucket_pos, int var, int hi_mark,
                          FitBddNode *hi, int lo_mark, FitBddNode *lo)
{
    n->bucket_pos = bucket_pos;
    n->var = var;
    n->hi_mark = hi_mark;
    n->hi = hi;
    n->lo_mark = lo_mark;
    n->lo = lo;
    n->state = FIT_BDD_ST_FRESH;
    n->refs = 0;
    n->info = NULL;
}

/* ------------------------------------------------------------------ */
/* pools and tables                                                    */
/* ------------------------------------------------------------------ */

/* a node from the free list, else from the pool (a new block is
 * appended to `blocks` when the current one is used up).  A node taken from
 * the free list is not cleared. */
static FitBddNode *bdd_node_alloc(FitList *blocks)
{
    FitBddNode *n;

    if (g_bdd_free_nodes != NULL && g_bdd_free_nodes->count != 0) {
        n = g_bdd_free_nodes->items[0];
        list_del_insert(g_bdd_free_nodes, 1);
        return n;
    }
    if (g_bdd_node_cur >= g_bdd_node_end) {
        int cells = g_bdd_block_cells;

        g_bdd_node_cur = util_calloc((size_t)cells, sizeof(FitBddNode));
        g_bdd_node_end = g_bdd_node_cur + cells;
        list_add(blocks, g_bdd_node_cur);
        for (int i = 0; i < cells; i++)
            g_bdd_node_cur[i].addr = g_bdd_next_addr + (unsigned)i * BDD_CELL_SIZE;
        g_bdd_next_addr += ((unsigned)cells * BDD_CELL_SIZE + 0x47u) & ~7u;
    }
    n = g_bdd_node_cur;
    g_bdd_node_cur++;
    return n;
}

/* ListDelInsert on a bucket; the node moved into the hole gets
 * its new position. */
static void bdd_bucket_delete(FitList *nodes, int pos)
{
    list_del_insert(nodes, pos);
    if (pos - 1 < nodes->count) {      /* no node moved when the last one was deleted */
        FitBddNode *moved = nodes->items[pos - 1];

        if (moved != NULL)
            moved->bucket_pos = pos - 1;
    }
}

/* the node (var, hi, lo) with the given edge marks.  Returns hi
 * itself when both edges are the same function.  An existing node with both
 * marks opposite is returned with *mark = 'I'. */
static FitBddNode *bdd_find_or_add(int var, FitBddNode *hi, int hi_mark, FitBddNode *lo,
                                   int lo_mark, int *mark)
{
    FitBddBucket *bucket;
    FitList *nodes;
    FitBddNode *n;

    if (hi == lo && hi_mark == lo_mark) {
        *mark = hi_mark;
        return hi;
    }
    bucket = g_bdd_unique->items[bdd_hash(hi, lo, g_bdd_unique_size)];
    nodes = bucket->nodes;
    for (int i = 0; i < min_count(nodes); i++) {
        n = nodes->items[i];
        if (var == n->var && hi == n->hi && lo == n->lo) {
            if (hi_mark == n->hi_mark) {
                if (lo_mark == n->lo_mark) {
                    *mark = FIT_BDD_DIRECT;
                    return n;
                }
            } else if (lo_mark != n->lo_mark) {
                *mark = FIT_BDD_INVERTED;
                return n;
            }
        }
    }
    *mark = FIT_BDD_DIRECT;
    n = bdd_node_alloc(g_bdd_node_blocks);
    bdd_node_init(n, min_count(nodes), var, hi_mark, hi, lo_mark, lo);
    list_add(nodes, n);
    return n;
}

static BddCacheEntry *bdd_cache_entry_alloc(FitList *blocks)
{
    BddCacheEntry *e;

    if (g_bdd_cache_cur >= g_bdd_cache_end) {
        int cells = g_bdd_block_cells;

        g_bdd_cache_cur = util_calloc((size_t)cells, sizeof(BddCacheEntry));
        g_bdd_cache_end = g_bdd_cache_cur + cells;
        list_add(blocks, g_bdd_cache_cur);
    }
    e = g_bdd_cache_cur;
    g_bdd_cache_cur++;
    return e;
}

static int bdd_cache_lookup(int op, int slot, FitBddNode *a, int a_mark, FitBddNode *b,
                            int b_mark, FitBddNode **res, int *res_mark)
{
    FitList *entries = g_bdd_cache->items[slot];

    for (int i = 0; i < min_count(entries); i++) {
        BddCacheEntry *e = entries->items[i];

        if (e->op == op && e->a == a && e->b == b && e->a_mark == a_mark &&
            e->b_mark == b_mark) {
            *res = e->res;
            *res_mark = e->res_mark;
            return 1;
        }
    }
    return 0;
}

/* remember a result of bdd_apply; a full slot overwrites its fifth entry */
static void bdd_cache_store(int op, int slot, FitBddNode *a, int a_mark, FitBddNode *b,
                            int b_mark, FitBddNode *res, int res_mark)
{
    FitList *entries = g_bdd_cache->items[slot];
    BddCacheEntry *e;

    if (entries == NULL || entries->count < BDD_CACHE_SLOT_MAX) {
        e = bdd_cache_entry_alloc(g_bdd_cache_blocks);
        list_add(entries, e);
    } else {
        e = entries->items[BDD_CACHE_SLOT_MAX - 1];
    }
    e->op = op;
    e->a = a;
    e->a_mark = a_mark;
    e->b = b;
    e->b_mark = b_mark;
    e->res = res;
    e->res_mark = res_mark;
}

/* (a, a_mark) op (b, b_mark) with op '*' (AND) or '+' (OR).
 * Returns 0 when the step limit is exceeded. */
static int bdd_apply(int op, FitBddNode *a, int a_mark, FitBddNode *b, int b_mark,
                     FitBddNode **res, int *res_mark)
{
    FitBddNode *term;
    FitBddNode *r_hi, *r_lo;
    int m_hi, m_lo;
    int slot;

    g_bdd_steps++;
    if (g_bdd_step_limit < g_bdd_steps)
        return 0;

    term = bdd_terminal();
    if (a == b) {
        if (a_mark == b_mark) {
            *res = a;
            *res_mark = a_mark;
            return 1;
        }
        if (op == FIT_SEL_AND) {
            *res = term;
            *res_mark = FIT_BDD_INVERTED;
            return 1;
        }
        if (op == FIT_SEL_OR) {
            *res = term;
            *res_mark = FIT_BDD_DIRECT;
            return 1;
        }
    }
    if (a == term) {
        if (op == FIT_SEL_AND) {
            if (a_mark == FIT_BDD_DIRECT) {
                *res = b;
                *res_mark = b_mark;
            } else {
                *res = a;
                *res_mark = a_mark;
            }
            return 1;
        }
        if (op == FIT_SEL_OR) {
            if (a_mark == FIT_BDD_DIRECT) {
                *res = a;
                *res_mark = FIT_BDD_DIRECT;
            } else {
                *res = b;
                *res_mark = b_mark;
            }
            return 1;
        }
    }
    if (b == term) {
        if (op == FIT_SEL_AND) {
            if (b_mark != FIT_BDD_DIRECT) {
                *res = b;
                *res_mark = b_mark;
            } else {
                *res = a;
                *res_mark = a_mark;
            }
            return 1;
        }
        if (op == FIT_SEL_OR) {
            if (b_mark == FIT_BDD_DIRECT) {
                *res = b;
                *res_mark = FIT_BDD_DIRECT;
            } else {
                *res = a;
                *res_mark = a_mark;
            }
            return 1;
        }
    }

    slot = bdd_hash(a, b, g_bdd_cache_size);
    if (a->var < b->var) {
        int hm = a->hi_mark, lm = a->lo_mark;

        if (bdd_cache_lookup(op, slot, a, a_mark, b, b_mark, res, res_mark))
            return 1;
        if (a_mark == FIT_BDD_INVERTED) {
            hm = bdd_flip(hm);
            lm = bdd_flip(lm);
        }
        if (!bdd_apply(op, a->hi, hm, b, b_mark, &r_hi, &m_hi))
            return 0;
        if (!bdd_apply(op, a->lo, lm, b, b_mark, &r_lo, &m_lo))
            return 0;
        *res = bdd_find_or_add(a->var, r_hi, m_hi, r_lo, m_lo, res_mark);
        bdd_cache_store(op, slot, a, a_mark, b, b_mark, *res, *res_mark);
    } else if (a->var != b->var) {
        int hm = b->hi_mark, lm = b->lo_mark;

        if (bdd_cache_lookup(op, slot, b, b_mark, a, a_mark, res, res_mark))
            return 1;
        if (b_mark == FIT_BDD_INVERTED) {
            hm = bdd_flip(hm);
            lm = bdd_flip(lm);
        }
        if (!bdd_apply(op, a, a_mark, b->hi, hm, &r_hi, &m_hi))
            return 0;
        if (!bdd_apply(op, a, a_mark, b->lo, lm, &r_lo, &m_lo))
            return 0;
        *res = bdd_find_or_add(b->var, r_hi, m_hi, r_lo, m_lo, res_mark);
        bdd_cache_store(op, slot, b, b_mark, a, a_mark, *res, *res_mark);
    } else {
        int ahm = a->hi_mark, alm = a->lo_mark;
        int bhm = b->hi_mark, blm = b->lo_mark;

        if (bdd_cache_lookup(op, slot, a, a_mark, b, b_mark, res, res_mark))
            return 1;
        if (a_mark == FIT_BDD_INVERTED) {
            ahm = bdd_flip(ahm);
            alm = bdd_flip(alm);
        }
        if (b_mark == FIT_BDD_INVERTED) {
            bhm = bdd_flip(bhm);
            blm = bdd_flip(blm);
        }
        if (!bdd_apply(op, a->hi, ahm, b->hi, bhm, &r_hi, &m_hi))
            return 0;
        if (!bdd_apply(op, a->lo, alm, b->lo, blm, &r_lo, &m_lo))
            return 0;
        *res = bdd_find_or_add(a->var, r_hi, m_hi, r_lo, m_lo, res_mark);
        bdd_cache_store(op, slot, a, a_mark, b, b_mark, *res, *res_mark);
    }
    return 1;
}

/* the unique table with `size` empty buckets and the terminal
 * node as first node of bucket 0. */
static void bdd_unique_create(FitList **unique, FitList **node_blocks, int size)
{
    FitBddBucket *b;
    FitBddNode *term;
    FitList *nodes;

    *unique = list_create_sized(size);
    for (int i = 0; i < size; i++) {
        b = util_calloc(1, sizeof(*b));
        b->nodes = list_create_sized(10);
        b->kept = 0;
        list_add(*unique, b);
    }
    term = bdd_node_alloc(*node_blocks);
    b = (*unique)->items[0];
    nodes = b->nodes;
    /* the terminal has no children */
    bdd_node_init(term, min_count(nodes), 0, FIT_BDD_DIRECT, NULL, FIT_BDD_DIRECT, NULL);
    term->state = FIT_BDD_ST_COUNTED;
    list_add(nodes, term);
}

/* one node per input: "variable i" = (terminal direct when 1,
 * terminal inverted when 0).  All of them go into the bucket the terminal
 * hashes to with itself. */
static void bdd_var_nodes_create(void)
{
    g_bdd_var_nodes = list_create_sized(g_bdd_n_in + 1);
    list_add(g_bdd_var_nodes, NULL);
    for (int i = 1; i <= g_bdd_n_in; i++) {
        FitBddNode *term = bdd_terminal();
        FitBddBucket *b = g_bdd_unique->items[bdd_hash(term, term, g_bdd_unique_size)];
        FitList *nodes = b->nodes;
        FitBddNode *n = bdd_node_alloc(g_bdd_node_blocks);

        n->bucket_pos = min_count(nodes);
        n->var = i;
        n->hi_mark = FIT_BDD_DIRECT;
        n->hi = bdd_terminal();
        n->lo_mark = FIT_BDD_INVERTED;
        n->lo = bdd_terminal();
        n->state = FIT_BDD_ST_KEEP;
        list_add(g_bdd_var_nodes, n);
        list_add(nodes, n);
    }
}

/* count the nodes below `node` that no output reached before and
 * enter them into the list of their variable. */
static void bdd_count_nodes(FitBddNode *node)
{
    while (node != bdd_terminal()) {
        node->refs++;
        if (node->refs != 1)
            return;
        node->state = FIT_BDD_ST_COUNTED;
        g_bdd_count++;
        list_add(g_bdd_var_lists->items[g_bdd_perm[node->var - 1]], node);
        bdd_count_nodes(node->hi);
        node = node->lo;
    }
}

static void bdd_order_create(int **perm, int **inv, int n)
{
    *perm = util_calloc((size_t)n, sizeof(int));
    *inv = util_calloc((size_t)n, sizeof(int));
    for (int i = 0; i < n; i++) {
        (*perm)[i] = i;
        (*inv)[i] = i + 1;
    }
}

static void bdd_cache_create(void)
{
    g_bdd_cache = list_create_sized(g_bdd_cache_size);
    for (int i = 0; i < g_bdd_cache_size; i++)
        list_add(g_bdd_cache, list_create_sized(BDD_CACHE_SLOT_MAX));
}

static void bdd_cache_pool_init(FitList **blocks)
{
    g_bdd_cache_end = NULL;
    g_bdd_cache_cur = NULL;
    *blocks = list_create_sized(1);
}

static void bdd_node_pool_init(FitList **blocks)
{
    g_bdd_node_end = NULL;
    g_bdd_node_cur = NULL;
    *blocks = list_create_sized(1);
}

/* protect the nodes of a local function from bdd_collect */
static void bdd_mark_keep(FitBddNode *node)
{
    while (node->state != FIT_BDD_ST_COUNTED && node->state != FIT_BDD_ST_KEEP &&
           node != bdd_terminal()) {
        node->state = FIT_BDD_ST_KEEP;
        bdd_mark_keep(node->hi);
        node = node->lo;
    }
}

/* empty the computed table and release its entry blocks */
static void bdd_cache_clear(void)
{
    for (int i = 0; i < min_count(g_bdd_cache_blocks); i++)
        free(g_bdd_cache_blocks->items[i]);
    g_bdd_cache_blocks->count = 0;
    g_bdd_cache_end = NULL;
    g_bdd_cache_cur = NULL;
    for (int i = 0; i < min_count(g_bdd_cache); i++) {
        FitList *entries = g_bdd_cache->items[i];

        entries->count = 0;
    }
}

/* move every node created since the last collection that is
 * neither counted nor kept to the free list. */
static void bdd_collect(void)
{
    bdd_cache_clear();
    for (int i = 0; i < min_count(g_bdd_unique); i++) {
        FitBddBucket *b = g_bdd_unique->items[i];
        int k = b->kept;

        while (k < min_count(b->nodes)) {
            FitBddNode *n = b->nodes->items[k];

            if (n->state != FIT_BDD_ST_COUNTED && n->state != FIT_BDD_ST_KEEP) {
                list_add(g_bdd_free_nodes, n);
                bdd_bucket_delete(b->nodes, k + 1);
            } else {
                k++;
            }
        }
        b->kept = min_count(b->nodes);
    }
}

static void bdd_local_roots_create(void)
{
    g_bdd_local_roots = list_create_sized(g_bdd_n_local);
    for (int i = 0; i < g_bdd_n_local; i++) {
        FitBddRoot *r = util_calloc(1, sizeof(*r));

        r->node = NULL;
        list_add(g_bdd_local_roots, r);
    }
}

static void bdd_out_roots_create(void)
{
    g_bdd_out_roots = list_create_sized(g_bdd_n_out);
    for (int i = 0; i < g_bdd_n_out; i++) {
        FitBddRoot *r = util_calloc(1, sizeof(*r));

        r->node = NULL;
        list_add(g_bdd_out_roots, r);
    }
}

static void bdd_var_lists_create(int n)
{
    g_bdd_var_lists = list_create_sized(n);
    for (int i = 0; i < n; i++)
        list_add(g_bdd_var_lists, list_create_sized(10));
}

static int bdd_from_tree(FitFctTree *tree, FitBddNode **node, int *mark);

/* the BDD of the function defining a local (is_local) or
 * an output, built on first use and kept in its FitBddRoot. */
static int bdd_from_defined_var(FitFctTree *def, int id, int is_local, FitBddNode **node,
                                int *mark)
{
    FitList *vars = is_local ? g_bdd_entity->locals : g_bdd_entity->outputs;
    FitList *roots = is_local ? g_bdd_local_roots : g_bdd_out_roots;
    int n = is_local ? g_bdd_n_local : g_bdd_n_out;
    FitVariable *var = NULL;
    FitBddRoot *root;
    int i;

    for (i = 0; i < n; i++) {
        var = vars->items[i];
        if (var->id == id)
            break;
    }
    if (i >= n)                 /* cannot happen: the id was found in this list */
        return 0;
    root = roots->items[i];
    if (root->node == NULL) {
        FitBddNode *n_new;
        int m_new;

        if (g_bdd_cache_blocks != NULL && g_bdd_cache_blocks->count > BDD_CACHE_BLOCKS_MAX)
            bdd_cache_clear();
        if (!bdd_from_tree(def, &n_new, &m_new))
            return 0;
        root->node = n_new;
        root->mark = m_new;
        root->name[0] = '\0';
        util_str_cat_n(root->name, var->name, FIT_NAME_MAX);
        if (!is_local)
            bdd_count_nodes(root->node);
    }
    if (is_local)
        bdd_mark_keep(root->node);
    *node = root->node;
    *mark = root->mark;
    return 1;
}

/* the BDD of an expression tree.  Returns 0 on failure (step
 * limit, unknown variable, an operator other than + * ! or a constant). */
static int bdd_from_tree(FitFctTree *tree, FitBddNode **node, int *mark)
{
    if (tree == NULL)
        return 0;

    if (tree->sel == FIT_SEL_ID) {
        int id = tree->u.id;
        int pos = var_list_pos_of_id(g_bdd_entity->inputs, id);
        FitFctTree *def;

        if (pos != 0) {
            *node = g_bdd_var_nodes->items[pos];
            *mark = FIT_BDD_DIRECT;
            return 1;
        }
        if (var_list_pos_of_id(g_bdd_entity->locals, id) == 0) {
            if (var_list_pos_of_id(g_bdd_entity->outputs, id) == 0)
                return 0;
            def = entity_fct_tree(g_bdd_entity, id);
            if (def != NULL)
                return bdd_from_defined_var(def, id, 0, node, mark);
        } else {
            def = entity_fct_tree(g_bdd_entity, id);
            if (def != NULL)
                return bdd_from_defined_var(def, id, 1, node, mark);
        }
        fprintf(g_msg, "ERROR : Can't find an output variable Id = %d\n", id);
        return 0;
    }

    if (tree->sel == FIT_SEL_OR || tree->sel == FIT_SEL_AND) {
        int n = min_count(tree->u.children);
        FitBddNode **nodes = util_calloc((size_t)n, sizeof(*nodes));
        int *marks = util_calloc((size_t)n, sizeof(*marks));
        FitBddNode *acc;
        int acc_mark;

        /* operands sorted by top variable, deepest first; among equal ones
         * the later operand comes first */
        for (int i = 0; i < n; i++) {
            FitBddNode *c;
            int cm;
            int j;

            if (!bdd_from_tree(tree->u.children->items[i], &c, &cm)) {
                free(nodes);
                free(marks);
                return 0;
            }
            for (j = i - 1; j >= 0; j--) {
                if (c->var < nodes[j]->var)
                    break;
                marks[j + 1] = marks[j];
                nodes[j + 1] = nodes[j];
            }
            marks[j + 1] = cm;
            nodes[j + 1] = c;
        }
        if (n == 0) {           /* no operands */
            free(nodes);
            free(marks);
            return 0;
        }
        acc = nodes[0];
        acc_mark = marks[0];
        for (int i = 1; i < n; i++) {
            FitBddNode *r;
            int rm;

            if (!bdd_apply(tree->sel, nodes[i], marks[i], acc, acc_mark, &r, &rm)) {
                free(nodes);
                free(marks);
                return 0;
            }
            acc = r;
            acc_mark = rm;
        }
        free(nodes);
        free(marks);
        *node = acc;
        *mark = acc_mark;
        return 1;
    }

    if (tree->sel == FIT_SEL_NOT) {
        int m;

        if (!bdd_from_tree(tree->u.children->items[0], node, &m))
            return 0;
        *mark = bdd_flip(m);
        return 1;
    }
    if (tree->sel == FIT_SEL_CONST) {
        *node = bdd_terminal();
        *mark = tree->u.value != 0 ? FIT_BDD_DIRECT : FIT_BDD_INVERTED;
        return 1;
    }
    fprintf(g_msg, "Bad Operator (operators are +,*,!).\n");
    return 0;
}

static void bdd_build(FitEntity *entity, FitList **roots, FitList **node_blocks,
                      FitList **unique, FitBddNode **terminal, int *n_nodes,
                      int max_nodes, int step_limit, int block_cells)
{
    int failed = 0;
    int complexity;

    g_bdd_entity = entity;
    g_bdd_steps = 0;
    g_bdd_step_limit = step_limit;
    g_bdd_block_cells = block_cells;
    g_bdd_next_addr = BDD_FIRST_ADDR;
    complexity = tt2_entity_complexity(entity);
    g_bdd_n_in = min_count(entity->inputs);
    g_bdd_n_local = min_count(entity->locals);
    g_bdd_n_out = min_count(entity->outputs);
    g_bdd_free_nodes = list_create_sized(block_cells);
    bdd_var_lists_create(g_bdd_n_in);
    bdd_order_create(&g_bdd_perm, &g_bdd_inv, g_bdd_n_in);
    g_bdd_unique_size = complexity / 4 + 1 + g_bdd_n_local * 10;
    g_bdd_cache_size = complexity / 6 + 1 + g_bdd_n_local * 0x14;
    bdd_cache_pool_init(&g_bdd_cache_blocks);
    bdd_node_pool_init(&g_bdd_node_blocks);
    bdd_unique_create(&g_bdd_unique, &g_bdd_node_blocks, g_bdd_unique_size);
    bdd_cache_create();
    bdd_var_nodes_create();
    bdd_out_roots_create();
    g_bdd_count = 1;

    if (g_bdd_n_local != 0) {
        bdd_local_roots_create();
        for (int i = 0; i < g_bdd_n_local; i++) {
            FitBddRoot *root = g_bdd_local_roots->items[i];
            FitVariable *var = entity->locals->items[i];

            if (root->node == NULL) {
                FitBddNode *node;
                int mark;

                if (g_bdd_cache_blocks != NULL &&
                    g_bdd_cache_blocks->count > BDD_CACHE_BLOCKS_MAX)
                    bdd_cache_clear();
                if (!bdd_from_tree(entity_fct_tree(entity, var->id), &node, &mark)) {
                    failed = 1;
                    break;
                }
                root->mark = mark;
                root->node = node;
            }
            bdd_mark_keep(root->node);
        }
        for (int i = 0; i < min_count(g_bdd_unique); i++) {
            FitBddBucket *b = g_bdd_unique->items[i];

            b->kept = min_count(b->nodes);
        }
    }

    if (!failed) {
        for (int i = 0; i < g_bdd_n_out; i++) {
            FitBddRoot *root = g_bdd_out_roots->items[i];
            int before;

            if (root->node == NULL) {
                FitVariable *var = entity->outputs->items[i];
                FitBddNode *node;
                int mark;

                if (!bdd_from_tree(entity_fct_tree(entity, var->id), &node, &mark)) {
                    failed = 1;
                    break;
                }
                root->node = node;
                root->mark = mark;
                root->name[0] = '\0';
                util_str_cat_n(root->name, var->name, FIT_NAME_MAX);
            }
            before = g_bdd_count;
            bdd_count_nodes(root->node);
            if (g_bdd_count - before > BDD_COLLECT_STEP)
                bdd_collect();
            if (max_nodes < g_bdd_count) {
                g_bdd_count = -1;
                break;
            }
        }
    }

    bdd_collect();
    if (failed)
        g_bdd_count = -2;
    list_destroy(&g_bdd_var_nodes);
    if (g_bdd_local_roots != NULL)
        list_free_all(&g_bdd_local_roots, min_free_block_cb);
    *roots = g_bdd_out_roots;
    *node_blocks = g_bdd_node_blocks;
    *unique = g_bdd_unique;
    *terminal = bdd_terminal();
    *n_nodes = g_bdd_count;
}

/* tail shared by the build entry points: release what only the build
 * needed */
static void bdd_build_cleanup(void)
{
    list_destroy(&g_bdd_free_nodes);
    g_bdd_free_nodes = NULL;
    list_free_all(&g_bdd_var_lists, bdd_destroy_list_cb);
    list_destroy(&g_bdd_cache_blocks);
    list_free_all(&g_bdd_cache, bdd_destroy_list_cb);
    free(g_bdd_perm);
    g_bdd_perm = NULL;
    free(g_bdd_inv);
    g_bdd_inv = NULL;
}

void bdd_build_entity_500(FitEntity *entity, FitList **roots, FitList **node_blocks,
                          FitList **unique, FitBddNode **terminal, int *n_nodes,
                          int max_nodes)
{
    bdd_build(entity, roots, node_blocks, unique, terminal, n_nodes, max_nodes, 5000000, 500);
    bdd_build_cleanup();
}

void bdd_build_entity_100(FitEntity *entity, FitList **roots, FitList **node_blocks,
                          FitList **unique, FitBddNode **terminal, int *n_nodes,
                          int max_nodes)
{
    bdd_build(entity, roots, node_blocks, unique, terminal, n_nodes, max_nodes, 5000000, 100);
    bdd_build_cleanup();
}

void bdd_build_entity_ex(FitEntity *entity, FitList **roots, FitList **node_blocks,
                         FitList **unique, FitBddNode **terminal, int *n_nodes,
                         int max_nodes, int step_limit, int block_cells)
{
    bdd_build(entity, roots, node_blocks, unique, terminal, n_nodes, max_nodes, step_limit,
              block_cells);
    bdd_build_cleanup();
}

/* ------------------------------------------------------------------ */
/* operations on BDDs                                                  */
/* ------------------------------------------------------------------ */

void bdd_free_unique_table(FitList *unique)
{
    for (int i = 0; i < min_count(unique); i++) {
        FitBddBucket *b = unique->items[i];

        list_destroy(&b->nodes);
        free(b);
    }
    list_destroy(&unique);
}

void bdd_free_blocks(FitList *blocks)
{
    for (int i = 0; i < min_count(blocks); i++)
        free(blocks->items[i]);
    list_destroy(&blocks);
}

void bdd_free_info_blocks(FitList *info_blocks)
{
    for (int i = 0; i < min_count(info_blocks); i++) {
        FitBddInfo *block = info_blocks->items[i];

        for (int k = 0; k < BDD_INFO_PER_BLOCK; k++)
            list_destroy(&block[k].parents);
        free(block);
    }
    list_destroy(&info_blocks);
}

int bdd_new_local_var(FitEntity *entity)
{
    int id = tt2_next_var_id(entity);
    FitVariable *v = var_new(0);
    char name[36];

    snprintf(name, sizeof(name), "SF%d", id);
    strcpy(v->name, name);
    v->id = id;
    if (entity->locals == NULL)
        entity->locals = list_create_sized(1);
    list_add(entity->locals, v);
    return id;
}

static FitFctTree *bdd_tree_const(int value)
{
    FitFctTree *t = fcttree_new();

    t->sel = FIT_SEL_CONST;
    t->u.value = value;
    return t;
}

static FitFctTree *bdd_tree_id(int id)
{
    FitFctTree *t = fcttree_new();

    t->sel = FIT_SEL_ID;
    t->u.id = id;
    return t;
}

static FitFctTree *bdd_tree_op(int sel, int capacity)
{
    FitFctTree *t = fcttree_new();

    t->sel = sel;
    t->u.children = list_create_sized(capacity);
    return t;
}

static int bdd_tree_is_const(const FitFctTree *t, int value)
{
    return t->sel == FIT_SEL_CONST && t->u.value == value;
}

/* the subtree below an edge; an inverted edge negates it */
static FitFctTree *bdd_edge_to_tree(FitBddNode *child, int edge_mark, FitEntity *entity);

/* expression of the function of `node`:
 *   x * f(hi) + !x * f(lo)
 * with constants folded.  A node with more than one parent becomes a new
 * local variable "SF<id>" of `entity` (unless `force`), defined once.
 * Needs the FitBddInfo records of bdd_annotate. */
static FitFctTree *bdd_node_to_tree(FitBddNode *node, FitEntity *entity, int force)
{
    FitFctTree *or_node, *and_hi, *and_lo, *lit, *lit2, *not_lit, *hi, *lo;
    int id;

    if (bdd_is_terminal(node))
        return bdd_tree_const(1);

    if (node->info->parents != NULL && node->info->parents->count > 1 && !force) {
        FitFct *f;
        FitFctTree *def;

        if (node->state == FIT_BDD_ST_SHARED) {
            int pos = list_find(g_shared_nodes, node, list_ptr_eq);
            FitVariable *v = entity->locals->items[pos - 1];

            return bdd_tree_id(v->id);
        }
        id = bdd_new_local_var(entity);
        list_add(g_shared_nodes, node);
        f = fct_new();
        f->var_id = id;
        def = bdd_node_to_tree(node, entity, 1);
        f->tree[0] = def;
        if (def == NULL)
            return NULL;
        list_add(entity->fcts, f);
        node->state = FIT_BDD_ST_SHARED;
        return bdd_tree_id(id);
    }

    if (min_count(entity->inputs) < node->var) {
        fprintf(g_msg, "Don't know one variable ...\n");
        return NULL;
    }
    id = ((FitVariable *)entity->inputs->items[node->var - 1])->id;
    or_node = bdd_tree_op(FIT_SEL_OR, 2);
    and_hi = bdd_tree_op(FIT_SEL_AND, 2);
    and_lo = bdd_tree_op(FIT_SEL_AND, 2);
    lit = bdd_tree_id(id);
    lit2 = bdd_tree_id(id);
    not_lit = bdd_tree_op(FIT_SEL_NOT, 1);
    list_add(not_lit->u.children, lit2);

    hi = bdd_edge_to_tree(node->hi, node->hi_mark, entity);
    if (hi == NULL)
        return NULL;
    lo = bdd_edge_to_tree(node->lo, node->lo_mark, entity);
    if (lo == NULL)
        return NULL;

    if (bdd_tree_is_const(hi, 1)) {
        fcttree_free(&hi);
        fcttree_free(&and_hi);
        and_hi = lit;
    } else if (bdd_tree_is_const(hi, 0)) {
        fcttree_free(&hi);
        fcttree_free(&lit);
        fcttree_free(&and_hi);
        and_hi = bdd_tree_const(0);
    } else {
        list_add(and_hi->u.children, lit);
        list_add(and_hi->u.children, hi);
    }
    if (bdd_tree_is_const(lo, 1)) {
        fcttree_free(&lo);
        fcttree_free(&and_lo);
        and_lo = not_lit;
    } else if (bdd_tree_is_const(lo, 0)) {
        fcttree_free(&lo);
        fcttree_free(&not_lit);
        fcttree_free(&and_lo);
        and_lo = bdd_tree_const(0);
    } else {
        list_add(and_lo->u.children, not_lit);
        list_add(and_lo->u.children, lo);
    }

    if (bdd_tree_is_const(and_hi, 0)) {
        fcttree_free(&and_hi);
        fcttree_free(&or_node);
        return and_lo;
    }
    if (bdd_tree_is_const(and_lo, 0)) {
        fcttree_free(&and_lo);
        fcttree_free(&or_node);
        return and_hi;
    }
    list_add(or_node->u.children, and_hi);
    list_add(or_node->u.children, and_lo);
    return or_node;
}

static FitFctTree *bdd_edge_to_tree(FitBddNode *child, int edge_mark, FitEntity *entity)
{
    FitFctTree *t = bdd_node_to_tree(child, entity, 0);

    if (t == NULL)
        return NULL;
    if (edge_mark == FIT_BDD_INVERTED) {
        if (bdd_tree_is_const(t, 1)) {
            fcttree_free(&t);
            t = bdd_tree_const(0);
        } else {
            FitFctTree *n = bdd_tree_op(FIT_SEL_NOT, 1);

            list_add(n->u.children, t);
            t = n;
        }
    }
    return t;
}

FitEntity *bdd_to_entity(FitList *roots, FitEntity *src)
{
    FitEntity *e = entity_new(src->name);

    e->n_in = src->n_in;
    e->n_out = src->n_out;
    e->n_local = src->n_local;
    e->inputs = list_map_copy(src->inputs, min_copy_var_cb);
    e->outputs = list_map_copy(src->outputs, min_copy_var_cb);
    e->locals = list_create();
    e->fcts = list_create();
    g_shared_nodes = list_create();
    for (int i = 0; i < min_count(roots); i++) {
        FitBddRoot *r = roots->items[i];
        FitFctTree *t = bdd_node_to_tree(r->node, e, 0);
        FitFct *f;

        if (t == NULL)
            return NULL;        /* the entity and the list stay allocated */
        if (r->mark == FIT_BDD_INVERTED) {
            FitFctTree *n = bdd_tree_op(FIT_SEL_NOT, 1);

            list_add(n->u.children, t);
            t = n;
        }
        f = fct_new();
        f->var_id = ((FitVariable *)src->outputs->items[i])->id;
        f->tree[0] = t;
        list_add(e->fcts, f);
    }
    e->n_local = min_count(e->locals);
    list_destroy(&g_shared_nodes);
    return e;
}

static FitBddInfo *bdd_info_alloc(FitList *blocks)
{
    FitBddInfo *info;

    if (g_info_left <= 0) {
        g_info_cur = util_calloc(BDD_INFO_PER_BLOCK, sizeof(FitBddInfo));
        g_info_left = BDD_INFO_PER_BLOCK;
        list_add(blocks, g_info_cur);
    }
    info = g_info_cur;
    g_info_cur++;
    g_info_left--;
    return info;
}

/* give `node` and everything below it a FitBddInfo; `parent` is
 * the node the walk came from (NULL: an output points here). */
static void bdd_annotate_node(FitBddNode *parent, FitBddNode *node, FitList *blocks)
{
    FitBddInfo *info;

    while (node->state != FIT_BDD_ST_ANNOTATED) {
        info = bdd_info_alloc(blocks);
        info->parents = list_create_sized(1);
        info->w4 = 0;
        info->w0 = 0;
        if (parent == NULL) {
            info->kind = 'O';
        } else {
            info->kind = 'N';
            list_add(info->parents, parent);
        }
        node->info = info;
        node->state = FIT_BDD_ST_ANNOTATED;
        if (bdd_is_terminal(node))
            return;
        bdd_annotate_node(node, node->hi, blocks);
        parent = node;
        node = node->lo;
    }
    info = node->info;
    if (parent == NULL) {
        info->kind = 'O';
        return;
    }
    if (list_find(info->parents, parent, list_ptr_eq) == 0)
        list_add(info->parents, parent);
}

/* 0 for a node without parents, else 1 + the highest level of
 * its parents */
static int bdd_node_level(FitBddNode *node)
{
    FitBddInfo *info = node->info;
    int max = 0;

    if (node->state == FIT_BDD_ST_DEPTH)
        return info->level;
    if (info->parents == NULL || info->parents->count == 0) {
        node->state = FIT_BDD_ST_DEPTH;
        info->level = 0;
        return 0;
    }
    for (int i = 0; i < min_count(node->info->parents); i++) {
        int l = bdd_node_level(node->info->parents->items[i]);

        if (max < l)
            max = l;
    }
    node->state = FIT_BDD_ST_DEPTH;
    node->info->level = max + 1;
    return max + 1;
}

void bdd_annotate(FitList *roots, FitList **info_blocks, FitBddNode *terminal)
{
    g_info_left = 0;
    g_info_blocks = list_create_sized(1);
    for (int i = 0; i < min_count(roots); i++) {
        FitBddRoot *r = roots->items[i];

        if (!bdd_is_single_var(r->node) || r->mark != FIT_BDD_DIRECT) {
            bdd_annotate_node(NULL, r->node, g_info_blocks);
        } else {
            /* an output that is just an input: the node is not marked 'O' */
            FitBddNode *n = r->node;

            if (n->state != FIT_BDD_ST_ANNOTATED) {
                FitBddInfo *info = bdd_info_alloc(g_info_blocks);

                info->parents = list_create_sized(1);
                info->w4 = 0;
                info->w0 = 0;
                info->kind = 'N';
                n->info = info;
                n->state = FIT_BDD_ST_ANNOTATED;
                if (!bdd_is_terminal(n))
                    bdd_annotate_node(n, n->hi, g_info_blocks);
            }
        }
    }
    if (roots == NULL || roots->count == 0)
        bdd_annotate_node(NULL, terminal, g_info_blocks);
    terminal->info->level = bdd_node_level(terminal);
    *info_blocks = g_info_blocks;
}

int bdd_equal(FitBddNode *a, int a_mark, FitBddNode *b, int b_mark)
{
    if (a->var != b->var)
        return 0;
    if (a_mark == b_mark) {
        if (a->var == 0 && b->var == 0)
            return 1;
        if (!bdd_equal(a->hi, a->hi_mark, b->hi, b->hi_mark))
            return 0;
        return bdd_equal(a->lo, a->lo_mark, b->lo, b->lo_mark) != 0;
    }
    if (a->var == 0 && b->var == 0)
        return 0;
    if (a_mark != FIT_BDD_INVERTED) {
        if (!bdd_equal(a->hi, a->hi_mark, b->hi, bdd_flip(b->hi_mark)))
            return 0;
        return bdd_equal(a->lo, a->lo_mark, b->lo, bdd_flip(b->lo_mark)) != 0;
    }
    if (!bdd_equal(a->hi, bdd_flip(a->hi_mark), b->hi, b->hi_mark))
        return 0;
    return bdd_equal(a->lo, bdd_flip(a->lo_mark), b->lo, b->lo_mark) != 0;
}

/* compare the first two roots of two lists; the result holds the
 * name of `first` when the first pair differs and of `second` when the
 * second pair differs.  (After a difference in the first pair the second
 * one is looked at again with index 1; further pairs are compared but never
 * reported.) */
FitList *bdd_compare_roots(FitList *roots_a, FitList *roots_b, FitVariable *first,
                           FitVariable *second)
{
    FitList *diff = list_create();
    int i = 0;

    while (i < min_count(roots_a)) {
        FitBddRoot *rb = roots_b->items[i];
        FitBddRoot *ra = roots_a->items[i];

        if (!bdd_equal(ra->node, ra->mark, rb->node, rb->mark)) {
            if (i == 0) {
                list_add(diff, first->name);
                i = 1;
                continue;
            }
            if (i == 1 && second != NULL)   /* nothing is added for a missing second */
                list_add(diff, second->name);
        }
        i++;
    }
    return diff;
}

static void bdd_level_walk(FitBddNode *node, FitList *by_level)
{
    while (node->state != FIT_BDD_ST_LEVELLED) {
        list_add(by_level->items[node->info->level], node);
        node->state = FIT_BDD_ST_LEVELLED;
        if (bdd_is_terminal(node))
            break;
        bdd_level_walk(node->hi, by_level);
        node = node->lo;
    }
}

FitList *bdd_nodes_by_level(FitList *roots, FitBddNode *terminal)
{
    FitList *by_level = list_create_sized(terminal->info->level);

    for (int i = 0; i <= terminal->info->level; i++)
        list_add(by_level, list_create_sized(0));
    for (int i = 0; i < min_count(roots); i++) {
        FitBddRoot *r = roots->items[i];

        bdd_level_walk(r->node, by_level);
    }
    return by_level;
}

static void bdd_swap_marks(FitBddNode *n)
{
    n->hi_mark = bdd_flip(n->hi_mark == FIT_BDD_INVERTED ? FIT_BDD_INVERTED : FIT_BDD_DIRECT);
    n->lo_mark = bdd_flip(n->lo_mark == FIT_BDD_INVERTED ? FIT_BDD_INVERTED : FIT_BDD_DIRECT);
}

FitBddNode *bdd_clone_inverted(FitBddNode *node, FitList *parents, FitList *top_nodes,
                               FitList *unique, FitList *node_blocks,
                               FitList *info_blocks, FitList *by_level)
{
    FitBddNode *c = bdd_node_alloc(node_blocks);
    FitBddBucket *bucket;
    FitBddInfo *info;
    int max = 0;

    c->var = node->var;
    c->hi = node->hi;
    c->hi_mark = node->hi_mark;
    c->lo = node->lo;
    c->lo_mark = node->lo_mark;
    bucket = unique->items[bdd_hash(c->hi, c->lo, min_count(unique))];
    list_add(bucket->nodes, c);
    info = bdd_info_alloc(info_blocks);
    c->info = info;
    info->w0 = node->info->w0;
    info->w4 = node->info->w4;
    info->level = node->info->level;
    info->parents = parents;
    info->kind = node->info->kind;
    bdd_swap_marks(c);
    list_add(c->hi->info->parents, c);
    if (c->hi != c->lo)
        list_add(c->lo->info->parents, c);
    if (parents == NULL) {
        list_add(top_nodes, c);
        return c;
    }
    for (int i = 0; i < parents->count; i++) {
        FitBddNode *p = parents->items[i];

        if (max < p->info->level)
            max = p->info->level;
    }
    list_add(by_level->items[max + 1], c);
    c->info->level = max + 1;
    return c;
}

void bdd_split_parents(FitBddNode *node, FitList *parents, FitList **direct,
                       FitList **inverted)
{
    *direct = list_create();
    *inverted = list_create();
    for (int i = 0; i < min_count(parents); i++) {
        FitBddNode *p = parents->items[i];

        if (p->hi == node)
            list_add(p->hi_mark == FIT_BDD_DIRECT ? *direct : *inverted, p);
        if (p->lo == node)
            list_add(p->lo_mark == FIT_BDD_DIRECT ? *direct : *inverted, p);
    }
}
