// SPDX-License-Identifier: MIT
#include "titleupdates.h"
#include "web.h"
#include <algorithm>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <ws2tcpip.h>
#include <winhttp.h>
#else
#include <netdb.h>
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

namespace tu {
const char CacheFolder[] = "/Hdd1/Cache";

namespace {
DWORD Big32(const unsigned char* p) { return (DWORD(p[0])<<24)|(DWORD(p[1])<<16)|(DWORD(p[2])<<8)|p[3]; }

std::string Hex8(DWORD v) {
    char text[12]; snprintf(text,sizeof(text),"%08X",(unsigned)v);
    return text;
}

// A JSON value as text: strings as they are, numbers without a fraction.
std::string AsText(const web::Value* v) {
    if(!v) return std::string();
    if(v->type==web::Value::String) return v->text;
    if(v->type==web::Value::Number) {
        char text[32]; snprintf(text,sizeof(text),"%.0f",v->number);
        return text;
    }
    return std::string();
}

std::string Upper(std::string s) { for(size_t i=0;i<s.size();++i) s[i]=char(toupper((unsigned char)s[i])); return s; }
std::string Lower(std::string s) { for(size_t i=0;i<s.size();++i) s[i]=char(tolower((unsigned char)s[i])); return s; }

void AddUpdate(const web::Value& u, const std::string& mediaId, std::vector<Offer>& out) {
    if(u.type!=web::Value::Object) return;
    Offer o;
    o.id=AsText(u.Get("TitleUpdateID"));
    if(o.id.empty()) return;
    std::string media=mediaId.empty() ? AsText(u.Get("MediaID")) : mediaId;
    o.mediaId=Upper(media);
    o.name=AsText(u.Get("Name"));
    o.hash=Lower(AsText(u.Get("hash")));
    o.uploaded=AsText(u.Get("UploadDate"));
    o.baseVersion=AsText(u.Get("BaseVersion"));
    o.version=atoi(AsText(u.Get("Version")).c_str());
    o.size=strtoull(AsText(u.Get("Size")).c_str(),NULL,10);
    if(o.name.empty()) o.name="Title Update "+std::to_string(o.version);
    out.push_back(o);
}

// The value of a header (name in lower case) in a block of them, or "".
std::string HeaderValue(const std::string& headers, const char* name) {
    std::string lower=Lower(headers), key=std::string("\n")+name+":";
    size_t at=("\n"+lower).find(key);
    if(at==std::string::npos) return std::string();
    at+=key.size()-1;  // In headers (without the added \n).
    size_t end=headers.find('\n',at);
    std::string value=headers.substr(at,end==std::string::npos ? std::string::npos : end-at);
    while(!value.empty() && (value[0]==' ' || value[0]=='\t')) value.erase(0,1);
    while(!value.empty() && (value.back()=='\r' || value.back()==' ')) value.pop_back();
    return value;
}

// filename="..." (or filename*=UTF-8''...) of a Content-Disposition, without folders.
std::string DispositionName(const std::string& value) {
    std::string lower=Lower(value), name;
    size_t at=lower.find("filename*=");
    if(at!=std::string::npos) {
        name=value.substr(at+10);
        size_t quote=name.find("''");
        if(quote!=std::string::npos) name=name.substr(quote+2);
        size_t end=name.find(';');
        if(end!=std::string::npos) name=name.substr(0,end);
        std::string decoded;
        for(size_t i=0;i<name.size();++i) {
            if(name[i]=='%' && i+2<name.size() && isxdigit((unsigned char)name[i+1]) && isxdigit((unsigned char)name[i+2])) {
                decoded+=char(strtol(name.substr(i+1,2).c_str(),NULL,16)); i+=2;
            } else decoded+=name[i];
        }
        name=decoded;
    } else if((at=lower.find("filename="))!=std::string::npos) {
        name=value.substr(at+9);
        if(!name.empty() && name[0]=='"') {
            size_t end=name.find('"',1);
            name=name.substr(1,end==std::string::npos ? std::string::npos : end-1);
        } else {
            size_t end=name.find(';');
            if(end!=std::string::npos) name=name.substr(0,end);
        }
    }
    while(!name.empty() && (name.back()==' ' || name.back()=='\r')) name.pop_back();
    size_t slash=name.find_last_of("/\\");
    if(slash!=std::string::npos) name=name.substr(slash+1);
    if(name=="." || name=="..") name.clear();
    return name;
}

const char UserAgent[] = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) NetDisc360";

// Plain HTTP over a socket, for http:// addresses (the tests' fake XboxUnity).
bool Wait(SOCKET s, bool writing, time_t deadline) {
    while(time(NULL)<deadline) {
        fd_set set; FD_ZERO(&set); FD_SET(s,&set);
        timeval interval={0,200000};
        int rc=select(int(s)+1,writing ? NULL : &set,writing ? &set : NULL,NULL,&interval);
        if(rc>0) return true;
        if(rc==SOCKET_ERROR) return false;
    }
    return false;
}

bool PlainGet(const std::string& url, std::string& headers, std::string& body, size_t limit, std::string& error) {
    std::string rest=url.substr(7);
    size_t slash=rest.find('/');
    std::string host=rest.substr(0,slash), path=slash==std::string::npos ? "/" : rest.substr(slash);
    std::string port="80";
    size_t colon=host.find(':');
    if(colon!=std::string::npos) { port=host.substr(colon+1); host=host.substr(0,colon); }
    addrinfo hints; memset(&hints,0,sizeof(hints));
    hints.ai_family=AF_INET; hints.ai_socktype=SOCK_STREAM;
    addrinfo* found=NULL;
    if(getaddrinfo(host.c_str(),port.c_str(),&hints,&found) || !found) { error="Can't find "+host+". Is this computer online?"; return false; }
    sockaddr_in to; memcpy(&to,found->ai_addr,sizeof(to));
    freeaddrinfo(found);
    SOCKET s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if(s==INVALID_SOCKET) { error="No socket."; return false; }
    u_long nonblocking=1;
    ioctlsocket(s,FIONBIO,&nonblocking);
    time_t deadline=time(NULL)+120;
    bool ok=!connect(s,reinterpret_cast<sockaddr*>(&to),sizeof(to)) || WSAGetLastError()==WSAEWOULDBLOCK;
    if(ok) {
        int failed=0;
        int length=sizeof(failed);
        ok=Wait(s,true,deadline) && !getsockopt(s,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&failed),&length) && !failed;
    }
    if(!ok) { closesocket(s); error="Can't reach "+host+". Is this computer online?"; return false; }
    std::string request="GET "+path+" HTTP/1.1\r\nHost: "+host+(port=="80" ? "" : ":"+port)+"\r\nUser-Agent: "+UserAgent+
        "\r\nAccept: application/json, text/javascript, */*; q=0.01\r\nX-Requested-With: XMLHttpRequest\r\nReferer: http://"+host+"/\r\nConnection: close\r\n\r\n";
    for(size_t sent=0;ok && sent<request.size();) {
        if(!Wait(s,true,deadline)) { ok=false; break; }
        int n=send(s,request.data()+sent,int(request.size()-sent),0);
        if(n>0) sent+=size_t(n);
        else if(n==0 || WSAGetLastError()!=WSAEWOULDBLOCK) ok=false;
    }
    std::string response;
    while(ok) {
        if(!Wait(s,false,deadline)) { ok=false; break; }
        char part[65536];
        int n=recv(s,part,sizeof(part),0);
        if(n==0) break;
        if(n<0) { if(WSAGetLastError()!=WSAEWOULDBLOCK) ok=false; continue; }
        response.append(part,size_t(n));
        if(response.size()>limit+65536) { error="The answer is larger than expected."; closesocket(s); return false; }
    }
    closesocket(s);
    if(!ok) { error="The answer from "+host+" stopped part way; try again."; return false; }
    size_t end=response.find("\r\n\r\n");
    if(end==std::string::npos) { error=host+" sent an answer this doesn't understand."; return false; }
    headers=response.substr(0,end+2);
    body=response.substr(end+4);
    if(Lower(HeaderValue(headers,"transfer-encoding")).find("chunked")!=std::string::npos) {
        std::string joined;
        for(size_t at=0;at<body.size();) {
            size_t line=body.find("\r\n",at);
            if(line==std::string::npos) break;
            size_t n=size_t(strtoul(body.substr(at,line-at).c_str(),NULL,16));
            if(!n) break;
            joined+=body.substr(line+2,n);
            at=line+2+n+2;
        }
        body.swap(joined);
    }
    if(body.size()>limit) { error="The answer is larger than expected."; return false; }
    return true;
}

