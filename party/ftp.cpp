// SPDX-License-Identifier: MIT
// The party app's tiny FTP server (ftp.h). It builds for the Xbox and, for
// the tests, on the host (tests/party_ftp.cpp), where the real NetDisc360
// server copies profiles to it as it does to a console.
#include "ftp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

#ifdef _XBOX
#include <xtl.h>
#include <winsockx.h>
typedef SOCKET Socket;
typedef CRITICAL_SECTION Mutex;
static void MutexInit(Mutex& m) { InitializeCriticalSection(&m); }
static void MutexLock(Mutex& m) { EnterCriticalSection(&m); }
static void MutexUnlock(Mutex& m) { LeaveCriticalSection(&m); }
static void CloseSocket(Socket s) { closesocket(s); }
static unsigned long Ticks() { return GetTickCount(); }
static bool Spawn(DWORD (WINAPI* run)(void*), void* with) {
    HANDLE thread=CreateThread(NULL,0,run,with,0,NULL);
    if(!thread) return false;
    CloseHandle(thread);
    return true;
}
#define THREAD_RESULT DWORD WINAPI
#define THREAD_DONE 0
static const char Separator='\\';
// The XDK's C library has these under other names.
#define snprintf(text,capacity,...) _snprintf_s(text,capacity,_TRUNCATE,__VA_ARGS__)
#define vsnprintf(text,capacity,format,a) _vsnprintf_s(text,capacity,_TRUNCATE,format,a)
#else
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
typedef int Socket;
typedef pthread_mutex_t Mutex;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
static void MutexInit(Mutex& m) { pthread_mutex_init(&m,NULL); }
static void MutexLock(Mutex& m) { pthread_mutex_lock(&m); }
static void MutexUnlock(Mutex& m) { pthread_mutex_unlock(&m); }
static void CloseSocket(Socket s) { close(s); }
static unsigned long Ticks() { timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (unsigned long)(t.tv_sec*1000+t.tv_nsec/1000000); }
static bool Spawn(void* (*run)(void*), void* with) {
    pthread_t thread;
    if(pthread_create(&thread,NULL,run,with)) return false;
    pthread_detach(thread);
    return true;
}
#define THREAD_RESULT void*
#define THREAD_DONE NULL
static const char Separator='/';
#define _stricmp strcasecmp
#endif

