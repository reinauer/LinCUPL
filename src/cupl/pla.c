/*
 * pla.c - Berkeley PLA files.
 *
 * Two writers: the plain <design>.pla of the 'b' option for devices without
 * a fitter, and the annotated PLA that is the input of the device
 * fitters, with "#$" property lines, pin lists and extension
 * names in the fitter's vocabulary.  PROPERTY and DECLARE statements of the
 * source arrive through <design>.oed and are passed on.
 */
#include "ascii.h"
#include "banner.h"
#include "fusemap.h"
#include "strutil.h"

#define NAME_BUF 256

/* class letter of a symbol type */
static const char k_type_class[] = "UFVI X01DNMT  GB";

/* Bits of the format letters in the device attributes */
enum {
    PF_NO_PIN0 = 0x01,      /* 'p': pin number 0 is not printed */
    PF_NO_TYPE = 0x02,      /* 'F': no .type line */
    PF_COPY_TT2 = 0x04,     /* '2': the file is also copied to <design>.tt2 */
    PF_SHORT_LABEL = 0x08,  /* 's' */
    PF_NO_PLUS = 0x10,      /* 'P': active-high signals carry no '+' */
    PF_TOOL = 0x20,         /* 'T': "TOOL CUPL" line */
    PF_NO_DEVICE = 0x40     /* 'D': no DEVICE line */
};

enum { USE_INPUT = 1, USE_OUTPUT = 2, USE_FALSE = 4, USE_TRUE = 8 };

typedef struct pla_row {
    struct pla_row *prev;
    struct pla_row *next;
    char text[];            /* inputs then outputs; '*' first = merged away */
} pla_row_t;

static pla_row_t *s_rows_head;
static pla_row_t *s_rows_tail;
static int s_ninputs;
static int s_noutputs;
static int s_nterms;
static uint8_t *s_usage;            /* USE_xxx per symbol */
static uint8_t *s_usage_inv;        /* second byte: symbol is inverted */
static int s_nsyms;
static char *s_phase;               /* text of the .phase line */
static int s_no_o;                  /* format letter 'o' absent */
static const char *s_prefix;        /* keywords of the "#$" lines, see select_keywords() */
static const char *s_group_kw;
static const char *s_jedec_kw;
static const char *s_pins_kw;
static const char *s_nodes_kw;
static int s_device_given;          /* the .oed file named the device */

/* declare.def: property classes and their items */
typedef struct decl_item {
    char name[0x18];
    int kind;
    int count;
    struct decl_item *next;
} decl_item_t;
#define DECL_MAX 20
static struct {
    char name[0x18];
    decl_item_t *items;
} s_decls[DECL_MAX];
static int s_ndecls;

/* .oed scanner */
enum {
    OT_IDENT = 0x101,
    OT_NUMBER = 0x102,
    OT_XILINX = 0x109,
    OT_DECLARE = 0x10a,
    OT_PROPERTY = 0x10e,
    OT_CYPRESS = 0x10f,
    OT_PHILIPS = 0x110
};
static int s_tok;
static char s_tok_text[80];
static char s_prev_text[80];

/* keywords of the .oed file and their token values */
static const struct {
    const char *name;
    int token;
} k_oed_keywords[] = {
    { "end", 0x106 }, { "Instance", 0x103 }, { "InternalNet", 0x104 }, { "ExternalNet", 0x105 },
    { "InputNet", 0x107 }, { "INSERT", 0x108 }, { "XILINX", OT_XILINX }, { "DECLARE", OT_DECLARE },
    { "BLOCK_REP", 0x10b }, { "INIT_TO", 0x10c }, { "PARAMETER", 0x10d }, { "S_I", 0x3e8 }, { "S_O", 0x7d0 },
    { "S_NODE", 0xfa0 }, { "PROPERTY", OT_PROPERTY }, { "YES", 1 }, { "NO", 0 }, { "S_NON", 0 }, { "S_COM", 1 },
    { "S_DFF", 2 }, { "S_DQF", 5 }, { "S_IO", 0x0e }, { "S_DQ", 8 }, { "S_INT", 0xa01 }, { "CYPRESS", OT_CYPRESS },
    { "PHILIPS", OT_PHILIPS }, { NULL, 0 }
};

void pla_reset(void)
{
    s_rows_head = s_rows_tail = NULL;
    s_ninputs = s_noutputs = s_nterms = s_nsyms = 0;
    s_usage = s_usage_inv = NULL;
    s_phase = NULL;
    s_device_given = 0;
    s_ndecls = 0;
    memset(s_decls, 0, sizeof(s_decls));
    s_tok = 0;
    s_tok_text[0] = s_prev_text[0] = '\0';
}

static int is_word_char(int c)
{
    return is_alnum(c) || c == '&' || c == '_';
}

/* A list is written to the PLA unless it belongs to a field, a function,
 * an intermediate variable or (fitter PLA only) a group. */
static int list_is_written(const symbol_t *s, int skip_groups)
{
    unsigned type = s->type & 0x0f;
    return s->eq != NULL && s->name[0] != '.' && type != 1 && type != 0x0b && type != 3
           && !(skip_groups && type == 0x0e);
}

/* =========================================================================
 * Plain PLA
 * ========================================================================= */

/* Mark the symbols a list uses as inputs (1) and outputs (2) */
static void mark_plain_usage(const equation_t *eq, uint8_t *usage)
{
    for (int t = 0; t < eq->nterms; t++) {
        if (TERM_CARE(eq, t)[0] & TERM_DELETED) {
            continue;
        }
        for (int i = 0; i < eq->nvars; i++) {
            unsigned type = g.syms[i].type & 0x0f;
            if (type == 2) {
                usage[i] |= 1;
            } else if ((type >= 5 && type <= 7) || type == 9 || type == 10 || type == 0x0c || type == 0x0d) {
                if (term_bit(TERM_CARE(eq, t), i)) {
                    usage[i] |= 1;
                }
            }
        }
        for (int i = 0; i < eq->nvars; i++) {
            unsigned type = g.syms[i].type & 0x0f;
            if ((type == 2 || type == 5 || (type >= 8 && type <= 10)) && term_bit(TERM_OUT(eq, t), i)) {
                usage[i] |= 2;
            }
        }
    }
}

