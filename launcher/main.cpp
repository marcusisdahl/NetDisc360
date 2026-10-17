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
#include <math.h>
#include "../shared/library.h"
#include "../third_party/imgui/imgui.h"
#include "imgui_impl_xbox360.h"
#include "ui.h"

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
// The launch data the tile's launcher hands the helper: BootData, then when
// the tile was pressed, so the helper's timer counts from there.
struct BootLaunch { char text[sizeof(BootData)]; DWORD started; };
// GetTickCount when the tile was pressed, for the timer at the top.
static DWORD Started=0;
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

// The loading screen (ui.cpp, in Xbox360-FastFTP's style). A title that
// never draws left the Guide dead, in the game it started too (seen on the
// console), so the launcher keeps presenting frames until it launches. Only
// the main thread draws; the mount runs on a worker that sets the steps.
static IDirect3DDevice9* Screen=NULL;
static nia::LoadScreen Shown;
static CRITICAL_SECTION ShownLock;
static WORD Held=0; // Controller buttons down at the last look.
static WORD Buttons() {
    WORD buttons=0;
    for(DWORD pad=0;pad<4;++pad) {
        XINPUT_STATE state; ZeroMemory(&state,sizeof(state));
        if(XInputGetState(pad,&state)==ERROR_SUCCESS) buttons|=state.Gamepad.wButtons;
    }
    return buttons;
}
// Buttons pressed since the last call.
static WORD Pressed() {
    WORD buttons=Buttons(),pressed=buttons&~Held;
    Held=buttons;
    return pressed;
}
static void ScreenStart() {
    if(ImGui::GetCurrentContext()) ImGui::DestroyContext();
    nia::InitLoadUi();
    IDirect3D9* d3d=Direct3DCreate9(D3D_SDK_VERSION);
    if(!d3d) return;
    D3DPRESENT_PARAMETERS pp; ZeroMemory(&pp,sizeof(pp));
    pp.BackBufferWidth=1280; pp.BackBufferHeight=720; pp.BackBufferFormat=D3DFMT_A8R8G8B8;
    pp.FrontBufferFormat=D3DFMT_LE_X8R8G8B8; pp.BackBufferCount=1;
    pp.MultiSampleType=D3DMULTISAMPLE_NONE; pp.SwapEffect=D3DSWAPEFFECT_DISCARD;
    pp.PresentationInterval=D3DPRESENT_INTERVAL_ONE;
    if(FAILED(d3d->CreateDevice(0,D3DDEVTYPE_HAL,NULL,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&Screen))) Screen=NULL;
    d3d->Release();
    if(Screen && !ImGuiXbox360Init(Screen)) { Screen->Release(); Screen=NULL; }
    // The A that started the tile may still be down.
    Held=Buttons();
}
// The bar glides to the fraction the steps set instead of jumping: quickly
// while it's far behind, never slower than BarMinSpeed a second, so it gets
// there. A smaller fraction is a new attempt (X), and the bar starts over.
static double Bar=0;
static DWORD BarAt=0;
static const double BarRate=5,BarMinSpeed=0.6;
static void Glide(double target) {
    DWORD now=GetTickCount();
    double seconds=BarAt ? (now-BarAt)/1000.0 : 0;
    BarAt=now;
    if(seconds>0.1) seconds=0.1;
    if(target<Bar) { Bar=target; return; }
    double speed=(target-Bar)*BarRate;
    if(speed<BarMinSpeed) speed=BarMinSpeed;
    Bar+=speed*seconds;
    if(Bar>target) Bar=target;
}
static void ScreenDraw() {
    if(!Screen) return;
    EnterCriticalSection(&ShownLock);
    nia::LoadScreen frame=Shown;
    LeaveCriticalSection(&ShownLock);
    Glide(frame.fraction);
    frame.fraction=Bar;
    frame.time=GetTickCount()/1000.0;
    frame.elapsed=(GetTickCount()-Started)/1000.0;
    ImGui::GetIO().DeltaTime=1.0f/60;
    nia::DrawLoadUi(frame);
    Screen->Clear(0,NULL,D3DCLEAR_TARGET,0xFF0F1725,1.0f,0);
    ImGuiXbox360Render(ImGui::GetDrawData());
    Screen->Present(NULL,NULL,NULL,NULL);
}
// Before launching anything.
static void ScreenStop() {
    if(Screen) { Screen->Release(); Screen=NULL; }
}
// The step under way, how far along the whole start is (0 to 1), and a line about it.
static void ShowStep(int step,double fraction,const char* format,...) {
    EnterCriticalSection(&ShownLock);
    Shown.step=step; Shown.fraction=fraction;
    va_list args; va_start(args,format); _vsnprintf_s(Shown.status,sizeof(Shown.status),_TRUNCATE,format,args); va_end(args);
    LeaveCriticalSection(&ShownLock);
}
// The log is one file per folder that every run appends to, each line with
// the console's time, so a run never wipes the one before it. Each run starts
// with a header line with the date and the console's uptime (a small uptime
// means a fresh boot). Past LogMax the next run starts the file over.
static FILE* Log=NULL;
static const long LogMax=512*1024;
static void OpenLog(bool run) {
    if(fopen_s(&Log,LogFile,"a")) { Log=NULL; return; }
    if(!run) return;
    fseek(Log,0,SEEK_END);
    if(ftell(Log)>LogMax) { fclose(Log); if(fopen_s(&Log,LogFile,"w")) Log=NULL; }
}
static CRITICAL_SECTION LogLock;
static void CloseLog() {
    EnterCriticalSection(&LogLock);
    if(Log) { fclose(Log); Log=NULL; }
    LeaveCriticalSection(&LogLock);
}
static void Stamp() {
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(Log,"%02u:%02u:%02u.%03u ",t.wHour,t.wMinute,t.wSecond,t.wMilliseconds);
}