namespace party {
namespace {

enum { PathMost = 512, NameMost = 42, DepthMost = 10, LineMost = 1024, ChunkBytes = 64 << 10, IdleSeconds = 120 };

Mutex Lock;
void Copy(char* to, size_t capacity, const char* from) { size_t n=strlen(from); if(n>=capacity) n=capacity-1; memcpy(to,from,n); to[n]=0; }
char Root[128]="";
int Port=FtpPort;
unsigned long AllowFrom=0;
FtpActivity Now;

void Change(int sessions, int transfers, const char* stored) {
    MutexLock(Lock);
    Now.sessions+=sessions; Now.transfers+=transfers; Now.lastAt=Ticks();
    if(stored) { ++Now.stored; Copy(Now.last,sizeof(Now.last),stored); }
    MutexUnlock(Lock);
}

// ---- Files, as the console (or the host) has them ----

struct Entry { char name[NameMost+1]; bool folder; unsigned long long size; };

#ifdef _XBOX
bool Info(const char* path, bool& folder, unsigned long long& size) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if(!GetFileAttributesEx(path,GetFileExInfoStandard,&a)) return false;
    folder=(a.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
    size=(unsigned long long)a.nFileSizeHigh<<32|a.nFileSizeLow;
    return true;
}
// Calls each(entry, with) for what's in folder; false if it can't be read.
template<class F> bool EachIn(const char* folder, F& each) {
    char pattern[PathMost+4];
    _snprintf_s(pattern,sizeof(pattern),_TRUNCATE,"%s\\*",folder);
    WIN32_FIND_DATA found;
    HANDLE find=FindFirstFile(pattern,&found);
    if(find==INVALID_HANDLE_VALUE) return GetLastError()==ERROR_FILE_NOT_FOUND || GetLastError()==ERROR_NO_MORE_FILES;
    do {
        Entry e;
        if(strlen(found.cFileName)>NameMost || !strcmp(found.cFileName,".") || !strcmp(found.cFileName,"..")) continue;
        strcpy_s(e.name,found.cFileName);
        e.folder=(found.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
        e.size=(unsigned long long)found.nFileSizeHigh<<32|found.nFileSizeLow;
        each(e);
    } while(FindNextFile(find,&found));
    FindClose(find);
    return true;
}
typedef HANDLE File;
const File NoFile=INVALID_HANDLE_VALUE;
File OpenRead(const char* path) { return CreateFile(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL); }
File OpenWrite(const char* path) { return CreateFile(path,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL); }
int ReadSome(File f, char* into, int most) { DWORD done=0; return ReadFile(f,into,DWORD(most),&done,NULL) ? int(done) : -1; }
bool WriteAll(File f, const char* from, int size) { DWORD done=0; return WriteFile(f,from,DWORD(size),&done,NULL) && done==DWORD(size); }
bool CloseFile(File f) { return CloseHandle(f)!=FALSE; }
bool MakeFolder(const char* path) { return CreateDirectory(path,NULL)!=FALSE; }
bool RemoveFolder(const char* path) { return RemoveDirectory(path)!=FALSE; }
bool RemoveFile(const char* path) { return DeleteFile(path)!=FALSE; }
bool Move(const char* from, const char* to) { return MoveFile(from,to)!=FALSE; }
#else
bool Info(const char* path, bool& folder, unsigned long long& size) {
    struct stat s;
    if(stat(path,&s)) return false;
    folder=S_ISDIR(s.st_mode); size=(unsigned long long)s.st_size;
    return true;
}
template<class F> bool EachIn(const char* folder, F& each) {
    DIR* d=opendir(folder);
    if(!d) return false;
    while(dirent* found=readdir(d)) {
        if(strlen(found->d_name)>NameMost || !strcmp(found->d_name,".") || !strcmp(found->d_name,"..")) continue;
        Entry e;
        Copy(e.name,sizeof(e.name),found->d_name);
        char path[PathMost+300];
        snprintf(path,sizeof(path),"%s/%s",folder,found->d_name);
        if(!Info(path,e.folder,e.size)) continue;
        each(e);
    }
    closedir(d);
    return true;
}
typedef int File;
const File NoFile=-1;
File OpenRead(const char* path) { return open(path,O_RDONLY); }
File OpenWrite(const char* path) { return open(path,O_WRONLY|O_CREAT|O_TRUNC,0644); }
int ReadSome(File f, char* into, int most) { return int(read(f,into,size_t(most))); }
bool WriteAll(File f, const char* from, int size) { return write(f,from,size_t(size))==size; }
bool CloseFile(File f) { return close(f)==0; }
bool MakeFolder(const char* path) { return mkdir(path,0755)==0; }
bool RemoveFolder(const char* path) { return rmdir(path)==0; }
bool RemoveFile(const char* path) { return unlink(path)==0; }
bool Move(const char* from, const char* to) { struct stat s; return stat(to,&s) && rename(from,to)==0; }
#endif

// ---- Paths ----
// An FTP path as its names: /Hdd1/Content/E0000123456789AB/... The first
// is always Hdd1, the second Content (any case).

struct Path { int count; char names[DepthMost][NameMost+1]; };

bool OfflineXuid(const char* name) {
    if(strlen(name)!=16 || toupper((unsigned char)name[0])!='E') return false;
    for(int i=0;i<16;++i) if(!isxdigit((unsigned char)name[i])) return false;
    return true;
}
bool GoodName(const char* name, size_t length) {
    if(!length || length>NameMost) return false;
    for(size_t i=0;i<length;++i) {
        unsigned char c=(unsigned char)name[i];
        if(c<32 || c>126 || strchr("\\:*?\"<>|",c)) return false;
    }
    return !(length==1 && name[0]=='.') && !(length==2 && name[0]=='.' && name[1]=='.');
}
// text against the current folder: false if it goes outside what's shown.
bool Resolve(const Path& here, const char* text, Path& out) {
    out=*text=='/' ? Path() : here;
    if(*text=='/') out.count=0;
    for(const char* at=text;*at;) {
        while(*at=='/') ++at;
        if(!*at) break;
        const char* end=strchr(at,'/'); if(!end) end=at+strlen(at);
        size_t length=size_t(end-at);
        if(length==1 && at[0]=='.') {}
        else if(length==2 && at[0]=='.' && at[1]=='.') { if(out.count) --out.count; }
        else {
            if(!GoodName(at,length) || out.count>=DepthMost) return false;
            memcpy(out.names[out.count],at,length); out.names[out.count][length]=0;
            ++out.count;
        }
        at=end;
    }
    if(out.count>=1 && _stricmp(out.names[0],"Hdd1")) return false;
    if(out.count>=2 && _stricmp(out.names[1],"Content")) return false;
    if(out.count>=3 && !OfflineXuid(out.names[2])) return false;
    return true;
}
// Inside an offline profile's folder (or that folder itself): may be written.
bool Writable(const Path& p) { return p.count>=3; }
void Text(const Path& p, char* text, size_t capacity) {
    text[0]=0;
    if(!p.count) { Copy(text,capacity,"/"); return; }
    size_t at=0;
    for(int i=0;i<p.count && at+1<capacity;++i) at+=size_t(snprintf(text+at,capacity-at,"/%s",i==0 ? "Hdd1" : i==1 ? "Content" : p.names[i]));
}
// Where it is on the drive (count>=2).
void Local(const Path& p, char* local, size_t capacity) {
    size_t at=size_t(snprintf(local,capacity,"%s%cContent",Root,Separator));
    for(int i=2;i<p.count && at+1<capacity;++i) at+=size_t(snprintf(local+at,capacity-at,"%c%s",Separator,p.names[i]));
}
// Whether p is a folder shown here: /, /Hdd1, /Hdd1/Content, or a real one under it.
bool IsFolder(const Path& p) {
    if(p.count<=2) return true;
    char local[PathMost]; Local(p,local,sizeof(local));
    bool folder=false; unsigned long long size=0;
    return Info(local,folder,size) && folder;
}

// ---- Sockets ----

bool SendAll(Socket s, const char* data, size_t size) {
    while(size) {
        int sent=send(s,data,int(size>ChunkBytes ? ChunkBytes : size),0);
        if(sent<=0) return false;
        data+=sent; size-=size_t(sent);
    }
    return true;
}
void Insecure(Socket s) {
#ifdef _XBOX
    // As for any connection from a PC on a hacked console (shared/isvr.cpp).
    BOOL on=TRUE;
    setsockopt(s,SOL_SOCKET,0x5801,(const char*)&on,sizeof(on));
    setsockopt(s,SOL_SOCKET,0x5802,(const char*)&on,sizeof(on));
#else
    (void)s;
#endif
}
void Timeout(Socket s, int seconds) {
#ifdef _XBOX
    int ms=seconds*1000;
    setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,(const char*)&ms,sizeof(ms));
    setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,(const char*)&ms,sizeof(ms));
#else
    timeval t; t.tv_sec=seconds; t.tv_usec=0;
    setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&t,sizeof(t));
    setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,&t,sizeof(t));
