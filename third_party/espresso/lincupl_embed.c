/*
 * lincupl_embed.c - the Espresso library as one translation unit.
 *
 * See lincupl_embed.h and README.lincupl.md.  The sources under espresso/
 * are included as they are; this file adds the allocator, the replacement
 * for exit()/abort() and the reset of the library's static variables, which
 * are only reachable from inside the same translation unit.
 *
 * This file is part of lincupl, not of Espresso.
 */
#include <setjmp.h>
#include <stdint.h>
#include "lincupl_embed.h"

#include "espresso/cofactor.c"
#include "espresso/cols.c"
#include "espresso/compl.c"
#include "espresso/contain.c"
#include "espresso/cubestr.c"
#include "espresso/cvrm.c"
#include "espresso/cvrmisc.c"
#include "espresso/dominate.c"
#include "espresso/espresso.c"
#include "espresso/essen.c"
#include "espresso/expand.c"
#include "espresso/gasp.c"
#include "espresso/gimpel.c"
#include "espresso/globals.c"
#include "espresso/indep.c"
#include "espresso/irred.c"
#include "espresso/matrix.c"
#include "espresso/mincov.c"
#include "espresso/part.c"
#include "espresso/reduce.c"
#include "espresso/rows.c"
#include "espresso/set.c"
#include "espresso/setc.c"
#include "espresso/sminterf.c"
#include "espresso/solution.c"
#include "espresso/sparse.c"
#include "espresso/unate.c"

/* From here on the C library is meant again. */
#undef malloc
#undef realloc
#undef free
#undef exit
#undef abort

/* Every block carries a header that links it into the list of live blocks. */
typedef struct block_header {
    struct block_header *prev;
    struct block_header *next;
    max_align_t align;
} block_header_t;

#define TICK_INTERVAL 256           /* allocations between two tick calls */

static block_header_t g_blocks = { .prev = &g_blocks, .next = &g_blocks };
static long g_live_blocks;
static jmp_buf g_give_up;
static int g_running;
static void (*g_tick)(void);
static unsigned g_tick_count;

static void give_up(int reason)
{
    if (g_running) {
        longjmp(g_give_up, reason);
    }
    /* not inside lincupl_esp_run(): nothing to return to */
    abort();
}

void *lincupl_esp_malloc(size_t size)
{
    block_header_t *h = malloc(sizeof(*h) + (size != 0 ? size : 1));
    if (h == NULL) {
        give_up(LINCUPL_ESP_NO_MEMORY);
    }
    h->next = g_blocks.next;
    h->prev = &g_blocks;
    g_blocks.next->prev = h;
    g_blocks.next = h;
    g_live_blocks++;
    if (g_tick != NULL && ++g_tick_count % TICK_INTERVAL == 0) {
        g_tick();
    }
    return h + 1;
}

void lincupl_esp_free(void *block)
{
    if (block != NULL) {
        block_header_t *h = (block_header_t *)block - 1;
        h->prev->next = h->next;
        h->next->prev = h->prev;
        g_live_blocks--;
        free(h);
    }
}

void *lincupl_esp_realloc(void *block, size_t size)
{
    if (block == NULL) {
        return lincupl_esp_malloc(size);
    }
    block_header_t *h = (block_header_t *)block - 1;
    block_header_t *prev = h->prev, *next = h->next;
    block_header_t *n = realloc(h, sizeof(*h) + (size != 0 ? size : 1));
    if (n == NULL) {
        give_up(LINCUPL_ESP_NO_MEMORY);
    }
    prev->next = n;
    next->prev = n;
    return n + 1;
}

void lincupl_esp_exit(int status)
{
    (void)status;
    give_up(LINCUPL_ESP_FATAL);
}

void lincupl_esp_abort(void)
{
    give_up(LINCUPL_ESP_ABORT);
}

int lincupl_esp_run(void (*body)(void *arg), void *arg)
{
    int reason = setjmp(g_give_up);
    if (reason == 0) {
        g_running = 1;
        body(arg);
    }
    g_running = 0;
    return reason;
}

void lincupl_esp_set_tick(void (*tick)(void))
{
    g_tick = tick;
    g_tick_count = 0;
}

long lincupl_esp_live_blocks(void)
{
    return g_live_blocks;
}

void lincupl_esp_teardown(void)
{
    while (g_blocks.next != &g_blocks) {
        lincupl_esp_free(g_blocks.next + 1);
    }
    /* the global and static variables of the library */
    memset(&cube, 0, sizeof(cube));         /* globals.c */
    memset(&cdata, 0, sizeof(cdata));
    set_family_garbage = NULL;              /* set.c: released set family headers */
    toggle = TRUE;                          /* reduce.c: alternates the two cube orders */
    Rp_current = 0;                         /* irred.c */
}
