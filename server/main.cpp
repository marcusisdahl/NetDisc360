// SPDX-License-Identifier: MIT
// NetDisc360, a NetISO server for Aurora: one program in place of
// netiso-srv. It serves the games in folders (and their subfolders) to the
// NetISO plugin, noticing new ones without a restart, and keeps the console's
// Hdd1:\NetDisc360\Games game folders in step with them over Aurora's FTP server, with
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
#include "saves.h"
#include "sync.h"
#include "installed.h"
#include "system.h"
#include "titleupdates.h"
#include "update.h"
#include "version.h"
#include "wake.h"
#include "web.h"
#include <algorithm>
#include <atomic>
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
enum { Ping = 0, GetIsoSize = 1, HasType1File = 2, ReadData = 3, GetIsoName = 4, MountIso = 5, Header = 20,
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

// Where NetDisc360 keeps its files on a console (see shared/helper.h): the
// game folders in the Xbox folder (Options::xboxFolder, DefaultXboxFolder
// unless set), which Aurora scans on its own, and NetDisc360's own files (the
// helper, wake.ini, the filter's markers) in XboxAppFolder. Both were in
// OldXboxFolder before; MoveOldFolder moves a console's tiles from there.
const char DefaultXboxFolder[]="/Hdd1/NetDisc360/Games", OldXboxFolder[]="/Hdd1/NetISO", HddAppFolder[]="/Hdd1/Apps/NetDisc360";

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
    // Aurora's web server (Aurora's settings, HTTP server), to start games and restart Aurora.
    std::string auroraWebUser, auroraWebPassword;
    int auroraWebPort;
    bool followAddress;  // NetISO.xex.txt names this computer again when its address changed (FollowAddress).
    bool keepAwake;      // Windows doesn't sleep while a console plays (AwakeLoop).
    int autoOffMinutes;  // A console is turned off this long after its last game ended (AutoOff); 0: never.
    int xbdmPort;        // The consoles' debug monitor (xbdm), which turns them off; --xbdm-port for the tests.
    // Wake-on-LAN (RefreshWake): whether each console gets wake.ini, so its
    // launcher can wake this computer, and the MAC address to wake instead
    // of the adapter's own ("" for that).
    bool wake;
    std::string wakeMac;
    // Game saves (SaveLoop): backed up after each game, keeping the newest
    // so many of each game; saveDelay: seconds after the console is back in Aurora.
    bool saveBackup;
    int saveKeep;
    double saveDelay;
    // Saves follow the profile (FollowConsole): newer saves of a profile go
    // onto a console that has that profile when it's in Aurora.
    bool saveFollow;
    // Sync between computers (SyncLoop, sync.h): the sync Worker's address,
    // this computer's key there (kept protected like the passwords), and its
    // device id and name there. No key: no sync.
    std::string syncUrl, syncKey, syncId, syncName;
    std::string xboxUnity;  // XboxUnity's API (see titleupdates.h); --xboxunity for the tests.
    // How a console the dashboard sets nothing for gets each game (see TileChoice): folder, god or none.
    std::string tileMode;
    Options() : ftpUser("xboxftp"), ftpPassword("xboxftp"), xboxFolder(DefaultXboxFolder), aurora("/Hdd1/Apps/Aurora"), webAddress("127.0.0.1"),
                port(IsvrPort), ftpPort(21), webPort(WebPort), push(true), filter(true), scan(true), verbose(false), pushOnce(false),
                webLan(false), setupDone(false), wizardDone(false), toldTray(false), onlineWait(120), removeWait(60), pingSeconds(30),
                backupEvery(7), backupKeep(5), updateCheck(true), updateAuto(false), updatePrerelease(false),
                skipInstalled(true), installedFolders("/Hdd1/Games"), auroraScripts(true), restartAurora(true),
                auroraWebUser("xboxhttp"), auroraWebPassword("xboxhttp"), auroraWebPort(9999), followAddress(true), keepAwake(true), autoOffMinutes(0), xbdmPort(XbdmPort),
                wake(true), saveBackup(true), saveKeep(10), saveDelay(10), saveFollow(true),
                xboxUnity("https://xboxunity.net/Resources/Lib"), tileMode("folder") {}
};
// The dashboard changes the settings while the server runs, so they're read
// with Current() (a copy) and changed under SettingsLock.
Options Settings;
std::mutex SettingsLock;
Options Current() { std::lock_guard<std::mutex> hold(SettingsLock); return Settings; }
// 8 hex digits, as Title IDs and Media IDs are written, and back (ok: it was).
DWORD HexName8(const std::string& text, bool& ok) {
    ok=text.size()==8;
    DWORD v=0;
    for(size_t i=0;i<text.size() && ok;++i) {
        char c=text[i];
        int d=c>='0' && c<='9' ? c-'0' : c>='a' && c<='f' ? c-'a'+10 : c>='A' && c<='F' ? c-'A'+10 : -1;
        if(d<0) ok=false; else v=(v<<4)|DWORD(d);
    }
    return v;
}
std::string Hex8(DWORD v) { char text[12]; _snprintf_s(text,sizeof(text),_TRUNCATE,"%08X",(unsigned)v); return text; }


// NetDisc360's own folder on a console: Hdd1:\Apps\NetDisc360, or, when the
// game folders aren't on the hard drive, the NetDisc360 folder next to them
// (as in Hdd1:\NetISO before). The tile launcher looks in both.
std::string XboxAppFolder(const Options& o) {
    return !_strnicmp(o.xboxFolder.c_str(),"/Hdd1/",6) ? std::string(HddAppFolder) : o.xboxFolder+"/"+HelperFolder;
}
// Whether the game folders are where NetDisc360 puts them unless told
// otherwise, so a console's tiles in OldXboxFolder are moved there.
bool DefaultLayout(const Options& o) { return !_stricmp(o.xboxFolder.c_str(),DefaultXboxFolder); }

