/*
 * dev.c - device and package description of the ATF1502 / ATF1504 / ATF1508
 * fitter: pin limits and pin lists of the packages, the package drawing of
 * the report, and the UIM and global OE multiplexer connectivity.
 *
 * Every function looks the package up in the selected device description
 * (dev_tables.c); where the devices differ in more than tables the
 * difference is spelled out in the code.
 *
 * The device name for a package ("P1508C84") is kept in
 * FitPackageDesc.dev_name.  The DCERP helpers are in report.c, their only
 * caller.
 */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include "base.h"
#include "dev.h"
#include "dev_priv.h"
#include "place.h"

const FitDeviceDesc *g_dev_desc;
static FitDeviceParams g_dev_params;            /* live copy of the device's parameters */
const FitDeviceParams *g_dev = &g_dev_params;
char *g_dev_package;

/* The drawing buffer: 160 rows of 160 bytes for the ATF1508, 100 rows of
 * 100 bytes for the ATF1504 and ATF1502.  One byte more so that a row that
 * lost its terminator still ends. */
#define DEV_DRAW_MAX   160
#define DEV_DRAW_SCAN  100                      /* rows dev_draw_extent looks at */
static char g_draw[DEV_DRAW_MAX * DEV_DRAW_MAX + 1];

/* ------------------------------------------------------------------ */
/* Selection of the device                                             */
/* ------------------------------------------------------------------ */

void dev_select(enum FitDevice device)
{
    g_dev_desc = &g_dev_descs[device];
    g_dev_params = g_dev_desc->params;
    g_dev_package = NULL;
}

int dev_select_by_name(const char *name)
{
    static const struct { const char *digits; enum FitDevice device; } known[] = {
        { "1502", FIT_DEV_1502 }, { "1504", FIT_DEV_1504 }, { "1508", FIT_DEV_1508 }
    };
    const char *base;
    const char *p;
    size_t i;

    if (name == NULL)
        return 0;
    base = name;
    for (p = name; *p != '\0'; p++)
        if (*p == '/' || *p == '\\')
            base = p + 1;
    for (i = 0; i < sizeof known / sizeof known[0]; i++) {
        if (strstr(base, known[i].digits) != NULL) {
            dev_select(known[i].device);
            return 1;
        }
    }
    return 0;
}

/* Package of this device called `name` (ignoring case), or NULL. */
static const FitPackageDesc *dev_find_package(const char *name)
{
    int i;

    if (name == NULL)
        return NULL;
    for (i = 0; i < g_dev_desc->n_packages; i++)
        if (util_str_icmp(name, g_dev_desc->packages[i].name) == 0)
            return &g_dev_desc->packages[i];
    return NULL;
}

const FitPackageDesc *dev_current_package(void)
{
    return dev_find_package(g_dev_package);
}

int dev_parse_pin(FitList *params, const char *text, char **end)
{
    const FitPackageDesc *pkg = dev_current_package();
    char *option = param_get(params, "device");
    const char *start = text;
    long pin;

    if (option == NULL)
        option = param_get(params, "dev");
    if (option != NULL) {
        lincupl_strupr(option);
        if (strstr(option, "CBGA132") || strstr(option, "B132") ||
            strstr(option, "C132"))
            pkg = dev_find_package("CBGA132");
        free(option);
    }
    while (isspace((unsigned char)*text)) text++;
    if (pkg != NULL && pkg->balls != NULL && isalpha((unsigned char)*text)) {
        const char *limit = text;
        while (isalnum((unsigned char)*limit)) limit++;
        for (int i = 0; i < pkg->n_pins; i++) {
            const char *name = pkg->balls[i].name;
            if (strlen(name) == (size_t)(limit - text) &&
                strncasecmp(name, text, (size_t)(limit - text)) == 0) {
                *end = (char *)limit;
                return i + 1;
            }
        }
        *end = (char *)start;
        return 0;
    }
    if (*text == 'P' || *text == 'p') text++;
    errno = 0;
    pin = strtol(text, end, 10);
    if (errno || *end == text || pin < 1 || pin > 256)
        return 0;
    return (int)pin;
}

