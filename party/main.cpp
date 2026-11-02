// SPDX-License-Identifier: MIT
// NetDiscParty.xex: the LAN party on each console. It shows this console's
// name (as NetDisc360 knows it), the guests who are here, and the raffle
// wheel, turning as it turns on the dashboard, the big screen and every other
// console: NetDisc360 plans each spin, and says how far into it it is, so
// all of them stop on the same guest at the same moment. B goes back to Aurora.
//
// The whole party runs from the consoles too (PartyEditAsk), as Jackbox's
// games do: while there's no Gamemaster, any console chooses one from the
// consoles NetDisc360 knows. On every console each player presses A on their
// own controller and types their name (the Guide's keyboard): they're on the
// guest list and here, and get a guest profile. The Gamemaster's console
// spins (Start for teams, X picks one guest) and sets the party up (RB):
// who plays (ticked) and new players, how many teams and their colours, and
// the party's consoles, with "Open the party on all consoles".
//
// Once the wheel drew the teams, the screen turns to this console's players
// (up to four, each in their team's colour) and asks them to sign in one
// after another, as a split-screen game does: each presses A on their own
// controller, and picks their profile in the Guide's sign-in if that
// controller has none yet. A profile named as a guest (the party's guest
// profiles) goes to that guest's seat. NetDisc360 is told who signed in on
// which controller (PartySignIn). Once everyone's in, it goes back to Aurora
// by itself after a few seconds; Y looks at the wheel again.
//
// Each guest's profile comes to the console they sit at while the wheel
// turns: Aurora's FTP server is gone while this app runs, so the app has a
// tiny one of its own (ftp.h) that NetDisc360 copies them through. While
// profiles are copied, B doesn't leave and no one is asked to sign in yet.
//
// It asks NetDisc360 on its NetISO port (4323) and waits there for news
// (PartyWait, a long poll), so it hears of a spin at once without asking
// again and again. The server is the one in party.ini next to this XEX
// (server=<address>[:port]), else the one NetDisc360.xex's drive uses, else
// the one in NetISO.xex.txt on Hdd1: or Usb0:, as the game launcher finds it.
#include <xtl.h>
#include <winsockx.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../shared/isvr.h"
#include "../shared/library.h"
#include "../shared/netdisc.h"
#include "../third_party/imgui/imgui.h"
#include "../launcher/imgui_impl_xbox360.h"
#include "../server/version.h"
#include "../shared/build_id.h"
#include "../plugin/profiles_job.h"
#include "ftp.h"
#include "state.h"
#include "ui.h"

struct KernelString { USHORT length,capacity; char* text; };
extern "C" LONG __stdcall ObCreateSymbolicLink(KernelString* link,KernelString* device);
extern "C" LONG __stdcall ObDeleteSymbolicLink(KernelString* link);
extern "C" LONG __stdcall XexGetModuleHandle(const char* name,HANDLE* module);
extern "C" LONG __stdcall XexGetProcedureAddress(HANDLE module,DWORD ordinal,void** address);

