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
// path, waits until the disc is the game, and starts Dvd:\default.xex the way
// Aurora's disc entry does. It also keeps NetISO from calling
// XamAppUnloadSelf for mounts that don't come from the Guide (see below). Every other launch, and
// a tile it can't mount, goes on to xam unchanged, so the tile's launcher
// still runs as before when the plugin can't help.
//
// That unload call turned out to be what killed the Guide, and with it
// skipped the launcher's own mount keeps the Guide too (seen on the console).
// So tiles now go to their launcher, which shows the loading screen and can
// be cancelled at every step; tiles=aurora in a game's netiso.ini still
// mounts from inside Aurora.
//
// A game started from a tile that asks for another of its discs gets that
// disc's ISO mounted (see Disc swaps below), and NetISO's read size and
// receive buffer are raised at start-up (see NetIsoTiles.ini below).
#include <xtl.h>
#include <ppcintrinsics.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include "../shared/isvr.h"

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
extern "C" LONG __stdcall NtCreateFile(HANDLE* handle,ACCESS_MASK access,ObjectAttributes* attributes,IoStatusBlock* status,LARGE_INTEGER* size,ULONG fileAttributes,ULONG share,ULONG disposition,ULONG options);
extern "C" LONG __stdcall NtWriteFile(HANDLE handle,HANDLE event,void* apc,void* context,IoStatusBlock* status,const void* buffer,ULONG length,LARGE_INTEGER* offset);
extern "C" LONG __stdcall KeSetEvent(void* event,LONG increment,BOOLEAN wait);
static const LONG NoMoreFiles=LONG(0x80000006);
extern "C" LONG __stdcall ExCreateThread(HANDLE* handle,DWORD stackSize,DWORD* threadId,void* apiStartup,LPTHREAD_START_ROUTINE start,void* parameter,DWORD flags);
static const DWORD SystemThread=2; // ExCreateThread: run in the system process
extern "C" LONG __stdcall XexGetModuleHandle(const char* name,HANDLE* module);
extern "C" UCHAR __stdcall KeGetCurrentProcessType();
extern "C" BOOLEAN __stdcall MmIsAddressValid(void* address);
static const UCHAR SystemProcess=2;
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
static const int QuietMs=5000,MountWaitMs=30000,PollMs=500,MaxNames=256;

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
// The log is a handle of Aurora's process, which the system thread doing
// the mount can't write to, so its lines wait here until it's done.
static volatile DWORD DeferThread=0;
static char Deferred[16384];
static size_t DeferredLength=0;
static bool Deferring() { return DeferThread && DeferThread==GetCurrentThreadId(); }
static void Write(const char* text) {
    if(Log==INVALID_HANDLE_VALUE) return;
    if(Deferring()) {
        size_t length=strlen(text);
        if(DeferredLength+length<sizeof(Deferred)) { memcpy(Deferred+DeferredLength,text,length); DeferredLength+=length; }
        return;
    }
    DWORD done=0; WriteFile(Log,text,strlen(text),&done,NULL);
}
// Writes the system thread's lines once it has finished.
static void WriteDeferred() {
    DeferThread=0;
    if(!DeferredLength) return;
    Deferred[DeferredLength]=0; DeferredLength=0;
    Write(Deferred);
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
static void Show(const char* text) {
    if(!Notice) return;
    static wchar_t wide[128];
    int i=0;
    for(;text[i] && i<127;++i) wide[i]=wchar_t((unsigned char)text[i]);
    wide[i]=0;
    Notice(0x22,0xFF,1,wide,NULL);
}
// Logged and shown. One the system thread asks for waits in Pending and is
// shown from Aurora's thread.
static char Pending[128];
static void Tell(const char* text) {
    Note("Notice: %s",text);
    if(Deferring()) strcpy_s(Pending,text); else Show(text);
}

// ---- NetISO's unload call ----

// Right after every mount request, NetISO.xex 1.0 calls XamAppUnloadSelf
// (xam 581) on the thread that asked (0x91104dc4, read from the binary).
// Asked from the file browser opened from the Guide, that closes the Guide,
// as NetISO's readme describes. Every mount asked from anywhere else (the
// launcher, the helper, this plugin on Aurora's thread) left the Guide dead
// until reboot, and so did a failed one (seen on the console); that this
// call is why is inferred, since there is no Guide app to close there. So
// NetISO's import of it is pointed here: the plugin's own mount thread and
// title threads skip it, and system threads such as the Guide's still get it.
static const DWORD AppUnloadSelfOrdinal=581;
static DWORD* const UnloadThunk=(DWORD*)0x9110c12c;
static const DWORD* const UnloadCall=(const DWORD*)0x91104dc0;
static DWORD UnloadThunkCode[4];
typedef DWORD (*AppUnload)(DWORD,DWORD,DWORD,DWORD);
static AppUnload AppUnloadSelf=NULL;
static volatile LONG UnloadsSkipped=0;
static DWORD UnloadHooked(DWORD a,DWORD b,DWORD c,DWORD d) {
    if(KeGetCurrentProcessType()!=SystemProcess || Deferring()) { InterlockedIncrement(&UnloadsSkipped); return 0; }
    return AppUnloadSelf(a,b,c,d);
}
// Points NetISO's import of XamAppUnloadSelf at UnloadHooked, once. Checks
// that NetISO 1.0 is where it was read: the mount call and the unload call
// in its directory handler, and a thunk that jumps to xam's export.
static const char* PatchUnload() {
    if(AppUnloadSelf) return NULL;
    HANDLE module=NULL; void* target=NULL;
    if(XexGetModuleHandle("NetISO.xex",&module)<0) return "NetISO.xex isn't loaded";
    if(XexGetModuleHandle("xam.xex",&module)<0 || XexGetProcedureAddress(module,AppUnloadSelfOrdinal,&target)<0) return "no XamAppUnloadSelf in xam";
    if(!MmIsAddressValid((void*)UnloadCall) || !MmIsAddressValid(UnloadThunk)) return "NetISO isn't at 0x91100000";
    if(UnloadCall[0]!=0x4BFFFDC1 || UnloadCall[1]!=0x48007369) return "NetISO's mount code isn't 1.0's";
    const DWORD* t=UnloadThunk;
    if((t[0]&0xFFFF0000)!=0x3D600000 || t[2]!=0x7D6903A6 || t[3]!=0x4E800420) return "NetISO's import isn't a thunk";
    DWORD high=(t[0]&0xFFFF)<<16,low=t[1]&0xFFFF,jump=0;
    if((t[1]>>16)==0x616B) jump=high|low; // ori r11,r11,low
    else if((t[1]>>16)==0x396B) jump=high+DWORD(int(short(low))); // addi r11,r11,low
    if(jump!=DWORD(target)) return "NetISO's import doesn't lead to XamAppUnloadSelf";
    memcpy(UnloadThunkCode,UnloadThunk,sizeof(UnloadThunkCode));
    AppUnloadSelf=(AppUnload)target;
    WriteJump(UnloadThunk,DWORD(UnloadHooked));
    FlushCode(UnloadThunk,sizeof(UnloadThunkCode));
    return NULL;
}
static void UnpatchUnload() {
    if(!AppUnloadSelf) return;
    memcpy(UnloadThunk,UnloadThunkCode,sizeof(UnloadThunkCode));
    FlushCode(UnloadThunk,sizeof(UnloadThunkCode));
    AppUnloadSelf=NULL;
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

// Title ID, Media ID and disc number from the execution info header
// (0x40006) of the default.xex on the disc that is in the drive now.
static DWORD Header[4096]; // The XEX header, 16 KB; its directory is at the start.
static bool DiscIds(DWORD& titleId,DWORD& mediaId,int* disc=NULL) {
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
        DWORD at=Header[7+i*2]; // media ID, version, base version, title ID, platform, type, disc, discs
        if(at+20>status.information) break;
        mediaId=Header[at/4]; titleId=Header[at/4+3];
        if(disc) *disc=int((Header[at/4+4]>>8)&0xFF);
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
// One directory query, returning one entry. The first names the pattern *
// and restarts the scan, as FindFirstFile does.
static LONG QueryFolder(HANDLE handle,bool first) {
    static char all[]="*";
    KernelString pattern={1,sizeof(all),all};
    IoStatusBlock io={0,0};
    return NtQueryDirectoryFile(handle,NULL,NULL,NULL,&io,&Entry,sizeof(Entry),first ? &pattern : NULL,first);
}
// Opens a folder with a trailing backslash, as FindFirstFile and the Guide's
// file browser open it (the server saw both mounts as <iso>\Mount\). Without
// it NetISO's drive answers for the folder above: listing the root never
// finished (Aurora hung), \Device\NetIso\<iso> listed the ISO's own name, and
// listing <iso>\Mount returned nothing and didn't mount; all seen on the
// console.
static LONG OpenFolder(const char* folder,HANDLE& handle) {
    char path[MAX_PATH]; sprintf_s(path,"%s\\",folder);
    return OpenPath(path,true,handle);
}
// Lists a folder by device path into Names; the number of names, or -1 when
// it can't be opened. Stops at MaxNames, or if an entry comes back twice in a
// row.
static int ListFolder(const char* folder) {
    HANDLE handle;
    LONG status=OpenFolder(folder,handle);
    if(status<0) { Note("%s: can't open (status %08X)",folder,status); return -1; }
    int count=0;
    for(;;) {
        LONG result=QueryFolder(handle,count==0);
        if(result<0) { if(result!=NoMoreFiles) Note("%s: listing stopped (status %08X)",folder,result); break; }
        if(count==MaxNames) { Note("%s: more than %d entries; using the first %d.",folder,MaxNames,MaxNames); break; }
        DWORD length=Entry.nameLength<MAX_PATH-1 ? Entry.nameLength : MAX_PATH-1;
        memcpy(Names[count],Entry.name,length); Names[count][length]=0;
        if(count && !strcmp(Names[count],Names[count-1])) { Note("%s: %s came back twice; stopping.",folder,Names[count]); break; }
        ++count;
    }
    NtClose(handle);
    Note("%s: %d entries",folder,count);
    return count;
}
static const char* FindName(int count,const char* name) {
    for(int i=0;i<count;++i) if(!_stricmp(Names[i],name)) return Names[i];
    return NULL;
}
// The ISO's Mount folder, \Device\NetIso\<iso>\Mount, after listing the root
// and the ISO's folder on the way, as Aurora's file browser does. Listing
// these folders doesn't mount. isoFolder gets the ISO's own folder.
static bool FindMountFolder(const Settings& settings,char* folder,size_t capacity,char* isoFolder) {
    isoFolder[0]=0;
    int count=ListFolder(IsoDevice);
    if(count<0) return false;
    const char* iso=FindName(count,settings.iso);
    if(!iso) { Note("The server doesn't list %s.",settings.iso); return false; }
    sprintf_s(isoFolder,capacity,"%s\\%s",IsoDevice,iso);
    count=ListFolder(isoFolder);
    if(count>=0 && !FindName(count,MountFolder)) Note("%s doesn't list %s; asking for it anyway.",isoFolder,MountFolder);
    sprintf_s(folder,capacity,"%s\\%s",isoFolder,MountFolder);
    return true;
}
// Opening and listing a Mount folder mounts: the plugin swaps the disc before
// the listing returns (empty), then fakes the tray open and close. With hold,
// the folder stays open (in MountHandle) until the caller closes it.
static HANDLE MountHandle=NULL;
static bool ListMountFolder(const char* folder,bool hold) {
    HANDLE handle;
    LONG status=OpenFolder(folder,handle);
    if(status<0) { Note("%s: can't open (status %08X)",folder,status); return false; }
    LONG result=QueryFolder(handle,true);
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

// ---- Server ----

// "192.168.1.20" or "192.168.1.20:4323", as in the launcher.
static bool ParseServer(const char* text,DWORD& address,int& port) {
    char host[64]; strcpy_s(host,text);
    char* colon=strchr(host,':');
    if(colon) { *colon=0; port=atoi(colon+1); if(port<1||port>65535) return false; }
    size_t length=strlen(host);
    while(length && (host[length-1]==' '||host[length-1]=='\t')) host[--length]=0;
    address=inet_addr(host);
    return address!=INADDR_NONE && address!=0;
}
static bool ReadDeviceText(const char* path,char* text,DWORD capacity) {
    text[0]=0;
    HANDLE file;
    if(OpenPath(path,false,file)<0) return false;
    IoStatusBlock status={0,0}; LARGE_INTEGER offset; offset.QuadPart=0;
    LONG read=NtReadFile(file,NULL,NULL,NULL,&status,text,capacity-1,&offset);
    NtClose(file);
    if(read<0) return false;
    text[status.information]=0;
    return true;
}
// server= in the tile's netiso.ini, or the NetISO plugin's own netiso.xex.txt
// on Hdd1: or Usb0:, as the launcher finds it.
static bool FindServer(DWORD& address,int& port) {
    char value[64];
    if(IniValue(Ini,"server",value,sizeof(value))) {
        if(ParseServer(value,address,port)) return true;
        Note("server=%s in %s is not an IPv4 address.",value,IniName); return false;
    }
    static const char* const plugin[]={"\\Device\\Harddisk0\\Partition1\\netiso.xex.txt","\\Device\\Mass0\\netiso.xex.txt"};
    for(int i=0;i<int(sizeof(plugin)/sizeof(plugin[0]));++i) {
        char text[128];
        if(!ReadDeviceText(plugin[i],text,sizeof(text))) continue;
        text[strcspn(text,"\r\n")]=0;
        if(ParseServer(text,address,port)) { Note("Server %s from %s.",text,plugin[i]); return true; }
    }
    Note("No server address in %s or netiso.xex.txt.",IniName);
    return false;
}
// Whether the NetISO server answers, asked before anything touches the disc
// drive or the NetISO drive. For a minute or two after boot the console can't
// reach it, and a tile started then froze Aurora for 14 s on the NetISO drive
// and killed the Guide without mounting anything (seen on the console).
// Uses Aurora's own network, which its FTP server runs on.
enum ServerState { ServerUnknown, ServerUp, ServerDown };
static ServerState CheckServer(char* where,size_t capacity) {
    DWORD address=0; int port=nia::IsvrPort;
    if(!FindServer(address,port)) { Note("Not checking the server."); return ServerUnknown; }
    nia::AddressText(address,where,capacity);
    WSADATA wsa;
    if(WSAStartup(MAKEWORD(2,2),&wsa)) { Note("Aurora's network isn't started; not checking the server."); return ServerUnknown; }
    ServerState state=ServerUnknown;
    SOCKET probe=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if(probe==INVALID_SOCKET) Note("No socket from Aurora's network (%d); not checking the server.",WSAGetLastError());
    else {
        closesocket(probe);
        nia::IsvrClient client(3);
        bool up=client.Connect(address,port,2) && client.Ping();
        if(up) Note("The server answers."); else Note("The server doesn't answer: %s",client.Error());
        client.Close();
        state=up ? ServerUp : ServerDown;
    }
    WSACleanup();
    return state;
}

// tiles_stop= in a game folder's netiso.ini stops the plugin after one step
// and goes back to Aurora, with no notice and without the tile's launcher, to
// find the step that kills the Guide: after the server check (server),
// reading the disc drive (disc), listing the NetISO drive's root (root),
// finding the ISO's Mount folder (iso), or the mount, before the start (mount).
enum Step { StepServer, StepDisc, StepRoot, StepIso, StepMount, NoStop };
static int StopStep() {
    static const char* const steps[]={"server","disc","root","iso","mount"};
    char value[16];
    if(!IniValue(Ini,"tiles_stop",value,sizeof(value)) || !value[0]) return NoStop;
    for(int i=0;i<NoStop;++i) if(!_stricmp(value,steps[i])) { Note("Test: stopping after %s (tiles_stop).",steps[i]); return i; }
    Note("Unknown tiles_stop=%s; not stopping.",value);
    return NoStop;
}

// What the plugin made of a tile.
enum Outcome {
    Started,    // the disc is the game; start it
    Fallback,   // couldn't mount; let the tile's launcher try
    Cancelled   // back to Aurora, starting nothing
};

// Two test switches in a game folder's netiso.ini, to find what Aurora's file
// browser does around its mount that keeps the Guide: hold_mount=1 keeps the
// Mount folder open until the disc is the game, and relist=1 lists the ISO's
// folder and the root again right after the mount, as a user leaving the
// Mount folder would.
static Outcome MountSteps(const Settings& settings,int stop) {
    bool game=DiscIsGame(settings);
    Note(game ? "The game is already mounted." : "The disc isn't the game.");
    if(stop==StepDisc) return Cancelled;
    if(game) return Started;
    if(!GetModuleHandle("NetISO.xex")) Note("NetISO.xex isn't loaded.");
    if(stop==StepRoot) { ListFolder(IsoDevice); return Cancelled; }
    bool hold=IniNumber("hold_mount",0)!=0,relist=IniNumber("relist",0)!=0;
    char folder[MAX_PATH],isoFolder[MAX_PATH];
    bool found=FindMountFolder(settings,folder,sizeof(folder),isoFolder);
    if(!found) { Tell("NetDisc 360: the server doesn't list this game. Is it running?"); return Fallback; }
    if(stop==StepIso) return Cancelled;
    LONG skipped=UnloadsSkipped;
    if(!ListMountFolder(folder,hold)) { Tell("NetDisc 360 couldn't mount this game."); return Fallback; }
    Note("NetISO's unload calls skipped during the mount: %ld.",UnloadsSkipped-skipped);
    if(relist) {
        Note("Listing the ISO's folder and the root again (relist).");
        if(isoFolder[0]) ListFolder(isoFolder);
        ListFolder(IsoDevice);
    }
    Sleep(QuietMs);
    for(int waited=QuietMs;waited<QuietMs+MountWaitMs;waited+=PollMs) {
        if(DiscIsGame(settings)) { Note("The disc is the game %d ms after the mount.",waited); CloseMountFolder(); return stop==StepMount ? Cancelled : Started; }
        Sleep(PollMs);
    }
    CloseMountFolder();
    DWORD titleId=0,mediaId=0;
    if(DiscIds(titleId,mediaId)) Note("The disc is still %08X/%08X after %d ms.",titleId,mediaId,QuietMs+MountWaitMs);
    else Note("No readable disc after %d ms.",QuietMs+MountWaitMs);
    Tell("NetDisc 360: the game didn't show up in the drive.");
    return Fallback;
}

// tiles_thread=system in netiso.ini runs the drive steps on a system thread
// while Aurora's thread waits, as a request from the Guide comes from the
// system process. NetISO doesn't look at the requester's process in its
// mount path apart from the unload call above (read from the binary), so
// this is only a test switch.
struct MountJob { const Settings* settings; int stop; Outcome outcome; };
static MountJob Job;
static DWORD WINAPI MountThread(void* job) {
    DeferThread=GetCurrentThreadId(); // Aurora's thread is waiting meanwhile.
    MountJob* mount=(MountJob*)job;
    mount->outcome=MountSteps(*mount->settings,mount->stop);
    return 0;
}
static const DWORD MountThreadMs=QuietMs+MountWaitMs+30000;
// Set while a system thread that overran its time may still be running.
static volatile bool ThreadOverran=false;
static Outcome MountOnSystemThread(const Settings& settings,int stop) {
    if(ThreadOverran) { Note("An earlier mount thread may still be running; not starting another."); return Fallback; }
    Job.settings=&settings; Job.stop=stop; Job.outcome=Fallback;
    Pending[0]=0;
    Note("Mounting from a system thread.");
    HANDLE thread=NULL; DWORD id=0;
    LONG status=ExCreateThread(&thread,0x10000,&id,NULL,MountThread,&Job,SystemThread);
    if(status<0) { Note("No system thread (status %08X); mounting from Aurora's thread.",status); return MountSteps(settings,stop); }
    DWORD waited=WaitForSingleObject(thread,MountThreadMs);
    CloseHandle(thread);
    if(waited!=WAIT_OBJECT_0) {
        // It still uses Job and keeps its lines to itself.
        ThreadOverran=true;
        Note("The system thread didn't finish in %lu ms.",MountThreadMs);
        Tell("NetDisc 360: the mount didn't finish.");
        return Cancelled;
    }
    WriteDeferred();
    if(Pending[0]) Show(Pending);
    return Job.outcome;
}

static Outcome Mount(const Settings& settings) {
    int stop=StopStep();
    char where[16]="";
    if(CheckServer(where,sizeof(where))==ServerDown) {
        static char text[96];
        sprintf_s(text,"NetDisc 360 server %s isn't answering. Try again in a minute.",where);
        Tell(text);
        return Cancelled;
    }
    if(stop==StepServer) return Cancelled;
    const char* unpatched=PatchUnload();
    if(unpatched) Note("Can't skip NetISO's unload call: %s.",unpatched);
    char thread[16];
    if(IniValue(Ini,"tiles_thread",thread,sizeof(thread)) && !_stricmp(thread,"system")) return MountOnSystemThread(settings,stop);
    return MountSteps(settings,stop);
}

// ---- NetISO's read size and receive buffer ----

// Both are set in NetIsoTiles.ini on the root of Usb0: or Hdd1:, the first
// one found. Returns which file set key, or NULL.
static const char* TuningValue(const char* key,char* value,size_t capacity) {
    static const char* const files[]={"\\Device\\Mass0\\NetIsoTiles.ini","\\Device\\Harddisk0\\Partition1\\NetIsoTiles.ini"};
    for(int i=0;i<int(sizeof(files)/sizeof(files[0]));++i) {
        char text[512];
        if(!ReadDeviceText(files[i],text,sizeof(text))) continue;
        return IniValue(text,key,value,capacity) ? files[i] : NULL;
    }
    return NULL;
}

// NetISO.xex 1.0 splits every disc read into 4 KB requests and sends the next
// only when the last one's data is in (its read loop at 0x911050cc, read from
// the binary). Each request costs a round trip, about 0.6 ms through a USB
// adapter, so games loaded at about 7 MB/s however fast the link was (seen on
// the console). It receives straight into the caller's buffer, so a bigger
// limit only means fewer requests; NetDisc serves up to 16 MB in one.
// read_kb= sets the limit: 4 (NetISO's own), 16, 64, 256 or 1024.
static DWORD* const ReadLimit=(DWORD*)0x911050e0;
// cmplwi cr6,r4,0x1000; blt cr6,+8; li r4,0x1000; bl ReadData
static const DWORD ReadLimitCode[4]={0x2B041000,0x41980008,0x38801000,0x4BFFE52D};
static const int DefaultReadKb=64;
static char ReadLimitNote[160];
// Set once at start-up, before NetISO has a disc to read, since the three
// instructions can't change at once under a read in progress; and left set
// when this plugin unloads, since they don't lead into it.
static void PatchReadLimit() {
    char value[16];
    const char* from=TuningValue("read_kb",value,sizeof(value));
    int kb=from ? atoi(value) : DefaultReadKb;
    if(!from) from="default";
    if(kb!=4 && kb!=16 && kb!=64 && kb!=256 && kb!=1024) {
        sprintf_s(ReadLimitNote,"read_kb=%d in %s isn't 4, 16, 64, 256 or 1024, so 4 KB per request (NetISO's own)",kb,from);
        return;
    }
    HANDLE module=NULL;
    if(XexGetModuleHandle("NetISO.xex",&module)<0) { strcpy_s(ReadLimitNote,"4 KB per request; NetISO.xex wasn't loaded before this plugin"); return; }
    if(!MmIsAddressValid(ReadLimit) || memcmp(ReadLimit,ReadLimitCode,sizeof(ReadLimitCode))) {
        strcpy_s(ReadLimitNote,"left alone; NetISO's read loop isn't 1.0's"); return;
    }
    if(kb==4) { sprintf_s(ReadLimitNote,"4 KB per request, NetISO's own (%s)",from); return; }
    DWORD bytes=DWORD(kb)*1024,shift=0;
    while((1u<<shift)<bytes) ++shift;
    // srwi. r0,r4,shift; beq +8; li or lis r4,bytes
    ReadLimit[0]=0x5480003F|((32-shift)<<11)|(shift<<6);
    ReadLimit[1]=0x41820008;
    ReadLimit[2]=bytes<0x8000 ? 0x38800000|bytes : 0x3C800000|(bytes>>16);
    FlushCode(ReadLimit,3*4);
    sprintf_s(ReadLimitNote,"up to %d KB per request (%s)",kb,from);
}

// Before connecting, NetISO sets only option 0x5801 on its socket, through
// NetDll_setsockopt (its helper at 0x91103000, read from the binary), so its
// receive buffer is XNet's default: 16 KB by the XDK's XNetStartupParams,
// not measured. The server can't have more than that in flight, which would
// hold a read near 16 KB per round trip, however big the request. rcv_kb=
// asks XNet for a bigger buffer on each connection NetISO makes from then on:
// 16, 32, 64, 128 or 256, 64 when left out; 0 leaves NetISO's socket as it
// was. A GTA V install through a USB adapter with 256 KB reads went from
// 18 MB/s to 27 MB/s with 64 KB (seen on the console).
static DWORD* const SocketOptionCall=(DWORD*)0x91103030;
static const DWORD SocketOptionCode=0x4800905D; // bl 0x9110c08c, NetISO's import of NetDll_setsockopt
static const DWORD* const SocketOptionThunk=(const DWORD*)0x9110c08c;
typedef int (*SetSocketOption)(DWORD caller,SOCKET s,int level,int name,const char* value,int length);
static int ReceiveBytes=0;
static volatile LONG ReceiveSets=0,ReceiveRefusals=0;
static const int DefaultReceiveKb=64;
static char ReceiveNote[160]="XNet's default";
static int SetSocketOptionHooked(DWORD caller,SOCKET s,int level,int name,const char* value,int length) {
    SetSocketOption set=(SetSocketOption)SocketOptionThunk;
    int result=set(caller,s,level,name,value,length);
    int bytes=ReceiveBytes;
    if(bytes) InterlockedIncrement(set(caller,s,SOL_SOCKET,SO_RCVBUF,(const char*)&bytes,sizeof(bytes)) ? &ReceiveRefusals : &ReceiveSets);
    return result;
}
// One word, so it can change while NetISO runs; NetISO connects only once
// the network is up, after this.
static void PatchReceiveBuffer() {
    char value[16];
    const char* from=TuningValue("rcv_kb",value,sizeof(value));
    int kb=DefaultReceiveKb;
    if(from && value[0]) kb=atoi(value); else from="default";
    if(kb==0) { sprintf_s(ReceiveNote,"XNet's default (rcv_kb=0 in %s)",from); return; }
    if(kb!=16 && kb!=32 && kb!=64 && kb!=128 && kb!=256) {
        sprintf_s(ReceiveNote,"XNet's default; rcv_kb=%d in %s isn't 16, 32, 64, 128 or 256",kb,from);
        return;
    }
    HANDLE module=NULL;
    if(XexGetModuleHandle("NetISO.xex",&module)<0) { strcpy_s(ReceiveNote,"XNet's default; NetISO.xex wasn't loaded before this plugin"); return; }
    if(!MmIsAddressValid(SocketOptionCall) || !MmIsAddressValid((void*)SocketOptionThunk) || *SocketOptionCall!=SocketOptionCode
       || (SocketOptionThunk[0]&0xFFFF0000)!=0x3D600000 || SocketOptionThunk[2]!=0x7D6903A6 || SocketOptionThunk[3]!=0x4E800420) {
        strcpy_s(ReceiveNote,"XNet's default; NetISO's socket setup isn't 1.0's"); return;
    }
    LONG distance=LONG(DWORD(SetSocketOptionHooked)-DWORD(SocketOptionCall));
    if(distance<-0x2000000 || distance>=0x2000000) { strcpy_s(ReceiveNote,"XNet's default; this plugin is out of NetISO's branch range"); return; }
    ReceiveBytes=kb*1024;
    *SocketOptionCall=0x48000001|(DWORD(distance)&0x03FFFFFC);
    FlushCode(SocketOptionCall,4);
    sprintf_s(ReceiveNote,"%d KB asked (%s)",kb,from);
}
static void UnpatchReceiveBuffer() {
    if(!ReceiveBytes) return;
    *SocketOptionCall=SocketOptionCode;
    FlushCode(SocketOptionCall,4);
    ReceiveBytes=0;
}

// ---- Disc swaps ----

// A game asks for another of its discs with XSwapDisc, xam.xex 2600 (disc
// number, a kernel event to set once that disc is in, and a third argument
// only checked for NULL; read from Nova.xex). When Aurora starts a game whose
// disc is 1 of 2 or more, it gives Nova.xex the other discs of that Title ID
// in its list, and Nova points the game's import of 2600 at its own swap: for
// a disc that is a folder it links the game's GAME: and D: to that folder. A
// tile's folder holds only its launcher, so GTA V hung at "Loading story
// mode" after its install disc asked for the play disc (seen on the console;
// that Nova did the swap is inferred). So for a game started from a tile,
// this plugin points that import at SwapDiscHooked instead, once the game is
// loaded and after Nova's change: it mounts the other disc's ISO, waits until
// the drive has it, and sets the event, as a player swapping the disc would.
// The discs are the tile folders next to the tile's own with the same Title
// ID in netiso.ini, numbered by their launcher's execution info, as Aurora
// finds them. Every other call goes on to what the import led to before.
static const DWORD SwapDiscOrdinal=2600;
static const int MaxDiscs=5;
// How long the plugin waits for a game it can hook; the game's own restarts
// on the same disc set fall within it too.
static const DWORD SwapIdleMs=120000;
typedef DWORD (*SwapDisc)(DWORD disc,void* event,void* extra);
static __declspec(naked) void SwapPreviousStub() {
    __asm {
        li r3,0x401
        li r3,0x402
        li r3,0x403
        li r3,0x404
        li r3,0x405
        li r3,0x406
        li r3,0x407
        li r3,0x408
    }
}
// The tile a launch came from, for the disc start that follows it.
static char TileDevice[MAX_PATH];
static struct { char folder[MAX_PATH]; DWORD titleId,at; } LastTile;
static bool TileFresh() { return LastTile.folder[0] && GetTickCount()-LastTile.at<10*60*1000; }
// The armed game: its tile folder, Title ID and discs (by number), set on
// the launching title's thread before the watcher is told; and the module
// the watcher hooked.
static volatile LONG SwapGeneration=0,SwapArmed=0,SwapBusy=0;
static DWORD SwapTitle=0;
static char SwapIsos[MaxDiscs+1][MAX_PATH];
static volatile HANDLE SwapModule=NULL;

// A drive path to a device path, for the drives tiles are on; "" for others.
static void DevicePath(const char* path,char* out,size_t capacity) {
    static const char* const drives[][2]={{"Hdd1:","\\Device\\Harddisk0\\Partition1"},{"Usb0:","\\Device\\Mass0"},
                                          {"Usb1:","\\Device\\Mass1"},{"Usb2:","\\Device\\Mass2"}};
    out[0]=0;
    if(path[0]=='\\') { strcpy_s(out,capacity,path); return; }
    for(int i=0;i<int(sizeof(drives)/sizeof(drives[0]));++i) {
        size_t length=strlen(drives[i][0]);
        if(!_strnicmp(path,drives[i][0],length)) { sprintf_s(out,capacity,"%s%s",drives[i][1],path+length); return; }
    }
}

// The swap's lines go to the netiso.log of the last tile started, through
// the kernel's file calls with its device path (a drive link made on a
// title's thread was gone by the time the game ran: "path not found", seen
// over xbdm), and to SwapTrace in memory, after a marker, for xbdm's getmem.
// The watcher runs on a system thread that has none of the CRT's per-thread
// data, so the lines are put together without the CRT's formatting: %s, %u
// (decimal) and %X (8 hex digits) only. A system thread's times carry on
// from the last title thread's clock by the tick count.
static char SwapLogDevice[MAX_PATH];
static struct {
    char marker[32]; volatile DWORD unused; volatile LONG ntStatus; volatile LONG length; char text[8192];
} SwapTrace={"NetIsoTiles disc swap trace",0,0,0,""};
static volatile LONG SwapWriting=0;
static volatile DWORD ClockMs=0,ClockTick=0;
static void TraceAdd(const char* text,LONG length) {
    LONG at=InterlockedExchangeAdd(&SwapTrace.length,length);
    if(at>=0 && at+length<LONG(sizeof(SwapTrace.text))) memcpy(SwapTrace.text+at,text,length);
}
static size_t Put(char* out,size_t at,size_t capacity,const char* text) {
    while(text && *text && at+1<capacity) out[at++]=*text++;
    out[at]=0;
    return at;
}
static size_t PutNumber(char* out,size_t at,size_t capacity,DWORD value,DWORD base,int width) {
    char digits[12]; int count=0;
    do { digits[count++]="0123456789ABCDEF"[value%base]; value/=base; } while(value && count<12);
    while(count<width && count<12) digits[count++]='0';
    while(count && at+1<capacity) out[at++]=digits[--count];
    out[at]=0;
    return at;
}
static size_t Format(char* out,size_t at,size_t capacity,const char* format,va_list args) {
    for(const char* f=format;*f && at+1<capacity;++f) {
        if(*f!='%' || !f[1]) { out[at++]=*f; continue; }
        char kind=*++f;
        if(kind=='s') { const char* text=va_arg(args,const char*); at=Put(out,at,capacity,text ? text : "(null)"); }
        else if(kind=='u') at=PutNumber(out,at,capacity,va_arg(args,DWORD),10,1);
        else if(kind=='X') at=PutNumber(out,at,capacity,va_arg(args,DWORD),16,8);
        else out[at++]=kind;
    }
    out[at]=0;
    return at;
}
// The log the swap's lines go to: folder's (a device path). On a title thread.
static void SwapLogTo(const char* folder) {
    while(InterlockedCompareExchange(&SwapWriting,1,0)) Sleep(1);
    sprintf_s(SwapLogDevice,"%s\\%s",folder,LogName);
    InterlockedExchange(&SwapWriting,0);
}
static bool SwapAppend(const char* text,DWORD length) {
    while(InterlockedCompareExchange(&SwapWriting,1,0)) Sleep(1);
    char device[MAX_PATH];
    size_t size=Put(device,0,sizeof(device),SwapLogDevice);
    LONG status=-1;
    if(size) {
        KernelString name={USHORT(size),USHORT(sizeof(device)),device};
        ObjectAttributes attributes={NULL,&name,0x40}; // OBJ_CASE_INSENSITIVE
        IoStatusBlock io={0,0}; HANDLE file=NULL;
        // GENERIC_WRITE with SYNCHRONIZE, FILE_OPEN_IF, a synchronous file.
        status=NtCreateFile(&file,0x40100000,&attributes,&io,NULL,FILE_ATTRIBUTE_NORMAL,FILE_SHARE_READ,3,0x60);
        if(status>=0) {
            LARGE_INTEGER end; end.LowPart=0xFFFFFFFF; end.HighPart=-1; // FILE_WRITE_TO_END_OF_FILE
            status=NtWriteFile(file,NULL,NULL,NULL,&io,text,length,&end);
            NtClose(file);
        }
        SwapTrace.ntStatus=status;
    }
    InterlockedExchange(&SwapWriting,0);
    return status>=0;
}
static void SwapNote(const char* format,...) {
    DWORD ms;
    if(KeGetCurrentProcessType()!=SystemProcess) {
        SYSTEMTIME t; GetLocalTime(&t);
        ms=((t.wHour*60+t.wMinute)*60+t.wSecond)*1000+t.wMilliseconds;
        ClockTick=GetTickCount(); ClockMs=ms;
    } else ms=(ClockMs+(GetTickCount()-ClockTick))%86400000;
    char line[512];
    size_t at=PutNumber(line,0,sizeof(line),ms/3600000,10,2);
    at=Put(line,at,sizeof(line),":"); at=PutNumber(line,at,sizeof(line),ms/60000%60,10,2);
    at=Put(line,at,sizeof(line),":"); at=PutNumber(line,at,sizeof(line),ms/1000%60,10,2);
    at=Put(line,at,sizeof(line),"."); at=PutNumber(line,at,sizeof(line),ms%1000,10,3);
    at=Put(line,at,sizeof(line)," ");
    va_list args; va_start(args,format);
    at=Format(line,at,sizeof(line)-2,format,args);
    va_end(args);
    at=Put(line,at,sizeof(line),"\r\n");
    TraceAdd(line,LONG(at));
    if(!SwapAppend(line,DWORD(at))) {
        char failed[64];
        size_t n=Put(failed,0,sizeof(failed),"  (not in the log: status ");
        n=PutNumber(failed,n,sizeof(failed),DWORD(SwapTrace.ntStatus),16,8);
        n=Put(failed,n,sizeof(failed),")\r\n");
        TraceAdd(failed,LONG(n));
    }
}

// The data of an XEX header field (an offset from the header), or NULL.
static const BYTE* XexField(const BYTE* header,DWORD size,DWORD key) {
    if(!header || size<24 || memcmp(header,"XEX2",4)) return NULL;
    const DWORD* words=(const DWORD*)header;
    for(DWORD i=0;i<words[5] && i<64 && 24+i*8+8<=size;++i)
        if(words[6+i*2]==key) return words[7+i*2]<size ? header+words[7+i*2] : NULL;
    return NULL;
}

// The tile's disc set, found on Aurora's thread when the tile starts, the
// way Aurora reads its folders: Win32 calls on the tile's drive path. (The
// kernel's listing of \Device\Harddisk0\Partition1\NetISO on the launching
// title's thread found no disc of GTA V; seen in the trace.) The discs are
// the tile folders next to the tile's own with the same Title ID in
// netiso.ini, numbered by their launcher's execution info, as Aurora finds
// them. Each step goes to the tile's log.
static DWORD DiscHeader[1024]; // 4 KB: the XEX header's field directory.
static DWORD PreparedTile=0;
static int PreparedDiscs=0;
// The disc number in an XEX's execution info, and how many discs the set
// has; 0 without one, -1 if the file can't be read. The directory is at the
// start, but the field itself can be further in: the launchers' is at
// 0x1490 of a 0x3000-byte header (seen on GTA V's tiles), so it's read
// where the directory points.
static int DiscOfXex(const char* path,int* discs=NULL) {
    HANDLE file=CreateFile(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file==INVALID_HANDLE_VALUE) return -1;
    DWORD done=0,at=0;
    if(ReadFile(file,DiscHeader,sizeof(DiscHeader),&done,NULL) && done>=24 && !memcmp(DiscHeader,"XEX2",4)) {
        DWORD headerSize=DiscHeader[2];
        for(DWORD i=0;i<DiscHeader[5] && 24+i*8+8<=done;++i)
            if(DiscHeader[6+i*2]==0x40006) { at=DiscHeader[7+i*2]; break; }
        if(at<24 || at+24>headerSize) at=0;
    }
    BYTE info[24]; DWORD got=0;
    bool read=at && SetFilePointer(file,LONG(at),NULL,FILE_BEGIN)==at && ReadFile(file,info,sizeof(info),&got,NULL) && got==sizeof(info);
    CloseHandle(file);
    if(!read) return 0;
    if(discs) *discs=info[19];
    return info[18];
}
static void PrepareDiscs(const Settings& settings) {
    PreparedTile=0; PreparedDiscs=0;
    char path[MAX_PATH]; sprintf_s(path,"%s\\%s",Folder,GameXex);
    int discs=0,disc=DiscOfXex(path,&discs);
    if(disc<=0) { Note("Disc swaps: no disc number in %s (%d).",path,disc); return; }
    if(discs<2) return;
    Note("Disc swaps: this tile is disc %d of %d.",disc,discs);
    char parent[MAX_PATH]; strcpy_s(parent,Folder);
    char* cut=strrchr(parent,'\\');
    if(!cut) { Note("Disc swaps: no folder above %s to find the other discs in.",Folder); return; }
    *cut=0;
    char pattern[MAX_PATH]; sprintf_s(pattern,"%s\\*",parent);
    WIN32_FIND_DATA found;
    HANDLE find=FindFirstFile(pattern,&found);
    if(find==INVALID_HANDLE_VALUE) { Note("Disc swaps: can't list %s (%lu).",parent,GetLastError()); return; }
    InterlockedExchange(&SwapArmed,0); // A game armed before keeps nothing of these.
    memset(SwapIsos,0,sizeof(SwapIsos));
    int folders=0,count=0;
    do {
        if(!(found.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)) continue;
        ++folders;
        char text[512]; Settings other;
        sprintf_s(path,"%s\\%s\\%s",parent,found.cFileName,IniName);
        if(!ReadText(path,text,sizeof(text)) || !ReadSettings(text,other) || other.titleId!=settings.titleId) continue;
        sprintf_s(path,"%s\\%s\\%s",parent,found.cFileName,GameXex);
        int number=DiscOfXex(path);
        if(number<1 || number>MaxDiscs) { Note("Disc swaps: %s has no disc number (%d).",path,number); continue; }
        if(SwapIsos[number][0]) { Note("Disc swaps: disc %d is both %s and %s; using %s.",number,SwapIsos[number],other.iso,SwapIsos[number]); continue; }
        strcpy_s(SwapIsos[number],other.iso);
        ++count;
        Note("Disc swaps: disc %d is %s (%s).",number,other.iso,found.cFileName);
    } while(FindNextFile(find,&found));
    FindClose(find);
    Note("Disc swaps: %d of the %d discs found among %d folders in %s.",count,discs,folders,parent);
    PreparedDiscs=count; PreparedTile=LastTile.at;
}

// Where a 16-byte thunk jumps: lis rN,high; ori or addi rN,rN,low; mtctr rN;
// bctr (as xam's loader, Nova and WriteJump write them), or a branch at its
// start. 0 if it's neither.
static DWORD JumpTarget(const DWORD* thunk) {
    DWORD first=thunk[0];
    if((first>>26)==18 && (first&3)==0) return BranchTarget(first,thunk);
    DWORD reg=(first>>21)&31,second=thunk[1];
    if((first>>26)!=15 || ((first>>16)&31)!=0) return 0; // lis rN (addis rN,0)
    if(((second>>21)&31)!=reg || ((second>>16)&31)!=reg) return 0;
    if(thunk[2]!=(0x7C0903A6|(reg<<21)) || thunk[3]!=Bctr) return 0; // mtctr rN; bctr
    DWORD high=(first&0xFFFF)<<16,low=second&0xFFFF;
    if((second>>26)==24) return high|low; // ori
    if((second>>26)==14) return high+DWORD(int(short(low))); // addi
    return 0;
}

// The running title's module comes from xboxkrnl's XexExecutableModuleHandle
// (export 403, a pointer to its loader entry): XexGetModuleHandle doesn't
// find a game's default.xex from the watcher's system thread (seen with
// GTA V). Both addresses are looked up when the plugin starts.
static const DWORD ExecutableModuleOrdinal=403;
static HANDLE* ExecutableModule=NULL;
static void* SwapDiscExport=NULL;
static void FindSwapExports() {
    HANDLE kernel=NULL,xam=NULL; void* address=NULL;
    if(XexGetModuleHandle("xboxkrnl.exe",&kernel)>=0 && XexGetProcedureAddress(kernel,ExecutableModuleOrdinal,&address)>=0) ExecutableModule=(HANDLE*)address;
    if(XexGetModuleHandle("xam.xex",&xam)>=0 && XexGetProcedureAddress(xam,SwapDiscOrdinal,&address)>=0) SwapDiscExport=address;
}
static HANDLE TitleModule() {
    if(!ExecutableModule || !MmIsAddressValid(ExecutableModule)) return NULL;
    HANDLE module=*ExecutableModule;
    return module && MmIsAddressValid(module) ? module : NULL;
}
// The game's import of XSwapDisc: the thunk after the import record that xam
// resolved to the export, in the import libraries of the running title, if
// it is SwapTitle. NULL if there is none (yet); what was found is in seen,
// for the log.
// Nova puts its own XSwapDisc in both the import's slot and its thunk, so
// once it has, the slot no longer holds xam's export. Its function is known
// by how it starts (read from the NetISO kit's Nova.xex): mflr r12; bl
// __savegprlr; stwu r1,-0x90(r1); mr r31,r3; mr r30,r4; mr r29,r5.
static bool NovaSwapDisc(DWORD address) {
    static const DWORD start[6]={0x7D8802A6,0,0x9421FF70,0x7C7F1B78,0x7C9E2378,0x7CBD2B78};
    const DWORD* code=(const DWORD*)address;
    if(address<0x90000000 || !MmIsAddressValid((void*)code) || !MmIsAddressValid((void*)(code+5))) return false;
    if((code[1]>>26)!=18 || (code[1]&3)!=1) return false; // bl
    for(int i=0;i<6;++i) if(i!=1 && code[i]!=start[i]) return false;
    return true;
}
enum SwapLook { NoGame, NoExport, NoHeader, OtherTitle, NoImports, NoImport, BadThunk, Found };
static DWORD* SwapThunk(HANDLE& module,SwapLook& seen,DWORD& detail) {
    void* target=SwapDiscExport;
    detail=0;
    seen=NoGame;
    module=TitleModule();
    if(!module) return NULL;
    seen=NoExport;
    if(!target) return NULL;
    // The module handle is its loader entry; the XEX header is at 0x58 (read from Nova.xex).
    seen=NoHeader;
    const BYTE* const* headerAt=(const BYTE* const*)((const BYTE*)module+0x58);
    if(!MmIsAddressValid((void*)headerAt) || !MmIsAddressValid((void*)*headerAt)) return NULL;
    const BYTE* header=*headerAt;
    const DWORD headerSize=0x10000;
    const BYTE* info=XexField(header,headerSize,0x40006);
    if(!info || !MmIsAddressValid((void*)info)) return NULL;
    detail=((const DWORD*)info)[3];
    seen=OtherTitle;
    if(detail!=SwapTitle) return NULL;
    seen=NoImports;
    const BYTE* imports=XexField(header,headerSize,0x103FF);
    if(!imports || !MmIsAddressValid((void*)imports)) return NULL;
    const DWORD* top=(const DWORD*)imports; // size, string table size, library count
    const BYTE* library=imports+12+top[1];
    seen=NoImport;
    detail=top[2];
    for(DWORD l=0;l<top[2] && l<32;++l) {
        if(!MmIsAddressValid((void*)library)) return NULL;
        // size, digest[20], id, version, minimum version, name index (16 bits), record count (16 bits), records
        DWORD size=*(const DWORD*)library,records=*(const WORD*)(library+38);
        const DWORD* record=(const DWORD*)(library+40);
        for(DWORD r=0;r+1<records;++r) {
            const DWORD* slot=(const DWORD*)record[r];
            if(!MmIsAddressValid((void*)slot) || (*slot!=DWORD(target) && !NovaSwapDisc(*slot))) continue;
            DWORD* thunk=(DWORD*)record[r+1];
            seen=BadThunk; detail=DWORD(thunk);
            if(!MmIsAddressValid(thunk) || !MmIsAddressValid(thunk+3) || !JumpTarget(thunk)) return NULL;
            seen=Found;
            return thunk;
        }
        if(size<40) return NULL;
        library+=size;
    }
    return NULL;
}

// What GTA V's call of XSwapDisc expects (read from its install disc): 0 and
// its event set once the disc is in, or ERROR_INVALID_PARAMETER for "no
// swap", which it handles itself; any other value traps. Nova's handler sets
// the event before it returns, so this one does too: the game's thread waits
// here while a thread of ours mounts the disc. If that takes longer than
// SwapWaitMs, the game gets 0 and the thread sets the event once the disc is
// in.
static const DWORD SwapRefused=ERROR_INVALID_PARAMETER,SwapWaitMs=QuietMs+MountWaitMs+5000;
static struct { DWORD disc; void* event; volatile LONG waiting,in; } SwapRequest;
static DWORD DiscNumber() {
    DWORD titleId=0,mediaId=0; int disc=0;
    return DiscIds(titleId,mediaId,&disc) && titleId==SwapTitle ? DWORD(disc) : 0;
}
// Puts the disc the game asked for in the drive, on a thread of the game's,
// so NetISO's unload call is skipped (see UnloadHooked).
static DWORD WINAPI SwapThread(void*) {
    DWORD disc=SwapRequest.disc; void* event=SwapRequest.event;
    SwapNote("The game asks for disc %u, %s.",disc,SwapIsos[disc]);
    bool in=DiscNumber()==disc;
    if(in) SwapNote("Disc %u is already in the drive.",disc);
    else {
        static char text[64]; sprintf_s(text,"NetDisc 360: putting in disc %lu",disc); Show(text);
        Settings settings; memset(&settings,0,sizeof(settings));
        strcpy_s(settings.iso,SwapIsos[disc]); settings.titleId=SwapTitle;
        char folder[MAX_PATH],isoFolder[MAX_PATH];
        if(!FindMountFolder(settings,folder,sizeof(folder),isoFolder)) SwapNote("The server doesn't list %s.",settings.iso);
        else if(!ListMountFolder(folder,false)) SwapNote("Can't open %s.",folder);
        else {
            Sleep(QuietMs);
            for(int waited=QuietMs;!in && waited<QuietMs+MountWaitMs;waited+=PollMs) {
                in=DiscNumber()==disc;
                if(in) SwapNote("Disc %u is in the drive %u ms after the mount.",disc,DWORD(waited)); else Sleep(PollMs);
            }
            if(!in) SwapNote("Disc %u didn't show up in the drive.",disc);
        }
    }
    if(!in) { static char text[64]; sprintf_s(text,"NetDisc 360 couldn't put in disc %lu",disc); Show(text); }
    InterlockedExchange(&SwapRequest.in,in ? 1 : 0);
    // The game's call stopped waiting: the event is set from here.
    if(!InterlockedExchange(&SwapRequest.waiting,0) && in) { KeSetEvent(event,1,FALSE); SwapNote("Told the game disc %u is in.",disc); }
    InterlockedExchange(&SwapBusy,0);
    return 0;
}
static DWORD SwapDiscHooked(DWORD disc,void* event,void* extra) {
    DWORD number=disc&0xFF;
    HANDLE module=NULL;
    if((module=TitleModule())==NULL || module!=SwapModule) {
        SwapNote("The game asks for disc %u (%X, %X, %X), but it isn't this tile's game; handing it on.",number,disc,DWORD(event),DWORD(extra));
        return ((SwapDisc)(void*)SwapPreviousStub)(disc,event,extra);
    }
    // Nova's handler can't put in a disc from the server, so a tile's game
    // never goes there.
    if(number<1 || number>DWORD(MaxDiscs) || !SwapIsos[number][0] || !event || !extra) {
        SwapNote("The game asks for disc %u (%X, %X, %X), which this tile has no folder for; told it there's no swap.",number,disc,DWORD(event),DWORD(extra));
        Show("NetDisc 360: this game asks for a disc it has no tile for");
        return SwapRefused;
    }
    if(InterlockedCompareExchange(&SwapBusy,1,0)) { SwapNote("The game asks for disc %u during a swap; told it there's no swap.",number); return SwapRefused; }
    SwapRequest.disc=number; SwapRequest.event=event;
    InterlockedExchange(&SwapRequest.in,0); InterlockedExchange(&SwapRequest.waiting,1);
    // CreateThread, for a title thread with the CRT's thread data.
    DWORD id=0;
    HANDLE thread=CreateThread(NULL,0x10000,SwapThread,NULL,0,&id);
    if(!thread) {
        InterlockedExchange(&SwapBusy,0);
        SwapNote("No thread for the swap; told the game there's no swap.");
        return SwapRefused;
    }
    DWORD waited=WaitForSingleObject(thread,SwapWaitMs);
    CloseHandle(thread);
    if(waited!=WAIT_OBJECT_0 && InterlockedExchange(&SwapRequest.waiting,0)) {
        SwapNote("Disc %u isn't in after %u s; the game waits for it.",number,SwapWaitMs/1000);
        return 0;
    }
    if(!SwapRequest.in) { SwapNote("Told the game there's no swap to disc %u.",number); return SwapRefused; }
    KeSetEvent(event,1,FALSE);
    SwapNote("Told the game disc %u is in.",number);
    return 0;
}

// The watcher: one system thread, started with the plugin, that keeps the
// armed game's XSwapDisc import pointed at SwapDiscHooked while a game of
// that Title ID runs, also after Nova changes it on a restart. (Watchers
// created during the launch, as the first builds did, were parked in the
// kernel and never ran; seen over xbdm.) ArmSwap gives it a new generation;
// it stops watching after SwapIdleMs without the game. Each change in what it
// finds is logged. No CRT calls here (see SwapNote).
static volatile LONG WatcherLoops=0,WatcherStop=0;
static HANDLE Watcher=NULL;
static DWORD WINAPI SwapWatch(void*) {
    LONG watching=0; DWORD seenAt=0,lastDetail=1; SwapLook last=Found;
    while(!WatcherStop) {
        InterlockedIncrement(&WatcherLoops);
        if(!SwapArmed) { Sleep(500); continue; }
        LONG generation=SwapGeneration;
        if(generation!=watching) {
            watching=generation; seenAt=GetTickCount(); last=Found; lastDetail=1;
            SwapNote("Disc swaps: watching for Title ID %X.",SwapTitle);
        }
        HANDLE module=NULL; SwapLook seen; DWORD detail;
        DWORD* thunk=SwapThunk(module,seen,detail);
        if(seen!=last || detail!=lastDetail) {
            switch(seen) {
            case NoGame: SwapNote("Disc swaps: no title running (%X).",DWORD(ExecutableModule)); break;
            case NoExport: SwapNote("Disc swaps: xam's export %u wasn't found when the plugin started.",SwapDiscOrdinal); break;
            case NoHeader: SwapNote("Disc swaps: the title (%X) has no XEX header in memory.",DWORD(module)); break;
            case OtherTitle: SwapNote("Disc swaps: the title (%X) is Title ID %X.",DWORD(module),detail); break;
            case NoImports: SwapNote("Disc swaps: the title (%X) has no import libraries.",DWORD(module)); break;
            case NoImport: SwapNote("Disc swaps: the title (%X), %u import libraries, doesn't import XSwapDisc.",DWORD(module),detail); break;
            case BadThunk: {
                const DWORD* at=(const DWORD*)detail;
                if(MmIsAddressValid((void*)at) && MmIsAddressValid((void*)(at+3)))
                    SwapNote("Disc swaps: the game's XSwapDisc import at %X reads %X %X %X %X, not a jump.",detail,at[0],at[1],at[2],at[3]);
                else SwapNote("Disc swaps: the game's XSwapDisc import at %X can't be read.",detail);
                Show("NetDisc 360 can't take this game's disc swaps.");
                break;
            }
            case Found: break;
            }
            last=seen; lastDetail=detail;
        }
        if(thunk) {
            seenAt=GetTickCount();
            DWORD to=JumpTarget(thunk);
            if(to!=DWORD(SwapDiscHooked)) {
                WriteJump((DWORD*)SwapPreviousStub,to);
                FlushCode((void*)SwapPreviousStub,4*4);
                SwapModule=module;
                WriteJump(thunk,DWORD(SwapDiscHooked));
                FlushCode(thunk,4*4);
                SwapNote("Disc swaps: the game's (import at %X, module %X) come here now; they went to %X, %s.",
                         DWORD(thunk),DWORD(module),to,to==DWORD(SwapDiscExport) ? "xam" : "another plugin (Nova's, from Aurora)");
                Show("NetDisc 360: disc swaps ready for this game");
            }
        } else if(seen!=OtherTitle && seen!=NoImport && GetTickCount()-seenAt>SwapIdleMs) {
            SwapNote("Disc swaps: no game of this Title ID for %u s; not watching any more.",SwapIdleMs/1000);
            InterlockedExchange(&SwapArmed,0);
        } else if(GetTickCount()-seenAt>SwapIdleMs*5) {
            SwapNote("Disc swaps: still no XSwapDisc import to take after %u s; not watching any more.",SwapIdleMs*5/1000);
            InterlockedExchange(&SwapArmed,0);
        }
        Sleep(thunk ? 1000 : 250);
    }
    return 0;
}
static void StartWatcher() {
    DWORD id=0;
    if(ExCreateThread(&Watcher,0x10000,&id,NULL,SwapWatch,NULL,SystemThread)<0) Watcher=NULL;
}
static void StopWatcher() {
    if(!Watcher) return;
    InterlockedExchange(&WatcherStop,1);
    WaitForSingleObject(Watcher,3000);
    CloseHandle(Watcher);
    Watcher=NULL;
}

// Called when a disc is started: if a tile was started just before, watch
// for its game.
static bool DiscStart(const char* path,const char* mountPath) {
    if(!path) return false;
    if(!_stricmp(path,AuroraDiscXex) || !_stricmp(path,"XLNI_DET_MEDIA") || !_stricmp(path,DiscXex)) return true;
    return mountPath && !_stricmp(mountPath,DiscDevice) && !_stricmp(path,GameXex);
}
// On the launching title's thread: finds the discs of the tile just
// started and hands them to the watcher. Once per start: XamLoaderLaunchTitle
// goes on through XamLoaderLaunchTitleEx, so both hooks see it (seen in the
// trace).
static DWORD ArmedTile=0,ArmedAt=0;
static void ArmSwap() {
    if(!TileFresh()) return;
    if(ArmedTile==LastTile.at && GetTickCount()-ArmedAt<10000) return;
    ArmedTile=LastTile.at; ArmedAt=GetTickCount();
    InterlockedExchange(&SwapArmed,0);
    if(PreparedTile!=LastTile.at) return; // A single disc.
    int discs=PreparedDiscs;
    if(discs<2) { SwapNote("Disc swaps: %u disc(s) of this game found, nothing to swap.",DWORD(discs)); return; }
    SwapTitle=LastTile.titleId; SwapModule=NULL;
    if(!Watcher) { SwapNote("Disc swaps: the watcher didn't start with the plugin, so no swaps."); return; }
    __sync(); // The discs before the generation, for the watcher.
    InterlockedIncrement(&SwapGeneration);
    InterlockedExchange(&SwapArmed,1);
    SwapNote("Disc swaps: on for this start (the watcher has looked %u times since the plugin started).",DWORD(WatcherLoops));
    static char text[64]; sprintf_s(text,"NetDisc 360: disc swaps on for this game (%d discs)",discs); Show(text);
}
// Each launch after a tile's, in that tile's log.
static void NoteLaunch(const char* path,const char* mountPath,DWORD flags,const char* what) {
    if(!TileFresh()) return;
    SwapNote("Launch %s%s%s (flags %X): %s.",mountPath ? mountPath : "",mountPath ? " " : "",path ? path : "(no path)",flags,what);
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
    DevicePath(path,TileDevice,sizeof(TileDevice));
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
    Note("NetISO reads: %s.",ReadLimitNote);
    Note("NetISO's receive buffer: %s; set on %ld connection(s), refused on %ld.",ReceiveNote,ReceiveSets,ReceiveRefusals);
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

// Mounts the tile's ISO and starts the disc. Returns only if it couldn't:
// Fallback when the tile's launcher should try, Cancelled when nothing should
// start.
static Outcome StartTile(const Settings& settings) {
    Outcome outcome=Mount(settings);
    if(outcome==Cancelled) {
        Note("Back to Aurora.");
        CloseLog();
        UnlinkDrive(TileLink);
        return Cancelled;
    }
    bool mounted=outcome==Started;
    LONG status=mounted ? LinkDrive(AuroraDiscLink,DiscDevice) : 0;
    if(status<0) Note("Can't link %s (status %08X).",AuroraDiscLink,status);
    if(mounted && status>=0) {
        Note("Starting %s, as Aurora's disc entry does",AuroraDiscXex);
        CloseLog();
        ArmSwap();
        Skip(true);
        DWORD result=((LaunchTitle)LaunchTitleHook.stub)(AuroraDiscXex,0);
        Skip(false);
        OpenLog(Folder);
        Note("XamLoaderLaunchTitle returned %08X.",result);
    }
    Note("Starting the tile's own launcher instead.");
    CloseLog();
    UnlinkDrive(TileLink);
    return Fallback;
}
// Tries a launch the plugin's way: NotTile when it isn't a tile (xam starts
// it as asked), Fallback when it's a tile the plugin couldn't start (xam
// starts the tile's launcher), Cancelled when nothing should start.
enum Handling { NotTile, TileFallback, TileCancelled };
static Handling Handled(const char* path,const char* mountPath) {
    if(Skipping() || InterlockedCompareExchange(&Busy,1,0)) return NotTile;
    Settings settings;
    Handling handling=NotTile;
    if(IsTile(path,mountPath,settings)) {
        strcpy_s(LastTile.folder,TileDevice); LastTile.titleId=settings.titleId; LastTile.at=GetTickCount();
        SwapLogTo(LastTile.folder);
        PrepareDiscs(settings);
        // The tile's own launcher starts it (see the top of this file);
        // tiles=aurora in netiso.ini mounts from inside Aurora instead.
        char value[16];
        if(!IniValue(Ini,"tiles",value,sizeof(value)) || _stricmp(value,"aurora")) {
            Note("The tile's own launcher starts it (tiles=aurora would mount from Aurora).");
            CloseLog();
            UnlinkDrive(TileLink);
            handling=TileFallback;
        } else handling=StartTile(settings)==Cancelled ? TileCancelled : TileFallback;
    }
    InterlockedExchange(&Busy,0);
    if(handling==TileFallback) Skip(true);
    return handling;
}
// What xam's launch calls get back for a tile that was cancelled.
static const DWORD CancelledLaunch=ERROR_NOT_READY;
static const char* LaunchKind(Handling handling,bool disc) { return handling!=NotTile ? "a tile" : disc ? "the disc" : "not a tile"; }
static DWORD LaunchTitleHooked(const char* path,DWORD flags) {
    Handling handling=Handled(path,NULL);
    if(handling==TileCancelled) return CancelledLaunch;
    bool disc=handling==NotTile && DiscStart(path,NULL);
    NoteLaunch(path,NULL,flags,LaunchKind(handling,disc));
    if(disc) ArmSwap();
    DWORD result=((LaunchTitle)LaunchTitleHook.stub)(path,flags);
    if(handling==TileFallback) Skip(false);
    return result;
}
static DWORD LaunchTitleExHooked(const char* path,const char* mountPath,const char* commandLine,DWORD flags) {
    Handling handling=Handled(path,mountPath);
    if(handling==TileCancelled) return CancelledLaunch;
    bool disc=handling==NotTile && DiscStart(path,mountPath);
    NoteLaunch(path,mountPath,flags,LaunchKind(handling,disc));
    if(disc) ArmSwap();
    DWORD result=((LaunchTitleEx)LaunchTitleExHook.stub)(path,mountPath,commandLine,flags);
    if(handling==TileFallback) Skip(false);
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
        // NetISO.xex is loaded before this plugin in launch.ini; if not, the
        // first tile patches its unload call, and its read size and receive
        // buffer stay NetISO's own.
        PatchUnload();
        PatchReadLimit();
        PatchReceiveBuffer();
        FindSwapExports();
        StartWatcher();
    } else if(reason==DLL_PROCESS_DETACH) {
        StopWatcher();
        UnpatchReceiveBuffer();
        UnpatchUnload();
        Unhook(LaunchTitleExHook);
        Unhook(LaunchTitleHook);
    }
    return TRUE;
}
