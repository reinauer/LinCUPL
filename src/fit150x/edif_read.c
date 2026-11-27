/* Read EDIF netlists, resolve references, and flatten hierarchy before
 * interpreting primitive cells. Display names never participate in EDIF
 * symbol lookup; identifiers and keywords are case insensitive. */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include "edif_priv.h"

#define ED_MEMORY_LIMIT (64u * 1024u * 1024u)
#define ED_DEPTH_LIMIT 128
#define ED_WIRE_LIMIT 65536

struct EdBlock { void *data; EdBlock *next; };

void ed_error(EdRead *r, EdNode *node, const char *format, ...)
{
    va_list args;
    fprintf(g_msg, "%s:%d: EDIF: ", r->filename,
            node != NULL ? node->line : r->line);
    va_start(args, format);
    vfprintf(g_msg, format, args);
    va_end(args);
    fputc('\n', g_msg);
    longjmp(r->failed, 1);
}

void *ed_alloc(EdRead *r, size_t size)
{
    EdBlock *block;
    if (r->allocated > ED_MEMORY_LIMIT - sizeof(*block) ||
        size > ED_MEMORY_LIMIT - sizeof(*block) - r->allocated)
        ed_error(r, NULL, "netlist exceeds the 64 MiB reader limit");
    block = util_calloc(1, sizeof(*block));
    block->data = util_calloc(size ? size : 1, 1);
    block->next = r->blocks;
    r->blocks = block;
    r->allocated += size + sizeof(*block);
    return block->data;
}

char *ed_string(EdRead *r, const char *text)
{
    char *copy = ed_alloc(r, strlen(text) + 1);
    strcpy(copy, text);
    return copy;
}

static void advance(EdRead *r)
{
    if (r->ch == '\n')
        r->line++;
    r->ch = fgetc(r->file);
    if (r->ch == EOF && ferror(r->file))
        ed_error(r, NULL, "error reading input");
}

static void whitespace(EdRead *r)
{
    while (r->ch != EOF && isspace((unsigned char)r->ch))
        advance(r);
}

static void token_char(EdRead *r, EdNode *n, char **token,
                       size_t *length, size_t *capacity, int ch)
{
    if (*length == 65535 || ch == 0)
        ed_error(r, n, "invalid or oversized token");
    if (*length + 1 >= *capacity) {
        size_t size = *capacity ? *capacity * 2 : 64;
        char *copy = ed_alloc(r, size);
        if (*length)
            memcpy(copy, *token, *length);
        *token = copy;
        *capacity = size;
    }
    (*token)[(*length)++] = (char)ch;
}

static EdNode *parse(EdRead *r, int depth)
{
    EdNode *n, **tail;
    char *token = NULL;
    size_t length = 0, capacity = 0;
    if (depth > ED_DEPTH_LIMIT)
        ed_error(r, NULL, "parentheses nested too deeply");
    if (++r->nodes > 1000000)
        ed_error(r, NULL, "too many syntax nodes");
    whitespace(r);
    n = ed_alloc(r, sizeof(*n));
    n->line = r->line;
    if (r->ch == '(') {
        advance(r);
        whitespace(r);
        tail = &n->child;
        while (r->ch != ')') {
            if (r->ch == EOF)
                ed_error(r, n, "unterminated list");
            *tail = parse(r, depth + 1);
            tail = &(*tail)->next;
            whitespace(r);
        }
        if (n->child == NULL || n->child->text == NULL || n->child->string)
            ed_error(r, n, "list must start with a keyword");
        advance(r);
        return n;
    }
    n->string = r->ch == '"';
    if (n->string)
        advance(r);
    while (r->ch != EOF) {
        int ch = r->ch;
        if (n->string ? ch == '"' : (isspace((unsigned char)ch) || ch == '(' || ch == ')'))
            break;
        if (n->string && ch == '%') {
            /* EDIF character escapes are decimal ASCII codes, not C
             * backslash escapes. One escape may contain several codes. */
            advance(r);
            do {
                unsigned value = 0;
                whitespace(r);
                if (!isdigit((unsigned char)r->ch))
                    ed_error(r, n, "invalid string escape");
                while (isdigit((unsigned char)r->ch)) {
                    value = value * 10 + (unsigned)(r->ch - '0');
                    if (value > 127)
                        ed_error(r, n, "string escape outside ASCII range");
                    advance(r);
                }
                token_char(r, n, &token, &length, &capacity, (int)value);
                whitespace(r);
            } while (r->ch != '%');
            advance(r);
            continue;
        }
        token_char(r, n, &token, &length, &capacity, ch);
        advance(r);
    }
    if (n->string) {
        if (r->ch != '"')
            ed_error(r, n, "unterminated string");
        advance(r);
    } else if (length == 0) {
        ed_error(r, n, "unexpected end of input or closing parenthesis");
    }
    if (!token)
        token = ed_alloc(r, 1);
    token[length] = '\0';
    n->text = token;
    return n;
}

