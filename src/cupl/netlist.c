/*
 * netlist.c - design files for other tools: PALASM equations (.pds, option c),
 * P-CAD PDIF schematic symbol (.pdf, option p) and EDIF netlist (.edf,
 * option t).
 */
#include "banner.h"
#include "fusemap.h"
#include "strutil.h"

/* =========================================================================
 * PALASM
 * ========================================================================= */
static FILE *s_pds_fp;
static char *s_pds_name;
static int s_pds_indent;            /* indentation of continued product terms */
static int s_pds_user;              /* virtual device, written as "USER" */

/* PALASM names of the devices */
static const struct {
    const char *device;
    const char *palasm;
} k_palasm_devices[] = {
    { "xilinx", "LCA" }, { "virtual", "USER" }, { "p10h8", "PAL10H8" }, { "p10l8", "PAL10L8" },
    { "p12h6", "PAL12H6" }, { "p12l6", "PAL12L6" }, { "p14h4", "PAL14H4" }, { "p14l4", "PAL14L4" },
    { "p16h2", "PAL16H2" }, { "p16l2", "PAL16L2" }, { "p16l8", "PAL16L8" }, { "p16p8", "PAL16P8" },
    { "p16c1", "PAL16C1" }, { "p16r4", "PAL16R4" }, { "p16r6", "PAL16R6" }, { "p16r8", "PAL16R8" },
    { "p16ra8", "PAL16RA8" }, { "p16rp4", "PAL16RP4" }, { "p16rp6", "PAL16RP6" }, { "p16rp8", "PAL16RP8" },
    { "p6l16", "PAL6L16" }, { "p1020p8", "PAL10H20P8" }, { "p1020g8", "PAL10H20G8" },
    { "p1020eg8", "PAL10H20EG8" }, { "p1020ev8", "PAL10H20EV8" }, { "p12l10", "PAL12L10" },
    { "p14l8", "PAL14L8" }, { "p16l6", "PAL16L6" }, { "p18l4", "PAL18L4" }, { "p20l2", "PAL20L2" },
    { "p20c1", "PAL20C1" }, { "p20l8", "PAL20L8" }, { "p20l10", "PAL20L10" }, { "p20x4", "PAL20X4" },
    { "p20x8", "PAL20X8" }, { "p20x10", "PAL20X10" }, { "p20r4", "PAL20R4" }, { "p20r6", "PAL20R6" },
    { "p20r8", "PAL20R8" }, { "p20ra10", "PAL20RA10" }, { "p20s10", "PAL20s10" }, { "p20rs4", "PAL20RS4" },
    { "p20rs10", "PAL20RS10" }, { "p22ip6", "PAL22IP6" }, { "p22rx8", "PAL22RX8" }
};

/* PALASM spelling of an extension; ":" marks register inputs */
static const struct {
    const char *ext;
    const char *palasm;
} k_palasm_exts[] = {
    { "oe", ".TRST" }, { "ar", ".RSTF" }, { "ap", ".SETF" }, { "sp", ".SETF" }, { "sr", ".RSTF" }, { "r", ":" },
    { "s", ":" }, { "j", ":" }, { "k", ":" }, { "byp", ".CMBF" }, { "ck", ".CLKF" }, { "ckmux", ".CLKF" },
    { "oemux", ".TRST" }, { "t", ":" }, { "d", ":" }
};

/* set/reset terms a register may have by default */
static const struct {
    const char *ext;
    const char *palasm;
    unsigned pair_ext;
} k_palasm_controls[4] = {
    { "ap", ".SETF", 5 }, { "sp", ".SETF", 3 }, { "ar", ".RSTF", 7 }, { "sr", ".RSTF", 6 }
};

/* Symbol with the given name and extension, including the first two */
static symbol_t *find_any_symbol(const char *name, const char *ext)
{
    for (int i = 0; i < g.nsyms; i++) {
        if (strcmp(name, g.syms[i].name) == 0 && strcmp(SYM_EXT(&g.syms[i]), ext) == 0) {
            return &g.syms[i];
        }
    }
    return NULL;
}

/* PALASM spelling of an extension, or NULL */
static const char *palasm_ext(const char *ext)
{
    for (size_t i = 0; i < sizeof(k_palasm_exts) / sizeof(k_palasm_exts[0]); i++) {
        if (strcmp(ext, k_palasm_exts[i].ext) == 0) {
            return k_palasm_exts[i].palasm;
        }
    }
    return NULL;
}

/* One variable of a product, '/' if negated */
static void pds_variable(int positive, int index)
{
    const symbol_t *s = &g.syms[index];
    FILE *fp = s_pds_fp;
    switch (s->type & 0x0f) {
    case 1:
    case 2:
    case 3:
    case 9:
        if (!positive) {
            out_putc('/', fp);
        }
        stream_printf(fp, "%s", s->name);
        stream_printf(fp, " ");
        s_pds_indent = (int)strlen(s->name);
        break;
    case 5:
    case 8:
    case 10: {
        if (!positive) {
            out_putc('/', fp);
        }
        const char *suffix = palasm_ext(SYM_EXT(s));
        if (suffix == NULL) {
            if (!s_pds_user) {
                trace_printf("WARNING: PDS2XNF does not support this macro configuration\n");
                trace_printf("         extension %s is ignored in output file \n", SYM_EXT(s));
            }
            stream_printf(fp, "%s", s->name);
        } else if (suffix[0] == ':') {
            stream_printf(fp, "%s %s", s->name, suffix);
        } else {
            stream_printf(fp, "%s%s ", s->name, suffix);
        }
        s_pds_indent = (int)(strlen(s->name) + (suffix != NULL ? strlen(suffix) : 0));
        break;
    }
    case 6:
        stream_printf(fp, positive ? "GND " : "VCC ");
        break;
    case 7:
        stream_printf(fp, positive ? "VCC " : "GND ");
        break;
    default:
        break;
    }
}

