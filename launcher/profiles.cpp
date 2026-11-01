// SPDX-License-Identifier: MIT
// Offline profiles made by the console (see profiles.h).
#include "profiles.h"
#include <stdio.h>
#include <string.h>

extern "C" LONG __stdcall XexGetModuleHandle(const char* name,HANDLE* module);
extern "C" LONG __stdcall XexGetProcedureAddress(HANDLE module,DWORD ordinal,void** address);
struct ProfileKernelString { USHORT length,capacity; char* text; };
extern "C" LONG __stdcall ObCreateSymbolicLink(ProfileKernelString* link,ProfileKernelString* device);
extern "C" LONG __stdcall ObDeleteSymbolicLink(ProfileKernelString* link);

namespace {
// X_XAMACCOUNTINFO: the profile's Account file before xam encrypts it,
// 0x17C bytes, big-endian like everything here.
#pragma pack(push,4)
struct AccountInfo {
    DWORD reservedFlags, liveFlags;
    WCHAR gamertag[16];
    ULONGLONG xuidOnline;
    DWORD cachedUserFlags, networkId;
    BYTE passcode[4];
    char onlineDomain[0x14], kerberosRealm[0x18];
    BYTE onlineKey[0x10];
    char passportMember[0x72], passportPassword[0x20], ownerPassportMember[0x72];
};
#pragma pack(pop)
typedef char AccountInfoSize[sizeof(AccountInfo)==0x17C ? 1 : -1];
const DWORD LanguageMask=0x3E000000, LanguageShift=25;

enum { OrdinalProfileCreate=0x230, OrdinalCreateEnumerator=0x231, OrdinalEnumerate=0x232, OrdinalCreationStatus=0x234, OrdinalFindAccount=0x235 };
typedef DWORD (__stdcall* ProfileCreate)(DWORD flags, DWORD* device, ULONGLONG xuid, AccountInfo* account,
                                          void* payment, void* userToken, void* ownerToken, DWORD* creation);
typedef DWORD (__stdcall* CreationStatus)(DWORD creation, ULONGLONG* offlineXuid);
typedef DWORD (__stdcall* FindAccount)(ULONGLONG xuid, AccountInfo* account, DWORD* device);
// PROFILEENUMRESULT (xkelib), 0x188 bytes.
#pragma pack(push,4)
struct EnumResult { ULONGLONG xuidOffline; AccountInfo account; DWORD device; };
#pragma pack(pop)
typedef char EnumResultSize[sizeof(EnumResult)==0x188 ? 1 : -1];
typedef DWORD (__stdcall* CreateEnumerator)(DWORD device, HANDLE* handle);
typedef DWORD (__stdcall* Enumerate)(HANDLE handle, DWORD flags, EnumResult* result, void* overlapped);
const DWORD LiveEnabled=0x20000000;
// XamProfileCreate's flags for a new profile. With 0 it answered
// ERROR_PATH_NOT_FOUND for the XUID it was given (it seems to look for an
// existing profile then); with 1 it made one (seen on the console,
// 2026-11-01), under an XUID of its own, whatever XUID it was given. It
// turns XUID 0 down as an invalid argument (with flags 0; not tried with 1).
const DWORD NewProfileFlag=1;
// Seconds a creation may take before it counts as failed (the dashboard's
// takes a moment: it writes the profile's package to the drive).
const DWORD CreationMostMs=60000, CreationPollMs=100;
// What XamProfileGetCreationStatus answers while xam is still at it: E_PENDING
// (seen on the console 2026-11-01, 0.16 s into a creation), not the
// ERROR_IO_PENDING xenia's notes expect.
const DWORD StillPending=0x8000000A;

void* Xam(DWORD ordinal) {
    HANDLE xam=NULL; void* address=NULL;
    if(XexGetModuleHandle("xam.xex",&xam)<0 || !xam || XexGetProcedureAddress(xam,ordinal,&address)<0) return NULL;
    return address;
}

void Pause(void (*wait)(int), int ms) { if(wait) wait(ms); else Sleep(ms); }

// The hard drive (device 1) as a drive, for the new profile's folders.
const char HddLink[]="\\??\\NdProf:", HddDevice[]="\\Device\\Harddisk0\\Partition1", HddRoot[]="NdProf:";
bool LinkHdd(bool on) {
    char name[32],device[64]; strcpy_s(name,HddLink); strcpy_s(device,HddDevice);
    ProfileKernelString link={USHORT(strlen(name)),USHORT(sizeof(name)),name}, target={USHORT(strlen(device)),USHORT(sizeof(device)),device};
    ObDeleteSymbolicLink(&link);
    return !on || ObCreateSymbolicLink(&link,&target)>=0;
}
// The folders a profile's package goes in, Content\<XUID>\FFFE07D1\00010000,
// from the top: the first count of them.
void ProfileFolder(ULONGLONG xuid, int count, char* path, size_t capacity) {
    sprintf_s(path,capacity,"%s\\Content",HddRoot);
    if(count>=1) { size_t at=strlen(path); sprintf_s(path+at,capacity-at,"\\%016I64X",xuid); }
    if(count>=2) strcat_s(path,capacity,"\\FFFE07D1");
    if(count>=3) strcat_s(path,capacity,"\\00010000");
}

// Zeroed records for zeroTokens: X_USER_PAYMENT_INFO is under 2 KB and a Live
// token 0x1C bytes, so these are big enough whatever xam reads.
BYTE ZeroPayment[4096], ZeroToken[2][256];
}

