/*
 * devlib.h - reading the device libraries (.dl).
 *
 * A library is a 199-byte header, a directory of 1000 entries of 26 bytes
 * and the device images the entries point at:
 *
 *   header  +0x00 magic number (DL_MAGIC)
 *           +0x02 access key (DL_ACCESS_KEY)
 *           +0x04 library name, +0x15 revision, +0x26 description
 *   entry   +0x00 device name, up to 16 characters
 *           +0x11 flags; bit 0 marks an entry in use
 *           +0x12 size of the device image
 *           +0x16 file offset of the device image
 *   image   13 section headers of 20 bytes (count at +4, record size at +6,
 *           offset of the records within the image at +8), then the records;
 *           the device information record (section 3) is located through
 *           the offset at DL_INFO_OFFSET.
 *
 * The callers open the file themselves (each with its own bookkeeping of
 * open files) and decide what counts as a usable image: some insist on the
 * whole image, others are content with part of it.
 */
#ifndef LINCUPL_DEVLIB_H
#define LINCUPL_DEVLIB_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "le.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DL_HEADER_SIZE 199
#define DL_MAGIC 0xd431
#define DL_ACCESS_KEY 0x28
#define DL_MAX_ENTRIES 1000
#define DL_ENTRY_SIZE 26
#define DL_ENTRY_NAME_SIZE 16
#define DL_ENTRY_FLAGS 0x11
#define DL_ENTRY_LENGTH 0x12
#define DL_ENTRY_OFFSET 0x16
#define DL_DATA_START (DL_HEADER_SIZE + DL_MAX_ENTRIES * DL_ENTRY_SIZE)
#define DL_SECTIONS 13
#define DL_SECTION_HEADER_SIZE 20
#define DL_INFO_OFFSET 0x44         /* image offset of the device information */
#define DL_READ_BLOCK 0x8000        /* images are read in blocks of this size */

/* dl_read_header() results; DL_CANNOT_OPEN is for callers that report a
 * failed open together with them */
enum {
    DL_OK = 0,
    DL_CANNOT_OPEN = 1,
    DL_NO_HEADER = 2,               /* file shorter than the header */
    DL_BAD_MAGIC = 3,
    DL_BAD_KEY = 5                  /* wrong access key */
};

/* how dl_find_device() compares device names */
enum {
    DL_MATCH_CASE = 0,
    DL_IGNORE_CASE = 1
};

/* Read and check the library header (DL_HEADER_SIZE bytes) */
int dl_read_header(FILE *fp, uint8_t *header);

/* Search the directory for an entry in use whose name is `name`; the entry
 * is left in `entry` (DL_ENTRY_SIZE bytes).  Returns 1 if found. */
int dl_find_device(FILE *fp, const char *name, int match, uint8_t *entry);

/* Position the file at the image of a directory entry; 0 on failure */
int dl_seek_image(FILE *fp, const uint8_t *entry);

/* Read up to `size` bytes of the image at the file position; returns the
 * number of bytes read, which is less than `size` if the file ends early */
size_t dl_read_image(FILE *fp, uint8_t *image, size_t size);

/* Read an image at the current file position and apply known corrections
 * to the in-memory device description. Use this when interpreting a design;
 * dl_read_image() remains the raw reader for library copying/editing. */
size_t dl_read_device_image(FILE *fp, const uint8_t *entry, uint8_t *image, size_t size);

static inline uint32_t dl_image_size(const uint8_t *entry)
{
    return rd32(entry + DL_ENTRY_LENGTH);
}

static inline uint32_t dl_image_offset(const uint8_t *entry)
{
    return rd32(entry + DL_ENTRY_OFFSET);
}

/* The compiler phases accept an image of `size` bytes of which `got` were
 * read when its first block is complete. */
static inline int dl_first_block_read(size_t got, size_t size)
{
    return size != 0 && got >= (size < DL_READ_BLOCK ? size : DL_READ_BLOCK);
}

#ifdef __cplusplus
}
#endif

#endif /* LINCUPL_DEVLIB_H */
