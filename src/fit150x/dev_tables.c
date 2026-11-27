/*
 * dev_tables.c - device tables of the ATF1502 / ATF1504 / ATF1508 fitter.
 *
 * For each device: the JEDEC layout (number of fuses, first fuse of each
 * macrocell's product terms, of the macrocell configuration, UIM, global
 * OE, configuration, JTAG, UES and reserved sections, and the NOTE lines
 * of the configuration section), the node names each block input (UIM)
 * and global OE multiplexer can select, and per package the pin of every
 * macrocell and dedicated input, the global clock, clear, OE, JTAG and
 * power-down pins, the test vector pin map, the pin limit and the text of
 * the package drawing with the position of each pin's label.  The tables
 * end in g_dev_descs, indexed by enum FitDevice.
 */
#include "dev.h"

/* ================================================================ */
/* ATF1502 */
/* ================================================================ */

/* ATF1502: first fuse of the product terms of each of the 32 macrocells. */
static const int pt_addr_1502[32] = {
    0, 480, 960, 1440, 1920, 2400, 2880, 3360, 3840, 4320, 4800, 5280, 5760, 6240, 6720, 7200,
    7680, 8160, 8640, 9120, 9600, 10080, 10560, 11040, 11520, 12000, 12480, 12960, 13440, 13920, 14400, 14880,
};

/* ATF1502: fuse address of the UIM section of each block pair. */
static const int uim_addr_1502[1] = { 16320 };

/* ATF1502: the NOTE line jed_write_cfg_note writes after the configuration
 * fuses ending before each of these fuse addresses, 24 entries. */
static const FitJedecNote notes_1502[24] = {
    { 15376, "  NOTE S16,S12 of block A * \n" },
    { 15408, "  NOTE S14,S11 of block A * \n" },
    { 15456, "  NOTE S9 ,S6  of block A * \n" },
    { 15488, "  NOTE S13,S10 of block A * \n" },
    { 15536, "  NOTE S20,S18 of block A * \n" },
    { 15568, "  NOTE S8 ,S21 of block A * \n" },
    { 15616, "  NOTE S7 ,S19 of block A * \n" },
    { 15648, "  NOTE S22,S5  of block A * \n" },
    { 15696, "  NOTE S23,S4  of block A * \n" },
    { 15728, "  NOTE S3 ,S15 of block A * \n" },
    { 15776, "  NOTE S0 ,S1  of block A * \n" },
    { 15808, "  NOTE S17,S2  of block A * \n" },
    { 15856, "  NOTE S16,S12 of block B * \n" },
    { 15888, "  NOTE S14,S11 of block B * \n" },
    { 15936, "  NOTE S9 ,S6  of block B * \n" },
    { 15968, "  NOTE S13,S10 of block B * \n" },
    { 16016, "  NOTE S20,S18 of block B * \n" },
    { 16048, "  NOTE S8 ,S21 of block B * \n" },
    { 16096, "  NOTE S7 ,S19 of block B * \n" },
    { 16128, "  NOTE S22,S5  of block B * \n" },
    { 16176, "  NOTE S23,S4  of block B * \n" },
    { 16208, "  NOTE S3 ,S15 of block B * \n" },
    { 16256, "  NOTE S0 ,S1  of block B * \n" },
    { 16288, "  NOTE S17,S2  of block B * \n" },
};

/* ATF1502: part name, QF, macrocell configuration length and first fuse,
 * global OE, configuration, JTAG, UES and reserved bits addresses, product
 * term and UIM addresses, the two configuration half addresses, the NOTE
 * lines. */
static const FitJedecLayout jedec_1502 = {
    "ATF1502AS", 16808, 960, 15360, 16720, 16750, 16782, 16786, 16802,
    pt_addr_1502, { 15360, 15840 }, uim_addr_1502, notes_1502, 24
};

static const FitJedecLayout jedec_1502_be = {
    "ATF1502BE", 16814, 960, 15360, 16720, 16750, 16782, 16790, 16806,
    pt_addr_1502, { 15360, 15840 }, uim_addr_1502, NULL, 0
};

/* ATF1502, 40 x 5 node names: nodes selectable by each block input multiplexer. */
static const char *const uim_src_1502[200] = {
    "B22p", "B29fb", "A16fb", "GND", "GCLK",
    "B31fb", "B30fb", "A6p", "GND", "A13fb",
    "B32fb", "B31fb", "A8p", "A5p", "A13fb",
    "B21p", "B30fb", "A6p", "A14fb", "GND",
    "B32fb", "B23p", "A7p", "A14fb", "GND",
    "B23p", "B29fb", "A15fb", "A5p", "GND",
    "B24p", "B31fb", "A8p", "A15fb", "GND",
    "B22p", "B30fb", "A16fb", "A7p", "GND",
    "B24p", "B21p", "A16fb", "A15fb", "GCLK",
    "B32fb", "B29fb", "A14fb", "GND", "GCLK",
    "B18p", "B25fb", "A12fb", "GND", "OE1",
    "B27fb", "B26fb", "A2p", "GND", "A9fb",
    "B28fb", "B27fb", "A4p", "A1p", "A9fb",
    "B17p", "B26fb", "A2p", "A10fb", "GND",
    "B28fb", "B19p", "A3p", "A10fb", "GND",
    "B19p", "B25fb", "A11fb", "A1p", "GND",
    "B20p", "B27fb", "A4p", "A11fb", "GND",
    "B18p", "B26fb", "A12fb", "A3p", "GND",
    "B20p", "B17p", "A12fb", "A11fb", "OE1",
    "B28fb", "B25fb", "A10fb", "GND", "OE1",
    "A15p", "A8fb", "B21fb", "GND", "GCLR",
    "A6fb", "A7fb", "B31p", "GND", "B24fb",
    "A5fb", "A6fb", "B29p", "B32p", "B24fb",
    "A16p", "A7fb", "B31p", "B23fb", "GND",
    "A5fb", "A14p", "B30p", "B23fb", "GND",
    "A14p", "A8fb", "B22fb", "B32p", "GND",
    "A13p", "A6fb", "B29p", "B22fb", "GND",
    "A15p", "A7fb", "B21fb", "B30p", "GND",
    "A13p", "A16p", "B21fb", "B22fb", "GCLR",
    "A5fb", "A8fb", "B23fb", "GND", "GCLR",
    "A11p", "A4fb", "B17fb", "GND", "OE2",
    "A2fb", "A3fb", "B27p", "GND", "B20fb",
    "A1fb", "A2fb", "B25p", "B28p", "B20fb",
    "A12p", "A3fb", "B27p", "B19fb", "GND",
    "A1fb", "A10p", "B26p", "B19fb", "GND",
    "A10p", "A4fb", "B18fb", "B28p", "GND",
    "A9p", "A2fb", "B25p", "B18fb", "GND",
    "A11p", "A3fb", "B17fb", "B26p", "GND",
    "A9p", "A12p", "B17fb", "B18fb", "OE2",
    "A1fb", "A4fb", "B19fb", "GND", "OE2",
};

/* ATF1502, 6 x 5 node names: nodes selectable by each global OE multiplexer. */
static const char *const oe_src_1502[30] = {
    "A1fb", "A12p", "B17fb", "B28p", "GND",
    "A11p", "B27p", "B19fb", "GND", "OE2",
    "A13p", "A8fb", "B29p", "B24fb", "GND",
    "A6fb", "A15p", "B22fb", "B31p", "GND",
    "B18p", "A2p", "A10fb", "GND", "OE1",
    "B23p", "B30fb", "A7p", "A14fb", "GND",
};

/* 36 rows: block, macrocell, pin. */
static const FitPkgRow rows_1502_plcc44[36] = {
    {1,1,4}, {1,2,5}, {1,3,6}, {1,4,7}, {1,5,8}, {1,6,9},
    {1,7,11}, {1,8,12}, {1,9,13}, {1,10,14}, {1,11,16}, {1,12,17},
    {1,13,18}, {1,14,19}, {1,15,20}, {1,16,21}, {2,17,41}, {2,18,40},
    {2,19,39}, {2,20,38}, {2,21,37}, {2,22,36}, {2,23,34}, {2,24,33},
    {2,25,32}, {2,26,31}, {2,27,29}, {2,28,28}, {2,29,27}, {2,30,26},
    {2,31,25}, {2,32,24}, {0,0,2}, {0,0,1}, {0,0,44}, {0,0,43},
};
/* the 14 pins that can drive a global OE */
static const int oe_pins_1502_plcc44[14] = { 44, 2, 17, 28, 16, 29, 18, 27, 20, 25, 40, 5, 34, 11 };
/* side and line of the label of each of the 44 pins */
static const FitPkgLabel labels_1502_plcc44[44] = {
    {3,33}, {3,30}, {3,27}, {3,24}, {3,21}, {3,18}, {1,14}, {1,15}, {1,16}, {1,17},
    {1,18}, {1,19}, {1,20}, {1,21}, {1,22}, {1,23}, {1,24}, {2,17}, {2,20}, {2,23},
    {2,26}, {2,29}, {2,32}, {2,35}, {2,38}, {2,41}, {2,44}, {2,47}, {0,24}, {0,23},
    {0,22}, {0,21}, {0,20}, {0,19}, {0,18}, {0,17}, {0,16}, {0,15}, {0,14}, {3,47},
    {3,44}, {3,41}, {3,38}, {3,35},
};
/* The 39 lines of the package drawing; dev_draw_package copies them into
 * rows of the drawing buffer (100 bytes per row) and ends 39 rows at
 * column 65. */
static const char *const drawing_1502_plcc44[39] = {
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                           V             G                       ",
    "                           C             N                       ",
    "                           C             D                       ",
    "               ____________________________________               ",
    "              /   6  5  4  3  2  1 44 43 42 41 40  \\              ",
    "             |  7                                39 |             ",
    "             |  8                                38 |             ",
    "             |  9                                37 |             ",
    "         GND | 10                                36 |             ",
    "             | 11                                35 | VCC         ",
    "             | 12            ATF1502             34 |             ",
    "             | 13          44-Lead PLCC          33 |             ",
    "             | 14                                32 |             ",
    "         VCC | 15                                31 |             ",
    "             | 16                                30 | GND         ",
    "             | 17                                29 |             ",
    "             |   18 19 20 21 22 23 24 25 26 27 28   |             ",
    "              \\____________________________________/              ",
    "                             G  V                                ",
    "                             N  C                                ",
    "                             D  C                                ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
};
static const unsigned char drawing_row_1502_plcc44[39] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23,
    24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38,
};

/* 36 rows: block, macrocell, pin. */
static const FitPkgRow rows_1502_tqfp44[36] = {
    {1,1,42}, {1,2,43}, {1,3,44}, {1,4,1}, {1,5,2}, {1,6,3},
    {1,7,5}, {1,8,6}, {1,9,7}, {1,10,8}, {1,11,10}, {1,12,11},
    {1,13,12}, {1,14,13}, {1,15,14}, {1,16,15}, {2,17,35}, {2,18,34},
    {2,19,33}, {2,20,32}, {2,21,31}, {2,22,30}, {2,23,28}, {2,24,27},
    {2,25,26}, {2,26,25}, {2,27,23}, {2,28,22}, {2,29,21}, {2,30,20},
    {2,31,19}, {2,32,18}, {0,0,40}, {0,0,39}, {0,0,38}, {0,0,37},
};
/* the 14 pins that can drive a global OE */
static const int oe_pins_1502_tqfp44[14] = { 38, 40, 11, 22, 10, 23, 12, 21, 14, 19, 34, 43, 28, 5 };
/* side and line of the label of each of the 44 pins */
static const FitPkgLabel labels_1502_tqfp44[44] = {
    {1,14}, {1,15}, {1,16}, {1,17}, {1,18}, {1,19}, {1,20}, {1,21}, {1,22}, {1,23},
    {1,24}, {2,17}, {2,20}, {2,23}, {2,26}, {2,29}, {2,32}, {2,35}, {2,38}, {2,41},
    {2,44}, {2,47}, {0,24}, {0,23}, {0,22}, {0,21}, {0,20}, {0,19}, {0,18}, {0,17},
    {0,16}, {0,15}, {0,14}, {3,47}, {3,44}, {3,41}, {3,38}, {3,35}, {3,32}, {3,29},
    {3,26}, {3,23}, {3,20}, {3,17},
};
/* The 39 lines of the package drawing; dev_draw_package copies them into
 * rows of the drawing buffer (100 bytes per row) and ends 39 rows at
 * column 65. */
static const char *const drawing_1502_tqfp44[39] = {
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                          V              G                       ",
    "                          C              N                       ",
    "                          C              D                       ",
    "               ____________________________________               ",
    "              /  44 43 42 41 40 39 38 37 36 35 34  \\             ",
    "             |  1                                33 |             ",
    "             |  2                                32 |             ",
    "             |  3                                31 |             ",
    "         GND |  4                                30 |             ",
    "             |  5                                29 | VCC         ",
    "             |  6            ATF1502             28 |             ",
    "             |  7          44-Lead TQFP          27 |             ",
    "             |  8                                26 |             ",
    "         VCC |  9                                25 |             ",
    "             | 10                                24 | GND         ",
    "             | 11                                23 |             ",
    "             |   12 13 14 15 16 17 18 19 20 21 22   |             ",
    "              \\____________________________________/             ",
    "                             G  V                                ",
    "                             N  C                                ",
    "                             D  C                                ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
};
static const unsigned char drawing_row_1502_tqfp44[39] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23,
    24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38,
};

/* ATF1502 packages, with their special pins (global clocks, global clear,
 * JTAG, power down), test vector pin maps and pin limits. */
static const FitPackageDesc packages_1502[2] = {
    { FIT_PKG_PLCC44, "PLCC44", "P1502C44", 44, 36, rows_1502_plcc44,
      { 43, 2, 41 }, 1, oe_pins_1502_plcc44, 14, { 7, 13, 32, 38 }, { 11, 25 },
      "XXNXXXXXXNXXXXNXXXXXXNNXXXXXXNXXXXNXXXXXXNXX",
      drawing_1502_plcc44, drawing_row_1502_plcc44, 39, 65, 39, 100,
      { 0, 12, 54, 10, 0, 11, 27, 10 }, labels_1502_plcc44, NULL },
    { FIT_PKG_TQFP44, "TQFP44", "P1502T44", 44, 36, rows_1502_tqfp44,
      { 37, 40, 35 }, 39, oe_pins_1502_tqfp44, 14, { 1, 7, 26, 32 }, { 5, 19 },
      "XXXNXXXXNXXXXXXNNXXXXXXNXXXXNXXXXXXNXXXXNXXX",
      drawing_1502_tqfp44, drawing_row_1502_tqfp44, 39, 65, 39, 100,
      { 0, 12, 54, 10, 0, 11, 27, 10 }, labels_1502_tqfp44, NULL },
};

/* ATF1502: vector pin maps for package names that are not packages of
 * this device (used only with the package name of another device). */
static const FitVectorMap vector_maps_1502[1] = {
    { NULL, NULL },
};

/* ================================================================ */
/* ATF1504 */
/* ================================================================ */

/* ATF1504: first fuse of the product terms of each of the 64 macrocells. */
static const int pt_addr_1504[64] = {
    7200, 6720, 6240, 5760, 5280, 4800, 4320, 3840, 3360, 2880, 2400, 1920, 1440, 960, 480, 0,
    14880, 14400, 13920, 13440, 12960, 12480, 12000, 11520, 11040, 10560, 10080, 9600, 9120, 8640, 8160, 7680,
    30240, 29760, 29280, 28800, 28320, 27840, 27360, 26880, 26400, 25920, 25440, 24960, 24480, 24000, 23520, 23040,
    22560, 22080, 21600, 21120, 20640, 20160, 19680, 19200, 18720, 18240, 17760, 17280, 16800, 16320, 15840, 15360,
};

/* ATF1504: fuse address of the UIM section of each block pair. */
static const int uim_addr_1504[2] = { 32640, 33360 };

/* ATF1504: the NOTE line jed_write_cfg_note writes after the configuration
 * fuses ending before each of these fuse addresses, 48 entries. */
