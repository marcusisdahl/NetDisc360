// NetISO launcher: one copy per game, made by tools/netiso_aurora.py.
//
// The copy carries the game's Title ID and Media ID in its XEX header, so
// Aurora scans it like an installed game. Started, it asks the NetISO plugin
// to mount the game's ISO from the server, waits until the emulated disc is
// that game, and launches the disc's default.xex.
//
// How the plugin mounts (read from NetISO.xex 1.0, the plugin has no source):
// it registers the file system device \Device\NetIso. Listing its folders
// shows the server's ISOs, each with a "Mount" folder inside. Listing that
// "Mount" folder sends MountIso to the server, and on success the plugin
// cycles a fake disc tray so the console sees the new disc at \Device\Cdrom0.
// Aurora's file browser does the same when you open those folders. This
// launcher walks the same folders by name instead of assuming their depth.
#include <xtl.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

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

// netiso.ini, written by the generator next to this XEX.
struct Settings { char iso[MAX_PATH]; DWORD titleId,mediaId; };
static bool ReadSettings(Settings& settings) {
    memset(&settings,0,sizeof(settings));
    FILE* file=NULL;
    if(fopen_s(&file,IniFile,"r")||!file) { Note("Can't open %s.",IniFile); return false; }
    char line[512];
    while(fgets(line,sizeof(line),file)) {
        line[strcspn(line,"\r\n")]=0;
        char* value=strchr(line,'=');
        if(!value||line[0]==';'||line[0]=='#') continue;
        *value++=0;
        if(!_stricmp(line,"iso")) strcpy_s(settings.iso,value);
        else if(!_stricmp(line,"title_id")) settings.titleId=strtoul(value,NULL,16);
        else if(!_stricmp(line,"media_id")) settings.mediaId=strtoul(value,NULL,16);
    }
    fclose(file);
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

static bool Run() {
    Settings settings;
    if(!ReadSettings(settings)) return false;
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

void __cdecl main() {
    if(fopen_s(&Log,LogFile,"w")) Log=NULL;
    bool mounted=Run();
    UnlinkDrive(DiscLink);
    if(mounted) {
        Note("Launching %s",LaunchXex);
        if(Log) { fclose(Log); Log=NULL; }
        XLaunchNewImage(LaunchXex,0); // Returns VOID in this SDK; returning at all means it failed.
        if(fopen_s(&Log,LogFile,"a")) Log=NULL;
        Note("XLaunchNewImage failed (%lu).",GetLastError());
    }
    if(Log) fclose(Log);
    XLaunchNewImage(XLAUNCH_KEYWORD_DASH,0);
}
