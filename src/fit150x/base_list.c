/*
 * base_list.c - the pointer list of the ATF150x fitter, and the stack of
 * message streams.
 *
 * The fitter keeps its own list package and merge sort because its output
 * depends on them: every operation defines the element order it leaves
 * behind, and list_sort makes a fixed sequence of comparator calls with
 * fixed arguments, which decides the order of equal items.  Placement
 * results depend on that order.  Do not replace anything here by a library
 * routine.
 */
#include <stdio.h>
#include <stdlib.h>

#include "base_int.h"

/* stack of message streams saved by msg_redirect_to */
static FitList *g_msg_stack;
/* the stream that was current before the last redirection */
static FILE *g_msg_saved;
/* 1 while messages are redirected (written, never read) */
static int g_msg_redirected;

void *list_identity(void *item)
{
    return item;
}

void list_print_int(FILE *out, void *item)
{
    fprintf(out, "%d", FIT_PTR2INT(item));
}

int list_ptr_eq(void *a, void *b)
{
    return a == b;
}

/* compares two integers stored in list slots: -1, 0 or 1 */
int list_int_cmp(void *a, void *b)
{
    int x = FIT_PTR2INT(a);
    int y = FIT_PTR2INT(b);

    if (x < y)
        return -1;
    return x > y;
}

int list_contains_ptr(FitList *l, void *item)
{
    for (int i = 0; i < l->count; i++) {
        if (l->items[i] == item)
            return 1;
    }
    return 0;
}

/* 1-based position of the lowest set bit of v, 0 if v is 0 */
unsigned util_lowest_set_bit(unsigned v)
{
    for (unsigned n = 1; n < 0x21; n++) {
        if ((v & (1u << ((n - 1) & 0x1f))) != 0)
            return n;
    }
    return 0;
}

/* new empty list with room for `capacity` items (at least 1) */
FitList *list_create_sized(int capacity)
{
    FitList *l = util_calloc(1, sizeof(*l));

    if (capacity == 0)
        capacity = 1;
    if (capacity < 0)       /* a negative size is treated like 0 */
        capacity = 1;
    l->capacity = capacity;
    l->count = 0;
    l->items = util_calloc((size_t)capacity, sizeof(void *));
    return l;
}

FitList *list_create(void)
{
    return list_create_sized(0x10);
}

/* frees the item array and the header, not the items; *l = NULL */
void list_destroy(FitList **l)
{
    if (*l != NULL) {
        free((*l)->items);
        free(*l);
        *l = NULL;
    }
}

/* removes item pos (1-based), the later items move up; pos < 1 does
 * nothing.  0 after an error message if pos is past the end. */
int list_del_shift(FitList *l, int pos)
{
    if (pos > 0) {
        if (l->count < pos) {
            util_report_error(FIT_ERR_BAD_INDEX, "Function ListDelShift");
            return 0;
        }
        for (; pos < l->count; pos++)
            l->items[pos - 1] = l->items[pos];
        l->count--;
    }
    return 1;
}

/* list_del_shift after fn(&slot) has released the item */
int list_del_shift_free(FitList *l, int pos, FitListFree fn)
{
    if (pos > 0) {
        if (l->count < pos) {
            util_report_error(FIT_ERR_BAD_INDEX, "Function ListDelShiftFree");
            return 0;
        }
        fn(&l->items[pos - 1]);
        for (; pos < l->count; pos++)
            l->items[pos - 1] = l->items[pos];
        l->count--;
    }
    return 1;
}

/* removes item pos (1-based) by moving the last item into its place */
int list_del_insert(FitList *l, int pos)
{
    int count;

    if (pos < 1)
        return 1;
    count = l == NULL ? 0 : l->count;
    if (count < pos) {
        util_report_error(FIT_ERR_BAD_INDEX, "Function ListDelInsert");
        return 0;
    }
    l->items[pos - 1] = l->items[l->count - 1];
    l->count--;
    return 1;
}