int ed_is(EdNode *n, const char *tag)
{
    return n && n->child && n->child->text &&
           util_str_icmp(n->child->text, tag) == 0;
}

EdNode *ed_arg(EdNode *n, int index)
{
    EdNode *p = n ? n->child : NULL;
    while (p && index--)
        p = p->next;
    return p;
}

EdNode *ed_find(EdRead *r, EdNode *n, const char *tag)
{
    EdNode *found = NULL;
    for (EdNode *p = n ? n->child : NULL; p; p = p->next) {
        if (!ed_is(p, tag))
            continue;
        if (found)
            ed_error(r, p, "duplicate %s", tag);
        found = p;
    }
    return found;
}

const char *ed_atom(EdRead *r, EdNode *n)
{
    if (!n || !n->text || n->string)
        ed_error(r, n, "expected an identifier or integer");
    return n->text;
}

const char *ed_name(EdRead *r, EdNode *n, int display)
{
    if (ed_is(n, "rename")) {
        EdNode *label = ed_arg(n, 2);
        if (!label || !label->string || label->next)
            ed_error(r, n, "expected (rename identifier \"name\")");
        return display ? label->text : ed_atom(r, ed_arg(n, 1));
    }
    return ed_atom(r, n);
}

static int integer(EdRead *r, EdNode *n, int max)
{
    const char *s = ed_atom(r, n);
    char *end;
    long value;
    errno = 0;
    value = strtol(s, &end, 10);
    if (errno || !*s || *end || value < 0 || value > max)
        ed_error(r, n, "integer must be between 0 and %d", max);
    return (int)value;
}

static void level(EdRead *r, EdNode *parent, const char *tag)
{
    EdNode *n = ed_find(r, parent, tag);
    if (!n || integer(r, ed_arg(n, 1), 0) || ed_arg(n, 2))
        ed_error(r, parent, "%s 0 is required", tag);
}

EdPort *ed_port(EdRead *r, EdView *v, const char *name, EdNode *where)
{
    for (EdPort *p = v->ports; p; p = p->next)
        if (util_str_icmp(p->id, name) == 0)
            return p;
    ed_error(r, where, "unknown port '%s' in cell '%s'", name, v->cell);
    return NULL;
}

/* Parse declaration shape; EDIF member indices always address the array
 * declaration, independently of any display name containing a range. */
static void port(EdRead *r, EdView *v, EdNode *n)
{
    EdPort *p = ed_alloc(r, sizeof(*p)), **tail = &v->ports;
    EdNode *name = ed_arg(n, 1), *dir = ed_find(r, n, "direction");
    const char *direction = ed_atom(r, ed_arg(dir, 1));
    p->node = n;
    p->width = 1;
    if (ed_is(name, "array")) {
        for (EdNode *d = ed_arg(name, 2); d; d = d->next) {
            int size = integer(r, d, ED_WIRE_LIMIT);
            if (!size || p->rank == 8 || p->width > ED_WIRE_LIMIT / size)
                ed_error(r, d, "invalid or oversized port array");
            p->dims[p->rank++] = size;
            p->width *= size;
        }
        if (!p->rank)
            ed_error(r, name, "array needs a dimension");
        name = ed_arg(name, 1);
    }
    p->id = ed_name(r, name, 0);
    p->name = ed_name(r, name, 1);
    p->direction = util_str_ieq(direction, "INPUT") ? ED_INPUT :
                   util_str_ieq(direction, "OUTPUT") ? ED_OUTPUT :
                   util_str_ieq(direction, "INOUT") ? ED_INOUT : 0;
    if (!p->direction)
        ed_error(r, dir, "unknown port direction '%s'", direction);
    for (; *tail; tail = &(*tail)->next)
        if (util_str_ieq((*tail)->id, p->id))
            ed_error(r, n, "duplicate port '%s'", p->id);
    p->offset = v->width;
    v->width += p->width;
    if (v->width > ED_WIRE_LIMIT)
        ed_error(r, n, "too many port bits");
    *tail = p;
}

