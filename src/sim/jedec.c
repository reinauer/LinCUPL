/*
 * sim_jedec - appending the simulated test vectors to the JEDEC file
 * of the design (option j).
 *
 * The fuse data of <design>.jed is copied to tmpcsim.im, any vectors it
 * already has are dropped, the new "V" fields are added, and at the end the
 * result is copied back with a new vector count and transmission checksum.
 *
 * Part of the CUPL simulator (see sim.c).
 */
#include "sim.h"
#include "jedsum.h"

#define JEDEC_MAX_PINS 254
#define STX 2
#define ETX 3

static char *g_jed_name;
static char *g_tmp_name;            /* tmpcsim.im */
static FILE *g_jed;
static FILE *g_tmp;
static unsigned g_checksum;         /* transmission checksum */
static int g_npins;                 /* pins of the device */
static char g_pins[JEDEC_MAX_PINS + 3]; /* one test condition per pin, 1-based */
static char g_text[JEDEC_MAX_PINS + 64]; /* field being written */

void jedec_reset(void)
{
    g_jed_name = g_tmp_name = NULL;
    g_jed = g_tmp = NULL;
    g_checksum = 0;
    g_npins = 0;
    g_pins[0] = '\0';
}

static void write_text(FILE *fp, const char *name, const char *text)
{
    g_checksum = jedsum_text(g_checksum, text);
    if (file_puts(text, fp) == EOF) {
        fatal_error(ERR_WRITE, name);
    }
}

static void check_write(FILE *fp, const char *name)
{
    fflush(fp);
    if (ferror(fp)) {
        fatal_error(ERR_WRITE, name);
    }
}

/* Mark the supply pins, which get 'N' in every vector. */
void jedec_find_power_pins(void)
{
    int count = dev_section_count(DEV_SEC_PINS);
    int power = 0;
    g_npins = (int)rd16(dev_header() + 8);
    if (g_npins > JEDEC_MAX_PINS) {
        g_npins = JEDEC_MAX_PINS;
    }
    memset(g_pins, 'X', (size_t)g_npins + 1);
    g_pins[g_npins + 1] = '\0';
    for (int i = 0; i < count; i++) {
        const uint8_t *pin = dev_pin(i);
        int kind = pin[PIN_KIND];
        int number = (int)rd16(pin + 2);
        char mark;
        if (kind != 3 && kind != 2 && kind != 4 && kind != 0x13 && (g_vsima || kind != 0x19)) {
            continue;
        }
        if (g_vsima) {
            mark = (dev_header()[0x12] == 4 && kind == 4) ? '1' : 'N';
        } else {
            mark = (kind == 0x19) ? '0' : 'N';
        }
        if (number <= g_npins + 1) {
            g_pins[number] = mark;
        }
        power++;
    }
    if (power < 2) {
        report_error(0x40d, "no power pins");
    }
}

/* Open the design's .jed file (named after the source with option n, else
 * as recorded in the .abs header) and create the temporary file. */
static void open_jedec_files(void)
{
    g_jed_name = make_file_name(g_dir_prefix, (g_options & OPT_N) ? g_base_name : g_abs_jedec_name, ".jed");
    g_tmp_name = make_file_name(g_dir_prefix, "tmpcsim", ".im");
    g_jed = open_file(g_jed_name, "rt");
    if (g_jed == NULL) {
        fatal_error(ERR_OPEN, g_jed_name);
    }
    g_tmp = open_file(g_tmp_name, "wt");
    if (g_tmp == NULL) {
        fatal_error(ERR_OPEN, g_tmp_name);
    }
}

/* After "QV": skip the rest of the field.  Returns 0 at end of file. */
static int skip_to_star(void)
{
    int c;
    do {
        c = read_char(g_jed);
        if (c == EOF) {
            return 0;
        }
    } while (c != '*');
    return 1;
}

/* Copy the JEDEC file up to its first P or V field (or its end), leaving
 * out QV fields.  Characters are written one behind so that the '*' or line
 * end in front of the cut can be judged. */
static void copy_fuse_data(void)
{
    int started = 0;        /* STX seen: characters count for the checksum */
    int line_after_star = 0;
    int prev = 0;
    for (;;) {
        int c = read_char(g_jed);
        if (c == EOF) {
            fatal_error(ERR_READ, g_jed_name);
        }
        switch (c) {
        case 'Q':
        case 'q':
            if (prev == '*' || prev == '\n') {
                int next = read_char(g_jed);
                if (next != 'v' && next != 'V') {
                    if (next != EOF) {
                        ungetc(next, g_jed);
                    }
                } else if (skip_to_star()) {
                    if (prev == '*') {
                        ungetc('*', g_jed);
                    }
                    prev = 0;
                    c = 0;
                }
            }
            line_after_star = 0;
            break;
        case STX:
            started = 1;
            break;
        case '\n':
            if (prev == '*') {
                line_after_star = 1;
            }
            break;
        case ETX:
        case 'P':
        case 'V':
        case 'p':
        case 'v':
            if (c == ETX || prev == '*' || line_after_star) {
                if (c != ETX || prev != 0) {
                    file_putc(prev, g_tmp);
                }
                if (started) {
                    g_checksum = jedsum_char(g_checksum, prev);
                }
                check_write(g_tmp, g_tmp_name);
                return;
            }
            break;
        default:
            line_after_star = 0;
            break;
        }
        if (prev != 0) {
            file_putc(prev, g_tmp);
        }
        if (prev == '\n') {
            check_write(g_tmp, g_tmp_name);
        }
        if (started) {
            g_checksum = jedsum_char(g_checksum, prev);
        }
        prev = c;
    }
}

