#!/usr/bin/env python3
"""
Generate store icons for pebble-yes-watch (no external deps).

Outputs:
  - store/icon_80.png
  - store/icon_144.png
"""

from __future__ import annotations

import binascii
import math
import os
import struct
import zlib


def _crc32(data: bytes) -> int:
    return binascii.crc32(data) & 0xFFFFFFFF


def _png_chunk(chunk_type: bytes, data: bytes) -> bytes:
    assert len(chunk_type) == 4
    length = struct.pack(">I", len(data))
    crc = struct.pack(">I", _crc32(chunk_type + data))
    return length + chunk_type + data + crc


def write_png_rgba(path: str, w: int, h: int, rgba: bytes) -> None:
    if len(rgba) != w * h * 4:
        raise ValueError("rgba length mismatch")

    # Filter type 0 per scanline.
    raw = bytearray()
    stride = w * 4
    for y in range(h):
        raw.append(0)
        raw.extend(rgba[y * stride : (y + 1) * stride])

    compressed = zlib.compress(bytes(raw), level=9)

    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)  # 8-bit RGBA
    png = bytearray()
    png.extend(sig)
    png.extend(_png_chunk(b"IHDR", ihdr))
    png.extend(_png_chunk(b"IDAT", compressed))
    png.extend(_png_chunk(b"IEND", b""))

    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(png)


def clamp01(x: float) -> float:
    return 0.0 if x < 0.0 else 1.0 if x > 1.0 else x


def lerp(a: float, b: float, t: float) -> float:
    return a + (b - a) * t


def mix(c0, c1, t: float):
    return (
        int(round(lerp(c0[0], c1[0], t))),
        int(round(lerp(c0[1], c1[1], t))),
        int(round(lerp(c0[2], c1[2], t))),
        int(round(lerp(c0[3], c1[3], t))),
    )


def icon_rgba(size: int, supersample: int = 4) -> bytes:
    S = supersample
    W = H = size * S
    cx = cy = (W - 1) / 2.0

    # Palette (roughly matching the watchface)
    BG = (10, 10, 12, 255)
    RING = (220, 220, 224, 255)
    RING_INNER = (70, 70, 74, 255)
    NIGHT = (7, 24, 88, 255)
    DAY = (122, 190, 198, 255)
    HAND = (232, 232, 236, 255)
    HUB = (210, 210, 214, 255)
    OUTLINE = (25, 25, 28, 255)

    r_face = (W * 0.48)
    ring_th = W * 0.07
    r_ring_out = r_face
    r_ring_in = r_face - ring_th
    r_disk = r_ring_in - W * 0.02

    # Day wedge (top) angles: clockwise, 0 at top.
    # Make a pleasant wide daylight sector for the icon.
    a0 = math.radians(-65)
    a1 = math.radians(+65)

    # Hand angle (pointing to ~2 o'clock)
    a_hand = math.radians(+65)

    # Moon disk at bottom
    moon_cy = cy + W * 0.18
    moon_r = W * 0.09
    moon_dx = moon_r * 0.45

    # Supersampled buffer
    buf = bytearray(W * H * 4)

    def put(ix: int, iy: int, c):
        o = (iy * W + ix) * 4
        buf[o : o + 4] = bytes((c[0], c[1], c[2], c[3]))

    for y in range(H):
        for x in range(W):
            dx = x - cx
            dy = y - cy
            rr = math.hypot(dx, dy)
            c = BG

            # Outer ring + subtle inner ring shading
            if rr <= r_ring_out:
                c = RING
            if rr <= r_ring_in:
                c = RING_INNER

            # Inner disk: base night
            if rr <= r_disk:
                c = NIGHT
                # Day wedge overlay
                ang = math.atan2(dx, -dy)  # 0 at top, clockwise positive
                # Normalize to [-pi, pi]
                if ang < -math.pi:
                    ang += 2 * math.pi
                if a0 <= ang <= a1:
                    c = DAY

            # Hand: a tapered triangle
            # Represent hand in polar; if within a narrow angular band and within radius, paint.
            if rr <= r_disk * 0.88:
                # smallest signed angle difference
                ang = math.atan2(dx, -dy)
                da = (ang - a_hand + math.pi) % (2 * math.pi) - math.pi
                # thickness tapers with radius
                t = rr / (r_disk * 0.88)
                half_w = (W * 0.012) * (1.25 - 0.9 * t)
                # angular width equivalent at this radius
                if rr > 1e-6:
                    max_da = half_w / rr
                    if abs(da) <= max_da and rr >= W * 0.03:
                        c = HAND

            # Hub
            if rr <= W * 0.04:
                c = HUB

            # Moon phase disk (simple: white disk + black shifted mask)
            mdx = x - cx
            mdy = y - moon_cy
            mrr = math.hypot(mdx, mdy)
            if mrr <= moon_r:
                c = (240, 240, 244, 255)
                # show a slight waning gibbous: black mask shifted right
                if math.hypot(mdx + moon_dx, mdy) <= moon_r:
                    c = (14, 14, 16, 255)

            # Tiny outlines for crispness at small sizes
            if abs(rr - r_ring_out) <= 1.2 or abs(rr - r_ring_in) <= 1.2:
                c = mix(c, OUTLINE, 0.35)

            put(x, y, c)

    # Downsample box filter
    out = bytearray(size * size * 4)
    for oy in range(size):
        for ox in range(size):
            r = g = b = a = 0
            for sy in range(S):
                for sx in range(S):
                    ix = ox * S + sx
                    iy = oy * S + sy
                    o = (iy * W + ix) * 4
                    r += buf[o + 0]
                    g += buf[o + 1]
                    b += buf[o + 2]
                    a += buf[o + 3]
            n = S * S
            oo = (oy * size + ox) * 4
            out[oo + 0] = r // n
            out[oo + 1] = g // n
            out[oo + 2] = b // n
            out[oo + 3] = a // n
    return bytes(out)


def main() -> None:
    for sz in (80, 144):
        rgba = icon_rgba(sz, supersample=4)
        write_png_rgba(f"store/icon_{sz}.png", sz, sz, rgba)
    print("Wrote store/icon_80.png and store/icon_144.png")


if __name__ == "__main__":
    main()


