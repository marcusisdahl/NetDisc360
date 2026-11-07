// SPDX-License-Identifier: MIT
#pragma once
// The Health page's checks of a console (see HealthCheck in main.cpp): the
// parts that only read or change text and bytes, so the tests can run them
// without a console.
#include "../shared/platform.h"
#include <string>
#include <vector>

namespace health {

// Where NetDisc360.xex must load: a system DLL loads only at its own base,
// and 0x91380000 (where it was linked before) is aac.xex's, which some
// consoles load before the plugins, so there it never loaded at all.
const DWORD PluginBase = 0x91C00000;

// The address an XEX loads at: its image base header (0x10201), else the
// security info's load address. 0 if it isn't an XEX2 file.
DWORD XexBase(const unsigned char* data, size_t size);

// One module xbdm's "modules" lists: a running XEX or DLL and where it is.
struct Module { std::string name; DWORD base, size; };
// The modules in xbdm's answer to "modules" (lines like name="xam.xex"
// base=0x815f0000 size=0x00dd0000 ...). False if it lists none.
bool ParseModules(const std::string& answer, std::vector<Module>& out);

// launch.ini (DashLaunch's) with pluginN = path in its [Plugins] section:
// that slot's line changed, or a new one after the section's last plugin
// (the section is added when there is none). The file's line ends are kept.
std::string SetPlugin(const std::string& ini, int slot, const std::string& path);
// launch.ini with slot's pluginN line commented out (; in front), as
// DashLaunch's own example lines are, so it's easy to put back.
std::string CommentPlugin(const std::string& ini, int slot);

// NetDisc360.ini with server= naming address: the first server= line
// changed (a :port after the address stays), or one added at the top.
std::string SetServer(const std::string& ini, const std::string& address);

}
