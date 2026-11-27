/*
 * dev.h - public interface of the device modules of the ATF150x fitter:
 * the device and package descriptions (dev.c, with the tables in
 * dev_tables.c), the fuse map and JEDEC writer (jed.c) and the fit report
 * (report.c).
 *
 * One program serves the three devices.  Everything that differs between
 * the ATF1502, ATF1504 and ATF1508 is reached through the device
 * description selected with dev_select() / dev_select_by_name(), which
 * main() has to call before anything else of the fitter runs.
 *
 * Lists of pin numbers hold the numbers themselves in the item pointers:
 * item i is (void *)(intptr_t)pin.
 */
#ifndef FIT150X_DEV_H
#define FIT150X_DEV_H

#include <stdint.h>
#include <stdio.h>
#include "fit_types.h"

/* ------------------------------------------------------------------ */
/* Device description                                                  */
/* ------------------------------------------------------------------ */

/* One row of a package table: rows 0..n_macrocells-1 are the macrocells,
 * the last n_ded_inputs rows the dedicated input pins (block 0,
 * macrocell 0). */
typedef struct FitPkgRow {
    int block;       /* 1..n_blocks, 0 for a dedicated input */
    int macrocell;   /* 1..n_macrocells, 0 for a dedicated input */
    int pin;         /* package pin, 0 when the macrocell has no pin */
} FitPkgRow;

/* Where dev_print_pinout writes a pin's signal name into the package
 * drawing (indexed by pin number - 1). */
typedef struct FitPkgLabel {
    int side;        /* 0 right of the package, 1 left, 2 below (written downwards), 3 above (ending at the package) */
    int line;        /* row (sides 0, 1) or column (sides 2, 3) of the drawing */
} FitPkgLabel;

/* The geometry of a package drawing that dev_print_pinout needs to place
 * the names. */
typedef struct FitPkgFrame {
    int left_col;    /* first column of the names on the left side */
    int left_end;    /* column after the last one on the left side */
    int right_col;   /* first column of the names on the right side */
    int right_width; /* width of the names on the right side */
    int top_row;     /* first row of the names above the package */
    int top_end;     /* last row of the names above the package */
    int bottom_row;  /* first row of the names below the package */
    int bottom_height; /* rows available below the package */
} FitPkgFrame;

/* Ball locations in JEDEC vector order.  Numeric packages use NULL. */
typedef struct FitBallPin {
    const char *name;
    const char *function;
} FitBallPin;

typedef struct FitPackageDesc {
    enum FitPackage id;
    const char *name;           /* "PLCC84": value of g_dev_package */
    const char *dev_name;       /* "P1508C84": -dev value */
    int n_pins;                 /* number of package pins */
    int io_limit;               /* pins a design may use (dev_check_pin_count), before JTAG / power-down
                                   pins are taken off */
    const FitPkgRow *rows;      /* n_macrocells + n_ded_inputs rows */
    int gclk_pins[3];           /* global clock pins GCLK1..3 (third is also OE1, second GCLR on some) */
    int gclr_pin;               /* global clear pin */
    const int *oe_pins;         /* pins that can drive a global OE; the first two are OE1 and OE2 */
    int n_oe_pins;              /* their number: 26 for the PLCC68 of the 1508, 30 for its other packages, 21 (1504), 14 (1502) */
    int jtag_pins[4];           /* TDI, TMS, TCK, TDO */
    int pd_pins[2];             /* PD1, PD2 */
    const char *vector_map;     /* one 'N'/'X' per pin for test vectors, NULL when jed_append_vectors has none */
    const char *const *drawing; /* text lines of the package drawing, n_drawing_lines of them */
    const unsigned char *drawing_row; /* row of the drawing buffer (0xa0-byte rows) each line is copied to */
    int n_drawing_lines;
    int drawing_cut;            /* column at which the drawing routine ends every row (0x55 ...) */
    int drawing_cut_rows;       /* number of rows it does that to */
    int drawing_stride;         /* bytes per row of the drawing buffer: 0xa0, 100 on the ATF1502 */
    FitPkgFrame frame;
    const FitPkgLabel *labels;  /* n_pins entries */
    const FitBallPin *balls;    /* n_pins entries, in occupied-ball row order */
} FitPackageDesc;