/* One row per product term of a list: inputs '0', '1', '-', outputs '1', '~' */
static void write_plain_rows(const equation_t *eq, const uint8_t *usage, FILE *fp)
{
    for (int t = 0; t < eq->nterms; t++) {
        if (TERM_CARE(eq, t)[0] & TERM_DELETED) {
            continue;
        }
        for (int i = 0; i < eq->nvars; i++) {
            if (usage[i] == 1 || usage[i] == 3) {
                if (!term_bit(TERM_CARE(eq, t), i)) {
                    out_putc('-', fp);
                } else {
                    out_putc(term_bit(TERM_VAL(eq, t), i) ? '1' : '0', fp);
                }
            }
        }
        out_putc(' ', fp);
        for (int i = 0; i < eq->nvars; i++) {
            if (usage[i] == 2 || usage[i] == 3) {
                out_putc(term_bit(TERM_OUT(eq, t), i) ? '1' : '~', fp);
            }
        }
        out_putc('\n', fp);
    }
}

/* Berkeley PLA file of the 'b' option */
void write_pla(void)
{
    char *name = make_path(source_dir(g.args[5]), g.args[5], ".pla");
    FILE *fp = open_file(name, "wt");
    if (fp == NULL) {
        fatal_error(ERR_OPEN, name);
    }
    out_printf(fp, "# Berkeley PLA format generated using\n#\n");
    print_banner_with(fp, "# ");
    out_printf(fp, "#\n");
    uint8_t *usage = alloc_zeroed((size_t)g.nsyms + 1, 1, "");
    int nterms = 0;
    for (int i = 0; i < g.nsyms; i++) {
        if (list_is_written(&g.syms[i], 0)) {
            mark_plain_usage(g.syms[i].eq, usage);
            nterms += g.syms[i].eq->nterms;
        }
    }
    int ninputs = 0;
    for (int i = 0; i < g.nsyms; i++) {
        const symbol_t *s = &g.syms[i];
        if (usage[i] != 1 && usage[i] != 3) {
            continue;
        }
        if (ninputs == 0) {
            out_printf(fp, "# Inputs  ");
        } else if (ninputs % 4 == 0) {
            out_printf(fp, "\n#         ");
        }
        if (i == 0) {
            out_putc('0', fp);
        } else if (i == 1) {
            out_putc('1', fp);
        } else {
            if (s->flags & SYMF_INVERTED) {
                out_printf(fp, "!");
            }
            out_printf(fp, "%s", s->name);
            if (SYM_EXT(s)[0] != '\0') {
                out_printf(fp, ".%s", SYM_EXT(s));
            }
        }
        out_putc(' ', fp);
        ninputs++;
    }
    out_putc('\n', fp);
    int noutputs = 0;
    for (int i = 0; i < g.nsyms; i++) {
        const symbol_t *s = &g.syms[i];
        if (usage[i] != 2 && usage[i] != 3) {
            continue;
        }
        if (noutputs == 0) {
            out_printf(fp, "# Outputs ");
        } else if (noutputs % 4 == 0) {
            out_printf(fp, "\n#         ");
        }
        unsigned kind = node8(s->node_index, NODE_KIND);
        if (kind == 1 || kind == 5 || kind == 6) {
            out_printf(fp, "!");
        }
        out_printf(fp, "%s", s->name);
        if (SYM_EXT(s)[0] != '\0') {
            out_printf(fp, ".%s", SYM_EXT(s));
        }
        noutputs++;
        out_putc(' ', fp);
    }
    out_putc('\n', fp);
    out_printf(fp, ".i %d\n", ninputs);
    out_printf(fp, ".o %d\n", noutputs);
    out_printf(fp, ".p %d\n", nterms);
    for (int i = 0; i < g.nsyms; i++) {
        if (list_is_written(&g.syms[i], 0)) {
            write_plain_rows(g.syms[i].eq, usage, fp);
        }
    }
    out_printf(fp, ".end\n");
    close_file(fp);
    free_tracked(usage);
}

/* =========================================================================
 * <design>.oed: PROPERTY and DECLARE statements
 * ========================================================================= */

/* Next token of the .oed file: a character, OT_NUMBER, a keyword token or
 * OT_IDENT; -1 at the end.  The previous token text is kept in s_prev_text. */
static int oed_next(FILE *fp)
{
    int c;
    snprintf(s_prev_text, sizeof(s_prev_text), "%s", s_tok_text);
    for (;;) {
        c = read_char(fp);
        if (c == EOF || c == 0x1a) {
            return -1;
        }
        if (c == ' ' || c == '\t' || c == '\n') {
            continue;
        }
        break;
    }
    if (!is_word_char(c)) {
        s_tok_text[0] = (char)c;
        s_tok_text[1] = '\0';
        return c;
    }
    size_t n = 0;
    while (is_word_char(c)) {
        if (n + 1 < sizeof(s_tok_text)) {
            s_tok_text[n++] = (char)c;
        }
        c = read_char(fp);
    }
    s_tok_text[n] = '\0';
    ungetc(c, fp);
    int digits = 1;
    for (const char *p = s_tok_text; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            digits = 0;
            break;
        }
    }
    if (digits) {
        return OT_NUMBER;
    }
    for (int i = 0; k_oed_keywords[i].name != NULL; i++) {
        if (strcasecmp(k_oed_keywords[i].name, s_tok_text) == 0) {
            return k_oed_keywords[i].token;
        }
    }
    return OT_IDENT;
}

/* Report a syntax error unless the current token is 'token'; advance */
static void oed_expect(int token, FILE *fp)
{
    if (s_tok != token) {
        report_error(ERR_OED_FILE, "syntax error");
        trace_printf("(%s)(%s)", s_prev_text, s_tok_text);
    }
    s_tok = oed_next(fp);
}

