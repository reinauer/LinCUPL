/*
 * parse - semantic actions of the statements that are not lowered into
 * generated text: header items, variable lists and numbers, PIN/NODE/
 * PINNODE, FIELD/GROUP, MIN/DEMORGAN/REGISTER_SELECT, assignments and
 * expressions, comparisons, and DECLARE/PROPERTY (copied to the .oed file).
 * The actions are called by the parser (parse_grammar.c) as the rules of
 * parse_tables.h match.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#include "platform.h"
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parse_int.h"
#include "strutil.h"

static char g_message[64];             /* scratch text for message details */
char g_parse_name_buf[NAME_BUF_SIZE];  /* scratch for generated names */

/* statement state */
static number_t *g_num_first;
static number_t *g_num_second;
static const char *g_range_prefix;
int g_parse_expand_fields;             /* field names stand for their members */
static int g_pending_not;              /* '!' seen */
static int g_negated_brackets;         /* open brackets that were negated */
static int g_else_chain;               /* inside a generated IF chain */
int g_parse_list_mode;                 /* numbers are list members */
static int g_append;                   /* statement starts with APPEND */
int g_parse_radix;                     /* TOK_RADIX_x */
int g_parse_brace_depth;
static int g_header_keyword;
symbol_t *g_parse_def_symbol;          /* FIELD/GROUP/FUNCTION being defined */
list_t g_parse_numbers;                /* numbers of the statement */
list_t g_parse_call_text;              /* body of the call being expanded */
static symbol_t *g_last_lhs;           /* last target newly assigned */
static int g_compare_stmt;             /* statement is "vars == vars" */
static int g_lhs_count;                /* targets of the current statement */
symbol_t **g_parse_lhs_syms;
static char *g_lhs_skip;               /* per target: left out until .fdel */
static int g_lhs_cap;
static int g_node_counter;
int g_parse_max_min_level;

/* DECLARE / PROPERTY */
typedef struct declare_item {
    char name[24];
    int kind;
    int arg;
    struct declare_item *next;
} declare_item_t;

static struct {
    char name[24];
    declare_item_t *items;
} g_declare[DECLARE_MAX];
static int g_declare_count;
static char g_declare_name[24];
static int g_property_start;
FILE *g_parse_oed_fp;

void parse_actions_reset(void)
{
    g_message[0] = '\0';
    g_parse_name_buf[0] = '\0';
    g_num_first = NULL;
    g_num_second = NULL;
    g_range_prefix = NULL;
    g_parse_expand_fields = 0;
    g_pending_not = 0;
    g_negated_brackets = 0;
    g_else_chain = 0;
    g_parse_list_mode = 0;
    g_append = 0;
    g_parse_radix = 0;
    g_parse_brace_depth = 0;
    g_header_keyword = 0;
    g_parse_def_symbol = NULL;
    parse_list_init(&g_parse_numbers);
    parse_list_init(&g_parse_call_text);
    g_last_lhs = NULL;
    g_compare_stmt = 0;
    g_lhs_count = 0;
    g_parse_lhs_syms = NULL;
    g_lhs_skip = NULL;
    g_lhs_cap = 0;
    g_node_counter = 0;
    g_parse_max_min_level = 0;
    memset(g_declare, 0, sizeof(g_declare));
    g_declare_count = 0;
    g_declare_name[0] = '\0';
    g_property_start = 1;
    g_parse_oed_fp = NULL;
}

/* statement state at the start of the input */
void parse_actions_begin(void)
{
    g_negated_brackets = 0;
    g_pending_not = 0;
    g_parse_work_level = 0;
    g_parse_brace_depth = 0;
    g_parse_expand_fields = 1;
    g_parse_list_mode = 0;
    g_append = 0;
}

/* =========================================================================
 * Semantic actions: header
 * ========================================================================= */

/* NAME, PARTNO, ... DEVICE, FORMAT: the text up to ';' follows */
int act_header_keyword(token_t *tok)
{
    g_header_keyword = tok->type;
    if (g_header_keyword == TOK_REV) g_header_keyword = TOK_REVISION;
    if (g_header_keyword == TOK_ASSY) g_header_keyword = TOK_ASSEMBLY;
    if (g_header_keyword == TOK_LOC) g_header_keyword = TOK_LOCATION;
    parse_push_mode();
    parse_lex_header_text_mode();
    parse_token_free(tok);
    return 0;
}

/* the text of a header item: store it in the table's header list unless
 * the item was given already */
int act_header_text(token_t *tok)
{
    tok->type = (uint8_t)g_header_keyword;
    if (parse_list_find_type(&g_parse_table->header, g_header_keyword) == NULL) {
        parse_list_append(tok, &g_parse_table->header);
    } else {
        parse_error_at(ERR_HEADER_TWICE, "");
    }
    parse_pop_mode();
    return 0;
}

/* the text of NAME: names the current table; it must be new */
int act_header_name(token_t *tok)
{
    if (parse_list_find_type(&g_parse_table->header, TOK_NAME) != NULL) {
        return act_header_text(tok);
    }
    if (tok->u.any == NULL) {
        parse_error_at(ERR_HEADER_NAME, "");
        parse_fatal_error(-1, "");
    }
    if (parse_find_table(tok->u.text) != NULL) {
        parse_error_at(ERR_HEADER_NAME, tok->u.text);
        parse_fatal_error(-1, "");
    }
    g_parse_table->name = tok->u.text;
    return act_header_text(tok);
}