// System notifications, read the way a game reads them. The plugin's fake
// disc swap while the launcher ran left the Guide dead in the game it then
// started; mounted from Aurora first, the same launch kept the Guide (seen on
// the console). Aurora reads its notifications; the launcher didn't. Each one
// is logged, to see what the swap sends.
static HANDLE Notifications=NULL;
// The log is also written from the mount thread; only the main thread draws.
static DWORD MainThread=0;
static void ReadNotifications() {
    if(!Notifications) return;
    DWORD id; ULONG_PTR param;
    while(XNotifyGetNext(Notifications,0,&id,&param)) {
        EnterCriticalSection(&LogLock);
        if(Log) { Stamp(); fprintf(Log,"  notification %08X, %08X\n",id,(DWORD)param); fflush(Log); }
        LeaveCriticalSection(&LogLock);
    }
}

// Sleep that keeps drawing and reading notifications on the main thread.
static void Wait(int ms) {
    if(GetCurrentThreadId()!=MainThread) { Sleep(ms); return; }
    for(DWORD start=GetTickCount();GetTickCount()-start<DWORD(ms);) { ReadNotifications(); ScreenDraw(); if(!Screen) Sleep(20); }
}

static void Note(const char* format,...) {
    if(GetCurrentThreadId()==MainThread) { ReadNotifications(); ScreenDraw(); }
    EnterCriticalSection(&LogLock);
    if(Log) {
        Stamp();
        va_list args; va_start(args,format); vfprintf(Log,format,args); va_end(args);
        fputc('\n',Log); fflush(Log);
    }
    LeaveCriticalSection(&LogLock);
}
static void NoteText(const char* text) { Note("%s",text); }
// Written before the notifications that were waiting, so they follow it.
static void NoteRun() {
    SYSTEMTIME t; GetLocalTime(&t);
    DWORD up=GetTickCount()/1000;
    EnterCriticalSection(&LogLock);
    if(Log) {
        fprintf(Log,"\n");
        Stamp();
        fprintf(Log,"==== %04u-%02u-%02u, NetISO launcher built %s %s, console up %lu:%02lu:%02lu ====\n",
                t.wYear,t.wMonth,t.wDay,__DATE__,__TIME__,up/3600,up/60%60,up%60);
        fflush(Log);
    }
    LeaveCriticalSection(&LogLock);
}

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
// would in Aurora's file browser, and gives that folder's path (inside
// NIsoL:). Listing these folders doesn't mount; listing Mount does. It stops
// without a path after the given number of listings.
enum Found { FoundMount, FoundStopped, FoundNoRoot, FoundNotListed, FoundNoMount };
static Found FindMountFolder(const Settings& settings,char* folder,size_t capacity,int listings) {
    strcpy_s(folder,capacity,IsoRoot);
    bool sawIso=false;
    for(int depth=0;depth<MaxDepth;++depth) {
        if(depth==listings) { Note("Stopping after %d listing(s) (stop_after).",depth); return FoundStopped; }
        int count=ListFolder(folder);
        // An empty root: the plugin couldn't reach the server.
        if(!depth && !count) return FoundNoRoot;
        const char* next=FindName(count,settings.iso);
        if(next) sawIso=true;
        else if((next=FindName(count,MountFolder))!=NULL) {
            strcat_s(folder,capacity,"\\"); strcat_s(folder,capacity,next);
            if(depth+1==listings) { Note("Stopping before listing %s (stop_after).",folder); return FoundStopped; }
            return FoundMount;
        }
        if(!next) {
            Note(sawIso?"No Mount folder under %s.":"%s isn't listed by the server.",sawIso?folder:settings.iso);
            return sawIso ? FoundNoMount : FoundNotListed;
        }
        strcat_s(folder,capacity,"\\"); strcat_s(folder,capacity,next);
    }
    Note("Gave up after %d folders without a Mount folder.",MaxDepth);
    return FoundNoMount;
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
static bool StartNetwork(int addressWaitMs);
static char ServerSource[48]="";

// A failed start, as the screen tells it: the step, a title and up to three
// lines, and whether trying again could help.
struct Failure { int step; bool retry; char title[96]; char lines[3][128]; };
static Failure Problem;
enum Result { Mounted, Failed, Cancelled };
static volatile LONG CancelAsked=0;
static Result Fail(int step,bool retry,const char* title,const char* line0,const char* line1=NULL,const char* line2=NULL) {
    Problem.step=step; Problem.retry=retry;
    strcpy_s(Problem.title,title);
    const char* lines[3]={line0,line1,line2};
    for(int i=0;i<3;++i) strcpy_s(Problem.lines[i],lines[i] ? lines[i] : "");
    Note("Failed: %s",title);
    for(int i=0;i<3;++i) if(Problem.lines[i][0]) Note("  %s",Problem.lines[i]);
    return Failed;
}
// Sleeps up to ms, in steps, unless B was pressed.
static bool Pause(int ms) {
    for(int waited=0;waited<ms;waited+=100) { if(CancelAsked) return false; Sleep(100); }
    return !CancelAsked;
}

// Right after a reboot the console can't reach the server for a minute or
// two (seen on the console), so until it has been up this long the launcher
// keeps trying instead of failing.
static const DWORD BootSeconds=240,RetrySeconds=10;
static Result WaitForServer(const char* ini) {
    DWORD address=0; int port=nia::IsvrPort;
    if(!FindServer(ini,address,port))
        return Fail(nia::StepServer,true,"No NetISO server is set up","The launcher found no server address for this game.",
                    "Put the PC's IP address in Hdd1:\\netiso.xex.txt, as the NetISO plugin reads it,",
                    "or server=<address> in this game's netiso.ini.");
    char where[32]; nia::AddressText(address,where,sizeof(where));
    EnterCriticalSection(&ShownLock); strcpy_s(Shown.server,where); LeaveCriticalSection(&ShownLock);
    if(!StartNetwork(0)) return Fail(nia::StepServer,true,"The Xbox's network didn't start","XNetStartup or WSAStartup failed.","Reboot the Xbox and try again.");
    const DWORD start=GetTickCount();
    char error[160]="";
    for(;;) {
        if(CancelAsked) return Cancelled;
        XNADDR own;
        if(XNetGetTitleXnAddr(&own)==XNET_GET_XNADDR_PENDING) strcpy_s(error,"The Xbox has no network address yet");
        else {
            nia::IsvrClient client(5);
            if(client.Connect(address,port,3) && client.Ping()) { Note("The server answers."); return Mounted; }
            strcpy_s(error,client.Error());
        }
        DWORD up=GetTickCount()/1000,waited=(GetTickCount()-start)/1000;
        bool booting=up<BootSeconds;
        if(!booting && waited>=RetrySeconds) break;
        if(booting) ShowStep(nia::StepServer,0.05,"The Xbox's network is still starting after the reboot. Waiting for the server... %lu s",waited);
        else ShowStep(nia::StepServer,0.05,"Waiting for the NetISO server at %s... %lu s",where,waited);
        if(!Pause(1000)) return Cancelled;
    }
    Note("Can't reach the NetISO server: %s",error);
    char line0[128],line2[128];
    _snprintf_s(line0,sizeof(line0),_TRUNCATE,"Nothing answered at %s, port %d.",where,port);
    _snprintf_s(line2,sizeof(line2),_TRUNCATE,"are on the same network. The address is in %s.",ServerSource);
    return Fail(nia::StepServer,true,"Can't reach the NetISO server",line0,"Start netiso-server.exe on the PC, and check that the PC and the Xbox",line2);
}

// stop_after= in netiso.ini stops short of the mount and goes back to
// Aurora: after linking the NetIso drive (link), listing its root (root) or
// listing the ISO's folder (iso). It tells which step breaks the Guide.
static const int NoStop=1000;
static int StopAfter(const char* ini) {
    static const char* const steps[]={"link","root","iso"};
    char value[16];
    if(!nia::IniValue(ini,"stop_after",value,sizeof(value)) || !value[0]) return NoStop;
    for(int i=0;i<3;++i) if(!_stricmp(value,steps[i])) return i;
    Note("Unknown stop_after=%s; mounting.",value);
    return NoStop;
}

// The NetISO plugin connects to the server by itself, and until it has, its
// drive lists one entry, "No Server!". Right after a reboot that lasted past
// the point where the launcher could reach the server (seen on the console
// at 1:07 after boot), so the launcher waits for it as for the server.
static const char NoServerEntry[]="No Server!";
static bool WaitForNetIso() {
    const DWORD start=GetTickCount();
    for(;;) {
        int count=ListFolder(IsoRoot);
        if(!FindName(count,NoServerEntry)) return true;
        DWORD up=GetTickCount()/1000,waited=(GetTickCount()-start)/1000;
        if(up>=BootSeconds && waited>=RetrySeconds*3) return false;
        ShowStep(nia::StepMount,0.12,"NetISO is still connecting to the server after the reboot... %lu s",waited);
        if(!Pause(2000)) return false;
    }
}

// The whole mount, on the worker thread: the server, the mount and the disc.
static Result Mount(const char* ini,const Settings& settings) {
    char game[96],line[128],line1[128];
    nia::GameName(settings.iso,game,sizeof(game));
    ShowStep(nia::StepServer,0.02,"Checking the NetISO server");
    if(DiscIsGame(settings)) { Note("The game is already mounted."); return Mounted; }
    Result server=WaitForServer(ini);
    if(server!=Mounted) return server;

    ShowStep(nia::StepMount,0.1,"Asking the server for the disc");
    if(!GetModuleHandle("NetISO.xex"))
        return Fail(nia::StepMount,false,"The NetISO plugin isn't running","NetISO.xex isn't loaded, so there is no NetISO drive to mount from.",
                    "Add it to DashLaunch's plugins in launch.ini and reboot the Xbox.");
    Note(GetModuleHandle("NetIsoTiles.xex") ? "NetIsoTiles.xex is loaded and passed this tile on." : "NetIsoTiles.xex isn't loaded.");
    LONG status=LinkDrive(IsoLink,IsoDevice);
    if(status<0) {
        _snprintf_s(line,sizeof(line),_TRUNCATE,"Linking %s failed (status %08X).",IsoDevice,status);
        return Fail(nia::StepMount,false,"The NetISO drive isn't there",line,"Check that NetISO.xex is in DashLaunch's plugins, and reboot the Xbox.");
    }
    if(!WaitForNetIso()) {
        UnlinkDrive(IsoLink);
        if(CancelAsked) return Cancelled;
        return Fail(nia::StepMount,true,"NetISO can't reach the server","The server answers this launcher, but the NetISO plugin still shows \"No Server!\".",
                    "Right after a reboot it can take a minute or two longer; try again in a moment.");
    }
    int stop=StopAfter(ini);
    char folder[MAX_PATH];
    Found found=FindMountFolder(settings,folder,sizeof(folder),stop);
    bool asked=found==FoundMount && ListMountFolder(folder);
    DWORD error=GetLastError();
    UnlinkDrive(IsoLink);
    if(stop!=NoStop) { Note("Back to Aurora without mounting."); return Cancelled; }
    if(found==FoundNoRoot)
        return Fail(nia::StepMount,true,"The NetISO plugin can't reach the server","The server answers, but the NetISO drive lists no games.",
                    "After a reboot the plugin can take a little longer; try again in a moment.");
    if(found==FoundNotListed) {
        char title[96]; _snprintf_s(title,sizeof(title),_TRUNCATE,"%s isn't on the server",game);
        _snprintf_s(line,sizeof(line),_TRUNCATE,"The server doesn't list %s.",settings.iso);
        return Fail(nia::StepMount,true,title,line,"It may have been renamed, moved or deleted on the PC.");
    }
    if(found==FoundNoMount) {
        _snprintf_s(line,sizeof(line),_TRUNCATE,"The NetISO drive shows %s without its Mount folder.",settings.iso);
        return Fail(nia::StepMount,true,"The NetISO drive looks different",line,"This launcher was made for NetISO.xex 1.0.");
    }
    if(!asked) {
        char title[96]; _snprintf_s(title,sizeof(title),_TRUNCATE,"The server didn't mount %s",game);
        _snprintf_s(line,sizeof(line),_TRUNCATE,"The NetISO drive answered the mount with error %lu.",error);
        return Fail(nia::StepMount,true,title,line,"The server's window on the PC may say why.");
    }
    if(CancelAsked) return Cancelled;

    ShowStep(nia::StepDisc,0.3,"The disc is going in. Waiting for the console to read it");
    int quiet=IniNumber(ini,"quiet_ms",QuietMs);
    if(quiet!=QuietMs) Note("Leaving the disc alone for %d ms (quiet_ms).",quiet);
    for(int waited=0;waited<quiet;waited+=PollMs) {
        ShowStep(nia::StepDisc,0.3+0.4*waited/quiet,"The disc is going in. Waiting for the console to read it");
        if(!Pause(PollMs)) return Cancelled;
    }
    for(int waited=quiet;waited<quiet+MountWaitMs;waited+=PollMs) {
        if(DiscIsGame(settings)) { Note("The disc is the game %d ms after the mount.",waited); return Mounted; }
        // Usually a second or two, at most MountWaitMs: the bar keeps creeping
        // toward 95% rather than crawling along the whole timeout.
        ShowStep(nia::StepDisc,0.95-0.25*exp(-(waited-quiet)/3000.0),"Reading the disc");
        if(!Pause(PollMs)) return Cancelled;
    }
    DWORD titleId=0,mediaId=0;
    if(DiscIds(titleId,mediaId)) {
        Note("The disc is still %08X/%08X after %d ms.",titleId,mediaId,MountWaitMs);
        _snprintf_s(line,sizeof(line),_TRUNCATE,"The disc in the drive is Title ID %08X, not %s's %08X.",titleId,game,settings.titleId);
        return Fail(nia::StepDisc,true,"A different game is in the drive",line,"The ISO may not be the game this tile was made for.");
    }
    Note("No readable disc after %d ms.",MountWaitMs);
    _snprintf_s(line1,sizeof(line1),_TRUNCATE,"The server mounted the game, but the console read no disc within %d s.",MountWaitMs/1000);
    return Fail(nia::StepDisc,true,"The disc didn't show up",line1,"Try again; if it keeps happening, reboot the Xbox.");
}

struct MountJob { const char* ini; const Settings* settings; Result result; };
static DWORD WINAPI MountThread(void* parameter) {
    MountJob* job=static_cast<MountJob*>(parameter);
    job->result=Mount(job->ini,*job->settings);
    return 0;
}
// Mounts on a worker while this thread draws and reads the controller. B
// goes back to Aurora at once, at any step: a worker stuck in a slow listing
// or a wait for the network ends with the title.
static MountJob Job;
static Result RunMount(const char* ini,const Settings& settings) {
    Job.ini=ini; Job.settings=&settings; Job.result=Failed;
    InterlockedExchange(&CancelAsked,0);
    HANDLE thread=CreateThread(NULL,64*1024,MountThread,&Job,0,NULL);
    if(!thread) return Mount(ini,settings);
    while(WaitForSingleObject(thread,0)==WAIT_TIMEOUT) {
        ReadNotifications();
        if(Pressed()&XINPUT_GAMEPAD_B) {
            InterlockedExchange(&CancelAsked,1);
            Note("B pressed: back to Aurora.");
            if(WaitForSingleObject(thread,300)==WAIT_TIMEOUT) { CloseHandle(thread); return Cancelled; }
            break;
        }
        ScreenDraw();
        if(!Screen) Sleep(20);
    }
    CloseHandle(thread);
    return CancelAsked ? Cancelled : Job.result;
}

// Draws until the bar has reached 100%, then keeps the finished screen up
// for holdMs, so the player sees it fill and can read it before the game
// takes over the screen. False if B was pressed meanwhile.
static const int HoldMs=1000;
static bool FinishBar(int holdMs) {
    const DWORD start=GetTickCount();
    DWORD full=0;
    while(Screen) {
        ReadNotifications();
        if(Pressed()&XINPUT_GAMEPAD_B) return false;
        ScreenDraw();
        if(Bar<1 && GetTickCount()-start<2000) continue;
        if(!full) full=GetTickCount();
        else if(GetTickCount()-full>=DWORD(holdMs)) break;
    }
    return true;
}

// Shows Problem until A or B (back to Aurora), X (try again, when that could
// help) or a minute has passed. True to try again.
static const int ErrorSeconds=60;
static bool ShowProblem() {
    if(!Screen) ScreenStart();
    EnterCriticalSection(&ShownLock);
    Shown.step=Problem.step; Shown.canRetry=Problem.retry;
    strcpy_s(Shown.errorTitle,Problem.title);
    for(int i=0;i<3;++i) strcpy_s(Shown.errorLines[i],Problem.lines[i]);
    LeaveCriticalSection(&ShownLock);
    bool again=false;
    for(DWORD start=GetTickCount();;) {
        int left=ErrorSeconds-int((GetTickCount()-start)/1000);
        EnterCriticalSection(&ShownLock); Shown.countdown=left; LeaveCriticalSection(&ShownLock);
        ReadNotifications();
        WORD pressed=Pressed();
        if(pressed&(XINPUT_GAMEPAD_A|XINPUT_GAMEPAD_B) || left<=0 || !Screen) break;
        if(Problem.retry && (pressed&XINPUT_GAMEPAD_X)) { again=true; break; }
        ScreenDraw();
    }
    Note(again ? "X pressed: trying again." : "Back to Aurora.");
    EnterCriticalSection(&ShownLock);
    Shown.errorTitle[0]=0; Shown.canRetry=false; Shown.countdown=0;
    LeaveCriticalSection(&ShownLock);
    return again;
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
    CloseLog();
    LaunchImage(path,0);
    OpenLog(false);
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
    ScreenStop();
    if(!_stricmp(start,"media")) {
        Note("Starting the disc in the drive (start=media)");
        Launch(DetectMedia);
    } else if(_stricmp(start,"nova")) {
        LONG status=LinkDrive(AuroraDiscLink,DiscDevice);
        if(status<0) Note("Can't link %s (status %08X).",AuroraDiscLink,status);
        else { Note("Starting %s, as Aurora's disc entry does",AuroraDiscXex); Launch(AuroraDiscXex); }
    }
    Note("Starting %s from %s, as Nova's file browser does",DiscXexName,DiscMount);
    CloseLog();
    DWORD result=XLaunchNewImageEx(DiscXexName,DiscMount,NULL,NovaLaunchFlags);
    OpenLog(false);
    Note("XamLoaderLaunchTitleEx returned %08X (%lu).",result,GetLastError());
}

// Mounts the game and starts it, showing what goes wrong until the player
// goes back to Aurora. mount_only=1 in netiso.ini mounts and goes back to
// Aurora without starting the game, to tell whether the mount or the start
// breaks something.
static void LaunchGame(const char* ini) {
    Settings settings;
    if(!ReadSettings(ini,settings)) {
        Fail(nia::StepServer,false,"This game's netiso.ini is incomplete","It needs iso= and title_id= lines.","Let netiso-server.exe or NetISO Sync write the folder again.");
        ShowProblem();
        return;
    }
    EnterCriticalSection(&ShownLock);
    strcpy_s(Shown.iso,settings.iso); nia::GameName(settings.iso,Shown.game,sizeof(Shown.game)); Shown.titleId=settings.titleId;
    LeaveCriticalSection(&ShownLock);
    if(LinkDrive(DiscLink,DiscDevice)<0) Note("Can't link %s.",DiscDevice);
    for(;;) {
        Result result=RunMount(ini,settings);
        if(result==Cancelled) break;
        if(result==Mounted) {
            if(IniNumber(ini,"mount_only",0)) { Note("mount_only=1: back to Aurora without starting the game."); break; }
            ShowStep(nia::StepStart,1,"Starting %s",Shown.game);
            if(!FinishBar(IniNumber(ini,"hold_ms",HoldMs))) { Note("B pressed: back to Aurora."); break; }
            UnlinkDrive(DiscLink);
            StartDisc(ini);
            char line[128]; _snprintf_s(line,sizeof(line),_TRUNCATE,"Starting the disc failed (error %lu).",GetLastError());
            Fail(nia::StepStart,true,"The game didn't start",line,"Try again, or start the disc from Aurora.");
            LinkDrive(DiscLink,DiscDevice);
        }
        if(!ShowProblem()) break;
    }
    UnlinkDrive(DiscLink);
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
        if(ParseServer(value,address,port)) { strcpy_s(ServerSource,"this game's netiso.ini"); return true; }
        Note("server=%s in %s is not an IPv4 address.",value,IniFile); return false;
    }
    static const char* plugin[]={"Hdd1:\\netiso.xex.txt","Usb0:\\netiso.xex.txt"};
    for(int i=0;i<int(sizeof(plugin)/sizeof(plugin[0]));++i) {
        char path[nia::PathMax],text[128];
        if(!MapPath(plugin[i],path,sizeof(path)) || !ReadText(path,text,sizeof(text))) continue;
        text[strcspn(text,"\r\n")]=0;
        if(ParseServer(text,address,port)) { Note("Server %s from %s.",text,plugin[i]); strcpy_s(ServerSource,plugin[i]); return true; }
    }
    Note("No server. Put server=<IP address> in %s.",IniFile);
    return false;
}

