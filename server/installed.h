// SPDX-License-Identifier: MIT
#pragma once
// The games installed on a console, found over Aurora's FTP server, so a game
// from the server that is installed there gets no tile: Aurora would list it
// twice. Extracted games are a default.xex in a folder, or in a folder in it
// ("Halo 3\Disc 1"), under the folders given; Games on Demand are in each of
// their drives' Content\0000000000000000\<Title ID>\00007000.
#include "ftp.h"
#include <map>
#include <string>
#include <vector>

namespace nia {
// path is the default.xex, or the GOD package's header file, as an FTP path.
struct InstalledGame { DWORD titleId; int disc; std::string path; bool god; };

// One for each console: what it read stays known until the file changes (by
// the size and time its folder's listing gives), so a look again reads only
// the folders.
class InstalledGames {
public:
    // Looks in folders (FTP paths, such as /Hdd1/Games), leaving out skip and
    // what is in it (the tiles' own folder). False, with why in error, when a
    // folder that is there couldn't be read; found is then what the latest
    // look that worked found.
    bool Find(FtpClient& ftp, const std::vector<std::string>& folders, const std::string& skip,
              std::vector<InstalledGame>& found, std::string& error);
private:
    struct Seen { U64 size; std::string stamp; DWORD titleId; int disc; };
    bool Walk(FtpClient& ftp, const std::string& folder, int depth, const std::string& skip, std::vector<InstalledGame>& out, std::string& error);
    bool Gods(FtpClient& ftp, const std::string& drive, std::vector<InstalledGame>& out, std::string& error);
    bool Ids(FtpClient& ftp, const std::string& path, const FtpEntry& file, bool god, DWORD& titleId, int& disc);
    std::map<std::string,Seen> seen_;
    std::vector<InstalledGame> last_;
};

// The folders as the setting lists them: separated by semicolons or line
// breaks, each an FTP path without a slash at its end.
std::vector<std::string> InstalledFolders(const std::string& setting);
}
