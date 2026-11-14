/*
 * base_var.c - the signal object (FitVariable) of the ATF150x fitter and
 * the searches for it by name and id.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base_int.h"
#include "tt.h"

/* a plain variable prints its name, kind 1 "name[a:b]" with the
 * first two words of its attribute array, kind 2 "name[a,b,...]" with the
 * attributes printed as numbers. */
void var_print(FILE *out, FitVariable *v)
{
    if (v == NULL)
        return;
    if (v->kind == 0) {
        fprintf(out, "%s", v->name);
    } else if (v->kind == 1) {
        int a = 0, b = 0;

        /* items[0] and items[1] are printed whatever the count is; the test
         * only keeps the reads inside the array */
        if (v->attrs != NULL && v->attrs->capacity > 1) {
            a = FIT_PTR2INT(v->attrs->items[0]);
            b = FIT_PTR2INT(v->attrs->items[1]);
        }
        fprintf(out, "%s[%d:%d]", v->name, a, b);
    } else if (v->kind == 2) {
        fprintf(out, "%s[", v->name);
        list_print(out, v->attrs, list_print_int, ",");
        fprintf(out, "]");
    }
}

/* new variable of the given kind; kinds 1 and 2 get an attribute list */
FitVariable *var_new(int kind)
{
    FitVariable *v = util_calloc(1, sizeof(*v));

    v->kind = (char)kind;
    if (kind == 1 || kind == 2)
        v->attrs = list_create();
    else
        v->attrs = NULL;
    v->cls = 0;
    v->aux = -1;
    v->pin = -1;
    v->has_ext = 0;
    v->ext = NULL;
    v->polarity = 1;
    return v;
}

void var_free(FitVariable **v)
{
    if (*v != NULL) {
        if ((*v)->attrs != NULL)
            list_free_all(&(*v)->attrs, util_free_ptr);
        if ((*v)->ext != NULL)
            free((*v)->ext);
        free(*v);
        *v = NULL;
    }
}

/* n new plain variables with generated names (util_gen_name of
 * name_kind, numbers from first_number on, skipping names that `existing`
 * already has with a nonzero id) and consecutive ids taken from *next_id. */
FitList *vars_new_generated(int n, int *next_id, int name_kind, int first_number, FitList *existing)
{
    FitList *out = list_create();

    for (; n > 0; n--) {
        char *name = util_gen_name(name_kind, first_number);
        FitVariable *v;

        first_number++;
        while (var_list_id_of_name(existing, name) != 0) {
            free(name);
            name = util_gen_name(name_kind, first_number);
            first_number++;
        }
        v = var_new(0);
        strcpy(v->name, name);
        free(name);
        v->id = *next_id;
        (*next_id)++;
        list_add(out, v);
    }
    return out;
}

/* copy of v; NULL for NULL */
FitVariable *var_copy(FitVariable *v)
{
    FitVariable *c;

    if (v == NULL)
        return NULL;
    c = util_calloc(1, sizeof(*c));
    c->kind = v->kind;
    strcpy(c->name, v->name);
    c->id = v->id;
    c->cls = v->cls;
    c->polarity = v->polarity;
    c->has_ext = v->has_ext;
    c->aux = v->aux;
    c->pin = v->pin;
    if (v->has_ext == 1) {
        c->ext = util_calloc(1, sizeof(*c->ext));
        if (v->ext != NULL)
            *c->ext = *v->ext;
    }
    /* the attribute items are shared with v, not copied */
    c->attrs = list_map_copy(v->attrs, list_identity);
    return c;
}

/* list comparator on FitVariable.id */
int var_cmp_id(void *a, void *b)
{
    int x = ((FitVariable *)a)->id;
    int y = ((FitVariable *)b)->id;

    if (x < y)
        return -1;
    return x > y;
}

/* id of the variable called `name` (ignoring case), 0 if none */
int var_list_id_of_name(FitList *vars, const char *name)
{
    for (int i = 0; i < (vars == NULL ? 0 : vars->count); i++) {
        FitVariable *v = vars->items[i];

        if (util_str_icmp(v->name, name) == 0)
            return v->id;
    }
    return 0;
}

FitVariable *var_list_find_name(FitList *vars, const char *name)
{
    for (int i = 0; i < (vars == NULL ? 0 : vars->count); i++) {
        FitVariable *v = vars->items[i];

        if (util_str_icmp(v->name, name) == 0)
            return v;
    }
    return NULL;
}

/* searches inputs, outputs and locals, in this order */
FitVariable *entity_find_var(FitEntity *e, const char *name)
{
    FitVariable *v;

    v = var_list_find_name(e->inputs, name);
    if (v != NULL)
        return v;
    v = var_list_find_name(e->outputs, name);
    if (v != NULL)
        return v;
    return var_list_find_name(e->locals, name);
}

FitVariable *var_list_find_id(FitList *vars, int id)
{
    for (int i = 0; i < (vars == NULL ? 0 : vars->count); i++) {
        FitVariable *v = vars->items[i];

        if (v->id == id)
            return v;
    }
    return NULL;
}

/* 1-based position of the last variable with this id, 0 if none */
int var_list_pos_of_id(FitList *vars, int id)
{
    int count = vars == NULL ? 0 : vars->count;

    for (int i = count - 1; i >= 0; i--) {
        if (((FitVariable *)vars->items[i])->id == id)
            return i + 1;
    }
    return 0;
}

/* list callback: two variables with the same name (ignoring case) */
int var_same_name(void *a, void *b)
{
    if (a == NULL || b == NULL)
        return a == b;
    return util_str_icmp(((FitVariable *)a)->name, ((FitVariable *)b)->name) == 0;
}

/* a new plain variable whose id is the entity's next free id and
 * whose name is "XXL_<n>", n counting up from that id until the name is
 * unused in the entity. */
FitVariable *var_new_unique(FitEntity *e)
{
    int n = tt2_next_var_id(e);
    FitVariable *v = var_new(0);

    v->id = n;
    snprintf(v->name, sizeof(v->name), "%s%d", "XXL_", n);
    while (entity_find_var(e, v->name) != NULL) {
        n++;
        snprintf(v->name, sizeof(v->name), "%s%d", "XXL_", n);
    }
    return v;
}

/* base itself if the entity has no variable of that name, else
 * "<base>0", "<base>_1", "<base>_2", ... - the first one that is free. */
static char *var_unique_name(FitEntity *e, const char *base)
{
    char *name;
    size_t size;
    int n = 0;

    if (entity_find_var(e, base) == NULL)
        return util_strdup(base);
    size = strlen(base) + 1 + 0xb;
    name = util_malloc(size);
    snprintf(name, size, "%s%d", base, 0);
    while (entity_find_var(e, name) != NULL) {
        n++;
        snprintf(name, size, "%s_%d", base, n);
    }
    return name;
}

/* name v "<base><suffix>", made unique in e (suffix may be NULL) */
void var_set_unique_name(FitEntity *e, FitVariable *v, const char *base, const char *suffix)
{
    char *wanted = util_strdup(base);
    char *name;

    if (suffix != NULL)
        util_str_append(&wanted, suffix);
    name = var_unique_name(e, wanted);
    util_str_copy_n(v->name, name, 0x100, 0);
    free(name);
    free(wanted);
}

int entity_var_id(FitEntity *e, const char *name)
{
    FitVariable *v = entity_find_var(e, name);

    if (v == NULL)
        return 0;
    return v->id;
}

int var_has_name(void *var, void *name)
{
    return util_str_ieq(((FitVariable *)var)->name, name);
}
