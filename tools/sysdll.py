"""Turns imagexex's title DLL into a system DLL, the kind DashLaunch loads
as a plugin (like NetISO.xex: module flags 0xA, 4 KB pages, loaded at
0x91xxxxxx).

The public imagexex refuses any base at 0x90000000 or above (IM1038), and
with <sysdll/> any title base (IM1067). So build.ps1 links the plugin at its
system base, patches only the PE header's ImageBase to a title base (pe-base),
lets imagexex package that (no <sysdll/>, uncompressed), and then this script
(xex) moves the XEX's headers to the system base. The image bytes are not
touched: the code was linked for the system base all along.

What changes, and the hash chains that change with it (checked against
NetISO.xex 1.0 and the FreestylePlugin Nova.xex, see --check):
- module flags: title module (0x1) off, exports to title (0x2) on;
- image base (0x10201), entry point (0x10100), load address (security
  +0x110), export table (+0x160), resource addresses (0x2FF) and each
  import record: moved by the difference between the bases;
- image flags (+0x10C): 4 KB pages (0x10000000), so each page descriptor
  counts 16 times as many pages;
- page descriptors: each one's SHA-1 covers the next block's data and the
  next descriptor; the image hash (+0x114) covers the first block and
  descriptor;
- import libraries: the security info's import digest (+0x12C) is SHA-1 of
  the first library from its offset 4, each library's digest that of the
  next one, the last zero;
- file format: written unencrypted;
- header digest (+0x164), as in shared/library.cpp FixHeaderDigest.

  python tools/sysdll.py pe-base IN.dll OUT.dll 0x88000000
  python tools/sysdll.py xex IN.xex OUT.xex 0x91380000
  python tools/sysdll.py check FILE.xex
"""
import hashlib
import struct
import sys

SMALL_PAGES = 0x10000000
RETAIL_KEY = bytes.fromhex('20B185A59D28FDC340583FBB0896BF91')
DEVKIT_KEY = bytes(16)

# ---- AES-128 decryption (the image key and data), standard library only ----

SBOX = [0] * 256
INV_SBOX = [0] * 256


def _init_sbox():
    p = q = 1
    while True:
        p ^= ((p << 1) ^ (0x1B if p & 0x80 else 0)) & 0xFF
        for shift in (1, 2, 4):
            q ^= q << shift
        q &= 0xFF
        if q & 0x80:
            q ^= 0x09
        x = q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) ^ ((q << 3) | (q >> 5)) ^ ((q << 4) | (q >> 4))
        SBOX[p] = (x ^ 0x63) & 0xFF
        if p == 1:
            break
    SBOX[0] = 0x63
    for i, v in enumerate(SBOX):
        INV_SBOX[v] = i


_init_sbox()


def _mul(a, b):
    r = 0
    while b:
        if b & 1:
            r ^= a
        a = ((a << 1) ^ (0x1B if a & 0x80 else 0)) & 0xFF
        b >>= 1
    return r


def _round_keys(key):
    words = [list(key[i:i + 4]) for i in range(0, 16, 4)]
    rcon = 1
    for i in range(4, 44):
        t = list(words[i - 1])
        if i % 4 == 0:
            t = [SBOX[b] for b in t[1:] + t[:1]]
            t[0] ^= rcon
            rcon = _mul(rcon, 2)
        words.append([a ^ b for a, b in zip(words[i - 4], t)])
    return [sum(words[r * 4:r * 4 + 4], []) for r in range(11)]


def _decrypt_block(keys, block):
    s = [a ^ b for a, b in zip(block, keys[10])]
    for r in range(9, -1, -1):
        s = [s[(i + 4 * (i % 4) * 3) % 16] for i in range(16)]  # inverse shift rows
        s = [INV_SBOX[b] for b in s]
        s = [a ^ b for a, b in zip(s, keys[r])]
        if r:
            t = []
            for c in range(4):
                a = s[c * 4:c * 4 + 4]
                t += [_mul(a[0], 14) ^ _mul(a[1], 11) ^ _mul(a[2], 13) ^ _mul(a[3], 9),
                      _mul(a[0], 9) ^ _mul(a[1], 14) ^ _mul(a[2], 11) ^ _mul(a[3], 13),
                      _mul(a[0], 13) ^ _mul(a[1], 9) ^ _mul(a[2], 14) ^ _mul(a[3], 11),
                      _mul(a[0], 11) ^ _mul(a[1], 13) ^ _mul(a[2], 9) ^ _mul(a[3], 14)]
            s = t
    return bytes(s)


def aes_ecb_decrypt(key, data):
    keys = _round_keys(key)
    return b''.join(_decrypt_block(keys, data[i:i + 16]) for i in range(0, len(data), 16))


