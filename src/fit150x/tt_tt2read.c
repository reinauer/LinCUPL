/*
 * tt_tt2read.c - tt2_read, the reader proper: the Berkeley PLA file with
 * "#$" lines that cupl writes for the fitter devices (.tt2).
 *
 *   #$ TOOL CUPL             sets the CUPL flag
 *   #$ DEVICE name           stored as the package name (replaced later by -dev)
 *   #$ PROPERTY ATMEL ...    tt_tt2prop.c
 *   #$ PINS n name[+|-][:pin] ...      declared pins, '-' = active low,
 *   #$ NODES n name[+|-][:node] ...    ":pin" = preassignment
 *   #$ anything else, # comment        skipped up to the next keyword
 *   .i .o .type .ilb .ob .phase .p     header, each at most once
 *   cubes, then .e or .end
 */
#include "tt_priv.h"

int      g_tt2_next_id;         /* next variable id while reading */
FitList *g_tt2_node_decls;      /* TtNodeDecl of the names seen with + / - or in PINS / NODES */

/* keyword indices of the table installed by tt2_lex_setup */
enum {
    KW_I = 1, KW_O = 2, KW_TYPE = 3, KW_ILB = 4, KW_OB = 5, KW_P = 6, KW_E = 7,
    KW_PHASE = 8, KW_HASH = 9, KW_HASH_DOLLAR = 10, KW_PINS = 11, KW_NODES = 12,
    KW_END = 13, KW_PROPERTY = 14, KW_DEVICE = 15, KW_TOOL = 16, KW_PACKET = 17
};

/* item of the pin preassignment list g_preassign; the place
 * modules read the first two fields as FitPreassignItem */
typedef struct TtPreassign {
    FitPreassignItem base;      /* name, pin */
    int is_pin;                 /* 1 from "#$ PINS", 0 from "#$ NODES" */
} TtPreassign;

/* keyword, delimiter and extra character tables of the .tt2 syntax */
static void tt2_lex_setup(void)
{
    /* the extended mode knows "Packet" as well */
    lex_set_keyword_table(FIT_EXT ? 17 : 16,
                          KW_I, ".i", KW_O, ".o", KW_TYPE, ".type", KW_ILB, ".ilb",
                          KW_OB, ".ob", KW_P, ".p", KW_E, ".e", KW_PHASE, ".phase",
                          KW_HASH, "#", KW_HASH_DOLLAR, "#$", KW_PINS, "PINS",
                          KW_NODES, "NODES", KW_END, ".end", KW_PROPERTY, "PROPERTY",
                          KW_DEVICE, "DEVICE", KW_TOOL, "TOOL", KW_PACKET, "Packet");
    lex_set_default_tables();
    lex_set_comment_chars(0, 0);
    lex_set_delimiter_table(21, '[', ']', '{', '}', '+', '^', ')', '(', ':', ';', '!', '?',
                            '=', '<', '>', '*', ',', '"', '\'', '/', '-');
    lex_set_extra_char_table(8, '%', '_', -1, '#', '$', '&', '.', '~');
    g_tt2_next_id = 1;
    g_tt2_node_decls = NULL;
}

/* skip to the next token that is one of the keywords 1..10 (at least one
 * token is consumed) */
static void skip_to_keyword(void)
{
    do {
        lex_next_token();
    } while (g_lex_token < 1 || g_lex_token > 10);
}

/* 1 if every character of s is in the list of characters */
static int tt2_digits_allowed(const char *s, FitList *allowed)
{
    size_t i;

    if (s == NULL)
        return 0;
    for (i = 0; i < strlen(s); i++)
        if (list_find(allowed, FIT_INT2PTR((int)s[i]), list_ptr_eq) == 0)
            return 0;
    return 1;
}

/* n characters of a cube, which the scanner may deliver as
 * several tokens ('-' is a delimiter).  The current token is the first
 * piece; on return the current token is the last piece.  New string, or
 * NULL after an error message. */
