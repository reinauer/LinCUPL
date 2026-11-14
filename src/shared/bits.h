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
    return __builtin_popcount(v);
}

#endif
