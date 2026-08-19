/* Icon names for the `icons` face.
 *
 * The glyphs come from DejaVu Sans, drawn through the ordinary text path: a coverage atlas does
 * not care whether a glyph is a letter or a pictogram, so icons cost no new drawing code and get
 * antialiasing and arbitrary colour for free. The alternative was shapes built from filled
 * rectangles, which is what the Lua bundle had to do and is why its icons looked like it.
 *
 * `font_t.first/last` are uint8_t and these glyphs live above 0xFF, so tools/font/make-font.py
 * packs the chosen codepoints into sequential byte codes from 0x20. The mapping is therefore
 * POSITIONAL: these constants and the ICON_CODEPOINTS list in the generator are one table split
 * across two files, and reordering either one silently swaps the icons. Change them together.
 */

#ifndef ELECTRA_ICONS_H
#define ELECTRA_ICONS_H

#define ICON_EXIT   "\x20"    /* U+2715 MULTIPLICATION X            */
#define ICON_RESET  "\x21"    /* U+21BB CLOCKWISE OPEN CIRCLE ARROW */
#define ICON_UNDO   "\x22"    /* U+21B6 ANTICLOCKWISE SEMICIRCLE    */
#define ICON_REDO   "\x23"    /* U+21B7 CLOCKWISE SEMICIRCLE        */
#define ICON_PREV   "\x24"    /* U+25C0 LEFT-POINTING TRIANGLE      */
#define ICON_NEXT   "\x25"    /* U+25B6 RIGHT-POINTING TRIANGLE     */
#define ICON_SETTINGS "\x26"  /* U+2699 GEAR                        */

#endif /* ELECTRA_ICONS_H */
