/*
 * parse - lowering of the state machine statements SEQUENCE, SEQUENCED,
 * SEQUENCEJK, SEQUENCERS, SEQUENCET and of CONDITION.  The transitions and
 * outputs are collected in a state table and turned into generated APPEND
 * statements that the parser reads back from the token queue.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "parse_int.h"

/* a flip-flop input */
typedef struct {
    int16_t code;                      /* extension code of the flip-flop input */
    const char *name;
} ff_input_t;

/* transition / output kinds of a state entry */
enum {
    ST_PLAIN = 0,
    ST_UNCONDITIONAL = 1,
    ST_CONDITIONAL = 2,
    ST_DEFAULT = 3,
    ST_DEFAULT_AFTER_IF = 4,
    ST_ASYNC_DEFAULT = 5,
    ST_ASYNC_CONDITIONAL = 6
};

typedef struct {
    uint32_t present;
    uint32_t next;
    symbol_t *condition;
    list_t *outputs;
    uint8_t next_kind;
    uint8_t out_kind;
} state_t;

static struct {
    uint32_t present;
    ff_input_t sync[3];
    ff_input_t async[3];
    int capacity;
    int count;
    int sync_count;
    int async_count;
    int sync_index;
    int async_index;
    int next_count;
    int next_index;
    state_t *states;
    int fill;                          /* entry being filled */
    int emit;                          /* entry being written */
    list_t *state_vars;
    int have_present;
} g_seq;
int g_parse_in_sequence;
static int g_if_counter;
static int g_conditional_outs;

void parse_sequence_reset(void)
{
    memset(&g_seq, 0, sizeof(g_seq));
    g_parse_in_sequence = 0;
    g_if_counter = 0;
    g_conditional_outs = 0;
}

/* =========================================================================
 * SEQUENCE / SEQUENCED / SEQUENCEJK / SEQUENCERS / SEQUENCET and CONDITION
 *
 * The transitions and outputs are collected in a state table.  At the
 * closing brace one APPEND statement per state entry and flip-flop input is
 * generated and parsed; ".nsq" tokens between them step through the table
 * and ".esq" ends it.
 * ========================================================================= */

/* a new, empty state table */
static void init_states(void)
{
    g_seq.capacity = STATE_CHUNK;
    g_seq.states = parse_alloc_tracked((size_t)g_seq.capacity * sizeof(state_t), "building sequence");
    g_seq.count = 0;
}

/* make room for STATE_CHUNK more state entries */
static void grow_states(void)
{
    state_t *grown = parse_alloc_tracked((size_t)(g_seq.capacity + STATE_CHUNK) * sizeof(state_t),
                                   "building sequence");
    memcpy(grown, g_seq.states, (size_t)g_seq.capacity * sizeof(state_t));
    g_seq.capacity += STATE_CHUNK;
    memset(&grown[g_seq.count], 0, (size_t)(g_seq.capacity - g_seq.count) * sizeof(state_t));
    g_seq.states = grown;
    g_seq.fill = g_seq.count;
}

static void begin_sequence(int count, int code1, const char *name1, int code2, const char *name2)
{
    g_seq.next_count = count;
    g_seq.async_count = 1;
    g_seq.sync_count = count;
    g_seq.sync[0].code = (int16_t)code1;
    g_seq.sync[0].name = name1;
    g_seq.sync[1].code = (int16_t)code2;
    g_seq.sync[1].name = name2;
    g_seq.async[0].code = 0;
    g_seq.async[0].name = NULL;
    init_states();
    g_seq.fill = 0;
    g_seq.states[0].outputs = NULL;
    g_seq.present = 0;
    g_seq.have_present = 0;
    g_parse_radix = TOK_RADIX_H;
}

/* SEQUENCE uses the flip-flop type of the device */
int act_sequence_begin(token_t *tok)
{
    (void)tok;
    parse_require_device(g_parse_table);
    const device_t *dev = g_parse_table->device;
    int inputs = (int)parse_dev_arch_byte(dev, ARCH_SEQ_INPUT_COUNT);
    int next = (int)parse_dev_arch_byte(dev, ARCH_SEQ_NEXT_COUNT);
    if (inputs == 0 || next == 0) {
        parse_fatal_error(ERR_NO_STATE_MACHINE, parse_device_entry_name(dev));
    }
    int code1 = (int)parse_dev_arch_byte(dev, ARCH_SEQ_INPUT_1);
    int code2 = (int)parse_dev_arch_byte(dev, ARCH_SEQ_INPUT_2);
    begin_sequence(inputs, code1, parse_dev_ext_name(code1, dev), code2, parse_dev_ext_name(code2, dev));
    g_seq.next_count = next;
    g_parse_in_sequence = 1;
    return 0;
}

