// SPDX-License-Identifier: MIT
#pragma once
// What the server (main.cpp) and its notification-area icon on Windows
// (tray.cpp) offer each other.
#include <string>
#include <vector>

// main.cpp
// Runs the server with these options (the command line without the program's
// name). config is the settings file (a UTF-8 path; "" for --config or
// none), and tray whether it runs behind the icon. Returns only if it can't
// start, with what went wrong in ServerProblem().
int RunServer(const std::vector<std::string>& args, const std::string& config, bool tray);
std::string ServerSummary();   // "12 games, 1 console connected", for the icon's tip.
std::string ServerProblem();   // The log's last line.
bool FirstTrayStart();         // True only the first time the icon shows, for its tip balloon.

#ifdef _WIN32
// tray.cpp
// Started without options (from Explorer, a shortcut, or at sign-in with
// --tray), the exe runs the server behind its icon and returns its exit code;
// with options it returns -1, and main runs the command-line server.
int TrayMain(int argc, char** argv);
// The server calls this when its dashboard listens (url) or can't (problem).
void DashboardReady(const std::string& url, const std::string& problem);
// A notification from the icon (UTF-8); clicking it opens that page of the
// dashboard, with a warning's icon or else an information one.
void Notify(const std::string& title, const std::string& text, const std::string& page = "#games", bool warning = true);
// Starts this exe again behind the icon, then quits once the new one waits
// for it; false with why if it can't start.
bool Restart(std::string& error);
// Puts data, a newer NetDisc.exe, in place of this exe (kept as .old until
// the new one is running) and restarts as it.
bool InstallExe(const std::string& data, std::string& error);
bool StartsWithWindows();
void SetStartWithWindows(bool on);
#endif
