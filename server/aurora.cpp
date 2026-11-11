// SPDX-License-Identifier: MIT
#include "aurora.h"
#include "web.h"
#include "../shared/isvr.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace nia {
const char CleanupScriptPath[] = "/User/Scripts/Utility/NetDisc360 Cleanup/Main.lua";
const char OnlineScriptPath[] = "/User/Scripts/Content/Filters/NetDiscOnline.lua";
const char OnlineMarker[] = "online.txt", ShownMarker[] = "shown.txt";
const char MoveMarker[] = "move.txt", MovedMarker[] = "moved.txt";

namespace {
enum { ResponseMost = 1 << 20, ImageMost = 32 << 20 };

bool Wait(SOCKET s, bool writing, double deadline) {
    while(Now()<deadline) {
        fd_set set; FD_ZERO(&set); FD_SET(s,&set);
        timeval interval={0,100000};
        int rc=select(0,writing ? NULL : &set,writing ? &set : NULL,NULL,&interval);
        if(rc>0) return true;
        if(rc==SOCKET_ERROR) return false;
    }
    return false;
}

// One request on a connection of its own, and the whole response (the
// request asks the server to close the connection after it).
bool Exchange(DWORD address, int port, const std::string& request, double seconds, std::string& response, size_t most = ResponseMost) {
    response.clear();
    SOCKET s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if(s==INVALID_SOCKET) return false;
    u_long nonblocking=1;
    ioctlsocket(s,FIONBIO,&nonblocking);
    sockaddr_in to; memset(&to,0,sizeof(to)); to.sin_family=AF_INET; to.sin_addr.s_addr=address; to.sin_port=htons(u_short(port));
    const double deadline=Now()+seconds;
    bool ok=!connect(s,reinterpret_cast<sockaddr*>(&to),sizeof(to)) || WSAGetLastError()==WSAEWOULDBLOCK;
    if(ok) {
        int error=0; int length=sizeof(error);
        ok=Wait(s,true,deadline) && !getsockopt(s,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&length) && !error;
    }
    for(size_t sent=0;ok && sent<request.size();) {
        if(!Wait(s,true,deadline)) { ok=false; break; }
        int n=send(s,request.data()+sent,int(request.size()-sent),0);
        if(n>0) sent+=size_t(n);
        else if(n==0 || WSAGetLastError()!=WSAEWOULDBLOCK) ok=false;
    }
    while(ok) {
        if(!Wait(s,false,deadline)) { ok=false; break; }
        char part[16384];
        int n=recv(s,part,sizeof(part),0);
        if(n==0) break;
        if(n<0) { if(WSAGetLastError()!=WSAEWOULDBLOCK) ok=false; continue; }
        response.append(part,size_t(n));
        if(response.size()>most) ok=false;
    }
    closesocket(s);
    return ok;
}

std::string FormValue(const std::string& text) {
    std::string out;
    for(size_t i=0;i<text.size();++i) {
        unsigned char c=(unsigned char)text[i];
        if(isalnum(c) || c=='-' || c=='_' || c=='.' || c=='~') out+=char(c);
        else { char hex[4]; _snprintf_s(hex,sizeof(hex),_TRUNCATE,"%%%02X",c); out+=hex; }
    }
    return out;
}

std::string Lower(std::string text) {
    for(size_t i=0;i<text.size();++i) text[i]=char(tolower((unsigned char)text[i]));
    return text;
}

// The status and body of a response; a chunked body is put together. basic:
// whether it asks for a login of the HTTP kind (Aurora's "HTTP security");
// cookie: the name=value of a cookie it sets, if any; type: its Content-Type.
bool ReadResponse(const std::string& response, int& status, std::string& body, bool& basic, std::string& cookie, std::string& type) {
    int major=0,minor=0;
    if(sscanf(response.c_str(),"HTTP/%d.%d %d",&major,&minor,&status)!=3) return false;
    size_t end=response.find("\r\n\r\n");
    if(end==std::string::npos) return false;
    std::string headers=Lower(response.substr(0,end));
    body=response.substr(end+4);
    basic=headers.find("\nwww-authenticate: basic")!=std::string::npos;
    size_t typeAt=headers.find("\ncontent-type:");
    type.clear();
    if(typeAt!=std::string::npos) {
        size_t from=headers.find_first_not_of(' ',typeAt+14);
        type=headers.substr(from,headers.find_first_of(";\r",from)-from);
    }
    size_t set=headers.find("\nset-cookie:");
    if(set!=std::string::npos) {
        size_t from=response.find_first_not_of(' ',set+12);
        size_t to=response.find_first_of(";\r",from);
        cookie=response.substr(from,to-from);
    }
    if(headers.find("\ntransfer-encoding: chunked")==std::string::npos) return true;
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
    return true;
}

std::string Base64(const std::string& data) {
    static const char digits[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for(size_t i=0;i<data.size();i+=3) {
        unsigned n=unsigned((unsigned char)data[i])<<16;
        if(i+1<data.size()) n|=unsigned((unsigned char)data[i+1])<<8;
        if(i+2<data.size()) n|=(unsigned char)data[i+2];
        out+=digits[(n>>18)&63];
        out+=digits[(n>>12)&63];
        out+=i+1<data.size() ? digits[(n>>6)&63] : '=';
        out+=i+2<data.size() ? digits[n&63] : '=';
    }
    return out;
}

// FTP path /Hdd1/NetDisc360/x as Aurora's scripts name it: Hdd1:\NetDisc360\x.
std::string ScriptPath(const std::string& ftpPath) {
    size_t slash=ftpPath.find('/',1);
    std::string drive=ftpPath.substr(1,slash==std::string::npos ? std::string::npos : slash-1);
    std::string rest=slash==std::string::npos ? std::string() : ftpPath.substr(slash);
    for(size_t i=0;i<rest.size();++i) if(rest[i]=='/') rest[i]='\\';
    return drive+":"+rest;
}
// The tiles' folder as Aurora keeps its games' folders, without the drive,
// in lower case and between backslashes (\netiso\); "" for a drive's top.
std::string TilesInList(const std::string& tiles) {
    size_t slash=tiles.find('/',1);
    if(slash==std::string::npos || slash+1>=tiles.size()) return std::string();
    std::string rest=Lower(tiles.substr(slash))+"\\";
    for(size_t i=0;i<rest.size();++i) if(rest[i]=='/') rest[i]='\\';
    return rest;
}
// A Lua string literal; bytes outside printable ASCII as \ddd.
std::string LuaQuote(const std::string& text) {
    std::string out="\"";
    for(size_t i=0;i<text.size();++i) {
        unsigned char c=(unsigned char)text[i];
        if(c=='\\' || c=='"') { out+='\\'; out+=char(c); }
        else if(c<' ' || c>126) { char code[8]; _snprintf_s(code,sizeof(code),_TRUNCATE,"\\%03u",c); out+=code; }
        else out+=char(c);
    }
    return out+"\"";
}
std::string Crlf(const std::string& text) {
    std::string out;
    for(size_t i=0;i<text.size();++i) { if(text[i]=='\n') out+='\r'; out+=text[i]; }
    return out;
}
}

AuroraWeb::AuroraWeb(DWORD address, int port, const std::string& user, const std::string& password, double timeoutSeconds)
    : address_(address), port_(port), user_(user), password_(password), timeout_(timeoutSeconds) {}

bool AuroraWeb::Send(const std::string& method, const std::string& path, const std::string& form, int& status, std::string& body) {
    char host[16]; AddressText(address_,host,sizeof(host));
    std::string request=method+" "+path+" HTTP/1.1\r\nHost: "+host+":"+std::to_string(port_)+"\r\nConnection: close\r\n";
    if(basic_) request+="Authorization: Basic "+Base64(user_+":"+password_)+"\r\n";
    else if(!token_.empty()) request+="Authorization: Bearer "+token_+"\r\n";
    if(!cookie_.empty()) request+="Cookie: "+cookie_+"\r\n";
    if(method=="POST") request+="Content-Type: application/x-www-form-urlencoded\r\nContent-Length: "+std::to_string(form.size())+"\r\n";
    request+="\r\n"+form;
    std::string response;
    if(!Exchange(address_,port_,request,timeout_,response,ImageMost)) {
        error_="Aurora's web server doesn't answer on port "+std::to_string(port_)+" (Aurora's settings, HTTP server: is it on?)";
        return false;
    }
    if(!ReadResponse(response,status,body,challenged_,cookie_,type_)) { error_="Aurora's web server sent an answer this doesn't understand"; return false; }
    return true;
}

// With "HTTP security" on in Aurora's settings, its web server asks for the
// login on every request (HTTP's own, Basic) and refuses /authenticate
// without it; then the login goes on each request instead of a token.
bool AuroraWeb::Login() {
    token_.clear();
    loggedIn_=false;
    int status=0;
    std::string body;
    const std::string form="username="+FormValue(user_)+"&password="+FormValue(password_);
    if(!Send("POST","/authenticate",form,status,body)) return false;
    if(status==401 && challenged_ && !basic_) {
        basic_=true;
        if(!Send("POST","/authenticate",form,status,body)) return false;
    }
    if(status==401 || status==403) { error_="Aurora's web server refused the login (Aurora's settings, HTTP server, has its user name and password)"; return false; }
    web::Value v;
    const web::Value* token;
    if(status/100==2 && web::Parse(body,v) && (token=v.Get("token")) && token->type==web::Value::String && !token->text.empty()) token_=token->text;
    else if(!basic_) {
        error_="Aurora's web server didn't log in (HTTP "+std::to_string(status)+")";
        return false;
    }
    loggedIn_=true;
    return true;
}

// Aurora 0.7b.2's own Nova (its web server module) serves /title and
// /title/launch; the older Nova of the NetISO kit has neither and serves the
// same as oldPath (/getGameInfo, /postLaunchGame), with the same fields.
bool AuroraWeb::Call(const std::string& method, const std::string& path, const std::string& oldMethod, const std::string& oldPath,
                     const std::string& form, std::string& body) {
    if(oldNova_) return Call(oldMethod,oldPath,form,body);
    if(Call(method,path,form,body)) return true;
    if(status_!=404) return false;
    oldNova_=true;
    return Call(oldMethod,oldPath,form,body);
}

bool AuroraWeb::Call(const std::string& method, const std::string& path, const std::string& form, std::string& body) {
    int& status=status_;
    status=0;
    if(!loggedIn_ && !Login()) return false;
    if(!Send(method,path,form,status,body)) return false;
    if(status==401 && !basic_) {
        if(challenged_) basic_=true;  // The token isn't enough: the login goes on each request.
        else if(!Login()) return false;  // The login ran out.
        if(!Send(method,path,form,status,body)) return false;
    }
    if(status==401 || status==403) { error_="Aurora's web server refused the login (Aurora's settings, HTTP server, has its user name and password)"; return false; }
    if(status/100!=2) {
        std::string shown=path.substr(0,path.find('?'));
        error_="Aurora's web server answered "+shown+" with HTTP "+std::to_string(status);
        return false;
    }
    return true;
}

bool AuroraWeb::Get(const std::string& path, const std::string& oldPath, std::string& body) {
    return Call("GET",path,"GET",oldPath,"",body);
}

bool AuroraWeb::Title(std::string& path) {
    std::string body;
    web::Value v;
    const web::Value* p;
    if(!Get("/title","/getGameInfo",body)) return false;
    if(!web::Parse(body,v) || !(p=v.Get("path")) || p->type!=web::Value::String) { error_="Aurora's web server didn't say what runs"; return false; }
    path=p->text;
    return true;
}

bool AuroraWeb::InAurora() {
    std::string path;
    if(!Title(path)) return false;
    std::string lower=Lower(path);
    return lower.size()>=11 && lower.compare(lower.size()-11,11,"\\aurora.xex")==0;
}

bool AuroraWeb::Launch(const std::string& folder, const std::string& exec) {
    std::string body;
    return Call("POST","/title/launch","POST","/postLaunchGame","path="+FormValue(folder)+"&exec="+FormValue(exec)+"&type=0",body);
}

bool AuroraWeb::RestartAurora() {
    std::string path;
    if(!Title(path)) return false;
    std::string lower=Lower(path);
    size_t cut=path.rfind('\\');
    if(lower.size()<11 || lower.compare(lower.size()-11,11,"\\aurora.xex") || cut==std::string::npos) { error_="Aurora isn't on screen"; return false; }
    return Launch(path.substr(0,cut),path.substr(cut+1));
}

bool AuroraWeb::Screenshots(std::string& json) {
    return Get("/screencapture/meta/list","/getScreenCaptures",json);
}

bool AuroraWeb::TakeScreenshot() {
    std::string body;
    return Get("/screencapture/meta","/getScreenCapture",body);
}

bool AuroraWeb::Screenshot(const std::string& id, std::string& image, std::string& type) {
    std::string uuid=FormValue(id);
    if(!Get("/image/screencapture?uuid="+uuid,"/getScreenCaptureImage?uuid="+uuid,image)) return false;
    type=type_.empty() ? "image/bmp" : type_;
    return true;
}

bool AuroraWeb::DeleteScreenshot(const std::string& id) {
    std::string uuid=FormValue(id), body;
    return Call("DELETE","/screencapture?uuid="+uuid,"POST","/postScreenCaptureDelete?uuid="+uuid,"uuid="+uuid,body);
}

bool Xbdm(DWORD address, int port, const std::string& command, std::string& error) {
    std::string response;
    // xbdm greets ("201- connected") before it takes a command; the
    // greeting and the answer come back on the one connection, which it
    // closes after "bye". A console that turns off may close it first.
    bool ok=Exchange(address,port,command+"\r\nbye\r\n",5,response);
    if(response.compare(0,3,"201")) {
        error="its debug monitor (xbdm, port "+std::to_string(port)+") doesn't answer; it runs only where xbdm.xex is loaded as a DashLaunch plugin";
        return false;
    }
    size_t line=response.find("\r\n");
    std::string answer=line==std::string::npos ? std::string() : response.substr(line+2,3);
    if(!answer.empty() && answer[0]!='2') {
        size_t end=response.find("\r\n",line+2);
        error="its debug monitor answered \""+response.substr(line+2,end==std::string::npos ? std::string::npos : end-line-2)+"\"";
        return false;
    }
    (void)ok;
    return true;
}

namespace {
// The text between <tag> and </tag> in an XML body (the first), with its
// entities decoded; "" if it isn't there.
std::string XmlText(const std::string& body, const std::string& tag) {
    size_t from=body.find("<"+tag+">");
    if(from==std::string::npos) return std::string();
    from+=tag.size()+2;
    size_t to=body.find("</"+tag+">",from);
    if(to==std::string::npos) return std::string();
    std::string raw=body.substr(from,to-from), out;
    for(size_t i=0;i<raw.size();++i) {
        if(raw[i]!='&') { out+=raw[i]; continue; }
        size_t semi=raw.find(';',i);
        if(semi==std::string::npos || semi-i>10) { out+='&'; continue; }
        std::string entity=raw.substr(i+1,semi-i-1);
        unsigned long code=0;
        if(entity=="amp") code='&';
        else if(entity=="lt") code='<';
        else if(entity=="gt") code='>';
        else if(entity=="quot") code='"';
        else if(entity=="apos") code='\'';
        else if(entity.size()>1 && entity[0]=='#') code=entity[1]=='x' || entity[1]=='X' ? strtoul(entity.c_str()+2,NULL,16) : strtoul(entity.c_str()+1,NULL,10);
        if(!code || code>0x10FFFF) { out+='&'; continue; }
        // As UTF-8.
        if(code<0x80) out+=char(code);
        else if(code<0x800) { out+=char(0xC0|(code>>6)); out+=char(0x80|(code&0x3F)); }
        else if(code<0x10000) { out+=char(0xE0|(code>>12)); out+=char(0x80|((code>>6)&0x3F)); out+=char(0x80|(code&0x3F)); }
        else { out+=char(0xF0|(code>>18)); out+=char(0x80|((code>>12)&0x3F)); out+=char(0x80|((code>>6)&0x3F)); out+=char(0x80|(code&0x3F)); }
        i=semi;
    }
    return out;
}
// text without control characters or bars (the settings file's separator),
// spaces at its ends trimmed, at most most bytes (not cutting a UTF-8 character).
std::string PlainName(const std::string& text, size_t most) {
    std::string out;
    for(size_t i=0;i<text.size();++i) {
        unsigned char c=(unsigned char)text[i];
        if(c<' ' || c==127) out+=' ';
        else if(c=='|') out+='/';
        else out+=char(c);
    }
    size_t a=out.find_first_not_of(' '), b=out.find_last_not_of(' ');
    out=a==std::string::npos ? std::string() : out.substr(a,b-a+1);
    if(out.size()>most) {
        size_t cut=most;
        while(cut>0 && (((unsigned char)out[cut])&0xC0)==0x80) --cut;
        out=out.substr(0,cut);
    }
    return out;
}
}

bool ConsoleName(DWORD address, int port, std::string& name, std::string& id, std::string& error) {
    name.clear(); id.clear();
    char host[16]; AddressText(address,host,sizeof(host));
    std::string request=std::string("GET / HTTP/1.1\r\nHost: ")+host+":"+std::to_string(port)+"\r\nConnection: close\r\n\r\n";
    std::string response;
    if(!Exchange(address,port,request,3,response,64*1024) && response.empty()) {
        error="its media sharing (port "+std::to_string(port)+") doesn't answer";
        return false;
    }
    size_t body=response.find("\r\n\r\n");
    if(response.compare(0,5,"HTTP/") || response.find(" 200",0)!=response.find(' ') || body==std::string::npos) {
        error="its media sharing answered something else";
        return false;
    }
    std::string xml=response.substr(body+4);
    if(XmlText(xml,"modelName")!="Xbox 360") { error="that isn't an Xbox 360's media sharing"; return false; }
    name=PlainName(XmlText(xml,"friendlyName"),ConsoleNameMost);
    id=Lower(PlainName(XmlText(xml,"UDN"),ConsoleNameMost));
    if(!id.compare(0,5,"uuid:")) id.erase(0,5);
    if(name.empty()) { error="it has no name"; return false; }
    return true;
}

bool ConsoleNotify(DWORD address, int port, const std::string& text, std::string& error) {
    std::string ascii;
    for(size_t i=0;i<text.size() && ascii.size()<100;++i) {
        unsigned char c=(unsigned char)text[i];
        if(c>=0x80 && (c&0xC0)==0x80) continue;  // The rest of a UTF-8 character.
        ascii+=c>=' ' && c<127 && c!='"' ? char(c) : c=='"' ? '\'' : '?';
    }
    std::string hex;
    for(size_t i=0;i<ascii.size();++i) { char two[3]; _snprintf_s(two,sizeof(two),_TRUNCATE,"%02X",(unsigned char)ascii[i]); hex+=two; }
    // JRPC2's XNotify: a string (tagged 2) and the icon (an int, 0: the Xbox logo).
    std::string command="consolefeatures ver=2 type=12 params=\"A\\0\\A\\2\\2/"+std::to_string(ascii.size())+"\\"+hex+"\\1\\0\\\"";
    std::string response;
    Exchange(address,port,command+"\r\nbye\r\n",5,response);
    if(response.compare(0,3,"201")) {
        error="its debug monitor (xbdm, port "+std::to_string(port)+") doesn't answer; it runs only where xbdm.xex is loaded as a DashLaunch plugin";
        return false;
    }
    size_t line=response.find("\r\n");
    size_t end=line==std::string::npos ? std::string::npos : response.find("\r\n",line+2);
    std::string answer=line==std::string::npos ? std::string() : response.substr(line+2,end==std::string::npos ? std::string::npos : end-line-2);
    if(!answer.compare(0,9,"200- S_OK")) return true;
    if(!answer.compare(0,11,"200- error=")) { error="JRPC2 answered \""+answer.substr(11)+"\""; return false; }
    error="its debug monitor has no JRPC2 (load JRPC2.xex as a DashLaunch plugin; it answered \""+answer+"\")";
    return false;
}

bool XbdmAnswer(DWORD address, int port, const std::string& command, std::string& answer, std::string& error) {
    std::string response;
    answer.clear();
    bool ok=Exchange(address,port,command+"\r\nbye\r\n",5,response);
    if(response.compare(0,3,"201")) {
        error="its debug monitor (xbdm, port "+std::to_string(port)+") doesn't answer; it runs only where xbdm.xex is loaded as a DashLaunch plugin";
        return false;
    }
    size_t line=response.find("\r\n");
    answer=line==std::string::npos ? std::string() : response.substr(line+2);
    size_t bye=answer.rfind("200- bye");
    if(bye!=std::string::npos) answer.erase(bye);
    if(answer.empty() || answer[0]!='2') {
        error="its debug monitor answered \""+answer.substr(0,answer.find("\r\n"))+"\"";
        return false;
    }
    (void)ok;
    return true;
}

std::string DevicePath(const std::string& ftpPath) {
    std::string lower=Lower(ftpPath);
    if(lower.compare(0,5,"/hdd1") || (lower.size()>5 && lower[5]!='/')) return std::string();
    std::string rest=ftpPath.substr(5);
    for(size_t i=0;i<rest.size();++i) if(rest[i]=='/') rest[i]='\\';
    return "\\Device\\Harddisk0\\Partition1"+rest;
}

// The Lua function NetDiscMove (see MoveScript in aurora.h): Aurora's scan
// path and rows for the tiles in from now point at tiles, for those whose
// folder (in lower case) is one of folders. It returns what it did, for
// MovedMarker: "unread" when it couldn't read Aurora's list, else
// "moved=<rows> left=<rows still in from> scanpath=<0 or 1>".
std::string MoveLua(const std::string& from, const std::string& tiles, const std::vector<std::string>& folders) {
    std::string old=TilesInList(from), to=TilesInList(tiles);
    if(old.size()<2 || to.size()<2) return std::string();
    // Without the first backslash; "to" keeps its capitals as Aurora would write them.
    old=old.substr(1);
    size_t slash=tiles.find('/',1);
    to=tiles.substr(slash+1)+"\\";
    for(size_t i=0;i<to.size();++i) if(to[i]=='/') to[i]='\\';
    std::string text=
        "local NetDiscFrom, NetDiscTo = "+LuaQuote(old)+", "+LuaQuote(to)+"\n"
        "local NetDiscHere = {\n";
    for(size_t i=0;i<folders.size();++i) text+="   ["+LuaQuote(Lower(folders[i]))+"] = true,\n";
    text+=R"lua(}
-- "Hdd1:\NetISO\Halo 3" as "Hdd1:", "\" and "NetISO\Halo 3".
local function NetDiscSplit(path)
   local head, rest = string.match(path or "", "^([^\\]*:)(.*)$")
   if head == nil then head, rest = "", path or "" end
   local slash = ""
   if string.sub(rest, 1, 1) == "\\" then slash, rest = "\\", string.sub(rest, 2) end
   return head, slash, rest
end
local function NetDiscSql(text) return (string.gsub(text, "'", "''")) end
local function NetDiscRun(sql)
   local ran, result = pcall(Sql.Execute, sql)
   return ran and result ~= false
end
local function NetDiscMove()
   local fromLength, moved, left, scanPath, target = string.len(NetDiscFrom), 0, 0, 0, nil
   -- The scan path first, kept with its Id, so the rows keep theirs.
   local readPaths, paths = pcall(Sql.ExecuteFetchRows, "SELECT Id, Path FROM Config.ScanPaths")
   if readPaths and paths ~= nil then
      local old = nil
      for _, row in pairs(paths) do
         local head, slash, rest = NetDiscSplit(row.Path)
         local folder = string.lower(rest)
         if string.sub(folder, -1) ~= "\\" then folder = folder .. "\\" end
         if folder == string.lower(NetDiscTo) then target = tonumber(row.Id) end
         if folder == NetDiscFrom and old == nil then old = row end
      end
      if target == nil and old ~= nil and tonumber(old.Id) ~= nil then
         local head, slash, rest = NetDiscSplit(old.Path)
         local path = head .. slash .. string.sub(NetDiscTo, 1, string.len(NetDiscTo) - 1)
         if string.sub(rest, -1) == "\\" then path = path .. "\\" end
         if NetDiscRun("UPDATE Config.ScanPaths SET Path = '" .. NetDiscSql(path) .. "' WHERE Id = " .. tonumber(old.Id)) then scanPath = 1 end
      end
   end
   local read, rows = pcall(Sql.ExecuteFetchRows, "SELECT Id, Directory FROM ContentItems")
   if not read or rows == nil then return "unread" end
   for _, row in pairs(rows) do
      local id = tonumber(row.Id)
      local head, slash, rest = NetDiscSplit(row.Directory)
      local old = string.sub(rest, 1, fromLength)
      if id ~= nil and string.lower(old) == NetDiscFrom then
         local tile = string.match(string.lower(string.sub(rest, fromLength + 1)), "^[^\\]+")
         if tile ~= nil and NetDiscHere[tile] then
            -- Written as Aurora wrote the old one: in lower case if that was.
            local to = NetDiscTo
            if old == string.lower(old) then to = string.lower(to) end
            local sql = "UPDATE ContentItems SET Directory = '" .. NetDiscSql(head .. slash .. to .. string.sub(rest, fromLength + 1)) .. "'"
            -- Under the new folder's own scan path, if Aurora has one already.
            if target ~= nil then sql = sql .. ", ScanPathId = " .. target end
            if NetDiscRun(sql .. " WHERE Id = " .. id) then moved = moved + 1 else left = left + 1 end
         elseif tile ~= nil then left = left + 1 end
      end
   end
   return "moved=" .. moved .. " left=" .. left .. " scanpath=" .. scanPath
end
)lua";
    return text;
}

