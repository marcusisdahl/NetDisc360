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

// The party in the settings file (party_... keys), and back, tidied.
void SaveParty(config::File& file, const Party& party);
void LoadParty(const config::File& file, Party& party);
}
