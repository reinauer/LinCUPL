/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, LinCUPL team
 *
 * Compare physical pin behavior using exhaustive csim truth tables.
 */
#include "sim_truth.h"
#include "locate.h"

#include <stdlib.h>
#include <string.h>

static void usage(FILE *out)
{
    fprintf(out, "Usage: jedcompare [-u LIBRARY] [-d DEVICE] [--max-inputs N] LEFT RIGHT\n"
                 "Prove combinational pin equivalence of two JEDEC files.\n"
                 "Uses csim's GAL16V8, GAL20V8 and GAL22V10 decoder.\n"
                 "Registers, output feedback and indeterminate levels are unsupported.\n"
                 "Checks every binary input combination and output enable (0/1/Z).\n"
                 "--max-inputs limits the combined used inputs (default 16; maximum 20).\n"
                 "Exit status: 0 equivalent, 1 different, 2 error or unsupported.\n");
}

static uint32_t project(uint32_t row, const unsigned *pins, unsigned count, const sim_truth_t *table)
{
    uint32_t index = 0;
    for (unsigned i = 0; i < table->ninputs; i++) {
        for (unsigned j = 0; j < count; j++) {
            if (pins[j] == table->inputs[i] && (row & (UINT32_C(1) << j)))
                index |= UINT32_C(1) << i;
        }
    }
    return index;
}

static int compare(const sim_truth_t *a, const sim_truth_t *b, unsigned max_inputs)
{
    unsigned pins[SIM_TRUTH_PINS], count = 0;
    unsigned char used[SIM_TRUTH_PINS] = {0};
    if (a->npins != b->npins) {
        printf("Different pin interfaces: %u pins versus %u pins\n", a->npins, b->npins);
        return 1;
    }
    for (unsigned p = 1; p <= a->npins; p++) {
        if (a->role[p] != b->role[p]) {
            static const char *const roles[] = {"unused/supply", "input", "output", "register"};
            printf("Different pin interfaces: P%u is %s versus %s\n", p, roles[a->role[p]], roles[b->role[p]]);
            return 1;
        }
    }
    for (unsigned i = 0; i < a->ninputs; i++) used[a->inputs[i]] = 1;
    for (unsigned i = 0; i < b->ninputs; i++) used[b->inputs[i]] = 1;
    for (unsigned p = 1; p <= a->npins; p++) {
        if (used[p]) pins[count++] = p;
    }
    if (count > max_inputs) {
        fprintf(stderr, "jedcompare: inconclusive: %u combined inputs exceed the limit of %u\n", count, max_inputs);
        return 2;
    }
    for (uint32_t row = 0; row < (UINT32_C(1) << count); row++) {
        uint32_t ai = project(row, pins, count, a), bi = project(row, pins, count, b);
        for (unsigned o = 0; o < a->noutputs; o++) {
            int av = a->values[(size_t)ai * a->noutputs + o];
            int bv = b->values[(size_t)bi * b->noutputs + o];
            if (av != bv) {
                printf("Not equivalent: P%u is %c versus %c\nInputs:", a->outputs[o], av, bv);
                for (unsigned j = 0; j < count; j++)
                    printf(" P%u=%u", pins[j], (unsigned)((row >> j) & 1));
                if (!count) printf(" (none)");
                putchar('\n');
                return 1;
            }
        }
    }
    printf("Equivalent: %u combinations, %u outputs (including high impedance)\n",
           UINT32_C(1) << count, a->noutputs);
    return 0;
}

int main(int argc, char **argv)
{
    const char *paths[2] = {NULL, NULL}, *device = "", *lib_arg = NULL;
    char library[PATH_MAX], device_name[28], error[256];
    sim_truth_t tables[2] = {0};
    unsigned max_inputs = 16;
    int npaths = 0, options = 1, result = 2;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (options && strcmp(arg, "--") == 0) {
            options = 0;
        } else if (options && strcmp(arg, "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (options && strcmp(arg, "-u") == 0 && i + 1 < argc && !lib_arg) {
            lib_arg = argv[++i];
        } else if (options && strcmp(arg, "-d") == 0 && i + 1 < argc && !*device) {
            device = argv[++i];
        } else if (options && strcmp(arg, "--max-inputs") == 0 && i + 1 < argc) {
            const char *n = argv[++i];
            char *end;
            unsigned long value;
            if (*n < '0' || *n > '9') goto bad_usage;
            value = strtoul(n, &end, 10);
            if (*end || value > SIM_TRUTH_MAX_INPUTS) goto bad_usage;
            max_inputs = (unsigned)value;
        } else if ((options && arg[0] == '-') || npaths == 2) {
            goto bad_usage;
        } else {
            paths[npaths++] = arg;
        }
    }
    if (npaths != 2 || strlen(device) >= sizeof(device_name)) goto bad_usage;
    for (size_t i = 0; i <= strlen(device); i++)
        device_name[i] = device[i] >= 'A' && device[i] <= 'Z' ? device[i] - 'A' + 'a' : device[i];
    if (lib_arg) {
        if (strlen(lib_arg) >= sizeof(library)) goto bad_usage;
        lincupl_resolve_explicit_dl(lib_arg, library, sizeof(library));
    } else if (!lincupl_find_default_dl(library, sizeof(library))) {
        fprintf(stderr, "jedcompare: no device library; use -u LIBRARY or run wincupl-extract-data\n");
        return 2;
    }
    for (int i = 0; i < 2; i++) {
        if (!sim_jed_truth(paths[i], library, device_name, max_inputs, &tables[i], error, sizeof(error))) {
            fprintf(stderr, "jedcompare: inconclusive: %s: %s\n", paths[i], error);
            goto done;
        }
        printf("%s: %s, %u used inputs, %u outputs\n", paths[i], tables[i].device,
               tables[i].ninputs, tables[i].noutputs);
    }
    result = compare(&tables[0], &tables[1], max_inputs);
done:
    sim_truth_free(&tables[0]);
    sim_truth_free(&tables[1]);
    if (fflush(stdout) == EOF) result = 2;
    return result;
bad_usage:
    usage(stderr);
    return 2;
}
