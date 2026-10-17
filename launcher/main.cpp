// NetISO launcher and NetISO Sync: one XEX, two jobs, picked by the
// netiso.ini next to it.
//
// Game launcher (netiso.ini has iso=): a copy made by NetISO Sync or
// tools/netiso_aurora.py, carrying the game's Title ID and Media ID in its
// XEX header so Aurora scans it like an installed game. Started, it asks the
// NetISO plugin to mount the game's ISO from the server, waits until the
// emulated disc is that game, and launches the disc's default.xex.
//
// NetISO Sync (no iso=): talks to the NetISO server directly, makes a game
// launcher folder for each ISO that has none yet (copying itself), removes
// the folders of ISOs that are gone, then starts after= or the dashboard.
// Its own connection has its own mounted ISO on the server, so syncing never
// changes the disc the plugin has mounted.
//
// How the plugin mounts (read from NetISO.xex 1.0, the plugin has no source):
// it registers the file system device \Device\NetIso. Listing its folders
// shows the server's ISOs, each with a "Mount" folder inside. Listing that
// "Mount" folder sends MountIso to the server, and on success the plugin
// cycles a fake disc tray so the console sees the new disc at \Device\Cdrom0.
// Aurora's file browser does the same when you open those folders. The
// launcher walks the same folders by name instead of assuming their depth.
#include <xtl.h>
#include <winsockx.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include "../shared/library.h"

struct KernelString { USHORT length,capacity; char* text; };
extern "C" LONG __stdcall ObCreateSymbolicLink(KernelString* link,KernelString* device);
extern "C" LONG __stdcall ObDeleteSymbolicLink(KernelString* link);
// xam.xex ordinal 421 (XamLoaderLaunchTitleEx; the RXDK import library calls it
// XLaunchNewImageEx): starts xexName from mountPath. Nova.xex's file browser
// starts files this way, with flags 0x10. It starts the game, but the Guide
// is dead in it, so it's only the fallback (and start=nova) now.
extern "C" DWORD __stdcall XLaunchNewImageEx(LPCSTR xexName,LPCSTR mountPath,LPCSTR commandLine,DWORD flags);
// XLaunchNewImage is declared DECLSPEC_NORETURN, so the compiler drops the
// code after a call, but it does return when the launch fails. Calling it
// through this pointer keeps that code.
typedef VOID (WINAPI* LaunchFunction)(LPCSTR,DWORD);
static LaunchFunction volatile LaunchImage=XLaunchNewImage;

static const char IniFile[]="game:\\netiso.ini",LogFile[]="game:\\netiso.log";
static const char IsoLink[]="\\??\\NIsoL:",IsoRoot[]="NIsoL:",IsoDevice[]="\\Device\\NetIso";
static const char DiscLink[]="\\??\\NIsoD:",DiscXex[]="NIsoD:\\default.xex",DiscDevice[]="\\Device\\Cdrom0";
static const char DiscMount[]="\\Device\\Cdrom0",DiscXexName[]="default.xex";
static const DWORD NovaLaunchFlags=0x10;
// Aurora's disc entry (Aurora.xex 0.7b.2: ScnProfileDVD, then its
// ContentLauncher) links Dvd: to \Device\Cdrom0 and starts the disc with
// XLaunchNewImage("Dvd:\default.xex",0), which is XamLoaderLaunchTitle with
// flags 0. Started that way, the game has a working Guide. The same call
// with \Device\Cdrom0\default.xex instead ended on the Xbox dashboard.
static const char AuroraDiscLink[]="\\??\\Dvd:",AuroraDiscXex[]="Dvd:\\default.xex";
// What XLaunchNewImage takes to start whatever disc is in the drive, like
// the Xbox dashboard does (read from Aurora's copy of XLaunchNewImage).
static const char DetectMedia[]="XLNI_DET_MEDIA";
static const char BootXex[]="game:\\boot.xex",BootData[]="NetISO boot";
static const char MountFolder[]="Mount";
static const int MountWaitMs=30000,PollMs=500,MaxDepth=4,MaxNames=256;
// After the mount the plugin fakes a tray open and close on its own thread
// (0.2 s apart), and the console then reads the new disc. The launcher used
// to read the disc right away and every 0.5 s while that went on; the game
// it started then had the Guide dead and crashed at its storage/profile
// prompt, while the same mount from Aurora's file browser, which leaves the
// disc alone, was fine (seen on the console). So the launcher keeps off the
// disc this long after the mount before it checks it.
// quiet_ms= in netiso.ini overrides it.
static const int QuietMs=5000;
// Whole number from netiso.ini, or fallback.
static int IniNumber(const char* ini,const char* key,int fallback) {
    char value[16];
    if(!nia::IniValue(ini,key,value,sizeof(value)) || !value[0]) return fallback;
    return atoi(value);
}

