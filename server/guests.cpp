// SPDX-License-Identifier: MIT
// The LAN party's guest profiles (see guests.h).
#include "guests.h"
#include "party.h"
#include "../shared/platform.h"
#include <algorithm>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace nia {
namespace {
std::string Number(long long n) { char text[24]; _snprintf_s(text,sizeof(text),_TRUNCATE,"%lld",n); return text; }
std::string LowerAscii(std::string text) { for(size_t i=0;i<text.size();++i) text[i]=char(tolower((unsigned char)text[i])); return text; }

// The plain letters for a Latin-1 or Latin Extended-A letter; NULL for any
// other character.
const char* Plain(unsigned c) {
    static const char* latin1[64]={
        "A","A","A","A","A","A","Ae","C","E","E","E","E","I","I","I","I",     // C0-CF
        "D","N","O","O","O","O","O",NULL,"O","U","U","U","U","Y","Th","ss",   // D0-DF
        "a","a","a","a","a","a","ae","c","e","e","e","e","i","i","i","i",     // E0-EF
        "d","n","o","o","o","o","o",NULL,"o","u","u","u","u","y","th","y" };  // F0-FF
    if(c>=0xC0 && c<=0xFF) return latin1[c-0xC0];
    // Latin Extended-A in pairs (capital, small), by the plain letter.
    static const struct { unsigned from, to; char letter; } extended[]={
        {0x100,0x105,'a'},{0x106,0x10D,'c'},{0x10E,0x111,'d'},{0x112,0x11B,'e'},{0x11C,0x123,'g'},{0x124,0x127,'h'},
        {0x128,0x131,'i'},{0x134,0x135,'j'},{0x136,0x138,'k'},{0x139,0x142,'l'},{0x143,0x14B,'n'},{0x14C,0x151,'o'},
        {0x154,0x159,'r'},{0x15A,0x161,'s'},{0x162,0x167,'t'},{0x168,0x173,'u'},{0x174,0x175,'w'},{0x176,0x178,'y'},{0x179,0x17E,'z'} };
    static char one[2][2]={{0,0},{0,0}};
    for(size_t i=0;i<sizeof(extended)/sizeof(extended[0]);++i)
        if(c>=extended[i].from && c<=extended[i].to) {
            bool capital=c==0x178 || (c!=0x138 && ((c-extended[i].from)%2==0));
            one[capital][0]=capital ? char(toupper(extended[i].letter)) : extended[i].letter;
            return one[capital];
        }
    if(c==0x152) return "Oe";
    if(c==0x153) return "oe";
    return NULL;
}

// The next character of UTF-8 text at at, moving past it; a byte that
// doesn't start one is taken as Latin-1.
unsigned NextChar(const std::string& text, size_t& at) {
    unsigned char b=(unsigned char)text[at];
    int more=b<0x80 ? 0 : (b&0xE0)==0xC0 ? 1 : (b&0xF0)==0xE0 ? 2 : (b&0xF8)==0xF0 ? 3 : -1;
    if(more<=0 || at+size_t(more)>=text.size()+0) { ++at; return b; }
    unsigned c=more==1 ? (b&0x1F) : more==2 ? (b&0x0F) : (b&0x07);
    for(int i=1;i<=more;++i) {
        unsigned char n=(unsigned char)text[at+i];
        if((n&0xC0)!=0x80) { ++at; return b; }
        c=(c<<6)|(n&0x3F);
    }
    at+=size_t(more)+1;
    return c;
}

std::vector<std::string> Words(const std::string& name) {
    std::string plain;
    for(size_t at=0;at<name.size();) {
        unsigned c=NextChar(name,at);
        const char* letters;
        if(c<0x80 && isalnum(int(c))) plain+=char(c);
        else if((letters=Plain(c))) plain+=letters;
        else plain+=' ';
    }
    std::vector<std::string> words;
    for(size_t at=0;at<plain.size();) {
        size_t from=plain.find_first_not_of(' ',at);
        if(from==std::string::npos) break;
        size_t to=plain.find(' ',from);
        if(to==std::string::npos) to=plain.size();
        words.push_back(plain.substr(from,to-from));
        at=to;
    }
    return words;
}

bool Taken(const std::vector<std::string>& taken, const std::string& tag) {
    for(size_t i=0;i<taken.size();++i) if(LowerAscii(taken[i])==LowerAscii(tag)) return true;
    return false;
}

// Name lists in the settings file: % | , ; = written as %XX (as party.cpp does).
std::string Escaped(const std::string& name) {
    std::string out;
    for(size_t i=0;i<name.size();++i) {
        char c=name[i];
        if(c=='%' || c=='|' || c==',' || c==';' || c=='=') { char hex[4]; _snprintf_s(hex,sizeof(hex),_TRUNCATE,"%%%02X",(unsigned char)c); out+=hex; }
        else out+=c;
    }
    return out;
}
std::string Unescaped(const std::string& text) {
    std::string out;
    for(size_t i=0;i<text.size();++i) {
        if(text[i]=='%' && i+2<text.size() && isxdigit((unsigned char)text[i+1]) && isxdigit((unsigned char)text[i+2])) {
            out+=char(strtol(text.substr(i+1,2).c_str(),NULL,16));
            i+=2;
        } else out+=text[i];
    }
    return out;
}
std::vector<std::string> Split(const std::string& text, char at) {
    std::vector<std::string> out;
    for(size_t from=0;from<=text.size();) {
        size_t to=text.find(at,from);
        if(to==std::string::npos) to=text.size();
        out.push_back(text.substr(from,to-from));
        from=to+1;
    }
    return out;
}

// key=value lines (CR LF or LF); later ones win.
std::map<std::string,std::string> Lines(const std::string& text) {
    std::map<std::string,std::string> out;
    std::vector<std::string> lines=Split(text,'\n');
    for(size_t i=0;i<lines.size();++i) {
        std::string line=lines[i];
        if(!line.empty() && line[line.size()-1]=='\r') line.erase(line.size()-1);
        size_t eq=line.find('=');
        if(line.empty() || line[0]==';' || eq==std::string::npos) continue;
        out[line.substr(0,eq)]=line.substr(eq+1);
    }
    return out;
}

// One line of text for an .ini value: no line breaks.
std::string OneLine(std::string text) {
    for(size_t i=0;i<text.size();++i) if(text[i]=='\r' || text[i]=='\n') text[i]=' ';
    return text;
}

// ---- STFS and XDBF (big-endian; the STFS volume descriptor's own fields little-endian) ----
unsigned Be16(const std::string& d, size_t at) { return (unsigned((unsigned char)d[at])<<8)|(unsigned char)d[at+1]; }
unsigned Be24(const std::string& d, size_t at) { return (unsigned((unsigned char)d[at])<<16)|(unsigned((unsigned char)d[at+1])<<8)|(unsigned char)d[at+2]; }
unsigned Be32(const std::string& d, size_t at) { return (Be16(d,at)<<16)|Be16(d,at+2); }
unsigned Le16(const std::string& d, size_t at) { return unsigned((unsigned char)d[at])|(unsigned((unsigned char)d[at+1])<<8); }
unsigned Le24(const std::string& d, size_t at) { return Le16(d,at)|(unsigned((unsigned char)d[at+2])<<16); }

const size_t StfsBlock=0x1000, StfsEntry=0x40, HashEntry=0x18;
const unsigned PerTable=0xAA, PerLevel1=0x70E4;

// The files of an STFS package (a profile's), read as Velocity
// (XboxInternals, StfsPackage.cpp) and Free60's STFS page describe it.
class Stfs {
public:
    explicit Stfs(const std::string& data) : d_(data), ok_(false) {
        if(d_.size()<0xA000 || (d_.compare(0,4,"CON ") && d_.compare(0,4,"LIVE") && d_.compare(0,4,"PIRS"))) return;
        unsigned header=Be32(d_,0x340), separation=(unsigned char)d_[0x37B];
        first_=(size_t(header)+0xFFF)&~size_t(0xFFF);
        sex_=(~separation)&1;  // 0: one hash table per level, 1: two.
        step0_=sex_ ? 0xAC : 0xAB;
        tableBlocks_=Le16(d_,0x37C);
        tableStart_=Le24(d_,0x37E);
        allocated_=Be32(d_,0x395);
        separation_=separation;
        top_=allocated_<=PerTable ? 0 : allocated_<=PerLevel1 ? 1 : 2;
        ok_=top_<2 && tableBlocks_>0;
    }
    bool Ok() const { return ok_; }
    // The display name (the profile's gamertag), UTF-16 at 0x411, as UTF-8.
    std::string DisplayName() const {
        std::string out;
        for(size_t at=0x411;at+1<0x491 && at+1<d_.size();at+=2) {
            unsigned c=Be16(d_,at);
            if(!c) break;
            if(c<0x80) out+=char(c);
            else if(c<0x800) { out+=char(0xC0|(c>>6)); out+=char(0x80|(c&0x3F)); }
            else { out+=char(0xE0|(c>>12)); out+=char(0x80|((c>>6)&0x3F)); out+=char(0x80|(c&0x3F)); }
        }
        return out;
    }
    // The file called name in the package's root (any case); false if it isn't there.
    bool File(const char* name, std::string& out) const {
        if(!ok_) return false;
        std::vector<unsigned> blocks;
        if(!Chain(tableStart_,tableBlocks_,false,blocks)) return false;
        for(size_t b=0;b<blocks.size();++b) {
            size_t at;
            if(!Address(blocks[b],at)) return false;
            for(size_t e=0;e<StfsBlock/StfsEntry;++e) {
                size_t entry=at+e*StfsEntry;
                unsigned flags=(unsigned char)d_[entry+0x28], length=flags&0x3F;
                if(!length) continue;
                std::string fileName=d_.substr(entry,std::min<size_t>(length,0x28));
                if(flags&0x80 || Be16(d_,entry+0x32)!=0xFFFF || LowerAscii(fileName)!=LowerAscii(name)) continue;
                unsigned count=Le24(d_,entry+0x29), start=Le24(d_,entry+0x2F), size=Be32(d_,entry+0x34);
                std::vector<unsigned> fileBlocks;
                if(size>(64u<<20) || !Chain(start,count,(flags&0x40)!=0,fileBlocks)) return false;
                out.clear();
                for(size_t k=0;k<fileBlocks.size() && out.size()<size;++k) {
                    size_t from;
                    if(!Address(fileBlocks[k],from)) return false;
                    out.append(d_,from,std::min<size_t>(StfsBlock,size-out.size()));
                }
                return out.size()==size;
            }
        }
        return false;
    }
private:
    // Velocity's ComputeDataBlockNumber: the block's place counting the hash tables before it.
    unsigned DataBlock(unsigned block) const {
        unsigned n=(((block+PerTable)/PerTable)<<sex_)+block;
        if(block<PerTable) return n;
        if(block<PerLevel1) return n+(((block+PerLevel1)/PerLevel1)<<sex_);
        return (1u<<sex_)+n+(((block+PerLevel1)/PerLevel1)<<sex_);
    }
    bool Address(unsigned block, size_t& at) const {
        at=(size_t(DataBlock(block))<<12)+first_;
        return block<allocated_ && at+StfsBlock<=d_.size();
    }
    unsigned Level0Table(unsigned block) const {
        if(block<PerTable) return 0;
        unsigned n=(block/PerTable)*step0_+(((block/PerLevel1)+1)<<sex_);
        return block/PerLevel1==0 ? n : n+(1u<<sex_);
    }
    // Where the hash entry for block is, in the hash table in use.
    bool HashAt(unsigned block, size_t& at) const {
        at=(size_t(Level0Table(block))<<12)+first_+(block%PerTable)*HashEntry;
        if(top_==0) at+=size_t(separation_&2)<<0xB;
        else {
            // The level 1 table (only one with fewer than 0x70E4 blocks), in
            // use as the volume descriptor says; its entry for this level 0
            // table says which of that one's two is in use.
            size_t level1=(size_t(step0_)<<12)+first_+(size_t(separation_&2)<<0xB)+(block/PerTable)*HashEntry;
            if(level1+HashEntry>d_.size()) return false;
            at+=size_t((unsigned char)d_[level1+0x14]&0x40)<<6;
        }
        return at+HashEntry<=d_.size();
    }
    bool Chain(unsigned start, unsigned count, bool consecutive, std::vector<unsigned>& out) const {
        out.clear();
        unsigned block=start;
        for(unsigned i=0;i<count;++i) {
            if(block>=allocated_) return false;
            out.push_back(block);
            if(consecutive) { ++block; continue; }
            size_t at;
            if(!HashAt(block,at)) return false;
            block=Be24(d_,at+0x15);
        }
        return true;
    }
    const std::string& d_;
    bool ok_;
    size_t first_;
    unsigned sex_, step0_, tableBlocks_, tableStart_, allocated_, separation_, top_;
};

// The dashboard's record (an XDBF file): the gamerscore and achievements its
// titles count (namespace 4: title ID, achievements, unlocked, gamerscore,
// gamerscore unlocked), or its gamer card's settings (namespace 3,
// 0x10040006 gamerscore, 0x10040007 achievements) when it lists no titles.
bool GpdTotals(const std::string& g, int& gamerscore, int& achievements) {
    if(g.size()<0x18 || g.compare(0,4,"XDBF")) return false;
    unsigned tableLength=Be32(g,0x08), entries=Be32(g,0x0C), freeLength=Be32(g,0x10);
    size_t data=0x18+size_t(tableLength)*0x12+size_t(freeLength)*8;
    if(entries>tableLength || data>g.size()) return false;
    long long score=0, earned=0, cardScore=-1, cardEarned=-1;
    bool titles=false;
    for(unsigned i=0;i<entries;++i) {
        size_t e=0x18+size_t(i)*0x12;
        unsigned space=Be16(g,e), offset=Be32(g,e+0x0A), length=Be32(g,e+0x0E);
        size_t at=data+offset;
        if(at>g.size() || length>g.size()-at) continue;
        if(space==4 && length>=0x14) { titles=true; earned+=Be32(g,at+8); score+=Be32(g,at+0x10); }
        else if(space==3 && length>=0x14) {
            unsigned setting=Be32(g,at);
            if(setting==0x10040006) cardScore=Be32(g,at+0x10);
            else if(setting==0x10040007) cardEarned=Be32(g,at+0x10);
        }
    }
    if(!titles && cardScore<0 && cardEarned<0) return false;
    gamerscore=int(titles ? score : std::max(0LL,cardScore));
    achievements=int(titles ? earned : std::max(0LL,cardEarned));
    return true;
}

long long Listed(const config::File& f, const char* key) { return std::max(0LL,std::min((long long)GuestProfilesMost,f.Number(key,0))); }
}

