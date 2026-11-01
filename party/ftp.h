// SPDX-License-Identifier: MIT
#pragma once
// A very small FTP server in NetDiscParty.xex, as in FastFTP360, for one
// job: NetDisc360 puts the guests' profiles on this console while the wheel
// is up. Aurora's own FTP server is gone while another app runs, so this
// one answers on the same port (21) in its place, and NetDisc360's FTP code
// works with either.
//
// It does only what that takes. Only NetDisc360's computer may connect (the
// server the app talks to; 0 lets anyone, for tests). It shows /Hdd1/Content
// and the offline profiles' content folders in it (Hdd1:\Content\E...), may
// read those, and may write, rename and delete only inside them: a console's
// own profiles (online XUIDs), Aurora and the games are out of its reach.
// A file is at most FtpFileMost bytes.
//
// FtpActivity says whether NetDisc360 is copying now, so the app doesn't
// leave in the middle of it.
namespace party {
enum { FtpPort = 21, FtpSessionsMost = 4 };
const unsigned long long FtpFileMost = 512ull << 20;

// Starts the server on its own thread: /Hdd1 is the folder root (on the
// Xbox a drive linked to \Device\Harddisk0\Partition1, as "PartyF:"; any
// folder in the host tests). allowFrom: the only address that may connect
// (network order); AllowFrom changes it later. False if it can't listen.
bool FtpStart(const char* root, int port, unsigned long allowFrom);
// The same on one address of the computer's (the tests' fake consoles).
bool FtpStartAt(const char* root, unsigned long address, int port, unsigned long allowFrom);
void FtpAllowFrom(unsigned long address);

struct FtpActivity {
    int sessions;          // Logged in now.
    int transfers;         // Files being sent or written now.
    unsigned stored;       // Files written since the app started.
    unsigned long lastAt;  // When a command last came (GetTickCount()).
    char last[64];         // The file written last (its name).
};
void FtpNow(FtpActivity& now);
}
