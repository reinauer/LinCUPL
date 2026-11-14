/*
 * base_int.h - declarations shared only between the source files of the
 * base modules (base_*.c, main.c).  Other packages include base.h.
 */
#ifndef FIT150X_BASE_INT_H
#define FIT150X_BASE_INT_H

#include "base.h"

/* base_list.c */
void base_list_reset(void);

/* base_util.c */
int   util_echo_note_stream(FILE *f);
int   util_echo_was_stream(void);
void  util_final_cleanup(void);
void  util_job_init(void);
char *util_first_word(char **pp);
int   param_parse_command(char *cmd, FitList **out);
const char *util_strrchr(const char *s, char c);
const char *util_getenv(const char *name);
void  util_timer_reset(void);
void  util_timer_print(FILE *out);
void  base_util_reset(void);

/* base_lex.c */
void  base_lex_reset(void);

/* base_entity.c */
void  base_entity_reset(void);

#endif /* FIT150X_BASE_INT_H */
