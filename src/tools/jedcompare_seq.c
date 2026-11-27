/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, LinCUPL team
 *
 * Explore paired reachable states without matching internal registers.
 */
#include "jedcompare_seq.h"
#include "sim_seq.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEQ_MEMORY_LIMIT ((size_t)512 * 1024 * 1024)
#define NO_PARENT UINT32_MAX

typedef struct {
    uint32_t parent;
    uint32_t input;
} seq_node_t;

typedef struct {
    unsigned char *states;
    seq_node_t *nodes;
    uint32_t *slots;             /* zero or a node index plus one */
    size_t state_size, stride, capacity, count, max_states;
    size_t nslots, byte_limit;
} seq_graph_t;

static unsigned pin_role(unsigned role)
{
    /* A register is an implementation choice behind an observable pin. */
    return role == SIM_PIN_REGISTER ? SIM_PIN_OUTPUT : role;
}

static int valid_pins(const sim_truth_t *pins)
{
    unsigned char inputs[SIM_TRUTH_PINS] = {0}, outputs[SIM_TRUTH_PINS] = {0};
    if (!pins || !pins->npins || pins->npins >= SIM_TRUTH_PINS ||
        pins->ninputs >= SIM_TRUTH_PINS || pins->noutputs >= SIM_TRUTH_PINS)
        return 0;
    for (unsigned i = 0; i < pins->ninputs; i++) {
        unsigned p = pins->inputs[i];
        if (!p || p > pins->npins || inputs[p] || pins->role[p] != SIM_PIN_INPUT)
            return 0;
        inputs[p] = 1;
    }
    for (unsigned i = 0; i < pins->noutputs; i++) {
        unsigned p = pins->outputs[i];
        if (!p || p > pins->npins || outputs[p] || pin_role(pins->role[p]) != SIM_PIN_OUTPUT)
            return 0;
        outputs[p] = 1;
    }
    for (unsigned p = 1; p <= pins->npins; p++) {
        if (pins->role[p] > SIM_PIN_REGISTER ||
            (pin_role(pins->role[p]) == SIM_PIN_OUTPUT && !outputs[p]))
            return 0;
    }
    return 1;
}

static int reserve_states(seq_graph_t *graph)
{
    /* Reserve the bounded arena once. Growing it with realloc could
     * retain old and new arenas together and exceed the buffer budget. */
    if (graph->states) return 0;
    size_t capacity = graph->max_states;
    size_t allowed = graph->byte_limit / graph->stride;
    if (capacity > graph->max_states) capacity = graph->max_states;
    if (capacity > allowed) capacity = allowed;
    if (capacity <= graph->count) return 0;
    unsigned char *states = malloc(capacity * graph->stride);
    if (!states) return 0;
    graph->states = states;
    graph->capacity = capacity;
    return 1;
}

static size_t state_slot(const seq_graph_t *graph, const unsigned char *state, int *found)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < graph->state_size; i++) {
        hash ^= state[i];
        hash *= UINT64_C(1099511628211);
    }
    size_t slot = (size_t)hash & (graph->nslots - 1);
    while (graph->slots[slot]) {
        size_t index = graph->slots[slot] - 1;
        if (!memcmp(graph->states + index * graph->stride, state, graph->state_size)) {
            *found = 1;
            return slot;
        }
        slot = (slot + 1) & (graph->nslots - 1);
    }
    *found = 0;
    return slot;
}

static void add_state(seq_graph_t *graph, size_t slot, const unsigned char *state,
                      uint32_t parent, uint32_t input)
{
    memcpy(graph->states + graph->count * graph->stride, state, graph->state_size);
    graph->nodes[graph->count].parent = parent;
    graph->nodes[graph->count].input = input;
    graph->slots[slot] = (uint32_t)++graph->count;
}

