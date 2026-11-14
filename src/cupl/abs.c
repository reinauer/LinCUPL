/*
 * abs.c - absolute file (<design>.abs) and equation listing (<design>.sim)
 * for the simulator.
 *
 * The absolute file has the layout of the intermediate file: header, symbol
 * table, then one record per OR array (12-byte head and the product terms
 * of all lists on that array), then the field lists, then an end record.
 */
#include "fusemap.h"

static cf_frame_t s_rec;            /* head of the record being written */
static FILE *s_sim_fp;              /* <design>.sim */
static char *s_sim_name;
static int s_col;                   /* output column in the .sim file */

void abs_reset(void)
{
    memset(&s_rec, 0, sizeof(s_rec));
    s_sim_fp = NULL;
    s_sim_name = NULL;
    s_col = 0;
}

/* Create <design>.abs */
void abs_open(void)
{
    memset(&s_rec, 0, sizeof(s_rec));
    g.abs_name = make_path(source_dir(g.args[5]), g.args[5], ".abs");
    g.abs_fp = open_file(g.abs_name, "wb");
    if (g.abs_fp == NULL) {
        fatal_error(ERR_OPEN, g.abs_name);
    }
}

/* Check the result of a write */
static void check_put(int ok)
{
    if (!ok) {
        fatal_error(ERR_DISK_WRITE, g.abs_name);
    }
    check_write(g.abs_fp, g.abs_name);
}

static void put_bytes(const uint8_t *data, size_t size)
{
    check_put(size != 0 && fwrite(data, size, 1, g.abs_fp) == 1);
}

static void put_frame(const cf_frame_t *head)
{
    check_put(cf_write_frame(g.abs_fp, head));
}

static void set_record_head(unsigned flags, unsigned or_array, unsigned nvars, unsigned nterms, unsigned max_terms)
{
    s_rec.flags = (uint16_t)flags;
    s_rec.or_array = (uint16_t)or_array;
    s_rec.nvars = (uint16_t)nvars;
    s_rec.nrows = (uint16_t)nterms;
    s_rec.max_terms = (uint16_t)max_terms;
}

/* The product terms of a list.  A list without terms is reported as a write
 * error (nothing is written); callers skip empty lists except for fields. */
static void put_terms(equation_t *eq)
{
    eq->total_words = eq->nterms * eq->term_words;
    size_t count = (size_t)eq->total_words;
    check_put(count != 0 && cf_write_words(g.abs_fp, eq->terms, count) == count);
}

/* Header of the absolute file: its own magic number and version, the symbol
 * count, the library directory entry, device and design name, then the
 * header strings of the intermediate file */
static void put_header(void)
{
    uint8_t hdr[CF_HEADER_SIZE];
    memset(hdr, 0, sizeof(hdr));
    wr16(hdr + CF_HDR_MAGIC, CF_MAGIC_ABS);
    wr16(hdr + CF_HDR_VERSION, CF_ABS_VERSION);
    wr16(hdr + CF_HDR_SYMBOLS, (unsigned)g.nsyms);
    size_t extra = cf_header_extra(g.hdr);
    wr16(hdr + CF_HDR_EXTRA, (unsigned)extra);
    /* library directory entry: flag, size, offset */
    memcpy(hdr + CF_HDR_DEVICE + DL_ENTRY_FLAGS, g.hdr + CF_HDR_DEVICE + DL_ENTRY_FLAGS, 9);
    snprintf((char *)hdr + CF_HDR_DEVICE, DL_ENTRY_NAME_SIZE + 1, "%s", g.device_name);
    snprintf((char *)hdr + CF_HDR_DESIGN, CF_HDR_DEVICE - CF_HDR_DESIGN, "%s", g.design_name);
    check_put(cf_write_header(g.abs_fp, hdr));
    if (extra != 0) {
        put_bytes((const uint8_t *)g.hdr_strings, extra);
    }
    check_write(g.abs_fp, g.abs_name);
}

/* A symbol record as in the intermediate file.  The terms field is 1 if
 * the symbol has a product-term list, else 0; readers only test it for
 * zero. */
static void put_symbol(const symbol_t *s)
{
    cf_symbol_t rec;
    memcpy(rec.name, s->name, SYM_NAME_SIZE);
    memcpy(rec.ext, SYM_EXT(s), SYM_EXT_SIZE);
    rec.pin = s->pin;
    rec.type = s->type;
    rec.flags = s->flags;
    memcpy(rec.attr, s->b2e, 4);
    rec.polarity = s->b32;
    rec.attr2 = s->b33;
    rec.pin_index = s->pin_index;
    rec.node_index = s->node_index;
    rec.or_index = s->or_index;
    rec.ext_code = s->ext_code;
    rec.terms = s->eq != NULL ? 1 : 0;
    check_put(cf_write_symbol(g.abs_fp, &rec));
}

