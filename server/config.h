// SPDX-License-Identifier: MIT
#pragma once
// NetDisc 360's settings file (--config; on Windows %APPDATA%\NetDisc
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
}