std::string CleanupScript(const std::string& tiles, const std::vector<std::string>& folders, const std::string& from, const std::string& app) {
    std::string inList=TilesInList(tiles);
    if(inList.empty()) return std::string();
    std::string text=
        "scriptTitle = \"NetDisc360 Cleanup\"\n"
        "scriptAuthor = \"NetDisc360\"\n"
        "scriptVersion = 1\n"
        "scriptDescription = \"Takes games whose NetDisc360 tile is gone off Aurora's list. It deletes no file.\"\n"
        "scriptPermissions = { \"sql\", \"filesystem\" }\n"
        "\n"
        "-- Written by NetDisc360 each time it updates "+ScriptPath(tiles)+", with the\n"
        "-- folders there then. Aurora keeps a game on its list when its folder is\n"
        "-- gone; this takes those of NetDisc360's tiles off the list, after asking.\n"
        "local Tiles = "+LuaQuote(inList)+"\n"
        "local Here = {\n";
    for(size_t i=0;i<folders.size();++i) text+="   ["+LuaQuote(Lower(folders[i]))+"] = true,\n";
    text+=
        "}\n"
        "local Tables = { \"UserRecentGames\", \"UserFavorites\", \"UserHidden\", \"TitleSettings\" }\n"
        "\n"
        "-- A game's folder as Aurora keeps it, in lower case and without its drive.\n"
        "local function Folder(directory)\n"
        "   local folder = string.gsub(string.lower(directory or \"\"), \"^[^\\\\]*:\", \"\")\n"
        "   if string.sub(folder, 1, 1) ~= \"\\\\\" then folder = \"\\\\\" .. folder end\n"
        "   return folder\n"
        "end\n"
        "\n"
        "local function Gone(directory)\n"
        "   local folder = Folder(directory)\n"
        "   if string.sub(folder, 1, string.len(Tiles)) ~= Tiles then return false end\n"
        "   local tile = string.match(string.sub(folder, string.len(Tiles) + 1), \"^[^\\\\]+\")\n"
        "   return tile ~= nil and not Here[tile]\n"
        "end\n"
        "\n"
        "function main()\n";
    std::string move=from.empty() ? std::string() : MoveLua(from,tiles,folders);
    if(!move.empty()) {
        // Before main, at the top: the move's own functions.
        text.insert(text.find("function main()\n"),
            "-- NetDisc360 moved its tiles from "+ScriptPath(from)+" to "+ScriptPath(tiles)+".\n"
            "-- Aurora's scan path and its rows for them follow them, so their art,\n"
            "-- favourites and play history stay, if Aurora's start didn't do that yet.\n"
            +move+"\n");
        text+=
        "   local ran, done = pcall(NetDiscMove)\n"
        "   if ran then pcall(FileSystem.WriteFile, "+LuaQuote(ScriptPath(app+"/"+MovedMarker))+", done) end\n"
        "   if ran and string.match(done or \"\", \"^moved=(%d+)\") ~= \"0\" and done ~= \"unread\" then Script.RefreshListOnExit(true) end\n";
    }
    text+=
        "   local read, rows = pcall(Sql.ExecuteFetchRows, \"SELECT Id, Directory FROM ContentItems\")\n"
        "   if not read or rows == nil then\n"
        "      Script.ShowMessageBox(\"NetDisc360 Cleanup\", \"Aurora's list couldn't be read, so nothing changed.\", \"OK\")\n"
        "      return\n"
        "   end\n"
        "   local gone = {}\n"
        "   for _, row in pairs(rows) do\n"
        "      local id = tonumber(row.Id)\n"
        "      if id ~= nil and Gone(row.Directory) then table.insert(gone, id) end\n"
        "   end\n"
        "   if #gone == 0 then\n"
        "      Script.ShowMessageBox(\"NetDisc360 Cleanup\", \"Every NetDisc360 game on Aurora's list still has its tile. Nothing to do.\", \"OK\")\n"
        "      return\n"
        "   end\n"
        "   local answer = Script.ShowMessageBox(\"NetDisc360 Cleanup\", #gone .. \" game(s) on Aurora's list have no NetDisc360 tile any more. Take them off the list? No file is deleted.\", \"Take off\", \"Cancel\")\n"
        "   if answer == nil or answer.Button ~= 1 then return end\n"
        "   local ids = table.concat(gone, \",\")\n"
        "   for _, name in ipairs(Tables) do\n"
        "      pcall(Sql.Execute, \"DELETE FROM Config.\" .. name .. \" WHERE ContentId IN (\" .. ids .. \")\")\n"
        "   end\n"
        "   pcall(Sql.Execute, \"DELETE FROM ContentItems WHERE Id IN (\" .. ids .. \")\")\n"
        "   Script.RefreshListOnExit(true)\n"
        "   Script.ShowMessageBox(\"NetDisc360 Cleanup\", \"Took \" .. #gone .. \" game(s) off Aurora's list.\", \"OK\")\n"
        "end\n";
    return Crlf(text);
}