// A plain screen with a progress bar, drawn while the launcher works. A title
// that never draws left the Guide dead, in the game it started too (seen on
// the console), so the launcher keeps presenting frames until it launches.
static IDirect3DDevice9* Screen=NULL;
static int Progress=0; // 0 to 100
static bool Failed=false; // Draws the bar red.
static void ScreenStart() {
    IDirect3D9* d3d=Direct3DCreate9(D3D_SDK_VERSION);
    if(!d3d) return;
    D3DPRESENT_PARAMETERS pp; ZeroMemory(&pp,sizeof(pp));
    pp.BackBufferWidth=1280; pp.BackBufferHeight=720; pp.BackBufferFormat=D3DFMT_X8R8G8B8; pp.BackBufferCount=1;
    pp.MultiSampleType=D3DMULTISAMPLE_NONE; pp.SwapEffect=D3DSWAPEFFECT_DISCARD;
    pp.PresentationInterval=D3DPRESENT_INTERVAL_ONE;
    if(FAILED(d3d->CreateDevice(0,D3DDEVTYPE_HAL,NULL,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&Screen))) Screen=NULL;
    d3d->Release();
}
static void ScreenDraw() {
    if(!Screen) return;
    Screen->Clear(0,NULL,D3DCLEAR_TARGET,D3DCOLOR_XRGB(16,16,16),1.0f,0);
    D3DRECT track={340,500,940,510},bar={340,500,340+6*Progress,510};
    Screen->Clear(1,&track,D3DCLEAR_TARGET,D3DCOLOR_XRGB(48,48,48),1.0f,0);
    if(Failed) Screen->Clear(1,&track,D3DCLEAR_TARGET,D3DCOLOR_XRGB(180,32,32),1.0f,0);
    else if(Progress>0) Screen->Clear(1,&bar,D3DCLEAR_TARGET,D3DCOLOR_XRGB(16,124,16),1.0f,0);
    Screen->Present(NULL,NULL,NULL,NULL);
}
// Before launching anything.
static void ScreenStop() {
    if(Screen) { Screen->Release(); Screen=NULL; }
}
static FILE* Log=NULL;

// System notifications, read the way a game reads them. The plugin's fake
// disc swap while the launcher ran left the Guide dead in the game it then
// started; mounted from Aurora first, the same launch kept the Guide (seen on
// the console). Aurora reads its notifications; the launcher didn't. Each one
// is logged, to see what the swap sends.
static HANDLE Notifications=NULL;
// The log is also written from the mount thread; only the main thread draws.
static CRITICAL_SECTION LogLock;
static DWORD MainThread=0;
static void ReadNotifications() {
    if(!Notifications) return;
    DWORD id; ULONG_PTR param;
    while(XNotifyGetNext(Notifications,0,&id,&param)) {
        EnterCriticalSection(&LogLock);
        if(Log) { fprintf(Log,"  notification %08X, %08X\n",id,(DWORD)param); fflush(Log); }
        LeaveCriticalSection(&LogLock);
    }
}

// Sleep that keeps drawing and reading notifications.
static void Wait(int ms) {
    for(int waited=0;waited<ms;waited+=20) { ReadNotifications(); ScreenDraw(); Sleep(20); }
}

