/*
 * atv5000.c - device specific product-term placement: ATV5000 (local and
 * global product terms of a macrocell) and the quadrant check of the EP1800.
 */
#include "fusemap.h"

/* ATV5000 macrocells: pin, quadrant, local terms, global terms, layout, first fuse, mode */
static const int32_t k_v5000_cells[241][7] = {
    { 1, 5, 0, 0, 0, 0, 1 },
    { 2, 5, 0, 0, 0, 0, 1 },
    { 68, 5, 0, 0, 0, 0, 1 },
    { 66, 5, 0, 0, 0, 0, 1 },
    { 32, 5, 0, 0, 0, 0, 1 },
    { 34, 5, 0, 0, 0, 0, 1 },
    { 35, 5, 0, 0, 0, 0, 1 },
    { 36, 5, 0, 0, 0, 0, 1 },
    { 851972, 0, 3, 2, 0, 95920, 2 },
    { 851973, 0, 3, 2, 0, 96720, 2 },
    { 851974, 0, 3, 2, 0, 97520, 2 },
    { 851975, 0, 3, 2, 0, 98320, 2 },
    { 851976, 0, 3, 2, 0, 99120, 2 },
    { 851977, 0, 3, 2, 0, 99920, 2 },
    { 851978, 0, 3, 2, 0, 100720, 2 },
    { 851979, 0, 3, 2, 0, 101520, 2 },
    { 851980, 0, 3, 2, 0, 102320, 2 },
    { 851981, 0, 3, 2, 0, 103120, 2 },
    { 851982, 0, 3, 2, 0, 103920, 2 },
    { 851983, 0, 3, 2, 0, 104720, 2 },
    { 851985, 0, 3, 2, 0, 105520, 2 },
    { 851986, 1, 3, 2, 0, 106320, 2 },
    { 851987, 1, 3, 2, 0, 107120, 2 },
    { 851989, 1, 3, 2, 0, 107920, 2 },
    { 851990, 1, 3, 2, 0, 108720, 2 },
    { 851991, 1, 3, 2, 0, 109520, 2 },
    { 851992, 1, 3, 2, 0, 110320, 2 },
    { 851993, 1, 3, 2, 0, 111120, 2 },
    { 851994, 1, 3, 2, 0, 111920, 2 },
    { 851995, 1, 3, 2, 0, 112720, 2 },
    { 851996, 1, 3, 2, 0, 113520, 2 },
    { 851997, 1, 3, 2, 0, 114320, 2 },
    { 851998, 1, 3, 2, 0, 115120, 2 },
    { 851999, 1, 3, 2, 0, 115920, 2 },
    { 852006, 2, 3, 2, 0, 116720, 2 },
    { 852007, 2, 3, 2, 0, 117520, 2 },
    { 852008, 2, 3, 2, 0, 118320, 2 },
    { 852009, 2, 3, 2, 0, 119120, 2 },
    { 852010, 2, 3, 2, 0, 119920, 2 },
    { 852011, 2, 3, 2, 0, 120720, 2 },
    { 852012, 2, 3, 2, 0, 121520, 2 },
    { 852013, 2, 3, 2, 0, 122320, 2 },
    { 852014, 2, 3, 2, 0, 123120, 2 },
    { 852015, 2, 3, 2, 0, 123920, 2 },
    { 852016, 2, 3, 2, 0, 124720, 2 },
    { 852017, 2, 3, 2, 0, 125520, 2 },
    { 852019, 2, 3, 2, 0, 126320, 2 },
    { 852020, 3, 3, 2, 0, 127120, 2 },
    { 852021, 3, 3, 2, 0, 127920, 2 },
    { 852023, 3, 3, 2, 0, 128720, 2 },
    { 852024, 3, 3, 2, 0, 129520, 2 },
    { 852025, 3, 3, 2, 0, 130320, 2 },
    { 852026, 3, 3, 2, 0, 131120, 2 },
    { 852027, 3, 3, 2, 0, 131920, 2 },
    { 852028, 3, 3, 2, 0, 132720, 2 },
    { 852029, 3, 3, 2, 0, 133520, 2 },
    { 852030, 3, 3, 2, 0, 134320, 2 },
    { 852031, 3, 3, 2, 0, 135120, 2 },
    { 852032, 3, 3, 2, 0, 135920, 2 },
    { 852033, 3, 3, 2, 0, 136720, 2 },
    { 327749, 0, 4, 1, 2, 156544, 1 },
    { 327750, 0, 4, 1, 2, 156224, 1 },
    { 327751, 0, 4, 1, 2, 155904, 1 },
    { 327752, 0, 4, 1, 2, 155584, 1 },
    { 327753, 0, 4, 1, 2, 155264, 1 },
    { 327754, 0, 4, 1, 2, 154944, 1 },
    { 327755, 1, 4, 1, 2, 156864, 1 },
    { 327756, 1, 4, 1, 2, 157184, 1 },
    { 327757, 1, 4, 1, 2, 157504, 1 },
    { 327758, 1, 4, 1, 2, 157824, 1 },
    { 327759, 1, 4, 1, 2, 158144, 1 },
    { 327760, 1, 4, 1, 2, 158464, 1 },
    { 327761, 2, 4, 1, 2, 160384, 1 },
    { 327762, 2, 4, 1, 2, 160064, 1 },
    { 327763, 2, 4, 1, 2, 159744, 1 },
    { 327764, 2, 4, 1, 2, 159424, 1 },
    { 327765, 2, 4, 1, 2, 159104, 1 },
    { 327766, 2, 4, 1, 2, 158784, 1 },
    { 327767, 3, 4, 1, 2, 160704, 1 },
    { 327768, 3, 4, 1, 2, 161024, 1 },
    { 327769, 3, 4, 1, 2, 161344, 1 },
    { 327770, 3, 4, 1, 2, 161664, 1 },
    { 327771, 3, 4, 1, 2, 161984, 1 },
    { 327772, 3, 4, 1, 2, 162304, 1 },
    { 262237, 0, 3, 1, 1, 105760, 1 },
    { 262238, 0, 3, 1, 1, 104960, 1 },
    { 262239, 0, 3, 1, 1, 104160, 1 },
    { 262240, 0, 3, 1, 1, 103360, 1 },
    { 262241, 0, 3, 1, 1, 102560, 1 },
    { 262242, 0, 3, 1, 1, 101760, 1 },
    { 262243, 0, 3, 1, 1, 100960, 1 },
    { 262244, 0, 3, 1, 1, 100160, 1 },
    { 262245, 0, 3, 1, 1, 99360, 1 },
    { 262246, 0, 3, 1, 1, 98560, 1 },
    { 262247, 0, 3, 1, 1, 97760, 1 },
    { 262248, 0, 3, 1, 1, 96960, 1 },
    { 262249, 0, 3, 1, 1, 96160, 1 },
    { 262250, 1, 3, 1, 1, 106560, 1 },
    { 262251, 1, 3, 1, 1, 107360, 1 },
    { 262252, 1, 3, 1, 1, 108160, 1 },
    { 262253, 1, 3, 1, 1, 108960, 1 },
    { 262254, 1, 3, 1, 1, 109760, 1 },
    { 262255, 1, 3, 1, 1, 110560, 1 },
    { 262256, 1, 3, 1, 1, 111360, 1 },
    { 262257, 1, 3, 1, 1, 112160, 1 },
    { 262258, 1, 3, 1, 1, 112960, 1 },
    { 262259, 1, 3, 1, 1, 113760, 1 },
    { 262260, 1, 3, 1, 1, 114560, 1 },
    { 262261, 1, 3, 1, 1, 115360, 1 },
    { 262262, 1, 3, 1, 1, 116160, 1 },
    { 262263, 2, 3, 1, 1, 126560, 1 },
    { 262264, 2, 3, 1, 1, 125760, 1 },
    { 262265, 2, 3, 1, 1, 124960, 1 },
    { 262266, 2, 3, 1, 1, 124160, 1 },
    { 262267, 2, 3, 1, 1, 123360, 1 },
    { 262268, 2, 3, 1, 1, 122560, 1 },
    { 262269, 2, 3, 1, 1, 121760, 1 },
    { 262270, 2, 3, 1, 1, 120960, 1 },
    { 262271, 2, 3, 1, 1, 120160, 1 },
    { 262272, 2, 3, 1, 1, 119360, 1 },
    { 262273, 2, 3, 1, 1, 118560, 1 },
    { 262274, 2, 3, 1, 1, 117760, 1 },
    { 262275, 2, 3, 1, 1, 116960, 1 },
    { 262276, 3, 3, 1, 1, 127360, 1 },
    { 262277, 3, 3, 1, 1, 128160, 1 },
    { 262278, 3, 3, 1, 1, 128960, 1 },
    { 262279, 3, 3, 1, 1, 129760, 1 },
    { 262280, 3, 3, 1, 1, 130560, 1 },
    { 262281, 3, 3, 1, 1, 131360, 1 },
    { 262282, 3, 3, 1, 1, 132160, 1 },
    { 262283, 3, 3, 1, 1, 132960, 1 },
    { 262284, 3, 3, 1, 1, 133760, 1 },
    { 262285, 3, 3, 1, 1, 134560, 1 },
    { 262286, 3, 3, 1, 1, 135360, 1 },
    { 262287, 3, 3, 1, 1, 136160, 1 },
    { 262288, 3, 3, 1, 1, 136960, 1 },
    { 262301, 0, 3, 1, 1, 95680, 1 },
    { 262300, 0, 3, 1, 1, 96480, 1 },
    { 262299, 0, 3, 1, 1, 97280, 1 },
    { 262298, 0, 3, 1, 1, 98080, 1 },
    { 262297, 0, 3, 1, 1, 98880, 1 },
    { 262296, 0, 3, 1, 1, 99680, 1 },
    { 262295, 0, 3, 1, 1, 100480, 1 },
    { 262294, 0, 3, 1, 1, 101280, 1 },
    { 262293, 0, 3, 1, 1, 102080, 1 },
    { 262292, 0, 3, 1, 1, 102880, 1 },
    { 262291, 0, 3, 1, 1, 103680, 1 },
    { 262290, 0, 3, 1, 1, 104480, 1 },
    { 262289, 0, 3, 1, 1, 105280, 1 },
    { 262302, 1, 3, 1, 1, 106080, 1 },
    { 262303, 1, 3, 1, 1, 106880, 1 },
    { 262304, 1, 3, 1, 1, 107680, 1 },
    { 262305, 1, 3, 1, 1, 108480, 1 },
    { 262306, 1, 3, 1, 1, 109280, 1 },
    { 262307, 1, 3, 1, 1, 110080, 1 },
    { 262308, 1, 3, 1, 1, 110880, 1 },
    { 262309, 1, 3, 1, 1, 111680, 1 },
    { 262310, 1, 3, 1, 1, 112480, 1 },
    { 262311, 1, 3, 1, 1, 113280, 1 },
    { 262312, 1, 3, 1, 1, 114080, 1 },
    { 262313, 1, 3, 1, 1, 114880, 1 },
    { 262314, 1, 3, 1, 1, 115680, 1 },
    { 262327, 2, 3, 1, 1, 116480, 1 },
    { 262326, 2, 3, 1, 1, 117280, 1 },
    { 262325, 2, 3, 1, 1, 118080, 1 },
    { 262324, 2, 3, 1, 1, 118880, 1 },
    { 262323, 2, 3, 1, 1, 119680, 1 },
    { 262322, 2, 3, 1, 1, 120480, 1 },
    { 262321, 2, 3, 1, 1, 121280, 1 },
    { 262320, 2, 3, 1, 1, 122080, 1 },
    { 262319, 2, 3, 1, 1, 122880, 1 },
    { 262318, 2, 3, 1, 1, 123680, 1 },
    { 262317, 2, 3, 1, 1, 124480, 1 },
    { 262316, 2, 3, 1, 1, 125280, 1 },
    { 262315, 2, 3, 1, 1, 126080, 1 },
    { 262328, 3, 3, 1, 1, 126880, 1 },
    { 262329, 3, 3, 1, 1, 127680, 1 },
    { 262330, 3, 3, 1, 1, 128480, 1 },
    { 262331, 3, 3, 1, 1, 129280, 1 },
    { 262332, 3, 3, 1, 1, 130080, 1 },
    { 262333, 3, 3, 1, 1, 130880, 1 },
    { 262334, 3, 3, 1, 1, 131680, 1 },
    { 262335, 3, 3, 1, 1, 132480, 1 },
    { 262336, 3, 3, 1, 1, 133280, 1 },
    { 262337, 3, 3, 1, 1, 134080, 1 },
    { 262338, 3, 3, 1, 1, 134880, 1 },
    { 262339, 3, 3, 1, 1, 135680, 1 },
    { 262340, 3, 3, 1, 1, 136480, 1 },
    { 327889, 0, 3, 2, 5, 95920, 1 },
    { 327888, 0, 3, 2, 5, 96720, 1 },
    { 327887, 0, 3, 2, 5, 97520, 1 },
    { 327886, 0, 3, 2, 5, 98320, 1 },
    { 327885, 0, 3, 2, 5, 99120, 1 },
    { 327884, 0, 3, 2, 5, 99920, 1 },
    { 327883, 0, 3, 2, 5, 100720, 1 },
    { 327882, 0, 3, 2, 5, 101520, 1 },
    { 327881, 0, 3, 2, 5, 102320, 1 },
    { 327880, 0, 3, 2, 5, 103120, 1 },
    { 327879, 0, 3, 2, 5, 103920, 1 },
    { 327878, 0, 3, 2, 5, 104720, 1 },
    { 327877, 0, 3, 2, 5, 105520, 1 },
    { 327890, 1, 3, 2, 5, 106320, 1 },
    { 327891, 1, 3, 2, 5, 107120, 1 },
    { 327892, 1, 3, 2, 5, 107920, 1 },
    { 327893, 1, 3, 2, 5, 108720, 1 },
    { 327894, 1, 3, 2, 5, 109520, 1 },
    { 327895, 1, 3, 2, 5, 110320, 1 },
    { 327896, 1, 3, 2, 5, 111120, 1 },
    { 327897, 1, 3, 2, 5, 111920, 1 },
    { 327898, 1, 3, 2, 5, 112720, 1 },
    { 327899, 1, 3, 2, 5, 113520, 1 },
    { 327900, 1, 3, 2, 5, 114320, 1 },
    { 327901, 1, 3, 2, 5, 115120, 1 },
    { 327902, 1, 3, 2, 5, 115920, 1 },
    { 327915, 2, 3, 2, 5, 116720, 1 },
    { 327914, 2, 3, 2, 5, 117520, 1 },
    { 327913, 2, 3, 2, 5, 118320, 1 },
    { 327912, 2, 3, 2, 5, 119120, 1 },
    { 327911, 2, 3, 2, 5, 119920, 1 },
    { 327910, 2, 3, 2, 5, 120720, 1 },
    { 327909, 2, 3, 2, 5, 121520, 1 },
    { 327908, 2, 3, 2, 5, 122320, 1 },
    { 327907, 2, 3, 2, 5, 123120, 1 },
    { 327906, 2, 3, 2, 5, 123920, 1 },
    { 327905, 2, 3, 2, 5, 124720, 1 },
    { 327904, 2, 3, 2, 5, 125520, 1 },
    { 327903, 2, 3, 2, 5, 126320, 1 },
    { 327916, 3, 3, 2, 5, 127120, 1 },
    { 327917, 3, 3, 2, 5, 127920, 1 },
    { 327918, 3, 3, 2, 5, 128720, 1 },
    { 327919, 3, 3, 2, 5, 129520, 1 },
    { 327920, 3, 3, 2, 5, 130320, 1 },
    { 327921, 3, 3, 2, 5, 131120, 1 },
    { 327922, 3, 3, 2, 5, 131920, 1 },
    { 327923, 3, 3, 2, 5, 132720, 1 },
    { 327924, 3, 3, 2, 5, 133520, 1 },
    { 327925, 3, 3, 2, 5, 134320, 1 },
    { 327926, 3, 3, 2, 5, 135120, 1 },
    { 327927, 3, 3, 2, 5, 135920, 1 },
    { 327928, 3, 3, 2, 5, 136720, 1 },
    { -2, -1, -1, -1, -1, -1, -1 },
};
/* ep1800 pins and nodes with the quadrant they belong to ('i' = global input) */
static const struct { int32_t pin; char quadrant; } k_ep1800_pins[0x50] = {
    { 14, 'i' },
    { 15, 'i' },
    { 16, 'i' },
    { 17, 'i' },
    { 19, 'i' },
    { 20, 'i' },
    { 21, 'i' },
    { 22, 'i' },
    { 48, 'i' },
    { 49, 'i' },
    { 50, 'i' },
    { 51, 'i' },
    { 53, 'i' },
    { 54, 'i' },
    { 55, 'i' },
    { 56, 'i' },
    { 2, 'a' },
    { 3, 'a' },
    { 4, 'a' },
    { 5, 'a' },
    { 6, 'a' },
    { 7, 'a' },
    { 8, 'a' },
    { 9, 'a' },
    { 10, 'a' },
    { 11, 'a' },
    { 12, 'a' },
    { 13, 'a' },
    { 69, 'a' },
    { 70, 'a' },
    { 71, 'a' },
    { 72, 'a' },
    { 23, 'b' },
    { 24, 'b' },
    { 25, 'b' },
    { 26, 'b' },
    { 27, 'b' },
    { 28, 'b' },
    { 29, 'b' },
    { 30, 'b' },
    { 31, 'b' },
    { 32, 'b' },
    { 33, 'b' },
    { 34, 'b' },
    { 73, 'b' },
    { 74, 'b' },
    { 75, 'b' },
    { 76, 'b' },
    { 36, 'c' },
    { 37, 'c' },
    { 38, 'c' },
    { 39, 'c' },
    { 40, 'c' },
    { 41, 'c' },
    { 42, 'c' },
    { 43, 'c' },
    { 44, 'c' },
    { 45, 'c' },
    { 46, 'c' },
    { 47, 'c' },
    { 77, 'c' },
    { 78, 'c' },
    { 79, 'c' },
    { 80, 'c' },
    { 57, 'd' },
    { 58, 'd' },
    { 59, 'd' },
    { 60, 'd' },
    { 61, 'd' },
    { 62, 'd' },
    { 63, 'd' },
    { 64, 'd' },
    { 65, 'd' },
    { 66, 'd' },
    { 67, 'd' },
    { 68, 'd' },
    { 81, 'd' },
    { 82, 'd' },
    { 83, 'd' },
    { 84, 'd' },
};