namespace {
// The party's commands (server/main.cpp): wait for news, spin, and say who signed in.
enum { PartyWait = 0x5057, PartySpinAsk = 0x5053, PartySignIn = 0x5049, PartyEditAsk = 0x5045, PartyJobAsk = 0x504A, SignedOut = 0x8000, WaitMs = 25000, TextMost = 64 << 10 };
// How long the screen stays on the wheel after it stopped, before it turns to
// the players (a pick's result is read longer), and how long it waits once
// everyone's in before it goes back to Aurora.
const DWORD PlayersAfterTeamsMs = 4000, PlayersAfterPickMs = 9000, LeaveMs = 10000;

// What the network thread found, under Lock; the main thread draws it.
CRITICAL_SECTION Lock;
party::State Latest;          // The newest party read.
DWORD LatestAt=0;             // When it came (GetTickCount()).
bool HaveLatest=false;
int LinkNow=party::LinkLooking;
char ServerText[32]="";
DWORD ServerAddress=0; int ServerPort=nia::IsvrPort;
char Notice[160]="";          // What the server said to the last spin or change.
bool NoticeGood=false;        // It's good news ("Welcome, Ola!"), not a problem.
bool FtpFailed=false;         // The FTP server for guest profiles couldn't start.
DWORD NoticeAt=0;

// ---- Where the server is (as launcher/main.cpp finds it) ----

LONG LinkDrive(const char* name,const char* device) {
    char nameText[32],deviceText[64]; strcpy_s(nameText,name); strcpy_s(deviceText,device);
    KernelString link={USHORT(strlen(nameText)),USHORT(sizeof(nameText)),nameText};
    KernelString target={USHORT(strlen(deviceText)),USHORT(sizeof(deviceText)),deviceText};
    ObDeleteSymbolicLink(&link);
    return ObCreateSymbolicLink(&link,&target);
}
void UnlinkDrive(const char* name) {
    char nameText[32]; strcpy_s(nameText,name);
    KernelString link={USHORT(strlen(nameText)),USHORT(sizeof(nameText)),nameText};
    ObDeleteSymbolicLink(&link);
}
bool ReadText(const char* path,char* text,DWORD capacity) {
    text[0]=0;
    HANDLE file=CreateFile(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file==INVALID_HANDLE_VALUE) return false;
    DWORD done=0; BOOL ok=ReadFile(file,text,capacity-1,&done,NULL);
    CloseHandle(file);
    text[ok ? done : 0]=0;
    return ok!=FALSE;
}
// "192.168.1.20" or "192.168.1.20:4323".
bool ParseServer(const char* text,DWORD& address,int& port) {
    char host[64];
    if(strlen(text)>=sizeof(host)) return false;
    strcpy_s(host,text);
    char* colon=strchr(host,':');
    if(colon) { *colon=0; port=atoi(colon+1); if(port<1||port>65535) return false; }
    size_t length=strlen(host);
    while(length && (host[length-1]==' '||host[length-1]=='\t'||host[length-1]=='\r')) host[--length]=0;
    address=inet_addr(host);
    return address!=INADDR_NONE && address!=0;
}

// NetDisc360.xex's drive, asked for the server it uses (shared/netdisc.h).
struct DriveIo { LONG status; ULONG_PTR information; };
struct DriveAttributes { HANDLE root; KernelString* name; ULONG attributes; };
typedef LONG (*DriveOpenCall)(HANDLE* handle,ACCESS_MASK access,DriveAttributes* attributes,DriveIo* io,ULONG share,ULONG options);
typedef LONG (*DriveControlCall)(HANDLE handle,HANDLE event,void* apc,void* context,DriveIo* io,ULONG code,void* in,ULONG inLength,void* out,ULONG outLength);
typedef LONG (*DriveCloseCall)(HANDLE handle);
bool DriveServer(DWORD& address,int& port) {
    HANDLE kernel=NULL; void* open=NULL; void* control=NULL; void* close=NULL;
    // xboxkrnl.exe: NtOpenFile 223, NtDeviceIoControlFile 217, NtClose 207.
    if(XexGetModuleHandle("xboxkrnl.exe",&kernel)<0 || XexGetProcedureAddress(kernel,223,&open)<0
       || XexGetProcedureAddress(kernel,217,&control)<0 || XexGetProcedureAddress(kernel,207,&close)<0 || !open || !control || !close) return false;
    static const char* const names[2]={"\\Device\\NetDisc360",netdisc::DeviceName};
    for(int i=0;i<2;++i) {
        char text[32]; strcpy_s(text,names[i]);
        KernelString name={USHORT(strlen(text)),USHORT(sizeof(text)),text};
        DriveAttributes attributes={NULL,&name,0x40};
        DriveIo io={0,0};
        HANDLE handle=NULL;
        if(((DriveOpenCall)open)(&handle,0x100001,&attributes,&io,FILE_SHARE_READ|FILE_SHARE_WRITE,0x20)<0) continue;
        netdisc::Status s; memset(&s,0,sizeof(s));
        DriveIo asked={0,0};
        bool known=((DriveControlCall)control)(handle,NULL,NULL,NULL,&asked,netdisc::ControlStatus,NULL,0,&s,sizeof(s))>=0 && s.version==netdisc::Version && s.address;
        ((DriveCloseCall)close)(handle);
        if(known) { address=s.address; port=int(s.port ? s.port : nia::IsvrPort); return true; }
    }
    return false;
}

bool FindServer(DWORD& address,int& port) {
    static char text[nia::IniMax];
    char value[64];
    if(ReadText("game:\\party.ini",text,sizeof(text)) && nia::IniValue(text,"server",value,sizeof(value)) && ParseServer(value,address,port)) return true;
    if(DriveServer(address,port)) return true;
    static const struct { const char* link; const char* device; } drives[]={
        {"\\??\\PartyH:","\\Device\\Harddisk0\\Partition1"},{"\\??\\PartyU:","\\Device\\Mass0"}};
    for(int i=0;i<2;++i) {
        if(LinkDrive(drives[i].link,drives[i].device)<0) continue;
        char path[64]; sprintf_s(path,"%s\\netiso.xex.txt",drives[i].link+4);
        char line[128];
        bool found=ReadText(path,line,sizeof(line));
        UnlinkDrive(drives[i].link);
        if(!found) continue;
        line[strcspn(line,"\r\n")]=0;
        port=nia::IsvrPort;
        if(ParseServer(line,address,port)) return true;
    }
    return false;
}

void SetLink(int link) { EnterCriticalSection(&Lock); LinkNow=link; LeaveCriticalSection(&Lock); }

// ---- The network thread: waits on the server for news, again and again ----

DWORD WINAPI Listen(void*) {
    XNetStartupParams params; memset(&params,0,sizeof(params));
    params.cfgSizeOfStruct=sizeof(params); params.cfgFlags=XNET_STARTUP_BYPASS_SECURITY;
    WSADATA wsa;
    SetLink(party::LinkWaiting);
    if(XNetStartup(&params) || WSAStartup(MAKEWORD(2,2),&wsa)) return 0;
    XNADDR mine;
    while(XNetGetTitleXnAddr(&mine)==XNET_GET_XNADDR_PENDING) Sleep(250);
    // The FTP server NetDisc360 copies guest profiles through (ftp.h): on
    // Aurora's port (party.ini ftp=<port> for another), for no one until the
    // server is found.
    {
        static char ini[nia::IniMax];
        char value[16];
        int port=party::FtpPort;
        if(ReadText("game:\\party.ini",ini,sizeof(ini)) && nia::IniValue(ini,"ftp",value,sizeof(value)) && atoi(value)>0 && atoi(value)<65536) port=atoi(value);
        if(LinkDrive("\\??\\PartyF:","\\Device\\Harddisk0\\Partition1")<0 || !party::FtpStart("PartyF:",port,INADDR_NONE)) FtpFailed=true;
    }
    static char text[TextMost];
    static party::State read;
    unsigned long long version=0;
    nia::IsvrClient client(WaitMs/1000.0+10);
    for(;;) {
        DWORD address=0; int port=nia::IsvrPort;
        if(!ServerAddress) {
            SetLink(party::LinkLooking);
            if(!FindServer(address,port)) { Sleep(5000); continue; }
            EnterCriticalSection(&Lock);
            ServerAddress=address; ServerPort=port;
            party::FtpAllowFrom(address);
            nia::AddressText(address,ServerText,sizeof(ServerText));
            LeaveCriticalSection(&Lock);
        }
        if(!client.Connected() && !client.Connect(ServerAddress,ServerPort,5)) {
            SetLink(party::LinkLost);
            // The address may have changed (another server, a new NetISO.xex.txt).
            ServerAddress=0;
            Sleep(3000);
            continue;
        }
        DWORD size=0;
        // The index says which build this app is, so the server knows an old one (WheelUpdate).
        if(!client.Text(PartyWait,NETDISC_BUILD_STAMP,version,WaitMs,text,sizeof(text),size)) { SetLink(party::LinkLost); client.Close(); Sleep(2000); continue; }
        DWORD at=GetTickCount();
        if(!party::Parse(text,size,read)) {
            // Not a NetDisc360 that knows the party (an older one, or "error ...").
            SetLink(party::LinkLost); client.Close(); Sleep(5000); continue;
        }
        version=read.version;
        EnterCriticalSection(&Lock);
        Latest=read; LatestAt=at; HaveLatest=true; LinkNow=party::LinkConnected;
        LeaveCriticalSection(&Lock);
    }
}

// The Gamemaster's spin, on a connection of its own (the other one is waiting).
DWORD WINAPI AskSpin(void* kind) {
    EnterCriticalSection(&Lock);
    DWORD address=ServerAddress; int port=ServerPort;
    LeaveCriticalSection(&Lock);
    char said[512]="";
    if(address) {
        nia::IsvrClient client(10);
        DWORD size=0;
        if(!client.Connect(address,port,5) || !client.Text(PartySpinAsk,DWORD(ULONG_PTR(kind)),0,0,said,sizeof(said),size)) strcpy_s(said,"error\tNetDisc360 didn't answer. Try again.");
        client.Close();
    } else strcpy_s(said,"error\tNo NetDisc360 found yet.");
    said[strcspn(said,"\n")]=0;
    EnterCriticalSection(&Lock);
    const char* tab=strchr(said,'\t');
    if(!strncmp(said,"error",5) && tab) { strcpy_s(Notice,tab+1); NoticeAt=GetTickCount(); NoticeGood=false; }
    else Notice[0]=0;
    LeaveCriticalSection(&Lock);
    return 0;
}

// Changes to the party from this console (PartyEditAsk), sent one after
// another in the order asked, by a thread of their own.
enum { EditsQueued = 16, EditBytes = 256 };
char Edits[EditsQueued][EditBytes];
int EditFirst=0, EditCount=0;  // Under Lock.
void QueueEdit(const char* format, ...) {
    char text[EditBytes];
    va_list a; va_start(a,format); _vsnprintf_s(text,sizeof(text),_TRUNCATE,format,a); va_end(a);
    EnterCriticalSection(&Lock);
    if(EditCount<EditsQueued) { strcpy_s(Edits[(EditFirst+EditCount)%EditsQueued],text); ++EditCount; }
    LeaveCriticalSection(&Lock);
}
DWORD WINAPI SendEdits(void*) {
    nia::IsvrClient client(10);
    for(;;) {
        char text[EditBytes];
        EnterCriticalSection(&Lock);
        bool have=EditCount>0;
        if(have) { strcpy_s(text,Edits[EditFirst]); EditFirst=(EditFirst+1)%EditsQueued; --EditCount; }
        DWORD address=ServerAddress; int port=ServerPort;
        LeaveCriticalSection(&Lock);
        if(!have) { Sleep(40); continue; }
        char said[512]="";
        DWORD size=0, length=DWORD(strlen(text));
        bool ok=address && (client.Connected() || client.Connect(address,port,5)) &&
                client.Text(PartyEditAsk,0,0,length,said,sizeof(said),size,text);
        if(!ok) { client.Close(); strcpy_s(said,address ? "error\tNetDisc360 didn't answer. Try again." : "error\tNo NetDisc360 found yet."); }
        said[strcspn(said,"\n")]=0;
        const char* tab=strchr(said,'\t');
        EnterCriticalSection(&Lock);
        if(tab && tab[1]) { strcpy_s(Notice,tab+1); NoticeAt=GetTickCount(); NoticeGood=strncmp(said,"error",5)!=0; }
        LeaveCriticalSection(&Lock);
    }
}

// Guest profiles made here, when the server asks (state.profileJob): while
// every party console shows this app, none can start NetDisc360's helper
// from Aurora, so this app makes them as the helper does (the plugin's
// profilejob::MakeFromJob), with the job and its gamer pictures from the
// server (PartyJobAsk index 0: the job; 1: a picture, its file name sent;
// 2: the answer sent). The server copies them to the other consoles.
char JobSeen[24]="";
volatile LONG JobBusy=0;
// Each request on a connection of its own: making a profile takes a while.
struct JobServer { DWORD address; int port; };
bool JobAsk(const JobServer& server, int index, const char* body, DWORD length, char* reply, DWORD capacity, DWORD& size) {
    nia::IsvrClient client(20);
    return server.address && client.Connect(server.address,server.port,5) && client.Text(PartyJobAsk,DWORD(index),0,length,reply,capacity,size,body);
}
int ReadJobPicture(void* with, const char* name, void* into, unsigned long capacity) {
    DWORD size=0;
    if(capacity<2 || !JobAsk(*static_cast<JobServer*>(with),1,name,DWORD(strlen(name)),static_cast<char*>(into),DWORD(capacity),size)) return -1;
    return int(size);
}
DWORD WINAPI MakeJob(void*) {
    JobServer server;
    EnterCriticalSection(&Lock);
    server.address=ServerAddress; server.port=ServerPort;
    strcpy_s(Notice,"Making guest profiles, one moment..."); NoticeAt=GetTickCount(); NoticeGood=true;
    LeaveCriticalSection(&Lock);
    static char job[4096], done[8192], said[256];
    DWORD size=0;
    int made=0;
    if(JobAsk(server,0,NULL,0,job,sizeof(job),size) && size) {
        size_t used=profilejob::MakeFromJob(job,ReadJobPicture,&server,done,sizeof(done),"party",&made);
        JobAsk(server,2,done,DWORD(used),said,sizeof(said),size);
    }
    EnterCriticalSection(&Lock);
    if(made) { sprintf_s(Notice,made==1 ? "1 guest profile made" : "%d guest profiles made",made); NoticeAt=GetTickCount(); NoticeGood=true; }
    LeaveCriticalSection(&Lock);
    InterlockedExchange(&JobBusy,0);
    return 0;
}

// The Guide's keyboard, for a name: a player joining (on their own
// controller), or the Gamemaster adding one in the setup.
enum KeyFor { KeyJoin, KeyAdd };
XOVERLAPPED KeyWait;
WCHAR KeyText[40];
int KeyPad=-1, KeyPurpose=KeyJoin;
bool OpenKeyboard(int pad, int purpose) {
    if(KeyPad>=0) return false;
    // A signed-in profile's gamertag to start from, when joining.
    WCHAR start[24]=L"";
    char gamertag[party::GamertagBytes];
    if(purpose==KeyJoin && XUserGetSigninState(DWORD(pad))!=eXUserSigninState_NotSignedIn && XUserGetName(DWORD(pad),gamertag,sizeof(gamertag))==ERROR_SUCCESS)
        for(int i=0;i<23 && gamertag[i];++i) { start[i]=WCHAR((unsigned char)gamertag[i]); start[i+1]=0; }
    ZeroMemory(&KeyWait,sizeof(KeyWait));
    KeyText[0]=0;
    DWORD r=XShowKeyboardUI(DWORD(pad),0,start,purpose==KeyJoin ? L"Join the party" : L"Add a player",
                            purpose==KeyJoin ? L"Type your name or gamertag" : L"Type the player's name",KeyText,32,&KeyWait);
    if(r!=ERROR_IO_PENDING && r!=ERROR_SUCCESS) {
        EnterCriticalSection(&Lock);
        strcpy_s(Notice,"The keyboard didn't open. Sign in on that controller, or join on the dashboard.");
        NoticeAt=GetTickCount(); NoticeGood=false;
        LeaveCriticalSection(&Lock);
        return false;
    }
    KeyPad=pad; KeyPurpose=purpose;
    return true;
}
// Each frame: the name typed, once the keyboard closed.
void WatchKeyboard() {
    if(KeyPad<0 || !XHasOverlappedIoCompleted(&KeyWait)) return;
    int purpose=KeyPurpose, pad=KeyPad;
    KeyPad=-1;
    if(XGetOverlappedResult(&KeyWait,NULL,FALSE)!=ERROR_SUCCESS) return;  // Cancelled.
    // As UTF-8, trimmed.
    char name[128]; size_t n=0;
    for(int i=0;KeyText[i] && n+4<sizeof(name);++i) {
        unsigned c=KeyText[i];
        if(c<0x20) continue;
        if(c<0x80) name[n++]=char(c);
        else if(c<0x800) { name[n++]=char(0xC0|(c>>6)); name[n++]=char(0x80|(c&0x3F)); }
        else { name[n++]=char(0xE0|(c>>12)); name[n++]=char(0x80|((c>>6)&0x3F)); name[n++]=char(0x80|(c&0x3F)); }
    }
    name[n]=0;
    char* at=name; while(*at==' ') ++at;
    for(size_t k=strlen(at);k && at[k-1]==' ';--k) at[k-1]=0;
    if(!*at) return;
    // Joined signed in to a profile: the server copies it to the party's
    // other consoles (one made here by hand, say).
    ULONGLONG xuid=0;
    char gamertag[party::GamertagBytes]="";
    if(purpose==KeyJoin && XUserGetSigninState(DWORD(pad))!=eXUserSigninState_NotSignedIn && XUserGetXUID(DWORD(pad),&xuid)==ERROR_SUCCESS && xuid) {
        if(XUserGetName(DWORD(pad),gamertag,sizeof(gamertag))!=ERROR_SUCCESS) gamertag[0]=0;
        for(char* c=gamertag;*c;++c) if(*c=='\t' || *c=='\n') *c=' ';
        QueueEdit("join\t%s\t%016I64X\t%s",at,xuid,gamertag);
    } else QueueEdit(purpose==KeyJoin ? "join\t%s" : "add\t%s",at);
}


// A sign-in (or a controller signed out), told to the server on a connection of its own.
struct SignInNews { int seat, controller; bool out; ULONGLONG xuid; char gamertag[party::GamertagBytes]; };
DWORD WINAPI TellSignIn(void* news) {
    SignInNews* n=static_cast<SignInNews*>(news);
    EnterCriticalSection(&Lock);
    DWORD address=ServerAddress; int port=ServerPort;
    LeaveCriticalSection(&Lock);
    if(address) {
        nia::IsvrClient client(10);
        char said[256]; DWORD size=0;
        DWORD length=n->out ? 0 : DWORD(strlen(n->gamertag));
        DWORD index=DWORD(n->seat&0xFF)|(DWORD(n->controller&3)<<8)|(n->out ? DWORD(SignedOut) : 0);
        if(client.Connect(address,port,5)) client.Text(PartySignIn,index,n->xuid,length,said,sizeof(said),size,length ? n->gamertag : NULL);
        client.Close();
    }
    delete n;
    return 0;
}
void Tell(int seat, int controller, bool out, ULONGLONG xuid, const char* gamertag) {
    SignInNews* n=new SignInNews;
    n->seat=seat; n->controller=controller; n->out=out; n->xuid=xuid;
    strcpy_s(n->gamertag,gamertag ? gamertag : "");
    HANDLE thread=CreateThread(NULL,0,TellSignIn,n,0,NULL);
    if(thread) CloseHandle(thread); else delete n;
}

// ---- The screen ----

IDirect3DDevice9* Screen=NULL;
HANDLE Notifications=NULL;
bool ScreenStart() {
    party::InitUi();
    IDirect3D9* d3d=Direct3DCreate9(D3D_SDK_VERSION);
    if(!d3d) return false;
    D3DPRESENT_PARAMETERS pp; ZeroMemory(&pp,sizeof(pp));
    pp.BackBufferWidth=1280; pp.BackBufferHeight=720; pp.BackBufferFormat=D3DFMT_A8R8G8B8;
    pp.FrontBufferFormat=D3DFMT_LE_X8R8G8B8; pp.BackBufferCount=1;
    pp.MultiSampleType=D3DMULTISAMPLE_NONE; pp.SwapEffect=D3DSWAPEFFECT_DISCARD;
    pp.PresentationInterval=D3DPRESENT_INTERVAL_ONE;
    if(FAILED(d3d->CreateDevice(0,D3DDEVTYPE_HAL,NULL,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&Screen))) Screen=NULL;
    d3d->Release();
    if(Screen && !ImGuiXbox360Init(Screen)) { Screen->Release(); Screen=NULL; }
    return Screen!=NULL;
}
// A controller's buttons; the left stick counts as the d-pad.
WORD Buttons(DWORD pad) {
    XINPUT_STATE state; ZeroMemory(&state,sizeof(state));
    if(XInputGetState(pad,&state)!=ERROR_SUCCESS) return 0;
    WORD b=state.Gamepad.wButtons;
    const SHORT x=state.Gamepad.sThumbLX, y=state.Gamepad.sThumbLY, edge=20000;
    if(y>edge) b|=XINPUT_GAMEPAD_DPAD_UP;
    if(y<-edge) b|=XINPUT_GAMEPAD_DPAD_DOWN;
    if(x<-edge) b|=XINPUT_GAMEPAD_DPAD_LEFT;
    if(x>edge) b|=XINPUT_GAMEPAD_DPAD_RIGHT;
    return b;
}
// The d-pad's directions pressed now, or held long enough to repeat.
const WORD Directions=XINPUT_GAMEPAD_DPAD_UP|XINPUT_GAMEPAD_DPAD_DOWN|XINPUT_GAMEPAD_DPAD_LEFT|XINPUT_GAMEPAD_DPAD_RIGHT;
WORD Repeated(WORD down, WORD pressed, DWORD now) {
    static WORD was=0; static DWORD since=0, last=0;
    WORD dir=WORD(down&Directions);
    if(pressed&Directions) { was=dir; since=last=now; return WORD(pressed&Directions); }
    if(!dir || dir!=was) { was=dir; since=last=now; return 0; }
    if(now-since>=400 && now-last>=110) { last=now; return dir; }
    return 0;
}

// The Gamemaster's setup (Screen::setup): a button on the picked row.
void SetupMove(party::Screen& f, int by) {
    int rows=party::SetupRows(f.state,f.setupTab);
    f.setupRow+=by;
    if(f.setupRow<0) f.setupRow=0;
    if(f.setupRow>=rows) f.setupRow=rows-1;
    if(f.setupRow<f.setupTop) f.setupTop=f.setupRow;
    if(f.setupRow>=f.setupTop+party::SetupShown) f.setupTop=f.setupRow-party::SetupShown+1;
}
// Left and right (by -1, 1), or A (by 0: on to the next) on the Teams tab.
void SetupChange(party::Screen& f, int by) {
    const party::State& s=f.state;
    if(f.setupTab!=party::TabTeams) return;
    if(f.setupRow==0) {
        int n=by ? s.teams+by : (s.teams>=party::TeamsMost ? 2 : s.teams+1);
        if(n>=2 && n<=party::TeamsMost) QueueEdit("teams\t%d",n);
        return;
    }
    int t=f.setupRow-1, count=s.paletteCount>0 ? s.paletteCount : party::TeamsMost;
    int c=(s.teamColour[t]+(by ? by : 1)+count)%count;
    QueueEdit("colour\t%d\t%d",t,c);
}
void SetupPress(party::Screen& f, int pad) {
    const party::State& s=f.state;
    const int r=f.setupRow;
    if(f.setupTab==party::TabPlayers) {
        if(r==0) OpenKeyboard(pad,KeyAdd);
        else if(r-1<s.guestCount) QueueEdit("here\t%s\t%d",s.guests[r-1].name,s.guests[r-1].here ? 0 : 1);
    } else if(f.setupTab==party::TabTeams) SetupChange(f,0);
    else if(r==0) QueueEdit("openall");
    else if(r-1<s.knownCount) QueueEdit("console\t%s\t%d",s.known[r-1].address,s.known[r-1].inParty ? 0 : 1);
}
bool SignedIn(int pad) { return XUserGetSigninState(DWORD(pad))!=eXUserSigninState_NotSignedIn; }

// ---- Signing in, on the players screen ----

party::Screen Frame;
bool ChooserOpen=false;     // The Guide's sign-in (or any of its screens) is up.
DWORD ChooserClosedAt=0;
DWORD SignedBefore=0;       // The controllers signed in when the chooser opened (bits).
bool SignedHere=false;      // Someone signed in on this screen (so everyone's in starts the countdown).
bool Stay=false;            // Y: no going back to Aurora by itself.
DWORD AllInAt=0;

int SeatOf(int pad) {
    for(int i=0;i<party::PerConsole;++i) if(Frame.players[i].controller==pad) return i;
    return -1;
}
int Asking(const party::Seat* mine) {
    for(int i=0;mine && i<mine->count;++i) if(Frame.players[i].controller<0) return i;
    return -1;
}
// Seats the profile signed in on pad: the seat of the guest it's named for,
// else the one asked now.
void Take(int pad, const party::Seat* mine) {
    if(!mine || SeatOf(pad)>=0) return;
    char gamertag[party::GamertagBytes]="";
    if(XUserGetName(DWORD(pad),gamertag,sizeof(gamertag))!=ERROR_SUCCESS) gamertag[0]=0;
    XUID xuid=0;
    if(XUserGetXUID(DWORD(pad),&xuid)!=ERROR_SUCCESS) xuid=0;
    int seat=-1;
    for(int i=0;i<mine->count && seat<0;++i) if(Frame.players[i].controller<0 && party::GamertagOf(gamertag,mine->names[i])) seat=i;
    if(seat<0) seat=Asking(mine);
    if(seat<0) return;
    Frame.players[seat].controller=pad;
    strcpy_s(Frame.players[seat].gamertag,gamertag);
    SignedHere=true;
    Tell(seat,pad,false,xuid,gamertag);
}
// A in the players screen, on pad: a signed-in controller joins at once;
// one without a profile opens the Guide's sign-in for it.
void SignInPad(int pad, const party::Seat* mine) {
    if(SeatOf(pad)>=0 || Frame.choosing>=0 || Asking(mine)<0) return;
    if(SignedIn(pad)) { Take(pad,mine); return; }
    SignedBefore=0;
    for(int p=0;p<4;++p) if(SignedIn(p)) SignedBefore|=1u<<p;
    if(XShowSigninUI(1,XSSUI_FLAGS_LOCALSIGNINONLY)==ERROR_SUCCESS) { Frame.choosing=pad; ChooserClosedAt=0; }
}
// Each frame: a profile chosen in the Guide, controllers signed out, and
// everyone's in.
void WatchSignIns(const party::Seat* mine, DWORD now) {
    if(Frame.choosing>=0) {
        for(int p=0;p<4;++p)
            if(!(SignedBefore&(1u<<p)) && SignedIn(p) && SeatOf(p)<0) { Take(p,mine); Frame.choosing=-1; break; }
        // Closed with no one signed in (its news can come a moment after it closes).
        if(Frame.choosing>=0 && !ChooserOpen && ChooserClosedAt && now-ChooserClosedAt>1500) Frame.choosing=-1;
    }
    for(int i=0;i<party::PerConsole;++i) {
        int pad=Frame.players[i].controller;
        if(pad>=0 && !SignedIn(pad)) { Frame.players[i].controller=-1; Frame.players[i].gamertag[0]=0; Tell(i,pad,true,0,NULL); }
    }
    Frame.asking=Asking(mine);
    if(Frame.asking<0 && mine && mine->count && SignedHere && !Stay && !Frame.copying) {
        if(!AllInAt) AllInAt=now;
        DWORD gone=now-AllInAt;
        Frame.leaveIn=gone>=LeaveMs ? 0 : int((LeaveMs-gone+999)/1000);
    } else { AllInAt=0; Frame.leaveIn=-1; }
}
// New seats here: no one signed in on them, but those the server says are
// (this screen opened again) whose controller still has a profile.
void NewSeats(const party::Seat* mine) {
    for(int i=0;i<party::PerConsole;++i) { Frame.players[i].controller=-1; Frame.players[i].gamertag[0]=0; }
    for(int k=0;k<Frame.state.signedCount;++k) {
        const party::SignedIn& in=Frame.state.signedIn[k];
        if(!mine || in.seat>=mine->count || !SignedIn(in.controller) || SeatOf(in.controller)>=0) continue;
        Frame.players[in.seat].controller=in.controller;
        strcpy_s(Frame.players[in.seat].gamertag,in.gamertag);
    }
    Frame.choosing=-1; SignedHere=false; Stay=false; AllInAt=0; Frame.leaveIn=-1;
}
}

