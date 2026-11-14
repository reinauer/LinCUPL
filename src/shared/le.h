/*
 * le.h - little-endian fields in byte buffers.
 *
 * The device libraries, the intermediate files of the compiler phases and
 * the absolute file store their numbers as little-endian 16- and 32-bit
 * words at arbitrary byte offsets.  These helpers read and write them
 * independently of the host's byte order and alignment.
 */
#ifndef LINCUPL_LE_H
#define LINCUPL_LE_H

#include <stdint.h>

static inline unsigned rd16(const uint8_t *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

/* a 16-bit word read as a signed number */
static inline int rd16s(const uint8_t *p)
{
    return (int16_t)rd16(p);
}

static inline uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void wr16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

#endif /* LINCUPL_LE_H */
