// SPDX-License-Identifier: MIT
#ifndef NETDISC_VERSION_H
#define NETDISC_VERSION_H
// NetDisc360's version, the same as its release's tag without the v. It is
// raised for each release (see RELEASING.md), and the Docker workflow checks
// that the tag matches. The update check compares it with GitHub's releases.
// On dev it is the next release's version with -dev (1.1.0-dev), so a dev
// build says what it is, and counts as older than that release but newer
// than the one before (it isn't offered that one as an update).
#define NETDISC_VERSION "1.1.0"
// The same for the exe's version resource (server.rc).
#define NETDISC_VERSION_NUMBERS 1,1,0,0
// The public key the in-app update checks each release's NetDisc360.exe
// with: ECDSA P-256, as 128 hex digits (X then Y), from
// tools/sign_release.py new-key. Empty: no signature is asked for, and only
// GitHub's SHA-256 of the file is checked. Once it is set, every release
// needs NetDisc360.exe.sig beside NetDisc360.exe (tools/sign_release.py sign).
#ifndef NETDISC_UPDATE_KEY  // A test build can give its own.
#define NETDISC_UPDATE_KEY ""
#endif
// Where the releases are.
#define NETDISC_REPO "marcusisdahl/NetDisc360"
#endif
