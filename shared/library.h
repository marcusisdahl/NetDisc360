// SPDX-License-Identifier: MIT
#pragma once
// NetISO Sync: makes one Aurora launcher folder per ISO on the server and
// removes the folders of ISOs that are gone. Shared by the Xbox build and the
// host tests; files are reached only through Storage.
#include "isvr.h"

namespace nia {
enum { ExecutionInfoSize = 24, FolderNameMax = 42, MaxFolders = 1024, PathMax = 320, IniMax = 4096 };

#ifdef _XBOX
const char PathSeparator = '\\';
#else
const char PathSeparator = '/';
#endif

class Storage {
public:
    virtual ~Storage() {}
    // Names of the folders directly in root; -1 if root couldn't be listed.
    virtual int ListFolders(const char* root, char names[][FolderNameMax + 1], int capacity) = 0;
    // Whole file into data; false if missing or larger than capacity.
    virtual bool ReadFile(const char* path, unsigned char* data, DWORD capacity, DWORD& size) = 0;
    // Whether the last ReadFile that failed did because the file isn't there,
    // rather than because reading it went wrong.
    virtual bool Missing() { return true; }
    virtual bool WriteFile(const char* path, const void* data, DWORD size) = 0;
    virtual bool MakeFolder(const char* path) = 0;
    // Deletes default.xex, boot.xex, netiso.ini, install.ini and netiso.log in
    // folder, then the folder if it is empty. Anything else in it is left alone.
    virtual bool RemoveGame(const char* folder) = 0;
    // Deletes a file: 1 if it was deleted, 0 if it wasn't there, -1 if it
    // couldn't be.
    virtual int RemoveFile(const char* path) = 0;
    // Error code of the last call that failed, for the log.
    virtual unsigned long LastError() = 0;
};

// updated counts kept folders whose launcher was rewritten with this version;
// failed counts files of kept folders that couldn't be written or removed.
struct SyncResult { int added, kept, updated, removed, skipped, held, failed; };

// Value of key in an ini text ("key=value" lines), or false.
bool IniValue(const char* text, const char* key, char* value, size_t capacity);
// Offset of the execution info (0x40006) in a XEX header, or 0.
DWORD ExecutionInfoOffset(const unsigned char* xex, DWORD size);
// Recomputes the header digest after the header was edited; false if the
// header is too short to have one.
bool FixHeaderDigest(unsigned char* xex, DWORD size);
// FATX-safe folder name for an ISO, unique among used (case-insensitive).
void FolderName(const char* isoName, char used[][FolderNameMax + 1], int usedCount, char* out);
// Reads the execution info of default.xex on the ISO mounted on client.
bool GameInfo(IsvrClient& client, unsigned char* info, char* error, size_t capacity);

typedef void (*Note)(const char* text);

// What the game on an ISO needs installed from another of the server's discs,
// its install disc (GTA V's play disc needs its install disc's packages).
// Sync writes it to the game folder's install.ini, which the launcher checks
// before it starts the game.
struct InstallNeed {
    bool known;                     // false: the folder's install.ini is left as it is.
    char iso[IsvrNameBytes];        // The install disc as the server lists it; empty: none needed.
    char packages[IniMax / 2];      // "package_N=<path under Content>|<bytes>" lines, each ending in \r\n.
};
typedef void (*FindInstall)(const char* iso, InstallNeed& need);

// What a disc is in its game's set, from the server's disc map (see
// server/discmap.h). Sync writes it into the game folder's netiso.ini as
// disc_role and disc_label, where the NetIsoTiles plugin reads it for its
// disc swap notices.
struct DiscRoleInfo {
    bool known;      // false: netiso.ini is left as it is.
    char role[16];   // install, play, content, next or own; empty for a game on one disc.
    char label[64];  // The disc's own name ("Undead Nightmare"), or empty.
};
typedef void (*FindRole)(const char* iso, DiscRoleInfo& info);
// launcher is the tile launcher XEX (any copy; its execution info is
// overwritten), each game folder's default.xex. boot, if not NULL, is the
// helper XEX without execution info that they share, kept in root as
// HelperFolder\HelperXex (see shared/helper.h); the boot.xex a game folder
// got before then goes.
// With removeGone false, folders whose ISO is gone are left and counted in held.
// findInstall, if not NULL, is asked about each game folder's install.ini,
// and findRole about the disc_role and disc_label in its netiso.ini.
bool Sync(IsvrClient& client, Storage& storage, const char* root, const unsigned char* launcher, DWORD launcherSize,
          const unsigned char* boot, DWORD bootSize, Note note, SyncResult& result, bool removeGone=true,
          FindInstall findInstall=NULL, FindRole findRole=NULL);
}
