// SPDX-License-Identifier: MIT
// Updates from GitHub's releases (see update.h).
#include "update.h"
#include "version.h"
#include "web.h"
#include "../shared/platform.h"
#include <algorithm>
#include <stdlib.h>
#include <string.h>
#include <vector>
#ifdef _WIN32
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#endif

namespace update {
namespace {
std::string Lower(std::string text) {
    for(size_t i=0;i<text.size();++i) if(text[i]>='A' && text[i]<='Z') text[i]=char(text[i]-'A'+'a');
    return text;
}

// "v1.2.3-beta.1+build" as its numbers (1, 2, 3) and its pre-release part (beta.1).
void Split(const std::string& version, std::vector<long>& numbers, std::string& pre) {
    std::string v=version;
    if(!v.empty() && (v[0]=='v' || v[0]=='V')) v.erase(0,1);
    size_t plus=v.find('+');
    if(plus!=std::string::npos) v.erase(plus);
    size_t dash=v.find('-');
    pre=dash==std::string::npos ? std::string() : v.substr(dash+1);
    v=v.substr(0,dash);
    for(size_t at=0;at<=v.size();) {
        size_t dot=v.find('.',at);
        if(dot==std::string::npos) dot=v.size();
        numbers.push_back(atol(v.substr(at,dot-at).c_str()));
        at=dot+1;
    }
    while(numbers.size()>1 && !numbers.back()) numbers.pop_back();
}

bool Digits(const std::string& text) { return !text.empty() && text.find_first_not_of("0123456789")==std::string::npos; }

int ComparePre(const std::string& a, const std::string& b) {
    if(a.empty() || b.empty()) return a.empty() ? (b.empty() ? 0 : 1) : -1;
    for(size_t i=0,j=0;;) {
        if(i>a.size() || j>b.size()) return i>a.size() ? (j>b.size() ? 0 : -1) : 1;
        size_t ei=a.find('.',i), ej=b.find('.',j);
        if(ei==std::string::npos) ei=a.size();
        if(ej==std::string::npos) ej=b.size();
        std::string x=a.substr(i,ei-i), y=b.substr(j,ej-j);
        int c;
        if(Digits(x) && Digits(y)) c=atol(x.c_str())<atol(y.c_str()) ? -1 : atol(x.c_str())>atol(y.c_str());
        else if(Digits(x)!=Digits(y)) c=Digits(x) ? -1 : 1;
        else c=x.compare(y)<0 ? -1 : x.compare(y)>0;
        if(c) return c;
        i=ei+1; j=ej+1;
    }
}

std::string Text(const web::Value& v, const char* key) {
    const web::Value* f=v.Get(key);
    return f && f->type==web::Value::String ? f->text : std::string();
}

bool Flag(const web::Value& v, const char* key) {
    const web::Value* f=v.Get(key);
    return f && f->type==web::Value::Bool && f->boolean;
}
}  // namespace

int Compare(const std::string& a, const std::string& b) {
    std::vector<long> x, y;
    std::string px, py;
    Split(a,x,px);
    Split(b,y,py);
    for(size_t i=0;i<x.size() || i<y.size();++i) {
        long p=i<x.size() ? x[i] : 0, q=i<y.size() ? y[i] : 0;
        if(p!=q) return p<q ? -1 : 1;
    }
    return ComparePre(px,py);
}

std::string ReleasesUrl() { return "https://api.github.com/repos/" NETDISC_REPO "/releases?per_page=20"; }

namespace {
Release Read(const web::Value& r) {
    Release out;
    out.tag=Text(r,"tag_name");
    out.version=out.tag[0]=='v' || out.tag[0]=='V' ? out.tag.substr(1) : out.tag;
    out.name=Text(r,"name");
    out.notes=Text(r,"body");
    out.page=Text(r,"html_url");
    out.published=Text(r,"published_at");
    out.prerelease=Flag(r,"prerelease");
    const web::Value* assets=r.Get("assets");
    for(size_t i=0;assets && assets->type==web::Value::Array && i<assets->items.size();++i) {
        const web::Value& a=assets->items[i];
        if(a.type!=web::Value::Object || Lower(Text(a,"name"))!="netdisc360.exe") continue;
        out.exeUrl=Text(a,"browser_download_url");
        out.exeDigest=Lower(Text(a,"digest"));
        const web::Value* size=a.Get("size");
        out.exeSize=size && size->type==web::Value::Number ? (long long)size->number : 0;
    }
    return out;
}
}  // namespace

bool List(const std::string& json, bool prereleases, std::vector<Release>& out, std::string& error) {
    out.clear();
    web::Value list;
    if(!web::Parse(json,list) || list.type!=web::Value::Array) { error="GitHub's answer wasn't a list of releases."; return false; }
    for(size_t i=0;i<list.items.size();++i) {
        const web::Value& r=list.items[i];
        if(r.type!=web::Value::Object || Flag(r,"draft") || (Flag(r,"prerelease") && !prereleases) || Text(r,"tag_name").empty()) continue;
        out.push_back(Read(r));
    }
    // Newest first; of two with the same version, the one GitHub listed first.
    std::stable_sort(out.begin(),out.end(),[](const Release& a, const Release& b) { return Compare(a.tag,b.tag)>0; });
    if(out.empty()) { error=prereleases ? "There are no releases on GitHub yet." : "There are no releases on GitHub yet (only test versions, if any)."; return false; }
    return true;
}

bool Newest(const std::string& json, bool prereleases, Release& out, std::string& error) {
    std::vector<Release> list;
    if(!List(json,prereleases,list,error)) return false;
    out=list[0];
    return true;
}

bool CheckExe(const std::string& data, const Release& r, std::string& error) {
    if(r.exeSize && (long long)data.size()!=r.exeSize) { error="The download was cut short; try again."; return false; }
    if(r.exeDigest.empty()) { error="GitHub gave no checksum for the release's NetDisc360.exe, so it wasn't installed."; return false; }
    if(r.exeDigest.compare(0,7,"sha256:") || web::Sha256Hex(data)!=r.exeDigest.substr(7)) {
        error="The download doesn't match the release's checksum, so it wasn't installed.";
        return false;
    }
    // A PE file for x64: "MZ", and at e_lfanew "PE\0\0" and machine 0x8664.
    unsigned pe=data.size()>=64 ? (unsigned char)data[60]|((unsigned char)data[61]<<8)|((unsigned char)data[62]<<16)|(unsigned(data[63]&0xFF)<<24) : 0;
    if(data.size()<64 || data.compare(0,2,"MZ") || size_t(pe)+6>data.size() || data.compare(pe,4,std::string("PE\0\0",4)) ||
       (unsigned char)data[pe+4]!=0x64 || (unsigned char)data[pe+5]!=0x86) {
        error="The download isn't a 64-bit Windows program, so it wasn't installed.";
        return false;
    }
    return true;
}

bool FromRepo(const std::string& url) {
    std::string from=Lower("https://github.com/" NETDISC_REPO "/releases/download/");
    return Lower(url.substr(0,from.size()))==from && url.find("..")==std::string::npos;
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

bool CanFetch() { return true; }

bool Fetch(const std::string& url, std::string& body, long long limit, Progress progress, std::string& error) {
    body.clear();
    std::wstring wide=Wide(url);
    wchar_t host[256], path[2048], extra[2048];
    URL_COMPONENTS parts;
    memset(&parts,0,sizeof(parts));
    parts.dwStructSize=sizeof(parts);
    parts.lpszHostName=host; parts.dwHostNameLength=ARRAYSIZE(host);
    parts.lpszUrlPath=path; parts.dwUrlPathLength=ARRAYSIZE(path);
    parts.lpszExtraInfo=extra; parts.dwExtraInfoLength=ARRAYSIZE(extra);
    if(!WinHttpCrackUrl(wide.c_str(),0,0,&parts) || parts.nScheme!=INTERNET_SCHEME_HTTPS) { error="Not an https address: "+url; return false; }
    std::wstring agent=L"NetDisc360/" + Wide(NETDISC_VERSION);
    Handle session(WinHttpOpen(agent.c_str(),WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0));
    if(!session.h) session.h=WinHttpOpen(agent.c_str(),WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);  // Before Windows 8.1.
    if(!session.h) { error="Windows' HTTP client didn't start."; return false; }
    WinHttpSetTimeouts(session.h,15000,15000,30000,60000);
    Handle connection(WinHttpConnect(session.h,host,parts.nPort,0));
    std::wstring target=std::wstring(path)+extra;
    Handle request(connection.h ? WinHttpOpenRequest(connection.h,L"GET",target.c_str(),NULL,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE) : NULL);
    if(request.h && !_wcsicmp(host,L"api.github.com"))
        WinHttpAddRequestHeaders(request.h,L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28",(DWORD)-1L,WINHTTP_ADDREQ_FLAG_ADD);
    if(!request.h || !WinHttpSendRequest(request.h,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0) || !WinHttpReceiveResponse(request.h,NULL)) {
        char text[128];
        _snprintf_s(text,sizeof(text),_TRUNCATE,"Can't reach GitHub (error %lu). Is this computer online?",GetLastError());
        error=text;
        return false;
    }
    DWORD status=0, size=sizeof(status);
    WinHttpQueryHeaders(request.h,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX);
    if(status!=200) {
        char text[160];
        if(status==404) _snprintf_s(text,sizeof(text),_TRUNCATE,"GitHub has no releases of NetDisc360 to show (404).");
        else if(status==403 || status==429) _snprintf_s(text,sizeof(text),_TRUNCATE,"GitHub is limiting checks from this network right now (%lu); NetDisc360 tries again later.",status);
        else _snprintf_s(text,sizeof(text),_TRUNCATE,"GitHub answered with status %lu.",status);
        error=text;
        return false;
    }
    DWORD length=0;
    size=sizeof(length);
    long long total=WinHttpQueryHeaders(request.h,WINHTTP_QUERY_CONTENT_LENGTH|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&length,&size,WINHTTP_NO_HEADER_INDEX) ? (long long)length : 0;
    if(total>limit) { error="GitHub's answer is larger than expected."; return false; }
    std::vector<char> part(65536);
    for(;;) {
        DWORD got=0;
        if(!WinHttpReadData(request.h,part.data(),DWORD(part.size()),&got)) {
            char text[128];
            _snprintf_s(text,sizeof(text),_TRUNCATE,"The download stopped (error %lu); try again.",GetLastError());
            error=text;
            return false;
        }
        if(!got) break;
        body.append(part.data(),got);
        if((long long)body.size()>limit) { error="GitHub's answer is larger than expected."; return false; }
        if(progress) progress((long long)body.size(),total);
    }
    return true;
}
#else
bool CanFetch() { return false; }

bool Fetch(const std::string&, std::string& body, long long, Progress, std::string& error) {
    body.clear();
    error="This build can't reach GitHub itself; the dashboard checks for updates.";
    return false;
}
#endif
}
