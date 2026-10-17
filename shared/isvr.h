// SPDX-License-Identifier: MIT
#pragma once
// Client for the NetISO server protocol ("ISVR", TCP 4323), as spoken by
// tuxuser/netiso-srv and the consolemods server.exe. Each connection has its
// own mounted ISO, so this never changes what the console's plugin has mounted.
#include "platform.h"

namespace nia {
enum { IsvrPort = 4323, IsvrSector = 2048, IsvrNameBytes = 256, IsvrMaxIsos = 1024, IsvrMaxRead = 65536 };

DWORD Read32(const unsigned char* p);
void Write32(unsigned char* p, DWORD value);
double Now();
// "a.b.c.d" for an IPv4 address in network byte order.
void AddressText(DWORD address, char* text, size_t capacity);

class IsvrClient {
public:
    explicit IsvrClient(double timeoutSeconds);
    ~IsvrClient();
    // address is IPv4 in network byte order.
    bool Connect(DWORD address, int port, double connectSeconds);
    bool Ping();
    // Name of ISO number index; an empty name means there is no such ISO.
    bool Name(int index, char* name /* IsvrNameBytes */);
    // Mounts by exact file name (the server matches the end of the name).
    bool Mount(const char* name);
    U64 Size() const { return size_; }
    bool Connected() const { return socket_!=INVALID_SOCKET; }
    bool Read(U64 offset, DWORD length, unsigned char* data);
    void Close();
    const char* Error() const { return error_; }
private:
    IsvrClient(const IsvrClient&);
    IsvrClient& operator=(const IsvrClient&);
    bool Fail(const char* format, ...);
    bool Wait(bool writing, double deadline);
    bool SendAll(const unsigned char* data, int count);
    bool ReceiveAll(unsigned char* data, DWORD count);
    SOCKET socket_;
    double timeout_;
    U64 size_;
    char error_[160];
};
}
