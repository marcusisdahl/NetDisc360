// SPDX-License-Identifier: MIT
// NetDisc360's HTTP client (see http.h).
#include "http.h"
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

namespace http {
namespace {
bool Starts(const std::string& text, const char* start) { return text.compare(0,strlen(start),start)==0; }

bool GoodKey(const std::string& key) {
    for(size_t i=0;i<key.size();++i) if(!isalnum((unsigned char)key[i]) && key[i]!='-' && key[i]!='_') return false;
    return key.size()<=200;
}
bool GoodHex(const std::string& hex) {
    for(size_t i=0;i<hex.size();++i) if(!isxdigit((unsigned char)hex[i])) return false;
    return hex.size()<=64;
}
bool GoodMethod(const std::string& m) { return m=="GET" || m=="PUT" || m=="POST" || m=="DELETE"; }
}  // namespace

bool GoodUrl(const std::string& url, const char* what, std::string& error) {
    const bool secure=Starts(url,"https://"), plain=Starts(url,"http://");
    const size_t from=secure ? 8 : 7;
    if(!secure && !plain) { error=std::string(what)+"'s address starts with https://."; return false; }
    if(url.size()>400) { error=std::string(what)+"'s address is too long."; return false; }
    for(size_t i=0;i<url.size();++i) {
        const char c=url[i];
        if(!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || strchr(".-_~:/%?&=",c))) {
            error=std::string(what)+"'s address can't have "+(c==' ' ? "spaces" : "'"+std::string(1,c)+"'")+" in it.";
            return false;
        }
    }
    const size_t slash=url.find_first_of("/?",from);
    const std::string host=url.substr(from,slash==std::string::npos ? std::string::npos : slash-from);
    if(host.empty() || host[0]==':' || host[0]=='.') { error=std::string(what)+"'s address has no host name."; return false; }
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

bool Send(const Call& call, int& status, std::string& body, std::string& error) {
    status=0; body.clear();
    if(!GoodUrl(call.url,"The service",error)) return false;
    if(!GoodKey(call.key) || !GoodHex(call.sha256) || !GoodMethod(call.method)) { error="That request can't be sent."; return false; }
    std::wstring wide=Wide(call.url);
    wchar_t host[256], path[2048], extra[2048];
    URL_COMPONENTS parts;
    memset(&parts,0,sizeof(parts));
    parts.dwStructSize=sizeof(parts);
    parts.lpszHostName=host; parts.dwHostNameLength=ARRAYSIZE(host);
    parts.lpszUrlPath=path; parts.dwUrlPathLength=ARRAYSIZE(path);
    parts.lpszExtraInfo=extra; parts.dwExtraInfoLength=ARRAYSIZE(extra);
    if(!WinHttpCrackUrl(wide.c_str(),0,0,&parts)) { error="That address isn't one Windows understands."; return false; }
    std::wstring agent=L"NetDisc360/"+Wide(NETDISC_VERSION);
    Handle session(WinHttpOpen(agent.c_str(),WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0));
    if(!session.h) session.h=WinHttpOpen(agent.c_str(),WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if(!session.h) { error="Windows' HTTP client didn't start."; return false; }
    const int ms=int(call.seconds*1000);
    WinHttpSetTimeouts(session.h,ms,ms,ms,ms);
    Handle connection(WinHttpConnect(session.h,host,parts.nPort,0));
    std::wstring target=std::wstring(path)+extra;
    Handle request(connection.h ? WinHttpOpenRequest(connection.h,Wide(call.method).c_str(),target.c_str(),NULL,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                     parts.nScheme==INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : NULL);
    std::wstring headers;
    if(!call.key.empty()) headers+=L"Authorization: Bearer "+Wide(call.key)+L"\r\n";
    if(!call.type.empty()) headers+=L"Content-Type: "+Wide(call.type)+L"\r\n";
    if(!call.sha256.empty()) headers+=L"X-Sha256: "+Wide(call.sha256)+L"\r\n";
    if(!request.h || !WinHttpSendRequest(request.h,headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),DWORD(headers.size()),
                                         call.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(call.body.data()),
                                         DWORD(call.body.size()),DWORD(call.body.size()),0) ||
       !WinHttpReceiveResponse(request.h,NULL)) {
        char text[160];
        _snprintf_s(text,sizeof(text),_TRUNCATE,"No answer (error %lu). Is this computer online?",GetLastError());
        error=text;
        return false;
    }
    DWORD code=0, size=sizeof(code);
    WinHttpQueryHeaders(request.h,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&code,&size,WINHTTP_NO_HEADER_INDEX);
    status=int(code);
    std::vector<char> part(65536);
    for(;;) {
        DWORD got=0;
        if(!WinHttpReadData(request.h,part.data(),DWORD(part.size()),&got)) { error="The answer stopped part way."; return false; }
        if(!got) break;
        body.append(part.data(),got);
        if(body.size()>call.most) { error="The answer is larger than expected."; return false; }
    }
    return true;
}
#else
namespace {
// Plain http: one request on a connection of its own.
bool SendPlain(const Call& call, int& status, std::string& body, std::string& error) {
    const size_t slash=call.url.find_first_of("/?",7);
    std::string host=call.url.substr(7,slash==std::string::npos ? std::string::npos : slash-7);
    std::string path=slash==std::string::npos ? "/" : call.url.substr(slash);
    if(path[0]=='?') path="/"+path;
    std::string port="80";
    const size_t colon=host.rfind(':');
    if(colon!=std::string::npos) { port=host.substr(colon+1); host=host.substr(0,colon); }
    addrinfo hints, * found=NULL;
    memset(&hints,0,sizeof(hints));
    hints.ai_family=AF_INET; hints.ai_socktype=SOCK_STREAM;
    if(getaddrinfo(host.c_str(),port.c_str(),&hints,&found) || !found) { error="The host name "+host+" isn't known."; return false; }
    SOCKET s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    struct timeval wait; wait.tv_sec=long(call.seconds); wait.tv_usec=long((call.seconds-double(long(call.seconds)))*1e6);
    setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&wait),sizeof(wait));
    setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&wait),sizeof(wait));
    const bool connected=s!=INVALID_SOCKET && !connect(s,found->ai_addr,socklen_t(found->ai_addrlen));
    freeaddrinfo(found);
    if(!connected) { if(s!=INVALID_SOCKET) closesocket(s); error="Nothing answers at "+host+":"+port+"."; return false; }
    std::string request=call.method+" "+path+" HTTP/1.1\r\nHost: "+host+(port=="80" ? "" : ":"+port)+"\r\nConnection: close\r\n"
                        "User-Agent: NetDisc360/" NETDISC_VERSION "\r\n";
    if(!call.key.empty()) request+="Authorization: Bearer "+call.key+"\r\n";
    if(!call.type.empty()) request+="Content-Type: "+call.type+"\r\n";
    if(!call.sha256.empty()) request+="X-Sha256: "+call.sha256+"\r\n";
    if(!call.body.empty() || call.method=="PUT" || call.method=="POST") request+="Content-Length: "+std::to_string(call.body.size())+"\r\n";
    request+="\r\n";
    request+=call.body;
    bool ok=true;
    for(size_t sent=0;ok && sent<request.size();) {
        int n=send(s,request.data()+sent,int(request.size()-sent),0);
        if(n>0) sent+=size_t(n); else ok=false;
    }
    std::string response;
    while(ok) {
        char part[65536];
        int n=recv(s,part,sizeof(part),0);
        if(n==0) break;
        if(n<0) { ok=false; break; }
        response.append(part,size_t(n));
        if(response.size()>call.most+16384) ok=false;
    }
    closesocket(s);
    int major=0, minor=0;
    size_t end=response.find("\r\n\r\n");
    if(!ok || sscanf(response.c_str(),"HTTP/%d.%d %d",&major,&minor,&status)!=3 || end==std::string::npos) {
        status=0;
        error="The answer didn't come whole.";
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
    if(body.size()>call.most) { body.clear(); error="The answer is larger than expected."; return false; }
    return true;
}

