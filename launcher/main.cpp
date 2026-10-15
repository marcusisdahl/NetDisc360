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

static const char IniFile[]="game:\\netiso.ini",LogFile[]="game:\\netiso.log";
static const char IsoLink[]="\\??\\NIsoL:",IsoRoot[]="NIsoL:",IsoDevice[]="\\Device\\NetIso";
static const char DiscLink[]="\\??\\NIsoD:",DiscXex[]="NIsoD:\\default.xex",DiscDevice[]="\\Device\\Cdrom0";
static const char LaunchXex[]="\\Device\\Cdrom0\\default.xex";
static const char MountFolder[]="Mount";
static const int MountWaitMs=30000,PollMs=500,MaxDepth=4,MaxNames=256;

static FILE* Log=NULL;
static void Note(const char* format,...) {
    if(!Log) return;
    va_list args; va_start(args,format); vfprintf(Log,format,args); va_end(args);
    fputc('\n',Log); fflush(Log);
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

// Walks from the root toward the ISO and its Mount folder, the way you
// would in Aurora's file browser, until a listing comes back empty.
static bool RequestMount(const Settings& settings) {
    char folder[MAX_PATH]; strcpy_s(folder,IsoRoot);
    bool sawIso=false;
    for(int depth=0;depth<MaxDepth;++depth) {
        int count=ListFolder(folder);
        if(!count) return sawIso;
        const char* next=FindName(count,settings.iso);
        if(next) sawIso=true;
        else next=FindName(count,MountFolder);
        if(!next) {
            Note(sawIso?"No Mount folder under %s.":"%s isn't listed by the server. Is the server running, and is the ISO in its folder?",
                sawIso?folder:settings.iso);
            return false;
        }
        strcat_s(folder,"\\"); strcat_s(folder,next);
    }
    Note("Gave up after %d folders without an empty Mount listing.",MaxDepth);
    return false;
}

static bool MountGame(const char* ini) {
    Settings settings;
    if(!ReadSettings(ini,settings)) return false;
    if(LinkDrive(DiscLink,DiscDevice)<0) { Note("Can't link %s.",DiscDevice); return false; }
    if(DiscIsGame(settings)) { Note("The game is already mounted."); return true; }
    if(!GetModuleHandle("NetISO.xex")) Note("NetISO.xex isn't loaded. Add it as a DashLaunch plugin.");
    LONG status=LinkDrive(IsoLink,IsoDevice);
    if(status<0) { Note("Can't link %s (status %08X). Is the NetISO plugin running?",IsoDevice,status); return false; }
    bool asked=RequestMount(settings);
    UnlinkDrive(IsoLink);
    if(!asked) return false;
    for(int waited=0;waited<MountWaitMs;waited+=PollMs) {
        if(DiscIsGame(settings)) { Note("Mounted after %d ms.",waited); return true; }
        Sleep(PollMs);
    }
    DWORD titleId=0,mediaId=0;
    if(DiscIds(titleId,mediaId)) Note("The disc is still %08X/%08X after %d ms.",titleId,mediaId,MountWaitMs);
    else Note("No readable disc after %d ms.",MountWaitMs);
    return false;
}

static void LaunchGame(const char* ini) {
    bool mounted=MountGame(ini);
    UnlinkDrive(DiscLink);
    if(!mounted) return;
    Note("Launching %s",LaunchXex);
    if(Log) { fclose(Log); Log=NULL; }
    XLaunchNewImage(LaunchXex,0); // Returns VOID in this SDK; returning at all means it failed.
    if(fopen_s(&Log,LogFile,"a")) Log=NULL;
    Note("XLaunchNewImage failed (%lu).",GetLastError());
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
        static const char* files[]={"default.xex","netiso.ini","netiso.log"};
        for(int i=0;i<3;++i) {
            char path[nia::PathMax]; _snprintf_s(path,sizeof(path),_TRUNCATE,"%s\\%s",folder,files[i]);
            DeleteFile(path);
        }
        return RemoveDirectory(folder)!=FALSE;
    }
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
        Sleep(250);
    }
    Note("No network address after 10 s.");
    return false;
}

static unsigned char Launcher[4<<20];
static void Sync(const char* ini) {
    DWORD address=0; int port=nia::IsvrPort;
    char value[nia::PathMax],games[nia::PathMax];
    if(!nia::IniValue(ini,"games",value,sizeof(value))) strcpy_s(value,"Hdd1:\\NetISO");
    if(!MapPath(value,games,sizeof(games))) return;
    Note("Games folder %s",value);
    // This XEX is the template; its execution info is overwritten per game.
    XboxStorage storage; DWORD size=0;
    if(!storage.ReadFile("game:\\default.xex",Launcher,sizeof(Launcher),size)) { Note("Can't read game:\\default.xex."); return; }
    if(!FindServer(ini,address,port) || !StartNetwork()) return;
    nia::IsvrClient client(10);
    bool connected=false;
    for(int attempt=0;attempt<3 && !connected;++attempt) {
        connected=client.Connect(address,port,3) && client.Ping();
        if(!connected) { Note("Server: %s",client.Error()); Sleep(1000); }
    }
    if(!connected) return;
    nia::SyncResult result;
    bool ok=nia::Sync(client,storage,games,Launcher,size,NoteText,result);
    Note("%s: %d added, %d kept, %d removed, %d skipped.",ok?"Done":"Stopped",result.added,result.kept,result.removed,result.skipped);
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
    XLaunchNewImage(path,0);
    if(fopen_s(&Log,LogFile,"a")) Log=NULL;
    Note("Couldn't start %s (%lu).",path,GetLastError());
}

void __cdecl main() {
    if(fopen_s(&Log,LogFile,"w")) Log=NULL;
    static char ini[nia::IniMax];
    if(!ReadText(IniFile,ini,sizeof(ini))) Note("Can't open %s.",IniFile);
    static char iso[MAX_PATH];
    if(nia::IniValue(ini,"iso",iso,sizeof(iso)) && iso[0]) LaunchGame(ini);
    else {
        Note("NetISO Sync");
        Sync(ini);
        LaunchAfterSync(ini);
    }
    UnlinkDrives();
    if(Log) fclose(Log);
    XLaunchNewImage(XLAUNCH_KEYWORD_DASH,0);
}
