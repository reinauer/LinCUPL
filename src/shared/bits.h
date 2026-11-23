/*
 * bits.h - bit counting for the cube and bit-vector code of the minimizers.
 */
#ifndef LINCUPL_BITS_H
#define LINCUPL_BITS_H

#include <stdint.h>

/* number of 1 bits of a 32-bit word (GCC and Clang compile this to a
 * single instruction where the processor has one) */
static inline int lincupl_popcount32(uint32_t v)
{
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcount(v);
#else
    /* Sum adjacent bits, then pairs, nibbles and bytes. */
    v -= (v >> 1) & UINT32_C(0x55555555);
    v = (v & UINT32_C(0x33333333)) + ((v >> 2) & UINT32_C(0x33333333));
    v = (v + (v >> 4)) & UINT32_C(0x0f0f0f0f);
    return (int)((v * UINT32_C(0x01010101)) >> 24);
#endif
}

#endif
