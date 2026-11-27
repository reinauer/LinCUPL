/*
 * dev_pinout.c - the package drawing of the extended mode report: a rectangle with
 * the pin numbers, the signal names left and right of it in the rows and
 * written vertically above and below it.  Every package has its fixed
 * picture; the names come from the slots (FitSlot) at the pins, the
 * supply pins have fixed names.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base.h"
#include "dev.h"
#include "dev_priv.h"

/* supply and unconnected pins; the VCCIO and VCCINT pins of the TQFP
 * packages are named so for BE devices only */
enum {
    PO_GND = -1,
    PO_VCC = -2,
    PO_NC = -3,
    PO_VCCIOA = -4,
    PO_VCCIOB = -5,
    PO_VCCINT = -6
};

/* one row: the pins left and right of it and the text between their
 * names */
typedef struct PinoutRow {
    short left, right;
    const char *text;
} PinoutRow;

/* the drawing of a package: lines above the rows (head) and below them
 * (foot), each list ends with NULL; the pins over and under the package
 * from left to right, ending with 0.  "ATF15xx" in a row is the device. */
typedef struct PinoutExt {
    const char *family, *package;
    const char *const *head;
    const PinoutRow *rows;
    const char *const *foot;
    const short *top, *bottom;
} PinoutExt;

static const char *const po_plcc68_head[] = {
    "+------------------------------------------------------+",
    "|   9  8  7  6  5  4  3  2  1 68 67 66 65 64 63 62 61  |",
    NULL
};
static const char *const po_plcc68_foot[] = {
    "|  27 28 29 30 31 32 33 34 35 36 37 38 39 40 41 42 43  |",
    "+------------------------------------------------------+",
    NULL
};
static const PinoutRow po_plcc68_rows[] = {
    { 10, 60, "| 10                                                60 |" },
    { PO_VCC, 59, "| 11                                                59 |" },
    { 12, PO_GND, "| 12                                                58 |" },
    { 13, 57, "| 13                                                57 |" },
    { 14, 56, "| 14                                                56 |" },
    { 15, 55, "| 15                                                55 |" },
    { PO_GND, 54, "| 16                  ATF15xx                       54 |" },
    { 17, PO_VCC, "| 17                68-Lead PLCC                    53 |" },
    { 18, 52, "| 18                                                52 |" },
    { 19, 51, "| 19                                                51 |" },
    { 20, 50, "| 20                                                50 |" },
    { PO_VCC, 49, "| 21                                                49 |" },
    { 22, PO_GND, "| 22                                                48 |" },
    { 23, 47, "| 23                                                47 |" },
    { 24, 46, "| 24                                                46 |" },
    { 25, 45, "| 25                                                45 |" },
    { PO_GND, 44, "| 26                                               44  |" },
    { 0, 0, NULL }
};
static const short po_plcc68_top[] = { 9, 8, 7, PO_GND, 5, 4, PO_VCC, 2, 1, 68, 67, PO_GND, 65, 64, PO_VCC, 62, 61, 0 };
static const short po_plcc68_bottom[] = { 27, 28, 29, 30, PO_VCC, 32, 33, PO_GND, PO_VCC, 36, 37, PO_GND, 39, 40, 41, 42, PO_VCC, 0 };

