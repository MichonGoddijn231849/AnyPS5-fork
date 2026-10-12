"""Images in the terminal for tools/fidelity: sixel (Windows Terminal 1.22+, xterm -ti vt340, mlterm, foot, WezTerm)
or, as the fallback, half-block characters with 24-bit colour (two image rows per text row).

Only what fidelity needs: reads the 8-bit RGB/RGBA, non-interlaced PNGs agc_surface_png writes (stb_image_write),
no other dependencies."""
from __future__ import annotations

import os
import shutil
import struct
import sys
import zlib


class Image:
    def __init__(self, width: int, height: int, rgb: bytearray):
        self.width, self.height, self.rgb = width, height, rgb    # 3 bytes per pixel, row major

    def pixel(self, x: int, y: int) -> tuple[int, int, int]:
        i = (y * self.width + x) * 3
        return self.rgb[i], self.rgb[i + 1], self.rgb[i + 2]


def read_png(path: str) -> Image:
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n": raise ValueError(f"{path}: not a PNG")
    at, idat, width = 8, bytearray(), 0
    while at < len(data):
        length, kind = struct.unpack(">I4s", data[at:at + 8])
        chunk = data[at + 8:at + 8 + length]
        if kind == b"IHDR":
            width, height, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", chunk)
            if depth != 8 or colour not in (2, 6) or interlace: raise ValueError(f"{path}: only 8-bit RGB/RGBA non-interlaced PNGs")
            channels = 3 if colour == 2 else 4
        elif kind == b"IDAT":
            idat += chunk
        elif kind == b"IEND":
            break
        at += 12 + length
    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    rgb = bytearray(width * height * 3)
    previous = bytearray(stride)
    for y in range(height):
        start = y * (stride + 1)
        kind, line = raw[start], bytearray(raw[start + 1:start + 1 + stride])
        for i in range(stride):
            left = line[i - channels] if i >= channels else 0
            up = previous[i]
            if kind == 1: line[i] = (line[i] + left) & 255
            elif kind == 2: line[i] = (line[i] + up) & 255
            elif kind == 3: line[i] = (line[i] + ((left + up) >> 1)) & 255
            elif kind == 4:
                corner = previous[i - channels] if i >= channels else 0
                p = left + up - corner
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - corner)
                line[i] = (line[i] + (left if pa <= pb and pa <= pc else up if pb <= pc else corner)) & 255
        for x in range(width):
            o = (y * width + x) * 3
            rgb[o:o + 3] = line[x * channels:x * channels + 3]
        previous = line
    return Image(width, height, rgb)


