// SPDX-License-Identifier: MIT
#pragma once
// The disc map: what each disc of a game on more than one disc is, so the
// console can say which disc goes in, and the dashboard can show it. It is
// kept in discs.ini next to the settings file, one [Title ID] section per
// game:
//
//   [5454082B]
//   name=Red Dead Redemption: Game of the Year Edition
//   disc1=own Red Dead Redemption
//   disc2=own Undead Nightmare
//
// The server adds a section for each such game it finds, from its own list
// of known games or else from what the discs carry, at the end of the file.
// It never changes a section that is there, so what the user writes stays;
// a section deleted is added again.
#include "discs.h"
#include <map>
#include <string>
#include <vector>

namespace nia {
// What a disc can be:
//   install  installs content, then the game asks for the play disc (GTA V's disc 1)
//   play     the disc that is played, needing the install disc's content (GTA V's disc 2)
//   content  optional content to install; the game plays without it (Forza 4's car packs)
//   next     the game goes on from the disc before and asks for this one (Final Fantasy XIII)
//   own      a game of its own, which never asks for another disc (Undead Nightmare)
bool KnownRole(const std::string& role);

struct MapDisc { std::string role, label; };
struct MapGame { std::string name; std::map<int,MapDisc> discs; };

class DiscMap {
public:
    // Reads the map from path, again only when the file has changed ("": no
    // file, the map is kept while the server runs). problems gets the lines
    // it couldn't use, when the file has changed since the last call.
    void Load(const std::string& path, std::vector<std::string>& problems);
    // Adds each game found on two discs or more that the map doesn't have
    // yet, from the built-in list or else from what its discs carry, also
    // to the file. added gets one line per game for the log; error why the
    // file couldn't be written, if so.
    void Learn(const std::vector<IsoFile>& discs, std::vector<std::string>& added, std::string& error);
    // Sets role and label on each disc the map knows.
    void Apply(std::vector<IsoFile>& discs) const;
    bool Has(DWORD titleId) const { return games_.count(titleId)!=0; }
private:
    std::string path_, text_;
    std::map<DWORD,MapGame> games_;
};

// The built-in list's games, for the tests and the dashboard: Title ID, name
// and discs as discs.ini writes them ("own Red Dead Redemption;own Undead Nightmare").
struct KnownGame { DWORD titleId; const char* name; const char* discs; };
const KnownGame* KnownGames(int& count);
}
