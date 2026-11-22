/*
 * expand_frame.c - frames and the CM file of the expand phase.
 *
 * The sum of products of an equation is written as a frame: one row of
 * three bit maps (outputs, variables used, variables true) per product
 * term.  When the symbol asks for minimisation, identical, covered and
 * adjacent rows are merged while the frame is built.  This file also
 * writes the section headers, symbol records and end markers of CM<pid>.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "expand_int.h"

/* -------------------------------------------------------------------------
 * State: the frame under construction
 * ------------------------------------------------------------------------- */
static frame_t g_frame;
static int g_row_capacity;
static int g_xor_row;                  /* last row of the first XOR operand (16 bit), -1 if none */
static int g_in_xor;
static row_compare_t g_compare;
static int g_term_count;

/* -------------------------------------------------------------------------
 * CM file: section header, symbol records and end markers
 * ------------------------------------------------------------------------- */

/* Write a symbol as a record of the CM file. */
static void write_symbol(const symbol_t *sym)
{
    cf_symbol_t rec;
    memcpy(rec.name, sym->name, SYMBOL_NAME_SIZE);
    memcpy(rec.ext, sym->ext, SYMBOL_EXT_SIZE);
    rec.pin = sym->pin;
    rec.type = sym->kind;
    rec.flags = sym->flags;
    memcpy(rec.attr, sym->attr, sizeof(rec.attr));
    rec.polarity = sym->polarity;
    rec.attr2 = sym->attr2;
    rec.pin_index = sym->pin_fn;
    rec.node_index = sym->node_fn;
    rec.or_index = sym->out_fn;
    rec.ext_code = sym->ext_code;
    rec.terms = rd32(sym->pad);
    if (!cf_write_symbol(expand_out, &rec) || fflush(expand_out) != 0) {
        expand_fatal_error(ERR_DISK_WRITE, expand_out_name);
    }
}

/* Copy a string into a fixed header image without leaving it. */
static void header_strcpy(uint8_t *header, size_t at, const uint8_t *src, size_t src_size)
{
    for (size_t i = 0; at + i < CF_HEADER_SIZE; i++) {
        uint8_t c = i < src_size ? src[i] : 0;
        header[at + i] = c;
        if (c == 0) {
            break;
        }
    }
}

/* Header of a design section in the CM file */
void expand_write_section_header(void)
{
    uint8_t out[CF_HEADER_SIZE];
    uint8_t *entry = out + CF_HDR_DEVICE;
    unsigned extra = cf_header_extra(expand_header);
    memset(out, 0, sizeof(out));
    wr16(out + CF_HDR_MAGIC, CF_MAGIC_CM);
    wr16(out + CF_HDR_SYMBOLS, (unsigned)(expand_symbol_count + expand_extra_count));
    entry[DL_ENTRY_FLAGS] = expand_lib_entry[DL_ENTRY_FLAGS];
    memcpy(entry + DL_ENTRY_LENGTH, expand_lib_entry + DL_ENTRY_LENGTH, 4);
    memcpy(entry + DL_ENTRY_OFFSET, expand_lib_entry + DL_ENTRY_OFFSET, 4);
    wr16(out + CF_HDR_EXTRA, extra);
    header_strcpy(out, CF_HDR_DESIGN, expand_header + CF_HDR_DESIGN, CF_HEADER_SIZE - CF_HDR_DESIGN);
    header_strcpy(out, CF_HDR_DEVICE, expand_lib_entry, DL_ENTRY_FLAGS);
    header_strcpy(out, CF_HDR_FORMAT, expand_header + CF_HDR_FORMAT, CF_HEADER_SIZE - CF_HDR_FORMAT);
    if (!cf_write_header(expand_out, out) || fflush(expand_out) != 0) {
        expand_fatal_error(ERR_DISK_WRITE, expand_out_name);
    }
    if (extra != 0) {
        if (fwrite(expand_header_extra, extra, 1, expand_out) == 0 || fflush(expand_out) != 0) {
            expand_fatal_error(ERR_DISK_WRITE, expand_out_name);
        }
    }
}

