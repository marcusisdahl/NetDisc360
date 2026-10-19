"""Controlled NetISO-protocol server for FastFTP's read benchmark.

Speaks the same wire protocol as tuxuser/netiso-srv (src/main.rs at 40e0a77):
20-byte big-endian requests b"ISVR", u16 command, u16 ISO index, u64 offset,
u32 length. Unlike that server it reads every request completely before acting
and sends every reply completely (the reference uses one read() per request
and an unbuffered try_write(), which can cut replies short; see
docs/READ-BENCHMARKS.md). Each connection has its own mounted ISO, as there.

It only reads the ISO files. Fault options exist for FastFTP's host tests.

    python tests/netiso_server.py <folder with .iso files> [--port 4323]
"""
import argparse
import os
import socket
import struct
import sys
import threading
import time

PING, GET_ISO_SIZE, HAS_TYPE1, READ_DATA, GET_ISO_NAME, MOUNT_ISO = range(6)
SECTOR = 2048


def exact(conn, count):
    data = bytearray()
    while len(data) < count:
        part = conn.recv(count - len(data))
        if not part:
            raise EOFError
        data.extend(part)
    return bytes(data)


class Client(threading.Thread):
    def __init__(self, conn, args, isos):
        super().__init__(daemon=True)
        self.conn, self.args, self.isos = conn, args, isos
        self.mounted = None
        self.reads = 0

    def send(self, data):
        fault = self.args.fault
        if fault == "split":
            # Many small writes with pauses: the client must reassemble replies.
            for i in range(0, len(data), 7):
                self.conn.sendall(data[i:i + 7])
                if i % 4096 == 0:
                    time.sleep(0.0005)
        elif fault == "trywrite":
            # Emulates the reference server when its send buffer is full:
            # only part of the reply is written and the rest is dropped.
            self.conn.sendall(data[:self.args.trywrite_limit])
        else:
            self.conn.sendall(data)

    def request(self):
        if self.args.fault == "split":
            data = b"".join(exact(self.conn, 1) for _ in range(20))
        else:
            data = exact(self.conn, 20)
        if data[:4] != b"ISVR":
            raise ValueError("bad magic")
        return struct.unpack(">HHQI", data[4:])

    def run(self):
        try:
            while True:
                command, index, offset, length = self.request()
                if not self.handle(command, index, offset, length):
                    break
        except (EOFError, ConnectionError, ValueError, OSError):
            pass
        finally:
            if self.mounted:
                self.mounted.close()
            self.conn.close()

    def handle(self, command, index, offset, length):
        fault = self.args.fault
        if command == PING:
            if fault == "not-netiso":
                self.send(b"HTTP/1.0")
            else:
                self.send(b"ISVRokOK")
            return fault != "close-after-ping"
        if command == GET_ISO_NAME:
            if length > 4096:
                return False
            if fault == "bad-name" and index == 0:
                name = b"bad\x01name.iso"
            elif fault == "long-name" and index == 0:
                name = b"x" * length
            else:
                name = os.path.basename(self.isos[index]).encode() if index < len(self.isos) else b""
            self.send(name[:length].ljust(length, b"\0"))
            return True
        if command == HAS_TYPE1:
            self.send(struct.pack(">I", 0))
            return True
        if command == MOUNT_ISO:
            name = exact(self.conn, length).decode("utf-8", "replace").replace("\\Mount", "").replace("\\", "").replace("\0", "")
            if fault == "bad-mount":
                self.send(struct.pack(">I", 7))
                return True
            found = next((p for p in self.isos if os.path.basename(p) == name), None)
            if self.mounted:
                self.mounted.close()
                self.mounted = None
            if found:
                self.mounted = open(found, "rb")
            self.send(struct.pack(">I", 1 if found else 0))
            return True
        if command == GET_ISO_SIZE:
            sectors = os.fstat(self.mounted.fileno()).st_size // SECTOR if self.mounted else 0
            self.send(struct.pack(">II", sectors, 4096 if fault == "bad-sector-size" else SECTOR))
            return True
        if command == READ_DATA:
            if not self.mounted:
                return True  # As the reference server: no reply at all.
            size = os.fstat(self.mounted.fileno()).st_size
            if length > 16 * 1048576 or offset + length > size:
                return False
            self.reads += 1
            self.mounted.seek(offset)
            data = self.mounted.read(length)
            nth = self.reads == self.args.nth
            if nth and fault == "corrupt":
                data = bytearray(data)
                data[len(data) // 2] ^= 0x40
                data = bytes(data)
            if nth and fault in ("truncate", "truncate-stall"):
                self.conn.sendall(data[:len(data) // 2])
                if fault == "truncate-stall":
                    time.sleep(60)
                return False
            if nth and fault == "stall":
                time.sleep(60)
                return False
            if nth and fault == "disconnect":
                return False
            if fault == "slow":
                time.sleep(self.args.delay)
            self.send(data)
            return True
        return False


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("folder")
    parser.add_argument("--port", type=int, default=4323)
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--fault", default="none", choices=["none", "split", "trywrite", "truncate", "truncate-stall", "stall",
                        "corrupt", "disconnect", "slow", "bad-sector-size", "bad-mount", "bad-name", "long-name",
                        "not-netiso", "close-after-ping"])
    parser.add_argument("--nth", type=int, default=5, help="ReadData request the fault applies to")
    parser.add_argument("--trywrite-limit", type=int, default=65536)
    parser.add_argument("--delay", type=float, default=0.05)
    args = parser.parse_args()
    isos = sorted(os.path.join(args.folder, n) for n in os.listdir(args.folder) if n.lower().endswith(".iso"))
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((args.bind, args.port))
    listener.listen(8)
    print("READY %d %d ISO(s)" % (listener.getsockname()[1], len(isos)), flush=True)
    for path in isos:
        print("  " + os.path.basename(path), flush=True)
    while True:
        conn, peer = listener.accept()
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        Client(conn, args, isos).start()


if __name__ == "__main__":
    sys.exit(main())