static const char *const po_plcc84_head[] = {
    "+------------------------------------------------------------------+",
    "|  11 10  9  8  7  6  5  4  3  2  1 84 83 82 81 80 79 78 77 76 75  |",
    NULL
};
static const char *const po_plcc84_foot[] = {
    "|  33 34 35 36 37 38 39 40 41 42 43 44 45 46 47 48 49 50 51 52 53  |",
    "+------------------------------------------------------------------+",
    NULL
};
static const PinoutRow po_plcc84_rows[] = {
    { 12, 74, "| 12                                                            74 |" },
    { PO_VCC, 73, "| 13                                                            73 |" },
    { 14, PO_GND, "| 14                                                            72 |" },
    { 15, 71, "| 15                                                            71 |" },
    { 16, 70, "| 16                                                            70 |" },
    { 17, 69, "| 17                                                            69 |" },
    { 18, 68, "| 18                                                            68 |" },
    { PO_GND, 67, "| 19                                                            67 |" },
    { 20, PO_VCC, "| 20                                                            66 |" },
    { 21, 65, "| 21                                                            65 |" },
    { 22, 64, "| 22                           ATF15xx                          64 |" },
    { 23, 63, "| 23                         84-Lead PLCC                       63 |" },
    { 24, 62, "| 24                                                            62 |" },
    { 25, 61, "| 25                                                            61 |" },
    { PO_VCC, 60, "| 26                                                            60 |" },
    { 27, PO_GND, "| 27                                                            59 |" },
    { 28, 58, "| 28                                                            58 |" },
    { 29, 57, "| 29                                                            57 |" },
    { 30, 56, "| 30                                                            56 |" },
    { 31, 55, "| 31                                                            55 |" },
    { PO_GND, 54, "| 32                                                            54 |" },
    { 0, 0, NULL }
};
static const short po_plcc84_top[] = { 11, 10, 9, 8, PO_GND, 6, 5, 4, PO_VCC, 2, 1, 84, 83, PO_GND, 81, 80, 79, PO_VCC, 77, 76, 75, 0 };
static const short po_plcc84_bottom[] = { 33, 34, 35, 36, 37, PO_VCC, 39, 40, 41, PO_GND, PO_VCC, 44, 45, 46, PO_GND, 48, 49, 50, 51, 52, PO_VCC, 0 };

static const char *const po_pqfp100_head[] = {
    "+---------------------------------------------------------------+",
    "| 100 99 98 97 96 95 94 93 92 91 90 89 88 87 86 85 84 83 82 81  |",
    NULL
};
static const char *const po_pqfp100_foot[] = {
    "|  31 32 33 34 35 36 37 38 39 40 41 42 43 44 45 46 47 48 49 50  |",
    "+---------------------------------------------------------------+",
    NULL
};
static const PinoutRow po_pqfp100_rows[] = {
    { 1, 80, "| 1                                                          80 |" },
    { 2, 79, "| 2                                                          79 |" },
    { 3, 78, "| 3                                                          78 |" },
    { 4, 77, "| 4                                                          77 |" },
    { PO_VCC, PO_GND, "| 5                                                          76 |" },
    { 6, 75, "| 6                                                          75 |" },
    { 7, 74, "| 7                                                          74 |" },
    { 8, 73, "| 8                                                          73 |" },
    { 9, 72, "| 9                                                          72 |" },
    { 10, 71, "| 10                                                         71 |" },
    { 11, 70, "| 11                                                         70 |" },
    { 12, 69, "| 12                                                         69 |" },
    { PO_GND, PO_VCC, "| 13                                                         68 |" },
    { 14, 67, "| 14                                                         67 |" },
    { 15, 66, "| 15                         ATF15xx                         66 |" },
    { 16, 65, "| 16                       100-Lead PQFP                     65 |" },
    { 17, 64, "| 17                                                         64 |" },
    { 18, 63, "| 18                                                         63 |" },
    { 19, 62, "| 19                                                         62 |" },
    { PO_VCC, PO_GND, "| 20                                                         61 |" },
    { 21, 60, "| 21                                                         60 |" },
    { 22, 59, "| 22                                                         59 |" },
    { 23, 58, "| 23                                                         58 |" },
    { 24, 57, "| 24                                                         57 |" },
    { 25, 56, "| 25                                                         56 |" },
    { 26, 55, "| 26                                                         55 |" },
    { 27, 54, "| 27                                                         54 |" },
    { PO_GND, PO_VCC, "| 28                                                         53 |" },
    { 29, 52, "| 29                                                         52 |" },
    { 30, 51, "| 30                                                         51 |" },
    { 0, 0, NULL }
};
static const short po_pqfp100_top[] = { 100, 99, 98, PO_GND, 96, 95, 94, PO_VCC, 92, 91, 90, 89, PO_GND, 87, 86, 85, PO_VCC, 83, 82, 81, 0 };
static const short po_pqfp100_bottom[] = { 31, 32, 33, 34, 35, PO_VCC, 37, 38, 39, PO_GND, PO_VCC, 42, 43, 44, PO_GND, 46, 47, 48, 49, 50, 0 };

