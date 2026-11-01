// SPDX-License-Identifier: MIT
#pragma once
// The LAN party as NetDiscParty.xex reads it from NetDisc360 (PartyText in
// server/main.cpp): its name, this console's name and seats, the guests who
// are here, the teams drawn and the raffle wheel's latest spin. Plain C++
// with fixed sizes, shared by the Xbox build and tests/party_preview.cpp.
namespace party {
enum { NameBytes = 132, AddressBytes = 16, WheelMost = 64, TeamsMost = 4, SeatsMost = 16, PerConsole = 4, KindBytes = 8, GamertagBytes = 32, NoteBytes = 200,
       GuestsMost = 64, KnownMost = 16, PaletteMost = 8 };
typedef char Name[NameBytes];

// A turn of the wheel: from where the turn before left it to to degrees
// (clockwise), in ms milliseconds, stopping on segment, whose guest joins
// team (-1: picked alone). See Spin in server/party.h.
struct Step { int segment, team; double to; int ms; };
struct Seat { char address[AddressBytes]; Name console; int count; Name names[PerConsole]; };
// A guest of this console's seats who signed in here already (PartySignIn):
// the seat, the controller (0-3) and the profile's gamertag.
struct SignedIn { int seat, controller; char gamertag[GamertagBytes]; };

// A colour a team can have (the server's TeamPalette): its name ("Red"),
// its fill, a brighter one for text on the dark screen, and whether text on
// the fill is dark.
struct Colour { char name[16]; unsigned fill, bright; bool ink; };
// A guest on the list (kept from party to party): here at this one, their
// gamerscore (-1: not read yet), and whether they joined on this console.
struct Guest { Name name; bool here; int gamerscore; bool joinedHere; };
// A console NetDisc360 knows: what it does now ("wheel", "aurora", "game"
// or "off"), and whether it's one of the party's.
struct Known { char address[AddressBytes]; Name name; char state[KindBytes]; bool inParty; };

struct State {
    bool valid;                     // A party was read.
    unsigned long long version;     // The server's, to wait on (PartyWait).
    Name console;                   // This console's name ("FABLE"), as the server knows it.
    char address[AddressBytes];     // Its address, as the server sees it.
    bool gamemaster;                // This console is the Gamemaster's.
    bool active;                    // The party is on.
    Name title, line;               // "Fall LAN", "Happy 28th, Ola!".
    char preset[16];
    unsigned accent, accent2;       // 0xRRGGBB.
    Name gamemasterName;            // The Gamemaster console's name; "" when none was chosen.
    int teams;                      // How many the raffle draws.
    int hereCount; Name here[WheelMost];
    // The latest spin (spinId 0: none): elapsed is how far into it the server
    // was when it answered, in milliseconds.
    unsigned spinId; char kind[KindBytes]; int elapsed, leadMs, pauseMs; Name question;
    int wheelCount; Name wheel[WheelMost];
    int stepCount; Step steps[WheelMost];
    // The teams drawn and where each sits (round 0: none drawn).
    int round; bool kept;
    int teamCount; int teamSize[TeamsMost]; Name team[TeamsMost][WheelMost];
    int seatCount; Seat seats[SeatsMost];
    int signedCount; SignedIn signedIn[PerConsole];
    // This console's seated guests' profiles on their way here (after a
    // spin or a swap): profiles is "copying", "ready" or "failed" ("": none
    // asked), done of total, the guest whose profile comes now, and a note
    // (who has no profile yet, or what went wrong).
    char profiles[KindBytes + 4]; int profilesDone, profilesTotal; Name profilesNow; char profilesNote[NoteBytes];
    // The server's build (NETDISC_BUILD_STAMP, the commit, the version), and
    // whether this app should make way for the newer one it has (update).
    unsigned serverBuild; char serverCommit[16]; char serverVersion[24]; bool update;
    // The colours a team can have, and each team's (an index into palette).
    int paletteCount; Colour palette[PaletteMost];
    int teamColour[TeamsMost];
    int guestCount; Guest guests[GuestsMost];
    int knownCount; Known known[KnownMost];
};
// Team t's colour, with Red, Blue, Green and Yellow when the server doesn't say.
Colour TeamColourOf(const State& s, int t);

// Reads the server's text into state; false (state.valid false) if it isn't one.
bool Parse(const char* text, unsigned length, State& state);
// The spin's length from the ask to the last stop, in milliseconds.
int SpinMs(const State& s);
// Where the wheel is ms into the spin, as SpinAngle in server/party.cpp: its
// angle in degrees, the step under way or resting after (-1 before the first
// moves), and whether that step has stopped.
double SpinAngle(const State& s, double ms, int* step, bool* stopped);
// How many guests the wheel has stopped on ms into the spin.
int Stopped(const State& s, double ms);
// When the wheel stops for step i, in milliseconds into the spin.
int StopMs(const State& s, int i);
// This console's seat ("" address: none), and the team its first guest is on (-1: none).
const Seat* MySeat(const State& s);
int TeamOf(const State& s, const char* name);
// Whether a profile's gamertag is a guest's name, as NetDisc360 makes guest
// profiles (GuestGamertag in server/guests.h): letters and digits compared
// without case, spaces or accents (Ø as O, Æ as AE), a long name cut short.
bool GamertagOf(const char* gamertag, const char* name);
}