#define V5000_CELLS ((int)(sizeof(k_v5000_cells) / sizeof(k_v5000_cells[0])))
#define LOCAL_WIDTH 0x50        /* fuses of a local (quadrant) product term */
#define GLOBAL_WIDTH 0xb8       /* fuses of a global product term */

enum { VC_PIN, VC_QUADRANT, VC_LOCAL, VC_GLOBAL, VC_LAYOUT, VC_FUSE, VC_MODE };

static int32_t s_cells[V5000_CELLS][7];     /* working copy of the table */

void dev_reset(void)
{
    memcpy(s_cells, k_v5000_cells, sizeof(s_cells));
}

/* Index of the cell of a pin in s_cells, or -1 */
static int cell_index(unsigned pin)
{
    for (int i = 0; ((unsigned)s_cells[i][VC_PIN] & 0xffff) != 0xfffe; i++) {
        if (((unsigned)s_cells[i][VC_PIN] & 0xffff) == pin) {
            return i;
        }
    }
    return -1;
}

/* Quadrant of a pin (5 for pins outside the quadrants), or -1 */
static int quadrant_of(unsigned pin)
{
    int i = cell_index(pin);
    return i < 0 ? -1 : s_cells[i][VC_QUADRANT];
}

/* Mode word of a cell.  Some are addressed relative to another cell; an
 * index outside the table gives a scratch word. */
