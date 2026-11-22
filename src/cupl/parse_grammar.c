/*
 * parse - the table-driven parser: keyword and punctuation lookup and the
 * driver that runs the grammar of parse_tables.h, calling the semantic
 * actions as rules match.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "parse_int.h"

typedef struct {
    uint16_t token;
    int16_t next;
    int16_t sub;
    uint16_t kind;
    action_fn action;
} rule_t;

/* rule kinds (see parse_rules) */
enum {
    RK_TOKEN = 0,
    RK_GROUP = 1,
    RK_ACCEPT = 2,
    RK_REJECT = 3,
    RK_SYNTAX_REJECT = 4,
    RK_OR_ACCEPT = 5,
    RK_OR_REJECT = 6,
    RK_REQUIRED = 7,
    RK_OR_RESTART = 8,
    RK_REQUIRED_END = 9,
    RK_INTERNAL = 10,
    RK_GROUP_REQUIRED = 11,
    RK_OR_COMPLAIN = 12,
    RK_GROUP_COMPLAIN = 13,
    RK_TOKEN_ACTION = 14,
    RK_ALWAYS = 15
};

#include "parse_tables.h"

#define KEYWORD_COUNT ((int)(sizeof(k_keywords) / sizeof(k_keywords[0])))
#define PUNCT_COUNT ((int)(sizeof(k_punctuation) / sizeof(k_punctuation[0])))

static int g_recovering;               /* skipping input after a syntax error */
int g_parse_last_token_type;

void parse_grammar_reset(void)
{
    g_recovering = 0;
    g_parse_last_token_type = 0;
}

/* name of a keyword or operator */
const char *parse_keyword_name(int id)
{
    for (int i = 0; i < KEYWORD_COUNT; i++) {
        if (k_keywords[i].id == id) {
            return k_keywords[i].name;
        }
    }
    return "keyword or operator";
}

/* binary search of the keyword table.  One-letter words are
 * never keywords. */
const keyword_t *parse_find_keyword(const char *text)
{
    char lower[NAME_BUF_SIZE];
    if (text == NULL || strlen(text) <= 1) {
        return NULL;
    }
    parse_copy_lower(lower, sizeof(lower), text);
    int low = 0;
    int high = KEYWORD_COUNT - 1;
    do {
        int mid = low + (high - low) / 2;
        int order = strcmp(lower, k_keywords[mid].name);
        if (order < 0) {
            high = mid - 1;
        } else if (order > 0) {
            low = mid + 1;
        } else {
            return &k_keywords[mid];
        }
    } while (low <= high);
    return NULL;
}

/* linear search of a keyword table, ignoring case */
const keyword_t *parse_find_in_table(const keyword_t *table, int count, const char *text)
{
    char lower[NAME_BUF_SIZE];
    parse_copy_lower(lower, sizeof(lower), text);
    for (int i = 0; i < count; i++) {
        if (strcmp(lower, table[i].name) == 0) {
            return &table[i];
        }
    }
    return NULL;
}

/* the punctuation entry of `text`, NULL if there is none */
const keyword_t *parse_find_punctuation(const char *text)
{
    return parse_find_in_table(k_punctuation, PUNCT_COUNT, text);
}

/* =========================================================================
 * Parser
 * ========================================================================= */

/* abandon the statement after a syntax error */
static void recover(const token_t *tok)
{
    int at_end = tok->type == TOK_SEMI || tok->type == TOK_EOF;
    parse_run_statement_end();
    if (!at_end) {
        parse_lex_recover(g_parse_name_buf);
    }
    parse_work_reset();
    g_recovering = 1;
}

/* run the group of rules that starts at `rule` (its header) on
 * the token `tok` and the tokens after it.
 *
 * Rules are tried in order.  When one matches, its action is called and
 * parsing continues at the rule `next` (relative to the group header) with
 * the next token.  What happens when a rule does not match depends on its
 * kind; by default the following rule is tried.  The return value is 1 if
 * the group accepted its input and 0 if it did not (the token is then still
 * the caller's to match). */
