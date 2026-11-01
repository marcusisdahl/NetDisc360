// SPDX-License-Identifier: MIT
// The disc drive from the server, built into NetDisc360.xex.
//
// Until now the console also needed cOz's NetISO.xex 1.0, a closed plugin
// that makes a game on the server look like a disc in the drive. This file
// does that job with code of our own, written from what NetISO.xex was seen
// to do (read from the binary), so the console needs only this plugin:
// - \Device\Cdrom0 taken over: while a disc from the server is in, the
//   drive's reads come from the server and its controls are answered the
//   way NetISO.xex answered them, which games and xam are known to accept.
//   With nothing from the server in, a real drive works as before.
// - Full disc images work: the game's file system is found behind the video
//   partition (XGD1, XGD2, XGD3) and every read is moved to it.
// - A fake tray cycle after each change, so the console sees the new disc.
// - XeKeysGetMediaID answers the disc's media ID, from its security sector.
// - \Device\NetDisc360, where the launcher asks for a disc by name with the
//   server's address, and asks how it went (shared/netdisc.h). NetISO.xex's
//   folders (\Device\NetIso\<iso>\Mount) and its XamAppUnloadSelf call,
//   which left the Guide dead after a mount, are gone.
//
// One system thread, the worker, does everything that waits: it alone talks
// to the server, over one connection (the ISVR protocol, shared/isvr.h),
// made at start to the server NetDisc360.ini or NetISO.xex.txt names and
// kept open, as NetISO.xex kept its own.
// The drive's reads and the launcher's commands are queued to it in the
// order they came. Unlike NetISO.xex, which asked for 4 KB at a time and
// waited for each piece, it asks for up to read_kb at once with several
// requests in flight; and when the connection drops it connects again and
// mounts the same disc before it gives the disc up.
//
// It all starts in DllMain, before DashLaunch starts the dashboard, in the
// order NetISO.xex started: the worker, the tray, \Device\Cdrom0 and the
// media ID, then \Device\NetDisc360. Done later, from the worker while the
// dashboard was starting, it froze Aurora and the Guide on the console.
// It stays off when NetISO.xex is loaded before it in launch.ini (or
// own_disc=0 in NetDisc360.ini), so both can sit in launch.ini while this
// one is tried.
//
// The worker is a system thread without the CRT's per-thread data, so
// nothing it runs formats with the CRT (no sprintf): see Format.
#include "disc.h"
#include <ppcintrinsics.h>
#include <string.h>
#include <stdarg.h>
#include <winsockx.h>
#ifndef TCP_NODELAY
#define TCP_NODELAY 1
#endif

extern "C" LONG __stdcall XexGetModuleHandle(const char* name,HANDLE* module);
extern "C" LONG __stdcall XexGetProcedureAddress(HANDLE module,DWORD ordinal,void** address);
extern "C" LONG __stdcall ExCreateThread(HANDLE* handle,DWORD stackSize,DWORD* threadId,void* apiStartup,LPTHREAD_START_ROUTINE start,void* parameter,DWORD flags);
extern "C" BOOLEAN __stdcall MmIsAddressValid(void* address);

