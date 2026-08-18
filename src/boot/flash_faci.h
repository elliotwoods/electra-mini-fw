/* Internal code-flash programming, via the Renesas FACI.
 *
 * This is the dangerous part of the bootloader: it writes the flash the device boots from.
 * It only ever touches the APPLICATION region (0x00120000 upward). Erasing or writing
 * anything below that would destroy either our bootloader or Electra's, so the range check
 * is not advisory — it is the last line of defence and every entry point applies it.
 */

#ifndef ELECTRA_FLASH_FACI_H
#define ELECTRA_FLASH_FACI_H

#include <stdint.h>

/* Code-flash program unit. The FACI writes whole units; a partial unit is not expressible,
 * so callers must pad.
 *
 * 256, NOT the 128 that is the usual figure for this family. Confirmed three ways in the
 * stock image: the on-chip product-info table at 0x00372814, the halfword count written
 * after the 0xE8 command, and the S-record applier's own 256-byte staging buffer. */
#define FLASH_WRITE_UNIT   256

/* Erase-block size for our region. Blocks 0-7 (0x00000000-0x0000FFFF) are 8 KB; everything
 * from 0x00010000 up is 32 KB. The application region starts at 0x00120000, so it is
 * entirely 32 KB blocks and exactly block-aligned. */
#define FLASH_ERASE_BLOCK  32768

/* Return codes. Negative is failure; the specific value says where it failed, because a
 * bootloader with no debugger needs its errors to be distinguishable over the wire. */
#define FLASH_OK              0
#define FLASH_ERR_RANGE      -1   /* target outside the application region — refused */
#define FLASH_ERR_ALIGN      -2   /* address or length not on a unit/block boundary */
#define FLASH_ERR_TIMEOUT    -3   /* FRDY never asserted */
#define FLASH_ERR_FACI       -4   /* ILGLERR / ERSERR / PRGERR reported by the FCU */
#define FLASH_ERR_VERIFY     -5   /* read-back did not match */
#define FLASH_ERR_UNSUPPORTED -6  /* driver not implemented yet */

/* --- the record block -------------------------------------------------------------------
 *
 * One 32 KB erase block inside OUR bootloader region, reserved for state that has to survive
 * losing power. Everything below 0x00120000 belongs to us: our bootloader occupies the first
 * 10 KB and the rest is dead remnants of the stock application that used to live at this
 * address, which nothing reads and nothing executes.
 *
 * Why not the data flash, which is what a persistent record is nominally for: it is already
 * fully in use. A dump of all 64 KB reads about 54% non-erased, uniformly, with the repeating
 * structure of a wear-levelled E2 emulation area -- and its first kilobyte holds the stock
 * firmware's USB descriptor strings ("Electra Controller", "Electra Port 1"). Claiming it would
 * break the stock firmware's identity if this device were ever reverted, which is exactly the
 * kind of one-way door the recovery design exists to avoid.
 *
 * Code flash endures far fewer erase cycles than data flash -- of order a thousand rather than
 * a hundred thousand -- so this block is written as an APPEND-ONLY LOG rather than rewritten in
 * place: 128 entries of one program unit each, and the block is erased only when it fills. That
 * turns a thousand erases into a hundred thousand writes, which is far past anything a boot
 * record will ever need.
 *
 * The address is deliberately the LAST block of the region, as far from the running bootloader
 * as the region allows. Erasing the block the bootloader is executing from would be fatal and
 * unrecoverable over USB. */
#define FLASH_RECORD_BASE   0x00118000UL
#define FLASH_RECORD_BYTES  FLASH_ERASE_BLOCK
#define FLASH_RECORD_SLOTS  (FLASH_RECORD_BYTES / FLASH_WRITE_UNIT)   /* 128 */

/* Write one program unit into the record block. Separately guarded from the application-region
 * entry points rather than sharing a widened range check: the guard on flash_write_unit is the
 * last thing standing between a bad offset and a device that needs physical recovery, and
 * loosening it to admit a second region would weaken it for the first. */
int flash_record_write(uint32_t addr, const uint8_t *data);

/* Erase the record block. Refuses if the address arithmetic would touch anything else. */
int flash_record_erase(void);

/* Erase the whole application region. Slow (seconds); the caller should expect that. */
int flash_erase_app(void);

/* Write exactly FLASH_WRITE_UNIT bytes at `addr`, which must be unit-aligned and inside
 * the application region. */
int flash_write_unit(uint32_t addr, const uint8_t *data);

/* CRC-32 (IEEE, the zlib polynomial) over a flash range, for host-side verification. */
uint32_t flash_crc32(uint32_t addr, uint32_t len);

#endif /* ELECTRA_FLASH_FACI_H */
