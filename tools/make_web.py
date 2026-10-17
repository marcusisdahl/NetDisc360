"""Builds server/web_page.h, the dashboard page that NetDisc 360 serves.

Puts web/icons.svg (the Lucide icons the page uses, as an SVG sprite) into
web/index.html where it says <!--icons-->, gzips it and writes it as a byte
array. Run it after changing the page:

    python tools/make_web.py              rebuilds server/web_page.h
    python tools/make_web.py --check      fails if server/web_page.h is out of date
    python tools/make_web.py --icons <lucide-static>/icons
                                          also remakes web/icons.svg from the icons
                                          the page names: ic('name') in its script,
                                          data-icon="name", href="#i-name", or any
                                          other quoted Lucide name in the script

The icons are from Lucide (npm package lucide-static, ISC license; see
third_party/lucide/LICENSE).
"""
import gzip
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAGE = os.path.join(ROOT, "web", "index.html")
ICONS = os.path.join(ROOT, "web", "icons.svg")
HEADER = os.path.join(ROOT, "server", "web_page.h")


def icon_names(page):
    names = set(re.findall(r"""ic\(\s*['"]([a-z0-9-]+)['"]""", page))
    names |= set(re.findall(r'data-icon="([a-z0-9-]+)"', page))
    names |= set(re.findall(r'#i-([a-z0-9-]+)', page))
    return sorted(names)


def make_icons(page, lucide):
    # Names picked at run time (ic(on ? 'play' : 'monitor')) are quoted in the script too.
    quoted = {n for n in re.findall(r"'([a-z0-9-]+)'", page) if os.path.exists(os.path.join(lucide, n + ".svg"))}
    symbols = []
    for name in sorted(set(icon_names(page)) | quoted):
        with open(os.path.join(lucide, name + ".svg"), encoding="utf-8") as f:
            svg = f.read()
        body = svg[svg.index(">", svg.index("<svg")) + 1:svg.rindex("</svg>")]
        body = re.sub(r"\s*\n\s*", "", body.strip())
        symbols.append('<symbol id="i-%s" viewBox="0 0 24 24">%s</symbol>' % (name, body))
    sprite = ('<!-- Lucide icons (ISC license, third_party/lucide/LICENSE) -->\n'
              '<svg xmlns="http://www.w3.org/2000/svg" style="display:none">\n' + "\n".join(symbols) + "\n</svg>\n")
    with open(ICONS, "w", encoding="utf-8", newline="\n") as f:
        f.write(sprite)


def build():
    with open(PAGE, encoding="utf-8") as f:
        page = f.read()
    with open(ICONS, encoding="utf-8") as f:
        icons = f.read()
    missing = [n for n in icon_names(page) if 'id="i-%s"' % n not in icons]
    if missing:
        sys.exit("web/icons.svg lacks %s; run with --icons <lucide-static>/icons" % ", ".join(missing))
    return page.replace("<!--icons-->", icons.strip()).encode("utf-8")


def header(html):
    data = gzip.compress(html, compresslevel=9, mtime=0)
    lines = []
    for i in range(0, len(data), 24):
        lines.append("    " + ",".join("0x%02x" % b for b in data[i:i + 24]) + ",")
    return ("// SPDX-License-Identifier: MIT\n"
            "// Made by tools/make_web.py from web/index.html and web/icons.svg; don't edit.\n"
            "// The dashboard page, gzipped (%d bytes, %d unzipped).\n"
            "#pragma once\n"
            "static const unsigned char WebPage[] = {\n%s\n};\n" % (len(data), len(html), "\n".join(lines)))


def current(html):
    """Whether server/web_page.h holds this page. Compares the page, not the
    gzip bytes, which differ between zlib builds."""
    try:
        with open(HEADER, encoding="utf-8") as f:
            data = bytes(int(b, 16) for b in re.findall(r"0x([0-9a-f]{2})", f.read()))
        return gzip.decompress(data) == html
    except (OSError, EOFError):
        return False


def main():
    args = sys.argv[1:]
    if args[:1] == ["--icons"]:
        with open(PAGE, encoding="utf-8") as f:
            make_icons(f.read(), args[1])
        args = args[2:]
    html = build()
    if args[:1] == ["--check"]:
        if not current(html):
            sys.exit("server/web_page.h is out of date: run python tools/make_web.py")
        return
    if current(html):
        return  # Left alone, so a build doesn't touch the file (or the git tree).
    with open(HEADER, "w", encoding="utf-8", newline="\n") as f:
        f.write(header(html))


if __name__ == "__main__":
    main()