static int32_t *cell_mode(int index)
{
    static int32_t outside;
    if (index < 0 || index >= V5000_CELLS) {
        return &outside;
    }
    return &s_cells[index][VC_MODE];
}

static int term_is_false(const uint16_t *care, const uint16_t *val)
{
    return (term_bit(care, 0) && term_bit(val, 0)) || (term_bit(care, 1) && !term_bit(val, 1));
}

/* Can the term be a local term of the macrocell's quadrant?
 * Returns 0 if it needs a global term. */
static int term_is_local(const uint16_t *care, int owner_index)
{
    const symbol_t *owner = &g.syms[owner_index];
    int local = 1;
    if (node16(owner->or_index, NODE_MAX_TERMS) == 1 && node8(owner->or_index, NODE_ROW_WIDTH) == GLOBAL_WIDTH) {
        return 0;
    }
    if (term_bit(care, 0) || term_bit(care, 1)) {
        return 1;
    }
    for (int i = 0; i < g.nsyms; i++) {
        if (!term_bit(care, i)) {
            continue;
        }
        const symbol_t *s = &g.syms[i];
        if (s->pin_index < 0) {
            report_symbol_error(1008, s);
            continue;
        }
        const uint8_t *p = pin_rec(s->pin_index);
        if (p[PIN_TRUE_COL] < LOCAL_WIDTH && p[PIN_COMP_COL] < LOCAL_WIDTH) {
            int own = quadrant_of(owner->pin);
            int other = quadrant_of(rd16(p + PIN_NUMBER));
            if (other != 5 && other != own) {
                trace_printf("warning:  %s.%s  %s.%s in different quadrant\n", owner->name, SYM_EXT(owner), s->name,
                             SYM_EXT(s));
            }
        } else {
            local = 0;
        }
    }
    return local;
}