const char *dev_pin_name(int pin, char *buffer, size_t size)
{
    const FitPackageDesc *pkg = dev_current_package();
    if (pkg != NULL && pkg->balls != NULL && pin > 0 && pin <= pkg->n_pins)
        return pkg->balls[pin - 1].name;
    snprintf(buffer, size, "%d", pin);
    return buffer;
}

void dev_params_set_clock_mode(int word, int value)
{
    if (word == 0)
        g_dev_params.gclk0_mode = value;
    else if (word == 1)
        g_dev_params.gclk1_mode = value;
    else
        g_dev_params.gclk2_mode = value;
}

/* ------------------------------------------------------------------ */
/* Packages and pins                                                   */
/* ------------------------------------------------------------------ */

int dev_check_pin_count(FitEntity *ent)
{
    const FitPackageDesc *pkg;
    int n = 0;
    int limit;

    if (ent->net != NULL && ent->net->aliases != NULL)
        n = -ent->net->aliases->count;
    n += (ent->inputs != NULL ? ent->inputs->count : 0);
    n += (ent->outputs != NULL ? ent->outputs->count : 0);

    /* Pins usable on the package; without a known package 0x44 on the
     * ATF1508 and 0x24 on the ATF1504 and ATF1502. */
    pkg = dev_current_package();
    if (pkg != NULL)
        limit = pkg->io_limit;
    else
        limit = (g_dev_desc->id == FIT_DEV_1508) ? 0x44 : 0x24;
    if (g_opt.jtag != 0)
        limit -= 4;
    if (g_opt.pd1 != 0)
        limit -= 1;
    if (g_opt.pd2 != 0)
        limit -= 1;
    if (limit < n) {
        fprintf(g_fit_report, "# ERROR : Design has %d IOs.\n", n);
        fprintf(g_msg, "# ERROR : Design has %d IOs.\n", n);
        return 0;
    }
    /* The percentage is computed in double precision. */
    if ((double)n / (double)limit * 100.0 > 95.0) {
        fprintf(g_fit_report, "Info: %s uses 95%% of the pins available in device %s\n",
                g_opt.out_base, g_dev_package);
        fprintf(g_fit_report, "  If you wish to have more pins available for future logic changes\n");
        fprintf(g_fit_report, "  Atmel  recommends using a larger device\n");
    }
    return 1;
}

/* Result of dev_pin_of_macrocell (which 0) and dev_special_pin (which 1)
 * for a package that matched none: the comparison of the name with the
 * last package name the lookup tries on this device. */
static int dev_chain_miss(const char *package, int which)
{
    static const char *const last[3][2] = {
        /* dev_pin_of_macrocell, dev_special_pin */
        { "TQFP44",  "TQFP44"  },   /* ATF1502 */
        { "TQFP44",  "PQFP100" },   /* ATF1504 */
        { "PQFP160", "PQFP160" }    /* ATF1508 */
    };
    return util_str_icmp(package, last[g_dev_desc->id][which]);
}

int dev_pin_of_macrocell(const char *package, int index)
{
    const FitPackageDesc *pkg = dev_find_package(package);

    if (pkg == NULL)
        return dev_chain_miss(package, 0);
    if (index < 0 || index >= g_dev->n_macrocells + g_dev->n_ded_inputs)
        return 0;       /* index outside the table */
    return pkg->rows[index].pin;
}

int dev_special_pin(int kind, int index)
{
    const FitPackageDesc *pkg = dev_current_package();

    if (pkg != NULL) {
        if (kind == 8 || kind == 9 || kind == 10) {
            if (index < 0 || index >= 3)
                return 0;   /* index outside the table */
            return pkg->gclk_pins[index];
        }
        if (kind == 4)
            return pkg->gclr_pin;
        if (kind == 0xb || (kind == 0xc && g_dev_desc->id == FIT_DEV_1502)) {
            if (index < 0 || index >= pkg->n_oe_pins)
                return 0;
            return pkg->oe_pins[index];
        }
    }
    /* No package or another kind: the chain runs to its end and the result
     * of the last comparison is returned. */
    return dev_chain_miss(g_dev_package, 1);
}

