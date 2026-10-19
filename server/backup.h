// SPDX-License-Identifier: MIT
#pragma once
// Backups of NetDisc 360's settings: netdisc.ini and the disc map
// (discs.ini), never the games. Each is a zip in the Backups folder next to
// the settings file, with backup.ini saying what made it:
//
//   NetDisc360_2026-10-18_23-15-06_manual.zip
//     backup.ini    app, version, platform, kind and time (Unix seconds)
//     netdisc.ini
//     discs.ini     when there is one
//
// The files are a few KB, so the zips are stored, not compressed, and only
// stored zips are read back: NetDisc 360's own, as they were downloaded. A
// netdisc.ini by itself can be restored too.
#include <map>
#include <string>
#include <vector>

namespace backup {
// kind: manual (made on the dashboard), scheduled, update (before an update
// installs) or restore (before a backup is restored).
struct Info { std::string name, kind, version, platform; long long time, size; };

// A stored zip of files (name: content), and back.
std::string Zip(const std::map<std::string,std::string>& files, long long time);
bool Unzip(const std::string& data, std::map<std::string,std::string>& files, std::string& error);

// Makes a backup of files in folder (made if need be); name gets its file name.
bool Make(const std::string& folder, const std::string& kind, const std::map<std::string,std::string>& files,
          std::string& name, std::string& error);
// The backups in folder, newest first.
std::vector<Info> List(const std::string& folder);
// The files of a backup (or of a netdisc.ini by itself), after checking it
// holds NetDisc 360's settings; info gets what its backup.ini says.
bool Open(const std::string& data, std::map<std::string,std::string>& files, Info& info, std::string& error);
// Deletes the oldest backups beyond keep, but never manual ones nor the one
// named spare (being restored); returns how many.
int Prune(const std::string& folder, int keep, const std::string& spare = std::string());
// Whether name can be a backup's file name: letters, digits, - _ and . only, ending in .zip.
bool Named(const std::string& name);
// The platform backups made here name: windows or linux.
const char* Platform();

bool FromBase64(const std::string& text, std::string& out);
}