/* DEVICE; a device given on the command line wins */
int act_header_device(token_t *tok)
{
    if (tok->u.text == NULL || tok->u.text[0] == '\0') {
        parse_error_at(ERR_SYNTAX, "empty DEVICE statement");
        parse_pop_mode();
        parse_token_free(tok);
        return 0;
    }
    if (g_parse_device_arg[0] == '&') {
        char *name = (char *)(uintptr_t)tok->u.text;
        g_parse_device_name = (name != NULL) ? name : "";
        for (; name != NULL && *name != '\0'; name++) {
            if (is_upper((unsigned char)*name)) {
                *name = (char)to_lower((unsigned char)*name);
            } else if (is_space((unsigned char)*name)) {
                *name = '\0';
                break;
            }
        }
    } else {
        g_parse_device_name = g_parse_device_arg;
    }
    parse_pop_mode();
    parse_require_device(g_parse_table);
    return 0;
}

int act_header_format(token_t *tok)
{
    g_parse_table->format = tok->u.text;
    parse_pop_mode();
    return 0;
}

/* =========================================================================
 * Semantic actions: variable lists and numbers
 * ========================================================================= */

/* tokens that only set a flag */
int act_modifier(token_t *tok)
{
    switch (tok->type) {
    case TOK_NOT:
        g_pending_not = 1;
        break;
    case TOK_APPEND:
        g_append = 1;
        break;
    case TOK_RADIX_B:
    case TOK_RADIX_D:
    case TOK_RADIX_H:
    case TOK_RADIX_O:
        g_parse_radix = tok->type;
        break;
    case TOK_LBRACE:
        g_parse_brace_depth++;
        break;
    default:
        break;
    }
    parse_token_free(tok);
    return 0;
}

int act_work_push(token_t *tok)
{
    (void)tok;
    parse_work_push();
    return 0;
}

int act_work_pop(token_t *tok)
{
    (void)tok;
    parse_work_pop();
    return 0;
}

/* a bracketed list is complete: add it to the enclosing list */
int act_work_merge(token_t *tok)
{
    (void)tok;
    if (g_parse_work_level < 1) {
        parse_fatal_error(1020, "");
    }
    parse_list_concat(g_parse_work - 1, g_parse_work);
    parse_work_pop();
    return 0;
}

int act_list_mode_on(token_t *tok)
{
    (void)tok;
    g_parse_list_mode = 1;
    return 0;
}

int act_list_mode_off(token_t *tok)
{
    (void)tok;
    g_parse_list_mode = 0;
    return 0;
}

/* '[' */
int act_lbracket(token_t *tok)
{
    if (!g_pending_not) {
        g_parse_work->flags |= 1;
    } else {
        g_negated_brackets = (signed char)(g_negated_brackets + 1);
        g_parse_work->flags &= (uint16_t)~1u;
    }
    g_pending_not = 0;
    parse_token_free(tok);
    return 0;
}

/* ']' */
int act_rbracket(token_t *tok)
{
    if (!(g_parse_work->flags & 1)) {
        g_negated_brackets = (signed char)(g_negated_brackets - 1);
    }
    g_parse_work->flags &= (uint16_t)~1u;
    parse_token_free(tok);
    return 0;
}

/* a '!' in front of the variable or of an odd number of enclosing brackets */
static int take_negation(void)
{
    int negated = (g_negated_brackets + g_pending_not) % 2 != 0;
    g_pending_not = 0;
    return negated;
}

/* the members of a FIELD.  Negating the field makes
 * every member negated; members that were negated already stay so. */
static list_t *expand_field(const token_t *tok, int negated)
{
    symbol_t *field = parse_find_symbol_of_type(parse_token_text(tok), NULL, SYM_FIELD, g_parse_table);
    if (field == NULL) {
        return NULL;
    }
    list_t *members = parse_list_copy(field->expr);
    if (members != NULL) {
        for (token_t *t = members->head; t != NULL; t = t->next) {
            t->flags |= (uint8_t)((t->flags & TF_NEGATED) != negated);
        }
    }
    return members;
}

/* a variable name */
int act_variable(token_t *tok)
{
    int negated = take_negation();
    list_t *members = g_parse_expand_fields ? expand_field(tok, negated) : NULL;
    if (members != NULL) {
        parse_list_concat(g_parse_work, members);
        parse_list_free(members);
    } else {
        if (negated) {
            tok->flags ^= TF_NEGATED;
        }
        parse_list_append(tok, g_parse_work);
    }
    return 0;
}

/* the variables prefix<from+-1> .. prefix<to> */
static list_t *make_range(const char *prefix, uint32_t from, uint32_t to, int type, int flags)
{
    uint32_t step = (from <= to) ? 1u : 0xffffffffu;
    list_t *list = parse_list_alloc();
    while (from != to) {
        from += step;
        parse_make_indexed_name(g_parse_name_buf, prefix, from);
        token_t *tok = parse_token_alloc();
        tok->u.text = parse_intern(g_parse_name_buf);
        tok->type = (uint8_t)type;
        tok->flags = (uint8_t)flags;
        parse_list_append(tok, list);
    }
    return list;
}

/* name<n> at the start of [name<n>..<m>] */
int act_range_first(token_t *tok)
{
    const char *text = parse_token_text(tok);
    g_num_first = parse_number_alloc();
    int index = parse_name_index(text);
    if (index < 0) {
        parse_error_at(ERR_BAD_NUMBER, text);
        g_num_first = NULL;
    } else {
        g_num_first->value = (uint32_t)index;
    }
    parse_name_prefix(g_parse_name_buf, sizeof(g_parse_name_buf), text);
    g_range_prefix = parse_intern(g_parse_name_buf);
    tok->type = TOK_IDENT_NUM;
    return act_variable(tok);
}