static FitList *dev_pin_list(const int *pins, int n)
{
    FitList *l = list_create();
    const FitPackageDesc *pkg = dev_current_package();
    int i;

    if (pkg != NULL)
        for (i = 0; i < n; i++)
            list_add(l, FIT_INT2PTR(pins[i]));
    return l;
}

FitList *dev_gclk_pin_list(void)
{
    const FitPackageDesc *pkg = dev_current_package();
    return dev_pin_list(pkg != NULL ? pkg->gclk_pins : NULL, 3);
}

FitList *dev_gclr_pin_list(void)
{
    const FitPackageDesc *pkg = dev_current_package();
    return dev_pin_list(pkg != NULL ? &pkg->gclr_pin : NULL, 1);
}

FitList *dev_oe_pin_list(void)
{
    const FitPackageDesc *pkg = dev_current_package();
    return dev_pin_list(pkg != NULL ? pkg->oe_pins : NULL, pkg != NULL ? pkg->n_oe_pins : 0);
}

FitList *dev_jtag_pin_list(void)
{
    const FitPackageDesc *pkg = dev_current_package();
    return dev_pin_list(pkg != NULL ? pkg->jtag_pins : NULL, 4);
}

FitList *dev_pd_pin_list(void)
{
    const FitPackageDesc *pkg = dev_current_package();
    return dev_pin_list(pkg != NULL ? pkg->pd_pins : NULL, 2);
}

/* marks the first package slot on each of the pins as having no
 * free pin. */
static void dev_mark_pins_used(FitList *slots, FitList *pins)
{
    int i, j;

    for (i = 0; i < (pins != NULL ? pins->count : 0); i++) {
        for (j = 0; j < (slots != NULL ? slots->count : 0); j++) {
            FitPkgSlot *s = slots->items[j];
            if (s->pin == FIT_PTR2INT(pins->items[i])) {
                s->no_pin = 1;
                break;
            }
        }
    }
}

FitList *dev_select_package(void)
{
    FitList *slots = list_create();
    const FitPackageDesc *pkg;
    int n_rows = g_dev->n_macrocells + g_dev->n_ded_inputs;
    int i;

    if (g_dev_package == NULL || (FIT_EXT && dev_current_package() == NULL)) {
        /* the extended mode says the same for a wrong name, and gives BE devices a
         * TQFP package */
        const char *def = g_dev_desc->default_package->name;

        if (FIT_EXT && g_opt.be != 0)
            def = strcmp(g_family, "1508") == 0 ? "TQFP100" : "TQFP44";
        fprintf(g_msg, "Warning: device not specified, defaulting to  %s\n", def);
        g_dev_package = util_strdup(def);   /* a wrong name is not freed */
    } else if (dev_current_package() == NULL) {
        fprintf(g_msg, "Warning: wrong device name, defaulting to  %s\n", g_dev_desc->default_package->name);
        g_dev_package = util_strdup(g_dev_desc->default_package->name);   /* the old name is not freed */
    }
    pkg = dev_current_package();
    lincupl_strupr(g_dev_package);
    for (i = 0; i < n_rows; i++) {
        FitPkgSlot *s = util_calloc(1, sizeof *s);
        s->block = pkg->rows[i].block;
        s->macrocell = pkg->rows[i].macrocell;
        s->pin = pkg->rows[i].pin;
        s->no_pin = (s->pin == 0) ? 1 : 0;
        s->i0c = 0;
        s->last_of_8 = ((i + 1) % 8 == 0) ? 1 : 0;
        s->first_of_8 = (i % 8 == 0) ? 1 : 0;
        list_add(slots, s);
    }
    if (g_opt.jtag != 0) {
        FitList *pins = dev_jtag_pin_list();
        dev_mark_pins_used(slots, pins);
        list_destroy(&pins);
    }
    return slots;
}

char *dev_family_string(void)
{
    return util_strdup(g_dev->family);
}

/* ------------------------------------------------------------------ */
/* Package drawing                                                     */
/* ------------------------------------------------------------------ */

/* Address of a character of the drawing buffer, NULL when it lies
 * outside. */
