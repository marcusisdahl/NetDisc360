// SPDX-License-Identifier: MIT
// Sync between computers through NetDisc360's sync Worker (see sync.h).
#include "sync.h"
#include "config.h"
#include "http.h"
#include "web.h"
#include <ctype.h>
#include <map>
#include <stdio.h>
#include <set>
#include <string.h>

namespace cloudsync {
namespace {
const size_t FileMost = 95u << 20;  // As the Worker takes (OBJECT_MOST).
const double Seconds = 60;

#ifdef _WIN32
const char Separator = '\\';
#else
const char Separator = '/';
#endif

std::string Join(const std::string& folder, const std::string& name) {
    if(folder.empty() || folder[folder.size()-1]=='/' || folder[folder.size()-1]=='\\') return folder+name;
    return folder+Separator+name;
}

bool EndsWith(const std::string& text, const char* end) {
    size_t n=strlen(end);
    return text.size()>=n && !text.compare(text.size()-n,n,end);
}

// The Worker's answer as JSON, or why not.
bool Answer(int status, const std::string& body, web::Value& v, std::string& error) {
    if(!web::Parse(body,v) || v.type!=web::Value::Object) {
        error=status ? "The sync's address doesn't answer as NetDisc360's sync (HTTP "+std::to_string(status)+")." : "The sync didn't answer.";
        return false;
    }
    if(status!=200) {
        const web::Value* e=v.Get("error");
        error=e && e->type==web::Value::String ? e->text : "The sync said no (HTTP "+std::to_string(status)+").";
        return false;
    }
    return true;
}

std::string Text(const web::Value& v, const char* key) {
    const web::Value* f=v.Get(key);
    return f && f->type==web::Value::String ? f->text : std::string();
}

bool Call(const std::string& method, const std::string& url, const std::string& key, const std::string& json,
          web::Value& v, std::string& error) {
    http::Call c;
    c.method=method; c.url=url; c.key=key; c.seconds=Seconds; c.most=8u << 20;
    if(!json.empty()) { c.type="application/json"; c.body=json; }
    int status=0;
    std::string body;
    if(!http::Send(c,status,body,error)) return false;
    return Answer(status,body,v,error);
}

// As a query parameter: anything but letters, digits and - _ . ~ as %XX.
std::string Escape(const std::string& text) {
    std::string out;
    for(size_t i=0;i<text.size();++i) {
        unsigned char c=(unsigned char)text[i];
        if(isalnum(c) || strchr("-_.~",c)) out+=char(c);
        else { char hex[4]; snprintf(hex,sizeof(hex),"%%%02X",c); out+=hex; }
    }
    return out;
}

struct Remote { std::string key, sha256; long long size; };

bool List(const std::string& base, const std::string& key, const std::string& prefix, std::vector<Remote>& out, std::string& error) {
    out.clear();
    std::string after;
    for(int page=0;page<1000;++page) {
        web::Value v;
        if(!Call("GET",base+"/v1/list?prefix="+prefix+(after.empty() ? "" : "&after="+after),key,"",v,error)) return false;
        const web::Value* items=v.Get("items");
        if(items && items->type==web::Value::Array)
            for(size_t i=0;i<items->items.size();++i) {
                const web::Value& it=items->items[i];
                const web::Value* size=it.Get("size");
                Remote r={Text(it,"key"),Text(it,"sha256"),size && size->type==web::Value::Number ? (long long)size->number : 0};
                if(!r.key.empty()) out.push_back(r);
            }
        after=Escape(Text(v,"next"));
        if(after.empty()) return true;
    }
    error="The sync's list is too long.";
    return false;
}

bool Get(const std::string& base, const std::string& key, const std::string& object, const std::string& sha,
         std::string& data, std::string& error) {
    http::Call c;
    c.method="GET"; c.url=base+"/v1/object/"+object; c.key=key; c.seconds=Seconds; c.most=FileMost;
    int status=0;
    if(!http::Send(c,status,data,error)) return false;
    if(status!=200) { web::Value v; Answer(status,data,v,error); data.clear(); return false; }
    if(!sha.empty() && web::Sha256Hex(data)!=sha) { data.clear(); error=object+" didn't arrive whole."; return false; }
    return true;
}

// conflict: another file of that name is already there.
bool Put(const std::string& base, const std::string& key, const std::string& object, const std::string& data,
         bool& conflict, std::string& error) {
    conflict=false;
    if(data.size()>FileMost) { error=object+" is too large to sync."; return false; }
    http::Call c;
    c.method="PUT"; c.url=base+"/v1/object/"+object; c.key=key; c.seconds=Seconds+double(data.size()>>20);
    c.type="application/octet-stream"; c.body=data; c.sha256=web::Sha256Hex(data);
    int status=0;
    std::string body;
    if(!http::Send(c,status,body,error)) return false;
    conflict=status==409;
    web::Value v;
    return Answer(status,body,v,error);
}

// Makes folder and the folders it is in, as need be.
void MakeFolders(const std::string& folder) {
    for(size_t at=folder.find_first_of("/\\",1);at!=std::string::npos;at=folder.find_first_of("/\\",at+1))
        if(at>0 && folder[at-1]!=':') config::MakeFolder(folder.substr(0,at));
    config::MakeFolder(folder);
}

bool Syncable(const std::string& name) { return GoodName(name) && (EndsWith(name,".zip") || EndsWith(name,".ini")); }

std::set<std::string> ReadSeen(const std::string& path) {
    std::set<std::string> seen;
    std::string text;
    if(!config::ReadFile(path,text,64u << 20)) return seen;
    for(size_t at=0;at<text.size();) {
        size_t end=text.find('\n',at);
        std::string line=text.substr(at,end==std::string::npos ? std::string::npos : end-at);
        at=end==std::string::npos ? text.size() : end+1;
        if(!line.empty() && line[line.size()-1]=='\r') line.erase(line.size()-1);
        if(!line.empty()) seen.insert(line);
    }
    return seen;
}
}  // namespace

bool GoodName(const std::string& name) {
    if(name.empty() || name.size()>128 || name.find_first_not_of('.')==std::string::npos) return false;
    for(size_t i=0;i<name.size();++i) if(!isalnum((unsigned char)name[i]) && !strchr("._-",name[i])) return false;
    return true;
}

std::string Base(const std::string& url) {
    std::string base=url;
    while(!base.empty() && base[base.size()-1]=='/') base.erase(base.size()-1);
    return base;
}

bool Connect(const std::string& url, const std::string& key, const std::string& name,
             std::string& id, std::string& deviceKey, std::string& error) {
    if(!http::GoodUrl(url,"The sync",error)) return false;
    web::Json j;
    j.Open('{').Set("name",name).Close('}');
    web::Value v;
    if(!Call("POST",Base(url)+"/v1/devices",key,j.Text(),v,error)) return false;
    id=Text(v,"id"); deviceKey=Text(v,"key");
    if(id.empty() || deviceKey.empty()) { error="The sync didn't give this computer a key."; return false; }
    return true;
}

bool Hello(const std::string& url, const std::string& key, std::string& id, std::string& name, std::string& error) {
    web::Value v;
    if(!Call("GET",Base(url)+"/v1/hello",key,"",v,error)) return false;
    id=Text(v,"id"); name=Text(v,"name");
    return true;
}

bool Devices(const std::string& url, const std::string& key, std::vector<Device>& out, std::string& error) {
    out.clear();
    web::Value v;
    if(!Call("GET",Base(url)+"/v1/devices",key,"",v,error)) return false;
    const web::Value* list=v.Get("devices");
    if(list && list->type==web::Value::Array)
        for(size_t i=0;i<list->items.size();++i) {
            const web::Value* added=list->items[i].Get("added");
            Device d={Text(list->items[i],"id"),Text(list->items[i],"name"),added && added->type==web::Value::Number ? (long long)added->number : 0};
            if(!d.id.empty()) out.push_back(d);
        }
    return true;
}

bool Remove(const std::string& url, const std::string& key, const std::string& id, std::string& error) {
    for(size_t i=0;i<id.size();++i) if(!isalnum((unsigned char)id[i])) { error="That isn't a device."; return false; }
    web::Value v;
    return Call("DELETE",Base(url)+"/v1/devices/"+id,key,"",v,error);
}

void Files(const std::string& url, const std::string& key, const Kind& kind, const std::string& seenFile, Report& report) {
    const std::string base=Base(url), prefix="files/"+kind.name+"/";
    std::vector<Remote> remote;
    std::string error;
    if(!List(base,key,prefix,remote,error)) { ++report.failed; report.notes.push_back(error); return; }
    for(size_t i=0;i<remote.size();++i) report.stored+=remote[i].size;
    // This computer's files, by path below the kind ("<XUID>/<file>" at depth 1).
    std::set<std::string> here;
    std::vector<std::string> folders;
    if(kind.depth==0) folders.push_back("");
    else {
        std::vector<config::Entry> subs=config::Folders(kind.folder);
        for(size_t i=0;i<subs.size();++i) if(GoodName(subs[i].name)) folders.push_back(subs[i].name);
    }
    for(size_t f=0;f<folders.size();++f) {
        std::vector<config::Entry> files=config::Files(folders[f].empty() ? kind.folder : Join(kind.folder,folders[f]));
        for(size_t i=0;i<files.size();++i)
            if(Syncable(files[i].name)) here.insert(folders[f].empty() ? files[i].name : folders[f]+"/"+files[i].name);
    }
    std::set<std::string> seen=ReadSeen(seenFile), there;
    const size_t wasSeen=seen.size();
    for(size_t i=0;i<remote.size();++i) {
        const std::string rel=remote[i].key.substr(prefix.size());
        // Only paths this computer would make itself: depth+1 parts, each a good name.
        std::vector<std::string> parts;
        for(size_t at=0;;) {
            size_t slash=rel.find('/',at);
            parts.push_back(rel.substr(at,slash==std::string::npos ? std::string::npos : slash-at));
            if(slash==std::string::npos) break;
            at=slash+1;
        }
        bool good=remote[i].key.compare(0,prefix.size(),prefix)==0 && int(parts.size())==kind.depth+1 && Syncable(parts.back());
        for(size_t p=0;good && p<parts.size();++p) good=GoodName(parts[p]);
        if(!good) continue;
        there.insert(rel);
        if(here.count(rel)) { seen.insert(rel); continue; }
        if(seen.count(rel)) continue;  // Synced before, and deleted here since.
        std::string data;
        if(!Get(base,key,remote[i].key,remote[i].sha256,data,error)) { ++report.failed; report.notes.push_back(error); continue; }
        std::string folder=kind.folder;
        MakeFolders(folder);
        if(kind.depth==1) { folder=Join(folder,parts[0]); config::MakeFolder(folder); }
        if(!config::WriteFile(Join(folder,parts.back()),data,false)) {
            ++report.failed;
            report.notes.push_back("Couldn't write "+config::FromUtf8(Join(folder,parts.back()))+".");
            continue;
        }
        seen.insert(rel);
        ++report.got;
    }
    for(std::set<std::string>::const_iterator h=here.begin();h!=here.end();++h) {
        if(there.count(*h)) continue;
        std::string data, path=kind.folder;
        size_t slash=h->find('/');
        path=slash==std::string::npos ? Join(path,*h) : Join(Join(path,h->substr(0,slash)),h->substr(slash+1));
        if(!config::ReadFile(path,data,FileMost)) { ++report.failed; report.notes.push_back(config::FromUtf8(path)+" couldn't be read, or is too large to sync."); continue; }
        bool conflict=false;
        if(!Put(base,key,prefix+*h,data,conflict,error)) {
            ++report.failed;
            report.notes.push_back(conflict ? kind.name+"/"+*h+": another computer synced a different file of that name." : error);
            continue;
        }
        seen.insert(*h);
        ++report.sent;
        report.stored+=(long long)data.size();
    }
    if(seen.size()!=wasSeen) {
        std::string text;
        for(std::set<std::string>::const_iterator s=seen.begin();s!=seen.end();++s) text+=*s+"\n";
        if(!config::WriteFile(seenFile,text,false)) report.notes.push_back("Couldn't write "+config::FromUtf8(seenFile)+".");
    }
}

bool PutState(const std::string& url, const std::string& key, const std::string& self, const std::string& name,
              const std::string& data, std::string& sentSha, std::string& error) {
    if(!GoodName(self) || !GoodName(name)) { error="That isn't a document's name."; return false; }
    std::string sha=web::Sha256Hex(data);
    if(sha==sentSha) return true;
    bool conflict=false;
    if(!Put(Base(url),key,"state/"+self+"/"+name,data,conflict,error)) return false;
    sentSha=sha;
    return true;
}

void GetStates(const std::string& url, const std::string& key, const std::string& self, const std::string& name,
               const std::string& folder, Report& report) {
    const std::string base=Base(url);
    std::vector<Remote> remote;
    std::string error;
    if(!List(base,key,"state/",remote,error)) { ++report.failed; report.notes.push_back(error); return; }
    for(size_t i=0;i<remote.size();++i) report.stored+=remote[i].size;
    for(size_t i=0;i<remote.size();++i) {
        const std::string& k=remote[i].key;  // state/<device>/<name>
        size_t slash=k.find('/',6);
        if(slash==std::string::npos) continue;
        std::string device=k.substr(6,slash-6);
        if(device==self || !GoodName(device) || k.substr(slash+1)!=name) continue;
        std::string mine=Join(Join(folder,device),name), had;
        if(config::ReadFile(mine,had,FileMost) && web::Sha256Hex(had)==remote[i].sha256) continue;
        std::string data;
        if(!Get(base,key,k,remote[i].sha256,data,error)) { ++report.failed; report.notes.push_back(error); continue; }
        config::MakeFolder(folder); config::MakeFolder(Join(folder,device));
        if(!config::WriteFile(mine,data,false)) { ++report.failed; report.notes.push_back("Couldn't write "+config::FromUtf8(mine)+"."); continue; }
        ++report.got;
    }
}
}  // namespace cloudsync