/* the end of [name<n>..<m>] */
int act_range_last(token_t *tok)
{
    const char *text = parse_token_text(tok);
    g_num_second = parse_number_alloc();
    if (take_negation()) {
        tok->flags |= TF_NEGATED;
    } else {
        tok->flags &= (uint8_t)~TF_NEGATED;
    }
    int index = parse_name_index(text);
    if (index < 0) {
        parse_error_at(ERR_BAD_NUMBER, text);
        g_num_second = NULL;
    } else {
        g_num_second->value = (uint32_t)index;
        list_t *range = make_range(g_range_prefix, g_num_first != NULL ? g_num_first->value : 0,
                                   (uint32_t)index, TOK_IDENT_NUM, tok->flags);
        parse_list_concat(g_parse_work, range);
        parse_list_free(range);
    }
    return 0;
}

/* the extension after '.' goes behind every variable of the
 * current list that has none yet */
int act_extension(token_t *tok)
{
    tok->type = TOK_EXTENSION;
    parse_list_insert_missing(tok, TOK_EXTENSION, g_parse_work);
    return 0;
}

/* the letter in 'b', 'o', 'd', 'h' */
int act_radix_prefix(token_t *tok)
{
    static const keyword_t radixes[] = {
        { "b", TOK_RADIX_B }, { "d", TOK_RADIX_D }, { "h", TOK_RADIX_H }, { "o", TOK_RADIX_O }
    };
    const keyword_t *radix = parse_find_in_table(radixes, 4, parse_token_text(tok));
    if (radix == NULL) {
        parse_error_at(ERR_SYNTAX, parse_token_text(tok));
        return 0;
    }
    g_parse_radix = radix->id;
    parse_token_free(tok);
    return 0;
}

static void number_token(token_t *tok)
{
    g_num_first = parse_number_text(parse_token_text(tok), g_parse_radix);
    tok->u.any = NULL;
    tok->u.num = g_num_first;
    tok->flags |= TF_NUMBER;
    tok->type = g_parse_list_mode ? TOK_LIST_NUMBER : TOK_NUMBER;
}

/* a number of a number list */
int act_number(token_t *tok)
{
    number_token(tok);
    parse_list_append(tok, &g_parse_numbers);
    return 0;
}

/* a number in a variable list */
int act_number_var(token_t *tok)
{
    number_token(tok);
    if (take_negation()) {
        tok->flags ^= TF_NEGATED;
    }
    parse_list_append(tok, g_parse_work);
    return 0;
}

/* the second number of n..m.  Outside value lists the numbers
 * in between are generated; inside them the pair is kept as a range. */
int act_number_range(token_t *tok)
{
    g_num_second = parse_number_text(parse_token_text(tok), g_parse_radix);
    number_t *from = parse_number_alloc();
    uint32_t step;
    if (g_num_first != NULL) {
        *from = *g_num_first;
    }
    if (parse_compare_numbers(g_num_second, from) < 0) {
        step = 0xffffffffu;
        from->value |= from->dontcare;
    } else {
        step = 1;
        g_num_second->value |= g_num_second->dontcare;
    }
    from->dontcare = 0;
    g_num_second->dontcare = 0;
    from->value += step;
    tok->flags |= TF_NUMBER;
    if (!g_parse_list_mode) {
        for (uint32_t value = from->value; value != g_num_second->value + step; value += step) {
            number_t *num = parse_number_alloc();
            num->value = value;
            token_t *copy = parse_token_alloc();
            copy->flags = tok->flags;
            copy->type = tok->type;
            copy->u.num = num;
            parse_list_append(copy, &g_parse_numbers);
        }
    } else {
        if (g_parse_numbers.tail == NULL) {
            parse_fatal_error(1032, "");
        }
        g_parse_numbers.tail->type = TOK_RANGE_FROM;
        token_t *copy = parse_token_alloc();
        copy->flags = tok->flags;
        copy->type = TOK_RANGE_TO;
        copy->u.num = g_num_second;
        parse_list_append(copy, &g_parse_numbers);
    }
    return 0;
}

/* The name under which a token of a variable list is entered.  A number in
 * a variable list is entered under the bytes of its value and don't-care
 * mask, read as a string up to the first zero byte.  This is deliberate:
 * the symbols such statements produce in the CB file depend on it. */
const char *parse_variable_name(const token_t *tok)
{
    if (!(tok->flags & TF_NUMBER)) {
        return tok->u.text != NULL ? tok->u.text : g_parse_empty_ext;
    }
    char bytes[9];
    const number_t *num = tok->u.num;
    for (int i = 0; i < 4; i++) {
        bytes[i] = (char)(num->value >> (8 * i));
        bytes[4 + i] = (char)(num->dontcare >> (8 * i));
    }
    bytes[8] = '\0';
    return parse_dup_string(bytes, "parsing");
}

