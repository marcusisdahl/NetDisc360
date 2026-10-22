// SPDX-License-Identifier: MIT
// The dashboard's web server (see web.h): HTTP/1.1 with keep-alive, one
// thread per connection, the page from web_page.h, and the password.
#include "web.h"
#include "web_page.h"
#include "../shared/isvr.h"
#include <map>
#include <atomic>
#include <ctype.h>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <time.h>
#ifdef _WIN32
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
typedef int AddressLength;
#else
typedef socklen_t AddressLength;
#endif

namespace web {
namespace {
// MaxBody is for a backup sent to /api/restore; everything else is small.
// A request has RequestSeconds to arrive whole once it has started, and a
// connection waits IdleSeconds for one at most.
enum { MaxHeader = 16 << 10, MaxBody = 256 << 10, MaxSmallBody = 16 << 10, MaxConnections = 64, MaxConnectionsFrom = 16,
       IdleSeconds = 15, RequestSeconds = 30, MaxDepth = 32, SessionDays = 30, HashRounds = 20000 };
const char Cookie[] = "netdisc_session";

// JSON ------------------------------------------------------------------------

struct Parser {
    const char* p;
    const char* end;
    void Space() { while(p<end && (*p==' ' || *p=='\t' || *p=='\r' || *p=='\n')) ++p; }
    bool Literal(const char* word) {
        size_t n=strlen(word);
        if(size_t(end-p)<n || memcmp(p,word,n)) return false;
        p+=n; return true;
    }
    static void Utf8(unsigned c, std::string& out) {
        if(c<0x80) out+=char(c);
        else if(c<0x800) { out+=char(0xC0|(c>>6)); out+=char(0x80|(c&0x3F)); }
        else if(c<0x10000) { out+=char(0xE0|(c>>12)); out+=char(0x80|((c>>6)&0x3F)); out+=char(0x80|(c&0x3F)); }
        else { out+=char(0xF0|(c>>18)); out+=char(0x80|((c>>12)&0x3F)); out+=char(0x80|((c>>6)&0x3F)); out+=char(0x80|(c&0x3F)); }
    }
    bool Hex4(unsigned& c) {
        if(end-p<4) return false;
        c=0;
        for(int i=0;i<4;++i,++p) {
            char h=*p; c<<=4;
            if(h>='0' && h<='9') c|=unsigned(h-'0');
            else if(h>='a' && h<='f') c|=unsigned(h-'a'+10);
            else if(h>='A' && h<='F') c|=unsigned(h-'A'+10);
            else return false;
        }
        return true;
    }
    bool Text(std::string& out) {
        if(p>=end || *p!='"') return false;
        ++p;
        while(p<end && *p!='"') {
            if(*p!='\\') { if((unsigned char)*p<0x20) return false; out+=*p++; continue; }
            if(++p>=end) return false;
            char e=*p++;
            switch(e) {
            case '"': case '\\': case '/': out+=e; break;
            case 'b': out+='\b'; break;
            case 'f': out+='\f'; break;
            case 'n': out+='\n'; break;
            case 'r': out+='\r'; break;
            case 't': out+='\t'; break;
            case 'u': {
                unsigned c;
                if(!Hex4(c)) return false;
                if(c>=0xD800 && c<0xDC00 && end-p>=6 && p[0]=='\\' && p[1]=='u') {
                    p+=2; unsigned low;
                    if(!Hex4(low) || low<0xDC00 || low>=0xE000) return false;
                    c=0x10000+((c-0xD800)<<10)+(low-0xDC00);
                }
                Utf8(c,out);
                break;
            }
            default: return false;
            }
        }
        if(p>=end) return false;
        ++p;
        return true;
    }
    bool Read(Value& v, int depth) {
        if(depth>MaxDepth) return false;
        Space();
        if(p>=end) return false;
        if(*p=='{') {
            v.type=Value::Object; ++p; Space();
            if(p<end && *p=='}') { ++p; return true; }
            for(;;) {
                Space();
                std::pair<std::string,Value> field;
                if(!Text(field.first)) return false;
                Space();
                if(p>=end || *p++!=':' || !Read(field.second,depth+1)) return false;
                v.fields.push_back(std::pair<std::string,Value>());
                v.fields.back().first.swap(field.first);
                std::swap(v.fields.back().second,field.second);
                Space();
                if(p<end && *p==',') { ++p; continue; }
                if(p<end && *p=='}') { ++p; return true; }
                return false;
            }
        }
        if(*p=='[') {
            v.type=Value::Array; ++p; Space();
            if(p<end && *p==']') { ++p; return true; }
            for(;;) {
                v.items.push_back(Value());
                if(!Read(v.items.back(),depth+1)) return false;
                Space();
                if(p<end && *p==',') { ++p; continue; }
                if(p<end && *p==']') { ++p; return true; }
                return false;
            }
        }
        if(*p=='"') { v.type=Value::String; return Text(v.text); }
        if(Literal("true")) { v.type=Value::Bool; v.boolean=true; return true; }
        if(Literal("false")) { v.type=Value::Bool; return true; }
        if(Literal("null")) return true;
        std::string number;
        while(p<end && strchr("+-0123456789.eE",*p)) number+=*p++;
        if(number.empty()) return false;
        char* stop;
        v.type=Value::Number; v.number=strtod(number.c_str(),&stop);
        return !*stop;
    }
};

// SHA-256 and HMAC, for the password ----------------------------------------------

struct Sha256 {
    unsigned h[8];
    unsigned char block[64];
    U64 length;
    size_t used;
    Sha256() : length(0), used(0) {
        static const unsigned start[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
        memcpy(h,start,sizeof(h));
    }
    static unsigned Rotate(unsigned x, int n) { return (x>>n)|(x<<(32-n)); }
    void Compress() {
        static const unsigned k[64]={
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
            0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
            0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
            0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        unsigned w[64];
        for(int i=0;i<16;++i) w[i]=(unsigned(block[i*4])<<24)|(unsigned(block[i*4+1])<<16)|(unsigned(block[i*4+2])<<8)|block[i*4+3];
        for(int i=16;i<64;++i) {
            unsigned s0=Rotate(w[i-15],7)^Rotate(w[i-15],18)^(w[i-15]>>3), s1=Rotate(w[i-2],17)^Rotate(w[i-2],19)^(w[i-2]>>10);
            w[i]=w[i-16]+s0+w[i-7]+s1;
        }
        unsigned a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],x=h[7];
        for(int i=0;i<64;++i) {
            unsigned t1=x+(Rotate(e,6)^Rotate(e,11)^Rotate(e,25))+((e&f)^(~e&g))+k[i]+w[i];
            unsigned t2=(Rotate(a,2)^Rotate(a,13)^Rotate(a,22))+((a&b)^(a&c)^(b&c));
            x=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=x;
    }
    void Add(const void* data, size_t count) {
        const unsigned char* p=static_cast<const unsigned char*>(data);
        length+=count;
        while(count--) { block[used++]=*p++; if(used==64) { Compress(); used=0; } }
    }
    std::string Done() {
        U64 bits=length*8;
        unsigned char pad=0x80;
        Add(&pad,1);
        pad=0;
        while(used!=56) Add(&pad,1);
        for(int i=7;i>=0;--i) { unsigned char byte=(unsigned char)(bits>>(i*8)); Add(&byte,1); }
        std::string out(32,'\0');
        for(int i=0;i<32;++i) out[i]=char(h[i/4]>>(24-8*(i%4)));
        return out;
    }
};

std::string Sha(const std::string& data) { Sha256 s; s.Add(data.data(),data.size()); return s.Done(); }

std::string Hmac(const std::string& key, const std::string& message) {
    std::string k=key.size()>64 ? Sha(key) : key;
    k.resize(64,'\0');
    std::string inner(64,'\0'),outer(64,'\0');
    for(int i=0;i<64;++i) { inner[i]=char(k[i]^0x36); outer[i]=char(k[i]^0x5c); }
    return Sha(outer+Sha(inner+message));
}

std::string Hex(const std::string& data) {
    static const char digits[]="0123456789abcdef";
    std::string out;
    for(size_t i=0;i<data.size();++i) { out+=digits[(unsigned char)data[i]>>4]; out+=digits[data[i]&15]; }
    return out;
}

// Bytes from the system's random source, for salts and the session key. A
// predictable one would let sessions be forged, so without one the server stops.
std::string Random(size_t count) {
    std::string out(count,'\0');
#ifdef _WIN32
    bool ok=BCRYPT_SUCCESS(BCryptGenRandom(NULL,reinterpret_cast<PUCHAR>(&out[0]),ULONG(count),BCRYPT_USE_SYSTEM_PREFERRED_RNG));
#else
    bool ok=false;
    FILE* f=fopen("/dev/urandom","rb");
    if(f) { ok=fread(&out[0],1,count,f)==count; fclose(f); }
#endif
    if(!ok) { fprintf(stderr,"The system's random numbers can't be read, so NetDisc360 can't keep the dashboard safe; it stops.\n"); abort(); }
    return out;
}

bool Same(const std::string& a, const std::string& b) {
    if(a.size()!=b.size()) return false;
    unsigned char diff=0;
    for(size_t i=0;i<a.size();++i) diff|=(unsigned char)(a[i]^b[i]);
    return !diff;
}

std::mutex PasswordLock, LoginLock;
std::string Hash;    // "salt:hash", under PasswordLock.
std::string Secret;  // Signs the sessions with Hash, under PasswordLock (see SetSessionSecret).

// What signs the sessions: the password's hash, signed with the server's
// secret when it has one, so a hash read from a backup can't make a session.
std::string SessionKey(const std::string& hash) {
    std::lock_guard<std::mutex> hold(PasswordLock);
    return Secret.empty() ? hash : Hmac(Secret,hash);
}

std::string Stretch(const std::string& salt, const std::string& password) {
    std::string h=Sha(salt+password);
    for(int i=0;i<HashRounds;++i) h=Sha(h+salt);
    return Hex(h);
}

bool CheckPassword(const std::string& password) {
    std::string hash=PasswordHash();
    size_t colon=hash.find(':');
    return colon!=std::string::npos && Same(Stretch(hash.substr(0,colon),password),hash.substr(colon+1));
}

// A session is its end (time()) and that signed with SessionKey, so it
// outlasts a restart and ends when the password changes.
std::string Session(const std::string& hash) {
    char end[24]; _snprintf_s(end,sizeof(end),_TRUNCATE,"%lld",(long long)time(NULL)+SessionDays*86400LL);
    return std::string(end)+"."+Hex(Hmac(SessionKey(hash),end));
}

bool ValidSession(const std::string& session) {
    std::string hash=PasswordHash();
    size_t dot=session.find('.');
    if(hash.empty() || dot==std::string::npos) return false;
    std::string end=session.substr(0,dot);
    long long now=(long long)time(NULL), until=atoll(end.c_str());
    return until>now && until<=now+SessionDays*86400LL+60 && Same(Hex(Hmac(SessionKey(hash),end)),session.substr(dot+1));
}

std::string SessionHeader(const std::string& value, long long seconds) {
    char age[24]; _snprintf_s(age,sizeof(age),_TRUNCATE,"%lld",seconds);
    return std::string("Set-Cookie: ")+Cookie+"="+value+"; Path=/; Max-Age="+age+"; HttpOnly; SameSite=Strict\r\n";
}

// HTTP ------------------------------------------------------------------------

std::string Lower(std::string text) {
    for(size_t i=0;i<text.size();++i) if(text[i]>='A' && text[i]<='Z') text[i]=char(text[i]-'A'+'a');
    return text;
}

bool EndsWith(const std::string& text, const char* end) {
    size_t n=strlen(end);
    return text.size()>=n && !text.compare(text.size()-n,n,end);
}

std::string Decode(const std::string& text) {
    std::string out;
    for(size_t i=0;i<text.size();++i) {
        if(text[i]=='+') out+=' ';
        else if(text[i]=='%' && i+2<text.size() && isxdigit((unsigned char)text[i+1]) && isxdigit((unsigned char)text[i+2])) {
            out+=char(strtol(text.substr(i+1,2).c_str(),NULL,16)); i+=2;
        } else out+=text[i];
    }
    return out;
}

// Without a password, only addresses and local names may reach the dashboard:
// a web page at a name its owner points at this PC ("DNS rebinding") would
// otherwise be able to read and change it.
bool LocalHost(std::string host) {
    host=Lower(host);
    if(!host.empty() && host[0]=='[') return host.find(']')!=std::string::npos;  // IPv6.
    size_t colon=host.find(':');
    if(colon!=std::string::npos) host.erase(colon);
    if(host.empty() || host.find('.')==std::string::npos) return true;  // "localhost", "nas", or none.
    if(host.find_first_not_of("0123456789.")==std::string::npos) return true;
    static const char* const local[]={".local",".lan",".home",".internal",".home.arpa",".localdomain"};
    for(size_t i=0;i<sizeof(local)/sizeof(local[0]);++i) if(EndsWith(host,local[i])) return true;
    return false;
}

struct Incoming {
    std::string method, target, version, body;
    std::vector<std::pair<std::string,std::string> > headers;
    std::string Header(const char* name) const {
        for(size_t i=0;i<headers.size();++i) if(headers[i].first==name) return headers[i].second;
        return std::string();
    }
};

const char* StatusText(int status) {
    switch(status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 431: return "Request Header Fields Too Large";
    default: return "Error";
    }
}

bool Loopback(DWORD address) { return (ntohl(address)>>24)==127; }
bool OnlyHere();

class Connection {
public:
    Connection(SOCKET socket, DWORD peer, Api api) : socket_(socket), peer_(peer), api_(api), started_(0) {}
    ~Connection() { closesocket(socket_); }
    void Run() {
        for(int served=0;served<1000;++served) {
            Incoming in;
            int status=ReadRequest(in);
            if(status<0) return;
            // Opened from another device before "this computer only" was chosen.
            if(OnlyHere() && !Loopback(peer_)) return;
            if(status) { Send(status,"application/json","{\"error\":\"bad request\"}","",true); return; }
            std::string connection=Lower(in.Header("connection"));
            bool close=connection=="close" || (in.version=="HTTP/1.0" && connection!="keep-alive");
            if(!Handle(in,close) || close) return;
        }
    }
private:
    bool Receive() {
        if(started_ && nia::Now()-started_>RequestSeconds) return false;  // Too slow to be a browser.
        char part[8192];
        int got=recv(socket_,part,sizeof(part),0);
        if(got<=0) return false;
        pending_.append(part,size_t(got));
        return true;
    }
    // 0 when a request was read, an HTTP status for a bad one, -1 when the connection ended.
    int ReadRequest(Incoming& in) {
        size_t end;
        started_=pending_.empty() ? 0 : nia::Now();
        while((end=pending_.find("\r\n\r\n"))==std::string::npos) {
            if(pending_.size()>MaxHeader) return 431;
            bool waiting=pending_.empty();
            if(!Receive()) return -1;
            if(waiting) started_=nia::Now();
        }
        std::string head=pending_.substr(0,end);
        pending_.erase(0,end+4);
        size_t line=head.find("\r\n");
        std::string first=head.substr(0,line);
        size_t a=first.find(' '), b=first.rfind(' ');
        if(a==std::string::npos || b==a) return 400;
        in.method=first.substr(0,a); in.target=first.substr(a+1,b-a-1); in.version=first.substr(b+1);
        while(line!=std::string::npos) {
            size_t start=line+2;
            line=head.find("\r\n",start);
            std::string h=head.substr(start,line==std::string::npos ? std::string::npos : line-start);
            size_t colon=h.find(':');
            if(colon==std::string::npos) continue;
            size_t value=h.find_first_not_of(" \t",colon+1);
            in.headers.push_back(std::make_pair(Lower(h.substr(0,colon)),value==std::string::npos ? std::string() : h.substr(value)));
        }
        if(!in.Header("transfer-encoding").empty()) return 400;
        long long length=atoll(in.Header("content-length").c_str());
        if(length<0) return 400;
        if(length>(in.target.compare(0,12,"/api/restore") ? MaxSmallBody : MaxBody)) return 413;
        while(pending_.size()<size_t(length)) if(!Receive()) return -1;
        in.body=pending_.substr(0,size_t(length));
        pending_.erase(0,size_t(length));
        return 0;
    }
    bool Send(int status, const char* type, const std::string& body, const std::string& extra, bool close) {
        char head[512];
        _snprintf_s(head,sizeof(head),_TRUNCATE,
                    "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nCache-Control: no-store\r\n"
                    "X-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n%s",
                    status,StatusText(status),type,unsigned(body.size()),close ? "Connection: close\r\n" : "");
        std::string out=head+extra+"\r\n"+body;
        const char* p=out.data();
        size_t left=out.size();
        while(left) {
            int sent=send(socket_,p,left>(1<<20) ? (1<<20) : int(left),0);
            if(sent<=0) return false;
            p+=sent; left-=size_t(sent);
        }
        return true;
    }
    bool Handle(const Incoming& in, bool close) {
        Request r;
        r.method=in.method;
        size_t question=in.target.find('?');
        r.path=in.target.substr(0,question);
        if(question!=std::string::npos) r.query=in.target.substr(question+1);
        r.body=in.body;
        bool locked=!PasswordHash().empty();
        if(!locked && !LocalHost(in.Header("host")))
            return Send(403,"text/plain","Open the dashboard by this computer's address (like http://192.168.1.10:8360), or set a password for it.","",close);
        if(r.method=="GET" && (r.path=="/" || r.path=="/index.html")) {
            std::string page(reinterpret_cast<const char*>(WebPage),sizeof(WebPage));
            return Send(200,"text/html; charset=utf-8",page,
                        "Content-Encoding: gzip\r\nX-Frame-Options: DENY\r\n"
                        "Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
                        "img-src 'self' data: https://raw.githubusercontent.com; connect-src 'self' https://raw.githubusercontent.com https://api.github.com; "
                        "base-uri 'none'; form-action 'none'; frame-ancestors 'none'\r\n",close);
        }
        if(r.path.compare(0,5,"/api/")) return Send(404,"text/plain","Not here.","",close);
        if(r.method!="GET" && r.method!="POST") return Send(405,"application/json","{\"error\":\"GET or POST\"}","",close);
        // Only the page sends this header; another site's page can't without
        // this server allowing it (which it never does). GETs need it too, as
        // another site could have one fetched as an image (say a folder
        // listing of \\its-server\share, which hands Windows' login to it);
        // only a backup's download is a plain link. A browser also says where
        // a request comes from.
        std::string origin=in.Header("origin"), site=in.Header("sec-fetch-site");
        bool download=r.method=="GET" && r.path=="/api/backup";
        if((in.Header("x-netdisc").empty() && !download) || (!origin.empty() && !EndsWith(origin,("://"+in.Header("host")).c_str())) ||
           site=="cross-site" || site=="same-site")
            return Send(403,"application/json","{\"error\":\"Only the dashboard page can do that.\"}","",close);
        std::string session;
        std::string cookies=in.Header("cookie");
        size_t at=cookies.find(std::string(Cookie)+"=");
        if(at!=std::string::npos) {
            at+=strlen(Cookie)+1;
            session=cookies.substr(at,cookies.find(';',at)==std::string::npos ? std::string::npos : cookies.find(';',at)-at);
        }
        bool in_=!locked || ValidSession(session);
        if(r.path=="/api/auth") {
            Json j; j.Open('{').Set("password",locked).Set("loggedIn",in_).Close('}');
            return Send(200,"application/json",j.Text(),"",close);
        }
        if(r.path=="/api/login" && r.method=="POST") {
            Value v; const Value* password;
            std::string given=Parse(r.body,v) && (password=v.Get("password")) && password->type==Value::String ? password->text : "";
            std::lock_guard<std::mutex> hold(LoginLock);  // One try at a time,
            if(!locked || CheckPassword(given)) return Send(200,"application/json","{\"ok\":true}",locked ? SessionHeader(Session(PasswordHash()),SessionDays*86400LL) : "",close);
            Sleep(1000);  // and a second each, so guessing is slow.
            return Send(401,"application/json","{\"error\":\"That isn't the password.\"}","",close);
        }
        if(r.path=="/api/logout" && r.method=="POST") return Send(200,"application/json","{\"ok\":true}",SessionHeader("",0),close);
        if(!in_) return Send(401,"application/json","{\"error\":\"login\"}","",close);
        std::string before=PasswordHash();
        Reply reply;
        api_(r,reply);
        // A new password ends every session; this one goes on with a new cookie.
        std::string after=PasswordHash(), extra;
        if(after!=before && !after.empty()) extra=SessionHeader(Session(after),SessionDays*86400LL);
        if(!reply.download.empty()) extra+="Content-Disposition: attachment; filename=\""+reply.download+"\"\r\n";
        return Send(reply.status,reply.type.c_str(),reply.body,extra,close);
    }
    SOCKET socket_;
    DWORD peer_;
    Api api_;
    std::string pending_;
    double started_;  // When the request being read started; 0 before its first byte.
};

std::mutex ListenLock;
SOCKET Listener=INVALID_SOCKET;
std::string ListenAddress;
int ListenPort;
std::atomic<int> Active(0);
std::mutex OpenLock;
std::map<DWORD,int> OpenFrom;  // Connections by address, under OpenLock.

bool OnlyHere() {
    std::lock_guard<std::mutex> hold(ListenLock);
    return ListenAddress=="127.0.0.1";
}

void Serve(SOCKET listener, Api api) {
    for(;;) {
        sockaddr_in from; AddressLength size=sizeof(from);
        SOCKET client=accept(listener,reinterpret_cast<sockaddr*>(&from),&size);
        if(client==INVALID_SOCKET) {
            { std::lock_guard<std::mutex> hold(ListenLock); if(Listener!=listener) return; }
            Sleep(100);
            continue;
        }
        DWORD peer=from.sin_addr.s_addr;
        {
            std::lock_guard<std::mutex> hold(OpenLock);
            if(Active>=MaxConnections || OpenFrom[peer]>=MaxConnectionsFrom) {
                if(!OpenFrom[peer]) OpenFrom.erase(peer);
                closesocket(client);
                continue;
            }
            ++OpenFrom[peer];
        }
#ifdef _WIN32
        DWORD idle=IdleSeconds*1000;
#else
        struct timeval idle; idle.tv_sec=IdleSeconds; idle.tv_usec=0;
#endif
        setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&idle),sizeof(idle));
        setsockopt(client,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&idle),sizeof(idle));
        ++Active;
        std::thread([client,peer,api]() {
            Connection(client,peer,api).Run();
            --Active;
            std::lock_guard<std::mutex> hold(OpenLock);
            if(!--OpenFrom[peer]) OpenFrom.erase(peer);
        }).detach();
    }
}

SOCKET Listen(const std::string& address, int port, std::string& error) {
    SOCKET s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
#ifndef _WIN32
    int on=1;
    setsockopt(s,SOL_SOCKET,SO_REUSEADDR,reinterpret_cast<const char*>(&on),sizeof(on));
#endif
    sockaddr_in at; memset(&at,0,sizeof(at)); at.sin_family=AF_INET; at.sin_port=htons(u_short(port));
    at.sin_addr.s_addr=inet_addr(address.c_str());
    if(s==INVALID_SOCKET || bind(s,reinterpret_cast<sockaddr*>(&at),sizeof(at)) || listen(s,32)) {
        char text[160];
        _snprintf_s(text,sizeof(text),_TRUNCATE,"can't listen on %s:%d (error %d); is another copy running?",address.c_str(),port,WSAGetLastError());
        error=text;
        if(s!=INVALID_SOCKET) closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}
}  // namespace

std::string Quote(const std::string& text) {
    std::string out="\"";
    for(size_t i=0;i<text.size();++i) {
        unsigned char c=(unsigned char)text[i];
        if(c=='"' || c=='\\') { out+='\\'; out+=char(c); }
        else if(c=='\n') out+="\\n";
        else if(c=='\r') out+="\\r";
        else if(c=='\t') out+="\\t";
        else if(c<0x20 || c=='<' || c=='>' || c=='&') { char e[8]; _snprintf_s(e,sizeof(e),_TRUNCATE,"\\u%04x",c); out+=e; }
        else out+=char(c);
    }
    return out+"\"";
}

Json& Json::Number(double value) {
    Comma();
    char text[40];
    if(value==(double)(long long)value && value<9e15 && value>-9e15) _snprintf_s(text,sizeof(text),_TRUNCATE,"%lld",(long long)value);
    else _snprintf_s(text,sizeof(text),_TRUNCATE,"%.6g",value);
    text_+=text;
    return *this;
}

Json& Json::Int(long long value) {
    Comma();
    char text[24]; _snprintf_s(text,sizeof(text),_TRUNCATE,"%lld",value);
    text_+=text;
    return *this;
}

const Value* Value::Get(const char* key) const {
    for(size_t i=0;i<fields.size();++i) if(fields[i].first==key) return &fields[i].second;
    return NULL;
}

bool Parse(const std::string& text, Value& out) {
    Parser p = {text.data(), text.data()+text.size()};
    out=Value();
    if(!p.Read(out,0)) return false;
    p.Space();
    return p.p==p.end;
}

std::string Request::Param(const char* name) const {
    std::string key=std::string(name)+"=";
    for(size_t at=0;at<=query.size();) {
        size_t end=query.find('&',at);
        if(end==std::string::npos) end=query.size();
        if(!query.compare(at,key.size(),key)) return Decode(query.substr(at+key.size(),end-at-key.size()));
        at=end+1;
    }
    return std::string();
}

void Reply::Error(int code, const std::string& message) {
    status=code;
    type="application/json";
    Json j; j.Open('{').Set("error",message).Close('}');
    body=j.Text();
}

bool Start(const std::string& address, int port, Api api, std::string& error) {
    std::lock_guard<std::mutex> hold(ListenLock);
    if(Listener!=INVALID_SOCKET && address==ListenAddress && port==ListenPort) return true;
    SOCKET old=Listener;
    if(old!=INVALID_SOCKET) {
        // The same port on another address: this one has to go first.
        Listener=INVALID_SOCKET;
        shutdown(old,2);
        closesocket(old);
    }
    SOCKET s=Listen(address,port,error);
    if(s==INVALID_SOCKET) {
        std::string ignored;
        if(old!=INVALID_SOCKET) s=Listen(ListenAddress,ListenPort,ignored);
        if(s!=INVALID_SOCKET) { Listener=s; std::thread(Serve,s,api).detach(); }
        return false;
    }
    Listener=s; ListenAddress=address; ListenPort=port;
    std::thread(Serve,s,api).detach();
    return true;
}

std::string HashPassword(const std::string& password) {
    if(password.empty()) return std::string();
    std::string salt=Hex(Random(16));
    return salt+":"+Stretch(salt,password);
}

void SetPasswordHash(const std::string& hash) {
    std::lock_guard<std::mutex> hold(PasswordLock);
    Hash=hash;
}

std::string PasswordHash() {
    std::lock_guard<std::mutex> hold(PasswordLock);
    return Hash;
}

void SetSessionSecret(const std::string& secret) {
    std::lock_guard<std::mutex> hold(PasswordLock);
    Secret=secret;
}

std::string NewSecret() { return Hex(Random(32)); }

std::string Sha256Hex(const std::string& data) { return Hex(Sha(data)); }
}
