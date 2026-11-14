/*
 * parse - output: writes the symbol tables and the equations to CB<pid>
 * for expand.  The symbols are flattened into a sorted array and numbered,
 * DeMorgan levels and REGISTER_SELECT conversions are applied, and
 * intermediate variables are expanded in place.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "parse_int.h"
#include "cfile.h"
#include "devlib.h"
#include "phase.h"

/* header items in the order they are written to the CB file */
static const uint8_t k_header_items[11] = {
    TOK_NAME, 0x3f /* partno */, TOK_REV, TOK_REVISION, 0x23 /* date */,
    0x27 /* designer */, 0x20 /* company */, TOK_ASSEMBLY, TOK_ASSY,
    TOK_LOC, TOK_LOCATION
};

static FILE *g_out;
static uint8_t g_sym_record[CF_CB_SYMBOL_SIZE]; /* symbol record of the CB file */
static int g_next_ord;
static int g_demorgan_count;
static int g_companion_count;

void parse_output_reset(void)
{
    g_out = NULL;
    memset(g_sym_record, 0, sizeof(g_sym_record));
    g_next_ord = 0;
    g_demorgan_count = 0;
    g_companion_count = 0;
}

/* =========================================================================
 * Output: CB<pid> (see cfile.h)
 *
 * For each symbol table:
 *   header record, header strings,
 *   symbol records: two marker records (type 6, 7), one per variable in
 *   sorted order, a marker record (type 4),
 *   token records: per assigned variable
 *     <variable> ( expression ) .pend
 *   and finally .xend (more tables follow) or .fini.
 * ========================================================================= */

/* one token record */
static void write_record(int type, int flags, uint32_t value)
{
    cf_token_t tok = { (uint8_t)type, (uint8_t)flags, value };
    if (!cf_write_token(g_out, &tok)) {
        parse_fatal_error(ERR_DISK_WRITE, g_parse_out_name);
    }
}

static void write_op(int id, int flags)
{
    write_record(id, flags, 0);
}

static void write_token(const token_t *tok)
{
    uint32_t value;
    if (tok->flags & TF_SYMBOL) {
        /* A number in a FIELD list has been "resolved" like a name; its
         * value is written as 0.  Nothing reads it. */
        value = 0;
    } else if (tok->flags & TF_NUMBER) {
        value = (uint32_t)tok->u.bit;
    } else {
        value = 0;      /* operators: only the type and flags are read */
    }
    write_record(tok->type, tok->flags, value);
}

static void write_symbol_ref(int flags, const symbol_t *sym)
{
    write_record(TOK_IDENT, flags, sym->ord);
}

/* one symbol record.  The record buffer is static and the names
 * are copied with strcpy, so the bytes behind a name are those of the
 * names written before. */
static void write_symbol_record(const symbol_t *sym)
{
    wr16(g_sym_record + CF_SYM_PIN, (uint16_t)sym->pin);
    g_sym_record[CF_SYM_TYPE] = sym->type;
    g_sym_record[CF_SYM_FLAGS] = sym->flags;
    g_sym_record[CF_SYM_POLARITY] = sym->demorgan & 0x7f;
    g_sym_record[CF_SYM_ATTR2] = (uint8_t)sym->regtype;
    wr16(g_sym_record + CF_SYM_ATTR, sym->ord);
    /* the name is copied whole: a long name runs into the extension and
     * the fields behind it */
    g_sym_record[CF_SYM_NAME] = 0;
    g_sym_record[CF_SYM_EXT] = 0;
    if (sym->name != NULL) {
        size_t n = strlen(sym->name) + 1;
        memcpy(g_sym_record, sym->name, n < sizeof(g_sym_record) ? n : sizeof(g_sym_record));
    }
    if (sym->ext != NULL) {
        size_t n = strlen(sym->ext) + 1;
        memcpy(g_sym_record + CF_SYM_EXT, sym->ext,
               n < sizeof(g_sym_record) - CF_SYM_EXT ? n : sizeof(g_sym_record) - CF_SYM_EXT);
    }
    if (fwrite(g_sym_record, sizeof(g_sym_record), 1, g_out) != 1) {
        parse_fatal_error(ERR_DISK_WRITE, g_parse_out_name);
    }
}

