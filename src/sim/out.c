/*
 * sim_out - formatting of test vectors, trace lines and $MSG texts
 * for the listing (.so), the waveform file (.wo) and the console.
 *
 * Part of the CUPL simulator (see sim.c).
 */
#include "sim.h"

#define OUT_LINE_MAX 0x400

/* character shown for a logic value (LV_*) in trace lines */
static const char k_level_chars[] = "??01C?XZ";

static void check_list_write(void)
{
    fflush(g_list_file);
    if (ferror(g_list_file)) {
        fatal_error(ERR_WRITE, g_list_name);
    }
}

int value_token_char(int token)
{
    switch (token) {
    case T_VAL_H: return 'H';
    case T_VAL_L: return 'L';
    case T_VAL_Z: return 'Z';
    case T_VAL_X: return 'X';
    case T_VAL_P: return 'P';
    case T_VAL_C: return 'C';
    case T_VAL_K: return 'K';
    case T_VAL_N: return 'N';
    case T_VAL_0: return '0';
    case T_VAL_1: return '1';
    case T_VAL_STAR: return '*';
    case T_VAL_R: return 'R';
    default: return 0;
    }
}

static void begin_line(void)
{
    memset(g_name_buf, ' ', OUT_LINE_MAX);
    g_name_buf[OUT_LINE_MAX] = '\0';
    memset(g_out_text, ' ', OUT_LINE_MAX);
    g_out_text[OUT_LINE_MAX] = '\0';
    g_out_pos = 0;
    g_out_text_pos = 0;
}

static void put_column(int ch)
{
    if (g_out_pos < (int)sizeof(g_name_buf) - 1) {
        g_name_buf[g_out_pos] = (char)ch;
    }
    g_out_pos++;
    if (g_out_text_pos < (int)sizeof(g_out_text) - 1) {
        g_out_text[g_out_text_pos] = (char)ch;
    }
    g_out_text_pos++;
}

static void end_line(void)
{
    if (g_out_pos < (int)sizeof(g_name_buf)) {
        g_name_buf[g_out_pos] = '\0';
    }
    if (g_out_text_pos < (int)sizeof(g_out_text)) {
        g_out_text[g_out_text_pos] = '\0';
    }
}

/* Copy the quoted texts ("...") and apply the spacings (%n) of a list to
 * the output line. */
static void emit_texts(const list_t *list)
{
    for (const list_node_t *n = list->head; n != NULL; n = n->next) {
        const char *text = n->u.text;
        if (text == NULL || text[0] == '\0') {
            continue;
        }
        if (n->kind == 0x43) {
            int len = (int)strlen(text);
            if (g_out_pos + len > OUT_LINE_MAX) {
                report_error(ERR_OUTPUT_FORMAT, text);
            }
            if (g_out_pos >= 0 && g_out_pos + len < (int)sizeof(g_name_buf) - 1) {
                memcpy(g_name_buf + g_out_pos, text, (size_t)len);
                g_name_buf[g_out_pos + len] = ' ';
            }
            g_out_pos += len;
        } else if (n->kind == 0x44) {
            g_out_pos += (int)strtol(text, NULL, 10);
            if (g_out_pos > OUT_LINE_MAX) {
                report_error(ERR_OUTPUT_FORMAT, text);
            }
        } else {
            report_error(ERR_OUTPUT_FORMAT, text);
        }
    }
}

/* $MSG / $EXIT text, to the listing and (option v) the console */
void print_message(list_t *list)
{
    memset(g_name_buf, ' ', OUT_LINE_MAX);
    g_name_buf[OUT_LINE_MAX] = '\0';
    g_out_pos = 0;
    emit_texts(list);
    if (g_out_pos >= 0 && g_out_pos < (int)sizeof(g_name_buf)) {
        g_name_buf[g_out_pos] = '\0';
    }
    if (g_list_file != NULL) {
        stream_printf(g_list_file, "%s\n", g_name_buf);
        check_list_write();
    }
    if (g_options & OPT_CONSOLE) {
        stream_printf(stdout, "%s\n", g_name_buf);
    }
}

/* title of a trace section */
void print_trace_title(const char *text)
{
    if (g_options & OPT_CONSOLE) {
        stream_printf(stdout, "%s\n", text);
    }
    if (g_list_file != NULL) {
        stream_printf(g_list_file, "%s\n", text);
        check_list_write();
    }
}