def aes_cbc_decrypt(key, data):
    keys = _round_keys(key)
    out, previous = [], bytes(16)
    for i in range(0, len(data) // 16 * 16, 16):
        block = data[i:i + 16]
        out.append(bytes(a ^ b for a, b in zip(_decrypt_block(keys, block), previous)))
        previous = block
    return b''.join(out) + data[len(data) // 16 * 16:]


# ---- XEX2 ----

def u32(data, at):
    return struct.unpack_from('>I', data, at)[0]


def put32(data, at, value):
    struct.pack_into('>I', data, at, value & 0xFFFFFFFF)


class Xex:
    def __init__(self, data):
        self.x = bytearray(data)
        if self.x[:4] != b'XEX2':
            raise SystemExit('not an XEX2 file')
        self.data_at = u32(self.x, 8)
        self.security = u32(self.x, 16)
        self.headers = {}  # key -> offset of the directory entry
        for i in range(u32(self.x, 20)):
            self.headers[u32(self.x, 24 + i * 8)] = 24 + i * 8

    def value(self, key):
        return u32(self.x, self.headers[key] + 4)

    def set_value(self, key, value):
        put32(self.x, self.headers[key] + 4, value)

    def format(self):
        at = self.value(0x3FF)
        size, encryption, compression = struct.unpack_from('>IHH', self.x, at)
        blocks = []
        if compression == 1:
            for i in range((size - 8) // 8):
                blocks.append(struct.unpack_from('>II', self.x, at + 8 + i * 8))
        return at, encryption, compression, blocks

    def payload(self):
        """The file's image data, decrypted (raw, as stored) and the loaded image."""
        at, encryption, compression, blocks = self.format()
        if compression not in (0, 1):
            raise SystemExit('the XEX is LZX compressed; package it with <format><raw/></format>')
        raw = bytes(self.x[self.data_at:])
        if encryption:
            sealed = bytes(self.x[self.security + 0x150:self.security + 0x160])
            for key in (RETAIL_KEY, DEVKIT_KEY):
                plain = aes_cbc_decrypt(aes_ecb_decrypt(key, sealed), raw)
                if plain[:2] == b'MZ':
                    raw = plain
                    break
            else:
                raise SystemExit('cannot decrypt the image with the retail or devkit key')
        image, p = bytearray(), 0
        if compression == 0:
            image += raw
        for data_size, zero_size in blocks:
            image += raw[p:p + data_size] + bytes(zero_size)
            p += data_size
        # The loaded image is the security info's image size; what the file
        # doesn't store is zeros.
        size = u32(self.x, self.security + 4)
        return raw, bytes(image[:size]).ljust(size, b'\0')

    def descriptors(self):
        count = u32(self.x, self.security + 0x180)
        return [self.security + 0x184 + i * 24 for i in range(count)]

    def page_size(self):
        return 4096 if u32(self.x, self.security + 0x10C) & SMALL_PAGES else 65536

    def libraries(self):
        if 0x103FF not in self.headers:
            return []
        at = self.value(0x103FF)
        string_size, count = u32(self.x, at + 4), u32(self.x, at + 8)
        p, libraries = at + 12 + string_size, []
        for _ in range(count):
            libraries.append(p)
            p += u32(self.x, p)
        return libraries

    # The three hash chains.

    def page_hashes(self, image, apply=False):
        descriptors, page = self.descriptors(), self.page_size()
        blocks, p = [], 0
        for d in descriptors:
            size = (u32(self.x, d) >> 4) * page
            blocks.append(image[p:p + size])
            p += size
        # Each hash covers the next descriptor, hash included, so they are
        # computed from the last one back; apply=True writes each before the
        # one before it is computed.
        result = {}
        for i in range(len(descriptors) - 1, -1, -1):
            d = descriptors[i]
            if i + 1 < len(descriptors):
                nxt = descriptors[i + 1]
                result[d + 4] = hashlib.sha1(blocks[i + 1] + bytes(self.x[nxt:nxt + 24])).digest()
            else:
                result[d + 4] = bytes(20)
            if apply:
                self.x[d + 4:d + 24] = result[d + 4]
        if descriptors:
            result[self.security + 0x114] = hashlib.sha1(blocks[0] + bytes(self.x[descriptors[0]:descriptors[0] + 24])).digest()
        return result

    def import_hashes(self, apply=False):
        libraries, result = self.libraries(), {}
        for i in range(len(libraries) - 1, -1, -1):
            at = libraries[i]
            if i + 1 < len(libraries):
                nxt = libraries[i + 1]
                result[at + 4] = hashlib.sha1(bytes(self.x[nxt + 4:nxt + u32(self.x, nxt)])).digest()
            else:
                result[at + 4] = bytes(20)
            if apply:
                self.x[at + 4:at + 24] = result[at + 4]
        if libraries:
            first = libraries[0]
            result[self.security + 0x12C] = hashlib.sha1(bytes(self.x[first + 4:first + u32(self.x, first)])).digest()
        return result

    def header_hash(self):
        s = self.security
        return {s + 0x164: hashlib.sha1(bytes(self.x[s + 0x17C:self.data_at]) + bytes(self.x[:s + 8])).digest()}

    def apply(self, hashes):
        for at, digest in sorted(hashes.items()):
            self.x[at:at + 20] = digest

    def mismatches(self, hashes):
        return [hex(at) for at, digest in sorted(hashes.items()) if bytes(self.x[at:at + 20]) != digest]


def check(xex, image):
    """Hash fields that don't match the file: [] for a consistent XEX."""
    return xex.mismatches(xex.page_hashes(image)) + xex.mismatches(xex.import_hashes()) + xex.mismatches(xex.header_hash())


def to_system(path_in, path_out, base):
    xex = Xex(open(path_in, 'rb').read())
    x, s = xex.x, xex.security
    flags = u32(x, 4)
    if not flags & 0x8:
        raise SystemExit('not a DLL (module flags %08X)' % flags)
    for key in (0x20104,):
        if key in xex.headers:
            raise SystemExit('optional header %X would need moving too' % key)
    if 0x10201 not in xex.headers or 0x10100 not in xex.headers:
        raise SystemExit('no image base or entry point header')
    raw, image = xex.payload()
    old = xex.value(0x10201)
    if old != u32(x, s + 0x110):
        raise SystemExit('image base %08X and load address %08X differ' % (old, u32(x, s + 0x110)))
    delta = base - old
    put32(x, 4, (flags & ~0x1) | 0x2)
    xex.set_value(0x10201, base)
    xex.set_value(0x10100, xex.value(0x10100) + delta)
    put32(x, s + 0x110, base)
    if u32(x, s + 0x160):
        put32(x, s + 0x160, u32(x, s + 0x160) + delta)
    # The PE was linked at the system base; the title base was only what
    # imagexex saw. The resource table (0x2FF: a size, then 8-byte names with
    # an address and a size each) holds addresses too.
    if 0x10001 in xex.headers:
        xex.set_value(0x10001, base)
    if 0x2FF in xex.headers:
        at = xex.value(0x2FF)
        for p in range(at + 4, at + u32(x, at), 16):
            put32(x, p + 8, u32(x, p + 8) + delta)
    for at in xex.libraries():
        count = struct.unpack_from('>H', x, at + 38)[0]
        for i in range(count):
            put32(x, at + 40 + i * 4, u32(x, at + 40 + i * 4) + delta)
    if xex.page_size() == 65536:
        for d in xex.descriptors():
            info = u32(x, d)
            put32(x, d, ((info >> 4) * 16) << 4 | (info & 0xF))
        put32(x, s + 0x10C, u32(x, s + 0x10C) | SMALL_PAGES)
    at, encryption, _, _ = xex.format()
    if encryption:
        struct.pack_into('>H', x, at + 4, 0)
        x[xex.data_at:] = raw
    xex.apply(xex.page_hashes(image, apply=True))
    xex.apply(xex.import_hashes(apply=True))
    xex.apply(xex.header_hash())
    left = check(xex, image)
    if left:
        raise SystemExit('hashes still wrong at ' + ', '.join(left))
    open(path_out, 'wb').write(bytes(x))
    print('%s: system DLL at %08X (was %08X), entry %08X, %d import libraries, %d page descriptors'
          % (path_out, base, old, xex.value(0x10100), len(xex.libraries()), len(xex.descriptors())))


def pe_base(path_in, path_out, base):
    pe = bytearray(open(path_in, 'rb').read())
    nt = struct.unpack_from('<I', pe, 0x3C)[0]
    if pe[nt:nt + 4] != b'PE\0\0' or struct.unpack_from('<H', pe, nt + 24)[0] != 0x10B:
        raise SystemExit('not a PE32 file')
    old = struct.unpack_from('<I', pe, nt + 24 + 28)[0]
    struct.pack_into('<I', pe, nt + 24 + 28, base)
    open(path_out, 'wb').write(bytes(pe))
    print('%s: PE image base %08X (linked at %08X)' % (path_out, base, old))


def main(argv):
    if len(argv) == 3 and argv[1] == 'check':
        xex = Xex(open(argv[2], 'rb').read())
        left = check(xex, xex.payload()[1])
        print('%s: %s' % (argv[2], 'hashes match' if not left else 'hashes wrong at ' + ', '.join(left)))
        return 1 if left else 0
    if len(argv) == 5 and argv[1] in ('xex', 'pe-base'):
        (to_system if argv[1] == 'xex' else pe_base)(argv[2], argv[3], int(argv[4], 0))
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