int Status(const std::string& headers) {
    size_t space=headers.find(' ');
    return space==std::string::npos ? 0 : atoi(headers.c_str()+space+1);
}

#ifdef _WIN32
std::wstring Wide(const std::string& text) {
    int n=MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),NULL,0);
    std::wstring out(size_t(n),L'\0');
    if(n) MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),&out[0],n);
    return out;
}
std::string Narrow(const std::wstring& text) {
    int n=WideCharToMultiByte(CP_UTF8,0,text.data(),int(text.size()),NULL,0,NULL,NULL);
    std::string out(size_t(n),'\0');
    if(n) WideCharToMultiByte(CP_UTF8,0,text.data(),int(text.size()),&out[0],n,NULL,NULL);
    return out;
}
struct Handle {
    HINTERNET h;
    explicit Handle(HINTERNET handle) : h(handle) {}
    ~Handle() { if(h) WinHttpCloseHandle(h); }
};

bool SecureGet(const std::string& url, std::string& headers, std::string& body, size_t limit, std::string& error) {
    std::wstring wide=Wide(url);
    wchar_t host[256], path[2048], extra[2048];
    URL_COMPONENTS parts;
    memset(&parts,0,sizeof(parts));
    parts.dwStructSize=sizeof(parts);
    parts.lpszHostName=host; parts.dwHostNameLength=ARRAYSIZE(host);
    parts.lpszUrlPath=path; parts.dwUrlPathLength=ARRAYSIZE(path);
    parts.lpszExtraInfo=extra; parts.dwExtraInfoLength=ARRAYSIZE(extra);
    if(!WinHttpCrackUrl(wide.c_str(),0,0,&parts)) { error="Not a web address: "+url; return false; }
    Handle session(WinHttpOpen(Wide(UserAgent).c_str(),WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0));
    if(!session.h) session.h=WinHttpOpen(Wide(UserAgent).c_str(),WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if(!session.h) { error="Windows' HTTP client didn't start."; return false; }
    WinHttpSetTimeouts(session.h,15000,15000,30000,120000);
    Handle connection(WinHttpConnect(session.h,host,parts.nPort,0));
    std::wstring target=std::wstring(path)+extra;
    Handle request(connection.h ? WinHttpOpenRequest(connection.h,L"GET",target.c_str(),NULL,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                     parts.nScheme==INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : NULL);
    if(request.h) {
        std::wstring extraHeaders=L"Accept: application/json, text/javascript, */*; q=0.01\r\nX-Requested-With: XMLHttpRequest\r\nReferer: https://"+std::wstring(host)+L"/";
        WinHttpAddRequestHeaders(request.h,extraHeaders.c_str(),(DWORD)-1L,WINHTTP_ADDREQ_FLAG_ADD);
    }
    if(!request.h || !WinHttpSendRequest(request.h,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0) || !WinHttpReceiveResponse(request.h,NULL)) {
        char text[160];
        _snprintf_s(text,sizeof(text),_TRUNCATE,"Can't reach %s (error %lu). Is this computer online?",Narrow(host).c_str(),GetLastError());
        error=text;
        return false;
    }
    DWORD size=0;
    WinHttpQueryHeaders(request.h,WINHTTP_QUERY_RAW_HEADERS_CRLF,WINHTTP_HEADER_NAME_BY_INDEX,NULL,&size,WINHTTP_NO_HEADER_INDEX);
    if(size) {
        std::wstring raw(size/sizeof(wchar_t)+1,L'\0');
        if(WinHttpQueryHeaders(request.h,WINHTTP_QUERY_RAW_HEADERS_CRLF,WINHTTP_HEADER_NAME_BY_INDEX,&raw[0],&size,WINHTTP_NO_HEADER_INDEX))
            headers=Narrow(raw.substr(0,size/sizeof(wchar_t)));
    }
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
        if(body.size()>limit) { error="The answer is larger than expected."; return false; }
    }
    return true;
}
#else
// curl, which the Docker image has, run without a shell.
bool SecureGet(const std::string& url, std::string& headers, std::string& body, size_t limit, std::string& error) {
    char headerPath[]="/tmp/netdisc-tu-h-XXXXXX", bodyPath[]="/tmp/netdisc-tu-b-XXXXXX";
    int h=mkstemp(headerPath), b=mkstemp(bodyPath);
    if(h<0 || b<0) { if(h>=0) { close(h); unlink(headerPath); } if(b>=0) { close(b); unlink(bodyPath); } error="No room for a download in /tmp."; return false; }
    close(h); close(b);
    std::string most=std::to_string(limit);
    const char* argv[]={"curl","-sS","-L","--max-time","600","--max-filesize",most.c_str(),"-A",UserAgent,
        "-H","Accept: application/json, text/javascript, */*; q=0.01","-H","X-Requested-With: XMLHttpRequest",
        "-e","https://xboxunity.net/","-D",headerPath,"-o",bodyPath,"--",url.c_str(),NULL};
    pid_t pid=0;
    int status=-1;
    bool started=!posix_spawnp(&pid,"curl",NULL,NULL,const_cast<char* const*>(argv),environ);
    if(started) waitpid(pid,&status,0);
    std::string raw;
    FILE* f=fopen(headerPath,"rb");
    if(f) { char part[4096]; size_t n; while((n=fread(part,1,sizeof(part),f))>0) raw.append(part,n); fclose(f); }
    f=fopen(bodyPath,"rb");
    if(f) { char part[65536]; size_t n; while((n=fread(part,1,sizeof(part),f))>0 && body.size()<=limit) body.append(part,n); fclose(f); }
    unlink(headerPath); unlink(bodyPath);
    if(!started) { error="This needs curl to reach XboxUnity over HTTPS, and it isn't installed."; return false; }
    if(!WIFEXITED(status) || WEXITSTATUS(status)) { error="Couldn't reach XboxUnity (curl exited with "+std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1)+"). Is this computer online?"; return false; }
    if(body.size()>limit) { error="The answer is larger than expected."; return false; }
    // After redirects, the last block of headers is the answer's.
    size_t last=raw.rfind("\r\n\r\nHTTP/");
    headers=last==std::string::npos ? raw : raw.substr(last+4);
    return true;
}
#endif