std::string GuestGamertag(const std::string& name, const std::vector<std::string>& taken) {
    std::vector<std::string> words=Words(name);
    std::string tag;
    if(words.empty()) tag="Guest";
    else {
        tag=words[0].substr(0,GamertagLongest);
        // The rest whole while they fit; past that, their initials.
        bool whole=true;
        for(size_t i=1;i<words.size();++i) {
            if(whole && tag.size()+1+words[i].size()<=GamertagLongest) { tag+=" "+words[i]; continue; }
            whole=false;
            if(tag.size()+2<=GamertagLongest) tag+=" "+words[i].substr(0,1);
        }
    }
    if(!Taken(taken,tag)) return tag;
    for(int n=2;n<1000;++n) {
        std::string end=" "+Number(n), base=tag.substr(0,GamertagLongest-end.size());
        while(!base.empty() && base[base.size()-1]==' ') base.erase(base.size()-1);
        if(!Taken(taken,base+end)) return base+end;
    }
    return tag;
}

bool OfflineXuid(const std::string& text, std::string& xuid) {
    if(text.size()!=16 || toupper((unsigned char)text[0])!='E') return false;
    for(size_t i=0;i<16;++i) if(!isxdigit((unsigned char)text[i])) return false;
    xuid=text;
    for(size_t i=0;i<16;++i) xuid[i]=char(toupper((unsigned char)xuid[i]));
    return true;
}

