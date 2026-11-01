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
    Screen();
};
// Creates the Dear ImGui context, its style and fonts.
void InitUi();
// Builds one frame; the caller renders ImGui::GetDrawData().
void DrawUi(const Screen& screen);
}