/* Token of declare.def; '#' starts a comment */
static int def_next(FILE *fp, char *text, size_t cap)
{
    int c;
    for (;;) {
        c = read_char(fp);
        if (c == EOF || c == 0x1a) {
            return -1;
        }
        if (c == ' ' || c == '\t' || c == '\n') {
            continue;
        }
        if (c != '#') {
            break;
        }
        do {
            c = read_char(fp);
        } while (c != '\n' && c != EOF);
        if (c == EOF) {
            return -1;
        }
    }
    if (!is_word_char(c)) {
        text[0] = (char)c;
        text[1] = '\0';
        return c;
    }
    size_t n = 0;
    while (is_word_char(c)) {
        if (n + 1 < cap) {
            text[n++] = (char)c;
        }
        c = read_char(fp);
    }
    text[n] = '\0';
    ungetc(c, fp);
    return 0x100;
}

/* Load declare.def from the work directory */
static int load_declarations(void)
{
    char text[80];
    char *name = make_path(g.args[1], "declare", ".def");
    FILE *fp = open_file(name, "r");
    if (fp == NULL) {
        fatal_error(ERR_OPEN, name);
    }
    while (s_ndecls < DECL_MAX) {
        if (def_next(fp, text, sizeof(text)) == -1) {
            break;
        }
        snprintf(s_decls[s_ndecls].name, sizeof(s_decls[0].name), "%s", text);
        def_next(fp, text, sizeof(text));
        s_decls[s_ndecls].items = NULL;
        decl_item_t **link = &s_decls[s_ndecls].items;
        int tok;
        do {
            def_next(fp, text, sizeof(text));
            decl_item_t *item = alloc_zeroed(1, sizeof(*item), "");
            snprintf(item->name, sizeof(item->name), "%s", text);
            def_next(fp, text, sizeof(text));
            item->kind = (int)strtol(text, NULL, 10);
            if (item->kind == 3) {
                def_next(fp, text, sizeof(text));
                item->count = (int)strtol(text, NULL, 10);
            }
            *link = item;
            link = &item->next;
            tok = def_next(fp, text, sizeof(text));
        } while (tok != ';' && tok != -1);
        s_ndecls++;
    }
    close_file(fp);
    return 0;
}

/* Kind of a declared property, -1 if unknown */
static int lookup_declaration(const char *class_name, const char *item_name, int *count)
{
    if (s_ndecls != 0 || load_declarations() == 0) {
        for (int i = 0; i < s_ndecls; i++) {
            if (strcmp(s_decls[i].name, class_name) != 0) {
                continue;
            }
            for (decl_item_t *it = s_decls[i].items; it != NULL; it = it->next) {
                if (strcmp(it->name, item_name) == 0) {
                    *count = it->count;
                    return it->kind;
                }
            }
        }
    }
    return -1;
}

/* Common start of the two DECLARE forms: ": [ a b c ]" -> "a b c " */
static int oed_read_list(FILE *oed, char *list, size_t cap)
{
    oed_expect(':', oed);
    oed_expect('[', oed);
    oed_expect(OT_IDENT, oed);
    snprintf(list, cap, "%s ", s_prev_text);
    int count = 1;
    while (s_tok != ']' && s_tok != -1) {
        oed_expect(OT_IDENT, oed);
        size_t used = strlen(list);
        snprintf(list + used, cap - used, "%s ", s_prev_text);
        count++;
    }
    return count;
}

/* DECLARE with a list of names */
static void oed_declare_list(int with_count, const char *a, const char *b, FILE *fp, FILE *oed)
{
    char list[1024];
    stream_printf(fp, "#$ PROPERTY %s %s ", a, b);
    int count = oed_read_list(oed, list, sizeof(list));
    if (with_count) {
        stream_printf(fp, "%d ", count);
    }
    stream_printf(fp, "%s\n", list);
    oed_expect(']', oed);
    oed_expect(';', oed);
}

/* DECLARE with a list of names followed by values */
static void oed_declare_values(const char *a, const char *b, int expected, FILE *fp, FILE *oed)
{
    char list[1024];
    stream_printf(fp, "#$ PROPERTY %s %s ", a, b);
    int count = oed_read_list(oed, list, sizeof(list));
    if (expected != count) {
        report_warning(ERR_DECLARE_COUNT, b);
    }
    stream_printf(fp, "[%s]", list);
    oed_expect(']', oed);
    int radix = 'd';
    unsigned value = 0;
    /* also ends at the end of the file */
    while (s_tok != -1) {
        if (s_tok == ';') {
            oed_expect(';', oed);
            stream_printf(fp, "\n");
            return;
        }
        if (s_tok == '\'') {
            oed_expect('\'', oed);
            oed_expect(s_tok, oed);
            radix = (signed char)s_prev_text[0];
            oed_expect('\'', oed);
        } else if (s_tok != OT_NUMBER) {
            oed_expect(s_tok, oed);
            stream_printf(fp, "%s", s_prev_text);
        } else {
            oed_expect(OT_NUMBER, oed);
            if (radix == 'd' || radix == 'D') {
                int v;
                if (sscanf(s_prev_text, "%d", &v) == 1) {
                    value = (unsigned)v;
                }
            } else if (radix == 'h' || radix == 'H') {
                sscanf(s_prev_text, "%x", &value);
            } else if (radix == 'o' || radix == 'O') {
                sscanf(s_prev_text, "%o", &value);
            }
            stream_printf(fp, "%x", value);
        }
    }
}