static char *tt2_read_digits(unsigned n, FitList *allowed)
{
    char *s = NULL;

    if (n < strlen(g_lex_text)) {
        lex_error(1, "Separator expected", 0);
        return NULL;
    }
    if (!tt2_digits_allowed(g_lex_text, allowed)) {
        lex_error(1, "Illegal digit", 0);
        return NULL;
    }
    util_str_append(&s, g_lex_text);
    while (strlen(s) < n) {
        lex_next_token();
        if (g_lex_token == FIT_TOK_EOF) {
            lex_error(1, "Unexpected EOF", 0);
            free(s);
            return NULL;
        }
        if (n < strlen(s) + strlen(g_lex_text)) {
            lex_error(1, "Separator expected", 0);
            free(s);
            return NULL;
        }
        if (!tt2_digits_allowed(g_lex_text, allowed)) {
            lex_error(1, "Illegal digit", 0);
            free(s);
            return NULL;
        }
        util_str_append(&s, g_lex_text);
    }
    return s;
}

/* 1-based position of the declaration of `name`, 0 if none */
int tt2_find_node_decl(const char *name)
{
    return list_find(g_tt2_node_decls, (void *)name, tt2_name_ref_equal);
}

int tt2_name_ref_equal(void *item, void *key)
{
    return util_str_ieq(*(char **)item, key);
}

/* the declaration record of a name, created on first use */
static TtNodeDecl *tt2_node_decl(const char *name)
{
    TtNodeDecl *decl;
    int pos;

    if (g_tt2_node_decls == NULL)
        g_tt2_node_decls = list_create();
    pos = tt2_find_node_decl(name);
    if (pos != 0)
        return g_tt2_node_decls->items[pos - 1];
    decl = util_calloc(1, sizeof *decl);
    util_str_append(&decl->name, name);
    decl->active = 1;
    decl->declared = 1;
    list_add(g_tt2_node_decls, decl);
    return decl;
}

/* a name must start with a letter or one of _ $ & ~; 0 after an error
 * message */
static int tt2_check_variable(FitVariable *v)
{
    char msg[1000];
    int i;

    if (v == NULL)
        return 0;
    if (util_starts_alpha(v->name) || v->name[0] == '_' || v->name[0] == '$'
        || v->name[0] == '&' || v->name[0] == '~') {
        for (i = 0; i < tt_count(v->attrs); i++) {
            /* The current token is tested once per index entry, not the
             * entry itself.  Deliberate: it decides which files are
             * accepted. */
            if (!util_is_digits(g_lex_text)) {
                lex_error(1, "Incorrect Index", 0);
                return 0;
            }
        }
        return 1;
    }
    strcpy(msg, "Incorrect variable near ");
    strcat(msg, v->name);
    lex_error(1, msg, 0);
    return 0;
}

/* one name of an .ilb / .ob list.  The bracket forms name[i],
 * name[i:j] and name[i,j,...] are parsed (kinds 1 and 2) and rejected by
 * tt2_check_var_list.  NULL after an error. */
static FitVariable *tt2_read_variable(void)
{
    FitVariable *v = NULL;

    if (g_lex_token != FIT_TOK_IDENT) {
        lex_error(1, "Variable expected", 0);
        return NULL;
    }
    if (!g_lex_failed) {
        v = var_new(0);
        strcpy(v->name, g_lex_text);
        v->id = -1;
    }
    lex_next_token();
    if (g_lex_token == FIT_TOK_LBRACKET) {
        lex_next_token();
        if (!g_lex_failed) {
            /* a copy of the token, so that the list owns its items
             * (tt2_check_variable tests the current token instead) */
            v->attrs = list_create();
            list_add(v->attrs, util_strdup(g_lex_text));
        }
        lex_next_token();
        if (g_lex_token == FIT_TOK_COMMA) {
            if (!g_lex_failed)
                v->kind = 2;
            while (g_lex_token == FIT_TOK_COMMA) {
                lex_next_token();
                if (!g_lex_failed)
                    list_add(v->attrs, util_strdup(g_lex_text));
            }
        } else if (g_lex_token == FIT_TOK_COLON) {
            if (!g_lex_failed)
                v->kind = 1;
            lex_next_token();
            if (!g_lex_failed)
                list_add(v->attrs, util_strdup(g_lex_text));
        }
        lex_expect(FIT_TOK_RBRACKET);
    }
    if (!g_lex_failed) {
        if (!tt2_check_variable(v))
            g_lex_failed = 1;
        if (!g_lex_failed)
            return v;
    }
    free(v);
    return NULL;
}

