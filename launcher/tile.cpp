// SPDX-License-Identifier: MIT
// The tile launcher: the default.xex in each game folder. NetDisc 360 or
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
// export 403, which the NetIsoTiles plugin reads the same way).
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

static const char LogFile[]="game:\\netiso.log",FolderHelper[]="game:\\boot.xex";
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
// Whether file is in the folder at a device path, which the Win32 calls
// don't take: looked at through a link.
static bool ExistsIn(const char* device,const char* file) {
    char name[]="\\??\\NIsoS:",target[nia::BootFolderMax+32];
    strcpy_s(target,device);
    KernelString link={USHORT(strlen(name)),USHORT(sizeof(name)),name};
    KernelString folder={USHORT(strlen(target)),USHORT(sizeof(target)),target};
    ObDeleteSymbolicLink(&link);
    if(ObCreateSymbolicLink(&link,&folder)<0) { Note("Can't link %s.",device); return false; }
    char path[64]; _snprintf_s(path,sizeof(path),_TRUNCATE,"NIsoS:\\%s",file);
    bool found=Exists(path);
    ObDeleteSymbolicLink(&link);
    return found;
}

static void Show(const wchar_t* text) {
    QueueNotice notice=(QueueNotice)Export("xam.xex",656);
    if(notice) notice(0x22,0xFF,1,text,NULL);
}

void __cdecl main() {
    static nia::BootLaunch data;
    memset(&data,0,sizeof(data));
    memcpy(data.text,nia::BootData,sizeof(nia::BootData));
    data.started=GetTickCount();
    // The shared helper, in HelperFolder next to this game folder, started
    // by its device path, as the next title resolves it without our links;
    // else the folder's own boot.xex, from before there was a shared one,
    // which reads the folder it is in and takes the launch data without one.
    char helper[nia::BootFolderMax+32]; helper[0]=0;
    DWORD size=sizeof(data);
    if(OwnFolder(data.folder,sizeof(data.folder))) {
        const char* name=strrchr(data.folder,'\\');
        char games[nia::BootFolderMax]; _snprintf_s(games,sizeof(games),_TRUNCATE,"%.*s",name ? int(name-data.folder) : 0,data.folder);
        char file[64]; _snprintf_s(file,sizeof(file),_TRUNCATE,"%s\\%s",nia::HelperFolder,nia::HelperXex);
        if(name && ExistsIn(games,file)) _snprintf_s(helper,sizeof(helper),_TRUNCATE,"%s\\%s",games,file);
        else Note("No %s in %s.",file,games);
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
    Show(L"NetDisc 360 couldn't start this game: its helper is missing. Start NetDisc 360 on your PC to put it back.");
    Sleep(500);
    LaunchImage(XLAUNCH_KEYWORD_DEFAULT_APP,0);
}
