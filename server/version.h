// SPDX-License-Identifier: MIT
#ifndef NETDISC_VERSION_H
#define NETDISC_VERSION_H
// NetDisc 360's version, the same as its release's tag without the v. It is
// raised for each release (see RELEASING.md), and the Docker workflow checks
// that the tag matches. The update check compares it with GitHub's releases.
#define NETDISC_VERSION "1.0.0"
// The same for the exe's version resource (server.rc).
#define NETDISC_VERSION_NUMBERS 1,0,0,0
// Where the releases are.
#define NETDISC_REPO "marcusisdahl/NetDisc-360"
#endif