/* Write the symbol table followed by the generated symbols. */
void expand_write_symbols(void)
{
    for (int i = 0; i < expand_symbol_count; i++) {
        write_symbol(&expand_symbols[i]);
    }
    for (int i = 0; i < expand_extra_count; i++) {
        write_symbol(&expand_extra_symbols[i]);
    }
}

/* A frame header without rows marks the end of a section or of
 * the file */
void expand_write_marker(unsigned kind)
{
    cf_frame_t marker = { (uint16_t)kind, 0, 0, 0, 0, 0 };
    if (!cf_write_frame(expand_out, &marker) || fflush(expand_out) != 0) {
        expand_fatal_error(ERR_DISK_WRITE, expand_out_name);
    }
}

/* -------------------------------------------------------------------------
 * Frames
 *
 * A row describes one product term with three bit maps of the same size:
 * the outputs the term belongs to, the variables it uses and the variables
 * that must be true.  Bits 0 and 1 of the variable maps stand for the
 * constants 0 and 1 (symbol 0 is never a variable); word 0 of the "used"
 * map carries the row flags.
 * ------------------------------------------------------------------------- */
static size_t map_words(void)
{
    return g_frame.row_words / 3;
}

/* Allocate the rows of a frame for the device's initial number of product
 * terms; the current row is row 0. */
static void frame_alloc(void)
{
    size_t words = (size_t)((((int)g_frame.var_count - 1) >> 4) + 2);
    g_frame.row_words = (uint32_t)(words * 3);
    g_row_capacity = (int)dev_u16(dev_info(INFO_MAX_TERMS));
    if (g_row_capacity < 10) {
        expand_fatal_error(ERR_INTERNAL_FRAME_SIZE, "");
    }
    g_frame.total_words = g_frame.row_words * (uint32_t)g_row_capacity;
    g_frame.rows = expand_alloc_tracked((size_t)g_frame.total_words * 2, "frame allocation");
    g_frame.cur_out = g_frame.rows;
    g_frame.cur_used = g_frame.cur_out + words;
    g_frame.cur_true = g_frame.cur_used + words;
}

/* Room for twice as many rows */
static void frame_grow(void)
{
    size_t words = map_words();
    size_t at = (size_t)(g_frame.cur_out - g_frame.rows);
    uint32_t old_total = g_frame.total_words;
    g_row_capacity *= 2;
    g_frame.total_words = g_frame.row_words * (uint32_t)g_row_capacity;
    uint16_t *rows = expand_alloc_tracked((size_t)g_frame.total_words * 2, "frame reallocation");
    memcpy(rows, g_frame.rows, (size_t)old_total * 2);
    g_frame.rows = rows;
    g_frame.cur_out = rows + at;
    g_frame.cur_used = g_frame.cur_out + words;
    g_frame.cur_true = g_frame.cur_used + words;
}

static void next_row(void)
{
    g_frame.cur_out += g_frame.row_words;
    g_frame.cur_used += g_frame.row_words;
    g_frame.cur_true += g_frame.row_words;
}

static void set_output_bit(void)
{
    map_set(g_frame.cur_out, map_words(), (int)expand_cur_index);
}

/* AND the constant 0 or 1 into the current row */
static void add_constant(int value)
{
    g_frame.cur_used[1] |= (uint16_t)(1u << (value ? BIT_TRUE : BIT_FALSE));
    g_frame.cur_true[1] |= (uint16_t)(1u << (value ? BIT_TRUE : BIT_FALSE));
}

/* Reduce a row to the constant `bit` */
static void set_row_constant(uint16_t *used, uint16_t *is_true, int bit, size_t words)
{
    for (size_t i = 1; i < words; i++) {
        used[i] = 0;
        is_true[i] = 0;
    }
    map_set(used, words, bit);
    map_set(is_true, words, bit);
}

static void clear_row(uint16_t *out, uint16_t *used, uint16_t *is_true, size_t words)
{
    memset(out, 0, words * 2);
    memset(used, 0, words * 2);
    memset(is_true, 0, words * 2);
}

