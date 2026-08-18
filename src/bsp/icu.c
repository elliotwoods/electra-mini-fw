/* ICU event-link setup.
 *
 * On the S7G2 the NVIC does not know what a peripheral is. Each NVIC slot is wired to a
 * peripheral event by writing that event's number into the corresponding IELSR register.
 * Enabling an NVIC interrupt without doing this produces a peripheral that never fires and
 * no diagnostic of any kind — which is why this file exists rather than being folded into
 * whichever driver needs it.
 *
 * The table is the stock firmware's, transcribed from 0x00100500 (25 entries, 8 bytes each).
 * We keep the full set even though our firmware will not use SCI or DMAC immediately: the
 * slot-to-vector mapping is what the vector table is built against, and diverging from it
 * would mean rebuilding both together for no benefit.
 */

#include "s7g2.h"
#include "bsp.h"

typedef struct {
    uint16_t event;     /* IELSR value, masked to 10 bits */
    const char *what;   /* for readability only; the compiler folds it away if unused */
} icu_slot_t;

static const icu_slot_t icu_slots[ICU_IELSR_COUNT] = {
    { 0x020, "DMAC ch0"  },   /* IRQ 0  */
    { 0x021, "DMAC ch1"  },   /* IRQ 1  */
    { 0x022, "DMAC ch2"  },   /* IRQ 2  */
    { 0x1FD, "DRW 2D"    },   /* IRQ 3  */
    { 0x007, "FCU?"      },   /* IRQ 4  — module id 0x06, identity inferred */
    { 0x008, "FCU?"      },   /* IRQ 5  */
    { 0x066, "RIIC0"     },   /* IRQ 6  — pot touch bus */
    { 0x063, "RIIC0"     },   /* IRQ 7  */
    { 0x065, "RIIC0"     },   /* IRQ 8  */
    { 0x064, "RIIC0"     },   /* IRQ 9  */
    { 0x070, "RIIC2"     },   /* IRQ 10 — LCD touch bus */
    { 0x06D, "RIIC2"     },   /* IRQ 11 */
    { 0x06F, "RIIC2"     },   /* IRQ 12 */
    { 0x06E, "RIIC2"     },   /* IRQ 13 */
    { 0x189, "SCI3"      },   /* IRQ 14 — MIDI DIN */
    { 0x186, "SCI3"      },   /* IRQ 15 */
    { 0x188, "SCI3"      },   /* IRQ 16 */
    { 0x187, "SCI3"      },   /* IRQ 17 */
    { 0x19B, "SCI6"      },   /* IRQ 18 */
    { 0x198, "SCI6"      },   /* IRQ 19 */
    { 0x19A, "SCI6"      },   /* IRQ 20 */
    { 0x199, "SCI6"      },   /* IRQ 21 */
    { 0x1C7, "SDHI0"     },   /* IRQ 22 */
    { 0x061, "USB"       },   /* IRQ 23 */
    { 0x173, "USB"       },   /* IRQ 24 */
};

void bsp_icu_init(void)
{
    for (unsigned i = 0; i < ICU_IELSR_COUNT; i++) {
        /* Full 32-bit store: this also clears IR, DTCE and DIM, which is what we want
         * from a cold start and what the stock firmware does. */
        REG32(ICU_IELSR + 4U * i) = (uint32_t)(icu_slots[i].event & 0x3FFU);
    }
}