static const char *const po_tqfp100_head[] = {
    "+------------------------------------------------------------------------------+",
    "| 100 99 98 97 96 95 94 93 92 91 90 89 88 87 86 85 84 83 82 81 80 79 78 77 76  |",
    NULL
};
static const char *const po_tqfp100_foot[] = {
    "|  26 27 28 29 30 31 32 33 34 35 36 37 38 39 40 41 42 43 44 45 46 47 48 49 50  |",
    "+------------------------------------------------------------------------------+",
    NULL
};
static const PinoutRow po_tqfp100_rows[] = {
    { 1, 75, "| 1                                                                         75 |" },
    { 2, PO_GND, "| 2                                                                         74 |" },
    { PO_VCCIOA, 73, "| 3                                                                         73 |" },
    { 4, 72, "| 4                                                                         72 |" },
    { 5, 71, "| 5                                                                         71 |" },
    { 6, 70, "| 6                                                                         70 |" },
    { 7, 69, "| 7                                                                         69 |" },
    { 8, 68, "| 8                                                                         68 |" },
    { 9, 67, "| 9                                                                         67 |" },
    { 10, PO_VCCIOB, "| 10                                                                        66 |" },
    { PO_GND, 65, "| 11                                                                        65 |" },
    { 12, 64, "| 12                                ATF15xx                                 64 |" },
    { 13, 63, "| 13                             100-Lead TQFP                              63 |" },
    { 14, 62, "| 14                                                                        62 |" },
    { 15, 61, "| 15                                                                        61 |" },
    { 16, 60, "| 16                                                                        60 |" },
    { 17, PO_GND, "| 17                                                                        59 |" },
    { PO_VCCIOA, 58, "| 18                                                                        58 |" },
    { 19, 57, "| 19                                                                        57 |" },
    { 20, 56, "| 20                                                                        56 |" },
    { 21, 55, "| 21                                                                        55 |" },
    { 22, 54, "| 22                                                                        54 |" },
    { 23, 53, "| 23                                                                        53 |" },
    { 24, 52, "| 24                                                                        52 |" },
    { 25, PO_VCCIOB, "| 25                                                                        51 |" },
    { 0, 0, NULL }
};
static const short po_tqfp100_top[] = { 100, 99, 98, 97, 96, PO_GND, 94, 93, 92, PO_VCCINT, 90, 89, 88, 87, PO_GND, 85, 84, 83, PO_VCCIOB, 81, 80, 79, 78, 77, 76, 0 };
static const short po_tqfp100_bottom[] = { PO_GND, 27, 28, 29, 30, 31, 32, 33, PO_VCCIOA, 35, 36, 37, PO_GND, PO_VCCINT, 40, 41, 42, PO_GND, 44, 45, 46, 47, 48, 49, 50, 0 };