std::vector<unsigned char> Launcher, Boot;
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
    // Its Games on Demand tiles (see GodTiles): their game folders in
    // GodHome, by the listed name of their disc (lower case), and the
    // packages of NetDisc360's that it has.
    std::map<std::string,std::string> godTiles;
    std::vector<InstalledGame> godPackages;
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
                                 answeredAt(-1), offlineAt(0), pingAt(0), suspectAt(0), marked(false), gameTitle(0), gameSince(0), gameEndedAt(0), pushDone(0), pushTotal(0) {
        AddressText(at,text,sizeof(text));
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

// Held while talking to a console's Aurora FTP server, so a save backup and
// its push thread don't use it at once.
std::mutex& FtpLockFor(DWORD address) {
    static std::mutex lock;
    static std::map<DWORD,std::mutex*> locks;
    std::lock_guard<std::mutex> hold(lock);
    std::mutex*& m=locks[address];
    if(!m) m=new std::mutex;
    return *m;
}
void QueueSaves(DWORD address, const std::string& game);
void SyncSoon();
void FollowStep();
void FollowSoon(DWORD address);
void FollowAllSoon();
void SyncJson(::web::Json& j, const struct Options& o);
void ConflictsJson(::web::Json& j);
void RefreshWake(const Console& console, FtpClient& ftp, const Options& o);

// A console that has used the server, kept in the settings file so the
// dashboard knows it after a restart.
struct KnownConsole { std::string address; long long seen; };
std::vector<KnownConsole> Known;  // Under SettingsLock.

// How a console gets each game, as its "Games on this console" on the
// dashboard says: folder, a game folder in xboxFolder (Hdd1:\NetISO), as
// always; god, a small Games on Demand package of the game's tile launcher
// (see GodTiles), which the Xbox's own menu lists as well as Aurora, with
// the game folder it starts from kept where Aurora doesn't look; none, not on
// that console. A console's mode ("": the default, Options' tileMode) goes
// for its games but those it names (by their listed name, lower case).
const char TileFolder[]="folder", TileGod[]="god", TileNone[]="none";
bool IsTileMode(const std::string& mode) { return mode==TileFolder || mode==TileGod || mode==TileNone; }
struct TileChoice { std::string mode; std::map<std::string,std::string> games; };
std::map<std::string,TileChoice> TileChoices;  // By the console's address, under SettingsLock.
// What a console gets of a game (its listed name), under SettingsLock.
std::string TileModeOf(const Options& o, const TileChoice* choice, const std::string& name) {
    if(choice) {
        std::map<std::string,std::string>::const_iterator game=choice->games.find(Lower(name));
        if(game!=choice->games.end()) return game->second;
        if(!choice->mode.empty()) return choice->mode;
    }
    return IsTileMode(o.tileMode) ? o.tileMode : std::string(TileFolder);
}
// This computer's addresses as far back as the settings file remembers
// (own_addresses), newest last, so a NetISO.xex.txt naming an address it had
// is told from one naming another server (see FollowAddress). Under SettingsLock.
std::vector<std::string> OwnAddresses;
const size_t OwnAddressesKept = 16;

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

// plays.ini: a line "times|seconds|last|name" for each game (last: time()).
void ReadPlays(const std::string& text, std::map<std::string,Played>& plays) {
    for(size_t at=0;at<text.size();) {
        size_t end=text.find('\n',at);
        std::string line=text.substr(at,end==std::string::npos ? std::string::npos : end-at);
        at=end==std::string::npos ? text.size() : end+1;
        if(!line.empty() && line[line.size()-1]=='\r') line.erase(line.size()-1);
        Played p={0,0,0};
        int name=0;
        if(sscanf(line.c_str(),"%d|%lf|%lld|%n",&p.times,&p.seconds,&p.last,&name)<3 || !name || !line[name]) continue;
        plays[line.substr(size_t(name))]=p;
    }
}

void LoadPlays(const std::string& configPath) {
    std::string path=PlaysPath(configPath), text;
    if(path.empty() || !config::ReadFile(path,text,16<<20)) return;
    std::lock_guard<std::mutex> hold(PlaysLock);
    ReadPlays(text,Plays);
}

// The play history of this computer's other devices in the sync (their
// plays.ini, kept in Sync\devices\<id>\), added up: the dashboard counts
// it with this computer's own. Under PlaysLock.
std::map<std::string,Played> OtherPlays;

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

// Saves the play history, and has the dashboard read the games again.
void PlaysChanged() {
    SavePlays();
    std::lock_guard<std::mutex> hold(GamesListLock);
    ++GamesVersion;
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
    return true;
}

// console mounted game (its listed name), or started it again from the disc.
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

// Tells when a console in a game is back in Aurora: its FTP port is tried
// (without logging in) FirstProbeSeconds after the mount and then every
// GameProbeSeconds, only while it's in a game, and answering means Aurora
// runs again. A game started from the disc in Aurora shows by its reads.
void AuroraLoop() {
    for(;;) {
        Options o=Current();
        int port=o.ftpPort;
        std::vector<Console*> consoles=AllConsoles();
        for(size_t i=0;i<consoles.size();++i) {
            Console& c=*consoles[i];
            AutoOff(c,o);
            if(c.connections<=0 || c.inAurora || c.offline || Now()*1000<c.probeAt) continue;
            std::string game;
            { std::lock_guard<std::mutex> hold(ConsoleLock); game=WithoutIso(c.mounted); }
            if(game.empty()) continue;
            c.probeAt=(long long)((Now()+GameProbeSeconds)*1000);
            FtpClient ftp(c.address,port,"","",3);
            if(!ftp.Greets()) continue;
            c.sentInAurora=c.sent.load();
            c.inAurora=true;
            c.ftp=2;
            XboxLog(c,"back in Aurora after %s (its FTP server answers).",game.c_str());
            PlayEnded(c);
            c.gameEndedAt=(long long)(Now()*1000);
            XboxEvent(c,EventXbox,"Back in Aurora after "+game,std::string(c.text)+" is back in Aurora after "+game);
            { std::lock_guard<std::mutex> hold(ConsoleLock); game=c.mounted; }
            QueueSaves(c.address,game);
            FollowSoon(c.address);
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
    PlayEnded(c);
    c.gameEndedAt=0;
    XboxLog(c,"offline: the last %d pings got no answer%s.",OfflinePings,c.suspectAt ? ", and its NetISO connection ended" : "");
    c.suspectAt=0;
    XboxEvent(c,EventXbox,"Went offline",std::string(c.text)+" went offline");
}

void BackOnline(Console& c, const char* how) {
    c.offline=false;
    FollowSoon(c.address);
    c.missed=0;
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
            if(!console_) Attach();
            int command=(request[4]<<8)|request[5];
            DWORD index=DWORD((request[6]<<8)|request[7]);
            U64 offset=(U64(Read32(request+8))<<32)|Read32(request+12);
            DWORD length=Read32(request+16);
            if(Settings.verbose) Log("%s: command %d, index %u, offset %llu, length %u",peer_,command,index,(unsigned long long)offset,length);
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
        ++console_->connections;
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
// As servers before the NetDisc360 folders wrote it; the server knew its own
// edit by these exact words, so they keep the name as it was then ("NetDisc 360").
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
// This server's edit. The tiles are in Hdd1:\NetDisc360\Games, or in
// Hdd1:\NetISO on a console whose tiles haven't moved yet (MoveOldFolder).
const char FilterNetDisc[] =
    "function GameListFilterXbox360(Content)\n"
    "   -- Add only legit xbox 360 games\n"
    "   return Content.Group == ContentGroup.Xbox360 or GameListFilterNetIso(Content)\n"
    "end\n"
    "\n"
    "-- NetDisc360: its game tiles in Hdd1:\\NetDisc360\\Games (Hdd1:\\NetISO before)\n"
    "-- play retail discs from the PC, so they count as Xbox 360 games here.\n"
    "-- Aurora files them as Unsigned because they aren't retail-signed. The\n"
    "-- NetISO Sync app itself stays out, and so do install discs, which their\n"
    "-- game's tile starts (NetDiscInstallDisc is in User\\Scripts\\Content\\Filters).\n"
    "function GameListFilterNetIso(Content)\n"
    "   local directory = '\\\\' .. string.lower(Content.Directory or '')\n"
    "   return (string.find(directory, '\\\\netdisc360\\\\games\\\\', 1, true) ~= nil or string.find(directory, '\\\\netiso\\\\', 1, true) ~= nil)\n"
    "      and string.find(directory, '\\\\netiso sync', 1, true) == nil\n"
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
    return ftp.Store(path.c_str(),data.data(),data.size()) && ftp.Retrieve(path.c_str(),back,data.size()+1) &&
           back.size()==data.size() && std::equal(back.begin(),back.end(),data.begin());
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
    if(text.find(Lines(FilterNetDisc,crlf))!=std::string::npos) {
        if(announce) XboxLog(console,"Aurora's Xbox 360 category includes the NetDisc360 games.");
        return;
    }
    // An earlier server's edit gets this one's, without a new backup.
    const char* const earlier[]={FilterNetIso,FilterNetIsoBefore};
    size_t at;
    for(int k=0;k<2;++k) {
        std::string before=Lines(earlier[k],crlf);
        at=text.find(before);
        if(at==std::string::npos) continue;
        std::string edited=text.substr(0,at)+Lines(FilterNetDisc,crlf)+text.substr(at+before.size());
        if(StoreChecked(ftp,path,edited))
            XboxLog(console,"updated NetDisc360's rule in Aurora's Xbox 360 category (%s): it takes the games in %s and leaves install discs out. Restart Aurora to see it.",
                    path.c_str(),o.xboxFolder.c_str());
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
                std::string whole=first.substr(0,from)+Lines(FilterNetDisc,keptCrlf)+first.substr(from+firstOriginal.size());
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
    std::string edited=text.substr(0,at)+Lines(FilterNetDisc,crlf)+text.substr(at+original.size());
    if(!StoreChecked(ftp,backup,text) || !StoreChecked(ftp,path,edited)) {
        XboxLog(console,"couldn't update Aurora's Xbox 360 category: %s",ftp.Error());
        XboxEvent(console,EventWarning,"Couldn't add the games to Aurora's Xbox 360 category");
        StoreChecked(ftp,path,text);  // The original back, should the edit have got there cut short.
        return;
    }
    XboxLog(console,"added the NetDisc360 games to Aurora's Xbox 360 category (%s; the original is kept as %s). Restart Aurora to see it.",path.c_str(),backup.c_str());
    XboxEvent(console,EventGood,"Games added to Aurora's Xbox 360 category; restart Aurora to see them there",
              std::string("Games added to Aurora's Xbox 360 category on ")+console.text+"; restart Aurora to see them there");
}

// Install and content discs (see discmap.h) are started from their game's
// tile, which offers the install, so Aurora needn't list their own tiles.
// This user filter of ours hides them (Filters, User, "Hide install discs"),
// and the Xbox 360 category leaves them out (FilterNetDisc). They are told
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
void EnsureFolders(FtpClient& ftp, const std::string& folder);
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

// Aurora's art for NetDisc360 and FastFTP360 ---------------------------------

// Aurora looks a title's cover up on XboxUnity by its Title ID, and XboxUnity
// has no entry for these two homebrew apps (only its admins add homebrew
// titles), so Aurora keeps empty placeholders for them in its GameData
// folder: <Title ID>_<n>/GC (cover), BK (background) and GL (icon and banner)
// .asset files of 2048 bytes. The server fills those in with the art from
// art/aurora (see tools/aurora_asset.py). Art that is already there, whether
// Aurora's or someone's own, is left alone.
struct AppArt { DWORD titleId; const char* app; const char* folder; int ids[3]; std::vector<unsigned char> files[3]; };
AppArt AppArts[] = {
    {0xFFFF4E49,"NetDisc360","netdisc360",{IDR_ART_NETDISC_GC,IDR_ART_NETDISC_BK,IDR_ART_NETDISC_GL}},
    {0xFFFF4646,"FastFTP360","fastftp360",{IDR_ART_FASTFTP_GC,IDR_ART_FASTFTP_BK,IDR_ART_FASTFTP_GL}},
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
    int n=0;
    for(size_t i=0;i<install->packages.size();++i) {
        if(install->packages[i].titleId!=game->titleId) continue;  // Only what this game needs.
        char line[IsvrNameBytes+64];
        _snprintf_s(line,sizeof(line),_TRUNCATE,"package_%d=%s|%llu\r\n",++n,install->packages[i].path.c_str(),
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

// How the console being pushed to gets each game (see TileChoice), under SyncLock.
Options PushOptions;
TileChoice PushChoice;
bool PushChosen;
// The games left out of the push's game folders since they're installed,
// and since they're Games on Demand tiles or none.
int PushLeftInstalled, PushLeftOther, PushWantGod;
std::vector<std::pair<std::string,std::string> > PushGodTiles;  // Listed name, game folder in GodHome.
// An original Xbox game has no Xbox 360 package to be: it keeps its folder.
std::string PushMode(const char* iso) {
    std::string mode=TileModeOf(PushOptions,PushChosen ? &PushChoice : NULL,iso);
    if(mode!=TileGod) return mode;
    std::lock_guard<std::mutex> hold(GamesListLock);
    for(size_t i=0;i<GamesList.size();++i) if(!_stricmp(GamesList[i].name.c_str(),iso)) return GamesList[i].xbe ? std::string(TileFolder) : mode;
    return mode;
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
// The game folders in xboxFolder: the games that get one, and aren't installed.
bool WantedFolder(const char* iso) {
    if(!WantedFor(iso)) { ++PushLeftInstalled; return false; }
    std::string mode=PushMode(iso);
    if(mode==TileGod) ++PushWantGod;
    if(mode!=TileFolder) { ++PushLeftOther; return false; }
    return true;
}
// Those in GodHome, of the Games on Demand tiles.
bool WantedGod(const char* iso) { return WantedFor(iso) && PushMode(iso)==TileGod; }
void PlacedGod(const char* iso, const char* folder) { PushGodTiles.push_back(std::make_pair(std::string(iso),std::string(folder))); }

// Games on Demand tiles ----------------------------------------------------------
//
// A game a console gets as Games on Demand (TileGod) has its game folder, made
// by Sync as every game folder is, in GodHome, where Aurora doesn't look, and
// a package in <drive>/Content/0000000000000000/<Title ID>/00007000 that holds
// two files: default.xex, the game's tile launcher, and netiso.ini, whose
// tile= names that game folder. The console takes the package for the game
// installed, so its own menu lists it as well as Aurora, by the name, Title ID
// and Media ID in its header. Started, the tile launcher (launcher/tile.cpp)
// starts the game folder's default.xex, and from there it goes as a tile in
// Aurora does: the plugin sees the tile's start, the helper mounts the disc
// from this server. The package is a few hundred KB; its header's name
// (GodTileName) tells it from a real copy of the game.

std::string FtpDrive(const std::string& path) {
    size_t slash=path.find('/',1);
    return slash==std::string::npos ? path : path.substr(0,slash);
}
std::string GodHome(const Options& o) { return FtpDrive(o.xboxFolder)+"/NetDisc360/GOD"; }
std::string GodContent(const Options& o) { return FtpDrive(o.xboxFolder)+"/Content/0000000000000000"; }

// The storage Sync writes through, keeping the default.xex last read or
// written in each folder, so a Games on Demand tile is made from its game
// folder's launcher without reading it again.
class KeptLaunchers : public Storage {
public:
    explicit KeptLaunchers(Storage& inner) : inner_(inner) {}
    int ListFolders(const char* root, char names[][FolderNameMax + 1], int capacity) { return inner_.ListFolders(root,names,capacity); }
    bool ReadFile(const char* path, unsigned char* data, DWORD capacity, DWORD& size) {
        bool ok=inner_.ReadFile(path,data,capacity,size);
        if(ok) Keep(path,data,size);
        return ok;
    }
    bool Missing() { return inner_.Missing(); }
    bool WriteFile(const char* path, const void* data, DWORD size) {
        bool ok=inner_.WriteFile(path,data,size);
        if(ok) Keep(path,static_cast<const unsigned char*>(data),size);
        return ok;
    }
    bool MakeFolder(const char* path) { return inner_.MakeFolder(path); }
    bool RemoveGame(const char* folder) { return inner_.RemoveGame(folder); }
    int RemoveFile(const char* path) { return inner_.RemoveFile(path); }
    unsigned long LastError() { return inner_.LastError(); }
    std::map<std::string,std::vector<unsigned char> > launchers;  // By folder (lower case).
private:
    void Keep(const char* path, const unsigned char* data, DWORD size) {
        std::string p(path);
        size_t slash=p.find_last_of('/');
        if(slash!=std::string::npos && Lower(p.substr(slash+1))=="default.xex") launchers[Lower(p.substr(0,slash))].assign(data,data+size);
    }
    Storage& inner_;
};

bool MakeFolders(FtpClient& ftp, const std::string& path, size_t from, std::string& error);
bool DeleteTree(FtpClient& ftp, const std::string& folder, int depth, std::string& error);
std::string ConsolePath(const std::string& ftpPath);
std::string SizeText(U64 n);

// Makes the package of a game folder's tile (its default.xex) in god.
// What the Xbox home shows of a Games on Demand tile: its header's name and
// icon. The name is Aurora's for the title (the game's own, as Aurora lists
// its folder tile or package), else XboxUnity's, else the ISO's without its
// release tags; the icon is XboxUnity's, the one Aurora shows. Each title is
// asked of XboxUnity once while the server runs, and not at all for
// GodArtRetrySeconds after it couldn't be reached.
struct GodArt { std::string name; std::vector<unsigned char> icon; };
std::mutex GodArtLock;
std::map<DWORD,GodArt> UnityArt;            // Titles XboxUnity answered for.
std::map<DWORD,std::string> AuroraTitles;   // Aurora's names, from consoles' content.db.
time_t UnityDownAt=0;
const double GodArtRetrySeconds=600;
extern const char AuroraContentDb[];

// XboxUnity's name and icon of a title; false if it couldn't be reached.
bool AskUnityArt(const Options& o, DWORD titleId, GodArt& art) {
    std::string body, file, error;
    if(!tu::Get(o.xboxUnity+"/TitleList.php?page=0&count=10&search="+Hex8(titleId)+"&sort=3&direction=1&category=0&filter=0",body,1<<20,file,error)) {
        Log("XboxUnity didn't answer for the name of %08X: %s",unsigned(titleId),error.c_str());
        return false;
    }
    web::Value v;
    const web::Value* items=web::Parse(body,v) && v.type==web::Value::Object ? v.Get("Items") : NULL;
    for(size_t i=0;items && items->type==web::Value::Array && i<items->items.size() && art.name.empty();++i) {
        const web::Value* id=items->items[i].Get("TitleID");
        const web::Value* name=items->items[i].Get("Name");
        if(id && id->type==web::Value::String && !_stricmp(id->text.c_str(),Hex8(titleId).c_str()) && name && name->type==web::Value::String) art.name=name->text;
    }
    // A PNG, which the header's 0x4000 bytes hold; XboxUnity has no icon for some titles.
    std::vector<unsigned char> icon;
    if(tu::Get(o.xboxUnity+"/Icon.php?tid="+Hex8(titleId),body,1<<20,file,error) && body.size()>8 && body.size()<=0x4000 && !memcmp(body.data(),"\x89PNG\r\n\x1a\n",8))
        art.icon.assign(body.begin(),body.end());
    return true;
}

// The name and icon a tile of titleId gets; listed is its listed name.
GodArt GodArtFor(const Options& o, DWORD titleId, const std::string& listed) {
    GodArt art;
    bool ask=false;
    {
        std::lock_guard<std::mutex> hold(GodArtLock);
        std::map<DWORD,GodArt>::const_iterator known=UnityArt.find(titleId);
        if(known!=UnityArt.end()) art=known->second;
        else ask=!o.xboxUnity.empty() && (!UnityDownAt || difftime(time(NULL),UnityDownAt)>=GodArtRetrySeconds);
    }
    if(ask) {
        GodArt asked;
        bool answered=AskUnityArt(o,titleId,asked);
        std::lock_guard<std::mutex> hold(GodArtLock);
        if(answered) { UnityArt[titleId]=asked; art=asked; UnityDownAt=0; }
        else UnityDownAt=time(NULL);
    }
    std::lock_guard<std::mutex> hold(GodArtLock);
    std::map<DWORD,std::string>::const_iterator aurora=AuroraTitles.find(titleId);
    if(aurora!=AuroraTitles.end()) art.name=aurora->second;
    if(art.name.empty()) { char friendly[512]; FriendlyGameName(listed.c_str(),friendly,sizeof(friendly)); art.name=config::ToUtf8(friendly); }
    return art;
}

// Reads Aurora's names of the titles on the console from its content.db, for
// GodArtFor. Not those of NetDisc360's own packages, whose name may be the
// one their header gave (and an older build's was the ISO's).
void ReadAuroraTitles(FtpClient& ftp, const Options& o) {
    std::vector<unsigned char> db;
    std::vector<AuroraItem> items;
    std::string why;
    if(!ftp.Retrieve((o.aurora+AuroraContentDb).c_str(),db,size_t(256)<<20) || !ReadAuroraItems(db,items,why)) return;
    std::map<DWORD,std::string> found;
    for(size_t i=0;i<items.size();++i) {
        const AuroraItem& item=items[i];
        if(!item.titleId || item.titleName.empty() || found.count(item.titleId)) continue;
        std::string directory=item.directory;
        while(!directory.empty() && (directory[directory.size()-1]=='\\' || directory[directory.size()-1]=='/')) directory.erase(directory.size()-1);
        size_t slash=directory.find_last_of("\\/");
        if(IsGodTileName(slash==std::string::npos ? directory : directory.substr(slash+1))) continue;
        found[item.titleId]=item.titleName;
    }
    std::lock_guard<std::mutex> hold(GodArtLock);
    for(std::map<DWORD,std::string>::const_iterator i=found.begin();i!=found.end();++i) AuroraTitles[i->first]=i->second;
}

bool MakeGodTile(const std::vector<unsigned char>& xex, const std::string& folder, const std::string& title,
                 const std::vector<unsigned char>& icon, IsoReader& reader, GodPackage& god, std::string& error) {
    DWORD titleId=0, mediaId=0; int disc=0;
    if(xex.size()<24 || !XexIds(&xex[0],DWORD(xex.size()),titleId,mediaId,disc) || !titleId) { error="its default.xex has no Title ID"; return false; }
    god.SetIcon(icon);
    std::string ini="; Written by NetDisc360: this Games on Demand package starts the game folder named here.\r\ntile="+ConsolePath(folder)+"\r\n";
    std::vector<std::pair<std::string,std::vector<unsigned char> > > files;
    files.push_back(std::make_pair(std::string("default.xex"),xex));
    files.push_back(std::make_pair(std::string("netiso.ini"),std::vector<unsigned char>(ini.begin(),ini.end())));
    if(!reader.Open(files)) { error=reader.Note(); return false; }
    if(!god.Make(reader,title,[](U64, U64) { return true; },error)) return false;
    god.SetName(GodTileName(mediaId,disc));
    return true;
}

// Brings the console's Games on Demand tiles in line with the game folders
// Sync put in GodHome (tiles: listed name, folder): a package that isn't
// there as made here is written, its Data file first and its header last, so
// a package cut short is never listed; NetDisc360's packages (had) of games
// without a folder there are deleted. changed counts both; false if any
// couldn't be, which the next push tries again.
bool GodTiles(const Console& console, FtpClient& ftp, const Options& o, const std::vector<std::pair<std::string,std::string> >& tiles,
              const std::map<std::string,std::vector<unsigned char> >& launchers, const std::vector<InstalledGame>& had, int& changed) {
    changed=0;
    bool ok=true;
    std::string content=GodContent(o);
    std::set<std::string> keep, there;
    for(size_t i=0;i<had.size();++i) there.insert(Lower(had[i].path));
    if(!tiles.empty()) ReadAuroraTitles(ftp,o);
    for(size_t i=0;i<tiles.size();++i) {
        std::string folder=GodHome(o)+"/"+tiles[i].second;
        std::map<std::string,std::vector<unsigned char> >::const_iterator kept=launchers.find(Lower(folder));
        std::vector<unsigned char> xex;
        if(kept!=launchers.end()) xex=kept->second;
        else if(!ftp.Retrieve((folder+"/default.xex").c_str(),xex,size_t(16)<<20)) {
            XboxLog(console,"couldn't read %s/default.xex for its Games on Demand tile: %s",folder.c_str(),ftp.Error());
            ok=false; continue;
        }
        DWORD titleId=0, mediaId=0; int disc=0;
        GodArt art;
        if(xex.size()>=24 && XexIds(&xex[0],DWORD(xex.size()),titleId,mediaId,disc) && titleId) {
            art=GodArtFor(o,titleId,tiles[i].first);
            // Each disc of a set its own name, as the Xbox home lists each.
            DWORD info=ExecutionInfoOffset(&xex[0],DWORD(xex.size()));
            std::string number="disc "+std::to_string(disc);
            if(info && info+ExecutionInfoSize<=xex.size() && xex[info+19]>1 && disc>0 && Lower(art.name).find(number)==std::string::npos)
                art.name+=" (Disc "+std::to_string(disc)+")";
        }
        IsoReader reader; GodPackage god; std::string error;
        if(!MakeGodTile(xex,folder,art.name,art.icon,reader,god,error)) {
            XboxLog(console,"couldn't make the Games on Demand tile of %s: %s.",tiles[i].first.c_str(),error.c_str());
            ok=false; continue;
        }
        std::string packages=content+"/"+Hex8(god.TitleId())+"/00007000", header=packages+"/"+god.Name(), data=header+".data";
        keep.insert(Lower(header));
        // As made here: its header's start, whose digest covers the rest of
        // it and so the hashes of the Data file, is the same.
        std::vector<unsigned char> head;
        size_t compare=std::min(god.Header().size(),size_t(0x400));
        if(there.count(Lower(header)) && ftp.RetrieveHead(header.c_str(),head,compare) && head.size()==compare && !memcmp(&head[0],&god.Header()[0],compare)) continue;
        bool written=MakeFolders(ftp,data+"/",FtpDrive(o.xboxFolder).size(),error);
        for(int part=0;part<god.Parts() && written;++part) {
            char name[16]; _snprintf_s(name,sizeof(name),_TRUNCATE,"/Data%04d",part);
            written=ftp.StoreFrom((data+name).c_str(),god.PartSize(part),[&](U64 offset, unsigned char* bytes, DWORD length) { return god.Part(part,offset,bytes,length); },
                                  [](U64) { return true; });
            if(!written) error=ftp.Error();
        }
        if(written && !ftp.Store(header.c_str(),god.Header().data(),god.Header().size())) { written=false; error=ftp.Error(); }
        if(!written) { XboxLog(console,"couldn't write the Games on Demand tile of %s (%s): %s",tiles[i].first.c_str(),header.c_str(),error.c_str()); ok=false; continue; }
        XboxLog(console,"put %s in as Games on Demand (%s, %s), named \"%s\"%s, which starts %s.",WithoutIso(tiles[i].first).c_str(),header.c_str(),
                SizeText(god.Bytes()).c_str(),art.name.c_str(),art.icon.empty() ? " with no icon" : " with its icon",folder.c_str());
        ++changed;
    }
    for(size_t i=0;i<had.size();++i) {
        const std::string& header=had[i].path;
        if(keep.count(Lower(header))) continue;
        std::string error;
        bool gone=ftp.Delete(header.c_str()) || ftp.Missing();
        if(!gone) error=ftp.Error();
        std::string data=header+".data";
        if(gone && ftp.Enters(data.c_str())) gone=DeleteTree(ftp,data,0,error);
        std::string parent=header.substr(0,header.find_last_of('/'));
        std::vector<FtpEntry> left;
        if(gone && ftp.List(parent.c_str(),left) && left.empty()) ftp.RemoveFolder(parent.c_str());
        if(!gone) { XboxLog(console,"couldn't delete the Games on Demand tile %s: %s",header.c_str(),error.c_str()); ok=false; continue; }
        XboxLog(console,"deleted the Games on Demand tile %s: its game isn't one on this console any more.",header.c_str());
        ++changed;
    }
    return ok;
}

bool Push(Console& console, FtpClient& ftp, SyncResult& result, bool removeGone, const Options& o, const std::vector<InstalledGame>& installed,
          const std::vector<InstalledGame>& godPackages) {
    std::vector<FtpEntry> entries;
    // A level at a time: Aurora's FTP makes no parent folders (/Hdd1/NetDisc360/Games on a console without NetDisc360).
    EnsureFolders(ftp,o.xboxFolder);
    if(!ftp.List(o.xboxFolder.c_str(),entries)) {
        XboxLog(console,"couldn't list %s: %s",o.xboxFolder.c_str(),ftp.Error());
        XboxEvent(console,EventWarning,"Couldn't open "+o.xboxFolder);
        return false;
    }
    RefreshSyncApp(console,ftp,entries,o);
    IsvrClient client(30);
    if(!client.Connect(htonl(0x7F000001),o.port,5) || !client.Ping()) { Log("Couldn't reach this server itself: %s",client.Error()); return false; }
    FtpStorage ftpStorage(ftp);
    KeptLaunchers storage(ftpStorage);
    bool ok, godOk=true;
    std::string reason;
    std::map<std::string,std::string> tiles, godFolders;
    std::vector<std::pair<std::string,std::string> > godTiles;
    SyncResult god;
    memset(&god,0,sizeof(god));
    int leftInstalled=0, leftOther=0;
    {
        std::lock_guard<std::mutex> hold(SyncLock);
        Pushing=&console; LastPushNote.clear();
        PushInstalled.clear();
        PushSkip=o.skipInstalled;
        for(size_t i=0;i<installed.size();++i) {
            std::string path=ConsolePath(installed[i].path);
            if(!path.empty()) PushInstalled.insert(std::make_pair(std::make_pair(installed[i].titleId,installed[i].disc),path));
        }
        PushOptions=o;
        {
            std::lock_guard<std::mutex> settings(SettingsLock);
            std::map<std::string,TileChoice>::const_iterator c=TileChoices.find(console.text);
            PushChosen=c!=TileChoices.end();
            PushChoice=PushChosen ? c->second : TileChoice();
        }
        PushLeftInstalled=PushLeftOther=PushWantGod=0;
        PushTiles.clear(); PushGodTiles.clear();
        // The helper they share goes in NetDisc360's own folder (Hdd1:\Apps\NetDisc360).
        std::string app=XboxAppFolder(o);
        ok=Sync(client,storage,o.xboxFolder.c_str(),Launcher.data(),DWORD(Launcher.size()),
                Boot.empty() ? NULL : Boot.data(),DWORD(Boot.size()),PushNote,result,removeGone,InstallFor,RoleFor,WantedFolder,PlacedAt,PushProgress,
                app.c_str());
        reason=LastPushNote;
        leftInstalled=PushLeftInstalled; leftOther=PushLeftOther;
        result.installed=leftInstalled;
        // The Games on Demand tiles' game folders, when the console has any
        // to have, or had: Sync takes those it should no longer have away.
        bool had=false;
        std::vector<FtpEntry> home;
        if(ok && !PushWantGod && ftp.Enters(GodHome(o).c_str()) && ftp.List(GodHome(o).c_str(),home))
            for(size_t i=0;i<home.size() && !had;++i) had=home[i].folder && _stricmp(home[i].name.c_str(),HelperFolder);
        if(ok && (PushWantGod || had || !godPackages.empty())) {
            std::string error;
            if(!MakeFolders(ftp,GodHome(o)+"/",FtpDrive(o.xboxFolder).size(),error)) { XboxLog(console,"%s",error.c_str()); godOk=false; }
            else godOk=Sync(client,storage,GodHome(o).c_str(),Launcher.data(),DWORD(Launcher.size()),
                            Boot.empty() ? NULL : Boot.data(),DWORD(Boot.size()),PushNote,god,removeGone,InstallFor,RoleFor,WantedGod,PlacedGod,PushProgress,
                            app.c_str());
            if(!godOk) XboxLog(console,"updating the Games on Demand tiles' game folders stopped: %s",LastPushNote.c_str());
        }
        console.pushTotal=0; console.pushDone=0;
        tiles.swap(PushTiles);
        godTiles.swap(PushGodTiles);
    }
    int godChanged=0;
    if(ok && godOk) godOk=GodTiles(console,ftp,o,godTiles,storage.launchers,godPackages,godChanged);
    if(ok && godChanged) {
        std::string reply, content=GodContent(o);
        if(o.scan && ftp.Site(("INITIATESCAN "+content).c_str(),reply) && reply.find("Unable")==std::string::npos)
            XboxLog(console,"asked Aurora to scan %s, so it lists the Games on Demand tiles now.",content.c_str());
    }
    for(size_t i=0;i<godTiles.size();++i) godFolders[Lower(godTiles[i].first)]=GodHome(o)+"/"+godTiles[i].second;
    if(!godOk) ++result.failed;
    if(ok) {
        std::lock_guard<std::mutex> hold(ConsoleLock);
        console.tiles.swap(tiles);
        if(godOk) console.godTiles.swap(godFolders);
    }
    if(ok && (god.added || god.removed || godChanged))
        XboxLog(console,"Games on Demand tiles: %d game folder(s) added and %d removed in %s, %d package(s) written or deleted.",god.added,god.removed,GodHome(o).c_str(),godChanged);
    if(ok && leftOther-int(godTiles.size())>0)
        XboxLog(console,"%s get%s no tile: set so on the Consoles page.",Count(leftOther-int(godTiles.size()),"game","games").c_str(),leftOther-int(godTiles.size())==1 ? "s" : "");
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
// drive, with backslashes ("\NetDisc360\Games\Halo 3" for /Hdd1/NetDisc360/Games/Halo 3).
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
        if(!folder.folder || !_stricmp(folder.name.c_str(),HelperFolder) || !_stricmp(folder.name.c_str(),SyncFolder)) continue;
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
    double nextProbe, probedAt, retryAt, scanAt, downSince, upSince;
    int scanTries;
    bool attempted, reachable, toldDown, toldWaiting;
    std::atomic<bool> everPushed;
    std::atomic<bool> finished;  // --push-once: done with this console.
    InstalledGames installed;    // What was read of its installed games.
    std::string installedSeen;   // Their Title IDs and discs, as last logged.
    bool toldInstalled;          // Looking for them failed, and the log said so.
    bool restarted;              // Aurora was restarted for its filter once (see RefreshAuroraScripts).
    bool moveChecked;            // Its tiles in OldXboxFolder were looked for, and moved (MoveOldFolder).
    bool moveRestart;            // Tiles were moved, and Aurora is to restart once its scripts are written.
    bool movedRestarted;         // Aurora was restarted once its rows had followed (FinishMove).
    std::string followed;        // The NetISO.xex.txt address the log said it left alone (see FollowAddress).
    PushState() : nextProbe(0), probedAt(0), retryAt(0), scanAt(0), downSince(0), upSince(0), scanTries(0),
                  attempted(false), reachable(false), toldDown(false), toldWaiting(false), everPushed(false), finished(false),
                  toldInstalled(false), restarted(false), moveChecked(false), moveRestart(false), movedRestarted(false) {}
};

// Aurora's FTP server takes one upload at a time: two at once leave files
// the console can't read (TinyXbox360BackupManager found this). So a
// console's game folders wait while a game or its content is copied there,
// and a copy waits for a push that has started (see WaitForPush).
struct PushMark {
    DWORD address; bool marked;
    explicit PushMark(DWORD at);
    ~PushMark();
};
// The games installed on the console, for its push and the dashboard (see installed.h).
void LookInstalled(Console& console, PushState& s, FtpClient& ftp, const Options& o, std::vector<InstalledGame>& found, std::vector<InstalledGame>& godPackages) {
    std::string error;
    std::vector<std::string> skip;
    skip.push_back(o.xboxFolder); skip.push_back(GodHome(o));
    bool ok=s.installed.Find(ftp,InstalledFolders(o.installedFolders),skip,found,godPackages,error);
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
    std::lock_guard<std::mutex> hold(ConsoleLock);
    console.installed=found;
    console.godPackages=godPackages;
}

// Makes folder unless it's there (a failed MKD costs FtpDll's connection).
void EnsureFolder(FtpClient& ftp, const std::string& folder) {
    if(!ftp.Enters(folder.c_str()) && ftp.Code()/100==5) ftp.MakeFolder(folder.c_str());
}
// folder and the folders it is in, below its drive (/Hdd1/NetDisc360 for
// /Hdd1/NetDisc360/Games).
void EnsureFolders(FtpClient& ftp, const std::string& folder) {
    std::string unused;
    MakeFolders(ftp,folder+"/",FtpDrive(folder).size(),unused);
}

// Restarts Aurora on the console through its web server, if Aurora is what it shows.
bool RestartAurora(const Console& console, const Options& o, const char* why) {
    AuroraWeb aurora(console.address,o.auroraWebPort,o.auroraWebUser,o.auroraWebPassword,5);
    if(!aurora.RestartAurora()) { XboxLog(console,"didn't restart Aurora %s: %s.",why,aurora.Error().c_str()); return false; }
    XboxLog(console,"restarted Aurora %s.",why);
    return true;
}

bool TuAuroraFollowUp(const Console& console, FtpClient& ftp, const Options& o);

// Aurora's cleanup script and "Hide PC games while NetDisc360 is off" filter
// (see aurora.h), and the marker the filter looks for, after a push. When the
// filter hides the PC games though this server runs (Aurora started while it
// didn't), Aurora is restarted, once, if it's on screen and restartAurora allows.
void RefreshAuroraScripts(Console& console, PushState& s, FtpClient& ftp, const Options& o) {
    std::vector<FtpEntry> entries;
    if(!ftp.List(o.xboxFolder.c_str(),entries)) return;
    std::vector<std::string> folders;
    for(size_t i=0;i<entries.size();++i) if(entries[i].folder) folders.push_back(entries[i].name);
    std::string helper=XboxAppFolder(o);
    EnsureFolders(ftp,helper);
    std::vector<FtpEntry> files;
    bool listed=ftp.List(helper.c_str(),files);
    // By the listing: a read of a file that isn't there can stall FtpDll's next one.
    bool marker=false, shown=false, moving=false;
    for(size_t i=0;i<files.size();++i) {
        if(!files[i].folder && !_stricmp(files[i].name.c_str(),OnlineMarker)) marker=true;
        if(!files[i].folder && !_stricmp(files[i].name.c_str(),ShownMarker)) shown=true;
        if(!files[i].folder && !_stricmp(files[i].name.c_str(),MoveMarker)) moving=true;
    }
    // Moved tiles whose rows in Aurora haven't followed yet: the scripts move them.
    std::string from=moving && DefaultLayout(o) ? std::string(OldXboxFolder) : std::string();
    std::string cleanup=CleanupScript(o.xboxFolder,folders,from,helper), online=OnlineScript(o.xboxFolder,helper,from,folders);
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
    if(!listed) return;
    if(!marker) {
        static const char text[]="NetDisc360 runs on the PC; it deletes this file when it quits.\r\n";
        if(!ftp.Store((helper+"/"+OnlineMarker).c_str(),text,sizeof(text)-1)) { XboxLog(console,"couldn't write %s/%s: %s",helper.c_str(),OnlineMarker,ftp.Error()); return; }
    }
    console.marked=true;
    if(TuAuroraFollowUp(console,ftp,o)) return;
    std::vector<unsigned char> state;
    if(!shown || s.restarted || !o.restartAurora || !ftp.Retrieve((helper+"/"+ShownMarker).c_str(),state,64)) return;
    if(std::string(state.begin(),state.end()).compare(0,7,"offline")) return;
    s.restarted=true;
    if(RestartAurora(console,o,"so its filter shows the PC games again")) XboxEvent(console,EventInfo,"Aurora restarted to show the PC games");
}

// Moves a console's tiles from OldXboxFolder, where servers before the
// NetDisc360 folders put them, to the Xbox folder, and NetDisc360's own files
// from its folder there (OldXboxFolder/NetDisc360) to XboxAppFolder. They're
// renamed over FTP on the same drive, so nothing is copied or sent again. A
// tile whose name is taken in the new folder (an older server put it back)
// goes if it holds nothing but the launcher's own files, as Sync removes
// one, else it stays; so does what isn't NetDisc360's (NetISO Sync). The
// old folders go only once they're empty.
// Aurora's rows follow the tiles at its next start (MoveLua, in the scripts
// RefreshAuroraScripts writes while MoveMarker is in XboxAppFolder), so the
// games keep their art, favourites and play history.
// The number of tiles moved; -1 if the old folder couldn't be read.
int MoveOldFolder(Console& console, FtpClient& ftp, const Options& o) {
    if(!ftp.Enters(OldXboxFolder)) return ftp.Code()/100==5 ? 0 : -1;  // Not there: nothing to move.
    std::vector<FtpEntry> old, here, appHere;
    std::string app=XboxAppFolder(o);
    EnsureFolders(ftp,o.xboxFolder); EnsureFolders(ftp,app);
    if(!ftp.List(OldXboxFolder,old) || !ftp.List(o.xboxFolder.c_str(),here) || !ftp.List(app.c_str(),appHere)) {
        XboxLog(console,"couldn't look at %s to move its games to %s: %s",OldXboxFolder,o.xboxFolder.c_str(),ftp.Error());
        return -1;
    }
    auto has=[](const std::vector<FtpEntry>& list, const std::string& name) {
        for(size_t i=0;i<list.size();++i) if(!_stricmp(list[i].name.c_str(),name.c_str())) return true;
        return false;
    };
    int moved=0, kept=0, failed=0, own=0, dropped=0;
    FtpStorage storage(ftp);
    for(size_t i=0;i<old.size();++i) {
        const FtpEntry& e=old[i];
        std::string from=std::string(OldXboxFolder)+"/"+e.name;
        if(e.folder && !_stricmp(e.name.c_str(),HelperFolder)) {
            std::vector<FtpEntry> files;
            if(!ftp.List(from.c_str(),files)) { ++failed; continue; }
            for(size_t k=0;k<files.size();++k) {
                std::string path=from+"/"+files[k].name;
                if(has(appHere,files[k].name)) {
                    // The new folder has its own already. The old helper goes, so no tile starts it.
                    if(!files[k].folder && !_stricmp(files[k].name.c_str(),HelperXex)) ftp.Delete(path.c_str());
                    continue;
                }
                if(ftp.Rename(path.c_str(),(app+"/"+files[k].name).c_str())) ++own;
                else XboxLog(console,"couldn't move %s to %s: %s",path.c_str(),app.c_str(),ftp.Error());
            }
            ftp.RemoveFolder(from.c_str());  // Only once it's empty.
            continue;
        }
        if(!e.folder || !_stricmp(e.name.c_str(),SyncFolder)) continue;
        if(has(here,e.name)) { if(storage.RemoveGame(from.c_str())) ++dropped; else ++kept; continue; }
        if(ftp.Rename(from.c_str(),(o.xboxFolder+"/"+e.name).c_str())) ++moved;
        else { ++failed; XboxLog(console,"couldn't move %s to %s: %s",from.c_str(),o.xboxFolder.c_str(),ftp.Error()); }
    }
    bool empty=ftp.RemoveFolder(OldXboxFolder);  // Only once it's empty.
    if(!moved && !own && !kept && !failed && !dropped) return 0;
    XboxLog(console,"moved %s from %s to %s%s%s%s%s%s.",Count(moved,"game folder","game folders").c_str(),OldXboxFolder,o.xboxFolder.c_str(),
            own ? (", and NetDisc360's own files to "+app).c_str() : "",
            dropped ? (" (removed "+Count(dropped,"older copy","older copies")+" of tiles the new folder has)").c_str() : "",
            kept ? (" ("+Count(kept,"folder","folders")+" left there: the same name is taken in the new folder)").c_str() : "",
            failed ? (" ("+Count(failed,"folder","folders")+" couldn't be moved)").c_str() : "",
            empty ? "; the old folder is gone" : "");
    if(moved) {
        static const char text[]="NetDisc360 moved its tiles here; Aurora's list follows them as it starts.\r\n";
        if(!ftp.Store((app+"/"+MoveMarker).c_str(),text,sizeof(text)-1))
            XboxLog(console,"couldn't write %s/%s, so Aurora's list may not follow the games: %s",app.c_str(),MoveMarker,ftp.Error());
        XboxEvent(console,EventGood,"Games moved to "+ConsolePath(o.xboxFolder),
                  "Games on "+std::string(console.text)+" moved to "+ConsolePath(o.xboxFolder));
    }
    return moved;
}

// What Aurora's scripts did with moved tiles (MovedMarker, see MoveLua). When
// rows moved, Aurora is restarted once more, so its list shows them in their
// new folder. MoveMarker goes once Aurora's list could be read, so the
// scripts stop moving; if it couldn't, the cleanup script can do it.
void FinishMove(Console& console, PushState& s, FtpClient& ftp, const Options& o) {
    std::string app=XboxAppFolder(o);
    std::vector<FtpEntry> files;
    if(!ftp.List(app.c_str(),files)) return;
    bool there=false;
    for(size_t i=0;i<files.size();++i) if(!files[i].folder && !_stricmp(files[i].name.c_str(),MovedMarker)) there=true;
    std::vector<unsigned char> data;
    if(!there || !ftp.Retrieve((app+"/"+MovedMarker).c_str(),data,256)) return;
    ftp.Delete((app+"/"+MovedMarker).c_str());
    std::string done(data.begin(),data.end());
    int rows=0, left=0, scanPath=0;
    if(sscanf(done.c_str(),"moved=%d left=%d scanpath=%d",&rows,&left,&scanPath)!=3) {
        XboxLog(console,"Aurora couldn't move its list to the games' new folder as it started (%s). Run Utilities, NetDisc360 Cleanup in Aurora to do it.",done.c_str());
        XboxEvent(console,EventWarning,"Run NetDisc360 Cleanup in Aurora's Utilities to finish moving the games");
        return;
    }
    ftp.Delete((app+"/"+MoveMarker).c_str());
    if(!rows && !scanPath) return;
    XboxLog(console,"Aurora's list follows the games to %s: %s moved%s%s.",o.xboxFolder.c_str(),Count(rows,"game","games").c_str(),
            scanPath ? ", and its scan path with them" : "",
            left ? (", "+Count(left,"game","games")+" stay on the list at their old place (Utilities, NetDisc360 Cleanup takes them off)").c_str() : "");
    if(s.movedRestarted || !o.restartAurora) return;
    s.movedRestarted=true;
    RestartAurora(console,o,"so its list shows the games in their new folder");
}

void FollowAddress(Console& console, PushState& s, FtpClient& ftp, const Options& o);

void PushTo(Console& console, PushState& s, const std::string& state, bool settled, double startedAt) {
    if(s.finished) return;
    PushMark mark(console.address);
    if(!mark.marked) return;
    Options o=Current();
    bool pushDue=settled && (!s.everPushed || state!=s.pushed || (s.retryAt && Now()>=s.retryAt));
    bool scanDue=s.scanTries && Now()>=s.scanAt;
    bool online=o.pushOnce || !s.downSince || console.seenAt.load()/1000.0>=s.downSince || Now()-s.upSince>=o.onlineWait;
    if(!(pushDue || scanDue) || Now()<s.nextProbe || !(!s.reachable || online || scanDue)) return;
    // Not while it plays a game: Aurora's FTP server isn't there to write
    // to, and a push then failed file by file (seen on the console). It goes
    // on once AuroraLoop sees Aurora again.
    bool playing;
    { std::lock_guard<std::mutex> hold(ConsoleLock); playing=console.connections>0 && !console.mounted.empty() && !console.inAurora; }
    if(playing && !o.pushOnce) return;
    bool idle=o.xboxes.empty() && Now()-console.seenAt.load()/1000.0>=IdleSeconds;
    if(!s.reachable && idle && Now()<s.probedAt+IdleProbeSeconds) return;
    s.probedAt=Now();
    std::lock_guard<std::mutex> ftpHold(FtpLockFor(console.address));
    FtpClient ftp(console.address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),15);
    if(!ftp.Open()) {
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
        s.upSince=Now();
        online=o.pushOnce || !s.downSince || console.seenAt.load()/1000.0>=s.downSince || o.onlineWait<=0;
    }
    if(o.filter && (back || pushDue)) GuardFilter(console,ftp,back,o);
    if(o.auroraScripts && DefaultLayout(o) && (back || pushDue || scanDue)) FinishMove(console,s,ftp,o);
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
        // Tiles from before the NetDisc360 folders move first, so the push finds them.
        if(DefaultLayout(o) && !s.moveChecked) {
            int moved=MoveOldFolder(console,ftp,o);
            s.moveChecked=moved>=0;
            if(moved>0) s.moveRestart=true;
        }
        std::vector<InstalledGame> installed, godPackages;
        LookInstalled(console,s,ftp,o,installed,godPackages);
        if(Push(console,ftp,result,(o.pushOnce || Now()-holdFrom>=o.removeWait) && !missing,o,installed,godPackages)) {
            s.everPushed=true; s.pushed=state;
            console.synced=(long long)time(NULL);
            RefreshWake(console,ftp,o);
            if(o.filter) { RefreshInstallFilter(console,ftp,o); RefreshRecentFilter(console,ftp,o); }
            if(o.auroraScripts) RefreshAuroraScripts(console,s,ftp,o);
            // Aurora's list still has the moved games at their old place: its
            // scripts move them as it starts.
            if(s.moveRestart) {
                s.moveRestart=false;
                if(!o.auroraScripts)
                    XboxLog(console,"Aurora's scripts are off in NetDisc360's settings, so its list can't follow the moved games: it adds them again as new games after its next scan of %s.",o.xboxFolder.c_str());
                else if(!(o.restartAurora && RestartAurora(console,o,"so its list follows the games to their new folder")))
                    XboxEvent(console,EventInfo,"Restart Aurora so its list follows the games to their new folder",
                              "Restart Aurora on "+std::string(console.text)+" so its list follows the games to their new folder");
                // A scan once it's back, which also looks for what its scripts did (FinishMove).
                if(o.scan && !s.scanTries) { s.scanTries=o.pushOnce ? 1 : ScanTries; s.scanAt=Now()+(o.pushOnce ? 0 : ScanRetrySeconds); }
            }
            // Folders held while a game folder is missing wait for it to come
            // back, which changes the state (see WatchLoop).
            s.retryAt=result.failed ? Now()+RetryFailedSeconds : result.held && !missing ? holdFrom+o.removeWait : result.skipped ? Now()+RetrySkippedSeconds : 0;
            if(result.added || result.removed) {
                if(o.scan) { s.scanTries=o.pushOnce ? 1 : ScanTries; s.scanAt=Now(); scanDue=true; }
                else XboxLog(console,"Aurora shows the changes after its next scan of %s.",o.xboxFolder.c_str());
            }
        } else s.nextProbe=Now()+30;
    }
    if(scanDue) {
        if(StartScan(console,ftp,o)) s.scanTries=0;
        else if(--s.scanTries) s.scanAt=Now()+ScanRetrySeconds;
        else XboxLog(console,"Aurora shows the changes after its next scan of %s.",o.xboxFolder.c_str());
    }
    if(o.pushOnce && s.attempted && !s.scanTries) s.finished=true;
    ftp.Close();
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
        bool known=false;
        {
            std::lock_guard<std::mutex> hold(SettingsLock);
            for(size_t k=0;k<Known.size() && !known;++k) if(Known[k].address==consoles[i]->text) { Known[k].seen=seen; known=true; }
            if(!known) { KnownConsole c = {consoles[i]->text, seen}; Known.push_back(c); }
        }
        std::map<DWORD,long long>::iterator last=saved.find(consoles[i]->address);
        if(!known || last==saved.end() || seen-last->second>600) { saved[consoles[i]->address]=seen; save=true; }
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
    Options o;
    std::vector<KnownConsole> known;
    { std::lock_guard<std::mutex> hold(SettingsLock); o=Settings; known=Known; }
    if(o.configPath.empty()) return false;
    std::lock_guard<std::mutex> hold(SaveLock);
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
    f.Set("xbox_layout","2");
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
    f.Set("wake",o.wake ? "1" : "0");
    f.Set("wake_mac",o.wakeMac);
    f.Set("save_backup",o.saveBackup ? "1" : "0");
    f.Set("save_keep",Number(o.saveKeep));
    f.Set("save_follow",o.saveFollow ? "1" : "0");
    f.Set("sync_url",o.syncUrl);
    f.Set("sync_key",o.syncKey.empty() ? std::string() : config::Protect(o.syncKey));
    f.Set("sync_id",o.syncId);
    f.Set("sync_name",o.syncName);
    f.Set("skip_installed",o.skipInstalled ? "1" : "0");
    f.Set("installed_folders",o.installedFolders);
    f.Set("aurora_scripts",o.auroraScripts ? "1" : "0");
    f.Set("restart_aurora",o.restartAurora ? "1" : "0");
    f.Set("aurora_web_user",o.auroraWebUser);
    f.Set("aurora_web_password",config::Protect(o.auroraWebPassword));
    f.Set("aurora_web_port",Number(o.auroraWebPort));
    f.Set("follow_address",o.followAddress ? "1" : "0");
    f.Set("keep_awake",o.keepAwake ? "1" : "0");
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
    f.Set("tile_mode",o.tileMode);
    std::map<std::string,TileChoice> tileChoices;
    { std::lock_guard<std::mutex> hold(SettingsLock); tileChoices=TileChoices; }
    before=Listed(f,"tile_choices");
    n=0;
    for(std::map<std::string,TileChoice>::const_iterator c=tileChoices.begin();c!=tileChoices.end();++c) {
        if(c->second.mode.empty() && c->second.games.empty()) continue;
        std::string value=c->first+"|"+c->second.mode;
        for(std::map<std::string,std::string>::const_iterator g=c->second.games.begin();g!=c->second.games.end();++g) value+="|"+g->second+":"+config::ToUtf8(g->first);
        f.Set(("tile_choice"+Number(++n)).c_str(),value);
    }
    f.Set("tile_choices",Number(n));
    for(long long i=n+1;i<=before;++i) f.Remove(("tile_choice"+Number(i)).c_str());
    before=Listed(f,"consoles");
    f.Set("consoles",Number((long long)known.size()));
    for(size_t i=0;i<known.size();++i) f.Set(("console"+Number((long long)i+1)).c_str(),known[i].address+"|"+Number(known[i].seen));
    for(long long i=(long long)known.size()+1;i<=before;++i) f.Remove(("console"+Number(i)).c_str());
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
    // Settings from before the NetDisc360 folders have the Xbox folder of
    // then, Hdd1:\NetISO, which was the default; it is the new default now,
    // and the consoles' tiles move there (MoveOldFolder). Saved again, the
    // settings say so (xbox_layout), so a later choice of the old folder stays.
    if(f.Get("xbox_layout").empty()) {
        std::string folder=o.xboxFolder;
        while(folder.size()>1 && folder[folder.size()-1]=='/') folder.erase(folder.size()-1);
        if(!_stricmp(folder.c_str(),OldXboxFolder)) o.xboxFolder=DefaultXboxFolder;
    }
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
    o.wake=Flag(f,"wake",o.wake);
    if(f.Has("wake_mac")) o.wakeMac=wake::CleanMac(f.Get("wake_mac"));
    o.saveBackup=Flag(f,"save_backup",o.saveBackup);
    o.saveFollow=Flag(f,"save_follow",o.saveFollow);
    if(f.Has("save_keep")) o.saveKeep=std::max(1,std::min(100,int(f.Number("save_keep",o.saveKeep))));
    if(f.Has("save_delay")) o.saveDelay=double(std::max(0,std::min(600,int(f.Number("save_delay",10)))));  // Only for the tests.
    o.skipInstalled=Flag(f,"skip_installed",o.skipInstalled);
    if(f.Has("installed_folders")) o.installedFolders=f.Get("installed_folders");
    o.auroraScripts=Flag(f,"aurora_scripts",o.auroraScripts);
    o.restartAurora=Flag(f,"restart_aurora",o.restartAurora);
    if(!f.Get("aurora_web_user").empty()) o.auroraWebUser=f.Get("aurora_web_user");
    if(f.Has("aurora_web_password") && config::Unprotect(f.Get("aurora_web_password"),password)) o.auroraWebPassword=password;
    // The sync's key only where it was made (it's protected for this Windows
    // user): settings restored from another computer don't sync as that one.
    o.syncUrl=f.Get("sync_url"); o.syncName=f.Get("sync_name"); o.syncKey.clear(); o.syncId.clear();
    if(!f.Get("sync_key").empty() && config::Unprotect(f.Get("sync_key"),password) && !o.syncUrl.empty()) { o.syncKey=password; o.syncId=f.Get("sync_id"); }
    if(f.Has("aurora_web_port")) o.auroraWebPort=std::max(1,std::min(65535,int(f.Number("aurora_web_port",o.auroraWebPort))));
    o.followAddress=Flag(f,"follow_address",o.followAddress);
    o.keepAwake=Flag(f,"keep_awake",o.keepAwake);
    if(f.Has("auto_off_minutes")) o.autoOffMinutes=std::max(0,std::min(1440,int(f.Number("auto_off_minutes",0))));
    if(IsTileMode(f.Get("tile_mode"))) o.tileMode=f.Get("tile_mode");
    {
        // "address|mode|style:listed name|..." (see TileChoice).
        std::map<std::string,TileChoice> tileChoices;
        for(long long i=1,n=Listed(f,"tile_choices");i<=n;++i) {
            std::string value=f.Get(("tile_choice"+Number(i)).c_str());
            std::vector<std::string> parts;
            for(size_t at=0;at<=value.size();) {
                size_t bar=value.find('|',at);
                if(bar==std::string::npos) bar=value.size();
                parts.push_back(value.substr(at,bar-at));
                at=bar+1;
            }
            if(parts.size()<2 || inet_addr(parts[0].c_str())==INADDR_NONE) continue;
            TileChoice& c=tileChoices[parts[0]];
            if(IsTileMode(parts[1])) c.mode=parts[1];
            for(size_t k=2;k<parts.size();++k) {
                size_t colon=parts[k].find(':');
                if(colon!=std::string::npos && colon+1<parts[k].size() && IsTileMode(parts[k].substr(0,colon)))
                    c.games[Lower(config::FromUtf8(parts[k].substr(colon+1)))]=parts[k].substr(0,colon);
            }
        }
        std::lock_guard<std::mutex> hold(SettingsLock);
        TileChoices.swap(tileChoices);
    }
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
        KnownConsole c = {value.substr(0,bar), atoll(value.c_str()+bar+1)};
        known.push_back(c);
    }
    std::lock_guard<std::mutex> hold(SettingsLock);
    Known.swap(known);
    return true;
}

// Wake-on-LAN and game saves ----------------------------------------------------

bool Text(const web::Value& v, const char* key, std::string& out);

// This computer's adapter toward the consoles, for the dashboard: its MAC
// address and IPv4 address (looked up once a minute).
void WakeAdapter(const Options& o, std::string& mac, std::string& address) {
    static std::mutex lock;
    static double at=-1000;
    static std::string lastMac, lastAddress;
    std::lock_guard<std::mutex> hold(lock);
    if(Now()-at>=60) {
        at=Now();
        DWORD toward=o.xboxes.empty() ? 0 : o.xboxes[0];
        if(!toward) { std::vector<Console*> consoles=AllConsoles(); if(!consoles.empty()) toward=consoles.back()->address; }
        if(!toward) toward=inet_addr("192.168.1.1");
        DWORD own=0, all=0;
        lastMac.clear(); lastAddress.clear();
        if(wake::Adapter(toward,lastMac,own,all)) { char text[16]; AddressText(own,text,sizeof(text)); lastAddress=text; }
    }
    mac=lastMac; address=lastAddress;
}

// What each console's wake.ini says, as last written or found (under WakeLock).
std::mutex WakeLock;
std::map<DWORD,std::string> WakeWritten;

// Keeps wake.ini in the helper's folder on a console (Hdd1:\Apps\NetDisc360)
// naming the adapter this computer reaches it with, so its launcher can
// wake this computer (see wake.h); takes it away when waking is off.
void RefreshWake(const Console& console, FtpClient& ftp, const Options& o) {
    std::string text;
    if(o.wake) {
        std::string mac; DWORD own=0, all=0;
        bool found=wake::Adapter(console.address,mac,own,all);
        if(!o.wakeMac.empty()) {
            mac=o.wakeMac;
            if(!found && own) all=htonl(ntohl(own)|0xFF);
        }
        if(!mac.empty() && own) text=wake::Ini(mac,own,all);
    }
    std::string path=XboxAppFolder(o)+"/wake.ini";
    {
        std::lock_guard<std::mutex> hold(WakeLock);
        std::map<DWORD,std::string>::const_iterator known=WakeWritten.find(console.address);
        if(known!=WakeWritten.end() && known->second==text) return;
    }
    std::vector<unsigned char> there;
    bool has=ftp.Retrieve(path.c_str(),there,4096);
    if(has && std::string(there.begin(),there.end())==text) { std::lock_guard<std::mutex> hold(WakeLock); WakeWritten[console.address]=text; return; }
    if(text.empty()) {
        if(has && !ftp.Delete(path.c_str())) { XboxLog(console,"couldn't delete %s: %s",path.c_str(),ftp.Error()); return; }
        if(has) XboxLog(console,"deleted %s: the Xbox doesn't wake this computer.",path.c_str());
        else if(o.wake) XboxLog(console,"no network adapter with a MAC address reaches it, so its launcher can't wake this computer.");
    } else {
        EnsureFolders(ftp,XboxAppFolder(o));
        if(!ftp.Store(path.c_str(),text.data(),text.size())) { XboxLog(console,"couldn't write %s: %s",path.c_str(),ftp.Error()); return; }
        XboxLog(console,"wrote %s, so its launcher can wake this computer (%s).",path.c_str(),text.substr(4,17).c_str());
    }
    std::lock_guard<std::mutex> hold(WakeLock);
    WakeWritten[console.address]=text;
}

std::string SavesFolder(const Options& o) { return o.configPath.empty() ? std::string() : config::FolderOf(o.configPath)+"Saves"; }
std::string SavePath(const Options& o, const std::string& name) {
#ifdef _WIN32
    return SavesFolder(o)+"\\"+name;
#else
    return SavesFolder(o)+"/"+name;
#endif
}

// The game a Title ID is among the served games, or "".
std::string GameOf(DWORD titleId) {
    std::vector<IsoFile> games=LatestGames();
    for(size_t i=0;i<games.size();++i) if(games[i].titleId==titleId) return WithoutIso(games[i].name);
    return std::string();
}

// One game's saves from a console into the Saves folder; false if the console
// couldn't be read. Logs what happened.
// The gamertags of the profiles signed in on a console, by XUID, from
// Aurora's web server: the console won't hand their packages out over FTP
// while they're signed in. Empty if Aurora doesn't answer.
std::map<std::string,std::string> SignedInTags(const Console& console, const Options& o) {
    std::map<std::string,std::string> tags;
    AuroraWeb aurora(console.address,o.auroraWebPort,o.auroraWebUser,o.auroraWebPassword,4);
    std::string body;
    web::Value v;
    if(!aurora.Get("/profile","/getProfileInfo",body) || !web::Parse(body,v) || v.type!=web::Value::Array) return tags;
    for(size_t i=0;i<v.items.size();++i) {
        const web::Value* xuid=v.items[i].Get("xuid");
        const web::Value* tag=v.items[i].Get("gamertag");
        if(!xuid || !tag || xuid->type!=web::Value::String || tag->type!=web::Value::String || tag->text.empty()) continue;
        std::string id=xuid->text;
        if(id.size()>2 && id[0]=='0' && (id[1]=='x' || id[1]=='X')) id.erase(0,2);
        for(size_t c=0;c<id.size();++c) id[c]=char(toupper((unsigned char)id[c]));
        if(id.size()==16 && id!="0000000000000000") tags[id]=tag->text;
    }
    return tags;
}

// Where a save backup came from, in its save.ini: the console and this
// computer (its name in the sync, else its own).
std::string ComputerName();
std::string FromLines(const Console& console, const Options& o) {
    return std::string("console=")+console.text+"\r\ncomputer="+(o.syncName.empty() ? ComputerName() : o.syncName)+"\r\n";
}

bool SaveGame(const Console& console, FtpClient& ftp, DWORD titleId, const std::string& game, const Options& o, saves::Result& r, std::string& error,
              const std::string& extra=std::string()) {
    if(!saves::Take(ftp,SavesFolder(o),titleId,config::ToUtf8(game),o.saveKeep,r,error,SignedInTags(console,o),FromLines(console,o)+extra)) {
        XboxLog(console,"couldn't back up the saves of %s: %s",game.c_str(),config::FromUtf8(error).c_str());
        return false;
    }
    if(!r.files) XboxLog(console,"%s has no saves on it to back up.",game.c_str());
    else if(r.same) XboxLog(console,"the saves of %s are as in their newest backup (%s).",game.c_str(),r.name.c_str());
    else {
        XboxLog(console,"backed up the saves of %s: %d file(s) of %d profile(s), %s.",game.c_str(),r.files,r.profiles,r.name.c_str());
        XboxEvent(console,EventGood,"Saves of "+game+" backed up",std::string("Saves of ")+game+" backed up from "+console.text);
        SyncSoon();
        FollowAllSoon();
    }
    return true;
}

// Saves to back up: a console that got back to Aurora after a game, from when.
struct SaveJob { DWORD address; std::string game; double at; };
std::mutex SaveJobLock;
std::deque<SaveJob> SaveJobs;

void QueueSaves(DWORD address, const std::string& game) {
    Options o=Current();
    if(!o.saveBackup || o.configPath.empty() || game.empty()) return;
    SaveJob job={address,game,Now()+o.saveDelay};
    std::lock_guard<std::mutex> hold(SaveJobLock);
    for(size_t i=0;i<SaveJobs.size();++i) if(SaveJobs[i].address==address) { SaveJobs[i]=job; return; }
    SaveJobs.push_back(job);
}

// Backs up the saves of the game each console played, a little after it is
// back in Aurora (Aurora's FTP server is up then, and the game has written
// its saves). One try more a minute later if the console couldn't be read.
void SaveLoop() {
    for(;;) {
        Sleep(1000);
        SaveJob job;
        job.address=0;
        {
            std::lock_guard<std::mutex> hold(SaveJobLock);
            std::deque<SaveJob>::iterator due=SaveJobs.begin();
            while(due!=SaveJobs.end() && due->at>Now()) ++due;
            if(due!=SaveJobs.end()) { job=*due; SaveJobs.erase(due); }
        }
        // No backup due: saves following their profile, a console at a time.
        if(!job.address) { FollowStep(); continue; }
        Console* console=ConsoleIfKnown(job.address);
        Options o=Current();
        if(!console || !o.saveBackup || o.configPath.empty()) continue;
        DWORD titleId=0;
        std::vector<IsoFile> games=LatestGames();
        for(size_t i=0;i<games.size() && !titleId;++i) if(games[i].name==job.game) titleId=games[i].titleId;
        if(!titleId) continue;
        bool ok;
        {
            std::lock_guard<std::mutex> ftpHold(FtpLockFor(job.address));
            FtpClient ftp(job.address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),15);
            saves::Result r; std::string error;
            ok=SaveGame(*console,ftp,titleId,WithoutIso(job.game),o,r,error);
            ftp.Close();
        }
        if(!ok && job.at>0) {
            job.at=-(Now()+60);  // Negative: tried once already.
            std::lock_guard<std::mutex> hold(SaveJobLock);
            SaveJobs.push_back(job);
        }
    }
}

// The save backups, by game, for the dashboard.
void SaveList(web::Reply& out) {
    Options o=Current();
    std::string folder=SavesFolder(o);
    std::vector<saves::Backup> list;
    if(!folder.empty()) list=saves::List(folder);
    std::map<DWORD,std::string> names;
    std::vector<IsoFile> games=LatestGames();
    for(size_t i=0;i<games.size();++i) if(!names.count(games[i].titleId)) names[games[i].titleId]=WithoutIso(games[i].name);
    web::Json j;
    j.Open('{').Set("available",!folder.empty()).Set("on",o.saveBackup).Set("folder",folder).Key("saves").Open('[');
    for(size_t i=0;i<list.size();++i) {
        const saves::Backup& b=list[i];
        char id[9]; snprintf(id,sizeof(id),"%08X",b.titleId);
        std::string game=names.count(b.titleId) ? config::ToUtf8(names[b.titleId]) : std::string(), data;
        std::vector<std::string> profiles;
        if(config::ReadFile(SavePath(o,b.name),data,300<<20)) {
            std::string named;
            saves::Describe(data,named,profiles);
            if(game.empty()) game=named;
        }
        j.Open('{').Set("name",b.name).Set("titleId",std::string(id)).Set("game",game).Set("time",b.time).Set("size",b.size).Key("profiles").Open('[');
        for(size_t p=0;p<profiles.size();++p) j.String(profiles[p]);
        j.Close(']').Close('}');
    }
    j.Close(']').Close('}');
    out.body=j.Text();
}

// A console the dashboard names, which has to be in Aurora (its FTP server up).
Console* SaveConsole(const web::Value& v, std::string& error) {
    std::string address;
    Text(v,"address",address);
    DWORD at=inet_addr(address.c_str());
    Console* console=address.empty() || at==INADDR_NONE ? NULL : ConsoleIfKnown(at);
    if(!console) { error="Choose a console that has used NetDisc360."; return NULL; }
    bool playing;
    { std::lock_guard<std::mutex> hold(ConsoleLock); playing=console->connections>0 && !console->mounted.empty() && !console->inAurora; }
    if(playing) { error=std::string(console->text)+" is playing a game. Go back to Aurora first."; return NULL; }
    return console;
}

// Backs up the saves of every game of this server that has saves on a console, now.
bool SaveAll(const web::Value& v, web::Reply& out, std::string& error) {
    Options o=Current();
    if(o.configPath.empty()) { error="The settings aren't kept in a file (start NetDisc360 with --config), so there's nowhere to keep saves."; return false; }
    Console* console=SaveConsole(v,error);
    if(!console) return false;
    std::lock_guard<std::mutex> ftpHold(FtpLockFor(console->address));
    FtpClient ftp(console->address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),15);
    if(!ftp.Open()) { error=std::string("Can't reach Aurora's FTP server on ")+console->text+": "+ftp.Error(); return false; }
    std::set<DWORD> titles;
    if(!saves::Titles(ftp,titles,error)) { error="Can't read the saves on "+std::string(console->text)+": "+error; return false; }
    int made=0, same=0;
    for(std::set<DWORD>::const_iterator t=titles.begin();t!=titles.end();++t) {
        std::string game=GameOf(*t);
        if(game.empty()) continue;  // Not one of this server's games.
        saves::Result r;
        if(!SaveGame(*console,ftp,*t,game,o,r,error)) return false;
        if(r.files && r.same) ++same;
        else if(r.files) ++made;
    }
    ftp.Close();
    web::Json j;
    j.Open('{').Set("ok",true).Set("made",made).Set("same",same).Close('}');
    out.body=j.Text();
    return true;
}

// Puts a save backup back on a console; the saves there now are backed up first.
bool RestoreSaves(const web::Value& v, std::string& error) {
    Options o=Current();
    std::string name, data;
    Text(v,"name",name);
    if(SavesFolder(o).empty() || !saves::Named(name) || !config::ReadFile(SavePath(o,name),data,300<<20)) { error="That save backup isn't there any more."; return false; }
    Console* console=SaveConsole(v,error);
    if(!console) return false;
    DWORD titleId=DWORD(strtoul(name.substr(0,8).c_str(),NULL,16));
    std::string game=GameOf(titleId);
    if(game.empty()) game=name.substr(0,8);
    std::lock_guard<std::mutex> ftpHold(FtpLockFor(console->address));
    FtpClient ftp(console->address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),15);
    if(!ftp.Open()) { error=std::string("Can't reach Aurora's FTP server on ")+console->text+": "+ftp.Error(); return false; }
    saves::Result before;
    if(!SaveGame(*console,ftp,titleId,game,o,before,error)) { error="The saves there now couldn't be backed up first, so nothing was changed: "+error; return false; }
    int files=0;
    if(!saves::Put(ftp,data,files,error)) {
        XboxLog(*console,"restoring the saves of %s from %s stopped: %s",game.c_str(),name.c_str(),error.c_str());
        return false;
    }
    // What was put back is these saves from now on, also for the profile's
    // other consoles (FollowConsole takes the newest restored_from as theirs).
    saves::Result after; std::string ignored;
    SaveGame(*console,ftp,titleId,game,o,after,ignored,"restored_from="+name+"\r\n");
    ftp.Close();
    XboxLog(*console,"restored the saves of %s from %s (%d file(s)).",game.c_str(),name.c_str(),files);
    XboxEvent(*console,EventInfo,"Saves of "+game+" restored",std::string("Saves of ")+game+" restored on "+console->text);
    return true;
}

// Saves follow the profile -------------------------------------------------------
//
// The save backups (made here, or synced from other computers) say what each
// profile's newest saves of a game are: those of the newest backup whose
// saves of that profile weren't in an older one (or that a restore put back,
// restored_from). When a console is in Aurora, each profile on it is
// compared with that:
//   - the same: nothing to do;
//   - none there, or saves that are in a backup already (older ones): the
//     newest go on the console, as nothing is lost;
//   - saves that are in no backup: they're backed up. If the newest came
//     from another console or computer, both played since, so the dashboard
//     asks which to keep (a conflict); until then nothing is put on it.
// Profiles not on the console are left alone.

struct SaveConflict {
    std::string address, title, xuid, gamertag, game;
    std::string held;     // The backup of the console's saves.
    std::string newest;   // The newest from elsewhere.
    std::string from;     // Where that came from ("console@computer").
    std::string digest;   // SHA-256 of the console's saves of that profile.
};
std::mutex FollowLock;
std::vector<SaveConflict> Conflicts2;              // Under FollowLock; kept in save-conflicts.ini.
std::map<DWORD,std::map<std::string,std::string> > FollowDone;  // Console -> title -> what was checked (under FollowLock).
std::set<DWORD> FollowAsked;                        // Consoles to check (under FollowLock).

std::string ConflictsPath(const Options& o) { return o.configPath.empty() ? std::string() : config::FolderOf(o.configPath)+"save-conflicts.ini"; }

void SaveConflicts(const Options& o) {
    std::string text, path=ConflictsPath(o);
    if(path.empty()) return;
    {
        std::lock_guard<std::mutex> hold(FollowLock);
        for(size_t i=0;i<Conflicts2.size();++i) {
            const SaveConflict& c=Conflicts2[i];
            text+=c.address+"|"+c.title+"|"+c.xuid+"|"+c.held+"|"+c.newest+"|"+c.digest+"|"+c.from+"|"+c.gamertag+"|"+c.game+"\n";
        }
    }
    config::WriteFile(path,text,false);
}

void LoadConflicts(const Options& o) {
    std::string text, path=ConflictsPath(o);
    if(path.empty() || !config::ReadFile(path,text,1<<20)) return;
    std::vector<SaveConflict> all;
    for(size_t at=0;at<text.size();) {
        size_t end=text.find('\n',at); if(end==std::string::npos) end=text.size();
        std::string line=text.substr(at,end-at); at=end+1;
        std::vector<std::string> f;
        for(size_t from=0;;) {
            size_t bar=line.find('|',from);
            f.push_back(line.substr(from,bar==std::string::npos ? std::string::npos : bar-from));
            if(bar==std::string::npos || f.size()==8) { if(bar!=std::string::npos) f.push_back(line.substr(bar+1)); break; }
            from=bar+1;
        }
        if(f.size()<9) continue;
        SaveConflict c={f[0],f[1],f[2],f[7],f[8],f[3],f[4],f[6],f[5]};
        all.push_back(c);
    }
    std::lock_guard<std::mutex> hold(FollowLock);
    Conflicts2.swap(all);
}

void FollowSoon(DWORD address) { std::lock_guard<std::mutex> hold(FollowLock); FollowAsked.insert(address); }
void FollowAllSoon() {
    std::vector<Console*> consoles=AllConsoles();
    std::lock_guard<std::mutex> hold(FollowLock);
    for(size_t i=0;i<consoles.size();++i) FollowAsked.insert(consoles[i]->address);
}

// Whether a console can be read now: it answers and is in Aurora, not in a game.
bool InAuroraNow(Console& c) {
    std::lock_guard<std::mutex> hold(ConsoleLock);
    bool playing=c.connections>0 && !c.mounted.empty() && !c.inAurora;
    return !playing && !c.offline && (c.answers || c.inAurora);
}

std::string GamertagIn(const std::map<std::string,std::string>& files, const std::string& xuid) {
    std::map<std::string,std::string>::const_iterator ini=files.find("save.ini");
    return ini==files.end() ? std::string() : saves::IniValue(ini->second,"profile_"+xuid);
}
std::string FromOf(const std::map<std::string,std::string>& files) {
    std::map<std::string,std::string>::const_iterator ini=files.find("save.ini");
    if(ini==files.end()) return std::string();
    std::string console=saves::IniValue(ini->second,"console"), computer=saves::IniValue(ini->second,"computer");
    return console.empty() ? std::string() : console+"@"+computer;
}

// Checks every profile on console against the backups (see above); false
// if the console couldn't be read (asked again later).
bool FollowConsole(Console& console) {
    Options o=Current();
    std::string folder=SavesFolder(o);
    if(!o.saveFollow || !o.saveBackup || folder.empty()) return true;
    std::vector<saves::Backup> list=saves::List(folder);
    if(list.empty()) return true;
    std::map<DWORD,std::vector<std::string> > byTitle;  // Oldest first.
    for(size_t i=list.size();i-->0;) byTitle[list[i].titleId].push_back(list[i].name);
    const std::string here=std::string(console.text)+"@"+(o.syncName.empty() ? ComputerName() : o.syncName);
    std::lock_guard<std::mutex> ftpHold(FtpLockFor(console.address));
    FtpClient ftp(console.address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),15);
    std::vector<std::string> on;
    std::string error;
    if(!ftp.Open() || !saves::ProfilesOn(ftp,on,error)) return false;
    std::string profiles;
    for(size_t i=0;i<on.size();++i) profiles+=on[i]+",";
    int put=0;
    bool changed=false;
    for(std::map<DWORD,std::vector<std::string> >::const_iterator t=byTitle.begin();t!=byTitle.end();++t) {
        char title[9]; snprintf(title,sizeof(title),"%08X",(unsigned)t->first);
        const std::string checked=t->second.back()+"|"+profiles;
        {
            std::lock_guard<std::mutex> hold(FollowLock);
            if(FollowDone[console.address][title]==checked) continue;
        }
        // Each profile's newest saves of this game, and every saves it had.
        struct Newest { std::string name, digest, from; std::set<std::string> seen; std::map<std::string,std::string> files; };
        std::map<std::string,Newest> newest;
        for(size_t b=0;b<t->second.size();++b) {
            std::map<std::string,std::string> files;
            if(!saves::Read(folder,t->second[b],files,error)) continue;
            const std::string ini=files["save.ini"];
            bool restored=!saves::IniValue(ini,"restored_from").empty(), held=saves::IniValue(ini,"held")=="1";
            for(size_t p=0;p<on.size();++p) {
                std::string mine=saves::ProfileSaves(files,on[p]);
                if(mine.empty()) continue;
                Newest& n=newest[on[p]];
                std::string digest=web::Sha256Hex(mine);
                if(!held && (!n.seen.count(digest) || restored)) { n.name=t->second[b]; n.digest=digest; n.from=FromOf(files); n.files=files; }
                n.seen.insert(digest);
            }
        }
        if(newest.empty()) {
            std::lock_guard<std::mutex> hold(FollowLock);
            FollowDone[console.address][title]=checked;
            continue;
        }
        std::map<std::string,std::string> there;
        if(!saves::Collect(ftp,t->first,there,error)) { XboxLog(console,"couldn't read the saves of %s to compare: %s",title,error.c_str()); return false; }
        std::string game=GameOf(t->first);
        if(game.empty()) game=title;
        std::vector<std::string> push;
        std::vector<SaveConflict> found;
        bool backUp=false;
        for(std::map<std::string,Newest>::iterator n=newest.begin();n!=newest.end();++n) {
            if(n->second.name.empty()) continue;  // Only held backups of it.
            std::string mine=saves::ProfileSaves(there,n->first), digest=mine.empty() ? std::string() : web::Sha256Hex(mine);
            if(digest==n->second.digest) continue;
            {
                // Asked already: wait for the answer.
                std::lock_guard<std::mutex> hold(FollowLock);
                bool open=false;
                for(size_t i=0;i<Conflicts2.size() && !open;++i)
                    open=Conflicts2[i].address==console.text && Conflicts2[i].title==title && Conflicts2[i].xuid==n->first;
                if(open) continue;
            }
            if(mine.empty() || n->second.seen.count(digest)) { push.push_back(n->first); continue; }
            backUp=true;
            if(!n->second.from.empty() && n->second.from!=here) {
                SaveConflict c;
                c.address=console.text; c.title=title; c.xuid=n->first; c.game=config::ToUtf8(game);
                c.gamertag=GamertagIn(n->second.files,n->first); c.newest=n->second.name; c.from=n->second.from; c.digest=digest;
                found.push_back(c);
            }
        }
        if(backUp) {
            saves::Result r;
            std::string extra=found.empty() ? std::string() : "held=1\r\n";
            if(!SaveGame(console,ftp,t->first,game,o,r,error,extra)) return false;
            for(size_t i=0;i<found.size();++i) found[i].held=r.name;
            changed=true;
        }
        if(!found.empty()) {
            {
                std::lock_guard<std::mutex> hold(FollowLock);
                Conflicts2.insert(Conflicts2.end(),found.begin(),found.end());
            }
            SaveConflicts(o);
            for(size_t i=0;i<found.size();++i) {
                std::string who=found[i].gamertag.empty() ? found[i].xuid : found[i].gamertag;
                XboxLog(console,"%s's saves of %s here differ from the newest from %s: both kept; the dashboard asks which to keep.",who.c_str(),game.c_str(),found[i].from.c_str());
                XboxEvent(console,EventWarning,"Which saves of "+game+"?",who+"'s saves of "+game+" on "+console.text+" differ from the newest from "+found[i].from+". Choose which to keep under Settings, Backups.");
            }
        }
        for(size_t i=0;i<push.size();++i) {
            Newest& n=newest[push[i]];
            int written=0;
            if(!saves::PutFiles(ftp,n.files,push[i],written,error)) { XboxLog(console,"couldn't put the newer saves of %s on it: %s",game.c_str(),error.c_str()); return false; }
            std::string who=GamertagIn(n.files,push[i]);
            if(who.empty()) who=push[i];
            XboxLog(console,"put %s's newest saves of %s on it (%s, %d file(s)).",who.c_str(),game.c_str(),n.name.c_str(),written);
            XboxEvent(console,EventGood,"Saves of "+game+" brought over",who+"'s newest saves of "+game+" are on "+console.text+" now");
            ++put;
        }
        std::lock_guard<std::mutex> hold(FollowLock);
        // A backup made here changes the newest: checked again next time.
        FollowDone[console.address][title]=backUp ? std::string() : checked;
    }
    ftp.Close();
    if(changed) SyncSoon();
    return true;
}

// The dashboard's answer to a conflict: keep "console" (its saves become the
// newest everywhere) or "other" (the newest go on the console; its own stay
// in their backup).
bool ResolveConflict(const web::Value& v, std::string& error) {
    Options o=Current();
    std::string address, title, xuid, keep;
    Text(v,"address",address); Text(v,"title",title); Text(v,"xuid",xuid); Text(v,"keep",keep);
    SaveConflict c;
    {
        std::lock_guard<std::mutex> hold(FollowLock);
        size_t i=0;
        while(i<Conflicts2.size() && !(Conflicts2[i].address==address && Conflicts2[i].title==title && Conflicts2[i].xuid==xuid)) ++i;
        if(i==Conflicts2.size()) { error="That was settled already."; return false; }
        c=Conflicts2[i];
    }
    std::string folder=SavesFolder(o);
    if(keep=="console") {
        std::string made;
        if(!saves::Copy(folder,c.held,"held","restored_from="+c.held+"\r\n",made,error)) return false;
        Log("Saves of %s: keeping the ones from %s (%s), for every console.",c.game.c_str(),c.address.c_str(),made.c_str());
    } else if(keep=="other") {
        DWORD at=inet_addr(c.address.c_str());
        Console* console=at==INADDR_NONE ? NULL : ConsoleIfKnown(at);
        if(!console || !InAuroraNow(*console)) { error="Turn on "+c.address+" and go to Aurora first, so its saves can be changed."; return false; }
        std::map<std::string,std::string> files;
        if(!saves::Read(folder,c.newest,files,error)) return false;
        std::lock_guard<std::mutex> ftpHold(FtpLockFor(console->address));
        FtpClient ftp(console->address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),15);
        int written=0;
        if(!ftp.Open() || !saves::PutFiles(ftp,files,c.xuid,written,error)) { if(error.empty()) error=ftp.Error(); return false; }
        ftp.Close();
        XboxLog(*console,"put the saves of %s from %s on it, as chosen (its own are kept in %s).",c.game.c_str(),c.from.c_str(),c.held.c_str());
    } else { error="Choose which saves to keep."; return false; }
    {
        std::lock_guard<std::mutex> hold(FollowLock);
        for(size_t i=0;i<Conflicts2.size();++i)
            if(Conflicts2[i].address==address && Conflicts2[i].title==title && Conflicts2[i].xuid==xuid) { Conflicts2.erase(Conflicts2.begin()+i); break; }
        FollowDone.clear();
    }
    SaveConflicts(o);
    FollowAllSoon();
    SyncSoon();
    return true;
}

