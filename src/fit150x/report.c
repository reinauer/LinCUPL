/*
 * report.c - the .fit report of the ATF1502 / ATF1504 / ATF1508 fitter:
 * fitting strategy, pin assignments, placement, package drawing, UIM
 * fan-in, macrocell table and resource usage.  The code is the same for the
 * three devices; device dependence comes through g_dev and the package.
 *
 * The report is written with LF line ends.
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "banner.h"
#include "base.h"
#include "dev.h"
#include "dev_priv.h"
#include "fit.h"
#include "min.h"
#include "place.h"
#include "syn.h"
#include "tt.h"

FILE *g_fit_report;
int g_report_fits = 1;

static int report_count(const FitList *l)
{
    return l != NULL ? l->count : 0;
}

/* util_str_ieq as a list comparison. */
static int report_name_eq(void *item, void *key)
{
    return util_str_ieq(item, key);
}

/* ------------------------------------------------------------------ */
/* Signal names as the report shows them                               */
/* ------------------------------------------------------------------ */

/* the first cell of the netlist one of whose signal names is
 * `name`. */
static FitInstance *report_find_cell(FitEntity *ent, const char *name)
{
    int i;

    if (ent == NULL || ent->net == NULL || ent->net->items == NULL)
        return NULL;
    for (i = 0; i < ent->net->items->count; i++) {
        FitNetItem *it = ent->net->items->items[i];
        FitInstance *c = it->cell;
        if (it->kind == FIT_GATE_UNKNOWN || it->kind == FIT_GATE_CELL13)
            continue;   /* other payloads are not cells */
        if (util_str_ieq(name, c->q) || util_str_ieq(name, c->q2) || util_str_ieq(name, c->clk)
            || util_str_ieq(name, c->ar) || util_str_ieq(name, c->ap) || util_str_ieq(name, c->ce)
            || util_str_ieq(name, c->oe) || util_str_ieq(name, c->q3) || util_str_ieq(name, c->mux_a)
            || util_str_ieq(name, c->mux_b) || util_str_ieq(name, c->mux_sel) || util_str_ieq(name, c->d)
            || util_str_ieq(name, c->pin))
            return c;
    }
    return NULL;
}

/* name of the pin of cell `c` that `name` is connected to:
 * <cell>.D / .T / .L / .TH for the output, .C / .LH for the clock, .AR,
 * .AP, .OE, .CE; the name itself for the data inputs.
 *
 * The text is built in one static buffer, so it is valid only until the
 * next call.  The callers print it at once, except the fan-in list of
 * report_write_fanin, which keeps the pointers and prints them after all
 * names have been formatted: every such entry then shows the text of the
 * last one (e.g. "SENSOR_TRIG.AR,SENSOR_TRIG.AR," for the inputs
 * ASYNCH_TRIG.AR and SENSOR_TRIG.AR).  This is deliberate: the fan-in
 * lines of the report are expected to look like that. */
static const char *report_cell_pin_name(FitEntity *ent, FitInstance *c, const char *name)
{
    static char out[2 * FIT_NAME_MAX + 8];
    const char *ext;
    int kind = tt3_gate_kind_of_output(ent, c->q);

    if (util_str_icmp(c->q, name) == 0) {
        if (kind == FIT_GATE_TFF)
            ext = ".T";
        else if (kind == FIT_GATE_LATCH)
            ext = ".L";
        else if (kind == FIT_GATE_TH || kind == FIT_GATE_ITH)
            ext = ".TH";
        else
            ext = ".D";
    } else if (util_str_icmp(c->clk, name) == 0) {
        ext = (kind == FIT_GATE_LATCH) ? ".LH" : ".C";
    } else if (util_str_icmp(c->ar, name) == 0) {
        ext = ".AR";
    } else if (util_str_icmp(c->ap, name) == 0) {
        ext = ".AP";
    } else if (util_str_icmp(c->oe, name) == 0) {
        ext = ".OE";
    } else if (util_str_icmp(c->ce, name) == 0) {
        ext = ".CE";
    } else {
        /* data inputs, pad and any other pin: the name itself */
        return name;
    }
    if (FIT_EXT) {
        /* the extended mode names the pin after the signal the output stands for */
        const char *base = latch_signal_source(ent, c->q);

        snprintf(out, sizeof out, "%.*s%s", FIT_NAME_MAX - 1, base != NULL ? base : "", ext);
        return out;
    }
    snprintf(out, sizeof out, "%.*s%s", FIT_NAME_MAX - 1, c->inst_name != NULL ? c->inst_name : "", ext);
    return out;
}

/* an alias of the netlist is shown as the cell pin it names. */
static const char *report_display_name(const char *name, FitEntity *ent)
{
    if (latch_find_alias((char *)name, ent->net) != 0) {
        FitInstance *c = report_find_cell(ent, name);
        if (c != NULL)
            return report_cell_pin_name(ent, c, name);
    }
    return name;
}

/* Extended mode: a foldback node that is placed in more than one block is named
 * "FB_<block>_<name>" in every block after the first */
typedef struct ReportFbName {
    int block;
    char *name;
    char *fb_name;
} ReportFbName;

static FitList *g_report_fb_seen;       /* foldback names placed so far */
static FitList *g_report_fb_names;      /* ReportFbName */

static FitSlot *report_slot_of_row(FitList *slots, const FitPkgSlot *row);

static void report_collect_fb_names(FitList *slots, FitList *rows)
{
    int i;

    for (i = 1; i <= report_count(rows); i++) {
        FitSlot *s = report_slot_of_row(slots, rows->items[i - 1]);

        if (util_str_icmp(s->foldback, FIT_FREE_NAME) == 0)
            continue;
        if (g_report_fb_seen == NULL)
            g_report_fb_seen = list_create();
        if (list_find(g_report_fb_seen, s->foldback, report_name_eq) != 0) {
            ReportFbName *fb = util_calloc(1, sizeof *fb);

            if (g_report_fb_names == NULL)
                g_report_fb_names = list_create();
            fb->block = s->block;
            fb->name = util_strdup(s->foldback);
            fb->fb_name = util_str_with_index("FB_", s->block);
            util_str_append(&fb->fb_name, "_");
            util_str_append(&fb->fb_name, s->foldback);
            list_add(g_report_fb_names, fb);
        } else {
            list_add(g_report_fb_seen, s->foldback);
        }
    }
}

/* the name of a signal in the feedback and foldback columns of block
 * `block` */
static const char *report_fb_display_name(const char *name, FitEntity *ent, int block)
{
    int i;

    if (FIT_EXT
        && list_find(g_fit_foldback_ids, FIT_INT2PTR(entity_var_id_by_name(ent, name)), list_ptr_eq) != 0) {
        for (i = 0; i < report_count(g_report_fb_names); i++) {
            ReportFbName *fb = g_report_fb_names->items[i];

            if (util_str_icmp(fb->name, name) == 0 && fb->block == block)
                return fb->fb_name;
        }
        return name;
    }
    return report_display_name(name, ent);
}

/* ------------------------------------------------------------------ */
/* Strategy section                                                    */
/* ------------------------------------------------------------------ */

static void report_print_names(FILE *f, FitList *names)
{
    int i;

    for (i = 0; i < report_count(names); i++)
        fprintf(f, " %s", (char *)names->items[i]);
}

static void report_file_line(FILE *f, const char *fmt, const char *base, const char *ext)
{
    char *name = util_strdup(base);

    util_str_append(&name, ext);
    fprintf(f, fmt, name);
    free(name);
}