static const FitJedecNote notes_1504[48] = {
    { 30736, "  NOTE S16,S12 of block A * \n" },
    { 30768, "  NOTE S14,S11 of block A * \n" },
    { 30816, "  NOTE S16,S12 of block B * \n" },
    { 30848, "  NOTE S14,S11 of block B * \n" },
    { 30896, "  NOTE S9 ,S6  of block A * \n" },
    { 30928, "  NOTE S13,S10 of block A * \n" },
    { 30976, "  NOTE S9 ,S6  of block B * \n" },
    { 31008, "  NOTE S13,S10 of block B * \n" },
    { 31056, "  NOTE S20,S18 of block A * \n" },
    { 31088, "  NOTE S8 ,S21 of block A * \n" },
    { 31136, "  NOTE S20,S18 of block B * \n" },
    { 31168, "  NOTE S8 ,S21 of block B * \n" },
    { 31216, "  NOTE S7 ,S19 of block A * \n" },
    { 31248, "  NOTE S22,S5  of block A * \n" },
    { 31296, "  NOTE S7 ,S19 of block B * \n" },
    { 31328, "  NOTE S22,S5  of block B * \n" },
    { 31376, "  NOTE S23,S4  of block A * \n" },
    { 31408, "  NOTE S3 ,S15 of block A * \n" },
    { 31456, "  NOTE S23,S4  of block B * \n" },
    { 31488, "  NOTE S3 ,S15 of block B * \n" },
    { 31536, "  NOTE S0 ,S1  of block A * \n" },
    { 31568, "  NOTE S17,S2  of block A * \n" },
    { 31616, "  NOTE S0 ,S1  of block B * \n" },
    { 31648, "  NOTE S17,S2  of block B * \n" },
    { 31696, "  NOTE S16,S12 of block D * \n" },
    { 31728, "  NOTE S14,S11 of block D * \n" },
    { 31776, "  NOTE S16,S12 of block C * \n" },
    { 31808, "  NOTE S14,S11 of block C * \n" },
    { 31856, "  NOTE S9 ,S6  of block D * \n" },
    { 31888, "  NOTE S13,S10 of block D * \n" },
    { 31936, "  NOTE S9 ,S6  of block C * \n" },
    { 31968, "  NOTE S13,S10 of block C * \n" },
    { 32016, "  NOTE S20,S18 of block D * \n" },
    { 32048, "  NOTE S8 ,S21 of block D * \n" },
    { 32096, "  NOTE S20,S18 of block C * \n" },
    { 32128, "  NOTE S8 ,S21 of block C * \n" },
    { 32176, "  NOTE S7 ,S19 of block D * \n" },
    { 32208, "  NOTE S22,S5  of block D * \n" },
    { 32256, "  NOTE S7 ,S19 of block C * \n" },
    { 32288, "  NOTE S22,S5  of block C * \n" },
    { 32336, "  NOTE S23,S4  of block D * \n" },
    { 32368, "  NOTE S3 ,S15 of block D * \n" },
    { 32416, "  NOTE S23,S4  of block C * \n" },
    { 32448, "  NOTE S3 ,S15 of block C * \n" },
    { 32496, "  NOTE S0 ,S1  of block D * \n" },
    { 32528, "  NOTE S17,S2  of block D * \n" },
    { 32576, "  NOTE S0 ,S1  of block C * \n" },
    { 32608, "  NOTE S17,S2  of block C * \n" },
};

/* ATF1504: part name, QF, macrocell configuration length and first fuse,
 * global OE, configuration, JTAG, UES and reserved bits addresses, product
 * term and UIM addresses, the two configuration half addresses, the NOTE
 * lines. */
static const FitJedecLayout jedec_1504 = {
    "ATF1504AS", 34192, 1920, 30720, 34080, 34134, 34166, 34170, 34186,
    pt_addr_1504, { 30720, 31680 }, uim_addr_1504, notes_1504, 48
};

static const FitJedecLayout jedec_1504_be = {
    "ATF1504BE", 34198, 1920, 30720, 34080, 34134, 34166, 34174, 34190,
    pt_addr_1504, { 30720, 31680 }, uim_addr_1504, NULL, 0
};

/* ATF1504, 40 x 9 node names: nodes selectable by each block input multiplexer. */
static const char *const uim_src_1504[360] = {
    "A9p", "A2fb", "A11p", "B24fb", "B31p", "C40fb", "C48p", "D60p", "D51fb",
    "D52p", "D59fb", "D50p", "C45fb", "C38p", "B29fb", "B21p", "A1p", "A10fb",
    "C46p", "A3fb", "B29p", "B22fb", "B32p", "C38fb", "D60p", "D52fb", "GND",
    "B23p", "D58fb", "C40p", "C47fb", "C37p", "B31fb", "A1p", "A9fb", "GND",
    "D59p", "A4fb", "B21fb", "B23fb", "B30p", "C37fb", "C48p", "D52fb", "GND",
    "A2p", "D57fb", "C48fb", "C46fb", "C39p", "B32fb", "B21p", "A9fb", "GND",
    "A1fb", "A3fb", "A11p", "B22fb", "B30p", "C39fb", "D58p", "C47p", "D51fb",
    "D60fb", "D58fb", "D50p", "C47fb", "C39p", "B30fb", "A3p", "B22p", "A10fb",
    "D57p", "A1fb", "A10p", "B21fb", "B31p", "C37fb", "D49fb", "GND", "D51fb",
    "A4p", "D60fb", "D51p", "C48fb", "C38p", "B32fb", "A12fb", "GND", "A10fb",
    "C46p", "A1fb", "A12p", "B23fb", "C39fb", "C40fb", "D58p", "D50fb", "GND",
    "B23p", "D60fb", "D49p", "C46fb", "B30fb", "B29fb", "A3p", "A11fb", "GND",
    "A9p", "A4fb", "A10p", "B23fb", "C45p", "C38fb", "C47p", "D50fb", "OE2",
    "D52p", "D57fb", "D51p", "C46fb", "B24p", "B31fb", "B22p", "A11fb", "OE1",
    "D59p", "A2fb", "B29p", "B21fb", "C45p", "C39fb", "D49fb", "D52fb", "GND",
    "A2p", "D59fb", "C40p", "C48fb", "B24p", "B30fb", "A12fb", "A9fb", "GND",
    "D57p", "A3fb", "A12p", "B24fb", "B32p", "C37fb", "D49fb", "GND", "OE2",
    "A4p", "D58fb", "D49p", "C45fb", "C37p", "B32fb", "A12fb", "GND", "OE1",
    "A2fb", "A4fb", "B22fb", "B24fb", "C38fb", "C40fb", "D50fb", "GND", "OE2",
    "D59fb", "D57fb", "C47fb", "C45fb", "B31fb", "B29fb", "A11fb", "GND", "OE1",
    "A13p", "A6fb", "A15p", "B20fb", "B27p", "C36fb", "C44p", "D64p", "D55fb",
    "D56p", "D63fb", "D54p", "C41fb", "C34p", "B25fb", "B17p", "A5p", "A14fb",
    "C42p", "A7fb", "B25p", "B18fb", "B28p", "C34fb", "D64p", "D56fb", "GND",
    "B19p", "D62fb", "C36p", "C43fb", "C33p", "B27fb", "A5p", "A13fb", "GND",
    "D63p", "A8fb", "B17fb", "B19fb", "B26p", "C33fb", "C44p", "D56fb", "GND",
    "A6p", "D61fb", "C44fb", "C42fb", "C35p", "B28fb", "B17p", "A13fb", "GND",
    "A5fb", "A7fb", "A15p", "B18fb", "B26p", "C35fb", "D62p", "C43p", "D55fb",
    "D64fb", "D62fb", "D54p", "C43fb", "C35p", "B26fb", "A7p", "B18p", "A14fb",
    "D61p", "A5fb", "A14p", "B17fb", "B27p", "C33fb", "D53fb", "GND", "D55fb",
    "A8p", "D64fb", "D55p", "C44fb", "C34p", "B28fb", "A16fb", "GND", "A14fb",
    "C42p", "A5fb", "A16p", "B19fb", "C35fb", "C36fb", "D62p", "D54fb", "GND",
    "B19p", "D64fb", "D53p", "C42fb", "B26fb", "B25fb", "A7p", "A15fb", "GND",
    "A13p", "A8fb", "A14p", "B19fb", "C41p", "C34fb", "C43p", "D54fb", "GCLR",
    "D56p", "D61fb", "D55p", "C42fb", "B20p", "B27fb", "B18p", "A15fb", "GCLK",
    "D63p", "A6fb", "B25p", "B17fb", "C41p", "C35fb", "D53fb", "D56fb", "GND",
    "A6p", "D63fb", "C36p", "C44fb", "B20p", "B26fb", "A16fb", "A13fb", "GND",
    "D61p", "A7fb", "A16p", "B20fb", "B28p", "C33fb", "D53fb", "GND", "GCLR",
    "A8p", "D62fb", "D53p", "C41fb", "C33p", "B28fb", "A16fb", "GND", "GCLK",
    "A6fb", "A8fb", "B18fb", "B20fb", "C34fb", "C36fb", "D54fb", "GND", "GCLR",
    "D63fb", "D61fb", "C43fb", "C41fb", "B27fb", "B25fb", "A15fb", "GND", "GCLK",
};

/* ATF1504, 6 x 9 node names: nodes selectable by each global OE multiplexer. */
static const char *const oe_src_1504[54] = {
    "GND", "D51p", "C40p", "C46fb", "B24p", "B30fb", "A3p", "A11fb", "OE1",
    "D57p", "A4fb", "A11p", "B24fb", "B30p", "C40fb", "GND", "D52fb", "OE2",
    "C46p", "A1fb", "A3fb", "A11p", "B21fb", "B30p", "GND", "D49fb", "D51fb",
    "A14p", "B25p", "B17fb", "B20fb", "C41p", "C33fb", "C36fb", "D62p", "GND",
    "A5fb", "A16p", "B25p", "B19fb", "C41p", "C35fb", "D64p", "D53fb", "GND",
    "B19p", "D62fb", "D53p", "C41fb", "C35p", "B25fb", "B17p", "A5p", "GND",
};

/* 68 rows: block, macrocell, pin. */
static const FitPkgRow rows_1504_plcc44[68] = {
    {1,1,12}, {1,2,0}, {1,3,11}, {1,4,9}, {1,5,8}, {1,6,0},
    {1,7,0}, {1,8,7}, {1,9,0}, {1,10,0}, {1,11,6}, {1,12,0},
    {1,13,0}, {1,14,5}, {1,15,0}, {1,16,4}, {2,17,21}, {2,18,0},
    {2,19,20}, {2,20,19}, {2,21,18}, {2,22,0}, {2,23,0}, {2,24,17},
    {2,25,16}, {2,26,0}, {2,27,0}, {2,28,0}, {2,29,0}, {2,30,14},
    {2,31,0}, {2,32,13}, {3,33,24}, {3,34,0}, {3,35,25}, {3,36,26},
    {3,37,27}, {3,38,0}, {3,39,0}, {3,40,28}, {3,41,29}, {3,42,0},
    {3,43,0}, {3,44,0}, {3,45,0}, {3,46,31}, {3,47,0}, {3,48,32},
    {4,49,33}, {4,50,0}, {4,51,34}, {4,52,36}, {4,53,37}, {4,54,0},
    {4,55,0}, {4,56,38}, {4,57,39}, {4,58,0}, {4,59,0}, {4,60,0},
    {4,61,0}, {4,62,40}, {4,63,0}, {4,64,41}, {0,0,2}, {0,0,1},
    {0,0,44}, {0,0,43},
};
/* the 21 pins that can drive a global OE */
static const int oe_pins_1504_plcc44[21] = { 44, 2, 34, 28, 17, 11, 39, 6, 14, 31, 5, 16, 29, 40, 4, 41, 20, 37, 25, 21, 8 };
/* side and line of the label of each of the 44 pins */
static const FitPkgLabel labels_1504_plcc44[44] = {
    {3,33}, {3,30}, {3,27}, {3,24}, {3,21}, {3,18}, {1,14}, {1,15}, {1,16}, {1,17},
    {1,18}, {1,19}, {1,20}, {1,21}, {1,22}, {1,23}, {1,24}, {2,17}, {2,20}, {2,23},
    {2,26}, {2,29}, {2,32}, {2,35}, {2,38}, {2,41}, {2,44}, {2,47}, {0,24}, {0,23},
    {0,22}, {0,21}, {0,20}, {0,19}, {0,18}, {0,17}, {0,16}, {0,15}, {0,14}, {3,47},
    {3,44}, {3,41}, {3,38}, {3,35},
};
/* The 39 lines of the package drawing; dev_draw_package copies them into
 * rows of the drawing buffer (100 bytes per row) and ends 39 rows at
 * column 65. */
static const char *const drawing_1504_plcc44[39] = {
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                           V             G                       ",
    "                           C             N                       ",
    "                           C             D                       ",
    "               ____________________________________               ",
    "              /   6  5  4  3  2  1 44 43 42 41 40  \\              ",
    "             |  7                                39 |             ",
    "             |  8                                38 |             ",
    "             |  9                                37 |             ",
    "         GND | 10                                36 |             ",
    "             | 11                                35 | VCC         ",
    "             | 12            ATF1504             34 |             ",
    "             | 13          44-Lead PLCC          33 |             ",
    "             | 14                                32 |             ",
    "         VCC | 15                                31 |             ",
    "             | 16                                30 | GND         ",
    "             | 17                                29 |             ",
    "             |   18 19 20 21 22 23 24 25 26 27 28   |             ",
    "              \\____________________________________/              ",
    "                             G  V                                ",
    "                             N  C                                ",
    "                             D  C                                ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
};
static const unsigned char drawing_row_1504_plcc44[39] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23,
    24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38,
};

/* 68 rows: block, macrocell, pin. */
static const FitPkgRow rows_1504_tqfp44[68] = {
    {1,1,6}, {1,2,0}, {1,3,5}, {1,4,3}, {1,5,2}, {1,6,0},
    {1,7,0}, {1,8,1}, {1,9,0}, {1,10,0}, {1,11,44}, {1,12,0},
    {1,13,0}, {1,14,43}, {1,15,0}, {1,16,42}, {2,17,15}, {2,18,0},
    {2,19,14}, {2,20,13}, {2,21,12}, {2,22,0}, {2,23,0}, {2,24,11},
    {2,25,10}, {2,26,0}, {2,27,0}, {2,28,0}, {2,29,0}, {2,30,8},
    {2,31,0}, {2,32,7}, {3,33,18}, {3,34,0}, {3,35,19}, {3,36,20},
    {3,37,21}, {3,38,0}, {3,39,0}, {3,40,22}, {3,41,23}, {3,42,0},
    {3,43,0}, {3,44,0}, {3,45,0}, {3,46,25}, {3,47,0}, {3,48,26},
    {4,49,27}, {4,50,0}, {4,51,28}, {4,52,30}, {4,53,31}, {4,54,0},
    {4,55,0}, {4,56,32}, {4,57,33}, {4,58,0}, {4,59,0}, {4,60,0},
    {4,61,0}, {4,62,34}, {4,63,0}, {4,64,35}, {0,0,40}, {0,0,39},
    {0,0,38}, {0,0,37},
};
/* the 21 pins that can drive a global OE */
static const int oe_pins_1504_tqfp44[21] = { 38, 40, 28, 22, 11, 5, 33, 44, 8, 25, 43, 10, 23, 34, 42, 35, 14, 31, 19, 15, 2 };
/* side and line of the label of each of the 44 pins */
static const FitPkgLabel labels_1504_tqfp44[44] = {
    {1,14}, {1,15}, {1,16}, {1,17}, {1,18}, {1,19}, {1,20}, {1,21}, {1,22}, {1,23},
    {1,24}, {2,17}, {2,20}, {2,23}, {2,26}, {2,29}, {2,32}, {2,35}, {2,38}, {2,41},
    {2,44}, {2,47}, {0,24}, {0,23}, {0,22}, {0,21}, {0,20}, {0,19}, {0,18}, {0,17},
    {0,16}, {0,15}, {0,14}, {3,47}, {3,44}, {3,41}, {3,38}, {3,35}, {3,32}, {3,29},
    {3,26}, {3,23}, {3,20}, {3,17},
};
/* The 39 lines of the package drawing; dev_draw_package copies them into
 * rows of the drawing buffer (100 bytes per row) and ends 39 rows at
 * column 65. */
static const char *const drawing_1504_tqfp44[39] = {
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                          V              G                       ",
    "                          C              N                       ",
    "                          C              D                       ",
    "               ____________________________________               ",
    "              /  44 43 42 41 40 39 38 37 36 35 34  \\             ",
    "             |  1                                33 |             ",
    "             |  2                                32 |             ",
    "             |  3                                31 |             ",
    "         GND |  4                                30 |             ",
    "             |  5                                29 | VCC         ",
    "             |  6            ATF1504             28 |             ",
    "             |  7          44-Lead TQFP          27 |             ",
    "             |  8                                26 |             ",
    "         VCC |  9                                25 |             ",
    "             | 10                                24 | GND         ",
    "             | 11                                23 |             ",
    "             |   12 13 14 15 16 17 18 19 20 21 22   |             ",
    "              \\____________________________________/             ",
    "                             G  V                                ",
    "                             N  C                                ",
    "                             D  C                                ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
    "                                                                  ",
};
static const unsigned char drawing_row_1504_tqfp44[39] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23,
    24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38,
};

