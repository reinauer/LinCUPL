/* ATF150x primitives used to adapt CUPL netlists before placement. */
#include "lib_priv.h"

enum {
    CONTROL_RESET = 1 << 0,
    CONTROL_PRESET = 1 << 1,
    CONTROL_OUTPUT_ENABLE = 1 << 2,
    CONTROL_CLOCK_ENABLE = 1 << 3,
    CONTROL_COMBINATIONS = 1 << 4
};

static void add_gate(FitList *gates, int kind, void *cell)
{
    FitLibGate *gate = util_calloc(1, sizeof(*gate));

    gate->kind = kind;
    gate->cell = cell;
    list_add(gates, gate);
}

static void add_buffer(FitList *gates, int type)
{
    FitLibBuf *cell = util_calloc(1, sizeof(*cell));

    cell->type = type;
    cell->name = util_strdup(type == FIT_BUF_BI ? "bidirectional" : "tristate");
    cell->in = util_strdup("D");
    if (type == FIT_BUF_BI)
        cell->out = util_strdup("Q");
    cell->pad = util_strdup("PAD");
    cell->oe = util_strdup("OE");
    cell->oe_pol = 1;
    add_gate(gates, FIT_GATE_BUF, cell);
}

/* Mapping uses pin presence and polarity, not timing or area. Enumerate
 * optional controls for each output polarity so unused controls need no
 * tie-off logic. Combinational equations stay in the entity's functions;
 * they do not need standard-cell gates in this library. */
FitList *lib_builtin_atf150x(void)
{
    static const struct {
        int kind;
        int clock;
        const char *name;
    } registers[] = {
        { FIT_GATE_LATCH, FIT_CLK_L1, "latch" },
        { FIT_GATE_DFF, FIT_CLK_RECK, "dff" },
        { FIT_GATE_TFF, FIT_CLK_RECK, "tff" }
    };
    FitList *gates = list_create();
    FitLibSeq *tristate = util_calloc(1, sizeof(*tristate));

    add_buffer(gates, FIT_BUF_BI);
    add_buffer(gates, FIT_BUF_TS);
    tristate->name = util_strdup("tristate_output");
    tristate->d = util_strdup("D");
    tristate->oe = util_strdup("OE");
    tristate->oe_pol = 1;
    tristate->q = util_strdup("Q");
    add_gate(gates, FIT_GATE_TH, tristate);

    for (size_t kind = 0; kind < sizeof(registers) / sizeof(registers[0]); kind++) {
        for (int inverted = 0; inverted < 2; inverted++) {
            for (int controls = 0; controls < CONTROL_COMBINATIONS; controls++) {
                FitLibSeq *cell = util_calloc(1, sizeof(*cell));
                char name[64];

                snprintf(name, sizeof(name), "%s_%s_%d", registers[kind].name,
                         inverted ? "qn" : "q", controls);
                cell->name = util_strdup(name);
                cell->clk = util_strdup("CLK");
                cell->clk_kind = registers[kind].clock;
                cell->d = util_strdup("D");
                if (controls & CONTROL_RESET)
                    cell->ar = util_strdup("AR");
                if (controls & CONTROL_PRESET)
                    cell->ap = util_strdup("AP");
                if (controls & CONTROL_CLOCK_ENABLE)
                    cell->ce = util_strdup("CE");
                if (controls & CONTROL_OUTPUT_ENABLE) {
                    cell->oe = util_strdup("OE");
                    cell->z = util_strdup("Z");
                }
                cell->ar_pol = cell->ap_pol = cell->ce_pol = cell->oe_pol = 1;
                if (inverted)
                    cell->qn = util_strdup("QN");
                else
                    cell->q = util_strdup("Q");
                add_gate(gates, registers[kind].kind, cell);
            }
        }
    }
    return gates;
}
