// SPDX-License-Identifier: MIT
#pragma once
// FTP client for Aurora's FTP server (FtpDll), and NetISO Sync's Storage on
// top of it, so the PC server can keep the console's game folders current.
//
// FtpDll, as seen from Aurora 0.7b.2 (2026-10-17): PASV only (no EPSV or
// MLSD), and LIST ignores a path argument and lists the current folder, so
// folders are always entered with CWD first. After a timeout its replies can
// fall one behind the commands, so any failure drops the connection and the
// next command logs in again.
#include "../shared/library.h"
#include <string>
#include <vector>

namespace nia {
class Link;

struct FtpEntry { std::string name; bool folder; };

class FtpClient {
public:
    // address is IPv4 in network byte order.
    FtpClient(DWORD address, int port, const char* user, const char* password, double timeoutSeconds);
    ~FtpClient();
    // Connects and logs in unless already connected.
    bool Open();
    // Whether an FTP server answers: connects, reads its greeting and leaves
    // without logging in.
    bool Greets();
    bool List(const char* folder, std::vector<FtpEntry>& entries);
    // Whole file into data; false if missing (Missing() is then true) or over limit bytes.
    bool Retrieve(const char* path, std::vector<unsigned char>& data, size_t limit);
    bool Store(const char* path, const void* data, size_t size);
    bool MakeFolder(const char* path);
    bool Exists(const char* folder);
    bool Delete(const char* path);
    bool RemoveFolder(const char* path);
    // A SITE command (FtpDll's own, such as INITIATESCAN); reply is its last
    // line. True for a 2xx reply.
    bool Site(const char* command, std::string& reply);
    void Close();
    bool Missing() const { return missing_; }
    // Last reply code, or 0 for a connection problem.
    int Code() const { return code_; }
    const char* Error() const { return error_; }
private:
    FtpClient(const FtpClient&);
    FtpClient& operator=(const FtpClient&);
    bool Fail(const char* format, ...);
    bool ReadLine(std::string& line);
    bool Reply();
    bool Send(const char* format, ...);
    bool Command(int expected, const char* format, ...);
    bool Enter(const std::string& folder);
    bool Passive(Link& data);
    bool ReceiveAll(Link& data, std::vector<unsigned char>& out, size_t limit);
    DWORD address_;
    int port_;
    std::string user_, password_, folder_, buffer_, text_;
    double timeout_;
    Link* control_;
    int code_;
    bool missing_;
    char error_[320];
};

class FtpStorage : public Storage {
public:
    explicit FtpStorage(FtpClient& ftp) : ftp_(ftp) {}
    int ListFolders(const char* root, char names[][FolderNameMax + 1], int capacity);
    bool ReadFile(const char* path, unsigned char* data, DWORD capacity, DWORD& size);
    bool WriteFile(const char* path, const void* data, DWORD size);
    bool MakeFolder(const char* path);
    bool RemoveGame(const char* folder);
    int RemoveFile(const char* path);
    unsigned long LastError();
private:
    FtpClient& ftp_;
};

// Parses one line of a Unix-style LIST reply; false if it isn't one.
bool ParseListLine(const std::string& line, FtpEntry& entry);
}
