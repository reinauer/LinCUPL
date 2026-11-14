/*
 * dev_priv.h - declarations shared only between the device modules
 * (dev.c, jed.c, report.c).
 */
#ifndef FIT150X_DEV_PRIV_H
#define FIT150X_DEV_PRIV_H

#include "dev.h"

/* dev.c */

/* Draws the package with the signal names of `slots` (FitSlot)
 * at their pins into f, followed by two empty lines. */
void dev_print_pinout(FILE *f, FitList *slots);

/* dev_pinout.c */

/* The same in the style of the extended mode: a fixed picture of the package. */
void dev_print_pinout_ext(FILE *f, FitList *slots);

/* Sets the mode of a global clock in the device parameter block
 * g_dev_params (word 0, 1, 2: gclk0_mode, gclk1_mode, gclk2_mode); used by
 * jed_twoclock. */
void dev_params_set_clock_mode(int word, int value);

/* jed.c */

/* New list of the slots whose terms are lent to `slot`: the
 * slots of its block whose cascade name is the slot's signal. */
FitList *jed_cascade_slots(FitSlot *slot);

#endif /* FIT150X_DEV_PRIV_H */