static void copy_row(uint16_t *out, uint16_t *used, uint16_t *is_true, const uint16_t *src_out,
                     const uint16_t *src_used, const uint16_t *src_true, size_t words)
{
    memmove(out, src_out, words * 2);
    memmove(used, src_used, words * 2);
    memmove(is_true, src_true, words * 2);
}

/* Word 1 of the map equals `value` and the rest is empty */
static int map_is_only(const uint16_t *map, unsigned value, size_t words)
{
    if (map[1] != value) {
        return 0;
    }
    for (size_t i = 2; i < words; i++) {
        if (map[i] != 0) {
            return 0;
        }
    }
    return 1;
}

/* Compare an existing row (a) with the row being added (b) */
static void compare_rows(row_compare_t *result, const uint16_t *a_used, const uint16_t *a_true,
                         const uint16_t *b_used, const uint16_t *b_true, size_t words)
{
    int diffs = 0, a_in_b = 0, b_in_a = 0, a_agrees = 0, b_agrees = 0;
    result->relation = 0;
    for (size_t w = 1; w < words; w++) {
        uint16_t common = a_used[w] & b_used[w];
        uint16_t differ = (uint16_t)(common & (a_true[w] ^ b_true[w]));
        if (a_used[w] == common) {
            a_in_b++;
            if (differ == 0) {
                a_agrees++;
            }
        }
        if (b_used[w] == common) {
            b_in_a++;
            if (differ == 0) {
                b_agrees++;
            }
        }
        if (diffs < 2 && b_used[w] == a_used[w] && b_true[w] != a_true[w]) {
            for (int bit = 0; bit < 16 && diffs < 2; bit++) {
                if (((b_true[w] ^ a_true[w]) >> bit) & 1) {
                    diffs++;
                    result->diff_bit = (int)(w - 1) * 16 + bit;
                }
            }
        }
    }
    int n = (int)words - 1;
    result->keep_first = 1;
    if (n == a_in_b && n == b_in_a) {
        if (diffs == 1) {
            result->relation = 3;
            return;
        }
        if (diffs == 0) {
            result->relation = 1;
            return;
        }
    }
    if (n == a_in_b && n == a_agrees) {
        result->relation = 2;
    } else if (n == b_in_a && n == b_agrees) {
        result->relation = 2;
        result->keep_first = 0;
    }
}

/* Try to merge the current row into one of the finished rows of
 * the frame: an equal row is dropped, of two rows where one covers the
 * other the more general one stays, and two rows that differ in the
 * polarity of one variable lose that variable (unless `no_adjacent`).  Rows
 * on different sides of an XOR are not compared.  Returns the row merged
 * with or -1. */
static int merge_row(int no_adjacent)
{
    if (g_frame.row_count == 0) {
        return -1;
    }
    size_t words = map_words();
    uint16_t *r_out = g_frame.rows;
    uint16_t *r_used = r_out + words;
    uint16_t *r_true = r_used + words;
    uint16_t *c_used = g_frame.cur_used, *c_true = g_frame.cur_true, *c_out = g_frame.cur_out;
    const uint16_t *split = NULL;
    int side = 0;
    if (g_xor_row >= 0) {
        split = g_frame.rows + (size_t)g_xor_row * g_frame.row_words + words;
        side = split < c_used ? 2 : 1;
    }
    for (int i = 0; i < (int)g_frame.row_count; i++, r_out += g_frame.row_words,
             r_used += g_frame.row_words, r_true += g_frame.row_words) {
        if (c_used == r_used) {
            continue;
        }
        if ((side == 2 && !(split < r_used)) || (side == 1 && !(r_used <= split))) {
            continue;
        }
        compare_rows(&g_compare, r_used, r_true, c_used, c_true, words);
        if (g_compare.relation == 1) {
            clear_row(c_out, c_used, c_true, words);
            return i;
        }
        if (g_compare.relation == 2) {
            if (g_compare.keep_first == 0) {
                copy_row(r_out, r_used, r_true, c_out, c_used, c_true, words);
                r_used[0] |= ROW_MORE;
            }
            clear_row(c_out, c_used, c_true, words);
            return i;
        }
        if (g_compare.relation == 3 && !no_adjacent) {
            int bit = g_compare.diff_bit;
            int word = (bit >> 4) + 1;
            uint16_t mask = (uint16_t)(1u << (bit & 15));
            if ((r_used[word] & mask) == 0) {
                expand_report_error(ERR_INTERNAL_MERGE_BIT, "");
            } else {
                r_used[word] &= (uint16_t)~mask;
            }
            r_true[word] &= (uint16_t)~mask;
            if (map_is_only(r_used, 0, words)) {
                r_used[1] |= 1u << BIT_TRUE;
                r_true[1] |= 1u << BIT_TRUE;
            }
            clear_row(c_out, c_used, c_true, words);
            return i;
        }
    }
    return -1;
}

