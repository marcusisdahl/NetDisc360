// SPDX-License-Identifier: MIT
#include "library.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace nia {
static const DWORD ExecutionInfo = 0x40006;
static const char GameIni[] = "netiso.ini", GameXex[] = "default.xex";
// (where the magic is, where the game partition starts), as netiso-srv checks.
static const U64 Partitions[][2] = { { 0xFDA0000, 0xFD90000 }, { 0x2090000, 0x2080000 }, { 0x10000, 0 } };
static const char XgdMagic[] = "MICROSOFT*XBOX*MEDIA";
enum { XgdMagicSize = 20, DirectoryMax = 1048576 };

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
        return at>=24 && at+ExecutionInfoSize<=headerSize ? at : 0;
    }
    return 0;
}

void FolderName(const char* isoName, char used[][FolderNameMax + 1], int usedCount, char* out) {
    char name[FolderNameMax+1]; size_t length=0;
    const char* dot=strrchr(isoName,'.');
    size_t stem=dot ? size_t(dot-isoName) : strlen(isoName);
    size_t start=0;
    while(start<stem && (isoName[start]==' ' || isoName[start]=='.')) ++start;
    // FATX allows letters, digits, space and !#$%&'()-.@[]^_`{}~ in names.
    // Commas are common in No-Intro names ("(USA, Europe)") and read fine dropped.
    for(size_t i=start;i<stem && length<FolderNameMax;++i) {
        char c=isoName[i];
        if(c==',') continue;
        bool ok=(c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || (c && strchr(" !#$%&'()-.@[]^_`{}~",c));
        name[length++]=ok ? c : '_';
    }
    while(length && (name[length-1]==' ' || name[length-1]=='.')) --length;
    name[length]=0;
    if(!length) strcpy_s(name,"Game");
    strcpy_s(out,FolderNameMax+1,name);
    for(int n=2;;++n) {
        bool taken=false;
        for(int i=0;i<usedCount && !taken;++i) taken=!_stricmp(used[i],out);
        if(!taken) return;
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

bool GameInfo(IsvrClient& client, unsigned char* info, char* error, size_t capacity) {
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
    U64 xexAt=0; DWORD xexSize=0;
    for(DWORD pos=0;pos+14<=rootSize;) {
        if(root[pos]==0xFF && root[pos+1]==0xFF) { pos=(pos/IsvrSector+1)*IsvrSector; continue; } // Padding to the next sector.
        DWORD length=root[pos+13];
        if(!length || pos+14+length>rootSize) break;
        if(length==11 && !_strnicmp(reinterpret_cast<const char*>(root+pos+14),"default.xex",11)) {
            xexAt=start+U64(Little32(root+pos+4))*IsvrSector; xexSize=Little32(root+pos+8); break;
        }
        pos=(pos+14+length+3)&~3u;
    }
    free(root);
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

struct Folder { char name[FolderNameMax+1]; char iso[IsvrNameBytes]; bool game, keep; };

bool Sync(IsvrClient& client, Storage& storage, const char* root, const unsigned char* launcher, DWORD launcherSize,
          Note note, SyncResult& result) {
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
    static char names[MaxFolders][FolderNameMax+1];
    static Folder folders[MaxFolders];
    int folderCount=storage.ListFolders(root,names,MaxFolders);
    static char ini[IniMax+1];
    for(int i=0;i<folderCount;++i) {
        Folder& f=folders[i]; memset(&f,0,sizeof(f)); strcpy_s(f.name,names[i]);
        char path[PathMax],folder[PathMax]; Join(folder,root,f.name); Join(path,folder,GameIni);
        DWORD size=0;
        if(storage.ReadFile(path,reinterpret_cast<unsigned char*>(ini),IniMax,size)) {
            ini[size]=0;
            f.game=IniValue(ini,"iso",f.iso,sizeof(f.iso)) && f.iso[0];
        }
    }

    for(int i=0;i<isoCount;++i) {
        bool have=false;
        for(int k=0;k<folderCount;++k) if(folders[k].game && !_stricmp(folders[k].iso,isos[i])) { folders[k].keep=true; have=true; }
        if(have) { ++result.kept; continue; }
        if(!client.Mount(isos[i])) {
            Say(note,"Skipped %s: %s",isos[i],client.Error()); ++result.skipped;
            if(!client.Connected()) return false;
            continue;
        }
        unsigned char info[ExecutionInfoSize]; char error[160];
        if(!GameInfo(client,info,error,sizeof(error))) {
            Say(note,"Skipped %s: %s",isos[i],error); ++result.skipped;
            if(!client.Connected()) return false;
            continue;
        }
        if(folderCount>=MaxFolders) { Say(note,"Too many folders in %s.",root); return false; }
        Folder& f=folders[folderCount]; memset(&f,0,sizeof(f));
        FolderName(isos[i],names,folderCount,f.name);
        strcpy_s(names[folderCount],f.name); strcpy_s(f.iso,isos[i]); f.game=f.keep=true; ++folderCount;
        char folder[PathMax],path[PathMax]; Join(folder,root,f.name);
        unsigned char* copy=static_cast<unsigned char*>(malloc(launcherSize));
        if(!copy) { Say(note,"Out of memory."); return false; }
        memcpy(copy,launcher,launcherSize); memcpy(copy+infoAt,info,ExecutionInfoSize);
        char text[IsvrNameBytes+160];
        _snprintf_s(text,sizeof(text),_TRUNCATE,"; Written by NetISO Sync. iso is the file name the NetISO server lists.\r\niso=%s\r\ntitle_id=%08X\r\nmedia_id=%08X\r\n",
            isos[i],Read32(info+12),Read32(info));
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

    // Folders whose ISO is gone. An empty listing could be a server problem,
    // so nothing is removed then.
    for(int k=0;k<folderCount && isoCount;++k) {
        if(!folders[k].game || folders[k].keep) continue;
        char folder[PathMax]; Join(folder,root,folders[k].name);
        if(storage.RemoveGame(folder)) { Say(note,"Removed %s (%s is no longer on the server).",folders[k].name,folders[k].iso); ++result.removed; }
        else Say(note,"Couldn't remove %s.",folder);
    }
    return true;
}
}