/* appends item; the capacity doubles when the list is full */
void list_add(FitList *l, void *item)
{
    if (l->capacity <= l->count) {
        l->capacity *= 2;
        l->items = util_realloc(l->items, (size_t)l->capacity * sizeof(void *));
    }
    l->items[l->count] = item;
    l->count++;
}

/* 1-based position of the first item with eq(item, key) != 0, else 0 */
int list_find(FitList *l, void *key, FitListEq eq)
{
    if (l != NULL) {
        for (int i = 0; i < l->count; i++) {
            if (eq(l->items[i], key) != 0)
                return i + 1;
        }
    }
    return 0;
}

/* list_find with a three-argument callback */
int list_find_x(FitList *l, void *key, FitListEq3 eq, void *extra)
{
    /* the count is read again on every round */
    for (int i = 0; i < (l == NULL ? 0 : l->count); i++) {
        if (eq(l->items[i], key, extra) != 0)
            return i + 1;
    }
    return 0;
}

/* fn(&slot) for every item, then list_destroy */
void list_free_all(FitList **l, FitListFree fn)
{
    if (*l != NULL) {
        /* the end of the array is computed once, before the first call */
        int count = (*l)->count;

        for (int i = 0; i < count; i++)
            fn(&(*l)->items[i]);
        list_destroy(l);
    }
}

/* drops the NULL items, keeping the order; returns the new count */
int list_compact(FitList *l)
{
    int n = 0;

    if (l == NULL || l->count == 0)
        return 0;
    for (int i = 0; i < l->count; i++) {
        if (l->items[i] != NULL)
            l->items[n++] = l->items[i];
    }
    l->count = n;
    return n;
}

/* 1 if every item of a has a match in b and every item of b has a
 * match in a.  The callback gets (item of the list being searched, item
 * looked for). */
int list_equal_sets(FitList *a, FitList *b, FitListEq eq)
{
    int i, j;

    if ((a == NULL || a->count == 0) && (b == NULL || b->count == 0))
        return 1;
    if (a == NULL || a->count == 0 || b == NULL || b->count == 0)
        return 0;
    i = 0;
    do {
        j = 0;
        for (;;) {
            if (j == b->count)
                return 0;
            if (eq(b->items[j++], a->items[i]) != 0)
                break;
        }
        i++;
    } while (i < a->count);
    j = 0;
    i = 0;
    while (i != a->count) {
        if (eq(a->items[i++], b->items[j]) != 0) {
            j++;
            i = 0;
            if (j >= b->count)
                return 1;
        }
    }
    return 0;
}

/* like list_equal_sets with a three-argument callback.  NULL or empty
 * lists give 0. */
int list_equal_sets_x(FitList *a, FitList *b, FitListEq3 eq, void *extra)
{
    int i, j;

    if (a == NULL || b == NULL || a->count <= 0)
        return 0;
    i = 0;
    do {
        j = 0;
        for (;;) {
            if (j == b->count)
                return 0;
            if (eq(b->items[j++], a->items[i], extra) != 0)
                break;
        }
        i++;
    } while (i < a->count);
    j = 0;
    do {
        i = 0;
        for (;;) {
            if (i == a->count)
                return 0;
            if (eq(a->items[i++], b->items[j], extra) != 0)
                break;
        }
        j++;
    } while (j < b->count);
    return 1;
}

/* new list of copy(item) for every item; NULL for NULL */
FitList *list_map_copy(FitList *l, FitListCopy copy)
{
    FitList *out;
    int count;

    if (l == NULL)
        return NULL;
    out = list_create_sized(l->count);
    count = l->count;
    for (int i = 0; i < count; i++)
        list_add(out, copy(l->items[i]));
    return out;
}

