/*
 * parse - tokens, token lists and numbers.
 *
 * Tokens and list headers come from pools; lists are singly linked chains
 * of tokens with a head/tail/count header.  The work list stack holds the
 * lists the semantic actions build.  Numbers carry a value and a mask of
 * don't-care bits.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "parse_int.h"

static token_t *g_free_tokens;
static list_t *g_free_lists;

static list_t g_work_stack[WORK_STACK_DEPTH];
list_t *g_parse_work;                  /* top of the stack */
int g_parse_work_level;

void parse_tokens_reset(void)
{
    g_free_tokens = NULL;
    g_free_lists = NULL;
    memset(g_work_stack, 0, sizeof(g_work_stack));
    parse_work_reset();
}

/* -------------------------------------------------------------------------
 * Token and list pools
 *
 * Tokens, list headers and symbols come from pools with a last-in first-out
 * free list, so that a token freed and allocated again is the same object.
 * ------------------------------------------------------------------------- */

/* a cleared token from the pool; the pool grows TOKEN_CHUNK at a time */
token_t *parse_token_alloc(void)
{
    if (g_free_tokens == NULL) {
        token_t *chunk = parse_alloc_tracked(TOKEN_CHUNK * sizeof(*chunk), "parsing");
        for (int i = 0; i < TOKEN_CHUNK - 1; i++) {
            chunk[i].next = &chunk[i + 1];
        }
        g_free_tokens = chunk;
    }
    token_t *tok = g_free_tokens;
    g_free_tokens = tok->next;
    tok->type = 0;
    tok->flags = 0;
    tok->u.any = NULL;
    tok->next = NULL;
    return tok;
}

void parse_token_free(token_t *tok)
{
    if (tok != NULL) {
        tok->next = g_free_tokens;
        g_free_tokens = tok;
    }
}

void parse_list_init(list_t *list)
{
    if (list != NULL) {
        list->head = NULL;
        list->tail = NULL;
        list->count = 0;
        list->flags = 0;
    }
}

/* an empty list header from the pool */
list_t *parse_list_alloc(void)
{
    if (g_free_lists == NULL) {
        list_t *chunk = parse_alloc_tracked(LIST_CHUNK * sizeof(*chunk), "parsing");
        for (int i = 0; i < LIST_CHUNK - 1; i++) {
            chunk[i].next_free = &chunk[i + 1];
        }
        g_free_lists = chunk;
    }
    list_t *list = g_free_lists;
    g_free_lists = list->next_free;
    parse_list_init(list);
    list->next_free = NULL;
    return list;
}

void parse_list_free(list_t *list)
{
    if (list != NULL) {
        list->next_free = g_free_lists;
        g_free_lists = list;
    }
}

/* a new header for the same tokens */
list_t *parse_list_clone_header(const list_t *list)
{
    if (list == NULL) {
        return NULL;
    }
    list_t *copy = parse_list_alloc();
    copy->head = list->head;
    copy->tail = list->tail;
    copy->count = list->count;
    copy->flags = list->flags;
    return copy;
}

void parse_list_push_front(token_t *tok, list_t *list)
{
    if (list == NULL) {
        return;
    }
    if (list->head == NULL) {
        list->head = tok;
        list->tail = tok;
        tok->next = NULL;
    } else {
        tok->next = list->head;
        list->head = tok;
    }
    list->count++;
}

/* Appending "no token" (see append_state_match) is accepted on purpose:
 * the chain ends at the old tail, the count still grows, and what is
 * appended later hangs off a token that cannot be reached from the head.
 * The statements generated for state 0 with flag o depend on this. */
void parse_list_append(token_t *tok, list_t *list)
{
    static token_t lost;
    if (list == NULL) {
        return;
    }
    if (tok == NULL) {
        if (list->head != NULL) {
            list->tail->next = NULL;
            list->tail = &lost;
        }
        list->count++;
        return;
    }
    if (list->head == NULL) {
        list->head = tok;
        list->tail = tok;
    } else {
        list->tail->next = tok;
        list->tail = tok;
    }
    list->count++;
    tok->next = NULL;
}