static void report_flag_list(FILE *f, const char *label, int flag, FitList *names, const char *item_fmt)
{
    int i;

    if (flag == 0) {
        fprintf(f, " %s = OFF\n", label);
    } else if (names == NULL) {
        fprintf(f, " %s = ON\n", label);
    } else {
        fprintf(f, " %s = ", label);
        for (i = 0; i < names->count; i++)
            fprintf(f, item_fmt, (char *)names->items[i]);
        fprintf(f, "\n");
    }
}

/* the first four characters of the package name ("PLCC"). */
static char *report_package_type(void)
{
    char *s = util_calloc(1, 5);

    if (g_dev_package != NULL)
        strncat(s, g_dev_package, 4);
    return s;
}

/* " label = a b c \n", or " label = off\n" for an empty list */
static void report_name_list(FILE *f, const char *label, FitList *names)
{
    int i;

    if (report_count(names) == 0) {
        fprintf(f, " %s = off\n", label);
        return;
    }
    fprintf(f, " %s = ", label);
    for (i = 0; i < names->count; i++)
        fprintf(f, "%s ", (char *)names->items[i]);
    fprintf(f, "\n");
}

static const char *report_on_off(int flag)
{
    return flag != 0 ? "on\n" : "off\n";
}

/* " voltage_level_X = 3.3" and so on */
static void report_voltage_level(FILE *f, const char *label, int level)
{
    static const char *const volts[4] = { "1.5", "1.8", "2.5", "3.3" };

    fprintf(f, " %s = %s\n", label, volts[level >= 1 && level <= 3 ? level : 0]);
}

/* Extended mode: the files, the technology and the options of the BE devices */
static void report_write_strategy_ext(const char *out_base, FILE *f)
{
    static const char *const tech[3] = { "AS", "ASV", "BE" };
    char *name;
    int i;

    fprintf(f, "\n****** Initial fitting strategy and property ******");
    fprintf(f, "\n Netlist_in_file = %s\n", g_in_name);
    report_file_line(f, " Netlist_out_file = %s\n", out_base, g_opt.out_edif != 0 ? ".edn" : ".tt3");
    report_file_line(f, " Jedec_file = %s\n", out_base, ".jed");
    if (g_opt.ifmt_edif == 0) {
        report_file_line(f, " Vector_file = %s\n", out_base, ".tmv");
        report_file_line(f, " verilog_file = %s\n", out_base, ".vt");
    }
    /* with both simulation formats the Verilog file is named ".VO" alone */
    name = util_strdup(out_base);
    if (g_opt.vhdl_sim != 0) {
        util_str_append(&name, ".VHO");
        fprintf(f, " Time_file = %s\n", name);
        free(name);
        report_file_line(f, " Time_file = %s\n", out_base, ".SDO");
        name = NULL;
    }
    if (g_opt.verilog_sim != 0) {
        util_str_append(&name, ".VO");
        fprintf(f, " Time_file = %s\n", name);
        free(name);
        report_file_line(f, " Time_file = %s\n", out_base, ".SDO");
        name = NULL;
    }
    free(name);
    report_file_line(f, " Log_file = %s\n", out_base, ".fit");
    fprintf(f, " Device_name = %s\n", g_dev_package);
    fprintf(f, " Tech_name = ATF%s%s \n", g_family, tech[g_opt.be != 0 ? 2 : g_opt.asv != 0]);
    name = report_package_type();
    fprintf(f, " Package_type = %s\n", name);
    free(name);
    if (g_opt.pin_filename != NULL)
        fprintf(f, " Preassign_file = %s\n", g_opt.pin_filename);
    if (g_opt.preassign == FIT_PREASSIGN_KEEP)
        fprintf(f, " Preassignment = keep \n");
    else if (g_opt.preassign == FIT_PREASSIGN_TRY)
        fprintf(f, " Preassignment = try \n");
    else
        fprintf(f, " Preassignment = ignore \n");
    fprintf(f, g_opt.security != 0 ? " Security_mode = ON\n" : " Security_mode = OFF\n");
    report_flag_list(f, "Pin-Keeper", g_opt.pin_keep, g_opt.pin_keep_list, "%s ");
    fprintf(f, g_opt.cupl != 0 ? " supporter = CUPL\n" : " supporter = ABEL\n");
    fprintf(f, g_opt.optimize != 0 ? " optimize = ON\n" : " optimize = OFF\n");
    if (g_opt.soft_buffer != NULL) {
        fprintf(f, " Soft_buffer = \n");
        for (i = 0; i < g_opt.soft_buffer->count; i++)
            fprintf(f, "%s ", (char *)g_opt.soft_buffer->items[i]);
        fprintf(f, "\n");
    }
    if (g_opt.global_fold != NULL) {
        fprintf(f, " Global_Fold = \n");
        for (i = 0; i < g_opt.global_fold->count; i++)
            fprintf(f, "%s ", (char *)g_opt.global_fold->items[i]);
        fprintf(f, "\n");
    }
    if (g_opt.xor_synthesis == 0) {
        fprintf(f, " Xor_synthesis = OFF\n");
    } else {
        fprintf(f, " Xor_synthesis = ");
        if (report_count(g_opt.xor_list) == 0)
            fprintf(f, " ON");
        else
            report_print_names(f, g_opt.xor_list);
        fprintf(f, "\n");
    }
    if (g_opt.foldback == 0) {
        fprintf(f, " Foldback_logic = OFF\n");
    } else {
        fprintf(f, " Foldback_logic = ");
        if (report_count(g_opt.foldback_list) == 0)
            fprintf(f, " on");
        else
            report_print_names(f, g_opt.foldback_list);
        fprintf(f, "\n");
    }
    if (g_opt.cascade_logic == 0) {
        fprintf(f, " Cascade_logic = OFF\n");
    } else {
        /* the test looks at the foldback list, the names come from the
         * cascade list */
        fprintf(f, " Cascade_logic = ");
        if (report_count(g_opt.foldback_list) == 0)
            fprintf(f, " on");
        else
            report_print_names(f, g_opt.cascade_list);
        fprintf(f, "\n");
    }
    report_flag_list(f, "Output_fast", g_opt.output_fast, g_opt.output_fast_list, " %s ");
    if (report_count(g_opt.twoclock) != 0)
        fprintf(f, " twoclock = %s \n", (char *)g_opt.twoclock->items[0]);
    if (g_opt.be != 0)
        report_name_list(f, "Schmitt_trigger", g_opt.schmitt_trigger != 0 ? g_opt.schmitt_list : NULL);
    if (util_str_icmp(g_family, "1504") == 0 || util_str_icmp(g_family, "1508") == 0) {
        if (g_opt.sstl_input == 0)
            fprintf(f, " SSTL_input = off\n");
        else if (report_count(g_opt.sstl_list) == 0)
            fprintf(f, " SSTL_input = on\n");
        else
            report_name_list(f, "SSTL_input", g_opt.sstl_list);
    }
    if (g_opt.be != 0) {
        report_name_list(f, "Pull_up", g_opt.pull_up_list);
        report_name_list(f, "Pull_down", g_opt.pull_down_list);
    }
    fprintf(f, " *******************************\n");
    fprintf(f, g_opt.pd1 != 0 ? " Power down pin 1 = ON\n" : " Power down pin 1 = OFF\n");
    fprintf(f, g_opt.pd2 != 0 ? " Power down pin 2 = ON\n" : " Power down pin 2 = OFF\n");
    if (g_opt.be == 0)
        fprintf(f, g_opt.power_reset != 0 ? " power_reset = ON\n" : " power_reset = OFF\n");
    fprintf(f, g_opt.jtag != 0 ? " JTAG = ON\n" : " JTAG = OFF\n");
    fprintf(f, g_opt.tdi_pullup != 0 ? " TDI pullup = ON\n" : " TDI pullup = OFF\n");
    fprintf(f, g_opt.tms_pullup != 0 ? " TMS pullup = ON\n" : " TMS pullup = OFF\n");
    if (g_opt.be == 0)
        report_flag_list(f, "MC_power", g_opt.mc_power, g_opt.mc_power_list, " %s, ");
    report_flag_list(f, "Open_collector", g_opt.open_collector, g_opt.open_collector_list, " %s, ");
    if (g_opt.be == 0) {
        fprintf(f, g_opt.gck_itd[0] != 0 ? " ITD0 = ON\n" : " ITD0 = OFF\n");
        fprintf(f, g_opt.gck_itd[1] != 0 ? " ITD1 = ON\n" : " ITD1 = OFF\n");
        fprintf(f, g_opt.gck_itd[2] != 0 ? " ITD2 = ON\n" : " ITD2 = OFF\n");
    }
    if (g_opt.fast_inlatch == 0)
        fprintf(f, " Fast_inlatch = off\n");
    else if (report_count(g_opt.fast_inlatch_list) == 0)
        fprintf(f, " Fast_inlatch = on\n");
    else
        report_name_list(f, "Fast_inlatch", g_opt.fast_inlatch_list);
    if (g_opt.ues != NULL)
        fprintf(f, " UES = %s\n", g_opt.ues);
    if (g_opt.tpd_text != NULL) {
        /* the speed grade codes of main_device_options_ext */
        static const char *const tpd[10] = { "5", "7", "5", "7", "10", "15", "20", "25", "15", "20" };

        fprintf(f, " TPD = %s\n", g_opt.tpd >= 0 && g_opt.tpd <= 9 ? tpd[g_opt.tpd] : "5");
    }
    fprintf(f, " Latch_synthesis = %s \n", g_opt.latch_synthesis != 0 ? "on" : "off");
    fprintf(f, " Push_gate = %s \n", g_opt.push_gate != 0 ? "on" : "off");
    fprintf(f, " Verilog_sim = %s \n", g_opt.verilog_sim != 0 ? "on" : "off");
    fprintf(f, " VHDL_sim = %s \n", g_opt.vhdl_sim != 0 ? "on" : "off");
    fprintf(f, " Out_Edif = %s \n", g_opt.out_edif != 0 ? "on" : "off");
    fprintf(f, " Logic Doubling = %s \n", g_opt.logic_doubling != 0 ? "on" : "off");
    if (g_opt.be != 0) {
        fprintf(f, " Unused_To_Pinkeeper = %s", report_on_off(g_opt.unused_to_pinkeeper));
        fprintf(f, " pull_up_Unused = %s", report_on_off(g_opt.pull_up_unused));
        fprintf(f, " unused_To_Ground = %s", report_on_off(g_opt.unused_to_ground));
        if (util_str_icmp(g_family, "1504") == 0)
            fprintf(f, g_opt.tff_ok == 0 ? " no_tff = on\n" : " no_tff = off\n");
        if (util_str_icmp(g_family, "1502") == 0) {
            report_voltage_level(f, "voltage_level_A", g_opt.voltage_a);
            report_voltage_level(f, "voltage_level_B", g_opt.voltage_b);
        }
    }
    fprintf(f, " ****** End of fitting strategy and property ******\n");
}

