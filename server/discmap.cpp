// SPDX-License-Identifier: MIT
// The disc map (see discmap.h).
#include "discmap.h"
#include "config.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace nia {

// Games on more than one disc whose discs aren't what the discs themselves
// suggest. The Title IDs are checked against the discs or a title database;
// a game left out is still added, from what its discs carry.
static const KnownGame Known[]={
    // The install disc is disc 1; the game asks for the play disc when it's done.
    {0x545408A7,"Grand Theft Auto V","install;play"},
    // Disc 2 is Undead Nightmare, a game of its own.
    {0x5454082B,"Red Dead Redemption: Game of the Year Edition","own Red Dead Redemption;own Undead Nightmare"},
    // Disc 2 installs car packs; the game plays without them.
    {0x4D530910,"Forza Motorsport 4","play;content Car packs"},
};

const KnownGame* KnownGames(int& count) {
    count=int(sizeof(Known)/sizeof(Known[0]));
    return Known;
}

bool KnownRole(const std::string& role) {
    return role=="install" || role=="play" || role=="content" || role=="next" || role=="own";
}

static const char Header[]=
    "; NetDisc 360's disc map: what each disc of a game on more than one disc is.\r\n"
    "; The console uses it to say which disc goes in, and the dashboard shows it.\r\n"
    ";\r\n"
    "; One [Title ID] section per game, and one line per disc: disc<N>=<role> <the disc's own name>\r\n"
    ";   install  installs content, then the game asks for the play disc\r\n"
    ";   play     the disc you play, which needs the install disc's content\r\n"
    ";   content  optional content to install; the game plays without it\r\n"
    ";   next     the game goes on from the disc before and asks for this one\r\n"
    ";   own      a game of its own that never asks for another disc\r\n"
    ";\r\n"
    "; NetDisc 360 adds each game it finds once and never changes it after, so\r\n"
    "; change any line here. Delete a game's section to have it added again.\r\n";

static std::string Trim(const std::string& text) {
    size_t start=0,end=text.size();
    while(start<end && isspace((unsigned char)text[start])) ++start;
    while(end>start && isspace((unsigned char)text[end-1])) --end;
    return text.substr(start,end-start);
}
static std::string Lower(std::string text) {
    for(size_t i=0;i<text.size();++i) text[i]=char(tolower((unsigned char)text[i]));
    return text;
}
// "own Undead Nightmare": the role, and the disc's name after it.
static MapDisc ParseDisc(const std::string& value) {
    MapDisc disc;
    size_t space=value.find_first_of(" \t");
    disc.role=Lower(value.substr(0,space));
    if(space!=std::string::npos) disc.label=Trim(value.substr(space));
    return disc;
}
static bool ParseTitleId(const std::string& text, DWORD& titleId) {
    if(text.size()!=8) return false;
    for(size_t i=0;i<text.size();++i) if(!isxdigit((unsigned char)text[i])) return false;
    titleId=DWORD(strtoul(text.c_str(),NULL,16));
    return titleId!=0;
}

static void Parse(const std::string& text, std::map<DWORD,MapGame>& games, std::vector<std::string>& problems) {
    DWORD current=0;
    int number=0;
    for(size_t at=0;at<text.size();) {
        size_t end=text.find('\n',at);
        if(end==std::string::npos) end=text.size();
        std::string line=Trim(text.substr(at,end-at));
        at=end+1; ++number;
        if(line.empty() || line[0]==';' || line[0]=='#') continue;
        char prefix[32]; snprintf(prefix,sizeof(prefix),"line %d: ",number);
        std::string where=prefix;
        if(line[0]=='[') {
            size_t close=line.find(']');
            std::string id=Trim(line.substr(1,close==std::string::npos ? std::string::npos : close-1));
            current=0;
            if(close==std::string::npos || !ParseTitleId(id,current)) problems.push_back(where+line+" isn't a Title ID (8 hex digits in [ ])");
            else games[current];
            continue;
        }
        if(!current) { problems.push_back(where+line+" isn't under a [Title ID]"); continue; }
        size_t equals=line.find('=');
        std::string key=Lower(Trim(line.substr(0,equals))),value=equals==std::string::npos ? "" : Trim(line.substr(equals+1));
        if(key=="name") { games[current].name=value; continue; }
        int disc=key.size()==5 && !key.compare(0,4,"disc") && key[4]>='1' && key[4]<='9' ? key[4]-'0' : 0;
        if(!disc || equals==std::string::npos) { problems.push_back(where+line+" isn't name= or disc1= to disc9="); continue; }
        MapDisc parsed=ParseDisc(value);
        if(!KnownRole(parsed.role)) { problems.push_back(where+"\""+parsed.role+"\" isn't a role (install, play, content, next or own)"); continue; }
        games[current].discs[disc]=parsed;
    }
}

void DiscMap::Load(const std::string& path, std::vector<std::string>& problems) {
    std::string text;
    if(!path.empty()) {
        FILE* file=config::Open(path,"rb");
        if(file) {
            char buffer[4096]; size_t got;
            while((got=fread(buffer,1,sizeof(buffer),file))>0) text.append(buffer,got);
            fclose(file);
        }
    }
    // Without a file, what was learned stays for as long as the server runs.
    if(path==path_ && text==text_) return;
    path_=path; text_=text;
    std::map<DWORD,MapGame> games;
    Parse(text,games,problems);
    games_.swap(games);
}

