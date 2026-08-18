/* See diag.h for why this records rather than sends, and why repeats coalesce. */

#include <string.h>

#include "emp.h"
#include "diag.h"

void emp_send(uint8_t channel, uint8_t opcode, const uint8_t *payload, uint32_t len);

/* One slot per distinct code in flight. Sized to the number of codes that can plausibly be
 * raised between two service ticks, not to the number that exists: a burst is nearly always one
 * code repeated, which is the case the coalescing handles. */
#define DIAG_SLOTS 8u

typedef struct {
    const char *detail;
    uint32_t    context;    /* from the FIRST occurrence; later ones only add to the count */
    uint32_t    count;
    uint16_t    code;
    uint8_t     severity;
    uint8_t     used;
} diag_slot_t;

static diag_slot_t slots[DIAG_SLOTS];
static uint32_t    raised[3];
static uint32_t    overflowed;

void emp_diag(uint8_t severity, uint16_t code, uint32_t context, const char *detail)
{
    if (severity > EMP_SEV_ERROR) severity = EMP_SEV_ERROR;
    raised[severity]++;

    for (unsigned i = 0; i < DIAG_SLOTS; i++) {
        if (slots[i].used && slots[i].code == code) {
            slots[i].count++;
            /* Keep the highest severity seen: the same code raised as WARN and then as ERROR
             * is an ERROR, and reporting the first one would understate it. */
            if (severity > slots[i].severity) slots[i].severity = severity;
            return;
        }
    }

    for (unsigned i = 0; i < DIAG_SLOTS; i++) {
        if (!slots[i].used) {
            slots[i].used     = 1;
            slots[i].code     = code;
            slots[i].severity = severity;
            slots[i].context  = context;
            slots[i].detail   = detail;
            slots[i].count    = 1;
            return;
        }
    }

    /* Every slot busy. Losing a diagnostic quietly would be the exact failure this file exists
     * to end, so the loss itself is reported on the next flush. */
    overflowed++;
}

static void send_one(uint8_t severity, uint16_t code, uint32_t context,
                     uint32_t count, const char *detail)
{
    /* severity u8, code u16, context u32, count u32, detail String -- docs/protocol.md 3.1. */
    uint8_t buf[96];
    uint32_t n = 0;

    buf[n++] = severity;
    buf[n++] = (uint8_t)code;
    buf[n++] = (uint8_t)(code >> 8);
    buf[n++] = (uint8_t)context;
    buf[n++] = (uint8_t)(context >> 8);
    buf[n++] = (uint8_t)(context >> 16);
    buf[n++] = (uint8_t)(context >> 24);
    buf[n++] = (uint8_t)count;
    buf[n++] = (uint8_t)(count >> 8);
    buf[n++] = (uint8_t)(count >> 16);
    buf[n++] = (uint8_t)(count >> 24);

    uint32_t len = 0;
    if (detail) { while (detail[len] && len < sizeof(buf) - 13u) len++; }
    buf[n++] = (uint8_t)len;
    buf[n++] = (uint8_t)(len >> 8);
    if (len) memcpy(buf + n, detail, len);
    n += len;

    emp_send(EMP_CH_CONTROL, EMP_OP_DIAG, buf, n);
}

void emp_diag_tick(void)
{
    for (unsigned i = 0; i < DIAG_SLOTS; i++) {
        if (!slots[i].used) continue;
        send_one(slots[i].severity, slots[i].code, slots[i].context,
                 slots[i].count, slots[i].detail);
        slots[i].used = 0;
    }

    if (overflowed) {
        send_one(EMP_SEV_WARN, EMP_DIAG_OVERFLOW, overflowed, overflowed,
                 "diagnostics lost");
        overflowed = 0;
    }
}

/* Throw away what is pending without sending it, keeping the running totals.
 *
 * For losing the host: the diagnostics in flight describe a link that no longer exists, and the
 * counts are still worth having, since "this device has complained 400 times" remains true
 * across a reconnect. */
void emp_diag_drop_pending(void)
{
    memset(slots, 0, sizeof(slots));
    overflowed = 0;
}

void emp_diag_reset(void)
{
    memset(slots, 0, sizeof(slots));
    memset(raised, 0, sizeof(raised));
    overflowed = 0;
}

uint32_t emp_diag_count(uint8_t severity)
{
    return (severity <= EMP_SEV_ERROR) ? raised[severity] : 0;
}
