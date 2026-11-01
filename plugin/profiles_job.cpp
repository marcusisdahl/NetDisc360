// SPDX-License-Identifier: MIT
// Guest profiles made under Aurora (profiles_job.h): the helper's ProfileJob
// (launcher/main.cpp) without its screen, on a system thread of this plugin.
#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../launcher/profiles.h"
#include "../shared/helper.h"
#include "profiles_job.h"

struct JobKernelString { USHORT length,capacity; char* text; };
extern "C" LONG __stdcall ObCreateSymbolicLink(JobKernelString* link,JobKernelString* device);
extern "C" LONG __stdcall ObDeleteSymbolicLink(JobKernelString* link);
extern "C" LONG __stdcall ExCreateThread(HANDLE* handle,DWORD stackSize,DWORD* threadId,void* apiStartup,LPTHREAD_START_ROUTINE start,void* parameter,DWORD flags);

namespace profilejob {
namespace {
// The hard drive, linked for this thread's files alone.
const char Link[]="\\??\\NdJob:", Drive[]="\\Device\\Harddisk0\\Partition1";
const char JobFile[]="NdJob:\\NetDisc360.job", DoneFile[]="NdJob:\\NetDisc360.done", DonePart[]="NdJob:\\NetDisc360.done.part";
// Looked for this often; not at all for the first while after the console
// starts (Aurora and the plugins settle first).
const DWORD EveryMs=2000, FirstMs=10000;
const DWORD SystemThread=2;  // ExCreateThread: run in the system process.

NoteCall Note=NULL;
ShowCall Show=NULL;
HANDLE Thread=NULL;
volatile LONG Stopping=0;

void Log(const char* format, ...) {
    if(!Note) return;
    static char line[512];
    va_list a; va_start(a,format); _vsnprintf_s(line,sizeof(line),_TRUNCATE,format,a); va_end(a);
    Note("Guest profiles: %s",line);
}
bool Linked() {
    static char name[16], device[48];
    strcpy_s(name,Link); strcpy_s(device,Drive);
    JobKernelString link={USHORT(strlen(name)),USHORT(sizeof(name)),name};
    JobKernelString target={USHORT(strlen(device)),USHORT(sizeof(device)),device};
    ObDeleteSymbolicLink(&link);
    return ObCreateSymbolicLink(&link,&target)>=0;
}
int ReadBytes(const char* path, void* into, DWORD capacity) {
    HANDLE file=CreateFile(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file==INVALID_HANDLE_VALUE) return -1;
    DWORD done=0;
    BOOL ok=ReadFile(file,into,capacity,&done,NULL);
    CloseHandle(file);
    return ok ? int(done) : -1;
}
// key=value on its own line (; starts a comment), as nia::IniValue reads it.
bool Value(const char* text, const char* key, char* value, size_t capacity) {
    size_t length=strlen(key);
    for(const char* line=text;*line;) {
        const char* end=line; while(*end && *end!='\n') ++end;
        while(line<end && (*line==' ' || *line=='\t')) ++line;
        if(size_t(end-line)>length && !_strnicmp(line,key,length) && line[length]=='=') {
            const char* v=line+length+1; const char* stop=end;
            while(stop>v && (stop[-1]=='\r' || stop[-1]==' ' || stop[-1]=='\t')) --stop;
            size_t n=size_t(stop-v); if(n>=capacity) n=capacity-1;
            memcpy(value,v,n); value[n]=0;
            return true;
        }
        line=*end ? end+1 : end;
    }
    value[0]=0;
    return false;
}
bool SafeName(const char* name) { return name[0] && !strchr(name,'\\') && !strchr(name,':') && !strstr(name,".."); }

// Guest n's gamer picture (pictureN= and picture_smallN=, in folder) on xuid; its line in done.
void Picture(const char* job, const char* folder, int n, ULONGLONG xuid, char* done, size_t& used, size_t capacity) {
    char key[24], name[64], path[160];
    sprintf_s(key,"picture%d",n);
    if(!Value(job,key,name,sizeof(name)) || !name[0]) return;
    static BYTE big[64*1024], small[32*1024];
    static char why[400];
    why[0]=0;
    int bigSize=-1, smallSize=0;
    if(!SafeName(name)) sprintf_s(why,"%s isn't a file name.",name);
    else {
        sprintf_s(path,"NdJob:%s\\%s",folder,name);
        bigSize=ReadBytes(path,big,sizeof(big));
        if(bigSize<=0) sprintf_s(why,"couldn't read %s.",path);
        sprintf_s(key,"picture_small%d",n);
        if(bigSize>0 && Value(job,key,name,sizeof(name)) && SafeName(name)) {
            sprintf_s(path,"NdJob:%s\\%s",folder,name);
            smallSize=ReadBytes(path,small,sizeof(small));
            if(smallSize<=0) smallSize=0;
        }
    }
    static const DWORD openFlags[3]={3,4,0};
    bool ok=false;
    if(bigSize>0) {
        nia::ProfilePicture picture={big,DWORD(bigSize),smallSize ? small : NULL,DWORD(smallSize),openFlags,3,false};
        ok=nia::SetProfilePicture(xuid,picture,why,sizeof(why));
    }
    Log("%s %016I64X's picture: %s",ok ? "gave" : "couldn't give",xuid,why);
    if(ok) used+=sprintf_s(done+used,capacity-used,"picture%d=ok\r\n",n);
    else {
        for(char* c=why;*c;++c) if(*c=='\r' || *c=='\n') *c=' ';
        used+=sprintf_s(done+used,capacity-used,"picture_error%d=%.240s\r\n",n,why);
    }
}

void RunJob() {
    static char job[4096];
    int size=ReadBytes(JobFile,job,sizeof(job)-1);
    if(size<0) return;
    job[size]=0;
    // First, so a crash or a freeze in xam doesn't start it again.
    if(!DeleteFile(JobFile)) { Log("couldn't delete the job (%lu); left alone.",GetLastError()); return; }
    char number[32]="", folder[96]="";
    Value(job,"job",number,sizeof(number));
    // The pictures' folder under Hdd1: (\NetISO\NetDisc360).
    Value(job,"folder",folder,sizeof(folder));
    if(strchr(folder,':') || strstr(folder,"..") || (folder[0] && folder[0]!='\\')) folder[0]=0;
    static nia::ProfileInfo profiles[nia::ProfilesListed];
    int listed=nia::ListProfiles(profiles,nia::ProfilesListed);
    nia::ProfileCall call=nia::DefaultProfileCall(profiles,listed);
    char value[32];
    if(Value(job,"device",value,sizeof(value)) && value[0]) call.device=strtoul(value,NULL,0);
    if(Value(job,"flags",value,sizeof(value)) && value[0]) call.flags=strtoul(value,NULL,0);
    ULONGLONG like=0;
    for(int i=0;i<listed && !like;++i) if((profiles[i].xuid>>60)==0xE) like=profiles[i].xuid;
    static char done[8192];
    size_t used=sprintf_s(done,"job=%s\r\nby=plugin\r\n",number);
    int asked=0, made=0;
    for(int n=1;n<=nia::ProfileJobMost && !Stopping;++n) {
        char key[16], name[64];
        sprintf_s(key,"guest%d",n);
        if(!Value(job,key,name,sizeof(name)) || !name[0]) {
            // profileN=<XUID>: one there already, for its picture only.
            sprintf_s(key,"profile%d",n);
            ULONGLONG existing=Value(job,key,name,sizeof(name)) && name[0] ? _strtoui64(name,NULL,16) : 0;
            if(existing) { ++asked; Picture(job,folder,n,existing,done,used,sizeof(done)); ++made; }
            continue;
        }
        ++asked;
        WCHAR gamertag[nia::GamertagMost+1];
        size_t length=strlen(name);
        if(length>nia::GamertagMost) length=nia::GamertagMost;
        for(size_t k=0;k<length;++k) gamertag[k]=WCHAR((unsigned char)name[k]);
        gamertag[length]=0;
        call.xuid=nia::NewOfflineXuid(like,profiles,listed);
        ULONGLONG xuid=0;
        DWORD code=0;
        char why[160]="";
        if(nia::MakeProfile(gamertag,call,xuid,code,why,sizeof(why),NULL)) {
            ++made;
            Log("made %s: %016I64X.",name,xuid);
            used+=sprintf_s(done+used,sizeof(done)-used,"guest%d=%016I64X\r\n",n,xuid);
            Picture(job,folder,n,xuid,done,used,sizeof(done));
        } else {
            Log("couldn't make %s: %s",name,why);
            for(char* c=why;*c;++c) if(*c=='\r' || *c=='\n') *c=' ';
            used+=sprintf_s(done+used,sizeof(done)-used,"error%d=%s\r\n",n,why);
        }
    }
    // Written aside, then named: the server never reads half of it.
    HANDLE file=CreateFile(DonePart,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    DWORD wrote=0;
    bool ok=file!=INVALID_HANDLE_VALUE && WriteFile(file,done,DWORD(used),&wrote,NULL) && wrote==used;
    if(file!=INVALID_HANDLE_VALUE) CloseHandle(file);
    DeleteFile(DoneFile);
    if(!ok || !MoveFile(DonePart,DoneFile)) Log("couldn't write NetDisc360.done (%lu).",GetLastError());
    Log("job %s: made %d of %d.",number[0] ? number : "?",made,asked);
    if(Show && made) {
        char text[64];
        sprintf_s(text,made==1 ? "NetDisc360: 1 guest profile made" : "NetDisc360: %d guest profiles made",made);
        Show(text);
    }
}

DWORD WINAPI Watch(void*) {
    for(DWORD waited=0;waited<FirstMs && !Stopping;waited+=500) Sleep(500);
    bool linked=false;
    while(!Stopping) {
        if(!linked && !(linked=Linked())) Log("couldn't link the hard drive; trying again.");
        if(linked && GetFileAttributes(JobFile)!=DWORD(-1)) RunJob();  // INVALID_FILE_ATTRIBUTES, which the Xbox headers don't have.
        for(DWORD waited=0;waited<EveryMs && !Stopping;waited+=250) Sleep(250);
    }
    return 0;
}
}

void Start(NoteCall note, ShowCall show) {
    Note=note; Show=show;
    DWORD id=0;
    if(ExCreateThread(&Thread,0x10000,&id,NULL,Watch,NULL,SystemThread)<0) { Thread=NULL; Log("the watching thread didn't start."); }
}
void Stop() {
    if(!Thread) return;
    InterlockedExchange(&Stopping,1);
    WaitForSingleObject(Thread,5000);
    CloseHandle(Thread);
    Thread=NULL;
}
}
