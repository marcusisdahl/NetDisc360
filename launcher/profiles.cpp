// SPDX-License-Identifier: MIT
// Offline profiles made by the console (see profiles.h).
#include "profiles.h"
#include <stdio.h>
#include <string.h>

extern "C" LONG __stdcall XexGetModuleHandle(const char* name,HANDLE* module);
extern "C" LONG __stdcall XexGetProcedureAddress(HANDLE module,DWORD ordinal,void** address);

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

enum { OrdinalProfileCreate=0x230, OrdinalCreationStatus=0x234, OrdinalFindAccount=0x235 };
typedef DWORD (__stdcall* ProfileCreate)(DWORD flags, DWORD* device, ULONGLONG xuid, AccountInfo* account,
                                          void* payment, void* userToken, void* ownerToken, DWORD* creation);
typedef DWORD (__stdcall* CreationStatus)(DWORD creation, ULONGLONG* offlineXuid);
typedef DWORD (__stdcall* FindAccount)(ULONGLONG xuid, AccountInfo* account, DWORD* device);
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

// Zeroed records for zeroTokens: X_USER_PAYMENT_INFO is under 2 KB and a Live
// token 0x1C bytes, so these are big enough whatever xam reads.
BYTE ZeroPayment[4096], ZeroToken[2][256];
}

namespace nia {
bool MakeProfile(const WCHAR* gamertag, const ProfileCall& call, ULONGLONG& xuid, char* error, size_t capacity, void (*wait)(int ms)) {
    xuid=0;
    error[0]=0;
    size_t length=gamertag ? wcslen(gamertag) : 0;
    if(!length || length>GamertagMost) { sprintf_s(error,capacity,"A gamertag has 1 to %d characters.",int(GamertagMost)); return false; }
    ProfileCreate create=(ProfileCreate)Xam(OrdinalProfileCreate);
    CreationStatus status=(CreationStatus)Xam(OrdinalCreationStatus);
    if(!create || !status) { sprintf_s(error,capacity,"This console's xam.xex doesn't have the profile calls."); return false; }
    static AccountInfo account;
    memset(&account,0,sizeof(account));
    memcpy(account.gamertag,gamertag,length*sizeof(WCHAR));
    // The console's language, as a profile made in its dashboard gets.
    account.cachedUserFlags=(DWORD(XGetLanguage())<<LanguageShift)&LanguageMask;
    DWORD device=call.device, creation=0;
    void* payment=call.zeroTokens ? ZeroPayment : NULL;
    void* userToken=call.zeroTokens ? ZeroToken[0] : NULL;
    void* ownerToken=call.zeroTokens ? ZeroToken[1] : NULL;
    DWORD result=create(call.flags,&device,0,&account,payment,userToken,ownerToken,&creation);
    if(result!=ERROR_SUCCESS && result!=ERROR_IO_PENDING) { sprintf_s(error,capacity,"xam didn't start it (XamProfileCreate %08lX).",result); return false; }
    if(!creation) { sprintf_s(error,capacity,"xam gave nothing to follow it by (XamProfileCreate %08lX).",result); return false; }
    for(DWORD start=GetTickCount();;) {
        ULONGLONG made=0;
        result=status(creation,&made);
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
