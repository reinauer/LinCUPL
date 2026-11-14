/*
 * fit_priv.h - declarations shared between fit_prep.c, fit_drv.c and
 * fit_drv_main.c (the fit modules).  Not for other packages.
 */
#ifndef FIT150X_FIT_PRIV_H
#define FIT150X_FIT_PRIV_H

#include "fit_types.h"

/* Number of items of a list that may be NULL. */
#define LN(l) ((l) != NULL ? (l)->count : 0)

/* Stub for code the CUPL flow cannot reach: "fit150x: <what> is not
 * supported" on stderr, exit status 3. */
void fit_unsupported(const char *what);

/* For states the fitter cannot handle (a null pointer, a position past
 * the end of a list): "fit150x: internal error: <what>" on stderr, exit
 * status 3.  `what` names the function and the condition. */
void fit_internal_error(const char *what);

/* comparator for list_find: case-insensitive string equality */
int fit_str_ieq_cb(void *a, void *b);

/* ---- fit_prep.c ---- */
void fit_prep_reset(void);
int  fit_prep_var_is_buried(FitEntity *ent, FitVariable *v);
void fit_prep_extract_operands(FitEntity *ent, FitFctTree *tree, FitList *sub, int flag);
void fit_prep_tree_to_sop(FitFctTree **ref, FitEntity *ent);
int  fit_prep_names_to_ids(FitEntity *ent, FitList *names, FitList **ids);
void fit_prep_find_foldbacks(FitEntity *ent);
void fit_prep_collapse_foldbacks(FitEntity *ent);
FitEntity *fit_prep_working_copy(FitEntity *ent);
void fit_prep_share_ctrl_terms(FitEntity *ent);
void fit_prep_detect_xor(FitEntity *ent, int n_listed);
void fit_prep_classify_registers(FitEntity *ent);
int  fit_prep_check_ctrl_conflicts(FitEntity *ent);
void fit_prep_normalise(FitEntity *ent);
void fit_prep_plan(FitEntity *ent, FitList **used, int *n_mc);

/* ---- fit_drv.c ---- */
void fit_drv_reset(void);
int  fit_drv_pass(FitEntity *ent, FitList *used, int *result);
int  fit_drv_verify_preassign(FitEntity *ent, FitList *slots);
void fit_drv_restore_fast_inlatch(FitList *saved);

/* ---- fit_drv_main.c ---- */
FitFct *fit_drv_xor_wide_operand(FitEntity *ent, FitFct *fct);
void fit_drv_inline_ctrl_products(FitEntity *ent);

#endif /* FIT150X_FIT_PRIV_H */