void ConflictsJson(web::Json& j) {
    std::lock_guard<std::mutex> hold(FollowLock);
    j.Key("saveConflicts").Open('[');
    for(size_t i=0;i<Conflicts2.size();++i) {
        const SaveConflict& c=Conflicts2[i];
        j.Open('{').Set("address",c.address).Set("title",c.title).Set("xuid",c.xuid).Set("gamertag",c.gamertag).Set("game",c.game)
         .Set("held",c.held).Set("newest",c.newest).Set("from",c.from).Close('}');
    }
    j.Close(']');
}

// Checks the consoles asked for, one at a time, between save backups: a
// console stays asked for until it's in Aurora. Once a minute, consoles not
// checked yet (new ones, and all after a start) are asked for.
void FollowStep() {
    static double lookedAt=-1e9, allAt=-1e9;
    if(Now()-lookedAt>60) {
        lookedAt=Now();
        // Every 10 minutes all of them: a profile put on a console since (by
        // hand, or by a party) gets its saves too. Games already checked with
        // the same profiles cost one look at the console's profiles.
        bool all=Now()-allAt>600;
        if(all) allAt=Now();
        std::vector<Console*> consoles=AllConsoles();
        std::lock_guard<std::mutex> hold(FollowLock);
        for(size_t i=0;i<consoles.size();++i) if(all || !FollowDone.count(consoles[i]->address)) FollowAsked.insert(consoles[i]->address);
    }
    std::vector<DWORD> asked;
    { std::lock_guard<std::mutex> hold(FollowLock); asked.assign(FollowAsked.begin(),FollowAsked.end()); }
    for(size_t i=0;i<asked.size();++i) {
        Console* console=ConsoleIfKnown(asked[i]);
        if(console && !InAuroraNow(*console)) continue;
        { std::lock_guard<std::mutex> hold(FollowLock); FollowAsked.erase(asked[i]); }
        if(!console) continue;
        bool ok=FollowConsole(*console);
        std::lock_guard<std::mutex> hold(FollowLock);
        if(!ok) FollowDone.erase(asked[i]);
        else FollowDone[asked[i]];  // Checked: not asked for again by the minute's look.
        return;
    }
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

void CopiesJson(web::Json& j);
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

// The consoles' choices of how they get the games (see TileChoice), with
// each game by the name the dashboard shows it by.
std::map<std::string,TileChoice> TileChoicesShown() {
    std::map<std::string,TileChoice> choices;
    { std::lock_guard<std::mutex> hold(SettingsLock); choices=TileChoices; }
    std::map<std::string,std::string> names;
    {
        std::lock_guard<std::mutex> hold(GamesListLock);
        for(size_t i=0;i<GamesList.size();++i) names[Lower(GamesList[i].name)]=config::ToUtf8(WithoutIso(GamesList[i].name));
    }
    for(std::map<std::string,TileChoice>::iterator c=choices.begin();c!=choices.end();++c) {
        std::map<std::string,std::string> shown;
        for(std::map<std::string,std::string>::const_iterator g=c->second.games.begin();g!=c->second.games.end();++g) {
            std::map<std::string,std::string>::const_iterator name=names.find(g->first);
            shown[name!=names.end() ? name->second : config::ToUtf8(WithoutIso(g->first))]=g->second;
        }
        c->second.games.swap(shown);
    }
    return choices;
}
// "tiles": {"mode": "" (the default), folder, god or none, "games": {name: mode}}.
void TileChoiceJson(web::Json& j, const std::map<std::string,TileChoice>& choices, const std::string& address) {
    std::map<std::string,TileChoice>::const_iterator c=choices.find(address);
    j.Key("tiles").Open('{').Set("mode",c==choices.end() ? std::string() : c->second.mode).Key("games").Open('{');
    if(c!=choices.end()) for(std::map<std::string,std::string>::const_iterator g=c->second.games.begin();g!=c->second.games.end();++g) j.Set(g->first.c_str(),g->second);
    j.Close('}').Close('}');
}

bool Text(const web::Value& v, const char* key, std::string& out);
bool Save();

// POST /api/consoles/tiles {address, mode, games: {name: mode}}: how that
// console gets the games; mode "" goes by the default, and games, if sent,
// is every game that goes otherwise than mode.
bool SetTileChoice(const web::Value& v, std::string& error) {
    std::string address, mode;
    if(!Text(v,"address",address) || inet_addr(address.c_str())==INADDR_NONE) { error="Send the console's address."; return false; }
    bool modeSent=Text(v,"mode",mode);
    if(modeSent && !mode.empty() && !IsTileMode(mode)) { error="A console gets its games as folder, god or none."; return false; }
    const web::Value* games=v.Get("games");
    std::map<std::string,std::string> picks;
    if(games) {
        if(games->type!=web::Value::Object) { error="games is {name: mode}."; return false; }
        std::lock_guard<std::mutex> hold(GamesListLock);
        for(size_t i=0;i<games->fields.size();++i) {
            const std::string& name=games->fields[i].first;
            const web::Value& m=games->fields[i].second;
            if(m.type!=web::Value::String || (!m.text.empty() && !IsTileMode(m.text))) { error="A game goes as folder, god or none."; return false; }
            if(m.text.empty()) continue;
            const IsoFile* game=NULL;
            for(size_t k=0;k<GamesList.size() && !game;++k) if(config::ToUtf8(WithoutIso(GamesList[k].name))==name) game=&GamesList[k];
            if(!game) { error="There's no game called "+name+" here."; return false; }
            picks[Lower(game->name)]=m.text;
        }
    }
    {
        std::lock_guard<std::mutex> hold(SettingsLock);
        TileChoice& c=TileChoices[address];
        if(modeSent) c.mode=mode;
        if(games) c.games.swap(picks);
        if(c.mode.empty() && c.games.empty()) TileChoices.erase(address);
    }
    Save();
    Log("Xbox %s: its games come %s now (set on the dashboard).",address.c_str(),
        mode==TileGod ? "as Games on Demand" : mode==TileNone ? "not at all" : mode==TileFolder ? "as game folders" : "as the default says");
    ++PushGeneration;
    return true;
}

void State(web::Reply& out) {
    Options o;
    std::vector<KnownConsole> known;
    { std::lock_guard<std::mutex> hold(SettingsLock); o=Settings; known=Known; }
    web::Json j;
    j.Open('{').Set("name","NetDisc360").Set("version",NETDISC_VERSION).Set("built",__DATE__ " " __TIME__);
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

    SyncJson(j,o);
    ConflictsJson(j);
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
    j.Set("wake",o.wake).Set("wakeMac",o.wakeMac).Set("saveBackup",o.saveBackup).Set("saveKeep",o.saveKeep).Set("saveFollow",o.saveFollow);
    {
        std::string mac,address;
        WakeAdapter(o,mac,address);
        j.Key("wakeAdapter").Open('{').Set("mac",mac).Set("address",address).Close('}');
    }
    j.Set("skipInstalled",o.skipInstalled).Set("installedFolders",o.installedFolders).Set("auroraScripts",o.auroraScripts).Set("tileMode",o.tileMode);
    j.Set("restartAurora",o.restartAurora).Set("auroraWebUser",o.auroraWebUser).Set("auroraWebPasswordSet",!o.auroraWebPassword.empty());
    j.Set("auroraWebPort",o.auroraWebPort).Set("followAddress",o.followAddress).Set("keepAwake",o.keepAwake).Set("autoOffMinutes",o.autoOffMinutes);
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
    std::map<std::string,TileChoice> tileChoices=TileChoicesShown();
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
            j.Open('{').Set("address",c.text).Set("connections",c.connections.load()).Set("sent",(unsigned long long)c.sent.load());
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
            j.Close(']');
            TileChoiceJson(j,tileChoices,c.text);
            j.Set("godTiles",(long long)c.godTiles.size()).Close('}');
        }
    }
    for(size_t k=0;k<known.size();++k) {
        bool listed=false;
        for(size_t i=0;i<consoles.size() && !listed;++i) listed=known[k].address==consoles[i]->text;
        if(listed) continue;
        j.Open('{').Set("address",known[k].address).Set("connections",0).Set("sent",0).Set("ftp",0).Set("synced",0LL).Set("mounted","");
        j.Set("playing",false).Set("inAurora",false).Set("ping",-1).Set("answers",false).Set("offline",false).Set("answered",0LL);
        j.Set("push",o.xboxes.empty() || std::find(o.xboxes.begin(),o.xboxes.end(),inet_addr(known[k].address.c_str()))!=o.xboxes.end());
        j.Set("seen",known[k].seen).Key("rate").Open('[').Close(']').Key("events").Open('[').Close(']');
        TileChoiceJson(j,tileChoices,known[k].address);
        j.Set("godTiles",0LL).Close('}');
    }
    j.Close(']');
    double peak;
    { std::lock_guard<std::mutex> hold(ConsoleLock); peak=Peak; }
    j.Key("totals").Open('{').Set("sent",(unsigned long long)total).Set("rate",rate).Set("peak",peak).Close('}');

    j.Key("events").Open('[');
    {
        std::lock_guard<std::mutex> hold(EventLock);
        for(size_t i=Events.size()>50 ? Events.size()-50 : 0;i<Events.size();++i) {
            const Happening& h=Events[i];
            j.Open('{').Set("id",h.id).Set("time",h.time).Set("kind",EventKinds[h.kind]).Set("text",config::ToUtf8(h.text)).Close('}');
        }
    }
    j.Close(']');
    CopiesJson(j);
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
    j.Set("disc",g.disc).Set("mediaId",g.mediaId ? Hex8(g.mediaId) : std::string()).Set("folder",folder).Set("installDisc",config::ToUtf8(WithoutIso(g.installDisc)));
    j.Set("role",g.role).Set("roleLabel",g.roleLabel).Set("incomplete",g.incomplete);
    U64 content=0;
    for(size_t i=0;i<g.packages.size();++i) content+=g.packages[i].size;
    j.Set("packages",int(g.packages.size())).Set("contentSize",(unsigned long long)content);
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
            for(std::map<std::string,std::string>::const_iterator t=c.godTiles.begin();t!=c.godTiles.end();++t) tiles[t->first].push_back(c.text);
            for(size_t k=0;k<c.installed.size();++k) {
                std::vector<std::string>& on=installed[std::make_pair(c.installed[k].titleId,c.installed[k].disc)];
                if(std::find(on.begin(),on.end(),std::string(c.text))==on.end()) on.push_back(c.text);
            }
        }
    }
    // The play history, with the games being played now counted so far.
    std::map<std::string,Played> plays;
    {
        std::lock_guard<std::mutex> hold(PlaysLock);
        plays=Plays;
        for(std::map<std::string,Played>::const_iterator p=OtherPlays.begin();p!=OtherPlays.end();++p) {
            Played& mine=plays[p->first];
            mine.times+=p->second.times; mine.seconds+=p->second.seconds; mine.last=std::max(mine.last,p->second.last);
        }
    }
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
            j.Close(']').Close('}');
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
struct LaunchIni { std::string path, defaultPath; std::vector<std::pair<int,std::string> > plugins; };
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
// Aurora's scan paths: settings.db's ScanPaths (Path under a drive, DeviceId,
// Depth), each drive named by content.db's MountedDevices (DeviceId,
// DeviceName: Hdd1, Usb0...), as FTP paths with their depth. False, with
// why in error, if the databases couldn't be read.
struct ScanPath { std::string path; int depth; };
bool AuroraScanPaths(FtpClient& ftp, const Options& o, std::vector<ScanPath>& out, std::string& error) {
    out.clear();
    std::string folder=o.aurora+"/Data/Databases";
    std::vector<FtpEntry> files;
    // Listed first: a read of a file that isn't there can stall FtpDll's next one.
    if(!ftp.List(folder.c_str(),files)) { error="Couldn't list "+folder+": "+ftp.Error(); return false; }
    std::string settingsName, contentName;
    for(size_t i=0;i<files.size();++i) {
        if(!_stricmp(files[i].name.c_str(),"settings.db")) settingsName=files[i].name;
        if(!_stricmp(files[i].name.c_str(),"content.db")) contentName=files[i].name;
    }
    if(settingsName.empty() || contentName.empty()) { error="Aurora's databases aren't in "+folder+"."; return false; }
    std::vector<unsigned char> settings, content;
    if(!ftp.Retrieve((folder+"/"+settingsName).c_str(),settings,64<<20) || !ftp.Retrieve((folder+"/"+contentName).c_str(),content,256<<20)) {
        error=std::string("Couldn't read Aurora's databases: ")+ftp.Error(); return false;
    }
    auto text=[](const SqlValue& v) { return v.null ? std::string() : !v.text.empty() ? v.text : std::to_string(v.number); };
    std::vector<std::vector<SqlValue> > devices, paths;
    std::vector<std::string> columns;
    columns.push_back("DeviceId"); columns.push_back("DeviceName");
    if(!ReadSqlTable(content,"MountedDevices",columns,devices,error)) { error="content.db: "+error; return false; }
    std::map<std::string,std::string> names;
    for(size_t i=0;i<devices.size();++i) names[text(devices[i][0])]=text(devices[i][1]);
    columns.clear(); columns.push_back("Path"); columns.push_back("DeviceId"); columns.push_back("Depth");
    if(!ReadSqlTable(settings,"ScanPaths",columns,paths,error)) { error="settings.db: "+error; return false; }
    for(size_t i=0;i<paths.size();++i) {
        std::map<std::string,std::string>::const_iterator d=names.find(text(paths[i][1]));
        if(d==names.end() || d->second.empty()) continue;
        std::string relative=text(paths[i][0]);
        for(size_t k=0;k<relative.size();++k) if(relative[k]=='\\') relative[k]='/';
        while(!relative.empty() && relative[0]=='/') relative.erase(0,1);
        while(!relative.empty() && relative.back()=='/') relative.pop_back();
        ScanPath p;
        p.path="/"+d->second+(relative.empty() ? std::string() : "/"+relative);
        p.depth=paths[i][2].null ? 1 : std::max(1,int(paths[i][2].number));
        out.push_back(p);
    }
    return true;
}

