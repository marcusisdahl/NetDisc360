"""Tests the PC server (server/) on Linux: the NetISO protocol as the plugin
and NetISO Sync use it, and pushing game folders to tests/aurora_ftp.py, a
stand-in for Aurora's FTP server.

Builds build/host/netiso-server with g++ (or uses NETISO_SERVER if set).

    python tests/test_server.py
"""
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import aurora_ftp  # noqa: E402
from test_generator import execution_info, header_digest, write_iso, xex  # noqa: E402

SERVER = os.environ.get("NETISO_SERVER") or os.path.join(ROOT, "build", "host", "netiso-server")
LUA = shutil.which("lua5.2") or shutil.which("lua")

# The part of Aurora 0.7b.2's Media\Scripts\Content\FilterFunctions.lua the server edits.
FILTER = (b"function GameListFilterXbox360(Content)\r\n"
          b"   -- Add only legit xbox 360 games\r\n"
          b"   return Content.Group == ContentGroup.Xbox360\r\n"
          b"end\r\n"
          b"\r\n"
          b"function GameListFilterXBLA(Content)\r\n"
          b"   -- Add only Xbox Live Arcade games\r\n"
          b"   return Content.Group == ContentGroup.XBLA\r\n"
          b"end\r\n")


def build_server():
    if os.environ.get("NETISO_SERVER"):
        return
    os.makedirs(os.path.dirname(SERVER), exist_ok=True)
    sources = [os.path.join(ROOT, p) for p in ("server/main.cpp", "server/ftp.cpp", "shared/library.cpp", "shared/isvr.cpp")]
    subprocess.run(["g++", "-std=c++11", "-O1", "-Wall", "-pthread", "-include", os.path.join(HERE, "posix_compat.h"),
                    "-o", SERVER] + sources, check=True)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def exact(conn, count):
    data = b""
    while len(data) < count:
        part = conn.recv(count - len(data))
        if not part:
            raise EOFError
        data += part
    return data


class Client:
    """The NetISO protocol, as the console's plugin speaks it."""

    def __init__(self, port):
        self.conn = socket.create_connection(("127.0.0.1", port), timeout=10)

    def close(self):
        self.conn.close()

    def request(self, command, index=0, offset=0, length=0, extra=b""):
        self.conn.sendall(b"ISVR" + struct.pack(">HHQI", command, index, offset, length) + extra)

    def ping(self):
        self.request(0)
        return exact(self.conn, 8)

    def names(self):
        out = []
        while True:
            self.request(4, index=len(out), length=256)
            name = exact(self.conn, 256).rstrip(b"\0").decode()
            if not name:
                return out
            out.append(name)

    def mount(self, name):
        data = name.encode()
        self.request(5, length=len(data), extra=data)
        return struct.unpack(">I", exact(self.conn, 4))[0]

    def size(self):
        self.request(1)
        return struct.unpack(">II", exact(self.conn, 8))

    def read(self, offset, length):
        self.request(3, offset=offset, length=length)
        return exact(self.conn, length)


class ServerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        build_server()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.isos = os.path.join(self.tmp.name, "isos")
        os.makedirs(self.isos)
        self.server = None

    def tearDown(self):
        if self.server:
            self.server.kill()
            self.server.wait()
            self.server.stdout.close()
        self.tmp.cleanup()

    def start(self, *extra):
        self.port = free_port()
        self.server = subprocess.Popen([SERVER, self.isos, "--port", str(self.port), "--no-push"] + list(extra),
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        deadline = time.time() + 10
        while time.time() < deadline:
            line = self.server.stdout.readline()
            if "Listening on port" in line:
                return
        self.fail("server didn't start")

    def add_iso(self, name, media_id, title_id, folder=None):
        path = os.path.join(folder or self.isos, name)
        write_iso(path, {"default.xex": xex(execution_info(media_id, title_id))})
        return path


class Protocol(ServerTest):
    def test_lists_mounts_and_reads(self):
        a = self.add_iso("b game.iso", 1, 0x11111111)
        os.makedirs(os.path.join(self.isos, "sub"))
        self.add_iso("A game.iso", 2, 0x22222222, os.path.join(self.isos, "sub"))
        self.start()
        c = Client(self.port)
        self.assertEqual(c.ping(), b"ISVRokOK")
        self.assertEqual(c.names(), ["A game.iso", "b game.iso"])  # Subfolders too, sorted by name.
        self.assertEqual(c.size(), (0, 2048))  # Nothing mounted yet.
        self.assertEqual(c.mount("\\b game.iso\\Mount\\"), 1)  # As the plugin's Mount folder asks.
        with open(a, "rb") as f:
            data = f.read()
        self.assertEqual(c.size(), (len(data) // 2048, 2048))
        self.assertEqual(c.read(0x10000, 4096), data[0x10000:0x10000 + 4096])
        self.assertEqual(c.read(len(data) - 100, 100), data[-100:])
        self.assertEqual(c.mount("game.iso"), 1)  # The first name ending like that, as netiso-srv.
        self.assertEqual(c.mount("[Disable Current ISO]"), 0)
        self.assertEqual(c.size(), (0, 2048))
        self.assertEqual(c.mount("\\nothing.iso\\Mount\\"), 0)
        self.assertEqual(c.mount("\\Mount\\"), 0)  # An empty name mounts nothing.
        c.close()

    def test_new_isos_show_without_a_restart(self):
        self.add_iso("a.iso", 1, 0x11111111)
        self.start()
        c = Client(self.port)
        self.assertEqual(c.names(), ["a.iso"])
        self.add_iso("c.iso", 3, 0x33333333)
        self.assertEqual(c.names(), ["a.iso", "c.iso"])  # Same connection, listed again.
        self.assertEqual(c.mount("c.iso"), 1)
        os.remove(os.path.join(self.isos, "a.iso"))
        self.assertEqual(c.names(), ["c.iso"])
        c.close()

    def test_read_past_the_end_closes_the_connection(self):
        path = self.add_iso("a.iso", 1, 0x11111111)
        self.start()
        c = Client(self.port)
        self.assertEqual(c.mount("a.iso"), 1)
        c.request(3, offset=os.path.getsize(path) - 10, length=20)
        self.assertEqual(c.conn.recv(10), b"")
        c.close()
        c = Client(self.port)  # The server carries on.
        self.assertEqual(c.ping(), b"ISVRokOK")
        c.close()

    def test_sync_against_it(self):
        # NetISO Sync's own code, as the console runs it, against this server.
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        self.start()
        sync_host = os.path.join(ROOT, "build", "host", "sync-host")
        if not os.path.exists(sync_host):
            self.skipTest("run tests/test_sync.py first")
        launcher = os.path.join(self.tmp.name, "launcher.xex")
        with open(launcher, "wb") as f:
            f.write(xex(execution_info(0, 0xFFFF4E49)))
        games = os.path.join(self.tmp.name, "NetISO")
        out = subprocess.run([sync_host, str(self.port), games, launcher], capture_output=True, text=True, timeout=60).stdout
        self.assertIn("added=1", out)
        self.assertEqual(os.listdir(games), ["Halo 3"])


class Push(ServerTest):
    def setUp(self):
        super().setUp()
        self.xbox = os.path.join(self.tmp.name, "xbox")
        self.aurora = os.path.join(self.xbox, "Hdd1", "Apps", "Aurora", "Media", "Scripts", "Content")
        os.makedirs(self.aurora)
        self.games = os.path.join(self.xbox, "Hdd1", "NetISO")
        self.ftp = aurora_ftp.Server(self.xbox)
        threading.Thread(target=self.ftp.serve_forever, daemon=True).start()
        self.launcher = os.path.join(self.tmp.name, "launcher.xex")
        with open(self.launcher, "wb") as f:
            f.write(xex(execution_info(0, 0xFFFF4E49), filler=b"LAUNCHER CODE" * 100))
        self.boot = os.path.join(self.tmp.name, "boot.xex")
        with open(self.boot, "wb") as f:
            f.write(b"BOOT HELPER")
        with open(os.path.join(self.aurora, "FilterFunctions.lua"), "wb") as f:
            f.write(FILTER)

    def tearDown(self):
        self.ftp.shutdown()
        self.ftp.server_close()
        super().tearDown()

    def push(self, expect=0, ftp_port=None):
        result = subprocess.run([SERVER, self.isos, "--port", str(free_port()), "--push-once", "--xbox", "127.0.0.1",
                                 "--ftp-port", str(ftp_port or self.ftp.server_address[1]),
                                 "--launcher", self.launcher, "--boot", self.boot],
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, expect, result.stdout + result.stderr)
        return result.stdout

    def test_pushes_game_folders_like_sync(self):
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        self.add_iso("Gears: Of War?.iso", 0x11111111, 0x4D5307D5)
        out = self.push()
        self.assertIn("2 added, 0 removed", out)
        self.assertEqual(sorted(os.listdir(self.games)), ["Gears_ Of War_", "Halo 3"])
        folder = os.path.join(self.games, "Halo 3")
        self.assertEqual(sorted(os.listdir(folder)), ["boot.xex", "default.xex", "netiso.ini"])
        with open(os.path.join(folder, "default.xex"), "rb") as f:
            launcher = f.read()
        self.assertIn(b"LAUNCHER CODE", launcher)
        self.assertIn(execution_info(0x1A2B3C4D, 0x4D5307E6), launcher)
        stored, computed = header_digest(launcher)
        self.assertEqual(stored, computed)
        with open(os.path.join(folder, "netiso.ini")) as f:
            self.assertIn("iso=Halo 3.iso", f.read())
        # Nothing new: nothing written but nothing removed either.
        self.ftp.stored.clear()
        out = self.push()
        self.assertIn("0 added, 0 removed, 2 kept (0 updated)", out)
        self.assertEqual([p for p in self.ftp.stored if "NetISO" in p], [])

    def test_updates_netiso_sync_to_this_build(self):
        sync = os.path.join(self.games, "NetISO Sync")
        os.makedirs(sync)
        with open(os.path.join(sync, "default.xex"), "wb") as f:
            f.write(b"OLD SYNC")
        with open(os.path.join(sync, "netiso.ini"), "w") as f:
            f.write("after=Hdd1:\\Apps\\Aurora\\Aurora.xex\n")
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        out = self.push()
        self.assertIn("Updated /Hdd1/NetISO/NetISO Sync on the Xbox to this build.", out)
        for name, path in (("default.xex", self.launcher), ("boot.xex", self.boot)):
            with open(os.path.join(sync, name), "rb") as f, open(path, "rb") as built:
                self.assertEqual(f.read(), built.read())
        with open(os.path.join(sync, "netiso.ini")) as f:
            self.assertIn("after=", f.read())
        self.assertIn("1 added, 0 removed", out)
        # Current: not written again.
        self.ftp.stored.clear()
        out = self.push()
        self.assertNotIn("NetISO Sync on the Xbox", out)
        self.assertEqual([p for p in self.ftp.stored if "Sync" in p], [])

    def test_removes_games_whose_iso_is_gone_and_keeps_other_folders(self):
        self.add_iso("a.iso", 1, 0x11111111)
        path = self.add_iso("b.iso", 2, 0x22222222)
        self.push()
        os.makedirs(os.path.join(self.games, "NetISO Sync"))
        with open(os.path.join(self.games, "NetISO Sync", "netiso.ini"), "w") as f:
            f.write("server=10.0.0.1\n")
        os.remove(path)
        out = self.push()
        self.assertIn("0 added, 1 removed", out)
        self.assertEqual(sorted(os.listdir(self.games)), ["NetISO Sync", "a"])

    def test_repairs_a_launcher_with_another_titles_ids(self):
        self.add_iso("a.iso", 0x1A2B3C4D, 0x4D5307E6)
        self.push()
        target = os.path.join(self.games, "a", "default.xex")
        with open(target, "rb") as f:
            good = f.read()
        shutil.copyfile(self.launcher, target)
        out = self.push()
        self.assertIn("rebuilt it from a.iso", out)
        with open(target, "rb") as f:
            self.assertEqual(f.read(), good)

    def test_adds_the_games_to_aurora_s_xbox_360_category_once(self):
        self.add_iso("a.iso", 1, 0x11111111)
        out = self.push()
        self.assertIn("Added the NetISO games to Aurora's Xbox 360 category", out)
        script = os.path.join(self.aurora, "FilterFunctions.lua")
        with open(script, "rb") as f:
            edited = f.read()
        with open(script + ".before-netiso", "rb") as f:
            self.assertEqual(f.read(), FILTER)
        self.assertIn(b"or GameListFilterNetIso(Content)\r\nend\r\n", edited)
        self.assertNotIn(b"\n", edited.replace(b"\r\n", b""))  # Still CRLF throughout.
        self.assertTrue(edited.endswith(FILTER[FILTER.index(b"\r\nfunction GameListFilterXBLA"):]))
        out = self.push()
        self.assertIn("Xbox 360 category includes the NetISO games", out)
        with open(script, "rb") as f:
            self.assertEqual(f.read(), edited)
        if LUA:
            check = os.path.join(self.tmp.name, "check.lua")
            with open(check, "w") as f:
                f.write("ContentGroup = { Xbox360 = 1, XBLA = 2 }\n"
                        "dofile(%r)\n"
                        "local function t(group, dir) return GameListFilterXbox360({ Group = group, Directory = dir }) end\n"
                        "assert(t(5, '\\\\netiso\\\\skate 3 (usa europe)'))\n"
                        "assert(not t(5, '\\\\netiso\\\\netiso sync'))\n"
                        "assert(not t(5, '\\\\apps\\\\aurora'))\n"
                        "assert(t(1, '\\\\content\\\\0000000000000000\\\\4d5307e6'))\n"
                        "print('lua ok')\n" % script)
            result = subprocess.run([LUA, check], capture_output=True, text=True)
            self.assertEqual(result.stdout.strip(), "lua ok", result.stderr)

    def test_leaves_an_unknown_filter_alone(self):
        changed = FILTER.replace(b"legit xbox 360", b"retail")
        with open(os.path.join(self.aurora, "FilterFunctions.lua"), "wb") as f:
            f.write(changed)
        self.add_iso("a.iso", 1, 0x11111111)
        out = self.push()
        self.assertIn("isn't the one this program knows", out)
        with open(os.path.join(self.aurora, "FilterFunctions.lua"), "rb") as f:
            self.assertEqual(f.read(), changed)

    def test_no_xbox(self):
        self.add_iso("a.iso", 1, 0x11111111)
        out = self.push(expect=1, ftp_port=free_port())
        self.assertIn("Waiting for the Xbox's FTP server", out)
        self.assertFalse(os.path.exists(self.games))


if __name__ == "__main__":
    unittest.main()