static void catalog(EdRead *r, EdNode *root)
{
    for (EdNode *lib = ed_arg(root, 2); lib; lib = lib->next) {
        if (!ed_is(lib, "library") && !ed_is(lib, "external"))
            continue;
        const char *library = ed_name(r, ed_arg(lib, 1), 0);
        level(r, lib, "edifLevel");
        for (EdNode *cell = ed_arg(lib, 2); cell; cell = cell->next) {
            if (!ed_is(cell, "cell"))
                continue;
            const char *id = ed_name(r, ed_arg(cell, 1), 0);
            for (EdNode *view = ed_arg(cell, 2); view; view = view->next) {
                EdView *v;
                EdNode *type, *interface;
                if (!ed_is(view, "view"))
                    continue;
                type = ed_find(r, view, "viewType");
                if (!util_str_ieq(ed_atom(r, ed_arg(type, 1)), "NETLIST"))
                    continue;
                v = ed_alloc(r, sizeof(*v));
                v->library = library;
                v->cell = id;
                v->name = ed_name(r, ed_arg(view, 1), 0);
                v->model = ed_name(r, ed_arg(cell, 1), 1);
                v->node = view;
                v->contents = ed_find(r, view, "contents");
                for (EdView *old = r->views; old; old = old->next)
                    if (util_str_ieq(old->library, library) &&
                        util_str_ieq(old->cell, id) && util_str_ieq(old->name, v->name))
                        ed_error(r, view, "duplicate view '%s/%s/%s'", library, id, v->name);
                interface = ed_find(r, view, "interface");
                if (!interface)
                    ed_error(r, view, "netlist view needs an interface");
                for (EdNode *p = ed_arg(interface, 1); p; p = p->next) {
                    if (ed_is(p, "port"))
                        port(r, v, p);
                    else if (!ed_is(p, "property") && !ed_is(p, "comment") &&
                             !ed_is(p, "designator"))
                        ed_error(r, p, "unsupported interface construct '%s'", ed_atom(r, ed_arg(p, 0)));
                }
                v->next = r->views;
                r->views = v;
            }
        }
    }
}

static EdView *resolve(EdRead *r, EdNode *ref, const char *default_lib)
{
    EdNode *cellref = ref, *libref;
    const char *view = NULL, *cell, *library;
    EdView *found = NULL;
    if (ed_is(ref, "viewRef")) {
        view = ed_atom(r, ed_arg(ref, 1));
        cellref = ed_find(r, ref, "cellRef");
    }
    if (!ed_is(cellref, "cellRef"))
        ed_error(r, ref, "expected a cellRef or viewRef");
    cell = ed_atom(r, ed_arg(cellref, 1));
    libref = ed_find(r, cellref, "libraryRef");
    library = libref ? ed_atom(r, ed_arg(libref, 1)) : default_lib;
    for (EdView *v = r->views; v; v = v->next) {
        if (!util_str_ieq(v->cell, cell) ||
            (library && !util_str_ieq(v->library, library)) ||
            (view && !util_str_ieq(v->name, view)))
            continue;
        if (found)
            ed_error(r, ref, "ambiguous reference to '%s'", cell);
        found = v;
    }
    if (!found)
        ed_error(r, ref, "unresolved cell/view reference '%s'", cell);
    return found;
}

EdWire *ed_root(EdWire *w)
{
    EdWire *root = w;
    while (root->parent != root)
        root = root->parent;
    while (w->parent != w) {
        EdWire *next = w->parent;
        w->parent = root;
        w = next;
    }
    return root;
}

static void join(EdWire *a, EdWire *b)
{
    a = ed_root(a);
    b = ed_root(b);
    if (a == b)
        return;
    if (a->rank < b->rank) {
        EdWire *tmp = a;
        a = b;
        b = tmp;
    }
    b->parent = a;
    a->connected |= b->connected;
    if (a->rank == b->rank)
        a->rank++;
}