/* Merge the finished rows with each other until nothing changes.
 * A row that goes away is replaced by the last row. */
static void reduce_rows(void)
{
    size_t words = map_words();
    size_t row = g_frame.row_words;
    uint16_t *x_out = NULL, *x_used = NULL, *x_true = NULL;
    uint16_t *l_out, *l_used, *l_true;
    int again;
    do {
        if (g_frame.row_count < 2) {
            return;
        }
        g_frame.cur_out = g_frame.rows;
        g_frame.cur_used = g_frame.cur_out + words;
        g_frame.cur_true = g_frame.cur_used + words;
        int side = g_xor_row >= 0;
        if (side) {
            x_out = g_frame.rows + (size_t)g_xor_row * row;
            x_used = x_out + words;
            x_true = x_used + words;
        }
        l_out = g_frame.rows + (size_t)(g_frame.row_count - 1) * row;
        l_used = l_out + words;
        l_true = l_used + words;
        int i = 0;
        again = 0;
        while (i < (int)g_frame.row_count) {
            int group = 0;
            if (g_xor_row >= 0 && g_xor_row < i) {
                side = 2;
            }
            g_compare.relation = 0;
            switch (side) {
            case 0: group = g_frame.row_count; break;
            case 1: group = g_xor_row + 1; break;
            case 2: group = (int)g_frame.row_count - g_xor_row - 1; break;
            }
            /* a false row is dropped unless it is the only one of its side */
            int drop = (g_frame.cur_used[1] & 1) != 0 && (g_frame.cur_true[1] & 1) != 0 && group > 1;
            if (drop || merge_row(0) >= 0) {
                if (side == 1) {
                    copy_row(g_frame.cur_out, g_frame.cur_used, g_frame.cur_true, x_out, x_used,
                             x_true, words);
                    copy_row(x_out, x_used, x_true, l_out, l_used, l_true, words);
                    x_used[0] |= ROW_MORE;
                    g_xor_row--;
                    x_out -= row;
                    x_used -= row;
                    x_true -= row;
                } else {
                    copy_row(g_frame.cur_out, g_frame.cur_used, g_frame.cur_true, l_out, l_used,
                             l_true, words);
                }
                clear_row(l_out, l_used, l_true, words);
                g_frame.cur_used[0] |= ROW_MORE;
                l_out -= row;
                l_used -= row;
                l_true -= row;
                l_used[0] = 0;
                g_frame.row_count--;
                if (g_compare.relation == 3) {
                    again = 1;
                }
            } else {
                next_row();
                i++;
            }
        }
    } while (again);
    l_used[0] = 0;
    if (g_xor_row >= 0) {
        g_frame.rows[(size_t)g_xor_row * row + words] = ROW_XOR_SPLIT;
    }
}

/* Detail text of some "invalid input" errors.  It is deliberately computed
 * as if a node were laid out as type byte, flags byte and two 32-bit
 * operands, and the four bytes at offset 2 of whatever the node's right
 * operand points to were a string pointer; the messages depend on this.
 * When the right operand is a sub-tree those bytes are its left operand:
 * the name of a variable, or a null pointer for the constant 0.  When it is
 * a variable they are characters 3 to 6 of its name: a null pointer for
 * names of one or two characters, otherwise no usable text (the result is
 * empty).  Returns NULL for a null pointer. */
