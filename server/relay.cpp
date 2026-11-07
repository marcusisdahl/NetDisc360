// SPDX-License-Identifier: MIT
// The party relay's HTTP client (see relay.h).
#include "relay.h"
#include "version.h"
#include "../shared/platform.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#ifdef _WIN32
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#else
#include <netdb.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace relay {
namespace {
const size_t AnswerMost = 256 * 1024;

bool Starts(const std::string& text, const char* start) { return text.compare(0,strlen(start),start)==0; }
}  // namespace

bool GoodUrl(const std::string& url, std::string& error) {
    const bool secure=Starts(url,"https://"), plain=Starts(url,"http://");
    const size_t from=secure ? 8 : 7;
    if(!secure && !plain) { error="The relay's address starts with https://."; return false; }
    if(url.size()>200) { error="The relay's address is too long."; return false; }
    for(size_t i=0;i<url.size();++i) {
        const char c=url[i];
        if(!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || strchr(".-_~:/%",c))) {
            error=std::string("The relay's address can't have ")+(c==' ' ? "spaces" : "'"+std::string(1,c)+"'")+" in it.";
            return false;
        }
    }
    const size_t slash=url.find('/',from);
    const std::string host=url.substr(from,slash==std::string::npos ? std::string::npos : slash-from);
    if(host.empty() || host[0]==':' || host[0]=='.') { error="The relay's address has no host name."; return false; }
    return true;
}

#ifdef _WIN32
namespace {
std::wstring Wide(const std::string& text) {
    int n=MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),NULL,0);
    std::wstring out(size_t(n),L'\0');
    if(n) MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),&out[0],n);
    return out;
}
struct Handle {
    HINTERNET h;
    explicit Handle(HINTERNET handle) : h(handle) {}
    ~Handle() { if(h) WinHttpCloseHandle(h); }
};
}  // namespace

bool Post(const std::string& url, const std::string& key, const std::string& json, double seconds,
          int& status, std::string& body, std::string& error) {
    status=0; body.clear();
    if(!GoodUrl(url,error)) return false;
    std::wstring wide=Wide(url);
    wchar_t host[256], path[2048], extra[2048];
    URL_COMPONENTS parts;
    memset(&parts,0,sizeof(parts));
    parts.dwStructSize=sizeof(parts);
    parts.lpszHostName=host; parts.dwHostNameLength=ARRAYSIZE(host);
    parts.lpszUrlPath=path; parts.dwUrlPathLength=ARRAYSIZE(path);
    parts.lpszExtraInfo=extra; parts.dwExtraInfoLength=ARRAYSIZE(extra);
    if(!WinHttpCrackUrl(wide.c_str(),0,0,&parts)) { error="The relay's address isn't one Windows understands."; return false; }
    std::wstring agent=L"NetDisc360/"+Wide(NETDISC_VERSION);
    Handle session(WinHttpOpen(agent.c_str(),WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0));
    if(!session.h) session.h=WinHttpOpen(agent.c_str(),WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if(!session.h) { error="Windows' HTTP client didn't start."; return false; }
    const int ms=int(seconds*1000);
    WinHttpSetTimeouts(session.h,ms,ms,ms,ms);
    Handle connection(WinHttpConnect(session.h,host,parts.nPort,0));
    std::wstring target=std::wstring(path)+extra;
    Handle request(connection.h ? WinHttpOpenRequest(connection.h,L"POST",target.c_str(),NULL,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                     parts.nScheme==INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : NULL);
    std::wstring headers=L"Content-Type: application/json\r\nAuthorization: Bearer "+Wide(key);
    if(!request.h || !WinHttpSendRequest(request.h,headers.c_str(),DWORD(headers.size()),const_cast<char*>(json.data()),DWORD(json.size()),DWORD(json.size()),0) ||
       !WinHttpReceiveResponse(request.h,NULL)) {
        char text[160];
        _snprintf_s(text,sizeof(text),_TRUNCATE,"The relay doesn't answer (error %lu). Is this PC online?",GetLastError());
        error=text;
        return false;
    }
    DWORD code=0, size=sizeof(code);
    WinHttpQueryHeaders(request.h,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&code,&size,WINHTTP_NO_HEADER_INDEX);
    status=int(code);
    std::vector<char> part(16384);
    for(;;) {
        DWORD got=0;
        if(!WinHttpReadData(request.h,part.data(),DWORD(part.size()),&got)) { error="The relay's answer stopped part way."; return false; }
        if(!got) break;
        body.append(part.data(),got);
        if(body.size()>AnswerMost) { error="The relay's answer is larger than expected."; return false; }
    }
    return true;
}
#else
namespace {
// Plain http: one request on a connection of its own.
bool PostPlain(const std::string& url, const std::string& key, const std::string& json, double seconds,
               int& status, std::string& body, std::string& error) {
    const size_t slash=url.find('/',7);
    std::string host=url.substr(7,slash==std::string::npos ? std::string::npos : slash-7), path=slash==std::string::npos ? "/" : url.substr(slash);
    std::string port="80";
    const size_t colon=host.rfind(':');
    if(colon!=std::string::npos) { port=host.substr(colon+1); host=host.substr(0,colon); }
    addrinfo hints, * found=NULL;
    memset(&hints,0,sizeof(hints));
    hints.ai_family=AF_INET; hints.ai_socktype=SOCK_STREAM;
    if(getaddrinfo(host.c_str(),port.c_str(),&hints,&found) || !found) { error="The relay's host name, "+host+", isn't known."; return false; }
    SOCKET s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    struct timeval wait; wait.tv_sec=long(seconds); wait.tv_usec=long((seconds-double(long(seconds)))*1e6);
    setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&wait),sizeof(wait));
    setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&wait),sizeof(wait));
    const bool connected=s!=INVALID_SOCKET && !connect(s,found->ai_addr,socklen_t(found->ai_addrlen));
    freeaddrinfo(found);
    if(!connected) { if(s!=INVALID_SOCKET) closesocket(s); error="The relay doesn't answer at "+host+":"+port+"."; return false; }
    std::string request="POST "+path+" HTTP/1.1\r\nHost: "+host+(port=="80" ? "" : ":"+port)+"\r\nConnection: close\r\n"
                        "User-Agent: NetDisc360/" NETDISC_VERSION "\r\nContent-Type: application/json\r\nAuthorization: Bearer "+key+"\r\n"
                        "Content-Length: "+std::to_string(json.size())+"\r\n\r\n"+json;
    bool ok=true;
    for(size_t sent=0;ok && sent<request.size();) {
        int n=send(s,request.data()+sent,int(request.size()-sent),0);
        if(n>0) sent+=size_t(n); else ok=false;
    }
    std::string response;
    while(ok) {
        char part[16384];
        int n=recv(s,part,sizeof(part),0);
        if(n==0) break;
        if(n<0) { ok=false; break; }
        response.append(part,size_t(n));
        if(response.size()>AnswerMost) ok=false;
    }
    closesocket(s);
    int major=0, minor=0;
    size_t end=response.find("\r\n\r\n");
    if(!ok || sscanf(response.c_str(),"HTTP/%d.%d %d",&major,&minor,&status)!=3 || end==std::string::npos) {
        error="The relay's answer didn't come whole.";
        return false;
    }
    body=response.substr(end+4);
    std::string head=response.substr(0,end);
    for(size_t i=0;i<head.size();++i) head[i]=char(tolower((unsigned char)head[i]));
    if(head.find("\ntransfer-encoding: chunked")!=std::string::npos) {
        std::string whole;
        for(size_t at=0;at<body.size();) {
            size_t line=body.find("\r\n",at);
            if(line==std::string::npos) break;
            unsigned long size=strtoul(body.c_str()+at,NULL,16);
            if(!size || line+2+size>body.size()) break;
            whole.append(body,line+2,size);
            at=line+2+size+2;
        }
        body=whole;
    }
    return true;
}

