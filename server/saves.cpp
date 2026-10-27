// SPDX-License-Identifier: MIT
#include "saves.h"
#include "backup.h"
#include "config.h"
#include <algorithm>
#include <ctype.h>
#include <map>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace saves {
namespace {
// A save is a few KB to a few MB; a game's saves together stay well under this.
const size_t MaxFile = 64 << 20, MaxTotal = 256 << 20;
// The profile package's own content: Title ID FFFE07D1, type 00010000.
const char ProfileFolder[] = "FFFE07D1/00010000";
const char SaveType[] = "00000001";

std::string Hex(DWORD value) { char text[9]; snprintf(text,sizeof(text),"%08X",value); return text; }
bool HexName(const std::string& name, size_t length) {
    if(name.size()!=length) return false;
    for(size_t i=0;i<length;++i) if(!isxdigit((unsigned char)name[i])) return false;
    return true;
}
std::string Upper(std::string text) {
    for(size_t i=0;i<text.size();++i) text[i]=char(toupper((unsigned char)text[i]));
    return text;
}
// The profiles' folders in Content: 16 hex digits, but not the all-zero one
// (content for every profile: title updates, DLC, games on demand).
bool Profiles(nia::FtpClient& ftp, std::vector<std::string>& out, std::string& error) {
    std::vector<nia::FtpEntry> entries;
    if(!ftp.List(ContentFolder,entries)) {
        if(ftp.Code()==550) return true;  // No Content folder: no saves.
        error=ftp.Error(); return false;
    }
    for(size_t i=0;i<entries.size();++i)
        if(entries[i].folder && HexName(entries[i].name,16) && entries[i].name!="0000000000000000") out.push_back(entries[i].name);
    return true;
}
// Whether folder (in parent's listing) is there, ignoring case; name gets its spelling.
bool Has(const std::vector<nia::FtpEntry>& entries, const std::string& wanted, std::string& name) {
    for(size_t i=0;i<entries.size();++i)
        if(entries[i].folder && Upper(entries[i].name)==Upper(wanted)) { name=entries[i].name; return true; }
    return false;
}
// Gamertags already read, by profile, so each profile's package is downloaded once.
std::mutex TagLock;
std::map<std::string,std::string> Tags;

std::string TagOf(nia::FtpClient& ftp, const std::string& profile) {
    { std::lock_guard<std::mutex> hold(TagLock); if(Tags.count(profile)) return Tags[profile]; }
    std::vector<unsigned char> package;
    std::string tag;
    if(ftp.Retrieve((std::string(ContentFolder)+"/"+profile+"/"+ProfileFolder+"/"+profile).c_str(),package,4<<20)) tag=Gamertag(package);
    std::lock_guard<std::mutex> hold(TagLock);
    Tags[profile]=tag;
    return tag;
}
std::string Path(const std::string& folder, const std::string& name) {
#ifdef _WIN32
    return folder+"\\"+name;
#else
    return folder+"/"+name;
#endif
}
// The files of a backup but its save.ini, which says when it was made.
bool SameSaves(const std::map<std::string,std::string>& a, const std::map<std::string,std::string>& b) {
    std::map<std::string,std::string> x=a, y=b;
    x.erase("save.ini"); y.erase("save.ini");
    return x==y;
}
std::string Value(const std::string& ini, const std::string& key) {
    for(size_t at=0;at<ini.size();) {
        size_t end=ini.find('\n',at); if(end==std::string::npos) end=ini.size();
        std::string line=ini.substr(at,end-at); at=end+1;
        if(!line.empty() && line[line.size()-1]=='\r') line.erase(line.size()-1);
        if(!line.compare(0,key.size()+1,key+"=")) return line.substr(key.size()+1);
    }
    return std::string();
}
}

std::string Gamertag(const std::vector<unsigned char>& package) {
    // An STFS package (CON/LIVE/PIRS) has its first display name, UTF-16
    // big-endian, at 0x411.
    if(package.size()<0x411+0x80 || !(!memcmp(package.data(),"CON ",4) || !memcmp(package.data(),"LIVE",4) || !memcmp(package.data(),"PIRS",4)))
        return std::string();
    std::string tag;
    for(size_t at=0x411;at<0x411+0x80;at+=2) {
        unsigned c=(package[at]<<8)|package[at+1];
        if(!c) break;
        if(c<0x20 || c>0x7E) return std::string();  // Gamertags are ASCII.
        tag+=char(c);
    }
    return tag;
}