/* make room for LHS_CHUNK more statement targets */
void parse_grow_targets(void)
{
    int cap = g_lhs_cap + LHS_CHUNK;
    symbol_t **syms = parse_alloc_tracked((size_t)cap * sizeof(*syms), "list processing");
    char *skip = parse_alloc_tracked((size_t)cap, "list processing");
    if (g_lhs_cap > 0) {
        memcpy(syms, g_parse_lhs_syms, (size_t)g_lhs_cap * sizeof(*syms));
        memcpy(skip, g_lhs_skip, (size_t)g_lhs_cap);
    }
    g_parse_lhs_syms = syms;
    g_lhs_skip = skip;
    g_lhs_cap = cap;
}

/* enter the variables of a list; they become the targets of the
 * statement (g_parse_lhs_syms) */
static void resolve_targets(list_t *list, int may_exist, int mark_pin_negated, int mark_lhs_negated,
                            int mark_assigned)
{
    if (list == NULL) {
        parse_fatal_error(1002, "");
    }
    g_lhs_count = 0;
    for (token_t *tok = list->head; tok != NULL; tok = tok->next) {
        int negated = tok->flags & TF_NEGATED;
        symbol_t *sym;
        if (g_lhs_count >= g_lhs_cap) {
            parse_grow_targets();
        }
        if (tok->next != NULL && tok->next->type == TOK_EXTENSION) {
            sym = parse_enter_symbol(parse_variable_name(tok), parse_token_text(tok->next), may_exist,
                                     g_parse_table);
            tok = tok->next;
        } else {
            sym = parse_enter_symbol(parse_variable_name(tok), NULL, may_exist, g_parse_table);
        }
        g_parse_lhs_syms[g_lhs_count] = sym;
        if (mark_lhs_negated && negated) {
            sym->flags |= SF_LHS_NEGATED;
        }
        if (mark_pin_negated && negated) {
            sym->flags |= SF_PIN_NEGATED;
        }
        if (mark_assigned) {
            sym->flags |= SF_ASSIGNED;
        }
        g_lhs_count++;
    }
}

/* =========================================================================
 * Semantic actions: declarations
 * ========================================================================= */

/* PIN / PINNODE; the numbers that follow are decimal */
int act_pin_keyword(token_t *tok)
{
    g_parse_radix = TOK_RADIX_D;
    parse_work_push();
    parse_token_free(tok);
    return 0;
}

static void size_message(int lhs, int rhs)
{
    snprintf(g_message, sizeof(g_message), "lhs size = %d, rhs size = %d", lhs, rhs);
}

/* PIN numbers = names; PINNODE numbers = names */
static void declare_pins(int type)
{
    if (g_parse_work->count == 0) {
        parse_fatal_error(1004, "");
    }
    resolve_targets(g_parse_work, 0, 1, 0, 0);
    if (g_parse_numbers.count != g_lhs_count && parse_virtual_flag()[0] != '1') {
        size_message(g_parse_numbers.count, g_lhs_count);
        parse_error_at(ERR_SIZE_MISMATCH, g_message);
    }
    token_t *number = g_parse_numbers.head;
    for (int i = 0; i < g_lhs_count; i++) {
        if (number == NULL && parse_virtual_flag()[0] == '1') {
            g_parse_lhs_syms[i]->pin = 0;
        } else if (number == NULL) {
            break;
        } else {
            g_parse_lhs_syms[i]->pin = (int16_t)number->u.num->value;
            number = number->next;
        }
        g_parse_lhs_syms[i]->type = (uint8_t)type;
    }
    parse_list_clear(g_parse_work);
    parse_work_pop();
    parse_list_clear(&g_parse_numbers);
    g_parse_radix = TOK_RADIX_H;
}

int act_pin_declare(token_t *tok)
{
    (void)tok;
    declare_pins(SYM_PIN);
    return 0;
}

int act_pinnode_declare(token_t *tok)
{
    (void)tok;
    declare_pins(SYM_NODE);
    return 0;
}

/* NODE names: the nodes are numbered after the device's pins */
int act_node_declare(token_t *tok)
{
    (void)tok;
    if (g_parse_work->count == 0) {
        parse_fatal_error(1025, "");
    }
    resolve_targets(g_parse_work, 0, 1, 0, 0);
    parse_require_device(g_parse_table);
    int first = (int)parse_dev_arch_byte(g_parse_table->device, ARCH_PIN_COUNT) + 1;
    for (int i = 0; i < g_lhs_count; i++) {
        g_parse_lhs_syms[i]->pin = (int16_t)(first + g_node_counter);
        g_node_counter++;
        g_parse_lhs_syms[i]->type = SYM_NODE;
    }
    parse_list_clear(g_parse_work);
    parse_work_pop();
    return 0;
}

/* FIELD name / GROUP name */
static int define_list_symbol(token_t *tok, int type)
{
    g_parse_def_symbol = parse_enter_symbol(parse_variable_name(tok), NULL, 0, g_parse_table);
    g_parse_def_symbol->type = (uint8_t)type;
    parse_token_free(tok);
    return 0;
}

int act_field_name(token_t *tok)
{
    return define_list_symbol(tok, SYM_FIELD);
}

int act_group_name(token_t *tok)
{
    return define_list_symbol(tok, SYM_GROUP);
}

/* end of FIELD/GROUP: the collected list becomes the members */
int act_field_end(token_t *tok)
{
    (void)tok;
    g_parse_def_symbol->expr = parse_list_clone_header(g_parse_work);
    g_parse_def_symbol = NULL;
    parse_work_pop();
    return 0;
}

int act_group_end(token_t *tok)
{
    return act_field_end(tok);
}

