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
    // Names of the folders directly in root.
    virtual int ListFolders(const char* root, char names[][FolderNameMax + 1], int capacity) = 0;
    // Whole file into data; false if missing or larger than capacity.
    virtual bool ReadFile(const char* path, unsigned char* data, DWORD capacity, DWORD& size) = 0;
    virtual bool WriteFile(const char* path, const void* data, DWORD size) = 0;
    virtual bool MakeFolder(const char* path) = 0;
    // Deletes default.xex, netiso.ini and netiso.log in folder, then the folder
    // if it is empty. Anything else in it is left alone.
    virtual bool RemoveGame(const char* folder) = 0;
};

struct SyncResult { int added, kept, removed, skipped; };

// Value of key in an ini text ("key=value" lines), or false.
bool IniValue(const char* text, const char* key, char* value, size_t capacity);
// Offset of the execution info (0x40006) in a XEX header, or 0.
DWORD ExecutionInfoOffset(const unsigned char* xex, DWORD size);
// FATX-safe folder name for an ISO, unique among used (case-insensitive).
void FolderName(const char* isoName, char used[][FolderNameMax + 1], int usedCount, char* out);
// Reads the execution info of default.xex on the ISO mounted on client.
bool GameInfo(IsvrClient& client, unsigned char* info, char* error, size_t capacity);

typedef void (*Note)(const char* text);
// launcher is a launcher XEX (any copy; its execution info is overwritten).
bool Sync(IsvrClient& client, Storage& storage, const char* root, const unsigned char* launcher, DWORD launcherSize,
          Note note, SyncResult& result);
}