// Whether a scan path finds a game whose default.xex is in a folder of folder.
bool ScansGamesIn(const std::vector<ScanPath>& paths, const std::string& folder) {
    auto levels=[](const std::string& p) { int n=0; for(size_t i=0;i<p.size();++i) n+=p[i]=='/'; return n; };
    for(size_t i=0;i<paths.size();++i) {
        const std::string& p=paths[i].path;
        bool inside=!_stricmp(p.c_str(),folder.c_str()) || (folder.size()>p.size() && !_strnicmp(folder.c_str(),p.c_str(),p.size()) && folder[p.size()]=='/');
        if(inside && levels(folder)+1-levels(p)<=paths[i].depth) return true;
    }
    return false;
}

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
    bool aurora=ftp.Exists(o.aurora.c_str());
    j.Set("login",true).Set("aurora",aurora).Set("gameFolder",ftp.Exists(o.xboxFolder.c_str()));
    // Whether Aurora scans the game folders, and Hdd1:\Games (where Copy to Xbox puts original Xbox games).
    std::vector<ScanPath> scans;
    std::string scanError;
    if(aurora && AuroraScanPaths(ftp,o,scans,scanError)) {
        j.Key("scanPaths").Open('[');
        for(size_t i=0;i<scans.size();++i) j.Open('{').Set("path",scans[i].path).Set("depth",scans[i].depth).Close('}');
        j.Close(']').Set("scansGames",ScansGamesIn(scans,o.xboxFolder)).Set("scansHddGames",ScansGamesIn(scans,"/Hdd1/Games"));
    } else if(aurora) j.Set("scanError",scanError);
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
    std::string folder=XboxAppFolder(o);
    EnsureFolders(ftp,folder);
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
        if(text.empty() || text[0]!='/') { error="The game folder on the Xbox is an FTP path like /Hdd1/NetDisc360/Games."; return false; }
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
    Whole(v,"saveKeep",1,100,next.saveKeep,error);
    if(!error.empty()) return false;
    Bool(v,"wake",next.wake);
    Bool(v,"saveBackup",next.saveBackup);
    Bool(v,"saveFollow",next.saveFollow);
    if(Text(v,"wakeMac",text)) {
        next.wakeMac=wake::CleanMac(text);
        if(next.wakeMac.empty() && !Trim(text).empty()) { error="\""+text+"\" isn't a MAC address like AA:BB:CC:DD:EE:FF."; return false; }
    }
    Whole(v,"auroraWebPort",1,65535,next.auroraWebPort,error);
    if(!error.empty()) return false;
    Bool(v,"skipInstalled",next.skipInstalled);
    if(Text(v,"tileMode",text)) {
        if(!IsTileMode(text)) { error="Games come to a console as folder, god or none."; return false; }
        next.tileMode=text;
    }
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
    if(next.wake!=was.wake) changed.push_back(next.wake ? "the Xbox can wake this computer" : "the Xbox doesn't wake this computer");
    if(next.wakeMac!=was.wakeMac) changed.push_back(next.wakeMac.empty() ? "waking this computer's own network adapter" : "waking "+next.wakeMac);
    if(next.saveBackup!=was.saveBackup) changed.push_back(next.saveBackup ? "game saves backed up after each game" : "game saves not backed up");
    if(next.saveFollow!=was.saveFollow) changed.push_back(next.saveFollow ? "saves follow the profile to each console" : "saves stay on their console");
    if(next.saveKeep!=was.saveKeep) changed.push_back("keeping the newest "+Count(next.saveKeep,"save backup","save backups")+" of each game");
    if(next.skipInstalled!=was.skipInstalled) changed.push_back(next.skipInstalled ? "games installed on a console get no tile" : "installed games get a tile too");
    if(next.tileMode!=was.tileMode) changed.push_back("new consoles get the games "+std::string(next.tileMode==TileGod ? "as Games on Demand" : next.tileMode==TileNone ? "not at all" : "as folders in "+next.xboxFolder));
    if(next.installedFolders!=was.installedFolders) changed.push_back("installed games looked for in "+(next.installedFolders.empty() ? std::string("Games on Demand only") : next.installedFolders));
    if(next.auroraScripts!=was.auroraScripts) changed.push_back(next.auroraScripts ? "Aurora scripts on" : "Aurora scripts off");
    if(next.restartAurora!=was.restartAurora) changed.push_back(next.restartAurora ? "Aurora restarted when the PC games appear or go" : "Aurora never restarted");
    if(next.auroraWebUser!=was.auroraWebUser || next.auroraWebPassword!=was.auroraWebPassword || next.auroraWebPort!=was.auroraWebPort) changed.push_back("Aurora's web server login");
    if(next.followAddress!=was.followAddress) changed.push_back(next.followAddress ? "NetISO follows this PC's address" : "NetISO.xex.txt left alone");
    if(next.keepAwake!=was.keepAwake) changed.push_back(next.keepAwake ? "PC kept awake while a console plays" : "PC may sleep while a console plays");
    if(next.autoOffMinutes!=was.autoOffMinutes)
        changed.push_back(next.autoOffMinutes ? "consoles turned off "+Count(next.autoOffMinutes,"minute","minutes")+" after their last game" : "consoles not turned off after their last game");
    {
        std::lock_guard<std::mutex> hold(SettingsLock);
        Settings.folders=next.folders; Settings.xboxes=next.xboxes; Settings.ftpUser=next.ftpUser; Settings.ftpPassword=next.ftpPassword;
        Settings.xboxFolder=next.xboxFolder; Settings.aurora=next.aurora; Settings.push=next.push; Settings.filter=next.filter;
        Settings.scan=next.scan; Settings.setupDone=next.setupDone; Settings.wizardDone=next.wizardDone; Settings.webLan=next.webLan;
        Settings.backupEvery=next.backupEvery; Settings.backupKeep=next.backupKeep;
        Settings.updateCheck=next.updateCheck; Settings.updateAuto=next.updateAuto; Settings.updatePrerelease=next.updatePrerelease;
        Settings.wake=next.wake; Settings.wakeMac=next.wakeMac; Settings.saveBackup=next.saveBackup; Settings.saveKeep=next.saveKeep; Settings.saveFollow=next.saveFollow;
        Settings.skipInstalled=next.skipInstalled; Settings.installedFolders=next.installedFolders; Settings.auroraScripts=next.auroraScripts;
        Settings.restartAurora=next.restartAurora; Settings.auroraWebUser=next.auroraWebUser; Settings.auroraWebPassword=next.auroraWebPassword;
        Settings.auroraWebPort=next.auroraWebPort; Settings.followAddress=next.followAddress; Settings.keepAwake=next.keepAwake;
        Settings.autoOffMinutes=next.autoOffMinutes; Settings.tileMode=next.tileMode;
    }
    // Pre-releases in or out changes which release is the newest.
    if(next.updatePrerelease!=was.updatePrerelease || (next.updateCheck && !was.updateCheck)) CheckSoon=true;
    if(next.xboxes!=was.xboxes) UpdateConsoles(next.xboxes);
    // Anything the push depends on writes every console's folders again (only what differs).
    if(next.xboxes!=was.xboxes || login || next.xboxFolder!=was.xboxFolder || next.aurora!=was.aurora ||
       (next.push && !was.push) || (next.filter && !was.filter) || next.skipInstalled!=was.skipInstalled ||
       next.installedFolders!=was.installedFolders || (next.auroraScripts && !was.auroraScripts) ||
       next.wake!=was.wake || next.wakeMac!=was.wakeMac || next.tileMode!=was.tileMode) ++PushGeneration;
    if(next.webLan!=was.webLan) StartWeb(Current(),true);
    // Saves follow the profile again: every console is checked anew.
    if((next.saveFollow && !was.saveFollow) || (next.saveBackup && !was.saveBackup)) {
        { std::lock_guard<std::mutex> hold(FollowLock); FollowDone.clear(); }
        FollowAllSoon();
    }
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

// Sync between computers -------------------------------------------------------
//
// With a key (Settings, Sync between computers), the save backups (Saves\),
// the profile backups (Backups\Profiles\) and the play history go to the
// sync Worker on Cloudflare and come back on every other computer connected
// to it (see sync.h): when NetDisc360 starts, every 5 minutes, after each
// save backup and when asked. Each computer keeps its own settings.

const double SyncEvery = 300;
std::atomic<bool> SyncAsked(true);
struct SyncState {
    bool running;
    long long last, lastOk;  // time() of the last pass, and of the last one without a failure.
    int sent, got, failed;   // In the last pass.
    std::string error;       // Its first failure.
    std::vector<cloudsync::Device> devices;
    std::map<std::string,std::string> sentDocs;  // SHA-256 of each document last sent (plays.ini, users.ini).
    long long stored;        // Bytes kept in the cloud, as of the last pass that listed everything.
    SyncState() : running(false), last(0), lastOk(0), sent(0), got(0), failed(0), stored(0) {}
};
std::mutex SyncStateLock;
SyncState SyncNow;  // Under SyncStateLock.

void SyncSoon() { SyncAsked=true; }

std::string CloudFolder(const Options& o) { return o.configPath.empty() ? std::string() : config::FolderOf(o.configPath)+"Sync"; }

std::string InFolder(const std::string& folder, const std::string& name) {
#ifdef _WIN32
    return folder+"\\"+name;
#else
    return folder+"/"+name;
#endif
}

// The other devices' play history, from Sync\devices\<id>\plays.ini.
void LoadOtherPlays(const Options& o) {
    std::map<std::string,Played> all;
    std::string devices=InFolder(CloudFolder(o),"devices");
    std::vector<config::Entry> ids=o.syncKey.empty() ? std::vector<config::Entry>() : config::Folders(devices);
    for(size_t i=0;i<ids.size();++i) {
        std::string text;
        std::map<std::string,Played> one;
        if(ids[i].name==o.syncId || !config::ReadFile(InFolder(InFolder(devices,ids[i].name),"plays.ini"),text,16<<20)) continue;
        ReadPlays(text,one);
        for(std::map<std::string,Played>::const_iterator p=one.begin();p!=one.end();++p) {
            Played& sum=all[p->first];
            sum.times+=p->second.times; sum.seconds+=p->second.seconds; sum.last=std::max(sum.last,p->second.last);
        }
    }
    {
        std::lock_guard<std::mutex> hold(PlaysLock);
        OtherPlays.swap(all);
    }
    std::lock_guard<std::mutex> hold(GamesListLock);
    ++GamesVersion;
}

