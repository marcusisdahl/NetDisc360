"""Tests tools/netiso_aurora.py against small ISOs built here.

    python tests/test_generator.py
"""
import os
import hashlib
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import netiso_aurora  # noqa: E402

SECTOR = 2048


SECURITY_SIZE = 0x180


def header_digest(data):
    """(stored, computed) XEX header digest, as the loader checks it."""
    header_size, security = struct.unpack_from(">I", data, 8)[0], struct.unpack_from(">I", data, 16)[0]
    computed = hashlib.sha1(bytes(data[security + 0x17C:header_size]) + bytes(data[:security + 8])).digest()
    return bytes(data[security + 0x164:security + 0x178]), computed


def xex(execution_info, filler=b""):
    """A XEX2 header with an execution info header, one other header and a
    security info with a valid header digest."""
    headers = [(0x10001, 0x82000000), (0x40006, 0)]
    table_end = 24 + 8 * len(headers)
    info_at = table_end + 8
    headers[1] = (0x40006, info_at)
    security = info_at + len(execution_info) + 40
    header_size = security + SECURITY_SIZE + 64
    data = bytearray(header_size)
    data[0:24] = struct.pack(">4sIIIII", b"XEX2", 0, header_size, 0, security, len(headers))
    for i, (key, value) in enumerate(headers):
        struct.pack_into(">II", data, 24 + 8 * i, key, value)
    data[info_at:info_at + len(execution_info)] = execution_info
    for i in range(security + 8, header_size):  # Stand-in signature, keys and imports.
        data[i] = (i * 7) & 0xFF
    data[security + 0x164:security + 0x178] = header_digest(data)[1]
    return bytes(data) + filler


def execution_info(media_id, title_id, disc=1, discs=1):
    return struct.pack(">IIIIBBBBI", media_id, 0x20000, 0, title_id, 0, 0, disc, discs, 0)


def write_iso(path, files, partition=(0x10000, 0), padding_entry=True):
    """An XDVDFS image with the given root files, at a partition layout."""
    magic_at, start = partition
    root_sector = 34
    entries = bytearray()
    sector = 40
    placed = []
    names = list(files)
    for i, name in enumerate(names):
        data = files[name]
        if padding_entry and i == 1:
            # Push the second entry into the next sector, as real images do.
            entries += b"\xff" * (SECTOR - len(entries))
        raw = name.encode("latin-1")
        entry = struct.pack("<HHIIBB", 0, 0, sector, len(data), 0x80, len(raw)) + raw
        entry += b"\xff" * (-len(entry) % 4)
        entries += entry
        placed.append((sector, data))
        sector += (len(data) + SECTOR - 1) // SECTOR
    with open(path, "wb") as f:
        f.seek(magic_at)
        f.write(netiso_aurora.XGD_MAGIC + struct.pack("<II", root_sector, len(entries)))
        f.seek(start + root_sector * SECTOR)
        f.write(entries)
        for at, data in placed:
            f.seek(start + at * SECTOR)
            f.write(data)
        f.seek(start + sector * SECTOR)
        f.write(b"\0" * SECTOR)


