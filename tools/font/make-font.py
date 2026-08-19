"""
Turn a TrueType face into an 8-bit alpha atlas the firmware can render.

    python make-font.py

Design, and why it is not the obvious one:

The RA8876 can composite a bitmap out of its own VRAM with the BTE, which is how the stock
firmware draws text — glyphs live in video memory and are block-copied. That is fast, but it is
1-bit: a glyph is either there or it is not, so the type is aliased. On a 800x480 panel viewed
at arm's length, aliased small text is exactly the thing that makes an instrument look cheap,
and "better typography" is one of the stated reasons this firmware exists.

So instead each glyph is stored as 8-bit COVERAGE in MCU flash, and the MCU blends it against
the actual foreground and background colours into a scanline buffer, which is then pushed to
the panel. That buys real antialiasing and arbitrary colours from one atlas, at a cost that is
affordable precisely because text is small: a 120x24 label is ~5.8 KB on the wire, about 6 ms
at 15 MHz, against 0.8 s for a full-screen repaint. Cost scales with ink, not with screen.

Two faces, because they do different jobs:

  * a proportional UI face for labels, which reads better in running text
  * a MONOSPACED face for values, so digits do not shift sideways as a number changes

That second one is not fussiness. A value readout that reflows while you turn a knob is
genuinely harder to read, and a control surface is mostly value readouts.
"""

import os
import sys

from PIL import Image, ImageDraw, ImageFont

FIRST, LAST = 0x20, 0x7E          # printable ASCII; UTF-8 beyond this needs a bigger atlas
OUT_DIR = os.path.join(os.path.dirname(__file__), "..", "..", "src", "app")

# Per-face character range, because the display face must not pay for the whole of ASCII.
#
# Sizes were raised across the board after the first look at a real panel: 16/22/30 px was
# legible with your nose against the glass and not from playing distance, which for an
# instrument is the same as illegible.
# Fonts are VENDORED, not taken from the system.
#
# The first version rendered Segoe UI, Consolas and Segoe MDL2 straight out of C:/Windows/Fonts.
# That works on the machine it was written on and nowhere else, and -- the reason it had to
# change -- the generated atlas is a derivative of a licensed font, so it could not be published
# with the firmware. DejaVu is under the Bitstream Vera licence, which permits redistribution
# and derived works, so the atlas in src/app/font_data.c can ship with the source that uses it.
#
# It also makes the build reproducible: no dependency on which fonts a machine happens to have.
VENDOR = os.path.join(os.path.dirname(__file__), "vendor")

# Icons come through this same pipeline, because a coverage atlas does not care whether a glyph
# is a letter or a pictogram. That buys antialiased icons in any colour at any size with no
# drawing code at all -- the alternative was shapes hand-built from filled rectangles, which is
# what the Lua bundle had to do and is why its icons looked like it.
#
# The obstacle is addressing: font_t.first/last are uint8_t and these glyphs live well above
# 0xFF. So a face may give an explicit LIST of codepoints instead of a (first, last) range, and
# the generator packs them into sequential byte codes from 0x20. src/app/icons.h names them, and
# that mapping is POSITIONAL -- reordering either list silently swaps the icons.
ICON_CODEPOINTS = [
    0x2715,   # MULTIPLICATION X                 -> ICON_EXIT
    0x21BB,   # CLOCKWISE OPEN CIRCLE ARROW      -> ICON_RESET
    0x21B6,   # ANTICLOCKWISE TOP SEMICIRCLE ARROW -> ICON_UNDO
    0x21B7,   # CLOCKWISE TOP SEMICIRCLE ARROW   -> ICON_REDO
    # Triangles rather than guillemets: at this size the guillemets rendered visibly lighter
    # than the four arrow glyphs beside them, and a legend strip wants even weight.
    0x25C0,   # BLACK LEFT-POINTING TRIANGLE     -> ICON_PREV
    0x25B6,   # BLACK RIGHT-POINTING TRIANGLE    -> ICON_NEXT
    0x2699,   # GEAR                              -> ICON_SETTINGS
]

FACES = [
    # name       file                 size  codepoints        description
    ("ui",      "DejaVuSans.ttf",      26,  (0x20, 0x7E),     "proportional, for labels"),
    ("value",   "DejaVuSansMono.ttf",  38,  (0x20, 0x7E),     "monospaced, so digits do not jitter"),
    ("small",   "DejaVuSans.ttf",      19,  (0x20, 0x7E),     "proportional, for units and secondary"),
    ("display", "DejaVuSansMono.ttf", 112,  (0x20, 0x39),     "the focused readout: digits only"),
    ("icons",   "DejaVuSans.ttf",      34,  ICON_CODEPOINTS,  "remapped to bytes; see icons.h"),
]