static void Note(const char* format,...) {
    if(GetCurrentThreadId()==MainThread) { ReadNotifications(); ScreenDraw(); }
    EnterCriticalSection(&LogLock);
    if(Log) {
        va_list args; va_start(args,format); vfprintf(Log,format,args); va_end(args);
        fputc('\n',Log); fflush(Log);
    }
    LeaveCriticalSection(&LogLock);
}
static void NoteText(const char* text) { Note("%s",text); }

static LONG LinkDrive(const char* name,const char* device) {
    char nameText[32],deviceText[64]; strcpy_s(nameText,name); strcpy_s(deviceText,device);
    KernelString link={USHORT(strlen(nameText)),USHORT(sizeof(nameText)),nameText};
    KernelString target={USHORT(strlen(deviceText)),USHORT(sizeof(deviceText)),deviceText};
    ObDeleteSymbolicLink(&link); // Left over from an interrupted run.
    return ObCreateSymbolicLink(&link,&target);
}
static void UnlinkDrive(const char* name) {
    char nameText[32]; strcpy_s(nameText,name);
    KernelString link={USHORT(strlen(nameText)),USHORT(sizeof(nameText)),nameText};
    ObDeleteSymbolicLink(&link);
}

// Whole small text file, NUL-terminated.
static bool ReadText(const char* path,char* text,DWORD capacity) {
    text[0]=0;
    HANDLE file=CreateFile(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file==INVALID_HANDLE_VALUE) return false;
    DWORD done=0; BOOL ok=ReadFile(file,text,capacity-1,&done,NULL);
    CloseHandle(file);
    text[ok ? done : 0]=0;
    return ok!=FALSE;
}

// ---- Game launcher ----

struct Settings { char iso[MAX_PATH]; DWORD titleId,mediaId; };
static bool ReadSettings(const char* ini,Settings& settings) {
    memset(&settings,0,sizeof(settings));
    char value[32];
    nia::IniValue(ini,"iso",settings.iso,sizeof(settings.iso));
    if(nia::IniValue(ini,"title_id",value,sizeof(value))) settings.titleId=strtoul(value,NULL,16);
    if(nia::IniValue(ini,"media_id",value,sizeof(value))) settings.mediaId=strtoul(value,NULL,16);
    if(!settings.iso[0]||!settings.titleId) { Note("%s needs iso= and title_id=.",IniFile); return false; }
    Note("ISO %s, Title ID %08X, Media ID %08X",settings.iso,settings.titleId,settings.mediaId);
    return true;
}

// Title ID and Media ID from the execution info header (0x40006) of the
// default.xex on the disc that is in the drive now.
static bool DiscIds(DWORD& titleId,DWORD& mediaId) {
    HANDLE file=CreateFile(DiscXex,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file==INVALID_HANDLE_VALUE) return false;
    bool found=false;
    DWORD header[6],done=0; // Read as words: the console is big-endian, like the file.
    if(ReadFile(file,header,sizeof(header),&done,NULL)&&done==sizeof(header)&&!memcmp(header,"XEX2",4)) {
        DWORD count=header[5];
        for(DWORD i=0;i<count&&i<64&&!found;++i) {
            DWORD entry[2];
            if(SetFilePointer(file,24+i*8,NULL,FILE_BEGIN)==INVALID_SET_FILE_POINTER) break;
            if(!ReadFile(file,entry,sizeof(entry),&done,NULL)||done!=sizeof(entry)) break;
            if(entry[0]!=0x40006) continue;
            DWORD info[4]; // media ID, version, base version, title ID
            if(SetFilePointer(file,entry[1],NULL,FILE_BEGIN)==INVALID_SET_FILE_POINTER) break;
            if(ReadFile(file,info,sizeof(info),&done,NULL)&&done==sizeof(info)) {
                mediaId=info[0]; titleId=info[3]; found=true;
            }
        }
    }
    CloseHandle(file);
    return found;
}
static bool DiscIsGame(const Settings& settings) {
    DWORD titleId=0,mediaId=0;
    if(!DiscIds(titleId,mediaId)) return false;
    return titleId==settings.titleId&&(!settings.mediaId||mediaId==settings.mediaId);
}

