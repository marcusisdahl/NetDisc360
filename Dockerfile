# NetDisc360 (a NetISO server for Aurora), for Linux, a NAS or anything else
# that runs Docker. The same server as NetDisc360.exe, built from server/ and shared/ for
# Linux. The game launchers it writes to the Xbox are Xbox programs built with
# the Xbox 360 SDK on Windows, so they come from the release's NetISO-Sync.zip
# (or from docker/launcher/, when default.xex and boot.xex are put there).
#
#   docker compose up -d      (see docker-compose.yml and the README)

# The base image by digest, so a rebuild of a tag gives the same image;
# Dependabot (.github/dependabot.yml) proposes the new digest when Debian
# updates it.
FROM debian:bookworm-slim@sha256:99c0b25c46d0214d5a2091092e7908907dcaecd6a962cde0ac700acee222b6f3 AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ ca-certificates curl unzip python3 \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY server/ server/
COPY shared/ shared/
COPY tests/posix_compat.h tests/
# The dashboard page, made into server/web_page.h (it isn't kept in git).
COPY web/ web/
COPY tools/make_web.py tools/
RUN python3 tools/make_web.py
RUN g++ -std=c++17 -O2 -Wall -pthread -include tests/posix_compat.h -o /netdisc \
        server/main.cpp server/discs.cpp server/discmap.cpp server/ftp.cpp server/web.cpp server/config.cpp \
        server/backup.cpp server/update.cpp server/installed.cpp server/aurora.cpp server/auroradb.cpp server/system.cpp \
        shared/library.cpp shared/isvr.cpp

ARG LAUNCHER_URL=https://github.com/marcusisdahl/NetDisc360/releases/latest/download/NetISO-Sync.zip
# Tried when LAUNCHER_URL has none: the dev image asks for the dev launchers
# first (the dev-launchers pre-release, see RELEASING.md), then the latest
# release's.
ARG LAUNCHER_FALLBACK_URL=
# 1: fail the build without launchers (for the published image).
ARG REQUIRE_LAUNCHER=0
COPY docker/launcher/ /launcher/
# unzip exits with 1 for a warning, such as the backslash paths of zips made
# by Windows PowerShell 5.1, after extracting everything; only the files count.
RUN fetch() { [ -n "$1" ] && curl -fsSL "$1" -o /tmp/launcher.zip \
         && { unzip -jo /tmp/launcher.zip '*default.xex' '*boot.xex' -d /launcher || [ $? -eq 1 ]; } \
         && [ -f /launcher/default.xex ] && [ -f /launcher/boot.xex ]; }; \
    if [ -f /launcher/default.xex ] && [ -f /launcher/boot.xex ]; then \
        echo "Using the launchers in docker/launcher/."; \
    elif fetch "$LAUNCHER_URL"; then \
        echo "Using the launchers from $LAUNCHER_URL."; \
    elif fetch "$LAUNCHER_FALLBACK_URL"; then \
        echo "Using the launchers from $LAUNCHER_FALLBACK_URL ($LAUNCHER_URL had none)."; \
    else \
        echo "WARNING: no launchers (none in $LAUNCHER_URL, or it didn't download). The server will only serve games, not add them to Aurora."; \
        rm -f /launcher/default.xex /launcher/boot.xex; \
        [ "$REQUIRE_LAUNCHER" != 1 ]; \
    fi

FROM debian:bookworm-slim@sha256:99c0b25c46d0214d5a2091092e7908907dcaecd6a962cde0ac700acee222b6f3
RUN apt-get update \
 && apt-get install -y --no-install-recommends tini tzdata \
 && rm -rf /var/lib/apt/lists/* \
 && groupadd --system --gid 1000 netdisc \
 && useradd --system --uid 1000 --gid 1000 --no-create-home netdisc \
 && mkdir /config && chown 1000:1000 /config
COPY --from=build /netdisc /usr/local/bin/netdisc
COPY --from=build /launcher/ /usr/local/share/netdisc/
# Aurora's art for NetDisc360's own tile and FastFTP360 (server/main.cpp, LoadAppArt).
COPY art/aurora/netdisc360/*.asset /usr/local/share/netdisc/aurora/netdisc360/
COPY art/aurora/fastftp360/*.asset /usr/local/share/netdisc/aurora/fastftp360/
COPY docker/entrypoint.sh /usr/local/bin/netdisc-entrypoint
RUN chmod 0755 /usr/local/bin/netdisc-entrypoint
USER netdisc
# The dashboard says how to update the image (docker compose pull) instead of
# offering to update itself.
ENV NETDISC_DOCKER=1
# The NetISO protocol and the dashboard. The server reaches the Xbox's FTP
# (port 21) itself.
EXPOSE 4323/tcp 8360/tcp
# /config keeps the dashboard's settings (netdisc.ini).
VOLUME ["/games", "/config"]
HEALTHCHECK --interval=60s --timeout=5s --start-period=30s \
    CMD bash -c 'exec 3<>/dev/tcp/127.0.0.1/${NETDISC_PORT:-4323}' || exit 1
ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/netdisc-entrypoint"]
CMD ["/games"]
