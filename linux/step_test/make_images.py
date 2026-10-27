"""Write the JPEG files that the tests decode, covering the layouts JPEGDEC handles.

Each baseline file also gets progressive twins from jpegtran, which holds the same coefficients, so prog_test
can check that they decode to exactly the same pixels. pairs.txt lists the twins. Any further arguments are
more baseline files to make twins of.
"""

import subprocess
import sys
from pathlib import Path

from PIL import Image

# jpegtran scan scripts, besides its default progressive script
COLOR_SCANS = {
    # Each component's DC in a scan of its own, and spectral selection without successive approximation
    "spectral": """
        0: 0-0, 0, 0; 1: 0-0, 0, 0; 2: 0-0, 0, 0;
        0: 1-5, 0, 0; 0: 6-63, 0, 0; 1: 1-63, 0, 0; 2: 1-63, 0, 0;
    """,
    # Successive approximation of several bits, in bands
    "approx": """
        0,1,2: 0-0, 0, 2;
        0: 1-9, 0, 3; 2: 1-63, 0, 2; 1: 1-63, 0, 2; 0: 10-63, 0, 3;
        0,1,2: 0-0, 2, 1;
        0: 1-63, 3, 2; 0: 1-63, 2, 1; 1: 1-63, 2, 1; 2: 1-63, 2, 1;
        0,1,2: 0-0, 1, 0;
        0: 1-63, 1, 0; 1: 1-63, 1, 0; 2: 1-63, 1, 0;
    """,
    # A DC scan of the two chroma components only
    "chroma_dc": """
        0: 0-0, 0, 0; 1,2: 0-0, 0, 0;
        0: 1-63, 0, 1; 1: 1-63, 0, 0; 2: 1-63, 0, 0; 0: 1-63, 1, 0;
    """,
}
GRAY_SCANS = {
    "approx": """
        0: 0-0, 0, 1; 0: 1-5, 0, 2; 0: 6-63, 0, 2;
        0: 1-63, 2, 1; 0: 0-0, 1, 0; 0: 1-63, 1, 0;
    """,
}


def pattern(width: int, height: int, mode: str) -> Image.Image:
    """A picture with gradients, edges and fine detail, so every DCT coefficient is used."""
    img = Image.new("RGB", (width, height))
    px = img.load()
    for y in range(height):
        for x in range(width):
            px[x, y] = (
                (x * 255) // max(1, width - 1),
                (y * 255) // max(1, height - 1),
                255 if ((x // 7) + (y // 5)) % 2 else (x * y) % 256,
            )
    return img.convert(mode)


def twins(base: Path, out: Path, gray: bool) -> list[Path]:
    """Progressive copies of a baseline file, made by jpegtran."""
    variants = {"prog": [], "prog_rst": ["-restart", "1"], "prog_rstb": ["-restart", "3B"]}
    for name, script in (GRAY_SCANS if gray else COLOR_SCANS).items():
        path = out / f"scans_{name}.txt"
        path.write_text(script)
        variants[f"scans_{name}"] = ["-scans", str(path)]
    made = []
    for name, args in variants.items():
        twin = out / f"twin_{name}_{base.name}"
        subprocess.run(["jpegtran", "-progressive", *args, "-outfile", str(twin), str(base)], check=True)
        made.append(twin)
    return made


def main(out: Path, more: list[Path]) -> None:
    out.mkdir(parents=True, exist_ok=True)
    pairs = []
    sizes = [(64, 48), (203, 157), (320, 240), (17, 9)]
    for width, height in sizes:
        rgb = pattern(width, height, "RGB")
        for subsampling, name in ((0, "444"), (1, "422"), (2, "420")):
            base = out / f"{name}_{width}x{height}.jpg"
            rgb.save(base, quality=85, subsampling=subsampling)
            pairs += [(base, twin) for twin in twins(base, out, False)]
            rgb.save(
                out / f"{name}_rst_{width}x{height}.jpg",
                quality=85,
                subsampling=subsampling,
                restart_marker_blocks=3,
            )
        base = out / f"gray_{width}x{height}.jpg"
        pattern(width, height, "L").save(base, quality=85)
        pairs += [(base, twin) for twin in twins(base, out, True)]
        rgb.save(out / f"prog_{width}x{height}.jpg", quality=85, progressive=True)
    # A 4:4:0 image, which JPEGDEC draws with 1x2 luma blocks
    base, source = out / "440_64x48.jpg", out / "440_64x48.ppm"
    pattern(64, 48, "RGB").save(source)
    subprocess.run(["cjpeg", "-quality", "85", "-sample", "1x2", "-outfile", str(base), str(source)], check=True)
    pairs += [(base, twin) for twin in twins(base, out, False)]
    for base in more:
        pairs += [(base, twin) for twin in twins(base, out, False)]
    (out / "pairs.txt").write_text("".join(f"{a} {b}\n" for a, b in pairs))


if __name__ == "__main__":
    main(Path(sys.argv[1]), [Path(p) for p in sys.argv[2:]])