// https through curl (the Docker image has it): the body goes in and the
// answer comes back through files of their own, the status on stdout.
bool SendCurl(const Call& call, int& status, std::string& body, std::string& error) {
    char in[]="/tmp/netdisc-http-in-XXXXXX", out[]="/tmp/netdisc-http-out-XXXXXX";
    int fin=mkstemp(in);
    if(fin<0) { error="A file for the request couldn't be made."; return false; }
    int fout=mkstemp(out);
    if(fout<0) { close(fin); unlink(in); error="A file for the request couldn't be made."; return false; }
    close(fout);
    bool written=write(fin,call.body.data(),call.body.size())==ssize_t(call.body.size());
    close(fin);
    std::string command="curl -sS --max-time "+std::to_string(int(call.seconds+0.5))+" -X "+call.method+" -A 'NetDisc360/" NETDISC_VERSION "'";
    if(!call.key.empty()) command+=" -H 'Authorization: Bearer "+call.key+"'";
    if(!call.type.empty()) command+=" -H 'Content-Type: "+call.type+"'";
    if(!call.sha256.empty()) command+=" -H 'X-Sha256: "+call.sha256+"'";
    if(call.method=="PUT" || call.method=="POST") command+=std::string(" --data-binary @")+in;
    command+=std::string(" --max-filesize ")+std::to_string(call.most)+" -o "+out+" -w '%{http_code}' '"+call.url+"' 2>&1";
    std::string said;
    FILE* p=written ? popen(command.c_str(),"r") : NULL;
    if(p) {
        char part[4096];
        size_t n;
        while((n=fread(part,1,sizeof(part),p))>0 && said.size()<65536) said.append(part,n);
    }
    const int result=p ? pclose(p) : -1;
    unlink(in);
    FILE* f=fopen(out,"rb");
    if(f) {
        char part[65536];
        size_t n;
        while((n=fread(part,1,sizeof(part),f))>0 && body.size()<=call.most) body.append(part,n);
        fclose(f);
    }
    unlink(out);
    if(!p) { error="curl couldn't be started."; return false; }
    if(WIFEXITED(result) && WEXITSTATUS(result)==127) { error="This server reaches https addresses through curl, which isn't installed."; return false; }
    const size_t digits=said.find_last_not_of("0123456789");
    status=atoi(said.c_str()+(digits==std::string::npos ? 0 : digits+1));
    if(result!=0 || !status) {
        status=0; body.clear();
        error="No answer: "+said.substr(0,said.find('\n'));
        return false;
    }
    if(body.size()>call.most) { body.clear(); error="The answer is larger than expected."; return false; }
    return true;
}
}  // namespace

bool Send(const Call& call, int& status, std::string& body, std::string& error) {
    status=0; body.clear();
    if(!GoodUrl(call.url,"The service",error)) return false;
    if(!GoodKey(call.key) || !GoodHex(call.sha256) || !GoodMethod(call.method)) { error="That request can't be sent."; return false; }
    return Starts(call.url,"http://") ? SendPlain(call,status,body,error) : SendCurl(call,status,body,error);
}
#endif
}  // namespace http
