// SPDX-License-Identifier: MIT
#pragma once
// The launcher's loading screen, drawn with Dear ImGui in the style of
// Xbox360-FastFTP. Shared by the Xbox build and tests/ui_preview.cpp, which
// renders it on a PC.
namespace nia {
// What the launcher is doing; the screen shows them as four steps.
enum LoadStep { StepServer, StepMount, StepDisc, StepStart, StepCount };
struct LoadScreen {
    char game[96];      // The game's name, from the ISO's.
    char iso[256];      // The ISO's file name on the server.
    char server[32];    // "192.168.1.20", or empty when unknown.
    unsigned titleId;
    int step;           // The step under way (or that failed).
    double fraction;    // Overall progress, 0 to 1.
    char status[160];   // One line under the progress bar.
    // A failure: a title shows the error card instead of the progress.
    char errorTitle[96];
    char errorLines[3][128];
    bool canRetry;
    // A question in place of a failure (an install disc to install first):
    // the card isn't red, B goes back to Aurora, and A and X do as labelled.
    bool question;
    char buttonA[32], buttonX[32];  // Empty: "Back to Aurora" and "Try again".
    int countdown;      // Seconds until it goes back to Aurora by itself; 0 hides it.
    double time;        // Seconds since start, for the animation.
    double elapsed;     // Seconds since the tile was pressed, shown at the top; below 0 hides it.
    double intro;       // Seconds into the logo intro, drawn over the screen; below 0 or past IntroSeconds: none.
    LoadScreen();
};
// How long the logo intro at the start lasts. The start goes on under it.
const double IntroSeconds=3.0;
// Creates the Dear ImGui context, style and fonts.
void InitLoadUi();
// Builds one frame; the caller renders ImGui::GetDrawData().
void DrawLoadUi(const LoadScreen& screen);
// "Skate 3 (USA, Europe) (En,Fr,De).iso" -> "Skate 3".
void GameName(const char* iso, char* name, size_t capacity);
}