static int parse_rules(const rule_t *rule, token_t *tok)
{
    int after_dot = 0;
    int base = rule->sub;

    if (rule->action != NULL && parse_actions_enabled()) {
        rule->action(tok);
    }
    rule++;
    for (;;) {
        int matched = 0;
        int restart = 0;
        int terminal = 0;
        int skip_action = 0;

        while (!matched) {
            if (rule->token == TOK_WORD
                && (tok->type == TOK_NUMBER || tok->type == TOK_IDENT || tok->type == TOK_IDENT_NUM)) {
                tok->type = TOK_WORD;
            }
            switch (rule->kind) {
            case RK_GROUP:
            case RK_GROUP_REQUIRED:
            case RK_GROUP_COMPLAIN:
                matched = parse_rules(&k_rules[rule->sub], tok);
                break;
            case RK_ACCEPT:
                parse_list_push_front(tok, &g_parse_queue);
                return 1;
            case RK_REJECT:
                return 0;
            case RK_SYNTAX_REJECT:
                parse_error_at(ERR_SYNTAX, parse_token_text(tok));
                return 0;
            case RK_INTERNAL:
                parse_fatal_error(1010, "");
                break;
            case RK_TOKEN_ACTION:
                if (tok->type == rule->token && rule->action != NULL && parse_actions_enabled()) {
                    matched = rule->action(tok);
                }
                break;
            case RK_ALWAYS:
                if (tok->type == TOK_EOF) {
                    parse_fatal_error(ERR_SYNTAX, "unexpected end of file");
                }
                matched = 1;
                break;
            default:
                terminal = 1;
                if (tok->type == rule->token) {
                    matched = 1;
                }
                break;
            }
            if (g_recovering) {
                /* unwind to the top group, which then simply goes on */
                if (base != 0) {
                    return 0;
                }
                g_recovering = 0;
                skip_action = 1;
                break;
            }
            if (tok->type != TOK_EOF && rule->token == TOK_ANY) {
                matched = 1;
            }
            if (matched) {
                break;
            }
            terminal = 0;
            switch (rule->kind) {
            case RK_OR_ACCEPT:
                return 1;
            case RK_OR_REJECT:
                return 0;
            case RK_REQUIRED:
            case RK_REQUIRED_END:
                parse_error_at(ERR_MISSING_SYMBOL, parse_keyword_name((int16_t)rule->token));
                recover(tok);
                return 1;
            case RK_OR_RESTART:
                parse_error_at(ERR_SYNTAX, parse_token_text(tok));
                matched = 1;
                restart = 1;
                break;
            case RK_GROUP_REQUIRED:
                parse_error_at(ERR_SYNTAX, parse_token_text(tok));
                recover(tok);
                return 1;
            case RK_OR_COMPLAIN:
            case RK_GROUP_COMPLAIN:
                parse_error_at(ERR_SYNTAX, parse_token_text(tok));
                rule++;
                break;
            default:
                rule++;
                break;
            }
        }
        if (!skip_action) {
            if (rule->kind != RK_TOKEN_ACTION && rule->action != NULL && parse_actions_enabled()) {
                rule->action(tok);
            }
            if (rule->action == NULL && terminal) {
                parse_token_free(tok);
            }
            if (rule->kind == RK_REQUIRED_END) {
                return 1;
            }
        }

        tok = parse_next_token();
        g_parse_last_token_type = tok->type;
        /* the word after a '.' is an extension, never an indexed name */
        if (!after_dot && tok->type == TOK_DOT) {
            after_dot = 1;
        } else if (after_dot && tok->type == TOK_DOT) {
            after_dot = 0;
        } else if (after_dot && (tok->type == TOK_IDENT || tok->type == TOK_IDENT_NUM)) {
            tok->type = TOK_IDENT;
            after_dot = 0;
        }
        rule = restart ? &k_rules[base + 1] : &k_rules[base + rule->next];
    }
}

/* parse the whole input */
void parse_grammar_run(void)
{
    parse_rules(&k_rules[0], parse_next_token());
}