void report_write_strategy(const char *out_base, FILE *f)
{
    char *type;

    if (FIT_EXT) {
        report_write_strategy_ext(out_base, f);
        return;
    }
    fprintf(f, "\n****** Initial fitting strategy and property ******");
    report_file_line(f, "\n Pla_in_file = %s\n", out_base, ".tt2");
    report_file_line(f, " Pla_out_file = %s\n", out_base, ".tt3");
    report_file_line(f, " Jedec_file = %s\n", out_base, ".jed");
    report_file_line(f, " Vector_file = %s\n", out_base, ".tmv");
    report_file_line(f, " verilog_file = %s\n", out_base, ".vt");
    fprintf(f, " Time_file = \n");
    report_file_line(f, " Log_file = %s\n", out_base, ".fit");
    fprintf(f, " err_file = \n");
    fprintf(f, " Device_name = %s\n", g_dev_package);
    fprintf(f, " Module_name = \n");
    type = report_package_type();
    fprintf(f, " Package_type = %s\n", type);
    free(type);
    fprintf(f, " Preassign_file = %s\n", g_opt.pin_filename ? g_opt.pin_filename : "");
    fprintf(f, " Property_file = \n");
    fprintf(f, " Sleep_mode = \n");
    if (g_opt.ifmt_edif)
        fprintf(f, " Preassignment = %s\n", g_opt.preassign == FIT_PREASSIGN_KEEP ? "keep" :
                g_opt.preassign == FIT_PREASSIGN_TRY ? "try" : "ignore");
    else
        fprintf(f, " Preassignment = \n");
    fprintf(f, g_opt.security != 0 ? " Security_mode = ON\n" : " Security_mode = OFF\n");
    fprintf(f, g_opt.pin_keep != 0 ? " Pin_keep_mode = ON\n" : " Pin_keep_mode = OFF\n");
    fprintf(f, " Dedicated_input_clock = \n");
    fprintf(f, " Dedicated_input_reset = \n");
    fprintf(f, " Dedicated_input_oe = \n");
    fprintf(f, g_opt.cupl != 0 ? " supporter = CUPL\n" : " supporter = ABEL\n");
    fprintf(f, g_opt.optimize != 0 ? " optimize = ON\n" : " optimize = OFF\n");
    fprintf(f, " Soft_buffer = \n");
    if (g_opt.xor_synthesis == 0) {
        fprintf(f, " Xor_synthesis = OFF\n");
    } else {
        fprintf(f, " Xor_synthesis = ");
        if (report_count(g_opt.xor_list) == 0)
            fprintf(f, " ON");
        else
            report_print_names(f, g_opt.xor_list);
        fprintf(f, "\n");
    }
    if (g_opt.foldback == 0) {
        fprintf(f, " Foldback_logic = OFF\n");
    } else {
        fprintf(f, " Foldback_logic = ");
        if (report_count(g_opt.foldback_list) == 0)
            fprintf(f, " on");
        else
            report_print_names(f, g_opt.foldback_list);
        fprintf(f, "\n");
    }
    fprintf(f, " Expander = \n");
    if (g_opt.cascade_logic == 0) {
        fprintf(f, " Cascade_logic = OFF\n");
    } else {
        /* the test looks at the foldback list, the names come from the
         * cascade list */
        fprintf(f, " Cascade_logic = ");
        if (report_count(g_opt.foldback_list) == 0)
            fprintf(f, " on");
        else
            report_print_names(f, g_opt.cascade_list);
        fprintf(f, "\n");
    }
    fprintf(f, " Dedicated_input = \n");
    report_flag_list(f, "Output_fast", g_opt.output_fast, g_opt.output_fast_list, " %s, ");
    if (report_count(g_opt.twoclock) != 0)
        fprintf(f, " twoclock = %s \n", (char *)g_opt.twoclock->items[0]);
    fprintf(f, " *******************************\n");
    fprintf(f, g_opt.pd1 != 0 ? " Power down pin 1 = ON\n" : " Power down pin 1 = OFF\n");
    fprintf(f, g_opt.pd2 != 0 ? " Power down pin 2 = ON\n" : " Power down pin 2 = OFF\n");
    fprintf(f, g_opt.power_reset != 0 ? " power_reset = ON\n" : " power_reset = OFF\n");
    fprintf(f, g_opt.jtag != 0 ? " JTAG = ON\n" : " JTAG = OFF\n");
    fprintf(f, g_opt.tdi_pullup != 0 ? " TDI pullup = ON\n" : " TDI pullup = OFF\n");
    fprintf(f, g_opt.tms_pullup != 0 ? " TMS pullup = ON\n" : " TMS pullup = OFF\n");
    report_flag_list(f, "MC_power", g_opt.mc_power, g_opt.mc_power_list, " %s, ");
    report_flag_list(f, "Open_collector", g_opt.open_collector, g_opt.open_collector_list, " %s, ");
    fprintf(f, g_opt.gck_itd[0] != 0 ? " ITD0 = ON\n" : " ITD0 = OFF\n");
    fprintf(f, g_opt.gck_itd[1] != 0 ? " ITD1 = ON\n" : " ITD1 = OFF\n");
    fprintf(f, g_opt.gck_itd[2] != 0 ? " ITD2 = ON\n" : " ITD2 = OFF\n");
    if (g_opt.fast_inlatch == 0) {
        fprintf(f, " Fast_inlatch = off\n");
    } else if (report_count(g_opt.fast_inlatch_list) == 0) {
        fprintf(f, " Fast_inlatch = on\n");
    } else {
        int i;
        fprintf(f, " Fast_inlatch = ");
        for (i = 0; i < g_opt.fast_inlatch_list->count; i++)
            fprintf(f, "%s ", (char *)g_opt.fast_inlatch_list->items[i]);
        fprintf(f, "\n");
    }
    fprintf(f, " *******************************\n");
}