// https through curl (the Docker image has it): the answer, then its status
// on a line of its own.
bool PostCurl(const std::string& url, const std::string& key, const std::string& json, double seconds,
              int& status, std::string& body, std::string& error) {
    for(size_t i=0;i<key.size();++i) if(!isalnum((unsigned char)key[i])) { error="The relay's key isn't right."; return false; }
    char file[]="/tmp/netdisc-relay-XXXXXX";
    int fd=mkstemp(file);
    if(fd<0) { error="A file for the relay couldn't be made."; return false; }
    bool written=write(fd,json.data(),json.size())==ssize_t(json.size());
    close(fd);
    std::string command="curl -sS --max-time "+std::to_string(int(seconds+0.5))+" -X POST -H 'Content-Type: application/json' -H 'Authorization: Bearer "+key+"'"
                        " -A 'NetDisc360/" NETDISC_VERSION "' --data-binary @"+file+" -w '\\n%{http_code}' '"+url+"' 2>&1";
    std::string out;
    FILE* p=written ? popen(command.c_str(),"r") : NULL;
    if(p) {
        char part[16384];
        size_t n;
        while((n=fread(part,1,sizeof(part),p))>0 && out.size()<=AnswerMost) out.append(part,n);
    }
    const int result=p ? pclose(p) : -1;
    unlink(file);
    if(!p) { error="curl couldn't be started."; return false; }
    if(WIFEXITED(result) && WEXITSTATUS(result)==127) { error="This server reaches https relays through curl, which isn't installed."; return false; }
    const size_t last=out.rfind('\n');
    status=last==std::string::npos ? 0 : atoi(out.c_str()+last+1);
    if(result!=0 || !status) {
        error="The relay doesn't answer: "+out.substr(0,out.find('\n'));
        return false;
    }
    body=out.substr(0,last);
    return true;
}
}  // namespace

bool Post(const std::string& url, const std::string& key, const std::string& json, double seconds,
          int& status, std::string& body, std::string& error) {
    status=0; body.clear();
    if(!GoodUrl(url,error)) return false;
    return Starts(url,"http://") ? PostPlain(url,key,json,seconds,status,body,error) : PostCurl(url,key,json,seconds,status,body,error);
}
#endif
}  // namespace relay