std::string ProfilePictureFile(int n, int size) { return "guest"+Number(n)+(size==32 ? "_32" : "")+".png"; }

std::string ProfileJobText(const std::string& job, const std::vector<std::string>& gamertags, bool pictures, const std::string& extra) {
    std::string out="; Made by NetDisc360: the LAN party's guest profiles for the helper to make.\r\njob="+job+"\r\n";
    for(size_t i=0;i<gamertags.size();++i) {
        std::string n=Number((long long)i+1);
        out+="guest"+n+"="+OneLine(gamertags[i])+"\r\n";
        if(pictures) out+="picture"+n+"="+ProfilePictureFile(int(i)+1,64)+"\r\npicture_small"+n+"="+ProfilePictureFile(int(i)+1,32)+"\r\n";
    }
    return out+extra;
}

bool ReadProfileDone(const std::string& text, ProfileJobDone& done) {
    done=ProfileJobDone();
    std::map<std::string,std::string> lines=Lines(text);
    if(!lines.count("job")) return false;
    done.job=lines["job"];
    for(std::map<std::string,std::string>::const_iterator i=lines.begin();i!=lines.end();++i) {
        bool picture=!i->first.compare(0,7,"picture"), pictureError=!i->first.compare(0,13,"picture_error");
        if(picture) {
            size_t at=pictureError ? 13 : 7;
            int n=atoi(i->first.c_str()+at);
            if(n<1 || Number(n)!=i->first.substr(at)) continue;
            if(pictureError || i->second!="ok") done.pictureErrors[n]=i->second.empty() ? std::string("The helper couldn't give it its picture.") : i->second;
            else done.pictures.insert(n);
            continue;
        }
        bool guest=!i->first.compare(0,5,"guest"), error=!i->first.compare(0,5,"error");
        if(!guest && !error) continue;
        int n=atoi(i->first.c_str()+5);
        if(n<1 || Number(n)!=i->first.substr(5)) continue;
        std::string xuid;
        if(error) done.errors[n]=i->second.empty() ? std::string("The helper couldn't make it.") : i->second;
        else if(OfflineXuid(i->second,xuid)) done.xuids[n]=xuid;
        else done.errors[n]="The helper gave no XUID for it ("+i->second+").";
    }
    for(std::map<int,std::string>::const_iterator i=done.xuids.begin();i!=done.xuids.end();++i) done.errors.erase(i->first);
    for(std::set<int>::const_iterator i=done.pictures.begin();i!=done.pictures.end();++i) done.pictureErrors.erase(*i);
    return true;
}

