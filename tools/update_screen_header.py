#!/usr/bin/env python3
"""Update NerdMiner RGB565 PROGMEM image arrays from image files.

This tool updates arrays like:
  const unsigned short initScreen[0xD480] PROGMEM = { ... };

Usage examples:
  python3 tools/update_screen_header.py --header src/media/images_320_170.h --list
  python3 tools/update_screen_header.py --header src/media/images_320_170.h \
      --images-dir assets/screens/320x170
  python3 tools/update_screen_header.py --header src/media/images_320_170.h \
      --map initScreen=/tmp/new-init.png --map MinerScreen=/tmp/new-miner.png
"""

from __future__ import annotations

import argparse
import re
import shutil
import struct
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

ARRAY_RE = re.compile(
    r"(?P<prefix>const\s+unsigned\s+short\s+(?P<name>\w+)\s*\[(?P<size>[^\]]+)\]\s*PROGMEM\s*=\s*\{)"
    r"(?P<body>.*?)"
    r"(?P<suffix>\};)",
    flags=re.S,
)

U16_CONST_RE = re.compile(r"const\s+uint16_t\s+(\w+)\s*=\s*(\d+)\s*;")

DEFAULT_EXTS = (".png", ".jpg", ".jpeg", ".bmp", ".webp")


@dataclass
class ArrayInfo:
    name: str
    size_expr: str
    expected_pixels: Optional[int]
    width_const: Optional[int]
    height_const: Optional[int]


class HeaderUpdateError(RuntimeError):
    pass


def parse_int_expr(expr: str) -> Optional[int]:
    expr = expr.strip()
    if not re.fullmatch(r"0x[0-9A-Fa-f]+|\d+", expr):
        return None
    return int(expr, 0)


def rgb_to_rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def _trailing_zeros(mask: int) -> int:
    shift = 0
    while mask and (mask & 1) == 0:
        shift += 1
        mask >>= 1
    return shift