/* Copy the PROPERTY and DECLARE statements into the PLA header */
static void copy_oed_properties(FILE *oed, FILE *fp)
{
    s_tok = oed_next(oed);
    while (s_tok != -1) {
        if (s_tok == OT_PROPERTY) {
            oed_expect(OT_PROPERTY, oed);
            if (s_tok == OT_CYPRESS) {
                oed_expect(OT_CYPRESS, oed);
                stream_printf(fp, "#$ %s ", s_prev_text);
                stream_printf(fp, "PROPERTY ");
            } else if (s_tok == OT_PHILIPS) {
                oed_expect(OT_PHILIPS, oed);
                stream_printf(fp, "#$ DEVICE ");
                s_device_given = 1;
            } else {
                stream_printf(fp, "#$ PROPERTY ");
                oed_expect(s_tok == OT_XILINX ? OT_XILINX : OT_IDENT, oed);
                stream_printf(fp, "%s ", s_prev_text);
            }
            int last = s_tok;
            oed_expect(s_tok, oed);
            stream_printf(fp, "%s", s_prev_text);
            if (last == OT_IDENT && !s_device_given) {
                stream_printf(fp, " ");
            }
            while (s_tok != -1 && s_tok != OT_PROPERTY && s_tok != OT_DECLARE) {
                last = s_tok;
                oed_expect(s_tok, oed);
                stream_printf(fp, "%s", s_prev_text);
                if (last == ',' || (last != '.' && last != '-' && last != ':'
                                    && (s_tok == OT_IDENT || s_tok == OT_NUMBER))) {
                    stream_printf(fp, " ");
                }
            }
            stream_printf(fp, "\n");
        } else if (s_tok == OT_DECLARE) {
            char a[80], b[80];
            int count = 0;
            oed_expect(OT_DECLARE, oed);
            oed_expect(s_tok, oed);
            snprintf(a, sizeof(a), "%s", s_prev_text);
            oed_expect(s_tok, oed);
            snprintf(b, sizeof(b), "%s", s_prev_text);
            switch (lookup_declaration(a, b, &count)) {
            case 1:
                oed_declare_list(1, a, b, fp, oed);
                break;
            case 3:
                oed_declare_values(a, b, count, fp, oed);
                break;
            case 5:
                oed_declare_list(0, a, b, fp, oed);
                break;
            default:
                while (s_tok != -1 && s_tok != ';') {
                    oed_expect(s_tok, oed);
                }
                break;
            }
        } else {
            oed_expect(s_tok, oed);
        }
    }
}

/* Open <design>.oed next to the source file, or return NULL */
static FILE *open_oed(void)
{
    char dir[PATH_MAX];
    char base[PATH_MAX];
    char path[2 * PATH_MAX + 8];
    lincupl_split_path(g.args[5], dir, sizeof(dir), base, sizeof(base), NULL, 0);
    snprintf(path, sizeof(path), "%s%s.oed", dir, base);
    return open_file(path, "rt");
}

/* =========================================================================
 * Fitter PLA
 * ========================================================================= */

/* Keywords used in the "#$" lines */
static void select_keywords(const char *letters)
{
    s_no_o = strchr(letters, 'o') == NULL;
    if (strchr(letters, 'A') == NULL) {
        s_prefix = "#$";
        s_jedec_kw = "JEDECFILE";
        s_pins_kw = "PINS";
        s_nodes_kw = "NODES";
        s_group_kw = strchr(letters, 'G') == NULL ? "GROUP" : NULL;
    } else {
        s_prefix = "#AMD";
        s_jedec_kw = "JED_FILE";
        s_pins_kw = "PIN";
        s_nodes_kw = "NODE";
        s_group_kw = "GROUP";
    }
    s_jedec_kw = strchr(letters, 'J') == NULL ? NULL : "JEDECFILE";
}

/* Copy without trailing blanks */
static void copy_trimmed(const char *src, char *dst, size_t cap)
{
    snprintf(dst, cap, "%s", src);
    size_t len = strlen(dst);
    while (len > 0 && dst[len - 1] == ' ') {
        len--;
    }
    dst[len] = '\0';
}

/* The fitter's name for an extension (section 5 of the device) */
static const char *extension_alias(const char *ext)
{
    unsigned count = sec_count(SEC_5);
    const char *rec = (const char *)sec_data(SEC_5);
    for (unsigned i = 0; i < count; i++, rec += 0x28) {
        if (strcasecmp(rec, ext) == 0) {
            return rec + 0x14;
        }
    }
    report_warning(ERR_UNKNOWN_EXTENSION, ext);
    return ext;
}

/* Comment header, TITLE lines, the .oed properties and the MODULE, JEDEC
 * file, DEVICE and COMMAND lines of the fitter PLA */
static void write_fitter_header(FILE *fp, int format, unsigned pf)
{
    char text[NAME_BUF];
    const char *part = (const char *)attr_ptr(ATTR_PART_NAME);
    if (pf & PF_TOOL) {
        stream_printf(fp, "%s %s CUPL\n", s_prefix, "TOOL");
    }
    stream_printf(fp, "# Berkeley PLA format generated using %s %s\n", PROGRAM_NAME, PROGRAM_VERSION);
    stream_printf(fp, "# %-9s%s", "Created", creation_time_text());
    stream_printf(fp, "#\n");
    static const char *const labels[8] = {
        "Name", "Partno", "Revision", "Date", "Designer", "Company", "Assembly", "Location"
    };
    for (int i = 0; i < 8; i++) {
        copy_trimmed(header_string(i), text, sizeof(text));
        stream_printf(fp, "#    %-12s%s\n", labels[i], text);
    }
    fflush(fp);
    stream_printf(fp, "#\n");
    int titles = format == 0x0d ? 2 : 1;
    for (int i = 0; i < titles; i++) {
        copy_trimmed(header_string(i), text, sizeof(text));
        stream_printf(fp, "%s TITLE  %s\n", s_prefix, text);
    }
    FILE *oed = open_oed();
    if (oed != NULL) {
        copy_oed_properties(oed, fp);
        close_file(oed);
    }
    /* module name: at most eight characters of the design name, cut at the
     * last blank or else at the last dot */
    char module[12];
    memset(module, 0, sizeof(module));
    strncpy(module, header_string(0), 8);
    char *cut = strrchr(module, ' ');
    if (cut == NULL) {
        cut = strrchr(module, '.');
    }
    if (cut != NULL) {
        *cut = '\0';
    }
    stream_printf(fp, "%s MODULE  %s\n", s_prefix, module);
    if (s_jedec_kw != NULL) {
        stream_printf(fp, "%s %s  %s\n", s_prefix, s_jedec_kw, module);
    }
    if (!(pf & PF_NO_DEVICE)) {
        if (part[0] == '\0') {
            if (!s_device_given) {
                if (strcasecmp((const char *)g.lib.dir_entry, "c374t") == 0) {
                    stream_printf(fp, "%s DEVICE C374\n", s_prefix);
                    stream_printf(fp, "%s PACKAGE CYC374TQFP\n", s_prefix);
                } else if (strcasecmp((const char *)g.lib.dir_entry, "c373t") == 0) {
                    stream_printf(fp, "%s DEVICE C373\n", s_prefix);
                    stream_printf(fp, "%s PACKAGE CYC373TQFP\n", s_prefix);
                } else {
                    stream_printf(fp, "%s DEVICE  %s\n", s_prefix, (const char *)g.lib.dir_entry);
                }
            }
        } else if (strncmp(part, " c374t", 6) == 0) {
            stream_printf(fp, "%s DEVICE C374\n", s_prefix);
            stream_printf(fp, "%s PACKAGE CYC374TQFP\n", s_prefix);
        } else if (strncmp(part, " c373t", 6) == 0) {
            stream_printf(fp, "%s DEVICE C373\n", s_prefix);
            stream_printf(fp, "%s PACKAGE CYC373TQFP\n", s_prefix);
        } else {
            stream_printf(fp, "%s DEVICE  %s\n", s_prefix, part);
        }
    }
    if (strlen(g.format_text) > 1) {
        copy_trimmed(g.format_text, text, sizeof(text));
        if (format == 0x0d) {
            stream_printf(fp, "%s COMMAND 1 _%s\n", s_prefix, text);
        } else if (format == 0x0a) {
            stream_printf(fp, "%s COMMAND %s\n", s_prefix, text);
        }
    }
    fflush(fp);
}