static const char *const po_pqfp160_head[] = {
    "+----------------------------------------------------------------------------------+",
    "|160 158 156 154 152 150 148 146 144 142 140 138 136 134 132 130 128 126 124 122   |",
    "|  159 157 155 153 151 149 147 145 143 141 139 137 135 133 131 129 127 125 123 121 |",
    NULL
};
static const char *const po_pqfp160_foot[] = {
    "|   42  44  46  48  50  52  54  56  58  60  62  64  66  68  70  72  74  76  78  80 |",
    "| 41  43  45  47  49  51  53  55  57  59  61  63  65  67  69  71  73  75  77  79   |",
    "+----------------------------------------------------------------------------------+",
    NULL
};
static const PinoutRow po_pqfp160_rows[] = {
    { PO_NC, PO_NC, "|1                                                                              120|" },
    { PO_NC, PO_NC, "|2                                                                              119|" },
    { PO_NC, PO_NC, "|3                                                                              118|" },
    { PO_NC, PO_NC, "|4                                                                              117|" },
    { PO_NC, PO_NC, "|5                                                                              116|" },
    { PO_NC, PO_NC, "|6                                                                              115|" },
    { PO_NC, PO_NC, "|7                                                                              114|" },
    { PO_VCC, PO_GND, "|8                                                                              113|" },
    { 9, 112, "|9                                                                              112|" },
    { 10, 111, "|10                                                                             111|" },
    { 11, 110, "|11                                                                             110|" },
    { 12, 109, "|12                                                                             109|" },
    { 13, 108, "|13                                                                             108|" },
    { 14, 107, "|14                                                                             107|" },
    { 15, 106, "|15                                                                             106|" },
    { 16, 105, "|16                                                                             105|" },
    { PO_GND, PO_VCC, "|17                                                                             104|" },
    { 18, 103, "|18                                                                             103|" },
    { 19, 102, "|19                                                                             102|" },
    { 20, 101, "|20                                  ATF15xx                                    101|" },
    { 21, 100, "|21                                160-Lead PLCC                                100|" },
    { 22, 99, "|22                                                                              99|" },
    { 23, 98, "|23                                                                              98|" },
    { 24, 97, "|24                                                                              97|" },
    { 25, 96, "|25                                                                              96|" },
    { PO_VCC, PO_GND, "|26                                                                              95|" },
    { 27, 94, "|27                                                                              94|" },
    { 28, 93, "|28                                                                              93|" },
    { 29, 92, "|29                                                                              92|" },
    { 30, 91, "|30                                                                              91|" },
    { 31, 90, "|31                                                                              90|" },
    { 32, 89, "|32                                                                              89|" },
    { 33, 88, "|33                                                                              88|" },
    { PO_NC, PO_NC, "|34                                                                              87|" },
    { PO_NC, PO_NC, "|35                                                                              86|" },
    { PO_NC, PO_NC, "|36                                                                              85|" },
    { PO_NC, PO_NC, "|37                                                                              84|" },
    { PO_NC, PO_NC, "|38                                                                              83|" },
    { PO_NC, PO_NC, "|39                                                                              82|" },
    { PO_NC, PO_NC, "|40                                                                              81|" },
    { 0, 0, NULL }
};
static const short po_pqfp160_top[] = { 160, 159, 158, PO_NC, PO_NC, PO_NC, PO_NC, 153, 152, 151, 150, 149, PO_GND, 147, 146, 145, 144, PO_VCC, 142, 141, 140, 139, PO_GND, 137, 136, 135, 134, PO_VCC, 132, 131, 130, 129, 128, PO_NC, PO_NC, PO_NC, PO_NC, 123, 122, 121, 0 };
static const short po_pqfp160_bottom[] = { 41, PO_GND, 43, PO_NC, PO_NC, PO_NC, PO_NC, 48, 49, 50, 51, 52, 53, 54, PO_VCC, 56, 57, 58, 59, PO_GND, PO_VCC, 62, 63, 64, 65, PO_GND, 67, 68, 69, 70, 71, 72, 73, PO_NC, PO_NC, PO_NC, PO_NC, 78, PO_VCC, 80, 0 };

static const char *const po_plcc44_head[] = {
    "+------------------------------------+",
    "|   6  5  4  3  2  1 44 43 42 41 40  |",
    NULL
};
static const char *const po_plcc44_foot[] = {
    "|  18 19 20 21 22 23 24 25 26 27 28  |",
    "+------------------------------------+",
    NULL
};
static const PinoutRow po_plcc44_rows[] = {
    { 7, 39, "| 7                               39 |" },
    { 8, 38, "| 8                               38 |" },
    { 9, 37, "| 9                               37 |" },
    { PO_GND, 36, "| 10                              36 |" },
    { 11, PO_VCC, "| 11            ATF15xx           35 |" },
    { 12, 34, "| 12         44-Lead PLCC         34 |" },
    { 13, 33, "| 13                              33 |" },
    { 14, 32, "| 14                              32 |" },
    { PO_VCC, 31, "| 15                              31 |" },
    { 16, PO_GND, "| 16                              30 |" },
    { 17, 29, "| 17                              29 |" },
    { 0, 0, NULL }
};
static const short po_plcc44_top[] = { 6, 5, 4, PO_VCC, 2, 1, 44, 43, PO_GND, 41, 40, 0 };
static const short po_plcc44_bottom[] = { 18, 19, 20, 21, PO_GND, PO_VCC, 24, 25, 26, 27, 28, 0 };

static const char *const po_tqfp44_head[] = {
    "+------------------------------------+",
    "|  44 43 42 41 40 39 38 37 36 35 34  |",
    NULL
};
static const char *const po_tqfp44_foot[] = {
    "|  12 13 14 15 16 17 18 19 20 21 22  |",
    "+------------------------------------+",
    NULL
};
static const PinoutRow po_tqfp44_rows[] = {
    { 1, 33, "|  1                              33 |" },
    { 2, 32, "|  2                              32 |" },
    { 3, 31, "|  3                              31 |" },
    { PO_GND, 30, "|  4                              30 |" },
    { 5, PO_VCCIOB, "|  5            ATF15xx           29 |" },
    { 6, 28, "|  6          44-Lead TQFP        28 |" },
    { 7, 27, "|  7                              27 |" },
    { 8, 26, "|  8                              26 |" },
    { PO_VCCIOA, 25, "|  9                              25 |" },
    { 10, PO_GND, "| 10                              24 |" },
    { 11, 23, "| 11                              23 |" },
    { 0, 0, NULL }
};
static const short po_tqfp44_top[] = { 44, 43, 42, PO_VCCINT, 40, 39, 38, 37, PO_GND, 35, 34, 0 };
static const short po_tqfp44_bottom[] = { 12, 13, 14, 15, PO_GND, PO_VCCINT, 18, 19, 20, 21, 22, 0 };

