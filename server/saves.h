// SPDX-License-Identifier: MIT
#pragma once
// Backups of game saves: after a console played a game, NetDisc360 copies
// that game's saves from every profile on the console's hard drive over
// Aurora's FTP server into one zip in the Saves folder next to the settings
// file, and can put any of them back.
//
//   Saves\4D5307E6_2026-10-26_23-15-06.zip
//     save.ini                          title_id, game, time and profile_<XUID>=<gamertag> lines
//     E0000123456789AB/<save file>      each file of /Hdd1/Content/<XUID>/<Title ID>/00000001
//
// A backup the same as the newest one of that game isn't kept twice.
#include "ftp.h"
#include <set>
#include <string>
#include <vector>

namespace saves {
const char ContentFolder[] = "/Hdd1/Content";
struct Backup { std::string name; DWORD titleId; long long time, size; };
struct Result { std::string name; int profiles, files; long long bytes; bool same; };

// Backs up the saves of titleId (game names it in save.ini) into folder, made
// if need be, then deletes the oldest of that game's backups beyond keep.
// With no saves, nothing is made and result.files is 0.
bool Take(nia::FtpClient& ftp, const std::string& folder, DWORD titleId, const std::string& game, int keep,
          Result& result, std::string& error);
// The Title IDs that have saves on the console, in any profile.
bool Titles(nia::FtpClient& ftp, std::set<DWORD>& titles, std::string& error);
// Puts a backup's saves back on the console, file by file (profiles that
// aren't on the console any more are skipped); files counts those written.
bool Put(nia::FtpClient& ftp, const std::string& data, int& files, std::string& error);
// The backups in folder, newest first.
std::vector<Backup> List(const std::string& folder);
// The game and profiles a backup's save.ini names.
bool Describe(const std::string& data, std::string& game, std::vector<std::string>& profiles);
// Whether name can be a backup's file name (as Take names them).
bool Named(const std::string& name);
// The gamertag in a profile's package (its STFS display name), or "".
std::string Gamertag(const std::vector<unsigned char>& package);
}