#endif
}
Socket Listener(unsigned long address, int port, int backlog) {
    Socket s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if(s==INVALID_SOCKET) return s;
    Insecure(s);
    int on=1;
    setsockopt(s,SOL_SOCKET,SO_REUSEADDR,(const char*)&on,sizeof(on));
    sockaddr_in at; memset(&at,0,sizeof(at));
    at.sin_family=AF_INET; at.sin_addr.s_addr=address; at.sin_port=htons((unsigned short)port);
    if(bind(s,(sockaddr*)&at,sizeof(at)) || listen(s,backlog)) { CloseSocket(s); return INVALID_SOCKET; }
    return s;
}
// Waits up to seconds for a connection on listener from address.
Socket AcceptFrom(Socket listener, unsigned long address, int seconds) {
    fd_set ready; FD_ZERO(&ready); FD_SET(listener,&ready);
    timeval wait; wait.tv_sec=seconds; wait.tv_usec=0;
    if(select(int(listener)+1,&ready,NULL,NULL,&wait)<=0) return INVALID_SOCKET;
    sockaddr_in from; memset(&from,0,sizeof(from));
#ifdef _XBOX
    int length=sizeof(from);
#else
    socklen_t length=sizeof(from);
#endif
    Socket s=accept(listener,(sockaddr*)&from,&length);
    if(s!=INVALID_SOCKET && from.sin_addr.s_addr!=address) { CloseSocket(s); return INVALID_SOCKET; }
    return s;
}

