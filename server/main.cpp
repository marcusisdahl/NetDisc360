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
#include "../shared/library.h"
#include "app.h"
#include "backup.h"
#include "config.h"
#include "discmap.h"
#include "discs.h"
#include "ftp.h"
#include "update.h"
#include "version.h"
#include "web.h"
#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
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
    Options() : ftpUser("xboxftp"), ftpPassword("xboxftp"), xboxFolder("/Hdd1/NetISO"), aurora("/Hdd1/Apps/Aurora"), webAddress("127.0.0.1"),
                port(IsvrPort), ftpPort(21), webPort(WebPort), push(true), filter(true), scan(true), verbose(false), pushOnce(false),
                webLan(false), setupDone(false), toldTray(false), onlineWait(120), removeWait(60), pingSeconds(30),
                backupEvery(7), backupKeep(5), updateCheck(true), updateAuto(false), updatePrerelease(false) {}
};
// The dashboard changes the settings while the server runs, so they're read
// with Current() (a copy) and changed under SettingsLock.
Options Settings;
std::mutex SettingsLock;
Options Current() { std::lock_guard<std::mutex> hold(SettingsLock); return Settings; }

std::vector<unsigned char> Launcher, Boot;
bool CanPush;         // A launcher is built in (or given), so game folders can be written.
bool InTray;          // NetDisc360.exe behind its notification-area icon (tray.cpp).
bool PasswordFixed;   // The dashboard's password comes from NETDISC_WEB_PASSWORD.
bool InDocker;        // In the Docker image (NETDISC_DOCKER), which is updated by pulling it.
double StartedAt;
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

