"""A stand-in for Aurora's FTP server (FtpDll), for tests/test_server.py.

Serves a local folder as the console's "/" and answers the way FtpDll did
on Aurora 0.7b.2 (2026-10-17): PASV only (no EPSV or MLSD), LIST and NLST
ignore any path and list the current folder in Unix style, a missing file is
"550 Could not open file.  Error:  2", and NOOP answers with PowerPC.

    python tests/aurora_ftp.py <folder> [--port 0]
"""
import argparse
import os
import socket
import socketserver
import sys
import time


class Handler(socketserver.StreamRequestHandler):
    def reply(self, line):
        self.wfile.write((line + "\r\n").encode("utf-8", "surrogateescape"))
        self.wfile.flush()

    def local(self, path):
        if not path.startswith("/"):
            path = self.cwd.rstrip("/") + "/" + path
        parts = [p for p in path.split("/") if p and p != "."]
        if ".." in parts:
            return None
        return os.path.join(self.server.root, *parts)

    def data_connection(self):
        if not self.passive:
            return None
        self.passive.settimeout(10)
        conn, _ = self.passive.accept()
        self.passive.close()
        self.passive = None
        return conn

    def handle(self):
        self.cwd = "/"
        self.passive = None
        user = None
        logged_in = False
        self.reply("220 FtpDll Ready")
        for raw in self.rfile:
            line = raw.decode("utf-8", "surrogateescape").rstrip("\r\n")
            command, _, argument = line.partition(" ")
            command = command.upper()
            self.server.log.append(line if command != "PASS" else "PASS ***")
            if command == "USER":
                user = argument
                self.reply("331 User %s OK, need password" % argument)
            elif command == "PASS":
                if user == self.server.user and argument == self.server.password:
                    logged_in = True
                    self.reply("230 User logged in")
                else:
                    self.reply("530 Login incorrect")
            elif command == "QUIT":
                self.reply("221 Goodbye")
                return
            elif not logged_in:
                self.reply("530 Not logged in")
            elif command == "SYST":
                self.reply("215 UNIX Type: L8")
            elif command == "TYPE":
                self.reply("200 Type set to %s" % argument)
            elif command == "SITE":
                # FtpDll 1655: "SITE INITIATESCAN <Path>" starts Aurora's content scan.
                verb, _, path = argument.partition(" ")
                if verb.upper() == "INITIATESCAN":
                    self.server.scans.append(path)
                    self.reply("200 Initiating Content Scan")
                elif verb.upper() == "REVISION":
                    self.reply("200 1655")
                else:
                    self.reply("502 Command SITE not implemented for this argument.")
            elif command == "NOOP":
                self.reply("200 ori r0, r0, 0")
            elif command == "PWD":
                self.reply('257 "%s"' % self.cwd)
            elif command == "CWD":
                path = self.local(argument)
                if path and os.path.isdir(path):
                    self.cwd = "/" + os.path.relpath(path, self.server.root).replace(os.sep, "/").strip(".") + "/"
                    self.cwd = self.cwd.replace("//", "/")
                    self.reply("250 Current directory is %s" % self.cwd)
                else:
                    self.reply("550 No such directory")
            elif command == "PASV":
                if self.passive:
                    self.passive.close()
                self.passive = socket.socket()
                self.passive.bind((self.server.host, 0))
                self.passive.listen(1)
                port = self.passive.getsockname()[1]
                self.reply("227 Entering Passive Mode (%s,%d,%d)" % (self.server.host.replace(".", ","), port >> 8, port & 255))
            elif command in ("LIST", "NLST"):
                # FtpDll ignores the argument.
                conn = self.data_connection()
                if not conn:
                    self.reply("425 No data connection")
                    continue
                self.reply("150 Opening data connection")
                folder = self.local(self.cwd)
                lines = []
                for name in sorted(os.listdir(folder)):
                    full = os.path.join(folder, name)
                    kind = "d" if os.path.isdir(full) else "-"
                    size = 0 if kind == "d" else os.path.getsize(full)
                    stamp = time.strftime("%b %d %H:%M", time.localtime(os.path.getmtime(full)))
                    lines.append("%srwxrwxrwx   1 root root %13d %s %s\r\n" % (kind, size, stamp, name))
                conn.sendall("".join(lines).encode("utf-8", "surrogateescape"))
                conn.close()
                self.reply("226 Transfer complete")
            elif command == "RETR":
                path = self.local(argument)
                conn = self.data_connection()
                if not path or not os.path.isfile(path):
                    if conn:
                        conn.close()
                    self.reply("550 Could not open file.  Error:  2")
                    continue
                self.reply("150 Opening data connection")
                with open(path, "rb") as f:
                    data = f.read()
                try:
                    conn.sendall(data)
                    sent = True
                except OSError:  # The client took what it wanted and closed.
                    sent = False
                conn.close()
                self.reply("226 File transferred" if sent else "426 Connection closed; transfer aborted")
            elif command == "STOR":
                path = self.local(argument)
                conn = self.data_connection()
                if not path or not os.path.isdir(os.path.dirname(path)) or os.path.isdir(path):
                    if conn:
                        conn.close()
                    self.reply("550 Could not create file.")
                    continue
                self.reply("150 Opening data connection")
                chunks = []
                while True:
                    part = conn.recv(65536)
                    if not part:
                        break
                    chunks.append(part)
                conn.close()
                with open(path, "wb") as f:
                    f.write(b"".join(chunks))
                self.server.stored.append(argument)
                self.reply("226 File transferred")
            elif command == "MKD":
                path = self.local(argument)
                try:
                    os.mkdir(path)
                    self.reply('257 "%s" created' % argument)
                except OSError:
                    self.reply("550 Could not create directory")
            elif command == "DELE":
                path = self.local(argument)
                if path and os.path.isfile(path):
                    os.remove(path)
                    self.reply("250 File deleted")
                else:
                    self.reply("550 Could not delete file")
            elif command == "RMD":
                path = self.local(argument)
                try:
                    os.rmdir(path)
                    self.reply("250 Directory removed")
                except OSError:
                    self.reply("550 Could not remove directory")
            else:
                self.reply("502 Command not found")


class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, root, port=0, user="xboxftp", password="xboxftp", host="127.0.0.1"):
        super().__init__((host, port), Handler)
        self.root, self.user, self.password, self.host = root, user, password, host
        self.log, self.stored, self.scans = [], [], []


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("folder")
    parser.add_argument("--port", type=int, default=0)
    args = parser.parse_args()
    server = Server(args.folder, args.port)
    print("READY %d" % server.server_address[1], flush=True)
    server.serve_forever()


if __name__ == "__main__":
    sys.exit(main())