// ---- A session ----

struct Session {
    Socket control, passive;
    unsigned long peer, self;  // Network order.
    Path here, renaming;
    bool user, in, haveRename;
    char buffer[LineMost*2]; size_t buffered;

    bool Reply(const char* format, ...) {
        char line[LineMost+8];
        va_list a; va_start(a,format); int n=vsnprintf(line,LineMost,format,a); va_end(a);
        if(n<0 || n>=LineMost) n=LineMost-1;
        line[n++]='\r'; line[n++]='\n';
        return SendAll(control,line,size_t(n));
    }
    // One command line; false when the connection goes (or a line is too long).
    bool ReadLine(char* line) {
        for(;;) {
            char* end=(char*)memchr(buffer,'\n',buffered);
            if(end) {
                size_t length=size_t(end-buffer);
                memcpy(line,buffer,length); line[length]=0;
                if(length && line[length-1]=='\r') line[length-1]=0;
                memmove(buffer,end+1,buffered-length-1); buffered-=length+1;
                return true;
            }
            if(buffered>=LineMost) return false;
            int got=recv(control,buffer+buffered,int(sizeof(buffer)-buffered),0);
            if(got<=0) return false;
            buffered+=size_t(got);
        }
    }
    void ClosePassive() { if(passive!=INVALID_SOCKET) { CloseSocket(passive); passive=INVALID_SOCKET; } }
    // The data connection PASV opened (from the same computer), or none.
    Socket Data() {
        if(passive==INVALID_SOCKET) { Reply("425 Use PASV first."); return INVALID_SOCKET; }
        Socket s=AcceptFrom(passive,peer,15);
        ClosePassive();
        if(s==INVALID_SOCKET) { Reply("425 No data connection came."); return s; }
        Timeout(s,30);
        return s;
    }