int act_sequenced_begin(token_t *tok)
{
    (void)tok;
    begin_sequence(1, 1, "d", 0, NULL);
    return 0;
}

int act_sequencejk_begin(token_t *tok)
{
    (void)tok;
    begin_sequence(2, 10, "j", 11, "k");
    return 0;
}

int act_sequencers_begin(token_t *tok)
{
    (void)tok;
    begin_sequence(2, 8, "s", 9, "r");
    return 0;
}

int act_sequencet_begin(token_t *tok)
{
    (void)tok;
    begin_sequence(1, 27, "t", 0, NULL);
    return 0;
}

/* CONDITION { IF expr OUT vars; ... DEFAULT OUT vars; } */
int act_condition_begin(token_t *tok)
{
    (void)tok;
    parse_require_device(g_parse_table);
    g_seq.sync_count = 0;
    g_seq.async_count = 1;
    g_seq.next_count = 0;
    g_seq.async[0].code = 0;
    g_seq.async[0].name = NULL;
    g_seq.sync[0].code = 0;
    g_seq.sync[0].name = NULL;
    g_seq.sync[1].code = 0;
    g_seq.sync[1].name = NULL;
    init_states();
    g_seq.fill = 0;
    g_seq.present = 0;
    g_seq.have_present = 1;
    g_parse_in_sequence = 1;
    g_seq.state_vars = NULL;
    g_parse_radix = TOK_RADIX_H;
    return 0;
}

/* the state variables */
int act_sequence_vars(token_t *tok)
{
    (void)tok;
    g_seq.state_vars = parse_list_clone_header(g_parse_work);
    parse_work_pop();
    return 0;
}

/* is there an entry for this present state already */
static int state_exists(uint32_t present)
{
    for (int i = 0; i < g_seq.count; i++) {
        if (g_seq.states[i].present == present) {
            return 1;
        }
    }
    return 0;
}

static uint32_t single_number(int error)
{
    if (g_parse_numbers.count != 1) {
        parse_error_at(error, "");
    }
    uint32_t value = (g_parse_numbers.head != NULL) ? g_parse_numbers.head->u.num->value : 0;
    /* the list is emptied without freeing its tokens */
    parse_list_init(&g_parse_numbers);
    g_parse_radix = TOK_RADIX_H;
    return value;
}

/* PRESENT n */
int act_present_state(token_t *tok)
{
    (void)tok;
    if (g_parse_numbers.count != 1) {
        parse_error_at(ERR_PRESENT_STATE, "");
    }
    g_seq.present = (g_parse_numbers.head != NULL) ? g_parse_numbers.head->u.num->value : 0;
    if (state_exists(g_seq.present)) {
        parse_error_at(ERR_DUPLICATE_STATE, parse_format_hex(g_seq.present));
    }
    parse_list_init(&g_parse_numbers);
    g_parse_radix = TOK_RADIX_H;
    g_seq.have_present = 1;
    g_conditional_outs = 0;
    return 0;
}

/* NEXT n */
int act_next_state(token_t *tok)
{
    (void)tok;
    g_seq.states[g_seq.fill].next = single_number(ERR_NEXT_STATE);
    return 0;
}

int act_next_state_default(token_t *tok)
{
    return act_next_state(tok);
}

/* IF: the condition is assigned to a new intermediate .in<n> */
int act_if_begin(token_t *tok)
{
    (void)tok;
    parse_work_push();
    parse_list_init(g_parse_work);
    snprintf(g_parse_name_buf, sizeof(g_parse_name_buf), ".in%d", g_if_counter++);
    parse_list_append(parse_token_new_text(g_parse_name_buf, TOK_IDENT), g_parse_work);
    act_assign_targets(NULL);
    g_seq.states[g_seq.fill].condition = g_parse_lhs_syms[0];
    return 0;
}

/* OUT vars */
int act_out_vars(token_t *tok)
{
    (void)tok;
    state_t *state = &g_seq.states[g_seq.fill];
    if (state->outputs == NULL) {
        state->outputs = parse_list_clone_header(g_parse_work);
    } else {
        parse_list_concat(state->outputs, g_parse_work);
    }
    parse_work_pop();
    return 0;
}

/* the ';' of a transition */
int act_transition_end(token_t *tok)
{
    (void)tok;
    if (!g_seq.have_present) {
        parse_error_at(ERR_PRESENT_STATE, "");
        g_seq.states[g_seq.fill].present = 0;
    } else {
        g_seq.states[g_seq.fill].present = g_seq.present;
    }
    g_seq.count++;
    g_seq.fill++;
    if (g_seq.capacity <= g_seq.count) {
        grow_states();
    }
    g_parse_radix = TOK_RADIX_H;
    return 0;
}

