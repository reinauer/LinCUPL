/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, LinCUPL team
 *
 * Compare physical pin behavior using exhaustive csim truth tables.
 */
#include "platform.h"
#include "sim_truth.h"
#include "locate.h"
#include "jedcompare_seq.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *out)
{
    fprintf(out, "Usage: jedcompare [-u LIBRARY] [-d DEVICE] [--max-inputs N]\n"
                 "                  [--sequential [--max-states N] [--max-transitions N]] LEFT RIGHT\n"
                 "Prove physical pin equivalence of two JEDEC files.\n"
                 "Uses csim's GAL16V8, GAL20V8, GAL22V10 and ATF150x decoders.\n"
                 "The default checks every combinational binary input and output enable (0/1/Z).\n"
                 "--sequential explores all reachable paired states from configured power-up.\n"
                 "Sequential mode currently supports ATF150x and prints shortest input traces.\n"
                 "Physical output feedback and indeterminate levels are unsupported.\n"
                 "--max-inputs limits the combined used inputs (default 16; maximum 20).\n"
                 "--max-states bounds reachable state pairs (default 10000; maximum 1000000).\n"
                 "--max-transitions bounds paired steps (default 1000000; maximum 1000000000).\n"
                 "State exploration buffers are limited to 512 MiB. Reaching a limit is inconclusive.\n"
                 "Exit status: 0 equivalent, 1 different, 2 inconclusive or error.\n");
}

static int parse_limit(const char *text, uint64_t minimum, uint64_t maximum, uint64_t *result)
{
    char *end;
    if (*text < '0' || *text > '9') return 0;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno == ERANGE || *end || value < minimum || value > maximum) return 0;
    *result = (uint64_t)value;
    return 1;
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
    lincupl_init_file_mode();
    const char *paths[2] = {NULL, NULL}, *device = "", *lib_arg = NULL;
    char library[PATH_MAX], device_name[28], error[256];
    sim_truth_t tables[2] = {0};
    unsigned max_inputs = 16;
    size_t max_states = JED_SEQ_DEFAULT_STATES;
    uint64_t max_transitions = JED_SEQ_DEFAULT_TRANSITIONS;
    int sequential = 0, sequential_limits = 0;
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
            uint64_t value;
            if (!parse_limit(argv[++i], 0, SIM_TRUTH_MAX_INPUTS, &value)) goto bad_usage;
            max_inputs = (unsigned)value;
        } else if (options && strcmp(arg, "--sequential") == 0 && !sequential) {
            sequential = 1;
        } else if (options && strcmp(arg, "--max-states") == 0 && i + 1 < argc) {
            uint64_t value;
            if (!parse_limit(argv[++i], 1, JED_SEQ_MAX_STATES, &value)) goto bad_usage;
            max_states = (size_t)value;
            sequential_limits = 1;
        } else if (options && strcmp(arg, "--max-transitions") == 0 && i + 1 < argc) {
            if (!parse_limit(argv[++i], 1, JED_SEQ_MAX_TRANSITIONS, &max_transitions)) goto bad_usage;
            sequential_limits = 1;
        } else if ((options && arg[0] == '-') || npaths == 2) {
            goto bad_usage;
        } else {
            paths[npaths++] = arg;
        }
    }
    if (npaths != 2 || strlen(device) >= sizeof(device_name) || (sequential_limits && !sequential)) goto bad_usage;
    for (size_t i = 0; i <= strlen(device); i++)
        device_name[i] = device[i] >= 'A' && device[i] <= 'Z' ? device[i] - 'A' + 'a' : device[i];
    if (lib_arg) {
        if (strlen(lib_arg) >= sizeof(library)) goto bad_usage;
        lincupl_resolve_explicit_dl(lib_arg, library, sizeof(library));
    } else if (!lincupl_find_default_dl(library, sizeof(library))) {
        /* Built-in decoders need no library. Other devices report the
         * missing file when their decoder tries to open it. */
        snprintf(library, sizeof(library), "lincupl.dl");
    }
    if (sequential) {
        result = jedcompare_sequential(paths, library, device_name, max_inputs, max_states, max_transitions);
        goto done;
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
