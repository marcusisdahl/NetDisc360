"""Tests the PC server (server/) on Linux: the NetISO protocol as the plugin
and NetISO Sync use it, pushing game folders to tests/aurora_ftp.py, a
stand-in for Aurora's FTP server, and the dashboard's web API.

Builds build/host/netdisc with g++ (or uses NETDISC_SERVER if set).

    python tests/test_server.py
"""
import base64
import gzip
import hashlib
import hmac
import http.client
import io
import json
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import aurora_ftp  # noqa: E402
from test_generator import execution_info, header_digest, write_iso, xex  # noqa: E402

SERVER = os.environ.get("NETDISC_SERVER") or os.path.join(ROOT, "build", "host", "netdisc")
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
    if os.environ.get("NETDISC_SERVER"):
        return
    os.makedirs(os.path.dirname(SERVER), exist_ok=True)
    sources = [os.path.join(ROOT, p) for p in ("server/main.cpp", "server/discs.cpp", "server/discmap.cpp", "server/ftp.cpp",
                                               "server/web.cpp", "server/config.cpp", "server/backup.cpp", "server/update.cpp",
                                               "shared/library.cpp", "shared/isvr.cpp")]
    subprocess.run(["g++", "-std=c++11", "-O1", "-Wall", "-pthread", "-include", os.path.join(HERE, "posix_compat.h"),
                    "-o", SERVER] + sources, check=True)


