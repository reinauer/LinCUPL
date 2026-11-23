/*
 * base.h - public interface of the base modules of the ATF150x fitter
 * (fit1502 / fit1504 / fit1508): temporary files, the pointer list,
 * utilities, the lexer, the entity table, variables, entity objects,
 * function trees, product terms, entity lookups and main / command line.
 *
 * Implemented in src/fit150x/base_*.c and src/fit150x/main.c.
 *
 * Conventions that hold for everything declared here
 *  - List positions returned by the find functions and taken by the delete
 *    and insert functions are 1-based; 0 means "not found".
 *  - A FitList item is a void *.  Lists of integers (variable ids, counts)
 *    store them with FIT_INT2PTR and read them with FIT_PTR2INT.
 *  - "free" callbacks (FitListFree) take the address of the slot, release
 *    what it points to and set the slot to NULL.
 *  - Both string comparisons are case-insensitive: util_str_ieq returns 1
 *    for equal, util_str_icmp 0.
 *  - Allocation never returns NULL: util_calloc / util_malloc / util_realloc
 *    print "Memory Full", set the exit code 3 and longjmp to the job.
 */
#ifndef FIT150X_BASE_H
#define FIT150X_BASE_H

#include "platform.h"
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "fit_types.h"
#include "strutil.h"

#define FIT_INT2PTR(v) ((void *)(intptr_t)(v))
#define FIT_PTR2INT(p) ((int)(intptr_t)(p))

/* Callback shapes beyond those of fit_types.h */
typedef int   (*FitListEq3)(void *item, void *key, void *extra);   /* nonzero = match */
typedef void *(*FitListCopy2)(void *item, void *extra);
typedef void  (*FitListPrint)(FILE *out, void *item);
typedef void  (*FitListPrint2)(FILE *out, void *item, void *extra);

/* ------------------------------------------------------------------ */
/* Globals                                                             */
/* ------------------------------------------------------------------ */

extern FILE *g_msg;               /* current message stream (stdout or the temporary log) */
extern int   g_exit_code;         /* what main returns; enum FitExitCode */
extern jmp_buf g_job_jmp;         /* set in main_run_job; values enum FitJump */
extern FitOptions g_opt;          /* the option values (fit_types.h) */
extern int g_fit_mode;         /* enum FitMode: the fitter version reproduced */
extern const char *g_family;      /* "1508" / "1504" / "1502" (from dev_family_name) */
extern char *g_version;           /* PROGRAM_VERSION */
extern char *g_command;           /* the command string "fit1508 <in> -name value ..." */
extern char *g_fitterdir;         /* $FITTERDIR or "" */
extern char *g_tmp_prefix;        /* "<FITTERDIR>/" used for temporary files */
extern char *g_lib_prefix;        /* "<FITTERDIR>/" used to find the library */
extern int   g_verbose;           /* 0 in the CUPL flow; gates lexer warnings */
extern FitList *g_open_files;     /* FILE * opened through util_fopen* */
extern FitList *g_tree_gc;        /* fcttree nodes waiting to be released */
extern FitList *g_keep_ids;       /* variable ids entity_simplify must not remove (set by others) */
extern FitList *g_preassign;  /* pin preassignments {char *name; int pin; int unused} (FitPreassignItem of place.h);
                                     created empty by the job, filled by the readers and by -Global_OE/-OE_node */
extern char *g_jedec_file;        /* value of -jedec_file or NULL */
extern char *g_vector_file;       /* value of -vector_file or NULL */
extern char *g_verilog_file;      /* value of -verilog_file or NULL */
extern char *g_time_file;         /* value of -time_file or NULL */
extern char *g_in_base;           /* input file name without extension */
extern char *g_in_name;           /* Extended mode: input file name without its directory */
extern char *g_tech_name;         /* Extended mode: the device name the technology is read from */
extern char  g_empty_str[1];      /* "" */

/* lexer state */
extern char g_lex_text[FIT_NAME_MAX + 1]; /* text of the current token (first 0x100 characters) */
extern char g_lex_long_text[0xa01];    /* full text of the current token */
extern int  g_lex_token;               /* enum FitLexToken, keyword index, or FIT_TOK_* below */
extern int  g_lex_line;                /* current line number */
extern int  g_lex_failed;              /* set by the error functions */
extern int  g_lex_error_count;
extern int  g_lex_eof;
extern char g_lex_char;                /* look-ahead character ((char)-1 at end of file) */

