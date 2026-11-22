/*
 * doc.c - documentation file (<design>.doc) and fuse plots.
 *
 * The .doc file holds the design header, the expanded product terms, the
 * symbol table and, with the 'f' option, a fuse plot and a chip diagram.
 * The fuse plot is produced by a small program stored in section 8 of the
 * device record; the same interpreter writes the HL download file.
 */
#include "fusemap.h"
#include "strutil.h"

/* class letter of a symbol type */
static const char k_type_class[] = "UFVI X01DNMT  GB";

static int s_col;                   /* output column of the product-term listing */

/* fuse plot */
#define PLOT_REGS 256
static FILE *s_plot_fp;
static char *s_plot_name;
static int s_plot_line_reg;         /* register that counts the lines of a block */
static unsigned s_plot_group;       /* group of the block being written */
static int32_t s_plot_reg[PLOT_REGS]; /* registers of the plot program */

static char s_label[24];            /* result of symbol_label() */

void doc_reset(void)
{
    s_col = 0;
    s_plot_fp = NULL;
    s_plot_name = NULL;
    s_plot_line_reg = 0;
    s_plot_group = 0;
    memset(s_plot_reg, 0, sizeof(s_plot_reg));
}

/* Create <design>.doc */
void doc_open(void)
{
    g.doc_name = make_path(source_dir(g.args[5]), g.args[5], ".doc");
    g.doc_fp = open_file(g.doc_name, "wt");
    if (g.doc_fp == NULL) {
        fatal_error(ERR_OPEN, g.doc_name);
    }
}

/* Centred title between two rules */
static void write_title(FILE *fp, const char *text, int width, int rule)
{
    int pad = (width - (int)strlen(text)) / 2;
    out_putc('\n', fp);
    for (int i = 0; i < width; i++) {
        out_putc(rule, fp);
    }
    out_putc('\n', fp);
    for (int i = 0; i < pad; i++) {
        out_putc(' ', fp);
    }
    out_printf(fp, "%s\n", text);
    for (int i = 0; i < width; i++) {
        out_putc(rule, fp);
    }
    out_putc('\n', fp);
    fflush(fp);
}

/* Output that can carry a polarity mark */
static int is_polarity_output(const symbol_t *s)
{
    unsigned type = s->type & 0x0f;
    if (type != 2 && type != 5 && type != 9 && type != 10) {
        return 0;
    }
    const char *ext = SYM_EXT(s);
    if (ext[0] == '\0') {
        return 1;
    }
    return strcmp(ext, "d") == 0 || strcmp(ext, "t") == 0 || strcmp(ext, "l") == 0 || strcmp(ext, "j") == 0
           || strcmp(ext, "k") == 0 || strcmp(ext, "r") == 0 || strcmp(ext, "s") == 0;
}

/* -------------------------------------------------------------------------
 * Expanded product terms
 * ------------------------------------------------------------------------- */

/* Continue on a new line when the next name would pass column 79 */
static void wrap_line(int length)
{
    if (length + s_col + 2 > 0x4f) {
        out_printf(g.doc_fp, "\n      ");
        s_col = 6;
    }
}