/* Which symbols appear as inputs and outputs of the PLA */
static void mark_usage(const equation_t *eq)
{
    for (int t = 0; t < eq->nterms; t++) {
        if (TERM_CARE(eq, t)[0] & TERM_DELETED) {
            continue;
        }
        for (int i = 0; i < eq->nvars; i++) {
            if (term_bit(TERM_CARE(eq, t), i)) {
                switch (g.syms[i].type & 0x0f) {
                case 2:
                case 5:
                case 0x0c:
                case 0x0d:
                case 9:
                case 10:
                    s_usage[i] |= USE_INPUT;
                    break;
                case 6:
                    s_usage[i] |= USE_FALSE;
                    break;
                case 7:
                    s_usage[i] |= USE_TRUE;
                    break;
                default:
                    break;
                }
            }
            s_usage_inv[i] = g.syms[i].flags & SYMF_INVERTED;
        }
        for (int i = 0; i < eq->nvars; i++) {
            if (term_bit(TERM_OUT(eq, t), i)) {
                switch (g.syms[i].type & 0x0f) {
                case 2:
                case 5:
                case 8:
                case 10:
                case 9:
                    s_usage[i] |= USE_OUTPUT;
                    break;
                default:
                    break;
                }
            }
            s_usage_inv[i] = g.syms[i].flags & SYMF_INVERTED;
        }
    }
}

/* Number of XOR splits in a list (terms that end a half) */
static int split_count(const equation_t *eq)
{
    int count = 0;
    if (eq != NULL) {
        for (int t = 0; t < eq->nterms; t++) {
            unsigned flags = TERM_CARE(eq, t)[0];
            if (!(flags & TERM_DELETED) && (flags & TERM_SPLIT)) {
                count++;
            }
        }
    }
    return count;
}

/* Extension of a register input */
static int is_register_ext(const char *ext)
{
    static const char *const k_exts[] = { "D", "L", "T", "J", "K", "R", "S", NULL };
    for (int i = 0; k_exts[i] != NULL; i++) {
        if (strcasecmp(k_exts[i], ext) == 0) {
            return 1;
        }
    }
    return 0;
}

/* The signal has a registered output */
static int has_register_output(const char *name)
{
    for (int i = 0; i < s_nsyms; i++) {
        if ((s_usage[i] & USE_OUTPUT) && strcmp(g.syms[i].name, name) == 0 && is_register_ext(SYM_EXT(&g.syms[i]))) {
            return 1;
        }
    }
    return 0;
}

/* The signal has a toggle (.t) output */
static int has_toggle_output(const char *name)
{
    for (int i = 0; i < s_nsyms; i++) {
        if ((s_usage[i] & USE_OUTPUT) && strcmp(g.syms[i].name, name) == 0
            && strcasecmp(SYM_EXT(&g.syms[i]), "t") == 0) {
            return 1;
        }
    }
    return 0;
}

/* Input of the given name with one of two extensions: 1 for the first,
 * 2 for the second, else 0 */
static int input_with_ext(const char *name, const char *ext1, const char *ext2)
{
    for (int i = 0; i < s_nsyms; i++) {
        if ((s_usage[i] & USE_INPUT) && strcmp(g.syms[i].name, name) == 0) {
            if (strcasecmp(SYM_EXT(&g.syms[i]), ext1) == 0) {
                return 1;
            }
            if (strcasecmp(SYM_EXT(&g.syms[i]), ext2) == 0) {
                return 2;
            }
        }
    }
    return 0;
}

static int is_io_control_ext(const char *ext)
{
    return strcasecmp(ext, "IOCK") == 0 || strcasecmp(ext, "IOAR") == 0 || strcasecmp(ext, "IOAP") == 0
           || strcasecmp(ext, "IOSR") == 0 || strcasecmp(ext, "IOSP") == 0;
}

/* 1 if the signal is fed back from a D register, 2 from a latch */
static int feedback_kind(const symbol_t *s)
{
    int q = input_with_ext(s->name, "DQ", "LQ");
    int io = is_io_control_ext(SYM_EXT(s)) ? input_with_ext(s->name, "IOD", "IOL") : 0;
    if (q == 1 || io == 1) {
        return 1;
    }
    if (q == 2 || io == 2) {
        return 2;
    }
    return 0;
}

/* Extension of a control term; when the output's
 * register is fed back (.DQ/.LQ) the I/O variant of the name is used */
static void control_extension(const symbol_t *s, char *out, size_t cap)
{
    static const char *const k_io_names[][2] = {
        { "ar", "ioar" }, { "sr", "iosr" }, { "ap", "ioap" }, { "sp", "iosp" }, { "ck", "iock" },
        { "ckmux", "iockmux" }, { NULL, NULL }
    };
    out[0] = '\0';
    if (input_with_ext(s->name, "DQ", "LQ") == 0) {
        snprintf(out, cap, "%s", SYM_EXT(s));
        return;
    }
    for (int i = 0; k_io_names[i][0] != NULL; i++) {
        if (strcasecmp(k_io_names[i][0], SYM_EXT(s)) == 0) {
            snprintf(out, cap, "%s", k_io_names[i][1]);
            return;
        }
    }
}