    void Pasv() {
        ClosePassive();
        passive=Listener(self,0,1);
        sockaddr_in at; memset(&at,0,sizeof(at));
#ifdef _XBOX
        int length=sizeof(at);
#else
        socklen_t length=sizeof(at);
#endif
        if(passive==INVALID_SOCKET || getsockname(passive,(sockaddr*)&at,&length)) { ClosePassive(); Reply("425 Can't open a data port."); return; }
        unsigned char* a=(unsigned char*)&self;
        int port=ntohs(at.sin_port);
        Reply("227 Entering Passive Mode (%u,%u,%u,%u,%d,%d).",a[0],a[1],a[2],a[3],port>>8,port&255);
    }
    struct Lister {
        char* text; size_t size, capacity;
        bool onlyOffline;
        void operator()(const Entry& e) {
            if(onlyOffline && (!e.folder || !OfflineXuid(e.name))) return;
            if(size+NameMost+80>capacity) return;
            size+=size_t(snprintf(text+size,capacity-size,"%s 1 root root %llu Jan 01 00:00 %s\r\n",
                                  e.folder ? "drwxrwxrwx" : "-rw-rw-rw-",e.size,e.name));
        }
    };
    void List(const char* argument, bool names) {
        // Options such as -la are left out; the rest is the folder.
        while(*argument=='-') { while(*argument && *argument!=' ') ++argument; while(*argument==' ') ++argument; }
        Path p;
        if(!Resolve(here,argument,p) || !IsFolder(p)) { ClosePassive(); Reply("550 No such folder."); return; }
        static const size_t Capacity=64<<10;
        char* text=(char*)malloc(Capacity);
        if(!text) { ClosePassive(); Reply("451 Out of memory."); return; }
        Lister list={text,0,Capacity,p.count==2};
        if(p.count==0) { Entry e={"Hdd1",true,0}; list(e); }
        else if(p.count==1) { Entry e={"Content",true,0}; list(e); }
        else { char local[PathMost]; Local(p,local,sizeof(local)); EachIn(local,list); }
        if(names) {  // NLST: the names alone.
            size_t out=0;
            for(size_t at=0;at<list.size;) {
                char* end=strstr(text+at,"\r\n");
                char* name=text+at; for(int field=0;field<8;++field) { name=strchr(name,' '); while(name && *name==' ') ++name; }
                size_t length=name ? size_t(end-name) : 0;
                memmove(text+out,name,length); out+=length; memcpy(text+out,"\r\n",2); out+=2;
                at=size_t(end-text)+2;
            }
            list.size=out;
        }
        Socket data=Data();
        if(data==INVALID_SOCKET) { free(text); return; }
        Reply("150 Here it comes.");
        bool sent=SendAll(data,text,list.size);
        CloseSocket(data); free(text);
        Reply(sent ? "226 Done." : "426 The data connection broke.");
    }
    void Retrieve(const char* argument) {
        Path p; char local[PathMost];
        if(!Resolve(here,argument,p) || p.count<4) { ClosePassive(); Reply("550 Not a file here."); return; }
        Local(p,local,sizeof(local));
        bool folder=false; unsigned long long size=0;
        File f=Info(local,folder,size) && !folder ? OpenRead(local) : NoFile;
        if(f==NoFile) { ClosePassive(); Reply("550 Can't open it."); return; }
        Socket data=Data();
        if(data==INVALID_SOCKET) { CloseFile(f); return; }
        Change(0,1,NULL);
        Reply("150 Sending %llu bytes.",size);
        char* chunk=(char*)malloc(ChunkBytes);
        bool ok=chunk!=NULL;
        for(int got;ok && (got=ReadSome(f,chunk,ChunkBytes))!=0;) ok=got>0 && SendAll(data,chunk,size_t(got));
        free(chunk); CloseFile(f); CloseSocket(data);
        Change(0,-1,NULL);
        Reply(ok ? "226 Done." : "426 Sending it failed.");
    }
    void Store(const char* argument) {
        Path p; char local[PathMost];
        if(!Resolve(here,argument,p) || !Writable(p) || p.count<4) { ClosePassive(); Reply("553 Files go in a guest profile's folder only."); return; }
        Local(p,local,sizeof(local));
        bool folder=false; unsigned long long size=0;
        if(Info(local,folder,size) && folder) { ClosePassive(); Reply("553 That's a folder."); return; }
        File f=OpenWrite(local);
        if(f==NoFile) { ClosePassive(); Reply("553 Can't write it (in use?)."); return; }
        Socket data=Data();
        if(data==INVALID_SOCKET) { CloseFile(f); RemoveFile(local); return; }
        Change(0,1,NULL);
        Reply("150 Send it.");
        char* chunk=(char*)malloc(ChunkBytes);
        bool ok=chunk!=NULL, big=false;
        unsigned long long total=0;
        for(int got;ok && (got=recv(data,chunk,ChunkBytes,0))!=0;) {
            if(got<0) { ok=false; break; }
            total+=(unsigned long long)got;
            if(total>FtpFileMost) { ok=false; big=true; break; }
            ok=WriteAll(f,chunk,got);
        }
        free(chunk);
        CloseSocket(data);
        ok=CloseFile(f) && ok;
        if(!ok) RemoveFile(local);
        Change(0,-1,ok ? p.names[p.count-1] : NULL);
        if(ok) Reply("226 Stored %llu bytes.",total);
        else Reply(big ? "552 Larger than a profile's file may be." : "451 Writing it failed.");
    }

