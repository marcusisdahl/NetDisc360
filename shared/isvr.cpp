// SPDX-License-Identifier: MIT
// Adapted from FastFTP's NetIsoClient (shared/readbench.cpp), which was tested
// against netiso-srv and its own fault-injecting server.
#include "isvr.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace nia {
enum { Ping_ = 0, GetIsoSize = 1, ReadData = 3, GetIsoName = 4, MountIso = 5, Header = 20 };

DWORD Read32(const unsigned char* p) { return (DWORD(p[0])<<24)|(DWORD(p[1])<<16)|(DWORD(p[2])<<8)|p[3]; }
void Write32(unsigned char* p, DWORD v) { p[0]=BYTE(v>>24); p[1]=BYTE(v>>16); p[2]=BYTE(v>>8); p[3]=BYTE(v); }
void AddressText(DWORD address, char* text, size_t capacity) {
    DWORD host=ntohl(address);
    _snprintf_s(text,capacity,_TRUNCATE,"%u.%u.%u.%u",host>>24,(host>>16)&0xFF,(host>>8)&0xFF,host&0xFF);
}
double Now() {
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter); QueryPerformanceFrequency(&frequency);
    return double(counter.QuadPart)/double(frequency.QuadPart);
}
static void Request(unsigned char* p, int command, DWORD index, U64 offset, DWORD length) {
    p[0]='I'; p[1]='S'; p[2]='V'; p[3]='R';
    p[4]=BYTE(command>>8); p[5]=BYTE(command); p[6]=BYTE(index>>8); p[7]=BYTE(index);
    Write32(p+8,DWORD(offset>>32)); Write32(p+12,DWORD(offset)); Write32(p+16,length);
}