/* Comment line jed_write_cfg_note writes into the macrocell configuration
 * section when the fuse counter has reached `addr`. */
typedef struct FitJedecNote {
    int addr;
    const char *text;           /* "  NOTE S16,S12 of block A * \n" */
} FitJedecNote;

/* Test vector pin map for a package name (jed_append_vectors). */
typedef struct FitVectorMap {
    const char *package;
    const char *map;
} FitVectorMap;

/* Everything the JEDEC writer needs to know about a device. */
typedef struct FitJedecLayout {
    const char *part;           /* "ATF1508AS": "JEDEC file for: %s" */
    int n_fuses;                /* QF: 74136 / 34192 / 16808 */
    int mc_cfg_fuses;           /* length of the macrocell configuration area (0xf00) */
    int mc_cfg_addr;            /* its first fuse (0xf000) */
    int global_oe_addr;
    int cfg_bits_addr;
    int jtag_bits_addr;
    int ues_bits_addr;
    int reserved_addr;          /* first of the reserved fuses written as 0 up to QF */
    const int *pt_addr;         /* first fuse of each macrocell's product terms; index mc-1 */
    int cfg_half_addr[2];       /* first fuse of the two halves of the macrocell configuration area */
    const int *uim_addr;        /* fuse address of the UIM section of each block pair */
    const FitJedecNote *notes;  /* texts of jed_write_cfg_note, sorted by address */
    int n_notes;
} FitJedecLayout;

/* BE parts share the array/routing wiring but use packed 27-bit cell
 * records and different global controls. Unsupported densities return
 * NULL/-1. Macrocell numbers here are zero-based. */
const FitJedecLayout *fit_be_jedec_layout(enum FitDevice id);
int fit_be_macrocell_base(enum FitDevice id, int mc);
/* Zero-based macrocell whose pad supplies VREF for I/O bank A=0 or B=1.
 * Returns -1 when the device/bank has no SSTL reference input. */
int fit_be_vref_macrocell(enum FitDevice id, int bank);
extern const signed char fit_be_cfg_offsets[24];

typedef struct FitDeviceDesc {
    enum FitDevice id;
    FitDeviceParams params;                 /* initial values of the parameter block */
    const FitJedecLayout *jedec;
    const FitPackageDesc *packages;         /* in the order the package names are compared */
    int n_packages;
    const FitPackageDesc *default_package;  /* used with a warning when -dev names none (dev_select_package) */
    int n_oe_pins;                          /* largest FitPackageDesc.n_oe_pins of the device: 30 / 21 / 14 */
    const char *const *uim_sources;         /* [FIT_UIM_PER_BLOCK][uim_width] node names selectable by each block input multiplexer */
    const char *const *oe_sources;          /* [FIT_GLOBAL_SLOTS][uim_width] node names selectable by each global OE multiplexer */
    const FitVectorMap *vector_maps;        /* maps for package names of the other devices */
    int n_vector_maps;
    int uim_cluster_limit;                  /* 20 / 10 / 10, read by alloc.c */
} FitDeviceDesc;

/* dev_tables.c: the three descriptions, indexed by enum FitDevice. */
extern const FitDeviceDesc g_dev_descs[3];

/* The selected device.  g_dev points at the live copy of the parameter
 * block: the last three words are changed by the fuse mapper
 * (jed_twoclock). */
extern const FitDeviceDesc *g_dev_desc;
extern const FitDeviceParams *g_dev;

/* Selects the device; resets the parameter block and the package.  Must be
 * called once before the fitter starts. */
void dev_select(enum FitDevice device);

/* Selects the device from a program name ("fit1504", ".../FIT1508.EXE") or a
 * -dev value ("P1502C44"): looks for "1502", "1504" or "1508" in the last
 * path component.  Returns 1 on success, 0 when the name contains none of
 * them (nothing is selected then). */
int dev_select_by_name(const char *name);

/* Description of the package g_dev_package names, NULL when it names none
 * of this device's packages (or is NULL). */