/* Token codes of the delimiters; FIT_TOK_IDENT -100 and
 * FIT_TOK_REAL -99 are in fit_types.h.  -99 is in fact "end of file". */
enum FitLexDelim {
    FIT_TOK_EOF      = -99,
    FIT_TOK_LBRACKET = -1,   /* [ */
    FIT_TOK_RBRACKET = -2,   /* ] */
    FIT_TOK_LBRACE   = -3,   /* { */
    FIT_TOK_RBRACE   = -4,   /* } */
    FIT_TOK_SLASH    = -5,   /* / */
    FIT_TOK_PLUS     = -6,   /* + */
    FIT_TOK_MINUS    = -7,   /* - */
    FIT_TOK_DOT      = -8,   /* . */
    FIT_TOK_CARET    = -9,   /* ^ */
    FIT_TOK_LPAREN   = -10,  /* ( */
    FIT_TOK_RPAREN   = -11,  /* ) */
    FIT_TOK_EQUAL    = -12,  /* = */
    FIT_TOK_LT       = -13,  /* < */
    FIT_TOK_LE       = -14,  /* <= */
    FIT_TOK_GT       = -15,  /* > */
    FIT_TOK_GE       = -16,  /* >= */
    FIT_TOK_EQEQ     = -17,  /* == */
    FIT_TOK_ASSIGN   = -18,  /* := */
    FIT_TOK_COLON    = -19,  /* : */
    FIT_TOK_SEMI     = -20,  /* ; */
    FIT_TOK_STAR     = -21,  /* * */
    FIT_TOK_STARSTAR = -22,  /* ** */
    FIT_TOK_BANG     = -23,  /* ! */
    FIT_TOK_QUESTION = -24,  /* ? */
    FIT_TOK_TILDE    = -25,  /* ~ */
    FIT_TOK_DQUOTE   = -26,  /* " */
    FIT_TOK_QUOTE    = -27,  /* ' */
    FIT_TOK_AMP      = -28,  /* & */
    FIT_TOK_COMMA    = -30   /* , */
};

/* ------------------------------------------------------------------ */
/* list                                                                */
/* ------------------------------------------------------------------ */

FitList *list_create_sized(int capacity);                 /* capacity 0 becomes 1 */
FitList *list_create(void);                               /* capacity 16 */
void  list_destroy(FitList **l);                          /* frees array and header, not the items; *l = NULL */
int   list_del_shift(FitList *l, int pos);                /* delete pos, keep order; 0 on bad index */
int   list_del_shift_free(FitList *l, int pos, FitListFree fn); /* fn(&slot) then delete */
int   list_del_insert(FitList *l, int pos);               /* move the last item into pos */
void  list_add(FitList *l, void *item);                   /* append, capacity doubles when full */
int   list_find(FitList *l, void *key, FitListEq eq);     /* first pos with eq(item, key) != 0, else 0; l may be NULL */
int   list_find_x(FitList *l, void *key, FitListEq3 eq, void *extra); /* same with eq(item, key, extra) */
void  list_free_all(FitList **l, FitListFree fn);         /* fn(&slot) for each item, then list_destroy */
int   list_compact(FitList *l);                           /* drop NULL items, return the new count */
int   list_equal_sets(FitList *a, FitList *b, FitListEq eq);  /* 1 if each item of one has a match in the other */
int   list_equal_sets_x(FitList *a, FitList *b, FitListEq3 eq, void *extra);
FitList *list_map_copy(FitList *l, FitListCopy copy);     /* new list of copy(item); NULL for NULL */
FitList *list_map_copy_x(FitList *l, FitListCopy2 copy, void *extra);
FitList *list_copy(FitList *l);                           /* shallow copy (capacity 16); NULL for NULL */
void  list_append_destroy(FitList *dst, FitList **src);   /* append *src to dst, destroy *src */
void  list_append_mapped(FitList *dst, FitList *src, FitListCopy copy);
void  list_append(FitList *dst, FitList *src);
void  list_print(FILE *out, FitList *l, FitListPrint print, const char *sep); /* items separated by "%s" of sep */
void  list_print_x(FILE *out, FitList *l, FitListPrint2 print, const char *sep, void *extra);
int   list_all_distinct(FitList *l, FitListEq eq);        /* 0 if two items match, else 1 */
FitList *list_intersection(FitList *a, FitList *b, FitListEq eq); /* items of a found in b; NULL when empty */
FitList *list_intersection_mapped(FitList *a, FitList *b, FitListEq3 eq, FitListCopy2 copy, void *extra);
FitList *list_union(FitList *a, FitList *b, FitListEq eq);    /* copy of a plus items of b not in a; NULL when empty */
void  list_sort(FitList *l, int ascending, FitListCmp cmp);   /* merge sort (base_list.c); ascending 0 sorts downwards */
FitList *list_difference_x(FitList *a, FitList *b, FitListEq3 eq, void *extra); /* items of a not in b */
void *list_extreme(FitList *l, int mode, FitListCmp cmp); /* mode 0: first maximum, 1: first minimum; NULL when empty */
int   list_insert_at(FitList *l, void *item, int pos);    /* item becomes position pos */
void  list_add_unique(FitList *l, void *item, FitListEq eq);
int   list_contains_ptr(FitList *l, void *item);          /* l must not be NULL */

