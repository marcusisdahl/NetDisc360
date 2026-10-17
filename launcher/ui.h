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
    char server[32];    // "10.10.1.56", or empty when unknown.
    unsigned titleId;
    int step;           // The step under way (or that failed).
    double fraction;    // Overall progress, 0 to 1.
    char status[160];   // One line under the progress bar.
    // A failure: a title shows the error card instead of the progress.
    char errorTitle[96];
    char errorLines[3][128];
    bool canRetry;
    int countdown;      // Seconds until it goes back to Aurora by itself; 0 hides it.
    double time;        // Seconds since start, for the animation.
    LoadScreen();
};
// Creates the Dear ImGui context, style and fonts.
void InitLoadUi();
// Builds one frame; the caller renders ImGui::GetDrawData().
void DrawLoadUi(const LoadScreen& screen);
// "Skate 3 (USA, Europe) (En,Fr,De).iso" -> "Skate 3".
void GameName(const char* iso, char* name, size_t capacity);
}
