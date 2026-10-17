// SPDX-License-Identifier: MIT
// The window of NetDisc.exe (NetDisc 360) on Windows, in three parts that share this
// header: gui.cpp runs the window, the server process, the notification-area
// icon and the settings; gui_library.cpp finds the games in the folders and
// their covers; gui_view.cpp draws the pages with Dear ImGui.
//
// Text for the pages is UTF-8; paths and settings are kept as Windows keeps
// them (UTF-16).
#pragma once
#ifdef _WIN32
#include "discs.h"  // Windows and Winsock, through platform.h.
#include <string>
#include <vector>

struct ImFont;
struct ImDrawList;
struct ID3D11Device;
struct ID3D11DeviceContext;

namespace gui {

enum Theme { ThemeSystem, ThemeLight, ThemeDark };

// A console that has used the server, kept between runs for the dashboard.
struct KnownConsole {
    std::string address;
    long long seen;  // When it was last connected (time()).
};

struct Config {
    std::vector<std::wstring> folders;
    std::wstring xbox;  // Only these consoles' game folders are kept up to date (addresses, comma separated); empty: every console.
    std::wstring user, password, options;
    int theme;
    bool covers;      // Download covers for the games page.
    bool sortBySize;
    bool showIso, showGod, showFolder;
    bool toldTray;
    bool setupDone;   // The dashboard's setup steps were all done once.
    std::vector<KnownConsole> consoles;
    Config() : user(L"xboxftp"), password(L"xboxftp"), theme(ThemeSystem), covers(true), sortBySize(false),
               showIso(true), showGod(true), showFolder(true), toldTray(false), setupDone(false) {}
};
extern Config Saved;

// A game the server serves.
struct Game {
    std::string name;      // As Aurora's folder is named, without ".iso".
    std::string path;      // The ISO, the extracted game's folder or the GOD package.
    unsigned long long size;
    nia::DiscKind kind;
    DWORD titleId;         // 0: not known.
    int disc;
};
// A game that is there more than once; only kept is served.
struct Duplicate { std::string copy, kept; };

struct Library {
    std::vector<Game> games;
    std::vector<Duplicate> duplicates;
    std::vector<int> perFolder;  // Games in each of Saved.folders.
    bool done;                   // False until the first scan of the folders has finished.
    Library() : done(false) {}
};
extern Library Games;

// A console using the server, from the server's "@stats" lines (--stats).
struct ConsoleState {
    std::string address;
    int connections;
    unsigned long long sent;  // Bytes of game data sent to it since the server started.
    int ftp;                  // Its Aurora's FTP server: 0 not tried yet, 1 not answering, 2 answers.
    long long ftpAt;          // When ftp last changed (time()).
    long long synced;         // When its game folders were last brought up to date (time()); 0: not yet.
    std::string mounted;      // Its disc, without ".iso"; empty: none.
    long long lastAt;         // The server's clock (ms) at its last line, for the rate.
    float rate[120];          // Bytes per second, by second: rate[s % 120] is for second s (Second()).
    long long rateSecond[120];
    ConsoleState() : connections(0), sent(0), ftp(0), ftpAt(0), synced(0), lastAt(0) {
        for (int i = 0; i < 120; ++i) { rate[i] = 0; rateSecond[i] = -1; }
    }
    float RateAt(long long second) const { return rateSecond[second % 120] == second ? rate[second % 120] : 0; }
};

// Something that happened, for the dashboard.
enum EventKind { EventInfo, EventGood, EventPlay, EventXbox, EventWarning };
struct Event {
    std::string time;  // "hh:mm"
    int kind;
    std::string text;
};

// What the server's output has said.
struct ServerState {
    bool running;
    int games;               // From its "N game(s) in" line; -1 before.
    std::vector<ConsoleState> consoles;
    unsigned long long sentBefore;  // Sent by earlier runs of the server since the window opened.
    float peak;                     // The highest rate to all consoles together (bytes/s) since the window opened.
    std::vector<Event> events;      // The latest last.
    std::vector<std::string> log;   // What it printed (the last lines), and what the window adds.
    ServerState() : running(false), games(-1), sentBefore(0), peak(0) {}
};
extern ServerState Server;

extern std::string ThisPc;     // The PC's addresses, for netiso.xex.txt.
long long Second();            // A clock in seconds, for the consoles' rates.
extern float Scale;            // The window's DPI / 96.
extern bool Dark;
extern ImFont *FontText, *FontBold, *FontMono, *FontIcons;
extern ID3D11Device* Device;
extern ID3D11DeviceContext* Context;

// gui.cpp: what the pages can do.
void AddFolders();                         // Asks for folders.
void AddFolder(const std::wstring& folder);
void RemoveFolder(size_t index);
void ShowInExplorer(const std::string& path);
void OpenUrl(const char* url);
void StartServer();
void StopServer(bool say);
void Quit();
void SetConnection(const std::wstring& xbox, const std::wstring& user, const std::wstring& password);
void ForgetConsole(const std::string& address);
void SetupDone();
void SetOptions(const std::wstring& options);
bool StartsWithWindows();
void SetStartWithWindows(bool on);
void SetTheme(int theme);
void SaveConfig();
std::wstring IniPath();
void WantFrames(int count);        // Draws the window this many more times.
void WantFrameAfter(int milliseconds);  // And once more after this long (animations, tooltips).
void Log(const std::string& line);

// gui_library.cpp
// Scans the folders now and while the window shows; posts each new list as
// libraryMessage and each cover as coverMessage to window.
void StartLibrary(HWND window, UINT libraryMessage, UINT coverMessage);
void Rescan();
bool Scanning();
void TakeLibrary(void* posted);  // What was posted as libraryMessage.
// The cover of a title as a Direct3D texture (an ImTextureID) and its pixel
// size, or 0 with width left 0 while it loads and set to -1 if it has none.
unsigned long long Cover(DWORD titleId, float& width, float& height);
void TakeCover(void* posted);    // What was posted as coverMessage.
// Covers drawn between SmoothImages(draw, true) and (draw, false) use mipmaps.
void SmoothImages(ImDrawList* draw, bool on);
void ForgetCovers(bool deleteFiles);
void StopLibrary();

// gui_view.cpp
void Draw();        // One frame of the window.
void ApplyStyle();  // Colors and sizes for Dark and Scale.

// UTF-8 <-> UTF-16, and the ANSI code page the server's paths are in.
std::string Utf8(const std::wstring& text);
std::wstring Wide(const std::string& utf8);
std::string AnsiToUtf8(const std::string& text);
std::string Utf8ToAnsi(const std::string& text);
}
#endif
