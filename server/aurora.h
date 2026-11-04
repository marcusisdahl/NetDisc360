// SPDX-License-Identifier: MIT
#pragma once
// What NetDisc360 does with Aurora besides its FTP server: its web server
// (port 9999, the login in Aurora's settings under HTTP server), which says
// what the console runs and starts a title, and the Lua scripts NetDisc360
// puts in Aurora's User\Scripts folder.
#include "../shared/platform.h"
#include <string>
#include <vector>

namespace nia {
class AuroraWeb {
public:
    AuroraWeb(DWORD address, int port, const std::string& user, const std::string& password, double timeoutSeconds);
    // The XEX the console runs now, as a device path
    // (\Device\Harddisk0\Partition1\Apps\Aurora\Aurora.xex).
    bool Title(std::string& path);
    // Whether that's Aurora itself, on screen.
    bool InAurora();
    // Starts exec (default.xex) in folder (a device path). The console leaves
    // whatever it runs, a game too.
    bool Launch(const std::string& folder, const std::string& exec);
    // Starts Aurora again, when it is what the console shows (never during a
    // game): false if it isn't, or couldn't be.
    bool RestartAurora();
    // A GET's body as it comes, from path, or from oldPath on the NetISO
    // kit's older Nova (/getTemperatureInfo for /temperature). Never
    // /system (/getSystemInfo): its answer has the console's CPU and DVD keys.
    bool Get(const std::string& path, const std::string& oldPath, std::string& body);
    // The console's screenshots (JSON, a list of {filename, titleid,
    // timestamp, ...}), one as an image (BMP), taking one, and deleting one.
    bool Screenshots(std::string& json);
    bool TakeScreenshot();
    bool Screenshot(const std::string& id, std::string& image, std::string& type);
    bool DeleteScreenshot(const std::string& id);
    const std::string& Error() const { return error_; }
private:
    bool Login();
    bool Call(const std::string& method, const std::string& path, const std::string& form, std::string& body);
    bool Call(const std::string& method, const std::string& path, const std::string& oldMethod, const std::string& oldPath,
              const std::string& form, std::string& body);
    bool Send(const std::string& method, const std::string& path, const std::string& form, int& status, std::string& body);
    DWORD address_;
    int port_;
    std::string user_, password_, token_, cookie_, type_, error_;
    double timeout_;
    bool basic_=false;       // The login goes on each request (HTTP Basic).
    bool challenged_=false;  // The last answer asked for that.
    bool loggedIn_=false;
    bool oldNova_=false;     // The NetISO kit's Nova: /getGameInfo, /postLaunchGame.
    int status_=0;           // The last call's HTTP status.
};

// Sends command to the console's debug monitor (xbdm, port 730), which only
// runs where xbdm.xex is loaded as a DashLaunch plugin: "shutdown" turns the
// console off. false with why in error.
enum { XbdmPort = 730 };
bool Xbdm(DWORD address, int port, const std::string& command, std::string& error);

// The console's name, as set on it (Settings, System, Console Settings,
// Console Name), and its fixed id (the UDN its media sharing gives, in lower
// case without "uuid:"; it ends in its network card's address), from the
// description its media sharing (UPnP, port 1027) gives without being asked
// first. False, with why in error, when that doesn't answer (media sharing
// off) or isn't an Xbox 360's. name is plain text (entities decoded, no
// control characters or bars), at most ConsoleNameMost bytes.
enum { ConsoleNamePort = 1027, ConsoleNameMost = 64 };
bool ConsoleName(DWORD address, int port, std::string& name, std::string& id, std::string& error);
// Shows text (ASCII; anything else becomes '?') in a notification on the
// console's screen, through the JRPC2 plugin on its debug monitor (xbdm).
// False, with why in error, when xbdm doesn't answer or JRPC2 isn't loaded.
bool ConsoleNotify(DWORD address, int port, const std::string& text, std::string& error);

// An FTP path on the hard drive (/Hdd1/NetISO/Halo 3) as a device path
// (\Device\Harddisk0\Partition1\NetISO\Halo 3); "" for another drive.
std::string DevicePath(const std::string& ftpPath);

// The Lua scripts, with CRLF line ends as Aurora's own. tiles is the game
// folder on the Xbox (/Hdd1/NetISO).
//
// The Utilities script "NetDisc360 Cleanup": Aurora keeps a game in its list
// when its folder is gone, so a tile NetDisc360 removed can stay there,
// starting nothing. The script removes those from Aurora's list (and its
// recent games, favourites, hidden games and title settings), after asking:
// the games listed from inside tiles whose folder isn't one of folders (the
// folders in tiles now). It deletes no file.
std::string CleanupScript(const std::string& tiles, const std::vector<std::string>& folders);
// The filter "Hide PC games while NetDisc360 is off" (Filters, User): it
// hides the tiles while OnlineMarker wasn't there as Aurora started.
// NetDisc360 keeps that file while it runs and deletes it when it quits.
// The first time the filter is used after a start, it writes ShownMarker
// ("online" or "offline"), so NetDisc360 can tell when Aurora hides the
// games while it runs (and restart Aurora). With tidy, it also takes the
// games whose tile is gone off Aurora's list as Aurora starts, as the cleanup
// script does but without asking (only while at least one tile is there).
std::string OnlineScript(const std::string& tiles, bool tidy);
extern const char CleanupScriptPath[], OnlineScriptPath[], OnlineMarker[], ShownMarker[];

// A game's title update as Aurora's own list should have it (TitleUpdateScript).
struct AuroraTu {
    unsigned titleId, mediaId;
    std::string hash;  // The Content ID (40 hex digits) of the one Aurora should have on; "": none.
    // To add it to Aurora's list (content.db TitleUpdates) when it isn't
    // there: backupPath "" adds nothing.
    std::string name, file, size, device, livePath, backupPath;
    unsigned base, version;
    AuroraTu() : titleId(0), mediaId(0), base(0), version(0) {}
};
// The filter script NetDiscTitleUpdates.lua (TitleUpdateScriptPath, in
// Aurora's folder). Aurora's title update manager makes each game's update
// folder match the updates Aurora has on (settings.db ActiveTitleUpdates)
// each time it starts the game: it copies the one on from its store and
// deletes the others. So a change made on the dashboard has to be in that
// list too. As Aurora starts, the script makes the list say wants, once:
// it writes what it did to done (a script path, Hdd1:\...) and does nothing
// while that file is there, so a later change in Aurora's own title update
// screen stays. Each line of done: <Title ID> <Media ID> on|off|missing|error.
std::string TitleUpdateScript(const std::vector<AuroraTu>& wants, const std::string& done);
extern const char TitleUpdateScriptPath[];
}
