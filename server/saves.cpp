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
// A save is a few KB to a few MB, but one file can be far bigger: SSX keeps
// a 106,663,936-byte ghost file (ssxvghosts) in its saves. So a file may be
// as big as a game's saves together may be, which keeps what one backup
// holds in memory bounded.
const size_t MaxTotal = 256 << 20, MaxFile = MaxTotal;
const char TooBig[] = "This game's saves are over 256 MB together, more than a save backup takes.";
// The profile package's own content: Title ID FFFE07D1, type 00010000.
const char ProfileFolder[] = "FFFE07D1/00010000";
const char SaveType[] = "00000001";

std::string Hex(DWORD value) { char text[9]; snprintf(text,sizeof(text),"%08X",(unsigned)value); return text; }
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

// A profile's gamertag: as known (signed in, from Aurora), else from its
// package. A signed-in profile's package is open on the console, which
// then refuses it over FTP (550, error 5), so a miss isn't kept: the next
// backup asks again.
std::string TagOf(nia::FtpClient& ftp, const std::string& profile, const std::map<std::string,std::string>& known) {
    std::map<std::string,std::string>::const_iterator k=known.find(Upper(profile));
    if(k!=known.end() && !k->second.empty()) {
        std::lock_guard<std::mutex> hold(TagLock);
        return Tags[profile]=k->second;
    }
    { std::lock_guard<std::mutex> hold(TagLock); if(Tags.count(profile)) return Tags[profile]; }
    std::vector<unsigned char> package;
    std::string tag;
    if(ftp.Retrieve((std::string(ContentFolder)+"/"+profile+"/"+ProfileFolder+"/"+profile).c_str(),package,4<<20)) tag=Gamertag(package);
    if(tag.empty()) return tag;
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
// A file as the listings show it: its size and date (FtpDll gives minutes).
std::string Seen(const nia::FtpEntry& entry) { return std::to_string((unsigned long long)entry.size)+" "+entry.stamp; }
// What the listings show of a game's saves ("<XUID>/<file>": Seen) as a
// short digest (64-bit FNV-1a, in hex); "" for no files.
std::string Digest(const std::map<std::string,std::string>& listed) {
    if(listed.empty()) return std::string();
    unsigned long long h=1469598103934665603ULL;
    for(std::map<std::string,std::string>::const_iterator f=listed.begin();f!=listed.end();++f) {
        std::string line=f->first+"\n"+f->second+"\n";
        for(size_t i=0;i<line.size();++i) { h^=(unsigned char)line[i]; h*=1099511628211ULL; }
    }
    char text[17]; snprintf(text,sizeof(text),"%016llX",h);
    return text;
}
// Whether backup name in folder came from console on computer (its
// save.ini's console= and computer= lines), so it's that console's own. With
// no console said, every backup is. One that says nothing of where it came
// from (made before backups said) is no console's.
bool FromHere(const std::string& folder, const std::string& name, const std::string& console, const std::string& computer) {
    if(console.empty()) return true;
    std::string ini;
    return IniOf(folder,name,ini) && Value(ini,"console")==console && Value(ini,"computer")==computer;
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
          Result& result, std::string& error, const std::map<std::string,std::string>& known, const std::string& extra,
          const std::set<std::string>& spare) {
    result=Result(); result.profiles=0; result.files=0; result.bytes=0; result.same=false;
    std::vector<std::string> profiles;
    if(!Profiles(ftp,profiles,error)) return false;
    std::map<std::string,std::string> files, listed;
    std::string ini="[Save]\r\ntitle_id="+Hex(titleId)+"\r\ngame="+game+"\r\n";
    long long now=(long long)time(NULL);
    ini+="time="+std::to_string(now)+"\r\n"+extra;
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
            // Too much by the listing's size: told before downloading it.
            if(entries[i].size>U64(MaxTotal-total)) { error=TooBig; return false; }
            std::vector<unsigned char> data;
            if(!ftp.Retrieve((saveFolder+"/"+entries[i].name).c_str(),data,MaxTotal-total)) { error=ftp.Error(); return false; }
            total+=data.size();
            const std::string key=Upper(profiles[p])+"/"+entries[i].name;
            files[key].assign(data.begin(),data.end());
            listed[key]=Seen(entries[i]);
            ++result.files;
        }
        if(result.files==before) continue;
        ++result.profiles;
        std::string tag=TagOf(ftp,profiles[p],known);
        ini+="profile_"+Upper(profiles[p])+"="+tag+"\r\n";
    }
    result.bytes=(long long)total;
    result.listing=Digest(listed);
    if(!result.files) return true;
    // What the listings showed of these saves, so a look at them later (no
    // download) tells whether they changed since this backup.
    ini+="listing="+result.listing+"\r\n";
    files["save.ini"]=ini;
    const std::string console=Value(ini,"console"), computer=Value(ini,"computer");
    std::vector<Backup> before=List(folder);
    for(size_t i=0;i<before.size();++i) {
        // Compared with this console's newest: another console's newest
        // being the same doesn't make one of this console's.
        if(before[i].titleId!=titleId || !FromHere(folder,before[i].name,console,computer)) continue;
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
    // The oldest of this game's backups from this console beyond keep go:
    // each console keeps its own newest few, so two consoles playing the same
    // game don't push out each other's. One a choice still waiting on the
    // dashboard needs (spare) always stays, and counts for nothing.
    std::vector<Backup> all=List(folder);
    int kept=0;
    for(size_t i=0;i<all.size();++i)
        if(all[i].titleId==titleId && !spare.count(all[i].name) && FromHere(folder,all[i].name,console,computer) && ++kept>keep)
            config::RemoveFile(Path(folder,all[i].name));
    return true;
}

