/*
 * place_priv.h - declarations shared by place.c, route.c and alloc.c (the
 * placer, the router and the block partition) that are not part of the
 * public interface place.h: the functions the three files call across
 * each other.
 */
#ifndef FIT150X_PLACE_PRIV_H
#define FIT150X_PLACE_PRIV_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fit_types.h"
#include "base.h"
#include "tt.h"
#include "lib.h"
#include "min.h"
#include "syn.h"
#include "fit.h"
#include "dev.h"
#include "place.h"

/* Number of items of a list that may be NULL. */
#define PLACE_COUNT(l) ((l) != NULL ? (l)->count : 0)

/* Typed access to list items. */
#define PLACE_SLOT(i)      ((FitSlot *)g_fit_slots->items[(i)])
#define PLACE_LIST(l, i)   ((FitList *)(l)->items[(i)])
#define PLACE_ID(l, i)     FIT_PTR2INT((l)->items[(i)])

/* Node names "<letter><macrocell><p|fb>": "H128fb" and its terminator
 * need 7 bytes; 8 are allocated. */
#define PLACE_NODE_NAME_SIZE 8

/* FitListEq over two strings, ignoring case. */
static inline int place_str_ieq(void *a, void *b)
{
    return util_str_ieq((const char *)a, (const char *)b);
}

/* Stops with "fit150x: internal error: <what>" on a null pointer the
 * fitter cannot handle; `what` names the function and the condition. */
void place_trap(const char *what);                                  /* place.c */

/* ---- place.c ---- */
int      place_slot_free_terms(FitSlot *slot);
int      place_block_of_pin(int pin);
FitList *place_uim_free_candidates(const char *node, int block);
int      place_uim_index_eq(void *a, void *b);
char    *place_node_name(FitSlot *slot, const char *suffix);          /* "<block letter><macrocell><suffix>" */

/* ---- route.c ---- */
int      route_mc_for_terms(int n_pt);
void     route_clear_uim(void);
FitList *route_block_pin_names(FitEntity *ent);
int      route_is_preassigned(FitEntity *ent, char *name);
int      route_fct_terms(FitEntity *ent, FitFct *fct);

/* ---- alloc.c ---- */
int      alloc_is_plain_node(FitEntity *ent, FitVariable *var);
FitList *alloc_block_clusters(FitEntity *ent, FitList *names, int block, FitList **failed_ids);
int      alloc_rebalance(FitEntity *ent, FitList *names);

#endif /* FIT150X_PLACE_PRIV_H */
