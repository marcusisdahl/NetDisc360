"""Reads Aurora's art files (GameData\\<TitleId>_<Id>\\GC|GL|BK|SS<TitleId>.asset).

Each file holds up to 25 textures (icon, banner, box art, slot, background
and 20 screenshots) after a 2048-byte header, as documented in
github.com/jrobiche/xbox360-aurora-developer-documentation (Aurora 0.7b.2).
This prints every texture's header fields and writes each one as a PNG, so
the files Aurora made can be compared with the ones NetDisc 360 makes.

    python tools/aurora_asset.py <file.asset> [--png <folder>]
"""
import argparse
import json
import os
import struct
import sys
import zlib

HEADER = 2048
SLOTS = ["icon", "banner", "boxart", "slot", "background"] + ["screenshot%d" % i for i in range(1, 21)]
FORMATS = {0x06: "8_8_8_8", 0x12: "DXT1", 0x13: "DXT2_3", 0x14: "DXT4_5"}


def bits(value, mask, shift):
    return (value & mask) >> shift


def parse(data):
    if len(data) < HEADER or data[:4] != b"RXEA":
        raise ValueError("not an Aurora asset (no RXEA header)")
    version, length, types, screenshots = struct.unpack(">4I", data[4:20])
    textures = []
    for slot in range(25):
        at = 0x14 + slot * 0x40
        index, size, extended = struct.unpack(">3I", data[at:at + 12])
        common, references, fence, read_fence, identifier, base_flush, mip_flush = struct.unpack(">7I", data[at + 12:at + 40])
        c0, c1, c2, c3, c4, c5 = struct.unpack(">6I", data[at + 40:at + 64])
        if not references and not size:
            continue
        textures.append({
            "slot": SLOTS[slot], "index": slot, "dataIndex": index, "dataLength": size, "extendedInfo": "%08X" % extended,
            "common": common, "referenceCount": references, "fence": fence, "readFence": read_fence,
            "identifier": identifier, "baseFlush": "%08X" % base_flush, "mipFlush": "%08X" % mip_flush,
            "constants": ["%08X" % c for c in (c0, c1, c2, c3, c4, c5)],
            "tiled": bits(c0, 0x80000000, 31), "pitch": bits(c0, 0x7FC00000, 22), "fetchType": bits(c0, 3, 0),
            "clamp": [bits(c0, 0x1C00, 10), bits(c0, 0xE000, 13), bits(c0, 0x70000, 16)],
            "baseAddress": bits(c1, 0xFFFFF000, 12), "stacked": bits(c1, 0x400, 10), "requestSize": bits(c1, 0x300, 8),
            "endian": bits(c1, 0xC0, 6), "format": FORMATS.get(c1 & 0x3F, "0x%02X" % (c1 & 0x3F)), "formatCode": c1 & 0x3F,
            "width": bits(c2, 0x1FFF, 0) + 1, "height": bits(c2, 0x3FFE000, 13) + 1, "size2dTop": bits(c2, 0xFC000000, 26),
            "swizzle": [bits(c3, 0xE, 1), bits(c3, 0x70, 4), bits(c3, 0x380, 7), bits(c3, 0x1C00, 10)],
            "filters": {"mag": bits(c3, 0x180000, 19), "min": bits(c3, 0x600000, 21), "mip": bits(c3, 0x1800000, 23)},
            "constant4": "%08X" % c4,
            "mipAddress": bits(c5, 0xFFFFF000, 12), "packedMips": bits(c5, 0x800, 11), "dimension": bits(c5, 0x600, 9),
        })
    return {"version": version, "imageDataLength": length, "fileLength": len(data), "assetTypes": "%08X" % types,
            "screenshotCount": screenshots, "textures": textures}


def swap(data, endian):
    if endian == 1:  # 8 in 16
        out = bytearray(len(data))
        out[0::2], out[1::2] = data[1::2], data[0::2]
        return bytes(out)
    if endian == 2:  # 8 in 32
        out = bytearray(len(data))
        for k in range(4):
            out[k::4] = data[3 - k::4]
        return bytes(out)
    if endian == 3:  # 16 in 32
        out = bytearray(len(data))
        out[0::4], out[1::4], out[2::4], out[3::4] = data[2::4], data[3::4], data[0::4], data[1::4]
        return bytes(out)
    return data