/* ------------------------------------------------------------------ */
/* Slots reserved for JTAG and power down                              */
/* ------------------------------------------------------------------ */

void report_reserve_jtag_slots(FitList *slots, FitList *pins)
{
    static const char *const names[4] = { "TDI", "TMS", "TCK", "TDO" };
    int i;

    for (i = 0; i < report_count(slots); i++) {
        FitSlot *s = slots->items[i];
        int pos = list_find(pins, FIT_INT2PTR(s->pin), list_ptr_eq);
        if (pos != 0) {
            if (pos >= 1 && pos <= 4)
                s->signal = (char *)names[pos - 1];     /* a string literal, not owned */
            s->i28 = 1;
        }
    }
}

void report_reserve_pd_slots(FitList *slots, FitList *pins)
{
    int i;

    for (i = 0; i < report_count(slots); i++) {
        FitSlot *s = slots->items[i];
        int pos = list_find(pins, FIT_INT2PTR(s->pin), list_ptr_eq);
        if (pos != 0) {
            if (pos == 1 && g_opt.pd1 != 0)
                s->signal = util_strdup("PD1");
            else if (pos == 2 && g_opt.pd2 != 0)
                s->signal = util_strdup("PD2");
            s->i28 = 1;
        }
    }
}

void report_put_blanks(FILE *f, int n)
{
    for (; n > 0; n--)
        fprintf(f, "%s", " ");
}

/* ------------------------------------------------------------------ */
/* Sections of the result                                              */
/* ------------------------------------------------------------------ */

/* global OE nodes, global pins, input pins, control equations. */
static void report_write_assignments(FILE *f, FitList *slots, FitEntity *ent)
{
    int n_patch = 0;
    int i;

    fprintf(f, "\nPerforming global Output Enable pin assignments ...\n");
    if (report_count(g_opt.global_oe) != 0) {
        for (i = 0; i < report_count(g_opt.global_oe); i++) {
            FitVariable *v = entity_var_by_id(ent, FIT_PTR2INT(g_opt.global_oe->items[i]));
            if (v != NULL) {
                char *node = NULL;
                FitSlot *s = route_node_of_name(v->name, &node);
                if (s != NULL)
                    fprintf(f, "%s assigned to node %d (MC %d) \n", report_display_name(v->name, ent),
                            s->macrocell + 600, s->macrocell);
            }
        }
    }
    fprintf(f, "\nPerforming global pin assignments ...\n");
    fprintf(f, "--------------------------------------\n\n\n\n");
    fprintf(f, "Final global control pins assignment (if applicable)...\n");
    fprintf(f, "-------------------------------------------------------\n");
    for (i = 0; i < report_count(slots); i++) {
        FitSlot *s = slots->items[i];
        if (list_find(g_fit_global_names, s->signal, report_name_eq) != 0)
            fprintf(f, "%s assigned to pin  %d\n", report_display_name(s->signal, ent), s->pin);
    }
    fprintf(f, "\n\n\nPerforming input pin pre-assignments ...\n");
    fprintf(f, "------------------------------------\n");
    for (i = 0; i < report_count(slots); i++) {
        FitSlot *s = slots->items[i];
        if (util_str_icmp(s->signal, FIT_FREE_NAME) != 0 && s->block == 0)
            fprintf(f, "%s assigned to pin  %d\n", report_display_name(s->signal, ent), s->pin);
    }
    for (i = 0; i < report_count(slots); i++) {
        FitSlot *s = slots->items[i];
        if (util_str_icmp(s->buried, FIT_FREE_NAME) != 0) {
            FitVariable *v = entity_find_var(ent, s->buried);
            /* the variable may be missing */
            if (v != NULL && esp_signal_is_control(v->name, ent) != 0) {
                fprintf(f, "%s equation needs patching.\n", report_display_name(s->buried, ent));
                n_patch++;
            }
        }
    }
    if (n_patch == 1)
        fprintf(f, FIT_EXT ? "%d control equation needs patching\n" : "%d control eqution need patching\n", 1);
    else if (n_patch > 1)
        fprintf(f, FIT_EXT ? "%d control equations need patching\n" : "%d control equtions need patching\n",
                n_patch);
}

/* "Attempt to place floating signals" section: the pin, feedback node
 * and foldback node of every used slot. */
static void report_write_placement(FILE *f, FitEntity *ent, FitList *slots)
{
    int i;

    fprintf(f, "\nAttempt to place floating signals ...\n");
    fprintf(f, "------------------------------------\n");
    for (i = 0; i < report_count(slots); i++) {
        FitSlot *s = slots->items[i];
        if (util_str_icmp(s->signal, FIT_FREE_NAME) != 0 && s->block != 0)
            fprintf(f, "%s is placed at pin %d (MC %d)\n", report_display_name(s->signal, ent),
                    s->pin, s->macrocell);
        if (util_str_icmp(s->buried, FIT_FREE_NAME) != 0)
            fprintf(f, "%s is placed at feedback node %d (MC %d)\n", report_fb_display_name(s->buried, ent, s->block),
                    s->macrocell + 600, s->macrocell);
        if (util_str_icmp(s->foldback, FIT_FREE_NAME) != 0 && util_str_icmp(s->foldback, "NA") != 0)
            fprintf(f, "%s is placed at foldback expander node %d (MC %d)\n",
                    report_fb_display_name(s->foldback, ent, s->block), s->macrocell + 300, s->macrocell);
    }
    fprintf(f, "\n");
}

/* the names grouped by the code of their first character (0..0x7f), one
 * group per line, in braces. */
static void report_write_name_groups(FitList *names)
{
    int ch, i;

    fprintf(g_fit_report, "{\n");
    for (ch = 0; ch < 0x80; ch++) {
        int any = 0;
        for (i = 0; i < names->count; i++) {
            const char *n = names->items[i];
            if (n != NULL && *n == ch) {
                fprintf(g_fit_report, "%s,", n);
                any = 1;
            }
        }
        if (any)
            fprintf(g_fit_report, "\n");
    }
    fprintf(g_fit_report, "}\n");
}

