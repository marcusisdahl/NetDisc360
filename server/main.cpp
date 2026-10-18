// SPDX-License-Identifier: MIT
// NetDisc 360, a NetISO server for Aurora: one program in place of
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
#include "config.h"
#include "discs.h"
#include "ftp.h"
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
const double SettleSeconds = 10, ProbeDownSeconds = 15, RetrySkippedSeconds = 600, ScanRetrySeconds = 30;
// A console that connected once (no --xbox) but not for this long is probed only every IdleProbeSeconds.
const double IdleSeconds = 86400, IdleProbeSeconds = 600;
// While a console is in a game, its FTP port is tried this long after the
// mount and then this often, to see when it's back in Aurora (AuroraLoop).
const double FirstProbeSeconds = 5, GameProbeSeconds = 15;
// Read after it got back to Aurora: a game started from the disc again.
const U64 RelaunchBytes = 32ull << 20;
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
    Options() : ftpUser("xboxftp"), ftpPassword("xboxftp"), xboxFolder("/Hdd1/NetISO"), aurora("/Hdd1/Apps/Aurora"), webAddress("127.0.0.1"),
                port(IsvrPort), ftpPort(21), webPort(WebPort), push(true), filter(true), scan(true), verbose(false), pushOnce(false),
                webLan(false), setupDone(false), toldTray(false), onlineWait(120), removeWait(60) {}
};
// The dashboard changes the settings while the server runs, so they're read
// with Current() (a copy) and changed under SettingsLock.
Options Settings;
std::mutex SettingsLock;
Options Current() { std::lock_guard<std::mutex> hold(SettingsLock); return Settings; }

std::vector<unsigned char> Launcher, Boot;
bool CanPush;         // A launcher is built in (or given), so game folders can be written.
bool InTray;          // NetDisc.exe behind its notification-area icon (tray.cpp).
bool PasswordFixed;   // The dashboard's password comes from NETDISC_WEB_PASSWORD.
double StartedAt;
// Raised by a settings change that needs every console's game folders checked again.
std::atomic<int> PushGeneration(0);
// When the game folders were last changed on the dashboard, in Now() ms: like
// a start, it holds back removals for removeWait (see WatchLoop).
std::atomic<long long> FoldersChangedAt(0);

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

