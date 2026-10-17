"""Makes one Aurora launcher per Xbox 360 ISO served by NetISO.

For each .iso under the ISO folder it reads default.xex from the ISO, copies
the launcher XEX with that game's execution info (Title ID, Media ID,
versions, disc number) written into its header, and writes netiso.ini with
the ISO's name next to it:

    <output>/<game>/default.xex
    <output>/<game>/netiso.ini

Copy the output folder to the console (for example Hdd1:\\NetISO) and add it
to Aurora as a scan path with depth 1. Aurora then lists each game with its
cover art; starting one mounts its ISO through the NetISO plugin and boots it.

    python tools/netiso_aurora.py <ISO folder> <output folder>
        [--launcher build/xbox/NetIsoLauncher.xex] [--recursive]

Only reads the ISOs. Game folders that already exist in the output folder
are rewritten, and nothing else there is touched.
"""
import argparse
import hashlib
import os
import re
import struct
import sys

SECTOR = 2048
XGD_MAGIC = b"MICROSOFT*XBOX*MEDIA"
# (where the magic is, where the game partition starts), as netiso-srv checks.
PARTITIONS = ((0xFDA0000, 0xFD90000), (0x2090000, 0x2080000), (0x10000, 0))
EXECUTION_INFO = 0x40006
EXECUTION_INFO_SIZE = 24
FATX_NAME_MAX = 42
# Header digest in the security info, and where the part it skips ends.
XEX_DIGEST_AT, XEX_DIGEST_END = 0x164, 0x17C
# FATX allows letters, digits, space and !#$%&'()-.@[]^_`{}~ in names.
FATX_OK = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 !#$%&'()-.@[]^_`{}~")
DEFAULT_LAUNCHER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "xbox", "NetIsoLauncher.xex")


class IsoError(Exception):
    pass


def read_at(f, offset, size):
    f.seek(offset)
    data = f.read(size)
    if len(data) != size:
        raise IsoError("ends before offset 0x%x" % (offset + size))
    return data


def partition_start(f, file_size):
    for magic_at, start in PARTITIONS:
        if file_size >= magic_at + len(XGD_MAGIC) and read_at(f, magic_at, len(XGD_MAGIC)) == XGD_MAGIC:
            return start
    raise IsoError("no Xbox 360 game partition (not an XGD ISO)")