// SHA-1 of any length.
struct Sha1 {
    DWORD h[5]={0x67452301,0xEFCDAB89,0x98BADCFE,0x10325476,0xC3D2E1F0};
    unsigned char block[64]; size_t used=0; U64 length=0;
    static DWORD Rotate(DWORD v, int n) { return (v<<n)|(v>>(32-n)); }
    void Compress() {
        DWORD w[80];
        for(int i=0;i<16;++i) w[i]=Big32(block+i*4);
        for(int i=16;i<80;++i) w[i]=Rotate(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
        DWORD a=h[0],b=h[1],c=h[2],d=h[3],e=h[4];
        for(int i=0;i<80;++i) {
            DWORD f,k;
            if(i<20) { f=(b&c)|(~b&d); k=0x5A827999; }
            else if(i<40) { f=b^c^d; k=0x6ED9EBA1; }
            else if(i<60) { f=(b&c)|(b&d)|(c&d); k=0x8F1BBCDC; }
            else { f=b^c^d; k=0xCA62C1D6; }
            DWORD t=Rotate(a,5)+f+e+k+w[i];
            e=d; d=c; c=Rotate(b,30); b=a; a=t;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e;
    }
    void Add(const unsigned char* p, size_t n) {
        length+=n;
        while(n) {
            size_t take=std::min(n,64-used);
            memcpy(block+used,p,take); used+=take; p+=take; n-=take;
            if(used==64) { Compress(); used=0; }
        }
    }
    std::string Hex() {
        U64 bits=length*8;
        unsigned char pad=0x80;
        Add(&pad,1);
        unsigned char zero=0;
        while(used!=56) Add(&zero,1);
        for(int i=7;i>=0;--i) { unsigned char c=(unsigned char)(bits>>(i*8)); Add(&c,1); }
        char text[41];
        for(int i=0;i<5;++i) snprintf(text+i*8,9,"%08x",(unsigned)h[i]);
        return text;
    }
};
}  // namespace

bool ParseInfo(const std::string& json, std::vector<Offer>& out, std::string& error) {
    out.clear();
    web::Value v;
    if(!web::Parse(json,v) || v.type!=web::Value::Object) {
        // XboxUnity answers a Title ID it has nothing for with an empty body or "[]".
        std::string trimmed=json;
        trimmed.erase(std::remove_if(trimmed.begin(),trimmed.end(),[](char c) { return isspace((unsigned char)c)!=0; }),trimmed.end());
        if(trimmed.empty() || trimmed=="[]" || trimmed=="null") return true;
        error="XboxUnity sent an answer this doesn't understand.";
        return false;
    }
    const web::Value* media=v.Get("MediaIDS");
    if(media && media->type==web::Value::Array)
        for(size_t i=0;i<media->items.size();++i) {
            const web::Value& m=media->items[i];
            const web::Value* updates=m.Get("Updates");
            std::string id=AsText(m.Get("MediaID"));
            if(updates && updates->type==web::Value::Array && !id.empty())
                for(size_t k=0;k<updates->items.size();++k) AddUpdate(updates->items[k],id,out);
        }
    const web::Value* updates=v.Get("Updates");
    if(updates && updates->type==web::Value::Array)
        for(size_t k=0;k<updates->items.size();++k) AddUpdate(updates->items[k],std::string(),out);
    return true;
}

std::vector<Offer> ForMedia(const std::vector<Offer>& all, DWORD mediaId) {
    std::vector<Offer> out;
    std::string media=Hex8(mediaId);
    for(size_t i=0;i<all.size();++i) if(all[i].mediaId==media) out.push_back(all[i]);
    std::stable_sort(out.begin(),out.end(),[](const Offer& a, const Offer& b) { return a.version>b.version; });
    return out;
}

std::string InfoUrl(const std::string& base, DWORD titleId) { return base+"/TitleUpdateInfo.php?titleid="+Hex8(titleId); }
std::string FileUrl(const std::string& base, const std::string& id) {
    std::string clean;
    for(size_t i=0;i<id.size();++i) if(isalnum((unsigned char)id[i])) clean+=id[i];
    return base+"/TitleUpdate.php?tuid="+clean;
}

bool Get(const std::string& url, std::string& body, size_t limit, std::string& fileName, std::string& error) {
    body.clear(); fileName.clear();
    std::string headers, at=url;
    for(int hop=0;;++hop) {
        bool ok;
        if(!Lower(at).compare(0,7,"http://")) ok=PlainGet(at,headers,body,limit,error);
        else if(!Lower(at).compare(0,8,"https://")) ok=SecureGet(at,headers,body,limit,error);
        else { error="Not a web address: "+at; return false; }
        if(!ok) return false;
        int status=Status(headers);
        std::string location=HeaderValue(headers,"location");
        if(status/100==3 && !location.empty() && hop<4) {
            if(location[0]=='/') {
                size_t host=at.find("//");
                size_t path=at.find('/',host==std::string::npos ? 0 : host+2);
                location=at.substr(0,path)+location;
            }
            at=location; body.clear();
            continue;
        }
        if(status && status!=200) { error="XboxUnity answered with HTTP "+std::to_string(status)+"."; return false; }
        break;
    }
    fileName=DispositionName(HeaderValue(headers,"content-disposition"));
    return true;
}

std::string FatxName(const std::string& name) {
    std::string out;
    for(size_t i=0;i<name.size() && out.size()<42;++i) {
        unsigned char c=(unsigned char)name[i];
        if(c<32 || c>126 || strchr("\"*+,/:;<=>?\\|",c)) continue;
        out+=char(c);
    }
    return out;
}

std::string ContentFolder(DWORD titleId) { return "/Hdd1/Content/0000000000000000/"+Hex8(titleId)+"/000B0000"; }

std::string ConsolePath(DWORD titleId, const std::string& name) {
    std::string file=FatxName(name);
    // Aurora's rule: an upper-case TU_ name is a cache update, any other a content package.
    if(!file.compare(0,3,"TU_")) return std::string(CacheFolder)+"/"+file;
    return ContentFolder(titleId)+"/"+file;
}

bool ReadPackageHeader(const unsigned char* data, size_t size, PackageHeader& out) {
    if(size<0x412) return false;
    memcpy(out.magic,data,4); out.magic[4]=0;
    if(strcmp(out.magic,"CON ") && strcmp(out.magic,"LIVE") && strcmp(out.magic,"PIRS")) return false;
    out.contentType=Big32(data+0x344);
    out.mediaId=Big32(data+0x354);
    out.version=Big32(data+0x358);
    out.baseVersion=Big32(data+0x35C);
    out.titleId=Big32(data+0x360);
    out.displayName.clear();
    // The first display name: UTF-16 big-endian, 0x80 bytes from 0x411.
    for(size_t i=0x411;i+1<size && i<0x411+0x80;i+=2) {
        unsigned c=(unsigned(data[i])<<8)|data[i+1];
        if(!c) break;
        if(c<0x80) out.displayName+=char(c);
        else if(c<0x800) { out.displayName+=char(0xC0|(c>>6)); out.displayName+=char(0x80|(c&0x3F)); }
        else { out.displayName+=char(0xE0|(c>>12)); out.displayName+=char(0x80|((c>>6)&0x3F)); out.displayName+=char(0x80|(c&0x3F)); }
    }
    return true;
}

std::string Sha1Hex(const void* data, size_t size) {
    Sha1 s;
    s.Add(static_cast<const unsigned char*>(data),size);
    return s.Hex();
}
}
