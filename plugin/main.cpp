// NetISO Tiles: a DashLaunch plugin that starts NetISO game tiles from inside
// Aurora.
//
// Whenever the game launcher (launcher/main.cpp) asked the NetISO plugin to
// mount, the Guide was dead until the next reboot: right after the mount,
// back in Aurora and in the game, with or without Nova.xex, whatever started
// the game (seen on the console). The same mount from Aurora's file browser,
// done while Aurora runs, keeps the Guide. So this plugin does the mount from
// Aurora, before Aurora goes away: it hooks xam's two launch calls, which
// Aurora.xex 0.7b.2 starts tiles with (XamLoaderLaunchTitle, ordinal 420,
// through XLaunchNewImage, and XamLoaderLaunchTitleEx, 421). When the XEX to
// start is default.xex in a folder whose netiso.ini names an ISO, it mounts
// that ISO on Aurora's thread, reaching the plugin's folders by their device
// path as Aurora's file browser does, waits until the disc is the game, and
// starts Dvd:\default.xex the way Aurora's disc entry does. Every other launch, and
// a tile it can't mount, goes on to xam unchanged, so the tile's launcher
// still runs as before when the plugin can't help.
#include <xtl.h>
#include <ppcintrinsics.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

struct KernelString { USHORT length,capacity; char* text; };
extern "C" LONG __stdcall ObCreateSymbolicLink(KernelString* link,KernelString* device);
extern "C" LONG __stdcall ObDeleteSymbolicLink(KernelString* link);
// The kernel's file calls, for device paths (\Device\...), which the Win32
// ones don't take.
struct ObjectAttributes { HANDLE root; KernelString* name; ULONG attributes; };
struct IoStatusBlock { LONG status; ULONG_PTR information; };
struct DirectoryInfo { ULONG next,index; LARGE_INTEGER created,accessed,written,changed,size,allocated; ULONG attributes,nameLength; char name[MAX_PATH]; };
extern "C" LONG __stdcall NtOpenFile(HANDLE* handle,ACCESS_MASK access,ObjectAttributes* attributes,IoStatusBlock* status,ULONG share,ULONG options);
extern "C" LONG __stdcall NtQueryDirectoryFile(HANDLE handle,HANDLE event,void* apc,void* context,IoStatusBlock* status,void* info,ULONG length,KernelString* pattern,BOOLEAN restart);
extern "C" LONG __stdcall NtReadFile(HANDLE handle,HANDLE event,void* apc,void* context,IoStatusBlock* status,void* buffer,ULONG length,LARGE_INTEGER* offset);
extern "C" LONG __stdcall NtClose(HANDLE handle);
static const LONG NoMoreFiles=LONG(0x80000006);
extern "C" LONG __stdcall XexGetModuleHandle(const char* name,HANDLE* module);
extern "C" LONG __stdcall XexGetProcedureAddress(HANDLE module,DWORD ordinal,void** address);

typedef DWORD (*LaunchTitle)(const char* path,DWORD flags);
typedef DWORD (*LaunchTitleEx)(const char* path,const char* mountPath,const char* commandLine,DWORD flags);
// xam.xex 656, called the way Aurora shows its own notices (type 0x22, any
// user, area 1).
typedef void (*QueueNotice)(DWORD type,DWORD user,unsigned __int64 areas,const wchar_t* text,void* context);
static const DWORD LaunchTitleOrdinal=420,LaunchTitleExOrdinal=421,QueueNoticeOrdinal=656;
static QueueNotice Notice=NULL;