/* Name suffix of the two XOR inputs of an output */
static const char *xor_suffix(const char *name)
{
    for (int i = 0; i < s_nsyms; i++) {
        const symbol_t *s = &g.syms[i];
        if ((s_usage[i] & USE_OUTPUT) && strcmp(s->name, name) == 0 && is_register_ext(SYM_EXT(s))) {
            if (strcasecmp(SYM_EXT(s), "D") == 0) {
                return ".XORL_REG";
            }
            if (strcasecmp(SYM_EXT(s), "T") == 0) {
                return ".XORL_T";
            }
            if (strcasecmp(SYM_EXT(s), "L") == 0) {
                return ".XORL_G";
            }
        }
    }
    return ".XORL";
}

/* Output that can carry a polarity mark (the same test as in doc.c) */
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

/* Start a label line, or continue it on a new line */
static int start_label(FILE *fp, const char *label, int col)
{
    if (col == 0) {
        stream_printf(fp, "%-6s", label);
        return 6;
    }
    if (col > 0x3c) {
        stream_printf(fp, "\n%-6s", "");
        fflush(fp);
        return 6;
    }
    return col;
}

static int put_blanks(FILE *fp, int count)
{
    for (int i = 0; i < count; i++) {
        out_putc(' ', fp);
    }
    return count;
}

/* ".<fitter name of 'reg'>" unless that name is empty; returns the column */
static int put_register_suffix(FILE *fp, int col)
{
    const char *alias = extension_alias("reg");
    if (alias[0] != '\0') {
        col += stream_printf(fp, ".%s", alias);
    }
    return col;
}

/* ":<pin>", left out for pin 0 with the 'p' format letter */
static void put_pin_number(FILE *fp, unsigned pf, int pin)
{
    if (!(pf & PF_NO_PIN0) || pin != 0) {
        stream_printf(fp, ":%d", pin);
    }
}

/* "#$ GROUP name member member ..." for a group symbol */
static void write_group(FILE *fp, const equation_t *eq)
{
    char text[512];
    char upper[80];
    int found = 0;
    text[0] = '\0';
    for (int i = 0; i < eq->nvars && !found; i++) {
        if (term_bit(TERM_OUT(eq, 0), i)) {
            snprintf(upper, sizeof(upper), "%s", g.syms[i].name);
            for (char *p = upper; *p != '\0'; p++) {
                if (*p >= 'a' && *p <= 'z') {
                    *p = (char)(*p - 'a' + 'A');
                }
            }
            snprintf(text, sizeof(text), "%s ", upper);
            found = 1;
        }
    }
    for (int t = 0; t < eq->nterms; t++) {
        if (TERM_CARE(eq, t)[0] & TERM_DELETED) {
            continue;
        }
        for (int i = 0; i < eq->nvars; i++) {
            if (!term_bit(TERM_CARE(eq, t), i)) {
                continue;
            }
            const char *name = g.syms[i].name;
            const char *demorg = strstr(name, "$demorg");
            size_t len = demorg != NULL ? (size_t)(demorg - name) : strlen(name);
            size_t used = strlen(text);
            if (used + len + 2 < sizeof(text)) {
                memcpy(text + used, name, len);
                text[used + len] = ' ';
                text[used + len + 1] = '\0';
            }
        }
    }
    if (s_group_kw != NULL) {
        stream_printf(fp, "%s %s %s\n", s_prefix, s_group_kw, text);
        fflush(fp);
    }
}

/* Append an empty row to the row list */
static pla_row_t *new_row(void)
{
    pla_row_t *row = alloc_tracked(sizeof(*row) + (size_t)s_ninputs + (size_t)s_noutputs + 9, "");
    row->prev = s_rows_tail;
    row->next = NULL;
    if (s_rows_head == NULL) {
        s_rows_head = row;
    } else {
        s_rows_tail->next = row;
    }
    s_rows_tail = row;
    return row;
}

/* One PLA row per product term of a list */
static void build_rows(const equation_t *eq, int format)
{
    int half = 0;
    for (int t = 0; t < eq->nterms; t++) {
        const uint16_t *out = TERM_OUT(eq, t);
        const uint16_t *care = TERM_CARE(eq, t);
        const uint16_t *val = TERM_VAL(eq, t);
        if (care[0] & TERM_DELETED) {
            continue;
        }
        int is_false = 0;
        pla_row_t *row = new_row();
        char *p = row->text;
        for (int i = 0; i < eq->nvars; i++) {
            if (s_usage[i] & USE_INPUT) {
                if (!term_bit(care, i)) {
                    *p++ = '-';
                } else if (!s_no_o) {
                    *p++ = term_bit(val, i) ? '1' : '0';
                } else {
                    int reg = has_register_output(g.syms[i].name);
                    int toggle = has_toggle_output(g.syms[i].name);
                    int plain = s_usage_inv[i] == 0 || (reg && (format != 0x0d || toggle));
                    *p++ = (term_bit(val, i) != 0) == plain ? '1' : '0';
                }
            }
            if ((s_usage[i] & USE_FALSE) && term_bit(care, i)) {
                is_false = 1;
            }
        }
        for (int i = 0; i < eq->nvars; i++) {
            int splits = split_count(g.syms[i].eq);
            if (!(s_usage[i] & USE_OUTPUT)) {
                continue;
            }
            int on = term_bit(out, i) && !is_false;
            if (splits < 1) {
                *p++ = on ? '1' : '0';
            } else {
                for (int k = 0; k <= splits; k++) {
                    *p++ = (on && k == half) ? '1' : '0';
                }
            }
        }
        if (care[0] & TERM_SPLIT) {
            half++;
        }
    }
}

