/*
 * tt_tt2prop.c - part of the .tt2 reader: "#$ PROPERTY ATMEL name [=]
 * value ...".  The names are compared without regard to case; the same
 * option globals are written again from the command line after the file
 * has been read (share/lincupl/FIT1500.M describes the properties from the
 * user's side).
 */
#include "tt_priv.h"

static int at_keyword(void)
{
    return g_lex_token >= 1 && g_lex_token <= 10;
}

static int value_is(const char *word)
{
    return util_str_icmp(g_lex_text, word) == 0;
}

/* the words up to the next keyword, the current one included, as copies */
static void collect_names(FitList *list)
{
    while (!at_keyword()) {
        if (g_lex_token == FIT_TOK_IDENT)
            list_add(list, util_strdup(g_lex_text));
        lex_next_token();
    }
}

static void collect_names_into(FitList **list)
{
    if (*list == NULL)
        *list = list_create();
    collect_names(*list);
}

/* the current token is the word after PROPERTY.  On return the
 * current token is the next keyword of the header. */
void tt2_property(void)
{
    char *name;

    if (util_str_icmp(g_lex_text, "ATMEL") != 0) {
        while (!at_keyword())
            lex_next_token();
        return;
    }
    lex_next_token();
    name = util_strdup(g_lex_text);     /* never released */
    lex_next_token();
    if (value_is("="))
        lex_next_token();
    /* the value is the next word; braces and the like are skipped */
    for (;;) {
        if (at_keyword())
            return;
        if (g_lex_token == FIT_TOK_IDENT)
            break;
        lex_next_token();
    }

    if (util_str_icmp(name, "security") == 0) {
        if (value_is("ON"))
            g_opt.security = 1;
    } else if (util_str_icmp(name, "logic_doubling") == 0) {
        if (value_is("ON"))
            g_opt.logic_doubling = 1;
    } else if (util_str_icmp(name, "pin_keep") == 0) {
        if (value_is("OFF"))
            g_opt.pin_keep = 0;
    } else if (util_str_icmp(name, "JTAG") == 0) {
        if (value_is("OFF"))
            g_opt.jtag = 0;
    } else if (util_str_icmp(name, "power_reset") == 0) {
        if (value_is("ON"))
            g_opt.power_reset = 1;
    } else if (util_str_icmp(name, "pd1") == 0) {
        if (value_is("on"))
            g_opt.pd1 = 1;
    } else if (util_str_icmp(name, "pd2") == 0) {
        if (value_is("on"))
            g_opt.pd2 = 1;
    } else if (util_str_icmp(name, "TDI_pullup") == 0) {
        if (value_is("on"))
            g_opt.tdi_pullup = 1;
    } else if (util_str_icmp(name, "TMS_pullup") == 0) {
        if (value_is("on"))
            g_opt.tms_pullup = 1;
    } else if (util_str_icmp(name, "GCK_ITD") == 0) {
        if (value_is("OFF")) {
            g_opt.gck_itd[0] = 0;
            g_opt.gck_itd[1] = 0;
            g_opt.gck_itd[2] = 0;
        } else if (value_is("GCK0")) {
            g_opt.gck_itd[1] = 0;
            g_opt.gck_itd[2] = 0;
        } else if (value_is("GCK1")) {
            g_opt.gck_itd[0] = 0;
            g_opt.gck_itd[2] = 0;
        } else if (value_is("GCK2")) {
            g_opt.gck_itd[0] = 0;
            g_opt.gck_itd[1] = 0;
        }
    } else if (util_str_icmp(name, "output_fast") == 0) {
        if (!value_is("off")) {
            g_opt.output_fast = 1;
            if (!value_is("on")) {
                /* Unlike the other lists this one is always started
                 * afresh, and its items are the address of the token
                 * buffer, not copies: every entry reads as whatever token
                 * was scanned last. */
                g_opt.output_fast_list = list_create();
                while (!at_keyword()) {
                    if (g_lex_token == FIT_TOK_IDENT)
                        list_add(g_opt.output_fast_list, g_lex_text);
                    lex_next_token();
                }
            }
        }
    } else if (util_str_icmp(name, "mc_power") == 0) {
        if (!value_is("off")) {
            g_opt.mc_power = 1;
            if (!value_is("on")) {
                g_opt.mc_power_list = list_create();    /* always a new list */
                collect_names(g_opt.mc_power_list);
            }
        }
    } else if (util_str_icmp(name, "open_collector") == 0) {
        if (!value_is("off")) {
            g_opt.open_collector = 1;
            if (!value_is("on"))
                collect_names_into(&g_opt.open_collector_list);
        }
    } else if (util_str_icmp(name, "cascade_logic") == 0) {
        if (!value_is("off")) {
            g_opt.cascade_logic = 1;
            if (!value_is("on"))
                collect_names_into(&g_opt.cascade_list);
        }
    } else if (util_str_icmp(name, "foldback_Logic") == 0 || util_str_icmp(name, "FOLD") == 0) {
        if (!value_is("off")) {
            g_opt.foldback = 1;
            if (!value_is("on"))
                collect_names_into(&g_opt.foldback_list);
        }
    } else if (util_str_icmp(name, "xor_synthesis") == 0) {
        if (!value_is("off")) {
            if (value_is("on"))
                g_opt.xor_synthesis = 1;
            else
                collect_names_into(&g_opt.xor_list);
        }
    } else if (util_str_icmp(name, "soft_buffer") == 0) {
        if (!value_is("off") && !value_is("on"))
            collect_names_into(&g_opt.soft_buffer);
    } else if (util_str_icmp(name, "global_fold") == 0) {
        if (!value_is("off") && !value_is("on"))
            collect_names_into(&g_opt.global_fold);
    } else if (util_str_icmp(name, "fast_inlatch") == 0) {
        if (!value_is("off")) {
            g_opt.fast_inlatch = 1;
            if (!value_is("on"))
                collect_names_into(&g_opt.fast_inlatch_list);
        }
    } else if (util_str_icmp(name, "Global_OE") == 0) {
        if (!value_is("off") && !value_is("on"))
            collect_names_into(&g_opt.global_oe);
    } else if (util_str_icmp(name, "preassign") == 0) {
        if (value_is("ignore"))
            g_opt.preassign = FIT_PREASSIGN_IGNORE;
        else if (value_is("keep"))
            g_opt.preassign = FIT_PREASSIGN_KEEP;
        else if (value_is("try"))
            g_opt.preassign = FIT_PREASSIGN_TRY;
    } else if (util_str_icmp(name, "twoclock") == 0) {
        /* the list is created by the job before the file is read */
        if (g_opt.twoclock == NULL)
            g_opt.twoclock = list_create();
        if (list_find(g_opt.twoclock, g_lex_text, tt_cb_str_ieq) == 0)
            list_add(g_opt.twoclock, util_strdup(g_lex_text));
    } else if (util_str_icmp(name, "push_gate") == 0) {
        if (value_is("off"))
            g_opt.push_gate = 0;
    } else if (util_str_icmp(name, "optimize") == 0) {
        if (value_is("off"))
            g_opt.optimize = 0;
    } else {
        fprintf(g_fit_report, "Warning : PROPERTY %s is invalid and ignored\n", name);
    }

    while (!at_keyword())
        lex_next_token();
}