/* 68 rows: block, macrocell, pin. */
static const FitPkgRow rows_1504_plcc68[68] = {
    {1,1,18}, {1,2,0}, {1,3,17}, {1,4,15}, {1,5,14}, {1,6,13},
    {1,7,0}, {1,8,12}, {1,9,10}, {1,10,0}, {1,11,9}, {1,12,8},
    {1,13,7}, {1,14,5}, {1,15,0}, {1,16,4}, {2,17,33}, {2,18,0},
    {2,19,32}, {2,20,30}, {2,21,29}, {2,22,28}, {2,23,0}, {2,24,27},
    {2,25,25}, {2,26,0}, {2,27,24}, {2,28,23}, {2,29,22}, {2,30,20},
    {2,31,0}, {2,32,19}, {3,33,36}, {3,34,0}, {3,35,37}, {3,36,39},
    {3,37,40}, {3,38,41}, {3,39,0}, {3,40,42}, {3,41,44}, {3,42,0},
    {3,43,45}, {3,44,46}, {3,45,47}, {3,46,49}, {3,47,0}, {3,48,50},
    {4,49,51}, {4,50,0}, {4,51,52}, {4,52,54}, {4,53,55}, {4,54,56},
    {4,55,0}, {4,56,57}, {4,57,59}, {4,58,0}, {4,59,60}, {4,60,61},
    {4,61,62}, {4,62,64}, {4,63,0}, {4,64,65}, {0,0,2}, {0,0,1},
    {0,0,68}, {0,0,67},
};
/* the 21 pins that can drive a global OE */
static const int oe_pins_1504_plcc68[21] = { 68, 2, 52, 42, 27, 17, 59, 9, 20, 49, 5, 25, 44, 64, 4, 65, 32, 55, 37, 33, 14 };
/* side and line of the label of each of the 68 pins */
static const FitPkgLabel labels_1504_plcc68[68] = {
    {3,37}, {3,35}, {3,33}, {3,31}, {3,29}, {3,27}, {3,25}, {3,23}, {3,21}, {1,16},
    {1,17}, {1,18}, {1,19}, {1,20}, {1,21}, {1,22}, {1,23}, {1,24}, {1,25}, {1,26},
    {1,27}, {1,28}, {1,29}, {1,30}, {1,31}, {1,32}, {2,22}, {2,24}, {2,26}, {2,28},
    {2,30}, {2,32}, {2,34}, {2,36}, {2,38}, {2,40}, {2,42}, {2,44}, {2,46}, {2,48},
    {2,50}, {2,52}, {2,54}, {0,32}, {0,31}, {0,30}, {0,29}, {0,28}, {0,27}, {0,26},
    {0,25}, {0,24}, {0,23}, {0,22}, {0,21}, {0,20}, {0,19}, {0,18}, {0,17}, {0,16},
    {3,53}, {3,51}, {3,49}, {3,47}, {3,45}, {3,43}, {3,41}, {3,39},
};
/* The 49 lines of the package drawing; dev_draw_package copies them into
 * rows of the drawing buffer (100 bytes per row) and ends 48 rows at
 * column 85. */
static const char *const drawing_1504_plcc68[49] = {
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                           G     V         G     V              \t             ",
    "                           N     C         N     C           \t                     ",
    "                           D     C         D     C           \t                     ",
    "                    ------------------------------------       \t             ",
    "                   / 9   7   5   3   1   67  65  63  61 \\   \t                     ",
    "                  /    8   6   4   2   68  66  64  62    \\              \t     ",
    "                 | 10               (*)                60 |    \t             ",
    "             VCC | 11                                  59 |          \t             ",
    "                 | 12                                  58 | GND \t             ",
    "                 | 13                                  57 |         \t             ",
    "                 | 14                                  56 |          \t             ",
    "                 | 15                                  55 |          \t             ",
    "             GND | 16                                  54 |          \t             ",
    "                 | 17            ATF1504               53 | VCC         \t     ",
    "                 | 18          68-Lead PLCC            52 |  \t                     ",
    "                 | 19                                  51 |          \t             ",
    "                 | 20                                  50 |          \t             ",
    "             VCC | 21                                  49 |          \t             ",
    "                 | 22                                  48 | GND         \t     ",
    "                 | 23                                  47 |          \t             ",
    "                 | 24                                  46 |          \t             ",
    "                 | 25                                  45 |  \t                     ",
    "             GND | 26                                  44 |          \t             ",
    "                  \\     28  30  32  34  36  38  40  42   / \t                     ",
    "                   \\  27  29  31  33  35  37  39  41  43/  \t                     ",
    "                    ------------------------------------\t                     ",
    "                              V     G V     G         V \t                     ",
    "                              C     N C     N         C \t                     ",
    "                              C     D C     D         C  \t                     ",
    "                                                                                   ",
    "                                                                                   ",
    "                                                                                   ",
    "                                                                                   ",
    "                                                                                   ",
    "                                                                                   ",
    "                                                                                   ",
    "                                                                                   ",
    "                                                                                   ",
    "                                                                                   ",
};
static const unsigned char drawing_row_1504_plcc68[49] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23,
    24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
    48,
};

/* 68 rows: block, macrocell, pin. */
static const FitPkgRow rows_1504_plcc84[68] = {
    {1,1,22}, {1,2,21}, {1,3,20}, {1,4,18}, {1,5,17}, {1,6,16},
    {1,7,15}, {1,8,14}, {1,9,12}, {1,10,11}, {1,11,10}, {1,12,9},
    {1,13,8}, {1,14,6}, {1,15,5}, {1,16,4}, {2,17,41}, {2,18,40},
    {2,19,39}, {2,20,37}, {2,21,36}, {2,22,35}, {2,23,34}, {2,24,33},
    {2,25,31}, {2,26,30}, {2,27,29}, {2,28,28}, {2,29,27}, {2,30,25},
    {2,31,24}, {2,32,23}, {3,33,44}, {3,34,45}, {3,35,46}, {3,36,48},
    {3,37,49}, {3,38,50}, {3,39,51}, {3,40,52}, {3,41,54}, {3,42,55},
    {3,43,56}, {3,44,57}, {3,45,58}, {3,46,60}, {3,47,61}, {3,48,62},
    {4,49,63}, {4,50,64}, {4,51,65}, {4,52,67}, {4,53,68}, {4,54,69},
    {4,55,70}, {4,56,71}, {4,57,73}, {4,58,74}, {4,59,75}, {4,60,76},
    {4,61,77}, {4,62,79}, {4,63,80}, {4,64,81}, {0,0,2}, {0,0,1},
    {0,0,84}, {0,0,83},
};
/* the 21 pins that can drive a global OE */
static const int oe_pins_1504_plcc84[21] = { 84, 2, 65, 52, 33, 20, 73, 10, 25, 60, 6, 31, 54, 79, 4, 81, 39, 68, 46, 41, 17 };
/* side and line of the label of each of the 84 pins */
static const FitPkgLabel labels_1504_plcc84[84] = {
    {3,42}, {3,40}, {3,38}, {3,36}, {3,34}, {3,32}, {3,30}, {3,28}, {3,26}, {3,23},
    {3,21}, {1,16}, {1,17}, {1,18}, {1,19}, {1,20}, {1,21}, {1,22}, {1,23}, {1,24},
    {1,25}, {1,26}, {1,27}, {1,28}, {1,29}, {1,30}, {1,31}, {1,32}, {1,33}, {1,34},
    {1,35}, {1,36}, {2,22}, {2,24}, {2,26}, {2,28}, {2,30}, {2,32}, {2,34}, {2,36},
    {2,38}, {2,40}, {2,42}, {2,44}, {2,46}, {2,48}, {2,50}, {2,52}, {2,54}, {2,56},
    {2,58}, {2,60}, {2,62}, {0,36}, {0,35}, {0,34}, {0,33}, {0,32}, {0,31}, {0,30},
    {0,29}, {0,28}, {0,27}, {0,26}, {0,25}, {0,24}, {0,23}, {0,22}, {0,21}, {0,20},
    {0,19}, {0,18}, {0,17}, {0,16}, {3,61}, {3,59}, {3,57}, {3,55}, {3,53}, {3,51},
    {3,49}, {3,47}, {3,45}, {3,43},
};
/* The 53 lines of the package drawing; dev_draw_package copies them into
 * rows of the drawing buffer (100 bytes per row) and ends 52 rows at
 * column 85. */
static const char *const drawing_1504_plcc84[53] = {
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                              G       V        G       V                           ",
    "                              N       C        N       C                           ",
    "                              D       C        D       C                           ",
    "                    -------------------------------------------                     ",
    "                   / 11   9   7   5   3   1  83  81  79  77  75 \\                   ",
    "                  /    10   8   6   4   2  84  82  80  78  76    \\                  ",
    "                 | 12                    (*)                   74 |                 ",
    "             VCC | 13                                          73 |                 ",
    "                 | 14                                          72 | GND             ",
    "                 | 15                                          71 |                 ",
    "                 | 16                                          70 |                 ",
    "                 | 17                                          69 |                 ",
    "                 | 18                                          68 |                 ",
    "             GND | 19                                          67 |                 ",
    "                 | 20                                          66 | VCC             ",
    "                 | 21                                          65 |                 ",
    "                 | 22                 ATF1504                  64 |                 ",
    "                 | 23               84-Lead PLCC               63 |                 ",
    "                 | 24                                          62 |                 ",
    "                 | 25                                          61 |                 ",
    "             VCC | 26                                          60 |                 ",
    "                 | 27                                          59 | GND             ",
    "                 | 28                                          58 |                 ",
    "                 | 29                                          57 |                 ",
    "                 | 30                                          56 |                 ",
    "                 | 31                                          55 |                 ",
    "             GND | 32                                          54 |                 ",
    "                  \\     34  36  38  40  42  44  46  48  50  52   /                  ",
    "                   \\  33  35  37  39  41  43  45  47  49  51  53/                   ",
    "              \t    --------------------------------------------                    ",
    "                                V       G V       G           V                     ",
    "                                C       N C       N           C                     ",
    "                                C       D C       D           C                     ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
};
static const unsigned char drawing_row_1504_plcc84[53] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23,
    24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
    48, 49, 50, 51, 52,
};

/* 68 rows: block, macrocell, pin. */
static const FitPkgRow rows_1504_pqfp100[68] = {
    {1,1,16}, {1,2,15}, {1,3,14}, {1,4,12}, {1,5,11}, {1,6,10},
    {1,7,8}, {1,8,6}, {1,9,4}, {1,10,3}, {1,11,100}, {1,12,99},
    {1,13,98}, {1,14,96}, {1,15,95}, {1,16,94}, {2,17,39}, {2,18,38},
    {2,19,37}, {2,20,35}, {2,21,34}, {2,22,33}, {2,23,32}, {2,24,31},
    {2,25,27}, {2,26,25}, {2,27,23}, {2,28,22}, {2,29,21}, {2,30,19},
    {2,31,18}, {2,32,17}, {3,33,42}, {3,34,43}, {3,35,44}, {3,36,46},
    {3,37,47}, {3,38,48}, {3,39,49}, {3,40,50}, {3,41,54}, {3,42,56},
    {3,43,58}, {3,44,59}, {3,45,60}, {3,46,62}, {3,47,63}, {3,48,64},
    {4,49,65}, {4,50,66}, {4,51,67}, {4,52,69}, {4,53,70}, {4,54,71},
    {4,55,73}, {4,56,75}, {4,57,77}, {4,58,78}, {4,59,81}, {4,60,82},
    {4,61,83}, {4,62,85}, {4,63,86}, {4,64,87}, {0,0,92}, {0,0,91},
    {0,0,90}, {0,0,89},
};
/* the 21 pins that can drive a global OE */
static const int oe_pins_1504_pqfp100[21] = { 90, 92, 67, 50, 31, 14, 77, 100, 19, 62, 96, 27, 54, 85, 94, 87, 37, 70, 44, 39, 11 };
/* side and line of the label of each of the 100 pins */
static const FitPkgLabel labels_1504_pqfp100[100] = {
    {1,16}, {1,17}, {1,18}, {1,19}, {1,20}, {1,21}, {1,22}, {1,23}, {1,24}, {1,25},
    {1,26}, {1,27}, {1,28}, {1,29}, {1,30}, {1,31}, {1,32}, {1,33}, {1,34}, {1,35},
    {1,36}, {1,37}, {1,38}, {1,39}, {1,40}, {1,41}, {1,42}, {1,43}, {1,44}, {1,45},
    {2,25}, {2,27}, {2,29}, {2,31}, {2,33}, {2,35}, {2,37}, {2,39}, {2,41}, {2,43},
    {2,45}, {2,47}, {2,49}, {2,51}, {2,53}, {2,55}, {2,57}, {2,59}, {2,61}, {2,63},
    {0,45}, {0,44}, {0,43}, {0,42}, {0,41}, {0,40}, {0,39}, {0,38}, {0,37}, {0,36},
    {0,35}, {0,34}, {0,33}, {0,32}, {0,31}, {0,30}, {0,29}, {0,28}, {0,27}, {0,26},
    {0,25}, {0,24}, {0,23}, {0,22}, {0,21}, {0,20}, {0,19}, {0,18}, {0,17}, {0,16},
    {3,63}, {3,61}, {3,59}, {3,57}, {3,55}, {3,53}, {3,51}, {3,49}, {3,47}, {3,45},
    {3,43}, {3,41}, {3,39}, {3,37}, {3,35}, {3,33}, {3,31}, {3,29}, {3,27}, {3,25},
};
/* The 63 lines of the package drawing; dev_draw_package copies them into
 * rows of the drawing buffer (100 bytes per row) and ends 0 rows at
 * column 82. */
static const char *const drawing_1504_pqfp100[63] = {
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                               G       V   G   G G       V                          ",
    "                               N       C   N   N N       C                          ",
    "                               D       C   D   D D       C                          ",
    "                       --------------------------------------------                 ",
    "                      / 100  98  96  94  92  90  88  86  84  82    \\                ",
    "                     /     99  97  95  93  91  89  87  85  83  81   \\               ",
    "                 NC |  1 (*)                                      80 | NC           ",
    "                 NC |  2                                          79 | NC           ",
    "                    |  3                                          78 |              ",
    "                    |  4                                          77 |              ",
    "                VCC |  5                                          76 | GND          ",
    "                    |  6                                          75 |              ",
    "                 NC |  7                                          74 | NC           ",
    "                    |  8                                          73 |              ",
    "                 NC |  9                                          72 | NC           ",
    "                    | 10                                          71 |              ",
    "                    | 11                                          70 |              ",
    "                    | 12                                          69 |              ",
    "                GND | 13                                          68 | VCC          ",
    "                    | 14                                          67 |              ",
    "                    | 15                                          66 |              ",
    "                    | 16                ATF1504                   65 |              ",
    "                    | 17             100-Lead PQFP                64 |              ",
    "                    | 18                                          63 |              ",
    "                    | 19                                          62 |              ",
    "                VCC | 20                                          61 | GND          ",
    "                    | 21                                          60 |              ",
    "                    | 22                                          59 |              ",
    "                    | 23                                          58 |              ",
    "                 NC | 24                                          57 | NC           ",
    "                    | 25                                          56 |              ",
    "                 NC | 26                                          55 | NC           ",
    "                    | 27                                          54 |              ",
    "                GND | 28                                          53 | VCC          ",
    "                 NC | 29                                          52 | NC           ",
    "                 NC | 30                                          51 | NC           ",
    "                     \\     32  34  36  38  40  42  44  46  48  50   /               ",
    "                      \\  31  33  35  37  39  41  43  45  47  49    /                ",
    "                       --------------------------------------------                 ",
    "                                   V       G V       G                              ",
    "                                   C       N C       N                              ",
    "                                   C       D C       D                              ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                              \t                     ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
};
static const unsigned char drawing_row_1504_pqfp100[63] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23,
    24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
    48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62,
};

/* 68 rows: block, macrocell, pin. */
static const FitPkgRow rows_1504_tqfp100[68] = {
    {1,1,14}, {1,2,13}, {1,3,12}, {1,4,10}, {1,5,9}, {1,6,8},
    {1,7,6}, {1,8,4}, {1,9,100}, {1,10,99}, {1,11,98}, {1,12,97},
    {1,13,96}, {1,14,94}, {1,15,93}, {1,16,92}, {2,17,37}, {2,18,36},
    {2,19,35}, {2,20,33}, {2,21,32}, {2,22,31}, {2,23,30}, {2,24,29},
    {2,25,25}, {2,26,23}, {2,27,21}, {2,28,20}, {2,29,19}, {2,30,17},
    {2,31,16}, {2,32,15}, {3,33,40}, {3,34,41}, {3,35,42}, {3,36,44},
    {3,37,45}, {3,38,46}, {3,39,47}, {3,40,48}, {3,41,52}, {3,42,54},
    {3,43,56}, {3,44,57}, {3,45,58}, {3,46,60}, {3,47,61}, {3,48,62},
    {4,49,63}, {4,50,64}, {4,51,65}, {4,52,67}, {4,53,68}, {4,54,69},
    {4,55,71}, {4,56,73}, {4,57,75}, {4,58,76}, {4,59,79}, {4,60,80},
    {4,61,81}, {4,62,83}, {4,63,84}, {4,64,85}, {0,0,90}, {0,0,89},
    {0,0,88}, {0,0,87},
};
/* the 21 pins that can drive a global OE */
static const int oe_pins_1504_tqfp100[21] = { 88, 90, 65, 48, 29, 12, 75, 98, 17, 60, 94, 25, 52, 83, 92, 85, 35, 68, 42, 37, 9 };
/* side and line of the label of each of the 100 pins */
static const FitPkgLabel labels_1504_tqfp100[100] = {
    {1,16}, {1,17}, {1,18}, {1,19}, {1,20}, {1,21}, {1,22}, {1,23}, {1,24}, {1,25},
    {1,26}, {1,27}, {1,28}, {1,29}, {1,30}, {1,31}, {1,32}, {1,33}, {1,34}, {1,35},
    {1,36}, {1,37}, {1,38}, {1,39}, {1,40}, {2,20}, {2,22}, {2,24}, {2,26}, {2,28},
    {2,30}, {2,32}, {2,34}, {2,36}, {2,38}, {2,40}, {2,42}, {2,44}, {2,46}, {2,48},
    {2,50}, {2,52}, {2,54}, {2,56}, {2,58}, {2,60}, {2,62}, {2,64}, {2,66}, {2,0},
    {0,40}, {0,39}, {0,38}, {0,37}, {0,36}, {0,35}, {0,34}, {0,33}, {0,32}, {0,31},
    {0,30}, {0,29}, {0,28}, {0,27}, {0,26}, {0,25}, {0,24}, {0,23}, {0,22}, {0,21},
    {0,20}, {0,19}, {0,18}, {0,17}, {0,16}, {3,68}, {3,66}, {3,64}, {3,62}, {3,60},
    {3,58}, {3,56}, {3,54}, {3,52}, {3,50}, {3,48}, {3,46}, {3,44}, {3,42}, {3,40},
    {3,38}, {3,36}, {3,34}, {3,32}, {3,30}, {3,28}, {3,26}, {3,24}, {3,22}, {3,20},
};
/* The 57 lines of the package drawing; dev_draw_package copies them into
 * rows of the drawing buffer (100 bytes per row) and ends 56 rows at
 * column 95. */