/* fan-in list of a block. */
static void report_write_fanin(FitEntity *ent, FitList *uims)
{
    FitList *names = list_create();
    int i;

    for (i = 0; i < report_count(uims); i++) {
        FitUimSlot *u = uims->items[i];
        /* names with a cell pin extension all point at the one buffer of
         * report_cell_pin_name: see there */
        list_add(names, (void *)report_display_name(u->signal, ent));
    }
    report_write_name_groups(names);
    list_destroy(&names);
}

/* "A".."H"; "A" for any other block number */
static const char *report_block_letter(int block)
{
    static const char *const letters[8] = { "A", "B", "C", "D", "E", "F", "G", "H" };

    if (block >= 1 && block <= 8)
        return letters[block - 1];
    return "A";
}

/* The UIM part of report_write_result. */
static void report_write_uim(FitEntity *ent, FitList *uim)
{
    int b, i;

    fprintf(g_fit_report, "\nUniversal-Interconnect-Multiplexer assignments\n");
    fprintf(g_fit_report, "------------------------------------------------\n");
    for (b = 0; b < report_count(uim); b++) {
        FitList *l = uim->items[b];
        /* the block number comes from the first item of the list; 1 when the
         * list is empty */
        int block = report_count(l) > 0 ? ((FitUimSlot *)l->items[0])->block : 1;

        fprintf(g_fit_report, "FanIn assignment for block %s [%d]\n", report_block_letter(block), report_count(l));
        report_write_fanin(ent, l);
        fprintf(g_fit_report, "Multiplexer assignment for block %s\n", report_block_letter(block));
        for (i = 0; i < report_count(l); i++) {
            FitUimSlot *u = l->items[i];
            size_t len = strlen(u->sel_node);

            fprintf(g_fit_report, strlen(u->signal) < 8 ? "%s\t\t\t" : "%s\t\t", report_display_name(u->signal, ent));
            /* a node name ending in 'p' is a pin */
            u->sel_is_pin = (len > 0 && u->sel_node[len - 1] == 'p') ? 1 : 0;
            if (u->sel_is_pin == 0)
                fprintf(g_fit_report, "(MC%d\tFB)", u->name_pos);
            else
                fprintf(g_fit_report, "(MC%d\tP) ", u->name_pos);
            if (u->index == 0x29)
                fprintf(g_fit_report, "  : Warning");
            else
                fprintf(g_fit_report, "  : MUX %d", u->index);
            fprintf(g_fit_report, "\t\tRef (%s)\n", report_display_name(u->sel_node, ent));
        }
        fprintf(g_fit_report, "\n");
    }
}

/* pin list in CUPL syntax.  Only pins 1..100 are listed. */
static void report_write_pin_list(FILE *f, FitEntity *ent, FitList *slots)
{
    int pin, i;

    fprintf(f, "\n%s Pin/Node Placement:\n", g_dev_package);
    fprintf(f, "------------------------------------\n");
    for (pin = 1; pin <= 100; pin++) {
        for (i = 0; i < report_count(slots); i++) {
            FitSlot *s = slots->items[i];
            const char *name;
            if (util_str_icmp(s->signal, FIT_FREE_NAME) == 0 || s->pin != pin)
                continue;
            name = report_display_name(s->signal, ent);
            if (pin > 9) {
                if (s->block == 0)
                    fprintf(f, "Pin %d = %s;\n", s->pin, name);
                else if (s->macrocell < 10)
                    fprintf(f, "Pin %d = %s; /* MC  %d */\n", s->pin, name, s->macrocell);
                else
                    fprintf(f, "Pin %d = %s; /* MC %d */ \n", s->pin, name, s->macrocell);
            } else if (s->block != 0) {
                fprintf(f, "Pin %d  = %s; /* MC %d */\n", s->pin, name, s->macrocell);
            } else {
                fprintf(f, "Pin %d  = %s;\n", s->pin, name);
            }
        }
    }
    for (i = 0; i < report_count(slots); i++) {
        FitSlot *s = slots->items[i];
        if (util_str_icmp(s->foldback, FIT_FREE_NAME) != 0 && util_str_icmp(s->foldback, "NA") != 0)
            fprintf(f, "PINNODE %d = %s; /* MC %d Foldback */\n", s->macrocell + 300,
                    report_fb_display_name(s->foldback, ent, s->block), s->macrocell);
    }
    for (i = 0; i < report_count(slots); i++) {
        FitSlot *s = slots->items[i];
        if (util_str_icmp(s->buried, FIT_FREE_NAME) != 0)
            fprintf(f, "PINNODE %d = %s; /* MC %d Feedback */\n", s->macrocell + 600,
                    report_fb_display_name(s->buried, ent, s->block), s->macrocell);
    }
}

/* number of product terms of the slot that are in use. */
static int report_used_pterms(const FitSlot *s)
{
    int n = 0;
    int i;

    for (i = 0; i < FIT_PT_PER_MC; i++)
        if (s->pt_use[i] != 0)
            n++;
    return n;
}