std::string ContentFolderFtp(const std::string& xuid) { return "/Hdd1/Content/"+xuid; }
std::string ProfilePackageFtp(const std::string& xuid) { return ContentFolderFtp(xuid)+"/FFFE07D1/00010000/"+xuid; }

bool ReadProfileStats(const std::string& package, ProfileStats& stats) {
    stats=ProfileStats();
    Stfs stfs(package);
    if(package.size()<0x491 || (package.compare(0,4,"CON ") && package.compare(0,4,"LIVE") && package.compare(0,4,"PIRS"))) return false;
    stats.gamertag=stfs.DisplayName();
    std::string gpd;
    if(stfs.Ok() && stfs.File("FFFE07D1.gpd",gpd)) GpdTotals(gpd,stats.gamerscore,stats.achievements);
    return true;
}

std::string ProfileCopyName(long long time, const std::string& console) {
    time_t t=time_t(time);
    struct tm local;
    char stamp[32]="0000-00-00_00-00-00";
#ifdef _WIN32
    bool known=!localtime_s(&local,&t);
#else
    bool known=localtime_r(&t,&local)!=NULL;
#endif
    if(known) strftime(stamp,sizeof(stamp),"%Y-%m-%d_%H-%M-%S",&local);
    std::string safe;
    for(size_t i=0;i<console.size();++i) safe+=isalnum((unsigned char)console[i]) || console[i]=='.' || console[i]=='-' ? console[i] : '_';
    return std::string(stamp)+"_"+safe+".zip";
}