/* list_map_copy with a two-argument copy callback */
FitList *list_map_copy_x(FitList *l, FitListCopy2 copy, void *extra)
{
    FitList *out;
    int count;

    if (l == NULL)
        return NULL;
    out = list_create_sized(l->count);
    count = l->count;
    for (int i = 0; i < count; i++)
        list_add(out, copy(l->items[i], extra));
    return out;
}

/* shallow copy (capacity 16); NULL for NULL */
FitList *list_copy(FitList *l)
{
    FitList *out;

    if (l == NULL)
        return NULL;
    out = list_create();
    for (int i = 0; i < l->count; i++)
        list_add(out, l->items[i]);
    return out;
}

/* appends the items of *src to dst and destroys *src */
void list_append_destroy(FitList *dst, FitList **src)
{
    if (*src != NULL) {
        int count = (*src)->count;

        for (int i = 0; i < count; i++)
            list_add(dst, (*src)->items[i]);
        list_destroy(src);
    }
}

/* appends copy(item) for every item of src */
void list_append_mapped(FitList *dst, FitList *src, FitListCopy copy)
{
    if (src != NULL) {
        int count = src->count;

        for (int i = 0; i < count; i++)
            list_add(dst, copy(src->items[i]));
    }
}

void list_append(FitList *dst, FitList *src)
{
    if (src != NULL) {
        int count = src->count;

        for (int i = 0; i < count; i++)
            list_add(dst, src->items[i]);
    }
}

/* prints the items with `print`, separated by sep */
void list_print(FILE *out, FitList *l, FitListPrint print, const char *sep)
{
    if (l != NULL && l->count != 0) {
        int count;

        print(out, l->items[0]);
        count = l->count;
        for (int i = 1; i < count; i++) {
            fprintf(out, "%s", sep);
            print(out, l->items[i]);
        }
    }
}

void list_print_x(FILE *out, FitList *l, FitListPrint2 print, const char *sep, void *extra)
{
    if (l != NULL && l->count != 0) {
        int count;

        print(out, l->items[0], extra);
        count = l->count;
        for (int i = 1; i < count; i++) {
            fprintf(out, "%s", sep);
            print(out, l->items[i], extra);
        }
    }
}

/* 0 as soon as eq(items[i], items[j]) holds for some i < j */
int list_all_distinct(FitList *l, FitListEq eq)
{
    if (l != NULL) {
        for (int i = 0; i < l->count; i++) {
            for (int j = i + 1; j < l->count; j++) {
                if (eq(l->items[i], l->items[j]) != 0)
                    return 0;
            }
        }
    }
    return 1;
}

/* new list of the items of a found in b; NULL if none */
FitList *list_intersection(FitList *a, FitList *b, FitListEq eq)
{
    FitList *out;

    if (a == NULL || b == NULL)
        return NULL;
    out = list_create();
    for (int i = 0; i < a->count; i++) {
        if (list_find(b, a->items[i], eq) != 0)
            list_add(out, a->items[i]);
    }
    if (out != NULL && out->count != 0)
        return out;
    list_destroy(&out);
    return NULL;
}

/* copies of the items of a found in b; NULL if none */
FitList *list_intersection_mapped(FitList *a, FitList *b, FitListEq3 eq, FitListCopy2 copy, void *extra)
{
    FitList *out;

    if (a == NULL || b == NULL)
        return NULL;
    out = list_create_sized(2);
    for (int i = 0; i < a->count; i++) {
        if (list_find_x(b, a->items[i], eq, extra) != 0)
            list_add(out, copy(a->items[i], extra));
    }
    if (out != NULL && out->count != 0)
        return out;
    list_destroy(&out);
    return NULL;
}

/* copy of a plus the items of b not found in a; NULL if empty */
FitList *list_union(FitList *a, FitList *b, FitListEq eq)
{
    FitList *out;

    if (a == NULL) {
        out = list_copy(b);
    } else if (b == NULL) {
        out = list_copy(a);
    } else {
        out = list_copy(a);
        for (int i = 0; i < b->count; i++) {
            if (list_find(a, b->items[i], eq) == 0)
                list_add(out, b->items[i]);
        }
    }
    if (out == NULL || out->count == 0) {
        list_destroy(&out);
        out = NULL;
    }
    return out;
}