def tiled_offset(x, y, width, log2_bpp):
    """Block (x, y) of a tiled Xenos texture width blocks wide -> block index."""
    aligned = (width + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (aligned >> 5)) << (log2_bpp + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bpp
    offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return (((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2)
            + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F)) >> log2_bpp


def colors565(c0, c1, four):
    def rgb(c):
        return ((c >> 11 & 31) * 255 // 31, (c >> 5 & 63) * 255 // 63, (c & 31) * 255 // 31)
    a, b = rgb(c0), rgb(c1)
    if four or c0 > c1:
        return [a, b, tuple((2 * p + q) // 3 for p, q in zip(a, b)), tuple((p + 2 * q) // 3 for p, q in zip(a, b))]
    return [a, b, tuple((p + q) // 2 for p, q in zip(a, b)), (0, 0, 0)]


def decode_block(block, kind):
    """16 RGBA pixels of one 4x4 block."""
    alpha = [255] * 16
    if kind == "DXT4_5":
        a0, a1 = block[0], block[1]
        table = [a0, a1] + ([((6 - i) * a0 + (1 + i) * a1) // 7 for i in range(6)] if a0 > a1 else
                            [((4 - i) * a0 + (1 + i) * a1) // 5 for i in range(4)] + [0, 255])
        index_bits = int.from_bytes(block[2:8], "little")
        alpha = [table[(index_bits >> (3 * i)) & 7] for i in range(16)]
        block = block[8:]
    elif kind == "DXT2_3":
        alpha = [((block[i // 2] >> (4 * (i & 1))) & 15) * 17 for i in range(16)]
        block = block[8:]
    c0, c1 = struct.unpack("<HH", block[:4])
    palette = colors565(c0, c1, kind != "DXT1")
    index_bits = struct.unpack("<I", block[4:8])[0]
    pixels = []
    for i in range(16):
        r, g, b = palette[(index_bits >> (2 * i)) & 3]
        a = alpha[i] if kind != "DXT1" or c0 > c1 or ((index_bits >> (2 * i)) & 3) != 3 else 0
        pixels.append((r, g, b, a))
    return pixels


def decode(data, texture):
    """The texture as rows of RGBA bytes, width x height, and the padded size it's stored at."""
    raw = swap(data[HEADER + texture["dataIndex"]:HEADER + texture["dataIndex"] + texture["dataLength"]], texture["endian"])
    kind = texture["format"]
    width, height = texture["width"], texture["height"]
    if kind == "8_8_8_8":
        block_w = block_h = 1
        block_bytes = 4
    elif kind in ("DXT1", "DXT2_3", "DXT4_5"):
        block_w = block_h = 4
        block_bytes = 8 if kind == "DXT1" else 16
    else:
        raise ValueError("texture format %s isn't handled" % kind)
    blocks_w = (width + block_w - 1) // block_w
    pitch_blocks = max(texture["pitch"] * 32 // block_w, blocks_w)
    rows_stored = len(raw) // (pitch_blocks * block_bytes)
    log2 = block_bytes.bit_length() - 1
    image = [bytearray(width * 4) for _ in range(height)]
    for by in range((height + block_h - 1) // block_h):
        for bx in range(blocks_w):
            n = tiled_offset(bx, by, pitch_blocks, log2) if texture["tiled"] else by * pitch_blocks + bx
            block = raw[n * block_bytes:(n + 1) * block_bytes]
            if len(block) < block_bytes:
                continue
            if kind == "8_8_8_8":
                # Stored A R G B after the endian swap.
                a, r, g, b = block
                pixels = [(r, g, b, a)]
            else:
                pixels = decode_block(block, kind)
            for i, pixel in enumerate(pixels):
                x, y = bx * block_w + i % block_w, by * block_h + i // block_w
                if x < width and y < height:
                    image[y][x * 4:x * 4 + 4] = bytes(pixel)
    return image, pitch_blocks * block_w, rows_stored * block_h


def write_png(path, rows):
    height, width = len(rows), len(rows[0]) // 4
    raw = b"".join(b"\0" + bytes(row) for row in rows)

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("asset")
    parser.add_argument("--png", help="folder to write each texture to as <file>.<slot>.png")
    args = parser.parse_args()
    with open(args.asset, "rb") as f:
        data = f.read()
    info = parse(data)
    for texture in info["textures"]:
        if args.png and texture["referenceCount"]:
            try:
                rows, stored_w, stored_h = decode(data, texture)
                texture["storedSize"] = [stored_w, stored_h]
                os.makedirs(args.png, exist_ok=True)
                name = "%s.%s.png" % (os.path.basename(args.asset), texture["slot"])
                write_png(os.path.join(args.png, name), rows)
                texture["png"] = name
            except ValueError as error:
                texture["png"] = str(error)
    json.dump(info, sys.stdout, indent=1)
    print()


if __name__ == "__main__":
    sys.exit(main())