// The users of the other computers (Sync\devices\<id>\users.ini, see
// users.h on the party branch) merged into this one's users.ini: the same
// XUID is the same user, the line changed last wins, removals carry over.
// A merge that changed users.ini sends it up on the next pass.
// TODO(party branch): once users.h is in dev, call MergeUsersFile(text, error)
// for each copy here; until then the copies are only kept.
void MergeOtherUsers(const Options& o) {
    std::string devices=InFolder(CloudFolder(o),"devices");
    std::vector<config::Entry> ids=config::Folders(devices);
    int copies=0;
    for(size_t i=0;i<ids.size();++i) {
        std::string text;
        if(ids[i].name!=o.syncId && config::ReadFile(InFolder(InFolder(devices,ids[i].name),"users.ini"),text,16<<20)) ++copies;
    }
    if(copies) Log("Sync: the users of %d other computer(s) are kept under %s.",copies,config::FromUtf8(devices).c_str());
}

void SyncPass(const Options& o) {
    const std::string folder=CloudFolder(o), url=o.syncUrl, key=o.syncKey;
    config::MakeFolder(folder);
    { std::lock_guard<std::mutex> hold(SyncStateLock); SyncNow.running=true; }
    cloudsync::Report report;
    std::string base=config::FolderOf(o.configPath);
    cloudsync::Kind saves={"saves",base+"Saves",0};
    cloudsync::Kind profiles={"profiles",InFolder(base+"Backups","Profiles"),1};
    cloudsync::Files(url,key,saves,InFolder(folder,"saves.seen"),report);
    int savesGot=report.got;
    cloudsync::Files(url,key,profiles,InFolder(folder,"profiles.seen"),report);
    // The play history and the users: this computer's own go up, the
    // others' come down (to Sync\devices\<id>\).
    std::map<std::string,std::string> sent;
    std::string error;
    { std::lock_guard<std::mutex> hold(SyncStateLock); sent=SyncNow.sentDocs; }
    const std::string devicesFolder=InFolder(folder,"devices");
    const std::string map=DiscMapPath(o.configPath), configFolder=map.substr(0,map.size()-9);
    const char* const docs[]={"plays.ini","users.ini"};
    for(size_t d=0;d<sizeof(docs)/sizeof(docs[0]);++d) {
        std::string text;
        if(config::ReadFile(configFolder+docs[d],text,16<<20) && !cloudsync::PutState(url,key,o.syncId,docs[d],text,sent[docs[d]],error)) {
            ++report.failed; report.notes.push_back(error);
        }
        int before=report.got;
        cloudsync::GetStates(url,key,o.syncId,docs[d],devicesFolder,report);
        if(report.got==before) continue;
        if(d==0) LoadOtherPlays(o);
        else MergeOtherUsers(o);
    }
    if(savesGot) FollowAllSoon();
    std::vector<cloudsync::Device> devices;
    bool listed=cloudsync::Devices(url,key,devices,error);
    if(!listed) { ++report.failed; report.notes.push_back(error); }
    {
        std::lock_guard<std::mutex> hold(SyncStateLock);
        SyncNow.running=false;
        SyncNow.last=(long long)time(NULL);
        if(!report.failed) SyncNow.lastOk=SyncNow.last;
        SyncNow.sent=report.sent; SyncNow.got=report.got; SyncNow.failed=report.failed;
        SyncNow.error=report.notes.empty() ? std::string() : report.notes[0];
        SyncNow.sentDocs=sent;
        if(listed) SyncNow.devices=devices;
        if(!report.failed || report.stored>SyncNow.stored) SyncNow.stored=report.stored;
    }
    if(report.sent || report.got) Log("Sync: sent %d and fetched %d backup(s) and play histories%s.",report.sent,report.got,
                                      savesGot ? " (new save backups from your other computers are under Saves)" : "");
    for(size_t i=0;i<report.notes.size() && i<5;++i) Log("Sync: %s",config::FromUtf8(report.notes[i]).c_str());
}

void SyncLoop() {
    double next=0;
    for(;;) {
        Sleep(1000);
        if(!SyncAsked && Now()<next) continue;
        SyncAsked=false;
        next=Now()+SyncEvery;
        Options o=Current();
        if(o.syncKey.empty() || o.syncUrl.empty() || o.syncId.empty() || o.configPath.empty()) continue;
        SyncPass(o);
    }
}

std::string ComputerName() {
    char name[256]={0};
    if(gethostname(name,sizeof(name)-1) || !name[0]) return "NetDisc360";
    return config::ToUtf8(name);
}

void SyncJson(web::Json& j, const Options& o) {
    std::lock_guard<std::mutex> hold(SyncStateLock);
    j.Key("sync").Open('{');
    j.Set("on",!o.syncKey.empty()).Set("url",o.syncUrl).Set("id",o.syncId).Set("name",o.syncName).Set("computer",ComputerName());
    j.Set("saved",!o.configPath.empty()).Set("running",SyncNow.running).Set("last",SyncNow.last).Set("lastOk",SyncNow.lastOk);
    j.Set("sent",SyncNow.sent).Set("got",SyncNow.got).Set("failed",SyncNow.failed).Set("error",SyncNow.error).Set("stored",SyncNow.stored);
    j.Key("devices").Open('[');
    for(size_t i=0;i<SyncNow.devices.size();++i)
        j.Open('{').Set("id",SyncNow.devices[i].id).Set("name",SyncNow.devices[i].name).Set("added",SyncNow.devices[i].added)
         .Set("self",SyncNow.devices[i].id==o.syncId).Close('}');
    j.Close(']').Close('}');
}

// The dashboard's Sync between computers: connect (url, key: the owner key
// or another computer's, name), off, now, remove (id).
void SyncApi(const std::string& what, const web::Value& v, web::Reply& out) {
    Options o=Current();
    std::string error;
    if(what=="connect") {
        std::string url, key, name;
        Text(v,"url",url); Text(v,"key",key); Text(v,"name",name);
        url=cloudsync::Base(url); key=Trim(key); name=Trim(name);
        if(name.empty()) name=ComputerName();
        if(o.configPath.empty()) { out.Error(400,"The settings aren't kept in a file (start NetDisc360 with --config), so there's nowhere to sync to."); return; }
        if(key.empty()) { out.Error(400,"Paste the sync's key: the owner key, or a key from another computer's NetDisc360."); return; }
        std::string id, deviceKey;
        if(!cloudsync::Connect(url,key,name,id,deviceKey,error)) { out.Error(400,error); return; }
        {
            std::lock_guard<std::mutex> hold(SettingsLock);
            Settings.syncUrl=url; Settings.syncKey=deviceKey; Settings.syncId=id; Settings.syncName=name;
        }
        { std::lock_guard<std::mutex> hold(SyncStateLock); SyncNow=SyncState(); }
        Save();
        Log("Sync: this computer is connected to %s as \"%s\".",url.c_str(),config::FromUtf8(name).c_str());
        Event(EventGood,"Sync between computers is on");
        SyncSoon();
    } else if(what=="off") {
        {
            std::lock_guard<std::mutex> hold(SettingsLock);
            Settings.syncKey.clear(); Settings.syncId.clear();
        }
        { std::lock_guard<std::mutex> hold(SyncStateLock); SyncNow=SyncState(); }
        { std::lock_guard<std::mutex> hold(PlaysLock); OtherPlays.clear(); }
        { std::lock_guard<std::mutex> hold(GamesListLock); ++GamesVersion; }
        Save();
        Log("Sync: turned off on this computer (its backups stay here and in the cloud).");
    } else if(what=="now") {
        if(o.syncKey.empty()) { out.Error(400,"Sync isn't on."); return; }
        SyncSoon();
    } else if(what=="remove") {
        std::string id;
        Text(v,"id",id);
        if(o.syncKey.empty()) { out.Error(400,"Sync isn't on."); return; }
        if(!cloudsync::Remove(o.syncUrl,o.syncKey,id,error)) { out.Error(400,error); return; }
        Log("Sync: a computer's key was taken away (%s).",id.c_str());
        SyncSoon();
    } else { out.Error(404,"Not here."); return; }
    web::Json j;
    j.Open('{').Set("ok",true);
    SyncJson(j,Current());
    j.Close('}');
    out.body=j.Text();
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
        Settings.autoOffMinutes=o.autoOffMinutes; Settings.tileMode=o.tileMode;
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
    j.Open('{').Set("version",NETDISC_VERSION).Set("built",__DATE__ " " __TIME__);
    UpdateJson(j,Current(),true);
    j.Close('}');
    out.body=j.Text();
}

// Copying to the console: whole games, install content, title updates ----------
//
// Copy to Xbox puts a whole game on the console's hard drive, so it plays
// without the network, as a Games on Demand package (see CopyGame). A game
// whose play disc needs an install disc's content (GTA V) gets that too,
// copied from the install disc to Content, where the launcher and the game
// look for it (see CopyContent); an install disc on its own copies just its
// content.
//
// Everything that writes to a console goes in that console's queue and runs
// one after another (see CopyWorker), as Aurora's FTP server takes one
// upload at a time: two at once leave files the console can't read. A
// queue goes on while the dashboard is closed.
const char ContentRoot[] = "/Hdd1/Content";

struct ContentCopy {
    int id;
    DWORD address;
    std::string kind;                 // game, content, update, package, files, remove.
    bool whole;                       // kind game.
    std::string console, game, disc;  // The game asked for, and the disc copied (UTF-8, as listed).
    std::string label;                // What the queue calls it ("Title Update 5"); for an update its id is key.
    std::string key;                  // Tells apart two jobs of a kind for the same game (an update's id).
    std::string folder;               // game: the FTP path of the folder its package goes in.
    std::string phase;                // game: prepare (reading the disc for its hashes), then send.
    std::string state, error, current;  // state: queued, copying, done, failed or stopped; current: the file being sent.
    std::string note;                 // Something to tell about one that's done.
    U64 total, done, copied;          // copied: sent this time (files already there count in done only).
    int files, file, skipped;
    long long queuedAt, startedAt, endedAt;  // time().
    double began;                     // Now() when sending started, for the speed.
    std::atomic<bool> stop;
    std::function<void(std::shared_ptr<ContentCopy>)> run;
    ContentCopy() : id(0), address(0), whole(false), total(0), done(0), copied(0), files(0), file(0), skipped(0),
                    queuedAt(0), startedAt(0), endedAt(0), began(0), stop(false) {}
};
std::mutex CopyLock;  // Guards the fields of each ContentCopy that change while it runs, and the lists below.
std::vector<std::shared_ptr<ContentCopy> > Copies;  // The queued, running and latest finished ones, oldest first.
std::set<DWORD> CopyWorkers;  // Consoles whose queue runs now.
int NextCopyId=1;
void WaitForPush(DWORD address);

std::string SizeText(U64 n) {
    char text[32];
    if(n>=1000000000ULL) _snprintf_s(text,sizeof(text),_TRUNCATE,"%.1f GB",n/1e9);
    else if(n>=1000000ULL) _snprintf_s(text,sizeof(text),_TRUNCATE,"%.0f MB",n/1e6);
    else _snprintf_s(text,sizeof(text),_TRUNCATE,"%.0f KB",n/1e3);
    return text;
}

void EndCopy(ContentCopy& c, const Console* console, const char* state, const std::string& error) {
    {
        std::lock_guard<std::mutex> hold(CopyLock);
        if(c.state!="copying" && c.state!="queued") return;  // Ended already.
        c.state=state; c.error=error; c.endedAt=(long long)time(NULL); c.current.clear();
    }
    char where[16]; AddressText(c.address,where,sizeof(where));
    std::string text;
    int kind=EventGood;
    bool done=!strcmp(state,"done"), stopped=!strcmp(state,"stopped");
    if(c.kind=="game") {
        if(done) text=c.game+" is on the hard drive ("+SizeText(c.total)+")";
        else if(stopped) { text="Stopped copying "+c.game+" to the hard drive"; kind=EventInfo; }
        else { text="Copying "+c.game+" to the hard drive stopped; the log says why"; kind=EventWarning; }
    } else if(c.kind=="update") {
        if(done) text=c.label+" for "+c.game+" is on the console"+(c.skipped ? " (it was there already)" : "");
        else if(stopped) { text="Stopped putting "+c.label+" for "+c.game+" on the console"; kind=EventInfo; }
        else { text="Putting "+c.label+" for "+c.game+" on the console stopped; the log says why"; kind=EventWarning; }
    } else if(c.kind=="package") {
        if(done) text=c.game+" is on the console";
        else if(stopped) { text="Stopped copying "+c.game; kind=EventInfo; }
        else { text="Copying "+c.game+" stopped; the log says why"; kind=EventWarning; }
    } else if(c.kind=="files") {
        if(done) text=c.game+" is on the hard drive ("+SizeText(c.total)+")";
        else if(stopped) { text="Stopped copying "+c.game+" to the hard drive"; kind=EventInfo; }
        else { text="Copying "+c.game+" to the hard drive stopped; the log says why"; kind=EventWarning; }
    } else if(c.kind=="tuoff") {
        if(done) text=c.game+" starts without a title update";
        else if(stopped) { text="Didn't turn "+c.game+"'s title updates off"; kind=EventInfo; }
        else { text="Turning "+c.game+"'s title updates off stopped; the log says why"; kind=EventWarning; }
    } else if(c.kind=="check") {
        if(stopped) { text="Didn't check its title updates"; kind=EventInfo; }
        else { text="Couldn't read its title updates; the log says why"; kind=EventWarning; }
    } else if(c.kind=="remove") {
        if(done) text=c.game+" is off the console's hard drive";
        else if(stopped) { text="Didn't remove "+c.game; kind=EventInfo; }
        else { text="Removing "+c.game+" from the console stopped; the log says why"; kind=EventWarning; }
    }
    else if(done) text=c.game+"'s content is installed ("+SizeText(c.total)+")";
    else if(stopped) { text="Stopped installing "+c.game+"'s content"; kind=EventInfo; }
    else { text="Installing "+c.game+"'s content stopped; the log says why"; kind=EventWarning; }
    Log("Xbox %s: %s%s%s",where,text.c_str(),error.empty() ? "." : ": ",error.c_str());
    if(console) XboxEvent(*console,kind,text);
    else Event(kind,"%s on %s",text.c_str(),where);
}

// Runs a console's queue, oldest first, until it's empty.
void CopyWorker(DWORD at) {
    for(;;) {
        std::shared_ptr<ContentCopy> job;
        {
            std::lock_guard<std::mutex> hold(CopyLock);
            for(size_t i=0;i<Copies.size() && !job;++i) if(Copies[i]->address==at && Copies[i]->state=="queued") job=Copies[i];
            if(!job) { CopyWorkers.erase(at); return; }
            job->state="copying"; job->startedAt=(long long)time(NULL); job->began=Now();
        }
        job->run(job);
        // A job that returned without ending (it shouldn't) mustn't hold the queue.
        EndCopy(*job,ConsoleIfKnown(at),"failed","It stopped without saying why.");
    }
}

// Makes the folders of an FTP path (a file's), from below root, a level at a time.
bool MakeFolders(FtpClient& ftp, const std::string& path, size_t from, std::string& error) {
    for(size_t at=path.find('/',from+1);at!=std::string::npos;at=path.find('/',at+1)) {
        std::string step=path.substr(0,at);
        if(ftp.Enters(step.c_str())) continue;
        // A failed MKD costs FtpDll's connection: only for one that isn't there.
        if(ftp.Code()/100!=5 || !ftp.MakeFolder(step.c_str())) { error="Couldn't make "+step+" on the console: "+ftp.Error(); return false; }
    }
    return true;
}

// The size of a file on the console, by its folder's listing; -1 if it isn't there.
long long RemoteSize(FtpClient& ftp, const std::string& path) {
    size_t slash=path.find_last_of('/');
    std::vector<FtpEntry> there;
    if(slash==std::string::npos || !ftp.List(path.substr(0,slash).c_str(),there)) return -1;
    for(size_t k=0;k<there.size();++k)
        if(!there[k].folder && !_stricmp(there[k].name.c_str(),path.c_str()+slash+1)) return (long long)there[k].size;
    return -1;
}

// Opens the console's FTP for a job, after a push that started first.
bool OpenFor(ContentCopy& c, FtpClient& ftp, const Console* console) {
    WaitForPush(c.address);
    if(ftp.Open()) return true;
    EndCopy(c,console,"failed",std::string(ftp.Error())+". Start Aurora on the console and try again.");
    return false;
}

// Copies the install disc's packages to the console, leaving out each one
// already there at its full size; a package cut short is deleted, so the
// launcher never takes it for installed.
void CopyContent(std::shared_ptr<ContentCopy> job, IsoFile disc) {
    ContentCopy& c=*job;
    Options o=Current();
    const Console* console=ConsoleIfKnown(c.address);
    FtpClient ftp(c.address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),30);
    if(!OpenFor(c,ftp,console)) return;
    IsoReader reader;
    if(!reader.Open(disc)) { EndCopy(c,console,"failed",WithoutIso(disc.name)+" can't be read here: "+reader.Note()); return; }
    for(size_t i=0;i<disc.packages.size();++i) {
        const Package& p=disc.packages[i];
        { std::lock_guard<std::mutex> hold(CopyLock); c.file=int(i+1); c.current=p.path.substr(p.path.find_last_of('\\')+1); }
        if(c.stop) { EndCopy(c,console,"stopped",""); return; }
        std::string remote=std::string(ContentRoot)+"/"+p.path;
        for(size_t k=0;k<remote.size();++k) if(remote[k]=='\\') remote[k]='/';
        std::string error;
        if(!MakeFolders(ftp,remote,strlen(ContentRoot)-1,error)) { EndCopy(c,console,"failed",error); return; }
        if(RemoteSize(ftp,remote)==(long long)p.size) {
            Log("Xbox %s: %s is installed already (%llu bytes).",c.console.c_str(),remote.c_str(),(unsigned long long)p.size);
            std::lock_guard<std::mutex> hold(CopyLock);
            c.done+=p.size; ++c.skipped;
            continue;
        }
        U64 at=0,size=0;
        std::string source=std::string("content\\")+(p.source.empty() ? p.path : p.source);
        if(!reader.Locate(source,at,size) || size!=p.size) {
            EndCopy(c,console,"failed","Couldn't find "+source+" on "+WithoutIso(disc.name)+" any more");
            return;
        }
        Log("Xbox %s: copying %s (%llu bytes) to %s.",c.console.c_str(),source.c_str(),(unsigned long long)p.size,remote.c_str());
        U64 before=c.done;
        bool ok=ftp.StoreFrom(remote.c_str(),p.size,
            [&](U64 offset, unsigned char* data, DWORD length) { return reader.Read(at+offset,data,length); },
            [&](U64 sent) {
                std::lock_guard<std::mutex> hold(CopyLock);
                c.copied+=before+sent-c.done; c.done=before+sent;
                return !c.stop.load();
            });
        if(!ok) {
            std::string why=ftp.Error();
            if(!ftp.Delete(remote.c_str()) && !ftp.Missing()) Log("Xbox %s: couldn't delete the part of %s that was sent: %s",c.console.c_str(),remote.c_str(),ftp.Error());
            if(c.stop) EndCopy(c,console,"stopped","");
            else EndCopy(c,console,"failed","Sending "+remote+" stopped: "+why);
            return;
        }
    }
    ftp.Close();
    EndCopy(c,console,"done","");
}

// The console a copy goes to: address, or with none the only console; never
// one this server doesn't know, nor one in a game (its FTP server only runs
// in Aurora).
bool CopyTarget(const std::string& address, DWORD& at, std::string& error) {
    std::vector<Console*> consoles=AllConsoles();
    Options o=Current();
    at=INADDR_NONE;
    if(address.empty()) {
        if(consoles.empty()) { error="No console has connected yet."; return false; }
        if(consoles.size()>1) { error="Choose the console to copy it to."; return false; }
        at=consoles[0]->address;
    } else {
        at=inet_addr(address.c_str());
        // Only a console this server knows: the button mustn't send files anywhere else.
        bool known=std::find(o.xboxes.begin(),o.xboxes.end(),at)!=o.xboxes.end();
        for(size_t i=0;i<consoles.size() && !known;++i) known=consoles[i]->address==at;
        if(at==INADDR_NONE || !known) { error="There's no console at "+address+"."; return false; }
    }
    Console* console=ConsoleIfKnown(at);
    if(console) {
        std::lock_guard<std::mutex> hold(ConsoleLock);
        if(console->connections>0 && !console->mounted.empty() && !console->inAurora) {
            error=std::string(console->text)+" is playing a game. Go back to Aurora first; its FTP server only runs there.";
            return false;
        }
    }
    return true;
}

// Puts job at the end of the console's queue, unless the same is in it
// already, and starts the queue if it isn't running.
bool AddCopy(std::shared_ptr<ContentCopy> job, DWORD at, std::string& error) {
    char text[16]; AddressText(at,text,sizeof(text));
    job->address=at; job->console=text;
    job->state="queued"; job->queuedAt=(long long)time(NULL);
    if(job->kind.empty()) job->kind=job->whole ? "game" : "content";
    std::lock_guard<std::mutex> hold(CopyLock);
    int ended=0;
    for(size_t i=Copies.size();i-->0;) {
        ContentCopy& c=*Copies[i];
        if(c.address!=at) continue;
        bool waiting=c.state=="queued" || c.state=="copying";
        if(waiting && c.kind==job->kind && c.game==job->game && c.disc==job->disc && c.key==job->key) {
            error=c.game+(c.state=="queued" ? " is in "+std::string(text)+"'s queue already." : " is being copied to "+std::string(text)+" already.");
            return false;
        }
        // The latest few finished ones stay, for the dashboard to show how they ended.
        if(!waiting && ++ended>12) Copies.erase(Copies.begin()+long(i));
    }
    job->id=NextCopyId++;
    Copies.push_back(job);
    if(CopyWorkers.insert(at).second) std::thread(CopyWorker,at).detach();
    return true;
}

std::string ToLowerAscii(std::string text) {
    for(size_t i=0;i<text.size();++i) text[i]=char(tolower((unsigned char)text[i]));
    return text;
}

// Copies a game to the console as a Games on Demand package, made from its
// disc while it is sent (see GodPackage): first reads the disc for the
// hashes, then sends the Data files, leaving out each one already there at
// its full size, so a copy stopped part way goes on where it was, and the
// header last, so Aurora never lists a package that isn't whole. A file cut
// short is deleted.
// A game a copy just put on the console, in its installed games, so the
// dashboard shows it there before the next look (the push's).
void MarkInstalled(DWORD address, DWORD titleId, int disc, const std::string& path, bool god) {
    Console* known=ConsoleIfKnown(address);
    if(!known || !titleId) return;
    std::lock_guard<std::mutex> hold(ConsoleLock);
    std::vector<InstalledGame>& list=known->installed;
    for(size_t i=0;i<list.size();++i) if(!_stricmp(list[i].path.c_str(),path.c_str())) return;
    InstalledGame game; game.titleId=titleId; game.disc=disc ? disc : 1; game.path=path; game.god=god;
    list.push_back(game);
}

void CopyGame(std::shared_ptr<ContentCopy> job, IsoFile disc) {
    ContentCopy& c=*job;
    Options o=Current();
    const Console* console=ConsoleIfKnown(c.address);
    IsoReader reader;
    if(!reader.Open(disc)) { EndCopy(c,console,"failed",WithoutIso(disc.name)+" can't be read here: "+reader.Note()); return; }
    GodPackage god;
    std::string error;
    { std::lock_guard<std::mutex> hold(CopyLock); c.phase="prepare"; }
    // Named and with its icon in the Xbox home as a tile's package is (see GodArtFor).
    std::string title=c.game;
    if(disc.titleId) { GodArt art=GodArtFor(o,disc.titleId,disc.name); title=art.name; god.SetIcon(art.icon); }
    bool made=god.Make(reader,title,[&](U64 done, U64 total) {
        std::lock_guard<std::mutex> hold(CopyLock);
        c.done=done; c.total=total;
        return !c.stop.load();
    },error);
    if(!made) {
        if(c.stop) EndCopy(c,console,"stopped","");
        else EndCopy(c,console,"failed",WithoutIso(disc.name)+" can't be made a Games on Demand package: "+error);
        return;
    }
    std::string folder=std::string(ContentRoot)+"/0000000000000000";
    std::string data=folder+"/"+god.Folder()+"/"+god.Name()+".data";
    for(size_t i=0;i<data.size();++i) if(data[i]=='\\') data[i]='/';
    std::string header=data.substr(0,data.size()-5);
    {
        std::lock_guard<std::mutex> hold(CopyLock);
        c.phase="send"; c.folder=header.substr(0,header.find_last_of('/'));
        c.done=0; c.copied=0; c.total=god.Bytes(); c.files=god.Parts()+1; c.began=Now();
    }
    Log("Xbox %s: sending %s as Games on Demand: %d Data file(s) and a header, %s, to %s.",c.console.c_str(),c.game.c_str(),god.Parts(),
        SizeText(c.total).c_str(),header.c_str());
    FtpClient ftp(c.address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),30);
    if(!OpenFor(c,ftp,console)) return;
    // Content\0000000000000000\<Title ID>\00007000\<Media ID>.data, a folder at a time.
    if(!MakeFolders(ftp,data+"/",strlen(ContentRoot)-1,error)) { EndCopy(c,console,"failed",error); return; }
    // A whole package there already: nothing to send.
    if(RemoteSize(ftp,header)==(long long)god.Header().size()) {
        Log("Xbox %s: %s is there already.",c.console.c_str(),header.c_str());
        { std::lock_guard<std::mutex> hold(CopyLock); c.done=c.total; c.skipped=c.files; c.file=c.files; }
        ftp.Close();
        EndCopy(c,console,"done","");
        return;
    }
    std::vector<FtpEntry> there;
    std::map<std::string,U64> sizes;
    if(ftp.List(data.c_str(),there)) for(size_t k=0;k<there.size();++k) if(!there[k].folder) sizes[ToLowerAscii(there[k].name)]=there[k].size;
    for(int part=0;part<=god.Parts();++part) {
        bool last=part==god.Parts();
        char name[16]; _snprintf_s(name,sizeof(name),_TRUNCATE,"Data%04d",part);
        std::string remote=last ? header : data+"/"+name;
        U64 size=last ? god.Header().size() : god.PartSize(part);
        { std::lock_guard<std::mutex> hold(CopyLock); c.file=part+1; c.current=last ? god.Name() : name; }
        if(c.stop) { EndCopy(c,console,"stopped",""); return; }
        std::map<std::string,U64>::const_iterator old=last ? sizes.end() : sizes.find(ToLowerAscii(name));
        if(old!=sizes.end() && old->second==size) {
            std::lock_guard<std::mutex> hold(CopyLock);
            c.done+=size; ++c.skipped;
            continue;
        }
        U64 before=c.done;
        bool ok=ftp.StoreFrom(remote.c_str(),size,
            [&](U64 offset, unsigned char* bytes, DWORD length) {
                if(!last) return god.Part(part,offset,bytes,length);
                memcpy(bytes,&god.Header()[size_t(offset)],length);
                return true;
            },
            [&](U64 sent) {
                std::lock_guard<std::mutex> hold(CopyLock);
                c.copied+=before+sent-c.done; c.done=before+sent;
                return !c.stop.load();
            });
        if(!ok) {
            std::string why=ftp.Error();
            if(!ftp.Delete(remote.c_str()) && !ftp.Missing()) Log("Xbox %s: couldn't delete the part of %s that was sent: %s",c.console.c_str(),remote.c_str(),ftp.Error());
            if(c.stop) EndCopy(c,console,"stopped","");
            else EndCopy(c,console,"failed","Sending "+remote+" stopped: "+why);
            return;
        }
    }
    std::string reply;
    if(ftp.Site((std::string("INITIATESCAN ")+folder).c_str(),reply) && reply.find("Unable")==std::string::npos)
        Log("Xbox %s: asked Aurora to scan %s, so it lists %s.",c.console.c_str(),folder.c_str(),c.game.c_str());
    else Log("Xbox %s: Aurora didn't start a scan of %s: %s",c.console.c_str(),folder.c_str(),reply.empty() ? ftp.Error() : reply.c_str());
    ftp.Close();
    MarkInstalled(c.address,disc.titleId,disc.disc,header,true);
    EndCopy(c,console,"done","");
}