/* The P field: pin numbers in the order of the vector columns. */
static void write_pin_list(void)
{
    char *text = alloc_tracked((size_t)g_npins * 12 + 16, "creating P field");
    char *p = text;
    p += sprintf(p, "P");
    for (int i = 1; i <= g_npins; i++) {
        p += sprintf(p, " %d", i);
    }
    sprintf(p, "\n*");
    write_text(g_tmp, g_tmp_name, text);
    free_tracked(text);
}

/* Start the vector output: copy the fuse data to the temporary file and
 * write the P field. */
void jedec_begin(void)
{
    jedec_find_power_pins();
    open_jedec_files();
    copy_fuse_data();
    write_pin_list();
}

/* The test conditions of the vector just simulated, by pin. */
static void fill_pin_conditions(void)
{
    g_pins[0] = ' ';
    for (int i = 0; i < g_order_count; i++) {
        const order_t *o = &g_order[i];
        int pin = o->pin;
        int swap01 = !g_vsima && !o->positive && !vector_is_preload();
        char c;
        if (pin <= 0 || pin > g_npins) {
            continue;
        }
        switch (o->result) {
        case T_VAL_H: c = o->positive ? 'H' : 'L'; break;
        case T_VAL_L: c = o->positive ? 'L' : 'H'; break;
        case T_VAL_Z: c = 'Z'; break;
        case T_VAL_X: c = 'X'; break;
        case T_VAL_P: c = 'P'; break;
        case T_VAL_C: c = 'C'; break;
        case T_VAL_K: c = 'K'; break;
        case T_VAL_N: c = 'N'; break;
        case T_VAL_0: c = swap01 ? '1' : '0'; break;
        case T_VAL_1: c = swap01 ? '0' : '1'; break;
        default: c = 'X'; break;
        }
        g_pins[pin] = c;
    }
    g_pins[g_npins + 1] = '\0';
}

/* Write the V field of the vector just simulated. */
void jedec_vector(void)
{
    fill_pin_conditions();
    snprintf(g_text, sizeof(g_text), "V%04d%s\n*", g_vector_number + 1, g_pins);
    write_text(g_tmp, g_tmp_name, g_text);
}

/* Copy the temporary file back over the JEDEC file, inserting the QV field
 * (number of vectors) in front of the G field, and update the transmission
 * checksum. */
static void copy_back(void)
{
    int started = 0;
    int star_seen = 0;
    int count_written = 0;      /* vsima: QV already written in front of an X field */
    int prev = 0;
    int c;
    while ((c = read_char(g_tmp)) != EOF) {
        if (c == '*') {
            if (started) {
                star_seen = 1;
            }
        } else if (g_vsima && (c == 'X' || c == 'x')) {
            /* vsima: the count also goes in front of the X field */
            if (star_seen && !count_written && (prev == '*' || prev == '\n' || prev == ' ')) {
                snprintf(g_text, sizeof(g_text), prev == '*' ? "*QV%d\n" : "\nQV%d*", g_vector_number);
                count_written = 1;
                write_text(g_jed, g_jed_name, g_text);
            }
        } else if (c == 'G' || c == 'g') {
            if (prev == '*' && star_seen) {
                snprintf(g_text, sizeof(g_text), "*QV%d\n", g_vector_number);
                write_text(g_jed, g_jed_name, g_text);
            } else if (prev == '\n' && star_seen) {
                snprintf(g_text, sizeof(g_text), "\nQV%d*", g_vector_number);
                write_text(g_jed, g_jed_name, g_text);
            }
        }
        if (prev == STX) {
            started = 1;
        }
        if (prev != 0) {
            file_putc(prev, g_jed);
        }
        if (started) {
            g_checksum = jedsum_char(g_checksum, prev);
        }
        if (prev == '\n') {
            check_write(g_jed, g_jed_name);
        }
        prev = c;
    }
}

/* Rewrite the JEDEC file from the temporary file and end it with ETX and
 * the transmission checksum. */
void jedec_finish(void)
{
    close_file(g_jed);
    close_file(g_tmp);
    g_jed = open_file(g_jed_name, "wt");
    if (g_jed == NULL) {
        fatal_error(ERR_WRITE, g_jed_name);
    }
    g_tmp = open_file(g_tmp_name, "rt");
    if (g_tmp == NULL) {
        fatal_error(ERR_WRITE, g_tmp_name);
    }
    g_checksum = 0;
    copy_back();
    g_checksum = jedsum_text(g_checksum, "*\x03");
    if (file_puts("*\x03", g_jed) == EOF) {
        fatal_error(ERR_WRITE, g_jed_name);
    }
    snprintf(g_text, sizeof(g_text), "%04X", (unsigned)g_checksum);
    if (file_puts(g_text, g_jed) == EOF) {
        fatal_error(ERR_WRITE, g_jed_name);
    }
    check_write(g_jed, g_jed_name);
    close_file(g_tmp);
    remove_file(g_tmp_name);
    close_file(g_jed);
    g_jed = g_tmp = NULL;
}
