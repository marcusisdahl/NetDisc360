// SPDX-License-Identifier: MIT
// NetISO server for Aurora: one program in place of netiso-srv. It serves the
// ISOs in a folder (and its subfolders) to the NetISO plugin, noticing new
// ones without a restart, and keeps the console's Hdd1:\NetISO game folders
// in step with them over Aurora's FTP server, with NetISO Sync's own code
// (shared/library.cpp). The launcher XEXs it writes are built into it.
//
//   netiso-server [options] [ISO folder]      (netiso-server -h lists them)
#include "../shared/library.h"
#include "ftp.h"
#include <algorithm>
#include <atomic>
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
#include <dirent.h>
typedef socklen_t AddressLength;
#endif

using namespace nia;

namespace {
enum { Ping = 0, GetIsoSize = 1, HasType1File = 2, ReadData = 3, GetIsoName = 4, MountIso = 5, Header = 20,
       MaxReadBytes = 16 << 20, MaxNameBytes = 4096, MaxDepth = 8 };
// An ISO counts as settled, and gets pushed, when nothing about it changed for this long.
const double SettleSeconds = 10, ProbeDownSeconds = 15, ProbeUpSeconds = 60, RetrySkippedSeconds = 600, FilterCheckSeconds = 600,
             ScanRetrySeconds = 30;
const int ScanTries = 20;

struct Options {
    std::string folder, ftpUser, ftpPassword, xboxFolder, aurora, launcherPath, bootPath;
    int port, ftpPort;
    DWORD xbox;
    bool push, filter, scan, verbose, pushOnce;
    Options() : folder("."), ftpUser("xboxftp"), ftpPassword("xboxftp"), xboxFolder("/Hdd1/NetISO"), aurora("/Hdd1/Apps/Aurora"),
                port(IsvrPort), ftpPort(21), xbox(0), push(true), filter(true), scan(true), verbose(false), pushOnce(false) {}
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

struct IsoFile { std::string path, name; U64 size; long long modified; };

bool IsIsoName(const char* name) {
    size_t length=strlen(name);
    return length>4 && !_stricmp(name+length-4,".iso");
}

void FindIsos(const std::string& folder, std::vector<IsoFile>& out, int depth=0) {
    if(depth>MaxDepth) return;
#ifdef _WIN32
    WIN32_FIND_DATAA found;
    HANDLE search=FindFirstFileA((folder+"\\*").c_str(),&found);
    if(search==INVALID_HANDLE_VALUE) return;
    do {
        const char* name=found.cFileName;
        if(!strcmp(name,".") || !strcmp(name,"..")) continue;
        std::string path=folder+"\\"+name;
        if(found.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) {
            // Junctions and links could loop.
            if(!(found.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) FindIsos(path,out,depth+1);
        } else if(IsIsoName(name)) {
            IsoFile iso; iso.path=path; iso.name=name;
            iso.size=(U64(found.nFileSizeHigh)<<32)|found.nFileSizeLow;
            iso.modified=(long long)((U64(found.ftLastWriteTime.dwHighDateTime)<<32)|found.ftLastWriteTime.dwLowDateTime);
            out.push_back(iso);
        }
    } while(FindNextFileA(search,&found));
    FindClose(search);
#else
    DIR* dir=opendir(folder.c_str()); if(!dir) return;
    while(struct dirent* e=readdir(dir)) {
        const char* name=e->d_name;
        if(!strcmp(name,".") || !strcmp(name,"..")) continue;
        std::string path=folder+"/"+name;
        struct stat st; if(lstat(path.c_str(),&st)) continue;
        if(S_ISDIR(st.st_mode)) FindIsos(path,out,depth+1);
        else if(S_ISREG(st.st_mode) && IsIsoName(name)) {
            IsoFile iso; iso.path=path; iso.name=name; iso.size=U64(st.st_size); iso.modified=(long long)st.st_mtime;
            out.push_back(iso);
        }
    }
    closedir(dir);
#endif
    if(!depth) std::sort(out.begin(),out.end(),[](const IsoFile& a, const IsoFile& b) {
        int order=_stricmp(a.name.c_str(),b.name.c_str());
        return order ? order<0 : a.path<b.path;
    });
}

std::vector<IsoFile> Isos() { std::vector<IsoFile> isos; FindIsos(Settings.folder,isos); return isos; }

// What the push compares: an ISO that is still being copied keeps changing.
std::string Fingerprint(const std::vector<IsoFile>& isos) {
    std::string text;
    for(size_t i=0;i<isos.size();++i) {
        char line[64]; _snprintf_s(line,sizeof(line),_TRUNCATE,"|%llu|%lld\n",(unsigned long long)isos[i].size,isos[i].modified);
        text+=isos[i].path+line;
    }
    return text;
}

class IsoReader {
public:
    IsoReader() : size_(0) {
#ifdef _WIN32
        file_=INVALID_HANDLE_VALUE;
#else
        file_=-1;
#endif
    }
    ~IsoReader() { Close(); }
    bool Open(const IsoFile& iso) {
        Close();
#ifdef _WIN32
        file_=CreateFileA(iso.path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
        if(file_==INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size;
        if(!GetFileSizeEx(file_,&size)) { Close(); return false; }
        size_=U64(size.QuadPart);
#else
        file_=open(iso.path.c_str(),O_RDONLY);
        if(file_<0) return false;
        struct stat st; if(fstat(file_,&st)) { Close(); return false; }
        size_=U64(st.st_size);
#endif
        return true;
    }
    bool IsOpen() const {
#ifdef _WIN32
        return file_!=INVALID_HANDLE_VALUE;
#else
        return file_>=0;
#endif
    }
    void Close() {
#ifdef _WIN32
        if(file_!=INVALID_HANDLE_VALUE) { CloseHandle(file_); file_=INVALID_HANDLE_VALUE; }
#else
        if(file_>=0) { close(file_); file_=-1; }
#endif
        size_=0;
    }
    U64 Size() const { return size_; }
    bool Read(U64 offset, unsigned char* data, DWORD length) {
        while(length) {
#ifdef _WIN32
            OVERLAPPED at; memset(&at,0,sizeof(at)); at.Offset=DWORD(offset); at.OffsetHigh=DWORD(offset>>32);
            DWORD got=0;
            if(!::ReadFile(file_,data,length,&got,&at) || !got) return false;
#else
            ssize_t got=pread(file_,data,length,off_t(offset));
            if(got<=0) { if(got<0 && errno==EINTR) continue; return false; }
#endif
            data+=got; offset+=U64(got); length-=DWORD(got);
        }
        return true;
    }
private:
#ifdef _WIN32
    HANDLE file_;
#else
    int file_;
#endif
    U64 size_;
};

// The NetISO server ----------------------------------------------------------

std::atomic<DWORD> XboxAddress(0);
bool Loopback(DWORD address) { return (ntohl(address)>>24)==127; }

// One client connection, speaking the protocol as tuxuser/netiso-srv does
// (src/main.rs), except that every request and reply is read and sent whole,
// the ISO list is read again whenever a client starts listing (index 0), so
// new ISOs show up without a restart, and an empty mount name mounts nothing.
class Connection {
public:
    Connection(SOCKET socket, DWORD address) : socket_(socket), address_(address) {
        AddressText(address,peer_,sizeof(peer_));
        quiet_=Loopback(address) && !Settings.verbose;  // The push's own connection.
    }
    ~Connection() { closesocket(socket_); }
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
            return SendAll(buffer_.data(),length);
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
            return 0;
        }
        std::vector<IsoFile> isos=Isos();
        const IsoFile* found=NULL;
        for(size_t i=0;i<isos.size() && !found && !name.empty();++i) if(!_stricmp(isos[i].name.c_str(),name.c_str())) found=&isos[i];
        // netiso-srv takes the first ISO whose name ends with the request.
        for(size_t i=0;i<isos.size() && !found && !name.empty();++i)
            if(isos[i].name.size()>=name.size() && !_stricmp(isos[i].name.c_str()+isos[i].name.size()-name.size(),name.c_str())) found=&isos[i];
        if(!found) { Log("%s asked to mount \"%s\", which isn't here.",peer_,name.c_str()); return 0; }
        if(!iso_.Open(*found)) { Log("%s: couldn't open %s.",peer_,found->path.c_str()); mounted_.clear(); return 0; }
        mounted_=found->name;
        if(!quiet_) Log("%s: mounted %s.",peer_,found->name.c_str());
        return 1;
    }
    SOCKET socket_;
    DWORD address_;
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
        if(!Loopback(address) && !Settings.xbox && XboxAddress.exchange(address)!=address) {
            char where[16]; AddressText(address,where,sizeof(where));
            if(Settings.push) Log("The Xbox is at %s; game folders go there.",where);
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
    "-- NetISO for Aurora: the game launchers in Hdd1:\\NetISO play retail discs\n"
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

void GuardFilter(FtpClient& ftp, bool announce) {
    std::string path=Settings.aurora+FilterScript;
    std::vector<unsigned char> data;
    if(!ftp.Retrieve(path.c_str(),data,1<<20)) {
        if(announce) Log("Couldn't read Aurora's %s (%s); the Xbox 360 category is left alone. --aurora names Aurora's folder.",path.c_str(),ftp.Error());
        return;
    }
    std::string text(data.begin(),data.end());
    if(text.find("GameListFilterNetIso")!=std::string::npos) {
        if(announce) Log("Aurora's Xbox 360 category includes the NetISO games.");
        return;
    }
    bool crlf=text.find("\r\n")!=std::string::npos;
    std::string original=Lines(FilterOriginal,crlf);
    size_t at=text.find(original);
    if(at==std::string::npos) {
        if(announce) Log("Aurora's Xbox 360 filter isn't the one this program knows, so it is left alone; the NetISO games show under All.");
        return;
    }
    std::string edited=text.substr(0,at)+Lines(FilterNetIso,crlf)+text.substr(at+original.size());
    std::string backup=path+".before-netiso";
    if(!ftp.Store(backup.c_str(),text.data(),text.size()) || !ftp.Store(path.c_str(),edited.data(),edited.size())) {
        Log("Couldn't update Aurora's Xbox 360 category: %s",ftp.Error());
        return;
    }
    Log("Added the NetISO games to Aurora's Xbox 360 category (%s; the original is kept as %s). Restart Aurora to see it.",path.c_str(),backup.c_str());
}

// Pushing the game folders ---------------------------------------------------

void PushNote(const char* text) { Log("Xbox: %s",text); }

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
void RefreshSyncApp(FtpClient& ftp, const std::vector<FtpEntry>& entries) {
    for(size_t i=0;i<entries.size();++i) {
        if(!entries[i].folder || _stricmp(entries[i].name.c_str(),SyncFolder)) continue;
        std::string folder=Settings.xboxFolder+"/"+entries[i].name;
        int launcher=RefreshRemote(ftp,folder+"/default.xex",Launcher);
        int boot=Boot.empty() ? 0 : RefreshRemote(ftp,folder+"/boot.xex",Boot);
        if(launcher<0 || boot<0) Log("Couldn't update %s on the Xbox: %s",folder.c_str(),ftp.Error());
        else if(launcher || boot) Log("Updated %s on the Xbox to this build.",folder.c_str());
        return;
    }
}

bool Push(FtpClient& ftp, SyncResult& result) {
    std::vector<FtpEntry> entries;
    ftp.MakeFolder(Settings.xboxFolder.c_str());
    if(!ftp.List(Settings.xboxFolder.c_str(),entries)) { Log("Couldn't list %s on the Xbox: %s",Settings.xboxFolder.c_str(),ftp.Error()); return false; }
    RefreshSyncApp(ftp,entries);
    IsvrClient client(30);
    if(!client.Connect(htonl(0x7F000001),Settings.port,5) || !client.Ping()) { Log("Couldn't reach this server itself: %s",client.Error()); return false; }
    FtpStorage storage(ftp);
    bool ok=Sync(client,storage,Settings.xboxFolder.c_str(),Launcher.data(),DWORD(Launcher.size()),
                 Boot.empty() ? NULL : Boot.data(),DWORD(Boot.size()),PushNote,result);
    if(ok) Log("The Xbox is up to date: %d added, %d removed, %d kept (%d updated), %d skipped.",result.added,result.removed,result.kept,result.updated,result.skipped);
    else Log("Updating the Xbox's game folders stopped: %s",ftp.Error()[0] ? ftp.Error() : "see above");
    return ok;
}

// Asks Aurora to scan the game folder now, with FtpDll's SITE INITIATESCAN,
// so new games show up at once and Aurora looks them up online while the
// network is up. Its own scan at boot runs before the Xbox has a network (40 s
// after boot on the test console), and a game it first finds then never gets
// its name or art. False when Aurora didn't start one (its scanner may be busy).
bool StartScan(FtpClient& ftp) {
    std::string reply;
    bool ok=ftp.Site(("INITIATESCAN "+Settings.xboxFolder).c_str(),reply) && reply.find("Unable")==std::string::npos;
    if(ok) Log("Asked Aurora to scan %s, so it lists the changes now and downloads the new games' art.",Settings.xboxFolder.c_str());
    else Log("Aurora didn't start a scan: %s",reply.empty() ? ftp.Error() : reply.c_str());
    return ok;
}

// Watches the ISO folder and the Xbox. Pushes when the ISOs changed and have
// settled, and when the Xbox's FTP server (Aurora) comes back after being away.
void PushLoop() {
    std::string previous,pushed;
    double changedAt=-1e9,nextProbe=0,retryAt=0,filterAt=0,scanAt=0;
    int scanTries=0;
    bool attempted=false;
    bool everPushed=false,reachable=false,toldDown=false;
    DWORD target=0;
    for(;;) {
        std::string state=Fingerprint(Isos());
        if(state!=previous) { if(!previous.empty() || changedAt>-1e9) changedAt=Now(); previous=state; }
        DWORD xbox=Settings.xbox ? Settings.xbox : XboxAddress.load();
        if(xbox && xbox!=target) { target=xbox; reachable=false; toldDown=false; nextProbe=0; }
        bool settled=Now()-changedAt>=SettleSeconds;
        bool due=reachable && settled && (!everPushed || state!=pushed || (retryAt && Now()>=retryAt));
        if(target && (due || Now()>=nextProbe)) {
            FtpClient ftp(target,Settings.ftpPort,Settings.ftpUser.c_str(),Settings.ftpPassword.c_str(),15);
            if(!ftp.Open()) {
                if(reachable || !toldDown) Log("Waiting for the Xbox's FTP server: %s",ftp.Error());
                reachable=false; toldDown=true; nextProbe=Now()+ProbeDownSeconds;
            } else {
                bool back=!reachable;
                if(back) { char where[16]; AddressText(target,where,sizeof(where)); Log("The Xbox's FTP server at %s answers.",where); }
                reachable=true; toldDown=false; nextProbe=Now()+ProbeUpSeconds;
                if(Settings.filter && (back || Now()>=filterAt)) { GuardFilter(ftp,back); filterAt=Now()+FilterCheckSeconds; }
                if(settled && (!everPushed || state!=pushed || (retryAt && Now()>=retryAt))) {
                    SyncResult result;
                    attempted=true;
                    if(Push(ftp,result)) {
                        everPushed=true; pushed=state;
                        retryAt=result.skipped ? Now()+RetrySkippedSeconds : 0;
                        if(result.added || result.removed) {
                            if(Settings.scan) { scanTries=Settings.pushOnce ? 1 : ScanTries; scanAt=Now(); }
                            else Log("Aurora shows the changes after its next scan of %s.",Settings.xboxFolder.c_str());
                        }
                    } else nextProbe=Now()+30;
                }
                if(scanTries && Now()>=scanAt) {
                    if(StartScan(ftp)) scanTries=0;
                    else if(--scanTries) scanAt=Now()+ScanRetrySeconds;
                    else Log("Aurora shows the changes after its next scan of %s.",Settings.xboxFolder.c_str());
                }
                if(Settings.pushOnce && attempted && !scanTries) exit(everPushed ? 0 : 1);
                ftp.Close();
            }
        }
        if(Settings.pushOnce && !reachable && toldDown) exit(1);
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
        "NetISO server for Aurora. Serves the ISOs in a folder and its subfolders to\n"
        "the NetISO plugin, and keeps the Xbox's game folders for Aurora in step with them.\n\n"
        "  netiso-server [options] [ISO folder]   (default: the current folder)\n\n"
        "  --xbox <address>        the Xbox to update (default: the console that connects)\n"
        "  --ftp-user <name>       Aurora's FTP login (xboxftp)\n"
        "  --ftp-password <text>   (xboxftp)\n"
        "  --xbox-folder <path>    the game folder on the Xbox, as an FTP path (/Hdd1/NetISO)\n"
        "  --aurora <path>         Aurora's folder on the Xbox, as an FTP path (/Hdd1/Apps/Aurora)\n"
        "  --no-push               only serve ISOs; leave the Xbox alone\n"
        "  --no-category           leave Aurora's Xbox 360 category alone\n"
        "  --no-scan               don't ask Aurora to scan after adding or removing games\n"
        "  --port <n>              NetISO port (4323)   --ftp-port <n>  (21)\n"
        "  -v                      log every request\n"
        "  -r, -b                  accepted for netiso-srv's command line; subfolders are always searched\n");
}

bool ParseArguments(int argc, char** argv) {
    bool folderGiven=false;
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
        else if(hasValue && a=="--xbox") {
            Settings.xbox=inet_addr(argv[++i]);
            if(Settings.xbox==INADDR_NONE || !Settings.xbox) { fprintf(stderr,"--xbox needs an IPv4 address like 192.168.1.20.\n"); return false; }
        }
        else if(hasValue && a=="--ftp-user") Settings.ftpUser=argv[++i];
        else if(hasValue && a=="--ftp-password") Settings.ftpPassword=argv[++i];
        else if(hasValue && a=="--xbox-folder") Settings.xboxFolder=argv[++i];
        else if(hasValue && a=="--aurora") Settings.aurora=argv[++i];
        else if(hasValue && a=="--port") Settings.port=atoi(argv[++i]);
        else if(hasValue && a=="--ftp-port") Settings.ftpPort=atoi(argv[++i]);
        else if(hasValue && a=="--launcher") Settings.launcherPath=argv[++i];
        else if(hasValue && a=="--boot") Settings.bootPath=argv[++i];
        else if(a[0]!='-' && !folderGiven) { Settings.folder=a; folderGiven=true; }
        else { fprintf(stderr,"Unknown option %s (-h lists them).\n",a.c_str()); return false; }
    }
    while(Settings.xboxFolder.size()>1 && Settings.xboxFolder[Settings.xboxFolder.size()-1]=='/') Settings.xboxFolder.erase(Settings.xboxFolder.size()-1);
    while(Settings.folder.size()>1 && (Settings.folder[Settings.folder.size()-1]=='/' || Settings.folder[Settings.folder.size()-1]=='\\')) Settings.folder.erase(Settings.folder.size()-1);
    return Settings.port>0 && Settings.port<65536 && Settings.ftpPort>0 && Settings.ftpPort<65536;
}

int Run(int argc, char** argv) {
    if(!ParseArguments(argc,argv)) return 2;
#ifdef _WIN32
    SetConsoleTitleA("NetISO server for Aurora");
    WSADATA wsa;
    if(WSAStartup(MAKEWORD(2,2),&wsa)) { fprintf(stderr,"Winsock didn't start.\n"); return 1; }
#endif
    Log("NetISO server for Aurora, built %s %s.",__DATE__,__TIME__);
    std::vector<IsoFile> isos=Isos();
    Log("%d ISO(s) in %s and its subfolders:",int(isos.size()),Settings.folder.c_str());
    for(size_t i=0;i<isos.size();++i) Log("  %s",isos[i].name.c_str());
    if(isos.empty()) Log("New ISOs put there are served without a restart.");

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
    if(Settings.push) {
        if(Settings.xbox) { char where[16]; AddressText(Settings.xbox,where,sizeof(where)); Log("Keeping %s on the Xbox at %s up to date.",Settings.xboxFolder.c_str(),where); }
        else Log("Keeping %s on the Xbox up to date once the Xbox connects (or give --xbox).",Settings.xboxFolder.c_str());
        std::thread(PushLoop).detach();
    }
    Serve(listener);
    return 0;
}
}

int main(int argc, char** argv) {
    int code=Run(argc,argv);
#ifdef _WIN32
    // Started from Explorer: keep the window open to show why it stopped.
    DWORD processes[2];
    if(code && GetConsoleProcessList(processes,2)==1) { printf("Press Enter to close."); getchar(); }
#endif
    return code;
}