/* usage per logic block and the totals. */
static void report_write_usage(FitEntity *ent, FILE *f, FitList *slots, FitList *uim)
{
    int n_ff = 0, n_cells = 0, n_pins = 0, n_fold = 0, n_casc = 0, n_nodes = 0;
    int n_in_pins = 0, n_out_pins = 0, n_pt = 0, n_ded = 0;
    int io_total;
    int block, i;

    (void)ent;
    /* the extended mode calls the logic cells macro cells and shows the fan-in as a
     * share of the 40 inputs of a block */
    fprintf(f, "\nLogic Array Block");
    fprintf(f, FIT_EXT ? "\t\t\tMacro Cells" : "\tLogic Cells");
    fprintf(f, "\tI/O Pins");
    fprintf(f, "\tFoldbacks");
    fprintf(f, "\tTotalPT");
    fprintf(f, FIT_EXT ? "\tFanIN" : "\t\tFanIN");
    fprintf(f, FIT_EXT ? "\t\tCascades" : "\tCascades");
    for (block = 1; block <= g_dev->n_blocks; block++) {
        int first = (block - 1) * 16 + 1;
        int cells = 0, pins = 0, fold = 0, casc = 0, pt = 0;
        /* The fan-in shown for a block is item block-1 of the routing list
         * whether the list is that long or not.  The list has lost its empty
         * blocks by then (place_prune_uim_slots deletes them with
         * list_del_shift), so for the last blocks this reads the slots
         * behind the end of the list, which still hold what was shifted out
         * of them: the fan-in shown for those blocks is that of another
         * block.  This is deliberate, the report keeps that table as it
         * is; the read stays within the allocated array. */
        FitList *fanin = (uim != NULL && block - 1 < uim->capacity) ? uim->items[block - 1] : NULL;

        if (FIT_EXT)
            fprintf(f, block == 8 ? "\n%c: MC%d- MC%d" : "\n%c: MC%d\t- MC%d", 'A' + block - 1, first, first + 0xf);
        else
            fprintf(f, block == 8 ? "\n%c: LC%d- LC%d" : "\n%c: LC%d\t- LC%d", 'A' + block - 1, first, first + 0xf);
        for (i = first - 1; i < first + 0xf; i++) {
            FitSlot *s = slots->items[i];
            pt += report_used_pterms(s);
            if (s->inst != NULL && s->inst->clk != NULL)
                n_ff++;
            if (util_str_icmp(s->signal, FIT_FREE_NAME) != 0) {
                pins++;
                /* the extended mode counts TDO as an output */
                if (s->fct_id == 0 && (!FIT_EXT || !util_str_ieq(s->signal, "TDO"))) {
                    n_in_pins++;
                } else {
                    cells++;
                    n_out_pins++;
                }
            }
            if (util_str_icmp(s->buried, FIT_FREE_NAME) != 0)
                cells++;
            if (util_str_icmp(s->foldback, FIT_FREE_NAME) != 0 && util_str_icmp(s->foldback, "NA") != 0)
                fold++;
            if (util_str_icmp(s->cascade, FIT_FREE_NAME) != 0)
                casc++;
        }
        fprintf(f, "\t\t%d/16(%d%%)", cells, cells * 100 / 16);
        fprintf(f, "\t%d/16(%d%%)", pins, pins * 100 / 16);
        fprintf(f, "\t%d/16(%d%%)", fold, fold * 100 / 16);
        fprintf(f, "\t%d/80(%d%%)", pt, pt * 100 / 80);
        if (FIT_EXT)
            fprintf(f, "\t%d/40(%d%%)", report_count(fanin), report_count(fanin) * 100 / 40);
        else
            fprintf(f, "\t(%d)", report_count(fanin));
        fprintf(f, "\t%d", casc);
        n_pt += pt;
        n_fold += fold;
        n_pins += pins;
        n_cells += cells;
        n_casc += casc;
        n_nodes += fold + cells;
    }
    for (i = 0; i < report_count(slots); i++) {
        FitSlot *s = slots->items[i];
        if (s->block == 0 && util_str_icmp(s->signal, FIT_FREE_NAME) != 0)
            n_ded++;
    }
    /* Number of I/O pins of the package. */
    if (util_str_icmp(g_dev_package, "PLCC44") == 0 || util_str_icmp(g_dev_package, "TQFP44") == 0)
        io_total = 0x20;
    else if (util_str_icmp(g_dev_package, "PLCC68") == 0)
        io_total = 0x30;
    else if (util_str_icmp(g_dev_package, "PLCC84") == 0)
        io_total = 0x40;
    else if (util_str_icmp(g_dev_package, "PQFP160") == 0)
        io_total = 0x60;
    else if (util_str_icmp(g_dev_package, "PQFP100") == 0 || util_str_icmp(g_dev_package, "TQFP100") == 0)
        io_total = util_str_icmp(g_family, "1504") != 0 ? 0x50 : 0x40;
    else
        io_total = 0x40;
    fprintf(f, "\n\nTotal dedicated input used:\t%d/4 \t(%d%%)", n_ded, n_ded * 100 / 4);
    fprintf(f, "\nTotal I/O pins used\t\t%d/%d \t(%d%%)", n_pins, io_total, n_pins * 100 / io_total);
    fprintf(f, FIT_EXT ? "\nTotal Macro cells used \t\t%d/%d \t(%d%%)" : "\nTotal Logic cells used \t\t%d/%d \t(%d%%)",
            n_cells + n_casc, g_dev->n_macrocells,
            (n_cells + n_casc) * 100 / g_dev->n_macrocells);
    fprintf(f, "\nTotal Flip-Flop used \t\t%d/%d \t(%d%%)", n_ff, g_dev->n_macrocells, n_ff * 100 / g_dev->n_macrocells);
    fprintf(f, "\nTotal Foldback logic used \t%d/%d \t(%d%%)", n_fold, g_dev->n_macrocells,
            n_fold * 100 / g_dev->n_macrocells);
    fprintf(f, "\nTotal Nodes+FB/MCells \t\t%d/%d \t(%d%%)", n_nodes, g_dev->n_macrocells,
            n_nodes * 100 / g_dev->n_macrocells);
    fprintf(f, "\nTotal cascade used \t\t%d", n_casc);
    fprintf(f, FIT_EXT ? "\nTotal input pins \t\t\t%d" : "\nTotal input pins \t\t%d", n_ded + n_in_pins);
    fprintf(f, "\nTotal output pins \t\t%d", n_out_pins);
    fprintf(f, FIT_EXT ? "\nTotal Pts \t\t\t\t%d" : "\nTotal Pts \t\t\t%d", n_pt);
}

/* A slot with every name FIT_FREE_NAME. */
static FitSlot *report_empty_slot(void)
{
    FitSlot *s = util_calloc(1, sizeof *s);

    s->cascade = (char *)FIT_FREE_NAME;
    s->signal = (char *)FIT_FREE_NAME;
    s->fn_kind = (char *)FIT_FREE_NAME;
    s->buried = (char *)FIT_FREE_NAME;
    s->foldback = (char *)FIT_FREE_NAME;
    return s;
}

/* the placement slot of a package row (by macrocell, or by pin
 * for a dedicated input); an empty slot when there is none. */
static FitSlot *report_slot_of_row(FitList *slots, const FitPkgSlot *row)
{
    int i;

    for (i = 0; i < report_count(slots); i++) {
        FitSlot *s = slots->items[i];
        if (row->macrocell == 0) {
            if (s->pin == row->pin)
                return s;
        } else if (s->macrocell == row->macrocell) {
            return s;
        }
    }
    return report_empty_slot();
}

/* output enable column. */
static const char *report_oe_text(const FitSlot *s)
{
    if (s->pt_use[4] == 6)
        return "PT";
    switch (s->oe_src) {
    case 0x12: return FIT_FREE_NAME;
    case 0xb:  return "OE0";
    case 0xc:  return "OE1";
    case 0xd:  return "OE2";
    case 0xe:  return "OE3";
    case 0xf:  return "OE4";
    case 0x10: return "OE5";
    case 0x11: return "on";
    default:   return "";
    }
}

/* the DCERP field: type, clock, clock enable, reset, preset. */
static const char *report_dcerp(const FitSlot *s)
{
    static char text[8];
    const char *type, *clock, *enable, *reset, *preset;

    if (s->cell_kind == 2)
        type = "T";
    else if (s->cell_kind == 4)
        type = "L";
    else if (s->cell_kind == 8 || s->cell_kind == 9 || s->cell_kind != 3)
        type = "C";
    else
        type = "D";
    if (s->clk_src == 8 || s->clk_src == 9 || s->clk_src == 10)
        clock = "g";
    else
        clock = (s->pt_use[3] == 3) ? "c" : "-";
    enable = (s->pt_use[3] == 7) ? "e" : "-";
    if (s->ar_src == 4)
        reset = "g";
    else
        reset = (s->pt_use[2] == 4) ? "r" : "-";
    preset = (s->pt_use[4] == 5) ? "p" : "-";
    snprintf(text, sizeof text, "%s%s%s%s%s", type, clock, enable, reset, preset);
    return text;
}

/* Extended mode: is the name in the list (ignoring case)? */
static int report_in_list(FitList *names, const char *name)
{
    return list_find(names, (void *)name, report_name_eq) != 0;
}

/* Extended mode: the pins that are dedicated inputs: pull down is possible there
 * and grounding is not */
static int report_dedicated_pin(const FitSlot *s)
{
    if (util_str_icmp(g_dev_package, "TQFP44") == 0)
        return s->pin > 36 && s->pin < 41;
    return s->pin > 86 && s->pin < 91;
}

/* Extended mode: the pin options field: for BE devices termination and hysteresis,
 * then slew rate and open collector */
