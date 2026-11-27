/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, LinCUPL team
 *
 * Deterministic transitions of a decoded JEDEC design. These operations
 * share simulator internals and must not run concurrently with csim.
 */
#ifndef LINCUPL_SIM_SEQ_H
#define LINCUPL_SIM_SEQ_H

#include "sim_truth.h"

typedef struct sim_seq_model sim_seq_model_t;

int sim_jed_seq_load(const char *path, const char *library, const char *device,
                     sim_seq_model_t **model, char *error, size_t size);
const sim_truth_t *sim_seq_pins(const sim_seq_model_t *model);
size_t sim_seq_state_size(const sim_seq_model_t *model);
void sim_seq_initial(sim_seq_model_t *model, unsigned char *state);
/* State buffers have sim_seq_state_size() bytes. Inputs and outputs are
 * indexed by physical pin, with SIM_TRUTH_PINS entries. Drive every used
 * input with 0 or 1; outputs are ASCII '0', '1' or 'Z'. An indeterminate
 * result or unsupported transition returns zero with an explanation.
 * The first transition settles inputs at power-up; subsequent transitions
 * include clock edges and asynchronous controls, as csim binary vectors do.
 */
int sim_seq_step(sim_seq_model_t *model, const unsigned char *state,
                 const unsigned char *inputs, unsigned char *next,
                 unsigned char *outputs, char *error, size_t size);
/* Storage names identify raw register Q, before output polarity and OE.
 * powerup is ASCII '0', '1' or 'X'. Preload changes storage only, before
 * the first input-settling step; it does not apply controls or clock it.
 */
unsigned sim_seq_storage_count(const sim_seq_model_t *model);
int sim_seq_storage(const sim_seq_model_t *model, unsigned index,
                    char *name, size_t size, unsigned char *powerup);
int sim_seq_preload(sim_seq_model_t *model, unsigned char *state,
                    unsigned index, unsigned value);
void sim_seq_free(sim_seq_model_t *model);

#endif