/* Give the cell the larger of its two term configurations */
static void enlarge_cell(int idx, int nlocal, int nglobal)
{
    int32_t *c = s_cells[idx];
    int32_t *mode = ((unsigned)c[VC_PIN] & 0xffff) < 0x44 ? &c[VC_MODE] : cell_mode(idx - 128);
    if (nglobal < 4 && nglobal + nlocal < 10) {
        c[VC_GLOBAL] = 3;
        c[VC_LOCAL] = 6;
        c[VC_LAYOUT] = 3;
        *mode = 0;
        c[VC_FUSE] -= 0xf0;
    } else {
        c[VC_GLOBAL] = 4;
        c[VC_LOCAL] = 9;
        c[VC_LAYOUT] = 4;
        *mode = 2;
        if (((unsigned)c[VC_PIN] & 0xffff) < 0x44) {
            c[VC_FUSE] -= 0xf0;
        }
    }
}

/* Choose the term configuration of the cells an array feeds */
static void route_array(const or_array_t *oa)
{
    for (const equation_t *eq = oa->eq; eq != NULL; eq = eq->next) {
        if (eq->nterms == 0 || node8(oa->sym->or_index, NODE_B24) == 3) {
            continue;
        }
        const symbol_t *s = &g.syms[eq->symbol];
        int nglobal = 0;
        int nlocal = 0;
        for (int t = 0; t < eq->nterms; t++) {
            if (term_is_local(TERM_CARE(eq, t), eq->symbol)) {
                nlocal++;
            } else {
                nglobal++;
            }
        }
        int idx = cell_index(s->pin);
        if (idx < 0) {
            continue;       /* pin without a cell */
        }
        int32_t *c = s_cells[idx];
        int fixed = node8(s->node_index, NODE_EXT) == 0 || node16(s->node_index, NODE_NUMBER) > 0x41;
        if (!fixed && nglobal <= 1 && nglobal + nlocal <= 4) {
            c[VC_FUSE] -= 0xf0;
            c[VC_LAYOUT] = 1;
            c[VC_GLOBAL] = 1;
            c[VC_LOCAL] = 3;
        } else if (!fixed && nglobal == 2 && nlocal + 2 <= 9) {
            c[VC_GLOBAL] = 3;
            c[VC_LOCAL] = 6;
            c[VC_LAYOUT] = 3;
            c[VC_MODE] = 0;
            c[VC_FUSE] -= 0xf0;
        } else if (c[VC_GLOBAL] < nglobal || c[VC_GLOBAL] + c[VC_LOCAL] < nglobal + nlocal) {
            enlarge_cell(idx, nlocal, nglobal);
        }
    }
}