static const char *const po_plcc68_1504_head[] = {
    "+------------------------------------------------------+",
    "|   9  8  7  6  5  4  3  2  1 68 67 66 65 64 63 62 61  |",
    NULL
};
static const char *const po_plcc68_1504_foot[] = {
    "|  27 28 29 30 31 32 33 34 35 36 37 38 39 40 41 42 43  |",
    "+------------------------------------------------------+",
    NULL
};
static const PinoutRow po_plcc68_1504_rows[] = {
    { 10, 60, "| 10                                                60 |" },
    { PO_VCC, 59, "| 11                                                59 |" },
    { 12, PO_GND, "| 12                                                58 |" },
    { 13, 57, "| 13                                                57 |" },
    { 14, 56, "| 14                                                56 |" },
    { 15, 55, "| 15                                                55 |" },
    { PO_GND, 54, "| 16                  ATF15xx                       54 |" },
    { 17, PO_VCC, "| 17                68-Lead PLCC                    53 |" },
    { 18, 52, "| 18                                                52 |" },
    { 19, 51, "| 19                                                51 |" },
    { 20, 50, "| 20                                                50 |" },
    { PO_VCC, 49, "| 21                                                49 |" },
    { 22, PO_GND, "| 22                                                48 |" },
    { 23, 47, "| 23                                                47 |" },
    { 24, 46, "| 24                                                46 |" },
    { 25, 45, "| 25                                                45 |" },
    { PO_GND, 44, "| 26                                                44 |" },
    { 0, 0, NULL }
};
static const short po_plcc68_1504_top[] = { 9, 8, 7, PO_GND, 5, 4, PO_VCC, 2, 1, 68, 67, PO_GND, 65, 64, PO_VCC, 62, 61, 0 };
static const short po_plcc68_1504_bottom[] = { 27, 28, 29, 30, PO_VCC, 32, 33, PO_GND, PO_VCC, 36, 37, PO_GND, 39, 40, 41, 42, PO_VCC, 0 };

static const char *const po_pqfp100_1504_head[] = {
    "+---------------------------------------------------------------+",
    "| 100 99 98 97 96 95 94 93 92 91 90 89 88 87 86 85 84 83 82 81  |",
    NULL
};
static const char *const po_pqfp100_1504_foot[] = {
    "|  31 32 33 34 35 36 37 38 39 40 41 42 43 44 45 46 47 48 49 50  |",
    "+---------------------------------------------------------------+",
    NULL
};
static const PinoutRow po_pqfp100_1504_rows[] = {
    { PO_NC, PO_NC, "| 1                                                          80 |" },
    { PO_NC, PO_NC, "| 2                                                          79 |" },
    { 3, 78, "| 3                                                          78 |" },
    { 4, 77, "| 4                                                          77 |" },
    { PO_VCC, PO_GND, "| 5                                                          76 |" },
    { 6, 75, "| 6                                                          75 |" },
    { PO_NC, PO_NC, "| 7                                                          74 |" },
    { 8, 73, "| 8                                                          73 |" },
    { PO_NC, PO_NC, "| 9                                                          72 |" },
    { 10, 71, "| 10                                                         71 |" },
    { 11, 70, "| 11                                                         70 |" },
    { 12, 69, "| 12                                                         69 |" },
    { PO_GND, PO_VCC, "| 13                                                         68 |" },
    { 14, 67, "| 14                                                         67 |" },
    { 15, 66, "| 15                         ATF15xx                         66 |" },
    { 16, 65, "| 16                       100-Lead PQFP                     65 |" },
    { 17, 64, "| 17                                                         64 |" },
    { 18, 63, "| 18                                                         63 |" },
    { 19, 62, "| 19                                                         62 |" },
    { PO_VCC, PO_GND, "| 20                                                         61 |" },
    { 21, 60, "| 21                                                         60 |" },
    { 22, 59, "| 22                                                         59 |" },
    { 23, 58, "| 23                                                         58 |" },
    { PO_NC, PO_NC, "| 24                                                         57 |" },
    { 25, 56, "| 25                                                         56 |" },
    { PO_NC, PO_NC, "| 26                                                         55 |" },
    { 27, 54, "| 27                                                         54 |" },
    { PO_GND, PO_VCC, "| 28                                                         53 |" },
    { PO_NC, PO_NC, "| 29                                                         52 |" },
    { PO_NC, PO_NC, "| 30                                                         51 |" },
    { 0, 0, NULL }
};
static const short po_pqfp100_1504_top[] = { 100, 99, 98, PO_GND, 96, 95, 94, PO_VCC, 92, 91, 90, 89, PO_GND, 87, 86, 85, PO_VCC, 83, 82, 81, 0 };
static const short po_pqfp100_1504_bottom[] = { 31, 32, 33, 34, 35, PO_VCC, 37, 38, 39, PO_GND, PO_VCC, 42, 43, 44, PO_GND, 46, 47, 48, 49, 50, 0 };

