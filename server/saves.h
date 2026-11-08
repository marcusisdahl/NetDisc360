// SPDX-License-Identifier: MIT
#pragma once
// Backups of game saves: after a console played a game, NetDisc360 copies
// that game's saves from every profile on the console's hard drive over
// Aurora's FTP server into one zip in the Saves folder next to the settings
// file, and can put any of them back.
//
//   Saves\4D5307E6_2026-10-26_23-15-06.zip
//     save.ini                          title_id, game, time, profile_<XUID>=<gamertag>,
//                                       console, computer and listing lines
//     E0000123456789AB/<save file>      each file of /Hdd1/Content/<XUID>/<Title ID>/00000001
//
// A backup the same as the newest one of that game from the same console
// isn't kept twice, and each console (on each computer) keeps its own newest
// few of each game.
// Saves go by the folders they're in (the Title ID and the XUID), never by a
// save file's own header: SSX's says Title ID 0.
#include "ftp.h"
#include <set>
#include <map>
#include <string>
#include <vector>

namespace saves {
const char ContentFolder[] = "/Hdd1/Content";
struct Backup { std::string name; DWORD titleId; long long time, size; };
// listing: the saves' digest as Listings gives it ("" without saves).
struct Result { std::string name, listing; int profiles, files; long long bytes; bool same; };

// Backs up the saves of titleId (game names it in save.ini) into folder, made
// if need be, then deletes the oldest of that game's backups beyond keep,
// counted for each console apart, but none that spare names. Saves over 256
// MB together are refused by their listed sizes, before any download. With
// no saves, nothing is made and result.files is 0. known: gamertags by
// profile (XUID, 16 upper-case hex digits) known already, such as the
// signed-in ones, whose packages the console won't hand out over FTP.
// extra: more save.ini lines ("key=value\r\n" each), such as where the saves
// came from (console= and computer=, which tell one console's backups from
// another's).
bool Take(nia::FtpClient& ftp, const std::string& folder, DWORD titleId, const std::string& game, int keep,
          Result& result, std::string& error,
          const std::map<std::string,std::string>& known=std::map<std::string,std::string>(),
          const std::string& extra=std::string(), const std::set<std::string>& spare=std::set<std::string>());
// The Title IDs that have saves on the console, in any profile.
bool Titles(nia::FtpClient& ftp, std::set<DWORD>& titles, std::string& error);
// What the saves of each of titles that has any look like in the console's
// FTP listings (each file's profile, name, size and date), as a short
// digest: it changes when a game writes its saves, and is read without
// downloading them. Take keeps the same in save.ini (listing=).
bool Listings(nia::FtpClient& ftp, const std::set<DWORD>& titles, std::map<DWORD,std::string>& out, std::string& error);
// Puts the saves in files (a backup's, unpacked) on the console; only one
// profile's (its XUID) unless only is "". With exact, each of those
// profiles' files there that files doesn't have are deleted after, so its
// saves are just those (a game with a file per slot keeps no newer slot
// beside them). put, if given, gets the profiles whose saves were put.
bool PutFiles(nia::FtpClient& ftp, const std::map<std::string,std::string>& files, const std::string& only, int& written, std::string& error,
              bool exact=false, std::vector<std::string>* put=NULL);
// The profiles on the console (XUIDs in upper case).
bool ProfilesOn(nia::FtpClient& ftp, std::vector<std::string>& xuids, std::string& error);
// A game's saves on the console now, as Take keeps them ("<XUID>/<file>").
bool Collect(nia::FtpClient& ftp, DWORD titleId, std::map<std::string,std::string>& files, std::string& error);
// One profile's saves among files (a backup's or Collect's) as one string,
// equal when the saves are; "" when it has none.
std::string ProfileSaves(const std::map<std::string,std::string>& files, const std::string& xuid);
// A backup in folder, unpacked.
bool Read(const std::string& folder, const std::string& name, std::map<std::string,std::string>& files, std::string& error);
// A backup's save.ini, read without its saves.
bool IniOf(const std::string& folder, const std::string& name, std::string& ini);
// A save.ini value.
std::string IniValue(const std::string& ini, const std::string& key);
// A copy of backup name made now (its save.ini without the line of key drop,
// plus extra lines, which replace those of the same keys): made is its name.
// With only (an XUID), just that profile's saves are in the copy.
bool Copy(const std::string& folder, const std::string& name, const std::string& drop, const std::string& extra, const std::string& only,
          std::string& made, std::string& error);
// The backups in folder, newest first.
std::vector<Backup> List(const std::string& folder);
// The game and profiles backup name in folder names, without reading its saves.
bool Describe(const std::string& folder, const std::string& name, std::string& game, std::vector<std::string>& profiles);
// Whether name can be a backup's file name (as Take names them).
bool Named(const std::string& name);
// The gamertag in a profile's package (its STFS display name), or "".
std::string Gamertag(const std::vector<unsigned char>& package);
}