/* can the current token start another name of the list */
static int tt2_token_starts_variable(const char *text, int token)
{
    if (token != FIT_TOK_IDENT)
        return 0;
    return util_starts_alpha(text) || text[0] == '$' || text[0] == '_' || text[0] == '~';
}

/* the "+" or "-" after a name of .ilb / .ob */
static void read_polarity_suffix(FitVariable *v)
{
    TtNodeDecl *decl;

    if (g_lex_text[0] != '-' && g_lex_text[0] != '+')
        return;
    if (v == NULL)
        tt_unreachable("read_polarity_suffix: polarity suffix after a rejected name");
    decl = tt2_node_decl(v->name);
    if (g_lex_text[0] == '-')
        decl->active = 2;
    lex_next_token();
}

/* the names after .ilb or .ob; NULL at end of file or after an
 * error */
static FitList *tt2_read_var_list(void)
{
    FitList *list = NULL;
    FitVariable *v;

    if (g_lex_token == FIT_TOK_EOF)
        return NULL;
    v = tt2_read_variable();
    if (!g_lex_failed) {
        list = list_create();
        list_add(list, v);
    }
    read_polarity_suffix(v);
    while (tt2_token_starts_variable(g_lex_text, g_lex_token)) {
        v = tt2_read_variable();
        if (!g_lex_failed && list != NULL)
            list_add(list, v);
        read_polarity_suffix(v);
    }
    if (g_lex_failed && list != NULL)
        list_destroy(&list);
    return list;
}

/* check the names, number them and reject duplicates */
static int tt2_check_var_list(FitList *list)
{
    int i;

    if (g_lex_failed)
        return 0;
    for (i = 0; i < tt_count(list); i++) {
        FitVariable *v = list->items[i];

        if (!tt2_check_variable(v))
            return 0;
        if (v->kind == 0) {
            v->id = g_tt2_next_id++;
        } else if (v->kind >= 1 && v->kind < 3) {
            lex_error(1, "Incorrect variable list (complex variables not supported)", 0);
            return 0;
        }
    }
    if (!list_all_distinct(list, var_same_name)) {
        lex_error(1, "Incorrect variable list (variable duplication)", 0);
        return 0;
    }
    return 1;
}

/* the per-output type string ('0'..'5') from the .type value: a
 * kind name applies to every output; anything else must be one digit per
 * output. */
static int tt2_type_string(const char *type, char **out, unsigned n_out)
{
    int kind = util_str_choice(type, 6, "f", "r", "fd", "fr", "dr", "fdr");
    char *s;

    if (kind == 0) {
        FitList *digits;
        int d, ok;

        if (strlen(type) != n_out) {
            lex_error(1, "Incorrect number of digits in the .type section", 0);
            return 0;
        }
        digits = list_create();
        for (d = 0; d < 6; d++)
            list_add(digits, FIT_INT2PTR('0' + d));
        /* the current token is checked, not the .type value.  Deliberate:
         * it decides which files are accepted. */
        ok = tt2_digits_allowed(g_lex_text, digits);
        list_destroy(&digits);
        if (!ok) {
            lex_error(1, "Illegal digits in the .type section", 0);
            return 0;
        }
        s = util_strdup(type);
    } else {
        s = util_calloc(1, (size_t)n_out + 1);
        if ((int)n_out > 0)
            memset(s, '/' + kind, n_out);
    }
    *out = s;
    return 1;
}

/* advances past a header keyword; an error if the section was seen
 * before */
static int tt2_section_once(int *seen, const char *name)
{
    char msg[1000];

    if (*seen == 0) {
        *seen = 1;
        lex_next_token();
        return 1;
    }
    sprintf(msg, "%s section must appear only once - Compilation aborted", name);
    lex_error(1, msg, 0);
    return 0;
}

/* the n entries "name[+|-][:number]" of a "#$ PINS" (is_pin 1)
 * or "#$ NODES" (0) line */
