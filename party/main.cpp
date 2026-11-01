// SPDX-License-Identifier: MIT
// NetDiscParty.xex: the LAN party on each console. It shows this console's
// name (as NetDisc360 knows it), the guests who are here, and the raffle
// wheel, turning as it turns on the dashboard, the big screen and every other
// console: NetDisc360 plans each spin, and says how far into it it is, so
// all of them stop on the same guest at the same moment. On the Gamemaster's
// console, A spins for teams and X picks one guest. B goes back to Aurora.
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
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../shared/isvr.h"
#include "../shared/library.h"
#include "../shared/netdisc.h"
#include "../third_party/imgui/imgui.h"
#include "../launcher/imgui_impl_xbox360.h"
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
enum { PartyWait = 0x5057, PartySpinAsk = 0x5053, PartySignIn = 0x5049, SignedOut = 0x8000, WaitMs = 25000, TextMost = 48 << 10 };
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
char Notice[160]="";          // What the server said to the Gamemaster's last spin.
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
        if(!client.Text(PartyWait,0,version,WaitMs,text,sizeof(text),size)) { SetLink(party::LinkLost); client.Close(); Sleep(2000); continue; }
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
    if(!strncmp(said,"error",5) && tab) { strcpy_s(Notice,tab+1); NoticeAt=GetTickCount(); }
    else Notice[0]=0;
    LeaveCriticalSection(&Lock);
    return 0;
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
WORD Buttons(DWORD pad) {
    XINPUT_STATE state; ZeroMemory(&state,sizeof(state));
    return XInputGetState(pad,&state)==ERROR_SUCCESS ? state.Gamepad.wButtons : 0;
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
    // A title that reads no notifications can leave the Guide dead (seen
    // with the launcher), so this reads them, as a game does.
    Notifications=XNotifyCreateListener(XNOTIFY_ALL);
    if(!ScreenStart()) { XLaunchNewImage(XLAUNCH_KEYWORD_DEFAULT_APP,0); return; }
    HANDLE listener=CreateThread(NULL,0,Listen,NULL,0,NULL);
    if(listener) CloseHandle(listener);
    party::Screen& frame=Frame;
    unsigned shownSpin=0;
    DWORD spinStart=0, started=GetTickCount(), askedAt=0;
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
        frame.link=LinkNow;
        strcpy_s(frame.server,ServerText);
        if(Notice[0] && now-NoticeAt>8000) Notice[0]=0;
        strcpy_s(frame.notice,Notice);
        if(!frame.notice[0] && FtpFailed) strcpy_s(frame.notice,"Guest profiles can't be copied here: the FTP port is taken.");
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
        frame.copying=ftp.sessions>0 || ftp.transfers>0 || (ftp.lastAt && now-ftp.lastAt<3000) ||
                      (frame.link==party::LinkConnected && mine && mine->count && !strcmp(frame.state.profiles,"copying"));
        WatchSignIns(mine,now);
        if((any&(XINPUT_GAMEPAD_B|XINPUT_GAMEPAD_BACK)) && !frame.copying) break;
        if(frame.view==party::ViewPlayers && mine && mine->count) {
            if(any&XINPUT_GAMEPAD_Y) { frame.view=party::ViewWheel; Stay=true; }
            for(int pad=0;pad<4;++pad) if(pressed[pad]&XINPUT_GAMEPAD_A) {
                if(frame.asking>=0) { if(!frame.copying) SignInPad(pad,mine); }
                else if(frame.leaveIn>=0) frame.leaveIn=0;
            }
            if(frame.leaveIn==0 && !frame.copying) break;
        } else {
            if((any&XINPUT_GAMEPAD_Y) && mine && mine->count && !(spun && after<0)) frame.view=party::ViewPlayers;
            // The Gamemaster spins: A for teams, X to pick one guest; once a second at most.
            if((any&(XINPUT_GAMEPAD_A|XINPUT_GAMEPAD_X)) && frame.state.valid && frame.state.gamemaster && now-askedAt>1000) {
                askedAt=now;
                HANDLE ask=CreateThread(NULL,0,AskSpin,(void*)(ULONG_PTR)(any&XINPUT_GAMEPAD_A ? 1 : 2),0,NULL);
                if(ask) CloseHandle(ask);
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
