/*
 * base_util.c - utilities of the ATF150x fitter: file helpers and temporary
 * names, error messages, checked allocation, checked file open/close,
 * command parameters, strings and timers.
 *
 * Helpers needed only by the EQN / EDIF readers or the simulation writers
 * are not implemented.
 *
 * Files are opened through lincupl_fopen_ci, so backslashes in paths become
 * slashes and existing files are found whatever the case of their name.
 * Bytes >= 0x80 belong to no character class.
 */
#include "locate.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "ascii.h"
#include "base_int.h"

FILE *g_msg;
int   g_exit_code;
jmp_buf g_job_jmp;
FitList *g_open_files;
char  g_empty_str[1];
char *g_tmp_prefix;

static int      g_tmp_counter;      /* number of temporary names handed out */
static FitList *g_tmp_names;        /* names to delete at exit */
static FILE    *g_echo_stream;
static FitList *g_final_list;       /* list released by util_final_cleanup; nothing adds to it */
static int      g_echo_lock;
static int      g_echo_lock_saved;
static int      g_echo_match = 1;
static int      g_job_flag;         /* cleared by util_job_init, never read */
static time_t   g_time0;            /* first time() seen by the timers */
static int      g_session;          /* util_session_number, 0 until first used */
static int      g_timer_mode;       /* 0: print nothing */
static double   g_session_elapsed;
static double   g_session_cpu;
static double   g_command_cpu;
static double   g_command_elapsed;
static double   g_timer_start;

/* ------------------------------------------------------------------ */
/* file helpers                                                        */
/* ------------------------------------------------------------------ */

/* fopen with backslashes taken as slashes and file names matched
 * without regard to case */
FILE *util_fopen_raw(const char *path, const char *mode)
{
    return lincupl_fopen_ci(path, mode);
}

/* Text-mode read of one character: CR LF becomes LF, Ctrl-Z ends the file. */
int file_getc_text(FILE *f)
{
    int c = getc(f);

    if (c == '\r') {
        int d = getc(f);

        if (d == '\n')
            return d;
        if (d != EOF)
            ungetc(d, f);
        return c;
    }
    if (c == 0x1a) {
        ungetc(c, f);
        return EOF;
    }
    return c;
}

/* fopen(path, "r"); NULL for a NULL path */
FILE *file_open_read(const char *path)
{
    if (path == NULL)
        return NULL;
    return util_fopen_raw(path, "r");
}

/* fopen(path, "w"); NULL for a NULL path */
FILE *file_open_write(const char *path)
{
    if (path == NULL)
        return NULL;
    return util_fopen_raw(path, "w");
}

/* can the file be opened for reading (mode r/R) or writing (w/W)?
 * Probing with a write mode creates or truncates the file. */
int file_accessible(const char *path, const char *mode)
{
    FILE *f;

    if (path == NULL || mode == NULL)
        return 0;
    if (util_str_icmp(mode, "r") == 0 || util_str_icmp(mode, "R") == 0) {
        f = file_open_read(path);
    } else if (util_str_icmp(mode, "w") == 0 || util_str_icmp(mode, "W") == 0) {
        f = file_open_write(path);
    } else {
        return 0;
    }
    if (f == NULL)
        return 0;
    util_fclose(f);
    return 1;
}

static void file_remove_if_exists(const char *path)
{
    if (file_accessible(path, "r") != 0)
        remove(path);
}

/* a number between 1 and 1000 derived from the time of the first
 * call; part of the temporary file names. */
static int util_session_number(void)
{
    if (g_session == 0) {
        long t = (long)time(NULL);

        g_session = (int)(((t % 100000) / 10) % 1000) + 1;
    }
    return g_session;
}

/* a new name "<prefix>a<session><counter>.tmp"; the name is
 * remembered and the file deleted by tmp_cleanup. */
char *tmp_log_name(void)
{
    char *name = util_calloc(1, 0x100);

    snprintf(name, 0x100, "%sa%04d%03d.tmp", g_tmp_prefix, util_session_number(), g_tmp_counter % 1000);
    g_tmp_counter++;
    if (g_tmp_names == NULL)
        g_tmp_names = list_create();
    list_add(g_tmp_names, util_strdup(name));
    return name;
}

