/*
 * parse - lowering of function calls, range and match operations and
 * TABLE statements.  Their actions do not build equations: they generate
 * ordinary CUPL statements as token lists and push them onto the front of
 * the token queue, from where the parser reads them back before it goes
 * on with the file.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "parse_int.h"

/* FUNCTION */
static int g_call_has_args;
static list_t g_func_params;
static int g_call_counter;

/* TABLE */
static struct {
    list_t *statement;
    list_t *inputs;
    list_t *outputs;
    list_t *in_values;
    list_t *out_values;
} g_xtable;

/* range / match */
typedef struct {
    uint32_t mask;
    uint32_t value;
} term_t;

static struct {
    uint32_t active;                   /* bits that take part */
    int indexed;
    int unindexed;
    int top_bit;
    int capacity;
    number_t *from;
    number_t *to;
    uint32_t step;
    int count;
    int result;
    term_t *terms;
} g_range;
static int g_mixed_warned;

void parse_lower_reset(void)
{
    g_call_has_args = 0;
    parse_list_init(&g_func_params);
    g_call_counter = 0;
    memset(&g_xtable, 0, sizeof(g_xtable));
    memset(&g_range, 0, sizeof(g_range));
    g_mixed_warned = 0;
}

/* =========================================================================
 * Semantic actions: FUNCTION definitions and calls
 *
 * The body of a function is stored as its token text with the parameters
 * renamed .P1, .P2, ... and the function name .P0.  A call is replaced by
 * a new intermediate variable .<n>.<function>; the body, with the arguments
 * substituted and .P0 replaced by that variable, is parsed after the
 * statement that contains the call.
 * ========================================================================= */

/* FUNCTION name: enter the function; its name is parameter 0 */
int act_function_name(token_t *tok)
{
    g_parse_def_symbol = parse_enter_symbol(parse_variable_name(tok), NULL, 0, g_parse_table);
    g_parse_def_symbol->type = SYM_FUNCTION;
    g_parse_def_symbol->flags |= SF_ASSIGNED;
    parse_list_init(&g_func_params);
    parse_list_append(tok, &g_func_params);
    return 0;
}

/* the parameter list; the body is collected in the list below */
int act_function_params(token_t *tok)
{
    (void)tok;
    parse_list_concat(&g_func_params, g_parse_work);
    parse_work_pop();
    parse_list_init(g_parse_work);
    return 0;
}

/* '{' in a function: kept in the body unless it is the outermost one */
int act_function_lbrace(token_t *tok)
{
    g_parse_brace_depth++;
    if (g_parse_brace_depth > 1) {
        parse_list_append(tok, g_parse_work);
    }
    return 0;
}

/* returns 1 at the brace that ends the function */
int act_function_rbrace(token_t *tok)
{
    g_parse_brace_depth--;
    if (g_parse_brace_depth < 1) {
        g_parse_def_symbol->expr = parse_list_alloc();
        parse_list_concat(g_parse_def_symbol->expr, g_parse_work);
        parse_list_init(g_parse_work);
        g_parse_def_symbol = NULL;
        parse_list_init(&g_func_params);
        return 1;
    }
    parse_list_append(tok, g_parse_work);
    return 0;
}

/* a name in the body; parameters are renamed */
int act_function_ident(token_t *tok)
{
    if (tok->u.any != NULL) {
        int index = 0;
        for (token_t *param = g_func_params.head; param != NULL; param = param->next, index++) {
            if (param->u.any == tok->u.any) {
                parse_make_indexed_name(g_parse_name_buf, ".P", (unsigned)index);
                const char *name = parse_intern(g_parse_name_buf);
                if (name != NULL) {
                    tok->u.text = name;
                }
                break;
            }
        }
    }
    parse_list_append(tok, g_parse_work);
    return 0;
}

/* any other token of a function body */
int act_function_token(token_t *tok)
{
    parse_list_append(tok, g_parse_work);
    return 0;
}

int act_call_lparen(token_t *tok)
{
    (void)tok;
    g_parse_expand_fields = 0;
    return 0;
}

int act_call_args_done(token_t *tok)
{
    (void)tok;
    g_call_has_args = 1;
    return 0;
}

/* replace the .P<n> names of a function body by the arguments.
 * Tokens are spliced into the chain without updating the list header. */
