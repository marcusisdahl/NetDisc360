// SPDX-License-Identifier: MIT
// The settings file and text conversions (see config.h).
#include "config.h"
#include "../shared/platform.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace config {
namespace {
const char Section[] = "[Server]";

std::string Lower(std::string text) {
    for(size_t i=0;i<text.size();++i) if(text[i]>='A' && text[i]<='Z') text[i]=char(text[i]-'A'+'a');
    return text;
}

std::string Trim(const std::string& text) {
    size_t a=text.find_first_not_of(" \t\r\n"), b=text.find_last_not_of(" \t\r\n");
    return a==std::string::npos ? std::string() : text.substr(a,b-a+1);
}

// UTF-16LE (the window's file, after its byte order mark) as UTF-8.
std::string FromUtf16(const unsigned char* p, size_t bytes) {
    std::string out;
    for(size_t i=0;i+1<bytes;i+=2) {
        unsigned c=p[i]|(unsigned(p[i+1])<<8);
        if(c>=0xD800 && c<0xDC00 && i+3<bytes) {
            unsigned low=p[i+2]|(unsigned(p[i+3])<<8);
            if(low>=0xDC00 && low<0xE000) { c=0x10000+((c-0xD800)<<10)+(low-0xDC00); i+=2; }
        }
        if(c<0x80) out+=char(c);
        else if(c<0x800) { out+=char(0xC0|(c>>6)); out+=char(0x80|(c&0x3F)); }
        else if(c<0x10000) { out+=char(0xE0|(c>>12)); out+=char(0x80|((c>>6)&0x3F)); out+=char(0x80|(c&0x3F)); }
        else { out+=char(0xF0|(c>>18)); out+=char(0x80|((c>>12)&0x3F)); out+=char(0x80|((c>>6)&0x3F)); out+=char(0x80|(c&0x3F)); }
    }
    return out;
}

#ifdef _WIN32
std::wstring Wide(const std::string& text, UINT page) {
    int n=MultiByteToWideChar(page,0,text.data(),int(text.size()),NULL,0);
    std::wstring out(size_t(n),L'\0');
    if(n) MultiByteToWideChar(page,0,text.data(),int(text.size()),&out[0],n);
    return out;
}

std::string Narrow(const std::wstring& text, UINT page) {
    int n=WideCharToMultiByte(page,0,text.data(),int(text.size()),NULL,0,NULL,NULL);
    std::string out(size_t(n),'\0');
    if(n) WideCharToMultiByte(page,0,text.data(),int(text.size()),&out[0],n,NULL,NULL);
    return out;
}
#endif
}  // namespace

bool File::Load(const std::string& path) {
    std::string raw;
    if(!ReadFile(path,raw,16<<20)) return false;
    Read(raw);
    return true;
}

void File::Read(const std::string& raw) {
    const unsigned char* bytes=reinterpret_cast<const unsigned char*>(raw.data());
    std::string text;
    if(raw.size()>=2 && bytes[0]==0xFF && bytes[1]==0xFE) text=FromUtf16(bytes+2,raw.size()-2);
    else if(raw.size()>=3 && !memcmp(raw.data(),"\xEF\xBB\xBF",3)) text=raw.substr(3);
    else text=raw;
    values_.clear();
    for(size_t at=0;at<text.size();) {
        size_t end=text.find('\n',at);
        if(end==std::string::npos) end=text.size();
        std::string line=Trim(text.substr(at,end-at));
        at=end+1;
        size_t equals=line.find('=');
        if(line.empty() || line[0]=='[' || line[0]==';' || line[0]=='#' || equals==std::string::npos) continue;
        Set(Trim(line.substr(0,equals)).c_str(),Trim(line.substr(equals+1)));
    }
}

std::string File::Text() const {
    std::string text=std::string(Section)+"\n";
    for(size_t i=0;i<values_.size();++i) text+=values_[i].first+"="+values_[i].second+"\n";
    return text;
}