const FitPackageDesc *dev_current_package(void);

/* Parse a numeric pin or a ball name for the package selected in params.
 * The end pointer permits callers to check comments and trailing text. */
int dev_parse_pin(FitList *params, const char *text, char **end);

/* Pin label in the current package; numeric labels use the caller's buffer. */
const char *dev_pin_name(int pin, char *buffer, size_t size);

/* Package name ("PLCC84" ...); set from -dev by main_device_package,
 * replaced by the default in dev_select_package.  Owned string or NULL. */
extern char *g_dev_package;

/* ------------------------------------------------------------------ */
/* dev.c: packages and pins                                            */
/* ------------------------------------------------------------------ */

/* Counts the pins the entity needs (inputs + outputs - aliases)
 * against the package.  Too many: "# ERROR : Design has %d IOs." to the
 * report and the message stream, returns 0.  More than 95 %: the "Info: %s
 * uses 95% of the pins available in device %s" paragraph in the report.
 * Returns 1 otherwise.  Called from fit_drv_check_limits. */
int dev_check_pin_count(FitEntity *ent);

/* Pin of row `index` (0-based: macrocell number - 1, then the
 * dedicated inputs) of package `package`; 0 = no pin.  For a name that is
 * not a package of the device the result is the case-insensitive
 * comparison of the name with the device's last package name (nonzero),
 * the value the package lookup ends with.  Used by place_create_slots and
 * alloc_block_io_pins. */
int dev_pin_of_macrocell(const char *package, int index);

/* Pin by function code: kind 8, 9, 10 -> global clock pin
 * [index], 4 -> global clear pin, 0xb (1502 also 0xc) -> global OE capable
 * pin [index].  For any other kind, or without a package, the result is
 * the comparison of the package name with the last name of the package
 * lookup (0 when the current package is that one, nonzero otherwise).
 * Used by place_dedicated_pin. */
int dev_special_pin(int kind, int index);

/* New list of the three global clock pins. */
FitList *dev_gclk_pin_list(void);

/* New list holding the global clear pin. */
FitList *dev_gclr_pin_list(void);

/* New list of the pins that can drive a global output enable. */
FitList *dev_oe_pin_list(void);

/* New list of the JTAG pins TDI, TMS, TCK, TDO. */
FitList *dev_jtag_pin_list(void);

/* New list of the power-down pins PD1, PD2. */
FitList *dev_pd_pin_list(void);

/* One entry of the list dev_select_package returns. */
typedef struct FitPkgSlot {
    int block;
    int macrocell;
    int pin;
    int i0c;          /* unknown; set to 0, never read */
    int no_pin;       /* 1 when pin == 0; also set to 1 for the JTAG pins when JTAG is on */
    int last_of_8;    /* 1 for every eighth row (index 7, 15, ...) */
    int first_of_8;   /* 1 for rows 0, 8, 16, ... */
} FitPkgSlot;

/* Builds the list of FitPkgSlot for the package in
 * g_dev_package; without a package or with an unknown one prints a warning
 * on the message stream and falls back to the device's default.  Upper-cases
 * g_dev_package. */
FitList *dev_select_package(void);

/* New copy of "1508" / "1504" / "1502".  Used by the banner
 * (main_print_banner). */
char *dev_family_string(void);

/* ------------------------------------------------------------------ */
/* dev.c: UIM connectivity                                             */
/* ------------------------------------------------------------------ */

/* The six global output-enable slots (list of FitUimSlot, created by
 * place_create_global_slots); defined in jed.c. */
extern FitList *g_jed_global_slots;

/* 1 when input k (0..uim_width-1) of the block input multiplexer
 * u->index can select the node called `node`.  Used by
 * place_uim_free_candidates and alloc_uim_candidates. */
int dev_uim_selects(const FitUimSlot *u, const char *node, int k);

/* Input number of multiplexer u->index that selects u->sel_node, -1
 * when none does. */
int dev_uim_input_of(const FitUimSlot *u);

/* The same for a global output-enable multiplexer. */
int dev_oe_input_of(const FitUimSlot *u);

