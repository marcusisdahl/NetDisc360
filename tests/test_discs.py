"""Tests the server's extracted-game folders and GOD packages (server/discs.cpp):
what it lists, and that the disc images it makes read as discs.

    python tests/test_discs.py
"""
import os
import struct
import subprocess
import sys
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from test_generator import execution_info, write_iso, xex  # noqa: E402
from test_server import ROOT, SERVER, Client, ServerTest, free_port  # noqa: E402

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
XGD2, XGD3 = 0xFD90000, 0x2080000
XGD2_IMAGE, XGD3_IMAGE = 7838695424, 8738846720  # Whole retail images.


def upper_key(name):
    return [ord(c.upper()) for c in name]


class Gdf:
    """Reads a disc image through the NetISO protocol, as the console does:
    names are found by walking each directory's search tree."""

    def __init__(self, client):
        self.c = client
        for magic_at, start in ((0xFDA0000, XGD2), (0x2090000, XGD3), (0x10000, 0)):
            if self.c.read(magic_at, 20) == MAGIC:
                self.start = start
                break
        else:
            raise AssertionError("no volume descriptor")
        volume = self.c.read(self.start + 32 * SECTOR, SECTOR)
        assert volume[0x7EC:0x7EC + 20] == MAGIC
        self.root = struct.unpack_from("<II", volume, 20)

    def table(self, sector, size):
        return self.c.read(self.start + sector * SECTOR, size) if size else b""

    @staticmethod
    def entry(table, at):
        left, right, sector, size, attributes, length = struct.unpack_from("<HHIIBB", table, at)
        name = table[at + 14:at + 14 + length].decode("latin-1")
        assert at // SECTOR == (at + 14 + length - 1) // SECTOR, "an entry crosses a sector"
        return left * 4, right * 4, sector, size, attributes, name

    def lookup(self, path):
        sector, size = self.root
        for part in path.split("/"):
            table = self.table(sector, size)
            at = 0
            while True:
                left, right, s, n, attributes, name = self.entry(table, at)
                if name.upper() == part.upper():
                    sector, size = s, n
                    break
                nxt = left if upper_key(part) < upper_key(name) else right
                if not nxt:
                    raise KeyError(path)
                at = nxt
        return sector, size

    def walk(self, sector=None, size=None, prefix=""):
        """{path: (sector, size, is_folder)}, checking each tree is in order."""
        if sector is None:
            sector, size = self.root
        out = {}
        table = self.table(sector, size)
        if not table:
            return out
        names = []

        def visit(at, low, high):
            left, right, s, n, attributes, name = self.entry(table, at)
            key = upper_key(name)
            assert (low is None or key > low) and (high is None or key < high), "tree out of order at " + name
            if left:
                visit(left, low, key)
            names.append(name)
            path = prefix + name
            folder = bool(attributes & 0x10)
            out[path] = (s, n, folder)
            if folder:
                out.update(self.walk(s, n, path + "/"))
            if right:
                visit(right, key, high)

        visit(0, None, None)
        assert names == sorted(names, key=upper_key)
        return out

    def read_file(self, sector, size):
        data = b""
        while len(data) < size:
            part = min(65536, size - len(data))
            data += self.c.read(self.start + sector * SECTOR + len(data), part)
        return data


def disk_files(folder, skip=()):
    out = {}
    for base, dirs, files in os.walk(folder):
        rel = os.path.relpath(base, folder).replace(os.sep, "/")
        rel = "" if rel == "." else rel + "/"
        dirs[:] = [d for d in dirs if rel + d not in skip]
        for d in dirs:
            out[rel + d] = None
        for f in files:
            with open(os.path.join(base, f), "rb") as h:
                out[rel + f] = h.read()
    return out


# GOD packages ---------------------------------------------------------------

BLOCK, GROUP, PART_DATA = 0x1000, 0xCC, 0xA1C4
PART_BYTES = PART_DATA * BLOCK