/* a state may have one unconditional statement and
 * one default of each kind */
static void check_duplicate(int kind, int outputs)
{
    for (int i = 0; i < g_seq.count; i++) {
        const state_t *state = &g_seq.states[i];
        int other = outputs ? state->out_kind : state->next_kind;
        if (state->present != g_seq.present
            || (other != ST_ASYNC_DEFAULT && other != ST_UNCONDITIONAL)) {
            continue;
        }
        if (other == ST_ASYNC_DEFAULT && kind == ST_ASYNC_DEFAULT) {
            parse_error_at(outputs ? ERR_MULTI_ASYNC_DEFAULT : ERR_MULTI_SYNC_DEFAULT,
                           parse_format_hex(g_seq.present));
        } else {
            parse_error_at(ERR_MULTI_UNCONDITIONAL, parse_format_hex(g_seq.present));
        }
    }
}

static int set_next_kind(token_t *tok, int kind)
{
    parse_token_free(tok);
    g_seq.states[g_seq.fill].next_kind = (uint8_t)kind;
    if (kind == ST_UNCONDITIONAL || kind == ST_ASYNC_DEFAULT) {
        check_duplicate(kind, 0);
    }
    return 0;
}

static int set_out_kind(token_t *tok, int kind)
{
    parse_token_free(tok);
    g_seq.states[g_seq.fill].out_kind = (uint8_t)kind;
    if (kind == ST_UNCONDITIONAL || kind == ST_ASYNC_DEFAULT) {
        check_duplicate(kind, 1);
    }
    return 0;
}

int act_next_unconditional(token_t *tok) { return set_next_kind(tok, ST_UNCONDITIONAL); }
int act_next_conditional(token_t *tok) { return set_next_kind(tok, ST_CONDITIONAL); }
int act_next_default(token_t *tok) { return set_next_kind(tok, ST_DEFAULT); }
int act_next_default_cond(token_t *tok) { return set_next_kind(tok, ST_DEFAULT_AFTER_IF); }
int act_next_async_default(token_t *tok) { return set_next_kind(tok, ST_ASYNC_DEFAULT); }
int act_next_async_cond(token_t *tok) { return set_next_kind(tok, ST_ASYNC_CONDITIONAL); }
int act_out_unconditional(token_t *tok) { return set_out_kind(tok, ST_UNCONDITIONAL); }
int act_out_conditional(token_t *tok) { return set_out_kind(tok, ST_CONDITIONAL); }
int act_out_default(token_t *tok) { return set_out_kind(tok, ST_DEFAULT); }
int act_out_default_cond(token_t *tok) { return set_out_kind(tok, ST_DEFAULT_AFTER_IF); }
int act_out_async_default(token_t *tok) { return set_out_kind(tok, ST_ASYNC_DEFAULT); }
int act_out_async_cond(token_t *tok) { return set_out_kind(tok, ST_ASYNC_CONDITIONAL); }

/* the flip-flop input bits that take the machine from state
 * `present` to `next` */
static uint32_t transition_bits(int code, const char *name, uint32_t present, uint32_t next)
{
    switch (code) {
    case 1:                 /* d */
    case 21:
        return next;
    case 8:                 /* s */
    case 10:                /* j */
    case 22:
        return ~present & next;
    case 9:                 /* r */
    case 11:                /* k */
        return ~next & present;
    case 27:                /* t */
    case 31:
        return present ^ next;
    default:
        parse_error_at(ERR_FLIPFLOP_TYPE, name);
        return next;
    }
}

/* the bits to assign to the output variables.  Variables with an
 * index give the bit of that index, others bits by position. */
static uint32_t output_bits(int code, const char *name, const list_t *outputs)
{
    uint32_t by_position = 0;
    uint32_t position_mask = 0;
    uint32_t by_index = 0;
    uint32_t index_mask = 0;
    for (const token_t *tok = outputs->head; tok != NULL; tok = tok->next) {
        if (tok->type == TOK_EXTENSION) {
            continue;
        }
        by_position <<= 1;
        position_mask <<= 1;
        int index = parse_name_index(parse_token_text(tok));
        if (outputs->count == 1 || index < 0 || index > 31) {
            if (!(tok->flags & TF_NEGATED)) {
                by_position |= 1;
            }
            position_mask |= 1;
        } else {
            if (!(tok->flags & TF_NEGATED)) {
                by_index |= 1u << index;
            }
            index_mask |= 1u << index;
        }
    }
    uint32_t value = by_position | by_index;
    uint32_t mask = position_mask | index_mask;
    switch (code) {
    case 0:
    case 31:
        return mask;
    case 1:
    case 8:
    case 10:
    case 21:
    case 22:
    case 27:
        return value;
    case 9:
    case 11:
        return ~value;
    default:
        parse_error_at(ERR_FLIPFLOP_TYPE, name);
        return value;
    }
}

