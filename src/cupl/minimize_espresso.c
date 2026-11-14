/*
 * minimize_espresso - minimization level 4 of the minimize phase.
 *
 * Level 4 runs espresso() of Berkeley Espresso 2.5.1 (third_party/espresso);
 * levels 2 and 3 use the minimizer implemented in minimize_cube.c,
 * minimize_cover.c and minimize_levels.c.  The covers of level 4 can
 * therefore differ from what the algorithm of the other levels would
 * produce.  minimize verifies them against the input cover as it does for
 * level 3.
 *
 * Espresso keeps the description of the cube structure and several work
 * areas in global variables.  They are set up for every call and released
 * afterwards, and the library's symbols are renamed (lincupl_embed.h), so
 * this file is the only place in the program that sees them.
 */
#include <stdint.h>
#include <stdlib.h>

/* free() of the C library; below, free means Espresso's allocator */
static void release(void *block)
{
    free(block);
}

#include "../third_party/espresso/lincupl_embed.h"
#include "../third_party/espresso/espresso/espresso.h"
#include "minimize_espresso.h"

typedef struct {
    const minimize_espresso_problem_t *problem;
    int *input_of;              /* Espresso input -> input of the problem */
    int *output_of;             /* Espresso output part -> output of the problem */
    int num_inputs;             /* inputs and outputs handed to Espresso */
    int num_outputs;
    uint32_t *result;
    int result_count;
    int out_of_memory;          /* the result array could not be allocated */
    long leaked;
} job_t;

static long g_leaked_blocks;

static int element(const uint32_t *cube_words, int e)
{
    return (int)((cube_words[(e >> 5) + 1] >> (e & 31)) & 1u);
}

/* The same for a cube of Espresso (its own macros shift a signed 1). */
static int has_part(pcube c, int e)
{
    return (int)((c[WHICH_WORD(e)] >> WHICH_BIT(e)) & 1u);
}

static void add_part(pcube c, int e)
{
    c[WHICH_WORD(e)] |= 1u << WHICH_BIT(e);
}

static const uint32_t *problem_cube(const uint32_t *cover, int index, int set_words)
{
    return cover + (size_t)index * (size_t)set_words;
}

/* An input matters if some cube restricts it; an output if some cube drives
 * it.  Espresso is given only those: the others are don't-cares in every
 * cube and would only make the cubes wider.  Their order is kept. */
static void select_columns(job_t *job)
{
    const minimize_espresso_problem_t *pr = job->problem;
    job->num_inputs = 0;
    job->num_outputs = 0;
    for (int i = 0; i < pr->num_inputs; i++) {
        int used = 0;
        for (int c = 0; c < pr->on_count + pr->dc_count && !used; c++) {
            const uint32_t *p = c < pr->on_count ? problem_cube(pr->on_set, c, pr->set_words)
                                                 : problem_cube(pr->dc_set, c - pr->on_count, pr->set_words);
            used = !(element(p, 2 * i) && element(p, 2 * i + 1));
        }
        if (used) {
            job->input_of[job->num_inputs++] = i;
        }
    }
    if (job->num_inputs == 0) {
        /* a function of no input at all: Espresso wants one binary variable */
        job->input_of[job->num_inputs++] = 0;
    }
    for (int k = 0; k < pr->num_outputs; k++) {
        int used = 0;
        for (int c = 0; c < pr->on_count + pr->dc_count && !used; c++) {
            const uint32_t *p = c < pr->on_count ? problem_cube(pr->on_set, c, pr->set_words)
                                                 : problem_cube(pr->dc_set, c - pr->on_count, pr->set_words);
            used = element(p, 2 * pr->num_inputs + k);
        }
        if (used) {
            job->output_of[job->num_outputs++] = k;
        }
    }
}

/* A cover of the problem as an Espresso cover. */
static pcover import_cover(const job_t *job, const uint32_t *cover, int count)
{
    const minimize_espresso_problem_t *pr = job->problem;
    pcover A = new_cover(count > 0 ? count : 1);
    pcube c = cube.temp[0];

    for (int n = 0; n < count; n++) {
        const uint32_t *p = problem_cube(cover, n, pr->set_words);
        (void)set_clear(c, cube.size);
        for (int i = 0; i < job->num_inputs; i++) {
            if (element(p, 2 * job->input_of[i])) {
                add_part(c, 2 * i);
            }
            if (element(p, 2 * job->input_of[i] + 1)) {
                add_part(c, 2 * i + 1);
            }
        }
        for (int k = 0; k < job->num_outputs; k++) {
            if (element(p, 2 * pr->num_inputs + job->output_of[k])) {
                add_part(c, cube.first_part[cube.output] + k);
            }
        }
        A = sf_addset(A, c);
    }
    return A;
}