static void substitute_arguments(const list_t *args, list_t *body)
{
    int highest = 0;
    int expected = args->count;
    for (token_t *tok = body->head; tok != NULL; tok = tok->next) {
        const char *text = parse_token_text(tok);
        if (text == NULL || text[0] != '.' || text[1] != 'P') {
            continue;
        }
        int index = parse_name_index(text);
        if (index < 0) {
            parse_fatal_error(1015, text);
        }
        if (highest < index) {
            highest = index;
        }
        token_t *arg = parse_list_nth_variable(args, index);
        if (arg == NULL) {
            parse_error_at(ERR_ARG_COUNT, "1");
        } else if (!(arg->flags & TF_NUMBER)) {
            tok->u = arg->u;
            token_t *ext = arg->next;
            if (ext != NULL && ext->type == TOK_EXTENSION) {
                token_t *name = parse_token_copy(ext);
                name->next = tok->next;
                name->type = TOK_IDENT;
                token_t *dot = parse_token_new_op(TOK_DOT);
                tok->next = dot;
                dot->next = name;
                expected--;
            }
        } else {
            list_t *number = parse_list_alloc();
            parse_append_number(arg->u.num, number);
            number->tail->next = tok->next;
            tok->u = number->head->u;
            tok->flags = number->head->flags;
            tok->next = number->head->next;
            tok->type = number->head->type;
            parse_list_free(number);
        }
    }
    if (highest < expected - 1) {
        parse_error_at(ERR_ARG_COUNT, "2");
    }
}

/* "append <result> = 'h'0;" so that the result exists even if
 * the body does not assign it */
static list_t *make_default_result(const token_t *result)
{
    list_t *list = parse_list_alloc();
    parse_append_op(list, TOK_APPEND);
    parse_list_append(parse_token_copy(result), list);
    parse_append_op(list, TOK_ASSIGN);
    parse_append_hex_number(0, list);
    parse_append_op(list, TOK_SEMI);
    return list;
}

/* name(arguments) */
int act_call(token_t *tok)
{
    (void)tok;
    if (!g_call_has_args) {
        parse_work_pop();
    }
    if (g_parse_work_level < 1) {
        parse_fatal_error(1022, "");
    }
    if (g_parse_work[-1].tail == NULL) {
        parse_fatal_error(1023, "");
    }
    token_t *name = parse_list_pop_back(g_parse_work - 1);
    symbol_t *function = parse_find_symbol_of_type(parse_token_text(name), NULL, SYM_FUNCTION, g_parse_table);
    if (function == NULL) {
        parse_error_at(ERR_UNDEFINED_FUNCTION, parse_token_text(name));
    } else {
        name->next = NULL;
        snprintf(g_parse_name_buf, sizeof(g_parse_name_buf), ".%d.%s", g_call_counter++,
                 or_null(parse_token_text(name)));
        parse_truncate_name(g_parse_name_buf, 0x21);
        name->u.text = parse_intern(g_parse_name_buf);
        parse_list_push_front(parse_token_copy(name), g_parse_work);
        if (function->expr == NULL) {
            parse_error_symbol(ERR_UNDEFINED_FUNCTION, function);
        } else {
            list_t *body = parse_list_copy(function->expr);
            substitute_arguments(g_parse_work, body);
            parse_list_concat(&g_parse_call_text, body);
            parse_list_push_front(name, &g_parse_queue);
            parse_list_free(body);
            list_t *result = make_default_result(name);
            parse_list_concat(&g_parse_call_text, result);
            parse_list_free(result);
        }
    }
    g_parse_expand_fields = 1;
    parse_work_pop();
    g_call_has_args = 0;
    return 0;
}

/* =========================================================================
 * Range and match operations: [vars]:number, [vars]:[n..m], [vars]:&
 *
 * The operation is replaced by its expansion as token text, pushed onto
 * the front of the token queue.
 * ========================================================================= */

/* prepend the tokens of `list` to the queue */
void parse_queue_prepend(list_t *list)
{
    parse_list_concat(list, &g_parse_queue);
}

/* [a,b,c]:& becomes a & b & c */
int act_list_operator(token_t *tok)
{
    list_t *copy = parse_list_copy(g_parse_work);
    parse_list_insert_between(tok, copy);
    parse_queue_prepend(copy);
    parse_list_clear(g_parse_work);
    parse_work_pop();
    parse_token_free(tok);
    g_parse_list_mode = 0;
    return 0;
}