bool File::Save(const std::string& path) const { return WriteFile(path,Text(),true); }

bool File::Has(const char* key) const {
    std::string k=Lower(key);
    for(size_t i=0;i<values_.size();++i) if(values_[i].first==k) return true;
    return false;
}

std::string File::Get(const char* key, const char* fallback) const {
    std::string k=Lower(key);
    for(size_t i=0;i<values_.size();++i) if(values_[i].first==k) return values_[i].second;
    return fallback;
}

long long File::Number(const char* key, long long fallback) const {
    std::string value=Get(key);
    return value.empty() ? fallback : atoll(value.c_str());
}

void File::Set(const char* key, const std::string& value) {
    std::string k=Lower(key), v;
    for(size_t i=0;i<value.size();++i) if(value[i]!='\n' && value[i]!='\r') v+=value[i];
    for(size_t i=0;i<values_.size();++i) if(values_[i].first==k) { values_[i].second=v; return; }
    values_.push_back(std::make_pair(k,v));
}

void File::Remove(const char* key) {
    std::string k=Lower(key);
    for(size_t i=0;i<values_.size();++i) if(values_[i].first==k) values_.erase(values_.begin()+long(i--));
}

#ifdef _WIN32
std::string Protect(const std::string& secret) {
    std::wstring text=Wide(secret,CP_UTF8);
    DATA_BLOB in={DWORD(text.size()*sizeof(wchar_t)),(BYTE*)text.data()}, out={0,NULL};
    if(!CryptProtectData(&in,L"NetDisc360 FTP password",NULL,NULL,NULL,CRYPTPROTECT_UI_FORBIDDEN,&out)) return std::string();
    DWORD n=0;
    std::string result;
    if(CryptBinaryToStringA(out.pbData,out.cbData,CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,NULL,&n)) {
        result.resize(n);
        CryptBinaryToStringA(out.pbData,out.cbData,CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,&result[0],&n);
        result.resize(n);
    }
    LocalFree(out.pbData);
    return result;
}

bool Unprotect(const std::string& stored, std::string& secret) {
    DWORD n=0;
    if(stored.empty() || !CryptStringToBinaryA(stored.c_str(),0,CRYPT_STRING_BASE64,NULL,&n,NULL,NULL)) return false;
    std::vector<BYTE> data(n);
    if(!CryptStringToBinaryA(stored.c_str(),0,CRYPT_STRING_BASE64,data.data(),&n,NULL,NULL)) return false;
    DATA_BLOB in={n,data.data()}, out={0,NULL};
    if(!CryptUnprotectData(&in,NULL,NULL,NULL,NULL,CRYPTPROTECT_UI_FORBIDDEN,&out)) return false;
    secret=Narrow(std::wstring((const wchar_t*)out.pbData,out.cbData/sizeof(wchar_t)),CP_UTF8);
    SecureZeroMemory(out.pbData,out.cbData);
    LocalFree(out.pbData);
    return true;
}

std::string ToUtf8(const std::string& native) { return Narrow(Wide(native,CP_ACP),CP_UTF8); }
std::string FromUtf8(const std::string& utf8) { return Narrow(Wide(utf8,CP_UTF8),CP_ACP); }
FILE* Open(const std::string& utf8Path, const char* mode) { return _wfopen(Wide(utf8Path,CP_UTF8).c_str(),Wide(mode,CP_UTF8).c_str()); }

bool IsFolder(const std::string& native) {
    DWORD attributes=GetFileAttributesA(native.c_str());
    return attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_DIRECTORY);
}
#else
std::string Protect(const std::string& secret) { return secret; }
bool Unprotect(const std::string& stored, std::string& secret) { secret=stored; return true; }
std::string ToUtf8(const std::string& native) { return native; }
std::string FromUtf8(const std::string& utf8) { return utf8; }
FILE* Open(const std::string& utf8Path, const char* mode) { return fopen(utf8Path.c_str(),mode); }

