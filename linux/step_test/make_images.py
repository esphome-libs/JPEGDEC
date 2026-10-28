"""Write the JPEG files that the tests decode, covering the layouts JPEGDEC handles.

Each baseline file also gets progressive twins from jpegtran, which holds the same coefficients, so prog_test
can check that they decode to exactly the same pixels. A color one also gets an extended sequential (SOF1) twin
whose Cr has DC and AC tables of its own, numbered past 1, as libjpeg writes when an image uses more than 2 tables. pairs.txt lists the
twins. Any further arguments are more baseline files to make twins of.
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


def sof1_twin(base: Path, out: Path, ac_table: int) -> Path:
    """An extended sequential copy of a baseline color file, whose Cr is coded with copies of the luma DC and AC tables,
    numbered 2 and ac_table, so each component has an AC table of its own. Every other bit of the scan stays as it was,
    so the coefficients are the same."""
    data = base.read_bytes()
    segments, tables, i = [], {}, 2
    while data[i + 1] != 0xDA:
        marker, length = data[i + 1], int.from_bytes(data[i + 2 : i + 4], "big")
        segment = data[i : i + 2 + length]
        if marker == 0xC0:
            segment = b"\xff\xc1" + segment[2:]
            sampling = [segment[11 + 3 * k] for k in range(segment[9])]
            height, width = int.from_bytes(segment[5:7], "big"), int.from_bytes(segment[7:9], "big")
        elif marker == 0xC4:  # every table of the segment: class and number, 16 counts, then the values
            j = 4
            while j < len(segment):
                counts, size = segment[j + 1 : j + 17], 17 + sum(segment[j + 1 : j + 17])
                codes, code, values = {}, 0, iter(segment[j + 17 : j + size])
                for bits, count in enumerate(counts, 1):
                    for _ in range(count):
                        codes[format(code, f"0{bits}b")] = next(values)
                        code += 1
                    code <<= 1
                tables[segment[j]] = (codes, segment[j + 1 : j + size])
                j += size
        segments.append(segment)
        i += 2 + length
    luma_dc = {v: c for c, v in tables[0x00][0].items()}
    luma_ac = {v: c for c, v in tables[0x10][0].items()}
    copy = b"\x02" + tables[0x00][1] + bytes([0x10 | ac_table]) + tables[0x10][1]
    segments.append(b"\xff\xc4" + (2 + len(copy)).to_bytes(2, "big") + copy)
    sos = bytearray(data[i : i + 2 + int.from_bytes(data[i + 2 : i + 4], "big")])
    sos[5 + 2 * 3 - 1] = 0x20 | ac_table  # Cr, the last of 3 components
    scan, k = bytearray(), i + len(sos)
    while not (data[k] == 0xFF and data[k + 1] != 0):
        scan.append(data[k])
        k += 1 + (data[k] == 0xFF)  # a stuffed zero
    bits = "".join(format(b, "08b") for b in scan)
    pos, out_bits = 0, []

    def symbol(table: int) -> int:
        nonlocal pos
        for n in range(1, 17):
            if bits[pos : pos + n] in tables[table][0]:
                out_bits.append(bits[pos : pos + n])
                pos += n
                return tables[table][0][bits[pos - n : pos]]
        raise ValueError("bad Huffman code")

    def extra(n: int) -> None:
        nonlocal pos
        out_bits.append(bits[pos : pos + n])
        pos += n

    h, v = sampling[0] >> 4, sampling[0] & 15
    for _ in range(-(-width // (8 * h)) * -(-height // (8 * v))):
        for comp, blocks in ((0, h * v), (1, 1), (2, 1)):
            for _ in range(blocks):
                s = symbol(0x00 if comp == 0 else 0x01)
                if comp == 2:  # coded again with the luma DC table
                    out_bits[-1] = luma_dc[s]
                extra(s)
                n = 1
                while n < 64:
                    rs = symbol(0x10 if comp == 0 else 0x11)
                    if comp == 2:  # coded again with the luma AC table
                        out_bits[-1] = luma_ac[rs]
                    if rs == 0:
                        break
                    extra(rs & 15)
                    n += 16 if rs == 0xF0 else (rs >> 4) + 1
    packed = "".join(out_bits)
    packed += "1" * (-len(packed) % 8)
    body = bytes(int(packed[j : j + 8], 2) for j in range(0, len(packed), 8)).replace(b"\xff", b"\xff\x00")
    twin = out / f"twin_sof1_{base.name}"
    twin.write_bytes(data[:2] + b"".join(segments) + bytes(sos) + body + b"\xff\xd9")
    return twin


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
            # AC table 3 goes in the decoder's third slot, as table 2 does
            pairs.append((base, sof1_twin(base, out, 3 if subsampling == 0 else 2)))
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
