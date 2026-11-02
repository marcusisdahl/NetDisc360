// SPDX-License-Identifier: MIT
#pragma once
// The LAN party's guest profiles: an offline Xbox profile for each guest on
// the party's list, the same one (one XUID) on every party console, so a
// guest can sit down at any of them. One console makes it (the helper's
// profile job, ProfileJob in shared/helper.h); the console signs it with its
// own keys, so the others take a copy of its package (as from a memory unit),
// which the server puts on them over FTP.
//
// When the party is over, each console's copy of a guest's content folder
// (Hdd1:\Content\<XUID>: the profile and the guest's game saves) is kept on
// this computer, as a zip in Backups/Profiles/<XUID>, and then removed from
// the console. When the name is on a later party's list, the kept copy with
// the most achievements goes back on the consoles instead of a new profile,
// so the guest has their achievements and saves again.
//
// Only profiles the server made itself are ever removed: the records say
// which XUIDs those are, and a console's own profiles are never in them.
// Logic only: main.cpp does the FTP and keeps the records in the settings
// file.
#include "config.h"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace nia {
enum { GamertagLongest = 15, GuestProfilesMost = 256 };

struct GuestProfile {
    std::string name;             // The guest's name, as on the list.
    std::string gamertag;         // Its gamertag on the consoles.
    std::string xuid;             // 16 hex digits (E000...), upper case; "" until made.
    long long made;               // When it was made or put back (Unix seconds).
    std::string madeOn;           // The console that made it.
    std::vector<std::string> on;  // The consoles it's on now, as far as the server knows.
    // Made on a console by hand (the guest signed in or joined with it), not
    // by NetDisc360: copied to the others all the same, but never removed
    // from madeOn.
    bool adopted;
    // Progress follows the player: the console they last signed in or
    // played on (its copy is the newest), and the consoles whose copy is
    // older than that one's, brought up to date when they can be.
    std::string playedOn;
    std::vector<std::string> stale;
    GuestProfile() : made(0), adopted(false) {}
};

// The gamertag for a guest's name: ASCII letters, digits and spaces (å, ø, é
// and the like as their plain letters, æ as ae, other characters as
// spaces), at most GamertagLongest characters: a longer name keeps its first
// word and the initials of the rest that fit ("Kristoffer Hansen":
// "Kristoffer H"). "Guest" when nothing is left. taken: gamertags given
// already (compared ignoring case); a clash gets a number at the end.
std::string GuestGamertag(const std::string& name, const std::vector<std::string>& taken);
// Whether text is an offline profile's XUID (16 hex digits, starting with
// E); xuid gets it in upper case.
bool OfflineXuid(const std::string& text, std::string& xuid);

// The helper's job for gamertags (guest1= the first), and what it wrote back.
// pictures: each gets a gamer picture, ProfilePictureFile(N, 64) and
// (N, 32), which go beside the job (picture.h makes them). extra: more
// lines for the job, as they are.
std::string ProfilePictureFile(int n, int size);
std::string ProfileJobText(const std::string& job, const std::vector<std::string>& gamertags, bool pictures = false, const std::string& extra = "");
struct ProfileJobDone {  // By N of guestN.
    std::string job;
    std::map<int,std::string> xuids, errors, pictureErrors;
    std::set<int> pictures;
};
bool ReadProfileDone(const std::string& text, ProfileJobDone& done);

// Where a profile is on a console, as Aurora's FTP server has it.
std::string ContentFolderFtp(const std::string& xuid);   // /Hdd1/Content/<XUID>
std::string ProfilePackageFtp(const std::string& xuid);  // .../FFFE07D1/00010000/<XUID>

// What a profile's package (STFS, as the console wrote it) says: its
// display name (the gamertag, or the XUID for a profile xam has just made:
// the gamertag itself is in the encrypted Account), and the gamerscore and achievements
// its dashboard record (FFFE07D1.gpd) counts, -1 if they can't be read.
struct ProfileStats { std::string gamertag; int gamerscore, achievements; ProfileStats() : gamerscore(-1), achievements(-1) {} };
bool ReadProfileStats(const std::string& package, ProfileStats& stats);

// A kept copy of a guest's content folder from one console: the zip (its
// files under Content/, as in Hdd1:\Content\<XUID>), and beside it a small
// .ini with what it is.
struct ProfileCopy {
    std::string file;             // The zip's file name.
    std::string name, gamertag, xuid, console;
    long long time, size;
    int gamerscore, achievements;
    ProfileCopy() : time(0), size(0), gamerscore(-1), achievements(-1) {}
};
std::string ProfileCopyName(long long time, const std::string& console);  // 2026-11-01_23-10-05_10.10.1.43.zip
std::string ProfileCopyIni(const ProfileCopy& copy);
bool ReadProfileCopyIni(const std::string& text, ProfileCopy& copy);
// The copies kept in folder (Backups/Profiles/<XUID>), newest first.
std::vector<ProfileCopy> ProfileCopies(const std::string& folder);
// The one to put back: the most gamerscore, then the most achievements, then
// the newest. NULL if there are none.
const ProfileCopy* BestProfileCopy(const std::vector<ProfileCopy>& copies);

// The records in the settings file (guest_profiles=count, guest_profileN=
// name|gamertag|xuid|made|madeOn|on,on), and back.
void SaveGuestProfiles(config::File& f, const std::vector<GuestProfile>& profiles);
void LoadGuestProfiles(const config::File& f, std::vector<GuestProfile>& profiles);
// The record for a guest's name (SameName), or NULL.
GuestProfile* GuestProfileNamed(std::vector<GuestProfile>& profiles, const std::string& name);
}
