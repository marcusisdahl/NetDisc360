// SPDX-License-Identifier: MIT
#pragma once
// A LAN party, as the dashboard's LAN party page runs it: its name and theme,
// the guests (kept from one party to the next, until the list is typed
// again), the consoles it plays on with the Gamemaster's (the host's) first,
// and the team raffle. The raffle draws 2, 3 or 4 teams from the guests who
// are here, sizes within one of each other, keeping two people together or
// apart where asked and not drawing last round's teams again; then it seats
// each team on a console of its own (split-screen, up to four on one), the
// Gamemaster's console first. Logic only: main.cpp keeps it in the settings
// file (SaveParty, LoadParty) and serves it to the page.
#include "config.h"
#include <string>
#include <utility>
#include <vector>

namespace nia {
typedef std::vector<std::string> Team;
typedef std::pair<std::string,std::string> NamePair;
typedef std::vector<std::pair<std::string,Team> > Seats;  // By console address, in the party's order.
enum { PartyGuestsMost = 64, GuestNameMost = 32, PartyNameMost = 64, PartyPairsMost = 64, SeatsPerConsole = 4,
       TeamsLeast = 2, TeamsMost = 4, RaffleTries = 2000 };

struct PartyTheme {
    std::string preset;           // classic, birthday, company or retro.
    std::string accent, accent2;  // Its two colours, #rrggbb.
    std::string person;           // A birthday's: whose, and how old (0: not said).
    int age;
    PartyTheme() : preset("classic"), accent("#107c10"), accent2("#ffffff"), age(0) {}
};

struct PartyGuest { std::string name; bool here; };

// The raffle wheel: a draw shown as a wheel of the guests' names that turns
// once for each of them and stops on the next one to join a team (or once,
// for one guest picked: who picks the game, say). The server plans every turn
// (where the wheel stops, how long it takes), so the dashboard, the big
// screen and NetDiscParty.xex on each console all show the same wheel at
// the same time: each is told how far into the spin it is now.
//
// The wheel's names are its segments, clockwise from the wheel's top when it
// hasn't turned; the pointer is at the right (3 o'clock), so the name it
// stops on reads level. A step turns the wheel clockwise from where
// the step before left it (0 degrees at first) to to degrees, in ms
// milliseconds, easing out as 1-(1-t)^3, and stops on segment, whose guest
// joins team (-1 for a pick). Then the wheel rests for pauseMs (each screen
// celebrates the guest it stopped on) before the next step. The first step starts leadMs after the spin was asked for, so
// every screen hears of it before it moves.
struct SpinStep { int segment; int team; double to; int ms; };
struct Spin {
    unsigned id;                     // 0: none since the server started.
    std::string kind;                // "teams" or "pick".
    std::string title;               // What a pick is for ("Who picks the game?"); "" for teams.
    std::vector<std::string> wheel;
    std::vector<SpinStep> steps;
    int teams, leadMs, pauseMs;
    double at;                       // When it was asked for (Now()); not kept in the settings file.
    Spin() : id(0), teams(0), leadMs(0), pauseMs(0), at(0) {}
    int TotalMs() const;             // From the ask to the last stop.
};
enum { SpinLeadMs = 1500, SpinPauseMs = 2500, SpinFirstMs = 7500, SpinTitleMost = 64 };

struct Party {
    bool active;
    std::string name;
    PartyTheme theme;
    std::string gamemaster;              // The Gamemaster console's address; "" before one is chosen.
    std::vector<std::string> consoles;   // The party's consoles (addresses), the Gamemaster's first.
    std::vector<PartyGuest> guests;
    int teams;                           // How many the raffle draws.
    std::vector<NamePair> together, apart;
    int round;                           // Rounds whose teams were kept (Keep).
    // The latest draw (or seats set by hand): its teams, its seats and what
    // it couldn't follow; kept once Keep made its teams the last ones.
    bool drawn, kept;
    std::vector<Team> drawnTeams;
    Seats seats;
    std::string note;
    std::vector<Team> last;              // The teams kept last, which the next raffle doesn't draw again.
    Spin spin;                           // The latest wheel; gone (id kept) once its draw is changed some other way.
    Party() : active(false), teams(2), round(0), drawn(false), kept(false) {}
};

// A name as typed (a guest's, the party's, a birthday's person): trimmed,
// UTF-8, no control characters, at least least and at most most characters.
// False, with a plain sentence in error, if it isn't one.
bool CleanName(const std::string& typed, size_t least, size_t most, const char* what, std::string& out, std::string& error);
// Whether two names are the same, ignoring case (ASCII and Latin-1 letters: Ø and ø).
bool SameName(const std::string& a, const std::string& b);
// The guest list's own spelling of name; "" if it isn't a guest.
std::string GuestNamed(const Party& party, const std::string& name);
// A colour as #rrggbb (lower case); false if it isn't one.
bool CleanColour(const std::string& typed, std::string& out);
bool KnownPreset(const std::string& preset);
// The guests who are here, in the list's order.
std::vector<std::string> GuestsHere(const Party& party);
// Brings a party read from a file, or changed, into line: guests unique and
// at most PartyGuestsMost, rules only between guests (both ways once, never
// together and apart at once), teams and theme within what they can be,
// the Gamemaster's console first among the consoles.
void TidyParty(Party& party);

// Draws teams from people (sizes within one of each other), following the
// rules between them where it can: together pairs on one team, apart pairs
// not, and not the teams of last again (compared as a set of sets, whatever
// the order). The same seed draws the same teams on every compiler. When no
// draw of RaffleTries follows them all, the one that breaks the fewest is
// returned, and note says what it couldn't follow. False, with why in
// error, when the teams can't be drawn at all.
bool Raffle(const std::vector<std::string>& people, int teams, const std::vector<NamePair>& together, const std::vector<NamePair>& apart,
            const std::vector<Team>& last, unsigned seed, std::vector<Team>& out, std::string& note, std::string& error);
// Seats teams on consoles (the Gamemaster's first): each team on consoles of
// its own, in parts of SeatsPerConsole, the biggest part on the first
// console; when there are fewer consoles than that takes, small teams share
// one. Every console gets an entry, empty if no one plays on it. False, with
// a plain sentence in error, when they don't fit (SeatsPerConsole a console).
bool Seat(const std::vector<Team>& teams, const std::vector<std::string>& consoles, Seats& seats, std::string& error);

// The wheel for teams drawn (see Spin): every guest in them on the wheel in
// an order of seed's, stopping on them a team at a time (each team's first,
// then each team's second, ...), each stop somewhere inside its segment.
void PlanTeamSpin(const std::vector<Team>& teams, unsigned seed, Spin& out);
// The wheel for one of people picked: picked is the index of the one.
void PlanPickSpin(const std::vector<std::string>& people, size_t picked, const std::string& title, unsigned seed, Spin& out);
// Where the wheel is ms after the spin was asked for: its angle in degrees,
// the step under way or resting after (-1 before the first moves), and
// whether that step has stopped. The dashboard (web/index.html) and
// NetDiscParty.xex (party/state.cpp) work it out the same way.
double SpinAngle(const Spin& spin, double ms, int* step=NULL, bool* stopped=NULL);
// The party's title and the line under it, as the dashboard shows them
// (partyTitle, partyLine in web/index.html).
std::string PartyTitle(const Party& party);
std::string PartyLine(const Party& party);

// The party in the settings file (party_... keys), and back, tidied.
void SaveParty(config::File& file, const Party& party);
void LoadParty(const config::File& file, Party& party);
}