static EdFrame *frame(EdRead *r, EdView *view, EdNode *node, const char *id, const char *name)
{
    EdFrame *f = ed_alloc(r, sizeof(*f));
    f->view = view;
    f->node = node;
    f->id = id;
    f->name = name;
    f->wires = ed_alloc(r, (size_t)view->width * sizeof(*f->wires));
    f->used = ed_alloc(r, (size_t)view->width);
    for (int i = 0; i < view->width; i++) {
        EdWire *wire;
        if (r->nwires == ED_WIRE_LIMIT)
            ed_error(r, node, "flattened netlist exceeds %d wires", ED_WIRE_LIMIT);
        wire = ed_alloc(r, sizeof(*wire));
        wire->number = ++r->nwires;
        wire->parent = wire;
        f->wires[i] = wire;
        list_add(r->wires, wire);
    }
    return f;
}

static char *member_name(EdRead *r, const char *base, int rank, const int *indices)
{
    size_t size = strlen(base) + 100;
    char *name = ed_alloc(r, size);
    strcpy(name, base);
    for (int i = 0; i < rank; i++) {
        size_t length = strlen(name);
        snprintf(name + length, size - length, "[%d]", indices[i]);
    }
    return name;
}

static const char *instance_ref(EdRead *r, EdNode *n)
{
    int indices[8], rank = 0;
    if (!ed_is(n, "member"))
        return ed_atom(r, n);
    for (EdNode *p = ed_arg(n, 2); p; p = p->next) {
        if (rank == 8)
            ed_error(r, n, "too many instance array indices");
        indices[rank++] = integer(r, p, ED_WIRE_LIMIT - 1);
    }
    if (!rank)
        ed_error(r, n, "missing instance array index");
    return member_name(r, ed_atom(r, ed_arg(n, 1)), rank, indices);
}

static EdWire **connection(EdRead *r, EdFrame *f, EdFrame *children, EdNode *ref, int *width)
{
    EdNode *instance = ed_find(r, ref, "instanceRef"), *member = ed_arg(ref, 1);
    EdPort *p;
    int offset = 0;
    if (!ed_is(ref, "portRef"))
        ed_error(r, ref, "joined requires portRef entries");
    if (instance) {
        const char *id = instance_ref(r, ed_arg(instance, 1));
        for (f = children; f; f = f->next)
            if (util_str_ieq(f->id, id))
                break;
        if (!f)
            ed_error(r, instance, "unknown instance '%s'", id);
    }
    if (ed_is(member, "member")) {
        EdNode *index = ed_arg(member, 2);
        p = ed_port(r, f->view, ed_atom(r, ed_arg(member, 1)), ref);
        for (int d = 0; d < p->rank; d++) {
            offset = offset * p->dims[d] + integer(r, index, p->dims[d] - 1);
            index = index->next;
        }
        if (!p->rank || index)
            ed_error(r, member, "wrong number of array indices");
        *width = 1;
    } else {
        p = ed_port(r, f->view, ed_atom(r, member), ref);
        *width = p->width;
    }
    return f->wires + p->offset + offset;
}