static int tt2_read_pin_list(int n, int is_pin)
{
    int i;

    for (i = 0; i < n; i++) {
        TtNodeDecl *decl;

        if (g_lex_token != FIT_TOK_IDENT)
            return 0;
        decl = tt2_node_decl(g_lex_text);
        decl->declared = is_pin;
        lex_next_token();
        if (g_lex_text[0] == '+') {
            lex_next_token();
        } else if (g_lex_text[0] == '-') {
            decl->active = 2;
            lex_next_token();
        }
        if (g_lex_text[0] == ':') {
            lex_next_token();
            if (util_str_icmp(g_lex_text, "0") != 0) {
                TtPreassign *item = util_calloc(1, sizeof *item);

                item->base.pin = atoi(g_lex_text);
                item->base.name = util_strdup(decl->name);
                item->is_pin = is_pin;
                list_add(g_preassign, item);
            }
            lex_next_token();
        }
    }
    return 1;
}

/* everything up to the first cube.  the extended mode adds the PROPERTY lines to
 * the job parameters `job`.  Returns 0 after an error. */
static int tt2_read_header(unsigned *n_in, unsigned *n_out, char **type, unsigned *n_p,
                           FitList **ilb, FitList **ob, char **phase, FitList *job)
{
    char type_text[FIT_NAME_MAX];
    char phase_text[sizeof g_lex_long_text];
    int seen_i = 0, seen_o = 0, seen_p = 0, seen_ilb = 0, seen_ob = 0;
    int seen_type = 0, seen_phase = 0;
    unsigned n;

    *n_in = 0xffffffffu;
    *n_out = 0xffffffffu;
    *type = NULL;
    *n_p = 0xffffffffu;
    *ilb = NULL;
    *ob = NULL;
    *phase = NULL;
    type_text[0] = '\0';
    phase_text[0] = '\0';

    for (;;) {
        switch (g_lex_token) {
        case KW_I:
            if (!tt2_section_once(&seen_i, ".i"))
                return 0;
            if (!util_str_to_uint(n_in, g_lex_text)) {
                lex_error(1, "Integer expected after .i - Compilation aborted", 0);
                return 0;
            }
            lex_next_token();
            continue;
        case KW_O:
            if (!tt2_section_once(&seen_o, ".o"))
                return 0;
            if (!util_str_to_uint(n_out, g_lex_text)) {
                lex_error(1, "Integer expected after .o - Compilation aborted", 0);
                return 0;
            }
            lex_next_token();
            continue;
        case KW_TYPE:
            if (!tt2_section_once(&seen_type, ".type"))
                return 0;
            strcpy(type_text, g_lex_text);
            lex_next_token();
            continue;
        case KW_ILB:
            if (!tt2_section_once(&seen_ilb, ".ilb"))
                return 0;
            *ilb = tt2_read_var_list();
            if (!tt2_check_var_list(*ilb))
                g_lex_failed = 1;
            break;
        case KW_OB:
            if (!tt2_section_once(&seen_ob, ".ob"))
                return 0;
            *ob = tt2_read_var_list();
            if (!tt2_check_var_list(*ob))
                g_lex_failed = 1;
            break;
        case KW_P:
            if (!tt2_section_once(&seen_p, ".p"))
                return 0;
            if (!util_str_to_uint(n_p, g_lex_text)) {
                lex_error(1, "Integer expected after .p - Compilation aborted", 0);
                return 0;
            }
            lex_next_token();
            continue;
        case KW_PHASE:
            if (!tt2_section_once(&seen_phase, ".phase"))
                return 0;
            if (!seen_o) {
                lex_error(1, ".o is required before .phase", 0);
                return 0;
            }
            strcpy(phase_text, g_lex_long_text);
            lex_next_token();
            continue;
        case KW_HASH:
            skip_to_keyword();
            continue;
        case KW_HASH_DOLLAR:
            lex_next_token();
            switch (g_lex_token) {
            case KW_PINS:
            case KW_NODES: {
                int is_pin = g_lex_token == KW_PINS;

                lex_next_token();
                if (!util_str_to_uint(&n, g_lex_text))
                    return 0;
                lex_next_token();
                if (!tt2_read_pin_list((int)n, is_pin))
                    return 0;
                break;
            }
            case KW_PROPERTY:
                lex_next_token();
                if (FIT_EXT)
                    tt2_property_param(job);
                else
                    tt2_property();
                break;
            case KW_DEVICE:
                /* the extended mode takes the technology from the device name and
                 * the package from "Packet" */
                lex_next_token();
                if (FIT_EXT)
                    g_tech_name = util_strdup(g_lex_text);
                else
                    g_dev_package = util_strdup(g_lex_text);
                lex_next_token();
                break;
            case KW_PACKET:
                lex_next_token();
                g_dev_package = util_strdup(g_lex_text);
                lex_next_token();
                break;
            case KW_TOOL:
                lex_next_token();
                if (util_str_icmp(g_lex_text, "CUPL") == 0)
                    g_opt.cupl = 1;
                skip_to_keyword();
                break;
            default:
                skip_to_keyword();
                break;
            }
            continue;
        default:
            if (*n_in == 0xffffffffu || *n_out == 0xffffffffu) {
                lex_error(1, ".i and .o sections are required", 0);
                return 0;
            }
            if (*ilb != NULL && (unsigned)(*ilb)->count != *n_in) {
                lex_error(1, "Mismatch between .i and .ilb sections", 0);
                return 0;
            }
            if (*ob != NULL && (unsigned)(*ob)->count != *n_out) {
                lex_error(1, "Mismatch between .o and .ob sections", 0);
                return 0;
            }
            if (!tt2_type_string(seen_type ? type_text : "fd", type, *n_out))
                return 0;
            if (!seen_phase)
                return 1;
            util_str_append(phase, phase_text);
            if (strlen(*phase) == *n_out)
                return 1;
            lex_error(1, "Bad number of characteres after .phase", 0);
            return 0;
        }
        /* after .ilb / .ob */
        if (g_lex_failed)
            return 0;
    }
}

