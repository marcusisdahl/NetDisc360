"""Tests compressed games (server/compress.cpp): converting ISOs, and ISOs in
.zip and .7z archives, to CSO files, and serving those like the ISO.

    python tests/test_compress.py
"""
import os
import random
import shutil
import struct
import subprocess
import sys
import time
import unittest
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from test_generator import execution_info, xex  # noqa: E402
from test_server import SERVER, Client, ServerTest, free_port  # noqa: E402

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
XGD2, XGD2_IMAGE = 0xFD90000, 7838695424
TITLE = 0x4D5307E6


def upper_key(name):
    return [ord(c.upper()) for c in name]


def write_disc(path, tree, start=0, size=None, noise=True, seed=1):
    """A disc image of tree {name: bytes or {name: ...}} with its game
    partition at start, each table a search tree as on a real disc. With
    noise, every byte no file uses is random, as on a retail disc (only
    around the files when size makes the image big and sparse).
    Returns {path: (offset, data)} of the files."""
    rng = random.Random(seed)
    sectors = [40]
    files, tables = {}, []

    def place(nbytes):
        at = sectors[0]
        sectors[0] += max(1, (nbytes + SECTOR - 1) // SECTOR) + 1  # A sector of noise between.
        return at

    def directory(node, prefix):
        names = sorted(node, key=upper_key)
        order, left, right = [], {}, {}

        def build(lo, hi):
            if lo >= hi:
                return None
            mid = (lo + hi) // 2
            order.append(names[mid])
            left[names[mid]], right[names[mid]] = build(lo, mid), build(mid + 1, hi)
            return names[mid]

        build(0, len(names))
        at, pos = {}, 0
        for name in order:
            need = (14 + len(name) + 3) & ~3
            if pos // SECTOR != (pos + need - 1) // SECTOR:
                pos = (pos // SECTOR + 1) * SECTOR  # Entries don't cross sectors.
            at[name] = pos
            pos += need
        table = bytearray(b"\xff" * max(SECTOR, (pos + SECTOR - 1) // SECTOR * SECTOR))
        sector = place(len(table))
        tables.append((sector, table))
        for name in order:
            value = node[name]
            if isinstance(value, dict):
                child, child_size = directory(value, prefix + name + "/")
                entry_sector, entry_size, attributes = child, child_size, 0x10
            else:
                entry_sector, entry_size, attributes = place(len(value)), len(value), 0x80
                files[prefix + name] = (start + entry_sector * SECTOR, value)
            struct.pack_into("<HHIIBB", table, at[name], at[left[name]] // 4 if left[name] else 0,
                             at[right[name]] // 4 if right[name] else 0, entry_sector, entry_size, attributes, len(name))
            table[at[name] + 14:at[name] + 14 + len(name)] = name.encode()
        return sector, len(table)

    root, root_size = directory(tree, "")
    end = start + sectors[0] * SECTOR
    size = size or end + 64 * 1024
    with open(path, "wb") as f:
        f.truncate(size)
        if noise and size <= 64 << 20:
            f.write(rng.randbytes(size))
        elif noise:  # Noise around the start and the files only.
            for at in (0, start, end):
                f.seek(at)
                f.write(rng.randbytes(1 << 20))
        volume = bytearray(rng.randbytes(SECTOR) if noise else bytes(SECTOR))
        volume[0:28] = MAGIC + struct.pack("<II", root, root_size)
        volume[0x7EC:0x800] = MAGIC
        f.seek(start + 32 * SECTOR)
        f.write(volume)
        for sector, table in tables:
            f.seek(start + sector * SECTOR)
            f.write(table)
        for offset, data in files.values():
            f.seek(offset)
            f.write(data)
    return files


def game_tree(seed=2):
    rng = random.Random(seed)
    return {
        "default.xex": xex(execution_info(0x1234, TITLE), filler=b"GAME CODE" * 1000),
        "media": {"intro.wmv": rng.randbytes(300000), "empty.bin": b"", "text.txt": b"hello " * 40000},
        "maps": {"a%02d.map" % i: rng.randbytes(rng.randrange(1, 90000)) for i in range(30)},
        "deep": {"er": {"est": {"file.dat": rng.randbytes(5000)}}},
    }


def compress(*args):
    return subprocess.run([SERVER, "--compress"] + list(args), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=600)


def raw_deflate(data, level=9):
    c = zlib.compressobj(level, zlib.DEFLATED, -15)
    return c.compress(data) + c.flush()


def write_cso(path, image, block=2048, shift=0):
    """A CSO file as other tools write it (maxcso's defaults: 2 KB blocks)."""
    blocks = (len(image) + block - 1) // block
    index, body = [], bytearray()
    at = 0x18 + 4 * (blocks + 1)
    for b in range(blocks):
        data = image[b * block:(b + 1) * block]
        packed = raw_deflate(data)
        pad = -(at + len(body)) % (1 << shift)
        body += b"\0" * pad
        where = (at + len(body)) >> shift
        if len(packed) >= len(data):
            index.append(where | 0x80000000)
            body += data
        else:
            index.append(where)
            body += packed
    body += b"\0" * (-(at + len(body)) % (1 << shift))
    index.append((at + len(body)) >> shift)
    with open(path, "wb") as f:
        f.write(b"CISO" + struct.pack("<IQIBBH", 0x18, len(image), block, 1, shift, 0))
        f.write(struct.pack("<%dI" % len(index), *index))
        f.write(body)


class Compress(ServerTest):
    def start_on(self, folder, *extra):
        self.port = free_port()
        self.server = subprocess.Popen([SERVER, folder, "--port", str(self.port), "--no-push", "--web-port", "0"] + list(extra),
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        deadline = time.time() + 10
        while time.time() < deadline:
            if "Listening on port" in self.server.stdout.readline():
                return
        self.fail("server didn't start")

    def served(self, folder, name):
        """A client with name mounted from a server on folder."""
        self.start_on(folder)
        c = Client(self.port)
        self.addCleanup(c.close)
        deadline = time.time() + 10
        while name not in c.names():
            self.assertLess(time.time(), deadline, c.names())
            time.sleep(0.2)
        self.assertTrue(c.mount(name))
        return c

    def check_files(self, c, files):
        for path, (offset, data) in files.items():
            for at in range(0, len(data), 65536):
                self.assertEqual(c.read(offset + at, min(65536, len(data) - at)), data[at:at + 65536], path)

    def game(self, name="Game.iso", **options):
        iso = os.path.join(self.isos, name)
        return iso, write_disc(iso, game_tree(), **options)

    def test_an_iso_drops_the_space_no_file_uses(self):
        iso, files = self.game()
        result = compress(iso)
        self.assertEqual(result.returncode, 0, result.stdout)
        cso = os.path.join(self.isos, "Game.cso")
        self.assertFalse(os.path.exists(cso + ".part"))
        # The random bytes between the files are gone; what the files hold is kept.
        used = sum(len(d) for _, d in files.values())
        self.assertLess(os.path.getsize(cso), used + 300000, result.stdout)
        with open(iso, "rb") as f:
            original = f.read()
        moved = os.path.join(self.tmp.name, "served")
        os.makedirs(moved)
        shutil.move(cso, moved)
        c = self.served(moved, "Game.iso")
        self.assertEqual(c.size(), (len(original) // SECTOR, SECTOR))
        self.check_files(c, files)
        # The volume descriptor is kept, the noise past the files is zeros.
        self.assertEqual(c.read(0x10000, 20), MAGIC)
        self.assertEqual(c.read(len(original) - 4096, 4096), bytes(4096))
        # A read across blocks and the very end.
        self.assertEqual(c.read(0x10000 + 0x7EC, 20), MAGIC)
        self.assertEqual(len(c.read(len(original) - 100, 100)), 100)

    def test_whole_keeps_every_byte(self):
        iso, _ = self.game()
        result = compress("--whole", iso)
        self.assertEqual(result.returncode, 0, result.stdout)
        with open(iso, "rb") as f:
            original = f.read()
        os.remove(iso)
        c = self.served(self.isos, "Game.iso")
        got = b"".join(c.read(at, min(65536, len(original) - at)) for at in range(0, len(original), 65536))
        self.assertEqual(got, original)

    def test_a_disc_without_a_file_table_is_kept_whole(self):
        iso = os.path.join(self.isos, "Odd.iso")
        data = random.Random(5).randbytes(3 << 20)
        with open(iso, "wb") as f:
            f.write(data)
        result = compress(iso)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("kept whole", result.stdout)
        os.remove(iso)
        c = self.served(self.isos, "Odd.iso")
        self.assertEqual(c.read(1 << 20, 65536), data[1 << 20:(1 << 20) + 65536])

    def test_an_xgd2_disc_keeps_its_size(self):
        iso = os.path.join(self.isos, "Big.iso")
        files = write_disc(iso, game_tree(), start=XGD2, size=XGD2_IMAGE)
        with open(iso, "rb") as f:
            first = f.read(0x10000)
        result = compress(iso)
        self.assertEqual(result.returncode, 0, result.stdout)
        cso = os.path.join(self.isos, "Big.cso")
        self.assertLess(os.path.getsize(cso), 20 << 20)
        os.remove(iso)
        c = self.served(self.isos, "Big.iso")
        self.assertEqual(c.size(), (XGD2_IMAGE // SECTOR, SECTOR))
        self.check_files(c, files)
        self.assertEqual(c.read(XGD2 + 0x10000, 20), MAGIC)
        self.assertEqual(c.read(0, 0x10000), first)  # The image's first 64 KB are kept as they were.
        self.assertEqual(c.read(XGD2 - 65536, 65536), bytes(65536))  # The video partition's noise is gone.

    def test_a_zip(self):
        iso, files = self.game("Halo 3 (USA).iso")
        archive = os.path.join(self.isos, "Halo 3 (USA).zip")
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
            z.writestr("readme.txt", "not a disc")
            z.write(iso, "Halo 3 (USA).iso")
        os.remove(iso)
        result = compress(archive)
        self.assertEqual(result.returncode, 0, result.stdout)
        os.remove(archive)
        c = self.served(self.isos, "Halo 3 (USA).iso")
        self.check_files(c, files)

    def test_a_damaged_zip_leaves_nothing(self):
        iso, _ = self.game()
        archive = os.path.join(self.isos, "Game.zip")
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_STORED) as z:
            z.write(iso, "Game.iso")
        os.remove(iso)
        with open(archive, "r+b") as f:
            f.seek(os.path.getsize(archive) // 2)
            byte = f.read(1)
            f.seek(-1, 1)
            f.write(bytes([byte[0] ^ 0xFF]))
        result = compress(archive)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("damaged", result.stdout)
        self.assertEqual(sorted(os.listdir(self.isos)), ["Game.zip"])

    @unittest.skipUnless(shutil.which("7z"), "needs 7z")
    def test_7z_archives(self):
        iso, files = self.game("One.iso")
        for method in ("LZMA2", "LZMA", "Copy"):
            archive = os.path.join(self.tmp.name, method + ".7z")
            subprocess.run(["7z", "a", "-bd", "-m0=" + method, archive, iso, os.path.join(HERE, "test_compress.py")],
                           check=True, stdout=subprocess.DEVNULL)
            result = compress(archive)
            self.assertEqual(result.returncode, 0, result.stdout)
            cso = os.path.join(self.tmp.name, method + ".cso")
            with open(cso, "rb") as f:
                self.assertEqual(f.read(4), b"CISO")
            if method == "LZMA2":
                os.remove(iso)
                shutil.move(cso, os.path.join(self.isos, "One.cso"))
                c = self.served(self.isos, "One.iso")
                self.check_files(c, files)
                c.close()
                self.server.kill()
                self.server.wait()
                self.server.stdout.close()
                self.server = None
                write_disc(iso, game_tree())
                os.remove(os.path.join(self.isos, "One.cso"))

    @unittest.skipUnless(shutil.which("7z"), "needs 7z")
    def test_7z_with_two_discs(self):
        one = os.path.join(self.tmp.name, "Game - Disc 1.iso")
        two = os.path.join(self.tmp.name, "Game - Disc 2.iso")
        write_disc(one, game_tree(3), seed=3)
        write_disc(two, game_tree(4), seed=4)
        archive = os.path.join(self.isos, "Game.7z")
        subprocess.run(["7z", "a", "-bd", archive, one, two], check=True, stdout=subprocess.DEVNULL)
        result = compress(archive)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(sorted(os.listdir(self.isos)), ["Game - Disc 1.cso", "Game - Disc 2.cso", "Game.7z"])

    @unittest.skipUnless(shutil.which("7z"), "needs 7z")
    def test_7z_with_a_filter_says_so(self):
        iso, _ = self.game()
        archive = os.path.join(self.isos, "Game.7z")
        subprocess.run(["7z", "a", "-bd", "-m0=Delta:4", "-m1=LZMA2", archive, iso], check=True, stdout=subprocess.DEVNULL)
        result = compress(archive)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("extract it with 7-Zip first", result.stdout)

    def test_reads_a_cso_from_another_tool(self):
        iso, files = self.game()
        with open(iso, "rb") as f:
            image = f.read()
        os.remove(iso)
        write_cso(os.path.join(self.isos, "Game.cso"), image, block=2048, shift=1)
        c = self.served(self.isos, "Game.iso")
        self.assertEqual(c.size(), (len(image) // SECTOR, SECTOR))
        self.check_files(c, files)
        self.assertEqual(c.read(12345, 70000), image[12345:12345 + 70000])

    def test_the_iso_plays_until_it_is_removed(self):
        iso, _ = self.game()
        self.assertEqual(compress(iso).returncode, 0)
        self.web = free_port()
        self.start_on(self.isos, "--web-port", str(self.web))
        deadline = time.time() + 10
        while True:
            games = self.json("GET", "/api/games")
            if games["games"] or time.time() > deadline:
                break
            time.sleep(0.2)
        self.assertEqual([(g["name"], g["kind"]) for g in games["games"]], [("Game", "iso")])
        self.assertEqual(games["conflicts"], [])  # Not a copy to choose: the ISO it was made from.
        os.remove(iso)
        deadline = time.time() + 10
        while True:
            games = self.json("GET", "/api/games")
            if [g["kind"] for g in games["games"]] == ["cso"] or time.time() > deadline:
                break
            time.sleep(0.2)
        self.assertEqual([(g["name"], g["kind"], g["titleId"]) for g in games["games"]], [("Game", "cso", "%08X" % TITLE)])

    def test_the_dashboard_compresses_an_archive(self):
        iso, files = self.game("Halo 3.iso")
        archive = os.path.join(self.isos, "Halo 3.zip")
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
            z.write(iso, "Halo 3.iso")
        os.remove(iso)
        self.web = free_port()
        self.start_on(self.isos, "--web-port", str(self.web))
        state = self.json("GET", "/api/compress?fresh=1")
        self.assertEqual([(a["path"], [(d["name"], d["done"]) for d in a["discs"]]) for a in state["archives"]],
                         [(archive, [("Halo 3", False)])])
        status, _, data = self.call("POST", "/api/compress", {"path": os.path.join(self.tmp.name, "elsewhere.zip")})
        self.assertEqual(status, 400, data)
        status, _, data = self.call("POST", "/api/compress", {"path": os.path.join(self.isos, "..", "x.iso")})
        self.assertEqual(status, 400, data)
        self.json("POST", "/api/compress", {"path": archive})
        deadline = time.time() + 60
        while True:
            state = self.json("GET", "/api/compress")
            if state["recent"] or time.time() > deadline:
                break
            time.sleep(0.2)
        self.assertTrue(state["recent"][0]["ok"], state)
        self.assertEqual(state["recent"][0]["output"], os.path.join(self.isos, "Halo 3.cso"))
        self.assertTrue(self.json("GET", "/api/compress?fresh=1")["archives"][0]["discs"][0]["done"])
        status, _, data = self.call("POST", "/api/compress", {"path": archive})
        self.assertEqual(status, 400, data)  # It is there already.
        c = Client(self.port)
        self.addCleanup(c.close)
        deadline = time.time() + 10
        while "Halo 3.iso" not in c.names():
            self.assertLess(time.time(), deadline)
            time.sleep(0.2)
        self.assertTrue(c.mount("Halo 3.iso"))
        self.check_files(c, files)


if __name__ == "__main__":
    unittest.main()
