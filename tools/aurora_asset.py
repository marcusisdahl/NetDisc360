#!/usr/bin/env python3
"""Writes Aurora .asset files (the art Aurora keeps in Data/GameData/<TitleID>_<n>/).

Layout, as Aurora and Aurora Asset Editor use it: a big-endian header ("RXEA",
version 1, data size, flags with one bit per used entry, screenshot count), 25
entries of 64 bytes (offset, size, extended info, 52-byte D3D texture header),
zero padding to 2048, then each texture's data. Textures are DXT5, untiled,
16-bit byte swapped, with width and height padded to multiples of 128.
"""
import struct, sys
import numpy as np
from PIL import Image

ICON, BANNER, BOXART, BACKGROUND = 0, 1, 2, 4
ENTRIES = 25
SIZES = {ICON: (64, 64), BANNER: (420, 95), BOXART: (900, 600), BACKGROUND: (1920, 1080)}


def pad128(n):
    return (n + 127) & ~127


def dxt5(img):
    """DXT5-compresses an RGBA image whose sides are multiples of 4."""
    a = np.asarray(img.convert('RGBA'), dtype=np.float32)
    h, w = a.shape[:2]
    blocks = a.reshape(h // 4, 4, w // 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 4)
    out = np.zeros((len(blocks), 16), np.uint8)
    # Alpha: endpoints max/min, 8-step palette, 3-bit indices.
    amax = blocks[:, :, 3].max(1); amin = blocks[:, :, 3].min(1)
    a0 = np.round(amax).astype(np.int32); a1 = np.round(amin).astype(np.int32)
    out[:, 0] = a0; out[:, 1] = a1
    pal = np.stack([a0, a1] + [((7 - i) * a0 + i * a1) / 7 for i in range(1, 7)], 1)  # codes 0..7
    aidx = np.abs(blocks[:, :, 3][:, :, None] - pal[:, None, :]).argmin(2).astype(np.uint64)
    abits = np.zeros(len(blocks), np.uint64)
    for i in range(16): abits |= aidx[:, i] << np.uint64(3 * i)
    for i in range(6): out[:, 2 + i] = ((abits >> np.uint64(8 * i)) & np.uint64(0xFF)).astype(np.uint8)
    # Colour: bounding-box endpoints in RGB565, 4-step palette.
    rgb = blocks[:, :, :3]
    hi = rgb.max(1); lo = rgb.min(1)
    def to565(c):
        c = np.round(c).astype(np.int32)
        return ((c[:, 0] >> 3) << 11) | ((c[:, 1] >> 2) << 5) | (c[:, 2] >> 3)
    def from565(v):
        r = (v >> 11) & 31; g = (v >> 5) & 63; b = v & 31
        return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], 1).astype(np.float32)
    c0 = to565(hi); c1 = to565(lo)
    swap = c0 < c1
    c0, c1 = np.where(swap, c1, c0), np.where(swap, c0, c1)
    p0 = from565(c0); p1 = from565(c1)
    cpal = np.stack([p0, p1, (2 * p0 + p1) / 3, (p0 + 2 * p1) / 3], 1)
    cidx = ((rgb[:, :, None, :] - cpal[:, None, :, :]) ** 2).sum(3).argmin(2).astype(np.uint32)
    cidx = np.where((c0 == c1)[:, None], 0, cidx)
    cbits = np.zeros(len(blocks), np.uint32)
    for i in range(16): cbits |= cidx[:, i] << np.uint32(2 * i)
    out[:, 8] = c0 & 0xFF; out[:, 9] = c0 >> 8; out[:, 10] = c1 & 0xFF; out[:, 11] = c1 >> 8
    for i in range(4): out[:, 12 + i] = (cbits >> np.uint32(8 * i)) & 0xFF
    return out.reshape(h // 4, w // 4, 16)


def texture(img, kind):
    w, h = SIZES[kind]
    img = img.convert('RGBA').resize((w, h), Image.LANCZOS) if img.size != (w, h) else img.convert('RGBA')
    pw, ph = pad128(w), pad128(h)
    canvas = Image.new('RGBA', (pw, ph), (0, 0, 0, 0)); canvas.paste(img, (0, 0))
    data = dxt5(canvas).tobytes()
    data = bytes(np.frombuffer(data, np.uint8).reshape(-1, 2)[:, ::-1].tobytes())  # 8-in-16 swap
    fetch = [(pw // 32) << 22 | 2, 0x54, (h - 1) << 13 | (w - 1), 0xD10, 0, 0xA00]
    header = struct.pack('>13I', 3, 1, 0, 0, 0, 0xFFFF0000, 0xFFFF0000, *fetch)
    return header, data


def asset(images):
    """images: {entry index: PIL image}. Returns the .asset file bytes."""
    table, blobs, offset, flags = [], [], 0, 0
    for i in range(ENTRIES):
        if i in images:
            header, data = texture(images[i], i)
            table.append(struct.pack('>3I', offset, len(data), 0) + header)
            blobs.append(data); offset += len(data); flags |= 1 << i
        else:
            table.append(bytes(64))
    head = struct.pack('>5I', 0x52584541, 1, offset, flags, 0) + b''.join(table)
    return head + bytes(2048 - len(head)) + b''.join(blobs)


if __name__ == '__main__':
    # tools/aurora_asset.py <title id> <out dir> cover=... background=... icon=... banner=...
    tid, outdir = sys.argv[1].upper(), sys.argv[2]
    args = dict(a.split('=', 1) for a in sys.argv[3:])
    files = {'GC': {BOXART: args['cover']}, 'BK': {BACKGROUND: args['background']},
             'GL': {ICON: args['icon'], BANNER: args['banner']}}
    for prefix, entries in files.items():
        data = asset({k: Image.open(v) for k, v in entries.items()})
        with open(f'{outdir}/{prefix}{tid}.asset', 'wb') as f: f.write(data)
        print(prefix, len(data))