/* close the registered files, delete the temporary files.
 * Closing a file moves the last registered file into its slot
 * (list_del_insert), so the loop below closes only every second one; the
 * others are closed when the process ends. */
void tmp_cleanup(void)
{
    char *name;

    for (int i = 0; i < (g_open_files == NULL ? 0 : g_open_files->count); i++)
        util_fclose(g_open_files->items[i]);
    for (int i = 0; i < (g_tmp_names == NULL ? 0 : g_tmp_names->count); i++) {
        const char *tmp = g_tmp_names->items[i];

        if (file_accessible(tmp, "r") != 0)
            file_remove_if_exists(tmp);
    }
    list_free_all(&g_tmp_names, util_free_ptr);
    g_tmp_names = NULL;
    name = util_calloc(1, 0x100);
    snprintf(name, 0x100, "%sasyl%d.ren", g_tmp_prefix, util_session_number());
    if (file_accessible(name, "r") != 0)
        file_remove_if_exists(name);
    free(name);
}

/* ------------------------------------------------------------------ */
/* messages, allocation, files                                         */
/* ------------------------------------------------------------------ */

/* prints the message for error `code` on g_msg; an unknown code is fatal
 * and aborts the job with exit code 99 */
void util_report_error(int code, const char *text)
{
    switch (code) {
    case 0:    fprintf(g_msg, "Error -  %s\n", text); return;
    case 1:    fprintf(g_msg, "Error - Undefined command  ('Help' for help): %s\n", text); return;
    case 3:    fprintf(g_msg, "Error - Illegal command usage : %s\n", text); return;
    case 4:    fprintf(g_msg, "Error - File not found : %s\n", text); return;
    case 5:    fprintf(g_msg, "Error - Too many entities defined %s\n", ""); return;
    case 6:    fprintf(g_msg, "Error - Entity type mismatch for requested function : %s\n", text); return;
    case 7:    fprintf(g_msg, "Error - Entity not found : %s\n", text); return;
    case 8:    fprintf(g_msg, "Error - Unknown parameter value : %s\n", text); return;
    case 9:
        fprintf(g_msg, "Error - Memory Full (%s)\n", text);
        fprintf(g_msg, "Error - Please abort session - Results will be incorrect%s", "");
        return;
    case 10:   fprintf(g_msg, "Error - Bad Index In DomList (%s)\n", text); return;
    case 0xb:  fprintf(g_msg, "Error - Bad Alias File (%s)\n", text); return;
    case 0xc:  fprintf(g_msg, "Internal Error (%s) - current result may be incorrect\n", text); return;
    case 0xd:  fprintf(g_msg, "Sorry but you are not allowed to use this command !\n"); return;
    case 0xe:  fprintf(g_msg, "Internal Warning (%s)\n", text); return;
    case 0xf:  fprintf(g_msg, "Unlicensed item (Sorry, but your license does not include support for %s)\n", text); return;
    case 0x10: fprintf(g_msg, "\nSomething wrong when opening logfile : %s\n", text); return;
    case 0x12: fprintf(g_msg, "Overflow unsigned integer : %s\n", text); return;
    case 0x13: fprintf(g_msg, "Not an unsigned integer : %s\n", text); return;
    case 0x14: fprintf(g_msg, "Negative Index in : %s\n", text); return;
    case 0x15: fprintf(g_msg, "Incorrect indices in : %s\n", text); return;
    case 0x16: fprintf(g_msg, "ERROR : StrSafeCatPX (Str2 is NULL)%s\n", text); return;
    case 0x17: fprintf(g_msg, "        Result may be incorrect%s\n", text); return;
    case 0x18: fprintf(g_msg, "Name truncation in StrSafeCatTX : %s\n", text); return;
    case 0x19: fprintf(g_msg, "ERROR : StrSafeCatTX (Str1 is NULL)%s\n", text); return;
    case 0x1a: fprintf(g_msg, "\nCan not Open File : %s !\n", text); return;
    case 0x1b: fprintf(g_msg, "\nCan not Close File : %s !\n", text); return;
    case 0x1c: fprintf(g_msg, "Error in format specification of message : %s\n", text); return;
    default:
        fprintf(g_msg, "Fatal Error -  %s\n", text);
        util_abort_job(99);
    }
}

