/* EDIF 2 0 0 netlist reader. Internal structures own no fitter objects. */
#ifndef FIT150X_EDIF_PRIV_H
#define FIT150X_EDIF_PRIV_H

#include <setjmp.h>
#include "tt_priv.h"

typedef struct EdNode {
    char *text;
    int line, string;
    struct EdNode *child, *next;
} EdNode;

typedef struct EdPort {
    const char *id, *name;
    EdNode *node;
    int direction, offset, width, rank, dims[8];
    struct EdPort *next;
} EdPort;

typedef struct EdView {
    const char *library, *cell, *name, *model;
    EdNode *node, *contents;
    EdPort *ports;
    int width, active;
    struct EdView *next;
} EdView;

typedef struct EdWire {
    struct EdWire *parent;
    int rank, number, connected, drivers, read, sequential, pad;
    FitVariable *var;
} EdWire;

typedef struct EdFrame {
    const char *id, *name;
    EdView *view;
    EdNode *node;
    EdWire **wires;
    unsigned char *used;
    struct EdFrame *next, *leaf_next;
} EdFrame;

typedef struct EdTop {
    const char *name;
    EdNode *node;
    char mapped[FIT_NAME_MAX];
    int direction, pin;
    EdWire *wire;
    struct EdTop *next;
} EdTop;

typedef struct EdBlock EdBlock;
typedef struct EdRead {
    FILE *file, *pin_file;
    const char *filename;
    int ch, line, nodes, nwires;
    size_t allocated;
    jmp_buf failed;
    EdBlock *blocks;
    EdView *views;
    EdFrame *leaves;
    EdTop *ports;
    FitList *wires, *library;
    FitLibHeader *lib_header;
    FitEntity *entity;
    int next_id;
} EdRead;

enum { ED_INPUT = 1, ED_OUTPUT = 2, ED_INOUT = 3 };

void *ed_alloc(EdRead *r, size_t size);
char *ed_string(EdRead *r, const char *text);
void ed_error(EdRead *r, EdNode *node, const char *format, ...);
int ed_is(EdNode *node, const char *tag);
EdNode *ed_arg(EdNode *node, int index);
EdNode *ed_find(EdRead *r, EdNode *node, const char *tag);
const char *ed_atom(EdRead *r, EdNode *node);
const char *ed_name(EdRead *r, EdNode *node, int display);
EdWire *ed_root(EdWire *wire);
EdPort *ed_port(EdRead *r, EdView *view, const char *name, EdNode *where);
void ed_lower(EdRead *r, FitList *params);

#endif