void *list_identity(void *item);                          /* FitListCopy returning its argument */
int   list_ptr_eq(void *a, void *b);                      /* FitListEq: a == b (also used for integers) */
int   list_int_cmp(void *a, void *b);                     /* FitListCmp on integers stored in the slots */
void  list_print_int(FILE *out, void *item);              /* FitListPrint "%d" */
unsigned util_lowest_set_bit(unsigned v);                 /* 1..32, 0 for v == 0 */

void  msg_redirect_to(const char *path);                  /* push g_msg, append messages to path */
void  msg_restore(void);                                  /* close the log, pop g_msg */

/* ------------------------------------------------------------------ */
/* temporary files                                                     */
/* ------------------------------------------------------------------ */

FILE *util_fopen_raw(const char *path, const char *mode); /* fopen with \\ -> /; existing files are found ignoring case */
int   file_getc_text(FILE *f);                            /* getc of a file opened in text mode: CR LF -> LF, Ctrl-Z = end of file */
FILE *file_open_read(const char *path);                   /* fopen "r" (text: CR dropped by the reader); NULL path -> NULL */
FILE *file_open_write(const char *path);                  /* fopen "w" */
int   file_accessible(const char *path, const char *mode);
char *tmp_log_name(void);                                 /* "<prefix>a%04d%03d.tmp", registered for deletion */
void  tmp_cleanup(void);                                  /* close all open files, delete the temporaries */

/* ------------------------------------------------------------------ */
/* util                                                                */
/* ------------------------------------------------------------------ */

/* error codes of util_report_error */
enum FitErrCode {
    FIT_ERR_PLAIN = 0, FIT_ERR_UNDEF_CMD = 1, FIT_ERR_USAGE = 3, FIT_ERR_FILE_NOT_FOUND = 4,
    FIT_ERR_TOO_MANY_ENTITIES = 5, FIT_ERR_ENTITY_TYPE = 6, FIT_ERR_ENTITY_NOT_FOUND = 7,
    FIT_ERR_PARAM_VALUE = 8, FIT_ERR_MEMORY = 9, FIT_ERR_BAD_INDEX = 10, FIT_ERR_ALIAS_FILE = 11,
    FIT_ERR_INTERNAL = 12, FIT_ERR_NOT_ALLOWED = 13, FIT_ERR_INTERNAL_WARNING = 14,
    FIT_ERR_UNLICENSED = 15, FIT_ERR_LOG_OPEN = 16, FIT_ERR_UINT_OVERFLOW = 18, FIT_ERR_NOT_UINT = 19,
    FIT_ERR_NEG_INDEX = 20, FIT_ERR_BAD_INDICES = 21, FIT_ERR_CAT_NULL2 = 22, FIT_ERR_MAY_BE_WRONG = 23,
    FIT_ERR_TRUNCATED = 24, FIT_ERR_CAT_NULL1 = 25, FIT_ERR_OPEN = 26, FIT_ERR_CLOSE = 27,
    FIT_ERR_FORMAT = 28
    /* any other code: "Fatal Error : %s" and util_abort_job(99) */
};