bool Named(const std::string& name) {
    // 4D5307E6_2026-10-26_23-15-06.zip
    if(name.size()!=32 || !HexName(name.substr(0,8),8) || name[8]!='_' || name.compare(28,4,".zip")) return false;
    for(size_t i=9;i<28;++i) if(!isdigit((unsigned char)name[i]) && name[i]!='-' && name[i]!='_') return false;
    return true;
}

std::vector<Backup> List(const std::string& folder) {
    std::vector<config::Entry> files=config::Files(folder);
    std::vector<Backup> out;
    for(size_t i=0;i<files.size();++i) {
        if(!Named(files[i].name)) continue;
        Backup b;
        b.name=files[i].name; b.titleId=DWORD(strtoul(files[i].name.substr(0,8).c_str(),NULL,16)); b.size=files[i].size;
        struct tm t; memset(&t,0,sizeof(t));
        if(sscanf(files[i].name.c_str()+9,"%d-%d-%d_%d-%d-%d",&t.tm_year,&t.tm_mon,&t.tm_mday,&t.tm_hour,&t.tm_min,&t.tm_sec)!=6) continue;
        t.tm_year-=1900; t.tm_mon-=1; t.tm_isdst=-1;
        b.time=(long long)mktime(&t);
        out.push_back(b);
    }
    std::sort(out.begin(),out.end(),[](const Backup& a, const Backup& b) { return a.time!=b.time ? a.time>b.time : a.name>b.name; });
    return out;
}

bool Titles(nia::FtpClient& ftp, std::set<DWORD>& titles, std::string& error) {
    std::vector<std::string> profiles;
    if(!Profiles(ftp,profiles,error)) return false;
    for(size_t p=0;p<profiles.size();++p) {
        std::vector<nia::FtpEntry> games;
        if(!ftp.List((std::string(ContentFolder)+"/"+profiles[p]).c_str(),games)) { error=ftp.Error(); return false; }
        for(size_t g=0;g<games.size();++g) {
            if(!games[g].folder || !HexName(games[g].name,8) || Upper(games[g].name)=="FFFE07D1") continue;
            std::vector<nia::FtpEntry> types;
            std::string name;
            if(!ftp.List((std::string(ContentFolder)+"/"+profiles[p]+"/"+games[g].name).c_str(),types)) { error=ftp.Error(); return false; }
            if(Has(types,SaveType,name)) titles.insert(DWORD(strtoul(games[g].name.c_str(),NULL,16)));
        }
    }
    return true;
}

bool Take(nia::FtpClient& ftp, const std::string& folder, DWORD titleId, const std::string& game, int keep,
          Result& result, std::string& error) {
    result=Result(); result.profiles=0; result.files=0; result.bytes=0; result.same=false;
    std::vector<std::string> profiles;
    if(!Profiles(ftp,profiles,error)) return false;
    std::map<std::string,std::string> files;
    std::string ini="[Save]\r\ntitle_id="+Hex(titleId)+"\r\ngame="+game+"\r\n";
    long long now=(long long)time(NULL);
    ini+="time="+std::to_string(now)+"\r\n";
    size_t total=0;
    for(size_t p=0;p<profiles.size();++p) {
        std::string profileFolder=std::string(ContentFolder)+"/"+profiles[p], titleName, typeName;
        std::vector<nia::FtpEntry> entries;
        if(!ftp.List(profileFolder.c_str(),entries)) { error=ftp.Error(); return false; }
        if(!Has(entries,Hex(titleId),titleName)) continue;
        if(!ftp.List((profileFolder+"/"+titleName).c_str(),entries)) { error=ftp.Error(); return false; }
        if(!Has(entries,SaveType,typeName)) continue;
        std::string saveFolder=profileFolder+"/"+titleName+"/"+typeName;
        if(!ftp.List(saveFolder.c_str(),entries)) { error=ftp.Error(); return false; }
        int before=result.files;
        for(size_t i=0;i<entries.size();++i) {
            if(entries[i].folder) continue;
            std::vector<unsigned char> data;
            if(!ftp.Retrieve((saveFolder+"/"+entries[i].name).c_str(),data,MaxFile)) { error=ftp.Error(); return false; }
            total+=data.size();
            if(total>MaxTotal) { error="This game's saves are over 256 MB together, more than a save backup takes."; return false; }
            files[Upper(profiles[p])+"/"+entries[i].name].assign(data.begin(),data.end());
            ++result.files;
        }
        if(result.files==before) continue;
        ++result.profiles;
        std::string tag=TagOf(ftp,profiles[p]);
        ini+="profile_"+Upper(profiles[p])+"="+tag+"\r\n";
    }
    result.bytes=(long long)total;
    if(!result.files) return true;
    files["save.ini"]=ini;
    std::vector<Backup> before=List(folder);
    for(size_t i=0;i<before.size();++i) {
        if(before[i].titleId!=titleId) continue;
        std::string data;
        std::map<std::string,std::string> newest;
        if(config::ReadFile(Path(folder,before[i].name),data,MaxTotal+(16<<20)) && backup::Unpack(data,newest,error,MaxFile) && SameSaves(newest,files)) {
            result.same=true; result.name=before[i].name;
            return true;
        }
        error.clear();
        break;
    }
    if(!config::MakeFolder(folder)) { error="Can't make the folder "+folder+"."; return false; }
    // Named by the time, a second later if one was made this second already.
    for(time_t t=time_t(now);;++t) {
        char stamp[32]; struct tm local;
#ifdef _WIN32
        localtime_s(&local,&t);
#else
        localtime_r(&t,&local);
#endif
        strftime(stamp,sizeof(stamp),"%Y-%m-%d_%H-%M-%S",&local);
        result.name=Hex(titleId)+"_"+stamp+".zip";
        bool taken=false;
        for(size_t i=0;i<before.size() && !taken;++i) taken=before[i].name==result.name;
        if(!taken) break;
    }
    if(!config::WriteFile(Path(folder,result.name),backup::Zip(files,now),false)) { error="Can't write "+Path(folder,result.name)+"."; return false; }
    // The oldest of this game's beyond keep go.
    std::vector<Backup> all=List(folder);
    int kept=0;
    for(size_t i=0;i<all.size();++i)
        if(all[i].titleId==titleId && ++kept>keep) config::RemoveFile(Path(folder,all[i].name));
    return true;
}

