"""Takes the Xbox files built into NetDisc360.exe (server/server.rc) out of
it, for the Docker image: the game launcher, its helper, the LAN party app and
the plugin. They are Xbox programs made with the Xbox 360 SDK on Windows, so
the Linux build can't make them; the release's NetDisc360.exe carries them.

    python3 tools/exe_files.py NetDisc360.exe <folder>

writes <folder>/default.xex, boot.xex, party/default.xex and NetDisc360.xex
(those the exe has; the launcher at least, or it fails). Reads the exe's
resource table only: nothing in it is run.
"""
import os
import struct
import sys

RT_RCDATA = 10
# server/resource.h's IDs, and where the Docker image keeps each
# (docker/entrypoint.sh and server/main.cpp look there).
FILES = {101: "default.xex", 102: "boot.xex", 112: "party/default.xex", 113: "NetDisc360.xex"}


def rcdata(exe):
    """{id: bytes} of the exe's RCDATA resources."""
    if exe[:2] != b"MZ":
        raise ValueError("not a Windows program")
    pe = struct.unpack_from("<I", exe, 0x3C)[0]
    if exe[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a Windows program")
    sections, optional = struct.unpack_from("<H12xH", exe, pe + 6)
    opt = pe + 24
    magic = struct.unpack_from("<H", exe, opt)[0]
    directories = opt + (112 if magic == 0x20B else 96)  # PE32+ or PE32.
    rva, size = struct.unpack_from("<II", exe, directories + 2 * 8)  # The resource table.
    if not rva:
        return {}
    table = []
    for s in range(sections):
        at = opt + optional + 40 * s
        vsize, vaddr, rawsize, rawat = struct.unpack_from("<IIII", exe, at + 8)
        table.append((vaddr, max(vsize, rawsize), rawat))

    def offset(address):
        for vaddr, length, rawat in table:
            if vaddr <= address < vaddr + length:
                return rawat + address - vaddr
        raise ValueError("a resource outside the exe's sections")
    root = offset(rva)

    def entries(directory):
        named, numbered = struct.unpack_from("<HH", exe, root + directory + 12)
        for i in range(named + numbered):
            name, child = struct.unpack_from("<II", exe, root + directory + 16 + 8 * i)
            yield name, child

    out = {}
    for kind, types in entries(0):
        if kind != RT_RCDATA or not types & 0x80000000:
            continue
        for ident, languages in entries(types & 0x7FFFFFFF):
            if ident & 0x80000000 or not languages & 0x80000000:
                continue  # Named, or not a folder of languages.
            for _, leaf in entries(languages & 0x7FFFFFFF):
                if leaf & 0x80000000:
                    continue
                address, length = struct.unpack_from("<II", exe, root + leaf)
                start = offset(address)
                out[ident] = exe[start:start + length]
                break
    return out


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    with open(sys.argv[1], "rb") as f:
        found = rcdata(f.read())
    if 101 not in found:
        sys.exit("%s has no game launcher built in." % sys.argv[1])
    for ident, name in FILES.items():
        if ident not in found:
            print("Not in it: %s" % name)
            continue
        path = os.path.join(sys.argv[2], name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(found[ident])
        print("%s (%d bytes)" % (name, len(found[ident])))


if __name__ == "__main__":
    main()