// Everything in the folders, each game once. A game that is there more than
// once is reported (once, until that changes) so the copies can be removed or
// one of them chosen.
std::vector<IsoFile> Isos() {
    std::vector<std::string> folders=Current().folders;
    std::vector<IsoFile> isos;
    std::set<std::string> paths;
    for(size_t i=0;i<folders.size();++i) {
        std::string folder=folders[i];
        if(folder.size()==3 && folder[1]==':') folder+='.';  // "D:\." for the top of D:.
        std::vector<IsoFile> found;
        FindIsos(folder,found);
        // A folder inside another one listed finds the same discs again.
        for(size_t k=0;k<found.size();++k) if(paths.insert(found[k].path).second) isos.push_back(found[k]);
    }
    ReadTitles(isos);
    std::lock_guard<std::mutex> hold(DuplicatesLock);
    std::set<std::string> chosen;
    for(std::map<std::string,Choice>::const_iterator c=Choices.begin();c!=Choices.end();++c) chosen.insert(c->second.path);
    std::vector<IsoFile> dropped,kept;
    DropDuplicates(isos,dropped,kept,chosen);
    UniqueNames(isos);
    LinkInstallDiscs(isos);
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
    explicit Console(DWORD at) : address(at), push(FromXbox(at)), seenAt(-1), connections(0), sent(0), ftp(0), synced(0), counted(0), hiddenAt(-2),
                                 inAurora(false), sentInAurora(0), probeAt(0) {
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
            if(c.connections<=0 || c.inAurora || Now()*1000<c.probeAt) continue;
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
    Connection(SOCKET socket, DWORD address) : socket_(socket), address_(address), console_(ConsoleAt(address)), pastEnd_(false) {
        AddressText(address,peer_,sizeof(peer_));
        quiet_=Loopback(address) && !Settings.verbose;  // The push's own connection.
        if(console_) ++console_->connections;
    }
    ~Connection() {
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
            if(console_) console_->seenAt=(long long)(Now()*1000);
            if(!Handle(command,index,offset,length)) break;
        }
        if(!quiet_) Log("%s disconnected.",peer_);
    }
private:
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
            if(!index || index>listing_.size()) listing_=Isos();
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
        std::vector<IsoFile> isos=Isos();
        const IsoFile* found=NULL;
        for(size_t i=0;i<isos.size() && !found && !name.empty();++i) if(!_stricmp(isos[i].name.c_str(),name.c_str())) found=&isos[i];
        // netiso-srv takes the first ISO whose name ends with the request.
        for(size_t i=0;i<isos.size() && !found && !name.empty();++i)
            if(isos[i].name.size()>=name.size() && !_stricmp(isos[i].name.c_str()+isos[i].name.size()-name.size(),name.c_str())) found=&isos[i];
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

void Serve(SOCKET listener) {
    for(;;) {
        sockaddr_in from; AddressLength size=sizeof(from);
        SOCKET client=accept(listener,reinterpret_cast<sockaddr*>(&from),&size);
        if(client==INVALID_SOCKET) { Sleep(100); continue; }
        int on=1;
        setsockopt(client,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&on),sizeof(on));
        setsockopt(client,SOL_SOCKET,SO_KEEPALIVE,reinterpret_cast<const char*>(&on),sizeof(on));
        DWORD address=from.sin_addr.s_addr;
        bool added;
        Console* console=ConsoleAt(address,&added);
        if(console) console->seenAt=(long long)(Now()*1000);
        if(added) {
            if(Current().push && console->push) Log("Found an Xbox at %s; its game folders are kept up to date.",console->text);
            else Log("Found an Xbox at %s.",console->text);
            XboxEvent(*console,EventXbox,"Connected for the first time",std::string("New console at ")+console->text);
        }
        std::thread([client,address]() { Connection(client,address).Run(); }).detach();
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
const char FilterNetIso[] =
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

std::string Lines(const char* text, bool crlf) {
    std::string out;
    for(const char* p=text;*p;++p) { if(*p=='\n' && crlf) out+='\r'; out+=*p; }
    return out;
}

void GuardFilter(const Console& console, FtpClient& ftp, bool announce, const Options& o) {
    std::string path=o.aurora+FilterScript;
    std::vector<unsigned char> data;
    if(!ftp.Retrieve(path.c_str(),data,1<<20)) {
        if(announce) XboxLog(console,"couldn't read Aurora's %s (%s); the Xbox 360 category is left alone. --aurora names Aurora's folder.",path.c_str(),ftp.Error());
        return;
    }
    std::string text(data.begin(),data.end());
    if(text.find("GameListFilterNetIso")!=std::string::npos) {
        if(announce) XboxLog(console,"Aurora's Xbox 360 category includes the NetISO games.");
        return;
    }
    bool crlf=text.find("\r\n")!=std::string::npos;
    std::string original=Lines(FilterOriginal,crlf);
    size_t at=text.find(original);
    if(at==std::string::npos) {
        if(announce) XboxLog(console,"Aurora's Xbox 360 filter isn't the one this program knows, so it is left alone; the NetISO games show under All.");
        return;
    }
    std::string edited=text.substr(0,at)+Lines(FilterNetIso,crlf)+text.substr(at+original.size());
    std::string backup=path+".before-netiso";
    if(!ftp.Store(backup.c_str(),text.data(),text.size()) || !ftp.Store(path.c_str(),edited.data(),edited.size())) {
        XboxLog(console,"couldn't update Aurora's Xbox 360 category: %s",ftp.Error());
        XboxEvent(console,EventWarning,"Couldn't add the games to Aurora's Xbox 360 category");
        return;
    }
    XboxLog(console,"added the NetISO games to Aurora's Xbox 360 category (%s; the original is kept as %s). Restart Aurora to see it.",path.c_str(),backup.c_str());
    XboxEvent(console,EventGood,"Games added to Aurora's Xbox 360 category; restart Aurora to see them there",
              std::string("Games added to Aurora's Xbox 360 category on ")+console.text+"; restart Aurora to see them there");
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
                Boot.empty() ? NULL : Boot.data(),DWORD(Boot.size()),PushNote,result,removeGone,InstallFor);
    }
    if(ok) XboxLog(console,"game folders up to date: %d added, %d removed, %d kept (%d updated), %d skipped.",result.added,result.removed,result.kept,result.updated,result.skipped);
    if(ok && result.held) XboxLog(console,"%d game folder(s) have no game here yet; they're removed if that's still so %.0f s after the start or the last change of game folders.",result.held,o.removeWait);
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
        if(Push(console,ftp,result,o.pushOnce || Now()-holdFrom>=o.removeWait,o)) {
            s.everPushed=true; s.pushed=state;
            console.synced=(long long)time(NULL);
            s.retryAt=result.held ? holdFrom+o.removeWait : result.skipped ? Now()+RetrySkippedSeconds : 0;
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
    for(int tick=0;;++tick) {
        std::vector<IsoFile> isos=Isos();
        std::string state=Fingerprint(isos);
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
        Options o=Current();
        char generation[16]; _snprintf_s(generation,sizeof(generation),_TRUNCATE,"#%d",PushGeneration.load());
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
            if(o.pushOnce && !states.empty() && finished) exit(failed ? 1 : 0);
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
bool Save() {
    Options o;
    std::vector<KnownConsole> known;
    { std::lock_guard<std::mutex> hold(SettingsLock); o=Settings; known=Known; }
    if(o.configPath.empty()) return false;
    std::lock_guard<std::mutex> hold(SaveLock);
    config::File f;
    f.Load(o.configPath);
    long long before=f.Number("folders",0);
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
    std::map<std::string,Choice> choices;
    { std::lock_guard<std::mutex> hold(DuplicatesLock); choices=Choices; }
    before=f.Number("choices",0);
    f.Set("choices",Number((long long)choices.size()));
    long long n=0;
    for(std::map<std::string,Choice>::const_iterator c=choices.begin();c!=choices.end();++c) {
        std::string copies;
        for(std::set<std::string>::const_iterator k=c->second.copies.begin();k!=c->second.copies.end();++k) copies+=(copies.empty() ? "" : ",")+*k;
        f.Set(("choice"+Number(++n)).c_str(),c->first+"|"+copies+"|"+config::ToUtf8(c->second.path));
    }
    for(long long i=n+1;i<=before;++i) f.Remove(("choice"+Number(i)).c_str());
    before=f.Number("consoles",0);
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

bool ParseArguments(const std::vector<std::string>& args, Options& o);

// The settings file's values over the command line's. The window kept extra
// command-line options in "options"; they come first, so the other keys win.
bool LoadConfig(Options& o) {
    config::File f;
    if(!f.Load(o.configPath)) return false;
    if(!f.Get("options").empty() && !ParseArguments(SplitCommandLine(config::FromUtf8(f.Get("options"))),o))
        Log("The settings file's options (%s) aren't all understood; see netdisc -h.",config::FromUtf8(f.Get("options")).c_str());
    if(f.Has("folders")) {
        o.folders.clear();
        for(long long i=1,n=f.Number("folders",0);i<=n;++i) {
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
    web::SetPasswordHash(f.Get("web_password"));
    // Which copy of a game to play: "key|copy ids|path" (see Choice).
    {
        std::lock_guard<std::mutex> hold(DuplicatesLock);
        for(long long i=1,n=f.Number("choices",0);i<=n;++i) {
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
            Choices[value.substr(0,bar)]=choice;
        }
    }
    for(long long i=1,n=f.Number("consoles",0);i<=n;++i) {
        std::string value=f.Get(("console"+Number(i)).c_str());
        size_t bar=value.find('|');
        if(bar==std::string::npos || !bar) continue;
        KnownConsole c = {value.substr(0,bar), atoll(value.c_str()+bar+1)};
        Known.push_back(c);
    }
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

void State(web::Reply& out) {
    Options o;
    std::vector<KnownConsole> known;
    { std::lock_guard<std::mutex> hold(SettingsLock); o=Settings; known=Known; }
    web::Json j;
    j.Open('{').Set("name","NetDisc 360").Set("built",__DATE__ " " __TIME__);
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
#ifdef _WIN32
    if(InTray) j.Set("startWithWindows",StartsWithWindows());
#endif
    j.Close('}');

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
        j.Set("playing",false).Set("inAurora",false);
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
    if(folders) FoldersChangedAt=(long long)(Now()*1000);
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
    {
        std::lock_guard<std::mutex> hold(SettingsLock);
        Settings.folders=next.folders; Settings.xboxes=next.xboxes; Settings.ftpUser=next.ftpUser; Settings.ftpPassword=next.ftpPassword;
        Settings.xboxFolder=next.xboxFolder; Settings.aurora=next.aurora; Settings.push=next.push; Settings.filter=next.filter;
        Settings.scan=next.scan; Settings.setupDone=next.setupDone; Settings.webLan=next.webLan;
    }
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
        // Shown as chosen right away; the served copy changes at the next look at the folders.
        c->chosen=true; c->open=false;
        std::lock_guard<std::mutex> games(GamesListLock);
        ++GamesVersion;
    }
    Log("%s: the copy at %s is played from now on (chosen on the dashboard).",name.c_str(),path.c_str());
    Event(EventInfo,"%s: playing the %s copy from now on",name.c_str(),kind.c_str());
    Save();
    return true;
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
        "NetDisc 360, a NetISO server for Aurora. Serves the games in folders and their\n"
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
        "  --port <n>              NetISO port (4323)   --ftp-port <n>  (21)\n"
        "  --web-port <n>          the dashboard's port (8360); 0: no dashboard\n"
        "  --web-bind <address>    where the dashboard listens: 127.0.0.1, this computer only\n"
        "                          (default), or 0.0.0.0, the network too. A password for it\n"
        "                          is set on the dashboard or with NETDISC_WEB_PASSWORD.\n"
        "  -v                      log every request\n"
        "  -r, -b                  accepted for netiso-srv's command line; subfolders are always searched\n");
}

bool ParseArguments(const std::vector<std::string>& args, Options& o) {
    for(size_t i=0;i<args.size();++i) {
        const std::string& a=args[i];
        bool hasValue=i+1<args.size();
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
        else if(hasValue && a=="--config") o.configPath=config::ToUtf8(args[++i]);
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

// Runs the server; returns only when it can't (see app.h).
int RunServer(const std::vector<std::string>& args, const std::string& configPath, bool tray) {
    InTray=tray;
    StartedAt=Now();
    // The Docker image passes the FTP password here, so it doesn't show in
    // the process list; --ftp-password still wins.
    if(const char* password=getenv("NETDISC_FTP_PASSWORD")) Settings.ftpPassword=password;
    Options o=Settings;
    if(!ParseArguments(args,o)) return 2;
    if(!configPath.empty()) o.configPath=configPath;
    bool fresh=!o.configPath.empty() && !LoadConfig(o);
    if(o.folders.empty() && o.configPath.empty()) o.folders.push_back(".");
    if(const char* password=getenv("NETDISC_WEB_PASSWORD")) {
        if(*password) { web::SetPasswordHash(web::HashPassword(password)); PasswordFixed=true; }
    }
#ifdef _WIN32
    SetConsoleTitleA("NetDisc 360");
    WSADATA wsa;
    if(WSAStartup(MAKEWORD(2,2),&wsa)) { fprintf(stderr,"Winsock didn't start.\n"); return 1; }
#endif
    Log("NetDisc 360, built %s %s.",__DATE__,__TIME__);
    if(!o.configPath.empty()) Log("Settings: %s",config::FromUtf8(o.configPath).c_str());

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
    { std::lock_guard<std::mutex> hold(SettingsLock); Settings=o; }
    if(fresh) Save();

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
    std::thread(SizeLoop).detach();
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