/* Rows with the same inputs are merged into the first one */
static void merge_rows(void)
{
    for (pla_row_t *a = s_rows_head; a != NULL && a->next != NULL; a = a->next) {
        for (pla_row_t *b = a->next; b != NULL; b = b->next) {
            if (b->text[0] == '*' || memcmp(a->text, b->text, (size_t)s_ninputs) != 0) {
                continue;
            }
            b->text[0] = '*';
            for (int k = 0; k < s_noutputs; k++) {
                if (b->text[s_ninputs + k] == '1') {
                    a->text[s_ninputs + k] = '1';
                }
            }
            s_nterms--;
        }
    }
}

/* Write the rows that were not merged away */
static void write_rows(FILE *fp)
{
    for (pla_row_t *row = s_rows_head; row != NULL; row = row->next) {
        if (row->text[0] == '*') {
            continue;
        }
        for (int i = 0; i < s_ninputs; i++) {
            out_putc(row->text[i], fp);
        }
        out_putc(' ', fp);
        for (int i = 0; i < s_noutputs; i++) {
            out_putc(row->text[s_ninputs + i], fp);
        }
        out_putc('\n', fp);
    }
    fflush(fp);
}

static void append(char *dst, size_t cap, const char *src)
{
    size_t used = strlen(dst);
    if (used < cap) {
        snprintf(dst + used, cap - used, "%s", src);
    }
}

/* Output name of an XOR pair: "name.XORL... name.XORR..." */
static int put_xor_pair(FILE *fp, char *name, size_t cap)
{
    int col = 0;
    append(name, cap, xor_suffix(name));
    col += stream_printf(fp, "%s", name);
    col += put_blanks(fp, 1);
    char *x = strstr(name, ".XORL");
    if (x != NULL) {
        x[4] = 'R';
    }
    col += stream_printf(fp, "%s", name);
    return col;
}

/* Write the fitter PLA to 'path': header, pin and node lists, .i/.o/.type,
 * labels, .phase, .p, the rows and .e */
