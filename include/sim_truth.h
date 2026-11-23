/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, LinCUPL team
 *
 * Exhaustive combinational observations from the existing simulator.
 * This is a result table, not a device description. Like sim_run(), the
 * operation uses simulator globals and must not run concurrently with it.
 */
#ifndef LINCUPL_SIM_TRUTH_H
#define LINCUPL_SIM_TRUTH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SIM_TRUTH_PINS 256
#define SIM_TRUTH_MAX_INPUTS 20

enum { SIM_PIN_NONE, SIM_PIN_INPUT, SIM_PIN_OUTPUT, SIM_PIN_REGISTER };

typedef struct {
    char device[28];
    unsigned npins;
    unsigned char role[SIM_TRUTH_PINS];
    unsigned inputs[SIM_TRUTH_PINS], outputs[SIM_TRUTH_PINS];
    unsigned ninputs, noutputs;
    uint32_t nrows;
    /* Row i drives inputs[j] with bit j of i. Each row contains noutputs
     * pin levels, encoded as the ASCII characters '0', '1' or 'Z'. */
    unsigned char *values;
} sim_truth_t;

/* Return 1 with a complete table, or 0 with a reason. No adjacent .pld or
 * .si files are read, and no files are written. Device "" selects the
 * decoder's automatic device detection. Library must name a .dl file.
 * Registers, output feedback, unknown levels and oversized tables fail.
 * On either outcome the caller releases the result with sim_truth_free(). */
int sim_jed_truth(const char *path, const char *library, const char *device,
                  unsigned max_inputs, sim_truth_t *table, char *error, size_t error_size);
void sim_truth_free(sim_truth_t *table);

#ifdef __cplusplus
}
#endif

#endif