/* the lowest `width` bits take part in the range */
static void set_range_width(int width)
{
    g_range.top_bit = width;
    g_range.active = 0;
    for (int i = 0; i < width; i++) {
        g_range.active |= 1u << (i & 31);
    }
}

/* which bit each variable of the list stands for.  Variables
 * with an index stand for that bit; if any has none, they are numbered by
 * position from the right. */
static void analyse_range_vars(const list_t *list)
{
    int count = 0;
    g_range.indexed = 0;
    g_range.unindexed = 0;
    g_range.top_bit = 0;
    g_range.active = 0;
    for (const token_t *tok = list->head; tok != NULL; tok = tok->next) {
        if (tok->type == TOK_EXTENSION) {
            continue;
        }
        count++;
        int index = parse_name_index(parse_token_text(tok));
        if (index < 0) {
            g_range.unindexed = 1;
        } else {
            g_range.indexed = 1;
        }
        if (g_range.top_bit < index) {
            g_range.top_bit = index;
        }
        if (index < 32) {
            g_range.active |= 1u << (index & 31);
        }
    }
    if (g_range.unindexed) {
        if (g_range.indexed && (!g_parse_in_sequence || !g_mixed_warned)) {
            parse_warning_at(ERR_MIXED_INDEX, "");
            g_mixed_warned = 1;
        }
        set_range_width(count);
    }
    if (g_range.top_bit > 31) {
        parse_error_at(ERR_INDEX_TOO_LARGE, "");
        set_range_width(32);
    }
}

/* the variables whose bit is in `active` and is not a
 * don't-care of `num`, negated where the bit of the number is 0.  The bit
 * of a variable is its index or, with `position` >= 0, its position. */
static list_t *select_bits(const list_t *list, int position, uint32_t active, const number_t *num)
{
    int count = 0;
    int by_position = position >= 0;
    if (list == NULL) {
        return NULL;
    }
    list_t *out = parse_list_alloc();
    for (const token_t *tok = list->head; tok != NULL; tok = tok->next) {
        if (tok->type == TOK_EXTENSION) {
            continue;
        }
        int index;
        if (by_position) {
            index = --position;
        } else {
            index = parse_name_index(parse_token_text(tok));
            if (index < 0) {
                parse_fatal_error(1030, "");
            }
        }
        uint32_t bit = 1u << (index & 31);
        if ((bit & active) && !(bit & num->dontcare)) {
            token_t *copy = parse_token_copy(tok);
            if (!(bit & num->value)) {
                copy->flags ^= TF_NEGATED;
            }
            parse_list_append(copy, out);
            if (tok->next != NULL && tok->next->type == TOK_EXTENSION) {
                parse_list_append(parse_token_copy(tok->next), out);
            }
            count++;
        }
    }
    if (by_position && position != 0) {
        parse_fatal_error(1018, "");
    }
    return count < 1 ? NULL : out;
}

/* the product term for one value: v1 & !v2 & ... */
static list_t *make_match_term(const list_t *list, uint32_t active, const number_t *num)
{
    if (active == 0) {
        parse_error_at(ERR_RANGE_ARGS, "match");
        active = 1u << (g_range.top_bit & 31);
    }
    list_t *term = select_bits(list, g_range.unindexed ? g_range.top_bit : -1, active, num);
    if (term == NULL) {
        parse_warning_at(ERR_ALL_DONTCARE, "");
        term = parse_list_alloc();
        parse_append_hex_number(0, term);
    } else {
        token_t *and_op = parse_token_new_op(TOK_AND);
        parse_list_insert_where(and_op, -1, TOK_EXTENSION, 1, term);
        parse_list_insert_where(parse_token_new_op(TOK_DOT), -1, TOK_EXTENSION, 0, term);
        parse_list_replace_type(TOK_EXTENSION, TOK_IDENT, term);
        parse_token_free(and_op);
    }
    return term;
}

/* try to combine the term (mask, value) with one of the stored
 * terms other than `skip`.  Returns the index of that term or -1.
 * g_range.result: 1 same term, 2 terms differed in one bit (now dropped),
 * 3 one term covers the other. */