std::string OnlineScript(const std::string& tiles, const std::string& app, const std::string& from, const std::vector<std::string>& folders, bool tidy) {
    std::string helper=app+"/";
    std::string inList=TilesInList(tiles);
    if(inList.empty()) return std::string();
    std::string text=
        "-- Written by NetDisc360, which keeps it up to date. Its filter \"Hide PC\n"
        "-- games while NetDisc360 is off\" (Filters, User) hides the games in\n"
        "-- "+ScriptPath(tiles)+" when NetDisc360 wasn't running as Aurora started:\n"
        "-- NetDisc360 keeps "+OnlineMarker+" in its folder there while it runs, and\n"
        "-- deletes it when it quits. When that changes and Aurora is on screen,\n"
        "-- NetDisc360 can restart Aurora, so the list follows (see its settings).\n"
        "local NetDiscMarker = "+LuaQuote(ScriptPath(helper+OnlineMarker))+"\n"
        "local NetDiscShown = "+LuaQuote(ScriptPath(helper+ShownMarker))+"\n"
        "local NetDiscTiles = "+LuaQuote(inList)+"\n"
        "local NetDiscRead, NetDiscThere = pcall(FileSystem.FileExists, NetDiscMarker)\n"
        "-- Without the file functions, nothing is hidden.\n"
        "NetDiscOnline = (not NetDiscRead) or NetDiscThere == true\n"
        "pcall(FileSystem.DeleteFile, NetDiscShown)\n"
        "local NetDiscTold = false\n";
    if(tidy) text+=
        "\n"
        "-- Games whose tile is gone come off Aurora's list as it starts (it keeps\n"
        "-- them otherwise, starting nothing), as NetDisc360 Cleanup does but without\n"
        "-- asking: only while at least one tile is still there, so a drive that\n"
        "-- can't be read changes nothing. It deletes no file.\n"
        "local NetDiscRoot = "+LuaQuote(ScriptPath(tiles)+"\\")+"\n"
        "local function NetDiscTidy()\n"
        "   local read, rows = pcall(Sql.ExecuteFetchRows, \"SELECT Id, Directory FROM ContentItems\")\n"
        "   if not read or rows == nil then return end\n"
        "   local gone, kept, looked = {}, 0, {}\n"
        "   for _, row in pairs(rows) do\n"
        "      local folder = string.gsub(string.lower(row.Directory or \"\"), \"^[^\\\\]*:\", \"\")\n"
        "      if string.sub(folder, 1, 1) ~= \"\\\\\" then folder = \"\\\\\" .. folder end\n"
        "      local id = tonumber(row.Id)\n"
        "      if id ~= nil and string.sub(folder, 1, string.len(NetDiscTiles)) == NetDiscTiles then\n"
        "         local tile = string.match(string.sub(folder, string.len(NetDiscTiles) + 1), \"^[^\\\\]+\")\n"
        "         if tile ~= nil and tile ~= \"netdisc360\" then\n"
        "            if looked[tile] == nil then\n"
        "               local asked, here = pcall(FileSystem.FileExists, NetDiscRoot .. tile .. \"\\\\default.xex\")\n"
        "               looked[tile] = (not asked) or here == true\n"
        "            end\n"
        "            if looked[tile] then kept = kept + 1 else table.insert(gone, id) end\n"
        "         end\n"
        "      end\n"
        "   end\n"
        "   if #gone == 0 or kept == 0 then return end\n"
        "   local ids = table.concat(gone, \",\")\n"
        "   for _, name in ipairs({ \"UserRecentGames\", \"UserFavorites\", \"UserHidden\", \"TitleSettings\" }) do\n"
        "      pcall(Sql.Execute, \"DELETE FROM Config.\" .. name .. \" WHERE ContentId IN (\" .. ids .. \")\")\n"
        "   end\n"
        "   pcall(Sql.Execute, \"DELETE FROM ContentItems WHERE Id IN (\" .. ids .. \")\")\n"
        "end\n"
        "pcall(NetDiscTidy)\n";
    std::string appInList=TilesInList(app);
    if(appInList.size()>1) text+=
        "\n"
        "-- NetDisc360's helper (boot.xex in "+ScriptPath(app)+") starts the games from\n"
        "-- their tiles; it is nothing to start from Aurora's list, where a scan path\n"
        "-- like \\Apps finds it. Hidden there for each profile Aurora knows of.\n"
        "local NetDiscApp = "+LuaQuote(appInList)+"\n"
        R"lua(local function NetDiscHideHelper()
   local read, rows = pcall(Sql.ExecuteFetchRows, "SELECT Id, Directory FROM ContentItems")
   if not read or rows == nil then return end
   local ids = {}
   for _, row in pairs(rows) do
      local folder = string.lower((string.gsub(row.Directory or "", "^[^\\]*:", "")))
      if string.sub(folder, 1, 1) ~= "\\" then folder = "\\" .. folder end
      if string.sub(folder, -1) ~= "\\" then folder = folder .. "\\" end
      if folder == NetDiscApp and tonumber(row.Id) ~= nil then table.insert(ids, tonumber(row.Id)) end
   end
   if #ids == 0 then return end
   local profiles = { [""] = true }
   for _, name in ipairs({ "UserHidden", "UserFavorites", "UserRecentGames" }) do
      local ran, found = pcall(Sql.ExecuteFetchRows, "SELECT DISTINCT ProfileId FROM Config." .. name)
      if ran and found ~= nil then
         for _, row in pairs(found) do
            if type(row.ProfileId) == "string" then profiles[string.upper(row.ProfileId)] = true end
         end
      end
   end
   for _, id in ipairs(ids) do
      for profile in pairs(profiles) do
         pcall(Sql.Execute, "INSERT OR IGNORE INTO Config.UserHidden (ContentId, ProfileId) VALUES (" .. id .. ", '" .. (string.gsub(profile, "'", "''")) .. "')")
      end
   end
end
pcall(NetDiscHideHelper)
)lua";
    // The tiles' launchers carry version LauncherVersion (see library.h), and
    // Aurora keeps that as the game's base version; it lists a game's title
    // updates only for its base version, so the list stayed empty. The base
    // version the game's own updates are for goes back in.
    text+=
        "\n"
        "-- NetDisc360's tile launchers have a version no title update is for, so the\n"
        "-- console never applies one to the launcher itself. Aurora keeps that as the\n"
        "-- game's base version and then lists none of its title updates: the base\n"
        "-- version of the game's own updates (same Title ID and Media ID) goes back.\n"
        R"lua(local function NetDiscBaseVersions()
   local read, rows = pcall(Sql.ExecuteFetchRows, "SELECT Id, Directory, TitleId, MediaId FROM ContentItems WHERE BaseVersion = 2147483647")
   if not read or rows == nil then return end
   local readUpdates, updates = pcall(Sql.ExecuteFetchRows, "SELECT TitleId, MediaId, BaseVersion FROM TitleUpdates")
   if not readUpdates or updates == nil then return end
   local base = {}
   for _, u in pairs(updates) do
      local key = tostring(tonumber(u.TitleId)) .. ":" .. tostring(tonumber(u.MediaId))
      local version = tonumber(u.BaseVersion)
      if version ~= nil and version ~= 2147483647 and (base[key] == nil or version < base[key]) then base[key] = version end
   end
   for _, row in pairs(rows) do
      local folder = string.lower((string.gsub(row.Directory or "", "^[^\\]*:", "")))
      if string.sub(folder, 1, 1) ~= "\\" then folder = "\\" .. folder end
      local id = tonumber(row.Id)
      local version = base[tostring(tonumber(row.TitleId)) .. ":" .. tostring(tonumber(row.MediaId))]
      if id ~= nil and version ~= nil and string.sub(folder, 1, string.len(NetDiscTiles)) == NetDiscTiles then
         pcall(Sql.Execute, "UPDATE ContentItems SET BaseVersion = " .. version .. " WHERE Id = " .. id)
      end
   end
