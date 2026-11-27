/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, LinCUPL team
 */
#include "seq.h"
#include <stdlib.h>

struct sim_seq_model { void *context; const sim_seq_ops_t *ops; };
sim_seq_model_t *sim_seq_create(void *context, const sim_seq_ops_t *ops)
{
    sim_seq_model_t *model = malloc(sizeof(*model));
    if (model) { model->context = context; model->ops = ops; }
    return model;
}
const sim_truth_t *sim_seq_pins(const sim_seq_model_t *m)
{ return m->ops->pins(m->context); }
size_t sim_seq_state_size(const sim_seq_model_t *m)
{ return m->ops->size(m->context); }
void sim_seq_initial(sim_seq_model_t *m, unsigned char *s)
{ m->ops->initial(m->context, s); }
int sim_seq_step(sim_seq_model_t *m, const unsigned char *s,
                 const unsigned char *in, unsigned char *next,
                 unsigned char *out, char *error, size_t size)
{ return m->ops->step(m->context, s, in, next, out, error, size); }
void sim_seq_free(sim_seq_model_t *m)
{ if (m) { m->ops->free(m->context); free(m); } }
unsigned sim_seq_storage_count(const sim_seq_model_t *m)
{ return m->ops->storage_count(m->context); }
int sim_seq_storage(const sim_seq_model_t *m, unsigned i, char *name,
                    size_t size, unsigned char *powerup)
{ return m->ops->storage(m->context, i, name, size, powerup); }
int sim_seq_preload(sim_seq_model_t *m, unsigned char *s,
                    unsigned i, unsigned value)
{ return value <= 1 && m->ops->preload(m->context, s, i, value); }