/* First fuse of the next free global term of a cell */
static uint32_t next_global_row(int idx, const symbol_t *s)
{
    int32_t *c = s_cells[idx];
    uint32_t base = node32(s->or_index, NODE_ROW_FUSE);
    int offset;
    switch (c[VC_LAYOUT]) {
    case 0:
        c[VC_GLOBAL]--;
        offset = (1 - c[VC_GLOBAL]) + 1;
        break;
    case 1:
    case 2:
        c[VC_GLOBAL]--;
        offset = 0;
        break;
    case 3:
        c[VC_GLOBAL]--;
        offset = 2 - c[VC_GLOBAL];
        break;
    case 4:
        c[VC_GLOBAL]--;
        offset = 3 - c[VC_GLOBAL];
        break;
    case 5:
        c[VC_GLOBAL]--;
        offset = 1 - c[VC_GLOBAL];
        break;
    default:
        return (uint32_t)idx * 7;
    }
    return base + (uint32_t)(offset * GLOBAL_WIDTH);
}

/* First fuse of the next free local term of a cell */
static uint32_t next_local_row(int idx)
{
    int32_t *c = s_cells[idx];
    int first;
    switch (c[VC_LAYOUT]) {
    case 0:
    case 1:
    case 5:
        first = 2;
        break;
    case 2:
        first = 3;
        break;
    case 3:
        first = 5;
        break;
    case 4:
        first = 8;
        break;
    default:
        return (uint32_t)idx * 7;
    }
    c[VC_LOCAL]--;
    return (uint32_t)((first - c[VC_LOCAL]) * LOCAL_WIDTH + c[VC_FUSE]);
}