static char *dev_draw_at(const FitPackageDesc *pkg, long row, long col)
{
    long off = row * pkg->drawing_stride + col;

    if (off < 0 || off >= (long)pkg->drawing_stride * pkg->drawing_stride)
        return NULL;
    return &g_draw[off];
}

static void dev_draw_put(const FitPackageDesc *pkg, long row, long col, char c)
{
    char *p = dev_draw_at(pkg, row, col);
    if (p != NULL)
        *p = c;
}

/* Copies the lines of the package picture into the drawing buffer and
 * ends a number of rows at a fixed column.  The picture and the numbers
 * are in the package table. */
static void dev_draw_package(const FitPackageDesc *pkg)
{
    int i;

    memset(g_draw, 0, sizeof g_draw);
    for (i = 0; i < pkg->n_drawing_lines; i++)
        strcpy(&g_draw[pkg->drawing_row[i] * pkg->drawing_stride], pkg->drawing[i]);
    for (i = 0; i < pkg->drawing_cut_rows; i++)
        g_draw[i * pkg->drawing_stride + pkg->drawing_cut] = '\0';
}

/* Rows at which the picture starts and ends: for each run of rows holding
 * something other than blanks, the first row minus 4 and the row after the
 * last. */
static FitList *dev_draw_extent(const FitPackageDesc *pkg)
{
    FitList *l = list_create();
    int inside = 0;
    int row;

    for (row = 0; row < DEV_DRAW_SCAN; row++) {
        const char *p = &g_draw[row * pkg->drawing_stride];
        while (*p == ' ')
            p++;
        if (*p != '\0') {
            if (!inside) {
                list_add(l, FIT_INT2PTR(row - 4));
                inside = 1;
            }
        } else if (inside) {
            list_add(l, FIT_INT2PTR(row));
            inside = 0;
        }
    }
    return l;
}

/* Draws the package with the signal names of the slots written next to
 * their pins. */
void dev_print_pinout(FILE *f, FitList *slots)
{
    const FitPackageDesc *pkg = dev_current_package();
    const FitPkgFrame *fr;
    FitList *extent;
    int first, end, row;
    int i;

    if (pkg == NULL)
        return;     /* no package: nothing is drawn */
    if (pkg->balls != NULL) {
        dev_print_pinout_ext(f, slots);
        return;
    }
    fr = &pkg->frame;
    dev_draw_package(pkg);
    for (i = 0; i < (slots != NULL ? slots->count : 0); i++) {
        FitSlot *slot = slots->items[i];
        const char *name = slot->signal;
        const FitPkgLabel *lab;
        unsigned len;
        long line;

        if (strcmp(name, FIT_FREE_NAME) == 0)
            continue;
        if (slot->pin < 1 || slot->pin > pkg->n_pins)
            continue;   /* no label for this pin */
        lab = &pkg->labels[slot->pin - 1];
        line = lab->line;
        len = (unsigned)strlen(name);
        if (lab->side == 0) {
            /* right of the package: left aligned at right_col */
            int c;
            unsigned u;
            for (c = fr->right_col; c < fr->right_width + fr->right_col; c++)
                dev_draw_put(pkg, line, c, ' ');
            for (u = (unsigned)fr->right_col; u < len + (unsigned)fr->right_col; u++)
                dev_draw_put(pkg, line, (long)u, name[u - (unsigned)fr->right_col]);
        } else if (lab->side == 1) {
            /* left of the package: right aligned, ending before left_end */
            int c;
            unsigned u;
            for (c = fr->left_col; c < fr->left_end; c++)
                dev_draw_put(pkg, line, c, ' ');
            for (u = (unsigned)fr->left_col; u < len; u++)
                dev_draw_put(pkg, line, (long)u + fr->left_end - (long)len, name[u]);
        } else if (lab->side == 3) {
            /* above the package: written downwards, ending in row top_end;
             * a name longer than top_end rows is not drawn at all (the
             * comparison is unsigned) */
            unsigned top_end = (unsigned)fr->top_end;
            int r;
            for (r = fr->top_row; r <= fr->top_end; r++)
                dev_draw_put(pkg, r, line, ' ');
            if (top_end - len < top_end) {
                unsigned rr = top_end;
                unsigned idx = len - 1;
                while ((int)rr >= 0) {
                    dev_draw_put(pkg, (long)rr, line, name[idx]);
                    idx--;
                    rr--;
                    if (rr <= top_end - len)
                        break;
                }
            }
        } else if (lab->side == 2) {
            /* below the package: written downwards from bottom_row */
            unsigned b = (unsigned)fr->bottom_row;
            int r;
            for (r = fr->bottom_row; r < fr->bottom_height + fr->bottom_row; r++)
                dev_draw_put(pkg, r, line, ' ');
            if (b < len + b) {
                unsigned rr = b;
                for (;;) {
                    dev_draw_put(pkg, (long)rr, line, name[rr - b]);
                    if (len + b <= rr + 1)
                        break;
                    rr++;
                }
            }
        }
    }
    extent = dev_draw_extent(pkg);
    /* An empty buffer prints nothing. */
    first = extent->count > 0 ? FIT_PTR2INT(extent->items[0]) : 0;
    end = extent->count > 1 ? FIT_PTR2INT(extent->items[1]) : (extent->count > 0 ? DEV_DRAW_SCAN : 0);
    g_draw[sizeof g_draw - 1] = '\0';
    for (row = first; row < end; row++)
        if (row >= 0)
            fprintf(f, "%s\n", &g_draw[row * pkg->drawing_stride]);
    fprintf(f, "\n\n");
    list_destroy(&extent);
}

