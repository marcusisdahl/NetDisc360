#!/bin/bash
# Turns the NETDISC_* environment variables into NetDisc 360's options.
# Whatever the container is given as its command (default: /games) comes
# after them, so more folders and options can be passed there too.
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

# The FTP password stays in the environment (NETDISC_FTP_PASSWORD), where the
# server reads it, so it isn't on the command line. It can also come from a
# file, such as a Docker secret.
if [ -n "$NETDISC_FTP_PASSWORD_FILE" ]; then
    NETDISC_FTP_PASSWORD=$(cat "$NETDISC_FTP_PASSWORD_FILE")
    export NETDISC_FTP_PASSWORD
fi

launchers=/usr/local/share/netdisc
[ -f "$launchers/default.xex" ] && options+=(--launcher "$launchers/default.xex")
[ -f "$launchers/boot.xex" ] && options+=(--boot "$launchers/boot.xex")

exec /usr/local/bin/netdisc "${options[@]}" "$@"