/* The minimized cover in the layout of the problem.  Inputs that Espresso
 * was not given are don't-cares. */
static void export_cover(job_t *job, pcover F)
{
    const minimize_espresso_problem_t *pr = job->problem;
    pcube p, last;
    int n = 0;

    job->result = calloc((size_t)(F->count > 0 ? F->count : 1) * (size_t)pr->set_words, sizeof(uint32_t));
    if (job->result == NULL) {
        job->out_of_memory = 1;
        return;
    }
    foreach_set(F, last, p) {
        uint32_t *r = job->result + (size_t)n * (size_t)pr->set_words;
        for (int e = 0; e < 2 * pr->num_inputs; e++) {
            r[(e >> 5) + 1] |= 1u << (e & 31);
        }
        for (int i = 0; i < job->num_inputs; i++) {
            for (int value = 0; value < 2; value++) {
                if (!has_part(p, 2 * i + value)) {
                    int e = 2 * job->input_of[i] + value;
                    r[(e >> 5) + 1] &= ~(1u << (e & 31));
                }
            }
        }
        for (int k = 0; k < job->num_outputs; k++) {
            if (has_part(p, cube.first_part[cube.output] + k)) {
                int e = 2 * pr->num_inputs + job->output_of[k];
                r[(e >> 5) + 1] |= 1u << (e & 31);
            }
        }
        n++;
    }
    job->result_count = n;
}

/* What the main program of Espresso does for a PLA of type "fd": set up the
 * cube structure, take the off-set as the complement of on-set and
 * don't-care set, minimize, and release everything. */
static void minimize(void *arg)
{
    job_t *job = arg;
    const minimize_espresso_problem_t *pr = job->problem;

    select_columns(job);
    if (job->num_outputs == 0) {
        return;                         /* nothing is driven: the empty cover */
    }

    cube.num_binary_vars = job->num_inputs;
    cube.num_vars = job->num_inputs + 1;
    cube.part_size = ALLOC(int, cube.num_vars);
    cube.part_size[cube.num_vars - 1] = job->num_outputs;
    cube_setup();

    pcover F = import_cover(job, pr->on_set, pr->on_count);
    pcover D = import_cover(job, pr->dc_set, pr->dc_count);
    pcover R = complement(cube2list(F, D));

    F = espresso(F, D, R);
    export_cover(job, F);

    free_cover(F);
    free_cover(D);
    free_cover(R);
    FREE(cube.part_size);
    setdown_cube();
    sf_cleanup();
    sm_cleanup();
    job->leaked = lincupl_esp_live_blocks();
}

int minimize_espresso_minimize(const minimize_espresso_problem_t *problem, uint32_t **result, int *result_count)
{
    job_t job;
    int status;

    memset(&job, 0, sizeof(job));
    job.problem = problem;
    *result = NULL;
    *result_count = 0;
    /* the plain C library: these two outlive the call into Espresso */
    job.input_of = calloc((size_t)problem->num_inputs + 1, sizeof(int));
    job.output_of = calloc((size_t)problem->num_outputs + 1, sizeof(int));
    if (job.input_of == NULL || job.output_of == NULL) {
        status = LINCUPL_ESP_NO_MEMORY;
    } else {
        lincupl_esp_teardown();
        lincupl_esp_set_tick(problem->tick);
        status = lincupl_esp_run(minimize, &job);
        lincupl_esp_set_tick(NULL);
        if (status == LINCUPL_ESP_OK && job.leaked > g_leaked_blocks) {
            g_leaked_blocks = job.leaked;
        }
        lincupl_esp_teardown();
    }
    release(job.input_of);
    release(job.output_of);
    if (status == LINCUPL_ESP_OK && job.out_of_memory) {
        status = LINCUPL_ESP_NO_MEMORY;
    }
    if (status != LINCUPL_ESP_OK) {
        release(job.result);
        return status == LINCUPL_ESP_NO_MEMORY ? MINIMIZE_ESPRESSO_NO_MEMORY : MINIMIZE_ESPRESSO_FAILED;
    }
    *result = job.result;
    *result_count = job.result_count;
    return MINIMIZE_ESPRESSO_OK;
}

long minimize_espresso_leaked_blocks(void)
{
    return g_leaked_blocks;
}