def _extract_channel(pixel: int, mask: int) -> int:
    if mask == 0:
        return 0
    shift = _trailing_zeros(mask)
    maxv = mask >> shift
    raw = (pixel & mask) >> shift
    if maxv == 0:
        return 0
    return (raw * 255 + (maxv // 2)) // maxv


def _read_bitfield_masks(data: bytes, dib_header_size: int, pixel_offset: int) -> Tuple[int, int, int]:
    # BITMAPV4/V5 headers include masks inside the DIB header.
    if dib_header_size >= 56:
        base = 14 + 40
        return (
            struct.unpack_from("<I", data, base)[0],
            struct.unpack_from("<I", data, base + 4)[0],
            struct.unpack_from("<I", data, base + 8)[0],
        )

    # BITMAPINFOHEADER may store masks immediately after the 40-byte DIB header.
    base = 14 + dib_header_size
    if pixel_offset >= base + 12:
        return (
            struct.unpack_from("<I", data, base)[0],
            struct.unpack_from("<I", data, base + 4)[0],
            struct.unpack_from("<I", data, base + 8)[0],
        )

    raise HeaderUpdateError("BMP bitfield masks are missing")


def _parse_bmp(path: Path) -> Tuple[int, int, List[Tuple[int, int, int]]]:
    data = path.read_bytes()
    if len(data) < 54 or data[:2] != b"BM":
        raise HeaderUpdateError(f"Unsupported BMP data in {path}")

    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    dib_header_size = struct.unpack_from("<I", data, 14)[0]
    if dib_header_size < 40:
        raise HeaderUpdateError(f"Unsupported BMP DIB header ({dib_header_size}) in {path}")

    width = struct.unpack_from("<i", data, 18)[0]
    height_raw = struct.unpack_from("<i", data, 22)[0]
    bits_per_pixel = struct.unpack_from("<H", data, 28)[0]
    compression = struct.unpack_from("<I", data, 30)[0]

    if width <= 0 or height_raw == 0:
        raise HeaderUpdateError(f"Invalid BMP dimensions {width}x{height_raw} in {path}")

    if bits_per_pixel not in (24, 32):
        raise HeaderUpdateError(
            f"Only 24-bit/32-bit BMP is supported; got {bits_per_pixel} in {path}"
        )

    # Supported encodings:
    # - BI_RGB (0): direct BGR/BGRA bytes
    # - BI_BITFIELDS (3): channel masks (common with 32-bit BMP from sips)
    if compression not in (0, 3):
        raise HeaderUpdateError(f"BMP compression={compression} is not supported in {path}")

    height = abs(height_raw)
    top_down = height_raw < 0

    bytes_per_pixel = bits_per_pixel // 8
    stride = ((width * bytes_per_pixel + 3) // 4) * 4
    expected_data_size = stride * height
    if pixel_offset + expected_data_size > len(data):
        raise HeaderUpdateError(f"BMP pixel data is truncated in {path}")

    pixels: List[Tuple[int, int, int]] = []
    rows = range(height) if top_down else range(height - 1, -1, -1)

    masks: Optional[Tuple[int, int, int]] = None
    if compression == 3:
        masks = _read_bitfield_masks(data, dib_header_size, pixel_offset)

    for y in rows:
        row_start = pixel_offset + y * stride
        for x in range(width):
            i = row_start + x * bytes_per_pixel
            if compression == 0:
                b = data[i]
                g = data[i + 1]
                r = data[i + 2]
            else:
                pixel = struct.unpack_from("<I", data, i)[0]
                assert masks is not None
                r = _extract_channel(pixel, masks[0])
                g = _extract_channel(pixel, masks[1])
                b = _extract_channel(pixel, masks[2])
            pixels.append((r, g, b))

    return width, height, pixels


def load_image_pixels(path: Path) -> Tuple[int, int, List[Tuple[int, int, int]]]:
    # Pillow path (if available)
    try:
        from PIL import Image  # type: ignore

        with Image.open(path) as img:
            img = img.convert("RGBA")
            alpha_bg = Image.new("RGBA", img.size, (0, 0, 0, 255))
            rgb = Image.alpha_composite(alpha_bg, img).convert("RGB")
            w, h = rgb.size
            return w, h, list(rgb.getdata())
    except Exception:
        pass

    # macOS fallback: convert to BMP via sips, then parse with stdlib.
    sips = shutil.which("sips")
    if not sips:
        raise HeaderUpdateError(
            "Could not load image: Pillow is unavailable and `sips` is not installed."
        )

    with tempfile.TemporaryDirectory() as td:
        bmp_path = Path(td) / "image.bmp"
        cmd = [sips, "-s", "format", "bmp", str(path), "--out", str(bmp_path)]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0:
            raise HeaderUpdateError(
                f"sips failed for {path}\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
            )
        return _parse_bmp(bmp_path)


def find_arrays(header_text: str) -> List[ArrayInfo]:
    constants: Dict[str, int] = {
        m.group(1): int(m.group(2))
        for m in U16_CONST_RE.finditer(header_text)
    }

    out: List[ArrayInfo] = []
    for m in ARRAY_RE.finditer(header_text):
        name = m.group("name")
        size_expr = m.group("size").strip()
        expected = parse_int_expr(size_expr)

        base = name[:-6] if name.endswith("Screen") else None
        w = constants.get(f"{base}Width") if base else None
        h = constants.get(f"{base}Height") if base else None

        out.append(
            ArrayInfo(
                name=name,
                size_expr=size_expr,
                expected_pixels=expected,
                width_const=w,
                height_const=h,
            )
        )

    return out


def build_array_body(values: Iterable[int], per_line: int = 16) -> str:
    vals = list(values)
    lines: List[str] = []
    for i in range(0, len(vals), per_line):
        chunk = vals[i:i + per_line]
        line = ", ".join(f"0x{v:04X}" for v in chunk)
        if i + per_line < len(vals):
            line += ","
        lines.append(line)
    return "\n".join(lines)


def resolve_image_for_array(
    array_name: str,
    image_map: Dict[str, Path],
    images_dir: Optional[Path],
) -> Optional[Path]:
    if array_name in image_map:
        return image_map[array_name]

    if images_dir is None:
        return None

    for ext in DEFAULT_EXTS:
        candidate = images_dir / f"{array_name}{ext}"
        if candidate.exists():
            return candidate

    # Also accept lowercase extension variations and exact stem match.
    for candidate in images_dir.iterdir() if images_dir.exists() else []:
        if candidate.is_file() and candidate.stem == array_name and candidate.suffix.lower() in DEFAULT_EXTS:
            return candidate

    return None


def update_header(
    header_text: str,
    arrays: List[ArrayInfo],
    image_map: Dict[str, Path],
    images_dir: Optional[Path],
    require_all_mapped: bool,
) -> Tuple[str, List[str]]:
    requested = set(image_map.keys())
    updated: List[str] = []

    def repl(match: re.Match[str]) -> str:
        name = match.group("name")
        source = resolve_image_for_array(name, image_map, images_dir)
        if source is None:
            return match.group(0)

        info = next((a for a in arrays if a.name == name), None)
        if info is None:
            raise HeaderUpdateError(f"Internal error: array metadata not found for {name}")

        if not source.exists():
            raise HeaderUpdateError(f"Image file for {name} does not exist: {source}")

        w, h, pixels = load_image_pixels(source)

        if info.width_const is not None and info.height_const is not None:
            if (w, h) != (info.width_const, info.height_const):
                raise HeaderUpdateError(
                    f"{name}: image size {w}x{h} does not match header constants "
                    f"{info.width_const}x{info.height_const}"
                )

        # Some existing headers declare a larger array than width*height.
        # In those cases the display uses width/height for rendering, so rely on
        # the dimension constants and only enforce array-size equality when dims
        # are not available.
        if info.expected_pixels is not None and (info.width_const is None or info.height_const is None):
            if w * h != info.expected_pixels:
                raise HeaderUpdateError(
                    f"{name}: image pixel count {w*h} does not match declared array size "
                    f"{info.expected_pixels} ({info.size_expr})"
                )

        values = [rgb_to_rgb565(r, g, b) for (r, g, b) in pixels]
        body = build_array_body(values)

        updated.append(name)
        return f"{match.group('prefix')}\n{body}\n{match.group('suffix')}"

    new_text = ARRAY_RE.sub(repl, header_text)

    if require_all_mapped:
        missing = sorted(requested.difference(updated))
        if missing:
            raise HeaderUpdateError(
                "Mapped arrays were not updated (name not found in header or no replacement happened): "
                + ", ".join(missing)
            )

    return new_text, updated


def parse_map_entries(entries: List[str]) -> Dict[str, Path]:
    out: Dict[str, Path] = {}
    for entry in entries:
        if "=" not in entry:
            raise HeaderUpdateError(f"Invalid --map entry '{entry}'. Expected format: name=/path/image.png")
        name, raw_path = entry.split("=", 1)
        name = name.strip()
        raw_path = raw_path.strip()
        if not name or not raw_path:
            raise HeaderUpdateError(f"Invalid --map entry '{entry}'.")
        out[name] = Path(raw_path).expanduser().resolve()
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description="Update NerdMiner image headers from PNG/JPG files.")
    parser.add_argument("--header", required=True, help="Path to images_*.h header file")
    parser.add_argument("--out", help="Output header path (default: overwrite --header)")
    parser.add_argument(
        "--images-dir",
        help="Directory with files named like <arrayName>.png (e.g. initScreen.png)",
    )
    parser.add_argument(
        "--map",
        action="append",
        default=[],
        help="Explicit array mapping: --map initScreen=/abs/path/init.png (repeatable)",
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="List arrays in header and expected dimensions/pixel counts, then exit",
    )
    parser.add_argument(
        "--require-all-mapped",
        action="store_true",
        help="Fail if any --map array name is not updated",
    )

    args = parser.parse_args()

    header_path = Path(args.header).expanduser().resolve()
    if not header_path.exists():
        raise HeaderUpdateError(f"Header does not exist: {header_path}")

    out_path = Path(args.out).expanduser().resolve() if args.out else header_path

    images_dir = Path(args.images_dir).expanduser().resolve() if args.images_dir else None
    if images_dir and not images_dir.exists():
        raise HeaderUpdateError(f"--images-dir does not exist: {images_dir}")

    image_map = parse_map_entries(args.map)

    text = header_path.read_text(encoding="utf-8", errors="ignore")
    arrays = find_arrays(text)

    if not arrays:
        raise HeaderUpdateError(f"No PROGMEM arrays found in {header_path}")

    if args.list:
        print(f"Header: {header_path}")
        for a in arrays:
            dims = f"{a.width_const}x{a.height_const}" if a.width_const and a.height_const else "n/a"
            expected = str(a.expected_pixels) if a.expected_pixels is not None else a.size_expr
            print(f"- {a.name}: size_expr={a.size_expr}, pixels={expected}, dims={dims}")
        return 0

    if not image_map and images_dir is None:
        raise HeaderUpdateError("Provide at least one input source: --images-dir and/or --map")

    new_text, updated = update_header(
        header_text=text,
        arrays=arrays,
        image_map=image_map,
        images_dir=images_dir,
        require_all_mapped=args.require_all_mapped,
    )

    if not updated:
        raise HeaderUpdateError(
            "No arrays were updated. Check image names (e.g. initScreen.png) or use --map name=/path/file.png"
        )

    out_path.write_text(new_text, encoding="utf-8")

    print(f"Updated {len(updated)} array(s) in {out_path}:")
    for name in updated:
        print(f"- {name}")

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except HeaderUpdateError as exc:
        print(f"Error: {exc}")
        raise SystemExit(1)