// Copies an original Xbox game to the console's hard drive as an extracted
// game, its disc's files in a folder of GamesRoot, which Aurora starts in
// the console's emulator as any extracted original Xbox game. Files already
// there at their full size are left out, so a stopped copy goes on.
const char GamesRoot[] = "/Hdd1/Games";
void CopyGameFiles(std::shared_ptr<ContentCopy> job, IsoFile disc) {
    ContentCopy& c=*job;
    Options o=Current();
    const Console* console=ConsoleIfKnown(c.address);
    IsoReader reader;
    if(!reader.Open(disc)) { EndCopy(c,console,"failed",WithoutIso(disc.name)+" can't be read here: "+reader.Note()); return; }
    std::vector<DiscFile> files;
    if(!reader.Files(files)) { EndCopy(c,console,"failed","The files on "+WithoutIso(disc.name)+" can't be read: "+reader.Note()); return; }
    U64 total=0;
    int count=0;
    for(size_t i=0;i<files.size();++i) {
        const DiscFile& f=files[i];
        if(f.path.empty() || f.path[f.path.size()-1]=='\\') continue;
        total+=f.size; ++count;
        // FATX takes names of up to 42 characters.
        std::string leaf=f.path.substr(f.path.find_last_of('\\')+1);
        if(leaf.size()>42) { EndCopy(c,console,"failed",f.path+" has a name too long for the console's hard drive (FATX takes 42 characters)"); return; }
    }
    std::string name=tu::FatxName(config::ToUtf8(WithoutIso(disc.name)));
    while(!name.empty() && (name.back()==' ' || name.back()=='.')) name.pop_back();
    if(name.empty()) { char text[12]; _snprintf_s(text,sizeof(text),_TRUNCATE,"%08X",(unsigned)disc.titleId); name=text; }
    std::string root=std::string(GamesRoot)+"/"+name;
    {
        std::lock_guard<std::mutex> hold(CopyLock);
        c.phase="send"; c.folder=root; c.total=total; c.files=count; c.done=0; c.copied=0; c.began=Now();
    }
    Log("Xbox %s: copying %s's %d files (%s) to %s.",c.console.c_str(),c.game.c_str(),count,SizeText(total).c_str(),root.c_str());
    FtpClient ftp(c.address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),30);
    if(!OpenFor(c,ftp,console)) return;
    std::string error;
    if(!MakeFolders(ftp,root+"/",strlen("/Hdd1")-1,error)) { EndCopy(c,console,"failed",error); return; }
    std::map<std::string,std::map<std::string,U64> > listed;  // Folder (lower case): its files' sizes.
    int file=0;
    for(size_t i=0;i<files.size();++i) {
        const DiscFile& f=files[i];
        std::string remote=root+"/"+f.path;
        for(size_t k=0;k<remote.size();++k) if(remote[k]=='\\') remote[k]='/';
        if(c.stop) { EndCopy(c,console,"stopped",""); return; }
        if(remote[remote.size()-1]=='/') {
            remote.erase(remote.size()-1);
            if(!ftp.Enters(remote.c_str()) && (ftp.Code()/100!=5 || !ftp.MakeFolder(remote.c_str()))) {
                EndCopy(c,console,"failed","Couldn't make "+remote+" on the console: "+ftp.Error()); return;
            }
            continue;
        }
        size_t slash=remote.find_last_of('/');
        std::string folder=ToLowerAscii(remote.substr(0,slash)), leaf=ToLowerAscii(remote.substr(slash+1));
        if(!listed.count(folder)) {
            std::vector<FtpEntry> there;
            std::map<std::string,U64>& sizes=listed[folder];
            if(ftp.List(remote.substr(0,slash).c_str(),there)) for(size_t k=0;k<there.size();++k) if(!there[k].folder) sizes[ToLowerAscii(there[k].name)]=there[k].size;
        }
        { std::lock_guard<std::mutex> hold(CopyLock); c.file=++file; c.current=f.path; }
        std::map<std::string,U64>::const_iterator old=listed[folder].find(leaf);
        if(old!=listed[folder].end() && old->second==f.size) {
            std::lock_guard<std::mutex> hold(CopyLock);
            c.done+=f.size; ++c.skipped;
            continue;
        }
        U64 before=c.done;
        bool ok=ftp.StoreFrom(remote.c_str(),f.size,
            [&](U64 offset, unsigned char* data, DWORD length) { return reader.Read(f.at+offset,data,length); },
            [&](U64 sent) {
                std::lock_guard<std::mutex> hold(CopyLock);
                c.copied+=before+sent-c.done; c.done=before+sent;
                return !c.stop.load();
            });
        if(!ok) {
            std::string why=ftp.Error();
            if(!ftp.Delete(remote.c_str()) && !ftp.Missing()) Log("Xbox %s: couldn't delete the part of %s that was sent: %s",c.console.c_str(),remote.c_str(),ftp.Error());
            if(c.stop) EndCopy(c,console,"stopped","");
            else EndCopy(c,console,"failed","Sending "+remote+" stopped: "+why);
            return;
        }
    }
    std::string reply;
    if(ftp.Site((std::string("INITIATESCAN ")+GamesRoot).c_str(),reply) && reply.find("Unable")==std::string::npos)
        Log("Xbox %s: asked Aurora to scan %s, so it lists %s.",c.console.c_str(),GamesRoot,c.game.c_str());
    else Log("Xbox %s: Aurora didn't start a scan of %s: %s (add it under Settings, Content, Scan paths if Aurora doesn't list the game).",c.console.c_str(),GamesRoot,reply.empty() ? ftp.Error() : reply.c_str());
    ftp.Close();
    MarkInstalled(c.address,disc.titleId,1,root+"/default.xbe",false);
    EndCopy(c,console,"done","");
}


// Queues a disc for the console: as Games on Demand, or its content for an
// install disc (game: the game it's for, as listed), or its files for an
// original Xbox game.
bool QueueDisc(const IsoFile& disc, const std::string& game, DWORD at, std::string& error) {
    std::string name=config::ToUtf8(WithoutIso(disc.name));
    std::shared_ptr<ContentCopy> job(new ContentCopy);
    job->game=game.empty() ? name : game; job->disc=name;
    if(!disc.packages.empty() && (disc.role=="install" || disc.role=="content" || !disc.mediaId)) {
        job->kind="content";
        job->label=name+"'s content";
        job->files=int(disc.packages.size());
        for(size_t i=0;i<disc.packages.size();++i) job->total+=disc.packages[i].size;
        job->run=[disc](std::shared_ptr<ContentCopy> j) { CopyContent(j,disc); };
    } else if(disc.titleId && disc.mediaId) {
        job->kind="game"; job->whole=true;
        job->game=name; job->label=name;
        job->total=disc.size; job->files=1;
        job->run=[disc](std::shared_ptr<ContentCopy> j) { CopyGame(j,disc); };
    } else if(disc.titleId && !disc.mediaId) {
        job->kind="files"; job->game=name; job->label=name;
        job->total=disc.size; job->files=1;
        job->run=[disc](std::shared_ptr<ContentCopy> j) { CopyGameFiles(j,disc); };
    } else {
        error=name+" isn't a game that can be copied: it has no default.xex (or default.xbe) that says what it is.";
        return false;
    }
    if(!AddCopy(job,at,error)) return false;
    Log("Xbox %s: queued %s (%s, from %s).",job->console.c_str(),job->label.c_str(),job->kind.c_str(),disc.path.c_str());
    // A disc that's played and carries packages too (Splinter Cell:
    // Blacklist's second disc and its textures, Halo: CE Anniversary's
    // Reach maps): its content after it.
    if(job->kind=="game" && !disc.packages.empty()) {
        IsoFile content=disc;
        content.role="install";
        std::string why;
        if(!QueueDisc(content,name,at,why)) Log("Xbox %s: %s's content isn't queued: %s",job->console.c_str(),name.c_str(),why.c_str());
    }
    return true;
}

// Copy to Xbox: the game listed as name to the console at address (with
// none, the only console). A play disc brings its install disc's content
// along; all: every disc of the game, in disc order.
bool StartGameCopy(const web::Value& body, std::string& error) {
    std::string name, address;
    Text(body,"game",name);
    Text(body,"address",address);
    const web::Value* every=body.Get("all");
    bool all=every && every->type==web::Value::Bool && every->boolean;
    std::vector<std::pair<IsoFile,std::string> > discs;  // A disc, and the game it's for.
    {
        std::lock_guard<std::mutex> hold(GamesListLock);
        const IsoFile* g=NULL;
        for(size_t i=0;i<GamesList.size() && !g;++i) if(config::ToUtf8(WithoutIso(GamesList[i].name))==name) g=&GamesList[i];
        if(!g) { error="There's no game called "+name+" here."; return false; }
        std::vector<const IsoFile*> chosen;
        if(all && g->titleId) {
            for(size_t i=0;i<GamesList.size();++i) if(GamesList[i].titleId==g->titleId) chosen.push_back(&GamesList[i]);
            std::stable_sort(chosen.begin(),chosen.end(),[](const IsoFile* a, const IsoFile* b) {
                // Install discs after the game, as the game's content.
                bool ai=!a->packages.empty() && a->role=="install", bi=!b->packages.empty() && b->role=="install";
                return ai!=bi ? bi : a->disc<b->disc;
            });
        } else chosen.push_back(g);
        for(size_t i=0;i<chosen.size();++i) {
            const IsoFile* d=chosen[i];
            discs.push_back(std::make_pair(*d,std::string()));
            // A play disc's install content goes along.
            if(d->packages.empty() && !d->installDisc.empty())
                for(size_t k=0;k<GamesList.size();++k)
                    if(GamesList[k].name==d->installDisc && std::find(chosen.begin(),chosen.end(),&GamesList[k])==chosen.end())
                        discs.push_back(std::make_pair(GamesList[k],config::ToUtf8(WithoutIso(d->name))));
        }
    }
    DWORD at=INADDR_NONE;
    if(!CopyTarget(address,at,error)) return false;
    int queued=0;
    std::string why;
    for(size_t i=0;i<discs.size();++i) {
        std::string one;
        if(QueueDisc(discs[i].first,discs[i].second,at,one)) ++queued;
        else if(why.empty()) why=one;
    }
    if(!queued) { error=why; return false; }
    if(Console* console=ConsoleIfKnown(at)) XboxEvent(*console,EventInfo,queued>1 ? "Copying "+std::to_string(queued)+" discs of "+name+" to the hard drive" : "Copying "+name+" to the hard drive");
    return true;
}

// The Install content API of before: the content of name's install disc.
bool StartCopy(const web::Value& body, std::string& error) {
    std::string name, address;
    Text(body,"game",name);
    Text(body,"address",address);
    IsoFile disc;
    std::string game;
    {
        std::lock_guard<std::mutex> hold(GamesListLock);
        const IsoFile* g=NULL;
        for(size_t i=0;i<GamesList.size() && !g;++i) if(config::ToUtf8(WithoutIso(GamesList[i].name))==name) g=&GamesList[i];
        if(!g) { error="There's no game called "+name+" here."; return false; }
        const IsoFile* carrier=g->packages.empty() ? NULL : g;
        for(size_t i=0;i<GamesList.size() && !carrier && !g->installDisc.empty();++i) if(GamesList[i].name==g->installDisc) carrier=&GamesList[i];
        if(!carrier) { error=name+" has no content to install."; return false; }
        disc=*carrier; game=config::ToUtf8(WithoutIso(g->name));
    }
    disc.role="install";  // Its content, even of a disc that's also played.
    DWORD at=INADDR_NONE;
    if(!CopyTarget(address,at,error)) return false;
    if(!QueueDisc(disc,game,at,error)) return false;
    if(Console* console=ConsoleIfKnown(at)) XboxEvent(*console,EventInfo,"Installing "+game+"'s content");
    return true;
}

std::set<DWORD> Writing;  // Consoles whose push thread writes now (CopyLock).

PushMark::PushMark(DWORD at) : address(at), marked(false) {
    std::lock_guard<std::mutex> hold(CopyLock);
    for(size_t i=0;i<Copies.size();++i) if(Copies[i]->address==address && Copies[i]->state=="copying") return;
    Writing.insert(address); marked=true;
}

PushMark::~PushMark() {
    if(!marked) return;
    std::lock_guard<std::mutex> hold(CopyLock);
    Writing.erase(address);
}

// Before a copy's first write: lets a push that started before it finish.
void WaitForPush(DWORD address) {
    for(int i=0;i<1200;++i) {
        { std::lock_guard<std::mutex> hold(CopyLock); if(!Writing.count(address)) return; }
        Sleep(100);
    }
}

// Stops the job id (one waiting is taken out of the queue), or with no id
// the one running on the console at address; all: that and its whole queue.
bool StopCopy(const web::Value& body, std::string& error) {
    std::string address;
    Text(body,"address",address);
    const web::Value* id=body.Get("id");
    const web::Value* every=body.Get("all");
    bool all=every && every->type==web::Value::Bool && every->boolean;
    int wanted=id && id->type==web::Value::Number ? int(id->number) : 0;
    std::vector<std::shared_ptr<ContentCopy> > dropped;
    bool found=false;
    {
        std::lock_guard<std::mutex> hold(CopyLock);
        for(size_t i=0;i<Copies.size();++i) {
            ContentCopy& c=*Copies[i];
            if(wanted ? c.id!=wanted : c.console!=address) continue;
            if(c.state=="copying") { c.stop=true; found=true; }
            else if(c.state=="queued" && (wanted || all)) { dropped.push_back(Copies[i]); found=true; }
        }
    }
    for(size_t i=0;i<dropped.size();++i) EndCopy(*dropped[i],ConsoleIfKnown(dropped[i]->address),"stopped","");
    if(!found) error="Nothing is being copied to "+(address.empty() ? std::string("that console") : address)+".";
    return found;
}

void CopiesJson(web::Json& j) {
    std::lock_guard<std::mutex> hold(CopyLock);
    j.Key("installs").Open('[');
    std::map<DWORD,int> place;  // Each console's queue, counted.
    for(size_t i=0;i<Copies.size();++i) {
        const ContentCopy& c=*Copies[i];
        double seconds=(c.endedAt ? double(c.endedAt-c.startedAt) : Now()-c.began);
        j.Open('{').Set("id",c.id).Set("kind",c.kind).Set("label",c.label).Set("key",c.key).Set("folder",c.folder).Set("phase",c.phase).Set("address",c.console).Set("game",c.game).Set("disc",c.disc).Set("state",c.state).Set("error",config::ToUtf8(c.error)).Set("note",c.note);
        if(c.state=="queued") j.Set("place",++place[c.address]);
        j.Set("file",c.file).Set("files",c.files).Set("skipped",c.skipped).Set("current",c.current);
        j.Set("total",(unsigned long long)c.total).Set("done",(unsigned long long)c.done);
        j.Set("rate",c.state!="queued" && seconds>1 ? double(c.copied)/seconds : 0.0).Set("queued",c.queuedAt).Set("started",c.startedAt).Set("ended",c.endedAt).Close('}');
    }
    j.Close(']');
}

// Removing a game from a console's hard drive -----------------------------------
//
// Only the game's own copy: its Games on Demand package (the header file and
// its .data folder) or its extracted folder. Never its title updates, DLC,
// saves or other discs, which are in other folders.

// Deletes folder and everything in it, a level at a time.
bool DeleteTree(FtpClient& ftp, const std::string& folder, int depth, std::string& error) {
    if(depth>8) { error="It goes deeper than expected: "+folder; return false; }
    std::vector<FtpEntry> there;
    if(!ftp.List(folder.c_str(),there)) { error=std::string("Couldn't list ")+folder+": "+ftp.Error(); return false; }
    for(size_t i=0;i<there.size();++i) {
        const std::string& name=there[i].name;
        if(name=="." || name=="..") continue;
        std::string path=folder+"/"+name;
        if(there[i].folder) { if(!DeleteTree(ftp,path,depth+1,error)) return false; }
        else if(!ftp.Delete(path.c_str())) { error="Couldn't delete "+path+": "+ftp.Error(); return false; }
    }
    if(!ftp.RemoveFolder(folder.c_str())) { error="Couldn't remove "+folder+": "+ftp.Error(); return false; }
    return true;
}

// Whether path is a GOD package header the console has: <drive>/Content/0000000000000000/<Title ID>/00007000/<8 hex>.
bool GodHeaderPath(const std::string& path) {
    std::vector<std::string> parts;
    for(size_t at=1;at<=path.size();) {
        size_t end=path.find('/',at);
        if(end==std::string::npos) end=path.size();
        parts.push_back(path.substr(at,end-at));
        at=end+1;
    }
    if(path.empty() || path[0]!='/' || parts.size()!=6) return false;
    bool ok=false;
    HexName8(parts[3],ok);
    // The header's name: hex digits (the Media ID, or longer from other tools).
    bool name=parts[5].size()>=8 && parts[5].size()<=42;
    for(size_t i=0;i<parts[5].size() && name;++i) name=isxdigit((unsigned char)parts[5][i])!=0;
    return ok && name && !_stricmp(parts[1].c_str(),"Content") && parts[2]=="0000000000000000" && parts[4]=="00007000";
}

void RemoveGame(std::shared_ptr<ContentCopy> job, InstalledGame game) {
    ContentCopy& c=*job;
    Options o=Current();
    const Console* console=ConsoleIfKnown(c.address);
    FtpClient ftp(c.address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),30);
    if(!OpenFor(c,ftp,console)) return;
    std::string error;
    bool ok;
    if(game.god) {
        { std::lock_guard<std::mutex> hold(CopyLock); c.folder=game.path; }
        // The header first: without it Aurora no longer lists a package part gone.
        ok=ftp.Delete(game.path.c_str()) || ftp.Missing();
        if(!ok) error="Couldn't delete "+game.path+": "+ftp.Error();
        std::string data=game.path+".data";
        if(ok && ftp.Enters(data.c_str())) ok=DeleteTree(ftp,data,0,error);
        // Its 00007000 folder too, once nothing else is in it.
        std::string parent=game.path.substr(0,game.path.find_last_of('/'));
        std::vector<FtpEntry> left;
        if(ok && ftp.List(parent.c_str(),left) && left.empty()) ftp.RemoveFolder(parent.c_str());
    } else {
        std::string folder=game.path.substr(0,game.path.find_last_of('/'));
        { std::lock_guard<std::mutex> hold(CopyLock); c.folder=folder; }
        ok=DeleteTree(ftp,folder,0,error);
    }
    ftp.Close();
    if(!ok) { EndCopy(c,console,"failed",error); return; }
    if(Console* known=ConsoleIfKnown(c.address)) {
        std::lock_guard<std::mutex> hold(ConsoleLock);
        std::vector<InstalledGame>& list=known->installed;
        for(size_t i=0;i<list.size();++i) if(list[i].path==game.path) { list.erase(list.begin()+long(i)); break; }
    }
    EndCopy(c,console,"done","");
}

// POST /api/console/remove {game, address}: the game's installed copy on that console.
bool StartRemove(const web::Value& body, std::string& error) {
    std::string name, address;
    Text(body,"game",name); Text(body,"address",address);
    IsoFile g;
    bool found=false;
    {
        std::lock_guard<std::mutex> hold(GamesListLock);
        for(size_t i=0;i<GamesList.size() && !found;++i) if(config::ToUtf8(WithoutIso(GamesList[i].name))==name) { g=GamesList[i]; found=true; }
    }
    if(!found || !g.titleId) { error="There's no game called "+name+" here."; return false; }
    DWORD at=INADDR_NONE;
    if(!CopyTarget(address,at,error)) return false;
    Console* console=ConsoleIfKnown(at);
    if(!console) { error="That console hasn't used NetDisc360 since it started."; return false; }
    InstalledGame copy;
    bool has=false;
    {
        std::lock_guard<std::mutex> hold(ConsoleLock);
        for(size_t i=0;i<console->installed.size() && !has;++i)
            if(console->installed[i].titleId==g.titleId && console->installed[i].disc==g.disc) { copy=console->installed[i]; has=true; }
    }
    if(!has) { error=name+" isn't installed on "+address+" (as far as its last look found)."; return false; }
    // Only what can be nothing but the game's own copy.
    Options o=Current();
    if(copy.god ? !GodHeaderPath(copy.path) : false) { error="Its copy isn't where a Games on Demand package goes ("+copy.path+"); remove it by hand."; return false; }
    if(!copy.god) {
        std::string folder=copy.path.substr(0,copy.path.find_last_of('/'));
        std::vector<std::string> roots=InstalledFolders(o.installedFolders);
        bool under=false;
        for(size_t i=0;i<roots.size();++i)
            if(folder.size()>roots[i].size()+1 && !_strnicmp(folder.c_str(),roots[i].c_str(),roots[i].size()) && folder[roots[i].size()]=='/') under=true;
        std::string lower=ToLowerAscii(folder);
        if(!under || lower.find(ToLowerAscii(o.aurora))==0 || lower.find(ToLowerAscii(o.xboxFolder))==0) {
            error="Its folder ("+folder+") isn't one this removes; remove it by hand.";
            return false;
        }
    }
    std::shared_ptr<ContentCopy> job(new ContentCopy);
    job->kind="remove"; job->game=name; job->disc=name; job->label="Remove "+name;
    job->run=[copy](std::shared_ptr<ContentCopy> j) { RemoveGame(j,copy); };
    if(!AddCopy(job,at,error)) return false;
    Log("Xbox %s: removing %s from its hard drive (%s).",job->console.c_str(),name.c_str(),copy.path.c_str());
    return true;
}

// Title updates ----------------------------------------------------------------
//
// The dashboard looks up a game's title updates on XboxUnity (see
// titleupdates.h), keeps those it downloads in TitleUpdates\<Title ID>\<its
// id>\ next to the settings, and puts one on a console through the console's
// queue, where Aurora would put it: Content\0000000000000000\<Title
// ID>\000B0000 or Hdd1:\Cache. What a console has is read from those folders
// (a check, also queued); an update is known as XboxUnity's by its size, or
// by the name it was downloaded under. Older updates of the same disc are
// moved out of the way (to <NetISO folder>\NetDisc360\Title updates), never
// deleted, so the console uses the one put there. Like Aurora's title update
// manager, a console's updates can be switched: one moved aside, or one in
// Aurora's own store (Data\TitleUpdates, only read), is made the one it
// uses, or the game's updates are all moved aside. Only the PC goes online.
struct TuTitle {
    std::vector<tu::Offer> offers;
    double fetchedAt;  // Now(); 0: never.
    std::string error;
    TuTitle() : fetchedAt(0) {}
};
// An update file on a console.
struct TuFile {
    std::string path, name;  // FTP path, and its name.
    U64 size;
    DWORD titleId, mediaId;  // From its header (0 if it has none that says).
    std::string displayName, contentId;  // contentId: see tu::PackageHeader.
    std::string where;  // "live": where the console loads it; "aside": moved out of the way by this server; "aurora": in Aurora's own store.
    DWORD version;      // From its header.
    TuFile() : size(0), titleId(0), mediaId(0), where("live"), version(0) {}
};
struct ConsoleTus {
    long long checkedAt;  // time(); 0: never.
    std::vector<TuFile> files;
    std::string error;
    ConsoleTus() : checkedAt(0) {}
};
std::mutex TuLock;
std::map<DWORD,TuTitle> TuInfo;          // By Title ID.
std::map<DWORD,ConsoleTus> TuConsoles;   // By console address.
struct TuScanState { bool running; int done, total, found; long long startedAt, endedAt; TuScanState() : running(false), done(0), total(0), found(0), startedAt(0), endedAt(0) {} } TuScan;

const char TuBackupFolder[] = "Title updates";
const double TuFresh = 6*3600;  // How long XboxUnity's list for a game is used before it's asked again.

std::string TuCacheFolder(const Options& o) {
    return o.configPath.empty() ? std::string() : config::FolderOf(o.configPath)+"TitleUpdates";
}

// The update id of titleId this PC has a copy of: its path and name; false if none.
bool TuCached(const Options& o, DWORD titleId, const std::string& id, std::string& path, std::string& name) {
    std::string root=TuCacheFolder(o);
    if(root.empty()) return false;
    std::string folder=root+"/"+Hex8(titleId)+"/"+tu::FatxName(id);
    std::vector<config::Entry> files=config::Files(folder);
    for(size_t i=0;i<files.size();++i) {
        if(files[i].name.empty() || files[i].name[0]=='.') continue;
        path=folder+"/"+files[i].name; name=files[i].name;
        return true;
    }
    return false;
}

// Which of XboxUnity's updates f is: by its Content ID, or for a Cache
// update (no header), the one downloaded here under that name; -1 if none.
int TuOfferOf(const Options& o, DWORD titleId, const TuFile& f, const std::vector<tu::Offer>& offers) {
    for(size_t u=0;u<offers.size();++u) if(!f.contentId.empty() && f.contentId==offers[u].hash) return int(u);
    if(!f.contentId.empty()) return -1;
    for(size_t u=0;u<offers.size();++u) {
        std::string path, cached;
        if(TuCached(o,titleId,offers[u].id,path,cached) && !_stricmp(tu::FatxName(cached).c_str(),f.name.c_str())) return int(u);
    }
    return -1;
}

// The update's number as XboxUnity counts it (a header's can be one lower), else its header's.
int TuNumber(const TuFile& f, int offer, const std::vector<tu::Offer>& offers) {
    return offer>=0 && offers[size_t(offer)].version>0 ? offers[size_t(offer)].version : int(f.version);
}

// XboxUnity's updates for titleId, asked again when older than TuFresh (or refresh).
bool LookupTus(DWORD titleId, bool refresh, std::vector<tu::Offer>& offers, std::string& error) {
    {
        std::lock_guard<std::mutex> hold(TuLock);
        std::map<DWORD,TuTitle>::const_iterator t=TuInfo.find(titleId);
        if(t!=TuInfo.end() && t->second.fetchedAt && Now()-t->second.fetchedAt<TuFresh && !refresh) {
            offers=t->second.offers; error=t->second.error;
            return error.empty();
        }
    }
    Options o=Current();
    std::string body, name;
    bool ok=tu::Get(tu::InfoUrl(o.xboxUnity,titleId),body,4<<20,name,error) && tu::ParseInfo(body,offers,error);
    std::lock_guard<std::mutex> hold(TuLock);
    TuTitle& t=TuInfo[titleId];
    t.fetchedAt=Now();
    if(ok) { t.offers=offers; t.error.clear(); }
    else t.error=error;
    return ok;
}

// The update's file on this PC, downloading it from XboxUnity first if it
// isn't here; checked against XboxUnity's size and checksum when it gives them.
bool FetchTu(const Options& o, DWORD titleId, const tu::Offer& offer, std::string& path, std::string& name, std::string& error,
             const std::function<void(U64)>& got) {
    if(TuCached(o,titleId,offer.id,path,name)) return true;
    std::string data;
    if(!tu::Get(tu::FileUrl(o.xboxUnity,offer.id),data,size_t(1)<<30,name,error)) return false;
    if(got) got(data.size());
    if(offer.size && !tu::SameSize(data.size(),offer.size)) { error="XboxUnity's file isn't the size it lists ("+std::to_string(data.size())+" bytes, not "+std::to_string(offer.size)+"); try again later."; return false; }
    tu::PackageHeader header;
    bool package=tu::ReadPackageHeader(reinterpret_cast<const unsigned char*>(data.data()),data.size(),header);
    // XboxUnity's hash is a package's Content ID (the SHA-1 in its header);
    // for a file that isn't a package, the SHA-1 of the file.
    if(offer.hash.size()==40 && !(package && header.contentId==offer.hash) && tu::Sha1Hex(data.data(),data.size())!=offer.hash) {
        error="XboxUnity's file doesn't match the checksum it lists; try again later."; return false;
    }
    if(!offer.size && offer.hash.size()!=40 && !package) { error="XboxUnity sent something that isn't a title update."; return false; }
    if(package && header.titleId && header.titleId!=titleId) { error="XboxUnity's file is for another game ("+Hex8(header.titleId)+")."; return false; }
    name=tu::FatxName(name);
    if(name.empty()) name=tu::FatxName((package ? "tu" : "TU_")+offer.id);
    std::string root=TuCacheFolder(o);
    if(root.empty()) { error="This server keeps no settings folder (--config), so it has nowhere to keep downloads."; return false; }
    std::string folder=root+"/"+Hex8(titleId)+"/"+tu::FatxName(offer.id);
    config::MakeFolder(root); config::MakeFolder(root+"/"+Hex8(titleId)); config::MakeFolder(folder);
    path=folder+"/"+name;
    if(!config::WriteFile(path,data,false)) { error="Couldn't keep the download in "+folder+"."; return false; }
    Log("Downloaded title update %d (%s) for %s from XboxUnity to %s.",offer.version,SizeText(data.size()).c_str(),Hex8(titleId).c_str(),path.c_str());
    return true;
}

// A file's header, into f (Cache updates have none).
void ReadTuHeader(FtpClient& ftp, TuFile& f) {
    std::vector<unsigned char> head;
    tu::PackageHeader h;
    if(ftp.RetrieveHead(f.path.c_str(),head,tu::PackageHeaderBytes) && tu::ReadPackageHeader(head.data(),head.size(),h)) {
        if(h.titleId) f.titleId=h.titleId;
        f.mediaId=h.mediaId; f.displayName=h.displayName; f.contentId=h.contentId; f.version=h.version;
    }
}

// The files in folder (none if it isn't there), and in its folders one level down.
void TuFilesIn(FtpClient& ftp, const std::string& folder, DWORD titleId, const char* where, std::vector<TuFile>& out, int depth) {
    if(!ftp.Enters(folder.c_str())) return;  // Aurora's FTP lists its top for a folder that isn't there.
    std::vector<FtpEntry> there;
    if(!ftp.List(folder.c_str(),there)) return;
    for(size_t i=0;i<there.size() && out.size()<1000;++i) {
        std::string path=folder+"/"+there[i].name;
        if(there[i].folder) { if(depth>0) TuFilesIn(ftp,path,titleId,where,out,depth-1); continue; }
        TuFile f; f.path=path; f.name=there[i].name; f.size=there[i].size; f.titleId=titleId; f.where=where;
        ReadTuHeader(ftp,f);
        out.push_back(f);
    }
}