bool Listings(nia::FtpClient& ftp, const std::set<DWORD>& titles, std::map<DWORD,std::string>& out, std::string& error) {
    out.clear();
    std::vector<std::string> profiles;
    if(!Profiles(ftp,profiles,error)) return false;
    std::map<DWORD,std::map<std::string,std::string> > listed;
    for(size_t p=0;p<profiles.size();++p) {
        std::string profileFolder=std::string(ContentFolder)+"/"+profiles[p];
        std::vector<nia::FtpEntry> games, entries;
        if(!ftp.List(profileFolder.c_str(),games)) { error=ftp.Error(); return false; }
        for(size_t g=0;g<games.size();++g) {
            if(!games[g].folder || !HexName(games[g].name,8)) continue;
            DWORD title=DWORD(strtoul(games[g].name.c_str(),NULL,16));
            if(!titles.count(title)) continue;
            std::string saveFolder=profileFolder+"/"+games[g].name+"/"+SaveType;
            // A game's folder with no saves (only its DLC, say) keeps the connection.
            if(!ftp.Enters(saveFolder.c_str())) {
                if(ftp.Code()/100==5) continue;
                error=ftp.Error(); return false;
            }
            if(!ftp.List(saveFolder.c_str(),entries)) { error=ftp.Error(); return false; }
            for(size_t i=0;i<entries.size();++i)
                if(!entries[i].folder) listed[title][Upper(profiles[p])+"/"+entries[i].name]=Seen(entries[i]);
        }
    }
    for(std::map<DWORD,std::map<std::string,std::string> >::const_iterator l=listed.begin();l!=listed.end();++l)
        if(!l->second.empty()) out[l->first]=Digest(l->second);
    return true;
}

bool Describe(const std::string& folder, const std::string& name, std::string& game, std::vector<std::string>& profiles) {
    std::vector<std::string> names;
    std::string ini;
    if(!backup::Peek(Path(folder,name),names,"save.ini",ini,1<<20)) return false;
    game=Value(ini,"game");
    for(size_t i=0;i<names.size();++i) {
        size_t slash=names[i].find('/');
        if(slash==std::string::npos) continue;
        std::string profile=names[i].substr(0,slash), tag=Value(ini,"profile_"+profile);
        std::string shown=tag.empty() ? profile : tag;
        if(std::find(profiles.begin(),profiles.end(),shown)==profiles.end()) profiles.push_back(shown);
    }
    return true;
}

