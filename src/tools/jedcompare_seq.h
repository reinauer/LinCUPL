/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef LINCUPL_JEDCOMPARE_SEQ_H
#define LINCUPL_JEDCOMPARE_SEQ_H

#include <stddef.h>
#include <stdint.h>

#define JED_SEQ_DEFAULT_STATES 10000U
#define JED_SEQ_MAX_STATES 1000000U
#define JED_SEQ_DEFAULT_TRANSITIONS UINT64_C(1000000)
#define JED_SEQ_MAX_TRANSITIONS UINT64_C(1000000000)

int jedcompare_sequential(const char *const paths[2], const char *library,
                          const char *device, unsigned max_inputs,
                          size_t max_states, uint64_t max_transitions,
                          const char *const preload[2]);
int jedcompare_list_state(const char *path, const char *library, const char *device);

#endif