static const char IniName[]="netiso.ini",LogName[]="netiso.log",GameXex[]="default.xex";
// The plugin's folders and the disc are reached by their device paths, the
// way Aurora's file browser reaches them, with no drive links of our own.
// The launcher's mount, through drive links it created and Win32 listings,
// killed the Guide every time while the file browser's never did (seen on
// the console), so this keeps to the browser's way of asking.
static const char IsoDevice[]="\\Device\\NetIso",DiscDevice[]="\\Device\\Cdrom0";
static const char DiscXex[]="\\Device\\Cdrom0\\default.xex";
// A tile given as a device path (\Device\...) is read through this link to its folder.
static const char TileLink[]="\\??\\NIsoT:",TileRoot[]="NIsoT:";
// Aurora's disc entry links Dvd: to \Device\Cdrom0 and starts the disc with
// XLaunchNewImage("Dvd:\default.xex",0), which is XamLoaderLaunchTitle with
// flags 0 (read from Aurora.xex 0.7b.2).
static const char AuroraDiscLink[]="\\??\\Dvd:",AuroraDiscXex[]="Dvd:\\default.xex";
static const char MountFolder[]="Mount";
// After the mount the plugin fakes a tray open and close on its own thread,
// and the console then reads the new disc. Reading the disc meanwhile broke
// the game the launcher started, so the disc is left alone this long first.
static const int QuietMs=5000,MountWaitMs=30000,PollMs=500,MaxDepth=4,MaxNames=256;

// ---- Hooks ----

// Jump through r11: lis r11,high; ori r11,r11,low; mtctr r11; bctr.
static void WriteJump(DWORD* at,DWORD target) {
    at[0]=0x3D600000|(target>>16);
    at[1]=0x616B0000|(target&0xFFFF);
    at[2]=0x7D6903A6;
    at[3]=0x4E800420;
}
static void FlushCode(void* at,DWORD size) {
    // The RXDK's ppcintrinsics.h has neither __icbi nor __isync; isync is emitted by its opcode.
    for(DWORD offset=0;offset<size;offset+=4) { __dcbst(offset,at); __sync(); }
    __sync(); __emit(0x4C00012C); // isync
}

// Code space for the two trampolines and for a copy of the compiler's
// register save routine, which xam functions often call (bl) in their first
// instructions. Written at start-up; the numbers only keep each body
// different, so the linker can't fold them into one.
static __declspec(naked) void LaunchTitleStub() {
    __asm {
        li r3,0x101
        li r3,0x102
        li r3,0x103
        li r3,0x104
        li r3,0x105
        li r3,0x106
        li r3,0x107
        li r3,0x108
    }
}
static __declspec(naked) void LaunchTitleExStub() {
    __asm {
        li r3,0x201
        li r3,0x202
        li r3,0x203
        li r3,0x204
        li r3,0x205
        li r3,0x206
        li r3,0x207
        li r3,0x208
    }
}
static __declspec(naked) void SaveRegisters() {
    __asm {
        li r3,0x301
        li r3,0x302
        li r3,0x303
        li r3,0x304
        li r3,0x305
        li r3,0x306
        li r3,0x307
        li r3,0x308
        li r3,0x309
        li r3,0x30A
        li r3,0x30B
        li r3,0x30C
        li r3,0x30D
        li r3,0x30E
        li r3,0x30F
        li r3,0x310
        li r3,0x311
        li r3,0x312
        li r3,0x313
        li r3,0x314
    }
}
// __savegprlr_14..31: std r14,-0x98(r1) ... std r31,-0x10(r1), then
// stw r12,-8(r1) and blr. Entering at rN saves rN..r31 and the return address.
static void WriteSaveRegisters() {
    DWORD* code=(DWORD*)SaveRegisters;
    for(DWORD r=14;r<=31;++r) code[r-14]=0xF8010000|(r<<21)|((0x10000-0x98+(r-14)*8)&0xFFFC);
    code[18]=0x91810000|(0x10000-8);
    code[19]=0x4E800020;
    FlushCode(code,20*4);
}

struct Hook { DWORD* function; DWORD original[4]; DWORD* stub; bool on; };
static Hook LaunchTitleHook={0},LaunchTitleExHook={0};