static char Names[MaxNames][MAX_PATH];
// Lists a NetIso folder. With the plugin, listing a "Mount" folder is what
// mounts, and that listing is empty.
static int ListFolder(const char* folder) {
    char pattern[MAX_PATH]; sprintf_s(pattern,"%s\\*",folder);
    WIN32_FIND_DATA found;
    HANDLE find=FindFirstFile(pattern,&found);
    if(find==INVALID_HANDLE_VALUE) { Note("%s: nothing listed (error %lu)",pattern,GetLastError()); return 0; }
    int count=0;
    do {
        if(count<MaxNames) strcpy_s(Names[count++],found.cFileName);
        Note("  %s",found.cFileName);
    } while(FindNextFile(find,&found));
    FindClose(find);
    Note("%s: %d entries",pattern,count);
    return count;
}
static const char* FindName(int count,const char* name) {
    for(int i=0;i<count;++i) if(!_stricmp(Names[i],name)) return Names[i];
    return NULL;
}

// Walks from the root toward the ISO's Mount folder by name, the way you
// would in Aurora's file browser, and returns that folder's path (inside
// NIsoL:). Listing these folders doesn't mount; listing Mount does.
static bool FindMountFolder(const Settings& settings,char* folder,size_t capacity) {
    strcpy_s(folder,capacity,IsoRoot);
    bool sawIso=false;
    for(int depth=0;depth<MaxDepth;++depth) {
        int count=ListFolder(folder);
        const char* next=FindName(count,settings.iso);
        if(next) sawIso=true;
        else if((next=FindName(count,MountFolder))!=NULL) {
            strcat_s(folder,capacity,"\\"); strcat_s(folder,capacity,next);
            return true;
        }
        if(!next) {
            Note(sawIso?"No Mount folder under %s.":"%s isn't listed by the server. Is the server running, and is the ISO in its folder?",
                sawIso?folder:settings.iso);
            return false;
        }
        strcat_s(folder,capacity,"\\"); strcat_s(folder,capacity,next);
    }
    Note("Gave up after %d folders without a Mount folder.",MaxDepth);
    return false;
}
// Lists a Mount folder, which mounts: the plugin swaps the disc before the
// listing returns (empty, error 18 no more files), then fakes the tray open
// and close on its own thread (read from NetISO.xex).
static bool ListMountFolder(const char* folder) {
    char pattern[MAX_PATH]; sprintf_s(pattern,"%s\\*",folder);
    WIN32_FIND_DATA found;
    HANDLE find=FindFirstFile(pattern,&found);
    DWORD error=ERROR_SUCCESS;
    if(find==INVALID_HANDLE_VALUE) error=GetLastError(); else FindClose(find);
    Note("%s: error %lu",folder,error);
    return error==ERROR_NO_MORE_FILES||error==ERROR_SUCCESS;
}

static bool FindServer(const char* ini,DWORD& address,int& port);
static bool StartNetwork();
// Whether the NetISO server answers, before asking the plugin to mount. Not
// knowing the server isn't fatal: the plugin may still have one.
static bool ServerAnswers(const char* ini) {
    DWORD address=0; int port=nia::IsvrPort;
    if(!FindServer(ini,address,port)) { Note("Not checking the server."); return true; }
    if(!StartNetwork()) return false;
    nia::IsvrClient client(5);
    bool ok=client.Connect(address,port,3) && client.Ping();
    if(ok) Note("The server answers.");
    else Note("Can't reach the NetISO server: %s",client.Error());
    client.Close();
    return ok;
}

