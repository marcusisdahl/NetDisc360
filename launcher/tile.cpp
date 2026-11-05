// SPDX-License-Identifier: MIT
// The tile launcher: the default.xex in each game folder. NetDisc360 or
// NetISO Sync puts the game's execution info (Title ID, Media ID, disc
// number) in its header, so Aurora lists the folder as that game and finds
// its name and art. Started, it hands the start to the helper, the full
// launcher that all the game folders share (see shared/helper.h), with this
// folder in the launch data. It draws nothing: the helper's loading screen is
// the first thing on screen, as when the full launcher handed over to its
// copy in the same folder.
//
// The folder is the path this XEX was started from, which the kernel keeps
// in the running title's loader entry (xboxkrnl's XexExecutableModuleHandle,
// export 403, which the NetDisc360 plugin reads the same way).
//
// The same launcher is the default.xex of a game's Games on Demand tile (see
// GodTiles in server/main.cpp): a package that holds only it and a
// netiso.ini whose tile= names the game folder it stands for. Started from
// there (game: is the package, which can't be written), it starts that
// folder's default.xex by a drive name, which the console then starts as
// Aurora starts a tile: the plugin sees a tile's start, and this launcher,
// in the folder, hands over to the helper.
#include "../shared/helper.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

struct KernelString { USHORT length,capacity; char* text; };
extern "C" LONG __stdcall ObCreateSymbolicLink(KernelString* link,KernelString* device);
extern "C" LONG __stdcall ObDeleteSymbolicLink(KernelString* link);
extern "C" LONG __stdcall XexGetModuleHandle(const char* name,HANDLE* module);
extern "C" LONG __stdcall XexGetProcedureAddress(HANDLE module,DWORD ordinal,void** address);
// XLaunchNewImage is declared DECLSPEC_NORETURN, but returns when the launch
// fails; through this pointer the code after it stays.
typedef VOID (WINAPI* LaunchFunction)(LPCSTR,DWORD);
static LaunchFunction volatile LaunchImage=XLaunchNewImage;
// xam.xex 656, called the way Aurora shows its own notices (see plugin/main.cpp).
typedef void (*QueueNotice)(DWORD type,DWORD user,unsigned __int64 areas,const wchar_t* text,void* context);

struct WideString { USHORT length,capacity; wchar_t* text; };
struct LoaderEntry { LIST_ENTRY links[3]; void* headers; void* base; DWORD size; WideString fullName,baseName; };

static char LogFile[nia::BootFolderMax+16]="game:\\netiso.log";
static const char FolderHelper[]="game:\\boot.xex",PackageIni[]="game:\\netiso.ini";
// Drive names the path may come with, and their devices (as in main.cpp).
static const struct { const char* name; const char* device; } Drives[]={
    {"Hdd1:","\\Device\\Harddisk0\\Partition1"},{"Usb0:","\\Device\\Mass0"},{"Usb1:","\\Device\\Mass1"},
};

// One line in the game folder's log, which the helper goes on with.
static void Note(const char* format,...) {
    FILE* log=NULL;
    if(fopen_s(&log,LogFile,"a")) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(log,"%02u:%02u:%02u.%03u Tile launcher: ",t.wHour,t.wMinute,t.wSecond,t.wMilliseconds);
    va_list args; va_start(args,format); vfprintf(log,format,args); va_end(args);
    fputs("\n",log);
    fclose(log);
}

static void* Export(const char* module,DWORD ordinal) {
    HANDLE handle=NULL; void* address=NULL;
    if(XexGetModuleHandle(module,&handle)<0 || XexGetProcedureAddress(handle,ordinal,&address)<0) return NULL;
    return address;
}

// This XEX's folder as a device path; false if the kernel's path isn't one
// the helper could reach.
static bool OwnFolder(char* folder,size_t capacity) {
    LoaderEntry* const* executable=(LoaderEntry* const*)Export("xboxkrnl.exe",403);
    const LoaderEntry* entry=executable ? *executable : NULL;
    if(!entry || !entry->fullName.text) { Note("The kernel has no path for this XEX."); return false; }
    char path[nia::BootFolderMax];
    size_t length=entry->fullName.length/2;
    if(length>=sizeof(path)) { Note("This XEX's path is too long."); return false; }
    for(size_t i=0;i<length;++i) path[i]=char(entry->fullName.text[i]);
    path[length]=0;
    char* name=strrchr(path,'\\');
    if(!name) { Note("This XEX's path has no folder: %s",path); return false; }
    *name=0;
    for(int i=0;i<int(sizeof(Drives)/sizeof(Drives[0]));++i) {
        size_t drive=strlen(Drives[i].name);
        if(!_strnicmp(path,Drives[i].name,drive)) { _snprintf_s(folder,capacity,_TRUNCATE,"%s%s",Drives[i].device,path+drive); return true; }
    }
    if(path[0]!='\\') { Note("This XEX's path isn't on a drive the helper can reach: %s",path); return false; }
    strcpy_s(folder,capacity,path);
    return true;
}

static bool Exists(const char* path) {
    return GetFileAttributes(path)!=DWORD(-1);  // INVALID_FILE_ATTRIBUTES, which the Xbox headers don't have.
}
// The shared helper in the folder at a device path, as a path through a
// drive linked to that folder ("" if it isn't there). Started by its device
// path, the helper never ran and the console went to the dashboard (seen on
// the console), as a disc's default.xex did by \Device\Cdrom0 (see
// main.cpp); by a drive name, like game:\boot.xex before, it starts. The
// link stays for the launch, which resolves it while this title still runs.
static bool LinkHelper(const char* device,char* path,size_t capacity) {
    char name[]="\\??\\NIsoS:",target[nia::BootFolderMax+32];
    strcpy_s(target,device);
    KernelString link={USHORT(strlen(name)),USHORT(sizeof(name)),name};
    KernelString folder={USHORT(strlen(target)),USHORT(sizeof(target)),target};
    ObDeleteSymbolicLink(&link);
    if(ObCreateSymbolicLink(&link,&folder)<0) { Note("Can't link %s.",device); return false; }
    _snprintf_s(path,capacity,_TRUNCATE,"NIsoS:\\%s",nia::HelperXex);
    if(Exists(path)) return true;
    ObDeleteSymbolicLink(&link);
    return false;
}