def render_face(name, filename, px, codes, note):
    path = os.path.join(VENDOR, filename)
    font = ImageFont.truetype(path, px)

    ascent, descent = font.getmetrics()
    line_height = ascent + descent

    glyphs = []
    blob = bytearray()

    # Either a contiguous range or an explicit list; both end up as sequential byte codes.
    if isinstance(codes, tuple):
        first, last = codes
        sources = list(range(first, last + 1))
    else:
        first = 0x20
        last = first + len(codes) - 1
        sources = list(codes)

    for slot, code in enumerate(sources):
        ch = chr(code)

        # Render on a generous canvas, then measure the ink. Storing the ink box plus its
        # offset rather than a fixed cell keeps the atlas small and lets the blitter skip
        # blank rows entirely.
        pad = px
        img = Image.new("L", (px * 3 + pad, line_height + pad * 2), 0)
        draw = ImageDraw.Draw(img)
        draw.text((pad, pad), ch, font=font, fill=255)

        bbox = img.getbbox()
        advance = int(round(font.getlength(ch)))

        byte_code = first + slot

        if bbox is None:                       # space and friends: no ink at all
            glyphs.append(dict(code=byte_code, w=0, h=0, bx=0, by=0, adv=advance, off=len(blob)))
            continue

        x0, y0, x1, y1 = bbox
        w, h = x1 - x0, y1 - y0
        crop = img.crop(bbox)

        glyphs.append(dict(code=byte_code, w=w, h=h,
                           bx=x0 - pad,             # left bearing from the pen position
                           by=y0 - pad,             # top, relative to the ascent line
                           adv=advance, off=len(blob)))
        blob += crop.tobytes()

    return dict(name=name, note=note, px=px, ascent=ascent, descent=descent,
                line_height=line_height, glyphs=glyphs, blob=bytes(blob),
                source=filename, first=first, last=last)


def emit(faces, path):
    # Pin LF even on Windows. Mixed CRLF in generated additions is reported as trailing
    # whitespace by git diff --check and makes an otherwise deterministic atlas platform-local.
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("/* GENERATED by tools/font/make-font.py — do not edit.\n"
                " *\n"
                " * 8-bit coverage per pixel, per glyph, cropped to its ink box. The renderer\n"
                " * blends this against the caller's colours, so one atlas serves every colour\n"
                " * combination and the type is antialiased rather than 1-bit.\n"
                " *\n"
                " * Sources:\n")
        for fc in faces:
            f.write(" *   %-6s %-14s %2d px  %s\n" % (fc["name"], fc["source"], fc["px"], fc["note"]))
        f.write(" */\n\n#include \"font.h\"\n\n")

        for fc in faces:
            f.write("static const uint8_t %s_pixels[] = {\n" % fc["name"])
            b = fc["blob"]
            for i in range(0, len(b), 20):
                f.write("    " + ",".join("%3d" % v for v in b[i:i + 20]) + ",\n")
            f.write("};\n\n")

            f.write("static const font_glyph_t %s_glyphs[] = {\n" % fc["name"])
            for g in fc["glyphs"]:
                f.write("    {%6d,%3d,%3d,%4d,%4d,%3d},  /* %-3s */\n"
                        % (g["off"], g["w"], g["h"], g["bx"], g["by"], g["adv"],
                           repr(chr(g["code"]))[1:-1]))
            f.write("};\n\n")

            f.write("const font_t font_%s = {\n"
                    "    .pixels = %s_pixels,\n"
                    "    .glyphs = %s_glyphs,\n"
                    "    .first = %d, .last = %d,\n"
                    "    .ascent = %d, .descent = %d, .line_height = %d,\n"
                    "};\n\n"
                    % (fc["name"], fc["name"], fc["name"], fc["first"], fc["last"],
                       fc["ascent"], fc["descent"], fc["line_height"]))


def main():
    faces = [render_face(*spec) for spec in FACES]

    out = os.path.abspath(os.path.join(OUT_DIR, "font_data.c"))
    emit(faces, out)

    total = sum(len(f["blob"]) for f in faces)
    print("wrote %s" % out)
    for fc in faces:
        print("  %-8s %3d px  %6d glyph bytes  line height %2d  (%s)"
              % (fc["name"], fc["px"], len(fc["blob"]), fc["line_height"], fc["note"]))
    print("  total %d bytes of flash for %d faces" % (total, len(faces)))


if __name__ == "__main__":
    main()