namespace {

typedef unsigned __int64 U64;
struct Text { USHORT length,capacity; const char* text; };   // ANSI_STRING
struct Attributes { HANDLE root; Text* name; ULONG attributes; };
struct IoStatus { LONG status; ULONG_PTR information; };
struct Event { DWORD header[4]; };                           // KEVENT
struct Link { Link* next; Link* previous; };                 // LIST_ENTRY

// ---- The kernel's and xam's calls, found by ordinal ----

typedef LONG (*IoCreateDeviceCall)(void* driver,ULONG extension,Text* name,ULONG type,BOOLEAN exclusive,void** device);
typedef LONG (*IoDeleteDeviceCall)(void* device);
typedef void (*IoCompleteRequestCall)(void* irp,CHAR boost);
typedef BYTE (*IoAcquireDeviceObjectLockCall)();
typedef void (*IoReleaseDeviceObjectLockCall)(BYTE irql);
typedef LONG (*ObReferenceObjectByNameCall)(Text* name,ULONG attributes,void* type,void* parse,void** object);
typedef void (*ObDereferenceObjectCall)(void* object);
typedef LONG (*ObCreateSymbolicLinkCall)(Text* link,Text* device);
typedef LONG (*ObDeleteSymbolicLinkCall)(Text* link);
typedef void (*KeInitializeEventCall)(Event* event,int type,BOOLEAN state);
typedef LONG (*KeSetEventCall)(void* event,LONG increment,BOOLEAN wait);
typedef LONG (*KeWaitForSingleObjectCall)(void* object,int reason,int mode,BOOLEAN alertable,LARGE_INTEGER* timeout);
typedef LONG (*KeDelayExecutionThreadCall)(int mode,BOOLEAN alertable,LARGE_INTEGER* interval);
typedef LONG (*KeSetBasePriorityThreadCall)(HANDLE thread,LONG priority);
typedef BYTE (*KfAcquireSpinLockCall)(DWORD* lock);
typedef void (*KfReleaseSpinLockCall)(DWORD* lock,BYTE irql);
typedef void (*HalRegisterSMCNotificationCall)(void* registration,BOOLEAN on);
typedef void (*HalSendSMCMessageCall)(BYTE* message,BYTE* reply);
typedef LONG (*XeKeysSetKeyCall)(WORD key,const void* data,DWORD size);
typedef LONG (*XeKeysGetMediaIDCall)(BYTE* id);
typedef LONG (*NtCreateFileCall)(HANDLE* handle,ACCESS_MASK access,Attributes* attributes,IoStatus* status,LARGE_INTEGER* size,ULONG fileAttributes,ULONG share,ULONG disposition,ULONG options);
typedef LONG (*NtWriteFileCall)(HANDLE handle,HANDLE event,void* apc,void* context,IoStatus* status,const void* buffer,ULONG length,LARGE_INTEGER* offset);
typedef LONG (*NtCloseCall)(HANDLE handle);
typedef void* (*RtlImageXexHeaderFieldCall)(const void* header,DWORD key);
typedef LONG (*DispatchCall)(void* device,BYTE* irp);
// xam's NetDll calls take who is calling first: 2, a system thread.
typedef SOCKET (*SocketCall)(DWORD caller,int family,int type,int protocol);
typedef int (*CloseSocketCall)(DWORD caller,SOCKET s);
typedef int (*IoctlSocketCall)(DWORD caller,SOCKET s,long command,u_long* value);
typedef int (*SetSocketOptionCall)(DWORD caller,SOCKET s,int level,int name,const char* value,int length);
typedef int (*ConnectCall)(DWORD caller,SOCKET s,const sockaddr* address,int length);
typedef int (*SelectCall)(DWORD caller,int count,fd_set* readable,fd_set* writable,fd_set* failed,const timeval* timeout);
typedef int (*ReceiveCall)(DWORD caller,SOCKET s,char* data,int length,int flags);
typedef int (*SendCall)(DWORD caller,SOCKET s,const char* data,int length,int flags);
typedef int (*LastErrorCall)();
typedef int (*WsaStartupCall)(DWORD caller,WORD version,void* data,DWORD versionFlags);
typedef int (*XNetStartupCall)(DWORD caller,const void* parameters);
typedef DWORD (*ConfigStatusCall)(DWORD caller,DWORD* status); // status: 16 words

struct Calls {
    IoCreateDeviceCall IoCreateDevice; IoDeleteDeviceCall IoDeleteDevice; IoCompleteRequestCall IoCompleteRequest;
    IoAcquireDeviceObjectLockCall IoAcquireDeviceObjectLock; IoReleaseDeviceObjectLockCall IoReleaseDeviceObjectLock;
    ObReferenceObjectByNameCall ObReferenceObjectByName; ObDereferenceObjectCall ObDereferenceObject;
    ObCreateSymbolicLinkCall ObCreateSymbolicLink; ObDeleteSymbolicLinkCall ObDeleteSymbolicLink; void* ObSymbolicLinkObjectType;
    KeInitializeEventCall KeInitializeEvent; KeSetEventCall KeSetEvent; KeWaitForSingleObjectCall KeWaitForSingleObject;
    KeDelayExecutionThreadCall KeDelayExecutionThread; KeSetBasePriorityThreadCall KeSetBasePriorityThread;
    KfAcquireSpinLockCall KfAcquireSpinLock; KfReleaseSpinLockCall KfReleaseSpinLock;
    HalRegisterSMCNotificationCall HalRegisterSMCNotification; HalSendSMCMessageCall HalSendSMCMessage;
    XeKeysSetKeyCall XeKeysSetKey; XeKeysGetMediaIDCall XeKeysGetMediaID;
    NtCreateFileCall NtCreateFile; NtWriteFileCall NtWriteFile; NtCloseCall NtClose;
    RtlImageXexHeaderFieldCall RtlImageXexHeaderField;
    SocketCall socket; CloseSocketCall closesocket; IoctlSocketCall ioctlsocket; SetSocketOptionCall setsockopt;
    ConnectCall connect; SelectCall select; ReceiveCall recv; SendCall send; LastErrorCall WSAGetLastError;
    WsaStartupCall WSAStartupEx; XNetStartupCall XNetStartup; ConfigStatusCall XnpGetConfigStatus;
} K;

struct Import { void** call; DWORD ordinal; };
static bool Find(const char* module,const Import* imports,int count) {
    HANDLE handle=NULL;
    if(XexGetModuleHandle(module,&handle)<0) return false;
    for(int i=0;i<count;++i)
        if(XexGetProcedureAddress(handle,imports[i].ordinal,imports[i].call)<0 || !*imports[i].call) return false;
    return true;
}
// Ordinals from xboxkrnl.exe's and xam.xex's export tables.
static bool FindCalls() {
    const Import kernel[]={
        {(void**)&K.IoCreateDevice,55},{(void**)&K.IoDeleteDevice,57},{(void**)&K.IoCompleteRequest,53},
        {(void**)&K.IoAcquireDeviceObjectLock,46},{(void**)&K.IoReleaseDeviceObjectLock,68},
        {(void**)&K.ObReferenceObjectByName,273},{(void**)&K.ObDereferenceObject,261},
        {(void**)&K.ObCreateSymbolicLink,259},{(void**)&K.ObDeleteSymbolicLink,260},{&K.ObSymbolicLinkObjectType,274},
        {(void**)&K.KeInitializeEvent,112},{(void**)&K.KeSetEvent,157},{(void**)&K.KeWaitForSingleObject,176},
        {(void**)&K.KeDelayExecutionThread,90},{(void**)&K.KeSetBasePriorityThread,153},
        {(void**)&K.KfAcquireSpinLock,177},{(void**)&K.KfReleaseSpinLock,180},
        {(void**)&K.HalRegisterSMCNotification,39},{(void**)&K.HalSendSMCMessage,41},
        {(void**)&K.XeKeysSetKey,578},{(void**)&K.XeKeysGetMediaID,690},
        {(void**)&K.NtCreateFile,210},{(void**)&K.NtWriteFile,255},{(void**)&K.NtClose,207},
        {(void**)&K.RtlImageXexHeaderField,299}};
    const Import xam[]={
        {(void**)&K.socket,3},{(void**)&K.closesocket,4},{(void**)&K.ioctlsocket,6},{(void**)&K.setsockopt,7},
        {(void**)&K.connect,12},{(void**)&K.select,15},{(void**)&K.recv,18},{(void**)&K.send,22},
        {(void**)&K.WSAGetLastError,27},{(void**)&K.WSAStartupEx,36},{(void**)&K.XNetStartup,51},{(void**)&K.XnpGetConfigStatus,105}};
    return Find("xboxkrnl.exe",kernel,int(sizeof(kernel)/sizeof(kernel[0]))) && Find("xam.xex",xam,int(sizeof(xam)/sizeof(xam[0])));
}

// ---- Requests (IRPs), by the offsets the console's kernel uses ----

enum {
    IrpStatus=0x10,IrpInformation=0x14,IrpUserBuffer=0x1C,IrpUserIosb=0x20,IrpUserEvent=0x24,IrpLink=0x30,IrpStack=0x50,
    StackFlags=2,StackControl=3,
    // ReadOffset is 64 bits; once queued, its high word holds the sector and its low word the buffer offset.
    ReadLength=4,ReadBuffer=8,ReadOffset=0xC,
    ControlOutLength=4,ControlIn=8,ControlInLength=0xC,ControlCode=0x10,
    CreateDisposition=8,
    DeviceDriver=8,DeviceFlags=0x14,DeviceSectorSize=0x20,DeviceAlignment=0x24,
    DeviceInitializing=0x10, // In DeviceFlags: set on \Device\Cdrom0 when the console has no drive.
    // A driver object: StartIo, DeleteDevice, DismountVolume, then the dispatch routines of majors 0 to 10.
    DriverWords=14, DriverStartIo=0, DriverDelete=1, DriverDismount=2, DriverCreate=3, DriverClose=3+1, DriverRead=3+2, DriverControl=3+9
};
static inline DWORD& Word(void* base,DWORD offset) { return *(DWORD*)((BYTE*)base+offset); }
static inline BYTE* Stack(BYTE* irp) { return (BYTE*)Word(irp,IrpStack); }

static const LONG Pending=0x103,InvalidRequest=LONG(0xC0000010),InvalidParameter=LONG(0xC000000D),NoMedia=LONG(0xC0000013),
                  NotReady=LONG(0xC00000A3),TooSmall=LONG(0xC0000023),NotSupported=LONG(0xC00000BB),DeviceError=LONG(0xC0000185),
                  AccessDenied=LONG(0xC0000022),Busy=LONG(0x80000011);

// ---- Settings and the drive's state ----

static disc::Settings S;
static const DWORD Sector=2048,SystemCaller=2;
static const int MaxDepth=8;
static volatile LONG Activated=0,Stopping=0,Running=0;
static HANDLE Worker=NULL;

// The disc in the drive. Written by the worker only; read anywhere.
static volatile DWORD Sectors=0;               // 0 = no disc from the server in.
static U64 Size=0,Partition=0;    // Bytes: the image, and where its game file system starts.
static char Mounted[netdisc::NameBytes];
static DWORD MountedLength=0;
// From its security sector: for XeKeysGetMediaID and control 0x24090.
static volatile LONG Security=0;
static BYTE MediaId[16];
static DWORD Layers[3],LayerWord,LayerTail[5];
// The drive state controls 0x24084 and 0x240A8 set and read, as NetISO.xex kept it.
static DWORD DriveState[4]={0,3,4,0};
static bool RealDrive=false;
static bool HasDisc() { return Sectors!=0; }

// What Status answers, under Lock; the worker publishes it (Publish).
static DWORD Lock=0;
static netdisc::Status Public;
static DWORD LastTicket=0;
static DWORD State=netdisc::StateOff,DoneTicket=0,DoneResult=netdisc::ResultNone;
static DWORD ServerAddress=0,ServerPort=netdisc::DefaultPort;
static volatile LONG Connected=0;
static DWORD Reads=0,FailedReads=0,Reconnects=0,SlowestReadMs=0;
static U64 BytesRead=0;
static char NoteText[128]="Not started.";
static volatile DWORD Unhandled=0;

// ---- Text without the CRT ----

static size_t Put(char* out,size_t at,size_t capacity,const char* text) {
    while(text && *text && at+1<capacity) out[at++]=*text++;
    if(at<capacity) out[at]=0;
    return at;
}
static size_t PutNumber(char* out,size_t at,size_t capacity,DWORD value,DWORD base,int width) {
    char digits[12]; int count=0;
    do { digits[count++]="0123456789ABCDEF"[value%base]; value/=base; } while(value && count<12);
    while(count<width && count<12) digits[count++]='0';
    while(count && at+1<capacity) out[at++]=digits[--count];
    if(at<capacity) out[at]=0;
    return at;
}
// %s, %u (decimal), %X (8 hex digits) and %I (an IPv4 address in network
// byte order) only.
static size_t Format(char* out,size_t at,size_t capacity,const char* format,va_list args) {
    for(const char* f=format;*f && at+1<capacity;++f) {
        if(*f!='%' || !f[1]) { out[at++]=*f; continue; }
        char kind=*++f;
        if(kind=='s') { const char* text=va_arg(args,const char*); at=Put(out,at,capacity,text ? text : "(none)"); }
        else if(kind=='u') at=PutNumber(out,at,capacity,va_arg(args,DWORD),10,1);
        else if(kind=='X') at=PutNumber(out,at,capacity,va_arg(args,DWORD),16,8);
        else if(kind=='I') {
            DWORD address=va_arg(args,DWORD);
            const BYTE* b=(const BYTE*)&address;
            for(int i=0;i<4;++i) { if(i) at=Put(out,at,capacity,"."); at=PutNumber(out,at,capacity,b[i],10,1); }
        }
        else out[at++]=kind;
    }
    if(at<capacity) out[at]=0;
    return at;
}

// ---- Log: Hdd1:\NetISO\NetDisc360\disc.log, this boot only ----

// Written through the kernel's file calls, by the plugin's start (DllMain)
// and by the worker, one line at a time. Times are seconds since the
// console started.
static char LogPath[]="\\Device\\Harddisk0\\Partition1\\NetISO\\NetDisc360\\disc.log";
static bool LogStarted=false;
static volatile LONG LogBusy=0;
static void Delay(DWORD ms);
static void Log(const char* format,...) {
    // Not a spin lock: the file calls below wait, which a spin lock's IRQL forbids.
    for(int i=0;InterlockedCompareExchange(&LogBusy,1,0)!=0;++i) { if(i>=200) return; Delay(5); }
    char line[384];
    DWORD ms=GetTickCount();
    size_t at=Put(line,0,sizeof(line),"[");
    at=PutNumber(line,at,sizeof(line),ms/1000,10,5); at=Put(line,at,sizeof(line),".");
    at=PutNumber(line,at,sizeof(line),ms%1000,10,3); at=Put(line,at,sizeof(line),"] ");
    va_list args; va_start(args,format);
    at=Format(line,at,sizeof(line)-2,format,args);
    va_end(args);
    at=Put(line,at,sizeof(line),"\r\n");
    Text name={USHORT(sizeof(LogPath)-1),USHORT(sizeof(LogPath)),LogPath};
    Attributes attributes={NULL,&name,0x40}; // OBJ_CASE_INSENSITIVE
    IoStatus io={0,0}; HANDLE file=NULL;
    // GENERIC_WRITE with SYNCHRONIZE; FILE_OVERWRITE_IF the first time this
    // boot, FILE_OPEN_IF after; a synchronous, non-directory file.
    if(K.NtCreateFile(&file,0x40100000,&attributes,&io,NULL,FILE_ATTRIBUTE_NORMAL,FILE_SHARE_READ,LogStarted ? 3 : 5,0x60)>=0) {
        LogStarted=true;
        LARGE_INTEGER end; end.LowPart=0xFFFFFFFF; end.HighPart=-1; // FILE_WRITE_TO_END_OF_FILE
        K.NtWriteFile(file,NULL,NULL,NULL,&io,line,DWORD(at),&end);
        K.NtClose(file);
    }
    InterlockedExchange(&LogBusy,0);
}
// The note Status shows, also logged.
static void SetNote(const char* format,...) {
    char text[sizeof(NoteText)];
    va_list args; va_start(args,format);
    Format(text,0,sizeof(text),format,args);
    va_end(args);
    BYTE irql=K.KfAcquireSpinLock(&Lock);
    memcpy(NoteText,text,sizeof(text));
    K.KfReleaseSpinLock(&Lock,irql);
    Log("%s",text);
}

static void Delay(DWORD ms) {
    LARGE_INTEGER interval; interval.QuadPart=-(__int64)ms*10000;
    K.KeDelayExecutionThread(1,FALSE,&interval);
}

// Copies the worker's state to what Status answers. Worker only.
static void Publish() {
    BYTE irql=K.KfAcquireSpinLock(&Lock);
    Public.version=netdisc::Version; Public.size=sizeof(Public);
    Public.state=State; Public.ticket=LastTicket; Public.done=DoneTicket; Public.result=DoneResult;
    Public.address=ServerAddress; Public.port=ServerPort; Public.connected=Connected ? 1 : 0;
    Public.sectors=Sectors; Public.partition=DWORD(Partition);
    Public.mediaId=Security ? *(const DWORD*)MediaId : 0;
    Public.reads=Reads; Public.readMb=DWORD(BytesRead>>20); Public.failedReads=FailedReads; Public.reconnects=Reconnects;
    Public.slowestReadMs=SlowestReadMs;
    Public.readBytes=S.readBytes; Public.depth=DWORD(S.depth); Public.receiveBytes=DWORD(S.receiveBytes);
    memcpy(Public.iso,Mounted,sizeof(Public.iso));
    memcpy(Public.note,NoteText,sizeof(Public.note));
    K.KfReleaseSpinLock(&Lock,irql);
}

// ---- The server (worker only) ----

static SOCKET Socket=INVALID_SOCKET;
static bool NetworkStarted=false;
static DWORD LastTalk=0;
static const DWORD WouldBlock=WSAEWOULDBLOCK;
// How long a reply may take: a disk on the server that has to spin up first
// can take a while.
static const long ReplySeconds=15,ConnectSeconds=3;

static bool ToldNoNetwork=false;
static bool NetworkUp() {
    if(!NetworkStarted) {
        // XNetStartupParams: its size, the rest XNet's defaults (as NetISO.xex started it).
        BYTE parameters[13]; memset(parameters,0,sizeof(parameters)); parameters[0]=13;
        static BYTE wsa[512];
        Log("Starting the network (XNetStartup).");
        if(int failed=K.XNetStartup(SystemCaller,parameters)) { Log("XNetStartup failed: %X.",DWORD(failed)); return false; }
        Log("Starting the network (WSAStartupEx).");
        if(int failed=K.WSAStartupEx(SystemCaller,2,wsa,0x20352400)) { Log("WSAStartupEx failed: %X.",DWORD(failed)); return false; }
        Log("The network is started.");
        NetworkStarted=true;
    }
    // XnpGetConfigStatus fills 64 bytes (NetISO.xex gave it 16 words); a
    // single word here let it overwrite the worker's stack, which stopped
    // the console at boot (xbdm caught the exception and halted the rest).
    DWORD status[16]; memset(status,0,sizeof(status));
    bool up=!K.XnpGetConfigStatus(SystemCaller,status) && (status[0]&1);
    if(!up && !ToldNoNetwork) { Log("The console's network isn't up yet (%X); waiting for it.",status[0]); ToldNoNetwork=true; }
    else if(up && ToldNoNetwork) { Log("The console's network is up."); ToldNoNetwork=false; }
    return up;
}
static void Disconnect(const char* why) {
    if(Socket!=INVALID_SOCKET) {
        K.closesocket(SystemCaller,Socket);
        Socket=INVALID_SOCKET;
        if(why) Log("Connection to the server closed: %s.",why);
    }
    InterlockedExchange(&Connected,0);
}
// Waits until the socket can be written (or read), up to seconds; false
// if it can't or it failed.
static bool Ready(bool writing,long seconds) {
    fd_set set,failed;
    set.fd_count=1; set.fd_array[0]=Socket; failed.fd_count=1; failed.fd_array[0]=Socket;
    timeval timeout={seconds,0};
    int ready=K.select(SystemCaller,0,writing ? NULL : &set,writing ? &set : NULL,&failed,&timeout);
    return ready>0 && set.fd_count && !failed.fd_count;
}
static bool SendAll(const BYTE* data,DWORD length) {
    while(length) {
        if(!Ready(true,ReplySeconds)) { Disconnect("the server took no more data"); return false; }
        int sent=K.send(SystemCaller,Socket,(const char*)data,int(length),0);
        if(sent>0) { data+=sent; length-=DWORD(sent); continue; }
        if(sent<0 && DWORD(K.WSAGetLastError())==WouldBlock) continue;
        Disconnect("sending failed"); return false;
    }
    return true;
}
static bool ReceiveAll(BYTE* data,DWORD length) {
    while(length) {
        if(!Ready(false,ReplySeconds)) { Disconnect("the server didn't answer in time"); return false; }
        int got=K.recv(SystemCaller,Socket,(char*)data,int(length),0);
        if(got>0) { data+=got; length-=DWORD(got); continue; }
        if(got<0 && DWORD(K.WSAGetLastError())==WouldBlock) continue;
        Disconnect(got ? "receiving failed" : "the server closed it"); return false;
    }
    LastTalk=GetTickCount();
    return true;
}
static void Put32(BYTE* at,DWORD value) { at[0]=BYTE(value>>24); at[1]=BYTE(value>>16); at[2]=BYTE(value>>8); at[3]=BYTE(value); }
static DWORD Get32(const BYTE* at) { return DWORD(at[0])<<24|DWORD(at[1])<<16|DWORD(at[2])<<8|at[3]; }
enum { Ping=0, GetIsoSize=1, ReadData=3, MountIso=5, Header=20 };
static void Request(BYTE* at,int command,U64 offset,DWORD length) {
    at[0]='I'; at[1]='S'; at[2]='V'; at[3]='R';
    at[4]=BYTE(command>>8); at[5]=BYTE(command); at[6]=0; at[7]=0;
    Put32(at+8,DWORD(offset>>32)); Put32(at+12,DWORD(offset)); Put32(at+16,length);
}
static bool Ask(int command,BYTE* reply,DWORD replyLength) {
    BYTE request[Header];
    Request(request,command,0,0);
    return SendAll(request,Header) && ReceiveAll(reply,replyLength);
}
// Header and name in one send: netiso-srv reads the name with one read().
static bool AskMount(const char* name,DWORD length,BYTE* reply) {
    BYTE message[Header+netdisc::NameBytes];
    if(length>=netdisc::NameBytes) return false;
    Request(message,MountIso,0,length); memcpy(message+Header,name,length);
    return SendAll(message,Header+length) && ReceiveAll(reply,4);
}
static bool Connect() {
    Disconnect(NULL);
    if(!ServerAddress) return false;
    Socket=K.socket(SystemCaller,AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if(Socket==INVALID_SOCKET) return false;
    BOOL on=TRUE;
    // Plain TCP to a LAN host: without these XNet only allows secure traffic.
    K.setsockopt(SystemCaller,Socket,SOL_SOCKET,0x5801,(const char*)&on,sizeof(on));
    K.setsockopt(SystemCaller,Socket,SOL_SOCKET,0x5802,(const char*)&on,sizeof(on));
    // The requests are 20 bytes each, and the next ones go out before the
    // data of the first is in.
    K.setsockopt(SystemCaller,Socket,IPPROTO_TCP,TCP_NODELAY,(const char*)&on,sizeof(on));
    if(S.receiveBytes) K.setsockopt(SystemCaller,Socket,SOL_SOCKET,SO_RCVBUF,(const char*)&S.receiveBytes,sizeof(S.receiveBytes));
    u_long nonblocking=1;
    K.ioctlsocket(SystemCaller,Socket,FIONBIO,&nonblocking);
    sockaddr_in to; memset(&to,0,sizeof(to));
    to.sin_family=AF_INET; to.sin_port=htons(u_short(ServerPort)); to.sin_addr.s_addr=ServerAddress;
    if(K.connect(SystemCaller,Socket,(const sockaddr*)&to,sizeof(to)) && DWORD(K.WSAGetLastError())!=WouldBlock) { Disconnect(NULL); return false; }
    if(!Ready(true,ConnectSeconds)) { Disconnect(NULL); return false; }
    BYTE reply[8];
    if(!Ask(Ping,reply,8)) return false;
    if(memcmp(reply,"ISVRokOK",8)) { Disconnect("it isn't a NetISO server"); return false; }
    InterlockedExchange(&Connected,1);
    return true;
}
// Reads length bytes at offset, in requests of up to S.readBytes with up
// to S.depth of them sent before the data of the first is in. The server
// answers them in order, each with exactly the bytes asked for.
static bool ReadAt(U64 offset,DWORD length,BYTE* data) {
    DWORD sent=0,received=0; int flying=0;
    while(received<length) {
        BYTE requests[Header*MaxDepth]; DWORD batch=0;
        while(flying<S.depth && sent<length) {
            DWORD part=length-sent<S.readBytes ? length-sent : S.readBytes;
            Request(requests+batch,ReadData,offset+sent,part);
            batch+=Header; sent+=part; ++flying;
        }
        if(batch && !SendAll(requests,batch)) return false;
        DWORD part=length-received<S.readBytes ? length-received : S.readBytes;
        if(!ReceiveAll(data+received,part)) return false;
        received+=part; --flying;
    }
    return true;
}

// ---- The fake tray ----

// The SMC's notification handlers: a list of {callback, unused, list entry}.
static Link* SmcHandlers=NULL;
static volatile BYTE TrayState=0; // 0 = the SMC answers for itself.
static BYTE SmcRegistration[16];
static void SmcNoticed(BYTE*,BYTE*) {}
// xam's import of HalSendSMCMessage comes here: a tray query (0x0A)
// answers the fake state while there is one.
static void SmcHooked(BYTE* message,BYTE* reply) {
    if(TrayState && reply && message[0]==0x0A) { reply[0]=0x0A; reply[1]=TrayState; return; }
    K.HalSendSMCMessage(message,reply);
}
static void SmcTell() {
    if(!SmcHandlers) return;
    BYTE message[16]; memset(message,0,sizeof(message));
    message[0]=0x83; message[1]=TrayState; // The tray changed.
    for(Link* entry=SmcHandlers->next;entry && entry!=SmcHandlers;entry=entry->next) {
        BYTE* registration=(BYTE*)entry-8;
        ((void (*)(BYTE*,BYTE*))Word(registration,0))(registration,message);
    }
}
// Opens and closes the tray, as changing the disc by hand does, so the
// console reads the drive again.
static void TrayCycle() {
    TrayState=0x60; SmcTell(); Delay(200);  // open
    TrayState=0x62; SmcTell();              // closed
    if(RealDrive) TrayState=0;              // The drive answers for itself again.
    Delay(200);
}

// ---- The disc (worker only) ----

static const char Magic[]="MICROSOFT*XBOX*MEDIA";
static BYTE SectorData[0x1000];
// Where the game's file system starts: a full image of an Xbox 360 disc
// (XGD2, XGD3) or of an original Xbox disc (XGD1) has the video partition
// first; an image of the game partition alone starts with it. For the 360
// discs, the security sector before it holds the media ID.
static void Probe() {
    static const DWORD starts[4]={0x0FD90000,0x02080000,0x18300000,0},security[2]={0x0FD8F000,0x02077000};
    Partition=0; Security=0;
    for(int i=0;i<4;++i) {
        U64 at=U64(starts[i])+0x10000;
        if(at+Sector>Size || !ReadAt(at,Sector,SectorData)) continue;
        if(memcmp(SectorData,Magic,sizeof(Magic)-1)) continue;
        Partition=starts[i];
        if(i>=2 || security[i]+sizeof(SectorData)>Size || !ReadAt(security[i],sizeof(SectorData),SectorData)) return;
        bool set=false;
        for(int b=0x20;b<0x30;++b) if(SectorData[b] && SectorData[b]!=0xFF) set=true;
        if(!set || memcmp(SectorData+0x20,SectorData+0xC60,16)) return;
        memcpy(MediaId,SectorData+0x20,16);
        memcpy(Layers,SectorData+0x804,12);
        LayerWord=Word(SectorData,0x900);
        memcpy(LayerTail,SectorData+0x908,20);
        Security=1;
        return;
    }
}
static void Forget() {
    Sectors=0; Size=0; Partition=0; Security=0; Mounted[0]=0; MountedLength=0; SlowestReadMs=0;
}
enum MountOutcome { MountOk, MountRefused, MountEmpty, MountLost };
// Mounts name on the server. On MountOk the disc is in (Sectors set).
static MountOutcome MountName(const char* name,DWORD length) {
    BYTE reply[8];
    if(!AskMount(name,length,reply)) return MountLost;
    if(!(Get32(reply)&1)) return MountRefused;
    if(!Ask(GetIsoSize,reply,8)) return MountLost;
    DWORD sectors=Get32(reply);
    if(!sectors || Get32(reply+4)!=Sector) return MountEmpty;
    Size=U64(sectors)*Sector;
    memcpy(Mounted,name,length); Mounted[length]=0; MountedLength=length;
    Probe();
    if(!Connected) { Forget(); return MountLost; }
    Sectors=sectors;
    return MountOk;
}
// Connects to the server again under a mounted disc and mounts it again,
// checking that it is the same size.
static bool Reconnect() {
    if(!NetworkUp() || !Connect()) return false;
    if(!HasDisc()) return true;
    BYTE reply[8];
    if(!AskMount(Mounted,MountedLength,reply) || !(Get32(reply)&1) || !Ask(GetIsoSize,reply,8)) { Disconnect(NULL); return false; }
    if(Get32(reply)!=Sectors) { Log("%s is %u sectors on the server now, not %u.",Mounted,Get32(reply),DWORD(Sectors)); Disconnect(NULL); return false; }
    ++Reconnects;
    Log("Connected again; %s is mounted again.",Mounted);
    return true;
}
// The server went away and didn't come back: the disc goes out.
static void LoseDisc() {
    if(!HasDisc()) return;
    SetNote("%s went out of the drive: the server stopped answering.",Mounted);
    Forget();
    State=netdisc::StateIdle;
    Publish();
    TrayCycle();
}

// ---- The queue to the worker ----

// The drive's reads (their IRPs, linked at IrpLink) and the commands, in
// the order they came.
enum Kind { MountCommand, EjectCommand };
struct Command { Link link; DWORD ticket; Kind kind; DWORD address,port; char iso[netdisc::NameBytes]; volatile LONG used; };
static const int CommandSlots=4;
static Command Commands[CommandSlots];
static Link Queue={&Queue,&Queue};
static Event QueueEvent;
static bool IsCommand(const Link* entry) { return entry>=&Commands[0].link && entry<=&Commands[CommandSlots-1].link; }
static void Append(Link* entry) {
    BYTE irql=K.KfAcquireSpinLock(&Lock);
    entry->next=&Queue; entry->previous=Queue.previous;
    Queue.previous->next=entry; Queue.previous=entry;
    K.KfReleaseSpinLock(&Lock,irql);
    K.KeSetEvent(&QueueEvent,1,FALSE);
}
static Link* Take() {
    BYTE irql=K.KfAcquireSpinLock(&Lock);
    Link* entry=Queue.next;
    if(entry==&Queue) entry=NULL;
    else { Queue.next=entry->next; entry->next->previous=&Queue; }
    K.KfReleaseSpinLock(&Lock,irql);
    return entry;
}
// From any thread: a ticket for the command, or 0 when the drive is off or
// four commands are waiting already.
static DWORD QueueCommand(Kind kind,DWORD address,DWORD port,const char* iso) {
    if(!Activated || Stopping) return 0;
    char name[netdisc::NameBytes]; memset(name,0,sizeof(name));
    for(size_t i=0;iso && iso[i] && i+1<sizeof(name);++i) name[i]=iso[i];
    DWORD ticket=0;
    BYTE irql=K.KfAcquireSpinLock(&Lock);
    for(int i=0;i<CommandSlots && !ticket;++i) {
        Command& command=Commands[i];
        if(command.used) continue;
        command.used=1; command.kind=kind; command.address=address; command.port=port ? port : DWORD(netdisc::DefaultPort);
        memcpy(command.iso,name,sizeof(name));
        ticket=command.ticket=++LastTicket;
        Public.ticket=ticket;
        command.link.next=&Queue; command.link.previous=Queue.previous;
        Queue.previous->next=&command.link; Queue.previous=&command.link;
    }
    K.KfReleaseSpinLock(&Lock,irql);
    if(ticket) K.KeSetEvent(&QueueEvent,1,FALSE);
    return ticket;
}
static void Complete(BYTE* irp,LONG status,CHAR boost) {
    Word(irp,IrpStatus)=DWORD(status);
    K.IoCompleteRequest(irp,boost);
}

// ---- The worker ----

static void Finish(DWORD ticket,DWORD result) {
    DoneTicket=ticket; DoneResult=result;
    Publish();
}
// Connects to the server, waiting up to networkMs for the console's
// network, then up to serverMs for the server.
static DWORD ConnectWithin(DWORD networkMs,DWORD serverMs) {
    DWORD start=GetTickCount(),up=0;
    for(;;) {
        if(!NetworkUp()) {
            if(GetTickCount()-start>=networkMs) return netdisc::ResultNoNetwork;
            Delay(250);
        } else {
            if(!up) up=GetTickCount();
            if(Connect()) return netdisc::ResultNone;
            if(GetTickCount()-up>=serverMs) return netdisc::ResultNoServer;
            Delay(1000);
        }
        if(Stopping) return netdisc::ResultNoServer;
    }
}
static void RunMount(const Command& command) {
    bool hadDisc=HasDisc();
    if(command.address && (command.address!=ServerAddress || command.port!=ServerPort)) {
        Disconnect(ServerAddress ? "the next disc is on another server" : NULL);
        ServerAddress=command.address; ServerPort=command.port;
    }
    DWORD length=DWORD(strlen(command.iso));
    Log("Mounting %s from %I, port %u (ticket %u).",command.iso,ServerAddress,ServerPort,command.ticket);
    // The disc in the drive goes out, whatever comes in.
    Forget();
    State=netdisc::StateConnecting; Publish();
    DWORD result=ServerAddress ? DWORD(netdisc::ResultNone) : DWORD(netdisc::ResultNoServer);
    for(int attempt=0;attempt<2 && result==netdisc::ResultNone;++attempt) {
        if(!Connected) {
            result=attempt ? ConnectWithin(5000,5000) : ConnectWithin(30000,10000);
            if(result!=netdisc::ResultNone) break;
        }
        State=netdisc::StateMounting; Publish();
        switch(MountName(command.iso,length)) {
        case MountOk: result=netdisc::ResultMounted; break;
        case MountRefused: result=netdisc::ResultNotOnServer; break;
        case MountEmpty: result=netdisc::ResultNotADisc; break;
        // A connection that had gone stale (the server restarted): once more.
        case MountLost: Forget(); if(attempt) result=netdisc::ResultLost; break;
        }
    }
    if(result==netdisc::ResultNone) result=netdisc::ResultLost;
    if(result==netdisc::ResultMounted) {
        SetNote("Mounted %s: %u sectors, game file system at %X%s.",Mounted,DWORD(Sectors),DWORD(Partition),Security ? ", media ID found" : "");
        State=netdisc::StateMounted;
    } else {
        static const char* const why[]={"","","","the console's network didn't come up","the server didn't answer",
                                        "the server doesn't have it","the server sent an empty disc","the server went away"};
        SetNote("Couldn't mount %s: %s.",command.iso,result<sizeof(why)/sizeof(why[0]) ? why[result] : "?");
        Forget();
        State=netdisc::StateIdle;
    }
    Publish();
    if(hadDisc || HasDisc()) TrayCycle();
    Finish(command.ticket,result);
}
static void RunEject(const Command& command) {
    bool hadDisc=HasDisc();
    State=netdisc::StateEjecting; Publish();
    if(Connected && hadDisc) {
        // The server's name for nothing: it closes the disc of this connection.
        static const char none[]="[Disable Current ISO]";
        BYTE reply[8];
        AskMount(none,sizeof(none)-1,reply);
        SetNote("Ejected %s.",Mounted);
    }
    Forget();
    State=netdisc::StateIdle; Publish();
    if(hadDisc) TrayCycle();
    Finish(command.ticket,netdisc::ResultEjected);
}
// A read from the drive, into the caller's buffer. While the server is
// away it connects again (and mounts the disc again), three tries a second
// apart, before the disc goes out.
static void ServeRead(BYTE* irp) {
    BYTE* stack=Stack(irp);
    DWORD length=Word(stack,ReadLength),sector=Word(stack,ReadOffset);
    BYTE* buffer=(BYTE*)Word(stack,ReadBuffer)+Word(stack,ReadOffset+4);
    Word(irp,IrpInformation)=0;
    if(!HasDisc()) { Complete(irp,DeviceError,1); return; }
    U64 offset=U64(sector)*Sector;
    DWORD start=GetTickCount();
    bool read=false;
    for(int attempt=0;attempt<3 && !read && !Stopping;++attempt) {
        if(attempt) Delay(1000);
        if(!Connected && !Reconnect()) continue;
        read=ReadAt(offset,length,buffer);
    }
    if(!read) {
        ++FailedReads;
        Log("Reading %u bytes at sector %u failed.",length,sector);
        LoseDisc();
        Complete(irp,DeviceError,1);
        return;
    }
    DWORD took=GetTickCount()-start;
    if(took>SlowestReadMs) SlowestReadMs=took;
    ++Reads; BytesRead+=length;
    Word(stack,ReadLength)=0;
    Word(irp,IrpInformation)=length;
    Complete(irp,0,1);
}
// Between requests. As NetISO.xex did, the drive stays connected to its
// server from the start, with a ping after 10 s without talk: the server
// sees the console from then on (it writes the console's game folders once
// it has), and the connection is known to work before a game needs it. A
// disc whose server went away is mounted again once it's back.
static const DWORD PingMs=10000,RetryMs=5000,IdleRetryMs=10000;
static DWORD LastTry=0;
static bool ToldWaiting=false;
static void Housekeeping() {
    DWORD now=GetTickCount();
    if(Connected && now-LastTalk>=PingMs) {
        BYTE reply[8];
        if(!Ask(Ping,reply,8)) Log("The server at %I stopped answering.",ServerAddress);
    }
    if(!Connected && ServerAddress && now-LastTry>=(HasDisc() ? RetryMs : IdleRetryMs)) {
        LastTry=now;
        if(HasDisc()) {
            if(Reconnect()) SetNote("The server is back; %s is still in the drive.",Mounted);
        } else if(NetworkUp()) {
            if(!ToldWaiting) Log("Connecting to the server at %I, port %u.",ServerAddress,ServerPort);
            if(Connect()) { Log("Connected to the server at %I, port %u.",ServerAddress,ServerPort); ToldWaiting=false; }
            else if(!ToldWaiting) { Log("The server at %I, port %u, doesn't answer; trying again every %u s.",ServerAddress,ServerPort,IdleRetryMs/1000); ToldWaiting=true; }
        }
    }
    if(DWORD code=Unhandled) { Unhandled=0; Log("The drive got control %X, which it doesn't answer.",code); }
}

// The drive's thread. As NetISO.xex's worker, it starts before the drive
// is taken over and connects to the server at once; then it serves the
// queue until the plugin stops.
static void Deactivate();
static bool NetIsoLoaded();
static DWORD WINAPI WorkerMain(void*) {
    Log("The drive's thread runs.");
    DWORD started=GetTickCount();
    bool checkedNetIso=false;
    while(!Stopping) {
        if(Activated) {
            for(Link* entry;!Stopping && (entry=Take())!=NULL;) {
                if(IsCommand(entry)) {
                    Command command=*(Command*)entry;
                    InterlockedExchange(&((Command*)entry)->used,0);
                    if(command.kind==MountCommand) RunMount(command); else RunEject(command);
                } else ServeRead((BYTE*)entry-IrpLink);
            }
        }
        if(Stopping) break;
        Housekeeping();
        // A NetISO.xex later in launch.ini than this plugin loads after the
        // drive was taken over, and takes it over again on top.
        if(!checkedNetIso && GetTickCount()-started>=10000) {
            checkedNetIso=true;
            if(Activated && NetIsoLoaded()) Log("NetISO.xex is loaded too, after this plugin: take it out of launch.ini, two disc drives don't mix.");
        }
        Publish();
        LARGE_INTEGER timeout; timeout.QuadPart=-10000000; // 1 s
        K.KeWaitForSingleObject(&QueueEvent,0,0,FALSE,&timeout);
    }
    if(Activated) Deactivate();
    Disconnect(NULL);
    InterlockedExchange(&Running,0);
    return 0;
}

// ---- \Device\Cdrom0 ----

static DWORD DriveDriver[DriverWords],OriginalDriver[DriverWords];
static DWORD OriginalDriverObject=0;
static BYTE* Drive=NULL;

static LONG Invalid(void*,BYTE* irp) {
    Complete(irp,InvalidRequest,0);
    return InvalidRequest;
}
static LONG DriveStartIo(void* device,BYTE* irp) {
    if(RealDrive) return ((DispatchCall)OriginalDriver[DriverStartIo])(device,irp);
    return Invalid(device,irp);
}
// Answers a control the way NetISO.xex did: the caller's status block and
// event first, then the completion.
static LONG Answer(BYTE* irp,LONG status,bool block,bool event) {
    IoStatus* io=(IoStatus*)Word(irp,IrpUserIosb);
    if(block && io) io->status=status;
    if(event && Word(irp,IrpUserEvent)) K.KeSetEvent((void*)Word(irp,IrpUserEvent),0,FALSE);
    Complete(irp,status,0);
    return status;
}
// 0x24800, check verify: is there a disc.
static LONG CheckVerify(BYTE* irp) {
    Answer(irp,HasDisc() ? 0 : Connected ? NotReady : NoMedia,true,true);
    return Pending;
}
// 0x2404C: the disc's sectors and their size.
static LONG DriveGeometry(BYTE* irp) {
    DWORD* out=(DWORD*)Word(irp,IrpUserBuffer);
    out[0]=Sectors; out[1]=Sector;
    IoStatus* io=(IoStatus*)Word(irp,IrpUserIosb);
    if(io) io->information=8;
    Word(irp,IrpInformation)=8;
    Answer(irp,0,true,true);
    return Pending;
}
// 0x24090: the disc's security information, from its security sector.
static LONG SecurityInfo(BYTE* irp,BYTE* stack) {
    DWORD length=Word(stack,ControlOutLength);
    LONG status=TooSmall;
    if(length>=8) {
        DWORD* out=(DWORD*)Word(irp,IrpUserBuffer);
        BYTE* bytes=(BYTE*)out;
        status=0;
        out[0]=LayerWord;
        bytes[4]=0; bytes[5]=3;
        DWORD information=8;
        if(length>=12) out[2]=0xC40;
        if(length>=0x28) {
            out[3]=Layers[2]-Layers[0]+1;
            out[4]=Layers[1]-(~(Layers[2]+1)&0xFFFFFF);
            memcpy(out+5,LayerTail,20);
            information=0x28;
        }
        Word(irp,IrpInformation)=information;
    }
    return Answer(irp,status,true,true);
}
// The drive's controls while a disc from the server is in, answered as
// NetISO.xex answered them (read from the binary).
static LONG DriveControlWithDisc(BYTE* irp,BYTE* stack,DWORD code) {
    DWORD* out=(DWORD*)Word(irp,IrpUserBuffer);
    DWORD outLength=Word(stack,ControlOutLength);
    switch(code) {
    case 0x24800: return CheckVerify(irp);
    case 0x2404C: return DriveGeometry(irp);
    case 0x24090: return SecurityInfo(irp,stack);
    case 0x335142:
        if(Word((void*)Word(stack,ControlIn),8)) return Answer(irp,InvalidRequest,false,false);
        ((BYTE*)out)[0]=8; ((BYTE*)out)[1]=2; ((BYTE*)out)[6]=2;
        Answer(irp,0,true,true);
        return Pending;
    case 0x24084: {
        if(Word(stack,ControlInLength)!=4) return Answer(irp,InvalidParameter,true,true);
        DWORD value=*(DWORD*)Word(stack,ControlIn);
        if(outLength<4) Word(irp,IrpInformation)=0; else { out[0]=DriveState[2]; Word(irp,IrpInformation)=4; }
        if(value>3) return Answer(irp,InvalidParameter,true,true);
        DriveState[0]=DriveState[3]=value;
        return Answer(irp,0,true,true);
    }
    case 0x240A8:
        if(outLength!=16) return Answer(irp,TooSmall,true,true);
        memcpy(out,DriveState,16);
        return Answer(irp,0,true,true);
    case 0x240C4: case 0x240B8: case 0x24098: case 0x240E0: return Answer(irp,0,true,true);
    case 0x240D0: return Answer(irp,NotSupported,true,true);
    case 0x240E4: Word(stack,ControlOutLength)=0; return Answer(irp,0,true,true);
    case 0x240CC: out[0]=3; Word(irp,IrpInformation)=4; return Answer(irp,0,false,true);
    case 0x24080: case 0x24094: { // The drive's authentication.
        DWORD key=0xC40;
        K.XeKeysSetKey(0x101C,&key,4);
        return Answer(irp,0,true,true);
    }
    case 0x240A0:
        if(!out || outLength!=4) return Answer(irp,InvalidParameter,false,false);
        out[0]=2;
        return Answer(irp,0,false,false);
    default:
        Unhandled=code;
        return Answer(irp,InvalidRequest,false,false);
    }
}
static LONG DriveControl(void* device,BYTE* irp) {
    BYTE* stack=Stack(irp);
    DWORD code=Word(stack,ControlCode);
    if(HasDisc()) return DriveControlWithDisc(irp,stack,code);
    if(RealDrive) return ((DispatchCall)OriginalDriver[DriverControl])(device,irp);
    if(code==0x24800) return CheckVerify(irp);
    return Invalid(device,irp);
}
// A read of the disc: checked, then queued to the worker as NetISO.xex
// queued it (the stack location keeps the sector and the buffer).
static LONG DriveRead(void* device,BYTE* irp) {
    if(!HasDisc()) {
        if(RealDrive) return ((DispatchCall)OriginalDriver[DriverRead])(device,irp);
        return Invalid(device,irp);
    }
    BYTE* stack=Stack(irp);
    // The 64-bit offset as two words: the stack location keeps it only 4-byte aligned.
    U64 offset=(U64(Word(stack,ReadOffset))<<32 | Word(stack,ReadOffset+4))+Partition;
    DWORD length=Word(stack,ReadLength);
    bool cached=(stack[StackFlags]&1)!=0; // The buffer is at ReadBuffer itself.
    Word(irp,IrpInformation)=0;
    if(!cached && ((Word(irp,IrpUserBuffer)&1) || (Word(stack,ReadBuffer)&1) || (length&(Sector-1)) || (DWORD(offset)&(Sector-1)) || offset+length>Size)) {
        Complete(irp,InvalidParameter,0);
        return InvalidParameter;
    }
    if(!length) { Complete(irp,0,0); return 0; }
    DWORD sector=DWORD(offset/Sector);
    Word(stack,ReadOffset+4)=cached ? 0 : Word(stack,ReadBuffer);
    if(!cached) Word(stack,ReadBuffer)=Word(irp,IrpUserBuffer);
    Word(stack,ReadOffset)=sector;
    stack[StackFlags]=0;
    stack[StackControl]|=1; // SL_PENDING_RETURNED
    Append((Link*)(irp+IrpLink));
    return Pending;
}
// Points the drive's device at a driver of ours (found through a link, as
// NetISO.xex found it): reads and controls come here, the rest goes on to
// the drive's own driver. On a console without a drive everything else is
// "invalid", and the device stops looking unfinished.
static bool HookDrive() {
    static char linkName[]="\\System??\\NetDisc360Drive:",target[]="\\Device\\Cdrom0";
    Text link={USHORT(sizeof(linkName)-1),USHORT(sizeof(linkName)),linkName},device={USHORT(sizeof(target)-1),USHORT(sizeof(target)),target};
    K.ObDeleteSymbolicLink(&link);
    if(K.ObCreateSymbolicLink(&link,&device)<0) return false;
    void* object=NULL;
    if(K.ObReferenceObjectByName(&link,0,K.ObSymbolicLinkObjectType,NULL,&object)>=0 && object) {
        Drive=(BYTE*)Word(object,0); // The link's target.
        K.ObDereferenceObject(object);
    }
    K.ObDeleteSymbolicLink(&link);
    if(!Drive || !MmIsAddressValid(Drive) || !MmIsAddressValid((void*)Word(Drive,DeviceDriver))) { Drive=NULL; return false; }
    BYTE irql=K.IoAcquireDeviceObjectLock();
    OriginalDriverObject=Word(Drive,DeviceDriver);
    memcpy(OriginalDriver,(void*)OriginalDriverObject,sizeof(OriginalDriver));
    memcpy(DriveDriver,OriginalDriver,sizeof(DriveDriver));
    RealDrive=!(Word(Drive,DeviceFlags)&DeviceInitializing);
    if(!RealDrive) {
        Word(Drive,DeviceFlags)&=~DWORD(DeviceInitializing);
        DriveDriver[DriverDelete]=DriveDriver[DriverDismount]=0;
        for(int i=DriverCreate;i<DriverWords;++i) DriveDriver[i]=DWORD(Invalid);
    }
    DriveDriver[DriverStartIo]=DWORD(DriveStartIo);
    DriveDriver[DriverRead]=DWORD(DriveRead);
    DriveDriver[DriverControl]=DWORD(DriveControl);
    Word(Drive,DeviceDriver)=DWORD(DriveDriver);
    K.IoReleaseDeviceObjectLock(irql);
    return true;
}
static void UnhookDrive() {
    if(!Drive) return;
    BYTE irql=K.IoAcquireDeviceObjectLock();
    Word(Drive,DeviceDriver)=OriginalDriverObject;
    K.IoReleaseDeviceObjectLock(irql);
    Drive=NULL;
}

// ---- Hooks in xam and the kernel ----

static void Flush(void* at,DWORD size) {
    // The RXDK's ppcintrinsics.h has no __isync; isync is emitted by its opcode.
    for(DWORD offset=0;offset<size;offset+=4) { __dcbst(offset,at); __sync(); }
    __emit(0x4C00012C);
}
// xam's imports of HalSendSMCMessage, pointed at SmcHooked.
static const int MaxSmcThunks=4;
static DWORD* SmcThunks[MaxSmcThunks];
static DWORD SmcThunkCode[MaxSmcThunks][4];
static int SmcThunkCount=0;
static int HookSmc() {
    HANDLE xam=NULL;
    if(XexGetModuleHandle("xam.xex",&xam)<0 || !xam) return 0;
    const void* header=*(void* const*)((const BYTE*)xam+0x58); // The loader entry's XEX header.
    const BYTE* imports=header ? (const BYTE*)K.RtlImageXexHeaderField(header,0x103FF) : NULL;
    if(!imports) return 0;
    const DWORD* top=(const DWORD*)imports; // size, string table size, library count
    const BYTE* library=imports+12+top[1];
    for(DWORD l=0;l<top[2] && l<32;++l) {
        if(!MmIsAddressValid((void*)library)) break;
        // size, digest[20], id, version, minimum version, name index (16 bits), record count (16 bits), records
        DWORD size=*(const DWORD*)library,records=*(const WORD*)(library+38);
        const DWORD* record=(const DWORD*)(library+40);
        for(DWORD r=0;r+1<records && SmcThunkCount<MaxSmcThunks;++r) {
            const DWORD* slot=(const DWORD*)record[r];
            if(!MmIsAddressValid((void*)slot) || *slot!=DWORD(K.HalSendSMCMessage)) continue;
            DWORD* thunk=(DWORD*)record[r+1];
            if(!MmIsAddressValid(thunk) || !MmIsAddressValid(thunk+3)) continue;
            // Only the loader's stub (lis r11; addi or ori r11; mtctr r11; bctr):
            // the next record is something else when xam reads the address only.
            if((thunk[0]&0xFFFF0000)!=0x3D600000 || ((thunk[1]&0xFFFF0000)!=0x396B0000 && (thunk[1]&0xFFFF0000)!=0x616B0000)
               || thunk[2]!=0x7D6903A6 || thunk[3]!=0x4E800420) continue;
            memcpy(SmcThunkCode[SmcThunkCount],thunk,16);
            DWORD target=DWORD(SmcHooked);
            thunk[0]=0x3D600000|(target>>16);    // lis r11,high
            thunk[1]=0x616B0000|(target&0xFFFF); // ori r11,r11,low
            thunk[2]=0x7D6903A6;                 // mtctr r11
            thunk[3]=0x4E800420;                 // bctr
            Flush(thunk,16);
            SmcThunks[SmcThunkCount++]=thunk;
        }
        if(size<40) break;
        library+=size;
    }
    return SmcThunkCount;
}
static void UnhookSmc() {
    for(int i=0;i<SmcThunkCount;++i) { memcpy(SmcThunks[i],SmcThunkCode[i],16); Flush(SmcThunks[i],16); }
    SmcThunkCount=0;
}
// The kernel's export of XeKeysGetMediaID, pointed at MediaIdHooked for
// the titles loaded from then on.
static DWORD* MediaIdSlot=NULL;
static DWORD MediaIdWas=0;
static LONG MediaIdHooked(BYTE* id) {
    if(HasDisc() && Security) { memcpy(id,MediaId,16); return 0; }
    return K.XeKeysGetMediaID(id);
}
static bool HookMediaId() {
    HANDLE kernel=NULL;
    if(XexGetModuleHandle("xboxkrnl.exe",&kernel)<0 || !kernel) return false;
    const BYTE* header=*(const BYTE* const*)((const BYTE*)kernel+0x58);
    if(!header || !MmIsAddressValid((void*)header) || K.RtlImageXexHeaderField(header,0xE10402)) return false; // Exports by name: another layout.
    const BYTE* security=*(const BYTE* const*)(header+0x10);
    if(!MmIsAddressValid((void*)(security+0x160))) return false;
    DWORD* table=*(DWORD**)(security+0x160); // The export table.
    if(!table || !MmIsAddressValid(table)) return false;
    DWORD base=(table[8]&0xFFFF)<<16;
    DWORD* slot=table+10+690; // After the table's header, by ordinal.
    if(!MmIsAddressValid(slot) || base+*slot!=DWORD(K.XeKeysGetMediaID)) return false;
    MediaIdWas=*slot;
    *slot=DWORD(MediaIdHooked)-base;
    Flush(slot,4);
    MediaIdSlot=slot;
    return true;
}
static void UnhookMediaId() {
    if(!MediaIdSlot) return;
    *MediaIdSlot=MediaIdWas;
    Flush(MediaIdSlot,4);
    MediaIdSlot=NULL;
}

// ---- \Device\NetDisc360: the launcher's way in ----

static DWORD ControlDriver[DriverWords];
static void* ControlDevice=NULL;
static LONG ControlCreate(void*,BYTE* irp) {
    LONG status=Stack(irp)[CreateDisposition]==1 ? 0 : AccessDenied; // FILE_OPEN only.
    Complete(irp,status,0);
    return status;
}
static LONG ControlClose(void*,BYTE* irp) {
    Complete(irp,0,0);
    return 0;
}
static bool Usable(void* at,DWORD length) { return at && MmIsAddressValid(at) && MmIsAddressValid((BYTE*)at+length-1); }
// The controls of shared/netdisc.h, on the caller's thread: they only copy
// and queue, so they never wait.
static LONG ControlDispatch(void*,BYTE* irp) {
    BYTE* stack=Stack(irp);
    void* in=(void*)Word(stack,ControlIn);
    void* out=(void*)Word(irp,IrpUserBuffer);
    DWORD inLength=Word(stack,ControlInLength),outLength=Word(stack,ControlOutLength);
    LONG status=InvalidParameter;
    DWORD information=0;
    switch(Word(stack,ControlCode)) {
    case netdisc::ControlStatus:
        if(outLength>=sizeof(netdisc::Status) && Usable(out,sizeof(netdisc::Status))) {
            disc::GetStatus(*(netdisc::Status*)out);
            information=sizeof(netdisc::Status); status=0;
        }
        break;
    case netdisc::ControlMount:
        if(inLength>=sizeof(netdisc::MountRequest) && Usable(in,sizeof(netdisc::MountRequest)) && outLength>=4 && Usable(out,4)) {
            netdisc::MountRequest request; memcpy(&request,in,sizeof(request));
            request.iso[sizeof(request.iso)-1]=0;
            if(request.version!=netdisc::Version || !request.iso[0]) break;
            DWORD ticket=disc::Mount(request.address,int(request.port),request.iso);
            *(DWORD*)out=ticket; information=4;
            status=ticket ? 0 : Busy;
        }
        break;
    case netdisc::ControlEject:
        if(outLength>=4 && Usable(out,4)) {
            DWORD ticket=disc::Eject();
            *(DWORD*)out=ticket; information=4;
            status=ticket ? 0 : Busy;
        }
        break;
    default:
        status=InvalidRequest;
    }
    Word(irp,IrpInformation)=information;
    Complete(irp,status,0);
    return status;
}
static bool MakeControlDevice() {
    for(int i=0;i<DriverWords;++i) ControlDriver[i]=DWORD(Invalid);
    ControlDriver[DriverDelete]=ControlDriver[DriverDismount]=0;
    ControlDriver[DriverCreate]=DWORD(ControlCreate);
    ControlDriver[DriverClose]=DWORD(ControlClose);
    ControlDriver[DriverControl]=DWORD(ControlDispatch);
    static char name[]="\\Device\\NetDisc360"; // netdisc::DeviceName without its last backslash.
    Text text={USHORT(sizeof(name)-1),USHORT(sizeof(name)),name};
    // Made as NetISO.xex made \Device\NetIso, which titles opened fine.
    if(K.IoCreateDevice(ControlDriver,0,&text,0x14,FALSE,&ControlDevice)<0) { ControlDevice=NULL; return false; }
    Word(ControlDevice,DeviceFlags)|=4;
    Word(ControlDevice,DeviceAlignment)=1;
    Word(ControlDevice,DeviceSectorSize)=Sector;
    Word(ControlDevice,DeviceFlags)&=~DWORD(DeviceInitializing);
    return true;
}

static bool NetIsoLoaded() {
    HANDLE module=NULL;
    return XexGetModuleHandle("NetISO.xex",&module)>=0 && module;
}
// The tray: the SMC's notification list (a handler registered and taken off
// again shows where it is) and xam's tray queries.
static int SetUpTray() {
    memset(SmcRegistration,0,sizeof(SmcRegistration));
    Word(SmcRegistration,0)=DWORD(SmcNoticed);
    K.HalRegisterSMCNotification(SmcRegistration,TRUE);
    SmcHandlers=(Link*)Word(SmcRegistration,8);
    K.HalRegisterSMCNotification(SmcRegistration,FALSE);
    return HookSmc();
}
// At the plugin's start (DllMain), before DashLaunch starts the dashboard,
// in the order NetISO.xex did it: the worker (already running), the tray,
// \Device\Cdrom0 with the media ID, then the launcher's device. Each step
// is logged before it is taken, so a start that hangs shows where.
static bool Activate() {
    Log("Setting up the tray.");
    int smc=SetUpTray();
    if(smc!=1) Log("xam's tray queries hooked: %u (1 expected).",DWORD(smc));
    Log("Taking over \\Device\\Cdrom0.");
    if(!HookDrive()) { UnhookSmc(); SetNote("Off: \\Device\\Cdrom0 couldn't be taken over."); Publish(); return false; }
    Log("Hooking XeKeysGetMediaID.");
    if(!HookMediaId()) Log("XeKeysGetMediaID couldn't be hooked; games get the console's own answer.");
    Log("Making \\Device\\NetDisc360.");
    if(!MakeControlDevice()) {
        UnhookMediaId(); UnhookDrive(); UnhookSmc();
        SetNote("Off: \\Device\\NetDisc360 couldn't be made.");
        Publish();
        return false;
    }
    State=netdisc::StateIdle;
    InterlockedExchange(&Activated,1);
    Log("Reads of up to %u KB, %u at once, receive buffer %u KB; %s.",S.readBytes/1024,DWORD(S.depth),DWORD(S.receiveBytes/1024),
        RealDrive ? "the console's own drive works while no disc from the server is in" : "the console has no drive");
    SetNote("On: this plugin is the disc drive.");
    Publish();
    return true;
}
// On the worker, when the plugin stops: reads still queued fail.
static void Deactivate() {
    Log("Giving the disc drive back.");
    InterlockedExchange(&Activated,0);
    UnhookDrive();
    UnhookMediaId();
    UnhookSmc();
    for(Link* entry;(entry=Take())!=NULL;)
        if(IsCommand(entry)) InterlockedExchange(&((Command*)entry)->used,0);
        else Complete((BYTE*)entry-IrpLink,DeviceError,0);
    if(ControlDevice) { K.IoDeleteDevice(ControlDevice); ControlDevice=NULL; }
    State=netdisc::StateOff;
    Publish();
}

} // namespace

namespace disc {

void Start(const Settings& settings) {
    S=settings;
    if(S.readBytes<Sector || S.readBytes>16*1024*1024) S.readBytes=64*1024;
    S.readBytes&=~(Sector-1);
    if(S.depth<1) S.depth=1;
    if(S.depth>MaxDepth) S.depth=MaxDepth;
    if(S.receiveBytes<0) S.receiveBytes=0;
    ServerAddress=S.address;
    ServerPort=S.port>0 && S.port<65536 ? DWORD(S.port) : DWORD(netdisc::DefaultPort);
    if(!S.enabled) { Put(NoteText,0,sizeof(NoteText),"Off: own_disc=0 in NetDisc360.ini."); return; }
    if(NetIsoLoaded()) { Put(NoteText,0,sizeof(NoteText),"Off: NetISO.xex is loaded, so it is the disc drive."); return; }
    if(!FindCalls()) { Put(NoteText,0,sizeof(NoteText),"Off: a kernel or xam call wasn't found."); return; }
    K.KeInitializeEvent(&QueueEvent,1,FALSE); // A synchronization event.
    Public.version=netdisc::Version; Public.size=sizeof(Public); Public.state=netdisc::StateOff;
    Put(NoteText,0,sizeof(NoteText),"Starting.");
    if(S.about) Log("%s",S.about);
    InterlockedExchange(&Running,1);
    // NetISO.xex's worker had these flags and this priority; this one has a
    // bigger stack.
    DWORD id=0;
    Log("Starting the drive's thread.");
    if(ExCreateThread(&Worker,0x10000,&id,NULL,WorkerMain,NULL,0x200006AA)>=0) K.KeSetBasePriorityThread(Worker,0x10);
    else { Worker=NULL; InterlockedExchange(&Running,0); SetNote("Off: its thread didn't start."); return; }
    if(!Activate()) {
        InterlockedExchange(&Stopping,1);
        K.KeSetEvent(&QueueEvent,1,FALSE);
    }
}

void Stop() {
    if(!Running) return;
    InterlockedExchange(&Stopping,1);
    K.KeSetEvent(&QueueEvent,1,FALSE);
    for(int i=0;i<100 && Running;++i) Delay(50);
}

bool Active() { return Activated!=0; }
const char* Note() { return NoteText; }

DWORD Mount(DWORD address,int port,const char* iso) {
    if(!iso || !iso[0]) return 0;
    return QueueCommand(MountCommand,address,DWORD(port),iso);
}
DWORD Eject() { return QueueCommand(EjectCommand,0,0,NULL); }

void GetStatus(netdisc::Status& status) {
    if(!K.KfAcquireSpinLock) {
        memset(&status,0,sizeof(status));
        status.version=netdisc::Version; status.size=sizeof(status); status.state=netdisc::StateOff;
        Put(status.note,0,sizeof(status.note),NoteText);
        return;
    }
    BYTE irql=K.KfAcquireSpinLock(&Lock);
    memcpy(&status,&Public,sizeof(status));
    K.KfReleaseSpinLock(&Lock,irql);
}

bool Finished(DWORD ticket,DWORD& result) {
    netdisc::Status status; GetStatus(status);
    if(LONG(status.done-ticket)<0) return false;
    result=status.done==ticket ? status.result : DWORD(netdisc::ResultNone);
    return true;
}

}