/* the '=' of MIN, DEMORGAN, REGISTER_SELECT */
static int decimal_follows(token_t *tok)
{
    g_parse_radix = TOK_RADIX_D;
    parse_token_free(tok);
    return 0;
}

int act_min_equals(token_t *tok) { return decimal_follows(tok); }
int act_demorgan_equals(token_t *tok) { return decimal_follows(tok); }
int act_regsel_equals(token_t *tok) { return decimal_follows(tok); }

/* MIN / DEMORGAN / REGISTER_SELECT vars = level: returns the level or -1 */
static int begin_level_statement(int limit, int error)
{
    if (g_parse_work->count == 0) {
        parse_fatal_error(1046, "");
    }
    resolve_targets(g_parse_work, 1, 0, 0, 0);
    int32_t level = (g_parse_numbers.head != NULL) ? (int32_t)g_parse_numbers.head->u.num->value : 0;
    if (g_parse_numbers.head == NULL || level > limit || level < 0) {
        parse_error_at(error, "");
        return -1;
    }
    return level;
}

static void end_level_statement(void)
{
    parse_list_clear(g_parse_work);
    parse_work_pop();
    parse_list_clear(&g_parse_numbers);
    g_parse_radix = TOK_RADIX_H;
}

/* MIN vars = level: set the minimisation level of the variables */
int act_min_level(token_t *tok)
{
    (void)tok;
    int level = begin_level_statement(4, ERR_MIN_LEVEL);
    if (level >= 0) {
        if (g_parse_max_min_level < level) {
            g_parse_max_min_level = level;
        }
        for (int i = 0; i < g_lhs_count; i++) {
            g_parse_lhs_syms[i]->flags |= (uint8_t)(level & SF_MIN_MASK);
            g_parse_lhs_syms[i]->flags |= SF_MIN_SET;
        }
    }
    end_level_statement();
    return 0;
}

/* DEMORGAN vars = level: set the DeMorgan level (0..2) of the variables */
int act_demorgan_level(token_t *tok)
{
    (void)tok;
    int level = begin_level_statement(2, ERR_DEMORGAN_LEVEL);
    if (level >= 0) {
        for (int i = 0; i < g_lhs_count; i++) {
            g_parse_lhs_syms[i]->demorgan |= (uint8_t)(level | 0x80);
        }
    }
    end_level_statement();
    return 0;
}

/* REGISTER_SELECT vars = level: set the register type (0..5) */
int act_regsel_level(token_t *tok)
{
    (void)tok;
    int level = begin_level_statement(5, ERR_REGSEL_LEVEL);
    if (level >= 0) {
        for (int i = 0; i < g_lhs_count; i++) {
            g_parse_lhs_syms[i]->regtype = (uint16_t)level;
        }
    }
    end_level_statement();
    return 0;
}

/* =========================================================================
 * Semantic actions: assignments and expressions
 *
 * An expression is not built as a tree: its tokens are appended, as they
 * are read, to the expression list of every target of the statement.
 * ========================================================================= */

static list_t *target_expr(int i)
{
    return (i < g_lhs_cap && g_parse_lhs_syms[i] != NULL) ? g_parse_lhs_syms[i]->expr : NULL;
}

/* the variables left of '=' */
int act_assign_targets(token_t *tok)
{
    (void)tok;
    if (g_parse_work->count == 0) {
        parse_fatal_error(1026, "");
    }
    resolve_targets(g_parse_work, 1, 0, 1, 1);
    for (int i = 0; i < g_lhs_count; i++) {
        symbol_t *sym = g_parse_lhs_syms[i];
        if (!g_append) {
            if (sym->expr == NULL) {
                sym->expr = parse_list_alloc();
                g_last_lhs = sym;
            } else {
                parse_error_symbol(ERR_ALREADY_ASSIGNED, sym);
            }
        } else if (sym->expr == NULL) {
            sym->expr = parse_list_alloc();
        } else if (sym->expr->count != 0) {
            parse_append_op(sym->expr, TOK_OR);
        }
        g_lhs_skip[i] = 0;
    }
    parse_list_clear(g_parse_work);
    parse_work_pop();
    return 0;
}

/* add a token to the expression of every target */
static void append_to_targets(const token_t *tok)
{
    if (tok == NULL) {
        return;
    }
    for (int i = 0; i < g_lhs_count; i++) {
        if (!g_lhs_skip[i]) {
            parse_list_append(parse_token_copy(tok), target_expr(i));
        }
    }
}

/* operators and parentheses of an expression */
int act_expr_token(token_t *tok)
{
    switch (tok->type) {
    case TOK_EQEQ:
        break;
    case TOK_LPAREN:
        if (g_pending_not) {
            tok->flags |= TF_NEGATED;
        }
        g_pending_not = 0;
        break;
    case TOK_RPAREN:
        break;
    case TOK_FDEL:
        /* end of a generated condition: all targets take part again */
        g_else_chain = 0;
        for (int i = 0; i < g_lhs_count; i++) {
            g_lhs_skip[i] = 0;
        }
        parse_token_free(tok);
        return 0;
    case TOK_ODEL:
        g_else_chain = 1;
        parse_token_free(tok);
        return 0;
    default:
        g_parse_radix = TOK_RADIX_H;
        break;
    }
    append_to_targets(tok);
    parse_token_free(tok);
    return 0;
}

