"""Tests the PC server (server/) on Linux: the NetISO protocol as the plugin
and NetISO Sync use it, pushing game folders to tests/aurora_ftp.py, a
stand-in for Aurora's FTP server, and the dashboard's web API.

Builds build/host/netdisc with g++ (or uses NETDISC_SERVER if set).

    python tests/test_server.py
"""
import gzip
import http.client
import json
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
    sources = [os.path.join(ROOT, p) for p in ("server/main.cpp", "server/discs.cpp", "server/ftp.cpp", "server/web.cpp",
                                               "server/config.cpp", "shared/library.cpp", "shared/isvr.cpp")]
    subprocess.run(["g++", "-std=c++11", "-O1", "-Wall", "-pthread", "-include", os.path.join(HERE, "posix_compat.h"),
                    "-o", SERVER] + sources, check=True)


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
            self.assertEqual(sorted(os.listdir(self.games)), ["a", "b"])
            self.assertEqual(len(self.ftp.scans), scans)
            self.assertTrue(self.wait_for(lambda: not os.path.exists(os.path.join(self.games, "b")), 20))
            self.assertGreaterEqual(time.time() - started, 6)
            self.assertTrue(self.wait_for(lambda: len(self.ftp.scans) == scans + 1, 10))
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

    def call(self, method, path, body=None, page=True, cookie=None, headers=None):
        """(status, response, data); a POST comes from the page unless page is False."""
        sent = {"Host": "127.0.0.1:%d" % self.web}
        if method == "POST":
            sent["Content-Type"] = "application/json"
            if page:
                sent["X-NetDisc"] = "1"
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

    def test_serves_the_page_and_the_games(self):
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        self.start()
        status, response, data = self.call("GET", "/")
        self.assertEqual(status, 200)
        self.assertEqual(response.getheader("Content-Encoding"), "gzip")
        self.assertIn("default-src 'none'", response.getheader("Content-Security-Policy"))
        self.assertIn(b"<title>NetDisc 360</title>", gzip.decompress(data))
        state = self.state_when(lambda s: s["library"]["games"] == 1)
        self.assertEqual(state["library"]["games"], 1)
        self.assertEqual(state["folders"], [{"path": self.isos, "games": 1, "found": True}])
        self.assertTrue(state["settings"]["saved"])
        self.assertTrue(state["settings"]["ftpPasswordSet"])
        games = self.json("GET", "/api/games")["games"]
        self.assertEqual([(g["name"], g["kind"], g["titleId"]) for g in games], [("Halo 3", "iso", "4D5307E6")])
        self.assertEqual(self.call("GET", "/api/nothing")[0], 404)

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
