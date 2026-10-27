"""Generate cropped coverage atlases from pinned Inter / JetBrains Mono sources.
DejaVu supplies only the existing icon glyphs. See vendor licences and fonts.lock.json.
Run with the pinned Pillow in tools/font/requirements.txt.
"""

import os
import json
import hashlib
import PIL
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
# Inter and JetBrains Mono are retained under SIL OFL 1.1; icons retain DejaVu.
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
    # The same glyph as the on-screen inspector's reset (web ResettableControl, U+27F2). The
    # clockwise circle arrow this replaced sat beside the undo semicircle and read as its twin.
    0x27F2,   # ANTICLOCKWISE GAPPED CIRCLE ARROW -> ICON_RESET
    0x21B6,   # ANTICLOCKWISE TOP SEMICIRCLE ARROW -> ICON_UNDO
    0x21B7,   # CLOCKWISE TOP SEMICIRCLE ARROW   -> ICON_REDO
    # Triangles rather than guillemets: at this size the guillemets rendered visibly lighter
    # than the four arrow glyphs beside them, and a legend strip wants even weight.
    0x25C0,   # BLACK LEFT-POINTING TRIANGLE     -> ICON_PREV
    0x25B6,   # BLACK RIGHT-POINTING TRIANGLE    -> ICON_NEXT
    0x2699,   # GEAR                              -> ICON_SETTINGS
    0x21A7,   # DOWNWARDS ARROW FROM BAR          -> ICON_STORE (the inspector's save-as-default)
    0x2194,   # LEFT RIGHT ARROW                  -> ICON_ZOOM (hold, then turn a dial)
    # Media transport (EMP/1.10 buttons). DejaVu has no U+23ED..23EF, so each is composed from
    # glyphs it does have: the triangles above and HEAVY VERTICAL BAR (U+275A), with the bars cut
    # to the triangle's height. A tuple is one glyph; see compose_icon().
    (0x275A, 0x25C0),          # bar, left triangle          -> ICON_TRACK_PREV
    (0x25B6, 0x275A, 0x275A),  # right triangle, two bars    -> ICON_PLAY_PAUSE
    (0x25B6, 0x275A),          # right triangle, bar         -> ICON_TRACK_NEXT
]

# Ink between the parts of a composed icon, in pixels at the icon face's size.
ICON_GAP = 3
TRIANGLES = (0x25B6, 0x25C0)

FACES = [
    # name       file                 size  codepoints        description
    ("ui",      "InterVariable.woff2", 26,  (0x20, 0x7E),     "proportional, for labels"),
    ("value",   "JetBrainsMono-Regular.woff2", 38,  (0x20, 0x7E),     "monospaced, so digits do not jitter"),
    ("compact", "JetBrainsMono-Regular.woff2", 26, (0x20, 0x7E), "monospaced compact values"),
    ("small",   "InterVariable.woff2", 19,  (0x20, 0x7E),     "proportional, for units and secondary"),
    ("display", "JetBrainsMono-Regular.woff2", 112,  (0x20, 0x39),     "the focused readout: digits only"),
    ("icons",   "DejaVuSans.ttf",      34,  ICON_CODEPOINTS,  "remapped to bytes; see icons.h"),
]


def compose_icon(font, px, line_height, parts):
    """One glyph from several, side by side ICON_GAP apart, on the same canvas and pen position a
    single glyph gets. Every part is cut to the rows the triangle inks, so a bar is exactly as
    tall as the triangle beside it (U+275A alone stands taller). Returns the canvas and an advance
    that keeps the triangle's own side bearings."""
    pad = px

    def ink(code):
        img = Image.new("L", (px * 3 + pad, line_height + pad * 2), 0)
        ImageDraw.Draw(img).text((pad, pad), chr(code), font=font, fill=255)
        return img, img.getbbox()

    rendered = [ink(code) for code in parts]
    triangle = next(box for code, (_, box) in zip(parts, rendered) if code in TRIANGLES)
    top, bottom = triangle[1], triangle[3]
    width = sum(box[2] - box[0] for _, box in rendered) + ICON_GAP * (len(parts) - 1)
    out = Image.new("L", (width + pad * 2, line_height + pad * 2), 0)
    x = pad
    for img, box in rendered:
        out.paste(img.crop((box[0], top, box[2], bottom)), (x, top))
        x += box[2] - box[0] + ICON_GAP
    tri = next(code for code in parts if code in TRIANGLES)
    bearings = int(round(font.getlength(chr(tri)))) - (triangle[2] - triangle[0])
    return out, width + bearings


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
        # Render on a generous canvas, then measure the ink. Storing the ink box plus its
        # offset rather than a fixed cell keeps the atlas small and lets the blitter skip
        # blank rows entirely.
        pad = px
        if isinstance(code, tuple):
            img, advance = compose_icon(font, px, line_height, code)
        else:
            ch = chr(code)
            img = Image.new("L", (px * 3 + pad, line_height + pad * 2), 0)
            draw = ImageDraw.Draw(img)
            draw.text((pad, pad), ch, font=font, fill=255)
            advance = int(round(font.getlength(ch)))

        bbox = img.getbbox()

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
    lock = json.load(open(os.path.join(os.path.dirname(__file__), "fonts.lock.json")))
    actual = {"pillow": PIL.__version__, "freetype": ImageFont.core.freetype2_version}
    if actual != lock["generator"]:
        raise SystemExit("Font generator versions differ from fonts.lock.json: " + str(actual))
    for source in lock["fonts"] + [lock["icons"]]:
        data = open(os.path.join(VENDOR, source["file"]), "rb").read()
        assert hashlib.sha256(data).hexdigest() == source["sha256"], source["file"]
    faces = [render_face(*spec) for spec in FACES]
    print("Pillow", PIL.__version__, "FreeType", ImageFont.core.freetype2_version)

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