static void flatten(EdRead *r, EdFrame *f, int depth)
{
    EdView *v = f->view;
    EdFrame *children = NULL, **tail = &children;
    if (v->active || depth > ED_DEPTH_LIMIT)
        ed_error(r, f->node, "recursive or excessively deep hierarchy at '%s'", f->name);
    if (!v->contents) {
        f->leaf_next = r->leaves;
        r->leaves = f;
        return;
    }
    v->active = 1;
    for (EdNode *n = ed_arg(v->contents, 1); n; n = n->next) {
        if (ed_is(n, "instance")) {
            EdNode *name = ed_arg(n, 1);
            int dims[8], rank = 0, count = 1;
            const char *id, *label;
            EdView *child = resolve(r, ed_find(r, n, "viewRef"), v->library);
            if (ed_is(name, "array")) {
                for (EdNode *d = ed_arg(name, 2); d; d = d->next) {
                    int size = integer(r, d, ED_WIRE_LIMIT);
                    if (!size || rank == 8 || count > ED_WIRE_LIMIT / size)
                        ed_error(r, name, "invalid instance array dimensions");
                    dims[rank++] = size;
                    count *= size;
                }
                if (!rank)
                    ed_error(r, name, "instance array needs a dimension");
                name = ed_arg(name, 1);
            }
            id = ed_name(r, name, 0);
            label = ed_name(r, name, 1);
            if (ed_find(r, n, "parameterAssign"))
                ed_error(r, n, "parameterized cells must be elaborated before fitting");
            for (int bit = 0; bit < count; bit++) {
                int indices[8], value = bit;
                const char *key = id, *display = label;
                char *path;
                for (int d = rank - 1; d >= 0; d--) {
                    indices[d] = value % dims[d];
                    value /= dims[d];
                }
                if (rank) {
                    key = member_name(r, id, rank, indices);
                    display = member_name(r, label, rank, indices);
                }
                for (EdFrame *old = children; old; old = old->next)
                    if (util_str_ieq(old->id, key))
                        ed_error(r, n, "duplicate instance '%s'", key);
                path = ed_alloc(r, strlen(f->name) + strlen(display) + 2);
                snprintf(path, strlen(f->name) + strlen(display) + 2,
                         "%s/%s", f->name, display);
                *tail = frame(r, child, n, key, path);
                tail = &(*tail)->next;
            }
        } else if (!ed_is(n, "net") && !ed_is(n, "comment") &&
                   !ed_is(n, "property")) {
            ed_error(r, n, "unsupported contents construct '%s'", ed_atom(r, ed_arg(n, 0)));
        }
    }
    for (EdNode *n = ed_arg(v->contents, 1); n; n = n->next) {
        EdNode *joined;
        EdNode *name;
        EdWire **first = NULL;
        int count = 0, declared_width = 1;
        if (!ed_is(n, "net"))
            continue;
        name = ed_arg(n, 1);
        if (ed_is(name, "array")) {
            if (!ed_arg(name, 2))
                ed_error(r, name, "net array needs a dimension");
            for (EdNode *d = ed_arg(name, 2); d; d = d->next) {
                int size = integer(r, d, ED_WIRE_LIMIT);
                if (!size || declared_width > ED_WIRE_LIMIT / size)
                    ed_error(r, name, "invalid net array dimensions");
                declared_width *= size;
            }
            name = ed_arg(name, 1);
        }
        for (EdNode *old = ed_arg(v->contents, 1); old != n; old = old->next) {
            EdNode *other;
            if (!ed_is(old, "net")) continue;
            other = ed_arg(old, 1);
            if (ed_is(other, "array")) other = ed_arg(other, 1);
            if (util_str_ieq(ed_name(r, other, 0), ed_name(r, name, 0)))
                ed_error(r, n, "duplicate net '%s'", ed_name(r, name, 0));
        }
        for (EdNode *p = ed_arg(n, 2); p; p = p->next)
            if (!ed_is(p, "joined") && !ed_is(p, "property") &&
                !ed_is(p, "comment") && !ed_is(p, "criticality"))
                ed_error(r, p, "unsupported net construct '%s'", ed_atom(r, ed_arg(p, 0)));
        joined = ed_find(r, n, "joined");
        if (!joined)
            ed_error(r, n, "net requires joined connections");
        for (EdNode *p = ed_arg(joined, 1); p; p = p->next) {
            int width;
            EdWire **bits = connection(r, f, children, p, &width);
            if (width != declared_width)
                ed_error(r, p, "net and port array widths differ");
            if (!first) {
                first = bits;
                count = width;
            } else if (count != width) {
                ed_error(r, p, "connected arrays have different widths");
            }
            for (int i = 0; i < width; i++) {
                join(first[i], bits[i]);
                ed_root(bits[i])->connected = 1;
            }
        }
    }
    for (EdFrame *child = children; child; child = child->next)
        flatten(r, child, depth + 1);
    v->active = 0;
}