static const char *wild_detail(const node_t *n, char *out, size_t size)
{
    int op = op_of(n->type);
    operand_kind_t rk = op != 0 ? right_kind(n) : K_VAR;
    out[0] = '\0';
    if (op == 0 && n->type != NODE_VAR && n->type != NODE_LIST) {
        return NULL;
    }
    if (rk == K_ZERO || rk == K_ONE) {
        return NULL;
    }
    if (rk == K_VAR || rk == K_NVAR) {
        const symbol_t *sym = n->right.sym;
        if (sym == NULL || (sym->name[2] == 0 && sym->name[3] == 0 && sym->name[4] == 0 &&
                            sym->name[5] == 0)) {
            return NULL;
        }
        return out;
    }
    const node_t *r = n->right.node;
    if (r == NULL) {
        return NULL;
    }
    int rop = op_of(r->type);
    operand_kind_t lk = rop != 0 ? left_kind(r) : K_ZERO;
    if (rop == 0) {
        return r->left.raw == 0 ? NULL : out;
    }
    if (lk == K_VAR || lk == K_NVAR) {
        if (r->left.sym == NULL) {
            return NULL;
        }
        symbol_name(r->left.sym, out, size);
    } else if (lk == K_NODE) {
        const node_t *l = r->left.node;
        if (l == NULL) {
            return NULL;
        }
        snprintf(out, size, "%c%c", l->type, l->flags);
    } else if (r->left.raw == 0) {
        return NULL;
    }
    return out;
}

/* Part of add_variable(): check that the variable can be used as an input
 * of the current output */
static void check_input(const node_t *n, const symbol_t *sym, int right_side, int negated)
{
    char name[SYMBOL_NAME_SIZE + 1];
    char wild_text[SYMBOL_NAME_SIZE + 1];
    symbol_name(sym, name, sizeof(name));
    const char *wild = wild_detail(n, wild_text, sizeof(wild_text));
    if (sym->pin_fn < 0) {
        if (!right_side && negated) {
            expand_report_error(ERR_INVALID_INPUT, wild);
        } else {
            expand_report_pin_error(ERR_INVALID_INPUT, sym);
        }
        return;
    }
    if (map_test(expand_pin_foreign, expand_pin_words, (int)dev_u16(dev_pinfn(sym->pin_fn, PINFN_PIN)))) {
        expand_report_error(ERR_INVALID_INPUT, wild);
        return;
    }
    unsigned ext = dev_u8(dev_nodefn(expand_cur_sym->node_fn, NODEFN_EXT));
    int clocked = 0;
    switch (dev_u8(dev_pinfn(sym->pin_fn, PINFN_TYPE))) {
    case 0: case 7: case 0x0b: case 0x0c: case 0x0d: case 0x0e:
    case 0x10: case 0x15: case 0x16: case 0x17:
        return;
    case 5: case 0x14:
        if (ext == '*' || ext == 'C' || ext == ',') {
            return;
        }
        expand_report_error(ERR_INVALID_INPUT, wild);
        if (right_side && !negated) {
            return;
        }
        clocked = 1;
        break;
    case 2: case 3: case 9:
        clocked = 1;
        break;
    default:
        break;
    }
    if (clocked) {
        if (ext == '+' || (right_side && !negated && (ext == '>' || ext == '?'))) {
            return;
        }
        expand_report_error(ERR_INVALID_INPUT, (!right_side && negated) ? wild : name);
    }
    if (!is_virtual_device()) {
        expand_report_error(ERR_INVALID_INPUT, wild);
    }
}

/* AND the right or left operand of n, a plain or negated variable, into
 * the current row */