/* Row for the next term of a list; *local is cleared when a
 * local term has to use a global row.  Returns -1 if the cell is full. */
static int64_t next_row(int owner_index, int *local)
{
    const symbol_t *s = &g.syms[owner_index];
    int idx = cell_index(s->pin);
    if (idx < 0) {
        fatal_error(1026, "");
    }
    if (*local == 0 && s_cells[idx][VC_GLOBAL] > 0) {
        return next_global_row(idx, s);
    }
    if (*local != 0) {
        if (s_cells[idx][VC_LOCAL] > 0) {
            return next_local_row(idx);
        }
        if (s_cells[idx][VC_GLOBAL] > 0) {
            *local = 0;
            return next_global_row(idx, s);
        }
    }
    report_symbol_error(ERR_TOO_MANY_PTERMS, s);
    return -1;
}

/* Blow the fuses of a local or global product term */
static void blow_v5000_term(const uint16_t *care, const uint16_t *val, int owner_index, uint32_t fuse, int local)
{
    uint8_t row[256];
    uint8_t old[256];
    int width = local ? LOCAL_WIDTH : GLOBAL_WIDTH;
    if (row_state.stride > 200) {
        fatal_error(1025, "");
    }
    if (term_is_false(care, val)) {
        for (int i = 0; i < width; i++) {
            set_fuse(fuse + (uint32_t)i, 0);
        }
        return;
    }
    memset(row, 1, sizeof(row));
    if (term_bit(care, 0) || term_bit(care, 1)) {
        write_fuse_row(row, fuse, width);
        return;
    }
    if (row_state.fuse > attr32(ATTR_NFUSES)) {
        return;
    }
    int untouched = read_fuse_row(old, fuse, width);
    for (int i = 0; i < g.nsyms; i++) {
        if (!term_bit(care, i)) {
            continue;
        }
        const symbol_t *s = &g.syms[i];
        if (s->pin_index < 0) {
            report_symbol_error(1008, s);
            continue;
        }
        const uint8_t *p = pin_rec(s->pin_index);
        if (term_bit(val, i) == input_inverted(i, s->pin_index)) {
            row[p[PIN_TRUE_COL]] = 1;
            row[p[PIN_COMP_COL]] = 0;
        } else {
            row[p[PIN_TRUE_COL]] = 0;
            row[p[PIN_COMP_COL]] = 1;
        }
    }
    if (!untouched && memcmp(row, old, (size_t)width) != 0) {
        report_symbol_error(ERR_PINNODE_CONFLICT, &g.syms[owner_index]);
    }
    write_fuse_row(row, fuse, width);
}

