/* FACI code-flash driver.
 *
 * This is the most dangerous code in the project: it programs the flash the device boots
 * from. Two structural facts drive its shape, both recovered from the stock firmware:
 *
 *   1. THE PROGRAMMING ROUTINES MUST EXECUTE FROM RAM. While code flash is in P/E mode it
 *      cannot serve instruction fetches, so any function running then — including its
 *      polling loops — must already be in RAM. The stock driver achieves this by linking
 *      the whole flash driver into .data; we mark the specific functions __ramfunc and let
 *      the linker script's .ramfunc section carry them, which is the same trick made
 *      explicit rather than incidental.
 *
 *   2. THE PROGRAM UNIT IS 256 BYTES, not the 128 that is the usual figure for this family.
 *      Confirmed three ways in the stock image: the on-chip product-info table, the
 *      halfword count written after the 0xE8 command, and the S-record applier's own
 *      256-byte staging buffer.
 *
 * Erase geometry (from the same product table): blocks 0-7 are 8 KB covering
 * 0x00000000-0x0000FFFF; everything from 0x00010000 to 0x003FFFFF is 32 KB blocks. Our
 * application region starts at 0x00120000 and is therefore entirely 32 KB blocks, and is
 * exactly block-aligned.
 */

#include "s7g2.h"
#include "flash_faci.h"
#include "app_launch.h"

/* FACI command-issuing area: commands are 8-bit stores, data is 16-bit stores. */
#define FACI_CMD8   (*(volatile uint8_t  *)0x407E0000UL)
#define FACI_CMD16  (*(volatile uint16_t *)0x407E0000UL)

/* FACI register block. */
#define FASTAT      0x407FE010UL   /* bit4 CMDLK */
#define FAEINT      0x407FE014UL
#define FRDYIE      0x407FE018UL
#define FSADDR      0x407FE030UL
#define FEADDR      0x407FE034UL
#define FSTATR      0x407FE080UL   /* b10 DBFULL, b12 PRGERR, b13 ERSERR, b15 FRDY */
#define FENTRYR     0x407FE084UL
#define FPESTAT     0x407FE0C0UL
#define FAWMON      0x407FE0DCUL
#define FCPSR       0x407FE0E0UL
#define FPCKAR      0x407FE0E4UL

#define FWEPROR     0x4001E416UL   /* flash P/E protect; must be 1 or every command fails */

#define FSTATR_DBFULL  (1UL << 10)
#define FSTATR_PRGERR  (1UL << 12)
#define FSTATR_ERSERR  (1UL << 13)
#define FSTATR_FRDY    (1UL << 15)

#define __ramfunc __attribute__((section(".ramfunc"), noinline, long_call))

/* Timeouts, in loop iterations. Derived the way the stock driver does: clock MHz * K / 3.
 * At 240 MHz ICLK these are generous; they exist to bound a hang, not to be precise. */
#define TMO_ERASE_32K   (240UL * 1040000UL / 3UL)
#define TMO_WRITE       (240UL *   15800UL / 3UL)
#define TMO_ENTRY       0x4B00UL

static uint8_t saved_cache;

/* ------------------------------------------------------------------ range guard */

/* Nothing outside the application region may ever be erased or written. Applied at every
 * entry point rather than once, because this is the last thing standing between a bad
 * offset from the host and a device that needs physical recovery. */
static int range_ok(uint32_t addr, uint32_t len)
{
    if (addr < APP_BASE_ADDR) return 0;
    if (addr + len < addr) return 0;
    if (addr + len > APP_BASE_ADDR + APP_MAX_BYTES) return 0;
    return 1;
}

/* ------------------------------------------------------------------ P/E mode */

__ramfunc static int pe_enter(void)
{
    /* Flash cache must be off before entering code-flash P/E, and invalidated after. */
    saved_cache = (uint8_t)(REG8(FCACHEE) & 1U);
    REG8(FCACHEE) = (uint8_t)(REG8(FCACHEE) & ~1U);

    REG8(FRDYIE) = 0;
    REG8(FAEINT) = 0;

    REG16(FENTRYR) = 0xAA01U;                     /* key 0xAA, code-flash P/E */
    for (uint32_t t = TMO_ENTRY; t; t--) {
        if (REG16(FENTRYR) == 0x0001U) return FLASH_OK;
    }
    return FLASH_ERR_TIMEOUT;
}