static bool MountGame(const char* ini) {
    Settings settings;
    if(!ReadSettings(ini,settings)) return false;
    if(LinkDrive(DiscLink,DiscDevice)<0) { Note("Can't link %s.",DiscDevice); return false; }
    if(DiscIsGame(settings)) { Note("The game is already mounted."); return true; }
    if(!ServerAnswers(ini)) { Failed=true; Wait(5000); return false; }
    Progress=5; ScreenDraw();
    if(!GetModuleHandle("NetISO.xex")) Note("NetISO.xex isn't loaded. Add it as a DashLaunch plugin.");
    LONG status=LinkDrive(IsoLink,IsoDevice);
    if(status<0) { Note("Can't link %s (status %08X). Is the NetISO plugin running?",IsoDevice,status); return false; }
    char folder[MAX_PATH];
    bool asked=FindMountFolder(settings,folder,sizeof(folder)) && ListMountFolder(folder);
    UnlinkDrive(IsoLink);
    if(!asked) { Failed=true; Wait(5000); return false; }
    int quiet=IniNumber(ini,"quiet_ms",QuietMs);
    if(quiet!=QuietMs) Note("Leaving the disc alone for %d ms (quiet_ms).",quiet);
    for(int waited=0;waited<quiet;waited+=PollMs) {
        Progress=10+60*waited/quiet;
        Wait(PollMs);
    }
    for(int waited=quiet;waited<quiet+MountWaitMs;waited+=PollMs) {
        if(DiscIsGame(settings)) { Note("The disc is the game %d ms after the mount.",waited); return true; }
        Progress=70+20*(waited-quiet)/MountWaitMs;
        Wait(PollMs);
    }
    DWORD titleId=0,mediaId=0;
    if(DiscIds(titleId,mediaId)) Note("The disc is still %08X/%08X after %d ms.",titleId,mediaId,MountWaitMs);
    else Note("No readable disc after %d ms.",MountWaitMs);
    Failed=true; Wait(5000);
    return false;
}

// Aurora starts a game with no launch data. The helper was started with
// "NetISO boot", which would otherwise go on to the game.
static void ClearLaunchData() {
    DWORD size=0;
    if(XGetLaunchDataSize(&size)!=ERROR_SUCCESS || !size) return;
    static char none[4];
    DWORD result=XSetLaunchData(none,0);
    Note("Clearing the launch data: %lu.",result);
}

// The XLaunchNewImage calls return only when the start failed. The log is
// closed while they run.
static void Launch(const char* path) {
    if(Log) { fclose(Log); Log=NULL; }
    LaunchImage(path,0);
    if(fopen_s(&Log,LogFile,"a")) Log=NULL;
    Note("XLaunchNewImage(%s) failed (%lu).",path,GetLastError());
}

// Starts default.xex on the disc the way Aurora's disc entry does, or as
// start= in netiso.ini says: aurora (the default), nova (Nova.xex's
// XamLoaderLaunchTitleEx call, the previous way) or media (whatever disc is
// in the drive, as the dashboard starts it). Returns only if that failed.
static void StartDisc(const char* ini) {
    char start[16]="aurora";
    nia::IniValue(ini,"start",start,sizeof(start));
    ClearLaunchData();
    Progress=100; ScreenDraw(); ScreenStop();
    if(!_stricmp(start,"media")) {
        Note("Starting the disc in the drive (start=media)");
        Launch(DetectMedia);
    } else if(_stricmp(start,"nova")) {
        LONG status=LinkDrive(AuroraDiscLink,DiscDevice);
        if(status<0) Note("Can't link %s (status %08X).",AuroraDiscLink,status);
        else { Note("Starting %s, as Aurora's disc entry does",AuroraDiscXex); Launch(AuroraDiscXex); }
    }
    Note("Starting %s from %s, as Nova's file browser does",DiscXexName,DiscMount);
    if(Log) { fclose(Log); Log=NULL; }
    DWORD result=XLaunchNewImageEx(DiscXexName,DiscMount,NULL,NovaLaunchFlags);
    if(fopen_s(&Log,LogFile,"a")) Log=NULL;
    Note("XamLoaderLaunchTitleEx returned %08X (%lu).",result,GetLastError());
}

// mount_only=1 in netiso.ini mounts and goes back to Aurora without starting
// the game, to tell whether the mount or the start breaks something.
static void LaunchGame(const char* ini) {
    bool mounted=MountGame(ini);
    UnlinkDrive(DiscLink);
    if(!mounted) return;
    if(IniNumber(ini,"mount_only",0)) { Note("mount_only=1: back to Aurora without starting the game."); return; }
    StartDisc(ini);
}