// Everything in the folders, each game once. A game that is there more than
// once is reported (once, until that changes) so the copies can be removed or
// one of them chosen.
std::vector<IsoFile> Isos() {
    std::vector<std::string> folders=Current().folders;
    std::vector<IsoFile> isos;
    std::set<std::string> paths;
    std::string missing;
    for(size_t i=0;i<folders.size();++i) {
        std::string folder=folders[i];
        if(folder.size()==3 && folder[1]==':') folder+='.';  // "D:\." for the top of D:.
        std::vector<IsoFile> found;
        if(!FindIsos(folder,found)) missing+="  "+folders[i]+"\n";
        // A folder inside another one listed finds the same discs again.
        for(size_t k=0;k<found.size();++k) if(paths.insert(found[k].path).second) isos.push_back(found[k]);
    }
    FoldersMissing=!missing.empty();
    {
        std::lock_guard<std::mutex> hold(MissingLock);
        if(missing!=MissingFolders) {
            if(!missing.empty()) Log("Can't read these game folders; until they're back, no game is removed from the consoles:\n%s",missing.substr(0,missing.size()-1).c_str());
            else Log("Every game folder can be read again.");
            MissingFolders=missing;
        }
    }
    ReadTitles(isos);
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
    explicit Console(DWORD at) : address(at), push(FromXbox(at)), seenAt(-1), connections(0), sent(0), ftp(0), synced(0), counted(0), hiddenAt(-2),
                                 inAurora(false), sentInAurora(0), probeAt(0), pingMs(-1), missed(0), answers(false), answered(false), offline(false), toldNoPings(false),
                                 answeredAt(-1), offlineAt(0), pingAt(0), suspectAt(0) {
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

// A console that has used the server, kept in the settings file so the
// dashboard knows it after a restart.
struct KnownConsole { std::string address; long long seen; };
std::vector<KnownConsole> Known;  // Under SettingsLock.

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

// Tells when a console in a game is back in Aurora: its FTP port is tried
// (without logging in) FirstProbeSeconds after the mount and then every
// GameProbeSeconds, only while it's in a game, and answering means Aurora
// runs again. A game started from the disc in Aurora shows by its reads.
void AuroraLoop() {
    for(;;) {
        int port=Current().ftpPort;
        std::vector<Console*> consoles=AllConsoles();
        for(size_t i=0;i<consoles.size();++i) {
            Console& c=*consoles[i];
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
    XboxLog(c,"offline: the last %d pings got no answer%s.",OfflinePings,c.suspectAt ? ", and its NetISO connection ended" : "");
    c.suspectAt=0;
    XboxEvent(c,EventXbox,"Went offline",std::string(c.text)+" went offline");
}

void BackOnline(Console& c, const char* how) {
    c.offline=false;
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
            if(console_) {
                console_->sent+=length;
                if(console_->inAurora && console_->sent-console_->sentInAurora>RelaunchBytes) {  // In a game again.
                    console_->inAurora=false;
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
        if(!found && !name.empty()) {  // Added since the latest look, perhaps.
            isos=Isos();
            found=Find(isos,name);
            ScanSoon=true;
        }
        if(!found) { Log("%s asked to mount \"%s\", which isn't here.",peer_,name.c_str()); return 0; }
        if(!iso_.Open(*found)) { Log("%s: couldn't open %s: %s.",peer_,found->path.c_str(),iso_.Note().c_str()); mounted_.clear(); Mounted(); return 0; }
        mounted_=found->name; pastEnd_=false;
        Mounted();
        if(console_) { console_->inAurora=false; console_->probeAt=(long long)((Now()+FirstProbeSeconds)*1000); }
        if(!quiet_) {
            if(found->kind==DiscIso) Log("%s: mounted %s.",peer_,found->name.c_str());
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
int RefreshRemote(FtpClient& ftp, const std::string& path, const std::vector<unsigned char>& data);
void RefreshInstallFilter(const Console& console, FtpClient& ftp, const Options& o) {
    std::string text=InstallFilter();
    if(text.empty()) return;
    std::string path=o.aurora+InstallFilterScript;
    int written=RefreshRemote(ftp,path,std::vector<unsigned char>(text.begin(),text.end()));
    if(written>0) XboxLog(console,"wrote Aurora's \"Hide install discs\" filter (%s). Restart Aurora, then pick it under Filters, User.",path.c_str());
    else if(written<0) XboxLog(console,"couldn't write %s: %s",path.c_str(),ftp.Error());
}

// Pushing the game folders ---------------------------------------------------

// Sync keeps its lists in static buffers (it runs on the console too), so the
// consoles' threads push one at a time.
std::mutex SyncLock;
const Console* Pushing;  // The console Sync is writing to, for PushNote (under SyncLock).
void PushNote(const char* text) { XboxLog(*Pushing,"%s",text); }

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
    for(size_t i=0;i<install->packages.size();++i) {
        char line[IsvrNameBytes+64];
        _snprintf_s(line,sizeof(line),_TRUNCATE,"package_%d=%s|%llu\r\n",int(i+1),install->packages[i].path.c_str(),
                    (unsigned long long)install->packages[i].size);
        if(lines.size()+strlen(line)>=sizeof(need.packages)) break;
        lines+=line;
    }
    memcpy(need.packages,lines.c_str(),lines.size()+1);
}

// What the disc map says a disc is, for its game folder's netiso.ini.
void RoleFor(const char* iso, DiscRoleInfo& info) {
    std::lock_guard<std::mutex> hold(GamesListLock);
    for(size_t i=0;i<GamesList.size();++i) {
        const IsoFile& g=GamesList[i];
        if(_stricmp(g.name.c_str(),iso)) continue;
        info.known=true;
        _snprintf_s(info.role,sizeof(info.role),_TRUNCATE,"%s",g.role.c_str());
        _snprintf_s(info.label,sizeof(info.label),_TRUNCATE,"%s",g.roleLabel.c_str());
        return;
    }
}

bool Push(const Console& console, FtpClient& ftp, SyncResult& result, bool removeGone, const Options& o) {
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
    {
        std::lock_guard<std::mutex> hold(SyncLock);
        Pushing=&console;
        ok=Sync(client,storage,o.xboxFolder.c_str(),Launcher.data(),DWORD(Launcher.size()),
                Boot.empty() ? NULL : Boot.data(),DWORD(Boot.size()),PushNote,result,removeGone,InstallFor,RoleFor);
    }
    if(ok && result.failed) XboxLog(console,"game folders written: %d added, %d removed, %d kept (%d updated), %d skipped, %d file(s) failed; trying those again in %.0f s.",
                                    result.added,result.removed,result.kept,result.updated,result.skipped,result.failed,RetryFailedSeconds);
    else if(ok) XboxLog(console,"game folders up to date: %d added, %d removed, %d kept (%d updated), %d skipped.",result.added,result.removed,result.kept,result.updated,result.skipped);
    if(ok && result.held && FoldersMissing) XboxLog(console,"%d game folder(s) have no game here; they stay until every game folder can be read again.",result.held);
    else if(ok && result.held) XboxLog(console,"%d game folder(s) have no game here yet; they're removed if that's still so %.0f s after the start or the last change of game folders.",result.held,o.removeWait);
    else if(!ok) {
        XboxLog(console,"updating the game folders stopped: %s",ftp.Error()[0] ? ftp.Error() : "see above");
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

// What a console's push thread knows about it.
struct PushState {
    std::string pushed;  // The games when its folders were last written.
    double nextProbe, probedAt, retryAt, scanAt, downSince, upSince;
    int scanTries;
    bool attempted, reachable, toldDown, toldWaiting;
    std::atomic<bool> everPushed;
    std::atomic<bool> finished;  // --push-once: done with this console.
    PushState() : nextProbe(0), probedAt(0), retryAt(0), scanAt(0), downSince(0), upSince(0), scanTries(0),
                  attempted(false), reachable(false), toldDown(false), toldWaiting(false), everPushed(false), finished(false) {}
};

void PushTo(Console& console, PushState& s, const std::string& state, bool settled, double startedAt) {
    if(s.finished) return;
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
    if(pushDue && !online && !s.toldWaiting) {
        XboxLog(console,"waiting for it to come online before writing game folders (its NetISO plugin connects to this server; at most %.0f s).",o.onlineWait);
        s.toldWaiting=true;
    }
    if(pushDue && online) {
        SyncResult result;
        s.attempted=true; s.toldWaiting=false;
        double holdFrom=std::max(startedAt,FoldersChangedAt.load()/1000.0);
        bool missing=FoldersMissing;
        if(Push(console,ftp,result,(o.pushOnce || Now()-holdFrom>=o.removeWait) && !missing,o)) {
            s.everPushed=true; s.pushed=state;
            console.synced=(long long)time(NULL);
            if(o.filter) RefreshInstallFilter(console,ftp,o);
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
                std::lock_guard<std::mutex> hold(GamesListLock);
                GamesList=isos; ++GamesVersion; GamesScanned=true;
            }
        }
        Options o=Current();
        // A missing game folder coming back pushes again, removing what is really gone.
        char generation[16]; _snprintf_s(generation,sizeof(generation),_TRUNCATE,"#%d%s",PushGeneration.load(),FoldersMissing ? "!" : "");
        { std::lock_guard<std::mutex> hold(GamesLock); GamesState=state+generation; GamesSettled=Now()-changedAt>=SettleSeconds && !o.folders.empty(); }
        if(tick%10==0) RememberConsoles();
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
            // --push-once (the tests): ends when every console given is done.
            // _Exit, as exit's destructors would pull the settings and lists
            // from under the threads that still run (AuroraLoop crashed in
            // Current() on CI).
            if(o.pushOnce && !states.empty() && finished) { fflush(NULL); _Exit(failed ? 1 : 0); }
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
    f.Set("aurora",o.aurora);
    if(CanPush) f.Set("push",o.push ? "1" : "0");
    f.Set("category",o.filter ? "1" : "0");
    f.Set("scan",o.scan ? "1" : "0");
    f.Set("setup_done",o.setupDone ? "1" : "0");
    f.Set("told_tray",o.toldTray ? "1" : "0");
    f.Set("web_lan",o.webLan ? "1" : "0");
    if(!PasswordFixed) f.Set("web_password",web::PasswordHash());
    f.Set("backup_every",Number(o.backupEvery));
    f.Set("backup_keep",Number(o.backupKeep));
    f.Set("update_check",o.updateCheck ? "1" : "0");
    f.Set("update_auto",o.updateAuto ? "1" : "0");
    f.Set("update_prerelease",o.updatePrerelease ? "1" : "0");
    {
        std::lock_guard<std::mutex> hold(SettingsLock);
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
    if(!f.Get("aurora").empty()) o.aurora=f.Get("aurora");
    o.push=Flag(f,"push",o.push);
    o.filter=Flag(f,"category",o.filter);
    o.scan=Flag(f,"scan",o.scan);
    o.setupDone=Flag(f,"setup_done",o.setupDone);
    o.toldTray=Flag(f,"told_tray",o.toldTray);
    o.webLan=Flag(f,"web_lan",o.webLan);
    if(f.Has("web_port")) o.webPort=int(f.Number("web_port",o.webPort));
    if(!PasswordFixed) web::SetPasswordHash(f.Get("web_password"));
    if(f.Has("backup_every")) o.backupEvery=std::max(0,std::min(365,int(f.Number("backup_every",o.backupEvery))));
    if(f.Has("backup_keep")) o.backupKeep=std::max(1,std::min(100,int(f.Number("backup_keep",o.backupKeep))));
    o.updateCheck=Flag(f,"update_check",o.updateCheck);
    o.updateAuto=Flag(f,"update_auto",o.updateAuto);
    o.updatePrerelease=Flag(f,"update_prerelease",o.updatePrerelease);
    {
        std::lock_guard<std::mutex> hold(SettingsLock);
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
    std::vector<std::string> addresses=PcAddresses();
    for(size_t i=0;i<addresses.size();++i) j.String(addresses[i]);
    j.Close(']');

    j.Key("settings").Open('{');
    j.Key("xboxes").Open('[');
    for(size_t i=0;i<o.xboxes.size();++i) { char text[16]; AddressText(o.xboxes[i],text,sizeof(text)); j.String(text); }
    j.Close(']');
    j.Set("ftpUser",o.ftpUser).Set("ftpPasswordSet",!o.ftpPassword.empty());  // Never the password itself.
    j.Set("push",o.push).Set("canPush",CanPush).Set("category",o.filter).Set("scan",o.scan);
    j.Set("xboxFolder",o.xboxFolder).Set("aurora",o.aurora).Set("setupDone",o.setupDone);
    j.Set("saved",!o.configPath.empty()).Set("webLan",o.webLan).Set("webAddress",WebAddress(o)).Set("lanSwitch",o.webAddress=="127.0.0.1");
    j.Set("password",!web::PasswordHash().empty()).Set("passwordFixed",PasswordFixed);
    j.Set("backupEvery",o.backupEvery).Set("backupKeep",o.backupKeep);
    j.Set("updateCheck",o.updateCheck).Set("updateAuto",o.updateAuto).Set("updatePrerelease",o.updatePrerelease);
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
        j.Open('{').Set("path",config::ToUtf8(o.folders[i])).Set("games",count).Set("found",config::IsFolder(o.folders[i])).Close('}');
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
            j.Open('{').Set("address",c.text).Set("connections",c.connections.load()).Set("sent",(unsigned long long)c.sent.load());
            j.Set("ftp",c.ftp.load()).Set("synced",c.synced.load()).Set("mounted",config::ToUtf8(WithoutIso(c.mounted))).Set("push",c.push.load());
            j.Set("playing",c.connections>0 && !c.mounted.empty() && !c.inAurora).Set("inAurora",c.inAurora.load());
            j.Set("ping",c.pingMs.load()).Set("answers",c.answers && !c.offline).Set("offline",c.offline.load());
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
        j.Open('{').Set("address",known[k].address).Set("connections",0).Set("sent",0).Set("ftp",0).Set("synced",0LL).Set("mounted","");
        j.Set("playing",false).Set("inAurora",false).Set("ping",-1).Set("answers",false).Set("offline",false).Set("answered",0LL);
        j.Set("push",o.xboxes.empty() || std::find(o.xboxes.begin(),o.xboxes.end(),inet_addr(known[k].address.c_str()))!=o.xboxes.end());
        j.Set("seen",known[k].seen).Key("rate").Open('[').Close(']').Key("events").Open('[').Close(']').Close('}');
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
    j.Set("role",g.role).Set("roleLabel",g.roleLabel);
}

void Games(web::Reply& out) {
    std::vector<std::string> folders=Current().folders;
    web::Json j;
    j.Open('{');
    {
        std::lock_guard<std::mutex> hold(GamesListLock);
        j.Set("version",GamesVersion).Key("games").Open('[');
        for(size_t i=0;i<GamesList.size();++i) { j.Open('{'); GameJson(j,GamesList[i],folders); j.Close('}'); }
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
    // Each goes into an FTP command, which a line break would end early.
    const char* ftpFields[]={"ftpUser","ftpPassword","xboxFolder","aurora"};
    for(int i=0;i<4;++i)
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
    Whole(v,"backupEvery",0,365,next.backupEvery,error);
    Whole(v,"backupKeep",1,100,next.backupKeep,error);
    if(!error.empty()) return false;
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
    {
        std::lock_guard<std::mutex> hold(SettingsLock);
        Settings.folders=next.folders; Settings.xboxes=next.xboxes; Settings.ftpUser=next.ftpUser; Settings.ftpPassword=next.ftpPassword;
        Settings.xboxFolder=next.xboxFolder; Settings.aurora=next.aurora; Settings.push=next.push; Settings.filter=next.filter;
        Settings.scan=next.scan; Settings.setupDone=next.setupDone; Settings.webLan=next.webLan;
        Settings.backupEvery=next.backupEvery; Settings.backupKeep=next.backupKeep;
        Settings.updateCheck=next.updateCheck; Settings.updateAuto=next.updateAuto; Settings.updatePrerelease=next.updatePrerelease;
    }
    // Pre-releases in or out changes which release is the newest.
    if(next.updatePrerelease!=was.updatePrerelease || (next.updateCheck && !was.updateCheck)) CheckSoon=true;
    if(next.xboxes!=was.xboxes) UpdateConsoles(next.xboxes);
    // Anything the push depends on writes every console's folders again (only what differs).
    if(next.xboxes!=was.xboxes || login || next.xboxFolder!=was.xboxFolder || next.aurora!=was.aurora ||
       (next.push && !was.push) || (next.filter && !was.filter)) ++PushGeneration;
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
        Settings.setupDone=o.setupDone; Settings.toldTray=o.toldTray; Settings.webLan=o.webLan;
        Settings.backupEvery=o.backupEvery; Settings.backupKeep=o.backupKeep;
        Settings.updateCheck=o.updateCheck; Settings.updateAuto=o.updateAuto && !strcmp(UpdateHow(),"app"); Settings.updatePrerelease=o.updatePrerelease;
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

void Api(const web::Request& r, web::Reply& out) {
    web::Value body;
    if(r.method=="POST" && !web::Parse(r.body,body)) { out.Error(400,"That isn't JSON."); return; }
    std::string error;
    if(r.method=="GET" && r.path=="/api/state") State(out);
    else if(r.method=="GET" && r.path=="/api/games") Games(out);
    else if(r.method=="GET" && r.path=="/api/log") LogSince(atoll(r.Param("after").c_str()),out);
    else if(r.method=="GET" && r.path=="/api/browse") Browse(r.Param("path"),out);
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
        "folder inside is a disc of its own), or Games on Demand packages. Each game is\n"
        "served once; extra copies are listed so they can be removed.\n\n"
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
        else if(a=="--no-category") o.filter=false;
        else if(a=="--no-scan") o.scan=false;
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
    return o.port>0 && o.port<65536 && o.ftpPort>0 && o.ftpPort<65536 && o.webPort>=0 && o.webPort<65536;
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
    bool fresh=!o.configPath.empty() && !LoadConfig(o);
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
    std::thread(BackupLoop).detach();
    std::thread(UpdateLoop).detach();
    Serve(listener);
    return 0;
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
