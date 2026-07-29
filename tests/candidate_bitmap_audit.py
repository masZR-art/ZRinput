#!/usr/bin/env python3
"""Audit the deterministic, offscreen Win11 candidate-window bitmap."""

from __future__ import annotations

from collections import Counter
from pathlib import Path
import struct
import sys


def fail(message: str) -> None:
    raise AssertionError(message)


def read_bmp(path: Path) -> tuple[int, int, list[tuple[int, int, int]]]:
    data = path.read_bytes()
    if data[:2] != b"BM" or len(data) < 54:
        fail("preview is not a valid BMP")
    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    width, signed_height = struct.unpack_from("<ii", data, 18)
    bits_per_pixel = struct.unpack_from("<H", data, 28)[0]
    if width <= 0 or signed_height == 0 or bits_per_pixel != 32:
        fail("preview must be an uncompressed 32-bit BMP")
    height = abs(signed_height)
    row_size = width * 4
    if pixel_offset + row_size * height > len(data):
        fail("preview bitmap payload is truncated")
    rows: list[list[tuple[int, int, int]]] = []
    for row in range(height):
        offset = pixel_offset + row * row_size
        pixels = []
        for column in range(width):
            blue, green, red, _unused = struct.unpack_from(
                "<BBBB", data, offset + column * 4
            )
            pixels.append((red, green, blue))
        rows.append(pixels)
    if signed_height > 0:
        rows.reverse()
    return width, height, [pixel for row in rows for pixel in row]


def bounds(
    pixels: list[tuple[int, int, int]],
    width: int,
    color: tuple[int, int, int],
    minimum_x: int,
    maximum_x: int,
) -> tuple[int, int, int, int] | None:
    points = [
        (index % width, index // width)
        for index, pixel in enumerate(pixels)
        if pixel == color and minimum_x <= index % width <= maximum_x
    ]
    if not points:
        return None
    return (
        min(point[0] for point in points),
        min(point[1] for point in points),
        max(point[0] for point in points),
        max(point[1] for point in points),
    )


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: candidate_bitmap_audit.py BITMAP", file=sys.stderr)
        return 2
    width, height, pixels = read_bmp(Path(sys.argv[1]))
    if (width, height) != (731, 41):
        fail(f"preview geometry drifted: {width}x{height}")

    colors = Counter(pixels)
    if colors[(44, 44, 44)] < 23_000:
        fail("Microsoft dark background is missing")
    if colors[(255, 255, 255)] < 500:
        fail("candidate and tool text did not render")
    if bounds(pixels, width, (56, 56, 56), 0, 99) != (12, 5, 89, 37):
        fail("selected candidate block geometry drifted")
    if bounds(pixels, width, (179, 193, 224), 0, 99) != (12, 12, 15, 29):
        fail("accent line geometry or color drifted")
    if bounds(pixels, width, (61, 61, 61), 580, 700) != (592, 3, 695, 38):
        fail("tool separators no longer match the Microsoft reference")
    if bounds(pixels, width, (56, 56, 56), 690, 730) != (701, 9, 725, 33):
        fail("menu button geometry drifted")
    chromatic = [
        (index % width, index // width, pixel)
        for index, pixel in enumerate(pixels)
        if max(pixel) - min(pixel) > 5
    ]
    if not chromatic or any(
        pixel != (179, 193, 224) or not (12 <= x <= 15 and 12 <= y <= 29)
        for x, y, pixel in chromatic
    ):
        fail("candidate text or icons contain ClearType color fringes")
    print("Candidate bitmap audit passed: 731x41 reference geometry.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, OSError) as error:
        print(f"FAILED: {error}", file=sys.stderr)
        sys.exit(1)