/* One variable of a product term, '!' if negated */
static void write_variable(int positive, int index)
{
    const symbol_t *s = &g.syms[index];
    FILE *fp = g.doc_fp;
    switch (s->type & 0x0f) {
    case 1:
    case 2:
    case 3:
    case 9:
    case 0x0e:
    case 0x0f:
        wrap_line((int)strlen(s->name));
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
        wrap_line((int)(strlen(s->name) + strlen(SYM_EXT(s)) + 2));
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

/* "name =>" followed by the sum of products; fields and groups
 * are written as lists */
static void write_expanded(const equation_t *eq)
{
    FILE *fp = g.doc_fp;
    int is_list = 0;
    int sep = 0;
    for (int i = 0; i < eq->nvars; i++) {
        if (!term_bit(TERM_OUT(eq, 0), i)) {
            continue;
        }
        write_variable(!(g.syms[i].flags & SYMF_20), i);
        unsigned type = g.syms[i].type & 0x0f;
        if (type == 1 || type == 0x0f || type == 0x0e) {
            sep = ',';
            is_list = 1;
        } else {
            sep = '&';
        }
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
            write_variable(term_bit(TERM_VAL(eq, t), i), i);
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
    if (is_list) {
        out_printf(fp, "\n");
        s_col = 0;
    }
    out_putc('\n', fp);
}

/* "Expanded Product Terms" section of the .doc file */
static void write_expanded_terms(void)
{
    write_title(g.doc_fp, "Expanded Product Terms", 0x4f, '=');
    out_putc('\n', g.doc_fp);
    for (int i = 2; i < g.nsyms; i++) {
        const symbol_t *s = &g.syms[i];
        if (s->eq != NULL && s->name[0] != '.' && s->b32 != 2 && s->b33 != 5) {
            write_expanded(s->eq);
        }
    }
}

/* -------------------------------------------------------------------------
 * Symbol table
 * ------------------------------------------------------------------------- */

/* "Symbol Table" section of the .doc file, with its legend */
static void write_symbol_table(void)
{
    FILE *fp = g.doc_fp;
    out_putc('\f', fp);
    write_title(fp, "Symbol Table", 0x4f, '=');
    out_printf(fp, "\n");
    out_printf(fp, "%-4s%-20s%-8s%-8s%-8s%-8s%-8s%-8s\n", "Pin", "Variable", "", "", "", "Pterms", " Max", " Min");
    out_printf(fp, "%-4s%-20s%-8s%-8s%-8s%-8s%-8s%-8s\n", "Pol", "  Name", "Ext", "Pin", "Type", " Used", "Pterms",
               "Level");
    out_printf(fp, "%-4s%-20s%-8s%-8s%-8s%-8s%-8s%-8s\n\n", "---", "--------", "---", "---", "----", "------",
               "------", "-----");
    for (int i = 2; i < g.nsyms; i++) {
        const symbol_t *s = &g.syms[i];
        unsigned type = s->type & 0x0f;
        if (s->name[0] == '.' || s->b32 == 2 || s->b33 >= 6
            || (type == 3 && (s->eq == NULL || s->eq->nterms == 0))) {
            continue;
        }
        int pol = ((s->flags & SYMF_INVERTED) && is_polarity_output(s)) ? '!' : ' ';
        out_printf(fp, " %-3c%-20s%-8s%-8d %-7c", pol, s->name, SYM_EXT(s), (int)s->pin, k_type_class[type]);
        if (s->eq == NULL) {
            out_printf(fp, "  %-6s  %-6s  %-6s\n", "-", "-", "-");
            continue;
        }
        switch (type) {
        case 2:
        case 5:
        case 8:
        case 9:
        case 10:
            out_printf(fp, "  %-6d  %-6d  %-6d\n", (int)s->eq->nterms, (int)s->eq->max_terms, (int)(s->flags & 7));
            break;
        case 3:
        case 0x0b:
            out_printf(fp, "  %-6d  %-6s  %-6s\n", (int)s->eq->nterms, "-", "-");
            break;
        default:
            out_printf(fp, "  %-6s  %-6s  %-6s\n", "-", "-", "-");
            break;
        }
    }
    out_printf(fp, "\n\n%s\n", "LEGEND    D : default variable         F : field      G : group");
    out_printf(fp, "%s\n", "          I : intermediate variable    N : node       M : extended node");
    out_printf(fp, "%s\n", "          U : undefined                V : variable   X : extended variable");
    out_printf(fp, "%s\n\n", "          T : function");
    fflush(fp);
}

/* Header, expanded product terms and symbol table */
void doc_write_symbols(void)
{
    write_title(g.doc_fp, g.design_name, 0x4f, '*');
    out_putc('\n', g.doc_fp);
    check_write(g.doc_fp, g.doc_name);
    print_banner(g.doc_fp);
    write_expanded_terms();
    write_symbol_table();
}

/* Merged and used product-term totals (IFL and type 5 devices only) */
void doc_write_totals(void)
{
    if (device_type() == DEVT_IFL || device_type() == DEVT_5) {
        out_printf(g.doc_fp, "\nTotal product terms merged: %d\n", g.merged_terms);
        out_printf(g.doc_fp, "\nTotal product terms used by OR Arrays: %d\n\n", g.or_terms_used);
        fflush(g.doc_fp);
    }
}

/* -------------------------------------------------------------------------
 * Chip diagram
 * ------------------------------------------------------------------------- */

/* Pin label of a symbol: '!' and at most 20 characters */
static const char *symbol_label(const symbol_t *s)
{
    memset(s_label, 0, sizeof(s_label));
    if (s->flags & SYMF_INVERTED) {
        s_label[0] = '!';
        lincupl_strcpy_trunc(s_label + 1, 0x14, s->name);
    } else {
        lincupl_strcpy_trunc(s_label, 0x15, s->name);
    }
    s_label[0x14] = '\0';
    return s_label;
}

/* Label of a pin without a symbol */
static const char *unused_pin_label(unsigned number)
{
    unsigned kind = 0;
    for (int i = 0; i < npins(); i++) {
        if (pin16(i, PIN_NUMBER) == number) {
            kind = pin8(i, PIN_KIND);
            break;
        }
    }
    switch (kind) {
    case 2:
        return "GND";
    case 3:
        return "Vcc";
    case 4:
        return "Vee";
    case 0x13:
        return "NC";
    default:
        return "";
    }
}

static const char *pin_label(unsigned number)
{
    const symbol_t *found = NULL;
    for (int i = 2; i < g.nsyms; i++) {
        if (g.syms[i].pin == number && (g.syms[i].type & 0x0f) == 2) {
            found = &g.syms[i];
        }
    }
    return found != NULL ? symbol_label(found) : unused_pin_label(number);
}

/* Design name centred in the top of the DIP outline; a name of
 * more than 14 characters is cut (in place) */
static void write_dip_name(char *name, FILE *fp)
{
    int len = (int)strlen(name);
    int left = 0;
    int right = 0;
    stream_printf(fp, "%30c|", ' ');
    if (len < 0x0f) {
        left = (0x0e - len) / 2;
        right = (0x0e - len) % 2 + left;
    } else {
        name[0x0e] = '\0';
    }
    for (int i = 0; i < left; i++) {
        stream_printf(fp, " ");
    }
    stream_printf(fp, "%s", name);
    for (int i = 0; i < right; i++) {
        stream_printf(fp, " ");
    }
    stream_printf(fp, "|\n");
    fflush(fp);
}

/* Dual in-line package */
static void write_dip_diagram(FILE *fp)
{
    unsigned count = attr8(ATTR_NPINS);
    unsigned left = 1;
    unsigned right = count;
    char label[32];
    stream_printf(fp, "\f");
    fflush(fp);
    write_title(fp, "Chip Diagram", 0x4f, '=');
    stream_printf(fp, "\n%31c______________\n", ' ');
    fflush(fp);
    write_dip_name(g.design_name, fp);
    for (unsigned i = 0; i < count / 2; i++) {
        snprintf(label, sizeof(label), "%s", pin_label(left));
        stream_printf(fp, "%25s", label);
        stream_printf(fp, " x---|%-2d%10c%2d|---x ", (int)left, ' ', (int)right);
        snprintf(label, sizeof(label), "%s", pin_label(right));
        stream_printf(fp, "%-25s\n", label);
        left++;
        right--;
        fflush(fp);
    }
    stream_printf(fp, "%30c|______________|\n\n", ' ');
    fflush(fp);
}

static void append(char *dst, size_t cap, const char *src)
{
    size_t used = strlen(dst);
    if (used < cap) {
        snprintf(dst + used, cap - used, "%s", src);
    }
}

/* Put the two digits of a pin number in the same column of two lines */
static void put_digits(char *tens, char *ones, size_t cap, int number)
{
    char d[16];
    snprintf(d, sizeof(d), "%d", number);
    size_t n = strlen(tens);
    if (n + 2 < cap) {
        tens[n] = d[0];
        tens[n + 1] = '\0';
        ones[n] = d[1];
        ones[n + 1] = '\0';
    }
}

/* Square (LCC) package, drawn for up to 68 pins */
static void write_lcc_diagram(FILE *fp)
{
    int count = (int)attr8(ATTR_NPINS);
    int side = count >> 2;
    char line[256];
    char tens[128];
    char ones[128];
    char label[32];
    if (side * 4 != count || count >= 0x45) {
        return;
    }
    int narrow = count >= 0x44;         /* 68 pins: two columns per pin instead of three */
    int half = count >> 3;
    int inner = side - 2;
    int left = 1;
    int right = count;
    int step = narrow ? 2 : 3;
    const char *bar = narrow ? " |" : "  |";
    const char *gap = narrow ? " " : "  ";
    stream_printf(fp, "\f");
    write_title(fp, "Chip Diagram", 0x4f, '=');
    stream_printf(fp, "\n");
    snprintf(line, sizeof(line), "%*s", 0x14, pin_label((unsigned)left));
    int w = 0x13;
    stream_printf(fp, "%*c", w, ' ');
    stream_printf(fp, "%s\n", line);
    fflush(fp);
    w = 0x14;
    left++;
    for (int i = 0; i < half; i++, left++) {
        snprintf(line, sizeof(line), "%*s", 0x14, pin_label((unsigned)left));
        for (int k = 0; k < i * 2 + 1; k++) {
            append(line, sizeof(line), bar);
        }
        append(line, sizeof(line), gap);
        append(line, sizeof(line), pin_label((unsigned)right));
        w -= step;
        stream_printf(fp, "%*c", w, ' ');
        stream_printf(fp, "%s\n", line);
        fflush(fp);
        right--;
    }
    w = 0x28 - (half + 1) * step;
    stream_printf(fp, "%*c", w, ' ');
    fflush(fp);
    for (int k = 0; k < side; k++) {
        stream_printf(fp, "%*cx", step - 1, ' ');
    }
    stream_printf(fp, "\n%*c", narrow ? w : w + 1, ' ');
    for (int k = 0; k < side; k++) {
        stream_printf(fp, narrow ? "_|" : "_|_");
    }
    stream_printf(fp, narrow ? "__\n" : "_\n");
    fflush(fp);
    /* pin numbers of the top side, counting down from the pin left of pin 1 */
    int number = left - 1;
    int first = 1;
    tens[0] = ones[0] = '\0';
    for (int k = 0; k < side; k++) {
        if (!narrow) {
            append(tens, sizeof(tens), first ? " " : "  ");
            append(ones, sizeof(ones), "  ");
        } else {
            append(tens, sizeof(tens), " ");
            append(ones, sizeof(ones), first ? "  " : " ");
        }
        first = 0;
        if (number == 0) {
            number = count;
        }
        if (number < 10) {
            char d[16];
            snprintf(d, sizeof(d), "%d", number);
            append(tens, sizeof(tens), d);
            append(ones, sizeof(ones), " ");
        } else {
            put_digits(tens, ones, sizeof(tens), number);
        }
        number--;
    }
    if (!narrow) {
        stream_printf(fp, "%*c", w, ' ');
        stream_printf(fp, "/%*s  |\n", step * side - 2, tens);
        stream_printf(fp, "%*c", w - 1, ' ');
        stream_printf(fp, "|%*s  |\n", step * side, ones);
    } else {
        stream_printf(fp, "%*c", w - 1, ' ');
        stream_printf(fp, "/%*s  |\n", step * side - 2, tens);
        stream_printf(fp, "%*c", w - 2, ' ');
        stream_printf(fp, "|%*s  |\n", step * side + 1, ones);
    }
    fflush(fp);
    for (int j = 0; j < side; j++, left++, right--) {
        snprintf(label, sizeof(label), "%s", pin_label((unsigned)left));
        stream_printf(fp, "%*s", narrow ? w - (2 * step + 1) : w - (step + 1), label);
        if (half == j) {
            stream_printf(fp, " x-|%-2d", left);
            int pad = ((step * (side + 1) - (int)strlen(g.design_name)) + 1) / 2 - step;
            stream_printf(fp, "%*c", pad, ' ');
            stream_printf(fp, "%s", g.design_name);
            stream_printf(fp, "%*c ", pad, ' ');
            stream_printf(fp, "%2d|-x ", right);
            stream_printf(fp, "%s\n", pin_label((unsigned)right));
        } else {
            stream_printf(fp, " x-|%-2d", left);
            stream_printf(fp, "%*c", narrow ? step * side - 1 : step * side - 2, ' ');
            stream_printf(fp, "%2d|-x ", right);
            stream_printf(fp, "%-*s\n", 0x14, pin_label((unsigned)right));
        }
    }
    tens[0] = ones[0] = '\0';
    number = left;
    for (int k = 0; k < side; k++) {
        append(tens, sizeof(tens), narrow ? " " : "  ");
        append(ones, sizeof(ones), narrow ? "_" : "__");
        if (number == 0) {
            number = count;
        }
        put_digits(tens, ones, sizeof(tens), number);
        number++;
    }
    if (!narrow) {
        stream_printf(fp, "%*c", w - 1, ' ');
        stream_printf(fp, "|%*s  |\n", step * side - 2, tens);
        stream_printf(fp, "%*c", w - 1, ' ');
        stream_printf(fp, "|%*s__|\n", step * side, ones);
    } else {
        stream_printf(fp, "%*c", w - 2, ' ');
        stream_printf(fp, "|%*s  |\n", step * side + 1, tens);
        stream_printf(fp, "%*c", w - 2, ' ');
        stream_printf(fp, "|_%*s__|\n", step * side, ones);
    }
    stream_printf(fp, "%*c", w, ' ');
    for (int k = 0; k < side; k++) {
        stream_printf(fp, "%s", bar);
    }
    stream_printf(fp, "\n");
    stream_printf(fp, "%*c", w, ' ');
    for (int k = 0; k < side; k++) {
        stream_printf(fp, narrow ? " x" : "  x");
    }
    stream_printf(fp, "\n");
    w = 0x14 - step * half;
    for (int i = 0; i < half; i++, left++, right--) {
        snprintf(line, sizeof(line), "%*s", 0x14, pin_label((unsigned)left));
        for (int k = inner; k > 0; k--) {
            append(line, sizeof(line), bar);
        }
        inner -= 2;
        append(line, sizeof(line), gap);
        append(line, sizeof(line), pin_label((unsigned)right));
        /* the literal text "*c" is written in place of the indentation (w
         * is not used); deliberate, the .doc file layout depends on it */
        stream_printf(fp, "*c");
        stream_printf(fp, "%s\n", line);
        w += step;
        fflush(fp);
    }
    snprintf(line, sizeof(line), "%-*s", 0x14, pin_label((unsigned)left));
    stream_printf(fp, "%*c", 0x27, ' ');
    stream_printf(fp, "%s\n\n", line);
    fflush(fp);
}

/* Chip diagram of the device: DIP or LCC outline with the pin names */
void write_chip_diagram(FILE *fp)
{
    if (attr8(0x0b) == 0) {
        write_dip_diagram(fp);
    } else {
        write_lcc_diagram(fp);
    }
}

/* -------------------------------------------------------------------------
 * Fuse plot / HL file: interpreter for section 8 of the device record
 * ------------------------------------------------------------------------- */
enum {
    PLOT_TEXT = 0,      /* char[9] */
    PLOT_OP = 9,        /* u8 */
    PLOT_GROUP = 10,    /* u8 */
    PLOT_A = 0x0b,      /* u32 */
    PLOT_B = 0x0f,
    PLOT_C = 0x13,
    PLOT_D = 0x17,
    PLOT_REC_SIZE = 0x1b
};

static const uint8_t *plot_rec(int i)
{
    return sec_data(SEC_8) + (ptrdiff_t)i * PLOT_REC_SIZE;
}

static int32_t *plot_reg(uint32_t index)
{
    return &s_plot_reg[index % PLOT_REGS];
}

/* First record with the given opcode / group */
static int find_plot_rec(int field, unsigned value)
{
    int count = (int)sec_count(SEC_8);
    for (int i = 0; i < count; i++) {
        if (plot_rec(i)[field] == value) {
            return i;
        }
    }
    return -1;
}

/* The line feeds that some plot records ask for are not written to the
 * file; each one prints an empty line on the status output instead.  This
 * is deliberate: the generated files depend on it. */
static void misplaced_newlines(uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        trace_printf("%s", "");
    }
}

/* One character per group of 'width' fuses, taken from
 * the record's text; forward or backward through the row */
static void plot_fuses(const uint8_t *rec, int backward)
{
    const char *chars = (const char *)rec;
    unsigned nchars = (unsigned)strlen(chars) & 0xffff;
    unsigned width = rd16(rec + PLOT_C);
    unsigned total = rd16(rec + PLOT_D);
    uint32_t pos = (uint32_t)*plot_reg(rd32(rec + PLOT_A)) + rd32(rec + PLOT_B);
    if (backward) {
        pos = pos + total - width;
    }
    for (unsigned done = 0; done < total && width != 0; done += width) {
        unsigned value = 0;
        for (unsigned j = 0; j < width; j++) {
            value = ((value << 1) + (unsigned)get_fuse(pos + j)) & 0xffff;
        }
        if (nchars <= value) {
            report_error(backward ? 1020 : 1019, "");
            value = 0;
        }
        out_putc((unsigned char)chars[value], s_plot_fp);
        pos = backward ? pos - width : pos + width;
    }
    out_putc(' ', s_plot_fp);
}

/* Run the set-up records of a block; returns the index of the
 * record that starts its line loop, or -1 */
static int plot_setup(int i)
{
    int count = (int)sec_count(SEC_8);
    for (; i >= 0 && i < count + 1; i++) {
        const uint8_t *rec = plot_rec(i);
        switch (rec[PLOT_OP]) {
        case 3:
            s_plot_line_reg = (int)(rd32(rec + PLOT_A) % PLOT_REGS);
            s_plot_reg[s_plot_line_reg] = (int32_t)rd32(rec + PLOT_B);
            *plot_reg(rd32(rec + PLOT_C)) = (int32_t)rd32(rec + PLOT_D);
            return i;
        case 4:
            stream_printf(s_plot_fp, "%s ", (const char *)rec);
            break;
        case 10:
            for (uint32_t k = 0; k < rd32(rec + PLOT_A); k++) {
                stream_printf(s_plot_fp, "\n");
            }
            break;
        case 0x0e:
            *plot_reg(rd32(rec + PLOT_A)) = (int32_t)rd32(rec + PLOT_B);
            break;
        case 0x13:
            for (uint32_t k = rd32(rec + PLOT_A); k < rd32(rec + PLOT_B) && k < PLOT_REGS; k++) {
                s_plot_reg[k] = (int32_t)rd32(rec + PLOT_C);
            }
            break;
        case 0x15:
            write_prom_plot(s_plot_fp);
            break;
        default:
            return -1;
        }
    }
    return -1;
}

/* Write one line of a block */
static void plot_line(int i)
{
    int count = (int)sec_count(SEC_8);
    int running = 1;
    while (running && i >= 0 && i < count + 1) {
        const uint8_t *rec = plot_rec(i);
        int32_t *ra = plot_reg(rd32(rec + PLOT_A));
        int32_t *rc = plot_reg(rd32(rec + PLOT_C));
        uint32_t b = rd32(rec + PLOT_B);
        uint32_t c = rd32(rec + PLOT_C);
        uint32_t d = rd32(rec + PLOT_D);
        switch (rec[PLOT_OP]) {
        case 3:
            break;
        case 4:
            stream_printf(s_plot_fp, "%s ", (const char *)rec);
            break;
        case 5:
            plot_fuses(rec, 0);
            break;
        case 6:
            plot_fuses(rec, 1);
            break;
        case 7:
            stream_printf(s_plot_fp, "%s%05lu ", (const char *)rec, (unsigned long)(uint32_t)*ra);
            break;
        case 8:
            running = 0;
            *ra = (int32_t)((uint32_t)*ra - b);
            *rc = (int32_t)((uint32_t)*rc + d);
            break;
        case 10:
            for (uint32_t k = 0; k < rd32(rec + PLOT_A); k++) {
                stream_printf(s_plot_fp, "\n");
            }
            break;
        case 0x0c:
            *ra = (int32_t)((uint32_t)*ra + b);
            *rc = (int32_t)((uint32_t)*rc + d);
            break;
        case 0x0d:
            *ra = (int32_t)((uint32_t)*ra - b);
            *rc = (int32_t)((uint32_t)*rc - d);
            break;
        case 0x0e:
            *ra = (int32_t)b;
            break;
        case 0x0f:
            stream_printf(s_plot_fp, "%s%lu ", (const char *)rec, (unsigned long)(uint32_t)*ra);
            misplaced_newlines(c);
            break;
        case 0x10:
            stream_printf(s_plot_fp, "%s%lu ", (const char *)rec, (unsigned long)(uint32_t)*ra);
            *ra = (int32_t)((uint32_t)*ra + b);
            misplaced_newlines(c);
            break;
        case 0x11:
            stream_printf(s_plot_fp, "%s%lu ", (const char *)rec, (unsigned long)(uint32_t)*ra);
            *ra = (int32_t)((uint32_t)*ra - b);
            misplaced_newlines(c);
            break;
        case 0x12:
            if ((uint32_t)*ra == b) {
                *ra = (int32_t)(c - 1);
            } else {
                *ra = (int32_t)((uint32_t)*ra - 1);
                i += (int32_t)d;
            }
            break;
        case 0x14:
            if ((uint32_t)*ra != b) {
                i += (int32_t)d;
            }
            break;
        case 0x16:
            stream_printf(s_plot_fp, "%s%02lu ", (const char *)rec, (unsigned long)(uint32_t)*ra);
            *ra = (int32_t)((uint32_t)*ra + b);
            misplaced_newlines(c);
            break;
        default:
            report_error(1018, "");
            break;
        }
        i++;
        fflush(s_plot_fp);
    }
}

/* Legend of the fuse plot symbols, by device type */
static void write_plot_legend(void)
{
    FILE *fp = s_plot_fp;
    switch (device_type()) {
    case 0:
    case 1:
    case 2:
    case 7:
        stream_printf(fp, "\n\n%s\n", "LEGEND    X : fuse not blown ");
        stream_printf(fp, "%s\n", "          - : fuse blown ");
        break;
    case 3:
    case 5:
    case 6:
        stream_printf(fp, "\n\n%s\n", "LEGEND    single fuse");
        stream_printf(fp, "%s\n", "          L : active level low        H : active level high");
        stream_printf(fp, "%s\n\n", "          A : active fuse not blown   . : active fuse blown");
        stream_printf(fp, "%s\n", "          double fuse");
        stream_printf(fp, "%s\n", "          0 : neither fuse blown      - : both fuses blown");
        stream_printf(fp, "%s\n", "          L : first fuse blown        H : second fuse blown");
        stream_printf(fp, "%s\n", "          A : generate                . : propagate");
        break;
    default:
        break;
    }
    fflush(fp);
}

/* HL file, named after the design unless the 'n' option is given */
static void open_plot_file(const char *ext)
{
    if (!(g.options & OPT_N)) {
        if (strcmp(g.design_name, "no_name") == 0) {
            report_warning(ERR_NO_NAME, "hl");
        }
        s_plot_name = make_path(source_dir(g.design_name), g.design_name, ext);
    } else {
        s_plot_name = make_path(source_dir(g.args[5]), g.args[5], ext);
    }
    s_plot_fp = open_file(s_plot_name, "wt");
    if (s_plot_fp == NULL) {
        fatal_error(ERR_OPEN, s_plot_name);
    }
}

/* Kind 9 is the fuse plot in the .doc file, kind 1 the HL file */
static void run_plot(int kind, const char *ext)
{
    int start = find_plot_rec(PLOT_OP, (unsigned)kind);
    if (start < 0) {
        report_error(1017, "no data on special format ");
        return;
    }
    s_plot_fp = NULL;
    if (kind == 9) {
        if (g.doc_fp == NULL) {
            doc_open();
            write_title(g.doc_fp, g.design_name, 0x4f, '*');
            stream_printf(g.doc_fp, "\n");
            print_banner(g.doc_fp);
        } else {
            stream_printf(g.doc_fp, "\f");
        }
        s_plot_fp = g.doc_fp;
        write_title(s_plot_fp, "Fuse Plot", 0x4f, '=');
        stream_printf(s_plot_fp, "\n");
        fflush(s_plot_fp);
    } else {
        open_plot_file(ext);
    }
    if (kind == 1) {
        out_putc(2, s_plot_fp);
    }
    check_write(s_plot_fp, s_plot_name != NULL ? s_plot_name : "");
    int i = plot_setup(start + 1);
    if (i < 0) {
        return;     /* the block has no line loop */
    }
    s_plot_group = plot_rec(i)[PLOT_GROUP];
    for (;;) {
        while (s_plot_reg[s_plot_line_reg] != 0) {
            plot_line(i);
            s_plot_reg[s_plot_line_reg]--;
        }
        s_plot_group++;
        i = find_plot_rec(PLOT_GROUP, s_plot_group);
        if (i < 0) {
            break;
        }
        i = plot_setup(i);
        if (i < 0) {
            break;
        }
    }
    if (kind == 1) {
        out_putc(3, s_plot_fp);
    } else if (kind == 9) {
        write_plot_legend();
    }
}

/* Close the HL file or the .doc file the fuse plot was written to */
void close_plot_file(void)
{
    if (s_plot_fp != NULL) {
        check_write(s_plot_fp, s_plot_name != NULL ? s_plot_name : "");
        close_file(s_plot_fp);
        if (g.doc_fp == s_plot_fp) {
            g.doc_fp = NULL;    /* the .doc file was closed with the plot */
        }
        s_plot_fp = NULL;
    }
}

/* Fuse plot in the .doc file followed by the chip diagram (kind 9), or the
 * HL file (kind 1).  Skipped when args[7] is "1". */
void write_fuse_plot(const char *ext, int kind)
{
    int virtual_device = g.args[7][0] == '1';
    if ((device_type() != DEVT_PROM || !g.map_built) && !virtual_device) {
        build_fuse_map();
    }
    if (!virtual_device) {
        run_plot(kind, ext);
    }
    if (kind == 9 && s_plot_fp != NULL) {
        write_chip_diagram(s_plot_fp);
    }
    close_plot_file();
}