/* Sum of products; with_lhs writes "name = " first */
static void pds_terms(const equation_t *eq, int with_lhs)
{
    FILE *fp = s_pds_fp;
    int is_list = 0;
    int sep = 0;
    for (int i = 0; i < eq->nvars; i++) {
        if (!term_bit(TERM_OUT(eq, 0), i)) {
            continue;
        }
        if (with_lhs) {
            pds_variable(!(g.syms[i].flags & SYMF_20), i);
        }
        if ((g.syms[i].type & 0x0f) == 1) {
            sep = ',';
            is_list = 1;
        } else {
            sep = '*';
        }
    }
    int indent = s_pds_indent;
    if (with_lhs) {
        stream_printf(fp, "= ");
    }
    int first_term = 1;
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
                stream_printf(fp, " %c ", sep);
            } else {
                first_var = 0;
            }
            pds_variable(term_bit(TERM_VAL(eq, t), i), i);
            if (i == 0) {
                break;
            }
        }
        first_term = 0;
        if (!with_lhs && eq->nterms - 1 == t) {
            break;
        }
        stream_printf(fp, "\n");
        for (int k = 0; k < indent; k++) {
            out_putc(' ', fp);
        }
        if (care[0] & TERM_MORE) {
            stream_printf(fp, "+ ");
        } else if (care[0] & TERM_SPLIT) {
            stream_printf(fp, ":+: ");
        }
    }
    if (with_lhs) {
        if (is_list) {
            out_putc('\n', fp);
        }
        out_putc('\n', fp);
    }
}

/* First symbol whose node is on the given OR array */
static symbol_t *find_symbol_on_array(unsigned array)
{
    for (int i = 0; i < g.nsyms; i++) {
        if (g.syms[i].node_index >= 0 && node16(g.syms[i].node_index, NODE_ARRAY) == array) {
            return &g.syms[i];
        }
    }
    return NULL;
}

/* Set / reset terms of a register that the design did not give */
static void pds_default_controls(const symbol_t *s)
{
    for (int i = 0; i < 4; i++) {
        if (find_any_symbol(s->name, k_palasm_controls[i].ext) != NULL) {
            continue;
        }
        int rec = find_extension_pair(s, s->pin, k_palasm_controls[i].pair_ext);
        symbol_t *term = find_symbol_on_array(rd16(pair_rec(rec) + PAIR_W0));
        if (term != NULL && term->eq != NULL) {
            stream_printf(s_pds_fp, "%s%s = ", s->name, k_palasm_controls[i].palasm);
            pds_terms(term->eq, 0);
            stream_printf(s_pds_fp, "\n\n");
        }
    }
}

/* CHIP line and pin list */
static void pds_write_chip(void)
{
    FILE *fp = s_pds_fp;
    int col = 0;
    int count, nodes, gnd, vcc;
    if (device_type() == DEVT_VIRTUAL) {
        /* highest pin number used, or the number of pins if larger */
        int highest = 0;
        int used = 0;
        s_pds_user = 1;
        for (int i = 0; i < g.nsyms; i++) {
            if ((g.syms[i].type & 0x0f) == 2) {
                if (highest < g.syms[i].pin) {
                    highest = g.syms[i].pin;
                }
                used++;
            }
        }
        count = used <= highest ? highest : used;
        nodes = gnd = vcc = 0;
    } else {
        count = (int)attr8(ATTR_NPINS);
        nodes = (int)attr16(ATTR_NNODES);
        gnd = (int)attr8(2);
        vcc = (int)attr8(3);
    }
    const char *device = NULL;
    for (size_t i = 0; i < sizeof(k_palasm_devices) / sizeof(k_palasm_devices[0]); i++) {
        if (strcmp((const char *)g.lib.dir_entry, k_palasm_devices[i].device) == 0) {
            device = k_palasm_devices[i].palasm;
            break;
        }
    }
    if (device == NULL) {
        device = "USER";
        trace_printf("WARNING:  %s is not recognized as a PALASM2 device\n", (const char *)g.lib.dir_entry);
        trace_printf("          Device name will be replaced by USER\n");
    }
    stream_printf(fp, "\nCHIP %s %s\n", make_path("", g.args[5], ""), device);
    if (strcmp(device, "USER") != 0) {
        count += nodes;
    }
    if (s_pds_user) {
        for (int i = 0; i < g.nsyms; i++) {
            if ((g.syms[i].type & 0x0f) == 2) {
                stream_printf(fp, "%s ", g.syms[i].name);
                col += (int)strlen(g.syms[i].name) + 1;
            }
        }
        return;
    }
    for (int pin = 1; pin <= count; pin++) {
        if (vcc == pin) {
            stream_printf(fp, "VCC ");
            col += 4;
        } else if (gnd == pin) {
            stream_printf(fp, "GND ");
            col += 4;
        } else {
            const symbol_t *s = NULL;
            for (int i = 0; i < g.nsyms; i++) {
                if ((g.syms[i].type & 0x0f) == 2 && g.syms[i].pin == pin) {
                    s = &g.syms[i];
                    break;
                }
            }
            if (s == NULL) {
                stream_printf(fp, "NC ");
                col += 3;
            } else {
                if (s->flags & SYMF_INVERTED) {
                    out_putc('/', fp);
                    col++;
                }
                stream_printf(fp, "%s ", s->name);
                col += (int)strlen(s->name) + 1;
            }
            if (col > 0x3c) {
                stream_printf(fp, "\n");
                col = 0;
            }
        }
    }
}

static void pds_blanks(int count)
{
    for (int i = 0; i < count; i++) {
        out_putc(' ', s_pds_fp);
    }
}

static int is_register_ext(const char *ext)
{
    return strcmp(ext, "d") == 0 || strcmp(ext, "r") == 0 || strcmp(ext, "k") == 0 || strcmp(ext, "t") == 0
           || strcmp(ext, "l") == 0;
}

/* EQUATIONS section; registers get .CLKF and .TRST lines for the clock and
 * enable pins that drive them */
