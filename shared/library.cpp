// SPDX-License-Identifier: MIT
#include "library.h"
#include "helper.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace nia {
static const DWORD ExecutionInfo = 0x40006;
static const char GameIni[] = "netiso.ini", GameXex[] = "default.xex", GameBoot[] = "boot.xex", GameInstall[] = "install.ini";
// (where the magic is, where the game partition starts), as netiso-srv checks.
static const U64 Partitions[][2] = { { 0xFDA0000, 0xFD90000 }, { 0x2090000, 0x2080000 }, { 0x10000, 0 } };
static const char XgdMagic[] = "MICROSOFT*XBOX*MEDIA";
enum { XgdMagicSize = 20, DirectoryMax = 1048576, XexDigestAt = 0x164, XexDigestEnd = 0x17C };

static void Say(Note note, const char* format, ...) {
    if(!note) return;
    char text[512];
    va_list a; va_start(a,format); _vsnprintf_s(text,sizeof(text),_TRUNCATE,format,a); va_end(a);
    note(text);
}
static DWORD Little32(const unsigned char* p) { return DWORD(p[0])|(DWORD(p[1])<<8)|(DWORD(p[2])<<16)|(DWORD(p[3])<<24); }
static void Join(char* out, const char* folder, const char* name) {
    _snprintf_s(out,PathMax,_TRUNCATE,"%s%c%s",folder,PathSeparator,name);
}

bool IniValue(const char* text, const char* key, char* value, size_t capacity) {
    size_t keyLength=strlen(key);
    for(const char* line=text;line && *line;) {
        const char* end=line+strcspn(line,"\r\n");
        if(size_t(end-line)>keyLength && line[keyLength]=='=' && !_strnicmp(line,key,keyLength)) {
            size_t length=size_t(end-line)-keyLength-1;
            if(length>=capacity) return false;
            memcpy(value,line+keyLength+1,length); value[length]=0;
            return true;
        }
        line=end+strspn(end,"\r\n");
    }
    return false;
}

DWORD ExecutionInfoOffset(const unsigned char* xex, DWORD size) {
    if(size<24 || memcmp(xex,"XEX2",4)) return 0;
    DWORD headerSize=Read32(xex+8),count=Read32(xex+20);
    if(headerSize>size) headerSize=size;
    for(DWORD i=0;i<count && 24+i*8+8<=headerSize;++i) {
        if(Read32(xex+24+i*8)!=ExecutionInfo) continue;
        DWORD at=Read32(xex+28+i*8);
        return at>=24 && at<=headerSize && headerSize-at>=ExecutionInfoSize ? at : 0;
    }
    return 0;
}