// addressWaitMs: how long to wait for the console's network address, which
// it may not have yet right after boot. Started once per run.
static bool StartNetwork(int addressWaitMs) {
    static bool started=false;
    if(!started) {
        XNetStartupParams params; memset(&params,0,sizeof(params));
        params.cfgSizeOfStruct=sizeof(params); params.cfgFlags=XNET_STARTUP_BYPASS_SECURITY;
        WSADATA wsa;
        if(XNetStartup(&params) || WSAStartup(MAKEWORD(2,2),&wsa)) { Note("Network startup failed."); return false; }
        started=true;
    }
    XNADDR address;
    for(int waited=0;waited<addressWaitMs;waited+=250) {
        if(XNetGetTitleXnAddr(&address)!=XNET_GET_XNADDR_PENDING) return true;
        Wait(250);
    }
    if(addressWaitMs) { Note("No network address after %d s.",addressWaitMs/1000); return false; }
    return true;
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
    if(!FindServer(ini,address,port) || !StartNetwork(10000)) return;
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
    if(!ok) return;
    ShowStep(nia::StepServer,1,"Done: %d added, %d removed",result.added,result.removed);
    FinishBar(HoldMs);
}

// Where Sync goes when it's done: after= in netiso.ini, or the dashboard.
// When Sync is DashLaunch's Default, after= must name Aurora, or the
// dashboard would be Sync again.
static void LaunchAfterSync(const char* ini) {
    char value[nia::PathMax],path[nia::PathMax];
    if(!nia::IniValue(ini,"after",value,sizeof(value)) || !value[0]) return;
    DevicePath(value,path,sizeof(path));
    Note("Starting %s",path);
    CloseLog();
    UnlinkDrives();
    ScreenStop();
    LaunchImage(path,0);
    OpenLog(false);
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
// Whether this start is the helper's, and if so when the tile was pressed
// (a launcher from before the timer sends no time; then it's now).
static bool StartedAsHelper(DWORD& started) {
    BootLaunch data; DWORD size=0;
    if(XGetLaunchDataSize(&size)!=ERROR_SUCCESS || (size!=sizeof(BootData) && size!=sizeof(data))) return false;
    if(XGetLaunchData(&data,size)!=ERROR_SUCCESS || memcmp(data.text,BootData,sizeof(BootData))) return false;
    if(size==sizeof(data) && GetTickCount()-data.started<60000) started=data.started;
    return true;
}
static void HandToHelper() {
    HANDLE file=CreateFile(BootXex,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file==INVALID_HANDLE_VALUE) { Note("No %s; mounting from the launcher.",BootXex); return; }
    CloseHandle(file);
    BootLaunch data; memcpy(data.text,BootData,sizeof(BootData)); data.started=Started;
    DWORD set=XSetLaunchData(&data,sizeof(data));
    if(set!=ERROR_SUCCESS) { Note("XSetLaunchData failed (%lu); mounting from the launcher.",set); return; }
    Note("Handing the start to %s",BootXex);
    CloseLog();
    LaunchImage(BootXex,0);
    OpenLog(false);
    Note("Couldn't start %s (%lu); mounting from the launcher.",BootXex,GetLastError());
}

void __cdecl main() {
    InitializeCriticalSection(&LogLock);
    InitializeCriticalSection(&ShownLock);
    MainThread=GetCurrentThreadId();
    Notifications=XNotifyCreateListener(XNOTIFY_ALL);
    Started=GetTickCount();
    bool helper=StartedAsHelper(Started);
    OpenLog(true);
    NoteRun();
    if(helper) Note("Started as the helper.");
    static char ini[nia::IniMax];
    if(!ReadText(IniFile,ini,sizeof(ini))) Note("Can't open %s.",IniFile);
    static char iso[MAX_PATH];
    bool game=nia::IniValue(ini,"iso",iso,sizeof(iso)) && iso[0];
    // The tile's launcher hands over before it draws anything: a frame of the
    // loading screen here, then the black of the switch, then the helper's
    // screen showed as a flicker (seen on the console). The game's name is
    // there from the first frame, so it doesn't pop in either.
    if(game && !helper) HandToHelper();
    if(game) nia::GameName(iso,Shown.game,sizeof(Shown.game));
    else {
        strcpy_s(Shown.game,"NetISO Sync");
        ShowStep(nia::StepServer,0.1,"Updating the game folders from the NetISO server");
    }
    ScreenStart();
    ScreenDraw();
    if(game) {
        LaunchGame(ini);
    } else {
        Note("NetISO Sync");
        Sync(ini);
        LaunchAfterSync(ini);
    }
    UnlinkDrives();
    CloseLog();
    ScreenStop();
    // A game that didn't start goes back to the default app, which DashLaunch
    // points at its Default (Aurora). Sync goes to the dashboard: it may be
    // the Default itself, and would start again.
    LaunchImage(game ? XLAUNCH_KEYWORD_DEFAULT_APP : XLAUNCH_KEYWORD_DASH,0);
}