def god_location(d):
    """(part, file offset) of data offset d, the way Xenia computes it
    (stfs_container_device.cc BlockToOffsetSVOD), in 0x800-byte sectors."""
    sector, rest = divmod(d, 0x800)
    file_block, part = sector % 0x14388, sector // 0x14388
    level0 = file_block // 0x198 + 1
    offset = level0 * 0x1000 + (level0 // PART_DATA + 1) * 0x1000
    return part, file_block * 0x800 + offset + rest


def part_bytes(data_blocks):
    groups, rest = divmod(data_blocks, GROUP)
    return BLOCK * (1 + groups * (GROUP + 1) + (rest + 1 if rest else 0))


def gdf_partition(files, root_sector=36):
    """{partition offset: bytes} for a disc with files {name: (sector, data)} in its root."""
    names = sorted(files, key=upper_key)
    order, left, right = [], {}, {}

    def tree(lo, hi):
        if lo >= hi:
            return None
        mid = (lo + hi) // 2
        order.append(names[mid])
        left[names[mid]], right[names[mid]] = tree(lo, mid), tree(mid + 1, hi)
        return names[mid]

    tree(0, len(names))
    at, pos = {}, 0
    for name in order:
        at[name] = pos
        pos += (14 + len(name) + 3) & ~3
    table = bytearray(b"\xff" * ((pos + SECTOR - 1) // SECTOR * SECTOR))
    for name in order:
        sector, data = files[name]
        struct.pack_into("<HHIIBB", table, at[name], at[left[name]] // 4 if left[name] else 0,
                         at[right[name]] // 4 if right[name] else 0, sector, len(data), 0x20, len(name))
        table[at[name] + 14:at[name] + 14 + len(name)] = name.encode()
    chunks = {0x10000: MAGIC + struct.pack("<II", root_sector, len(table)), 0x10000 + 0x7EC: MAGIC,
              root_sector * SECTOR: bytes(table)}
    for sector, data in files.values():
        chunks[sector * SECTOR] = data
    return chunks


def write_god(folder, title_id, files, title="A Game", disc=1, discs=1, egdf=False, name="0123456789ABCDEF0123"):
    """A GOD package for a disc with root files {name: (sector, data)}, in
    folder/<Title ID>/00007000. Returns the partition's chunks."""
    content = os.path.join(folder, "%08X" % title_id, "00007000")
    os.makedirs(content + "/" + name + ".data")
    header = bytearray(0xB000)
    header[0:4] = b"LIVE"
    struct.pack_into(">I", header, 0x344, 0x7000)
    struct.pack_into(">I", header, 0x360, title_id)
    header[0x366], header[0x367] = disc, discs
    raw = title.encode("utf-16-be")
    header[0x411:0x411 + len(raw)] = raw
    with open(os.path.join(content, name), "wb") as f:
        f.write(header)
    chunks = gdf_partition(files)
    # XSF (iso2god): the data starts at the partition. EGDF: at its volume descriptor.
    shift = 0x10000 if egdf else 0
    end = max(at + len(data) for at, data in chunks.items()) - shift
    blocks = (end + BLOCK - 1) // BLOCK
    parts = (blocks + PART_DATA - 1) // PART_DATA
    handles = []
    for p in range(parts):
        f = open(os.path.join(content, name + ".data", "Data%04d" % p), "wb")
        f.truncate(part_bytes(min(PART_DATA, blocks - p * PART_DATA)))
        handles.append(f)
    for at, data in chunks.items():
        d = at - shift
        while data:
            run = min(len(data), BLOCK - d % BLOCK)
            part, offset = god_location(d)
            handles[part].seek(offset)
            handles[part].write(data[:run])
            d += run
            data = data[run:]
    for f in handles:
        f.close()
    return chunks


class Discs(ServerTest):
    def start(self, *extra):
        self.port = free_port()
        self.server = subprocess.Popen([SERVER, self.isos, "--port", str(self.port), "--no-push"] + list(extra),
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.startup = ""
        deadline = time.time() + 10
        while time.time() < deadline:
            line = self.server.stdout.readline()
            self.startup += line
            if "Listening on port" in line:
                return
        self.fail("server didn't start:\n" + self.startup)

    def log(self):
        """Everything the server printed, once it is stopped."""
        self.server.kill()
        return self.startup + self.server.stdout.read()

    def make_game(self, name, title_id, disc=1, folder=None):
        game = os.path.join(folder or self.isos, name)
        os.makedirs(game)
        with open(os.path.join(game, "default.xex"), "wb") as f:
            f.write(xex(execution_info(0x1234, title_id, disc), filler=b"GAME CODE" * 1000))
        return game

    def test_extracted_folder_reads_as_a_disc(self):
        game = self.make_game("Halo Reach", 0x4D53085B)
        many = os.path.join(game, "maps")
        os.makedirs(many)
        for i in range(400):  # Several sectors of directory table and a deep tree.
            with open(os.path.join(many, "Map_%03d%s.bin" % (i, "x" * (i % 37))), "wb") as f:
                f.write(bytes([i & 0xFF]) * (i * 13))
        for name in ("a_b", "AAB", "aa", "A[1]", "zz", "Z_", "~tilde", "_under"):
            with open(os.path.join(game, name), "wb") as f:
                f.write(name.encode() * 700)
        os.makedirs(os.path.join(game, "empty"))
        os.makedirs(os.path.join(game, "deep", "er", "still"))
        with open(os.path.join(game, "deep", "er", "still", "end.txt"), "wb") as f:
            f.write(os.urandom(SECTOR * 3 + 5))
        open(os.path.join(game, "zero.bin"), "wb").close()
        disc2 = self.make_game("Disc 2", 0x4D53085B, disc=2, folder=game)
        with open(os.path.join(disc2, "second.bin"), "wb") as f:
            f.write(b"two" * 5000)
        self.start()
        c = Client(self.port)
        self.assertEqual(c.names(), ["Halo Reach - Disc 2.iso", "Halo Reach.iso"])
        self.assertEqual(c.mount("\\Halo Reach.iso\\Mount\\"), 1)
        sectors, sector_size = c.size()
        self.assertEqual(sector_size, 2048)
        gdf = Gdf(c)
        self.assertEqual(gdf.start, XGD2)
        found = gdf.walk()
        expected = disk_files(game, skip={"Disc 2"})
        self.assertEqual(sorted(found), sorted(expected))
        for path, data in expected.items():
            sector, size, folder = found[path]
            self.assertEqual(folder, data is None, path)
            if data is not None:
                self.assertEqual(gdf.read_file(sector, size), data, path)
            self.assertEqual(gdf.lookup(path)[:2], (sector, size), path)
        self.assertEqual(found["empty"][:2], (0, 0))
        # As big as a whole XGD2 image, with zeros after the files.
        self.assertEqual(sectors * 2048, XGD2_IMAGE)
        end = XGD2 + max((s * 2048 + n + 2047) // 2048 * 2048 for s, n, _ in found.values())
        self.assertEqual(c.read(end, 4096), bytes(4096))

        self.assertEqual(c.mount("Halo Reach - Disc 2.iso"), 1)
        gdf = Gdf(c)
        found = gdf.walk()
        self.assertEqual(sorted(found), ["default.xex", "second.bin"])
        self.assertEqual(gdf.read_file(*found["second.bin"][:2]), b"two" * 5000)
        c.close()

    def test_sync_reads_the_title_from_a_folder_and_a_god_package(self):
        self.make_game("Portal 2", 0x45410950)
        write_god(os.path.join(self.isos, "Guitar Hero - World Tour"), 0x4156081A,
                  {"default.xex": (40, xex(execution_info(0x6C6A8EF1, 0x4156081A)))})
        self.start()
        sync_host = os.path.join(ROOT, "build", "host", "sync-host")
        if not os.path.exists(sync_host):
            self.skipTest("run tests/test_sync.py first")
        launcher = os.path.join(self.tmp.name, "launcher.xex")
        with open(launcher, "wb") as f:
            f.write(xex(execution_info(0, 0xFFFF4E49)))
        games = os.path.join(self.tmp.name, "NetISO")
        out = subprocess.run([sync_host, str(self.port), games, launcher], capture_output=True, text=True, timeout=60).stdout
        self.assertIn("added=2", out)
        self.assertEqual(sorted(os.listdir(games)), ["Guitar Hero - World Tour", "Portal 2"])
        with open(os.path.join(games, "Portal 2", "netiso.ini")) as f:
            ini = f.read()
        self.assertIn("iso=Portal 2.iso", ini)
        self.assertIn("title_id=45410950", ini)
        with open(os.path.join(games, "Guitar Hero - World Tour", "netiso.ini")) as f:
            self.assertIn("title_id=4156081A", f.read())

    def test_god_package_across_parts(self):
        big = os.urandom(3 * BLOCK + 123)
        # A file that starts in Data0000 and ends in Data0001, crossing hash groups.
        boundary_sector = (PART_BYTES - 0x2800) // SECTOR
        files = {"default.xex": (40, xex(execution_info(1, 0x4156081A))),
                 "middle.bin": (40 + 0x198 * 3 - 1, os.urandom(SECTOR * 3)),
                 "across.bin": (boundary_sector, big)}
        chunks = write_god(os.path.join(self.isos, "Some Game"), 0x4156081A, files)
        self.start()
        c = Client(self.port)
        self.assertEqual(c.names(), ["Some Game.iso"])
        self.assertEqual(c.mount("Some Game.iso"), 1)
        gdf = Gdf(c)
        self.assertEqual(gdf.start, XGD2)
        found = gdf.walk()
        for name, (sector, data) in files.items():
            self.assertEqual(found[name][:2], (sector, len(data)))
            self.assertEqual(gdf.read_file(sector, len(data)), data, name)
        # Everything before the partition reads as zeros.
        self.assertEqual(c.read(XGD2 - 4096, 4096), bytes(4096))
        sectors, _ = c.size()
        self.assertEqual(sectors * 2048, XGD2_IMAGE)
        c.close()

    def test_microsoft_layout_and_names_from_the_package(self):
        # EGDF: the data starts at the volume descriptor. The package is right
        # in the served folder, so the title in it names the game.
        files = {"default.xex": (40, xex(execution_info(1, 0x415607F7, 2, 2))), "x.bin": (60, b"xyz" * 999)}
        write_god(self.isos, 0x415607F7, files, title="Guitar Hero III: Legends of Rock", disc=2, discs=2, egdf=True)
        self.start()
        c = Client(self.port)
        self.assertEqual(c.names(), ["Guitar Hero III - Legends of Rock - Disc 2.iso"])
        self.assertEqual(c.mount("Guitar Hero III - Legends of Rock - Disc 2.iso"), 1)
        gdf = Gdf(c)
        found = gdf.walk()
        self.assertEqual(gdf.read_file(*found["x.bin"][:2]), b"xyz" * 999)
        c.close()

    def test_each_game_once_and_duplicates_reported(self):
        self.add_iso("Halo Reach.iso", 1, 0x4D53085B)
        self.make_game("Halo Reach", 0x4D53085B)
        write_god(os.path.join(self.isos, "Halo Reach GOD"), 0x4D53085B, {"default.xex": (40, xex(execution_info(1, 0x4D53085B)))})
        write_god(os.path.join(self.isos, "Portal 2 GOD"), 0x45410950, {"default.xex": (40, xex(execution_info(1, 0x45410950)))})
        self.make_game("Portal 2", 0x45410950)
        self.make_game("Other", 0x11111111)
        self.make_game("Other", 0x22222222, folder=os.path.join(self.isos, "more"))  # Another game, same name.
        self.start()
        c = Client(self.port)
        names = c.names()
        self.assertEqual(names, ["Halo Reach.iso", "Other.iso", "Other 2.iso", "Portal 2 GOD.iso"])
        c.close()
        log = self.log()
        self.assertIn("3 game(s) are here more than once", log)
        self.assertIn(os.path.join(self.isos, "Halo Reach") + "\n    is the same game as " + os.path.join(self.isos, "Halo Reach.iso"), log)
        self.assertIn(os.path.join(self.isos, "Halo Reach GOD", "4D53085B") + " (GOD package)\n", log)
        self.assertIn("(Title ID 45410950, disc 1); only that one is served.", log)
        self.assertEqual(log.count("more than once"), 1)  # Once, not at every listing.

    def test_original_xbox_games_and_other_folders(self):
        # An original Xbox game's folder is a disc of its own; what's inside
        # it isn't looked at.
        xbe = os.path.join(self.isos, "Halo 2")
        os.makedirs(os.path.join(xbe, "inside"))
        with open(os.path.join(xbe, "default.xbe"), "wb") as f:
            f.write(b"XBEH")
        self.add_iso("in an xbe folder.iso", 1, 0x11111111, xbe)
        other = os.path.join(self.tmp.name, "other")
        os.makedirs(other)
        self.make_game("Elsewhere", 0x33333333, folder=other)
        self.start(other)
        c = Client(self.port)
        self.assertEqual(c.names(), ["Halo 2.iso", "Elsewhere.iso"])
        c.close()

    def test_an_original_xbox_iso_and_folder_are_one_game(self):
        # Both have the Title ID from default.xbe's certificate, so the ISO is
        # served and the folder is reported as the same game.
        base, cert = 0x10000, 0x10180
        xbe = bytearray(0x300)
        xbe[0:4] = b"XBEH"
        struct.pack_into("<I", xbe, 0x104, base)
        struct.pack_into("<I", xbe, 0x118, cert)
        struct.pack_into("<I", xbe, cert - base + 8, 0x4D530004)
        write_iso(os.path.join(self.isos, "Halo.iso"), {"default.xbe": bytes(xbe)})
        folder = os.path.join(self.isos, "Halo CE")
        os.makedirs(folder)
        with open(os.path.join(folder, "default.xbe"), "wb") as f:
            f.write(bytes(xbe))
        self.start()
        c = Client(self.port)
        self.assertEqual(c.names(), ["Halo.iso"])
        c.close()
        log = self.log()
        self.assertIn(folder + "\n    is the same game as " + os.path.join(self.isos, "Halo.iso"), log)
        self.assertIn("(Title ID 4D530004, disc 1)", log)

    def test_a_full_original_xbox_disc_image(self):
        # Redump's format: the video partition, then the game partition at
        # 0x18300000. Only the game partition is served, where an extracted
        # Xbox game's is (XGD2), so the console finds it; and the game is the
        # same as its folder.
        base, cert = 0x10000, 0x10180
        xbe = bytearray(0x300)
        xbe[0:4] = b"XBEH"
        struct.pack_into("<I", xbe, 0x104, base)
        struct.pack_into("<I", xbe, 0x118, cert)
        struct.pack_into("<I", xbe, cert - base + 8, 0x4D530004)
        iso = os.path.join(self.isos, "Halo.iso")
        write_iso(iso, {"default.xbe": bytes(xbe), "maps.bin": b"MAP" * 5000}, partition=(0x18310000, 0x18300000))
        with open(iso, "r+b") as f:
            f.write(b"VIDEO PARTITION")
        folder = os.path.join(self.isos, "Halo CE")
        os.makedirs(folder)
        with open(os.path.join(folder, "default.xbe"), "wb") as f:
            f.write(bytes(xbe))
        self.start("-v")  # Logs the test's own mounts too.
        c = Client(self.port)
        self.assertEqual(c.names(), ["Halo.iso"])
        self.assertEqual(c.mount("Halo.iso"), 1)
        self.assertEqual(c.size()[0] * 2048, XGD2_IMAGE)
        self.assertEqual(c.read(0, 4096), bytes(4096))
        self.assertEqual(c.read(XGD2 + 0x10000, 20), MAGIC)
        self.assertEqual(c.read(XGD2 + 40 * SECTOR, len(xbe)), bytes(xbe))
        self.assertEqual(c.read(XGD2 + 41 * SECTOR, 15000), b"MAP" * 5000)
        end = XGD2 + os.path.getsize(iso) - 0x18300000
        self.assertEqual(c.read(end, 4096), bytes(4096))  # Past the image's end, inside the disc.
        c.close()
        log = self.log()
        self.assertIn(folder + "\n    is the same game as " + iso, log)
        self.assertIn("mounted Halo.iso (ISO, original Xbox full disc image)", log)

    def test_a_served_folder_is_not_a_game(self):
        # Game files left at the top of the ISO folder don't hide the ISOs.
        with open(os.path.join(self.isos, "default.xex"), "wb") as f:
            f.write(xex(execution_info(1, 0x4156081A)))
        open(os.path.join(self.isos, "default.xbe"), "wb").close()
        self.add_iso("a.iso", 1, 0x11111111)
        self.make_game("Game", 0x22222222)
        self.start()
        c = Client(self.port)
        self.assertEqual(c.names(), ["a.iso", "Game.iso"])
        c.close()

    def test_a_header_pointing_past_its_end(self):
        # An execution info offset near 4 GB once wrapped the bounds check and
        # crashed the server while it read the game's title.
        bad = bytearray(xex(execution_info(1, 0x22222222)))
        struct.pack_into(">I", bad, 24 + 8 + 4, 0xFFFFFFF0)
        folder = os.path.join(self.isos, "Broken")
        os.makedirs(folder)
        with open(os.path.join(folder, "default.xex"), "wb") as f:
            f.write(bytes(bad))
        self.add_iso("a.iso", 1, 0x11111111)
        self.start()
        c = Client(self.port)
        self.assertEqual(c.names(), ["a.iso", "Broken.iso"])  # Served, as a game without a title.
        c.close()
        self.assertIsNone(self.server.poll())

    def test_a_name_an_iso_already_has(self):
        iso = self.add_iso("Kameo.iso", 1, 0x11111111)
        self.make_game("Kameo", 0x22222222)  # Another game with the same name.
        self.start()
        c = Client(self.port)
        self.assertEqual(c.names(), ["Kameo 2.iso", "Kameo.iso"])
        self.assertEqual(c.mount("Kameo 2.iso"), 1)
        self.assertEqual(Gdf(c).start, XGD2)  # The folder, not the ISO (which has no XGD2 layout).
        self.assertEqual(c.mount("Kameo.iso"), 1)
        self.assertEqual(c.size()[0] * 2048, os.path.getsize(iso))
        c.close()

    def test_isos_with_the_same_name_in_two_folders(self):
        os.makedirs(os.path.join(self.isos, "a"))
        os.makedirs(os.path.join(self.isos, "b"))
        first = self.add_iso("Game.iso", 1, 0x11111111, os.path.join(self.isos, "a"))
        second = self.add_iso("Game.iso", 2, 0x22222222, os.path.join(self.isos, "b"))
        with open(second, "r+b") as f:  # A different size, to tell them apart.
            f.truncate(os.path.getsize(first) + 5 * SECTOR)
        self.make_game("Game", 0x33333333)  # A third game with that name.
        self.start()
        c = Client(self.port)
        self.assertEqual(sorted(c.names()), ["Game 2.iso", "Game 3.iso", "Game.iso"])
        self.assertEqual(c.mount("Game.iso"), 1)
        self.assertEqual(c.size()[0] * 2048, os.path.getsize(first))
        self.assertEqual(c.mount("Game 2.iso"), 1)
        self.assertEqual(c.size()[0] * 2048, os.path.getsize(second))
        self.assertEqual(c.mount("Game 3.iso"), 1)
        self.assertEqual(Gdf(c).start, XGD2)  # The folder.
        c.close()

    def test_a_folder_too_big_for_xgd2_and_its_last_sector(self):
        # Like GTA V's install disc: XGD3, and the files end on an odd sector.
        game = self.make_game("Big", 0x545408A7)
        for name in ("a.bin", "b.bin"):
            with open(os.path.join(game, name), "wb") as f:
                f.truncate(0xF0000000)
        with open(os.path.join(game, "z.bin"), "wb") as f:
            f.write(b"end" * 1000)  # One sector and a half: the last file.
        self.start()
        c = Client(self.port)
        self.assertEqual(c.mount("Big.iso"), 1)
        gdf = Gdf(c)
        self.assertEqual(gdf.start, XGD3)
        self.assertEqual(c.size()[0] * 2048, XGD3_IMAGE)
        sector, size, _ = gdf.walk()["z.bin"]
        self.assertEqual(gdf.read_file(sector, size), b"end" * 1000)
        last = XGD3 + (sector + 1) * 2048  # The file's second and last sector.
        self.assertEqual(c.read(last, 4096), (b"end" * 1000)[2048:] + bytes(4096 - 952))
        c.close()

    def test_a_read_past_the_end_of_an_iso(self):
        iso = self.add_iso("Odd.iso", 1, 0x11111111)
        with open(iso, "r+b") as f:  # An odd number of sectors, ending in a known one.
            sectors = (os.path.getsize(iso) + 2047) // 2048 | 1
            f.truncate(sectors * 2048)
            f.seek((sectors - 1) * 2048)
            f.write(b"L" * 2048)
        self.start()
        c = Client(self.port)
        self.assertEqual(c.mount("Odd.iso"), 1)
        self.assertEqual(c.size()[0], sectors)
        self.assertEqual(c.read((sectors - 1) * 2048, 4096), b"L" * 2048 + bytes(2048))
        self.assertEqual(c.read(sectors * 2048 + 8192, 4096), bytes(4096))
        self.assertEqual(c.ping(), b"ISVRokOK")  # Still connected.
        c.close()
        self.assertEqual(self.log().count("sending zeros past its end"), 1)

    def test_a_file_too_big_for_a_disc(self):
        game = self.make_game("Huge", 0x44444444)
        with open(os.path.join(game, "big.bin"), "wb") as f:
            f.truncate(4 << 30)
        self.start()
        c = Client(self.port)
        self.assertEqual(c.mount("Huge.iso"), 0)
        c.close()
        self.assertIn("is over 4 GB, which a disc can't hold", self.log())


if __name__ == "__main__":
    unittest.main()