/* "----> " and the names of the symbols in a product term. */
static void print_term_symbols(FILE *fp, const uint16_t *plane)
{
    stream_printf(fp, "----> ");
    for (int i = 0; i < g_nsyms; i++) {
        if (bit_test(plane + 1, i)) {
            if (g_syms[i].type[0] == '\0') {
                stream_printf(fp, "%s ", g_syms[i].name);
            } else {
                stream_printf(fp, "%s.%s ", g_syms[i].name, g_syms[i].type);
            }
        }
    }
    if (g_vsima) {
        stream_printf(fp, "\n");
    } else {
        file_putc('\n', fp);
    }
    fflush(fp);
}

/* One trace line: the ORDER columns as seen by a product term (regs == 0)
 * or the register states (regs == 1). */
static void print_trace_to(FILE *fp, const uint16_t *term, const uint16_t *used, const uint16_t *inverted,
                           int value, int regs)
{
    const order_t *o = g_order;
    begin_line();
    for (int i = 0; i < g_order_count; i++, o++) {
        int idx, ch;
        if (g_out_pos > OUT_LINE_MAX - 1) {
            report_error(ERR_OUTPUT_FORMAT, "line too long");
            break;
        }
        emit_texts(&o->before);
        g_order[i].text_col = (int16_t)g_out_pos;
        idx = o->sym_index;
        if (regs == 0) {
            if (!bit_test(used + 1, idx)) {
                ch = '-';
            } else {
                ch = k_level_chars[get_input_value(idx, bit_test(inverted + 1, idx)) & 7];
            }
        } else if (regs == 1) {
            ch = k_level_chars[g_reg_state[idx] & 7];
        } else {
            ch = '?';
        }
        put_column(ch);
    }
    emit_texts(&o->before);
    end_line();
    if (regs == 0) {
        print_term_symbols(fp, term);
        stream_printf(fp, "(%c)   %s\n", k_level_chars[value & 7], g_name_buf);
    } else if (regs == 1) {
        stream_printf(fp, "REGS  %s\n", g_name_buf);
    }
}

/* Write a trace line to the console (option v) and the listing. */
void print_trace_line(const uint16_t *term, const uint16_t *used, const uint16_t *inverted, int value, int regs)
{
    if (g_options & OPT_CONSOLE) {
        print_trace_to(stdout, term, used, inverted, value, regs);
    }
    if (g_list_file != NULL) {
        print_trace_to(g_list_file, term, used, inverted, value, regs);
    }
}

/* Format the result of a vector.  `quiet` only lays out the columns;
 * pass < 0 prints the final numbered line with its messages, otherwise an
 * intermediate (trace) line. */
void print_vector(int quiet, int pass)
{
    const order_t *o = g_order;
    begin_line();
    for (int i = 0; i < g_order_count; i++, o++) {
        int token, ch;
        if (g_out_pos > OUT_LINE_MAX) {
            report_error(ERR_OUTPUT_FORMAT, "line too long");
            break;
        }
        emit_texts(&o->before);
        token = (int8_t)o->result;
        if (token == T_VAL_R && g_vec_level[o->sym_index] != LV_UNKNOWN) {
            token = (g_vec_level[o->sym_index] != LV_LOW) ? T_VAL_1 : T_VAL_0;
        }
        g_order[i].text_col = (int16_t)g_out_pos;
        ch = value_token_char(token);
        put_column(ch != 0 ? ch : '?');
    }
    emit_texts(&o->before);
    end_line();
    if (quiet) {
        return;
    }
    if (pass < 0) {
        if (g_options & OPT_LISTING) {
            stream_printf(g_list_file, "%04d: %s\n", g_vector_number + 1, g_name_buf);
            check_list_write();
            stream_printf(g_wave_file, "#V %04d %s\n", g_vector_number + 1, g_out_text);
            fflush(g_wave_file);
            if (ferror(g_wave_file)) {
                fatal_error(ERR_WRITE, g_wave_name);
            }
        }
        if (g_options & OPT_CONSOLE) {
            stream_printf(stdout, "%04d: %s\n", g_vector_number + 1, g_name_buf);
        }
        if (queued_error_count() != 0) {
            if (g_options & OPT_LISTING) {
                print_queued_errors(g_list_file, 0);
                print_queued_errors(g_wave_file, 1);
            }
            if (g_options & OPT_CONSOLE) {
                print_queued_errors(stdout, 0);
            }
            clear_queued_errors();
        }
    } else if (g_trace_level != 0) {
        if (g_options & OPT_LISTING) {
            stream_printf(g_list_file, "      %s\n", g_name_buf);
            check_list_write();
        }
        if (g_options & OPT_CONSOLE) {
            stream_printf(stdout, "      %s\n", g_name_buf);
        }
    }
}