static void write_fitter_file(const char *path, int format, unsigned pf)
{
    select_keywords((const char *)attr_ptr(0x70));
    FILE *fp = open_file(path, "wt");
    if (fp == NULL) {
        fatal_error(ERR_OPEN, path);
    }
    write_fitter_header(fp, format, pf);
    if (g.options & OPT_G) {
        stream_printf(fp, "#$ PROPERTY CUPL SECURITY = ON\n");
        fflush(fp);
    }
    int npin = (int)attr8(ATTR_NPINS);
    s_nsyms = g.nsyms;
    s_usage = alloc_zeroed((size_t)s_nsyms + 1, 1, "");
    s_usage_inv = alloc_zeroed((size_t)s_nsyms + 1, 1, "");
    s_nterms = 0;
    for (int i = 0; i < g.nsyms; i++) {
        if (list_is_written(&g.syms[i], 1)) {
            mark_usage(g.syms[i].eq);
            s_nterms += g.syms[i].eq->nterms;
        }
    }
    s_ninputs = 0;
    s_noutputs = 0;
    for (int i = 0; i < s_nsyms; i++) {
        if (s_usage[i] & USE_INPUT) {
            s_ninputs++;
        }
        if (s_usage[i] & USE_OUTPUT) {
            s_noutputs++;
        }
        s_noutputs += split_count(g.syms[i].eq);
    }
    int npins_used = 0;
    int nnodes_used = 0;
    for (int i = 2; i < g.nsyms; i++) {
        if (g.syms[i].name[0] != '.') {
            char cls = k_type_class[g.syms[i].type & 0x0f];
            if (cls == 'N') {
                nnodes_used++;
            } else if (cls == 'V') {
                npins_used++;
            }
        }
    }
    if (npins_used > 0) {
        stream_printf(fp, "%s %s  %2d ", s_prefix, s_pins_kw, npins_used);
        fflush(fp);
        for (int i = 2; i < s_nsyms; i++) {
            const symbol_t *s = &g.syms[i];
            if (k_type_class[s->type & 0x0f] != 'V') {
                continue;
            }
            stream_printf(fp, "%s", s->name);
            fflush(fp);
            int mark = !s_no_o || has_register_output(s->name);
            if ((s->flags & SYMF_INVERTED) && mark) {
                stream_printf(fp, "-");
            } else if (!(pf & PF_NO_PLUS)) {
                stream_printf(fp, "+");
            }
            put_pin_number(fp, pf, s->pin);
            put_blanks(fp, 1);
            fflush(fp);
        }
        stream_printf(fp, "\n");
        fflush(fp);
    }
    if (nnodes_used > 0) {
        stream_printf(fp, "%s %s %2d ", s_prefix, s_nodes_kw, nnodes_used);
        for (int i = 2; i < s_nsyms; i++) {
            const symbol_t *s = &g.syms[i];
            if (k_type_class[s->type & 0x0f] != 'N') {
                continue;
            }
            stream_printf(fp, "%s", s->name);
            if (!s_no_o && (s->flags & SYMF_INVERTED)) {
                stream_printf(fp, "-");
            } else if (!(pf & PF_NO_PLUS)) {
                stream_printf(fp, "+");
            }
            int number = s->pin;
            if ((format == 0x0d || format == 0x0a) && npin < number) {
                number -= npin - 1;
            }
            put_pin_number(fp, pf, number);
            put_blanks(fp, 1);
        }
        stream_printf(fp, "\n");
        fflush(fp);
    }
    if (format == 1 || format == 0x0a || format == 0x0d) {
        for (int i = 0; i < g.nsyms; i++) {
            if ((g.syms[i].type & 0x0f) == 0x0e && g.syms[i].eq != NULL) {
                write_group(fp, g.syms[i].eq);
            }
        }
    }
    stream_printf(fp, ".i %d\n", s_ninputs);
    fflush(fp);
    stream_printf(fp, ".o %d\n", s_noutputs);
    fflush(fp);
    if (!(pf & PF_NO_TYPE)) {
        stream_printf(fp, ".type f\n");
        fflush(fp);
    }
    int col = 0;
    for (int i = 0; i < s_nsyms; i++) {
        const symbol_t *s = &g.syms[i];
        if (!(s_usage[i] & USE_INPUT)) {
            continue;
        }
        col = start_label(fp, ".ilb", col);
        if (pf & PF_SHORT_LABEL) {
            col = 1;
        }
        col += stream_printf(fp, "%s", s->name);
        if (SYM_EXT(s)[0] == '\0') {
            int registered;
            if (format == 3 || format == 0x0a || format == 0x0d) {
                registered = input_with_ext(s->name, "PIN", "IO") != 0;
            } else {
                registered = has_register_output(s->name);
            }
            if (registered) {
                col = put_register_suffix(fp, col);
            }
        } else {
            const char *alias = extension_alias(SYM_EXT(s));
            if (alias[0] != '\0') {
                col += stream_printf(fp, ".%s", alias);
            }
        }
        col += put_blanks(fp, 1);
    }
    stream_printf(fp, "\n");
    fflush(fp);
    s_phase = alloc_zeroed((size_t)s_noutputs + 2 + 16, 1, "");
    size_t phase_cap = (size_t)s_noutputs + 2 + 16;
    col = 0;
    for (int i = 0; i < s_nsyms; i++) {
        const symbol_t *s = &g.syms[i];
        char name[NAME_BUF];
        char ext[NAME_BUF];
        if (!(s_usage[i] & USE_OUTPUT)) {
            continue;
        }
        col = start_label(fp, ".ob", col);
        if (pf & PF_SHORT_LABEL) {
            col = 1;
        }
        snprintf(name, sizeof(name), "%s", s->name);
        ext[0] = '\0';
        int splits = split_count(s->eq);
        if (SYM_EXT(s)[0] != '\0') {
            if (format == 0x0a) {
                if (splits == 0) {
                    int kind = feedback_kind(s);
                    if (kind == 1) {
                        append(name, sizeof(name), "_R");
                        col += 2;
                    } else if (kind == 2) {
                        append(name, sizeof(name), "_L");
                        col += 2;
                    }
                    snprintf(ext, sizeof(ext), "%s", SYM_EXT(s));
                } else {
                    if (splits > 1) {
                        fatal_error(ERR_TOO_MANY_XOR, name);
                    }
                    col += put_xor_pair(fp, name, sizeof(name));
                }
            } else if (format == 0x0d) {
                if (feedback_kind(s) > 0) {
                    append(name, sizeof(name), "_R");
                    col += 2;
                }
                snprintf(ext, sizeof(ext), "%s", SYM_EXT(s));
            } else {
                control_extension(s, ext, sizeof(ext));
            }
            if (splits == 0 || format != 0x0a) {
                const char *alias = extension_alias(ext);
                if (alias[0] != '\0') {
                    append(name, sizeof(name), ".");
                    append(name, sizeof(name), alias);
                }
            }
        }
        if (splits == 0) {
            col += stream_printf(fp, "%s", name);
        } else if (format == 0x0a) {
            if (SYM_EXT(s)[0] == '\0') {
                if (splits > 1) {
                    fatal_error(ERR_TOO_MANY_XOR, name);
                }
                col += put_xor_pair(fp, name, sizeof(name));
            }
        } else {
            append(name, sizeof(name), ".X0");
            for (int k = 0; k <= splits; k++) {
                name[strlen(name) - 1]++;
                col += stream_printf(fp, "%s", name);
                if (k != splits) {
                    col += put_blanks(fp, 1);
                }
            }
        }
        /* every output is given phase 1; only the '-' mark differs */
        if (is_polarity_output(s)) {
            if (format == 0x0d || !has_register_output(s->name)) {
                if (((s->flags & SYMF_INVERTED) != 0) != ((s->flags & SYMF_20) != 0)) {
                    stream_printf(fp, "-");
                }
            } else if (s->flags & SYMF_20) {
                stream_printf(fp, "-");
            }
        }
        append(s_phase, phase_cap, "1");
        if (splits > 0) {
            append(s_phase, phase_cap, "1");
        }
        col += put_blanks(fp, 1);
    }
    stream_printf(fp, "\n");
    fflush(fp);
    stream_printf(fp, ".phase %s\n", s_phase);
    fflush(fp);
    for (int i = 0; i < g.nsyms; i++) {
        if (list_is_written(&g.syms[i], 1)) {
            build_rows(g.syms[i].eq, format);
        }
    }
    merge_rows();
    stream_printf(fp, ".p %d\n", s_nterms);
    fflush(fp);
    write_rows(fp);
    stream_printf(fp, ".e\n");
    fflush(fp);
    close_file(fp);
}

static void copy_file(const char *from, const char *to)
{
    FILE *in = open_file(from, "rb");
    if (in == NULL) {
        return;
    }
    FILE *out = open_file(to, "wb");
    if (out != NULL) {
        char buf[0x7fff];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), in)) != 0) {
            fwrite(buf, 1, n, out);
        }
        close_file(out);
    }
    close_file(in);
}

/* PLA file for the fitter of the device */
void write_fitter_pla(int format)
{
    const char *letters = (const char *)attr_ptr(0x70);
    unsigned pf = 0;
    if (strchr(letters, 'p') != NULL) {
        pf |= PF_NO_PIN0;
    }
    if (strchr(letters, 'F') != NULL) {
        pf |= PF_NO_TYPE;
    }
    if (strchr(letters, '2') != NULL) {
        pf |= PF_COPY_TT2;
    }
    if (strchr(letters, 's') != NULL) {
        pf |= PF_SHORT_LABEL;
    }
    if (strchr(letters, 'P') != NULL) {
        pf |= PF_NO_PLUS;
    }
    if (strchr(letters, 'T') != NULL) {
        pf |= PF_TOOL;
    }
    if (strchr(letters, 'D') != NULL) {
        pf |= PF_NO_DEVICE;
    }
    char *path = make_path(source_dir(g.args[5]), g.args[5], ".pla");
    s_rows_head = s_rows_tail = NULL;
    s_nterms = s_ninputs = s_noutputs = s_nsyms = 0;
    s_phase = NULL;
    write_fitter_file(path, format, pf);
    if (pf & PF_COPY_TT2) {
        copy_file(path, make_path(source_dir(g.args[5]), g.args[5], ".tt2"));
    }
}
