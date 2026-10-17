#!/bin/bash
# Turns the NETISO_* environment variables into netiso-server's options.
# Whatever the container is given as its command (default: /games) comes
# after them, so more folders and options can be passed there too.
set -e

options=()
value() { [ -n "$2" ] && options+=("$1" "$2"); return 0; }
switch() { case "$2" in 1|[Tt]rue|[Yy]es|[Oo]n) options+=("$1");; esac; return 0; }

value --xbox "$NETISO_XBOX"
value --ftp-user "$NETISO_FTP_USER"
value --xbox-folder "$NETISO_XBOX_FOLDER"
value --aurora "$NETISO_AURORA"
value --port "$NETISO_PORT"
value --ftp-port "$NETISO_FTP_PORT"
value --online-wait "$NETISO_ONLINE_WAIT"
switch --no-push "$NETISO_NO_PUSH"
switch --no-category "$NETISO_NO_CATEGORY"
switch --no-scan "$NETISO_NO_SCAN"
switch -v "$NETISO_VERBOSE"

# The FTP password stays in the environment (NETISO_FTP_PASSWORD), where the
# server reads it, so it isn't on the command line. It can also come from a
# file, such as a Docker secret.
if [ -n "$NETISO_FTP_PASSWORD_FILE" ]; then
    NETISO_FTP_PASSWORD=$(cat "$NETISO_FTP_PASSWORD_FILE")
    export NETISO_FTP_PASSWORD
fi

launchers=/usr/local/share/netiso
[ -f "$launchers/default.xex" ] && options+=(--launcher "$launchers/default.xex")
[ -f "$launchers/boot.xex" ] && options+=(--boot "$launchers/boot.xex")

exec /usr/local/bin/netiso-server "${options[@]}" "$@"