end
pcall(NetDiscBaseVersions)
)lua";
    text+=
        "\n-- Aurora lists a title update in Hdd1:\\Cache it found itself with no copy in\n"
        "-- its store. As it starts a game it deletes the update files and puts the\n"
        "-- one on back from its store, so with that entry on the game ends up with\n"
        "-- no update at all. When Aurora also has the same update with a copy in its\n"
        "-- store, that one goes on instead (from the next Aurora start).\n"
        R"lua(local function NetDiscTuBackups()
   local read, rows = pcall(Sql.ExecuteFetchRows, "SELECT t.Id, t.Hash, t.TitleId, t.MediaId FROM Config.ActiveTitleUpdates a JOIN TitleUpdates t ON t.Id = a.TitleUpdateId WHERE IFNULL(t.BackupPath, '') = ''")
   if not read or rows == nil then return end
   for _, row in pairs(rows) do
      local id, titleId, mediaId = tonumber(row.Id), tonumber(row.TitleId), tonumber(row.MediaId)
      if id ~= nil and titleId ~= nil and mediaId ~= nil and row.Hash ~= nil and row.Hash ~= "" then
         local readCopy, copies = pcall(Sql.ExecuteFetchRows, "SELECT Id FROM TitleUpdates WHERE Hash = '" .. string.gsub(row.Hash, "'", "''") ..
            "' AND TitleId = " .. string.format("%.0f", titleId) .. " AND MediaId = " .. string.format("%.0f", mediaId) .. " AND IFNULL(BackupPath, '') <> '' ORDER BY Id LIMIT 1")
         local copy = readCopy and copies ~= nil and copies[1] ~= nil and tonumber(copies[1].Id) or nil
         local added, result = false, false
         if copy ~= nil then added, result = pcall(Sql.Execute, "INSERT OR IGNORE INTO Config.ActiveTitleUpdates (TitleUpdateId) VALUES (" .. string.format("%.0f", copy) .. ")") end
         if added and result ~= false then
            pcall(Sql.Execute, "DELETE FROM Config.ActiveTitleUpdates WHERE TitleUpdateId = " .. string.format("%.0f", id))
         end
      end
   end