/* Place the product terms of an array in its macrocell */
static void place_array(or_array_t *oa)
{
    int orn = oa->sym->or_index;
    row_state.and_width = (int)node8(orn, NODE_ROW_WIDTH);
    if (row_state.and_width == 0) {
        return;
    }
    int split = (int)node8(orn, NODE_SPLIT_TERMS);
    row_state.or_width = (int)node8(orn, NODE_OR_WIDTH);
    row_state.fuse = node32(orn, NODE_ROW_FUSE);
    row_state.second = (uint32_t)((row_state.and_width + row_state.or_width) * split) + row_state.fuse;
    row_state.or_fuse = 0;
    row_state.stride = row_state.and_width;
    int placed = 0;
    for (equation_t *eq = oa->eq; eq != NULL; eq = eq->next) {
        check_macrocell(eq->symbol);
        if (eq->nterms == 0) {
            continue;
        }
        const symbol_t *s = &g.syms[eq->symbol];
        int idx = cell_index(s->pin);
        unsigned number = node16(s->node_index, NODE_NUMBER);
        if (number > 0xc4) {
            *cell_mode(idx - 180) = 1;
        }
        if (number < 0x91 && number > 0x5c) {
            *cell_mode(idx - 76) &= ~2;
        }
        if (number < 0xc5 && number > 0x90) {
            *cell_mode(idx - 128) |= 1;
        }
        if (node8(orn, NODE_B24) == 3) {
            placed++;
            continue;
        }
        /* after "global feedback in local product term" t is not advanced:
         * the remaining passes of the loop look at the same term again.
         * This is deliberate; the error messages and the term count checked
         * below depend on it. */
        int t = 0;
        for (int pass = 0; pass < eq->nterms; pass++) {
            uint16_t *care = TERM_CARE(eq, t);
            if (!(care[0] & TERM_DELETED)) {
                placed++;
                int local = term_is_local(care, eq->symbol);
                if (local == 0 && node8(orn, NODE_ROW_WIDTH) == LOCAL_WIDTH) {
                    report_symbol_error(ERR_GLOBAL_FEEDBACK, s);
                    continue;
                }
                int64_t fuse;
                if (node16(orn, NODE_MAX_TERMS) == 1) {
                    fuse = node32(orn, NODE_ROW_FUSE);
                } else {
                    fuse = next_row(eq->symbol, &local);
                }
                if (fuse != -1) {
                    blow_v5000_term(care, TERM_VAL(eq, t), eq->symbol, (uint32_t)fuse, local);
                }
            }
            if (care[0] & TERM_SPLIT) {
                row_state.fuse = row_state.second;
            }
            t++;
        }
    }
    if (oa->nterms != placed) {
        fatal_error(1006, "");
    }
}