static const char *report_pin_options(const FitSlot *s)
{
    static char text[8];
    const char *term = "n", *hyst = "-", *slew, *opencol;

    if (g_opt.output_fast == 0)
        slew = "s";
    else
        slew = g_opt.output_fast_list == NULL || report_in_list(g_opt.output_fast_list, s->signal) ? "f" : "s";
    if (g_opt.open_collector == 0)
        opencol = "-";
    else
        opencol = g_opt.open_collector_list == NULL || report_in_list(g_opt.open_collector_list, s->signal) ? "o" : "-";
    if (g_opt.be == 0) {
        snprintf(text, sizeof text, "%s%s", slew, opencol);
        return text;
    }
    if (util_str_icmp(s->signal, FIT_FREE_NAME) == 0) {
        if (g_opt.unused_to_ground != 0)
            term = report_dedicated_pin(s) ? "d" : "g";
        else if (g_opt.pull_up_unused != 0)
            term = "u";
        else if (g_opt.unused_to_pinkeeper != 0)
            term = "k";
    } else if (g_opt.pin_keep != 0 && (g_opt.pin_keep_list == NULL || report_in_list(g_opt.pin_keep_list, s->signal))) {
        term = "k";
    } else if (report_in_list(g_opt.pull_up_list, s->signal)) {
        term = "u";
    } else if (report_in_list(g_opt.pull_down_list, s->signal)) {
        term = "d";
    }
    if (g_opt.schmitt_trigger != 0 && report_in_list(g_opt.schmitt_list, s->signal))
        hyst = "s";
    else if (g_opt.sstl_input != 0 && report_in_list(g_opt.sstl_list, s->signal))
        hyst = "S";
    snprintf(text, sizeof text, "%s%s%s%s", term, hyst, slew, opencol);
    return text;
}

/* The macrocell table of report_write_result. */
static void report_write_mc_table(FitEntity *ent, FitList *slots, FitList *rows)
{
    FILE *f = g_fit_report;
    unsigned w_pin = 9, w_fb = 8, w_fold = 9, w_casc = 0xb;
    int i;

    for (i = 1; i <= report_count(rows); i++) {
        FitSlot *s = report_slot_of_row(slots, rows->items[i - 1]);
        unsigned n;
        if ((n = (unsigned)strlen(report_display_name(s->signal, ent))) > w_pin)
            w_pin = n;
        if ((n = (unsigned)strlen(report_fb_display_name(s->buried, ent, s->block))) > w_fb)
            w_fb = n;
        if ((n = (unsigned)strlen(report_fb_display_name(s->foldback, ent, s->block))) > w_fold)
            w_fold = n;
        if ((n = (unsigned)strlen(report_display_name(s->cascade, ent))) > w_casc)
            w_casc = n;
    }
    fprintf(f, "MCell Pin# Oe   ");
    fprintf(f, "PinDrive ");
    report_put_blanks(f, (int)w_pin - 8);
    fprintf(f, "DCERP  ");
    fprintf(f, "FBDrive ");
    report_put_blanks(f, (int)w_fb - 7);
    fprintf(f, "DCERP  ");
    fprintf(f, "Foldback ");
    report_put_blanks(f, (int)w_fold - 8);
    fprintf(f, "CascadeOut ");
    report_put_blanks(f, (int)w_casc - 7);
    if (FIT_EXT)
        fprintf(f, g_opt.be == 0 ? "TotPT SO\n" : "TotPT THSO\n");
    else
        fprintf(f, "TotPT output_slew\n");
    for (i = 1; i <= report_count(rows); i++) {
        const FitPkgSlot *row = rows->items[i - 1];
        const char *text;
        FitSlot *s;
        int pad;

        if (g_dev->n_macrocells < i)
            fprintf(f, "MC0");
        else
            fprintf(f, "MC%d", i);
        if (i < 10 || g_dev->n_macrocells < i)
            fprintf(f, "   %d", row->pin);
        else if (i < 100)
            fprintf(f, "  %d", row->pin);
        else
            fprintf(f, " %d", row->pin);
        if (row->pin < 10)
            pad = 5;
        else if (row->pin < 100)
            pad = 4;
        else
            pad = 3;
        report_put_blanks(f, pad - 1);
        s = report_slot_of_row(slots, row);
        text = report_oe_text(s);
        fprintf(f, "%s", text);
        report_put_blanks(f, 5 - (int)strlen(text));
        text = report_display_name(s->signal, ent);
        fprintf(f, "%s", text);
        report_put_blanks(f, (int)(w_pin - (unsigned)strlen(text)) + 1);
        if (util_str_icmp(s->signal, FIT_FREE_NAME) == 0) {
            report_put_blanks(f, 7);
        } else if (s->fct_id == 0 && (!FIT_EXT || util_str_icmp(s->signal, "TDO") != 0)) {
            fprintf(f, "%s", "INPUT");
            report_put_blanks(f, 2);
        } else if (util_str_icmp(s->buried, FIT_FREE_NAME) == 0) {
            text = report_dcerp(s);
            fprintf(f, "%s", text);
            report_put_blanks(f, 7 - (int)strlen(text));
        } else {
            fprintf(f, "%s", "C----");
            report_put_blanks(f, 2);
        }
        text = report_fb_display_name(s->buried, ent, s->block);
        fprintf(f, "%s", text);
        report_put_blanks(f, (int)(w_fb - (unsigned)strlen(text)) + 1);
        if (util_str_icmp(s->buried, FIT_FREE_NAME) == 0) {
            report_put_blanks(f, 7);
        } else {
            text = report_dcerp(s);
            fprintf(f, "%s", text);
            report_put_blanks(f, 7 - (int)strlen(text));
        }
        text = report_fb_display_name(s->foldback, ent, s->block);
        fprintf(f, "%s", text);
        report_put_blanks(f, (int)(w_fold - (unsigned)strlen(text)) + 1);
        if (util_str_icmp(s->cascade, FIT_FREE_NAME) == 0) {
            fprintf(f, "%s", report_display_name(s->cascade, ent));
            pad = (int)(w_casc - (unsigned)strlen(s->cascade)) + 4;
        } else {
            text = report_display_name(s->cascade, ent);
            fprintf(f, "-> %s", text);
            pad = (int)(w_casc - (unsigned)strlen(text)) + 1;
        }
        report_put_blanks(f, pad);
        fprintf(f, "%d", report_used_pterms(s));
        report_put_blanks(f, 5);
        if (FIT_EXT) {
            fprintf(f, "%s \n", report_pin_options(s));
            continue;
        }
        if (g_opt.output_fast == 0
            || (g_opt.output_fast_list != NULL
                && list_find(g_opt.output_fast_list, s->signal, report_name_eq) == 0))
            fprintf(f, "slow\n");
        else
            fprintf(f, "fast\n");
    }
}

/* Extended mode: the command string, the program name and the input file on the
 * first line and every option with its value on a line of its own.  The
 * words are taken from g_command, which is used up. */
static void report_print_command(void)
{
    char *word, *next;

    fprintf(g_fit_report, "\n\n");
    for (;;) {
        word = util_next_word(&g_command, 0);
        if (word == NULL)
            return;
        if (word[0] == '-')
            fprintf(g_fit_report, "\n");
        fputs(word, g_fit_report);
        next = util_next_word(&g_command, 0);
        while (next != NULL && next[0] != '-') {
            fprintf(g_fit_report, " %s", next);
            free(next);
            next = util_next_word(&g_command, 0);
        }
        fprintf(g_fit_report, "\n");
        if (next == NULL)
            return;
        fprintf(g_fit_report, "%s ", next);
        free(next);
        free(word);
    }
}

