// SPDX-License-Identifier: MIT
// NetDisc360, a NetISO server for Aurora: one program in place of
// netiso-srv. It serves the games in folders (and their subfolders) to the
// NetISO plugin, noticing new ones without a restart, and keeps the console's
// Hdd1:\NetISO game folders in step with them over Aurora's FTP server, with
// NetISO Sync's own code (shared/library.cpp). The launcher XEXs it writes are
// built into it. Its dashboard is a web page it serves itself (web.cpp), where
// the settings can be changed while it runs.
//
//   netdisc [options] [game folder ...]      (netdisc -h lists them)
#include "../shared/helper.h"
#include "../shared/library.h"
#include "app.h"
#include "aurora.h"
#include "auroradb.h"
#include "backup.h"
#include "config.h"
#include "discmap.h"
#include "discs.h"
#include "ftp.h"
#include "guests.h"
#include "picture.h"
#include "installed.h"
#include "party.h"
#include "relay.h"
#include "system.h"
#include "systemlink.h"
#include "update.h"
#include "users.h"
#include "version.h"
#include "../shared/build_id.h"
#include "web.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <random>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <thread>
#include <time.h>
#include <vector>
#include "resource.h"
#ifdef _WIN32
#include <iphlpapi.h>
#include <mstcpip.h>
#pragma comment(lib, "iphlpapi.lib")
typedef int AddressLength;
#else
#include <dirent.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <signal.h>
typedef socklen_t AddressLength;
#endif

using namespace nia;

namespace {
// PartyWait, PartySpinAsk and PartySignIn are NetDiscParty.xex's (party/),
// not NetISO's: see Connection::PartyRequest.
enum { Ping = 0, GetIsoSize = 1, HasType1File = 2, ReadData = 3, GetIsoName = 4, MountIso = 5, PartyWait = 0x5057, PartySpinAsk = 0x5053, PartySignIn = 0x5049, PartyEditAsk = 0x5045, PartyJobAsk = 0x504A, Header = 20,
       MaxReadBytes = 16 << 20, MaxNameBytes = 4096, MaxDepth = 8, RateSeconds = 120, WebPort = 8360 };
// An ISO counts as settled, and gets pushed, when nothing about it changed for this long.
const double SettleSeconds = 10, ProbeDownSeconds = 15, RetrySkippedSeconds = 600, RetryFailedSeconds = 60, ScanRetrySeconds = 30;
// A console that connected once (no --xbox) but not for this long is probed only every IdleProbeSeconds.
const double IdleSeconds = 86400, IdleProbeSeconds = 600;
// While a console is in a game, its FTP port is tried this long after the
// mount and then this often, to see when it's back in Aurora (AuroraLoop).
const double FirstProbeSeconds = 5, GameProbeSeconds = 15;
// Read after it got back to Aurora: a game started from the disc again.
const U64 RelaunchBytes = 32ull << 20;
// Read after a mount: the game was played (see PlayStarted).
const U64 PlayBytes = 16ull << 20;
// A console that answered pings before is offline after this many unanswered
// in a row, each waited for this long, and once its NetISO connections end
// (PingLoop), which a quick keepalive makes them do within SuspectSeconds.
const int OfflinePings = 3, PingTimeoutMs = 3000;
const double SuspectSeconds = 20;
const int ScanTries = 20;
const size_t LogKeep = 3000, EventKeep = 100;

struct Options {
    std::vector<std::string> folders;
    std::string ftpUser, ftpPassword, xboxFolder, aurora, launcherPath, bootPath;
    std::string configPath;  // The settings file (UTF-8), if there is one.
    std::string webAddress;  // Where the dashboard listens: 127.0.0.1 (this PC) or 0.0.0.0.
    int port, ftpPort, webPort;
    std::vector<DWORD> xboxes;  // --xbox: the consoles to update; none: every console that connects.
    bool push, filter, scan, verbose, pushOnce;
    bool webLan;      // The dashboard's "other devices" switch: listens on 0.0.0.0 instead of webAddress.
    bool setupDone;   // The dashboard's setup steps were done once.
    bool wizardDone;  // The first start's setup wizard was finished or skipped.
    bool toldTray;    // The notification-area icon's first tip was shown.
    // After the Xbox's FTP server comes back, the longest wait for the Xbox
    // to show it's online before game folders are written (see WatchLoop).
    double onlineWait, removeWait;
    // How often the games installed on a console are looked at again, so one
    // installed or deleted there (in Aurora, over FTP) gets or loses its tile.
    // They're also looked at when Aurora starts or a game ends; this is for a
    // change in Aurora without either, and is slow so FTP is left alone.
    double installedEvery;
    double pingSeconds;  // How often each console is pinged to see it's still on (PingLoop); 0: never.
    // Backups of the settings (BackupLoop): every so many days (0: only by
    // hand), keeping the newest so many but those made by hand.
    int backupEvery, backupKeep;
    // Updates (UpdateLoop): whether to look for them, install them by
    // themselves (NetDisc360.exe), and count test versions (pre-releases) too.
    bool updateCheck, updateAuto, updatePrerelease;
    std::string releasesFile;  // --releases: GitHub's list of releases from this file instead (for the tests).
    // Games installed on a console get no tile (see installed.h); they're
    // looked for in these FTP folders (semicolons between) and their drives'
    // Games on Demand. With skipInstalled off they get one, whose launcher
    // asks which copy to play (local_copy in its netiso.ini).
    bool skipInstalled;
    std::string installedFolders;
    // Aurora's scripts (see aurora.h): the cleanup script and the filter that
    // hides the PC games while the server is off, with its marker file; and
    // Aurora restarted (only while it's on screen) when that filter shows the
    // other state than the server is in.
    bool auroraScripts, restartAurora;
    // Aurora's filter script also takes the games whose tile is gone off its
    // list as Aurora starts (see OnlineScript in aurora.h), without asking.
    bool tidyAurora;
    // A profile for each new name on the LAN party's guest list, made by
    // itself (MakeProfilesSoon); --no-auto-profiles (the tests) leaves that
    // to the page's Make profiles.
    bool autoProfiles;
    // "Set up this console": the plugin set (file names in slot order) and the
    // folder on this computer with the plugin files (see ConsoleSetup).
    std::string pluginSet, pluginKit;
    // Aurora's web server (Aurora's settings, HTTP server), to start games and restart Aurora.
    std::string auroraWebUser, auroraWebPassword;
    int auroraWebPort;
    bool followAddress;  // NetISO.xex.txt names this computer again when its address changed (FollowAddress).
    bool keepAwake;      // Windows doesn't sleep while a console plays (AwakeLoop).
    int autoOffMinutes;  // A console is turned off this long after its last game ended (AutoOff); 0: never.
    int xbdmPort;        // The consoles' debug monitor (xbdm), which turns them off; --xbdm-port for the tests.
    // Each console's own name (see ConsoleName in aurora.h) is read from its
    // media sharing on this port (--name-port for the tests), and shown on
    // its screen when Aurora starts, with showName (through JRPC2).
    int namePort;
    bool showName;
    Options() : ftpUser("xboxftp"), ftpPassword("xboxftp"), xboxFolder("/Hdd1/NetISO"), aurora("/Hdd1/Apps/Aurora"), webAddress("127.0.0.1"),
                port(IsvrPort), ftpPort(21), webPort(WebPort), push(true), filter(true), scan(true), verbose(false), pushOnce(false),
                webLan(false), setupDone(false), wizardDone(false), toldTray(false), onlineWait(120), removeWait(60), installedEvery(300), pingSeconds(30),
                backupEvery(7), backupKeep(5), updateCheck(true), updateAuto(false), updatePrerelease(false),
                skipInstalled(true), installedFolders("/Hdd1/Games"), auroraScripts(true), restartAurora(true), tidyAurora(false), autoProfiles(true),
                pluginSet("NetISO.xex, NetIsoTiles.xex, xbdm.xex, AutoDNS.xex, JRPC2.xex"),
                auroraWebUser("xboxhttp"), auroraWebPassword("xboxhttp"), auroraWebPort(9999), followAddress(true), keepAwake(true), autoOffMinutes(0), xbdmPort(XbdmPort),
                namePort(ConsoleNamePort), showName(true) {}
};
// The dashboard changes the settings while the server runs, so they're read
// with Current() (a copy) and changed under SettingsLock.
Options Settings;
std::mutex SettingsLock;
Options Current() { std::lock_guard<std::mutex> hold(SettingsLock); return Settings; }

std::vector<unsigned char> Launcher, Boot;
std::vector<unsigned char> WheelApp;  // NetDiscParty.xex, built in, to put on a console that hasn't got it (OpenWheel).
bool CanPush;         // A launcher is built in (or given), so game folders can be written.
bool InTray;          // NetDisc360.exe behind its notification-area icon (tray.cpp).
bool PasswordFixed;   // The dashboard's password comes from NETDISC_WEB_PASSWORD.
bool InDocker;        // In the Docker image (NETDISC_DOCKER), which is updated by pulling it.
// Settings the first start got from its command line or environment (in
// Docker, the NETDISC_* variables), which the setup wizard shows as filled in.
struct Preset { bool folders, xboxes, ftp; } Given;
double StartedAt;
std::atomic<long long> GameStarts(0);  // Mounts of a game by a console, for /metrics.
// The command line's options, under the settings file's: what a restored
// backup's settings go over (Reload).
Options CommandLine;
// The version before an update and the one installed, kept in the settings
// file until the new version has started and said so. Should the old one
// start instead (the new one didn't, and was swapped back), the new one is
// skipped by automatic updates (UpdateSkip).
std::string UpdatedFrom, UpdateTo, UpdateSkip;
// The backup of the settings made just before that update (update_backup).
std::string UpdateBackup;
// The versions this copy has run, oldest first, for the dashboard's update
// history (update_history, under SettingsLock). how: app (NetDisc360
// installed it), other (put in place another way: by hand, or a new Docker
// image), first (the first start) or failed (it didn't start, so from was
// put back).
struct Installed { std::string version, from, how, backup; long long at; };
std::vector<Installed> UpdateHistory;
enum { HistoryKeep = 20 };
// Raised by a settings change that needs every console's game folders checked again.
std::atomic<int> PushGeneration(0);
// When the game folders were last changed on the dashboard, in Now() ms: like
// a start, it holds back removals for removeWait (see WatchLoop).
std::atomic<long long> FoldersChangedAt(0);
// Whether a game folder couldn't be read at the latest look (a drive or share
// that isn't there): its games only seem gone then, so no game folder is
// removed from a console meanwhile (see PushTo). MissingFolders, under
// MissingLock, lists them for the log.
std::atomic<bool> FoldersMissing(false);
std::mutex MissingLock;
std::string MissingFolders;

// The log, and what the dashboard shows of it --------------------------------

struct LogLine { long long id; std::string text; };
std::mutex LogLock;
std::deque<LogLine> LogLines;  // The latest, for the dashboard.
long long LogCount;

void Log(const char* format, ...) {
    char text[2048];
    va_list a; va_start(a,format); _vsnprintf_s(text,sizeof(text),_TRUNCATE,format,a); va_end(a);
    time_t now=time(NULL); struct tm local;
#ifdef _WIN32
    localtime_s(&local,&now);
#else
    localtime_r(&now,&local);
#endif
    char stamp[16]; _snprintf_s(stamp,sizeof(stamp),_TRUNCATE,"%02d:%02d:%02d",local.tm_hour,local.tm_min,local.tm_sec);
    std::lock_guard<std::mutex> hold(LogLock);
    printf("%s %s\n",stamp,text);
    fflush(stdout);
    LogLine line = {++LogCount, std::string(stamp)+" "+text};
    LogLines.push_back(line);
    if(LogLines.size()>LogKeep) LogLines.pop_front();
}

// What happened, in a few words, for the dashboard's activity list.
enum EventKind { EventInfo, EventGood, EventPlay, EventXbox, EventWarning };
const char* const EventKinds[] = {"info", "good", "play", "xbox", "warning"};
struct Happening { long long id, time; int kind; std::string text; };
std::mutex EventLock;
std::deque<Happening> Events;
long long EventCount;

void Event(int kind, const char* format, ...) {
    char text[1024];
    va_list a; va_start(a,format); _vsnprintf_s(text,sizeof(text),_TRUNCATE,format,a); va_end(a);
    std::lock_guard<std::mutex> hold(EventLock);
    Happening h = {++EventCount, (long long)time(NULL), kind, text};
    Events.push_back(h);
    if(Events.size()>EventKeep) Events.pop_front();
}

std::string Lower(std::string text) {
    for(size_t i=0;i<text.size();++i) text[i]=char(tolower((unsigned char)text[i]));
    return text;
}

std::string WithoutIso(const std::string& name) {
    return name.size()>4 && !_stricmp(name.c_str()+name.size()-4,".iso") ? name.substr(0,name.size()-4) : name;
}

// Files ----------------------------------------------------------------------

// A folder as given, without the slashes at its end: "D:\" stays a drive's
// top, not D:'s current folder.
std::string CleanFolder(std::string folder) {
    while(folder.size()>1 && (folder[folder.size()-1]=='/' || folder[folder.size()-1]=='\\')) folder.erase(folder.size()-1);
    if(folder.size()==2 && folder[1]==':') folder+='\\';
    return folder;
}

bool SameFolder(const std::string& a, const std::string& b) {
#ifdef _WIN32
    return !_stricmp(a.c_str(),b.c_str());
#else
    return a==b;
#endif
}

// Whether path is inside folder.
bool Within(const std::string& path, const std::string& folder) {
    if(path.size()<=folder.size()) return false;
#ifdef _WIN32
    if(_strnicmp(path.c_str(),folder.c_str(),folder.size())) return false;
#else
    if(path.compare(0,folder.size(),folder)) return false;
#endif
    char last=folder[folder.size()-1], next=path[folder.size()];
    return last=='/' || last=='\\' || next=='/' || next=='\\';
}

// The discs (ISOs, extracted games, GOD packages) are found and read in discs.cpp.

// The games as the dashboard shows them: the latest list, and the size of
// each extracted game (counted by SizeLoop, since that reads every folder).
std::mutex GamesListLock;
std::vector<IsoFile> GamesList;
std::map<std::string,std::pair<long long,U64> > FolderSizes;  // Path: the default.xex time it was counted for, and the bytes.
long long GamesVersion;  // Changes with either, or with the conflicts.
bool GamesScanned;       // The folders were read once.
// WatchLoop looks at the folders every second, or, where that takes long (a
// NAS, a slow share, a big library), less often: ScanEffort times as long
// as the last look took, at most ScanMostSeconds apart, so a NAS's disks can
// sleep. ScanSoon has it look again at once (a change on the dashboard, or a
// console asking for a game the latest look didn't find).
const double ScanEffort = 10, ScanMostSeconds = 30;
std::atomic<bool> ScanSoon(false);

// Games that are in the folders more than once. Each is a conflict: every
// copy of one game (Title ID and disc), and the one served. Until the user
// chooses a copy on the dashboard, the server serves one by itself (an ISO
// over a GOD package over a folder) and the conflict is open: the dashboard
// shows it, and on Windows the icon says so once. A choice is kept in the
// settings file with the copies there were then, so a copy that turns up
// later, or the chosen one going, opens the conflict again.
struct Choice { std::string path; std::set<std::string> copies; };  // The chosen copy, and CopyId of each copy then.
struct Conflict { std::string key, name; IsoFile served; std::vector<IsoFile> copies; bool chosen, open; };
std::mutex DuplicatesLock;
std::map<std::string,Choice> Choices;     // By ConflictKey, under DuplicatesLock.
std::vector<Conflict> Conflicts;          // The latest, served copy first in each, under DuplicatesLock.
std::string DuplicatesReported, ConflictsShown;
std::set<std::string> Announced;          // Open conflicts already announced (key and copies).

std::string ConflictKey(const IsoFile& disc) {
    char key[16]; _snprintf_s(key,sizeof(key),_TRUNCATE,"%08X-%d",disc.titleId,disc.disc);
    return key;
}
// A short name for a copy's path, for the settings file.
std::string CopyId(const std::string& path) {
    unsigned int h=2166136261u;
    for(size_t i=0;i<path.size();++i) { h^=(unsigned char)path[i]; h*=16777619u; }
    char id[12]; _snprintf_s(id,sizeof(id),_TRUNCATE,"%08x",h);
    return id;
}

// Where a disc is, as the user would remove it: a GOD package by its Title ID folder.
std::string Location(const IsoFile& disc) {
    if(disc.kind!=DiscGod) return disc.path;
    std::string path=disc.path;
    for(int i=0;i<2;++i) { size_t at=path.find_last_of("/\\"); if(at!=std::string::npos) path.erase(at); }
    return path+" (GOD package)";
}

std::string Count(int n, const char* one, const char* many);
std::string Number(long long n);
std::vector<std::string> PluginSetNames(const std::string& text);

// Brings Conflicts up to date with what Isos() found, under DuplicatesLock,
// and tells about conflicts that are newly open.
void NoteConflicts(const std::vector<IsoFile>& dropped, const std::vector<IsoFile>& kept) {
    std::map<std::string,Conflict> found;
    for(size_t i=0;i<dropped.size();++i) {
        Conflict& c=found[ConflictKey(kept[i])];
        if(c.copies.empty()) { c.key=ConflictKey(kept[i]); c.served=kept[i]; c.name=WithoutIso(kept[i].name); c.copies.push_back(kept[i]); }
        c.copies.push_back(dropped[i]);
    }
    std::vector<Conflict> now;
    std::vector<std::string> fresh;
    std::set<std::string> announced;
    std::string shown;
    for(std::map<std::string,Conflict>::iterator f=found.begin();f!=found.end();++f) {
        Conflict& c=f->second;
        std::string copies;
        for(size_t i=0;i<c.copies.size();++i) copies+="|"+CopyId(c.copies[i].path);
        std::map<std::string,Choice>::const_iterator choice=Choices.find(c.key);
        c.chosen=choice!=Choices.end() && choice->second.path==c.served.path;
        c.open=!c.chosen;
        for(size_t i=0;i<c.copies.size() && !c.open;++i) c.open=!choice->second.copies.count(CopyId(c.copies[i].path));
        shown+=c.key+copies+(c.open ? "?" : "")+c.served.path+"\n";
        if(c.open) {
            announced.insert(c.key+copies);
            if(!Announced.count(c.key+copies)) fresh.push_back(c.name);
        }
        now.push_back(c);
    }
    std::sort(now.begin(),now.end(),[](const Conflict& a, const Conflict& b) { return _stricmp(a.name.c_str(),b.name.c_str())<0; });
    Conflicts.swap(now);
    Announced.swap(announced);
    if(shown!=ConflictsShown) {
        ConflictsShown=shown;
        std::lock_guard<std::mutex> hold(GamesListLock);
        ++GamesVersion;
    }
    if(fresh.empty()) return;
    std::string text=fresh.size()==1 ? fresh[0]+" is in your folders more than once"
                                     : Count(int(fresh.size()),"game is","games are")+" in your folders more than once";
    Event(EventWarning,"%s. Choose which copy to play on the Games page.",text.c_str());
#ifdef _WIN32
    if(InTray) Notify("Choose which copy to play",config::ToUtf8(text)+". Click here to choose.");
#endif
}

// The Title ID the launcher XEX carries as built (FFFF4E49, "NI").
const DWORD LauncherTitleId=0xFFFF4E49;

// The disc map, discs.ini next to the settings file (see discmap.h); in
// memory only without a settings file.
DiscMap Map;
std::mutex MapLock;
std::string DiscMapPath(const std::string& configPath) {
    if(configPath.empty()) return std::string();
    size_t cut=configPath.find_last_of("/\\");
    return (cut==std::string::npos ? std::string() : configPath.substr(0,cut+1))+"discs.ini";
}
// What the disc map says each disc is; a game on more than one disc that
// the map doesn't have yet is added to it. The map has the last word on
// install discs: only a play disc needs one, or a disc the game goes on to
// after it (Wolfenstein's), and only from an install or content disc.
void MapDiscs(std::vector<IsoFile>& isos) {
    std::string path=DiscMapPath(Current().configPath),error;
    std::vector<std::string> problems,added;
    {
        std::lock_guard<std::mutex> hold(MapLock);
        Map.Load(path,problems);
        Map.Learn(isos,added,error);
        Map.Apply(isos);
    }
    for(size_t i=0;i<problems.size();++i) Log("Disc map %s, %s; that line is left out.",config::FromUtf8(path).c_str(),problems[i].c_str());
    for(size_t i=0;i<added.size();++i) Log("Disc map: added %s.",added[i].c_str());
    if(!error.empty()) Log("Disc map: %s; the games just added are kept until the server stops.",error.c_str());
    for(size_t i=0;i<isos.size();++i) {
        IsoFile& d=isos[i];
        if(d.installDisc.empty()) continue;
        const IsoFile* carrier=NULL;
        for(size_t k=0;k<isos.size() && !carrier;++k) if(isos[k].name==d.installDisc) carrier=&isos[k];
        bool fits=(d.role.empty() || d.role=="play" || d.role=="next") &&
                  (!carrier || carrier->role.empty() || carrier->role=="install" || carrier->role=="content");
        if(!fits) d.installDisc.clear();
    }
}

// A game folder that had games and has none now, though it can be read: a
// NAS share or a drive that isn't mounted, so the empty folder under it shows
// (or a USB drive's letter now on another drive). Its games only seem gone,
// so it counts as missing and no game is removed meanwhile. A folder emptied
// on purpose: removing it from the list, or a restart, lets its tiles go.
std::map<std::string,size_t> FolderGames;  // The most games each folder had since the start, under MissingLock.
std::set<std::string> EmptiedFolders;      // Those that have none now, under MissingLock.
bool Emptied(const std::string& folder, size_t games) {
    std::lock_guard<std::mutex> hold(MissingLock);
    size_t& most=FolderGames[folder];
    most=std::max(most,games);
    return !games && most;
}

// ISOs shorter than their files say (see discs.h), told about once each.
std::set<std::string> ToldIncomplete;  // Under MissingLock.
void NoteIncomplete(const std::vector<IsoFile>& isos) {
    std::set<std::string> now;
    for(size_t i=0;i<isos.size();++i) if(isos[i].incomplete) now.insert(isos[i].path);
    std::vector<std::string> fresh;
    {
        std::lock_guard<std::mutex> hold(MissingLock);
        for(std::set<std::string>::const_iterator p=now.begin();p!=now.end();++p) if(!ToldIncomplete.count(*p)) fresh.push_back(*p);
        ToldIncomplete.swap(now);
    }
    for(size_t i=0;i<fresh.size();++i) {
        Log("%s is shorter than the files on it say: it was cut off (a download or copy that stopped). A game crashes where it reads the missing part; copy it again.",fresh[i].c_str());
        size_t cut=fresh[i].find_last_of("/\\");
        Event(EventWarning,"%s is incomplete; copy it again",WithoutIso(cut==std::string::npos ? fresh[i] : fresh[i].substr(cut+1)).c_str());
    }
}

// Everything in the folders, each game once. A game that is there more than
// once is reported (once, until that changes) so the copies can be removed or
// one of them chosen.
std::vector<IsoFile> Isos() {
    std::vector<std::string> folders=Current().folders;
    std::vector<IsoFile> isos;
    std::set<std::string> paths;
    std::string missing;
    std::set<std::string> emptied;
    for(size_t i=0;i<folders.size();++i) {
        std::string folder=folders[i];
        if(folder.size()==3 && folder[1]==':') folder+='.';  // "D:\." for the top of D:.
        std::vector<IsoFile> found;
        if(!FindIsos(folder,found)) missing+="  "+folders[i]+"\n";
        else if(Emptied(folders[i],found.size())) { missing+="  "+folders[i]+" (it had games, and has none now)\n"; emptied.insert(folders[i]); }
        // A folder inside another one listed finds the same discs again.
        for(size_t k=0;k<found.size();++k) if(paths.insert(found[k].path).second) isos.push_back(found[k]);
    }
    FoldersMissing=!missing.empty();
    {
        std::lock_guard<std::mutex> hold(MissingLock);
        EmptiedFolders.swap(emptied);
        if(missing!=MissingFolders) {
            if(!missing.empty()) Log("Can't read these game folders; until they're back, no game is removed from the consoles:\n%s",missing.substr(0,missing.size()-1).c_str());
            else Log("Every game folder can be read again.");
            MissingFolders=missing;
        }
    }
    ReadTitles(isos);
    NoteIncomplete(isos);
    // The launcher's own build (NetISO Sync, in a build folder that is also
    // served) has default.xex too, with the launcher's Title ID; it's no game.
    for(size_t i=isos.size();i-->0;) if(isos[i].kind==DiscFolder && isos[i].titleId==LauncherTitleId) isos.erase(isos.begin()+i);
    std::lock_guard<std::mutex> hold(DuplicatesLock);
    std::set<std::string> chosen;
    for(std::map<std::string,Choice>::const_iterator c=Choices.begin();c!=Choices.end();++c) chosen.insert(c->second.path);
    std::vector<IsoFile> dropped,kept;
    DropDuplicates(isos,dropped,kept,chosen);
    UniqueNames(isos);
    LinkInstallDiscs(isos);
    MapDiscs(isos);
    for(size_t i=0;i<kept.size();++i)  // With the names just given.
        for(size_t k=0;k<isos.size();++k) if(isos[k].path==kept[i].path) { kept[i]=isos[k]; break; }
    std::string report;
    for(size_t i=0;i<dropped.size();++i) {
        char line[96]; _snprintf_s(line,sizeof(line),_TRUNCATE,"Title ID %08X, disc %d",dropped[i].titleId,dropped[i].disc);
        report+="  "+Location(dropped[i])+"\n    is the same game as "+Location(kept[i])+" ("+line+"); only that one is served.\n";
    }
    if(report!=DuplicatesReported) {
        DuplicatesReported=report;
        if(!report.empty()) {
            report.erase(report.size()-1);
            Log("%d game(s) are here more than once. Choose which copy to play on the dashboard's Games page, or remove the extra copies:\n%s",int(dropped.size()),report.c_str());
        }
    }
    NoteConflicts(dropped,kept);
    return isos;
}

// A mount of a name that isn't in the latest list looks through the game
// folders right away (a game copied in a moment ago starts at once), but at
// most every 30 s: a tile whose game was removed shouldn't have every try
// read a whole NAS library on the console's connection. Otherwise the next
// scan finds it.
std::atomic<long long> LookedForMissingAt(-1000000);
bool MayLookNow() {
    long long now=(long long)(Now()*1000), last=LookedForMissingAt;
    return now-last>=30000 && LookedForMissingAt.compare_exchange_strong(last,now);
}

// The games WatchLoop found at its latest look at the folders, so a console
// listing or mounting doesn't have them all read again (over a network, that
// takes a while); the folders themselves before the first look.
std::vector<IsoFile> LatestGames() {
    {
        std::lock_guard<std::mutex> hold(GamesListLock);
        if(GamesScanned) return GamesList;
    }
    return Isos();
}

// When the server first saw each game (by its served name), for Aurora's
// "Recently added" filter. Kept in first-seen.txt beside the settings
// ("<time()>\t<name>" lines); the games there when the file is first made
// count as old (0), so only games added after that are new.
std::mutex FirstSeenLock;
std::map<std::string,long long> FirstSeen;  // Under FirstSeenLock.
bool FirstSeenLoaded=false;
std::string FirstSeenPath(const Options& o) { return o.configPath.empty() ? std::string() : config::FolderOf(o.configPath)+"first-seen.txt"; }
void NoteFirstSeen(const std::vector<IsoFile>& isos) {
    if(isos.empty()) return;
    std::string path=FirstSeenPath(Current());
    std::lock_guard<std::mutex> hold(FirstSeenLock);
    bool fresh=false;
    if(!FirstSeenLoaded) {
        FirstSeenLoaded=true;
        std::string text;
        if(!path.empty() && config::ReadFile(path,text,16<<20)) {
            for(size_t at=0;at<text.size();) {
                size_t end=text.find('\n',at); if(end==std::string::npos) end=text.size();
                std::string line=text.substr(at,end-at); at=end+1;
                if(!line.empty() && line[line.size()-1]=='\r') line.erase(line.size()-1);
                size_t tab=line.find('\t');
                if(tab!=std::string::npos) FirstSeen[line.substr(tab+1)]=atoll(line.substr(0,tab).c_str());
            }
        } else fresh=true;
    }
    long long now=(long long)time(NULL);
    bool changed=fresh;
    for(size_t i=0;i<isos.size();++i)
        if(!FirstSeen.count(isos[i].name)) { FirstSeen[isos[i].name]=fresh ? 0 : now; changed=true; }
    if(!changed || path.empty()) return;
    std::string text;
    for(std::map<std::string,long long>::const_iterator f=FirstSeen.begin();f!=FirstSeen.end();++f) {
        char number[24]; _snprintf_s(number,sizeof(number),_TRUNCATE,"%lld\t",f->second);
        text+=number+f->first+"\n";
    }
    config::WriteFile(path,text,false);
}
// The games first seen in the last days, as names.
std::set<std::string> SeenSince(long long since) {
    std::set<std::string> out;
    std::lock_guard<std::mutex> hold(FirstSeenLock);
    for(std::map<std::string,long long>::const_iterator f=FirstSeen.begin();f!=FirstSeen.end();++f) if(f->second>=since) out.insert(f->first);
    return out;
}

// What the push compares: an ISO that is still being copied keeps changing.
std::string Fingerprint(const std::vector<IsoFile>& isos) {
    std::string text;
    for(size_t i=0;i<isos.size();++i) {
        char line[64]; _snprintf_s(line,sizeof(line),_TRUNCATE,"|%llu|%lld\n",(unsigned long long)isos[i].size,isos[i].modified);
        text+=isos[i].path+line;
    }
    return text;
}

// The NetISO server ----------------------------------------------------------

bool Loopback(DWORD address) { return (ntohl(address)>>24)==127; }
// Whether the game folders of the console at address are kept up to date.
bool FromXbox(DWORD address) {
    std::lock_guard<std::mutex> hold(SettingsLock);
    if(Settings.xboxes.empty()) return !Loopback(address);
    return std::find(Settings.xboxes.begin(),Settings.xboxes.end(),address)!=Settings.xboxes.end();
}

// A console that uses this server: one given with --xbox, or any that
// connects. Several can play at once, each with its own disc; each gets its
// game folders kept up to date (WatchLoop) and its numbers shown on the
// dashboard.
struct Console {
    DWORD address;
    char text[16];
    std::atomic<bool> push;            // FromXbox.
    // When it last connected or sent a NetISO request (its plugin or a
    // launcher), in Now() milliseconds. Those connections start on the Xbox,
    // so they show its own network is up (see WatchLoop).
    std::atomic<long long> seenAt;
    std::atomic<int> connections;
    std::atomic<U64> sent;             // Bytes of game data sent to it.
    std::atomic<int> ftp;              // Its Aurora's FTP server: 0 not tried, 1 not answering, 2 answers.
    std::atomic<long long> synced;     // When its game folders were last brought up to date (time()).
    // Under ConsoleLock:
    std::string mounted;
    double rate[RateSeconds];          // Bytes per second, by second (RateLoop): rate[s % RateSeconds] is for second s,
    long long rateSecond[RateSeconds]; // which is in rateSecond.
    U64 counted;                       // sent at the last second counted.
    long long hiddenAt;                // Forgotten on the dashboard when last seen at this seenAt; -2: not.
    mutable std::deque<Happening> recent;  // Its latest events (XboxEvent), for its card on the dashboard.
    // Back in Aurora since its latest game (AuroraLoop). NetISO keeps its
    // connection and disc after a game ends, so only Aurora's FTP server,
    // which never runs during a game, tells.
    std::atomic<bool> inAurora;
    std::atomic<U64> sentInAurora;     // sent when it got back.
    std::atomic<long long> probeAt;    // When AuroraLoop next tries its FTP port, in Now() milliseconds.
    // PingLoop's pings: the latest answer's round trip in ms (-1: none yet),
    // whether the latest ping was answered and any ever was, the misses in a
    // row, and offline: it answered before, then stopped (see PingLoop).
    std::atomic<int> pingMs, missed;
    std::atomic<bool> answers, answered, offline;
    std::atomic<bool> toldNoPings;     // "Doesn't answer pings, but ..." was logged since its last answer.
    std::atomic<long long> answeredAt, offlineAt, pingAt;  // In Now() milliseconds.
    std::atomic<long long> suspectAt;  // When its connections got a quick keepalive; 0: they didn't.
    std::set<SOCKET> sockets;          // Its NetISO connections, under ConsoleLock.
    // From its latest push, under ConsoleLock: its game folders by the
    // listed name of their disc (lower case), and the games installed on it.
    std::map<std::string,std::string> tiles;
    std::vector<InstalledGame> installed;
    // Raised when it's back in Aurora (after a game, or turned on again),
    // and when its first NetISO connection comes (Aurora starting, its
    // plugin listing the games): its push thread looks at what's installed
    // on it again, and writes its game folders again if that changed, so a
    // game installed since loses its tile and one deleted gets it back (see
    // PushTo).
    std::atomic<bool> recheck;
    // Its own name and fixed id (see ConsoleName in aurora.h; "" until read),
    // under ConsoleLock, and when NamesLoop last tried (Now() milliseconds).
    std::string name, id;
    std::atomic<long long> nameAt;
    // This server's FTP sessions with it now (see Busy): Aurora is never
    // restarted while one is open (see SafeToRestart).
    std::atomic<int> busy;
    // The title updates its Aurora has (see LookTitleUpdates), under
    // ConsoleLock: as last read (time() then; 0: never), and why the latest
    // look failed ("" if it didn't).
    std::vector<TitleUpdate> titleUpdates;
    long long titleUpdatesRead;
    std::string titleUpdatesError;
    // Held while they're looked at, so one look at a time (its push thread,
    // Start on all); taken before ConsoleLock. Under it, what the looks keep
    // of Aurora's databases, content.db [0] and settings.db [1]: the listing
    // (size and time) whose copy was read, and the one last listed and since
    // when (Now()); the tables as read; what the log last said.
    std::mutex updatesLock;
    std::string dbRead[2], dbListed[2];
    double dbListedAt[2];
    std::vector<TitleUpdate> dbUpdates;
    std::vector<long long> dbActive;
    std::string toldUpdates;
    // A game started from the dashboard (StartOn), until it's seen back in
    // Aurora: when (Now() milliseconds; 0: none), and whether Aurora's FTP
    // server stopped answering since, as it does while a game runs (see
    // FollowLaunch).
    std::atomic<long long> launchedAt;
    std::atomic<bool> launchLeft;
    // NetDisc360's marker for Aurora's "Hide PC games while NetDisc360 is
    // off" filter is on it (see aurora.h), so quitting deletes it.
    std::atomic<bool> marked;
    // The game it plays (for the play history), under ConsoleLock: its
    // listed name without .iso ("" for none), Title ID and since when (Now()).
    std::string game;
    DWORD gameTitle;
    double gameSince;
    // When its latest game ended (it was back in Aurora), in Now()
    // milliseconds; 0 once it turned off, or plays again (see AutoOff).
    std::atomic<long long> gameEndedAt;
    // While its game folders are being brought up to date: of how many games
    // (0: not now) Sync has looked at how many, for the dashboard.
    mutable std::atomic<int> pushDone, pushTotal;
    explicit Console(DWORD at) : address(at), push(FromXbox(at)), seenAt(-1), connections(0), sent(0), ftp(0), synced(0), counted(0), hiddenAt(-2),
                                 inAurora(false), sentInAurora(0), probeAt(0), pingMs(-1), missed(0), answers(false), answered(false), offline(false), toldNoPings(false),
                                 answeredAt(-1), offlineAt(0), pingAt(0), suspectAt(0), recheck(false), nameAt(0), busy(0), titleUpdatesRead(0), launchedAt(0), launchLeft(false),
                                 marked(false), gameTitle(0), gameSince(0), gameEndedAt(0), pushDone(0), pushTotal(0) {
        AddressText(at,text,sizeof(text));
        dbListedAt[0]=dbListedAt[1]=0;
        for(int i=0;i<RateSeconds;++i) { rate[i]=0; rateSecond[i]=-1; }
    }
    double RateAt(long long second) const { return rateSecond[second%RateSeconds]==second ? rate[second%RateSeconds] : 0; }
};
std::mutex ConsoleLock;  // Taken before SettingsLock when both are.
std::vector<Console*> Consoles;  // Never removed, so a Console* stays valid.
double Peak;             // The most sent to all consoles together in a second, under ConsoleLock.
long long RateSecond=-1; // The latest second counted, under ConsoleLock.

// The console at address, added the first time it shows up; NULL for this PC
// (the push's own connections), unless it was given with --xbox (the tests).
Console* ConsoleAt(DWORD address, bool* added=NULL) {
    if(added) *added=false;
    if(Loopback(address) && !FromXbox(address)) return NULL;
    std::lock_guard<std::mutex> hold(ConsoleLock);
    for(size_t i=0;i<Consoles.size();++i) if(Consoles[i]->address==address) return Consoles[i];
    Consoles.push_back(new Console(address));
    if(added) *added=true;
    return Consoles.back();
}

// The console at address if it's known already; NULL if not.
Console* ConsoleIfKnown(DWORD address) {
    std::lock_guard<std::mutex> hold(ConsoleLock);
    for(size_t i=0;i<Consoles.size();++i) if(Consoles[i]->address==address) return Consoles[i];
    return NULL;
}

std::vector<Console*> AllConsoles() {
    std::lock_guard<std::mutex> hold(ConsoleLock);
    return Consoles;
}

// Counts an FTP session of this server with a console while it's open (see
// Console::busy), so Aurora isn't restarted under it.
struct Busy {
    Console* c;
    explicit Busy(Console* console) : c(console) { if(c) ++c->busy; }
    ~Busy() { if(c) --c->busy; }
private:
    Busy(const Busy&); Busy& operator=(const Busy&);
};

// A console that has used the server, kept in the settings file so the
// dashboard knows it after a restart, with its own name and id as last read
// (see ConsoleName in aurora.h; "" if never).
struct KnownConsole { std::string address; long long seen; std::string id, name; };
std::vector<KnownConsole> Known;  // Under SettingsLock.
// This computer's addresses as far back as the settings file remembers
// (own_addresses), newest last, so a NetISO.xex.txt naming an address it had
// is told from one naming another server (see FollowAddress). Under SettingsLock.
std::vector<std::string> OwnAddresses;
const size_t OwnAddressesKept = 16;
// The LAN party (see party.h), kept in the settings file; under PartyLock,
// which no other lock is taken under.
Party TheParty;
std::mutex PartyLock;
// Held while the party is changed (ChangeParty, PartyRaffle, KeepTeams,
// ChangeSeats, LoadConfig), from reading it to saving it, so changes sent
// at once (each request has its own thread) don't undo each other. Taken
// before any other lock.
std::mutex PartyChangeLock;
Party CurrentParty() { std::lock_guard<std::mutex> hold(PartyLock); return TheParty; }
// The LAN party's guest profiles (see guests.h), kept in the settings file;
// under GuestLock, which no other lock is taken under.
std::vector<GuestProfile> GuestProfiles;
std::mutex GuestLock;
std::vector<GuestProfile> GuestProfilesNow() { std::lock_guard<std::mutex> hold(GuestLock); return GuestProfiles; }
// The users (see users.h), kept in users.ini beside the settings file, also
// under GuestLock. UsersSaved: each one's line (UserContent) as users.ini
// last had it, by id, so a user whose line changes gets a new "changed";
// UsersWritten: the file's text as last written or read.
std::vector<User> Users;
std::map<std::string,std::string> UsersSaved;
std::string UsersWritten;
// The user called name, made if there's none (under GuestLock).
User& UserFor(const std::string& name) {
    User* u=UserNamed(Users,name);
    if(u) return *u;
    User fresh;
    fresh.id=NewUserId(); fresh.name=name; fresh.changed=(long long)time(NULL);
    Users.push_back(fresh);
    return Users.back();
}
std::string UserIdOf(const std::string& name) { std::lock_guard<std::mutex> hold(GuestLock); User* u=UserNamed(Users,name); return u ? u->id : std::string(); }
// The seated guests' profiles going to their consoles after a spin or a
// swap (PushSeatProfiles), by console address: what NetDiscParty.xex and
// the page show meanwhile. Under GuestLock.
struct SeatProfiles {
    std::string state;   // "copying", "ready" or "failed".
    int done, total;     // Guests whose profile is on it, of those seated there who have one.
    std::string now;     // The guest whose profile is being copied.
    std::string note;    // Who has no profile yet, or what went wrong.
    SeatProfiles() : done(0), total(0) {}
};
std::map<std::string,SeatProfiles> SeatPushes;
void PushSeatProfilesSoon();
void AdoptGuestProfile(const std::string& name, unsigned long long xuid, const std::string& gamertag, const std::string& console);
void PlaysOn(const std::string& name, unsigned long long xuid, const std::string& console);
std::string PartyAppJobAnswer(const std::string& console, int index, const std::string& body);
std::string PartyAppJobFor(const std::string& console);
std::vector<std::string> GuestsWithoutProfile();
// Each name on the guest list gets its profile by itself (MakeProfilesSoon),
// seconds after the list last changed.
enum { AutoMakeDelaySeconds = 8 };
void MakeProfilesSoon(double seconds);
// What each guest did at the parties (see GuestStats in party.h): kept in
// the settings file with the guest list, by name; under StatsLock, which no
// other lock is taken under.
std::vector<GuestStats> GuestStatsList;
std::mutex StatsLock;
std::atomic<bool> StatsReading(false);  // POST /api/party/stats reads them now.
std::vector<GuestStats> GuestStatsNow() { std::lock_guard<std::mutex> hold(StatsLock); return GuestStatsList; }
GuestStats& StatsOf(const std::string& name) {  // Under StatsLock.
    for(size_t i=0;i<GuestStatsList.size();++i) if(SameName(GuestStatsList[i].name,name)) return GuestStatsList[i];
    GuestStats g; g.name=name;
    GuestStatsList.push_back(g);
    return GuestStatsList.back();
}
// The guests playing on a console now (or who played on it last): those who
// signed in on its party screen, else those seated there; none when no party is on.
std::vector<std::string> GuestsAt(const std::string& address);
// A game on console ended after seconds: the time goes to its guests'.
void GuestsPlayed(const std::string& address, double seconds, const std::string& game);
// The gamerscore of the guests at a console, read again from their profiles
// there a little after it's back in Aurora (ReadGuestStats).
void GuestStatsSoon(const std::string& address, double seconds);
// The party's version, one more with each change (Save), which
// NetDiscParty.xex waits on (PartyWait) so it hears of a spin at once
// without asking again and again.
std::mutex PartyNewsLock;
std::condition_variable PartyNewsCome;
unsigned long long PartyVersion=1;
// The live feed on the big screen (Marcus 2026-11-02): what happens at the
// party as it happens, newest last; FeedMost kept, while a party is on.
// Join from a phone: how posting the party to its relay went (RelayLoop).
struct RelayState { bool ok; std::string error; double at; int joins; RelayState() : ok(false), at(0), joins(0) {} };
RelayState RelayNow;
std::mutex RelayLock;
// A party code: 6 letters and digits, none that look alike (no I, O, 0, 1).
std::string NewPartyCode() {
    static const char letters[]="ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    const std::string random=web::NewSecret();
    std::string code;
    for(int i=0;i<6;++i) code+=letters[strtoul(random.substr(size_t(i)*2,2).c_str(),NULL,16)%32];
    return code;
}
struct FeedItem { long long at; std::string kind, text; };
std::deque<FeedItem> Feed;
std::mutex FeedLock;
enum { FeedMost = 30, FeedShown = 12 };
// When anything was last done for the party (a join, a sign-in, a spin, a
// game started or won), for Turn off all when idle; Now() seconds.
std::atomic<long long> PartyActiveAt(0);
// How often the live feed asks each party console's Aurora for its signed-in
// profiles' gamerscore (LiveLoop).
double LiveEverySeconds = 40;
void PartyBusy() { PartyActiveAt=(long long)Now(); }
void PartyNews();
Party CurrentParty();
void PartyFeed(const char* kind, const char* format, ...) {
    if(!CurrentParty().active) return;
    char text[400];
    va_list a; va_start(a,format); _vsnprintf_s(text,sizeof(text),_TRUNCATE,format,a); va_end(a);
    {
        std::lock_guard<std::mutex> hold(FeedLock);
        FeedItem item={(long long)time(NULL),kind,text};
        Feed.push_back(item);
        while(Feed.size()>FeedMost) Feed.pop_front();
    }
    PartyBusy();
    PartyNews();
}
void PartyNews() {
    { std::lock_guard<std::mutex> hold(PartyNewsLock); ++PartyVersion; }
    PartyNewsCome.notify_all();
}
// The version once it isn't seen any more, or after ms.
unsigned long long WaitForParty(unsigned long long seen, int ms) {
    std::unique_lock<std::mutex> hold(PartyNewsLock);
    PartyNewsCome.wait_for(hold,std::chrono::milliseconds(ms),[seen]() { return PartyVersion!=seen; });
    return PartyVersion;
}
// Who signed in on NetDiscParty.xex (PartySignIn): on each console (by its
// address), the guest of a seat there, the controller (0-3) and the Xbox
// profile. Only those still in a seat on that console count (SignedIn), so
// a new draw or a swap needs no clearing. Under PartyLock.
struct PartySignInRecord { std::string name, gamertag; int controller; unsigned long long xuid; };
std::map<std::string,std::vector<PartySignInRecord> > SignIns;
// What counts of it now: for each seat on the console address, its record (NULL: not signed in).
std::vector<const PartySignInRecord*> SignedIn(const Party& p, const std::string& address) {
    std::vector<const PartySignInRecord*> out;
    for(size_t i=0;i<p.seats.size();++i) if(p.seats[i].first==address) {
        const std::vector<PartySignInRecord>& had=SignIns[address];
        for(size_t k=0;k<p.seats[i].second.size();++k) {
            const PartySignInRecord* found=NULL;
            for(size_t r=0;r<had.size() && !found;++r) if(SameName(had[r].name,p.seats[i].second[k])) found=&had[r];
            out.push_back(found);
        }
    }
    return out;
}
// The consoles whose NetDiscParty.xex waited on the server lately (PartyWait),
// with when (Now()): those show the wheel, so don't need it opened.
std::mutex WheelSeenLock;
std::map<std::string,double> WheelSeen;
const double WheelSeenSeconds = 40;
// The consoles OpenWheel had Aurora start the wheel on, with when: Aurora is
// closing there for a moment, so its FTP server is left alone.
std::map<std::string,double> WheelOpened;  // Under WheelSeenLock.
bool WheelOpening(const std::string& address, double seconds) {
    std::lock_guard<std::mutex> hold(WheelSeenLock);
    std::map<std::string,double>::const_iterator at=WheelOpened.find(address);
    return at!=WheelOpened.end() && Now()-at->second<seconds;
}
bool ShowsWheel(const std::string& address) {
    std::lock_guard<std::mutex> hold(WheelSeenLock);
    std::map<std::string,double>::const_iterator at=WheelSeen.find(address);
    return at!=WheelSeen.end() && Now()-at->second<WheelSeenSeconds;
}
// Which build of NetDiscParty.xex each console runs (its NETDISC_BUILD_STAMP,
// sent with PartyWait; 0 from one older than that), and the build each was
// last told to make way for this server's (WheelUpdate). Under WheelSeenLock.
std::map<std::string,unsigned> WheelBuild, WheelUpdateAsked;
bool WheelAppBuiltIn=false;  // This server carries the wheel app of its own build (not Docker's download).
// Whether the app on a console is older than the one this server would put
// there, and it hasn't been told so for that build yet: it then goes back to
// Aurora by itself (asking PartyEdit "reopen"), and OpenWheel puts the new one there.
bool WheelUpdate(const std::string& address) {
    if(!WheelAppBuiltIn || !NETDISC_BUILD_STAMP) return false;
    std::lock_guard<std::mutex> hold(WheelSeenLock);
    std::map<std::string,unsigned>::const_iterator b=WheelBuild.find(address), a=WheelUpdateAsked.find(address);
    return b!=WheelBuild.end() && b->second && b->second<NETDISC_BUILD_STAMP && (a==WheelUpdateAsked.end() || a->second!=b->second);
}
// The guests who joined on a console's party screen (PartyEdit "join"), by
// their name in lower case: the console's address. Under PartyLock.
std::map<std::string,std::string> JoinedOn;
std::string PartyText(unsigned long long version, const std::string& address);
void VoteJson(web::Json& j);
std::string PartyEdit(const std::string& peer, const std::string& text, std::string& said);
bool ChangeParty(const web::Value& v, std::string& error);
// The consoles to open the wheel on, for a spin or "open on all" from
// console from: the party's (all: every console that answers), not from,
// not showing it already, and in Aurora (not in a game).
std::vector<Console*> WheelToOpen(const std::string& from, bool all);
// Opens the wheel on each of them, on threads of their own (OpenWheel).
void OpenWheels(const std::vector<Console*>& consoles);
bool OpenPartyEverywhere(const std::string& from, int& opened, std::string& error);
// How long the wheel waits before it turns when it was opened on the
// consoles first: Aurora closes, the app starts and finds the server.
enum { OpenLeadMs = 12000 };
bool StartSpin(bool pick, int teams, const std::string& title, int seed, const std::vector<std::string>* consoles, int leadMs,
               Spin& out, std::string& note, std::string& error, const std::vector<std::pair<std::string,int> >* among = NULL);

// A line about one console: "Xbox 192.168.1.30: ...".
void XboxLog(const Console& console, const char* format, ...) {
    char text[2048];
    va_list a; va_start(a,format); _vsnprintf_s(text,sizeof(text),_TRUNCATE,format,a); va_end(a);
    Log("Xbox %s: %s",console.text,text);
}

const size_t ConsoleEventKeep = 5;
// An event about one console: in the activity list as "text on <address>"
// (or as global, if given), and as text under the console's own card.
void XboxEvent(const Console& console, int kind, const std::string& text, const std::string& global = std::string()) {
    Event(kind,"%s",(global.empty() ? text+" on "+console.text : global).c_str());
    Happening h = {0, (long long)time(NULL), kind, text};
    std::lock_guard<std::mutex> hold(ConsoleLock);
    console.recent.push_back(h);
    if(console.recent.size()>ConsoleEventKeep) console.recent.pop_front();
}

// Play history ----------------------------------------------------------------
//
// How often and how long each game was played, by its listed name (without
// .iso, in lower case), kept in plays.ini next to the settings file (in
// memory only without one): a game counts from its mount until the console
// is back in Aurora, goes offline, or mounts another game. A mount of
// another disc of the same game (a disc swap) goes on with it.

struct Played { int times; double seconds; long long last; };
std::mutex PlaysLock;
std::map<std::string,Played> Plays;  // Under PlaysLock.

std::string PlaysPath(const std::string& configPath) {
    std::string map=DiscMapPath(configPath);
    return map.empty() ? std::string() : map.substr(0,map.size()-9)+"plays.ini";
}
std::string UsersPath(const std::string& configPath) {
    std::string map=DiscMapPath(configPath);
    return map.empty() ? std::string() : map.substr(0,map.size()-9)+"users.ini";
}

// The users brought up to date with what they're kept from (under GuestLock;
// stats read before, as StatsLock isn't taken under it): every guest on the
// party's list and every profile made is a user, and each user's profile
// and stats are the records' (GuestProfiles, GuestStatsList). A user whose
// line changed gets "changed" now.
void RefreshUsers(const Party& party, const std::vector<GuestStats>& stats) {
    long long now=(long long)time(NULL);
    for(size_t i=0;i<party.guests.size();++i) UserFor(party.guests[i].name);
    for(size_t i=0;i<GuestProfiles.size();++i) if(!GuestProfiles[i].xuid.empty()) UserFor(GuestProfiles[i].name);
    for(size_t i=0;i<Users.size();++i) {
        User& u=Users[i];
        if(u.removed) continue;
        GuestProfile* p=GuestProfileNamed(GuestProfiles,u.name);
        if(p && !p->xuid.empty()) { u.gamertag=p->gamertag; u.xuid=p->xuid; u.made=p->made; u.adopted=p->adopted; }
        else { u.gamertag.clear(); u.xuid.clear(); u.made=0; u.adopted=false; }
        for(size_t k=0;k<stats.size();++k) {
            if(!SameName(stats[k].name,u.name)) continue;
            const GuestStats& g=stats[k];
            u.gamerscore=g.gamerscore; u.achievements=g.achievements; u.readAt=g.readAt; u.playSeconds=g.playSeconds;
            u.plays=g.plays; u.parties=g.parties; u.lastParty=g.lastParty; u.lastGame=g.lastGame;
            break;
        }
        std::string content=UserContent(u);
        std::string& saved=UsersSaved[u.id];
        if(saved!=content) { u.changed=std::max(now,u.changed+1); saved=content; }
    }
}
// A user's profile into the records (under GuestLock): the record of the
// same XUID takes the user's name (renamed on another computer), and the
// record named takes the profile. Which consoles have it is this computer's.
void UserIntoRecords(const User& u) {
    if(u.xuid.empty()) return;  // Not made yet: the record (if any) says what's planned.
    for(size_t i=0;i<GuestProfiles.size();++i)
        if(GuestProfiles[i].xuid==u.xuid && !SameName(GuestProfiles[i].name,u.name) && !GuestProfileNamed(GuestProfiles,u.name)) GuestProfiles[i].name=u.name;
    GuestProfile* p=GuestProfileNamed(GuestProfiles,u.name);
    if(!p) { GuestProfile fresh; fresh.name=u.name; GuestProfiles.push_back(fresh); p=&GuestProfiles.back(); }
    if(p->xuid!=u.xuid) { p->on.clear(); p->stale.clear(); p->playedOn.clear(); p->madeOn.clear(); }
    p->gamertag=u.gamertag; p->xuid=u.xuid; p->made=u.made; p->adopted=u.adopted;
}
// A user's stats into GuestStatsList (under StatsLock).
GuestStats& StatsOf(const std::string& name);
void UserIntoStats(const User& u) {
    if(u.gamerscore<0 && u.achievements<0 && !u.readAt && !u.playSeconds && !u.plays && !u.parties && !u.lastParty && u.lastGame.empty()) return;
    GuestStats& g=StatsOf(u.name);
    g.gamerscore=u.gamerscore; g.achievements=u.achievements; g.readAt=u.readAt; g.playSeconds=u.playSeconds;
    g.plays=u.plays; g.parties=u.parties; g.lastParty=u.lastParty; g.lastGame=u.lastGame;
}
// users.ini into the records (LoadConfig); the first time there's none, the
// users are made from the party's guest list and the records at the next Save.
void LoadUsers(const std::string& configPath) {
    std::string path=UsersPath(configPath), text;
    std::vector<User> users;
    bool had=!path.empty() && config::ReadFile(path,text,16<<20);
    if(had) ReadUsers(text,users);
    {
        std::lock_guard<std::mutex> hold(GuestLock);
        Users=users;
        UsersSaved.clear();
        UsersWritten=had ? text : std::string();
        for(size_t i=0;i<Users.size();++i) {
            UsersSaved[Users[i].id]=UserContent(Users[i]);
            if(!Users[i].removed) UserIntoRecords(Users[i]);
        }
    }
    std::lock_guard<std::mutex> hold(StatsLock);
    for(size_t i=0;i<users.size();++i) if(!users[i].removed) UserIntoStats(users[i]);
}
// users.ini written (Save), if it changed.
void SaveUsers(const std::string& configPath, const Party& party) {
    std::vector<GuestStats> stats=GuestStatsNow();
    std::string path=UsersPath(configPath), text;
    {
        std::lock_guard<std::mutex> hold(GuestLock);
        RefreshUsers(party,stats);
        text=UsersText(Users);
        if(path.empty() || text==UsersWritten) return;
        UsersWritten=text;
    }
    if(!config::WriteFile(path,text,false)) Log("Couldn't save the users in %s.",config::FromUtf8(path).c_str());
}

// plays.ini: a line "times|seconds|last|name" for each game (last: time()).
void LoadPlays(const std::string& configPath) {
    std::string path=PlaysPath(configPath), text;
    if(path.empty() || !config::ReadFile(path,text,16<<20)) return;
    std::lock_guard<std::mutex> hold(PlaysLock);
    for(size_t at=0;at<text.size();) {
        size_t end=text.find('\n',at);
        std::string line=text.substr(at,end==std::string::npos ? std::string::npos : end-at);
        at=end==std::string::npos ? text.size() : end+1;
        if(!line.empty() && line[line.size()-1]=='\r') line.erase(line.size()-1);
        Played p={0,0,0};
        int name=0;
        if(sscanf(line.c_str(),"%d|%lf|%lld|%n",&p.times,&p.seconds,&p.last,&name)<3 || !name || !line[name]) continue;
        Plays[line.substr(size_t(name))]=p;
    }
}

void SavePlays() {
    std::string path=PlaysPath(Current().configPath), text;
    if(path.empty()) return;
    {
        std::lock_guard<std::mutex> hold(PlaysLock);
        for(std::map<std::string,Played>::const_iterator p=Plays.begin();p!=Plays.end();++p) {
            char line[64]; _snprintf_s(line,sizeof(line),_TRUNCATE,"%d|%.0f|%lld|",p->second.times,p->second.seconds,p->second.last);
            text+=line+p->first+"\n";
        }
    }
    if(!config::WriteFile(path,text,false)) Log("Couldn't save the play history to %s.",config::FromUtf8(path).c_str());
}

// Has the dashboard read the games again: what a console has of them (its
// tiles, its installed games, its title updates) changed, or which consoles
// their title updates are compared on.
void GamesChanged() {
    std::lock_guard<std::mutex> hold(GamesListLock);
    ++GamesVersion;
}

// Saves the play history, and has the dashboard read the games again.
void PlaysChanged() {
    SavePlays();
    GamesChanged();
}

// Ends the game console plays, adding its time; true if there was one.
bool PlayEnded(Console& console) {
    std::string game;
    double since;
    {
        std::lock_guard<std::mutex> hold(ConsoleLock);
        game.swap(console.game);
        since=console.gameSince;
        console.gameTitle=0;
    }
    if(game.empty()) return false;
    {
        std::lock_guard<std::mutex> hold(PlaysLock);
        Plays[Lower(game)].seconds+=std::max(0.0,Now()-since);
    }
    PlaysChanged();
    GuestsPlayed(console.text,std::max(0.0,Now()-since),game);
    return true;
}

// console mounted game (its listed name), or started it again from the disc.
// The guests signed in on this console's party screen play there now: their
// copies on the other consoles fall behind (PlaysOn).
void PlayersPlayOn(const std::string& address) {
    Party p=CurrentParty();
    std::vector<PartySignInRecord> here;
    {
        std::lock_guard<std::mutex> hold(PartyLock);
        std::vector<const PartySignInRecord*> in=SignedIn(p,address);
        for(size_t i=0;i<in.size();++i) if(in[i]) here.push_back(*in[i]);
    }
    for(size_t i=0;i<here.size();++i) PlaysOn(here[i].name,here[i].xuid,address);
}
void PlayStarted(Console& console, const std::string& name, DWORD titleId) {
    std::string game=config::ToUtf8(WithoutIso(name));
    {
        std::lock_guard<std::mutex> hold(ConsoleLock);
        if(!console.game.empty() && titleId && console.gameTitle==titleId) return;  // Its other disc.
    }
    PlayEnded(console);
    {
        std::lock_guard<std::mutex> hold(ConsoleLock);
        console.game=game; console.gameTitle=titleId; console.gameSince=Now();
    }
    console.gameEndedAt=0;
    PlayersPlayOn(console.text);
    {
        std::lock_guard<std::mutex> hold(PlaysLock);
        Played& p=Plays[Lower(game)];
        ++p.times;
        p.last=(long long)time(NULL);
    }
    PlaysChanged();
}

// Turns a console off (through its debug monitor, xbdm) autoOffMinutes after
// its latest game ended, if it's still in Aurora and on then. Once a game.
void AutoOff(Console& c, const Options& o) {
    long long ended=c.gameEndedAt;
    if(!o.autoOffMinutes || !ended || Now()*1000-ended<o.autoOffMinutes*60000.0) return;
    if(c.busy>0) return;  // Not while this server works on it over FTP: once that's done.
    c.gameEndedAt=0;
    if(!c.inAurora || c.offline) return;
    DWORD address=c.address;
    int minutes=o.autoOffMinutes, port=o.xbdmPort;
    std::thread([address,minutes,port]() {
        Console* console=ConsoleIfKnown(address);
        std::string error;
        if(!console) return;
        if(Xbdm(address,port,"shutdown",error)) {
            XboxLog(*console,"turned off, %s after its last game.",Count(minutes,"minute","minutes").c_str());
            XboxEvent(*console,EventXbox,"Turned off after its last game");
        } else XboxLog(*console,"couldn't be turned off after its last game: %s.",error.c_str());
    }).detach();
}

// A game started from the dashboard that NetISO doesn't serve (a copy
// installed on the console) shows nothing here while it runs, so its end is
// told by Aurora's FTP server: tried (without logging in) FirstProbeSeconds
// after the start, then every GameProbeSeconds, it stops answering once the
// game runs, and answers again back in Aurora. Then what's installed on the
// console and its title updates are looked at again (Console::recheck). A
// start whose game never seemed to run (one quit within GameProbeSeconds, or
// one that didn't start) counts as back after LaunchGiveUpSeconds.
const double LaunchGiveUpSeconds = 120;
void FollowLaunch(Console& c, int port) {
    c.probeAt=(long long)((Now()+GameProbeSeconds)*1000);
    FtpClient ftp(c.address,port,"","",3);
    if(!ftp.Greets()) { c.launchLeft=true; return; }
    long long started=c.launchedAt;
    if(!started || (!c.launchLeft && Now()*1000-started<LaunchGiveUpSeconds*1000)) return;
    c.launchedAt=0;
    c.ftp=2;
    c.recheck=true;
    if(c.launchLeft) XboxLog(c,"back in Aurora after the game started from the dashboard (its FTP server answers).");
}

// Tells when a console in a game is back in Aurora: its FTP port is tried
// (without logging in) FirstProbeSeconds after the mount and then every
// GameProbeSeconds, only while it's in a game, and answering means Aurora
// runs again. A game started from the disc in Aurora shows by its reads;
// one started from the dashboard, by FollowLaunch.
// The live feed's gamerscore (Marcus 2026-11-02: "Ola earned 20 G in Halo
// 3" on the big screen as it happens): every LiveEverySeconds, each party
// console's Aurora (its web server answers in a game too) says its
// signed-in profiles' gamerscore; a guest's that rose is news. And Turn off
// all when idle: the party's consoles turn off once none of them played or
// did anything for the party for its idleOffMinutes.
bool PowerOn(Console& console, const std::string& action, std::string& error);
void CloseVoteIfDue();
std::map<std::string,int> LiveScores;  // By XUID (upper case): the gamerscore seen last.
void LiveLook(Console& c, const Options& o, const std::string& game) {
    AuroraWeb aurora(c.address,o.auroraWebPort,o.auroraWebUser,o.auroraWebPassword,4);
    std::string body;
    web::Value v;
    if(!aurora.Get("/profile","/getProfileInfo",body) || !web::Parse(body,v) || v.type!=web::Value::Array) return;
    std::vector<GuestProfile> records=GuestProfilesNow();
    for(size_t i=0;i<v.items.size();++i) {
        const web::Value* x=v.items[i].Get("xuid");
        const web::Value* score=v.items[i].Get("gamerscore");
        const web::Value* in=v.items[i].Get("signedin");
        if(!x || x->type!=web::Value::String || !score || score->type!=web::Value::Number || (in && in->type==web::Value::Number && in->number==0)) continue;
        std::string xuid;
        if(!OfflineXuid(x->text,xuid)) continue;
        const GuestProfile* guest=NULL;
        for(size_t k=0;k<records.size() && !guest;++k) if(records[k].xuid==xuid) guest=&records[k];
        if(!guest) continue;
        int now=int(score->number), before=-1;
        {
            std::lock_guard<std::mutex> hold(StatsLock);
            std::map<std::string,int>::iterator seen=LiveScores.find(xuid);
            if(seen!=LiveScores.end()) before=seen->second;
            LiveScores[xuid]=now;
            GuestStats& g=StatsOf(guest->name);
            if(g.partyStart<0) g.partyStart=g.gamerscore>=0 ? g.gamerscore : now;
            if(before<0 && g.gamerscore>=0) before=g.gamerscore;
            g.gamerscore=std::max(g.gamerscore,now);
        }
        if(before>=0 && now>before) {
            if(game.empty()) PartyFeed("score","%s earned %d G",guest->name.c_str(),now-before);
            else PartyFeed("score","%s earned %d G in %s",guest->name.c_str(),now-before,game.c_str());
        }
    }
}
void LiveLoop() {
    double next=0;
    bool off=false;  // Turned off for this idle stretch already.
    for(;;Sleep(2000)) {
        Party p=CurrentParty();
        Options o=Current();
        std::vector<Console*> consoles;
        for(size_t i=0;i<p.consoles.size();++i) { Console* c=ConsoleIfKnown(inet_addr(p.consoles[i].c_str())); if(c) consoles.push_back(c); }
        bool playing=false;
        std::vector<Console*> on;
        {
            std::lock_guard<std::mutex> hold(ConsoleLock);
            for(size_t i=0;i<consoles.size();++i) {
                Console& c=*consoles[i];
                if(c.offline || !(c.answers || c.connections>0)) continue;
                on.push_back(&c);
                if(!c.game.empty() || c.launchedAt) playing=true;
            }
        }
        if(playing) PartyBusy();
        CloseVoteIfDue();  // A game vote whose time is up closes (and the wheel spins).
        if(!p.active || Now()<next) {} else {
            next=Now()+LiveEverySeconds;
            for(size_t i=0;i<on.size();++i) {
                std::string game;
                { std::lock_guard<std::mutex> hold(ConsoleLock); game=on[i]->game; }
                LiveLook(*on[i],o,game);
            }
        }
        // Turn off all when idle.
        long long busyAt=PartyActiveAt.load();
        if(!busyAt) { PartyBusy(); continue; }
        bool idle=p.idleOffMinutes>0 && !on.empty() && Now()-double(busyAt)>=p.idleOffMinutes*60.0;
        if(!idle) { off=false; continue; }
        if(off) continue;
        off=true;
        Log("LAN party: no one played or did anything for the party for %s, so its consoles turn off (Turn off all when idle).",Count(p.idleOffMinutes,"minute","minutes").c_str());
        Event(EventInfo,"The party's consoles turned off after %s idle",Count(p.idleOffMinutes,"minute","minutes").c_str());
        for(size_t i=0;i<on.size();++i) {
            std::string error;
            if(!PowerOn(*on[i],"off",error)) XboxLog(*on[i],"didn't turn off when idle: %s",error.c_str());
        }
    }
}
void AuroraLoop() {
    for(;;) {
        Options o=Current();
        int port=o.ftpPort;
        std::vector<Console*> consoles=AllConsoles();
        for(size_t i=0;i<consoles.size();++i) {
            Console& c=*consoles[i];
            AutoOff(c,o);
            if(c.offline || Now()*1000<c.probeAt) continue;
            std::string game;
            { std::lock_guard<std::mutex> hold(ConsoleLock); game=WithoutIso(c.mounted); }
            // Not a NetISO game (its tile's disc mounts it, and that's told below).
            if(c.launchedAt && (c.connections<=0 || c.inAurora || game.empty())) { FollowLaunch(c,port); continue; }
            if(c.connections<=0 || c.inAurora || game.empty()) continue;
            c.probeAt=(long long)((Now()+GameProbeSeconds)*1000);
            FtpClient ftp(c.address,port,"","",3);
            if(!ftp.Greets()) continue;
            c.sentInAurora=c.sent.load();
            c.inAurora=true;
            c.ftp=2;
            XboxLog(c,"back in Aurora after %s (its FTP server answers).",game.c_str());
            c.recheck=true;
            c.launchedAt=0;
            PlayEnded(c);
            GuestStatsSoon(c.text,20);
            c.gameEndedAt=(long long)(Now()*1000);
            XboxEvent(c,EventXbox,"Back in Aurora after "+game,std::string(c.text)+" is back in Aurora after "+game);
        }
        Sleep(1000);
    }
}

// A ping, for PingLoop: a TCP connect to port at address, answered when it's
// accepted or refused (a reset, which comes only from a console that's on).
// The Xbox answers no ICMP ping (seen on the console), in Aurora or in a
// game. The round trip in ms, or -1 without an answer within timeoutMs.
int Knock(DWORD address, int port, int timeoutMs) {
    SOCKET s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if(s==INVALID_SOCKET) return -1;
    u_long nonblocking=1;
    ioctlsocket(s,FIONBIO,&nonblocking);
    sockaddr_in to; memset(&to,0,sizeof(to)); to.sin_family=AF_INET; to.sin_addr.s_addr=address; to.sin_port=htons(u_short(port));
#ifdef _WIN32
    const int refused=WSAECONNREFUSED, refusedNow=WSAECONNREFUSED;
#else
    const int refused=ECONNREFUSED, refusedNow=20000+ECONNREFUSED;  // WSAGetLastError's code for it (posix_compat.h).
#endif
    double started=Now();
    int status=connect(s,reinterpret_cast<sockaddr*>(&to),sizeof(to)) ? WSAGetLastError() : 0;
    bool answered=!status || status==refusedNow;
    while(status==WSAEWOULDBLOCK && (Now()-started)*1000<timeoutMs) {
        fd_set writable,failed; FD_ZERO(&writable); FD_ZERO(&failed); FD_SET(s,&writable); FD_SET(s,&failed);
        timeval interval={0,100000};
        int rc=select(0,NULL,&writable,&failed,&interval);
        if(rc==SOCKET_ERROR) break;
        if(rc==0) continue;
        int error=0; int length=sizeof(error);
        if(getsockopt(s,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&length)) break;
        answered=!error || error==refused;
        break;
    }
    if(answered && !status) shutdown(s,SD_SEND);
    closesocket(s);
    return answered ? int((Now()-started)*1000+0.5) : -1;
}

// The keepalive of a NetISO connection to a console that stopped answering
// pings (quick): a probe a second, so a connection to a console that is gone
// ends within seconds, and one whose network still answers stays. quick
// false puts back the usual timing.
void QuickKeepalive(SOCKET s, bool quick) {
#ifdef _WIN32
    tcp_keepalive k; k.onoff=1; k.keepalivetime=quick ? 1000 : 7200000; k.keepaliveinterval=1000;
    DWORD bytes;
    WSAIoctl(s,SIO_KEEPALIVE_VALS,&k,sizeof(k),NULL,0,&bytes,NULL,NULL);
#else
    int idle=quick ? 1 : 7200, interval=quick ? 1 : 75, count=quick ? 5 : 9;
    unsigned int timeout=quick ? 10000 : 0;  // Also for a reply it never acknowledged.
    setsockopt(s,IPPROTO_TCP,TCP_KEEPIDLE,&idle,sizeof(idle));
    setsockopt(s,IPPROTO_TCP,TCP_KEEPINTVL,&interval,sizeof(interval));
    setsockopt(s,IPPROTO_TCP,TCP_KEEPCNT,&count,sizeof(count));
    setsockopt(s,IPPROTO_TCP,TCP_USER_TIMEOUT,&timeout,sizeof(timeout));
#endif
}

void QuickKeepalives(Console& c, bool quick) {
    std::lock_guard<std::mutex> hold(ConsoleLock);
    for(std::set<SOCKET>::const_iterator s=c.sockets.begin();s!=c.sockets.end();++s) QuickKeepalive(*s,quick);
}

// Its connections' keepalive back to the usual, once it shows it's on.
void NotSuspect(Console& c) {
    if(!c.suspectAt) return;
    c.suspectAt=0;
    QuickKeepalives(c,false);
}

void GoOffline(Console& c, long long now) {
    c.offlineAt=now;
    c.offline=true;
    c.launchedAt=0;  // Back online, it's looked at again (BackOnline).
    PlayEnded(c);
    c.gameEndedAt=0;
    XboxLog(c,"offline: the last %d pings got no answer%s.",OfflinePings,c.suspectAt ? ", and its NetISO connection ended" : "");
    c.suspectAt=0;
    XboxEvent(c,EventXbox,"Went offline",std::string(c.text)+" went offline");
}

void BackOnline(Console& c, const char* how) {
    c.offline=false;
    c.missed=0;
    c.recheck=true;
    XboxLog(c,"back online (%s).",how);
    XboxEvent(c,EventXbox,"Back online",std::string(c.text)+" is back online");
}

// Pings each console (Knock, on Aurora's FTP port) every pingSeconds (--ping)
// to tell when one is switched off: a console that's off sends NetISO nothing, and its NetISO connection
// would only end after the system's TCP keepalive (two hours on Windows), so
// it showed as connected all that time. A console that sent NetISO a request
// within pingSeconds isn't pinged. One that answered pings before and then
// misses OfflinePings in a row is offline, once its NetISO connections end:
// they get a quick keepalive (QuickKeepalive), which ends them within seconds
// if the console is gone. A connection that outlasts it shows the console is
// on, though not answering pings. A console that never answered a ping is
// never offline.
// Each console's own name (see ConsoleName in aurora.h), read while it's on
// (it answers pings, uses NetISO or Aurora's FTP server answers): when it's
// not known yet, NameRetrySeconds after a try that failed, and every
// NameEverySeconds besides, so a name changed on the console shows.
const double NameEverySeconds = 600, NameRetrySeconds = 60;
bool LookName(Console& c, const Options& o) {
    std::string name, id, error;
    c.nameAt=(long long)(Now()*1000);
    if(!ConsoleName(c.address,o.namePort,name,id,error)) return false;
    std::string was;
    { std::lock_guard<std::mutex> hold(ConsoleLock); was=c.name; c.name=name; c.id=id; }
    if(was!=name) XboxLog(c,"its own name (Console Settings on it) is %s.",name.c_str());
    return true;
}
// Its name as the page shows it: its own, or as last read (kept in the
// settings file); "" if never read.
std::string NameOf(const Console& c) {
    {
        std::lock_guard<std::mutex> hold(ConsoleLock);
        if(!c.name.empty()) return c.name;
    }
    std::lock_guard<std::mutex> hold(SettingsLock);
    for(size_t k=0;k<Known.size();++k) if(Known[k].address==c.text) return Known[k].name;
    return std::string();
}
void NamesLoop() {
    for(;;) {
        Options o=Current();
        std::vector<Console*> consoles=AllConsoles();
        for(size_t i=0;i<consoles.size();++i) {
            Console& c=*consoles[i];
            long long now=(long long)(Now()*1000);
            bool on=(c.answers && !c.offline) || (c.seenAt>=0 && now-c.seenAt<60000) || c.ftp==2;
            if(!on) continue;
            bool known;
            { std::lock_guard<std::mutex> hold(ConsoleLock); known=!c.name.empty(); }
            if(c.nameAt && now-c.nameAt<(long long)(1000*(known ? NameEverySeconds : NameRetrySeconds))) continue;
            LookName(c,o);
        }
        Sleep(2000);
    }
}

// Shows "This is <name> (<address>)" on the console's screen (see
// ConsoleNotify in aurora.h): when Aurora starts (see PushTo), with
// showName, and from the dashboard's Identify button.
bool ShowName(Console& c, const Options& o, std::string& error) {
    std::string name=NameOf(c);
    if(name.empty() && LookName(c,o)) name=NameOf(c);
    std::string text=name.empty() ? std::string("This is ")+c.text : "This is "+name+" ("+c.text+")";
    return ConsoleNotify(c.address,o.xbdmPort,text,error);
}

void PingLoop() {
    for(;;) {
        Options o=Current();
        double every=o.pingSeconds;
        std::vector<Console*> consoles=AllConsoles();
        for(size_t i=0;i<consoles.size() && every>0;++i) {
            Console& c=*consoles[i];
            long long now=(long long)(Now()*1000);
            if(c.offline && c.seenAt>c.offlineAt) BackOnline(c,"it sent a NetISO request");
            if(c.suspectAt) {
                if(c.seenAt>c.suspectAt) { NotSuspect(c); c.missed=0; }
                else if(c.connections<=0) GoOffline(c,now);
                else if(now-c.suspectAt>=SuspectSeconds*1000) {
                    if(!c.toldNoPings.exchange(true)) XboxLog(c,"doesn't answer pings, but its NetISO connection does, so it's on.");
                    NotSuspect(c);
                    c.missed=0;
                }
                continue;
            }
            if(now<c.pingAt) continue;
            bool idle=now-std::max(c.seenAt.load(),c.answeredAt.load())>=IdleSeconds*1000;
            c.pingAt=now+(long long)(1000*(idle && !c.answers ? IdleProbeSeconds : every));
            if(c.seenAt>=0 && now-c.seenAt<every*1000) { c.missed=0; continue; }  // NetISO shows it's on.
            int ms=Knock(c.address,o.ftpPort,PingTimeoutMs);
            if(ms>=0) {
                c.pingMs=ms; c.answers=true; c.answered=true; c.missed=0; c.toldNoPings=false;
                c.answeredAt=(long long)(Now()*1000);
                if(c.offline) BackOnline(c,"it answers pings again");
                continue;
            }
            c.answers=false;
            if(++c.missed>=OfflinePings && c.answered && !c.offline) {
                if(c.connections<=0) GoOffline(c,now);
                else { c.suspectAt=now; QuickKeepalives(c,true); }
            }
        }
        Sleep(1000);
    }
}

// Each second, what each console was sent in it, for the dashboard's chart.
void RateLoop() {
    for(;;) {
        long long second=(long long)Now();
        std::vector<Console*> consoles=AllConsoles();
        {
            std::lock_guard<std::mutex> hold(ConsoleLock);
            if(second!=RateSecond) {
                double total=0;
                for(size_t i=0;i<consoles.size();++i) {
                    Console& c=*consoles[i];
                    U64 sent=c.sent;
                    double rate=sent>=c.counted ? double(sent-c.counted) : 0;
                    c.counted=sent;
                    c.rate[second%RateSeconds]=rate; c.rateSecond[second%RateSeconds]=second;
                    total+=rate;
                }
                Peak=std::max(Peak,total);
                RateSecond=second;
            }
        }
        Sleep(1000);
    }
}

// Keeps Windows awake (see system.h) while a console plays a game from this
// server, and for AwakeSeconds after the last read: a game whose disc goes
// with the computer sleeping crashes. Then Windows may sleep again.
#ifdef _WIN32
const double AwakeSeconds = 600;
void AwakeLoop() {
    bool awake=false;
    U64 counted=0;
    double readAt=-AwakeSeconds;
    for(;;) {
        U64 sent=0;
        bool playing=false;
        std::vector<Console*> consoles=AllConsoles();
        {
            std::lock_guard<std::mutex> hold(ConsoleLock);
            for(size_t i=0;i<consoles.size();++i) {
                const Console& c=*consoles[i];
                sent+=c.sent;
                playing=playing || (c.connections>0 && !c.mounted.empty() && !c.inAurora && !c.offline);
            }
        }
        if(sent!=counted) { counted=sent; readAt=Now(); }
        bool want=Current().keepAwake && (playing || Now()-readAt<AwakeSeconds);
        if(want!=awake) {
            KeepAwake(want);
            awake=want;
            if(want) Log("Keeping this computer awake while a console plays.");
            else Log("This computer may sleep again: no console has played for %.0f minutes.",AwakeSeconds/60);
        }
        Sleep(5000);
    }
}
#endif

// One client connection, speaking the protocol as tuxuser/netiso-srv does
// (src/main.rs), except that every request and reply is read and sent whole,
// the ISO list is read again whenever a client starts listing (index 0), so
// new ISOs show up without a restart, an empty mount name mounts nothing, and
// a read past the end gets zeros there instead of a closed connection.
class Connection {
public:
    Connection(SOCKET socket, DWORD address) : socket_(socket), address_(address), console_(NULL), pastEnd_(false) {
        AddressText(address,peer_,sizeof(peer_));
        quiet_=Loopback(address) && !Settings.verbose;  // The push's own connection.
    }
    ~Connection() {
        if(console_) {
            std::lock_guard<std::mutex> hold(ConsoleLock);
            console_->sockets.erase(socket_);
        }
        closesocket(socket_);
        if(console_) --console_->connections;
    }
    void Run() {
        if(!quiet_) Log("%s connected.",peer_);
        unsigned char request[Header];
        while(ReceiveAll(request,Header)) {
            if(memcmp(request,"ISVR",4)) { Log("%s sent something that isn't a NetISO request; closing.",peer_); break; }
            int command=(request[4]<<8)|request[5];
            DWORD index=DWORD((request[6]<<8)|request[7]);
            U64 offset=(U64(Read32(request+8))<<32)|Read32(request+12);
            DWORD length=Read32(request+16);
            if(Settings.verbose) Log("%s: command %d, index %u, offset %llu, length %u",peer_,command,index,(unsigned long long)offset,length);
            // NetDiscParty.xex's own: it isn't the console's disc drive, so
            // its connection doesn't count as one (Attach).
            if(command==PartyWait || command==PartySpinAsk || command==PartySignIn || command==PartyEditAsk || command==PartyJobAsk) {
                if(!PartyRequest(command,index,offset,length)) break;
                continue;
            }
            if(!console_) Attach();
            if(console_) console_->seenAt=(long long)(Now()*1000);
            if(!Handle(command,index,offset,length)) break;
        }
        if(!quiet_) Log("%s disconnected.",peer_);
    }
private:
    // Its console, found by its first NetISO request: anything else that
    // connects (a port scan, say) isn't taken for an Xbox, which would get
    // the FTP login on its next push.
    void Attach() {
        bool added;
        console_=ConsoleAt(address_,&added);
        if(!console_) return;
        console_->seenAt=(long long)(Now()*1000);
        // A first connection is Aurora (re)starting, its NetISO plugin listing
        // the games: its installed games may have changed meanwhile.
        if(++console_->connections==1) console_->recheck=true;
        {
            std::lock_guard<std::mutex> hold(ConsoleLock);
            console_->sockets.insert(socket_);
        }
        if(added) {
            if(Current().push && console_->push) Log("Found an Xbox at %s; its game folders are kept up to date.",console_->text);
            else Log("Found an Xbox at %s.",console_->text);
            XboxEvent(*console_,EventXbox,"Connected for the first time",std::string("New console at ")+console_->text);
        }
    }
    bool ReceiveAll(unsigned char* data, DWORD count) {
        while(count) {
            int got=recv(socket_,reinterpret_cast<char*>(data),int(count),0);
            if(got<=0) return false;
            data+=got; count-=DWORD(got);
        }
        return true;
    }
    bool SendAll(const unsigned char* data, size_t count) {
        while(count) {
            int part=count>(1<<20) ? (1<<20) : int(count);
            int sent=send(socket_,reinterpret_cast<const char*>(data),part,0);
            if(sent<=0) return false;
            data+=sent; count-=size_t(sent);
        }
        return true;
    }
    bool Send32(DWORD value) { unsigned char reply[4]; Write32(reply,value); return SendAll(reply,4); }
    // NetDiscParty.xex (party/main.cpp): PartyWait sends the party as text
    // (PartyText) once its version isn't offset any more (at once for 0),
    // or after length milliseconds (at most PartyWaitMost); PartySpinAsk
    // spins the wheel from the Gamemaster's console: index 1 for teams
    // (length of them; 0: the party's), 2 to pick one guest. Each reply is
    // its length (4 bytes) and then text: the party, or "ok", or "error" and
    // a sentence after a tab. PartySignIn says a guest signed in on the
    // console's NetDiscParty.xex: index is the seat (0-3) and the controller
    // (0-3) times 256, with 0x8000 when the controller signed out instead;
    // offset is the profile's XUID; length bytes of its gamertag follow the
    // request. PartyWait's index is the app's build (NETDISC_BUILD_STAMP; 0
    // from older ones). PartyEditAsk changes the party from a console: length
    // bytes of text follow (a command and its fields, tab-separated: see
    // PartyEdit); the reply is "ok" (a sentence after a tab, maybe) or "error".
    enum { PartyWaitMost = 30000, GamertagBytesMost = 64, SignedOut = 0x8000, EditBytesMost = 512, JobBytesMost = 16384 };
    bool SendText(const std::string& text) { return Send32(DWORD(text.size())) && SendAll(reinterpret_cast<const unsigned char*>(text.data()),text.size()); }
    bool PartyRequest(int command, DWORD index, U64 offset, DWORD length) {
        // The guests' names go to the Xboxes only (as --xbox and the
        // settings' xboxes list them, else anything but this computer).
        std::string gamertag, edit;
        if(command==PartyEditAsk) {
            if(length>EditBytesMost) { Log("%s sent a party change of %u bytes; closing.",peer_,length); return false; }
            std::vector<unsigned char> text(length+1);
            if(length && !ReceiveAll(text.data(),length)) return false;
            edit.assign(reinterpret_cast<const char*>(text.data()),length);
        }
        if(command==PartyJobAsk) {
            if(length>JobBytesMost) { Log("%s sent %u bytes about a profile job; closing.",peer_,length); return false; }
            std::vector<unsigned char> text(length+1);
            if(length && !ReceiveAll(text.data(),length)) return false;
            edit.assign(reinterpret_cast<const char*>(text.data()),length);
        }
        if(command==PartySignIn) {
            // Read first: it's part of the request, whatever the answer.
            if(length>GamertagBytesMost) { Log("%s sent a gamertag of %u bytes; closing.",peer_,length); return false; }
            char text[GamertagBytesMost];
            if(length && !ReceiveAll(reinterpret_cast<unsigned char*>(text),length)) return false;
            gamertag.assign(text,length);
            for(size_t i=0;i<gamertag.size();++i) if((unsigned char)gamertag[i]<32) gamertag[i]=' ';
        }
        if(!FromXbox(address_)) return SendText("error\tThis isn't one of NetDisc360's Xboxes.\n");
        if(command==PartyJobAsk) return SendText(PartyAppJobAnswer(peer_,int(index),edit));
        if(command==PartyEditAsk) {
            std::string said, error=PartyEdit(peer_,edit,said);
            return SendText(error.empty() ? "ok\t"+said+"\n" : "error\t"+error+"\n");
        }
        if(command==PartyWait) {
            { std::lock_guard<std::mutex> hold(WheelSeenLock); WheelSeen[peer_]=Now(); WheelBuild[peer_]=index; }
            unsigned long long version=WaitForParty(offset,int(std::min<DWORD>(length,PartyWaitMost)));
            { std::lock_guard<std::mutex> hold(WheelSeenLock); WheelSeen[peer_]=Now(); }
            return SendText(PartyText(version,peer_));
        }
        if(command==PartySignIn) {
            std::string error=SignIn(int(index&0xFF),int((index>>8)&0x7F),(index&SignedOut)!=0,offset,gamertag);
            return SendText(error.empty() ? std::string("ok\n") : "error\t"+error+"\n");
        }
        Party p=CurrentParty();
        std::string error, note;
        Spin spin;
        if(p.gamemaster!=peer_) error="Only the Gamemaster's console spins the wheel.";
        else if(index!=1 && index!=2) error="Ask for teams or one guest.";
        else if(index==1 && length && (length<TeamsLeast || length>TeamsMost)) error="Draw 2, 3 or 4 teams.";
        else {
            // Spun on the console: the party starts if it hadn't, and the
            // wheel opens on its other consoles that are in Aurora (they get
            // a long lead to open it).
            if(!p.active) {
                web::Value change; change.type=web::Value::Object;
                web::Value on; on.type=web::Value::Bool; on.boolean=true;
                change.fields.push_back(std::make_pair(std::string("active"),on));
                if(!ChangeParty(change,error)) return SendText("error\t"+error+"\n");
            }
            std::vector<Console*> others=WheelToOpen(peer_,false);
            if(StartSpin(index==2,int(length),std::string(),-1,NULL,others.empty() ? int(SpinLeadMs) : int(OpenLeadMs),spin,note,error)) {
                Log("%s spun the raffle wheel.",peer_);
                OpenWheels(others);
            }
        }
        return SendText(error.empty() ? std::string("ok\n") : "error\t"+error+"\n");
    }
    // A guest signed in (or a controller signed out) on this console's
    // NetDiscParty.xex; "" or why not.
    std::string SignIn(int seat, int controller, bool out, unsigned long long xuid, const std::string& gamertag) {
        if(controller>3) return "There are four controllers.";
        std::string name;
        {
            std::lock_guard<std::mutex> hold(PartyLock);
            std::vector<PartySignInRecord>& had=SignIns[peer_];
            if(out) {
                for(size_t i=had.size();i-->0;) if(had[i].controller==controller) had.erase(had.begin()+i);
            } else {
                for(size_t i=0;i<TheParty.seats.size() && name.empty();++i)
                    if(TheParty.seats[i].first==peer_ && seat>=0 && size_t(seat)<TheParty.seats[i].second.size()) name=TheParty.seats[i].second[seat];
                if(name.empty()) return "No one sits in that seat on this console.";
                for(size_t i=had.size();i-->0;) if(had[i].controller==controller || SameName(had[i].name,name)) had.erase(had.begin()+i);
                PartySignInRecord r; r.name=name; r.gamertag=gamertag; r.controller=controller; r.xuid=xuid;
                had.push_back(r);
            }
        }
        PartyBusy();
        if(out) Log("%s: controller %d signed out on the party screen.",peer_,controller+1);
        else Log("%s: %s signed in on controller %d (%s).",peer_,name.c_str(),controller+1,gamertag.empty() ? "no gamertag" : gamertag.c_str());
        if(!out && xuid) { AdoptGuestProfile(name,xuid,gamertag,peer_); PlaysOn(name,xuid,peer_); }
        PartyNews();
        // Their progress here goes to the other consoles while they're away.
        if(out) PushSeatProfilesSoon();
        return std::string();
    }
    bool Handle(int command, DWORD index, U64 offset, DWORD length) {
        switch(command) {
        case Ping: return SendAll(reinterpret_cast<const unsigned char*>("ISVRokOK"),8);
        case GetIsoSize: {
            unsigned char reply[8];
            Write32(reply,iso_.IsOpen() ? DWORD(iso_.Size()/IsvrSector) : 0); Write32(reply+4,IsvrSector);
            return SendAll(reply,8);
        }
        case HasType1File: return Send32(0);
        case ReadData: {
            // Like netiso-srv, nothing mounted gets no reply. Bytes past the
            // end are zeros: netiso-srv closes the connection, which takes the
            // disc away from a game that read 4 KB at an ISO's last sector.
            if(!iso_.IsOpen()) return true;
            if(length>MaxReadBytes) { Log("%s asked for %u bytes at once; closing.",peer_,length); return false; }
            U64 size=iso_.Size();
            DWORD inside=offset>=size ? 0 : DWORD(std::min<U64>(length,size-offset));
            buffer_.resize(length);
            if(inside && !iso_.Read(offset,buffer_.data(),inside)) { Log("Reading %s failed; closing %s.",mounted_.c_str(),peer_); return false; }
            if(inside<length) {
                memset(buffer_.data()+inside,0,length-inside);
                if(!pastEnd_) Log("%s asked for bytes %llu-%llu of a %llu-byte disc; sending zeros past its end.",peer_,(unsigned long long)offset,(unsigned long long)(offset+length),(unsigned long long)size);
                pastEnd_=true;
            }
            if(!SendAll(buffer_.data(),length)) return false;
            // A mount counts as a play once the game reads: NetISO mounts
            // its last disc again as the console starts, which reads little.
            readSinceMount_+=length;
            if(playPending_ && readSinceMount_>=PlayBytes) { playPending_=false; PlayStarted(*console_,mounted_,mountedTitle_); }
            if(console_) {
                console_->sent+=length;
                if(console_->inAurora && console_->sent-console_->sentInAurora>RelaunchBytes) {  // In a game again.
                    console_->inAurora=false;
                    PlayStarted(*console_,mounted_,mountedTitle_);
                    console_->probeAt=(long long)((Now()+GameProbeSeconds)*1000);
                }
            }
            return true;
        }
        case GetIsoName: {
            if(length>MaxNameBytes) return false;
            if(!index || index>listing_.size()) listing_=LatestGames();
            std::vector<unsigned char> reply(length,0);
            if(index<listing_.size()) memcpy(reply.data(),listing_[index].name.data(),std::min<size_t>(length,listing_[index].name.size()));
            return SendAll(reply.data(),reply.size());
        }
        case MountIso: {
            if(length>MaxNameBytes) return false;
            std::vector<unsigned char> raw(length);
            if(length && !ReceiveAll(raw.data(),length)) return false;
            return Send32(Mount(std::string(raw.begin(),raw.end())));
        }
        default:
            Log("%s sent unknown command %d; closing.",peer_,command);
            return false;
        }
    }
    DWORD Mount(const std::string& raw) {
        // "\<iso>\Mount\" from the plugin's folders, or a bare name.
        std::string name;
        for(size_t i=0;i<raw.size();) {
            if(!raw.compare(i,6,"\\Mount")) { i+=6; continue; }
            if(raw[i]!='\\' && raw[i]) name+=raw[i];
            ++i;
        }
        if(name=="[Disable Current ISO]") {
            if(!quiet_) Log("%s: unmounting %s.",peer_,mounted_.empty() ? "nothing" : mounted_.c_str());
            iso_.Close(); mounted_.clear();
            Mounted();
            return 0;
        }
        std::vector<IsoFile> isos=LatestGames();
        const IsoFile* found=Find(isos,name);
        if(!found && !name.empty() && MayLookNow()) {  // Added since the latest look, perhaps.
            isos=Isos();
            found=Find(isos,name);
            ScanSoon=true;
        } else if(!found && !name.empty()) ScanSoon=true;
        if(!found) { Log("%s asked to mount \"%s\", which isn't here.",peer_,name.c_str()); return 0; }
        if(!iso_.Open(*found)) { Log("%s: couldn't open %s: %s.",peer_,found->path.c_str(),iso_.Note().c_str()); mounted_.clear(); Mounted(); return 0; }
        mounted_=found->name; mountedTitle_=found->titleId; pastEnd_=false;
        readSinceMount_=0; playPending_=console_!=NULL;
        Mounted();
        if(console_) { console_->inAurora=false; console_->probeAt=(long long)((Now()+FirstProbeSeconds)*1000); ++GameStarts; }
        if(!quiet_) {
            if(found->kind==DiscIso && iso_.Note()=="ISO") Log("%s: mounted %s.",peer_,found->name.c_str());
            else Log("%s: mounted %s (%s).",peer_,found->name.c_str(),iso_.Note().c_str());
            if(console_) XboxEvent(*console_,EventPlay,WithoutIso(found->name)+" started");
            else Event(EventPlay,"%s started on %s",WithoutIso(found->name).c_str(),peer_);
        }
        return 1;
    }
    static const IsoFile* Find(const std::vector<IsoFile>& isos, const std::string& name) {
        const IsoFile* found=NULL;
        for(size_t i=0;i<isos.size() && !found && !name.empty();++i) if(!_stricmp(isos[i].name.c_str(),name.c_str())) found=&isos[i];
        // netiso-srv takes the first ISO whose name ends with the request.
        for(size_t i=0;i<isos.size() && !found && !name.empty();++i)
            if(isos[i].name.size()>=name.size() && !_stricmp(isos[i].name.c_str()+isos[i].name.size()-name.size(),name.c_str())) found=&isos[i];
        return found;
    }
    // The console's disc is the one its latest mount asked for.
    void Mounted() {
        if(!console_) return;
        std::lock_guard<std::mutex> hold(ConsoleLock);
        console_->mounted=mounted_;
    }
    SOCKET socket_;
    DWORD address_;
    Console* console_;
    char peer_[16];
    bool quiet_;
    std::vector<IsoFile> listing_;
    IsoReader iso_;
    std::string mounted_;
    DWORD mountedTitle_=0;
    U64 readSinceMount_=0;
    bool playPending_=false;  // The mount isn't counted as a play yet.
    std::vector<unsigned char> buffer_;
    bool pastEnd_;  // A read past the end of this mount was logged.
};

// Connections at once from one address. A console uses a few (NetISO, a
// launcher, the plugin's disc swap); many more is something else on the
// network, which would otherwise get a thread for each. This PC's own (the
// push's) aren't counted.
enum { MaxConnectionsFrom = 16 };
std::mutex OpenLock;
std::map<DWORD,int> OpenFrom;

void Serve(SOCKET listener) {
    long long refusedAt=0;
    for(;;) {
        sockaddr_in from; AddressLength size=sizeof(from);
        SOCKET client=accept(listener,reinterpret_cast<sockaddr*>(&from),&size);
        if(client==INVALID_SOCKET) { Sleep(100); continue; }
        DWORD address=from.sin_addr.s_addr;
        bool counted=!Loopback(address);
        if(counted) {
            std::lock_guard<std::mutex> hold(OpenLock);
            if(OpenFrom[address]>=MaxConnectionsFrom) {
                closesocket(client);
                long long now=(long long)Now();
                if(now-refusedAt>=60) {
                    refusedAt=now;
                    char text[16]; AddressText(address,text,sizeof(text));
                    Log("Refused a connection from %s: it has %d open already.",text,int(MaxConnectionsFrom));
                }
                continue;
            }
            ++OpenFrom[address];
        }
        int on=1;
        setsockopt(client,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&on),sizeof(on));
        setsockopt(client,SOL_SOCKET,SO_KEEPALIVE,reinterpret_cast<const char*>(&on),sizeof(on));
        // An Xbox already known is seen at once (its network is up); a new
        // one once it sends a request (Connection::Attach).
        if(Console* known=ConsoleIfKnown(address)) known->seenAt=(long long)(Now()*1000);
        std::thread([client,address,counted]() {
            Connection(client,address).Run();
            if(counted) {
                std::lock_guard<std::mutex> hold(OpenLock);
                if(!--OpenFrom[address]) OpenFrom.erase(address);
            }
        }).detach();
    }
}

// Aurora's Xbox 360 category -------------------------------------------------

// Aurora files the launchers as Unsigned, since they aren't retail-signed,
// and its "Xbox 360" category shows only retail games. Its filter is a Lua
// script, so the category gets one more rule. An Aurora update puts the
// original back, so it is checked whenever the Xbox comes back.
const char FilterScript[] = "/Media/Scripts/Content/FilterFunctions.lua";
const char FilterOriginal[] =
    "function GameListFilterXbox360(Content)\n"
    "   -- Add only legit xbox 360 games\n"
    "   return Content.Group == ContentGroup.Xbox360\n"
    "end";
const char FilterNetIsoBefore[] =  // As servers before the install disc filter wrote it.
    "function GameListFilterXbox360(Content)\n"
    "   -- Add only legit xbox 360 games\n"
    "   return Content.Group == ContentGroup.Xbox360 or GameListFilterNetIso(Content)\n"
    "end\n"
    "\n"
    "-- NetDisc 360: the game launchers in Hdd1:\\NetISO play retail discs\n"
    "-- from the NetISO server, so they count as Xbox 360 games here. Aurora files\n"
    "-- them as Unsigned because they aren't retail-signed. The NetISO Sync app\n"
    "-- itself stays out.\n"
    "function GameListFilterNetIso(Content)\n"
    "   local directory = '\\\\' .. string.lower(Content.Directory or '')\n"
    "   return string.find(directory, '\\\\netiso\\\\', 1, true) ~= nil and string.find(directory, '\\\\netiso\\\\netiso sync', 1, true) == nil\n"
    "end";
// The server knows its own edit by these exact words, so they keep the name
// as it was then ("NetDisc 360").
const char FilterNetIso[] =
    "function GameListFilterXbox360(Content)\n"
    "   -- Add only legit xbox 360 games\n"
    "   return Content.Group == ContentGroup.Xbox360 or GameListFilterNetIso(Content)\n"
    "end\n"
    "\n"
    "-- NetDisc 360: the game launchers in Hdd1:\\NetISO play retail discs\n"
    "-- from the NetISO server, so they count as Xbox 360 games here. Aurora files\n"
    "-- them as Unsigned because they aren't retail-signed. The NetISO Sync app\n"
    "-- itself stays out, and so do install discs, which their game's tile\n"
    "-- starts (NetDiscInstallDisc is in User\\Scripts\\Content\\Filters).\n"
    "function GameListFilterNetIso(Content)\n"
    "   local directory = '\\\\' .. string.lower(Content.Directory or '')\n"
    "   return string.find(directory, '\\\\netiso\\\\', 1, true) ~= nil and string.find(directory, '\\\\netiso\\\\netiso sync', 1, true) == nil\n"
    "      and not (NetDiscInstallDisc ~= nil and NetDiscInstallDisc(Content))\n"
    "end";

std::string Lines(const char* text, bool crlf) {
    std::string out;
    for(const char* p=text;*p;++p) { if(*p=='\n' && crlf) out+='\r'; out+=*p; }
    return out;
}

// Writes data to path and reads it back: a connection lost during the upload
// leaves the file cut short, which for Aurora's own script would break it.
bool StoreChecked(FtpClient& ftp, const std::string& path, const std::string& data) {
    std::vector<unsigned char> back;
    // As bytes: a char above 0x7F isn't equal to the same unsigned char.
    return ftp.Store(path.c_str(),data.data(),data.size()) && ftp.Retrieve(path.c_str(),back,data.size()+1) &&
           back.size()==data.size() && (data.empty() || !memcmp(&back[0],data.data(),data.size()));
}

void GuardFilter(const Console& console, FtpClient& ftp, bool announce, const Options& o) {
    std::string path=o.aurora+FilterScript;
    std::vector<unsigned char> data;
    if(!ftp.Retrieve(path.c_str(),data,1<<20)) {
        if(announce) XboxLog(console,"couldn't read Aurora's %s (%s); the Xbox 360 category is left alone. --aurora names Aurora's folder.",path.c_str(),ftp.Error());
        return;
    }
    std::string text(data.begin(),data.end());
    bool crlf=text.find("\r\n")!=std::string::npos;
    if(text.find(Lines(FilterNetIso,crlf))!=std::string::npos) {
        if(announce) XboxLog(console,"Aurora's Xbox 360 category includes the NetISO games.");
        return;
    }
    // An earlier server's edit gets this one's, without a new backup.
    std::string before=Lines(FilterNetIsoBefore,crlf);
    size_t at=text.find(before);
    if(at!=std::string::npos) {
        std::string edited=text.substr(0,at)+Lines(FilterNetIso,crlf)+text.substr(at+before.size());
        if(StoreChecked(ftp,path,edited))
            XboxLog(console,"Aurora's Xbox 360 category now leaves install discs out (%s). Restart Aurora to see it.",path.c_str());
        else {
            XboxLog(console,"couldn't update Aurora's Xbox 360 category: %s",ftp.Error());
            StoreChecked(ftp,path,text);
        }
        return;
    }
    if(text.find("GameListFilterNetIso")!=std::string::npos) {
        if(announce) XboxLog(console,"Aurora's Xbox 360 category has a NetISO rule this program doesn't know, so it is left alone.");
        return;
    }
    std::string original=Lines(FilterOriginal,crlf);
    std::string backup=path+".before-netiso";
    at=text.find(original);
    if(at==std::string::npos) {
        // Our edit cut short by a lost connection: written again in full
        // from the original kept beside it.
        std::vector<unsigned char> kept;
        if(!text.empty() && ftp.Retrieve(backup.c_str(),kept,1<<20)) {
            std::string first(kept.begin(),kept.end());
            bool keptCrlf=first.find("\r\n")!=std::string::npos;
            std::string firstOriginal=Lines(FilterOriginal,keptCrlf);
            size_t from=first.find(firstOriginal);
            if(from!=std::string::npos) {
                std::string whole=first.substr(0,from)+Lines(FilterNetIso,keptCrlf)+first.substr(from+firstOriginal.size());
                if(text.size()<whole.size() && !whole.compare(0,text.size(),text)) {
                    if(StoreChecked(ftp,path,whole)) XboxLog(console,"Aurora's %s was cut short; wrote it again in full.",path.c_str());
                    else XboxLog(console,"Aurora's %s is cut short, and writing it again failed: %s",path.c_str(),ftp.Error());
                    return;
                }
            }
        }
        if(announce) XboxLog(console,"Aurora's Xbox 360 filter isn't the one this program knows, so it is left alone; the NetISO games show under All.");
        return;
    }
    std::string edited=text.substr(0,at)+Lines(FilterNetIso,crlf)+text.substr(at+original.size());
    if(!StoreChecked(ftp,backup,text) || !StoreChecked(ftp,path,edited)) {
        XboxLog(console,"couldn't update Aurora's Xbox 360 category: %s",ftp.Error());
        XboxEvent(console,EventWarning,"Couldn't add the games to Aurora's Xbox 360 category");
        StoreChecked(ftp,path,text);  // The original back, should the edit have got there cut short.
        return;
    }
    XboxLog(console,"added the NetISO games to Aurora's Xbox 360 category (%s; the original is kept as %s). Restart Aurora to see it.",path.c_str(),backup.c_str());
    XboxEvent(console,EventGood,"Games added to Aurora's Xbox 360 category; restart Aurora to see them there",
              std::string("Games added to Aurora's Xbox 360 category on ")+console.text+"; restart Aurora to see them there");
}

// Install and content discs (see discmap.h) are started from their game's
// tile, which offers the install, so Aurora needn't list their own tiles.
// This user filter of ours hides them (Filters, User, "Hide install discs"),
// and the Xbox 360 category leaves them out (FilterNetIso). They are told
// apart by Title ID and Media ID, which Aurora reads from their launchers.
const char InstallFilterScript[] = "/User/Scripts/Content/Filters/NetDiscInstallDiscs.lua";
std::string InstallFilter() {
    std::map<DWORD,std::set<DWORD> > discs;
    {
        std::lock_guard<std::mutex> hold(GamesListLock);
        for(size_t i=0;i<GamesList.size();++i) {
            const IsoFile& g=GamesList[i];
            if((g.role=="install" || g.role=="content") && g.titleId && g.mediaId) discs[g.titleId].insert(g.mediaId);
        }
    }
    if(discs.empty()) return std::string();
    std::string text=
        "-- Written by NetDisc360, which keeps it up to date: the install discs\r\n"
        "-- and content discs of its games, by Title ID and Media ID. Each is\r\n"
        "-- started from its game's tile, which offers the install, so this filter\r\n"
        "-- hides their own tiles. Aurora's Xbox 360 category leaves them out too.\r\n"
        "NetDiscInstallDiscs = {\r\n";
    for(std::map<DWORD,std::set<DWORD> >::const_iterator t=discs.begin();t!=discs.end();++t) {
        char line[64]; _snprintf_s(line,sizeof(line),_TRUNCATE,"   [0x%08X] = {",t->first);
        text+=line;
        for(std::set<DWORD>::const_iterator m=t->second.begin();m!=t->second.end();++m) {
            _snprintf_s(line,sizeof(line),_TRUNCATE," [0x%08X] = true,",*m);
            text+=line;
        }
        text+=" },\r\n";
    }
    text+=
        "}\r\n"
        "\r\n"
        "-- Aurora may hand IDs over as signed numbers.\r\n"
        "local function NetDiscId(id)\r\n"
        "   if id ~= nil and id < 0 then return id + 4294967296 end\r\n"
        "   return id\r\n"
        "end\r\n"
        "\r\n"
        "function NetDiscInstallDisc(Content)\r\n"
        "   local discs = NetDiscInstallDiscs[NetDiscId(Content.TitleId)]\r\n"
        "   return discs ~= nil and discs[NetDiscId(Content.MediaId)] == true\r\n"
        "end\r\n"
        "\r\n"
        "GameListFilterCategories.User[\"Hide install discs\"] = function(Content)\r\n"
        "   return not NetDiscInstallDisc(Content)\r\n"
        "end\r\n";
    return text;
}
// Aurora's "Recently added" filter (Filters, User): the games the server
// first saw in the last RecentDays days (NoteFirstSeen), by Title ID and
// Media ID like the install disc filter. Written with the game folders;
// Aurora reads it when it starts.
const char RecentFilterScript[] = "/User/Scripts/Content/Filters/NetDiscRecent.lua";
const int RecentDays = 30;
std::string RecentFilter() {
    std::set<std::string> recent=SeenSince((long long)time(NULL)-RecentDays*86400LL);
    std::map<DWORD,std::set<DWORD> > games;
    {
        std::lock_guard<std::mutex> hold(GamesListLock);
        for(size_t i=0;i<GamesList.size();++i) {
            const IsoFile& g=GamesList[i];
            if(g.titleId && g.mediaId && recent.count(g.name)) games[g.titleId].insert(g.mediaId);
        }
    }
    char head[512];
    _snprintf_s(head,sizeof(head),_TRUNCATE,
        "-- Written by NetDisc360, which keeps it up to date: the games it first\r\n"
        "-- saw in the last %d days, by Title ID and Media ID. Aurora reads it\r\n"
        "-- when it starts.\r\n"
        "NetDiscRecentGames = {\r\n",RecentDays);
    std::string text=head;
    for(std::map<DWORD,std::set<DWORD> >::const_iterator t=games.begin();t!=games.end();++t) {
        char line[64]; _snprintf_s(line,sizeof(line),_TRUNCATE,"   [0x%08X] = {",t->first);
        text+=line;
        for(std::set<DWORD>::const_iterator m=t->second.begin();m!=t->second.end();++m) {
            _snprintf_s(line,sizeof(line),_TRUNCATE," [0x%08X] = true,",*m);
            text+=line;
        }
        text+=" },\r\n";
    }
    text+=
        "}\r\n"
        "\r\n"
        "-- Aurora may hand IDs over as signed numbers.\r\n"
        "local function NetDiscRecentId(id)\r\n"
        "   if id ~= nil and id < 0 then return id + 4294967296 end\r\n"
        "   return id\r\n"
        "end\r\n"
        "\r\n"
        "GameListFilterCategories.User[\"Recently added\"] = function(Content)\r\n"
        "   local discs = NetDiscRecentGames[NetDiscRecentId(Content.TitleId)]\r\n"
        "   return discs ~= nil and discs[NetDiscRecentId(Content.MediaId)] == true\r\n"
        "end\r\n";
    return text;
}
int RefreshRemote(FtpClient& ftp, const std::string& path, const std::vector<unsigned char>& data);
void EnsureFolder(FtpClient& ftp, const std::string& folder);
void RefreshRecentFilter(const Console& console, FtpClient& ftp, const Options& o) {
    std::string text=RecentFilter();
    std::string path=o.aurora+RecentFilterScript;
    if(!ftp.Enters(o.aurora.c_str())) return;  // GuardFilter tells about that.
    // An Aurora that has no user filters yet has no folder for them either.
    const std::string script=RecentFilterScript;
    for(size_t at=1;(at=script.find('/',at))!=std::string::npos;++at) EnsureFolder(ftp,o.aurora+script.substr(0,at));
    int written=RefreshRemote(ftp,path,std::vector<unsigned char>(text.begin(),text.end()));
    if(written>0) XboxLog(console,"wrote Aurora's \"Recently added\" filter (%s); Aurora shows the change after it restarts (Filters, User).",path.c_str());
    else if(written<0) XboxLog(console,"couldn't write %s: %s",path.c_str(),ftp.Error());
}
void RefreshInstallFilter(const Console& console, FtpClient& ftp, const Options& o) {
    std::string text=InstallFilter();
    if(text.empty()) return;
    std::string path=o.aurora+InstallFilterScript;
    int written=RefreshRemote(ftp,path,std::vector<unsigned char>(text.begin(),text.end()));
    if(written>0) XboxLog(console,"wrote Aurora's \"Hide install discs\" filter (%s). Restart Aurora, then pick it under Filters, User.",path.c_str());
    else if(written<0) XboxLog(console,"couldn't write %s: %s",path.c_str(),ftp.Error());
}

// Aurora's art for NetDisc360, its party app and FastFTP360 ---------------------------------

// Aurora looks a title's cover up on XboxUnity by its Title ID, and XboxUnity
// has no entry for these homebrew apps (only its admins add homebrew
// titles), so Aurora keeps empty placeholders for them in its GameData
// folder: <Title ID>_<n>/GC (cover), BK (background) and GL (icon and banner)
// .asset files of 2048 bytes. The server fills those in with the art from
// art/aurora (see tools/aurora_asset.py). Art that is already there, whether
// Aurora's or someone's own, is left alone.
struct AppArt { DWORD titleId; const char* app; const char* folder; int ids[3]; std::vector<unsigned char> files[3]; };
AppArt AppArts[] = {
    {0xFFFF4E49,"NetDisc360","netdisc360",{IDR_ART_NETDISC_GC,IDR_ART_NETDISC_BK,IDR_ART_NETDISC_GL}},
    {0xFFFF4646,"FastFTP360","fastftp360",{IDR_ART_FASTFTP_GC,IDR_ART_FASTFTP_BK,IDR_ART_FASTFTP_GL}},
    {0xFFFF4E50,"NetDisc360 Party","netdiscparty",{IDR_ART_PARTY_GC,IDR_ART_PARTY_BK,IDR_ART_PARTY_GL}},
};
const char* const ArtKinds[3] = {"GC","BK","GL"};
const size_t ArtPlaceholder = 2048;  // An .asset file with no picture in it: the header alone.
const char GameDataFolder[] = "/Data/GameData";

bool BuiltIn(int id, std::vector<unsigned char>& out);
bool LoadFile(const std::string& path, std::vector<unsigned char>& out);
// From the exe's resources on Windows, or else from an aurora folder beside
// the launcher given with --launcher (in the Docker image,
// /usr/local/share/netdisc/aurora; see the Dockerfile).
void LoadAppArt(const Options& o) {
    std::string folder=o.launcherPath.substr(0,o.launcherPath.find_last_of("/\\")+1)+"aurora/";
    for(size_t a=0;a<sizeof(AppArts)/sizeof(AppArts[0]);++a)
        for(int k=0;k<3;++k) {
            AppArt& art=AppArts[a];
            char name[32]; _snprintf_s(name,sizeof(name),_TRUNCATE,"%s%08X.asset",ArtKinds[k],unsigned(art.titleId));
            if(!BuiltIn(art.ids[k],art.files[k]) && !o.launcherPath.empty()) LoadFile(folder+art.folder+"/"+name,art.files[k]);
        }
}

void GuardAppArt(const Console& console, FtpClient& ftp, const Options& o) {
    std::string root=o.aurora+GameDataFolder;
    std::vector<FtpEntry> entries;
    // Enters keeps the connection when the folder isn't there.
    if(!ftp.Enters(root.c_str()) || !ftp.List(root.c_str(),entries)) return;  // Aurora keeps its folder elsewhere; GuardFilter says so.
    for(size_t i=0;i<entries.size();++i) {
        unsigned titleId=0, number=0;
        char rest;
        if(!entries[i].folder || sscanf(entries[i].name.c_str(),"%8X_%8X%c",&titleId,&number,&rest)!=2) continue;
        for(size_t a=0;a<sizeof(AppArts)/sizeof(AppArts[0]);++a) {
            const AppArt& art=AppArts[a];
            if(art.titleId!=titleId) continue;
            int written=0;
            for(int k=0;k<3;++k) {
                if(art.files[k].empty()) continue;
                char name[32]; _snprintf_s(name,sizeof(name),_TRUNCATE,"/%s%08X.asset",ArtKinds[k],titleId);
                std::string path=root+"/"+entries[i].name+name;
                std::vector<unsigned char> old, back;
                // Bigger than a placeholder (or unreadable): there's art already.
                if(!ftp.Retrieve(path.c_str(),old,ArtPlaceholder) && !ftp.Missing()) continue;
                const std::vector<unsigned char>& data=art.files[k];
                if(ftp.Store(path.c_str(),data.data(),data.size()) && ftp.Retrieve(path.c_str(),back,data.size()+1) && back==data) ++written;
                else {
                    XboxLog(console,"couldn't write %s: %s",path.c_str(),ftp.Error());
                    if(!old.empty()) ftp.Store(path.c_str(),old.data(),old.size());  // The placeholder back, rather than a cut-short file.
                }
            }
            if(written) {
                XboxLog(console,"wrote %s's cover art to Aurora (%s/%s). Restart Aurora to see it.",art.app,root.c_str(),entries[i].name.c_str());
                XboxEvent(console,EventGood,std::string(art.app)+"'s cover art added to Aurora; restart Aurora to see it");
            }
        }
    }
}

// Pushing the game folders ---------------------------------------------------

// Sync keeps its lists in static buffers (it runs on the console too), so the
// consoles' threads push one at a time.
std::mutex SyncLock;
const Console* Pushing;  // The console Sync is writing to, for PushNote (under SyncLock).
std::string LastPushNote;  // Sync's latest note, the likely reason it stopped (under SyncLock).
void PushNote(const char* text) { LastPushNote=text; XboxLog(*Pushing,"%s",text); }
void PushProgress(int done, int total) { Pushing->pushTotal=total; Pushing->pushDone=done; }

// Writes data to path on the Xbox unless it already holds exactly that.
// 1 if written, 0 if current, -1 on failure.
int RefreshRemote(FtpClient& ftp, const std::string& path, const std::vector<unsigned char>& data) {
    std::vector<unsigned char> old;
    if(ftp.Retrieve(path.c_str(),old,data.size()+1) && old==data) return 0;
    return ftp.Store(path.c_str(),data.data(),data.size()) ? 1 : -1;
}

const char SyncFolder[]="NetISO Sync";
// The party wheel app's folder in the Xbox folder, as build.ps1 makes it (see OpenWheel).
const char WheelAppFolder[]="NetDisc360 Party";
// The LAN party app on every console, as a tile in Aurora (NetDisc360
// Party, in the Xbox folder): this server's copy, once each time it starts.
std::mutex PartyAppLock;
std::set<std::string> PartyAppPut;  // Under PartyAppLock.
void PutPartyApp(const Console& console, FtpClient& ftp, const Options& o) {
    if(WheelApp.empty()) return;
    {
        std::lock_guard<std::mutex> hold(PartyAppLock);
        if(PartyAppPut.count(console.text)) return;
    }
    std::string folder=o.xboxFolder+"/"+WheelAppFolder;
    EnsureFolder(ftp,folder);
    int wrote=RefreshRemote(ftp,folder+"/default.xex",WheelApp);
    if(wrote<0) { XboxLog(console,"couldn't put the LAN party app on it: %s",ftp.Error()); return; }
    if(wrote>0) XboxLog(console,"has the LAN party app (NetDisc360 Party) in Aurora now.");
    std::lock_guard<std::mutex> hold(PartyAppLock);
    PartyAppPut.insert(console.text);
}
// NetISO Sync's own folder, if it's there, gets this build's launcher and
// helper as they are (the launcher keeps its own Title ID, FFFF4E49): an
// older Sync can't read the launchers this server writes, and would only
// log errors.
void RefreshSyncApp(const Console& console, FtpClient& ftp, const std::vector<FtpEntry>& entries, const Options& o) {
    for(size_t i=0;i<entries.size();++i) {
        if(!entries[i].folder || _stricmp(entries[i].name.c_str(),SyncFolder)) continue;
        std::string folder=o.xboxFolder+"/"+entries[i].name;
        int launcher=RefreshRemote(ftp,folder+"/default.xex",Launcher);
        int boot=Boot.empty() ? 0 : RefreshRemote(ftp,folder+"/boot.xex",Boot);
        if(launcher<0 || boot<0) XboxLog(console,"couldn't update %s: %s",folder.c_str(),ftp.Error());
        else if(launcher || boot) {
            XboxLog(console,"updated %s to this build.",folder.c_str());
            XboxEvent(console,EventGood,"NetISO Sync updated to this version");
        }
        return;
    }
}

std::string Count(int n, const char* one, const char* many) {
    char text[64]; _snprintf_s(text,sizeof(text),_TRUNCATE,"%d %s",n,n==1 ? one : many);
    return text;
}

// What a game folder's install.ini says (see shared/library.h): a disc whose
// game has an install disc here names it and its packages; any other game
// needs none, so an install.ini left from an install disc that is gone is
// emptied.
void InstallFor(const char* iso, InstallNeed& need) {
    std::lock_guard<std::mutex> hold(GamesListLock);
    const IsoFile* game=NULL;
    for(size_t i=0;i<GamesList.size() && !game;++i) if(!_stricmp(GamesList[i].name.c_str(),iso)) game=&GamesList[i];
    if(!game) return;
    need.known=true;
    const IsoFile* install=NULL;
    for(size_t i=0;i<GamesList.size() && !install && !game->installDisc.empty();++i) if(GamesList[i].name==game->installDisc) install=&GamesList[i];
    if(!install) return;
    _snprintf_s(need.iso,sizeof(need.iso),_TRUNCATE,"%s",install->name.c_str());
    std::string lines;
    for(size_t i=0;i<install->packages.size();++i) {
        char line[IsvrNameBytes+64];
        _snprintf_s(line,sizeof(line),_TRUNCATE,"package_%d=%s|%llu\r\n",int(i+1),install->packages[i].path.c_str(),
                    (unsigned long long)install->packages[i].size);
        if(lines.size()+strlen(line)>=sizeof(need.packages)) break;
        lines+=line;
    }
    memcpy(need.packages,lines.c_str(),lines.size()+1);
}

// The games installed on the console being pushed to, by Title ID and disc,
// with the console's path to each, and its game folders as Sync tells them
// (all under SyncLock). PushSkip: installed games get no game folder.
std::map<std::pair<DWORD,int>,std::string> PushInstalled;
std::map<std::string,std::string> PushTiles;
bool PushSkip=true;

// What the disc map says a disc is, and an original Xbox game's emulator
// (see xefu.h), for its game folder's netiso.ini.
void RoleFor(const char* iso, DiscRoleInfo& info) {
    std::lock_guard<std::mutex> hold(GamesListLock);
    for(size_t i=0;i<GamesList.size();++i) {
        const IsoFile& g=GamesList[i];
        if(_stricmp(g.name.c_str(),iso)) continue;
        info.known=true;
        _snprintf_s(info.role,sizeof(info.role),_TRUNCATE,"%s",g.role.c_str());
        _snprintf_s(info.label,sizeof(info.label),_TRUNCATE,"%s",g.roleLabel.c_str());
        _snprintf_s(info.xefu,sizeof(info.xefu),_TRUNCATE,"%s",g.xefu.c_str());
        std::map<std::pair<DWORD,int>,std::string>::const_iterator local=g.titleId ? PushInstalled.find(std::make_pair(g.titleId,g.disc)) : PushInstalled.end();
        if(local!=PushInstalled.end()) _snprintf_s(info.local,sizeof(info.local),_TRUNCATE,"%s",local->second.c_str());
        return;
    }
}

// "/Hdd1/Games/Halo 3/default.xex" -> "Hdd1:\Games\Halo 3\default.xex", as the launcher takes it.
std::string ConsolePath(const std::string& ftpPath) {
    size_t slash=ftpPath.find('/',1);
    if(ftpPath.empty() || ftpPath[0]!='/' || slash==std::string::npos) return std::string();
    std::string out=ftpPath.substr(1,slash-1)+":"+ftpPath.substr(slash);
    for(size_t i=0;i<out.size();++i) if(out[i]=='/') out[i]='\\';
    return out;
}

// Whether the console gets a game folder for a disc: not when the same game
// (Title ID and disc) is installed on it, unless installed games keep theirs.
bool WantedFor(const char* iso) {
    if(PushInstalled.empty() || !PushSkip) return true;
    std::lock_guard<std::mutex> hold(GamesListLock);
    for(size_t i=0;i<GamesList.size();++i)
        if(!_stricmp(GamesList[i].name.c_str(),iso)) return !GamesList[i].titleId || !PushInstalled.count(std::make_pair(GamesList[i].titleId,GamesList[i].disc));
    return true;
}
void PlacedAt(const char* iso, const char* folder) { PushTiles[Lower(iso)]=folder; }

bool Push(Console& console, FtpClient& ftp, SyncResult& result, bool removeGone, const Options& o, const std::vector<InstalledGame>& installed) {
    std::vector<FtpEntry> entries;
    ftp.MakeFolder(o.xboxFolder.c_str());
    if(!ftp.List(o.xboxFolder.c_str(),entries)) {
        XboxLog(console,"couldn't list %s: %s",o.xboxFolder.c_str(),ftp.Error());
        XboxEvent(console,EventWarning,"Couldn't open "+o.xboxFolder);
        return false;
    }
    RefreshSyncApp(console,ftp,entries,o);
    IsvrClient client(30);
    if(!client.Connect(htonl(0x7F000001),o.port,5) || !client.Ping()) { Log("Couldn't reach this server itself: %s",client.Error()); return false; }
    FtpStorage storage(ftp);
    bool ok;
    std::string reason;
    std::map<std::string,std::string> tiles;
    {
        std::lock_guard<std::mutex> hold(SyncLock);
        Pushing=&console; LastPushNote.clear();
        PushInstalled.clear();
        PushSkip=o.skipInstalled;
        for(size_t i=0;i<installed.size();++i) {
            std::string path=ConsolePath(installed[i].path);
            if(!path.empty()) PushInstalled.insert(std::make_pair(std::make_pair(installed[i].titleId,installed[i].disc),path));
        }
        PushTiles.clear();
        ok=Sync(client,storage,o.xboxFolder.c_str(),Launcher.data(),DWORD(Launcher.size()),
                Boot.empty() ? NULL : Boot.data(),DWORD(Boot.size()),PushNote,result,removeGone,InstallFor,RoleFor,WantedFor,PlacedAt,PushProgress);
        console.pushTotal=0; console.pushDone=0;
        reason=LastPushNote;
        tiles.swap(PushTiles);
    }
    if(ok) {
        bool changed;
        { std::lock_guard<std::mutex> hold(ConsoleLock); changed=console.tiles!=tiles; console.tiles.swap(tiles); }
        if(changed) GamesChanged();
    }
    if(ok && result.failed) XboxLog(console,"game folders written: %d added, %d removed, %d kept (%d updated), %d skipped, %d file(s) failed; trying those again in %.0f s.",
                                    result.added,result.removed,result.kept,result.updated,result.skipped,result.failed,RetryFailedSeconds);
    else if(ok) XboxLog(console,"game folders up to date: %d added, %d removed, %d kept (%d updated), %d skipped.",result.added,result.removed,result.kept,result.updated,result.skipped);
    if(ok && result.installed) XboxLog(console,"%s installed on it get%s no game folder: Aurora lists the installed copy.",
                                       Count(result.installed,"game","games").c_str(),result.installed==1 ? "s" : "");
    if(ok && result.held && FoldersMissing) XboxLog(console,"%d game folder(s) have no game here; they stay until every game folder can be read again.",result.held);
    else if(ok && result.held) XboxLog(console,"%d game folder(s) have no game here yet; they're removed if that's still so %.0f s after the start or the last change of game folders.",result.held,o.removeWait);
    else if(!ok) {
        // The FTP error is the reason only when the connection went; else
        // it can be an old one Sync got past (a missing file it deleted).
        XboxLog(console,"updating the game folders stopped: %s",!ftp.Code() && ftp.Error()[0] ? ftp.Error() : !reason.empty() ? reason.c_str() : ftp.Error()[0] ? ftp.Error() : "see above");
        XboxEvent(console,EventWarning,"Updating the game folders stopped; the log says why",
                  std::string("Updating the game folders on ")+console.text+" stopped; the log says why");
    }
    // Launchers updated: game folders whose launcher was rewritten with this version.
    if(ok && (result.added || result.removed || result.updated)) {
        std::string change;
        if(result.added) change=Count(result.added,"game","games")+" added";
        if(result.removed) change+=(change.empty() ? "" : ", ")+Count(result.removed,"game","games")+" removed";
        if(result.updated) change+=(change.empty() ? "" : ", ")+Count(result.updated,"launcher","launchers")+" updated";
        XboxEvent(console,EventGood,change);
    }
    return ok;
}

// Asks Aurora to scan the game folder now, with FtpDll's SITE INITIATESCAN,
// so new games show up at once and Aurora looks them up online. A game its own
// scan finds while Aurora is starting never gets its name or art (see
// WatchLoop). False when Aurora didn't start one (its scanner may be busy).
bool StartScan(const Console& console, FtpClient& ftp, const Options& o) {
    std::string reply;
    bool ok=ftp.Site(("INITIATESCAN "+o.xboxFolder).c_str(),reply) && reply.find("Unable")==std::string::npos;
    if(ok) {
        XboxLog(console,"asked Aurora to scan %s, so it lists the changes now and downloads the new games' art.",o.xboxFolder.c_str());
        XboxEvent(console,EventInfo,"Aurora is looking up the new games",std::string("Aurora on ")+console.text+" is looking up the new games");
    }
    else XboxLog(console,"Aurora didn't start a scan: %s",reply.empty() ? ftp.Error() : reply.c_str());
    return ok;
}

// Tiles with no art ------------------------------------------------------------

// Aurora looks a game up online (its name, cover and background) only once,
// when a scan first adds its tile, and never tries again; a tile its own scan
// adds while Aurora is starting gets no lookup at all (see WatchLoop). Such a
// tile gets a fresh lookup when its folder is renamed and Aurora scans: Aurora
// drops the old tile, with its GameData folder, and adds the game as new
// (seen on the console with Banjo-Kazooie, 2026-10-17). That resets the
// tile's own Aurora data, which for these tiles is little more than the
// missing art.
//
// Which tiles have art is read from Aurora itself: its content database says
// which row is which game folder, and the row's GameData folder,
// <Title ID>_<row id>, holds its cover as GC<Title ID>.asset. Aurora keeps an
// empty one (the 2048-byte header alone) when the lookup found nothing.
const char AuroraContentDb[] = "/Data/Databases/content.db";
struct ArtlessTile { std::string folder, title; };

// Aurora's Directory for a game folder: the game folder's path without its
// drive, with backslashes ("\NetISO\Halo 3" for /Hdd1/NetISO/Halo 3).
std::string AuroraDirectory(const Options& o, const std::string& folder) {
    std::string path=o.xboxFolder;
    size_t drive=path.find('/',1);
    path=(drive==std::string::npos ? std::string() : path.substr(drive))+"/"+folder;
    std::replace(path.begin(),path.end(),'/','\\');
    return path;
}

bool PlaysNow(const Console& console) {
    std::lock_guard<std::mutex> hold(ConsoleLock);
    return console.connections>0 && !console.mounted.empty() && !console.inAurora;
}

// Whether Aurora may be restarted on console now: never while this server
// works on it over FTP (writing game folders, scripts or title updates,
// reading logs or Aurora's databases), as that would cut the work off. own
// is the caller's own sessions, which are done. Why not, in why.
bool SafeToRestart(const Console& console, int own, std::string& why) {
    if(console.busy>own) { why="NetDisc360 is still working on it over FTP"; return false; }
    return true;
}
// SafeToRestart, waiting up to seconds for that work to end (a push of game
// folders takes a few seconds): for what the dashboard asks.
bool SafeSoon(const Console& console, double seconds, std::string& why) {
    for(double until=Now()+seconds;!SafeToRestart(console,0,why);Sleep(250)) if(Now()>=until) return false;
    return true;
}

// The game folders on the console whose tile in Aurora has no cover, and
// every folder there (for new names). False, with why in error, if Aurora
// couldn't be asked.
bool FindArtless(FtpClient& ftp, const Options& o, std::vector<FtpEntry>& folders, std::vector<ArtlessTile>& out, std::string& error) {
    out.clear();
    if(!ftp.List(o.xboxFolder.c_str(),folders)) { error=std::string("Couldn't open ")+o.xboxFolder+": "+ftp.Error(); return false; }
    std::vector<unsigned char> db;
    std::string dbPath=o.aurora+AuroraContentDb;
    if(!ftp.Retrieve(dbPath.c_str(),db,size_t(256)<<20)) { error="Couldn't read Aurora's "+dbPath+": "+ftp.Error(); return false; }
    std::vector<AuroraItem> items;
    std::string why;
    if(!ReadAuroraItems(db,items,why)) { error="Couldn't read Aurora's "+dbPath+": "+why; return false; }
    std::string root=o.aurora+GameDataFolder;
    std::vector<FtpEntry> data;
    if(!ftp.List(root.c_str(),data)) {
        if(ftp.Code()!=550) { error="Couldn't open "+root+": "+ftp.Error(); return false; }
        data.clear();  // No GameData folder: no game has art yet.
    }
    std::set<std::string> withData;
    for(size_t i=0;i<data.size();++i) if(data[i].folder) {
        std::string name=data[i].name;
        for(size_t k=0;k<name.size();++k) name[k]=char(toupper((unsigned char)name[k]));
        withData.insert(name);
    }
    for(size_t f=0;f<folders.size();++f) {
        const FtpEntry& folder=folders[f];
        // NetDisc360's helper has no tile, and NetISO Sync's art isn't Aurora's to find.
        if(!folder.folder || !_stricmp(folder.name.c_str(),HelperFolder) || !_stricmp(folder.name.c_str(),SyncFolder) || !_stricmp(folder.name.c_str(),WheelAppFolder)) continue;
        std::string directory=AuroraDirectory(o,folder.name);
        const AuroraItem* item=NULL;
        for(size_t i=0;i<items.size() && !item;++i) if(!_stricmp(items[i].directory.c_str(),directory.c_str())) item=&items[i];
        if(!item || !item->titleId) continue;  // Not in Aurora yet: its first scan looks it up.
        char id[24], cover[32];
        _snprintf_s(id,sizeof(id),_TRUNCATE,"%08X_%08X",item->titleId,unsigned(item->id));
        _snprintf_s(cover,sizeof(cover),_TRUNCATE,"GC%08X.asset",item->titleId);
        bool art=false;
        std::vector<FtpEntry> files;
        if(withData.count(id)) {
            if(!ftp.List((root+"/"+id).c_str(),files)) { error="Couldn't open "+root+"/"+id+": "+ftp.Error(); return false; }
            for(size_t i=0;i<files.size() && !art;++i) art=!files[i].folder && !_stricmp(files[i].name.c_str(),cover) && files[i].size>ArtPlaceholder;
        }
        if(art) continue;
        ArtlessTile tile; tile.folder=folder.name; tile.title=item->titleName.empty() ? folder.name : item->titleName;
        out.push_back(tile);
    }
    return true;
}

// Renames each of the tiles' folders (all when only is empty, else those
// named in it) and asks Aurora to scan, so it looks them up again. The new
// name is the one Sync would give the game's disc with the old name taken:
// the game's name with or without its tags, else with a number. Sync finds
// a folder by the disc in its netiso.ini, so it keeps the new name.
void RepairArt(const Console& console, FtpClient& ftp, const Options& o, std::vector<FtpEntry>& folders, const std::vector<ArtlessTile>& tiles,
               const std::set<std::string>& only, std::vector<std::pair<std::string,std::string> >& renamed, std::string& error) {
    std::lock_guard<std::mutex> hold(SyncLock);  // Not while Sync writes the folders.
    std::vector<std::string> used;
    for(size_t i=0;i<folders.size();++i) used.push_back(folders[i].name);
    for(size_t t=0;t<tiles.size();++t) {
        const std::string& old=tiles[t].folder;
        if(!only.empty() && !only.count(old)) continue;
        std::string base=o.xboxFolder+"/"+old;
        std::vector<unsigned char> ini;
        char iso[IsvrNameBytes]="";
        if(ftp.Retrieve((base+"/netiso.ini").c_str(),ini,IniMax)) {
            ini.push_back(0);
            IniValue((const char*)ini.data(),"iso",iso,sizeof(iso));
        } else if(!ftp.Missing()) { error=std::string("Couldn't read ")+base+"/netiso.ini: "+ftp.Error(); break; }
        if(!iso[0]) _snprintf_s(iso,sizeof(iso),_TRUNCATE,"%s.iso",old.c_str());
        std::vector<char> room((used.size()+1)*(FolderNameMax+1));
        char (*taken)[FolderNameMax+1]=reinterpret_cast<char(*)[FolderNameMax+1]>(room.data());
        int count=0;
        for(size_t i=0;i<used.size();++i) if(used[i].size()<=FolderNameMax) strcpy_s(taken[size_t(count++)],FolderNameMax+1,used[i].c_str());
        char name[FolderNameMax+1];
        FolderName(iso,taken,count,name);
        if(!ftp.Rename(base.c_str(),(o.xboxFolder+"/"+name).c_str())) { error="Couldn't rename "+base+": "+ftp.Error(); break; }
        XboxLog(console,"renamed %s to %s, so Aurora looks %s up again.",base.c_str(),name,tiles[t].title.c_str());
        used.push_back(name);
        renamed.push_back(std::make_pair(old,std::string(name)));
    }
    if(renamed.empty()) return;
    XboxEvent(console,EventInfo,"Fetching art again for "+Count(int(renamed.size()),"game","games"),
              std::string("Fetching art again for ")+Count(int(renamed.size()),"game","games")+" on "+console.text);
    if(!StartScan(console,ftp,o) && error.empty()) error="The folders were renamed, but Aurora didn't start a scan. Its next scan looks them up.";
}

// What a console's push thread knows about it.
struct PushState {
    std::string pushed;  // The games when its folders were last written.
    double nextProbe, probedAt, retryAt, scanAt, downSince, upSince, installedAt;
    int scanTries;
    bool attempted, reachable, toldDown, toldWaiting;
    std::atomic<bool> everPushed;
    std::atomic<bool> finished;  // --push-once: done with this console.
    InstalledGames installed;    // What was read of its installed games.
    std::string installedSeen;   // Their Title IDs and discs, as last logged.
    bool toldInstalled;          // Looking for them failed, and the log said so.
    bool installedChanged;       // What's installed changed since its folders were last written (see Console::recheck).
    bool restarted;              // Aurora was restarted for its filter once (see RefreshAuroraScripts).
    // That restart is due, since restartSince (Now()), and waits until
    // nothing else goes on with the console (see RestartWhenSafe).
    bool restartWanted;
    double restartSince;
    std::string followed;        // The NetISO.xex.txt address the log said it left alone (see FollowAddress).
    PushState() : nextProbe(0), probedAt(0), retryAt(0), scanAt(0), downSince(0), upSince(0), installedAt(0), scanTries(0),
                  attempted(false), reachable(false), toldDown(false), toldWaiting(false), everPushed(false), finished(false),
                  toldInstalled(false), installedChanged(false), restarted(false), restartWanted(false), restartSince(0) {}
};

// The games installed on the console, for its push and the dashboard (see installed.h).
void LookInstalled(Console& console, PushState& s, FtpClient& ftp, const Options& o, std::vector<InstalledGame>& found) {
    std::string error;
    bool ok=s.installed.Find(ftp,InstalledFolders(o.installedFolders),o.xboxFolder,found,error);
    if(!ok && !s.toldInstalled)
        XboxLog(console,"couldn't look for the games installed on it (%s); %s.",error.c_str(),
                found.empty() ? "until it can, its installed games get game folders too" : "going by what it found before");
    s.toldInstalled=!ok;
    std::string seen;
    for(size_t i=0;i<found.size();++i) seen+=Number(found[i].titleId)+"-"+Number(found[i].disc)+",";
    if(ok && seen!=s.installedSeen) {
        s.installedSeen=seen;
        XboxLog(console,"%s installed on it (in %s, or as Games on Demand).",Count(int(found.size()),"game is","games are").c_str(),o.installedFolders.c_str());
    }
    bool changed;
    {
        std::lock_guard<std::mutex> hold(ConsoleLock);
        changed=console.installed.size()!=found.size();
        for(size_t i=0;i<found.size() && !changed;++i) {
            const InstalledGame& a=console.installed[i], & b=found[i];
            changed=a.titleId!=b.titleId || a.disc!=b.disc || a.path!=b.path || a.god!=b.god;
        }
        console.installed=found;
    }
    if(changed) GamesChanged();
}

// The title updates the console's Aurora has, and which it applies, for the
// dashboard (see auroradb.h): its content.db and settings.db, never
// written. Read while the server talks to the console anyway (a push, or
// its return to Aurora), and before Start on all.
//
// A file is downloaded again only when its folder's listing shows a new
// size or time, once that listing is a minute old. The listing gives the
// time to the minute only, and a database changed in place keeps its size
// (a title update put to use in Aurora), so a change later in the minute a
// file was read shows no difference. Its copy counts as the one for that
// listing only when it was read StampSeconds after the listing was first
// seen, when that minute was over on the console; until then each look
// reads the file again.
const char AuroraDatabases[] = "/Data/Databases";
const double StampSeconds = 60;
void LookTitleUpdates(Console& console, FtpClient& ftp, const Options& o) {
    std::lock_guard<std::mutex> looking(console.updatesLock);
    std::string folder=o.aurora+AuroraDatabases, error;
    std::vector<FtpEntry> entries;
    // Entered first: a folder that isn't there keeps FtpDll's connection.
    if(!ftp.Enters(folder.c_str())) error=ftp.Code()/100==5 ? "Aurora has no "+folder+" folder" : "couldn't open "+folder+": "+ftp.Error();
    else if(!ftp.List(folder.c_str(),entries)) error="couldn't list "+folder+": "+ftp.Error();
    static const char* const names[2]={"content.db","settings.db"};
    const FtpEntry* files[2]={NULL,NULL};
    for(size_t i=0;i<entries.size();++i)
        for(int k=0;k<2;++k) if(!entries[i].folder && !_stricmp(entries[i].name.c_str(),names[k])) files[k]=&entries[i];
    if(error.empty() && !files[0]) error="Aurora's content.db isn't in "+folder;
    for(int k=0;k<2 && error.empty();++k) {
        // No settings.db: no update is in use.
        std::string now=files[k] ? Number((long long)files[k]->size)+"|"+files[k]->stamp : std::string("none");
        if(now==console.dbRead[k]) continue;
        if(now!=console.dbListed[k]) { console.dbListed[k]=now; console.dbListedAt[k]=Now(); }
        bool settled=!files[k] || Now()-console.dbListedAt[k]>=StampSeconds;
        console.dbRead[k].clear();
        std::vector<unsigned char> data;
        std::string path=folder+"/"+names[k], why;
        if(!files[k]) console.dbActive.clear();
        else if(!ftp.Retrieve(path.c_str(),data,size_t(256)<<20)) { error="couldn't read "+path+": "+ftp.Error(); break; }
        else if(!(k ? ReadActiveTitleUpdates(data,console.dbActive,why) : ReadTitleUpdates(data,console.dbUpdates,why))) { error="couldn't read "+path+": "+why; break; }
        if(settled) console.dbRead[k]=now;
    }
    std::vector<TitleUpdate> updates=console.dbUpdates;
    int active=0;
    for(size_t i=0;i<updates.size();++i) {
        updates[i].active=std::find(console.dbActive.begin(),console.dbActive.end(),updates[i].id)!=console.dbActive.end();
        active+=updates[i].active;
    }
    std::string told=error.empty() ? Count(int(updates.size()),"title update","title updates")+" in Aurora, "+Number(active)+" in use" : error;
    if(told!=console.toldUpdates) {
        if(error.empty()) XboxLog(console,"%s.",told.c_str());
        else XboxLog(console,"couldn't look at the title updates in Aurora: %s.",error.c_str());
        console.toldUpdates=told;
    }
    bool changed;
    {
        std::lock_guard<std::mutex> hold(ConsoleLock);
        console.titleUpdatesError=error;
        if(!error.empty()) return;
        changed=!console.titleUpdatesRead || console.titleUpdates.size()!=updates.size();
        for(size_t i=0;i<updates.size() && !changed;++i) {
            const TitleUpdate& a=console.titleUpdates[i], & b=updates[i];
            changed=a.id!=b.id || a.titleId!=b.titleId || a.mediaId!=b.mediaId || a.baseVersion!=b.baseVersion || a.version!=b.version ||
                    a.name!=b.name || a.file!=b.file || a.active!=b.active;
        }
        console.titleUpdates.swap(updates);
        console.titleUpdatesRead=(long long)time(NULL);
    }
    if(changed) GamesChanged();
}

// The title updates of consoles looked at now, each on a connection of its
// own and all at once, and waited for: for Start on all, and a party that
// starts, so they're compared as they are, whether or not game folders are
// written. Not on a console that's off or in a NetISO game, as Aurora's FTP
// server isn't there; what was read before stays.
void LookTitleUpdatesNow(const std::vector<Console*>& consoles, const Options& o, double timeoutSeconds) {
    std::vector<std::thread> looks;
    for(size_t i=0;i<consoles.size();++i) {
        Console* c=consoles[i];
        if(!c || c->offline) continue;
        { std::lock_guard<std::mutex> hold(ConsoleLock); if(c->connections>0 && !c->mounted.empty() && !c->inAurora) continue; }
        looks.push_back(std::thread([c,&o,timeoutSeconds]() {
            Busy busy(c);
            FtpClient ftp(c->address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),timeoutSeconds);
            if(ftp.Open()) LookTitleUpdates(*c,ftp,o);
            ftp.Close();
        }));
    }
    for(size_t i=0;i<looks.size();++i) looks[i].join();
}

// Makes folder unless it's there (a failed MKD costs FtpDll's connection).
void EnsureFolder(FtpClient& ftp, const std::string& folder) {
    if(!ftp.Enters(folder.c_str()) && ftp.Code()/100==5) ftp.MakeFolder(folder.c_str());
}

// Restarts Aurora on the console through its web server, if Aurora is what it shows.
bool RestartAurora(const Console& console, const Options& o, const char* why) {
    AuroraWeb aurora(console.address,o.auroraWebPort,o.auroraWebUser,o.auroraWebPassword,5);
    if(!aurora.RestartAurora()) { XboxLog(console,"didn't restart Aurora %s: %s.",why,aurora.Error().c_str()); return false; }
    XboxLog(console,"restarted Aurora %s.",why);
    return true;
}

// Aurora's cleanup script and "Hide PC games while NetDisc360 is off" filter
// (see aurora.h), and the marker the filter looks for, after a push. When the
// filter hides the PC games though this server runs (Aurora started while it
// didn't), Aurora is restarted, once, if it's on screen and restartAurora allows.
void RefreshAuroraScripts(Console& console, PushState& s, FtpClient& ftp, const Options& o) {
    std::vector<FtpEntry> entries;
    if(!ftp.List(o.xboxFolder.c_str(),entries)) return;
    std::vector<std::string> folders;
    for(size_t i=0;i<entries.size();++i) if(entries[i].folder) folders.push_back(entries[i].name);
    std::string cleanup=CleanupScript(o.xboxFolder,folders), online=OnlineScript(o.xboxFolder,o.tidyAurora);
    if(cleanup.empty() || online.empty()) return;  // Game folders at a drive's top: no script could tell them apart.
    std::string cleanupPath=o.aurora+CleanupScriptPath, onlinePath=o.aurora+OnlineScriptPath;
    if(!ftp.Enters(o.aurora.c_str())) return;  // GuardFilter tells about that.
    // The folders the scripts go in, from Aurora's own down.
    const char* const paths[]={CleanupScriptPath,OnlineScriptPath};
    for(int k=0;k<2;++k)
        for(size_t at=1;(at=std::string(paths[k]).find('/',at))!=std::string::npos;++at)
            EnsureFolder(ftp,o.aurora+std::string(paths[k]).substr(0,at));
    int written=RefreshRemote(ftp,cleanupPath,std::vector<unsigned char>(cleanup.begin(),cleanup.end()));
    if(written<0) XboxLog(console,"couldn't write %s: %s",cleanupPath.c_str(),ftp.Error());
    written=RefreshRemote(ftp,onlinePath,std::vector<unsigned char>(online.begin(),online.end()));
    if(written>0) XboxLog(console,"wrote Aurora's \"Hide PC games while NetDisc360 is off\" filter (%s). Restart Aurora, then pick it under Filters, User.",onlinePath.c_str());
    else if(written<0) XboxLog(console,"couldn't write %s: %s",onlinePath.c_str(),ftp.Error());
    std::string helper=o.xboxFolder+"/"+HelperFolder;
    EnsureFolder(ftp,helper);
    std::vector<FtpEntry> files;
    if(!ftp.List(helper.c_str(),files)) return;
    // By the listing: a read of a file that isn't there can stall FtpDll's next one.
    bool marker=false, shown=false;
    for(size_t i=0;i<files.size();++i) {
        if(!files[i].folder && !_stricmp(files[i].name.c_str(),OnlineMarker)) marker=true;
        if(!files[i].folder && !_stricmp(files[i].name.c_str(),ShownMarker)) shown=true;
    }
    if(!marker) {
        static const char text[]="NetDisc360 runs on the PC; it deletes this file when it quits.\r\n";
        if(!ftp.Store((helper+"/"+OnlineMarker).c_str(),text,sizeof(text)-1)) { XboxLog(console,"couldn't write %s/%s: %s",helper.c_str(),OnlineMarker,ftp.Error()); return; }
    }
    console.marked=true;
    std::vector<unsigned char> state;
    if(!shown || s.restarted || !o.restartAurora || !ftp.Retrieve((helper+"/"+ShownMarker).c_str(),state,64)) return;
    if(std::string(state.begin(),state.end()).compare(0,7,"offline")) return;
    s.restarted=true;
    s.restartWanted=true;  // Once this push is done (see RestartWhenSafe).
    s.restartSince=Now();
}

// Aurora's restart for its filter (see RefreshAuroraScripts), once its scan
// was asked for and no other FTP work goes on with it; own as for
// SafeToRestart. One that can't be done for RestartWaitMost is dropped.
const double RestartWaitMost = 300;
void RestartWhenSafe(Console& console, PushState& s, const Options& o, int own) {
    if(s.scanTries) return;
    std::string why;
    if(!SafeToRestart(console,own,why)) {
        if(Now()-s.restartSince<RestartWaitMost) return;
        XboxLog(console,"didn't restart Aurora for its filter: %s.",why.c_str());
        s.restartWanted=false;
        return;
    }
    s.restartWanted=false;
    if(RestartAurora(console,o,"so its filter shows the PC games again")) XboxEvent(console,EventInfo,"Aurora restarted to show the PC games");
}

void FollowAddress(Console& console, PushState& s, FtpClient& ftp, const Options& o);

void PushTo(Console& console, PushState& s, const std::string& state, bool settled, double startedAt) {
    if(s.finished) return;
    Options o=Current();
    if(s.restartWanted) RestartWhenSafe(console,s,o,0);
    bool pushDue=settled && (!s.everPushed || state!=s.pushed || s.installedChanged || (s.retryAt && Now()>=s.retryAt));
    bool scanDue=s.scanTries && Now()>=s.scanAt;
    // Back in Aurora, or Aurora (re)started (see Console::recheck), and every
    // installedEvery seconds besides: what's installed is looked at again
    // (listings only, see installed.h), so a game installed or deleted on the
    // console gets or loses its tile; back in Aurora also its title updates.
    bool timed=s.everPushed && o.installedEvery>0 && Now()>=s.installedAt+o.installedEvery;
    bool recheck=(console.recheck || timed) && s.everPushed && !pushDue && !o.pushOnce;
    bool online=o.pushOnce || !s.downSince || console.seenAt.load()/1000.0>=s.downSince || Now()-s.upSince>=o.onlineWait;
    if(!(pushDue || scanDue || recheck) || Now()<s.nextProbe || !(!s.reachable || online || scanDue || recheck)) return;
    // Not while it plays a game: Aurora's FTP server isn't there to write
    // to, and a push then failed file by file (seen on the console). It goes
    // on once AuroraLoop sees Aurora again.
    bool playing;
    { std::lock_guard<std::mutex> hold(ConsoleLock); playing=console.connections>0 && !console.mounted.empty() && !console.inAurora; }
    if(playing && !o.pushOnce) return;
    // Nor while it shows the party wheel: the FTP server answering then is
    // the wheel app's, for guest profiles only (party/ftp.h).
    if(ShowsWheel(console.text) && !o.pushOnce) return;
    bool idle=o.xboxes.empty() && Now()-console.seenAt.load()/1000.0>=IdleSeconds;
    if(!s.reachable && idle && Now()<s.probedAt+IdleProbeSeconds) return;
    s.probedAt=Now();
    Busy busy(&console);
    FtpClient ftp(console.address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),15);
    if(!ftp.Open()) {
        // Only to look again: quietly, as Aurora may still be starting.
        if(recheck && !scanDue) { s.nextProbe=Now()+ProbeDownSeconds; return; }
        if(s.reachable || !s.toldDown) {
            XboxLog(console,"waiting for its FTP server (Aurora): %s",ftp.Error());
            XboxEvent(console,EventInfo,"Waiting for Aurora");
        }
        if(s.reachable || !s.downSince) s.downSince=Now();
        s.reachable=false; s.toldDown=true; s.toldWaiting=false; s.nextProbe=Now()+ProbeDownSeconds;
        console.ftp=1;
        s.finished=o.pushOnce;
        return;
    }
    bool back=!s.reachable;
    s.reachable=true; s.toldDown=false;
    console.ftp=2;
    if(back) {
        XboxLog(console,"its FTP server answers.");
        XboxEvent(console,EventXbox,"Aurora answers",std::string("Aurora on ")+console.text+" answers");
        // Back in Aurora: a guest still without a profile gets one now.
        if(!o.pushOnce && !GuestsWithoutProfile().empty()) MakeProfilesSoon(10);
        // Aurora started: its name on its screen, so it's told from the others.
        if(o.showName && !o.pushOnce) {
            Console* c=&console;
            std::thread([c,o]() {
                std::string error;
                if(!ShowName(*c,o,error)) XboxLog(*c,"couldn't show its name on its screen: %s.",error.c_str());
            }).detach();
        }
        s.upSince=Now();
        online=o.pushOnce || !s.downSince || console.seenAt.load()/1000.0>=s.downSince || o.onlineWait<=0;
    }
    bool lookUpdates=false;
    if(recheck) {
        lookUpdates=console.recheck;
        console.recheck=false;
        s.installedAt=Now();
        std::string before=s.installedSeen;
        std::vector<InstalledGame> found;
        LookInstalled(console,s,ftp,o,found);
        if(s.installedSeen!=before) {
            XboxLog(console,"what's installed on it changed; its game folders are written again.");
            s.installedChanged=true;
            pushDue=settled;
        }
    }
    if(o.filter && (back || pushDue)) GuardFilter(console,ftp,back,o);
    if(back || pushDue) GuardAppArt(console,ftp,o);
    if(pushDue && !online && !s.toldWaiting) {
        XboxLog(console,"waiting for it to come online before writing game folders (its NetISO plugin connects to this server; at most %.0f s).",o.onlineWait);
        s.toldWaiting=true;
    }
    if(pushDue && online) {
        SyncResult result;
        s.attempted=true; s.toldWaiting=false;
        double holdFrom=std::max(startedAt,FoldersChangedAt.load()/1000.0);
        bool missing=FoldersMissing;
        // Before the push's own connections to this server count as the console's.
        if(o.followAddress) FollowAddress(console,s,ftp,o);
        std::vector<InstalledGame> installed;
        LookInstalled(console,s,ftp,o,installed);
        s.installedAt=Now();
        if(Push(console,ftp,result,(o.pushOnce || Now()-holdFrom>=o.removeWait) && !missing,o,installed)) {
            s.everPushed=true; s.pushed=state; s.installedChanged=false;
            console.recheck=false;  // The push looked at everything a recheck would.
            lookUpdates=true;
            console.synced=(long long)time(NULL);
            if(o.filter) { RefreshInstallFilter(console,ftp,o); RefreshRecentFilter(console,ftp,o); }
            PutPartyApp(console,ftp,o);
            if(o.auroraScripts) RefreshAuroraScripts(console,s,ftp,o);
            // Folders held while a game folder is missing wait for it to come
            // back, which changes the state (see WatchLoop).
            s.retryAt=result.failed ? Now()+RetryFailedSeconds : result.held && !missing ? holdFrom+o.removeWait : result.skipped ? Now()+RetrySkippedSeconds : 0;
            if(result.added || result.removed) {
                if(o.scan) { s.scanTries=o.pushOnce ? 1 : ScanTries; s.scanAt=Now(); scanDue=true; }
                else XboxLog(console,"Aurora shows the changes after its next scan of %s.",o.xboxFolder.c_str());
            }
        } else s.nextProbe=Now()+30;
    }
    if(lookUpdates) LookTitleUpdates(console,ftp,o);
    if(scanDue) {
        if(StartScan(console,ftp,o)) s.scanTries=0;
        else if(--s.scanTries) s.scanAt=Now()+ScanRetrySeconds;
        else XboxLog(console,"Aurora shows the changes after its next scan of %s.",o.xboxFolder.c_str());
    }
    if(o.pushOnce && s.attempted && !s.scanTries) s.finished=true;
    ftp.Close();
    if(s.restartWanted) RestartWhenSafe(console,s,o,1);  // This session is done.
}

// The games' fingerprint, and whether it has settled, for the consoles' threads.
std::mutex GamesLock;
std::string GamesState;
bool GamesSettled;

// One console's push thread, so a console that is off (its FTP connection
// waits up to 15 s) doesn't hold up the others.
void ConsoleLoop(Console* console, PushState* s, double startedAt) {
    while(!s->finished) {
        std::string state;
        bool settled;
        { std::lock_guard<std::mutex> hold(GamesLock); state=GamesState; settled=GamesSettled; }
        if(console->push && Current().push) PushTo(*console,*s,state,settled,startedAt);
        if(!s->finished) Sleep(1000);
    }
}

bool Save();
void NoteOwnAddresses();

// Notes games that came or went, for the dashboard's activity.
void NoteChanges(const std::set<std::string>& before, const std::set<std::string>& now) {
    std::vector<std::string> added,removed;
    for(std::set<std::string>::const_iterator i=now.begin();i!=now.end();++i) if(!before.count(*i)) added.push_back(*i);
    for(std::set<std::string>::const_iterator i=before.begin();i!=before.end();++i) if(!now.count(*i)) removed.push_back(*i);
    if(added.size()==1) Event(EventGood,"New game: %s",WithoutIso(added[0]).c_str());
    else if(!added.empty()) Event(EventGood,"%d new games in your folders",int(added.size()));
    if(removed.size()==1) Event(EventInfo,"Game gone from your folders: %s",WithoutIso(removed[0]).c_str());
    else if(!removed.empty()) Event(EventInfo,"%d games gone from your folders",int(removed.size()));
}

// Keeps the consoles that connected in the settings file, now and then.
void RememberConsoles() {
    static std::map<DWORD,long long> saved;
    std::vector<Console*> consoles=AllConsoles();
    bool save=false;
    for(size_t i=0;i<consoles.size();++i) {
        long long at=consoles[i]->seenAt;
        if(at<0) continue;
        {
            std::lock_guard<std::mutex> hold(ConsoleLock);
            if(consoles[i]->hiddenAt!=-2 && at<=consoles[i]->hiddenAt) continue;  // Forgotten on the dashboard.
        }
        long long seen=(long long)time(NULL)-(long long)(Now()-at/1000.0);
        std::string id, name;
        { std::lock_guard<std::mutex> hold(ConsoleLock); id=consoles[i]->id; name=consoles[i]->name; }
        bool known=false, renamed=false;
        {
            std::lock_guard<std::mutex> hold(SettingsLock);
            for(size_t k=0;k<Known.size() && !known;++k) {
                if(Known[k].address!=consoles[i]->text) continue;
                Known[k].seen=seen; known=true;
                if(!name.empty() && (Known[k].name!=name || Known[k].id!=id)) { Known[k].name=name; Known[k].id=id; renamed=true; }
            }
            if(!known) { KnownConsole c = {consoles[i]->text, seen, id, name}; Known.push_back(c); }
        }
        std::map<DWORD,long long>::iterator last=saved.find(consoles[i]->address);
        if(!known || renamed || last==saved.end() || seen-last->second>600) { saved[consoles[i]->address]=seen; save=true; }
    }
    if(save) Save();
}

// Watches the game folders and keeps each console's game folders in step with
// them, each console on its own thread. A console is only contacted when there is
// something to do: the games changed (and have settled), or a scan or a
// skipped game is still due. Until its FTP server (Aurora) answers, it's tried
// every ProbeDownSeconds.
//
// A game folder that Aurora finds while the Xbox is still coming online gets
// no online lookup, so no name or art, ever: Banjo 38 s after a boot, and 17
// of 26 folders written as soon as Aurora's FTP answered after a reboot (seen
// on the console, 2026-10-17). Aurora's FTP answers well before the Xbox can
// reach out (its NetISO plugin lists "No Server!" for a minute or two). So
// after its FTP server comes back, the folders are written once the Xbox has
// connected to this server, which only a working network can, or after
// onlineWait seconds at most. A game run over NetISO while Aurora was away
// keeps sending requests, so there's no wait after it.
//
// Game folders whose game is gone are only removed removeWait seconds after
// the start, or after the game folders were last changed on the dashboard: a
// games drive can come up after a server started with Windows, and folders
// get added one at a time. Removing them early would drop the games from
// Aurora, with their art and stats. With no game folders set at all (a new install), nothing is written.
void WatchLoop() {
    std::string previous;
    const double startedAt=Now();
    double changedAt=-1e9;
    std::map<DWORD,PushState> states;
    std::set<std::string> names;
    std::string state;
    double nextScan=0;
    for(int tick=0;;++tick) {
        bool soon=ScanSoon.exchange(false);
        if(!tick || soon || Now()>=nextScan) {
            double began=Now();
            std::vector<IsoFile> isos=Isos();
            nextScan=Now()+std::max(1.0,std::min(ScanMostSeconds,(Now()-began)*ScanEffort))-0.05;
            state=Fingerprint(isos);
            if(state!=previous || !tick) {
                if(state!=previous && (!previous.empty() || changedAt>-1e9)) changedAt=Now();
                previous=state;
                std::set<std::string> now;
                for(size_t i=0;i<isos.size();++i) now.insert(isos[i].name);
                if(tick) NoteChanges(names,now);
                names.swap(now);
                NoteFirstSeen(isos);
                std::lock_guard<std::mutex> hold(GamesListLock);
                GamesList=isos; ++GamesVersion; GamesScanned=true;
            }
        }
        Options o=Current();
        // A missing game folder coming back pushes again, removing what is really gone.
        char generation[16]; _snprintf_s(generation,sizeof(generation),_TRUNCATE,"#%d%s",PushGeneration.load(),FoldersMissing ? "!" : "");
        { std::lock_guard<std::mutex> hold(GamesLock); GamesState=state+generation; GamesSettled=Now()-changedAt>=SettleSeconds && !o.folders.empty(); }
        if(tick%10==0) RememberConsoles();
        if(tick%60==0) NoteOwnAddresses();
        if(o.push) {
            std::vector<Console*> consoles=AllConsoles();
            bool finished=true,failed=false;
            for(size_t i=0;i<consoles.size();++i) {
                if(!consoles[i]->push) continue;
                bool added=!states.count(consoles[i]->address);
                PushState& s=states[consoles[i]->address];  // Never erased, so the thread's pointer stays valid.
                if(added) std::thread(ConsoleLoop,consoles[i],&s,startedAt).detach();
                finished=finished && s.finished;
                failed=failed || !s.everPushed;
            }
            // --push-once (the tests): ends when every console given is done,
            // as a server that quits (its marker for Aurora's filter goes).
            // _Exit, as exit's destructors would pull the settings and lists
            // from under the threads that still run (AuroraLoop crashed in
            // Current() on CI).
            if(o.pushOnce && !states.empty() && finished) { ServerStopping(); fflush(NULL); _Exit(failed ? 1 : 0); }
        }
        Sleep(1000);
    }
}

// Counts the bytes of each extracted game for the dashboard, which takes
// reading every file's entry, so it's done here rather than in WatchLoop.
void SizeLoop() {
    for(;;) {
        std::vector<IsoFile> games;
        { std::lock_guard<std::mutex> hold(GamesListLock); games=GamesList; }
        {   // And the copies not served, for the dashboard's conflicts.
            std::lock_guard<std::mutex> hold(DuplicatesLock);
            for(size_t i=0;i<Conflicts.size();++i) games.insert(games.end(),Conflicts[i].copies.begin()+1,Conflicts[i].copies.end());
        }
        for(size_t i=0;i<games.size();++i) {
            if(games[i].kind!=DiscFolder) continue;
            {
                std::lock_guard<std::mutex> hold(GamesListLock);
                std::map<std::string,std::pair<long long,U64> >::const_iterator known=FolderSizes.find(games[i].path);
                if(known!=FolderSizes.end() && known->second.first==games[i].modified) continue;
            }
            U64 bytes=FolderBytes(games[i].path);
            std::lock_guard<std::mutex> hold(GamesListLock);
            FolderSizes[games[i].path]=std::make_pair(games[i].modified,bytes);
            ++GamesVersion;
        }
        Sleep(3000);
    }
}

// The settings file ------------------------------------------------------------

std::mutex SaveLock;

std::string AddressList(const std::vector<DWORD>& xboxes) {
    std::string list;
    for(size_t i=0;i<xboxes.size();++i) {
        char text[16]; AddressText(xboxes[i],text,sizeof(text));
        list+=(i ? ", " : "")+std::string(text);
    }
    return list;
}

// Addresses separated by commas, semicolons or spaces; false at one that isn't.
bool ReadAddresses(const std::string& list, std::vector<DWORD>& out, std::string& bad) {
    std::string text=list;
    for(size_t i=0;i<text.size();++i) if(text[i]==',' || text[i]==';' || text[i]=='\t') text[i]=' ';
    for(size_t at=0;(at=text.find_first_not_of(' ',at))!=std::string::npos;) {
        size_t end=text.find(' ',at);
        std::string one=text.substr(at,end==std::string::npos ? std::string::npos : end-at);
        at=end;
        DWORD address=inet_addr(one.c_str());
        if(address==INADDR_NONE || !address) { bad=one; return false; }
        if(std::find(out.begin(),out.end(),address)==out.end()) out.push_back(address);
    }
    return true;
}

std::string Number(long long n) { char text[24]; _snprintf_s(text,sizeof(text),_TRUNCATE,"%lld",n); return text; }

// An address the page or the settings file names a console by, if it's
// written as the dashboard writes them (10.10.1.42); "" if not. inet_addr
// also takes 10.10.298, 012.012.01.052 and anything after a space, so one
// console could be listed under any number of names, and a line break
// would go into the log.
std::string CanonicalAddress(const std::string& typed) {
    DWORD at=inet_addr(typed.c_str());
    if(typed.empty() || at==INADDR_NONE || !at) return std::string();
    char text[16]; AddressText(at,text,sizeof(text));
    return typed==text ? typed : std::string();
}

// Writes the settings to the settings file, if there is one. Keys this
// version doesn't use (the window's theme, say) stay as they were.
// A count the settings file gives, such as how many folders it lists: one
// from a file made by hand or a backup could otherwise be any number.
long long Listed(const config::File& f, const char* key) { return std::max(0LL,std::min(1000LL,f.Number(key,0))); }

// A version as the settings file and the dashboard take it: what a tag can hold.
std::string CleanVersion(const std::string& version) {
    std::string out;
    for(size_t i=0;i<version.size() && out.size()<40;++i) {
        char c=version[i];
        if((c>='0' && c<='9') || (c>='a' && c<='z') || (c>='A' && c<='Z') || c=='.' || c=='-' || c=='+') out+=c;
    }
    return out;
}

// The update history as one setting: "version|time|how|from|backup", comma between.
std::string HistoryText(const std::vector<Installed>& history) {
    std::string text;
    for(size_t i=0;i<history.size();++i)
        text+=(i ? "," : "")+history[i].version+"|"+Number(history[i].at)+"|"+history[i].how+"|"+history[i].from+"|"+history[i].backup;
    return text;
}

std::vector<Installed> ReadHistory(const std::string& text) {
    std::vector<Installed> out;
    for(size_t at=0;at<text.size();) {
        size_t end=text.find(',',at);
        if(end==std::string::npos) end=text.size();
        std::vector<std::string> parts;
        for(size_t p=at;p<=end;) {
            size_t bar=text.find('|',p);
            if(bar==std::string::npos || bar>end) bar=end;
            parts.push_back(text.substr(p,bar-p));
            p=bar+1;
        }
        at=end+1;
        if(parts.size()<3) continue;
        Installed e;
        e.version=CleanVersion(parts[0]);
        e.at=atoll(parts[1].c_str());
        e.how=parts[2]=="app" || parts[2]=="other" || parts[2]=="failed" ? parts[2] : "first";
        e.from=parts.size()>3 ? CleanVersion(parts[3]) : std::string();
        e.backup=parts.size()>4 && backup::Named(parts[4]) ? parts[4] : std::string();
        if(!e.version.empty()) out.push_back(e);
    }
    if(out.size()>HistoryKeep) out.erase(out.begin(),out.end()-HistoryKeep);
    return out;
}

// Adds this start to the update history: an update NetDisc360 installed (or
// one that didn't start, so this version was put back), or a version that
// came another way; false if there's nothing to add. Under SettingsLock.
bool NoteStart(const std::string& updatedFrom, const std::string& updateTo, const std::string& backupName) {
    long long now=(long long)time(NULL);
    std::string last;
    for(size_t i=UpdateHistory.size();i-->0 && last.empty();) if(UpdateHistory[i].how!="failed") last=UpdateHistory[i].version;
    Installed e;
    e.at=now;
    if(!updatedFrom.empty() && updatedFrom!=NETDISC_VERSION) {
        e.version=NETDISC_VERSION; e.from=CleanVersion(updatedFrom); e.how="app"; e.backup=backupName;
    } else if(!updateTo.empty()) {
        e.version=CleanVersion(updateTo); e.from=NETDISC_VERSION; e.how="failed"; e.backup=backupName;
        UpdateHistory.push_back(e);
        if(last==NETDISC_VERSION) e.version.clear();
        else { e.version=NETDISC_VERSION; e.from=last; e.how=last.empty() ? "first" : "other"; e.backup.clear(); }
    } else if(last!=NETDISC_VERSION) {
        e.version=NETDISC_VERSION; e.from=last; e.how=last.empty() ? "first" : "other";
    } else return false;
    if(!e.version.empty()) UpdateHistory.push_back(e);
    if(UpdateHistory.size()>HistoryKeep) UpdateHistory.erase(UpdateHistory.begin(),UpdateHistory.end()-HistoryKeep);
    return true;
}

bool Save() {
    PartyNews();
    // What's saved is read once it's this call's turn, so the last to write
    // the file writes the newest settings, consoles and party.
    std::lock_guard<std::mutex> saving(SaveLock);
    Options o;
    std::vector<KnownConsole> known;
    { std::lock_guard<std::mutex> hold(SettingsLock); o=Settings; known=Known; }
    if(o.configPath.empty()) return false;
    Party party=CurrentParty();
    config::File f;
    f.Load(o.configPath);
    long long before=Listed(f,"folders");
    f.Set("folders",Number((long long)o.folders.size()));
    for(size_t i=0;i<o.folders.size();++i) f.Set(("folder"+Number((long long)i+1)).c_str(),config::ToUtf8(o.folders[i]));
    for(long long i=(long long)o.folders.size()+1;i<=before;++i) f.Remove(("folder"+Number(i)).c_str());
    f.Set("xbox",AddressList(o.xboxes));
    f.Set("ftp_user",o.ftpUser);
    f.Set("ftp_password",config::Protect(o.ftpPassword));
    f.Set("xbox_folder",o.xboxFolder);
    f.Set("aurora",o.aurora);
    if(CanPush) f.Set("push",o.push ? "1" : "0");
    f.Set("category",o.filter ? "1" : "0");
    f.Set("scan",o.scan ? "1" : "0");
    f.Set("setup_done",o.setupDone ? "1" : "0");
    f.Set("wizard_done",o.wizardDone ? "1" : "0");
    f.Set("told_tray",o.toldTray ? "1" : "0");
    f.Set("web_lan",o.webLan ? "1" : "0");
    if(!PasswordFixed) f.Set("web_password",web::PasswordHash());
    f.Set("backup_every",Number(o.backupEvery));
    f.Set("backup_keep",Number(o.backupKeep));
    f.Set("update_check",o.updateCheck ? "1" : "0");
    f.Set("update_auto",o.updateAuto ? "1" : "0");
    f.Set("update_prerelease",o.updatePrerelease ? "1" : "0");
    f.Set("skip_installed",o.skipInstalled ? "1" : "0");
    f.Set("installed_folders",o.installedFolders);
    f.Set("aurora_scripts",o.auroraScripts ? "1" : "0");
    f.Set("tidy_aurora",o.tidyAurora ? "1" : "0");
    f.Set("plugin_set",o.pluginSet);
    f.Set("plugin_kit",o.pluginKit);
    f.Set("restart_aurora",o.restartAurora ? "1" : "0");
    f.Set("aurora_web_user",o.auroraWebUser);
    f.Set("aurora_web_password",config::Protect(o.auroraWebPassword));
    f.Set("aurora_web_port",Number(o.auroraWebPort));
    f.Set("follow_address",o.followAddress ? "1" : "0");
    f.Set("keep_awake",o.keepAwake ? "1" : "0");
    f.Set("show_name",o.showName ? "1" : "0");
    f.Set("auto_off_minutes",std::to_string(o.autoOffMinutes));
    {
        std::lock_guard<std::mutex> hold(SettingsLock);
        std::string own;
        for(size_t i=0;i<OwnAddresses.size();++i) own+=(i ? "," : "")+OwnAddresses[i];
        f.Set("own_addresses",own);
        if(UpdatedFrom.empty()) f.Remove("updated_from");
        else f.Set("updated_from",UpdatedFrom);
        if(UpdateTo.empty()) f.Remove("update_to");
        else f.Set("update_to",UpdateTo);
        if(UpdateSkip.empty()) f.Remove("update_skip");
        else f.Set("update_skip",UpdateSkip);
        if(UpdateBackup.empty()) f.Remove("update_backup");
        else f.Set("update_backup",UpdateBackup);
        f.Set("update_history",HistoryText(UpdateHistory));
    }
    std::map<std::string,Choice> choices;
    { std::lock_guard<std::mutex> hold(DuplicatesLock); choices=Choices; }
    before=Listed(f,"choices");
    f.Set("choices",Number((long long)choices.size()));
    long long n=0;
    for(std::map<std::string,Choice>::const_iterator c=choices.begin();c!=choices.end();++c) {
        std::string copies;
        for(std::set<std::string>::const_iterator k=c->second.copies.begin();k!=c->second.copies.end();++k) copies+=(copies.empty() ? "" : ",")+*k;
        f.Set(("choice"+Number(++n)).c_str(),c->first+"|"+copies+"|"+config::ToUtf8(c->second.path));
    }
    for(long long i=n+1;i<=before;++i) f.Remove(("choice"+Number(i)).c_str());
    before=Listed(f,"consoles");
    f.Set("consoles",Number((long long)known.size()));
    // address|seen, then |id|name once its name was read (the name last: no bar is in it).
    for(size_t i=0;i<known.size();++i)
        f.Set(("console"+Number((long long)i+1)).c_str(),known[i].address+"|"+Number(known[i].seen)+
              (known[i].name.empty() ? std::string() : "|"+known[i].id+"|"+known[i].name));
    for(long long i=(long long)known.size()+1;i<=before;++i) f.Remove(("console"+Number(i)).c_str());
    SaveParty(f,party);
    SaveGuestProfiles(f,GuestProfilesNow());
    SaveGuestStats(f,GuestStatsNow());
    SaveUsers(o.configPath,party);
    if(f.Save(o.configPath)) return true;
    static bool told;
    if(!told) Log("Couldn't save the settings in %s; changes last until the server stops.",config::FromUtf8(o.configPath).c_str());
    told=true;
    return false;
}

bool Flag(const config::File& f, const char* key, bool fallback) {
    std::string value=f.Get(key);
    return value.empty() ? fallback : value!="0";
}

std::vector<std::string> SplitCommandLine(const std::string& line) {
    std::vector<std::string> args;
    std::string arg;
    bool quoted=false,any=false;
    for(size_t i=0;i<line.size();++i) {
        char c=line[i];
        if(c=='"') { quoted=!quoted; any=true; }
        else if((c==' ' || c=='\t') && !quoted) { if(any) args.push_back(arg); arg.clear(); any=false; }
        else { arg+=c; any=true; }
    }
    if(any) args.push_back(arg);
    return args;
}

bool ParseArguments(const std::vector<std::string>& args, Options& o, bool fromFile=false);

// The settings file's values over the command line's. The window kept extra
// command-line options in "options"; they come first, so the other keys win.
bool LoadConfig(Options& o) {
    config::File f;
    if(!f.Load(o.configPath)) return false;
    if(!f.Get("options").empty() && !ParseArguments(SplitCommandLine(config::FromUtf8(f.Get("options"))),o,true))
        Log("The settings file's options (%s) aren't all understood; see netdisc -h.",config::FromUtf8(f.Get("options")).c_str());
    if(f.Has("folders")) {
        o.folders.clear();
        for(long long i=1,n=Listed(f,"folders");i<=n;++i) {
            std::string folder=f.Get(("folder"+Number(i)).c_str());
            if(!folder.empty()) o.folders.push_back(CleanFolder(config::FromUtf8(folder)));
        }
    }
    if(f.Has("xbox")) {
        std::vector<DWORD> xboxes; std::string bad;
        if(ReadAddresses(f.Get("xbox"),xboxes,bad)) o.xboxes=xboxes;
        else Log("The settings file's consoles list has \"%s\", which isn't an IPv4 address; it's left out.",bad.c_str());
    }
    if(!f.Get("ftp_user").empty()) o.ftpUser=f.Get("ftp_user");
    std::string password;
    if(f.Has("ftp_password") && config::Unprotect(f.Get("ftp_password"),password)) o.ftpPassword=password;
    if(!f.Get("xbox_folder").empty()) o.xboxFolder=f.Get("xbox_folder");
    if(!f.Get("aurora").empty()) o.aurora=f.Get("aurora");
    o.push=Flag(f,"push",o.push);
    o.filter=Flag(f,"category",o.filter);
    o.scan=Flag(f,"scan",o.scan);
    o.setupDone=Flag(f,"setup_done",o.setupDone);
    // A settings file from before the wizard: that copy was set up already.
    o.wizardDone=Flag(f,"wizard_done",true);
    o.toldTray=Flag(f,"told_tray",o.toldTray);
    o.webLan=Flag(f,"web_lan",o.webLan);
    if(f.Has("web_port")) o.webPort=int(f.Number("web_port",o.webPort));
    if(!PasswordFixed) web::SetPasswordHash(f.Get("web_password"));
    if(f.Has("backup_every")) o.backupEvery=std::max(0,std::min(365,int(f.Number("backup_every",o.backupEvery))));
    if(f.Has("backup_keep")) o.backupKeep=std::max(1,std::min(100,int(f.Number("backup_keep",o.backupKeep))));
    o.updateCheck=Flag(f,"update_check",o.updateCheck);
    o.updateAuto=Flag(f,"update_auto",o.updateAuto);
    o.updatePrerelease=Flag(f,"update_prerelease",o.updatePrerelease);
    o.skipInstalled=Flag(f,"skip_installed",o.skipInstalled);
    if(f.Has("installed_folders")) o.installedFolders=f.Get("installed_folders");
    o.auroraScripts=Flag(f,"aurora_scripts",o.auroraScripts);
    o.tidyAurora=Flag(f,"tidy_aurora",o.tidyAurora);
    if(f.Has("plugin_set")) o.pluginSet=f.Get("plugin_set");
    o.pluginKit=f.Get("plugin_kit");
    o.restartAurora=Flag(f,"restart_aurora",o.restartAurora);
    if(!f.Get("aurora_web_user").empty()) o.auroraWebUser=f.Get("aurora_web_user");
    if(f.Has("aurora_web_password") && config::Unprotect(f.Get("aurora_web_password"),password)) o.auroraWebPassword=password;
    if(f.Has("aurora_web_port")) o.auroraWebPort=std::max(1,std::min(65535,int(f.Number("aurora_web_port",o.auroraWebPort))));
    o.followAddress=Flag(f,"follow_address",o.followAddress);
    o.keepAwake=Flag(f,"keep_awake",o.keepAwake);
    o.showName=Flag(f,"show_name",o.showName);
    if(f.Has("auto_off_minutes")) o.autoOffMinutes=std::max(0,std::min(1440,int(f.Number("auto_off_minutes",0))));
    {
        std::lock_guard<std::mutex> hold(SettingsLock);
        OwnAddresses.clear();
        std::string own=f.Get("own_addresses");
        for(size_t at=0;at<own.size();) {
            size_t comma=own.find(',',at);
            if(comma==std::string::npos) comma=own.size();
            std::string one=own.substr(at,comma-at);
            if(inet_addr(one.c_str())!=INADDR_NONE && OwnAddresses.size()<OwnAddressesKept) OwnAddresses.push_back(one);
            at=comma+1;
        }
        UpdatedFrom=f.Get("updated_from");
        UpdateTo=f.Get("update_to");
        UpdateSkip=f.Get("update_skip");
        UpdateBackup=f.Get("update_backup");
        UpdateHistory=ReadHistory(f.Get("update_history"));
    }
    // Which copy of a game to play: "key|copy ids|path" (see Choice).
    {
        std::map<std::string,Choice> choices;
        for(long long i=1,n=Listed(f,"choices");i<=n;++i) {
            std::string value=f.Get(("choice"+Number(i)).c_str());
            size_t bar=value.find('|'), second=bar==std::string::npos ? bar : value.find('|',bar+1);
            if(second==std::string::npos || !bar || second+1>=value.size()) continue;
            Choice choice;
            choice.path=config::FromUtf8(value.substr(second+1));
            std::string copies=value.substr(bar+1,second-bar-1);
            for(size_t at=0;at<copies.size();) {
                size_t comma=copies.find(',',at);
                if(comma==std::string::npos) comma=copies.size();
                if(comma>at) choice.copies.insert(copies.substr(at,comma-at));
                at=comma+1;
            }
            choices[value.substr(0,bar)]=choice;
        }
        std::lock_guard<std::mutex> hold(DuplicatesLock);
        Choices.swap(choices);
    }
    std::vector<KnownConsole> known;
    for(long long i=1,n=Listed(f,"consoles");i<=n;++i) {
        std::string value=f.Get(("console"+Number(i)).c_str());
        size_t bar=value.find('|');
        if(bar==std::string::npos || !bar) continue;
        KnownConsole c = {value.substr(0,bar), atoll(value.c_str()+bar+1), std::string(), std::string()};
        size_t idAt=value.find('|',bar+1), nameAt=idAt==std::string::npos ? idAt : value.find('|',idAt+1);
        if(nameAt!=std::string::npos) { c.id=value.substr(idAt+1,nameAt-idAt-1); c.name=value.substr(nameAt+1); }
        known.push_back(c);
    }
    // The LAN party: its consoles are addresses (a file made by hand could have anything).
    Party party;
    LoadParty(f,party);
    std::vector<GuestProfile> guestProfiles;
    LoadGuestProfiles(f,guestProfiles);
    { std::lock_guard<std::mutex> hold(GuestLock); GuestProfiles.swap(guestProfiles); }
    std::vector<GuestStats> stats;
    LoadGuestStats(f,stats);
    { std::lock_guard<std::mutex> hold(StatsLock); GuestStatsList.swap(stats); }
    LoadUsers(o.configPath);
    party.gamemaster=CanonicalAddress(party.gamemaster);
    for(size_t i=party.consoles.size();i-->0;)
        if(CanonicalAddress(party.consoles[i]).empty()) party.consoles.erase(party.consoles.begin()+long(i));
    TidyParty(party);
    {
        std::lock_guard<std::mutex> change(PartyChangeLock);
        std::lock_guard<std::mutex> hold(PartyLock);
        TheParty=party;
    }
    std::lock_guard<std::mutex> hold(SettingsLock);
    Known.swap(known);
    return true;
}

// The dashboard ----------------------------------------------------------------

std::string WebAddress(const Options& o) { return o.webLan ? "0.0.0.0" : o.webAddress; }

// This computer's IPv4 addresses on networks with a gateway (Windows) or
// that are up (elsewhere): what netiso.xex.txt names.
std::vector<std::string> PcAddresses() {
    std::vector<std::string> out;
#ifdef _WIN32
    ULONG size=16384;
    std::vector<BYTE> buffer(size);
    IP_ADAPTER_ADDRESSES* list=(IP_ADAPTER_ADDRESSES*)buffer.data();
    ULONG flags=GAA_FLAG_INCLUDE_GATEWAYS|GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER;
    if(GetAdaptersAddresses(AF_INET,flags,NULL,list,&size)==ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        list=(IP_ADAPTER_ADDRESSES*)buffer.data();
    }
    if(GetAdaptersAddresses(AF_INET,flags,NULL,list,&size)!=NO_ERROR) return out;
    for(IP_ADAPTER_ADDRESSES* a=list;a;a=a->Next) {
        if(a->OperStatus!=IfOperStatusUp || a->IfType==IF_TYPE_SOFTWARE_LOOPBACK || !a->FirstGatewayAddress) continue;
        for(IP_ADAPTER_UNICAST_ADDRESS* u=a->FirstUnicastAddress;u;u=u->Next) {
            char text[16]; AddressText(((sockaddr_in*)u->Address.lpSockaddr)->sin_addr.s_addr,text,sizeof(text));
            out.push_back(text);
        }
    }
#else
    struct ifaddrs* list;
    if(getifaddrs(&list)) return out;
    for(struct ifaddrs* a=list;a;a=a->ifa_next) {
        if(!a->ifa_addr || a->ifa_addr->sa_family!=AF_INET || !(a->ifa_flags&IFF_UP) || (a->ifa_flags&IFF_LOOPBACK)) continue;
        std::string name=a->ifa_name;
        if(!name.compare(0,6,"docker") || !name.compare(0,3,"br-") || !name.compare(0,4,"veth") || !name.compare(0,5,"virbr")) continue;
        char text[16]; AddressText(((sockaddr_in*)a->ifa_addr)->sin_addr.s_addr,text,sizeof(text));
        out.push_back(text);
    }
    freeifaddrs(list);
#endif
    return out;
}

// Updates ------------------------------------------------------------------

// What the update check found and how an install goes (UpdateLoop,
// InstallUpdate), for the dashboard.
struct UpdateStatus {
    bool checking, installing;
    long long checkedAt;     // When a check last got GitHub's answer (Unix time); 0: not yet.
    std::string error;       // Why the last check got none.
    bool found;              // latest is GitHub's newest release.
    update::Release latest;
    std::vector<update::Release> releases;  // All of them, newest first, for the changes since this version.
    std::string stage;       // While installing: download, check or restart.
    long long got, total;    // Of the download.
    std::string installError;
    std::string told;        // The version the log and the icon told about.
    UpdateStatus() : checking(false), installing(false), checkedAt(0), found(false), got(0), total(0) {}
};
UpdateStatus Updates;
std::mutex UpdateLock;
std::atomic<bool> CheckSoon(false);  // UpdateLoop checks within seconds.

// How this copy is updated: app (NetDisc360.exe installs it), docker (pull the
// image) or manual (download it, or build it, by hand).
const char* UpdateHow() {
#ifdef _WIN32
    if(InTray) return "app";
#endif
    return InDocker ? "docker" : "manual";
}

// Whether the server asks GitHub itself; if not, the page does.
bool ChecksUpdates(const Options& o) { return update::CanFetch() || !o.releasesFile.empty(); }

void UpdateJson(web::Json& j, const Options& o, bool notes) {
    std::vector<Installed> history;
    { std::lock_guard<std::mutex> hold(SettingsLock); history=UpdateHistory; }
    std::lock_guard<std::mutex> hold(UpdateLock);
    const UpdateStatus& u=Updates;
    bool newer=u.found && update::Compare(u.latest.version,NETDISC_VERSION)>0;
    j.Key("update").Open('{').Set("how",UpdateHow()).Set("checks",ChecksUpdates(o)).Set("checking",u.checking);
    j.Set("checkedAt",u.checkedAt).Set("error",u.error).Set("newer",newer);
    j.Set("installing",u.installing).Set("stage",u.stage).Set("got",u.got).Set("total",u.total).Set("installError",u.installError);
    if(u.found) {
        j.Key("latest").Open('{').Set("tag",u.latest.tag).Set("version",u.latest.version).Set("name",u.latest.name);
        j.Set("page",u.latest.page).Set("published",u.latest.published).Set("prerelease",u.latest.prerelease);
        j.Set("size",u.latest.exeSize).Set("exe",!u.latest.exeUrl.empty());
        if(notes) j.Set("notes",u.latest.notes);
        j.Close('}');
    } else j.Key("latest").Null();
    if(notes) {
        // Every release GitHub lists, newer: those since this version (the
        // changelog before an update), and this one's own notes.
        j.Key("releases").Open('[');
        for(size_t i=0;u.found && i<u.releases.size();++i) {
            const update::Release& r=u.releases[i];
            j.Open('{').Set("tag",r.tag).Set("version",r.version).Set("name",r.name).Set("notes",r.notes);
            j.Set("page",r.page).Set("published",r.published).Set("prerelease",r.prerelease).Set("newer",update::Compare(r.version,NETDISC_VERSION)>0).Close('}');
        }
        j.Close(']');
    }
    j.Key("history").Open('[');
    for(size_t i=history.size();i-->0;)
        j.Open('{').Set("version",history[i].version).Set("at",history[i].at).Set("how",history[i].how).Set("from",history[i].from).Set("backup",history[i].backup).Close('}');
    j.Close(']');
    j.Close('}');
}

// The LAN party's pictures and the consoles' title updates ------------------

// The party theme's pictures (its logo, its background photos), uploaded on
// the dashboard: in the folder "party" next to the settings file, under
// names the server gives them (logo.png, background3.jpg), never the
// browser's. Too big for the settings' backups, which leave them out.
enum { PartyPictureMost = 3 << 20, PartyBackgroundsMost = 8 };
std::mutex PartyFilesLock;  // While pictures are added or removed.

std::string PartyFolder(const Options& o) { return o.configPath.empty() ? std::string() : config::FolderOf(o.configPath)+"party"; }

std::string InFolder(const std::string& folder, const std::string& name) {
#ifdef _WIN32
    return folder+"\\"+name;
#else
    return folder+"/"+name;
#endif
}

// The pictures the party takes, by extension and type.
const char* const PictureTypes[][2]={ { "png", "image/png" }, { "jpg", "image/jpeg" }, { "webp", "image/webp" } };

// A picture's type by its file name's extension; NULL if it's no picture of ours.
const char* PictureType(const std::string& name) {
    size_t dot=name.rfind('.');
    for(size_t i=0;i<sizeof(PictureTypes)/sizeof(PictureTypes[0]) && dot!=std::string::npos;++i)
        if(name.compare(dot+1,std::string::npos,PictureTypes[i][0])==0) return PictureTypes[i][1];
    return NULL;
}

// The party's pictures as they are in its folder: the logo's file name ("" for
// none), and the background photos' in their order (by their number). version
// changes with any of them (their names, sizes and times; "" for none), so a
// page on another device (the big screen on a TV) loads them again.
struct PartyPictures { std::string logo; std::vector<std::string> backgrounds; long most; std::string version; PartyPictures() : most(0) {} };
PartyPictures PicturesNow(const Options& o) {
    PartyPictures out;
    std::string folder=PartyFolder(o);
    if(folder.empty()) return out;
    std::vector<config::Entry> files=config::Files(folder);
    std::vector<std::pair<long,std::string> > numbered;
    unsigned hash=2166136261u;  // FNV-1a over the pictures' names, sizes and times.
    for(size_t i=0;i<files.size();++i) {
        const std::string& name=files[i].name;
        if(!PictureType(name)) continue;
        std::string seen=name+"|"+Number(files[i].size)+"|"+Number(files[i].time)+";";
        for(size_t k=0;k<seen.size();++k) hash=(hash^(unsigned char)seen[k])*16777619u;
        std::string stem=name.substr(0,name.rfind('.'));
        if(stem=="logo") out.logo=name;
        else if(stem.size()>10 && stem.size()<=19 && !stem.compare(0,10,"background") && stem.find_first_not_of("0123456789",10)==std::string::npos)
            numbered.push_back(std::make_pair(atol(stem.c_str()+10),name));
    }
    std::sort(numbered.begin(),numbered.end());
    for(size_t i=0;i<numbered.size();++i) { out.backgrounds.push_back(numbered[i].second); out.most=std::max(out.most,numbered[i].first); }
    char version[12]; _snprintf_s(version,sizeof(version),_TRUNCATE,"%08x",hash);
    if(!out.logo.empty() || !out.backgrounds.empty()) out.version=version;
    return out;
}

void NamesJson(web::Json& j, const std::vector<std::string>& names) {
    j.Open('[');
    for(size_t i=0;i<names.size();++i) j.String(names[i]);
    j.Close(']');
}

void TeamsJson(web::Json& j, const std::vector<Team>& teams) {
    j.Open('[');
    for(size_t i=0;i<teams.size();++i) NamesJson(j,teams[i]);
    j.Close(']');
}

void PairsJson(web::Json& j, const std::vector<NamePair>& pairs) {
    j.Open('[');
    for(size_t i=0;i<pairs.size();++i) j.Open('[').String(pairs[i].first).String(pairs[i].second).Close(']');
    j.Close(']');
}

void SeatsJson(web::Json& j, const Seats& seats) {
    j.Open('{');
    for(size_t i=0;i<seats.size();++i) { j.Key(seats[i].first.c_str()); NamesJson(j,seats[i].second); }
    j.Close('}');
}

// The raffle wheel's latest spin (see Spin in party.h), with how far into it
// the server is now (elapsed, milliseconds; below 0 before the ask, never),
// so each screen turns its wheel from where the others are. null when there
// is none, or its draw was changed since.
void SpinJson(web::Json& j, const Spin& s) {
    if(!s.id || s.steps.empty()) { j.Null(); return; }
    j.Open('{').Set("id",(long long)s.id).Set("kind",s.kind).Set("title",s.title).Set("teams",s.teams).Key("wheel");
    NamesJson(j,s.wheel);
    j.Key("steps").Open('[');
    for(size_t i=0;i<s.steps.size();++i) {
        // Three decimals, as planned (Json's numbers keep six digits only).
        char to[32]; _snprintf_s(to,sizeof(to),_TRUNCATE,"%.3f",s.steps[i].to);
        j.Open('{').Set("segment",s.steps[i].segment).Set("team",s.steps[i].team).Key("to").Raw(to).Set("ms",s.steps[i].ms).Close('}');
    }
    j.Close(']').Set("leadMs",s.leadMs).Set("pauseMs",s.pauseMs).Set("totalMs",s.TotalMs()).Set("elapsed",(long long)((Now()-s.at)*1000)).Close('}');
}

// Readiness check per console (Marcus 2026-11-02): what stands between a
// party console and the next game, in plain sentences; none: ready.
std::vector<std::string> NotReady(const Party& p, const std::string& address) {
    std::vector<std::string> out;
    Console* c=ConsoleIfKnown(inet_addr(address.c_str()));
    bool on=false, disc=false;
    if(c) {
        std::lock_guard<std::mutex> hold(ConsoleLock);
        on=!c->offline && (c->answers || c->connections>0);
        disc=c->connections>0;
    }
    if(!on) { out.push_back("It's off, or doesn't answer"); return out; }
    if(!disc) out.push_back("Its NetDisc360 disc drive isn't connected, so games from this PC can't start on it");
    if(ShowsWheel(address)) {
        std::lock_guard<std::mutex> hold(WheelSeenLock);
        std::map<std::string,unsigned>::const_iterator b=WheelBuild.find(address);
        if(WheelAppBuiltIn && NETDISC_BUILD_STAMP && b!=WheelBuild.end() && b->second<NETDISC_BUILD_STAMP) out.push_back("Its party app is older than this server's; it updates itself in Aurora");
    }
    // Every guest here has their profile on it (Marcus: on every party console).
    std::vector<GuestProfile> records=GuestProfilesNow();
    std::vector<std::string> here=GuestsHere(p), missing, none;
    for(size_t i=0;i<here.size();++i) {
        GuestProfile* r=GuestProfileNamed(records,here[i]);
        if(!r || r->xuid.empty()) none.push_back(here[i]);
        else if(std::find(r->on.begin(),r->on.end(),address)==r->on.end()) missing.push_back(here[i]);
    }
    auto names=[](const std::vector<std::string>& list) {
        std::string text;
        for(size_t i=0;i<list.size();++i) text+=(i ? (i+1==list.size() ? " and " : ", ") : "")+list[i];
        return text;
    };
    if(!missing.empty()) out.push_back(names(missing)+(missing.size()==1 ? "'s profile isn't" : "'s profiles aren't")+" on it yet");
    if(!none.empty()) out.push_back(names(none)+(none.size()==1 ? " has" : " have")+" no profile yet");
    // Its seated players signed in on its party screen.
    std::lock_guard<std::mutex> hold(PartyLock);
    std::vector<const PartySignInRecord*> in=SignedIn(p,address);
    int signedIn=0;
    for(size_t i=0;i<in.size();++i) if(in[i]) ++signedIn;
    if(p.drawn && !in.empty() && signedIn<int(in.size()))
        out.push_back(Number(signedIn)+" of "+Number(int(in.size()))+" seated players signed in");
    return out;
}

// The party as the page reads it (in /api/state).
void PartyJson(web::Json& j, const Party& p, const PartyPictures& pictures) {
    j.Open('{').Set("active",p.active).Set("name",p.name);
    j.Key("theme").Open('{').Set("preset",p.theme.preset).Set("accent",p.theme.accent).Set("accent2",p.theme.accent2);
    j.Set("person",p.theme.person).Set("age",p.theme.age).Set("logo",!pictures.logo.empty()).Set("backgrounds",int(pictures.backgrounds.size()));
    j.Set("pictures",pictures.version).Close('}');
    j.Set("gamemaster",p.gamemaster).Key("consoles");
    NamesJson(j,p.consoles);
    // Each guest, with what they did at the parties (GuestStats).
    std::vector<GuestStats> stats=GuestStatsNow();
    j.Key("guests").Open('[');
    for(size_t i=0;i<p.guests.size();++i) {
        j.Open('{').Set("name",p.guests[i].name).Set("here",p.guests[i].here);
        const GuestStats* g=NULL;
        for(size_t k=0;k<stats.size() && !g;++k) if(SameName(stats[k].name,p.guests[i].name)) g=&stats[k];
        if(g) {
            j.Key("stats").Open('{').Set("gamerscore",g->gamerscore).Set("achievements",g->achievements).Set("readAt",g->readAt)
             .Set("earned",p.active && g->partyStart>=0 && g->gamerscore>=g->partyStart ? g->gamerscore-g->partyStart : 0)
             .Set("playSeconds",g->playSeconds).Set("plays",g->plays).Set("lastGame",g->lastGame)
             .Set("parties",g->parties).Set("lastParty",g->lastParty).Close('}');
        }
        j.Close('}');
    }
    j.Close(']').Set("statsReading",StatsReading.load()).Set("teams",p.teams).Set("balance",BalanceNames[p.balance]).Set("game",p.game).Set("started",p.started).Set("idleOffMinutes",p.idleOffMinutes);
    // Join from a phone: the relay, the party code and its page, and how posting goes.
    {
        RelayState r;
        if(!p.phoneRelay.empty()) { std::lock_guard<std::mutex> hold(RelayLock); r=RelayNow; }
        j.Key("phone").Open('{').Set("relay",p.phoneRelay).Set("code",p.phoneRelay.empty() ? std::string() : p.phoneCode)
         .Set("url",p.phoneRelay.empty() || p.phoneCode.empty() ? std::string() : p.phoneRelay+"/"+p.phoneCode)
         .Set("ok",r.ok).Set("error",r.error).Set("joins",r.joins).Close('}');
    }
    j.Key("vote");
    VoteJson(j);
    // The live feed, newest first.
    j.Key("feed").Open('[');
    {
        std::lock_guard<std::mutex> hold(FeedLock);
        for(size_t i=Feed.size(),n=0;i-->0 && n<FeedShown;++n) j.Open('{').Set("at",Feed[i].at).Set("kind",Feed[i].kind).Set("text",Feed[i].text).Close('}');
    }
    j.Close(']');
    // Rounds won, and the points they give (most first).
    j.Key("wins").Open('[');
    for(size_t i=0;i<p.wins.size();++i) {
        j.Open('{').Set("round",p.wins[i].round).Set("game",p.wins[i].game).Set("team",p.wins[i].team).Set("at",p.wins[i].at).Key("players");
        NamesJson(j,p.wins[i].players);
        j.Close('}');
    }
    j.Close(']').Key("points").Open('[');
    std::vector<std::pair<std::string,int> > points=PartyPoints(p);
    for(size_t i=0;i<points.size();++i) j.Open('{').Set("name",points[i].first).Set("points",points[i].second).Close('}');
    j.Close(']').Key("teamColours").Open('[');
    for(size_t i=0;i<p.teamColours.size();++i) j.Int(p.teamColours[i]);
    j.Close(']').Key("palette").Open('[');
    for(int i=0;i<TeamColoursCount;++i)
        j.Open('{').Set("name",TeamPalette[i].name).Set("fill",TeamPalette[i].fill).Set("text",TeamPalette[i].text).Set("bright",TeamPalette[i].bright).Set("ink",TeamPalette[i].darkInk).Close('}');
    j.Close(']').Key("together");
    PairsJson(j,p.together);
    j.Key("apart");
    PairsJson(j,p.apart);
    j.Set("round",p.round).Key("drawn");
    if(p.drawn) {
        j.Open('{').Key("teams");
        TeamsJson(j,p.drawnTeams);
        j.Key("seats");
        SeatsJson(j,p.seats);
        j.Set("note",p.note).Set("kept",p.kept).Close('}');
    } else j.Null();
    j.Key("last");
    TeamsJson(j,p.last);
    j.Key("spin");
    SpinJson(j,p.spin);
    // Who signed in on each console's party screen: for each seat, the
    // controller (1-4) and gamertag, or null.
    j.Key("signedIn").Open('{');
    {
        std::lock_guard<std::mutex> hold(PartyLock);
        for(size_t i=0;i<p.seats.size();++i) {
            std::vector<const PartySignInRecord*> seats=SignedIn(p,p.seats[i].first);
            j.Key(p.seats[i].first.c_str()).Open('[');
            for(size_t k=0;k<seats.size();++k)
                if(seats[k]) j.Open('{').Set("controller",seats[k]->controller+1).Set("gamertag",seats[k]->gamertag).Close('}');
                else j.Null();
            j.Close(']');
        }
    }
    j.Close('}').Key("ready").Open('{');
    for(size_t i=0;i<p.consoles.size();++i) {
        std::vector<std::string> problems=NotReady(p,p.consoles[i]);
        j.Key(p.consoles[i].c_str()).Open('[');
        for(size_t k=0;k<problems.size();++k) j.String(problems[k]);
        j.Close(']');
    }
    j.Close('}').Key("wheelOn").Open('[');
    {
        std::lock_guard<std::mutex> hold(WheelSeenLock);
        double now=Now();
        for(std::map<std::string,double>::const_iterator i=WheelSeen.begin();i!=WheelSeen.end();++i) if(now-i->second<WheelSeenSeconds) j.String(i->first);
    }
    j.Close(']');
    // The seated guests' profiles going to their consoles (PushSeatProfiles).
    j.Key("profilesTo").Open('{');
    {
        std::lock_guard<std::mutex> hold(GuestLock);
        for(std::map<std::string,SeatProfiles>::const_iterator i=SeatPushes.begin();i!=SeatPushes.end();++i)
            j.Key(i->first.c_str()).Open('{').Set("state",i->second.state).Set("done",i->second.done).Set("total",i->second.total)
             .Set("now",i->second.now).Set("note",i->second.note).Close('}');
    }
    j.Close('}');
    j.Close('}');
}

// The title updates of the consoles compared (see LookTitleUpdates): those
// in only (the party's, in its order), or every console when it's empty.
// read: they were read once at least (readAt, time()); error: why the latest
// look failed.
struct ConsoleUpdates { std::string address; bool read; long long readAt; std::string error; std::vector<TitleUpdate> updates; };
std::vector<ConsoleUpdates> UpdatesOf(const std::vector<std::string>& only) {
    std::vector<Console*> consoles=AllConsoles();
    std::vector<ConsoleUpdates> out;
    std::lock_guard<std::mutex> hold(ConsoleLock);
    for(size_t k=0;k<(only.empty() ? consoles.size() : only.size());++k)
        for(size_t i=0;i<consoles.size();++i) {
            const Console& c=*consoles[i];
            if(only.empty() ? i!=k : only[k]!=c.text) continue;
            if(only.empty() && c.hiddenAt!=-2 && c.seenAt<=c.hiddenAt && !c.connections) continue;  // Forgotten.
            ConsoleUpdates u;
            u.address=c.text; u.read=c.titleUpdatesRead!=0; u.readAt=c.titleUpdatesRead; u.error=c.titleUpdatesError; u.updates=c.titleUpdates;
            out.push_back(u);
        }
    return out;
}

// The consoles whose title updates are compared: the party's while it's on, else all (empty).
std::vector<std::string> ComparedConsoles() {
    Party p=CurrentParty();
    return p.active ? p.consoles : std::vector<std::string>();
}

// A title update as the page names it: "Title Update 8".
std::string UpdateName(const TitleUpdate& u) {
    if(u.version) return "Title Update "+Number(u.version);
    return u.name.empty() ? u.file : u.name;
}

// One game's title updates on consoles: the one each console that was read
// applies (NULL: none), the newest that any of them has (the highest
// version, then base version; of those for the game's own disc, its Media
// ID, when there are such), and whether the ones applied differ. The
// pointers are into consoles, which has to outlive the answer.
struct GameUpdates { std::vector<std::pair<std::string,const TitleUpdate*> > inUse; const TitleUpdate* newest; bool mismatch; };
GameUpdates JudgeUpdates(DWORD titleId, DWORD mediaId, const std::vector<ConsoleUpdates>& consoles) {
    GameUpdates g;
    g.newest=NULL; g.mismatch=false;
    if(!titleId) return g;
    bool ownDisc=false;
    for(size_t c=0;c<consoles.size() && mediaId;++c)
        for(size_t i=0;i<consoles[c].updates.size();++i) ownDisc=ownDisc || (consoles[c].updates[i].titleId==titleId && consoles[c].updates[i].mediaId==mediaId);
    std::vector<unsigned> versions;
    for(size_t c=0;c<consoles.size();++c) {
        if(!consoles[c].read) continue;
        const TitleUpdate* use=NULL;
        for(size_t i=0;i<consoles[c].updates.size();++i) {
            const TitleUpdate& u=consoles[c].updates[i];
            if(u.titleId!=titleId) continue;
            if((!ownDisc || u.mediaId==mediaId) && (!g.newest || u.version>g.newest->version || (u.version==g.newest->version && u.baseVersion>g.newest->baseVersion))) g.newest=&u;
            // Of those applied (one each disc), the one for this disc, else the highest.
            bool better=!use || ((u.mediaId==mediaId)!=(use->mediaId==mediaId) ? u.mediaId==mediaId : u.version>use->version);
            if(u.active && better) use=&u;
        }
        g.inUse.push_back(std::make_pair(consoles[c].address,use));
        unsigned version=use ? use->version : 0;
        if(std::find(versions.begin(),versions.end(),version)==versions.end()) versions.push_back(version);
    }
    g.mismatch=versions.size()>1;
    return g;
}

// The dashboard asks for its state every second; what it shows of the
// network and the game folders changes rarely, so those are looked up at most
// every few seconds (a NAS folder can take a while to answer).
std::mutex StateCacheLock;
std::vector<std::string> CachedAddresses;
double AddressesAt=-1;
std::map<std::string,std::pair<bool,double> > CachedFolders;  // Folder: found, when looked.
std::vector<std::string> RecentPcAddresses() {
    {
        std::lock_guard<std::mutex> hold(StateCacheLock);
        if(AddressesAt>=0 && Now()-AddressesAt<10) return CachedAddresses;
    }
    std::vector<std::string> fresh=PcAddresses();
    std::lock_guard<std::mutex> hold(StateCacheLock);
    CachedAddresses=fresh; AddressesAt=Now();
    return fresh;
}
bool RecentlyFolder(const std::string& folder) {
    {
        std::lock_guard<std::mutex> hold(StateCacheLock);
        std::map<std::string,std::pair<bool,double> >::iterator i=CachedFolders.find(folder);
        if(i!=CachedFolders.end() && Now()-i->second.second<5) return i->second.first;
    }
    bool found=config::IsFolder(folder);
    std::lock_guard<std::mutex> hold(StateCacheLock);
    if(CachedFolders.size()>64) CachedFolders.clear();
    CachedFolders[folder]=std::make_pair(found,Now());
    return found;
}

void State(web::Reply& out) {
    Options o;
    std::vector<KnownConsole> known;
    { std::lock_guard<std::mutex> hold(SettingsLock); o=Settings; known=Known; }
    web::Json j;
    j.Open('{').Set("name","NetDisc360").Set("version",NETDISC_VERSION).Set("commit",NETDISC_COMMIT).Set("built",__DATE__ " " __TIME__);
#ifdef _WIN32
    j.Set("platform","windows");
#else
    j.Set("platform","linux");
#endif
    j.Set("tray",InTray).Set("time",(long long)time(NULL)).Set("uptime",Now()-StartedAt);
    j.Key("ports").Open('{').Set("netiso",o.port).Set("web",o.webPort).Close('}');
    j.Key("addresses").Open('[');
    std::vector<std::string> addresses=RecentPcAddresses();
    for(size_t i=0;i<addresses.size();++i) j.String(addresses[i]);
    j.Close(']');

    j.Key("settings").Open('{');
    j.Key("xboxes").Open('[');
    for(size_t i=0;i<o.xboxes.size();++i) { char text[16]; AddressText(o.xboxes[i],text,sizeof(text)); j.String(text); }
    j.Close(']');
    j.Set("ftpUser",o.ftpUser).Set("ftpPasswordSet",!o.ftpPassword.empty());  // Never the password itself.
    j.Set("push",o.push).Set("canPush",CanPush).Set("category",o.filter).Set("scan",o.scan);
    j.Set("xboxFolder",o.xboxFolder).Set("aurora",o.aurora).Set("setupDone",o.setupDone).Set("wizardDone",o.wizardDone);
    j.Key("preset").Open('{').Set("folders",Given.folders).Set("xboxes",Given.xboxes).Set("ftp",Given.ftp).Close('}');
    j.Set("saved",!o.configPath.empty()).Set("webLan",o.webLan).Set("webAddress",WebAddress(o)).Set("lanSwitch",o.webAddress=="127.0.0.1");
    j.Set("password",!web::PasswordHash().empty()).Set("passwordFixed",PasswordFixed);
    j.Set("backupEvery",o.backupEvery).Set("backupKeep",o.backupKeep);
    j.Set("updateCheck",o.updateCheck).Set("updateAuto",o.updateAuto).Set("updatePrerelease",o.updatePrerelease);
    j.Set("skipInstalled",o.skipInstalled).Set("installedFolders",o.installedFolders).Set("auroraScripts",o.auroraScripts);
    j.Set("restartAurora",o.restartAurora).Set("auroraWebUser",o.auroraWebUser).Set("auroraWebPasswordSet",!o.auroraWebPassword.empty());
    j.Set("auroraWebPort",o.auroraWebPort).Set("followAddress",o.followAddress).Set("keepAwake",o.keepAwake).Set("autoOffMinutes",o.autoOffMinutes);
    j.Set("showName",o.showName).Set("tidyAurora",o.tidyAurora).Set("pluginSet",o.pluginSet).Set("pluginKit",o.pluginKit);
#ifdef _WIN32
    if(InTray) j.Set("startWithWindows",StartsWithWindows());
#endif
    j.Close('}');
    j.Key("files").Open('{').Set("settings",o.configPath).Set("discMap",DiscMapPath(o.configPath));
    j.Set("backups",o.configPath.empty() ? std::string() : config::FolderOf(o.configPath)+"Backups").Close('}');
    UpdateJson(j,o,false);

    std::vector<IsoFile> games;
    long long version;
    bool scanned;
    { std::lock_guard<std::mutex> hold(GamesListLock); games=GamesList; version=GamesVersion; scanned=GamesScanned; }
    int duplicates=0,conflicts=0;
    {
        std::lock_guard<std::mutex> hold(DuplicatesLock);
        for(size_t i=0;i<Conflicts.size();++i) { duplicates+=int(Conflicts[i].copies.size())-1; conflicts+=Conflicts[i].open; }
    }
    { std::lock_guard<std::mutex> hold(GamesListLock); version=GamesVersion; }
    j.Key("library").Open('{').Set("games",int(games.size())).Set("duplicates",duplicates).Set("conflicts",conflicts);
    j.Set("version",version).Set("scanned",scanned).Close('}');
    j.Key("folders").Open('[');
    for(size_t i=0;i<o.folders.size();++i) {
        int count=0;
        for(size_t g=0;g<games.size();++g) if(Within(games[g].path,o.folders[i])) ++count;
        bool emptied;
        { std::lock_guard<std::mutex> hold(MissingLock); emptied=EmptiedFolders.count(o.folders[i])!=0; }
        j.Open('{').Set("path",config::ToUtf8(o.folders[i])).Set("games",count).Set("found",RecentlyFolder(o.folders[i])).Set("emptied",emptied).Close('}');
    }
    j.Close(']');

    std::vector<Console*> consoles=AllConsoles();
    U64 total=0;
    double rate=0;
    j.Key("consoles").Open('[');
    {
        std::lock_guard<std::mutex> hold(ConsoleLock);
        long long last=RateSecond;
        for(size_t i=0;i<consoles.size();++i) {
            Console& c=*consoles[i];
            total+=c.sent;
            rate+=c.RateAt(last);
            long long at=c.seenAt;
            if(c.hiddenAt!=-2 && at<=c.hiddenAt && !c.connections) continue;  // Forgotten.
            // Its own name, or as last read (the settings file's).
            std::string name=c.name, id=c.id;
            for(size_t k=0;k<known.size() && name.empty();++k) if(known[k].address==c.text) { name=known[k].name; id=known[k].id; }
            j.Open('{').Set("address",c.text).Set("name",name).Set("id",id).Set("connections",c.connections.load()).Set("sent",(unsigned long long)c.sent.load());
            j.Set("ftp",c.ftp.load()).Set("synced",c.synced.load()).Set("mounted",config::ToUtf8(WithoutIso(c.mounted))).Set("push",c.push.load());
            if(c.pushTotal) j.Key("pushing").Open('{').Set("done",c.pushDone.load()).Set("total",c.pushTotal.load()).Close('}');
            j.Set("playing",c.connections>0 && !c.mounted.empty() && !c.inAurora).Set("inAurora",c.inAurora.load());
            bool answers=c.answers && !c.offline;  // Before pingMs: PingLoop sets pingMs first, so answers implies a ping time.
            j.Set("ping",c.pingMs.load()).Set("answers",answers).Set("offline",c.offline.load());
            long long answered=c.answeredAt;
            j.Set("answered",answered<0 ? 0LL : (long long)time(NULL)-(long long)(Now()-answered/1000.0));
            j.Set("seen",at<0 ? 0LL : (long long)time(NULL)-(long long)(Now()-at/1000.0));
            j.Key("rate").Open('[');
            for(long long s=last-RateSeconds+1;s<=last;++s) j.Number(c.RateAt(s));
            j.Close(']').Key("events").Open('[');
            for(size_t e=0;e<c.recent.size();++e)
                j.Open('{').Set("time",c.recent[e].time).Set("kind",EventKinds[c.recent[e].kind]).Set("text",config::ToUtf8(c.recent[e].text)).Close('}');
            j.Close(']').Close('}');
        }
    }
    for(size_t k=0;k<known.size();++k) {
        bool listed=false;
        for(size_t i=0;i<consoles.size() && !listed;++i) listed=known[k].address==consoles[i]->text;
        if(listed) continue;
        j.Open('{').Set("address",known[k].address).Set("name",known[k].name).Set("id",known[k].id).Set("connections",0).Set("sent",0).Set("ftp",0).Set("synced",0LL).Set("mounted","");
        j.Set("playing",false).Set("inAurora",false).Set("ping",-1).Set("answers",false).Set("offline",false).Set("answered",0LL);
        j.Set("push",o.xboxes.empty() || std::find(o.xboxes.begin(),o.xboxes.end(),inet_addr(known[k].address.c_str()))!=o.xboxes.end());
        j.Set("seen",known[k].seen).Key("rate").Open('[').Close(']').Key("events").Open('[').Close(']').Close('}');
    }
    j.Close(']');
    double peak;
    { std::lock_guard<std::mutex> hold(ConsoleLock); peak=Peak; }
    j.Key("totals").Open('{').Set("sent",(unsigned long long)total).Set("rate",rate).Set("peak",peak).Close('}');

    Party party=CurrentParty();
    j.Key("party");
    PartyJson(j,party,PicturesNow(o));
    // Games whose consoles (the party's while it's on) apply different title updates.
    std::vector<ConsoleUpdates> updates=UpdatesOf(party.active ? party.consoles : std::vector<std::string>());
    std::set<DWORD> mismatched;
    for(size_t i=0;i<games.size();++i) if(!mismatched.count(games[i].titleId) && JudgeUpdates(games[i].titleId,games[i].mediaId,updates).mismatch) mismatched.insert(games[i].titleId);
    j.Key("titleUpdates").Open('{').Set("mismatches",int(mismatched.size())).Close('}');

    j.Key("events").Open('[');
    {
        std::lock_guard<std::mutex> hold(EventLock);
        for(size_t i=Events.size()>50 ? Events.size()-50 : 0;i<Events.size();++i) {
            const Happening& h=Events[i];
            j.Open('{').Set("id",h.id).Set("time",h.time).Set("kind",EventKinds[h.kind]).Set("text",config::ToUtf8(h.text)).Close('}');
        }
    }
    j.Close(']');
    { std::lock_guard<std::mutex> hold(LogLock); j.Set("logId",LogCount); }
    j.Close('}');
    out.body=j.Text();
}

const char* KindName(DiscKind kind) { return kind==DiscIso ? "iso" : kind==DiscGod ? "god" : "folder"; }

void GameJson(web::Json& j, const IsoFile& g, const std::vector<std::string>& folders) {
    char title[12]; _snprintf_s(title,sizeof(title),_TRUNCATE,"%08X",g.titleId);
    int folder=-1;
    for(size_t f=0;f<folders.size() && folder<0;++f) if(Within(g.path,folders[f])) folder=int(f);
    U64 size=g.size;
    bool known=g.kind!=DiscFolder;
    if(!known) {
        std::map<std::string,std::pair<long long,U64> >::const_iterator s=FolderSizes.find(g.path);
        if(s!=FolderSizes.end()) { size=s->second.second; known=true; }
    }
    j.Set("name",config::ToUtf8(WithoutIso(g.name))).Set("path",config::ToUtf8(g.path)).Set("kind",KindName(g.kind));
    j.Set("size",(unsigned long long)size).Set("sizeKnown",known).Set("titleId",g.titleId ? std::string(title) : std::string());
    j.Set("disc",g.disc).Set("folder",folder).Set("installDisc",config::ToUtf8(WithoutIso(g.installDisc)));
    j.Set("role",g.role).Set("roleLabel",g.roleLabel).Set("incomplete",g.incomplete);
    // System Link (see systemlink.h): players on one console and in all, null where it isn't known.
    const SystemLinkGame* link=g.titleId ? FindSystemLink(g.titleId) : NULL;
    j.Set("systemLink",link!=NULL).Key("perConsole");
    if(link && link->perConsole) j.Int(link->perConsole); else j.Null();
    j.Key("total");
    if(link && link->total) j.Int(link->total); else j.Null();
    j.Set("xbe",g.xbe).Set("xefu",g.xefu).Set("xefuWhy",g.xefuWhy);
}

void Games(web::Reply& out) {
    std::vector<std::string> folders=Current().folders;
    // The consoles with a tile for each game (by its listed name, in lower
    // case), and with it installed (by Title ID and disc), from their latest push.
    std::map<std::string,std::vector<std::string> > tiles;
    std::map<std::pair<DWORD,int>,std::vector<std::string> > installed;
    std::vector<Console*> consoles=AllConsoles();
    {
        std::lock_guard<std::mutex> hold(ConsoleLock);
        for(size_t i=0;i<consoles.size();++i) {
            const Console& c=*consoles[i];
            for(std::map<std::string,std::string>::const_iterator t=c.tiles.begin();t!=c.tiles.end();++t) tiles[t->first].push_back(c.text);
            for(size_t k=0;k<c.installed.size();++k) {
                std::vector<std::string>& on=installed[std::make_pair(c.installed[k].titleId,c.installed[k].disc)];
                if(std::find(on.begin(),on.end(),std::string(c.text))==on.end()) on.push_back(c.text);
            }
        }
    }
    // The title updates of the consoles compared (the party's while it's on).
    std::vector<ConsoleUpdates> updates=UpdatesOf(ComparedConsoles());
    // The play history, with the games being played now counted so far.
    std::map<std::string,Played> plays;
    { std::lock_guard<std::mutex> hold(PlaysLock); plays=Plays; }
    {
        std::lock_guard<std::mutex> hold(ConsoleLock);
        for(size_t i=0;i<consoles.size();++i)
            if(!consoles[i]->game.empty()) plays[Lower(consoles[i]->game)].seconds+=std::max(0.0,Now()-consoles[i]->gameSince);
    }
    web::Json j;
    j.Open('{');
    {
        std::lock_guard<std::mutex> hold(GamesListLock);
        j.Set("version",GamesVersion).Key("games").Open('[');
        for(size_t i=0;i<GamesList.size();++i) {
            const IsoFile& g=GamesList[i];
            j.Open('{');
            GameJson(j,g,folders);
            std::map<std::string,std::vector<std::string> >::const_iterator t=tiles.find(Lower(g.name));
            j.Key("tiles").Open('[');
            for(size_t k=0;t!=tiles.end() && k<t->second.size();++k) j.String(t->second[k]);
            j.Close(']');
            std::map<std::string,Played>::const_iterator p=plays.find(Lower(config::ToUtf8(WithoutIso(g.name))));
            if(p!=plays.end()) j.Set("plays",p->second.times).Set("played",p->second.seconds).Set("lastPlayed",p->second.last);
            std::map<std::pair<DWORD,int>,std::vector<std::string> >::const_iterator n=g.titleId ? installed.find(std::make_pair(g.titleId,g.disc)) : installed.end();
            j.Key("installedOn").Open('[');
            for(size_t k=0;n!=installed.end() && k<n->second.size();++k) j.String(n->second[k]);
            j.Close(']');
            // The title update each console applies to it (null: none), and whether they differ.
            GameUpdates u=JudgeUpdates(g.titleId,g.mediaId,updates);
            j.Key("titleUpdates").Open('{');
            for(size_t k=0;k<u.inUse.size();++k) {
                j.Key(u.inUse[k].first.c_str());
                if(u.inUse[k].second) j.String(UpdateName(*u.inUse[k].second)); else j.Null();
            }
            j.Close('}').Set("tuMismatch",u.mismatch).Close('}');
        }
        j.Close(']');
    }
    // Each game that is there more than once, with every copy, the served one first.
    j.Key("conflicts").Open('[');
    {
        std::lock_guard<std::mutex> hold(DuplicatesLock);
        std::lock_guard<std::mutex> sizes(GamesListLock);
        for(size_t i=0;i<Conflicts.size();++i) {
            const Conflict& c=Conflicts[i];
            j.Open('{').Set("key",c.key).Set("name",config::ToUtf8(c.name)).Set("served",config::ToUtf8(c.served.path));
            j.Set("chosen",c.chosen).Set("open",c.open).Key("copies").Open('[');
            for(size_t k=0;k<c.copies.size();++k) { j.Open('{'); GameJson(j,c.copies[k],folders); j.Close('}'); }
            j.Close(']').Close('}');
        }
    }
    j.Close(']').Close('}');
    out.body=j.Text();
}

void LogSince(long long after, web::Reply& out) {
    web::Json j;
    j.Open('{').Key("lines").Open('[');
    std::lock_guard<std::mutex> hold(LogLock);
    size_t first=0;
    while(first<LogLines.size() && LogLines[first].id<=after) ++first;
    if(LogLines.size()-first>1000) first=LogLines.size()-1000;
    for(size_t i=first;i<LogLines.size();++i) j.Open('{').Set("id",LogLines[i].id).Set("text",config::ToUtf8(LogLines[i].text)).Close('}');
    j.Close(']').Set("last",LogCount).Close('}');
    out.body=j.Text();
}

// The folder browser of the "Add a game folder" dialog -------------------------

bool ListFolders(const std::string& folder, std::vector<std::string>& out) {
#ifdef _WIN32
    WIN32_FIND_DATAA found;
    std::string pattern=folder+(folder[folder.size()-1]=='\\' ? "*" : "\\*");
    HANDLE search=FindFirstFileA(pattern.c_str(),&found);
    if(search==INVALID_HANDLE_VALUE) return false;
    do {
        if(!(found.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) || !strcmp(found.cFileName,".") || !strcmp(found.cFileName,"..")) continue;
        if(found.dwFileAttributes&(FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_SYSTEM)) continue;
        out.push_back(found.cFileName);
    } while(FindNextFileA(search,&found) && out.size()<5000);
    FindClose(search);
#else
    DIR* dir=opendir(folder.c_str());
    if(!dir) return false;
    while(struct dirent* d=readdir(dir)) {
        if(d->d_name[0]=='.' || out.size()>=5000) continue;
        if(config::IsFolder((folder=="/" ? folder : folder+"/")+d->d_name)) out.push_back(d->d_name);
    }
    closedir(dir);
#endif
    std::sort(out.begin(),out.end(),[](const std::string& a, const std::string& b) { return _stricmp(a.c_str(),b.c_str())<0; });
    return true;
}

std::string Child(const std::string& folder, const std::string& name) {
    char last=folder[folder.size()-1];
#ifdef _WIN32
    return folder+(last=='\\' || last=='/' ? "" : "\\")+name;
#else
    return folder+(last=='/' ? "" : "/")+name;
#endif
}

// Whether a folder looks like it has games: an ISO, an extracted game's
// default.xex or a GOD package's 00007000 folder, within depth levels.
bool HasGames(const std::string& folder, int depth) {
    std::vector<std::string> folders;
#ifdef _WIN32
    WIN32_FIND_DATAA found;
    HANDLE search=FindFirstFileA(Child(folder,"*").c_str(),&found);
    if(search==INVALID_HANDLE_VALUE) return false;
    bool games=false;
    do {
        std::string name=found.cFileName;
        if(name=="." || name=="..") continue;
        if(found.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) {
            if(!_stricmp(name.c_str(),"00007000")) games=true;
            else folders.push_back(name);
        } else games=(name.size()>4 && !_stricmp(name.c_str()+name.size()-4,".iso")) || !_stricmp(name.c_str(),"default.xex");
    } while(!games && FindNextFileA(search,&found));
    FindClose(search);
#else
    DIR* dir=opendir(folder.c_str());
    if(!dir) return false;
    bool games=false;
    while(struct dirent* d=readdir(dir)) {
        std::string name=d->d_name;
        if(name=="." || name=="..") continue;
        if(config::IsFolder(Child(folder,name))) {
            if(name=="00007000") games=true;
            else folders.push_back(name);
        } else if((name.size()>4 && !_stricmp(name.c_str()+name.size()-4,".iso")) || !_stricmp(name.c_str(),"default.xex")) games=true;
        if(games) break;
    }
    closedir(dir);
#endif
    for(size_t i=0;i<folders.size() && !games && depth>0 && i<200;++i) games=HasGames(Child(folder,folders[i]),depth-1);
    return games;
}

void Place(web::Json& j, const std::string& name, const std::string& path) {
    if(config::IsFolder(path)) j.Open('{').Set("name",config::ToUtf8(name)).Set("path",config::ToUtf8(path)).Close('}');
}

void Browse(const std::string& asked, web::Reply& out) {
    std::string path=asked.empty() ? std::string() : CleanFolder(config::FromUtf8(asked));
    web::Json j;
    j.Open('{');
    j.Key("places").Open('[');
#ifdef _WIN32
    char drives[256];
    DWORD n=GetLogicalDriveStringsA(sizeof(drives),drives);
    for(const char* d=drives;n && n<sizeof(drives) && *d;d+=strlen(d)+1) Place(j,std::string(d,2),d);
    if(const char* home=getenv("USERPROFILE")) {
        static const char* const names[]={"Desktop","Downloads","Documents","Videos"};
        for(int i=0;i<4;++i) Place(j,names[i],Child(home,names[i]));
    }
    if(path.empty()) {
        j.Close(']').Set("path","").Set("parent","").Key("folders").Open('[');
        for(const char* d=drives;n && n<sizeof(drives) && *d;d+=strlen(d)+1) j.String(std::string(d,2)+"\\");
        j.Close(']').Set("games",false).Set("separator","\\").Close('}');
        out.body=j.Text();
        return;
    }
#else
    Place(j,"/","/");
    Place(j,"/games","/games");
    Place(j,"/mnt","/mnt");
    Place(j,"/media","/media");
    if(const char* home=getenv("HOME")) Place(j,"Home",home);
    if(path.empty()) path="/";
#endif
    j.Close(']');
    std::vector<std::string> folders;
    if(!ListFolders(path,folders)) { out.Error(404,"Can't open "+config::ToUtf8(path)+"."); return; }
    std::string parent;
    size_t cut=path.find_last_of("/\\");
#ifdef _WIN32
    if(path.size()>3 && cut!=std::string::npos) parent=CleanFolder(path.substr(0,cut));
#else
    if(path!="/" && cut!=std::string::npos) parent=cut ? path.substr(0,cut) : "/";
#endif
    j.Set("path",config::ToUtf8(path)).Set("parent",config::ToUtf8(parent));
    j.Key("folders").Open('[');
    for(size_t i=0;i<folders.size();++i) j.String(config::ToUtf8(folders[i]));
    j.Close(']').Set("games",HasGames(path,2));
#ifdef _WIN32
    j.Set("separator","\\");
#else
    j.Set("separator","/");
#endif
    j.Close('}');
    out.body=j.Text();
}

// Changes from the dashboard ---------------------------------------------------

bool Text(const web::Value& v, const char* key, std::string& out) {
    const web::Value* f=v.Get(key);
    if(!f || f->type!=web::Value::String) return false;
    out=f->text;
    return true;
}

bool Bool(const web::Value& v, const char* key, bool& out) {
    const web::Value* f=v.Get(key);
    if(!f || f->type!=web::Value::Bool) return false;
    out=f->boolean;
    return true;
}

// The setup wizard ---------------------------------------------------------------

// Finding consoles: every address of the /24 around this computer's (or the
// one asked for) is tried on Aurora's FTP port and xbdm's (730), and what
// greets there tells an Xbox: Aurora's FtpDll, another Xbox FTP server, or
// xbdm's "201- connected". The Xbox answers no ICMP ping, so it's TCP.
const int XbdmPort = 730;
struct Probe { DWORD address; int port; SOCKET s; bool open; std::string greeting; };

void ProbeBatch(std::vector<Probe>& batch, int timeoutMs) {
    for(size_t i=0;i<batch.size();++i) {
        Probe& p=batch[i];
        p.s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        if(p.s==INVALID_SOCKET) continue;
        u_long nonblocking=1;
        ioctlsocket(p.s,FIONBIO,&nonblocking);
        sockaddr_in to; memset(&to,0,sizeof(to)); to.sin_family=AF_INET; to.sin_addr.s_addr=p.address; to.sin_port=htons(u_short(p.port));
        int status=connect(p.s,reinterpret_cast<sockaddr*>(&to),sizeof(to)) ? WSAGetLastError() : 0;
        if(status && status!=WSAEWOULDBLOCK) { closesocket(p.s); p.s=INVALID_SOCKET; }
        else if(!status) p.open=true;
    }
    // Connected, then greeted: each within the time left.
    double until=Now()+timeoutMs/1000.0;
    for(int phase=0;phase<2;++phase) {
        for(;;) {
            fd_set set,failed; FD_ZERO(&set); FD_ZERO(&failed);
            int waiting=0;
            for(size_t i=0;i<batch.size();++i) {
                Probe& p=batch[i];
                if(p.s==INVALID_SOCKET || (phase==0 ? p.open : !p.open || !p.greeting.empty())) continue;
                FD_SET(p.s,&set); FD_SET(p.s,&failed); ++waiting;
            }
            double left=until-Now();
            if(!waiting || left<=0) break;
            timeval interval={0,long(std::min(left,0.1)*1e6)};
            int rc=phase==0 ? select(0,NULL,&set,&failed,&interval) : select(0,&set,NULL,&failed,&interval);
            if(rc==SOCKET_ERROR) break;
            for(size_t i=0;i<batch.size() && rc>0;++i) {
                Probe& p=batch[i];
                if(p.s==INVALID_SOCKET) continue;
                if(FD_ISSET(p.s,&failed)) { closesocket(p.s); p.s=INVALID_SOCKET; continue; }
                if(!FD_ISSET(p.s,&set)) continue;
                if(phase==0) {
                    int error=0; int length=sizeof(error);
                    if(getsockopt(p.s,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&length) || error) { closesocket(p.s); p.s=INVALID_SOCKET; }
                    else p.open=true;
                } else {
                    char text[200];
                    int got=recv(p.s,text,sizeof(text)-1,0);
                    if(got<=0) { closesocket(p.s); p.s=INVALID_SOCKET; continue; }
                    p.greeting.assign(text,size_t(got));
                }
            }
        }
        until=Now()+timeoutMs/1000.0;  // The greeting gets its own time.
    }
    for(size_t i=0;i<batch.size();++i) if(batch[i].s!=INVALID_SOCKET) { closesocket(batch[i].s); batch[i].s=INVALID_SOCKET; }
}

bool Has(const std::string& text, const char* word) {
    std::string lower=text;
    for(size_t i=0;i<lower.size();++i) lower[i]=char(tolower((unsigned char)lower[i]));
    return lower.find(word)!=std::string::npos;
}

void FindConsoles(const web::Value& body, web::Reply& out) {
    Options o=Current();
    std::vector<std::string> own=PcAddresses();
    std::string around;
    Text(body,"around",around);
    std::vector<DWORD> networks;
    if(!around.empty()) {
        DWORD a=inet_addr(around.c_str());
        if(a==INADDR_NONE) { out.Error(400,"\""+around+"\" isn't an IPv4 address like 192.168.1.20."); return; }
        networks.push_back(htonl(ntohl(a)&0xFFFFFF00));
    }
    for(size_t i=0;i<own.size() && around.empty();++i) {
        DWORD net=htonl(ntohl(inet_addr(own[i].c_str()))&0xFFFFFF00);
        if(std::find(networks.begin(),networks.end(),net)==networks.end()) networks.push_back(net);
    }
    if(networks.size()>4) networks.resize(4);
    std::vector<Probe> probes;
    for(size_t n=0;n<networks.size();++n)
        for(DWORD host=1;host<255;++host) {
            DWORD address=htonl(ntohl(networks[n])+host);
            char text[16]; AddressText(address,text,sizeof(text));
            if(std::find(own.begin(),own.end(),std::string(text))!=own.end()) continue;
            int ports[2]={o.ftpPort,XbdmPort};
            for(int k=0;k<2;++k) { Probe p={address,ports[k],INVALID_SOCKET,false,std::string()}; probes.push_back(p); }
        }
    // Winsock's select takes 64 sockets at most; a few batches at once.
    const size_t Batch=48;
    std::vector<std::thread> threads;
    std::atomic<size_t> next(0);
    for(int t=0;t<6;++t) threads.push_back(std::thread([&]() {
        for(size_t at;(at=next.fetch_add(Batch))<probes.size();) {
            std::vector<Probe> batch(probes.begin()+at,probes.begin()+std::min(probes.size(),at+Batch));
            ProbeBatch(batch,800);
            std::copy(batch.begin(),batch.end(),probes.begin()+at);
        }
    }));
    for(size_t t=0;t<threads.size();++t) threads[t].join();
    struct Found { DWORD address; bool aurora, ftp, xbdm; };
    std::vector<Found> found;
    for(size_t i=0;i<probes.size();++i) {
        const Probe& p=probes[i];
        if(!p.open || p.greeting.empty()) continue;
        bool aurora=p.port==o.ftpPort && Has(p.greeting,"ftpdll");
        bool ftp=p.port==o.ftpPort && !aurora && !p.greeting.compare(0,3,"220") && (Has(p.greeting,"xbox") || Has(p.greeting,"aurora") || Has(p.greeting,"fastftp"));
        bool xbdm=p.port==XbdmPort && !p.greeting.compare(0,3,"201");
        if(!aurora && !ftp && !xbdm) continue;
        size_t k=0;
        while(k<found.size() && found[k].address!=p.address) ++k;
        if(k==found.size()) { Found f={p.address,false,false,false}; found.push_back(f); }
        found[k].aurora=found[k].aurora || aurora; found[k].ftp=found[k].ftp || ftp; found[k].xbdm=found[k].xbdm || xbdm;
    }
    web::Json j;
    j.Open('{').Key("networks").Open('[');
    for(size_t n=0;n<networks.size();++n) { char text[16]; AddressText(networks[n],text,sizeof(text)); j.String(std::string(text)+"/24"); }
    j.Close(']').Key("found").Open('[');
    for(size_t i=0;i<found.size();++i) {
        char text[16]; AddressText(found[i].address,text,sizeof(text));
        j.Open('{').Set("address",text).Set("aurora",found[i].aurora).Set("ftp",found[i].ftp).Set("xbdm",found[i].xbdm)
         .Set("known",ConsoleIfKnown(found[i].address)!=NULL).Close('}');
    }
    j.Close(']').Close('}');
    Log("Looked for consoles on %s: %s.",Count(int(networks.size()),"network","networks").c_str(),Count(int(found.size()),"found","found").c_str());
    out.body=j.Text();
}

// A console found on the network, or typed in: it's listed and gets its game
// folders like one whose NetISO connected.
bool AddConsole(const web::Value& body, std::string& error) {
    std::string text;
    DWORD address;
    if(!Text(body,"address",text) || (address=inet_addr(text.c_str()))==INADDR_NONE || !address) { error="Send the console's IPv4 address, like 192.168.1.30."; return false; }
    bool added;
    if(!ConsoleAt(address,&added)) { error="That's this computer."; return false; }
    if(added) Log("Xbox %s added on the dashboard.",text.c_str());
    ++PushGeneration;
    return true;
}

// DashLaunch's device names in launch.ini, as Aurora's FTP server has them.
std::string FtpPath(std::string path) {
    for(size_t i=0;i<path.size();++i) if(path[i]=='\\') path[i]='/';
    size_t colon=path.find(':');
    if(colon==std::string::npos) return std::string();
    std::string device=path.substr(0,colon), rest=path.substr(colon+1);
    std::string lower=device;
    for(size_t i=0;i<lower.size();++i) lower[i]=char(tolower((unsigned char)lower[i]));
    if(lower=="usb") device="Usb0";
    else if(lower=="hdd") device="Hdd1";
    else if(lower=="usbmu" || lower=="usbmu0") device="UsbMu0";
    if(rest.empty() || rest[0]!='/') rest="/"+rest;
    return "/"+device+rest;
}

std::string Trim(const std::string& text) {
    size_t a=text.find_first_not_of(" \t\r\n\xEF\xBB\xBF"), b=text.find_last_not_of(" \t\r\n");
    return a==std::string::npos ? std::string() : text.substr(a,b-a+1);
}

// DashLaunch's launch.ini on the console: the first one DashLaunch reads
// (USB before the hard drive), its default dashboard and its plugins by slot.
struct LaunchIni { std::string path, defaultPath, text; std::vector<std::pair<int,std::string> > plugins; };
// listFirst: each drive's listing tells where launch.ini is before it's read
// (a read of a file that isn't there can stall FtpDll's next one); without
// it, each place is tried in turn, as the setup wizard does.
bool ReadLaunchIni(FtpClient& ftp, LaunchIni& ini, bool listFirst) {
    static const char* const drives[]={"/Usb0","/Usb1","/Usb2","/Hdd1"};
    std::vector<unsigned char> data;
    ini=LaunchIni();
    for(size_t i=0;i<sizeof(drives)/sizeof(drives[0]) && ini.path.empty();++i) {
        std::string place=std::string(drives[i])+"/launch.ini";
        if(listFirst) {
            std::vector<FtpEntry> entries;
            bool there=false;
            if(!ftp.Enters(drives[i])) { if(ftp.Code()/100==5) continue; return false; }
            if(!ftp.List(drives[i],entries)) return false;
            for(size_t k=0;k<entries.size() && !there;++k) there=!entries[k].folder && !_stricmp(entries[k].name.c_str(),"launch.ini");
            if(!there) continue;
            if(!ftp.Retrieve(place.c_str(),data,256<<10)) return false;
            ini.path=place;
        }
        else if(ftp.Retrieve(place.c_str(),data,256<<10)) ini.path=place;
        else if(!ftp.Missing()) ftp.Close();  // Not there is fine; anything else starts over.
    }
    if(ini.path.empty()) return true;
    std::string file(data.begin(),data.end()), section;
    ini.text=file;
    for(size_t at=0;at<file.size();) {
        size_t end=file.find('\n',at);
        if(end==std::string::npos) end=file.size();
        std::string line=Trim(file.substr(at,end-at));
        at=end+1;
        size_t comment=line.find(';');
        if(comment!=std::string::npos) line=Trim(line.substr(0,comment));
        if(line.empty() || line[0]=='#') continue;
        if(line[0]=='[') { section=Lower(line); continue; }
        size_t equals=line.find('=');
        if(equals==std::string::npos) continue;
        std::string key=Lower(Trim(line.substr(0,equals))), value=Trim(line.substr(equals+1));
        if(section=="[paths]" && key=="default") ini.defaultPath=value;
        if(section=="[plugins]" && key.size()==7 && !key.compare(0,6,"plugin") && key[6]>='1' && key[6]<='9' && !value.empty())
            ini.plugins.push_back(std::make_pair(key[6]-'0',value));
    }
    return true;
}

// What a plugin in launch.ini is, by its file name: tiles (NetDisc360.xex,
// the disc drive too since it took over from NetISO.xex; NetIsoTiles.xex
// before), netiso, usbnet (a USB network adapter's driver: NetUsb360, or
// UsbNet360 and CUsbEnet before it) or other.
const char* PluginKind(const std::string& name) {
    std::string lower=Lower(name);
    return lower.find("netdisc360")!=std::string::npos || lower.find("netisotiles")!=std::string::npos ? "tiles" : lower.find("netiso")!=std::string::npos ? "netiso"
         : lower.find("netusb")!=std::string::npos || lower.find("usbnet")!=std::string::npos || lower.find("cusbenet")!=std::string::npos ? "usbnet" : "other";
}

// Adds this computer's addresses now to OwnAddresses.
void NoteOwnAddresses() {
    std::vector<std::string> now=PcAddresses();
    bool changed=false;
    {
        std::lock_guard<std::mutex> hold(SettingsLock);
        for(size_t i=0;i<now.size();++i) {
            if(std::find(OwnAddresses.begin(),OwnAddresses.end(),now[i])!=OwnAddresses.end()) continue;
            OwnAddresses.push_back(now[i]);
            changed=true;
        }
        while(OwnAddresses.size()>OwnAddressesKept) OwnAddresses.erase(OwnAddresses.begin());
    }
    if(changed) Save();
}

// The server NetDisc360.xex's drive connects to, read over FTP as the plugin
// reads it: server= in the first NetDisc360.ini (or NetIsoTiles.ini, its old
// name) of the plugin's folder and the roots of Usb0: and Hdd1:, else the
// first line of NetISO.xex.txt next to the plugin or on those roots. from is
// the file it came from, empty if none names one.
std::string PluginServerOnConsole(FtpClient& ftp, const std::string& folder, std::string& from) {
    std::vector<unsigned char> data;
    std::vector<std::string> inis;
    static const char* const names[]={"NetDisc360.ini","NetIsoTiles.ini"};
    for(int n=0;n<2;++n) {
        inis.push_back(folder+names[n]);
        inis.push_back(std::string("/Usb0/")+names[n]);
        inis.push_back(std::string("/Hdd1/")+names[n]);
    }
    from.clear();
    for(size_t i=0;i<inis.size();++i) {
        if(!ftp.Retrieve(inis[i].c_str(),data,64<<10)) { if(!ftp.Missing()) ftp.Close(); continue; }
        // The plugin reads the first one there is, with or without server=.
        std::string file(data.begin(),data.end());
        for(size_t at=0;at<file.size();) {
            size_t end=file.find('\n',at);
            if(end==std::string::npos) end=file.size();
            std::string line=Trim(file.substr(at,end-at));
            at=end+1;
            size_t equals=line.find('=');
            if(equals==std::string::npos) continue;
            std::string key=Trim(line.substr(0,equals));
            for(size_t k=0;k<key.size();++k) key[k]=char(tolower((unsigned char)key[k]));
            if(key!="server") continue;
            std::string value=Trim(line.substr(equals+1));
            from=inis[i];
            return value.substr(0,value.find(':'));
        }
        break;
    }
    const std::string txts[]={folder+"NetISO.xex.txt","/Hdd1/netiso.xex.txt","/Usb0/netiso.xex.txt"};
    for(size_t i=0;i<3;++i) {
        if(!ftp.Retrieve(txts[i].c_str(),data,4096)) { if(!ftp.Missing()) ftp.Close(); continue; }
        std::string text(data.begin(),data.end());
        from=txts[i];
        return Trim(text.substr(0,text.find_first_of("\r\n")));
    }
    return std::string();
}

// A router can give this computer a new address (after a restart, say), and
// the file on the console that names the server (NetDisc360.ini's server=
// for NetDisc360.xex's drive, NetISO.xex.txt for NetISO.xex) still names the
// old one: the console finds no server. Before a push the file is made to
// name this computer again when the console has no connection here and sent
// nothing for a while, the file names an address this computer had before
// but not now, no NetISO server answers there, and this computer reaches the
// console from an address on the console's own network (not Docker's). The
// plugins read the file as the console starts, so it needs a restart then.
const double FollowQuietSeconds = 600;
void FollowAddress(Console& console, PushState& s, FtpClient& ftp, const Options& o) {
    long long seen=console.seenAt;
    if(console.connections>0 || (seen>=0 && Now()*1000-seen<FollowQuietSeconds*1000)) return;
    // A step before may have dropped the connection (a failed command does).
    DWORD here=ftp.Open() ? ftp.LocalAddress() : 0;
    if(!here || (ntohl(here)^ntohl(console.address))>>8) return;
    LaunchIni ini;
    if(!ReadLaunchIni(ftp,ini,true)) return;
    char now[16]; AddressText(here,now,sizeof(now));
    std::vector<std::string> current=PcAddresses(), done;
    for(size_t i=0;i<ini.plugins.size();++i) {
        std::string plugin=FtpPath(ini.plugins[i].second);
        if(plugin.empty()) continue;
        const char* kind=PluginKind(plugin.substr(plugin.rfind('/')+1));
        std::string path, old;
        if(!strcmp(kind,"netiso")) {
            path=plugin+".txt";
            std::string folder=path.substr(0,path.rfind('/')), name=path.substr(path.rfind('/')+1);
            std::vector<FtpEntry> entries;
            bool there=false;
            if(!ftp.List(folder.empty() ? "/" : folder.c_str(),entries)) return;
            for(size_t k=0;k<entries.size() && !there;++k) there=!entries[k].folder && !_stricmp(entries[k].name.c_str(),name.c_str());
            if(!there) continue;
        }
        else if(!strcmp(kind,"tiles")) {
            PluginServerOnConsole(ftp,plugin.substr(0,plugin.rfind('/')+1),path);
            if(path.empty()) continue;
        }
        else continue;
        if(std::find(done.begin(),done.end(),Lower(path))!=done.end()) continue;
        done.push_back(Lower(path));
        std::vector<unsigned char> data;
        if(!ftp.Retrieve(path.c_str(),data,64<<10)) { if(!ftp.Missing()) return; continue; }
        std::string text(data.begin(),data.end()), file=path.substr(path.rfind('/')+1);
        // Where the address is: NetISO.xex.txt's first line, or the value of
        // an .ini's server= (an address, and maybe :port after it).
        size_t at=0, end=text.find_first_of("\r\n");
        if(Lower(file).find(".ini")!=std::string::npos) {
            for(at=0;at<text.size();at=end+1) {
                end=text.find('\n',at); if(end==std::string::npos) end=text.size();
                size_t equals=text.find('=',at);
                if(equals<end && Lower(Trim(text.substr(at,equals-at)))=="server") { at=equals+1; break; }
            }
            if(at>=text.size()) continue;
        }
        if(end==std::string::npos) end=text.size();
        while(at<end && (text[at]==' ' || text[at]=='\t')) ++at;
        size_t stop=text.find_first_of(": \t\r\n",at);
        if(stop==std::string::npos || stop>end) stop=end;
        old=text.substr(at,stop-at);
        DWORD before=inet_addr(old.c_str());
        bool had;
        { std::lock_guard<std::mutex> hold(SettingsLock); had=std::find(OwnAddresses.begin(),OwnAddresses.end(),old)!=OwnAddresses.end(); }
        if(old==now || before==INADDR_NONE || !before || !had || std::find(current.begin(),current.end(),old)!=current.end()) continue;
        IsvrClient probe(3);
        if(probe.Connect(before,o.port,3) && probe.Ping()) {
            if(s.followed!=old) XboxLog(console,"its %s names %s, an address this computer had, where a NetISO server answers now; it's left as it is.",file.c_str(),old.c_str());
            s.followed=old;
            continue;
        }
        std::string fixed=text.substr(0,at)+now+text.substr(stop);
        if(!StoreChecked(ftp,path,fixed)) { XboxLog(console,"couldn't write %s: %s",path.c_str(),ftp.Error()); return; }
        XboxLog(console,"its %s (%s) named %s, this computer's address before; it names %s now. Restart the Xbox so it reads it.",file.c_str(),path.c_str(),old.c_str(),now);
        XboxEvent(console,EventWarning,std::string("Restart the Xbox: it looks for this PC at its new address, ")+now);
#ifdef _WIN32
        if(InTray) Notify("Restart your Xbox",std::string("This PC's address changed to ")+now+". "+console.text+" was told; restart the Xbox so it finds the games again.","#consoles");
#endif
    }
}


// What the console has: Aurora's FTP login works, Aurora and the game folder
// are where the settings say, and DashLaunch's launch.ini loads NetDisc360.xex
// (NetIsoTiles.xex before) and NetISO.xex if it's still used (with
// NetISO.xex.txt naming this computer), each file's SHA-256 for the dashboard
// to match with the releases.
void CheckConsole(const web::Value& body, web::Reply& out) {
    Options o=Current();
    std::string text, user=o.ftpUser, password=o.ftpPassword;
    DWORD address;
    if(!Text(body,"address",text) || (address=inet_addr(text.c_str()))==INADDR_NONE || !address) { out.Error(400,"Send the console's IPv4 address, like 192.168.1.30."); return; }
    // The wizard tries a login before saving it.
    if(Text(body,"ftpUser",user) && user.empty()) user=o.ftpUser;
    if(!Text(body,"ftpPassword",password) || password.empty()) password=o.ftpPassword;
    if(std::find_if(user.begin(),user.end(),[](char c) { return (unsigned char)c<' '; })!=user.end() ||
       std::find_if(password.begin(),password.end(),[](char c) { return (unsigned char)c<' '; })!=password.end()) { out.Error(400,"That can't have a line break or tab in it."); return; }
    Busy busy(ConsoleIfKnown(address));
    FtpClient ftp(address,o.ftpPort,user.c_str(),password.c_str(),8);
    web::Json j;
    j.Open('{').Set("address",text);
    if(!ftp.Greets()) {
        j.Set("ftp",false).Close('}');
        out.body=j.Text();
        return;
    }
    j.Set("ftp",true);
    if(!ftp.Open()) {
        j.Set("login",false).Set("error",std::string(ftp.Error())).Close('}');
        out.body=j.Text();
        return;
    }
    j.Set("login",true).Set("aurora",ftp.Exists(o.aurora.c_str())).Set("gameFolder",ftp.Exists(o.xboxFolder.c_str()));
    LaunchIni ini;
    ReadLaunchIni(ftp,ini,false);
    j.Set("launchIni",ini.path).Set("default",ini.defaultPath);
    const std::vector<std::pair<int,std::string> >& plugins=ini.plugins;
    std::vector<unsigned char> data;
    j.Key("plugins").Open('[');
    std::vector<std::string> addresses=PcAddresses();
    for(size_t i=0;i<plugins.size();++i) {
        std::string path=FtpPath(plugins[i].second), name=path.substr(path.rfind('/')+1);
        const char* kind=PluginKind(name);
        j.Open('{').Set("slot",plugins[i].first).Set("path",plugins[i].second).Set("name",name).Set("kind",kind);
        bool found=!path.empty() && ftp.Retrieve(path.c_str(),data,16<<20);
        if(!found && !ftp.Missing()) ftp.Close();
        j.Set("found",found);
        if(found) j.Set("size",(long long)data.size()).Set("sha256",web::Sha256Hex(std::string(data.begin(),data.end())));
        // NetDisc360.xex's own drive: the server it connects to, found where
        // the plugin looks (plugin/main.cpp, PluginServer).
        if(found && !strcmp(kind,"tiles")) {
            std::string folder=path.substr(0,path.rfind('/')+1), from, server=PluginServerOnConsole(ftp,folder,from);
            j.Set("txt",!from.empty()).Set("server",server).Set("serverFrom",from)
             .Set("here",!server.empty() && std::find(addresses.begin(),addresses.end(),server)!=addresses.end());
        }
        // NetISO.xex.txt: the address of the server NetISO connects to.
        if(found && !strcmp(kind,"netiso")) {
            std::vector<unsigned char> txt;
            bool has=ftp.Retrieve((path+".txt").c_str(),txt,4096);
            if(!has && !ftp.Missing()) ftp.Close();
            std::string server=Trim(std::string(txt.begin(),txt.end()));
            j.Set("txt",has).Set("server",server)
             .Set("here",has && std::find(addresses.begin(),addresses.end(),server)!=addresses.end());
        }
        j.Close('}');
    }
    j.Close(']');
    Console* known=ConsoleIfKnown(address);
    j.Set("connected",known && known->seenAt>=0).Close('}');
    ftp.Close();
    out.body=j.Text();
}

// POST /api/consoles/art {address, repair, folders}: the console's tiles with
// no art, and with repair, renames their folders (or those in folders) so
// Aurora fetches it again.
void ConsoleArt(const web::Value& body, web::Reply& out) {
    Options o=Current();
    std::string text;
    DWORD address;
    if(!Text(body,"address",text) || (address=inet_addr(text.c_str()))==INADDR_NONE || !address) { out.Error(400,"Send the console's IPv4 address, like 192.168.1.30."); return; }
    bool repair=false;
    Bool(body,"repair",repair);
    std::set<std::string> only;
    if(const web::Value* list=body.Get("folders"))
        for(size_t i=0;i<list->items.size();++i) if(list->items[i].type==web::Value::String) only.insert(list->items[i].text);
    Console* console=ConsoleIfKnown(address);
    if(!console) { out.Error(404,"No console with that address has used this server."); return; }
    if(PlaysNow(*console)) { out.Error(409,"It's playing a game. Aurora's FTP server is back once it's in Aurora again."); return; }
    Busy busy(console);
    FtpClient ftp(address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),15);
    if(!ftp.Open()) { out.Error(502,std::string("Aurora doesn't answer: ")+ftp.Error()); return; }
    std::vector<FtpEntry> folders;
    std::vector<ArtlessTile> tiles;
    std::string error;
    if(!FindArtless(ftp,o,folders,tiles,error)) { ftp.Close(); out.Error(502,error); return; }
    std::vector<std::pair<std::string,std::string> > renamed;
    if(repair) RepairArt(*console,ftp,o,folders,tiles,only,renamed,error);
    else XboxLog(*console,"%s in Aurora without art.",Count(int(tiles.size()),"tile","tiles").c_str());
    ftp.Close();
    web::Json j;
    j.Open('{').Set("address",text).Key("tiles").Open('[');
    for(size_t i=0;i<tiles.size();++i) j.Open('{').Set("folder",tiles[i].folder).Set("title",tiles[i].title).Close('}');
    j.Close(']').Key("renamed").Open('[');
    for(size_t i=0;i<renamed.size();++i) j.Open('{').Set("from",renamed[i].first).Set("to",renamed[i].second).Close('}');
    j.Close(']');
    if(!error.empty()) j.Set("error",error);
    j.Close('}');
    out.body=j.Text();
}

// How fast the network between this server and a console is: a test file
// written to the console over Aurora's FTP (the direction games stream in)
// and read back, then deleted. It measures the cable, the switch and the
// console's network port or USB adapter together, so a slow link shows up
// without starting a game. Not while it plays, or while its game folders are
// being written.
const int SpeedTestBytes = 32<<20;
void SpeedTest(const web::Value& body, web::Reply& out) {
    Options o=Current();
    std::string text;
    DWORD address;
    if(!Text(body,"address",text) || (address=inet_addr(text.c_str()))==INADDR_NONE || !address) { out.Error(400,"Send the console's IPv4 address, like 192.168.1.30."); return; }
    Console* console=ConsoleIfKnown(address);
    if(!console) { out.Error(404,"No console with that address has used this server."); return; }
    if(PlaysNow(*console)) { out.Error(409,"It's playing a game. Test once it's back in Aurora."); return; }
    std::unique_lock<std::mutex> busy(SyncLock,std::try_to_lock);
    if(!busy.owns_lock()) { out.Error(409,"A console's game folders are being written right now; try again in a moment."); return; }
    FtpClient ftp(address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),30);
    if(!ftp.Open()) { out.Error(502,std::string("Aurora doesn't answer: ")+ftp.Error()); return; }
    std::string folder=o.xboxFolder+"/"+HelperFolder;
    ftp.MakeFolder(o.xboxFolder.c_str()); ftp.MakeFolder(folder.c_str());
    std::string path=folder+"/speed-test.tmp";
    std::vector<unsigned char> data(SpeedTestBytes);
    for(size_t i=0;i<data.size();++i) data[i]=(unsigned char)(i*2654435761u>>24);  // Not all zeros, should anything compress.
    double started=Now();
    bool sent=ftp.Store(path.c_str(),data.data(),data.size());
    double up=Now()-started;
    std::vector<unsigned char> back;
    started=Now();
    bool got=sent && ftp.Retrieve(path.c_str(),back,data.size()+1) && back.size()==data.size();
    double down=Now()-started;
    std::string error=sent && got ? std::string() : ftp.Error();
    ftp.Delete(path.c_str());
    ftp.Close();
    if(!sent || !got) { out.Error(502,"The test file couldn't be "+std::string(sent ? "read back" : "written")+": "+error); return; }
    double toXbox=data.size()/std::max(up,0.001), fromXbox=data.size()/std::max(down,0.001);
    XboxLog(*console,"speed test: %.1f MB/s to the Xbox, %.1f MB/s back (%d MB each way over Aurora's FTP).",toXbox/1e6,fromXbox/1e6,SpeedTestBytes>>20);
    web::Json j;
    j.Open('{').Set("address",text).Set("toXbox",toXbox).Set("fromXbox",fromXbox).Set("bytes",SpeedTestBytes).Close('}');
    out.body=j.Text();
}

// For Prometheus, Grafana or Home Assistant: /metrics (see web.cpp, which
// asks for the dashboard's password, if it has one, as HTTP Basic auth or
// the dashboard's login).
std::string Label(const std::string& text) {
    std::string out;
    for(size_t i=0;i<text.size();++i) { if(text[i]=='\\' || text[i]=='"') out+='\\'; if(text[i]!='\n') out+=text[i]; }
    return out;
}
void Metrics(web::Reply& out) {
    std::string m;
    char line[256];
    auto add=[&](const char* name, const char* help, const char* type) {
        _snprintf_s(line,sizeof(line),_TRUNCATE,"# HELP %s %s\n# TYPE %s %s\n",name,help,name,type); m+=line;
    };
    add("netdisc_info","NetDisc360's version.","gauge");
    m+="netdisc_info{version=\""+Label(NETDISC_VERSION)+"\"} 1\n";
    add("netdisc_uptime_seconds","Seconds since the server started.","gauge");
    _snprintf_s(line,sizeof(line),_TRUNCATE,"netdisc_uptime_seconds %.0f\n",Now()-StartedAt); m+=line;
    size_t games;
    { std::lock_guard<std::mutex> hold(GamesListLock); games=GamesList.size(); }
    add("netdisc_games","Games served.","gauge");
    _snprintf_s(line,sizeof(line),_TRUNCATE,"netdisc_games %u\n",unsigned(games)); m+=line;
    add("netdisc_game_starts_total","Games started (mounted by a console) since the server started.","counter");
    _snprintf_s(line,sizeof(line),_TRUNCATE,"netdisc_game_starts_total %lld\n",GameStarts.load()); m+=line;
    std::vector<Console*> consoles=AllConsoles();
    std::string up,connections,sent,rate,playing,ping;
    {
        std::lock_guard<std::mutex> hold(ConsoleLock);
        long long last=RateSecond;
        for(size_t i=0;i<consoles.size();++i) {
            const Console& c=*consoles[i];
            std::string label=std::string("{console=\"")+c.text+"\"} ";
            up+="netdisc_console_up"+label+(c.answers && !c.offline ? "1\n" : "0\n");
            _snprintf_s(line,sizeof(line),_TRUNCATE,"%d\n",c.connections.load()); connections+="netdisc_console_connections"+label+line;
            _snprintf_s(line,sizeof(line),_TRUNCATE,"%llu\n",(unsigned long long)c.sent.load()); sent+="netdisc_console_sent_bytes_total"+label+line;
            _snprintf_s(line,sizeof(line),_TRUNCATE,"%.0f\n",c.RateAt(last)); rate+="netdisc_console_rate_bytes_per_second"+label+line;
            playing+="netdisc_console_playing"+label+(c.connections>0 && !c.mounted.empty() && !c.inAurora ? "1\n" : "0\n");
            if(c.answers && !c.offline) { _snprintf_s(line,sizeof(line),_TRUNCATE,"%d\n",c.pingMs.load()); ping+="netdisc_console_ping_ms"+label+line; }
        }
    }
    add("netdisc_console_up","Whether the console answers pings.","gauge"); m+=up;
    add("netdisc_console_connections","The console's NetISO connections.","gauge"); m+=connections;
    add("netdisc_console_sent_bytes_total","Game data sent to the console since the server started.","counter"); m+=sent;
    add("netdisc_console_rate_bytes_per_second","Game data sent to the console in the last second.","gauge"); m+=rate;
    add("netdisc_console_playing","Whether the console is playing a game from the server.","gauge"); m+=playing;
    add("netdisc_console_ping_ms","The console's latest ping time.","gauge"); m+=ping;
    out.type="text/plain; version=0.0.4; charset=utf-8";
    out.body=m;
}

// A whole number from low to high; false if it isn't there, and error if it isn't one.
bool Whole(const web::Value& v, const char* key, int low, int high, int& out, std::string& error) {
    const web::Value* f=v.Get(key);
    if(!f) return false;
    if(f->type!=web::Value::Number || f->number!=double(int(f->number)) || f->number<low || f->number>high) {
        error=std::string(key)+" is a whole number from "+Number(low)+" to "+Number(high)+".";
        return false;
    }
    out=int(f->number);
    return true;
}

std::string FtpFolder(std::string path) {
    while(path.size()>1 && path[path.size()-1]=='/') path.erase(path.size()-1);
    return path;
}

void UpdateConsoles(const std::vector<DWORD>& xboxes) {
    for(size_t i=0;i<xboxes.size();++i) ConsoleAt(xboxes[i]);
    std::lock_guard<std::mutex> hold(ConsoleLock);
    for(size_t i=0;i<Consoles.size();++i) Consoles[i]->push=FromXbox(Consoles[i]->address);
}

void StartWeb(const Options& o, bool announce);

bool ChangeSettings(const web::Value& v, std::string& error) {
    if(v.type!=web::Value::Object) { error="Send the settings as a JSON object."; return false; }
    Options was=Current(), next=was;
    std::vector<std::string> changed;
    const web::Value* list;
    if((list=v.Get("folders"))) {
        if(list->type!=web::Value::Array) { error="folders is a list."; return false; }
        next.folders.clear();
        for(size_t i=0;i<list->items.size();++i) {
            if(list->items[i].type!=web::Value::String) { error="folders is a list of paths."; return false; }
            std::string folder=CleanFolder(config::FromUtf8(list->items[i].text));
            if(folder.empty()) continue;
            bool listed=false;
            for(size_t k=0;k<next.folders.size() && !listed;++k) listed=SameFolder(next.folders[k],folder);
            if(listed) continue;
            if(!config::IsFolder(folder)) { error=list->items[i].text+" isn't a folder this computer can open."; return false; }
            next.folders.push_back(folder);
        }
    }
    std::string text;
    if(Text(v,"xboxes",text)) {
        std::vector<DWORD> xboxes; std::string bad;
        if(!ReadAddresses(text,xboxes,bad)) { error="\""+bad+"\" isn't an IPv4 address like 192.168.1.20."; return false; }
        next.xboxes=xboxes;
    }
    // Each goes into an FTP command (or Aurora's web login), which a line break would end early.
    const char* ftpFields[]={"ftpUser","ftpPassword","xboxFolder","aurora","auroraWebUser","auroraWebPassword"};
    for(int i=0;i<6;++i)
        if(Text(v,ftpFields[i],text) && std::find_if(text.begin(),text.end(),[](char c) { return (unsigned char)c<' '; })!=text.end()) {
            error="That can't have a line break or tab in it."; return false;
        }
    if(Text(v,"ftpUser",text)) {
        if(text.empty()) { error="Aurora's FTP user name can't be empty."; return false; }
        next.ftpUser=text;
    }
    Text(v,"ftpPassword",next.ftpPassword);
    if(Text(v,"xboxFolder",text)) {
        if(text.empty() || text[0]!='/') { error="The game folder on the Xbox is an FTP path like /Hdd1/NetISO."; return false; }
        next.xboxFolder=FtpFolder(text);
    }
    if(Text(v,"aurora",text)) {
        if(text.empty() || text[0]!='/') { error="Aurora's folder is an FTP path like /Hdd1/Apps/Aurora."; return false; }
        next.aurora=FtpFolder(text);
    }
    if(Bool(v,"push",next.push) && next.push && !CanPush) { error="This build has no launcher built in, so it can't add games to the Xbox."; return false; }
    Bool(v,"category",next.filter);
    Bool(v,"scan",next.scan);
    Bool(v,"setupDone",next.setupDone);
    Bool(v,"wizardDone",next.wizardDone);
    Whole(v,"backupEvery",0,365,next.backupEvery,error);
    Whole(v,"backupKeep",1,100,next.backupKeep,error);
    Whole(v,"auroraWebPort",1,65535,next.auroraWebPort,error);
    if(!error.empty()) return false;
    Bool(v,"skipInstalled",next.skipInstalled);
    if(Text(v,"installedFolders",text)) {
        std::vector<std::string> folders=InstalledFolders(text);
        std::string list;
        for(size_t i=0;i<folders.size();++i) list+=(i ? ";" : "")+folders[i];
        if(!text.empty() && folders.empty()) { error="The folders with installed games are FTP paths like /Hdd1/Games, with semicolons between."; return false; }
        next.installedFolders=list;
    }
    Bool(v,"auroraScripts",next.auroraScripts);
    Bool(v,"restartAurora",next.restartAurora);
    if(Text(v,"auroraWebUser",text)) {
        if(text.empty()) { error="Aurora's web server user name can't be empty."; return false; }
        next.auroraWebUser=text;
    }
    Text(v,"auroraWebPassword",next.auroraWebPassword);
    Bool(v,"followAddress",next.followAddress);
    Bool(v,"keepAwake",next.keepAwake);
    Bool(v,"showName",next.showName);
    Bool(v,"tidyAurora",next.tidyAurora);
    if(Text(v,"pluginSet",text)) {
        std::vector<std::string> set=PluginSetNames(text);
        if(set.size()>5) { error="DashLaunch loads 5 plugins at most."; return false; }
        std::string list;
        for(size_t i=0;i<set.size();++i) list+=(i ? ", " : "")+set[i];
        next.pluginSet=list;
    }
    if(Text(v,"pluginKit",text)) {
        if(!text.empty() && !config::IsFolder(config::FromUtf8(text))) { error="That folder with the plugin files isn't there: "+text; return false; }
        next.pluginKit=text;
    }
    if(!Whole(v,"autoOffMinutes",0,1440,next.autoOffMinutes,error) && !error.empty()) return false;
    Bool(v,"updateCheck",next.updateCheck);
    if(Bool(v,"updateAuto",next.updateAuto) && next.updateAuto && strcmp(UpdateHow(),"app")) {
        error=InDocker ? "The Docker image is updated by pulling the new one, not by itself." : "Only NetDisc360.exe in the notification area installs updates by itself.";
        return false;
    }
    Bool(v,"updatePrerelease",next.updatePrerelease);
    if(Bool(v,"webLan",next.webLan) && next.webLan && !was.webLan && web::PasswordHash().empty()) {
        error="Set a password for the dashboard first, so only you can change it from another device.";
        return false;
    }
#ifdef _WIN32
    bool boot;
    if(InTray && Bool(v,"startWithWindows",boot) && boot!=StartsWithWindows()) {
        SetStartWithWindows(boot);
        changed.push_back(boot ? "starts with Windows" : "doesn't start with Windows");
    }
#endif
    bool folders=next.folders.size()!=was.folders.size();
    for(size_t i=0;i<next.folders.size() && !folders;++i) folders=next.folders[i]!=was.folders[i];
    if(folders) { FoldersChangedAt=(long long)(Now()*1000); ScanSoon=true; }
    if(folders) changed.push_back("game folders: "+(next.folders.empty() ? std::string("none") : Count(int(next.folders.size()),"folder","folders")));
    if(next.xboxes!=was.xboxes) changed.push_back("consoles to update: "+(next.xboxes.empty() ? std::string("every console that connects") : AddressList(next.xboxes)));
    bool login=next.ftpUser!=was.ftpUser || next.ftpPassword!=was.ftpPassword;
    if(login) changed.push_back("Aurora's FTP login");
    if(next.xboxFolder!=was.xboxFolder) changed.push_back("game folder on the Xbox: "+next.xboxFolder);
    if(next.aurora!=was.aurora) changed.push_back("Aurora's folder: "+next.aurora);
    if(next.push!=was.push) changed.push_back(next.push ? "adding games to the consoles on" : "adding games to the consoles off");
    if(next.filter!=was.filter) changed.push_back(next.filter ? "Xbox 360 category on" : "Xbox 360 category off");
    if(next.scan!=was.scan) changed.push_back(next.scan ? "Aurora scan on" : "Aurora scan off");
    if(next.webLan!=was.webLan) changed.push_back(next.webLan ? "dashboard open to other devices" : "dashboard on this computer only");
    if(next.backupEvery!=was.backupEvery)
        changed.push_back(next.backupEvery ? "settings backed up every "+Count(next.backupEvery,"day","days") : "settings backed up only by hand");
    if(next.backupKeep!=was.backupKeep) changed.push_back("keeping the newest "+Count(next.backupKeep,"automatic backup","automatic backups"));
    if(next.updateCheck!=was.updateCheck) changed.push_back(next.updateCheck ? "looking for updates" : "not looking for updates");
    if(next.updateAuto!=was.updateAuto) changed.push_back(next.updateAuto ? "updates install by themselves" : "updates install when you say");
    if(next.updatePrerelease!=was.updatePrerelease) changed.push_back(next.updatePrerelease ? "test versions count as updates" : "only full releases count as updates");
    if(next.skipInstalled!=was.skipInstalled) changed.push_back(next.skipInstalled ? "games installed on a console get no tile" : "installed games get a tile too");
    if(next.installedFolders!=was.installedFolders) changed.push_back("installed games looked for in "+(next.installedFolders.empty() ? std::string("Games on Demand only") : next.installedFolders));
    if(next.auroraScripts!=was.auroraScripts) changed.push_back(next.auroraScripts ? "Aurora scripts on" : "Aurora scripts off");
    if(next.restartAurora!=was.restartAurora) changed.push_back(next.restartAurora ? "Aurora restarted when the PC games appear or go" : "Aurora never restarted");
    if(next.auroraWebUser!=was.auroraWebUser || next.auroraWebPassword!=was.auroraWebPassword || next.auroraWebPort!=was.auroraWebPort) changed.push_back("Aurora's web server login");
    if(next.followAddress!=was.followAddress) changed.push_back(next.followAddress ? "NetISO follows this PC's address" : "NetISO.xex.txt left alone");
    if(next.keepAwake!=was.keepAwake) changed.push_back(next.keepAwake ? "PC kept awake while a console plays" : "PC may sleep while a console plays");
    if(next.pluginSet!=was.pluginSet) changed.push_back("plugin set "+next.pluginSet);
    if(next.pluginKit!=was.pluginKit) changed.push_back(next.pluginKit.empty() ? std::string("no folder with plugin files") : "plugin files from "+next.pluginKit);
    if(next.tidyAurora!=was.tidyAurora) changed.push_back(next.tidyAurora ? "Aurora's list tidied as it starts" : "Aurora's list not tidied as it starts");
    if(next.showName!=was.showName) changed.push_back(next.showName ? "each console's name shown on it when Aurora starts" : "consoles' names not shown on them");
    if(next.autoOffMinutes!=was.autoOffMinutes)
        changed.push_back(next.autoOffMinutes ? "consoles turned off "+Count(next.autoOffMinutes,"minute","minutes")+" after their last game" : "consoles not turned off after their last game");
    {
        std::lock_guard<std::mutex> hold(SettingsLock);
        Settings.folders=next.folders; Settings.xboxes=next.xboxes; Settings.ftpUser=next.ftpUser; Settings.ftpPassword=next.ftpPassword;
        Settings.xboxFolder=next.xboxFolder; Settings.aurora=next.aurora; Settings.push=next.push; Settings.filter=next.filter;
        Settings.scan=next.scan; Settings.setupDone=next.setupDone; Settings.wizardDone=next.wizardDone; Settings.webLan=next.webLan;
        Settings.backupEvery=next.backupEvery; Settings.backupKeep=next.backupKeep;
        Settings.updateCheck=next.updateCheck; Settings.updateAuto=next.updateAuto; Settings.updatePrerelease=next.updatePrerelease;
        Settings.skipInstalled=next.skipInstalled; Settings.installedFolders=next.installedFolders; Settings.auroraScripts=next.auroraScripts;
        Settings.restartAurora=next.restartAurora; Settings.auroraWebUser=next.auroraWebUser; Settings.auroraWebPassword=next.auroraWebPassword;
        Settings.auroraWebPort=next.auroraWebPort; Settings.followAddress=next.followAddress; Settings.keepAwake=next.keepAwake;
        Settings.autoOffMinutes=next.autoOffMinutes; Settings.showName=next.showName; Settings.tidyAurora=next.tidyAurora;
        Settings.pluginSet=next.pluginSet; Settings.pluginKit=next.pluginKit;
    }
    // Pre-releases in or out changes which release is the newest.
    if(next.updatePrerelease!=was.updatePrerelease || (next.updateCheck && !was.updateCheck)) CheckSoon=true;
    if(next.xboxes!=was.xboxes) UpdateConsoles(next.xboxes);
    // Anything the push depends on writes every console's folders again (only what differs).
    if(next.xboxes!=was.xboxes || login || next.xboxFolder!=was.xboxFolder || next.aurora!=was.aurora ||
       (next.push && !was.push) || (next.filter && !was.filter) || next.skipInstalled!=was.skipInstalled ||
       next.installedFolders!=was.installedFolders || (next.auroraScripts && !was.auroraScripts) || next.tidyAurora!=was.tidyAurora) ++PushGeneration;
    if(next.webLan!=was.webLan) StartWeb(Current(),true);
    if(!changed.empty()) {
        std::string what;
        for(size_t i=0;i<changed.size();++i) what+=(i ? "; " : "")+changed[i];
        Log("Settings changed on the dashboard: %s.",what.c_str());
        Event(EventInfo,"Settings changed: %s",what.c_str());
    }
    Save();
    return true;
}

// The copy of a game to play, chosen on the dashboard: served from the next
// look at the folders (within a second), and kept in the settings file.
bool ChooseCopy(const web::Value& v, std::string& error) {
    std::string key,path;
    if(!Text(v,"key",key) || !Text(v,"path",path)) { error="Send the game's key and the path of the copy to play."; return false; }
    path=config::FromUtf8(path);
    std::string name,kind;
    {
        std::lock_guard<std::mutex> hold(DuplicatesLock);
        Conflict* c=NULL;
        for(size_t i=0;i<Conflicts.size() && !c;++i) if(Conflicts[i].key==key) c=&Conflicts[i];
        if(!c) { error="That game isn't in your folders more than once any more."; return false; }
        const IsoFile* copy=NULL;
        for(size_t i=0;i<c->copies.size() && !copy;++i) if(c->copies[i].path==path) copy=&c->copies[i];
        if(!copy) { error="That copy isn't in your folders any more."; return false; }
        Choice choice;
        choice.path=path;
        for(size_t i=0;i<c->copies.size();++i) choice.copies.insert(CopyId(c->copies[i].path));
        Choices[key]=choice;
        name=c->name; kind=copy->kind==DiscIso ? "ISO" : copy->kind==DiscGod ? "Games on Demand" : "extracted";
        // Shown as chosen right away; the served copy changes at the next look at the folders, now.
        c->chosen=true; c->open=false;
        ScanSoon=true;
        std::lock_guard<std::mutex> games(GamesListLock);
        ++GamesVersion;
    }
    Log("%s: the copy at %s is played from now on (chosen on the dashboard).",name.c_str(),path.c_str());
    Event(EventInfo,"%s: playing the %s copy from now on",name.c_str(),kind.c_str());
    Save();
    return true;
}

// Backups of the settings ---------------------------------------------------------

std::mutex BackupLock;

// Backups next to the settings file; "" without one.
std::string BackupFolder(const Options& o) { return o.configPath.empty() ? std::string() : config::FolderOf(o.configPath)+"Backups"; }

// A backup of the settings file and the disc map as they are now.
bool MakeBackup(const char* kind, std::string& name, std::string& error, const std::string& spare = std::string()) {
    Options o=Current();
    if(o.configPath.empty()) { error="The settings aren't kept in a file (start NetDisc360 with --config), so there's nothing to back up."; return false; }
    Save();
    std::map<std::string,std::string> files;
    std::string text;
    if(!config::ReadFile(o.configPath,text,1<<20)) { error="Can't read "+o.configPath+"."; return false; }
    files["netdisc.ini"]=text;
    if(config::ReadFile(DiscMapPath(o.configPath),text,1<<20)) files["discs.ini"]=text;
    std::lock_guard<std::mutex> hold(BackupLock);
    if(!backup::Make(BackupFolder(o),kind,files,name,error)) {
        Log("Couldn't back up the settings: %s",config::FromUtf8(error).c_str());
        return false;
    }
    Log("Settings backed up (%s): %s",kind,config::FromUtf8(name).c_str());
    int removed=backup::Prune(BackupFolder(o),o.backupKeep,spare);
    if(removed) Log("Deleted the %s oldest automatic backup%s, keeping the newest %d.",Number(removed).c_str(),removed==1 ? "" : "s",o.backupKeep);
    return true;
}

void Backups(web::Reply& out) {
    Options o=Current();
    std::string folder=BackupFolder(o);
    std::vector<backup::Info> list;
    if(!folder.empty()) { std::lock_guard<std::mutex> hold(BackupLock); list=backup::List(folder); }
    web::Json j;
    j.Open('{').Set("available",!folder.empty()).Set("folder",folder).Set("platform",backup::Platform()).Key("backups").Open('[');
    for(size_t i=0;i<list.size();++i) {
        const backup::Info& b=list[i];
        j.Open('{').Set("name",b.name).Set("kind",b.kind).Set("version",b.version).Set("platform",b.platform);
        j.Set("time",b.time).Set("size",b.size).Close('}');
    }
    j.Close(']').Close('}');
    out.body=j.Text();
}

std::string BackupPath(const std::string& name) {
#ifdef _WIN32
    return BackupFolder(Current())+"\\"+name;
#else
    return BackupFolder(Current())+"/"+name;
#endif
}

// A backup in the folder, by its file name, as its bytes.
bool ReadBackup(const std::string& name, std::string& data, std::string& error) {
    std::string folder=BackupFolder(Current());
    if(folder.empty() || !backup::Named(name)) { error="There's no backup by that name."; return false; }
    std::lock_guard<std::mutex> hold(BackupLock);
    if(!config::ReadFile(BackupPath(name),data,8<<20)) { error="That backup isn't there any more."; return false; }
    return true;
}

// The settings from the file again, after a restore: the command line's
// options under what the file says, as at a start (the ports stay until the
// next start).
void Reload(std::vector<std::string>& notes) {
    Options was=Current(), o=CommandLine;
    o.configPath=was.configPath;
    LoadConfig(o);
    if(!CanPush) o.push=false;
    if(o.webLan && web::PasswordHash().empty()) {
        o.webLan=false;
        notes.push_back("The backup lets other devices open the dashboard but has no password, so it's on this computer only until you set one.");
    }
    if(o.webPort!=was.webPort) notes.push_back("The backup's dashboard port ("+Number(o.webPort)+") is used from the next start.");
    {
        std::lock_guard<std::mutex> hold(SettingsLock);
        Settings.folders=o.folders; Settings.xboxes=o.xboxes; Settings.ftpUser=o.ftpUser; Settings.ftpPassword=o.ftpPassword;
        Settings.xboxFolder=o.xboxFolder; Settings.aurora=o.aurora; Settings.push=o.push; Settings.filter=o.filter; Settings.scan=o.scan;
        Settings.setupDone=o.setupDone; Settings.wizardDone=o.wizardDone; Settings.toldTray=o.toldTray; Settings.webLan=o.webLan;
        Settings.backupEvery=o.backupEvery; Settings.backupKeep=o.backupKeep;
        Settings.updateCheck=o.updateCheck; Settings.updateAuto=o.updateAuto && !strcmp(UpdateHow(),"app"); Settings.updatePrerelease=o.updatePrerelease;
        Settings.skipInstalled=o.skipInstalled; Settings.installedFolders=o.installedFolders; Settings.auroraScripts=o.auroraScripts;
        Settings.restartAurora=o.restartAurora; Settings.auroraWebUser=o.auroraWebUser; Settings.auroraWebPassword=o.auroraWebPassword;
        Settings.auroraWebPort=o.auroraWebPort; Settings.followAddress=o.followAddress; Settings.keepAwake=o.keepAwake;
        Settings.autoOffMinutes=o.autoOffMinutes; Settings.showName=o.showName; Settings.tidyAurora=o.tidyAurora;
        Settings.pluginSet=o.pluginSet; Settings.pluginKit=o.pluginKit;
    }
    UpdateConsoles(o.xboxes);
    FoldersChangedAt=(long long)(Now()*1000);
    ScanSoon=true;
    ++PushGeneration;
    { std::lock_guard<std::mutex> hold(GamesListLock); ++GamesVersion; }
    if(o.webLan!=was.webLan) StartWeb(Current(),true);
    CheckSoon=true;
    Save();
}

// Puts a backup's settings (data: the backup's bytes) in place of the ones
// now, which are backed up first, and uses them at once.
// from: the backup's name when it is one in the Backups folder.
bool Restore(const std::string& data, std::vector<std::string>& notes, std::string& error, const std::string& from = std::string()) {
    std::map<std::string,std::string> files;
    backup::Info info;
    if(!backup::Open(data,files,info,error)) return false;
    Options now=Current();
    if(now.configPath.empty()) { error="The settings aren't kept in a file (start NetDisc360 with --config), so a backup can't be restored."; return false; }
    std::string undo;
    if(!MakeBackup("restore",undo,error,from)) { error="The settings now couldn't be backed up first, so nothing was restored. "+error; return false; }
    config::File f;
    f.Read(files["netdisc.ini"]);
    // Aurora's FTP password is encrypted for the Windows user that saved it;
    // where it can't be read, the one in use stays.
    std::string password;
    bool here=info.platform.empty() || info.platform==backup::Platform();
    if(f.Has("ftp_password") && (!here || !config::Unprotect(f.Get("ftp_password"),password))) {
        notes.push_back("Aurora's FTP password in the backup can only be read by the Windows user that made it, so the password you use now was kept.");
        f.Set("ftp_password",config::Protect(now.ftpPassword));
    }
    if(f.Has("aurora_web_password") && (!here || !config::Unprotect(f.Get("aurora_web_password"),password)))
        f.Set("aurora_web_password",config::Protect(now.auroraWebPassword));
    f.Remove("updated_from");
    f.Remove("update_to");
    f.Remove("update_backup");
    {   // The update history, and a version skipped since it didn't start here, are this copy's own, not the backup's.
        std::lock_guard<std::mutex> hold(SettingsLock);
        f.Set("update_history",HistoryText(UpdateHistory));
        if(UpdateSkip.empty()) f.Remove("update_skip");
        else f.Set("update_skip",UpdateSkip);
    }
    int missing=0, folders=int(Listed(f,"folders"));
    for(int i=1;i<=folders;++i) if(!config::IsFolder(config::FromUtf8(f.Get(("folder"+Number(i)).c_str())))) ++missing;
    if(missing) notes.push_back(Count(missing,"of the backup's game folders isn't","of the backup's game folders aren't")+" on this computer; change them on the Game folders page.");
    if(PasswordFixed && f.Has("web_password")) notes.push_back("The dashboard's password stays the one set with NETDISC_WEB_PASSWORD.");
    {
        std::lock_guard<std::mutex> hold(SaveLock);
        if(!config::WriteFile(now.configPath,f.Text(),true)) { error="Can't write "+now.configPath+"."; return false; }
    }
    if(files.count("discs.ini")) {
        std::lock_guard<std::mutex> hold(MapLock);
        if(!config::WriteFile(DiscMapPath(now.configPath),files["discs.ini"],false)) notes.push_back("The disc map couldn't be written, so it stays as it was.");
    }
    Reload(notes);
    char made[64]="";
    if(info.time) {
        time_t t=time_t(info.time);
        struct tm local;
#ifdef _WIN32
        bool known=!localtime_s(&local,&t);
#else
        bool known=localtime_r(&t,&local)!=NULL;
#endif
        if(known) strftime(made,sizeof(made),"%Y-%m-%d %H:%M",&local);  // A time out of range would end the program on Windows.
    }
    Log("Settings restored from a backup%s%s%s; the settings before are in %s.",*made ? " made " : "",made,
        info.version.empty() ? "" : (" by NetDisc360 "+info.version).c_str(),config::FromUtf8(undo).c_str());
    for(size_t i=0;i<notes.size();++i) Log("  %s",config::FromUtf8(notes[i]).c_str());
    Event(EventInfo,"Settings restored from a backup%s%s",*made ? " made " : "",made);
    return true;
}

// Scheduled backups: one every backupEvery days, looked for every ten
// minutes from five minutes after the start (so a first start's half-made
// settings aren't the first backup).
void BackupLoop() {
    Sleep(300000);
    for(;;) {
        Options o=Current();
        std::string folder=BackupFolder(o), name, error;
        if(o.backupEvery>0 && !folder.empty()) {
            long long newest=0;
            {
                std::lock_guard<std::mutex> hold(BackupLock);
                std::vector<backup::Info> list=backup::List(folder);
                for(size_t i=0;i<list.size();++i) if(list[i].kind=="scheduled") newest=std::max(newest,list[i].time);
            }
            if((long long)time(NULL)-newest>=o.backupEvery*86400LL-60) MakeBackup("scheduled",name,error);
        }
        Sleep(600000);
    }
}

// Updates, continued ----------------------------------------------------------------

// GitHub's list of releases (or --releases's file, for the tests).
bool FetchReleases(const Options& o, std::string& body, std::string& error) {
    if(o.releasesFile.empty()) return update::Fetch(update::ReleasesUrl(),body,8<<20,NULL,error);
    if(config::ReadFile(o.releasesFile,body,8<<20)) return true;
    error="Can't read "+o.releasesFile+".";
    return false;
}

bool UpdateNewer() {
    std::lock_guard<std::mutex> hold(UpdateLock);
    return Updates.found && update::Compare(Updates.latest.version,NETDISC_VERSION)>0;
}

// Asks GitHub for the newest release; false with why in error.
bool CheckForUpdate(std::string& error) {
    Options o=Current();
    {
        std::lock_guard<std::mutex> hold(UpdateLock);
        if(Updates.checking) { error="It's checking already."; return false; }
        Updates.checking=true;
    }
    std::string body;
    update::Release r;
    std::vector<update::Release> releases;
    bool answered=FetchReleases(o,body,error);
    bool ok=answered && update::List(body,o.updatePrerelease,releases,error);
    if(ok) r=releases[0];
    std::string tell;
    {
        std::lock_guard<std::mutex> hold(UpdateLock);
        Updates.checking=false;
        if(answered) Updates.checkedAt=(long long)time(NULL);
        Updates.error=ok ? std::string() : error;
        Updates.found=ok;
        if(ok) { Updates.latest=r; Updates.releases.swap(releases); }
        if(ok && update::Compare(r.version,NETDISC_VERSION)>0 && Updates.told!=r.version) tell=Updates.told=r.version;
    }
    if(!ok) Log("Update check: %s",error.c_str());
    if(!tell.empty()) {
        Log("NetDisc360 %s is out (this is %s): %s",tell.c_str(),NETDISC_VERSION,r.page.c_str());
        Event(EventInfo,"NetDisc360 %s is out; see what's new under Settings, Updates",tell.c_str());
#ifdef _WIN32
        if(InTray && !o.updateAuto) Notify("NetDisc360 "+tell+" is out","Click here to see what's new and install it.","#settings/updates",false);
#endif
    }
    return ok;
}

void DownloadProgress(long long got, long long total) {
    std::lock_guard<std::mutex> hold(UpdateLock);
    Updates.got=got; Updates.total=total;
}

// Whether a console is connected (playing, or about to).
bool ConsolesBusy() {
    std::vector<Console*> consoles=AllConsoles();
    for(size_t i=0;i<consoles.size();++i) if(consoles[i]->connections>0) return true;
    return false;
}

// Downloads the newest release's NetDisc360.exe, checks it, backs up the
// settings and puts it in place of this exe, which then restarts as it. One
// installing by itself (automatic) leaves it should a console start a game.
// wanted: the version whose changes were shown, if any; should a newer one
// have come out since, nothing is installed, so its changes can be seen first.
void InstallUpdate(bool automatic, std::string wanted) {
    Options o=Current();
    std::string body, exe, error, name;
    update::Release r;
    bool ok=FetchReleases(o,body,error) && update::Newest(body,o.updatePrerelease,r,error);
    if(ok && update::Compare(r.version,NETDISC_VERSION)<=0) { ok=false; error="This is the newest version already."; }
    if(ok && !wanted.empty() && r.version!=wanted) {
        ok=false;
        error="NetDisc360 "+r.version+" came out since you looked. Check now to see its changes, then install it.";
        CheckSoon=true;
    }
    if(ok && r.exeUrl.empty()) { ok=false; error="Release "+r.tag+" has no NetDisc360.exe to install; download it from GitHub."; }
    if(ok && o.releasesFile.empty() && !update::FromRepo(r.exeUrl)) { ok=false; error="Release "+r.tag+"'s NetDisc360.exe isn't on "+NETDISC_REPO+"'s releases, so it wasn't installed."; }
    if(ok) {
        Log("Downloading NetDisc360 %s: %s",r.version.c_str(),r.exeUrl.c_str());
        ok=update::Fetch(r.exeUrl,exe,256<<20,DownloadProgress,error);
    }
    if(ok) {
        std::lock_guard<std::mutex> hold(UpdateLock);
        Updates.stage="check";
    }
    ok=ok && update::CheckExe(exe,r,error);
    // With an update key (version.h), the release's signature has to match too.
    if(ok && update::Signed()) {
        std::string signature;
        if(r.sigUrl.empty()) { ok=false; error="Release "+r.tag+" has no NetDisc360.exe.sig, so it wasn't installed."; }
        else if(o.releasesFile.empty() && !update::FromRepo(r.sigUrl)) { ok=false; error="Release "+r.tag+"'s signature isn't on "+NETDISC_REPO+"'s releases, so nothing was installed."; }
        else ok=update::Fetch(r.sigUrl,signature,4096,NULL,error) && update::CheckSignature(exe,signature,error);
    }
#ifdef _WIN32
    if(ok && automatic && ConsolesBusy()) { ok=false; error="A console started a game; it installs later."; }
#endif
    // The settings are backed up first, to go back to from the Backups page.
    if(ok) {
        std::lock_guard<std::mutex> hold(UpdateLock);
        Updates.stage="backup";
    }
    if(ok && !o.configPath.empty() && !MakeBackup("update",name,error)) {
        ok=false;
        error="The settings couldn't be backed up first, so nothing was installed. "+error;
    }
    if(ok) {
        std::lock_guard<std::mutex> hold(UpdateLock);
        Updates.stage="restart";
    }
#ifdef _WIN32
    if(ok) {
        { std::lock_guard<std::mutex> hold(SettingsLock); UpdatedFrom=NETDISC_VERSION; UpdateTo=r.version; UpdateBackup=name; }
        Save();
        Log("Installing NetDisc360 %s in place of %s; it restarts.",r.version.c_str(),NETDISC_VERSION);
        Event(EventInfo,"Installing NetDisc360 %s; it restarts in a moment",r.version.c_str());
        ok=InstallExe(exe,error);
        if(!ok) { { std::lock_guard<std::mutex> hold(SettingsLock); UpdatedFrom.clear(); UpdateTo.clear(); UpdateBackup.clear(); } Save(); }
    }
#else
    if(ok) { ok=false; error="This build doesn't install updates itself."; }
#endif
    if(ok) return;  // It's restarting.
    Log("The update wasn't installed: %s",error.c_str());
    Event(EventWarning,"The update wasn't installed: %s",error.c_str());
    std::lock_guard<std::mutex> hold(UpdateLock);
    Updates.installing=false;
    Updates.stage.clear();
    Updates.installError=error;
}

bool StartInstall(std::string& error, bool automatic=false, const std::string& wanted=std::string()) {
    if(strcmp(UpdateHow(),"app")) {
        error=InDocker ? "The Docker image is updated by pulling the new one: docker compose pull, then docker compose up -d."
                       : "This copy is updated by hand: download the new one from GitHub.";
        return false;
    }
    std::lock_guard<std::mutex> hold(UpdateLock);
    if(Updates.installing) { error="It's installing already."; return false; }
    Updates.installing=true; Updates.stage="download"; Updates.got=Updates.total=0; Updates.installError.clear();
    std::thread(InstallUpdate,automatic,wanted).detach();
    return true;
}

// Looks for an update a minute after the start, then every twelve hours (an
// hour after a check that failed). One that installs by itself waits until
// no console is playing.
void UpdateLoop() {
    double next=Now()+60;
    for(;;) {
        Sleep(5000);
        Options o=Current();
        if(!ChecksUpdates(o) || (!o.updateCheck && !CheckSoon)) continue;
        if(Now()<next && !CheckSoon) continue;
        CheckSoon=false;
        std::string error;
        next=Now()+(CheckForUpdate(error) ? 12*3600 : 3600);
        if(!o.updateAuto || !UpdateNewer() || strcmp(UpdateHow(),"app")) continue;
        std::string skip;
        { std::lock_guard<std::mutex> hold(SettingsLock); skip=UpdateSkip; }
        { std::lock_guard<std::mutex> hold(UpdateLock); if(!skip.empty() && Updates.latest.version==skip) continue; }
        if(ConsolesBusy()) { next=Now()+1800; continue; }
        Log("Installing the update by itself (Settings, Updates).");
        StartInstall(error,true);
    }
}

void UpdateInfo(web::Reply& out) {
    web::Json j;
    j.Open('{').Set("version",NETDISC_VERSION).Set("commit",NETDISC_COMMIT).Set("built",__DATE__ " " __TIME__);
    UpdateJson(j,Current(),true);
    j.Close('}');
    out.body=j.Text();
}

// Starting a game from the dashboard -------------------------------------------

// The library's game listed as name (without .iso, in UTF-8); false if it isn't there.
bool LibraryGame(const std::string& name, IsoFile& game) {
    std::lock_guard<std::mutex> hold(GamesListLock);
    for(size_t i=0;i<GamesList.size();++i) if(config::ToUtf8(WithoutIso(GamesList[i].name))==name) { game=GamesList[i]; return true; }
    return false;
}

// Starts game on console c through Aurora's web server: from its tile, or
// from its copy installed on the hard drive (an extracted one; a Games on
// Demand copy can't be started this way yet). With preferInstalled the
// installed copy goes first, so the tile's "which copy?" question can't
// hold the start up; one Aurora can't start from here (on a USB drive)
// leaves it to the tile. how says which it was: installed or tile. The
// console leaves what it runs, a game too (the page asks first).
const double StartWaitMost = 20, PowerWaitMost = 5;
bool StartOn(Console& c, const IsoFile& game, bool preferInstalled, std::string& how, std::string& error) {
    Options o=Current();
    // An installed copy's file: default.xex, or an original Xbox game's default.xbe.
    std::string tile, installed, elsewhere, installedExec="default.xex", elsewhereExec="default.xex";
    bool god=false;
    {
        std::lock_guard<std::mutex> hold(ConsoleLock);
        std::map<std::string,std::string>::const_iterator t=c.tiles.find(Lower(game.name));
        if(t!=c.tiles.end()) tile=o.xboxFolder+"/"+t->second;
        for(size_t i=0;i<c.installed.size() && installed.empty() && game.titleId;++i) {
            const InstalledGame& g=c.installed[i];
            if(g.titleId!=game.titleId || g.disc!=game.disc) continue;
            size_t cut=g.path.rfind('/');
            std::string at=g.path.substr(0,cut), file=g.path.substr(cut+1);
            if(g.god) god=true;
            else if(!DevicePath(at).empty()) { installed=at; installedExec=file; }
            else if(elsewhere.empty()) { elsewhere=at; elsewhereExec=file; }
        }
    }
    std::string folder=(preferInstalled || tile.empty()) && !installed.empty() ? installed : !tile.empty() ? tile : elsewhere;
    std::string exec=folder==installed ? installedExec : folder==elsewhere && folder!=tile ? elsewhereExec : "default.xex";
    how=folder.empty() ? std::string() : folder==tile ? "tile" : "installed";
    if(folder.empty()) {
        error=god ? "It's installed on that console as Games on Demand; start it in Aurora."
                  : "That console has no tile for it yet: its game folders are written while it's in Aurora.";
        return false;
    }
    std::string device=DevicePath(folder);
    if(device.empty()) { error="Aurora starts games from the dashboard on the hard drive only, and this one is in "+folder+"."; return false; }
    // Aurora ends as the game starts: not while this server writes to it.
    std::string why;
    if(!SafeSoon(c,StartWaitMost,why)) { error="Not now: "+why+". Try again in a moment."; return false; }
    AuroraWeb aurora(c.address,o.auroraWebPort,o.auroraWebUser,o.auroraWebPassword,8);
    if(!aurora.Launch(device,exec)) {
        error=aurora.Error()+".";
        XboxLog(c,"couldn't start %s from the dashboard: %s",WithoutIso(game.name).c_str(),error.c_str());
        return false;
    }
    XboxLog(c,"started %s from the dashboard (%s).",WithoutIso(game.name).c_str(),folder.c_str());
    XboxEvent(c,EventPlay,WithoutIso(game.name)+" started from the dashboard");
    // Its return to Aurora, to look at it again (see FollowLaunch).
    c.launchLeft=false;
    c.probeAt=(long long)((Now()+FirstProbeSeconds)*1000);
    c.launchedAt=(long long)(Now()*1000);
    return true;
}

// Which copy of game Start on all starts on consoles: "installed" when each
// has it installed (as Aurora starts it from here), "tile" when some have it
// installed and the others not but every one has its tile (the copy on this
// PC for all), "" otherwise (each its own way, as StartOn picks).
std::string SameCopy(const std::vector<Console*>& consoles, const IsoFile& game) {
    int installed=0, tiles=0, known=0;
    std::lock_guard<std::mutex> hold(ConsoleLock);
    for(size_t i=0;i<consoles.size();++i) {
        if(!consoles[i]) continue;
        const Console& c=*consoles[i];
        ++known;
        if(c.tiles.count(Lower(game.name))) ++tiles;
        bool has=false;
        for(size_t k=0;k<c.installed.size() && !has && game.titleId;++k) {
            const InstalledGame& g=c.installed[k];
            has=g.titleId==game.titleId && g.disc==game.disc && !g.god && !DevicePath(g.path.substr(0,g.path.rfind('/'))).empty();
        }
        installed+=has;
    }
    if(!known) return std::string();
    if(installed==known) return "installed";
    if(installed && tiles==known) return "tile";
    return std::string();
}

// POST /api/play {game, console}: starts a game on a console, its installed
// copy if it has one, else its tile.
bool Play(const web::Value& v, std::string& error) {
    std::string name, address, how;
    if(!Text(v,"game",name) || !Text(v,"console",address)) { error="Send the game and the console."; return false; }
    DWORD at=inet_addr(address.c_str());
    Console* c=at==INADDR_NONE || !at ? NULL : ConsoleIfKnown(at);
    if(!c) { error="That console hasn't used NetDisc360 since it started."; return false; }
    IsoFile game;
    if(!LibraryGame(name,game)) { error="That game isn't in your folders any more."; return false; }
    return StartOn(*c,game,true,how,error);
}

// "Check my setup" -------------------------------------------------------------

// Text to share without the addresses and Windows user names in it.
// The console a dashboard request names (by its address), if it has used
// this server; else NULL with why in error.
Console* AskedConsole(const std::string& address, std::string& error) {
    DWORD at=inet_addr(address.c_str());
    Console* c=address.empty() || at==INADDR_NONE ? NULL : ConsoleIfKnown(at);
    if(!c) error="That console hasn't used NetDisc360 since it started.";
    return c;
}

// What the console's Aurora says about it (its web server): temperatures,
// the profiles signed in and what runs. Each as Aurora gives it, or null.
// Asked only while the dashboard shows it, so nothing polls the console.
void ConsoleStatus(const std::string& address, web::Reply& out) {
    std::string error;
    Console* c=AskedConsole(address,error);
    if(!c) { out.Error(400,error); return; }
    Options o=Current();
    AuroraWeb aurora(c->address,o.auroraWebPort,o.auroraWebUser,o.auroraWebPassword,4);
    const char* parts[][3]={ { "temperature", "/temperature", "/getTemperatureInfo" }, { "profiles", "/profile", "/getProfileInfo" },
                             { "title", "/title", "/getGameInfo" }, { "memory", "/memory", "/getMemoryInfo" } };
    web::Json j;
    j.Open('{');
    bool any=false;
    for(size_t i=0;i<sizeof(parts)/sizeof(parts[0]);++i) {
        std::string body;
        web::Value v;
        j.Key(parts[i][0]);
        if(aurora.Get(parts[i][1],parts[i][2],body) && web::Parse(body,v)) { j.Raw(body); any=true; }
        else {
            j.Null();
            if(!any && aurora.Error().find("doesn't answer")!=std::string::npos) break;  // Off, or in a game without it.
        }
    }
    if(!any) { out.Error(400,aurora.Error()+"."); return; }
    j.Close('}');
    out.body=j.Text();
}

// The console's screenshots (Aurora's list, as it gives it), one as an image,
// taking one and deleting one.
void Screenshots(const web::Request& r, const web::Value& body, web::Reply& out) {
    std::string address=r.Param("address"), id=r.Param("id"), error;
    Text(body,"address",address);
    Text(body,"id",id);
    Console* c=AskedConsole(address,error);
    if(!c) { out.Error(400,error); return; }
    Options o=Current();
    AuroraWeb aurora(c->address,o.auroraWebPort,o.auroraWebUser,o.auroraWebPassword,r.path=="/api/console/screenshot" ? 30 : 8);
    bool ok;
    if(r.path=="/api/console/screenshots") {
        web::Value v;
        ok=aurora.Screenshots(out.body) && web::Parse(out.body,v);
        if(ok && v.type!=web::Value::Array) out.body="["+out.body+"]";
    } else if(r.path=="/api/console/screenshot" && r.method=="GET") ok=aurora.Screenshot(id,out.body,out.type);
    else if(r.path=="/api/console/screenshot/take") {
        ok=aurora.TakeScreenshot();
        if(ok) { XboxLog(*c,"took a screenshot (asked on the dashboard)."); out.body="{\"ok\":true}"; }
    } else {
        ok=aurora.DeleteScreenshot(id);
        if(ok) { XboxLog(*c,"deleted the screenshot %s (asked on the dashboard).",id.c_str()); out.body="{\"ok\":true}"; }
    }
    if(!ok) out.Error(400,aurora.Error().empty() ? std::string("Aurora's answer wasn't a list.") : aurora.Error()+".");
}

// Turns a console off or restarts it (its debug monitor), or starts Aurora
// again on it (Aurora's web server), from a game too: action off, restart or
// aurora. A restart is a cold one, as with the power button: DashLaunch loads
// its plugins again.
bool PowerOn(Console& console, const std::string& action, std::string& error) {
    Console* c=&console;
    Options o=Current();
    std::string why;
    if((action=="off" || action=="restart" || action=="aurora") && !SafeSoon(*c,PowerWaitMost,why)) { error="Not now: "+why+". Try again in a moment."; return false; }
    if(action=="off") {
        if(!Xbdm(c->address,o.xbdmPort,"shutdown",error)) { error="It can't be turned off from here: "+error+"."; return false; }
        XboxLog(*c,"turned off (asked on the dashboard).");
        XboxEvent(*c,EventXbox,"Turned off from the dashboard");
        return true;
    }
    if(action=="restart") {
        if(!Xbdm(c->address,o.xbdmPort,"magicboot COLD",error)) { error="It can't be restarted from here: "+error+"."; return false; }
        XboxLog(*c,"restarting (asked on the dashboard).");
        XboxEvent(*c,EventXbox,"Restarted from the dashboard");
        c->launchedAt=0;
        return true;
    }
    if(action!="aurora") { error="Send action off, restart or aurora."; return false; }
    std::string folder=DevicePath(o.aurora);
    if(folder.empty()) { error="Aurora is restarted from the dashboard only when it's on the hard drive, and it's in "+o.aurora+"."; return false; }
    AuroraWeb aurora(c->address,o.auroraWebPort,o.auroraWebUser,o.auroraWebPassword,8);
    std::string running, exec="Aurora.xex";
    // Aurora's own name for its XEX, when it's what runs.
    if(aurora.Title(running)) {
        size_t cut=running.rfind('\\');
        if(cut!=std::string::npos && !_stricmp(running.substr(0,cut).c_str(),folder.c_str())) exec=running.substr(cut+1);
    }
    if(!aurora.Launch(folder,exec)) { error=aurora.Error()+"."; return false; }
    XboxLog(*c,"Aurora started again (asked on the dashboard).");
    XboxEvent(*c,EventXbox,"Aurora restarted from the dashboard");
    c->launchedAt=0;
    c->recheck=true;  // Back in Aurora: what's installed and its title updates are looked at again.
    return true;
}

// "Set up this console" ---------------------------------------------------------

// The plugin set (pluginSet: file names in slot order) rolled out to a console
// from a folder on this computer holding the plugin files (pluginKit), as the
// Consoles page asks: launch.ini's plugin lines become the set, a file the
// console lacks is copied next to its other plugins, and each plugin's own
// settings files (<name>.ini, <name>.xex.txt) end up both at the top of that
// drive and next to the plugin, as the plugins and the launcher read them from
// either; none is overwritten or deleted. launch.ini is backed up first, on the
// console (launch.ini.netdisc-<time>.bak beside it) and here, and a backup can
// be put back. DashLaunch loads plugins as the console starts: it takes a
// restart. NetISO.xex, xbdm.xex and JRPC2.xex can't be shared, so NetDisc360
// has none of its own: they come from the kit folder.
const int PluginSlots = 5;  // DashLaunch reads plugin1 to plugin5.
const size_t PluginMost = 16 << 20, PluginConfigMost = 64 << 10;

std::string FileOf(const std::string& path) { size_t cut=path.find_last_of("/\\"); return cut==std::string::npos ? path : path.substr(cut+1); }
std::string FolderOfFtp(const std::string& path) { size_t cut=path.rfind('/'); return cut==std::string::npos ? std::string() : path.substr(0,cut); }
std::string KitPath(const std::string& folder, const std::string& name) {
    if(folder.empty()) return name;
    char last=folder[folder.size()-1];
    return last=='/' || last=='\\' ? folder+name : InFolder(folder,name);
}

// DashLaunch's spelling of a path Aurora's FTP server has (/Usb0/Plugins/x.xex: Usb:\Plugins\x.xex).
std::string DashPath(const std::string& ftpPath) {
    size_t slash=ftpPath.find('/',1);
    std::string device=ftpPath.substr(1,slash==std::string::npos ? std::string::npos : slash-1);
    std::string rest=slash==std::string::npos ? std::string() : ftpPath.substr(slash), lower=Lower(device);
    if(lower=="usb0") device="Usb"; else if(lower=="hdd1") device="Hdd";
    std::replace(rest.begin(),rest.end(),'/','\\');
    return device+":"+rest;
}

// The plugin set's file names, each once, at most PluginSlots.
std::vector<std::string> PluginSetNames(const std::string& text) {
    std::vector<std::string> out;
    for(size_t at=0;at<=text.size();) {
        size_t end=text.find_first_of(",;",at);
        if(end==std::string::npos) end=text.size();
        std::string name=FileOf(Trim(text.substr(at,end-at)));
        at=end+1;
        bool listed=false;
        for(size_t i=0;i<out.size() && !listed;++i) listed=!_stricmp(out[i].c_str(),name.c_str());
        if(!name.empty() && !listed) out.push_back(name);
    }
    return out;
}

// launch.ini's text with its [Plugins] section's pluginN lines replaced by
// paths, in slot order, where the first of them was; every other line, a
// plugin line commented out too, stays as it was. No [Plugins]: one is added.
std::string WithPlugins(const std::string& text, const std::vector<std::string>& paths) {
    const char* eol=text.find("\r\n")!=std::string::npos || text.find('\n')==std::string::npos ? "\r\n" : "\n";
    std::string lines;
    for(size_t i=0;i<paths.size();++i) lines+="plugin"+Number((long long)i+1)+" = "+paths[i]+eol;
    std::string out;
    bool placed=false, inPlugins=false, sawPlugins=false;
    auto ended=[&out,eol]() { if(!out.empty() && out[out.size()-1]!='\n') out+=eol; };
    for(size_t at=0;at<text.size();) {
        size_t end=text.find('\n',at);
        size_t next=end==std::string::npos ? text.size() : end+1;
        std::string raw=text.substr(at,next-at), line=Trim(raw);
        at=next;
        if(!line.empty() && line[0]=='[') {
            if(inPlugins && !placed) { ended(); out+=lines; placed=true; }
            inPlugins=!Lower(line).compare(0,9,"[plugins]");
            sawPlugins=sawPlugins || inPlugins;
            ended();
            out+=raw;
            continue;
        }
        size_t equals=line.find('=');
        std::string key=equals==std::string::npos ? std::string() : Lower(Trim(line.substr(0,equals)));
        if(inPlugins && key.size()==7 && !key.compare(0,6,"plugin") && key[6]>='1' && key[6]<='9') {
            if(!placed) { ended(); out+=lines; placed=true; }
            continue;
        }
        out+=raw;
    }
    if(inPlugins && !placed) { ended(); out+=lines; placed=true; }
    if(!sawPlugins) { ended(); if(!out.empty()) out+=eol; out+=std::string("[Plugins]")+eol+lines; }
    return out;
}

// A file in the kit folder: its bytes, if it's there.
bool KitFile(const Options& o, const std::string& name, std::string& data, size_t most) {
    if(o.pluginKit.empty()) return false;
    std::vector<config::Entry> files=config::Files(o.pluginKit);
    for(size_t i=0;i<files.size();++i)
        if(!_stricmp(files[i].name.c_str(),name.c_str())) return config::ReadFile(KitPath(o.pluginKit,files[i].name),data,most);
    return false;
}
// A plugin's own settings files' names: <name>.ini and <name>.xex.txt (the
// way NetISO.xex names its own).
std::vector<std::string> PluginConfigs(const std::string& file) {
    std::string base=file.size()>4 && !_stricmp(file.c_str()+file.size()-4,".xex") ? file.substr(0,file.size()-4) : file;
    std::vector<std::string> out;
    out.push_back(base+".ini");
    out.push_back(file+".txt");
    return out;
}

bool FtpHas(FtpClient& ftp, const std::string& folder, const std::string& name, U64* size = NULL) {
    std::vector<FtpEntry> entries;
    if(!ftp.List(folder.c_str(),entries)) return false;
    for(size_t i=0;i<entries.size();++i)
        if(!entries[i].folder && !_stricmp(entries[i].name.c_str(),name.c_str())) { if(size) *size=entries[i].size; return true; }
    return false;
}

// What setting the console up does: launch.ini's new text, the plugins (each
// its slot's path and where it comes from) and the files copied.
struct SetupPlan {
    LaunchIni ini;
    std::string folder;                       // Where new plugins go (FTP path).
    std::vector<std::string> paths, from;     // Per slot: DashLaunch path; "console" or "kit".
    std::vector<std::pair<std::string,std::string> > copies;  // FTP path, its bytes (from the kit, or the other copy, or made).
    std::vector<std::string> copied;          // What each copy is, for the page.
    std::string text;
};

// The plan for plugins (file names, or launch.ini paths to keep, in slot
// order) on the console logged in on ftp. False with why in error.
bool PlanSetup(FtpClient& ftp, const Options& o, const std::vector<std::string>& plugins, SetupPlan& plan, std::string& error) {
    if(!ReadLaunchIni(ftp,plan.ini,true)) { error=std::string("Couldn't read launch.ini: ")+ftp.Error(); return false; }
    if(plan.ini.path.empty()) { error="There's no launch.ini on its USB drive or hard drive: DashLaunch's installer writes one."; return false; }
    if(plugins.size()>size_t(PluginSlots)) { error="DashLaunch loads 5 plugins at most."; return false; }
    // New plugins go where its plugins are (the first one's folder), else in
    // Plugins on launch.ini's drive.
    for(size_t i=0;i<plan.ini.plugins.size() && plan.folder.empty();++i) {
        std::string path=FtpPath(plan.ini.plugins[i].second);
        if(!path.empty()) plan.folder=FolderOfFtp(path);
    }
    std::string drive=plan.ini.path.substr(0,plan.ini.path.find('/',1));
    if(plan.folder.empty()) plan.folder=drive+"/Plugins";
    std::string top=plan.folder.substr(0,plan.folder.find('/',1));
    for(size_t i=0;i<plugins.size();++i) {
        std::string name=FileOf(plugins[i]), path;
        if(name.empty() || name.find_first_of(":*?\"<>|")!=std::string::npos) { error="\""+plugins[i]+"\" isn't a plugin's file name."; return false; }
        for(size_t k=0;k<plan.ini.plugins.size() && path.empty();++k)
            if(!_stricmp(FileOf(plan.ini.plugins[k].second).c_str(),name.c_str())) path=plan.ini.plugins[k].second;
        std::string ftpPath=path.empty() ? plan.folder+"/"+name : FtpPath(path);
        for(size_t k=0;k<plan.paths.size();++k)
            if(!_stricmp(FileOf(plan.paths[k]).c_str(),name.c_str())) { error=name+" is in the list twice."; return false; }
        if(!path.empty() && FtpHas(ftp,FolderOfFtp(ftpPath),name)) { plan.paths.push_back(path); plan.from.push_back("console"); }
        else {
            std::string data;
            if(!KitFile(o,name,data,PluginMost))
                { error=name+" isn't on the console"+(o.pluginKit.empty() ? std::string(", and no folder with plugin files is chosen.") : ", nor in "+o.pluginKit+"."); return false; }
            plan.copies.push_back(std::make_pair(ftpPath,data));
            plan.copied.push_back(DashPath(ftpPath)+", from "+o.pluginKit);
            plan.paths.push_back(DashPath(ftpPath));
            plan.from.push_back("kit");
        }
        // Its settings files at the drive's top and beside it: the one there is
        // copied to where it's missing; with neither, the kit's.
        std::string beside=FolderOfFtp(ftpPath);
        bool one=!_stricmp(beside.c_str(),top.c_str());  // A plugin at the drive's top: one place.
        std::vector<std::string> configs=PluginConfigs(name);
        for(size_t c=0;c<configs.size();++c) {
            std::string places[2]={top+"/"+configs[c],beside+"/"+configs[c]};
            bool has[2];
            has[0]=FtpHas(ftp,top,configs[c]);
            has[1]=one ? has[0] : FtpHas(ftp,beside,configs[c]);
            if(has[0] && has[1]) continue;
            std::vector<unsigned char> bytes;
            std::string data, made;
            if(has[0] || has[1]) {
                const std::string& at=places[has[0] ? 0 : 1];
                if(!ftp.Retrieve(at.c_str(),bytes,PluginConfigMost)) continue;
                data.assign(bytes.begin(),bytes.end());
                made="a copy of "+DashPath(at);
            } else if(KitFile(o,configs[c],data,PluginConfigMost)) made="from "+o.pluginKit;
            else if(Lower(configs[c])=="netiso.xex.txt") {
                // NetISO.xex reads the server's address from it: this computer's.
                DWORD here=ftp.LocalAddress();
                if(!here) continue;
                char text[16]; AddressText(here,text,sizeof(text));
                data=std::string(text)+"\r\n";
                made="naming this computer ("+std::string(text)+")";
            } else continue;
            for(int k=0;k<(one ? 1 : 2);++k) if(!has[k]) {
                plan.copies.push_back(std::make_pair(places[k],data));
                plan.copied.push_back(DashPath(places[k])+", "+made);
            }
        }
    }
    plan.text=WithPlugins(plan.ini.text,plan.paths);
    return true;
}

// The console a setup request names, logged in, when it's in Aurora and
// this server isn't working on it already.
Console* SetupConsole(const std::string& address, std::string& error) {
    Console* c=AskedConsole(address,error);
    if(!c) return NULL;
    if(PlaysNow(*c)) { error="It's playing a game: set it up in Aurora."; return NULL; }
    return c;
}

std::string StampNow() {
    time_t t=time(NULL);
    struct tm local;
    char text[32]="0";
#ifdef _WIN32
    bool known=!localtime_s(&local,&t);
#else
    bool known=localtime_r(&t,&local)!=NULL;
#endif
    if(known) strftime(text,sizeof(text),"%Y%m%d-%H%M%S",&local);
    return text;
}

// launch.ini as it is now, kept on the console beside it and here
// (Backups/Consoles/<address>), before a change. The console's copy's name in name.
bool BackUpLaunchIni(FtpClient& ftp, const Console& c, const Options& o, const LaunchIni& ini, std::string& name, std::string& error) {
    name="launch.ini.netdisc-"+StampNow()+".bak";
    if(!StoreChecked(ftp,FolderOfFtp(ini.path)+"/"+name,ini.text)) { error=std::string("Couldn't back up launch.ini on the console: ")+ftp.Error(); return false; }
    std::string here=BackupFolder(o);
    if(!here.empty()) {
        std::string folder=KitPath(KitPath(here,"Consoles"),c.text);
        config::MakeFolder(here); config::MakeFolder(KitPath(here,"Consoles")); config::MakeFolder(folder);
        if(!config::WriteFile(KitPath(folder,name),ini.text,false)) XboxLog(c,"couldn't keep a copy of its launch.ini in %s.",config::FromUtf8(folder).c_str());
    }
    XboxLog(c,"its launch.ini (%s) backed up as %s.",ini.path.c_str(),name.c_str());
    return true;
}

void SetupJson(web::Json& j, FtpClient& ftp, const Options& o, const SetupPlan* plan, const LaunchIni& ini) {
    j.Set("launchIni",ini.path).Set("launchIniText",ini.text).Set("kit",o.pluginKit).Key("set").Open('[');
    std::vector<std::string> set=PluginSetNames(o.pluginSet);
    for(size_t i=0;i<set.size();++i) j.String(set[i]);
    j.Close(']').Key("plugins").Open('[');
    for(size_t i=0;i<ini.plugins.size();++i) {
        std::string path=FtpPath(ini.plugins[i].second);
        j.Open('{').Set("slot",ini.plugins[i].first).Set("path",ini.plugins[i].second).Set("name",FileOf(ini.plugins[i].second))
         .Set("found",!path.empty() && FtpHas(ftp,FolderOfFtp(path),FileOf(path))).Close('}');
    }
    j.Close(']').Key("kitFiles").Open('[');
    if(!o.pluginKit.empty()) {
        std::vector<config::Entry> files=config::Files(o.pluginKit);
        for(size_t i=0;i<files.size();++i)
            if(files[i].name.size()>4 && !_stricmp(files[i].name.c_str()+files[i].name.size()-4,".xex")) j.String(files[i].name);
    }
    j.Close(']');
    // The console's backups of launch.ini, newest first.
    j.Key("backups").Open('[');
    std::vector<FtpEntry> entries;
    std::vector<std::string> backups;
    if(!ini.path.empty() && ftp.List(FolderOfFtp(ini.path).c_str(),entries))
        for(size_t i=0;i<entries.size();++i)
            if(!entries[i].folder && !Lower(entries[i].name).compare(0,19,"launch.ini.netdisc-")) backups.push_back(entries[i].name);
    std::sort(backups.rbegin(),backups.rend());
    for(size_t i=0;i<backups.size();++i) j.String(backups[i]);
    j.Close(']');
    if(plan) {
        j.Set("text",plan->text).Set("folder",DashPath(plan->folder)).Key("slots").Open('[');
        for(size_t i=0;i<plan->paths.size();++i) j.Open('{').Set("path",plan->paths[i]).Set("from",plan->from[i]).Close('}');
        j.Close(']').Key("copies").Open('[');
        for(size_t i=0;i<plan->copied.size();++i) j.String(plan->copied[i]);
        j.Close(']');
    }
}

// GET /api/console/setup?address=: its launch.ini, plugins and backups, the
// plugin set and the kit folder's plugins.
// POST /api/console/setup {address, plugins: [file names in slot order], apply}:
// what it would do (text, slots, copies), or with apply, does it.
void ConsoleSetup(const std::string& address, const web::Value* v, web::Reply& out) {
    std::string error;
    Console* c=SetupConsole(address,error);
    if(!c) { out.Error(400,error); return; }
    Options o=Current();
    bool apply=false, post=v!=NULL;
    std::vector<std::string> plugins;
    if(post) {
        Bool(*v,"apply",apply);
        const web::Value* list=v->Get("plugins");
        if(!list || list->type!=web::Value::Array) { out.Error(400,"Send the plugins, in slot order."); return; }
        for(size_t i=0;i<list->items.size();++i) if(list->items[i].type==web::Value::String) plugins.push_back(list->items[i].text);
    }
    std::string why;
    if(apply && !SafeSoon(*c,PowerWaitMost,why)) { out.Error(409,"Not now: "+why+". Try again in a moment."); return; }
    Busy busy(c);
    FtpClient ftp(c->address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),15);
    if(!ftp.Open()) { out.Error(502,std::string("Aurora's FTP server doesn't answer (it does while Aurora is on screen): ")+ftp.Error()); return; }
    SetupPlan plan;
    web::Json j;
    j.Open('{');
    if(!post) {
        LaunchIni ini;
        if(!ReadLaunchIni(ftp,ini,true)) { out.Error(502,std::string("Couldn't read launch.ini: ")+ftp.Error()); return; }
        SetupJson(j,ftp,o,NULL,ini);
        ftp.Close();
        out.body=j.Close('}').Text();
        return;
    }
    if(!PlanSetup(ftp,o,plugins,plan,error)) { ftp.Close(); out.Error(400,error); return; }
    std::string base;
    if(apply && Text(*v,"base",base) && base!=web::Sha256Hex(plan.ini.text)) { ftp.Close(); out.Error(409,"launch.ini changed since it was read: look at it again."); return; }
    if(apply) {
        std::string backup;
        if(!BackUpLaunchIni(ftp,*c,o,plan.ini,backup,error)) { ftp.Close(); out.Error(502,error); return; }
        for(size_t i=0;i<plan.copies.size();++i) {
            EnsureFolder(ftp,FolderOfFtp(plan.copies[i].first));
            if(!StoreChecked(ftp,plan.copies[i].first,plan.copies[i].second)) {
                std::string failed=std::string("Couldn't write ")+DashPath(plan.copies[i].first)+": "+ftp.Error()+". launch.ini wasn't changed.";
                ftp.Close(); out.Error(502,failed); return;
            }
        }
        if(!StoreChecked(ftp,plan.ini.path,plan.text)) {
            std::string failed=std::string("Couldn't write launch.ini: ")+ftp.Error()+". Its backup is "+backup+".";
            ftp.Close(); out.Error(502,failed); return;
        }
        std::string list;
        for(size_t i=0;i<plan.paths.size();++i) list+=(i ? ", " : "")+FileOf(plan.paths[i]);
        XboxLog(*c,"set up: its launch.ini loads %s (%s copied). Restart it to load them.",list.empty() ? "no plugins" : list.c_str(),Count(int(plan.copies.size()),"file","files").c_str());
        XboxEvent(*c,EventWarning,"Set up: restart it to load its plugins");
        j.Set("applied",true).Set("backup",backup);
    }
    SetupJson(j,ftp,o,&plan,plan.ini);
    j.Set("base",web::Sha256Hex(plan.ini.text));
    ftp.Close();
    out.body=j.Close('}').Text();
}

// POST /api/console/setup/restore {address, backup}: launch.ini as that
// backup (on the console) has it, after backing up the one there now.
bool RestoreLaunchIni(const web::Value& v, std::string& done, std::string& error) {
    std::string address, name, why;
    Text(v,"address",address);
    Console* c=SetupConsole(address,error);
    if(!c) return false;
    if(!Text(v,"backup",name) || Lower(name).compare(0,19,"launch.ini.netdisc-") || name.find_first_of("/\\")!=std::string::npos) { error="Send one of its launch.ini backups."; return false; }
    if(!SafeSoon(*c,PowerWaitMost,why)) { error="Not now: "+why+". Try again in a moment."; return false; }
    Options o=Current();
    Busy busy(c);
    FtpClient ftp(c->address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),15);
    if(!ftp.Open()) { error=std::string("Aurora's FTP server doesn't answer: ")+ftp.Error(); return false; }
    LaunchIni ini;
    std::vector<unsigned char> data;
    if(!ReadLaunchIni(ftp,ini,true) || ini.path.empty()) { error="Couldn't read its launch.ini."; ftp.Close(); return false; }
    if(!ftp.Retrieve((FolderOfFtp(ini.path)+"/"+name).c_str(),data,256<<10)) { error="That backup isn't there."; ftp.Close(); return false; }
    std::string backup;
    if(!BackUpLaunchIni(ftp,*c,o,ini,backup,error)) { ftp.Close(); return false; }
    if(!StoreChecked(ftp,ini.path,std::string(data.begin(),data.end()))) { error=std::string("Couldn't write launch.ini: ")+ftp.Error(); ftp.Close(); return false; }
    ftp.Close();
    XboxLog(*c,"its launch.ini put back as %s had it (the one before is %s). Restart it to load its plugins.",name.c_str(),backup.c_str());
    XboxEvent(*c,EventWarning,"launch.ini put back: restart it");
    done=backup;
    return true;
}

// POST /api/console/identify {address}: "This is <name> (<address>)" on its screen.
bool Identify(const web::Value& v, std::string& error) {
    std::string address;
    Text(v,"address",address);
    Console* c=AskedConsole(address,error);
    if(!c) return false;
    if(!ShowName(*c,Current(),error)) { error="It can't show a message: "+error+"."; return false; }
    return true;
}

// POST /api/console/power {address, action}.
bool Power(const web::Value& v, std::string& error) {
    std::string address, action;
    Text(v,"address",address);
    Text(v,"action",action);
    Console* c=AskedConsole(address,error);
    return c && PowerOn(*c,action,error);
}

// The LAN party ------------------------------------------------------------------
//
// The dashboard's LAN party page (see party.h): the party itself, its team
// raffle and seats, its theme's pictures, and the commands for all its
// consoles at once (Start on all, Back to Aurora on all, Turn off all).

// Whether address is a console the dashboard knows, written as it writes
// addresses (see CanonicalAddress): one that used this server since it
// started, or one the settings file remembers.
bool KnownAddress(const std::string& address) {
    if(CanonicalAddress(address).empty()) return false;
    if(ConsoleIfKnown(inet_addr(address.c_str()))) return true;
    std::lock_guard<std::mutex> hold(SettingsLock);
    for(size_t i=0;i<Known.size();++i) if(Known[i].address==address) return true;
    return false;
}

// A list of strings from the page; false if it isn't one.
bool Strings(const web::Value& v, std::vector<std::string>& out) {
    out.clear();
    if(v.type!=web::Value::Array) return false;
    for(size_t i=0;i<v.items.size();++i) {
        if(v.items[i].type!=web::Value::String) return false;
        out.push_back(v.items[i].text);
    }
    return true;
}

// Consoles the page names for the party: known ones, each once, as
// addresses; false with why in error otherwise.
bool PartyConsoles(const web::Value& v, const char* what, std::vector<std::string>& out, std::string& error) {
    std::vector<std::string> list;
    if(!Strings(v,list)) { error=std::string(what)+" is a list of console addresses."; return false; }
    out.clear();
    for(size_t i=0;i<list.size();++i) {
        if(!KnownAddress(list[i])) { error=list[i]+" isn't a console NetDisc360 knows."; return false; }
        if(std::find(out.begin(),out.end(),list[i])==out.end()) out.push_back(list[i]);
    }
    return true;
}

// POST /api/party: changes any of active, name, theme {preset, accent,
// accent2, person, age}, gamemaster, consoles, guests [{name, here}], teams,
// together and apart [[name, name]]. Typing the guest list again replaces
// it (it's kept from one party to the next); the Gamemaster's console is
// always one of the party's, its first. A party that ends leaves the next
// one its guests, consoles, theme and rules, but no one here and no
// rounds, teams or seats.
bool ChangeParty(const web::Value& v, std::string& error) {
    if(v.type!=web::Value::Object) { error="Send the party as a JSON object."; return false; }
    std::lock_guard<std::mutex> changing(PartyChangeLock);
    Party was=CurrentParty(), next=was;
    const web::Value* f;
    std::string text;
    if((f=v.Get("active"))) {
        if(f->type!=web::Value::Bool) { error="active is true or false."; return false; }
        next.active=f->boolean;
        // A new party counts its own rounds won.
        if(!was.active && next.active) {
            next.wins.clear(); next.game.clear(); next.started=(long long)time(NULL);
            std::lock_guard<std::mutex> hold(FeedLock);
            Feed.clear();
        }
        if(was.active && !next.active) {
            for(size_t i=0;i<next.guests.size();++i) next.guests[i].here=false;
            next.round=0; next.drawn=false; next.kept=false;
            next.drawnTeams.clear(); next.seats.clear(); next.note.clear(); next.last.clear();
        }
    }
    if((f=v.Get("name"))) {
        if(f->type!=web::Value::String) { error="The party's name is text."; return false; }
        if(!CleanName(f->text,0,PartyNameMost,"The party's name",next.name,error)) return false;
    }
    if((f=v.Get("theme"))) {
        if(f->type!=web::Value::Object) { error="theme is an object."; return false; }
        if(Text(*f,"preset",text)) {
            if(!KnownPreset(text)) { error="The theme is classic, birthday, company or retro."; return false; }
            next.theme.preset=text;
        }
        if((Text(*f,"accent",text) && !CleanColour(text,next.theme.accent)) || (Text(*f,"accent2",text) && !CleanColour(text,next.theme.accent2))) {
            error="The theme's colours are written like #107c10."; return false;
        }
        if(Text(*f,"person",text) && !CleanName(text,0,GuestNameMost,"The birthday person's name",next.theme.person,error)) return false;
        if(!Whole(*f,"age",0,150,next.theme.age,error) && !error.empty()) return false;
    }
    if((f=v.Get("guests"))) {
        if(f->type!=web::Value::Array) { error="guests is a list of {name, here}."; return false; }
        if(f->items.size()>PartyGuestsMost) { error="A party has "+Number(PartyGuestsMost)+" guests at most."; return false; }
        next.guests.clear();
        for(size_t i=0;i<f->items.size();++i) {
            const web::Value& g=f->items[i];
            PartyGuest guest={std::string(),false};
            if(g.type!=web::Value::Object || !Text(g,"name",text)) { error="guests is a list of {name, here}."; return false; }
            if(!CleanName(text,1,GuestNameMost,"A guest's name",guest.name,error)) return false;
            if(g.Get("here") && !Bool(g,"here",guest.here)) { error="here is true or false."; return false; }
            if(!GuestNamed(next,guest.name).empty()) { error=guest.name+" is on the guest list twice."; return false; }
            next.guests.push_back(guest);
        }
    }
    if((f=v.Get("consoles")) && !PartyConsoles(*f,"consoles",next.consoles,error)) return false;
    if((f=v.Get("gamemaster"))) {
        if(f->type!=web::Value::String || (!f->text.empty() && !KnownAddress(f->text))) {
            error=f->type==web::Value::String ? f->text+" isn't a console NetDisc360 knows." : "gamemaster is a console's address.";
            return false;
        }
        next.gamemaster=f->text;
    }
    if(!Whole(v,"teams",TeamsLeast,TeamsMost,next.teams,error) && !error.empty()) return false;
    if(!Whole(v,"idleOffMinutes",0,IdleOffMost,next.idleOffMinutes,error) && !error.empty()) return false;
    if((f=v.Get("phoneRelay"))) {
        if(f->type!=web::Value::String) { error="phoneRelay is the relay's address."; return false; }
        text=f->text;
        while(!text.empty() && (text.back()=='/' || text.back()==' ')) text.pop_back();
        while(!text.empty() && text[0]==' ') text.erase(0,1);
        if(!text.empty() && !relay::GoodUrl(text,error)) return false;
        next.phoneRelay=text;
    }
    // A new code for each party (and on asking), so last time's QR codes don't join this one.
    std::string code;
    if(Text(v,"phoneCode",code) && code!="new") { error="phoneCode is \"new\" (a new party code)."; return false; }
    if(!next.phoneRelay.empty()) {
        if(next.phoneKey.empty()) next.phoneKey=web::NewSecret();
        if(next.phoneCode.empty() || (next.active && !was.active) || code=="new") next.phoneCode=NewPartyCode();
    }
    if(Text(v,"balance",text)) {
        int b=-1;
        for(int k=0;k<BalanceCount;++k) if(text==BalanceNames[k]) b=k;
        if(b<0) { error="balance is random, wins or gamerscore."; return false; }
        next.balance=b;
    }
    if((f=v.Get("teamColours"))) {
        if(f->type!=web::Value::Array || f->items.size()>TeamsMost) { error="teamColours is a list of up to "+Number(TeamsMost)+" colours (0-"+Number(TeamColoursCount-1)+")."; return false; }
        std::vector<int> colours=next.teamColours;
        for(size_t i=0;i<f->items.size();++i) {
            const web::Value& c=f->items[i];
            if(c.type!=web::Value::Number || c.number!=int(c.number) || c.number<0 || c.number>=TeamColoursCount) { error="A team's colour is 0-"+Number(TeamColoursCount-1)+"."; return false; }
            colours[i]=int(c.number);
        }
        for(size_t i=0;i<f->items.size();++i)
            for(size_t k=0;k<colours.size();++k)
                if(k!=i && colours[k]==colours[i]) {
                    // Taken by another team: that one gets this one's old colour (a swap).
                    if(k<f->items.size()) { error="Two teams can't have the same colour."; return false; }
                    colours[k]=next.teamColours[i];
                }
        next.teamColours=colours;
    }
    for(int rule=0;rule<2;++rule) {
        const char* key=rule ? "apart" : "together";
        if(!(f=v.Get(key))) continue;
        std::vector<NamePair>& pairs=rule ? next.apart : next.together;
        if(f->type!=web::Value::Array || f->items.size()>PartyPairsMost) { error=std::string(key)+" is a list of up to "+Number(PartyPairsMost)+" pairs of guests' names."; return false; }
        pairs.clear();
        for(size_t i=0;i<f->items.size();++i) {
            std::vector<std::string> pair;
            if(!Strings(f->items[i],pair) || pair.size()!=2) { error=std::string(key)+" is a list of pairs of guests' names."; return false; }
            std::string a=GuestNamed(next,pair[0]), b=GuestNamed(next,pair[1]);
            if(a.empty() || b.empty()) { error=(a.empty() ? pair[0] : pair[1])+" isn't on the guest list."; return false; }
            if(SameName(a,b)) { error=a+" can't be kept "+(rule ? "apart from" : "together with")+" themselves."; return false; }
            pairs.push_back(std::make_pair(a,b));
        }
    }
    for(size_t i=0;i<next.together.size();++i)
        for(size_t k=0;k<next.apart.size();++k) {
            const NamePair& t=next.together[i], & a=next.apart[k];
            if((SameName(t.first,a.first) && SameName(t.second,a.second)) || (SameName(t.first,a.second) && SameName(t.second,a.first))) {
                error=t.first+" and "+t.second+" can't be kept together and apart at once."; return false;
            }
        }
    TidyParty(next);
    { std::lock_guard<std::mutex> hold(PartyLock); TheParty=next; }
    // The guests' stats: a party that ends counts for those who were here;
    // one that starts (or a guest who comes to it) notes their gamerscore,
    // so what they earn at it shows.
    {
        std::lock_guard<std::mutex> hold(StatsLock);
        long long now=(long long)time(NULL);
        if(was.active && !next.active) {
            for(size_t i=0;i<was.guests.size();++i) if(was.guests[i].here) { GuestStats& g=StatsOf(was.guests[i].name); ++g.parties; g.lastParty=now; }
            for(size_t i=0;i<GuestStatsList.size();++i) GuestStatsList[i].partyStart=-1;
        }
        if(next.active)
            for(size_t i=0;i<next.guests.size();++i)
                if(next.guests[i].here) { GuestStats& g=StatsOf(next.guests[i].name); if(g.partyStart<0) g.partyStart=g.gamerscore; }
    }
    // The games' title updates are compared on the party's consoles while
    // it's on, looked at again as it starts (or gets more consoles), so the
    // warning before Start on all is about them as they are.
    if(next.active!=was.active || (next.active && next.consoles!=was.consoles)) GamesChanged();
    if(next.active && (!was.active || next.consoles!=was.consoles)) {
        std::vector<Console*> consoles;
        for(size_t i=0;i<next.consoles.size();++i) consoles.push_back(ConsoleIfKnown(inet_addr(next.consoles[i].c_str())));
        Options o=Current();
        std::thread([consoles,o]() { LookTitleUpdatesNow(consoles,o,8); }).detach();
    }
    if(next.active && !was.active) {
        Log("LAN party %s started on the dashboard: %s, the Gamemaster console %s.",next.name.empty() ? "" : ("\""+next.name+"\"").c_str(),
            Count(int(next.consoles.size()),"console","consoles").c_str(),next.gamemaster.empty() ? "not chosen yet" : next.gamemaster.c_str());
        Event(EventInfo,"LAN party started%s%s",next.name.empty() ? "" : ": ",next.name.c_str());
    } else if(!next.active && was.active) {
        Log("LAN party ended on the dashboard.");
        Event(EventInfo,"LAN party ended");
    }
    Save();
    return true;
}

// The same team said to win again this soon is a double press.
enum { WinAgainSeconds = 60, PartyTextPointsMost = 16 };
// How strong each of here is for a fair draw (Party::balance): their wins
// this party, or their gamerscore (one not read yet: the others' average).
// Empty: drawn at random.
std::vector<double> Strengths(const Party& p, const std::vector<std::string>& here) {
    std::vector<double> out;
    if(p.balance==BalanceWins) {
        std::vector<std::pair<std::string,int> > points=PartyPoints(p);
        for(size_t i=0;i<here.size();++i) {
            double n=0;
            for(size_t k=0;k<points.size();++k) if(SameName(points[k].first,here[i])) n=points[k].second;
            out.push_back(n);
        }
    } else if(p.balance==BalanceGamerscore) {
        std::vector<GuestStats> stats=GuestStatsNow();
        double sum=0;
        int known=0;
        for(size_t i=0;i<here.size();++i) {
            double score=-1;
            for(size_t k=0;k<stats.size();++k) if(SameName(stats[k].name,here[i])) score=stats[k].gamerscore;
            out.push_back(score);
            if(score>=0) { sum+=score; ++known; }
        }
        for(size_t i=0;i<out.size();++i) if(out[i]<0) out[i]=known ? sum/known : 0;
    }
    return out;
}

// Who won the round: team t of the latest draw (Marcus 2026-11-02, points
// for the night), said by the Gamemaster on the dashboard or the console;
// t < 0 takes the last one said back. said: a sentence for the screens.
bool SayWinner(int t, const std::string& by, std::string& said, std::string& error) {
    std::lock_guard<std::mutex> changing(PartyChangeLock);
    Party p=CurrentParty();
    if(t<0) {
        if(p.wins.empty()) { error="No round's winner was said yet."; return false; }
        RoundWin w=p.wins.back();
        { std::lock_guard<std::mutex> hold(PartyLock); if(!TheParty.wins.empty()) TheParty.wins.pop_back(); }
        Save();
        said=w.team+"'s win in round "+Number(w.round)+" is taken back.";
        Log("LAN party: %s (%s)",said.c_str(),by.c_str());
        return true;
    }
    if(!p.drawn || t>=int(p.drawnTeams.size())) { error="Draw the teams first, then say which one won."; return false; }
    RoundWin w;
    w.round=p.wins.empty() ? 1 : p.wins.back().round+1;
    w.game=p.game; w.team=TeamName(p.teamColours,t); w.players=p.drawnTeams[size_t(t)]; w.at=(long long)time(NULL);
    if(!p.wins.empty() && p.wins.back().players==w.players && w.at-p.wins.back().at<WinAgainSeconds) { error=w.team+"'s win is counted already."; return false; }
    {
        std::lock_guard<std::mutex> hold(PartyLock);
        TheParty.wins.push_back(w);
        TidyParty(TheParty);
    }
    Save();
    std::string players;
    for(size_t i=0;i<w.players.size();++i) players+=(i ? (i+1==w.players.size() ? " and " : ", ") : "")+w.players[i];
    said=w.team+" won round "+Number(w.round)+(w.game.empty() ? std::string() : " ("+w.game+")")+": a point each for "+players+".";
    Log("LAN party: %s (%s)",said.c_str(),by.c_str());
    Event(EventInfo,"%s won round %d%s",w.team.c_str(),w.round,w.game.empty() ? "" : (" of "+w.game).c_str());
    PartyFeed("win","%s",said.c_str());
    return true;
}
// POST /api/party/winner {team}: team (0-3) of the latest draw won this round; -1 takes the last back.
void PartyWinner(const web::Value& v, web::Reply& out) {
    int team=-2;
    std::string error, said;
    if(!Whole(v,"team",-1,TeamsMost-1,team,error)) { out.Error(400,error.empty() ? "Send the team that won (0-3), or -1 to take the last back." : error); return; }
    if(!SayWinner(team,"on the dashboard",said,error)) { out.Error(400,error); return; }
    web::Json j;
    j.Open('{').Set("ok",true).Set("said",said).Close('}');
    out.body=j.Text();
}

// POST /api/party/raffle {teams, seed?}: draws teams from the guests who are
// here and seats them on the party's consoles; seed draws the same teams again.
std::atomic<unsigned> Draws(0);
void PartyRaffle(const web::Value& v, web::Reply& out) {
    std::lock_guard<std::mutex> changing(PartyChangeLock);
    Party p=CurrentParty();
    std::string error, note;
    int teams=p.teams, seed=-1;
    if((!Whole(v,"teams",TeamsLeast,TeamsMost,teams,error) && !error.empty()) || (!Whole(v,"seed",0,0x7FFFFFFF,seed,error) && !error.empty())) { out.Error(400,error); return; }
    // Through 64 bits: the microseconds since the start outgrow an unsigned after 71 minutes.
    unsigned from=seed>=0 ? unsigned(seed) : unsigned(time(NULL))*2654435761u^unsigned((unsigned long long)(Now()*1e6))^(++Draws*40503u);
    std::vector<Team> drawn;
    Seats seats;
    std::vector<std::string> here=GuestsHere(p);
    std::vector<double> strength=Strengths(p,here);
    if(!Raffle(here,teams,p.together,p.apart,p.last,from,drawn,note,error,strength.empty() ? NULL : &strength) || !Seat(drawn,p.consoles,seats,error)) { out.Error(400,error); return; }
    int round;
    {
        std::lock_guard<std::mutex> hold(PartyLock);
        TheParty.teams=teams; TheParty.drawn=true; TheParty.kept=false;
        TheParty.drawnTeams=drawn; TheParty.seats=seats; TheParty.note=note;
        TheParty.spin.steps.clear();  // Its teams aren't the ones drawn any more.
        round=TheParty.round+1;
    }
    Save();
    web::Json j;
    j.Open('{').Set("ok",true).Set("round",round).Key("teams");
    TeamsJson(j,drawn);
    j.Key("seats");
    SeatsJson(j,seats);
    j.Set("note",note).Close('}');
    out.body=j.Text();
    PushSeatProfilesSoon();
}

// The raffle wheel (see Spin in party.h): teams drawn as PartyRaffle draws
// them, or one guest picked, with every turn of the wheel planned, so the
// dashboard, the big screen and each console's NetDiscParty.xex show the
// same spin. From the dashboard (POST /api/party/spin) or the Gamemaster's
// console (PartySpinAsk). seed < 0: a new draw; teams 0: the party's.
// consoles: the ones it's for (NULL: the party's); any not in the party yet
// join it, and the teams are seated on these only. leadMs: how long before
// the wheel moves (longer while the consoles open the wheel, see OpenWheel).
bool StartSpin(bool pick, int teams, const std::string& title, int seed, const std::vector<std::string>* consoles, int leadMs,
               Spin& out, std::string& note, std::string& error, const std::vector<std::pair<std::string,int> >* among) {
    std::lock_guard<std::mutex> changing(PartyChangeLock);
    Party p=CurrentParty();
    std::vector<std::string> on=consoles ? *consoles : p.consoles, joining;
    for(size_t i=0;i<on.size();++i) if(std::find(p.consoles.begin(),p.consoles.end(),on[i])==p.consoles.end()) joining.push_back(on[i]);
    // The Gamemaster's first, as the party has them.
    std::vector<std::string>::iterator host=std::find(on.begin(),on.end(),p.gamemaster);
    if(host!=on.end()) std::rotate(on.begin(),host,host+1);
    if(!pick && on.empty()) { error="Choose the consoles the wheel is for: tick the party's consoles first."; return false; }
    double now=Now();
    if(!p.spin.steps.empty() && (now-p.spin.at)*1000<p.spin.TotalMs()) { error="The wheel is still turning."; return false; }
    if(!teams) teams=p.teams;
    unsigned from=seed>=0 ? unsigned(seed) : unsigned(time(NULL))*2654435761u^unsigned((unsigned long long)(now*1e6))^(++Draws*40503u);
    std::vector<std::string> here=GuestsHere(p);
    Spin spin;
    std::vector<Team> drawn;
    Seats seats;
    note.clear();
    if(pick && among) {
        // One of these (games voted for), each as likely as its weight (votes).
        if(among->size()<2) { error="The wheel needs two choices at least."; return false; }
        std::vector<std::string> names;
        int total=0;
        for(size_t i=0;i<among->size();++i) { names.push_back((*among)[i].first); total+=std::max(1,(*among)[i].second); }
        int roll=int(std::mt19937(from)()%unsigned(total));
        size_t picked=0;
        for(;picked+1<among->size() && roll>=std::max(1,(*among)[picked].second);++picked) roll-=std::max(1,(*among)[picked].second);
        PlanPickSpin(names,picked,title,from,spin);
    } else if(pick) {
        if(here.size()<2) { error=here.empty() ? "No one is here yet: tick the guests who are here." : "Tick at least two guests who are here, so the wheel has a choice."; return false; }
        PlanPickSpin(here,std::mt19937(from)()%here.size(),title,from,spin);
    } else {
        std::vector<double> strength=Strengths(p,here);
        if(!Raffle(here,teams,p.together,p.apart,p.last,from,drawn,note,error,strength.empty() ? NULL : &strength) || !Seat(drawn,on,seats,error)) return false;
        PlanTeamSpin(drawn,from,spin);
        // Every party console has an entry, empty when no one plays on it.
        for(size_t i=0;i<p.consoles.size();++i)
            if(std::find(on.begin(),on.end(),p.consoles[i])==on.end()) seats.push_back(std::make_pair(p.consoles[i],Team()));
    }
    spin.leadMs=std::max<int>(leadMs,SpinLeadMs);
    // A new id for each spin, also after a restart (spins aren't kept).
    spin.id=std::max(unsigned(time(NULL)),p.spin.id+1);
    spin.at=now;
    {
        std::lock_guard<std::mutex> hold(PartyLock);
        if(!pick) {
            TheParty.teams=teams; TheParty.drawn=true; TheParty.kept=false;
            TheParty.drawnTeams=drawn; TheParty.seats=seats; TheParty.note=note;
        }
        TheParty.spin=spin;
        TheParty.consoles.insert(TheParty.consoles.end(),joining.begin(),joining.end());
    }
    Save();
    out=spin;
    for(size_t i=0;i<joining.size();++i) Log("%s joins the LAN party for the wheel.",joining[i].c_str());
    if(pick && among) Log("The raffle wheel is picking one of %d games: %s",int(among->size()),title.c_str());
    else if(pick) Log("The raffle wheel is picking one of %d guests%s.",int(here.size()),title.empty() ? "" : (": "+title).c_str());
    else Log("The raffle wheel is drawing %d teams from %d guests.",teams,int(here.size()));
    PartyBusy();
    // Each guest's profile to the console they sit at, while the wheel turns.
    if(!pick) PushSeatProfilesSoon();
    return true;
}

std::string RunningGame(Console& c, const Options& o);

// Opens NetDiscParty.xex on a console from Aurora (its web server, as Start
// on all starts a game), putting this server's copy in the Xbox folder first
// when the console's isn't the same. how: "showing" when it shows the wheel
// already, "opened", or "copied and opened".
bool OpenWheel(Console& c, std::string& how, std::string& error) {
    if(ShowsWheel(c.text)) { how="showing"; return true; }
    Options o=Current();
    std::string folder=o.xboxFolder+"/"+WheelAppFolder;
    bool copied=false, placed;
    { std::lock_guard<std::mutex> hold(PartyAppLock); placed=PartyAppPut.count(c.text)>0; }
    // This build's app was put there already (in Aurora): no FTP. Otherwise
    // it's copied first, when Aurora's FTP answers; in a game it doesn't, so
    // the copy there from before is started (Aurora's web server starts it
    // from a game too): an older one makes way for this one by itself, back
    // in Aurora (see WheelUpdate), and is copied then.
    std::string ftpError;
    if(!WheelApp.empty() && !placed) {
        // Its own connection, closed before Aurora is asked to go.
        FtpClient ftp(c.address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),15);
        if(!ftp.Open()) ftpError=ftp.Error();
        else {
            ftp.MakeFolder(o.xboxFolder.c_str());
            ftp.MakeFolder(folder.c_str());
            int wrote=RefreshRemote(ftp,folder+"/default.xex",WheelApp);
            if(wrote<0) { error="Couldn't put the wheel on it: "+std::string(ftp.Error())+"."; return false; }
            copied=wrote>0;
            if(copied) XboxLog(c,"wrote the party wheel app to %s.",folder.c_str());
            std::lock_guard<std::mutex> hold(PartyAppLock);
            PartyAppPut.insert(c.text);
        }
        if(!ftpError.empty()) XboxLog(c,"Aurora's FTP doesn't answer (%s), so the party app already there is started.",ftpError.c_str());
    }
    std::string device=DevicePath(folder), why;
    if(device.empty()) { error="Aurora starts apps from the hard drive only, and the Xbox folder is "+o.xboxFolder+"."; return false; }
    if(!SafeSoon(c,StartWaitMost,why)) { error="Not now: "+why+". Try again in a moment."; return false; }
    AuroraWeb aurora(c.address,o.auroraWebPort,o.auroraWebUser,o.auroraWebPassword,8);
    if(!aurora.Launch(device,"default.xex")) {
        error=aurora.Error()+".";
        if(!ftpError.empty()) error+=" Aurora's FTP didn't answer either ("+ftpError+"), so the party app couldn't be put on it: start it once from Aurora.";
        else if(WheelApp.empty()) error+=" (This server has no copy of the wheel app to put on it: copy the \"NetDisc360 Party\" folder into "+device+" by hand.)";
        XboxLog(c,"couldn't open the party wheel: %s",error.c_str());
        return false;
    }
    { std::lock_guard<std::mutex> hold(WheelSeenLock); WheelOpened[c.text]=Now(); }
    XboxLog(c,"opened the party wheel.");
    how=copied ? "copied and opened" : "opened";
    return true;
}

// The party's consoles that are on and don't show the party app (in a game
// or in Aurora), but from.
std::vector<Console*> PartyConsolesAway(const std::string& from) {
    Party p=CurrentParty();
    std::vector<Console*> known, out;
    for(size_t i=0;i<p.consoles.size();++i) {
        if(p.consoles[i]==from || ShowsWheel(p.consoles[i])) continue;
        Console* c=ConsoleIfKnown(inet_addr(p.consoles[i].c_str()));
        if(c) known.push_back(c);
    }
    std::lock_guard<std::mutex> hold(ConsoleLock);
    for(size_t i=0;i<known.size();++i) if(!known[i]->offline && (known[i]->answers || known[i]->connections>0)) out.push_back(known[i]);
    return out;
}
std::vector<Console*> WheelToOpen(const std::string& from, bool all) {
    Party p=CurrentParty();
    Options o=Current();
    std::vector<Console*> candidates, out;
    // Only consoles that are on (one that's off, or never seen, can't open it).
    if(all) {
        std::vector<Console*> consoles=AllConsoles();
        std::lock_guard<std::mutex> hold(ConsoleLock);
        for(size_t i=0;i<consoles.size();++i)
            if(!consoles[i]->offline && (consoles[i]->answers || consoles[i]->connections>0)) candidates.push_back(consoles[i]);
    } else {
        std::vector<Console*> known;
        for(size_t i=0;i<p.consoles.size();++i) { Console* c=ConsoleIfKnown(inet_addr(p.consoles[i].c_str())); if(c) known.push_back(c); }
        std::lock_guard<std::mutex> hold(ConsoleLock);
        for(size_t i=0;i<known.size();++i) if(!known[i]->offline && (known[i]->answers || known[i]->connections>0)) candidates.push_back(known[i]);
    }
    std::vector<std::string> running(candidates.size());
    std::vector<std::thread> asks;
    for(size_t i=0;i<candidates.size();++i) {
        if(candidates[i]->text==from || ShowsWheel(candidates[i]->text)) { running[i]="-"; continue; }
        asks.push_back(std::thread([&running,&candidates,&o,i]() { running[i]=RunningGame(*candidates[i],o); }));
    }
    for(size_t i=0;i<asks.size();++i) asks[i].join();
    for(size_t i=0;i<candidates.size();++i) if(running[i].empty()) out.push_back(candidates[i]);
    return out;
}
void OpenWheels(const std::vector<Console*>& consoles) {
    for(size_t i=0;i<consoles.size();++i) {
        Console* c=consoles[i];
        std::thread([c]() {
            std::string how, error;
            if(!OpenWheel(*c,how,error)) XboxLog(*c,"couldn't open the party wheel: %s",error.c_str());
        }).detach();
    }
}

// A console's wheel app went back to Aurora to make way for this server's
// newer one (WheelUpdate): once Aurora is back, OpenWheel puts the new one
// there and starts it.
void ReopenWheel(const std::string& address) {
    Console* c=ConsoleIfKnown(inet_addr(address.c_str()));
    if(!c) return;
    {
        std::lock_guard<std::mutex> hold(WheelSeenLock);
        WheelSeen.erase(address);
        WheelUpdateAsked[address]=WheelBuild[address];
    }
    std::thread([c]() {
        Sleep(6000);
        std::string how, error;
        for(double until=Now()+60;;Sleep(3000)) {
            if(OpenWheel(*c,how,error)) { XboxLog(*c,"has the party wheel app of this build now."); return; }
            if(Now()>until) { XboxLog(*c,"couldn't put the newer party wheel app on it: %s",error.c_str()); return; }
        }
    }).detach();
}

namespace {
web::Value Field(web::Value::Type type) { web::Value v; v.type=type; return v; }
web::Value TextValue(const std::string& text) { web::Value v=Field(web::Value::String); v.text=text; return v; }
web::Value BoolValue(bool b) { web::Value v=Field(web::Value::Bool); v.boolean=b; return v; }
web::Value NumberValue(double n) { web::Value v=Field(web::Value::Number); v.number=n; return v; }
void Put(web::Value& object, const char* key, const web::Value& v) { object.fields.push_back(std::make_pair(std::string(key),v)); }
// The guest list as ChangeParty takes it, with name's here set (added, here, if it isn't on it).
web::Value GuestsValue(const Party& p, const std::string& name, bool here) {
    web::Value list=Field(web::Value::Array);
    bool found=false;
    for(size_t i=0;i<p.guests.size();++i) {
        web::Value g=Field(web::Value::Object);
        bool is=!name.empty() && SameName(p.guests[i].name,name);
        found=found || is;
        Put(g,"name",TextValue(p.guests[i].name));
        Put(g,"here",BoolValue(is ? here : p.guests[i].here));
        list.items.push_back(g);
    }
    if(!found && !name.empty()) {
        web::Value g=Field(web::Value::Object);
        Put(g,"name",TextValue(name)); Put(g,"here",BoolValue(here));
        list.items.push_back(g);
    }
    return list;
}
std::vector<std::string> Fields(const std::string& text) {
    std::vector<std::string> out;
    std::string line=text.substr(0,text.find_first_of("\r\n"));
    for(size_t from=0;from<=line.size();) {
        size_t tab=line.find('\t',from);
        if(tab==std::string::npos) tab=line.size();
        out.push_back(line.substr(from,tab-from));
        from=tab+1;
    }
    return out;
}
}

// A change to the party from a console's party screen (NetDiscParty.xex,
// PartyEditAsk): text is a command and its fields, tab-separated. Anyone
// joins (join <name>: on the guest list and here) or leaves (leave <name>);
// add <name> is the Gamemaster's join for someone else.
// The rest is the Gamemaster's, or anyone's while there is none, or while
// the Gamemaster's console doesn't show the party: gamemaster <address>
// (the party starts with it), here <name> <1|0>, console <address> <1|0>
// (one of the party's), teams <2-4>, colour <team 0-3> <colour 0-7>, start,
// openall (the party app opens on every console in Aurora, which join it).
// reopen: this console's app went back to Aurora for this server's newer
// one. "" or why not; said is a sentence for the screen.
// Game vote (Marcus 2026-11-02): every player votes from their controller
// for the next game, among those every party console can start; the wheel
// then spins between the top picks (as likely as their votes), and the one
// it stops on starts on every party console (Start on all).
struct GameVote {
    unsigned id;                          // 0: none since the server started.
    bool open;
    std::vector<std::string> games;
    std::map<std::string,int> votes;      // By "console/controller": the game's index.
    double endsAt;                        // Now() when it closes by itself.
    std::string picked;                   // What the wheel stopped on, once it did.
    GameVote() : id(0), open(false), endsAt(0) {}
};
GameVote Vote;
std::mutex VoteLock;
enum { VoteGamesMost = 10, VoteSecondsLeast = 15, VoteSecondsMost = 300, VoteSecondsUsual = 60, VoteTopMost = 4 };
void PartyStart(const web::Value& v, web::Reply& out);
// Start on all with the game the vote picked (games running end: the vote asked for it).
void StartVoted(const std::string& game) {
    web::Value v=Field(web::Value::Object);
    Put(v,"game",TextValue(game));
    Put(v,"confirm",BoolValue(true));
    web::Reply out;
    PartyStart(v,out);
    if(out.status>=400) Log("LAN party: the voted game %s couldn't start: %s",game.c_str(),out.body.c_str());
}
// The games every party console can start (its tile or an installed copy),
// System Link games first, then by name.
std::vector<std::string> PartyGames(const Party& p) {
    std::vector<IsoFile> games;
    { std::lock_guard<std::mutex> hold(GamesListLock); games=GamesList; }
    std::vector<Console*> consoles;
    for(size_t i=0;i<p.consoles.size();++i) { Console* c=ConsoleIfKnown(inet_addr(p.consoles[i].c_str())); if(c) consoles.push_back(c); }
    std::vector<std::pair<int,std::string> > out;
    if(consoles.empty()) return std::vector<std::string>();
    std::lock_guard<std::mutex> hold(ConsoleLock);
    for(size_t g=0;g<games.size();++g) {
        if(games[g].disc>1) continue;  // A game's other discs.
        bool all=true;
        for(size_t i=0;i<consoles.size() && all;++i) {
            const Console& c=*consoles[i];
            bool has=c.tiles.count(Lower(games[g].name))>0;
            for(size_t k=0;k<c.installed.size() && !has && games[g].titleId;++k) has=c.installed[k].titleId==games[g].titleId && !c.installed[k].god;
            all=has;
        }
        if(!all) continue;
        std::string name=config::ToUtf8(WithoutIso(games[g].name));
        if(std::find_if(out.begin(),out.end(),[&name](const std::pair<int,std::string>& x) { return x.second==name; })!=out.end()) continue;
        out.push_back(std::make_pair(games[g].titleId && FindSystemLink(games[g].titleId) ? 0 : 1,name));
    }
    std::sort(out.begin(),out.end());
    std::vector<std::string> names;
    for(size_t i=0;i<out.size();++i) names.push_back(out[i].second);
    return names;
}
bool CloseVote(bool cancel, std::string& said, std::string& error);
// Opens a vote among the party's games (or those given); seconds until it closes by itself.
bool OpenVote(int seconds, const std::vector<std::string>* given, std::string& said, std::string& error) {
    Party p=CurrentParty();
    if(!p.active) { error="Start the party first."; return false; }
    std::vector<std::string> games=given ? *given : PartyGames(p);
    if(games.size()>VoteGamesMost) games.resize(VoteGamesMost);
    if(games.size()<2) { error=games.empty() ? "No game is on every party console yet, so there's nothing to vote for." : "Only "+games[0]+" is on every party console: start it with Start on all."; return false; }
    {
        std::lock_guard<std::mutex> hold(VoteLock);
        if(Vote.open) { error="A vote is on already."; return false; }
        Vote.id=std::max(unsigned(time(NULL)),Vote.id+1);
        Vote.open=true; Vote.games=games; Vote.votes.clear(); Vote.picked.clear();
        Vote.endsAt=Now()+seconds;
    }
    said="Vote for the next game: "+Count(int(games.size()),"game","games")+", "+Number(seconds)+" seconds.";
    Log("LAN party: a game vote opened (%s).",Count(int(games.size()),"game","games").c_str());
    PartyFeed("vote","Vote for the next game on your controller!");
    PartyNews();
    return true;
}
// A controller's vote (console/pad), for game index (-1: taken back).
bool CastVote(const std::string& console, int pad, int index, std::string& error) {
    {
        std::lock_guard<std::mutex> hold(VoteLock);
        if(!Vote.open) { error="The vote is over."; return false; }
        if(pad<0 || pad>3 || index<-1 || index>=int(Vote.games.size())) { error="That isn't a game of the vote."; return false; }
        std::string key=console+"/"+Number(pad);
        if(index<0) Vote.votes.erase(key); else Vote.votes[key]=index;
    }
    PartyBusy();
    PartyNews();
    return true;
}
void CloseVoteIfDue() {
    bool due;
    { std::lock_guard<std::mutex> hold(VoteLock); due=Vote.open && Now()>=Vote.endsAt; }
    std::string said, error;
    if(due && !CloseVote(false,said,error)) Log("LAN party: the game vote couldn't close: %s",error.c_str());
}
// The votes for each game of the vote now.
std::vector<int> VoteCounts(const GameVote& v) {
    std::vector<int> counts(v.games.size(),0);
    for(std::map<std::string,int>::const_iterator i=v.votes.begin();i!=v.votes.end();++i) if(i->second>=0 && size_t(i->second)<counts.size()) ++counts[size_t(i->second)];
    return counts;
}
// Closes the vote: the wheel spins between the top picks (no votes: all of
// them), and the game it stops on starts on every party console after.
bool CloseVote(bool cancel, std::string& said, std::string& error) {
    GameVote v;
    {
        std::lock_guard<std::mutex> hold(VoteLock);
        if(!Vote.open) { error="No vote is on."; return false; }
        Vote.open=false;
        v=Vote;
    }
    if(cancel) { said="The game vote is called off."; Log("LAN party: %s",said.c_str()); PartyNews(); return true; }
    std::vector<int> counts=VoteCounts(v);
    std::vector<std::pair<int,size_t> > ranked;
    for(size_t i=0;i<counts.size();++i) ranked.push_back(std::make_pair(-counts[i],i));
    std::stable_sort(ranked.begin(),ranked.end());
    std::vector<std::pair<std::string,int> > top;
    for(size_t i=0;i<ranked.size();++i) {
        if(top.size()>=VoteTopMost) break;
        if(ranked[0].first<0 && ranked[i].first==0) break;  // Games with no vote stay off the wheel when some got one.
        top.push_back(std::make_pair(v.games[ranked[i].second],-ranked[i].first));
    }
    if(top.size()==1) {
        // One game got every vote: no wheel, it starts.
        said=top[0].first+" won the vote.";
        { std::lock_guard<std::mutex> hold(VoteLock); Vote.picked=top[0].first; }
        PartyFeed("vote","%s",said.c_str());
        std::string game=top[0].first;
        std::thread([game]() { Sleep(3000); StartVoted(game); }).detach();
        return true;
    }
    Spin spin;
    std::string note;
    std::vector<Console*> others=WheelToOpen(std::string(),false);
    if(!StartSpin(true,0,"Which game?",-1,NULL,others.empty() ? int(SpinLeadMs) : int(OpenLeadMs),spin,note,error,&top)) return false;
    OpenWheels(others);
    std::string game=spin.wheel[size_t(spin.steps.back().segment)];
    { std::lock_guard<std::mutex> hold(VoteLock); Vote.picked=game; }
    said="The wheel picks the game from the top "+Number(int(top.size()))+".";
    Log("LAN party: the vote is over; %s",said.c_str());
    // Once the wheel stopped (and the screens cheered), the game starts everywhere.
    int wait=spin.TotalMs()+4000;
    std::thread([game,wait]() { Sleep(DWORD(wait)); PartyFeed("vote","The wheel picked %s",game.c_str()); StartVoted(game); }).detach();
    return true;
}
// POST /api/party/vote {action: open (seconds?), close or cancel}.
void PartyVote(const web::Value& v, web::Reply& out) {
    std::string action, error, said;
    int seconds=VoteSecondsUsual;
    if(!Text(v,"action",action) || (action!="open" && action!="close" && action!="cancel")) { out.Error(400,"Send action open, close or cancel."); return; }
    if(!Whole(v,"seconds",VoteSecondsLeast,VoteSecondsMost,seconds,error) && !error.empty()) { out.Error(400,error); return; }
    bool ok=action=="open" ? OpenVote(seconds,NULL,said,error) : CloseVote(action=="cancel",said,error);
    if(!ok) { out.Error(400,error); return; }
    web::Json j;
    j.Open('{').Set("ok",true).Set("said",said).Close('}');
    out.body=j.Text();
}
// The vote as the page reads it (party.vote), or null.
void VoteJson(web::Json& j) {
    std::lock_guard<std::mutex> hold(VoteLock);
    if(!Vote.id) { j.Null(); return; }
    std::vector<int> counts=VoteCounts(Vote);
    j.Open('{').Set("id",(long long)Vote.id).Set("open",Vote.open).Set("left",Vote.open ? std::max(0,int(Vote.endsAt-Now()+0.5)) : 0).Set("picked",Vote.picked).Key("games").Open('[');
    for(size_t i=0;i<Vote.games.size();++i) j.Open('{').Set("name",Vote.games[i]).Set("votes",counts[i]).Close('}');
    j.Close(']').Set("voters",int(Vote.votes.size())).Close('}');
}

// Join from a phone (relay/): while a party is on, NetDisc360 posts it to
// the relay every 3 seconds, as phones see it (names, teams, the vote,
// points, the feed; no addresses), and takes back what phones asked: a
// name to join with, or a vote. Each join gets its answer back to the phone
// on the next post. A party that ends is posted once more, as over.
enum { RelayEveryMs = 3000, RelaySoonMs = 300, RelayFeedMost = 6, RelaySeconds = 10 };  // Every 3 s: a night of it fits Cloudflare's free plan.
std::string PartyEdit(const std::string& peer, const std::string& text, std::string& said);
std::string RelayBody(const Party& p, const std::vector<std::string>& answers) {
    web::Json j;
    j.Open('{').Key("party").Open('{').Set("active",p.active).Set("name",p.name).Set("accent",p.theme.accent).Set("game",p.game).Set("round",p.round);
    j.Key("guests");
    NamesJson(j,GuestsHere(p));
    j.Key("teams").Open('[');
    if(p.drawn)
        for(size_t t=0;t<p.drawnTeams.size();++t) {
            std::string consoles;
            for(size_t s=0;s<p.seats.size();++s) {
                bool theirs=false;
                for(size_t m=0;m<p.seats[s].second.size() && !theirs;++m)
                    theirs=std::find(p.drawnTeams[t].begin(),p.drawnTeams[t].end(),p.seats[s].second[m])!=p.drawnTeams[t].end();
                if(!theirs) continue;
                Console* c=ConsoleIfKnown(inet_addr(p.seats[s].first.c_str()));
                consoles+=(consoles.empty() ? "" : ", ")+(c ? NameOf(*c) : p.seats[s].first);
            }
            const int colour=t<p.teamColours.size() ? p.teamColours[t] : 0;
            j.Open('{').Set("name",TeamName(p.teamColours,int(t))).Set("colour",TeamPalette[colour].fill).Set("console",consoles).Key("players");
            NamesJson(j,p.drawnTeams[t]);
            j.Close('}');
        }
    j.Close(']').Key("vote");
    {
        std::lock_guard<std::mutex> hold(VoteLock);
        if(!Vote.id) j.Null();
        else {
            std::vector<int> counts=VoteCounts(Vote);
            j.Open('{').Set("id",(long long)Vote.id).Set("open",Vote.open).Set("left",Vote.open ? std::max(0,int(Vote.endsAt-Now()+0.5)) : 0).Set("picked",Vote.picked).Key("games").Open('[');
            for(size_t i=0;i<Vote.games.size();++i) j.Open('{').Set("name",Vote.games[i]).Set("votes",counts[i]).Close('}');
            j.Close(']').Close('}');
        }
    }
    j.Key("points").Open('[');
    std::vector<std::pair<std::string,int> > points=PartyPoints(p);
    for(size_t i=0;i<points.size() && i<PartyTextPointsMost;++i) j.Open('{').Set("name",points[i].first).Set("points",points[i].second).Close('}');
    j.Close(']').Key("feed").Open('[');
    {
        std::lock_guard<std::mutex> hold(FeedLock);
        int n=0;
        for(std::deque<FeedItem>::const_reverse_iterator i=Feed.rbegin();i!=Feed.rend() && n<RelayFeedMost;++i,++n) j.Open('{').Set("text",i->text).Set("at",i->at).Close('}');
    }
    j.Close(']').Close('}').Key("answers").Open('[');
    for(size_t i=0;i<answers.size();++i) j.Raw(answers[i]);
    j.Close(']').Close('}');
    return j.Text();
}
// What a phone asked for (an item of the relay's asks), done; its answer, if it gets one.
std::string RelayAsk(const web::Value& ask) {
    std::string kind, phone, name, id;
    Text(ask,"kind",kind); Text(ask,"phone",phone); Text(ask,"id",id);
    if(phone.empty() || phone.size()>40) return std::string();
    Party p=CurrentParty();
    if(kind=="vote") {
        const web::Value* vote=ask.Get("vote"), * game=ask.Get("game");
        if(!vote || !game || vote->type!=web::Value::Number || game->type!=web::Value::Number) return std::string();
        { std::lock_guard<std::mutex> hold(VoteLock); if((double)Vote.id!=vote->number) return std::string(); }
        std::string error;
        if(!CastVote("phone "+phone.substr(0,12),0,int(game->number),error)) Log("LAN party: a phone's vote didn't count: %s",error.c_str());
        return std::string();
    }
    if(kind!="join" || !Text(ask,"name",name)) return std::string();
    std::string said, error=p.active ? PartyEdit("phone","join\t"+name,said) : std::string("The party hasn't started yet.");
    std::string listed=GuestNamed(CurrentParty(),name);
    if(error.empty()) {
        Log("LAN party: %s joined from a phone.",listed.c_str());
        std::lock_guard<std::mutex> hold(RelayLock);
        ++RelayNow.joins;
    }
    web::Json j;
    j.Open('{').Set("id",id).Set("phone",phone).Set("ok",error.empty()).Set("said",error.empty() ? said : error).Set("name",error.empty() ? listed : name).Close('}');
    return j.Text();
}
void RelayLoop() {
    std::vector<std::string> answers;
    std::string over;  // The code last posted as over (the party ended).
    std::string told;  // The problem last logged.
    std::string posted;  // The room posted to last: a new one starts over (its joins too).
    int failures=0;
    for(;;) {
        Party p=CurrentParty();
        const std::string room=p.phoneRelay+"/api/room/"+p.phoneCode+"/host";
        if(room!=posted) { std::lock_guard<std::mutex> hold(RelayLock); RelayNow=RelayState(); posted=room; }
        if(p.phoneRelay.empty() || p.phoneCode.empty() || (!p.active && over==room)) {
            Sleep(1000);
            continue;
        }
        int status=0;
        std::string reply, error;
        bool ok=relay::Post(room,p.phoneKey,RelayBody(p,answers),RelaySeconds,status,reply,error);
        web::Value v;
        const bool parsed=ok && web::Parse(reply,v) && v.type==web::Value::Object;
        if(ok && status!=200) {
            ok=false;
            std::string said;
            if(parsed) Text(v,"error",said);
            error=status==403 ? "Another NetDisc360 has this party code; make a new one." : "The relay answered HTTP "+Number(status)+(said.empty() ? "." : ": "+said);
        } else if(ok && !parsed) { ok=false; error="The relay's answer isn't one NetDisc360 understands. Is the address right?"; }
        {
            std::lock_guard<std::mutex> hold(RelayLock);
            RelayNow.ok=ok; RelayNow.error=ok ? std::string() : error;
            if(ok) RelayNow.at=Now();
        }
        if(!ok) {
            if(error!=told) { Log("LAN party: the phone page's relay %s: %s",p.phoneRelay.c_str(),error.c_str()); told=error; }
            ++failures;
            Sleep(DWORD(std::min(30000,RelayEveryMs<<std::min(failures,4))));
            continue;
        }
        if(failures || !told.empty()) { Log("LAN party: phones reach the party again through %s.",p.phoneRelay.c_str()); told.clear(); }
        failures=0;
        answers.clear();
        if(!p.active) over=room; else over.clear();
        const web::Value* asks=v.Get("asks");
        for(size_t i=0;asks && asks->type==web::Value::Array && i<asks->items.size();++i) {
            if(asks->items[i].type!=web::Value::Object) continue;
            std::string answer=RelayAsk(asks->items[i]);
            if(!answer.empty()) answers.push_back(answer);
        }
        // Asks done: the phones see the party as it is now, soon.
        Sleep(asks && asks->type==web::Value::Array && !asks->items.empty() ? RelaySoonMs : RelayEveryMs);
    }
}

// "This is your 4th LAN: 12345 G so far." for a guest who was at earlier
// LANs ("" for one who wasn't).
std::string Ordinal(int n) {
    int tens=n%100, ones=n%10;
    return Number(n)+(tens>=11 && tens<=13 ? "th" : ones==1 ? "st" : ones==2 ? "nd" : ones==3 ? "rd" : "th");
}
std::string EarlierLans(const std::string& name) {
    std::vector<GuestStats> stats=GuestStatsNow();
    for(size_t i=0;i<stats.size();++i) {
        const GuestStats& g=stats[i];
        if(!SameName(g.name,name) || g.parties<=0) continue;
        std::string out="This is your "+Ordinal(g.parties+1)+" LAN";
        if(g.gamerscore>=0) out+=": "+Number(g.gamerscore)+" G so far";
        else if(g.plays>0) out+=": "+Count(g.plays,"game","games")+" played so far";
        return out+".";
    }
    return std::string();
}
std::string PartyEdit(const std::string& peer, const std::string& text, std::string& said) {
    std::vector<std::string> f=Fields(text);
    PartyBusy();
    const std::string& what=f[0];
    Party p=CurrentParty();
    std::string error;
    web::Value change=Field(web::Value::Object);
    if(what=="reopen") { ReopenWheel(peer); said="Updating the party app."; return ""; }
    if(what=="vote") {
        // A controller's vote: vote <controller> <game index or -1>.
        int pad=f.size()>=3 ? atoi(f[1].c_str()) : -1, index=f.size()>=3 ? atoi(f[2].c_str()) : -2;
        return CastVote(peer,pad,index,error) ? "" : error;
    }
    if(what=="join" || what=="leave" || what=="add") {
        std::string name;
        if(f.size()<2 || !CleanName(f[1],1,GuestNameMost,"Your name",name,error)) return error.empty() ? "Type a name." : error;
        std::string listed=GuestNamed(p,name);
        if(what=="leave") {
            if(listed.empty()) return name+" isn't on the guest list.";
            Put(change,"guests",GuestsValue(p,listed,false));
            if(!ChangeParty(change,error)) return error;
            said=listed+" left the party.";
            return "";
        }
        if(listed.empty() && p.guests.size()>=PartyGuestsMost) return "The guest list is full ("+Number(PartyGuestsMost)+" names).";
        if(what=="add" && !(p.gamemaster.empty() || p.gamemaster==peer || !ShowsWheel(p.gamemaster))) return "Only the Gamemaster's console adds players; join with A.";
        Put(change,"guests",GuestsValue(p,listed.empty() ? name : listed,true));
        if(!ChangeParty(change,error)) return error;
        if(what=="join") { std::lock_guard<std::mutex> hold(PartyLock); JoinedOn[Lower(listed.empty() ? name : listed)]=peer; }
        // Joined signed in to a profile of their own (join <name> <XUID> <gamertag>): theirs.
        if(what=="join" && f.size()>=3 && !f[2].empty()) AdoptGuestProfile(listed.empty() ? name : listed,strtoull(f[2].c_str(),NULL,16),f.size()>=4 ? f[3] : std::string(),peer);
        Log("%s joined the LAN party on %s.",(listed.empty() ? name : listed).c_str(),peer.c_str());
        MakeProfilesSoon(AutoMakeDelaySeconds);
        said=listed.empty() ? "Welcome, "+name+"!" : "Welcome back, "+listed+"!";
        // Welcome back with what they did at earlier LANs (Marcus 2026-11-02).
        std::string earlier=listed.empty() ? std::string() : EarlierLans(listed);
        if(!earlier.empty()) said+=" "+earlier;
        if(what=="join" || what=="add") PartyFeed("join","%s",listed.empty() ? (name+" joined the party").c_str() : said.c_str());
        return "";
    }
    bool master=p.gamemaster.empty() || p.gamemaster==peer || !ShowsWheel(p.gamemaster);
    if(!master) return "Only the Gamemaster's console sets up the party.";
    if(what=="gamemaster") {
        if(f.size()<2 || !KnownAddress(f[1])) return "Choose one of the consoles NetDisc360 knows.";
        Put(change,"gamemaster",TextValue(f[1]));
        web::Value consoles=Field(web::Value::Array);
        consoles.items.push_back(TextValue(f[1]));
        for(size_t i=0;i<p.consoles.size();++i) if(p.consoles[i]!=f[1]) consoles.items.push_back(TextValue(p.consoles[i]));
        Put(change,"consoles",consoles);
        Put(change,"active",BoolValue(true));
        if(!ChangeParty(change,error)) return error;
        Console* c=ConsoleIfKnown(inet_addr(f[1].c_str()));
        said=(c && !c->name.empty() ? c->name : f[1])+" is the Gamemaster.";
        return "";
    }
    if(what=="here") {
        std::string listed=f.size()>=3 ? GuestNamed(p,f[1]) : std::string();
        if(listed.empty()) return "That isn't a guest.";
        Put(change,"guests",GuestsValue(p,listed,f[2]=="1"));
        if(!ChangeParty(change,error)) return error;
        if(f[2]=="1") MakeProfilesSoon(AutoMakeDelaySeconds);
        return "";
    }
    if(what=="console") {
        if(f.size()<3 || !KnownAddress(f[1])) return "Choose one of the consoles NetDisc360 knows.";
        if(f[1]==p.gamemaster && f[2]!="1") return "The Gamemaster's console is always in the party.";
        web::Value consoles=Field(web::Value::Array);
        for(size_t i=0;i<p.consoles.size();++i) if(p.consoles[i]!=f[1]) consoles.items.push_back(TextValue(p.consoles[i]));
        if(f[2]=="1") consoles.items.push_back(TextValue(f[1]));
        Put(change,"consoles",consoles);
        return ChangeParty(change,error) ? "" : error;
    }
    if(what=="teams") {
        int n=f.size()>=2 ? atoi(f[1].c_str()) : 0;
        if(n<TeamsLeast || n>TeamsMost) return "Draw 2, 3 or 4 teams.";
        Put(change,"teams",NumberValue(n));
        return ChangeParty(change,error) ? "" : error;
    }
    if(what=="colour") {
        int t=f.size()>=3 ? atoi(f[1].c_str()) : -1, c=f.size()>=3 ? atoi(f[2].c_str()) : -1;
        if(t<0 || t>=TeamsMost || c<0 || c>=TeamColoursCount) return "That isn't a team's colour.";
        // Taken by another team: they swap.
        std::vector<int> colours=p.teamColours;
        for(size_t k=0;k<colours.size();++k) if(int(k)!=t && colours[k]==c) colours[k]=colours[t];
        colours[t]=c;
        web::Value list=Field(web::Value::Array);
        for(size_t k=0;k<colours.size();++k) list.items.push_back(NumberValue(colours[k]));
        Put(change,"teamColours",list);
        return ChangeParty(change,error) ? "" : error;
    }
    if(what=="start") {
        Put(change,"active",BoolValue(true));
        if(p.gamemaster.empty()) {
            Put(change,"gamemaster",TextValue(peer));
            web::Value consoles=Field(web::Value::Array);
            consoles.items.push_back(TextValue(peer));
            for(size_t i=0;i<p.consoles.size();++i) if(p.consoles[i]!=peer) consoles.items.push_back(TextValue(p.consoles[i]));
            Put(change,"consoles",consoles);
        }
        return ChangeParty(change,error) ? "" : error;
    }
    if(what=="winner") {
        int t=f.size()>=2 ? atoi(f[1].c_str()) : -2;
        if(t<-1 || t>=TeamsMost) return "That isn't a team.";
        return SayWinner(t,"on "+peer,said,error) ? "" : error;
    }
    if(what=="balance") {
        if(f.size()<2) return "Say random, wins or gamerscore.";
        Put(change,"balance",TextValue(f[1]));
        return ChangeParty(change,error) ? "" : error;
    }
    if(what=="voteopen") return OpenVote(f.size()>=2 && atoi(f[1].c_str())>=VoteSecondsLeast ? std::min<int>(VoteSecondsMost,atoi(f[1].c_str())) : int(VoteSecondsUsual),NULL,said,error) ? "" : error;
    if(what=="voteclose" || what=="votecancel") return CloseVote(what=="votecancel",said,error) ? "" : error;
    if(what=="backall") {
        // Back to the party on all (Marcus 2026-11-02): from a game too, which ends.
        std::vector<Console*> back=PartyConsolesAway(peer);
        OpenWheels(back);
        if(!back.empty()) Log("%s brought %s back to the party.",peer.c_str(),Count(int(back.size()),"console","consoles").c_str());
        said=back.empty() ? "Every party console that's on shows the party already." : "Bringing "+Count(int(back.size()),"console","consoles")+" back to the party.";
        return "";
    }
    if(what=="openall") {
        int opened=0;
        if(!OpenPartyEverywhere(peer,opened,error)) return error;
        said=opened ? "Opening the party on "+Count(opened,"more console","more consoles")+"." : "Every console that's on shows the party already.";
        return "";
    }
    return "NetDisc360 doesn't know that change ("+what+"); is it older than this app?";
}

// Opens the party app on every console that answers and is in Aurora
// (WheelToOpen), but from, and makes them the party's consoles; opened is
// how many it opens on.
bool OpenPartyEverywhere(const std::string& from, int& opened, std::string& error) {
    Party p=CurrentParty();
    std::vector<Console*> open=WheelToOpen(from,true);
    // The party's consoles: those it has, and every one that shows it or opens it now.
    std::vector<std::string> consoles=p.consoles;
    if(!from.empty() && std::find(consoles.begin(),consoles.end(),from)==consoles.end()) consoles.push_back(from);
    for(size_t i=0;i<open.size();++i) if(std::find(consoles.begin(),consoles.end(),std::string(open[i]->text))==consoles.end()) consoles.push_back(open[i]->text);
    {
        std::lock_guard<std::mutex> hold(WheelSeenLock);
        for(std::map<std::string,double>::const_iterator i=WheelSeen.begin();i!=WheelSeen.end();++i)
            if(Now()-i->second<WheelSeenSeconds && KnownAddress(i->first) && std::find(consoles.begin(),consoles.end(),i->first)==consoles.end()) consoles.push_back(i->first);
    }
    web::Value change=Field(web::Value::Object), list=Field(web::Value::Array);
    for(size_t i=0;i<consoles.size();++i) list.items.push_back(TextValue(consoles[i]));
    Put(change,"consoles",list);
    Put(change,"active",BoolValue(true));
    if(p.gamemaster.empty() && !from.empty()) Put(change,"gamemaster",TextValue(from));
    if(!ChangeParty(change,error)) return false;
    OpenWheels(open);
    opened=int(open.size());
    if(opened) Log("Opening the LAN party app on %s%s.",Count(opened,"console","consoles").c_str(),from.empty() ? "" : (" (asked on "+from+")").c_str());
    return true;
}

// POST /api/party/spin {kind: "teams" or "pick", teams?, title?, seed?,
// consoles?, open?, confirm?}: consoles are the ones the wheel is for (else
// the party's; any not in the party join it); open opens the wheel on them
// (OpenWheel), the wheel waiting OpenLeadMs for them before it turns. A
// console in a game would leave it, so that's asked first (409 with busy),
// unless confirm.
void PartySpin(const web::Value& v, web::Reply& out) {
    if(v.type!=web::Value::Object) { out.Error(400,"Send the spin as a JSON object."); return; }
    std::string error, kind="teams", title, note;
    int teams=0, seed=-1;
    if(Text(v,"kind",kind) && kind!="teams" && kind!="pick") { out.Error(400,"kind is teams or pick."); return; }
    if((!Whole(v,"teams",TeamsLeast,TeamsMost,teams,error) && !error.empty()) || (!Whole(v,"seed",0,0x7FFFFFFF,seed,error) && !error.empty())) { out.Error(400,error); return; }
    std::string typed;
    if(Text(v,"title",typed) && !CleanName(typed,0,SpinTitleMost,"The wheel's question",title,error)) { out.Error(400,error); return; }
    std::vector<std::string> consoles;
    const web::Value* given=v.Get("consoles");
    if(given && !PartyConsoles(*given,"consoles",consoles,error)) { out.Error(400,error); return; }
    bool open=false, confirm=false;
    Bool(v,"open",open);
    Bool(v,"confirm",confirm);
    Party p=CurrentParty();
    std::vector<std::string> opening=given ? consoles : p.consoles;
    if(open && opening.empty()) { out.Error(400,"Choose the consoles to open the wheel on: tick the party's consoles first."); return; }
    std::vector<Console*> found;
    if(open) {
        if(!p.spin.steps.empty() && (Now()-p.spin.at)*1000<p.spin.TotalMs()) { out.Error(400,"The wheel is still turning."); return; }
        for(size_t i=0;i<opening.size();++i) found.push_back(ConsoleIfKnown(inet_addr(opening[i].c_str())));
        if(!confirm) {
            Options o=Current();
            std::vector<std::string> running(found.size());
            std::vector<std::thread> asks;
            for(size_t i=0;i<found.size();++i)
                if(found[i] && !ShowsWheel(opening[i])) asks.push_back(std::thread([&running,&found,&o,i]() { running[i]=RunningGame(*found[i],o); }));
            for(size_t i=0;i<asks.size();++i) asks[i].join();
            web::Json j;
            j.Open('{').Key("busy").Open('[');
            int busy=0;
            for(size_t i=0;i<running.size();++i)
                if(!running[i].empty()) { j.Open('{').Set("address",opening[i]).Set("title",running[i]).Close('}'); ++busy; }
            j.Close(']');
            if(busy) {
                j.Set("error",busy==1 ? "A console is in a game, which it leaves to open the wheel." : "Some consoles are in a game, which they leave to open the wheel.").Close('}');
                out.status=409;
                out.body=j.Text();
                return;
            }
        }
    }
    Spin spin;
    if(!StartSpin(kind=="pick",teams,title,seed,given && kind=="teams" ? &consoles : NULL,open ? int(OpenLeadMs) : int(SpinLeadMs),spin,note,error)) { out.Error(400,error); return; }
    struct Result { bool ok; std::string how, error; };
    std::vector<Result> results(found.size());
    std::vector<std::thread> opens;
    for(size_t i=0;i<found.size();++i) opens.push_back(std::thread([&results,&found,i]() {
        Result& r=results[i];
        r.ok=false;
        if(!found[i]) r.error="That console hasn't used NetDisc360 since it started.";
        else r.ok=OpenWheel(*found[i],r.how,r.error);
    }));
    for(size_t i=0;i<opens.size();++i) opens[i].join();
    web::Json j;
    j.Open('{').Set("ok",true).Key("spin");
    SpinJson(j,spin);
    j.Set("note",note);
    if(open) {
        j.Key("opened").Open('[');
        for(size_t i=0;i<results.size();++i) j.Open('{').Set("address",opening[i]).Set("ok",results[i].ok).Set("how",results[i].how).Set("error",results[i].error).Close('}');
        j.Close(']');
    }
    j.Close('}');
    out.body=j.Text();
}

// The party as NetDiscParty.xex reads it (see party/state.h): one line for
// each thing, its fields after a tab. Names never hold a tab or a line end
// (CleanName). address is the console that asks: its own name and seats.
std::string PartyText(unsigned long long version, const std::string& address) {
    Party p=CurrentParty();
    std::map<std::string,std::string> names;
    {
        std::vector<Console*> consoles=AllConsoles();
        std::lock_guard<std::mutex> hold(ConsoleLock);
        for(size_t i=0;i<consoles.size();++i) names[consoles[i]->text]=consoles[i]->name;
    }
    // A console with no name read is "Console 2", by its place in the party.
    auto nameOf=[&](const std::string& ip) -> std::string {
        std::map<std::string,std::string>::const_iterator n=names.find(ip);
        if(n!=names.end() && !n->second.empty()) return n->second;
        size_t at=std::find(p.consoles.begin(),p.consoles.end(),ip)-p.consoles.begin();
        return at<p.consoles.size() ? "Console "+Number(int(at)+1) : std::string("This Xbox");
    };
    std::string t="netdisc-party\t1\nversion\t"+Number((long long)version)+"\n";
    auto line=[&t](const char* key, const std::vector<std::string>& fields) {
        t+=key;
        for(size_t i=0;i<fields.size();++i) { t+='\t'; t+=fields[i]; }
        t+='\n';
    };
    line("console",{nameOf(address),address,address==p.gamemaster ? "1" : "0"});
    // This server's build, and whether this console's app should make way for its newer one.
    line("build",{Number((long long)NETDISC_BUILD_STAMP),NETDISC_COMMIT,NETDISC_VERSION,WheelUpdate(address) ? "1" : "0"});
    line("party",{p.active ? "1" : "0",PartyTitle(p),PartyLine(p),p.theme.preset});
    line("colours",{p.theme.accent,p.theme.accent2});
    if(!p.gamemaster.empty()) line("gamemaster",{nameOf(p.gamemaster),p.gamemaster});
    line("teams",{Number(p.teams)});
    line("balance",{BalanceNames[p.balance]});
    // The game vote: its id, open (1) or over, seconds left, what it picked;
    // each game with its votes; and this console's controllers' votes.
    {
        std::lock_guard<std::mutex> hold(VoteLock);
        if(Vote.id) {
            std::vector<int> counts=VoteCounts(Vote);
            line("vote",{Number((long long)Vote.id),Vote.open ? "1" : "0",Number(Vote.open ? std::max(0,int(Vote.endsAt-Now()+0.5)) : 0),Vote.picked});
            for(size_t i=0;i<Vote.games.size();++i) line("votegame",{Vote.games[i],Number(counts[i])});
            for(int pad=0;pad<4;++pad) {
                std::map<std::string,int>::const_iterator mine=Vote.votes.find(address+"/"+Number(pad));
                if(mine!=Vote.votes.end()) line("myvote",{Number(pad),Number(mine->second)});
            }
        }
    }
    // Readiness: how many of the party's consoles are ready, and the first thing in the way.
    if(!p.consoles.empty()) {
        int ready=0;
        std::string first;
        for(size_t i=0;i<p.consoles.size();++i) {
            std::vector<std::string> problems=NotReady(p,p.consoles[i]);
            if(problems.empty()) ++ready;
            else if(first.empty()) first=nameOf(p.consoles[i])+": "+problems[0];
        }
        line("ready",{Number(ready),Number(int(p.consoles.size())),first});
    }
    // The night's points (most first) and rounds won.
    {
        std::vector<std::pair<std::string,int> > points=PartyPoints(p);
        for(size_t i=0;i<points.size() && i<PartyTextPointsMost;++i) line("points",{points[i].first,Number(points[i].second)});
        if(!p.wins.empty()) line("won",{Number(p.wins.back().round),p.wins.back().team,p.wins.back().game});
    }
    {
        std::string job=PartyAppJobFor(address);
        if(!job.empty()) line("makeprofiles",{job});
    }
    // The colours a team can have (index, name, fill, bright, dark text), and each team's.
    for(int i=0;i<TeamColoursCount;++i)
        line("palette",{Number(i),TeamPalette[i].name,TeamPalette[i].fill,TeamPalette[i].bright,TeamPalette[i].darkInk ? "1" : "0"});
    {
        std::vector<std::string> colours;
        for(size_t i=0;i<p.teamColours.size();++i) colours.push_back(Number(p.teamColours[i]));
        line("teamcolours",colours);
    }
    std::vector<std::string> here=GuestsHere(p);
    line("here",here);
    // The whole guest list (kept from party to party): here or not, the
    // gamerscore (-1: not read), and whether they joined on this console.
    {
        std::vector<GuestStats> stats=GuestStatsNow();
        std::map<std::string,std::string> joined;
        { std::lock_guard<std::mutex> hold(PartyLock); joined=JoinedOn; }
        for(size_t i=0;i<p.guests.size();++i) {
            int score=-1;
            for(size_t k=0;k<stats.size();++k) if(SameName(stats[k].name,p.guests[i].name)) score=stats[k].gamerscore;
            std::map<std::string,std::string>::const_iterator j=joined.find(Lower(p.guests[i].name));
            line("guest",{p.guests[i].here ? "1" : "0",p.guests[i].name,Number(score),j!=joined.end() && j->second==address ? "1" : "0"});
        }
    }
    // The consoles NetDisc360 knows, for the Gamemaster to choose from:
    // address, name, what it does (wheel, aurora, game or off) and whether
    // it's one of the party's.
    {
        std::vector<Console*> consoles=AllConsoles();
        for(size_t i=0;i<consoles.size();++i) {
            Console& c=*consoles[i];
            std::string state;
            {
                std::lock_guard<std::mutex> hold(ConsoleLock);
                if(c.hiddenAt!=-2 && c.seenAt<=c.hiddenAt && !c.connections) continue;  // Forgotten.
                state=c.offline ? "off" : c.connections>0 && !c.mounted.empty() && !c.inAurora ? "game" : c.answers || c.connections>0 ? "aurora" : "off";
            }
            if(ShowsWheel(c.text)) state="wheel";
            // Named as Aurora calls it, else by its address (to tell them apart).
            std::map<std::string,std::string>::const_iterator n=names.find(c.text);
            line("known",{c.text,n!=names.end() && !n->second.empty() ? n->second : std::string(c.text),state,std::find(p.consoles.begin(),p.consoles.end(),std::string(c.text))!=p.consoles.end() ? "1" : "0"});
        }
    }
    if(!p.spin.steps.empty()) {
        const Spin& s=p.spin;
        line("spin",{Number((long long)s.id),s.kind,Number((long long)((Now()-s.at)*1000)),Number(s.leadMs),Number(s.pauseMs),s.title});
        line("wheel",s.wheel);
        for(size_t i=0;i<s.steps.size();++i) {
            char to[32]; _snprintf_s(to,sizeof(to),_TRUNCATE,"%.3f",s.steps[i].to);
            line("step",{Number(s.steps[i].segment),Number(s.steps[i].team),to,Number(s.steps[i].ms)});
        }
    }
    if(p.drawn && !p.drawnTeams.empty()) {
        line("round",{Number(p.round+(p.kept ? 0 : 1)),p.kept ? "1" : "0"});
        for(size_t i=0;i<p.drawnTeams.size();++i) line("team",p.drawnTeams[i]);
        for(size_t i=0;i<p.seats.size();++i) {
            std::vector<std::string> fields={p.seats[i].first,nameOf(p.seats[i].first)};
            fields.insert(fields.end(),p.seats[i].second.begin(),p.seats[i].second.end());
            line("seat",fields);
        }
        // Who signed in on this console already (PartySignIn): the seat, the controller and the gamertag.
        std::lock_guard<std::mutex> hold(PartyLock);
        std::vector<const PartySignInRecord*> signedIn=SignedIn(p,address);
        for(size_t i=0;i<signedIn.size();++i) if(signedIn[i]) line("signin",{Number(int(i)),Number(signedIn[i]->controller),signedIn[i]->gamertag});
    }
    // This console's guest profiles on their way (PushSeatProfiles): state, done, total, whose now, note.
    {
        std::lock_guard<std::mutex> hold(GuestLock);
        std::map<std::string,SeatProfiles>::const_iterator to=SeatPushes.find(address);
        if(to!=SeatPushes.end()) line("profiles",{to->second.state,Number(to->second.done),Number(to->second.total),to->second.now,to->second.note});
    }
    return t;
}

// POST /api/party/keep: the teams drawn are this round's, and the next
// raffle doesn't draw them again.
bool KeepTeams(int& round, std::string& error) {
    std::lock_guard<std::mutex> changing(PartyChangeLock);
    {
        std::lock_guard<std::mutex> hold(PartyLock);
        if(!TheParty.drawn || TheParty.drawnTeams.empty()) { error="Draw the teams first."; return false; }
        if(!TheParty.kept) { TheParty.last=TheParty.drawnTeams; TheParty.kept=true; ++TheParty.round; }
        round=TheParty.round;
    }
    Save();
    return true;
}

// POST /api/party/seats {seats: {address: [names]}}: seats changed by hand
// (a swap): the party's consoles, each once, up to SeatsPerConsole guests
// who are here on each, each guest once. A console left out gets no one.
bool ChangeSeats(const web::Value& v, std::string& error) {
    const web::Value* given=v.Get("seats");
    if(!given || given->type!=web::Value::Object) { error="Send seats: the guests on each console, by its address."; return false; }
    std::lock_guard<std::mutex> changing(PartyChangeLock);
    Party p=CurrentParty();
    std::vector<std::string> here=GuestsHere(p), seen, listed;
    Seats seats;
    for(size_t i=0;i<p.consoles.size();++i) seats.push_back(std::make_pair(p.consoles[i],Team()));
    for(size_t f=0;f<given->fields.size();++f) {
        const std::string& address=given->fields[f].first;
        size_t at=std::find(p.consoles.begin(),p.consoles.end(),address)-p.consoles.begin();
        if(at==p.consoles.size()) { error=address+" isn't one of the party's consoles."; return false; }
        // The JSON can name a console twice, which would seat more than SeatsPerConsole on it.
        if(std::find(listed.begin(),listed.end(),address)!=listed.end()) { error=address+" is listed twice."; return false; }
        listed.push_back(address);
        std::vector<std::string> names;
        if(!Strings(given->fields[f].second,names)) { error="Each console's seats are a list of guests' names."; return false; }
        if(names.size()>SeatsPerConsole) { error="Up to "+Number(SeatsPerConsole)+" play on one console (split-screen)."; return false; }
        for(size_t i=0;i<names.size();++i) {
            std::string name=GuestNamed(p,names[i]);
            bool isHere=false;
            for(size_t k=0;k<here.size() && !isHere;++k) isHere=SameName(here[k],names[i]);
            if(name.empty() || !isHere) { error=names[i]+" isn't a guest who is here."; return false; }
            for(size_t k=0;k<seen.size();++k) if(SameName(seen[k],name)) { error=name+" has two seats."; return false; }
            seen.push_back(name);
            seats[at].second.push_back(name);
        }
    }
    {
        std::lock_guard<std::mutex> hold(PartyLock);
        TheParty.seats=seats;
        if(!TheParty.drawn) { TheParty.drawn=true; TheParty.kept=false; TheParty.drawnTeams.clear(); TheParty.note.clear(); }
    }
    Save();
    return true;
}

// The consoles a party command is for: those the page names (consoles),
// else the party's; the Gamemaster's first.
bool CommandConsoles(const web::Value& v, std::vector<std::string>& out, std::string& error) {
    Party p=CurrentParty();
    const web::Value* given=v.Get("consoles");
    if(given) { if(!PartyConsoles(*given,"consoles",out,error)) return false; }
    else out=p.consoles;
    if(out.empty()) { error="Choose the Gamemaster console and tick the party's consoles first."; return false; }
    std::vector<std::string>::iterator host=std::find(out.begin(),out.end(),p.gamemaster);
    if(host!=out.end()) std::rotate(out.begin(),host,host+1);
    return true;
}

// A game a console runs, by the path Aurora's web server gives: its tile's
// or its installed copy's game, else the folder it's in.
std::string RunningName(const Console& c, const Options& o, const std::string& path) {
    size_t cut=path.rfind('\\');
    std::string folder=cut==std::string::npos ? path : path.substr(0,cut);
    std::string listed;
    DWORD titleId=0;
    {
        std::lock_guard<std::mutex> hold(ConsoleLock);
        for(std::map<std::string,std::string>::const_iterator t=c.tiles.begin();t!=c.tiles.end() && listed.empty();++t)
            if(!_stricmp(DevicePath(o.xboxFolder+"/"+t->second).c_str(),folder.c_str())) listed=t->first;
        for(size_t i=0;i<c.installed.size() && !titleId;++i)
            if(!_stricmp(DevicePath(c.installed[i].path.substr(0,c.installed[i].path.rfind('/'))).c_str(),folder.c_str())) titleId=c.installed[i].titleId;
    }
    {
        std::lock_guard<std::mutex> hold(GamesListLock);
        for(size_t i=0;i<GamesList.size();++i)
            if((!listed.empty() && Lower(GamesList[i].name)==listed) || (titleId && GamesList[i].titleId==titleId)) return config::ToUtf8(WithoutIso(GamesList[i].name));
    }
    cut=folder.rfind('\\');
    return cut==std::string::npos ? folder : folder.substr(cut+1);
}

// What console c runs when it isn't Aurora: its NetISO game, or what Aurora's
// web server names (a game installed on it, say). "" in Aurora, or when that
// can't be told: whether Aurora's web server answers during a game isn't
// known (it doesn't always), so then only a NetISO game shows.
std::string RunningGame(Console& c, const Options& o) {
    {
        std::lock_guard<std::mutex> hold(ConsoleLock);
        if(c.connections>0 && !c.mounted.empty() && !c.inAurora) return c.game.empty() ? config::ToUtf8(WithoutIso(c.mounted)) : c.game;
    }
    AuroraWeb aurora(c.address,o.auroraWebPort,o.auroraWebUser,o.auroraWebPassword,4);
    std::string path;
    if(!aurora.Title(path) || path.empty()) return std::string();
    std::string lower=Lower(path);
    if(lower.size()>=11 && !lower.compare(lower.size()-11,11,"\\aurora.xex")) return std::string();
    return RunningName(c,o,path);
}

// The title updates of a game on consoles, for Start on all's answer: the one
// each console that was read applies ("none" for none), the newest any of
// them has, whether they differ, and the consoles whose title updates were
// never read (unread), so left out.
void UpdatesJson(web::Json& j, const IsoFile& game, const std::vector<std::string>& addresses) {
    std::vector<ConsoleUpdates> consoles=UpdatesOf(addresses);  // u points into it.
    GameUpdates u=JudgeUpdates(game.titleId,game.mediaId,consoles);
    j.Key("titleUpdates").Open('{').Set("mismatch",u.mismatch).Key("byConsole").Open('{');
    for(size_t i=0;i<u.inUse.size();++i) j.Set(u.inUse[i].first.c_str(),u.inUse[i].second ? UpdateName(*u.inUse[i].second) : std::string("none"));
    j.Close('}').Set("newest",u.newest ? UpdateName(*u.newest) : std::string()).Key("unread").Open('[');
    for(size_t i=0;i<addresses.size();++i) {
        bool read=false;
        for(size_t k=0;k<u.inUse.size() && !read;++k) read=u.inUse[k].first==addresses[i];
        if(!read) j.String(addresses[i]);
    }
    j.Close(']').Close('}');
}

// POST /api/party/start {game, consoles?, confirm?}: Start on all. The same
// game on every party console (or those named), the Gamemaster's first,
// then the others at once; each from its installed copy if it has one (an
// extracted one), else its tile. A console in a game would leave it, so
// without confirm the answer is 409 with the busy consoles, for the page to
// ask once and send again with confirm. The consoles' title updates are
// looked at first (at once with that), for the answer's warning.
void PartyStart(const web::Value& v, web::Reply& out) {
    std::string name, error;
    if(!Text(v,"game",name)) { out.Error(400,"Send the game."); return; }
    IsoFile game;
    if(!LibraryGame(name,game)) { out.Error(400,"That game isn't in your folders any more."); return; }
    std::vector<std::string> addresses;
    if(!CommandConsoles(v,addresses,error)) { out.Error(400,error); return; }
    bool confirm=false;
    Bool(v,"confirm",confirm);
    Options o=Current();
    std::vector<Console*> consoles;
    for(size_t i=0;i<addresses.size();++i) consoles.push_back(ConsoleIfKnown(inet_addr(addresses[i].c_str())));
    std::thread updates([&consoles,&o]() { LookTitleUpdatesNow(consoles,o,4); });
    if(!confirm) {
        std::vector<std::string> running(consoles.size());
        std::vector<std::thread> asks;
        for(size_t i=0;i<consoles.size();++i)
            if(consoles[i]) asks.push_back(std::thread([&running,&consoles,&o,i]() { running[i]=RunningGame(*consoles[i],o); }));
        for(size_t i=0;i<asks.size();++i) asks[i].join();
        updates.join();
        web::Json j;
        j.Open('{').Key("busy").Open('[');
        int busy=0;
        for(size_t i=0;i<running.size();++i)
            if(!running[i].empty()) { j.Open('{').Set("address",addresses[i]).Set("title",running[i]).Close('}'); ++busy; }
        j.Close(']');
        if(busy) {
            j.Set("error",busy==1 ? "A console is in a game, which it leaves to start this one." : "Some consoles are in a game, which they leave to start this one.").Close('}');
            out.status=409;
            out.body=j.Text();
            return;
        }
    } else updates.join();
    // The same copy on every console (Marcus 2026-11-02): when some have it
    // installed and others don't, all start the network copy (its tile),
    // the very same disc for everyone.
    std::string copy=SameCopy(consoles,game);
    const bool preferInstalled=copy!="tile";
    if(copy=="tile") Log("Start on all: %s is installed on some of the consoles only, so every console starts the copy on this PC (the same one).",WithoutIso(game.name).c_str());
    struct Result { bool ok; std::string how, error; };
    std::vector<Result> results(consoles.size());
    auto start=[&results,&consoles,&game,preferInstalled](size_t i) {
        Result& r=results[i];
        r.ok=false;
        if(!consoles[i]) r.error="That console hasn't used NetDisc360 since it started.";
        else r.ok=StartOn(*consoles[i],game,preferInstalled,r.how,r.error);
    };
    start(0);
    std::vector<std::thread> starts;
    for(size_t i=1;i<consoles.size();++i) starts.push_back(std::thread(start,i));
    for(size_t i=0;i<starts.size();++i) starts[i].join();
    int started=0;
    web::Json j;
    j.Open('{').Set("ok",true).Key("results").Open('[');
    for(size_t i=0;i<results.size();++i) {
        j.Open('{').Set("address",addresses[i]).Set("ok",results[i].ok).Set("how",results[i].how).Set("error",results[i].error).Close('}');
        started+=results[i].ok;
    }
    j.Close(']').Set("copy",copy);
    UpdatesJson(j,game,addresses);
    j.Close('}');
    out.body=j.Text();
    if(started) {
        { std::lock_guard<std::mutex> hold(PartyLock); TheParty.game=config::ToUtf8(WithoutIso(game.name)); }
        Save();
        PartyFeed("game","%s started on %s",config::ToUtf8(WithoutIso(game.name)).c_str(),Count(started,"console","consoles").c_str());
    }
    Log("Start on all: %s started on %d of %s.",WithoutIso(game.name).c_str(),started,Count(int(results.size()),"console","consoles").c_str());
    Event(EventPlay,"%s started on %d of %s (Start on all)",WithoutIso(game.name).c_str(),started,Count(int(results.size()),"console","consoles").c_str());
}

// POST /api/party/power {action, consoles?}: Back to Aurora on all (aurora),
// Back to the party on all (party: the party app, from a game too) or Turn
// off all (off), on the party's consoles (or those named) at once.
void PartyPower(const web::Value& v, web::Reply& out) {
    std::string action, error;
    if(!Text(v,"action",action) || (action!="aurora" && action!="off" && action!="party")) { out.Error(400,"Send action aurora, party or off."); return; }
    std::vector<std::string> addresses;
    if(!CommandConsoles(v,addresses,error)) { out.Error(400,error); return; }
    std::vector<std::string> errors(addresses.size());
    std::vector<char> done(addresses.size(),0);
    std::vector<std::thread> threads;
    for(size_t i=0;i<addresses.size();++i)
        threads.push_back(std::thread([&addresses,&errors,&done,&action,i]() {
            Console* c=AskedConsole(addresses[i],errors[i]);
            std::string how;
            done[i]=c && (action=="party" ? OpenWheel(*c,how,errors[i]) : PowerOn(*c,action,errors[i]));
        }));
    for(size_t i=0;i<threads.size();++i) threads[i].join();
    web::Json j;
    j.Open('{').Key("results").Open('[');
    for(size_t i=0;i<addresses.size();++i) j.Open('{').Set("address",addresses[i]).Set("ok",done[i]!=0).Set("error",done[i] ? std::string() : errors[i]).Close('}');
    j.Close(']').Close('}');
    out.body=j.Text();
}

// Whether data starts as a picture of type does (PNG, JPEG, WebP).
bool PictureIs(const std::string& data, const std::string& type) {
    if(type=="image/png") return data.size()>8 && !data.compare(0,8,"\x89PNG\r\n\x1a\n",8);
    if(type=="image/jpeg") return data.size()>3 && !data.compare(0,3,"\xFF\xD8\xFF",3);
    if(type=="image/webp") return data.size()>12 && !data.compare(0,4,"RIFF") && !data.compare(8,4,"WEBP");
    return false;
}

// POST /api/party/asset {kind: logo or background, type, data (base64)}: a
// picture for the party's theme. A new logo takes the old one's place; a
// background photo goes after the others. index: where it is.
void PartyAsset(const web::Value& v, web::Reply& out) {
    Options o=Current();
    std::string folder=PartyFolder(o), kind, type, data, bytes;
    if(folder.empty()) { out.Error(400,"The settings aren't kept in a file (start NetDisc360 with --config), so pictures can't be kept."); return; }
    if(!Text(v,"kind",kind) || (kind!="logo" && kind!="background")) { out.Error(400,"kind is logo or background."); return; }
    const char* extension=NULL;
    Text(v,"type",type);
    for(size_t i=0;i<sizeof(PictureTypes)/sizeof(PictureTypes[0]);++i) if(type==PictureTypes[i][1]) extension=PictureTypes[i][0];
    if(!extension) { out.Error(400,"Send a PNG, JPEG or WebP picture."); return; }
    if(!Text(v,"data",data) || !backup::FromBase64(data,bytes) || bytes.empty()) { out.Error(400,"The picture didn't arrive whole; try again."); return; }
    if(bytes.size()>PartyPictureMost) {
        char size[32]; _snprintf_s(size,sizeof(size),_TRUNCATE,"%.1f",bytes.size()/1048576.0);
        out.Error(400,std::string("That picture is ")+size+" MB; "+Number(PartyPictureMost>>20)+" MB at most."); return;
    }
    if(!PictureIs(bytes,type)) { out.Error(400,std::string("That file isn't a ")+(extension[0]=='p' ? "PNG" : extension[0]=='j' ? "JPEG" : "WebP")+" picture."); return; }
    std::lock_guard<std::mutex> hold(PartyFilesLock);
    PartyPictures pictures=PicturesNow(o);
    if(kind=="background" && pictures.backgrounds.size()>=PartyBackgroundsMost) {
        out.Error(400,"A party has "+Number(PartyBackgroundsMost)+" background photos at most; remove one first."); return;
    }
    std::string file=kind=="logo" ? std::string("logo.")+extension : "background"+Number(pictures.most+1)+"."+extension;
    if(!config::MakeFolder(folder) || !config::WriteFile(InFolder(folder,file),bytes,false)) { out.Error(400,"The picture couldn't be saved in "+folder+"."); return; }
    if(kind=="logo" && !pictures.logo.empty() && pictures.logo!=file) config::RemoveFile(InFolder(folder,pictures.logo));
    Log("The LAN party's %s was added on the dashboard (%s).",kind=="logo" ? "logo" : "background photo",InFolder(folder,file).c_str());
    web::Json j;
    j.Open('{').Set("ok",true).Set("index",kind=="logo" ? 0 : int(pictures.backgrounds.size())).Close('}');
    out.body=j.Text();
}

// POST /api/party/asset/remove {kind, index?}: removes the logo, a background
// photo (by index), or every background photo (without one).
bool RemovePartyAsset(const web::Value& v, std::string& error) {
    Options o=Current();
    std::string kind;
    int index=-1;
    if(!Text(v,"kind",kind) || (kind!="logo" && kind!="background")) { error="kind is logo or background."; return false; }
    if(!Whole(v,"index",0,PartyBackgroundsMost-1,index,error) && !error.empty()) return false;
    std::lock_guard<std::mutex> hold(PartyFilesLock);
    PartyPictures pictures=PicturesNow(o);
    std::vector<std::string> gone;
    if(kind=="logo") { if(!pictures.logo.empty()) gone.push_back(pictures.logo); }
    else if(index<0) gone=pictures.backgrounds;
    else if(size_t(index)<pictures.backgrounds.size()) gone.push_back(pictures.backgrounds[size_t(index)]);
    else { error="There's no background photo "+Number(index+1)+"."; return false; }
    for(size_t i=0;i<gone.size();++i) config::RemoveFile(InFolder(PartyFolder(o),gone[i]));
    return true;
}

// GET /api/party/asset?kind=logo, or ?kind=background&n=0: the picture.
void PartyAssetFile(const web::Request& r, web::Reply& out) {
    Options o=Current();
    std::string kind=r.Param("kind"), n=r.Param("n"), file;
    PartyPictures pictures=PicturesNow(o);
    if(kind=="logo") file=pictures.logo;
    else if(kind=="background" && !n.empty() && n.size()<=3 && n.find_first_not_of("0123456789")==std::string::npos && size_t(atoi(n.c_str()))<pictures.backgrounds.size())
        file=pictures.backgrounds[size_t(atoi(n.c_str()))];
    if(file.empty() || !config::ReadFile(InFolder(PartyFolder(o),file),out.body,PartyPictureMost+1)) { out.Error(404,"There's no such picture."); return; }
    out.type=PictureType(file);
}

// GET /api/party/profiles/picture?gamertag=...&size=64 (or 32): the gamer
// picture a guest profile with that gamertag gets (picture.h).
void GuestPictureFile(const web::Request& r, web::Reply& out) {
    std::string tag=r.Param("gamertag");
    if(tag.empty() || tag.size()>GamertagLongest) { out.Error(400,"Which gamertag?"); return; }
    out.body=GamerPicturePng(tag,r.Param("size")=="32" ? 32 : 64);
    out.type="image/png";
}

// The LAN party's guest profiles (see guests.h) --------------------------------

// What the guest profile work is doing, for the page: one piece of work at a time.
struct ProfileWork {
    bool running;
    std::string what, step, result, error;
    long long at;  // When it started or ended (Unix seconds).
    ProfileWork() : running(false), at(0) {}
};
ProfileWork Work;  // Under GuestLock.
// The longest a file in a guest's content folder may be (a game's save can be
// big; a profile is a few MB).
const size_t ProfileFileMost = size_t(512)<<20;
const int ProfileTreeDeepest = 6;

std::string ProfilesFolder(const Options& o) { std::string b=BackupFolder(o); return b.empty() ? b : KitPath(b,"Profiles"); }

void WorkStep(const char* format, ...) {
    char text[512];
    va_list a; va_start(a,format); _vsnprintf_s(text,sizeof(text),_TRUNCATE,format,a); va_end(a);
    std::lock_guard<std::mutex> hold(GuestLock);
    Work.step=text;
}
// A record changed by the work: written back by name, then saved.
void KeepGuestProfile(const GuestProfile& p) {
    {
        std::lock_guard<std::mutex> hold(GuestLock);
        GuestProfile* had=GuestProfileNamed(GuestProfiles,p.name);
        if(had) *had=p; else GuestProfiles.push_back(p);
    }
    Save();
}
void SetOn(GuestProfile& p, const std::string& console, bool on) {
    std::vector<std::string>::iterator at=std::find(p.on.begin(),p.on.end(),console);
    if(on && at==p.on.end()) p.on.push_back(console);
    if(!on && at!=p.on.end()) p.on.erase(at);
    std::vector<std::string>::iterator old=std::find(p.stale.begin(),p.stale.end(),console);
    if(!on && old!=p.stale.end()) p.stale.erase(old);
    if(!on && p.playedOn==console) { p.playedOn.clear(); p.stale.clear(); }
}
// p is on console now, as the record has it now (another console's copy
// may have been noted meanwhile); p gets the record.
// fresh: its copy there is as new as the one on the console they played
// on last (false: older, brought up to date later).
void MarkProfileOn(GuestProfile& p, const std::string& console, bool fresh = true) {
    {
        std::lock_guard<std::mutex> hold(GuestLock);
        GuestProfile* had=GuestProfileNamed(GuestProfiles,p.name);
        if(had && had->xuid==p.xuid) p=*had;
    }
    SetOn(p,console,true);
    std::vector<std::string>::iterator old=std::find(p.stale.begin(),p.stale.end(),console);
    if(fresh && old!=p.stale.end()) p.stale.erase(old);
    if(!fresh && old==p.stale.end() && !p.playedOn.empty() && p.playedOn!=console) p.stale.push_back(console);
    KeepGuestProfile(p);
}
// Progress follows the player (Marcus): name signed in or plays on console
// with their guest profile (xuid), so the copy there is the newest and the
// others are older from now on; PushSeatsOnce brings those up to date.
void PlaysOn(const std::string& name, unsigned long long xuid, const std::string& console) {
    char hex[24];
    _snprintf_s(hex,sizeof(hex),_TRUNCATE,"%016llX",xuid);
    {
        std::lock_guard<std::mutex> hold(GuestLock);
        GuestProfile* had=GuestProfileNamed(GuestProfiles,name);
        if(!had || had->xuid.empty() || _stricmp(had->xuid.c_str(),hex)) return;
        if(std::find(had->on.begin(),had->on.end(),console)==had->on.end()) had->on.push_back(console);
        std::vector<std::string> stale;
        for(size_t i=0;i<had->on.size();++i) if(had->on[i]!=console) stale.push_back(had->on[i]);
        if(had->playedOn==console && had->stale==stale) return;
        had->playedOn=console; had->stale=stale;
    }
    Save();
}
// The console with name's newest progress, when the copy on console is older
// than it ("" when it isn't).
std::string NewerOn(const std::string& name, const std::string& console) {
    std::lock_guard<std::mutex> hold(GuestLock);
    GuestProfile* had=GuestProfileNamed(GuestProfiles,name);
    if(!had || had->playedOn.empty() || had->playedOn==console) return std::string();
    if(std::find(had->stale.begin(),had->stale.end(),console)==had->stale.end()) return std::string();
    if(std::find(had->on.begin(),had->on.end(),had->playedOn)==had->on.end()) return std::string();
    return had->playedOn;
}
// A guest who signs in (or joins) on a console's party screen with an
// offline profile of their own, one made there by hand: it becomes their
// guest profile, unless NetDisc360's for them is on a console already, and
// goes to the party's other consoles like the others (PushSeatProfilesSoon).
void AdoptGuestProfile(const std::string& name, unsigned long long xuid, const std::string& gamertag, const std::string& console) {
    char hex[24];
    _snprintf_s(hex,sizeof(hex),_TRUNCATE,"%016llX",xuid);
    std::string id;
    if(name.empty() || !OfflineXuid(hex,id)) return;
    GuestProfile p;
    {
        std::lock_guard<std::mutex> hold(GuestLock);
        GuestProfile* had=GuestProfileNamed(GuestProfiles,name);
        if(had && had->xuid==id) {
            if(std::find(had->on.begin(),had->on.end(),console)!=had->on.end()) return;
            p=*had;
        } else if(had && !had->xuid.empty() && !had->on.empty()) return;  // Theirs from NetDisc360, on a console.
        else {
            if(had) p=*had; else p.name=name;
            // No other guest's profile is this one.
            for(size_t i=0;i<GuestProfiles.size();++i) if(GuestProfiles[i].xuid==id && !SameName(GuestProfiles[i].name,name)) return;
            p.xuid=id; p.gamertag=gamertag.empty() ? id : gamertag; p.made=(long long)time(NULL); p.madeOn=console; p.adopted=true; p.on.clear();
        }
    }
    SetOn(p,console,true);
    KeepGuestProfile(p);
    Log("%s: %s's own profile %s (%s) is their guest profile now, for every party console.",console.c_str(),name.c_str(),p.gamertag.c_str(),id.c_str());
    PushSeatProfilesSoon();
}

// Every file under folder on the console, by its path under it (with /).
bool ReadTree(FtpClient& ftp, const std::string& folder, const std::string& under, std::map<std::string,std::string>& files, std::string& error, int depth = 0) {
    std::vector<FtpEntry> entries;
    std::string here=under.empty() ? folder : folder+"/"+under;
    if(!ftp.List(here.c_str(),entries)) { error="Couldn't open "+DashPath(here)+": "+ftp.Error(); return false; }
    for(size_t i=0;i<entries.size();++i) {
        const FtpEntry& e=entries[i];
        if(e.name.empty() || e.name=="." || e.name=="..") continue;
        std::string path=under.empty() ? e.name : under+"/"+e.name;
        if(e.folder) {
            if(depth>=ProfileTreeDeepest) { error=DashPath(folder+"/"+path)+" goes deeper than a content folder does."; return false; }
            if(!ReadTree(ftp,folder,path,files,error,depth+1)) return false;
            continue;
        }
        std::vector<unsigned char> data;
        if(!ftp.Retrieve((folder+"/"+path).c_str(),data,ProfileFileMost)) { error="Couldn't read "+DashPath(folder+"/"+path)+": "+ftp.Error(); return false; }
        files[path].assign(data.begin(),data.end());
    }
    return true;
}
// Writes files (paths under folder, with /) to the console, making their folders.
bool WriteTree(FtpClient& ftp, const std::string& folder, const std::map<std::string,std::string>& files, std::string& error) {
    std::set<std::string> made;
    for(std::map<std::string,std::string>::const_iterator f=files.begin();f!=files.end();++f) {
        std::string path=folder+"/"+f->first;
        for(size_t cut=std::string("/Hdd1").size();(cut=path.find('/',cut+1))!=std::string::npos;) {
            std::string parent=path.substr(0,cut);
            if(made.insert(parent).second) EnsureFolder(ftp,parent);
        }
        if(!StoreChecked(ftp,path,f->second)) { error="Couldn't write "+DashPath(path)+": "+ftp.Error(); return false; }
    }
    return true;
}
// Removes folder and everything in it from the console.
bool DeleteTree(FtpClient& ftp, const std::string& folder, std::string& error, int depth = 0) {
    std::vector<FtpEntry> entries;
    if(!ftp.List(folder.c_str(),entries)) { error="Couldn't open "+DashPath(folder)+": "+ftp.Error(); return false; }
    for(size_t i=0;i<entries.size();++i) {
        const FtpEntry& e=entries[i];
        if(e.name.empty() || e.name=="." || e.name=="..") continue;
        std::string path=folder+"/"+e.name;
        if(e.folder ? depth<ProfileTreeDeepest && DeleteTree(ftp,path,error,depth+1) : ftp.Delete(path.c_str())) continue;
        if(!e.folder || error.empty()) error="Couldn't delete "+DashPath(path)+": "+ftp.Error();
        return false;
    }
    if(!ftp.RemoveFolder(folder.c_str())) { error="Couldn't delete "+DashPath(folder)+": "+ftp.Error(); return false; }
    return true;
}

// Whether the profile xuid is signed in on the console, as its Aurora says
// (1 yes, 0 no, -1 it can't tell).
int SignedIn(const Console& c, const Options& o, const std::string& xuid) {
    AuroraWeb aurora(c.address,o.auroraWebPort,o.auroraWebUser,o.auroraWebPassword,5);
    std::string body;
    web::Value v;
    if(!aurora.Get("/profile","/getProfileInfo",body) || !web::Parse(body,v) || v.type!=web::Value::Array) return -1;
    for(size_t i=0;i<v.items.size();++i) {
        const web::Value* x=v.items[i].Get("xuid");
        if(x && x->type==web::Value::String && !_stricmp(x->text.c_str(),xuid.c_str())) {
            const web::Value* in=v.items[i].Get("signedin");
            return in && in->type==web::Value::Number && in->number==0 ? 0 : 1;
        }
    }
    return 0;
}

// The consoles guest profiles go on: the party's, or every console NetDisc360 knows.
std::vector<Console*> ProfileConsoles() {
    Party party=CurrentParty();
    std::vector<std::string> addresses=party.consoles;
    if(addresses.empty()) {
        std::lock_guard<std::mutex> hold(SettingsLock);
        for(size_t i=0;i<Known.size();++i) addresses.push_back(Known[i].address);
    }
    std::vector<Console*> out;
    for(size_t i=0;i<addresses.size();++i) {
        Console* c=ConsoleIfKnown(inet_addr(addresses[i].c_str()));
        if(c && std::find(out.begin(),out.end(),c)==out.end()) out.push_back(c);
    }
    return out;
}
std::string ConsoleWord(const Console& c) { std::string name=NameOf(c); return name.empty() ? std::string(c.text) : name; }
// The consoles at these addresses that NetDisc360 knows.
std::vector<Console*> ConsolesAt(const std::vector<std::string>& addresses) {
    std::vector<Console*> out;
    for(size_t i=0;i<addresses.size();++i) {
        Console* c=ConsoleIfKnown(inet_addr(addresses[i].c_str()));
        if(c && std::find(out.begin(),out.end(),c)==out.end()) out.push_back(c);
    }
    return out;
}
// profiles.ini in NetDisc360's folder on the console (users.h): the profiles
// the records say are on it, and whose they are, so another NetDisc360 that
// meets the console knows them. The lines of profiles this computer has no
// record of (another computer's) stay. False if it couldn't be written.
bool WriteProfileMarks(FtpClient& ftp, const Console& c, const Options& o) {
    std::vector<ProfileMark> marks, had;
    std::vector<unsigned char> data;
    if(ftp.Retrieve((o.xboxFolder+"/"+HelperFolder+"/profiles.ini").c_str(),data,1<<20)) ReadProfileMarks(std::string(data.begin(),data.end()),had);
    {
        std::lock_guard<std::mutex> hold(GuestLock);
        for(size_t i=0;i<had.size();++i) {
            bool ours=false;
            for(size_t k=0;k<GuestProfiles.size() && !ours;++k) ours=GuestProfiles[k].xuid==had[i].xuid;
            if(!ours) marks.push_back(had[i]);
        }
        for(size_t i=0;i<GuestProfiles.size();++i) {
            const GuestProfile& p=GuestProfiles[i];
            if(p.xuid.empty() || std::find(p.on.begin(),p.on.end(),std::string(c.text))==p.on.end()) continue;
            ProfileMark m;
            m.xuid=p.xuid; m.name=p.name; m.gamertag=p.gamertag; m.made=p.made; m.adopted=p.adopted;
            User* u=UserNamed(Users,p.name);
            if(u) m.user=u->id;
            marks.push_back(m);
        }
    }
    std::map<std::string,std::string> files;
    files["profiles.ini"]=ProfileMarksText(marks);
    std::string error;
    if(WriteTree(ftp,o.xboxFolder+"/"+HelperFolder,files,error)) return true;
    Log("%s: couldn't write the list of NetDisc360's profiles there: %s",ConsoleWord(c).c_str(),error.c_str());
    return false;
}
void MarkProfilesOn(const std::vector<Console*>& consoles, const Options& o) {
    for(size_t k=0;k<consoles.size();++k) {
        Busy busy(consoles[k]);
        FtpClient ftp(consoles[k]->address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),10);
        if(ftp.Open()) WriteProfileMarks(ftp,*consoles[k],o);
    }
}

// The NetDisc360.xex plugin's way (plugin/profiles_job.h): the job on the
// hard drive's root, which the plugin takes within seconds and does while
// Aurora stays on screen. 1: made (done says how each went); 0: no plugin
// took it (or it couldn't make any), so the helper does it; -1: taken, but
// no answer came (error says so: the helper isn't started then, as the
// plugin may still be making them). A console whose plugin didn't take a job
// isn't asked again while the server runs.
const char PluginJobPath[]="/Hdd1/NetDisc360.job", PluginDonePath[]="/Hdd1/NetDisc360.done";
const double PluginTakeSeconds=6;
std::mutex PluginJobLock;
std::set<std::string> NoPluginJobs;  // Under PluginJobLock.
int PluginProfileJob(Console& c, const Options& o, const std::string& folder, const std::string& job, const std::vector<std::string>& gamertags, ProfileJobDone& done, std::string& error) {
    if(folder.compare(0,6,"/Hdd1/")) return 0;
    { std::lock_guard<std::mutex> hold(PluginJobLock); if(NoPluginJobs.count(c.text)) return 0; }
    std::string under=folder.substr(5);
    for(size_t i=0;i<under.size();++i) if(under[i]=='/') under[i]='\\';
    auto clean=[&](FtpClient& ftp) {
        for(size_t i=0;i<gamertags.size();++i)
            for(int size=64;size>=32;size-=32) ftp.Delete((folder+"/"+ProfilePictureFile(int(i)+1,size)).c_str());
    };
    {
        Busy busy(&c);
        FtpClient ftp(c.address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),15);
        ftp.Delete(PluginDonePath);
        if(!ftp.Open() || !StoreChecked(ftp,PluginJobPath,ProfileJobText(job,gamertags,true,"folder="+under+"\r\n"))) return 0;
    }
    // Taken: the plugin deletes it first.
    bool taken=false;
    for(double until=Now()+PluginTakeSeconds;!taken && Now()<until;) {
        Sleep(1000);
        Busy busy(&c);
        FtpClient ftp(c.address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),10);
        taken=ftp.Open() && !FtpHas(ftp,"/Hdd1","NetDisc360.job") && ftp.Code()/100==2;
    }
    if(!taken) {
        Busy busy(&c);
        FtpClient ftp(c.address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),10);
        // Gone after all (taken just now): wait for it below.
        if(ftp.Open() && ftp.Delete(PluginJobPath)) {
            { std::lock_guard<std::mutex> hold(PluginJobLock); NoPluginJobs.insert(c.text); }
            XboxLog(c,"its NetDisc360.xex plugin doesn't make guest profiles (an older one, or none), so the helper does.");
            return 0;
        }
    }
    XboxLog(c,"making %s with its NetDisc360.xex plugin (Aurora stays on screen).",Count(int(gamertags.size()),"guest profile","guest profiles").c_str());
    std::string text;
    for(double until=Now()+20+10*double(gamertags.size());;Sleep(1000)) {
        if(Now()>until) { error="The console's NetDisc360.xex plugin took the job but didn't answer: look at its netiso.log."; return -1; }
        Busy busy(&c);
        FtpClient ftp(c.address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),10);
        std::vector<unsigned char> data;
        if(!ftp.Open() || !ftp.Retrieve(PluginDonePath,data,64*1024)) continue;
        text.assign(data.begin(),data.end());
        ftp.Delete(PluginDonePath);
        clean(ftp);
        break;
    }
    if(!ReadProfileDone(text,done) || done.job!=job) { error="The plugin's answer isn't this job's."; return -1; }
    if(done.xuids.empty()) {
        // It couldn't make any (xam may not take it from a plugin): the helper's way, from now on.
        { std::lock_guard<std::mutex> hold(PluginJobLock); NoPluginJobs.insert(c.text); }
        XboxLog(c,"its plugin couldn't make the guest profiles (%s), so the helper does.",done.errors.empty() ? "no reason given" : done.errors.begin()->second.c_str());
        done=ProfileJobDone();
        return 0;
    }
    return 1;
}

// Profiles made by NetDiscParty.xex (party/main.cpp, MakeJob), on a
// console that shows it, when every console does (none can start the helper
// from Aurora): the app sees "makeprofiles <job>" in the party, asks for the
// job and its gamer pictures (PartyJobAsk) and answers with what the helper
// writes (ProfileDone). One job at a time.
struct PartyAppJob { std::string console, job, text, done; std::map<std::string,std::string> pictures; bool answered; PartyAppJob() : answered(false) {} };
std::mutex PartyAppJobLock;
PartyAppJob AppJob;  // Under PartyAppJobLock; console "" when there is none.
std::string PartyAppJobFor(const std::string& console) {
    std::lock_guard<std::mutex> hold(PartyAppJobLock);
    return AppJob.console==console && !AppJob.answered ? AppJob.job : std::string();
}
std::string PartyAppJobAnswer(const std::string& console, int index, const std::string& body) {
    std::lock_guard<std::mutex> hold(PartyAppJobLock);
    if(AppJob.console!=console || AppJob.answered) return std::string();
    if(index==0) return AppJob.text;
    if(index==1) { std::map<std::string,std::string>::const_iterator p=AppJob.pictures.find(body); return p==AppJob.pictures.end() ? std::string() : p->second; }
    if(index==2) { AppJob.done=body; AppJob.answered=true; return "ok\n"; }
    return std::string();
}
bool PartyAppProfileJob(Console& c, const std::vector<std::string>& gamertags, ProfileJobDone& done, std::string& error) {
    std::string job=Number((long long)time(NULL));
    {
        std::lock_guard<std::mutex> hold(PartyAppJobLock);
        AppJob=PartyAppJob();
        AppJob.console=c.text; AppJob.job=job; AppJob.text=ProfileJobText(job,gamertags,true);
        for(size_t i=0;i<gamertags.size();++i)
            for(int size=64;size>=32;size-=32) AppJob.pictures[ProfilePictureFile(int(i)+1,size)]=GamerPicturePng(gamertags[i],size);
    }
    PartyNews();
    XboxLog(c,"making %s with the LAN party app (every console shows it).",Count(int(gamertags.size()),"guest profile","guest profiles").c_str());
    std::string text;
    for(double until=Now()+30+10*double(gamertags.size());;Sleep(500)) {
        {
            std::lock_guard<std::mutex> hold(PartyAppJobLock);
            if(AppJob.answered) { text=AppJob.done; AppJob=PartyAppJob(); break; }
            if(Now()>until) { AppJob=PartyAppJob(); error="The LAN party app didn't answer (an older one can't make profiles: it updates itself back in Aurora)."; break; }
        }
    }
    PartyNews();
    if(!error.empty()) return false;
    if(!ReadProfileDone(text,done) || done.job!=job) { error="The LAN party app's answer isn't this job's."; return false; }
    return true;
}

// Makes gamertags as profiles on console c with the helper's profile job
// (ProfileJob in shared/helper.h): the job in the helper's folder, the helper
// started from Aurora, its answer read once Aurora is back. False, with why
// in error, if the job didn't run; done says how each profile went.
bool RunProfileJob(Console& c, const Options& o, const std::vector<std::string>& gamertags, ProfileJobDone& done, std::string& error) {
    std::string folder=o.xboxFolder+"/"+HelperFolder, device=DevicePath(folder), job=Number((long long)time(NULL));
    if(device.empty()) { error="The helper is started from the hard drive only, and the game folders are in "+o.xboxFolder+"."; return false; }
    std::string why;
    if(!SafeSoon(c,StartWaitMost,why)) { error="Not now: "+why+"."; return false; }
    {
        Busy busy(&c);
        FtpClient ftp(c.address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),15);
        if(!ftp.Open()) { error=std::string("Aurora's FTP server doesn't answer (it does while Aurora is on screen): ")+ftp.Error(); return false; }
        if(!FtpHas(ftp,folder,HelperXex)) { error="NetDisc360's helper isn't on it yet ("+DashPath(folder+"/"+HelperXex)+"): it's written while the console is in Aurora."; return false; }
        ftp.Delete((folder+"/"+ProfileDone).c_str());
        // Each guest's gamer picture (picture.h) beside the job.
        for(size_t i=0;i<gamertags.size();++i)
            for(int size=64;size>=32;size-=32)
                if(!StoreChecked(ftp,folder+"/"+ProfilePictureFile(int(i)+1,size),GamerPicturePng(gamertags[i],size))) { error=std::string("Couldn't write the gamer pictures: ")+ftp.Error(); return false; }
        ftp.Close();
    }
    // The NetDisc360.xex plugin makes them under Aurora if it can (no Aurora restart).
    int plugin=PluginProfileJob(c,o,folder,job,gamertags,done,error);
    if(plugin>0) return true;
    if(plugin<0) return false;
    {
        Busy busy(&c);
        FtpClient ftp(c.address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),15);
        if(!ftp.Open() || !StoreChecked(ftp,folder+"/"+ProfileJob,ProfileJobText(job,gamertags,true))) { error=std::string("Couldn't write the job: ")+ftp.Error(); return false; }
        ftp.Close();
    }
    AuroraWeb aurora(c.address,o.auroraWebPort,o.auroraWebUser,o.auroraWebPassword,8);
    if(!aurora.Launch(device,HelperXex)) { error="Aurora didn't start the helper: "+aurora.Error()+"."; return false; }
    XboxLog(c,"making %s with the helper (started from Aurora).",Count(int(gamertags.size()),"guest profile","guest profiles").c_str());
    // Aurora goes while the helper works (a few seconds a profile), then comes back.
    Sleep(4000);
    std::string text;
    for(double until=Now()+40+10*double(gamertags.size());;Sleep(2000)) {
        if(Now()>until) { error="Aurora didn't come back with the helper's answer: look at the console, and at "+DashPath(folder)+"\\netiso.log."; return false; }
        AuroraWeb back(c.address,o.auroraWebPort,o.auroraWebUser,o.auroraWebPassword,4);
        if(!back.InAurora()) continue;
        Busy busy(&c);
        FtpClient ftp(c.address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),10);
        std::vector<unsigned char> data;
        if(!ftp.Open() || !ftp.Retrieve((folder+"/"+ProfileDone).c_str(),data,64*1024)) {
            if(ftp.Missing() && FtpHas(ftp,folder,ProfileJob)) { error="The helper didn't start: the job is still there."; return false; }
            continue;
        }
        text.assign(data.begin(),data.end());
        ftp.Delete((folder+"/"+ProfileDone).c_str());
        for(size_t i=0;i<gamertags.size();++i)
            for(int size=64;size>=32;size-=32) ftp.Delete((folder+"/"+ProfilePictureFile(int(i)+1,size)).c_str());
        break;
    }
    if(!ReadProfileDone(text,done) || done.job!=job) { error="The helper's answer isn't this job's."; return false; }
    return true;
}

// Keeps a copy of a guest's content folder from console c on this computer
// (Backups/Profiles/<XUID>: the zip and its .ini). False, with why in error.
bool KeepProfileCopy(const GuestProfile& p, const Console& c, const Options& o, const std::map<std::string,std::string>& files, ProfileCopy& copy, std::string& error) {
    std::string root=ProfilesFolder(o);
    if(root.empty()) { error="There's no settings folder to keep it in."; return false; }
    std::string folder=KitPath(root,p.xuid);
    config::MakeFolder(BackupFolder(o)); config::MakeFolder(root); config::MakeFolder(folder);
    std::map<std::string,std::string> zipped;
    long long size=0;
    for(std::map<std::string,std::string>::const_iterator f=files.begin();f!=files.end();++f) { zipped["Content/"+p.xuid+"/"+f->first]=f->second; size+=(long long)f->second.size(); }
    copy=ProfileCopy();
    copy.time=(long long)time(NULL);
    copy.file=ProfileCopyName(copy.time,c.text);
    copy.name=p.name; copy.gamertag=p.gamertag; copy.xuid=p.xuid; copy.console=c.text; copy.size=size; copy.user=UserIdOf(p.name);
    std::map<std::string,std::string>::const_iterator package=files.find("FFFE07D1/00010000/"+p.xuid);
    ProfileStats stats;
    if(package!=files.end() && ReadProfileStats(package->second,stats)) { copy.gamerscore=stats.gamerscore; copy.achievements=stats.achievements; }
    std::string zip=backup::Zip(zipped,copy.time), back;
    if(!config::WriteFile(KitPath(folder,copy.file),zip,false) || !config::ReadFile(KitPath(folder,copy.file),back,zip.size()+1) || back!=zip) {
        error="Couldn't write "+config::FromUtf8(KitPath(folder,copy.file));
        return false;
    }
    std::string ini=copy.file.substr(0,copy.file.size()-4)+".ini";
    if(!config::WriteFile(KitPath(folder,ini),ProfileCopyIni(copy),false)) { error="Couldn't write "+config::FromUtf8(KitPath(folder,ini)); return false; }
    return true;
}
// The files of a kept copy, by their path under the content folder.
bool OpenProfileCopy(const Options& o, const ProfileCopy& copy, std::map<std::string,std::string>& files, std::string& error) {
    std::string data;
    std::map<std::string,std::string> zipped;
    std::string path=KitPath(KitPath(ProfilesFolder(o),copy.xuid),copy.file);
    if(!config::ReadFile(path,data,size_t(2048)<<20) || !backup::UnzipPaths(data,zipped,error)) { if(error.empty()) error="Couldn't read "+config::FromUtf8(path); return false; }
    files.clear();
    std::string prefix="Content/"+copy.xuid+"/";
    for(std::map<std::string,std::string>::const_iterator f=zipped.begin();f!=zipped.end();++f)
        if(!f->first.compare(0,prefix.size(),prefix) && f->first.size()>prefix.size() && f->first.find("..")==std::string::npos) files[f->first.substr(prefix.size())]=f->second;
    if(!files.count("FFFE07D1/00010000/"+copy.xuid)) { error=copy.file+" has no profile in it."; return false; }
    return true;
}

// Makes, puts back or copies the guests' profiles so each of the guests
// named has one on every console (ProfileConsoles, or only those at these
// addresses: the Users page's "Put on"): a kept copy goes back if there is
// one, else one console makes it; then it's copied to the others. Runs on
// its own thread; Work tells the page how it goes.
void MakeGuestProfilesOn(std::vector<std::string> names, std::vector<std::string> only) {
    Options o=Current();
    std::vector<Console*> consoles=only.empty() ? ProfileConsoles() : ConsolesAt(only);
    std::vector<std::string> notes, failed;
    if(consoles.empty()) failed.push_back("No console has used NetDisc360 yet.");
    // Who needs what: the record for each name (made if it's new).
    std::vector<GuestProfile> todo;
    {
        std::lock_guard<std::mutex> hold(GuestLock);
        for(size_t i=0;i<names.size();++i) {
            GuestProfile* p=GuestProfileNamed(GuestProfiles,names[i]);
            if(!p) {
                std::vector<std::string> taken;
                for(size_t k=0;k<GuestProfiles.size();++k) taken.push_back(GuestProfiles[k].gamertag);
                GuestProfile fresh;
                fresh.name=names[i];
                fresh.gamertag=GuestGamertag(names[i],taken);
                GuestProfiles.push_back(fresh);
                p=&GuestProfiles.back();
            }
            todo.push_back(*p);
        }
    }
    Save();
    // Which consoles have each one now (its content folder), and which can be reached.
    std::vector<Console*> open;
    for(size_t k=0;k<consoles.size() && failed.empty();++k) {
        Console& c=*consoles[k];
        WorkStep("Looking at %s",ConsoleWord(c).c_str());
        Busy busy(&c);
        FtpClient ftp(c.address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),10);
        if(!ftp.Open()) { notes.push_back(ConsoleWord(c)+" isn't in Aurora, so its profiles come later"); continue; }
        open.push_back(&c);
        for(size_t i=0;i<todo.size();++i)
            if(!todo[i].xuid.empty()) SetOn(todo[i],c.text,ftp.Enters(ContentFolderFtp(todo[i].xuid).c_str()));
        ftp.Close();
    }
    if(failed.empty() && open.empty()) failed.push_back("None of the consoles is in Aurora (its FTP server answers only then).");
    // Those that are on no console: their best kept copy, or a new one.
    std::vector<size_t> make;
    std::map<size_t,std::map<std::string,std::string> > sources;
    for(size_t i=0;i<todo.size() && failed.empty();++i) {
        GuestProfile& p=todo[i];
        if(!p.xuid.empty() && !p.on.empty()) continue;
        std::vector<ProfileCopy> copies=p.xuid.empty() ? std::vector<ProfileCopy>() : ProfileCopies(KitPath(ProfilesFolder(o),p.xuid));
        const ProfileCopy* best=BestProfileCopy(copies);
        std::string error;
        if(best && OpenProfileCopy(o,*best,sources[i],error)) {
            notes.push_back(p.name+" gets their profile back (from "+best->console+(best->gamerscore>=0 ? ", "+Number(best->gamerscore)+" G" : std::string())+")");
            continue;
        }
        if(best) notes.push_back(p.name+"'s kept profile couldn't be read ("+error+"), so they get a new one");
        sources.erase(i);
        p.xuid.clear(); p.on.clear();
        make.push_back(i);
    }
    // One console makes the new ones: the Gamemaster's if it's in Aurora.
    if(!make.empty() && failed.empty()) {
        Party party=CurrentParty();
        // Not one showing the party wheel: the helper starts from Aurora.
        Console* maker=NULL;
        for(size_t k=0;k<open.size();++k) if(!ShowsWheel(open[k]->text) && (!maker || open[k]->text==party.gamemaster)) maker=open[k];
        // Every one shows the party app: it makes them (the Gamemaster's first).
        bool app=false;
        if(!maker) for(size_t k=0;k<open.size();++k) if(ShowsWheel(open[k]->text) && (!maker || open[k]->text==party.gamemaster)) { maker=open[k]; app=true; }
        if(!maker) failed.push_back("No console can make the new profiles now: one in Aurora or showing the LAN party app can");
        for(size_t from=0;maker && from<make.size();from+=ProfileJobMost) {
            std::vector<std::string> tags;
            for(size_t n=from;n<make.size() && n<from+ProfileJobMost;++n) tags.push_back(todo[make[n]].gamertag);
            WorkStep(app ? "%s makes %s in the LAN party app" : "%s makes %s (Aurora is back in a moment)",ConsoleWord(*maker).c_str(),Count(int(tags.size()),"profile","profiles").c_str());
            ProfileJobDone done;
            std::string error;
            if(!(app ? PartyAppProfileJob(*maker,tags,done,error) : RunProfileJob(*maker,o,tags,done,error))) { failed.push_back(ConsoleWord(*maker)+" couldn't make them: "+error); break; }
            for(size_t n=from;n<make.size() && n<from+ProfileJobMost;++n) {
                GuestProfile& p=todo[make[n]];
                int slot=int(n-from)+1;
                if(!done.xuids.count(slot)) { failed.push_back(p.name+": "+(done.errors.count(slot) ? done.errors[slot] : std::string("the helper said nothing about it."))); continue; }
                p.xuid=done.xuids[slot]; p.made=(long long)time(NULL); p.madeOn=maker->text; p.on.assign(1,maker->text);
                XboxLog(*maker,"made the guest profile %s (%s) for %s.",p.gamertag.c_str(),p.xuid.c_str(),p.name.c_str());
                // Without its picture it's a profile all the same.
                if(done.pictureErrors.count(slot)) XboxLog(*maker,"couldn't give %s its gamer picture: %s",p.gamertag.c_str(),done.pictureErrors[slot].c_str());
                KeepGuestProfile(p);
            }
        }
    }
    // Then each goes on the consoles that don't have it.
    for(size_t i=0;i<todo.size() && failed.empty();++i) {
        GuestProfile& p=todo[i];
        if(p.xuid.empty()) continue;
        std::map<std::string,std::string>& files=sources[i];
        for(size_t k=0;k<open.size();++k) {
            Console& c=*open[k];
            if(std::find(p.on.begin(),p.on.end(),std::string(c.text))!=p.on.end()) continue;
            std::string error;
            if(files.empty()) {
                // Read from a console that has it, the one that made it first;
                // those looked at just now first, then the others it's on
                // (the Users page puts it on one console from another).
                std::vector<Console*> from, more=ConsolesAt(p.on);
                for(size_t s=0;s<open.size();++s) if(std::find(p.on.begin(),p.on.end(),std::string(open[s]->text))!=p.on.end()) from.insert(open[s]->text==p.madeOn ? from.begin() : from.end(),open[s]);
                for(size_t s=0;s<more.size();++s) if(std::find(open.begin(),open.end(),more[s])==open.end()) from.push_back(more[s]);
                if(from.empty()) { notes.push_back(p.name+" is on no console in Aurora now, so it's copied later"); break; }
                std::string tried;
                for(size_t s=0;s<from.size() && files.empty();++s) {
                    WorkStep("Reading %s's profile from %s",p.name.c_str(),ConsoleWord(*from[s]).c_str());
                    Busy busy(from[s]);
                    FtpClient ftp(from[s]->address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),20);
                    error.clear();
                    if(ftp.Open() && ReadTree(ftp,ContentFolderFtp(p.xuid),std::string(),files,error)) break;
                    tried+=(tried.empty() ? "" : "; ")+ConsoleWord(*from[s])+": "+(error.empty() ? std::string(ftp.Error()) : error);
                    files.clear();
                }
                if(files.empty()) { failed.push_back("Couldn't read "+p.name+"'s profile on "+tried); break; }
            }
            WorkStep("Copying %s's profile to %s",p.name.c_str(),ConsoleWord(c).c_str());
            Busy busy(&c);
            FtpClient ftp(c.address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),20);
            if(!ftp.Open() || !WriteTree(ftp,ContentFolderFtp(p.xuid),files,error)) {
                failed.push_back("Couldn't copy "+p.name+"'s profile to "+ConsoleWord(c)+": "+(error.empty() ? std::string(ftp.Error()) : error));
                continue;
            }
            SetOn(p,c.text,true);
            XboxLog(c,"has the guest profile %s (%s) for %s now.",p.gamertag.c_str(),p.xuid.c_str(),p.name.c_str());
        }
        if(!p.made) p.made=(long long)time(NULL);
        KeepGuestProfile(p);
    }
    MarkProfilesOn(open,o);
    int ready=0;
    for(size_t i=0;i<todo.size();++i) if(!todo[i].xuid.empty() && todo[i].on.size()>=open.size() && !open.empty()) ++ready;
    std::string result=Count(ready,"guest has a profile","guests have a profile")+" on "+(open.size()==consoles.size() ? "every console" : Count(int(open.size()),"console","consoles"));
    for(size_t i=0;i<notes.size();++i) result+=". "+notes[i];
    Log("Guest profiles: %s.%s%s",result.c_str(),failed.empty() ? "" : " ",failed.empty() ? "" : failed[0].c_str());
    std::lock_guard<std::mutex> hold(GuestLock);
    Work.running=false; Work.step.clear(); Work.at=(long long)time(NULL);
    Work.result=result+".";
    Work.error.clear();
    for(size_t i=0;i<failed.size();++i) Work.error+=(i ? " " : "")+failed[i]+(failed[i].empty() || failed[i][failed[i].size()-1]=='.' ? "" : ".");
}

void MakeGuestProfiles(std::vector<std::string> names) { MakeGuestProfilesOn(names,std::vector<std::string>()); }

// Keeps a copy of each named guest's content folder from every console that
// has it, then removes it there: never one the server didn't make (its
// package must have the record's gamertag), nor one signed in now.
void RemoveGuestProfilesFrom(std::vector<std::string> names, std::vector<std::string> only) {
    Options o=Current();
    std::vector<Console*> consoles=only.empty() ? ProfileConsoles() : ConsolesAt(only), touched;
    std::vector<std::string> failed, notes;
    int removed=0, kept=0;
    for(size_t i=0;i<names.size();++i) {
        GuestProfile p;
        {
            std::lock_guard<std::mutex> hold(GuestLock);
            GuestProfile* had=GuestProfileNamed(GuestProfiles,names[i]);
            if(!had || had->xuid.empty()) continue;
            p=*had;
        }
        // Its consoles: the party's, and any other it was put on (or only those asked for).
        std::vector<Console*> where=consoles;
        for(size_t k=0;k<p.on.size() && only.empty();++k) {
            Console* c=ConsoleIfKnown(inet_addr(p.on[k].c_str()));
            if(c && std::find(where.begin(),where.end(),c)==where.end()) where.push_back(c);
        }
        for(size_t k=0;k<where.size();++k) {
            Console& c=*where[k];
            WorkStep("Keeping %s's profile from %s",p.name.c_str(),ConsoleWord(c).c_str());
            Busy busy(&c);
            FtpClient ftp(c.address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),20);
            if(!ftp.Open()) {
                if(std::find(p.on.begin(),p.on.end(),std::string(c.text))!=p.on.end()) notes.push_back(ConsoleWord(c)+" isn't in Aurora, so "+p.name+"'s profile stays there for now");
                continue;
            }
            std::string folder=ContentFolderFtp(p.xuid), error;
            if(!ftp.Enters(folder.c_str())) { SetOn(p,c.text,false); continue; }
            if(p.adopted && p.madeOn==c.text) { notes.push_back(p.name+"'s own profile stays on "+ConsoleWord(c)+", where they made it"); continue; }
            std::map<std::string,std::string> files;
            if(!ReadTree(ftp,folder,std::string(),files,error)) { failed.push_back(p.name+" on "+ConsoleWord(c)+": "+error); continue; }
            std::map<std::string,std::string>::const_iterator package=files.find("FFFE07D1/00010000/"+p.xuid);
            ProfileStats stats;
            // The package's name is its gamertag, or (as xam names one it
            // makes, seen on the consoles 2026-11-01) its own XUID.
            if(package==files.end() || !ReadProfileStats(package->second,stats) || (_stricmp(stats.gamertag.c_str(),p.gamertag.c_str()) && _stricmp(stats.gamertag.c_str(),p.xuid.c_str()))) {
                failed.push_back(p.name+" on "+ConsoleWord(c)+": "+DashPath(folder)+" isn't the profile NetDisc360 made ("+(package==files.end() ? std::string("no profile in it") : "it's called "+stats.gamertag)+"), so it's left alone");
                continue;
            }
            if(SignedIn(c,o,p.xuid)==1) { failed.push_back(p.name+" is signed in on "+ConsoleWord(c)+": sign them out first"); continue; }
            ProfileCopy copy;
            if(!KeepProfileCopy(p,c,o,files,copy,error)) { failed.push_back(p.name+" on "+ConsoleWord(c)+": "+error+", so it's not removed"); continue; }
            ++kept;
            WorkStep("Removing %s's profile from %s",p.name.c_str(),ConsoleWord(c).c_str());
            if(!DeleteTree(ftp,folder,error)) { failed.push_back(p.name+" on "+ConsoleWord(c)+": "+error+" (a copy is kept)"); continue; }
            ++removed;
            SetOn(p,c.text,false);
            if(std::find(touched.begin(),touched.end(),&c)==touched.end()) touched.push_back(&c);
            XboxLog(c,"guest profile %s (%s) for %s kept as %s and removed.",p.gamertag.c_str(),p.xuid.c_str(),p.name.c_str(),copy.file.c_str());
        }
        KeepGuestProfile(p);
    }
    MarkProfilesOn(touched,o);
    std::string result=Count(removed,"guest profile removed","guest profiles removed")+", "+Count(kept,"copy","copies")+" kept in "+config::FromUtf8(ProfilesFolder(o));
    for(size_t i=0;i<notes.size();++i) result+=". "+notes[i];
    Log("Guest profiles: %s.",result.c_str());
    if(removed) Event(EventInfo,"%s",Count(removed,"guest profile removed from the consoles","guest profiles removed from the consoles").c_str());
    std::lock_guard<std::mutex> hold(GuestLock);
    Work.running=false; Work.step.clear(); Work.at=(long long)time(NULL);
    Work.result=result+".";
    Work.error.clear();
    for(size_t i=0;i<failed.size();++i) Work.error+=(i ? " " : "")+failed[i]+".";
}

void RemoveGuestProfiles(std::vector<std::string> names) { RemoveGuestProfilesFrom(names,std::vector<std::string>()); }

// Keeps a copy of each named user's content folder (their profile and
// saves) from each console that has it, or only those at these addresses,
// and leaves it there (the Users page's "Back up").
void BackUpGuestProfiles(std::vector<std::string> names, std::vector<std::string> only) {
    Options o=Current();
    std::vector<std::string> failed, notes;
    std::vector<Console*> touched;
    int kept=0;
    for(size_t i=0;i<names.size();++i) {
        GuestProfile p;
        {
            std::lock_guard<std::mutex> hold(GuestLock);
            GuestProfile* had=GuestProfileNamed(GuestProfiles,names[i]);
            if(!had || had->xuid.empty()) continue;
            p=*had;
        }
        std::vector<Console*> where=ConsolesAt(only.empty() ? p.on : only);
        if(where.empty()) { notes.push_back(p.name+"'s profile is on no console NetDisc360 knows now"); continue; }
        for(size_t k=0;k<where.size();++k) {
            Console& c=*where[k];
            WorkStep("Backing up %s's profile from %s",p.name.c_str(),ConsoleWord(c).c_str());
            Busy busy(&c);
            FtpClient ftp(c.address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),20);
            if(!ftp.Open()) { notes.push_back(ConsoleWord(c)+" isn't in Aurora, so "+p.name+"'s profile there isn't backed up"); continue; }
            std::string folder=ContentFolderFtp(p.xuid), error;
            if(!ftp.Enters(folder.c_str())) {
                if(std::find(p.on.begin(),p.on.end(),std::string(c.text))!=p.on.end()) notes.push_back(p.name+"'s profile isn't on "+ConsoleWord(c)+" any more");
                SetOn(p,c.text,false);
                continue;
            }
            SetOn(p,c.text,true);
            if(std::find(touched.begin(),touched.end(),&c)==touched.end()) touched.push_back(&c);
            std::map<std::string,std::string> files;
            if(!ReadTree(ftp,folder,std::string(),files,error)) { failed.push_back(p.name+" on "+ConsoleWord(c)+": "+error); continue; }
            ProfileCopy copy;
            if(!KeepProfileCopy(p,c,o,files,copy,error)) { failed.push_back(p.name+" on "+ConsoleWord(c)+": "+error); continue; }
            ++kept;
            XboxLog(c,"%s's profile %s (%s) and saves kept as %s.",p.name.c_str(),p.gamertag.c_str(),p.xuid.c_str(),copy.file.c_str());
        }
        KeepGuestProfile(p);
    }
    MarkProfilesOn(touched,o);
    std::string result=Count(kept,"copy","copies")+" kept in "+config::FromUtf8(ProfilesFolder(o));
    for(size_t i=0;i<notes.size();++i) result+=". "+notes[i];
    Log("Profiles backed up: %s.",result.c_str());
    std::lock_guard<std::mutex> hold(GuestLock);
    Work.running=false; Work.step.clear(); Work.at=(long long)time(NULL);
    Work.result=result+".";
    Work.error.clear();
    for(size_t i=0;i<failed.size();++i) Work.error+=(i ? " " : "")+failed[i]+".";
}

// The seated guests' profiles to their consoles, after a spin or a swap: on
// each console, each guest seated there whose profile it hasn't got gets a
// copy, from a console that has it (the one that made it first) or else the
// copy kept on this computer, so they can sign in at once. A console that
// shows the party wheel takes it through the wheel app's FTP server (party/
// ftp.h); one opening the wheel is waited for (Aurora is closing then);
// otherwise Aurora's FTP server, if Aurora is on screen. Guests who have no
// profile yet are noted: the LAN party page makes them (MakeGuestProfiles).
// SeatPushes tells the wheel app and the page how it goes.
enum { SeatWheelWaitSeconds = 45, SeatWorkWaitSeconds = 600 };
std::mutex SeatPushRunLock;
bool SeatPushRunning=false, SeatPushAgain=false;  // Under SeatPushRunLock.

std::string OneLine(std::string text) {
    for(size_t i=0;i<text.size();++i) if(text[i]=='\t' || text[i]=='\r' || text[i]=='\n') text[i]=' ';
    return text;
}
void SeatPushSet(const std::string& address, const std::string& state, int done, int total, const std::string& now, const std::string& note) {
    {
        std::lock_guard<std::mutex> hold(GuestLock);
        SeatProfiles& s=SeatPushes[address];
        s.state=state; s.done=done; s.total=total; s.now=now; s.note=OneLine(note);
    }
    PartyNews();
}
// One guest's content folder: from the other consoles that have it (the
// one that made it first), else the best copy kept here.
// One guest's content folder from the console at address.
bool ProfileFilesOn(const GuestProfile& p, const std::string& address, const Options& o, std::map<std::string,std::string>& files, std::string& error) {
    files.clear();
    Console* c=ConsoleIfKnown(inet_addr(address.c_str()));
    if(!c) { error=address+" hasn't used NetDisc360 since it started"; return false; }
    Busy busy(c);
    FtpClient ftp(c->address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),20);
    std::string why;
    if(ftp.Open() && ReadTree(ftp,ContentFolderFtp(p.xuid),std::string(),files,why)) return true;
    error=why.empty() ? std::string(ftp.Error()) : why;
    XboxLog(*c,"couldn't give %s's profile to copy: %s",p.name.c_str(),error.c_str());
    return false;
}
// from: the console it came from ("" for the copy on this computer).
bool SeatProfileFiles(const GuestProfile& p, const std::string& skip, const Options& o, std::map<std::string,std::string>& files, std::string& error, std::string* came = NULL) {
    // The newest progress first (where they played last), then the one that made it.
    std::vector<std::string> from;
    for(size_t i=0;i<p.on.size();++i) if(p.on[i]!=skip && p.on[i]!=p.playedOn) from.insert(p.on[i]==p.madeOn ? from.begin() : from.end(),p.on[i]);
    if(!p.playedOn.empty() && p.playedOn!=skip && std::find(p.on.begin(),p.on.end(),p.playedOn)!=p.on.end()) from.insert(from.begin(),p.playedOn);
    for(size_t i=0;i<from.size();++i) {
        std::string why;
        if(ProfileFilesOn(p,from[i],o,files,why)) { if(came) *came=from[i]; return true; }
    }
    if(came) came->clear();
    std::vector<ProfileCopy> copies=ProfileCopies(KitPath(ProfilesFolder(o),p.xuid));
    const ProfileCopy* best=BestProfileCopy(copies);
    if(best && OpenProfileCopy(o,*best,files,error)) return true;
    if(error.empty()) error="no console with it answers, and there's no copy of it on this computer";
    return false;
}
// Whether guest g is signed in on console c: on its party screen (seated
// there), or as its Aurora says. Their profile there is never written then.
bool SignedInHere(const GuestProfile& g, const std::string& address, const Console& c, const Options& o) {
    Party p=CurrentParty();
    {
        std::lock_guard<std::mutex> hold(PartyLock);
        std::vector<const PartySignInRecord*> in=SignedIn(p,address);
        for(size_t i=0;i<in.size();++i) if(in[i] && SameName(in[i]->name,g.name)) return true;
    }
    return SignedIn(c,o,g.xuid)==1;
}
void PushSeatsOnce() {
    Options o=Current();
    Party p=CurrentParty();
    Seats seats=p.drawn ? p.seats : Seats();
    {
        // Consoles out of the party, with no one seated, have nothing coming.
        std::lock_guard<std::mutex> hold(GuestLock);
        for(std::map<std::string,SeatProfiles>::iterator i=SeatPushes.begin();i!=SeatPushes.end();) {
            bool seated=std::find(p.consoles.begin(),p.consoles.end(),i->first)!=p.consoles.end();
            for(size_t k=0;k<seats.size();++k) if(seats[k].first==i->first && !seats[k].second.empty()) seated=true;
            if(seated) ++i; else SeatPushes.erase(i++);
        }
    }
    // What each console needs: its guests' records; those without a profile are noted.
    struct ConsoleTodo { std::string address; std::vector<GuestProfile> guests; std::string none; };
    std::vector<ConsoleTodo> todo;
    std::vector<GuestProfile> records=GuestProfilesNow();
    bool any=false;
    for(size_t i=0;i<records.size();++i) if(!records[i].xuid.empty()) any=true;
    // A party without guest profiles: nothing to say.
    if(!any) { std::lock_guard<std::mutex> hold(GuestLock); SeatPushes.clear(); return; }
    // Every party console gets every profile of the guests here (Marcus: so
    // anyone can go to another console later), its seated guests' first.
    std::vector<std::string> consoles=p.consoles, here=GuestsHere(p);
    for(size_t k=0;k<seats.size();++k)
        if(!seats[k].second.empty() && std::find(consoles.begin(),consoles.end(),seats[k].first)==consoles.end()) consoles.push_back(seats[k].first);
    for(size_t k=0;k<consoles.size();++k) {
        Team seated;
        for(size_t s=0;s<seats.size();++s) if(seats[s].first==consoles[k]) seated=seats[s].second;
        ConsoleTodo t;
        t.address=consoles[k];
        std::vector<std::string> none;
        for(size_t i=0;i<seated.size();++i) {
            GuestProfile* r=GuestProfileNamed(records,seated[i]);
            if(r && !r->xuid.empty()) t.guests.push_back(*r); else none.push_back(seated[i]);
        }
        for(size_t i=0;i<here.size();++i) {
            if(std::find(seated.begin(),seated.end(),here[i])!=seated.end()) continue;
            GuestProfile* r=GuestProfileNamed(records,here[i]);
            if(r && !r->xuid.empty()) t.guests.push_back(*r);
        }
        for(size_t i=0;i<none.size();++i) t.none+=(i ? (i+1==none.size() ? " and " : ", ") : "")+none[i];
        if(!none.empty()) t.none+=(none.size()==1 ? " has" : " have")+std::string(" no profile yet (the LAN party page makes them)");
        SeatPushSet(t.address,t.guests.empty() ? "ready" : "copying",0,int(t.guests.size()),"",t.none);
        if(!t.guests.empty()) todo.push_back(t);
    }
    if(todo.empty()) return;
    // One piece of profile work at a time (Work): after the page's, if one runs.
    for(double until=Now()+SeatWorkWaitSeconds;;Sleep(1000)) {
        {
            std::lock_guard<std::mutex> hold(GuestLock);
            if(!Work.running) { Work=ProfileWork(); Work.running=true; Work.what="copy to the seats"; Work.at=(long long)time(NULL); break; }
        }
        if(Now()>until) {
            for(size_t k=0;k<todo.size();++k) SeatPushSet(todo[k].address,"failed",0,int(todo[k].guests.size()),"","The guest profiles were busy for too long; swap or spin again.");
            return;
        }
    }
    int copied=0, had=0, updated=0;
    std::vector<std::string> failed;
    for(size_t k=0;k<todo.size();++k) {
        ConsoleTodo& t=todo[k];
        Console* c=ConsoleIfKnown(inet_addr(t.address.c_str()));
        int total=int(t.guests.size()), done=0;
        if(!c) { SeatPushSet(t.address,"failed",0,total,"","That console hasn't used NetDisc360 since it started."); continue; }
        std::string word=ConsoleWord(*c);
        // The wheel app (waited for when it was just opened); else Aurora, if it's on screen.
        bool wheel=false;
        for(double until=Now()+SeatWheelWaitSeconds;!(wheel=ShowsWheel(t.address)) && Now()<until && WheelOpening(t.address,SeatWheelWaitSeconds+15);) Sleep(500);
        if(!wheel) {
            AuroraWeb aurora(c->address,o.auroraWebPort,o.auroraWebUser,o.auroraWebPassword,4);
            if(!aurora.InAurora()) {
                SeatPushSet(t.address,"failed",0,total,"",word+" shows neither the wheel nor Aurora, so its profiles come with the next spin or swap.");
                failed.push_back(word+": not reachable");
                continue;
            }
        }
        std::string note=t.none, problem;
        for(size_t i=0;i<t.guests.size();++i) {
            GuestProfile& g=t.guests[i];
            SeatPushSet(t.address,"copying",done,total,g.name,note);
            {
                std::lock_guard<std::mutex> hold(GuestLock);
                Work.step="Copying "+g.name+"'s profile to "+word;
            }
            std::string error;
            Busy busy(c);
            FtpClient ftp(c->address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),20);
            if(!ftp.Open()) { problem=std::string("its FTP server doesn't answer: ")+ftp.Error(); break; }
            if(ftp.Enters(ContentFolderFtp(g.xuid).c_str())) {
                // There already (or still from an earlier party).
                ++done;
                if(std::find(g.on.begin(),g.on.end(),t.address)==g.on.end()) MarkProfileOn(g,t.address,false);
                // Progress follows the player: older than where they played
                // last, it's brought up to date (not while they're signed in here).
                std::string newer=NewerOn(g.name,t.address);
                if(newer.empty() || SignedInHere(g,t.address,*c,o)) { ++had; continue; }
                ftp.Close();
                std::map<std::string,std::string> files;
                Console* from=ConsoleIfKnown(inet_addr(newer.c_str()));
                std::string fromWord=from ? ConsoleWord(*from) : newer;
                SeatPushSet(t.address,"copying",done-1,total,g.name,note);
                if(!ProfileFilesOn(g,newer,o,files,error)) {
                    ++had;
                    XboxLog(*c,"keeps its older copy of %s's profile for now: %s, with their newest progress, gave none (%s).",g.name.c_str(),fromWord.c_str(),error.c_str());
                    continue;
                }
                if(!ftp.Open() || !WriteTree(ftp,ContentFolderFtp(g.xuid),files,error)) {
                    problem="Couldn't bring "+g.name+"'s profile up to date: "+(error.empty() ? std::string(ftp.Error()) : error);
                    --done;
                    continue;
                }
                ftp.Close();
                ++updated;
                MarkProfileOn(g,t.address);
                XboxLog(*c,"has %s's newest progress now (from %s).",g.name.c_str(),fromWord.c_str());
                continue;
            }
            ftp.Close();
            std::map<std::string,std::string> files;
            std::string came;
            if(!SeatProfileFiles(g,t.address,o,files,error,&came)) { problem=g.name+"'s profile: "+error; continue; }
            if(!ftp.Open() || !WriteTree(ftp,ContentFolderFtp(g.xuid),files,error)) {
                problem="Couldn't copy "+g.name+"'s profile: "+(error.empty() ? std::string(ftp.Error()) : error);
                continue;
            }
            ftp.Close();
            ++done; ++copied;
            MarkProfileOn(g,t.address,came.empty() || g.playedOn.empty() || came==g.playedOn);
            XboxLog(*c,"has %s's guest profile (%s) now%s.",g.name.c_str(),g.gamertag.c_str(),wheel ? ", through the party wheel" : "");
        }
        if(!problem.empty()) {
            XboxLog(*c,"didn't get every guest's profile: %s",problem.c_str());
            failed.push_back(word+": "+problem);
            note=problem+(note.empty() ? "" : ". "+note);
        }
        SeatPushSet(t.address,done==total ? "ready" : "failed",done,total,"",note);
    }
    std::string result=Count(copied,"profile copied","profiles copied")+" to the party's consoles ("+Number(had)+" there already"+(updated ? ", "+Number(updated)+" brought up to date" : std::string())+")";
    Log("Guest profiles for the party's consoles: %s.%s%s",result.c_str(),failed.empty() ? "" : " ",failed.empty() ? "" : failed[0].c_str());
    std::lock_guard<std::mutex> hold(GuestLock);
    Work.running=false; Work.step.clear(); Work.at=(long long)time(NULL);
    Work.result=result+".";
    Work.error.clear();
    for(size_t i=0;i<failed.size();++i) Work.error+=(i ? " " : "")+failed[i]+".";
}
// Runs PushSeatsOnce on a thread of its own; asked again meanwhile, once more after.
void PushSeatProfilesSoon() {
    {
        std::lock_guard<std::mutex> hold(SeatPushRunLock);
        SeatPushAgain=true;
        if(SeatPushRunning) return;
        SeatPushRunning=true;
    }
    std::thread([]() {
        for(;;) {
            {
                std::lock_guard<std::mutex> hold(SeatPushRunLock);
                if(!SeatPushAgain) { SeatPushRunning=false; return; }
                SeatPushAgain=false;
            }
            PushSeatsOnce();
        }
    }).detach();
}

// New names on the guest list get their profile without being asked
// (Marcus: as soon as the name is written): MakeGuestProfiles for every
// name that has none, once the list has been still for a few seconds, and
// then each seated guest's profile goes to their console. Tried again
// a minute later while a name has none (no console in Aurora, say), at most
// AutoMakeTries times for the same names.
enum { AutoMakeTries = 3, AutoMakeRetrySeconds = 60 };
std::mutex AutoMakeLock;
double AutoMakeAt=0;          // Under AutoMakeLock.
bool AutoMakeRunning=false;
std::vector<std::string> GuestsWithoutProfile() {
    Party p=CurrentParty();
    std::vector<GuestProfile> records=GuestProfilesNow();
    std::vector<std::string> names;
    // Only those here: the list keeps every guest from party to party
    // (Marcus: they don't join every time), and one who isn't here needs none.
    for(size_t i=0;i<p.guests.size();++i) {
        if(!p.guests[i].here) continue;
        GuestProfile* r=GuestProfileNamed(records,p.guests[i].name);
        if(!r || r->xuid.empty()) names.push_back(p.guests[i].name);
    }
    return names;
}
void MakeProfilesSoon(double seconds) {
    if(!Current().autoProfiles) return;
    std::lock_guard<std::mutex> hold(AutoMakeLock);
    AutoMakeAt=Now()+seconds;
    if(AutoMakeRunning) return;
    AutoMakeRunning=true;
    std::thread([]() {
        std::vector<std::string> tried;
        int tries=0;
        for(;;Sleep(1000)) {
            {
                std::lock_guard<std::mutex> hold(AutoMakeLock);
                if(Now()<AutoMakeAt) continue;
            }
            std::vector<std::string> names=GuestsWithoutProfile();
            if(names!=tried) { tried=names; tries=0; }
            if(names.empty() || tries>=AutoMakeTries) { std::lock_guard<std::mutex> hold(AutoMakeLock); AutoMakeRunning=false; return; }
            bool start=false;
            {
                std::lock_guard<std::mutex> hold(GuestLock);
                if(!Work.running) { Work=ProfileWork(); Work.running=true; Work.what="make"; Work.at=(long long)time(NULL); start=true; }
            }
            if(start) {
                ++tries;
                Log("Guest profiles: making %s for the names on the guest list.",Count(int(names.size()),"profile","profiles").c_str());
                MakeGuestProfiles(names);
                // Then to the seats, now that there are more to copy.
                PushSeatProfilesSoon();
            }
            std::lock_guard<std::mutex> hold(AutoMakeLock);
            AutoMakeAt=Now()+(start ? AutoMakeRetrySeconds : 5);
        }
    }).detach();
}

// The guests' stats -----------------------------------------------------------

std::vector<std::string> GuestsAt(const std::string& address) {
    Party p=CurrentParty();
    std::vector<std::string> out;
    if(!p.active) return out;
    {
        std::lock_guard<std::mutex> hold(PartyLock);
        std::map<std::string,std::vector<PartySignInRecord> >::const_iterator in=SignIns.find(address);
        if(in!=SignIns.end()) for(size_t i=0;i<in->second.size();++i) out.push_back(in->second[i].name);
    }
    if(out.empty() && p.drawn)
        for(size_t i=0;i<p.seats.size();++i) if(p.seats[i].first==address) out=p.seats[i].second;
    return out;
}

void GuestsPlayed(const std::string& address, double seconds, const std::string& game) {
    std::vector<std::string> names=GuestsAt(address);
    if(names.empty() || seconds<30) return;  // Less is a game that didn't start, not a game played.
    {
        std::lock_guard<std::mutex> hold(StatsLock);
        for(size_t i=0;i<names.size();++i) {
            GuestStats& g=StatsOf(names[i]);
            g.playSeconds+=(long long)seconds; ++g.plays; g.lastGame=game;
        }
    }
    Save();
}

// Reads the gamerscore and achievements of the named guests' profiles on
// the console at address (over Aurora's FTP server, or the party wheel's),
// for those whose profile is on it. The most a console has is kept.
// Returns how many were read.
int ReadGuestStats(const std::string& address, const std::vector<std::string>& names) {
    Console* c=ConsoleIfKnown(inet_addr(address.c_str()));
    if(!c || names.empty()) return 0;
    Options o=Current();
    Party p=CurrentParty();
    std::vector<GuestProfile> records=GuestProfilesNow();
    std::vector<std::pair<std::string,std::string> > wanted;  // Name, XUID.
    for(size_t i=0;i<names.size();++i) {
        GuestProfile* r=GuestProfileNamed(records,names[i]);
        if(r && !r->xuid.empty() && std::find(r->on.begin(),r->on.end(),address)!=r->on.end()) wanted.push_back(std::make_pair(names[i],r->xuid));
    }
    if(wanted.empty()) return 0;
    Busy busy(c);
    FtpClient ftp(c->address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),15);
    if(!ftp.Open()) return 0;
    int read=0;
    std::vector<std::pair<std::string,int> > earned;  // For the live feed.
    for(size_t i=0;i<wanted.size();++i) {
        std::vector<unsigned char> data;
        if(!ftp.Retrieve(ProfilePackageFtp(wanted[i].second).c_str(),data,size_t(64)<<20)) continue;
        ProfileStats stats;
        if(!ReadProfileStats(std::string(data.begin(),data.end()),stats) || stats.gamerscore<0) continue;
        bool here=false;
        for(size_t k=0;k<p.guests.size();++k) if(p.guests[k].here && SameName(p.guests[k].name,wanted[i].first)) here=true;
        std::lock_guard<std::mutex> hold(StatsLock);
        GuestStats& g=StatsOf(wanted[i].first);
        if(p.active && here && g.partyStart<0) g.partyStart=g.gamerscore>=0 ? g.gamerscore : stats.gamerscore;
        if(p.active && here && g.gamerscore>=0 && stats.gamerscore>g.gamerscore) earned.push_back(std::make_pair(wanted[i].first,stats.gamerscore-g.gamerscore));
        g.gamerscore=std::max(g.gamerscore,stats.gamerscore);
        g.achievements=std::max(g.achievements,stats.achievements);
        g.readAt=(long long)time(NULL);
        ++read;
    }
    if(read) Save();
    for(size_t i=0;i<earned.size();++i) PartyFeed("score","%s earned %d G",earned[i].first.c_str(),earned[i].second);
    return read;
}

void GuestStatsSoon(const std::string& address, double seconds) {
    std::thread([address,seconds]() {
        Sleep(DWORD(seconds*1000));
        ReadGuestStats(address,GuestsAt(address));
    }).detach();
}

// POST /api/party/stats: reads every guest's gamerscore again, from each
// console their profile is on (those that answer), in the background.
void RefreshGuestStats(web::Reply& out) {
    if(StatsReading.exchange(true)) { out.body="{\"ok\":true,\"already\":true}"; return; }
    std::thread([]() {
        Party p=CurrentParty();
        std::vector<GuestProfile> records=GuestProfilesNow();
        std::map<std::string,std::vector<std::string> > byConsole;
        for(size_t i=0;i<p.guests.size();++i) {
            GuestProfile* r=GuestProfileNamed(records,p.guests[i].name);
            if(r) for(size_t k=0;k<r->on.size();++k) byConsole[r->on[k]].push_back(p.guests[i].name);
        }
        std::vector<std::thread> reads;
        for(std::map<std::string,std::vector<std::string> >::const_iterator i=byConsole.begin();i!=byConsole.end();++i) {
            std::string address=i->first; std::vector<std::string> names=i->second;
            reads.push_back(std::thread([address,names]() { ReadGuestStats(address,names); }));
        }
        for(size_t i=0;i<reads.size();++i) reads[i].join();
        StatsReading=false;
        PartyNews();
    }).detach();
    out.body="{\"ok\":true}";
}

// GET /api/party/profiles: the guests' profiles (every guest on the list,
// and any other the server made that is still on a console or kept), each
// with its consoles and kept copies, and the work under way.
void GuestProfilesJson(web::Reply& out) {
    Options o=Current();
    Party party=CurrentParty();
    std::vector<GuestProfile> records=GuestProfilesNow();
    ProfileWork work;
    { std::lock_guard<std::mutex> hold(GuestLock); work=Work; }
    std::vector<std::string> taken;
    for(size_t i=0;i<records.size();++i) taken.push_back(records[i].gamertag);
    std::vector<GuestProfile> list;
    for(size_t i=0;i<party.guests.size();++i) {
        GuestProfile* had=GuestProfileNamed(records,party.guests[i].name);
        GuestProfile p;
        if(had) p=*had;
        else { p.name=party.guests[i].name; p.gamertag=GuestGamertag(p.name,taken); taken.push_back(p.gamertag); }
        list.push_back(p);
    }
    for(size_t i=0;i<records.size();++i) if(GuestNamed(party,records[i].name).empty() && !records[i].xuid.empty()) list.push_back(records[i]);
    web::Json j;
    j.Open('{').Key("work").Open('{').Set("running",work.running).Set("what",work.what).Set("step",work.step).Set("result",work.result)
     .Set("error",work.error).Set("at",work.at).Close('}').Set("folder",config::FromUtf8(ProfilesFolder(o))).Key("profiles").Open('[');
    for(size_t i=0;i<list.size();++i) {
        const GuestProfile& p=list[i];
        std::vector<ProfileCopy> copies=p.xuid.empty() ? std::vector<ProfileCopy>() : ProfileCopies(KitPath(ProfilesFolder(o),p.xuid));
        const ProfileCopy* best=BestProfileCopy(copies);
        j.Open('{').Set("name",p.name).Set("gamertag",p.gamertag).Set("xuid",p.xuid).Set("guest",!GuestNamed(party,p.name).empty())
         .Set("made",p.made).Set("madeOn",p.madeOn).Key("on").Open('[');
        for(size_t k=0;k<p.on.size();++k) j.String(p.on[k]);
        j.Close(']').Set("copies",int(copies.size())).Key("best");
        if(best) j.Open('{').Set("gamerscore",best->gamerscore).Set("achievements",best->achievements).Set("time",best->time).Set("console",best->console).Close('}');
        else j.Null();
        j.Close('}');
    }
    out.body=j.Close(']').Close('}').Text();
}

// POST /api/party/profiles {action, names?}: make (every guest on the list,
// or names: a profile on every console, a kept one put back), remove (keep
// a copy, then remove them from the consoles; every one made, or names) or
// forget (a record of one on no console: its kept copies stay).
void GuestProfilesChange(const web::Value& v, web::Reply& out) {
    std::string action;
    Text(v,"action",action);
    std::vector<std::string> names;
    const web::Value* list=v.Get("names");
    if(list && !Strings(*list,names)) { out.Error(400,"names is a list of guests' names."); return; }
    Party party=CurrentParty();
    std::vector<GuestProfile> records=GuestProfilesNow();
    if(action=="forget") {
        if(names.size()!=1) { out.Error(400,"Name the one to forget."); return; }
        std::lock_guard<std::mutex> hold(GuestLock);
        GuestProfile* p=GuestProfileNamed(GuestProfiles,names[0]);
        if(!p) { out.Error(404,"There's no profile for "+names[0]+"."); return; }
        if(!p->on.empty()) { out.Error(409,"It's still on a console: remove it first."); return; }
        GuestProfiles.erase(GuestProfiles.begin()+(p-&GuestProfiles[0]));
    } else if(action=="make" || action=="remove") {
        bool make=action=="make";
        if(!list) {
            if(make) for(size_t i=0;i<party.guests.size();++i) names.push_back(party.guests[i].name);
            else for(size_t i=0;i<records.size();++i) if(!records[i].xuid.empty()) names.push_back(records[i].name);
        }
        for(size_t i=0;i<names.size();++i) {
            bool known=make ? !GuestNamed(party,names[i]).empty() : GuestProfileNamed(records,names[i])!=NULL;
            if(!known) { out.Error(400,names[i]+(make ? " isn't on the guest list." : " has no guest profile.")); return; }
        }
        if(names.empty()) { out.Error(400,make ? "The guest list is empty." : "There are no guest profiles to remove."); return; }
        {
            std::lock_guard<std::mutex> hold(GuestLock);
            if(Work.running) { out.Error(409,"The guest profiles are busy: "+(Work.step.empty() ? Work.what : Work.step)+"."); return; }
            Work=ProfileWork();
            Work.running=true; Work.what=action; Work.at=(long long)time(NULL);
        }
        if(make) std::thread(MakeGuestProfiles,names).detach();
        else std::thread(RemoveGuestProfiles,names).detach();
        Log("Guest profiles: %s for %s (asked on the dashboard).",make ? "making" : "removing",Count(int(names.size()),"guest","guests").c_str());
    } else { out.Error(400,"Send action make, remove or forget."); return; }
    Save();
    GuestProfilesJson(out);
}

// The users page (Marcus 2026-11-03: a users or gamertags function apart
// from the LAN party, so each one's profile and saves follow them) -------

// Names off the party's guest list (a user removed), and names changed there
// (a user renamed on another computer), with their rules.
void PartyNamesChanged(const std::vector<std::string>& dropped, const std::vector<std::pair<std::string,std::string> >& renamed) {
    if(dropped.empty() && renamed.empty()) return;
    {
        std::lock_guard<std::mutex> change(PartyChangeLock);
        std::lock_guard<std::mutex> hold(PartyLock);
        Party& p=TheParty;
        for(size_t i=p.guests.size();i-->0;)
            for(size_t k=0;k<dropped.size();++k) if(SameName(p.guests[i].name,dropped[k])) { p.guests.erase(p.guests.begin()+long(i)); break; }
        for(size_t k=0;k<renamed.size();++k) {
            for(size_t i=0;i<p.guests.size();++i) if(SameName(p.guests[i].name,renamed[k].first)) p.guests[i].name=renamed[k].second;
            for(size_t i=0;i<p.together.size();++i) {
                if(SameName(p.together[i].first,renamed[k].first)) p.together[i].first=renamed[k].second;
                if(SameName(p.together[i].second,renamed[k].first)) p.together[i].second=renamed[k].second;
            }
            for(size_t i=0;i<p.apart.size();++i) {
                if(SameName(p.apart[i].first,renamed[k].first)) p.apart[i].first=renamed[k].second;
                if(SameName(p.apart[i].second,renamed[k].first)) p.apart[i].second=renamed[k].second;
            }
        }
        TidyParty(p);
    }
    PartyNews();
}

// users.h's MergeUsersFile (defined after this namespace, as nia's, so any file can call it).
bool MergeUsersFileHere(const std::string& text, std::string& error) {
    std::vector<User> other;
    ReadUsers(text,other);
    if(other.empty() && text.find("[Users]")==std::string::npos) { error="That isn't a NetDisc360 users file (users.ini)."; return false; }
    Party party=CurrentParty();
    std::vector<GuestStats> stats=GuestStatsNow();
    std::vector<User> changedUsers;
    std::vector<std::string> dropped;
    std::vector<std::pair<std::string,std::string> > renamed;
    {
        std::lock_guard<std::mutex> hold(GuestLock);
        RefreshUsers(party,stats);
        std::map<std::string,std::string> namesBefore;
        for(size_t i=0;i<Users.size();++i) if(!Users[i].removed) namesBefore[Users[i].id]=Users[i].name;
        std::vector<std::string> ids=MergeUsers(Users,other,(long long)time(NULL));
        for(size_t i=0;i<ids.size();++i) {
            User* u=UserWithId(Users,ids[i]);
            if(!u) continue;
            std::map<std::string,std::string>::const_iterator was=namesBefore.find(u->id);
            if(u->removed) {
                // Removed on another computer: its record goes, unless its profile is on a console here.
                if(was==namesBefore.end()) continue;
                GuestProfile* p=GuestProfileNamed(GuestProfiles,was->second);
                if(p && !p->on.empty()) { u->removed=0; u->changed=(long long)time(NULL)+1; continue; }
                if(p) GuestProfiles.erase(GuestProfiles.begin()+(p-&GuestProfiles[0]));
                dropped.push_back(was->second);
                UsersSaved[u->id]=UserContent(*u);
                continue;
            }
            if(was!=namesBefore.end() && was->second!=u->name) {
                renamed.push_back(std::make_pair(was->second,u->name));
                GuestProfile* p=GuestProfileNamed(GuestProfiles,was->second);
                if(p && !GuestProfileNamed(GuestProfiles,u->name)) p->name=u->name;
            }
            UserIntoRecords(*u);
            UsersSaved[u->id]=UserContent(*u);
            changedUsers.push_back(*u);
        }
    }
    {
        std::lock_guard<std::mutex> hold(StatsLock);
        for(size_t i=0;i<renamed.size();++i)
            for(size_t k=0;k<GuestStatsList.size();++k) if(SameName(GuestStatsList[k].name,renamed[i].first)) GuestStatsList[k].name=renamed[i].second;
        for(size_t i=0;i<dropped.size();++i)
            for(size_t k=GuestStatsList.size();k-->0;) if(SameName(GuestStatsList[k].name,dropped[i])) GuestStatsList.erase(GuestStatsList.begin()+long(k));
        for(size_t i=0;i<changedUsers.size();++i) UserIntoStats(changedUsers[i]);
    }
    PartyNamesChanged(dropped,renamed);
    Save();
    Log("Users: merged another computer's users (%s changed here).",Count(int(changedUsers.size()+dropped.size()),"user","users").c_str());
    return true;
}

// GET /api/users: every user (not removed), with their profile, the consoles
// it's on, their stats and the copies kept here, and the work under way.
void UsersJson(web::Reply& out) {
    Options o=Current();
    Party party=CurrentParty();
    std::vector<GuestStats> stats=GuestStatsNow();
    std::vector<User> users;
    std::vector<GuestProfile> records;
    ProfileWork work;
    {
        std::lock_guard<std::mutex> hold(GuestLock);
        RefreshUsers(party,stats);
        users=Users; records=GuestProfiles; work=Work;
    }
    std::vector<std::string> taken;
    for(size_t i=0;i<records.size();++i) taken.push_back(records[i].gamertag);
    web::Json j;
    j.Open('{').Key("work").Open('{').Set("running",work.running).Set("what",work.what).Set("step",work.step).Set("result",work.result)
     .Set("error",work.error).Set("at",work.at).Close('}').Set("folder",config::FromUtf8(ProfilesFolder(o))).Set("file",config::FromUtf8(UsersPath(o.configPath)))
     .Key("users").Open('[');
    for(size_t i=0;i<users.size();++i) {
        const User& u=users[i];
        if(u.removed) continue;
        GuestProfile* p=GuestProfileNamed(records,u.name);
        std::string gamertag=p ? p->gamertag : std::string();
        if(gamertag.empty()) { gamertag=GuestGamertag(u.name,taken); taken.push_back(gamertag); }
        std::string guest=GuestNamed(party,u.name);
        bool here=false;
        for(size_t k=0;k<party.guests.size();++k) if(SameName(party.guests[k].name,u.name)) here=party.guests[k].here;
        std::vector<ProfileCopy> copies=u.xuid.empty() ? std::vector<ProfileCopy>() : ProfileCopies(KitPath(ProfilesFolder(o),u.xuid));
        const ProfileCopy* best=BestProfileCopy(copies);
        j.Open('{').Set("id",u.id).Set("name",u.name).Set("gamertag",gamertag).Set("xuid",u.xuid).Set("made",u.made).Set("adopted",u.adopted)
         .Set("madeOn",p ? p->madeOn : std::string()).Set("playedOn",p ? p->playedOn : std::string()).Set("changed",u.changed)
         .Set("guest",!guest.empty()).Set("here",here).Key("on").Open('[');
        for(size_t k=0;p && k<p->on.size();++k) j.String(p->on[k]);
        j.Close(']').Key("stale").Open('[');
        for(size_t k=0;p && k<p->stale.size();++k) j.String(p->stale[k]);
        j.Close(']').Key("stats").Open('{').Set("gamerscore",u.gamerscore).Set("achievements",u.achievements).Set("readAt",u.readAt)
         .Set("playSeconds",u.playSeconds).Set("plays",u.plays).Set("parties",u.parties).Set("lastParty",u.lastParty).Set("lastGame",u.lastGame).Close('}')
         .Set("copies",int(copies.size())).Set("newest",copies.empty() ? 0LL : copies[0].time).Key("best");
        if(best) j.Open('{').Set("gamerscore",best->gamerscore).Set("achievements",best->achievements).Set("time",best->time).Set("console",best->console).Close('}');
        else j.Null();
        j.Close('}');
    }
    out.body=j.Close(']').Close('}').Text();
}

// POST /api/users {action, ...}: add {name}; remove {id} (their profile must
// be on no console: its kept copies stay); adopt {console, xuid, gamertag,
// name?} (a profile someone made on a console becomes a user's); import
// {text} (another computer's users.ini, merged in).
void UsersChange(const web::Value& v, web::Reply& out) {
    std::string action, name, id, error;
    Text(v,"action",action);
    if(action=="add") {
        Text(v,"name",name);
        if(!CleanName(name,1,GuestNameMost,"The name",name,error)) { out.Error(400,error); return; }
        std::lock_guard<std::mutex> hold(GuestLock);
        if(UserNamed(Users,name)) { out.Error(409,"There's a user called "+name+" already."); return; }
        if(Users.size()>=UsersMost) { out.Error(409,"NetDisc360 keeps "+Number(UsersMost)+" users at most."); return; }
        UserFor(name);
    } else if(action=="remove") {
        Text(v,"id",id);
        std::string named;
        {
            std::lock_guard<std::mutex> hold(GuestLock);
            User* u=UserWithId(Users,id);
            if(!u || u->removed) { out.Error(404,"There's no such user."); return; }
            GuestProfile* p=GuestProfileNamed(GuestProfiles,u->name);
            if(p && !p->xuid.empty() && !p->on.empty()) {
                std::string where;
                for(size_t k=0;k<p->on.size();++k) {
                    Console* c=ConsoleIfKnown(inet_addr(p->on[k].c_str()));
                    where+=(k ? ", " : "")+(c ? ConsoleWord(*c) : p->on[k]);
                }
                out.Error(409,u->name+"'s profile is still on "+where+": remove it from the consoles first (a copy is kept on this computer).");
                return;
            }
            if(p) GuestProfiles.erase(GuestProfiles.begin()+(p-&GuestProfiles[0]));
            named=u->name;
            u->removed=(long long)time(NULL);
            u->xuid.clear(); u->gamertag.clear(); u->made=0; u->adopted=false;
            u->changed=std::max(u->removed,u->changed+1);
            UsersSaved[u->id]=UserContent(*u);
        }
        {
            std::lock_guard<std::mutex> hold(StatsLock);
            for(size_t k=GuestStatsList.size();k-->0;) if(SameName(GuestStatsList[k].name,named)) GuestStatsList.erase(GuestStatsList.begin()+long(k));
        }
        PartyNamesChanged(std::vector<std::string>(1,named),std::vector<std::pair<std::string,std::string> >());
        Log("Users: %s removed (their kept profile copies stay).",named.c_str());
    } else if(action=="adopt") {
        std::string console, xuidText, gamertag, xuid;
        Text(v,"console",console); Text(v,"xuid",xuidText); Text(v,"gamertag",gamertag); Text(v,"name",name);
        if(!OfflineXuid(xuidText,xuid)) { out.Error(400,"That isn't an offline profile's XUID (16 hex digits starting with E)."); return; }
        if(!ConsoleIfKnown(inet_addr(console.c_str()))) { out.Error(400,console+" isn't a console NetDisc360 knows."); return; }
        if(name.empty()) name=gamertag;
        if(!CleanName(name,1,GuestNameMost,"The name",name,error)) { out.Error(400,error); return; }
        {
            std::lock_guard<std::mutex> hold(GuestLock);
            for(size_t k=0;k<GuestProfiles.size();++k)
                if(GuestProfiles[k].xuid==xuid) { out.Error(409,"That profile is "+GuestProfiles[k].name+"'s already."); return; }
            GuestProfile* p=GuestProfileNamed(GuestProfiles,name);
            if(p && !p->xuid.empty()) { out.Error(409,name+" has a profile already; give this one another name."); return; }
            UserFor(name);
        }
        AdoptGuestProfile(name,strtoull(xuid.c_str(),NULL,16),gamertag,console);
        GuestProfile* got;
        {
            std::lock_guard<std::mutex> hold(GuestLock);
            got=GuestProfileNamed(GuestProfiles,name);
            if(!got || got->xuid!=xuid) { out.Error(409,"That profile couldn't be "+name+"'s."); return; }
        }
        Options o=Current();
        MarkProfilesOn(ConsolesAt(std::vector<std::string>(1,console)),o);
    } else if(action=="import") {
        std::string text;
        Text(v,"text",text);
        if(!MergeUsersFileHere(text,error)) { out.Error(400,error); return; }
    } else { out.Error(400,"Send action add, remove, adopt or import."); return; }
    Save();
    UsersJson(out);
}

// POST /api/users/profiles {action, ids, consoles?}: put (each user's
// profile on the consoles: made if they have none, else their kept copy or
// another console's), backup (a copy of it and their saves kept here) or
// remove (a copy kept, then removed from them). consoles: addresses; without
// it, the party's consoles (or every one) for put, where it is for the others.
void UserProfilesChange(const web::Value& v, web::Reply& out) {
    std::string action;
    Text(v,"action",action);
    std::vector<std::string> ids, consoles, names;
    const web::Value* f=v.Get("ids");
    if(!f || !Strings(*f,ids) || ids.empty()) { out.Error(400,"ids is a list of users' ids."); return; }
    if((f=v.Get("consoles")) && !Strings(*f,consoles)) { out.Error(400,"consoles is a list of consoles' addresses."); return; }
    for(size_t i=0;i<consoles.size();++i)
        if(!ConsoleIfKnown(inet_addr(consoles[i].c_str()))) { out.Error(400,consoles[i]+" isn't a console NetDisc360 knows."); return; }
    if(action!="put" && action!="backup" && action!="remove") { out.Error(400,"Send action put, backup or remove."); return; }
    {
        std::lock_guard<std::mutex> hold(GuestLock);
        for(size_t i=0;i<ids.size();++i) {
            User* u=UserWithId(Users,ids[i]);
            if(!u || u->removed) { out.Error(404,"There's no user with the id "+ids[i]+"."); return; }
            GuestProfile* p=GuestProfileNamed(GuestProfiles,u->name);
            if(action!="put" && (!p || p->xuid.empty())) { out.Error(400,u->name+" has no profile yet."); return; }
            names.push_back(u->name);
        }
        if(Work.running) { out.Error(409,"The profiles are busy: "+(Work.step.empty() ? Work.what : Work.step)+"."); return; }
        Work=ProfileWork();
        Work.running=true; Work.what=action; Work.at=(long long)time(NULL);
    }
    if(action=="put") std::thread(MakeGuestProfilesOn,names,consoles).detach();
    else if(action=="backup") std::thread(BackUpGuestProfiles,names,consoles).detach();
    else std::thread(RemoveGuestProfilesFrom,names,consoles).detach();
    Log("Users: %s %s's profile%s (asked on the dashboard).",action=="put" ? "putting" : action=="backup" ? "backing up" : "removing",
        names.size()==1 ? names[0].c_str() : Count(int(names.size()),"user","users").c_str(),names.size()==1 ? "" : "s");
    UsersJson(out);
}

// The library's games by Title ID: the first one's name, and the listed name
// of each (in lower case, as a console's tiles have them).
void LibraryByTitle(std::map<DWORD,std::string>& names, std::map<DWORD,std::vector<std::string> >& listed) {
    std::lock_guard<std::mutex> hold(GamesListLock);
    for(size_t i=0;i<GamesList.size();++i) {
        const IsoFile& g=GamesList[i];
        if(!g.titleId) continue;
        if(!names.count(g.titleId)) names[g.titleId]=config::ToUtf8(WithoutIso(g.name));
        listed[g.titleId].push_back(Lower(g.name));
    }
}

// GET /api/systemlink: the System Link games (see systemlink.h), each with
// whether it's in the library and which consoles have it (a tile, or
// installed): the party's while it's on, else every console.
void SystemLinkList(web::Reply& out) {
    std::vector<std::string> only=ComparedConsoles();
    std::map<DWORD,std::string> names;
    std::map<DWORD,std::vector<std::string> > listed, on;
    LibraryByTitle(names,listed);
    std::vector<Console*> consoles=AllConsoles();
    {
        std::lock_guard<std::mutex> hold(ConsoleLock);
        for(size_t k=0;k<(only.empty() ? consoles.size() : only.size());++k)
            for(size_t i=0;i<consoles.size();++i) {
                const Console& c=*consoles[i];
                if(only.empty() ? i!=k : only[k]!=c.text) continue;
                if(only.empty() && c.hiddenAt!=-2 && c.seenAt<=c.hiddenAt && !c.connections) continue;  // Forgotten.
                std::set<DWORD> has;
                for(std::map<DWORD,std::vector<std::string> >::const_iterator t=listed.begin();t!=listed.end();++t)
                    for(size_t n=0;n<t->second.size();++n) if(c.tiles.count(t->second[n])) has.insert(t->first);
                for(size_t n=0;n<c.installed.size();++n) has.insert(c.installed[n].titleId);
                for(std::set<DWORD>::const_iterator t=has.begin();t!=has.end();++t) on[*t].push_back(c.text);
            }
    }
    web::Json j;
    j.Open('{').Set("attribution",SystemLinkAttribution).Key("games").Open('[');
    for(size_t i=0;i<SystemLinkCount;++i) {
        const SystemLinkGame& g=SystemLinkGames[i];
        char title[12]; _snprintf_s(title,sizeof(title),_TRUNCATE,"%08X",g.titleId);
        j.Open('{').Set("titleId",title).Set("name",g.name).Set("platform",g.xbox ? "xbox" : "360").Key("total");
        if(g.total) j.Int(g.total); else j.Null();
        j.Key("perConsole");
        if(g.perConsole) j.Int(g.perConsole); else j.Null();
        std::map<DWORD,std::string>::const_iterator name=names.find(g.titleId);
        j.Set("notes",g.notes).Set("inLibrary",name!=names.end()).Key("game");
        if(name!=names.end()) j.String(name->second); else j.Null();
        j.Key("consoles");
        NamesJson(j,on[g.titleId]);
        j.Close('}');
    }
    j.Close(']').Close('}');
    out.body=j.Text();
}

// A time() as ISO 8601 in UTC: 2026-11-01T18:30:00Z.
std::string IsoTime(long long when) {
    time_t t=time_t(when);
    struct tm utc;
#ifdef _WIN32
    if(gmtime_s(&utc,&t)) return std::string();
#else
    if(!gmtime_r(&t,&utc)) return std::string();
#endif
    char text[32];
    strftime(text,sizeof(text),"%Y-%m-%dT%H:%M:%SZ",&utc);
    return text;
}

// GET /api/titleupdates: each console's title updates as last read from its
// Aurora (the party's consoles while it's on, else all), and the library's
// games that any of them has one for: the one each console applies, the
// newest, and whether they differ.
void TitleUpdatesList(web::Reply& out) {
    std::vector<ConsoleUpdates> consoles=UpdatesOf(ComparedConsoles());
    web::Json j;
    j.Open('{').Key("consoles").Open('[');
    for(size_t c=0;c<consoles.size();++c) {
        const ConsoleUpdates& u=consoles[c];
        j.Open('{').Set("address",u.address).Key("read");
        if(u.read) j.String(IsoTime(u.readAt)); else j.Null();
        j.Set("error",u.error).Key("tus").Open('[');
        for(size_t i=0;i<u.updates.size();++i) {
            const TitleUpdate& t=u.updates[i];
            char title[12], media[12];
            _snprintf_s(title,sizeof(title),_TRUNCATE,"%08X",t.titleId);
            _snprintf_s(media,sizeof(media),_TRUNCATE,"%08X",t.mediaId);
            j.Open('{').Set("titleId",title).Set("mediaId",media).Set("baseVersion",(long long)t.baseVersion).Set("version",(long long)t.version);
            j.Set("name",t.name).Set("file",t.file).Set("active",t.active).Close('}');
        }
        j.Close(']').Close('}');
    }
    j.Close(']').Key("games").Open('[');
    std::vector<IsoFile> games;
    { std::lock_guard<std::mutex> hold(GamesListLock); games=GamesList; }
    std::set<DWORD> shown;
    for(size_t i=0;i<games.size();++i) {
        const IsoFile& g=games[i];
        if(!g.titleId || shown.count(g.titleId)) continue;
        GameUpdates u=JudgeUpdates(g.titleId,g.mediaId,consoles);
        if(!u.newest) continue;  // No console has one.
        shown.insert(g.titleId);
        char title[12]; _snprintf_s(title,sizeof(title),_TRUNCATE,"%08X",g.titleId);
        j.Open('{').Set("titleId",title).Set("game",config::ToUtf8(WithoutIso(g.name))).Key("byConsole").Open('{');
        for(size_t k=0;k<u.inUse.size();++k) {
            j.Key(u.inUse[k].first.c_str());
            if(u.inUse[k].second) j.Open('{').Set("version",(long long)u.inUse[k].second->version).Set("name",UpdateName(*u.inUse[k].second)).Close('}');
            else j.Null();
        }
        j.Close('}').Key("newest").Open('{').Set("version",(long long)u.newest->version).Set("name",UpdateName(*u.newest)).Close('}');
        j.Set("mismatch",u.mismatch).Close('}');
    }
    j.Close(']').Close('}');
    out.body=j.Text();
}

// A game's netiso.log from its folder on the console (Aurora's FTP server,
// which answers only while Aurora runs): the last 256 KB.
void GameLog(const std::string& address, const std::string& name, web::Reply& out) {
    std::string error;
    Console* c=AskedConsole(address,error);
    if(!c) { out.Error(400,error); return; }
    std::string folder;
    {
        std::lock_guard<std::mutex> hold(ConsoleLock);
        for(std::map<std::string,std::string>::const_iterator t=c->tiles.begin();t!=c->tiles.end() && folder.empty();++t)
            if(Lower(config::ToUtf8(WithoutIso(t->first)))==Lower(name)) folder=t->second;
    }
    if(folder.empty()) { out.Error(400,"That console has no folder for it."); return; }
    Options o=Current();
    std::string path=o.xboxFolder+"/"+folder;
    Busy busy(c);
    FtpClient ftp(c->address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),8);
    std::vector<FtpEntry> files;
    // Listed first: FtpDll answers a missing file late, and the next command with it.
    if(!ftp.List(path.c_str(),files)) { out.Error(400,ftp.Code() ? std::string("Its game folder isn't there.") : "Aurora's FTP server doesn't answer: it runs only while Aurora is on screen."); return; }
    const FtpEntry* log=NULL;
    for(size_t i=0;i<files.size() && !log;++i) if(!files[i].folder && !_stricmp(files[i].name.c_str(),"netiso.log")) log=&files[i];
    web::Json j;
    j.Open('{').Set("folder",o.xboxFolder+"/"+config::ToUtf8(folder));
    if(!log) { j.Set("text","").Set("missing",true).Close('}'); out.body=j.Text(); return; }
    std::vector<unsigned char> data;
    if(!ftp.Retrieve((path+"/netiso.log").c_str(),data,4<<20)) { out.Error(400,std::string("Its netiso.log couldn't be read: ")+ftp.Error()+"."); return; }
    const size_t keep=256<<10;
    std::string text(data.begin()+long(data.size()>keep ? data.size()-keep : 0),data.end());
    for(size_t i=0;i<text.size();++i) if((unsigned char)text[i]>126 || ((unsigned char)text[i]<32 && text[i]!='\n' && text[i]!='\r' && text[i]!='\t')) text[i]='?';
    j.Set("text",text).Set("size",(unsigned long long)data.size()).Close('}');
    out.body=j.Text();
}

std::string Scrubbed(const std::string& text) {
    std::string out;
    for(size_t i=0;i<text.size();) {
        // a.b.c.d
        size_t at=i;
        int parts=0;
        while(parts<4) {
            size_t digits=at;
            while(digits<text.size() && digits-at<3 && isdigit((unsigned char)text[digits])) ++digits;
            if(digits==at) break;
            ++parts;
            at=digits;
            if(parts<4) { if(at<text.size() && text[at]=='.') ++at; else break; }
        }
        bool before=i>0 && (isdigit((unsigned char)text[i-1]) || text[i-1]=='.');
        if(parts==4 && !before && (at>=text.size() || !isdigit((unsigned char)text[at]))) { out+="x.x.x.x"; i=at; continue; }
        // \Users\<name>\ or /home/<name>/
        if(!_strnicmp(text.c_str()+i,"\\Users\\",7) || !strncmp(text.c_str()+i,"/home/",6)) {
            char slash=text[i];
            size_t name=i+(slash=='/' ? 6 : 7), end=text.find(slash,name);
            out+=text.substr(i,name-i)+"(user)";
            i=end==std::string::npos ? text.size() : end;
            continue;
        }
        out+=text[i++];
    }
    return out;
}

// What the server can tell about its own setup. The page adds each console's
// checks (CheckConsole), and makes the report from both, scrubbed too.
void SetupCheck(web::Reply& out) {
    Options o=Current();
    std::vector<IsoFile> games;
    bool scanned;
    { std::lock_guard<std::mutex> hold(GamesListLock); games=GamesList; scanned=GamesScanned; }
    int kinds[3]={0,0,0}, found=0, open=0;
    for(size_t i=0;i<games.size();++i) ++kinds[games[i].kind];
    for(size_t i=0;i<o.folders.size();++i) if(config::IsFolder(o.folders[i])) ++found;
    { std::lock_guard<std::mutex> hold(DuplicatesLock); for(size_t i=0;i<Conflicts.size();++i) open+=Conflicts[i].open; }
    std::vector<std::string> networks=NetworkCategories();
    bool publicNetwork=std::find(networks.begin(),networks.end(),"public")!=networks.end();
    web::Json j;
    j.Open('{').Set("version",NETDISC_VERSION).Set("runs",UpdateHow()).Set("scanned",scanned);
    j.Set("folders",int(o.folders.size())).Set("foldersFound",found).Set("games",int(games.size()));
    j.Set("iso",kinds[DiscIso]).Set("god",kinds[DiscGod]).Set("extracted",kinds[DiscFolder]).Set("conflicts",open);
    j.Set("canPush",CanPush).Set("push",o.push).Set("category",o.filter).Set("scan",o.scan).Set("skipInstalled",o.skipInstalled);
    j.Set("auroraScripts",o.auroraScripts).Set("followAddress",o.followAddress).Set("keepAwake",o.keepAwake).Set("webLan",o.webLan);
    j.Key("networks").Open('[');
    for(size_t i=0;i<networks.size();++i) j.String(networks[i]);
    j.Close(']');
#ifdef _WIN32
    j.Set("canFix",publicNetwork && InTray);
#else
    j.Set("canFix",false);
#endif
    j.Set("publicNetwork",publicNetwork);
    j.Key("warnings").Open('[');
    {
        std::lock_guard<std::mutex> hold(EventLock);
        int n=0;
        for(size_t i=Events.size();i-->0 && n<10;) if(Events[i].kind==EventWarning) { j.String(Scrubbed(config::ToUtf8(Events[i].text))); ++n; }
    }
    j.Close(']').Close('}');
    out.body=j.Text();
}

void Api(const web::Request& r, web::Reply& out) {
    web::Value body;
    if(r.method=="POST" && !web::Parse(r.body,body)) { out.Error(400,"That isn't JSON."); return; }
    std::string error;
    if(r.method=="GET" && r.path=="/api/state") State(out);
    else if(r.method=="GET" && r.path=="/api/games") Games(out);
    else if(r.method=="GET" && r.path=="/api/log") LogSince(atoll(r.Param("after").c_str()),out);
    else if(r.method=="GET" && r.path=="/api/browse") Browse(r.Param("path"),out);
    else if(r.method=="POST" && r.path=="/api/consoles/find") FindConsoles(body,out);
    else if(r.method=="POST" && r.path=="/api/consoles/check") CheckConsole(body,out);
    else if(r.method=="GET" && r.path=="/api/setup") SetupCheck(out);
    else if(r.method=="GET" && r.path=="/api/console/status") ConsoleStatus(r.Param("address"),out);
    else if(r.method=="GET" && r.path=="/api/console/gamelog") GameLog(r.Param("address"),r.Param("game"),out);
    else if((r.method=="GET" && (r.path=="/api/console/screenshots" || r.path=="/api/console/screenshot")) ||
            (r.method=="POST" && (r.path=="/api/console/screenshot/take" || r.path=="/api/console/screenshot/delete"))) Screenshots(r,body,out);
    else if(r.method=="POST" && r.path=="/api/console/power") {
        if(Power(body,error)) out.body="{\"ok\":true}";
        else out.Error(400,error);
    }
    else if(r.method=="GET" && r.path=="/api/console/setup") ConsoleSetup(r.Param("address"),NULL,out);
    else if(r.method=="POST" && r.path=="/api/console/setup") { std::string address; Text(body,"address",address); ConsoleSetup(address,&body,out); }
    else if(r.method=="POST" && r.path=="/api/console/setup/restore") {
        std::string backup;
        if(RestoreLaunchIni(body,backup,error)) { web::Json j; out.body=j.Open('{').Set("ok",true).Set("backup",backup).Close('}').Text(); }
        else out.Error(400,error);
    }
    else if(r.method=="POST" && r.path=="/api/console/identify") {
        if(Identify(body,error)) out.body="{\"ok\":true}";
        else out.Error(400,error);
    }
    else if(r.method=="POST" && r.path=="/api/play") {
        if(Play(body,error)) out.body="{\"ok\":true}";
        else out.Error(400,error);
    }
    else if(r.method=="POST" && r.path=="/api/party") {
        if(ChangeParty(body,error)) { out.body="{\"ok\":true}"; MakeProfilesSoon(AutoMakeDelaySeconds); }
        else out.Error(400,error);
    }
    else if(r.method=="POST" && r.path=="/api/party/raffle") PartyRaffle(body,out);
    else if(r.method=="POST" && r.path=="/api/party/stats") RefreshGuestStats(out);
    else if(r.method=="POST" && r.path=="/api/party/open") {
        int opened=0;
        if(!OpenPartyEverywhere("",opened,error)) out.Error(400,error);
        else { web::Json j; j.Open('{').Set("ok",true).Set("opened",opened).Close('}'); out.body=j.Text(); }
    }
    else if(r.method=="POST" && r.path=="/api/party/spin") PartySpin(body,out);
    else if(r.method=="POST" && r.path=="/api/party/keep") {
        int round;
        if(!KeepTeams(round,error)) out.Error(400,error);
        else { web::Json j; j.Open('{').Set("ok",true).Set("round",round).Close('}'); out.body=j.Text(); }
    }
    else if(r.method=="POST" && r.path=="/api/party/seats") {
        if(ChangeSeats(body,error)) { out.body="{\"ok\":true}"; PushSeatProfilesSoon(); }
        else out.Error(400,error);
    }
    else if(r.method=="POST" && r.path=="/api/party/start") PartyStart(body,out);
    else if(r.method=="POST" && r.path=="/api/party/winner") PartyWinner(body,out);
    else if(r.method=="POST" && r.path=="/api/party/vote") PartyVote(body,out);
    else if(r.method=="POST" && r.path=="/api/party/power") PartyPower(body,out);
    else if(r.method=="POST" && r.path=="/api/party/asset") PartyAsset(body,out);
    else if(r.method=="POST" && r.path=="/api/party/asset/remove") {
        if(RemovePartyAsset(body,error)) out.body="{\"ok\":true}";
        else out.Error(400,error);
    }
    else if(r.method=="GET" && r.path=="/api/party/asset") PartyAssetFile(r,out);
    else if(r.method=="GET" && r.path=="/api/party/profiles") GuestProfilesJson(out);
    else if(r.method=="GET" && r.path=="/api/users") UsersJson(out);
    else if(r.method=="POST" && r.path=="/api/users") UsersChange(body,out);
    else if(r.method=="POST" && r.path=="/api/users/profiles") UserProfilesChange(body,out);
    else if(r.method=="GET" && r.path=="/api/party/profiles/picture") GuestPictureFile(r,out);
    else if(r.method=="POST" && r.path=="/api/party/profiles") GuestProfilesChange(body,out);
    else if(r.method=="GET" && r.path=="/api/systemlink") SystemLinkList(out);
    else if(r.method=="GET" && r.path=="/api/titleupdates") TitleUpdatesList(out);
    else if(r.method=="POST" && r.path=="/api/network/private") {
#ifdef _WIN32
        if(!InTray) out.Error(400,"Only NetDisc360.exe in the notification area can change the network.");
        else {
            // Windows' prompt waits for an answer on this computer.
            std::thread([]() {
                std::string why;
                if(MakeNetworksPrivate(why)) Log("Windows was asked to make the Public network(s) Private (the administrator prompt was answered).");
                else Log("The network wasn't changed: %s",why.c_str());
            }).detach();
            Log("Asking Windows to make the network Private; answer its prompt on this computer.");
            out.body="{\"ok\":true}";
        }
#else
        out.Error(400,"Only Windows has network categories.");
#endif
    }
    else if(r.method=="POST" && r.path=="/api/consoles/art") ConsoleArt(body,out);
    else if(r.method=="POST" && r.path=="/api/consoles/speed") SpeedTest(body,out);
    else if(r.method=="GET" && r.path=="/api/metrics") Metrics(out);
    else if(r.method=="POST" && r.path=="/api/consoles/add") {
        if(AddConsole(body,error)) out.body="{\"ok\":true}";
        else out.Error(400,error);
    }
    else if(r.method=="POST" && r.path=="/api/settings") {
        if(ChangeSettings(body,error)) out.body="{\"ok\":true}";
        else out.Error(400,error);
    } else if(r.method=="POST" && r.path=="/api/conflict") {
        if(ChooseCopy(body,error)) out.body="{\"ok\":true}";
        else out.Error(400,error);
    } else if(r.method=="POST" && r.path=="/api/password") {
        std::string password;
        if(PasswordFixed) out.Error(400,"The password is set with NETDISC_WEB_PASSWORD, so it's changed there.");
        else if(!Text(body,"password",password)) out.Error(400,"Send the new password.");
        else if(password.empty() && Current().webLan) out.Error(400,"Turn off \"Other devices can open it\" first; without a password anyone on the network could change your settings.");
        else {
            web::SetPasswordHash(web::HashPassword(password));
            Log(password.empty() ? "The dashboard's password was removed." : "The dashboard's password was changed.");
            Save();
            out.body="{\"ok\":true}";
        }
    } else if(r.method=="GET" && r.path=="/api/backups") Backups(out);
    else if(r.method=="POST" && r.path=="/api/backup") {
        std::string name;
        if(!MakeBackup("manual",name,error)) out.Error(400,error);
        else { web::Json j; j.Open('{').Set("ok",true).Set("name",name).Close('}'); out.body=j.Text(); }
    } else if(r.method=="GET" && r.path=="/api/backup") {
        std::string name=r.Param("name");
        if(!ReadBackup(name,out.body,error)) out.Error(404,error);
        else { out.type="application/zip"; out.download=name; }
    } else if(r.method=="POST" && r.path=="/api/backup/delete") {
        std::string name, data;
        Text(body,"name",name);
        if(!ReadBackup(name,data,error)) out.Error(404,error);
        else {
            bool removed;
            { std::lock_guard<std::mutex> hold(BackupLock); removed=config::RemoveFile(BackupPath(name)); }
            if(!removed) out.Error(400,"That backup couldn't be deleted.");
            else { Log("Deleted the backup %s.",name.c_str()); out.body="{\"ok\":true}"; }
        }
    } else if(r.method=="POST" && r.path=="/api/restore") {
        // A backup in the folder by name, or a file from the browser (base64).
        std::string name, file, data;
        std::vector<std::string> notes;
        bool ok;
        if(Text(body,"name",name)) ok=ReadBackup(name,data,error);
        else if(Text(body,"file",file)) {
            ok=backup::FromBase64(file,data) && !data.empty();
            if(!ok) error="That file didn't arrive whole; try again.";
        } else { ok=false; error="Send a backup's name, or a file."; }
        if(ok && Restore(data,notes,error,name)) {
            web::Json j;
            j.Open('{').Set("ok",true).Key("notes").Open('[');
            for(size_t i=0;i<notes.size();++i) j.String(notes[i]);
            j.Close(']').Close('}');
            out.body=j.Text();
        } else out.Error(400,error);
    } else if(r.method=="GET" && r.path=="/api/update") UpdateInfo(out);
    else if(r.method=="POST" && r.path=="/api/update/check") {
        if(!ChecksUpdates(Current())) out.Error(400,"The dashboard checks for updates itself here.");
        else { CheckForUpdate(error); UpdateInfo(out); }
    } else if(r.method=="POST" && r.path=="/api/update/install") {
        std::string wanted;
        Text(body,"version",wanted);
        if(StartInstall(error,false,wanted)) out.body="{\"ok\":true}";
        else out.Error(400,error);
#ifdef _WIN32
    } else if(r.method=="POST" && r.path=="/api/restart" && InTray) {
        bool installing;
        { std::lock_guard<std::mutex> hold(UpdateLock); installing=Updates.installing; }
        if(installing) out.Error(400,"It's installing an update, which restarts it.");
        else {
            Log("Restarting (asked on the dashboard).");
            if(Restart(error)) out.body="{\"ok\":true}";
            else out.Error(400,error);
        }
#endif
    } else if(r.method=="POST" && r.path=="/api/forget") {
        std::string address;
        Text(body,"address",address);
        {
            std::lock_guard<std::mutex> hold(SettingsLock);
            for(size_t i=0;i<Known.size();++i) if(Known[i].address==address) Known.erase(Known.begin()+long(i--));
        }
        {
            std::lock_guard<std::mutex> hold(ConsoleLock);
            for(size_t i=0;i<Consoles.size();++i) if(address==Consoles[i]->text) Consoles[i]->hiddenAt=Consoles[i]->seenAt;
        }
        Save();
        out.body="{\"ok\":true}";
    } else out.Error(404,"There's no "+r.method+" "+r.path+".");
}

std::mutex WebLock;
std::string DashboardUrl;

void StartWeb(const Options& o, bool announce) {
    if(o.webPort<=0) return;
    std::lock_guard<std::mutex> hold(WebLock);
    std::string address=WebAddress(o), error;
    char url[64];
    if(!web::Start(address,o.webPort,Api,error)) {
        Log("The dashboard %s: %s",announce ? "stays where it was" : "isn't available",error.c_str());
#ifdef _WIN32
        if(InTray && !announce) DashboardReady("",error);
#endif
        return;
    }
    _snprintf_s(url,sizeof(url),_TRUNCATE,"http://127.0.0.1:%d/",o.webPort);
    DashboardUrl=url;
    std::vector<std::string> addresses=PcAddresses();
    if(address=="0.0.0.0" && !addresses.empty()) {
        std::string others;
        for(size_t i=0;i<addresses.size();++i) others+=(i ? ", http://" : "http://")+addresses[i]+":"+Number(o.webPort)+"/";
        Log("Dashboard: %s (from other devices too: %s)",url,others.c_str());
    } else if(address=="0.0.0.0") Log("Dashboard: port %d on this computer's address.",o.webPort);
    else Log("Dashboard: %s (on this computer).",url);
#ifdef _WIN32
    if(InTray && !announce) DashboardReady(url,"");
#endif
}

// Start-up ---------------------------------------------------------------

bool LoadFile(const std::string& path, std::vector<unsigned char>& out) {
    FILE* f=fopen(path.c_str(),"rb"); if(!f) return false;
    out.clear(); unsigned char part[65536]; size_t got;
    while((got=fread(part,1,sizeof(part),f))>0) out.insert(out.end(),part,part+got);
    fclose(f);
    return !out.empty();
}

bool BuiltIn(int id, std::vector<unsigned char>& out) {
#ifdef _WIN32
    HRSRC found=FindResourceA(NULL,MAKEINTRESOURCEA(id),MAKEINTRESOURCEA(10) /* RT_RCDATA */);
    HGLOBAL loaded=found ? ::LoadResource(NULL,found) : NULL;
    const unsigned char* data=loaded ? static_cast<const unsigned char*>(LockResource(loaded)) : NULL;
    if(!data) return false;
    out.assign(data,data+SizeofResource(NULL,found));
    return true;
#else
    (void)id; (void)out;
    return false;
#endif
}

void Usage() {
    printf(
        "NetDisc360, a NetISO server for Aurora. Serves the games in folders and their\n"
        "subfolders to the NetISO plugin, and keeps the Xbox's game folders for Aurora in\n"
        "step with them. Its dashboard is a web page: http://127.0.0.1:8360 by default.\n"
        "Games can be ISOs, extracted to a folder (default.xex at the top; a \"Disc 2\"\n"
        "folder inside is a disc of its own), or Games on Demand packages. An original\n"
        "Xbox game (default.xbe at the top) is served too. Each game is served once;\n"
        "extra copies are listed so they can be removed.\n\n"
        "  netdisc [options] [folder ...]   (default: the current folder)\n\n"
        "  --config <file>         keep the settings in this file, and take them from it:\n"
        "                          what the dashboard changes is kept there and wins over\n"
        "                          these options\n"
        "  --xbox <address>        a console to update; give it once for each (default: every\n"
        "                          console that connects)\n"
        "  --ftp-user <name>       Aurora's FTP login (xboxftp)\n"
        "  --ftp-password <text>   (xboxftp; or set NETDISC_FTP_PASSWORD)\n"
        "  --xbox-folder <path>    the game folder on the Xbox, as an FTP path (/Hdd1/NetISO)\n"
        "  --aurora <path>         Aurora's folder on the Xbox, as an FTP path (/Hdd1/Apps/Aurora)\n"
        "  --no-push               only serve games; leave the Xbox alone\n"
        "  --no-category           leave Aurora's Xbox 360 category alone\n"
        "  --no-scan               don't ask Aurora to scan after adding or removing games\n"
        "  --no-auto-profiles      don't make a LAN party guest's profile by itself once\n"
        "                          the name is on the list (Make profiles on the page does)\n"
        "  --installed <folders>   where games installed on the Xbox are, as FTP paths with\n"
        "                          semicolons between (/Hdd1/Games); those and Games on Demand\n"
        "                          get no game folder\n"
        "  --no-installed          give games installed on the Xbox a game folder too\n"
        "  --no-aurora-scripts     no cleanup script or \"PC is off\" filter in Aurora\n"
        "  --aurora-web-port <n>   Aurora's web server, to start games and restart Aurora (9999)\n"
        "  --xbdm-port <n>         The consoles' debug monitor, to turn them off (730)\n"
        "  --online-wait <s>       after the Xbox's FTP comes back, the longest wait for the Xbox\n"
        "                          to come online before writing game folders (120)\n"
        "  --remove-wait <s>       how long after the start (or a change of game folders) game\n"
        "                          folders whose game is gone are kept on the Xbox (60)\n"
        "  --installed-every <s>   how often each console's installed games are looked at again (300)\n"
        "  --ping <s>              how often each console is pinged to see it's still on (30);\n"
        "                          0: never\n"
        "  --port <n>              NetISO port (4323)   --ftp-port <n>  (21)\n"
        "  --web-port <n>          the dashboard's port (8360); 0: no dashboard\n"
        "  --web-bind <address>    where the dashboard listens: 127.0.0.1, this computer only\n"
        "                          (default), or 0.0.0.0, the network too. A password for it\n"
        "                          is set on the dashboard or with NETDISC_WEB_PASSWORD.\n"
        "  -v                      log every request\n"
        "  -r, -b                  accepted for netiso-srv's command line; subfolders are always searched\n");
}

// fromFile: the settings file's "options" (see LoadConfig), which may come
// from a backup: there, nothing that ends the program, opens the dashboard to
// the network or picks the files it runs or sends to the Xbox.
bool ParseArguments(const std::vector<std::string>& args, Options& o, bool fromFile) {
    for(size_t i=0;i<args.size();++i) {
        const std::string& a=args[i];
        bool hasValue=i+1<args.size();
        if(fromFile && (a=="-h" || a=="--help" || a=="/?" || a=="--push-once" || a=="--config" || a=="--releases" ||
                        a=="--web-bind" || a=="--launcher" || a=="--boot")) return false;
        if(a=="-h" || a=="--help" || a=="/?") { Usage(); exit(0); }
        else if(a=="-r" || a=="-b") {}
        else if(a=="-v") o.verbose=true;
        else if(a=="--no-push") o.push=false;
        else if(a=="--no-category") o.filter=false;
        else if(a=="--no-scan") o.scan=false;
        else if(a=="--no-auto-profiles") o.autoProfiles=false;
        else if(a=="--no-installed") o.skipInstalled=false;
        else if(a=="--no-aurora-scripts") o.auroraScripts=false;
        else if(a=="--tidy-aurora") o.tidyAurora=true;  // For the tests.
        else if(a=="--push-once") o.pushOnce=true;  // For the tests.
        else if(hasValue && a=="--xbox") {
            DWORD xbox=inet_addr(args[++i].c_str());
            if(xbox==INADDR_NONE || !xbox) { fprintf(stderr,"--xbox needs an IPv4 address like 192.168.1.20.\n"); return false; }
            if(std::find(o.xboxes.begin(),o.xboxes.end(),xbox)==o.xboxes.end()) o.xboxes.push_back(xbox);
        }
        else if(hasValue && a=="--ftp-user") o.ftpUser=args[++i];
        else if(hasValue && a=="--ftp-password") o.ftpPassword=args[++i];
        else if(hasValue && a=="--xbox-folder") o.xboxFolder=args[++i];
        else if(hasValue && a=="--aurora") o.aurora=args[++i];
        else if(hasValue && a=="--installed") o.installedFolders=args[++i];
        else if(hasValue && a=="--aurora-web-port") o.auroraWebPort=atoi(args[++i].c_str());
        else if(hasValue && a=="--live-every") LiveEverySeconds=std::max(1.0,atof(args[++i].c_str()));  // For the tests.
        else if(hasValue && a=="--xbdm-port") o.xbdmPort=atoi(args[++i].c_str());
        else if(hasValue && a=="--name-port") o.namePort=atoi(args[++i].c_str());  // For the tests.
        else if(hasValue && a=="--port") o.port=atoi(args[++i].c_str());
        else if(hasValue && a=="--ftp-port") o.ftpPort=atoi(args[++i].c_str());
        else if(hasValue && a=="--launcher") o.launcherPath=args[++i];
        else if(hasValue && a=="--boot") o.bootPath=args[++i];
        else if(hasValue && a=="--online-wait") o.onlineWait=atof(args[++i].c_str());
        else if(hasValue && a=="--remove-wait") o.removeWait=atof(args[++i].c_str());
        else if(hasValue && a=="--installed-every") o.installedEvery=atof(args[++i].c_str());
        else if(hasValue && a=="--ping") o.pingSeconds=atof(args[++i].c_str());
        else if(hasValue && a=="--config") o.configPath=config::ToUtf8(args[++i]);
        else if(hasValue && a=="--releases") o.releasesFile=config::ToUtf8(args[++i]);  // For the tests.
        else if(hasValue && a=="--web-port") o.webPort=atoi(args[++i].c_str());
        else if(hasValue && a=="--web-bind") {
            o.webAddress=args[++i];
            if(inet_addr(o.webAddress.c_str())==INADDR_NONE) { fprintf(stderr,"--web-bind needs an IPv4 address like 0.0.0.0.\n"); return false; }
        }
        else if(a[0]!='-') o.folders.push_back(CleanFolder(a));
        else { fprintf(stderr,"Unknown option %s (-h lists them).\n",a.c_str()); return false; }
    }
    o.xboxFolder=FtpFolder(o.xboxFolder);
    return o.port>0 && o.port<65536 && o.ftpPort>0 && o.ftpPort<65536 && o.webPort>=0 && o.webPort<65536 && o.auroraWebPort>0 && o.auroraWebPort<65536;
}
}  // namespace

// users.h: another computer's users.ini merged into this one's (the sync).
bool nia::MergeUsersFile(const std::string& text, std::string& error) { return MergeUsersFileHere(text,error); }

// What the dashboard's sessions are signed with besides the password's hash
// (see web.h): kept next to the settings file but not in it, so a backup
// doesn't carry it. Without a settings file, a new one each start.
std::string SessionSecret(const Options& o) {
    if(o.configPath.empty()) return web::NewSecret();
    std::string path=config::FolderOf(o.configPath)+"session.key", secret;
    if(config::ReadFile(path,secret,256) && secret.size()>=32) return secret;
    secret=web::NewSecret();
    if(!config::WriteFile(path,secret,true)) Log("Couldn't write %s; logins last until NetDisc360 restarts.",config::FromUtf8(path).c_str());
    return secret;
}

#ifdef _WIN32
// Ctrl+C or the window closed: ServerStopping first, then Windows ends it.
BOOL WINAPI ConsoleClosing(DWORD) {
    ServerStopping();
    return FALSE;
}
#endif

// Runs the server; returns only when it can't (see app.h).
int RunServer(const std::vector<std::string>& args, const std::string& configPath, bool tray) {
    InTray=tray;
    StartedAt=Now();
    // The Docker image passes the FTP password here, so it doesn't show in
    // the process list; --ftp-password still wins.
    if(const char* password=getenv("NETDISC_FTP_PASSWORD")) Settings.ftpPassword=password;
    InDocker=getenv("NETDISC_DOCKER") && *getenv("NETDISC_DOCKER");
    Options o=Settings;
    if(!ParseArguments(args,o)) return 2;
    if(!configPath.empty()) o.configPath=configPath;
    CommandLine=o;
    Given.folders=!o.folders.empty(); Given.xboxes=!o.xboxes.empty();
    Given.ftp=getenv("NETDISC_FTP_PASSWORD") || o.ftpUser!=Options().ftpUser || o.ftpPassword!=Options().ftpPassword;
    bool fresh=!o.configPath.empty() && !LoadConfig(o);
    // Without a settings file nothing the wizard sets would be kept.
    if(o.configPath.empty()) o.wizardDone=true;
    if(o.folders.empty() && o.configPath.empty()) o.folders.push_back(".");
    if(const char* password=getenv("NETDISC_WEB_PASSWORD")) {
        if(*password) { web::SetPasswordHash(web::HashPassword(password)); PasswordFixed=true; }
    }
#ifdef _WIN32
    SetConsoleTitleA("NetDisc360");
    WSADATA wsa;
    if(WSAStartup(MAKEWORD(2,2),&wsa)) { fprintf(stderr,"Winsock didn't start.\n"); return 1; }
#endif
    Log("NetDisc360 %s%s%s, built %s %s.",NETDISC_VERSION,NETDISC_COMMIT[0] ? " " : "",NETDISC_COMMIT,__DATE__,__TIME__);
    if(!o.configPath.empty()) Log("Settings: %s",config::FromUtf8(o.configPath).c_str());
    if(!o.configPath.empty()) Log("Disc map: %s",config::FromUtf8(DiscMapPath(o.configPath)).c_str());

    if(o.push || !o.launcherPath.empty()) {
        CanPush=o.launcherPath.empty() ? BuiltIn(IDR_LAUNCHER,Launcher) : LoadFile(o.launcherPath,Launcher);
        if(!o.launcherPath.empty() && !CanPush) { fprintf(stderr,"Can't read %s.\n",o.launcherPath.c_str()); return 2; }
        // boot.xex is optional: without it, each launcher mounts by itself.
        if(o.bootPath.empty()) BuiltIn(IDR_BOOT,Boot);
        else if(!LoadFile(o.bootPath,Boot)) { fprintf(stderr,"Can't read %s.\n",o.bootPath.c_str()); return 2; }
        // The party wheel app (party/), for OpenWheel; in Docker, next to the art.
        WheelAppBuiltIn=BuiltIn(IDR_PARTY_APP,WheelApp);
        if(!WheelAppBuiltIn) LoadFile("/usr/local/share/netdisc/party/default.xex",WheelApp);
        if(CanPush && !ExecutionInfoOffset(Launcher.data(),DWORD(Launcher.size()))) { fprintf(stderr,"The launcher XEX has no execution info header.\n"); return 2; }
        if(!CanPush && o.push) Log("This build has no launcher built in, so it won't update the Xbox (--launcher gives one).");
    }
    if(!CanPush) o.push=false;
    LoadAppArt(o);
#ifdef _WIN32
    if(!InTray) o.updateAuto=false;
#else
    o.updateAuto=false;
#endif
    std::string updatedFrom, updateTo;
    bool noted;
    {
        std::lock_guard<std::mutex> hold(SettingsLock);
        Settings=o;
        updatedFrom.swap(UpdatedFrom);
        updateTo.swap(UpdateTo);
        std::string backupName;
        backupName.swap(UpdateBackup);
        if(updatedFrom==NETDISC_VERSION && !updateTo.empty()) UpdateSkip=updateTo;  // It didn't take.
        else if(!updatedFrom.empty()) UpdateSkip.clear();
        noted=!o.configPath.empty() && NoteStart(updatedFrom,updatedFrom==NETDISC_VERSION ? updateTo : std::string(),backupName);
    }
    if(fresh || !updatedFrom.empty() || noted) Save();
    web::SetSessionSecret(SessionSecret(o));
    if(!updatedFrom.empty() && updatedFrom!=NETDISC_VERSION) {
        Log("Updated from NetDisc360 %s to %s.",updatedFrom.c_str(),NETDISC_VERSION);
        Event(EventGood,"Updated to NetDisc360 %s (from %s)",NETDISC_VERSION,updatedFrom.c_str());
    } else if(!updateTo.empty()) {
        Log("NetDisc360 %s didn't start after the update, so %s was put back. It won't install %s by itself.",updateTo.c_str(),NETDISC_VERSION,updateTo.c_str());
        Event(EventWarning,"NetDisc360 %s didn't start, so %s was put back; it won't install that version by itself",updateTo.c_str(),NETDISC_VERSION);
    }

    LoadPlays(o.configPath);
    std::string where;
    for(size_t i=0;i<o.folders.size();++i) where+=(i ? ", " : "")+o.folders[i];
    std::vector<IsoFile> isos=Isos();
    if(o.folders.empty()) Log("No game folders yet; add them on the dashboard.");
    else {
        Log("%d game(s) in %s and subfolders:",int(isos.size()),where.c_str());
        for(size_t i=0;i<isos.size();++i) Log("  %s",isos[i].name.c_str());
        if(isos.empty()) Log("New games put there are served without a restart.");
    }
    Event(EventGood,"Server started, serving %s",Count(int(isos.size()),"game","games").c_str());

    SOCKET listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    int on=1;
#ifndef _WIN32
    setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,reinterpret_cast<const char*>(&on),sizeof(on));
#endif
    sockaddr_in at; memset(&at,0,sizeof(at)); at.sin_family=AF_INET; at.sin_port=htons(u_short(o.port));
    at.sin_addr.s_addr=htonl(o.pushOnce ? 0x7F000001 : 0);
    if(listener==INVALID_SOCKET || bind(listener,reinterpret_cast<sockaddr*>(&at),sizeof(at)) || listen(listener,16)) {
        Log("Can't listen on port %d (error %d). Is netiso-srv or another copy of this server still running?",o.port,WSAGetLastError());
        return 1;
    }
    (void)on;
    Log("Listening on port %d.",o.port);
#ifdef _WIN32
    if(!InTray) SetConsoleCtrlHandler(ConsoleClosing,TRUE);
#else
    {   // docker stop, Ctrl+C: ServerStopping first. Blocked here, before
        // the threads start, so only the waiting thread gets them.
        sigset_t stops;
        sigemptyset(&stops); sigaddset(&stops,SIGTERM); sigaddset(&stops,SIGINT);
        pthread_sigmask(SIG_BLOCK,&stops,NULL);
        std::thread([stops]() {
            int got=0;
            sigwait(&stops,&got);
            Log("Stopping.");
            ServerStopping();
            fflush(NULL);
            _Exit(0);
        }).detach();
    }
#endif
    StartWeb(o,false);
    for(size_t i=0;i<o.xboxes.size();++i) {
        Console* console=ConsoleAt(o.xboxes[i]);
        if(console && o.push) Log("Keeping %s on the Xbox at %s up to date.",o.xboxFolder.c_str(),console->text);
    }
    if(o.push && o.xboxes.empty()) Log("Keeping %s up to date on every Xbox that connects (or give --xbox).",o.xboxFolder.c_str());
    std::thread(WatchLoop).detach();
    std::thread(RateLoop).detach();
    std::thread(AuroraLoop).detach();
    std::thread(LiveLoop).detach();
    std::thread(RelayLoop).detach();
    std::thread(PingLoop).detach();
    std::thread(NamesLoop).detach();
    std::thread(SizeLoop).detach();
#ifdef _WIN32
    std::thread(AwakeLoop).detach();
#endif
    std::thread(BackupLoop).detach();
    std::thread(UpdateLoop).detach();
    Serve(listener);
    return 0;
}

void ServerStopping() {
    Options o=Current();
    std::vector<Console*> consoles=AllConsoles();
    for(size_t i=0;i<consoles.size();++i) PlayEnded(*consoles[i]);
    std::shared_ptr<std::atomic<int> > left(new std::atomic<int>(0));
    for(size_t i=0;i<consoles.size();++i) {
        Console* c=consoles[i];
        if(!c->marked.exchange(false)) continue;
        ++*left;
        std::thread([c,o,left]() {
            Busy busy(c);
            FtpClient ftp(c->address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),3);
            std::string helper=o.xboxFolder+"/"+HelperFolder;
            std::vector<FtpEntry> files;
            bool shown=false;
            ftp.Delete((helper+"/"+OnlineMarker).c_str());
            if(o.restartAurora && ftp.List(helper.c_str(),files))
                for(size_t k=0;k<files.size();++k) if(!files[k].folder && !_stricmp(files[k].name.c_str(),ShownMarker)) shown=true;
            std::vector<unsigned char> state;
            if(shown && ftp.Retrieve((helper+"/"+ShownMarker).c_str(),state,64) && !std::string(state.begin(),state.end()).compare(0,6,"online"))
                RestartAurora(*c,o,"so its filter hides the PC games while NetDisc360 is off");
            ftp.Close();
            --*left;
        }).detach();
    }
    for(double until=Now()+5;*left>0 && Now()<until;) Sleep(100);
}

std::string ServerSummary() {
    int games;
    { std::lock_guard<std::mutex> hold(GamesListLock); games=int(GamesList.size()); }
    int playing=0;
    std::vector<Console*> consoles=AllConsoles();
    for(size_t i=0;i<consoles.size();++i) if(consoles[i]->connections>0) ++playing;
    int open=0;
    { std::lock_guard<std::mutex> hold(DuplicatesLock); for(size_t i=0;i<Conflicts.size();++i) open+=Conflicts[i].open; }
    std::string text=Count(games,"game","games");
    if(playing) text+=", "+Count(playing,"console","consoles")+" connected";
    if(open) text+=", "+Count(open,"game","games")+" to choose a copy of";
    return text;
}

std::string ServerProblem() {
    std::lock_guard<std::mutex> hold(LogLock);
    return LogLines.empty() ? std::string() : LogLines.back().text.substr(9);
}

bool FirstTrayStart() {
    {
        std::lock_guard<std::mutex> hold(SettingsLock);
        if(Settings.toldTray) return false;
        Settings.toldTray=true;
    }
    Save();
    return true;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    int tray=TrayMain(argc,argv);
    if(tray>=0) return tray;
#endif
    std::vector<std::string> args(argv+1,argv+argc);
    int code=RunServer(args,"",false);
#ifdef _WIN32
    // Started from Explorer: keep the window open to show why it stopped.
    DWORD processes[2];
    if(code && GetConsoleProcessList(processes,2)==1) { printf("Press Enter to close."); getchar(); }
#endif
    return code;
}