/* the members of FIELD and GROUP lists become symbol references */
static void resolve_list_symbols(symbol_t *sym, symtab_t *table)
{
    if (sym == NULL) {
        return;
    }
    resolve_list_symbols(sym->left, table);
    if ((sym->type == SYM_FIELD || sym->type == SYM_GROUP) && sym->expr != NULL) {
        for (token_t *tok = sym->expr->head; tok != NULL; tok = tok->next) {
            if (tok->flags & TF_SYMBOL) {
                continue;
            }
            const char *name = parse_variable_name(tok);
            tok->u.any = NULL;
            if (tok->next != NULL && tok->next->type == TOK_EXTENSION) {
                tok->u.sym = parse_enter_symbol(name, parse_token_text(tok->next), 1, table);
                tok->next = tok->next->next;
            } else {
                tok->u.sym = parse_enter_symbol(name, NULL, 1, table);
            }
            tok->flags |= TF_SYMBOL;
        }
    }
    resolve_list_symbols(sym->right, table);
}

/* 2 for an output (no extension, or a flip-flop input), 1 for a
 * control extension, 0 for anything else */
static int extension_class(const char *ext)
{
    static const char *const outputs[] = { "d", "l", "j", "k", "s", "r", "t", "t1", "t2", NULL };
    static const char *const controls[] = { "ap", "ar", "sp", "sr", "ck", "oe", "ce", "le", NULL };
    if (ext == NULL) {
        return 0;
    }
    if (ext[0] == '\0') {
        return 2;
    }
    for (int i = 0; outputs[i] != NULL; i++) {
        if (strcmp(ext, outputs[i]) == 0) {
            return 2;
        }
    }
    for (int i = 0; controls[i] != NULL; i++) {
        if (strcmp(ext, controls[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

/* decide the DeMorgan level of each assigned variable; only
 * outputs of programmable polarity can have one */
static void settle_demorgan(symbol_t *sym)
{
    if (sym == NULL) {
        return;
    }
    settle_demorgan(sym->left);
    if (sym->expr != NULL && sym->type != SYM_FIELD && sym->type != SYM_GROUP
        && sym->type != SYM_VARIABLE && sym->type != SYM_FUNCTION) {
        int code = parse_dev_ext_code(sym->ext, g_parse_device);
        int index = parse_dev_pin_index(sym->pin & 0xff, code, g_parse_device);
        if (index < 0) {
            sym->demorgan = 0;
        } else {
            uint32_t kind = parse_dev_pin_kind(g_parse_device, index);
            if (kind == 3 || kind == 7 || kind == 6 || parse_virtual_flag()[0] == '1') {
                if (extension_class(sym->ext) == 2) {
                    sym->demorgan |= (uint8_t)g_parse_demorgan_mode;
                } else {
                    sym->demorgan = 0;
                }
            } else {
                if (extension_class(sym->ext) == 2 && (g_parse_demorgan_mode != 0 || sym->demorgan != 0)) {
                    parse_report_now(ERR_FIXED_POLARITY, 1, sym->name, NULL);
                }
                sym->demorgan = 0;
            }
        }
    }
    if ((sym->demorgan & 0x7f) == 2) {
        g_demorgan_count++;
    }
    settle_demorgan(sym->right);
}

/* copy the symbols of a tree into the array */
static void flatten_symbols(const symbol_t *sym, symbol_t **fill)
{
    if (sym == NULL) {
        return;
    }
    flatten_symbols(sym->left, fill);
    **fill = *sym;
    (*fill)->left = NULL;
    (*fill)->right = NULL;
    (*fill)->next = NULL;
    (*fill)++;
    flatten_symbols(sym->right, fill);
}

/* order of the symbols in the file: by name, then extension; a
 * one-letter extension sorts before the others */
static int compare_symbols(const symbol_t *a, const symbol_t *b)
{
    int order = parse_compare_names(a->name, b->name);
    if (order == 0) {
        size_t len_a = strlen(a->ext != NULL ? a->ext : "");
        size_t len_b = strlen(b->ext != NULL ? b->ext : "");
        if ((len_a == 1 && len_b > 0) || (len_a > 0 && len_b == 1)) {
            order = (len_a == 1) ? -1 : 1;
        } else {
            order = strcmp(a->ext != NULL ? a->ext : "", b->ext != NULL ? b->ext : "");
        }
    }
    return order;
}

/* a stable insertion sort: symbols that compare equal keep their order */
static void sort_symbols(symbol_t *array, int count)
{
    for (int i = 1; i < count; i++) {
        for (int j = i; j > 0 && compare_symbols(&array[j - 1], &array[j]) > 0; j--) {
            symbol_t swap = array[j - 1];
            array[j - 1] = array[j];
            array[j] = swap;
        }
    }
}

/* give the symbols of the table the numbers of their copies */
static void copy_ords(symbol_t *sym, const symbol_t *array)
{
    if (sym == NULL) {
        return;
    }
    copy_ords(sym->left, array);
    for (int i = 0; i < g_parse_total_syms; i++) {
        if (parse_compare_names(array[i].name, sym->name) == 0
            && strcmp(text_or_empty(array[i].ext), text_or_empty(sym->ext)) == 0) {
            sym->ord = array[i].ord;
        }
    }
    copy_ords(sym->right, array);
}

static void copy_table_ords(symtab_t *table, const symbol_t *array)
{
    for (int i = 0; i < HASH_SIZE; i++) {
        for (symbol_t *sym = table->hash[i]; sym != NULL; sym = sym->next) {
            copy_ords(sym, array);
        }
    }
}

/* collect, sort and number the symbols of a table.  A variable with
 * DeMorgan level 2 gets a twin "<name>$demorg". */
static void collect_symbols(symtab_t *table, symbol_t *array)
{
    symbol_t *fill = array;
    for (int i = 0; i < HASH_SIZE; i++) {
        for (const symbol_t *sym = table->hash[i]; sym != NULL; sym = sym->next) {
            flatten_symbols(sym, &fill);
        }
    }
    symbol_t *twin = &array[g_parse_total_syms];
    for (int i = 0; i < g_parse_total_syms; i++) {
        if ((array[i].demorgan & 0x7f) != 2) {
            continue;
        }
        *twin = array[i];
        char *name = parse_alloc_tracked(strlen(text_or_empty(array[i].name)) + sizeof("$demorg"), "");
        strcpy(name, text_or_empty(array[i].name));
        strcat(name, "$demorg");
        twin->name = name;
        twin->ext = parse_dup_string(text_or_empty(array[i].ext), "");
        array[i].demorgan = 0x80;
        twin->demorgan = 0x82;
        twin++;
    }
    g_parse_total_syms += g_demorgan_count;
    sort_symbols(array, g_parse_total_syms);
    for (int i = 0; i < g_parse_total_syms; i++) {
        array[i].ord = (uint16_t)g_next_ord++;
    }
    copy_table_ords(table, array);
    /* the references in the expressions now point into the array */
    for (int i = 0; i < g_parse_total_syms; i++) {
        if (array[i].expr == NULL) {
            continue;
        }
        for (token_t *tok = array[i].expr->head; tok != NULL; tok = tok->next) {
            if (parse_is_variable_token(tok) && (tok->flags & TF_SYMBOL)) {
                tok->u.sym = &array[tok->u.sym->ord - 2];
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * REGISTER_SELECT: flip-flop type conversion
 * ------------------------------------------------------------------------- */

/* code of a flip-flop input extension */
static int flipflop_code(const char *ext)
{
    static const struct { const char *ext; int code; } codes[] = {
        { "d", 1 }, { "t", 27 }, { "j", 10 }, { "k", 11 }, { "r", 9 }, { "s", 8 }
    };
    for (int i = 0; i < 6; i++) {
        if (strcmp(ext != NULL ? ext : "", codes[i].ext) == 0) {
            return codes[i].code;
        }
    }
    return 0;
}

/* the extension a REGISTER_SELECT level converts to */
static const char *select_extension(int level)
{
    switch (level) {
    case 1: return "d";
    case 2: return "t";
    case 3: return "j";
    case 4: return "s";
    default: return NULL;
    }
}

/* does the device have the flip-flop type of this level */
static int device_has_register(int level)
{
    if (level == 0) {
        return 1;
    }
    if (level > 0 && level < 5) {
        return parse_dev_ext_code(select_extension(level), g_parse_device) != -1;
    }
    return parse_dev_ext_code("d", g_parse_device) != -1 && parse_dev_ext_code("t", g_parse_device) != -1;
}

static int needs_companion(int regtype)
{
    return regtype == 0x103 || regtype == 0x1b03 || regtype == 0x104 || regtype == 0x1b04
        || regtype == 0x105 || regtype == 0x1b05;
}

/* mark the flip-flop inputs of the variable `sel` (which carries
 * a REGISTER_SELECT level) for conversion.  The inputs are found by
 * comparing only as many characters as the name of `sel` has. */
static void apply_register_select(const symbol_t *sel, symbol_t *array)
{
    int companions = g_companion_count;
    int first = 1;
    int level = sel->regtype;
    size_t len = strlen(text_or_empty(sel->name));
    for (int i = 0; i < g_parse_total_syms; i++) {
        symbol_t *sym = &array[i];
        int code = flipflop_code(sym->ext);
        if (strncmp(text_or_empty(sel->name), text_or_empty(sym->name), len) != 0 || sym->expr == NULL || code == 0) {
            continue;
        }
        if (!first && level != 5) {
            sym->regtype = (uint16_t)(code << 8);
            continue;
        }
        sym->regtype = (uint16_t)((code << 8) + (level & 0xff));
        if (needs_companion(sym->regtype)) {
            g_companion_count++;
        }
        const char *other_ext;
        if (sym->regtype == 0x105) {
            other_ext = select_extension(1);
        } else if (sym->regtype == 0x1b05) {
            other_ext = select_extension(2);
        } else {
            other_ext = select_extension(level & 0xff);
        }
        if (other_ext == NULL) {
            other_ext = "";
        }
        /* take DeMorgan level and register type of the target extension */
        for (int j = 0; j < g_parse_total_syms; j++) {
            if (strcmp(text_or_empty(array[j].name), text_or_empty(sym->name)) == 0
                && strcmp(array[j].ext != NULL ? array[j].ext : "", other_ext) == 0) {
                sym->demorgan = array[j].demorgan;
                sym->regtype |= array[j].regtype & 0xff;
                break;
            }
        }
        if (sym->regtype != 0x105 && sym->regtype != 0x1b05) {
            sym->ext = select_extension(level & 0xff);
        }
        first = 0;
        if (companions != g_companion_count) {
            return;
        }
    }
}

/* apply each REGISTER_SELECT level whose register type the device has;
 * the others are dropped with a warning */
static void check_register_selects(symbol_t *array)
{
    for (int i = 0; i < g_parse_total_syms; i++) {
        symbol_t *sym = &array[i];
        if (sym->regtype == 0 || (sym->regtype >> 8) != 0) {
            continue;
        }
        if (!device_has_register(sym->regtype)) {
            parse_warning_at(ERR_REGISTER_TYPE, sym->name);
            sym->regtype = 0;
        } else {
            apply_register_select(sym, array);
            if (sym->ext == NULL || sym->ext[0] == '\0') {
                sym->regtype = 0;
            }
        }
    }
}

/* D/T conversions and SR/JK to D/T conversions need a
 * second equation: add a twin symbol for it */
static void add_companions(symbol_t *array, int count)
{
    symbol_t *twin = &array[count];
    for (int i = 0; i < count; i++) {
        symbol_t *sym = &array[i];
        if (!needs_companion(sym->regtype)) {
            continue;
        }
        *twin = *sym;
        twin->name = parse_dup_string(text_or_empty(sym->name), "");
        const char *ext = sym->ext != NULL ? sym->ext : "";
        if (sym->regtype == 0x105) {
            twin->ext = "t";
        } else if (sym->regtype == 0x1b05) {
            twin->ext = "d";
        } else if (ext[0] == 's') {
            twin->ext = "r";
        } else if (ext[0] == 'j') {
            twin->ext = "k";
        } else {
            twin->ext = "";
        }
        if (sym->regtype == 0x1b05) {
            sym->regtype = 5;
            twin->regtype = 0x1b01;
        } else if (sym->regtype == 0x105) {
            sym->regtype = 5;
            twin->regtype = 0x102;
        }
        twin->ord = (uint16_t)g_next_ord++;
        twin++;
    }
}

/* -------------------------------------------------------------------------
 * Header
 * ------------------------------------------------------------------------- */

/* total length of the header strings.  Missing items are added
 * as "xxxxxx" (ASSEMBLY/ASSY, LOC/LOCATION and REV/REVISION are pairs of
 * which one is enough). */
static int header_strings_length(symtab_t *table)
{
    int have_assembly = 0;
    int have_loc = 0;
    int have_rev = 0;
    int missing = 0;
    int total = 0;
    for (int i = 0; i < 11; i++) {
        int item = k_header_items[i];
        token_t *tok = parse_list_find_type(&table->header, item);
        if (tok == NULL) {
            if (item == TOK_ASSEMBLY || item == TOK_LOC || item == TOK_REV
                || (item == TOK_ASSY && have_assembly) || (item == TOK_LOCATION && have_loc)
                || (item == TOK_REVISION && have_rev)) {
                continue;
            }
            missing = 1;
            tok = parse_token_alloc();
            tok->type = (uint8_t)item;
            tok->u.text = "xxxxxx";
            parse_list_append(tok, &table->header);
        }
        if (item == TOK_ASSEMBLY) {
            have_assembly = 1;
        } else if (item == TOK_LOC) {
            have_loc = 1;
        } else if (item == TOK_REV) {
            have_rev = 1;
        }
        if (tok->u.text == NULL) {
            tok->u.text = g_parse_empty_ext;
        }
        total += (int)strlen(tok->u.text) + 1;
    }
    if (missing) {
        parse_report_now(ERR_MISSING_HEADER, 1, NULL, NULL);
    }
    return total;
}

/* the header record.  It is written twice: first with the number of
 * symbols known before the register conversions, and again over the first
 * when the table is complete.  Bytes that no field sets are zero. */
static void write_header(symtab_t *table, int symbol_count, int with_strings)
{
    uint8_t rec[CF_HEADER_SIZE];
    uint8_t *entry = rec + CF_HDR_DEVICE;
    const char *device = parse_device_entry_name(table->device);
    memset(rec, 0, sizeof(rec));
    wr16(rec + CF_HDR_MAGIC, CF_MAGIC_CB);
    wr16(rec + CF_HDR_SYMBOLS, (uint32_t)symbol_count);
    if (device == NULL) {
        parse_fatal_error(1042, "");
    }
    entry[DL_ENTRY_FLAGS] = table->device->entry[DL_ENTRY_FLAGS];
    memcpy(entry + DL_ENTRY_LENGTH, table->device->entry + DL_ENTRY_LENGTH, 8);
    snprintf((char *)entry, DL_ENTRY_NAME_SIZE + 1, "%s", device);
    strncpy((char *)rec + CF_HDR_DESIGN, table->name, 0x1c);
    if (g_parse_max_min_level < g_parse_min_level) {
        g_parse_max_min_level = g_parse_min_level;
    }
    rec[CF_HDR_FORMAT] = (g_parse_max_min_level < 2) ? ':' : '.';
    if (table->format != NULL) {
        strncat((char *)rec + CF_HDR_FORMAT, table->format, 0x10);
    }
    rec[CF_HDR_FORMAT + 0x0f] = 0;
    wr16(rec + CF_HDR_EXTRA, (uint32_t)header_strings_length(table));
    cf_write_header(g_out, rec);
    fflush(g_out);
    if (ferror(g_out)) {
        parse_fatal_error(ERR_DISK_WRITE, g_parse_out_name);
    }
    if (!with_strings) {
        return;
    }
    for (int i = 0; i < 11; i++) {
        const token_t *tok = parse_list_find_type(&table->header, k_header_items[i]);
        if (tok != NULL) {
            if (fwrite(tok->u.text, strlen(tok->u.text) + 1, 1, g_out) != 1) {
                parse_fatal_error(ERR_DISK_WRITE, g_parse_out_name);
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * Equations
 * ------------------------------------------------------------------------- */

/* control extensions are always minimised with level 1 */
static int is_control_extension(const char *ext)
{
    static const char *const names[] = { "oe", "oemux", "ck", "ckmux", "ar", "ap", "sr", "sp", "iockmux", NULL };
    for (int i = 0; ext != NULL && names[i] != NULL; i++) {
        if (strcmp(ext, names[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

/* the symbol record of a variable; a variable without an explicit MIN level
 * gets the level of flag m (control extensions: 1) */
static void write_symbol(symbol_t *sym)
{
    int is_list = sym->type == SYM_FIELD || sym->type == SYM_GROUP;
    if (!is_list && sym->expr != NULL && !(sym->flags & SF_ASSIGNED)) {
        sym->flags |= SF_ASSIGNED;
        parse_report_now(1024, 2, sym->name, sym->ext);
    }
    if (!is_list && !(sym->flags & SF_MIN_SET)) {
        if (is_control_extension(sym->ext)) {
            sym->flags |= 1;
        } else {
            sym->flags |= (uint8_t)(g_parse_min_level & SF_MIN_MASK);
        }
    }
    write_symbol_record(sym);
}

/* point the references at the final array */
static void rebind_references(symbol_t *sym, symbol_t *array, int count)
{
    if (sym->expr == NULL) {
        return;
    }
    for (token_t *tok = sym->expr->head; tok != NULL; tok = tok->next) {
        if (!parse_is_variable_token(tok) || !(tok->flags & TF_SYMBOL)) {
            continue;
        }
        const symbol_t *old = tok->u.sym;
        for (int i = 0; i < count; i++) {
            if (strcmp(text_or_empty(array[i].name), text_or_empty(old->name)) == 0
                && (old->ext == array[i].ext
                    || strcmp(array[i].ext != NULL ? array[i].ext : "", old->ext != NULL ? old->ext : "") == 0)) {
                tok->u.sym = &array[i];
                break;
            }
        }
    }
}

static void emit_expression(const symbol_t *parent, const symbol_t *sym, int depth);

/* an intermediate variable is replaced by its expression in
 * parentheses, negated if exactly one of reference and definition is.
 * `parent` is the variable whose expression contains the reference. */
static void emit_intermediate(const symbol_t *parent, const token_t *ref, int depth)
{
    const symbol_t *sym = ref->u.sym;
    int negated = ((sym->flags & SF_LHS_NEGATED) != 0) != ((ref->flags & TF_NEGATED) != 0);
    write_op(TOK_LPAREN, negated);
    if (sym->expr == NULL) {
        parse_report_now(ERR_NOT_ASSIGNED, 0, sym->name, NULL);
    }
    /* emit_expression() only notices a variable that refers to itself or to
     * the variable it is being expanded in; a longer cycle is caught by the
     * depth limit. */
    if (depth >= MAX_EXPAND_DEPTH) {
        parse_fatal_error(ERR_SELF_DEPENDENT, sym->name);
    }
    emit_expression(parent, sym, depth + 1);
    write_op(TOK_RPAREN, 0);
}

/* the expression of a variable; `parent` is the variable in
 * whose expression it is being expanded (NULL at the top) */
static void emit_expression(const symbol_t *parent, const symbol_t *sym, int depth)
{
    if (sym->expr == NULL) {
        return;
    }
    for (token_t *tok = sym->expr->head; tok != NULL; tok = tok->next) {
        if (!parse_is_variable_token(tok)) {
            write_token(tok);
            continue;
        }
        tok->type = TOK_IDENT;
        if (!(tok->flags & TF_SYMBOL)) {
            parse_fatal_error(1035, "");
        }
        const symbol_t *ref = tok->u.sym;
        switch (ref->type) {
        case SYM_PIN:
        case SYM_PIN_EXT:
        case SYM_NODE:
        case SYM_NODE_EXT:
            write_symbol_ref(tok->flags, ref);
            break;
        case SYM_VARIABLE:
            if (ref == parent) {
                parse_fatal_error(ERR_SELF_DEPENDENT, ref->name);
            }
            if (sym->type == SYM_FIELD || sym->type == SYM_GROUP) {
                write_symbol_ref(tok->flags, ref);
            } else {
                emit_intermediate(sym, tok, depth);
            }
            break;
        default:
            parse_report_now(1041, 2, ref->name, ref->ext);
            break;
        }
    }
}

/* the expression of a symbol, or the constant 0 if there is none */
static void emit_or_zero(const symbol_t *sym)
{
    if (sym == NULL) {
        write_record(TOK_NUMBER, TF_NUMBER, 0);
    } else {
        emit_expression(NULL, sym, 0);
    }
}

/* the symbol `name` whose extension has the flip-flop code */
static const symbol_t *find_flipflop_input(const char *name, int code, const symbol_t *array)
{
    for (int i = 0; i < g_parse_total_syms; i++) {
        if (strcmp(text_or_empty(array[i].name), name) == 0 && (array[i].regtype & 0xff00) == (code << 8)) {
            return &array[i];
        }
    }
    return NULL;
}

/* the other input of a two-input flip-flop whose input `sym` is
 * converted: apply_register_select leaves it unconverted, and
 * write_table renames it to the intermediate ".<name>".  The original
 * looked for the dotted name only when converting JK to D; for SR to D,
 * SR to T and JK to T it found nothing and dropped that input's terms. */
static const symbol_t *find_partner_input(const symbol_t *sym, int code, const symbol_t *array)
{
    char dotted[300];
    snprintf(dotted, sizeof(dotted), ".%s", text_or_empty(sym->name));
    const symbol_t *partner = find_flipflop_input(dotted, code, array);
    return partner != NULL ? partner : find_flipflop_input(text_or_empty(sym->name), code, array);
}

/* the output variable that a flip-flop input belongs to */
static const symbol_t *find_output(const symbol_t *sym, const symbol_t *array, int count)
{
    char name[260];
    const char *full = text_or_empty(sym->name);
    snprintf(name, sizeof(name), "%s", full[0] == '.' ? full + 1 : full);
    char *suffix = strrchr(name, '$');
    if (suffix != NULL && sym->demorgan != 0 && strcmp(suffix, "$demorg") == 0) {
        *suffix = '\0';
    }
    /* An output with a DeMorgan twin ("<name>$demorg", 0x82) is marked
     * 0x80; only its level must be 0.  The original asked for no marker
     * either and stopped with "symbol table corrupted" when it converted
     * an output declared with DEMORGAN level 2. */
    for (int i = 0; i < count; i++) {
        if (strcmp(text_or_empty(array[i].name), name) == 0 && (array[i].ext == NULL || array[i].ext[0] == '\0')
            && (array[i].demorgan & 0x7f) == 0) {
            return &array[i];
        }
    }
    return NULL;
}

#define OPEN() write_op(TOK_LPAREN, 0)
#define OPEN_NOT() write_op(TOK_LPAREN, TF_NEGATED)
#define CLOSE() write_op(TOK_RPAREN, 0)
#define AND() write_op(TOK_AND, 0)
#define OR() write_op(TOK_OR, 0)
#define XOR() write_op(TOK_XOR, 0)
#define Q() write_record(TOK_IDENT, 2, out->ord)       /* the output itself */
#define NOT_Q() write_record(TOK_IDENT, 3, out->ord)

/* the equation of a flip-flop input that is converted to another
 * flip-flop type.  The regtype is
 * <extension code> << 8 | <REGISTER_SELECT level>. */
static void emit_converted(const symbol_t *sym, const symbol_t *array, int count)
{
    const symbol_t *out = find_output(sym, array, count);
    const symbol_t *set;
    const symbol_t *reset;
    if (out == NULL) {
        parse_fatal_error(1050, "symbol table corrupted");
    }
    int code = sym->regtype >> 8;
    switch (sym->regtype) {
    case 0x102:             /* D from T: (d) $ (q) */
    case 0x1b01:            /* T from D */
        OPEN(); OPEN(); emit_expression(NULL, sym, 0); CLOSE(); XOR(); OPEN(); Q(); CLOSE(); CLOSE();
        break;
    case 0x103:
        write_op(TOK_LPAREN, flipflop_code(sym->ext) == 11);
        emit_expression(NULL, sym, 0);
        CLOSE();
        break;
    case 0x104:
        write_op(TOK_LPAREN, flipflop_code(sym->ext) == 9);
        emit_expression(NULL, sym, 0);
        CLOSE();
        break;
    case 0x1b03:
        OPEN(); emit_expression(NULL, sym, 0); CLOSE();
        break;
    case 0x1b04:
        OPEN(); OPEN(); emit_expression(NULL, sym, 0); CLOSE(); AND();
        write_op(TOK_LPAREN, flipflop_code(sym->ext) == 9 ? 0 : TF_NEGATED);
        Q(); CLOSE(); CLOSE();
        break;
    case 0x801:             /* D from SR: (s) # (!(r) & (q)) */
    case 0x901:
    case 0x802:             /* T from SR: ((s) & !q) # ((r) & q) */
    case 0x902:
        if (code == 8) {
            set = sym;
            reset = find_partner_input(sym, 9, array);
        } else {
            reset = sym;
            set = find_partner_input(sym, 8, array);
        }
        if ((sym->regtype & 0xff) == 1) {
            OPEN(); OPEN(); emit_or_zero(set); CLOSE(); OR(); OPEN(); OPEN_NOT(); emit_or_zero(reset); CLOSE();
            AND(); OPEN(); Q(); CLOSE(); CLOSE(); CLOSE();
        } else {
            OPEN(); OPEN(); OPEN(); emit_or_zero(set); CLOSE(); AND(); NOT_Q(); CLOSE(); OR();
            OPEN(); OPEN(); emit_or_zero(reset); CLOSE(); AND(); Q(); CLOSE(); CLOSE();
        }
        break;
    case 0xa01:             /* D from JK: ((j) & !q) # ((j) & !(k)) # (!(k) & q) */
    case 0xb01:
    case 0xa02:             /* T from JK */
    case 0xb02:
        if (code == 10) {
            set = sym;
            reset = find_partner_input(sym, 11, array);
        } else {
            reset = sym;
            set = find_partner_input(sym, 10, array);
        }
        if ((sym->regtype & 0xff) == 1) {
            OPEN(); OPEN(); OPEN(); emit_or_zero(set); CLOSE(); AND(); NOT_Q(); CLOSE(); OR();
            OPEN(); OPEN(); emit_or_zero(set); CLOSE(); AND(); OPEN_NOT(); emit_or_zero(reset); CLOSE(); CLOSE(); OR();
            OPEN(); OPEN_NOT(); emit_or_zero(reset); CLOSE(); AND(); Q(); CLOSE(); CLOSE();
        } else {
            OPEN(); OPEN(); OPEN(); emit_or_zero(set); CLOSE(); AND(); OPEN(); emit_or_zero(reset); CLOSE(); CLOSE(); OR();
            OPEN(); OPEN_NOT(); emit_or_zero(set); CLOSE(); AND(); OPEN(); emit_or_zero(reset); CLOSE(); AND(); Q(); CLOSE(); OR();
            OPEN(); OPEN(); emit_or_zero(set); CLOSE(); AND(); OPEN_NOT(); emit_or_zero(reset); CLOSE(); AND(); NOT_Q(); CLOSE(); CLOSE();
        }
        break;
    default:
        OPEN(); emit_expression(NULL, sym, 0); CLOSE();
        break;
    }
}

/* <variable> ( expression ) .pend */
static void emit_equation(const symbol_t *sym, const symbol_t *array, int count)
{
    if (sym->type == SYM_FUNCTION || sym->expr == NULL) {
        return;
    }
    write_record(TOK_IDENT, 10, sym->ord);
    if (sym->regtype == 0 || sym->regtype == 5) {
        OPEN();
        emit_expression(NULL, sym, 0);
        CLOSE();
    } else {
        emit_converted(sym, array, count);
    }
    write_op(TOK_PEND, 0);
}

/* write one symbol table */
static void write_table(symtab_t *table)
{
    symbol_t marker;

    for (int i = 0; i < HASH_SIZE; i++) {
        for (symbol_t *sym = table->hash[i]; sym != NULL; sym = sym->next) {
            resolve_list_symbols(sym, table);
        }
    }
    memset(&marker, 0, sizeof(marker));

    g_demorgan_count = 0;
    for (int i = 0; i < HASH_SIZE; i++) {
        for (symbol_t *sym = table->hash[i]; sym != NULL; sym = sym->next) {
            settle_demorgan(sym);
        }
    }
    int twins = g_demorgan_count;
    write_header(table, table->nsyms + g_demorgan_count + 3, 1);

    marker.type = 6;
    g_next_ord = 1;
    write_symbol_record(&marker);
    marker.ord = (uint16_t)g_next_ord++;
    marker.type = 7;
    write_symbol_record(&marker);

    symbol_t *first = parse_alloc_tracked((size_t)(g_parse_total_syms + twins + 1) * sizeof(symbol_t), "");
    collect_symbols(table, first);
    check_register_selects(first);

    int count = g_parse_total_syms;
    symbol_t *array = parse_alloc_tracked((size_t)(count + g_companion_count + 1) * sizeof(symbol_t), "");
    memcpy(array, first, (size_t)count * sizeof(symbol_t));
    add_companions(array, count);
    count += g_companion_count;
    sort_symbols(array, count);
    for (int i = 0; i < count; i++) {
        array[i].ord = (uint16_t)(i + 2);
    }
    copy_table_ords(table, array);

    for (int i = 0; i < count; i++) {
        symbol_t *sym = &array[i];
        if (sym->regtype != 0 && (sym->regtype & 0xff) == 0) {
            /* a flip-flop input replaced by a conversion becomes the
             * intermediate ".<name>" */
            char *name = parse_alloc_tracked(strlen(text_or_empty(sym->name)) + 2, "");
            strcpy(name, ".");
            strcat(name, text_or_empty(sym->name));
            sym->name = name;
            sym->type = SYM_VARIABLE;
        }
        write_symbol(sym);
    }
    for (int i = 0; i < count; i++) {
        symbol_t *sym = &array[i];
        if (sym->type != SYM_FUNCTION && sym->expr != NULL && (sym->regtype == 0 || sym->regtype == 5)) {
            rebind_references(sym, array, count);
        }
    }
    marker.type = 4;
    marker.name = NULL;
    marker.ext = NULL;
    write_symbol_record(&marker);
    for (int i = 0; i < count; i++) {
        emit_equation(&array[i], array, count);
    }
}

/* write all symbol tables to CB<pid> */
void parse_write_output(void)
{
    static const char pattern[] = "CBXXXXXX";
    g_parse_out_name = parse_dup_string(pattern, "");
    phase_temp_name(g_parse_out_name, g_parse_pid_text);
    g_out = parse_open_file(g_parse_out_name, "wb");
    if (g_out == NULL) {
        parse_report_now(ERR_OPEN, 0, g_parse_out_name, NULL);
        return;
    }
    for (symtab_t *table = g_parse_first_table; table != NULL; table = table->next) {
        write_table(table);
        fseek(g_out, 0, SEEK_SET);
        write_header(table, table->nsyms + g_demorgan_count + 3 + g_companion_count, 0);
        fseek(g_out, 0, SEEK_END);
        write_op(table->next == NULL ? TOK_FINI : TOK_XEND, 0);
    }
    fflush(g_out);
    if (ferror(g_out)) {
        parse_fatal_error(ERR_DISK_WRITE, g_parse_out_name);
    }
    parse_close_file(g_out);
}