/* ------------------------------------------------------------------ */
/* UIM connectivity                                                    */
/* ------------------------------------------------------------------ */

static const char *dev_uim_source(int mux, int k)
{
    return g_dev_desc->uim_sources[mux * g_dev->uim_width + k];
}

static const char *dev_oe_source(int mux, int k)
{
    return g_dev_desc->oe_sources[mux * g_dev->uim_width + k];
}

int dev_uim_selects(const FitUimSlot *u, const char *node, int k)
{
    if (u->index < 0 || u->index >= FIT_UIM_PER_BLOCK || k < 0 || k >= g_dev->uim_width)
        return 0;       /* index outside the table */
    return util_str_icmp(node, dev_uim_source(u->index, k)) == 0;
}

int dev_uim_input_of(const FitUimSlot *u)
{
    int k;

    if (u->index < 0 || u->index >= FIT_UIM_PER_BLOCK)
        return -1;
    for (k = 0; k < g_dev->uim_width; k++)
        if (util_str_icmp(u->sel_node, dev_uim_source(u->index, k)) == 0)
            return k;
    return -1;
}

int dev_oe_input_of(const FitUimSlot *u)
{
    int k;

    if (u->index < 0 || u->index >= FIT_GLOBAL_SLOTS)
        return -1;
    for (k = 0; k < g_dev->uim_width; k++)
        if (util_str_icmp(u->sel_node, dev_oe_source(u->index, k)) == 0)
            return k;
    return -1;
}

/* 1-based position of the slot's pin among the global OE capable pins, 0
 * when it is none of them. */
static int dev_oe_pin_position(const FitSlot *slot)
{
    FitList *pins = dev_oe_pin_list();
    int pos = 0;
    int i;

    for (i = 0; i < pins->count; i++) {
        if (slot->pin == FIT_PTR2INT(pins->items[i])) {
            pos = i + 1;
            break;
        }
    }
    list_destroy(&pins);
    return pos;
}

/* Block letter of the global OE tables: A..H, A..D or A, B depending on
 * the device; "X" for any other block number. */
static const char *dev_block_letter(int block)
{
    static const char *const letters[8] = { "A", "B", "C", "D", "E", "F", "G", "H" };

    if (block >= 1 && block <= g_dev->n_blocks)
        return letters[block - 1];
    return "X";
}

/* Node name of a slot's pin as the global OE tables spell it: "OE1" /
 * "OE2" for the first two OE capable pins, else <block letter><macrocell>"p".
 * New string. */