static const char *const po_tqfp100_1504_head[] = {
    "+------------------------------------------------------------------------------+",
    "| 100 99 98 97 96 95 94 93 92 91 90 89 88 87 86 85 84 83 82 81 80 79 78 77 76  |",
    NULL
};
static const char *const po_tqfp100_1504_foot[] = {
    "|  26 27 28 29 30 31 32 33 34 35 36 37 38 39 40 41 42 43 44 45 46 47 48 49 50  |",
    "+------------------------------------------------------------------------------+",
    NULL
};
static const PinoutRow po_tqfp100_1504_rows[] = {
    { PO_NC, 75, "| 1                                                                         75 |" },
    { PO_NC, PO_GND, "| 2                                                                         74 |" },
    { PO_VCCIOA, 73, "| 3                                                                         73 |" },
    { 4, PO_NC, "| 4                                                                         72 |" },
    { PO_NC, 71, "| 5                                                                         71 |" },
    { 6, PO_NC, "| 6                                                                         70 |" },
    { PO_NC, 69, "| 7                                                                         69 |" },
    { 8, 68, "| 8                                                                         68 |" },
    { 9, 67, "| 9                                                                         67 |" },
    { 10, PO_VCCIOB, "| 10                                                                        66 |" },
    { PO_GND, 65, "| 11                                                                        65 |" },
    { 12, 64, "| 12                                ATF15xx                                 64 |" },
    { 13, 63, "| 13                             100-Lead TQFP                              63 |" },
    { 14, 62, "| 14                                                                        62 |" },
    { 15, 61, "| 15                                                                        61 |" },
    { 16, 60, "| 16                                                                        60 |" },
    { 17, PO_GND, "| 17                                                                        59 |" },
    { PO_VCCIOA, 58, "| 18                                                                        58 |" },
    { 19, 57, "| 19                                                                        57 |" },
    { 20, 56, "| 20                                                                        56 |" },
    { 21, PO_NC, "| 21                                                                        55 |" },
    { PO_NC, 54, "| 22                                                                        54 |" },
    { 23, PO_NC, "| 23                                                                        53 |" },
    { PO_NC, 52, "| 24                                                                        52 |" },
    { 25, PO_VCCIOB, "| 25                                                                        51 |" },
    { 0, 0, NULL }
};
static const short po_tqfp100_1504_top[] = { 100, 99, 98, 97, 96, PO_GND, 94, 93, 92, PO_VCCINT, 90, 89, 88, 87, PO_GND, 85, 84, 83, PO_VCCIOB, 81, 80, 79, PO_NC, PO_NC, 76, 0 };
static const short po_tqfp100_1504_bottom[] = { PO_GND, PO_NC, PO_NC, 29, 30, 31, 32, 33, PO_VCCIOA, 35, 36, 37, PO_GND, PO_VCCINT, 40, 41, 42, PO_GND, 44, 45, 46, 47, 48, PO_NC, PO_NC, 0 };

