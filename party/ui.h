// SPDX-License-Identifier: MIT
#pragma once
// NetDiscParty.xex's screen: the raffle wheel and this console's place in
// the LAN party, drawn with Dear ImGui in the launcher's style (see
// launcher/ui.cpp). Shared by the Xbox build and tests/party_preview.cpp.
#include "state.h"

namespace party {
// What the server connection is doing, for the line at the bottom.
enum Link { LinkLooking, LinkWaiting, LinkConnected, LinkLost };
struct Screen {
    State state;
    double spinMs;      // How far into the spin (state.spinId) it is now.
    double time;        // Seconds since the start, for the animation.
    int link;           // Link
    char server[32];    // "192.168.1.20"; "" when there is none.
    char notice[160];   // A line the Gamemaster's last spin got back ("" for none).
    Screen();
};
// Creates the Dear ImGui context, its style and fonts.
void InitUi();
// Builds one frame; the caller renders ImGui::GetDrawData().
void DrawUi(const Screen& screen);
}
