/*
 * expand_input.c - reading the CB file in the expand phase.
 *
 * Every design section of CB<pid> starts with a header and the symbol
 * table; the equations follow as streams of infix tokens, which an
 * operator-precedence parser turns into expression trees.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "expand_int.h"

/* Parser state */
static int g_paren_depth;
static int g_line_tokens;
static node_t **g_operands;
static int g_operand_top;
static int g_operand_cap;
static op_entry_t *g_ops;
static int g_op_top;
static int g_op_cap;

/* -------------------------------------------------------------------------
 * CB file: section header and symbol table
 * ------------------------------------------------------------------------- */

static void read_symbol(symbol_t *sym)
{
    cf_symbol_t rec;
    if (!cf_read_symbol(expand_in, &rec, CF_CB_SYMBOL_SIZE)) {
        expand_fatal_error(ERR_DISK_READ, expand_in_name);
    }
    memcpy(sym->name, rec.name, SYMBOL_NAME_SIZE);
    memcpy(sym->ext, rec.ext, SYMBOL_EXT_SIZE);
    sym->pin = rec.pin;
    sym->kind = rec.type;
    sym->flags = rec.flags;
    memcpy(sym->attr, rec.attr, sizeof(sym->attr));
    sym->polarity = rec.polarity;
    sym->attr2 = rec.attr2;
}

/* Read the header and the symbol table of the next design section.  The
 * table ends with a SYM_END record, which is not counted. */
void expand_read_section_header(void)
{
    if (expand_in == NULL) {
        phase_temp_name(expand_in_name, expand_args[3]);
        expand_in = expand_open_file(expand_in_name, "rb");
        if (expand_in == NULL) {
            expand_fatal_error(ERR_OPEN, expand_in_name);
        }
    }
    if (!cf_read_header(expand_in, expand_header)) {
        expand_fatal_error(ERR_DISK_READ, expand_in_name);
    }
    if (rd16(expand_header + CF_HDR_MAGIC) != CF_MAGIC_CB) {
        expand_report_error(ERR_INVALID_FILE, "intermediate file");
        expand_fatal_error(-1, "");
    }
    expand_header[CF_HDR_EXTRA - 1] = '\0'; /* keep the device name terminated */
    unsigned extra = cf_header_extra(expand_header);
    if (extra != 0) {
        expand_header_extra = expand_alloc_tracked(extra, "symbol table");
        if (fread(expand_header_extra, extra, 1, expand_in) == 0) {
            expand_fatal_error(ERR_DISK_READ, expand_in_name);
        }
    }

    int count = (int)rd16(expand_header + CF_HDR_SYMBOLS);
    expand_symbol_count = count;
    expand_symbol_alloc = count > 0 ? count : 1;
    expand_symbols = expand_alloc_tracked((size_t)expand_symbol_alloc * sizeof(symbol_t), "symbol table");
    symbol_t *sym = expand_symbols;
    read_symbol(sym);
    for (int n = 1; (sym->kind & 0x0f) != SYM_END; n++) {
        if (n >= count) {
            expand_report_error(ERR_INTERNAL_SYMBOL_COUNT, "");
            break;
        }
        sym->pin_fn = -1;
        sym->node_fn = -1;
        sym->out_fn = -1;
        sym++;
        read_symbol(sym);
    }
    expand_symbol_count = count - 1;
}

/* -------------------------------------------------------------------------
 * Expression parser: an operator-precedence parser over the token stream
 * of the CB file
 * ------------------------------------------------------------------------- */

/* The operand stack with one spare slot in front of it.  After an underflow
 * (malformed token stream) the top index can reach -1; the spare slot is
 * index -1, so pushes and pops there still work.  Below it pushes are
 * dropped and pops yield NULL. */
static node_t **operand_stack_alloc(int cap)
{
    node_t **store = expand_alloc_tracked(((size_t)cap + 1) * sizeof(*store), "initialization");
    return store + 1;
}

/* Allocate the operand and operator stacks, both empty. */
void expand_stacks_alloc(void)
{
    g_operand_cap = STACK_STEP;
    g_operands = operand_stack_alloc(STACK_STEP);
    g_operand_top = 0;
    g_op_cap = STACK_STEP;
    g_ops = expand_alloc_tracked(STACK_STEP * sizeof(*g_ops), "initialization");
    g_op_top = 0;
}

