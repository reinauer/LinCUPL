/*
 * lincupl_embed.h - how lincupl embeds the Espresso sources in this directory.
 *
 * The files under espresso/ are compiled unchanged as one translation unit
 * (lincupl_embed.c).  This header is included in front of espresso.h by that
 * unit and by the code that calls the library (src/cuplm_espresso.c):
 *
 *   - lincupl_rename.h gives every external symbol of the library the
 *     prefix lincupl_esp_, so that names like cube, expand or verify cannot
 *     collide with the rest of the program;
 *   - malloc/realloc/free are routed to a small tracking allocator, so that
 *     everything the library allocated can be released after a call even
 *     when it stopped in the middle;
 *   - exit() and abort(), which the library calls on internal errors
 *     (fatal() in cvrmisc.c, fail() in mincov.c, assert), return control to
 *     lincupl_esp_run() instead of ending the process.
 *
 * This file is part of lincupl, not of Espresso.
 */
#ifndef LINCUPL_ESPRESSO_EMBED_H
#define LINCUPL_ESPRESSO_EMBED_H

/* everything port.h includes, before the names below become macros */
#include <stdio.h>
#include <ctype.h>
#include <sys/types.h>
#include <math.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include <stddef.h>

#include "lincupl_rename.h"

enum {
    LINCUPL_ESP_OK = 0,
    LINCUPL_ESP_FATAL = 1,      /* the library called exit() */
    LINCUPL_ESP_NO_MEMORY = 2,  /* an allocation failed */
    LINCUPL_ESP_ABORT = 3       /* the library called abort() */
};

/* Run body(arg).  Returns LINCUPL_ESP_OK, or the reason why the library
 * gave up; in that case body did not return.  Not reentrant: one call at a
 * time (the library keeps its state in global variables). */
int lincupl_esp_run(void (*body)(void *arg), void *arg);

/* Called from the allocator now and then, so that the caller can show that
 * the program is still working.  NULL for none. */
void lincupl_esp_set_tick(void (*tick)(void));

/* Blocks the library currently holds. */
long lincupl_esp_live_blocks(void);

/* Release every block the library still holds and put its static state
 * (cube structure, free lists, the reduce ordering switch) back to what it
 * is when the program starts. */
void lincupl_esp_teardown(void);

void *lincupl_esp_malloc(size_t size);
void *lincupl_esp_realloc(void *block, size_t size);
void lincupl_esp_free(void *block);
void lincupl_esp_exit(int status);
void lincupl_esp_abort(void);

#define malloc(size) lincupl_esp_malloc(size)
#define realloc(block, size) lincupl_esp_realloc(block, size)
#define free(block) lincupl_esp_free(block)
#define exit(status) lincupl_esp_exit(status)
#define abort() lincupl_esp_abort()

#endif