std::string TuAsideFolder(const Options& o, DWORD titleId) {
    return XboxAppFolder(o)+"/"+TuBackupFolder+"/"+Hex8(titleId);
}

// Reads the update files of the titles on a console: each title's 000B0000
// and the Cache's TU_ files, with each one's header (Cache updates have
// none). With o, also those switched off: moved aside by this server, and in
// Aurora's store (Data\TitleUpdates\<console>\<Title ID>\<Content ID>\).
bool ReadConsoleTus(FtpClient& ftp, const std::set<DWORD>& titles, std::vector<TuFile>& out, std::string& error, const Options* o=NULL) {
    out.clear();
    for(std::set<DWORD>::const_iterator t=titles.begin();t!=titles.end();++t) {
        std::string folder=tu::ContentFolder(*t);
        if(!ftp.Enters(folder.c_str())) {
            if(ftp.Code()/100==5) continue;
            error=ftp.Error(); return false;
        }
        std::vector<FtpEntry> there;
        if(!ftp.List(folder.c_str(),there)) { error=ftp.Error(); return false; }
        for(size_t i=0;i<there.size();++i) {
            if(there[i].folder) continue;
            TuFile f; f.path=folder+"/"+there[i].name; f.name=there[i].name; f.size=there[i].size; f.titleId=*t;
            ReadTuHeader(ftp,f);
            out.push_back(f);
        }
    }
    if(o) {
        for(std::set<DWORD>::const_iterator t=titles.begin();t!=titles.end();++t) TuFilesIn(ftp,TuAsideFolder(*o,*t),*t,"aside",out,1);
        std::string store=o->aurora+"/Data/TitleUpdates";
        std::vector<FtpEntry> devices;
        if(ftp.Enters(store.c_str()) && ftp.List(store.c_str(),devices))
            for(size_t d=0;d<devices.size();++d) {
                if(!devices[d].folder) continue;
                std::string device=store+"/"+devices[d].name;
                std::vector<FtpEntry> games;
                if(!ftp.Enters(device.c_str()) || !ftp.List(device.c_str(),games)) continue;
                for(size_t g=0;g<games.size();++g) {
                    bool ok=false;
                    DWORD titleId=games[g].folder ? DWORD(strtoul(games[g].name.c_str(),NULL,16)) : 0;
                    ok=titleId && titles.count(titleId);
                    if(ok) TuFilesIn(ftp,device+"/"+games[g].name,titleId,"aurora",out,1);
                }
            }
    }
    std::vector<FtpEntry> cache;
    if(ftp.Enters(tu::CacheFolder) && ftp.List(tu::CacheFolder,cache))
        for(size_t i=0;i<cache.size();++i) {
            if(cache[i].folder || cache[i].name.compare(0,3,"TU_")) continue;
            TuFile f; f.path=std::string(tu::CacheFolder)+"/"+cache[i].name; f.name=cache[i].name; f.size=cache[i].size;
            out.push_back(f);
        }
    return true;
}

// The Title IDs of the games here, for a check of a console's updates.
std::set<DWORD> LibraryTitles() {
    std::set<DWORD> titles;
    std::lock_guard<std::mutex> hold(GamesListLock);
    for(size_t i=0;i<GamesList.size();++i) if(GamesList[i].titleId && GamesList[i].mediaId) titles.insert(GamesList[i].titleId);
    return titles;
}

void CheckTus(std::shared_ptr<ContentCopy> job) {
    ContentCopy& c=*job;
    Options o=Current();
    const Console* console=ConsoleIfKnown(c.address);
    std::set<DWORD> titles=LibraryTitles();
    { std::lock_guard<std::mutex> hold(CopyLock); c.files=int(titles.size()); }
    FtpClient ftp(c.address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),30);
    std::vector<TuFile> files;
    std::string error;
    bool ok=OpenFor(c,ftp,console) && ReadConsoleTus(ftp,titles,files,error,&o);
    ftp.Close();
    {
        std::lock_guard<std::mutex> hold(TuLock);
        ConsoleTus& t=TuConsoles[c.address];
        if(ok) { t.files=files; t.checkedAt=(long long)time(NULL); t.error.clear(); }
        else if(c.state=="copying") t.error=error;
    }
    if(!ok) { if(c.state=="copying") EndCopy(c,console,"failed",error); return; }
    Log("Xbox %s: it has %d title update file(s) for the games here.",c.console.c_str(),int(files.size()));
    std::lock_guard<std::mutex> hold(CopyLock);
    c.state="done"; c.endedAt=(long long)time(NULL); c.done=c.total; c.file=c.files;
}

bool QueueTuCheck(DWORD at, std::string& error) {
    std::shared_ptr<ContentCopy> job(new ContentCopy);
    job->kind="check"; job->game="Title updates"; job->label="Check its title updates";
    job->run=CheckTus;
    return AddCopy(job,at,error);
}

// The names (in lower case) of the updates of titleId downloaded to this PC.
std::set<std::string> TuCachedNames(const Options& o, DWORD titleId) {
    std::set<std::string> names;
    std::string root=TuCacheFolder(o);
    if(root.empty()) return names;
    std::string folder=root+"/"+Hex8(titleId);
    std::vector<config::Entry> ids=config::Folders(folder);  // One per XboxUnity update id.
    for(size_t i=0;i<ids.size();++i) {
        std::vector<config::Entry> files=config::Files(folder+"/"+ids[i].name);
        for(size_t k=0;k<files.size();++k) if(!files[k].name.empty() && files[k].name[0]!='.') names.insert(Lower(tu::FatxName(files[k].name)));
    }
    return names;
}

// Moves the updates of the disc the console loads (but keep) aside, each to
// <NetISO folder>\NetDisc360\Title updates\<Title ID>\<its Content ID>\, so
// two of the same name don't meet. A Cache update counts when it was
// downloaded here for the game (it has no header that says).
void MoveTusAside(FtpClient& ftp, const Options& o, ContentCopy& c, DWORD titleId, DWORD mediaId, const std::string& keep, std::vector<std::string>& moved) {
    std::vector<TuFile> files;
    std::set<DWORD> one; one.insert(titleId);
    std::string error;
    if(!ReadConsoleTus(ftp,one,files,error)) { Log("Xbox %s: couldn't read its title updates (%s).",c.console.c_str(),error.c_str()); return; }
    std::set<std::string> cached=TuCachedNames(o,titleId);
    std::string backup=TuAsideFolder(o,titleId);
    for(size_t i=0;i<files.size();++i) {
        const TuFile& f=files[i];
        if(!_stricmp(f.path.c_str(),keep.c_str())) continue;
        bool cache=!f.path.compare(0,strlen(tu::CacheFolder),tu::CacheFolder);
        if(cache ? !cached.count(Lower(f.name)) : (f.titleId!=titleId || (f.mediaId && f.mediaId!=mediaId))) continue;
        // A Cache update has no Content ID: its folder is named by its name and size, short enough for FATX.
        std::string key=Lower(f.name)+"-"+std::to_string(f.size);
        std::string to=backup+"/"+(f.contentId.empty() ? "cache-"+tu::Sha1Hex(key.data(),key.size()).substr(0,16)+"-"+std::to_string(f.size) : f.contentId)+"/"+f.name;
        std::string why;
        if(RemoteSize(ftp,to)>=0) to=backup+"/"+tu::FatxName(f.contentId.substr(0,8)+"-"+std::to_string((long long)time(NULL)))+"/"+f.name;  // That one's there already.
        if(!MakeFolders(ftp,to,FtpDrive(to).size(),why) || !ftp.Rename(f.path.c_str(),to.c_str())) {
            Log("Xbox %s: couldn't move the update %s out of the way (%s); it stays.",c.console.c_str(),f.path.c_str(),why.empty() ? ftp.Error() : why.c_str());
            continue;
        }
        Log("Xbox %s: moved the update %s to %s.",c.console.c_str(),f.path.c_str(),to.c_str());
        moved.push_back(f.name);
    }
}

// What the console has of titleId's updates now, into TuConsoles.
void RefreshTus(FtpClient& ftp, const Options& o, DWORD address, DWORD titleId) {
    std::vector<TuFile> files;
    std::set<DWORD> one; one.insert(titleId);
    std::string error;
    if(!ReadConsoleTus(ftp,one,files,error,&o)) return;
    std::lock_guard<std::mutex> hold(TuLock);
    ConsoleTus& t=TuConsoles[address];
    std::vector<TuFile> kept;
    for(size_t i=0;i<t.files.size();++i) if(t.files[i].titleId!=titleId && t.files[i].path.compare(0,strlen(tu::CacheFolder),tu::CacheFolder)) kept.push_back(t.files[i]);
    for(size_t i=0;i<files.size();++i) if(files[i].titleId==titleId || !files[i].path.compare(0,strlen(tu::CacheFolder),tu::CacheFolder)) kept.push_back(files[i]);
    t.files.swap(kept);
    if(!t.checkedAt) t.checkedAt=(long long)time(NULL);
}

// Aurora's own list of the updates it has on, kept like the dashboard's
// choices (see TitleUpdateScript): what each console's script still has to
// do, by (Title ID << 32 | Media ID), and the number in its done file's name.
// Aurora reads that list once as it starts, before its scripts run, so a
// change the script makes is used from the start after: restartAfter, when
// the console still needs that second start (done: the script's done file).
struct TuAuroraState {
    std::map<unsigned long long,AuroraTu> wants; long long seq; bool restartAfter; std::string done;
    TuAuroraState() : seq(0), restartAfter(false) {}
};
std::map<DWORD,TuAuroraState> TuAurora;
const char TuAuroraDone[] = "tu-aurora-";

// Aurora's store folder for this console's updates: Data\TitleUpdates\<its
// drive's ID>, the one (if there are several) that has titleId's; "" if none.
std::string TuAuroraDevice(FtpClient& ftp, const Options& o, DWORD titleId) {
    std::string store=o.aurora+"/Data/TitleUpdates";
    std::vector<FtpEntry> devices;
    if(!ftp.Enters(store.c_str()) || !ftp.List(store.c_str(),devices)) return std::string();
    std::string first;
    for(size_t d=0;d<devices.size();++d) {
        const std::string& name=devices[d].name;
        if(!devices[d].folder || name.size()!=40 || name.find_first_not_of("0123456789ABCDEFabcdef")!=std::string::npos) continue;
        if(first.empty()) first=name;
        if(ftp.Enters((store+"/"+name+"/"+Hex8(titleId)).c_str())) return name;
    }
    return first;
}

// After Aurora was restarted: true once it's back, its web server answering
// (it starts after Aurora's plugins). Nothing is sent for the first 25 s,
// while Aurora builds its list and loads its plugins.
bool WaitForAurora(DWORD address, const Options& o) {
    Sleep(25000);
    for(int tries=0;tries<20;++tries) {
        AuroraWeb aurora(address,o.auroraWebPort,o.auroraWebUser,o.auroraWebPassword,5);
        std::string path;
        if(aurora.Title(path)) return true;
        Sleep(5000);
    }
    return false;
}

// Makes Aurora have on (in its own list) the update now live at live, or
// none (on NULL), so it keeps that when it starts the game: a script Aurora
// runs as it starts (see TitleUpdateScript), and Aurora restarted when it's
// on screen and the settings allow. An update Aurora hasn't got gets a copy
// in its store. ftp is open, and closed here. What happened, for the job's note.
std::string TuInAurora(FtpClient& ftp, const Options& o, ContentCopy& c, const Console* console, DWORD titleId, DWORD mediaId,
                       const TuFile* on, const std::string& live, const std::vector<unsigned char>* bytes) {
    std::string game=c.game;
    AuroraTu want;
    want.titleId=titleId; want.mediaId=mediaId;
    bool cache=on && on->contentId.empty();
    if(on && !cache) {
        for(size_t i=0;i<on->contentId.size();++i) want.hash+=char(toupper((unsigned char)on->contentId[i]));
        want.name=on->displayName.empty() ? on->name : on->displayName; want.file=on->name;
        want.base=on->version;
        std::vector<tu::Offer> offers;
        { std::lock_guard<std::mutex> hold(TuLock); std::map<DWORD,TuTitle>::const_iterator t=TuInfo.find(titleId); if(t!=TuInfo.end()) offers=t->second.offers; }
        int number=TuNumber(*on,TuOfferOf(o,titleId,*on,offers),offers);
        want.version=number>int(on->version) ? unsigned(number) : on->version+1;
        char size[32]; _snprintf_s(size,sizeof(size),_TRUNCATE,"%.1f MB",on->size/1048576.0);
        want.size=size;
        want.device=TuAuroraDevice(ftp,o,titleId);
        if(!want.device.empty()) {
            std::string folder=o.aurora+"/Data/TitleUpdates/"+want.device+"/"+Hex8(titleId)+"/"+want.hash, store=folder+"/"+on->name, why;
            bool there=RemoteSize(ftp,store)==(long long)on->size;
            if(!there) {
                // Aurora copies its store's file in place when it starts the game: one there, then.
                std::vector<unsigned char> data;
                if(bytes) data=*bytes;
                else if(!ftp.Retrieve(live.c_str(),data,size_t(1)<<30)) data.clear();
                there=data.size()==on->size && MakeFolders(ftp,store,o.aurora.size(),why) && ftp.Store(store.c_str(),data.data(),data.size());
                if(there) Log("Xbox %s: put a copy of %s in Aurora's store, at %s.",c.console.c_str(),live.c_str(),store.c_str());
                else Log("Xbox %s: couldn't put a copy of %s in Aurora's store (%s).",c.console.c_str(),live.c_str(),why.empty() ? ftp.Error() : why.c_str());
            }
            if(there) {
                want.backupPath="Game:\\Data\\TitleUpdates\\"+want.device+"\\"+Hex8(titleId)+"\\"+want.hash+"\\"+on->name;
                want.livePath="\\Content\\0000000000000000\\"+Hex8(titleId)+"\\000B0000\\";
            }
        }
    }
    std::string helper=XboxAppFolder(o);
    EnsureFolder(ftp,helper);
    std::vector<FtpEntry> files;
    ftp.List(helper.c_str(),files);
    std::string done;
    std::vector<AuroraTu> wants;
    {
        std::lock_guard<std::mutex> hold(TuLock);
        TuAuroraState& a=TuAurora[c.address];
        // The last script ran: what it had is done.
        std::string last=TuAuroraDone+std::to_string(a.seq)+".txt";
        for(size_t i=0;i<files.size();++i) if(a.seq && !_stricmp(files[i].name.c_str(),last.c_str())) a.wants.clear();
        a.wants[((unsigned long long)titleId<<32)|mediaId]=want;
        a.seq=std::max(a.seq+1,(long long)time(NULL));
        done=helper+"/"+TuAuroraDone+std::to_string(a.seq)+".txt";
        a.done=done; a.restartAfter=true;  // Until the job restarts Aurora after the script ran.
        for(std::map<unsigned long long,AuroraTu>::const_iterator w=a.wants.begin();w!=a.wants.end();++w) wants.push_back(w->second);
    }
    for(size_t i=0;i<files.size();++i)
        if(!files[i].folder && !_strnicmp(files[i].name.c_str(),TuAuroraDone,strlen(TuAuroraDone))) ftp.Delete((helper+"/"+files[i].name).c_str());
    std::string text=TitleUpdateScript(wants,done), path=o.aurora+TitleUpdateScriptPath;
    const std::string script=TitleUpdateScriptPath;
    for(size_t at=1;(at=script.find('/',at))!=std::string::npos;++at) EnsureFolder(ftp,o.aurora+script.substr(0,at));
    if(RefreshRemote(ftp,path,std::vector<unsigned char>(text.begin(),text.end()))<0) {
        std::string why=ftp.Error();
        ftp.Close();
        Log("Xbox %s: couldn't write %s: %s",c.console.c_str(),path.c_str(),why.c_str());
        return "Aurora's own list wasn't changed (its script couldn't be written), so Aurora may switch it back when it starts "+game+".";
    }
    std::string what=want.hash.empty() ? "no update of "+game : c.label+" of "+game;
    Log("Xbox %s: wrote %s, so Aurora has %s on as it starts.",c.console.c_str(),path.c_str(),what.c_str());
    ftp.Close();
    // Aurora uses the change from its second start from now: the first runs
    // the script, after Aurora read its list.
    std::string twice="Restart Aurora twice before you start "+game+": the first time Aurora takes this, and it uses it from the second. Otherwise Aurora switches it back when it starts the game.";
    if(!o.restartAurora || !console) return twice;
    std::string later="When Aurora is on screen again, NetDisc360 restarts it twice so it takes this; don't start "+game+" before that, or Aurora switches it back.";
    if(!RestartAurora(*console,o,"so it takes the title update")) return later;
    // Nothing goes to the console while Aurora starts (COD hung once loading
    // its plugins while this read its FTP): only once it's up, the script's
    // done file, which says what it did.
    if(!WaitForAurora(c.address,o)) return "Aurora didn't come back after its restart; check the console, then Aurora's title updates for "+game+".";
    std::string result;
    for(int tries=0;tries<5 && result.empty() && !c.stop;++tries) {
        if(tries) Sleep(3000);
        FtpClient check(c.address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),10);
        if(!check.Open()) continue;
        std::vector<unsigned char> answer;
        if(RemoteSize(check,done)>=0 && check.Retrieve(done.c_str(),answer,4096)) result.assign(answer.begin(),answer.end());
        check.Close();
    }
    if(result.empty()) return "Aurora restarted, but its script didn't say it switched it; check Aurora's title updates for "+game+" before you start it.";
    bool again=false;
    for(int tries=0;tries<3 && !again;++tries) {
        if(tries) Sleep(5000);
        again=RestartAurora(*console,o,"so it uses its new title update list");
    }
    if(again) WaitForAurora(c.address,o);  // The next job's FTP waits for it too.
    {
        std::lock_guard<std::mutex> hold(TuLock);
        TuAuroraState& a=TuAurora[c.address];
        a.wants.clear();
        if(a.done==done) a.restartAfter=!again;
    }
    std::string pending=again ? std::string() : " Restart Aurora once before you start "+game+", so Aurora uses it.";
    char ids[32]; _snprintf_s(ids,sizeof(ids),_TRUNCATE,"%08X %08X ",titleId,mediaId);
    size_t at=result.find(ids);
    std::string said=at==std::string::npos ? std::string() : result.substr(at+strlen(ids),result.find_first_of("\r\n",at)-at-strlen(ids));
    if(said=="on") return "Aurora has it on too, so it stays when Aurora starts "+game+"."+pending;
    if(said=="off") return (cache ? "Aurora has none of its own on for "+game+" (it doesn't list this kind of update)." : std::string("Aurora has none on either."))+pending;
    if(said=="missing") return "Aurora didn't take it on its list; switch it on in Aurora's title updates for "+game+", or Aurora removes it when it starts the game.";
    return "Aurora couldn't change its list ("+(said.empty() ? std::string("no answer for this game") : said)+"); check Aurora's title updates for "+game+".";
}

// After a push: a console whose Aurora ran the title update script (its
// done file is there) but hasn't started since gets that second start, so it
// uses the new list (see TuAuroraState), when it's on screen.
bool TuAuroraFollowUp(const Console& console, FtpClient& ftp, const Options& o) {
    std::string done;
    {
        std::lock_guard<std::mutex> hold(TuLock);
        std::map<DWORD,TuAuroraState>::const_iterator a=TuAurora.find(console.address);
        if(a==TuAurora.end() || !a->second.restartAfter || a->second.done.empty()) return false;
        done=a->second.done;
    }
    if(!o.restartAurora || RemoteSize(ftp,done)<0) return false;  // Aurora hasn't run it yet.
    if(!RestartAurora(console,o,"so it uses its new title update list")) return false;
    std::lock_guard<std::mutex> hold(TuLock);
    TuAuroraState& a=TuAurora[console.address];
    if(a.done==done) { a.restartAfter=false; a.wants.clear(); }
    return true;
}

// The update on the console, made the one it uses: older updates of the
// same disc (by their headers) are moved to the backup folder first.
void PutTu(std::shared_ptr<ContentCopy> job, DWORD titleId, DWORD mediaId, tu::Offer offer) {
    ContentCopy& c=*job;
    Options o=Current();
    const Console* console=ConsoleIfKnown(c.address);
    std::string path, name, error;
    { std::lock_guard<std::mutex> hold(CopyLock); c.phase="download"; c.total=offer.size; }
    if(!FetchTu(o,titleId,offer,path,name,error,[&](U64 n) { std::lock_guard<std::mutex> hold(CopyLock); c.total=n; c.done=n; })) {
        EndCopy(c,console,"failed","Couldn't get "+offer.name+" from XboxUnity: "+error); return;
    }
    if(c.stop) { EndCopy(c,console,"stopped",""); return; }
    std::string data;
    if(!config::ReadFile(path,data,size_t(1)<<30)) { EndCopy(c,console,"failed","Couldn't read "+path+"."); return; }
    std::string remote=tu::ConsolePath(titleId,name);
    {
        std::lock_guard<std::mutex> hold(CopyLock);
        c.phase="send"; c.total=data.size(); c.done=0; c.copied=0; c.files=1; c.file=1; c.current=name; c.folder=remote.substr(0,remote.find_last_of('/')); c.began=Now();
    }
    FtpClient ftp(c.address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),30);
    if(!OpenFor(c,ftp,console)) return;
    if(!MakeFolders(ftp,remote,strlen("/Hdd1")-1,error)) { EndCopy(c,console,"failed",error); return; }
    // Older updates of this disc, out of the way, unless asked to keep them.
    std::vector<std::string> moved;
    if(!c.key.empty()) MoveTusAside(ftp,o,c,titleId,mediaId,remote,moved);
    if(RemoteSize(ftp,remote)==(long long)data.size()) {
        Log("Xbox %s: %s is there already.",c.console.c_str(),remote.c_str());
        std::lock_guard<std::mutex> hold(CopyLock);
        c.done=c.total; ++c.skipped;
    } else {
        bool ok=ftp.StoreFrom(remote.c_str(),data.size(),
            [&](U64 offset, unsigned char* bytes, DWORD length) { memcpy(bytes,data.data()+size_t(offset),length); return true; },
            [&](U64 sent) {
                std::lock_guard<std::mutex> hold(CopyLock);
                c.copied=sent; c.done=sent;
                return !c.stop.load();
            });
        if(!ok) {
            std::string why=ftp.Error();
            if(!ftp.Delete(remote.c_str()) && !ftp.Missing()) Log("Xbox %s: couldn't delete the part of %s that was sent: %s",c.console.c_str(),remote.c_str(),ftp.Error());
            if(c.stop) EndCopy(c,console,"stopped","");
            else EndCopy(c,console,"failed","Sending "+remote+" stopped: "+why);
            return;
        }
        Log("Xbox %s: put %s for %s on it, at %s.",c.console.c_str(),c.label.c_str(),c.game.c_str(),remote.c_str());
    }
    RefreshTus(ftp,o,c.address,titleId);
    TuFile put; put.path=remote; put.name=name; put.size=data.size(); put.titleId=titleId;
    tu::PackageHeader header;
    if(tu::ReadPackageHeader(reinterpret_cast<const unsigned char*>(data.data()),data.size(),header)) {
        put.mediaId=header.mediaId; put.displayName=header.displayName; put.contentId=header.contentId; put.version=header.version;
    }
    std::vector<unsigned char> bytes(data.begin(),data.end());
    std::string aurora=TuInAurora(ftp,o,c,console,titleId,mediaId,&put,remote,&bytes);
    {
        std::lock_guard<std::mutex> hold(CopyLock);
        c.note=moved.empty() ? aurora : "Moved the older "+std::string(moved.size()==1 ? "update" : "updates")+" to "+XboxAppFolder(o)+"/"+TuBackupFolder+". "+aurora;
    }
    EndCopy(c,console,"done","");
}

// Puts an update the console has switched off back where it loads it: one
// moved aside is moved back, one in Aurora's store is copied (Aurora's stays).
// The game's other updates are moved aside first.
void UseTu(std::shared_ptr<ContentCopy> job, DWORD titleId, DWORD mediaId, TuFile from) {
    ContentCopy& c=*job;
    Options o=Current();
    const Console* console=ConsoleIfKnown(c.address);
    std::string remote=tu::ConsolePath(titleId,from.name), error;
    {
        std::lock_guard<std::mutex> hold(CopyLock);
        c.phase="send"; c.total=from.size; c.done=0; c.files=1; c.file=1; c.current=from.name; c.folder=remote.substr(0,remote.find_last_of('/')); c.began=Now();
    }
    FtpClient ftp(c.address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),30);
    if(!OpenFor(c,ftp,console)) return;
    if(!MakeFolders(ftp,remote,strlen("/Hdd1")-1,error)) { EndCopy(c,console,"failed",error); return; }
    std::vector<unsigned char> data;
    if(from.where=="aurora" && !ftp.Retrieve(from.path.c_str(),data,size_t(1)<<30)) { EndCopy(c,console,"failed","Couldn't read "+from.path+": "+ftp.Error()); return; }
    if(from.where=="aurora" && data.size()!=from.size) { EndCopy(c,console,"failed","Got "+std::to_string(data.size())+" bytes of "+from.path+", not "+std::to_string(from.size)+"."); return; }
    std::vector<std::string> moved;
    MoveTusAside(ftp,o,c,titleId,mediaId,from.where=="aside" ? from.path : remote,moved);
    if(RemoteSize(ftp,remote)==(long long)from.size) {
        Log("Xbox %s: %s is there already.",c.console.c_str(),remote.c_str());
        std::lock_guard<std::mutex> hold(CopyLock);
        ++c.skipped;
    } else if(from.where=="aside") {
        if(!ftp.Rename(from.path.c_str(),remote.c_str())) { EndCopy(c,console,"failed","Couldn't move "+from.path+" back: "+ftp.Error()); return; }
        Log("Xbox %s: moved %s back to %s.",c.console.c_str(),from.path.c_str(),remote.c_str());
    } else {
        bool ok=ftp.StoreFrom(remote.c_str(),data.size(),
            [&](U64 offset, unsigned char* bytes, DWORD length) { memcpy(bytes,data.data()+size_t(offset),length); return true; },
            [&](U64 sent) { std::lock_guard<std::mutex> hold(CopyLock); c.done=sent; return !c.stop.load(); });
        if(!ok) {
            std::string why=ftp.Error();
            if(!ftp.Delete(remote.c_str()) && !ftp.Missing()) Log("Xbox %s: couldn't delete the part of %s that was sent: %s",c.console.c_str(),remote.c_str(),ftp.Error());
            if(c.stop) EndCopy(c,console,"stopped",""); else EndCopy(c,console,"failed","Sending "+remote+" stopped: "+why);
            return;
        }
        Log("Xbox %s: copied %s from Aurora's store to %s.",c.console.c_str(),from.path.c_str(),remote.c_str());
    }
    { std::lock_guard<std::mutex> hold(CopyLock); c.done=c.total; }
    RefreshTus(ftp,o,c.address,titleId);
    std::string aurora=TuInAurora(ftp,o,c,console,titleId,mediaId,&from,remote,from.where=="aurora" ? &data : NULL);
    { std::lock_guard<std::mutex> hold(CopyLock); c.note=aurora; }
    EndCopy(c,console,"done","");
}

// Moves the game's updates aside, so it starts without one.
void OffTu(std::shared_ptr<ContentCopy> job, DWORD titleId, DWORD mediaId) {
    ContentCopy& c=*job;
    Options o=Current();
    const Console* console=ConsoleIfKnown(c.address);
    FtpClient ftp(c.address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),30);
    if(!OpenFor(c,ftp,console)) return;
    std::vector<std::string> moved;
    MoveTusAside(ftp,o,c,titleId,mediaId,std::string(),moved);
    RefreshTus(ftp,o,c.address,titleId);
    std::string aurora=TuInAurora(ftp,o,c,console,titleId,mediaId,NULL,std::string(),NULL);
    {
        std::lock_guard<std::mutex> hold(CopyLock);
        c.done=c.total; c.skipped=moved.empty() ? 1 : 0;
        c.note=(moved.empty() ? std::string("It had no update of it on.") : "Moved "+std::to_string(moved.size())+" to "+XboxAppFolder(o)+"/"+TuBackupFolder+".")+" "+aurora;
    }
    EndCopy(c,console,"done","");
}

// The listed game called name: its Title ID and Media ID; false if it's not one with updates.
bool TuGame(const std::string& name, IsoFile& game, std::string& error) {
    std::lock_guard<std::mutex> hold(GamesListLock);
    for(size_t i=0;i<GamesList.size();++i)
        if(config::ToUtf8(WithoutIso(GamesList[i].name))==name) {
            game=GamesList[i];
            if(!game.titleId || !game.mediaId) { error=name+" isn't an Xbox 360 game, so it has no title updates."; return false; }
            return true;
        }
    error="There's no game called "+name+" here.";
    return false;
}

// Whether the console has the game: a tile of it, or it installed.
bool ConsoleHasGame(const Console& c, const IsoFile& g) {
    std::lock_guard<std::mutex> hold(ConsoleLock);
    if(c.tiles.count(Lower(g.name)) || c.godTiles.count(Lower(g.name))) return true;
    for(size_t i=0;i<c.installed.size();++i) if(c.installed[i].titleId==g.titleId) return true;
    return false;
}

