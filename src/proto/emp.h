/* EMP/1 — Electra Mini Protocol, shared constants.
 *
 * Normative definition is docs/protocol.md. Where this header and that document disagree, the
 * document is right and this header is a bug. The Rust codec in the host crate is written
 * against the same document, and both sides pin the same constants in tests so a drift is a
 * test failure rather than a field failure.
 *
 * Nothing here does I/O. The whole codec is pure functions over caller-owned buffers, which is
 * what lets it compile and run on the host for testing — a protocol that can only be exercised
 * on the device is a protocol that will not be exercised.
 */

#ifndef ELECTRA_EMP_H
#define ELECTRA_EMP_H

#include <stdint.h>

#define EMP_MAGIC            0xE1u
#define EMP_VERSION_MAJOR    1u
#define EMP_HEADER_BYTES     8u
#define EMP_PREFIX_BYTES     8u          /* total_len u32 + crc32c u32, on FIRST && !LAST */

#define EMP_MTU_HID          56u
#define EMP_MTU_BULK         1016u
#define EMP_MAX_MESSAGE      65536u      /* protocol ceiling */
#define EMP_MAX_MESSAGE_RX   8192u       /* what THIS device advertises it will accept */
#define EMP_MAX_STRING_BYTES 512u

/* Fragment flags. */
#define EMP_FLAG_FIRST       0x01u
#define EMP_FLAG_LAST        0x02u

/* Channels. 0 is reserved for control and is never allowed to be starved by bulk traffic. */
#define EMP_CH_CONTROL       0u
#define EMP_CH_SURFACE       1u
#define EMP_CH_INPUT         2u

/* Control, single-fragment only (rule F1). */
#define EMP_OP_HELLO         0x01u
#define EMP_OP_READY         0x02u
#define EMP_OP_PING          0x03u
#define EMP_OP_PONG          0x04u
#define EMP_OP_HEARTBEAT     0x05u
#define EMP_OP_DIAG          0x06u
#define EMP_OP_FLOW          0x07u
#define EMP_OP_BYE           0x08u
#define EMP_OP_ECHO          0x0Eu
#define EMP_OP_ECHO_REPLY    0x0Fu

/* Surface, host -> device. */
#define EMP_OP_DESC_BEGIN    0x20u
#define EMP_OP_DESC_FIELD    0x21u
#define EMP_OP_DESC_END      0x22u
#define EMP_OP_DESC_ABORT    0x23u
#define EMP_OP_VALUES        0x24u
#define EMP_OP_REVEAL        0x25u
#define EMP_OP_TRANSPORT     0x26u
#define EMP_OP_CLEAR         0x27u

/* Input, device -> host. */
#define EMP_OP_EDIT          0x50u
#define EMP_OP_RESET         0x51u
#define EMP_OP_PLAY_PAUSE    0x52u
#define EMP_OP_SCREEN        0x53u
#define EMP_OP_FOCUS         0x54u
#define EMP_OP_DESC_REQUEST  0x55u
#define EMP_OP_EDIT_DELTA    0x56u
#define EMP_OP_DESC_ACK      0x57u
#define EMP_OP_BUTTON        0x58u

/* Field kinds, mirroring SurfaceFieldKind on the host. */
#define EMP_KIND_TOGGLE      0u
#define EMP_KIND_NUMBER      1u
#define EMP_KIND_CHOICE      2u
#define EMP_KIND_READONLY    3u
#define EMP_KIND_COLOR       4u

/* Field `present` bitmask.
 *
 * min/max/step ride on the wire unconditionally with this mask saying which are meaningful —
 * 24 bytes spent to buy a fixed 36-byte prefix, so a bare-metal parser does one bounds check
 * and one copy instead of branchy offset arithmetic. Pinned here and in docs/protocol.md;
 * both codecs assert these values. */
#define EMP_PRESENT_MIN       0x0001u
#define EMP_PRESENT_MAX       0x0002u
#define EMP_PRESENT_STEP      0x0004u
#define EMP_PRESENT_PRECISION 0x0008u
#define EMP_PRESENT_DEFAULT   0x0010u
#define EMP_PRESENT_PATH      0x0020u
#define EMP_PRESENT_UNIT      0x0040u
#define EMP_PRESENT_CHOICES   0x0080u

/* Field flags. */
#define EMP_FIELD_TRUNCATED   0x01u   /* something was cut; never silently */
#define EMP_FIELD_READONLY    0x02u

/* Why an edit happened, on EDIT. Diagnostics for the host, not semantics. */
#define EMP_CAUSE_ROTATE      0u
#define EMP_CAUSE_PUSH        1u
#define EMP_CAUSE_TOUCH       2u
#define EMP_CAUSE_RESET       3u   /* restored to the declared default */
#define EMP_CAUSE_UNDO        4u   /* stepped through the device's local history */

/* DESC_REQUEST reasons. */
#define EMP_REQ_REVISION_UNKNOWN 1u
#define EMP_REQ_SEQUENCE_BROKEN  2u
#define EMP_REQ_CRC_MISMATCH     3u
#define EMP_REQ_TOO_MANY_FIELDS  4u

/* Value tags. */
#define EMP_VAL_BOOL         0x00u
#define EMP_VAL_NUMBER       0x01u
#define EMP_VAL_CHOICE       0x02u
#define EMP_VAL_TEXT         0x03u
#define EMP_VAL_COLOR        0x04u

/* Errors. Negative, distinct, and never collapsed into a generic failure — "the protocol
 * broke" is exactly the diagnosis that wastes days, and this codebase has paid that bill. */
#define EMP_OK                       0
#define EMP_ERR_SHORT               -1   /* fewer bytes than a header needs */
#define EMP_ERR_BAD_MAGIC           -2
#define EMP_ERR_BAD_VERSION         -3
#define EMP_ERR_TOO_LONG            -4   /* payload_len or total_len beyond what we accept */
#define EMP_ERR_FRAGMENT_UNEXPECTED -5   /* continuation that does not match the message open */
#define EMP_ERR_CRC                 -6
#define EMP_ERR_NO_SPACE            -7   /* caller's output buffer cannot hold the fragment */
#define EMP_ERR_BAD_CHANNEL         -8   /* control message split across fragments (F1) */

#endif /* ELECTRA_EMP_H */