static int merge_term(uint32_t mask, uint32_t value, int skip)
{
    g_range.result = 0;
    if (g_range.count == 0 || g_parse_min_level == 0) {
        return -1;
    }
    for (int i = 0; i < g_range.count; i++) {
        term_t *term = &g_range.terms[i];
        if (i == skip) {
            continue;
        }
        uint32_t common = mask & term->mask;
        if (term->mask == mask) {
            if ((value & mask) == (term->value & mask)) {
                g_range.result = 1;
                return i;
            }
            int differing = 0;
            int which = 0;
            for (int bit = 0; bit <= g_range.top_bit; bit++) {
                if ((1u << (bit & 31)) & (value ^ term->value) & mask) {
                    differing++;
                    which = bit;
                }
            }
            if (differing == 1) {
                term->mask &= ~(1u << (which & 31));
                term->value &= ~(1u << (which & 31));
                if (term->mask == 0) {
                    parse_error_at(ERR_RANGE_MIN, "");
                }
                g_range.result = 2;
                return i;
            }
        }
        if (common == mask && (value & common) == (common & term->value)) {
            term->mask = mask;
            term->value = value;
            g_range.result = 3;
            return i;
        }
        if (common == term->mask && (term->value & common) == (common & value)) {
            g_range.result = 3;
            return i;
        }
    }
    return -1;
}

/* order the limits and drop the low bits that take all values */
static void normalise_range(void)
{
    number_t *from = g_range.from;
    number_t *to = g_range.to;
    if (parse_compare_numbers(from, to) > 0) {
        number_t swap = *to;
        *to = *from;
        *from = swap;
    }
    to->value |= to->dontcare;
    from->dontcare = 0;
    to->dontcare = 0;
    uint32_t low = 0;
    int bits = 0;
    for (; bits < 32; bits++) {
        uint32_t bit = 1u << bits;
        if ((bit & g_range.active) && (!(bit & to->value) || (bit & from->value))) {
            break;
        }
        low |= bit;
    }
    if (g_range.top_bit < bits) {
        parse_error_at(ERR_RANGE_ARGS, "always true (in range)");
        to->value = from->value;
        g_range.step = 1;
    } else {
        g_range.step = low + 1;
        g_range.active &= ~low;
        to->value &= ~low;
    }
}

/* the product terms that cover from..to */
static void minimise_range(void)
{
    normalise_range();
    g_range.capacity = TERM_CHUNK;
    g_range.terms = parse_alloc_tracked((size_t)g_range.capacity * sizeof(term_t), "minimizing expression");
    g_range.count = 0;
    for (uint32_t value = g_range.from->value; value <= g_range.to->value; value += g_range.step) {
        if (merge_term(g_range.active, value, -1) < 0) {
            g_range.terms[g_range.count].mask = g_range.active;
            g_range.terms[g_range.count].value = value;
            g_range.count++;
        }
        if (g_range.count == g_range.capacity) {
            term_t *grown = parse_alloc_tracked((size_t)(g_range.capacity + TERM_CHUNK) * sizeof(term_t),
                                          "minimizing expression");
            memcpy(grown, g_range.terms, (size_t)g_range.capacity * sizeof(term_t));
            g_range.terms = grown;
            g_range.capacity += TERM_CHUNK;
        }
        if (value + g_range.step < value) {
            break;
        }
    }
    /* combine the terms with each other until nothing changes */
    int again;
    do {
        if (g_range.count == 0) {
            parse_error_at(ERR_RANGE_MIN, "");
        }
        if (g_range.count < 2) {
            return;
        }
        again = 0;
        int i = 0;
        while (i < g_range.count) {
            if (merge_term(g_range.terms[i].mask, g_range.terms[i].value, i) < 0) {
                i++;
                continue;
            }
            term_t last = g_range.terms[g_range.count - 1];
            g_range.terms[g_range.count - 1] = g_range.terms[i];
            g_range.terms[i] = last;
            g_range.count--;
            g_range.terms[g_range.count].mask = 0;
            g_range.terms[g_range.count].value = 0;
            if (g_range.result == 2) {
                again = 1;
            }
        }
    } while (again);
}