IsvrClient::IsvrClient(double timeout) : socket_(INVALID_SOCKET), timeout_(timeout), size_(0) { error_[0]=0; }
IsvrClient::~IsvrClient() { Close(); }
void IsvrClient::Close() {
    if(socket_!=INVALID_SOCKET) { shutdown(socket_,SD_SEND); closesocket(socket_); socket_=INVALID_SOCKET; }
}
bool IsvrClient::Fail(const char* format, ...) {
    va_list a; va_start(a,format); _vsnprintf_s(error_,sizeof(error_),_TRUNCATE,format,a); va_end(a);
    Close(); // A stream that failed mid-reply cannot be resynchronized.
    return false;
}
bool IsvrClient::Wait(bool writing, double deadline) {
    for(;;) {
        if(Now()>deadline) return Fail("No reply from the server for %.0f s",timeout_);
        fd_set set; FD_ZERO(&set); FD_SET(socket_,&set);
        timeval interval={0,100000};
        int rc=select(0,writing ? NULL : &set,writing ? &set : NULL,NULL,&interval);
        if(rc>0) return true;
        if(rc==SOCKET_ERROR) return Fail("Socket wait failed (%d)",WSAGetLastError());
    }
}
bool IsvrClient::SendAll(const unsigned char* data, int count) {
    if(socket_==INVALID_SOCKET) return Fail("Not connected");
    const double deadline=Now()+timeout_;
    while(count) {
        if(!Wait(true,deadline)) return false;
        int sent=send(socket_,reinterpret_cast<const char*>(data),count,0);
        if(sent>0) { data+=sent; count-=sent; }
        else if(sent==0 || WSAGetLastError()!=WSAEWOULDBLOCK) return Fail("Connection lost while sending (%d)",WSAGetLastError());
        else Sleep(1);
    }
    return true;
}
// The idle timeout restarts whenever data arrives.
bool IsvrClient::ReceiveAll(unsigned char* data, DWORD count) {
    DWORD received=0; double deadline=Now()+timeout_;
    while(received<count) {
        if(!Wait(false,deadline)) return false;
        int got=recv(socket_,reinterpret_cast<char*>(data+received),int(count-received),0);
        if(got>0) { received+=DWORD(got); deadline=Now()+timeout_; continue; }
        if(got==0) return Fail("The server closed the connection after %u of %u bytes",received,count);
        if(WSAGetLastError()!=WSAEWOULDBLOCK) return Fail("Connection lost while receiving (%d)",WSAGetLastError());
        Sleep(1);
    }
    return true;
}
bool IsvrClient::Connect(DWORD address, int port, double connectSeconds) {
    Close(); size_=0;
    socket_=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if(socket_==INVALID_SOCKET) return Fail("Socket create failed (%d)",WSAGetLastError());
#ifdef _XBOX
    // Plain TCP to a LAN host: without these XNet only allows secure traffic.
    BOOL enabled=TRUE;
    if(setsockopt(socket_,SOL_SOCKET,0x5801,reinterpret_cast<const char*>(&enabled),sizeof(enabled)) ||
       setsockopt(socket_,SOL_SOCKET,0x5802,reinterpret_cast<const char*>(&enabled),sizeof(enabled)))
        return Fail("LAN socket options failed (%d)",WSAGetLastError());
#endif
    u_long nonblocking=1;
    if(ioctlsocket(socket_,FIONBIO,&nonblocking)) return Fail("Nonblocking socket failed (%d)",WSAGetLastError());
    sockaddr_in to; memset(&to,0,sizeof(to)); to.sin_family=AF_INET; to.sin_addr.s_addr=address; to.sin_port=htons(u_short(port));
    if(connect(socket_,reinterpret_cast<sockaddr*>(&to),sizeof(to))) {
        int code=WSAGetLastError();
        if(code!=WSAEWOULDBLOCK) return Fail("Connect failed (%d)",code);
        const double deadline=Now()+connectSeconds;
        for(;;) {
            if(Now()>deadline) { char where[16]; AddressText(address,where,sizeof(where)); return Fail("No answer from %s port %d",where,port); }
            fd_set writable,failed; FD_ZERO(&writable); FD_ZERO(&failed); FD_SET(socket_,&writable); FD_SET(socket_,&failed);
            timeval interval={0,100000};
            int rc=select(0,NULL,&writable,&failed,&interval);
            if(rc==SOCKET_ERROR) return Fail("Connect wait failed (%d)",WSAGetLastError());
            if(rc==0) continue;
            int status=0;
#ifdef SO_ERROR
            int length=sizeof(status);
            if(getsockopt(socket_,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&status),&length)) status=0;
#endif
            // The Xbox SDK has no SO_ERROR; a refused connect shows in the except set.
            if(FD_ISSET(socket_,&failed) || status) return Fail("The server refused the connection. Is it running on port %d?",port);
            break;
        }
    }
    return true;
}
bool IsvrClient::Ping() {
    unsigned char request[Header],reply[8];
    Request(request,Ping_,0,0,0);
    if(!SendAll(request,Header) || !ReceiveAll(reply,8)) return false;
    if(memcmp(reply,"ISVRokOK",8)) return Fail("Not a NetISO server");
    return true;
}
bool IsvrClient::Name(int index, char* name) {
    unsigned char request[Header],reply[IsvrNameBytes];
    Request(request,GetIsoName,DWORD(index),0,IsvrNameBytes);
    if(!SendAll(request,Header) || !ReceiveAll(reply,IsvrNameBytes)) return false;
    const unsigned char* end=static_cast<const unsigned char*>(memchr(reply,0,IsvrNameBytes));
    if(!end) return Fail("ISO %d has a name of 256 bytes or more",index);
    memcpy(name,reply,size_t(end-reply)+1);
    return true;
}
bool IsvrClient::Mount(const char* name) {
    size_t length=strlen(name);
    if(!length || length>=IsvrNameBytes) return Fail("Bad ISO name");
    // Header and name in one send: netiso-srv reads the name with one read().
    unsigned char message[Header+IsvrNameBytes],reply[8];
    Request(message,MountIso,0,0,DWORD(length)); memcpy(message+Header,name,length);
    size_=0;
    if(!SendAll(message,int(Header+length)) || !ReceiveAll(reply,4)) return false;
    DWORD code=Read32(reply);
    if(code!=1) { _snprintf_s(error_,sizeof(error_),_TRUNCATE,"The server could not mount %s (%u)",name,code); return code==0 ? false : Fail("%s",error_); }
    Request(message,GetIsoSize,0,0,0);
    if(!SendAll(message,Header) || !ReceiveAll(reply,8)) return false;
    if(Read32(reply+4)!=IsvrSector) return Fail("Unexpected sector size %u",Read32(reply+4));
    size_=U64(Read32(reply))*IsvrSector;
    return true;
}
bool IsvrClient::Read(U64 offset, DWORD length, unsigned char* data) {
    // Reading past the end makes the server drop the connection.
    if(!length || length>IsvrMaxRead || offset>size_ || length>size_-offset) {
        _snprintf_s(error_,sizeof(error_),_TRUNCATE,"Read outside the ISO"); return false;
    }
    unsigned char request[Header];
    Request(request,ReadData,0,offset,length);
    return SendAll(request,Header) && ReceiveAll(data,length);
}
}
