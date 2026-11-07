// SPDX-License-Identifier: MIT
#pragma once
// The helper: one copy of the full launcher (launcher/main.cpp, built without
// execution info, like Aurora) for all the game folders, which mounts a
// game's disc from the server and starts it. Each game folder has only the
// tile launcher (launcher/tile.cpp) as its default.xex, which carries the
// game's Title ID for Aurora and hands the start to the helper, so a game
// folder holds a few kilobytes instead of two copies of the full launcher.
//
// Where NetDisc360 keeps its files on a console: the game folders in
// Hdd1:\NetDisc360\Games (the server's Xbox folder, which Aurora scans on its
// own), and NetDisc360's own files (the helper, wake.ini, logs) in AppFolder,
// Hdd1:\Apps\NetDisc360, with the other apps. Both were in Hdd1:\NetISO before
// (NetDisc360's own files in its NetDisc360 folder); the server moves them.
//
// The helper is HelperXex in AppFolder (Hdd1:\Apps\NetDisc360\boot.xex), or,
// when the game folders aren't on the hard drive, HelperFolder\HelperXex in
// the folder they are in, as it was in Hdd1:\NetISO. Aurora lists nothing for
// either, since they have no default.xex. A game folder made before then has
// its own copy as boot.xex, which the tile launcher starts when neither
// shared one is there.
#include "platform.h"

namespace nia {
const char HelperFolder[] = "NetDisc360", HelperXex[] = "boot.xex";
const char AppFolder[] = "Hdd1:\\Apps\\NetDisc360", AppDevice[] = "\\Device\\Harddisk0\\Partition1\\Apps\\NetDisc360";
const char GamesFolder[] = "Hdd1:\\NetDisc360\\Games";
const char BootData[] = "NetISO boot";
enum { BootFolderMax = 260 };
// The launch data the tile launcher hands the helper: BootData, when the tile
// was pressed (GetTickCount), so the helper's timer counts from there, and
// the game folder as a device path (\Device\Harddisk0\Partition1\NetDisc360\Games\Halo 3).
// A game folder's own boot.xex gets only the first two, as before; it reads
// the folder it is in.
struct BootLaunch { char text[sizeof(BootData)]; DWORD started; char folder[BootFolderMax]; };
enum { BootLaunchBefore = sizeof(BootData) + 4 };  // With no folder; sizeof(BootData) alone came before the timer.
}
