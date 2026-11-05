// SPDX-License-Identifier: MIT
// Backups of the settings (see backup.h).
#include "backup.h"
#include "config.h"
#include "version.h"
#include "../shared/platform.h"
#include <algorithm>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace backup {
namespace {
const char Prefix[] = "NetDisc360_";
const size_t MaxFile = 1 << 20;  // Settings are a few KB; anything this big isn't ours.

unsigned Crc32(const std::string& data) {
    static unsigned table[256];
    if(!table[1]) {
        for(unsigned i=0;i<256;++i) {
            unsigned c=i;
            for(int k=0;k<8;++k) c=c&1 ? 0xEDB88320u^(c>>1) : c>>1;
            table[i]=c;
        }
    }
    unsigned c=0xFFFFFFFFu;
    for(size_t i=0;i<data.size();++i) c=table[(c^(unsigned char)data[i])&0xFF]^(c>>8);
    return c^0xFFFFFFFFu;
}

void Put16(std::string& out, unsigned v) { out+=char(v&0xFF); out+=char((v>>8)&0xFF); }
void Put32(std::string& out, unsigned v) { Put16(out,v&0xFFFF); Put16(out,v>>16); }
unsigned Get16(const std::string& in, size_t at) { return (unsigned char)in[at]|((unsigned char)in[at+1]<<8); }
unsigned Get32(const std::string& in, size_t at) { return Get16(in,at)|(Get16(in,at+2)<<16); }

struct tm Local(long long when) {
    time_t t=time_t(when);
    struct tm out;
#ifdef _WIN32
    bool known=!localtime_s(&out,&t);
#else
    bool known=localtime_r(&t,&out)!=NULL;
#endif
    if(!known) { memset(&out,0,sizeof(out)); out.tm_mday=1; out.tm_year=70; }  // Out of range: strftime would end the program on Windows.
    return out;
}

std::string Lower(std::string text) {
    for(size_t i=0;i<text.size();++i) if(text[i]>='A' && text[i]<='Z') text[i]=char(text[i]-'A'+'a');
    return text;
}

// Keys only NetDisc360's settings file has, to tell it from any other ini.
bool Settings(const std::string& text) {
    config::File f;
    f.Read(text);
    static const char* const keys[]={"folders","xbox","ftp_user","xbox_folder","aurora","setup_done","web_lan","consoles","choices"};
    for(size_t i=0;i<sizeof(keys)/sizeof(keys[0]);++i) if(f.Has(keys[i])) return true;
    return false;
}

std::string Join(const std::string& folder, const std::string& name) {
    if(folder.empty()) return name;
    char last=folder[folder.size()-1];
#ifdef _WIN32
    return folder+(last=='/' || last=='\\' ? "" : "\\")+name;
#else
    return folder+(last=='/' ? "" : "/")+name;
#endif
}
}  // namespace

const char* Platform() {
#ifdef _WIN32
    return "windows";
#else
    return "linux";
#endif
}

std::string Zip(const std::map<std::string,std::string>& files, long long when) {
    struct tm t=Local(when);
    unsigned dosTime=(unsigned(t.tm_hour)<<11)|(unsigned(t.tm_min)<<5)|unsigned(t.tm_sec/2);
    unsigned dosDate=(unsigned(t.tm_year>=80 ? t.tm_year-80 : 0)<<9)|(unsigned(t.tm_mon+1)<<5)|unsigned(t.tm_mday);
    std::string out, directory;
    for(std::map<std::string,std::string>::const_iterator f=files.begin();f!=files.end();++f) {
        unsigned crc=Crc32(f->second), offset=unsigned(out.size()), size=unsigned(f->second.size());
        Put32(out,0x04034B50); Put16(out,20); Put16(out,0x0800 /* UTF-8 names */); Put16(out,0);
        Put16(out,dosTime); Put16(out,dosDate); Put32(out,crc); Put32(out,size); Put32(out,size);
        Put16(out,unsigned(f->first.size())); Put16(out,0);
        out+=f->first; out+=f->second;
        Put32(directory,0x02014B50); Put16(directory,20); Put16(directory,20); Put16(directory,0x0800); Put16(directory,0);
        Put16(directory,dosTime); Put16(directory,dosDate); Put32(directory,crc); Put32(directory,size); Put32(directory,size);
        Put16(directory,unsigned(f->first.size())); Put16(directory,0); Put16(directory,0); Put16(directory,0); Put16(directory,0);
        Put32(directory,0); Put32(directory,offset);
        directory+=f->first;
    }
    unsigned at=unsigned(out.size());
    out+=directory;
    Put32(out,0x06054B50); Put16(out,0); Put16(out,0); Put16(out,unsigned(files.size())); Put16(out,unsigned(files.size()));
    Put32(out,unsigned(directory.size())); Put32(out,at); Put16(out,0);
    return out;
}