/* Slot 0 is never used; the stack grows by STACK_STEP when full. */
static void push_operand(node_t *n)
{
    if (g_operand_top >= g_operand_cap - 1) {
        int cap = g_operand_cap + STACK_STEP;
        node_t **grown = operand_stack_alloc(cap);
        memcpy(grown, g_operands, (size_t)g_operand_cap * sizeof(*grown));
        g_operands = grown;
        g_operand_cap = cap;
    }
    g_operand_top++;
    if (g_operand_top >= -1) {
        g_operands[g_operand_top] = n;
    }
}

/* Popping the empty stack is reported; see operand_stack_alloc(). */
static node_t *pop_operand(void)
{
    if (g_operand_top == 0) {
        expand_report_error(ERR_INTERNAL_OPERAND_UNDERFLOW, "");
    }
    /* after an underflow slot 0 and the spare slot below it are handed out,
     * holding whatever was pushed there since */
    node_t *n = g_operand_top >= -1 ? g_operands[g_operand_top] : NULL;
    g_operand_top--;
    return n;
}

/* Push an operator with its priority and number of operands; the stack
 * grows by STACK_STEP when full. */
static void push_op(node_t *n, int priority, int arity)
{
    if (g_op_top >= g_op_cap - 1) {
        int cap = g_op_cap + STACK_STEP;
        op_entry_t *grown = expand_alloc_tracked((size_t)cap * sizeof(*grown), "initialization");
        memcpy(grown, g_ops, (size_t)g_op_cap * sizeof(*grown));
        g_ops = grown;
        g_op_cap = cap;
    }
    g_op_top++;
    if (g_op_top >= 0) {
        g_ops[g_op_top].node = n;
        g_ops[g_op_top].priority = priority & 0xffff;
        g_ops[g_op_top].arity = (uint8_t)arity;
    }
}

/* Priority of the topmost operator; 0 for an empty stack */
static int top_priority(void)
{
    return g_op_top >= 0 ? g_ops[g_op_top].priority : 0;
}

/* Report operands or operators left over at the end of an equation, then
 * empty both stacks. */
static void check_stacks_empty(void)
{
    if (g_operand_top != 0) {
        expand_report_error(ERR_INTERNAL_OPERANDS_LEFT, "");
    }
    if (g_op_top != 0) {
        expand_report_error(ERR_INTERNAL_OPS_LEFT, "");
    }
    g_operand_top = 0;
    g_op_top = 0;
}

/* An operator of the given priority arrives.  Operators of higher
 * priority on the stack get their operands first; operators of the same
 * priority stay, so chains associate to the right.  Priority 0 ends the
 * expression. */
static void reduce(node_t *op_node, int priority, int arity)
{
    if (priority == 0 && g_paren_depth != 0) {
        expand_report_error(ERR_PARENTHESES, "stack evaluation");
        g_paren_depth = 0;
    }
    int level = g_paren_depth * 10 + priority;
    while (top_priority() > level) {
        if (g_op_top == 0) {
            expand_report_error(ERR_INTERNAL_OP_UNDERFLOW, "");
        }
        op_entry_t entry = { 0, NULL, 0 };
        if (g_op_top >= 0) {
            entry = g_ops[g_op_top];
        }
        g_op_top--;
        if (entry.node == NULL) {
            return;
        }
        node_t *left = NULL;
        node_t *right = pop_operand();
        if (entry.arity == 2) {
            left = pop_operand();
        }
        expand_fold_operands(right, left, entry.node);
        if (right != NULL && right->type != NODE_VAR && right->type != NODE_CONST) {
            entry.node->right.node = right;
        }
        if (left != NULL && left->type != NODE_VAR && left->type != NODE_CONST) {
            entry.node->left.node = left;
        }
        push_operand(entry.node);
    }
    if (level != 0) {
        push_op(op_node, level, arity);
    }
}

/* The operands of a FIELD definition as a list.  Every member but
 * the innermost one becomes a NODE_LIST. */