static void pds_write_equations(void)
{
    FILE *fp = s_pds_fp;
    const symbol_t *clocks[4];      /* at most four clock pins are used */
    int nclocks = 0;
    const symbol_t *enable = NULL;
    stream_printf(fp, "\n\nEQUATIONS\n\n");
    if (!s_pds_user) {
        for (int i = 0; i < g.nsyms; i++) {
            const symbol_t *s = &g.syms[i];
            if (s->pin_index < 0) {
                continue;
            }
            unsigned kind = pin8(s->pin_index, PIN_KIND);
            if (strcmp(SYM_EXT(s), "ck") != 0 && (kind == 5 || kind == 0x0c) && nclocks < 4) {
                clocks[nclocks++] = s;
            }
            if (strcmp(SYM_EXT(s), "oe") != 0 && (kind == 9 || kind == 0x0d)) {
                enable = s;
            }
        }
    }
    for (int i = 0; i < g.nsyms; i++) {
        const symbol_t *s = &g.syms[i];
        const char *ext = SYM_EXT(s);
        unsigned type = s->type & 0x0f;
        if (s->eq == NULL || s->name[0] == '.' || type == 1 || type == 3) {
            continue;
        }
        if (strcmp(ext, "t") == 0) {
            stream_printf(fp, "%s := %s :+:\n", s->name, s->name);
            s_pds_indent = (int)strlen(s->name) + 2;
            pds_blanks(s_pds_indent);
            out_putc('(', fp);
            pds_terms(s->eq, 0);
            stream_printf(fp, ")\n");
        } else if (strcmp(ext, "k") == 0 || strcmp(ext, "r") == 0) {
            int is_k = ext[0] == 'k';
            stream_printf(fp, "%s := %s * /(", s->name, s->name);
            s_pds_indent = (int)strlen(s->name) + 2;
            pds_terms(s->eq, 0);
            stream_printf(fp, ") + \n");
            s_pds_indent = (int)strlen(s->name) + 2;
            pds_blanks(s_pds_indent);
            if (is_k) {
                stream_printf(fp, "/%s * (", s->name);
            } else {
                stream_printf(fp, "(");
            }
            const symbol_t *other = find_any_symbol(s->name, is_k ? "j" : "s");
            if (other != NULL && other->eq != NULL) {
                pds_terms(other->eq, 0);
            }
            stream_printf(fp, ")\n\n");
        } else if (strcmp(ext, "j") == 0 || strcmp(ext, "s") == 0) {
            continue;
        } else {
            pds_terms(s->eq, 1);
        }
        if (!s_pds_user && is_register_ext(ext)) {
            for (int c = 0; c < nclocks; c++) {
                if (find_any_symbol(s->name, "ck") == NULL && clocks[c] != NULL
                    && find_extension_pair(s, clocks[c]->pin, 0x0f) != -1) {
                    stream_printf(fp, "%s.CLKF = %s\n\n", s->name, clocks[c]->name);
                    break;
                }
            }
            pds_default_controls(s);
        }
        if (!s_pds_user && s->node_index >= 0 && enable != NULL && find_extension_pair(s, enable->pin, 0x11) != -1
            && s->pin <= attr8(ATTR_NPINS) && find_any_symbol(s->name, "oe") == NULL) {
            stream_printf(fp, "%s.TRST = %s\n\n", s->name, enable->name);
        }
    }
}

/* PALASM equation file <design>.pds */
void write_palasm(void)
{
    static const char *const labels[6] = { "TITLE", "PATTERN", "REVISION", "DATE", "AUTHOR", "COMPANY" };
    s_pds_name = make_path(source_dir(g.args[5]), g.args[5], ".pds");
    s_pds_fp = open_file(s_pds_name, "wt");
    if (s_pds_fp == NULL) {
        fatal_error(ERR_OPEN, s_pds_name);
    }
    for (int i = 0; i < 6; i++) {
        stream_printf(s_pds_fp, "%-16s%s\n", labels[i], header_string(i));
    }
    pds_write_chip();
    pds_write_equations();
    close_file(s_pds_fp);
    s_pds_fp = NULL;
}

/* =========================================================================
 * PDIF symbol
 * ========================================================================= */
typedef struct {
    uint8_t kind;           /* 1 enable, 2 clock, 3 input, 5 I/O, 6 output */
    uint8_t inverted;
    char name[11];
} pdif_pin_t;

static pdif_pin_t *s_pins;          /* indexed by pin number */
static int s_npins;
static int s_kind_count[7];         /* pins of each kind */
static int s_longest_in;            /* longest name on the input side */
static int s_longest_out;           /* longest name on the output side */
static int s_left, s_right;         /* pins on each side */
static int s_org_x, s_org_y;
static int s_width, s_height, s_base; /* size of the symbol body */

/* Number a name ends with, or -1 */
static int trailing_number(const char *name)
{
    const char *p = name + strlen(name);
    while (p > name && p[-1] >= '0' && p[-1] <= '9') {
        p--;
    }
    if (*p < '0' || *p > '9') {
        return -1;
    }
    return (int)strtol(p, NULL, 10);
}

/* No other pin has this name */
static int pdif_name_is_unique(const char *name, int pin)
{
    for (int i = 1; i <= (int)attr8(ATTR_NPINS); i++) {
        if (i != pin && strcmp(name, s_pins[i].name) == 0) {
            return 0;
        }
    }
    return 1;
}

static void pdif_set_kind(int pin, int kind)
{
    s_kind_count[kind]++;
    s_pins[pin].kind = (uint8_t)kind;
}

/* Sort the pins into kinds and make names of at most 8 characters */
static void pdif_classify(void)
{
    for (int k = 0; k < 7; k++) {
        if (k != 4) {
            s_kind_count[k] = 0;
        }
    }
    for (int i = 0; i < g.nsyms; i++) {
        const symbol_t *s = &g.syms[i];
        if (s->type != 2 || s->pin >= s_npins) {
            continue;
        }
        int pin = s->pin;
        s_pins[pin].inverted = s->flags & SYMF_INVERTED;
        if (s->node_index >= 0) {
            unsigned e = node8(s->node_index, NODE_EXT);
            if (e == 1 || (e >= 8 && e <= 11) || e == 0x15 || e == 0x16 || e == 0x1b || e == 0x1c) {
                pdif_set_kind(pin, 6);
            } else if (s->flags & SYMF_40) {
                pdif_set_kind(pin, 5);
            }
        }
        if (s->pin_index >= 0) {
            switch (pin8(s->pin_index, PIN_KIND)) {
            case 7:
            case 0x10:
                if (s->flags & SYMF_40) {
                    break;
                }
                /* fall through */
            case 0:
            case 2:
            case 10:
            case 0x11:
            case 0x16:
                pdif_set_kind(pin, 3);
                break;
            case 3:
            case 4:
                pdif_set_kind(pin, 6);
                break;
            case 5:
            case 0x0c:
            case 0x14:
            case 0x15:
                pdif_set_kind(pin, 2);
                break;
            case 9:
            case 0x0d:
                pdif_set_kind(pin, 1);
                break;
            default:
                break;
            }
        }
    }
    for (int i = 0; i < g.nsyms; i++) {
        const symbol_t *s = &g.syms[i];
        if (s->type == 2 && s->pin < s_npins && strlen(s->name) < 9) {
            strcpy(s_pins[s->pin].name, s->name);
            lincupl_strupr(s_pins[s->pin].name);
        }
    }
    for (int i = 0; i < g.nsyms; i++) {
        const symbol_t *s = &g.syms[i];
        if (s->type != 2 || s->pin >= s_npins || strlen(s->name) <= 8) {
            continue;
        }
        char *name = s_pins[s->pin].name;
        int len = 8;
        int unique;
        do {
            int number = trailing_number(s->name);
            if (number < 0) {
                memcpy(name, s->name, (size_t)len);
                name[len] = '\0';
            } else {
                memcpy(name, s->name, (size_t)len - 2);
                snprintf(name + len - 2, sizeof(s_pins[0].name) - (size_t)(len - 2), "%d", number);
            }
            len--;
            unique = pdif_name_is_unique(name, s->pin);
        } while (!unique && len > 2);
        lincupl_strupr(name);
    }
    for (int pin = 1; pin <= (int)attr8(ATTR_NPINS); pin++) {
        int len = (int)strlen(s_pins[pin].name);
        if (s_pins[pin].inverted && len + 1 < (int)sizeof(s_pins[0].name)) {
            strcat(s_pins[pin].name, "'");
        }
        int kind = s_pins[pin].kind;
        if (kind >= 1 && kind <= 3) {
            if (s_longest_in < len) {
                s_longest_in = len;
            }
        } else if ((kind == 5 || kind == 6) && s_longest_out < len) {
            s_longest_out = len;
        }
    }
}

