// SPDX-License-Identifier: MIT
#pragma once
// The helper: one copy of the full launcher (launcher/main.cpp, built without
// execution info, like Aurora) for all the game folders, which mounts a
// game's disc from the server and starts it. Each game folder has only the
// tile launcher (launcher/tile.cpp) as its default.xex, which carries the
// game's Title ID for Aurora and hands the start to the helper, so a game
// folder holds a few kilobytes instead of two copies of the full launcher.
//
// The helper is HelperFolder\HelperXex in the folder the game folders are in
// (Hdd1:\NetISO\NetDisc360\boot.xex). Aurora lists nothing for that folder,
// since it has no default.xex. A game folder made before has its own copy as
// boot.xex, which the tile launcher starts when the shared one isn't there.
#include "platform.h"

namespace nia {
const char HelperFolder[] = "NetDisc360", HelperXex[] = "boot.xex";
const char BootData[] = "NetISO boot";
enum { BootFolderMax = 260 };
// The launch data the tile launcher hands the helper: BootData, when the tile
// was pressed (GetTickCount), so the helper's timer counts from there, and
// the game folder as a device path (\Device\Harddisk0\Partition1\NetISO\Halo 3).
// A game folder's own boot.xex gets only the first two, as before; it reads
// the folder it is in.
struct BootLaunch { char text[sizeof(BootData)]; DWORD started; char folder[BootFolderMax]; };
enum { BootLaunchBefore = sizeof(BootData) + 4 };  // With no folder; sizeof(BootData) alone came before the timer.
}
