// SPDX-License-Identifier: MIT
#pragma once
// The disc drive from the server, built into the NetDisc360.xex plugin: what
// the closed NetISO.xex 1.0 plugin did, so the console needs only this one.
// See disc.cpp; the launcher reaches it through shared/netdisc.h.
#include <xtl.h>
#include "../shared/netdisc.h"

namespace disc {
struct Settings {
    DWORD readBytes;    // The most asked for in one request (read_kb=).
    int depth;          // Requests in flight at once (read_depth=).
    int receiveBytes;   // SO_RCVBUF on the connection (rcv_kb=), 0 = XNet's own.
    bool enabled;       // own_disc=0 in NetDisc360.ini turns the drive off.
    // The server from server= in NetDisc360.ini or NetISO.xex.txt (network
    // byte order; 0 = none). The drive connects to it at start and stays
    // connected, as NetISO.xex did, so the server sees the console.
    DWORD address;
    int port;
    const char* about;  // Where the settings came from, for the drive's log; kept.
};
// At DllMain. The drive's thread starts at once; a couple of seconds later,
// when DashLaunch has loaded the other plugins, it becomes the disc drive
// (\Device\NetDisc360, \Device\Cdrom0, the tray and the media ID), unless
// NetISO.xex is loaded or the settings turn it off.
void Start(const Settings& settings);
// At unload: gives the drive back to the console as it was.
void Stop();
// Whether this plugin is the disc drive (and NetISO.xex isn't).
bool Active();
// Why it's on or off, for the logs.
const char* Note();
// The same as the controls in shared/netdisc.h, for the plugin's own use:
// a ticket (0 when the drive is off), and its state.
DWORD Mount(DWORD address,int port,const char* iso);
DWORD Eject();
void GetStatus(netdisc::Status& status);
// Whether the ticket is finished; result gets its netdisc::Result.
bool Finished(DWORD ticket,DWORD& result);
}