bool PutFiles(nia::FtpClient& ftp, const std::map<std::string,std::string>& in, const std::string& only, int& written, std::string& error,
              bool exact, std::vector<std::string>* put) {
    written=0;
    if(put) put->clear();
    std::map<std::string,std::string> files=in;
    if(!files.count("save.ini")) { error="That isn't a save backup."; return false; }
    std::string title=Upper(Value(files["save.ini"],"title_id"));
    if(!HexName(title,8)) { error="That save backup names no Title ID."; return false; }
    std::vector<std::string> profiles;
    if(!Profiles(ftp,profiles,error)) return false;
    std::map<std::string,std::set<std::string> > made;  // Each save folder written to: the files put there (upper case).
    for(std::map<std::string,std::string>::const_iterator f=files.begin();f!=files.end();++f) {
        size_t slash=f->first.find('/');
        if(slash==std::string::npos) continue;
        std::string profile=f->first.substr(0,slash), name=f->first.substr(slash+1), there;
        if(!HexName(profile,16) || name.empty() || name.find_first_of("/\\")!=std::string::npos || name==".." || name==".") continue;
        if(!only.empty() && Upper(profile)!=Upper(only)) continue;
        bool known=false;
        for(size_t i=0;i<profiles.size() && !known;++i) if(Upper(profiles[i])==profile) { known=true; there=profiles[i]; }
        if(!known) continue;
        std::string folder=std::string(ContentFolder)+"/"+there+"/"+title+"/"+SaveType;
        if(!made.count(folder)) {
            ftp.MakeFolder((std::string(ContentFolder)+"/"+there+"/"+title).c_str());
            if(!ftp.MakeFolder(folder.c_str())) { error=ftp.Error(); return false; }
            made[folder];
            if(put) put->push_back(Upper(profile));
        }
        if(!ftp.Store((folder+"/"+name).c_str(),f->second.data(),f->second.size())) { error=ftp.Error(); return false; }
        made[folder].insert(Upper(name));
        ++written;
    }
    if(!written) { error="None of the profiles in that backup are on this console."; return false; }
    // The files there these saves don't have go, only once all of them are
    // there: a slot file newer than the saves put back would stay beside them.
    for(std::map<std::string,std::set<std::string> >::const_iterator m=made.begin();exact && m!=made.end();++m) {
        std::vector<nia::FtpEntry> entries;
        if(!ftp.List(m->first.c_str(),entries)) { error=ftp.Error(); return false; }
        for(size_t i=0;i<entries.size();++i) {
            if(entries[i].folder || m->second.count(Upper(entries[i].name))) continue;
            if(!ftp.Delete((m->first+"/"+entries[i].name).c_str())) { error=ftp.Error(); return false; }
        }
    }
    return true;
}

bool ProfilesOn(nia::FtpClient& ftp, std::vector<std::string>& xuids, std::string& error) {
    std::vector<std::string> profiles;
    if(!Profiles(ftp,profiles,error)) return false;
    xuids.clear();
    for(size_t i=0;i<profiles.size();++i) xuids.push_back(Upper(profiles[i]));
    return true;
}

bool Collect(nia::FtpClient& ftp, DWORD titleId, std::map<std::string,std::string>& files, std::string& error) {
    files.clear();
    std::vector<std::string> profiles;
    if(!Profiles(ftp,profiles,error)) return false;
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
        for(size_t i=0;i<entries.size();++i) {
            if(entries[i].folder) continue;
            if(entries[i].size>U64(MaxTotal-total)) { error=TooBig; return false; }
            std::vector<unsigned char> data;
            if(!ftp.Retrieve((saveFolder+"/"+entries[i].name).c_str(),data,MaxTotal-total)) { error=ftp.Error(); return false; }
            total+=data.size();
            files[Upper(profiles[p])+"/"+entries[i].name].assign(data.begin(),data.end());
        }
    }
    return true;
}