void report_open(const char *out_base)
{
    char *name = util_strdup(out_base);
    time_t now;

    util_str_append(&name, ".fit");
    g_fit_report = file_open_write(name);
    if (g_fit_report == NULL) {
        fprintf(g_msg, "Cannot open  file %s\n", name);
        return;         /* the name is not freed on this path */
    }
    time(&now);
    fprintf(g_fit_report, "%s %s fit%s: ATF%s Fitter, ", PROGRAM_NAME, g_version, g_family,
            g_family);
    fprintf(g_fit_report, "running %s\n\n", ctime(&now));
    if (FIT_EXT)
        report_print_command();
    else
        fprintf(g_fit_report, "%s\n\n", g_command);
    free(name);
}

/* Extended mode: what the names of the package drawing mean */
static void report_write_pin_legend_ext(FILE *f)
{
    if (g_opt.be == 0) {
        fprintf(f, "\nVCC = Supply Voltage pin for the device core\n");
    } else {
        fprintf(f, "\nVCCINT = Supply Voltage pin for the device core\n");
        fprintf(f, "\nVCCIOA, VCCIOB = Supply Voltage pin for the output buffers\n");
    }
    fprintf(f, "\nGND = GND pin which must be connected to ground\n");
    if (g_opt.jtag != 0)
        fprintf(f, "\nTMS,TDI,TDO,TDI = JTAG pins reserved for JTAG interface\n");
    if (g_opt.be == 0) {
        fprintf(f, "\nNC = Unused I/O pins which must be unconnected on the board\n");
        return;
    }
    fprintf(f, "\nUnused_GND = unused pin which is configured as ground\n");
    fprintf(f, "\nUnused_PU = unused pin which is pulled up\n");
    fprintf(f, "\nUnused_PK = unused pin which is connected to Pin Keeper\n");
    fprintf(f, "\nUnused_Z = unused pin which is configured as floating\n");
    if (g_opt.sstl_input != 0)
        fprintf(f, "\nVREFA, VREFB = Reference Voltage inputs for bankA and bankB\n");
}

/* Extended mode: the legend of the pin options column of the macrocell table */
static void report_write_options_legend_ext(FILE *f)
{
    if (g_opt.be == 0) {
        fprintf(f, "\nSO Pin Options Field = Summary of Allocations.\n");
        fprintf(f, "||\n");
        fprintf(f, "||_OpenCol [o,-]      ==  o = Open Collector enabled, - CMOS drive.\n");
        fprintf(f, "|\n");
        fprintf(f, "|__Slew [s,f]         ==  Output Slew/Drive rate, s = slow/low, f = fast/hi drive.\n");
        fprintf(f, "\n\n");
        return;
    }
    fprintf(f, "\nTHSO Pin Options Field = Summary of Allocations.\n");
    fprintf(f, "||||\n");
    fprintf(f, "||||_OpenCol [o,-]      ==  o = Open Collector enabled, - CMOS drive.\n");
    fprintf(f, "|||\n");
    fprintf(f, "|||__Slew [s,f]         ==  Output Slew/Drive rate, s = slow/low, f = fast/hi drive.\n");
    fprintf(f, "||\n");
    fprintf(f, "||___Hyst[s,d,-]        ==  Hysteresis, s = schmitt_trigger, S = SSTL input, - = none.\n");
    fprintf(f, "|\n");
    fprintf(f, "|____Termin [k,u,d,g,n] ==  Pin Termination, k = Pin Keeper, u = Pull up, d = Pull down, g = gnd, n = none.\n");
    fprintf(f, "\n");
    fprintf(f, "Note: Pull down can be used only on dedicated pins & gnd only on unused pins,\n");
    fprintf(f, "SSTL input is only avalaible for 1504BE/1508BE devices \n\n\n");
}

void report_write_result(FitEntity *ent, const char *out_base, FitList *slots, FitList *uim)
{
    FILE *f;
    FitList *rows = dev_select_package();

    if (FIT_EXT)
        report_collect_fb_names(slots, rows);
    report_write_assignments(g_fit_report, slots, ent);
    report_write_placement(g_fit_report, ent, slots);
    f = g_fit_report;
    if (FIT_EXT) {
        dev_print_pinout_ext(f, slots);
        report_write_pin_legend_ext(f);
    } else {
        dev_print_pinout(f, slots);
        fprintf(f, "\nVCC = Supply Voltage pin which must be connected to (5.0V or 3.0V)\n");
        fprintf(f, "\nGND = GND pin which must be connected to ground\n");
        if (g_opt.jtag != 0)
            fprintf(f, "\nTMS,TDI,TDO,TDI = JTAG pins which must reserved for the JTAG interface\n");
        fprintf(f, "\nNC = Unused I/O pins which must be unconnected on the board\n");
    }
    report_write_uim(ent, uim);
    if (g_report_fits == 0)
        fprintf(f, "Warning : JEDEC file not created\n");
    else
        fprintf(f, "Creating JEDEC file %s.jed ...\n", out_base);
    tt3_print_programmed_logic(f, ent);
    report_write_pin_list(f, ent, slots);
    fprintf(f, "\n** Resource Usage **\n\n");
    fprintf(f, "\nDCERP Field = Summary of Allocations.\n");
    fprintf(f, "|||||\n");
    fprintf(f, "|||||_Preset [p,-]       ==  p = PT preset, - No Preset.\n");
    fprintf(f, "||||\n");
    fprintf(f, "||||__Reset [g,r,-]      ==  g= Global AR, r = PT reset, - No reset.\n");
    fprintf(f, "|||\n");
    if (FIT_EXT)
        fprintf(f, "|||___Clock Enable [e,-] ==  e = Product Term, - always enabled.\n");
    else
        fprintf(f, "|||___Clock Enable [e,-] ==  e = Product Term, - always enabled, - none.\n");
    fprintf(f, "||\n");
    fprintf(f, "||____Clock [c,g,-],     ==  c = Product term, g = Global term, - No Clock.\n");
    fprintf(f, "|\n");
    fprintf(f, "|_____Type [C,D,L,T],    ==  Register type C= combin, D=dff, L=latch, T=tff.\n");
    fprintf(f, "\n");
    fprintf(f, "For input only = INPUT.\n\n");
    if (FIT_EXT)
        report_write_options_legend_ext(f);
    report_write_mc_table(ent, slots, rows);
    report_write_usage(ent, f, slots, uim);
    fprintf(f, "\nCreating pla file %s.tt3 with %d inputs %d outputs, %d pins %d nodes and %d pterms...\n",
            g_opt.out_base, g_tt3_n_in, g_tt3_n_out, g_tt3_n_pins, g_tt3_n_nodes, g_tt3_n_pterms);
    if (g_report_fits == 0)
        fprintf(f, "\n----------------  End fitter\n");
    else
        fprintf(f, "\n----------------  End fitter, Design FITS\n");
    if (g_report_fits == 0)
        fprintf(f, "$Device %s does not fit \n", g_dev_package);
    else if (FIT_EXT)
        fprintf(f, "$Device %s fits; JTAG %s; Secure %s\n", g_dev_package, g_opt.jtag != 0 ? "ON" : "OFF",
                g_opt.security != 0 ? "ON" : "OFF");
    else
        fprintf(f, "$Device %s fits \n", g_dev_package);
    util_timer_update();
    fprintf(f, "FIT%s completed in %4.2f seconds\n", g_family, util_timer_session_elapsed());
    util_fclose(g_fit_report);
    g_fit_report = NULL;
    list_free_all(&rows, util_free_ptr);
}