void __cdecl main() {
    InitializeCriticalSection(&Lock);
    // Before any thread or frame asks the FTP server anything (FtpNow).
    party::FtpInit();
    // A title that reads no notifications can leave the Guide dead (seen
    // with the launcher), so this reads them, as a game does.
    Notifications=XNotifyCreateListener(XNOTIFY_ALL);
    if(!ScreenStart()) { XLaunchNewImage(XLAUNCH_KEYWORD_DEFAULT_APP,0); return; }
    HANDLE listener=CreateThread(NULL,0,Listen,NULL,0,NULL);
    if(listener) CloseHandle(listener);
    party::Screen& frame=Frame;
    unsigned shownSpin=0;
    DWORD spinStart=0, started=GetTickCount(), askedAt=0, updatingAt=0;
    _snprintf_s(frame.version,sizeof(frame.version),_TRUNCATE,"v%s%s%s",NETDISC_VERSION,NETDISC_COMMIT[0] ? "  " : "",NETDISC_COMMIT);
    HANDLE edits=CreateThread(NULL,0,SendEdits,NULL,0,NULL);
    if(edits) CloseHandle(edits);
    WORD held[4];  // The A that started this may still be down.
    for(DWORD pad=0;pad<4;++pad) held[pad]=Buttons(pad);
    char seatKey[party::PerConsole*party::NameBytes+1]="\x01";  // Whose seats these are: none read yet.
    bool turnedToPlayers=false;
    for(;;) {
        DWORD id; ULONG_PTR param;
        while(Notifications && XNotifyGetNext(Notifications,0,&id,&param))
            if(id==XN_SYS_UI) { ChooserOpen=param!=0; if(!ChooserOpen) ChooserClosedAt=GetTickCount(); }
        WORD pressed[4], any=0;
        for(DWORD pad=0;pad<4;++pad) { WORD b=Buttons(pad); pressed[pad]=WORD(b&~held[pad]); held[pad]=b; any|=pressed[pad]; }
        DWORD now=GetTickCount();
        bool newSpin=false;
        EnterCriticalSection(&Lock);
        if(HaveLatest) {
            frame.state=Latest;
            // When the spin started, by this console's clock: the earliest
            // guess is the best (the answer that took least long to come).
            DWORD start=LatestAt-DWORD(Latest.elapsed);
            if(Latest.spinId!=shownSpin) { shownSpin=Latest.spinId; spinStart=start; newSpin=true; }
            else if(int(start-spinStart)<0) spinStart=start;
            HaveLatest=false;
        }
        // Profiles to make here (once per job).
        if(frame.state.profileJob[0] && strcmp(frame.state.profileJob,JobSeen) && !JobBusy) {
            strcpy_s(JobSeen,frame.state.profileJob);
            InterlockedExchange(&JobBusy,1);
            HANDLE maker=CreateThread(NULL,0,MakeJob,NULL,0,NULL);
            if(maker) CloseHandle(maker); else InterlockedExchange(&JobBusy,0);
        }
        frame.link=LinkNow;
        strcpy_s(frame.server,ServerText);
        if(Notice[0] && now-NoticeAt>8000) Notice[0]=0;
        strcpy_s(frame.notice,Notice);
        frame.noticeGood=NoticeGood;
        if(!frame.notice[0] && FtpFailed) { strcpy_s(frame.notice,"Guest profiles can't be copied here: the FTP port is taken."); frame.noticeGood=false; }
        LeaveCriticalSection(&Lock);
        frame.spinMs=frame.state.spinId ? double(int(now-spinStart)) : 0;
        frame.time=(now-started)/1000.0;
        // This console's seats: new ones start the signing in again.
        const party::Seat* mine=frame.state.valid ? party::MySeat(frame.state) : NULL;
        char key[sizeof(seatKey)]="";
        for(int i=0;mine && i<mine->count;++i) { strcat_s(key,mine->names[i]); strcat_s(key,"\t"); }
        if(strcmp(key,seatKey)) { strcpy_s(seatKey,key); NewSeats(mine); turnedToPlayers=false; }
        // A new spin is watched on the wheel; a while after it stops, the screen turns to the players.
        if(newSpin) { frame.view=party::ViewWheel; turnedToPlayers=false; }
        const bool spun=frame.state.spinId && frame.state.stepCount>0;
        const double after=spun ? frame.spinMs-party::SpinMs(frame.state) : 1e9;
        if(!turnedToPlayers && mine && mine->count && after>=(strcmp(frame.state.kind,"pick") ? PlayersAfterTeamsMs : PlayersAfterPickMs)) {
            turnedToPlayers=true; frame.view=party::ViewPlayers;
        }
        // Guest profiles being copied here (by NetDisc360's word, or its FTP
        // connection busy now or a moment ago): no leaving, no signing in yet.
        party::FtpActivity ftp;
        party::FtpNow(ftp);
        frame.copying=JobBusy!=0 || ftp.sessions>0 || ftp.transfers>0 || (ftp.lastAt && now-ftp.lastAt<3000) ||
                      (frame.link==party::LinkConnected && mine && mine->count && !strcmp(frame.state.profiles,"copying"));
        WatchSignIns(mine,now);
        WatchKeyboard();
        frame.typing=KeyPad;
        WORD down=0;
        for(int pad=0;pad<4;++pad) down|=held[pad];
        const WORD moved=Repeated(down,any,now);
        const party::State& st=frame.state;
        const bool turning=spun && after<0;
        // This app makes way for the server's newer one, when nothing is under way.
        if(st.valid && st.update && !frame.updating && !frame.copying && !turning && KeyPad<0 && !ChooserOpen) {
            frame.updating=true; updatingAt=now; frame.setup=false;
            QueueEdit("reopen");
        }
        if(frame.updating) { if(now-updatingAt>2500) break; }
        else if(KeyPad>=0 || ChooserOpen) {}  // The Guide has the controllers.
        else if(frame.setup && st.valid && st.gamemaster) {
            // The Gamemaster's setup: B closes it, LB and RB change tabs.
            if(any&(XINPUT_GAMEPAD_B|XINPUT_GAMEPAD_BACK)) frame.setup=false;
            int tab=frame.setupTab+((any&XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 1 : 0)-((any&XINPUT_GAMEPAD_LEFT_SHOULDER) ? 1 : 0);
            if(tab!=frame.setupTab) { frame.setupTab=(tab+party::TabsCount)%party::TabsCount; frame.setupRow=frame.setupTop=0; }
            if(moved&XINPUT_GAMEPAD_DPAD_UP) SetupMove(frame,-1);
            if(moved&XINPUT_GAMEPAD_DPAD_DOWN) SetupMove(frame,1);
            SetupMove(frame,0);  // The list may have shrunk.
            if(moved&XINPUT_GAMEPAD_DPAD_LEFT) SetupChange(frame,-1);
            if(moved&XINPUT_GAMEPAD_DPAD_RIGHT) SetupChange(frame,1);
            for(int pad=0;pad<4;++pad) if(pressed[pad]&XINPUT_GAMEPAD_A) { SetupPress(frame,pad); break; }
            if((any&XINPUT_GAMEPAD_Y) && frame.setupTab==party::TabConsoles && frame.setupRow>0 && frame.setupRow-1<st.knownCount)
                QueueEdit("gamemaster\t%s",st.known[frame.setupRow-1].address);
        } else {
            if(frame.setup) frame.setup=false;  // No longer the Gamemaster's.
            if((any&(XINPUT_GAMEPAD_B|XINPUT_GAMEPAD_BACK)) && !frame.copying) break;
            if(frame.view==party::ViewPlayers && mine && mine->count) {
                if(any&XINPUT_GAMEPAD_Y) { frame.view=party::ViewWheel; Stay=true; }
                for(int pad=0;pad<4;++pad) if(pressed[pad]&XINPUT_GAMEPAD_A) {
                    if(frame.asking>=0) { if(!frame.copying) SignInPad(pad,mine); }
                    else if(frame.leaveIn>=0) frame.leaveIn=0;
                }
                if(frame.leaveIn==0 && !frame.copying) break;
            } else if(st.valid && !st.gamemasterName[0]) {
                // No Gamemaster yet: choose one of the consoles that are on.
                int choices[party::KnownMost];
                int n=party::HostChoices(st,choices);
                if(moved&XINPUT_GAMEPAD_DPAD_UP) --frame.hostRow;
                if(moved&XINPUT_GAMEPAD_DPAD_DOWN) ++frame.hostRow;
                if(frame.hostRow>=n) frame.hostRow=n-1;
                if(frame.hostRow<0) frame.hostRow=0;
                if((any&XINPUT_GAMEPAD_A) && n) QueueEdit("gamemaster\t%s",st.known[choices[frame.hostRow]].address);
            } else if(st.valid) {
                if((any&XINPUT_GAMEPAD_Y) && mine && mine->count && !turning) frame.view=party::ViewPlayers;
                // Each player joins on their own controller: A, then their name.
                if(!turning) for(int pad=0;pad<4;++pad) if(pressed[pad]&XINPUT_GAMEPAD_A) { OpenKeyboard(pad,KeyJoin); break; }
                if(st.gamemaster && !turning) {
                    if(any&XINPUT_GAMEPAD_RIGHT_SHOULDER) { frame.setup=true; frame.setupTab=party::TabPlayers; frame.setupRow=frame.setupTop=0; }
                    // The Gamemaster spins: Start for teams, X to pick one guest; once a second at most.
                    if((any&(XINPUT_GAMEPAD_START|XINPUT_GAMEPAD_X)) && now-askedAt>1000) {
                        askedAt=now;
                        HANDLE ask=CreateThread(NULL,0,AskSpin,(void*)(ULONG_PTR)(any&XINPUT_GAMEPAD_START ? 1 : 2),0,NULL);
                        if(ask) CloseHandle(ask);
                    }
                }
            }
        }
        ImGui::GetIO().DeltaTime=1.0f/60;
        party::DrawUi(frame);
        Screen->Clear(0,NULL,D3DCLEAR_TARGET,0xFF0F1725,1.0f,0);
        ImGuiXbox360Render(ImGui::GetDrawData());
        Screen->Present(NULL,NULL,NULL,NULL);
    }
    Screen->Release(); Screen=NULL;
    XLaunchNewImage(XLAUNCH_KEYWORD_DEFAULT_APP,0);
}