/* sort items[lo..hi]; ascending != 0 sorts upwards, 0 downwards.
 * Ranges of two and three items are sorted by explicit comparisons, longer
 * ones are halved, sorted and merged through a temporary list.  The order
 * and the arguments of the comparator calls are fixed on purpose: they
 * decide where equal items end up, and the fitter's results depend on it. */
static void list_sort_range(FitList *l, int lo, int hi, int ascending, FitListCmp cmp)
{
    void **it = l->items;
    void *t;
    int r;

    if (hi == lo + 2) {
        void *first, *last;

        r = cmp(it[lo], it[lo + 1]);
        last = l->items[hi];
        first = l->items[lo];
        if (r < 1) {
            r = cmp(first, last);
            it = l->items;
            if (r >= 0) {
                if (ascending == 0) {
                    t = it[lo + 1];
                } else {
                    t = it[hi];
                    it[hi] = it[lo + 1];
                }
                it[lo + 1] = it[lo];
                it[lo] = t;
                return;
            }
            r = cmp(it[lo + 1], it[hi]);
            it = l->items;
            if (r >= 0) {
                t = it[hi];
                if (ascending == 0) {
                    it[hi] = it[lo];
                    it[lo] = it[lo + 1];
                    it[lo + 1] = t;
                    return;
                }
                it[hi] = it[lo + 1];
                it[lo + 1] = t;
                return;
            }
            if (ascending != 0)
                return;
        } else {
            r = cmp(first, last);
            it = l->items;
            if (r < 1) {
                if (ascending == 0) {
                    t = it[hi];
                    it[hi] = it[lo + 1];
                } else {
                    t = it[lo + 1];
                }
                it[lo + 1] = it[lo];
                it[lo] = t;
                return;
            }
            r = cmp(it[lo + 1], it[hi]);
            it = l->items;
            if (r < 0) {
                t = it[hi];
                if (ascending == 0) {
                    it[hi] = it[lo + 1];
                    it[lo + 1] = t;
                    return;
                }
                it[hi] = it[lo];
                it[lo] = it[lo + 1];
                it[lo + 1] = t;
                return;
            }
            if (ascending == 0)
                return;
        }
        /* swap the two ends */
        t = it[hi];
        it[hi] = it[lo];
        it[lo] = t;
    } else if (hi == lo + 1) {
        r = cmp(it[lo], it[hi]);
        it = l->items;
        if (ascending == 0 ? r < 0 : r > 0) {
            t = it[hi];
            it[hi] = it[lo];
            it[lo] = t;
        }
    } else {
        int mid = (lo + hi) / 2;
        int right, left, k, n;
        FitList *tmp;

        if (hi <= lo)       /* cannot happen from list_sort */
            return;
        list_sort_range(l, lo, mid, ascending, cmp);
        right = mid + 1;
        list_sort_range(l, right, hi, ascending, cmp);
        left = lo;
        if (ascending == 0) {
            while (left <= mid && cmp(l->items[left], l->items[right]) >= 0)
                left++;
        } else {
            while (left <= mid && cmp(l->items[left], l->items[right]) < 1)
                left++;
        }
        k = left;
        tmp = list_create_sized(right - k);
        for (n = k; n < right; n++)
            list_add(tmp, l->items[n]);
        n = 0;
        if (ascending == 0) {
            while (n < tmp->count && right <= hi) {
                if (cmp(tmp->items[n], l->items[right]) < 0)
                    t = l->items[right++];
                else
                    t = tmp->items[n++];
                l->items[k++] = t;
            }
        } else {
            while (n < tmp->count && right <= hi) {
                if (cmp(tmp->items[n], l->items[right]) < 1)
                    t = tmp->items[n++];
                else
                    t = l->items[right++];
                l->items[k++] = t;
            }
        }
        while (n < tmp->count)
            l->items[k++] = tmp->items[n++];
        list_destroy(&tmp);
    }
}