static int counterexample(const seq_graph_t *graph, size_t parent, uint32_t input,
                           const unsigned *pins, unsigned count,
                           unsigned output, int left, int right)
{
    size_t depth = 0;
    for (size_t n = parent; graph->nodes[n].parent != NO_PARENT; n = graph->nodes[n].parent)
        depth++;
    uint32_t *trace = malloc((depth + 1) * sizeof(*trace));
    if (!trace) {
        fprintf(stderr, "jedcompare: inconclusive: cannot allocate the counterexample trace\n");
        return 2;
    }
    trace[depth] = input;
    size_t i = depth;
    for (size_t n = parent; graph->nodes[n].parent != NO_PARENT; n = graph->nodes[n].parent)
        trace[--i] = graph->nodes[n].input;
    printf("Not equivalent: P%u is %c versus %c\n", output, left, right);
    printf("Shortest input trace: %zu steps (step 0 settles power-up inputs)\n", depth + 1);
    for (i = 0; i <= depth; i++) {
        printf("Step %zu:", i);
        for (unsigned j = 0; j < count; j++)
            printf(" P%u=%u", pins[j], (unsigned)((trace[i] >> j) & 1));
        if (!count) printf(" (none)");
        putchar('\n');
    }
    free(trace);
    return 1;
}

