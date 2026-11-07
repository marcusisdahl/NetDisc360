"""Builds server/systemlink.cpp, NetDisc360's list of System Link games (the
dashboard's System Link list), from data/systemlink.json. Run it after
changing the JSON:

    python tools/make_systemlink.py              rebuilds server/systemlink.cpp
    python tools/make_systemlink.py --check      fails if server/systemlink.cpp is out of date
    python tools/make_systemlink.py <json> [<cpp>]
                                                 from another file like it

The JSON lists games as {"name", "titleId" (8 hex digits, or "" when not
known), "platform" ("360" or "xbox"), "players": {"total", "perConsole"}
(numbers, or null when not known), "notes"}, with an "attribution" for the
whole list. Only games with a Title ID go in, since that's how the server
tells a game in the library; a game listed twice under one Title ID (GRID
and Race Driver: GRID) becomes one row, its names joined and each count the
higher one known. The data is CC BY-SA 4.0 (see data/README.md), and so is
the table this writes; the server's code stays MIT.
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, "data", "systemlink.json")
TARGET = os.path.join(ROOT, "server", "systemlink.cpp")
MERGED = "A game listed twice under one Title ID is one row here, its names joined."


def literal(text):
    """A C string literal of UTF-8 text: anything but printable ASCII as
    octal escapes (three digits, so a digit after one can't run into it)."""
    out = []
    for b in text.encode("utf-8"):
        c = chr(b)
        if c in '"\\?':
            out.append("\\" + c)
        elif 32 <= b < 127:
            out.append(c)
        else:
            out.append("\\%03o" % b)
    return '"' + "".join(out) + '"'


def count(value):
    return value if isinstance(value, int) and not isinstance(value, bool) and value >= 1 else 0


def rows(data):
    merged = {}
    for game in data["games"]:
        title = (game.get("titleId") or "").strip().upper()
        if not title:
            continue
        if not re.fullmatch(r"[0-9A-F]{8}", title):
            sys.exit("%s: Title ID %r isn't 8 hex digits" % (game.get("name"), title))
        if game.get("platform") not in ("360", "xbox"):
            sys.exit("%s: platform %r isn't 360 or xbox" % (game.get("name"), game.get("platform")))
        players = game.get("players") or {}
        row = merged.setdefault(title, {"names": [], "platform": game["platform"], "total": 0, "perConsole": 0, "notes": []})
        if row["platform"] != game["platform"]:
            sys.exit("Title ID %s is both a 360 and an Xbox game" % title)
        name = game["name"].strip()
        if name not in row["names"]:
            row["names"].append(name)
        row["total"] = max(row["total"], count(players.get("total")))
        row["perConsole"] = max(row["perConsole"], count(players.get("perConsole")))
        note = (game.get("notes") or "").strip()
        if note and note not in row["notes"]:
            row["notes"].append(note)
    out = []
    for title in sorted(merged, key=lambda t: int(t, 16)):
        row = merged[title]
        if row["total"] and row["perConsole"] > row["total"]:
            sys.exit("%s: %d per console but %d in all" % (title, row["perConsole"], row["total"]))
        out.append((title, " / ".join(row["names"]), row["platform"], row["total"], row["perConsole"], " | ".join(row["notes"])))
    return out


def source(data, path):
    lines = ["    {0x%s,%s,%s,%d,%d,\n     %s}," % (title, literal(name), "true" if platform == "xbox" else "false", total, per, literal(notes))
             for title, name, platform, total, per, notes in rows(data)]
    attribution = (data.get("attribution") or "").strip()
    if not attribution:
        sys.exit("%s has no attribution" % path)
    return ("// SPDX-License-Identifier: CC-BY-SA-4.0\n"
            "// Made by tools/make_systemlink.py from data/systemlink.json; don't edit.\n"
            "// The System Link games (see systemlink.h). Like the data it comes from,\n"
            "// this table is CC BY-SA 4.0 (data/README.md); the rest of the server is MIT.\n"
            "#include \"systemlink.h\"\n"
            "\n"
            "namespace nia {\n"
            "const char SystemLinkAttribution[] =\n"
            "    %s;\n"
            "\n"
            "// By Title ID: {titleId, name, xbox (an original Xbox game), total, perConsole (0: not known), notes}.\n"
            "const SystemLinkGame SystemLinkGames[] = {\n"
            "%s\n"
            "};\n"
            "const size_t SystemLinkCount = sizeof(SystemLinkGames)/sizeof(SystemLinkGames[0]);\n"
            "\n"
            "const SystemLinkGame* FindSystemLink(DWORD titleId) {\n"
            "    size_t low=0, high=SystemLinkCount;\n"
            "    while(low<high) {\n"
            "        size_t middle=(low+high)/2;\n"
            "        if(SystemLinkGames[middle].titleId<titleId) low=middle+1;\n"
            "        else high=middle;\n"
            "    }\n"
            "    return low<SystemLinkCount && SystemLinkGames[low].titleId==titleId ? &SystemLinkGames[low] : NULL;\n"
            "}\n"
            "}\n" % (literal(attribution + " " + MERGED), "\n".join(lines)))


def main():
    args = sys.argv[1:]
    check = "--check" in args
    args = [a for a in args if a != "--check"]
    path = args[0] if args else SOURCE
    target = args[1] if len(args) > 1 else TARGET
    with open(path, encoding="utf-8") as f:
        text = source(json.load(f), path)
    try:
        with open(target, encoding="utf-8") as f:
            now = f.read()
    except OSError:
        now = None
    if check:
        if now != text:
            sys.exit("%s is out of date: run python tools/make_systemlink.py" % os.path.relpath(target, ROOT))
        return
    if now != text:
        with open(target, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)


if __name__ == "__main__":
    main()