void list_sort(FitList *l, int ascending, FitListCmp cmp)
{
    if (l != NULL && l->count > 1)
        list_sort_range(l, 0, l->count - 1, ascending, cmp);
}

/* new list of the items of a not found in b (never NULL) */
FitList *list_difference_x(FitList *a, FitList *b, FitListEq3 eq, void *extra)
{
    FitList *out = list_create();

    for (int i = 0; i < (a == NULL ? 0 : a->count); i++) {
        if (list_find_x(b, a->items[i], eq, extra) == 0)
            list_add(out, a->items[i]);
    }
    return out;
}

/* mode 0 returns the first item that no later item exceeds
 * (cmp(later, best) > 0 replaces best), mode 1 the first minimum
 * (cmp(later, best) < 0 replaces best); any other mode the first item. */
void *list_extreme(FitList *l, int mode, FitListCmp cmp)
{
    int best = 0;

    if (l == NULL || l->count == 0)
        return NULL;
    for (int i = 1; i < l->count; i++) {
        if (mode == 0) {
            if (cmp(l->items[i], l->items[best]) > 0)
                best = i;
        } else if (mode == 1) {
            if (cmp(l->items[i], l->items[best]) < 0)
                best = i;
        }
    }
    return l->items[best];
}

/* inserts item so that it becomes position pos (1..count + 1) */
int list_insert_at(FitList *l, void *item, int pos)
{
    int count = l == NULL ? 0 : l->count;

    if (count + 1 < pos || pos < 1) {
        util_report_error(FIT_ERR_PLAIN, "in ListInsertInList !!\n");
        return 0;
    }
    list_add(l, item);
    for (int i = l->count - 1; i >= pos; i--)
        l->items[i] = l->items[i - 1];
    l->items[pos - 1] = item;
    return 1;
}

void list_add_unique(FitList *l, void *item, FitListEq eq)
{
    if (list_find(l, item, eq) == 0)
        list_add(l, item);
}

/* remember the current message stream and append the messages to
 * the file `path` from now on. */
void msg_redirect_to(const char *path)
{
    g_msg_saved = g_msg;
    if (g_msg_stack == NULL)
        g_msg_stack = list_create();
    list_add(g_msg_stack, g_msg_saved);
    g_msg = util_fopen_checked(path, "a");
    if (g_msg == NULL) {    /* not reached: util_fopen_checked does not return NULL */
        g_msg = g_msg_saved;
        util_report_error(FIT_ERR_LOG_OPEN, path);
        util_abort_job(1);
    }
    g_msg_redirected = 1;
    if (util_echo_note_stream(g_msg_saved) == 0)
        return;
    util_echo_off();
    util_echo_set(g_msg);
    util_echo_restore();
}

/* close the log and go back to the stream saved by
 * msg_redirect_to. */
void msg_restore(void)
{
    util_fclose(g_msg);
    g_msg = g_msg_saved;
    list_del_insert(g_msg_stack, g_msg_stack == NULL ? 0 : g_msg_stack->count);
    if (g_msg_stack == NULL || g_msg_stack->count == 0)
        list_destroy(&g_msg_stack);
    else
        g_msg_saved = g_msg_stack->items[g_msg_stack->count - 1];
    g_msg_redirected = 0;
    if (util_echo_was_stream() == 0)
        return;
    util_echo_off();
    util_echo_set(g_msg);
    util_echo_restore();
}

/* reset the statics of this file at the start of a run */
void base_list_reset(void)
{
    list_destroy(&g_msg_stack);
    g_msg_saved = NULL;
    g_msg_redirected = 0;
    (void)g_msg_redirected;
}