int jedcompare_sequential(const char *const paths[2], const char *library,
                          const char *device, unsigned max_inputs,
                          size_t max_states, uint64_t max_transitions)
{
    sim_seq_model_t *models[2] = {NULL, NULL};
    const sim_truth_t *pins[2];
    unsigned inputs[SIM_TRUTH_PINS], ninputs = 0, noutputs = 0;
    unsigned char used[SIM_TRUTH_PINS] = {0};
    unsigned char input[SIM_TRUTH_PINS] = {0}, output[2][SIM_TRUTH_PINS];
    unsigned char *current = NULL, *next = NULL;
    size_t sizes[2], fixed_bytes;
    seq_graph_t graph = {0};
    uint64_t transitions = 0;
    char error[256];
    int result = 2;

    if (!max_states || max_states > JED_SEQ_MAX_STATES || !max_transitions ||
        max_transitions > JED_SEQ_MAX_TRANSITIONS || max_inputs > SIM_TRUTH_MAX_INPUTS) {
        fprintf(stderr, "jedcompare: inconclusive: invalid exploration limits\n");
        return 2;
    }
    for (int i = 0; i < 2; i++) {
        error[0] = '\0';
        if (!sim_jed_seq_load(paths[i], library, device, &models[i], error, sizeof(error))) {
            fprintf(stderr, "jedcompare: inconclusive: %s: %s\n", paths[i], error);
            goto done;
        }
        pins[i] = sim_seq_pins(models[i]);
        sizes[i] = sim_seq_state_size(models[i]);
        if (!valid_pins(pins[i])) {
            fprintf(stderr, "jedcompare: inconclusive: invalid decoded pin interface: %s\n", paths[i]);
            goto done;
        }
        printf("%s: %s, %u used inputs, %u outputs\n", paths[i], pins[i]->device,
               pins[i]->ninputs, pins[i]->noutputs);
    }
    if (pins[0]->npins != pins[1]->npins) {
        printf("Different pin interfaces: %u pins versus %u pins\n", pins[0]->npins, pins[1]->npins);
        result = 1;
        goto done;
    }
    for (unsigned p = 1; p <= pins[0]->npins; p++) {
        if (pin_role(pins[0]->role[p]) != pin_role(pins[1]->role[p])) {
            static const char *const roles[] = {"unused/supply", "input", "output", "register"};
            printf("Different pin interfaces: P%u is %s versus %s\n", p,
                   roles[pins[0]->role[p]], roles[pins[1]->role[p]]);
            result = 1;
            goto done;
        }
        if (pin_role(pins[0]->role[p]) == SIM_PIN_OUTPUT) noutputs++;
    }
    for (int i = 0; i < 2; i++) {
        for (unsigned j = 0; j < pins[i]->ninputs; j++) used[pins[i]->inputs[j]] = 1;
    }
    for (unsigned p = 1; p <= pins[0]->npins; p++) {
        if (used[p]) inputs[ninputs++] = p;
    }
    if (ninputs > max_inputs) {
        fprintf(stderr, "jedcompare: inconclusive: %u combined inputs exceed the limit of %u\n",
                ninputs, max_inputs);
        goto done;
    }
    if (sizes[0] > SIZE_MAX - sizes[1]) goto memory_limit;
    graph.state_size = sizes[0] + sizes[1];
    graph.stride = graph.state_size ? graph.state_size : 1;
    graph.max_states = max_states;
    graph.nslots = 1;
    while (graph.nslots < 2 * max_states) graph.nslots *= 2;
    /* The validated count limits keep these products small on 32-bit
     * hosts too. Reserve space for the longest counterexample as well. */
    fixed_bytes = max_states * sizeof(*graph.nodes) +
                  graph.nslots * sizeof(*graph.slots) + (max_states + 1) * sizeof(uint32_t);
    if (graph.stride > (SEQ_MEMORY_LIMIT - fixed_bytes) / 3) goto memory_limit;
    graph.byte_limit = SEQ_MEMORY_LIMIT - fixed_bytes - 2 * graph.stride;
    graph.nodes = calloc(max_states, sizeof(*graph.nodes));
    graph.slots = calloc(graph.nslots, sizeof(*graph.slots));
    current = calloc(1, graph.stride);
    next = calloc(1, graph.stride);
    if (!graph.nodes || !graph.slots || !current || !next || !reserve_states(&graph)) goto memory_limit;
    sim_seq_initial(models[0], current);
    sim_seq_initial(models[1], current + sizes[0]);
    int found;
    size_t slot = state_slot(&graph, current, &found);
    add_state(&graph, slot, current, NO_PARENT, 0);
    for (size_t head = 0; head < graph.count; head++) {
        /* Keep a stable parent snapshot for the outgoing edge loop. */
        memcpy(current, graph.states + head * graph.stride, graph.state_size);
        for (uint32_t row = 0; row < (UINT32_C(1) << ninputs); row++) {
            if (transitions == max_transitions) {
                fprintf(stderr, "jedcompare: inconclusive: transition limit of %" PRIu64
                        " reached with unexplored transitions (%zu state pairs found)\n",
                        max_transitions, graph.count);
                goto done;
            }
            transitions++;
            for (unsigned j = 0; j < ninputs; j++) input[inputs[j]] = (unsigned char)((row >> j) & 1);
            memset(output, 0, sizeof(output));
            for (int i = 0; i < 2; i++) {
                size_t offset = i ? sizes[0] : 0;
                error[0] = '\0';
                if (!sim_seq_step(models[i], current + offset, input, next + offset,
                                  output[i], error, sizeof(error))) {
                    fprintf(stderr, "jedcompare: inconclusive: transition of %s: %s\n", paths[i], error);
                    goto done;
                }
            }
            for (unsigned p = 1; p <= pins[0]->npins; p++) {
                if (pin_role(pins[0]->role[p]) != SIM_PIN_OUTPUT) continue;
                int a = output[0][p], b = output[1][p];
                if ((a != '0' && a != '1' && a != 'Z') || (b != '0' && b != '1' && b != 'Z')) {
                    fprintf(stderr, "jedcompare: inconclusive: indeterminate output at P%u\n", p);
                    goto done;
                }
                if (a != b) {
                    result = counterexample(&graph, head, row, inputs, ninputs, p, a, b);
                    goto done;
                }
            }
            slot = state_slot(&graph, next, &found);
            if (found) continue;
            if (graph.count == max_states) {
                fprintf(stderr, "jedcompare: inconclusive: state limit of %zu reached with unexplored states\n",
                        max_states);
                goto done;
            }
            if (graph.count == graph.capacity && !reserve_states(&graph)) goto memory_limit;
            add_state(&graph, slot, next, (uint32_t)head, row);
        }
    }
    printf("Equivalent: %zu reachable state pairs, %" PRIu64
           " transitions, %u outputs (including high impedance)\n", graph.count, transitions, noutputs);
    result = 0;
    goto done;
memory_limit:
    fprintf(stderr, "jedcompare: inconclusive: cannot allocate states within the 512 MiB memory limit\n");
done:
    free(graph.states);
    free(graph.nodes);
    free(graph.slots);
    free(current);
    free(next);
    sim_seq_free(models[0]);
    sim_seq_free(models[1]);
    return result;
}