def scaled(image: Image, width: int, height: int) -> Image:
    """Box-filtered down (nearest up) to width x height."""
    rgb = bytearray(width * height * 3)
    for y in range(height):
        y0, y1 = y * image.height // height, max((y + 1) * image.height // height, y * image.height // height + 1)
        for x in range(width):
            x0, x1 = x * image.width // width, max((x + 1) * image.width // width, x * image.width // width + 1)
            total, count = [0, 0, 0], 0
            for sy in range(y0, y1, max(1, (y1 - y0) // 4)):
                for sx in range(x0, x1, max(1, (x1 - x0) // 4)):
                    p = image.pixel(sx, sy)
                    total[0] += p[0]; total[1] += p[1]; total[2] += p[2]
                    count += 1
            o = (y * width + x) * 3
            rgb[o:o + 3] = bytes(c // count for c in total)
    return Image(width, height, rgb)


def fit(image: Image, max_width: int, max_height: int) -> Image:
    scale = min(max_width / image.width, max_height / image.height, 1.0 if image.width >= 64 else 4.0)
    return scaled(image, max(1, int(image.width * scale)), max(1, int(image.height * scale)))


# A 6x6x6 colour cube plus 40 greys: 256 sixel colour registers.
def palette_index(r: int, g: int, b: int) -> int:
    if max(r, g, b) - min(r, g, b) < 12:
        return 216 + min(39, (r + g + b) // 3 * 40 // 256)
    return (r * 6 // 256) * 36 + (g * 6 // 256) * 6 + (b * 6 // 256)


def palette_rgb(index: int) -> tuple[int, int, int]:
    if index >= 216:
        v = (index - 216) * 255 // 39
        return v, v, v
    return (index // 36) * 51, (index // 6 % 6) * 51, (index % 6) * 51


def adaptive_palette(image: Image, size: int = 256) -> tuple[list[tuple[int, int, int]], list[int]]:
    """The image's own palette (its `size` most common colours at 5 bits per channel) and each pixel's entry."""
    keys = [((image.rgb[i] >> 3) << 10) | ((image.rgb[i + 1] >> 3) << 5) | (image.rgb[i + 2] >> 3) for i in range(0, len(image.rgb), 3)]
    counts: dict[int, int] = {}
    for key in keys: counts[key] = counts.get(key, 0) + 1
    chosen = sorted(counts, key=counts.get, reverse=True)[:size]
    palette = [(((k >> 10) & 31) * 255 // 31, ((k >> 5) & 31) * 255 // 31, (k & 31) * 255 // 31) for k in chosen]
    nearest: dict[int, int] = {k: i for i, k in enumerate(chosen)}
    for key in counts:
        if key in nearest: continue
        r, g, b = ((key >> 10) & 31) * 255 // 31, ((key >> 5) & 31) * 255 // 31, (key & 31) * 255 // 31
        nearest[key] = min(range(len(palette)), key=lambda i: (palette[i][0] - r) ** 2 + (palette[i][1] - g) ** 2 + (palette[i][2] - b) ** 2)
    return palette, [nearest[k] for k in keys]


def to_sixel(image: Image) -> str:
    palette, indices = adaptive_palette(image)
    used = sorted(set(indices))
    out = [f'\x1bPq"1;1;{image.width};{image.height}']
    for index in used:
        r, g, b = palette[index]
        out.append(f"#{index};2;{r * 100 // 255};{g * 100 // 255};{b * 100 // 255}")
    for band in range(0, image.height, 6):
        rows = range(band, min(band + 6, image.height))
        colours = sorted({indices[y * image.width + x] for y in rows for x in range(image.width)})
        for n, colour in enumerate(colours):
            out.append(f"#{colour}")
            run_char, run = None, 0
            for x in range(image.width):
                bits = 0
                for bit, y in enumerate(rows):
                    if indices[y * image.width + x] == colour: bits |= 1 << bit
                char = chr(63 + bits)
                if char == run_char: run += 1
                else:
                    if run_char is not None: out.append(f"!{run}{run_char}" if run > 3 else run_char * run)
                    run_char, run = char, 1
            out.append(f"!{run}{run_char}" if run > 3 else run_char * run)
            out.append("$" if n + 1 < len(colours) else "")
        out.append("-")
    out.append("\x1b\\")
    return "".join(out)


def to_half_blocks(image: Image) -> str:
    lines = []
    for y in range(0, image.height, 2):
        parts = []
        for x in range(image.width):
            top = image.pixel(x, y)
            bottom = image.pixel(x, y + 1) if y + 1 < image.height else (0, 0, 0)
            parts.append(f"\x1b[38;2;{top[0]};{top[1]};{top[2]}m\x1b[48;2;{bottom[0]};{bottom[1]};{bottom[2]}m▀")
        lines.append("".join(parts) + "\x1b[0m")
    return "\n".join(lines)


def sixel_supported() -> bool:
    # Windows Terminal sets WT_SESSION and draws sixel since 1.22; APS5_SIXEL=0/1 overrides the guess.
    forced = os.environ.get("APS5_SIXEL")
    if forced is not None: return forced not in ("", "0")
    return bool(os.environ.get("WT_SESSION")) or os.environ.get("TERM_PROGRAM") in ("WezTerm", "mlterm") or "foot" in os.environ.get("TERM", "")


def show(path: str, mode: str = "auto", max_width: int | None = None) -> str:
    """The PNG as terminal text: sixel ('sixel'), half blocks ('blocks') or the better one available ('auto')."""
    image = read_png(path)
    columns, rows = shutil.get_terminal_size((160, 48))
    if mode == "auto": mode = "sixel" if sixel_supported() else "blocks"
    if mode == "sixel":
        # About 10 x 20 pixels per cell in Windows Terminal at the default font size.
        return to_sixel(fit(image, max_width or columns * 9, (rows - 6) * 18))
    return to_half_blocks(fit(image, max_width or columns - 1, (rows - 6) * 2))


if __name__ == "__main__":
    if os.name == "nt": os.system("")
    for argument in sys.argv[1:]:
        print(argument)
        print(show(argument, os.environ.get("APS5_IMAGE_MODE", "auto")))


def write_png(path: str, image: Image) -> None:
    raw = b"".join(b"\x00" + bytes(image.rgb[y * image.width * 3:(y + 1) * image.width * 3]) for y in range(image.height))
    chunk = lambda kind, data: struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
    with open(path, "wb") as file:
        file.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", image.width, image.height, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def save_side_by_side(paths: list[str], out: str, gap: int = 4) -> None:
    images = [read_png(path) for path in paths]
    height = max(image.height for image in images)
    width = sum(image.width for image in images) + gap * (len(images) - 1)
    rgb = bytearray(width * height * 3)
    x0 = 0
    for image in images:
        for y in range(image.height):
            rgb[(y * width + x0) * 3:(y * width + x0 + image.width) * 3] = image.rgb[y * image.width * 3:(y + 1) * image.width * 3]
        x0 += image.width + gap
    write_png(out, Image(width, height, rgb))