/* ENVIRONMENT, USER and DISPLAY sections */
static void pdif_environment(FILE *fp)
{
    char when[40];
    snprintf(when, sizeof(when), "%s", creation_time_text());
    char *nl = strchr(when, '\n');
    if (nl != NULL) {
        *nl = '\0';
    }
    stream_printf(fp, " {ENVIRONMENT\n");
    stream_printf(fp, "  {PDIFvrev 3.00}\n");
    stream_printf(fp, "  {Program \"%s Version %s\"}\n", PROGRAM_NAME, PROGRAM_VERSION);
    stream_printf(fp, "  {DBtype \"Schematic\"}\n");
    stream_printf(fp, "  {DBvrev 1.01}\n");
    stream_printf(fp, "  {DBtime \"%s \"}\n", when);
    stream_printf(fp, "  {DBunit \"MIL\"}\n");
    stream_printf(fp, "  {DBgrid %d}\n", 10);
    stream_printf(fp, "  {Lyrstr \"WIRES\" 1 \"BUS\" 1 \"GATE\" 2 \"IEEE\" 2 \"PINFUN\" 3 \"PINNUM\" 1 \n");
    stream_printf(fp, "          \"PINNAM\" 6 \"PINCON\" 4 \"REFDES\" 2 \"ATTR\" 6 \"SDOT\" 1 \n");
    stream_printf(fp, "          \"DEVICE\" 5 \"OUTLIN\" 5 \"ATTR2\" 6 \"NOTES\" 6 \"NETNAM\" 4 \n");
    stream_printf(fp, "          \"CMPNAM\" 5 \"BORDER\" 5}\n");
    stream_printf(fp, " }\n\n");
    stream_printf(fp, " {USER\n");
    stream_printf(fp, "  {VIEW\n");
    stream_printf(fp, "   {Mode SYMB}\n");
    stream_printf(fp, "   {Nlst OPEN}\n");
    stream_printf(fp, "   {Vw 0 0 2}\n");
    stream_printf(fp, "   {Lv 12 2 2 2 0 0 2 2 2 2 0 0 2 1 2 0 0 0 0}\n");
    stream_printf(fp, "   {Gs %d %d}\n", 10, 10);
    stream_printf(fp, "  }\n");
    stream_printf(fp, " }\n\n");
    stream_printf(fp, " {DISPLAY\n");
    stream_printf(fp, "  [Ly \"PINNUM\"]\n");
    stream_printf(fp, "  [Ls \"SOLID\"][Wd 0]\n");
    stream_printf(fp, "  [Ts 15][Tj \"LC\"][Tr 0][Tm \"N\"]\n");
    stream_printf(fp, " }\n\n");
}

static int pdif_npins(void)
{
    return (int)attr8(ATTR_NPINS);
}

static int left_gap(void)
{
    return s_kind_count[1] > 0 || s_kind_count[2] > 0;
}

/* PIN_DEF section: pin connection points, inputs left, outputs right */
static void pdif_pin_def(FILE *fp)
{
    static const char *const types[7] = { "", "INPUT", "INPUT", "INPUT", "", "I/O", "OUTPUT" };
    stream_printf(fp, "  {PIN_DEF\n");
    stream_printf(fp, "   [Ly \"PINCON\"]\n");
    int x = 100;
    int y = s_left * 20 + s_base;
    for (int kind = 1; kind <= 3; kind++) {
        if (kind == 3 && left_gap()) {
            y -= 20;
        }
        if (s_kind_count[kind] <= 0) {
            continue;
        }
        for (int pin = 1; pin <= pdif_npins(); pin++) {
            if (s_pins[pin].kind == kind) {
                stream_printf(fp, "   {P %s {Pt \"%s\"}{Lq 0}{Ploc %d %d}}\n", s_pins[pin].name, types[kind], x, y);
                y -= 20;
            }
        }
    }
    s_org_x = x;
    s_org_y = y + 20;
    x = s_width + 0xa0;
    y = s_base + 20;
    for (int kind = 5; kind <= 6; kind++) {
        if (s_kind_count[kind] <= 0) {
            continue;
        }
        for (int pin = 1; pin <= pdif_npins(); pin++) {
            if (s_pins[pin].kind == kind) {
                stream_printf(fp, "   {P %s {Pt \"%s\"}{Lq 0}{Ploc %d %d}}\n", s_pins[pin].name, types[kind], x, y);
                y += 20;
            }
        }
    }
    stream_printf(fp, "  }\n\n");
}

/* PKG section: reference designator, pin number positions and pin order */
static void pdif_package(FILE *fp)
{
    stream_printf(fp, "  {PKG\n");
    stream_printf(fp, "   [Ly \"REFDES\"]\n");
    stream_printf(fp, "   [Ts 25][Tj \"CB\"][Tr 0][Tm \"N\"]\n");
    stream_printf(fp, "   {Rdl %d %d}\n\n", (s_width >> 1) + (s_width & 1) + 0x82, s_height + 10);
    stream_printf(fp, "   [Ly \"PINNUM\"]\n");
    int x = 0x78;
    int y = s_left * 20 + s_base + 10;
    if (left_gap()) {
        stream_printf(fp, "   [Ts 15][Tj \"RC\"]\n");
        for (int pin = 1; pin <= pdif_npins(); pin++) {
            if (s_pins[pin].kind == 1 || s_pins[pin].kind == 2) {
                stream_printf(fp, "   {Pnl %d %d}\n", x, y);
                y -= 20;
            }
        }
        y -= 20;
    }
    if (s_kind_count[3] > 0) {
        stream_printf(fp, "   [Ts 15][Tj \"RC\"]\n");
        for (int pin = 1; pin <= pdif_npins(); pin++) {
            if (s_pins[pin].kind == 3) {
                stream_printf(fp, "   {Pnl %d %d}\n", x, y);
                y -= 20;
            }
        }
    }
    x = s_width + 0x8c;
    y = s_base + 0x1e;
    if (s_right > 0) {
        stream_printf(fp, "   [Ts 15][Tj \"LC\"]\n");
        for (int pin = 1; pin <= pdif_npins(); pin++) {
            if (s_pins[pin].kind == 5 || s_pins[pin].kind == 6) {
                stream_printf(fp, "   {Pnl %d %d}\n", x, y);
                y += 20;
            }
        }
    }
    stream_printf(fp, "\n   {Sd A");
    for (int kind = 1; kind < 7; kind++) {
        int n = 1;
        for (int pin = 1; n <= s_kind_count[kind] && pin <= pdif_npins(); pin++) {
            if (s_pins[pin].kind == kind) {
                stream_printf(fp, " %d", pin);
                n++;
            }
        }
    }
    stream_printf(fp, "}\n");
    stream_printf(fp, "  }\n\n");
}