static void add_variable(const node_t *n, int right_side, int negated)
{
    symbol_t *sym = right_side ? n->right.sym : n->left.sym;
    size_t words = map_words();
    int index = sym != NULL ? (int)(sym - expand_symbols) : 0;
    if (map_test(g_frame.cur_used, words, index) &&
        map_test(g_frame.cur_true, words, index) == negated) {
        add_constant(0); /* x & !x */
    }
    map_set(g_frame.cur_used, words, index);
    if (!negated) {
        map_set(g_frame.cur_true, words, index);
    }
    set_output_bit();
    if (expand_cur_sym->kind == SYM_FIELD || expand_cur_sym->kind == SYM_FIELD2) {
        /* every member of a FIELD is a row of its own */
        next_row();
        g_term_count++;
        g_frame.row_count++;
        if ((int)g_frame.row_count >= g_row_capacity) {
            frame_grow();
        }
        return;
    }
    if (sym != NULL) {
        check_input(n, sym, right_side, negated);
    }
}

/* An OR; the current row is complete */
static void end_product(void)
{
    size_t words = map_words();
    if (g_frame.cur_used[1] & 1) {
        set_row_constant(g_frame.cur_used, g_frame.cur_true, BIT_FALSE, words);
    }
    if (expand_min_level != 0 && merge_row(0) >= 0) {
        if (g_in_xor && g_xor_row == 0) {
            expand_report_error(ERR_INTERNAL_XOR_MERGE, "");
        }
        if (g_in_xor && g_xor_row > 0) {
            g_xor_row--;
        }
        return;
    }
    g_frame.cur_used[0] |= ROW_MORE;
    set_output_bit();
    next_row();
    g_term_count++;
    g_frame.row_count++;
    if ((int)g_frame.row_count >= g_row_capacity) {
        frame_grow();
    }
    set_output_bit();
}

static void emit_operand(const node_t *n, int right_side)
{
    switch (right_side ? right_kind(n) : left_kind(n)) {
    case K_VAR: add_variable(n, right_side, 0); break;
    case K_NVAR: add_variable(n, right_side, 1); break;
    case K_ZERO: add_constant(0); break;
    case K_ONE: add_constant(1); break;
    default: break;
    }
}

/* Walk the sum of products in order and fill the rows */
static void emit_tree(const node_t *n)
{
    if (n == NULL) {
        return;
    }
    if (has_left_node(n) || n->type == NODE_LIST) {
        emit_tree(n->left.node);
    }
    g_in_xor = 0;
    int op = op_of(n->type);
    if (op != 0) {
        emit_operand(n, 0);
        if (op == OP_XOR) {
            g_xor_row = (int16_t)g_frame.row_count;
            g_in_xor = 1;
        }
        if (op != OP_AND) {
            end_product();
        }
        emit_operand(n, 1);
    } else if (n->type == NODE_VAR || n->type == NODE_LIST) {
        add_variable(n, 1, (n->flags & NODE_NEGATED) != 0);
    } else if (n->type == NODE_CONST) {
        add_constant(kind_of(n) == K_ONE);
    } else {
        expand_report_error(ERR_INTERNAL_EMIT_TYPE, "");
    }
    if (has_right_node(n)) {
        emit_tree(n->right.node);
    }
}

/* Rows of an equation */
static void emit_equation(const node_t *expr)
{
    size_t words = map_words();
    int kind = expand_cur_sym->kind;
    if ((kind == SYM_PIN || kind == SYM_PINNODE || kind == SYM_NODE || kind == SYM_HIDDEN_NODE) &&
        expand_cur_sym->node_fn < 0) {
        expand_report_pin_error(ERR_INVALID_OUTPUT, expand_cur_sym);
    }
    g_xor_row = -1;
    set_output_bit();
    emit_tree(expr);
    if (kind == SYM_FIELD || kind == SYM_FIELD2) {
        return;
    }
    if (expand_min_level == 0 || merge_row(0) < 0) {
        g_frame.row_count++;
        set_output_bit();
        if (g_frame.cur_used[1] & 1) {
            set_row_constant(g_frame.cur_used, g_frame.cur_true, BIT_FALSE, words);
        }
    } else {
        g_frame.cur_used -= g_frame.row_words;
        g_frame.cur_used[0] = 0;
    }
    if (expand_min_level != 0) {
        reduce_rows();
    }
    if (g_xor_row >= 0) {
        g_frame.rows[(size_t)g_xor_row * g_frame.row_words + words] = ROW_XOR_SPLIT;
    }
}