/* "& !(cond # cond ...)": none of the conditions of
 * the state holds */
static list_t *make_default_condition(uint32_t present, int for_outputs)
{
    list_t *list = parse_list_alloc();
    int count = 0;
    if (!for_outputs && g_conditional_outs == 0) {
        return list;
    }
    parse_append_op(list, TOK_AND);
    parse_append_op(list, TOK_NOT);
    parse_append_op(list, TOK_LPAREN);
    for (int i = 0; i < g_seq.count; i++) {
        const state_t *state = &g_seq.states[i];
        int applies = for_outputs ? state->out_kind == ST_CONDITIONAL
                                  : (state->out_kind == ST_DEFAULT_AFTER_IF || state->next_kind == ST_CONDITIONAL);
        if (state->present == present && applies) {
            if (count > 0) {
                parse_append_op(list, TOK_OR);
            }
            parse_list_append(parse_token_new_text(state->condition != NULL ? state->condition->name : NULL,
                                                   TOK_IDENT), list);
            count++;
        }
    }
    if (count == 0) {
        parse_append_hex_number(0, list);
    }
    parse_append_op(list, TOK_RPAREN);
    return list;
}

/* "& [state vars]:present", or with flag o the one state bit */
static void append_state_match(list_t *list, const state_t *state)
{
    parse_append_op(list, TOK_AND);
    if (!g_parse_one_hot) {
        parse_append_bracket_vars(list, g_seq.state_vars);
        parse_append_op(list, TOK_COLON);
        parse_list_append(parse_token_new_hex(state->present, TOK_NUMBER), list);
        return;
    }
    /* lowest bit of the state number, then the state variable of that
     * bit, counted from the right */
    int bit = -1;
    for (int i = 0; i < 32; i++) {
        if (state->present & (1u << i)) {
            bit = i;
            break;
        }
    }
    const token_t *tok = g_seq.state_vars->head;
    const token_t *found = NULL;
    for (int n = g_seq.state_vars->count - 1; found == NULL && tok != NULL && n >= 0; n--) {
        if (n == bit) {
            found = tok;
        }
        tok = tok->next;
    }
    if (found == NULL) {
        /* state 0 has no bit: this appends nothing and cuts the list */
        found = tok;
    }
    parse_list_append(parse_token_copy(found), list);
}

/* the variables of a list, each with the given extension */
static list_t *vars_with_extension(const char *ext, const list_t *vars)
{
    list_t *list = parse_list_copy(vars);
    if (ext != NULL) {
        parse_list_insert_missing(parse_token_new_text(ext, TOK_EXTENSION), TOK_EXTENSION, list);
    }
    return list;
}

static void append_condition_name(list_t *list, const state_t *state)
{
    parse_append_op(list, TOK_AND);
    parse_list_append(parse_token_new_text(state->condition != NULL ? state->condition->name : NULL, TOK_IDENT),
                      list);
}

/* append [state vars.<input>] = .odel 'h'bits & <state match>
 * [& condition] .fdel ; */
static list_t *make_next_statement(const ff_input_t *input)
{
    const state_t *state = &g_seq.states[g_seq.emit];
    list_t *list = parse_list_alloc();
    parse_append_op(list, TOK_APPEND);
    list_t *vars = vars_with_extension(input->name, g_seq.state_vars);
    parse_append_bracket_vars(list, vars);
    parse_list_destroy(vars);
    parse_append_op(list, TOK_ASSIGN);
    parse_append_op(list, TOK_ODEL);
    parse_append_hex_number(transition_bits(input->code, input->name, state->present, state->next), list);
    append_state_match(list, state);
    if (state->next_kind == ST_CONDITIONAL) {
        append_condition_name(list, state);
        g_conditional_outs++;
    } else if (state->next_kind == ST_ASYNC_DEFAULT) {
        list_t *condition = make_default_condition(state->present, 0);
        parse_list_concat(list, condition);
        parse_list_free(condition);
    }
    parse_append_op(list, TOK_FDEL);
    parse_append_op(list, TOK_SEMI);
    return list;
}

/* append [outputs.<input>] = .odel 'h'bits [& <state match>]
 * [& condition] .fdel ; */