void  util_report_error(int code, const char *text);
void *util_calloc(size_t n, size_t size);                 /* zeroed; never returns NULL */
void *util_malloc(size_t size);                           /* zero-filled */
void *util_realloc(void *p, size_t size);
FILE *util_fopen_checked(const char *path, const char *mode); /* registers the file; failure: "Can not Open File", exit code 5, longjmp */
FILE *util_fopen(const char *path, const char *mode);     /* registers the file; NULL on failure */
int   util_fclose(FILE *f);                               /* unregisters and closes */
void  util_abort_job(int code);                           /* exit code (0 becomes 6), longjmp(g_job_jmp, 0); no return */
void  util_free_ptr(void **p);                            /* free(*p); *p = NULL (FitListFree for malloc'ed items) */

void  util_echo_set(FILE *f);                             /* (stream noted for the log echo; no visible effect) */
void  util_echo_off(void);
void  util_echo_restore(void);

FitParam *param_new(const char *name, const char *value); /* NULL name/value give "" */
char *param_get(FitList *params, const char *name);       /* util_calloc'ed copy of the value, NULL if absent; caller frees */
FitParam *param_find(FitList *params, const char *name);  /* the first parameter called name (ignoring case), NULL if absent */
char *util_next_word(char **pp, int join);                 /* next word of a command string, *pp moved behind it; NULL at the end */
void  param_copy_into(FitList *src, FitList *dst, const char *name);

int   util_str_ieq(const char *a, const char *b);         /* 1 if equal ignoring case; NULL only equals NULL */
int   util_str_icmp(const char *a, const char *b);        /* 0 if equal ignoring case; with a NULL argument: a != b */
int   util_str_to_uint(unsigned *out, const char *s);     /* decimal; 0 and a message on error */
int   util_starts_alpha(const char *s);                   /* nonzero if s[0] is a letter */
int   util_str_choice(const char *s, int n, ...);         /* n const char * follow; 1-based index of the one equal to s (ignoring case), else 0 */
int   util_is_digits(const char *s);                      /* 1 if every character is a digit ("" gives 1) */
char *util_strdup(const char *s);
char *util_strdup_or_null(const char *s);                 /* NULL for NULL */
char *util_str_with_index(const char *s, int index);      /* new "<s><index>"; NULL s gives "<index>" */
void  util_str_insert_front(const char *prefix, char *s); /* s = prefix + s in place (caller provides room) */
char *util_gen_name(int kind, int n);                     /* new 0x100-byte string "XXI_<n>" (kind 0), "XXO_<n>" (1), "XXL_<n>" (2) */
void  util_str_append(char **s, const char *t);           /* *s = *s + t in a new block (old one freed); *s NULL: copy of t */
void  util_str_cat_n(char *dst, const char *src, int size);   /* strncat within a buffer of `size` bytes */
int   util_str_copy_n(char *dst, const char *src, size_t size, int warn); /* copy, truncating to size-1 */
char *util_base_name(const char *path);                   /* new string: file name without directory and extension */
char *util_join_wrapped(FitList *strings, const char *sep, const char *indent, unsigned width);
void  util_print_str_list(FILE *out, FitList *strings, const char *sep);
void  util_timer_update(void);
double util_timer_session_elapsed(void);                 /* seconds since the start, as of the last util_timer_update */

/* ------------------------------------------------------------------ */
/* lexer                                                               */
/* ------------------------------------------------------------------ */

void  lex_next_token(void);                               /* result in g_lex_token / g_lex_text */
void  lex_error_text(const char *msg, const char *arg);   /* "Error on line %d : %s%s" */
void  lex_error(int mode, const char *msg, int tok, ...); /* "Error on line %d : %s"; if mode != 0 skip tokens until one of
                                                             tok, ... (list ended by 0) or end of file is current */
void  lex_error_skip(int mode, int tok, ...);              /* lex_error without a message (EDIF mode only; not called) */
int   lex_set_keyword_table(int n, ...);                  /* n pairs (int index, const char *word), index = 1..n */
int   lex_set_separator_table(int n, ...);                /* n characters (as int), at most 20 */
int   lex_set_delimiter_table(int n, ...);                /* n characters, at most 30 */
int   lex_set_extra_char_table(int n, ...);               /* n characters, at most 30 */
int   lex_set_default_tables(void);                       /* the three tables every reader starts from */
void  lex_set_streams(FILE *in, FILE *err);
void  lex_start(void);                                    /* line 1, counters cleared, first character read */
void  lex_set_comment_chars(char start, char end);
void  lex_expect(int tok);                                /* next token if current == tok, else "<name> expected" error */
const char *lex_token_name(int tok);                      /* spelling of a keyword or delimiter token */