/* an operand that is a single variable goes to every target */
static int assign_single_variable(list_t *list)
{
    token_t *tok = list->head;
    symbol_t *sym;
    if (list->count == 1) {
        sym = parse_enter_symbol(parse_variable_name(tok), NULL, 1, g_parse_table);
    } else if (list->count == 2 && tok->next->type == TOK_EXTENSION) {
        sym = parse_enter_symbol(parse_variable_name(tok), parse_token_text(tok->next), 1, g_parse_table);
    } else {
        return 0;
    }
    tok->u.sym = sym;
    tok->flags |= TF_SYMBOL;
    sym->flags |= SF_USED;
    append_to_targets(tok);
    return 1;
}

/* an operand that is a list of variables: one per target.  In a
 * comparison (vars == vars) all of them go to the first target. */
static void assign_variables(const list_t *list)
{
    int at = 0;
    int count = 0;
    list_t *copy = parse_list_copy(list);
    if (copy == NULL) {
        parse_fatal_error(1003, "");
    }
    if (g_parse_last_token_type == TOK_EQEQ) {
        g_compare_stmt = 1;
    }
    if (assign_single_variable(copy)) {
        return;
    }
    token_t *tok = copy->head;
    while (tok != NULL) {
        token_t *next = tok->next;
        if (!g_compare_stmt && count >= g_lhs_count) {
            parse_error_at(ERR_SIZE_MISMATCH, parse_token_text(tok));
            break;
        }
        int skip = (at < g_lhs_cap) ? g_lhs_skip[at] : 0;
        if (!(tok->flags & TF_NUMBER)) {
            symbol_t *sym;
            if (tok->next != NULL && tok->next->type == TOK_EXTENSION) {
                sym = parse_enter_symbol(parse_variable_name(tok), parse_token_text(tok->next), 1, g_parse_table);
                next = next->next;
            } else {
                sym = parse_enter_symbol(parse_variable_name(tok), NULL, 1, g_parse_table);
            }
            tok->u.sym = sym;
            tok->flags |= TF_SYMBOL;
            sym->flags |= SF_USED;
        } else {
            uint32_t bit = tok->u.num->value & 1;
            tok->u.any = NULL;
            tok->u.bit = bit;
        }
        if (!skip) {
            parse_list_append(tok, target_expr(at));
        } else {
            parse_token_free(tok);
        }
        tok = next;
        if (!g_compare_stmt) {
            count++;
            at++;
        }
    }
    if (!g_compare_stmt && count < g_lhs_count) {
        size_message(g_lhs_count, count);
        parse_error_at(ERR_SIZE_MISMATCH, g_message);
    }
}

/* an operand that is a number: one bit per target.  Targets with
 * an index take the bit of that index, others take the bits from the right.
 * Inside a generated condition (".odel") a target whose bit is 0 drops out
 * until the condition ends (".fdel"). */
static void assign_number(const token_t *tok)
{
    number_t *num = tok->u.num;
    uint32_t value = num->value;
    uint32_t bit = 0;
    for (int i = g_lhs_count - 1; i >= 0; i--) {
        if (!g_lhs_skip[i]) {
            token_t *copy = parse_token_copy(tok);
            copy->flags &= TF_NUMBER | TF_NEGATED;
            int index = parse_name_index(g_parse_lhs_syms[i]->name);
            if (g_lhs_count == 1 || index < 0 || index > 31) {
                bit = num->value & 1;
            } else {
                bit = value >> index & 1;
            }
            copy->u.any = NULL;
            copy->u.bit = bit;
            parse_list_append(copy, target_expr(i));
        }
        if (g_else_chain && bit == 0) {
            g_lhs_skip[i] = 1;
        }
        num->value >>= 1;
    }
}

/* an operand (variable list or number) is complete; `tok` is the
 * token after it, which is read again */
int act_expr_operand(token_t *tok)
{
    if (g_parse_work->count == 1 && (g_parse_work->head->flags & TF_NUMBER)) {
        assign_number(g_parse_work->head);
    } else {
        assign_variables(g_parse_work);
    }
    parse_list_clear(&g_parse_numbers);
    parse_list_clear(g_parse_work);
    parse_work_pop();
    g_else_chain = 0;
    parse_list_push_front(tok, &g_parse_queue);
    return 0;
}

/* -------------------------------------------------------------------------
 * Comparisons: "name = vars == vars"
 * ------------------------------------------------------------------------- */

int parse_is_variable_token(const token_t *tok)
{
    return tok->type == TOK_IDENT || tok->type == TOK_IDENT_NUM;
}

/* open <design>.oed for writing, once */
static void open_oed(void)
{
    if (g_parse_oed_fp == NULL) {
        char *name = parse_make_file_name(parse_directory_prefix(g_parse_source_path), g_parse_source_path,
                                          ".oed");
        g_parse_oed_fp = parse_open_file(name, "wt");
        if (g_parse_oed_fp == NULL) {
            parse_fatal_error(ERR_OPEN, name);
        }
    }
}

static void oed_printf(const char *fmt, ...)
{
    char text[1024];
    va_list ap;
    if (g_parse_oed_fp == NULL) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    fputs(text, g_parse_oed_fp);
}

static const char *operand_text(const token_t *tok)
{
    if (tok->flags & TF_SYMBOL) {
        return tok->u.sym->name;
    }
    return (tok->flags & TF_NUMBER) ? NULL : tok->u.text;
}

/* name.cmp = vars == vars becomes a PROPERTY line in the .oed
 * file instead of an equation */