namespace nia {
int ListProfiles(ProfileInfo* out, int capacity) {
    CreateEnumerator start=(CreateEnumerator)Xam(OrdinalCreateEnumerator);
    Enumerate next=(Enumerate)Xam(OrdinalEnumerate);
    HANDLE handle=NULL;
    if(!start || !next || start(0,&handle)!=ERROR_SUCCESS || !handle) return 0;
    int count=0;
    static EnumResult result;
    for(int guard=0;count<capacity && guard<256;++guard) {
        memset(&result,0,sizeof(result));
        if(next(handle,0,&result,NULL)!=ERROR_SUCCESS) break;
        ProfileInfo& p=out[count++];
        memset(&p,0,sizeof(p));
        p.xuid=result.xuidOffline; p.xuidOnline=result.account.xuidOnline;
        memcpy(p.gamertag,result.account.gamertag,GamertagMost*sizeof(WCHAR));
        p.device=result.device; p.reservedFlags=result.account.reservedFlags; p.liveFlags=result.account.liveFlags;
        p.cachedUserFlags=result.account.cachedUserFlags; p.networkId=result.account.networkId;
    }
    CloseHandle(handle);
    return count;
}

ProfileCall DefaultProfileCall(const ProfileInfo* profiles, int count) {
    ProfileCall call;
    memset(&call,0,sizeof(call));
    call.device=1;
    call.flags=NewProfileFlag;
    call.cachedUserFlags=(DWORD(XGetLanguage())<<LanguageShift)&LanguageMask;
    // An offline one (not Live) first, else any.
    const ProfileInfo* like=NULL;
    for(int i=0;i<count && !like;++i) if(!(profiles[i].reservedFlags&LiveEnabled) && !profiles[i].xuidOnline) like=&profiles[i];
    if(!like && count) like=&profiles[0];
    if(like) {
        call.device=like->device;
        call.reservedFlags=like->reservedFlags&0xFF;  // Its version; no Live, no passcode.
        call.cachedUserFlags=like->cachedUserFlags&(LanguageMask|0xFF00);  // Its language and country.
    }
    return call;
}

ULONGLONG NewOfflineXuid(ULONGLONG like, const ProfileInfo* profiles, int count) {
    ULONGLONG low=like&0xFFFFFFFFull;
    for(DWORD seed=GetTickCount();;seed=seed*1103515245+12345) {
        ULONGLONG xuid=0xE000000000000000ull|(ULONGLONG((seed>>8)&0xFFFF)<<32)|low;
        bool used=false;
        for(int i=0;i<count && !used;++i) used=profiles[i].xuid==xuid;
        if(!used && ((seed>>8)&0xFFFF)) return xuid;
    }
}

bool MakeProfileWith(const WCHAR* gamertag, size_t length, const ProfileCall& call, ULONGLONG& xuid, DWORD& code, char* error, size_t capacity, void (*wait)(int ms));
bool MakeProfile(const WCHAR* gamertag, const ProfileCall& call, ULONGLONG& xuid, DWORD& code, char* error, size_t capacity, void (*wait)(int ms)) {
    xuid=0;
    code=0;
    error[0]=0;
    size_t length=gamertag ? wcslen(gamertag) : 0;
    if(!length || length>GamertagMost) { sprintf_s(error,capacity,"A gamertag has 1 to %d characters.",int(GamertagMost)); return false; }
    ProfileCreate create=(ProfileCreate)Xam(OrdinalProfileCreate);
    CreationStatus status=(CreationStatus)Xam(OrdinalCreationStatus);
    if(!create || !status) { sprintf_s(error,capacity,"This console's xam.xex doesn't have the profile calls."); return false; }
    // xam answers ERROR_PATH_NOT_FOUND for an XUID whose folders aren't
    // there (seen on the console), so they're made first, and the ones made
    // are removed again if it fails.
    bool folders=call.xuid && call.makeFolders, made=false;
    bool madeAt[4]={false,false,false,false};
    char folder[96];
    if(folders && !LinkHdd(true)) { sprintf_s(error,capacity,"Couldn't reach the hard drive for its folders."); return false; }
    for(int depth=1;folders && depth<=3 && !error[0];++depth) {
        ProfileFolder(call.xuid,depth,folder,sizeof(folder));
        if(CreateDirectory(folder,NULL)) madeAt[depth]=true;
        else if(GetLastError()!=ERROR_ALREADY_EXISTS) sprintf_s(error,capacity,"Couldn't make %s (%lu).",folder,GetLastError());
    }
    if(!error[0]) made=MakeProfileWith(gamertag,length,call,xuid,code,error,capacity,wait);
    // The folders made here go again (if xam left them empty) when it failed,
    // or made it under an XUID of its own (seen on the console: it does).
    for(int depth=3;folders && (!made || xuid!=call.xuid) && depth>=1;--depth)
        if(madeAt[depth]) { ProfileFolder(call.xuid,depth,folder,sizeof(folder)); RemoveDirectory(folder); }
    if(folders) LinkHdd(false);
    return made;
}

// MakeProfile once its folders are there.
bool MakeProfileWith(const WCHAR* gamertag, size_t length, const ProfileCall& call, ULONGLONG& xuid, DWORD& code, char* error, size_t capacity, void (*wait)(int ms)) {
    ProfileCreate create=(ProfileCreate)Xam(OrdinalProfileCreate);
    CreationStatus status=(CreationStatus)Xam(OrdinalCreationStatus);
    static AccountInfo account;
    memset(&account,0,sizeof(account));
    memcpy(account.gamertag,gamertag,length*sizeof(WCHAR));
    account.reservedFlags=call.reservedFlags;
    account.cachedUserFlags=call.cachedUserFlags;
    DWORD device=call.device, creation=0;
    void* payment=call.zeroTokens ? ZeroPayment : NULL;
    void* userToken=call.zeroTokens ? ZeroToken[0] : NULL;
    void* ownerToken=call.zeroTokens ? ZeroToken[1] : NULL;
    DWORD result=create(call.flags,&device,call.xuid,&account,payment,userToken,ownerToken,&creation);
    code=result;
    if(result!=ERROR_SUCCESS && result!=ERROR_IO_PENDING) { sprintf_s(error,capacity,"xam didn't start it (XamProfileCreate %08lX).",result); return false; }
    if(!creation) { sprintf_s(error,capacity,"xam gave nothing to follow it by (XamProfileCreate %08lX).",result); return false; }
    for(DWORD start=GetTickCount();;) {
        ULONGLONG made=0;
        result=status(creation,&made);
        code=result;
        if(result==ERROR_SUCCESS) { xuid=made; break; }
        if(result!=ERROR_IO_PENDING && result!=StillPending) { sprintf_s(error,capacity,"xam couldn't make it (XamProfileGetCreationStatus %08lX).",result); return false; }
        if(GetTickCount()-start>CreationMostMs) { sprintf_s(error,capacity,"xam was still at it after %lu seconds (%08lX).",CreationMostMs/1000,result); return false; }
        Pause(wait,CreationPollMs);
    }
    if(!xuid) { sprintf_s(error,capacity,"xam made it but gave no XUID."); return false; }
    return true;
}

bool HasProfile(ULONGLONG xuid, WCHAR* gamertag) {
    if(gamertag) gamertag[0]=0;
    FindAccount find=(FindAccount)Xam(OrdinalFindAccount);
    if(!find || !xuid) return false;
    static AccountInfo account;
    memset(&account,0,sizeof(account));
    DWORD device=0;
    if(find(xuid,&account,&device)!=ERROR_SUCCESS) return false;
    if(gamertag) { memcpy(gamertag,account.gamertag,GamertagMost*sizeof(WCHAR)); gamertag[GamertagMost]=0; }
    return true;
}
}
