// SPDX-License-Identifier: MIT
#include "aurora.h"
#include "web.h"
#include "../shared/isvr.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

namespace nia {
const char CleanupScriptPath[] = "/User/Scripts/Utility/NetDisc360 Cleanup/Main.lua";
const char OnlineScriptPath[] = "/User/Scripts/Content/Filters/NetDiscOnline.lua";
const char OnlineMarker[] = "online.txt", ShownMarker[] = "shown.txt";

namespace {
enum { ResponseMost = 1 << 20 };

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
bool Exchange(DWORD address, int port, const std::string& request, double seconds, std::string& response) {
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
        if(response.size()>ResponseMost) ok=false;
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
// whether it asks for a login of the HTTP kind (Aurora's "HTTP security").
bool ReadResponse(const std::string& response, int& status, std::string& body, bool& basic) {
    int major=0,minor=0;
    if(sscanf(response.c_str(),"HTTP/%d.%d %d",&major,&minor,&status)!=3) return false;
    size_t end=response.find("\r\n\r\n");
    if(end==std::string::npos) return false;
    std::string headers=Lower(response.substr(0,end));
    body=response.substr(end+4);
    basic=headers.find("\nwww-authenticate: basic")!=std::string::npos;
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

// FTP path /Hdd1/NetISO/x as Aurora's scripts name it: Hdd1:\NetISO\x.
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

bool AuroraWeb::Send(const char* method, const char* path, const std::string& form, int& status, std::string& body) {
    char host[16]; AddressText(address_,host,sizeof(host));
    std::string request=std::string(method)+" "+path+" HTTP/1.1\r\nHost: "+host+":"+std::to_string(port_)+"\r\nConnection: close\r\n";
    if(basic_) request+="Authorization: Basic "+Base64(user_+":"+password_)+"\r\n";
    else if(!token_.empty()) request+="Authorization: Bearer "+token_+"\r\n";
    if(!strcmp(method,"POST")) request+="Content-Type: application/x-www-form-urlencoded\r\nContent-Length: "+std::to_string(form.size())+"\r\n";
    request+="\r\n"+form;
    std::string response;
    if(!Exchange(address_,port_,request,timeout_,response)) {
        error_="Aurora's web server doesn't answer on port "+std::to_string(port_)+" (Aurora's settings, HTTP server: is it on?)";
        return false;
    }
    if(!ReadResponse(response,status,body,challenged_)) { error_="Aurora's web server sent an answer this doesn't understand"; return false; }
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

bool AuroraWeb::Call(const char* method, const char* path, const std::string& form, std::string& body) {
    int status=0;
    if(!loggedIn_ && !Login()) return false;
    if(!Send(method,path,form,status,body)) return false;
    if(status==401 && !basic_) {
        if(challenged_) basic_=true;  // The token isn't enough: the login goes on each request.
        else if(!Login()) return false;  // The login ran out.
        if(!Send(method,path,form,status,body)) return false;
    }
    if(status==401 || status==403) { error_="Aurora's web server refused the login (Aurora's settings, HTTP server, has its user name and password)"; return false; }
    if(status/100!=2) { error_=std::string("Aurora's web server answered ")+path+" with HTTP "+std::to_string(status); return false; }
    return true;
}

bool AuroraWeb::Title(std::string& path) {
    std::string body;
    web::Value v;
    const web::Value* p;
    if(!Call("GET","/title","",body)) return false;
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
    return Call("POST","/title/launch","path="+FormValue(folder)+"&exec="+FormValue(exec)+"&type=0",body);
}

bool AuroraWeb::RestartAurora() {
    std::string path;
    if(!Title(path)) return false;
    std::string lower=Lower(path);
    size_t cut=path.rfind('\\');
    if(lower.size()<11 || lower.compare(lower.size()-11,11,"\\aurora.xex") || cut==std::string::npos) { error_="Aurora isn't on screen"; return false; }
    return Launch(path.substr(0,cut),path.substr(cut+1));
}

std::string DevicePath(const std::string& ftpPath) {
    std::string lower=Lower(ftpPath);
    if(lower.compare(0,5,"/hdd1") || (lower.size()>5 && lower[5]!='/')) return std::string();
    std::string rest=ftpPath.substr(5);
    for(size_t i=0;i<rest.size();++i) if(rest[i]=='/') rest[i]='\\';
    return "\\Device\\Harddisk0\\Partition1"+rest;
}

std::string CleanupScript(const std::string& tiles, const std::vector<std::string>& folders) {
    std::string inList=TilesInList(tiles);
    if(inList.empty()) return std::string();
    std::string text=
        "scriptTitle = \"NetDisc360 Cleanup\"\n"
        "scriptAuthor = \"NetDisc360\"\n"
        "scriptVersion = 1\n"
        "scriptDescription = \"Takes games whose NetDisc360 tile is gone off Aurora's list. It deletes no file.\"\n"
        "scriptPermissions = { \"sql\" }\n"
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
        "function main()\n"
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

std::string OnlineScript(const std::string& tiles) {
    std::string helper=tiles+"/NetDisc360/";
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
        "local NetDiscTold = false\n"
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
}
