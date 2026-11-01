// SPDX-License-Identifier: MIT
#pragma once
// The games that can be played over System Link (consoles cabled to one
// network, each with its own screen), with how many can play on one console
// and in all, for Xbox 360 and original Xbox games: the dashboard's System
// Link list and the LAN party page's game picker. Built in from
// data/systemlink.json by tools/make_systemlink.py, whose output
// systemlink.cpp is. The numbers come from Wikipedia's lists of System Link
// games and are CC BY-SA 4.0, as is that table (see data/README.md).
#include "../shared/platform.h"
#include <stddef.h>

namespace nia {
struct SystemLinkGame {
    DWORD titleId;
    const char* name;       // In UTF-8.
    bool xbox;              // An original Xbox game; else an Xbox 360 one.
    int total, perConsole;  // Players in all, and on one console; 0 where it isn't known.
    const char* notes;      // What the sources add (modes, limits), in English.
};
// By Title ID, each once.
extern const SystemLinkGame SystemLinkGames[];
extern const size_t SystemLinkCount;
// Where the list comes from, its licence and what was changed, for the dashboard.
extern const char SystemLinkAttribution[];
// The game with titleId, or NULL if it isn't listed.
const SystemLinkGame* FindSystemLink(DWORD titleId);
}
