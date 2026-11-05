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

// An FTP path on the hard drive (/Hdd1/NetDisc360/Games/Halo 3) as a device path
// (\Device\Harddisk0\Partition1\NetDisc360\Games\Halo 3); "" for another drive.
std::string DevicePath(const std::string& ftpPath);

// The Lua scripts, with CRLF line ends as Aurora's own. tiles is the game
// folder on the Xbox (/Hdd1/NetDisc360/Games).
//
// The Utilities script "NetDisc360 Cleanup": Aurora keeps a game in its list
// when its folder is gone, so a tile NetDisc360 removed can stay there,
// starting nothing. The script removes those from Aurora's list (and its
// recent games, favourites, hidden games and title settings), after asking:
// the games listed from inside tiles whose folder isn't one of folders (the
// folders in tiles now). It deletes no file.
//
// When from (the tiles' folder before, /Hdd1/NetISO) isn't "", both scripts
// also move Aurora's scan path for from and its rows for the tiles there to
// tiles (MoveLua), so a game keeps its art, favourites and play history when
// its folder moved from from to tiles (MoveOldFolder in main.cpp). Only the
// rows of tiles in folders move. The filter does it as Aurora starts; the
// cleanup script, should that have found no Sql, when it is run. Each writes
// what it did to MovedMarker in app, NetDisc360's folder on the Xbox.
std::string CleanupScript(const std::string& tiles, const std::vector<std::string>& folders, const std::string& from, const std::string& app);
// The filter "Hide PC games while NetDisc360 is off" (Filters, User): it
// hides the tiles while OnlineMarker wasn't there as Aurora started.
// NetDisc360 keeps that file while it runs and deletes it when it quits.
// The first time the filter is used after a start, it writes ShownMarker
// ("online" or "offline"), so NetDisc360 can tell when Aurora hides the
// games while it runs (and restart Aurora).
// Its markers are in app.
std::string OnlineScript(const std::string& tiles, const std::string& app, const std::string& from, const std::vector<std::string>& folders);
std::string MoveLua(const std::string& from, const std::string& tiles, const std::vector<std::string>& folders);
// MoveMarker: NetDisc360 moved tiles to this console's tiles folder, and
// Aurora's rows haven't followed yet; MovedMarker: what the scripts did.
extern const char CleanupScriptPath[], OnlineScriptPath[], OnlineMarker[], ShownMarker[], MoveMarker[], MovedMarker[];

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