// ---- NetISO Sync ----

// Drive names as Aurora shows them, linked under names of our own.
struct Drive { const char* name; const char* link; const char* device; };
static const Drive Drives[]={
    {"Hdd1:","\\??\\NIsoH:","\\Device\\Harddisk0\\Partition1"},
    {"Usb0:","\\??\\NIsoU0:","\\Device\\Mass0"},
    {"Usb1:","\\??\\NIsoU1:","\\Device\\Mass1"},
};
static const int DriveCount=int(sizeof(Drives)/sizeof(Drives[0]));
// "Hdd1:\NetISO" -> "NIsoH:\NetISO", linking the drive. Other paths (game:,
// a \Device path) are used as they are.
static bool MapPath(const char* path,char* out,size_t capacity) {
    for(int i=0;i<DriveCount;++i) {
        size_t length=strlen(Drives[i].name);
        if(_strnicmp(path,Drives[i].name,length)) continue;
        if(LinkDrive(Drives[i].link,Drives[i].device)<0) { Note("Can't link %s.",Drives[i].device); return false; }
        _snprintf_s(out,capacity,_TRUNCATE,"%s%s",Drives[i].link+4,path+length);
        return true;
    }
    strcpy_s(out,capacity,path);
    return true;
}
// "Hdd1:\Apps\Aurora\Aurora.xex" -> "\Device\Harddisk0\Partition1\Apps\...",
// which the next title can resolve without our links.
static void DevicePath(const char* path,char* out,size_t capacity) {
    for(int i=0;i<DriveCount;++i) {
        size_t length=strlen(Drives[i].name);
        if(!_strnicmp(path,Drives[i].name,length)) { _snprintf_s(out,capacity,_TRUNCATE,"%s%s",Drives[i].device,path+length); return; }
    }
    strcpy_s(out,capacity,path);
}
static void UnlinkDrives() {
    for(int i=0;i<DriveCount;++i) UnlinkDrive(Drives[i].link);
}

