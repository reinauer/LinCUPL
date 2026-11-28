/* SPDX-License-Identifier: BSD-2-Clause */
/* Restore held outputs while interleaving independent sequential models. */
#include "platform.h"
#include "sim_seq.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    sim_seq_model_t *model;
    unsigned char *state, *next, *saved;
    size_t size;
} fixture_t;

static unsigned data_pin, pd_pin, oe_pin, clock_pin, out_pin, invert, oe_active;

static void step(fixture_t *f, unsigned data, unsigned pd, unsigned clock,
                 unsigned enabled, int expected)
{
    unsigned char inputs[SIM_TRUTH_PINS] = {0}, outputs[SIM_TRUTH_PINS];
    char error[256];
    inputs[data_pin] = (unsigned char)data;
    inputs[pd_pin] = (unsigned char)pd;
    inputs[oe_pin] = (unsigned char)(enabled ? oe_active : !oe_active);
    inputs[clock_pin] = (unsigned char)clock;
    if (!sim_seq_step(f->model, f->state, inputs, f->next, outputs, error, sizeof(error))) {
        fprintf(stderr, "transition failed: %s\n", error);
        exit(1);
    }
    if (expected >= 0 && outputs[out_pin] != (unsigned char)('0' + (expected ^ invert))) {
        fprintf(stderr, "P%u: expected %d, got %c\n", out_pin, expected ^ invert, outputs[out_pin]);
        exit(1);
    }
    memcpy(f->state, f->next, f->size);
}

int main(int argc, char **argv)
{
    fixture_t f[2] = {0};
    char error[256];
    if (argc != 10) return 2;
    lincupl_init_file_mode();
    data_pin = (unsigned)atoi(argv[3]);
    pd_pin = (unsigned)atoi(argv[4]);
    oe_pin = (unsigned)atoi(argv[5]);
    clock_pin = (unsigned)atoi(argv[6]);
    out_pin = (unsigned)atoi(argv[7]);
    invert = (unsigned)atoi(argv[8]);
    oe_active = (unsigned)atoi(argv[9]);
    for (unsigned i = 0; i < 2; i++) {
        if (!sim_jed_seq_load(argv[1], argv[2], "", &f[i].model, error, sizeof(error))) {
            fprintf(stderr, "load failed: %s\n", error);
            return 1;
        }
        f[i].size = sim_seq_state_size(f[i].model);
        f[i].state = malloc(3 * f[i].size);
        if (!f[i].state) return 1;
        f[i].next = f[i].state + f[i].size;
        f[i].saved = f[i].next + f[i].size;
        sim_seq_initial(f[i].model, f[i].state);
        step(&f[i], !i, 0, 0, 1, -1);
        step(&f[i], !i, 0, 1, 1, !i);
        memcpy(f[i].saved, f[i].state, f[i].size);
    }
    /* Each model must retain its own data and enable despite alternating
     * activation and opposite input levels during power-down. */
    for (unsigned i = 0; i < 2; i++) step(&f[i], i, 1, 0, 0, !i);
    for (unsigned i = 0; i < 2; i++) step(&f[i], i, 1, 1, 0, !i);
    for (unsigned i = 0; i < 2; i++) {
        step(&f[i], i, 0, 0, 1, clock_pin ? !i : (int)i);
        step(&f[i], i, 0, 1, 1, (int)i);
    }
    /* Replay from an earlier state after the live buffers have changed. */
    for (unsigned i = 0; i < 2; i++) {
        memcpy(f[i].state, f[i].saved, f[i].size);
        step(&f[i], i, 1, 0, 0, !i);
    }
    for (unsigned i = 0; i < 2; i++) {
        sim_seq_free(f[i].model);
        free(f[i].state);
    }
    return 0;
}