static const char *bit_name(EdRead *r, EdPort *p, int bit)
{
    char *name;
    size_t size = strlen(p->name) + 100;
    int indices[8], value = bit;
    if (!p->rank)
        return p->name;
    name = ed_alloc(r, size);
    if (p->rank == 1) {
        const char *open = strrchr(p->name, '[');
        char close = ']';
        char *colon, *end;
        long first, last;
        if (!open) { open = strrchr(p->name, '('); close = ')'; }
        if (!open) { open = strrchr(p->name, '<'); close = '>'; }
        if (open) {
            errno = 0;
            first = strtol(open + 1, &colon, 10);
            last = *colon == ':' ? strtol(colon + 1, &end, 10) : 0;
            if (!errno && colon > open + 1 && *colon == ':' &&
                end > colon + 1 && *end == close && !end[1] &&
                first >= -1000000000 && first <= 1000000000 &&
                last >= -1000000000 && last <= 1000000000) {
                if (labs(last - first) + 1 != p->width)
                    ed_error(r, p->node, "display range does not match array width");
                snprintf(name, size, "%.*s[%ld]", (int)(open - p->name), p->name,
                         first + (last >= first ? bit : -bit));
                return name;
            }
        }
    }
    strcpy(name, p->name);
    for (int d = p->rank - 1; d >= 0; d--) {
        indices[d] = value % p->dims[d];
        value /= p->dims[d];
    }
    for (int d = 0; d < p->rank; d++) {
        size_t len = strlen(name);
        snprintf(name + len, size - len, "[%d]", indices[d]);
    }
    return name;
}

static void check_properties(EdRead *r, EdNode *node)
{
    for (EdNode *n = node; n; n = n->next) {
        if (ed_is(n, "property") &&
            util_str_ieq(ed_name(r, ed_arg(n, 1), 1), "INIT"))
            ed_error(r, n, "initial state is not supported; use an explicit reset");
        if (n->child)
            check_properties(r, n->child);
    }
}

int edif_read(FILE *file, FitEntity **entity, FitList *params)
{
    EdRead *r = util_calloc(1, sizeof(*r));
    int success = 0;
    r->file = file;
    r->filename = g_in_name ? g_in_name : "input";
    r->line = 1;
    r->wires = list_create();
    *entity = NULL;
    if (setjmp(r->failed) == 0) {
        EdNode *root, *version, *design;
        EdFrame *top;
        EdTop **tail = &r->ports;
        int pins = 0;
        r->ch = fgetc(file);
        root = parse(r, 0);
        whitespace(r);
        if (r->ch != EOF || !ed_is(root, "edif"))
            ed_error(r, root, "expected one edif document");
        version = ed_find(r, root, "edifVersion");
        if (!version || integer(r, ed_arg(version, 1), 2) != 2 ||
            integer(r, ed_arg(version, 2), 0) || integer(r, ed_arg(version, 3), 0))
            ed_error(r, root, "only EDIF version 2 0 0 is supported");
        level(r, root, "edifLevel");
        level(r, ed_find(r, root, "keywordMap"), "keywordLevel");
        check_properties(r, root);
        catalog(r, root);
        design = ed_find(r, root, "design");
        if (!design)
            ed_error(r, root, "missing design selection");
        top = frame(r, resolve(r, ed_find(r, design, "cellRef"), NULL), design,
                    ed_name(r, ed_arg(design, 1), 0), ed_name(r, ed_arg(design, 1), 1));
        if (!top->view->contents)
            ed_error(r, design, "top design has no contents");
        flatten(r, top, 0);
        for (EdPort *p = top->view->ports; p; p = p->next) {
            for (int i = 0; i < p->width; i++) {
                if (++pins > 256)
                    ed_error(r, p->node, "too many package ports for an ATF150x device");
                EdTop *pin = ed_alloc(r, sizeof(*pin));
                pin->name = bit_name(r, p, i);
                pin->node = p->node;
                pin->direction = p->direction;
                pin->wire = ed_root(top->wires[p->offset + i]);
                *tail = pin;
                tail = &pin->next;
            }
        }
        ed_lower(r, params);
        *entity = r->entity;
        r->entity = NULL;
        success = 1;
    }
    entity_free(&r->entity);
    if (r->pin_file)
        fclose(r->pin_file);
    list_destroy(&r->wires);
    list_free_all(&r->library, lib_gate_free);
    lib_header_free(&r->lib_header);
    while (r->blocks) {
        EdBlock *next = r->blocks->next;
        free(r->blocks->data);
        free(r->blocks);
        r->blocks = next;
    }
    free(r);
    return success;
}
