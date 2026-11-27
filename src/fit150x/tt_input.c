/*
 * tt_input.c - input_driver: reads the design in the format named by the
 * parameter "ifmt" and registers it in the entity table.
 *
 * The PLA (.tt2) and EDIF readers are implemented. The equation reader
 * (ifmt EQN), the PALASM post-processing and the ABEL branch
 * (one entity per item of a list that no reader fills in) are not: they are
 * reachable only with a -str ifmt value the CUPL flow never passes.
 */
#include "tt_priv.h"

int input_read(FitList *params, FitList *job)
{
    const char *file = ((FitParam *)params->items[0])->value;
    FitEntity *entity = NULL;
    char *ifmt, *name;
    FILE *in;
    int slot, type;

    ifmt = param_get(params, "ifmt");
    if (ifmt == NULL) {
        ifmt = util_strdup("EQN");
    } else if (util_str_choice(ifmt, 7, "EQN", "PALASM", "EDIF", "TT", "ABEL", "LDF", "NSR") == 0) {
        util_report_error(FIT_ERR_PARAM_VALUE, "allowed values for -ifmt are EQN/PALASM/TTF/ABEL/LDF");
        g_exit_code = FIT_EXIT_USAGE;
        return 0;
    }
    in = file_open_read(file);
    if (in == NULL) {
        util_report_error(FIT_ERR_FILE_NOT_FOUND, file);
        g_exit_code = FIT_EXIT_INPUT_OPEN;
        return 0;
    }

    if (util_str_icmp(ifmt, "EDIF") == 0 && !edif_read(in, &entity, job)) {
        util_fclose(in);
        free(ifmt);
        g_exit_code = FIT_EXIT_INPUT_SYNTAX;
        return 0;
    }
    if (util_str_icmp(ifmt, "TT") == 0 && !tt2_read(in, &entity, job)) {
        g_exit_code = FIT_EXIT_INPUT_SYNTAX;
        return 0;
    }
    if (util_str_icmp(ifmt, "EQN") == 0)
        tt_not_supported("-str ifmt eqn");
    if (util_str_icmp(ifmt, "TT") != 0 && util_str_icmp(ifmt, "EDIF") != 0)
        /* PALASM, ABEL, LDF, NSR pass the check above but have no
         * reader. */
        tt_not_supported("this -str ifmt value");

    tt2_compact_entity(entity);         /* for every format but ABEL */

    free(ifmt);

    name = param_get(params, "d");
    if (name == NULL) {
        slot = 1;
        name = util_strdup(g_empty_str);
    } else {
        slot = entity_table_find(name, &type);
        if (slot == -1 && !entity_table_new_slot(&slot))
            return 0;
    }
    entity_table_store(slot, name, file, FIT_ENT_ENTITY, entity);
    tt2_print_entity_stats(g_msg, entity);
    util_fclose(in);
    return 1;
}

int input_entity_has_dc(FitEntity *entity)
{
    int i;

    for (i = 0; i < tt_count(entity->fcts); i++) {
        FitFct *fct = entity->fcts->items[i];
        FitFctTree *dc = fct->tree[2];

        if (dc == NULL)
            continue;
        if (dc->sel != FIT_SEL_CONST || dc->u.value == 1)
            return 1;
    }
    return 0;
}
