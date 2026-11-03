// SPDX-License-Identifier: MIT
#pragma once
// Wake-on-LAN for the launcher: NetDisc360 writes this computer's network
// adapter (its MAC address, and where a wake packet reaches it) to
// Hdd1:\NetISO\NetDisc360\wake.ini on each console, and a launcher that finds
// the server asleep sends the magic packet there and waits for it
// (launcher/main.cpp, WaitForServer).
//
//   mac=AA:BB:CC:DD:EE:FF
//   address=192.168.1.20      this computer, which a wake packet goes to as well
//   broadcast=192.168.1.255   its network's broadcast address
#include "../shared/platform.h"
#include <string>

namespace wake {
// The adapter of this computer that reaches toward (IPv4, network byte order):
// its MAC address as "AA:BB:CC:DD:EE:FF", its address and its network's
// broadcast address. False if there's none with a MAC address (or none at all).
bool Adapter(DWORD toward, std::string& mac, DWORD& address, DWORD& broadcast);
// "aa-bb-cc-dd-ee-ff", "AABBCCDDEEFF" or "aa:bb:cc:dd:ee:ff" as
// "AA:BB:CC:DD:EE:FF"; "" if it isn't a MAC address.
std::string CleanMac(const std::string& text);
// The text of wake.ini.
std::string Ini(const std::string& mac, DWORD address, DWORD broadcast);
}