static char *dev_oe_node_name(const FitSlot *slot)
{
    int pos = dev_oe_pin_position(slot);
    char *name;

    if (pos == 1)
        return util_strdup("OE1");
    if (pos == 2)
        return util_strdup("OE2");
    name = util_str_with_index(dev_block_letter(slot->block), slot->macrocell);
    util_str_append(&name, "p");
    return name;
}

int dev_assign_global_oe(FitSlot *slot)
{
    char *node = dev_oe_node_name(slot);
    int mux, k;

    for (mux = 0; mux < FIT_GLOBAL_SLOTS; mux++) {
        FitUimSlot *g = g_jed_global_slots->items[mux];
        if (util_str_icmp(g->signal, FIT_FREE_NAME) == 0 || g->signal == NULL) {
            for (k = 0; k < g_dev->uim_width; k++) {
                if (util_str_icmp(node, dev_oe_source(mux, k)) == 0
                    && util_str_icmp(g->signal, FIT_FREE_NAME) == 0) {
                    g->signal = util_strdup(slot->signal);  /* the old strings are not freed */
                    g->sel_node = util_strdup(node);
                    g->index = mux;
                    free(node);
                    return 1;
                }
            }
        }
    }
    free(node);
    return 0;
}

int dev_find_global_oe(FitSlot *slot, int assign, int which)
{
    char node[32];      /* "H128fb" needs 7 bytes */
    int mux, k;

    snprintf(node, sizeof node, "%s%d%s", place_block_letter(slot), slot->macrocell,
             which == 1 ? "p" : "fb");
    for (mux = 0; mux < FIT_GLOBAL_SLOTS; mux++) {
        FitUimSlot *g = g_jed_global_slots->items[mux];
        if (util_str_icmp(g->signal, FIT_FREE_NAME) == 0 || g->signal == NULL) {
            for (k = 0; k < g_dev->uim_width; k++) {
                if (util_str_icmp(node, dev_oe_source(mux, k)) == 0) {
                    if (assign != 0) {
                        /* the extended mode takes the buried signal of a slot without
                         * an output signal */
                        int pin = which == 1
                                  && (!FIT_EXT || util_str_icmp(slot->signal, FIT_FREE_NAME) != 0);

                        g->signal = util_strdup(pin ? slot->signal : slot->buried);
                        g->sel_node = util_strdup(node);
                        g->index = mux;
                    }
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* util_str_ieq as a list comparison. */
static int dev_name_eq(void *item, void *key)
{
    return util_str_ieq(item, key);
}

/* Adds a copy of every source name of multiplexer row `mux` to `l` unless it
 * is there already. */
static void dev_collect_sources(FitList *l, int mux)
{
    int k;

    for (k = 0; k < g_dev->uim_width; k++) {
        const char *name = dev_uim_source(mux, k);
        if (list_find(l, (void *)name, dev_name_eq) == 0)
            list_add(l, util_strdup(name));
    }
}

/* The grouping of the forty multiplexers differs:
 *   ATF1508: even rows / odd rows                          -> 2 lists
 *   ATF1504: rows 0..19 even / odd, rows 20..39 even / odd -> 4 lists
 *   ATF1502: rows 0..9, 10..19, 20..29, 30..39             -> 4 lists */
FitList *dev_uim_node_lists(void)
{
    FitList *result = list_create();
    int mux;

    if (g_dev_desc->id == FIT_DEV_1502) {
        int group;
        for (group = 0; group < 4; group++) {
            FitList *l = list_create();
            for (mux = group * 10; mux < group * 10 + 10; mux++)
                dev_collect_sources(l, mux);
            list_add(result, l);
        }
    } else {
        int half = (g_dev_desc->id == FIT_DEV_1504) ? 20 : FIT_UIM_PER_BLOCK;
        int start;
        for (start = 0; start < FIT_UIM_PER_BLOCK; start += half) {
            FitList *even = list_create();
            FitList *odd = list_create();
            for (mux = start; mux < start + half; mux++)
                dev_collect_sources((mux % 2 == 0) ? even : odd, mux);
            list_add(result, even);
            list_add(result, odd);
        }
    }
    return result;
}