end
pcall(NetDiscTuBackups)
)lua";
    std::string move=from.empty() ? std::string() : MoveLua(from,tiles,folders);
    if(!move.empty()) text+=
        "\n"
        "-- NetDisc360 moved its tiles from "+ScriptPath(from)+" to "+ScriptPath(tiles)+".\n"
        "-- As Aurora starts, its scan path and its rows for them follow them, so\n"
        "-- their art, favourites and play history stay. What it did goes in\n"
        "-- "+MovedMarker+" for NetDisc360, which then restarts Aurora so its list follows.\n"
        +move+
        "local NetDiscMoveRan, NetDiscMoveDone = pcall(NetDiscMove)\n"
        "if NetDiscMoveRan then pcall(FileSystem.WriteFile, "+LuaQuote(ScriptPath(helper+MovedMarker))+", NetDiscMoveDone) end\n";
    text+=
        "\n"
        "GameListFilterCategories.User[\"Hide PC games while NetDisc360 is off\"] = function(Content)\n"
        "   if not NetDiscTold then\n"
        "      NetDiscTold = true\n"
        "      pcall(FileSystem.WriteFile, NetDiscShown, NetDiscOnline and \"online\" or \"offline\")\n"
        "   end\n"
        "   if NetDiscOnline then return true end\n"
        "   return string.find(\"\\\\\" .. string.lower(Content.Directory or \"\"), NetDiscTiles, 1, true) == nil\n"
        "end\n";
    return Crlf(text);
}