static const char *const drawing_1504_tqfp100[57] = {
    "                                                                                             ",
    "                                                                                             ",
    "                                                                                             ",
    "                                                                                             ",
    "                                                                                             ",
    "                                                                                             ",
    "                                                                                             ",
    "                                                                                             ",
    "                                                                                             ",
    "                                                                                             ",
    "                              G       V         G       V                                    ",
    "                              N       C         N       C       N N                          ",
    "                              D       C         D       C       C C                          ",
    "                  ------------------------------------------------------                     ",
    "                 / 100  98  96  94  92  90  88  86  84  82  80  78  76  \\                   ",
    "                /     99  97  95  93  91  89  87  85  83  81  79  77     \\                  ",
    "            NC | 1                                                     75 |                  ",
    "            NC | 2                                                     74 | GND              ",
    "           VCC | 3                                                     73 |                  ",
    "               | 4                                                     72 | NC               ",
    "            NC | 5                                                     71 |                  ",
    "               | 6                                                     70 | NC               ",
    "            NC | 7                                                     69 |                  ",
    "               | 8                                                     68 |                  ",
    "               | 9                                                     67 |                  ",
    "               | 10                                                    66 | VCC              ",
    "           GND | 11                                                    65 |                  ",
    "               | 12                     ATF1504                        64 |                  ",
    "               | 13                  100-Lead TQFP                     63 |                  ",
    "               | 14                                                    62 |                  ",
    "               | 15                                                    61 |                  ",
    "               | 16                                                    60 |                  ",
    "               | 17                                                    59 | GND              ",
    "           VCC | 18                                                    58 |                  ",
    "               | 19                                                    57 |                  ",
    "               | 20                                                    56 |                  ",
    "               | 21                                                    55 | NC               ",
    "            NC | 22                                                    54 |                  ",
    "               | 23                                                    53 | NC               ",
    "            NC | 24                                                    52 |                  ",
    "               | 25                                                    51 | VCC              ",
    "                \\     27  29  31  33  35  37  39  41  43  45   47  49    /                   ",
    "                 \\  26  28  30  32  34  36  38  40  42  44  46  48   50 /                    ",
    "                  ------------------------------------------------------                     ",
    "                    G N N           V       G V       G            N N                       ",
    "                    N C C           C       N C       N            C C                       ",
    "                    D               C       D C       D                                      ",
    "                                                                                             ",
    "                                                                                             ",
    "                                                                                             ",
    "                                                                                             ",
    "                                                                                             ",
    "                                                                                             ",
    "                                                                                             ",
    "                                                                                             ",
    "                                                                                             ",
    "                                                                                             ",
};
static const unsigned char drawing_row_1504_tqfp100[57] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23,
    24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
    48, 49, 50, 51, 52, 53, 54, 55, 56,
};

/* ATF1504 packages, with their special pins (global clocks, global clear,
 * JTAG, power down), test vector pin maps and pin limits. */
static const FitPackageDesc packages_1504[6] = {
    { FIT_PKG_PLCC44, "PLCC44", "P1504C44", 44, 36, rows_1504_plcc44,
      { 43, 2, 41 }, 1, oe_pins_1504_plcc44, 21, { 7, 13, 32, 38 }, { 11, 25 },
      "XXNXXXXXXNXXXXNXXXXXXNNXXXXXXNXXXXNXXXXXXNXX",
      drawing_1504_plcc44, drawing_row_1504_plcc44, 39, 65, 39, 100,
      { 0, 12, 54, 10, 0, 11, 27, 10 }, labels_1504_plcc44, NULL },
    { FIT_PKG_TQFP44, "TQFP44", "P1504T44", 44, 36, rows_1504_tqfp44,
      { 37, 40, 35 }, 39, oe_pins_1504_tqfp44, 21, { 1, 7, 26, 32 }, { 5, 19 },
      "XXXNXXXXNXXXXXXNNXXXXXXNXXXXNXXXXXXNXXXXNXXX",
      drawing_1504_tqfp44, drawing_row_1504_tqfp44, 39, 65, 39, 100,
      { 0, 12, 54, 10, 0, 11, 27, 10 }, labels_1504_tqfp44, NULL },
    { FIT_PKG_PLCC68, "PLCC68", "P1504C68", 68, 52, rows_1504_plcc68,
      { 67, 2, 65 }, 1, oe_pins_1504_plcc68, 21, { 12, 19, 50, 57 }, { 17, 37 },
      "XXNXXNXXXXNXXXXNXXXXNXXXXNXXXXNXXNNXXNXXXXNXXXXNXXXXNXXXXNXXXXNXXNXX",
      drawing_1504_plcc68, drawing_row_1504_plcc68, 49, 85, 48, 100,
      { 0, 16, 60, 10, 0, 12, 36, 10 }, labels_1504_plcc68, NULL },
    { FIT_PKG_PLCC84, "PLCC84", "P1504C84", 84, 68, rows_1504_plcc84,
      { 83, 2, 81 }, 1, oe_pins_1504_plcc84, 21, { 14, 23, 62, 71 }, { 20, 46 },
      "XXNXXXNXXXXXNXXXXXNXXXXXXNXXXXXNXXXXXNXXXNNXXXNXXXXXNXXXXXNXXXXXXNXXXXXNXXXXXNXXXNXX",
      drawing_1504_plcc84, drawing_row_1504_plcc84, 53, 85, 52, 100,
      { 0, 16, 68, 10, 0, 12, 40, 10 }, labels_1504_plcc84, NULL },
    { FIT_PKG_PQFP100, "PQFP100", "P1504Q100", 100, 68, rows_1504_pqfp100,
      { 89, 92, 87 }, 91, oe_pins_1504_pqfp100, 21, { 6, 17, 64, 75 }, { 14, 44 },
      "XXXXNXXXXXXXNXXXXXXNXXXXXXXNXXXXXXXNXXXNNXXXNXXXXXXXNXXXXXXXNXXXXXXNXXXXXXXNXXXXXXXNXXXNXXXXNXXXNXXX",
      drawing_1504_pqfp100, drawing_row_1504_pqfp100, 63, 82, 0, 100,
      { 0, 19, 71, 10, 0, 12, 49, 10 }, labels_1504_pqfp100, NULL },
    { FIT_PKG_TQFP100, "TQFP100", "P1504T100", 100, 68, rows_1504_tqfp100,
      { 87, 90, 85 }, 89, oe_pins_1504_tqfp100, 21, { 4, 15, 62, 73 }, { 12, 42 },
      "XXNXXXXXXXNXXXXXXNXXXXXXXNXXXXXXXNXXXNNXXXNXXXXXXXNXXXXXXXNXXXXXXNXXXXXXXNXXXXXXXNXXXNXXXXNXXXNXXXXX",
      drawing_1504_tqfp100, drawing_row_1504_tqfp100, 57, 95, 56, 100,
      { 0, 14, 76, 10, 0, 12, 44, 10 }, labels_1504_tqfp100, NULL },
};

/* ATF1504: vector pin maps for package names that are not packages of
 * this device (used only with the package name of another device). */
static const FitVectorMap vector_maps_1504[1] = {
    { NULL, NULL },
};

/* ================================================================ */
/* ATF1508 */
/* ================================================================ */

/* ATF1508: first fuse of the product terms of each of the 128 macrocells. */
static const int pt_addr_1508[128] = {
    7200, 6720, 6240, 5760, 5280, 4800, 4320, 3840, 3360, 2880, 2400, 1920, 1440, 960, 480, 0,
    14880, 14400, 13920, 13440, 12960, 12480, 12000, 11520, 11040, 10560, 10080, 9600, 9120, 8640, 8160, 7680,
    22560, 22080, 21600, 21120, 20640, 20160, 19680, 19200, 18720, 18240, 17760, 17280, 16800, 16320, 15840, 15360,
    30240, 29760, 29280, 28800, 28320, 27840, 27360, 26880, 26400, 25920, 25440, 24960, 24480, 24000, 23520, 23040,
    60960, 60480, 60000, 59520, 59040, 58560, 58080, 57600, 57120, 56640, 56160, 55680, 55200, 54720, 54240, 53760,
    53280, 52800, 52320, 51840, 51360, 50880, 50400, 49920, 49440, 48960, 48480, 48000, 47520, 47040, 46560, 46080,
    45600, 45120, 44640, 44160, 43680, 43200, 42720, 42240, 41760, 41280, 40800, 40320, 39840, 39360, 38880, 38400,
    37920, 37440, 36960, 36480, 36000, 35520, 35040, 34560, 34080, 33600, 33120, 32640, 32160, 31680, 31200, 30720,
};

/* ATF1508: fuse address of the UIM section of each block pair. */
static const int uim_addr_1508[4] = { 65280, 67440, 69600, 71760 };

/* ATF1508: the NOTE line jed_write_cfg_note writes after the configuration
 * fuses ending before each of these fuse addresses, 96 entries. */
static const FitJedecNote notes_1508[96] = {
    { 61456, "  NOTE S16,S12 of block A * \n" },
    { 61488, "  NOTE S14,S11 of block A * \n" },
    { 61536, "  NOTE S16,S12 of block B * \n" },
    { 61568, "  NOTE S14,S11 of block B * \n" },
    { 61616, "  NOTE S16,S12 of block C * \n" },
    { 61648, "  NOTE S14,S11 of block C * \n" },
    { 61696, "  NOTE S16,S12 of block D * \n" },
    { 61728, "  NOTE S14,S11 of block D * \n" },
    { 61776, "  NOTE S9 ,S6  of block A * \n" },
    { 61808, "  NOTE S13,S10 of block A * \n" },
    { 61856, "  NOTE S9 ,S6  of block B * \n" },
    { 61888, "  NOTE S13,S10 of block B * \n" },
    { 61936, "  NOTE S9 ,S6  of block C * \n" },
    { 61968, "  NOTE S13,S10 of block C * \n" },
    { 62016, "  NOTE S9 ,S6  of block D * \n" },
    { 62048, "  NOTE S13,S10 of block D * \n" },
    { 62096, "  NOTE S20,S18 of block A * \n" },
    { 62128, "  NOTE S8 ,S21 of block A * \n" },
    { 62176, "  NOTE S20,S18 of block B * \n" },
    { 62208, "  NOTE S8 ,S21 of block B * \n" },
    { 62256, "  NOTE S20,S18 of block C * \n" },
    { 62288, "  NOTE S8 ,S21 of block C * \n" },
    { 62336, "  NOTE S20,S18 of block D * \n" },
    { 62368, "  NOTE S8 ,S21 of block D * \n" },
    { 62416, "  NOTE S7 ,S19 of block A * \n" },
    { 62448, "  NOTE S22,S5  of block A * \n" },
    { 62496, "  NOTE S7 ,S19 of block B * \n" },
    { 62528, "  NOTE S22,S5  of block B * \n" },
    { 62576, "  NOTE S7 ,S19 of block C * \n" },
    { 62608, "  NOTE S22,S5  of block C * \n" },
    { 62656, "  NOTE S7 ,S19 of block D * \n" },
    { 62688, "  NOTE S22,S5  of block D * \n" },
    { 62736, "  NOTE S23,S4  of block A * \n" },
    { 62768, "  NOTE S3 ,S15 of block A * \n" },
    { 62816, "  NOTE S23,S4  of block B * \n" },
    { 62848, "  NOTE S3 ,S15 of block B * \n" },
    { 62896, "  NOTE S23,S4  of block C * \n" },
    { 62928, "  NOTE S3 ,S15 of block C * \n" },
    { 62976, "  NOTE S23,S4  of block D * \n" },
    { 63008, "  NOTE S3 ,S15 of block D * \n" },
    { 63056, "  NOTE S0 ,S1  of block A * \n" },
    { 63088, "  NOTE S17,S2  of block A * \n" },
    { 63136, "  NOTE S0 ,S1  of block B * \n" },
    { 63168, "  NOTE S17,S2  of block B * \n" },
    { 63216, "  NOTE S0 ,S1  of block C * \n" },
    { 63248, "  NOTE S17,S2  of block C * \n" },
    { 63296, "  NOTE S0 ,S1  of block D * \n" },
    { 63328, "  NOTE S17,S2  of block D * \n" },
    { 63376, "  NOTE S16,S12 of block H * \n" },
    { 63408, "  NOTE S14,S11 of block H * \n" },
    { 63456, "  NOTE S16,S12 of block G * \n" },
    { 63488, "  NOTE S14,S11 of block G * \n" },
    { 63536, "  NOTE S16,S12 of block F * \n" },
    { 63568, "  NOTE S14,S11 of block F * \n" },
    { 63616, "  NOTE S16,S12 of block E * \n" },
    { 63648, "  NOTE S14,S11 of block E * \n" },
    { 63696, "  NOTE S9 ,S6  of block H * \n" },
    { 63728, "  NOTE S13,S10 of block H * \n" },
    { 63776, "  NOTE S9 ,S6  of block G * \n" },
    { 63808, "  NOTE S13,S10 of block G * \n" },
    { 63856, "  NOTE S9 ,S6  of block F * \n" },
    { 63888, "  NOTE S13,S10 of block F * \n" },
    { 63936, "  NOTE S9 ,S6  of block E * \n" },
    { 63968, "  NOTE S13,S10 of block E * \n" },
    { 64016, "  NOTE S20,S18 of block H * \n" },
    { 64048, "  NOTE S8 ,S21 of block H * \n" },
    { 64096, "  NOTE S20,S18 of block G * \n" },
    { 64128, "  NOTE S8 ,S21 of block G * \n" },
    { 64176, "  NOTE S20,S18 of block F * \n" },
    { 64208, "  NOTE S8 ,S21 of block F * \n" },
    { 64256, "  NOTE S20,S18 of block E * \n" },
    { 64288, "  NOTE S8 ,S21 of block E * \n" },
    { 64336, "  NOTE S7 ,S19 of block H * \n" },
    { 64368, "  NOTE S22,S5  of block H * \n" },
    { 64416, "  NOTE S7 ,S19 of block G * \n" },
    { 64448, "  NOTE S22,S5  of block G * \n" },
    { 64496, "  NOTE S7 ,S19 of block F * \n" },
    { 64528, "  NOTE S22,S5  of block F * \n" },
    { 64576, "  NOTE S7 ,S19 of block E * \n" },
    { 64608, "  NOTE S22,S5  of block E * \n" },
    { 64656, "  NOTE S23,S4  of block H * \n" },
    { 64688, "  NOTE S3 ,S15 of block H * \n" },
    { 64736, "  NOTE S23,S4  of block G * \n" },
    { 64768, "  NOTE S3 ,S15 of block G * \n" },
    { 64816, "  NOTE S23,S4  of block F * \n" },
    { 64848, "  NOTE S3 ,S15 of block F * \n" },
    { 64896, "  NOTE S23,S4  of block E * \n" },
    { 64928, "  NOTE S3 ,S15 of block E * \n" },
    { 64976, "  NOTE S0 ,S1  of block H * \n" },
    { 65008, "  NOTE S17,S2  of block H * \n" },
    { 65056, "  NOTE S0 ,S1  of block G * \n" },
    { 65088, "  NOTE S17,S2  of block G * \n" },
    { 65136, "  NOTE S0 ,S1  of block F * \n" },
    { 65168, "  NOTE S17,S2  of block F * \n" },
    { 65216, "  NOTE S0 ,S1  of block E * \n" },
    { 65248, "  NOTE S17,S2  of block E * \n" },
};

/* ATF1508: part name, QF, macrocell configuration length and first fuse,
 * global OE, configuration, JTAG, UES and reserved bits addresses, product
 * term and UIM addresses, the two configuration half addresses, the NOTE
 * lines. */
static const FitJedecLayout jedec_1508 = {
    "ATF1508AS", 74136, 3840, 61440, 73920, 74082, 74114, 74118, 74134,
    pt_addr_1508, { 61440, 63360 }, uim_addr_1508, notes_1508, 96
};

static const FitJedecLayout jedec_1508_be = {
    "ATF1508BE", 74146, 3840, 61440, 73920, 74082, 74114, 74122, 74138,
    pt_addr_1508, { 61440, 63360 }, uim_addr_1508, NULL, 0
};