std::string ProfileSaves(const std::map<std::string,std::string>& files, const std::string& xuid) {
    std::string out, prefix=Upper(xuid)+"/";
    for(std::map<std::string,std::string>::const_iterator f=files.begin();f!=files.end();++f) {
        if(Upper(f->first.substr(0,prefix.size()))!=prefix) continue;
        std::string name=f->first.substr(prefix.size());
        out+=std::to_string(name.size())+":"+name+std::to_string(f->second.size())+":"+f->second;
    }
    return out;
}

bool Read(const std::string& folder, const std::string& name, std::map<std::string,std::string>& files, std::string& error) {
    std::string data;
    files.clear();
    if(!Named(name) || !config::ReadFile(Path(folder,name),data,MaxTotal+(16<<20))) { error=name+" couldn't be read."; return false; }
    if(!backup::Unpack(data,files,error,MaxFile)) return false;
    if(!files.count("save.ini")) { error=name+" isn't a save backup."; return false; }
    return true;
}

bool IniOf(const std::string& folder, const std::string& name, std::string& ini) {
    std::vector<std::string> names;
    return Named(name) && backup::Peek(Path(folder,name),names,"save.ini",ini,1<<20);
}

std::string IniValue(const std::string& ini, const std::string& key) { return Value(ini,key); }

bool Copy(const std::string& folder, const std::string& name, const std::string& drop, const std::string& extra, const std::string& only,
          std::string& made, std::string& error) {
    std::map<std::string,std::string> files;
    if(!Read(folder,name,files,error)) return false;
    const std::string prefix=Upper(only)+"/";
    if(!only.empty()) {
        for(std::map<std::string,std::string>::iterator f=files.begin();f!=files.end();)
            if(f->first!="save.ini" && Upper(f->first.substr(0,prefix.size()))!=prefix) files.erase(f++);
            else ++f;
        if(files.size()<2) { error=name+" has none of that profile's saves."; return false; }
    }
    // save.ini without the drop key's line (and with only, the other
    // profiles' gamertags), with time and extra anew.
    std::string ini, old=files["save.ini"];
    long long now=(long long)time(NULL);
    for(size_t at=0;at<old.size();) {
        size_t end=old.find('\n',at); if(end==std::string::npos) end=old.size();
        std::string line=old.substr(at,end-at); at=end+1;
        if(!line.empty() && line[line.size()-1]=='\r') line.erase(line.size()-1);
        if(line.empty() || (!drop.empty() && !line.compare(0,drop.size()+1,drop+"=")) || !line.compare(0,5,"time=")) continue;
        if(!only.empty() && !line.compare(0,8,"profile_") && Upper(line.substr(8,16))!=Upper(only)) continue;
        ini+=line+"\r\n";
    }
    ini+="time="+std::to_string(now)+"\r\n"+extra;
    files["save.ini"]=ini;
    std::vector<Backup> before=List(folder);
    for(time_t t=time_t(now);;++t) {
        char stamp[32]; struct tm local;
#ifdef _WIN32
        localtime_s(&local,&t);
#else
        localtime_r(&t,&local);
#endif
        strftime(stamp,sizeof(stamp),"%Y-%m-%d_%H-%M-%S",&local);
        made=name.substr(0,8)+"_"+stamp+".zip";
        bool taken=false;
        for(size_t i=0;i<before.size() && !taken;++i) taken=before[i].name==made;
        if(!taken) break;
    }
    if(!config::WriteFile(Path(folder,made),backup::Zip(files,now),false)) { error="Can't write "+Path(folder,made)+"."; return false; }
    return true;
}
}
