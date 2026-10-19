#!/bin/bash
# Turns the NETDISC_* environment variables into NetDisc360's options.
# Whatever the container is given as its command (default: /games) comes
# after them, so more folders and options can be passed there too.
#
# The settings are kept in /config/netdisc.ini. The variables give the first
# start's settings; after that, what's saved there (the dashboard's changes)
# wins, except NETDISC_WEB_PASSWORD, which always does.
set -e

options=()
value() { [ -n "$2" ] && options+=("$1" "$2"); return 0; }
switch() { case "$2" in 1|[Tt]rue|[Yy]es|[Oo]n) options+=("$1");; esac; return 0; }

# Several consoles: addresses separated by commas or spaces.
for xbox in ${NETDISC_XBOX//,/ }; do options+=(--xbox "$xbox"); done
value --ftp-user "$NETDISC_FTP_USER"
value --xbox-folder "$NETDISC_XBOX_FOLDER"
value --aurora "$NETDISC_AURORA"
value --port "$NETDISC_PORT"
value --ftp-port "$NETDISC_FTP_PORT"
value --online-wait "$NETDISC_ONLINE_WAIT"
switch --no-push "$NETDISC_NO_PUSH"
switch --no-category "$NETDISC_NO_CATEGORY"
switch --no-scan "$NETDISC_NO_SCAN"
switch -v "$NETDISC_VERBOSE"

# The dashboard, open to the network (it's in a container; publish the port
# or use host networking).
value --web-port "${NETDISC_WEB_PORT:-8360}"
value --web-bind "${NETDISC_WEB_BIND:-0.0.0.0}"

config=${NETDISC_CONFIG:-/config}
if [ -n "$config" ] && [ "$config" != none ]; then
    if [ ! -w "$config" ]; then
        echo "netdisc: $config isn't writable by user $(id -u):$(id -g), so dashboard changes won't be kept." >&2
        echo "netdisc: give it to that user, e.g.  sudo chown $(id -u):$(id -g) <the folder mounted at $config>" >&2
    fi
    options+=(--config "$config/netdisc.ini")
fi

# The passwords stay in the environment (NETDISC_FTP_PASSWORD and
# NETDISC_WEB_PASSWORD), where the server reads them, so they aren't on the
# command line. They can also come from files, such as Docker secrets.
if [ -n "$NETDISC_FTP_PASSWORD_FILE" ]; then
    NETDISC_FTP_PASSWORD=$(cat "$NETDISC_FTP_PASSWORD_FILE")
    export NETDISC_FTP_PASSWORD
fi
if [ -n "$NETDISC_WEB_PASSWORD_FILE" ]; then
    NETDISC_WEB_PASSWORD=$(cat "$NETDISC_WEB_PASSWORD_FILE")
    export NETDISC_WEB_PASSWORD
fi

launchers=/usr/local/share/netdisc
[ -f "$launchers/default.xex" ] && options+=(--launcher "$launchers/default.xex")
[ -f "$launchers/boot.xex" ] && options+=(--boot "$launchers/boot.xex")

exec /usr/local/bin/netdisc "${options[@]}" "$@"