/* ATF1508, 40 x 27 node names: nodes selectable by each block input multiplexer. */
static const char *const uim_src_1508[1080] = {
    "B30p", "B23fb", "C43p", "C36fb", "E77p", "D56fb", "E71fb", "F92p", "F84fb", "A9p", "H113fb", "H124p", "B18fb", "B27p", "C46p", "C40fb", "E73p", "D52fb", "E67fb", "F96p", "F88fb", "A13p", "H117fb", "H128p", "A3fb", "A7fb", "B19fb",
    "G99p", "G106fb", "F86p", "F93fb", "D52p", "E73fb", "D58fb", "C37p", "C45fb", "H120p", "A16fb", "A5p", "G111fb", "G102p", "F83p", "F89fb", "D56p", "E77fb", "D62fb", "C33p", "C41fb", "H116p", "A12fb", "A1p", "H126fb", "H122fb", "G110fb",
    "GCLR", "B24fb", "C41p", "D54fb", "E70fb", "F89p", "F81fb", "F92p", "G103fb", "A12p", "H121p", "H113fb", "B27p", "B20fb", "C38fb", "C48p", "D52fb", "E76p", "E68fb", "G105p", "C35fb", "G98fb", "H118fb", "GND", "A1fb", "A4fb", "A5fb",
    "OE1", "G105fb", "F88p", "E75fb", "D59fb", "C40p", "C48fb", "C37p", "B26fb", "H117p", "A8p", "A16fb", "G102p", "G109fb", "F91fb", "F81p", "E77fb", "D53p", "D61fb", "B24p", "F94fb", "B31fb", "A11fb", "GND", "H128fb", "H125fb", "H124fb",
    "B30p", "B24fb", "C34fb", "C44p", "D56fb", "E80p", "E72fb", "G109p", "F84fb", "G102fb", "H114fb", "OE2", "B20fb", "C45p", "D57p", "D50fb", "D51fb", "F93p", "F96p", "F85fb", "G99fb", "A16p", "H125p", "H117fb", "A1fb", "A8fb", "E66fb",
    "G99p", "G105fb", "F95fb", "F85p", "E73fb", "D49p", "D57fb", "B20p", "C45fb", "B27fb", "A15fb", "GCLK", "G109fb", "F84p", "E72p", "E79fb", "E78fb", "C36p", "C33p", "C44fb", "B30fb", "H113p", "A4p", "A12fb", "H128fb", "H121fb", "D63fb",
    "B32p", "C33fb", "D53fb", "D64p", "E69fb", "F91p", "G109p", "F83fb", "A11p", "G104fb", "H121p", "H114fb", "B18fb", "B28p", "D57p", "C40fb", "E75p", "E65fb", "F85fb", "G107p", "G97fb", "A13p", "H126p", "H119fb", "A3fb", "A8fb", "A5fb",
    "G97p", "F96fb", "E76fb", "E65p", "D60fb", "C38p", "B20p", "C46fb", "H118p", "B25fb", "A8p", "A15fb", "G111fb", "G101p", "E72p", "F89fb", "D54p", "D64fb", "C44fb", "B22p", "B32fb", "H116p", "A3p", "A10fb", "H126fb", "H121fb", "H124fb",
    "B22fb", "B32p", "D61p", "C36fb", "E78p", "E69fb", "F81fb", "G110p", "G101fb", "A9p", "H123p", "H115fb", "B28p", "C37fb", "D49fb", "D60p", "E65fb", "F94p", "G105p", "F87fb", "A14p", "G100fb", "H125p", "H118fb", "A1fb", "A7fb", "E66fb",
    "G107fb", "G97p", "E68p", "F93fb", "D51p", "D60fb", "C48fb", "B19p", "B28fb", "H120p", "A6p", "A14fb", "G101p", "F92fb", "E80fb", "E69p", "D64fb", "C35p", "B24p", "C42fb", "H115p", "B29fb", "A4p", "A11fb", "H128fb", "H122fb", "D63fb",
    "C43p", "C34fb", "D62p", "D53fb", "F89p", "E71fb", "F82fb", "G110p", "A11p", "G103fb", "H113fb", "H115fb", "C48p", "C39fb", "D60p", "D50fb", "E75p", "E68fb", "G108p", "F87fb", "G97fb", "A16p", "H128p", "H120fb", "B21fb", "A2fb", "B17fb",
    "F86p", "F95fb", "E67p", "E76fb", "C40p", "D58fb", "C47fb", "B19p", "H118p", "B26fb", "A16fb", "A14fb", "F81p", "F90fb", "E69p", "E79fb", "D54p", "D61fb", "B21p", "C42fb", "B32fb", "H113p", "A1p", "A9fb", "G108fb", "H127fb", "G112fb",
    "B24fb", "C44p", "D64p", "D54fb", "E78p", "E72fb", "F83fb", "G112p", "G101fb", "A12p", "H124p", "H116fb", "C46p", "C38fb", "D59p", "D49fb", "F93p", "E67fb", "G107p", "F86fb", "C35fb", "A14p", "G99fb", "H119fb", "B21fb", "A6fb", "B17fb",
    "G105fb", "F85p", "E65p", "E75fb", "D51p", "D57fb", "C46fb", "B17p", "B28fb", "H117p", "A5p", "A13fb", "F83p", "F91fb", "E70p", "E80fb", "C36p", "D62fb", "B22p", "C43fb", "F94fb", "H115p", "B30fb", "A10fb", "G108fb", "H123fb", "G112fb",
    "GCLR", "B23fb", "C33fb", "D62p", "B29p", "E80p", "F82fb", "G112p", "D55fb", "G102fb", "H123p", "H116fb", "B25p", "B18fb", "C45p", "C39fb", "E73p", "D51fb", "F94p", "F87fb", "F88fb", "G100fb", "GND", "H120fb", "A4fb", "A6fb", "E66fb",
    "OE1", "G106fb", "F96fb", "E67p", "G100p", "D49p", "C47fb", "B17p", "E74fb", "B27fb", "A6p", "A13fb", "G104p", "G111fb", "F84p", "F90fb", "D56p", "E78fb", "C35p", "C42fb", "C41fb", "B29fb", "GND", "A9fb", "H125fb", "H123fb", "D63fb",
    "B22fb", "C41p", "GND", "D54fb", "B29p", "E77p", "E70fb", "F91p", "D55fb", "F84fb", "G104fb", "OE2", "B25p", "C37fb", "D59p", "D50fb", "D51fb", "E76p", "F86fb", "G108p", "C35fb", "G98fb", "H126p", "H120fb", "A2fb", "A8fb", "B19fb",
    "G107fb", "F88p", "GND", "E75fb", "G100p", "D52p", "D59fb", "C38p", "E74fb", "C45fb", "B25fb", "GCLK", "G104p", "F92fb", "E70p", "E79fb", "E78fb", "D53p", "C43fb", "B21p", "F94fb", "B31fb", "A3p", "A9fb", "H127fb", "H121fb", "G110fb",
    "B30p", "B23fb", "C43p", "C36fb", "E77p", "D56fb", "E71fb", "F92p", "F84fb", "A9p", "H113fb", "H124p", "B20fb", "C45p", "D57p", "D50fb", "D52fb", "F93p", "F96p", "F85fb", "G99fb", "A16p", "H125p", "H117fb", "A3fb", "A8fb", "E66fb",
    "G99p", "G106fb", "F86p", "F93fb", "D52p", "E73fb", "D58fb", "C37p", "C45fb", "H120p", "A16fb", "A5p", "G109fb", "F84p", "E72p", "E79fb", "E77fb", "C36p", "C33p", "C44fb", "B30fb", "H113p", "A4p", "A12fb", "H126fb", "H121fb", "D63fb",
    "B24fb", "C41p", "D61p", "D54fb", "E70fb", "F89p", "F81fb", "F92p", "G103fb", "A12p", "H121p", "H113fb", "B27p", "B20fb", "C46p", "C40fb", "E73p", "D52fb", "E67fb", "F96p", "F88fb", "A13p", "H117fb", "H128p", "A4fb", "A7fb", "B19fb",
    "G105fb", "F88p", "E68p", "E75fb", "D59fb", "C40p", "C48fb", "C37p", "B26fb", "H117p", "A8p", "A16fb", "G102p", "G109fb", "F83p", "F89fb", "D56p", "E77fb", "D62fb", "C33p", "C41fb", "H116p", "A12fb", "A1p", "H125fb", "H122fb", "G110fb",
    "B30p", "B24fb", "C34fb", "C44p", "D56fb", "E80p", "E72fb", "G109p", "G102fb", "G103fb", "H114fb", "OE2", "B28p", "C37fb", "D49fb", "D60p", "E65fb", "F94p", "G105p", "F87fb", "A14p", "G100fb", "H125p", "H118fb", "A1fb", "A7fb", "B19fb",
    "G99p", "G105fb", "F95fb", "F85p", "E73fb", "D49p", "D57fb", "B20p", "B27fb", "B26fb", "A15fb", "GCLK", "G101p", "F92fb", "E80fb", "E69p", "D64fb", "C35p", "B24p", "C42fb", "H115p", "B29fb", "A4p", "A11fb", "H128fb", "H122fb", "G110fb",
    "GCLR", "B32p", "C33fb", "D53fb", "E69fb", "F91p", "G109p", "F83fb", "A11p", "G104fb", "H121p", "H114fb", "B27p", "B20fb", "C38fb", "C48p", "D52fb", "E76p", "E68fb", "G105p", "E88fb", "G98fb", "H118fb", "GND", "A3fb", "A7fb", "A5fb",
    "OE1", "G97p", "F96fb", "E76fb", "D60fb", "C38p", "B20p", "C46fb", "H118p", "B25fb", "A8p", "A15fb", "G102p", "G109fb", "F91fb", "F81p", "E77fb", "D53p", "D61fb", "B24p", "C41fb", "B31fb", "A11fb", "GND", "H126fb", "H122fb", "H124fb",
    "B22fb", "B32p", "D61p", "C36fb", "E78p", "E69fb", "F81fb", "G110p", "G101fb", "A9p", "H123p", "H115fb", "C46p", "C38fb", "D59p", "D49fb", "F93p", "E67fb", "G107p", "F86fb", "A14p", "G99fb", "H118fb", "H119fb", "A1fb", "A6fb", "B17fb",
    "G107fb", "G97p", "E68p", "F93fb", "D51p", "D60fb", "C48fb", "B19p", "B28fb", "H120p", "A6p", "A14fb", "F83p", "F91fb", "E70p", "E80fb", "C36p", "D62fb", "B22p", "C43fb", "H115p", "B30fb", "A11fb", "A10fb", "H128fb", "H123fb", "G112fb",
    "C43p", "C34fb", "D62p", "D53fb", "F89p", "E71fb", "F82fb", "G110p", "A11p", "G103fb", "GND", "H115fb", "B18fb", "B28p", "D57p", "C40fb", "E75p", "E65fb", "F85fb", "G107p", "G97fb", "A13p", "H126p", "H119fb", "B21fb", "A2fb", "A5fb",
    "F86p", "F95fb", "E67p", "E76fb", "C40p", "D58fb", "C47fb", "B19p", "H118p", "B26fb", "GND", "A14fb", "G111fb", "G101p", "E72p", "F89fb", "D54p", "D64fb", "C44fb", "B22p", "B32fb", "H116p", "A3p", "A10fb", "G108fb", "H127fb", "H124fb",
    "B22fb", "C44p", "D64p", "D54fb", "E78p", "E72fb", "F83fb", "G112p", "G101fb", "A12p", "H124p", "H116fb", "B25p", "C37fb", "D59p", "D49fb", "D51fb", "E76p", "F86fb", "G108p", "C35fb", "G98fb", "H126p", "H120fb", "B21fb", "A8fb", "B19fb",
    "G107fb", "F85p", "E65p", "E75fb", "D51p", "D57fb", "C46fb", "B17p", "B28fb", "H117p", "A5p", "A13fb", "G104p", "F92fb", "E70p", "E80fb", "E78fb", "D53p", "C43fb", "B21p", "F94fb", "B31fb", "A3p", "A9fb", "G108fb", "H121fb", "G110fb",
    "B23fb", "C33fb", "D62p", "GND", "B29p", "E80p", "F82fb", "G112p", "D55fb", "G102fb", "H123p", "H116fb", "C48p", "C39fb", "D60p", "D50fb", "E75p", "E68fb", "G108p", "F87fb", "G97fb", "A16p", "H128p", "H120fb", "A4fb", "A6fb", "B17fb",
    "G106fb", "F96fb", "E67p", "GND", "G100p", "D49p", "C47fb", "B17p", "E74fb", "B27fb", "A6p", "A13fb", "F81p", "F90fb", "E69p", "E79fb", "D54p", "D61fb", "B21p", "C42fb", "B32fb", "H113p", "A1p", "A9fb", "H125fb", "H123fb", "G112fb",
    "GCLR", "B22fb", "C41p", "D53fb", "B29p", "E77p", "E70fb", "F91p", "D55fb", "F84fb", "G104fb", "OE2", "B25p", "B18fb", "C45p", "C39fb", "E73p", "D51fb", "F94p", "F86fb", "C35fb", "F88fb", "G100fb", "GND", "A2fb", "A6fb", "E66fb",
    "OE1", "G107fb", "F88p", "E76fb", "G100p", "D52p", "D59fb", "C38p", "E74fb", "C45fb", "B25fb", "GCLK", "G104p", "G111fb", "F84p", "F90fb", "D56p", "E78fb", "C35p", "C43fb", "F94fb", "C41fb", "B29fb", "GND", "H127fb", "H123fb", "D63fb",
    "GCLR", "C34fb", "D61p", "C36fb", "D56fb", "E70fb", "E72fb", "F82fb", "G101fb", "G104fb", "H114fb", "H116fb", "GND", "C37fb", "C39fb", "GND", "GND", "E65fb", "E68fb", "GND", "G97fb", "G99fb", "H117fb", "GND", "B21fb", "A3fb", "B17fb",
    "OE1", "F95fb", "E68p", "F93fb", "E73fb", "D59fb", "D57fb", "C47fb", "B28fb", "B25fb", "A15fb", "A13fb", "GND", "F92fb", "F90fb", "GND", "GND", "D64fb", "D61fb", "GND", "B32fb", "B30fb", "A12fb", "GND", "G108fb", "H126fb", "G112fb",
    "B23fb", "C33fb", "GND", "D64p", "E69fb", "E71fb", "F81fb", "F83fb", "D55fb", "G102fb", "H115fb", "OE2", "GND", "C38fb", "C40fb", "GND", "GND", "E67fb", "F85fb", "GND", "G98fb", "G100fb", "H119fb", "GND", "A2fb", "A4fb", "A5fb",
    "G106fb", "F96fb", "GND", "E65p", "D60fb", "D58fb", "C48fb", "C46fb", "E74fb", "B27fb", "A14fb", "GCLK", "GND", "F91fb", "F89fb", "GND", "GND", "D62fb", "C44fb", "GND", "B31fb", "B29fb", "A10fb", "GND", "H127fb", "H125fb", "H124fb",
};

/* ATF1508, 6 x 27 node names: nodes selectable by each global OE multiplexer. */
static const char *const oe_src_1508[162] = {
    "G112fb", "H123fb", "XXXX", "XXXX", "A11fb", "GND", "B32fb", "C43fb", "B24p", "D64fb", "D56p", "E80fb", "E72p", "F91fb", "G104p", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX", "OE1",
    "B19fb", "A8fb", "GND", "H120fb", "H125p", "G99fb", "A13p", "F88fb", "E67fb", "F93p", "XXXX", "D51fb", "C40fb", "C45p", "B27p", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX",
    "XXXX", "GND", "A3fb", "XXXX", "XXXX", "XXXX", "C35fb", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX", "H115fb", "G104fb", "A11p", "F83fb", "G109p", "E72fb", "D56fb", "E77p", "B29p", "D61p", "B24fb", "XXXX",
    "XXXX", "A3fb", "B21fb", "XXXX", "XXXX", "XXXX", "C35fb", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX", "OE2", "H115fb", "GND", "G101fb", "G112p", "F83fb", "E80p", "E69fb", "D64p", "D53fb", "B32p", "XXXX",
    "B19fb", "A5fb", "GND", "H128p", "H117fb", "A16p", "G99fb", "F85fb", "F96p", "E67fb", "D51fb", "D59p", "C48p", "C37fb", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX",
    "XXXX", "XXXX", "H128fb", "GND", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX", "XXXX", "A16fb", "A8p", "H120p", "B27fb", "C48fb", "C40p", "D59fb", "E75fb", "F88p", "F96fb", "G99p", "G107fb",
};