/* Pin stub on the left side; an inverted pin gets a bubble */
static void pdif_left_stub(FILE *fp, int inverted, int x0, int x1, int y)
{
    if (inverted) {
        stream_printf(fp, "   {L %d %d %d %d}\n", x0 - 10, y, x1, y);
        stream_printf(fp, "   {C %d %d %d}\n", x0 - 5, y, 5);
    } else {
        stream_printf(fp, "   {L %d %d %d %d}\n", x0, y, x1, y);
    }
}

/* PIC section: body outline, pin stubs, pin names and device name */
static void pdif_picture(FILE *fp)
{
    stream_printf(fp, "  {PIC\n");
    stream_printf(fp, "   [Ly \"GATE\"]\n");
    stream_printf(fp, "   [Ts 15][Tj \"LC\"][Tr 0][Tm \"N\"]\n");
    stream_printf(fp, "   {R %d %d %d %d}\n", 0x82, s_height, s_width + 0x82, s_base);
    int x0 = 0x82;
    int x1 = x0 - 30;
    int y = s_left * 20 + s_base;
    for (int kind = 1; kind <= 3; kind++) {
        if (kind == 3 && left_gap()) {
            y -= 20;
        }
        if (s_kind_count[kind] <= 0) {
            continue;
        }
        for (int pin = 1; pin <= pdif_npins(); pin++) {
            if (s_pins[pin].kind != kind) {
                continue;
            }
            pdif_left_stub(fp, s_pins[pin].inverted, x0, x1, y);
            if (kind == 2) {
                stream_printf(fp, "   {L %d %d %d %d %d %d}\n", x0, y + 10, x0 + 10, y, x0, y - 10);
            }
            y -= 20;
        }
    }
    x0 = s_width + 0x82;
    x1 = x0 + 0x1e;
    y = s_base + 20;
    for (int kind = 5; kind <= 6; kind++) {
        if (s_kind_count[kind] <= 0) {
            continue;
        }
        for (int pin = 1; pin <= pdif_npins(); pin++) {
            if (s_pins[pin].kind != kind) {
                continue;
            }
            if (s_pins[pin].inverted) {
                stream_printf(fp, "   {C %d %d %d}\n", x0 + 5, y, 5);
                stream_printf(fp, "   {L %d %d %d %d}\n", x0 + 10, y, x1, y);
            } else {
                stream_printf(fp, "   {L %d %d %d %d}\n", x0, y, x1, y);
            }
            y += 20;
        }
    }
    stream_printf(fp, "   [Ly \"PINNAM\"]\n");
    stream_printf(fp, "   [Tj \"LC\"]\n");
    int x = 0x8c;
    y = s_left * 20 + s_base;
    for (int kind = 1; kind <= 3; kind++) {
        if (kind == 3 && left_gap()) {
            y -= 20;
        }
        if (s_kind_count[kind] <= 0) {
            continue;
        }
        for (int pin = 1; pin <= pdif_npins(); pin++) {
            if (s_pins[pin].kind == kind) {
                stream_printf(fp, "   {T \"%s\" %d %d}\n", s_pins[pin].name, x, y);
                y -= 20;
            }
        }
    }
    stream_printf(fp, "   [Tj \"RC\"]\n");
    x = s_width + 0x78;
    y = s_base + 20;
    for (int kind = 5; kind <= 6; kind++) {
        if (s_kind_count[kind] <= 0) {
            continue;
        }
        for (int pin = 1; pin <= pdif_npins(); pin++) {
            if (s_pins[pin].kind == kind) {
                stream_printf(fp, "   {T \"%s\" %d %d}\n", s_pins[pin].name, x, y);
                y += 20;
            }
        }
    }
    stream_printf(fp, "   [Ly \"DEVICE\"]\n");
    stream_printf(fp, "   [Tj \"CT\"]\n");
    char device[40];
    snprintf(device, sizeof(device), "%s", (const char *)g.lib.dir_entry);
    lincupl_strupr(device);
    stream_printf(fp, "   {T \"%s\" %d %d}\n", device, (s_width >> 1) + (s_width & 1) + 0x82, s_base - 10);
    stream_printf(fp, "  }\n\n");
}

/* SYMBOL section: size of the body, then its subsections */
static void pdif_symbol(FILE *fp, const char *component)
{
    s_left = s_kind_count[1] + s_kind_count[2] + s_kind_count[3];
    if (left_gap() && s_kind_count[3] > 0) {
        s_left++;
    }
    s_right = s_kind_count[5] + s_kind_count[6];
    int longest = s_right < s_left ? s_left : s_right;
    int width = (s_longest_in + s_longest_out + 4) * 10;
    s_width = width < 0x78 ? 0x78 : width;
    s_height = (longest + 1) * 20;
    s_base = 0;
    stream_printf(fp, " {SYMBOL\n");
    pdif_pin_def(fp);
    pdif_package(fp);
    pdif_picture(fp);
    /* attributes */
    stream_printf(fp, "  {ATR\n");
    stream_printf(fp, "   {IN\n");
    stream_printf(fp, "    {Org %d %d}\n", s_org_x, s_org_y);
    stream_printf(fp, "    {Ty 255}\n");
    stream_printf(fp, "   }\n");
    stream_printf(fp, "   {EX\n");
    stream_printf(fp, "    [Ly \"ATTR2\"]\n");
    stream_printf(fp, "    [Ts 12][Tj \"CT\"][Tr 0][Tm \"N\"]\n");
    stream_printf(fp, "    {At PLD %s %d %d}\n", component, (s_width >> 1) + (s_width & 1) + 0x82, s_height);
    stream_printf(fp, "   }\n");
    stream_printf(fp, "  }\n");
    stream_printf(fp, " }\n\n");
}