// Which of offers (newest first) a console's files are: the index of the
// newest it has, or -1; other: it has an update of the disc that isn't one of them.
int TuOnConsole(const ConsoleTus& t, DWORD titleId, DWORD mediaId, const std::vector<tu::Offer>& offers, const Options& o, bool& other) {
    other=false;
    int best=-1;
    for(size_t i=0;i<t.files.size();++i) {
        const TuFile& f=t.files[i];
        if(f.where!="live") continue;  // Switched off.
        bool cache=!f.path.compare(0,strlen(tu::CacheFolder),tu::CacheFolder);
        if(!cache && (f.titleId!=titleId || (f.mediaId && f.mediaId!=mediaId))) continue;
        bool known=false;
        for(size_t k=0;k<offers.size();++k) {
            std::string path, name;
            bool same=(!f.contentId.empty() && f.contentId==offers[k].hash) || (offers[k].size && tu::SameSize(f.size,offers[k].size) && !cache) ||
                      (TuCached(o,titleId,offers[k].id,path,name) && !_stricmp(tu::FatxName(name).c_str(),f.name.c_str()));
            if(!same) continue;
            known=true;
            if(best<0 || int(k)<best) best=int(k);
        }
        if(!known && !cache) other=true;
    }
    return best;
}

void TuOfferJson(web::Json& j, const tu::Offer& u, DWORD titleId, const Options& o) {
    std::string path, name;
    bool here=TuCached(o,titleId,u.id,path,name);
    j.Open('{').Set("id",u.id).Set("name",u.name).Set("version",u.version).Set("size",(unsigned long long)u.size).Set("uploaded",u.uploaded)
     .Set("mediaId",u.mediaId).Set("onPc",here).Set("file",here ? name : std::string()).Close('}');
}

// GET /api/titleupdates?game=: XboxUnity's updates for the game's disc, and
// what each console has of them (as last checked).
void TitleUpdates(const std::string& name, bool refresh, web::Reply& out) {
    IsoFile g;
    std::string error;
    if(!TuGame(name,g,error)) { out.Error(404,error); return; }
    std::vector<tu::Offer> all;
    bool looked=LookupTus(g.titleId,refresh,all,error);
    std::vector<tu::Offer> offers=tu::ForMedia(all,g.mediaId);
    Options o=Current();
    web::Json j;
    j.Open('{').Set("game",name).Set("titleId",Hex8(g.titleId)).Set("mediaId",Hex8(g.mediaId)).Set("error",looked ? std::string() : error);
    j.Set("otherDiscs",int(all.size()-offers.size()));
    j.Key("offers").Open('[');
    for(size_t i=0;i<offers.size();++i) TuOfferJson(j,offers[i],g.titleId,o);
    j.Close(']').Key("consoles").Open('[');
    std::vector<Console*> consoles=AllConsoles();
    for(size_t i=0;i<consoles.size();++i) {
        const Console& c=*consoles[i];
        ConsoleTus t;
        { std::lock_guard<std::mutex> hold(TuLock); std::map<DWORD,ConsoleTus>::const_iterator f=TuConsoles.find(c.address); if(f!=TuConsoles.end()) t=f->second; }
        bool other=false;
        int has=t.checkedAt ? TuOnConsole(t,g.titleId,g.mediaId,offers,o,other) : -1;
        j.Open('{').Set("address",std::string(c.text)).Set("hasGame",ConsoleHasGame(c,g)).Set("checked",t.checkedAt).Set("checkError",t.error);
        j.Set("has",has>=0 ? offers[size_t(has)].id : std::string()).Set("hasName",has>=0 ? offers[size_t(has)].name : std::string()).Set("other",other);
        j.Key("files").Open('[');
        for(size_t k=0;k<t.files.size();++k) {
            const TuFile& f=t.files[k];
            bool cache=!f.path.compare(0,strlen(tu::CacheFolder),tu::CacheFolder);
            if(!cache && f.titleId!=g.titleId) continue;
            // A Cache update has no header: only one downloaded here is known to be this game's.
            int offer=TuOfferOf(o,g.titleId,f,offers);
            if(cache && offer<0) continue;
            if(!cache && f.mediaId && f.mediaId!=g.mediaId) continue;  // Another version of the game's.
            j.Open('{').Set("path",f.path).Set("name",f.name).Set("size",(unsigned long long)f.size).Set("title",f.displayName).Set("mediaId",f.mediaId ? Hex8(f.mediaId) : std::string());
            j.Set("where",f.where).Set("on",f.where=="live").Set("version",TuNumber(f,offer,offers)).Set("contentId",f.contentId).Set("offer",offer>=0 ? offers[size_t(offer)].id : std::string()).Close('}');
        }
        j.Close(']').Close('}');
    }
    j.Close(']').Close('}');
    out.body=j.Text();
}

// Queues an update for a console; replace: older ones of the disc move out of the way.
bool QueueTu(const IsoFile& g, const tu::Offer& offer, DWORD at, bool replace, std::string& error) {
    std::shared_ptr<ContentCopy> job(new ContentCopy);
    job->kind="update"; job->game=config::ToUtf8(WithoutIso(g.name)); job->disc=job->game;
    job->label=offer.version>0 ? "Title update "+std::to_string(offer.version) : offer.name; job->key=replace ? offer.id : std::string();
    job->total=offer.size; job->files=1;
    DWORD titleId=g.titleId, mediaId=g.mediaId;
    job->run=[titleId,mediaId,offer](std::shared_ptr<ContentCopy> j) { PutTu(j,titleId,mediaId,offer); };
    if(!AddCopy(job,at,error)) return false;
    Log("Xbox %s: queued %s for %s (XboxUnity %s).",job->console.c_str(),job->label.c_str(),job->game.c_str(),offer.id.c_str());
    return true;
}

// POST /api/titleupdates/use {game, address, path}: the switched-off update at
// path (as the console's last check listed it) made the one the game uses.
// POST /api/titleupdates/off {game, address}: none of the game's updates on.
bool SwitchTitleUpdate(const web::Value& body, bool off, std::string& error) {
    std::string name, address, path;
    Text(body,"game",name); Text(body,"address",address); Text(body,"path",path);
    IsoFile g;
    DWORD at=INADDR_NONE;
    if(!TuGame(name,g,error) || !CopyTarget(address,at,error)) return false;
    std::shared_ptr<ContentCopy> job(new ContentCopy);
    job->game=config::ToUtf8(WithoutIso(g.name)); job->disc=job->game; job->files=1;
    DWORD titleId=g.titleId, mediaId=g.mediaId;
    if(off) {
        job->kind="tuoff"; job->label="Title updates off";
        job->run=[titleId,mediaId](std::shared_ptr<ContentCopy> j) { OffTu(j,titleId,mediaId); };
    } else {
        TuFile from;
        bool found=false;
        {
            std::lock_guard<std::mutex> hold(TuLock);
            std::map<DWORD,ConsoleTus>::const_iterator t=TuConsoles.find(at);
            if(t!=TuConsoles.end())
                for(size_t i=0;i<t->second.files.size() && !found;++i) {
                    const TuFile& f=t->second.files[i];
                    if(f.path==path && f.where!="live" && f.titleId==titleId && (!f.mediaId || f.mediaId==mediaId)) { from=f; found=true; }
                }
        }
        if(!found) { error="The console's last check has no switched-off update of "+name+" at "+path+"; check it again."; return false; }
        std::vector<tu::Offer> offers;
        { std::lock_guard<std::mutex> hold(TuLock); std::map<DWORD,TuTitle>::const_iterator t=TuInfo.find(titleId); if(t!=TuInfo.end()) offers=t->second.offers; }
        int number=TuNumber(from,TuOfferOf(Current(),titleId,from,offers),offers);
        job->kind="update"; job->label=number ? "Title update "+std::to_string(number) : from.name; job->total=from.size;
        job->run=[titleId,mediaId,from](std::shared_ptr<ContentCopy> j) { UseTu(j,titleId,mediaId,from); };
    }
    if(!AddCopy(job,at,error)) return false;
    Log("Xbox %s: queued %s for %s.",job->console.c_str(),off ? "title updates off" : job->label.c_str(),job->game.c_str());
    return true;
}

// POST /api/titleupdates/install {game, id (default: the newest), address
// (or all: each console with the game), keep (leave older ones where they are)}
// or {everything: true}: the newest of every game to each console with it
// that hasn't got it, by the last checks.
bool InstallTitleUpdates(const web::Value& body, std::string& error, int& queued) {
    queued=0;
    Options o=Current();
    const web::Value* flag=body.Get("everything");
    bool everything=flag && flag->type==web::Value::Bool && flag->boolean;
    flag=body.Get("keep");
    bool replace=!(flag && flag->type==web::Value::Bool && flag->boolean);
    flag=body.Get("all");
    bool all=everything || (flag && flag->type==web::Value::Bool && flag->boolean);
    std::string name, id, address;
    Text(body,"game",name); Text(body,"id",id); Text(body,"address",address);
    std::vector<IsoFile> games;
    if(everything) {
        std::lock_guard<std::mutex> hold(GamesListLock);
        std::set<std::pair<DWORD,DWORD> > seen;
        for(size_t i=0;i<GamesList.size();++i)
            if(GamesList[i].titleId && GamesList[i].mediaId && seen.insert(std::make_pair(GamesList[i].titleId,GamesList[i].mediaId)).second) games.push_back(GamesList[i]);
    } else {
        IsoFile g;
        if(!TuGame(name,g,error)) return false;
        games.push_back(g);
    }
    std::vector<Console*> consoles=AllConsoles();
    std::string why;
    for(size_t i=0;i<games.size();++i) {
        const IsoFile& g=games[i];
        std::vector<tu::Offer> offers;
        if(everything) {
            std::lock_guard<std::mutex> hold(TuLock);
            std::map<DWORD,TuTitle>::const_iterator t=TuInfo.find(g.titleId);
            if(t==TuInfo.end()) continue;  // Not looked up: the scan does that first.
            offers=tu::ForMedia(t->second.offers,g.mediaId);
        } else {
            std::vector<tu::Offer> found;
            if(!LookupTus(g.titleId,false,found,error)) { error="Couldn't ask XboxUnity: "+error; return false; }
            offers=tu::ForMedia(found,g.mediaId);
        }
        if(offers.empty()) { if(!everything) { error="XboxUnity has no title update for this disc of "+name+"."; return false; } continue; }
        size_t pick=0;
        if(!id.empty() && !everything) {
            while(pick<offers.size() && offers[pick].id!=id) ++pick;
            if(pick==offers.size()) { error="XboxUnity has no such update for this disc."; return false; }
        }
        for(size_t k=0;k<consoles.size();++k) {
            const Console& c=*consoles[k];
            if(!all && address!=c.text) continue;
            if(all && (!ConsoleHasGame(c,g) || c.offline)) continue;
            if(everything) {
                ConsoleTus t;
                { std::lock_guard<std::mutex> hold(TuLock); std::map<DWORD,ConsoleTus>::const_iterator f=TuConsoles.find(c.address); if(f==TuConsoles.end()) continue; t=f->second; }
                bool other=false;
                if(TuOnConsole(t,g.titleId,g.mediaId,offers,o,other)==0) continue;  // Has the newest.
            }
            DWORD at=INADDR_NONE;
            std::string one;
            if(!CopyTarget(c.text,at,one) || !QueueTu(g,offers[pick],at,replace,one)) { if(why.empty()) why=one; continue; }
            ++queued;
        }
    }
    if(!queued && !everything) {
        error=why.empty() ? (all ? "No console has "+name+" (a tile of it, or it installed)." : "There's no console at "+address+".") : why;
        return false;
    }
    if(!queued && everything && !why.empty()) { error=why; return false; }
    return true;
}

// POST /api/titleupdates/scan: looks up every game here on XboxUnity (one a
// second, those looked up lately left out), then queues a check of each
// console in Aurora.
void ScanTitleUpdates() {
    std::vector<DWORD> titles;
    { std::set<DWORD> all=LibraryTitles(); titles.assign(all.begin(),all.end()); }
    {
        std::lock_guard<std::mutex> hold(TuLock);
        TuScan.total=int(titles.size()); TuScan.done=0; TuScan.found=0;
    }
    int found=0;
    for(size_t i=0;i<titles.size();++i) {
        bool fresh;
        { std::lock_guard<std::mutex> hold(TuLock); std::map<DWORD,TuTitle>::const_iterator t=TuInfo.find(titles[i]); fresh=t!=TuInfo.end() && t->second.fetchedAt && Now()-t->second.fetchedAt<TuFresh && t->second.error.empty(); }
        std::vector<tu::Offer> offers;
        std::string error;
        if(LookupTus(titles[i],false,offers,error) && !offers.empty()) ++found;
        { std::lock_guard<std::mutex> hold(TuLock); TuScan.done=int(i+1); TuScan.found=found; }
        if(!fresh) Sleep(1000);  // Gently: XboxUnity is run by volunteers.
    }
    std::vector<Console*> consoles=AllConsoles();
    for(size_t i=0;i<consoles.size();++i) {
        if(consoles[i]->offline) continue;
        std::string error;
        QueueTuCheck(consoles[i]->address,error);
    }
    Log("Looked up %d game(s) on XboxUnity: %d have title updates.",int(titles.size()),found);
    std::lock_guard<std::mutex> hold(TuLock);
    TuScan.running=false; TuScan.endedAt=(long long)time(NULL);
}

bool StartTuScan(std::string& error) {
    std::lock_guard<std::mutex> hold(TuLock);
    if(TuScan.running) { error="It's looking already."; return false; }
    TuScan=TuScanState();
    TuScan.running=true; TuScan.startedAt=(long long)time(NULL);
    std::thread(ScanTitleUpdates).detach();
    return true;
}

// GET /api/titleupdates/all: every game here with XboxUnity's newest update
// for its disc (by the last look-up), and whether each console has it.
void AllTitleUpdates(web::Reply& out) {
    Options o=Current();
    std::vector<IsoFile> games;
    {
        std::lock_guard<std::mutex> hold(GamesListLock);
        std::set<std::pair<DWORD,DWORD> > seen;
        for(size_t i=0;i<GamesList.size();++i)
            if(GamesList[i].titleId && GamesList[i].mediaId && seen.insert(std::make_pair(GamesList[i].titleId,GamesList[i].mediaId)).second) games.push_back(GamesList[i]);
    }
    std::vector<Console*> consoles=AllConsoles();
    std::map<DWORD,TuTitle> info;
    std::map<DWORD,ConsoleTus> checks;
    TuScanState scan;
    { std::lock_guard<std::mutex> hold(TuLock); info=TuInfo; checks=TuConsoles; scan=TuScan; }
    web::Json j;
    j.Open('{').Key("scan").Open('{').Set("running",scan.running).Set("done",scan.done).Set("total",scan.total).Set("found",scan.found).Set("ended",scan.endedAt).Close('}');
    j.Key("consoles").Open('[');
    for(size_t k=0;k<consoles.size();++k) {
        std::map<DWORD,ConsoleTus>::const_iterator t=checks.find(consoles[k]->address);
        j.Open('{').Set("address",std::string(consoles[k]->text)).Set("checked",t==checks.end() ? 0LL : t->second.checkedAt).Set("error",t==checks.end() ? std::string() : t->second.error).Close('}');
    }
    j.Close(']').Key("games").Open('[');
    for(size_t i=0;i<games.size();++i) {
        const IsoFile& g=games[i];
        std::map<DWORD,TuTitle>::const_iterator t=info.find(g.titleId);
        if(t==info.end()) continue;
        std::vector<tu::Offer> offers=tu::ForMedia(t->second.offers,g.mediaId);
        if(offers.empty()) continue;
        j.Open('{').Set("game",config::ToUtf8(WithoutIso(g.name))).Set("titleId",Hex8(g.titleId)).Key("newest");
        TuOfferJson(j,offers[0],g.titleId,o);
        j.Key("consoles").Open('[');
        for(size_t k=0;k<consoles.size();++k) {
            const Console& c=*consoles[k];
            std::map<DWORD,ConsoleTus>::const_iterator ct=checks.find(c.address);
            bool other=false;
            int has=ct!=checks.end() && ct->second.checkedAt ? TuOnConsole(ct->second,g.titleId,g.mediaId,offers,o,other) : -1;
            j.Open('{').Set("address",std::string(c.text)).Set("hasGame",ConsoleHasGame(c,g)).Set("checked",ct!=checks.end() && ct->second.checkedAt!=0)
             .Set("newest",has==0).Set("has",has>=0 ? offers[size_t(has)].name : std::string()).Set("other",other).Close('}');
        }
        j.Close(']').Close('}');
    }
    j.Close(']').Close('}');
    out.body=j.Text();
}

// Starting a game from the dashboard -------------------------------------------

// Starts a game on a console through Aurora's web server: its game folder
// there, or its copy installed on the hard drive. The console leaves what it
// runs, a game too (the page asks first).
bool Play(const web::Value& v, std::string& error) {
    std::string name, address;
    if(!Text(v,"game",name) || !Text(v,"console",address)) { error="Send the game and the console."; return false; }
    DWORD at=inet_addr(address.c_str());
    Console* c=at==INADDR_NONE || !at ? NULL : ConsoleIfKnown(at);
    if(!c) { error="That console hasn't used NetDisc360 since it started."; return false; }
    IsoFile game;
    bool found=false;
    {
        std::lock_guard<std::mutex> hold(GamesListLock);
        for(size_t i=0;i<GamesList.size() && !found;++i) if(config::ToUtf8(WithoutIso(GamesList[i].name))==name) { game=GamesList[i]; found=true; }
    }
    if(!found) { error="That game isn't in your folders any more."; return false; }
    Options o=Current();
    std::string folder;
    bool god=false;
    {
        std::lock_guard<std::mutex> hold(ConsoleLock);
        std::map<std::string,std::string>::const_iterator tile=c->tiles.find(Lower(game.name));
        if(tile!=c->tiles.end()) folder=o.xboxFolder+"/"+tile->second;
        tile=c->godTiles.find(Lower(game.name));
        if(folder.empty() && tile!=c->godTiles.end()) folder=tile->second;
        for(size_t i=0;i<c->installed.size() && folder.empty();++i) {
            const InstalledGame& g=c->installed[i];
            if(g.titleId!=game.titleId || g.disc!=game.disc) continue;
            if(g.god) god=true;
            else folder=g.path.substr(0,g.path.rfind('/'));
        }
    }
    if(folder.empty()) {
        error=god ? "It's installed on that console as Games on Demand; start it in Aurora."
                  : "That console has no tile for it yet: its game folders are written while it's in Aurora.";
        return false;
    }
    std::string device=DevicePath(folder);
    if(device.empty()) { error="Aurora starts games from the dashboard on the hard drive only, and this one is in "+folder+"."; return false; }
    AuroraWeb aurora(c->address,o.auroraWebPort,o.auroraWebUser,o.auroraWebPassword,8);
    if(!aurora.Launch(device,"default.xex")) {
        error=aurora.Error()+".";
        XboxLog(*c,"couldn't start %s from the dashboard: %s",WithoutIso(game.name).c_str(),error.c_str());
        return false;
    }
    XboxLog(*c,"started %s from the dashboard (%s).",WithoutIso(game.name).c_str(),folder.c_str());
    XboxEvent(*c,EventPlay,WithoutIso(game.name)+" started from the dashboard");
    return true;
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

// Turns a console off (its debug monitor), or starts Aurora again on it
// (Aurora's web server), from a game too.
bool Power(const web::Value& v, std::string& error) {
    std::string address, action;
    Text(v,"address",address);
    Text(v,"action",action);
    Console* c=AskedConsole(address,error);
    if(!c) return false;
    Options o=Current();
    if(action=="off") {
        if(!Xbdm(c->address,o.xbdmPort,"shutdown",error)) { error="It can't be turned off from here: "+error+"."; return false; }
        XboxLog(*c,"turned off (asked on the dashboard).");
        XboxEvent(*c,EventXbox,"Turned off from the dashboard");
        return true;
    }
    if(action!="aurora") { error="Send action off or aurora."; return false; }
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
    return true;
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
        Options o=Current();
        for(std::map<std::string,std::string>::const_iterator t=c->tiles.begin();t!=c->tiles.end() && folder.empty();++t)
            if(Lower(config::ToUtf8(WithoutIso(t->first)))==Lower(name)) folder=o.xboxFolder+"/"+t->second;
        for(std::map<std::string,std::string>::const_iterator t=c->godTiles.begin();t!=c->godTiles.end() && folder.empty();++t)
            if(Lower(config::ToUtf8(WithoutIso(t->first)))==Lower(name)) folder=t->second;
    }
    if(folder.empty()) { out.Error(400,"That console has no folder for it."); return; }
    Options o=Current();
    std::string path=folder;
    FtpClient ftp(c->address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),8);
    std::vector<FtpEntry> files;
    // Listed first: FtpDll answers a missing file late, and the next command with it.
    if(!ftp.List(path.c_str(),files)) { out.Error(400,ftp.Code() ? std::string("Its game folder isn't there.") : "Aurora's FTP server doesn't answer: it runs only while Aurora is on screen."); return; }
    const FtpEntry* log=NULL;
    for(size_t i=0;i<files.size() && !log;++i) if(!files[i].folder && !_stricmp(files[i].name.c_str(),"netiso.log")) log=&files[i];
    web::Json j;
    j.Open('{').Set("folder",config::ToUtf8(folder));
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
    else if(r.method=="POST" && r.path=="/api/install") {
        if(StartCopy(body,error)) out.body="{\"ok\":true}";
        else out.Error(400,error);
    } else if(r.method=="POST" && r.path=="/api/copy") {
        if(StartGameCopy(body,error)) out.body="{\"ok\":true}";
        else out.Error(400,error);
    } else if(r.method=="POST" && r.path=="/api/consoles/tiles") {
        if(SetTileChoice(body,error)) out.body="{\"ok\":true}";
        else out.Error(400,error);
    } else if(r.method=="POST" && r.path=="/api/console/remove") {
        if(StartRemove(body,error)) out.body="{\"ok\":true}";
        else out.Error(400,error);
    } else if(r.method=="GET" && r.path=="/api/titleupdates") TitleUpdates(r.Param("game"),r.Param("refresh")=="1",out);
    else if(r.method=="GET" && r.path=="/api/titleupdates/all") AllTitleUpdates(out);
    else if(r.method=="POST" && r.path=="/api/titleupdates/install") {
        int queued=0;
        if(InstallTitleUpdates(body,error,queued)) out.body="{\"ok\":true,\"queued\":"+std::to_string(queued)+"}";
        else out.Error(400,error);
    } else if(r.method=="POST" && (r.path=="/api/titleupdates/use" || r.path=="/api/titleupdates/off")) {
        if(SwitchTitleUpdate(body,r.path=="/api/titleupdates/off",error)) out.body="{\"ok\":true}";
        else out.Error(400,error);
    } else if(r.method=="POST" && r.path=="/api/titleupdates/scan") {
        if(StartTuScan(error)) out.body="{\"ok\":true}";
        else out.Error(400,error);
    } else if(r.method=="POST" && r.path=="/api/titleupdates/check") {
        std::string address;
        Text(body,"address",address);
        DWORD at=INADDR_NONE;
        if(CopyTarget(address,at,error) && QueueTuCheck(at,error)) out.body="{\"ok\":true}";
        else out.Error(400,error);
    } else if(r.method=="POST" && r.path=="/api/install/stop") {
        if(StopCopy(body,error)) out.body="{\"ok\":true}";
        else out.Error(400,error);
    }
    else if(r.method=="GET" && r.path=="/api/setup") SetupCheck(out);
    else if(r.method=="GET" && r.path=="/api/console/status") ConsoleStatus(r.Param("address"),out);
    else if(r.method=="GET" && r.path=="/api/console/gamelog") GameLog(r.Param("address"),r.Param("game"),out);
    else if((r.method=="GET" && (r.path=="/api/console/screenshots" || r.path=="/api/console/screenshot")) ||
            (r.method=="POST" && (r.path=="/api/console/screenshot/take" || r.path=="/api/console/screenshot/delete"))) Screenshots(r,body,out);
    else if(r.method=="POST" && r.path=="/api/console/power") {
        if(Power(body,error)) out.body="{\"ok\":true}";
        else out.Error(400,error);
    }
    else if(r.method=="POST" && r.path=="/api/play") {
        if(Play(body,error)) out.body="{\"ok\":true}";
        else out.Error(400,error);
    }
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
    } else if(r.method=="GET" && r.path=="/api/saves") SaveList(out);
    else if(r.method=="GET" && r.path=="/api/save") {
        std::string name=r.Param("name");
        Options o=Current();
        if(SavesFolder(o).empty() || !saves::Named(name) || !config::ReadFile(SavePath(o,name),out.body,300<<20)) out.Error(404,"That save backup isn't there any more.");
        else { out.type="application/zip"; out.download=name; }
    } else if(r.method=="POST" && r.path=="/api/saves/backup") {
        if(!SaveAll(body,out,error)) out.Error(400,error);
    } else if(r.method=="POST" && r.path=="/api/saves/conflict") {
        if(ResolveConflict(body,error)) out.body="{\"ok\":true}";
        else out.Error(400,error);
    } else if(r.method=="POST" && r.path=="/api/saves/restore") {
        if(RestoreSaves(body,error)) out.body="{\"ok\":true}";
        else out.Error(400,error);
    } else if(r.method=="POST" && r.path=="/api/saves/delete") {
        std::string name;
        Text(body,"name",name);
        Options o=Current();
        if(SavesFolder(o).empty() || !saves::Named(name) || !config::RemoveFile(SavePath(o,name))) out.Error(404,"That save backup isn't there any more.");
        else { Log("Deleted the save backup %s.",name.c_str()); out.body="{\"ok\":true}"; }
    } else if(r.method=="POST" && !r.path.compare(0,10,"/api/sync/")) SyncApi(r.path.substr(10),body,out);
    else if(r.method=="GET" && r.path=="/api/update") UpdateInfo(out);
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
        "  --xbox-folder <path>    the game folder on the Xbox, as an FTP path (/Hdd1/NetDisc360/Games)\n"
        "  --aurora <path>         Aurora's folder on the Xbox, as an FTP path (/Hdd1/Apps/Aurora)\n"
        "  --no-push               only serve games; leave the Xbox alone\n"
        "  --no-category           leave Aurora's Xbox 360 category alone\n"
        "  --no-scan               don't ask Aurora to scan after adding or removing games\n"
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
        else if(a=="--push") o.push=true;  // After --no-push (the tests' dashboard servers).
        else if(a=="--no-category") o.filter=false;
        else if(a=="--no-scan") o.scan=false;
        else if(a=="--no-installed") o.skipInstalled=false;
        else if(a=="--no-aurora-scripts") o.auroraScripts=false;
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
        else if(hasValue && a=="--xbdm-port") o.xbdmPort=atoi(args[++i].c_str());
        else if(hasValue && a=="--xboxunity") o.xboxUnity=args[++i];
        else if(hasValue && a=="--port") o.port=atoi(args[++i].c_str());
        else if(hasValue && a=="--ftp-port") o.ftpPort=atoi(args[++i].c_str());
        else if(hasValue && a=="--launcher") o.launcherPath=args[++i];
        else if(hasValue && a=="--boot") o.bootPath=args[++i];
        else if(hasValue && a=="--online-wait") o.onlineWait=atof(args[++i].c_str());
        else if(hasValue && a=="--remove-wait") o.removeWait=atof(args[++i].c_str());
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
    Log("NetDisc360 %s, built %s %s.",NETDISC_VERSION,__DATE__,__TIME__);
    if(!o.configPath.empty()) Log("Settings: %s",config::FromUtf8(o.configPath).c_str());
    if(!o.configPath.empty()) Log("Disc map: %s",config::FromUtf8(DiscMapPath(o.configPath)).c_str());

    if(o.push || !o.launcherPath.empty()) {
        CanPush=o.launcherPath.empty() ? BuiltIn(IDR_LAUNCHER,Launcher) : LoadFile(o.launcherPath,Launcher);
        if(!o.launcherPath.empty() && !CanPush) { fprintf(stderr,"Can't read %s.\n",o.launcherPath.c_str()); return 2; }
        // boot.xex is optional: without it, each launcher mounts by itself.
        if(o.bootPath.empty()) BuiltIn(IDR_BOOT,Boot);
        else if(!LoadFile(o.bootPath,Boot)) { fprintf(stderr,"Can't read %s.\n",o.bootPath.c_str()); return 2; }
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
    LoadOtherPlays(o);
    LoadConflicts(o);
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
    std::thread(PingLoop).detach();
    std::thread(SizeLoop).detach();
#ifdef _WIN32
    std::thread(AwakeLoop).detach();
#endif
    std::thread(BackupLoop).detach();
    std::thread(SyncLoop).detach();
    std::thread(SaveLoop).detach();
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
            FtpClient ftp(c->address,o.ftpPort,o.ftpUser.c_str(),o.ftpPassword.c_str(),3);
            std::string helper=XboxAppFolder(o);
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