/* free a two-word record whose second word is an allocation */
static void util_free_pair(void **p)
{
    if (*p != NULL) {
        void **rec = *p;

        if (rec[1] != NULL)
            free(rec[1]);
        free(*p);
        *p = NULL;
    }
}

/* the echo stream: noted and compared, never written to */
void util_echo_set(FILE *f)
{
    if (g_echo_lock == 0)
        g_echo_stream = f;
}

void util_echo_off(void)
{
    g_echo_lock_saved = g_echo_lock;
    g_echo_lock = 0;
}

void util_echo_restore(void)
{
    g_echo_lock = g_echo_lock_saved;
}

/* 1 if f is the echo stream; the answer is kept for util_echo_was_stream */
int util_echo_note_stream(FILE *f)
{
    g_echo_match = f == g_echo_stream;
    return g_echo_match;
}

int util_echo_was_stream(void)
{
    return g_echo_match;
}

void util_final_cleanup(void)
{
    list_free_all(&g_final_list, util_free_pair);
}

void util_job_init(void)
{
    g_job_flag = 0;
    (void)g_job_flag;
}

static void util_memory_full(void)
{
    fprintf(g_msg, "\nMemory Full\n");
    g_exit_code = FIT_EXIT_MEMORY;
    longjmp(g_job_jmp, FIT_JUMP_MEMORY);
}

/* calloc that never returns NULL: on failure "Memory Full", exit code 3
 * and a longjmp to the job */
void *util_calloc(size_t n, size_t size)
{
    void *p;

    /* calloc(0, x) may return NULL; ask for at least one byte */
    if (n == 0)
        n = 1;
    if (size == 0)
        size = 1;
    p = calloc(n, size);
    if (p == NULL)
        util_memory_full();
    return p;
}

/* like util_calloc for one block; zero-filled so that nothing ever
 * depends on what a fresh block contains. */
void *util_malloc(size_t size)
{
    void *p = calloc(1, size == 0 ? 1 : size);

    if (p == NULL)
        util_memory_full();
    return p;
}

void *util_realloc(void *p, size_t size)
{
    void *q = realloc(p, size == 0 ? 1 : size);

    if (q == NULL)
        util_memory_full();
    return q;
}

/* opens and registers a file; on failure "Can not Open File", exit code 5
 * and a longjmp to the job */
FILE *util_fopen_checked(const char *path, const char *mode)
{
    FILE *f = util_fopen_raw(path, mode);

    if (f == NULL) {
        fprintf(g_msg, "\nCan not Open File : %s !\n", path);
        g_exit_code = FIT_EXIT_FILE_OPEN;
        longjmp(g_job_jmp, FIT_JUMP_FILE);
    }
    list_add(g_open_files, f);
    return f;
}

/* opens and registers a file; NULL on failure */
FILE *util_fopen(const char *path, const char *mode)
{
    FILE *f = util_fopen_raw(path, mode);

    if (f != NULL)
        list_add(g_open_files, f);
    return f;
}

/* unregisters and closes a file */
int util_fclose(FILE *f)
{
    int pos = list_find(g_open_files, f, list_ptr_eq);
    int r;

    if (pos != 0)
        list_del_insert(g_open_files, pos);
    r = fclose(f);
    if (r != 0) {
        /* the message has room for two status words of the stream;
         * both are printed as 0 */
        fprintf(g_msg, "\nCan not Close File [%d/%d]!\n", 0, 0);
    }
    return r;
}

/* sets the exit code (0 becomes FIT_EXIT_ABORT) and leaves the job with a
 * longjmp; does not return */
void util_abort_job(int code)
{
    util_echo_off();
    g_exit_code = code;
    if (code == 0)
        g_exit_code = FIT_EXIT_ABORT;
    longjmp(g_job_jmp, FIT_JUMP_ABORT);
}

/* free(*p); *p = NULL: the list free callback for plain malloc blocks */
void util_free_ptr(void **p)
{
    free(*p);
    *p = NULL;
}

