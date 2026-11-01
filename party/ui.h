// SPDX-License-Identifier: MIT
#pragma once
// NetDiscParty.xex's screen: the raffle wheel and this console's place in
// the LAN party, drawn with Dear ImGui in the launcher's style (see
// launcher/ui.cpp). Shared by the Xbox build and tests/party_preview.cpp.
#include "state.h"

namespace party {
// What the server connection is doing, for the line at the bottom.
enum Link { LinkLooking, LinkWaiting, LinkConnected, LinkLost };
// The wheel, or this console's players signing in one after another, as a
// split-screen game asks them (after the wheel drew the teams).
enum View { ViewWheel, ViewPlayers };
// A seat's guest on the players screen: the controller they signed in on
// (0-3; -1 not yet) and their profile's gamertag.
struct Player { int controller; char gamertag[GamertagBytes]; };
struct Screen {
    State state;
    double spinMs;      // How far into the spin (state.spinId) it is now.
    double time;        // Seconds since the start, for the animation.
    int link;           // Link
    char server[32];    // "192.168.1.20"; "" when there is none.
    char notice[160];   // A line the Gamemaster's last spin got back ("" for none).
    int view;           // View
    Player players[PerConsole];  // By seat on this console (MySeat).
    int asking;         // The seat whose guest is asked to press A (-1: everyone's in).
    int choosing;       // The controller whose profile chooser (the Guide's) is open; -1 none.
    int leaveIn;        // Seconds to Aurora, once everyone's in; -1: staying.
    // Guest profiles are being copied to this console (state.profiles, or
    // the app's FTP server busy): B doesn't leave, and no one signs in yet.
    bool copying;
    bool noticeGood;    // notice is good news ("Welcome, Ola!"), not a problem.
    // The Gamemaster's party setup over the wheel (Setup): its tab (SetupTab),
    // the row picked, and the first row shown (the list scrolls).
    bool setup; int setupTab, setupRow, setupTop;
    int hostRow;        // The console picked while there's no Gamemaster (the known consoles that are on).
    int typing;         // The controller typing a name to join (-1: none).
    bool updating;      // Making way for the server's newer app: back to Aurora in a moment.
    char version[48];   // This app's version and build ("v1.1.0-dev  c9c9b76").
    Screen();
};
// The setup's tabs, and how many rows each has: Players (Add a player, then
// every guest), Teams (how many, then each team's colour) and Consoles (Open
// the party on all, then every console NetDisc360 knows).
enum SetupTab { TabPlayers, TabTeams, TabConsoles, TabsCount };
int SetupRows(const State& s, int tab);
enum { SetupShown = 6 };  // Rows shown at once.
// The consoles to choose the Gamemaster from (those that are on), by
// hostRow: indices into s.known; count of them.
int HostChoices(const State& s, int* out);
// Creates the Dear ImGui context, its style and fonts.
void InitUi();
// Builds one frame; the caller renders ImGui::GetDrawData().
void DrawUi(const Screen& screen);
}