/* Mode fuses of the cells of pins 4 to 65 */
static void write_cell_modes(void)
{
    uint32_t fuse = 0x27b40;
    int idx = cell_index(4);
    if (idx < 0) {
        return;
    }
    for (; ((unsigned)s_cells[idx][VC_PIN] & 0xffff) < 0x42; idx++, fuse += 9) {
        unsigned mode = (unsigned)s_cells[idx][VC_MODE] & 0xff;
        set_fuse(fuse, mode & 1);
        set_fuse(fuse + 3, (mode >> 1) & 1);
    }
}

/* Product-term placement of the ATV5000, called while the fuse map is
 * built.  The arrays are placed by pin range: I/O cells of the quadrants,
 * buried cells, then the remaining pins and nodes. */
void v5000_build(void)
{
    static const struct {
        unsigned first, last;
        int route;
    } k_ranges[4] = { { 0x45, 0x90, 0 }, { 0x91, 0xc4, 1 }, { 0, 0x44, 1 }, { 0xc5, 0xffff, 0 } };
    for (int r = 0; r < 4; r++) {
        for (int i = 0; i < g.nor_arrays; i++) {
            or_array_t *oa = &g.or_arrays[i];
            if (oa->sym == NULL || oa->sym->or_index < 0 || oa->nterms <= 0 || oa->sym->pin < k_ranges[r].first
                || oa->sym->pin > k_ranges[r].last) {
                continue;
            }
            if (k_ranges[r].route && node8(oa->sym->node_index, NODE_B25) != 0) {
                route_array(oa);
            }
            place_array(oa);
        }
    }
    write_cell_modes();
}

/* -------------------------------------------------------------------------
 * EP1800
 * ------------------------------------------------------------------------- */

/* Quadrant letter of an EP1800 pin or node ('i' = global input) */
static int ep1800_quadrant(unsigned pin)
{
    for (int i = 0; i < 0x50; i++) {
        if ((unsigned)k_ep1800_pins[i].pin == pin) {
            return k_ep1800_pins[i].quadrant;
        }
    }
    return 0;       /* not in the table */
}

/* Inputs of a term must come from the quadrant of the macrocell
 * or be global */
static int ep1800_check_term(const uint16_t *care, int owner_index, int owner_quadrant)
{
    int errors = 0;
    if (term_bit(care, 0) || term_bit(care, 1)) {
        return 0;
    }
    for (int i = 0; i < g.nsyms; i++) {
        if (!term_bit(care, i)) {
            continue;
        }
        const symbol_t *s = &g.syms[i];
        if (s->pin_index < 0) {
            report_symbol_error(1008, s);
            continue;
        }
        int quadrant = ep1800_quadrant(s->pin);
        if (owner_quadrant != quadrant && quadrant != 'i' && pin8(s->pin_index, PIN_COMP_COL) < 0x18) {
            report_from_to_error(ERR_LOCAL_FEEDBACK, s, &g.syms[owner_index]);
            errors++;
        }
    }
    return errors;
}

/* Number of product terms of the array with an illegal feedback */
int ep1800_check_array(or_array_t *oa)
{
    int bad = 0;
    for (const equation_t *eq = oa->eq; eq != NULL; eq = eq->next) {
        if (eq->nterms == 0 || node8(oa->sym->or_index, NODE_B24) == 3) {
            continue;
        }
        int quadrant = ep1800_quadrant(g.syms[eq->symbol].pin);
        for (int t = 0; t < eq->nterms; t++) {
            if (ep1800_check_term(TERM_CARE(eq, t), eq->symbol, quadrant) != 0) {
                bad++;
            }
        }
    }
    return bad;
}