// A Games on Demand tile's netiso.ini: tile= the game folder
// (Hdd1:\NetDisc360\GOD\Halo 3); false, with nothing set, when there is
// none, as in a game folder, whose netiso.ini names the game's disc instead.
static bool PackageTile(char* folder,size_t capacity) {
    FILE* ini=NULL;
    if(fopen_s(&ini,PackageIni,"rb")) return false;
    char text[1024];
    size_t size=fread(text,1,sizeof(text)-1,ini);
    fclose(ini);
    text[size]=0;
    for(char* line=text;*line;) {
        char* end=line+strcspn(line,"\r\n");
        char next=*end; *end=0;
        if(!_strnicmp(line,"tile=",5) && line[5]) { strcpy_s(folder,capacity,line+5); return true; }
        if(!next) break;
        line=end+1;
    }
    return false;
}

// Starts the game folder's default.xex, linking its drive name first: a
// title started from the Xbox's own menu may have no Hdd1: (the kernel
// keeps one link of a name, so one that's there stays). Returns only if the
// start failed.
static void StartFolderTile(const char* folder) {
    _snprintf_s(LogFile,sizeof(LogFile),_TRUNCATE,"%s\\netiso.log",folder);
    for(int i=0;i<int(sizeof(Drives)/sizeof(Drives[0]));++i) {
        size_t drive=strlen(Drives[i].name);
        if(_strnicmp(folder,Drives[i].name,drive)) continue;
        char name[16],device[64];
        _snprintf_s(name,sizeof(name),_TRUNCATE,"\\??\\%s",Drives[i].name);
        strcpy_s(device,Drives[i].device);
        KernelString link={USHORT(strlen(name)),USHORT(sizeof(name)),name};
        KernelString target={USHORT(strlen(device)),USHORT(sizeof(device)),device};
        ObCreateSymbolicLink(&link,&target);
    }
    char xex[nia::BootFolderMax+16];
    _snprintf_s(xex,sizeof(xex),_TRUNCATE,"%s\\default.xex",folder);
    if(!Exists(xex)) { Note("Started as Games on Demand, but %s isn't there.",xex); return; }
    Note("Started as Games on Demand: starting %s",xex);
    LaunchImage(xex,0);
    Note("Couldn't start %s (%lu).",xex,GetLastError());
}

static void Show(const wchar_t* text) {
    QueueNotice notice=(QueueNotice)Export("xam.xex",656);
    if(notice) notice(0x22,0xFF,1,text,NULL);
}

void __cdecl main() {
    static char tile[nia::BootFolderMax];
    if(PackageTile(tile,sizeof(tile))) {
        StartFolderTile(tile);
        Show(L"NetDisc360 couldn't start this game: its folder on the console is missing. Start NetDisc360 on your PC to put it back.");
        Sleep(500);
        LaunchImage(XLAUNCH_KEYWORD_DEFAULT_APP,0);
        return;
    }
    static nia::BootLaunch data;
    memset(&data,0,sizeof(data));
    memcpy(data.text,nia::BootData,sizeof(nia::BootData));
    data.started=GetTickCount();
    // The shared helper, in NetDisc360's folder in Hdd1:\Apps, or in
    // HelperFolder next to this game folder (game folders that aren't on the
    // hard drive, and those in Hdd1:\NetISO before); else the folder's own
    // boot.xex, from before there was a shared one, which reads the folder it
    // is in and takes the launch data without one.
    char helper[64]; helper[0]=0;
    DWORD size=sizeof(data);
    if(OwnFolder(data.folder,sizeof(data.folder))) {
        const char* name=strrchr(data.folder,'\\');
        char shared[nia::BootFolderMax+32];
        if(LinkHelper(nia::AppDevice,helper,sizeof(helper))) Note("The helper is %s\\%s.",nia::AppDevice,nia::HelperXex);
        else {
            _snprintf_s(shared,sizeof(shared),_TRUNCATE,"%.*s\\%s",name ? int(name-data.folder) : 0,data.folder,nia::HelperFolder);
            if(name && LinkHelper(shared,helper,sizeof(helper))) Note("The helper is %s\\%s.",shared,nia::HelperXex);
            else { Note("No %s in %s or %s.",nia::HelperXex,nia::AppDevice,shared); helper[0]=0; }
        }
    }
    if(!helper[0] && Exists(FolderHelper)) { strcpy_s(helper,FolderHelper); size=nia::BootLaunchBefore; }
    if(helper[0]) {
        DWORD set=XSetLaunchData(&data,size);
        if(set==ERROR_SUCCESS) {
            Note("Handing the start to %s",helper);
            LaunchImage(helper,0);
            Note("Couldn't start %s (%lu).",helper,GetLastError());
        }
        else Note("XSetLaunchData failed (%lu).",set);
    }
    // Nothing to hand over to: back to Aurora, saying why.
    Show(L"NetDisc360 couldn't start this game: its helper is missing. Start NetDisc360 on your PC to put it back.");
    Sleep(500);
    LaunchImage(XLAUNCH_KEYWORD_DEFAULT_APP,0);
}