/* free the tokens of a list.  The count must match the chain. */
void parse_list_clear(list_t *list)
{
    if (list == NULL) {
        return;
    }
    token_t *tok = list->head;
    while (tok != NULL) {
        token_t *next = tok->next;
        parse_token_free(tok);
        list->count--;
        tok = next;
    }
    list->head = NULL;
    list->tail = NULL;
    if (list->count != 0) {
        parse_fatal_error(1017, "");
    }
}

void parse_list_destroy(list_t *list)
{
    parse_list_clear(list);
    parse_list_free(list);
}

token_t *parse_list_pop_front(list_t *list)
{
    if (list == NULL || list->head == NULL) {
        return NULL;
    }
    token_t *tok = list->head;
    if (list->head == list->tail) {
        list->head = NULL;
        list->tail = NULL;
    } else {
        list->head = tok->next;
    }
    list->count--;
    return tok;
}

token_t *parse_list_pop_back(list_t *list)
{
    if (list == NULL || list->tail == NULL) {
        return NULL;
    }
    token_t *tok = list->tail;
    if (list->head == list->tail) {
        list->head = NULL;
        list->tail = NULL;
    } else {
        token_t *prev = list->head;
        while (prev != NULL && prev->next != tok) {
            prev = prev->next;
        }
        if (prev == NULL) {
            parse_fatal_error(1017, "");
        }
        list->tail = prev;
        prev->next = NULL;
    }
    list->count--;
    return tok;
}

/* append the tokens of `src` to `dst`.  Afterwards both headers
 * describe the combined list. */
void parse_list_concat(list_t *dst, list_t *src)
{
    if (dst == NULL || src == NULL) {
        return;
    }
    if (dst->head == NULL) {
        dst->head = src->head;
        dst->tail = src->tail;
        dst->count = src->count;
    } else if (src->head != NULL) {
        dst->tail->next = src->head;
        dst->tail = src->tail;
        dst->count = (int16_t)(dst->count + src->count);
    }
    src->head = dst->head;
    src->tail = dst->tail;
    src->count = dst->count;
}

token_t *parse_token_copy(const token_t *tok)
{
    if (tok == NULL) {
        return NULL;
    }
    token_t *copy = parse_token_alloc();
    copy->flags = tok->flags;
    copy->type = tok->type;
    copy->u = tok->u;
    return copy;
}

/* a new list with copies of all tokens */
list_t *parse_list_copy(const list_t *list)
{
    if (list == NULL) {
        return NULL;
    }
    list_t *copy = parse_list_alloc();
    for (const token_t *tok = list->head; tok != NULL; tok = tok->next) {
        parse_list_append(parse_token_copy(tok), copy);
    }
    return copy;
}

/* a keyword or operator token */
token_t *parse_token_new_op(int id)
{
    token_t *tok = parse_token_alloc();
    tok->flags = 0;
    tok->type = (uint8_t)id;
    tok->u.text = parse_keyword_name(id);
    return tok;
}

/* a token of the given type whose value is the interned text */
token_t *parse_token_new_text(const char *text, int type)
{
    token_t *tok = parse_token_alloc();
    tok->u.text = parse_intern(text);
    tok->flags = 0;
    tok->type = (uint8_t)type;
    return tok;
}

void parse_append_op(list_t *list, int id)
{
    parse_list_append(parse_token_new_op(id), list);
}

void parse_list_set_negated(list_t *list, int negated)
{
    for (token_t *tok = list->head; tok != NULL; tok = tok->next) {
        if (negated) {
            tok->flags |= TF_NEGATED;
        } else {
            tok->flags &= (uint8_t)~TF_NEGATED;
        }
    }
}

static void insert_copy_after(token_t *at, const token_t *model, list_t *list)
{
    token_t *copy = parse_token_alloc();
    copy->flags = model->flags;
    copy->type = model->type;
    copy->u = model->u;
    copy->next = at->next;
    at->next = copy;
    list->count++;
}

/* put a copy of `model` between all tokens of the list */
void parse_list_insert_between(const token_t *model, list_t *list)
{
    if (model == NULL || list == NULL || list->head == NULL) {
        return;
    }
    token_t *tok = list->head;
    while (tok->next != NULL) {
        token_t *next = tok->next;
        insert_copy_after(tok, model, list);
        tok = next;
    }
}

