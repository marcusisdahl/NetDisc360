# NetISO server for Aurora, for Linux, a NAS or anything else that runs Docker.
# The same server as netiso-server.exe, built from server/ and shared/ for
# Linux. The game launchers it writes to the Xbox are Xbox programs built with
# the Xbox 360 SDK on Windows, so they come from the release's NetISO-Sync.zip
# (or from docker/launcher/, when default.xex and boot.xex are put there).
#
#   docker compose up -d      (see docker-compose.yml and the README)

FROM debian:bookworm-slim AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ ca-certificates curl unzip \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY server/ server/
COPY shared/ shared/
COPY tests/posix_compat.h tests/
RUN g++ -std=c++17 -O2 -Wall -pthread -include tests/posix_compat.h -o /netiso-server \
        server/main.cpp server/discs.cpp server/ftp.cpp shared/library.cpp shared/isvr.cpp

ARG LAUNCHER_URL=https://github.com/marcusisdahl/NetISO-Aurora/releases/latest/download/NetISO-Sync.zip
# 1: fail the build without launchers (for the published image).
ARG REQUIRE_LAUNCHER=0
COPY docker/launcher/ /launcher/
RUN if [ -f /launcher/default.xex ] && [ -f /launcher/boot.xex ]; then \
        echo "Using the launchers in docker/launcher/."; \
    elif curl -fsSL "$LAUNCHER_URL" -o /tmp/launcher.zip \
         && unzip -jo /tmp/launcher.zip '*default.xex' '*boot.xex' -d /launcher; then \
        echo "Using the launchers from $LAUNCHER_URL."; \
    else \
        echo "WARNING: no launchers ($LAUNCHER_URL didn't download). The server will only serve games, not add them to Aurora."; \
        rm -f /launcher/default.xex /launcher/boot.xex; \
        [ "$REQUIRE_LAUNCHER" != 1 ]; \
    fi

FROM debian:bookworm-slim
RUN apt-get update \
 && apt-get install -y --no-install-recommends tini tzdata \
 && rm -rf /var/lib/apt/lists/* \
 && groupadd --system --gid 1000 netiso \
 && useradd --system --uid 1000 --gid 1000 --no-create-home netiso
COPY --from=build /netiso-server /usr/local/bin/netiso-server
COPY --from=build /launcher/ /usr/local/share/netiso/
COPY docker/entrypoint.sh /usr/local/bin/netiso-entrypoint
RUN chmod 0755 /usr/local/bin/netiso-entrypoint
USER netiso
# The NetISO protocol. The server reaches the Xbox's FTP (port 21) itself.
EXPOSE 4323/tcp
VOLUME ["/games"]
HEALTHCHECK --interval=60s --timeout=5s --start-period=30s \
    CMD bash -c 'exec 3<>/dev/tcp/127.0.0.1/${NETISO_PORT:-4323}' || exit 1
ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/netiso-entrypoint"]
CMD ["/games"]
