// SPDX-License-Identifier: MIT
#include "http.h"
#include "../shared/platform.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <winhttp.h>
#else
#include <netdb.h>
#endif

namespace nia {
namespace {
const int TimeoutMs = 20000;
std::string Text(const char* format, unsigned long code) {
    char text[160]; _snprintf_s(text,sizeof(text),_TRUNCATE,format,code); return text;
}
}

#ifdef _WIN32
bool HttpGet(const std::string& url, std::vector<unsigned char>& body, size_t limit, int& status, std::string& error) {
    body.clear(); status=0;
    std::wstring wide(url.begin(),url.end());
    URL_COMPONENTS parts; memset(&parts,0,sizeof(parts)); parts.dwStructSize=sizeof(parts);
    wchar_t host[256],path[2048];
    parts.lpszHostName=host; parts.dwHostNameLength=256; parts.lpszUrlPath=path; parts.dwUrlPathLength=2048;
    if(!WinHttpCrackUrl(wide.c_str(),0,0,&parts)) { error="Bad address "+url; return false; }
    HINTERNET session=WinHttpOpen(L"netiso-server",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if(!session) session=WinHttpOpen(L"netiso-server",WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if(!session) { error=Text("WinHttpOpen failed (%lu)",GetLastError()); return false; }
    WinHttpSetTimeouts(session,TimeoutMs,TimeoutMs,TimeoutMs,TimeoutMs);
    HINTERNET connection=WinHttpConnect(session,host,parts.nPort,0);
    HINTERNET request=connection ? WinHttpOpenRequest(connection,L"GET",path,NULL,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                       parts.nScheme==INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : NULL;
    bool ok=false;
    if(!request) error=Text("Can't open the request (%lu)",GetLastError());
    else if(!WinHttpSendRequest(request,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0) || !WinHttpReceiveResponse(request,NULL))
        error=Text("No answer (%lu)",GetLastError());
    else {
        DWORD code=0,size=sizeof(code);
        WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&code,&size,WINHTTP_NO_HEADER_INDEX);
        status=int(code);
        if(code!=200) error=Text("HTTP %lu",code);
        else {
            ok=true;
            for(;;) {
                DWORD available=0;
                if(!WinHttpQueryDataAvailable(request,&available)) { ok=false; error=Text("Download failed (%lu)",GetLastError()); break; }
                if(!available) break;
                if(body.size()+available>limit) { ok=false; error="Too large"; break; }
                size_t at=body.size(); body.resize(at+available);
                DWORD got=0;
                if(!WinHttpReadData(request,&body[at],available,&got)) { ok=false; error=Text("Download failed (%lu)",GetLastError()); break; }
                body.resize(at+got);
            }
        }
    }
    if(request) WinHttpCloseHandle(request);
    if(connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ok;
}
#else
// Plain http/1.0 to the host tests' server.
bool HttpGet(const std::string& url, std::vector<unsigned char>& body, size_t limit, int& status, std::string& error) {
    body.clear(); status=0;
    if(url.compare(0,7,"http://")) { error="Only http:// here: "+url; return false; }
    size_t slash=url.find('/',7);
    std::string host=url.substr(7,slash==std::string::npos ? std::string::npos : slash-7),path=slash==std::string::npos ? "/" : url.substr(slash);
    std::string port="80";
    size_t colon=host.find(':');
    if(colon!=std::string::npos) { port=host.substr(colon+1); host.erase(colon); }
    addrinfo hints; memset(&hints,0,sizeof(hints)); hints.ai_family=AF_INET; hints.ai_socktype=SOCK_STREAM;
    addrinfo* found=NULL;
    if(getaddrinfo(host.c_str(),port.c_str(),&hints,&found) || !found) { error="Can't find "+host; return false; }
    SOCKET s=socket(AF_INET,SOCK_STREAM,0);
    timeval timeout={TimeoutMs/1000,0};
    setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    bool connected=connect(s,found->ai_addr,found->ai_addrlen)==0;
    freeaddrinfo(found);
    if(!connected) { closesocket(s); error="Can't connect to "+host; return false; }
    std::string request="GET "+path+" HTTP/1.0\r\nHost: "+host+"\r\nUser-Agent: netiso-server\r\n\r\n";
    send(s,request.data(),int(request.size()),0);
    std::vector<unsigned char> all;
    char part[16384];
    for(;;) {
        int got=recv(s,part,sizeof(part),0);
        if(got<=0) break;
        all.insert(all.end(),part,part+got);
        if(all.size()>limit+65536) break;
    }
    closesocket(s);
    std::string head(all.begin(),all.end());
    size_t end=head.find("\r\n\r\n");
    if(end==std::string::npos || sscanf(head.c_str(),"HTTP/%*s %d",&status)!=1) { error="Bad answer from "+host; return false; }
    if(status!=200) { error=Text("HTTP %lu",(unsigned long)status); return false; }
    if(all.size()-(end+4)>limit) { error="Too large"; return false; }
    body.assign(all.begin()+long(end+4),all.end());
    return true;
}
#endif
}
