// SPDX-License-Identifier: MIT
// Just enough of the Win32/Winsock surface to build shared/bench.cpp and
// shared/readbench.cpp on Linux or macOS for host tests. Force-included with
// -include; the Xbox and Windows builds never see it.
#pragma once
#ifndef _WIN32
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>

#define __int64 long long
typedef unsigned int DWORD;
typedef int LONG;
typedef int BOOL;
typedef unsigned char BYTE;
typedef unsigned short u_short;
typedef unsigned long u_long;
typedef void* HANDLE;
typedef int SOCKET;
typedef long long LONGLONG;
typedef union { LONGLONG QuadPart; } LARGE_INTEGER;
#define TRUE 1
#define FALSE 0
#define WINAPI
#define WSAAPI
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define SD_SEND SHUT_WR
#define WSAEWOULDBLOCK 10035
#define WSAEINVAL 10022
#define NO_ERROR 0
#define _TRUNCATE ((size_t)-1)
#define INVALID_HANDLE_VALUE ((HANDLE)(long)-1)
#define INVALID_FILE_SIZE 0xFFFFFFFFu
#define INVALID_SET_FILE_POINTER 0xFFFFFFFFu
#define GENERIC_READ 0x80000000u
#define FILE_SHARE_READ 1
#define OPEN_EXISTING 3
#define FILE_BEGIN 0
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_FLAG_NO_BUFFERING 0x20000000
#define FILE_FLAG_RANDOM_ACCESS 0x10000000
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000

static inline int WSAGetLastError() {
    if(errno==EWOULDBLOCK || errno==EAGAIN || errno==EINPROGRESS) return WSAEWOULDBLOCK;
    return errno ? 20000+errno : 0;
}
static inline void WSASetLastError(int code) { errno=code==WSAEWOULDBLOCK ? EWOULDBLOCK : code; }
static inline DWORD GetLastError() { return DWORD(errno); }
static inline int closesocket(SOCKET s) { return close(s); }
static inline int ioctlsocket(SOCKET s, long, u_long* value) {
    int flags=fcntl(s,F_GETFL,0); if(flags<0) return -1;
    return fcntl(s,F_SETFL,*value ? flags|O_NONBLOCK : flags&~O_NONBLOCK)<0 ? -1 : 0;
}
#undef FIONBIO
#define FIONBIO 0
// Winsock ignores select's first argument; POSIX needs the highest fd + 1.
static inline int nb_select(int, fd_set* r, fd_set* w, fd_set* e, struct timeval* t) {
    int rc=select(FD_SETSIZE,r,w,e,t); return rc<0 ? -1 : rc;
}
#define select nb_select
static inline int nb_send(SOCKET s, const char* data, int count, int flags) { return int(send(s,data,size_t(count),flags|MSG_NOSIGNAL)); }
#define send nb_send
static inline int nb_recv(SOCKET s, char* data, int count, int flags) { return int(recv(s,data,size_t(count),flags)); }
#define recv nb_recv
static inline int nb_getsockopt(SOCKET s, int level, int name, char* value, int* length) {
    socklen_t l=socklen_t(*length); int rc=getsockopt(s,level,name,value,&l); *length=int(l); return rc;
}
#define getsockopt nb_getsockopt
static inline void Sleep(DWORD ms) { usleep(ms ? ms*1000 : 0); if(!ms) sched_yield(); }

static inline BOOL QueryPerformanceCounter(LARGE_INTEGER* c) {
    timespec t; clock_gettime(CLOCK_MONOTONIC,&t); c->QuadPart=LONGLONG(t.tv_sec)*1000000000LL+t.tv_nsec; return TRUE;
}
static inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* f) { f->QuadPart=1000000000LL; return TRUE; }
static inline int _vsnprintf_s(char* out, size_t capacity, size_t, const char* format, va_list a) {
    if(!capacity) return -1;
    int n=vsnprintf(out,capacity,format,a);
    return n<0 || size_t(n)>=capacity ? -1 : n;
}
static inline int _snprintf_s(char* out, size_t capacity, size_t count, const char* format, ...) {
    va_list a; va_start(a,format); int n=_vsnprintf_s(out,capacity,count,format,a); va_end(a); return n;
}
static inline int strcpy_s(char* out, size_t capacity, const char* text) {
    size_t n=strlen(text); if(n>=capacity) { if(capacity) out[0]=0; return 34; }
    memcpy(out,text,n+1); return 0;
}
template<size_t N> static inline int strcpy_s(char (&out)[N], const char* text) { return strcpy_s(out,N,text); }
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
// Files: HANDLE carries the descriptor + 1, so 0 stays an invalid value.
static inline HANDLE CreateFile(const char* path, DWORD, DWORD, void*, DWORD, DWORD, void*) {
    int fd=open(path,O_RDONLY); if(fd<0) return INVALID_HANDLE_VALUE;
    struct stat st; if(fstat(fd,&st) || S_ISDIR(st.st_mode)) { close(fd); errno=EISDIR; return INVALID_HANDLE_VALUE; }
    return reinterpret_cast<HANDLE>(long(fd)+1);
}
static inline int nb_fd(HANDLE h) { return int(reinterpret_cast<long>(h)-1); }
static inline BOOL CloseHandle(HANDLE h) { return close(nb_fd(h))==0; }
static inline DWORD GetFileSize(HANDLE h, DWORD* high) {
    struct stat st; if(fstat(nb_fd(h),&st)) return INVALID_FILE_SIZE;
    errno=0; *high=DWORD((unsigned long long)st.st_size>>32); return DWORD(st.st_size);
}
static inline DWORD SetFilePointer(HANDLE h, LONG low, LONG* high, DWORD) {
    off_t at=off_t(((unsigned long long)(DWORD)*high<<32)|(DWORD)low);
    errno=0; if(lseek(nb_fd(h),at,SEEK_SET)<0) return INVALID_SET_FILE_POINTER;
    return DWORD(at);
}
static inline BOOL ReadFile(HANDLE h, void* data, DWORD count, DWORD* got, void*) {
    DWORD total=0;
    while(total<count) {
        ssize_t n=read(nb_fd(h),static_cast<char*>(data)+total,count-total);
        if(n<0) { if(errno==EINTR) continue; *got=total; return FALSE; }
        if(n==0) break;
        total+=DWORD(n);
    }
    *got=total; return TRUE;
}
#endif
