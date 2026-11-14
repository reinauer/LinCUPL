/*
 * lib_priv.h - declarations shared by the sources of the lib modules (lib_techmap.c,
 * lib_load.c, lib_parse.c, lib_bin.c).  Not for other packages.
 */
#ifndef FIT150X_LIB_PRIV_H
#define FIT150X_LIB_PRIV_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fit_types.h"
#include "base.h"
#include "tt.h"
#include "syn.h"
#include "lib.h"

/* ------------------------------------------------------------------ */
/* The fields of the netlist cells by their role in the library stage.  */
/* In a FitInstance the library stage keeps the data input in q and the */
/* Q / QN outputs in d / d2, the reverse of the names in fit_types.h,   */
/* and the J / K (S / R) inputs in q2 / q3; all uses in the lib modules */
/* go through these macros.                                             */
/* ------------------------------------------------------------------ */

/* FitInstance */
#define INS_NAME(i)     ((i)->inst_name)
#define INS_CLK(i)      ((i)->clk)       /* clock / latch enable signal */
#define INS_CLK_KIND(i) ((i)->clk_kind)  /* enum FitLibClock */
#define INS_AR(i)       ((i)->ar)
#define INS_AR_POL(i)   ((i)->ar_pol)
#define INS_AP(i)       ((i)->ap)
#define INS_AP_POL(i)   ((i)->ap_pol)
#define INS_CE(i)       ((i)->ce)
#define INS_CE_POL(i)   ((i)->ce_pol)
#define INS_OE(i)       ((i)->oe)
#define INS_OE_POL(i)   ((i)->oe_pol)
#define INS_D(i)        ((i)->q)         /* data input (D, T; input of a TH gate) */
#define INS_J(i)        ((i)->q2)        /* J / S */
#define INS_J_POL(i)    ((i)->q2_pol)
#define INS_K(i)        ((i)->q3)        /* K / R */
#define INS_K_POL(i)    ((i)->q3_pol)
#define INS_MUX_A(i)    ((i)->mux_a)
#define INS_MUX_B(i)    ((i)->mux_b)
#define INS_MUX_SEL(i)  ((i)->mux_sel)
#define INS_Q(i)        ((i)->d)         /* Q output (output of a TH gate) */
#define INS_QN(i)       ((i)->d2)        /* QN output */
#define INS_Z(i)        ((i)->pin)       /* tri-state output */

/* FitCell13 (pad buffer) */
#define BUF_NAME(c)     ((c)->inst_name)
#define BUF_TYPE(c)     ((c)->buf_type)  /* enum FitLibBufType */
#define BUF_IN(c)       ((c)->in)
#define BUF_OUT(c)      ((c)->out)
#define BUF_PAD(c)      ((c)->pad)
#define BUF_OE(c)       ((c)->oe)
#define BUF_OE_POL(c)   ((c)->oe_pol)

#define INS_MODEL(i)    ((i)->name)      /* name of the gate (model) the cell is an instance of */
#define BUF_MODEL(c)    ((c)->model)     /* the same for a buffer */

/* FitUnknownGate / FitUnknownPin */
#define UG_MODEL(g)     ((g)->model)     /* model (gate) name */
#define UG_INST(g)      ((g)->inst_name)
#define UPIN_DIR(p)     ((p)->dir)       /* 1 input, 0 output */
#define UPIN_NAME(p)    ((p)->name)
#define UPIN_SIGNAL(p)  ((p)->signal)    /* connected signal */

/* Number of items of a list that may be NULL. */
#define LCOUNT(l) ((l) != NULL ? (l)->count : 0)

/* Test aid.  With -DLIB_TRACE the functions that change the entity during
 * the library stage write one line each to the message stream, so that two
 * runs can be compared step by step; techmap_message then prints its
 * messages too.  Off in normal builds. */
#ifdef LIB_TRACE
#define LIB_TRACE_F(...) fprintf(g_msg, __VA_ARGS__)
#else
#define LIB_TRACE_F(...) ((void)0)
#endif
#define LIB_TRACE_S(s) ((s) != NULL ? (const char *)(s) : "(null)")

/* Called where a pointer the code relies on is NULL: ends the job through
 * the "INTERNAL ERROR" handler.  `what` says what is wrong; it is not
 * printed. */
void lib_internal_error(const char *what);

/* ------------------------------------------------------------------ */
/* lib_techmap.c                                                       */
/* ------------------------------------------------------------------ */

extern int   g_lib_strict_async;
extern char *g_lib_latch_mode;

int  techmap_adapt_netlist(FitEntity *ent, FitLibHandle *lib, FitList *unmapped);
void techmap_message(const char *fmt, const char *a, const char *b, int level);
void techmap_reset(void);

/* ------------------------------------------------------------------ */
/* lib_load.c                                                          */
/* ------------------------------------------------------------------ */

int  lib_str_ieq(void *a, void *b);    /* FitListEq around util_str_ieq */
int  lib_id_has_alias(int id, FitEntity *ent);
FitVariable *lib_input_to_output(FitEntity *ent, char *name);
int  lib_pin_available(char *want, char *have);
int  lib_ctrl_same(char *a, int a_pol, char *b, int b_pol);
int  lib_same_presence(char *a, char *b);
int  lib_inputs_match(FitInstance *ins, FitLibSeq *gate);
int  lib_outputs_available(FitInstance *ins, FitLibSeq *gate);
int  lib_controls_same(FitInstance *ins, FitLibSeq *gate);
int  lib_controls_available(FitInstance *ins, FitLibSeq *gate);
FitLibKindGroup *lib_kind_group_find(FitList *groups, int kind);
FitLibKindGroup *lib_kind_group_get(FitList *groups, int kind);
FitList *lib_group_by_kind(FitList *gates);
void lib_kind_group_free(void **group_ref);
void lib_load_reset(void);

/* ------------------------------------------------------------------ */
/* lib_parse.c                                                         */
/* ------------------------------------------------------------------ */

extern float g_lib_default_fanout;

void lib_outpin_free(void **outpin_ref);
void lib_parse_reset(void);

/* ------------------------------------------------------------------ */
/* lib_bin.c (binary library)                                          */
/* ------------------------------------------------------------------ */

int  libbin_is_binary(FILE *f);
int  libbin_read_library(FILE *f, FitList **gates, FitLibHeader **header);

#endif /* FIT150X_LIB_PRIV_H */
