// SPDX-License-Identifier: MIT
#pragma once
// NetDisc360's users (Marcus 2026-11-03): everyone it knows, apart from any
// LAN party, each with their Xbox profile: one offline profile, the same
// XUID on every console it's put on, so their saves and achievements follow
// them from console to console. A LAN party's guest list picks its players
// from them (a name typed there becomes a user too).
//
// They're kept in users.ini beside the settings file, which holds nothing
// about this computer (no folders, no console addresses), so computers can
// share it (the sync of PR #30): one line per user, keyed by a stable id
// that's there from the start (a user exists before their profile does),
// with the profile's XUID once there is one. Two lines with the same XUID
// are the same user. Each line says when it last changed, and the one
// changed last wins when computers' copies are merged; a removed user's
// line stays (removed=when) so the removal reaches the other computers,
// with its name but no XUID or gamertag (their kept profile copies stay on
// disk; the same XUID adopted again is a new user).
//
// Which consoles have a profile is this computer's (console addresses):
// the guest_profile lines of the settings file (guests.h). Logic only:
// main.cpp keeps them, makes the profiles and serves the page.
#include <string>
#include <vector>

namespace nia {
enum { UsersMost = 1024, UserIdLength = 17 };

struct User {
    std::string id;          // "u" and 16 hex digits (lower case), made once; never changes.
    std::string name;        // As the lists show it (a party's guest list too).
    std::string gamertag;    // Their profile's gamertag; "" until there is one.
    std::string xuid;        // Its XUID: 16 hex digits starting with E, upper case; "" until made.
    long long made;          // When the profile was made or adopted (Unix seconds); 0: none.
    bool adopted;            // Made on a console by hand, not by NetDisc360.
    long long changed;       // When this line last changed (Unix seconds).
    long long removed;       // When the user was removed (Unix seconds); 0: not.
    // What they did, as GuestStats (party.h) has it: gamerscore and
    // achievements as last read (-1: not yet) and when, time played, games
    // started, parties and the latest's end, and the latest game.
    int gamerscore, achievements;
    long long readAt, playSeconds;
    int plays, parties;
    long long lastParty;
    std::string lastGame;
    User() : made(0), adopted(false), changed(0), removed(0), gamerscore(-1), achievements(-1), readAt(0), playSeconds(0), plays(0), parties(0), lastParty(0) {}
};

// A new id: "u" and 16 random hex digits.
std::string NewUserId();
bool IsUserId(const std::string& text);

// users.ini: "; ..." comments, [Users], count=N and userN=id|name|gamertag|
// xuid|made|adopted|changed|removed|gamerscore|achievements|readAt|
// playSeconds|plays|parties|lastParty|lastGame, in UTF-8; "%" and "|" in a
// text are %25 and %7C. ReadUsers leaves out lines it can't read (no id,
// a bad XUID) and later lines with an id or XUID already read.
std::string UsersText(const std::vector<User>& users);
void ReadUsers(const std::string& text, std::vector<User>& users);
// A user's line without "changed": two that compare equal say the same.
std::string UserContent(const User& user);

// The user (not removed) with that name (ignoring case: SameName), XUID or
// id (removed too); NULL if none.
User* UserNamed(std::vector<User>& users, const std::string& name);
User* UserWithXuid(std::vector<User>& users, const std::string& xuid);
User* UserWithId(std::vector<User>& users, const std::string& id);

// Merges another computer's users into these, per user: the line with the
// same XUID, else the same id, is the same user, and the one changed last
// wins (keeping this side's id). A user only the other side has is added.
// Two users (not removed) left with one name: if one has no profile, it was
// the same person typed on two computers, so it's folded into the other
// (its line removed); if both have one, the newer gets a number ("Ola 2").
// Returns the ids of the users that changed here.
std::vector<std::string> MergeUsers(std::vector<User>& mine, const std::vector<User>& other, long long now);

// The server's (main.cpp): merges users.ini text from another computer into
// this one's, saves it and tells the dashboard. False (error says why) if
// the text isn't a users file.
bool MergeUsersFile(const std::string& text, std::string& error);

// The file on each console (in NetDisc360's folder:
// Hdd1:\NetISO\NetDisc360\profiles.ini) that says which of its profiles are
// NetDisc360's (made by it, or a user's own it was given) and whose they
// are, so any NetDisc360 (or a page) that meets the console knows them:
// [Profiles] and <XUID>=<user id>|<name>|<gamertag>|<made>|<adopted: 1 or 0>.
struct ProfileMark { std::string xuid, user, name, gamertag; long long made; bool adopted; ProfileMark() : made(0), adopted(false) {} };
std::string ProfileMarksText(const std::vector<ProfileMark>& marks);
void ReadProfileMarks(const std::string& text, std::vector<ProfileMark>& marks);
}