/* an input that has the name of an output is the pin feedback of
 * that output: it becomes "<name>.PIN". */
static int tt2_mark_pin_inputs(FitList *ilb, FitList *ob)
{
    int ok = 1;
    int i;

    if (ilb == NULL)
        return 1;
    for (i = 0; i < ilb->count; i++) {
        FitVariable *v = ilb->items[i];
        char *pin_name;

        if (var_list_find_name(ob, v->name) == NULL)
            continue;
        pin_name = util_strdup(v->name);
        util_str_append(&pin_name, ".PIN");
        if (var_list_find_name(ob, pin_name) != NULL
            || var_list_find_name(ilb, pin_name) != NULL) {
            fprintf(g_msg, "Error - Variable %s can not be in .ilb and .olb\n", v->name);
            ok = 0;
        } else {
            util_str_cat_n(v->name, ".PIN", FIT_NAME_MAX);
        }
        free(pin_name);
    }
    return ok;
}

int tt2_read(FILE *in, FitEntity **entity, FitList *job)
{
    unsigned n_in, n_out, n_p;
    char *type, *phase;
    FitList *ilb, *ob;
    FitList *pla, *converted, *in_digits, *out_digits, *dotted;
    FitEntity *e;
    int nw_all, nw_out, next_id, status;
    size_t u;

    lex_set_streams(in, g_msg);
    tt2_lex_setup();
    lex_start();
    lex_next_token();
    *entity = NULL;

    if (!tt2_read_header(&n_in, &n_out, &type, &n_p, &ilb, &ob, &phase, job))
        return 0;
    if (!tt2_mark_pin_inputs(ilb, ob))
        return 0;
    nw_all = tt_words((int)(n_in + n_out));
    nw_out = tt_words((int)n_out);
    e = entity_new(NULL);

    /* In the cubes '~' separates tokens; with 256 or more columns every
     * digit is a token of its own (the token buffer holds 255 characters). */
    if ((int)n_in < 0x100 && (int)n_out < 0x100)
        lex_set_delimiter_table(22, '[', ']', '{', '}', '+', '^', ')', '(', ':', ';', '!',
                                '?', '=', '<', '>', '*', ',', '"', '\'', '/', '-', '~');
    else
        lex_set_delimiter_table(24, '[', ']', '{', '}', '+', '^', ')', '(', ':', ';', '!',
                                '?', '=', '<', '>', '*', ',', '"', '\'', '/', '-', '~',
                                '0', '1');

    pla = list_create();
    in_digits = list_create();
    list_add(in_digits, FIT_INT2PTR('0'));
    list_add(in_digits, FIT_INT2PTR('1'));
    list_add(in_digits, FIT_INT2PTR('-'));
    out_digits = list_create();
    list_add(out_digits, FIT_INT2PTR('0'));
    list_add(out_digits, FIT_INT2PTR('1'));
    list_add(out_digits, FIT_INT2PTR('-'));
    list_add(out_digits, FIT_INT2PTR('~'));

    if (g_lex_token != KW_E) {
        while (g_lex_token != KW_END && g_lex_token != FIT_TOK_EOF) {
            char *in_str = NULL;
            char *out_str, *row;
            FitPTermIO *io;

            if (n_in != 0) {
                in_str = tt2_read_digits(n_in, in_digits);
                if (in_str == NULL)
                    return 0;
            }
            /* the input part of a cube covers the outputs (all '-') and
             * then the inputs */
            row = util_calloc(1, (size_t)n_in + 1 + n_out);
            if ((int)n_out > 0)
                memset(row, '-', n_out);
            if (n_in != 0) {
                memcpy(row + n_out, in_str, (size_t)n_in + 1);
                lex_next_token();
            }
            out_str = tt2_read_digits(n_out, out_digits);
            if (out_str == NULL)
                return 0;
            lex_next_token();
            io = ptermio_new(nw_all, nw_out);
            ptermio_build(io, row, out_str, n_in + n_out, n_out);
            list_add(pla, io);
            free(in_str);
            free(row);
            free(out_str);
            if (g_lex_token == KW_E)
                break;
        }
        if (g_lex_token != KW_E && g_lex_token != KW_END) {
            lex_error(1, ".e expected", 0);
            return 0;
        }
    }
    lex_next_token();
    if (g_lex_token != FIT_TOK_EOF) {
        lex_error(1, "EOF expected", 0);
        return 0;
    }
    if (n_p != 0xffffffffu && n_p != (unsigned)tt_count(pla)) {
        lex_error(1, "Mismatch between the .p and the number of cubes", 0);
        fprintf(g_msg, "Error -  Found %d product terms instead of %d\n", tt_count(pla),
                (int)n_p);
        return 0;
    }

    e->n_in = (int)n_in;
    e->n_out = (int)n_out;
    e->n_local = 0;
    next_id = 1;
    e->inputs = ilb != NULL ? ilb
                            : vars_new_generated(e->n_in, &next_id, 0, 1, NULL);
    e->outputs = ob != NULL ? ob
                            : vars_new_generated(e->n_out, &next_id, 1, 1, NULL);
    e->locals = list_create();
    list_destroy(&out_digits);
    list_destroy(&in_digits);

    converted = tt2_convert_pla_type(pla, (int)(n_in + n_out), (int)n_out, type, &status);
    list_free_all(&pla, tt_cb_free_ptermio);
    tt2_report_set_conflict(g_msg, status);
    if (converted == NULL) {
        lex_error(1, "Unable to convert to the default .type", 0);
        entity_free(&e);
        return 0;
    }
    free(type);
    tt2_fct_from_pla(converted, e);
    list_free_all(&converted, tt_cb_free_ptermio);

    /* .phase: '0' inverts the output.  The id is computed from the
     * position, which assumes that .ilb came before .ob. */
    if (phase != NULL) {
        for (u = 0; u < strlen(phase); u++)
            if (phase[u] == '0')
                tt2_invert_fct(e, e->n_in + 1 + (int)u);
        free(phase);
    }

    /* names with an extension: XOR pairs, pin feedbacks, registers */
    dotted = tt2_collect_dotted(e);
    if (dotted != NULL) {
        tt2_build_xors(e, dotted);
        dotted = tt2_collect_dotted(e);     /* the first list is not released */
        if (dotted != NULL) {
            tt2_merge_pin_inputs(e, dotted);
            if (!tt2_build_instances(e, dotted))
                return 0;
        }
    }
    if (!tt2_apply_node_decls(e)) {
        entity_free(&e);
        return 0;
    }
    tt2_delete_unused_cycles(e);
    *entity = e;
    return 1;
}