/* ( term # term # ... ) for a range of values */
static list_t *make_range_terms(const list_t *vars)
{
    minimise_range();
    number_t *num = parse_number_alloc();
    list_t *out = parse_list_alloc();
    parse_list_push_front(parse_token_new_op(TOK_LPAREN), out);
    for (int i = 0; i < g_range.count; i++) {
        num->value = g_range.terms[i].value;
        num->dontcare = 0;
        list_t *term = make_match_term(vars, g_range.terms[i].mask, num);
        parse_list_concat(out, term);
        parse_append_op(out, TOK_OR);
    }
    g_range.capacity = 0;
    parse_token_free(parse_list_pop_back(out));
    parse_append_op(out, TOK_RPAREN);
    return out;
}

/* [vars]:value or [vars]:[values].  Every element of a list stands for
 * its own set of values and the result is their union: each element starts
 * from all the bits of the variables, whatever a range before it left
 * open.  The elements are prepended to the queue one by one, so the
 * expansion lists them last to first, in parentheses. */
int act_range_match(token_t *tok)
{
    (void)tok;
    int first = 1;
    if (g_parse_numbers.head == NULL) {
        parse_fatal_error(1031, "");
    }
    parse_list_copy(g_parse_work);
    analyse_range_vars(g_parse_work);
    uint32_t all_bits = g_range.active;
    for (token_t *num = g_parse_numbers.head; num != NULL; num = num->next) {
        g_range.active = all_bits;
        if (num->type == TOK_LIST_NUMBER) {
            list_t *term = make_match_term(g_parse_work, g_range.active, num->u.num);
            if (first) {
                parse_list_push_front(parse_token_new_op(TOK_RPAREN), &g_parse_queue);
                first = 0;
            }
            parse_list_push_front(parse_token_new_op(num->next == NULL ? TOK_LPAREN : TOK_OR), term);
            parse_queue_prepend(term);
        } else if (num->type == TOK_RANGE_FROM) {
            g_range.from = num->u.num;
            if (num->next == NULL) {
                parse_fatal_error(1034, "");
            }
            g_range.to = num->next->u.num;
            parse_queue_prepend(make_range_terms(g_parse_work));
            num = num->next;
            if (num->next != NULL) {
                parse_list_push_front(parse_token_new_op(TOK_OR), &g_parse_queue);
            } else if (!first) {
                /* a range ends a list that has single values before it:
                 * open the parenthesis the first of them closes */
                parse_list_push_front(parse_token_new_op(TOK_LPAREN), &g_parse_queue);
            }
        } else {
            parse_fatal_error(1033, "");
        }
    }
    parse_list_clear(g_parse_work);
    parse_work_pop();
    parse_list_clear(&g_parse_numbers);
    g_parse_list_mode = 0;
    g_parse_radix = TOK_RADIX_H;
    return 0;
}

/* =========================================================================
 * TABLE inputs => outputs { values => value; ... }
 *
 * Every row becomes the statement
 *   append [outputs] = .odel 'h'value & [inputs]:values .fdel
 * which is parsed next; the ';' of the row ends it.
 * ========================================================================= */

/* the variables of a list as "[a,b.ext,c]" */
static list_t *bracket_vars(const list_t *vars)
{
    list_t *list = parse_list_copy(vars);
    parse_list_insert_where(parse_token_new_op(TOK_COMMA), -1, TOK_EXTENSION, 1, list);
    parse_list_insert_where(parse_token_new_op(TOK_DOT), -1, TOK_EXTENSION, 0, list);
    parse_list_push_front(parse_token_new_op(TOK_LBRACKET), list);
    parse_append_op(list, TOK_RBRACKET);
    parse_list_replace_type(TOK_EXTENSION, TOK_IDENT, list);
    return list;
}

void parse_append_bracket_vars(list_t *list, const list_t *vars)
{
    list_t *bracket = bracket_vars(vars);
    parse_list_concat(list, bracket);
    parse_list_free(bracket);
}

static const char *radix_letter(const number_t *num)
{
    int radix = parse_number_radix(num);
    return radix == 2 ? "b" : (radix == 8 ? "o" : "h");
}