/* Extended mode: "#$ PROPERTY ATMEL [{] name [=] value ..." becomes the job
 * parameter `name` ("FOLD" is foldback_Logic).  The value is the words up
 * to the next keyword or "}", separated by commas; a parameter the command
 * line or an earlier line already gave keeps its value, extended by ","
 * and this one when the two differ.  The options are then taken from the
 * parameters as if they had been on the command line. */
void tt2_property_param(FitList *job)
{
    char value[FIT_PARAM_VALUE_MAX];
    FitParam *param;
    char *name;
    int words = 0;

    if (util_str_icmp(g_lex_text, "ATMEL") != 0) {
        while (!at_keyword())
            lex_next_token();
        return;
    }
    lex_next_token();
    name = util_strdup(g_lex_text);     /* never released */
    if (util_str_icmp(name, "{") == 0) {
        lex_next_token();
        name = util_strdup(g_lex_text);
    }
    if (util_str_icmp(name, "FOLD") == 0)
        name = util_strdup("foldback_Logic");
    lex_next_token();
    if (value_is("="))
        lex_next_token();
    for (;;) {
        if (at_keyword())
            return;
        if (g_lex_token == FIT_TOK_IDENT)
            break;
        lex_next_token();
    }

    param = param_find(job, name);
    value[0] = '\0';
    while (!at_keyword()) {
        if (words == 1) {
            if (value_is("}"))
                break;
            if (value_is(",")) {
                util_str_cat_n(value, g_lex_text, sizeof value);
                words = 0;
            } else {
                util_str_cat_n(value, ",", sizeof value);
                util_str_cat_n(value, g_lex_text, sizeof value);
            }
        } else {
            util_str_cat_n(value, g_lex_text, sizeof value);
            words++;
        }
        lex_next_token();
    }
    if (param == NULL) {
        param = util_calloc(1, sizeof *param);
        util_str_copy_n(param->name, name, sizeof param->name, 1);
        util_str_copy_n(param->value, value, sizeof param->value, 1);
        list_add(job, param);
    } else if (util_str_icmp(param->value, value) != 0) {
        util_str_cat_n(param->value, ",", sizeof param->value);
        util_str_cat_n(param->value, value, sizeof param->value);
    }
    while (!at_keyword())
        lex_next_token();
}