static const PinoutExt pinouts[] = {
    { "1508", "PLCC68", po_plcc68_head, po_plcc68_rows, po_plcc68_foot, po_plcc68_top, po_plcc68_bottom },
    { "1508", "PLCC84", po_plcc84_head, po_plcc84_rows, po_plcc84_foot, po_plcc84_top, po_plcc84_bottom },
    { "1508", "PQFP100", po_pqfp100_head, po_pqfp100_rows, po_pqfp100_foot, po_pqfp100_top, po_pqfp100_bottom },
    { "1508", "TQFP100", po_tqfp100_head, po_tqfp100_rows, po_tqfp100_foot, po_tqfp100_top, po_tqfp100_bottom },
    { "1508", "PQFP160", po_pqfp160_head, po_pqfp160_rows, po_pqfp160_foot, po_pqfp160_top, po_pqfp160_bottom },
    { "1504", "PLCC44", po_plcc44_head, po_plcc44_rows, po_plcc44_foot, po_plcc44_top, po_plcc44_bottom },
    { "1504", "TQFP44", po_tqfp44_head, po_tqfp44_rows, po_tqfp44_foot, po_tqfp44_top, po_tqfp44_bottom },
    { "1504", "PLCC68", po_plcc68_1504_head, po_plcc68_1504_rows, po_plcc68_1504_foot, po_plcc68_1504_top, po_plcc68_1504_bottom },
    { "1504", "PLCC84", po_plcc84_head, po_plcc84_rows, po_plcc84_foot, po_plcc84_top, po_plcc84_bottom },
    { "1504", "PQFP100", po_pqfp100_1504_head, po_pqfp100_1504_rows, po_pqfp100_1504_foot, po_pqfp100_1504_top, po_pqfp100_1504_bottom },
    { "1504", "TQFP100", po_tqfp100_1504_head, po_tqfp100_1504_rows, po_tqfp100_1504_foot, po_tqfp100_1504_top, po_tqfp100_1504_bottom },
    { "1502", "PLCC44", po_plcc44_head, po_plcc44_rows, po_plcc44_foot, po_plcc44_top, po_plcc44_bottom },
    { "1502", "TQFP44", po_tqfp44_head, po_tqfp44_rows, po_tqfp44_foot, po_tqfp44_top, po_tqfp44_bottom },
};

/* The name drawn at a pin: the signal of the slot at the pin; a free pin
 * is blank on the ATF1508 and "NC" on the others, or for BE devices names
 * what is done with unused pins.  A pin without a slot takes the name of
 * the last slot. */
static char *pinout_slot_name(FitList *slots, int pin)
{
    FitSlot *slot = NULL;
    int i;

    for (i = 0; i < (slots != NULL ? slots->count : 0); i++) {
        slot = slots->items[i];
        if (slot->pin == pin)
            break;
    }
    if (slot != NULL && strcmp(slot->signal, FIT_FREE_NAME) != 0)
        return util_strdup(slot->signal);
    if (g_opt.be == 0)
        return util_strdup(strcmp(g_family, "1508") == 0 ? "   " : "NC");
    if (g_opt.unused_to_ground != 0)
        return util_strdup("Unused_GND");
    if (g_opt.pull_up_unused != 0)
        return util_strdup("Unused_PU");
    if (g_opt.unused_to_pinkeeper != 0)
        return util_strdup("Unused_PK");
    return util_strdup("Unused_Z");
}

static char *pinout_name(FitList *slots, int code)
{
    static const char *const supply[6] = { "GND", "VCC", "NC", "VCCIOA", "VCCIOB", "VCCINT" };

    if (code > 0)
        return pinout_slot_name(slots, code);
    if (code <= PO_VCCIOA && g_opt.be == 0)
        return util_strdup("VCC");
    return util_strdup(supply[-code - 1]);
}

static void pinout_blanks(FILE *f, int n)
{
    for (; n > 0; n--)
        fputc(' ', f);
}

/* the longest name of the list, at least `least` */
static int pinout_width(FitList *names, int least)
{
    int w = least;
    int i;

    for (i = 0; i < (names != NULL ? names->count : 0); i++)
        if ((int)strlen(names->items[i]) > w)
            w = (int)strlen(names->items[i]);
    return w;
}

/* the names of the pins above (codes from `top`) or below the package */
static FitList *pinout_names(FitList *slots, const short *codes)
{
    FitList *l = list_create();

    for (; *codes != 0; codes++)
        list_add(l, pinout_name(slots, *codes));
    return l;
}

/* the names written vertically, one column per name in `height` lines;
 * above the package (top != 0) they end in the last line, below it they
 * start in the first */