// What the discs of a game that the built-in list doesn't have suggest: a
// disc carrying packages for the game is its install disc when it comes
// first (as GTA V's does), else optional content (as Forza 4's car packs);
// discs without are played. With no packages, the game goes on from disc to
// disc, as most games on more than one disc do.
static bool Guess(const std::vector<const IsoFile*>& set, MapGame& game) {
    std::map<int,const IsoFile*> discs;
    for(size_t i=0;i<set.size();++i) if(set[i]->disc>0 && set[i]->disc<=9 && !discs.count(set[i]->disc)) discs[set[i]->disc]=set[i];
    if(discs.size()<2) return false;
    int carrier=0,carriers=0,firstOther=0;
    for(std::map<int,const IsoFile*>::const_iterator d=discs.begin();d!=discs.end();++d)
        if(!d->second->packages.empty()) { carrier=d->first; ++carriers; }
    for(std::map<int,const IsoFile*>::const_iterator d=discs.begin();d!=discs.end() && !firstOther;++d)
        if(d->first!=carrier) firstOther=d->first;
    for(int n=1;n<=discs.rbegin()->first;++n) {
        MapDisc& disc=game.discs[n];
        if(carriers!=1) disc.role="next";
        else if(n!=carrier) disc.role="play";
        else disc.role=carrier<firstOther ? "install" : "content";
    }
    std::string name=discs.begin()->second->name;
    if(name.size()>4 && !Lower(name.substr(name.size()-4)).compare(".iso")) name.erase(name.size()-4);
    game.name=config::ToUtf8(name);
    return true;
}

static std::string Describe(DWORD titleId, const MapGame& game, const char* from) {
    char id[16]; snprintf(id,sizeof(id),"%08X",(unsigned)titleId);
    std::string line=game.name+" ("+id+"):";
    for(std::map<int,MapDisc>::const_iterator d=game.discs.begin();d!=game.discs.end();++d) {
        char disc[32]; snprintf(disc,sizeof(disc)," disc %d %s",d->first,d->second.role.c_str());
        line+=(d==game.discs.begin() ? "" : ",")+std::string(disc);
        if(!d->second.label.empty()) line+=" ("+d->second.label+")";
    }
    return line+", "+from;
}

void DiscMap::Learn(const std::vector<IsoFile>& discs, std::vector<std::string>& added, std::string& error) {
    std::map<DWORD,std::vector<const IsoFile*> > sets;
    for(size_t i=0;i<discs.size();++i) if(discs[i].titleId && !games_.count(discs[i].titleId)) sets[discs[i].titleId].push_back(&discs[i]);
    std::string sections;
    for(std::map<DWORD,std::vector<const IsoFile*> >::const_iterator s=sets.begin();s!=sets.end();++s) {
        MapGame game;
        bool known=false;
        for(size_t k=0;k<sizeof(Known)/sizeof(Known[0]) && !known;++k) {
            if(Known[k].titleId!=s->first) continue;
            game.name=Known[k].name;
            int n=0;
            for(const char* at=Known[k].discs;*at;) {
                const char* end=at+strcspn(at,";");
                game.discs[++n]=ParseDisc(std::string(at,end));
                at=*end ? end+1 : end;
            }
            known=true;
        }
        if(!known && !Guess(s->second,game)) continue;
        games_[s->first]=game;
        added.push_back(Describe(s->first,game,known ? "from the built-in list" : "guessed from the discs"));
        char id[16]; snprintf(id,sizeof(id),"%08X",(unsigned)s->first);
        sections+=std::string("\r\n[")+id+"]\r\n";
        sections+=known ? "; From NetDisc 360's list of known games.\r\n"
                        : "; NetDisc 360's guess from the discs it found; change it if it's wrong.\r\n";
        sections+="name="+game.name+"\r\n";
        for(std::map<int,MapDisc>::const_iterator d=game.discs.begin();d!=game.discs.end();++d) {
            char key[16]; snprintf(key,sizeof(key),"disc%d=",d->first);
            sections+=key+d->second.role+(d->second.label.empty() ? "" : " "+d->second.label)+"\r\n";
        }
    }
    if(sections.empty() || path_.empty()) return;
    std::string more=text_.empty() ? Header+sections
                                   : (text_[text_.size()-1]=='\n' ? "" : "\r\n")+sections;
    FILE* file=config::Open(path_,"ab");
    bool written=file && fwrite(more.data(),1,more.size(),file)==more.size();
    if(file && fclose(file)!=0) written=false;
    if(written) text_+=more;
    else error="couldn't write "+config::FromUtf8(path_);
}

void DiscMap::Apply(std::vector<IsoFile>& discs) const {
    for(size_t i=0;i<discs.size();++i) {
        IsoFile& d=discs[i];
        d.role.clear(); d.roleLabel.clear();
        std::map<DWORD,MapGame>::const_iterator g=games_.find(d.titleId);
        if(g==games_.end()) continue;
        std::map<int,MapDisc>::const_iterator disc=g->second.discs.find(d.disc);
        if(disc==g->second.discs.end()) continue;
        d.role=disc->second.role; d.roleLabel=disc->second.label;
    }
}
}
