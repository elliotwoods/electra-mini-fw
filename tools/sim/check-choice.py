"""Pixel-level checks for the centred adaptive Choice drill-down."""

from pathlib import Path
import subprocess
import sys

from PIL import Image


BG = (0, 0, 0)
CYAN = (0, 255, 255)


def has_ink(image, box):
    x0, y0, x1, y1 = box
    pixels = image.load()
    return any(pixels[x, y] != BG for y in range(y0, y1) for x in range(x0, x1))


def has_white(image, box):
    x0, y0, x1, y1 = box
    pixels = image.load()
    return any(pixels[x, y] == (255, 255, 255)
               for y in range(y0, y1) for x in range(x0, x1))


def cyan_bounds(image, y0, y1):
    pixels = image.load()
    points = [(x, y) for y in range(y0, y1) for x in range(image.width)
              if pixels[x, y] == CYAN]
    assert points, "selected marker is missing"
    return (min(x for x, _ in points), min(y for _, y in points),
            max(x for x, _ in points), max(y for _, y in points))


def render(exe, out_dir, scene):
    path = out_dir / f"{scene}.ppm"
    subprocess.run([str(exe), scene, str(path)], check=True)
    return Image.open(path).convert("RGB")


def assert_slots(image, y0, centres, radius):
    for centre in centres:
        assert has_ink(image, (centre - radius, y0, centre + radius + 1, y0 + 85)), \
            f"no label ink around slot centre {centre}"


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: check-choice.py <sim.exe> <output-dir>")
    exe = Path(sys.argv[1]).resolve()
    out_dir = Path(sys.argv[2]).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    scenes = {name: render(exe, out_dir, name) for name in (
        "choice-first", "choice-middle", "choice-last",
        "choice-7", "choice-5", "choice-3-top")}

    # The selected option's marker is invariant and wholly inside the replaced row in both
    # orientations. White selected-label ink must occupy that same central slot.
    for name, image in scenes.items():
        y0 = 290 if name.endswith("top") else 69
        assert cyan_bounds(image, y0, y0 + 121) == (385, y0 + 108, 414, y0 + 110), name
        assert has_white(image, (300, y0, 500, y0 + 105)), name

    # Waveform uses five slots. Endpoint slots remain empty instead of shifting or wrapping,
    # and continuation arrows exist only on the side with hidden real options.
    first, middle, last = (scenes[n] for n in
                           ("choice-first", "choice-middle", "choice-last"))
    assert not has_ink(first, (40, 69, 340, 160))
    assert not has_ink(first, (12, 69, 50, 160))
    assert has_ink(first, (750, 69, 788, 160))
    assert not has_ink(middle, (12, 69, 50, 160))
    assert not has_ink(middle, (750, 69, 788, 160))
    assert has_ink(last, (12, 69, 50, 160))
    assert not has_ink(last, (750, 69, 788, 160))
    assert not has_ink(last, (460, 69, 760, 160))

    # Representative complete label sets select each adaptive density. Symmetric centres are
    # derived from the same integer pitches as the firmware, always including exact x=400.
    assert_slots(scenes["choice-7"], 69, (70, 180, 290, 400, 510, 620, 730), 38)
    assert_slots(scenes["choice-5"], 69, (90, 245, 400, 555, 710), 65)
    assert_slots(scenes["choice-3-top"], 290, (142, 400, 658), 105)

    print("Choice carousel pixel checks passed")


if __name__ == "__main__":
    main()