def root_entries(f, start):
    """Yields (name, sector, size) for each entry in the root directory."""
    volume = read_at(f, start + 32 * SECTOR, 28)
    root_sector, root_size = struct.unpack_from("<II", volume, 20)
    if not root_size or root_size > 64 * 1024 * 1024:
        raise IsoError("bad root directory size %d" % root_size)
    data = read_at(f, start + root_sector * SECTOR, root_size)
    pos = 0
    while pos + 14 <= len(data):
        if data[pos:pos + 2] == b"\xff\xff":  # Padding up to the next sector.
            pos = (pos // SECTOR + 1) * SECTOR
            continue
        sector, size, _attributes, length = struct.unpack_from("<IIBB", data, pos + 4)
        if not length or pos + 14 + length > len(data):
            break
        yield data[pos + 14:pos + 14 + length].decode("latin-1"), sector, size
        pos = (pos + 14 + length + 3) & ~3


def xex_headers(data):
    """Returns {key: value} of a XEX2 header's optional header table."""
    if data[:4] != b"XEX2":
        raise IsoError("default.xex is not a XEX2 file")
    count = struct.unpack_from(">I", data, 20)[0]
    if 24 + count * 8 > len(data):
        raise IsoError("default.xex header is cut short")
    return dict(struct.unpack_from(">II", data, 24 + i * 8) for i in range(count))


def game_execution_info(iso_path):
    """Returns the 24-byte execution info of the ISO's default.xex."""
    with open(iso_path, "rb") as f:
        start = partition_start(f, os.fstat(f.fileno()).st_size)
        for name, sector, size in root_entries(f, start):
            if name.lower() == "default.xex":
                break
        else:
            raise IsoError("no default.xex in the root of the disc")
        offset = start + sector * SECTOR
        head = read_at(f, offset, min(size, 24))
        header_size = struct.unpack_from(">I", head, 8)[0] if len(head) >= 12 else 0
        if not 24 <= header_size <= min(size, 0x100000):
            raise IsoError("default.xex has a bad header size")
        headers = xex_headers(read_at(f, offset, header_size))
        at = headers.get(EXECUTION_INFO)
        if at is None or at + EXECUTION_INFO_SIZE > header_size:
            raise IsoError("default.xex has no execution info")
        return read_at(f, offset + at, EXECUTION_INFO_SIZE)


def fix_header_digest(xex):
    """Recomputes the header digest of a XEX whose header was edited.

    The digest covers the header from the end of the security info's fixed
    part to the end of the header, then everything before the RSA signature.
    The loader checks it even when the signature check is patched out.
    """
    header_size, security = struct.unpack_from(">I", xex, 8)[0], struct.unpack_from(">I", xex, 16)[0]
    if header_size > len(xex) or security < 24 or security + XEX_DIGEST_END > header_size:
        return False
    digest = hashlib.sha1(bytes(xex[security + XEX_DIGEST_END:header_size]) + bytes(xex[:security + 8])).digest()
    xex[security + XEX_DIGEST_AT:security + XEX_DIGEST_AT + 20] = digest
    return True


def launcher_info_offset(launcher):
    headers = xex_headers(launcher)
    at = headers.get(EXECUTION_INFO)
    header_size = struct.unpack_from(">I", launcher, 8)[0]
    if at is None or at + EXECUTION_INFO_SIZE > header_size:
        raise SystemExit("The launcher XEX has no execution info header. Build it with a TitleID (see launcher/NetIsoLauncher.vcxproj).")
    return at


# Release tags in No-Intro and Redump names say which release an ISO is, not
# which game: "(USA, Europe)", "(En,Ja,Fr,De)", "(Rev 1)", "(v1.02)", "[!]".
# Folders leave them out, so Aurora shows a clean name until its lookup names
# the game. Other tags stay: "(Disc 1)", "(Bonus Disc)", "(Demo)". Matches
# FolderName in shared/library.cpp.
REGIONS = {r.lower() for r in (
    "USA", "Europe", "Japan", "World", "Asia", "Australia", "Austria", "Belgium", "Brazil", "Canada", "China", "Czech",
    "Denmark", "Finland", "France", "Germany", "Greece", "Hong Kong", "India", "Ireland", "Israel", "Italy", "Korea",
    "Latin America", "Mexico", "Netherlands", "New Zealand", "Norway", "Poland", "Portugal", "Russia", "Scandinavia",
    "South Africa", "Spain", "Sweden", "Switzerland", "Taiwan", "Turkey", "UK", "Unknown")}
RELEASE_ITEM = re.compile(r"[A-Z][a-z](-[A-Za-z]+)?|[Rr][Ee][Vv] .+|[vV][0-9][0-9.]*|!")


def release_tag(tag):
    return all(item.strip(" ").lower() in REGIONS or RELEASE_ITEM.fullmatch(item.strip(" ")) for item in tag.split(","))


def friendly_stem(stem):
    """stem without the release tags among the tags at its end, or all of it if only tags would be left."""
    end, kept = len(stem), []
    while True:
        while end and stem[end - 1] == " ":
            end -= 1
        opener = {")": "(", "]": "["}.get(stem[end - 1]) if end else None
        if not opener:
            break
        at = stem.rfind(opener, 0, end - 1)
        if at <= 0:
            break
        if not release_tag(stem[at + 1:end - 1]):
            if len(kept) == 16:
                break
            kept.append(stem[at:end])
        end = at
    title = stem[:end].rstrip(" ")
    return " ".join([title] + kept[::-1]) if title else stem


def safe_name(text):
    # Commas are common in No-Intro names ("(USA, Europe)") and read fine dropped.
    name = "".join(c if c in FATX_OK else "_" for c in text.replace(",", ""))
    return name.lstrip(" .")[:FATX_NAME_MAX].rstrip(" .") or "Game"


def folder_name(iso_name, used):
    stem = os.path.splitext(iso_name)[0]
    name = safe_name(friendly_stem(stem))
    # Another release of the same game keeps its tags, and after that a number.
    candidate, n = name if name.lower() not in used else safe_name(stem), 2
    while candidate.lower() in used:
        suffix = " (%d)" % n
        candidate = name[:FATX_NAME_MAX - len(suffix)].rstrip(" .") + suffix
        n += 1
    used.add(candidate.lower())
    return candidate


def find_isos(root, recursive):
    if recursive:
        for folder, _dirs, files in os.walk(root):
            for name in sorted(files):
                if name.lower().endswith(".iso"):
                    yield os.path.join(folder, name)
    else:
        for name in sorted(os.listdir(root)):
            path = os.path.join(root, name)
            if name.lower().endswith(".iso") and os.path.isfile(path):
                yield path


def main(argv=None):
    parser = argparse.ArgumentParser(description="Make one Aurora launcher per NetISO ISO.")
    parser.add_argument("isos", help="folder the NetISO server serves")
    parser.add_argument("output", help="folder to write the game folders to")
    parser.add_argument("--launcher", default=DEFAULT_LAUNCHER, help="NetIsoLauncher.xex (default: %(default)s)")
    parser.add_argument("--recursive", action="store_true", help="also look in subfolders (like netiso-srv -r)")
    args = parser.parse_args(argv)

    with open(args.launcher, "rb") as f:
        launcher = f.read()
    info_at = launcher_info_offset(launcher)
    os.makedirs(args.output, exist_ok=True)

    made, failed, used, names = 0, 0, set(), {}
    for iso in find_isos(args.isos, args.recursive):
        iso_name = os.path.basename(iso)
        # The plugin mounts by file name (the server matches the end of the
        # name), so two ISOs with one name can't be told apart.
        if iso_name.lower() in names:
            print("skipped  %s: same file name as %s" % (iso, names[iso_name.lower()]))
            failed += 1
            continue
        names[iso_name.lower()] = iso
        try:
            info = game_execution_info(iso)
        except (IsoError, OSError) as error:
            print("skipped  %s: %s" % (iso, error))
            failed += 1
            continue
        media_id, title_id = struct.unpack_from(">I", info, 0)[0], struct.unpack_from(">I", info, 12)[0]
        disc, discs = info[18], info[19]
        game = folder_name(iso_name, used)
        folder = os.path.join(args.output, game)
        os.makedirs(folder, exist_ok=True)
        patched = bytearray(launcher)
        patched[info_at:info_at + EXECUTION_INFO_SIZE] = info
        fix_header_digest(patched)
        with open(os.path.join(folder, "default.xex"), "wb") as f:
            f.write(patched)
        with open(os.path.join(folder, "netiso.ini"), "w", newline="\r\n") as f:
            f.write("; Written by netiso_aurora.py. iso is the file name the NetISO server lists.\n")
            f.write("iso=%s\ntitle_id=%08X\nmedia_id=%08X\n" % (iso_name, title_id, media_id))
        print("made     %s -> %s (Title ID %08X, Media ID %08X, disc %d/%d)" % (iso_name, game, title_id, media_id, disc, discs))
        made += 1
    print("%d launcher(s) made, %d ISO(s) skipped." % (made, failed))
    return 0 if made or not failed else 1


if __name__ == "__main__":
    sys.exit(main())