__ramfunc static int pe_exit(void)
{
    int rc = FLASH_OK;

    for (uint32_t t = TMO_ENTRY; t; t--) {
        if (REG32(FSTATR) & FSTATR_FRDY) break;
    }
    if (REG8(FASTAT) & 0x10U) {                   /* CMDLK: clear status */
        FACI_CMD8 = 0x50U;
        rc = FLASH_ERR_FACI;
    }

    REG16(FENTRYR) = 0xAA00U;                     /* back to read mode */
    for (uint32_t t = TMO_ENTRY; t; t--) {
        if (REG16(FENTRYR) == 0) break;
    }

    /* Invalidate and restore the flash cache, or the core will happily serve stale
     * instructions from the region we just rewrote. */
    REG8(FCACHEIV) = (uint8_t)(REG8(FCACHEIV) | 1U);
    for (uint32_t t = TMO_ENTRY; t; t--) {
        if (!(REG8(FCACHEIV) & 1U)) break;
    }
    REG8(FCACHEE) = (uint8_t)((REG8(FCACHEE) & ~1U) | saved_cache);

    return rc;
}

__ramfunc static int check_errors(void)
{
    if (REG8(FASTAT) & 0x10U) return FLASH_ERR_FACI;
    if (REG32(FSTATR) & (FSTATR_PRGERR | FSTATR_ERSERR)) return FLASH_ERR_FACI;
    return FLASH_OK;
}

/* Forced stop, used to recover a wedged FCU rather than leaving it locked. */
__ramfunc static void forced_stop(void)
{
    FACI_CMD8 = 0xB3U;
    for (uint32_t t = TMO_ENTRY; t; t--) {
        if (REG32(FSTATR) & FSTATR_FRDY) break;
    }
    FACI_CMD8 = 0x50U;
}

/* ------------------------------------------------------------------ erase */

__ramfunc static int erase_block_ram(uint32_t addr)
{
    REG16(FCPSR) = 1U;
    REG32(FSADDR) = addr;
    FACI_CMD8 = 0x20U;
    FACI_CMD8 = 0xD0U;

    for (uint32_t t = TMO_ERASE_32K; t; t--) {
        if (REG32(FSTATR) & FSTATR_FRDY) return check_errors();
    }
    forced_stop();
    return FLASH_ERR_TIMEOUT;
}

/* ------------------------------------------------------------------ write */

__ramfunc static int write_unit_ram(uint32_t addr, const uint8_t *data)
{
    REG32(FSADDR) = addr;
    FACI_CMD8 = 0xE8U;
    FACI_CMD8 = (uint8_t)(FLASH_WRITE_UNIT / 2);      /* halfword count: 128 */

    for (uint32_t i = 0; i < FLASH_WRITE_UNIT; i += 2) {
        FACI_CMD16 = (uint16_t)(data[i] | ((uint16_t)data[i + 1] << 8));
        for (uint32_t t = TMO_WRITE; t; t--) {
            if (!(REG32(FSTATR) & FSTATR_DBFULL)) break;
        }
    }

    FACI_CMD8 = 0xD0U;
    for (uint32_t t = TMO_WRITE; t; t--) {
        if (REG32(FSTATR) & FSTATR_FRDY) return check_errors();
    }
    forced_stop();
    return FLASH_ERR_TIMEOUT;
}

/* ------------------------------------------------------------------ public */

/* One-time setup. FPCKAR must match FCLK or every command fails in ways that are hard to
 * diagnose without a debugger. FCLK is 60 MHz here (SCKDIVCR 0x20011221, FCK = /4). */
static void faci_setup(void)
{
    REG8(FWEPROR) = 0x01U;
    REG16(FPCKAR) = (uint16_t)(0x1E00U | (F_FCLK_HZ / 1000000UL));   /* 0x1E3C */
}

/* Its own guard, deliberately not a widened range_ok(). See the header. */
static int record_range_ok(uint32_t addr, uint32_t len)
{
    if (addr < FLASH_RECORD_BASE) return 0;
    if (addr + len < addr) return 0;
    if (addr + len > FLASH_RECORD_BASE + FLASH_RECORD_BYTES) return 0;
    return 1;
}