/* 132 rows: block, macrocell, pin. */
static const FitPkgRow rows_1508_plcc68[132] = {
    {1,1,0}, {1,2,0}, {1,3,10}, {1,4,0}, {1,5,9}, {1,6,0},
    {1,7,0}, {1,8,8}, {1,9,0}, {1,10,0}, {1,11,7}, {1,12,0},
    {1,13,5}, {1,14,0}, {1,15,0}, {1,16,4}, {2,17,0}, {2,18,0},
    {2,19,18}, {2,20,0}, {2,21,17}, {2,22,0}, {2,23,0}, {2,24,15},
    {2,25,0}, {2,26,0}, {2,27,14}, {2,28,0}, {2,29,13}, {2,30,0},
    {2,31,0}, {2,32,12}, {3,33,0}, {3,34,0}, {3,35,25}, {3,36,0},
    {3,37,24}, {3,38,0}, {3,39,0}, {3,40,23}, {3,41,0}, {3,42,0},
    {3,43,22}, {3,44,0}, {3,45,20}, {3,46,0}, {3,47,0}, {3,48,19},
    {4,49,0}, {4,50,0}, {4,51,33}, {4,52,0}, {4,53,32}, {4,54,0},
    {4,55,0}, {4,56,30}, {4,57,0}, {4,58,0}, {4,59,29}, {4,60,0},
    {4,61,28}, {4,62,0}, {4,63,0}, {4,64,27}, {5,65,0}, {5,66,0},
    {5,67,36}, {5,68,0}, {5,69,37}, {5,70,0}, {5,71,0}, {5,72,39},
    {5,73,0}, {5,74,0}, {5,75,40}, {5,76,0}, {5,77,41}, {5,78,0},
    {5,79,0}, {5,80,42}, {6,81,0}, {6,82,0}, {6,83,44}, {6,84,0},
    {6,85,45}, {6,86,0}, {6,87,0}, {6,88,46}, {6,89,0}, {6,90,0},
    {6,91,47}, {6,92,0}, {6,93,49}, {6,94,0}, {6,95,0}, {6,96,50},
    {7,97,0}, {7,98,0}, {7,99,51}, {7,100,0}, {7,101,52}, {7,102,0},
    {7,103,0}, {7,104,54}, {7,105,0}, {7,106,0}, {7,107,55}, {7,108,0},
    {7,109,56}, {7,110,0}, {7,111,0}, {7,112,57}, {8,113,0}, {8,114,0},
    {8,115,59}, {8,116,0}, {8,117,60}, {8,118,0}, {8,119,0}, {8,120,61},
    {8,121,0}, {8,122,0}, {8,123,62}, {8,124,0}, {8,125,64}, {8,126,0},
    {8,127,0}, {8,128,65}, {0,0,2}, {0,0,1}, {0,0,84}, {0,0,83},
};
/* the 26 pins that can drive a global OE */
static const int oe_pins_1508_plcc68[26] = { 68, 2, 15, 30, 39, 54, 64, 5, 49, 20, 14, 7, 56, 41, 13, 28, 57, 42, 27, 12, 65, 4, 50, 29, 19, 8 };
/* side and line of the label of each of the 68 pins */
static const FitPkgLabel labels_1508_plcc68[68] = {
    {3,37}, {3,35}, {3,33}, {3,31}, {3,29}, {3,27}, {3,25}, {3,23}, {3,21}, {1,16},
    {1,17}, {1,18}, {1,19}, {1,20}, {1,21}, {1,22}, {1,23}, {1,24}, {1,25}, {1,26},
    {1,27}, {1,28}, {1,29}, {1,30}, {1,31}, {1,32}, {2,22}, {2,24}, {2,26}, {2,28},
    {2,30}, {2,32}, {2,34}, {2,36}, {2,38}, {2,40}, {2,42}, {2,44}, {2,46}, {2,48},
    {2,50}, {2,52}, {2,54}, {0,32}, {0,31}, {0,30}, {0,29}, {0,28}, {0,27}, {0,26},
    {0,25}, {0,24}, {0,23}, {0,22}, {0,21}, {0,20}, {0,19}, {0,18}, {0,17}, {0,16},
    {3,53}, {3,51}, {3,49}, {3,47}, {3,45}, {3,43}, {3,41}, {3,39},
};
/* The 49 lines of the package drawing; dev_draw_package copies them into
 * rows of the drawing buffer (160 bytes per row) and ends 48 rows at
 * column 85. */
static const char *const drawing_1508_plcc68[49] = {
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                           G     V         G     V              \t             ",
    "                           N     C         N     C           \t                     ",
    "                           D     C         D     C           \t                     ",
    "                    ------------------------------------       \t             ",
    "                   / 9   7   5   3   1   67  65  63  61 \\   \t                     ",
    "                  /    8   6   4   2   68  66  64  62    \\              \t     ",
    "                 | 10               (*)                60 |    \t             ",
    "             VCC | 11                                  59 |          \t             ",
    "                 | 12                                  58 | GND \t             ",
    "                 | 13                                  57 |         \t             ",
    "                 | 14                                  56 |          \t             ",
    "                 | 15                                  55 |          \t             ",
    "             GND | 16                                  54 |          \t             ",
    "                 | 17            ATF1508               53 | VCC         \t     ",
    "                 | 18          68-Lead PLCC            52 |  \t                     ",
    "                 | 19                                  51 |          \t             ",
    "                 | 20                                  50 |          \t             ",
    "             VCC | 21                                  49 |          \t             ",
    "                 | 22                                  48 | GND         \t     ",
    "                 | 23                                  47 |          \t             ",
    "                 | 24                                  46 |          \t             ",
    "                 | 25                                  45 |  \t                     ",
    "             GND | 26                                  44 |          \t             ",
    "                  \\     28  30  32  34  36  38  40  42   / \t                     ",
    "                   \\  27  29  31  33  35  37  39  41  43/  \t                     ",
    "                    ------------------------------------\t                     ",
    "                              V     G V     G         V \t                     ",
    "                              C     N C     N         C \t                     ",
    "                              C     D C     D         C  \t                     ",
    "                                                                                   ",
    "                                                                                   ",
    "                                                                                   ",
    "                                                                                   ",
    "                                                                                   ",
    "                                                                                   ",
    "                                                                                   ",
    "                                                                                   ",
    "                                                                                   ",
    "                                                                                   ",
};
static const unsigned char drawing_row_1508_plcc68[49] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23,
    24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
    48,
};

/* 132 rows: block, macrocell, pin. */
static const FitPkgRow rows_1508_plcc84[132] = {
    {1,1,0}, {1,2,0}, {1,3,12}, {1,4,0}, {1,5,11}, {1,6,10},
    {1,7,0}, {1,8,9}, {1,9,0}, {1,10,0}, {1,11,8}, {1,12,0},
    {1,13,6}, {1,14,5}, {1,15,0}, {1,16,4}, {2,17,22}, {2,18,0},
    {2,19,21}, {2,20,0}, {2,21,20}, {2,22,0}, {2,23,0}, {2,24,18},
    {2,25,17}, {2,26,0}, {2,27,16}, {2,28,0}, {2,29,15}, {2,30,0},
    {2,31,0}, {2,32,14}, {3,33,0}, {3,34,0}, {3,35,31}, {3,36,0},
    {3,37,30}, {3,38,29}, {3,39,0}, {3,40,28}, {3,41,0}, {3,42,0},
    {3,43,27}, {3,44,0}, {3,45,25}, {3,46,24}, {3,47,0}, {3,48,23},
    {4,49,41}, {4,50,0}, {4,51,40}, {4,52,0}, {4,53,39}, {4,54,0},
    {4,55,0}, {4,56,37}, {4,57,36}, {4,58,0}, {4,59,35}, {4,60,0},
    {4,61,34}, {4,62,0}, {4,63,0}, {4,64,33}, {5,65,44}, {5,66,0},
    {5,67,45}, {5,68,0}, {5,69,46}, {5,70,0}, {5,71,0}, {5,72,48},
    {5,73,49}, {5,74,0}, {5,75,50}, {5,76,0}, {5,77,51}, {5,78,0},
    {5,79,0}, {5,80,52}, {6,81,0}, {6,82,0}, {6,83,54}, {6,84,0},
    {6,85,55}, {6,86,56}, {6,87,0}, {6,88,57}, {6,89,0}, {6,90,0},
    {6,91,58}, {6,92,0}, {6,93,60}, {6,94,61}, {6,95,0}, {6,96,62},
    {7,97,63}, {7,98,0}, {7,99,64}, {7,100,0}, {7,101,65}, {7,102,0},
    {7,103,0}, {7,104,67}, {7,105,68}, {7,106,0}, {7,107,69}, {7,108,0},
    {7,109,70}, {7,110,0}, {7,111,0}, {7,112,71}, {8,113,0}, {8,114,0},
    {8,115,73}, {8,116,0}, {8,117,74}, {8,118,75}, {8,119,0}, {8,120,76},
    {8,121,0}, {8,122,0}, {8,123,77}, {8,124,0}, {8,125,79}, {8,126,80},
    {8,127,0}, {8,128,81}, {0,0,2}, {0,0,1}, {0,0,84}, {0,0,83},
};
/* the 30 pins that can drive a global OE */
static const int oe_pins_1508_plcc84[30] = { 84, 2, 18, 37, 48, 67, 79, 6, 60, 25, 16, 8, 70, 51, 15, 34, 71, 52, 33, 14, 81, 4, 62, 35, 23, 9, 75, 29, 56, 63 };
/* side and line of the label of each of the 84 pins */
static const FitPkgLabel labels_1508_plcc84[84] = {
    {3,42}, {3,40}, {3,38}, {3,36}, {3,34}, {3,32}, {3,30}, {3,28}, {3,26}, {3,23},
    {3,21}, {1,16}, {1,17}, {1,18}, {1,19}, {1,20}, {1,21}, {1,22}, {1,23}, {1,24},
    {1,25}, {1,26}, {1,27}, {1,28}, {1,29}, {1,30}, {1,31}, {1,32}, {1,33}, {1,34},
    {1,35}, {1,36}, {2,22}, {2,24}, {2,26}, {2,28}, {2,30}, {2,32}, {2,34}, {2,36},
    {2,38}, {2,40}, {2,42}, {2,44}, {2,46}, {2,48}, {2,50}, {2,52}, {2,54}, {2,56},
    {2,58}, {2,60}, {2,62}, {0,36}, {0,35}, {0,34}, {0,33}, {0,32}, {0,31}, {0,30},
    {0,29}, {0,28}, {0,27}, {0,26}, {0,25}, {0,24}, {0,23}, {0,22}, {0,21}, {0,20},
    {0,19}, {0,18}, {0,17}, {0,16}, {3,61}, {3,59}, {3,57}, {3,55}, {3,53}, {3,51},
    {3,49}, {3,47}, {3,45}, {3,43},
};
/* The 53 lines of the package drawing; dev_draw_package copies them into
 * rows of the drawing buffer (160 bytes per row) and ends 53 rows at
 * column 85. */
static const char *const drawing_1508_plcc84[53] = {
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                              G       V        G       V                            ",
    "                              N       C        N       C                            ",
    "                              D       C        D       C                            ",
    "                    -------------------------------------------                     ",
    "                   / 11   9   7   5   3   1  83  81  79  77  75 \\                  ",
    "                  /    10   8   6   4   2  84  82  80  78  76    \\                 ",
    "                 | 12                    (*)                   74 |                 ",
    "             VCC | 13                                          73 |                 ",
    "                 | 14                                          72 | GND             ",
    "                 | 15                                          71 |                 ",
    "                 | 16                                          70 |                 ",
    "                 | 17                                          69 |                 ",
    "                 | 18                                          68 |                 ",
    "             GND | 19                                          67 |                 ",
    "                 | 20                                          66 | VCC             ",
    "                 | 21                                          65 |                 ",
    "                 | 22                 ATF1508                  64 |                 ",
    "                 | 23               84-Lead PLCC               63 |                 ",
    "                 | 24                                          62 |                 ",
    "                 | 25                                          61 |                 ",
    "             VCC | 26                                          60 |                 ",
    "                 | 27                                          59 | GND             ",
    "                 | 28                                          58 |                 ",
    "                 | 29                                          57 |                 ",
    "                 | 30                                          56 |                 ",
    "                 | 31                                          55 |                 ",
    "             GND | 32                                          54 |                 ",
    "                  \\     34  36  38  40  42  44  46  48  50  52   /                 ",
    "                   \\  33  35  37  39  41  43  45  47  49  51  53/                  ",
    "              \t    --------------------------------------------                     ",
    "                                V       G V       G           V                     ",
    "                                C       N C       N           C                     ",
    "                                C       D C       D           C                     ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
};
static const unsigned char drawing_row_1508_plcc84[53] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23,
    24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
    48, 49, 50, 51, 52,
};

/* 132 rows: block, macrocell, pin. */
static const FitPkgRow rows_1508_pqfp100[132] = {
    {1,1,4}, {1,2,0}, {1,3,3}, {1,4,0}, {1,5,2}, {1,6,1},
    {1,7,0}, {1,8,100}, {1,9,99}, {1,10,0}, {1,11,98}, {1,12,0},
    {1,13,96}, {1,14,95}, {1,15,0}, {1,16,94}, {2,17,16}, {2,18,0},
    {2,19,15}, {2,20,0}, {2,21,14}, {2,22,12}, {2,23,0}, {2,24,11},
    {2,25,10}, {2,26,0}, {2,27,9}, {2,28,0}, {2,29,8}, {2,30,7},
    {2,31,0}, {2,32,6}, {3,33,27}, {3,34,0}, {3,35,26}, {3,36,0},
    {3,37,25}, {3,38,24}, {3,39,0}, {3,40,23}, {3,41,22}, {3,42,0},
    {3,43,21}, {3,44,0}, {3,45,19}, {3,46,18}, {3,47,0}, {3,48,17},
    {4,49,39}, {4,50,0}, {4,51,38}, {4,52,0}, {4,53,37}, {4,54,35},
    {4,55,0}, {4,56,34}, {4,57,33}, {4,58,0}, {4,59,32}, {4,60,0},
    {4,61,31}, {4,62,30}, {4,63,0}, {4,64,29}, {5,65,42}, {5,66,0},
    {5,67,43}, {5,68,0}, {5,69,44}, {5,70,46}, {5,71,0}, {5,72,47},
    {5,73,48}, {5,74,0}, {5,75,49}, {5,76,0}, {5,77,50}, {5,78,51},
    {5,79,0}, {5,80,52}, {6,81,54}, {6,82,0}, {6,83,55}, {6,84,0},
    {6,85,56}, {6,86,57}, {6,87,0}, {6,88,58}, {6,89,59}, {6,90,0},
    {6,91,60}, {6,92,0}, {6,93,62}, {6,94,63}, {6,95,0}, {6,96,64},
    {7,97,65}, {7,98,0}, {7,99,66}, {7,100,0}, {7,101,67}, {7,102,69},
    {7,103,0}, {7,104,70}, {7,105,71}, {7,106,0}, {7,107,72}, {7,108,0},
    {7,109,73}, {7,110,74}, {7,111,0}, {7,112,75}, {8,113,77}, {8,114,0},
    {8,115,78}, {8,116,0}, {8,117,79}, {8,118,80}, {8,119,0}, {8,120,81},
    {8,121,82}, {8,122,0}, {8,123,83}, {8,124,0}, {8,125,85}, {8,126,86},
    {8,127,0}, {8,128,87}, {0,0,92}, {0,0,91}, {0,0,90}, {0,0,89},
};
/* the 30 pins that can drive a global OE */
static const int oe_pins_1508_pqfp100[30] = { 90, 92, 11, 34, 47, 70, 85, 96, 62, 19, 9, 98, 73, 50, 8, 31, 75, 52, 29, 6, 87, 94, 64, 32, 17, 100, 80, 24, 57, 65 };
/* side and line of the label of each of the 100 pins */
static const FitPkgLabel labels_1508_pqfp100[100] = {
    {1,16}, {1,17}, {1,18}, {1,19}, {1,20}, {1,21}, {1,22}, {1,23}, {1,24}, {1,25},
    {1,26}, {1,27}, {1,28}, {1,29}, {1,30}, {1,31}, {1,32}, {1,33}, {1,34}, {1,35},
    {1,36}, {1,37}, {1,38}, {1,39}, {1,40}, {1,41}, {1,42}, {1,43}, {1,44}, {1,45},
    {2,25}, {2,27}, {2,29}, {2,31}, {2,33}, {2,35}, {2,37}, {2,39}, {2,41}, {2,43},
    {2,45}, {2,47}, {2,49}, {2,51}, {2,53}, {2,55}, {2,57}, {2,59}, {2,61}, {2,63},
    {0,45}, {0,44}, {0,43}, {0,42}, {0,41}, {0,40}, {0,39}, {0,38}, {0,37}, {0,36},
    {0,35}, {0,34}, {0,33}, {0,32}, {0,31}, {0,30}, {0,29}, {0,28}, {0,27}, {0,26},
    {0,25}, {0,24}, {0,23}, {0,22}, {0,21}, {0,20}, {0,19}, {0,18}, {0,17}, {0,16},
    {3,63}, {3,61}, {3,59}, {3,57}, {3,55}, {3,53}, {3,51}, {3,49}, {3,47}, {3,45},
    {3,43}, {3,41}, {3,39}, {3,37}, {3,35}, {3,33}, {3,31}, {3,29}, {3,27}, {3,25},
};
/* The 63 lines of the package drawing; dev_draw_package copies them into
 * rows of the drawing buffer (160 bytes per row) and ends 63 rows at
 * column 85. */