/* DETAIL section: one net per used pin */
static void pdif_detail(FILE *fp)
{
    stream_printf(fp, " {DETAIL\n");
    stream_printf(fp, "  {ANNOTATE\n");
    stream_printf(fp, "  }\n\n");
    stream_printf(fp, "  {NET_DEF\n");
    for (int kind = 1; kind < 7; kind++) {
        int n = 1;
        for (int pin = 1; n <= s_kind_count[kind] && pin <= pdif_npins(); pin++) {
            if (s_pins[pin].kind == kind) {
                stream_printf(fp, "   {N %s\n   }\n", s_pins[pin].name);
                n++;
            }
        }
    }
    stream_printf(fp, "  }\n\n");
    stream_printf(fp, "  {SUBCOMP\n");
    stream_printf(fp, "  }\n");
    stream_printf(fp, " }\n");
}

/* P-CAD PDIF symbol file <design>.pdf */
void write_pdif(void)
{
    char *name = make_path(source_dir(g.args[5]), g.args[5], ".pdf");
    FILE *fp = open_file(name, "wt");
    if (fp == NULL) {
        fatal_error(ERR_OPEN, name);
    }
    /* one entry per pin of the device; symbols with higher numbers are
     * ignored */
    s_npins = pdif_npins() + 1;
    s_pins = alloc_zeroed((size_t)s_npins + 1, sizeof(pdif_pin_t), "Building PDIF file");
    pdif_classify();
    char *component = dup_string(g.args[5], "Building PDIF file");
    lincupl_strupr(component);
    stream_printf(fp, "{COMPONENT %s.SYM\n\n", component);
    pdif_environment(fp);
    pdif_symbol(fp, component);
    pdif_detail(fp);
    stream_printf(fp, "}\n");
    free_tracked(component);
    free_tracked(s_pins);
    s_pins = NULL;
    close_file(fp);
}

/* =========================================================================
 * EDIF netlist
 * ========================================================================= */
typedef struct {
    char name[0x14];
    char instance[0x28];    /* "PIN<number>" */
    int cell;               /* code of the library cell, -1 = unused */
    uint8_t has_node;
    uint8_t inverter;       /* non-zero if the inverted signal is used */
} edif_pin_t;

typedef struct edif_term {
    int ninputs;
    int *inputs;            /* symbol index, negative = inverted; +-9999 = constant */
    int id;                 /* AND gate number, or +-9999 */
    struct edif_term *next;
} edif_term_t;

typedef struct {
    int nterms;
    int gate;               /* OR gate number */
    edif_term_t *terms;
    uint8_t used;
} edif_sym_t;

static FILE *s_edf_fp;
static char *s_edf_name;
static edif_pin_t *s_edf_pins;      /* indexed by pin or node number */
static int s_edf_npins;
static edif_sym_t *s_edf_syms;      /* indexed like g.syms */
static int s_edf_gate;              /* next gate number */
static int s_edf_net;               /* next net number */

/* library cells by code */
static const struct {
    const char *name;
    int code;
} k_edif_cells[48] = {
    { "M0", 0 }, { "M1", 101 }, { "M2", 100 }, { "M3", 131 }, { "M4", 121 }, { "M5", 130 }, { "M6", 120 },
    { "M7", 226 }, { "M8", 221 }, { "M9", 225 }, { "M10", 220 }, { "M11", 326 }, { "M12", 321 }, { "M13", 325 },
    { "M14", 320 }, { "M15", 505 }, { "M16", 111 }, { "M17", 110 }, { "M18", 305 }, { "M19", 300 },
    { "M20", 600 }, { "M21", 700 }, { "M22", 801 }, { "M23", 236 }, { "M24", 235 }, { "M25", 405 }, { "M26", 0 },
    { "M27", 0 }, { "M28", 0 }, { "M29", 0 }, { "M30", 0 }, { "M31", 0 }, { "M32", 0 }, { "M33", 0 }, { "M34", 1 },
    { "M35", 286 }, { "M36", 285 }, { "M35", 276 }, { "M36", 275 }, { "M35", 271 }, { "M36", 270 },
    { "M37", 266 }, { "M38", 265 }, { "M39", 256 }, { "M40", 255 }, { "M41", 0 }, { "M42", 0 }, { "M43", 0 }
};

/* Index of the library cell with the given code, or -1 */
static int edif_find_cell(int code)
{
    for (int i = 0; i < 48; i++) {
        if (k_edif_cells[i].code == code) {
            return i;
        }
    }
    return -1;
}

/* Cells whose output is the inverted signal */
static int edif_cell_inverts(int code)
{
    return code == 0xdd || code == 0xe2 || code == 0xe1 || code == 0xdc;
}

/* The table has one entry per pin and node of the device and is extended to
 * the largest number a symbol uses (node numbers of the fitter devices are
 * far larger).  The extra entries start out as zero bytes, not as "unused"
 * (cell -1), and are never printed. */
static edif_pin_t *edif_pin(unsigned number)
{
    return &s_edf_pins[number < (unsigned)s_edf_npins ? number : 0];
}

/* AND terms of one list; returns the number of terms */
static int edif_build_terms(const equation_t *eq, int owner)
{
    int count = 0;
    for (int t = 0; t < eq->nterms; t++) {
        const uint16_t *care = TERM_CARE(eq, t);
        const uint16_t *val = TERM_VAL(eq, t);
        int inputs[100];
        if (care[0] & TERM_DELETED) {
            continue;
        }
        count++;
        edif_term_t *term = alloc_zeroed(1, sizeof(*term), "creating edif netlist");
        for (int i = 0; i < eq->nvars; i++) {
            if (!term_bit(care, i)) {
                continue;
            }
            int value = 0;
            int have = 1;
            switch (g.syms[i].type & 0x0f) {
            case 2:
            case 5:
            case 9:
            case 10: {
                edif_pin_t *p = edif_pin(g.syms[i].pin);
                s_edf_syms[i].used = 1;
                if (edif_cell_inverts(p->cell) == term_bit(val, i)) {
                    if (p->inverter == 0) {
                        p->inverter = (uint8_t)s_edf_gate;
                        s_edf_gate++;
                    }
                    value = -i;
                } else {
                    value = i;
                }
                if (g.syms[i].node_index >= 0) {
                    p->has_node = 1;
                }
                break;
            }
            case 6:
                value = term_bit(val, i) ? -9999 : 9999;
                break;
            case 7:
                value = term_bit(val, i) ? 9999 : -9999;
                break;
            default:
                have = 0;
                break;
            }
            if (have && term->ninputs < 100) {
                inputs[term->ninputs++] = value;
            }
            if (i == 0) {
                break;
            }
        }
        if (term->ninputs == 1 && (inputs[0] == 9999 || inputs[0] == -9999)) {
            term->id = inputs[0];
        } else if (term->ninputs > 0) {
            term->id = s_edf_gate++;
            term->inputs = alloc_zeroed((size_t)term->ninputs, sizeof(int), "creating edif netlist");
            memcpy(term->inputs, inputs, (size_t)term->ninputs * sizeof(int));
        } else {
            continue;
        }
        /* append the term to the owner's list */
        edif_term_t **link = &s_edf_syms[owner].terms;
        while (*link != NULL) {
            link = &(*link)->next;
        }
        *link = term;
    }
    return count;
}

