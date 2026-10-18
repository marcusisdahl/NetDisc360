"""Tests NetISO Sync (shared/library.cpp) against tests/netiso_server.py.

Builds build/host/sync-host with g++ (or uses SYNC_HOST if set), serves
generated ISOs and checks the folders Sync writes, including that they match
what tools/netiso_aurora.py writes for the same ISOs.

    python tests/test_sync.py
"""
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import netiso_aurora  # noqa: E402
from test_generator import execution_info, header_digest, write_iso, xex  # noqa: E402

SYNC_HOST = os.environ.get("SYNC_HOST") or os.path.join(ROOT, "build", "host", "sync-host")


def build_host():
    if os.environ.get("SYNC_HOST"):
        return
    os.makedirs(os.path.dirname(SYNC_HOST), exist_ok=True)
    sources = [os.path.join(ROOT, p) for p in ("tests/sync_host.cpp", "shared/library.cpp", "shared/isvr.cpp")]
    subprocess.run(["g++", "-std=c++11", "-O1", "-Wall", "-include", os.path.join(HERE, "posix_compat.h"),
                    "-o", SYNC_HOST] + sources, check=True)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Sync(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        build_host()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.isos = os.path.join(self.tmp.name, "isos")
        self.games = os.path.join(self.tmp.name, "NetISO")
        os.makedirs(self.isos)
        self.launcher = os.path.join(self.tmp.name, "launcher.xex")
        with open(self.launcher, "wb") as f:
            f.write(xex(execution_info(0, 0xFFFF4E49), filler=b"LAUNCHER CODE" * 100))
        self.server = None

    def tearDown(self):
        self.stop_server()
        self.tmp.cleanup()

    def start_server(self):
        self.stop_server()
        self.port = free_port()
        self.server = subprocess.Popen([sys.executable, os.path.join(HERE, "netiso_server.py"), self.isos,
                                        "--port", str(self.port), "--bind", "127.0.0.1"], stdout=subprocess.PIPE, text=True)
        self.assertTrue(self.server.stdout.readline().startswith("READY"))

    def stop_server(self):
        if self.server:
            self.server.kill()
            self.server.wait()
            self.server.stdout.close()
            self.server = None

    def sync(self, expect_ok=True, boot=None):
        # The test server reads its ISO list at start, like netiso-srv per connection.
        self.start_server()
        result = subprocess.run([SYNC_HOST, str(self.port), self.games, self.launcher] + ([boot] if boot else []),
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode == 0, expect_ok, result.stdout + result.stderr)
        return result.stdout

    def add_iso(self, name, media_id, title_id, **kwargs):
        write_iso(os.path.join(self.isos, name), {"default.xex": xex(execution_info(media_id, title_id))}, **kwargs)

    def test_matches_the_pc_generator(self):
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        self.add_iso("Gears: Of War?.iso", 0x11111111, 0x4D5307D5, partition=(0xFDA0000, 0xFD90000))
        self.add_iso("Forza.iso", 0x22222222, 0x4D5309C9, partition=(0x2090000, 0x2080000))
        out = self.sync()
        self.assertIn("added=3", out)
        pc = os.path.join(self.tmp.name, "pc")
        self.assertEqual(netiso_aurora.main([self.isos, pc, "--launcher", self.launcher]), 0)
        self.assertEqual(sorted(os.listdir(self.games)), sorted(os.listdir(pc)))
        for game in os.listdir(pc):
            for name in ("default.xex", "netiso.ini"):
                with open(os.path.join(pc, game, name), "rb") as a, open(os.path.join(self.games, game, name), "rb") as b:
                    left, right = a.read(), b.read()
                if name == "netiso.ini":  # Only the comment line differs.
                    left, right = left.split(b"\n", 1)[1], right.split(b"\n", 1)[1]
                self.assertEqual(left, right, "%s/%s" % (game, name))
                if name == "default.xex":
                    stored, computed = header_digest(right)
                    self.assertEqual(stored, computed, game)

    def test_adds_new_isos_and_keeps_existing(self):
        self.add_iso("a.iso", 1, 0x11111111)
        self.assertIn("added=1", self.sync())
        marker = os.path.join(self.games, "a", "netiso.log")
        open(marker, "w").close()
        self.add_iso("b.iso", 2, 0x22222222)
        out = self.sync()
        self.assertIn("added=1 kept=1 removed=0", out)
        self.assertIn("updated=0", out)  # Same launcher: nothing rewritten.
        self.assertTrue(os.path.exists(marker))  # The existing folder was not rewritten.
        self.assertEqual(sorted(os.listdir(self.games)), ["a", "b"])

    def test_updates_launchers_made_by_an_older_sync(self):
        self.add_iso("a.iso", 0x1A2B3C4D, 0x4D5307E6)
        self.sync()
        path = os.path.join(self.games, "a", "default.xex")
        with open(path, "rb") as f:
            before = f.read()
        with open(self.launcher, "wb") as f:  # A new version of the launcher.
            f.write(xex(execution_info(0, 0xFFFF4E49), filler=b"NEWER CODE" * 150))
        out = self.sync()
        self.assertIn("kept=1", out)
        self.assertIn("updated=1", out)
        with open(path, "rb") as f:
            after = f.read()
        self.assertIn(b"NEWER CODE", after)
        at = netiso_aurora.xex_headers(after)[0x40006]
        self.assertEqual(after[at:at + 24], before[at:at + 24])  # Still the game's IDs.
        stored, computed = header_digest(after)
        self.assertEqual(stored, computed)
        self.assertIn("updated=0", self.sync())  # Current now.

    def test_repairs_a_launcher_with_another_titles_ids(self):
        self.add_iso("a.iso", 0x1A2B3C4D, 0x4D5307E6)
        self.sync()
        path = os.path.join(self.games, "a", "default.xex")
        with open(path, "rb") as f:
            good = f.read()
        shutil.copyfile(self.launcher, path)  # The template copied over the game's launcher.
        out = self.sync()
        self.assertIn("rebuilt it from a.iso", out)
        self.assertIn("kept=1", out)
        self.assertIn("updated=1", out)
        with open(path, "rb") as f:
            self.assertEqual(f.read(), good)
        self.assertIn("updated=0", self.sync())

    def test_the_game_folders_share_one_helper(self):
        boot = os.path.join(self.tmp.name, "boot.xex")
        with open(boot, "wb") as f:
            f.write(b"BOOT HELPER 1")
        helper = os.path.join(self.games, "NetDisc 360", "boot.xex")
        self.add_iso("a.iso", 1, 0x11111111)
        self.sync()  # Made without the helper.
        self.assertFalse(os.path.exists(helper))
        # A game folder from before has its own copy, which goes once the shared one is there.
        with open(os.path.join(self.games, "a", "boot.xex"), "wb") as f:
            f.write(b"OLD HELPER")
        self.add_iso("b.iso", 2, 0x22222222)
        out = self.sync(boot=boot)
        self.assertIn("added=1 kept=1", out)
        self.assertIn("updated=1", out)  # a lost its own helper.
        self.assertIn("Put the helper the game folders share in", out)
        with open(helper, "rb") as f:
            self.assertEqual(f.read(), b"BOOT HELPER 1")
        for game in ("a", "b"):
            self.assertEqual(sorted(os.listdir(os.path.join(self.games, game))), ["default.xex", "netiso.ini"])
        out = self.sync(boot=boot)
        self.assertIn("updated=0", out)
        self.assertNotIn("Put the helper", out)
        with open(boot, "wb") as f:
            f.write(b"BOOT HELPER 2, longer")
        out = self.sync(boot=boot)
        self.assertIn("updated=0", out)  # One file for all of them.
        self.assertIn("Put the helper the game folders share in", out)
        with open(helper, "rb") as f:
            self.assertEqual(f.read(), b"BOOT HELPER 2, longer")
        # Its folder is no game folder, and stays when the games go.
        os.remove(os.path.join(self.isos, "b.iso"))
        self.assertIn("removed=1", self.sync(boot=boot))
        self.assertEqual(sorted(os.listdir(self.games)), ["NetDisc 360", "a"])

    def test_a_game_folder_keeps_its_own_helper_while_the_shared_one_cant_be_written(self):
        boot = os.path.join(self.tmp.name, "boot.xex")
        with open(boot, "wb") as f:
            f.write(b"BOOT HELPER")
        self.add_iso("a.iso", 1, 0x11111111)
        self.sync()
        with open(os.path.join(self.games, "a", "boot.xex"), "wb") as f:
            f.write(b"OLD HELPER")
        with open(os.path.join(self.games, "NetDisc 360"), "wb") as f:
            f.write(b"a file where the helper's folder goes")
        out = self.sync(boot=boot, expect_ok=True)
        self.assertIn("Couldn't write", out)
        self.assertTrue(os.path.exists(os.path.join(self.games, "a", "boot.xex")))

    def test_removes_folders_of_isos_that_are_gone(self):
        self.add_iso("a.iso", 1, 0x11111111)
        self.add_iso("b.iso", 2, 0x22222222)
        self.sync()
        os.makedirs(os.path.join(self.games, "NetISO Sync"))  # Not a game folder: no iso= in it.
        with open(os.path.join(self.games, "NetISO Sync", "netiso.ini"), "w") as f:
            f.write("server=127.0.0.1\n")
        os.remove(os.path.join(self.isos, "b.iso"))
        out = self.sync()
        self.assertIn("added=0 kept=1 removed=1", out)
        self.assertEqual(sorted(os.listdir(self.games)), ["NetISO Sync", "a"])

    def test_keeps_folders_with_other_files(self):
        self.add_iso("a.iso", 1, 0x11111111)
        self.add_iso("b.iso", 2, 0x22222222)
        self.sync()
        open(os.path.join(self.games, "b", "cover.png"), "w").close()
        os.remove(os.path.join(self.isos, "b.iso"))
        out = self.sync()
        self.assertIn("Couldn't remove", out)
        self.assertEqual(os.listdir(os.path.join(self.games, "b")), ["cover.png"])

    def test_empty_server_removes_nothing(self):
        self.add_iso("a.iso", 1, 0x11111111)
        self.sync()
        os.remove(os.path.join(self.isos, "a.iso"))
        self.assertIn("removed=0", self.sync())
        self.assertEqual(os.listdir(self.games), ["a"])

    def test_skips_bad_isos(self):
        with open(os.path.join(self.isos, "junk.iso"), "wb") as f:
            f.write(b"\0" * 300000)
        write_iso(os.path.join(self.isos, "noxex.iso"), {"other.bin": b"1234"})
        self.add_iso("good.iso", 3, 0x33333333)
        out = self.sync()
        self.assertIn("added=1 kept=0 removed=0 skipped=2", out)
        self.assertIn("not an Xbox 360 game ISO", out)
        self.assertIn("no default.xex", out)

    def test_name_clash_gets_a_unique_folder(self):
        os.makedirs(os.path.join(self.games, "Halo"))  # Someone else's folder.
        self.add_iso("Halo.iso", 1, 0x11111111)
        self.sync()
        self.assertEqual(sorted(os.listdir(self.games)), ["Halo", "Halo (2)"])
        self.assertEqual(os.listdir(os.path.join(self.games, "Halo")), [])

    def test_folder_names_match_the_pc_tool(self):
        isos = ["Call of Duty - Black Ops (USA, Europe).iso", "Halo+ ;Reach=.iso",
                "Banjo-Kazooie - Nuts & Bolts (USA) (En,Ja,Fr,De,Es,It).iso", "Quake 4 (USA) (Bonus Disc).iso",
                "Lost Odyssey (USA) (En,Ja,Fr,De,Es,It) (Disc 2).iso", "Forza Motorsport 2 (Europe) (Rev 1) [!].iso",
                "Halo 3 (USA).iso", "Halo 3 (Europe) (En,Fr,De).iso", "Halo 3 (Japan).iso", "(USA) (En,Fr).iso"]
        for n, name in enumerate(isos):
            self.add_iso(name, n + 1, 0x4D530000 + n)
        self.sync()
        used = set()
        expected = [netiso_aurora.folder_name(name, used) for name in sorted(isos)]
        self.assertIn("Banjo-Kazooie - Nuts & Bolts", expected)
        self.assertIn("Halo 3 (USA)", expected)  # Europe sorts first and is plain "Halo 3".
        self.assertEqual(sorted(os.listdir(self.games)), sorted(expected))

    def test_keeps_a_renamed_folder(self):
        # Folders are matched to ISOs by netiso.ini, so a folder renamed on the
        # Xbox (or named by an older version) stays as it is.
        self.add_iso("Banjo-Kazooie - Nuts & Bolts (USA) (En,Ja,Fr,De,Es,It).iso", 1, 0x4D5307ED)
        self.sync()
        os.rename(os.path.join(self.games, "Banjo-Kazooie - Nuts & Bolts"), os.path.join(self.games, "Banjo (USA)"))
        out = self.sync()
        self.assertIn("added=0 kept=1 removed=0", out)
        self.assertEqual(os.listdir(self.games), ["Banjo (USA)"])

    def test_a_folder_it_cannot_write_is_skipped(self):
        self.add_iso("a.iso", 1, 0x11111111)
        self.add_iso("b.iso", 2, 0x22222222)
        os.makedirs(self.games)
        open(os.path.join(self.games, "a"), "w").close()  # A file where a's folder would go.
        out = self.sync()
        self.assertIn("added=1 kept=0 removed=0 skipped=1", out)
        self.assertIn("couldn't write", out)
        self.assertTrue(os.path.isfile(os.path.join(self.games, "a")))
        self.assertTrue(os.path.isfile(os.path.join(self.games, "b", "default.xex")))

    def test_no_server(self):
        result = subprocess.run([SYNC_HOST, str(free_port()), self.games, self.launcher], capture_output=True, text=True, timeout=30)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("CONNECT FAILED", result.stdout)
        self.assertFalse(os.path.exists(self.games))


if __name__ == "__main__":
    unittest.main()