static const char *const drawing_1508_pqfp100[63] = {
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                               G       V   G   G G       V                          ",
    "                               N       C   N   N N       C                          ",
    "                               D       C   D   D D       C                          ",
    "                       --------------------------------------------                 ",
    "                      / 100  98  96  94  92  90  88  86  84  82    \\               ",
    "                     /     99  97  95  93  91  89  87  85  83  81   \\              ",
    "                    |  1 (*)                                      80 |              ",
    "                    |  2                                          79 |              ",
    "                    |  3                                          78 |              ",
    "                    |  4                                          77 |              ",
    "                VCC |  5                                          76 | GND          ",
    "                    |  6                                          75 |              ",
    "                    |  7                                          74 |              ",
    "                    |  8                                          73 |              ",
    "                    |  9                                          72 |              ",
    "                    | 10                                          71 |              ",
    "                    | 11                                          70 |              ",
    "                    | 12                                          69 |              ",
    "                GND | 13                                          68 | VCC          ",
    "                    | 14                                          67 |              ",
    "                    | 15                                          66 |              ",
    "                    | 16                ATF1508                   65 |              ",
    "                    | 17             100-Lead PQFP                64 |              ",
    "                    | 18                                          63 |              ",
    "                    | 19                                          62 |              ",
    "                VCC | 20                                          61 | GND          ",
    "                    | 21                                          60 |              ",
    "                    | 22                                          59 |              ",
    "                    | 23                                          58 |              ",
    "                    | 24                                          57 |              ",
    "                    | 25                                          56 |              ",
    "                    | 26                                          55 |              ",
    "                    | 27                                          54 |              ",
    "                GND | 28                                          53 | VCC          ",
    "                    | 29                                          52 |              ",
    "                    | 30                                          51 |              ",
    "                     \\     32  34  36  38  40  42  44  46  48  50   /              ",
    "                      \\  31  33  35  37  39  41  43  45  47  49    /               ",
    "                       --------------------------------------------                 ",
    "                                   V       G V       G                              ",
    "                                   C       N C       N                              ",
    "                                   C       D C       D                              ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                              \t                     ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
    "                                                                                    ",
};
static const unsigned char drawing_row_1508_pqfp100[63] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23,
    24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
    48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62,
};

/* 132 rows: block, macrocell, pin. */
static const FitPkgRow rows_1508_tqfp100[132] = {
    {1,1,2}, {1,2,0}, {1,3,1}, {1,4,0}, {1,5,100}, {1,6,99},
    {1,7,0}, {1,8,98}, {1,9,97}, {1,10,0}, {1,11,96}, {1,12,0},
    {1,13,94}, {1,14,93}, {1,15,0}, {1,16,92}, {2,17,14}, {2,18,0},
    {2,19,13}, {2,20,0}, {2,21,12}, {2,22,10}, {2,23,0}, {2,24,9},
    {2,25,8}, {2,26,0}, {2,27,7}, {2,28,0}, {2,29,6}, {2,30,5},
    {2,31,0}, {2,32,4}, {3,33,25}, {3,34,0}, {3,35,24}, {3,36,0},
    {3,37,23}, {3,38,22}, {3,39,0}, {3,40,21}, {3,41,20}, {3,42,0},
    {3,43,19}, {3,44,0}, {3,45,17}, {3,46,16}, {3,47,0}, {3,48,15},
    {4,49,37}, {4,50,0}, {4,51,36}, {4,52,0}, {4,53,35}, {4,54,33},
    {4,55,0}, {4,56,32}, {4,57,31}, {4,58,0}, {4,59,30}, {4,60,0},
    {4,61,29}, {4,62,28}, {4,63,0}, {4,64,27}, {5,65,40}, {5,66,0},
    {5,67,41}, {5,68,0}, {5,69,42}, {5,70,44}, {5,71,0}, {5,72,45},
    {5,73,46}, {5,74,0}, {5,75,47}, {5,76,0}, {5,77,48}, {5,78,49},
    {5,79,0}, {5,80,50}, {6,81,52}, {6,82,0}, {6,83,53}, {6,84,0},
    {6,85,54}, {6,86,55}, {6,87,0}, {6,88,56}, {6,89,57}, {6,90,0},
    {6,91,58}, {6,92,0}, {6,93,60}, {6,94,61}, {6,95,0}, {6,96,62},
    {7,97,63}, {7,98,0}, {7,99,64}, {7,100,0}, {7,101,65}, {7,102,67},
    {7,103,0}, {7,104,68}, {7,105,69}, {7,106,0}, {7,107,70}, {7,108,0},
    {7,109,71}, {7,110,72}, {7,111,0}, {7,112,73}, {8,113,75}, {8,114,0},
    {8,115,76}, {8,116,0}, {8,117,77}, {8,118,78}, {8,119,0}, {8,120,79},
    {8,121,80}, {8,122,0}, {8,123,81}, {8,124,0}, {8,125,83}, {8,126,84},
    {8,127,0}, {8,128,85}, {0,0,90}, {0,0,89}, {0,0,88}, {0,0,87},
};
/* the 30 pins that can drive a global OE */
static const int oe_pins_1508_tqfp100[30] = { 88, 90, 9, 32, 45, 68, 83, 94, 60, 17, 7, 96, 71, 48, 6, 29, 73, 50, 27, 4, 85, 92, 62, 30, 15, 98, 78, 22, 55, 63 };
/* side and line of the label of each of the 100 pins */
static const FitPkgLabel labels_1508_tqfp100[100] = {
    {1,16}, {1,17}, {1,18}, {1,19}, {1,20}, {1,21}, {1,22}, {1,23}, {1,24}, {1,25},
    {1,26}, {1,27}, {1,28}, {1,29}, {1,30}, {1,31}, {1,32}, {1,33}, {1,34}, {1,35},
    {1,36}, {1,37}, {1,38}, {1,39}, {1,40}, {2,20}, {2,22}, {2,24}, {2,26}, {2,28},
    {2,30}, {2,32}, {2,34}, {2,36}, {2,38}, {2,40}, {2,42}, {2,44}, {2,46}, {2,48},
    {2,50}, {2,52}, {2,54}, {2,56}, {2,58}, {2,60}, {2,62}, {2,64}, {2,66}, {2,0},
    {0,40}, {0,39}, {0,38}, {0,37}, {0,36}, {0,35}, {0,34}, {0,33}, {0,32}, {0,31},
    {0,30}, {0,29}, {0,28}, {0,27}, {0,26}, {0,25}, {0,24}, {0,23}, {0,22}, {0,21},
    {0,20}, {0,19}, {0,18}, {0,17}, {0,16}, {3,68}, {3,66}, {3,64}, {3,62}, {3,60},
    {3,58}, {3,56}, {3,54}, {3,52}, {3,50}, {3,48}, {3,46}, {3,44}, {3,42}, {3,40},
    {3,38}, {3,36}, {3,34}, {3,32}, {3,30}, {3,28}, {3,26}, {3,24}, {3,22}, {3,20},
};
/* The 57 lines of the package drawing; dev_draw_package copies them into
 * rows of the drawing buffer (160 bytes per row) and ends 57 rows at
 * column 95. */
static const char *const drawing_1508_tqfp100[57] = {
    "                                                                                             ",
    "                                                                                             ",
    "                                                                                             ",
    "                                                                                             ",
    "                                                                                             ",
    "                                                                                             ",
    "                                                                                             ",
    "                                                                                             ",
    "                                                                                             ",
    "                                                                                             ",
    "                              G       V         G       V                                    ",
    "                              N       C         N       C                                    ",
    "                              D       C         D       C                                    ",
    "                  ------------------------------------------------------                     ",
    "                 / 100  98  96  94  92  90  88  86  84  82  80  78  76  \\                   ",
    "                /     99  97  95  93  91  89  87  85  83  81  79  77     \\                  ",
    "               | 1                                                     75 |                  ",
    "               | 2                                                     74 | GND              ",
    "           VCC | 3                                                     73 |                  ",
    "               | 4                                                     72 |                  ",
    "               | 5                                                     71 |                  ",
    "               | 6                                                     70 |                  ",
    "               | 7                                                     69 |                  ",
    "               | 8                                                     68 |                  ",
    "               | 9                                                     67 |                  ",
    "               | 10                                                    66 | VCC              ",
    "           GND | 11                                                    65 |                  ",
    "               | 12                     ATF1508                        64 |                  ",
    "               | 13                  100-Lead TQFP                     63 |                  ",
    "               | 14                                                    62 |                  ",
    "               | 15                                                    61 |                  ",
    "               | 16                                                    60 |                  ",
    "               | 17                                                    59 | GND              ",
    "           VCC | 18                                                    58 |                  ",
    "               | 19                                                    57 |                  ",
    "               | 20                                                    56 |                  ",
    "               | 21                                                    55 |                  ",
    "               | 22                                                    54 |                  ",
    "               | 23                                                    53 |                  ",
    "               | 24                                                    52 |                  ",
    "               | 25                                                    51 | VCC              ",
    "                \\     27  29  31  33  35  37  39  41  43  45   47  49    /                  ",
    "                 \\  26  28  30  32  34  36  38  40  42  44  46  48   50 /                   ",
    "                  ------------------------------------------------------                     ",
    "                    G               V       G V       G                                      ",
    "                    N               C       N C       N                                      ",
    "                    D               C       D C       D                                      ",
    "                                                                                             ",
    "                                                                                             ",
    "                                                                                             ",
    "                                                                                             ",
    "                                                                                             ",
    "                                                                                             ",
    "                                                                                             ",
    "                                                                                             ",
    "                                                                                             ",
    "                                                                                             ",
};
static const unsigned char drawing_row_1508_tqfp100[57] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23,
    24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
    48, 49, 50, 51, 52, 53, 54, 55, 56,
};

/* 132 rows: block, macrocell, pin. */
static const FitPkgRow rows_1508_pqfp160[132] = {
    {1,1,160}, {1,2,0}, {1,3,159}, {1,4,158}, {1,5,153}, {1,6,152},
    {1,7,0}, {1,8,151}, {1,9,150}, {1,10,0}, {1,11,149}, {1,12,147},
    {1,13,146}, {1,14,145}, {1,15,0}, {1,16,144}, {2,17,21}, {2,18,0},
    {2,19,20}, {2,20,19}, {2,21,18}, {2,22,16}, {2,23,0}, {2,24,15},
    {2,25,14}, {2,26,0}, {2,27,13}, {2,28,12}, {2,29,11}, {2,30,10},
    {2,31,0}, {2,32,9}, {3,33,41}, {3,34,0}, {3,35,33}, {3,36,32},
    {3,37,31}, {3,38,30}, {3,39,0}, {3,40,29}, {3,41,28}, {3,42,0},
    {3,43,27}, {3,44,25}, {3,45,24}, {3,46,23}, {3,47,0}, {3,48,22},
    {4,49,59}, {4,50,0}, {4,51,58}, {4,52,57}, {4,53,56}, {4,54,54},
    {4,55,0}, {4,56,53}, {4,57,52}, {4,58,0}, {4,59,51}, {4,60,50},
    {4,61,49}, {4,62,48}, {4,63,0}, {4,64,43}, {5,65,62}, {5,66,0},
    {5,67,63}, {5,68,64}, {5,69,65}, {5,70,67}, {5,71,0}, {5,72,68},
    {5,73,69}, {5,74,0}, {5,75,70}, {5,76,71}, {5,77,72}, {5,78,73},
    {5,79,0}, {5,80,78}, {6,81,80}, {6,82,0}, {6,83,88}, {6,84,89},
    {6,85,90}, {6,86,91}, {6,87,0}, {6,88,92}, {6,89,93}, {6,90,0},
    {6,91,94}, {6,92,96}, {6,93,97}, {6,94,98}, {6,95,0}, {6,96,99},
    {7,97,100}, {7,98,0}, {7,99,101}, {7,100,102}, {7,101,103}, {7,102,105},
    {7,103,0}, {7,104,106}, {7,105,107}, {7,106,0}, {7,107,108}, {7,108,109},
    {7,109,110}, {7,110,111}, {7,111,0}, {7,112,112}, {8,113,121}, {8,114,0},
    {8,115,122}, {8,116,123}, {8,117,128}, {8,118,129}, {8,119,0}, {8,120,130},
    {8,121,131}, {8,122,0}, {8,123,132}, {8,124,134}, {8,125,135}, {8,126,136},
    {8,127,0}, {8,128,137}, {0,0,142}, {0,0,141}, {0,0,140}, {0,0,139},
};
/* the 30 pins that can drive a global OE */
static const int oe_pins_1508_pqfp160[30] = { 140, 142, 15, 53, 68, 106, 135, 146, 97, 24, 13, 149, 110, 72, 11, 49, 112, 78, 43, 9, 137, 144, 99, 51, 22, 151, 129, 30, 91, 100 };
/* side and line of the label of each of the 160 pins */
static const FitPkgLabel labels_1508_pqfp160[160] = {
    {1,16}, {1,17}, {1,18}, {1,19}, {1,20}, {1,21}, {1,22}, {1,23}, {1,24}, {1,25},
    {1,26}, {1,27}, {1,28}, {1,29}, {1,30}, {1,31}, {1,32}, {1,33}, {1,34}, {1,35},
    {1,36}, {1,37}, {1,38}, {1,39}, {1,40}, {1,41}, {1,42}, {1,43}, {1,44}, {1,45},
    {1,46}, {1,47}, {1,48}, {1,49}, {1,50}, {1,51}, {1,52}, {1,53}, {1,54}, {1,55},
    {2,25}, {2,27}, {2,29}, {2,31}, {2,33}, {2,35}, {2,37}, {2,39}, {2,41}, {2,43},
    {2,45}, {2,47}, {2,49}, {2,51}, {2,53}, {2,55}, {2,57}, {2,59}, {2,61}, {2,63},
    {2,65}, {2,67}, {2,69}, {2,71}, {2,73}, {2,75}, {2,77}, {2,79}, {2,81}, {2,83},
    {2,85}, {2,87}, {2,89}, {2,91}, {2,93}, {2,95}, {2,97}, {2,99}, {2,101}, {2,103},
    {0,45}, {0,44}, {0,43}, {0,42}, {0,41}, {0,40}, {0,39}, {0,38}, {0,37}, {0,36},
    {0,35}, {0,34}, {0,33}, {0,32}, {0,31}, {0,30}, {0,29}, {0,28}, {0,27}, {0,26},
    {0,25}, {0,24}, {0,23}, {0,22}, {0,21}, {0,20}, {0,19}, {0,18}, {0,17}, {0,16},
    {0,15}, {0,14}, {0,13}, {0,12}, {0,11}, {0,10}, {0,9}, {0,8}, {0,7}, {0,6},
    {3,63}, {3,61}, {3,59}, {3,57}, {3,55}, {3,53}, {3,51}, {3,49}, {3,47}, {3,45},
    {3,43}, {3,41}, {3,39}, {3,37}, {3,35}, {3,33}, {3,31}, {3,29}, {3,27}, {3,25},
    {3,23}, {3,21}, {3,19}, {3,17}, {3,15}, {3,13}, {3,11}, {3,9}, {3,7}, {3,5},
    {0,0}, {0,0}, {0,0}, {0,0}, {0,0}, {0,0}, {0,0}, {0,0}, {0,0}, {0,0},
};
/* The 72 lines of the package drawing; dev_draw_package copies them into
 * rows of the drawing buffer (160 bytes per row) and ends 72 rows at
 * column 120. */