/* Symbol of the same name that is a register input or feedback */
static symbol_t *find_register_symbol(const char *name)
{
    static const char *const k_exts[] = { "d", "l", "j", "k", "s", "r", "t", "dq", "lq", NULL };
    for (int i = 0; i < g.nsyms; i++) {
        symbol_t *s = &g.syms[i];
        if (strcmp(s->name, name) != 0) {
            continue;
        }
        for (int k = 0; k_exts[k] != NULL; k++) {
            if (strcasecmp(SYM_EXT(s), k_exts[k]) == 0) {
                return s;
            }
        }
    }
    return NULL;
}

static int is_field(const symbol_t *s)
{
    return (s->type & 0x0f) == 1 || (s->type & 0x0f) == 0x0e;
}

/* Write the absolute file of the current design: header, symbol table, one
 * record per OR array (per list on virtual devices), then the fields */
void abs_write(void)
{
    if (!g.map_built || device_type() != DEVT_PROM) {
        build_fuse_map();
    }
    put_header();
    int is_virtual = device_type() == DEVT_VIRTUAL;
    if (is_virtual) {
        /* virtual devices have no nodes: number the lists and the inputs */
        int list = 1;
        int input = 1;
        for (int i = 0; i < g.nsyms; i++) {
            symbol_t *s = &g.syms[i];
            if (s->eq == NULL) {
                s->node_index = -1;
            } else {
                s->eq->or_array = (uint16_t)list++;
                s->node_index = (int16_t)s->eq->or_array;
            }
            if (attr8(ATTR_PLA_FORMAT) == 1) {
                s->pin_index = (s->flags & SYMF_INPUT) ? (int16_t)input++ : -1;
            } else if (s->pin_index == 0) {
                s->pin_index = -1;
            }
        }
        for (int i = 0; i < g.nsyms; i++) {
            symbol_t *s = &g.syms[i];
            if (SYM_EXT(s)[0] == '\0' && s->node_index < 0) {
                symbol_t *reg = find_register_symbol(s->name);
                if (reg != NULL && reg->node_index >= 0) {
                    s->node_index = reg->node_index;
                }
            }
        }
    }
    for (int i = 0; i < g.nsyms; i++) {
        put_symbol(&g.syms[i]);
    }
    if (is_virtual) {
        for (int i = 0; i < g.nsyms; i++) {
            symbol_t *s = &g.syms[i];
            if (s->eq != NULL && s->name[0] != '.' && !is_field(s)) {
                set_record_head(s->eq->flags, s->eq->or_array, s->eq->nvars, s->eq->nterms, s->eq->max_terms);
                put_frame(&s_rec);
                if (s->eq->nterms != 0) {
                    put_terms(s->eq);
                }
            }
        }
    } else {
        for (int i = 0; i < g.nor_arrays; i++) {
            equation_t *first = g.or_arrays[i].eq;
            if (first == NULL) {
                continue;
            }
            unsigned nterms = 0;
            for (equation_t *eq = first; eq != NULL; eq = eq->next) {
                nterms += eq->nterms;
            }
            set_record_head(first->flags, first->or_array, first->nvars, nterms & 0xffff, first->max_terms);
            put_frame(&s_rec);
            for (equation_t *eq = first; eq != NULL; eq = eq->next) {
                if (eq->nterms != 0) {
                    put_terms(eq);
                }
            }
        }
    }
    for (int i = 0; i < g.nsyms; i++) {
        symbol_t *s = &g.syms[i];
        if (is_field(s) && s->eq != NULL) {
            s->eq->flags |= CF_FRAME_FIELD;
            cf_frame_t head = { s->eq->flags, s->eq->or_array, s->eq->nvars,
                                s->eq->nterms, s->eq->w8, s->eq->max_terms };
            put_frame(&head);
            put_terms(s->eq);
        }
    }
}

/* End record; only the flag word is set, the rest is whatever the
 * last record head held */
void abs_end(int code)
{
    s_rec.flags = (uint16_t)code;
    put_frame(&s_rec);
}

/* -------------------------------------------------------------------------
 * <design>.sim
 * ------------------------------------------------------------------------- */

/* Create <design>.sim */
void sim_open(void)
{
    s_sim_name = make_path(source_dir(g.args[5]), g.args[5], ".sim");
    s_sim_fp = open_file(s_sim_name, "wt");
    if (s_sim_fp == NULL) {
        fatal_error(ERR_OPEN, s_sim_name);
    }
}

/* One variable of a product term: name, name.ext or a constant, '!' when
 * complemented; advances s_col */
static void sim_variable(int positive, int index)
{
    const symbol_t *s = &g.syms[index];
    FILE *fp = s_sim_fp;
    switch (s->type & 0x0f) {
    case 1:
    case 2:
    case 3:
    case 9:
    case 0x0e:
    case 0x0f:
        if (!positive) {
            out_putc('!', fp);
            s_col++;
        }
        out_printf(fp, "%s", s->name);
        s_col += (int)strlen(s->name);
        break;
    case 5:
    case 8:
    case 10:
        if (!positive) {
            out_putc('!', fp);
            s_col++;
        }
        out_printf(fp, "%s.%s ", s->name, SYM_EXT(s));
        s_col += (int)(strlen(s->name) + strlen(SYM_EXT(s)) + 2);
        break;
    case 6:
        out_printf(fp, positive ? "0 " : "1 ");
        break;
    case 7:
        out_printf(fp, positive ? "1 " : "0 ");
        break;
    default:
        if (!positive) {
            out_putc('!', fp);
        }
        out_printf(fp, "? ");
        break;
    }
}

