/* Diagnostics, so "never silently" has a mechanism behind it.
 *
 * docs/protocol.md forbids silent degradation in several places -- a truncated string, a
 * non-finite f64, an unknown value tag, a refused fragment. Every one of those was implemented
 * as a comment saying it must be reported, above code that returned a value the caller threw
 * away. The device degraded exactly as designed and told nobody.
 *
 * Two design points, both of which exist because of where these are raised from.
 *
 * NOTHING IS SENT FROM emp_diag(). It records; emp_diag_tick() sends. Almost every call site is
 * inside a decoder, part-way through a message, and re-entering the wire from there would mean
 * framing an outbound message using state that a half-finished inbound one is still walking.
 * The cost is up to one service period of latency on a diagnostic, which is nothing.
 *
 * REPEATS COALESCE. A missing glyph is raised once per character per repaint; an uncovered
 * codepoint on a 60 Hz meter would otherwise produce a message per frame per glyph and turn a
 * diagnostic into a denial of service against the link it is reporting on. Each code holds one
 * slot, and the flush carries how many times it fired.
 *
 * Depends on <stdint.h> and <string.h> only, like the rest of src/proto -- see docs/protocol.md
 * section 7, which is what lets this code run under a sanitiser on a workstation.
 */

#ifndef EMP_DIAG_H
#define EMP_DIAG_H

#include <stdint.h>

/* Severity. The host turns these into snapshot.log and error_count; none of them, at any
 * severity, may close the pipe or fail a caller (docs/protocol.md 3.1). */
#define EMP_SEV_INFO   0u
#define EMP_SEV_WARN   1u
#define EMP_SEV_ERROR  2u

/* Codes. Stable numbers: a host reads these from a device it did not build. */
#define EMP_DIAG_NONE                 0u
#define EMP_DIAG_STRING_TRUNCATED     1u   /* string pool exhausted; label dropped */
#define EMP_DIAG_NON_FINITE           2u   /* NaN or infinity on the wire, substituted with 0 */
#define EMP_DIAG_UNKNOWN_VALUE_TAG    3u   /* stepped over; context = the tag */
#define EMP_DIAG_VALUE_UNDECODABLE    4u   /* reserved tag, no length; record abandoned */
#define EMP_DIAG_CHOICES_EXHAUSTED    5u   /* option labels dropped; the knob shows indices */
#define EMP_DIAG_DESC_TOO_MANY_FIELDS 6u
#define EMP_DIAG_DESC_SEQUENCE        7u   /* context = the index that was expected */
#define EMP_DIAG_DESC_CRC             8u
#define EMP_DIAG_REVISION_UNKNOWN     9u
#define EMP_DIAG_FRAGMENT_UNEXPECTED  10u
#define EMP_DIAG_REASSEMBLY_TIMEOUT   11u
#define EMP_DIAG_UNKNOWN_OPCODE       12u  /* context = (channel << 8) | opcode */
#define EMP_DIAG_TX_DROPPED           13u
#define EMP_DIAG_GLYPH_MISSING        14u  /* context = the codepoint */
#define EMP_DIAG_OVERFLOW             15u  /* diagnostics lost because every slot was in use */

/* Raise one. `detail` must be a string literal or otherwise outlive the flush -- only the
 * pointer is kept, because copying a message this code exists to make cheap would defeat it.
 * Safe to call from inside a decoder, from an interrupt-driven path, and at any rate. */
void emp_diag(uint8_t severity, uint16_t code, uint32_t context, const char *detail);

/* Send whatever has accumulated. Call from the service loop. */
void emp_diag_tick(void);

/* Drop everything pending without sending, for tests and for a session reset -- diagnostics
 * from before a host attached are not that host's business. */
void emp_diag_reset(void);

/* How many diagnostics have been raised since reset, by severity index (0..2). A running total
 * for whoever asks -- the console reports it, and it survives the coalescing above, so "this
 * device has complained 400 times" is answerable even when the messages themselves were merged
 * down to a handful. */
uint32_t emp_diag_count(uint8_t severity);

#endif /* EMP_DIAG_H */