/* Nets "&N<n>" joined to ports 1, 2, ... of OR_GATE0.  They do not describe
 * the design; their number follows a fixed rule that keeps the generated
 * .edf files compatible: with size = length of the source file name
 * (without a leading "./") plus one and pad = (8 - nsyms * 13 % 8) % 8, the
 * count is size << (8 * pad) for pad 0 to 2, and zero otherwise. */
static void edif_write_stray_nets(void)
{
    unsigned pad = (8 - ((unsigned)g.nsyms * 13) % 8) % 8;
    const char *path = g.args[5];
    if (path[0] == '.') {
        path += strlen(path) >= 2 ? 2 : strlen(path);
    }
    uint32_t count = 0;
    if (pad < 3) {
        count = ((uint32_t)strlen(path) + 1) << (8 * pad);
    }
    int port = 1;
    for (uint32_t i = 0; i < count; i++) {
        out_printf(s_edf_fp, "     (Net &N%d\n", s_edf_net);
        out_printf(s_edf_fp, "       (Joined\n");
        out_printf(s_edf_fp, "         (PortRef &P%d (InstanceRef &OR_GATE%d))\n", port, 0);
        out_printf(s_edf_fp, "       )\n");
        out_printf(s_edf_fp, "     )\n");
        port++;
        s_edf_net++;
    }
}

/* Registers clocked or enabled by a pin */
static void edif_write_qualifiers(void)
{
    for (int i = 2; i < g.nsyms; i++) {
        const symbol_t *s = &g.syms[i];
        if ((s->type & 0x0f) != 2 || s->node_index >= 0) {
            continue;
        }
        unsigned kind = pin8(s->pin_index, PIN_KIND);
        if (kind == 5 || kind == 0x0c) {
            for (int k = 2; k < g.nsyms; k++) {
                const symbol_t *r = &g.syms[k];
                if (find_any_symbol(r->name, "ck") == NULL && is_register_ext(SYM_EXT(r))
                    && find_extension_pair(r, s->pin, 0x0f) != -1) {
                    out_printf(s_edf_fp, "\n      (qualify PIN%d CK)", (int)r->pin);
                }
            }
        }
        if (kind == 9 || kind == 0x0d) {
            for (int k = 2; k < g.nsyms; k++) {
                const symbol_t *r = &g.syms[k];
                if (find_any_symbol(r->name, "oe") == NULL && r->pin_index < 0 && r->node_index >= 0
                    && find_extension_pair(r, s->pin, 0x11) != -1) {
                    out_printf(s_edf_fp, "\n      (qualify PIN%d OE)", (int)r->pin);
                }
            }
        }
    }
}

/* contents: pin instances with their library cells, then the OR and AND
 * gates of every list */
static void edif_write_contents(void)
{
    FILE *fp = s_edf_fp;
    const symbol_t *enable = NULL;
    int total = (int)(attr8(ATTR_NPINS) + attr16(ATTR_NNODES));
    out_printf(fp, "    (contents\n");
    for (int i = 0; i < g.nsyms; i++) {
        const symbol_t *s = &g.syms[i];
        if (s->pin_index >= 0 && strcmp(SYM_EXT(s), "oe") != 0
            && (pin8(s->pin_index, PIN_KIND) == 9 || pin8(s->pin_index, PIN_KIND) == 0x0d)) {
            enable = s;
        }
    }
    for (int i = 2; i < g.nsyms; i++) {
        const symbol_t *s = &g.syms[i];
        const char *ext = SYM_EXT(s);
        edif_pin_t *p = edif_pin(s->pin);
        unsigned type = s->type & 0x0f;
        if (s->name[0] == '.') {
            continue;
        }
        if (strcmp(ext, "oe") == 0) {
            p->cell += 0x14;
        } else if (strcmp(ext, "io") == 0) {
            p->cell += 10;
        } else if (strcmp(ext, "dfb") == 0) {
            p->cell += 0x28;
        } else if (strcmp(ext, "int") == 0) {
            p->cell += 0x1e;
        } else if (strcmp(ext, "byp") == 0) {
            p->cell += 0x32;
        } else if (type == 2 || type == 9) {
            snprintf(p->name, sizeof(p->name), "%s", s->name);
            snprintf(p->instance, sizeof(p->instance), "PIN%d", (int)s->pin);
            if (s->node_index < 0) {
                p->cell = (s->flags & SYMF_INVERTED) != 0;
            } else {
                unsigned e = node8(s->node_index, NODE_EXT);
                unsigned kind = node8(s->node_index, NODE_KIND);
                if (type == 9) {
                    if (e == 0) {
                        p->cell = 600;
                    } else if (e == 1) {
                        p->cell = 400;
                    }
                } else if (e == 0) {
                    p->cell = 100;
                } else if (e == 1) {
                    p->cell = 200;
                } else if (e == 0x0b || e == 8) {
                    p->cell = 300;
                }
                if (kind == 1 || kind == 6) {
                    p->cell++;
                }
            }
            if (s->node_index >= 0 && enable != NULL && find_extension_pair(s, enable->pin, 0x11) != -1
                && s->pin <= attr8(ATTR_NPINS) && find_any_symbol(s->name, "oe") == NULL) {
                p->cell += 0x14;
            }
        }
    }
    /* AND terms and OR gate of every list */
    for (int i = 0; i < g.nsyms; i++) {
        const symbol_t *s = &g.syms[i];
        if (s->eq != NULL && s->name[0] != '.' && (s->type & 0x0f) != 3) {
            s_edf_syms[i].nterms = edif_build_terms(s->eq, i);
            s_edf_syms[i].gate = s_edf_gate++;
        }
    }
    for (int pin = 1; pin <= total; pin++) {
        edif_pin_t *p = &s_edf_pins[pin];
        int cell = -1;
        if (p->has_node) {
            p->cell += 5;
        }
        if (p->cell != -1) {
            cell = edif_find_cell(p->cell);
        }
        if (cell == -1) {
            if (p->cell != -1) {
                out_printf(fp, "     (Instance &PIN_%s \n", p->name);
                out_printf(fp, "       (ViewRef NetlistView\n");
                if (p->inverter == 0) {
                    out_printf(fp, "         (CellRef &PORTIN (LibraryRef COMPONENTS))\n");
                } else {
                    out_printf(fp, "         (CellRef &PORTLIN (LibraryRef COMPONENTS))\n");
                }
                out_printf(fp, "       )\n");
                out_printf(fp, "       (Property PartValue (String \"Input only pin\"))\n");
                out_printf(fp, "     )\n");
            }
        } else {
            out_printf(fp, "     (Instance &PIN_%s \n", p->name);
            out_printf(fp, "       (ViewRef NetlistView\n");
            out_printf(fp, "         (CellRef &%s (LibraryRef COMPONENTS))\n", k_edif_cells[cell].name);
            out_printf(fp, "       )\n");
            out_printf(fp, "       (Property PartValue (String \"%s\"))\n", k_edif_cells[cell].name);
            out_printf(fp, "     )\n");
        }
    }
    for (int i = 2; i < g.nsyms; i++) {
        const edif_sym_t *es = &s_edf_syms[i];
        if (es->nterms <= 1) {
            continue;
        }
        out_printf(fp, "     (Instance &OR_GATE%d\n", es->gate);
        out_printf(fp, "       (ViewRef NetlistView\n");
        out_printf(fp, "         (CellRef &OR%d (LibraryRef COMPONENTS))\n", es->nterms);
        out_printf(fp, "       )\n");
        out_printf(fp, "       (Property PartValue (String \"Or Gate\"))\n");
        out_printf(fp, "     )\n");
        /* AND gates feeding this OR gate */
        for (const edif_term_t *term = es->terms; term != NULL; term = term->next) {
            if (term->ninputs > 0 && term->id != 9999 && term->id != -9999) {
                out_printf(fp, "     (Instance &AND_GATE%d\n", term->id);
                out_printf(fp, "       (ViewRef NetlistView\n");
                out_printf(fp, "         (CellRef &AND%d (LibraryRef COMPONENTS))\n", term->ninputs);
                out_printf(fp, "       )\n");
                out_printf(fp, "       (Property PartValue (String \"And Gate\"))\n");
                out_printf(fp, "     )\n");
            }
        }
    }
    edif_write_stray_nets();
    edif_write_qualifiers();
    out_printf(fp, "    )\n");
}

