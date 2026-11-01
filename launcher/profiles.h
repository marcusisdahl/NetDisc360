// SPDX-License-Identifier: MIT
#pragma once
// Offline Xbox profiles made by the console itself, for the LAN party's
// guests: xam's own profile calls, found by ordinal (the XDK has no import
// for them), as the Xbox dashboard's "Create Profile" uses them. The console
// signs the new profile with its own keys, so every other console takes a
// copy of it (as with a profile moved on a memory unit), which is how the
// server gives the same profile to every party console.
//
// The calls, as xenia (xenia-canary, xam_profile.cc and xam.h) describes them:
// XamProfileCreate (ordinal 0x230) starts the creation as a task and hands
// back a creation record; XamProfileGetCreationStatus (0x234) answers
// ERROR_IO_PENDING until the profile is made, then gives its offline XUID
// (E000...) and frees the record. XamProfileFindAccount (0x235) reads a
// profile's account (its gamertag) by XUID.
//
// Kept apart from the helper's own code so the LAN party's console app can
// use it too (to sign the guests in).
#include <xtl.h>

namespace nia {
enum { GamertagMost = 15 };

// How xam is called: device is the storage device to make it on (1: the hard
// drive); flags go to xam as they are (0, as far as known, for an offline
// profile). zeroTokens hands xam zeroed payment and Live token records
// instead of none, in case a console's xam reads them without looking.
struct ProfileCall { DWORD device, flags; bool zeroTokens; };

// Makes an offline profile called gamertag (1 to GamertagMost characters),
// its offline XUID in xuid. wait(ms) is called while xam works on it (to keep
// a screen drawn); it may be NULL. False, with a plain sentence in error.
bool MakeProfile(const WCHAR* gamertag, const ProfileCall& call, ULONGLONG& xuid, char* error, size_t capacity, void (*wait)(int ms));

// Whether this console has the profile xuid; its gamertag in gamertag
// (GamertagMost + 1 characters), if given.
bool HasProfile(ULONGLONG xuid, WCHAR* gamertag);
}