/* Rows of a generated symbol: every product term of the node
 * function is preset to 0 or 1 */
static void emit_preset(void)
{
    g_term_count = 0;
    g_frame.row_count = 0;
    g_xor_row = -1;
    if (expand_cur_sym->node_fn < 0) {
        expand_report_error(ERR_INTERNAL_PRESET_NO_NODE, "");
        return;
    }
    for (int i = 0; i < (int)g_frame.max_terms; i++) {
        if ((int)g_frame.row_count >= g_row_capacity) {
            frame_grow(); /* more product terms than rows allocated */
        }
        set_output_bit();
        int is_output = (expand_cur_sym->flags & SYMF_OUTPUT) != 0;
        switch (dev_u8(dev_nodefn(expand_cur_sym->node_fn, NODEFN_DEFAULT))) {
        case 1:
            add_constant(!is_output);
            break;
        case 2:
            add_constant(is_output);
            break;
        case 9:
            add_constant(map_test(expand_nodefn_default, expand_nodefn_words, expand_cur_sym->node_fn));
            break;
        default:
            expand_report_error(ERR_INTERNAL_PRESET_KIND, "");
            break;
        }
        next_row();
        g_term_count++;
        g_frame.row_count++;
    }
}

/* Several outputs share the product terms of this node function */
static int check_shared_terms(const symbol_t *sym)
{
    if (sym == NULL || sym->node_fn < 0) {
        return 0;
    }
    switch (dev_u8(dev_nodefn(sym->node_fn, NODEFN_SHARING))) {
    case 1: {
        int other = expand_find_line_sharer(sym);
        /* Inverting the register equation does not free the sum used by
         * another function of this pin.  POL_FIXED is an alternative
         * implementation of the same equation, not another consumer. */
        int conflict = other >= 0 && sym->polarity != POL_FIXED &&
                       (sym->polarity == POL_KEEP || sym->pin == expand_symbols[other].pin) &&
                       map_test(expand_conflict_map, expand_conflict_words, other);
        /* A register alias and its physical pin consume the same sum.
         * Check every earlier consumer: the first symbol on the line may
         * only read feedback, and equation inversion cannot add a sum. */
        unsigned ext = dev_u8(dev_nodefn(sym->node_fn, NODEFN_EXT));
        for (int i = 0; !conflict && sym->polarity != POL_FIXED &&
             i < expand_symbol_count; i++) {
            const symbol_t *previous = &expand_symbols[i];
            if (previous->node_fn < 0 || previous->polarity == POL_FIXED ||
                !map_test(expand_conflict_map, expand_conflict_words, i)) {
                continue;
            }
            unsigned prev_ext = dev_u8(dev_nodefn(previous->node_fn, NODEFN_EXT));
            if (ext != 1 && ext != 0x1b && prev_ext != 1 && prev_ext != 0x1b) {
                continue;
            }
            conflict = dev_u16(dev_nodefn(sym->node_fn, NODEFN_LINE)) ==
                           dev_u16(dev_nodefn(previous->node_fn, NODEFN_LINE)) &&
                       dev_u32(dev_nodefn(sym->node_fn, NODEFN_GROUP)) ==
                           dev_u32(dev_nodefn(previous->node_fn, NODEFN_GROUP));
        }
        if (conflict) {
            expand_report_pin_error(ERR_EXCLUDED_OUTPUT, sym);
        }
        map_set(expand_conflict_map, expand_conflict_words, (int)expand_cur_index);
        return conflict;
    }
    case 2:
        expand_report_pin_error(ERR_INVALID_OUTPUT_USAGE, sym);
        return 1;
    default:
        return 0;
    }
}

/* Tidy the frame up: a product that is 1 together with variables
 * loses the 1, a sum that contains 1 becomes 1 and a sum of 0s becomes a
 * single 0 */
