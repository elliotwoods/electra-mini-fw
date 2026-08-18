/* Append-only boot record in flash. See persist.h for why it exists and why it lives there. */

#include <string.h>
#include "s7g2.h"
#include "persist.h"
#include "flash_faci.h"

static const persist_rec_t *slot(unsigned i)
{
    return (const persist_rec_t *)(FLASH_RECORD_BASE + (uint32_t)i * FLASH_WRITE_UNIT);
}

/* An erased program unit reads as all-ones. Used to find the append point without keeping any
 * state of our own -- the flash IS the state, which is the point of an append-only log. */
static int slot_erased(unsigned i)
{
    const uint8_t *p = (const uint8_t *)slot(i);
    for (uint32_t b = 0; b < FLASH_WRITE_UNIT; b++) {
        if (p[b] != 0xFFu) return 0;
    }
    return 1;
}

int persist_read(persist_rec_t *out)
{
    if (!out) return 0;

    /* Scan forward and keep the last valid entry rather than stopping at the first erased slot.
     * A power loss partway through an append can leave a torn entry with erased slots on either
     * side of good ones, and stopping early would then read a stale record as current. */
    int found = 0;
    for (unsigned i = 0; i < FLASH_RECORD_SLOTS; i++) {
        const persist_rec_t *r = slot(i);
        if (persist_valid(r)) { *out = *r; found = 1; }
    }
    return found;
}

int persist_write(const persist_rec_t *rec)
{
    uint8_t unit[FLASH_WRITE_UNIT];

    unsigned i = 0;
    while (i < FLASH_RECORD_SLOTS && !slot_erased(i)) i++;

    if (i >= FLASH_RECORD_SLOTS) {
        /* Full. Erasing loses the history, which is fine -- only the newest entry has ever
         * meant anything; the older ones exist because appending is cheaper than rewriting. */
        int rc = flash_record_erase();
        if (rc != FLASH_OK) return rc;
        i = 0;
    }

    /* Pad with 0xFF rather than zero, so an unused tail is indistinguishable from erased flash
     * and a future format can extend the structure without rewriting this block. */
    memset(unit, 0xFF, sizeof(unit));
    memcpy(unit, rec, sizeof(*rec));

    return flash_record_write(FLASH_RECORD_BASE + (uint32_t)i * FLASH_WRITE_UNIT, unit);
}

/* Does the stored record describe the image that is actually in flash right now? */
static int matches(const persist_rec_t *r, uint32_t app_crc, uint32_t app_len)
{
    return r->app_crc == app_crc && r->app_len == app_len;
}

int persist_note_launch(uint32_t app_crc, uint32_t app_len)
{
    persist_rec_t r;
    int have = persist_read(&r);

    if (have && matches(&r, app_crc, app_len) && r.proven == PERSIST_PROVEN) {
        /* Already trusted. Writing on every boot of a working device would burn the block for
         * no information, so this is the case that must cost nothing. */
        return FLASH_OK;
    }

    if (!have || !matches(&r, app_crc, app_len)) {
        /* A different image from the one on record -- a fresh flash. It starts unproven with a
         * clean count, which is the whole reason the record is keyed to the CRC: inheriting the
         * previous image's proof would disarm the crash-loop protection exactly when a newly
         * flashed image needs it most. */
        memset(&r, 0, sizeof(r));
        r.magic   = PERSIST_MAGIC;
        r.app_crc = app_crc;
        r.app_len = app_len;
        r.proven  = 0;
        r.launches = 0;
    }

    r.launches++;
    r.checksum = persist_checksum(&r);
    return persist_write(&r);
}

int persist_mark_proven(uint32_t app_crc, uint32_t app_len)
{
    persist_rec_t r;
    int have = persist_read(&r);

    if (have && matches(&r, app_crc, app_len) && r.proven == PERSIST_PROVEN) return FLASH_OK;

    memset(&r, 0, sizeof(r));
    r.magic    = PERSIST_MAGIC;
    r.app_crc  = app_crc;
    r.app_len  = app_len;
    r.launches = 0;                 /* proven, so the attempt count has done its job */
    r.proven   = PERSIST_PROVEN;
    r.checksum = persist_checksum(&r);
    return persist_write(&r);
}