bool Describe(const std::string& data, std::string& game, std::vector<std::string>& profiles) {
    std::map<std::string,std::string> files;
    std::string error;
    if(!backup::Unpack(data,files,error,MaxFile) || !files.count("save.ini")) return false;
    const std::string& ini=files["save.ini"];
    game=Value(ini,"game");
    for(std::map<std::string,std::string>::const_iterator f=files.begin();f!=files.end();++f) {
        size_t slash=f->first.find('/');
        if(slash==std::string::npos) continue;
        std::string profile=f->first.substr(0,slash), tag=Value(ini,"profile_"+profile);
        std::string shown=tag.empty() ? profile : tag;
        if(std::find(profiles.begin(),profiles.end(),shown)==profiles.end()) profiles.push_back(shown);
    }
    return true;
}

bool Put(nia::FtpClient& ftp, const std::string& data, int& written, std::string& error) {
    written=0;
    std::map<std::string,std::string> files;
    if(!backup::Unpack(data,files,error,MaxFile)) return false;
    if(!files.count("save.ini")) { error="That isn't a save backup."; return false; }
    std::string title=Upper(Value(files["save.ini"],"title_id"));
    if(!HexName(title,8)) { error="That save backup names no Title ID."; return false; }
    std::vector<std::string> profiles;
    if(!Profiles(ftp,profiles,error)) return false;
    std::set<std::string> made;
    for(std::map<std::string,std::string>::const_iterator f=files.begin();f!=files.end();++f) {
        size_t slash=f->first.find('/');
        if(slash==std::string::npos) continue;
        std::string profile=f->first.substr(0,slash), name=f->first.substr(slash+1), there;
        if(!HexName(profile,16) || name.empty() || name.find_first_of("/\\")!=std::string::npos || name==".." || name==".") continue;
        bool known=false;
        for(size_t i=0;i<profiles.size() && !known;++i) if(Upper(profiles[i])==profile) { known=true; there=profiles[i]; }
        if(!known) continue;
        std::string folder=std::string(ContentFolder)+"/"+there+"/"+title+"/"+SaveType;
        if(!made.count(folder)) {
            ftp.MakeFolder((std::string(ContentFolder)+"/"+there+"/"+title).c_str());
            if(!ftp.MakeFolder(folder.c_str())) { error=ftp.Error(); return false; }
            made.insert(folder);
        }
        if(!ftp.Store((folder+"/"+name).c_str(),f->second.data(),f->second.size())) { error=ftp.Error(); return false; }
        ++written;
    }
    if(!written) { error="None of the profiles in that backup are on this console."; return false; }
    return true;
}
}