bool IsFolder(const std::string& native) {
    struct stat st;
    return !stat(native.c_str(),&st) && S_ISDIR(st.st_mode);
}
#endif

bool ReadFile(const std::string& utf8Path, std::string& data, size_t limit) {
    FILE* f=Open(utf8Path,"rb");
    if(!f) return false;
    data.clear();
    char part[65536]; size_t got;
    while((got=fread(part,1,sizeof(part),f))>0 && data.size()<=limit) data.append(part,got);
    fclose(f);
    return data.size()<=limit;
}

bool WriteFile(const std::string& utf8Path, const std::string& data, bool secret) {
    std::string temporary=utf8Path+".new";
    FILE* f=Open(temporary,"wb");
    if(!f) return false;
#ifndef _WIN32
    if(secret) fchmod(fileno(f),0600);
#else
    (void)secret;
#endif
    bool ok=fwrite(data.data(),1,data.size(),f)==data.size();
    ok=fclose(f)==0 && ok;
#ifdef _WIN32
    ok=ok && MoveFileExW(Wide(temporary,CP_UTF8).c_str(),Wide(utf8Path,CP_UTF8).c_str(),MOVEFILE_REPLACE_EXISTING);
#else
    ok=ok && !rename(temporary.c_str(),utf8Path.c_str());
#endif
    if(!ok) RemoveFile(temporary);
    return ok;
}

std::string FolderOf(const std::string& path) {
    size_t cut=path.find_last_of("/\\");
    return cut==std::string::npos ? std::string() : path.substr(0,cut+1);
}

#ifdef _WIN32
bool RemoveFile(const std::string& utf8Path) { return DeleteFileW(Wide(utf8Path,CP_UTF8).c_str())!=0; }

bool MakeFolder(const std::string& utf8Path) {
    std::wstring path=Wide(utf8Path,CP_UTF8);
    if(CreateDirectoryW(path.c_str(),NULL)) return true;
    DWORD attributes=GetFileAttributesW(path.c_str());
    return attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_DIRECTORY);
}

std::vector<Entry> Files(const std::string& utf8Folder) {
    std::vector<Entry> out;
    WIN32_FIND_DATAW found;
    std::string folder=utf8Folder;
    if(!folder.empty() && folder[folder.size()-1]!='\\' && folder[folder.size()-1]!='/') folder+='\\';
    HANDLE search=FindFirstFileW(Wide(folder+"*",CP_UTF8).c_str(),&found);
    if(search==INVALID_HANDLE_VALUE) return out;
    do {
        if(found.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) continue;
        // FILETIME counts 100 ns from 1601.
        long long time=(((long long)found.ftLastWriteTime.dwHighDateTime<<32)|found.ftLastWriteTime.dwLowDateTime)/10000000-11644473600LL;
        Entry e={Narrow(found.cFileName,CP_UTF8),((long long)found.nFileSizeHigh<<32)|found.nFileSizeLow,time};
        out.push_back(e);
    } while(FindNextFileW(search,&found));
    FindClose(search);
    return out;
}
#else
bool RemoveFile(const std::string& utf8Path) { return !unlink(utf8Path.c_str()); }

bool MakeFolder(const std::string& utf8Path) { return !mkdir(utf8Path.c_str(),0700) || IsFolder(utf8Path); }

std::vector<Entry> Files(const std::string& utf8Folder) {
    std::vector<Entry> out;
    DIR* dir=opendir(utf8Folder.c_str());
    if(!dir) return out;
    std::string folder=utf8Folder;
    if(!folder.empty() && folder[folder.size()-1]!='/') folder+='/';
    while(struct dirent* d=readdir(dir)) {
        struct stat st;
        if(stat((folder+d->d_name).c_str(),&st) || !S_ISREG(st.st_mode)) continue;
        Entry e={d->d_name,(long long)st.st_size,(long long)st.st_mtime};
        out.push_back(e);
    }
    closedir(dir);
    return out;
}
#endif
}