std::string ProfileCopyIni(const ProfileCopy& c) {
    return "; A guest profile's content folder from one console, kept by NetDisc360.\r\nfile="+OneLine(c.file)+"\r\nname="+OneLine(c.name)+
           "\r\ngamertag="+OneLine(c.gamertag)+"\r\nxuid="+c.xuid+"\r\nconsole="+OneLine(c.console)+"\r\ntime="+Number(c.time)+
           "\r\nsize="+Number(c.size)+"\r\ngamerscore="+Number(c.gamerscore)+"\r\nachievements="+Number(c.achievements)+"\r\n";
}

bool ReadProfileCopyIni(const std::string& text, ProfileCopy& c) {
    c=ProfileCopy();
    std::map<std::string,std::string> lines=Lines(text);
    if(lines["file"].empty() || !OfflineXuid(lines["xuid"],c.xuid)) return false;
    c.file=lines["file"]; c.name=lines["name"]; c.gamertag=lines["gamertag"]; c.console=lines["console"];
    c.time=atoll(lines["time"].c_str()); c.size=atoll(lines["size"].c_str());
    c.gamerscore=lines.count("gamerscore") ? atoi(lines["gamerscore"].c_str()) : -1;
    c.achievements=lines.count("achievements") ? atoi(lines["achievements"].c_str()) : -1;
    return true;
}

