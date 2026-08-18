/* Boot state that survives losing power.
 *
 * The device could not boot without a host. The bootloader refuses to launch an application it
 * has no evidence for, and that evidence lived in SRAM -- so a cold power-on always found
 * `health == 0` and held in the bootloader forever. On a bench that is an inconvenience; for an
 * instrument it means the thing does not turn on.
 *
 * The evidence has to be non-volatile, and it has to be PER IMAGE. A flag that merely says
 * "some application once worked here" would let a freshly flashed broken image inherit the
 * previous one's proof, which is worse than no flag: the crash-loop protection would be
 * disarmed exactly when it was needed. So the record carries the CRC of the image it vouches
 * for, and the bootloader only believes it if the image still matches.
 *
 * Storage is an APPEND-ONLY LOG in one 32 KB erase block of our own bootloader region -- see
 * FLASH_RECORD_BASE in flash_faci.h for why there and not the data flash. Each entry is one
 * program unit; the newest valid entry wins; the block is erased only when it fills. Code flash
 * endures far fewer erase cycles than data flash, and appending is what makes that affordable.
 *
 * Deliberately shared between the application and the bootloader: they must agree byte for byte
 * about this structure, and the surest way to guarantee that is one definition.
 */

#ifndef ELECTRA_PERSIST_H
#define ELECTRA_PERSIST_H

#include <stdint.h>

#define PERSIST_MAGIC   0x50524331UL      /* "PRC1" */

/* An image is believed only while all three agree with what is actually in flash. */
typedef struct {
    uint32_t magic;
    uint32_t app_crc;      /* CRC-32 of the application image this record vouches for */
    uint32_t app_len;      /* how many bytes that CRC covers */
    uint32_t launches;      /* attempts since this image last proved itself */
    uint32_t proven;        /* PERSIST_PROVEN once it has run and reported in */
    uint32_t checksum;      /* ~sum of the five above; a half-written entry must not read valid */
} persist_rec_t;

#define PERSIST_PROVEN  0x4F4B4159UL      /* "OKAY" */

/* How many launches of an UNPROVEN image before the bootloader stops trying. Deliberately
 * larger than the SRAM counter's limit: a cold-boot crash loop now costs a flash write per
 * attempt, so the count wants to be meaningful rather than merely small. */
#define PERSIST_LAUNCH_LIMIT 5u

static inline uint32_t persist_checksum(const persist_rec_t *r)
{
    return ~(r->magic + r->app_crc + r->app_len + r->launches + r->proven);
}

static inline int persist_valid(const persist_rec_t *r)
{
    return r->magic == PERSIST_MAGIC && r->checksum == persist_checksum(r);
}

/* Newest valid entry, or zero if the block holds none. Read-only; safe from either image and
 * at any time, since it only reads flash. */
int persist_read(persist_rec_t *out);

/* Append an entry. Erases and restarts the block when it is full. Returns a FLASH_* code. */
int persist_write(const persist_rec_t *rec);

/* Record that this image is about to be launched, so a crash loop is bounded across power
 * cycles. No-op once the image is proven -- that is what keeps the write count near zero in
 * normal use. */
int persist_note_launch(uint32_t app_crc, uint32_t app_len);

/* Record that this image has run and reported itself working. */
int persist_mark_proven(uint32_t app_crc, uint32_t app_len);

#endif /* ELECTRA_PERSIST_H */