/* ------------------------------------------------------------------ */
/* parameters                                                          */
/* ------------------------------------------------------------------ */

static void bounded_copy(char *dst, const char *src, size_t size)
{
    size_t n = strlen(src);

    if (n >= size)
        n = size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* room for a parameter value: the extended mode keeps up to 2000 characters,
 * the legacy mode up to 255 */
static size_t param_value_size(void)
{
    return FIT_EXT ? FIT_PARAM_VALUE_MAX : FIT_NAME_MAX;
}

/* new parameter; the name and value are cut to the size of their fields */
FitParam *param_new(const char *name, const char *value)
{
    FitParam *p = util_calloc(1, sizeof(*p));

    if (name == NULL)
        p->name[0] = '\0';
    else
        bounded_copy(p->name, name, sizeof(p->name));
    if (value == NULL)
        p->value[0] = '\0';
    else
        bounded_copy(p->value, value, param_value_size());
    return p;
}

/* copy of the value of parameter `name` (ignoring case), NULL if absent */
char *param_get(FitList *params, const char *name)
{
    if (params == NULL)
        return NULL;
    for (int i = 0; i < params->count; i++) {
        FitParam *p = params->items[i];

        if (util_str_icmp(p->name, name) == 0) {
            char *copy = util_calloc(1, strlen(p->value) + 1);

            strcpy(copy, p->value);
            return copy;
        }
    }
    return NULL;
}

static int param_name_eq(void *param, void *name)
{
    return util_str_icmp(((FitParam *)param)->name, name) == 0;
}

FitParam *param_find(FitList *params, const char *name)
{
    int pos;

    if (params == NULL)
        return NULL;
    pos = list_find(params, (void *)(uintptr_t)name, param_name_eq);
    return pos != 0 ? params->items[pos - 1] : NULL;
}

/* removes the first parameter called `name`, if any */
static void param_remove(FitList *params, const char *name)
{
    int pos = list_find(params, (void *)(uintptr_t)name, param_name_eq);

    if (pos != 0)
        list_del_shift(params, pos);
}

/* if src has the parameter, give dst a copy of it (replacing the
 * one dst may have, with a warning when the values differ). */
void param_copy_into(FitList *src, FitList *dst, const char *name)
{
    char *value, *old;

    if (src == NULL || dst == NULL)
        return;
    value = param_get(src, name);
    if (value == NULL)
        return;
    old = param_get(dst, name);
    if (old != NULL) {
        if (util_str_icmp(value, old) != 0)
            fprintf(g_msg, "Warning - Overwritting parameter %s\n", name);
        param_remove(dst, name);
        free(old);
    }
    list_add(dst, param_new(name, value));
    free(value);
}

#define WORD_MAX 1000   /* size of the word buffers of util_first_word and util_next_word */

static void skip_spaces(char **pp)
{
    while (is_space(**pp))
        (*pp)++;
}

/* reads the next blank-separated word into buf (at most WORD_MAX - 1
 * characters): 1 and a word, or -1 at the end of the string (buf
 * untouched) */
static int scan_word(const char *p, char *buf)
{
    size_t n = 0;

    while (is_space(*p))
        p++;
    if (*p == '\0')
        return -1;
    while (*p != '\0' && !is_space(*p)) {
        if (n < WORD_MAX - 1)
            buf[n++] = *p;
        p++;
    }
    buf[n] = '\0';
    return 1;
}

static void cat_char(char *buf, char c)
{
    char one[2];

    one[0] = c;
    one[1] = '\0';
    util_str_cat_n(buf, one, WORD_MAX);
}

/* the first word of a command string (the program name), quotes
 * removed; *pp is moved behind it. */
char *util_first_word(char **pp)
{
    char buf[WORD_MAX];

    buf[0] = '\0';
    skip_spaces(pp);
    if (**pp == '\0')
        return NULL;
    if (**pp == '"') {
        (*pp)++;
        while (**pp != '"' && **pp != '\0') {
            cat_char(buf, **pp);
            (*pp)++;
        }
        if (**pp == '"')
            (*pp)++;
    } else {
        scan_word(*pp, buf);
        *pp += strlen(buf);
    }
    if (buf[0] == '\0')
        return NULL;
    return util_strdup(buf);
}

/* the next word of a command string.  A quoted string is one
 * word and keeps its quotes.  With join != 0 (the value of an option whose
 * name starts with "i") all following words up to the next one starting
 * with '-' are joined with single blanks, so that an input file name may
 * contain blanks. */
char *util_next_word(char **pp, int join)
{
    char buf[WORD_MAX];
    char word[WORD_MAX];
    int count = 0;

    buf[0] = '\0';
    skip_spaces(pp);
    if (**pp == '\0')
        return NULL;
    if (**pp == '"') {
        cat_char(buf, **pp);
        (*pp)++;
        while (**pp != '"' && **pp != '\0') {
            cat_char(buf, **pp);
            (*pp)++;
        }
        if (**pp == '"') {
            cat_char(buf, '"');
            (*pp)++;
        }
    } else if (join == 0) {
        scan_word(*pp, buf);
        *pp += strlen(buf);
    } else {
        int r;

        word[0] = '\0';
        r = scan_word(*pp, word);
        while (r != 0 && word[0] != '-' && **pp != '\0') {
            if (count != 0)
                util_str_cat_n(buf, " ", WORD_MAX);
            util_str_cat_n(buf, word, WORD_MAX);
            skip_spaces(pp);
            *pp += strlen(word);
            count++;
            r = scan_word(*pp, word);
        }
    }
    if (buf[0] == '\0')
        return NULL;
    return util_strdup(buf);
}

/* Extended mode: as param_parse_command, but an option given again adds its value
 * to the first one's, separated by a comma (an option without value
 * clears it), and the value of -lib, like that of -i, may contain blanks */
static int param_parse_command_ext(char *cmd, FitList **out)
{
    char *w;
    FitParam *p;

    if (*cmd == '\0') {
        *out = NULL;
        return 1;
    }
    *out = list_create();
    w = util_next_word(&cmd, 0);
    while (w != NULL && *w != '-') {
        p = util_calloc(1, sizeof(*p));
        p->name[0] = '\0';
        util_str_copy_n(p->value, w, sizeof(p->value), 1);
        list_add(*out, p);
        free(w);
        w = util_next_word(&cmd, 0);
    }
    if (w == NULL) {
        if (*out == NULL || (*out)->count == 0)
            list_destroy(out);
        return 1;
    }
    for (;;) {
        int pos, fresh, join;

        if (*w != '-') {
            list_destroy(out);
            return 0;
        }
        pos = list_find(*out, w + 1, param_name_eq);
        fresh = pos == 0;
        if (fresh) {
            p = util_calloc(1, sizeof(*p));
            util_str_copy_n(p->name, w + 1, sizeof(p->name), 1);
        } else {
            p = (*out)->items[pos - 1];
        }
        join = w[1] == 'i' || (w[1] == 'l' && w[2] == 'i' && w[3] == 'b');
        free(w);
        w = util_next_word(&cmd, join);
        if (w == NULL || *w == '-') {
            p->value[0] = '\0';
            if (fresh)
                list_add(*out, p);
            if (w == NULL)
                return 1;
        } else {
            if (fresh) {
                util_str_copy_n(p->value, w, sizeof(p->value), 1);
                list_add(*out, p);
            } else {
                util_str_cat_n(p->value, ",", sizeof(p->value));
                util_str_cat_n(p->value, w, sizeof(p->value));
            }
            free(w);
            w = util_next_word(&cmd, 0);
            if (w == NULL)
                return 1;
        }
    }
}

/* "word word -name value -name ..." -> list of FitParam.  Words
 * before the first option get an empty name.  Returns 0 (and destroys the
 * list) when a word that is not an option follows an option's value. */
int param_parse_command(char *cmd, FitList **out)
{
    if (FIT_EXT)
        return param_parse_command_ext(cmd, out);

    char *w;
    FitParam *p;

    if (*cmd == '\0') {
        *out = NULL;
        return 1;
    }
    *out = list_create();
    w = util_next_word(&cmd, 0);
    for (;;) {
        if (w == NULL)
            break;
        if (*w == '-') {
            for (;;) {
                int join;

                if (*w != '-') {
                    list_destroy(out);
                    return 0;
                }
                p = util_calloc(1, sizeof(*p));
                util_str_copy_n(p->name, w + 1, sizeof(p->name), 1);
                join = w[0] == '-' && w[1] == 'i';
                free(w);
                w = util_next_word(&cmd, join);
                if (w == NULL)
                    break;
                if (*w == '-') {
                    p->value[0] = '\0';
                    list_add(*out, p);
                } else {
                    util_str_copy_n(p->value, w, FIT_NAME_MAX, 1);
                    free(w);
                    list_add(*out, p);
                    w = util_next_word(&cmd, 0);
                    if (w == NULL)
                        return 1;
                }
            }
            p->value[0] = '\0';
            list_add(*out, p);
            return 1;
        }
        p = util_calloc(1, sizeof(*p));
        p->name[0] = '\0';
        util_str_copy_n(p->value, w, FIT_NAME_MAX, 1);
        list_add(*out, p);
        free(w);
        w = util_next_word(&cmd, 0);
    }
    if (*out == NULL || (*out)->count == 0)
        list_destroy(out);
    return 1;
}

/* ------------------------------------------------------------------ */
/* strings                                                             */
/* ------------------------------------------------------------------ */

/* 1 if equal ignoring case; NULL only equals NULL */
int util_str_ieq(const char *a, const char *b)
{
    if (a == NULL || b == NULL)
        return a == b;
    return strcasecmp(a, b) == 0;
}

/* parses an unsigned decimal number; 0 and a message if s is not one or
 * it overflows */
int util_str_to_uint(unsigned *out, const char *s)
{
    unsigned v = 0;
    const char *text = s;

    if (util_is_digits(s) == 0) {
        util_report_error(FIT_ERR_NOT_UINT, s);
        return 0;
    }
    for (; *s != '\0'; s++) {
        unsigned next = v * 10u + (unsigned)(*s - '0');

        if (v > next) {
            util_report_error(FIT_ERR_UINT_OVERFLOW, text);
            return 0;
        }
        v = next;
    }
    *out = v;
    return 1;
}

/* compares ignoring case (-1, 0, 1); with a NULL argument: a != b.
 * dev_chain_miss passes the value on, so it is exactly -1, 0 or 1. */
int util_str_icmp(const char *a, const char *b)
{
    int r;

    if (a == NULL || b == NULL)
        return a != b;
    r = strcasecmp(a, b);
    return (r > 0) - (r < 0);
}

int util_starts_alpha(const char *s)
{
    return is_alpha(*s);
}

/* 1-based index of the first of the n strings that equals s (ignoring
 * case), else 0 */
int util_str_choice(const char *s, int n, ...)
{
    va_list ap;
    int found = 0;

    if (s == NULL)
        return 0;
    va_start(ap, n);
    for (int i = 1; i <= n; i++) {
        const char *choice = va_arg(ap, const char *);

        if (util_str_icmp(s, choice) == 0) {
            found = i;
            break;
        }
    }
    va_end(ap);
    return found;
}

/* 1 if every character is a digit (also for "") */
int util_is_digits(const char *s)
{
    for (; *s != '\0'; s++) {
        if (!is_digit(*s))
            return 0;
    }
    return 1;
}

char *util_strdup(const char *s)
{
    char *copy = util_calloc(1, strlen(s) + 1);

    strcpy(copy, s);
    return copy;
}

char *util_strdup_or_null(const char *s)
{
    if (s == NULL)
        return NULL;
    return util_strdup(s);
}

static void util_print_str(FILE *out, void *s)
{
    fprintf(out, "%s", (const char *)s);
}

/* new string "<s><index>" ("<index>" for a NULL s); NULL after a message
 * for a negative index */
char *util_str_with_index(const char *s, int index)
{
    char *out;
    size_t size;

    if (index < 0) {
        util_report_error(FIT_ERR_NEG_INDEX, "StrAppendIndexToStr");
        return NULL;
    }
    if (s == NULL)
        s = "";
    size = (size_t)snprintf(NULL, 0, "%s%d", s, index) + 1;
    out = util_calloc(1, size);
    snprintf(out, size, "%s%d", s, index);
    return out;
}

void util_print_str_list(FILE *out, FitList *strings, const char *sep)
{
    list_print(out, strings, util_print_str, sep);
}

/* put prefix in front of s, in place.  The caller's buffer must
 * have room for both. */
void util_str_insert_front(const char *prefix, char *s)
{
    size_t n = strlen(s);
    size_t m = strlen(prefix);

    memmove(s + m, s, n + 1);
    memcpy(s, prefix, m);
}

/* "XXI_<n>" (kind 0), "XXO_<n>" (kind 1), "XXL_<n>" (kind 2), an
 * empty string for any other kind; 0x100 bytes are allocated. */
char *util_gen_name(int kind, int n)
{
    char *name = util_calloc(1, 0x100);

    if (kind == 0)
        snprintf(name, 0x100, "%s%d", "XXI_", n);
    else if (kind == 1)
        snprintf(name, 0x100, "%s%d", "XXO_", n);
    else if (kind == 2)
        snprintf(name, 0x100, "%s%d", "XXL_", n);
    return name;
}

/* *s = *s + t in a new block, the old one freed; *s NULL: a copy of t */
void util_str_append(char **s, const char *t)
{
    char *out;

    if (t == NULL) {
        util_report_error(FIT_ERR_CAT_NULL2, "");
        util_report_error(FIT_ERR_MAY_BE_WRONG, "");
        return;
    }
    if (*s == NULL) {
        *s = util_strdup(t);
        return;
    }
    out = util_calloc(1, strlen(*s) + strlen(t) + 1);
    strcpy(out, *s);
    strcat(out, t);
    free(*s);
    *s = out;
}

/* append src to dst, which is a buffer of `size`
 * bytes; reports a truncation. */
void util_str_cat_n(char *dst, const char *src, int size)
{
    size_t have;

    if (src == NULL)
        return;
    if (dst == NULL) {
        util_report_error(FIT_ERR_CAT_NULL1, "");
        util_report_error(FIT_ERR_MAY_BE_WRONG, "");
        return;
    }
    have = strlen(dst);
    if ((unsigned)size - 1u <= (unsigned)(have + strlen(src)))
        util_report_error(FIT_ERR_TRUNCATED, dst);
    if (size > 0 && have < (size_t)size - 1)
        strncat(dst, src, (size_t)size - have - 1);
}

/* copy src into dst, a buffer of `size` bytes.
 * Returns 0 when the text had to be cut (reported if warn != 0). */
int util_str_copy_n(char *dst, const char *src, size_t size, int warn)
{
    if (src == NULL) {
        *dst = '\0';
        return 1;
    }
    if (dst == NULL) {
        util_report_error(FIT_ERR_INTERNAL, "in StrSafeCatTX (Str1 is NULL)");
        return 0;
    }
    if (size - 1 <= strlen(src)) {
        if (warn != 0)
            util_report_error(FIT_ERR_TRUNCATED, src);
        strncpy(dst, src, size);
        dst[size - 1] = '\0';
        return 0;
    }
    strcpy(dst, src);
    return 1;
}

/* strrchr that accepts NULL; a backslash also matches a slash,
 * because every caller looks for the last path separator. */
const char *util_strrchr(const char *s, char c)
{
    const char *found;

    if (s == NULL || c == '\0')
        return NULL;
    found = strrchr(s, c);
    if (c == '\\') {
        const char *slash = strrchr(s, '/');

        if (slash != NULL && (found == NULL || slash > found))
            found = slash;
    }
    return found;
}

/* the file name of a path without its directory and without
 * everything from the first '.' on. */
char *util_base_name(const char *path)
{
    const char *sep;
    char *out;
    size_t len, n = 0;

    if (path == NULL)
        return NULL;
    sep = util_strrchr(path, '\\');
    if (sep != NULL)
        path = sep + 1;
    len = strlen(path);
    out = util_calloc(len + 1, 1);
    while (n < len && path[n] != '.') {
        out[n] = path[n];
        n++;
    }
    out[n] = '\0';
    return out;
}

/* join the strings with sep, starting with indent and beginning
 * a new line (newline, indent) before a string that would pass column
 * `width`; "..." ends the text once it is longer than 0x400 characters. */
char *util_join_wrapped(FitList *strings, const char *sep, const char *indent, unsigned width)
{
    char *out = util_strdup(indent);
    size_t col = strlen(indent);
    int count = strings == NULL ? 0 : strings->count;

    for (int i = 0; i < count; i++) {
        const char *s = strings->items[i];

        if (strlen(s) + strlen(out) + strlen(sep) > 0x400) {
            util_str_append(&out, "...");
            return out;
        }
        if (strlen(sep) + col + strlen(s) > width) {
            if (!(strlen(s) > width && col == strlen(indent))) {
                util_str_append(&out, "\n");
                util_str_append(&out, indent);
                col = strlen(indent);
            }
        }
        util_str_append(&out, s);
        if (i != count - 1)
            util_str_append(&out, sep);
        col = strlen(sep) + col + strlen(s);
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* timers                                                              */
/* ------------------------------------------------------------------ */

/* whole seconds since the first call (usec is always 0) */
static void util_elapsed_seconds(int *sec, int *usec)
{
    time_t now = time(NULL);

    if (g_time0 == 0) {
        g_time0 = now;
        *sec = 0;
        *usec = 0;
        return;
    }
    *sec = (int)difftime(now, g_time0);
    *usec = 0;
}

/* the "CPU time" is wall-clock time as well */
static void util_cpu_seconds(int *sec, int *usec)
{
    util_elapsed_seconds(sec, usec);
}

const char *util_getenv(const char *name)
{
    return getenv(name);
}

/* timer mode 0, all figures cleared, start time taken now */
void util_timer_reset(void)
{
    int sec, usec;

    util_elapsed_seconds(&sec, &usec);
    g_timer_mode = 0;
    g_session_elapsed = 0;
    g_session_cpu = 0;
    g_command_cpu = 0;
    g_command_elapsed = 0;
    g_timer_start = (double)usec * 1e-06 + (double)sec;
}

/* updates the session and command figures */
void util_timer_update(void)
{
    int csec, cusec, esec, eusec;
    double prev_elapsed, prev_cpu;

    util_cpu_seconds(&csec, &cusec);
    util_elapsed_seconds(&esec, &eusec);
    prev_elapsed = g_session_elapsed;
    prev_cpu = g_session_cpu;
    g_session_cpu = (double)cusec * 1e-06 + (double)csec;
    g_command_cpu = g_session_cpu - prev_cpu;
    g_session_elapsed = ((double)eusec * 1e-06 + (double)esec) - g_timer_start;
    g_command_elapsed = g_session_elapsed - prev_elapsed;
}

/* session time as of the last util_timer_update, which the report prints
 * ("FIT%s completed in %4.2f seconds") */
double util_timer_session_elapsed(void)
{
    return g_session_elapsed;
}

/* nothing in the mode the fitter runs in (g_timer_mode is 0) */
void util_timer_print(FILE *out)
{
    if (g_timer_mode == 1) {
        fprintf(out, "Command CPU time : %4.2f s | ", g_command_cpu);
        fprintf(out, "Command Elapsed time : %4.2f s\n", g_command_elapsed);
        fprintf(out, "Session CPU time : %4.2f s | ", g_session_cpu);
        fprintf(out, "Session Elapsed time : %4.2f s\n", g_session_elapsed);
    }
    if (g_timer_mode == 2) {
        fprintf(out, "CPU : %4.2f / %4.2f s | Elapsed : %4.2f / %4.2f s\n",
                g_command_cpu, g_session_cpu, g_command_elapsed, g_session_elapsed);
    }
}

/* reset the statics of this file at the start of a run */
void base_util_reset(void)
{
    g_msg = stdout;
    g_exit_code = 0;
    g_open_files = NULL;
    g_empty_str[0] = '\0';
    g_tmp_counter = 0;
    g_tmp_names = NULL;
    g_echo_stream = NULL;
    g_final_list = NULL;
    g_echo_lock = 0;
    g_echo_lock_saved = 0;
    g_echo_match = 1;
    g_time0 = 0;
    g_session = 0;
    g_timer_mode = 0;
}