class XboxStorage : public nia::Storage {
public:
    int ListFolders(const char* root,char names[][nia::FolderNameMax+1],int capacity) {
        char pattern[nia::PathMax]; _snprintf_s(pattern,sizeof(pattern),_TRUNCATE,"%s\\*",root);
        WIN32_FIND_DATA found;
        HANDLE find=FindFirstFile(pattern,&found);
        if(find==INVALID_HANDLE_VALUE) return 0;
        int count=0;
        do {
            if((found.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) && count<capacity && strlen(found.cFileName)<=nia::FolderNameMax)
                strcpy_s(names[count++],nia::FolderNameMax+1,found.cFileName);
        } while(FindNextFile(find,&found));
        FindClose(find);
        return count;
    }
    bool ReadFile(const char* path,unsigned char* data,DWORD capacity,DWORD& size) {
        HANDLE file=CreateFile(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
        if(file==INVALID_HANDLE_VALUE) return false;
        DWORD high=0,length=GetFileSize(file,&high);
        bool ok=!high && length<=capacity && ::ReadFile(file,data,length,&size,NULL) && size==length;
        CloseHandle(file);
        return ok;
    }
    bool WriteFile(const char* path,const void* data,DWORD size) {
        HANDLE file=CreateFile(path,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
        if(file==INVALID_HANDLE_VALUE) return false;
        DWORD done=0;
        bool ok=::WriteFile(file,data,size,&done,NULL) && done==size;
        return CloseHandle(file) && ok;
    }
    bool MakeFolder(const char* path) { return CreateDirectory(path,NULL) || GetLastError()==ERROR_ALREADY_EXISTS; }
    bool RemoveGame(const char* folder) {
        static const char* files[]={"default.xex","boot.xex","netiso.ini","netiso.log"};
        for(int i=0;i<4;++i) {
            char path[nia::PathMax]; _snprintf_s(path,sizeof(path),_TRUNCATE,"%s\\%s",folder,files[i]);
            DeleteFile(path);
        }
        return RemoveDirectory(folder)!=FALSE;
    }
    unsigned long LastError() { return GetLastError(); }
};

// "192.168.1.20" or "192.168.1.20:4323".
static bool ParseServer(const char* text,DWORD& address,int& port) {
    char host[64]; strcpy_s(host,text);
    char* colon=strchr(host,':');
    if(colon) { *colon=0; port=atoi(colon+1); if(port<1||port>65535) return false; }
    size_t length=strlen(host);
    while(length && (host[length-1]==' '||host[length-1]=='\t')) host[--length]=0;
    address=inet_addr(host);
    return address!=INADDR_NONE && address!=0;
}
// server= in netiso.ini, or the NetISO plugin's own netiso.xex.txt.
static bool FindServer(const char* ini,DWORD& address,int& port) {
    char value[64];
    if(nia::IniValue(ini,"server",value,sizeof(value))) {
        if(ParseServer(value,address,port)) return true;
        Note("server=%s in %s is not an IPv4 address.",value,IniFile); return false;
    }
    static const char* plugin[]={"Hdd1:\\netiso.xex.txt","Usb0:\\netiso.xex.txt"};
    for(int i=0;i<int(sizeof(plugin)/sizeof(plugin[0]));++i) {
        char path[nia::PathMax],text[128];
        if(!MapPath(plugin[i],path,sizeof(path)) || !ReadText(path,text,sizeof(text))) continue;
        text[strcspn(text,"\r\n")]=0;
        if(ParseServer(text,address,port)) { Note("Server %s from %s.",text,plugin[i]); return true; }
    }
    Note("No server. Put server=<IP address> in %s.",IniFile);
    return false;
}

static bool StartNetwork() {
    XNetStartupParams params; memset(&params,0,sizeof(params));
    params.cfgSizeOfStruct=sizeof(params); params.cfgFlags=XNET_STARTUP_BYPASS_SECURITY;
    WSADATA wsa;
    if(XNetStartup(&params) || WSAStartup(MAKEWORD(2,2),&wsa)) { Note("Network startup failed."); return false; }
    // Right after boot the console may still be getting its address.
    XNADDR address;
    for(int waited=0;waited<10000;waited+=250) {
        if(XNetGetTitleXnAddr(&address)!=XNET_GET_XNADDR_PENDING) return true;
        Wait(250);
    }
    Note("No network address after 10 s.");
    return false;
}

static unsigned char Launcher[4<<20],Boot[4<<20];
static void Sync(const char* ini) {
    DWORD address=0; int port=nia::IsvrPort;
    char value[nia::PathMax],games[nia::PathMax];
    if(!nia::IniValue(ini,"games",value,sizeof(value))) strcpy_s(value,"Hdd1:\\NetISO");
    if(!MapPath(value,games,sizeof(games))) return;
    Note("Games folder %s",value);
    // This XEX is the template; its execution info is overwritten per game.
    XboxStorage storage; DWORD size=0;
    if(!storage.ReadFile("game:\\default.xex",Launcher,sizeof(Launcher),size)) { Note("Can't read game:\\default.xex."); return; }
    // The helper each game folder gets (see HandToHelper), if it's here.
    DWORD bootSize=0;
    bool boot=storage.ReadFile(BootXex,Boot,sizeof(Boot),bootSize);
    if(!boot) Note("No %s; games will mount from their own launcher.",BootXex);
    if(!FindServer(ini,address,port) || !StartNetwork()) return;
    nia::IsvrClient client(10);
    bool connected=false;
    for(int attempt=0;attempt<3 && !connected;++attempt) {
        connected=client.Connect(address,port,3) && client.Ping();
        if(!connected) { Note("Server: %s",client.Error()); Wait(1000); }
    }
    if(!connected) return;
    nia::SyncResult result;
    bool ok=nia::Sync(client,storage,games,Launcher,size,boot ? Boot : NULL,bootSize,NoteText,result);
    Note("%s: %d added, %d kept (%d updated), %d removed, %d skipped.",ok?"Done":"Stopped",result.added,result.kept,result.updated,result.removed,result.skipped);
    client.Close();
}

// Where Sync goes when it's done: after= in netiso.ini, or the dashboard.
// When Sync is DashLaunch's Default, after= must name Aurora, or the
// dashboard would be Sync again.
static void LaunchAfterSync(const char* ini) {
    char value[nia::PathMax],path[nia::PathMax];
    if(!nia::IniValue(ini,"after",value,sizeof(value)) || !value[0]) return;
    DevicePath(value,path,sizeof(path));
    Note("Starting %s",path);
    if(Log) { fclose(Log); Log=NULL; }
    UnlinkDrives();
    ScreenStop();
    LaunchImage(path,0);
    if(fopen_s(&Log,LogFile,"a")) Log=NULL;
    Note("Couldn't start %s (%lu).",path,GetLastError());
}

// The game's launcher hands the whole start to boot.xex in its folder, a copy
// of this program built without execution info (no Title ID), like Aurora.
// The plugin's fake tray cycle, run while the launcher (with the game's Title
// ID) was the running title, left the Guide dead until reboot, and the game
// crashed at its storage/profile prompt; mounted while Aurora ran, the same
// game was fine (seen on the console). The launcher needs the Title ID for
// Aurora's tile, so the mount happens in the helper instead.
// Whether this start came with the launch data text given.
static bool LaunchedWith(const char* text) {
    char data[32]; DWORD size=0,length=DWORD(strlen(text)+1);
    if(XGetLaunchDataSize(&size)!=ERROR_SUCCESS || size!=length || size>sizeof(data)) return false;
    return XGetLaunchData(data,size)==ERROR_SUCCESS && !memcmp(data,text,length);
}
static void HandToHelper() {
    HANDLE file=CreateFile(BootXex,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file==INVALID_HANDLE_VALUE) { Note("No %s; mounting from the launcher.",BootXex); return; }
    CloseHandle(file);
    DWORD set=XSetLaunchData((PVOID)BootData,sizeof(BootData));
    if(set!=ERROR_SUCCESS) { Note("XSetLaunchData failed (%lu); mounting from the launcher.",set); return; }
    Note("Handing the start to %s",BootXex);
    if(Log) { fclose(Log); Log=NULL; }
    ScreenStop();
    LaunchImage(BootXex,0);
    if(fopen_s(&Log,LogFile,"a")) Log=NULL;
    Note("Couldn't start %s (%lu); mounting from the launcher.",BootXex,GetLastError());
    ScreenStart();
}

void __cdecl main() {
    InitializeCriticalSection(&LogLock);
    MainThread=GetCurrentThreadId();
    Notifications=XNotifyCreateListener(XNOTIFY_ALL);
    ScreenStart();
    ScreenDraw();
    bool helper=LaunchedWith(BootData);
    // The helper appends to the launcher's log.
    if(fopen_s(&Log,LogFile,helper ? "a" : "w")) Log=NULL;
    Note("NetISO launcher built %s %s",__DATE__,__TIME__);
    if(helper) Note("Started as the helper.");
    static char ini[nia::IniMax];
    if(!ReadText(IniFile,ini,sizeof(ini))) Note("Can't open %s.",IniFile);
    static char iso[MAX_PATH];
    bool game=nia::IniValue(ini,"iso",iso,sizeof(iso)) && iso[0];
    if(game) {
        if(!helper) HandToHelper();
        LaunchGame(ini);
    } else {
        Note("NetISO Sync");
        Sync(ini);
        LaunchAfterSync(ini);
    }
    UnlinkDrives();
    if(Log) fclose(Log);
    ScreenStop();
    // A game that didn't start goes back to the default app, which DashLaunch
    // points at its Default (Aurora). Sync goes to the dashboard: it may be
    // the Default itself, and would start again.
    LaunchImage(game ? XLAUNCH_KEYWORD_DEFAULT_APP : XLAUNCH_KEYWORD_DASH,0);
}