/* ------------------------------------------------------------------ */
/* entity table                                                        */
/* ------------------------------------------------------------------ */

/* slot types */
enum FitSlotType { FIT_ENT_ENTITY = 2, FIT_ENT_LIBRARY = 3, FIT_ENT_FREE = 5 };

void  entity_table_init(void);
int   entity_table_find(const char *name, int *type);     /* slot (>= 4) or -1 (*type = 5) */
int   entity_table_new_slot(int *slot);                   /* 1 and a free slot, else 0 */
int   entity_table_store(int slot, const char *name, const char *file, int type, void *object); /* frees what the slot held */
int   entity_table_read(const char *name, int type, int *found_type, void **object, int *slot, int report);
int   entity_table_get(const char *name, int type, int *found_type, void **object, int *slot); /* = read with report 1 */

/* library object registered in the table by lib_open */
typedef struct FitLibHandle {
    int      kind;    /* 1 = "stdcell" text library, 2 / 5 = other formats (never produced) */
    char    *file;    /* path it was read from */
    FitList *gates;   /* gate list built by the library parser */
    void    *info;    /* second result of the parser; *(char **)info is printed when not NULL */
} FitLibHandle;

int   lib_open(const char *file, const char *name, char *fmt, FitLibHandle **out); /* find and parse a library, register it */
int   lib_gate_has_name(void *gate, char *name);          /* FitListEq over the gate list (kind-tagged items) */
void  var_print(FILE *out, FitVariable *v);               /* name, "name[%d:%d]" or "name(a,b,...)" */

/* ------------------------------------------------------------------ */
/* variables                                                           */
/* ------------------------------------------------------------------ */

FitVariable *var_new(int kind);
void  var_free(FitVariable **v);                          /* (FitListFree) */
FitList *vars_new_generated(int n, int *next_id, int name_kind, int first_number, FitList *existing); /* n variables named util_gen_name(name_kind, first_number..) not in `existing`, ids from *next_id */
FitVariable *var_copy(FitVariable *v);                    /* (FitListCopy) */
int   var_cmp_id(void *a, void *b);                       /* FitListCmp on FitVariable.id */
int   var_list_id_of_name(FitList *vars, const char *name);   /* id or 0 */
FitVariable *var_list_find_name(FitList *vars, const char *name);
FitVariable *entity_find_var(FitEntity *e, const char *name); /* inputs, outputs, locals in this order */
FitVariable *var_list_find_id(FitList *vars, int id);
int   var_list_pos_of_id(FitList *vars, int id);          /* 1-based, searched from the end; 0 if absent */
int   var_same_name(void *a, void *b);                    /* FitListEq over two FitVariable */
FitVariable *var_new_unique(FitEntity *e);                /* variable with a fresh id and a generated name unused in e */
void  var_set_unique_name(FitEntity *e, FitVariable *v, const char *base, const char *suffix);
int   entity_var_id(FitEntity *e, const char *name);      /* id or 0 */
int   var_has_name(void *var, void *name);                /* FitListEq: FitVariable against a char * */

/* ------------------------------------------------------------------ */
/* entity objects                                                      */
/* ------------------------------------------------------------------ */