/* give every token that has no token of type `type` behind it a
 * copy of `model` */
void parse_list_insert_missing(const token_t *model, int type, list_t *list)
{
    if (model == NULL || list == NULL) {
        return;
    }
    token_t *tok = list->head;
    while (tok != NULL) {
        token_t *next = tok->next;
        if (tok->type != type && (next == NULL || next->type != type)) {
            insert_copy_after(tok, model, list);
            if (next == NULL) {
                list->tail = tok->next;
            }
        }
        tok = next;
    }
}

/* put a copy of `model` between each pair of tokens whose types
 * match (or, with `unless`, do not match) `first` and `second`; -1 matches
 * any type. */
void parse_list_insert_where(const token_t *model, int first, int second, int unless, list_t *list)
{
    if (model == NULL || list == NULL) {
        return;
    }
    token_t *tok = list->head;
    while (tok != NULL) {
        token_t *next = tok->next;
        if (next != NULL) {
            int match = (first < 0 || tok->type == first) && (second < 0 || next->type == second);
            if (match != (unless != 0)) {
                insert_copy_after(tok, model, list);
            }
        }
        tok = next;
    }
}

/* the n-th token that is not an extension */
token_t *parse_list_nth_variable(const list_t *list, int n)
{
    if (list != NULL) {
        int i = 0;
        for (token_t *tok = list->head; tok != NULL; tok = tok->next) {
            if (tok->type != TOK_EXTENSION && i++ == n) {
                return tok;
            }
        }
    }
    return NULL;
}

token_t *parse_list_find_type(const list_t *list, int type)
{
    if (list == NULL) {
        return NULL;
    }
    for (token_t *tok = list->head; tok != NULL; tok = tok->next) {
        if (tok->type == type) {
            return tok;
        }
    }
    return NULL;
}

