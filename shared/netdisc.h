// SPDX-License-Identifier: MIT
#pragma once
// The disc drive in the NetDisc360.xex plugin (plugin/disc.cpp), as the
// launcher talks to it: the device \Device\NetDisc360 and three controls.
//
// Open the device (NtOpenFile on DeviceName) and send the controls with
// NtDeviceIoControlFile. Mount and Eject only hand the work to the drive and
// give back a ticket; Status tells when the drive has finished that ticket
// and how it went. So whoever asked can stop waiting at any time (B on the
// loading screen, or the title ending) without leaving the drive waiting on
// it, and the drive's own thread is the only one that talks to the server.
//
// The plugin's own disc swaps call the drive directly (plugin/disc.h).
#include <xtl.h>

namespace netdisc {

// Opened with the backslash after it, the way \Device\NetIso was opened.
static const char DeviceName[]="\\Device\\NetDisc360\\";
enum { Version = 1, NameBytes = 256, DefaultPort = 4323 };

// CTL_CODE(0x8360, 0x800 + n, METHOD_NEITHER, FILE_ANY_ACCESS): the input
// and output buffers are the caller's own.
static const DWORD ControlStatus=0x83602003;  // out: Status
static const DWORD ControlMount =0x83602007;  // in: MountRequest; out: DWORD ticket
static const DWORD ControlEject =0x8360200B;  // out: DWORD ticket

struct MountRequest {
    DWORD version;          // Version
    DWORD address;          // The server's IPv4 address, network byte order; 0 = the server the drive uses now.
    DWORD port;             // 0 = DefaultPort.
    char iso[NameBytes];    // The ISO's name as the server lists it, ending in 0.
};

// What the drive is doing now.
enum State {
    StateOff,         // Not the disc drive (NetISO.xex is, or own_disc=0).
    StateIdle,        // No disc from the server in the drive.
    StateConnecting,  // Waiting for the console's network or the server.
    StateMounting,    // The server is opening the disc.
    StateMounted,     // A disc from the server is in the drive.
    StateEjecting
};
// How a finished ticket went.
enum Result {
    ResultNone,         // No ticket finished yet.
    ResultMounted,      // The disc is in; the tray went out and in.
    ResultEjected,
    ResultNoNetwork,    // The console's network didn't come up.
    ResultNoServer,     // Nothing answered at the address, or it isn't a NetISO server.
    ResultNotOnServer,  // The server doesn't have that ISO.
    ResultNotADisc,     // The server opened it, but it has no Xbox file system.
    ResultLost          // The server went away during the mount.
};

struct Status {
    DWORD version;        // The drive's Version.
    DWORD size;           // sizeof(Status) as the drive filled it.
    DWORD state;          // State
    DWORD ticket;         // The latest ticket given out.
    DWORD done;           // The latest ticket finished,
    DWORD result;         // and its Result.
    DWORD address;        // The server, network byte order (0 = none yet),
    DWORD port;
    DWORD connected;      // 1 while the drive's connection to it is open.
    DWORD sectors;        // Of the disc in the drive, 2048 bytes each; 0 = none.
    DWORD partition;      // Where its game file system starts, in bytes.
    DWORD mediaId;        // The first 4 bytes of its media ID, 0 if it has none.
    DWORD reads;          // Disc reads served since the plugin started,
    DWORD readMb;         // their megabytes,
    DWORD failedReads;    // reads the server didn't answer even after reconnecting,
    DWORD reconnects;     // and connections made again under a mounted disc.
    DWORD slowestReadMs;  // The longest a read waited for the server, this disc.
    DWORD readBytes;      // Settings: the most asked for in one request,
    DWORD depth;          // how many requests are in flight at once,
    DWORD receiveBytes;   // and the connection's receive buffer (0 = XNet's own).
    char iso[NameBytes];  // The disc in the drive.
    char note[128];       // Why it's off, or what went wrong last.
};

}