FitFct *fct_new(void);
FitFct *fct_copy(FitFct *f);                              /* (FitListCopy) */
FitNetlist *netlist_new(void);
FitNetItem *netitem_new(void);                            /* also used for FitAlias (same 8-byte shape): see alias_new */
FitAlias *alias_new(void);
FitUnknownGate *unknown_gate_new(void);
FitUnknownPin *unknown_pin_new(void);
FitInstance *instance_new(void);
FitEntity *entity_new(const char *name);
FitNetlist *netlist_copy(FitNetlist *n);
FitEntity *entity_copy(FitEntity *e);
void  fct_free(FitFct **f);                               /* (FitListFree; needs fcttree_gc_begin/_end around it) */
void  fcttree_gc_begin(void);
void  fcttree_gc_end(void);
void  cell13_free(FitCell13 **c);
void  netitem_free(FitNetItem **it);                      /* (FitListFree) */
void  alias_free(FitAlias **a);                           /* (FitListFree) */
void  entity_free(FitEntity **e);
void  entity_clear(FitEntity *e);                         /* release the contents, keep the struct */
void  entity_shallow_assign(FitEntity *src, FitEntity *dst);
void  entity_remove_fct(FitEntity *e, FitFct *f);         /* drop f and its (output or local) variable */
FitFctTree *fcttree_propagate_inv(FitFctTree *t, unsigned invert); /* copy with NOTs pushed to the leaves */
FitEntity *entity_copy_inv_propagated(FitEntity *e);
FitFctTree *fcttree_from_pterm(FitPTerm *p, FitList *vars);
FitFctTree *fcttree_from_pterms(FitList *pterms, FitList *vars);
char *entity_var_name(FitEntity *e, int id);              /* new string: name of variable id, or a generated name */
int   entity_is_output(FitEntity *e, int id);
int   entity_is_input(FitEntity *e, int id);
int   entity_is_local(FitEntity *e, int id);
FitFctTree *entity_fct_tree(FitEntity *e, int id);        /* tree[0] of the function defining id, or NULL */
FitFct *entity_find_fct(FitEntity *e, int id);
void  entity_update_counts(FitEntity *e);                 /* n_in / n_out / n_local from the list sizes */

/* ------------------------------------------------------------------ */
/* function trees                                                      */
/* ------------------------------------------------------------------ */

FitFctTree *fcttree_new(void);                            /* selector 'T' */
void  fcttree_free_deferred(FitFctTree **t);              /* (FitListFree) queue the nodes on g_tree_gc */
void  fcttree_free(FitFctTree **t);                       /* gc_begin, free_deferred, gc_end; *t = NULL */
FitFctTree *fcttree_copy(FitFctTree *t, FitListCopy user_copy); /* user_copy may be NULL */
FitList *fcttree_to_pterms3(FitFctTree *t, FitList *v1, FitList *v2, FitList *v3); /* over the concatenation of three variable lists */
FitList *fcttree_to_pterms(FitFctTree *t, FitList *vars); /* list of FitPTerm */
void  fcttree_collect_ids(FitFctTree *t, FitList *ids);   /* add each variable id once */
int   fcttree_count_ids(FitFctTree *t);                   /* number of distinct ids */
void  fcttree_depth_rec(FitEntity *e, FitFctTree *t, FitList *depths, int *max, int level);
void  fcttree_depth(FitEntity *e, FitFctTree *t, int *depth);
unsigned fcttree_find_cycle(FitEntity *e, FitFctTree *t, FitList *path, FitList *seen, char **cycle);
unsigned fcttree_count_id(FitFctTree *t, int id);         /* occurrences of id */
int   fcttree_count_leaves(FitFctTree *t);
void  fcttree_substitute(FitFctTree **t, FitFct *f);      /* replace leaves f->var_id by f->tree[0] (shared, not copied) */
void  fcttree_compact(FitFctTree **t);
FitFctTree *fcttree_new_id(int id);
FitFctTree *fcttree_new_const(int value);
FitFctTree *fcttree_new_not(FitFctTree *child);           /* NULL for NULL */
FitFctTree *fcttree_new_binary(int sel, FitFctTree *a, FitFctTree *b);
FitFctTree *fcttree_new_binary_ids(int sel, int id_a, int id_b);
FitList *pterm_complement(FitPTerm *p, int nwords);       /* list of single-literal FitPTerm */
extern FitList *g_depth_visited;                          /* ids being visited by fcttree_depth_rec (set by the caller) */

/* ------------------------------------------------------------------ */
/* product terms                                                       */
/* ------------------------------------------------------------------ */