bool Unpack(const std::string& data, std::map<std::string,std::string>& files, std::string& error, size_t maxFile) {
    files.clear();
    // The end record is the last 22 bytes, unless the zip has a comment.
    size_t end=std::string::npos;
    for(size_t at=data.size()>=22 ? data.size()-22 : std::string::npos, stop=data.size()>65557 ? data.size()-65557 : 0;
        at!=std::string::npos && at>=stop;--at) {
        if(Get32(data,at)==0x06054B50) { end=at; break; }
        if(!at) break;
    }
    if(end==std::string::npos) { error="It isn't a zip file, or it's cut short."; return false; }
    unsigned count=Get16(data,end+10), size=Get32(data,end+12), at=Get32(data,end+16);
    if(size_t(at)+size>end) { error="The zip file is damaged."; return false; }
    for(unsigned i=0;i<count;++i) {
        if(size_t(at)+46>end || Get32(data,at)!=0x02014B50) { error="The zip file is damaged."; return false; }
        unsigned method=Get16(data,at+10), crc=Get32(data,at+16), packed=Get32(data,at+20), unpacked=Get32(data,at+24);
        unsigned nameLength=Get16(data,at+28), extra=Get16(data,at+30), comment=Get16(data,at+32), local=Get32(data,at+42);
        if(size_t(at)+46+nameLength>end) { error="The zip file is damaged."; return false; }
        std::string name=data.substr(at+46,nameLength);
        at+=46+nameLength+extra+comment;
        if(!name.empty() && name[name.size()-1]=='/') continue;  // A folder.
        if(method!=0) {
            error="Its files are compressed, which NetDisc360's backups never are. Restore the backup as it was downloaded, or choose its netdisc.ini by itself.";
            return false;
        }
        if(size_t(local)+30>data.size() || Get32(data,local)!=0x04034B50) { error="The zip file is damaged."; return false; }
        size_t start=size_t(local)+30+Get16(data,local+26)+Get16(data,local+28);
        if(packed!=unpacked || unpacked>maxFile || start+unpacked>data.size()) {
            error=maxFile==MaxFile ? "The zip file is damaged, or holds more than settings." : "The zip file is damaged.";
            return false;
        }
        std::string content=data.substr(start,unpacked);
        if(Crc32(content)!=crc) { error="The zip file is damaged (a file's checksum is wrong)."; return false; }
        files[name]=content;
    }
    return true;
}

bool Unzip(const std::string& data, std::map<std::string,std::string>& files, std::string& error) {
    std::map<std::string,std::string> named;
    if(!Unpack(data,named,error,MaxFile)) return false;
    for(std::map<std::string,std::string>::const_iterator f=named.begin();f!=named.end();++f) {
        size_t slash=f->first.find_last_of("/\\");
        files[Lower(slash==std::string::npos ? f->first : f->first.substr(slash+1))]=f->second;
    }
    return true;
}

bool Make(const std::string& folder, const std::string& kind, const std::map<std::string,std::string>& files,
          std::string& name, std::string& error) {
    if(!config::MakeFolder(folder)) { error="Can't make the folder "+folder+"."; return false; }
    long long now=(long long)time(NULL);
    std::map<std::string,std::string> all=files;
    char about[256];
    _snprintf_s(about,sizeof(about),_TRUNCATE,"[Backup]\napp=NetDisc360\nversion=%s\nplatform=%s\nkind=%s\ntime=%lld\n",
                NETDISC_VERSION,Platform(),kind.c_str(),now);
    all["backup.ini"]=about;
    struct tm t=Local(now);
    char stamp[32];
    strftime(stamp,sizeof(stamp),"%Y-%m-%d_%H-%M-%S",&t);
    std::vector<config::Entry> there=config::Files(folder);
    for(int n=1;;++n) {
        char suffix[16]="";
        if(n>1) _snprintf_s(suffix,sizeof(suffix),_TRUNCATE,"-%d",n);
        name=std::string(Prefix)+stamp+suffix+"_"+kind+".zip";
        bool taken=false;
        for(size_t i=0;i<there.size() && !taken;++i) taken=Lower(there[i].name)==Lower(name);
        if(!taken) break;
    }
    // It holds Aurora's FTP login, so like the settings file it's for its owner only.
    if(!config::WriteFile(Join(folder,name),Zip(all,now),true)) { error="Can't write "+Join(folder,name)+"."; return false; }
    return true;
}