static void write_compare_property(const symbol_t *sym)
{
    int balance = 0;
    token_t *tok;
    open_oed();
    oed_printf("PROPERTY INTEL @EQ %s.%s = ", or_null(sym->name), or_null(sym->ext));
    oed_printf("[ ");
    for (tok = sym->expr->head; tok != NULL && tok->type != TOK_EQEQ; tok = tok->next) {
        if (balance > 0) {
            oed_printf(", ");
        }
        if (parse_is_variable_token(tok)) {
            if (operand_text(tok) != NULL) {
                oed_printf("%s", operand_text(tok));
            }
            balance++;
        } else {
            oed_printf("%s", or_null(operand_text(tok)));
        }
    }
    if (tok != NULL) {
        oed_printf(" ] == [");
        for (tok = tok->next; tok != NULL; tok = tok->next) {
            if (parse_is_variable_token(tok)) {
                if (operand_text(tok) != NULL) {
                    oed_printf("%s", operand_text(tok));
                }
                balance--;
            } else {
                oed_printf("%s", or_null(operand_text(tok)));
            }
            if (balance > 0) {
                oed_printf(", ");
            }
        }
    }
    oed_printf("]\n");
    if (balance != 0) {
        char text[600];
        snprintf(text, sizeof(text), "Warning: Different size of vectors to be compared: %s.%s\n",
                 or_null(sym->name), or_null(sym->ext));
        parse_trace_puts(text);
    }
}

/* rewrite "a1 a0 == b1 b0 rest" in the expression of `sym` as
 * "( !(b1 $ a1) & !(b0 $ a0) ) rest" */
static void lower_compare(symbol_t *sym)
{
    list_t *expr = sym->expr;
    token_t *tok;
    token_t *last_other = NULL;
    token_t *left;

    for (tok = expr->head; tok != NULL && tok->type != TOK_EQEQ; tok = tok->next) {
        if (!parse_is_variable_token(tok)) {
            last_other = tok;
        }
    }
    if (last_other == NULL) {
        left = expr->head;
        expr->head = NULL;
        expr->tail = NULL;
    } else {
        left = last_other->next;
        last_other->next = NULL;
        expr->tail = last_other;
    }
    if (tok == NULL) {
        parse_fatal_error(ERR_COMPARE_SIZE, sym->name);
    }
    token_t *right = tok->next;
    token_t *rest = right;
    int right_count = 0;
    int left_count = 0;
    for (; rest != NULL && parse_is_variable_token(rest); rest = rest->next) {
        right_count++;
    }
    for (tok = left; tok != NULL && parse_is_variable_token(tok); tok = tok->next) {
        left_count++;
    }
    if (left_count != right_count) {
        parse_fatal_error(ERR_COMPARE_SIZE, sym->name);
    }
    parse_append_op(expr, TOK_LPAREN);
    for (int i = 0; i < right_count; i++) {
        token_t *next_left = left->next;
        token_t *next_right = right->next;
        token_t *open = parse_token_new_op(TOK_LPAREN);
        open->flags = TF_NEGATED;
        parse_list_append(open, expr);
        parse_list_append(right, expr);
        parse_append_op(expr, TOK_XOR);
        parse_list_append(left, expr);
        parse_append_op(expr, TOK_RPAREN);
        if (i != right_count - 1) {
            parse_append_op(expr, TOK_AND);
        }
        left = next_left;
        right = next_right;
    }
    parse_append_op(expr, TOK_RPAREN);
    while (rest != NULL) {
        token_t *next = rest->next;
        parse_list_append(rest, expr);
        rest = next;
    }
}

/* the expression of a statement is complete */
void parse_run_statement_end(void)
{
    g_append = 0;
    if (g_compare_stmt == 1 && g_last_lhs != NULL && g_last_lhs->expr != NULL) {
        if (strcasecmp(g_last_lhs->ext, "cmp") == 0) {
            write_compare_property(g_last_lhs);
            parse_list_free(g_last_lhs->expr);
            g_last_lhs->expr = NULL;
        } else {
            lower_compare(g_last_lhs);
        }
    }
    g_compare_stmt = 0;
}

int act_statement_end(token_t *tok)
{
    (void)tok;
    parse_run_statement_end();
    return 0;
}

/* at the ';' the text of the function calls of the statement is
 * queued, to be parsed next */
int act_flush_call_results(token_t *tok)
{
    if (g_parse_call_text.count != 0) {
        parse_list_concat(&g_parse_queue, &g_parse_call_text);
        parse_list_init(&g_parse_call_text);
        parse_token_free(tok);
    }
    return 0;
}

/* "vars;" */
int act_statement_no_expr(token_t *tok)
{
    parse_error_at(ERR_MISSING_SYMBOL, "=");
    parse_run_statement_end();
    return act_flush_call_results(tok);
}

/* =========================================================================
 * DECLARE and PROPERTY: copied to the .oed file
 * ========================================================================= */

/* next word of declare.def; '#' starts a comment */
static int read_def_word(FILE *fp, char *word, size_t size)
{
    int c;
    size_t n = 0;
    for (;;) {
        do {
            c = parse_read_char(fp);
            if (c == EOF) {
                return -1;
            }
        } while (c == ' ' || c == '\t' || c == '\n');
        if (c != '#') {
            break;
        }
        do {
            c = parse_read_char(fp);
        } while (c != '\n' && c != EOF);
        if (c == EOF) {
            return -1;
        }
    }
    if (!is_alnum(c) && c != '&' && c != '_') {
        word[0] = (char)c;
        word[1] = '\0';
        return c;
    }
    while (is_alnum(c) || c == '&' || c == '_') {
        if (n + 1 < size) {
            word[n++] = (char)c;
        }
        c = parse_read_char(fp);
    }
    word[n] = '\0';
    parse_unread_char(c, fp);
    return 0x100;
}