void  pterm_set_literal(FitPTerm *p, int var, char c);    /* var is 1-based; c is '0' '1' '-' */
unsigned *pterm_list_support(FitList *pterms, int nwords);
int   pterm_is_tautology(FitPTerm *p, int nwords);        /* 1 if no literal is constrained */
int   pterm_bit(FitPTerm *p, int var);                   /* bit of variable var (1-based) in p->bits */
void  pterm_alloc(FitPTerm *p, int nwords);               /* allocate both vectors */
FitPTerm *pterm_new_raw(int nwords);
void  pterm_init_dc(FitPTerm *p, int nwords);             /* allocate; bits 0, care all ones */
FitPTerm *pterm_new(int nwords);                          /* pterm_init_dc on a new term */
void  pterm_init_zero(FitPTerm *p, int nwords);           /* allocate; both vectors 0 */
FitPTerm *pterm_new_zero(int nwords);
FitPTermIO *ptermio_new(int nwords_in, int nwords_out);
void  pterm_build(FitPTerm *p, const char *s, unsigned n);/* from n characters 0 1 - ? ~ */
void  ptermio_build(FitPTermIO *p, const char *in, const char *out, unsigned n_in, unsigned n_out);
int   pterm_equal(void *a, void *b, void *nwords);        /* FitListEq3; nwords passed with FIT_INT2PTR */
int   pterm_covers(FitPTerm *a, FitPTerm *b, int nwords);
void  pterm_to_string(FitPTerm *p, unsigned n, char *out, int warn); /* n characters and a NUL */
FitPTerm *pterm_copy_words(unsigned nwords_copy, int nwords, FitPTerm *src);
void  pterm_print(FILE *out, FitPTerm *p, unsigned n);
void  pterm_release(FitPTerm *p);                         /* free both vectors */
void  pterm_free(FitPTerm **p);                           /* (FitListFree) */
void  ptermio_free(FitPTermIO **p);                       /* (FitListFree) */
int   pterm_init_with_value(FitPTerm *p, const char *c, int nwords);
unsigned pterm_has_literal(FitPTerm *p, unsigned lit);    /* lit = 2 * index + (1 for the complemented literal) */
int   pterm_literal_code(FitPTerm *p, unsigned index);    /* bits*2 + care */
FitPTerm *pterm_copy(FitPTerm *p, int nwords);            /* NULL for NULL */
int   pterm_count_literals(FitPTerm *p, int nwords);
FitList *ptermio_select_output(FitList *pterms, int out, char c);
void  ptermio_merge_output(FitList *pterms, FitList *into, int out, int nwords_in, int nwords_out, char c);
int   pterm_lists_disjoint(FitList *a, FitList *b, int nwords);
void  pterm_and_into(FitPTerm *dst, FitPTerm *src, int nwords);
FitPTerm *pterm_and(FitPTerm *a, FitPTerm *b, int nwords);/* new product, NULL if empty */
FitList *pterm_list_and(FitList *a, FitList *b, int nwords);
void  pterm_and_to(FitPTerm *a, FitPTerm *b, FitPTerm *dst, int nwords);
int   fcttree_replace_id(FitFctTree *t, int id, int new_value, int new_sel);
FitEntity *entity_simplified_copy(FitEntity *e);          /* copy with buffers, constants and duplicate locals removed */

/* ------------------------------------------------------------------ */
/* entity lookups                                                      */
/* ------------------------------------------------------------------ */

int   entity_has_input_id(FitEntity *e, int id);
int   var_id_eq(void *var, void *id);                     /* FitListEq: FitVariable.id against FIT_INT2PTR(id) */
int   fct_id_eq(void *fct, void *id);                     /* FitListEq: FitFct.var_id (first int of the item) against an id */
FitInstance *netlist_find_instance(FitEntity *e, const char *out_name); /* by q, q2 or q3 */
FitNetItem *netlist_find_item(FitEntity *e, const char *out_name);
int   entity_var_id_by_name(FitEntity *e, const char *name); /* locals, outputs, inputs; 0 if absent */
FitVariable *entity_var_by_id(FitEntity *e, int id);      /* locals, outputs, inputs */
int   netlist_has_name(FitEntity *e, const char *name);
int   netlist_alias_target_id(FitEntity *e, const char *name);
int   alias_name_eq(void *alias, void *name);             /* FitListEq: first char * of the item against a name */
int   entity_count_fct_using(FitEntity *e, int id);
int   entity_var_is_node(FitEntity *e, FitVariable *v);   /* 1 if v is a local or aliased to one */
void  netlist_absorb_tristates(FitEntity *e);             /* merges kind-8 items into the register they follow */
void  entity_toggle_inv(FitEntity *e, int id);            /* id <-> !id in every function */

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int   fit150x_main(int argc, char **argv);

#endif /* FIT150X_BASE_H */