bool Open(const std::string& data, std::map<std::string,std::string>& files, Info& info, std::string& error) {
    info=Info();
    info.size=(long long)data.size();
    files.clear();
    if(data.size()>=4 && !data.compare(0,4,"PK\x03\x04",4)) {
        if(!Unzip(data,files,error)) return false;
        std::map<std::string,std::string>::const_iterator about=files.find("backup.ini");
        if(about!=files.end()) {
            config::File f;
            f.Read(about->second);
            info.kind=f.Get("kind"); info.version=f.Get("version"); info.platform=f.Get("platform");
            info.time=f.Number("time",0);
            files.erase("backup.ini");
        }
        // Only the settings; anything else in it is left out.
        for(std::map<std::string,std::string>::iterator f=files.begin();f!=files.end();)
            if(f->first!="netdisc.ini" && f->first!="discs.ini") files.erase(f++);
            else ++f;
        if(!files.count("netdisc.ini")) { error="This zip has no netdisc.ini, so it isn't a NetDisc360 backup."; return false; }
    } else if(data.size()<=MaxFile && Settings(data)) {
        files["netdisc.ini"]=data;
    } else {
        error="This isn't a NetDisc360 backup (a .zip from Backups) or a netdisc.ini.";
        return false;
    }
    if(!Settings(files["netdisc.ini"])) { error="Its netdisc.ini has none of NetDisc360's settings."; return false; }
    return true;
}

std::vector<Info> List(const std::string& folder) {
    std::vector<Info> out;
    std::vector<config::Entry> files=config::Files(folder);
    for(size_t i=0;i<files.size();++i) {
        if(!Named(files[i].name) || files[i].name.compare(0,sizeof(Prefix)-1,Prefix) || files[i].size>8*(long long)MaxFile) continue;
        Info info;
        std::string data, error;
        std::map<std::string,std::string> contents;
        if(!config::ReadFile(Join(folder,files[i].name),data,8*MaxFile) || !Open(data,contents,info,error)) continue;
        info.name=files[i].name;
        info.size=files[i].size;
        if(!info.time) info.time=files[i].time;
        if(info.kind.empty()) info.kind="manual";
        out.push_back(info);
    }
    struct Newest { bool operator()(const Info& a, const Info& b) const { return a.time!=b.time ? a.time>b.time : a.name>b.name; } };
    std::sort(out.begin(),out.end(),Newest());
    return out;
}

int Prune(const std::string& folder, int keep, const std::string& spare) {
    std::vector<Info> all=List(folder);
    int kept=0, removed=0;
    for(size_t i=0;i<all.size();++i) {
        if(all[i].kind=="manual" || all[i].name==spare) continue;
        if(++kept>keep && config::RemoveFile(Join(folder,all[i].name))) ++removed;
    }
    return removed;
}

bool Named(const std::string& name) {
    if(name.size()<5 || name.size()>120 || name[0]=='.' || Lower(name.substr(name.size()-4))!=".zip") return false;
    for(size_t i=0;i<name.size();++i) {
        char c=name[i];
        if(!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='-' || c=='_' || c=='.')) return false;
    }
    return name.find("..")==std::string::npos;
}

bool FromBase64(const std::string& text, std::string& out) {
    out.clear();
    unsigned bits=0;
    int count=0;
    for(size_t i=0;i<text.size();++i) {
        char c=text[i];
        int v;
        if(c>='A' && c<='Z') v=c-'A';
        else if(c>='a' && c<='z') v=c-'a'+26;
        else if(c>='0' && c<='9') v=c-'0'+52;
        else if(c=='+' || c=='-') v=62;
        else if(c=='/' || c=='_') v=63;
        else if(c=='=' || c=='\r' || c=='\n' || c==' ') continue;
        else return false;
        bits=(bits<<6)|unsigned(v);
        if((count+=6)>=8) { count-=8; out+=char((bits>>count)&0xFF); }
    }
    return true;
}
}
