/*
 * ascii.h - character classes of the "C" locale for the compiler, the
 * simulator and the fitter.
 *
 * Unlike <ctype.h> these never depend on the locale and take any int: the
 * argument is reduced to a byte, so a plain (signed) char works as well, and
 * bytes from 0x80 up are in no class.  to_upper / to_lower keep EOF (-1) and
 * otherwise return the byte, changed only for ASCII letters.
 */
#ifndef LINCUPL_ASCII_H
#define LINCUPL_ASCII_H

static inline int is_digit(int c) { c &= 0xff; return c >= '0' && c <= '9'; }
static inline int is_lower(int c) { c &= 0xff; return c >= 'a' && c <= 'z'; }
static inline int is_upper(int c) { c &= 0xff; return c >= 'A' && c <= 'Z'; }
static inline int is_alpha(int c) { return is_lower(c) || is_upper(c); }
static inline int is_alnum(int c) { return is_alpha(c) || is_digit(c); }
static inline int is_space(int c) { c &= 0xff; return c == ' ' || (c >= '\t' && c <= '\r'); }
static inline int is_print(int c) { c &= 0xff; return c >= 0x20 && c <= 0x7e; }

static inline int to_upper(int c)
{
    if (c == -1)
        return -1;
    return is_lower(c) ? (c & 0xff) - 'a' + 'A' : (c & 0xff);
}

static inline int to_lower(int c)
{
    if (c == -1)
        return -1;
    return is_upper(c) ? (c & 0xff) - 'A' + 'a' : (c & 0xff);
}

#endif
