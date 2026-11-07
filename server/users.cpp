// SPDX-License-Identifier: MIT
// NetDisc360's users and users.ini (see users.h).
#include "users.h"
#include "party.h"
#include "web.h"
#include <algorithm>
#include <ctype.h>
#include <map>
#include <set>
#include <stdio.h>
#include <stdlib.h>

namespace nia {
namespace {
std::string Escape(const std::string& text) {
    std::string out;
    for(size_t i=0;i<text.size();++i) {
        if(text[i]=='%') out+="%25";
        else if(text[i]=='|') out+="%7C";
        else if(text[i]=='\r' || text[i]=='\n') out+=' ';
        else out+=text[i];
    }
    return out;
}
std::string Unescape(const std::string& text) {
    std::string out;
    for(size_t i=0;i<text.size();++i) {
        if(text[i]=='%' && i+2<text.size() && isxdigit((unsigned char)text[i+1]) && isxdigit((unsigned char)text[i+2])) {
            out+=char(strtol(text.substr(i+1,2).c_str(),NULL,16));
            i+=2;
        } else out+=text[i];
    }
    return out;
}
std::vector<std::string> Split(const std::string& text, char by) {
    std::vector<std::string> out;
    size_t at=0;
    for(;;) {
        size_t end=text.find(by,at);
        out.push_back(text.substr(at,end==std::string::npos ? std::string::npos : end-at));
        if(end==std::string::npos) return out;
        at=end+1;
    }
}
std::string Number(long long n) { char text[32]; snprintf(text,sizeof(text),"%lld",n); return text; }
bool Xuid(const std::string& text, std::string& out) {
    if(text.size()!=16 || toupper((unsigned char)text[0])!='E') return false;
    out.clear();
    for(size_t i=0;i<text.size();++i) {
        if(!isxdigit((unsigned char)text[i])) return false;
        out+=char(toupper((unsigned char)text[i]));
    }
    return true;
}
// The lines of a key=value file, by key (the first of each), in order.
std::vector<std::pair<std::string,std::string> > Lines(const std::string& text) {
    std::vector<std::pair<std::string,std::string> > out;
    std::set<std::string> seen;
    size_t at=0;
    if(text.compare(0,3,"\xEF\xBB\xBF")==0) at=3;
    while(at<text.size()) {
        size_t end=text.find('\n',at);
        std::string line=text.substr(at,end==std::string::npos ? std::string::npos : end-at);
        at=end==std::string::npos ? text.size() : end+1;
        if(!line.empty() && line[line.size()-1]=='\r') line.erase(line.size()-1);
        if(line.empty() || line[0]==';' || line[0]=='#' || line[0]=='[') continue;
        size_t eq=line.find('=');
        if(eq==std::string::npos || !eq) continue;
        std::string key=line.substr(0,eq);
        if(seen.insert(key).second) out.push_back(std::make_pair(key,line.substr(eq+1)));
    }
    return out;
}
}

std::string NewUserId() { return "u"+web::NewSecret().substr(0,UserIdLength-1); }

bool IsUserId(const std::string& text) {
    if(text.size()!=size_t(UserIdLength) || text[0]!='u') return false;
    for(size_t i=1;i<text.size();++i) if(!isxdigit((unsigned char)text[i]) || isupper((unsigned char)text[i])) return false;
    return true;
}

std::string UserContent(const User& u) {
    return u.id+"|"+Escape(u.name)+"|"+Escape(u.gamertag)+"|"+u.xuid+"|"+Number(u.made)+"|"+(u.adopted ? "1" : "0")+"|"+Number(u.removed)+"|"+
           Number(u.gamerscore)+"|"+Number(u.achievements)+"|"+Number(u.readAt)+"|"+Number(u.playSeconds)+"|"+Number(u.plays)+"|"+
           Number(u.parties)+"|"+Number(u.lastParty)+"|"+Escape(u.lastGame);
}

std::string UsersText(const std::vector<User>& users) {
    std::string text="; NetDisc360's users: everyone it knows, with their Xbox profile (see server/users.h).\r\n"
                     "; Shared between computers: one line per user, the one changed last wins.\r\n[Users]\r\n";
    size_t count=std::min(users.size(),size_t(UsersMost));
    text+="count="+Number((long long)count)+"\r\n";
    for(size_t i=0;i<count;++i) {
        const User& u=users[i];
        text+="user"+Number((long long)i+1)+"="+u.id+"|"+Escape(u.name)+"|"+Escape(u.gamertag)+"|"+u.xuid+"|"+Number(u.made)+"|"+(u.adopted ? "1" : "0")+"|"+
              Number(u.changed)+"|"+Number(u.removed)+"|"+Number(u.gamerscore)+"|"+Number(u.achievements)+"|"+Number(u.readAt)+"|"+
              Number(u.playSeconds)+"|"+Number(u.plays)+"|"+Number(u.parties)+"|"+Number(u.lastParty)+"|"+Escape(u.lastGame)+"\r\n";
    }
    return text;
}

void ReadUsers(const std::string& text, std::vector<User>& users) {
    users.clear();
    std::map<std::string,std::string> values;
    std::vector<std::pair<std::string,std::string> > lines=Lines(text);
    for(size_t i=0;i<lines.size();++i) values.insert(lines[i]);
    long long count=atoll(values["count"].c_str());
    if(count>UsersMost) count=UsersMost;
    std::set<std::string> ids, xuids;
    for(long long n=1;n<=count;++n) {
        std::map<std::string,std::string>::const_iterator line=values.find("user"+Number(n));
        if(line==values.end()) continue;
        std::vector<std::string> parts=Split(line->second,'|');
        if(parts.size()<8) continue;
        User u;
        u.id=parts[0];
        u.name=Unescape(parts[1]);
        u.gamertag=Unescape(parts[2]);
        if(!IsUserId(u.id) || ids.count(u.id)) continue;
        if(!parts[3].empty() && (!Xuid(parts[3],u.xuid) || xuids.count(u.xuid))) continue;
        u.made=atoll(parts[4].c_str());
        u.adopted=parts[5]=="1";
        u.changed=atoll(parts[6].c_str());
        u.removed=atoll(parts[7].c_str());
        if(parts.size()>=16) {
            u.gamerscore=atoi(parts[8].c_str()); u.achievements=atoi(parts[9].c_str());
            u.readAt=atoll(parts[10].c_str()); u.playSeconds=atoll(parts[11].c_str());
            u.plays=atoi(parts[12].c_str()); u.parties=atoi(parts[13].c_str());
            u.lastParty=atoll(parts[14].c_str()); u.lastGame=Unescape(parts[15]);
        }
        // A user who isn't removed has a name.
        if(u.name.empty() && !u.removed) continue;
        ids.insert(u.id);
        if(!u.xuid.empty()) xuids.insert(u.xuid);
        users.push_back(u);
    }
}

User* UserNamed(std::vector<User>& users, const std::string& name) {
    for(size_t i=0;i<users.size();++i) if(!users[i].removed && SameName(users[i].name,name)) return &users[i];
    return NULL;
}
User* UserWithXuid(std::vector<User>& users, const std::string& xuid) {
    if(xuid.empty()) return NULL;
    for(size_t i=0;i<users.size();++i) if(users[i].xuid==xuid) return &users[i];
    return NULL;
}
User* UserWithId(std::vector<User>& users, const std::string& id) {
    for(size_t i=0;i<users.size();++i) if(users[i].id==id) return &users[i];
    return NULL;
}

std::vector<std::string> MergeUsers(std::vector<User>& mine, const std::vector<User>& other, long long now) {
    std::set<std::string> changed;
    for(size_t i=0;i<other.size();++i) {
        const User& o=other[i];
        User* had=UserWithXuid(mine,o.xuid);
        if(!had) had=UserWithId(mine,o.id);
        if(!had) {
            // Another user here with that id has another profile: a new id for this one.
            User u=o;
            if(UserWithId(mine,u.id)) u.id=NewUserId();
            mine.push_back(u);
            changed.insert(u.id);
            continue;
        }
        if(o.changed<=had->changed) continue;
        std::string id=had->id;
        // Its profile stays, if the other side's line has none.
        std::string xuid=had->xuid, gamertag=had->gamertag;
        long long made=had->made;
        bool adopted=had->adopted;
        *had=o;
        had->id=id;
        if(had->xuid.empty() && !xuid.empty()) { had->xuid=xuid; had->gamertag=gamertag; had->made=made; had->adopted=adopted; }
        changed.insert(id);
    }
    // One name, two users (not removed).
    for(size_t i=0;i<mine.size();++i) {
        if(mine[i].removed) continue;
        for(size_t k=i+1;k<mine.size();++k) {
            if(mine[k].removed || !SameName(mine[i].name,mine[k].name)) continue;
            User& a=mine[i];
            User& b=mine[k];
            if(a.xuid.empty() || b.xuid.empty()) {
                // The same person typed on two computers: the one without a
                // profile (or the later id) goes, its stats to the other.
                bool aGoes=a.xuid.empty() && (!b.xuid.empty() || a.id>b.id);
                User& keep=aGoes ? b : a;
                User& gone=aGoes ? a : b;
                keep.gamerscore=std::max(keep.gamerscore,gone.gamerscore); keep.achievements=std::max(keep.achievements,gone.achievements);
                keep.readAt=std::max(keep.readAt,gone.readAt); keep.playSeconds=std::max(keep.playSeconds,gone.playSeconds);
                keep.plays=std::max(keep.plays,gone.plays); keep.parties=std::max(keep.parties,gone.parties);
                if(gone.lastParty>keep.lastParty) { keep.lastParty=gone.lastParty; }
                if(keep.lastGame.empty()) keep.lastGame=gone.lastGame;
                keep.changed=std::max(now,keep.changed+1);
                gone.removed=now; gone.changed=std::max(now,gone.changed+1);
                changed.insert(keep.id); changed.insert(gone.id);
                if(aGoes) break;
                continue;
            }
            // Two people, one name: the newer line gets a number.
            User& renamed=a.changed>b.changed || (a.changed==b.changed && a.id>b.id) ? a : b;
            std::string base=renamed.name;
            for(int n=2;;++n) {
                std::string name=base+" "+Number(n);
                if(!UserNamed(mine,name)) { renamed.name=name; break; }
            }
            renamed.changed=std::max(now,renamed.changed+1);
            changed.insert(renamed.id);
            if(&renamed==&a) break;
        }
    }
    return std::vector<std::string>(changed.begin(),changed.end());
}

std::string ProfileMarksText(const std::vector<ProfileMark>& marks) {
    std::string text="; NetDisc360's Xbox profiles on this console: XUID=user id|name|gamertag|made|adopted.\r\n"
                     "; The same XUID is the same person on every console. See NetDisc360's server/users.h.\r\n[Profiles]\r\n";
    for(size_t i=0;i<marks.size();++i)
        text+=marks[i].xuid+"="+marks[i].user+"|"+Escape(marks[i].name)+"|"+Escape(marks[i].gamertag)+"|"+Number(marks[i].made)+"|"+(marks[i].adopted ? "1" : "0")+"\r\n";
    return text;
}

void ReadProfileMarks(const std::string& text, std::vector<ProfileMark>& marks) {
    marks.clear();
    std::vector<std::pair<std::string,std::string> > lines=Lines(text);
    for(size_t i=0;i<lines.size();++i) {
        ProfileMark m;
        if(!Xuid(lines[i].first,m.xuid)) continue;
        std::vector<std::string> parts=Split(lines[i].second,'|');
        if(parts.size()<4) continue;
        m.user=parts[0]; m.name=Unescape(parts[1]); m.gamertag=Unescape(parts[2]); m.made=atoll(parts[3].c_str());
        m.adopted=parts.size()>=5 && parts[4]=="1";
        marks.push_back(m);
    }
}
}
