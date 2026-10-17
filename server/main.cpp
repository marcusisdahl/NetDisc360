// SPDX-License-Identifier: MIT
// NetDisc 360, a NetISO server for Aurora: one program in place of
// netiso-srv. It serves the games in folders (and their subfolders) to the
// NetISO plugin, noticing new ones without a restart, and keeps the console's
// Hdd1:\NetISO game folders in step with them over Aurora's FTP server, with
// NetISO Sync's own code (shared/library.cpp). The launcher XEXs it writes are
// built into it.
//
//   netdisc [options] [game folder ...]      (netdisc -h lists them)
#include "../shared/library.h"
#include "discs.h"
#include "ftp.h"
#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
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
typedef int AddressLength;
#else
typedef socklen_t AddressLength;
#endif

using namespace nia;

namespace {
enum { Ping = 0, GetIsoSize = 1, HasType1File = 2, ReadData = 3, GetIsoName = 4, MountIso = 5, Header = 20,
       MaxReadBytes = 16 << 20, MaxNameBytes = 4096, MaxDepth = 8 };
// An ISO counts as settled, and gets pushed, when nothing about it changed for this long.
const double SettleSeconds = 10, ProbeDownSeconds = 15, RetrySkippedSeconds = 600, ScanRetrySeconds = 30;
// A console that connected once (no --xbox) but not for this long is probed only every IdleProbeSeconds.
const double IdleSeconds = 86400, IdleProbeSeconds = 600;
const int ScanTries = 20;

struct Options {
    std::vector<std::string> folders;
    std::string ftpUser, ftpPassword, xboxFolder, aurora, launcherPath, bootPath;
    int port, ftpPort;
    std::vector<DWORD> xboxes;  // --xbox: the consoles to update; none: every console that connects.
    bool push, filter, scan, verbose, pushOnce, stats;
    // After the Xbox's FTP server comes back, the longest wait for the Xbox
    // to show it's online before game folders are written (see PushLoop).
    double onlineWait, removeWait;
    Options() : ftpUser("xboxftp"), ftpPassword("xboxftp"), xboxFolder("/Hdd1/NetISO"), aurora("/Hdd1/Apps/Aurora"),
                port(IsvrPort), ftpPort(21), push(true), filter(true), scan(true), verbose(false), pushOnce(false), stats(false),
                onlineWait(120), removeWait(60) {}
};
Options Settings;
std::vector<unsigned char> Launcher, Boot;

std::mutex LogLock;
void Log(const char* format, ...) {
    char text[2048];
    va_list a; va_start(a,format); _vsnprintf_s(text,sizeof(text),_TRUNCATE,format,a); va_end(a);
    time_t now=time(NULL); struct tm local;
#ifdef _WIN32
    localtime_s(&local,&now);
#else
    localtime_r(&now,&local);
#endif
    std::lock_guard<std::mutex> hold(LogLock);
    printf("%02d:%02d:%02d %s\n",local.tm_hour,local.tm_min,local.tm_sec,text);
    fflush(stdout);
}

// Files ----------------------------------------------------------------------

// The discs (ISOs, extracted games, GOD packages) are found and read in discs.cpp.
std::mutex DuplicatesLock;
std::string DuplicatesReported;

// Where a disc is, as the user would remove it: a GOD package by its Title ID folder.
std::string Location(const IsoFile& disc) {
    if(disc.kind!=DiscGod) return disc.path;
    std::string path=disc.path;
    for(int i=0;i<2;++i) { size_t at=path.find_last_of("/\\"); if(at!=std::string::npos) path.erase(at); }
    return path+" (GOD package)";
}

// Everything in the folders, each game once. A game that is there more than
// once is reported (once, until that changes) so the copies can be removed.
std::vector<IsoFile> Isos() {
    std::vector<IsoFile> isos;
    for(size_t i=0;i<Settings.folders.size();++i) FindIsos(Settings.folders[i],isos);
    ReadTitles(isos);
    std::vector<IsoFile> dropped,kept;
    DropDuplicates(isos,dropped,kept);
    UniqueNames(isos);
    std::string report;
    for(size_t i=0;i<dropped.size();++i) {
        char line[96]; _snprintf_s(line,sizeof(line),_TRUNCATE,"Title ID %08X, disc %d",dropped[i].titleId,dropped[i].disc);
        report+="  "+Location(dropped[i])+"\n    is the same game as "+Location(kept[i])+" ("+line+"); only that one is served.\n";
    }
    std::lock_guard<std::mutex> hold(DuplicatesLock);
    if(report!=DuplicatesReported) {
        DuplicatesReported=report;
        if(!report.empty()) {
            report.erase(report.size()-1);
            Log("%d game(s) are here more than once. Remove the extra copies:\n%s",int(dropped.size()),report.c_str());
        }
    }
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
    if(Settings.xboxes.empty()) return !Loopback(address);
    return std::find(Settings.xboxes.begin(),Settings.xboxes.end(),address)!=Settings.xboxes.end();
}

// A console that uses this server: one given with --xbox, or any that
// connects. Several can play at once, each with its own disc; each gets its
// game folders kept up to date (PushLoop) and its numbers shown in the window
// (--stats).
struct Console {
    DWORD address;
    char text[16];
    bool push;                         // FromXbox.
    // When it last connected or sent a NetISO request (its plugin or a
    // launcher), in Now() milliseconds. Those connections start on the Xbox,
    // so they show its own network is up (see PushLoop).
    std::atomic<long long> seenAt;
    std::atomic<int> connections;
    std::atomic<U64> sent;             // Bytes of game data sent to it.
    std::atomic<int> ftp;              // Its Aurora's FTP server: 0 not tried, 1 not answering, 2 answers.
    std::atomic<long long> synced;     // When its game folders were last brought up to date (time()).
    std::string mounted;               // Under ConsoleLock.
    explicit Console(DWORD at) : address(at), push(FromXbox(at)), seenAt(-1), connections(0), sent(0), ftp(0), synced(0) {
        AddressText(at,text,sizeof(text));
    }
};
std::mutex ConsoleLock;
std::vector<Console*> Consoles;  // Never removed, so a Console* stays valid.

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

// A line about one console: "Xbox 192.168.1.30: ...".
void XboxLog(const Console& console, const char* format, ...) {
    char text[2048];
    va_list a; va_start(a,format); _vsnprintf_s(text,sizeof(text),_TRUNCATE,format,a); va_end(a);
    Log("Xbox %s: %s",console.text,text);
}

// With --stats, one line per console each second that something changed (and
// every 5 s anyway), for the window's dashboard. Not part of the log:
//   @stats <address> at=<ms> connections=<n> sent=<bytes> ftp=<0|1|2> synced=<time> mounted=<name>
void StatsLoop() {
    std::vector<std::string> last;
    for(int tick=0;;++tick) {
        std::vector<Console*> consoles=AllConsoles();
        last.resize(consoles.size());
        for(size_t i=0;i<consoles.size();++i) {
            Console& c=*consoles[i];
            std::string mounted;
            { std::lock_guard<std::mutex> hold(ConsoleLock); mounted=c.mounted; }
            char line[512];
            _snprintf_s(line,sizeof(line),_TRUNCATE,"%s connections=%d sent=%llu ftp=%d synced=%lld mounted=%s",c.text,c.connections.load(),
                        (unsigned long long)c.sent.load(),c.ftp.load(),c.synced.load(),mounted.c_str());
            if(line==last[i] && tick%5) continue;
            last[i]=line;
            std::lock_guard<std::mutex> hold(LogLock);
            printf("@stats %s at=%lld %s\n",c.text,(long long)(Now()*1000),line+strlen(c.text)+1);
            fflush(stdout);
        }
        Sleep(1000);
    }
}

// One client connection, speaking the protocol as tuxuser/netiso-srv does
// (src/main.rs), except that every request and reply is read and sent whole,
// the ISO list is read again whenever a client starts listing (index 0), so
// new ISOs show up without a restart, and an empty mount name mounts nothing.
class Connection {
public:
    Connection(SOCKET socket, DWORD address) : socket_(socket), address_(address), console_(ConsoleAt(address)) {
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
        case ReadData:
            // Like netiso-srv: nothing mounted gets no reply, and a read
            // outside the ISO ends the connection.
            if(!iso_.IsOpen()) return true;
            if(length>MaxReadBytes || offset>iso_.Size() || length>iso_.Size()-offset) {
                Log("%s asked for bytes %llu-%llu of a %llu-byte ISO; closing.",peer_,(unsigned long long)offset,(unsigned long long)(offset+length),(unsigned long long)iso_.Size());
                return false;
            }
            buffer_.resize(length);
            if(length && !iso_.Read(offset,buffer_.data(),length)) { Log("Reading %s failed; closing %s.",mounted_.c_str(),peer_); return false; }
            if(!SendAll(buffer_.data(),length)) return false;
            if(console_) console_->sent+=length;
            return true;
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
        mounted_=found->name;
        Mounted();
        if(!quiet_) {
            if(found->kind==DiscIso) Log("%s: mounted %s.",peer_,found->name.c_str());
            else Log("%s: mounted %s (%s).",peer_,found->name.c_str(),iso_.Note().c_str());
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
        if(added && Settings.push && console->push) Log("Found an Xbox at %s; its game folders are kept up to date.",console->text);
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

void GuardFilter(const Console& console, FtpClient& ftp, bool announce) {
    std::string path=Settings.aurora+FilterScript;
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
        return;
    }
    XboxLog(console,"added the NetISO games to Aurora's Xbox 360 category (%s; the original is kept as %s). Restart Aurora to see it.",path.c_str(),backup.c_str());
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
void RefreshSyncApp(const Console& console, FtpClient& ftp, const std::vector<FtpEntry>& entries) {
    for(size_t i=0;i<entries.size();++i) {
        if(!entries[i].folder || _stricmp(entries[i].name.c_str(),SyncFolder)) continue;
        std::string folder=Settings.xboxFolder+"/"+entries[i].name;
        int launcher=RefreshRemote(ftp,folder+"/default.xex",Launcher);
        int boot=Boot.empty() ? 0 : RefreshRemote(ftp,folder+"/boot.xex",Boot);
        if(launcher<0 || boot<0) XboxLog(console,"couldn't update %s: %s",folder.c_str(),ftp.Error());
        else if(launcher || boot) XboxLog(console,"updated %s to this build.",folder.c_str());
        return;
    }
}

bool Push(const Console& console, FtpClient& ftp, SyncResult& result, bool removeGone) {
    std::vector<FtpEntry> entries;
    ftp.MakeFolder(Settings.xboxFolder.c_str());
    if(!ftp.List(Settings.xboxFolder.c_str(),entries)) { XboxLog(console,"couldn't list %s: %s",Settings.xboxFolder.c_str(),ftp.Error()); return false; }
    RefreshSyncApp(console,ftp,entries);
    IsvrClient client(30);
    if(!client.Connect(htonl(0x7F000001),Settings.port,5) || !client.Ping()) { Log("Couldn't reach this server itself: %s",client.Error()); return false; }
    FtpStorage storage(ftp);
    bool ok;
    {
        std::lock_guard<std::mutex> hold(SyncLock);
        Pushing=&console;
        ok=Sync(client,storage,Settings.xboxFolder.c_str(),Launcher.data(),DWORD(Launcher.size()),
                Boot.empty() ? NULL : Boot.data(),DWORD(Boot.size()),PushNote,result,removeGone);
    }
    if(ok) XboxLog(console,"game folders up to date: %d added, %d removed, %d kept (%d updated), %d skipped.",result.added,result.removed,result.kept,result.updated,result.skipped);
    if(ok && result.held) XboxLog(console,"%d game folder(s) have no game here yet; they're removed if that's still so %.0f s after the start.",result.held,Settings.removeWait);
    else if(!ok) XboxLog(console,"updating the game folders stopped: %s",ftp.Error()[0] ? ftp.Error() : "see above");
    return ok;
}

// Asks Aurora to scan the game folder now, with FtpDll's SITE INITIATESCAN,
// so new games show up at once and Aurora looks them up online. A game its own
// scan finds while Aurora is starting never gets its name or art (see
// PushLoop). False when Aurora didn't start one (its scanner may be busy).
bool StartScan(const Console& console, FtpClient& ftp) {
    std::string reply;
    bool ok=ftp.Site(("INITIATESCAN "+Settings.xboxFolder).c_str(),reply) && reply.find("Unable")==std::string::npos;
    if(ok) XboxLog(console,"asked Aurora to scan %s, so it lists the changes now and downloads the new games' art.",Settings.xboxFolder.c_str());
    else XboxLog(console,"Aurora didn't start a scan: %s",reply.empty() ? ftp.Error() : reply.c_str());
    return ok;
}

// What PushLoop knows about one console.
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
    bool pushDue=settled && (!s.everPushed || state!=s.pushed || (s.retryAt && Now()>=s.retryAt));
    bool scanDue=s.scanTries && Now()>=s.scanAt;
    bool online=Settings.pushOnce || !s.downSince || console.seenAt.load()/1000.0>=s.downSince || Now()-s.upSince>=Settings.onlineWait;
    if(!(pushDue || scanDue) || Now()<s.nextProbe || !(!s.reachable || online || scanDue)) return;
    bool idle=Settings.xboxes.empty() && Now()-console.seenAt.load()/1000.0>=IdleSeconds;
    if(!s.reachable && idle && Now()<s.probedAt+IdleProbeSeconds) return;
    s.probedAt=Now();
    FtpClient ftp(console.address,Settings.ftpPort,Settings.ftpUser.c_str(),Settings.ftpPassword.c_str(),15);
    if(!ftp.Open()) {
        if(s.reachable || !s.toldDown) XboxLog(console,"waiting for its FTP server (Aurora): %s",ftp.Error());
        if(s.reachable || !s.downSince) s.downSince=Now();
        s.reachable=false; s.toldDown=true; s.toldWaiting=false; s.nextProbe=Now()+ProbeDownSeconds;
        console.ftp=1;
        s.finished=Settings.pushOnce;
        return;
    }
    bool back=!s.reachable;
    s.reachable=true; s.toldDown=false;
    console.ftp=2;
    if(back) {
        XboxLog(console,"its FTP server answers.");
        s.upSince=Now();
        online=Settings.pushOnce || !s.downSince || console.seenAt.load()/1000.0>=s.downSince || Settings.onlineWait<=0;
    }
    if(Settings.filter && (back || pushDue)) GuardFilter(console,ftp,back);
    if(pushDue && !online && !s.toldWaiting) {
        XboxLog(console,"waiting for it to come online before writing game folders (its NetISO plugin connects to this server; at most %.0f s).",Settings.onlineWait);
        s.toldWaiting=true;
    }
    if(pushDue && online) {
        SyncResult result;
        s.attempted=true; s.toldWaiting=false;
        if(Push(console,ftp,result,Settings.pushOnce || Now()-startedAt>=Settings.removeWait)) {
            s.everPushed=true; s.pushed=state;
            console.synced=(long long)time(NULL);
            s.retryAt=result.held ? startedAt+Settings.removeWait : result.skipped ? Now()+RetrySkippedSeconds : 0;
            if(result.added || result.removed) {
                if(Settings.scan) { s.scanTries=Settings.pushOnce ? 1 : ScanTries; s.scanAt=Now(); scanDue=true; }
                else XboxLog(console,"Aurora shows the changes after its next scan of %s.",Settings.xboxFolder.c_str());
            }
        } else s.nextProbe=Now()+30;
    }
    if(scanDue) {
        if(StartScan(console,ftp)) s.scanTries=0;
        else if(--s.scanTries) s.scanAt=Now()+ScanRetrySeconds;
        else XboxLog(console,"Aurora shows the changes after its next scan of %s.",Settings.xboxFolder.c_str());
    }
    if(Settings.pushOnce && s.attempted && !s.scanTries) s.finished=true;
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
        PushTo(*console,*s,state,settled,startedAt);
        if(!s->finished) Sleep(1000);
    }
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
// the start: the window restarts the server for each folder added in it, and
// a games drive can come up after a server started with Windows. Removing
// them early would drop the games from Aurora, with their art and stats.
void PushLoop() {
    std::string previous;
    const double startedAt=Now();
    double changedAt=-1e9;
    std::map<DWORD,PushState> states;
    for(;;) {
        std::string state=Fingerprint(Isos());
        if(state!=previous) { if(!previous.empty() || changedAt>-1e9) changedAt=Now(); previous=state; }
        { std::lock_guard<std::mutex> hold(GamesLock); GamesState=state; GamesSettled=Now()-changedAt>=SettleSeconds; }
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
        if(Settings.pushOnce && !states.empty() && finished) exit(failed ? 1 : 0);
        Sleep(1000);
    }
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
        "step with them.\n"
        "Games can be ISOs, extracted to a folder (default.xex at the top; a \"Disc 2\"\n"
        "folder inside is a disc of its own), or Games on Demand packages. Each game is\n"
        "served once; extra copies are listed so they can be removed.\n\n"
        "  netdisc [options] [folder ...]   (default: the current folder)\n\n"
        "  --xbox <address>        a console to update; give it once for each (default: every\n"
        "                          console that connects)\n"
        "  --ftp-user <name>       Aurora's FTP login (xboxftp)\n"
        "  --ftp-password <text>   (xboxftp)\n"
        "  --xbox-folder <path>    the game folder on the Xbox, as an FTP path (/Hdd1/NetISO)\n"
        "  --aurora <path>         Aurora's folder on the Xbox, as an FTP path (/Hdd1/Apps/Aurora)\n"
        "  --no-push               only serve ISOs; leave the Xbox alone\n"
        "  --no-category           leave Aurora's Xbox 360 category alone\n"
        "  --no-scan               don't ask Aurora to scan after adding or removing games\n"
        "  --online-wait <s>       after the Xbox's FTP comes back, the longest wait for the Xbox\n"
        "                          to come online before writing game folders (120)\n"
        "  --remove-wait <s>       how long after the start game folders whose game is gone\n"
        "                          are kept on the Xbox (60)\n"
        "  --port <n>              NetISO port (4323)   --ftp-port <n>  (21)\n"
        "  -v                      log every request\n"
        "  -r, -b                  accepted for netiso-srv's command line; subfolders are always searched\n");
}

bool ParseArguments(int argc, char** argv) {
    for(int i=1;i<argc;++i) {
        std::string a=argv[i];
        bool hasValue=i+1<argc;
        if(a=="-h" || a=="--help" || a=="/?") { Usage(); exit(0); }
        else if(a=="-r" || a=="-b") {}
        else if(a=="-v") Settings.verbose=true;
        else if(a=="--no-push") Settings.push=false;
        else if(a=="--no-category") Settings.filter=false;
        else if(a=="--no-scan") Settings.scan=false;
        else if(a=="--push-once") Settings.pushOnce=true;  // For the tests.
        else if(a=="--stats") Settings.stats=true;  // For the window.
        else if(hasValue && a=="--xbox") {
            DWORD xbox=inet_addr(argv[++i]);
            if(xbox==INADDR_NONE || !xbox) { fprintf(stderr,"--xbox needs an IPv4 address like 192.168.1.20.\n"); return false; }
            if(std::find(Settings.xboxes.begin(),Settings.xboxes.end(),xbox)==Settings.xboxes.end()) Settings.xboxes.push_back(xbox);
        }
        else if(hasValue && a=="--ftp-user") Settings.ftpUser=argv[++i];
        else if(hasValue && a=="--ftp-password") Settings.ftpPassword=argv[++i];
        else if(hasValue && a=="--xbox-folder") Settings.xboxFolder=argv[++i];
        else if(hasValue && a=="--aurora") Settings.aurora=argv[++i];
        else if(hasValue && a=="--port") Settings.port=atoi(argv[++i]);
        else if(hasValue && a=="--ftp-port") Settings.ftpPort=atoi(argv[++i]);
        else if(hasValue && a=="--launcher") Settings.launcherPath=argv[++i];
        else if(hasValue && a=="--boot") Settings.bootPath=argv[++i];
        else if(hasValue && a=="--online-wait") Settings.onlineWait=atof(argv[++i]);
        else if(hasValue && a=="--remove-wait") Settings.removeWait=atof(argv[++i]);
        else if(a[0]!='-') {
            while(a.size()>1 && (a[a.size()-1]=='/' || a[a.size()-1]=='\\')) a.erase(a.size()-1);
            Settings.folders.push_back(a);
        }
        else { fprintf(stderr,"Unknown option %s (-h lists them).\n",a.c_str()); return false; }
    }
    while(Settings.xboxFolder.size()>1 && Settings.xboxFolder[Settings.xboxFolder.size()-1]=='/') Settings.xboxFolder.erase(Settings.xboxFolder.size()-1);
    if(Settings.folders.empty()) Settings.folders.push_back(".");
    return Settings.port>0 && Settings.port<65536 && Settings.ftpPort>0 && Settings.ftpPort<65536;
}

int Run(int argc, char** argv) {
    // The window (gui.cpp) and the Docker image pass the FTP password here,
    // so it doesn't show in the process list; --ftp-password still wins.
    if(const char* password=getenv("NETDISC_FTP_PASSWORD")) Settings.ftpPassword=password;
    if(!ParseArguments(argc,argv)) return 2;
#ifdef _WIN32
    SetConsoleTitleA("NetDisc 360");
    WSADATA wsa;
    if(WSAStartup(MAKEWORD(2,2),&wsa)) { fprintf(stderr,"Winsock didn't start.\n"); return 1; }
#endif
    Log("NetDisc 360, built %s %s.",__DATE__,__TIME__);
    std::string where;
    for(size_t i=0;i<Settings.folders.size();++i) where+=(i ? ", " : "")+Settings.folders[i];
    std::vector<IsoFile> isos=Isos();
    Log("%d game(s) in %s and subfolders:",int(isos.size()),where.c_str());
    for(size_t i=0;i<isos.size();++i) Log("  %s",isos[i].name.c_str());
    if(isos.empty()) Log("New games put there are served without a restart.");

    if(Settings.push) {
        bool loaded=Settings.launcherPath.empty() ? BuiltIn(IDR_LAUNCHER,Launcher) : LoadFile(Settings.launcherPath,Launcher);
        if(!Settings.launcherPath.empty() && !loaded) { fprintf(stderr,"Can't read %s.\n",Settings.launcherPath.c_str()); return 2; }
        // boot.xex is optional: without it, each launcher mounts by itself.
        if(Settings.bootPath.empty()) BuiltIn(IDR_BOOT,Boot);
        else if(!LoadFile(Settings.bootPath,Boot)) { fprintf(stderr,"Can't read %s.\n",Settings.bootPath.c_str()); return 2; }
        if(!loaded) {
            Log("This build has no launcher built in, so it won't update the Xbox (--launcher gives one).");
            Settings.push=false;
        } else if(!ExecutionInfoOffset(Launcher.data(),DWORD(Launcher.size()))) {
            fprintf(stderr,"The launcher XEX has no execution info header.\n"); return 2;
        }
    }

    SOCKET listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    int on=1;
#ifndef _WIN32
    setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,reinterpret_cast<const char*>(&on),sizeof(on));
#endif
    sockaddr_in at; memset(&at,0,sizeof(at)); at.sin_family=AF_INET; at.sin_port=htons(u_short(Settings.port));
    at.sin_addr.s_addr=htonl(Settings.pushOnce ? 0x7F000001 : 0);
    if(listener==INVALID_SOCKET || bind(listener,reinterpret_cast<sockaddr*>(&at),sizeof(at)) || listen(listener,16)) {
        Log("Can't listen on port %d (error %d). Is netiso-srv or another copy of this server still running?",Settings.port,WSAGetLastError());
        return 1;
    }
    (void)on;
    Log("Listening on port %d.",Settings.port);
    for(size_t i=0;i<Settings.xboxes.size();++i) {
        Console* console=ConsoleAt(Settings.xboxes[i]);
        if(console && Settings.push) Log("Keeping %s on the Xbox at %s up to date.",Settings.xboxFolder.c_str(),console->text);
    }
    if(Settings.push) {
        if(Settings.xboxes.empty()) Log("Keeping %s up to date on every Xbox that connects (or give --xbox).",Settings.xboxFolder.c_str());
        std::thread(PushLoop).detach();
    }
    if(Settings.stats) std::thread(StatsLoop).detach();
    Serve(listener);
    return 0;
}
}

#ifdef _WIN32
int GuiMain(int argc, char** argv);  // gui.cpp: the window when started without options.
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    int window=GuiMain(argc,argv);
    if(window>=0) return window;
#endif
    int code=Run(argc,argv);
#ifdef _WIN32
    // Started from Explorer: keep the window open to show why it stopped.
    DWORD processes[2];
    if(code && GetConsoleProcessList(processes,2)==1) { printf("Press Enter to close."); getchar(); }
#endif
    return code;
}