std::vector<ProfileCopy> ProfileCopies(const std::string& folder) {
    std::vector<ProfileCopy> out;
    std::vector<config::Entry> files=config::Files(folder);
    for(size_t i=0;i<files.size();++i) {
        const std::string& n=files[i].name;
        if(n.size()<5 || LowerAscii(n.substr(n.size()-4))!=".ini") continue;
        std::string text, sep=folder.empty() || folder[folder.size()-1]=='/' || folder[folder.size()-1]=='\\' ? "" : "/";
        ProfileCopy copy;
        if(!config::ReadFile(folder+sep+n,text,64*1024) || !ReadProfileCopyIni(text,copy)) continue;
        bool zip=false;
        for(size_t k=0;k<files.size() && !zip;++k) zip=files[k].name==copy.file;
        if(zip) out.push_back(copy);
    }
    struct Newest { bool operator()(const ProfileCopy& a, const ProfileCopy& b) const { return a.time!=b.time ? a.time>b.time : a.file>b.file; } };
    std::sort(out.begin(),out.end(),Newest());
    return out;
}

const ProfileCopy* BestProfileCopy(const std::vector<ProfileCopy>& copies) {
    const ProfileCopy* best=NULL;
    for(size_t i=0;i<copies.size();++i) {
        const ProfileCopy& c=copies[i];
        if(!best || c.gamerscore>best->gamerscore || (c.gamerscore==best->gamerscore && (c.achievements>best->achievements ||
           (c.achievements==best->achievements && c.time>best->time)))) best=&c;
    }
    return best;
}

void SaveGuestProfiles(config::File& f, const std::vector<GuestProfile>& profiles) {
    long long before=Listed(f,"guest_profiles");
    size_t count=std::min(profiles.size(),size_t(GuestProfilesMost));
    f.Set("guest_profiles",Number((long long)count));
    for(size_t i=0;i<count;++i) {
        const GuestProfile& p=profiles[i];
        std::string on;
        for(size_t k=0;k<p.on.size();++k) on+=(k ? "," : "")+Escaped(p.on[k]);
        f.Set(("guest_profile"+Number((long long)i+1)).c_str(),Escaped(p.name)+"|"+Escaped(p.gamertag)+"|"+p.xuid+"|"+Number(p.made)+"|"+Escaped(p.madeOn)+"|"+on);
    }
    for(long long i=(long long)count+1;i<=before;++i) f.Remove(("guest_profile"+Number(i)).c_str());
}

void LoadGuestProfiles(const config::File& f, std::vector<GuestProfile>& profiles) {
    profiles.clear();
    for(long long i=1,n=Listed(f,"guest_profiles");i<=n;++i) {
        std::vector<std::string> parts=Split(f.Get(("guest_profile"+Number(i)).c_str()),'|');
        if(parts.size()<6) continue;
        GuestProfile p;
        p.name=Unescaped(parts[0]); p.gamertag=Unescaped(parts[1]);
        if(p.name.empty() || (!parts[2].empty() && !OfflineXuid(parts[2],p.xuid))) continue;
        p.made=atoll(parts[3].c_str()); p.madeOn=Unescaped(parts[4]);
        std::vector<std::string> on=Split(parts[5],',');
        for(size_t k=0;k<on.size();++k) if(!on[k].empty()) p.on.push_back(Unescaped(on[k]));
        if(!GuestProfileNamed(profiles,p.name)) profiles.push_back(p);
    }
}

GuestProfile* GuestProfileNamed(std::vector<GuestProfile>& profiles, const std::string& name) {
    for(size_t i=0;i<profiles.size();++i) if(SameName(profiles[i].name,name)) return &profiles[i];
    return NULL;
}
}