    void Run() {
        Timeout(control,IdleSeconds);
        Reply("220 NetDiscParty: guest profiles only.");
        char line[LineMost];
        while(ReadLine(line)) {
            char* argument=line;
            while(*argument && *argument!=' ') ++argument;
            if(*argument) *argument++=0;
            for(char* c=line;*c;++c) *c=char(toupper((unsigned char)*c));
            const char* command=line;
            Change(0,0,NULL);
            if(!strcmp(command,"QUIT")) { Reply("221 Bye."); break; }
            if(!strcmp(command,"USER")) { user=true; Reply("331 Any password."); continue; }
            if(!strcmp(command,"PASS")) {
                if(!user) { Reply("503 USER first."); continue; }
                if(!in) { in=true; Change(1,0,NULL); }
                Reply("230 In.");
                continue;
            }
            if(!in) { Reply("530 Log in first."); continue; }
            Path p; char text[PathMost];
            if(!strcmp(command,"SYST")) Reply("215 UNIX Type: L8");
            else if(!strcmp(command,"NOOP")) Reply("200 OK.");
            else if(!strcmp(command,"TYPE") || !strcmp(command,"MODE") || !strcmp(command,"STRU")) Reply("200 OK.");
            else if(!strcmp(command,"FEAT")) Reply("211 No features.");
            else if(!strcmp(command,"PWD") || !strcmp(command,"XPWD")) { Text(here,text,sizeof(text)); Reply("257 \"%s\"",text); }
            else if(!strcmp(command,"CWD") || !strcmp(command,"CDUP")) {
                if(Resolve(here,!strcmp(command,"CDUP") ? ".." : argument,p) && IsFolder(p)) { here=p; Reply("250 OK."); }
                else Reply("550 No such folder.");
            }
            else if(!strcmp(command,"PASV")) Pasv();
            else if(!strcmp(command,"LIST") || !strcmp(command,"NLST")) List(argument,command[0]=='N');
            else if(!strcmp(command,"RETR")) Retrieve(argument);
            else if(!strcmp(command,"STOR")) Store(argument);
            else if(!strcmp(command,"SIZE")) {
                char local[PathMost]; bool folder=true; unsigned long long size=0;
                if(Resolve(here,argument,p) && p.count>=4) { Local(p,local,sizeof(local)); Info(local,folder,size); }
                if(folder) Reply("550 Not a file."); else Reply("213 %llu",size);
            }
            else if(!strcmp(command,"MKD") || !strcmp(command,"XMKD")) {
                char local[PathMost];
                if(!Resolve(here,argument,p) || !Writable(p)) { Reply("550 Folders go in a guest profile's folder only."); continue; }
                Local(p,local,sizeof(local)); Text(p,text,sizeof(text));
                if(MakeFolder(local)) Reply("257 \"%s\" made.",text); else Reply("550 Can't make it.");
            }
            else if(!strcmp(command,"DELE") || !strcmp(command,"RMD") || !strcmp(command,"XRMD")) {
                char local[PathMost];
                bool folder=command[0]!='D';
                if(!Resolve(here,argument,p) || !Writable(p) || (!folder && p.count<4)) { Reply("550 Not here."); continue; }
                Local(p,local,sizeof(local));
                if(folder ? RemoveFolder(local) : RemoveFile(local)) Reply("250 Deleted."); else Reply("550 Can't delete it.");
            }
            else if(!strcmp(command,"RNFR")) {
                haveRename=Resolve(here,argument,renaming) && Writable(renaming) && renaming.count>=4;
                if(haveRename) Reply("350 And the new name?"); else Reply("550 Not here.");
            }
            else if(!strcmp(command,"RNTO")) {
                char from[PathMost], to[PathMost];
                bool ok=haveRename && Resolve(here,argument,p) && Writable(p) && p.count>=4;
                haveRename=false;
                if(ok) { Local(renaming,from,sizeof(from)); Local(p,to,sizeof(to)); ok=Move(from,to); }
                if(ok) Reply("250 Renamed."); else Reply("553 Can't rename it.");
            }
            else Reply("502 Not here.");
        }
        ClosePassive();
        if(in) Change(-1,0,NULL);
        CloseSocket(control);
    }
};

