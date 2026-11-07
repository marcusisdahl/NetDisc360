// SPDX-License-Identifier: MIT
#include "installed.h"
#include "discs.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

namespace nia {
namespace {
// default.xex's start holds its header, and so the execution info, for
// almost every game; a longer header is read again whole, up to HeaderMost.
// A Games on Demand header file has its IDs in its first 0x368 bytes.
enum { XexHead = 0x10000, HeaderMost = 0x100000, GodHead = 0x400, Depth = 2 };
const char ContentFolder[] = "/Content/0000000000000000";
const char GodType[] = "00007000";

std::string Lower(std::string text) {
    for(size_t i=0;i<text.size();++i) text[i]=char(tolower((unsigned char)text[i]));
    return text;
}
// Whether path is folder or inside it, ignoring case as FATX does.
bool Inside(const std::string& path, const std::string& folder) {
    std::string p=Lower(path), f=Lower(folder);
    return !f.empty() && (p==f || (p.size()>f.size() && !p.compare(0,f.size(),f) && p[f.size()]=='/'));
}
bool TitleFolder(const std::string& name) {
    if(name.size()!=8) return false;
    for(size_t i=0;i<name.size();++i) if(!isxdigit((unsigned char)name[i])) return false;
    return true;
}
DWORD Big32(const unsigned char* p) { return (DWORD(p[0])<<24)|(DWORD(p[1])<<16)|(DWORD(p[2])<<8)|p[3]; }
bool InsideAny(const std::string& path, const std::vector<std::string>& folders) {
    for(size_t i=0;i<folders.size();++i) if(Inside(path,folders[i])) return true;
    return false;
}
const char GodTilePrefix[] = "4E4433";  // "ND3".
}

std::string GodTileName(DWORD mediaId, int disc) {
    char name[24]; _snprintf_s(name,sizeof(name),_TRUNCATE,"%s%02X%08X",GodTilePrefix,unsigned(disc)&0xFF,(unsigned)mediaId);
    return name;
}
bool IsGodTileName(const std::string& name, int* disc) {
    if(name.size()!=16 || _strnicmp(name.c_str(),GodTilePrefix,6)) return false;
    for(size_t i=0;i<name.size();++i) if(!isxdigit((unsigned char)name[i])) return false;
    if(disc) *disc=int(strtoul(name.substr(6,2).c_str(),NULL,16));
    return true;
}

std::vector<std::string> InstalledFolders(const std::string& setting) {
    std::vector<std::string> out;
    for(size_t at=0;at<=setting.size();) {
        size_t end=setting.find_first_of(";\r\n",at);
        if(end==std::string::npos) end=setting.size();
        std::string one=setting.substr(at,end-at);
        at=end+1;
        size_t a=one.find_first_not_of(" \t"), b=one.find_last_not_of(" \t");
        one=a==std::string::npos ? std::string() : one.substr(a,b-a+1);
        while(one.size()>1 && one[one.size()-1]=='/') one.erase(one.size()-1);
        if(one.size()<2 || one[0]!='/') continue;
        bool listed=false;
        for(size_t i=0;i<out.size() && !listed;++i) listed=Lower(out[i])==Lower(one);
        if(!listed) out.push_back(one);
    }
    return out;
}

bool InstalledGames::Ids(FtpClient& ftp, const std::string& path, const FtpEntry& file, bool god, DWORD& titleId, int& disc) {
    std::map<std::string,Seen>::const_iterator known=seen_.find(Lower(path));
    if(known!=seen_.end() && known->second.size==file.size && known->second.stamp==file.stamp) {
        titleId=known->second.titleId; disc=known->second.disc;
        return titleId!=0;
    }
    std::vector<unsigned char> head;
    DWORD mediaId=0;
    titleId=0; disc=0;
    if(!ftp.RetrieveHead(path.c_str(),head,god ? GodHead : XexHead)) return false;  // Tried again next time.
    bool found=false;
    if(god) found=!head.empty() && GodIds(&head[0],DWORD(head.size()),titleId,mediaId,disc);
    else if(path.size()>4 && Lower(path.substr(path.size()-4))==".xbe") {
        // An original Xbox game: the Title ID is in default.xbe's
        // certificate, within its headers; it's disc 1, as the server has it.
        DWORD headers=0;
        found=!head.empty() && XbeIds(&head[0],DWORD(head.size()),titleId,&headers);
        if(!found && headers>head.size() && headers<=HeaderMost && ftp.RetrieveHead(path.c_str(),head,headers))
            found=!head.empty() && XbeIds(&head[0],DWORD(head.size()),titleId);
        disc=1;
    } else if(head.size()>=24) {
        found=XexIds(&head[0],DWORD(head.size()),titleId,mediaId,disc);
        DWORD header=Big32(&head[8]);
        if(!found && !memcmp(&head[0],"XEX2",4) && header>head.size() && header<=HeaderMost && ftp.RetrieveHead(path.c_str(),head,header))
            found=head.size()>=24 && XexIds(&head[0],DWORD(head.size()),titleId,mediaId,disc);
    }
    if(!found) titleId=0;
    Seen seen={file.size,file.stamp,titleId,disc};
    seen_[Lower(path)]=seen;
    return found;
}

// A game is a folder with default.xex, or an original Xbox game's
// default.xbe; one with neither is looked into, depth folders down.
bool InstalledGames::Walk(FtpClient& ftp, const std::string& folder, int depth, const std::vector<std::string>& skip,
                          std::vector<InstalledGame>& out, std::string& error) {
    std::vector<FtpEntry> entries;
    if(!ftp.List(folder.c_str(),entries)) { error="couldn't list "+folder+": "+ftp.Error(); return false; }
    size_t xex=entries.size(), xbe=entries.size();
    for(size_t i=0;i<entries.size();++i) {
        if(entries[i].folder) continue;
        std::string name=Lower(entries[i].name);
        if(name=="default.xex") xex=i;
        else if(name=="default.xbe") xbe=i;
    }
    if(xex<entries.size() || xbe<entries.size()) {
        const FtpEntry& file=entries[xex<entries.size() ? xex : xbe];
        std::string path=folder+"/"+file.name;
        InstalledGame game;
        game.path=path; game.god=false;
        if(Ids(ftp,path,file,false,game.titleId,game.disc)) out.push_back(game);
        return true;
    }
    for(size_t i=0;i<entries.size() && depth>0;++i) {
        std::string child=folder+"/"+entries[i].name;
        if(!entries[i].folder || InsideAny(child,skip)) continue;
        if(!Walk(ftp,child,depth-1,skip,out,error)) return false;
    }
    return true;
}

bool InstalledGames::Gods(FtpClient& ftp, const std::string& drive, std::vector<InstalledGame>& out, std::vector<InstalledGame>& tiles, std::string& error) {
    std::string content="/"+drive+ContentFolder;
    std::vector<FtpEntry> titles, files;
    if(!ftp.Enters(content.c_str())) {
        if(ftp.Code()/100==5) return true;  // No Content folder there.
        error="couldn't open "+content+": "+ftp.Error();
        return false;
    }
    if(!ftp.List(content.c_str(),titles)) { error="couldn't list "+content+": "+ftp.Error(); return false; }
    for(size_t t=0;t<titles.size();++t) {
        if(!titles[t].folder || !TitleFolder(titles[t].name)) continue;
        // Most Title ID folders hold saves, updates or add-ons, not a game:
        // trying to go in costs no listing.
        std::string packages=content+"/"+titles[t].name+"/"+GodType;
        if(!ftp.Enters(packages.c_str())) {
            if(ftp.Code()/100==5) continue;
            error="couldn't open "+packages+": "+ftp.Error();
            return false;
        }
        if(!ftp.List(packages.c_str(),files)) { error="couldn't list "+packages+": "+ftp.Error(); return false; }
        for(size_t f=0;f<files.size();++f) {
            if(files[f].folder) continue;  // A package's .data folder.
            InstalledGame game;
            game.path=packages+"/"+files[f].name; game.god=true;
            // NetDisc360's own tile of a game (see GodTileName) is no copy of it.
            if(IsGodTileName(files[f].name,&game.disc)) { game.titleId=DWORD(strtoul(titles[t].name.c_str(),NULL,16)); tiles.push_back(game); }
            else if(Ids(ftp,game.path,files[f],true,game.titleId,game.disc)) out.push_back(game);
        }
    }
    return true;
}

bool InstalledGames::Find(FtpClient& ftp, const std::vector<std::string>& folders, const std::vector<std::string>& skip,
                          std::vector<InstalledGame>& found, std::vector<InstalledGame>& tiles, std::string& error) {
    std::vector<InstalledGame> out, own;
    std::vector<std::string> drives;
    bool ok=true;
    std::vector<std::string> all(folders);
    all.insert(all.end(),skip.begin(),skip.end());
    for(size_t i=0;i<all.size() && ok;++i) {
        const std::string& folder=all[i];
        if(i<folders.size() && !InsideAny(folder,skip)) {
            if(ftp.Enters(folder.c_str())) ok=Walk(ftp,folder,Depth,skip,out,error);
            else if(ftp.Code()/100!=5) { error="couldn't open "+folder+": "+ftp.Error(); ok=false; }
        }
        size_t slash=folder.find('/',1);
        std::string drive=folder.substr(1,slash==std::string::npos ? std::string::npos : slash-1);
        bool listed=false;
        for(size_t k=0;k<drives.size() && !listed;++k) listed=Lower(drives[k])==Lower(drive);
        if(!listed) drives.push_back(drive);
    }
    for(size_t i=0;i<drives.size() && ok;++i) ok=Gods(ftp,drives[i],out,own,error);
    if(ok) { last_=out; lastTiles_=own; }
    found=last_; tiles=lastTiles_;
    return ok;
}
}
