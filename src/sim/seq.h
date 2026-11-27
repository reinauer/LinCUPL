/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef LINCUPL_SEQ_INTERNAL_H
#define LINCUPL_SEQ_INTERNAL_H
#include "sim_seq.h"

typedef struct sim_seq_ops {
    const sim_truth_t *(*pins)(const void *);
    size_t (*size)(const void *);
    void (*initial)(void *, unsigned char *);
    int (*step)(void *, const unsigned char *, const unsigned char *,
                unsigned char *, unsigned char *, char *, size_t);
    void (*free)(void *);
    unsigned (*storage_count)(const void *);
    int (*storage)(const void *, unsigned, char *, size_t, unsigned char *);
    int (*preload)(void *, unsigned char *, unsigned, unsigned);
} sim_seq_ops_t;

sim_seq_model_t *sim_seq_create(void *context, const sim_seq_ops_t *ops);
sim_seq_model_t *sim_legacy_seq_copy(char *error, size_t size);
#endif