static void finish_frame(void)
{
    if (g_frame.row_count == 0) {
        return;
    }
    size_t words = map_words();
    uint16_t *used = g_frame.rows + words;
    uint16_t *is_true = used + words;
    int part = 0;
    unsigned false_rows = 0;
    uint8_t part_flags[4] = { 0, 0, 0, 0 }; /* [0..1]: part contains 1, [2..3]: contains 0 */
    for (int i = 0; i < (int)g_frame.row_count; i++) {
        if (used[0] & ROW_BAD) {
            expand_report_error(ERR_INTERNAL_ROW_FLAG, "");
        }
        if (used[1] & (1u << BIT_TRUE)) {
            if (!map_is_only(used, 1u << BIT_TRUE, words)) {
                used[1] &= (uint16_t)~(1u << BIT_TRUE);
                is_true[1] &= (uint16_t)~(1u << BIT_TRUE);
            } else if (part < 4) {
                part_flags[part] = 1;
            }
        }
        if (used[1] & (1u << BIT_FALSE)) {
            if (part + 2 < 4) {
                part_flags[part + 2] = 1;
            }
            false_rows++;
        }
        if (used[0] & ROW_XOR_SPLIT) {
            part++;
        }
        used += g_frame.row_words;
        is_true += g_frame.row_words;
    }
    used[-(long)g_frame.row_words] = 0;
    used = g_frame.rows + words;
    is_true = used + words;
    if (part != 0) {
        return;
    }
    if (part_flags[0]) {
        g_frame.row_count = 1;
        used[0] = 0;
        set_row_constant(used, is_true, BIT_TRUE, words);
    } else if (part_flags[2] && g_frame.row_count == 1) {
        used[0] = 0;
    } else if (part_flags[2] && g_frame.row_count == false_rows) {
        used[0] = 0;
        g_frame.row_count = 1;
    }
}

/* Write the frame header and its rows to the CM file. */
static void write_frame(void)
{
    cf_frame_t header = { g_frame.flags, g_frame.reserved, g_frame.var_count,
                          g_frame.row_count, g_frame.has_xor, g_frame.max_terms };
    if (!cf_write_frame(expand_out, &header)) {
        expand_fatal_error(ERR_DISK_WRITE, expand_out_name);
    }
    size_t count = (size_t)g_frame.row_count * g_frame.row_words;
    if (cf_write_words(expand_out, g_frame.rows, count) != count) {
        expand_fatal_error(ERR_DISK_WRITE, expand_out_name);
    }
    if (fflush(expand_out) != 0) {
        expand_fatal_error(ERR_DISK_WRITE, expand_out_name);
    }
}

/* Build and write the frame of the current
 * symbol */
void expand_build_frame(const node_t *expr, int preset)
{
    const symbol_t *sym = expand_cur_sym;
    g_term_count = 0;
    g_frame.flags = 0;
    g_frame.var_count = (uint16_t)(expand_symbol_count + expand_extra_count);
    g_frame.row_count = 0;
    if (expand_invert_output) {
        g_frame.flags |= FRAME_INVERTED;
    }
    if (sym->out_fn < 0) {
        g_frame.max_terms = (uint16_t)dev_u16(dev_info(INFO_MAX_TERMS));
    } else {
        g_frame.max_terms = (uint16_t)dev_u16(dev_nodefn(sym->out_fn, NODEFN_TERMS));
        g_frame.has_xor = (uint16_t)dev_u8(dev_nodefn(sym->out_fn, NODEFN_XOR));
        if (sym->kind == SYM_PIN || sym->kind == SYM_PINNODE || sym->kind == SYM_NODE ||
            sym->kind == SYM_HIDDEN_NODE) {
            g_frame.flags |= sym->flags & SYMF_MIN_LEVEL;
        }
    }
    frame_alloc();
    if (preset) {
        emit_preset();
    } else {
        emit_equation(expr);
    }
    if (check_shared_terms(sym)) {
        g_frame.row_count = 0;
    }
    finish_frame();
    write_frame();
}

/* Reset the frame state for a new run of the phase. */
void expand_frame_reset(void)
{
    g_term_count = 0;
    memset(&g_frame, 0, sizeof(g_frame));
    g_row_capacity = 0;
    g_xor_row = -1;
    g_in_xor = 0;
    memset(&g_compare, 0, sizeof(g_compare));
}
