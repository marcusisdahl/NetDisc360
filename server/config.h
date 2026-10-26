// SPDX-License-Identifier: MIT
#pragma once
// NetDisc360's settings file (--config; on Windows %APPDATA%\NetDisc
// 360\netdisc.ini): "key=value" lines under [Server], kept in UTF-8. The
// window before the dashboard wrote the same file in UTF-16, which is read too.
//
// Also the conversions between the server's paths, which are in the ANSI
// code page on Windows (discs.cpp uses the ...A functions), and the UTF-8 of
// the page and the settings file. Elsewhere both are UTF-8.
#include <stdio.h>
#include <string>
#include <utility>
#include <vector>

namespace config {

class File {
public:
    bool Load(const std::string& path);        // False if there is no such file.
    bool Save(const std::string& path) const;  // Replaces it whole.
    void Read(const std::string& raw);         // The file's bytes, as Load reads them.
    std::string Text() const;                  // What Save writes.
    bool Empty() const { return values_.empty(); }
    bool Has(const char* key) const;
    std::string Get(const char* key, const char* fallback = "") const;
    long long Number(const char* key, long long fallback) const;
    void Set(const char* key, const std::string& value);
    void Remove(const char* key);
private:
    std::vector<std::pair<std::string,std::string> > values_;
};

// A secret kept in the file: on Windows encrypted for this Windows user
// (DPAPI, as the window did), elsewhere as it is (the file is readable by
// its owner only).
std::string Protect(const std::string& secret);
bool Unprotect(const std::string& stored, std::string& secret);

std::string ToUtf8(const std::string& native);
std::string FromUtf8(const std::string& utf8);
FILE* Open(const std::string& utf8Path, const char* mode);
bool IsFolder(const std::string& native);

// Files by UTF-8 path, for the settings and their backups.
bool ReadFile(const std::string& utf8Path, std::string& data, size_t limit);  // False if missing or larger.
// Replaces the file whole, through path.new; secret: readable by its owner only (not on Windows).
bool WriteFile(const std::string& utf8Path, const std::string& data, bool secret);
bool RemoveFile(const std::string& utf8Path);
// The file's size and last change, finer than seconds where the system keeps
// that, to tell whether it changed without reading it. False if missing.
bool Stamp(const std::string& utf8Path, long long& size, long long& time);
bool MakeFolder(const std::string& utf8Path);
struct Entry { std::string name; long long size, time; };
std::vector<Entry> Files(const std::string& utf8Folder);  // The files directly in it.
std::string FolderOf(const std::string& path);            // Up to and with the last separator; "" if none.
}