/* a value list as "'h'[n,n..m,n]" */
static list_t *make_value_list(const list_t *values)
{
    list_t *list = parse_list_alloc();
    parse_append_op(list, TOK_QUOTE);
    parse_list_append(parse_token_new_text(radix_letter(values->head->u.num), TOK_IDENT), list);
    parse_append_op(list, TOK_QUOTE);
    parse_append_op(list, TOK_LBRACKET);
    for (const token_t *tok = values->head; tok != NULL; tok = tok->next) {
        if (tok->type == TOK_RANGE_FROM) {
            if (list->tail->type != TOK_LBRACKET) {
                parse_append_op(list, TOK_COMMA);
            }
            parse_list_append(parse_token_new_number_text(tok, TOK_NUMBER), list);
            parse_append_op(list, TOK_DOT);
            parse_append_op(list, TOK_DOT);
        } else if (tok->type != TOK_LIST_NUMBER) {
            parse_list_append(parse_token_new_number_text(tok, TOK_NUMBER), list);
        } else {
            if (list->tail->type != TOK_LBRACKET) {
                parse_append_op(list, TOK_COMMA);
            }
            parse_list_append(parse_token_new_number_text(tok, TOK_NUMBER), list);
        }
    }
    parse_append_op(list, TOK_RBRACKET);
    return list;
}

/* build the APPEND statement for the current TABLE row */
static void build_table_statement(void)
{
    list_t *list = parse_list_alloc();
    g_xtable.statement = list;
    parse_append_op(list, TOK_APPEND);
    parse_append_bracket_vars(list, g_xtable.outputs);
    parse_append_op(list, TOK_ASSIGN);
    parse_append_op(list, TOK_ODEL);
    if (g_xtable.out_values->head == NULL || g_xtable.in_values == NULL
        || g_xtable.in_values->head == NULL) {
        parse_fatal_error(ERR_BAD_TABLE, "");
    }
    parse_append_number(g_xtable.out_values->head->u.num, list);
    parse_append_op(list, TOK_AND);
    parse_append_bracket_vars(list, g_xtable.inputs);
    parse_append_op(list, TOK_COLON);
    if (g_xtable.in_values->count < 2) {
        const token_t *value = g_xtable.in_values->head;
        parse_append_op(list, TOK_QUOTE);
        parse_list_append(parse_token_new_text(radix_letter(value->u.num), TOK_IDENT), list);
        parse_append_op(list, TOK_QUOTE);
        parse_list_append(parse_token_new_number_text(value, TOK_NUMBER), list);
    } else {
        list_t *values = make_value_list(g_xtable.in_values);
        parse_list_concat(list, values);
        parse_list_free(values);
    }
    parse_append_op(list, TOK_FDEL);
}

/* TABLE inputs */
int act_table_inputs(token_t *tok)
{
    (void)tok;
    g_xtable.outputs = NULL;
    g_xtable.in_values = NULL;
    g_xtable.out_values = NULL;
    g_xtable.inputs = parse_list_clone_header(g_parse_work);
    g_parse_radix = TOK_RADIX_H;
    g_parse_list_mode = 1;
    parse_work_pop();
    return 0;
}

/* => outputs */
int act_table_outputs(token_t *tok)
{
    (void)tok;
    g_xtable.outputs = parse_list_clone_header(g_parse_work);
    parse_work_pop();
    return 0;
}

/* the input values of a row */
int act_table_row_inputs(token_t *tok)
{
    (void)tok;
    g_xtable.in_values = parse_list_clone_header(&g_parse_numbers);
    parse_list_init(&g_parse_numbers);
    g_parse_radix = TOK_RADIX_H;
    g_parse_list_mode = 0;
    return 0;
}

/* the output value of a row */
int act_table_row_outputs(token_t *tok)
{
    (void)tok;
    g_xtable.out_values = parse_list_clone_header(&g_parse_numbers);
    if (g_xtable.out_values->count > 1) {
        parse_error_at(ERR_BAD_TABLE, "");
    }
    parse_list_init(&g_parse_numbers);
    build_table_statement();
    parse_queue_prepend(g_xtable.statement);
    g_parse_radix = TOK_RADIX_H;
    g_parse_list_mode = 1;
    return 0;
}

/* '}' */
int act_table_end(token_t *tok)
{
    parse_list_free(g_xtable.statement);
    parse_list_destroy(g_xtable.in_values);
    parse_list_destroy(g_xtable.out_values);
    parse_list_destroy(g_xtable.inputs);
    parse_list_destroy(g_xtable.outputs);
    parse_token_free(tok);
    g_parse_radix = TOK_RADIX_H;
    g_parse_list_mode = 0;
    return 0;
}