static const DWORD Bctr=0x4E800420;
static DWORD BranchTarget(DWORD instruction,const DWORD* at) {
    LONG offset=LONG(instruction&0x03FFFFFC);
    if(offset&0x02000000) offset-=0x04000000;
    return DWORD((const BYTE*)at+offset);
}
// A relative bl among the copied instructions is a call to xam's own copy of
// the register save routine; it becomes a call to ours. Returns 0 for other
// relative branches, which a copy can't run from elsewhere.
static DWORD Relocate(DWORD instruction,const DWORD* from,const DWORD* to) {
    DWORD opcode=instruction>>26;
    if(opcode==16) return 0; // bc
    if(opcode!=18) return instruction;
    if((instruction&3)!=1) return 0;
    DWORD target=*(const DWORD*)BranchTarget(instruction,from);
    const DWORD* ours=(const DWORD*)SaveRegisters;
    for(int i=0;i<19;++i) if(ours[i]==target) return 0x48000001|(DWORD((const BYTE*)&ours[i]-(const BYTE*)to)&0x03FFFFFC);
    return 0;
}
// The stub sets ctr to the function's fifth instruction (lis/addi/mtctr r11,
// before the copies, which may set r11 for the rest of the function), runs
// the first four, and branches there with bctr. A b as the fourth (a wrapper
// that goes on into another function) becomes that bctr, to b's target. A
// function that ends within its first three instructions is too short for
// the four-instruction jump and isn't hooked.
static bool HookStart(Hook& hook,DWORD ordinal,void* destination,void* stub) {
    HANDLE xam=NULL; void* address=NULL;
    if(XexGetModuleHandle("xam.xex",&xam)<0 || XexGetProcedureAddress(xam,ordinal,&address)<0 || !address) return false;
    DWORD* function=(DWORD*)address; DWORD* code=(DWORD*)stub;
    DWORD next=DWORD(&function[4]);
    for(int i=0;i<4;++i) {
        DWORD instruction=function[i],opcode=instruction>>26;
        bool jump=opcode==18 && (instruction&3)==0;
        DWORD kind=(instruction>>1)&0x3FF; // bclr 16, bcctr 528
        bool leaves=jump || (opcode==19 && (kind==16||kind==528) && (instruction&0x02800000)==0x02800000); // b, blr, bctr
        if(leaves && i<3) return false;
        if(jump) { next=BranchTarget(instruction,&function[i]); code[3+i]=Bctr; continue; }
        DWORD moved=Relocate(instruction,&function[i],&code[3+i]);
        if(!moved) return false;
        code[3+i]=moved;
    }
    code[0]=0x3D600000|(((next>>16)+((next&0x8000)?1:0))&0xFFFF);
    code[1]=0x396B0000|(next&0xFFFF);
    code[2]=0x7D6903A6;
    code[7]=Bctr;
    FlushCode(code,8*4);
    memcpy(hook.original,function,sizeof(hook.original));
    hook.function=function; hook.stub=code;
    WriteJump(function,DWORD(destination));
    FlushCode(function,4*4);
    hook.on=true;
    return true;
}
static void Unhook(Hook& hook) {
    if(!hook.on) return;
    memcpy(hook.function,hook.original,sizeof(hook.original));
    FlushCode(hook.function,4*4);
    hook.on=false;
}

// ---- Log ----