static list_t *make_output_statement(const ff_input_t *input)
{
    const state_t *state = &g_seq.states[g_seq.emit];
    list_t *list = parse_list_alloc();
    parse_append_op(list, TOK_APPEND);
    if (state->outputs == NULL) {
        parse_fatal_error(1039, "");
    }
    list_t *vars = vars_with_extension(input->name, state->outputs);
    parse_list_set_negated(vars, 0);
    parse_append_bracket_vars(list, vars);
    parse_list_destroy(vars);
    parse_append_op(list, TOK_ASSIGN);
    parse_append_op(list, TOK_ODEL);
    parse_append_hex_number(g_seq.state_vars == NULL ? 0xffffffffu
                                               : output_bits(input->code, input->name, state->outputs), list);
    if (g_seq.state_vars != NULL) {
        append_state_match(list, state);
    }
    if (state->out_kind == ST_CONDITIONAL || state->out_kind == ST_DEFAULT_AFTER_IF) {
        append_condition_name(list, state);
    } else if (state->out_kind == ST_ASYNC_DEFAULT || state->out_kind == ST_ASYNC_CONDITIONAL) {
        list_t *condition = make_default_condition(state->present, state->out_kind == ST_ASYNC_DEFAULT);
        parse_list_concat(list, condition);
        parse_list_free(condition);
    }
    parse_append_op(list, TOK_FDEL);
    parse_append_op(list, TOK_SEMI);
    return list;
}

/* which statements the current state entry needs */
static void select_statements(void)
{
    const state_t *state = &g_seq.states[g_seq.emit];
    if (state->next_kind == ST_PLAIN) {
        g_seq.next_index = g_seq.next_count;
    } else if (state->next_kind <= ST_ASYNC_CONDITIONAL) {
        g_seq.next_index = 0;
    } else {
        parse_fatal_error(1037, "");
    }
    switch (state->out_kind) {
    case ST_PLAIN:
        g_seq.sync_index = g_seq.sync_count;
        g_seq.async_index = g_seq.async_count;
        break;
    case ST_UNCONDITIONAL:
    case ST_CONDITIONAL:
    case ST_ASYNC_DEFAULT:
        g_seq.sync_index = g_seq.sync_count;
        g_seq.async_index = 0;
        break;
    case ST_DEFAULT:
    case ST_DEFAULT_AFTER_IF:
    case ST_ASYNC_CONDITIONAL:
        g_seq.async_index = g_seq.async_count;
        g_seq.sync_index = 0;
        break;
    default:
        parse_fatal_error(1038, "");
    }
}

/* queue the next statement of the current state entry, or
 * ".nsq" when it has no more */
int act_seq_emit(token_t *tok)
{
    (void)tok;
    if (g_seq.next_index < g_seq.next_count) {
        parse_queue_prepend(make_next_statement(&g_seq.sync[g_seq.next_index]));
        g_seq.next_index++;
    } else if (g_seq.sync_index < g_seq.sync_count) {
        parse_queue_prepend(make_output_statement(&g_seq.sync[g_seq.sync_index]));
        g_seq.sync_index++;
    } else if (g_seq.async_index < g_seq.async_count) {
        parse_queue_prepend(make_output_statement(&g_seq.async[g_seq.async_index]));
        g_seq.async_index++;
    } else {
        parse_list_push_front(parse_token_new_op(TOK_NSQ), &g_parse_queue);
    }
    return 0;
}

static void emit_state_entry(void)
{
    if (g_seq.capacity <= g_seq.emit) {
        grow_states();
    }
    if (g_seq.emit < g_seq.count) {
        select_statements();
        act_seq_emit(NULL);
    } else {
        parse_list_push_front(parse_token_new_op(TOK_ESQ), &g_parse_queue);
    }
}

/* the closing brace */
int act_sequence_end(token_t *tok)
{
    g_seq.emit = 0;
    parse_token_free(tok);
    for (int i = 0; i < g_seq.count; i++) {
        const state_t *state = &g_seq.states[i];
        if (state->next_kind != ST_PLAIN && !state_exists(state->next)) {
            parse_error_at(ERR_UNKNOWN_NEXT_STATE, parse_format_hex(state->next));
        }
    }
    emit_state_entry();
    return 0;
}

/* ".nsq" */
int act_seq_next_entry(token_t *tok)
{
    parse_token_free(tok);
    g_seq.emit++;
    emit_state_entry();
    return 0;
}

/* ".esq" */
int act_seq_done(token_t *tok)
{
    (void)tok;
    g_seq.capacity = 0;
    g_parse_list_mode = 0;
    return 0;
}