/* Gives the signal of `slot` a free global OE multiplexer that can
 * select its node (dev_oe_node_name names the node: "OE1", "OE2" or
 * <block letter><macrocell>"p"); fills the global slot's signal, node and
 * index.  Returns 1 on success, 0 when there is none.  Called from
 * fit_drv_take_global_pin. */
int dev_assign_global_oe(FitSlot *slot);

/* Looks for a free global OE multiplexer that can select the node
 * of `slot` (block letter from place_block_letter).  With assign != 0 the
 * multiplexer is taken for slot->signal (which == 1) or slot->buried
 * (otherwise).  Returns 1 when one was found.  Used by the fit driver and
 * place_find_slots. */
int dev_find_global_oe(FitSlot *slot, int assign, int which);

/* New list of lists of node names (copies), each name once per list: on
 * the ATF1508 the nodes selectable by the even-numbered block input
 * multiplexers and those of the odd-numbered ones; four groups of
 * multiplexers on the ATF1504 and ATF1502 (see dev.c).  Used by
 * alloc_rebalance. */
FitList *dev_uim_node_lists(void);

/* ------------------------------------------------------------------ */
/* jed.c: fuse map and JEDEC file                                      */
/* ------------------------------------------------------------------ */

/* Variable id of an ID node below any number of NOT nodes, 0 when
 * the tree is something else. */
int jed_tree_var_id(const FitFctTree *tree);

/* 1 when the first two children of the node have the same
 * selector.  Used by tt3_print_xor. */
int jed_children_same_sel(const FitFctTree *tree);

/* Translates the placed and routed design into the fuse image
 * (g_jed_map): ent is the fitted entity, slots the placement list
 * g_fit_slots (FitSlot), uim the routing result g_fit_uim_slots (list of
 * lists of FitUimSlot).  Returns 1, or 0 after an error message in the
 * report ("Fuse mapping fail" is the caller's).  Called from fit_driver. */
int jed_build_fuse_map(FitEntity *ent, FitList *slots, FitList *uim);

/* Writes <out_base>.jed from the fuse image, appending test
 * vectors from <out_base>.tmv when that file exists.  Returns 1.  Called
 * from fit_driver. */
int jed_write_file(const char *out_base);

/* The fuse image built by jed_build_fuse_map. */
extern FitJedecMap *g_jed_map;

/* ------------------------------------------------------------------ */
/* report.c: the .fit report                                           */
/* ------------------------------------------------------------------ */

/* The <out>.fit report; opened by report_open, closed by
 * report_write_result. */
extern FILE *g_fit_report;

/* 1 (initial value) while the design fits; the driver clears it
 * before report_write_result when it does not.  Selects "Design FITS" /
 * "$Device %s fits" in the report. */
extern int g_report_fits;

/* Creates <out_base>.fit (g_fit_report) and writes its first lines:
 * version, ctime of now, the command string (g_command).  Called from the
 * job main_fit_job. */
void report_open(const char *out_base);

/* "****** Initial fitting strategy and property ******" section.
 * Called from fit_driver. */
void report_write_strategy(const char *out_base, FILE *f);

/* Names the slots sitting on the JTAG pins of `pins` (list from
 * dev_jtag_pin_list) "TDI", "TMS", "TCK", "TDO" and marks them used.
 * Called from place_create_slots. */
void report_reserve_jtag_slots(FitList *slots, FitList *pins);

/* The same for the power-down pins "PD1", "PD2", when pd1 / pd2
 * are on.  Called from place_create_slots. */
void report_reserve_pd_slots(FitList *slots, FitList *pins);

/* Writes n blanks.  Used by the .io writer fit_drv_write_io_file. */
void report_put_blanks(FILE *f, int n);

/* The body of the report (to g_fit_report) after the passes:
 * pin assignments, placement list, package drawing, UIM fan-in, resource
 * usage, "$Device %s fits"; closes the report.  out_base is the name printed
 * in "Creating JEDEC file %s.jed"; slots is g_fit_slots, uim
 * g_fit_uim_slots.  Called from fit_driver. */
void report_write_result(FitEntity *ent, const char *out_base, FitList *slots, FitList *uim);

#endif /* FIT150X_DEV_H */
