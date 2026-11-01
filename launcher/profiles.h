// SPDX-License-Identifier: MIT
#pragma once
// Offline Xbox profiles made by the console itself, for the LAN party's
// guests: xam's own profile calls, found by ordinal (the XDK has no import
// for them), as the Xbox dashboard's "Create Profile" uses them. The console
// signs the new profile with its own keys, so every other console takes a
// copy of it (as with a profile moved on a memory unit), which is how the
// server gives the same profile to every party console.
//
// The calls, as xenia (xenia-canary, xam_profile.cc and xam.h) and xkelib
// (_xamext.h) describe them, and as seen on the console:
// XamProfileCreate (ordinal 0x230) starts the creation as a task and hands
// back a creation record; XamProfileGetCreationStatus (0x234) answers
// E_PENDING (0x8000000A) until xam is done, then gives the new profile's
// offline XUID (E000...), or why it couldn't. XamProfileCreateEnumerator and
// XamProfileEnumerate (0x231, 0x232) list the console's profiles with their
// accounts and storage devices. XamProfileFindAccount (0x235) reads one
// profile's account by XUID.
//
// Kept apart from the helper's own code so the LAN party's console app can
// use it too (to sign the guests in).
#include <xtl.h>

namespace nia {
enum { GamertagMost = 15, ProfilesListed = 32 };

// A profile on the console, as XamProfileEnumerate gives it (its account's
// fields that say how it was made; no keys or passwords).
struct ProfileInfo {
    ULONGLONG xuid, xuidOnline;
    WCHAR gamertag[GamertagMost + 1];
    DWORD device, reservedFlags, liveFlags, cachedUserFlags, networkId;
};
// The console's profiles, at most capacity of them; how many.
int ListProfiles(ProfileInfo* out, int capacity);

// How xam is called: device is the storage device to make it on (the one the
// console's own profiles are on; 1 is its hard drive's usual number); flags
// go to xam as they are (1 makes a new profile); xuid is an offline XUID to
// give it (xam answers an invalid argument for 0, and makes the profile
// under an XUID of its own anyway); reservedFlags and cachedUserFlags go
// into the account (an offline profile's from the same console are what the
// dashboard writes);
// zeroTokens hands xam zeroed payment and Live token records instead of none;
// makeFolders makes the XUID's folders on the hard drive first
// (Content\<XUID>\FFFE07D1\00010000), without which xam answers
// ERROR_PATH_NOT_FOUND for an XUID it's given (seen on the console).
struct ProfileCall {
    DWORD device, flags;
    ULONGLONG xuid;
    DWORD reservedFlags, cachedUserFlags;
    bool zeroTokens, makeFolders;
};
// The call for this console: flags 1, its first offline profile's device and
// account flags (the console's language when it has none), no XUID yet.
ProfileCall DefaultProfileCall(const ProfileInfo* profiles, int count);
// A new offline XUID like those the console gives (E000, 16 random bits, then
// the low 32 bits of like: the console's own), one no profile in profiles has.
ULONGLONG NewOfflineXuid(ULONGLONG like, const ProfileInfo* profiles, int count);

// Makes an offline profile called gamertag (1 to GamertagMost characters),
// its offline XUID in xuid. wait(ms) is called while xam works on it (to keep
// a screen drawn); it may be NULL. False, with a plain sentence in error;
// code gets xam's answer (0 when it never got that far).
bool MakeProfile(const WCHAR* gamertag, const ProfileCall& call, ULONGLONG& xuid, DWORD& code, char* error, size_t capacity, void (*wait)(int ms));

// Whether this console has the profile xuid; its gamertag in gamertag
// (GamertagMost + 1 characters), if given.
bool HasProfile(ULONGLONG xuid, WCHAR* gamertag);

// A gamer picture for a profile: PNGs, as the console keeps them in the
// profile's package (tile_64.png, 64 by 64, and tile_32.png, 32 by 32, as
// xenia's notes have it). The profile is opened as a drive with
// XamProfileOpen (0x237), the files written and the drive closed with
// XamProfileClose (0x238), which has xam sign the package again with the
// console's keys. openFlags: XamProfileOpen's flags to try in turn until the
// files can be written (what they mean isn't known; 3 is "open an existing
// one" for content). thumbnail: the package's thumbnail (what the Storage
// screen shows) gets the big picture too, with
// XamContentSetThumbnailInternal (0x272).
struct ProfilePicture {
    const void* big; DWORD bigSize;
    const void* small; DWORD smallSize;
    const DWORD* openFlags; int openFlagCount;
    bool thumbnail;
};
// Puts picture on the profile xuid. how gets what was done, or why not, in
// a sentence (for the log).
bool SetProfilePicture(ULONGLONG xuid, const ProfilePicture& picture, char* how, size_t capacity);
}
