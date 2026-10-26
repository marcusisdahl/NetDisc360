// SPDX-License-Identifier: MIT
#include "compress.h"
#include "config.h"
#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string.h>
#include <thread>
#ifndef _WIN32
#include <dirent.h>
#include <sys/statvfs.h>
#endif
#include "../third_party/miniz/miniz.h"
#if !defined(_WIN32) && defined(FILE_ATTRIBUTE_NORMAL)
#undef FILE_ATTRIBUTE_NORMAL  // tests/posix_compat.h's; 7zTypes.h has its own.
#endif
#include "../third_party/lzma/7z.h"
#include "../third_party/lzma/7zCrc.h"
#include "../third_party/lzma/Lzma2Dec.h"
#include "../third_party/lzma/LzmaDec.h"

namespace nia {
namespace {
enum { Sector = 2048, CsoHeader = 0x18, CsoBlock = 0x8000, ReadAhead = 0x200000, BatchBlocks = 64, MaxDepth = 8,
       TableMost = 0x4000000, Level = 6 };
const DWORD Plain = 0x80000000u;
const char Magic[] = "MICROSOFT*XBOX*MEDIA";
// Where a disc image's volume descriptor can be: a game partition at 0 (an
// image of the partition alone), XGD3, XGD2 or an original Xbox disc (XGD1);
// the partition starts 0x10000 before it.
const U64 Volumes[] = { 0x10000, 0x2090000, 0xFDA0000, 0x18310000 };

#ifdef _WIN32
const char Slash = '\\';
#else
const char Slash = '/';
#endif
DWORD Little32(const unsigned char* p) { return DWORD(p[0])|(DWORD(p[1])<<8)|(DWORD(p[2])<<16)|(DWORD(p[3])<<24); }
DWORD Little16(const unsigned char* p) { return DWORD(p[0])|(DWORD(p[1])<<8); }
void PutLittle32(unsigned char* p, DWORD v) { p[0]=BYTE(v); p[1]=BYTE(v>>8); p[2]=BYTE(v>>16); p[3]=BYTE(v>>24); }
U64 Little64(const unsigned char* p) { return U64(Little32(p))|(U64(Little32(p+4))<<32); }
bool EndsWith(const std::string& name, const char* end) {
    size_t n=strlen(end);
    return name.size()>n && !_stricmp(name.c_str()+name.size()-n,end);
}
std::string Leaf(const std::string& path) {
    size_t at=path.find_last_of("/\\");
    return at==std::string::npos ? path : path.substr(at+1);
}
std::string Parent(const std::string& path) {
    size_t at=path.find_last_of("/\\");
    return at==std::string::npos ? std::string(".") : path.substr(0,at);
}
std::string Stem(const std::string& name) {
    size_t dot=name.find_last_of('.');
    return dot==std::string::npos || !dot ? name : name.substr(0,dot);
}

// Files ------------------------------------------------------------------------

class RawFile {
public:
    RawFile() {
#ifdef _WIN32
        file_=INVALID_HANDLE_VALUE;
#else
        file_=-1;
#endif
    }
    ~RawFile() { Close(); }
    bool Open(const std::string& path, bool write) {
        Close();
#ifdef _WIN32
        file_=write ? CreateFileA(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL)
                    : CreateFileA(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
        return file_!=INVALID_HANDLE_VALUE;
#else
        file_=write ? open(path.c_str(),O_RDWR|O_CREAT|O_TRUNC,0644) : open(path.c_str(),O_RDONLY);
        return file_>=0;
#endif
    }
    bool IsOpen() const {
#ifdef _WIN32
        return file_!=INVALID_HANDLE_VALUE;
#else
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
    long long Read(U64 offset, void* data, size_t length) {
        size_t total=0;
        unsigned char* at=static_cast<unsigned char*>(data);
        while(total<length) {
            DWORD want=DWORD(std::min<size_t>(length-total,0x40000000));
#ifdef _WIN32
            OVERLAPPED o; memset(&o,0,sizeof(o)); o.Offset=DWORD(offset); o.OffsetHigh=DWORD(offset>>32);
            DWORD got=0;
            if(!::ReadFile(file_,at+total,want,&got,&o)) return GetLastError()==ERROR_HANDLE_EOF ? (long long)total : -1;
#else
            ssize_t got=pread(file_,at+total,want,off_t(offset));
            if(got<0) { if(errno==EINTR) continue; return -1; }
#endif
            if(!got) break;
            total+=size_t(got); offset+=U64(got);
        }
        return (long long)total;
    }
    bool Write(U64 offset, const void* data, size_t length) {
        const unsigned char* at=static_cast<const unsigned char*>(data);
        while(length) {
            DWORD want=DWORD(std::min<size_t>(length,0x40000000));
#ifdef _WIN32
            OVERLAPPED o; memset(&o,0,sizeof(o)); o.Offset=DWORD(offset); o.OffsetHigh=DWORD(offset>>32);
            DWORD put=0;
            if(!::WriteFile(file_,at,want,&put,&o) || !put) return false;
#else
            ssize_t put=pwrite(file_,at,want,off_t(offset));
            if(put<0 && errno==EINTR) continue;
            if(put<=0) return false;
#endif
            at+=put; length-=size_t(put); offset+=U64(put);
        }
        return true;
    }
private:
    RawFile(const RawFile&);
    RawFile& operator=(const RawFile&);
#ifdef _WIN32
    HANDLE file_;
#else
    int file_;
#endif
};

bool RemovePath(const std::string& path) {
#ifdef _WIN32
    return DeleteFileA(path.c_str())!=0;
#else
    return remove(path.c_str())==0;
#endif
}
bool Exists(const std::string& path) {
#ifdef _WIN32
    return GetFileAttributesA(path.c_str())!=INVALID_FILE_ATTRIBUTES;
#else
    struct stat st; return stat(path.c_str(),&st)==0;
#endif
}
// Without replacing a file already at to.
bool RenamePath(const std::string& from, const std::string& to) {
#ifdef _WIN32
    return MoveFileExA(from.c_str(),to.c_str(),0)!=0;
#else
    if(Exists(to)) return false;
    return rename(from.c_str(),to.c_str())==0;
#endif
}

struct Item { std::string name; bool folder; U64 size; long long modified; };
bool ListFolder(const std::string& folder, std::vector<Item>& out) {
    out.clear();
#ifdef _WIN32
    WIN32_FIND_DATAA found;
    HANDLE search=FindFirstFileA((folder+"\\*").c_str(),&found);
    if(search==INVALID_HANDLE_VALUE) return false;
    do {
        if(!strcmp(found.cFileName,".") || !strcmp(found.cFileName,"..")) continue;
        Item e; e.name=found.cFileName;
        e.folder=(found.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
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
        struct stat st; if(lstat((folder+Slash+d->d_name).c_str(),&st)) continue;
        if(!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) continue;
        Item e; e.name=d->d_name; e.folder=S_ISDIR(st.st_mode); e.size=e.folder ? 0 : U64(st.st_size); e.modified=(long long)st.st_mtime;
        out.push_back(e);
    }
    closedir(dir);
#endif
    return true;
}

// CSO files --------------------------------------------------------------------
//
// A 0x18-byte header ("CISO", header size, image bytes, block size, version
// 1, index shift), then one 32-bit entry per block and one after the last:
// where the block starts in the file, shifted right by the index shift, with
// the top bit set for a block kept as it is; any other is raw deflate.

struct CsoLayout { U64 total; DWORD block; int shift; size_t blocks; };

class CsoImage : public DiscImage {
public:
    CsoImage() : next_(0), packedAt_(0) { for(int i=0;i<Slots;++i) slot_[i]=~U64(0); }
    bool Open(const std::string& path, std::string& error) {
        unsigned char h[CsoHeader];
        U64 fileBytes=0;
        if(!file_.Open(path,false) || !file_.Size(fileBytes)) { error="it can't be opened"; return false; }
        if(file_.Read(0,h,sizeof(h))!=(long long)sizeof(h) || memcmp(h,"CISO",4)) { error="it isn't a CSO file"; return false; }
        layout_.total=Little64(h+8); layout_.block=Little32(h+16); layout_.shift=h[21];
        if(h[20]>1) { error="it is a CSO version 2 file, which NetDisc360 can't read (make it with maxcso --format=cso1)"; return false; }
        if(layout_.block<Sector || layout_.block>0x100000 || (layout_.block&(layout_.block-1)) || layout_.shift>16 ||
           !layout_.total || layout_.total>(U64(1)<<40)) { error="its header is damaged"; return false; }
        layout_.blocks=size_t((layout_.total+layout_.block-1)/layout_.block);
        index_.resize(layout_.blocks+1);
        size_t bytes=index_.size()*4;
        std::vector<unsigned char> raw(bytes);
        if(CsoHeader+U64(bytes)>fileBytes || file_.Read(CsoHeader,&raw[0],bytes)!=(long long)bytes) { error="it ends early"; return false; }
        for(size_t i=0;i<index_.size();++i) index_[i]=Little32(&raw[i*4]);
        for(size_t i=0;i<layout_.blocks;++i)
            if(Start(i+1)<Start(i)) { error="its block table is damaged"; return false; }
        if(Start(layout_.blocks)>fileBytes) { error="it ends early"; return false; }
        for(int i=0;i<Slots;++i) data_[i].resize(layout_.block);
        return true;
    }
    U64 Size() const { return layout_.total; }
    bool Read(U64 offset, unsigned char* data, DWORD length) {
        if(offset>layout_.total || length>layout_.total-offset) return false;
        size_t last=length ? size_t((offset+length-1)/layout_.block) : 0;
        while(length) {
            size_t b=size_t(offset/layout_.block);
            DWORD in=DWORD(offset%layout_.block), n=std::min<DWORD>(length,BlockBytes(b)-in);
            const unsigned char* block=Block(b,last);
            if(!block) return false;
            memcpy(data,block+in,n);
            data+=n; offset+=n; length-=n;
        }
        return true;
    }
    std::string Description() const { return "compressed (CSO)"; }
private:
    enum { Slots = 4 };
    U64 Start(size_t b) const { return U64(index_[b]&~Plain)<<layout_.shift; }
    DWORD BlockBytes(size_t b) const { return DWORD(std::min<U64>(layout_.block,layout_.total-U64(b)*layout_.block)); }
    // Block b unpacked; last is the last block of the read, whose packed
    // bytes are read along with b's.
    const unsigned char* Block(size_t b, size_t last) {
        for(int i=0;i<Slots;++i) if(slot_[i]==b) return &data_[i][0];
        U64 from=Start(b), to=b+1<=layout_.blocks ? U64(index_[b+1]&~Plain)<<layout_.shift : from;
        if(from<packedAt_ || to>packedAt_+packed_.size()) {
            size_t end=b+1;
            while(end<=last && end<layout_.blocks && Start(end+1)-from<=ReadAhead) ++end;
            U64 until=Start(end);
            if(until-from>TableMost) return NULL;
            packed_.resize(size_t(until-from));
            packedAt_=from;
            if(!packed_.empty() && file_.Read(from,&packed_[0],packed_.size())!=(long long)packed_.size()) { packed_.clear(); return NULL; }
        }
        int s=next_; next_=(next_+1)%Slots;
        slot_[s]=~U64(0);
        DWORD want=BlockBytes(b);
        const unsigned char* src=packed_.empty() ? NULL : &packed_[size_t(from-packedAt_)];
        size_t have=size_t(to-from);
        if(index_[b]&Plain) {
            if(have<want) return NULL;
            memcpy(&data_[s][0],src,want);
        } else if(!have || tinfl_decompress_mem_to_mem(&data_[s][0],want,src,have,0)!=want) return NULL;
        slot_[s]=b;
        return &data_[s][0];
    }
    RawFile file_;
    CsoLayout layout_;
    std::vector<DWORD> index_;
    U64 slot_[Slots];
    std::vector<unsigned char> data_[Slots];
    int next_;
    std::vector<unsigned char> packed_;
    U64 packedAt_;
};

// Sources: an ISO file, or one inside an archive --------------------------------
//
// A disc in an archive can only be unpacked from its start, so reading it
// further back than the last read starts it over; reading ahead unpacks the
// bytes between.

class IsoSource : public DiscImage {
public:
    bool Open(const std::string& path, std::string& error) {
        if(!file_.Open(path,false) || !file_.Size(size_)) { error="it can't be opened"; return false; }
        return true;
    }
    U64 Size() const { return size_; }
    bool Read(U64 offset, unsigned char* data, DWORD length) { return file_.Read(offset,data,length)==(long long)length; }
    std::string Description() const { return "ISO"; }
private:
    RawFile file_;
    U64 size_;
};

class StreamSource : public DiscImage {
public:
    StreamSource() : size_(0), at_(0), started_(false), restarts_(0), crc_(0) {}
    U64 Size() const { return size_; }
    bool Read(U64 offset, unsigned char* data, DWORD length) {
        if(offset>size_ || length>size_-offset) return false;
        if(!started_ || offset<at_) {
            if(started_) ++restarts_;
            if(!Restart()) return false;
            started_=true; at_=0; crc_=0;
        }
        if(!Skip(offset-at_)) return false;
        return Take(data,length);
    }
    // Unpacks the rest, so the archive's own check of the disc runs.
    bool Finish() {
        if(!started_) return false;
        if(!Skip(size_-at_)) return false;
        return Check(crc_);
    }
    int Restarts() const { return restarts_; }
    std::string Description() const { return "archive"; }
    const std::string& Error() const { return error_; }
protected:
    virtual bool Restart() = 0;
    virtual bool Next(unsigned char* data, size_t length) = 0;  // Exactly length bytes, or false.
    virtual bool Check(DWORD crc) = 0;  // crc: CRC-32 of the whole disc.
    U64 size_;
    std::string error_;
private:
    bool Take(unsigned char* data, size_t length) {
        if(!Next(data,length)) return false;
        crc_=DWORD(mz_crc32(crc_,data,length));
        at_+=length;
        return true;
    }
    bool Skip(U64 bytes) {
        if(bytes && scratch_.empty()) scratch_.resize(0x100000);
        while(bytes) {
            size_t n=size_t(std::min<U64>(bytes,scratch_.size()));
            if(!Take(&scratch_[0],n)) return false;
            bytes-=n;
        }
        return true;
    }
    U64 at_;
    bool started_;
    int restarts_;
    DWORD crc_;
    std::vector<unsigned char> scratch_;
};

// .7z ---------------------------------------------------------------------------
//
// Through the LZMA SDK's reading of the archive's header. A folder (a run of
// files packed together) is unpacked here, as it streams, so a disc of
// several GB never has to fit in memory: one coder (LZMA, LZMA2 or none) and
// one packed stream, as 7-Zip packs ISOs; filters (BCJ, ...) and encrypted
// archives aren't read.

void* SzAllocate(ISzAllocPtr, size_t size) { return size ? malloc(size) : NULL; }
void SzRelease(ISzAllocPtr, void* address) { free(address); }
const ISzAlloc Allocator = { SzAllocate, SzRelease };
std::once_flag CrcTable;

struct SeekFile {
    ISeekInStream vt;
    RawFile* file;
    U64 at, size;
};
SRes SeekFileRead(ISeekInStreamPtr p, void* buf, size_t* size) {
    SeekFile* f=(SeekFile*)p;
    long long got=f->file->Read(f->at,buf,*size);
    if(got<0) { *size=0; return SZ_ERROR_READ; }
    *size=size_t(got); f->at+=U64(got);
    return SZ_OK;
}
SRes SeekFileSeek(ISeekInStreamPtr p, Int64* pos, ESzSeek origin) {
    SeekFile* f=(SeekFile*)p;
    Int64 base=origin==SZ_SEEK_SET ? 0 : origin==SZ_SEEK_CUR ? Int64(f->at) : Int64(f->size);
    if(base+*pos<0) return SZ_ERROR_PARAM;
    f->at=U64(base+*pos); *pos=Int64(f->at);
    return SZ_OK;
}

enum { MethodCopy = 0, MethodLzma = 0x030101, MethodLzma2 = 0x21 };

class SevenZip {
public:
    SevenZip() : open_(false) { SzArEx_Init(&db_); }
    ~SevenZip() { if(open_) SzArEx_Free(&db_,&Allocator); }
    bool Open(const std::string& path, std::string& error) {
        std::call_once(CrcTable,[] { CrcGenerateTable(); });
        if(!file_.Open(path,false) || !file_.Size(size_)) { error="it can't be opened"; return false; }
        seek_.vt.Read=SeekFileRead; seek_.vt.Seek=SeekFileSeek; seek_.file=&file_; seek_.at=0; seek_.size=size_;
        buffer_.resize(1<<16);
        LookToRead2_CreateVTable(&look_,False);
        look_.buf=&buffer_[0]; look_.bufSize=buffer_.size(); look_.realStream=&seek_.vt;
        LookToRead2_INIT(&look_);
        SRes r=SzArEx_Open(&db_,&look_.vt,&Allocator,&Allocator);
        if(r!=SZ_OK) { error=r==SZ_ERROR_NO_ARCHIVE ? "it isn't a 7z archive" : r==SZ_ERROR_UNSUPPORTED ? "its header uses something NetDisc360 can't read (an encrypted header?)" : "it is damaged"; return false; }
        open_=true;
        return true;
    }
    // The ISOs in it, as file numbers.
    void Isos(std::vector<UInt32>& files, std::vector<ArchiveDisc>& discs) {
        for(UInt32 i=0;i<db_.NumFiles;++i) {
            if(SzArEx_IsDir(&db_,i)) continue;
            std::string name=Name(i);
            if(!EndsWith(name,".iso") || !SzArEx_GetFileSize(&db_,i)) continue;
            ArchiveDisc d; d.name=Leaf(name); d.size=SzArEx_GetFileSize(&db_,i);
            files.push_back(i); discs.push_back(d);
        }
    }
    const CSzArEx& Db() const { return db_; }
    RawFile& File() { return file_; }
private:
    std::string Name(UInt32 i) {
        size_t n=SzArEx_GetFileNameUtf16(&db_,i,NULL);
        std::vector<UInt16> wide(n+1);
        SzArEx_GetFileNameUtf16(&db_,i,&wide[0]);
        std::string out;
        for(size_t k=0;k<n && wide[k];++k) {
            unsigned c=wide[k];
            if(c>=0xD800 && c<0xDC00 && k+1<n && wide[k+1]>=0xDC00 && wide[k+1]<0xE000) { c=0x10000+((c-0xD800)<<10)+(wide[k+1]-0xDC00); ++k; }
            if(c<0x80) out+=char(c);
            else if(c<0x800) { out+=char(0xC0|(c>>6)); out+=char(0x80|(c&0x3F)); }
            else if(c<0x10000) { out+=char(0xE0|(c>>12)); out+=char(0x80|((c>>6)&0x3F)); out+=char(0x80|(c&0x3F)); }
            else { out+=char(0xF0|(c>>18)); out+=char(0x80|((c>>12)&0x3F)); out+=char(0x80|((c>>6)&0x3F)); out+=char(0x80|(c&0x3F)); }
        }
        return out;
    }
    RawFile file_;
    U64 size_;
    SeekFile seek_;
    std::vector<unsigned char> buffer_;
    CLookToRead2 look_;
    CSzArEx db_;
    bool open_;
};

class SevenZipSource : public StreamSource {
public:
    SevenZipSource() : method_(0), lzmaReady_(false), lzma2Ready_(false) { LzmaDec_CONSTRUCT(&lzma_); Lzma2Dec_CONSTRUCT(&lzma2_); }
    ~SevenZipSource() { if(lzmaReady_) LzmaDec_Free(&lzma_,&Allocator); if(lzma2Ready_) Lzma2Dec_Free(&lzma2_,&Allocator); }
    bool Open(const std::string& path, int entry, std::string& error) {
        if(!archive_.Open(path,error)) return false;
        std::vector<UInt32> files; std::vector<ArchiveDisc> discs;
        archive_.Isos(files,discs);
        if(entry<0 || size_t(entry)>=files.size()) { error="that disc isn't in it"; return false; }
        const CSzArEx& db=archive_.Db();
        file_=files[entry];
        UInt32 folder=db.FileToFolder[file_];
        if(folder==(UInt32)-1) { error="that disc is empty"; return false; }
        CSzFolder f; CSzData sd;
        const Byte* props=db.db.CodersData+db.db.FoCodersOffsets[folder];
        sd.Data=props; sd.Size=db.db.FoCodersOffsets[folder+1]-db.db.FoCodersOffsets[folder];
        if(SzGetNextFolderItem(&f,&sd)!=SZ_OK || f.NumCoders!=1 || f.NumPackStreams!=1) {
            error="it is packed with a filter or several methods; extract it with 7-Zip first"; return false;
        }
        method_=f.Coders[0].MethodID;
        if(method_==MethodLzma) {
            if(LzmaDec_Allocate(&lzma_,props+f.Coders[0].PropsOffset,f.Coders[0].PropsSize,&Allocator)!=SZ_OK) { error="its LZMA settings can't be read"; return false; }
            lzmaReady_=true;
        } else if(method_==MethodLzma2) {
            if(f.Coders[0].PropsSize!=1 || Lzma2Dec_Allocate(&lzma2_,props[f.Coders[0].PropsOffset],&Allocator)!=SZ_OK) { error="its LZMA2 settings can't be read"; return false; }
            lzma2Ready_=true;
        } else if(method_!=MethodCopy) {
            error=method_==0x06F10701 ? "it is encrypted; extract it with 7-Zip first" : "it is packed with a method NetDisc360 can't read (only LZMA and LZMA2); extract it with 7-Zip first";
            return false;
        }
        UInt32 pack=db.db.FoStartPackStreamIndex[folder];
        packFrom_=db.dataPos+db.db.PackPositions[pack];
        packTo_=db.dataPos+db.db.PackPositions[pack+1];
        skip_=db.UnpackPositions[file_]-db.UnpackPositions[db.FolderToFile[folder]];
        size_=SzArEx_GetFileSize(&db,file_);
        hasCrc_=SzBitWithVals_Check(&db.CRCs,file_);
        crc_=hasCrc_ ? db.CRCs.Vals[file_] : 0;
        return true;
    }
protected:
    bool Restart() {
        readAt_=packFrom_; inAt_=inHave_=0;
        if(in_.empty()) in_.resize(0x100000);
        if(method_==MethodLzma) LzmaDec_Init(&lzma_);
        else if(method_==MethodLzma2) Lzma2Dec_Init(&lzma2_);
        // The files packed before this one in the folder.
        std::vector<unsigned char> skip(0x100000);
        for(U64 left=skip_;left;) {
            size_t n=size_t(std::min<U64>(left,skip.size()));
            if(!Unpack(&skip[0],n)) return false;
            left-=n;
        }
        return true;
    }
    bool Next(unsigned char* data, size_t length) { return Unpack(data,length); }
    bool Check(DWORD crc) {
        if(hasCrc_ && crc!=crc_) { error_="the disc in the archive is damaged (its CRC doesn't match)"; return false; }
        return true;
    }
private:
    bool Fill() {
        if(inAt_<inHave_) return true;
        if(readAt_>=packTo_) return false;
        size_t n=size_t(std::min<U64>(in_.size(),packTo_-readAt_));
        long long got=archive_.File().Read(readAt_,&in_[0],n);
        if(got<=0) return false;
        readAt_+=U64(got); inAt_=0; inHave_=size_t(got);
        return true;
    }
    bool Unpack(unsigned char* data, size_t length) {
        while(length) {
            if(!Fill()) { error_="the archive ends early"; return false; }
            SizeT out=length, in=inHave_-inAt_;
            if(method_==MethodCopy) {
                out=std::min<SizeT>(out,in); memcpy(data,&in_[inAt_],out); in=out;
            } else {
                ELzmaStatus status;
                SRes r=method_==MethodLzma ? LzmaDec_DecodeToBuf(&lzma_,data,&out,&in_[inAt_],&in,LZMA_FINISH_ANY,&status)
                                           : Lzma2Dec_DecodeToBuf(&lzma2_,data,&out,&in_[inAt_],&in,LZMA_FINISH_ANY,&status);
                if(r!=SZ_OK) { error_="the archive is damaged"; return false; }
                if(!out && !in) { error_="the archive ends early"; return false; }
            }
            inAt_+=in; data+=out; length-=out;
        }
        return true;
    }
    SevenZip archive_;
    UInt32 file_, method_;
    CLzmaDec lzma_;
    CLzma2Dec lzma2_;
    bool lzmaReady_, lzma2Ready_, hasCrc_;
    DWORD crc_;
    U64 packFrom_, packTo_, skip_, readAt_;
    std::vector<unsigned char> in_;
    size_t inAt_, inHave_;
};

// .zip --------------------------------------------------------------------------
//
// Through miniz: stored or deflated entries, Zip64 for discs over 4 GB.

size_t ZipRead(void* opaque, mz_uint64 offset, void* buf, size_t n) {
    long long got=static_cast<RawFile*>(opaque)->Read(offset,buf,n);
    return got<0 ? 0 : size_t(got);
}

class Zip {
public:
    Zip() : open_(false) { memset(&zip_,0,sizeof(zip_)); }
    ~Zip() { if(open_) mz_zip_reader_end(&zip_); }
    bool Open(const std::string& path, std::string& error) {
        U64 size=0;
        if(!file_.Open(path,false) || !file_.Size(size)) { error="it can't be opened"; return false; }
        zip_.m_pRead=ZipRead; zip_.m_pIO_opaque=&file_;
        if(!mz_zip_reader_init(&zip_,size,0)) { error="it isn't a zip archive, or it is damaged"; return false; }
        open_=true;
        return true;
    }
    void Isos(std::vector<mz_uint>& files, std::vector<ArchiveDisc>& discs) {
        for(mz_uint i=0;i<mz_zip_reader_get_num_files(&zip_);++i) {
            mz_zip_archive_file_stat st;
            if(!mz_zip_reader_file_stat(&zip_,i,&st) || st.m_is_directory || !st.m_uncomp_size) continue;
            if(!EndsWith(st.m_filename,".iso")) continue;
            ArchiveDisc d; d.name=Leaf(st.m_filename); d.size=st.m_uncomp_size;
            files.push_back(i); discs.push_back(d);
        }
    }
    mz_zip_archive* Archive() { return &zip_; }
private:
    RawFile file_;
    mz_zip_archive zip_;
    bool open_;
};

class ZipSource : public StreamSource {
public:
    ZipSource() : iter_(NULL) {}
    ~ZipSource() { if(iter_) mz_zip_reader_extract_iter_free(iter_); }
    bool Open(const std::string& path, int entry, std::string& error) {
        if(!zip_.Open(path,error)) return false;
        std::vector<mz_uint> files; std::vector<ArchiveDisc> discs;
        zip_.Isos(files,discs);
        if(entry<0 || size_t(entry)>=files.size()) { error="that disc isn't in it"; return false; }
        file_=files[entry]; size_=discs[entry].size;
        mz_zip_archive_file_stat st;
        if(!mz_zip_reader_file_stat(zip_.Archive(),file_,&st)) { error="it is damaged"; return false; }
        if(st.m_is_encrypted) { error="it is encrypted; extract it first"; return false; }
        if(st.m_method!=0 && st.m_method!=MZ_DEFLATED) { error="it is packed with a method NetDisc360 can't read (only deflate); extract it first"; return false; }
        return true;
    }
protected:
    bool Restart() {
        if(iter_) mz_zip_reader_extract_iter_free(iter_);
        iter_=mz_zip_reader_extract_iter_new(zip_.Archive(),file_,0);
        if(!iter_) { error_="the disc in the archive can't be read"; return false; }
        return true;
    }
    bool Next(unsigned char* data, size_t length) {
        while(length) {
            size_t got=mz_zip_reader_extract_iter_read(iter_,data,length);
            if(!got) { error_="the archive is damaged or ends early"; return false; }
            data+=got; length-=got;
        }
        return true;
    }
    // miniz checks the CRC itself once the whole entry is read.
    bool Check(DWORD) {
        mz_zip_reader_extract_iter_state* done=iter_; iter_=NULL;
        if(!mz_zip_reader_extract_iter_free(done)) { error_="the disc in the archive is damaged (its CRC doesn't match)"; return false; }
        return true;
    }
private:
    Zip zip_;
    mz_uint file_;
    mz_zip_reader_extract_iter_state* iter_;
};

bool IsZipName(const std::string& name) { return EndsWith(name,".zip"); }

// The sectors a disc's files use --------------------------------------------------
//
// The game partition's volume descriptor names the root directory table;
// each table is a binary tree of entries (left and right child offsets in
// 4-byte units, sector, size, attributes, name), padded with 0xFF. The tree
// is walked, and each table is read through from its start too, as
// discs.cpp reads them, and what either finds counts. If anything doesn't
// add up (a link or a file outside the disc), the disc is kept whole. Tables
// are read in the order they are on the disc, so an archive is unpacked
// from its start as few times as can be.

struct GdfEntry { DWORD sector, size; bool folder; };

// The offsets of a table's entries, through its tree from the first entry
// (false if a link leads outside it).
bool TreeOffsets(const std::vector<unsigned char>& table, std::set<DWORD>& offsets) {
    if(table.size()<14 || (table[0]==0xFF && table[1]==0xFF)) return true;
    std::vector<DWORD> pending(1,0);
    while(!pending.empty()) {
        DWORD at=pending.back(); pending.pop_back();
        if(!offsets.insert(at).second) continue;
        if(U64(at)+14>table.size()) return false;
        DWORD left=Little16(&table[at]), right=Little16(&table[at+2]);
        if(left && left!=0xFFFF) pending.push_back(left*4);
        if(right && right!=0xFFFF) pending.push_back(right*4);
    }
    return true;
}
// As discs.cpp ReadTable: in the order they are stored.
void StoredOffsets(const std::vector<unsigned char>& table, std::set<DWORD>& offsets) {
    DWORD size=DWORD(table.size());
    for(DWORD pos=0;pos+14<=size;) {
        if(table[pos]==0xFF && table[pos+1]==0xFF) { pos=(pos/Sector+1)*Sector; continue; }
        DWORD length=table[pos+13];
        if(!length || pos+14+length>size) break;
        offsets.insert(pos);
        pos=(pos+14+length+3)&~3u;
    }
}
// Every entry either way finds, so no file is missed.
bool TableEntries(const std::vector<unsigned char>& table, std::vector<GdfEntry>& out) {
    std::set<DWORD> offsets;
    if(!TreeOffsets(table,offsets)) return false;
    StoredOffsets(table,offsets);
    for(std::set<DWORD>::const_iterator o=offsets.begin();o!=offsets.end();++o) {
        DWORD at=*o, length=table[at+13];
        if(!length || U64(at)+14+length>table.size()) return false;
        GdfEntry e; e.sector=Little32(&table[at+4]); e.size=Little32(&table[at+8]); e.folder=(table[at+12]&0x10)!=0;
        out.push_back(e);
    }
    return true;
}

class UsedMap {
public:
    explicit UsedMap(U64 bytes) : bits_(size_t((bytes+Sector-1)/Sector),false), kept_(0) {}
    void Mark(U64 from, U64 bytes) {
        if(!bytes) return;
        size_t a=size_t(from/Sector), b=std::min(bits_.size(),size_t((from+bytes+Sector-1)/Sector));
        for(size_t i=a;i<b;++i) if(!bits_[i]) { bits_[i]=true; kept_+=Sector; }
    }
    void All() { std::fill(bits_.begin(),bits_.end(),true); kept_=U64(bits_.size())*Sector; }
    bool Used(size_t sector) const { return sector<bits_.size() && bits_[sector]; }
    U64 Kept() const { return kept_; }
private:
    std::vector<bool> bits_;
    U64 kept_;
};

bool FindFiles(DiscImage& image, UsedMap& used, const std::atomic<bool>* cancel) {
    U64 partition=0; DWORD rootSector=0, rootSize=0;
    bool found=false;
    unsigned char vd[Sector];
    for(size_t i=0;i<sizeof(Volumes)/sizeof(Volumes[0]) && !found;++i) {
        if(image.Size()<Volumes[i]+Sector) continue;
        if(image.Read(Volumes[i],vd,Sector) && !memcmp(vd,Magic,20)) {
            partition=Volumes[i]-0x10000; rootSector=Little32(vd+20); rootSize=Little32(vd+24); found=true;
        }
    }
    if(!found || !rootSize) return false;
    // The start of the image, and the partition's reserved sectors and volume descriptor.
    used.Mark(0,0x10000);
    used.Mark(partition,0x10000+Sector);
    // Tables still to read, by where they are.
    std::map<U64,DWORD> pending;
    std::set<U64> seen;
    pending[partition+U64(rootSector)*Sector]=rootSize;
    U64 last=0;
    size_t tables=0;
    while(!pending.empty()) {
        if(cancel && *cancel) return false;
        std::map<U64,DWORD>::iterator next=pending.lower_bound(last);
        if(next==pending.end()) next=pending.begin();
        U64 at=next->first; DWORD size=next->second;
        pending.erase(next);
        if(!seen.insert(at).second) continue;
        if(++tables>100000 || size>TableMost || at+size>image.Size()) return false;
        std::vector<unsigned char> table(size);
        if(!image.Read(at,&table[0],size)) return false;
        last=at+size;
        used.Mark(at,size);
        std::vector<GdfEntry> entries;
        if(!TableEntries(table,entries)) return false;
        for(size_t i=0;i<entries.size();++i) {
            const GdfEntry& e=entries[i];
            U64 from=partition+U64(e.sector)*Sector;
            if(!e.size) continue;
            if(from+e.size>image.Size()) return false;
            if(e.folder) pending[from]=e.size;
            else used.Mark(from,e.size);
        }
    }
    return true;
}

// Writing ------------------------------------------------------------------------

// The smallest index shift that lets every block start be told, even if no block shrinks.
int ShiftFor(U64 total, size_t blocks) {
    for(int shift=0;shift<16;++shift) {
        U64 worst=CsoHeader+U64(blocks+1)*4+total+U64(blocks)*(U64(1)<<shift);
        if(worst<(U64(0x80000000u)<<shift)) return shift;
    }
    return 16;
}

bool Fail(std::string& error, const std::string& why) { error=why; return false; }
}

bool IsCsoName(const std::string& name) { return EndsWith(name,".cso"); }
bool IsArchiveName(const std::string& name) { return EndsWith(name,".7z") || IsZipName(name); }

DiscImage* OpenCsoImage(const std::string& path, std::string& error) {
    std::unique_ptr<CsoImage> image(new CsoImage);
    return image->Open(path,error) ? image.release() : NULL;
}

bool ListArchive(const std::string& path, std::vector<ArchiveDisc>& discs, std::string& error) {
    discs.clear();
    if(IsZipName(path)) {
        Zip zip; std::vector<mz_uint> files;
        if(!zip.Open(path,error)) return false;
        zip.Isos(files,discs);
    } else {
        SevenZip archive; std::vector<UInt32> files;
        if(!archive.Open(path,error)) return false;
        archive.Isos(files,discs);
    }
    return true;
}

std::string CsoPathFor(const std::string& iso) { return Parent(iso)+Slash+Stem(Leaf(iso))+".cso"; }
std::string CsoPathFor(const std::string& archive, const std::vector<ArchiveDisc>& discs, size_t index) {
    if(discs.size()<=1) return CsoPathFor(archive);
    std::string name=config::FromUtf8(Stem(discs[index].name)), clean;
    for(size_t i=0;i<name.size();++i) clean+=strchr("\\/:*?\"<>|",name[i]) || (unsigned char)name[i]<' ' ? '_' : name[i];
    return Parent(archive)+Slash+clean+".cso";
}

namespace {
struct KnownArchive { U64 size; long long modified; std::vector<ArchiveDisc> discs; std::string problem; };
std::mutex ArchivesLock;
std::map<std::string,KnownArchive> Archives;

void ScanArchives(const std::string& folder, int depth, std::vector<ArchiveFile>& out) {
    if(depth>MaxDepth) return;
    std::vector<Item> items;
    if(!ListFolder(folder,items)) return;
    for(size_t i=0;i<items.size();++i) {
        const Item& e=items[i];
        std::string path=folder+Slash+e.name;
        if(e.folder) {
            // A GOD package's data and an extracted game's files hold no archives to play.
            if(!EndsWith(e.name,".data")) ScanArchives(path,depth+1,out);
            continue;
        }
        if(!IsArchiveName(e.name)) continue;
        ArchiveFile a; a.path=path; a.size=e.size; a.modified=e.modified;
        bool known=false;
        {
            std::lock_guard<std::mutex> hold(ArchivesLock);
            std::map<std::string,KnownArchive>::const_iterator k=Archives.find(path);
            if(k!=Archives.end() && k->second.size==e.size && k->second.modified==e.modified) { a.discs=k->second.discs; a.problem=k->second.problem; known=true; }
        }
        if(!known) {
            if(!ListArchive(path,a.discs,a.problem)) a.discs.clear();
            KnownArchive k; k.size=e.size; k.modified=e.modified; k.discs=a.discs; k.problem=a.problem;
            std::lock_guard<std::mutex> hold(ArchivesLock);
            Archives[path]=k;
        }
        if(a.discs.empty() && a.problem.empty()) continue;  // No ISO in it.
        for(size_t d=0;d<a.discs.size();++d) a.outputs.push_back(CsoPathFor(path,a.discs,d));
        out.push_back(a);
    }
}
}

void FindArchives(const std::string& folder, std::vector<ArchiveFile>& out) { ScanArchives(folder,0,out); }

U64 FreeBytes(const std::string& folder) {
#ifdef _WIN32
    ULARGE_INTEGER free;
    return GetDiskFreeSpaceExA(folder.c_str(),&free,NULL,NULL) ? U64(free.QuadPart) : 0;
#else
    struct statvfs s;
    return statvfs(folder.c_str(),&s) ? 0 : U64(s.f_bavail)*U64(s.f_frsize);
#endif
}

bool CompressDisc(const std::string& source, int entry, const std::string& output, bool keepWhole,
                  CompressProgress& progress, const std::atomic<bool>* cancel, CompressResult& result, std::string& error) {
    result=CompressResult();
    std::unique_ptr<DiscImage> image;
    StreamSource* stream=NULL;
    if(entry<0) {
        std::unique_ptr<IsoSource> iso(new IsoSource);
        if(!iso->Open(source,error)) return false;
        image.reset(iso.release());
    } else if(IsZipName(source)) {
        std::unique_ptr<ZipSource> zip(new ZipSource);
        if(!zip->Open(source,entry,error)) return false;
        stream=zip.get(); image.reset(zip.release());
    } else {
        std::unique_ptr<SevenZipSource> sz(new SevenZipSource);
        if(!sz->Open(source,entry,error)) return false;
        stream=sz.get(); image.reset(sz.release());
    }
    const U64 total=image->Size();
    if(!total) return Fail(error,"the disc is empty");
    result.imageBytes=total;
    progress.total=total; progress.done=0; progress.step=StepTable;
    auto streamError=[&](const char* fallback) { return stream && !stream->Error().empty() ? stream->Error() : std::string(fallback); };

    UsedMap used(total);
    if(keepWhole || !FindFiles(*image,used,cancel)) { used.All(); result.whole=true; }
    if(cancel && *cancel) return Fail(error,"stopped");
    if(stream && !stream->Error().empty()) return Fail(error,stream->Error());
    result.keptBytes=std::min(used.Kept(),total);

    CsoLayout layout;
    layout.total=total; layout.block=CsoBlock; layout.blocks=size_t((total+CsoBlock-1)/CsoBlock);
    layout.shift=ShiftFor(total,layout.blocks);
    std::string folder=Parent(output);
    U64 room=FreeBytes(folder), need=result.keptBytes+U64(layout.blocks+1)*4+(U64(64)<<20);
    if(room && room<need) {
        char text[160]; _snprintf_s(text,sizeof(text),_TRUNCATE,"there isn't room for it: it needs up to %.1f GB, and %.1f GB are free",double(need)/1e9,double(room)/1e9);
        return Fail(error,text);
    }

    std::string part=output+".part";
    RawFile out;
    if(!out.Open(part,true)) return Fail(error,"can't write "+part+" (is the folder read-only?)");
    struct Cleanup { RawFile& f; const std::string& path; bool keep; ~Cleanup() { f.Close(); if(!keep) RemovePath(path); } } cleanup={ out, part, false };

    // Workers deflate the blocks of each batch; a block that doesn't shrink
    // is kept as it is.
    unsigned workers=std::max(1u,std::min(8u,std::thread::hardware_concurrency()));
    const size_t batch=size_t(BatchBlocks)*workers;
    std::vector<unsigned char> raw(batch*CsoBlock), packed(batch*CsoBlock);
    std::vector<size_t> packedBytes(batch);
    std::vector<DWORD> index(layout.blocks+1);
    const mz_uint flags=tdefl_create_comp_flags_from_zip_params(Level,-15,MZ_DEFAULT_STRATEGY);
    std::vector<unsigned char> zero(CsoBlock,0), zeroPacked(CsoBlock);
    size_t zeroBytes=tdefl_compress_mem_to_mem(&zeroPacked[0],zeroPacked.size(),&zero[0],zero.size(),flags);
    if(!zeroBytes) return Fail(error,"deflate doesn't work");

    U64 at=CsoHeader+U64(index.size())*4;
    const U64 unit=U64(1)<<layout.shift;
    std::vector<unsigned char> head(size_t(at),0);
    if(!out.Write(0,&head[0],head.size())) return Fail(error,"can't write "+part);
    // CRC-32 of the blocks that hold something; the empty ones are checked
    // by what they were written as.
    DWORD crc=MZ_CRC32_INIT;
    std::vector<bool> empty(layout.blocks,false);
    progress.step=StepCompress;
    const size_t perBlock=CsoBlock/Sector;
    for(size_t first=0;first<layout.blocks;first+=batch) {
        if(cancel && *cancel) return Fail(error,"stopped");
        size_t count=std::min(batch,layout.blocks-first);
        std::vector<char> kind(count,0);  // 0: no file uses it, 1: deflate it.
        for(size_t i=0;i<count;++i) {
            size_t b=first+i;
            U64 from=U64(b)*CsoBlock;
            DWORD bytes=DWORD(std::min<U64>(CsoBlock,total-from));
            unsigned char* data=&raw[i*CsoBlock];
            bool any=false;
            for(size_t s=0;s<perBlock && !any;++s) any=used.Used(b*perBlock+s);
            if(!any) { memset(data,0,CsoBlock); continue; }
            if(!image->Read(from,data,bytes)) return Fail(error,streamError("the disc can't be read"));
            if(bytes<CsoBlock) memset(data+bytes,0,CsoBlock-bytes);
            for(size_t s=0;s<perBlock;++s) if(!used.Used(b*perBlock+s)) memset(data+s*Sector,0,Sector);
            kind[i]=1;
        }
        std::vector<std::thread> threads;
        for(unsigned w=0;w<workers;++w) threads.push_back(std::thread([&,w] {
            for(size_t i=w;i<count;i+=workers) {
                if(!kind[i]) continue;
                size_t b=first+i;
                DWORD bytes=DWORD(std::min<U64>(CsoBlock,total-U64(b)*CsoBlock));
                packedBytes[i]=tdefl_compress_mem_to_mem(&packed[i*CsoBlock],bytes-1,&raw[i*CsoBlock],bytes,flags);
            }
        }));
        for(size_t t=0;t<threads.size();++t) threads[t].join();
        for(size_t i=0;i<count;++i) {
            size_t b=first+i;
            DWORD bytes=DWORD(std::min<U64>(CsoBlock,total-U64(b)*CsoBlock));
            at=(at+unit-1)/unit*unit;
            if((at>>layout.shift)>=Plain) return Fail(error,"the disc is too big for a CSO file");
            const unsigned char* data; size_t size; bool plain=false;
            empty[b]=!kind[i] && bytes==CsoBlock;
            if(!empty[b]) crc=DWORD(mz_crc32(crc,&raw[i*CsoBlock],bytes));
            if(empty[b]) { data=&zeroPacked[0]; size=zeroBytes; }
            else if(kind[i] && packedBytes[i]) { data=&packed[i*CsoBlock]; size=packedBytes[i]; }
            else if(!kind[i]) { size=tdefl_compress_mem_to_mem(&packed[i*CsoBlock],CsoBlock,&raw[i*CsoBlock],bytes,flags); data=&packed[i*CsoBlock]; if(!size) return Fail(error,"deflate doesn't work"); }
            else { data=&raw[i*CsoBlock]; size=bytes; plain=true; }
            index[b]=DWORD(at>>layout.shift)|(plain ? Plain : 0);
            if(!out.Write(at,data,size)) return Fail(error,"can't write "+part+" (is the drive full?)");
            at+=size;
        }
        progress.done=std::min<U64>(total,U64(first+count)*CsoBlock);
    }
    if(stream) {  // The archive's own check of the disc, once all of it is unpacked.
        if(cancel && *cancel) return Fail(error,"stopped");
        if(!stream->Finish()) return Fail(error,streamError("the archive can't be read to its end"));
    }
    at=(at+unit-1)/unit*unit;
    if((at>>layout.shift)>=Plain) return Fail(error,"the disc is too big for a CSO file");
    index[layout.blocks]=DWORD(at>>layout.shift);
    memcpy(&head[0],"CISO",4);
    PutLittle32(&head[4],CsoHeader);
    PutLittle32(&head[8],DWORD(total)); PutLittle32(&head[12],DWORD(total>>32));
    PutLittle32(&head[16],CsoBlock);
    head[20]=1; head[21]=BYTE(layout.shift);
    for(size_t i=0;i<index.size();++i) PutLittle32(&head[CsoHeader+i*4],index[i]);
    if(!out.Write(0,&head[0],head.size())) return Fail(error,"can't write "+part);
    if(at>0) { unsigned char none=0; U64 size=0; if(out.Size(size) && size<at && !out.Write(at-1,&none,1)) return Fail(error,"can't write "+part); }
    out.Close();
    result.outputBytes=at;

    // Read it back, as the server will.
    progress.step=StepCheck; progress.done=0;
    {
        std::string why;
        std::unique_ptr<DiscImage> check(OpenCsoImage(part,why));
        RawFile file;
        if(!check.get() || !file.Open(part,false)) return Fail(error,"the new file can't be read back"+(why.empty() ? std::string() : ": "+why));
        std::vector<unsigned char> buffer(0x100000), packedZero(zeroBytes);
        DWORD again=MZ_CRC32_INIT;
        for(size_t b=0;b<layout.blocks;) {
            if(cancel && *cancel) return Fail(error,"stopped");
            size_t end=b+1;
            if(empty[b]) {
                U64 from=U64(index[b]&~Plain)<<layout.shift, to=U64(index[b+1]&~Plain)<<layout.shift;
                if((index[b]&Plain) || to-from<zeroBytes || file.Read(from,&packedZero[0],zeroBytes)!=(long long)zeroBytes ||
                   memcmp(&packedZero[0],&zeroPacked[0],zeroBytes)) return Fail(error,"the new file doesn't read back the same (a disk problem?)");
            } else {
                while(end<layout.blocks && !empty[end] && (end-b+1)*CsoBlock<=buffer.size()) ++end;
                U64 from=U64(b)*CsoBlock;
                DWORD n=DWORD(std::min<U64>(U64(end-b)*CsoBlock,total-from));
                if(!check->Read(from,&buffer[0],n)) return Fail(error,"the new file can't be read back");
                again=DWORD(mz_crc32(again,&buffer[0],n));
            }
            b=end;
            progress.done=std::min<U64>(total,U64(b)*CsoBlock);
        }
        if(again!=crc) return Fail(error,"the new file doesn't read back the same (a disk problem?)");
    }
    if(!RenamePath(part,output)) return Fail(error,"can't rename "+part+" to "+Leaf(output)+(Exists(output) ? " (that file is there already)" : ""));
    cleanup.keep=true;
    return true;
}
}