void parse_list_replace_type(int from, int to, list_t *list)
{
    if (list != NULL) {
        for (token_t *tok = list->head; tok != NULL; tok = tok->next) {
            if (tok->type == from) {
                tok->type = (uint8_t)to;
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * Work list stack
 * ------------------------------------------------------------------------- */

void parse_work_reset(void)
{
    g_parse_work = g_work_stack;
    g_parse_work_level = 0;
}

void parse_work_pop(void)
{
    if (g_parse_work == g_work_stack) {
        parse_fatal_error(1019, "");
    }
    g_parse_work--;
    g_parse_work_level--;
}

void parse_work_push(void)
{
    g_parse_work++;
    if (g_parse_work == g_work_stack + WORK_STACK_DEPTH) {
        parse_fatal_error(1021, "");
    }
    parse_list_init(g_parse_work);
    g_parse_work_level++;
}

/* -------------------------------------------------------------------------
 * Numbers
 * ------------------------------------------------------------------------- */

number_t *parse_number_alloc(void)
{
    return parse_alloc_tracked(sizeof(number_t), "parsing");
}

/* convert number text in the current radix; x digits are
 * don't-cares */
number_t *parse_number_text(const char *text, int radix)
{
    number_t *num = parse_number_alloc();
    uint32_t base;
    if (text == NULL || text[0] == '\0') {
        return num;
    }
    switch (radix) {
    case TOK_RADIX_B: base = 2; break;
    case TOK_RADIX_D: base = 10; break;
    case TOK_RADIX_H: base = 16; break;
    case TOK_RADIX_O: base = 8; break;
    default:
        parse_error_at(1008, text);
        return num;
    }
    uint32_t value = 0;
    uint32_t mask = 0;
    for (const char *p = text; *p != '\0'; p++) {
        uint32_t digit = 0;
        uint32_t dontcare = 0;
        int c = (unsigned char)*p;
        if (is_alpha(c)) {
            int upper = to_upper(c);
            if (upper == 'X') {
                if (radix == TOK_RADIX_D) {
                    parse_warning_at(ERR_DECIMAL_DONTCARE, "");
                } else {
                    dontcare = base - 1;
                }
            } else {
                digit = (uint32_t)(upper - 0x37);
            }
        } else if (is_digit(c)) {
            digit = (uint32_t)(c - '0');
        } else {
            digit = base + 1;
        }
        if (digit >= base) {
            parse_error_at(ERR_BAD_NUMBER, text);
        }
        value = value * base + digit;
        mask = mask * base + dontcare;
    }
    num->value = value;
    num->dontcare = mask;
    return num;
}

/* lower-case hex text in a static buffer */
const char *parse_format_hex(uint32_t value)
{
    static char text[16];
    snprintf(text, sizeof(text), "%lx", (unsigned long)value);
    return text;
}

/* the radix in which the don't-care bits of a number fall on
 * digit boundaries */
int parse_number_radix(const number_t *num)
{
    uint32_t mask = num->dontcare;
    if (mask == 0) {
        return 16;
    }
    int hex = 1;
    for (int i = 0; i < 8; i++, mask >>= 4) {
        if ((mask & 0xf) != 0 && (mask & 0xf) != 0xf) {
            hex = 0;
            break;
        }
    }
    int octal = 1;
    mask = num->dontcare;
    for (int i = 0; i < 10; i++, mask >>= 3) {
        if ((mask & 7) != 0 && (mask & 7) != 7) {
            octal = 0;
            break;
        }
    }
    return hex ? 16 : (octal ? 8 : 2);
}

/* number text with x for don't-care digits, in a static buffer */
static const char *format_number(const number_t *num, int radix)
{
    static char text[40];
    uint32_t bits = num->value | num->dontcare;
    unsigned shift;
    if (radix == 2) {
        if (bits == 0) {
            strcpy(text, "0");
        } else {
            for (int i = 0; i < 32; i++) {
                text[i] = (bits << i & 0x80000000u) ? '1' : '0';
            }
            text[32] = '\0';
        }
        shift = 1;
    } else if (radix == 8) {
        snprintf(text, sizeof(text), "%lo", (unsigned long)bits);
        shift = 3;
    } else {
        snprintf(text, sizeof(text), "%lx", (unsigned long)bits);
        shift = 4;
    }
    int len = (int)strlen(text);
    unsigned bit = 0;
    for (int i = 0; i < len; i++) {
        if (num->dontcare & (1u << (bit & 0x1f))) {
            text[len - 1 - i] = 'x';
        }
        bit = (bit + shift) & 0xff;
    }
    return text;
}

/* compare two numbers with their don't-care bits set */
int parse_compare_numbers(const number_t *a, const number_t *b)
{
    uint32_t x = a->value | a->dontcare;
    uint32_t y = b->value | b->dontcare;
    return x < y ? -1 : (x == y ? 0 : 1);
}

/* a text token for the number in `tok` */
token_t *parse_token_new_number_text(const token_t *tok, int type)
{
    token_t *copy = parse_token_alloc();
    if (tok->flags & TF_NUMBER) {
        copy->u.text = parse_intern(format_number(tok->u.num, parse_number_radix(tok->u.num)));
    }
    copy->flags = 0;
    copy->type = (uint8_t)type;
    return copy;
}

token_t *parse_token_new_hex(uint32_t value, int type)
{
    return parse_token_new_text(parse_format_hex(value), type);
}

/* append 'h'<value> */
void parse_append_hex_number(uint32_t value, list_t *list)
{
    parse_append_op(list, TOK_QUOTE);
    parse_list_append(parse_token_new_text("h", TOK_IDENT), list);
    parse_append_op(list, TOK_QUOTE);
    parse_list_append(parse_token_new_hex(value, TOK_NUMBER), list);
}

/* append '<radix>'<number> */
void parse_append_number(const number_t *num, list_t *list)
{
    int radix = parse_number_radix(num);
    parse_append_op(list, TOK_QUOTE);
    parse_list_append(parse_token_new_text(radix == 2 ? "b" : (radix == 8 ? "o" : "h"), TOK_IDENT), list);
    parse_append_op(list, TOK_QUOTE);
    parse_list_append(parse_token_new_text(format_number(num, radix), TOK_NUMBER), list);
}