/* read declare.def from the work directory: entity classes and their
 * items */
static void load_declare_defs(void)
{
    char path[PATH_MAX];
    char word[80];
    size_t len = strlen(g_parse_work_dir);
    snprintf(path, sizeof(path) - 16, "%s", g_parse_work_dir);
    if (len == 0 || g_parse_work_dir[len - 1] != '\\') {
        strcat(path, "\\");
    }
    strcat(path, "declare.def");
    FILE *fp = parse_open_file(path, "r");
    if (fp == NULL) {
        parse_fatal_error(ERR_OPEN, path);
    }
    do {
        if (read_def_word(fp, word, sizeof(word)) == -1) {
            break;
        }
        lincupl_strcpy_trunc(g_declare[g_declare_count].name, sizeof(g_declare[0].name), word);
        read_def_word(fp, word, sizeof(word));
        g_declare[g_declare_count].items = NULL;
        int result;
        do {
            read_def_word(fp, word, sizeof(word));
            declare_item_t *item = parse_alloc_tracked(sizeof(*item), "");
            lincupl_strcpy_trunc(item->name, sizeof(item->name), word);
            read_def_word(fp, word, sizeof(word));
            item->kind = (int)strtol(word, NULL, 10);
            if (item->kind == 3) {
                read_def_word(fp, word, sizeof(word));
                item->arg = (int)strtol(word, NULL, 10);
            }
            declare_item_t **link = &g_declare[g_declare_count].items;
            while (*link != NULL) {
                link = &(*link)->next;
            }
            *link = item;
            result = read_def_word(fp, word, sizeof(word));
            if (result == -1) {
                /* a file that ends inside an item list is reported as
                 * "out of memory" */
                parse_fatal_error(ERR_OUT_OF_MEMORY, "");
            }
        } while (result != ';');
        g_declare_count++;
    } while (g_declare_count != DECLARE_MAX);
    parse_close_file(fp);
}

/* is `entity` an item of the DECLARE class `name` in declare.def */
static int declare_known(const char *name, const char *entity)
{
    if (g_declare_count == 0) {
        load_declare_defs();
    }
    for (int i = 0; i < g_declare_count; i++) {
        if (strcmp(g_declare[i].name, name) == 0) {
            for (declare_item_t *item = g_declare[i].items; item != NULL; item = item->next) {
                if (strcmp(item->name, entity) == 0) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* DECLARE name: starts a DECLARE line in the .oed file */
int act_declare_name(token_t *tok)
{
    open_oed();
    oed_printf("DECLARE %s ", or_null(parse_token_text(tok)));
    snprintf(g_declare_name, sizeof(g_declare_name), "%s",
             parse_token_text(tok) != NULL ? parse_token_text(tok) : "");
    parse_token_free(tok);
    return 0;
}

/* the entity of a DECLARE; it must be listed in declare.def */
int act_declare_entity(token_t *tok)
{
    const char *text = parse_token_text(tok);
    oed_printf("%s :  ", or_null(text));
    if (!declare_known(g_declare_name, text != NULL ? text : "")) {
        parse_fatal_error(ERR_DECLARE_ENTITY, text);
    }
    parse_token_free(tok);
    return 0;
}

/* the variables of a DECLARE as "[ a b ... ] " */
int act_declare_vars(token_t *tok)
{
    (void)tok;
    oed_printf("[ ");
    for (const token_t *var = g_parse_work->head; var != NULL; var = var->next) {
        oed_printf("%s ", or_null(parse_token_text(var)));
    }
    oed_printf("] ");
    parse_work_pop();
    return 0;
}

static int oed_word(token_t *tok, const char *text)
{
    oed_printf("%s", text);
    parse_token_free(tok);
    return 0;
}

int act_declare_init(token_t *tok) { return oed_word(tok, "INIT_TO "); }
int act_declare_end(token_t *tok) { return oed_word(tok, ";\n"); }
int act_declare_lparen(token_t *tok) { return oed_word(tok, "("); }
int act_declare_rparen(token_t *tok) { return oed_word(tok, ")"); }

/* any other word of a DECLARE */
int act_declare_word(token_t *tok)
{
    oed_printf("%s ", or_null(parse_token_text(tok)));
    parse_token_free(tok);
    return 0;
}

/* PROPERTY name: the name is written before each line of the body */
int act_property_name(token_t *tok)
{
    open_oed();
    snprintf(g_declare_name, sizeof(g_declare_name), "%s",
             parse_token_text(tok) != NULL ? parse_token_text(tok) : "");
    g_property_start = 1;
    parse_token_free(tok);
    fflush(stdout);
    return 0;
}

/* a word of the PROPERTY body; ';' ends a line */
int act_property_word(token_t *tok)
{
    const char *text = parse_token_text(tok);
    if (g_property_start) {
        oed_printf("PROPERTY  %s ", g_declare_name);
        g_property_start = 0;
    }
    oed_printf("%s ", or_null(text));
    if (text != NULL && strcmp(text, ";") == 0) {
        oed_printf("\n");
        g_property_start = 1;
    }
    parse_token_free(tok);
    return 0;
}

int act_property_end(token_t *tok)
{
    (void)tok;
    oed_printf("\n");
    return 0;
}