int Sessions=0;  // Under Lock.

THREAD_RESULT RunSession(void* with) {
    Session* s=static_cast<Session*>(with);
    s->Run();
    delete s;
    MutexLock(Lock); --Sessions; MutexUnlock(Lock);
    return THREAD_DONE;
}

THREAD_RESULT Listen(void* with) {
    Socket listener=Socket((long)(size_t)with);
    for(;;) {
        sockaddr_in from; memset(&from,0,sizeof(from));
#ifdef _XBOX
        int length=sizeof(from);
#else
        socklen_t length=sizeof(from);
#endif
        Socket s=accept(listener,(sockaddr*)&from,&length);
        if(s==INVALID_SOCKET) continue;
        MutexLock(Lock);
        unsigned long allowed=AllowFrom;
        bool room=Sessions<FtpSessionsMost;
        if(room) ++Sessions;
        MutexUnlock(Lock);
        if(!room || (allowed && from.sin_addr.s_addr!=allowed)) {
            const char* no=room ? "421 Only NetDisc360's computer may connect.\r\n" : "421 Too many connections.\r\n";
            send(s,no,int(strlen(no)),0);
            CloseSocket(s);
            if(room) { MutexLock(Lock); --Sessions; MutexUnlock(Lock); }
            continue;
        }
        Insecure(s);
        int on=1;
        setsockopt(s,IPPROTO_TCP,TCP_NODELAY,(const char*)&on,sizeof(on));
        Session* session=new Session;
        memset(session,0,sizeof(*session));
        session->control=s; session->passive=INVALID_SOCKET; session->peer=from.sin_addr.s_addr;
        sockaddr_in self; memset(&self,0,sizeof(self));
        length=sizeof(self);
        getsockname(s,(sockaddr*)&self,&length);
        session->self=self.sin_addr.s_addr;
        if(!Spawn(RunSession,session)) { CloseSocket(s); delete session; MutexLock(Lock); --Sessions; MutexUnlock(Lock); }
    }
    return THREAD_DONE;
}
}

bool FtpStart(const char* root, int port, unsigned long allowFrom) { return FtpStartAt(root,INADDR_ANY,port,allowFrom); }
bool FtpStartAt(const char* root, unsigned long address, int port, unsigned long allowFrom) {
    MutexInit(Lock);
    memset(&Now,0,sizeof(Now));
    Copy(Root,sizeof(Root),root);
    Port=port; AllowFrom=allowFrom;
    Socket listener=Listener(address,port,4);
    if(listener==INVALID_SOCKET) return false;
    if(!Spawn(Listen,(void*)(size_t)listener)) { CloseSocket(listener); return false; }
    return true;
}
void FtpAllowFrom(unsigned long address) { MutexLock(Lock); AllowFrom=address; MutexUnlock(Lock); }
void FtpNow(FtpActivity& now) { MutexLock(Lock); now=Now; MutexUnlock(Lock); }
}