static const char *const drawing_1508_pqfp160[72] = {
    " \t\t                                                                       \t\t\t                     ",
    "                                                                                      \t\t\t\t\t     ",
    "                                                                                       \t\t\t             ",
    "                                                                                       \t\t\t\t     ",
    "                                                                                       \t\t\t\t     ",
    "                                                                                       \t\t\t\t     ",
    "                                                                                       \t\t\t\t     ",
    "                                                                                       \t\t\t\t     ",
    "                                                                                        \t\t\t\t     ",
    "                                                                                         \t\t\t\t     ",
    "                      \t\t\t\t              G       V         G       V                                    ",
    "                     \t\t\t\t              N       C         N       C                                    ",
    "          \t\t\t\t\t              D       C         D       C                                    ",
    "                  ----------------------------------------------------------------------------------                        ",
    "                 /   159 157 155 153 151 149 147 145 143 141 139 137 135 133 131 129 127 125 123 121  \\                    ",
    "                /  160 158 156 154 152 150 148 146 144 142 140 138 136 134 132 130 128 126 124 122     \\                   ",
    "               | 1                                             \t\t\t              120 |                  ",
    "               | 2                                              \t\t                      119 | GND              ",
    "           VCC | 3                                             \t\t\t\t      118 |                  ",
    "               | 4                                              \t\t\t\t      117 |                  ",
    "               | 5                                             \t\t\t\t      116 |                  ",
    "               | 6                                             \t\t\t\t      115 |                  ",
    "               | 7                                             \t\t\t\t      114 |                  ",
    "               | 8                                             \t\t\t\t      113 |                  ",
    "               | 9                                             \t\t\t\t      112 |                  ",
    "               | 10                                             \t\t\t\t      111 | VCC              ",
    "           GND | 11                                            \t\t\t\t      110 |                  ",
    "               | 12                                                \t\t\t\t      109 |                  ",
    "               | 13                                             \t\t\t\t      108 |                  ",
    "               | 14                                            \t\t\t\t      107 |                  ",
    "               | 15                                            \t\t\t\t      106 |                  ",
    "               | 16                                            \t\t\t\t      105 |                  ",
    "               | 17                                  \t\t                   \t              104 | GND              ",
    "           VCC | 18                                  \t\t\t      \t\t\t      103 |                  ",
    "               | 19                                  ATF1508    \t\t\t\t      102 |                  ",
    "               | 20                                160-Lead PQFP          \t\t              101 |                  ",
    "               | 21                                            \t\t\t\t      100 |                  ",
    "               | 22                                            \t\t\t\t       99 |                  ",
    "               | 23                                             \t\t\t\t       98 |                  ",
    "               | 24                                             \t\t\t\t       97 |                  ",
    "               | 25                                              \t\t\t\t       96 | VCC              ",
    "           GND | 26                                           \t\t\t\t\t       95 |                  ",
    "               | 27                                     \t\t\t\t               94 |                  ",
    "               | 28                                \t\t                \t\t       93 |                  ",
    "               | 29                                     \t\t\t\t               92 |                  ",
    "               | 30                                    \t\t\t\t               91 |                  ",
    "               | 31                                      \t\t\t\t               90 |                  ",
    "               | 32                                                \t\t\t\t       89 | GND              ",
    "           VCC | 33                                     \t\t\t\t               88 |                  ",
    "               | 34                                \t\t\t\t                       87 |                  ",
    "               | 35                                  \t\t\t\t                       86 |                  ",
    "               | 36                                  \t\t\t\t                       85 |                  ",
    "               | 37                             \t\t\t                               84 |                  ",
    "               | 38                         \t\t\t\t                               83 |                  ",
    "               | 39                                    \t\t\t                       82 |                  ",
    "               | 40                                  \t\t\t\t                       81 | VCC              ",
    "                \\     42  44  46  48  50  52  54  56  58  60  62  64  66  68  70  72  74  76  78  80   /                   ",
    "                 \\  41  43  45  47  49  51  53  55  57  59  61  63  65  67  69  71  73  75  77  79    /                    ",
    "                  -------------------------------------------------------------------------------------                     ",
    "                   \t\t\t\t    G               V       G V       G                                      ",
    "                   \t\t\t\t    N               C       N C       N                                      ",
    "                  \t\t\t\t    D               C       D C       D                                      ",
    "                       \t\t\t\t                                                                     ",
    "                       \t\t\t\t                                                                     ",
    "                        \t\t\t\t                                                                     ",
    "                        \t\t\t\t                                                                     ",
    "                        \t\t\t\t                                                                     ",
    "                        \t\t\t\t                                                                     ",
    "                        \t\t\t\t                                                                     ",
    "                        \t\t\t\t                                                                     ",
    "                        \t\t\t\t                                                                     ",
    "                            \t\t\t\t\t                                                             ",
};
static const unsigned char drawing_row_1508_pqfp160[72] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23,
    24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
    48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71,
};

/* ATF1508BE CBGA132, data sheet tables 13-1 and 13-2.  Pin numbers
 * enumerate occupied balls by row, left to right, as JEDEC vector positions.
 * MC17 uses G2: C2 is the separate OE1 input. */
static const FitPkgRow rows_1508_cbga132[132] = {
    {1,1,61}, {1,2,0}, {1,3,55}, {1,4,0}, {1,5,56}, {1,6,57},
    {1,7,0}, {1,8,49}, {1,9,50}, {1,10,0}, {1,11,51}, {1,12,0},
    {1,13,43}, {1,14,44}, {1,15,0}, {1,16,29}, {2,17,62}, {2,18,0},
    {2,19,63}, {2,20,0}, {2,21,67}, {2,22,68}, {2,23,0}, {2,24,69},
    {2,25,73}, {2,26,0}, {2,27,74}, {2,28,0}, {2,29,79}, {2,30,81},
    {2,31,0}, {2,32,99}, {3,33,15}, {3,34,0}, {3,35,16}, {3,36,0},
    {3,37,1}, {3,38,18}, {3,39,0}, {3,40,4}, {3,41,33}, {3,42,0},
    {3,43,19}, {3,44,0}, {3,45,5}, {3,46,34}, {3,47,0}, {3,48,114},
    {4,49,120}, {4,50,0}, {4,51,93}, {4,52,0}, {4,53,107}, {4,54,121},
    {4,55,0}, {4,56,94}, {4,57,95}, {4,58,0}, {4,59,109}, {4,60,0},
    {4,61,123}, {4,62,96}, {4,63,0}, {4,64,110}, {5,65,65}, {5,66,0},
    {5,67,64}, {5,68,0}, {5,69,60}, {5,70,59}, {5,71,0}, {5,72,58},
    {5,73,53}, {5,74,0}, {5,75,52}, {5,76,0}, {5,77,48}, {5,78,47},
    {5,79,0}, {5,80,46}, {6,81,70}, {6,82,0}, {6,83,71}, {6,84,0},
    {6,85,77}, {6,86,76}, {6,87,0}, {6,88,84}, {6,89,83}, {6,90,0},
    {6,91,90}, {6,92,0}, {6,93,89}, {6,94,88}, {6,95,0}, {6,96,100},
    {7,97,40}, {7,98,0}, {7,99,26}, {7,100,0}, {7,101,12}, {7,102,39},
    {7,103,0}, {7,104,25}, {7,105,11}, {7,106,0}, {7,107,38}, {7,108,0},
    {7,109,10}, {7,110,37}, {7,111,0}, {7,112,23}, {8,113,118}, {8,114,0},
    {8,115,117}, {8,116,0}, {8,117,132}, {8,118,130}, {8,119,0}, {8,120,101},
    {8,121,115}, {8,122,0}, {8,123,129}, {8,124,0}, {8,125,128}, {8,126,127},
    {8,127,0}, {8,128,98}, {0,0,31}, {0,0,3}, {0,0,30}, {0,0,86},
};
static const int oe_pins_1508_cbga132[30] = {
    30, 31, 69, 94, 58, 25, 128, 43, 89, 5, 74, 51, 10, 48, 79, 123, 23, 46, 110, 99, 98, 29, 100, 109, 114, 49, 130, 18, 76, 40
};
static const FitBallPin balls_1508_cbga132[132] = {
    { "A1", "I/O" }, { "A2", "VCCINT" }, { "A3", "GCLR" }, { "A4", "I/O" },
    { "A5", "I/O" }, { "A6", "NC" }, { "A7", "VCCIOB" }, { "A8", "NC" },
    { "A9", "GND" }, { "A10", "I/O" }, { "A11", "I/O" }, { "A12", "I/O" },
    { "A13", "NC" }, { "A14", "VCCIOB" }, { "B1", "I/O" }, { "B2", "I/O" },
    { "B3", "GND" }, { "B4", "I/O" }, { "B5", "I/O" }, { "B6", "NC" },
    { "B7", "NC" }, { "B8", "NC" }, { "B9", "I/O/TDO" }, { "B10", "NC" },
    { "B11", "I/O" }, { "B12", "I/O" }, { "B13", "NC" }, { "B14", "GND" },
    { "C1", "I/O" }, { "C2", "OE1" }, { "C3", "OE2/GCLK2" }, { "C4", "VCCIOB" },
    { "C5", "I/O" }, { "C6", "I/O" }, { "C7", "NC" }, { "C8", "NC" },
    { "C9", "I/O" }, { "C10", "I/O" }, { "C11", "I/O" }, { "C12", "I/O" },
    { "C13", "NC" }, { "C14", "NC" }, { "D1", "I/O" }, { "D2", "I/O" },
    { "D3", "NC" }, { "D12", "I/O" }, { "D13", "I/O" }, { "D14", "I/O" },
    { "E1", "I/O" }, { "E2", "I/O" }, { "E3", "I/O" }, { "E12", "I/O" },
    { "E13", "I/O" }, { "E14", "GND" }, { "F1", "I/O/PD1" }, { "F2", "I/O" },
    { "F3", "I/O" }, { "F12", "I/O" }, { "F13", "I/O" }, { "F14", "I/O" },
    { "G1", "I/O" }, { "G2", "I/O" }, { "G3", "I/O" }, { "G12", "I/O/PD2" },
    { "G13", "I/O" }, { "G14", "VCCIOA" }, { "H1", "I/O/VREFA" }, { "H2", "I/O" },
    { "H3", "I/O" }, { "H12", "I/O" }, { "H13", "I/O" }, { "H14", "GND" },
    { "J1", "I/O" }, { "J2", "I/O" }, { "J3", "VCCIOA" }, { "J12", "I/O" },
    { "J13", "I/O" }, { "J14", "GND" }, { "K1", "I/O" }, { "K2", "GND" },
    { "K3", "I/O" }, { "K12", "VCCINT" }, { "K13", "I/O" }, { "K14", "I/O" },
    { "L1", "NC" }, { "L2", "GCLK1" }, { "L3", "NC" }, { "L12", "I/O" },
    { "L13", "I/O/VREFB" }, { "L14", "I/O" }, { "M1", "NC" }, { "M2", "NC" },
    { "M3", "I/O" }, { "M4", "I/O" }, { "M5", "I/O" }, { "M6", "I/O" },
    { "M7", "NC" }, { "M8", "I/O/GCLK3" }, { "M9", "I/O/TDI" }, { "M10", "I/O/TCK" },
    { "M11", "I/O" }, { "M12", "NC" }, { "M13", "NC" }, { "M14", "NC" },
    { "N1", "GND" }, { "N2", "NC" }, { "N3", "I/O" }, { "N4", "NC" },
    { "N5", "I/O" }, { "N6", "I/O" }, { "N7", "NC" }, { "N8", "NC" },
    { "N9", "GND" }, { "N10", "I/O/TMS" }, { "N11", "I/O" }, { "N12", "GND" },
    { "N13", "I/O" }, { "N14", "I/O" }, { "P1", "VCCINT" }, { "P2", "I/O" },
    { "P3", "I/O" }, { "P4", "GND" }, { "P5", "I/O" }, { "P6", "NC" },
    { "P7", "VCCIOA" }, { "P8", "NC" }, { "P9", "I/O" }, { "P10", "I/O" },
    { "P11", "I/O" }, { "P12", "I/O" }, { "P13", "VCCIOA" }, { "P14", "I/O" },
};

/* ATF1508 packages, with their special pins (global clocks, global clear,
 * JTAG, power down), test vector pin maps and pin limits. */
static const FitPackageDesc packages_1508[6] = {
    { FIT_PKG_PLCC68, "PLCC68", "P1508C68", 68, 52, rows_1508_plcc68,
      { 67, 2, 65 }, 1, oe_pins_1508_plcc68, 26, { 12, 19, 50, 57 }, { 10, 36 },
      "XXNXXNXXXXNXXXXNXXXXNXXXXNXXXXNXXNNXXNXXXXNXXXXNXXXXNXXXXNXXXXNXXNXX",
      drawing_1508_plcc68, drawing_row_1508_plcc68, 49, 85, 48, 160,
      { 0, 16, 60, 10, 0, 12, 36, 10 }, labels_1508_plcc68, NULL },
    { FIT_PKG_PLCC84, "PLCC84", "P1508C84", 84, 68, rows_1508_plcc84,
      { 83, 2, 81 }, 1, oe_pins_1508_plcc84, 30, { 14, 23, 62, 71 }, { 12, 45 },
      "XXNXXXNXXXXXNXXXXXNXXXXXXNXXXXXNXXXXXNXXXNNXXXNXXXXXNXXXXXNXXXXXXNXXXXXNXXXXXNXXXNXX",
      drawing_1508_plcc84, drawing_row_1508_plcc84, 53, 85, 53, 160,
      { 0, 16, 68, 10, 0, 12, 40, 10 }, labels_1508_plcc84, NULL },
    { FIT_PKG_PQFP100, "PQFP100", "P1508Q100", 100, 84, rows_1508_pqfp100,
      { 89, 92, 87 }, 91, oe_pins_1508_pqfp100, 30, { 6, 17, 64, 75 }, { 3, 43 },
      "XXXXNXXXXXXXNXXXXXXNXXXXXXXNXXXXXXXNXXXNNXXXNXXXXXXXNXXXXXXXNXXXXXXNXXXXXXXNXXXXXXXNXXXNXXXXNXXXNXXX",
      drawing_1508_pqfp100, drawing_row_1508_pqfp100, 63, 85, 63, 160,
      { 0, 19, 71, 10, 0, 12, 49, 10 }, labels_1508_pqfp100, NULL },
    { FIT_PKG_TQFP100, "TQFP100", "P1508T100", 100, 84, rows_1508_tqfp100,
      { 87, 90, 85 }, 89, oe_pins_1508_tqfp100, 30, { 4, 15, 62, 73 }, { 1, 41 },
      "XXNXXXXXXXNXXXXXXNXXXXXXXNXXXXXXXNXXXNNXXXNXXXXXXXNXXXXXXXNXXXXXXNXXXXXXXNXXXXXXXNXXXNXXXXNXXXNXXXXX",
      drawing_1508_tqfp100, drawing_row_1508_tqfp100, 57, 95, 57, 160,
      { 0, 14, 76, 10, 0, 12, 44, 10 }, labels_1508_tqfp100, NULL },
    { FIT_PKG_PQFP160, "PQFP160", "P1508Q160", 160, 100, rows_1508_pqfp160,
      { 139, 142, 137 }, 141, oe_pins_1508_pqfp160, 30, { 9, 22, 99, 112 }, { 63, 159 },
      "NNNNNNNNXXXXXXXXNXXXXXXXXNXXXXXXXNNNNNNNXNXNNNNXXXXXXXNXXXXNNXXXXNXXXXXXXNNNNXNXNNNNNNNXXXXXXXNXXXXXXXXNXXXXXXXXNNNNNNNNXXXNNNNXXXXXNXXXXNXXXXNXXXXNXXXXXNNNNXXX",
      drawing_1508_pqfp160, drawing_row_1508_pqfp160, 72, 120, 72, 160,
      { 0, 16, 60, 10, 0, 12, 36, 10 }, labels_1508_pqfp160, NULL },
    { FIT_PKG_CBGA132, "CBGA132", "P1508B132", 132, 84, rows_1508_cbga132,
      { 86, 31, 98 }, 3, oe_pins_1508_cbga132, 30, { 99, 114, 100, 23 }, { 55, 64 },
      "XNXXXNNNNXXXNNXXNXXNNNXNXXNNXXXNXXNNXXXXNNXXNXXXXXXXXNXXXXXXXXXXXNXXXXXNXXNXXNXNXNXXNXNXXXNNXXXXNXXXXNNNNNXNXXNNNXXNXXNXXNXNNNXXXXNX",
      NULL, NULL, 0, 0, 0, 0, { 0 }, NULL, balls_1508_cbga132 },
};

/* ATF1508: vector pin maps for package names that are not packages of
 * this device (used only with the package name of another device). */
static const FitVectorMap vector_maps_1508[2] = {
    { "PLCC44", "XXNXXXXXXNXXXXNXXXXXXNNXXXXXXNXXXXNXXXXXXNXX" },
    { "TQFP44", "XXXNXXXXNXXXXXXNNXXXXXXNXXXXNXXXXXXNXXXXNXXX" },
};

/* The device descriptions, indexed by enum FitDevice. */
const FitDeviceDesc g_dev_descs[3] = {
    { FIT_DEV_1502, { 2, 32, 4, 5, 3, 1, 2, 3, "1502" }, &jedec_1502,
      packages_1502, 2, &packages_1502[0], 14, uim_src_1502, oe_src_1502, vector_maps_1502, 0, 10 },
    { FIT_DEV_1504, { 4, 64, 4, 9, 3, 1, 2, 3, "1504" }, &jedec_1504,
      packages_1504, 6, &packages_1504[0], 21, uim_src_1504, oe_src_1504, vector_maps_1504, 0, 10 },
    { FIT_DEV_1508, { 8, 128, 4, 27, 5, 1, 2, 3, "1508" }, &jedec_1508,
      packages_1508, 6, &packages_1508[1], 30, uim_src_1508, oe_src_1508, vector_maps_1508, 2, 20 },
};

/* Logical fitter controls to BE cell-record offsets. Low-power/PT-power
 * controls have no BE encoding; storage, termination, hysteresis and I/O
 * standard are handled separately by their technology-specific consumers. */
const signed char fit_be_cfg_offsets[24] = {
    7, 6, 5, 18, 17, 16, 26, 15, -1, 25, 24, 23,
    22, 21, 20, 14, 19, 13, 12, 11, 3, -1, 2, 9
};

const FitJedecLayout *fit_be_jedec_layout(enum FitDevice id)
{
    if (id == FIT_DEV_1502) return &jedec_1502_be;
    if (id == FIT_DEV_1504) return &jedec_1504_be;
    if (id == FIT_DEV_1508) return &jedec_1508_be;
    return NULL;
}

int fit_be_macrocell_base(enum FitDevice id, int mc)
{
    static const int order_1504[4] = {3, 0, 1, 2};
    static const int order_1508[8] = {6, 7, 0, 1, 2, 3, 4, 5};
    const FitJedecLayout *layout = fit_be_jedec_layout(id);
    int block, row;
    if (layout == NULL || mc < 0 || mc >= g_dev_descs[id].params.n_macrocells)
        return -1;
    block = mc / 16;
    if (id == FIT_DEV_1502) {
        row = 15 - mc % 16;
    } else if (id == FIT_DEV_1504) {
        row = block < 2 ? 15 - mc % 16 : mc % 16;
        block = order_1504[block];
    } else {
        row = block == 0 || block == 2 || block == 3 ? 15 - mc % 16 : mc % 16;
        block = order_1508[block];
    }
    return layout->mc_cfg_addr + block * 480 + row * 27;
}