/* EDIF netlist <design>.edf */
void write_edif(void)
{
    s_edf_name = make_path(source_dir(g.args[5]), g.args[5], ".edf");
    s_edf_fp = open_file(s_edf_name, "wt");
    if (s_edf_fp == NULL) {
        fatal_error(ERR_OPEN, s_edf_name);
    }
    FILE *fp = s_edf_fp;
    int device_pins = (int)(attr8(ATTR_NPINS) + attr16(ATTR_NNODES));
    s_edf_npins = device_pins + 1;
    for (int i = 0; i < g.nsyms; i++) {
        if (g.syms[i].pin >= s_edf_npins) {
            s_edf_npins = g.syms[i].pin + 1;
        }
    }
    s_edf_pins = alloc_zeroed((size_t)s_edf_npins + 1, sizeof(edif_pin_t), "creating edif netlist");
    s_edf_syms = alloc_zeroed((size_t)g.nsyms + 1, sizeof(edif_sym_t), "creating edif netlist");
    for (int i = 1; i <= device_pins; i++) {
        s_edf_pins[i].cell = -1;
    }
    char when[40];
    snprintf(when, sizeof(when), "%s", creation_time_text());
    if (when[0] != '\0') {
        when[strlen(when) - 1] = '\0';
    }
    out_printf(fp, "(EDIF %s\n", make_path("", g.args[5], ""));
    out_printf(fp, "  (EDIFVersion 2 0 0)\n");
    out_printf(fp, "  (EDIFLevel  0)\n");
    out_printf(fp, "  (keywordMap(keywordLevel 0))\n");
    out_printf(fp, "  (status\n");
    out_printf(fp, "   (Written\n");
    out_printf(fp, "    (TimeStamp %s)\n", when);
    out_printf(fp, "    (dataOrigin \"CUPL\")\n");
    out_printf(fp, "    (Comment \"%s\")\n", PROGRAM_COPYRIGHT);
    out_printf(fp, "   )\n");
    out_printf(fp, "  )\n");
    out_printf(fp, " (library SHEETS\n");
    out_printf(fp, "   (edifLevel 0)\n");
    out_printf(fp, "   (technology (numberDefinition))\n");
    out_printf(fp, "  (cell %s\n", make_path("", g.args[5], ""));
    out_printf(fp, "   (cellType generic)\n");
    out_printf(fp, "   (view netListView\n");
    out_printf(fp, "    (viewType netlist)\n");
    out_printf(fp, "    (interface\n");
    out_printf(fp, "     (port &VCC  (direction INPUT))\n");
    out_printf(fp, "     (port &GND  (direction INPUT))\n");
    for (int i = 2; i < g.nsyms; i++) {
        const symbol_t *s = &g.syms[i];
        if ((s->type & 0x0f) == 2) {
            out_printf(fp, "     (port &%s  (direction %s))\n", s->name, s->node_index < 0 ? "INPUT" : "OUTPUT");
        }
    }
    out_printf(fp, "    )\n");
    edif_write_contents();
    out_printf(fp, "   )\n");
    out_printf(fp, "  )\n");
    out_printf(fp, " )\n");
    out_printf(fp, ")\n");
    close_file(fp);
    s_edf_fp = NULL;
}

void net_reset(void)
{
    s_pds_fp = NULL;
    s_pds_name = NULL;
    s_pds_indent = 0;
    s_pds_user = 0;
    s_pins = NULL;
    s_npins = 0;
    memset(s_kind_count, 0, sizeof(s_kind_count));
    s_longest_in = s_longest_out = 0;
    s_left = s_right = s_org_x = s_org_y = s_width = s_height = s_base = 0;
    s_edf_fp = NULL;
    s_edf_name = NULL;
    s_edf_pins = NULL;
    s_edf_npins = 0;
    s_edf_syms = NULL;
    s_edf_gate = 1;
    s_edf_net = 1;
}