const char LinkOffScriptPath[] = "/User/Scripts/Content/Filters/NetDiscLinkOff.lua";

std::string LinkOffScript(const std::string& done) {
    std::string text=
        "-- Written by NetDisc360 when \"Turn LiNK off\" was chosen on its Health page.\n"
        "-- With LiNK on, Nova takes the System Link traffic of games streamed from\n"
        "-- NetDisc360, so they don't find the other consoles. As Aurora starts, this\n"
        "-- turns LiNK off in Aurora's settings once and writes what it did to\n"
        "-- NetDiscLinkDone; while that file is there it does nothing, so LiNK\n"
        "-- switched on again in Aurora stays on. Restart the console afterwards.\n"
        "local NetDiscLinkDone = "+LuaQuote(ScriptPath(done))+"\n"
        "\n"
        "local NetDiscLinkRead, NetDiscLinkThere = pcall(FileSystem.FileExists, NetDiscLinkDone)\n"
        "if Sql ~= nil and NetDiscLinkRead and NetDiscLinkThere ~= true then\n"
        "   local ok, result = pcall(Sql.Execute, \"UPDATE Config.SystemSettings SET Value = '0' WHERE Name = 'PluginLiNK'\")\n"
        "   pcall(FileSystem.WriteFile, NetDiscLinkDone, (ok and result ~= false) and \"off\\r\\n\" or \"error\\r\\n\")\n"
        "end\n";
    return Crlf(text);
}