// Appended to the tile's netiso.log, in the launcher's format.
static HANDLE Log=INVALID_HANDLE_VALUE;
static char LogLine[1024];
static void Write(const char* text) {
    if(Log==INVALID_HANDLE_VALUE) return;
    DWORD done=0; WriteFile(Log,text,strlen(text),&done,NULL);
}
static void Note(const char* format,...) {
    if(Log==INVALID_HANDLE_VALUE) return;
    SYSTEMTIME t; GetLocalTime(&t);
    int length=sprintf_s(LogLine,"%02u:%02u:%02u.%03u ",t.wHour,t.wMinute,t.wSecond,t.wMilliseconds);
    va_list args; va_start(args,format);
    int more=_vsnprintf_s(LogLine+length,sizeof(LogLine)-length-2,_TRUNCATE,format,args);
    va_end(args);
    length+=more<0 ? int(strlen(LogLine+length)) : more;
    strcpy_s(LogLine+length,sizeof(LogLine)-length,"\r\n");
    Write(LogLine);
}
static void OpenLog(const char* folder) {
    char path[MAX_PATH]; sprintf_s(path,"%s\\%s",folder,LogName);
    Log=CreateFile(path,GENERIC_WRITE,FILE_SHARE_READ,NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(Log==INVALID_HANDLE_VALUE) return;
    SetFilePointer(Log,0,NULL,FILE_END);
    SYSTEMTIME t; GetLocalTime(&t);
    DWORD up=GetTickCount()/1000;
    Write("\r\n");
    Note("==== %04u-%02u-%02u, NetISO Tiles plugin built %s %s, console up %lu:%02lu:%02lu ====",
         t.wYear,t.wMonth,t.wDay,__DATE__,__TIME__,up/3600,up/60%60,up%60);
}
static void CloseLog() {
    if(Log!=INVALID_HANDLE_VALUE) { CloseHandle(Log); Log=INVALID_HANDLE_VALUE; }
}

// A notice on screen, like Aurora's own.
static void Tell(const char* text) {
    Note("Notice: %s",text);
    if(!Notice) return;
    static wchar_t wide[128];
    int i=0;
    for(;text[i] && i<127;++i) wide[i]=wchar_t((unsigned char)text[i]);
    wide[i]=0;
    Notice(0x22,0xFF,1,wide,NULL);
}

// ---- Mounting ----

static LONG LinkDrive(const char* name,const char* device) {
    static char nameText[32],deviceText[MAX_PATH]; strcpy_s(nameText,name); strcpy_s(deviceText,device);
    KernelString link={USHORT(strlen(nameText)),USHORT(sizeof(nameText)),nameText};
    KernelString target={USHORT(strlen(deviceText)),USHORT(sizeof(deviceText)),deviceText};
    ObDeleteSymbolicLink(&link);
    return ObCreateSymbolicLink(&link,&target);
}
static void UnlinkDrive(const char* name) {
    static char nameText[32]; strcpy_s(nameText,name);
    KernelString link={USHORT(strlen(nameText)),USHORT(sizeof(nameText)),nameText};
    ObDeleteSymbolicLink(&link);
}

static bool ReadText(const char* path,char* text,DWORD capacity) {
    text[0]=0;
    HANDLE file=CreateFile(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file==INVALID_HANDLE_VALUE) return false;
    DWORD done=0; BOOL ok=ReadFile(file,text,capacity-1,&done,NULL);
    CloseHandle(file);
    text[ok ? done : 0]=0;
    return ok!=FALSE;
}
// Same rules as nia::IniValue in shared/library.cpp.
static bool IniValue(const char* text,const char* key,char* value,size_t capacity) {
    size_t keyLength=strlen(key);
    for(const char* line=text;line && *line;) {
        const char* end=line+strcspn(line,"\r\n");
        if(size_t(end-line)>keyLength && line[keyLength]=='=' && !_strnicmp(line,key,keyLength)) {
            size_t length=size_t(end-line)-keyLength-1;
            if(length>=capacity) return false;
            memcpy(value,line+keyLength+1,length); value[length]=0;
            return true;
        }
        line=end+strspn(end,"\r\n");
    }
    return false;
}

struct Settings { char iso[MAX_PATH]; DWORD titleId,mediaId; };
static bool ReadSettings(const char* ini,Settings& settings) {
    memset(&settings,0,sizeof(settings));
    char value[32];
    IniValue(ini,"iso",settings.iso,sizeof(settings.iso));
    if(IniValue(ini,"title_id",value,sizeof(value))) settings.titleId=strtoul(value,NULL,16);
    if(IniValue(ini,"media_id",value,sizeof(value))) settings.mediaId=strtoul(value,NULL,16);
    return settings.iso[0] && settings.titleId;
}

// Opens a folder or a file by its device path, read-only and synchronous.
static LONG OpenPath(const char* path,bool folder,HANDLE& handle) {
    char text[MAX_PATH]; strcpy_s(text,path);
    KernelString name={USHORT(strlen(text)),USHORT(sizeof(text)),text};
    ObjectAttributes attributes={NULL,&name,0x40}; // OBJ_CASE_INSENSITIVE
    IoStatusBlock status={0,0};
    handle=NULL;
    // FILE_LIST_DIRECTORY or FILE_READ_DATA (both 1) with SYNCHRONIZE;
    // FILE_DIRECTORY_FILE or FILE_NON_DIRECTORY_FILE, with FILE_SYNCHRONOUS_IO_NONALERT.
    return NtOpenFile(&handle,0x100001,&attributes,&status,folder ? 3 : 1,folder ? 0x21 : 0x60);
}

// Title ID and Media ID from the execution info header (0x40006) of the
// default.xex on the disc that is in the drive now.
static DWORD Header[4096]; // The XEX header, 16 KB; its directory is at the start.
static bool DiscIds(DWORD& titleId,DWORD& mediaId) {
    HANDLE file;
    if(OpenPath(DiscXex,false,file)<0) return false;
    IoStatusBlock status={0,0}; LARGE_INTEGER offset; offset.QuadPart=0;
    LONG read=NtReadFile(file,NULL,NULL,NULL,&status,Header,sizeof(Header),&offset);
    NtClose(file);
    if(read<0 || status.information<24 || memcmp(Header,"XEX2",4)) return false;
    DWORD count=Header[5];
    for(DWORD i=0;i<count&&i<64;++i) {
        if(24+i*8+8>status.information) break;
        if(Header[6+i*2]!=0x40006) continue;
        DWORD at=Header[7+i*2]; // media ID, version, base version, title ID
        if(at+16>status.information) break;
        mediaId=Header[at/4]; titleId=Header[at/4+3];
        return true;
    }
    return false;
}
static bool DiscIsGame(const Settings& settings) {
    DWORD titleId=0,mediaId=0;
    if(!DiscIds(titleId,mediaId)) return false;
    return titleId==settings.titleId&&(!settings.mediaId||mediaId==settings.mediaId);
}

static char Names[MaxNames][MAX_PATH];
static DirectoryInfo Entry;
// Lists a folder by device path into Names; the number of names, or -1 when
// it can't be opened. Each query returns one entry.
static int ListFolder(const char* folder) {
    HANDLE handle;
    LONG status=OpenPath(folder,true,handle);
    if(status<0) { Note("%s: can't open (status %08X)",folder,status); return -1; }
    int count=0;
    for(;;) {
        IoStatusBlock io={0,0};
        LONG result=NtQueryDirectoryFile(handle,NULL,NULL,NULL,&io,&Entry,sizeof(Entry),NULL,FALSE);
        if(result<0) { if(result!=NoMoreFiles) Note("%s: listing stopped (status %08X)",folder,result); break; }
        DWORD length=Entry.nameLength<MAX_PATH-1 ? Entry.nameLength : MAX_PATH-1;
        if(count<MaxNames) { memcpy(Names[count],Entry.name,length); Names[count][length]=0; ++count; }
    }
    NtClose(handle);
    Note("%s: %d entries",folder,count);
    return count;
}
static const char* FindName(int count,const char* name) {
    for(int i=0;i<count;++i) if(!_stricmp(Names[i],name)) return Names[i];
    return NULL;
}
// Walks from the root to the ISO's Mount folder by name, as the launcher and
// Aurora's file browser do. Listing these folders doesn't mount. isoFolder
// gets the ISO's own folder.
static bool FindMountFolder(const Settings& settings,char* folder,size_t capacity,char* isoFolder) {
    strcpy_s(folder,capacity,IsoDevice);
    isoFolder[0]=0;
    for(int depth=0;depth<MaxDepth;++depth) {
        int count=ListFolder(folder);
        if(count<0) return false;
        const char* next=FindName(count,settings.iso);
        if(!next && (next=FindName(count,MountFolder))!=NULL) {
            strcat_s(folder,capacity,"\\"); strcat_s(folder,capacity,next);
            return true;
        }
        if(!next) return false;
        strcat_s(folder,capacity,"\\"); strcat_s(folder,capacity,next);
        strcpy_s(isoFolder,capacity,folder);
    }
    return false;
}
// Opening and listing a Mount folder mounts: the plugin swaps the disc before
// the listing returns (empty), then fakes the tray open and close. With hold,
// the folder stays open (in MountHandle) until the caller closes it.
static HANDLE MountHandle=NULL;
static bool ListMountFolder(const char* folder,bool hold) {
    HANDLE handle;
    LONG status=OpenPath(folder,true,handle);
    if(status<0) { Note("%s: can't open (status %08X)",folder,status); return false; }
    IoStatusBlock io={0,0};
    LONG result=NtQueryDirectoryFile(handle,NULL,NULL,NULL,&io,&Entry,sizeof(Entry),NULL,FALSE);
    Note("%s: listed (status %08X)",folder,result);
    if(hold) MountHandle=handle; else NtClose(handle);
    return true;
}
static void CloseMountFolder() {
    if(!MountHandle) return;
    NtClose(MountHandle); MountHandle=NULL;
    Note("Closed the Mount folder (hold_mount).");
}

static char Folder[MAX_PATH],Ini[2048];
// Whole number from the tile's netiso.ini, or fallback.
static int IniNumber(const char* key,int fallback) {
    char value[16];
    if(!IniValue(Ini,key,value,sizeof(value)) || !value[0]) return fallback;
    return atoi(value);
}

// Two test switches in a game folder's netiso.ini, to find what Aurora's file
// browser does around its mount that keeps the Guide: hold_mount=1 keeps the
// Mount folder open until the disc is the game, and relist=1 lists the ISO's
// folder and the root again right after the mount, as a user leaving the
// Mount folder would.
static bool Mount(const Settings& settings) {
    if(DiscIsGame(settings)) { Note("The game is already mounted."); return true; }
    if(!GetModuleHandle("NetISO.xex")) Note("NetISO.xex isn't loaded.");
    bool hold=IniNumber("hold_mount",0)!=0,relist=IniNumber("relist",0)!=0;
    char folder[MAX_PATH],isoFolder[MAX_PATH];
    bool found=FindMountFolder(settings,folder,sizeof(folder),isoFolder);
    if(!found) { Tell("NetISO: the server doesn't list this game. Is it running?"); return false; }
    if(!ListMountFolder(folder,hold)) { Tell("NetISO couldn't mount this game."); return false; }
    if(relist) {
        Note("Listing the ISO's folder and the root again (relist).");
        if(isoFolder[0]) ListFolder(isoFolder);
        ListFolder(IsoDevice);
    }
    Sleep(QuietMs);
    for(int waited=QuietMs;waited<QuietMs+MountWaitMs;waited+=PollMs) {
        if(DiscIsGame(settings)) { Note("The disc is the game %d ms after the mount.",waited); CloseMountFolder(); return true; }
        Sleep(PollMs);
    }
    CloseMountFolder();
    DWORD titleId=0,mediaId=0;
    if(DiscIds(titleId,mediaId)) Note("The disc is still %08X/%08X after %d ms.",titleId,mediaId,QuietMs+MountWaitMs);
    else Note("No readable disc after %d ms.",QuietMs+MountWaitMs);
    Tell("NetISO: the game didn't show up in the drive.");
    return false;
}

// ---- Tiles ----

// Whether xex (from mountPath, if given) is a NetISO tile: default.xex next to
// a netiso.ini with iso= and title_id=. Folder is then where to read them.
static bool IsTile(const char* xex,const char* mountPath,Settings& settings) {
    if(!xex) return false;
    char path[MAX_PATH];
    if(mountPath && mountPath[0]) sprintf_s(path,"%s\\%s",mountPath,xex); else strcpy_s(path,xex);
    char* name=strrchr(path,'\\');
    if(!name || _stricmp(name+1,GameXex)) return false;
    *name=0;
    // A device path is read through a link to its folder.
    if(path[0]=='\\') {
        if(LinkDrive(TileLink,path)<0) return false;
        strcpy_s(Folder,TileRoot);
    } else strcpy_s(Folder,path);
    char ini[MAX_PATH]; sprintf_s(ini,"%s\\%s",Folder,IniName);
    if(!ReadText(ini,Ini,sizeof(Ini)) || !ReadSettings(Ini,settings)) { UnlinkDrive(TileLink); return false; }
    OpenLog(Folder);
    Note("Tile %s",path);
    Note("ISO %s, Title ID %08X, Media ID %08X",settings.iso,settings.titleId,settings.mediaId);
    return true;
}

// The launches the plugin makes itself go straight to xam, also when one of
// xam's two calls goes on through the other. A launch that works doesn't
// return (the title ends), so this is the thread and the time, not a flag to
// clear: another thread, or the same one much later, is a new launch.
static volatile DWORD SkipThread=0,SkipSince=0;
static void Skip(bool on) {
    SkipSince=GetTickCount();
    SkipThread=on ? GetCurrentThreadId() : 0;
}
static bool Skipping() {
    return SkipThread==GetCurrentThreadId() && GetTickCount()-SkipSince<60000;
}
// One tile at a time.
static volatile LONG Busy=0;

// Mounts the tile's ISO and starts the disc. Returns only if it couldn't;
// the caller then starts the tile itself.
static void StartTile(const Settings& settings) {
    bool mounted=Mount(settings);
    LONG status=mounted ? LinkDrive(AuroraDiscLink,DiscDevice) : 0;
    if(status<0) Note("Can't link %s (status %08X).",AuroraDiscLink,status);
    if(mounted && status>=0) {
        Note("Starting %s, as Aurora's disc entry does",AuroraDiscXex);
        CloseLog();
        Skip(true);
        DWORD result=((LaunchTitle)LaunchTitleHook.stub)(AuroraDiscXex,0);
        Skip(false);
        OpenLog(Folder);
        Note("XamLoaderLaunchTitle returned %08X.",result);
    }
    Note("Starting the tile's own launcher instead.");
    CloseLog();
    UnlinkDrive(TileLink);
}
// Tries a tile launch the plugin's way. Returns true when it was a tile the
// plugin couldn't start; the caller then lets xam start the tile as asked.
static bool Handled(const char* path,const char* mountPath) {
    if(Skipping() || InterlockedCompareExchange(&Busy,1,0)) return false;
    Settings settings;
    bool tile=IsTile(path,mountPath,settings);
    if(tile) StartTile(settings);
    InterlockedExchange(&Busy,0);
    if(tile) Skip(true);
    return tile;
}
static DWORD LaunchTitleHooked(const char* path,DWORD flags) {
    bool handled=Handled(path,NULL);
    DWORD result=((LaunchTitle)LaunchTitleHook.stub)(path,flags);
    if(handled) Skip(false);
    return result;
}
static DWORD LaunchTitleExHooked(const char* path,const char* mountPath,const char* commandLine,DWORD flags) {
    bool handled=Handled(path,mountPath);
    DWORD result=((LaunchTitleEx)LaunchTitleExHook.stub)(path,mountPath,commandLine,flags);
    if(handled) Skip(false);
    return result;
}

BOOL APIENTRY DllMain(HANDLE module,DWORD reason,LPVOID reserved) {
    UNREFERENCED_PARAMETER(module); UNREFERENCED_PARAMETER(reserved);
    if(reason==DLL_PROCESS_ATTACH) {
        HANDLE xam=NULL; void* address=NULL;
        if(XexGetModuleHandle("xam.xex",&xam)>=0 && XexGetProcedureAddress(xam,QueueNoticeOrdinal,&address)>=0) Notice=(QueueNotice)address;
        WriteSaveRegisters();
        // Both or neither: a launch must not go through one hook into the other half-done.
        if(HookStart(LaunchTitleHook,LaunchTitleOrdinal,LaunchTitleHooked,LaunchTitleStub)
           && !HookStart(LaunchTitleExHook,LaunchTitleExOrdinal,LaunchTitleExHooked,LaunchTitleExStub))
            Unhook(LaunchTitleHook);
    } else if(reason==DLL_PROCESS_DETACH) {
        Unhook(LaunchTitleExHook);
        Unhook(LaunchTitleHook);
    }
    return TRUE;
}