def write_tree_iso(path, tree, start=0x2080000):
    """An XDVDFS image (XGD3 layout) of a tree {name: bytes or {name: ...}}:
    each directory's entries one after another, as the server reads them."""
    SECTOR = 2048
    blocks = {}  # sector: data
    next_sector = [33]

    def place(data):
        at = next_sector[0]
        blocks[at] = data
        next_sector[0] += max(1, (len(data) + SECTOR - 1) // SECTOR)
        return at

    def directory(node):
        entries = bytearray()
        for name, value in node.items():
            if isinstance(value, dict):
                table = directory(value)
                sector, size, attributes = place(table), len(table), 0x10
            else:
                sector, size, attributes = place(value), len(value), 0x80
            raw = name.encode("latin-1")
            entry = struct.pack("<HHIIBB", 0, 0, sector, size, attributes, len(raw)) + raw
            entries += entry + b"\xff" * (-len(entry) % 4)
        return bytes(entries)

    root = directory(tree)
    root_sector = place(root)
    with open(path, "wb") as f:
        f.seek(start + 0x10000)
        f.write(b"MICROSOFT*XBOX*MEDIA" + struct.pack("<II", root_sector, len(root)))
        for sector, data in blocks.items():
            f.seek(start + sector * SECTOR)
            f.write(data)
        f.seek(start + next_sector[0] * SECTOR)
        f.write(b"\0" * SECTOR)


def own_address():
    """An address of this machine other than loopback, or None."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        try:
            s.connect(("192.0.2.1", 9))
            address = s.getsockname()[0]
        except OSError:
            return None
    return None if address.startswith("127.") or address == "0.0.0.0" else address


OWN_ADDRESS = own_address()


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def connect_from(address, port):
    """A connection to the server on port from address, once it listens."""
    deadline = time.time() + 10
    while True:
        conn = socket.socket()
        conn.bind((address, 0))
        try:
            conn.connect(("127.0.0.1", port))
            conn.settimeout(10)
            return conn
        except ConnectionRefusedError:
            conn.close()
            if time.time() > deadline:
                raise
            time.sleep(0.2)


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
        self.server = subprocess.Popen([SERVER, self.isos, "--port", str(self.port), "--no-push", "--web-port", "0"] + list(extra),
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

    # The dashboard's API on port self.web.
    def call(self, method, path, body=None, page=True, cookie=None, headers=None):
        """(status, response, data); an API call comes from the page unless page is False."""
        sent = {"Host": "127.0.0.1:%d" % self.web}
        if page and path.startswith("/api/"):
            sent["X-NetDisc"] = "1"
        if method == "POST":
            sent["Content-Type"] = "application/json"
        if cookie:
            sent["Cookie"] = cookie
        sent.update(headers or {})
        data = None if body is None else json.dumps(body)
        deadline = time.time() + 10
        while True:  # The dashboard starts just after the NetISO port.
            conn = http.client.HTTPConnection("127.0.0.1", self.web, timeout=10)
            try:
                conn.request(method, path, data, sent)
                response = conn.getresponse()
                return response.status, response, response.read()
            except ConnectionRefusedError:
                if time.time() > deadline:
                    raise
                time.sleep(0.2)
            finally:
                conn.close()

    def json(self, method, path, body=None, **options):
        status, _, data = self.call(method, path, body, **options)
        self.assertEqual(status, 200, data)
        return json.loads(data)

    def state_when(self, condition, seconds=10):
        deadline = time.time() + seconds
        while True:
            state = self.json("GET", "/api/state")
            if condition(state) or time.time() > deadline:
                return state
            time.sleep(0.2)


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
        # A game added since the server last looked at the folders mounts at once,
        self.assertEqual(c.mount("c.iso"), 1)
        # and is listed from its next look (each second here), on the same connection.
        self.assertTrue(self.eventually(lambda: c.names() == ["a.iso", "c.iso"]))
        os.remove(os.path.join(self.isos, "a.iso"))
        self.assertTrue(self.eventually(lambda: c.names() == ["c.iso"]))
        c.close()

    def eventually(self, check, seconds=5):
        deadline = time.time() + seconds
        while time.time() < deadline:
            if check():
                return True
            time.sleep(0.2)
        return False

    def test_a_read_over_16_mb_closes_the_connection(self):
        # Past the end is zeros instead (test_discs.py).
        self.add_iso("a.iso", 1, 0x11111111)
        self.start()
        c = Client(self.port)
        self.assertEqual(c.mount("a.iso"), 1)
        c.request(3, offset=0, length=(16 << 20) + 1)
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

    def push(self, expect=0, ftp_port=None, extra=()):
        result = subprocess.run([SERVER, self.isos, "--port", str(free_port()), "--push-once", "--xbox", "127.0.0.1",
                                 "--web-port", "0", "--ftp-port", str(ftp_port or self.ftp.server_address[1]),
                                 "--launcher", self.launcher, "--boot", self.boot] + list(extra),
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, expect, result.stdout + result.stderr)
        return result.stdout

    def test_pushes_game_folders_like_sync(self):
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        self.add_iso("Gears: Of War?.iso", 0x11111111, 0x4D5307D5)
        out = self.push()
        self.assertIn("2 added, 0 removed", out)
        self.assertEqual(sorted(os.listdir(self.games)), ["Gears_ Of War_", "Halo 3", "NetDisc360"])
        folder = os.path.join(self.games, "Halo 3")
        self.assertEqual(sorted(os.listdir(folder)), ["default.xex", "netiso.ini"])
        # The helper is one file for all the game folders, in a folder of its own.
        self.assertEqual(os.listdir(os.path.join(self.games, "NetDisc360")), ["boot.xex"])
        with open(os.path.join(self.games, "NetDisc360", "boot.xex"), "rb") as f:
            self.assertEqual(f.read(), b"BOOT HELPER")
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

    def test_takes_the_ftp_password_from_the_environment(self):
        # The Docker image passes the password this way.
        self.ftp.password = "s3cret pass"
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        os.environ["NETDISC_FTP_PASSWORD"] = "s3cret pass"
        try:
            self.assertIn("1 added, 0 removed", self.push())
        finally:
            del os.environ["NETDISC_FTP_PASSWORD"]
        self.assertNotIn("s3cret", "".join(self.ftp.log))
        # --ftp-password wins over it.
        os.environ["NETDISC_FTP_PASSWORD"] = "wrong"
        try:
            result = subprocess.run([SERVER, self.isos, "--port", str(free_port()), "--push-once", "--xbox", "127.0.0.1",
                                     "--web-port", "0", "--ftp-port", str(self.ftp.server_address[1]), "--ftp-password", "s3cret pass",
                                     "--launcher", self.launcher, "--boot", self.boot],
                                    capture_output=True, text=True, timeout=60)
        finally:
            del os.environ["NETDISC_FTP_PASSWORD"]
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_asks_aurora_to_scan_after_adding_or_removing_games(self):
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        path = self.add_iso("Skate 3.iso", 0x5C087C2C, 0x454108E6)
        out = self.push()
        self.assertEqual(self.ftp.scans, ["/Hdd1/NetISO"])
        self.assertIn("asked Aurora to scan /Hdd1/NetISO", out)
        # Nothing added or removed: no scan.
        self.push()
        self.assertEqual(len(self.ftp.scans), 1)
        os.remove(path)
        self.push()
        self.assertEqual(len(self.ftp.scans), 2)

    def run_server(self, *options, xbox="127.0.0.1", ftp_port=None):
        port = free_port()
        server = subprocess.Popen([SERVER, self.isos, "--port", str(port), "--xbox", xbox, "--web-port", "0",
                                   "--ftp-port", str(ftp_port or self.ftp.server_address[1]),
                                   "--launcher", self.launcher, "--boot", self.boot] + list(options),
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        return server, port

    def wait_for(self, condition, seconds):
        deadline = time.time() + seconds
        while not condition() and time.time() < deadline:
            time.sleep(0.2)
        return condition()

    def logins(self, ftp=None):
        return sum(1 for line in (ftp or self.ftp).log if line.startswith("USER"))

    def test_logs_in_only_when_something_changed(self):
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        server, _ = self.run_server()
        try:
            self.assertTrue(self.wait_for(lambda: self.ftp.scans, 10))
            first = self.logins()
            # Nothing changed: the Xbox is left alone.
            time.sleep(3)
            self.assertEqual(self.logins(), first)
            # A new ISO is written once it has settled (10 s).
            self.add_iso("Skate 3.iso", 0x5C087C2C, 0x454108E6)
            self.assertTrue(self.wait_for(lambda: len(self.ftp.scans) == 2, 25))
            self.assertTrue(os.path.isdir(os.path.join(self.games, "Skate 3")))
            done = self.logins()
            time.sleep(3)
            self.assertEqual(self.logins(), done)
        finally:
            server.kill()
            server.communicate()

    @unittest.skipUnless(OWN_ADDRESS, "needs an address other than loopback")
    def test_waits_for_the_xbox_to_come_online_after_aurora_comes_back(self):
        # Aurora's FTP answers before the Xbox can reach out; folders Aurora
        # finds then get no online lookup. The server writes once the Xbox
        # connects to it (its NetISO plugin), not as soon as FTP answers.
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        ftp_port = free_port()
        server, port = self.run_server("--online-wait", "60", xbox=OWN_ADDRESS, ftp_port=ftp_port)
        ftp = None
        try:
            time.sleep(2)  # The Xbox is off: its FTP doesn't answer.
            ftp = aurora_ftp.Server(self.xbox, ftp_port, host=OWN_ADDRESS)
            threading.Thread(target=ftp.serve_forever, daemon=True).start()
            self.assertTrue(self.wait_for(lambda: self.logins(ftp), 20))
            time.sleep(2)
            self.assertFalse(os.path.isdir(os.path.join(self.games, "Halo 3")))
            with socket.socket() as plugin:
                plugin.bind((OWN_ADDRESS, 0))
                plugin.connect((OWN_ADDRESS, port))
                self.assertTrue(self.wait_for(lambda: ftp.scans, 10))
            self.assertTrue(os.path.isfile(os.path.join(self.games, "Halo 3", "default.xex")))
        finally:
            server.kill()
            out = server.communicate()[0]
            if ftp:
                ftp.shutdown()
                ftp.server_close()
        self.assertIn("waiting for it to come online", out)

    def test_keeps_every_console_up_to_date(self):
        # Two consoles, each with its own Aurora: both get the game folders.
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        other = os.path.join(self.tmp.name, "xbox2")
        os.makedirs(os.path.join(other, "Hdd1", "Apps", "Aurora", "Media", "Scripts", "Content"))
        ftp2 = aurora_ftp.Server(other, self.ftp.server_address[1], host="127.0.0.2")
        threading.Thread(target=ftp2.serve_forever, daemon=True).start()
        try:
            out = self.push(extra=["--xbox", "127.0.0.2"])
        finally:
            ftp2.shutdown()
            ftp2.server_close()
        self.assertIn("Xbox 127.0.0.1: game folders up to date: 1 added", out)
        self.assertIn("Xbox 127.0.0.2: game folders up to date: 1 added", out)
        self.assertTrue(os.path.isfile(os.path.join(self.games, "Halo 3", "default.xex")))
        self.assertTrue(os.path.isfile(os.path.join(other, "Hdd1", "NetISO", "Halo 3", "default.xex")))

    def test_updates_netiso_sync_to_this_build(self):
        sync = os.path.join(self.games, "NetISO Sync")
        os.makedirs(sync)
        with open(os.path.join(sync, "default.xex"), "wb") as f:
            f.write(b"OLD SYNC")
        with open(os.path.join(sync, "netiso.ini"), "w") as f:
            f.write("after=Hdd1:\\Apps\\Aurora\\Aurora.xex\n")
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        out = self.push()
        self.assertIn("updated /Hdd1/NetISO/NetISO Sync to this build.", out)
        for name, path in (("default.xex", self.launcher), ("boot.xex", self.boot)):
            with open(os.path.join(sync, name), "rb") as f, open(path, "rb") as built:
                self.assertEqual(f.read(), built.read())
        with open(os.path.join(sync, "netiso.ini")) as f:
            self.assertIn("after=", f.read())
        self.assertIn("1 added, 0 removed", out)
        # Current: not written again.
        self.ftp.stored.clear()
        out = self.push()
        self.assertNotIn("NetISO Sync to this build", out)
        self.assertEqual([p for p in self.ftp.stored if "Sync" in p], [])

    def test_moves_game_folders_from_before_to_the_shared_helper(self):
        # A folder from before: the full launcher with the game's IDs, and its own helper.
        folder = os.path.join(self.games, "Halo 3")
        os.makedirs(folder)
        old = xex(execution_info(0x1A2B3C4D, 0x4D5307E6), filler=b"FULL LAUNCHER" * 250000)
        with open(os.path.join(folder, "default.xex"), "wb") as f:
            f.write(old)
        with open(os.path.join(folder, "boot.xex"), "wb") as f:
            f.write(b"OLD HELPER" * 1000)
        with open(os.path.join(folder, "netiso.ini"), "w") as f:
            f.write("iso=Halo 3.iso\ntitle_id=4D5307E6\nmedia_id=1A2B3C4D\n")
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        out = self.push()
        self.assertIn("Updated the launcher in Halo 3.", out)
        self.assertIn("1 kept (1 updated)", out)
        self.assertEqual(sorted(os.listdir(folder)), ["default.xex", "netiso.ini"])
        with open(os.path.join(folder, "default.xex"), "rb") as f:
            launcher = f.read()
        self.assertIn(b"LAUNCHER CODE", launcher)
        self.assertIn(execution_info(0x1A2B3C4D, 0x4D5307E6), launcher)
        with open(os.path.join(self.games, "NetDisc360", "boot.xex"), "rb") as f:
            self.assertEqual(f.read(), b"BOOT HELPER")
        self.assertIn("1 kept (0 updated)", self.push())

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
        self.assertEqual(sorted(os.listdir(self.games)), ["NetDisc360", "NetISO Sync", "a"])

    def test_leaves_a_gone_game_s_folder_alone_when_it_holds_more(self):
        self.add_iso("a.iso", 1, 0x11111111)
        path = self.add_iso("b.iso", 2, 0x22222222)
        self.push()
        with open(os.path.join(self.games, "b", "save.dat"), "w") as f:
            f.write("mine")
        os.remove(path)
        out = self.push()
        self.assertIn("Couldn't remove", out)
        self.assertEqual(sorted(os.listdir(os.path.join(self.games, "b"))), ["default.xex", "netiso.ini", "save.dat"])

    def test_keeps_the_games_of_a_game_folder_that_isn_t_there(self):
        # A drive or share that drops out mustn't take its games out of Aurora.
        self.add_iso("a.iso", 1, 0x11111111)
        other = os.path.join(self.tmp.name, "drive")
        os.makedirs(other)
        self.add_iso("b.iso", 2, 0x22222222, other)
        result = subprocess.run([SERVER, self.isos, other, "--port", str(free_port()), "--push-once", "--xbox", "127.0.0.1",
                                 "--web-port", "0", "--ftp-port", str(self.ftp.server_address[1]),
                                 "--launcher", self.launcher, "--boot", self.boot], capture_output=True, text=True, timeout=60)
        self.assertEqual(sorted(os.listdir(self.games)), ["NetDisc360", "a", "b"], result.stdout)
        os.rename(other, other + " (unplugged)")
        result = subprocess.run([SERVER, self.isos, other, "--port", str(free_port()), "--push-once", "--xbox", "127.0.0.1",
                                 "--web-port", "0", "--ftp-port", str(self.ftp.server_address[1]),
                                 "--launcher", self.launcher, "--boot", self.boot], capture_output=True, text=True, timeout=60)
        self.assertIn("Can't read these game folders", result.stdout)
        self.assertIn("they stay until every game folder can be read again", result.stdout)
        self.assertEqual(sorted(os.listdir(self.games)), ["NetDisc360", "a", "b"])

    def test_keeps_a_gone_game_for_a_while_after_starting(self):
        # The window restarts the server for each folder added; a game from a
        # folder not added yet mustn't be dropped from Aurora meanwhile.
        self.add_iso("a.iso", 1, 0x11111111)
        path = self.add_iso("b.iso", 2, 0x22222222)
        self.push()
        os.remove(path)
        scans = len(self.ftp.scans)
        started = time.time()
        server, _ = self.run_server("--remove-wait", "6")
        try:
            before = self.logins()
            self.assertTrue(self.wait_for(lambda: self.logins() > before, 10))
            time.sleep(1)
            self.assertEqual(sorted(os.listdir(self.games)), ["NetDisc360", "a", "b"])
            self.assertEqual(len(self.ftp.scans), scans)
            self.assertTrue(self.wait_for(lambda: not os.path.exists(os.path.join(self.games, "b")), 20))
            self.assertGreaterEqual(time.time() - started, 6)
            self.assertTrue(self.wait_for(lambda: len(self.ftp.scans) == scans + 1, 10))
        finally:
            server.kill()
            out = server.communicate()[0]
        self.assertIn("1 game folder(s) have no game here yet", out)
        self.assertIn("0 added, 1 removed", out)

    def test_keeps_a_gone_game_for_a_while_after_a_folder_change(self):
        # Changing the game folders on the dashboard holds removals back like
        # a start does, so folders can be added or swapped one at a time.
        self.add_iso("a.iso", 1, 0x11111111)
        more = os.path.join(self.tmp.name, "more")
        os.makedirs(more)
        self.add_iso("b.iso", 2, 0x22222222, more)
        web = free_port()
        started = time.time()
        server, _ = self.run_server("--remove-wait", "15", "--web-port", str(web),
                                    "--config", os.path.join(self.tmp.name, "netdisc.ini"), more)
        try:
            self.assertTrue(self.wait_for(lambda: self.ftp.scans, 15))
            self.assertEqual(sorted(os.listdir(self.games)), ["NetDisc360", "a", "b"])
            time.sleep(max(0, started + 16 - time.time()))  # The start's hold is over.
            before = self.logins()
            conn = http.client.HTTPConnection("127.0.0.1", web, timeout=10)
            conn.request("POST", "/api/settings", json.dumps({"folders": [self.isos]}),
                         {"Content-Type": "application/json", "X-NetDisc": "1", "Host": "127.0.0.1:%d" % web})
            self.assertEqual(conn.getresponse().status, 200)
            conn.close()
            changed = time.time()
            self.assertTrue(self.wait_for(lambda: self.logins() > before, 20))  # Once the games settled.
            time.sleep(1)
            self.assertEqual(sorted(os.listdir(self.games)), ["NetDisc360", "a", "b"])
            self.assertTrue(self.wait_for(lambda: not os.path.exists(os.path.join(self.games, "b")), 20))
            self.assertGreaterEqual(time.time() - changed, 14)
        finally:
            server.kill()
            out = server.communicate()[0]
        self.assertIn("1 game folder(s) have no game here yet", out)
        self.assertIn("0 added, 1 removed", out)

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
        self.assertIn("added the NetISO games to Aurora's Xbox 360 category", out)
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

    def test_hides_install_discs_in_aurora(self):
        filters = os.path.join(self.xbox, "Hdd1", "Apps", "Aurora", "User", "Scripts", "Content", "Filters")
        os.makedirs(filters)
        # An earlier server's edit of the Xbox 360 category gets this one's.
        before = FILTER.replace(b"   return Content.Group == ContentGroup.Xbox360\r\nend\r\n", (
            "   return Content.Group == ContentGroup.Xbox360 or GameListFilterNetIso(Content)\r\n"
            "end\r\n"
            "\r\n"
            "-- NetDisc 360: the game launchers in Hdd1:\\NetISO play retail discs\r\n"
            "-- from the NetISO server, so they count as Xbox 360 games here. Aurora files\r\n"
            "-- them as Unsigned because they aren't retail-signed. The NetISO Sync app\r\n"
            "-- itself stays out.\r\n"
            "function GameListFilterNetIso(Content)\r\n"
            "   local directory = '\\\\' .. string.lower(Content.Directory or '')\r\n"
            "   return string.find(directory, '\\\\netiso\\\\', 1, true) ~= nil and"
            " string.find(directory, '\\\\netiso\\\\netiso sync', 1, true) == nil\r\n"
            "end\r\n").encode())
        self.assertNotEqual(before, FILTER)
        with open(os.path.join(self.aurora, "FilterFunctions.lua"), "wb") as f:
            f.write(before)
        write_tree_iso(os.path.join(self.isos, "GTA V - Disc 2.iso"), {
            "default.xex": xex(execution_info(0x8C48794E, 0x545408A7, 1, 2)),
            "content": {"0000000000000000": {"545408A7": {"00000002": {"545408A700000000": b"PIRS" + bytes(100)}}}}})
        write_iso(os.path.join(self.isos, "GTA V.iso"), {"default.xex": xex(execution_info(0x79E465C5, 0x545408A7, 2, 2))})
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        out = self.push()
        self.assertIn("now leaves install discs out", out)
        self.assertIn("wrote Aurora's \"Hide install discs\" filter", out)
        script = os.path.join(filters, "NetDiscInstallDiscs.lua")
        with open(script, "rb") as f:
            text = f.read()
        self.assertIn(b"NetDiscInstallDiscs = {\r\n   [0x545408A7] = { [0x8C48794E] = true, },\r\n}\r\n", text)
        with open(os.path.join(self.aurora, "FilterFunctions.lua"), "rb") as f:
            edited = f.read()
        self.assertIn(b"and not (NetDiscInstallDisc ~= nil and NetDiscInstallDisc(Content))", edited)
        self.assertFalse(os.path.exists(os.path.join(self.aurora, "FilterFunctions.lua.before-netiso")))  # No new backup.
        # Nothing new: neither is written again.
        self.ftp.stored.clear()
        out = self.push()
        self.assertIn("Xbox 360 category includes the NetISO games", out)
        self.assertEqual([p for p in self.ftp.stored if "Scripts" in p], [])
        if LUA:
            check = os.path.join(self.tmp.name, "check.lua")
            with open(check, "w") as f:
                f.write("ContentGroup = { Xbox360 = 1, XBLA = 2 }\n"
                        "GameListFilterCategories = { User = {} }\n"
                        "dofile(%r)\n"
                        "dofile(%r)\n"
                        "local hide = GameListFilterCategories.User['Hide install discs']\n"
                        "local install = { Group = 5, Directory = '\\\\netiso\\\\gta v - disc 2', TitleId = 0x545408A7, MediaId = 0x8C48794E }\n"
                        "local play = { Group = 5, Directory = '\\\\netiso\\\\gta v', TitleId = 0x545408A7, MediaId = 0x79E465C5 }\n"
                        "assert(not hide(install))\n"
                        "assert(hide(play))\n"
                        "assert(not hide({ TitleId = 0x545408A7, MediaId = 0x8C48794E - 4294967296 }))\n"
                        "assert(hide({ TitleId = 0x4D5307E6, MediaId = 1 }))\n"
                        "assert(not GameListFilterXbox360(install))\n"
                        "assert(GameListFilterXbox360(play))\n"
                        "print('lua ok')\n" % (script, os.path.join(self.aurora, "FilterFunctions.lua")))
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

    def test_tells_when_launchers_are_updated(self):
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        self.push()
        with open(self.launcher, "wb") as f:  # A newer build's launcher.
            f.write(xex(execution_info(0, 0xFFFF4E49), filler=b"NEWER LAUNCHER" * 100))
        self.web, self.port = free_port(), free_port()
        self.server = subprocess.Popen([SERVER, self.isos, "--port", str(self.port), "--xbox", "127.0.0.1", "--web-port", str(self.web),
                                        "--ftp-port", str(self.ftp.server_address[1]), "--launcher", self.launcher, "--boot", self.boot,
                                        "--online-wait", "0"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        state = self.state_when(lambda s: any("launcher" in e["text"] for e in s["events"]), 20)
        self.assertIn("1 launcher updated on 127.0.0.1", [e["text"] for e in state["events"]])
        console = [c for c in state["consoles"] if c["address"] == "127.0.0.1"][0]
        self.assertIn("1 launcher updated", [e["text"] for e in console["events"]])
        self.assertLessEqual(len(console["events"]), 5)

    def test_skips_the_launcher_s_own_build_folder(self):
        # A build folder served as a game folder holds NetISO Sync, the
        # launcher as built, with its own Title ID: no tile for it.
        for name, title_id in (("NetISO Sync", 0xFFFF4E49), ("Halo 3", 0x4D5307E6)):
            folder = os.path.join(self.isos, name)
            os.makedirs(folder)
            with open(os.path.join(folder, "default.xex"), "wb") as f:
                f.write(xex(execution_info(0x12345678, title_id, 1, 1)))
        out = self.push()
        self.assertIn("1 added", out)
        self.assertEqual(sorted(os.listdir(self.games)), ["Halo 3", "NetDisc360"])

    def test_names_the_install_disc_a_game_needs(self):
        def game(folder, media_id, title_id, disc, discs=2, content=None):
            os.makedirs(folder)
            with open(os.path.join(folder, "default.xex"), "wb") as f:
                f.write(xex(execution_info(media_id, title_id, disc, discs)))
            for path, data in (content or {}).items():
                os.makedirs(os.path.dirname(os.path.join(folder, path)), exist_ok=True)
                with open(os.path.join(folder, path), "wb") as f:
                    f.write(data)
        # GTA V as extracted: the play disc on top, the install disc in Disc 2.
        packages = "content/0000000000000000/545408A7/00000002/"
        game(os.path.join(self.isos, "GTA V"), 0x79E465C5, 0x545408A7, 2)
        game(os.path.join(self.isos, "GTA V", "Disc 2"), 0x0C48794E, 0x545408A7, 1,
             content={packages + "545408A700000000": b"PIRS" + bytes(3000), packages + "545408A700000001": b"PIRS" + bytes(10)})
        # Forza 4 as ISOs: its second disc carries car packs.
        self.add_iso("Forza 4.iso", 0x11112222, 0x4D530910)
        write_tree_iso(os.path.join(self.isos, "Forza 4 - Disc 2.iso"), {
            "default.xex": xex(execution_info(0x33334444, 0x4D530910, 2, 2)),
            "content": {"0000000000000000": {"4d530910": {"00000002": {"4d53091000000001": b"PIRS" + bytes(500)}}}}})
        # A swap disc carries none; a package for another game doesn't count.
        self.add_iso("Red Dead.iso", 0x55556666, 0x5454082B)
        write_iso(os.path.join(self.isos, "Red Dead - Disc 2.iso"), {"default.xex": xex(execution_info(0x77778888, 0x5454082B, 2, 2))})
        game(os.path.join(self.isos, "Halo CE"), 0x99990000, 0x4D53085C, 1, 1,
             content={"content/0000000000000000/4D53085B/00080000/4D53085B01000060": b"PIRS" + bytes(100)})
        out = self.push()
        self.assertIn("7 added", out)

        def install(folder):
            path = os.path.join(self.games, folder, "install.ini")
            if not os.path.exists(path):
                return None
            with open(path) as f:
                return f.read()
        gta = install("GTA V")
        self.assertIn("install_iso=GTA V - Disc 2.iso\n", gta)
        self.assertIn("install_title_id=545408A7\n", gta)
        self.assertIn("install_media_id=0C48794E\n", gta)
        self.assertIn("install_folder=GTA V - Disc 2\n", gta)
        self.assertIn("package_1=0000000000000000\\545408A7\\00000002\\545408A700000000|3004\n", gta)
        self.assertIn("package_2=0000000000000000\\545408A7\\00000002\\545408A700000001|14\n", gta)
        self.assertIn("GTA V needs GTA V - Disc 2.iso installed first", out)
        forza = install("Forza 4")
        self.assertIn("install_iso=Forza 4 - Disc 2.iso\n", forza)
        self.assertIn("install_media_id=33334444\n", forza)
        self.assertIn("package_1=0000000000000000\\4d530910\\00000002\\4d53091000000001|504\n", forza)
        for folder in ("GTA V - Disc 2", "Forza 4 - Disc 2", "Red Dead", "Red Dead - Disc 2", "Halo CE"):
            self.assertIsNone(install(folder), folder)
        # Nothing new: nothing written.
        self.ftp.stored.clear()
        self.push()
        self.assertEqual([p for p in self.ftp.stored if "NetISO" in p], [])
        # The install disc gone: the game needs none any more, and its folder goes as a whole.
        os.remove(os.path.join(self.isos, "Forza 4 - Disc 2.iso"))
        out = self.push()
        self.assertIn("Forza 4 needs no install disc any more", out)
        self.assertEqual(install("Forza 4"), "")
        self.assertFalse(os.path.exists(os.path.join(self.games, "Forza 4 - Disc 2")))

    def test_keeps_a_disc_map_and_tells_each_disc_what_it_is(self):
        def disc(name, media_id, title_id, number, discs, content=None):
            files = {"default.xex": xex(execution_info(media_id, title_id, number, discs))}
            if content:
                files["content"] = content
            write_tree_iso(os.path.join(self.isos, name), files)
        # From the built-in list: GTA V's install and play discs, Red Dead's two games.
        disc("GTA V - Disc 2.iso", 0x0C48794E, 0x545408A7, 1, 2,
             {"0000000000000000": {"545408A7": {"00000002": {"545408A700000000": b"PIRS" + bytes(100)}}}})
        disc("GTA V.iso", 0x79E465C5, 0x545408A7, 2, 2)
        disc("Red Dead.iso", 0x55556666, 0x5454082B, 1, 2)
        disc("Red Dead - Disc 2.iso", 0x77778888, 0x5454082B, 2, 2)
        # Wolfenstein's install disc, then three discs it goes on through.
        disc("Wolfenstein.iso", 0x201, 0x425307F5, 1, 4,
             {"0000000000000000": {"425307F5": {"00000002": {"425307F500000000": b"PIRS" + bytes(100)}}}})
        disc("Wolfenstein - Disc 2.iso", 0x202, 0x425307F5, 2, 4)
        disc("Wolfenstein - Disc 3.iso", 0x203, 0x425307F5, 3, 4)
        # Not in it: a game that goes on from disc to disc, and one disc alone.
        for n in (1, 2, 3):
            disc("Saga - Disc %d.iso" % n, 0x100 + n, 0x12340001, n, 3)
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        # In the list but alone: Fallout 3 on one disc shares its Title ID with
        # the Game of the Year Edition, whose disc 2 is an add-on disc.
        disc("Fallout 3.iso", 0x301, 0x425307D5, 1, 1)
        config = os.path.join(self.tmp.name, "netdisc.ini")
        out = self.push(extra=("--config", config))
        self.assertIn("12 added", out)
        self.assertIn("Disc map: added Red Dead Redemption: Game of the Year Edition (5454082B): disc 1 own (Red Dead Redemption),"
                      " disc 2 own (Undead Nightmare), from the built-in list.", out)
        self.assertIn("Disc map: added Saga - Disc 1 (12340001): disc 1 next, disc 2 next, disc 3 next, guessed from the discs.", out)
        path = os.path.join(self.tmp.name, "discs.ini")
        with open(path, newline="") as f:
            map_text = f.read()
        self.assertIn("\r\n[545408A7]\r\n; From NetDisc360's list of known games.\r\nname=Grand Theft Auto V\r\n"
                      "disc1=install\r\ndisc2=play\r\n", map_text)
        self.assertIn("[5454082B]\r\n; From NetDisc360's list of known games.\r\n"
                      "name=Red Dead Redemption: Game of the Year Edition\r\n"
                      "disc1=own Red Dead Redemption\r\ndisc2=own Undead Nightmare\r\n", map_text)
        self.assertIn("[12340001]\r\n; NetDisc360's guess from the discs it found; change it if it's wrong.\r\n"
                      "name=Saga - Disc 1\r\ndisc1=next\r\ndisc2=next\r\ndisc3=next\r\n", map_text)
        self.assertIn("[425307F5]\r\n; From NetDisc360's list of known games.\r\nname=Wolfenstein: The New Order\r\n"
                      "disc1=install\r\ndisc2=next\r\ndisc3=next\r\ndisc4=next\r\n", map_text)
        self.assertNotIn("4D5307E6", map_text)
        self.assertNotIn("425307D5", map_text)

        def ini(folder):
            with open(os.path.join(self.games, folder, "netiso.ini")) as f:
                return f.read()
        self.assertIn("disc_role=install\n", ini("GTA V - Disc 2"))
        self.assertNotIn("disc_label", ini("GTA V - Disc 2"))
        self.assertIn("disc_role=play\n", ini("GTA V"))
        self.assertIn("iso=Red Dead - Disc 2.iso\n", ini("Red Dead - Disc 2"))
        self.assertIn("disc_role=own\ndisc_label=Undead Nightmare\n", ini("Red Dead - Disc 2"))
        self.assertIn("disc_role=next\n", ini("Saga - Disc 3"))
        self.assertNotIn("disc_role", ini("Halo 3"))
        self.assertNotIn("disc_role", ini("Fallout 3"))
        # The discs Wolfenstein goes on through need its install, like a play disc.
        self.assertIn("disc_role=install\n", ini("Wolfenstein"))
        for folder in ("Wolfenstein - Disc 2", "Wolfenstein - Disc 3"):
            self.assertIn("disc_role=next\n", ini(folder))
            with open(os.path.join(self.games, folder, "install.ini")) as f:
                self.assertIn("install_iso=Wolfenstein.iso\n", f.read())
        # Nothing new: nothing written, and the map isn't added to.
        self.ftp.stored.clear()
        self.push(extra=("--config", config))
        self.assertEqual([p for p in self.ftp.stored if "NetISO" in p], [])
        with open(path, newline="") as f:
            self.assertEqual(f.read(), map_text)
        # The user's word stays, and goes to the console: Saga's disc 3 is a
        # bonus disc, and GTA V's disc 1 is no install disc after all.
        changed = map_text.replace("disc3=next", "disc3=own Bonus disc").replace("disc1=install", "disc1=own")
        changed += "[nonsense]\r\n"
        with open(path, "w", newline="") as f:
            f.write(changed)
        out = self.push(extra=("--config", config))
        self.assertIn("[nonsense] isn't a Title ID", out)
        self.assertIn("disc_role=own\ndisc_label=Bonus disc\n", ini("Saga - Disc 3"))
        self.assertIn("iso=Saga - Disc 3.iso\n", ini("Saga - Disc 3"))
        self.assertIn("disc_role=own\n", ini("GTA V - Disc 2"))
        with open(os.path.join(self.games, "GTA V", "install.ini")) as f:
            self.assertEqual(f.read(), "")  # A play disc of a set without an install disc needs none.
        with open(path, newline="") as f:
            self.assertEqual(f.read(), changed)
        # A section deleted is added again.
        with open(path, "w", newline="") as f:
            f.write(changed.split("[12340001]")[0])
        out = self.push(extra=("--config", config))
        self.assertIn("Disc map: added Saga - Disc 1 (12340001)", out)
        self.assertIn("disc_role=next\n", ini("Saga - Disc 3"))
        self.assertNotIn("disc_label", ini("Saga - Disc 3"))

    def test_the_built_in_disc_list_is_sound(self):
        with open(os.path.join(ROOT, "server", "discmap.cpp")) as f:
            source = f.read()
        table = source[source.index("static const KnownGame Known[]={"):]
        table = table[:table.index("\n};")]
        games = re.findall(r'\{0x([0-9A-F]{8}),"([^"]+)","([^"]+)"\}', table)
        self.assertGreater(len(games), 40)
        self.assertEqual(len(games), table.count("{0x"))
        ids = [title_id for title_id, _, _ in games]
        self.assertEqual(len(ids), len(set(ids)), "a Title ID is in the list twice")
        for title_id, name, discs in games:
            parts = discs.split(";")
            self.assertGreaterEqual(len(parts), 2, name)
            self.assertLessEqual(len(parts), 9, name)
            for part in parts:
                self.assertIn(part.split(" ")[0], ("install", "play", "content", "next", "own"), name)
            roles = [part.split(" ")[0] for part in parts]
            # An install disc comes with a disc to play, and a content disc names what it holds.
            if "install" in roles:
                self.assertTrue("play" in roles or "next" in roles, name)
            for part in parts:
                if part.startswith("content"):
                    self.assertGreater(len(part), len("content "), name)

    def test_no_xbox(self):
        self.add_iso("a.iso", 1, 0x11111111)
        out = self.push(expect=1, ftp_port=free_port())
        self.assertIn("waiting for its FTP server", out)
        self.assertFalse(os.path.exists(self.games))


class Dashboard(ServerTest):
    """The dashboard's web API, as web/index.html uses it."""

    def setUp(self):
        super().setUp()
        self.config = os.path.join(self.tmp.name, "netdisc.ini")

    def start(self, *extra):
        self.web = free_port()
        super().start("--web-port", str(self.web), "--config", self.config, *extra)

    def restart(self, *extra):
        self.server.kill()
        self.server.wait()
        self.server.stdout.close()
        self.start(*extra)

    def test_serves_the_page_and_the_games(self):
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        self.start()
        status, response, data = self.call("GET", "/")
        self.assertEqual(status, 200)
        self.assertEqual(response.getheader("Content-Encoding"), "gzip")
        self.assertIn("default-src 'none'", response.getheader("Content-Security-Policy"))
        self.assertIn(b"<title>NetDisc360</title>", gzip.decompress(data))
        state = self.state_when(lambda s: s["library"]["games"] == 1)
        self.assertEqual(state["library"]["games"], 1)
        self.assertEqual(state["folders"], [{"path": self.isos, "games": 1, "found": True}])
        self.assertTrue(state["settings"]["saved"])
        self.assertTrue(state["settings"]["ftpPasswordSet"])
        games = self.json("GET", "/api/games")["games"]
        self.assertEqual([(g["name"], g["kind"], g["titleId"]) for g in games], [("Halo 3", "iso", "4D5307E6")])
        self.assertEqual(self.call("GET", "/api/nothing")[0], 404)

    def test_a_game_names_its_install_disc(self):
        self.add_iso("Forza 4.iso", 0x11112222, 0x4D530910)
        write_tree_iso(os.path.join(self.isos, "Forza 4 - Disc 2.iso"), {
            "default.xex": xex(execution_info(0x33334444, 0x4D530910, 2, 2)),
            "content": {"0000000000000000": {"4D530910": {"00000002": {"4D53091000000001": b"PIRS" + bytes(500)}}}}})
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        self.start()
        self.state_when(lambda s: s["library"]["games"] == 3)
        games = self.json("GET", "/api/games")["games"]
        self.assertEqual([(g["name"], g["installDisc"], g["role"], g["roleLabel"]) for g in games],
                         [("Forza 4 - Disc 2", "", "content", "Car packs"), ("Forza 4", "Forza 4 - Disc 2", "play", ""),
                          ("Halo 3", "", "", "")])
        with open(os.path.join(self.tmp.name, "discs.ini")) as f:
            self.assertIn("[4D530910]\n", f.read())

    def test_a_game_there_twice_waits_for_a_choice(self):
        iso = self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        folder = os.path.join(self.isos, "Halo 3 extracted")
        os.makedirs(folder)
        with open(os.path.join(folder, "default.xex"), "wb") as f:
            f.write(xex(execution_info(0x1234, 0x4D5307E6)))
        self.start()
        # The games list comes a moment after the conflict is found.
        state = self.state_when(lambda s: s["library"]["games"] == 1 and s["library"]["conflicts"] == 1)
        self.assertEqual([state["library"][k] for k in ("games", "duplicates", "conflicts")], [1, 1, 1])
        self.assertIn("Halo 3 is in your folders more than once. Choose which copy to play on the Games page.",
                      [e["text"] for e in state["events"]])
        conflict = self.json("GET", "/api/games")["conflicts"][0]
        self.assertEqual([conflict[k] for k in ("key", "name", "served", "open", "chosen")], ["4D5307E6-1", "Halo 3", iso, True, False])
        self.assertEqual([(c["kind"], c["path"]) for c in conflict["copies"]], [("iso", iso), ("folder", folder)])
        # Only a copy that is there can be chosen.
        self.assertEqual(self.call("POST", "/api/conflict", {"key": "4D5307E6-1", "path": "/nowhere"})[0], 400)
        self.assertEqual(self.call("POST", "/api/conflict", {"key": "4D5307E6-1", "path": folder}, page=False)[0], 403)
        self.json("POST", "/api/conflict", {"key": "4D5307E6-1", "path": folder})
        self.assertEqual(self.json("GET", "/api/state")["library"]["conflicts"], 0)
        state = self.state_when(lambda s: self.json("GET", "/api/games")["conflicts"][0]["served"] == folder)
        c = Client(self.port)
        self.assertEqual(c.names(), ["Halo 3 extracted.iso"])
        c.close()
        # Kept across a restart, with the copies it was made for.
        with open(self.config) as f:
            self.assertIn("choice1=4D5307E6-1|", f.read())
        self.restart()
        state = self.state_when(lambda s: s["library"]["scanned"] and s["library"]["duplicates"] == 1)
        self.assertEqual(state["library"]["conflicts"], 0)
        c = Client(self.port)
        self.assertEqual(c.names(), ["Halo 3 extracted.iso"])
        c.close()
        # A copy that turns up later asks again; the chosen one still plays until then.
        self.add_iso("Halo 3 again.iso", 0x1A2B3C4D, 0x4D5307E6)
        state = self.state_when(lambda s: s["library"]["conflicts"] == 1)
        self.assertEqual(state["library"]["duplicates"], 2)
        conflict = self.json("GET", "/api/games")["conflicts"][0]
        self.assertEqual([conflict[k] for k in ("served", "open", "chosen")], [folder, True, True])

    def test_reports_each_console(self):
        # What each console was sent, second by second, and its disc.
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        self.start("--xbox", "127.0.0.2", "-v")  # -v: connections from this computer count too.
        with connect_from("127.0.0.2", self.port) as plugin:
            c = Client.__new__(Client)
            c.conn = plugin
            self.assertEqual(c.mount("Halo 3.iso"), 1)
            c.read(0, 65536)
            state = self.state_when(lambda s: any(sum(x["rate"]) for x in s["consoles"]))
        console = [x for x in state["consoles"] if x["address"] == "127.0.0.2"][0]
        self.assertEqual((console["connections"], console["sent"], console["mounted"]), (1, 65536, "Halo 3"))
        self.assertEqual(sum(console["rate"]), 65536)
        self.assertGreaterEqual(state["totals"]["peak"], 65536)
        self.assertIn("Halo 3 started on 127.0.0.2", [e["text"] for e in state["events"]])
        self.assertIn("Halo 3 started", [e["text"] for e in console["events"]])  # Under its own card.

    @unittest.skipUnless(OWN_ADDRESS, "needs an address other than loopback")
    def test_a_connection_without_a_request_is_no_console(self):
        # A port scan isn't taken for an Xbox, which would get the FTP login.
        self.start("--no-push")
        probe = socket.socket()
        probe.bind((OWN_ADDRESS, 0))
        probe.connect(("127.0.0.1", self.port))
        probe.close()
        time.sleep(1)
        self.assertEqual(self.json("GET", "/api/state")["consoles"], [])
        with connect_from(OWN_ADDRESS, self.port) as plugin:
            plugin.sendall(b"ISVR" + bytes(16))  # A ping.
            self.assertEqual(plugin.recv(8), b"ISVRokOK")
            state = self.state_when(lambda s: s["consoles"])
        self.assertEqual([x["address"] for x in state["consoles"]], [OWN_ADDRESS])

    def test_tells_when_a_console_is_back_in_aurora(self):
        # NetISO keeps its connection and disc after a game ends; Aurora's FTP
        # server, which never runs during a game, answering again tells.
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        ftp_port = free_port()
        self.start("--xbox", "127.0.0.2", "-v", "--ftp-port", str(ftp_port))
        console = lambda s: [x for x in s["consoles"] if x["address"] == "127.0.0.2"][0]  # noqa: E731
        with connect_from("127.0.0.2", self.port) as plugin:
            c = Client.__new__(Client)
            c.conn = plugin
            self.assertEqual(c.mount("Halo 3.iso"), 1)
            self.assertTrue(console(self.state_when(lambda s: console(s)["playing"]))["playing"])
            os.makedirs(os.path.join(self.tmp.name, "xbox"))
            ftp = aurora_ftp.Server(os.path.join(self.tmp.name, "xbox"), ftp_port, host="127.0.0.2")
            threading.Thread(target=ftp.serve_forever, daemon=True).start()
            try:
                state = self.state_when(lambda s: console(s)["inAurora"], 25)
                self.assertEqual([console(state)[k] for k in ("playing", "inAurora", "mounted")], [False, True, "Halo 3"])
                self.assertIn("Back in Aurora after Halo 3", [e["text"] for e in console(state)["events"]])
                self.assertIn("127.0.0.2 is back in Aurora after Halo 3", [e["text"] for e in state["events"]])
                self.assertFalse(any(line.startswith("USER") for line in ftp.log))  # Never logged in.
                # The disc started again from Aurora: its reads show it.
                for _ in range(33 * 16):
                    c.read(0, 65536)
                state = self.state_when(lambda s: console(s)["playing"])
                self.assertEqual([console(state)[k] for k in ("playing", "inAurora")], [True, False])
            finally:
                ftp.shutdown()
                ftp.server_close()

    def test_pings_each_console(self):
        # A ping now and then (a connect to its FTP port, refused here) shows
        # a console is on, even with no NetISO connection (one that stops
        # answering goes offline: PingLoop).
        self.start("--xbox", "127.0.0.2", "--ping", "1", "--ftp-port", str(free_port()))
        # The web page can answer just before the console from --xbox is listed.
        console = lambda s: next((x for x in s["consoles"] if x["address"] == "127.0.0.2"), {})  # noqa: E731
        state = self.state_when(lambda s: console(s).get("answers"))
        self.assertEqual([console(state)[k] for k in ("answers", "offline", "connections")], [True, False, 0])
        self.assertGreaterEqual(console(state)["ping"], 0)

    def test_changes_come_only_from_the_page(self):
        self.start()
        # Another site's page can't send the header, or claim this origin.
        self.assertEqual(self.call("POST", "/api/settings", {"scan": False}, page=False)[0], 403)
        self.assertEqual(self.call("POST", "/api/settings", {"scan": False}, headers={"Origin": "http://evil.example"})[0], 403)
        # Without a password, only names a site can't point here (DNS rebinding).
        self.assertEqual(self.call("GET", "/api/state", headers={"Host": "evil.example:%d" % self.web})[0], 403)
        self.assertEqual(self.call("GET", "/api/state", headers={"Host": "nas.local:%d" % self.web})[0], 200)
        status, _, data = self.call("POST", "/api/settings", {"xboxes": "192.168.1.20, xbox"})
        self.assertEqual(status, 400)
        self.assertIn("isn't an IPv4 address", json.loads(data)["error"])
        self.json("POST", "/api/settings", {"scan": False, "xboxes": "192.168.1.20 192.168.1.21", "ftpPassword": "s3cret"})
        settings = self.json("GET", "/api/state")["settings"]
        self.assertEqual((settings["scan"], settings["xboxes"]), (False, ["192.168.1.20", "192.168.1.21"]))
        self.assertNotIn("s3cret", json.dumps(settings))
        with open(self.config) as f:
            saved = f.read()
        self.assertIn("scan=0\n", saved)
        self.assertIn("xbox=192.168.1.20, 192.168.1.21\n", saved)
        # Reads too: another site could load /api/browse?path=\\its-server\share as an image.
        self.assertEqual(self.call("GET", "/api/browse?path=%2Ftmp", page=False)[0], 403)
        self.assertEqual(self.call("GET", "/api/state", headers={"Sec-Fetch-Site": "cross-site"})[0], 403)
        self.assertEqual(self.call("GET", "/api/state", headers={"Sec-Fetch-Site": "same-origin"})[0], 200)
        # A line break would start another FTP command.
        self.assertEqual(self.call("POST", "/api/settings", {"aurora": "/Hdd1/Apps/Aurora\r\nRMD /Hdd1"})[0], 400)

    def test_settings_file_options_can_t_open_or_end_it(self):
        # A settings file (or a restored backup) can't do what only a command line should.
        with open(self.config, "w") as f:
            f.write("[Server]\noptions=-h\nfolders=1000000000000\n")
        self.start()
        self.assertIsNone(self.server.poll())
        self.assertEqual(self.json("GET", "/api/state")["settings"]["webAddress"], "127.0.0.1")
        with open(self.config, "w") as f:
            f.write("[Server]\noptions=--web-bind 0.0.0.0\n")
        self.restart()
        self.assertEqual(self.json("GET", "/api/state")["settings"]["webAddress"], "127.0.0.1")

    def test_saved_settings_win_over_the_command_line(self):
        os.makedirs(os.path.join(self.tmp.name, "more"))
        self.start("--xbox", "192.168.1.20")
        more = os.path.join(self.tmp.name, "more")
        self.json("POST", "/api/settings", {"xboxes": "192.168.1.30", "folders": [self.isos, more]})
        status, _, data = self.call("POST", "/api/settings", {"folders": [os.path.join(self.tmp.name, "none")]})
        self.assertEqual(status, 400)
        self.restart("--xbox", "192.168.1.20")
        state = self.json("GET", "/api/state")
        self.assertEqual(state["settings"]["xboxes"], ["192.168.1.30"])
        self.assertEqual([f["path"] for f in state["folders"]], [self.isos, more])

    def backups(self):
        return self.json("GET", "/api/backups")["backups"]

    def test_backs_up_and_restores_the_settings(self):
        self.start()
        self.json("POST", "/api/settings", {"scan": False, "xboxes": "192.168.1.20"})
        name = self.json("POST", "/api/backup", {})["name"]
        self.assertTrue(re.match(r"NetDisc360_\d{4}-\d\d-\d\d_\d\d-\d\d-\d\d_manual\.zip$", name), name)
        listed = self.backups()
        self.assertEqual([(b["name"], b["kind"], b["version"]) for b in listed], [(name, "manual", self.json("GET", "/api/state")["version"])])
        # Downloaded as a zip that holds the settings file, and what made it.
        status, response, data = self.call("GET", "/api/backup?name=" + name)
        self.assertEqual(status, 200)
        self.assertEqual(response.getheader("Content-Type"), "application/zip")
        self.assertIn('attachment; filename="%s"' % name, response.getheader("Content-Disposition"))
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            self.assertEqual(sorted(z.namelist()), ["backup.ini", "netdisc.ini"])
            self.assertIn(b"scan=0\n", z.read("netdisc.ini"))
            self.assertIn(b"kind=manual\n", z.read("backup.ini"))
        self.assertEqual(self.call("GET", "/api/backup?name=../netdisc.ini")[0], 404)
        # Restored by name: at once, with the settings before kept as a backup.
        self.json("POST", "/api/settings", {"scan": True, "xboxes": "192.168.1.30"})
        self.assertEqual(self.json("POST", "/api/restore", {"name": name})["notes"], [])
        settings = self.json("GET", "/api/state")["settings"]
        self.assertEqual((settings["scan"], settings["xboxes"]), (False, ["192.168.1.20"]))
        with open(self.config) as f:
            self.assertIn("xbox=192.168.1.20\n", f.read())
        self.assertEqual(sorted(b["kind"] for b in self.backups()), ["manual", "restore"])
        # Restored from a file: the zip as downloaded, or a netdisc.ini by itself.
        history = self.json("GET", "/api/state")["update"]["history"]
        self.json("POST", "/api/restore", {"file": base64.b64encode(
            b"[Server]\nxbox=192.168.1.77\nscan=1\nupdate_skip=9.9.9\nupdate_history=0.1.0|5|first||\n").decode()})
        self.assertEqual(self.json("GET", "/api/state")["settings"]["xboxes"], ["192.168.1.77"])
        # The update history and a skipped version are this computer's, not the backup's.
        self.assertEqual(self.json("GET", "/api/state")["update"]["history"], history)
        with open(self.config) as f:
            self.assertNotIn("update_skip", f.read())
        self.json("POST", "/api/restore", {"file": base64.b64encode(data).decode()})
        self.assertEqual(self.json("GET", "/api/state")["settings"]["xboxes"], ["192.168.1.20"])
        # Anything else is refused, and changes nothing.
        squeezed = io.BytesIO()
        with zipfile.ZipFile(squeezed, "w", zipfile.ZIP_DEFLATED) as z:
            z.writestr("netdisc.ini", "[Server]\nxbox=192.168.1.99\n")
        for wrong, why in [(b"hello", "isn't a NetDisc360 backup"), (squeezed.getvalue(), "compressed"), (data[:-30], "zip")]:
            status, _, answer = self.call("POST", "/api/restore", {"file": base64.b64encode(wrong).decode()})
            self.assertEqual(status, 400)
            self.assertIn(why, json.loads(answer)["error"])
        self.assertEqual(self.json("GET", "/api/state")["settings"]["xboxes"], ["192.168.1.20"])
        # Deleted.
        before = len(self.backups())
        self.json("POST", "/api/backup/delete", {"name": name})
        self.assertEqual(len(self.backups()), before - 1)
        self.assertNotIn(name, [b["name"] for b in self.backups()])

    def test_keeps_the_newest_automatic_backups(self):
        self.start()
        folder = os.path.join(self.tmp.name, "Backups")
        os.makedirs(folder)
        with open(self.config, "rb") as f:
            settings = f.read()
        for days in range(1, 5):
            when = int(time.time()) - days * 86400
            stamp = time.strftime("%Y-%m-%d_%H-%M-%S", time.localtime(when))
            with zipfile.ZipFile(os.path.join(folder, "NetDisc360_%s_scheduled.zip" % stamp), "w") as z:
                z.writestr("backup.ini", "[Backup]\nkind=scheduled\nversion=0.9\ntime=%d\n" % when)
                z.writestr("netdisc.ini", settings)
        self.json("POST", "/api/settings", {"backupKeep": 2, "backupEvery": 1})
        self.assertEqual(self.call("POST", "/api/settings", {"backupKeep": 0})[0], 400)
        self.json("POST", "/api/backup", {})
        kinds = [b["kind"] for b in self.backups()]
        self.assertEqual(kinds, ["manual", "scheduled", "scheduled"])  # Newest first; yours are never deleted.
        with open(self.config) as f:
            saved = f.read()
        self.assertIn("backup_keep=2\n", saved)
        self.assertIn("backup_every=1\n", saved)

    def test_finds_the_newest_release(self):
        releases = os.path.join(self.tmp.name, "releases.json")

        def release(tag, prerelease=False, draft=False):
            return {"tag_name": tag, "name": tag, "body": "## New\n- Things", "html_url": "https://example.com/" + tag,
                    "published_at": "2026-10-18T10:00:00Z", "draft": draft, "prerelease": prerelease,
                    "assets": [{"name": "NetDisc360.exe", "size": 1234, "browser_download_url": "https://example.com/NetDisc360.exe",
                                "digest": "sha256:" + "0" * 64}]}
        with open(releases, "w") as f:
            json.dump([release("v99.1.0-beta.2", prerelease=True), release("v99.9.0"), release("v99.10.0"),
                       release("v100.0.0", draft=True), release("v0.1.0")], f)
        self.start("--releases", releases)
        state = self.json("GET", "/api/state")
        self.assertRegex(state["version"], r"^\d+\.\d+\.\d+")
        self.assertEqual((state["update"]["how"], state["update"]["checks"], state["update"]["newer"]), ("manual", True, False))
        # Drafts and pre-releases aren't updates; 99.10 is newer than 99.9.
        update = self.json("POST", "/api/update/check", {})["update"]
        self.assertEqual((update["newer"], update["latest"]["version"], update["latest"]["exe"]), (True, "99.10.0", True))
        self.assertIn("## New", update["latest"]["notes"])
        self.assertNotIn("notes", self.json("GET", "/api/state")["update"]["latest"])
        self.json("POST", "/api/settings", {"updatePrerelease": True})
        self.assertEqual(self.json("POST", "/api/update/check", {})["update"]["latest"]["version"], "99.10.0")
        with open(releases, "w") as f:
            json.dump([release("v99.11.0-rc.1", prerelease=True), release("v99.10.0")], f)
        self.assertEqual(self.json("POST", "/api/update/check", {})["update"]["latest"]["version"], "99.11.0-rc.1")
        # Here (not NetDisc360.exe in the notification area) it's updated by hand.
        status, _, data = self.call("POST", "/api/update/install", {})
        self.assertEqual(status, 400)
        self.assertIn("by hand", json.loads(data)["error"])
        self.assertEqual(self.call("POST", "/api/settings", {"updateAuto": True})[0], 400)
        with open(releases, "w") as f:
            f.write("[]")
        update = self.json("POST", "/api/update/check", {})["update"]
        self.assertEqual((update["newer"], update["latest"]), (False, None))
        self.assertIn("no releases", update["error"])

    def test_lists_the_changes_since_this_version(self):
        releases = os.path.join(self.tmp.name, "releases.json")
        here = "0.0.1"

        def release(tag, prerelease=False):
            return {"tag_name": tag, "name": "", "body": "Notes of " + tag, "html_url": "https://example.com/" + tag,
                    "published_at": "2026-10-18T10:00:00Z", "draft": False, "prerelease": prerelease, "assets": []}
        with open(releases, "w") as f:
            json.dump([release("v99.1.0"), release("v99.3.0"), release("v99.2.0"), release("v99.4.0-beta.1", prerelease=True),
                       release("v0.0.1")], f)
        self.start("--releases", releases)
        version = self.json("GET", "/api/state")["version"]
        update = self.json("POST", "/api/update/check", {})["update"]
        # Every release, newest first, so the changes of each version since this one show, not only the newest's.
        self.assertEqual([r["version"] for r in update["releases"]], ["99.3.0", "99.2.0", "99.1.0", "0.0.1"])
        self.assertEqual([r["newer"] for r in update["releases"]], [True, True, True, False])
        self.assertEqual(update["releases"][1]["notes"], "Notes of v99.2.0")
        self.assertNotIn("releases", self.json("GET", "/api/state")["update"])
        self.json("POST", "/api/settings", {"updatePrerelease": True})
        update = self.json("POST", "/api/update/check", {})["update"]
        self.assertEqual(update["releases"][0]["version"], "99.4.0-beta.1")
        self.assertTrue(update["releases"][0]["prerelease"])
        self.assertNotEqual(version, here)

    def test_keeps_an_update_history(self):
        self.start()
        version = self.json("GET", "/api/state")["version"]
        # The first start.
        history = self.json("GET", "/api/state")["update"]["history"]
        self.assertEqual([(e["version"], e["how"], e["from"]) for e in history], [(version, "first", "")])
        self.assertAlmostEqual(history[0]["at"], time.time(), delta=60)
        # An update NetDisc360 installed: the old version wrote where it went, and its backup.
        with open(self.config) as f:
            saved = f.read()
        with open(self.config, "w") as f:
            f.write(saved.replace("update_history=%s|" % version, "update_history=0.9.0|") +
                    "updated_from=0.9.0\nupdate_to=%s\nupdate_backup=NetDisc360_2026-10-20_10-00-00_update.zip\n" % version)
        self.restart()
        history = self.json("GET", "/api/update")["update"]["history"]
        self.assertEqual([(e["version"], e["how"], e["from"], e["backup"]) for e in history],
                         [(version, "app", "0.9.0", "NetDisc360_2026-10-20_10-00-00_update.zip"), ("0.9.0", "first", "", "")])
        with open(self.config) as f:
            saved = f.read()
        self.assertNotIn("updated_from", saved)
        self.assertNotIn("update_backup", saved)
        # An update that didn't start: this version was put back.
        with open(self.config, "w") as f:
            f.write(saved + "updated_from=%s\nupdate_to=99.0.0\n" % version)
        self.restart()
        history = self.json("GET", "/api/state")["update"]["history"]
        self.assertEqual([(e["version"], e["how"], e["from"]) for e in history[:2]], [("99.0.0", "failed", version), (version, "app", "0.9.0")])
        # Put in place another way (by hand, or a new Docker image).
        with open(self.config) as f:
            saved = f.read()
        with open(self.config, "w") as f:
            f.write(re.sub(r"(?m)^update_history=.*$", "update_history=0.9.5|100|app|0.9.0|", saved))
        self.restart()
        history = self.json("GET", "/api/state")["update"]["history"]
        self.assertEqual([(e["version"], e["how"], e["from"]) for e in history], [(version, "other", "0.9.5"), ("0.9.5", "app", "0.9.0")])
        # A restart without a new version adds nothing; nonsense in the setting is left out.
        with open(self.config) as f:
            saved = f.read()
        with open(self.config, "w") as f:
            f.write(saved.replace("update_history=", "update_history=<b>|x|evil|a|../../x,"))
        self.restart()
        again = self.json("GET", "/api/state")["update"]["history"]
        self.assertEqual(again[0], history[0])
        self.assertTrue(all(e["backup"] in ("", "NetDisc360_2026-10-20_10-00-00_update.zip") for e in again))
        self.assertTrue(all(e["how"] in ("app", "other", "first", "failed") for e in again))
        self.assertTrue(all(re.fullmatch(r"[0-9A-Za-z.+-]+", e["version"]) for e in again))

    def test_a_password_locks_it(self):
        self.start()
        status, response, _ = self.call("POST", "/api/password", {"password": "hunter2 x"})
        self.assertEqual(status, 200)
        self.assertIn("HttpOnly", response.getheader("Set-Cookie"))
        cookie = response.getheader("Set-Cookie").split(";")[0]
        # Without the cookie: only the login page.
        self.assertEqual(self.call("GET", "/api/state")[0], 401)
        self.assertEqual(self.json("GET", "/api/auth"), {"password": True, "loggedIn": False})
        self.assertEqual(self.call("GET", "/api/state", cookie=cookie)[0], 200)
        # With a password, any name for this computer works.
        self.assertEqual(self.call("GET", "/api/state", cookie=cookie, headers={"Host": "netdisc.example.com"})[0], 200)
        started = time.time()
        self.assertEqual(self.call("POST", "/api/login", {"password": "hunter2"})[0], 401)
        self.assertGreaterEqual(time.time() - started, 0.9)
        status, response, _ = self.call("POST", "/api/login", {"password": "hunter2 x"})
        self.assertEqual(status, 200)
        self.assertTrue(response.getheader("Set-Cookie").startswith("netdisc_session="))
        # Kept in the settings file, hashed; sessions last across a restart.
        with open(self.config) as f:
            saved = f.read()
        self.assertIn("web_password=", saved)
        self.assertNotIn("hunter2", saved)
        self.restart()
        self.assertEqual(self.call("GET", "/api/state")[0], 401)
        self.assertEqual(self.call("GET", "/api/state", cookie=cookie)[0], 200)
        # A new password ends the old sessions.
        status, response, _ = self.call("POST", "/api/password", {"password": "other"}, cookie=cookie)
        self.assertEqual(status, 200)
        self.assertEqual(self.call("GET", "/api/state", cookie=cookie)[0], 401)
        self.assertEqual(self.call("GET", "/api/state", cookie=response.getheader("Set-Cookie").split(";")[0])[0], 200)
        # The hash in the settings file (and so in every backup) can't make a session.
        with open(self.config) as f:
            stored = re.search(r"web_password=(\S+)", f.read()).group(1)
        end = str(int(time.time()) + 3600)
        forged = "netdisc_session=%s.%s" % (end, hmac.new(stored.encode(), end.encode(), hashlib.sha256).hexdigest())
        self.assertEqual(self.call("GET", "/api/state", cookie=forged)[0], 401)

    def test_takes_the_password_from_the_environment(self):
        os.environ["NETDISC_WEB_PASSWORD"] = "from docker"
        try:
            self.start()
        finally:
            del os.environ["NETDISC_WEB_PASSWORD"]
        self.assertEqual(self.call("GET", "/api/state")[0], 401)
        status, response, _ = self.call("POST", "/api/login", {"password": "from docker"})
        self.assertEqual(status, 200)
        cookie = response.getheader("Set-Cookie").split(";")[0]
        self.assertEqual(self.call("POST", "/api/password", {"password": "x"}, cookie=cookie)[0], 400)
        self.assertTrue(self.json("GET", "/api/state", cookie=cookie)["settings"]["passwordFixed"])



if __name__ == "__main__":
    unittest.main()