const char TitleUpdateScriptPath[] = "/User/Scripts/Content/Filters/NetDiscTitleUpdates.lua";

std::string TitleUpdateScript(const std::vector<AuroraTu>& wants, const std::string& done) {
    std::string text=
        "-- Written by NetDisc360 when a title update is switched on or off on its\n"
        "-- dashboard. Each time Aurora starts a game, its title update manager puts\n"
        "-- the update Aurora has on in place and deletes the others, so as Aurora\n"
        "-- starts, this makes Aurora's own list say what was chosen. It does that\n"
        "-- once and writes what it did to NetDiscTuDone; while that file is there it\n"
        "-- does nothing, so a change made in Aurora's title update screen stays.\n"
        "local NetDiscTuDone = "+LuaQuote(ScriptPath(done))+"\n"
        "local NetDiscTuWants = {\n";
    for(size_t i=0;i<wants.size();++i) {
        const AuroraTu& w=wants[i];
        char ids[32]; _snprintf_s(ids,sizeof(ids),_TRUNCATE,"%08X %08X",w.titleId,w.mediaId);
        text+="   { titleId = "+std::to_string((unsigned long long)w.titleId)+", mediaId = "+std::to_string((unsigned long long)w.mediaId)+", ids = "+LuaQuote(ids)+
              ", hash = "+LuaQuote(w.hash)+", name = "+LuaQuote(w.name)+", file = "+LuaQuote(w.file)+", size = "+LuaQuote(w.size)+
              ", device = "+LuaQuote(w.device)+", livePath = "+LuaQuote(w.livePath)+", backupPath = "+LuaQuote(w.backupPath)+
              ", base = "+std::to_string((unsigned long long)w.base)+", version = "+std::to_string((unsigned long long)w.version)+" },\n";
    }
    text+=
        "}\n"
        "\n"
        "local function NetDiscTuText(text)\n"
        "   return \"'\" .. (string.gsub(text or \"\", \"'\", \"''\")) .. \"'\"\n"
        "end\n"
        "\n"
        "-- Aurora may keep IDs as signed numbers.\n"
        "local function NetDiscTuId(id)\n"
        "   return \"(\" .. string.format(\"%.0f\", id) .. \",\" .. string.format(\"%.0f\", id - 4294967296) .. \")\"\n"
        "end\n"
        "\n"
        "local function NetDiscTuRun(query)\n"
        "   local ok, result = pcall(Sql.Execute, query)\n"
        "   return ok and result ~= false\n"
        "end\n"
        "\n"
        "local function NetDiscTuApply(want)\n"
        "   local game = \"TitleId IN \" .. NetDiscTuId(want.titleId) .. \" AND MediaId IN \" .. NetDiscTuId(want.mediaId)\n"
        "   local mine = \"Hash = \" .. NetDiscTuText(want.hash) .. \" AND \" .. game\n"
        "   if want.hash ~= \"\" and want.backupPath ~= \"\" then\n"
        "      -- Aurora hasn't seen it yet: on its list, with the copy in its store.\n"
        "      NetDiscTuRun(\"INSERT INTO TitleUpdates (DisplayName, FileName, LiveDeviceId, LivePath, TitleId, MediaId, BaseVersion, Version, Hash, BackupPath, FileSize) SELECT \" ..\n"
        "         NetDiscTuText(want.name) .. \", \" .. NetDiscTuText(want.file) .. \", \" .. NetDiscTuText(want.device) .. \", \" .. NetDiscTuText(want.livePath) .. \", \" ..\n"
        "         string.format(\"%.0f\", want.titleId) .. \", \" .. string.format(\"%.0f\", want.mediaId) .. \", \" .. want.base .. \", \" .. want.version .. \", \" ..\n"
        "         NetDiscTuText(want.hash) .. \", \" .. NetDiscTuText(want.backupPath) .. \", \" .. NetDiscTuText(want.size) ..\n"
        "         \" WHERE NOT EXISTS (SELECT 1 FROM TitleUpdates WHERE \" .. mine .. \")\")\n"
        "   end\n"
        "   if not NetDiscTuRun(\"DELETE FROM Config.ActiveTitleUpdates WHERE TitleUpdateId IN (SELECT Id FROM TitleUpdates WHERE \" .. game .. \")\") then return \"error\" end\n"
        "   if want.hash == \"\" then return \"off\" end\n"
        "   -- Aurora may list the same update twice (in Hdd1:\\Cache and in the\n"
        "   -- game's folder). It puts the one on back from its store as it starts the\n"
        "   -- game and deletes the rest, so an entry with no copy in its store would\n"
        "   -- leave the game with no update at all: put on one that has a copy.\n"
        "   if not NetDiscTuRun(\"INSERT OR IGNORE INTO Config.ActiveTitleUpdates (TitleUpdateId) SELECT Id FROM TitleUpdates WHERE \" .. mine ..\n"
        "      \" ORDER BY IFNULL(BackupPath, '') <> '' DESC, LiveDeviceId = \" .. NetDiscTuText(want.device) .. \" DESC LIMIT 1\") then return \"error\" end\n"
        "   local read, rows = pcall(Sql.ExecuteFetchRows, \"SELECT TitleUpdateId FROM Config.ActiveTitleUpdates WHERE TitleUpdateId IN (SELECT Id FROM TitleUpdates WHERE \" .. mine .. \")\")\n"
        "   if read and rows ~= nil and next(rows) ~= nil then return \"on\" end\n"
        "   return \"missing\"\n"
        "end\n"
        "\n"
        "local NetDiscTuRead, NetDiscTuThere = pcall(FileSystem.FileExists, NetDiscTuDone)\n"
        "if Sql ~= nil and NetDiscTuRead and NetDiscTuThere ~= true then\n"
        "   local lines = {}\n"
        "   for _, want in ipairs(NetDiscTuWants) do\n"
        "      local ok, result = pcall(NetDiscTuApply, want)\n"
        "      table.insert(lines, want.ids .. \" \" .. (ok and result or \"error\"))\n"
        "   end\n"
        "   pcall(FileSystem.WriteFile, NetDiscTuDone, table.concat(lines, \"\\r\\n\") .. \"\\r\\n\")\n"
        "end\n";
    return Crlf(text);
}
}