static node_t *build_list(void)
{
    if (g_operand_top == 0) {
        return NULL;
    }
    node_t *head = pop_operand();
    node_t *cur = head;
    if (head != NULL) {
        head->type = NODE_LIST;
    }
    while (g_operand_top != 0 && cur != NULL) {
        cur->type = NODE_LIST;
        cur->left.node = pop_operand();
        cur = cur->left.node;
    }
    return head;
}

/* Read one token of the CB file; end of file is fatal. */
static void read_token(cf_token_t *token)
{
    if (!cf_read_token(expand_in, token)) {
        expand_fatal_error(ERR_DISK_READ, expand_in_name);
    }
}

/* Feed one token to the parser; returns 0 at the end of the equation. */
static int handle_token(const cf_token_t *token)
{
    int type = token->type;
    int flags = token->flags;
    uint32_t value = token->value;
    node_t *n;

    switch (type) {
    case TOK_END_EQUATION:
        if (is_field_symbol(expand_cur_sym)) {
            expand_expr = build_list();
        } else {
            reduce(NULL, 0, 1);
            if (g_operand_top != 0) {
                expand_expr = pop_operand();
            }
        }
        check_stacks_empty();
        return 0;
    case TOK_OR:
        reduce(expand_new_node_of(OP_OR, flags), 4, 2);
        break;
    case TOK_XOR:
        reduce(expand_new_node_of(OP_XOR, flags), 3, 2);
        break;
    case TOK_AND:
        reduce(expand_new_node_of(OP_AND, flags), 5, 2);
        break;
    case TOK_OPEN:
        if (flags & TOKEN_FLAG_NEGATED) {
            reduce(expand_new_node_of(NODE_NOT, 0), 6, 1);
        }
        g_paren_depth++;
        break;
    case TOK_CLOSE:
        g_paren_depth--;
        if (g_paren_depth < 0) {
            expand_report_error(ERR_PARENTHESES, "node evaluation");
            g_paren_depth = 0;
        }
        break;
    case TOK_VARIABLE:
        if (value >= (uint32_t)expand_symbol_alloc) {
            expand_fatal_error(ERR_INTERNAL_BAD_TOKEN, ""); /* symbol index out of range */
        }
        n = expand_new_node_of(NODE_VAR, flags);
        n->right.sym = &expand_symbols[value];
        push_operand(n);
        break;
    case TOK_CONSTANT:
    case TOK_CONSTANT2:
        n = expand_new_node_of(NODE_CONST, flags);
        n->right.raw = value;
        push_operand(n);
        break;
    case TOK_LINE:
        if (g_line_tokens == 0) {
            expand_line = (int)value;
        }
        g_line_tokens++;
        break;
    default:
        expand_fatal_error(ERR_INTERNAL_BAD_TOKEN, "");
    }
    return 1;
}

/* Read the next equation into expand_expr.  Returns 2 when there is
 * one, 1 at the end of a design section that is followed by another one and
 * 0 at the end of the file. */
int expand_read_equation(void)
{
    cf_token_t token;
    read_token(&token);
    if (token.type == TOK_END_SECTION) {
        return 1;
    }
    if (token.type == TOK_END_FILE) {
        return 0;
    }
    if ((token.flags & TOKEN_FLAG_TARGET) == 0) {
        expand_report_error(ERR_INTERNAL_NO_TARGET, "");
        return 0;
    }
    uint32_t index = token.value;
    if (index >= (uint32_t)expand_symbol_alloc) {
        expand_fatal_error(ERR_INTERNAL_BAD_TOKEN, ""); /* symbol index out of range */
    }
    expand_cur_sym = &expand_symbols[index];
    expand_cur_index = index;
    g_operand_top = 0;
    g_op_top = 0;
    g_line_tokens = 0;
    do {
        read_token(&token);
        if (token.type == TOK_END_SECTION) {
            return 1;
        }
        if (token.type == TOK_END_FILE) {
            return 0;
        }
    } while (handle_token(&token));
    return 2;
}

/* Reset the parser state for a new run of the phase. */
void expand_input_reset(void)
{
    g_paren_depth = 0;
    g_line_tokens = 0;
    g_operands = NULL;
    g_operand_top = 0;
    g_operand_cap = 0;
    g_ops = NULL;
    g_op_top = 0;
    g_op_cap = 0;
}