static int is_list_type(const symbol_t *s)
{
    unsigned type = s->type & 0x0f;
    return type == 1 || type == 0x0f || type == 0x0e;
}

/* "FIELD name = a,b,c" */
static void sim_field(const equation_t *eq)
{
    FILE *fp = s_sim_fp;
    int is_list = 0;
    int sep = 0;
    for (int i = 0; i < eq->nvars; i++) {
        if (!term_bit(TERM_OUT(eq, 0), i)) {
            continue;
        }
        if (is_list_type(&g.syms[i])) {
            out_printf(fp, "FIELD ");
            sim_variable(!(g.syms[i].flags & SYMF_20), i);
            out_printf(fp, " = ");
            sep = ',';
            is_list = 1;
        } else {
            sep = '&';
        }
    }
    s_col = 4;
    if (!is_list) {
        return;
    }
    int first_term = 1;
    for (int t = 0; t < eq->nterms; t++) {
        if (TERM_CARE(eq, t)[0] & TERM_DELETED) {
            continue;
        }
        int first_var = 1;
        for (int i = 0; i < eq->nvars; i++) {
            if (!term_bit(TERM_CARE(eq, t), i)) {
                continue;
            }
            if (first_var && first_term) {
                first_var = 0;
            } else {
                out_printf(fp, "%c", sep);
                s_col += 3;
            }
            sim_variable(term_bit(TERM_VAL(eq, t), i), i);
            if (i == 0) {
                break;
            }
        }
        first_term = 0;
    }
    out_printf(fp, "\n");
    s_col = 0;
}

/* "name =>" and the sum of products; lists are skipped */
static void sim_equation(const equation_t *eq)
{
    FILE *fp = s_sim_fp;
    int is_list = 0;
    int last_is_list = 0;
    int sep = 0;
    for (int i = 0; i < eq->nvars; i++) {
        if (!term_bit(TERM_OUT(eq, 0), i)) {
            continue;
        }
        if (is_list_type(&g.syms[i])) {
            sep = ',';
            is_list = 1;
            last_is_list = 1;
        } else {
            sep = '&';
            last_is_list = 0;
            sim_variable(!(g.syms[i].flags & SYMF_20), i);
        }
    }
    if (last_is_list) {
        return;
    }
    out_printf(fp, " =>\n    ");
    int first_term = 1;
    s_col = 4;
    for (int t = 0; t < eq->nterms; t++) {
        const uint16_t *care = TERM_CARE(eq, t);
        if (care[0] & TERM_DELETED) {
            continue;
        }
        int first_var = 1;
        for (int i = 0; i < eq->nvars; i++) {
            if (!term_bit(care, i)) {
                continue;
            }
            if (!first_var || (is_list && !first_term)) {
                out_printf(fp, " %c ", sep);
                s_col += 3;
            } else {
                first_var = 0;
            }
            sim_variable(term_bit(TERM_VAL(eq, t), i), i);
            if (i == 0) {
                break;
            }
        }
        first_term = 0;
        if (care[0] & TERM_MORE) {
            out_printf(fp, "\n  # ");
            s_col = 4;
        } else if (care[0] & TERM_SPLIT) {
            out_printf(fp, "\n  $ ");
            s_col = 4;
        } else if (!is_list) {
            out_printf(fp, "\n");
            s_col = 0;
        }
    }
    out_putc('\n', fp);
}

static int is_listed(const symbol_t *s)
{
    return s->eq != NULL && s->name[0] != '.' && s->b32 != 2 && s->b33 != 5;
}

/* Write the %SIGNAL, %FIELD and %EQUATION sections of the .sim file */
void sim_write(void)
{
    FILE *fp = s_sim_fp;
    check_write(fp, s_sim_name);
    out_printf(fp, "%%SIGNAL\n");
    for (int i = 2; i < g.nsyms; i++) {
        const symbol_t *s = &g.syms[i];
        unsigned type = s->type & 0x0f;
        if (type == 2 || type == 9) {
            out_printf(fp, "PIN %3d = %c%s \n", (int)s->pin, (s->flags & SYMF_INVERTED) ? '!' : ' ', s->name);
        }
    }
    out_printf(fp, "%%END\n\n");
    fflush(fp);
    out_printf(fp, "%%FIELD\n");
    for (int i = 2; i < g.nsyms; i++) {
        if (is_listed(&g.syms[i])) {
            sim_field(g.syms[i].eq);
        }
    }
    out_printf(fp, "%%END\n\n");
    out_printf(fp, "%%EQUATION\n");
    for (int i = 2; i < g.nsyms; i++) {
        if (is_listed(&g.syms[i])) {
            sim_equation(g.syms[i].eq);
        }
    }
    out_printf(fp, "%%END\n");
}