int flash_record_write(uint32_t addr, const uint8_t *data)
{
    if (!record_range_ok(addr, FLASH_WRITE_UNIT)) return FLASH_ERR_RANGE;
    if (addr % FLASH_WRITE_UNIT)                  return FLASH_ERR_ALIGN;

    faci_setup();

    uint32_t primask;
    __asm__ volatile("mrs %0, primask" : "=r"(primask));
    __asm__ volatile("cpsid i" ::: "memory");

    int rc = pe_enter();
    if (rc == FLASH_OK) rc = write_unit_ram(addr, data);
    int rc2 = pe_exit();

    __asm__ volatile("msr primask, %0" :: "r"(primask) : "memory");
    if (rc != FLASH_OK) return rc;
    if (rc2 != FLASH_OK) return rc2;

    for (uint32_t i = 0; i < FLASH_WRITE_UNIT; i++) {
        if (REG8(addr + i) != data[i]) return FLASH_ERR_VERIFY;
    }
    return FLASH_OK;
}

int flash_record_erase(void)
{
    /* Belt and braces on a constant, because the cost of this one being wrong is a device that
     * cannot be recovered over USB. The block must sit inside our region, above the bootloader,
     * and below the application. */
    if (FLASH_RECORD_BASE % FLASH_ERASE_BLOCK)                       return FLASH_ERR_ALIGN;
    if (FLASH_RECORD_BASE < 0x00108000UL)                            return FLASH_ERR_RANGE;
    if (FLASH_RECORD_BASE + FLASH_RECORD_BYTES > APP_BASE_ADDR)      return FLASH_ERR_RANGE;

    faci_setup();

    uint32_t primask;
    __asm__ volatile("mrs %0, primask" : "=r"(primask));
    __asm__ volatile("cpsid i" ::: "memory");

    int rc = pe_enter();
    if (rc == FLASH_OK) rc = erase_block_ram(FLASH_RECORD_BASE);
    int rc2 = pe_exit();

    __asm__ volatile("msr primask, %0" :: "r"(primask) : "memory");
    return rc != FLASH_OK ? rc : rc2;
}

int flash_erase_app(void)
{
    faci_setup();

    /* A stale access window left behind by an interrupted update makes every erase fail
     * with CMDLK, and the symptom gives no hint why. Check before doing anything. */
    uint32_t awmon = REG32(FAWMON);
    uint32_t faws = awmon & 0x7FFU, fawe = (awmon >> 16) & 0x7FFU;
    if (faws != fawe) {
        /* A window is programmed. We deliberately do NOT clear it here: doing so is a
         * data-flash config-set that also carries BTFLG, and getting that wrong is worse
         * than refusing. Report it and let a human decide. */
        return FLASH_ERR_FACI;
    }

    uint32_t primask;
    __asm__ volatile("mrs %0, primask" : "=r"(primask));
    __asm__ volatile("cpsid i" ::: "memory");

    int rc = pe_enter();
    if (rc == FLASH_OK) {
        for (uint32_t a = APP_BASE_ADDR;
             a < APP_BASE_ADDR + APP_MAX_BYTES;
             a += FLASH_ERASE_BLOCK) {
            rc = erase_block_ram(a);
            if (rc != FLASH_OK) break;
        }
    }
    int rc2 = pe_exit();

    __asm__ volatile("msr primask, %0" :: "r"(primask) : "memory");
    return rc != FLASH_OK ? rc : rc2;
}

int flash_write_unit(uint32_t addr, const uint8_t *data)
{
    if (!range_ok(addr, FLASH_WRITE_UNIT)) return FLASH_ERR_RANGE;
    if (addr % FLASH_WRITE_UNIT)           return FLASH_ERR_ALIGN;

    faci_setup();

    uint32_t primask;
    __asm__ volatile("mrs %0, primask" : "=r"(primask));
    __asm__ volatile("cpsid i" ::: "memory");

    int rc = pe_enter();
    if (rc == FLASH_OK) rc = write_unit_ram(addr, data);
    int rc2 = pe_exit();

    __asm__ volatile("msr primask, %0" :: "r"(primask) : "memory");
    if (rc != FLASH_OK) return rc;
    if (rc2 != FLASH_OK) return rc2;

    /* Read back. Code flash reads normally outside P/E mode, and the stock updater's lack
     * of any verification is one of the two sharp edges we are deliberately not copying. */
    for (uint32_t i = 0; i < FLASH_WRITE_UNIT; i++) {
        if (REG8(addr + i) != data[i]) return FLASH_ERR_VERIFY;
    }
    return FLASH_OK;
}

/* CRC-32 IEEE, table-free to keep the bootloader small. ~8 cycles a byte at 240 MHz. */
uint32_t flash_crc32(uint32_t addr, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFUL;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= REG8(addr + i);
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320UL & (uint32_t)(-(int32_t)(crc & 1)));
        }
    }
    return ~crc;
}
