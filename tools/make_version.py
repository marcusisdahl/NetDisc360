"""Writes shared/build_id.h: which build this is, for every program NetDisc360
makes (the server and its dashboard, NetDiscParty.xex, the launchers and the
NetDisc360.xex plugin), so each one can show it beside the version
(server/version.h):

    #define NETDISC_COMMIT "c9c9b76"          the git commit ("" outside git;
                                              a + after it: files changed since)
    #define NETDISC_BUILD_STAMP 1762032000u   when this was run (Unix seconds)

build.ps1 runs it first, so everything one build makes carries the same
stamp: the server tells the wheel app it carries from an older one by it.
Other builds (the tests, Docker, the Visual Studio project) make it through
tools/make_web.py when it's missing. shared/build_id.h isn't kept in git.

    python tools/make_version.py              (re)writes it
    python tools/make_version.py --if-missing writes it only if it isn't there
"""
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "shared", "build_id.h")


def commit():
    def git(*args):
        return subprocess.run(["git", "-C", ROOT] + list(args), stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                              text=True, check=True).stdout.strip()
    try:
        head = git("rev-parse", "--short=7", "HEAD")
        changed = git("status", "--porcelain", "--untracked-files=no")
    except (OSError, subprocess.CalledProcessError):
        return ""
    return head + ("+" if changed else "")


def write():
    text = ("// Made by tools/make_version.py; not kept in git.\n"
            "#pragma once\n"
            f'#define NETDISC_COMMIT "{commit()}"\n'
            f"#define NETDISC_BUILD_STAMP {int(time.time())}u\n")
    with open(HEADER, "w", newline="\n") as f:
        f.write(text)


def ensure():
    if not os.path.exists(HEADER):
        write()


if __name__ == "__main__":
    if sys.argv[1:] == ["--if-missing"]:
        ensure()
    else:
        write()