// SHA-1, for the XEX header digest (the Xbox build could use XeCryptSha, but
// the host tests need it too).
struct Sha1 {
    DWORD h[5]; unsigned char block[64]; DWORD used; U64 total;
    Sha1() : used(0), total(0) { h[0]=0x67452301; h[1]=0xEFCDAB89; h[2]=0x98BADCFE; h[3]=0x10325476; h[4]=0xC3D2E1F0; }
    static DWORD Rotate(DWORD v, int n) { return (v<<n)|(v>>(32-n)); }
    void Compress() {
        DWORD w[80];
        for(int i=0;i<16;++i) w[i]=Read32(block+i*4);
        for(int i=16;i<80;++i) w[i]=Rotate(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
        DWORD a=h[0],b=h[1],c=h[2],d=h[3],e=h[4];
        for(int i=0;i<80;++i) {
            DWORD f,k;
            if(i<20) { f=(b&c)|(~b&d); k=0x5A827999; }
            else if(i<40) { f=b^c^d; k=0x6ED9EBA1; }
            else if(i<60) { f=(b&c)|(b&d)|(c&d); k=0x8F1BBCDC; }
            else { f=b^c^d; k=0xCA62C1D6; }
            DWORD t=Rotate(a,5)+f+e+k+w[i];
            e=d; d=c; c=Rotate(b,30); b=a; a=t;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e;
    }
    void Add(const unsigned char* data, DWORD size) {
        total+=size;
        while(size--) { block[used++]=*data++; if(used==64) { Compress(); used=0; } }
    }
    void Finish(unsigned char* digest) {
        U64 bits=total*8;
        unsigned char one=0x80,zero=0;
        Add(&one,1);
        while(used!=56) Add(&zero,1);
        for(int i=7;i>=0;--i) { block[used++]=(unsigned char)(bits>>(i*8)); }
        Compress();
        for(int i=0;i<5;++i) Write32(digest+i*4,h[i]);
    }
};

bool FixHeaderDigest(unsigned char* xex, DWORD size) {
    if(size<24 || memcmp(xex,"XEX2",4)) return false;
    DWORD headerSize=Read32(xex+8),security=Read32(xex+16);
    if(headerSize>size || security<24 || security+XexDigestEnd>headerSize) return false;
    // The header digest (security info + 0x164) covers the header from the
    // end of the security info's fixed part to the end of the header, then
    // everything before the security info's RSA signature. It is what the
    // signature signs, and the loader checks it even when the signature check
    // is patched out, so a copy with edited execution info needs a new one.
    Sha1 sha;
    sha.Add(xex+security+XexDigestEnd,headerSize-security-XexDigestEnd);
    sha.Add(xex,security+8);
    sha.Finish(xex+security+XexDigestAt);
    return true;
}

// Release tags in No-Intro and Redump names say which release an ISO is, not
// which game: "(USA, Europe)", "(En,Ja,Fr,De)", "(Rev 1)", "(v1.02)", "[!]".
// Folders leave them out, so Aurora shows a clean name until its lookup
// names the game. Other tags stay: "(Disc 1)", "(Bonus Disc)", "(Demo)".
static const char* const Regions[] = { "USA", "Europe", "Japan", "World", "Asia", "Australia", "Austria", "Belgium", "Brazil",
    "Canada", "China", "Czech", "Denmark", "Finland", "France", "Germany", "Greece", "Hong Kong", "India", "Ireland", "Israel",
    "Italy", "Korea", "Latin America", "Mexico", "Netherlands", "New Zealand", "Norway", "Poland", "Portugal", "Russia",
    "Scandinavia", "South Africa", "Spain", "Sweden", "Switzerland", "Taiwan", "Turkey", "UK", "Unknown" };

static bool ReleaseItem(const char* item, size_t length) {
    while(length && *item==' ') { ++item; --length; }
    while(length && item[length-1]==' ') --length;
    if(!length) return false;
    for(size_t i=0;i<sizeof(Regions)/sizeof(Regions[0]);++i)
        if(strlen(Regions[i])==length && !_strnicmp(Regions[i],item,length)) return true;
    // A language: En, Ja, Zh-Hant, Pt-BR.
    if(length>=2 && item[0]>='A' && item[0]<='Z' && item[1]>='a' && item[1]<='z') {
        if(length==2) return true;
        bool letters=length>3 && item[2]=='-';
        for(size_t i=3;i<length && letters;++i) letters=isalpha(static_cast<unsigned char>(item[i]))!=0;
        if(letters) return true;
    }
    // A revision or version: Rev 1, Rev A, v1.02.
    if(length>4 && !_strnicmp(item,"Rev ",4)) return true;
    if(length>=2 && (item[0]=='v' || item[0]=='V') && isdigit(static_cast<unsigned char>(item[1]))) {
        bool digits=true;
        for(size_t i=2;i<length && digits;++i) digits=isdigit(static_cast<unsigned char>(item[i])) || item[i]=='.';
        if(digits) return true;
    }
    return length==1 && *item=='!';
}

static bool ReleaseTag(const char* tag, size_t length) {
    for(size_t start=0;;) {
        size_t end=start;
        while(end<length && tag[end]!=',') ++end;
        if(!ReleaseItem(tag+start,end-start)) return false;
        if(end==length) return true;
        start=end+1;
    }
}

// stem without the release tags among the tags at its end, or all of stem if
// nothing but tags would be left.
static void FriendlyStem(const char* stem, size_t length, char* out, size_t capacity) {
    enum { TagsMax = 16 };
    size_t keptAt[TagsMax],keptEnd[TagsMax]; int keptCount=0;
    size_t end=length;
    for(;;) {
        while(end && stem[end-1]==' ') --end;
        if(!end) break;
        char open=stem[end-1]==')' ? '(' : stem[end-1]==']' ? '[' : 0;
        if(!open) break;
        size_t at=end-1; bool found=false;
        while(at && !found) found=stem[--at]==open;
        if(!found || !at) break;
        if(!ReleaseTag(stem+at+1,end-at-2)) {
            if(keptCount==TagsMax) break;
            keptAt[keptCount]=at; keptEnd[keptCount]=end; ++keptCount;
        }
        end=at;
    }
    while(end && stem[end-1]==' ') --end;
    if(!end) { _snprintf_s(out,capacity,_TRUNCATE,"%.*s",int(length),stem); return; }
    _snprintf_s(out,capacity,_TRUNCATE,"%.*s",int(end),stem);
    for(int k=keptCount-1;k>=0;--k) {
        size_t used=strlen(out);
        _snprintf_s(out+used,capacity-used,_TRUNCATE," %.*s",int(keptEnd[k]-keptAt[k]),stem+keptAt[k]);
    }
}

// FATX allows letters, digits, space and !#$%&'()-.@[]^_`{}~ in names, up to 42.
// Commas are common in No-Intro names ("(USA, Europe)") and read fine dropped.
static void SafeName(const char* text, size_t length, char* out) {
    char name[IsvrNameBytes]; size_t used=0;
    for(size_t i=0;i<length && used<sizeof(name)-1;++i) {
        char c=text[i];
        if(c==',') continue;
        bool ok=(c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || (c && strchr(" !#$%&'()-.@[]^_`{}~",c));
        name[used++]=ok ? c : '_';
    }
    name[used]=0;
    size_t start=0;
    while(name[start]==' ' || name[start]=='.') ++start;
    size_t keep=used-start;
    if(keep>FolderNameMax) keep=FolderNameMax;
    while(keep && (name[start+keep-1]==' ' || name[start+keep-1]=='.')) --keep;
    if(!keep) strcpy_s(out,FolderNameMax+1,"Game");
    else _snprintf_s(out,FolderNameMax+1,_TRUNCATE,"%.*s",int(keep),name+start);
}

static bool Taken(char used[][FolderNameMax + 1], int usedCount, const char* name) {
    for(int i=0;i<usedCount;++i) if(!_stricmp(used[i],name)) return true;
    return false;
}

void FolderName(const char* isoName, char used[][FolderNameMax + 1], int usedCount, char* out) {
    const char* dot=strrchr(isoName,'.');
    size_t stem=dot ? size_t(dot-isoName) : strlen(isoName);
    char friendly[IsvrNameBytes],name[FolderNameMax+1];
    FriendlyStem(isoName,stem,friendly,sizeof(friendly));
    SafeName(friendly,strlen(friendly),name);
    strcpy_s(out,FolderNameMax+1,name);
    if(!Taken(used,usedCount,out)) return;
    // Another release of the same game keeps its tags, and after that a number.
    SafeName(isoName,stem,out);
    for(int n=2;Taken(used,usedCount,out);++n) {
        char suffix[16]; _snprintf_s(suffix,sizeof(suffix),_TRUNCATE," (%d)",n);
        size_t keep=strlen(name); size_t room=FolderNameMax-strlen(suffix);
        if(keep>room) keep=room;
        while(keep && (name[keep-1]==' ' || name[keep-1]=='.')) --keep;
        _snprintf_s(out,FolderNameMax+1,_TRUNCATE,"%.*s%s",int(keep),name,suffix);
    }
}

static bool Fail(char* error, size_t capacity, const char* text) { _snprintf_s(error,capacity,_TRUNCATE,"%s",text); return false; }
static bool ReadIso(IsvrClient& client, U64 offset, DWORD length, unsigned char* data) {
    while(length) {
        DWORD part=length>DWORD(IsvrMaxRead) ? DWORD(IsvrMaxRead) : length;
        if(!client.Read(offset,part,data)) return false;
        offset+=part; data+=part; length-=part;
    }
    return true;
}

// An original Xbox game's default.xbe: the Title ID from its certificate
// (little-endian, at the certificate's address less the base address), made
// into execution info as a XEX has it, with no Media ID and as disc 1 of 1.
static bool XbeInfo(IsvrClient& client, U64 xbeAt, DWORD xbeSize, unsigned char* info, char* error, size_t capacity) {
    unsigned char block[0x120];
    if(xbeSize<sizeof(block) || !client.Read(xbeAt,sizeof(block),block)) return Fail(error,capacity,xbeSize<sizeof(block) ? "default.xbe is too small" : client.Error());
    if(memcmp(block,"XBEH",4)) return Fail(error,capacity,"default.xbe is not an XBE file");
    DWORD base=Little32(block+0x104),certificate=Little32(block+0x118);
    if(certificate<base || certificate-base>xbeSize-12) return Fail(error,capacity,"default.xbe has a bad certificate address");
    unsigned char cert[12];
    if(!client.Read(xbeAt+(certificate-base),12,cert)) return Fail(error,capacity,client.Error());
    DWORD titleId=Little32(cert+8);
    if(!titleId) return Fail(error,capacity,"default.xbe has no Title ID");
    memset(info,0,ExecutionInfoSize);
    Write32(info+12,titleId);
    info[18]=1; info[19]=1;
    return true;
}

bool GameInfo(IsvrClient& client, unsigned char* info, char* error, size_t capacity, bool* xbe) {
    if(xbe) *xbe=false;
    unsigned char block[64];
    U64 start=0; bool found=false;
    for(int i=0;i<3 && !found;++i) {
        if(client.Size()<Partitions[i][0]+XgdMagicSize) continue;
        if(!client.Read(Partitions[i][0],XgdMagicSize,block)) return Fail(error,capacity,client.Error());
        if(!memcmp(block,XgdMagic,XgdMagicSize)) { start=Partitions[i][1]; found=true; }
    }
    if(!found) return Fail(error,capacity,"not an Xbox 360 game ISO");
    if(!client.Read(start+32*IsvrSector,28,block)) return Fail(error,capacity,client.Error());
    DWORD rootSector=Little32(block+20),rootSize=Little32(block+24);
    if(!rootSize || rootSize>DirectoryMax) return Fail(error,capacity,"bad root directory");
    unsigned char* root=static_cast<unsigned char*>(malloc(rootSize));
    if(!root) return Fail(error,capacity,"out of memory");
    if(!ReadIso(client,start+U64(rootSector)*IsvrSector,rootSize,root)) { free(root); return Fail(error,capacity,client.Error()); }
    U64 xexAt=0,xbeAt=0; DWORD xexSize=0,xbeSize=0;
    for(DWORD pos=0;pos+14<=rootSize;) {
        if(root[pos]==0xFF && root[pos+1]==0xFF) { pos=(pos/IsvrSector+1)*IsvrSector; continue; } // Padding to the next sector.
        DWORD length=root[pos+13];
        if(!length || pos+14+length>rootSize) break;
        if(length==11 && !_strnicmp(reinterpret_cast<const char*>(root+pos+14),"default.xex",11)) {
            xexAt=start+U64(Little32(root+pos+4))*IsvrSector; xexSize=Little32(root+pos+8); break;
        }
        if(length==11 && !_strnicmp(reinterpret_cast<const char*>(root+pos+14),"default.xbe",11)) {
            xbeAt=start+U64(Little32(root+pos+4))*IsvrSector; xbeSize=Little32(root+pos+8);
        }
        pos=(pos+14+length+3)&~3u;
    }
    free(root);
    if(!xexSize && xbeSize) {
        if(!XbeInfo(client,xbeAt,xbeSize,info,error,capacity)) return false;
        if(xbe) *xbe=true;
        return true;
    }
    if(!xexSize) return Fail(error,capacity,"no default.xex in the root of the disc");
    if(xexSize<24 || !client.Read(xexAt,24,block)) return Fail(error,capacity,xexSize<24 ? "default.xex is too small" : client.Error());
    if(memcmp(block,"XEX2",4)) return Fail(error,capacity,"default.xex is not a XEX2 file");
    DWORD headerSize=Read32(block+8),count=Read32(block+20);
    if(headerSize>xexSize || count>256 || 24+count*8>headerSize) return Fail(error,capacity,"default.xex has a bad header");
    unsigned char table[256*8];
    if(count && !client.Read(xexAt+24,count*8,table)) return Fail(error,capacity,client.Error());
    for(DWORD i=0;i<count;++i) {
        if(Read32(table+i*8)!=ExecutionInfo) continue;
        DWORD at=Read32(table+i*8+4);
        if(at<24 || at+ExecutionInfoSize>headerSize) break;
        if(!client.Read(xexAt+at,ExecutionInfoSize,info)) return Fail(error,capacity,client.Error());
        return true;
    }
    return Fail(error,capacity,"default.xex has no execution info");
}

struct Folder { char name[FolderNameMax+1]; char iso[IsvrNameBytes]; DWORD titleId, mediaId; bool game, keep;
                char role[16], label[64]; };  // disc_role and disc_label in its netiso.ini.

// Rewrites a game folder's default.xex when it was made from an older
// launcher, keeping its game's execution info, or putting in info if given.
// 1 if rewritten, 0 if it was current, -1 if it couldn't be read or written,
// -2 if its execution info is for another title than titleId (0: any title),
// so the game's own has to be read from the ISO. That happened when the
// template's default.xex was copied over a game's.
static int RefreshLauncher(Storage& storage, const char* folder, const unsigned char* launcher, DWORD launcherSize, DWORD infoAt,
                           DWORD titleId, const unsigned char* info) {
    char path[PathMax]; Join(path,folder,GameXex);
    // Room for the full launcher a folder had as default.xex before the tile launcher.
    DWORD capacity=launcherSize+0x800000,size=0;
    unsigned char* old=static_cast<unsigned char*>(malloc(capacity));
    unsigned char* copy=static_cast<unsigned char*>(malloc(launcherSize));
    int result=-1;
    if(old && copy) {
        bool read=storage.ReadFile(path,old,capacity,size);
        DWORD oldInfo=read ? ExecutionInfoOffset(old,size) : 0;
        if(!info && oldInfo && titleId && Read32(old+oldInfo+12)!=titleId) result=-2;
        else if(info || oldInfo) {
            memcpy(copy,launcher,launcherSize); memcpy(copy+infoAt,info ? info : old+oldInfo,ExecutionInfoSize);
            FixHeaderDigest(copy,launcherSize);
            if(read && size==launcherSize && !memcmp(old,copy,size)) result=0;
            else if(storage.WriteFile(path,copy,launcherSize)) result=1;
        }
    }
    free(old); free(copy);
    return result;
}

// Writes data as folder\name unless the file already holds exactly that.
// 1 if written, 0 if it was current, -1 on failure.
static int RefreshFile(Storage& storage, const char* folder, const char* name, const unsigned char* data, DWORD size) {
    char path[PathMax]; Join(path,folder,name);
    unsigned char* old=static_cast<unsigned char*>(malloc(size+1));
    DWORD oldSize=0;
    bool same=old && storage.ReadFile(path,old,size+1,oldSize) && oldSize==size && !memcmp(old,data,size);
    free(old);
    if(same) return 0;
    return storage.WriteFile(path,data,size) ? 1 : -1;
}

// Puts a disc's role as findRole gives it (see library.h) into its game
// folder's netiso.ini, keeping the other lines; a disc without one has the
// lines taken out. The file is read and written only when they change.
static void RefreshRole(Storage& storage, const char* root, const Folder& game, FindRole findRole, Note note, SyncResult& result) {
    static DiscRoleInfo info;
    memset(&info,0,sizeof(info));
    findRole(game.iso,info);
    if(!info.known) return;
    if(!info.role[0]) info.label[0]=0;
    if(!strcmp(info.role,game.role) && !strcmp(info.label,game.label)) return;
    char folder[PathMax],path[PathMax]; Join(folder,root,game.name); Join(path,folder,GameIni);
    static char old[IniMax+1],text[IniMax+1];
    DWORD size=0;
    if(!storage.ReadFile(path,reinterpret_cast<unsigned char*>(old),IniMax,size)) {
        Say(note,"Couldn't read %s to put the disc's role in it (error %lu).",path,storage.LastError()); ++result.failed;
        return;
    }
    old[size]=0;
    size_t at=0;
    for(const char* line=old;*line;) {
        const char* end=line+strcspn(line,"\r\n");
        size_t length=size_t(end-line);
        if(length && _strnicmp(line,"disc_role=",10) && _strnicmp(line,"disc_label=",11) && at+length+2<sizeof(text)) {
            memcpy(text+at,line,length); at+=length;
            text[at++]='\r'; text[at++]='\n';
        }
        line=end+strspn(end,"\r\n");
    }
    if(info.role[0]) {
        int n=_snprintf_s(text+at,sizeof(text)-at,_TRUNCATE,info.label[0] ? "disc_role=%s\r\ndisc_label=%s\r\n" : "disc_role=%s\r\n",info.role,info.label);
        if(n>0) at+=size_t(n);
    }
    int written=RefreshFile(storage,folder,GameIni,reinterpret_cast<const unsigned char*>(text),DWORD(at));
    if(written<0) { Say(note,"Couldn't write %s (error %lu).",path,storage.LastError()); ++result.failed; }
    else if(written>0 && info.role[0]) Say(note,"%s is the %s disc%s%s%s; its netiso.ini says so.",game.name,info.role,
                                            info.label[0] ? " (" : "",info.label,info.label[0] ? ")" : "");
    else if(written>0) Say(note,"%s is no longer part of a set; took the disc role out of its netiso.ini.",game.name);
}

// Writes, or empties, a game folder's install.ini as the caller's findInstall
// says (see library.h).
static void RefreshInstall(Storage& storage, const char* root, const Folder* folders, int folderCount, const Folder& game,
                           FindInstall findInstall, Note note, SyncResult& result) {
    static InstallNeed need;
    memset(&need,0,sizeof(need));
    findInstall(game.iso,need);
    if(!need.known) return;
    char folder[PathMax],path[PathMax]; Join(folder,root,game.name); Join(path,folder,GameInstall);
    static char text[IniMax+1];
    text[0]=0;
    if(need.iso[0]) {
        // Its folder gives the install disc's Media ID, so the launcher can
        // tell it from the play disc it shares the Title ID with.
        const Folder* disc=NULL;
        for(int k=0;k<folderCount && !disc;++k) if(folders[k].game && folders[k].keep && !_stricmp(folders[k].iso,need.iso)) disc=&folders[k];
        _snprintf_s(text,sizeof(text),_TRUNCATE,
            "; Written by NetDisc360: this game's install disc, and the packages it puts under Content.\r\n"
            "install_iso=%s\r\ninstall_title_id=%08X\r\ninstall_media_id=%08X\r\ninstall_folder=%s\r\n%s",
            need.iso,game.titleId,disc ? disc->mediaId : 0,disc ? disc->name : "",need.packages);
    } else {
        // It needs none (any more): one that is there is emptied, none is made.
        DWORD size=0;
        if(!storage.ReadFile(path,reinterpret_cast<unsigned char*>(text),IniMax,size) || !size) return;
        text[0]=0;
    }
    int written=RefreshFile(storage,folder,GameInstall,reinterpret_cast<const unsigned char*>(text),DWORD(strlen(text)));
    if(written<0) { Say(note,"Couldn't write %s (error %lu).",path,storage.LastError()); ++result.failed; }
    else if(written && need.iso[0]) Say(note,"%s needs %s installed first; its install.ini says so.",game.name,need.iso);
    else if(written) Say(note,"%s needs no install disc any more; emptied its install.ini.",game.name);
}

bool Sync(IsvrClient& client, Storage& storage, const char* root, const unsigned char* launcher, DWORD launcherSize,
          const unsigned char* boot, DWORD bootSize, Note note, SyncResult& result, bool removeGone, FindInstall findInstall,
          FindRole findRole) {
    memset(&result,0,sizeof(result));
    DWORD infoAt=ExecutionInfoOffset(launcher,launcherSize);
    if(!infoAt) { Say(note,"The launcher XEX has no execution info header."); return false; }

    // What the server has. Names come back in its order; an empty one ends the list.
    static char isos[IsvrMaxIsos][IsvrNameBytes];
    int isoCount=0;
    for(;isoCount<IsvrMaxIsos;++isoCount) {
        if(!client.Name(isoCount,isos[isoCount])) { Say(note,"Listing the server's ISOs failed: %s",client.Error()); return false; }
        if(!isos[isoCount][0]) break;
    }
    Say(note,"The server has %d ISO(s).",isoCount);

    // What is already here. A folder is a game folder when its netiso.ini names an ISO.
    storage.MakeFolder(root);
    // The helper the game folders share. A game folder's own boot.xex from
    // before goes only once it's there: its tile launcher starts that one
    // without it.
    bool helperReady=false;
    if(boot) {
        char helperFolder[PathMax],helperPath[PathMax]; Join(helperFolder,root,HelperFolder); Join(helperPath,helperFolder,HelperXex);
        storage.MakeFolder(helperFolder);
        int written=RefreshFile(storage,helperFolder,HelperXex,boot,bootSize);
        if(written<0) { Say(note,"Couldn't write %s (error %lu); the game folders keep their own helper.",helperPath,storage.LastError()); ++result.failed; }
        else if(written>0) Say(note,"Put the helper the game folders share in %s.",helperPath);
        helperReady=written>=0;
    }
    static char names[MaxFolders][FolderNameMax+1];
    static Folder folders[MaxFolders];
    int folderCount=storage.ListFolders(root,names,MaxFolders);
    // Not knowing what is there, every game would look new and could be
    // written over a folder of the same name.
    if(folderCount<0) { Say(note,"Couldn't list %s (error %lu).",root,storage.LastError()); return false; }
    static char ini[IniMax+1];
    for(int i=0;i<folderCount;++i) {
        Folder& f=folders[i]; memset(&f,0,sizeof(f)); strcpy_s(f.name,names[i]);
        char path[PathMax],folder[PathMax]; Join(folder,root,f.name); Join(path,folder,GameIni);
        DWORD size=0;
        if(storage.ReadFile(path,reinterpret_cast<unsigned char*>(ini),IniMax,size)) {
            ini[size]=0;
            f.game=IniValue(ini,"iso",f.iso,sizeof(f.iso)) && f.iso[0];
            char id[16];
            if(IniValue(ini,"title_id",id,sizeof(id))) f.titleId=DWORD(strtoul(id,NULL,16));
            if(IniValue(ini,"media_id",id,sizeof(id))) f.mediaId=DWORD(strtoul(id,NULL,16));
            if(!IniValue(ini,"disc_role",f.role,sizeof(f.role))) f.role[0]=0;
            if(!IniValue(ini,"disc_label",f.label,sizeof(f.label))) f.label[0]=0;
        } else if(!storage.Missing()) {
            // Its game would get a second folder; better to try again later.
            Say(note,"Couldn't read %s (error %lu).",path,storage.LastError());
            return false;
        }
    }

    for(int i=0;i<isoCount;++i) {
        bool have=false;
        for(int k=0;k<folderCount;++k) if(folders[k].game && !_stricmp(folders[k].iso,isos[i])) { folders[k].keep=true; have=true; }
        if(have) {
            ++result.kept;
            // Folders made by an older Sync get this version's launcher.
            for(int k=0;k<folderCount;++k) {
                if(!folders[k].keep || _stricmp(folders[k].iso,isos[i])) continue;
                char folder[PathMax]; Join(folder,root,folders[k].name);
                int refreshed=RefreshLauncher(storage,folder,launcher,launcherSize,infoAt,folders[k].titleId,NULL);
                if(refreshed==-2) {
                    unsigned char info[ExecutionInfoSize]; char error[160];
                    if(!client.Mount(isos[i])) _snprintf_s(error,sizeof(error),_TRUNCATE,"%s",client.Error());
                    else if(GameInfo(client,info,error,sizeof(error))) {
                        refreshed=RefreshLauncher(storage,folder,launcher,launcherSize,infoAt,0,info);
                        if(refreshed>0) Say(note,"The launcher in %s had another title's IDs; rebuilt it from %s.",folders[k].name,isos[i]);
                    }
                    if(refreshed==-2) {
                        Say(note,"The launcher in %s has another title's IDs, and %s couldn't be read to repair it: %s",folders[k].name,isos[i],error);
                        if(!client.Connected()) return false;
                        continue;
                    }
                }
                if(helperReady && refreshed>=0) {  // The shared helper does its job now.
                    char helper[PathMax]; Join(helper,folder,GameBoot);
                    int removed=storage.RemoveFile(helper);
                    refreshed=removed<0 ? -1 : refreshed|removed;
                }
                if(refreshed>0) { Say(note,"Updated the launcher in %s.",folders[k].name); ++result.updated; }
                else if(refreshed<0) { Say(note,"Couldn't update the launcher in %s (error %lu).",folders[k].name,storage.LastError()); ++result.failed; }
            }
            continue;
        }
        if(!client.Mount(isos[i])) {
            Say(note,"Skipped %s: %s",isos[i],client.Error()); ++result.skipped;
            if(!client.Connected()) return false;
            continue;
        }
        unsigned char info[ExecutionInfoSize]; char error[160]; bool xbe=false;
        if(!GameInfo(client,info,error,sizeof(error),&xbe)) {
            Say(note,"Skipped %s: %s",isos[i],error); ++result.skipped;
            if(!client.Connected()) return false;
            continue;
        }
        if(folderCount>=MaxFolders) { Say(note,"Too many folders in %s.",root); return false; }
        Folder& f=folders[folderCount]; memset(&f,0,sizeof(f));
        FolderName(isos[i],names,folderCount,f.name);
        strcpy_s(names[folderCount],f.name); strcpy_s(f.iso,isos[i]); f.game=f.keep=true; ++folderCount;
        f.titleId=Read32(info+12); f.mediaId=Read32(info);
        char folder[PathMax],path[PathMax]; Join(folder,root,f.name);
        unsigned char* copy=static_cast<unsigned char*>(malloc(launcherSize));
        if(!copy) { Say(note,"Out of memory."); return false; }
        memcpy(copy,launcher,launcherSize); memcpy(copy+infoAt,info,ExecutionInfoSize);
        FixHeaderDigest(copy,launcherSize);
        char text[IsvrNameBytes+260];
        int length=_snprintf_s(text,sizeof(text),_TRUNCATE,"; Written by NetISO Sync. iso is the file name the NetISO server lists.\r\niso=%s\r\ntitle_id=%08X\r\nmedia_id=%08X\r\n",
            isos[i],Read32(info+12),Read32(info));
        // An original Xbox game: the launcher starts default.xbe, which the
        // console runs in its original Xbox emulator.
        if(xbe && length>0) { int more=_snprintf_s(text+length,sizeof(text)-length,_TRUNCATE,"xbe=1\r\n"); length=more>0 ? length+more : -1; }
        if(findRole && length>0) {  // A disc of a set says which (see RefreshRole).
            static DiscRoleInfo role;
            memset(&role,0,sizeof(role));
            findRole(isos[i],role);
            if(role.known && role.role[0]) {
                _snprintf_s(text+length,sizeof(text)-length,_TRUNCATE,role.label[0] ? "disc_role=%s\r\ndisc_label=%s\r\n" : "disc_role=%s\r\n",role.role,role.label);
                strcpy_s(f.role,role.role); strcpy_s(f.label,role.label);
            }
        }
        Join(path,folder,GameXex);
        bool written=storage.MakeFolder(folder) && storage.WriteFile(path,copy,launcherSize);
        free(copy);
        Join(path,folder,GameIni);
        written=written && storage.WriteFile(path,text,DWORD(strlen(text)));
        if(!written) {
            // One bad name shouldn't stop the rest; the next sync tries it again.
            Say(note,"Skipped %s: couldn't write %s (error %lu).",isos[i],folder,storage.LastError());
            storage.RemoveGame(folder);
            folders[--folderCount].game=false;
            ++result.skipped;
            continue;
        }
        Say(note,"Added %s as %s (Title ID %08X, Media ID %08X).",isos[i],f.name,Read32(info+12),Read32(info));
        ++result.added;
    }

    if(findInstall) for(int k=0;k<folderCount;++k)
        if(folders[k].game && folders[k].keep) RefreshInstall(storage,root,folders,folderCount,folders[k],findInstall,note,result);
    if(findRole) for(int k=0;k<folderCount;++k)
        if(folders[k].game && folders[k].keep) RefreshRole(storage,root,folders[k],findRole,note,result);

    // Folders whose ISO is gone. An empty listing could be a server problem,
    // so nothing is removed then.
    for(int k=0;k<folderCount && isoCount;++k) {
        if(!folders[k].game || folders[k].keep) continue;
        if(!removeGone) { ++result.held; continue; }
        char folder[PathMax]; Join(folder,root,folders[k].name);
        if(storage.RemoveGame(folder)) { Say(note,"Removed %s (%s is no longer on the server).",folders[k].name,folders[k].iso); ++result.removed; }
        else { Say(note,"Couldn't remove %s.",folder); ++result.failed; }
    }
    return true;
}
}