static void pinout_vertical(FILE *f, FitList *names, int height, int indent, int top)
{
    int dense = util_str_icmp(g_dev_package, "PQFP160") == 0;
    int row, i;

    for (row = 1; row <= height; row++) {
        pinout_blanks(f, indent);
        fputs(dense ? "  " : "    ", f);
        for (i = 0; i < names->count; i++) {
            const char *name = names->items[i];
            int len = (int)strlen(name);

            if (top != 0)
                fputc(len > height - row ? name[row + len - height - 1] : ' ', f);
            else
                fputc(len >= row ? name[row - 1] : ' ', f);
            fputs(dense ? " " : "  ", f);
        }
        fputc('\n', f);
    }
}

/* a line of the picture; "ATF15xx" is printed with the device's number */
static void pinout_text(FILE *f, const char *text)
{
    const char *p = strstr(text, "ATF15xx");

    if (p == NULL) {
        fputs(text, f);
        return;
    }
    fprintf(f, "%.*sATF%s%s", (int)(p - text), text, g_family, p + 7);
}

void dev_print_pinout_ext(FILE *f, FitList *slots)
{
    const FitPackageDesc *pkg = dev_current_package();
    const PinoutExt *po = NULL;
    FitList *used, *top, *bottom;
    int width, jtag_marks, i;

    if (pkg != NULL && pkg->balls != NULL) {
        fprintf(f, "\n%s ball assignments (top view row order)\n", pkg->name);
        fprintf(f, "Vector Ball Function       Signal\n");
        for (int pin = 1; pin <= pkg->n_pins; pin++) {
            const FitBallPin *ball = &pkg->balls[pin - 1];
            const char *name = "";
            for (i = 0; i < (slots != NULL ? slots->count : 0); i++) {
                FitSlot *slot = slots->items[i];
                if (slot->pin == pin && strcmp(slot->signal, FIT_FREE_NAME)) {
                    name = slot->signal;
                    break;
                }
            }
            fprintf(f, "%6d %-4s %-14s %s\n", pin, ball->name,
                    ball->function, name);
        }
        return;
    }

    for (i = 0; i < (int)(sizeof pinouts / sizeof pinouts[0]); i++)
        if (strcmp(pinouts[i].family, g_family) == 0 && util_str_icmp(g_dev_package, pinouts[i].package) == 0)
            po = &pinouts[i];
    if (po == NULL) {
        fprintf(f, "\n\n");
        return;
    }

    /* the left names are right aligned to the longest name of all
     * placed signals, at least 17 characters */
    used = list_create();
    for (i = 0; i < (slots != NULL ? slots->count : 0); i++) {
        FitSlot *slot = slots->items[i];

        if (strcmp(slot->signal, FIT_FREE_NAME) != 0)
            list_add(used, slot->signal);
    }
    width = pinout_width(used, 17);
    /* on the ATF1502 in TQFP44 the JTAG pins are marked with "*" while
     * JTAG is on */
    jtag_marks = g_opt.jtag != 0 && strcmp(g_family, "1502") == 0 && util_str_icmp(g_dev_package, "TQFP44") == 0;
    top = pinout_names(slots, po->top);
    bottom = pinout_names(slots, po->bottom);

    pinout_vertical(f, top, pinout_width(top, 0), width, 1);
    for (i = 0; po->head[i] != NULL; i++) {
        pinout_blanks(f, width);
        fprintf(f, "%s\n", po->head[i]);
    }
    for (i = 0; po->rows[i].text != NULL; i++) {
        char *left = pinout_name(slots, po->rows[i].left);
        char *right = pinout_name(slots, po->rows[i].right);

        if (jtag_marks && (po->rows[i].left == 1 || po->rows[i].left == 7))
            util_str_append(&left, "*");
        if (jtag_marks && (po->rows[i].right == 26 || po->rows[i].right == 32))
            util_str_append(&right, "*");
        pinout_blanks(f, width - (int)strlen(left));
        fputs(left, f);
        pinout_text(f, po->rows[i].text);
        fprintf(f, "%s\n", right);
        free(left);
        free(right);
    }
    for (i = 0; po->foot[i] != NULL; i++) {
        pinout_blanks(f, width);
        fprintf(f, "%s\n", po->foot[i]);
    }
    pinout_vertical(f, bottom, pinout_width(bottom, 0), width, 0);
    fprintf(f, "\n");
    fprintf(f, "\n\n");
    list_destroy(&used);
    list_free_all(&top, util_free_ptr);
    list_free_all(&bottom, util_free_ptr);
}