class Generator(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = self.tmp.name
        self.isos = os.path.join(self.root, "isos")
        self.out = os.path.join(self.root, "out")
        os.makedirs(self.isos)
        self.launcher = os.path.join(self.root, "NetIsoLauncher.xex")
        with open(self.launcher, "wb") as f:
            f.write(xex(execution_info(0, 0xFFFF4E49), filler=b"LAUNCHER CODE" * 100))

    def tearDown(self):
        self.tmp.cleanup()

    def run_tool(self, *extra):
        return netiso_aurora.main([self.isos, self.out, "--launcher", self.launcher] + list(extra))

    def read_out(self, game, name):
        with open(os.path.join(self.out, game, name), "rb") as f:
            return f.read()

    def test_patches_execution_info_and_writes_ini(self):
        info = execution_info(0x1A2B3C4D, 0x4D5307E6, disc=1, discs=1)
        write_iso(os.path.join(self.isos, "Halo 3.iso"), {"media": b"x" * 5000, "Default.XEX": xex(info, b"GAME" * 300)})
        self.assertEqual(self.run_tool(), 0)
        made = self.read_out("Halo 3", "default.xex")
        with open(self.launcher, "rb") as f:
            original = f.read()
        at = netiso_aurora.xex_headers(original)[0x40006]
        self.assertEqual(made[at:at + 24], info)
        # Only the execution info and the header digest change, and the new
        # digest is valid.
        digest_at = struct.unpack_from(">I", original, 16)[0] + 0x164
        def rest(data):
            return data[:at] + data[at + 24:digest_at] + data[digest_at + 20:]
        self.assertEqual(rest(made), rest(original))
        stored, computed = header_digest(made)
        self.assertEqual(stored, computed)
        self.assertNotEqual(stored, header_digest(original)[0])
        ini = self.read_out("Halo 3", "netiso.ini").decode()
        self.assertIn("iso=Halo 3.iso\r\n", ini)
        self.assertIn("title_id=4D5307E6\r\n", ini)
        self.assertIn("media_id=1A2B3C4D\r\n", ini)

    def test_writes_the_shared_helper(self):
        write_iso(os.path.join(self.isos, "a.iso"), {"default.xex": xex(execution_info(1, 0x11111111))})
        helper = os.path.join(self.root, "boot.xex")
        with open(helper, "wb") as f:
            f.write(b"BOOT HELPER")
        self.assertEqual(self.run_tool("--helper", helper), 0)
        self.assertEqual(sorted(os.listdir(self.out)), ["NetDisc360", "a"])
        self.assertEqual(self.read_out("NetDisc360", "boot.xex"), b"BOOT HELPER")
        self.assertEqual(sorted(os.listdir(os.path.join(self.out, "a"))), ["default.xex", "netiso.ini"])

    def test_takes_the_helper_next_to_the_tile_launcher(self):
        write_iso(os.path.join(self.isos, "a.iso"), {"default.xex": xex(execution_info(1, 0x11111111))})
        with open(os.path.join(self.root, "boot.xex"), "wb") as f:
            f.write(b"BOOT HELPER")
        self.assertEqual(self.run_tool(), 0)
        self.assertEqual(self.read_out("NetDisc360", "boot.xex"), b"BOOT HELPER")

    def test_xgd2_and_xgd3_layouts(self):
        write_iso(os.path.join(self.isos, "a.iso"), {"default.xex": xex(execution_info(1, 0x11111111))}, partition=(0xFDA0000, 0xFD90000))
        write_iso(os.path.join(self.isos, "b.iso"), {"default.xex": xex(execution_info(2, 0x22222222))}, partition=(0x2090000, 0x2080000))
        self.assertEqual(self.run_tool(), 0)
        self.assertIn(b"title_id=11111111", self.read_out("a", "netiso.ini"))
        self.assertIn(b"title_id=22222222", self.read_out("b", "netiso.ini"))

    def test_skips_bad_isos_and_keeps_going(self):
        with open(os.path.join(self.isos, "junk.iso"), "wb") as f:
            f.write(b"\0" * 100000)
        write_iso(os.path.join(self.isos, "noxex.iso"), {"other.bin": b"1234"})
        write_iso(os.path.join(self.isos, "good.iso"), {"default.xex": xex(execution_info(3, 0x33333333))})
        self.assertEqual(self.run_tool(), 0)
        self.assertEqual(sorted(os.listdir(self.out)), ["good"])

    def test_fatx_safe_folder_names(self):
        # Windows can't name a file with these characters, so this calls the
        # naming function directly.
        used = set()
        name = netiso_aurora.folder_name("A very long game name: with bad*chars? that keeps going.iso", used)
        self.assertEqual(name, "A very long game name_ with bad_chars_ tha")
        self.assertEqual(netiso_aurora.folder_name("Trailing dots. .iso", used), "Trailing dots")
        # FATX has no commas or plus signs; commas are dropped, others replaced.
        self.assertEqual(netiso_aurora.folder_name("Call of Duty - Black Ops (Demo, Kiosk).iso", used), "Call of Duty - Black Ops (Demo Kiosk)")
        self.assertEqual(netiso_aurora.folder_name("Halo+ ;Reach=.iso", used), "Halo_ _Reach_")

    def test_friendly_folder_names(self):
        # Region, language and revision tags go; tags that tell games or discs apart stay.
        cases = {
            "Banjo-Kazooie - Nuts & Bolts (USA) (En,Ja,Fr,De,Es,It).iso": "Banjo-Kazooie - Nuts & Bolts",
            "Call of Duty - Black Ops (USA, Europe).iso": "Call of Duty - Black Ops",
            "Quake 4 (USA) (Bonus Disc).iso": "Quake 4 (Bonus Disc)",
            "Lost Odyssey (USA) (En,Ja,Fr,De,Es,It) (Disc 2).iso": "Lost Odyssey (Disc 2)",
            "Forza Motorsport 2 (Europe) (Rev 1) [!].iso": "Forza Motorsport 2",
            "Halo 3 (Hong Kong) (v1.02) (Zh-Hant).iso": "Halo 3",
            "Ghost Recon (Legendary Edition) (USA).iso": "Ghost Recon (Legendary Edition)",
            "[RF] Some Game (Europe).iso": "[RF] Some Game",
            "Game (Demo) (Japan).iso": "Game (Demo)",
            "(USA) (En,Fr).iso": "(USA)",
            "Grand Theft Auto IV (USA) (En,Fr,De,Es,It) (Platinum Hits).iso": "Grand Theft Auto IV (Platinum Hits)",
        }
        for iso, expected in cases.items():
            self.assertEqual(netiso_aurora.folder_name(iso, set()), expected, iso)
        # Another release of the same game keeps its tags, and after that gets a number.
        used = set()
        self.assertEqual(netiso_aurora.folder_name("Halo 3 (USA).iso", used), "Halo 3")
        self.assertEqual(netiso_aurora.folder_name("Halo 3 (Europe) (En,Fr,De).iso", used), "Halo 3 (Europe) (EnFrDe)")
        self.assertEqual(netiso_aurora.folder_name("Halo 3.iso", used), "Halo 3 (2)")

    def test_unique_folder_names(self):
        long_name = "A very long game name that keeps going and going and going.iso"
        write_iso(os.path.join(self.isos, long_name), {"default.xex": xex(execution_info(4, 0x44444444))})
        write_iso(os.path.join(self.isos, long_name[:-4] + " again.iso"), {"default.xex": xex(execution_info(5, 0x55555555))})
        self.assertEqual(self.run_tool(), 0)
        folders = sorted(os.listdir(self.out))
        self.assertEqual(len(folders), 2)
        for folder in folders:
            self.assertLessEqual(len(folder), 42)

    def test_recursive_and_duplicate_names(self):
        os.makedirs(os.path.join(self.isos, "x"))
        os.makedirs(os.path.join(self.isos, "y"))
        write_iso(os.path.join(self.isos, "x", "same.iso"), {"default.xex": xex(execution_info(6, 0x66666666))})
        write_iso(os.path.join(self.isos, "y", "same.iso"), {"default.xex": xex(execution_info(7, 0x77777777))})
        self.assertEqual(self.run_tool(), 0)
        self.assertEqual(os.listdir(self.out), [])  # Not recursive: nothing in the top folder.
        self.assertEqual(self.run_tool("--recursive"), 0)
        self.assertEqual(os.listdir(self.out), ["same"])

    def test_launcher_without_execution_info(self):
        with open(self.launcher, "wb") as f:
            f.write(struct.pack(">4sIIIII", b"XEX2", 0, 24, 0, 0, 0))
        write_iso(os.path.join(self.isos, "g.iso"), {"default.xex": xex(execution_info(8, 0x88888888))})
        with self.assertRaises(SystemExit):
            self.run_tool()

    def test_command_line(self):
        write_iso(os.path.join(self.isos, "cli.iso"), {"default.xex": xex(execution_info(9, 0x99999999))})
        tool = os.path.join(HERE, "..", "tools", "netiso_aurora.py")
        result = subprocess.run([sys.executable, tool, self.isos, self.out, "--launcher", self.launcher], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("1 launcher(s) made", result.stdout)


if __name__ == "__main__":
    unittest.main()
