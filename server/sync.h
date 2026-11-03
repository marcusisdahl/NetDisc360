// SPDX-License-Identifier: MIT
#pragma once
// Sync between computers: every NetDisc360 (the PC at home, a laptop, any
// other) keeps its own settings and copies what is worth sharing through
// NetDisc360's sync Worker on Cloudflare (sync/), which keeps it in R2:
//
//   files/<kind>/<path>     backups that never change once made, both ways:
//                           game saves (Saves\), profiles (Backups\Profiles\)
//   state/<device>/<name>   each device's own documents (its plays.ini),
//                           which the others read
//
// A backup this computer deleted (an old one it no longer keeps) isn't
// fetched again: Sync\<kind>.seen lists every file already synced here.
// Nothing here deletes anything in the cloud or on this computer.
#include <string>
#include <vector>

namespace cloudsync {

struct Device { std::string id, name; long long added; };

// A folder of backups to sync: files directly in folder (depth 0) or in its
// folders (depth 1, like Backups\Profiles\<XUID>\). Only .zip and .ini files
// with names the Worker takes are synced.
struct Kind { std::string name, folder; int depth; };

struct Report {
    int sent, got, failed;
    std::vector<std::string> notes;  // What went wrong, for the log.
    Report() : sent(0), got(0), failed(0) {}
};

// Connects this computer with a key: the owner key (the Worker's OWNER_KEY)
// or another device's. The device's own key and id come back.
bool Connect(const std::string& url, const std::string& key, const std::string& name,
             std::string& id, std::string& deviceKey, std::string& error);
// Who key is: its device's id and name.
bool Hello(const std::string& url, const std::string& key, std::string& id, std::string& name, std::string& error);
bool Devices(const std::string& url, const std::string& key, std::vector<Device>& out, std::string& error);
bool Remove(const std::string& url, const std::string& key, const std::string& id, std::string& error);

// One pass over a kind: sends this computer's backups the cloud doesn't
// have, and fetches the ones this computer never had. seenFile: the list of
// files already synced (made if need be).
void Files(const std::string& url, const std::string& key, const Kind& kind, const std::string& seenFile, Report& report);

// Sends this device's document name (its bytes) unless it is what was sent
// last (sentSha, updated).
bool PutState(const std::string& url, const std::string& key, const std::string& self, const std::string& name,
              const std::string& data, std::string& sentSha, std::string& error);
// The other devices' documents called name, by device id ("" where a device
// has none): fetched only when changed since what is in folder\<id>\<name>,
// which keeps them.
void GetStates(const std::string& url, const std::string& key, const std::string& self, const std::string& name,
               const std::string& folder, Report& report);

// Whether name is one the Worker takes for a file (letters, digits, . _ -).
bool GoodName(const std::string& name);
// The address as the Worker's base: no trailing slash.
std::string Base(const std::string& url);
}
