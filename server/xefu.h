// SPDX-License-Identifier: MIT
#pragma once
// Which version of the console's original Xbox emulator an original Xbox
// game starts in. The emulator, xefu, is in HddX:\Compatibility in several
// versions: Microsoft's xefu.xex, xefu2, xefu3, xefu5, xefu1_1, xefu6, xefu7
// and xefu7b, and in the Hacked Xefu Pack also the ones from the Xbox One
// (xefu2019, xefu2021a, b and c) and config_loader_xefu7 (xefu7 that loads
// Configs\<Title ID>.bin, a game's fixes). The console's xbox.xex picks one
// from its own list of supported games and starts xefu.xex for the rest,
// though many of those only play on another version (Crazy Taxi 3 freezes on
// xefu.xex). NetDisc360's list says which, from the ConsoleMods
// compatibility list; Sync writes the pick into the game folder's
// netiso.ini as xefu=, and the NetIsoTiles plugin starts that version when
// the console starts the emulator for the game (see plugin/main.cpp).
// Nothing in HddX:\Compatibility is changed, as Xefu Spoofer does for every
// game at once.
#include "../shared/platform.h"
#include <string>

namespace nia {
struct KnownXefu { DWORD titleId; const char* xefu; const char* name; const char* why; };
// NetDisc360's pick for an original Xbox game, or NULL to leave it to the
// console. why is what the list says, for the dashboard.
const KnownXefu* XefuFor(DWORD titleId);
// The whole list, sorted by Title ID, for the tests.
const KnownXefu* KnownXefus(int& count);
// An emulator's name as discs.ini and netiso.ini take it: xefu or
// config_loader_xefu, then up to 24 letters, digits and _ (not a xefutitle).
bool XefuName(const std::string& name);
}
