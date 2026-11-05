"""Tests the PC server (server/) on Linux: the NetISO protocol as the plugin
and NetISO Sync use it, pushing game folders to tests/aurora_ftp.py, a
stand-in for Aurora's FTP server, and the dashboard's web API.

Builds build/host/netdisc with g++ (or uses NETDISC_SERVER if set).

    python tests/test_server.py
"""
import base64
import contextlib
import gzip
import ftplib
import hashlib
import hmac
import http.client
import http.server
import io
import json
import os
import re
import shutil
import socket
import socketserver
import sqlite3
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.parse
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import aurora_ftp  # noqa: E402
try:  # Lua, to run the scripts NetDisc360 puts in Aurora; tests that need it skip without it.
    import lupa
    try:
        import lupa.lua51  # noqa: F401  Aurora's Lua is 5.1.
    except ImportError:
        pass
except ImportError:
    lupa = None

AURORA_TU_TABLES = (
    "CREATE TABLE TitleUpdates ( Id INTEGER PRIMARY KEY AUTOINCREMENT, DisplayName TEXT COLLATE NOCASE, FileName TEXT COLLATE NOCASE, "
    "LiveDeviceId TEXT COLLATE NOCASE, LivePath TEXT COLLATE NOCASE, TitleId INTEGER, MediaId INTEGER, BaseVersion INTEGER, Version INTEGER, "
    "Hash TEXT COLLATE NOCASE, BackupPath TEXT COLLATE NOCASE, FileSize INTEGER, UNIQUE ( Hash, LiveDeviceId, LivePath ) )",
    "CREATE TABLE ActiveTitleUpdates ( Id INTEGER PRIMARY KEY, TitleUpdateId INTEGER, UNIQUE( TitleUpdateId ) )")


def run_aurora_script(path, xbox, content, settings):
    """Runs one of NetDisc360's Aurora filter scripts as Aurora does when it
    starts: Sql on content.db with settings.db as Config, and FileSystem on
    the fake console's drive (xbox)."""
    lua = getattr(lupa, "lua51", None) or lupa  # Aurora's Lua is 5.1.
    runtime = lua.LuaRuntime(unpack_returned_tuples=True)
    db = sqlite3.connect(content)
    db.execute("ATTACH DATABASE ? AS Config", (settings,))

    def execute(query):
        try:
            db.execute(query)
            db.commit()
            return True
        except sqlite3.Error:
            return False

    def fetch(query):
        cursor = db.execute(query)
        names = [d[0] for d in cursor.description]
        return runtime.table_from([runtime.table_from({n: (None if v is None else str(v)) for n, v in zip(names, row)}) for row in cursor.fetchall()])

    def local(name):
        drive, rest = name.split(":", 1)
        return os.path.join(xbox, drive, *[p for p in rest.split("\\") if p])

    def write(name, text):
        with open(local(name), "w", newline="") as f:
            f.write(text)
        return True

    runtime.globals().Sql = runtime.table_from({"Execute": execute, "ExecuteFetchRows": fetch})
    runtime.globals().FileSystem = runtime.table_from({"FileExists": lambda name: os.path.exists(local(name)), "WriteFile": write})
    runtime.globals().GameListFilterCategories = runtime.table_from({"User": runtime.table_from({})})
    with open(path, encoding="utf-8") as f:
        runtime.execute(f.read())
    db.close()
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
    # The dashboard page as the server builds it in (not kept in git).
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "make_web.py")], check=True)
    sources = [os.path.join(ROOT, p) for p in ("server/main.cpp", "server/discs.cpp", "server/discmap.cpp", "server/xefu.cpp", "server/ftp.cpp",
                                               "server/web.cpp", "server/config.cpp", "server/backup.cpp", "server/update.cpp",
                                               "server/installed.cpp", "server/aurora.cpp", "server/auroradb.cpp", "server/titleupdates.cpp", "server/system.cpp", "server/saves.cpp", "server/wake.cpp",
                                               "server/party.cpp", "server/guests.cpp", "server/picture.cpp", "server/systemlink.cpp", "server/relay.cpp", "server/users.cpp", "shared/library.cpp", "shared/isvr.cpp")]
    subprocess.run(["g++", "-std=c++11", "-O1", "-Wall", "-pthread", "-include", os.path.join(HERE, "posix_compat.h"),
                    "-o", SERVER] + sources, check=True)


def lua_string(text):
    """text as a Lua string literal."""
    return '"' + "".join("\\%03d" % ord(c) if c in '\\"' or ord(c) < 32 or ord(c) > 126 else c for c in text) + '"'


def write_tree_iso(path, tree, start=0x2080000):
    """An XDVDFS image (XGD3 layout) of a tree {name: bytes or {name: ...}}:
    each directory's entries one after another, as the server reads them.
    A number for a file is that many zero bytes, left sparse."""
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
            elif isinstance(value, int):
                sector, size, attributes = next_sector[0], value, 0x80
                next_sector[0] += max(1, (value + SECTOR - 1) // SECTOR)
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
    # Free on every address, as the server listens on all of them: a test's
    # second console (127.0.0.2) leaves FTP data connections in TIME_WAIT on
    # its own address, which a port free on 127.0.0.1 alone can be held by.
    with socket.socket() as s:
        s.bind(("", 0))
        return s.getsockname()[1]


AURORA_XEX = "\\Device\\Harddisk0\\Partition1\\Apps\\Aurora\\Aurora.xex"


# Aurora's web server's routes: the newer API's (Aurora 0.7b.2's own Nova)
# and the older Nova's of the NetISO kit, by what they do.
AURORA_ROUTES = {
    "title": ("GET", "/title", "GET", "/getGameInfo"),
    "launch": ("POST", "/title/launch", "POST", "/postLaunchGame"),
    "temperature": ("GET", "/temperature", "GET", "/getTemperatureInfo"),
    "profiles": ("GET", "/profile", "GET", "/getProfileInfo"),
    "memory": ("GET", "/memory", "GET", "/getMemoryInfo"),
    "shots": ("GET", "/screencapture/meta/list", "GET", "/getScreenCaptures"),
    "take": ("GET", "/screencapture/meta", "GET", "/getScreenCapture"),
    "shot": ("GET", "/image/screencapture", "GET", "/getScreenCaptureImage"),
    "delete": ("DELETE", "/screencapture", "POST", "/postScreenCaptureDelete"),
}
SHOT = b"BM" + bytes(60)  # A screenshot as Aurora gives it: a BMP.


class AuroraWebHandler(http.server.BaseHTTPRequestHandler):
    """Aurora's web server as NetISO-Manager uses it (port 9999): a login
    (xboxhttp/xboxhttp) for a token, what runs, starting a title, the
    console's temperatures, profiles and screenshots. With basic (Aurora's
    "HTTP security"), every request needs HTTP's own login, /authenticate
    too. With old, it's the NetISO kit's older Nova: no /authenticate, its
    own paths for the rest, and a session cookie."""

    def log_message(self, *args):
        pass

    def answer(self, status, body, kind="application/json"):
        if self.server.basic and self.headers.get("Authorization") != "Basic " + base64.b64encode(b"xboxhttp:xboxhttp").decode():
            status, body, kind = 401, {}, "application/json"
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(status)
        if self.server.old:
            self.send_header("Set-Cookie", "fsplugin_http_session=s1; path=/")
        if status == 401 and self.server.basic:
            self.send_header("WWW-Authenticate", 'Basic realm="Freestyle Plugin"')
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def form(self):
        length = int(self.headers.get("Content-Length") or 0)
        return dict(urllib.parse.parse_qsl(self.rfile.read(length).decode()))

    def allowed(self):
        return self.server.basic or self.headers.get("Authorization") == "Bearer t0ken"

    def route(self):
        path, _, query = self.path.partition("?")
        for name, (method, new, old_method, old) in AURORA_ROUTES.items():
            if (self.command, path) == ((old_method, old) if self.server.old else (method, new)):
                return name, dict(urllib.parse.parse_qsl(query))
        return None, {}

    def handle_any(self):
        form = self.form() if self.command == "POST" else {}
        name, query = self.route()
        if self.path == "/authenticate" and not self.server.old:
            if form.get("username") == "xboxhttp" and form.get("password") == "xboxhttp":
                self.answer(200, {"token": "t0ken"})
            else:
                self.answer(401, {})
        elif not name:
            self.answer(404, {})
        elif not self.allowed():
            self.answer(401, {})
        elif name == "title":
            self.answer(200, {"path": self.server.running, "titleid": "0xFFFE07D1"})
        elif name == "launch":
            self.server.launched.append((form.get("path"), form.get("exec"), form.get("type")))
            self.server.cookies.append(self.headers.get("Cookie"))
            if self.server.on_launch:
                self.server.on_launch(form.get("path"), form.get("exec"))
            self.answer(200, {})
        elif name == "temperature":
            self.answer(200, {"celsius": True, "cpu": 51.25, "gpu": 48.0, "memory": 45.5, "case": 40.0})
        elif name == "profiles":
            self.answer(200, self.server.profiles)
        elif name == "memory":
            self.answer(200, {"total": 536870912, "used": 268435456, "free": 268435456})
        elif name == "shots":
            self.answer(200, [{"filename": f, "titleid": "0x4D5307E6", "timestamp": f[8:] + "0"} for f in self.server.shots])
        elif name == "take":
            self.server.shots.append("00000000%014d" % (20261026230000 + len(self.server.shots)))
            self.answer(200, {"filename": self.server.shots[-1]})
        elif name == "shot":
            if query.get("uuid") in self.server.shots:
                self.answer(200, SHOT, "image/bmp")
            else:
                self.answer(404, {})
        elif name == "delete":
            uuid = query.get("uuid") or form.get("uuid")
            if uuid in self.server.shots:
                self.server.shots.remove(uuid)
                self.answer(200, {})
            else:
                self.answer(404, {})

    do_GET = do_POST = do_DELETE = handle_any


class AuroraWeb(http.server.ThreadingHTTPServer):
    """host and port: a second console's, on 127.0.0.2 at the first one's
    port (the server asks every console's Aurora on one port)."""
    daemon_threads = True

    def __init__(self, running=AURORA_XEX, basic=False, old=False, host="127.0.0.1", port=0):
        super().__init__((host, port), AuroraWebHandler)
        self.running = running
        self.basic = basic
        self.old = old
        self.cookies = []
        self.launched = []
        self.on_launch = None  # Called with a start's path and file, to act as what it starts.
        self.profiles = [{"index": 0, "gamertag": "Player One", "gamerscore": 1000, "signedin": 1, "xuid": "0009000000000001"},
                         {"index": 1, "gamertag": "", "gamerscore": 0, "signedin": 0, "xuid": "0000000000000000"}]
        self.shots = ["000000002026101900442758"]
        self.port = self.server_address[1]
        threading.Thread(target=self.serve_forever, daemon=True).start()

    def close(self):
        self.shutdown()
        self.server_close()


class Xbdm(socketserver.ThreadingTCPServer):
    """The console's debug monitor (xbdm, port 730), as far as turning it off
    goes; host and port as for AuroraWeb. With jrpc (JRPC2 loaded), its
    notifications (consolefeatures type=12) are answered and kept in notes,
    as the text shown; commands has the rest."""
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, host="127.0.0.1", port=0, jrpc=True):
        self.commands = []
        self.notes = []
        self.jrpc = jrpc
        outer = self

        class Handler(socketserver.StreamRequestHandler):
            def handle(self):
                self.wfile.write(b"201- connected\r\n")
                for line in self.rfile:
                    command = line.strip().decode()
                    if command.startswith("consolefeatures "):
                        found = re.match(r'consolefeatures ver=2 type=12 params="A\\0\\A\\2\\2/(\d+)\\([0-9A-F]*)\\1\\(\d+)\\"$', command)
                        if not outer.jrpc:
                            self.wfile.write(b"407- unknown command\r\n")
                        elif found and int(found.group(1)) * 2 == len(found.group(2)):
                            outer.notes.append(bytes.fromhex(found.group(2)).decode("ascii"))
                            self.wfile.write(b"200- S_OK\r\n")
                        else:
                            self.wfile.write(b"200- error=The paramaters were not found\r\n")
                        continue
                    if command == "bye":  # only goodbye: not kept
                        self.wfile.write(b"200- bye\r\n")
                        return
                    outer.commands.append(command)
                    if command in ("shutdown", "magicboot COLD"):
                        return  # the console powers off or restarts: xbdm closes without an answer
                    self.wfile.write(b"407- unknown command\r\n")

        super().__init__((host, port), Handler)
        self.port = self.server_address[1]
        threading.Thread(target=self.serve_forever, daemon=True).start()

    def close(self):
        self.shutdown()
        self.server_close()


class MediaSharing(http.server.ThreadingHTTPServer):
    """An Xbox 360's media sharing description (port 1027), as consoles give
    it: its own name (friendlyName) and fixed id (UDN)."""
    daemon_threads = True

    def __init__(self, name="FABLE", udn="uuid:46687478-4105-2000-0000-0022484BE1C5", model="Xbox 360", host="127.0.0.1", port=0):
        outer = self
        self.name, self.udn, self.model, self.asked = name, udn, model, 0

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                outer.asked += 1
                body = ('<?xml version="1.0"?><root xmlns="urn:schemas-upnp-org:device-1-0"><specVersion><major>1</major>'
                        '<minor>0</minor></specVersion><device><deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>'
                        '<manufacturer>Microsoft Corporation</manufacturer><modelName>%s</modelName><modelNumber>2</modelNumber>'
                        '<friendlyName>%s</friendlyName><serialNumber>466874784105</serialNumber><UDN>%s</UDN></device></root>'
                        % (outer.model, outer.name, outer.udn)).encode()
                self.send_response(200)
                self.send_header("Content-Type", 'text/xml; charset="utf-8"')
                self.send_header("Connection", "close")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        super().__init__((host, port), Handler)
        self.port = self.server_address[1]
        threading.Thread(target=self.serve_forever, daemon=True).start()

    def close(self):
        self.shutdown()
        self.server_close()


class FakeUnityHandler(http.server.BaseHTTPRequestHandler):
    """XboxUnity's TitleUpdateInfo.php and TitleUpdate.php, from self.server.titles
    ({Title ID: answer}) and self.server.files ({id: (name, bytes)}); and its
    TitleList.php and Icon.php, from self.server.names and .icons ({Title ID: ...})."""
    def log_message(self, *args):
        pass

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        query = urllib.parse.parse_qs(url.query)
        self.server.asked.append(self.path)
        if url.path == "/Resources/Lib/TitleUpdateInfo.php":
            body = json.dumps(self.server.titles.get(query.get("titleid", [""])[0], [])).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            # Chunked, as PHP sends it.
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for at in range(0, len(body), 100):
                part = body[at:at + 100]
                self.wfile.write(b"%x\r\n%s\r\n" % (len(part), part))
            self.wfile.write(b"0\r\n\r\n")
            return
        names, icons = getattr(self.server, "names", {}), getattr(self.server, "icons", {})
        if url.path == "/Resources/Lib/TitleList.php":
            search = query.get("search", [""])[0].upper()
            items = [{"TitleID": search, "HBTitleID": "00000000", "Name": names[search], "TitleType": "360"}] if search in names else []
            body = json.dumps({"Items": items, "Count": len(items), "Pages": 1, "Page": 0}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html")  # As XboxUnity labels its JSON.
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if url.path == "/Resources/Lib/Icon.php" and query.get("tid", [""])[0].upper() in icons:
            body = icons[query["tid"][0].upper()]
            self.send_response(200)
            self.send_header("Content-Type", "image/png")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if url.path == "/Resources/Lib/TitleUpdate.php" and query.get("tuid", [""])[0] in self.server.files:
            name, data = self.server.files[query["tuid"][0]]
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Disposition", 'attachment; filename="%s"' % name)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        self.send_response(404)
        self.end_headers()


def tu_package(title_id, media_id, name, size=0x3000):
    """A title update package as the console keeps it: a LIVE header with its IDs."""
    data = bytearray(b"LIVE" + bytes(size - 4))
    struct.pack_into(">I", data, 0x344, 0xB0000)
    struct.pack_into(">I", data, 0x354, media_id)
    struct.pack_into(">I", data, 0x360, title_id)
    encoded = name.encode("utf-16-be")
    data[0x411:0x411 + len(encoded)] = encoded
    data[-16:] = os.urandom(16)
    data[0x32C:0x340] = hashlib.sha1(bytes(data[0x344:])).digest()  # Its Content ID, XboxUnity's "hash".
    return bytes(data)


def content_id(package):
    return package[0x32C:0x340].hex()


def god_header(media_id, title_id, disc=1):
    """A Games on Demand package's header file, as far as it names the game."""
    header = bytearray(0x400)
    header[0:4] = b"LIVE"
    struct.pack_into(">I", header, 0x344, 0x7000)
    struct.pack_into(">I", header, 0x354, media_id)
    struct.pack_into(">I", header, 0x360, title_id)
    header[0x366] = disc
    return bytes(header)


def title_update_dbs(folder, updates, active, table_key=False):
    """Aurora's content.db and settings.db in folder (its Data/Databases), as
    Aurora 0.7b.2 has their title update tables: updates as (id, title id,
    media id, base version, version, name, file), active the ids in use.
    DWORDs go in as signed numbers, as Aurora keeps them. table_key: Id is
    the table's own PRIMARY KEY (Id) instead of its column's."""
    os.makedirs(folder, exist_ok=True)
    signed = lambda v: v - (1 << 32) if v >= 1 << 31 else v
    for name in ("content.db", "settings.db"):
        if os.path.exists(os.path.join(folder, name)):
            os.remove(os.path.join(folder, name))
    db = sqlite3.connect(os.path.join(folder, "content.db"))
    db.execute("CREATE TABLE ContentItems ( Id INTEGER PRIMARY KEY AUTOINCREMENT, Directory TEXT, TitleId INTEGER, TitleName TEXT )")
    db.execute("CREATE TABLE TitleUpdates ( Id\t\tINTEGER%s, DisplayName TEXT, FileName TEXT, LiveDeviceId TEXT, LivePath TEXT, TitleId INTEGER, "
               "MediaId INTEGER, BaseVersion INTEGER, Version INTEGER, Hash TEXT, BackupPath TEXT, FileSize INTEGER%s )"
               % (("", ", PRIMARY KEY (Id)") if table_key else (" PRIMARY KEY AUTOINCREMENT", "")))
    for number, title, media, base, version, name, file in updates:
        db.execute("INSERT INTO TitleUpdates (Id, DisplayName, FileName, LiveDeviceId, LivePath, TitleId, MediaId, BaseVersion, Version, Hash, BackupPath, FileSize) "
                   "VALUES (?, ?, ?, '0000000000000000', ?, ?, ?, ?, ?, x'00112233', '', 1048576)",
                   (number, name, file, "Hdd1:\\Content\\0000000000000000\\%08X\\000B0000\\%s" % (title, file), signed(title), signed(media), signed(base), version))
    db.commit()
    db.close()
    db = sqlite3.connect(os.path.join(folder, "settings.db"))
    db.execute("CREATE TABLE ActiveTitleUpdates ( Id INTEGER PRIMARY KEY, TitleUpdateId INTEGER UNIQUE )")
    for number in active:
        db.execute("INSERT INTO ActiveTitleUpdates (TitleUpdateId) VALUES (?)", (number,))
    db.commit()
    db.close()


def spin_at(spin, ms):
    """Where the wheel is ms into a spin, as SpinAngle (server/party.cpp) and
    spinAt (web/index.html) work it out: (angle, step, stopped, stops so far)."""
    angle, t, step, stopped = 0.0, ms - spin["leadMs"], -1, 1
    for i, s in enumerate(spin["steps"]):
        if i:
            t -= spin["pauseMs"]
        if t < 0:
            break
        step = i
        if t < s["ms"]:
            f = 1 - t / s["ms"]
            angle += (s["to"] - angle) * (1 - f * f * f)
            stopped = 0
            break
        angle = s["to"]
        t -= s["ms"]
    return angle, step, stopped, 0 if step < 0 else step + 1 if stopped else step


PARTY_PARSE = os.path.join(ROOT, "build", "host", "party-parse")


def build_party_parse():
    """tests/party_parse.cpp with NetDiscParty.xex's party/state.cpp, built once."""
    if not getattr(build_party_parse, "built", False):
        os.makedirs(os.path.dirname(PARTY_PARSE), exist_ok=True)
        subprocess.run(["g++", "-std=c++11", "-O1", "-Wall", "-o", PARTY_PARSE, os.path.join(HERE, "party_parse.cpp"),
                        os.path.join(ROOT, "party", "state.cpp")], check=True)
        build_party_parse.built = True
    return PARTY_PARSE


PARTY_FTP = os.path.join(ROOT, "build", "host", "party-ftp")


def build_party_ftp():
    """tests/party_ftp.cpp with NetDiscParty.xex's FTP server (party/ftp.cpp), built once."""
    if not getattr(build_party_ftp, "built", False):
        os.makedirs(os.path.dirname(PARTY_FTP), exist_ok=True)
        subprocess.run(["g++", "-std=c++11", "-O1", "-Wall", "-pthread", "-o", PARTY_FTP, os.path.join(HERE, "party_ftp.cpp"),
                        os.path.join(ROOT, "party", "ftp.cpp")], check=True)
        build_party_ftp.built = True
    return PARTY_FTP


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


def xbe(title_id):
    """An original Xbox game's default.xbe with title_id in its certificate."""
    base, cert = 0x10000, 0x10180
    data = bytearray(0x300)
    data[0:4] = b"XBEH"
    struct.pack_into("<I", data, 0x104, base)
    struct.pack_into("<I", data, 0x118, cert)
    struct.pack_into("<I", data, cert - base + 8, title_id)
    return bytes(data)


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

    def ports(self):
        """Extra arguments with ports of their own, picked again on each try."""
        return []

    def start(self, *extra):
        # free_port's port can be taken by someone else before the server
        # binds it; then the server says so and stops, and gets another.
        for _ in range(3):
            self.port = free_port()
            self.server = subprocess.Popen([SERVER, self.isos, "--port", str(self.port), "--no-push", "--web-port", "0", "--no-auto-profiles", "--xboxunity", ""] + self.ports() + list(extra),
                                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            deadline = time.time() + 10
            said = []
            while time.time() < deadline:
                line = self.server.stdout.readline()
                if "Listening on port" in line:
                    return
                if line:
                    said.append(line.rstrip())
                elif self.server.poll() is not None:
                    break
            if not any("an't listen on" in line for line in said):
                break
            self.server.kill()
            self.server.wait()
            self.server.stdout.close()
        self.fail("server didn't start:\n" + "\n".join(said[-20:]))

    def add_iso(self, name, media_id, title_id, folder=None):
        path = os.path.join(folder or self.isos, name)
        write_iso(path, {"default.xex": xex(execution_info(media_id, title_id))})
        return path

    # The dashboard's API on port self.web.
    def call(self, method, path, body=None, page=True, cookie=None, headers=None):
        """(status, response, data); an API call comes from the page unless page
        is False. A body in bytes goes as it is (JSON that json.dumps can't make)."""
        sent = {"Host": "127.0.0.1:%d" % self.web}
        if page and path.startswith("/api/"):
            sent["X-NetDisc"] = "1"
        if method == "POST":
            sent["Content-Type"] = "application/json"
        if cookie:
            sent["Cookie"] = cookie
        sent.update(headers or {})
        data = None if body is None else body if isinstance(body, bytes) else json.dumps(body)
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

    def test_follows_linked_folders_but_not_loops(self):
        elsewhere = os.path.join(self.tmp.name, "nas")
        os.makedirs(elsewhere)
        self.add_iso("Linked.iso", 1, 0x11111111, elsewhere)
        os.symlink(elsewhere, os.path.join(self.isos, "share"))
        os.symlink(self.isos, os.path.join(self.isos, "loop"))  # Back to itself.
        other = os.path.join(self.tmp.name, "other")
        os.makedirs(other)
        os.symlink(self.add_iso("Other.iso", 2, 0x22222222, other), os.path.join(self.isos, "Also.iso"))
        self.start()
        c = Client(self.port)
        self.assertEqual(c.names(), ["Also.iso", "Linked.iso"])
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
        self.games = os.path.join(self.xbox, "Hdd1", "NetDisc360", "Games")
        self.app = os.path.join(self.xbox, "Hdd1", "Apps", "NetDisc360")
        self.ftp = aurora_ftp.Server(self.xbox, free_port())  # Free on 127.0.0.2 too (second_console).
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
                                 "--launcher", self.launcher, "--boot", self.boot, "--xboxunity", ""] + list(extra),
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
        self.assertEqual(sorted(os.listdir(folder)), ["default.xex", "netiso.ini"])
        # The helper is one file for all the game folders, in a folder of its own.
        self.assertEqual(os.listdir(self.app), ["boot.xex"])
        with open(os.path.join(self.app, "boot.xex"), "rb") as f:
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
        self.assertEqual([p for p in self.ftp.stored if "NetDisc360/Games" in p], [])

    def read_god_files(self, header_path):
        """The files at the top of a GOD package's disc, its hashes checked on the way."""
        import hashlib
        sha = lambda b: hashlib.sha1(b).digest()
        with open(header_path, "rb") as f:
            h = f.read()
        self.assertEqual(h[0x32C:0x340], sha(h[0x344:0x344 + 0xACBC]))
        blocks = int.from_bytes(h[0x392:0x395], "big")
        self.assertEqual(int.from_bytes(h[0x3A0:0x3A4], "little"), 1)
        with open(os.path.join(header_path + ".data", "Data0000"), "rb") as f:
            part = f.read()
        master = part[:4096]
        self.assertEqual(sha(master), h[0x37D:0x37D + 20])
        data = bytearray()
        at = 4096
        for g in range((blocks + 0xCB) // 0xCC):
            hashes = part[at:at + 4096]
            self.assertEqual(master[g * 20:g * 20 + 20], sha(hashes))
            n = min(0xCC, blocks - g * 0xCC)
            for b in range(n):
                block = part[at + 4096 * (1 + b):at + 4096 * (2 + b)]
                self.assertEqual(hashes[b * 20:b * 20 + 20], sha(block))
                data += block
            at += 4096 * (1 + n)
        self.assertEqual(at, len(part))
        self.assertEqual(bytes(data[0x10000:0x10014]), b"MICROSOFT*XBOX*MEDIA")
        root, size = struct.unpack_from("<II", data, 0x10014)
        files, table = {}, bytes(data[root * 2048:root * 2048 + size])
        at = 0
        while at + 14 <= len(table):
            if table[at:at + 2] == b"\xff\xff":
                at = (at // 2048 + 1) * 2048
                continue
            sector, length, _, n = struct.unpack_from("<IIBB", table, at + 4)
            files[table[at + 14:at + 14 + n].decode()] = bytes(data[sector * 2048:sector * 2048 + length])
            at += (14 + n + 3) & ~3
        return h, files

    def test_gives_a_console_its_games_as_games_on_demand_where_set(self):
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        self.add_iso("Skate 3.iso", 0x5C087C2C, 0x454108E6)
        content = os.path.join(self.xbox, "Hdd1", "Content", "0000000000000000")
        # A real copy of another game, which stays.
        os.makedirs(os.path.join(content, "41560817", "00007000"))
        with open(os.path.join(content, "41560817", "00007000", "AABBCCDD"), "wb") as f:
            f.write(god_header(0xAABBCCDD, 0x41560817))
        config = os.path.join(self.tmp.name, "netdisc.ini")
        with open(config, "w") as f:
            f.write("tile_choices=1\ntile_choice1=127.0.0.1|god|folder:Skate 3.iso\n")
        out = self.push(extra=("--config", config))
        # Halo 3 as Games on Demand: its game folder where Aurora doesn't look, and a package in Content.
        self.assertEqual(sorted(os.listdir(self.games)), ["Skate 3"])
        home = os.path.join(self.xbox, "Hdd1", "NetDisc360", "GOD")
        self.assertEqual(sorted(os.listdir(home)), ["Halo 3"])
        self.assertEqual(sorted(os.listdir(os.path.join(home, "Halo 3"))), ["default.xex", "netiso.ini"])
        with open(os.path.join(self.app, "boot.xex"), "rb") as f:  # The one helper both share.
            self.assertEqual(f.read(), b"BOOT HELPER")
        packages = os.path.join(content, "4D5307E6", "00007000")
        name = "4E443301" + "1A2B3C4D"
        self.assertEqual(sorted(os.listdir(packages)), [name, name + ".data"])
        self.assertEqual(os.listdir(os.path.join(packages, name + ".data")), ["Data0000"])
        header, files = self.read_god_files(os.path.join(packages, name))
        self.assertEqual(header[:4], b"LIVE")
        be = lambda at: int.from_bytes(header[at:at + 4], "big")
        self.assertEqual((be(0x344), be(0x354), be(0x360)), (0x7000, 0x1A2B3C4D, 0x4D5307E6))
        self.assertEqual(header[0x411:0x411 + 12].decode("utf-16-be"), "Halo 3")
        with open(os.path.join(home, "Halo 3", "default.xex"), "rb") as f:
            self.assertEqual(files["default.xex"], f.read())
        self.assertIn(b"tile=Hdd1:\\NetDisc360\\GOD\\Halo 3\r\n", files["netiso.ini"])
        self.assertLess(os.path.getsize(os.path.join(packages, name + ".data", "Data0000")), 1 << 20)
        self.assertIn("put Halo 3 in as Games on Demand", out)
        self.assertIn("/Hdd1/Content/0000000000000000", self.ftp.scans)
        # Nothing new: nothing written. The tile is no installed copy, so Halo 3 keeps its game folder.
        self.ftp.stored.clear()
        out = self.push(extra=("--config", config))
        self.assertEqual([p for p in self.ftp.stored if "Content" in p or "NetDisc360/GOD" in p], [])
        self.assertTrue(os.path.isdir(os.path.join(home, "Halo 3")))
        # Back to folders: the package and the game folder in GOD go; the other game's copy stays.
        with open(config, "w") as f:
            f.write("tile_choices=1\ntile_choice1=127.0.0.1|folder\n")
        out = self.push(extra=("--config", config))
        self.assertEqual(sorted(os.listdir(self.games)), ["Halo 3", "Skate 3"])
        self.assertEqual(os.listdir(home), [])
        self.assertFalse(os.path.exists(packages))
        self.assertEqual(os.listdir(os.path.join(content, "41560817", "00007000")), ["AABBCCDD"])
        # None: no folder or package at all for that game.
        with open(config, "w") as f:
            f.write("tile_mode=god\ntile_choices=1\ntile_choice1=127.0.0.1||none:Skate 3.iso\n")
        out = self.push(extra=("--config", config))
        self.assertEqual(sorted(os.listdir(self.games)), [])
        self.assertEqual(sorted(os.listdir(home)), ["Halo 3"])
        self.assertFalse(os.path.exists(os.path.join(content, "454108E6")))

    def test_names_games_on_demand_tiles_as_aurora_and_xboxunity_do(self):
        # What the Xbox home shows: the header's name and icon.
        self.add_iso("Halo 3 (USA, Europe) (En,Ja,Fr).iso", 0x1A2B3C4D, 0x4D5307E6)
        self.add_iso("Skate 3 (USA).iso", 0x5C087C2C, 0x454108E6)
        self.add_iso("Fable II (Europe) (En,Fr).iso", 0x22223333, 0x4D5307F1)
        # A set's discs: each its own name (the ISO's tags say which already for the second).
        for name, media, disc in (("Mass Effect 2 (USA).iso", 0x31313131, 1), ("Mass Effect 2 (USA) (Disc 2).iso", 0x32323232, 2)):
            write_iso(os.path.join(self.isos, name), {"default.xex": xex(execution_info(media, 0x454108C2, disc, 2))})
        # Aurora's name wins (its folder tile's, not one of NetDisc360's
        # packages, whose name came from a header).
        databases = os.path.join(self.xbox, "Hdd1", "Apps", "Aurora", "Data", "Databases")
        os.makedirs(databases)
        db = sqlite3.connect(os.path.join(databases, "content.db"))
        db.execute("CREATE TABLE ContentItems ( Id INTEGER PRIMARY KEY AUTOINCREMENT, Directory TEXT COLLATE NOCASE, Executable TEXT, "
                   "TitleId INTEGER, TitleName TEXT, Description TEXT, ScanPathId INTEGER )")
        for directory, title, name in (("\\content\\0000000000000000\\4d5307e6\\00007000\\4e443301aabbccdd", 0x4D5307E6, "Halo 3 from a header"),
                                       ("\\netiso\\halo 3", 0x4D5307E6, "Halo 3: Édition épique"),
                                       ("\\content\\0000000000000000\\4d5307f1\\00007000\\4e44330122223333", 0x4D5307F1, "Fable II (Europe) (En,Fr)")):
            db.execute("INSERT INTO ContentItems (Directory, Executable, TitleId, TitleName, ScanPathId) VALUES (?, 'default.xex', ?, ?, 3)",
                       (directory, title, name))
        db.commit()
        db.close()
        icon = b"\x89PNG\r\n\x1a\n" + os.urandom(3000)
        unity = http.server.ThreadingHTTPServer(("127.0.0.1", 0), FakeUnityHandler)
        unity.asked, unity.titles, unity.files = [], {}, {}
        unity.names = {"4D5307E6": "Halo 3 (XboxUnity)", "454108E6": "skate. 3"}
        unity.icons = {"454108E6": icon, "4D5307E6": b"\x89PNG\r\n\x1a\n" + bytes(0x4000)}  # Halo 3's is too big for the header.
        threading.Thread(target=unity.serve_forever, daemon=True).start()
        config = os.path.join(self.tmp.name, "netdisc.ini")
        with open(config, "w") as f:
            f.write("tile_mode=god\n")
        try:
            address = ("--config", config, "--xboxunity", "http://127.0.0.1:%d/Resources/Lib" % unity.server_port)
            out = self.push(extra=address)
            content = os.path.join(self.xbox, "Hdd1", "Content", "0000000000000000")
            def header(title, media, disc=1):
                with open(os.path.join(content, title, "00007000", "4E4433%02X%s" % (disc, media)), "rb") as f:
                    return f.read()
            def name(h, at=0x411):
                return h[at:at + 0x80].decode("utf-16-be").split("\0")[0]
            be = lambda h, at: int.from_bytes(h[at:at + 4], "big")
            halo, skate, fable = header("4D5307E6", "1A2B3C4D"), header("454108E6", "5C087C2C"), header("4D5307F1", "22223333")
            # Every language's display name, and the title name.
            self.assertEqual([name(halo, 0x411 + k * 0x80) for k in range(18)] + [name(halo, 0x1691)], ["Halo 3: Édition épique"] * 19)
            self.assertEqual(name(skate), "skate. 3")
            # Neither Aurora (its name is the ISO's) nor XboxUnity knows Fable II: the ISO's name without its tags.
            self.assertEqual(name(fable), "Fable II")
            self.assertEqual([name(header("454108C2", m, d)) for m, d in (("31313131", 1), ("32323232", 2))], ["Mass Effect 2 (Disc 1)", "Mass Effect 2 (Disc 2)"])
            self.assertEqual((be(skate, 0x1712), be(skate, 0x1716)), (len(icon), len(icon)))
            self.assertEqual((skate[0x171A:0x171A + len(icon)], skate[0x571A:0x571A + len(icon)]), (icon, icon))
            self.assertEqual((be(halo, 0x1712), be(fable, 0x1712)), (0, 0))
            self.assertEqual(halo[0x171A:0x971A], bytes(0x8000))
            self.read_god_files(os.path.join(content, "454108E6", "00007000", "4E4433015C087C2C"))
            self.assertIn('named "skate. 3" with its icon', out)
            # XboxUnity is asked once a title while the server runs; its answer
            # is the same, so the next push writes nothing.
            self.ftp.stored.clear()
            self.push(extra=address)
            self.assertEqual([p for p in self.ftp.stored if "Content" in p], [])
        finally:
            unity.shutdown()
            unity.server_close()
        lists = [a for a in unity.asked if a.startswith("/Resources/Lib/TitleList.php")]
        self.assertEqual(len(lists), 8)  # 4 titles, 2 pushes (each its own server run).
        self.assertTrue(all("search=" in a for a in lists))

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
        self.assertEqual(self.ftp.scans, ["/Hdd1/NetDisc360/Games"])
        self.assertIn("asked Aurora to scan /Hdd1/NetDisc360/Games", out)
        # Nothing added or removed: no scan.
        self.push()
        self.assertEqual(len(self.ftp.scans), 1)
        os.remove(path)
        self.push()
        self.assertEqual(len(self.ftp.scans), 2)

    def run_server(self, *options, xbox="127.0.0.1", ftp_port=None, auto_profiles=False):
        port = free_port()
        server = subprocess.Popen([SERVER, self.isos, "--port", str(port), "--xbox", xbox, "--web-port", "0"] + ([] if auto_profiles else ["--no-auto-profiles"]) + [
                                   "--ftp-port", str(ftp_port or self.ftp.server_address[1]),
                                   "--launcher", self.launcher, "--boot", self.boot, "--xboxunity", ""] + list(options),
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

    def test_makes_a_game_folder_whose_parent_folder_is_missing(self):
        # Aurora's FTP makes one folder level per MKD, so /Hdd1/NetDisc360/Test
        # on a console without /Hdd1/NetDisc360 is made a level at a time.
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        out = self.push(extra=("--xbox-folder", "/Hdd1/NetDisc360/Test"))
        self.assertNotIn("couldn't list", out)
        self.assertTrue(os.path.isfile(os.path.join(self.xbox, "Hdd1", "NetDisc360", "Test", "Halo 3", "default.xex")))

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
        self.assertTrue(os.path.isfile(os.path.join(other, "Hdd1", "NetDisc360", "Games", "Halo 3", "default.xex")))

    def test_tells_the_launcher_how_to_wake_this_computer(self):
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        config = os.path.join(self.tmp.name, "netdisc.ini")
        wake = os.path.join(self.app, "wake.ini")
        # The console here is this computer itself, whose loopback adapter
        # has no MAC address; a MAC address given in the settings is used.
        with open(config, "w") as f:
            f.write("[Server]\nwake_mac=aa-bb-cc-dd-ee-0f\n")
        self.push(extra=("--config", config))
        with open(wake, "rb") as f:
            self.assertEqual(f.read(), b"mac=AA:BB:CC:DD:EE:0F\r\naddress=127.0.0.1\r\nbroadcast=127.0.0.255\r\n")
        # Waking off: the file goes.
        with open(config, "w") as f:
            f.write("[Server]\nwake=0\nwake_mac=AA:BB:CC:DD:EE:0F\n")
        self.add_iso("Gears of War 2.iso", 0x2B3C4D5E, 0x4D53082D)
        self.assertIn("doesn't wake this computer", self.push(extra=("--config", config, "-v")))
        self.assertFalse(os.path.exists(wake))

    def test_updates_netiso_sync_to_this_build(self):
        sync = os.path.join(self.games, "NetISO Sync")
        os.makedirs(sync)
        with open(os.path.join(sync, "default.xex"), "wb") as f:
            f.write(b"OLD SYNC")
        with open(os.path.join(sync, "netiso.ini"), "w") as f:
            f.write("after=Hdd1:\\Apps\\Aurora\\Aurora.xex\n")
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        out = self.push()
        self.assertIn("updated /Hdd1/NetDisc360/Games/NetISO Sync to this build.", out)
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
        with open(os.path.join(self.app, "boot.xex"), "rb") as f:
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
        self.assertEqual(sorted(os.listdir(self.games)), ["NetISO Sync", "a"])

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
        self.assertEqual(sorted(os.listdir(self.games)), ["a", "b"], result.stdout)
        os.rename(other, other + " (unplugged)")
        result = subprocess.run([SERVER, self.isos, other, "--port", str(free_port()), "--push-once", "--xbox", "127.0.0.1",
                                 "--web-port", "0", "--ftp-port", str(self.ftp.server_address[1]),
                                 "--launcher", self.launcher, "--boot", self.boot], capture_output=True, text=True, timeout=60)
        self.assertIn("Can't read these game folders", result.stdout)
        self.assertIn("they stay until every game folder can be read again", result.stdout)
        self.assertEqual(sorted(os.listdir(self.games)), ["a", "b"])

    def test_keeps_the_games_of_a_game_folder_that_empties(self):
        # A NAS share that isn't mounted shows as the empty folder under it:
        # its games only seem gone, so they stay in Aurora.
        self.add_iso("a.iso", 1, 0x11111111)
        other = os.path.join(self.tmp.name, "nas")
        os.makedirs(other)
        moved = os.path.join(self.tmp.name, "away.iso")
        path = self.add_iso("b.iso", 2, 0x22222222, other)
        self.web = free_port()
        server, _ = self.run_server(other, "--remove-wait", "2", "--web-port", str(self.web))
        try:
            self.assertTrue(self.wait_for(lambda: os.path.isdir(os.path.join(self.games, "b")), 15))
            os.rename(path, moved)
            state = self.state_when(lambda s: any(f["emptied"] for f in s["folders"]), 10)
            self.assertEqual([f["emptied"] for f in state["folders"]], [False, True])
            time.sleep(14)  # Past the settling and the removal wait.
            self.assertEqual(sorted(os.listdir(self.games)), ["a", "b"])
            self.assertIn("(it had games, and has none now)", json.dumps(self.json("GET", "/api/log?after=0")))
            # Back: nothing held any more.
            os.rename(moved, path)
            state = self.state_when(lambda s: not any(f["emptied"] for f in s["folders"]), 10)
            self.assertEqual([f["emptied"] for f in state["folders"]], [False, False])
        finally:
            server.kill()
            server.communicate()

    def test_counts_how_often_and_how_long_each_game_is_played(self):
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        self.add_iso("Skate 3.iso", 0x5C087C2C, 0x454108E6)
        config = os.path.join(self.tmp.name, "netdisc.ini")
        self.web = free_port()
        server, port = self.run_server("--web-port", str(self.web), "--config", config)
        try:
            self.assertTrue(self.wait_for(lambda: self.ftp.scans, 15))
            client = Client(port)
            # A mount alone isn't a play: NetISO mounts its last disc as the console starts.
            self.assertEqual(client.mount("Skate 3.iso"), 1)
            client.read(0, 2048)
            self.assertEqual(client.mount("Halo 3.iso"), 1)
            for i in range(17):
                client.read(i << 20, 1 << 20)
            games = {}
            self.assertTrue(self.wait_for(lambda: games.update({g["name"]: g for g in self.json("GET", "/api/games")["games"]}) or games.get("Halo 3", {}).get("plays") == 1, 5))
            self.assertNotIn("plays", games["Skate 3"])
            self.assertGreater(games["Halo 3"]["lastPlayed"], time.time() - 60)
            with open(os.path.join(self.tmp.name, "plays.ini")) as f:
                self.assertRegex(f.read(), r"^1\|\d+\|\d+\|halo 3\n$")
            client.close()
        finally:
            server.kill()
            server.communicate()
        # Kept for the next start.
        server, port = self.run_server("--web-port", str(self.web), "--config", config)
        try:
            games = {g["name"]: g for g in self.json("GET", "/api/games")["games"]}
            self.assertEqual(games["Halo 3"]["plays"], 1)
        finally:
            server.kill()
            server.communicate()

    def test_console_names_and_identify(self):
        # Each console's own name, from its media sharing, on the dashboard
        # and in the settings file; shown on its screen when Aurora starts and
        # with Identify (through JRPC2 on its debug monitor).
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        xbdm, sharing = Xbdm(), MediaSharing(name="FABLE &amp; Co")
        self.web = free_port()
        config = os.path.join(self.tmp.name, "names.ini")
        server, _ = self.run_server("--web-port", str(self.web), "--config", config, "--xbdm-port", str(xbdm.port),
                                    "--name-port", str(sharing.port))
        try:
            self.assertTrue(self.wait_for(lambda: self.ftp.scans, 15))
            def console():
                return self.json("GET", "/api/state")["consoles"][0]
            self.assertTrue(self.wait_for(lambda: console()["name"] == "FABLE & Co", 15))
            self.assertEqual(console()["id"], "46687478-4105-2000-0000-0022484be1c5")
            # Aurora started (its FTP server answered): its name on its screen.
            self.assertTrue(self.wait_for(lambda: xbdm.notes, 10))
            self.assertEqual(xbdm.notes, ["This is FABLE & Co (127.0.0.1)"])
            self.json("POST", "/api/console/identify", {"address": "127.0.0.1"})
            self.assertEqual(len(xbdm.notes), 2)
            self.assertEqual(xbdm.commands, [])  # Nothing else was sent.
            # Without JRPC2 it says so.
            xbdm.jrpc = False
            status, _, data = self.call("POST", "/api/console/identify", {"address": "127.0.0.1"})
            self.assertEqual(status, 400)
            self.assertIn(b"JRPC2", data)
            # Kept for the next start, when the console may be off.
            def saved():
                with open(config) as f:
                    return f.read()
            self.assertTrue(self.wait_for(lambda: "|FABLE & Co\n" in saved(), 30))
            self.assertIn("console1=127.0.0.1|", saved())
            self.assertFalse(self.json("GET", "/api/state")["settings"]["showName"] is False)
            self.json("POST", "/api/settings", {"showName": False})
            self.assertFalse(self.json("GET", "/api/state")["settings"]["showName"])
            # Tidying Aurora's list as it starts: off until chosen.
            self.assertIs(self.json("GET", "/api/state")["settings"]["tidyAurora"], False)
            self.json("POST", "/api/settings", {"tidyAurora": True})
            self.assertIs(self.json("GET", "/api/state")["settings"]["tidyAurora"], True)
            self.assertIn("tidy_aurora=1\n", saved())
        finally:
            server.kill()
            server.communicate()
            xbdm.close()
            sharing.close()

    def test_a_media_player_that_isn_t_a_360_gives_no_name(self):
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        sharing = MediaSharing(model="Some TV")
        self.web = free_port()
        server, _ = self.run_server("--web-port", str(self.web), "--name-port", str(sharing.port))
        try:
            self.assertTrue(self.wait_for(lambda: self.ftp.scans, 15))
            self.assertTrue(self.wait_for(lambda: sharing.asked, 15))
            time.sleep(0.5)
            self.assertEqual(self.json("GET", "/api/state")["consoles"][0]["name"], "")
        finally:
            server.kill()
            server.communicate()
            sharing.close()

    def test_sets_a_console_up_with_the_plugin_set(self):
        # launch.ini's plugin lines become the set; files the console lacks
        # come from the kit folder, settings files end up at the drive's top
        # and beside the plugin; launch.ini is backed up first, and put back.
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        usb = os.path.join(self.xbox, "Usb0")
        plugins = os.path.join(usb, "Plugins")
        os.makedirs(plugins)
        before = ("[Paths]\r\nDefault = Hdd:\\Apps\\Aurora\\Aurora.xex\r\n\r\n[Plugins]\r\nplugin1 = Usb:\\Plugins\\NetISO.xex\r\n"
                  "plugin2 = Usb:\\Plugins\\NetIsoTiles.xex\r\n; plugin3 = Usb:\\Plugins\\Old.xex\r\nplugin3 = Usb:\\Plugins\\UsbdSecPatch.xex\r\n"
                  "\r\n[Settings]\r\npingpatch = true\r\n")
        files = {os.path.join(usb, "launch.ini"): before, os.path.join(plugins, "NetISO.xex"): "netiso", os.path.join(plugins, "NetIsoTiles.xex"): "tiles",
                 os.path.join(plugins, "UsbdSecPatch.xex"): "patch", os.path.join(usb, "NetIsoTiles.ini"): "rcvbuf=65536\r\n",
                 os.path.join(plugins, "NetISO.xex.txt"): "10.0.0.5\r\n"}
        kit = os.path.join(self.tmp.name, "kit")
        os.makedirs(kit)
        for name, data in (("xbdm.xex", "xbdm"), ("JRPC2.xex", "jrpc2"), ("AutoDNS.xex", "autodns"), ("xbdm.ini", "dbgname=FABLE\r\n")):
            files[os.path.join(kit, name)] = data
        for path, data in files.items():
            with open(path, "w", newline="") as f:
                f.write(data)
        def read(*parts):
            with open(os.path.join(usb, *parts), newline="") as f:
                return f.read()
        self.web = free_port()
        config = os.path.join(self.tmp.name, "setup.ini")
        server, _ = self.run_server("--web-port", str(self.web), "--config", config)
        try:
            self.assertTrue(self.wait_for(lambda: self.ftp.scans, 15))
            status, _, data = self.call("POST", "/api/settings", {"pluginKit": os.path.join(self.tmp.name, "nowhere")})
            self.assertEqual(status, 400)
            self.json("POST", "/api/settings", {"pluginKit": kit})
            # What it has.
            r = self.json("GET", "/api/console/setup?address=127.0.0.1")
            self.assertEqual(r["launchIni"], "/Usb0/launch.ini")
            self.assertEqual([(p["slot"], p["name"], p["found"]) for p in r["plugins"]],
                             [(1, "NetISO.xex", True), (2, "NetIsoTiles.xex", True), (3, "UsbdSecPatch.xex", True)])
            self.assertEqual(r["set"], ["NetISO.xex", "NetIsoTiles.xex", "xbdm.xex", "AutoDNS.xex", "JRPC2.xex"])
            self.assertEqual(sorted(r["kitFiles"]), ["AutoDNS.xex", "JRPC2.xex", "xbdm.xex"])
            self.assertEqual(r["backups"], [])
            # What it would do: nothing written yet.
            want = {"address": "127.0.0.1", "plugins": r["set"]}
            plan = self.json("POST", "/api/console/setup", want)
            self.assertEqual([x["from"] for x in plan["slots"]], ["console", "console", "kit", "kit", "kit"])
            self.assertIn("[Plugins]\r\nplugin1 = Usb:\\Plugins\\NetISO.xex\r\nplugin2 = Usb:\\Plugins\\NetIsoTiles.xex\r\n"
                          "plugin3 = Usb:\\Plugins\\xbdm.xex\r\nplugin4 = Usb:\\Plugins\\AutoDNS.xex\r\nplugin5 = Usb:\\Plugins\\JRPC2.xex\r\n"
                          "; plugin3 = Usb:\\Plugins\\Old.xex\r\n\r\n[Settings]\r\npingpatch = true\r\n", plan["text"])
            self.assertTrue(plan["text"].startswith("[Paths]\r\nDefault = Hdd:\\Apps\\Aurora\\Aurora.xex\r\n"))
            self.assertIn("Usb:\\Plugins\\NetIsoTiles.ini, a copy of Usb:\\NetIsoTiles.ini", plan["copies"])
            self.assertIn("Usb:\\NetISO.xex.txt, a copy of Usb:\\Plugins\\NetISO.xex.txt", plan["copies"])
            self.assertIn("Usb:\\xbdm.ini, from " + kit, plan["copies"])
            self.assertIn("Usb:\\Plugins\\JRPC2.xex, from " + kit, plan["copies"])
            self.assertEqual(read("launch.ini"), before)
            self.assertFalse(os.path.exists(os.path.join(plugins, "xbdm.xex")))
            # Rules: 5 at most, each from the console or the kit.
            for plugins_asked, code in ((r["set"] + ["UsbdSecPatch.xex"], 400), (["NetISO.xex", "Nope.xex"], 400), (["NetISO.xex", "NetISO.xex"], 400)):
                self.assertEqual(self.call("POST", "/api/console/setup", {"address": "127.0.0.1", "plugins": plugins_asked})[0], code)
            self.assertEqual(self.call("POST", "/api/console/setup", dict(want, apply=True, base="0" * 64))[0], 409)
            # Done: backed up on the console and here, files copied, launch.ini as previewed.
            done = self.json("POST", "/api/console/setup", dict(want, apply=True, base=plan["base"]))
            self.assertTrue(done["applied"])
            self.assertEqual(read("launch.ini"), plan["text"])
            self.assertRegex(done["backup"], r"^launch\.ini\.netdisc-\d{8}-\d{6}\.bak$")
            self.assertEqual(read(done["backup"]), before)
            with open(os.path.join(self.tmp.name, "Backups", "Consoles", "127.0.0.1", done["backup"]), newline="") as f:
                self.assertEqual(f.read(), before)
            self.assertEqual(read("Plugins", "xbdm.xex"), "xbdm")
            self.assertEqual(read("Plugins", "JRPC2.xex"), "jrpc2")
            self.assertEqual(read("xbdm.ini"), read("Plugins", "xbdm.ini"))
            self.assertEqual(read("Plugins", "NetIsoTiles.ini"), "rcvbuf=65536\r\n")
            self.assertEqual(read("NetISO.xex.txt"), "10.0.0.5\r\n")
            self.assertEqual(read("Plugins", "UsbdSecPatch.xex"), "patch")  # Left out of launch.ini, not deleted.
            # Again: nothing more to copy.
            again = self.json("POST", "/api/console/setup", want)
            self.assertEqual(again["copies"], [])
            self.assertEqual(again["text"], plan["text"])
            # Put back.
            time.sleep(1.1)  # Backups are named by the second.
            self.assertEqual(self.call("POST", "/api/console/setup/restore", {"address": "127.0.0.1", "backup": "../launch.ini"})[0], 400)
            back = self.json("POST", "/api/console/setup/restore", {"address": "127.0.0.1", "backup": done["backup"]})
            self.assertEqual(read("launch.ini"), before)
            self.assertEqual(read(back["backup"]), plan["text"])
            self.assertEqual(self.json("GET", "/api/console/setup?address=127.0.0.1")["backups"], [back["backup"], done["backup"]])
        finally:
            server.kill()
            server.communicate()

    def test_console_tools_on_the_dashboard(self):
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        aurora, xbdm = AuroraWeb(), Xbdm()
        self.web = free_port()
        server, _ = self.run_server("--web-port", str(self.web), "--aurora-web-port", str(aurora.port), "--xbdm-port", str(xbdm.port))
        try:
            self.assertTrue(self.wait_for(lambda: self.ftp.scans, 15))
            for old in (False, True):
                aurora.old = aurora.basic = old
                # What Aurora says about the console.
                status = self.json("GET", "/api/console/status?address=127.0.0.1")
                self.assertEqual(status["temperature"]["cpu"], 51.25)
                self.assertEqual(status["profiles"][0]["gamertag"], "Player One")
                self.assertEqual(status["title"]["path"], AURORA_XEX)
                # Its screenshots: listed, one as an image, taking one, deleting one.
                shots = self.json("GET", "/api/console/screenshots?address=127.0.0.1")
                self.assertEqual([x["filename"] for x in shots], aurora.shots)
                status, response, data = self.call("GET", "/api/console/screenshot?address=127.0.0.1&id=" + aurora.shots[0])
                self.assertEqual((status, response.getheader("Content-Type"), data), (200, "image/bmp", SHOT))
                self.json("POST", "/api/console/screenshot/take", {"address": "127.0.0.1"})
                self.assertEqual(len(self.json("GET", "/api/console/screenshots?address=127.0.0.1")), 2)
                self.json("POST", "/api/console/screenshot/delete", {"address": "127.0.0.1", "id": aurora.shots[-1]})
                self.assertEqual(len(aurora.shots), 1)
                # Aurora started again.
                aurora.launched.clear()
                self.json("POST", "/api/console/power", {"address": "127.0.0.1", "action": "aurora"})
                self.assertEqual(aurora.launched, [("\\Device\\Harddisk0\\Partition1\\Apps\\Aurora", "Aurora.xex", "0")])
            # Never while NetDisc360 is copying to it over FTP.
            self.ftp.gate.clear()
            try:
                self.add_iso("Skate 3.iso", 0x22222222, 0x45410950)
                # Written once it has settled (10 s).
                def uploading():
                    log = list(self.ftp.log)
                    into = [i for i, l in enumerate(log) if l == "CWD /Hdd1/NetDisc360/Games/Skate 3"]
                    return into and any(l.startswith("STOR") for l in log[into[0]:])
                self.assertTrue(self.wait_for(uploading, 30))
                aurora.launched.clear()
                status, _, data = self.call("POST", "/api/console/power", {"address": "127.0.0.1", "action": "aurora"})
                self.assertEqual(status, 400)
                self.assertIn(b"Not now: NetDisc360 is still working on it over FTP", data)
                self.assertEqual(aurora.launched, [])
            finally:
                self.ftp.gate.set()
            # Then it can (wait_for asks once more when it's true).
            self.assertTrue(self.wait_for(lambda: aurora.launched or self.call("POST", "/api/console/power", {"address": "127.0.0.1", "action": "aurora"})[0] == 200, 20))
            self.assertEqual(len(aurora.launched), 1, aurora.launched)
            # Turned off through its debug monitor.
            self.json("POST", "/api/console/power", {"address": "127.0.0.1", "action": "off"})
            self.assertEqual(xbdm.commands, ["shutdown"])
            # Restarted the same way (a cold start: DashLaunch loads its plugins again).
            self.json("POST", "/api/console/power", {"address": "127.0.0.1", "action": "restart"})
            self.assertEqual(xbdm.commands, ["shutdown", "magicboot COLD"])
            self.assertEqual(self.call("POST", "/api/console/power", {"address": "127.0.0.1", "action": "reboot"})[0], 400)
            xbdm.close()
            status, _, data = self.call("POST", "/api/console/power", {"address": "127.0.0.1", "action": "off"})
            self.assertEqual(status, 400)
            self.assertIn(b"debug monitor (xbdm, port", data)
            # A game's netiso.log from its folder.
            log = self.json("GET", "/api/console/gamelog?address=127.0.0.1&game=Halo%203")
            self.assertEqual((log["missing"], log["folder"]), (True, "/Hdd1/NetDisc360/Games/Halo 3"))
            with open(os.path.join(self.games, "Halo 3", "netiso.log"), "w") as f:
                f.write("==== 2026-10-26\nmounted Halo 3.iso\n")
            self.assertIn("mounted Halo 3.iso", self.json("GET", "/api/console/gamelog?address=127.0.0.1&game=Halo%203")["text"])
            status, _, data = self.call("GET", "/api/console/gamelog?address=127.0.0.1&game=Gears")
            self.assertEqual(status, 400)
            self.assertIn(b"no folder for it", data)
            # A console that never used the server.
            self.assertEqual(self.call("GET", "/api/console/status?address=127.0.0.7")[0], 400)
        finally:
            aurora.close()
            server.kill()
            server.communicate()

    def test_tells_an_incomplete_iso(self):
        # Its directory says a file goes on past the end of the ISO: a
        # download or copy that stopped.
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        path = os.path.join(self.isos, "Skate 3.iso")
        write_iso(path, {"default.xex": xex(execution_info(0x5C087C2C, 0x454108E6)), "data.bin": bytes(range(256)) * 256})
        os.truncate(path, os.path.getsize(path) - 2048 - 1000)
        self.web = free_port()
        server, _ = self.run_server("--web-port", str(self.web))
        try:
            games = {g["name"]: g for g in self.json("GET", "/api/games")["games"]}
            self.assertEqual((games["Skate 3"]["incomplete"], games["Halo 3"]["incomplete"]), (True, False))
            log = json.dumps(self.json("GET", "/api/log?after=0"))
            self.assertIn("Skate 3.iso is shorter than the files on it say", log)
            self.assertNotIn("Halo 3.iso is shorter", log)
            self.assertIn("Skate 3 is incomplete; copy it again", json.dumps(self.json("GET", "/api/state")["events"]))
        finally:
            server.kill()
            server.communicate()

    def test_turns_a_console_off_after_its_last_game(self):
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        config = os.path.join(self.tmp.name, "netdisc.ini")
        with open(config, "w") as f:
            f.write("[Server]\nauto_off_minutes=1\n")
        xbdm = Xbdm()
        self.web = free_port()
        server, port = self.run_server("--web-port", str(self.web), "--config", config, "--xbdm-port", str(xbdm.port))
        try:
            self.assertTrue(self.wait_for(lambda: self.ftp.scans, 15))
            self.assertEqual(self.json("GET", "/api/state")["settings"]["autoOffMinutes"], 1)
            client = Client(port)
            self.assertEqual(client.mount("Halo 3.iso"), 1)
            client.read(0, 1 << 20)
            # Back in Aurora (its FTP server answers 5 s after the mount), then a minute.
            self.assertTrue(self.wait_for(lambda: "back in Aurora after Halo 3" in json.dumps(self.json("GET", "/api/log?after=0")), 15))
            self.assertEqual(xbdm.commands, [])
            self.assertTrue(self.wait_for(lambda: xbdm.commands, 70))
            self.assertEqual(xbdm.commands, ["shutdown"])
            self.assertTrue(self.wait_for(lambda: "turned off, 1 minute after its last game" in json.dumps(self.json("GET", "/api/log?after=0")), 5))
            client.close()
        finally:
            xbdm.close()
            server.kill()
            server.communicate()

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
            self.assertEqual(sorted(os.listdir(self.games)), ["a", "b"])
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
            self.assertEqual(sorted(os.listdir(self.games)), ["a", "b"])
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
        self.assertIn("added the NetDisc360 games to Aurora's Xbox 360 category", out)
        script = os.path.join(self.aurora, "FilterFunctions.lua")
        with open(script, "rb") as f:
            edited = f.read()
        with open(script + ".before-netiso", "rb") as f:
            self.assertEqual(f.read(), FILTER)
        self.assertIn(b"or GameListFilterNetIso(Content)\r\nend\r\n", edited)
        self.assertNotIn(b"\n", edited.replace(b"\r\n", b""))  # Still CRLF throughout.
        self.assertTrue(edited.endswith(FILTER[FILTER.index(b"\r\nfunction GameListFilterXBLA"):]))
        out = self.push()
        self.assertIn("Xbox 360 category includes the NetDisc360 games", out)
        with open(script, "rb") as f:
            self.assertEqual(f.read(), edited)
        if LUA:
            check = os.path.join(self.tmp.name, "check.lua")
            with open(check, "w") as f:
                f.write("ContentGroup = { Xbox360 = 1, XBLA = 2 }\n"
                        "dofile(%r)\n"
                        "local function t(group, dir) return GameListFilterXbox360({ Group = group, Directory = dir }) end\n"
                        "assert(t(5, '\\\\netdisc360\\\\games\\\\skate 3 (usa europe)'))\n"
                        "assert(t(5, 'netdisc360\\\\games\\\\halo 3'))\n"
                        "assert(t(5, '\\\\netiso\\\\skate 3 (usa europe)'))\n"  # Not moved yet.
                        "assert(not t(5, '\\\\netiso\\\\netiso sync'))\n"
                        "assert(not t(5, '\\\\netdisc360\\\\games\\\\netiso sync'))\n"
                        "assert(not t(5, '\\\\netdisc360\\\\other'))\n"
                        "assert(not t(5, '\\\\apps\\\\aurora'))\n"
                        "assert(t(1, '\\\\content\\\\0000000000000000\\\\4d5307e6'))\n"
                        "print('lua ok')\n" % script)
            result = subprocess.run([LUA, check], capture_output=True, text=True)
            self.assertEqual(result.stdout.strip(), "lua ok", result.stderr)

    def test_fills_in_aurora_s_empty_art_for_netdisc360_and_fastftp360(self):
        # Aurora's placeholders (2048 bytes) get the art from the aurora
        # folder beside the launcher; art that is there already stays.
        art = os.path.join(self.tmp.name, "aurora")
        for app, title in (("netdisc360", "FFFF4E49"), ("fastftp360", "FFFF4646"), ("netdiscparty", "FFFF4E50")):
            os.makedirs(os.path.join(art, app))
            for kind in ("GC", "BK", "GL"):
                with open(os.path.join(art, app, "%s%s.asset" % (kind, title)), "wb") as f:
                    f.write(("%s %s ART" % (app, kind)).encode() * 1000)
        game_data = os.path.join(self.xbox, "Hdd1", "Apps", "Aurora", "Data", "GameData")
        placeholder = b"RXEA" + b"\0" * 2044
        folders = {"FFFF4E49_00000048": "FFFF4E49", "FFFF4646_0000005B": "FFFF4646", "FFFF4E50_00000071": "FFFF4E50", "4D5307E6_00000006": "4D5307E6"}
        for folder, title in folders.items():
            os.makedirs(os.path.join(game_data, folder))
            for kind in ("GC", "BK", "GL"):
                with open(os.path.join(game_data, folder, "%s%s.asset" % (kind, title)), "wb") as f:
                    f.write(placeholder)
        own = os.path.join(game_data, "FFFF4646_0000005B", "GCFFFF4646.asset")
        with open(own, "wb") as f:
            f.write(b"SOMEONE'S OWN COVER" * 200)
        self.add_iso("a.iso", 1, 0x11111111)
        out = self.push()
        self.assertIn("wrote NetDisc360's cover art to Aurora", out)
        self.assertIn("wrote FastFTP360's cover art to Aurora", out)
        self.assertIn("wrote NetDisc360 Party's cover art to Aurora", out)

        def read(folder, name):
            with open(os.path.join(game_data, folder, name), "rb") as f:
                return f.read()
        self.assertEqual(read("FFFF4E49_00000048", "GCFFFF4E49.asset"), b"netdisc360 GC ART" * 1000)
        self.assertEqual(read("FFFF4E49_00000048", "GLFFFF4E49.asset"), b"netdisc360 GL ART" * 1000)
        self.assertEqual(read("FFFF4646_0000005B", "BKFFFF4646.asset"), b"fastftp360 BK ART" * 1000)
        self.assertEqual(read("FFFF4E50_00000071", "GCFFFF4E50.asset"), b"netdiscparty GC ART" * 1000)
        self.assertEqual(read("FFFF4646_0000005B", "GCFFFF4646.asset"), b"SOMEONE'S OWN COVER" * 200)
        self.assertEqual(read("4D5307E6_00000006", "GC4D5307E6.asset"), placeholder)  # Other titles: Aurora's.
        out = self.push()
        self.assertNotIn("cover art to Aurora", out)

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
        self.assertIn("updated NetDisc360's rule in Aurora's Xbox 360 category", out)
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
        self.assertIn("Xbox 360 category includes the NetDisc360 games", out)
        self.assertEqual([p for p in self.ftp.stored if "Scripts" in p], [])
        if LUA:
            check = os.path.join(self.tmp.name, "check.lua")
            with open(check, "w") as f:
                f.write("ContentGroup = { Xbox360 = 1, XBLA = 2 }\n"
                        "GameListFilterCategories = { User = {} }\n"
                        "dofile(%r)\n"
                        "dofile(%r)\n"
                        "local hide = GameListFilterCategories.User['Hide install discs']\n"
                        "local install = { Group = 5, Directory = '\\\\netdisc360\\\\games\\\\gta v - disc 2', TitleId = 0x545408A7, MediaId = 0x8C48794E }\n"
                        "local play = { Group = 5, Directory = '\\\\netdisc360\\\\games\\\\gta v', TitleId = 0x545408A7, MediaId = 0x79E465C5 }\n"
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
        self.assertEqual(sorted(os.listdir(self.games)), ["Halo 3"])

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
        self.assertEqual([p for p in self.ftp.stored if "NetDisc360/Games" in p], [])
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
        self.assertEqual([p for p in self.ftp.stored if "NetDisc360/Games" in p], [])
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

    def test_original_xbox_games_get_their_emulator(self):
        def game(name, title_id):
            folder = os.path.join(self.isos, name)
            os.makedirs(folder)
            with open(os.path.join(folder, "default.xbe"), "wb") as f:
                f.write(xbe(title_id))
        game("Crazy Taxi 3", 0x49470023)  # Freezes on xefu.xex: xefu7 from the list.
        # A config in the Hacked Xefu Pack; a full disc image in redump's format.
        write_iso(os.path.join(self.isos, "Gunvalkyrie.iso"), {"default.xbe": xbe(0x5345000B)}, partition=(0x18310000, 0x18300000))
        game("Halo", 0x4D530004)          # The console's own pick is right.
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        config = os.path.join(self.tmp.name, "netdisc.ini")
        out = self.push(extra=("--config", config))
        self.assertIn("4 added", out)

        def ini(folder):
            with open(os.path.join(self.games, folder, "netiso.ini")) as f:
                return f.read()
        self.assertIn("xbe=1\nxefu=xefu7\n", ini("Crazy Taxi 3"))
        self.assertIn("xbe=1\nxefu=config_loader_xefu7\n", ini("Gunvalkyrie"))
        self.assertNotIn("xefu", ini("Halo"))
        self.assertNotIn("xefu", ini("Halo 3"))
        # Nothing new: nothing written.
        self.ftp.stored.clear()
        self.push(extra=("--config", config))
        self.assertEqual([p for p in self.ftp.stored if "NetDisc360/Games" in p], [])
        # discs.ini has the last word, also on a game the list leaves alone;
        # xefu= with nothing is the console's pick.
        path = os.path.join(self.tmp.name, "discs.ini")
        with open(path, "w", newline="") as f:
            f.write("[49470023]\r\nxefu=XEFU2021C\r\n[5345000B]\r\nxefu=\r\n[4D530004]\r\nname=Halo\r\nxefu=xefu7b\r\n"
                    "[4D5307E6]\r\nxefu=xefu7\r\n[12345678]\r\nxefu=xefutitle\r\n")
        out = self.push(extra=("--config", config))
        self.assertIn('"xefutitle" isn\'t an emulator\'s name', out)
        self.assertIn("Crazy Taxi 3 starts in the xefu2021c emulator; its netiso.ini says so.", out)
        self.assertIn("Gunvalkyrie starts in the emulator the console picks; took xefu out of its netiso.ini.", out)
        self.assertIn("xefu=xefu2021c\n", ini("Crazy Taxi 3"))
        self.assertIn("iso=Crazy Taxi 3.iso\n", ini("Crazy Taxi 3"))
        self.assertIn("xbe=1\n", ini("Crazy Taxi 3"))
        self.assertNotIn("xefu", ini("Gunvalkyrie"))
        self.assertIn("xefu=xefu7b\n", ini("Halo"))
        self.assertNotIn("xefu", ini("Halo 3"))  # An Xbox 360 game has no emulator.

    def test_the_built_in_emulator_list_is_sound(self):
        with open(os.path.join(ROOT, "server", "xefu.cpp")) as f:
            source = f.read()
        table = source[source.index("static const KnownXefu Known[]={"):]
        table = table[:table.index("\n};")]
        rows = re.findall(r'\{0x([0-9A-F]{8}),"([a-z0-9_]+)","([^"]+)","([^"]+)"\},', table)
        self.assertGreater(len(rows), 500)
        self.assertEqual(len(rows), table.count("{0x"))
        ids = [int(title_id, 16) for title_id, _, _, _ in rows]
        self.assertEqual(ids, sorted(set(ids)), "the list is looked up by halves, so sorted, each Title ID once")
        versions = {"xefu2", "xefu3", "xefu5", "xefu1_1", "xefu6", "xefu7", "xefu7b", "xefu2019",
                    "xefu2021a", "xefu2021b", "xefu2021c", "config_loader_xefu7"}
        for title_id, version, name, why in rows:
            self.assertIn(version, versions, name)  # xefu.xex is what the console starts anyway.
        picks = {title_id: version for title_id, version, _, _ in rows}
        self.assertEqual(picks["49470023"], "xefu7")  # Crazy Taxi 3
        self.assertEqual(picks["54540086"], "xefu2021c")  # GTA San Andreas
        self.assertNotIn("4D530004", picks)  # Halo
        self.assertNotIn("4541005B", picks)  # Burnout 3

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

    def install(self, folder, media_id, title_id, xbox=None):
        """A game installed on the console (or the one whose files are in
        xbox): its default.xex in folder (under Hdd1)."""
        path = os.path.join(xbox or self.xbox, "Hdd1", *folder.split("/"))
        os.makedirs(path)
        with open(os.path.join(path, "default.xex"), "wb") as f:
            f.write(xex(execution_info(media_id, title_id)))

    def test_leaves_out_games_installed_on_the_console(self):
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        self.add_iso("Skate 3.iso", 0x5C087C2C, 0x454108E6)
        self.add_iso("Gears.iso", 0x11111111, 0x4D5307D5)
        out = self.push()
        self.assertIn("3 added", out)
        # Halo 3 is installed in Hdd1:\Games (two folders down), Skate 3 as
        # Games on Demand; Gears has only a save there.
        self.install("Games/Shooters/Halo 3", 0x1A2B3C4D, 0x4D5307E6)
        content = os.path.join(self.xbox, "Hdd1", "Content", "0000000000000000")
        os.makedirs(os.path.join(content, "454108E6", "00007000", "0123456789ABCDEF0123.data"))
        with open(os.path.join(content, "454108E6", "00007000", "0123456789ABCDEF0123"), "wb") as f:
            f.write(god_header(0x5C087C2C, 0x454108E6))
        os.makedirs(os.path.join(content, "4D5307D5", "00000001"))
        # Hdd1 itself is looked into too, but not the game folders in it,
        # whose launchers have the games' IDs.
        out = self.push(extra=["--installed", "/Hdd1/Games;/Hdd1"])
        self.assertIn("2 games are installed on it (in /Hdd1/Games;/Hdd1, or as Games on Demand)", out)
        self.assertIn("2 games installed on it get no game folder", out)
        self.assertIn("2 removed", out)
        self.assertEqual(sorted(os.listdir(self.games)), ["Gears"])
        # Only the package's header is read, and only its first bytes.
        self.assertNotIn("RETR 0123456789ABCDEF0123.data", self.ftp.log)
        # Without it, they're back, and their launchers offer the installed copy.
        out = self.push(extra=["--no-installed", "--installed", "/Hdd1/Games;/Hdd1"])
        self.assertIn("2 added", out)
        self.assertEqual(sorted(os.listdir(self.games)), ["Gears", "Halo 3", "Skate 3"])
        def local_copy(game):
            with open(os.path.join(self.games, game, "netiso.ini"), encoding="latin-1") as f:
                return [line[len("local_copy="):] for line in f.read().splitlines() if line.startswith("local_copy=")]
        self.assertEqual(local_copy("Halo 3"), ["Hdd1:\\Games\\Shooters\\Halo 3\\default.xex"])
        self.assertEqual(local_copy("Skate 3"), ["Hdd1:\\Content\\0000000000000000\\454108E6\\00007000\\0123456789ABCDEF0123"])
        self.assertEqual(local_copy("Gears"), [])
        # An installed copy that goes is taken out again, the rest of the file kept.
        shutil.rmtree(os.path.join(self.xbox, "Hdd1", "Games"))
        out = self.push(extra=["--no-installed", "--installed", "/Hdd1/Games;/Hdd1"])
        self.assertIn("Halo 3 is no longer installed on the console; took local_copy out of its netiso.ini.", out)
        self.assertEqual(local_copy("Halo 3"), [])
        with open(os.path.join(self.games, "Halo 3", "netiso.ini"), encoding="latin-1") as f:
            self.assertIn("iso=Halo 3.iso", f.read())
        self.assertEqual(len(local_copy("Skate 3")), 1)

    def test_leaves_out_original_xbox_games_installed_on_the_console(self):
        # An original Xbox game is a folder with default.xbe, its Title ID in
        # the certificate, there as on the PC; a 360 game beside it stays.
        def xbe(title_id, cert_at=0x180):
            base = 0x10000
            data = bytearray(cert_at + 0x20)
            data[0:4] = b"XBEH"
            struct.pack_into("<I", data, 0x104, base)
            struct.pack_into("<I", data, 0x108, len(data))
            struct.pack_into("<I", data, 0x118, base + cert_at)
            struct.pack_into("<I", data, cert_at + 8, title_id)
            return bytes(data)
        for name in ("Halo CE", "Burnout 3"):
            os.makedirs(os.path.join(self.isos, name))
        with open(os.path.join(self.isos, "Halo CE", "default.xbe"), "wb") as f:
            f.write(xbe(0x4D530004))
        with open(os.path.join(self.isos, "Burnout 3", "default.xbe"), "wb") as f:
            f.write(xbe(0x4541000D))
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        out = self.push()
        self.assertIn("3 added", out)
        # Halo CE is installed; its certificate lies past the first 64 KB
        # read, so the rest of its headers is read too.
        folder = os.path.join(self.xbox, "Hdd1", "Games", "Halo CE")
        os.makedirs(folder)
        with open(os.path.join(folder, "default.xbe"), "wb") as f:
            f.write(xbe(0x4D530004, cert_at=0x10400))
        out = self.push(extra=["--installed", "/Hdd1/Games"])
        self.assertIn("1 game is installed on it", out)
        self.assertIn("1 removed", out)
        self.assertEqual(sorted(os.listdir(self.games)), ["Burnout 3", "Halo 3"])

    def test_a_game_deleted_from_the_console_gets_its_folder_back(self):
        # Installed games are looked at again while the server runs, though
        # nothing changed on the PC: one deleted in Aurora gets its tile.
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        self.add_iso("Skate 3.iso", 0x5C087C2C, 0x454108E6)
        self.install("Games/Halo 3", 0x1A2B3C4D, 0x4D5307E6)
        server, _ = self.run_server("--installed", "/Hdd1/Games", "--installed-every", "1")
        try:
            self.assertTrue(self.wait_for(lambda: self.ftp.scans, 15))
            self.assertEqual(sorted(os.listdir(self.games)), ["Skate 3"])
            shutil.rmtree(os.path.join(self.xbox, "Hdd1", "Games", "Halo 3"))
            self.assertTrue(self.wait_for(lambda: os.path.isdir(os.path.join(self.games, "Halo 3")), 15))
            self.assertTrue(self.wait_for(lambda: len(self.ftp.scans) == 2, 10))
        finally:
            server.kill()
            out = server.stdout.read()
        self.assertIn("1 game is installed on it", out)
        self.assertIn("0 games are installed on it", out)

    def test_installed_games_are_looked_at_when_aurora_starts(self):
        # Aurora's NetISO plugin connecting (Aurora started again) is when
        # they're looked at, long before the slow look now and then.
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        self.install("Games/Halo 3", 0x1A2B3C4D, 0x4D5307E6)
        server, port = self.run_server("--installed", "/Hdd1/Games", "--installed-every", "1000")
        try:
            self.assertTrue(self.wait_for(lambda: self.ftp.scans or os.path.isfile(os.path.join(self.app, "boot.xex")), 15))
            time.sleep(2)
            self.assertFalse(os.path.isdir(os.path.join(self.games, "Halo 3")))
            shutil.rmtree(os.path.join(self.xbox, "Hdd1", "Games", "Halo 3"))
            time.sleep(3)
            self.assertFalse(os.path.isdir(os.path.join(self.games, "Halo 3")))  # Not looked at yet.
            c = Client(port)
            c.names()
            self.assertTrue(self.wait_for(lambda: os.path.isdir(os.path.join(self.games, "Halo 3")), 15))
            c.close()
        finally:
            server.kill()
            server.stdout.read()

    def test_writes_aurora_s_cleanup_script_and_online_filter(self):
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        self.add_iso("Skate 3.iso", 0x5C087C2C, 0x454108E6)
        out = self.push(extra=["--tidy-aurora"])  # With the tidy as Aurora starts (off unless chosen).
        self.assertIn("wrote Aurora's \"Hide PC games while NetDisc360 is off\" filter", out)
        scripts = os.path.join(self.xbox, "Hdd1", "Apps", "Aurora", "User", "Scripts")
        cleanup = os.path.join(scripts, "Utility", "NetDisc360 Cleanup", "Main.lua")
        online = os.path.join(scripts, "Content", "Filters", "NetDiscOnline.lua")
        for path in (cleanup, online):
            with open(path, "rb") as f:
                self.assertIn(b"\r\n", f.read())
        # The marker is there while the server runs; it goes as it quits.
        self.assertIn("online.txt", self.ftp.stored)
        self.assertIn("DELE online.txt", self.ftp.log)
        self.assertEqual(os.listdir(self.app), ["boot.xex"])
        # Nothing new: neither is written again.
        self.ftp.stored.clear()
        self.push(extra=["--tidy-aurora"])
        self.assertEqual(self.ftp.stored, ["online.txt"])
        if LUA:
            check = os.path.join(self.tmp.name, "check.lua")
            with open(check, "w") as f:
                f.write(r"""
local files = { ['Hdd1:\\Apps\\NetDisc360\\online.txt'] = 'x' }
FileSystem = {
   FileExists = function(path) return files[path] ~= nil end,
   DeleteFile = function(path) files[path] = nil end,
   WriteFile = function(path, text) files[path] = text end }
local shown = 'Hdd1:\\Apps\\NetDisc360\\shown.txt'
local tile = { Directory = '\\NetDisc360\\Games\\Halo 3\\' }
local installed = { Directory = '\\Games\\Halo 3\\' }
-- Aurora starts while NetDisc360 runs: everything shows.
files[shown] = 'offline'
GameListFilterCategories = { User = {} }
dofile(%(online)r)
local show = GameListFilterCategories.User['Hide PC games while NetDisc360 is off']
assert(files[shown] == nil)
assert(show(tile) and show(installed))
assert(files[shown] == 'online')
-- Aurora starts while it doesn't: the tiles are hidden.
files = {}
GameListFilterCategories = { User = {} }
dofile(%(online)r)
show = GameListFilterCategories.User['Hide PC games while NetDisc360 is off']
assert(not show(tile) and show(installed))
assert(not show({ Directory = 'NetDisc360\\Games\\Skate 3' }))
assert(files[shown] == 'offline')
-- The cleanup takes the rows of tiles that are gone off the list, after asking.
local done, asked, refreshed = {}, nil, false
Sql = {
   ExecuteFetchRows = function(query) return {
      { Id = 1, Directory = '\\NetDisc360\\Games\\Halo 3\\' },
      { Id = '2', Directory = 'Hdd1:\\NetDisc360\\Games\\Old Game\\' },
      { Id = 3, Directory = '\\Games\\Old Game\\' },
      { Id = 4, Directory = '\\NETDISC360\\GAMES\\Gone\\' },
      { Id = 5, Directory = '\\NetDisc360\\Games\\Skate 3' } } end,
   Execute = function(query) table.insert(done, query) end }
Script = {
   ShowMessageBox = function(title, text, ...) asked = { ... }; return { Button = 1 } end,
   RefreshListOnExit = function(on) refreshed = on end }
dofile(%(cleanup)r)
assert(scriptTitle == 'NetDisc360 Cleanup' and scriptPermissions[1] == 'sql')
main()
assert(#done == 5, #done)
assert(done[1] == 'DELETE FROM Config.UserRecentGames WHERE ContentId IN (2,4)', done[1])
assert(done[5] == 'DELETE FROM ContentItems WHERE Id IN (2,4)', done[5])
assert(refreshed and asked[1] == 'OK')
-- Cancel changes nothing.
done = {}
Script.ShowMessageBox = function() return { Button = 2 } end
main()
assert(#done == 0)
-- As Aurora starts, the filter's script does the same without asking:
-- the rows of tiles without a default.xex (Skate 3 isn't one: no file).
files = { ['Hdd1:\\NetDisc360\\Games\\halo 3\\default.xex'] = 'x' }
Script = nil
GameListFilterCategories = { User = {} }
dofile(%(online)r)
assert(#done == 5, #done)
assert(done[1] == 'DELETE FROM Config.UserRecentGames WHERE ContentId IN (2,4,5)', done[1])
assert(done[5] == 'DELETE FROM ContentItems WHERE Id IN (2,4,5)', done[5])
-- No tile there at all (a drive it can't read): nothing changes.
done, files = {}, {}
dofile(%(online)r)
assert(#done == 0)
print('lua ok')
""" % {"online": online, "cleanup": cleanup})
            result = subprocess.run([LUA, check], capture_output=True, text=True)
            self.assertEqual(result.stdout.strip(), "lua ok", result.stderr)
        # Without it, the filter changes nothing in Aurora's list.
        with open(online, "rb") as f:
            self.assertIn(b"NetDiscTidy", f.read())
        self.push()
        with open(online, "rb") as f:
            self.assertNotIn(b"NetDiscTidy", f.read())

    def test_restarts_aurora_when_its_filter_hid_the_games(self):
        # Aurora started while the server didn't run, and the filter hid the
        # games: Aurora, on screen, is restarted once so they show.
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        helper = self.app
        os.makedirs(helper)
        with open(os.path.join(helper, "shown.txt"), "w") as f:
            f.write("offline")
        aurora = AuroraWeb()
        try:
            out = self.push(extra=["--aurora-web-port", str(aurora.port)])
            self.assertIn("restarted Aurora so its filter shows the PC games again", out)
            self.assertEqual(aurora.launched, [("\\Device\\Harddisk0\\Partition1\\Apps\\Aurora", "Aurora.xex", "0")])
            # The filter showed them as the server quit: restarted to hide them.
            with open(os.path.join(helper, "shown.txt"), "w") as f:
                f.write("online")
            aurora.launched.clear()
            out = self.push(extra=["--aurora-web-port", str(aurora.port)])
            self.assertIn("restarted Aurora so its filter hides the PC games while NetDisc360 is off", out)
            self.assertEqual(len(aurora.launched), 1, aurora.launched)
            # Never during a game.
            aurora.running = "\\Device\\Cdrom0\\default.xex"
            aurora.launched.clear()
            out = self.push(extra=["--aurora-web-port", str(aurora.port)])
            self.assertIn("didn't restart Aurora so its filter hides the PC games while NetDisc360 is off: Aurora isn't on screen", out)
            self.assertEqual(aurora.launched, [])
        finally:
            aurora.close()
        # The NetISO kit's older Nova, with Aurora's HTTP security on.
        with open(os.path.join(helper, "shown.txt"), "w") as f:
            f.write("offline")
        aurora = AuroraWeb(basic=True, old=True)
        try:
            out = self.push(extra=["--aurora-web-port", str(aurora.port)])
            self.assertIn("restarted Aurora so its filter shows the PC games again", out)
            self.assertEqual(aurora.launched, [("\\Device\\Harddisk0\\Partition1\\Apps\\Aurora", "Aurora.xex", "0")])
        finally:
            aurora.close()

    def test_moves_the_tiles_from_hdd1_netiso(self):
        # A console set up before the NetDisc360 folders: its tiles in
        # Hdd1:\NetISO, NetDisc360's own files in Hdd1:\NetISO\NetDisc360.
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        self.add_iso("Skate 3.iso", 0x5C087C2C, 0x454108E6)
        self.add_iso("Gears.iso", 0x11111111, 0x4D5307D5)
        old = os.path.join(self.xbox, "Hdd1", "NetISO")
        for folder in ("Halo 3", "Skate 3", "Gears", "NetISO Sync", "NetDisc360"):
            os.makedirs(os.path.join(old, folder))
        for game in ("Halo 3", "Skate 3", "Gears"):
            with open(os.path.join(old, game, "netiso.ini"), "w") as f:
                f.write("[NetISO]\r\niso=%s.iso\r\n" % game)
            with open(os.path.join(old, game, "default.xex"), "wb") as f:
                f.write(b"OLD LAUNCHER")
        with open(os.path.join(old, "Skate 3", "save.dat"), "w") as f:
            f.write("kept with its folder")
        for name, data in (("boot.xex", b"OLD HELPER"), ("disc.log", b"log"), ("profiles.ini", b"[Profiles]")):
            with open(os.path.join(old, "NetDisc360", name), "wb") as f:
                f.write(data)
        # Skate 3's and Gears' names are taken in the new folder already (an
        # older server put them back): the old Skate 3 has a file of someone
        # else's and stays; the old Gears is only a tile and goes.
        for game in ("Skate 3", "Gears"):
            os.makedirs(os.path.join(self.games, game))
            with open(os.path.join(self.games, game, "netiso.ini"), "w") as f:
                f.write("[NetISO]\r\niso=%s.iso\r\n" % game)
        aurora = AuroraWeb()
        try:
            out = self.push(extra=["--aurora-web-port", str(aurora.port)])
            self.assertIn("moved 1 game folder from /Hdd1/NetISO to /Hdd1/NetDisc360/Games, and NetDisc360's own files to /Hdd1/Apps/NetDisc360"
                          " (removed 1 older copy of tiles the new folder has) (1 folder left there: the same name is taken in the new folder).", out)
            self.assertIn("/Hdd1/NetDisc360/Games/Halo 3", self.ftp.renamed)  # Renamed, not copied.
            self.assertEqual(sorted(os.listdir(self.games)), ["Gears", "Halo 3", "Skate 3"])
            self.assertEqual(sorted(os.listdir(old)), ["NetISO Sync", "Skate 3"])  # Not NetDisc360's, or taken.
            self.assertEqual(sorted(os.listdir(os.path.join(self.games, "Halo 3"))), ["default.xex", "netiso.ini"])
            self.assertIn("save.dat", os.listdir(os.path.join(old, "Skate 3")))
            self.assertEqual(sorted(os.listdir(self.app)), ["boot.xex", "disc.log", "move.txt", "profiles.ini"])  # online.txt went as it quit.
            with open(os.path.join(self.app, "boot.xex"), "rb") as f:
                self.assertEqual(f.read(), b"BOOT HELPER")
            self.assertIn("restarted Aurora so its list follows the games to their new folder", out)
            self.assertEqual(len(aurora.launched), 1)
            online = os.path.join(self.xbox, "Hdd1", "Apps", "Aurora", "User", "Scripts", "Content", "Filters", "NetDiscOnline.lua")
            cleanup = os.path.join(self.xbox, "Hdd1", "Apps", "Aurora", "User", "Scripts", "Utility", "NetDisc360 Cleanup", "Main.lua")
            for path in (online, cleanup):
                with open(path, "rb") as f:
                    self.assertIn(b"local function NetDiscMove()", f.read())
            if LUA:
                self.check_move_script(online, cleanup)
            # Aurora's start moved its rows: restarted once more, so its list
            # shows them; the scripts stop moving.
            with open(os.path.join(self.app, "moved.txt"), "w") as f:
                f.write("moved=2 left=1 scanpath=1")
            aurora.launched.clear()
            out = self.push(extra=["--aurora-web-port", str(aurora.port)])
            self.assertIn("Aurora's list follows the games to /Hdd1/NetDisc360/Games: 2 games moved, and its scan path with them, 1 game stay", out)
            self.assertIn("restarted Aurora so its list shows the games in their new folder", out)
            self.assertNotIn("its list follows the games to their new folder", out)
            self.assertEqual(len(aurora.launched), 1)
            self.assertNotIn("move.txt", os.listdir(self.app))
            self.assertNotIn("moved.txt", os.listdir(self.app))
            with open(online, "rb") as f:
                self.assertNotIn(b"NetDiscMove", f.read())
            # The filter couldn't read Aurora's list: the cleanup script is
            # next, and the scripts keep moving.
            with open(os.path.join(self.app, "move.txt"), "w") as f:
                f.write("x")
            with open(os.path.join(self.app, "moved.txt"), "w") as f:
                f.write("unread")
            aurora.launched.clear()
            out = self.push(extra=["--aurora-web-port", str(aurora.port)])
            self.assertIn("Run Utilities, NetDisc360 Cleanup in Aurora to do it.", out)
            self.assertEqual(aurora.launched, [])
            self.assertIn("move.txt", os.listdir(self.app))
        finally:
            aurora.close()

    def check_move_script(self, online, cleanup):
        """Runs the scripts' NetDiscMove as Aurora would, against SQLite
        databases made like Aurora's, and checks what it did."""
        db_dir = os.path.join(self.tmp.name, "db")
        os.makedirs(db_dir, exist_ok=True)
        content, settings = os.path.join(db_dir, "content.db"), os.path.join(db_dir, "settings.db")

        def fresh():
            for path in (content, settings):
                if os.path.exists(path):
                    os.remove(path)
            db = sqlite3.connect(content)
            db.execute("CREATE TABLE ContentItems ( Id INTEGER PRIMARY KEY AUTOINCREMENT, Directory TEXT COLLATE NOCASE, Executable TEXT COLLATE NOCASE, "
                       "ScanPathId INTEGER, UNIQUE (Directory, Executable, ScanPathId) )")
            for directory, scan in (("\\netiso\\halo 3", 2), ("\\NetISO\\Skate 3", 2), ("\\netiso\\gone game", 2), ("\\games\\halo 3", 1),
                                    ("Hdd1:\\NetISO\\Halo 3\\", 2), ("\\netiso sync\\x", 2)):
                db.execute("INSERT INTO ContentItems (Directory, Executable, ScanPathId) VALUES (?, 'default.xex', ?)", (directory, scan))
            # NetDisc360's helper, as Aurora's \Apps scan path finds it.
            db.execute("INSERT INTO ContentItems (Directory, Executable, ScanPathId) VALUES ('\\apps\\netdisc360', 'boot.xex', 3)")
            db.commit()
            db.close()
            db = sqlite3.connect(settings)
            db.execute("CREATE TABLE ScanPaths ( Id INTEGER PRIMARY KEY AUTOINCREMENT, Path TEXT, DeviceId TEXT, Depth INTEGER )")
            db.execute("INSERT INTO ScanPaths (Path, DeviceId, Depth) VALUES ('\\Games', 'hdd', 1), ('\\NetISO', 'hdd', 1)")
            for table in ("UserHidden", "UserFavorites"):
                db.execute("CREATE TABLE %s ( Id INTEGER PRIMARY KEY AUTOINCREMENT, ContentId INTEGER, ProfileId TEXT COLLATE NOCASE, "
                           "UNIQUE (ContentId, ProfileId) )" % table)
            db.execute("CREATE TABLE UserRecentGames ( Id INTEGER PRIMARY KEY AUTOINCREMENT, DateTime INTEGER, ContentId INTEGER, "
                       "ProfileId TEXT COLLATE NOCASE, UNIQUE (ContentId, ProfileId) )")
            # Hidden by hand in the old folder; a favourite of another profile.
            db.execute("INSERT INTO UserHidden (ContentId, ProfileId) VALUES (20, 'E00002F2FA71CA6F'), (20, '')")
            db.execute("INSERT INTO UserFavorites (ContentId, ProfileId) VALUES (1, 'e0000bf5ae159e2b')")
            db.commit()
            db.close()

        def run(script, call):
            db = sqlite3.connect(content)
            db.execute("ATTACH DATABASE ? AS Config", (settings,))
            rows = {}
            for query, table in (("SELECT Id, Path FROM Config.ScanPaths", "paths"), ("SELECT Id, Directory FROM ContentItems", "items")):
                rows[table] = db.execute(query).fetchall()
            profiles = db.execute("SELECT ProfileId FROM Config.UserHidden UNION SELECT ProfileId FROM Config.UserFavorites "
                                  "UNION SELECT ProfileId FROM Config.UserRecentGames").fetchall()
            harness = os.path.join(self.tmp.name, "move.lua")

            def lua_rows(items, column):
                return "{ " + ", ".join("{ Id = %d, %s = %s }" % (i, column, lua_string(v)) for i, v in items) + " }"
            with open(harness, "w") as f:
                f.write("local paths = %s\nlocal items = %s\n" % (lua_rows(rows["paths"], "Path"), lua_rows(rows["items"], "Directory")))
                f.write("local profiles = { %s }\n" % ", ".join("{ ProfileId = %s }" % lua_string(p) for p, in profiles))
                f.write("files = {}\n"
                        "FileSystem = { FileExists = function(p) return files[p] ~= nil end, DeleteFile = function(p) files[p] = nil end,\n"
                        "   WriteFile = function(p, t) files[p] = t end }\n"
                        "Sql = { ExecuteFetchRows = function(q) if string.find(q, 'ScanPaths') then return paths end\n"
                        "   if string.find(q, 'ProfileId') then return profiles end return items end,\n"
                        "   Execute = function(q) print('SQL ' .. q) return true end }\n"
                        "Script = { ShowMessageBox = function() return { Button = 2 } end, RefreshListOnExit = function() end }\n"
                        "GameListFilterCategories = { User = {} }\n"
                        "dofile(%r)\n%s\n"
                        "print('MOVED ' .. tostring(files['Hdd1:\\\\Apps\\\\NetDisc360\\\\moved.txt']))\n" % (script, call))
            result = subprocess.run([LUA, harness], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            moved = None
            for line in result.stdout.splitlines():
                if line.startswith("SQL "):
                    db.execute(line[4:])
                elif line.startswith("MOVED "):
                    moved = line[6:]
            db.commit()
            items = db.execute("SELECT Id, Directory, ScanPathId FROM ContentItems ORDER BY Id").fetchall()
            paths = db.execute("SELECT Id, Path FROM Config.ScanPaths ORDER BY Id").fetchall()
            hidden = db.execute("SELECT ContentId, upper(ProfileId) FROM Config.UserHidden ORDER BY ContentId, upper(ProfileId)").fetchall()
            db.close()
            self.hidden = hidden
            return moved, items, paths

        # As Aurora starts (the filter), and from Utilities (the cleanup script).
        for script, call in ((online, ""), (cleanup, "main()")):
            fresh()
            moved, items, paths = run(script, call)
            self.assertEqual(moved, "moved=3 left=1 scanpath=1", script)
            # Only the tiles that are in the new folder now; the scan path keeps its Id.
            self.assertEqual(items, [(1, "\\netdisc360\\games\\halo 3", 2), (2, "\\NetDisc360\\Games\\Skate 3", 2), (3, "\\netiso\\gone game", 2),
                                     (4, "\\games\\halo 3", 1), (5, "Hdd1:\\NetDisc360\\Games\\Halo 3\\", 2), (6, "\\netiso sync\\x", 2),
                                     (7, "\\apps\\netdisc360", 3)])
            self.assertEqual(paths, [(1, "\\Games"), (2, "\\NetDisc360\\Games")])
            # As Aurora starts, the helper is hidden for every profile Aurora knows of.
            helper = [(7, ""), (7, "E00002F2FA71CA6F"), (7, "E0000BF5AE159E2B")] if script == online else []
            self.assertEqual(self.hidden, helper + [(20, ""), (20, "E00002F2FA71CA6F")])
            # Again: nothing left to do.
            moved, again, _ = run(script, call)
            self.assertEqual(moved, "moved=0 left=1 scanpath=0")
            self.assertEqual(again, items)
        # Aurora had a scan path for the new folder already: the rows go under it.
        fresh()
        db = sqlite3.connect(settings)
        db.execute("INSERT INTO ScanPaths (Path, DeviceId, Depth) VALUES ('NetDisc360\\Games\\', 'hdd', 1)")
        db.commit()
        db.close()
        moved, items, paths = run(online, "")
        self.assertEqual(moved, "moved=3 left=1 scanpath=0")
        self.assertEqual(items[0], (1, "\\netdisc360\\games\\halo 3", 3))
        self.assertEqual(paths[1], (2, "\\NetISO"))

    def test_follows_this_computer_s_new_address(self):
        # NetISO.xex.txt names 127.0.0.9, an address this computer had: once
        # the console is quiet and nothing answers there, it names this one.
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        usb = os.path.join(self.xbox, "Usb0")
        os.makedirs(os.path.join(usb, "Plugins"))
        with open(os.path.join(usb, "launch.ini"), "w") as f:
            f.write("[Paths]\nDefault = Hdd:\\Apps\\Aurora\\Aurora.xex\n[Plugins]\nplugin1 = Usb:\\Plugins\\NetISO.xex\n")
        with open(os.path.join(usb, "Plugins", "NetISO.xex.txt"), "w") as f:
            f.write("127.0.0.9\r\n")
        config = os.path.join(self.tmp.name, "netdisc.ini")
        with open(config, "w") as f:
            f.write("[Server]\nown_addresses=10.0.0.5,127.0.0.9\n")
        out = self.push(extra=["--config", config])
        self.assertIn("its NetISO.xex.txt (/Usb0/Plugins/NetISO.xex.txt) named 127.0.0.9, this computer's address before; it names 127.0.0.1 now", out)
        with open(os.path.join(usb, "Plugins", "NetISO.xex.txt"), newline="") as f:
            self.assertEqual(f.read(), "127.0.0.1\r\n")
        # An address this computer never had is left alone.
        with open(os.path.join(usb, "Plugins", "NetISO.xex.txt"), "w") as f:
            f.write("127.0.0.8")
        out = self.push(extra=["--config", config])
        self.assertNotIn("NetISO.xex.txt (", out)
        with open(os.path.join(usb, "Plugins", "NetISO.xex.txt")) as f:
            self.assertEqual(f.read(), "127.0.0.8")
        # NetDisc360.xex's own drive reads server= in NetDisc360.ini.
        with open(os.path.join(usb, "launch.ini"), "w") as f:
            f.write("[Paths]\nDefault = Hdd:\\Apps\\Aurora\\Aurora.xex\n[Plugins]\nplugin1 = Usb:\\Plugins\\NetDisc360.xex\n")
        with open(os.path.join(usb, "Plugins", "NetDisc360.ini"), "w", newline="") as f:
            f.write("; NetDisc360.xex\r\nwait = 90\r\nServer = 127.0.0.9:4323\r\n")
        out = self.push(extra=["--config", config])
        self.assertIn("its NetDisc360.ini (/Usb0/Plugins/NetDisc360.ini) named 127.0.0.9, this computer's address before; it names 127.0.0.1 now", out)
        with open(os.path.join(usb, "Plugins", "NetDisc360.ini"), newline="") as f:
            self.assertEqual(f.read(), "; NetDisc360.xex\r\nwait = 90\r\nServer = 127.0.0.1:4323\r\n")
        os.remove(os.path.join(usb, "Plugins", "NetDisc360.ini"))
        with open(os.path.join(usb, "launch.ini"), "w") as f:
            f.write("[Paths]\nDefault = Hdd:\\Apps\\Aurora\\Aurora.xex\n[Plugins]\nplugin1 = Usb:\\Plugins\\NetISO.xex\n")
        # And so is all of it when following is off.
        with open(os.path.join(usb, "Plugins", "NetISO.xex.txt"), "w") as f:
            f.write("127.0.0.9")
        with open(config, "a") as f:
            f.write("follow_address=0\n")
        self.push(extra=["--config", config])
        with open(os.path.join(usb, "Plugins", "NetISO.xex.txt")) as f:
            self.assertEqual(f.read(), "127.0.0.9")

    def test_starts_a_game_from_the_dashboard(self):
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        self.add_iso("Skate 3.iso", 0x5C087C2C, 0x454108E6)
        self.install("Games/Skate 3", 0x5C087C2C, 0x454108E6)
        aurora = AuroraWeb()
        self.web = free_port()
        server, _ = self.run_server("--web-port", str(self.web), "--aurora-web-port", str(aurora.port))
        try:
            self.assertTrue(self.wait_for(lambda: self.ftp.scans, 15))
            games = {g["name"]: g for g in self.json("GET", "/api/games")["games"]}
            self.assertEqual(games["Halo 3"]["tiles"], ["127.0.0.1"])
            self.assertEqual(games["Skate 3"]["installedOn"], ["127.0.0.1"])
            self.assertEqual(self.json("POST", "/api/play", {"game": "Halo 3", "console": "127.0.0.1"}), {"ok": True})
            # An installed game starts from where it's installed.
            self.json("POST", "/api/play", {"game": "Skate 3", "console": "127.0.0.1"})
            self.assertEqual(aurora.launched, [("\\Device\\Harddisk0\\Partition1\\NetDisc360\\Games\\Halo 3", "default.xex", "0"),
                                               ("\\Device\\Harddisk0\\Partition1\\Games\\Skate 3", "default.xex", "0")])
            status, _, data = self.call("POST", "/api/play", {"game": "Halo 3", "console": "127.0.0.7"})
            self.assertEqual(status, 400)
            self.assertIn(b"hasn't used NetDisc360", data)
            # With Aurora's HTTP security on, the login goes on each request.
            aurora.launched.clear()
            aurora.basic = True
            self.assertEqual(self.json("POST", "/api/play", {"game": "Halo 3", "console": "127.0.0.1"}), {"ok": True})
            self.assertEqual(aurora.launched, [("\\Device\\Harddisk0\\Partition1\\NetDisc360\\Games\\Halo 3", "default.xex", "0")])
            self.json("POST", "/api/settings", {"auroraWebPassword": "wrong"})
            status, _, data = self.call("POST", "/api/play", {"game": "Halo 3", "console": "127.0.0.1"})
            self.assertEqual(status, 400)
            self.assertIn(b"refused the login", data)
            self.assertTrue(self.wait_for(lambda: "couldn't start Halo 3 from the dashboard: Aurora's web server refused the login" in json.dumps(self.json("GET", "/api/log?after=0")), 5))
            # The NetISO kit's older Nova starts it at /postLaunchGame.
            self.json("POST", "/api/settings", {"auroraWebPassword": "xboxhttp"})
            aurora.launched.clear()
            aurora.old = True
            self.assertEqual(self.json("POST", "/api/play", {"game": "Halo 3", "console": "127.0.0.1"}), {"ok": True})
            self.assertEqual(aurora.launched, [("\\Device\\Harddisk0\\Partition1\\NetDisc360\\Games\\Halo 3", "default.xex", "0")])
            self.assertEqual(aurora.cookies[-1], "fsplugin_http_session=s1")
            # Aurora's web server off.
            aurora.close()
            status, _, data = self.call("POST", "/api/play", {"game": "Halo 3", "console": "127.0.0.1"})
            self.assertEqual(status, 400)
            self.assertIn(b"doesn't answer on port", data)
        finally:
            aurora.close()
            server.kill()
            server.communicate()

    def second_console(self):
        """A second console at 127.0.0.2 with its own Aurora, whose FTP server
        is on the first one's port: its files' folder and its FTP server."""
        other = os.path.join(self.tmp.name, "xbox2")
        os.makedirs(os.path.join(other, "Hdd1", "Apps", "Aurora", "Media", "Scripts", "Content"))
        ftp = aurora_ftp.Server(other, self.ftp.server_address[1], host="127.0.0.2")
        threading.Thread(target=ftp.serve_forever, daemon=True).start()
        self.addCleanup(ftp.server_close)
        self.addCleanup(ftp.shutdown)
        return other, ftp

    def test_start_on_all_plays_the_same_copy_everywhere(self):
        # Installed on one console only, with a tile on each (--no-installed):
        # every console starts the copy on this PC, the same for all.
        self.add_iso("Skate 3.iso", 0x5C087C2C, 0x454108E6)
        other, ftp2 = self.second_console()
        self.install("Games/Skate 3", 0x5C087C2C, 0x454108E6, xbox=other)
        aurora = AuroraWeb()
        aurora2 = AuroraWeb(host="127.0.0.2", port=aurora.port)
        self.addCleanup(aurora.close)
        self.addCleanup(aurora2.close)
        self.web = free_port()
        server, _ = self.run_server("--xbox", "127.0.0.2", "--web-port", str(self.web), "--aurora-web-port", str(aurora.port), "--no-installed")
        try:
            self.assertTrue(self.wait_for(lambda: self.ftp.scans and ftp2.scans, 20))
            self.assertTrue(self.wait_for(lambda: {g["name"]: g for g in self.json("GET", "/api/games")["games"]}["Skate 3"]["installedOn"] == ["127.0.0.2"], 20))
            self.json("POST", "/api/party", {"active": True, "gamemaster": "127.0.0.2", "consoles": ["127.0.0.1"]})
            done = self.json("POST", "/api/party/start", {"game": "Skate 3"})
            self.assertEqual(done["copy"], "tile")
            self.assertEqual([(r["address"], r["ok"], r["how"]) for r in done["results"]], [("127.0.0.2", True, "tile"), ("127.0.0.1", True, "tile")])
            self.assertEqual(aurora2.launched, [("\\Device\\Harddisk0\\Partition1\\NetDisc360\\Games\\Skate 3", "default.xex", "0")])
        finally:
            server.kill()
            out = server.communicate()[0]
        self.assertIn("so every console starts the copy on this PC (the same one).", out)

    def edit_from(self, address, port, text):
        """A change to the party from a console's party screen (PartyEditAsk): its answer."""
        data = text.encode("utf-8")
        with connect_from(address, port) as conn:
            conn.sendall(b"ISVR" + struct.pack(">HHQI", 0x5045, 0, 0, len(data)) + data)
            return exact(conn, struct.unpack(">I", exact(conn, 4))[0]).decode("utf-8")

    def test_game_vote_and_readiness(self):
        # Marcus 2026-11-02: everyone votes for the next game with their
        # controller; the wheel spins between the top picks and the game it
        # stops on starts on every party console. Each console says if it's ready.
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        self.add_iso("Skate 3.iso", 0x5C087C2C, 0x454108E6)
        self.add_iso("Zuma.iso", 0x2B3C4D5E, 0x58410912)
        other, ftp2 = self.second_console()
        aurora = AuroraWeb()
        aurora2 = AuroraWeb(host="127.0.0.2", port=aurora.port)
        self.addCleanup(aurora.close)
        self.addCleanup(aurora2.close)
        self.web = free_port()
        server, port = self.run_server("--xbox", "127.0.0.2", "--web-port", str(self.web), "--aurora-web-port", str(aurora.port))
        drive = None
        try:
            self.assertTrue(self.wait_for(lambda: self.ftp.scans and ftp2.scans, 20))
            self.assertTrue(self.wait_for(lambda: all(len(g["tiles"]) == 2 for g in self.json("GET", "/api/games")["games"]), 20))
            status, _, data = self.call("POST", "/api/party/vote", {"action": "open"})
            self.assertEqual((status, json.loads(data)["error"]), (400, "Start the party first."))
            self.json("POST", "/api/party", {"active": True, "gamemaster": "127.0.0.2", "consoles": ["127.0.0.1"]})
            # Readiness: both answer, but neither has its disc drive connected; then the first one does.
            ready = self.json("GET", "/api/state")["party"]["ready"]
            self.assertEqual(ready, {a: ["Its NetDisc360 disc drive isn't connected, so games from this PC can't start on it"] for a in ["127.0.0.1", "127.0.0.2"]})
            drive = Client(port)
            drive.ping()
            self.assertTrue(self.wait_for(lambda: self.json("GET", "/api/state")["party"]["ready"]["127.0.0.1"] == [], 10))
            lines = {}
            for line in self.party_text_from("127.0.0.1", port).split("\n"):
                fields = line.split("\t")
                lines.setdefault(fields[0], []).append(fields[1:])
            self.assertEqual(lines["ready"][0][:2], ["1", "2"])
            self.assertTrue(lines["ready"][0][2].endswith(": Its NetDisc360 disc drive isn't connected, so games from this PC can't start on it"), lines["ready"])
            # The vote opens among the games every party console has.
            said = self.json("POST", "/api/party/vote", {"action": "open", "seconds": 120})["said"]
            self.assertEqual(said, "Vote for the next game: 3 games, 120 seconds.")
            status, _, data = self.call("POST", "/api/party/vote", {"action": "open"})
            self.assertEqual((status, json.loads(data)["error"]), (400, "A vote is on already."))
            vote = self.json("GET", "/api/state")["party"]["vote"]
            games = [g["name"] for g in vote["games"]]
            self.assertEqual(sorted(games), ["Halo 3", "Skate 3", "Zuma"])
            self.assertTrue(vote["open"])
            self.assertGreater(vote["left"], 100)
            skate, halo = games.index("Skate 3"), games.index("Halo 3")
            # Each controller votes on its console; any console votes, only the Gamemaster's closes it.
            self.assertEqual(self.edit_from("127.0.0.1", port, "vote\t0\t%d" % skate), "ok\t\n")
            self.assertEqual(self.edit_from("127.0.0.1", port, "vote\t1\t%d" % skate), "ok\t\n")
            self.assertEqual(self.edit_from("127.0.0.2", port, "vote\t0\t%d" % skate), "ok\t\n")
            self.assertEqual(self.edit_from("127.0.0.2", port, "vote\t1\t9"), "error\tThat isn't a game of the vote.\n")
            self.party_text_from("127.0.0.2", port)  # The Gamemaster's console shows the party.
            self.assertEqual(self.edit_from("127.0.0.1", port, "voteclose"), "error\tOnly the Gamemaster's console sets up the party.\n")
            text = self.party_text_from("127.0.0.1", port)
            self.assertIn("myvote\t0\t%d\n" % skate, text)
            self.assertIn("myvote\t1\t%d\n" % skate, text)
            self.assertIn("votegame\tSkate 3\t3\n", text)
            # Every vote for one game: it starts on all, no wheel.
            self.assertEqual(self.edit_from("127.0.0.2", port, "voteclose"), "ok\tSkate 3 won the vote.\n")
            started = lambda a, game: ("\\Device\\Harddisk0\\Partition1\\NetDisc360\\Games\\" + game, "default.xex", "0") in a.launched
            self.assertTrue(self.wait_for(lambda: started(aurora, "Skate 3") and started(aurora2, "Skate 3"), 15))
            party = self.json("GET", "/api/state")["party"]
            self.assertEqual((party["vote"]["open"], party["vote"]["picked"], party["game"]), (False, "Skate 3", "Skate 3"))
            # Split votes: the wheel spins between the top two only (none for Zuma), then the one it stops on starts.
            aurora.launched.clear()
            aurora2.launched.clear()
            self.assertEqual(self.edit_from("127.0.0.2", port, "voteopen\t30"), "ok\tVote for the next game: 3 games, 30 seconds.\n")
            vote = self.json("GET", "/api/state")["party"]["vote"]
            games = [g["name"] for g in vote["games"]]
            skate, halo = games.index("Skate 3"), games.index("Halo 3")
            self.assertEqual(self.edit_from("127.0.0.1", port, "vote\t0\t%d" % halo), "ok\t\n")
            self.assertEqual(self.edit_from("127.0.0.1", port, "vote\t1\t%d" % halo), "ok\t\n")
            self.assertEqual(self.edit_from("127.0.0.2", port, "vote\t2\t%d" % skate), "ok\t\n")
            self.assertEqual(self.edit_from("127.0.0.2", port, "vote\t3\t%d" % skate), "ok\t\n")
            self.assertEqual(self.edit_from("127.0.0.2", port, "vote\t3\t-1"), "ok\t\n")  # Taken back.
            vote = self.json("GET", "/api/state")["party"]["vote"]
            self.assertEqual(({g["name"]: g["votes"] for g in vote["games"]}, vote["voters"]), ({"Halo 3": 2, "Skate 3": 1, "Zuma": 0}, 3))
            self.assertEqual(self.json("POST", "/api/party/vote", {"action": "close"})["said"], "The wheel picks the game from the top 2.")
            party = self.json("GET", "/api/state")["party"]
            spin = party["spin"]
            self.assertEqual((spin["title"], set(spin["wheel"])), ("Which game?", {"Halo 3", "Skate 3"}))
            picked = party["vote"]["picked"]
            self.assertEqual(spin["wheel"][spin["steps"][-1]["segment"]], picked)
            self.assertFalse(started(aurora, picked))  # Not before the wheel stopped.
            self.assertTrue(self.wait_for(lambda: started(aurora, picked) and started(aurora2, picked), spin["totalMs"] / 1000 + 15))
            # Called off: nothing starts.
            self.json("POST", "/api/party/vote", {"action": "open", "seconds": 15})
            self.assertEqual(self.json("POST", "/api/party/vote", {"action": "cancel"})["said"], "The game vote is called off.")
            status, _, data = self.call("POST", "/api/party/vote", {"action": "close"})
            self.assertEqual((status, json.loads(data)["error"]), (400, "No vote is on."))
            feed = [x["text"] for x in self.json("GET", "/api/state")["party"]["feed"]]
            self.assertIn("Skate 3 won the vote.", feed)
            self.assertIn("The wheel picked " + picked, feed)
        finally:
            if drive:
                drive.close()
            server.kill()
            out = server.communicate()[0]
        self.assertIn("LAN party: a game vote opened (3 games).", out)
        self.assertIn("LAN party: The game vote is called off.", out)

    def test_saved_parties_start_again_with_one_tap(self):
        # Marcus 2026-11-04: save a party's setup (games, teams, players) and
        # start it again later with one tap. Tonight's games are what the vote offers.
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        self.add_iso("Skate 3.iso", 0x5C087C2C, 0x454108E6)
        self.add_iso("Zuma.iso", 0x2B3C4D5E, 0x58410912)
        other, ftp2 = self.second_console()
        aurora = AuroraWeb()
        aurora2 = AuroraWeb(host="127.0.0.2", port=aurora.port)
        self.addCleanup(aurora.close)
        self.addCleanup(aurora2.close)
        config = os.path.join(self.tmp.name, "netdisc.ini")
        self.web = free_port()

        def stop(server):
            server.kill()
            try:
                server.communicate()
            except ValueError:  # Stopped already.
                pass
        server, _ = self.run_server("--xbox", "127.0.0.2", "--web-port", str(self.web), "--aurora-web-port", str(aurora.port), "--config", config)
        self.addCleanup(stop, server)

        def party():
            return self.json("GET", "/api/state")["party"]

        def presets():
            return {x["name"]: x for x in party()["presets"]}
        self.assertTrue(self.wait_for(lambda: self.ftp.scans and ftp2.scans, 20))
        self.assertTrue(self.wait_for(lambda: all(len(g["tiles"]) == 2 for g in self.json("GET", "/api/games")["games"]), 20))
        # Friday: two consoles, four players (three here), three teams, a rule, two of tonight's games on both (and one that isn't).
        self.json("POST", "/api/party", {"gamemaster": "127.0.0.2", "consoles": ["127.0.0.1"], "teams": 3, "balance": "wins",
                                         "guests": [{"name": "Ola", "here": True}, {"name": "Kari", "here": True}, {"name": "Lars", "here": True}, {"name": "Tom", "here": False}],
                                         "together": [["Ola", "Kari"]], "games": ["Zuma", "Halo 3", "Not Here", "zuma"], "name": "Friday LAN", "idleOffMinutes": 30})
        self.assertEqual(party()["games"], ["Zuma", "Halo 3", "Not Here"])
        for body in ({"games": "Zuma"}, {"games": ["x"] * 11}, {"games": [""]}):
            self.assertEqual(self.call("POST", "/api/party", body)[0], 400, body)
        r = self.json("POST", "/api/party/presets", {"action": "save", "name": "Friday"})
        self.assertEqual(r["said"], "Friday is saved: start it again with one tap.")
        friday = presets()["Friday"]
        self.assertEqual((friday["party"], friday["guests"], friday["here"], friday["teams"], friday["consoles"], friday["games"]),
                         ("Friday LAN", 4, 3, 3, ["127.0.0.2", "127.0.0.1"], ["Zuma", "Halo 3", "Not Here"]))
        self.assertTrue(re.match(r"^[0-9a-f]{12}$", friday["id"]))
        for body, code in (({"action": "save", "name": ""}, 400), ({"action": "rename", "id": "000000000000", "name": "X"}, 404), ({"action": "nap"}, 400),
                           ({"action": "start", "id": "000000000000"}, 404), ({"action": "save", "id": "000000000000", "name": "X"}, 404)):
            self.assertEqual(self.call("POST", "/api/party/presets", body)[0], code, body)
        # Another party in its place, then Friday again with one tap: as saved, and on.
        self.json("POST", "/api/party", {"consoles": [], "gamemaster": "127.0.0.1", "teams": 2, "guests": [{"name": "Nora", "here": True}], "together": [], "games": [], "name": "Other"})
        self.json("POST", "/api/party/presets", {"action": "save", "name": "Small"})
        self.assertEqual(list(presets()), ["Small", "Friday"])  # Newest first.
        r = self.json("POST", "/api/party/presets", {"action": "start", "id": friday["id"]})
        self.assertEqual(r["said"], "Friday is on: 3 players here, 2 consoles, 3 games.")
        p = party()
        self.assertEqual((p["active"], p["name"], p["gamemaster"], p["consoles"], p["teams"], p["balance"], p["games"], p["idleOffMinutes"]),
                         (True, "Friday LAN", "127.0.0.2", ["127.0.0.2", "127.0.0.1"], 3, "wins", ["Zuma", "Halo 3", "Not Here"], 30))
        self.assertEqual([(g["name"], g["here"]) for g in p["guests"]], [("Ola", True), ("Kari", True), ("Lars", True), ("Tom", False)])
        self.assertIn("Friday is on!", [x["text"] for x in p["feed"]])
        # The vote offers tonight's games every party console has, in their order.
        said = self.json("POST", "/api/party/vote", {"action": "open", "seconds": 60})["said"]
        self.assertEqual(said, "Vote for the next game: 2 games, 60 seconds.")
        self.assertEqual([g["name"] for g in party()["vote"]["games"]], ["Zuma", "Halo 3"])
        self.json("POST", "/api/party/vote", {"action": "cancel"})
        # While a party is on, starting another asks first (end: true ends it).
        small = presets()["Small"]
        status, _, data = self.call("POST", "/api/party/presets", {"action": "start", "id": small["id"]})
        self.assertEqual((status, json.loads(data)["error"]), (409, "A party is on: end it first, or start Small in its place."))
        self.json("POST", "/api/party/presets", {"action": "start", "id": small["id"], "end": True})
        p = party()
        self.assertEqual((p["active"], p["name"], [g["name"] for g in p["guests"]], p["games"], p["consoles"]), (True, "Other", ["Nora"], [], ["127.0.0.1"]))
        # Renamed and deleted; Friday saved again in place keeps its id.
        self.assertEqual(self.call("POST", "/api/party/presets", {"action": "rename", "id": small["id"], "name": "friday"})[0], 409)
        self.json("POST", "/api/party/presets", {"action": "rename", "id": small["id"], "name": "Tiny"})
        self.json("POST", "/api/party/presets", {"action": "save", "id": friday["id"], "name": "Friday"})
        self.assertEqual((list(presets()), presets()["Friday"]["id"], presets()["Friday"]["guests"]), (["Friday", "Tiny"], friday["id"], 1))
        self.json("POST", "/api/party/presets", {"action": "delete", "id": small["id"]})
        self.assertEqual(list(presets()), ["Friday"])
        # Kept in the settings file across a restart.
        stop(server)
        server, _ = self.run_server("--xbox", "127.0.0.2", "--web-port", str(self.web), "--aurora-web-port", str(aurora.port), "--config", config)
        self.addCleanup(stop, server)
        self.assertEqual([(x["name"], x["id"]) for x in party()["presets"]], [("Friday", friday["id"])])
        self.assertEqual(party()["games"], [])

    def test_join_and_vote_from_a_phone(self):
        # Marcus 2026-11-02: guests join and vote from their phones, through
        # the party relay on Cloudflare (relay/), which NetDisc360 posts the
        # party to; here a stand-in for it on the PC.
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        self.add_iso("Skate 3.iso", 0x5C087C2C, 0x454108E6)
        posts, asks, refuse = [], [], []

        class Relay(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                posts.append((self.path, self.headers["Authorization"], body))
                status, out = (403, {"ok": False, "error": "Another NetDisc360 has this party code."}) if refuse else (200, {"ok": True, "asks": asks[:]})
                if refuse:
                    refuse.pop()
                else:
                    asks.clear()
                data = json.dumps(out).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def log_message(self, *args):
                pass

        relay = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Relay)
        threading.Thread(target=relay.serve_forever, daemon=True).start()
        self.addCleanup(relay.server_close)
        self.addCleanup(relay.shutdown)
        url = "http://127.0.0.1:%d" % relay.server_address[1]
        self.web = free_port()
        server, _ = self.run_server("--web-port", str(self.web))
        try:
            self.assertTrue(self.wait_for(lambda: all(g["tiles"] == ["127.0.0.1"] for g in self.json("GET", "/api/games")["games"]), 20))
            for relay_url, words in [("ftp://example.com", "The relay's address starts with https://."), ("https://a b.dev", "The relay's address can't have spaces in it."),
                                     ("https://:443", "The relay's address has no host name.")]:
                status, _, data = self.call("POST", "/api/party", {"phoneRelay": relay_url})
                self.assertEqual((status, json.loads(data)["error"]), (400, words))
            self.json("POST", "/api/party", {"phoneRelay": url + "/"})
            phone = self.json("GET", "/api/state")["party"]["phone"]
            first = phone["code"]
            self.assertRegex(first, r"^[A-HJ-NP-Z2-9]{6}$")
            self.assertEqual((phone["relay"], phone["url"]), (url, url + "/" + first))
            # No party yet: posted once, as not on.
            self.assertTrue(self.wait_for(lambda: posts, 10))
            path, key, body = posts[0]
            self.assertEqual((path, body["party"]["active"]), ("/api/room/%s/host" % first, False))
            self.assertRegex(key, r"^Bearer [0-9a-f]{64}$")
            time.sleep(3)
            self.assertEqual(len(posts), 1)
            # The party starts with a new code, posted every 3 seconds as phones see it.
            self.json("POST", "/api/party", {"active": True, "name": "Fall LAN", "gamemaster": "127.0.0.1", "guests": [{"name": "Ann", "here": True}]})
            code = self.json("GET", "/api/state")["party"]["phone"]["code"]
            self.assertNotEqual(code, first)
            room = "/api/room/%s/host" % code
            self.assertTrue(self.wait_for(lambda: len([p for p in posts if p[0] == room]) >= 2, 10))
            self.assertEqual(posts[-1][1], key)  # The same key, for each code.
            party = posts[-1][2]["party"]
            self.assertEqual((party["active"], party["name"], party["guests"], party["teams"], party["vote"], party["points"]), (True, "Fall LAN", ["Ann"], [], None, []))
            self.assertNotIn("127.0.0", json.dumps(posts[-1][2]))  # No addresses.
            self.assertTrue(self.wait_for(lambda: self.json("GET", "/api/state")["party"]["phone"]["ok"], 5))
            # A phone joins; its answer goes back with the next post.
            asks.append({"id": "a1", "phone": "phone-one-1234", "kind": "join", "name": "Bea"})
            asks.append({"id": "a2", "phone": "phone-two-5678", "kind": "join", "name": "x" * 40})
            answers = lambda: [a for p in posts for a in p[2].get("answers", [])]
            self.assertTrue(self.wait_for(lambda: len(answers()) == 2, 10))
            self.assertEqual(answers()[0], {"id": "a1", "phone": "phone-one-1234", "ok": True, "said": "Welcome, Bea!", "name": "Bea"})
            self.assertEqual((answers()[1]["ok"], answers()[1]["said"]), (False, "Your name is 40 characters long; 32 at most."))
            party = self.json("GET", "/api/state")["party"]
            self.assertIn({"name": "Bea", "here": True}, [{"name": g["name"], "here": g["here"]} for g in party["guests"]])
            self.assertEqual(party["phone"]["joins"], 1)
            self.assertTrue(self.wait_for(lambda: "Bea" in posts[-1][2]["party"]["guests"], 10))
            # A vote from a phone counts, for the vote it was cast in only.
            self.json("POST", "/api/party/vote", {"action": "open", "seconds": 60})
            vote = self.json("GET", "/api/state")["party"]["vote"]
            self.assertTrue(self.wait_for(lambda: (posts[-1][2]["party"]["vote"] or {}).get("id") == vote["id"], 10))
            self.assertEqual([g["name"] for g in posts[-1][2]["party"]["vote"]["games"]], [g["name"] for g in vote["games"]])
            asks.append({"id": "v1", "phone": "phone-one-1234", "kind": "vote", "vote": vote["id"], "game": 1})
            asks.append({"id": "v2", "phone": "phone-two-5678", "kind": "vote", "vote": vote["id"] - 1, "game": 0})
            self.assertTrue(self.wait_for(lambda: self.json("GET", "/api/state")["party"]["vote"]["voters"] == 1, 10))
            self.assertEqual([g["votes"] for g in self.json("GET", "/api/state")["party"]["vote"]["games"]], [0, 1])
            self.json("POST", "/api/party/vote", {"action": "cancel"})
            # The relay refuses (another NetDisc360 has the code): the dashboard says so; a new code mends it.
            refuse.append(True)
            self.assertTrue(self.wait_for(lambda: self.json("GET", "/api/state")["party"]["phone"]["error"] == "Another NetDisc360 has this party code; make a new one.", 10))
            self.json("POST", "/api/party", {"phoneCode": "new"})
            self.assertTrue(self.wait_for(lambda: self.json("GET", "/api/state")["party"]["phone"]["ok"], 15))
            code = self.json("GET", "/api/state")["party"]["phone"]["code"]
            # The party ends: posted once more, as over, then left alone.
            self.json("POST", "/api/party", {"active": False})
            self.assertTrue(self.wait_for(lambda: posts[-1][0] == "/api/room/%s/host" % code and not posts[-1][2]["party"]["active"], 10))
            count = len(posts)
            time.sleep(4)
            self.assertEqual(len(posts), count)
            # Turned off: the relay isn't posted to.
            self.json("POST", "/api/party", {"phoneRelay": ""})
            self.assertEqual(self.json("GET", "/api/state")["party"]["phone"], {"relay": "", "code": "", "url": "", "ok": False, "error": "", "joins": 0})
        finally:
            server.kill()
            out = server.communicate()[0]
        self.assertIn("LAN party: Bea joined from a phone.", out)
        self.assertIn("Another NetDisc360 has this party code; make a new one.", out)

    def test_starts_a_game_on_every_party_console(self):
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        self.add_iso("Skate 3.iso", 0x5C087C2C, 0x454108E6)
        other, ftp2 = self.second_console()
        self.install("Games/Skate 3", 0x5C087C2C, 0x454108E6, xbox=other)  # On the second console only.
        # Halo 3 runs Title Update 8 on the first console and none on the second.
        title_update_dbs(os.path.join(self.xbox, "Hdd1", "Apps", "Aurora", "Data", "Databases"),
                         [(5, 0x4D5307E6, 0x1A2B3C4D, 0, 8, "Title Update 8", "tu00000008_00000000")], [5])
        title_update_dbs(os.path.join(other, "Hdd1", "Apps", "Aurora", "Data", "Databases"), [], [])
        aurora = AuroraWeb()
        aurora2 = AuroraWeb(host="127.0.0.2", port=aurora.port)
        self.web = free_port()
        server, _ = self.run_server("--xbox", "127.0.0.2", "--web-port", str(self.web), "--aurora-web-port", str(aurora.port))
        try:
            self.assertTrue(self.wait_for(lambda: self.ftp.scans and ftp2.scans, 20))
            games = {g["name"]: g for g in self.json("GET", "/api/games")["games"]}
            self.assertEqual((games["Skate 3"]["tiles"], games["Skate 3"]["installedOn"]), (["127.0.0.1"], ["127.0.0.2"]))
            # The Gamemaster console is chosen, and goes first.
            self.json("POST", "/api/party", {"active": True, "gamemaster": "127.0.0.2", "consoles": ["127.0.0.1"]})
            self.assertEqual(self.json("GET", "/api/state")["party"]["consoles"], ["127.0.0.2", "127.0.0.1"])
            # Each console starts it from its installed copy if it has one, else from its tile.
            done = self.json("POST", "/api/party/start", {"game": "Skate 3"})
            self.assertEqual([(r["address"], r["ok"], r["how"], r["error"]) for r in done["results"]],
                             [("127.0.0.2", True, "installed", ""), ("127.0.0.1", True, "tile", "")])
            self.assertEqual(done["copy"], "")  # The first console has no copy but the tile.
            self.assertEqual(aurora2.launched, [("\\Device\\Harddisk0\\Partition1\\Games\\Skate 3", "default.xex", "0")])
            self.assertEqual(aurora.launched, [("\\Device\\Harddisk0\\Partition1\\NetDisc360\\Games\\Skate 3", "default.xex", "0")])
            self.assertEqual(done["titleUpdates"], {"mismatch": False, "byConsole": {"127.0.0.2": "none", "127.0.0.1": "none"}, "newest": "", "unread": []})
            # A console in a game: the page is told (409) and asks once, then it starts with confirm.
            aurora.running = "\\Device\\Harddisk0\\Partition1\\NetDisc360\\Games\\Skate 3\\default.xex"
            aurora.launched.clear()
            aurora2.launched.clear()
            status, _, data = self.call("POST", "/api/party/start", {"game": "Halo 3"})
            self.assertEqual(status, 409)
            self.assertEqual(json.loads(data)["busy"], [{"address": "127.0.0.1", "title": "Skate 3"}])
            self.assertIn("in a game", json.loads(data)["error"])
            self.assertEqual((aurora.launched, aurora2.launched), ([], []))
            done = self.json("POST", "/api/party/start", {"game": "Halo 3", "confirm": True})
            self.assertEqual([(r["address"], r["ok"], r["how"]) for r in done["results"]], [("127.0.0.2", True, "tile"), ("127.0.0.1", True, "tile")])
            self.assertEqual(aurora.launched, [("\\Device\\Harddisk0\\Partition1\\NetDisc360\\Games\\Halo 3", "default.xex", "0")])
            self.assertEqual(aurora2.launched, [("\\Device\\Harddisk0\\Partition1\\NetDisc360\\Games\\Halo 3", "default.xex", "0")])
            # They run different title updates: a warning in the answer.
            self.assertEqual(done["titleUpdates"], {"mismatch": True, "byConsole": {"127.0.0.2": "none", "127.0.0.1": "Title Update 8"},
                                                    "newest": "Title Update 8", "unread": []})
            # Title Update 8 put to use on the second console in Aurora: Start
            # on all looks at the title updates again before it starts.
            title_update_dbs(os.path.join(other, "Hdd1", "Apps", "Aurora", "Data", "Databases"),
                             [(1, 0x4D5307E6, 0x1A2B3C4D, 0, 8, "Title Update 8", "tu00000008_00000000")], [1])
            done = self.json("POST", "/api/party/start", {"game": "Halo 3", "confirm": True})
            self.assertEqual(done["titleUpdates"], {"mismatch": False, "byConsole": {"127.0.0.2": "Title Update 8", "127.0.0.1": "Title Update 8"},
                                                    "newest": "Title Update 8", "unread": []})
            # One that doesn't answer fails alone; consoles can be named.
            aurora2.close()
            done = self.json("POST", "/api/party/start", {"game": "Halo 3", "consoles": ["127.0.0.1", "127.0.0.2"], "confirm": True})
            results = {r["address"]: r for r in done["results"]}
            self.assertTrue(results["127.0.0.1"]["ok"])
            self.assertFalse(results["127.0.0.2"]["ok"])
            self.assertIn("doesn't answer", results["127.0.0.2"]["error"])
            for body, words in [({"game": "Gears"}, b"isn't in your folders"), ({"game": "Halo 3", "consoles": ["127.0.0.9"]}, b"isn't a console"),
                                ({}, b"Send the game")]:
                status, _, data = self.call("POST", "/api/party/start", body)
                self.assertEqual(status, 400)
                self.assertIn(words, data)
        finally:
            aurora.close()
            aurora2.close()
            server.kill()
            server.communicate()

    def test_makes_keeps_and_gives_back_guest_profiles(self):
        # The Gamemaster's console makes each guest's profile (the helper's
        # job, started from Aurora); the server copies it to the others.
        # Removing keeps a copy here first; the same name gets it back.
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        other, ftp2 = self.second_console()
        for root in (self.xbox, other):
            os.makedirs(os.path.join(root, "Hdd1", "Content", "0000000000000000"))
        aurora = AuroraWeb()
        aurora2 = AuroraWeb(host="127.0.0.2", port=aurora.port)
        made, pictures = [], []

        def package(gamertag):
            name = gamertag.encode("utf-16-be")
            # Bytes above 0x7F too, as a real (signed) package has.
            return b"CON " + bytes(range(256)) + bytes(0x411 - 4 - 256) + name + bytes(0xA000 - 0x411 - len(name))

        def helper(path, exe):
            # The helper as the console runs it: the job in its folder, a
            # profile (and a save) for each gamertag, its answer back.
            folder = os.path.join(other, "Hdd1", "Apps", "NetDisc360")
            with open(os.path.join(folder, "profiles.job")) as f:
                job = dict(line.split("=", 1) for line in f.read().splitlines() if "=" in line and not line.startswith(";"))
            os.remove(os.path.join(folder, "profiles.job"))
            answer = "job=" + job["job"] + "\r\n"
            for key in sorted(k for k in job if k.startswith("guest")):
                xuid = "E000%04X5EEDF00D" % (0x100 + len(made))
                made.append((job[key], xuid))
                # Its gamer picture beside the job: the PNGs, 64 and 32 pixels square.
                n = key[5:]
                for name, size in ((job["picture" + n], 64), (job["picture_small" + n], 32)):
                    with open(os.path.join(folder, name), "rb") as f:
                        png = f.read()
                    pictures.append((job[key], name, png[:8], struct.unpack(">II", png[16:24])))
                    self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
                    self.assertEqual(struct.unpack(">II", png[16:24]), (size, size))
                answer += "picture" + n + "=ok\r\n" if n == "1" else "picture_error" + n + "=XamProfileOpen with flags 3: 80070057\r\n"
                content = os.path.join(other, "Hdd1", "Content", xuid)
                os.makedirs(os.path.join(content, "FFFE07D1", "00010000"))
                os.makedirs(os.path.join(content, "4D5307E6", "00000001"))
                # A profile xam has just made is named for its XUID; the
                # gamertag is only in its (encrypted) Account file.
                with open(os.path.join(content, "FFFE07D1", "00010000", xuid), "wb") as f:
                    f.write(package(xuid if len(made) % 2 else job[key]))
                with open(os.path.join(content, "4D5307E6", "00000001", "save"), "wb") as f:
                    f.write(b"halo save " + xuid.encode())
                answer += key + "=" + xuid + "\r\n"
            with open(os.path.join(folder, "profiles.done"), "w", newline="") as f:
                f.write(answer)
        aurora2.on_launch = helper
        config = os.path.join(self.tmp.name, "profiles.ini")
        self.web = free_port()
        server, _ = self.run_server("--xbox", "127.0.0.2", "--web-port", str(self.web), "--aurora-web-port", str(aurora.port), "--config", config, auto_profiles=True)

        def finished():
            self.assertTrue(self.wait_for(lambda: not self.json("GET", "/api/party/profiles")["work"]["running"], 60))
            return self.json("GET", "/api/party/profiles")

        def has(root, xuid):
            return os.path.exists(os.path.join(root, "Hdd1", "Content", xuid, "FFFE07D1", "00010000", xuid))
        try:
            self.assertTrue(self.wait_for(lambda: self.ftp.scans and ftp2.scans, 20))
            self.json("POST", "/api/party", {"gamemaster": "127.0.0.2", "consoles": ["127.0.0.1"],
                                             "guests": [{"name": "Ståle Øvrebø", "here": True}, {"name": "Kristoffer Hansen", "here": False}]})
            r = self.json("GET", "/api/party/profiles")
            self.assertEqual([(g["name"], g["gamertag"], g["xuid"], g["on"]) for g in r["profiles"]],
                             [("Ståle Øvrebø", "Stale Ovrebo", "", []), ("Kristoffer Hansen", "Kristoffer H", "", [])])
            # Made on the Gamemaster's console, copied to the other with the saves.
            self.json("POST", "/api/party/profiles", {"action": "make"})
            self.assertEqual(self.call("POST", "/api/party/profiles", {"action": "make"})[0], 409)  # One at a time.
            r = finished()
            self.assertEqual(r["work"]["error"], "")
            self.assertIn("2 guests have a profile on every console", r["work"]["result"])
            self.assertEqual(aurora2.launched, [("\\Device\\Harddisk0\\Partition1\\Apps\\NetDisc360", "boot.xex", "0")])
            self.assertEqual(aurora.launched, [])
            self.assertEqual([g for g, _ in made], ["Stale Ovrebo", "Kristoffer H"])
            xuids = [x for _, x in made]
            self.assertEqual([(g["xuid"], sorted(g["on"]), g["madeOn"]) for g in r["profiles"]],
                             [(x, ["127.0.0.1", "127.0.0.2"], "127.0.0.2") for x in xuids])
            for xuid in xuids:
                self.assertTrue(has(self.xbox, xuid) and has(other, xuid))
                with open(os.path.join(self.xbox, "Hdd1", "Content", xuid, "4D5307E6", "00000001", "save"), "rb") as f:
                    self.assertEqual(f.read(), b"halo save " + xuid.encode())
            self.assertFalse(os.path.exists(os.path.join(other, "Hdd1", "Apps", "NetDisc360", "profiles.done")))
            # The pictures went with the job and are gone again; one the helper
            # couldn't give is only noted (the profile is made all the same).
            self.assertEqual([(g, f) for g, f, _, _ in pictures], [("Stale Ovrebo", "guest1.png"), ("Stale Ovrebo", "guest1_32.png"), ("Kristoffer H", "guest2.png"), ("Kristoffer H", "guest2_32.png")])
            self.assertEqual([f for f in os.listdir(os.path.join(other, "Hdd1", "Apps", "NetDisc360")) if f.endswith(".png")], [])
            # The page shows each guest's picture.
            status, response, png = self.call("GET", "/api/party/profiles/picture?gamertag=Stale%20Ovrebo")
            self.assertEqual((status, response.getheader("Content-Type"), png[:8], struct.unpack(">II", png[16:24])), (200, "image/png", b"\x89PNG\r\n\x1a\n", (64, 64)))
            idat = png.index(b"IDAT")
            pixels = zlib.decompress(png[idat + 4:idat + 4 + struct.unpack(">I", png[idat - 4:idat])[0]])
            self.assertEqual(len(pixels), 64 * (64 * 3 + 1))  # 8-bit RGB rows, each with its filter byte.
            self.assertGreater(len(set(pixels[1 + 64 * 3 * 32:64 * 3 * 33])), 2)  # The letters across the middle.
            self.assertEqual(self.call("GET", "/api/party/profiles/picture?gamertag=Stale%20Ovrebo", page=False)[0], 403)
            self.assertEqual(self.call("GET", "/api/party/profiles/picture?gamertag=")[0], 400)
            # Asked again: they're there, nothing to make.
            self.json("POST", "/api/party/profiles", {"action": "make"})
            finished()
            self.assertEqual(len(aurora2.launched), 1)
            # A console's own profiles are never removed: a copy named otherwise stays.
            own = os.path.join(self.xbox, "Hdd1", "Content", xuids[1], "FFFE07D1", "00010000", xuids[1])
            with open(own, "wb") as f:
                f.write(package("Someone Else"))
            self.json("POST", "/api/party/profiles", {"action": "remove"})
            r = finished()
            self.assertIn("isn't the profile NetDisc360 made", r["work"]["error"])
            self.assertIn("3 guest profiles removed, 3 copies kept", r["work"]["result"])
            self.assertFalse(has(self.xbox, xuids[0]) or has(other, xuids[0]) or has(other, xuids[1]))
            self.assertTrue(has(self.xbox, xuids[1]))
            self.assertEqual([(g["on"], g["copies"]) for g in r["profiles"]], [([], 2), (["127.0.0.1"], 1)])
            kept = os.path.join(self.tmp.name, "Backups", "Profiles", xuids[0])
            self.assertEqual(sorted(n[-4:] for n in os.listdir(kept)), [".ini", ".ini", ".zip", ".zip"])
            with open(config, encoding="utf-8") as f:
                self.assertIn("guest_profile1=Ståle Øvrebø|Stale Ovrebo|" + xuids[0] + "|", f.read())
            # Ståle comes back: the kept copy (with the save) goes on both, no new profile.
            self.json("POST", "/api/party/profiles", {"action": "make", "names": ["Ståle Øvrebø"]})
            r = finished()
            self.assertEqual(r["work"]["error"], "")
            self.assertIn("Ståle Øvrebø gets their profile back", r["work"]["result"])
            self.assertEqual(len(aurora2.launched), 1)
            self.assertTrue(has(self.xbox, xuids[0]) and has(other, xuids[0]))
            with open(os.path.join(other, "Hdd1", "Content", xuids[0], "4D5307E6", "00000001", "save"), "rb") as f:
                self.assertEqual(f.read(), b"halo save " + xuids[0].encode())
            for body, code in (({"action": "make", "names": ["Nobody"]}, 400), ({"action": "forget", "names": ["Ståle Øvrebø"]}, 409), ({"action": "nap"}, 400)):
                self.assertEqual(self.call("POST", "/api/party/profiles", body)[0], code)
            # A name put on the list gets its profile by itself, a few seconds later, on both.
            self.json("POST", "/api/party", {"guests": [{"name": "Ståle Øvrebø", "here": True}, {"name": "Kristoffer Hansen", "here": False}, {"name": "Nora", "here": True}]})
            self.assertTrue(self.wait_for(lambda: any(g["name"] == "Nora" and g["xuid"] and len(g["on"]) == 2 for g in self.json("GET", "/api/party/profiles")["profiles"]), 40))
            self.assertEqual(made[-1][0], "Nora")
            self.assertEqual(len(aurora2.launched), 2)
        finally:
            aurora.close()
            aurora2.close()
            server.kill()
            server.communicate()

    def test_users_keep_one_profile_each_on_any_console(self):
        # The Users page: everyone NetDisc360 knows, apart from the party (its
        # guests are users too). A user's profile goes on the consoles asked
        # for, is backed up and removed there; profiles.ini on each console
        # says which of its profiles are NetDisc360's and whose; users.ini
        # (beside the settings) merges another computer's.
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        other, ftp2 = self.second_console()
        for root in (self.xbox, other):
            os.makedirs(os.path.join(root, "Hdd1", "Content", "0000000000000000"))
        aurora = AuroraWeb()
        aurora2 = AuroraWeb(host="127.0.0.2", port=aurora.port)
        self.addCleanup(aurora.close)
        self.addCleanup(aurora2.close)
        made = []

        def helper(path, exe):
            folder = os.path.join(other, "Hdd1", "Apps", "NetDisc360")
            with open(os.path.join(folder, "profiles.job")) as f:
                job = dict(line.split("=", 1) for line in f.read().splitlines() if "=" in line and not line.startswith(";"))
            os.remove(os.path.join(folder, "profiles.job"))
            answer = "job=" + job["job"] + "\r\n"
            for key in sorted(k for k in job if k.startswith("guest")):
                xuid = "E000%04X5EEDF00D" % (0x200 + len(made))
                made.append((job[key], xuid))
                content = os.path.join(other, "Hdd1", "Content", xuid)
                os.makedirs(os.path.join(content, "FFFE07D1", "00010000"))
                name = job[key].encode("utf-16-be")
                with open(os.path.join(content, "FFFE07D1", "00010000", xuid), "wb") as f:
                    f.write(b"CON " + bytes(0x411 - 4) + name + bytes(0xA000 - 0x411 - len(name)))
                answer += key + "=" + xuid + "\r\n"
            with open(os.path.join(folder, "profiles.done"), "w", newline="") as f:
                f.write(answer)
        aurora2.on_launch = helper
        config = os.path.join(self.tmp.name, "netdisc.ini")
        users_ini = os.path.join(self.tmp.name, "users.ini")
        self.web = free_port()

        def stop(server):
            server.kill()
            try:
                server.communicate()
            except ValueError:  # Stopped already.
                pass
        server, _ = self.run_server("--xbox", "127.0.0.2", "--web-port", str(self.web), "--aurora-web-port", str(aurora.port), "--config", config)
        self.addCleanup(stop, server)

        def users():
            return {u["name"]: u for u in self.json("GET", "/api/users")["users"]}

        def finished():
            self.assertTrue(self.wait_for(lambda: not self.json("GET", "/api/users")["work"]["running"], 60))
            r = self.json("GET", "/api/users")
            self.assertEqual(r["work"]["error"], "")
            return {u["name"]: u for u in r["users"]}

        def lines():
            with open(users_ini, encoding="utf-8") as f:
                return {v.split("|")[0]: v.split("|") for k, v in (l.split("=", 1) for l in f.read().splitlines() if l.startswith("user"))}

        def marks(root):
            path = os.path.join(root, "Hdd1", "Apps", "NetDisc360", "profiles.ini")
            if not os.path.exists(path):
                return {}
            with open(path, encoding="utf-8") as f:
                return {k: v for k, v in (l.split("=", 1) for l in f.read().splitlines() if l[:1] == "E" and "=" in l)}
        self.assertTrue(self.wait_for(lambda: self.ftp.scans and ftp2.scans, 20))
        # The party's guests are users (the list made them).
        self.json("POST", "/api/party", {"gamemaster": "127.0.0.2", "guests": [{"name": "Ola", "here": True}, {"name": "Kari", "here": False}]})
        u = users()
        self.assertEqual(sorted(u), ["Kari", "Ola"])
        self.assertTrue(all(re.match(r"^u[0-9a-f]{16}$", x["id"]) for x in u.values()))
        self.assertEqual((u["Ola"]["guest"], u["Ola"]["here"], u["Kari"]["here"], u["Ola"]["xuid"], u["Ola"]["gamertag"]), (True, True, False, "", "Ola"))
        self.assertTrue(self.wait_for(lambda: os.path.exists(users_ini) and len(lines()) == 2, 10))
        # One apart from the party; a name can't be two users.
        nora = self.json("POST", "/api/users", {"action": "add", "name": "Nora"})
        nora = next(x for x in nora["users"] if x["name"] == "Nora")
        self.assertFalse(nora["guest"])
        for body, code in (({"action": "add", "name": "nora"}, 409), ({"action": "add", "name": ""}, 400), ({"action": "nap"}, 400),
                           ({"action": "remove", "id": "u0000000000000000"}, 404), ({"action": "import", "text": "hello"}, 400)):
            self.assertEqual(self.call("POST", "/api/users", body)[0], code, body)
        self.assertEqual(self.call("POST", "/api/users/profiles", {"action": "backup", "ids": [nora["id"]]})[0], 400)  # No profile yet.
        self.assertEqual(self.call("POST", "/api/users/profiles", {"action": "put", "ids": [nora["id"]], "consoles": ["10.9.9.9"]})[0], 400)
        # Nora's profile on one console: it makes it.
        self.json("POST", "/api/users/profiles", {"action": "put", "ids": [nora["id"]], "consoles": ["127.0.0.2"]})
        u = finished()
        self.assertEqual(made, [("Nora", u["Nora"]["xuid"])])
        xuid = u["Nora"]["xuid"]
        self.assertEqual((u["Nora"]["on"], u["Nora"]["madeOn"], u["Nora"]["adopted"]), (["127.0.0.2"], "127.0.0.2", False))
        self.assertEqual(marks(other), {xuid: nora["id"] + "|Nora|Nora|" + str(u["Nora"]["made"]) + "|0"})
        self.assertEqual(marks(self.xbox), {})
        self.assertTrue(self.wait_for(lambda: lines()[nora["id"]][3] == xuid, 10))
        # Then on the other: copied, not made again; both say whose it is.
        self.json("POST", "/api/users/profiles", {"action": "put", "ids": [nora["id"]], "consoles": ["127.0.0.1"]})
        u = finished()
        self.assertEqual(len(made), 1)
        self.assertEqual(sorted(u["Nora"]["on"]), ["127.0.0.1", "127.0.0.2"])
        self.assertTrue(os.path.exists(os.path.join(self.xbox, "Hdd1", "Content", xuid, "FFFE07D1", "00010000", xuid)))
        self.assertEqual(list(marks(self.xbox)), [xuid])
        # Backed up from both: each copy says whose it is.
        self.json("POST", "/api/users/profiles", {"action": "backup", "ids": [nora["id"]]})
        u = finished()
        self.assertEqual(u["Nora"]["copies"], 2)
        kept = os.path.join(self.tmp.name, "Backups", "Profiles", xuid)
        for name in os.listdir(kept):
            if name.endswith(".ini"):
                with open(os.path.join(kept, name), encoding="utf-8") as f:
                    self.assertIn("user=" + nora["id"], f.read())
        # Not forgotten while it's on a console; removed from one, the other keeps it.
        self.assertEqual(self.call("POST", "/api/users", {"action": "remove", "id": nora["id"]})[0], 409)
        self.json("POST", "/api/users/profiles", {"action": "remove", "ids": [nora["id"]], "consoles": ["127.0.0.1"]})
        u = finished()
        self.assertEqual(u["Nora"]["on"], ["127.0.0.2"])
        self.assertFalse(os.path.exists(os.path.join(self.xbox, "Hdd1", "Content", xuid)))
        self.assertEqual(marks(self.xbox), {})
        # A line another computer wrote on the console stays.
        path = os.path.join(other, "Hdd1", "Apps", "NetDisc360", "profiles.ini")
        with open(path, "a", encoding="utf-8", newline="") as f:
            f.write("E0000000000AB0CD=u00000000000000aa|Per|Per|1|1\r\n")
        self.json("POST", "/api/users/profiles", {"action": "remove", "ids": [nora["id"]]})
        u = finished()
        self.assertEqual(u["Nora"]["on"], [])
        self.assertEqual(marks(other), {"E0000000000AB0CD": "u00000000000000aa|Per|Per|1|1"})
        # Forgotten: off the users, the line stays (removed, no profile) so the
        # other computers forget them too; the kept copies stay.
        self.json("POST", "/api/users", {"action": "remove", "id": nora["id"]})
        self.assertNotIn("Nora", users())
        self.assertTrue(self.wait_for(lambda: lines()[nora["id"]][7] != "0", 10))
        self.assertEqual(lines()[nora["id"]][1:4], ["Nora", "", ""])
        # Backed up from both, and before each removal (a copy from the same second and console is the same one).
        self.assertGreaterEqual(len([n for n in os.listdir(kept) if n.endswith(".zip")]), 3)
        # Another computer's users.ini: Per is new (with a profile), Ola was
        # renamed there later, Kari was forgotten there.
        ola, kari = users()["Ola"]["id"], users()["Kari"]["id"]
        later = int(time.time()) + 100
        text = ("[Users]\r\ncount=3\r\n"
                "user1=u00000000000000aa|Per|Per|E0000000000AB0CD|1700000000|0|%d|0|1200|30|0|3600|2|1|0|Halo 3\r\n"
                "user2=%s|Ola Nordmann||||0|%d|0|-1|-1|0|0|0|0|0|\r\n"
                "user3=%s|Kari||||0|%d|%d|-1|-1|0|0|0|0|0|\r\n") % (later, ola, later, kari, later, later)
        self.json("POST", "/api/users", {"action": "import", "text": text})
        u = users()
        self.assertEqual(sorted(u), ["Ola Nordmann", "Per"])
        self.assertEqual((u["Per"]["xuid"], u["Per"]["stats"]["gamerscore"], u["Per"]["stats"]["playSeconds"], u["Per"]["on"]), ("E0000000000AB0CD", 1200, 3600, []))
        self.assertEqual(u["Ola Nordmann"]["id"], ola)
        self.assertEqual([g["name"] for g in self.json("GET", "/api/state")["party"]["guests"]], ["Ola Nordmann"])
        # A profile made on a console by hand (signed in there) becomes a user's.
        r = self.json("POST", "/api/users", {"action": "adopt", "console": "127.0.0.1", "xuid": "e0000000000ab0ce", "gamertag": "Vik"})
        viktor = next(x for x in r["users"] if x["name"] == "Vik")
        self.assertEqual((viktor["xuid"], viktor["on"], viktor["adopted"], viktor["madeOn"]), ("E0000000000AB0CE", ["127.0.0.1"], True, "127.0.0.1"))
        self.assertEqual(marks(self.xbox), {"E0000000000AB0CE": viktor["id"] + "|Vik|Vik|" + str(viktor["made"]) + "|1"})
        for body, code in (({"console": "127.0.0.1", "xuid": "E0000000000AB0CE", "gamertag": "Vik", "name": "Someone"}, 409),
                           ({"console": "127.0.0.1", "xuid": "0009000000000001", "gamertag": "Online"}, 400),
                           ({"console": "10.9.9.9", "xuid": "E0000000000AB0CF", "gamertag": "Who"}, 400)):
            self.assertEqual(self.call("POST", "/api/users", dict(body, action="adopt"))[0], code, body)
        # Kept across a restart: the records come from users.ini.
        stop(server)
        server, _ = self.run_server("--xbox", "127.0.0.2", "--web-port", str(self.web), "--aurora-web-port", str(aurora.port), "--config", config)
        self.addCleanup(stop, server)
        self.assertEqual(sorted(users()), ["Ola Nordmann", "Per", "Vik"])
        self.assertEqual((users()["Per"]["xuid"], users()["Vik"]["on"]), ("E0000000000AB0CD", ["127.0.0.1"]))

    def test_the_plugin_makes_profiles_with_aurora_on_screen(self):
        # A console whose NetDisc360.xex plugin takes the job (NetDisc360.job
        # on the drive's root) makes the profiles under Aurora: the helper
        # isn't started. Here a thread does what the plugin does.
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        aurora = AuroraWeb()
        self.addCleanup(aurora.close)
        root = os.path.join(self.xbox, "Hdd1")
        stop, jobs = threading.Event(), []

        def plugin():
            while not stop.is_set():
                path = os.path.join(root, "NetDisc360.job")
                if os.path.exists(path):
                    with open(path) as f:
                        job = dict(line.split("=", 1) for line in f.read().splitlines() if "=" in line and not line.startswith(";"))
                    os.remove(path)
                    jobs.append(job)
                    # The pictures are where folder= says.
                    folder = os.path.join(root, *job["folder"].strip("\\").split("\\"))
                    self.assertTrue(os.path.exists(os.path.join(folder, job["picture1"])))
                    xuid = "E00000000000B%03X" % len(jobs)
                    os.makedirs(os.path.join(root, "Content", xuid, "FFFE07D1", "00010000"))
                    with open(os.path.join(root, "Content", xuid, "FFFE07D1", "00010000", xuid), "wb") as f:
                        f.write(b"CON " + bytes(0x2000))
                    with open(os.path.join(root, "NetDisc360.done"), "w", newline="") as f:
                        f.write("job=%s\r\nby=plugin\r\nguest1=%s\r\npicture1=ok\r\n" % (job["job"], xuid))
                stop.wait(0.3)
        threading.Thread(target=plugin, daemon=True).start()
        self.addCleanup(stop.set)
        config = os.path.join(self.tmp.name, "plugin.ini")
        self.web = free_port()
        server, _ = self.run_server("--web-port", str(self.web), "--aurora-web-port", str(aurora.port), "--config", config, auto_profiles=True)
        try:
            self.assertTrue(self.wait_for(lambda: self.ftp.scans, 20))
            self.json("POST", "/api/party", {"gamemaster": "127.0.0.1", "consoles": ["127.0.0.1"], "guests": [{"name": "Nora", "here": True}]})
            self.assertTrue(self.wait_for(lambda: any(g["xuid"] for g in self.json("GET", "/api/party/profiles")["profiles"]), 40))
            r = self.json("GET", "/api/party/profiles")
            self.assertEqual([(g["name"], g["xuid"], g["on"]) for g in r["profiles"]], [("Nora", "E00000000000B001", ["127.0.0.1"])])
            self.assertEqual(aurora.launched, [])  # Aurora stayed on screen.
            self.assertEqual([j["guest1"] for j in jobs], ["Nora"])
            self.assertFalse(os.path.exists(os.path.join(root, "NetDisc360.done")))
            self.assertEqual([f for f in os.listdir(os.path.join(root, "Apps", "NetDisc360")) if f.endswith(".png")], [])
        finally:
            stop.set()
            server.kill()
            out = server.communicate()[0]
        self.assertIn("making 1 guest profile with its NetDisc360.xex plugin (Aurora stays on screen).", out)

    def test_the_party_app_makes_profiles_while_every_console_shows_it(self):
        # Every console shows NetDiscParty.xex (none can start the helper
        # from Aurora): the Gamemaster's app makes the profile. Here a thread
        # does what the app does: it sees "makeprofiles" in the party, asks
        # for the job and its picture (PartyJobAsk) and answers. A guest who
        # joins signed in to a profile made by hand gets that one (adopted).
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        root = os.path.join(self.xbox, "Hdd1")
        stop, asked = threading.Event(), []
        port_box = {}

        def job_ask(index, body=b""):
            with connect_from("127.0.0.1", port_box["port"]) as conn:
                conn.sendall(b"ISVR" + struct.pack(">HHQI", 0x504A, index, 0, len(body)) + body)
                return exact(conn, struct.unpack(">I", exact(conn, 4))[0])

        def app():
            seen = ""
            while not stop.is_set():
                try:
                    with connect_from("127.0.0.1", port_box["port"]) as conn:
                        conn.sendall(b"ISVR" + struct.pack(">HHQI", 0x5057, 0, 0, 0))
                        text = exact(conn, struct.unpack(">I", exact(conn, 4))[0]).decode("utf-8")
                    job = [l.split("\t")[1] for l in text.splitlines() if l.startswith("makeprofiles\t")]
                    if job and job[0] != seen:
                        seen = job[0]
                        lines = dict(l.split("=", 1) for l in job_ask(0).decode().splitlines() if "=" in l)
                        asked.append((lines, len(job_ask(1, lines["picture1"].encode())), job_ask(1, b"../x")))
                        xuid = "E00000000000C001"
                        os.makedirs(os.path.join(root, "Content", xuid, "FFFE07D1", "00010000"))
                        with open(os.path.join(root, "Content", xuid, "FFFE07D1", "00010000", xuid), "wb") as f:
                            f.write(b"CON " + bytes(0x2000))
                        asked.append(job_ask(2, ("job=%s\r\nby=party\r\nguest1=%s\r\npicture1=ok\r\n" % (lines["job"], xuid)).encode()))
                except OSError:
                    pass
                stop.wait(0.3)
        config = os.path.join(self.tmp.name, "app.ini")
        self.web = free_port()
        server, port_box["port"] = self.run_server("--web-port", str(self.web), "--config", config)
        threading.Thread(target=app, daemon=True).start()
        self.addCleanup(stop.set)
        try:
            self.assertTrue(self.wait_for(lambda: self.ftp.scans, 20))
            self.json("POST", "/api/party", {"active": True, "gamemaster": "127.0.0.1", "consoles": ["127.0.0.1"], "guests": [{"name": "Nora", "here": True}]})
            self.assertTrue(self.wait_for(lambda: "127.0.0.1" in self.json("GET", "/api/state")["party"]["wheelOn"], 10))
            self.json("POST", "/api/party/profiles", {"action": "make"})
            self.assertTrue(self.wait_for(lambda: any(g["xuid"] for g in self.json("GET", "/api/party/profiles")["profiles"]), 40))
            r = self.json("GET", "/api/party/profiles")
            self.assertEqual([(g["name"], g["xuid"], g["on"]) for g in r["profiles"]], [("Nora", "E00000000000C001", ["127.0.0.1"])])
            lines, picture, bad = asked[0]
            self.assertEqual((lines["guest1"], picture > 100, bad), ("Nora", True, b""))
            self.assertEqual(asked[1], b"ok\n")
            self.assertNotIn("makeprofiles", "".join(self.party_text_from("127.0.0.1", port_box["port"])))
            # Joined signed in to a profile of one's own (offline): it's theirs.
            with connect_from("127.0.0.1", port_box["port"]) as conn:
                edit = "join\tGabbi\tE00000000000D00D\tGabbi G".encode()
                conn.sendall(b"ISVR" + struct.pack(">HHQI", 0x5045, 0, 0, len(edit)) + edit)
                self.assertEqual(exact(conn, struct.unpack(">I", exact(conn, 4))[0]), "ok\tWelcome, Gabbi!\n".encode())
            gabbi = [g for g in self.json("GET", "/api/party/profiles")["profiles"] if g["name"] == "Gabbi"]
            self.assertEqual([(g["gamertag"], g["xuid"], g["madeOn"], g["on"]) for g in gabbi], [("Gabbi G", "E00000000000D00D", "127.0.0.1", ["127.0.0.1"])])
        finally:
            stop.set()
            server.kill()
            out = server.communicate()[0]
        self.assertIn("making 1 guest profile with the LAN party app (every console shows it).", out)
        self.assertIn("Gabbi's own profile Gabbi G (E00000000000D00D) is their guest profile now", out)
        with open(config) as f:
            self.assertIn("|127.0.0.1|127.0.0.1|adopted", f.read())

    def party_text_from(self, address, port):
        with connect_from(address, port) as conn:
            conn.sendall(b"ISVR" + struct.pack(">HHQI", 0x5057, 0, 0, 0))
            return exact(conn, struct.unpack(">I", exact(conn, 4))[0]).decode("utf-8")

    def test_copies_profiles_to_the_seats_through_the_wheel_app(self):
        # After a spin or a swap, each seated guest's profile goes to their
        # console: here the second shows the wheel, so through the wheel
        # app's own FTP server (party/ftp.cpp, the real one, on the host).
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        other = os.path.join(self.tmp.name, "xbox2")
        os.makedirs(os.path.join(other, "Hdd1", "Content", "0009000000ABCDEF"))  # The console's own (online) profile.
        app = subprocess.Popen([build_party_ftp(), os.path.join(other, "Hdd1"), "127.0.0.2", str(self.ftp.server_address[1])],
                               stdout=subprocess.PIPE, text=True)
        self.addCleanup(app.stdout.close)
        self.addCleanup(app.wait)
        self.addCleanup(app.kill)
        self.assertEqual(app.stdout.readline().strip(), "ready")
        ola, kari = "E00000000000A001", "E00000000000A002"

        def package(gamertag):
            name = gamertag.encode("utf-16-be")
            return b"CON " + bytes(range(256)) + bytes(0x411 - 4 - 256) + name + bytes(0x2000 - 0x411 - len(name))
        # Ola's profile is on the first console (Aurora's FTP server there);
        # Kari's only in a copy kept on this computer.
        content = os.path.join(self.xbox, "Hdd1", "Content", ola)
        os.makedirs(os.path.join(content, "FFFE07D1", "00010000"))
        os.makedirs(os.path.join(content, "4D5307E6", "00000001"))
        with open(os.path.join(content, "FFFE07D1", "00010000", ola), "wb") as f:
            f.write(package("Ola"))
        with open(os.path.join(content, "4D5307E6", "00000001", "save"), "wb") as f:
            f.write(b"ola's save" * 50000)  # Larger than one chunk.
        kept = os.path.join(self.tmp.name, "Backups", "Profiles", kari)
        os.makedirs(kept)
        with zipfile.ZipFile(os.path.join(kept, "2026-10-30_23-00-00_127.0.0.1.zip"), "w") as z:
            z.writestr("Content/%s/FFFE07D1/00010000/%s" % (kari, kari), package("Kari"))
        with open(os.path.join(kept, "2026-10-30_23-00-00_127.0.0.1.ini"), "w", newline="") as f:
            f.write("file=2026-10-30_23-00-00_127.0.0.1.zip\r\nname=Kari\r\ngamertag=Kari\r\nxuid=%s\r\nconsole=127.0.0.1\r\ntime=1793400000\r\nsize=8192\r\n" % kari)
        config = os.path.join(self.tmp.name, "seats.ini")
        with open(config, "w", newline="") as f:
            f.write("guest_profiles=2\r\nguest_profile1=Ola|Ola|%s|1793400000|127.0.0.1|127.0.0.1\r\nguest_profile2=Kari|Kari|%s|1793400000|127.0.0.1|\r\n" % (ola, kari))
        aurora = AuroraWeb()  # The first console is in Aurora.
        self.addCleanup(aurora.close)
        self.web = free_port()
        server, port = self.run_server("--xbox", "127.0.0.2", "--web-port", str(self.web), "--aurora-web-port", str(aurora.port), "--config", config)
        stop = threading.Event()

        def wheel():
            # The second console's wheel app waits on the server (PartyWait), so it shows the wheel.
            while not stop.is_set():
                try:
                    with connect_from("127.0.0.2", port) as conn:
                        conn.sendall(b"ISVR" + struct.pack(">HHQI", 0x5057, 0, 0, 0))
                        exact(conn, struct.unpack(">I", exact(conn, 4))[0])
                except OSError:
                    pass
                stop.wait(1)
        threading.Thread(target=wheel, daemon=True).start()
        self.addCleanup(stop.set)

        def text_for(address):
            with connect_from(address, port) as conn:
                conn.sendall(b"ISVR" + struct.pack(">HHQI", 0x5057, 0, 0, 0))
                return exact(conn, struct.unpack(">I", exact(conn, 4))[0]).decode("utf-8")
        try:
            self.assertTrue(self.wait_for(lambda: self.ftp.scans, 20))
            self.json("POST", "/api/party", {"active": True, "gamemaster": "127.0.0.1", "consoles": ["127.0.0.1", "127.0.0.2"],
                                             "guests": [{"name": n, "here": True} for n in ("Ola", "Kari", "Lars", "Tom")]})
            self.assertTrue(self.wait_for(lambda: "127.0.0.2" in self.json("GET", "/api/state")["party"]["wheelOn"], 10))
            self.json("POST", "/api/party/seats", {"seats": {"127.0.0.2": ["Ola", "Kari", "Lars"], "127.0.0.1": ["Tom"]}})
            self.assertTrue(self.wait_for(lambda: self.json("GET", "/api/state")["party"]["profilesTo"].get("127.0.0.2", {}).get("state") not in (None, "copying"), 30))
            to = self.json("GET", "/api/state")["party"]["profilesTo"]
            self.assertEqual(to["127.0.0.2"], {"state": "ready", "done": 2, "total": 2, "now": "", "note": "Lars has no profile yet (the LAN party page makes them)"})
            # Every guest here gets their profile on every party console (Marcus 2026-11-02), seated or not.
            self.assertTrue(self.wait_for(lambda: self.json("GET", "/api/state")["party"]["profilesTo"].get("127.0.0.1", {}).get("state") not in (None, "copying"), 30))
            to = self.json("GET", "/api/state")["party"]["profilesTo"]
            self.assertEqual(to["127.0.0.1"], {"state": "ready", "done": 2, "total": 2, "now": "", "note": "Tom has no profile yet (the LAN party page makes them)"})
            with open(os.path.join(self.xbox, "Hdd1", "Content", kari, "FFFE07D1", "00010000", kari), "rb") as f:
                self.assertEqual(f.read(), package("Kari"))
            # Byte for byte, the save too; the console's own profile is untouched.
            for xuid, root in ((ola, self.xbox), (kari, None)):
                package_path = os.path.join("Hdd1", "Content", xuid, "FFFE07D1", "00010000", xuid)
                with open(os.path.join(other, package_path), "rb") as f:
                    self.assertEqual(f.read(), package("Ola" if xuid == ola else "Kari"))
            with open(os.path.join(other, "Hdd1", "Content", ola, "4D5307E6", "00000001", "save"), "rb") as f:
                self.assertEqual(f.read(), b"ola's save" * 50000)
            self.assertEqual(sorted(os.listdir(os.path.join(other, "Hdd1", "Content"))), ["0009000000ABCDEF", ola, kari])
            # The records know where they are now; the wheel app is told.
            profiles = {g["name"]: sorted(g["on"]) for g in self.json("GET", "/api/party/profiles")["profiles"]}
            self.assertEqual(profiles, {"Ola": ["127.0.0.1", "127.0.0.2"], "Kari": ["127.0.0.1", "127.0.0.2"], "Lars": [], "Tom": []})
            self.assertIn("\nprofiles\tready\t2\t2\t\tLars has no profile yet (the LAN party page makes them)\n", text_for("127.0.0.2"))
            # The wheel app's FTP server writes nowhere else, and shows only offline profiles.
            ftp = ftplib.FTP()
            ftp.connect("127.0.0.2", self.ftp.server_address[1])
            ftp.login("xboxftp", "xboxftp")
            ftp.cwd("/Hdd1/Content")
            self.assertEqual(sorted(ftp.nlst()), [ola, kari])
            for bad in ("/Hdd1/Content/0009000000ABCDEF", "/Hdd1/NetISO", "/Hdd1/Content/../Apps"):
                self.assertRaises(ftplib.error_perm, ftp.cwd, bad)
            self.assertRaises(ftplib.error_perm, ftp.storbinary, "STOR /Hdd1/Content/x.bin", io.BytesIO(b"x"))
            self.assertRaises(ftplib.error_perm, ftp.mkd, "/Hdd1/Apps")
            ftp.quit()
            # Swapped again: there already, nothing copied twice.
            self.json("POST", "/api/party/seats", {"seats": {"127.0.0.2": ["Kari"], "127.0.0.1": ["Ola"]}})
            self.assertTrue(self.wait_for(lambda: self.json("GET", "/api/state")["party"]["profilesTo"].get("127.0.0.1", {}).get("total") == 2
                                          and self.json("GET", "/api/state")["party"]["profilesTo"]["127.0.0.1"]["state"] != "copying", 30))
            self.assertEqual(self.json("GET", "/api/state")["party"]["profilesTo"]["127.0.0.2"]["done"], 2)
        finally:
            stop.set()
            server.kill()
            out = server.communicate()[0]
        self.assertIn("Guest profiles for the party's consoles: 3 profiles copied to the party's consoles (1 there already).", out)
        self.assertIn("Guest profiles for the party's consoles: 0 profiles copied to the party's consoles (4 there already).", out)

    def test_progress_follows_the_player_to_the_other_consoles(self):
        # Ola plays on the second console (the wheel app's FTP there); once
        # drawn to the first, her newest profile and saves are copied there
        # over her older copy (Marcus 2026-11-02: switch consoles freely).
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        other = os.path.join(self.tmp.name, "xbox2")
        os.makedirs(os.path.join(other, "Hdd1"))
        app = subprocess.Popen([build_party_ftp(), os.path.join(other, "Hdd1"), "127.0.0.2", str(self.ftp.server_address[1])],
                               stdout=subprocess.PIPE, text=True)
        self.addCleanup(app.stdout.close)
        self.addCleanup(app.wait)
        self.addCleanup(app.kill)
        self.assertEqual(app.stdout.readline().strip(), "ready")
        ola = "E00000000000A001"

        def put(root, package, save):
            content = os.path.join(root, "Hdd1", "Content", ola)
            os.makedirs(os.path.join(content, "FFFE07D1", "00010000"), exist_ok=True)
            os.makedirs(os.path.join(content, "4D5307E6", "00000001"), exist_ok=True)
            with open(os.path.join(content, "FFFE07D1", "00010000", ola), "wb") as f:
                f.write(package)
            with open(os.path.join(content, "4D5307E6", "00000001", "save"), "wb") as f:
                f.write(save)

        def read(root, *path):
            with open(os.path.join(root, "Hdd1", "Content", ola, *path), "rb") as f:
                return f.read()
        put(self.xbox, b"CON old" + bytes(4096), b"old save")
        put(other, b"CON old" + bytes(4096), b"old save")
        config = os.path.join(self.tmp.name, "seats.ini")
        with open(config, "w", newline="") as f:
            f.write("guest_profiles=1\r\nguest_profile1=Ola|Ola|%s|1793400000|127.0.0.1|127.0.0.1,127.0.0.2\r\n" % ola)
        aurora = AuroraWeb()
        self.addCleanup(aurora.close)
        self.web = free_port()
        server, port = self.run_server("--xbox", "127.0.0.2", "--web-port", str(self.web), "--aurora-web-port", str(aurora.port), "--config", config)
        stop = threading.Event()

        def wheel():
            while not stop.is_set():
                try:
                    with connect_from("127.0.0.2", port) as conn:
                        conn.sendall(b"ISVR" + struct.pack(">HHQI", 0x5057, 0, 0, 0))
                        exact(conn, struct.unpack(">I", exact(conn, 4))[0])
                except OSError:
                    pass
                stop.wait(1)
        threading.Thread(target=wheel, daemon=True).start()
        self.addCleanup(stop.set)

        def sign_in(address, seat, controller, xuid, gamertag=b"Ola"):
            with connect_from(address, port) as conn:
                conn.sendall(b"ISVR" + struct.pack(">HHQI", 0x5049, seat | (controller << 8), xuid, len(gamertag)) + gamertag)
                return exact(conn, struct.unpack(">I", exact(conn, 4))[0]).decode("utf-8")

        def settled(address):
            to = self.json("GET", "/api/state")["party"]["profilesTo"].get(address, {})
            return to.get("state") not in (None, "copying")
        try:
            self.assertTrue(self.wait_for(lambda: self.ftp.scans, 20))
            self.json("POST", "/api/party", {"active": True, "gamemaster": "127.0.0.1", "consoles": ["127.0.0.1", "127.0.0.2"],
                                             "guests": [{"name": "Ola", "here": True}, {"name": "Kari", "here": True}]})
            self.assertTrue(self.wait_for(lambda: "127.0.0.2" in self.json("GET", "/api/state")["party"]["wheelOn"], 10))
            self.json("POST", "/api/party/seats", {"seats": {"127.0.0.2": ["Ola"], "127.0.0.1": ["Kari"]}})
            self.assertTrue(self.wait_for(lambda: settled("127.0.0.1") and settled("127.0.0.2"), 30))
            # She signs in there and plays: her copy there is the newest now.
            self.assertEqual(sign_in("127.0.0.2", 0, 0, int(ola, 16)), "ok\n")
            put(other, b"CON new" + bytes(8192), b"new save, level 9" * 1000)
            # Drawn to the first console next: it gets her newest progress.
            self.json("POST", "/api/party/seats", {"seats": {"127.0.0.1": ["Ola"], "127.0.0.2": ["Kari"]}})
            self.assertTrue(self.wait_for(lambda: read(self.xbox, "4D5307E6", "00000001", "save") == b"new save, level 9" * 1000, 30))
            self.assertEqual(read(self.xbox, "FFFE07D1", "00010000", ola), b"CON new" + bytes(8192))
            self.assertTrue(self.wait_for(lambda: settled("127.0.0.1"), 30))
            # Up to date now: not copied again.
            self.json("POST", "/api/party/seats", {"seats": {"127.0.0.1": ["Ola", "Kari"]}})
            self.assertTrue(self.wait_for(lambda: settled("127.0.0.1") and settled("127.0.0.2"), 30))
            time.sleep(1)
        finally:
            stop.set()
            server.kill()
            out = server.communicate()[0]
        self.assertIn("127.0.0.1: has Ola's newest progress now (from ", out)
        self.assertEqual(out.count("newest progress now"), 1)
        self.assertIn("1 brought up to date)", out)
        with open(config) as f:
            self.assertIn("|127.0.0.2|", f.read())  # Where she played last, kept.

    def test_live_feed_and_turn_off_all_when_idle(self):
        # A guest's gamerscore rising on a console (as its Aurora says, in a
        # game too) is news on the big screen; the party's consoles turn off
        # once nothing happened for the party for its idle minutes.
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        ola = "E00000000000A001"
        config = os.path.join(self.tmp.name, "live.ini")
        with open(config, "w", newline="") as f:
            f.write("guest_profiles=1\r\nguest_profile1=Ola|Ola|%s|1793400000|127.0.0.1|127.0.0.1\r\n" % ola)
        aurora = AuroraWeb()
        self.addCleanup(aurora.close)
        aurora.profiles = [{"index": 0, "gamertag": "Ola", "gamerscore": 100, "signedin": 1, "xuid": ola}]
        xbdm = Xbdm()
        self.addCleanup(xbdm.close)
        self.web = free_port()
        server, port = self.run_server("--web-port", str(self.web), "--aurora-web-port", str(aurora.port), "--xbdm-port", str(xbdm.port),
                                       "--config", config, "--live-every", "1")
        try:
            self.assertTrue(self.wait_for(lambda: self.ftp.scans, 20))
            # The console is on: its NetISO plugin is connected.
            keep = connect_from("127.0.0.1", port)
            self.addCleanup(keep.close)
            keep.sendall(b"ISVR" + struct.pack(">HHQI", 0, 0, 0, 0))
            self.json("POST", "/api/party", {"active": True, "gamemaster": "127.0.0.1", "consoles": ["127.0.0.1"], "guests": [{"name": "Ola", "here": True}]})
            time.sleep(2.5)
            aurora.profiles[0]["gamerscore"] = 120
            feed = lambda: [x["text"] for x in self.json("GET", "/api/state")["party"]["feed"]]
            self.assertTrue(self.wait_for(lambda: "Ola earned 20 G" in feed(), 10), feed())
            self.assertEqual([x["kind"] for x in self.json("GET", "/api/state")["party"]["feed"]], ["score"])
            self.assertEqual(xbdm.commands, [])
            # Turn off all when idle: a minute here (the tests can't wait an hour).
            self.assertEqual(self.call("POST", "/api/party", {"idleOffMinutes": 999})[0], 400)
            self.json("POST", "/api/party", {"idleOffMinutes": 1})
            self.assertEqual(self.json("GET", "/api/state")["party"]["idleOffMinutes"], 1)
            self.assertTrue(self.wait_for(lambda: xbdm.commands == ["shutdown"], 75), xbdm.commands)
            time.sleep(3)
            self.assertEqual(xbdm.commands, ["shutdown"])  # Once.
        finally:
            server.kill()
            out = server.communicate()[0]
        self.assertIn("its consoles turn off (Turn off all when idle).", out)

    def test_turns_every_party_console_off_or_back_to_aurora(self):
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        _, ftp2 = self.second_console()
        aurora = AuroraWeb()
        aurora2 = AuroraWeb(host="127.0.0.2", port=aurora.port)
        xbdm = Xbdm()
        xbdm2 = Xbdm(host="127.0.0.2", port=xbdm.port)
        self.web = free_port()
        server, _ = self.run_server("--xbox", "127.0.0.2", "--web-port", str(self.web), "--aurora-web-port", str(aurora.port),
                                    "--xbdm-port", str(xbdm.port))
        try:
            # Once their game folders are written (never while that goes on).
            self.assertTrue(self.wait_for(lambda: self.ftp.scans and ftp2.scans, 20))
            self.json("POST", "/api/party", {"gamemaster": "127.0.0.2", "consoles": ["127.0.0.1", "127.0.0.2"]})
            done = self.json("POST", "/api/party/power", {"action": "aurora"})
            self.assertEqual(done, {"results": [{"address": "127.0.0.2", "ok": True, "error": ""}, {"address": "127.0.0.1", "ok": True, "error": ""}]})
            for each in (aurora, aurora2):
                self.assertEqual(each.launched, [("\\Device\\Harddisk0\\Partition1\\Apps\\Aurora", "Aurora.xex", "0")])
            # Back to the party on all: the party app, from a game too.
            done = self.json("POST", "/api/party/power", {"action": "party"})
            self.assertEqual(done, {"results": [{"address": "127.0.0.2", "ok": True, "error": ""}, {"address": "127.0.0.1", "ok": True, "error": ""}]})
            for each in (aurora, aurora2):
                self.assertEqual(each.launched[1:], [("\\Device\\Harddisk0\\Partition1\\NetDisc360\\Games\\NetDisc360 Party", "default.xex", "0")])
                del each.launched[1:]
            # Only those named.
            self.json("POST", "/api/party/power", {"action": "off", "consoles": ["127.0.0.2"]})
            self.assertEqual((xbdm.commands, xbdm2.commands), ([], ["shutdown"]))
            # One without its debug monitor fails alone.
            xbdm.close()
            done = self.json("POST", "/api/party/power", {"action": "off"})
            results = {r["address"]: r for r in done["results"]}
            self.assertTrue(results["127.0.0.2"]["ok"])
            self.assertFalse(results["127.0.0.1"]["ok"])
            self.assertIn("debug monitor (xbdm, port", results["127.0.0.1"]["error"])
            self.assertEqual(xbdm2.commands, ["shutdown", "shutdown"])
            for body in ({"action": "sleep"}, {"action": "off", "consoles": ["127.0.0.9"]}):
                self.assertEqual(self.call("POST", "/api/party/power", body)[0], 400)
        finally:
            for each in (aurora, aurora2, xbdm2):
                each.close()
            server.kill()
            server.communicate()

    def test_reads_each_console_s_title_updates(self):
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        self.add_iso("Skate 3.iso", 0x5C087C2C, 0x454108E6)
        other, ftp2 = self.second_console()
        # Halo 3: Title Update 8 on the first console (3 there too, unused),
        # on the second it's there but not used. Skate 3: 2 on both.
        databases = os.path.join(self.xbox, "Hdd1", "Apps", "Aurora", "Data", "Databases")
        databases2 = os.path.join(other, "Hdd1", "Apps", "Aurora", "Data", "Databases")
        title_update_dbs(databases, [(5, 0x4D5307E6, 0x1A2B3C4D, 0, 8, "Title Update 8", "tu00000008_00000000"),
                                     (6, 0x454108E6, 0x5C087C2C, 0x10000, 2, "Title Update 2", "tu00000002_00000000"),
                                     (7, 0x4D5307E6, 0x1A2B3C4D, 0, 3, "Title Update 3", "tu00000003_00000000")], [5, 6])
        title_update_dbs(databases2, [(1, 0x4D5307E6, 0x1A2B3C4D, 0, 8, "Title Update 8", "tu00000008_00000000"),
                                      (2, 0x454108E6, 0x5C087C2C, 0x10000, 2, "Title Update 2", "tu00000002_00000000")], [2], table_key=True)
        aurora = AuroraWeb()
        aurora2 = AuroraWeb(host="127.0.0.2", port=aurora.port)
        self.web = free_port()
        server, _ = self.run_server("--xbox", "127.0.0.2", "--web-port", str(self.web), "--aurora-web-port", str(aurora.port))
        try:
            self.assertTrue(self.wait_for(lambda: all(c["read"] for c in self.json("GET", "/api/titleupdates/compare")["consoles"]), 25))
            first = time.time()  # After the databases' listing was first seen.
            listed = self.json("GET", "/api/titleupdates/compare")
            consoles = {c["address"]: c for c in listed["consoles"]}
            self.assertEqual(sorted(consoles), ["127.0.0.1", "127.0.0.2"])
            self.assertTrue(re.match(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$", consoles["127.0.0.1"]["read"]))
            self.assertEqual(consoles["127.0.0.1"]["error"], "")
            self.assertEqual(sorted((t["titleId"], t["version"], t["active"]) for t in consoles["127.0.0.1"]["tus"]),
                             [("454108E6", 2, True), ("4D5307E6", 3, False), ("4D5307E6", 8, True)])
            self.assertIn({"titleId": "454108E6", "mediaId": "5C087C2C", "baseVersion": 65536, "version": 2, "name": "Title Update 2",
                           "file": "tu00000002_00000000", "active": True}, consoles["127.0.0.1"]["tus"])
            self.assertEqual(sorted((t["titleId"], t["version"], t["active"]) for t in consoles["127.0.0.2"]["tus"]),
                             [("454108E6", 2, True), ("4D5307E6", 8, False)])
            games = {g["game"]: g for g in listed["games"]}
            self.assertEqual(games["Halo 3"], {"titleId": "4D5307E6", "game": "Halo 3", "byConsole": {"127.0.0.1": {"version": 8, "name": "Title Update 8"},
                                                                                                      "127.0.0.2": None},
                                               "newest": {"version": 8, "name": "Title Update 8"}, "mismatch": True})
            self.assertFalse(games["Skate 3"]["mismatch"])
            library = {g["name"]: g for g in self.json("GET", "/api/games")["games"]}
            self.assertEqual((library["Halo 3"]["titleUpdates"], library["Halo 3"]["tuMismatch"]), ({"127.0.0.1": "Title Update 8", "127.0.0.2": None}, True))
            self.assertEqual((library["Skate 3"]["titleUpdates"], library["Skate 3"]["tuMismatch"]),
                             ({"127.0.0.1": "Title Update 2", "127.0.0.2": "Title Update 2"}, False))
            self.assertEqual(self.json("GET", "/api/state")["titleUpdates"], {"mismatches": 1})
            # A party compares its own consoles only; the page reads the games again.
            version = self.json("GET", "/api/state")["library"]["version"]
            self.json("POST", "/api/party", {"active": True, "consoles": ["127.0.0.1"]})
            self.assertEqual(self.json("GET", "/api/state")["titleUpdates"], {"mismatches": 0})
            self.assertNotEqual(self.json("GET", "/api/state")["library"]["version"], version)
            library = {g["name"]: g for g in self.json("GET", "/api/games")["games"]}
            self.assertEqual(library["Halo 3"]["titleUpdates"], {"127.0.0.1": "Title Update 8"})
            self.json("POST", "/api/party", {"active": False})
            # Title Update 8 put to use on the second console, its settings.db
            # changed in place: as big as before, and as old in a listing,
            # which gives the time to the minute. Back in Aurora, the
            # databases are read again, as they were last read in the minute
            # their listing was first seen: no mismatch any more.
            fetched = lambda: [line for line in ftp2.log if line in ("RETR content.db", "RETR settings.db")]
            self.assertEqual(fetched(), ["RETR content.db", "RETR settings.db"])
            files = [os.path.join(databases2, name) for name in ("content.db", "settings.db")]
            listed = [(os.path.getsize(f), os.stat(f).st_mtime) for f in files]
            title_update_dbs(databases2, [(1, 0x4D5307E6, 0x1A2B3C4D, 0, 8, "Title Update 8", "tu00000008_00000000"),
                                          (2, 0x454108E6, 0x5C087C2C, 0x10000, 2, "Title Update 2", "tu00000002_00000000")], [2, 1], table_key=True)
            for f, (size, mtime) in zip(files, listed):
                self.assertEqual(os.path.getsize(f), size)
                os.utime(f, (mtime, mtime))
            version = self.json("GET", "/api/state")["library"]["version"]
            self.json("POST", "/api/console/power", {"address": "127.0.0.2", "action": "aurora"})
            self.assertTrue(self.wait_for(lambda: self.json("GET", "/api/state")["titleUpdates"]["mismatches"] == 0, 10))
            self.assertNotEqual(self.json("GET", "/api/state")["library"]["version"], version)  # The games' title updates changed.
            self.assertEqual(fetched()[2:], ["RETR content.db", "RETR settings.db"])
            # Once their listing is a minute old, the copies read then stay:
            # back in Aurora, the databases are listed again, but read again
            # only once they changed; never written.
            def back_in_aurora():
                looked = ftp2.log.count("CWD /Hdd1/Apps/Aurora/Data/Databases")
                self.json("POST", "/api/console/power", {"address": "127.0.0.2", "action": "aurora"})
                self.assertTrue(self.wait_for(lambda: ftp2.log.count("CWD /Hdd1/Apps/Aurora/Data/Databases") > looked, 10))
                time.sleep(1)
            time.sleep(max(0, first + 61 - time.time()))
            back_in_aurora()
            self.assertEqual(fetched()[4:], ["RETR content.db", "RETR settings.db"])
            back_in_aurora()
            self.assertEqual(len(fetched()), 6)
            self.assertEqual(self.json("GET", "/api/state")["titleUpdates"]["mismatches"], 0)
            self.assertFalse([line for line in ftp2.log + self.ftp.log if line.startswith("STOR") and "Databases" in line])
        finally:
            aurora.close()
            aurora2.close()
            server.kill()
            out = server.communicate()[0]
        self.assertIn("Xbox 127.0.0.1: 3 title updates in Aurora, 2 in use.", out)

    def test_compares_title_updates_without_writing_game_folders(self):
        # With game folders not written, the party's consoles' title updates
        # are still read: as the party starts, and before Start on all.
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        other, ftp2 = self.second_console()
        databases2 = os.path.join(other, "Hdd1", "Apps", "Aurora", "Data", "Databases")
        title_update_dbs(os.path.join(self.xbox, "Hdd1", "Apps", "Aurora", "Data", "Databases"),
                         [(5, 0x4D5307E6, 0x1A2B3C4D, 0, 8, "Title Update 8", "tu00000008_00000000")], [5])
        title_update_dbs(databases2, [], [])
        aurora = AuroraWeb()
        aurora2 = AuroraWeb(host="127.0.0.2", port=aurora.port)
        self.web = free_port()
        server, _ = self.run_server("--no-push", "--xbox", "127.0.0.2", "--xbox", "127.0.0.3", "--web-port", str(self.web), "--aurora-web-port", str(aurora.port))
        try:
            time.sleep(2)
            self.assertEqual((self.logins(), self.logins(ftp2)), (0, 0))
            self.assertFalse(any(c["read"] for c in self.json("GET", "/api/titleupdates/compare")["consoles"]))
            self.json("POST", "/api/party", {"active": True, "gamemaster": "127.0.0.1", "consoles": ["127.0.0.2"]})
            self.assertTrue(self.wait_for(lambda: self.json("GET", "/api/state")["titleUpdates"]["mismatches"] == 1, 10))
            # Title Update 8 put to use on the second console.
            title_update_dbs(databases2, [(1, 0x4D5307E6, 0x1A2B3C4D, 0, 8, "Title Update 8", "tu00000008_00000000")], [1])
            done = self.json("POST", "/api/party/start", {"game": "Halo 3"})
            self.assertEqual(done["titleUpdates"], {"mismatch": False, "byConsole": {"127.0.0.1": "Title Update 8", "127.0.0.2": "Title Update 8"},
                                                    "newest": "Title Update 8", "unread": []})
            self.assertEqual(self.json("GET", "/api/state")["titleUpdates"]["mismatches"], 0)
            # One whose title updates were never read (its Aurora doesn't answer) is said to be left out.
            done = self.json("POST", "/api/party/start", {"game": "Halo 3", "consoles": ["127.0.0.1", "127.0.0.3"]})
            self.assertEqual(done["titleUpdates"], {"mismatch": False, "byConsole": {"127.0.0.1": "Title Update 8"}, "newest": "Title Update 8",
                                                    "unread": ["127.0.0.3"]})
            # Nothing was written.
            self.assertFalse([line for line in self.ftp.log + ftp2.log if line.split(" ")[0] in ("STOR", "MKD", "DELE", "RNFR", "RMD")])
        finally:
            aurora.close()
            aurora2.close()
            server.kill()
            server.communicate()

    def test_a_game_installed_later_loses_its_tile(self):
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        self.add_iso("Skate 3.iso", 0x5C087C2C, 0x454108E6)
        server, port = self.run_server("--remove-wait", "0")
        client = None
        try:
            self.assertTrue(self.wait_for(lambda: self.ftp.scans, 15))
            self.assertTrue(os.path.isdir(os.path.join(self.games, "Skate 3")))
            # Installed while Aurora runs: nothing changes yet.
            self.install("Games/Skate 3", 0x5C087C2C, 0x454108E6)
            time.sleep(2)
            self.assertTrue(os.path.isdir(os.path.join(self.games, "Skate 3")))
            # A game played and ended: back in Aurora, the console is looked at
            # again, and Skate 3 loses its tile.
            client = Client(port)
            self.assertEqual(client.mount("Halo 3.iso"), 1)
            client.read(0, 1 << 20)
            self.assertTrue(self.wait_for(lambda: not os.path.isdir(os.path.join(self.games, "Skate 3")), 25))
            self.assertTrue(os.path.isdir(os.path.join(self.games, "Halo 3")))
        finally:
            if client:
                client.close()
            server.kill()
            out = server.communicate()[0]
        self.assertIn("what's installed on it changed; its game folders are written again.", out)

    def test_a_game_installed_while_one_started_from_the_dashboard_ran_loses_its_tile(self):
        # A copy installed on the console, started from the dashboard, isn't
        # NetISO's: its return to Aurora shows by Aurora's FTP server, gone
        # while the game runs. Back in Aurora, the console is looked at again.
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        self.add_iso("Skate 3.iso", 0x5C087C2C, 0x454108E6)
        self.install("Games/Halo 3", 0x1A2B3C4D, 0x4D5307E6)
        aurora = AuroraWeb()
        self.web = free_port()
        server, _ = self.run_server("--remove-wait", "0", "--web-port", str(self.web), "--aurora-web-port", str(aurora.port))
        try:
            self.assertTrue(self.wait_for(lambda: self.ftp.scans, 15))
            self.assertTrue(os.path.isdir(os.path.join(self.games, "Skate 3")))
            self.assertEqual(self.json("POST", "/api/play", {"game": "Halo 3", "console": "127.0.0.1"}), {"ok": True})
            self.assertEqual(aurora.launched, [("\\Device\\Harddisk0\\Partition1\\Games\\Halo 3", "default.xex", "0")])
            # The game runs, so Aurora's FTP server is gone; Skate 3 is installed meanwhile.
            port = self.ftp.server_address[1]
            self.ftp.shutdown()
            self.ftp.server_close()
            self.install("Games/Skate 3", 0x5C087C2C, 0x454108E6)
            time.sleep(7)
            self.assertTrue(os.path.isdir(os.path.join(self.games, "Skate 3")))
            # Back in Aurora.
            self.ftp = aurora_ftp.Server(self.xbox, port)
            threading.Thread(target=self.ftp.serve_forever, daemon=True).start()
            self.assertTrue(self.wait_for(lambda: not os.path.isdir(os.path.join(self.games, "Skate 3")), 40))
        finally:
            aurora.close()
            server.kill()
            out = server.communicate()[0]
        self.assertIn("Xbox 127.0.0.1: back in Aurora after the game started from the dashboard (its FTP server answers).", out)
        self.assertIn("what's installed on it changed; its game folders are written again.", out)

    def test_starts_the_tile_when_aurora_can_t_start_the_installed_copy(self):
        # Aurora's web server starts games on the hard drive only: a copy
        # installed on a USB drive leaves the start to the tile, and a copy on
        # the hard drive goes before one there.
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        self.add_iso("Skate 3.iso", 0x5C087C2C, 0x454108E6)
        for drive, name, media_id, title_id in (("Usb0", "Halo 3", 0x1A2B3C4D, 0x4D5307E6), ("Usb0", "Skate 3", 0x5C087C2C, 0x454108E6),
                                                ("Hdd1", "Skate 3", 0x5C087C2C, 0x454108E6)):
            path = os.path.join(self.xbox, drive, "Games", name)
            os.makedirs(path)
            with open(os.path.join(path, "default.xex"), "wb") as f:
                f.write(xex(execution_info(media_id, title_id)))
        aurora = AuroraWeb()
        self.web = free_port()
        server, _ = self.run_server("--no-installed", "--installed", "/Usb0/Games;/Hdd1/Games", "--web-port", str(self.web),
                                    "--aurora-web-port", str(aurora.port))
        try:
            self.assertTrue(self.wait_for(lambda: self.ftp.scans, 15))
            games = {g["name"]: g for g in self.json("GET", "/api/games")["games"]}
            self.assertEqual((games["Halo 3"]["tiles"], games["Halo 3"]["installedOn"]), (["127.0.0.1"], ["127.0.0.1"]))
            self.assertEqual(self.json("POST", "/api/play", {"game": "Halo 3", "console": "127.0.0.1"}), {"ok": True})
            self.assertEqual(self.json("POST", "/api/play", {"game": "Skate 3", "console": "127.0.0.1"}), {"ok": True})
            self.json("POST", "/api/party", {"gamemaster": "127.0.0.1"})
            done = self.json("POST", "/api/party/start", {"game": "Halo 3"})
            self.assertEqual([(r["address"], r["ok"], r["how"], r["error"]) for r in done["results"]], [("127.0.0.1", True, "tile", "")])
            self.assertEqual(aurora.launched, [("\\Device\\Harddisk0\\Partition1\\NetDisc360\\Games\\Halo 3", "default.xex", "0"),
                                               ("\\Device\\Harddisk0\\Partition1\\Games\\Skate 3", "default.xex", "0"),
                                               ("\\Device\\Harddisk0\\Partition1\\NetDisc360\\Games\\Halo 3", "default.xex", "0")])
        finally:
            aurora.close()
            server.kill()
            server.communicate()

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

    def ports(self):
        self.web = free_port()
        return ["--web-port", str(self.web)]

    def start(self, *extra):
        super().start("--config", self.config, *extra)

    def restart(self, *extra):
        self.server.kill()
        self.server.wait()
        self.server.stdout.close()
        self.start(*extra)

    def test_sets_how_each_console_gets_the_games(self):
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        self.add_iso("Skate 3.iso", 0x5C087C2C, 0x454108E6)
        with open(self.config, "w") as f:
            f.write("consoles=1\nconsole1=192.168.1.20|1700000000\n")
        self.start()
        self.state_when(lambda s: s["library"]["games"] == 2)
        tiles = lambda: {c["address"]: c["tiles"] for c in self.json("GET", "/api/state")["consoles"]}
        self.assertEqual(tiles()["192.168.1.20"], {"mode": "", "games": {}})
        self.assertEqual(self.json("GET", "/api/state")["settings"]["tileMode"], "folder")
        self.json("POST", "/api/consoles/tiles", {"address": "192.168.1.20", "mode": "god", "games": {"Skate 3": "none", "Halo 3": ""}})
        self.assertEqual(tiles()["192.168.1.20"], {"mode": "god", "games": {"Skate 3": "none"}})
        self.json("POST", "/api/settings", {"tileMode": "none"})
        self.assertEqual(self.json("GET", "/api/state")["settings"]["tileMode"], "none")
        for bad in ({"address": "192.168.1.20", "mode": "disc"}, {"address": "192.168.1.20", "games": {"Halo 3": "iso"}},
                    {"address": "192.168.1.20", "games": {"Halo 4": "god"}}, {"mode": "god"}):
            self.assertEqual(self.call("POST", "/api/consoles/tiles", bad)[0], 400, bad)
        self.assertEqual(self.call("POST", "/api/settings", {"tileMode": "usb"})[0], 400)
        # Kept in the settings file.
        self.restart()
        self.state_when(lambda s: s["library"]["games"] == 2)
        self.assertEqual(tiles()["192.168.1.20"], {"mode": "god", "games": {"Skate 3": "none"}})
        self.assertEqual(self.json("GET", "/api/state")["settings"]["tileMode"], "none")
        # Back to the default, with no game of its own: nothing kept.
        self.json("POST", "/api/consoles/tiles", {"address": "192.168.1.20", "mode": "", "games": {}})
        self.assertEqual(tiles()["192.168.1.20"], {"mode": "", "games": {}})
        with open(self.config) as f:
            self.assertIn("tile_choices=0", f.read())

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
        self.assertEqual(state["folders"], [{"path": self.isos, "games": 1, "found": True, "emptied": False}])
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

    def test_an_original_xbox_game_names_its_emulator(self):
        folder = os.path.join(self.isos, "Crazy Taxi 3")
        os.makedirs(folder)
        with open(os.path.join(folder, "default.xbe"), "wb") as f:
            f.write(xbe(0x49470023))
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        self.start()
        self.state_when(lambda s: s["library"]["games"] == 2)
        games = self.json("GET", "/api/games")["games"]
        self.assertEqual([(g["name"], g["xbe"], g["xefu"]) for g in games],
                         [("Crazy Taxi 3", True, "xefu7"), ("Halo 3", False, "")])
        self.assertEqual(games[0]["xefuWhy"], "in-game on xefu7, untested on xefu")

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

    def test_checks_its_own_setup(self):
        # A game there twice, named with an address: the report leaves it out.
        self.add_iso("Halo 3 10.1.2.3.iso", 0x1A2B3C4D, 0x4D5307E6)
        folder = os.path.join(self.isos, "Halo 3 extracted")
        os.makedirs(folder)
        with open(os.path.join(folder, "default.xex"), "wb") as f:
            f.write(xex(execution_info(0x1234, 0x4D5307E6)))
        self.add_iso("Skate 3.iso", 0x5C087C2C, 0x454108E6)
        self.start()
        self.state_when(lambda s: s["library"]["games"] == 2 and s["library"]["conflicts"] == 1)
        setup = self.json("GET", "/api/setup")
        self.assertEqual([setup[k] for k in ("folders", "foldersFound", "games", "iso", "god", "extracted", "conflicts")],
                         [1, 1, 2, 2, 0, 0, 1])
        self.assertEqual([setup[k] for k in ("skipInstalled", "auroraScripts", "followAddress", "keepAwake")], [True] * 4)
        # Only Windows has network categories to fix.
        self.assertEqual([setup[k] for k in ("networks", "publicNetwork", "canFix")], [[], False, False])
        self.assertEqual(self.call("POST", "/api/network/private", {})[0], 400)
        warning = [w for w in setup["warnings"] if "more than once" in w]
        self.assertEqual(len(warning), 1)
        self.assertIn("Halo 3 x.x.x.x", warning[0])
        self.assertNotIn("10.1.2.3", json.dumps(setup))

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

    def save_console(self, root):
        """A console's hard drive with Halo 3 saves in one profile, and another game's in it too."""
        profile = os.path.join(root, "Hdd1", "Content", "E0000123456789AB")
        os.makedirs(os.path.join(profile, "4D5307E6", "00000001"))
        os.makedirs(os.path.join(profile, "41560817", "00000001"))
        os.makedirs(os.path.join(profile, "FFFE07D1", "00010000"))
        os.makedirs(os.path.join(root, "Hdd1", "Content", "0000000000000000", "4D5307E6", "000B0000"))
        package = bytearray(0x1000)
        package[0:4] = b"CON "
        package[0x411:0x411 + 12] = "Marcus".encode("utf-16-be")
        with open(os.path.join(profile, "FFFE07D1", "00010000", "E0000123456789AB"), "wb") as f:
            f.write(package)
        for name, data in (("4D5307E6/00000001/Campaign", b"halo save 1"), ("41560817/00000001/Save", b"other game")):
            with open(os.path.join(profile, *name.split("/")), "wb") as f:
                f.write(data)
        return os.path.join(profile, "4D5307E6", "00000001", "Campaign")

    def test_backs_up_a_game_s_saves_after_it_ends_and_restores_them(self):
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        with open(self.config, "w") as f:
            f.write("[Server]\nsave_delay=0\n")
        ftp_port = free_port()
        # The profile is signed in: the console won't hand out its package
        # over FTP, so its gamertag comes from Aurora's web server.
        aurora = AuroraWeb(host="127.0.0.2")
        aurora.profiles = [{"index": 0, "gamertag": "Marcus", "gamerscore": 20, "signedin": 1, "xuid": "E0000123456789AB"},
                           {"index": 1, "gamertag": "", "gamerscore": 0, "signedin": 0, "xuid": "0000000000000000"}]
        self.addCleanup(aurora.close)
        self.start("--xbox", "127.0.0.2", "-v", "--ftp-port", str(ftp_port), "--aurora-web-port", str(aurora.port))
        xbox = os.path.join(self.tmp.name, "xbox")
        save = self.save_console(xbox)
        package = os.path.join(xbox, "Hdd1", "Content", "E0000123456789AB", "FFFE07D1", "00010000", "E0000123456789AB")
        self.assertTrue(os.path.isfile(package))
        console = lambda s: [x for x in s["consoles"] if x["address"] == "127.0.0.2"][0]  # noqa: E731
        with connect_from("127.0.0.2", self.port) as plugin:
            c = Client.__new__(Client)
            c.conn = plugin
            self.assertEqual(c.mount("Halo 3.iso"), 1)
            self.state_when(lambda s: console(s)["playing"])
            ftp = aurora_ftp.Server(xbox, ftp_port, host="127.0.0.2")
            ftp.locked.add(os.path.normcase(os.path.abspath(package)))
            threading.Thread(target=ftp.serve_forever, daemon=True).start()
            try:
                state = self.state_when(lambda s: "Saves of Halo 3 backed up" in [e["text"] for e in console(s)["events"]], 30)
                self.assertIn("Saves of Halo 3 backed up", [e["text"] for e in console(state)["events"]])
                listed = self.json("GET", "/api/saves")
                self.assertTrue(listed["on"])
                self.assertEqual([(b["titleId"], b["game"], b["profiles"]) for b in listed["saves"]], [("4D5307E6", "Halo 3", ["Marcus"])])
                name = listed["saves"][0]["name"]
                self.assertTrue(re.match(r"4D5307E6_\d{4}-\d\d-\d\d_\d\d-\d\d-\d\d\.zip$", name), name)
                # Only that game's saves, by profile, and what they are.
                status, response, data = self.call("GET", "/api/save?name=" + name)
                self.assertEqual(status, 200)
                self.assertEqual(response.getheader("Content-Type"), "application/zip")
                with zipfile.ZipFile(io.BytesIO(data)) as z:
                    self.assertEqual(sorted(z.namelist()), ["E0000123456789AB/Campaign", "save.ini"])
                    self.assertEqual(z.read("E0000123456789AB/Campaign"), b"halo save 1")
                    self.assertIn(b"profile_E0000123456789AB=Marcus\r\n", z.read("save.ini"))
                self.assertEqual(self.call("GET", "/api/save?name=../netdisc.ini")[0], 404)
                # Not while it plays.
                for _ in range(33 * 16):
                    c.read(0, 65536)
                self.state_when(lambda s: console(s)["playing"])
                status, _, data = self.call("POST", "/api/saves/restore", {"name": name, "address": "127.0.0.2"})
                self.assertEqual(status, 400)
                self.assertIn(b"is playing a game", data)
            finally:
                ftp.shutdown()
                ftp.server_close()
        # Back in Aurora with the save changed: Back up now keeps both, and
        # skips games that aren't this server's and saves already kept.
        ftp = aurora_ftp.Server(xbox, ftp_port, host="127.0.0.2")
        threading.Thread(target=ftp.serve_forever, daemon=True).start()
        try:
            self.assertEqual(self.json("POST", "/api/saves/backup", {"address": "127.0.0.2"}), {"ok": True, "made": 0, "same": 1})
            with open(save, "wb") as f:
                f.write(b"halo save 2")
            self.assertEqual(self.json("POST", "/api/saves/backup", {"address": "127.0.0.2"}), {"ok": True, "made": 1, "same": 0})
            self.assertEqual(len(self.json("GET", "/api/saves")["saves"]), 2)
            # Restored: the old save is back, and the one there before is kept.
            self.assertEqual(self.json("POST", "/api/saves/restore", {"name": name, "address": "127.0.0.2"}), {"ok": True})
            with open(save, "rb") as f:
                self.assertEqual(f.read(), b"halo save 1")
            saves = self.json("GET", "/api/saves")["saves"]
            self.assertEqual(len(saves), 2)  # The save before was kept already.
            self.assertEqual(self.json("POST", "/api/saves/delete", {"name": saves[-1]["name"]}), {"ok": True})
            self.assertEqual(len(self.json("GET", "/api/saves")["saves"]), 1)
            self.assertEqual(self.call("POST", "/api/saves/restore", {"name": name, "address": "127.0.0.9"})[0], 400)
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

    def test_keeps_the_console_extras(self):
        self.start()
        self.assertEqual(self.call("POST", "/api/settings", {"installedFolders": "Games"})[0], 400)
        self.assertEqual(self.call("POST", "/api/settings", {"auroraWebPort": 0})[0], 400)
        self.assertEqual(self.call("POST", "/api/settings", {"auroraWebUser": ""})[0], 400)
        self.assertEqual(self.call("POST", "/api/settings", {"autoOffMinutes": -1})[0], 400)
        self.json("POST", "/api/settings", {"skipInstalled": False, "installedFolders": " /Hdd1/Games/ ;/Usb0/Games\n/hdd1/games",
                                             "auroraWebPassword": "w3b pass", "auroraWebPort": 8080, "restartAurora": False,
                                             "keepAwake": False, "followAddress": False, "auroraScripts": False, "autoOffMinutes": 30})
        for _ in range(2):  # As changed, and as saved.
            settings = self.json("GET", "/api/state")["settings"]
            self.assertEqual([settings[k] for k in ("skipInstalled", "installedFolders", "auroraWebUser", "auroraWebPasswordSet", "auroraWebPort",
                                                    "restartAurora", "keepAwake", "followAddress", "auroraScripts", "autoOffMinutes")],
                             [False, "/Hdd1/Games;/Usb0/Games", "xboxhttp", True, 8080, False, False, False, False, 30])
            self.assertNotIn("w3b pass", json.dumps(settings))
            self.restart()
        with open(self.config) as f:
            saved = f.read()
        self.assertIn("installed_folders=/Hdd1/Games;/Usb0/Games\n", saved)
        self.assertIn("aurora_web_port=8080\n", saved)
        self.assertIn("auto_off_minutes=30\n", saved)
        # Empty: Games on Demand only.
        self.json("POST", "/api/settings", {"installedFolders": ""})
        self.assertEqual(self.json("GET", "/api/state")["settings"]["installedFolders"], "")

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

    def test_settings_from_before_move_the_game_folder(self):
        # Hdd1:\NetISO was the default; settings from then get the new one.
        with open(self.config, "w") as f:
            f.write("[Server]\nxbox_folder=/Hdd1/NetISO/\n")
        self.start()
        self.assertEqual(self.json("GET", "/api/state")["settings"]["xboxFolder"], "/Hdd1/NetDisc360/Games")
        # Chosen again after that, it stays.
        self.json("POST", "/api/settings", {"xboxFolder": "/Hdd1/NetISO"})
        self.restart()
        self.assertEqual(self.json("GET", "/api/state")["settings"]["xboxFolder"], "/Hdd1/NetISO")
        # Another folder was never the default: it stays as it is.
        with open(self.config, "w") as f:
            f.write("[Server]\nxbox_folder=/Usb0/Games\n")
        self.restart()
        self.assertEqual(self.json("GET", "/api/state")["settings"]["xboxFolder"], "/Usb0/Games")

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

    def test_the_setup_wizard_shows_once(self):
        # A first start (no settings file) opens the wizard; finishing it is kept.
        self.start()
        settings = self.json("GET", "/api/state")["settings"]
        self.assertFalse(settings["wizardDone"])
        self.assertEqual(settings["preset"], {"folders": True, "xboxes": False, "ftp": False})
        self.json("POST", "/api/settings", {"wizardDone": True})
        self.restart()
        self.assertTrue(self.json("GET", "/api/state")["settings"]["wizardDone"])
        # A settings file from before the wizard: that copy was set up already.
        with open(self.config, "w") as f:
            f.write("[Server]\nscan=1\n")
        self.restart()
        self.assertTrue(self.json("GET", "/api/state")["settings"]["wizardDone"])

    def test_finds_and_checks_a_console(self):
        xbox = os.path.join(self.tmp.name, "xbox")
        for folder in ("Usb0", "Hdd1/Apps/Aurora", "Hdd1/Plugins"):
            os.makedirs(os.path.join(xbox, folder))
        files = {"Usb0/launch.ini": b"[Paths]\r\nDefault = Hdd:\\Apps\\Aurora\\Aurora.xex\r\n[Plugins]\r\n"
                                    b"plugin1 = Usb:\\NetISO.xex ; the server\r\nplugin2 = Usb:\\NetIsoTiles.xex\r\nplugin3 = Usb:\\Gone.xex\r\n"
                                    b"plugin4 = Hdd:\\Plugins\\NetDisc360.xex\r\n",
                 "Usb0/NetISO.xex": b"NETISO", "Usb0/NetISO.xex.txt": b"10.0.0.99\r\n", "Usb0/NetIsoTiles.xex": b"TILES",
                 "Hdd1/Plugins/NetDisc360.xex": b"DRIVE", "Hdd1/Plugins/NetDisc360.ini": b"read_kb=256\r\nserver = 10.0.0.7:4323\r\n"}
        for name, data in files.items():
            with open(os.path.join(xbox, name), "wb") as f:
                f.write(data)
        # Aurora's scan paths: Hdd1:\Games two folders deep and a USB drive, not Hdd1:\NetISO.
        databases = os.path.join(xbox, "Hdd1", "Apps", "Aurora", "Data", "Databases")
        os.makedirs(databases)
        db = sqlite3.connect(os.path.join(databases, "content.db"))
        db.execute("CREATE TABLE MountedDevices (Id INTEGER PRIMARY KEY AUTOINCREMENT, DeviceId TEXT, DeviceName TEXT, MountPoint TEXT)")
        db.executemany("INSERT INTO MountedDevices (DeviceId, DeviceName) VALUES (?, ?)", [("A1B2", "Hdd1"), ("C3D4", "Usb0")])
        db.commit(); db.close()
        db = sqlite3.connect(os.path.join(databases, "settings.db"))
        db.execute("CREATE TABLE ScanPaths (Id INTEGER PRIMARY KEY AUTOINCREMENT, Path TEXT, DeviceId TEXT, Depth INTEGER, ScriptData TEXT)")
        db.executemany("INSERT INTO ScanPaths (Path, DeviceId, Depth) VALUES (?, ?, ?)", [("\\Games\\", "A1B2", 2), ("\\Apps", "C3D4", 1), ("\\Gone", "FFFF", 1)])
        db.commit(); db.close()
        ftp = aurora_ftp.Server(xbox, 0, host="127.0.0.2")  # A port free on 127.0.0.2 itself.
        ftp_port = ftp.server_address[1]
        threading.Thread(target=ftp.serve_forever, daemon=True).start()
        try:
            self.start("--xbox", "127.0.0.2", "--ftp-port", str(ftp_port))
            found = self.json("POST", "/api/consoles/find", {"around": "127.0.0.2"})
            self.assertEqual(found["networks"], ["127.0.0.0/24"])
            self.assertEqual([(c["address"], c["aurora"]) for c in found["found"]], [("127.0.0.2", True)])
            self.assertEqual(self.call("POST", "/api/consoles/find", {"around": "xbox"})[0], 400)
            r = self.json("POST", "/api/consoles/check", {"address": "127.0.0.2"})
            self.assertEqual([r[k] for k in ("ftp", "login", "aurora", "gameFolder", "launchIni")], [True, True, True, False, "/Usb0/launch.ini"])
            self.assertEqual(r["default"], "Hdd:\\Apps\\Aurora\\Aurora.xex")
            self.assertEqual(r["scanPaths"], [{"path": "/Hdd1/Games", "depth": 2}, {"path": "/Usb0/Apps", "depth": 1}])
            self.assertEqual((r["scansGames"], r["scansHddGames"]), (False, True))
            netiso, tiles, gone, drive = r["plugins"]
            self.assertEqual([netiso[k] for k in ("slot", "kind", "found", "size", "txt", "server", "here")], [1, "netiso", True, 6, True, "10.0.0.99", False])
            self.assertEqual(netiso["sha256"], hashlib.sha256(b"NETISO").hexdigest())
            self.assertEqual([tiles["kind"], tiles["sha256"]], ["tiles", hashlib.sha256(b"TILES").hexdigest()])
            # The plugin's server as it finds it: no NetDisc360.ini, so NetISO.xex.txt next to it.
            self.assertEqual([tiles[k] for k in ("txt", "server", "serverFrom", "here")], [True, "10.0.0.99", "/Usb0/NetISO.xex.txt", False])
            self.assertEqual([gone["kind"], gone["found"]], ["other", False])
            # The plugin's name since it has its own disc drive, from the hard drive as on an RGH console.
            self.assertEqual([drive["slot"], drive["kind"], drive["found"], drive["sha256"]], [4, "tiles", True, hashlib.sha256(b"DRIVE").hexdigest()])
            # server= in the NetDisc360.ini next to it, the port left off.
            self.assertEqual([drive[k] for k in ("txt", "server", "serverFrom")], [True, "10.0.0.7", "/Hdd1/Plugins/NetDisc360.ini"])
            # A login typed in the wizard is tried before it's saved.
            r = self.json("POST", "/api/consoles/check", {"address": "127.0.0.2", "ftpPassword": "wrong"})
            self.assertEqual([r["ftp"], r["login"]], [True, False])
            self.assertFalse(self.json("POST", "/api/consoles/check", {"address": "127.0.0.3"})["ftp"])
        finally:
            ftp.shutdown()
            ftp.server_close()

    def test_finds_and_repairs_tiles_without_art(self):
        xbox = os.path.join(self.tmp.name, "xbox")
        aurora = os.path.join(xbox, "Hdd1", "Apps", "Aurora", "Data")
        games = os.path.join(xbox, "Hdd1", "NetDisc360", "Games")
        for folder in ("Databases", "GameData"):
            os.makedirs(os.path.join(aurora, folder))
        for folder in ("Halo 3", "Banjo-Kazooie - Nuts & Bolts", "Skate 3", "New Game", "NetISO Sync"):
            os.makedirs(os.path.join(games, folder))
        with open(os.path.join(games, "Banjo-Kazooie - Nuts & Bolts", "netiso.ini"), "w") as f:
            f.write("[NetISO]\r\niso=Banjo-Kazooie - Nuts & Bolts (USA) (En,Ja,Fr).iso\r\n")
        # Aurora's own table, as its content.db has it (tabs and all), on
        # small pages with long rows, so the file has interior and overflow pages.
        db = sqlite3.connect(os.path.join(aurora, "Databases", "content.db"))
        db.execute("PRAGMA page_size=1024")
        db.execute("CREATE TABLE ContentItems ( Id\t\t\tINTEGER PRIMARY KEY AUTOINCREMENT,  Directory\t\tTEXT COLLATE NOCASE,  "
                   "Executable\tTEXT COLLATE NOCASE,  TitleId\t\tINTEGER,  TitleName\t\tTEXT COLLATE NOCASE,  Description\tTEXT COLLATE NOCASE,  "
                   "ScanPathId\tINTEGER, CaseIndex DEFAULT 0,  UNIQUE (Directory, Executable, ScanPathId ) )")
        rows = [("\\apps\\aurora", 0, "Aurora"), ("\\netdisc360\\games\\netiso sync", -45495, "NetISO Sync")]
        rows += [("\\games\\filler %d" % i, i, "Filler %d" % i) for i in range(300)]
        rows += [("\\netdisc360\\games\\halo 3", 0x4D5307E6, "Halo 3"), ("\\NetDisc360\\Games\\Banjo-Kazooie - Nuts & Bolts", 0x4D5307ED, "Banjo-Kazooie: Nuts & Bolts"),
                 ("\\netdisc360\\games\\skate 3", 0x454108E6 - (1 << 32), "Skate 3")]
        for directory, title, name in rows:
            db.execute("INSERT INTO ContentItems (Directory, Executable, TitleId, TitleName, Description, ScanPathId) VALUES (?, 'default.xex', ?, ?, ?, 3)",
                       (directory, title, name, "A long description. " * 200))
        db.commit()
        ids = dict(db.execute("SELECT TitleName, Id FROM ContentItems"))
        db.close()
        # Halo 3 has its cover; Banjo got Aurora's empty one; Skate 3 has no GameData folder.
        for name, title, size in (("Halo 3", 0x4D5307E6, 5000), ("Banjo-Kazooie: Nuts & Bolts", 0x4D5307ED, 2048)):
            folder = os.path.join(aurora, "GameData", "%08X_%08X" % (title, ids[name]))
            os.makedirs(folder)
            with open(os.path.join(folder, "GC%08X.asset" % title), "wb") as f:
                f.write(b"RXEA" + bytes(size - 4))
        ftp = aurora_ftp.Server(xbox, 0, host="127.0.0.2")  # A port free on 127.0.0.2 itself.
        ftp_port = ftp.server_address[1]
        threading.Thread(target=ftp.serve_forever, daemon=True).start()
        try:
            self.start("--xbox", "127.0.0.2", "--ftp-port", str(ftp_port))
            # The console is known once the server's first push to it has
            # begun, which can take a moment after start-up.
            deadline = time.time() + 15
            while self.call("POST", "/api/consoles/art", {"address": "127.0.0.2"})[0] == 404 and time.time() < deadline:
                time.sleep(0.2)
            found = self.json("POST", "/api/consoles/art", {"address": "127.0.0.2"})
            self.assertEqual(found["tiles"], [{"folder": "Banjo-Kazooie - Nuts & Bolts", "title": "Banjo-Kazooie: Nuts & Bolts"},
                                              {"folder": "Skate 3", "title": "Skate 3"}])
            self.assertEqual((found["renamed"], ftp.renamed, ftp.scans), ([], [], []))
            # Just one: its folder gets a new name, and Aurora a scan.
            done = self.json("POST", "/api/consoles/art", {"address": "127.0.0.2", "repair": True, "folders": ["Skate 3"]})
            self.assertEqual(done["renamed"], [{"from": "Skate 3", "to": "Skate 3 (2)"}])
            self.assertNotIn("error", done)
            self.assertEqual(ftp.scans, ["/Hdd1/NetDisc360/Games"])
            # The renamed tile isn't Aurora's yet; the rest are fetched again,
            # under the disc's name with its tags (cut to 42 characters).
            done = self.json("POST", "/api/consoles/art", {"address": "127.0.0.2", "repair": True})
            self.assertEqual([t["folder"] for t in done["tiles"]], ["Banjo-Kazooie - Nuts & Bolts"])
            self.assertEqual(done["renamed"], [{"from": "Banjo-Kazooie - Nuts & Bolts", "to": "Banjo-Kazooie - Nuts & Bolts (USA) (EnJaFr"}])
            self.assertEqual(sorted(os.listdir(games)), ["Banjo-Kazooie - Nuts & Bolts (USA) (EnJaFr", "Halo 3", "NetISO Sync", "New Game", "Skate 3 (2)"])
            self.assertEqual(os.listdir(os.path.join(games, "Banjo-Kazooie - Nuts & Bolts (USA) (EnJaFr")), ["netiso.ini"])
            self.assertEqual(len(ftp.scans), 2)
            self.assertEqual(self.call("POST", "/api/consoles/art", {"address": "127.0.0.3"})[0], 404)
        finally:
            ftp.shutdown()
            ftp.server_close()

    PARTY_CONSOLES = ("--xbox", "10.10.1.41", "--xbox", "10.10.1.42", "--xbox", "10.10.1.43")
    # The teams' colours (TeamPalette in server/party.cpp).
    PALETTE = [{"name": n, "fill": "#" + f, "text": "#" + t, "bright": "#" + b, "ink": i} for n, f, t, b, i in (
        ("Red", "e5483e", "b3261e", "ff6b61", False), ("Blue", "3f7fe6", "2559b0", "6ea8ff", False), ("Green", "2c9a2c", "0e6e0e", "5fd35f", False),
        ("Yellow", "d9a400", "7f6200", "ffd23f", True), ("Orange", "f07c1e", "a8500c", "ffa257", False), ("Purple", "8e52d6", "6a35b0", "b98cff", False),
        ("Pink", "e0509a", "b02a72", "ff8cc6", False), ("Teal", "149c9c", "0b6d6d", "4fd8d8", False))]

    def party_now(self):
        """The party in /api/state, without the guests' numbers and the team colours."""
        party = self.json("GET", "/api/state")["party"]
        for key in ("statsReading", "teamColours", "palette", "started", "feed", "vote", "ready", "phone", "presets"):
            del party[key]
        for g in party["guests"]:
            g.pop("stats", None)
        return party

    def refused(self, path, cases):
        """Each (body, words) in cases is refused (400) with words in its error."""
        for body, words in cases:
            status, _, data = self.call("POST", path, body)
            self.assertEqual(status, 400, (body, data))
            self.assertIn(words, json.loads(data)["error"], body)

    def test_keeps_the_lan_party(self):
        self.start(*self.PARTY_CONSOLES)
        self.assertEqual(self.json("GET", "/api/state")["party"],
                         {"active": False, "name": "", "theme": {"preset": "classic", "accent": "#107c10", "accent2": "#ffffff", "person": "", "age": 0,
                                                                 "logo": False, "backgrounds": 0, "pictures": ""},
                          "gamemaster": "", "consoles": [], "guests": [], "statsReading": False, "teams": 2, "teamColours": [0, 1, 2, 3], "palette": self.PALETTE,
                          "balance": "random", "game": "", "started": 0, "idleOffMinutes": 0, "feed": [], "wins": [], "points": [], "vote": None, "ready": {},
                          "phone": {"relay": "", "code": "", "url": "", "ok": False, "error": "", "joins": 0},
                          "together": [], "apart": [], "round": 0, "drawn": None, "last": [], "spin": None, "signedIn": {}, "wheelOn": [], "profilesTo": {},
                          "games": [], "presets": []})
        # The teams' colours: 4 of the palette's 8, kept; one another team has swaps.
        self.json("POST", "/api/party", {"teamColours": [5, 4, 7, 3]})
        self.assertEqual(self.json("GET", "/api/state")["party"]["teamColours"], [5, 4, 7, 3])
        self.json("POST", "/api/party", {"teamColours": [7]})
        self.assertEqual(self.json("GET", "/api/state")["party"]["teamColours"], [7, 4, 5, 3])
        self.refused("/api/party", [({"teamColours": [8]}, "colour is 0-7"), ({"teamColours": [0, 1, 2, 3, 4]}, "teamColours")])
        self.restart(*self.PARTY_CONSOLES)
        self.assertEqual(self.json("GET", "/api/state")["party"]["teamColours"], [7, 4, 5, 3])
        self.json("POST", "/api/party", {"teamColours": [0, 1, 2, 3]})
        tricky = "Cid | 50%, ok; ="  # Every character the settings file's lists split at.
        self.json("POST", "/api/party", {"active": True, "name": " Bjørn's 40th ", "gamemaster": "10.10.1.42",
                                         "theme": {"preset": "birthday", "person": "Bjørn", "age": 40, "accent": "#FF8800", "accent2": "#1a1a1a"},
                                         "consoles": ["10.10.1.41", "10.10.1.42", "10.10.1.41"],
                                         "guests": [{"name": "Bjørn", "here": True}, {"name": tricky, "here": True}, {"name": "Ann"}], "teams": 3,
                                         "together": [["BJØRN", tricky]], "apart": [["ann", "Bjørn"]], "games": ["Halo 3", tricky, "halo 3 "]})
        party = {"active": True, "name": "Bjørn's 40th", "theme": {"preset": "birthday", "accent": "#ff8800", "accent2": "#1a1a1a", "person": "Bjørn", "age": 40,
                                                                    "logo": False, "backgrounds": 0, "pictures": ""},
                 "gamemaster": "10.10.1.42", "consoles": ["10.10.1.42", "10.10.1.41"],
                 "guests": [{"name": "Bjørn", "here": True}, {"name": tricky, "here": True}, {"name": "Ann", "here": False}], "teams": 3,
                 "balance": "random", "game": "", "idleOffMinutes": 0, "wins": [], "points": [],
                 "together": [["Bjørn", tricky]], "apart": [["Ann", "Bjørn"]], "round": 0, "drawn": None, "last": [], "spin": None, "signedIn": {}, "wheelOn": [], "profilesTo": {},
                 "games": ["Halo 3", tricky]}
        self.assertIn("LAN party started: Bjørn's 40th", [e["text"] for e in self.json("GET", "/api/state")["events"]])
        for _ in range(2):  # As changed, and as saved.
            self.assertEqual(self.party_now(), party)
            self.restart(*self.PARTY_CONSOLES)
        with open(self.config, encoding="utf-8") as f:
            saved = f.read()
        self.assertIn("party_guest2=1|Cid %7C 50%25%2C ok%3B %3D\n", saved)
        self.assertIn("party_consoles=10.10.1.42, 10.10.1.41\n", saved)
        # Kept in a backup. The guest list stays from one party to the next
        # (with no one here yet), until it's typed again; rules about guests
        # who aren't on it go.
        name = self.json("POST", "/api/backup", {})["name"]
        self.json("POST", "/api/party", {"active": False})
        self.assertEqual(self.party_now()["guests"], [dict(g, here=False) for g in party["guests"]])
        self.json("POST", "/api/party", {"guests": [{"name": "Ann", "here": True}, {"name": "Dee"}]})
        changed = self.party_now()
        self.assertEqual((changed["guests"], changed["together"], changed["apart"]), ([{"name": "Ann", "here": True}, {"name": "Dee", "here": False}], [], []))
        self.json("POST", "/api/restore", {"name": name})
        self.assertEqual(self.party_now(), party)
        # What isn't right is refused, and changes nothing.
        self.refused("/api/party", [
            ([], "JSON object"),
            ({"active": "yes"}, "active is true or false."),
            ({"name": "x" * 65}, "The party's name is 65 characters long; 64 at most."),
            ({"teams": 5}, "teams is a whole number from 2 to 4."),
            ({"teams": 2.5}, "teams is a whole number from 2 to 4."),
            ({"theme": {"preset": "space"}}, "classic, birthday, company or retro"),
            ({"theme": {"accent": "red"}}, "written like #107c10"),
            ({"theme": {"accent2": "#12345g"}}, "written like #107c10"),
            ({"theme": {"age": 151}}, "age is a whole number from 0 to 150."),
            ({"theme": {"person": "x" * 33}}, "The birthday person's name is 33 characters long"),
            ({"guests": [{"name": " "}]}, "A guest's name can't be empty."),
            ({"guests": [{"name": "ø" * 33}]}, "A guest's name is 33 characters long; 32 at most."),
            ({"guests": [{"name": "Ann\nBob"}]}, "control character"),
            ({"guests": [{"name": "Ann"}, {"name": "ANN"}]}, "ANN is on the guest list twice."),
            ({"guests": [{"name": "Øyvind"}, {"name": "øyvind"}]}, "øyvind is on the guest list twice."),
            ({"guests": [{"name": "Ann", "here": "yes"}]}, "here is true or false."),
            ({"guests": ["Ann"]}, "guests is a list of {name, here}."),
            ({"guests": [{"name": "Guest %d" % i} for i in range(65)]}, "64 guests at most"),
            ({"consoles": ["10.10.1.99"]}, "10.10.1.99 isn't a console NetDisc360 knows."),
            ({"consoles": "10.10.1.41"}, "consoles is a list of console addresses."),
            ({"gamemaster": "0.0.0.0"}, "isn't a console NetDisc360 knows."),
            # A known console written another way than 10.10.1.42, which inet_addr takes too.
            ({"consoles": ["10.10.1.42 x"]}, "isn't a console NetDisc360 knows."),
            ({"consoles": ["10.10.1.42\tjunk"]}, "isn't a console NetDisc360 knows."),
            ({"consoles": ["10.10.298"]}, "isn't a console NetDisc360 knows."),
            ({"consoles": ["168427818"]}, "isn't a console NetDisc360 knows."),
            ({"consoles": ["012.012.01.052"]}, "isn't a console NetDisc360 knows."),
            ({"gamemaster": "10.10.1.42\n12:00:00 Fake: the dashboard's password was removed."}, "isn't a console NetDisc360 knows."),
            ({"together": [["Ann", "Zed"]]}, "Zed isn't on the guest list."),
            ({"together": [["Ann"]]}, "together is a list of pairs of guests' names."),
            ({"apart": [["Ann", "ANN"]]}, "Ann can't be kept apart from themselves."),
            ({"together": [["Ann", "Bjørn"]], "apart": [["Bjørn", "Ann"]]}, "can't be kept together and apart at once."),
        ])
        self.assertEqual(self.party_now(), party)
        self.assertNotIn("Fake:", json.dumps(self.json("GET", "/api/log?after=0")))

    def party_text(self, address, version=0, wait_ms=0):
        """The party as NetDiscParty.xex reads it (PartyWait), from address."""
        with connect_from(address, self.port) as conn:
            conn.sendall(b"ISVR" + struct.pack(">HHQI", 0x5057, 0, version, wait_ms))
            size = struct.unpack(">I", exact(conn, 4))[0]
            text = exact(conn, size).decode("utf-8")
        lines = {}
        for line in text.splitlines():
            key, *fields = line.split("\t")
            lines.setdefault(key, []).append(fields)
        return text, lines

    def spin_ask(self, address, index, teams=0):
        """The Gamemaster's console spinning the wheel (PartySpinAsk): its answer."""
        with connect_from(address, self.port) as conn:
            conn.sendall(b"ISVR" + struct.pack(">HHQI", 0x5053, index, 0, teams))
            size = struct.unpack(">I", exact(conn, 4))[0]
            return exact(conn, size).decode("utf-8")

    def check_landings(self, spin):
        """Each turn stops inside its own segment, clockwise, never on a line."""
        width = 360 / len(spin["wheel"])
        before = 0
        for step in spin["steps"]:
            self.assertGreater(step["to"], before + 360)  # A whole turn at least.
            under = (90 - step["to"]) % 360 / width  # The pointer is at the right.
            self.assertGreater(under, step["segment"] + 0.19)
            self.assertLess(under, step["segment"] + 0.81)
            before = step["to"]

    def test_raffle_wheel(self):
        consoles = self.PARTY_CONSOLES + ("--xbox", "127.0.0.2")
        self.start(*consoles)
        here = ["Ann", "Bob", "Cid", "Dee", "Eve", "Fay", "Øyvind"]
        self.json("POST", "/api/party", {"active": True, "name": "Wheel night", "gamemaster": "127.0.0.2", "consoles": ["10.10.1.42"], "teams": 2,
                                         "guests": [{"name": n, "here": True} for n in here] + [{"name": "Kim"}], "together": [["Ann", "Bob"]]})
        # The Gamemaster's console picks one guest; another console can't.
        self.assertEqual(self.spin_ask("127.0.0.1", 2), "error\tThis isn't one of NetDisc360's Xboxes.\n")
        self.assertEqual(self.spin_ask("127.0.0.2", 3), "error\tAsk for teams or one guest.\n")
        self.assertEqual(self.spin_ask("127.0.0.2", 2), "ok\n")
        spin = self.json("GET", "/api/state")["party"]["spin"]
        self.assertEqual((spin["kind"], sorted(spin["wheel"]), len(spin["steps"]), spin["steps"][0]["team"]), ("pick", sorted(here), 1, -1))
        self.assertEqual((spin["leadMs"], spin["pauseMs"], spin["totalMs"]), (1500, 2500, 1500 + spin["steps"][0]["ms"]))
        self.assertTrue(0 <= spin["elapsed"] < 5000, spin["elapsed"])
        self.check_landings(spin)
        self.assertIsNone(self.json("GET", "/api/state")["party"]["drawn"])  # A pick draws no teams.
        # One spin at a time, from either side.
        self.refused("/api/party/spin", [({}, "The wheel is still turning.")])
        self.assertEqual(self.spin_ask("127.0.0.2", 1), "error\tThe wheel is still turning.\n")
        # A draw without the wheel ends it.
        self.json("POST", "/api/party/raffle", {"seed": 1})
        self.assertIsNone(self.json("GET", "/api/state")["party"]["spin"])
        # Teams on the wheel: the same draw as the raffle's, a stop for each guest, each team's first, then each team's second.
        drawn = self.json("POST", "/api/party/spin", {"teams": 3, "seed": 5})
        self.assertEqual(self.json("POST", "/api/party/raffle", {"teams": 3, "seed": 5})["teams"], self.json("GET", "/api/state")["party"]["drawn"]["teams"])
        spin = drawn["spin"]
        self.assertEqual((spin["kind"], spin["teams"], sorted(spin["wheel"]), drawn["note"]), ("teams", 3, sorted(here), ""))
        party = self.json("GET", "/api/state")["party"]
        self.assertIsNone(party["spin"])  # The raffle just above replaced it.
        drawn = self.json("POST", "/api/party/spin", {"teams": 3, "seed": 5})
        spin, party = drawn["spin"], self.json("GET", "/api/state")["party"]
        self.assertEqual(party["spin"]["id"], spin["id"])
        teams = party["drawn"]["teams"]
        picked = [[spin["wheel"][s["segment"]] for s in spin["steps"] if s["team"] == t] for t in range(3)]
        self.assertEqual([sorted(t) for t in picked], [sorted(t) for t in teams])
        self.assertEqual([s["team"] for s in spin["steps"]], [0, 1, 2, 0, 1, 2, 0])
        self.assertEqual(sorted(spin["wheel"][s["segment"]] for s in spin["steps"]), sorted(here))
        self.assertEqual([s["ms"] for s in spin["steps"]], [7500] + [4000] * 6)
        self.check_landings(spin)
        team_of = {n: i for i, t in enumerate(teams) for n in t}
        self.assertEqual(team_of["Ann"], team_of["Bob"])
        # What the console reads: its own name, the party, the wheel, the draw and the seats.
        text, lines = self.party_text("127.0.0.2")
        self.assertEqual(lines["netdisc-party"], [["1"]])
        self.assertEqual(lines["console"], [["Console 1", "127.0.0.2", "1"]])
        self.assertEqual(lines["party"], [["1", "Wheel night", "LAN party", "classic"]])
        self.assertEqual(lines["here"], [here])
        self.assertEqual(lines["wheel"], [spin["wheel"]])
        self.assertEqual(lines["spin"][0][:2] + lines["spin"][0][3:], [str(spin["id"]), "teams", "1500", "2500", ""])
        self.assertEqual([[int(s[0]), int(s[1]), float(s[2]), int(s[3])] for s in lines["step"]], [[s["segment"], s["team"], s["to"], s["ms"]] for s in spin["steps"]])
        self.assertEqual(lines["team"], teams)
        self.assertEqual([s[:2] for s in lines["seat"]], [["127.0.0.2", "Console 1"], ["10.10.1.42", "Console 2"]])
        self.assertEqual(lines["round"], [["1", "0"]])
        # NetDiscParty.xex reads the same (party/state.cpp), and turns its wheel as the server plans it.
        moments = [0, 1499, 1500, 4000, 8999, 9000, 10000, 11499, 11500, 13000, spin["totalMs"] - 1, spin["totalMs"], spin["totalMs"] + 5000]
        read = subprocess.run([build_party_parse()] + [str(m) for m in moments], input=text.encode("utf-8"), stdout=subprocess.PIPE, check=True).stdout.decode("utf-8")
        self.assertIn("valid 1\nversion %s\nconsole Console 1|127.0.0.2|1\nparty 1|Wheel night|LAN party\n" % lines["version"][0][0], read)
        self.assertIn("spin %d|teams|1500|2500|%d\nwheel 7\nsteps 7\nteams 3\nseats 2\n" % (spin["id"], spin["totalMs"]), read)
        mine = [n for n in lines["seat"][0][2:]]
        self.assertIn("mine Console 1|%d|%d\n" % (len(mine), team_of[mine[0]]), read)
        for line in read.splitlines():
            if not line.startswith("at "):
                continue
            _, ms, angle, step, stopped, done = line.split()
            want = spin_at(spin, float(ms))
            self.assertAlmostEqual(float(angle), want[0], places=2, msg=line)
            self.assertEqual((int(step), int(stopped), int(done)), want[1:], line)
        version = int(lines["version"][0][0])
        # Waiting on a version it has: the answer comes once it changes, or when the wait ends.
        started = time.time()
        _, again = self.party_text("127.0.0.2", version, 400)
        self.assertGreaterEqual(time.time() - started, 0.35)
        self.assertEqual(again["version"], [[str(version)]])
        threading.Timer(0.3, lambda: self.json("POST", "/api/party/keep", {})).start()
        started = time.time()
        _, kept = self.party_text("127.0.0.2", version, 10000)
        self.assertLess(time.time() - started, 5)
        self.assertGreater(int(kept["version"][0][0]), version)
        self.assertEqual(kept["round"], [["1", "1"]])
        # Not a NetISO connection: the console isn't counted as one, so not listed as connected.
        self.assertEqual([c["connections"] for c in self.json("GET", "/api/state")["consoles"] if c["address"] == "127.0.0.2"], [0])
        # What isn't right is refused.
        self.json("POST", "/api/party/raffle", {})
        self.refused("/api/party/spin", [({"kind": "colours"}, "kind is teams or pick."), ({"teams": 5}, "teams is a whole number from 2 to 4."),
                                         ({"kind": "pick", "title": "x" * 65}, "65 characters long; 64 at most."), ([], "JSON object")])
        self.json("POST", "/api/party", {"guests": [{"name": "Ann", "here": True}]})
        self.refused("/api/party/spin", [({"kind": "pick"}, "Tick at least two guests who are here")])
        self.json("POST", "/api/party", {"active": False})
        # The Gamemaster's console starts the party again to spin; no one's here yet.
        self.assertIn("No one is here yet", self.spin_ask("127.0.0.2", 2))

    def sign_in(self, address, seat, controller, gamertag=b"", xuid=0, out=False):
        """NetDiscParty.xex saying who signed in (PartySignIn): its answer."""
        with connect_from(address, self.port) as conn:
            index = seat | (controller << 8) | (0x8000 if out else 0)
            conn.sendall(b"ISVR" + struct.pack(">HHQI", 0x5049, index, xuid, len(gamertag)) + gamertag)
            size = struct.unpack(">I", exact(conn, 4))[0]
            return exact(conn, size).decode("utf-8")

    def edit_ask(self, address, text):
        """A change to the party from a console's party screen (PartyEditAsk): its answer."""
        data = text.encode("utf-8")
        with connect_from(address, self.port) as conn:
            conn.sendall(b"ISVR" + struct.pack(">HHQI", 0x5045, 0, 0, len(data)) + data)
            size = struct.unpack(">I", exact(conn, 4))[0]
            return exact(conn, size).decode("utf-8")

    def test_who_won_points_and_fair_teams(self):
        self.start(*self.PARTY_CONSOLES + ("--xbox", "127.0.0.2", "--xbox", "127.0.0.3"))
        names = ["Ann", "Bob", "Cid", "Dag", "Eva", "Fia"]
        self.json("POST", "/api/party", {"active": True, "gamemaster": "127.0.0.2", "consoles": ["127.0.0.2", "127.0.0.3"],
                                         "guests": [{"name": n, "here": True} for n in names]})
        party = self.json("GET", "/api/state")["party"]
        self.assertEqual((party["balance"], party["wins"], party["points"], party["game"]), ("random", [], [], ""))
        self.assertGreater(party["started"], 0)
        self.refused("/api/party/winner", [({"team": 0}, "Draw the teams first"), ({"team": 7}, "")])
        drawn = self.json("POST", "/api/party/raffle", {"teams": 2, "seed": 5})["teams"]
        said = self.json("POST", "/api/party/winner", {"team": 0})["said"]
        self.assertEqual(said, "Team Red won round 1: a point each for %s, %s and %s." % tuple(drawn[0]))
        # A second press at once is the same win.
        self.refused("/api/party/winner", [({"team": 0}, "Team Red's win is counted already.")])
        party = self.json("GET", "/api/state")["party"]
        self.assertEqual([(w["round"], w["team"], w["players"]) for w in party["wins"]], [(1, "Team Red", drawn[0])])
        self.assertEqual(party["points"], [{"name": n, "points": 1} for n in sorted(drawn[0])])
        # Taken back, then said again; the console sees the points (PartyText).
        self.assertEqual(self.json("POST", "/api/party/winner", {"team": -1})["said"], "Team Red's win in round 1 is taken back.")
        self.assertEqual(self.json("GET", "/api/state")["party"]["wins"], [])
        self.json("POST", "/api/party/winner", {"team": 1})
        _, lines = self.party_text("127.0.0.2")
        self.assertEqual(lines["points"], [[n, "1"] for n in sorted(drawn[1])])
        self.assertEqual(lines["won"], [["1", "Team Blue", ""]])
        self.assertEqual(lines["balance"], [["random"]])
        # Fair teams by wins: the three with a point are spread over the teams, every draw.
        self.json("POST", "/api/party", {"balance": "wins"})
        self.refused("/api/party", [({"balance": "height"}, "balance is random, wins or gamerscore")])
        for seed in range(6):
            teams = self.json("POST", "/api/party/raffle", {"teams": 2, "seed": seed})["teams"]
            winners = [len([n for n in t if n in drawn[1]]) for t in teams]
            self.assertEqual(sorted(winners), [1, 2], (seed, teams))
        # From the Gamemaster's console too; the winners are kept over a restart and cleared when a new party starts.
        self.assertEqual(self.edit_ask("127.0.0.2", "balance\tgamerscore"), "ok\t\n")
        red = self.json("GET", "/api/state")["party"]["drawn"]["teams"][0]
        self.assertEqual(self.edit_ask("127.0.0.2", "winner\t0"), "ok\tTeam Red won round 2: a point each for %s, %s and %s.\n" % tuple(red))
        self.assertEqual(self.edit_ask("127.0.0.3", "winner\t1"), "error\tOnly the Gamemaster's console sets up the party.\n")
        self.assertEqual(self.json("GET", "/api/state")["party"]["balance"], "gamerscore")
        self.json("POST", "/api/party", {"active": False})
        self.assertEqual(len(self.json("GET", "/api/state")["party"]["wins"]), 2)  # The recap shows them.
        self.json("POST", "/api/party", {"active": True})
        self.assertEqual(self.json("GET", "/api/state")["party"]["wins"], [])
        # Welcome back with what they did at earlier LANs, on the console and in the live feed.
        self.assertEqual(self.edit_ask("127.0.0.3", "join\tAnn"), "ok\tWelcome back, Ann! This is your 2nd LAN.\n")
        self.assertEqual(self.edit_ask("127.0.0.3", "join\tGus"), "ok\tWelcome, Gus!\n")
        self.assertEqual([(x["kind"], x["text"]) for x in self.json("GET", "/api/state")["party"]["feed"]],
                         [("join", "Gus joined the party"), ("join", "Welcome back, Ann! This is your 2nd LAN.")])

    def test_players_pick_their_own_profile_in_the_party_app(self):
        # Marcus 2026-11-05: in the party app a player picks who they are,
        # also a user whose profile is only kept on the PC (synced from
        # another computer); it comes to the console with the others.
        self.start("--xbox", "127.0.0.2", "--xbox", "127.0.0.3")
        self.assertEqual(self.edit_ask("127.0.0.2", "gamemaster\t127.0.0.2"), "ok\t127.0.0.2 is the Gamemaster.\n")
        later = int(time.time())
        text = ("[Users]\r\ncount=3\r\n"
                "user1=u00000000000000a1|Marcus|GERYHA|E00000000000A003|1700000000|1|%d|0|48230|900|0|0|0|4|1700000100|Halo 3\r\n"
                "user2=u00000000000000a2|Nora|Nora|E00000000000B001|1700000000|0|%d|0|-1|-1|0|0|0|1|1700000200|\r\n"
                "user3=u00000000000000a3|Per||||0|%d|0|-1|-1|0|0|0|0|0|\r\n") % (later, later, later)
        self.json("POST", "/api/users", {"action": "import", "text": text})
        # Marcus's profile is kept on this PC (as the sync brings it); Nora's is nowhere.
        kept = os.path.join(self.json("GET", "/api/users")["folder"], "E00000000000A003")
        os.makedirs(kept)
        with open(os.path.join(kept, "2026-11-01_20-00-00_10.10.1.44.zip"), "wb") as f:
            f.write(b"PK")
        with open(os.path.join(kept, "2026-11-01_20-00-00_10.10.1.44.ini"), "w") as f:
            f.write("file=2026-11-01_20-00-00_10.10.1.44.zip\nname=Marcus\ngamertag=GERYHA\nxuid=E00000000000A003\nconsole=10.10.1.44\ntime=1762020000\ngamerscore=48230\n")
        text, lines = self.party_text("127.0.0.3")
        # Those at a party latest first; where each one's profile is for this console.
        self.assertEqual(lines["user"], [["Nora", "Nora", "E00000000000B001", "-1", "new", ""],
                                         ["Marcus", "GERYHA", "E00000000000A003", "48230", "pc", ""],
                                         ["Per", "", "", "-1", "new", ""]])
        # NetDiscParty.xex reads them, and finds the user of the profile signed in on a controller.
        parsed = subprocess.run([build_party_parse(), "userof=e00000000000a003=Someone", "userof==geryha", "userof=E0000000000FFFFF=Nobody"],
                                input=text.encode("utf-8"), capture_output=True, check=True).stdout.decode("utf-8").splitlines()
        self.assertIn("user Marcus|GERYHA|E00000000000A003|48230|pc|", parsed)
        self.assertIn("user Per|||-1|new|", parsed)
        self.assertEqual([l for l in parsed if l.startswith("userof")],
                         ["userof e00000000000a003|Someone 1", "userof |geryha 1", "userof E0000000000FFFFF|Nobody -1"])
        # Picked: they join by their name as the Users page has it, and are told their profile comes.
        self.assertEqual(self.edit_ask("127.0.0.3", "join\tmarcus"),
                         "ok\tWelcome back, Marcus! This is your 5th LAN: 48230 G so far. Your profile comes to your Xbox after the wheel.\n")
        self.assertEqual([(g["name"], g["here"]) for g in self.json("GET", "/api/state")["party"]["guests"]], [("Marcus", True)])
        # Their own profile, not a new one: it's on no console yet, and the kept copy goes to the seat.
        record = next(g for g in self.json("GET", "/api/party/profiles")["profiles"] if g["name"] == "Marcus")
        self.assertEqual((record["xuid"], record["gamertag"]), ("E00000000000A003", "GERYHA"))
        self.assertEqual(self.edit_ask("127.0.0.3", "join\tGus"), "ok\tWelcome, Gus!\n")

    def test_party_from_the_console(self):
        self.start(*self.PARTY_CONSOLES + ("--xbox", "127.0.0.2", "--xbox", "127.0.0.3"))
        # No Gamemaster yet: the app shows the server's build, the palette and the consoles to choose from.
        _, lines = self.party_text("127.0.0.2")
        build = lines["build"][0]
        self.assertEqual((len(build), build[3]), (4, "0"))
        self.assertTrue(build[0].isdigit() and build[2].startswith("1."), build)
        self.assertEqual(lines["palette"], [[str(i), c["name"], c["fill"], c["bright"], "1" if c["ink"] else "0"] for i, c in enumerate(self.PALETTE)])
        self.assertEqual(lines["teamcolours"], [["0", "1", "2", "3"]])
        self.assertNotIn("gamemaster", lines)
        self.assertIn(["127.0.0.2", "127.0.0.2", "wheel", "0"], lines["known"])
        # Anyone joins by typing a name; another console (not the Gamemaster's) can't set the party up.
        self.assertEqual(self.edit_ask("127.0.0.2", "gamemaster\t127.0.0.2"), "ok\t127.0.0.2 is the Gamemaster.\n")
        party = self.json("GET", "/api/state")["party"]
        self.assertEqual((party["active"], party["gamemaster"], party["consoles"]), (True, "127.0.0.2", ["127.0.0.2"]))
        self.party_text("127.0.0.3")
        self.assertEqual(self.edit_ask("127.0.0.3", "join\t Ann "), "ok\tWelcome, Ann!\n")
        self.assertEqual(self.edit_ask("127.0.0.3", "join\tann"), "ok\tWelcome back, Ann!\n")
        self.assertEqual(self.edit_ask("127.0.0.3", "add\tBob"), "error\tOnly the Gamemaster's console adds players; join with A.\n")
        self.assertEqual(self.edit_ask("127.0.0.3", "teams\t3"), "error\tOnly the Gamemaster's console sets up the party.\n")
        self.assertEqual(self.edit_ask("127.0.0.2", "add\tBob"), "ok\tWelcome, Bob!\n")
        self.assertEqual(self.edit_ask("127.0.0.2", "add\tCid"), "ok\tWelcome, Cid!\n")
        self.assertEqual(self.edit_ask("127.0.0.2", "here\tCid\t0"), "ok\t\n")
        self.assertEqual(self.edit_ask("127.0.0.2", "teams\t3"), "ok\t\n")
        self.assertEqual(self.edit_ask("127.0.0.2", "teams\t5"), "error\tDraw 2, 3 or 4 teams.\n")
        self.assertEqual(self.edit_ask("127.0.0.2", "colour\t0\t6"), "ok\t\n")
        self.assertEqual(self.edit_ask("127.0.0.2", "colour\t1\t6"), "ok\t\n")  # Team 1 had pink: they swap.
        self.assertEqual(self.edit_ask("127.0.0.2", "colour\t0\t8"), "error\tThat isn't a team's colour.\n")
        self.assertEqual(self.edit_ask("127.0.0.2", "console\t127.0.0.3\t1"), "ok\t\n")
        self.assertEqual(self.edit_ask("127.0.0.2", "console\t127.0.0.2\t0"), "error\tThe Gamemaster's console is always in the party.\n")
        self.assertEqual(self.edit_ask("127.0.0.2", "dance"), "error\tNetDisc360 doesn't know that change (dance); is it older than this app?\n")
        self.assertEqual(self.edit_ask("127.0.0.1", "join\tEve"), "error\tThis isn't one of NetDisc360's Xboxes.\n")
        party = self.json("GET", "/api/state")["party"]
        self.assertEqual([(g["name"], g["here"]) for g in party["guests"]], [("Ann", True), ("Bob", True), ("Cid", False)])
        self.assertEqual((party["teams"], party["teamColours"], party["consoles"]), (3, [1, 6, 2, 3], ["127.0.0.2", "127.0.0.3"]))
        # Each console sees the guest list (with who joined on it) and the team colours.
        _, lines = self.party_text("127.0.0.3")
        self.assertEqual(lines["guest"], [["1", "Ann", "-1", "1"], ["1", "Bob", "-1", "0"], ["0", "Cid", "-1", "0"]])
        self.assertEqual((lines["teamcolours"], lines["teams"]), ([["1", "6", "2", "3"]], [["3"]]))
        self.assertEqual(lines["gamemaster"], [["Console 1", "127.0.0.2"]])
        # NetDiscParty.xex reads it so (party/state.cpp).
        text, _ = self.party_text("127.0.0.3")
        read = subprocess.run([build_party_parse()], input=text.encode("utf-8"), stdout=subprocess.PIPE, check=True).stdout.decode("utf-8").splitlines()
        self.assertIn("build %s|%s|%s|0" % tuple(build[:3]), read)
        self.assertEqual([l for l in read if l.startswith(("teamcolour ", "palette ", "guest "))],
                         ["teamcolour 0 Blue|3f7fe6|6ea8ff|0", "teamcolour 1 Pink|e0509a|ff8cc6|0", "teamcolour 2 Green|2c9a2c|5fd35f|0", "teamcolour 3 Yellow|d9a400|ffd23f|1",
                          "palette 8", "guest Ann|1|-1|1", "guest Bob|1|-1|0", "guest Cid|0|-1|0"])
        self.assertIn("known 127.0.0.3|127.0.0.3|wheel|1", read)
        # Leaving keeps the name on the list for the next LAN.
        self.assertEqual(self.edit_ask("127.0.0.3", "leave\tBob"), "ok\tBob left the party.\n")
        self.assertEqual([(g["name"], g["here"]) for g in self.json("GET", "/api/state")["party"]["guests"]], [("Ann", True), ("Bob", False), ("Cid", False)])
        # Open on all: every console that shows the party already, so none opens; the gamerscores are read in the background.
        self.assertEqual(self.json("POST", "/api/party/open", {}), {"ok": True, "opened": 0})
        self.assertEqual(self.edit_ask("127.0.0.2", "openall"), "ok\tEvery console that's on shows the party already.\n")
        self.assertEqual(self.edit_ask("127.0.0.2", "backall"), "ok\tEvery party console that's on shows the party already.\n")
        self.assertTrue(self.json("POST", "/api/party/stats", {})["ok"])
        for _ in range(50):
            if not self.json("GET", "/api/state")["party"]["statsReading"]:
                break
            time.sleep(0.1)
        self.assertFalse(self.json("GET", "/api/state")["party"]["statsReading"])

    def test_wheel_for_consoles_and_sign_in(self):
        self.start(*self.PARTY_CONSOLES + ("--xbox", "127.0.0.2", "--xbox", "127.0.0.3"))
        here = ["Ann", "Bob", "Cid", "Dee", "Eve", "Fay"]
        self.json("POST", "/api/party", {"active": True, "gamemaster": "127.0.0.2", "consoles": ["127.0.0.2"], "teams": 2,
                                         "guests": [{"name": n, "here": True} for n in here]})
        self.refused("/api/party/spin", [({"consoles": ["10.9.9.9"]}, "isn't a console NetDisc360 knows.")])
        # Both consoles show the wheel already (they wait on the server), so opening it there is done.
        self.party_text("127.0.0.2")
        self.party_text("127.0.0.3")
        drawn = self.json("POST", "/api/party/spin", {"teams": 2, "seed": 3, "consoles": ["127.0.0.3", "127.0.0.2"], "open": True})
        self.assertEqual(drawn["spin"]["leadMs"], 12000)  # A wait while the consoles open it.
        self.assertEqual([(o["address"], o["ok"], o["how"]) for o in drawn["opened"]], [("127.0.0.3", True, "showing"), ("127.0.0.2", True, "showing")])
        party = self.json("GET", "/api/state")["party"]
        self.assertEqual(party["consoles"], ["127.0.0.2", "127.0.0.3"])  # .3 joined the party, after the Gamemaster's.
        self.assertEqual(sorted(party["wheelOn"]), ["127.0.0.2", "127.0.0.3"])
        seats = party["drawn"]["seats"]
        self.assertEqual((list(seats), [len(x) for x in seats.values()]), (["127.0.0.2", "127.0.0.3"], [3, 3]))
        self.assertEqual(party["signedIn"], {"127.0.0.2": [None, None, None], "127.0.0.3": [None, None, None]})
        # A guest signs in on controller 2 of .3: the dashboard and the console see it.
        first = seats["127.0.0.3"][0]
        self.assertEqual(self.sign_in("127.0.0.3", 0, 1, b"Gamer Ann", 0xE000000000000001), "ok\n")
        self.assertEqual(self.json("GET", "/api/state")["party"]["signedIn"]["127.0.0.3"], [{"controller": 2, "gamertag": "Gamer Ann"}, None, None])
        _, lines = self.party_text("127.0.0.3")
        self.assertEqual(lines["signin"], [["0", "1", "Gamer Ann"]])
        self.assertNotIn("signin", self.party_text("127.0.0.2")[1])
        # The same controller on another seat moves; a seat no one sits in, a fifth controller and a PC are refused.
        self.assertEqual(self.sign_in("127.0.0.3", 2, 1, b"Gamer Ann"), "ok\n")
        self.assertEqual(self.json("GET", "/api/state")["party"]["signedIn"]["127.0.0.3"], [None, None, {"controller": 2, "gamertag": "Gamer Ann"}])
        self.assertEqual(self.sign_in("127.0.0.3", 3, 0, b"X"), "error\tNo one sits in that seat on this console.\n")
        self.assertEqual(self.sign_in("127.0.0.3", 0, 4), "error\tThere are four controllers.\n")
        self.assertEqual(self.sign_in("127.0.0.1", 0, 0, b"X"), "error\tThis isn't one of NetDisc360's Xboxes.\n")
        # Signed out: gone.
        self.assertEqual(self.sign_in("127.0.0.3", 0, 1, out=True), "ok\n")
        self.assertEqual(self.json("GET", "/api/state")["party"]["signedIn"]["127.0.0.3"], [None, None, None])
        # A sign-in counts while its guest keeps the seat: a swap moves it out.
        self.sign_in("127.0.0.3", 0, 0, b"P1")
        swapped = {"127.0.0.2": seats["127.0.0.2"], "127.0.0.3": seats["127.0.0.3"][1:]}
        self.json("POST", "/api/party/seats", {"seats": swapped})
        self.assertEqual(self.json("GET", "/api/state")["party"]["signedIn"]["127.0.0.3"], [None, None])
        self.assertEqual(first, seats["127.0.0.3"][0])
        # The console seats a guest profile by its gamertag (party/state.cpp), as NetDisc360 names them.
        tags = [("Kristoffer H", "Kristoffer Hansen", "1"), ("Oyvind", "Øyvind", "1"), ("AEsa", "Æsa", "1"), ("ola", "OLA", "1"), ("Ola", "Olav", "0"), ("KIOSK 1", "Ola", "0")]
        read = subprocess.run([build_party_parse()] + ["tag=%s=%s" % (t, n) for t, n, _ in tags], input=b"", stdout=subprocess.PIPE, check=True).stdout.decode("utf-8")
        for t, n, want in tags:
            self.assertIn("tag %s|%s %s\n" % (t, n, want), read)

    def test_raffle_follows_the_rules(self):
        self.start(*self.PARTY_CONSOLES)
        here = ["Ann", "Bob", "Cid", "Dee", "Eve", "Fay", "Gus", "Hal", "Ivy", "Jon"]
        self.json("POST", "/api/party", {"active": True, "gamemaster": "10.10.1.42", "consoles": ["10.10.1.41", "10.10.1.43"], "teams": 3,
                                         "guests": [{"name": n, "here": True} for n in here] + [{"name": "Kim", "here": False}],
                                         "together": [["Ann", "Bob"], ["Bob", "Cid"]], "apart": [["Dee", "Eve"], ["Ann", "Fay"]]})
        self.refused("/api/party/keep", [({}, "Draw the teams first.")])
        last = None
        for seed in range(50):
            drawn = self.json("POST", "/api/party/raffle", {"seed": seed})
            teams = drawn["teams"]
            self.assertEqual((drawn["ok"], drawn["round"], drawn["note"]), (True, seed + 1, ""), seed)
            self.assertEqual(sorted(len(t) for t in teams), [3, 3, 4], seed)  # Within one of each other.
            self.assertEqual(sorted(n for t in teams for n in t), here, seed)  # Kim isn't here.
            team_of = {n: i for i, t in enumerate(teams) for n in t}
            self.assertTrue(team_of["Ann"] == team_of["Bob"] == team_of["Cid"], seed)
            self.assertNotEqual(team_of["Dee"], team_of["Eve"], seed)
            self.assertNotEqual(team_of["Ann"], team_of["Fay"], seed)
            as_set = sorted(sorted(t) for t in teams)
            self.assertNotEqual(as_set, last, seed)  # Not last round's teams again.
            # A team to a console, the biggest on the Gamemaster's.
            seats = drawn["seats"]
            self.assertEqual(list(seats), ["10.10.1.42", "10.10.1.41", "10.10.1.43"])
            self.assertEqual(len(seats["10.10.1.42"]), 4)
            self.assertEqual(sorted(sorted(s) for s in seats.values()), as_set)
            self.assertEqual(self.json("GET", "/api/state")["party"]["drawn"], {"teams": teams, "seats": seats, "note": "", "kept": False})
            self.assertEqual(self.json("POST", "/api/party/keep", {}), {"ok": True, "round": seed + 1})
            self.assertEqual(self.json("POST", "/api/party/keep", {}), {"ok": True, "round": seed + 1})  # Once.
            last = as_set
        party = self.json("GET", "/api/state")["party"]
        self.assertEqual((party["round"], sorted(sorted(t) for t in party["last"]), party["drawn"]["kept"]), (50, last, True))
        # The same seed draws the same teams.
        first = self.json("POST", "/api/party/raffle", {"seed": 7})
        self.assertEqual(self.json("POST", "/api/party/raffle", {"seed": 7}), first)
        self.assertEqual(first["round"], 51)
        # Kept across a restart: the draw and last round's teams.
        self.restart(*self.PARTY_CONSOLES)
        party = self.json("GET", "/api/state")["party"]
        self.assertEqual((party["round"], sorted(sorted(t) for t in party["last"]), party["drawn"]["teams"], party["drawn"]["seats"]),
                         (50, last, first["teams"], first["seats"]))
        # Rules that can't all hold: the best draw, and why.
        self.json("POST", "/api/party", {"teams": 4, "together": [["Ann", "Bob"], ["Bob", "Cid"], ["Cid", "Dee"]], "apart": []})
        drawn = self.json("POST", "/api/party/raffle", {})
        self.assertEqual(sorted(len(t) for t in drawn["teams"]), [2, 2, 3, 3])
        self.assertIn("couldn't be kept together.", drawn["note"])
        # Two here and two teams: there's only one draw, so it comes again, and says so.
        self.json("POST", "/api/party", {"guests": [{"name": "Ann", "here": True}, {"name": "Bob", "here": True}, {"name": "Cid"}], "teams": 2,
                                         "together": [], "apart": []})
        self.json("POST", "/api/party/raffle", {})
        self.json("POST", "/api/party/keep", {})
        self.assertEqual(self.json("POST", "/api/party/raffle", {})["note"], "These are last round's teams again: no other draw follows the rules.")
        # Too few here, or too few consoles.
        self.refused("/api/party/raffle", [({"teams": 3}, "Only 2 guests are here; 3 teams need at least 3."), ({"teams": 5}, "teams is a whole number from 2 to 4."),
                                           ({"seed": -1}, "seed is a whole number")])
        self.json("POST", "/api/party", {"guests": [{"name": n, "here": True} for n in here], "consoles": [], "gamemaster": "10.10.1.42"})
        self.refused("/api/party/raffle", [({"teams": 2}, "10 people need at least 3 consoles (up to 4 on one, split-screen), and the party has 1.")])
        self.json("POST", "/api/party", {"guests": [{"name": n} for n in here]})
        self.refused("/api/party/raffle", [({}, "No one is here yet: tick the guests who are here.")])
        # Seats swapped by hand.
        self.json("POST", "/api/party", {"guests": [{"name": n, "here": n != "Jon"} for n in here], "consoles": ["10.10.1.41", "10.10.1.43"]})
        self.json("POST", "/api/party/seats", {"seats": {"10.10.1.43": ["Ann", "bob"], "10.10.1.42": ["Cid"]}})
        self.assertEqual(list(self.json("GET", "/api/state")["party"]["drawn"]["seats"].items()),
                         [("10.10.1.42", ["Cid"]), ("10.10.1.41", []), ("10.10.1.43", ["Ann", "Bob"])])
        self.refused("/api/party/seats", [
            ({}, "Send seats"),
            ({"seats": {"10.10.1.42": ["Ann", "Bob", "Cid", "Dee", "Eve"]}}, "Up to 4 play on one console (split-screen)."),
            ({"seats": {"10.10.1.42": ["Jon"]}}, "Jon isn't a guest who is here."),
            ({"seats": {"10.10.1.42": ["Zed"]}}, "Zed isn't a guest who is here."),
            ({"seats": {"10.10.1.42": ["Ann"], "10.10.1.41": ["ANN"]}}, "Ann has two seats."),
            ({"seats": {"10.10.1.99": ["Ann"]}}, "10.10.1.99 isn't one of the party's consoles."),
            ({"seats": {"10.10.1.42": "Ann"}}, "a list of guests' names"),
            # A console named twice can't seat more than four.
            (b'{"seats": {"10.10.1.42": ["Ann", "Bob", "Cid", "Dee"], "10.10.1.42": ["Eve", "Fay", "Gus", "Hal"]}}', "10.10.1.42 is listed twice."),
        ])
        self.assertEqual(list(self.json("GET", "/api/state")["party"]["drawn"]["seats"].items()),
                         [("10.10.1.42", ["Cid"]), ("10.10.1.41", []), ("10.10.1.43", ["Ann", "Bob"])])
        # A party that ends leaves the next one its guests, but no one here, and no rounds, teams or seats.
        self.json("POST", "/api/party/raffle", {"teams": 2})
        self.json("POST", "/api/party/keep", {})
        self.json("POST", "/api/party", {"active": False})
        self.json("POST", "/api/party", {"active": True})
        party = self.json("GET", "/api/state")["party"]
        self.assertEqual((party["round"], party["drawn"], party["last"]), (0, None, []))
        self.assertEqual([{"name": g["name"], "here": g["here"]} for g in party["guests"]], [{"name": n, "here": False} for n in here])
        # Each guest's numbers stay: two parties started, nothing read or played.
        self.assertEqual({g["name"]: (g["stats"]["parties"], g["stats"]["gamerscore"], g["stats"]["playSeconds"]) for g in party["guests"]}, {n: (0 if n == "Jon" else 1, -1, 0) for n in here})
        self.assertEqual((party["teams"], party["consoles"]), (2, ["10.10.1.42", "10.10.1.41", "10.10.1.43"]))
        self.restart(*self.PARTY_CONSOLES)
        self.assertEqual(self.json("GET", "/api/state")["party"], party)

    def test_keeps_the_party_s_pictures(self):
        self.start()
        png = b"\x89PNG\r\n\x1a\n" + b"P" * 100
        jpeg = b"\xff\xd8\xff\xe0" + b"J" * 100
        webp = b"RIFF\x64\x00\x00\x00WEBPVP8 " + b"W" * 100
        folder = os.path.join(self.tmp.name, "party")
        send = lambda kind, kind_type, data, **more: self.call("POST", "/api/party/asset", dict(
            {"kind": kind, "type": kind_type, "data": base64.b64encode(data).decode()}, **more))
        picture = lambda query: self.call("GET", "/api/party/asset?" + query)
        self.assertEqual(picture("kind=logo")[0], 404)
        status, _, data = send("logo", "image/png", png)
        self.assertEqual((status, json.loads(data)), (200, {"ok": True, "index": 0}))
        status, response, data = picture("kind=logo")
        self.assertEqual((status, response.getheader("Content-Type"), response.getheader("X-Content-Type-Options"), data), (200, "image/png", "nosniff", png))
        self.assertEqual(self.call("GET", "/api/party/asset?kind=logo", page=False)[0], 403)  # The page fetches it.
        # A new logo takes the old one's place; a name sent is no file name.
        self.assertEqual(send("logo", "image/jpeg", jpeg, name="../../netdisc.ini")[0], 200)
        status, response, data = picture("kind=logo")
        self.assertEqual((response.getheader("Content-Type"), data), ("image/jpeg", jpeg))
        self.assertEqual(os.listdir(folder), ["logo.jpg"])
        # Background photos go after the others, and come back by their place.
        for index, (kind_type, data) in enumerate([("image/webp", webp), ("image/png", png), ("image/jpeg", jpeg)]):
            self.assertEqual(json.loads(send("background", kind_type, data)[2]), {"ok": True, "index": index})
        for n, (kind_type, data) in enumerate([("image/webp", webp), ("image/png", png), ("image/jpeg", jpeg)]):
            status, response, got = picture("kind=background&n=%d" % n)
            self.assertEqual((status, response.getheader("Content-Type"), got), (200, kind_type, data))
        for query in ("kind=background&n=3", "kind=background&n=-1", "kind=background&n=0x1", "kind=background", "kind=../netdisc.ini",
                      "kind=background&n=../../netdisc.ini", "kind=logo.jpg"):
            self.assertEqual(picture(query)[0], 404, query)
        self.assertEqual((self.json("GET", "/api/state")["party"]["theme"]["logo"], self.json("GET", "/api/state")["party"]["theme"]["backgrounds"]), (True, 3))
        # The pictures' version changes with any of them, so another device
        # (the big screen) loads them again, though there are as many.
        version = lambda: self.json("GET", "/api/state")["party"]["theme"]["pictures"]
        versions = [version()]
        self.assertTrue(re.match(r"[0-9a-f]{8}$", versions[0]), versions)
        self.json("POST", "/api/party/asset/remove", {"kind": "background", "index": 1})
        self.assertEqual(picture("kind=background&n=1")[2], jpeg)
        self.assertEqual(json.loads(send("background", "image/png", png)[2]), {"ok": True, "index": 2})
        versions.append(version())
        self.assertEqual(send("logo", "image/jpeg", jpeg + b"J")[0], 200)  # logo.jpg again.
        versions.append(version())
        self.assertEqual(len(set(versions)), 3, versions)
        self.assertEqual(version(), versions[-1])
        self.assertEqual(sorted(os.listdir(folder)), ["background1.webp", "background3.jpg", "background4.png", "logo.jpg"])
        # Anything else is refused.
        self.refused("/api/party/asset", [
            ({"kind": "logo", "type": "image/png", "data": base64.b64encode(jpeg).decode()}, "That file isn't a PNG picture."),
            ({"kind": "logo", "type": "image/jpeg", "data": base64.b64encode(b"<svg xmlns='http://www.w3.org/2000/svg'/>").decode()}, "isn't a JPEG picture."),
            ({"kind": "background", "type": "image/webp", "data": base64.b64encode(b"RIFF\x00\x00\x00\x00WAVEfmt ").decode()}, "isn't a WebP picture."),
            ({"kind": "logo", "type": "image/svg+xml", "data": base64.b64encode(b"<svg/>").decode()}, "Send a PNG, JPEG or WebP picture."),
            ({"kind": "logo", "type": "text/html", "data": base64.b64encode(png).decode()}, "Send a PNG, JPEG or WebP picture."),
            ({"kind": "logo", "type": "image/png", "data": base64.b64encode(png + bytes((3 << 20) - len(png) + 1)).decode()}, "That picture is 3.0 MB; 3 MB at most."),
            ({"kind": "../logo", "type": "image/png", "data": base64.b64encode(png).decode()}, "kind is logo or background."),
            ({"kind": "logo", "type": "image/png", "data": "not base64!"}, "didn't arrive whole"),
            ({"kind": "logo", "type": "image/png"}, "didn't arrive whole"),
        ])
        self.assertEqual(sorted(os.listdir(folder)), ["background1.webp", "background3.jpg", "background4.png", "logo.jpg"])
        self.assertEqual(send("logo", "image/png", png + bytes((3 << 20) - len(png)))[0], 200)  # 3 MB is fine.
        self.assertEqual(len(picture("kind=logo")[2]), 3 << 20)
        # Only the page has one that big read.
        with socket.create_connection(("127.0.0.1", self.web), timeout=10) as conn:
            conn.sendall(b"POST /api/party/asset HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nContent-Type: application/json\r\nContent-Length: 1000000\r\n\r\n" % self.web)
            self.assertTrue(conn.recv(100).startswith(b"HTTP/1.1 413 "))
        # Eight background photos at most.
        for _ in range(5):
            self.assertEqual(send("background", "image/png", png)[0], 200)
        self.refused("/api/party/asset", [({"kind": "background", "type": "image/png", "data": base64.b64encode(png).decode()}, "8 background photos at most")])
        self.refused("/api/party/asset/remove", [({"kind": "background", "index": 8}, "index is a whole number from 0 to 7."), ({"kind": "logos"}, "kind is logo or background.")])
        self.json("POST", "/api/party/asset/remove", {"kind": "background", "index": 7})
        self.refused("/api/party/asset/remove", [({"kind": "background", "index": 7}, "There's no background photo 8.")])
        for name in os.listdir(folder):
            self.assertTrue(re.match(r"(logo|background\d+)\.(png|jpg|webp)$", name), name)
        # Every background photo, then the logo.
        self.json("POST", "/api/party/asset/remove", {"kind": "background"})
        self.json("POST", "/api/party/asset/remove", {"kind": "logo"})
        self.assertEqual(os.listdir(folder), [])
        theme = self.json("GET", "/api/state")["party"]["theme"]
        self.assertEqual((theme["logo"], theme["backgrounds"], theme["pictures"]), (False, 0, ""))

    def test_the_system_link_list_is_sound(self):
        # The built-in table is the data's, as tools/make_systemlink.py makes it.
        subprocess.run([sys.executable, os.path.join(ROOT, "tools", "make_systemlink.py"), "--check"], check=True)
        with open(os.path.join(ROOT, "data", "systemlink.json"), encoding="utf-8") as f:
            source = json.load(f)
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        self.add_iso("Skate 3.iso", 0x5C087C2C, 0x454108E6)
        self.start()
        self.state_when(lambda s: s["library"]["scanned"] and s["library"]["games"] == 2)  # The games are read just after the start.
        listed = self.json("GET", "/api/systemlink")
        ids = [g["titleId"] for g in listed["games"]]
        self.assertGreater(len(ids), 300)
        self.assertEqual(len(ids), len(set(ids)), "a Title ID is in the list twice")
        self.assertEqual(ids, sorted(ids, key=lambda t: int(t, 16)))
        self.assertEqual(set(ids), {g["titleId"].strip().upper() for g in source["games"] if (g.get("titleId") or "").strip()})
        for game in listed["games"]:
            self.assertTrue(re.match(r"[0-9A-F]{8}$", game["titleId"]), game)
            self.assertTrue(game["name"].strip(), game)
            self.assertIn(game["platform"], ("360", "xbox"), game)
            for key in ("total", "perConsole"):
                self.assertTrue(game[key] is None or (isinstance(game[key], int) and game[key] >= 1), game)
            if game["total"] and game["perConsole"]:
                self.assertGreaterEqual(game["total"], game["perConsole"], game)
        for words in ("Wikipedia", "CC BY-SA 4.0", "creativecommons.org/licenses/by-sa/4.0"):
            self.assertIn(words, listed["attribution"])
        halo = [g for g in listed["games"] if g["titleId"] == "4D5307E6"][0]
        self.assertEqual((halo["name"], halo["total"], halo["perConsole"], halo["inLibrary"], halo["game"], halo["consoles"]), ("Halo 3", 16, 4, True, "Halo 3", []))
        self.assertTrue(all(not g["inLibrary"] and g["game"] is None for g in listed["games"] if g["titleId"] != "4D5307E6"))
        games = {g["name"]: g for g in self.json("GET", "/api/games")["games"]}
        self.assertEqual([games["Halo 3"][k] for k in ("systemLink", "perConsole", "total")], [True, 4, 16])
        self.assertEqual([games["Skate 3"][k] for k in ("systemLink", "perConsole", "total")], [False, None, None])

    def test_installs_a_game_s_content_on_the_console(self):
        # GTA V extracted, its install disc in Disc 2; Forza 4's car packs on an ISO.
        packages = "content/0000000000000000/545408A7/00000002/"
        first, second = os.urandom(3 << 20) + b"END", b"PIRS" + bytes(10)
        for folder, media_id, disc, content in (("GTA V", 0x79E465C5, 2, {}),
                                                 ("GTA V/Disc 2", 0x0C48794E, 1, {packages + "545408A700000000": first, packages + "545408A700000001": second})):
            os.makedirs(os.path.join(self.isos, folder))
            with open(os.path.join(self.isos, folder, "default.xex"), "wb") as f:
                f.write(xex(execution_info(media_id, 0x545408A7, disc, 2)))
            for path, data in content.items():
                os.makedirs(os.path.dirname(os.path.join(self.isos, folder, path)), exist_ok=True)
                with open(os.path.join(self.isos, folder, path), "wb") as f:
                    f.write(data)
        car_pack = b"PIRS" + os.urandom(5000)
        self.add_iso("Forza 4.iso", 0x11112222, 0x4D530910)
        write_tree_iso(os.path.join(self.isos, "Forza 4 - Disc 2.iso"), {
            "default.xex": xex(execution_info(0x33334444, 0x4D530910, 2, 2)),
            "content": {"0000000000000000": {"4D530910": {"00000002": {"4D53091000000001": car_pack}}}}})
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        xbox = os.path.join(self.tmp.name, "xbox")
        os.makedirs(os.path.join(xbox, "Hdd1"))
        content = os.path.join(xbox, "Hdd1", "Content", "0000000000000000")
        ftp = aurora_ftp.Server(xbox, 0, host="127.0.0.2")  # A port free on 127.0.0.2 itself.
        ftp_port = ftp.server_address[1]
        threading.Thread(target=ftp.serve_forever, daemon=True).start()
        try:
            self.start("--xbox", "127.0.0.2", "--ftp-port", str(ftp_port))
            self.state_when(lambda s: s["library"]["games"] == 5)
            games = {g["name"]: g for g in self.json("GET", "/api/games")["games"]}
            self.assertEqual((games["GTA V - Disc 2"]["packages"], games["GTA V - Disc 2"]["contentSize"]), (2, len(first) + len(second)))
            self.assertEqual((games["GTA V"]["packages"], games["GTA V"]["installDisc"]), (0, "GTA V - Disc 2"))

            def install(game, address="127.0.0.2"):
                self.json("POST", "/api/install", {"game": game, "address": address})
                state = self.state_when(lambda s: s["installs"] and s["installs"][-1]["state"] not in ("queued", "copying"), 30)
                return state["installs"][-1]

            def on_xbox(path):
                with open(os.path.join(content, *path.split("/")), "rb") as f:
                    return f.read()
            # From the play disc: its install disc's packages, whole, where the launcher looks.
            job = install("GTA V")
            self.assertEqual([job[k] for k in ("state", "game", "disc", "files", "skipped", "address")], ["done", "GTA V", "GTA V - Disc 2", 2, 0, "127.0.0.2"])
            self.assertEqual(job["done"], job["total"])
            self.assertEqual(on_xbox("545408A7/00000002/545408A700000000"), first)
            self.assertEqual(on_xbox("545408A7/00000002/545408A700000001"), second)
            # Again: nothing to copy. A package cut short is copied again.
            self.assertEqual(install("GTA V")["skipped"], 2)
            with open(os.path.join(content, "545408A7", "00000002", "545408A700000000"), "r+b") as f:
                f.truncate(1000)
            ftp.stored.clear()
            job = install("GTA V - Disc 2")
            self.assertEqual((job["state"], job["skipped"]), ("done", 1))
            self.assertEqual(on_xbox("545408A7/00000002/545408A700000000"), first)
            self.assertEqual(len(ftp.stored), 1)
            # From inside an ISO.
            self.assertEqual(install("Forza 4")["state"], "done")
            self.assertEqual(on_xbox("4D530910/00000002/4D53091000000001"), car_pack)
            self.assertIn("Forza 4's content is installed", json.dumps(self.json("GET", "/api/state")["events"]))
            # Only games with content, only to known consoles.
            self.assertEqual(self.call("POST", "/api/install", {"game": "Halo 3", "address": "127.0.0.2"})[0], 400)
            self.assertEqual(self.call("POST", "/api/install", {"game": "GTA V", "address": "10.9.8.7"})[0], 400)
            self.assertEqual(self.call("POST", "/api/install/stop", {"address": "127.0.0.2"})[0], 400)
        finally:
            ftp.shutdown()
            ftp.server_close()

    def test_copies_a_whole_game_to_the_console_as_games_on_demand(self):
        # Big enough for two Data files (0xA1C4 blocks, 166 MB, each): a sparse gap, then a file.
        big = os.urandom(1 << 20)
        iso = os.path.join(self.isos, "Halo 3 (USA).iso")
        write_tree_iso(iso, {
            "default.xex": xex(execution_info(0x1A2B3C4D, 0x4D5307E6, 1, 1)),
            "media": {"start.map": big, "gap.bin": 0xA1C4 * 4096, "end.map": b"tail" * 1000}})
        os.makedirs(os.path.join(self.isos, "Forza 4"))
        with open(os.path.join(self.isos, "Forza 4", "default.xex"), "wb") as f:
            f.write(xex(execution_info(0x11112222, 0x4D530910, 1, 1)))
        with open(os.path.join(self.isos, "Forza 4", "cars.bin"), "wb") as f:
            f.write(b"car" * 3000)
        xbox = os.path.join(self.tmp.name, "xbox")
        os.makedirs(os.path.join(xbox, "Hdd1"))
        content = os.path.join(xbox, "Hdd1", "Content", "0000000000000000")
        ftp = aurora_ftp.Server(xbox, 0, host="127.0.0.2")  # A port free on 127.0.0.2 itself.
        ftp_port = ftp.server_address[1]
        threading.Thread(target=ftp.serve_forever, daemon=True).start()
        try:
            self.start("--xbox", "127.0.0.2", "--ftp-port", str(ftp_port))
            self.state_when(lambda s: s["library"]["games"] >= 2)

            def copy(game, address="127.0.0.2"):
                self.json("POST", "/api/copy", {"game": game, "address": address})
                state = self.state_when(lambda s: s["installs"] and s["installs"][-1]["state"] not in ("queued", "copying"), 60)
                return state["installs"][-1]

            job = copy("Halo 3 (USA)")
            self.assertEqual([job[k] for k in ("state", "kind", "folder", "files", "skipped")],
                             ["done", "game", "/Hdd1/Content/0000000000000000/4D5307E6/00007000", 3, 0], job.get("error"))
            self.assertEqual(job["done"], job["total"])
            folder = os.path.join(content, "4D5307E6", "00007000")
            with open(iso, "rb") as f:
                image = f.read()
            self.check_god(os.path.join(folder, "1A2B3C4D"), image, 0x2080000, 0x4D5307E6, 0x1A2B3C4D, "Halo 3")  # Without its release tags, as the Xbox home shows it.
            self.assertEqual(ftp.scans[-1], "/Hdd1/Content/0000000000000000")
            self.assertIn("Halo 3 (USA) is on the hard drive", json.dumps(self.json("GET", "/api/state")["events"]))
            # Again: it's there. Without its header, a Data file cut short is sent again and the rest kept.
            self.assertEqual(copy("Halo 3 (USA)")["skipped"], 3)
            os.remove(os.path.join(folder, "1A2B3C4D"))
            with open(os.path.join(folder, "1A2B3C4D.data", "Data0001"), "r+b") as f:
                f.truncate(1000)
            ftp.stored.clear()
            self.assertEqual(copy("Halo 3 (USA)")["skipped"], 1)
            self.assertEqual(len(ftp.stored), 2)
            self.check_god(os.path.join(folder, "1A2B3C4D"), image, 0x2080000, 0x4D5307E6, 0x1A2B3C4D, "Halo 3")
            # An extracted folder, as the disc the server serves of it.
            self.assertEqual(copy("Forza 4")["state"], "done")
            self.assertTrue(os.path.isfile(os.path.join(content, "4D530910", "00007000", "11112222")))
            # Only Xbox 360 games, only to known consoles.
            self.assertEqual(self.call("POST", "/api/copy", {"game": "Forza 4", "address": "10.9.8.7"})[0], 400)
        finally:
            ftp.shutdown()
            ftp.server_close()

    def test_copies_a_bonus_disc_s_packages_where_their_headers_say(self):
        # Skyrim's add-on disc keeps its DLC under a placeholder Title ID; the
        # package's own header says whose it is. Avatar items stay behind.
        def package(title_id, content_type, size=0x1000):
            data = bytearray(b"LIVE" + bytes(size - 4))
            struct.pack_into(">I", data, 0x344, content_type)
            struct.pack_into(">I", data, 0x360, title_id)
            data[-8:] = os.urandom(8)
            return bytes(data)
        dlc, maps, avatar = package(0x425307D6, 0x2), package(0x4D53085B, 0x2, 0x2000), package(0x425307D6, 0x9000)
        write_tree_iso(os.path.join(self.isos, "Skyrim - Add-ons.iso"), {
            "default.xex": xex(execution_info(0x55667788, 0x425307D6, 2, 2)),
            "content": {"0000000000000000": {
                "FFED2000": {"FFFFFFFF": {"Dawnguard": dlc, "Avatar": avatar}},
                "4D53085B": {"00000002": {"Reach maps": maps}}}}})
        xbox = os.path.join(self.tmp.name, "xbox")
        os.makedirs(os.path.join(xbox, "Hdd1"))
        content = os.path.join(xbox, "Hdd1", "Content", "0000000000000000")
        ftp = aurora_ftp.Server(xbox, 0, host="127.0.0.2")  # A port free on 127.0.0.2 itself.
        ftp_port = ftp.server_address[1]
        threading.Thread(target=ftp.serve_forever, daemon=True).start()
        try:
            self.start("--xbox", "127.0.0.2", "--ftp-port", str(ftp_port))
            self.state_when(lambda s: s["library"]["games"] == 1)
            game = self.json("GET", "/api/games")["games"][0]
            self.assertEqual(game["packages"], 2)
            self.json("POST", "/api/copy", {"game": "Skyrim - Add-ons", "address": "127.0.0.2"})
            jobs = self.state_when(lambda s: len(s["installs"]) == 2 and all(j["state"] not in ("queued", "copying") for j in s["installs"]), 60)["installs"]
            self.assertEqual([(j["kind"], j["state"]) for j in jobs], [("game", "done"), ("content", "done")], [j["error"] for j in jobs])
            with open(os.path.join(content, "425307D6", "00000002", "Dawnguard"), "rb") as f:
                self.assertEqual(f.read(), dlc)
            with open(os.path.join(content, "4D53085B", "00000002", "Reach maps"), "rb") as f:
                self.assertEqual(f.read(), maps)
            self.assertFalse(os.path.exists(os.path.join(content, "425307D6", "00009000")))
            self.assertFalse(os.path.exists(os.path.join(content, "FFED2000")))
        finally:
            ftp.shutdown()
            ftp.server_close()

    def test_copies_an_original_xbox_game_as_its_files(self):
        folder = os.path.join(self.isos, "Halo - Combat Evolved")
        os.makedirs(os.path.join(folder, "maps"))
        xbe = bytearray(0x2000)
        xbe[:4] = b"XBEH"
        struct.pack_into("<I", xbe, 0x104, 0x10000)          # Base address.
        struct.pack_into("<I", xbe, 0x118, 0x10000 + 0x1000)  # Certificate.
        struct.pack_into("<I", xbe, 0x1000 + 8, 0x4D530004)   # Its Title ID.
        files = {"default.xbe": bytes(xbe), os.path.join("maps", "a10.map"): os.urandom(70000), os.path.join("maps", "ui.map"): b"ui" * 100}
        for name, data in files.items():
            with open(os.path.join(folder, name), "wb") as f:
                f.write(data)
        xbox = os.path.join(self.tmp.name, "xbox")
        os.makedirs(os.path.join(xbox, "Hdd1"))
        ftp = aurora_ftp.Server(xbox, 0, host="127.0.0.2")  # A port free on 127.0.0.2 itself.
        ftp_port = ftp.server_address[1]
        threading.Thread(target=ftp.serve_forever, daemon=True).start()
        try:
            self.start("--xbox", "127.0.0.2", "--ftp-port", str(ftp_port))
            self.state_when(lambda s: s["library"]["games"] == 1)
            self.json("POST", "/api/copy", {"game": "Halo - Combat Evolved", "address": "127.0.0.2"})
            job = self.state_when(lambda s: s["installs"] and s["installs"][-1]["state"] not in ("queued", "copying"), 60)["installs"][-1]
            self.assertEqual((job["kind"], job["state"], job["folder"]), ("files", "done", "/Hdd1/Games/Halo - Combat Evolved"), job["error"])
            for name, data in files.items():
                with open(os.path.join(xbox, "Hdd1", "Games", "Halo - Combat Evolved", name), "rb") as f:
                    self.assertEqual(f.read(), data, name)
            self.assertEqual(ftp.scans[-1], "/Hdd1/Games")
            # The dashboard shows it installed there right away.
            game = next(g for g in self.json("GET", "/api/games")["games"] if g["name"] == "Halo - Combat Evolved")
            self.assertEqual(game["installedOn"], ["127.0.0.2"])
            # Again: every file is there.
            self.json("POST", "/api/copy", {"game": "Halo - Combat Evolved", "address": "127.0.0.2"})
            job = self.state_when(lambda s: s["installs"][-1]["id"] != job["id"] and s["installs"][-1]["state"] not in ("queued", "copying"), 60)["installs"][-1]
            self.assertEqual(job["skipped"], 3)
        finally:
            ftp.shutdown()
            ftp.server_close()

    def test_removes_a_game_installed_on_the_console_and_nothing_else(self):
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        self.add_iso("Forza 4.iso", 0x11112222, 0x4D530910)
        xbox = os.path.join(self.tmp.name, "xbox")
        content = os.path.join(xbox, "Hdd1", "Content", "0000000000000000")
        package = os.path.join(content, "4D5307E6", "00007000", "1A2B3C4D")
        os.makedirs(package + ".data")
        with open(package, "wb") as f:
            f.write(god_header(0x1A2B3C4D, 0x4D5307E6))
        with open(os.path.join(package + ".data", "Data0000"), "wb") as f:
            f.write(bytes(5000))
        # Its title update, DLC and save stay.
        for keep in ("000B0000/tu00000002_00000000", "00000002/Map pack", "../../E0000123456789AB/4D5307E6/00000001/save"):
            path = os.path.normpath(os.path.join(content, "4D5307E6", keep))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(b"keep")
        forza = os.path.join(xbox, "Hdd1", "Games", "Forza 4")
        os.makedirs(os.path.join(forza, "media"))
        with open(os.path.join(forza, "default.xex"), "wb") as f:
            f.write(xex(execution_info(0x11112222, 0x4D530910)))
        with open(os.path.join(forza, "media", "cars.bin"), "wb") as f:
            f.write(b"cars")
        ftp = aurora_ftp.Server(xbox, 0, host="127.0.0.2")  # A port free on 127.0.0.2 itself.
        ftp_port = ftp.server_address[1]
        threading.Thread(target=ftp.serve_forever, daemon=True).start()
        launcher, boot = os.path.join(self.tmp.name, "launcher.xex"), os.path.join(self.tmp.name, "boot.xex")
        for path in (launcher, boot):
            with open(path, "wb") as f:
                f.write(xex(execution_info(0, 0)) + b"LAUNCHER CODE")
        try:
            self.start("--xbox", "127.0.0.2", "--ftp-port", str(ftp_port), "--launcher", launcher, "--boot", boot, "--push")
            self.state_when(lambda s: s["library"]["games"] == 2)
            for _ in range(100):
                games = {g["name"]: g for g in self.json("GET", "/api/games")["games"]}
                if games["Halo 3"]["installedOn"] and games["Forza 4"]["installedOn"]:
                    break
                time.sleep(0.2)
            self.assertEqual(games["Halo 3"]["installedOn"], ["127.0.0.2"], self.json("GET", "/api/log?after=0"))

            def remove(game):
                self.json("POST", "/api/console/remove", {"game": game, "address": "127.0.0.2"})
                return self.state_when(lambda s: s["installs"] and s["installs"][-1]["state"] not in ("queued", "copying"), 30)["installs"][-1]
            job = remove("Halo 3")
            self.assertEqual((job["kind"], job["state"]), ("remove", "done"), job["error"])
            self.assertEqual(sorted(os.listdir(os.path.join(content, "4D5307E6"))), ["00000002", "000B0000"])
            self.assertTrue(os.path.isfile(os.path.join(content, "4D5307E6", "000B0000", "tu00000002_00000000")))
            self.assertTrue(os.path.isfile(os.path.join(xbox, "Hdd1", "Content", "E0000123456789AB", "4D5307E6", "00000001", "save")))
            self.assertEqual(remove("Forza 4")["state"], "done")
            self.assertEqual(os.listdir(os.path.join(xbox, "Hdd1", "Games")), [])
            # Not installed (any more): says so.
            status, _, data = self.call("POST", "/api/console/remove", {"game": "Halo 3", "address": "127.0.0.2"})
            self.assertEqual(status, 400)
            self.assertIn("isn't installed", json.loads(data)["error"])
        finally:
            ftp.shutdown()
            ftp.server_close()

    def test_puts_title_updates_on_consoles_from_xboxunity(self):
        halo = self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        self.add_iso("Forza 4.iso", 0x11112222, 0x4D530910)
        newest, older = tu_package(0x4D5307E6, 0x1A2B3C4D, "Title Update 2"), tu_package(0x4D5307E6, 0x1A2B3C4D, "Title Update 1", 0x2000)
        cache_tu = os.urandom(5000)
        kb = lambda data: (len(data) + 1023) // 1024  # XboxUnity lists sizes in KB.
        unity = http.server.ThreadingHTTPServer(("127.0.0.1", 0), FakeUnityHandler)
        unity.asked = []
        unity.titles = {
            "4D5307E6": {"Type": 1, "MediaIDS": [
                {"MediaID": "1A2B3C4D", "Updates": [
                    {"TitleUpdateID": "110", "Version": "1", "Name": "Title Update 1", "Size": str(kb(older)), "hash": content_id(older)},
                    {"TitleUpdateID": "111", "Version": "2", "Name": "Title Update 2", "Size": str(kb(newest)), "hash": content_id(newest).upper()}]},
                {"MediaID": "99999999", "Updates": [{"TitleUpdateID": "120", "Version": "3", "Name": "Title Update 3"}]}]},
            # The other shape: each update says its disc.
            "4D530910": {"Type": 2, "Updates": [{"TitleUpdateID": "130", "Version": "4", "MediaID": "11112222", "Size": str(kb(cache_tu)), "hash": hashlib.sha1(cache_tu).hexdigest()}]}}
        unity.files = {"111": ("tu00000002_00000000", newest), "110": ("tu00000001_00000000", older), "130": ("TU_16L61V6_0000014000000.00000000000O9", cache_tu)}
        threading.Thread(target=unity.serve_forever, daemon=True).start()
        xbox = os.path.join(self.tmp.name, "xbox")
        updates = os.path.join(xbox, "Hdd1", "Content", "0000000000000000", "4D5307E6", "000B0000")
        os.makedirs(updates)
        os.makedirs(os.path.join(xbox, "Hdd1", "Cache"))
        # The console has Title Update 1, put there by Aurora.
        with open(os.path.join(updates, "tu00000001_00000000"), "wb") as f:
            f.write(older)
        ftp = aurora_ftp.Server(xbox, 0, host="127.0.0.2")  # A port free on 127.0.0.2 itself.
        ftp_port = ftp.server_address[1]
        threading.Thread(target=ftp.serve_forever, daemon=True).start()
        try:
            self.start("--xbox", "127.0.0.2", "--ftp-port", str(ftp_port), "--xboxunity", "http://127.0.0.1:%d/Resources/Lib" % unity.server_port)
            self.state_when(lambda s: s["library"]["games"] == 2 and s["consoles"])
            self.state_when(lambda s: any(c.get("tiles") for c in s["consoles"]) or True)

            def wait_queue():
                return self.state_when(lambda s: s["installs"] and all(j["state"] not in ("queued", "copying") for j in s["installs"]), 60)["installs"]

            found = self.json("GET", "/api/titleupdates?game=" + urllib.parse.quote("Halo 3"))
            self.assertEqual([(u["id"], u["name"], u["onPc"]) for u in found["offers"]], [("111", "Title Update 2", False), ("110", "Title Update 1", False)])
            self.assertEqual((found["titleId"], found["mediaId"], found["otherDiscs"]), ("4D5307E6", "1A2B3C4D", 1))
            self.assertEqual(found["consoles"][0]["checked"], 0)
            # A check of the console finds Title Update 1 by its size.
            self.json("POST", "/api/titleupdates/check", {"address": "127.0.0.2"})
            self.assertEqual(wait_queue()[-1]["state"], "done")
            found = self.json("GET", "/api/titleupdates?game=Halo%203")
            console = found["consoles"][0]
            self.assertEqual((console["has"], console["hasName"], console["other"]), ("110", "Title Update 1", False))
            self.assertEqual([f["title"] for f in console["files"]], ["Title Update 1"])
            # The newest: downloaded here, put where Aurora puts it, the older one moved out of the way.
            self.json("POST", "/api/titleupdates/install", {"game": "Halo 3", "address": "127.0.0.2"})
            job = wait_queue()[-1]
            self.assertEqual([job[k] for k in ("kind", "state", "label", "game")], ["update", "done", "Title update 2", "Halo 3"], job["error"])
            self.assertIn("Moved the older update", job["note"])
            self.assertEqual(os.listdir(updates), ["tu00000002_00000000"])
            with open(os.path.join(updates, "tu00000002_00000000"), "rb") as f:
                self.assertEqual(f.read(), newest)
            aside = os.path.join(xbox, "Hdd1", "Apps", "NetDisc360", "Title updates", "4D5307E6")
            with open(os.path.join(aside, content_id(older), "tu00000001_00000000"), "rb") as f:
                self.assertEqual(f.read(), older)
            kept = os.path.join(os.path.dirname(self.config), "TitleUpdates", "4D5307E6", "111", "tu00000002_00000000")
            with open(kept, "rb") as f:
                self.assertEqual(f.read(), newest)
            found = self.json("GET", "/api/titleupdates?game=Halo%203")
            self.assertEqual(found["consoles"][0]["has"], "111")
            self.assertTrue(found["offers"][0]["onPc"])
            self.assertIn("Title update 2 for Halo 3 is on the console", json.dumps(self.json("GET", "/api/state")["events"]))
            # Again: there already, and not downloaded again.
            asked = len(unity.asked)
            self.json("POST", "/api/titleupdates/install", {"game": "Halo 3", "address": "127.0.0.2", "id": "111"})
            self.assertEqual(wait_queue()[-1]["skipped"], 1)
            self.assertFalse([a for a in unity.asked[asked:] if "TitleUpdate.php" in a])
            # Like Aurora's manager: every update the console has, on or off,
            # Aurora's own store too (only read), and switched by moving files.
            device = "3930383034324250323438335351314B54354158"  # Aurora's ID for the hard drive.
            store = os.path.join(xbox, "Hdd1", "Apps", "Aurora", "Data", "TitleUpdates", device, "4D5307E6")
            for package in (older, newest):
                os.makedirs(os.path.join(store, content_id(package).upper()))
                with open(os.path.join(store, content_id(package).upper(), "tu0000000%d_00000000" % (1 if package is older else 2)), "wb") as f:
                    f.write(package)
            stored = sorted(os.listdir(store))

            def switched():
                self.json("POST", "/api/titleupdates/check", {"address": "127.0.0.2"})
                wait_queue()
                console = self.json("GET", "/api/titleupdates?game=Halo%203")["consoles"][0]
                return console, sorted((f["where"], f["offer"]) for f in console["files"])
            console, files = switched()
            self.assertEqual(files, [("aside", "110"), ("aurora", "110"), ("aurora", "111"), ("live", "111")])
            self.assertEqual(console["has"], "111")
            # Off: the game starts without one.
            self.json("POST", "/api/titleupdates/off", {"game": "Halo 3", "address": "127.0.0.2"})
            job = wait_queue()[-1]
            self.assertEqual((job["kind"], job["state"]), ("tuoff", "done"), job["error"])
            self.assertEqual(os.listdir(updates), [])
            console, files = switched()
            self.assertEqual(files, [("aside", "110"), ("aside", "111"), ("aurora", "110"), ("aurora", "111")])
            self.assertEqual(console["has"], "")
            # The one moved aside, moved back.
            path = next(f["path"] for f in console["files"] if f["where"] == "aside" and f["offer"] == "110")
            self.json("POST", "/api/titleupdates/use", {"game": "Halo 3", "address": "127.0.0.2", "path": path})
            self.assertEqual(wait_queue()[-1]["state"], "done", wait_queue()[-1]["error"])
            self.assertEqual(os.listdir(updates), ["tu00000001_00000000"])
            console, files = switched()
            self.assertEqual(files, [("aside", "111"), ("aurora", "110"), ("aurora", "111"), ("live", "110")])
            # The one in Aurora's store, copied; Aurora's stays as it is.
            path = next(f["path"] for f in console["files"] if f["where"] == "aurora" and f["offer"] == "111")
            self.json("POST", "/api/titleupdates/use", {"game": "Halo 3", "address": "127.0.0.2", "path": path})
            job = wait_queue()[-1]
            # Labelled with XboxUnity's number when it lists the update (a header's can be one lower).
            self.assertEqual((job["state"], job["label"]), ("done", "Title update 2"), job["error"])
            self.assertEqual(os.listdir(updates), ["tu00000002_00000000"])
            with open(os.path.join(updates, "tu00000002_00000000"), "rb") as f:
                self.assertEqual(f.read(), newest)
            self.assertEqual(sorted(os.listdir(store)), stored)
            console, files = switched()
            self.assertEqual(files, [("aside", "110"), ("aside", "111"), ("aurora", "110"), ("aurora", "111"), ("live", "111")])
            self.assertEqual(console["has"], "111")
            # Aurora's own list says the same: Aurora puts the update it has on
            # in place each time it starts the game and deletes the others. A
            # script Aurora runs as it starts does that. Aurora reads the list
            # before its scripts run, so it uses the change from the start
            # after; it isn't restarted here (no Aurora web server), so the job
            # says that's still to come.
            self.assertIn("restarts it twice so it takes this; don't start Halo 3 before that", job["note"])
            script = os.path.join(xbox, "Hdd1", "Apps", "Aurora", "User", "Scripts", "Content", "Filters", "NetDiscTitleUpdates.lua")
            helper = os.path.join(xbox, "Hdd1", "Apps", "NetDisc360")
            if lupa:
                content, settings = os.path.join(self.tmp.name, "content.db"), os.path.join(self.tmp.name, "settings.db")
                for name, table in ((content, AURORA_TU_TABLES[0]), (settings, AURORA_TU_TABLES[1])):
                    with contextlib.closing(sqlite3.connect(name)) as db:
                        db.execute(table)
                with contextlib.closing(sqlite3.connect(content)) as db, db:  # Aurora has Title Update 1 on its list, and on.
                    db.execute("INSERT INTO TitleUpdates (Id, FileName, LiveDeviceId, LivePath, TitleId, MediaId, Hash, BackupPath) VALUES (7, 'tu00000001_00000000', ?, ?, ?, ?, ?, 'x')",
                               (device, "\\Content\\0000000000000000\\4D5307E6\\000B0000\\", 0x4D5307E6, 0x1A2B3C4D, content_id(older).upper()))
                    db.execute("INSERT INTO TitleUpdates (Id, FileName, TitleId, MediaId, Hash) VALUES (8, 'other', 1, 2, 'AB')")
                with contextlib.closing(sqlite3.connect(settings)) as db, db:
                    db.execute("INSERT INTO ActiveTitleUpdates (TitleUpdateId) VALUES (7), (8)")

                def aurora_starts():
                    run_aurora_script(script, xbox, content, settings)
                    with contextlib.closing(sqlite3.connect(content)) as db:
                        db.execute("ATTACH DATABASE ? AS Config", (settings,))
                        return db.execute("SELECT t.Hash, t.FileName, t.BackupPath, t.TitleId, t.MediaId, t.Version FROM Config.ActiveTitleUpdates a "
                                          "JOIN TitleUpdates t ON t.Id = a.TitleUpdateId ORDER BY t.Id").fetchall()
                active = aurora_starts()
                backup = "Game:\\Data\\TitleUpdates\\%s\\4D5307E6\\%s\\tu00000002_00000000" % (device, content_id(newest).upper())
                self.assertEqual(active, [("AB", "other", None, 1, 2, None), (content_id(newest).upper(), "tu00000002_00000000", backup, 0x4D5307E6, 0x1A2B3C4D, 2)])
                done = [n for n in os.listdir(helper) if n.startswith("tu-aurora-")]
                self.assertEqual(len(done), 1)
                with open(os.path.join(helper, done[0])) as f:
                    self.assertEqual(f.read(), "4D5307E6 1A2B3C4D on\n")
                # Once: a change made in Aurora afterwards stays.
                with contextlib.closing(sqlite3.connect(settings)) as db, db:
                    db.execute("DELETE FROM ActiveTitleUpdates")
                self.assertEqual(aurora_starts(), [])
                # Off: none on in Aurora either.
                with contextlib.closing(sqlite3.connect(settings)) as db, db:
                    db.execute("INSERT INTO ActiveTitleUpdates (TitleUpdateId) VALUES (7)")
                self.json("POST", "/api/titleupdates/off", {"game": "Halo 3", "address": "127.0.0.2"})
                self.assertEqual(wait_queue()[-1]["state"], "done")
                self.assertEqual(aurora_starts(), [])
                self.assertNotEqual([n for n in os.listdir(helper) if n.startswith("tu-aurora-")], done)  # The old done file goes.
                path = next(f["path"] for f in switched()[0]["files"] if f["where"] == "aside" and f["offer"] == "111")
                self.json("POST", "/api/titleupdates/use", {"game": "Halo 3", "address": "127.0.0.2", "path": path})
                self.assertEqual(wait_queue()[-1]["state"], "done")
                self.assertEqual([a[0] for a in aurora_starts()], [content_id(newest).upper()])
            else:
                print("Skipped running the Aurora script: no lupa (pip install lupa).", file=sys.stderr)
            with open(script, encoding="utf-8") as f:
                self.assertIn(content_id(newest).upper(), f.read())
            # Only a switched-off update it listed.
            self.assertEqual(self.call("POST", "/api/titleupdates/use", {"game": "Halo 3", "address": "127.0.0.2", "path": "/Hdd1/Apps/Aurora/Aurora.xex"})[0], 400)
            # An update with an upper-case TU_ name goes in the Cache.
            self.json("POST", "/api/titleupdates/install", {"game": "Forza 4", "address": "127.0.0.2"})
            self.assertEqual(wait_queue()[-1]["state"], "done")
            with open(os.path.join(xbox, "Hdd1", "Cache", "TU_16L61V6_0000014000000.00000000000O9"), "rb") as f:
                self.assertEqual(f.read(), cache_tu)
            # A Cache update (no header) is switched off and on too: it's known
            # as the game's because it was downloaded here.
            def forza():
                self.json("POST", "/api/titleupdates/check", {"address": "127.0.0.2"})
                wait_queue()
                console = self.json("GET", "/api/titleupdates?game=Forza%204")["consoles"][0]
                return console, sorted((f["where"], f["offer"], f["version"]) for f in console["files"])
            console, files = forza()
            self.assertEqual((console["has"], files), ("130", [("live", "130", 4)]))
            self.json("POST", "/api/titleupdates/off", {"game": "Forza 4", "address": "127.0.0.2"})
            job = wait_queue()[-1]
            self.assertEqual((job["kind"], job["state"], job["skipped"]), ("tuoff", "done", 0), job["error"])
            self.assertEqual(os.listdir(os.path.join(xbox, "Hdd1", "Cache")), [])
            console, files = forza()
            self.assertEqual((console["has"], files), ("", [("aside", "130", 4)]))
            path = console["files"][0]["path"]
            self.assertLessEqual(len(path.split("/")[-2]), 42)  # FATX's longest name.
            self.json("POST", "/api/titleupdates/use", {"game": "Forza 4", "address": "127.0.0.2", "path": path})
            job = wait_queue()[-1]
            self.assertEqual((job["state"], job["label"]), ("done", "Title update 4"), job["error"])
            self.assertEqual(os.listdir(os.path.join(xbox, "Hdd1", "Cache")), ["TU_16L61V6_0000014000000.00000000000O9"])
            self.assertEqual(forza()[0]["has"], "130")
            # The whole library at once: looked up, then each console checked.
            self.json("POST", "/api/titleupdates/scan", {})
            scan = None
            for _ in range(100):
                scan = self.json("GET", "/api/titleupdates/all")
                if not scan["scan"]["running"]:
                    break
                time.sleep(0.2)
            wait_queue()
            scan = self.json("GET", "/api/titleupdates/all")
            self.assertEqual(scan["scan"]["found"], 2)
            rows = {g["game"]: g for g in scan["games"]}
            self.assertEqual(rows["Halo 3"]["newest"]["name"], "Title Update 2")
            self.assertTrue(rows["Halo 3"]["consoles"][0]["newest"])
            self.assertTrue(rows["Forza 4"]["consoles"][0]["newest"])
            # Nothing older is left to update.
            self.assertEqual(self.json("POST", "/api/titleupdates/install", {"everything": True})["queued"], 0)
            # A file that isn't what XboxUnity says is refused.
            unity.files["110"] = ("tu00000001_00000000", b"<html>busy</html>")
            shutil.rmtree(os.path.join(os.path.dirname(self.config), "TitleUpdates", "4D5307E6"), ignore_errors=True)
            self.json("POST", "/api/titleupdates/install", {"game": "Halo 3", "address": "127.0.0.2", "id": "110"})
            job = wait_queue()[-1]
            self.assertEqual(job["state"], "failed")
            self.assertIn("isn't the size it lists", job["error"])
            self.assertEqual(os.listdir(updates), ["tu00000002_00000000"])
            # Unknown games and discs without updates say so.
            self.assertEqual(self.call("GET", "/api/titleupdates?game=Nope")[0], 404)
        finally:
            ftp.shutdown()
            ftp.server_close()
            unity.shutdown()
            unity.server_close()

    def check_god(self, header_path, image, start, title_id, media_id, title):
        """Reads a GOD package back as the console does, checking each hash
        on the way: its data is the image's game partition, from start."""
        import hashlib
        sha = lambda b: hashlib.sha1(b).digest()
        with open(header_path, "rb") as f:
            h = f.read()
        self.assertEqual(len(h), 0xB000)
        self.assertEqual(h[:4], b"LIVE")
        be = lambda at, n=4: int.from_bytes(h[at:at + n], "big")
        self.assertEqual((be(0x344), be(0x354), be(0x360)), (0x7000, media_id, title_id))
        self.assertEqual(h[0x411:0x411 + 2 * len(title)].decode("utf-16-be"), title)
        self.assertEqual(h[0x32C:0x340], sha(h[0x344:0x344 + 0xACBC]))
        blocks = be(0x392, 3)
        parts = int.from_bytes(h[0x3A0:0x3A4], "little")
        self.assertEqual(parts, (blocks + 0xA1C3) // 0xA1C4)
        data = bytearray()
        expected_master = h[0x37D:0x37D + 20]
        total = 0
        for p in range(parts):
            with open(os.path.join(header_path + ".data", "Data%04d" % p), "rb") as f:
                part = f.read()
            total += len(part)
            master = part[:4096]
            self.assertEqual(sha(master), expected_master, "Data%04d's master block" % p)
            in_part = min(0xA1C4, blocks - p * 0xA1C4)
            groups = (in_part + 0xCB) // 0xCC
            self.assertEqual(len(part), 4096 * (1 + groups + in_part))
            at = 4096
            for g in range(groups):
                hashes = part[at:at + 4096]
                self.assertEqual(master[g * 20:g * 20 + 20], sha(hashes))
                n = min(0xCC, in_part - g * 0xCC)
                for b in range(n):
                    block = part[at + 4096 * (1 + b):at + 4096 * (2 + b)]
                    self.assertEqual(hashes[b * 20:b * 20 + 20], sha(block))
                    data += block
                at += 4096 * (1 + n)
            expected_master = master[groups * 20:groups * 20 + 20]
        self.assertEqual(be(0x3A4) * 0x100, total)
        self.assertEqual(len(data), blocks * 4096)
        partition = image[start:start + len(data)]
        self.assertEqual(bytes(data[:len(partition)]), partition)
        self.assertEqual(bytes(data[len(partition):]), bytes(len(data) - len(partition)))

    def test_tells_when_aurora_isn_t_there_to_install_to(self):
        self.add_iso("Forza 4.iso", 0x11112222, 0x4D530910)
        write_tree_iso(os.path.join(self.isos, "Forza 4 - Disc 2.iso"), {
            "default.xex": xex(execution_info(0x33334444, 0x4D530910, 2, 2)),
            "content": {"0000000000000000": {"4D530910": {"00000002": {"4D53091000000001": b"PIRS" + bytes(500)}}}}})
        self.start("--xbox", "127.0.0.2", "--ftp-port", str(free_port()))
        self.state_when(lambda s: s["library"]["games"] == 2)
        self.json("POST", "/api/install", {"game": "Forza 4"})
        job = self.state_when(lambda s: s["installs"][-1]["state"] not in ("queued", "copying"), 30)["installs"][-1]
        self.assertEqual(job["state"], "failed")
        self.assertIn("Start Aurora", job["error"])

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
        # A wrong password: that address waits a second before it may try
        # again (doubling with each one more), without holding others up.
        self.assertEqual(self.call("POST", "/api/login", {"password": "hunter2"})[0], 401)
        status, _, body = self.call("POST", "/api/login", {"password": "hunter2 x"})
        self.assertEqual(status, 429)
        self.assertIn("Try again in 1 s", json.loads(body)["error"])
        time.sleep(1.1)
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

    def test_logging_out_ends_the_session(self):
        self.start()
        _, response, _ = self.call("POST", "/api/password", {"password": "hunter2"})
        cookie = response.getheader("Set-Cookie").split(";")[0]
        self.assertEqual(self.call("GET", "/api/state", cookie=cookie)[0], 200)
        status, response, _ = self.call("POST", "/api/logout", {}, cookie=cookie)
        self.assertEqual(status, 200)
        self.assertIn("Max-Age=0", response.getheader("Set-Cookie"))
        # A copy of the cookie doesn't work any more either.
        self.assertEqual(self.call("GET", "/api/state", cookie=cookie)[0], 401)
        status, response, _ = self.call("POST", "/api/login", {"password": "hunter2"})
        self.assertEqual(self.call("GET", "/api/state", cookie=response.getheader("Set-Cookie").split(";")[0])[0], 200)

    def test_metrics_for_prometheus(self):
        self.add_iso("Halo 3.iso", 0x1A2B3C4D, 0x4D5307E6)
        self.start("--xbox", "127.0.0.2")
        client = Client.__new__(Client)  # As the console at 127.0.0.2.
        client.conn = socket.create_connection(("127.0.0.1", self.port), timeout=10, source_address=("127.0.0.2", 0))
        self.assertEqual(client.mount("Halo 3.iso"), 1)
        client.close()
        status, response, body = self.call("GET", "/metrics", page=False)
        self.assertEqual(status, 200)
        self.assertTrue(response.getheader("Content-Type").startswith("text/plain"))
        text = body.decode()
        self.assertIn("netdisc_games 1\n", text)
        self.assertIn("netdisc_game_starts_total 1\n", text)
        self.assertIn('netdisc_console_connections{console="127.0.0.2"}', text)
        # With a password: as HTTP Basic auth, any user name.
        self.call("POST", "/api/password", {"password": "hunter2"})
        status, response, _ = self.call("GET", "/metrics", page=False)
        self.assertEqual(status, 401)
        self.assertIn("Basic", response.getheader("WWW-Authenticate"))
        good = {"Authorization": "Basic " + base64.b64encode(b"prometheus:hunter2").decode()}
        self.assertEqual(self.call("GET", "/metrics", page=False, headers=good)[0], 200)
        bad = {"Authorization": "Basic " + base64.b64encode(b"prometheus:guess").decode()}
        self.assertEqual(self.call("GET", "/metrics", page=False, headers=bad)[0], 401)
        self.assertEqual(self.call("GET", "/metrics", page=False, headers=good)[0], 429)  # It waits a second now.

    def test_speed_test_over_aurora_ftp(self):
        xbox = os.path.join(self.tmp.name, "xbox")
        os.makedirs(os.path.join(xbox, "Hdd1"))
        ftp = aurora_ftp.Server(xbox, 0, host="127.0.0.2")  # A port free on 127.0.0.2 itself.
        ftp_port = ftp.server_address[1]
        threading.Thread(target=ftp.serve_forever, daemon=True).start()
        try:
            self.start("--xbox", "127.0.0.2", "--ftp-port", str(ftp_port))
            result = self.json("POST", "/api/consoles/speed", {"address": "127.0.0.2"})
            self.assertEqual(result["bytes"], 32 << 20)
            self.assertGreater(result["toXbox"], 0)
            self.assertGreater(result["fromXbox"], 0)
            # The test file is gone again.
            self.assertEqual(os.listdir(os.path.join(xbox, "Hdd1", "Apps", "NetDisc360")), [])
            self.assertEqual(self.call("POST", "/api/consoles/speed", {"address": "127.0.0.3"})[0], 404)
        finally:
            ftp.shutdown()
            ftp.server_close()

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
