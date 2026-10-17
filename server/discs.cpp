// SPDX-License-Identifier: MIT
#include "discs.h"
#include <algorithm>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <map>
#include <mutex>
#ifndef _WIN32
#include <dirent.h>
#endif

namespace nia {
namespace {
enum { Sector = 2048, VolumeSector = 32, MaxDepth = 8, MaxOpenFiles = 16, MaxName = 255 };
const char Magic[] = "MICROSOFT*XBOX*MEDIA";
const size_t MagicSize = 20;
// Where the game partition starts in a full XGD2 or XGD3 disc image, as in
// the retail ISOs the plugin already plays. Before it is only zeros here: the
// video partition and the security sectors aren't in a folder or GOD package.
const U64 Xgd2Partition = 0xFD90000, Xgd3Partition = 0x2080000;
const U64 Xgd2PartitionMax = 7838695424ULL - Xgd2Partition;  // An XGD2 image is 7,838,695,424 bytes.
U64 PartitionStart(U64 partitionBytes) { return partitionBytes<=Xgd2PartitionMax ? Xgd2Partition : Xgd3Partition; }

#ifdef _WIN32
const char Slash = '\\';
#else
const char Slash = '/';
#endif
std::string Join(const std::string& a, const std::string& b) { return a+Slash+b; }
std::string Leaf(const std::string& path) {
    size_t at=path.find_last_of("/\\");
    return at==std::string::npos ? path : path.substr(at+1);
}
std::string Parent(const std::string& path) {
    size_t at=path.find_last_of("/\\");
    return at==std::string::npos ? std::string() : path.substr(0,at);
}
U64 RoundUp(U64 value, U64 unit) { return (value+unit-1)/unit*unit; }
DWORD Little32(const unsigned char* p) { return DWORD(p[0])|(DWORD(p[1])<<8)|(DWORD(p[2])<<16)|(DWORD(p[3])<<24); }
void PutLittle16(unsigned char* p, DWORD v) { p[0]=BYTE(v); p[1]=BYTE(v>>8); }
void PutLittle32(unsigned char* p, DWORD v) { p[0]=BYTE(v); p[1]=BYTE(v>>8); p[2]=BYTE(v>>16); p[3]=BYTE(v>>24); }
DWORD Big32(const unsigned char* p) { return (DWORD(p[0])<<24)|(DWORD(p[1])<<16)|(DWORD(p[2])<<8)|p[3]; }
double Seconds() { return double(clock())/CLOCKS_PER_SEC; }

// Files and folders ----------------------------------------------------------

struct Entry { std::string name; bool folder; U64 size; long long modified; };

bool List(const std::string& folder, std::vector<Entry>& out) {
    out.clear();
#ifdef _WIN32
    WIN32_FIND_DATAA found;
    HANDLE search=FindFirstFileA((folder+"\\*").c_str(),&found);
    if(search==INVALID_HANDLE_VALUE) return false;
    do {
        if(!strcmp(found.cFileName,".") || !strcmp(found.cFileName,"..")) continue;
        Entry e; e.name=found.cFileName;
        e.folder=(found.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
        // Junctions and links could loop.
        if(e.folder && (found.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        e.size=(U64(found.nFileSizeHigh)<<32)|found.nFileSizeLow;
        e.modified=(long long)((U64(found.ftLastWriteTime.dwHighDateTime)<<32)|found.ftLastWriteTime.dwLowDateTime);
        out.push_back(e);
    } while(FindNextFileA(search,&found));
    FindClose(search);
#else
    DIR* dir=opendir(folder.c_str()); if(!dir) return false;
    while(struct dirent* d=readdir(dir)) {
        if(!strcmp(d->d_name,".") || !strcmp(d->d_name,"..")) continue;
        struct stat st; if(lstat(Join(folder,d->d_name).c_str(),&st)) continue;
        if(!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) continue;
        Entry e; e.name=d->d_name; e.folder=S_ISDIR(st.st_mode); e.size=e.folder ? 0 : U64(st.st_size); e.modified=(long long)st.st_mtime;
        out.push_back(e);
    }
    closedir(dir);
#endif
    return true;
}
const Entry* Find(const std::vector<Entry>& entries, const std::string& name, bool folder) {
    for(size_t i=0;i<entries.size();++i)
        if(entries[i].folder==folder && !_stricmp(entries[i].name.c_str(),name.c_str())) return &entries[i];
    return NULL;
}

class File {
public:
    File() {
#ifdef _WIN32
        file_=INVALID_HANDLE_VALUE;
#else
        file_=-1;
#endif
    }
    ~File() { Close(); }
    bool Open(const std::string& path) {
        Close();
#ifdef _WIN32
        file_=CreateFileA(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
        return file_!=INVALID_HANDLE_VALUE;
#else
        file_=open(path.c_str(),O_RDONLY);
        return file_>=0;
#endif
    }
    void Close() {
#ifdef _WIN32
        if(file_!=INVALID_HANDLE_VALUE) { CloseHandle(file_); file_=INVALID_HANDLE_VALUE; }
#else
        if(file_>=0) { close(file_); file_=-1; }
#endif
    }
    bool Size(U64& size) {
#ifdef _WIN32
        LARGE_INTEGER s; if(!GetFileSizeEx(file_,&s)) return false;
        size=U64(s.QuadPart);
#else
        struct stat st; if(fstat(file_,&st)) return false;
        size=U64(st.st_size);
#endif
        return true;
    }
    // Bytes read; fewer than length at the end of the file, -1 on an error.
    long long Read(U64 offset, unsigned char* data, DWORD length) {
        DWORD total=0;
        while(total<length) {
#ifdef _WIN32
            OVERLAPPED at; memset(&at,0,sizeof(at)); at.Offset=DWORD(offset); at.OffsetHigh=DWORD(offset>>32);
            DWORD got=0;
            if(!::ReadFile(file_,data+total,length-total,&got,&at)) return GetLastError()==ERROR_HANDLE_EOF ? total : -1;
#else
            ssize_t got=pread(file_,data+total,length-total,off_t(offset));
            if(got<0) { if(errno==EINTR) continue; return -1; }
#endif
            if(!got) break;
            total+=DWORD(got); offset+=U64(got);
        }
        return total;
    }
private:
    File(const File&);
    File& operator=(const File&);
#ifdef _WIN32
    HANDLE file_;
#else
    int file_;
#endif
};

// Names on the console: NetISO puts them in paths, so no slashes or colons.
std::string CleanName(const std::string& raw) {
    std::string out;
    for(size_t i=0;i<raw.size();++i) {
        unsigned char c=(unsigned char)raw[i];
        if(c==':') { while(!out.empty() && out[out.size()-1]==' ') out.erase(out.size()-1); out+=" -"; continue; }
        out+=(c<' ' || c>'~' || strchr("\\/*?\"<>|",c)) ? '_' : char(c);
    }
    size_t start=out.find_first_not_of(" .");
    if(start==std::string::npos) return std::string();
    out=out.substr(start);
    while(!out.empty() && (out[out.size()-1]==' ' || out[out.size()-1]=='.')) out.erase(out.size()-1);
    return out;
}

// ISO files ------------------------------------------------------------------

class IsoImage : public DiscImage {
public:
    bool Open(const std::string& path, std::string& error) {
        if(!file_.Open(path) || !file_.Size(size_)) { error="it can't be opened"; return false; }
        return true;
    }
    U64 Size() const { return size_; }
    bool Read(U64 offset, unsigned char* data, DWORD length) { return file_.Read(offset,data,length)==(long long)length; }
    std::string Description() const { return "ISO"; }
private:
    File file_;
    U64 size_;
};

// Games on Demand ------------------------------------------------------------
//
// A GOD package is the game partition of the disc (from its start; the
// volume descriptor is at 0x10000 in it) cut into Data0000, Data0001, ... in
// <header>.data. Each file starts with a master hash block, then groups of a
// hash block and 0xCC data blocks, 4 KB each; every file but the last holds
// 0xA1C4 data blocks. Xenia reads them the same way (stfs_container_device.cc).
// Packages made with iso2god (the "XSF" layout) have the volume descriptor at
// 0x10000 of the data, ones from Microsoft ("EGDF") at 0.

enum { GodBlock = 0x1000, GodGroupData = 0xCC, GodPartData = 0xA1C4, GodContentType = 0x7000, GodHeaderRead = 0x600 };
const U64 GodPartBytes = U64(GodPartData)*GodBlock;

struct GodHeader { DWORD titleId; int disc, discs; std::string title; };

bool ReadGodHeader(const std::string& path, GodHeader& header) {
    File f; unsigned char h[GodHeaderRead];
    if(!f.Open(path) || f.Read(0,h,sizeof(h))!=(long long)sizeof(h)) return false;
    if(memcmp(h,"CON ",4) && memcmp(h,"LIVE",4) && memcmp(h,"PIRS",4)) return false;
    if(Big32(h+0x344)!=GodContentType) return false;
    header.titleId=Big32(h+0x360); header.disc=h[0x366]; header.discs=h[0x367];
    header.title.clear();
    for(int i=0;i<0x80;++i) {  // UTF-16BE, from 0x411.
        unsigned c=(unsigned(h[0x411+i*2])<<8)|h[0x412+i*2];
        if(!c) break;
        header.title+=c<0x80 ? char(c) : '_';
    }
    return true;
}

DWORD GodDataBlocks(U64 fileBytes) {
    U64 blocks=fileBytes/GodBlock;
    if(blocks<=1) return 0;
    U64 rest=blocks-1, groups=rest/(GodGroupData+1), left=rest%(GodGroupData+1);
    return DWORD(groups*GodGroupData+(left ? left-1 : 0));
}

class GodImage : public DiscImage {
public:
    bool Open(const std::string& header, std::string& error) {
        std::string folder=header+".data";
        dataBytes_=0;
        for(int n=0;n<10000;++n) {
            char name[16]; _snprintf_s(name,sizeof(name),_TRUNCATE,"Data%04d",n);
            std::unique_ptr<File> part(new File);
            if(!part->Open(Join(folder,name))) break;
            U64 bytes=0;
            if(!part->Size(bytes)) { error=std::string("can't read ")+name; return false; }
            DWORD blocks=GodDataBlocks(bytes);
            if(!parts_.empty() && last_<GodPartData) { error="a Data file before "+std::string(name)+" is incomplete"; return false; }
            last_=blocks; dataBytes_+=U64(blocks)*GodBlock;
            parts_.push_back(std::move(part));
        }
        if(parts_.empty()) { error="there is no Data0000 in "+Leaf(folder); return false; }
        unsigned char magic[MagicSize];
        static const U64 at[]={0x10000,0};
        bool found=false;
        for(int i=0;i<2 && !found;++i) {
            if(at[i]+MagicSize>dataBytes_ || !ReadData(at[i],magic,MagicSize)) continue;
            if(!memcmp(magic,Magic,MagicSize)) { magicAt_=at[i]; found=true; }
        }
        if(!found) { error="it holds no Xbox 360 disc"; return false; }
        U64 partition=PartitionStart(dataBytes_+0x10000-magicAt_);
        base_=partition+0x10000-magicAt_;
        size_=RoundUp(base_+dataBytes_,Sector);
        return true;
    }
    U64 Size() const { return size_; }
    bool Read(U64 offset, unsigned char* data, DWORD length) {
        memset(data,0,length);
        U64 end=offset+length;
        if(end<=base_ || offset>=base_+dataBytes_) return true;
        U64 from=offset<base_ ? base_ : offset, to=end>base_+dataBytes_ ? base_+dataBytes_ : end;
        return ReadData(from-base_,data+(from-offset),DWORD(to-from));
    }
    std::string Description() const {
        char text[64]; _snprintf_s(text,sizeof(text),_TRUNCATE,"GOD package, %d part(s)",int(parts_.size()));
        return text;
    }
private:
    bool ReadData(U64 at, unsigned char* data, DWORD length) {
        while(length) {
            size_t part=size_t(at/GodPartBytes);
            if(part>=parts_.size()) return false;
            U64 inside=at%GodPartBytes, block=inside/GodBlock, within=inside%GodBlock;
            // The master hash block, then a hash block before every group.
            U64 fileAt=U64(GodBlock)*(2+block+block/GodGroupData)+within;
            U64 run=(GodGroupData-block%GodGroupData)*GodBlock-within;
            DWORD n=run<length ? DWORD(run) : length;
            if(parts_[part]->Read(fileAt,data,n)!=(long long)n) return false;
            at+=n; data+=n; length-=n;
        }
        return true;
    }
    std::vector<std::unique_ptr<File> > parts_;
    DWORD last_;
    U64 dataBytes_, magicAt_, base_, size_;
};

// Extracted games ------------------------------------------------------------
//
// The folder becomes a disc in the format of a game partition (XDVDFS, which
// the Xbox 360 calls GDF): the volume descriptor at sector 32, then the
// directory tables, then the files, each from the start of a sector. Each
// directory table is a binary search tree of its entries, ordered by
// name without case, with the root entry first. Entries don't cross sectors;
// the rest is 0xFF. extract-xiso builds the same.

bool NameLess(const std::string& a, const std::string& b) {
    size_t n=a.size()<b.size() ? a.size() : b.size();
    for(size_t i=0;i<n;++i) {
        int x=toupper((unsigned char)a[i]),y=toupper((unsigned char)b[i]);
        if(x!=y) return x<y;
    }
    return a.size()<b.size();
}

// A subfolder that is a disc of its own ("Disc 2" with its own default.xex),
// so it is listed by itself and left out of the folder it's in.
bool SeparateDisc(const std::string& path) {
    if(_strnicmp(Leaf(path).c_str(),"disc",4)) return false;
    std::vector<Entry> entries;
    return List(path,entries) && Find(entries,"default.xex",false);
}

struct Node {
    std::string name;
    bool folder;
    U64 size;
    int file;  // Into FolderImage::paths_.
    DWORD sector, bytes;
    std::vector<Node> children;
};
struct Extent {
    U64 start, size;  // From the start of the partition.
    int file;
    bool operator<(const Extent& o) const { return start<o.start; }
};

class FolderImage : public DiscImage {
public:
    bool Open(const std::string& folder, std::string& error) {
        double started=Seconds();
        Node root; root.folder=true; root.size=0; root.file=-1;
        if(!Walk(folder,root,true,error)) return false;
        DWORD next=VolumeSector+1;
        if(!PlaceTables(root,next,error)) return false;
        metaEnd_=next;
        PlaceFiles(root,next);
        meta_.assign(size_t(metaEnd_-VolumeSector)*Sector,0xFF);
        unsigned char* volume=&meta_[0];
        memset(volume,0,Sector);
        memcpy(volume,Magic,MagicSize);
        PutLittle32(volume+20,root.sector); PutLittle32(volume+24,root.bytes);
        U64 now=(U64(time(NULL))+11644473600ULL)*10000000ULL;  // FILETIME.
        PutLittle32(volume+28,DWORD(now)); PutLittle32(volume+32,DWORD(now>>32));
        memcpy(volume+0x7EC,Magic,MagicSize);
        WriteTables(root);
        partition_=PartitionStart(U64(next)*Sector);
        size_=partition_+U64(next)*Sector;
        char text[96]; _snprintf_s(text,sizeof(text),_TRUNCATE,"folder, %d file(s), read in %.1f s",int(paths_.size()),Seconds()-started);
        description_=text;
        return true;
    }
    U64 Size() const { return size_; }
    bool Read(U64 offset, unsigned char* data, DWORD length) {
        memset(data,0,length);
        U64 end=offset+length;
        if(end<=partition_) return true;
        U64 from=offset<partition_ ? 0 : offset-partition_, to=end-partition_;
        unsigned char* out=data+(from+partition_-offset);
        // The volume descriptor and directory tables.
        U64 metaFrom=U64(VolumeSector)*Sector, metaTo=U64(metaEnd_)*Sector;
        if(from<metaTo && to>metaFrom) {
            U64 a=from>metaFrom ? from : metaFrom, b=to<metaTo ? to : metaTo;
            memcpy(out+(a-from),&meta_[size_t(a-metaFrom)],size_t(b-a));
        }
        // The files.
        Extent key; key.start=from;
        std::vector<Extent>::const_iterator e=std::upper_bound(extents_.begin(),extents_.end(),key);
        if(e!=extents_.begin()) --e;
        for(;e!=extents_.end() && e->start<to;++e) {
            if(e->start+e->size<=from) continue;
            U64 a=from>e->start ? from : e->start, b=to<e->start+e->size ? to : e->start+e->size;
            File* f=Opened(e->file);
            // A file that got shorter since the mount reads as zeros past its end.
            if(!f || f->Read(a-e->start,out+(a-from),DWORD(b-a))<0) { failed_=paths_[size_t(e->file)]; return false; }
        }
        return true;
    }
    std::string Description() const { return description_; }
private:
    bool Walk(const std::string& path, Node& node, bool top, std::string& error) {
        std::vector<Entry> entries;
        if(!List(path,entries)) { error="can't list "+path; return false; }
        for(size_t i=0;i<entries.size();++i) {
            const Entry& e=entries[i];
            if(e.name.size()>MaxName) { error="the name "+e.name+" is too long for a disc"; return false; }
            std::string inside=Join(path,e.name);
            if(e.folder && top && SeparateDisc(inside)) continue;
            Node child; child.name=e.name; child.folder=e.folder; child.size=e.size; child.file=-1; child.sector=child.bytes=0;
            if(e.folder) { if(!Walk(inside,child,false,error)) return false; }
            else {
                if(e.size>0xFFFFFFFFULL) { error=inside+" is over 4 GB, which a disc can't hold"; return false; }
                child.file=int(paths_.size()); paths_.push_back(inside);
            }
            node.children.push_back(child);
        }
        std::sort(node.children.begin(),node.children.end(),[](const Node& a, const Node& b) { return NameLess(a.name,b.name); });
        return true;
    }
    // The tree over children[lo, hi) in the order its entries are written (root first).
    static int Tree(int lo, int hi, std::vector<int>& order, std::vector<int>& left, std::vector<int>& right) {
        if(lo>=hi) return -1;
        int middle=lo+(hi-lo)/2;
        order.push_back(middle);
        left[size_t(middle)]=Tree(lo,middle,order,left,right);
        right[size_t(middle)]=Tree(middle+1,hi,order,left,right);
        return middle;
    }
    struct Table { std::vector<int> order, left, right; std::vector<DWORD> at; DWORD bytes; };
    static Table Arrange(const Node& folder) {
        Table t; int n=int(folder.children.size());
        t.left.assign(size_t(n),-1); t.right.assign(size_t(n),-1); t.at.assign(size_t(n),0);
        Tree(0,n,t.order,t.left,t.right);
        DWORD pos=0;
        for(size_t i=0;i<t.order.size();++i) {
            DWORD size=DWORD((14+folder.children[size_t(t.order[i])].name.size()+3)&~size_t(3));
            if(pos%Sector+size>Sector) pos=DWORD(RoundUp(pos,Sector));
            t.at[size_t(t.order[i])]=pos; pos+=size;
        }
        t.bytes=DWORD(RoundUp(pos,Sector));
        return t;
    }
    bool PlaceTables(Node& folder, DWORD& next, std::string& error) {
        Table t=Arrange(folder);
        if(t.bytes/4>0xFFFF) { error="the folder "+(folder.name.empty() ? std::string("at the top") : folder.name)+" has too many files for a disc"; return false; }
        folder.bytes=t.bytes; folder.sector=t.bytes ? next : 0;
        next+=t.bytes/Sector;
        for(size_t i=0;i<folder.children.size();++i)
            if(folder.children[i].folder && !PlaceTables(folder.children[i],next,error)) return false;
        return true;
    }
    void PlaceFiles(Node& folder, DWORD& next) {
        for(size_t i=0;i<folder.children.size();++i) {
            Node& c=folder.children[i];
            if(c.folder) { PlaceFiles(c,next); continue; }
            c.sector=c.size ? next : 0; c.bytes=DWORD(c.size);
            if(!c.size) continue;
            Extent e; e.start=U64(next)*Sector; e.size=c.size; e.file=c.file;
            extents_.push_back(e);
            next+=DWORD(RoundUp(c.size,Sector)/Sector);
        }
    }
    void WriteTables(const Node& folder) {
        if(!folder.bytes) return;
        Table t=Arrange(folder);
        unsigned char* table=&meta_[size_t(folder.sector-VolumeSector)*Sector];
        for(size_t i=0;i<folder.children.size();++i) {
            const Node& c=folder.children[i];
            unsigned char* p=table+t.at[i];
            PutLittle16(p,t.left[i]<0 ? 0 : t.at[size_t(t.left[i])]/4);
            PutLittle16(p+2,t.right[i]<0 ? 0 : t.at[size_t(t.right[i])]/4);
            PutLittle32(p+4,c.sector); PutLittle32(p+8,c.bytes);
            p[12]=c.folder ? 0x10 : 0x20;  // Directory, archive.
            p[13]=BYTE(c.name.size());
            memcpy(p+14,c.name.data(),c.name.size());
        }
        for(size_t i=0;i<folder.children.size();++i) if(folder.children[i].folder) WriteTables(folder.children[i]);
    }
    File* Opened(int index) {
        for(size_t i=0;i<open_.size();++i) {
            if(open_[i].first!=index) continue;
            if(i) std::rotate(open_.begin(),open_.begin()+long(i),open_.begin()+long(i)+1);
            return open_[0].second.get();
        }
        std::unique_ptr<File> f(new File);
        if(!f->Open(paths_[size_t(index)])) return NULL;
        open_.insert(open_.begin(),std::make_pair(index,std::move(f)));
        if(open_.size()>MaxOpenFiles) open_.pop_back();
        return open_[0].second.get();
    }
    std::vector<std::string> paths_;
    std::vector<Extent> extents_;
    std::vector<unsigned char> meta_;
    std::vector<std::pair<int,std::unique_ptr<File> > > open_;
    DWORD metaEnd_;
    U64 partition_, size_;
    std::string description_, failed_;
};

// Title IDs ------------------------------------------------------------------

enum { ExecutionInfoKey = 0x40006, ExecutionInfoBytes = 24, XexHeaderMax = 0x100000, RootMax = 0x100000 };

// Title ID and disc number from the execution info in a XEX header.
bool XexTitle(const unsigned char* xex, DWORD size, DWORD& titleId, int& disc) {
    if(size<24 || memcmp(xex,"XEX2",4)) return false;
    DWORD count=Big32(xex+20);
    for(DWORD i=0;i<count && 24+i*8+8<=size;++i) {
        if(Big32(xex+24+i*8)!=ExecutionInfoKey) continue;
        DWORD at=Big32(xex+28+i*8);
        if(at<24 || at+ExecutionInfoBytes>size) return false;
        titleId=Big32(xex+at+12); disc=xex[at+18] ? xex[at+18] : 1;
        return titleId!=0;
    }
    return false;
}
// Reads default.xex's header, from a file or from inside a disc image.
template<class Reader> bool HeaderTitle(Reader read, DWORD xexSize, DWORD& titleId, int& disc) {
    unsigned char start[24];
    if(xexSize<24 || !read(0,start,24)) return false;
    DWORD size=Big32(start+8);
    if(size>xexSize) size=xexSize;
    if(size<24 || size>XexHeaderMax) return false;
    std::vector<unsigned char> header(size);
    return read(0,&header[0],size) && XexTitle(&header[0],size,titleId,disc);
}
bool FileTitle(const std::string& path, DWORD& titleId, int& disc) {
    File f; U64 size=0;
    if(!f.Open(path) || !f.Size(size)) return false;
    return HeaderTitle([&](U64 at, unsigned char* data, DWORD n) { return f.Read(at,data,n)==(long long)n; },
                       size>0xFFFFFFFFULL ? 0xFFFFFFFFu : DWORD(size),titleId,disc);
}
// As NetISO Sync finds default.xex (shared/library.cpp GameInfo).
bool ImageTitle(DiscImage& image, DWORD& titleId, int& disc) {
    static const U64 partitions[][2]={ { 0xFDA0000, 0xFD90000 }, { 0x2090000, 0x2080000 }, { 0x10000, 0 } };
    unsigned char block[32];
    U64 start=0; bool found=false;
    for(int i=0;i<3 && !found;++i) {
        if(image.Size()<partitions[i][0]+Sector) continue;
        if(image.Read(partitions[i][0],block,32) && !memcmp(block,Magic,MagicSize)) { start=partitions[i][1]; found=true; }
    }
    if(!found) return false;
    DWORD rootSector=Little32(block+20),rootSize=Little32(block+24);
    U64 rootAt=start+U64(rootSector)*Sector;
    if(!rootSize || rootSize>RootMax || rootAt+rootSize>image.Size()) return false;
    std::vector<unsigned char> root(rootSize);
    if(!image.Read(rootAt,&root[0],rootSize)) return false;
    for(DWORD pos=0;pos+14<=rootSize;) {
        if(root[pos]==0xFF && root[pos+1]==0xFF) { pos=(pos/Sector+1)*Sector; continue; }
        DWORD length=root[pos+13];
        if(!length || pos+14+length>rootSize) break;
        if(length==11 && !_strnicmp(reinterpret_cast<const char*>(&root[pos+14]),"default.xex",11)) {
            U64 xexAt=start+U64(Little32(&root[pos+4]))*Sector;
            DWORD xexSize=Little32(&root[pos+8]);
            if(xexAt+xexSize>image.Size()) return false;
            return HeaderTitle([&](U64 at, unsigned char* data, DWORD n) { return image.Read(xexAt+at,data,n); },xexSize,titleId,disc);
        }
        pos=(pos+14+length+3)&~3u;
    }
    return false;
}

struct Known { U64 size; long long modified; DWORD titleId; int disc; };
std::mutex TitlesLock;
std::map<std::string,Known> Titles;

int Rank(DiscKind kind) { return kind==DiscIso ? 0 : kind==DiscGod ? 1 : 2; }

// Finding discs --------------------------------------------------------------

bool IsIsoName(const std::string& name) { return name.size()>4 && !_stricmp(name.c_str()+name.size()-4,".iso"); }

void AddFolder(const std::string& path, const std::string& name, const std::vector<Entry>& entries, std::vector<IsoFile>& out) {
    IsoFile disc; disc.path=path; disc.kind=DiscFolder; disc.size=0; disc.titleId=0; disc.disc=0;
    const Entry* xex=Find(entries,"default.xex",false);
    disc.modified=xex ? xex->modified : 0;
    std::string clean=CleanName(name);
    disc.name=(clean.empty() ? std::string("Game") : clean)+".iso";
    out.push_back(disc);
}

// depth is that of the folder the header is in (00007000).
void AddGod(const std::string& folder, int depth, const Entry& header, std::vector<IsoFile>& out) {
    GodHeader h;
    std::string path=Join(folder,header.name);
    if(!ReadGodHeader(path,h)) return;
    IsoFile disc; disc.path=path; disc.kind=DiscGod; disc.modified=header.modified; disc.size=0;
    disc.titleId=h.titleId; disc.disc=h.disc ? h.disc : 1;
    std::vector<Entry> parts;
    if(List(path+".data",parts)) for(size_t i=0;i<parts.size();++i) disc.size+=parts[i].size;
    // <game>\<Title ID>\00007000\<header>: the game's folder names it, as the
    // user named it. Without one, the title in the package does.
    std::string name;
    if(depth>=3) name=CleanName(Leaf(Parent(Parent(folder))));
    if(name.empty()) name=CleanName(h.title);
    if(name.empty()) name=CleanName(header.name);
    if(h.discs>1 && h.disc>0) { char suffix[24]; _snprintf_s(suffix,sizeof(suffix),_TRUNCATE," - Disc %d",h.disc); name+=suffix; }
    disc.name=name+".iso";
    out.push_back(disc);
}

void Scan(const std::string& folder, int depth, std::vector<IsoFile>& out) {
    if(depth>MaxDepth) return;
    std::vector<Entry> entries;
    if(!List(folder,entries)) return;
    // A served folder itself is a library, not a game, even with a
    // default.xex left in it: the ISOs and folders in it are what count.
    if(depth>0 && Find(entries,"default.xex",false)) {
        AddFolder(folder,Leaf(folder),entries,out);
        for(size_t i=0;i<entries.size();++i) {
            std::string inside=Join(folder,entries[i].name);
            if(!entries[i].folder || !SeparateDisc(inside)) continue;
            std::vector<Entry> disc; List(inside,disc);
            AddFolder(inside,Leaf(folder)+" - "+entries[i].name,disc,out);
        }
        return;
    }
    // An original Xbox game, which NetISO can't start; no need to look inside.
    if(depth>0 && Find(entries,"default.xbe",false)) return;
    for(size_t i=0;i<entries.size();++i) {
        const Entry& e=entries[i];
        if(e.folder) continue;
        if(IsIsoName(e.name)) {
            IsoFile iso; iso.path=Join(folder,e.name); iso.name=e.name; iso.size=e.size; iso.modified=e.modified; iso.kind=DiscIso;
            iso.titleId=0; iso.disc=0;
            out.push_back(iso);
        } else if(Find(entries,e.name+".data",true)) AddGod(folder,depth,e,out);
    }
    for(size_t i=0;i<entries.size();++i) {
        const Entry& e=entries[i];
        if(!e.folder) continue;
        // A GOD package's data, read through its header.
        if(e.name.size()>5 && !_stricmp(e.name.c_str()+e.name.size()-5,".data") && Find(entries,e.name.substr(0,e.name.size()-5),false)) continue;
        Scan(Join(folder,e.name),depth+1,out);
    }
}
}

void FindIsos(const std::string& folder, std::vector<IsoFile>& out) {
    Scan(folder,0,out);
    std::sort(out.begin(),out.end(),[](const IsoFile& a, const IsoFile& b) {
        int order=_stricmp(a.name.c_str(),b.name.c_str());
        return order ? order<0 : a.path<b.path;
    });
}

void UniqueNames(std::vector<IsoFile>& discs) {
    for(size_t i=0;i<discs.size();++i) {
        if(discs[i].kind==DiscIso) continue;
        std::string name=discs[i].name;
        size_t cut=name.size()-4;  // Before ".iso".
        for(int n=2;;++n) {
            bool taken=false;
            for(size_t k=0;k<discs.size() && !taken;++k) taken=k!=i && !_stricmp(discs[k].name.c_str(),discs[i].name.c_str()) && (k<i || discs[k].kind==DiscIso);
            if(!taken) break;
            char number[16]; _snprintf_s(number,sizeof(number),_TRUNCATE," %d",n);
            discs[i].name=name.substr(0,cut)+number+name.substr(cut);
        }
    }
}

void ReadTitles(std::vector<IsoFile>& discs) {
    for(size_t i=0;i<discs.size();++i) {
        IsoFile& d=discs[i];
        if(d.kind==DiscGod) continue;  // From its header already.
        {
            std::lock_guard<std::mutex> hold(TitlesLock);
            std::map<std::string,Known>::const_iterator k=Titles.find(d.path);
            if(k!=Titles.end() && k->second.size==d.size && k->second.modified==d.modified) {
                d.titleId=k->second.titleId; d.disc=k->second.disc; continue;
            }
        }
        DWORD titleId=0; int disc=0;
        if(d.kind==DiscFolder) FileTitle(Join(d.path,"default.xex"),titleId,disc);
        else {
            IsoImage iso; std::string error;
            if(iso.Open(d.path,error)) ImageTitle(iso,titleId,disc);
        }
        d.titleId=titleId; d.disc=titleId ? disc : 0;
        Known known={ d.size, d.modified, d.titleId, d.disc };
        std::lock_guard<std::mutex> hold(TitlesLock);
        Titles[d.path]=known;
    }
}

void DropDuplicates(std::vector<IsoFile>& discs, std::vector<IsoFile>& dropped, std::vector<IsoFile>& kept) {
    dropped.clear(); kept.clear();
    std::vector<bool> drop(discs.size(),false);
    for(size_t i=0;i<discs.size();++i) {
        if(!discs[i].titleId || drop[i]) continue;
        size_t best=i;
        for(size_t k=i+1;k<discs.size();++k)
            if(discs[k].titleId==discs[i].titleId && discs[k].disc==discs[i].disc && Rank(discs[k].kind)<Rank(discs[best].kind)) best=k;
        for(size_t k=i;k<discs.size();++k) {
            if(k==best || drop[k] || discs[k].titleId!=discs[i].titleId || discs[k].disc!=discs[i].disc) continue;
            drop[k]=true; dropped.push_back(discs[k]); kept.push_back(discs[best]);
        }
    }
    std::vector<IsoFile> rest;
    for(size_t i=0;i<discs.size();++i) if(!drop[i]) rest.push_back(discs[i]);
    discs.swap(rest);
}

DiscImage* OpenFolderImage(const std::string& folder, std::string& error) {
    std::unique_ptr<FolderImage> image(new FolderImage);
    return image->Open(folder,error) ? image.release() : NULL;
}
DiscImage* OpenGodImage(const std::string& header, std::string& error) {
    std::unique_ptr<GodImage> image(new GodImage);
    return image->Open(header,error) ? image.release() : NULL;
}

bool IsoReader::Open(const IsoFile& disc) {
    Close();
    std::string error;
    DiscImage* image=NULL;
    if(disc.kind==DiscFolder) image=OpenFolderImage(disc.path,error);
    else if(disc.kind==DiscGod) image=OpenGodImage(disc.path,error);
    else {
        std::unique_ptr<IsoImage> iso(new IsoImage);
        if(iso->Open(disc.path,error)) image=iso.release();
    }
    if(!image) { note_=error; return false; }
    image_.reset(image);
    note_=image->Description();
    return true;
}
}
