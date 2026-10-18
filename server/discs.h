// SPDX-License-Identifier: MIT
#pragma once
// What the server lists as discs: ISO files, Xbox 360 games extracted to a
// folder (default.xex at the top), and Games on Demand packages. The NetISO
// plugin reads a disc only as sectors, so an extracted folder or a GOD
// package is shown as the disc image it came from, put together while it is
// read. Nothing is written next to the games.
#include "../shared/platform.h"
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace nia {
enum DiscKind { DiscIso, DiscGod, DiscFolder };
// A content package on an install disc: its path under Content
// (0000000000000000\<Title ID>\<type>\<name>, as on the disc) and size.
struct Package { std::string path; U64 size; };
// name is what the server lists. For folders and GOD packages it is the
// game's name with ".iso" added, so NetISO Sync names the console's game
// folder from it as for an ISO.
// titleId and disc come from the game's default.xex (0: not known yet, or not a game).
// packages are those the disc carries for its own game, under content at its
// top; installDisc is the listed name of the disc whose packages this one's
// game needs installed (see LinkInstallDiscs). role and roleLabel are what
// the disc map says the disc is (see discmap.h; the label in UTF-8), empty
// if it has nothing.
struct IsoFile { std::string path, name; U64 size; long long modified; DiscKind kind; DWORD titleId; int disc;
                 std::vector<Package> packages; std::string installDisc, role, roleLabel; };

// The discs in folder and its subfolders, sorted by name, added to out.
void FindIsos(const std::string& folder, std::vector<IsoFile>& out);
// Gives a disc whose listed name is taken (ignoring case) a number: an ISO
// keeps its name over a folder or GOD package, otherwise the first one does.
void UniqueNames(std::vector<IsoFile>& discs);
// Reads each disc's Title ID, disc number and packages (remembered between calls).
void ReadTitles(std::vector<IsoFile>& discs);
// Sets installDisc on each disc of a game that carries no packages while
// another disc of it does: GTA V's play disc, Forza 4's first disc.
void LinkInstallDiscs(std::vector<IsoFile>& discs);
// The same game (Title ID and disc number) more than once: keeps the copy
// whose path is in chosen, else an ISO over a GOD package over a folder, and
// moves the others to dropped, with kept naming the one kept for each.
void DropDuplicates(std::vector<IsoFile>& discs, std::vector<IsoFile>& dropped, std::vector<IsoFile>& kept,
                    const std::set<std::string>& chosen);
// The bytes of the files in folder and its subfolders (an extracted game's size, for the dashboard).
U64 FolderBytes(const std::string& folder);

class DiscImage {
public:
    virtual ~DiscImage() {}
    virtual U64 Size() const = 0;
    // offset and length are inside Size().
    virtual bool Read(U64 offset, unsigned char* data, DWORD length) = 0;
    // What it is, for the log ("1234 files", "38 GOD parts").
    virtual std::string Description() const = 0;
};

class IsoReader {
public:
    bool Open(const IsoFile& disc);
    bool IsOpen() const { return image_.get()!=NULL; }
    void Close() { image_.reset(); }
    U64 Size() const { return image_.get() ? image_->Size() : 0; }
    bool Read(U64 offset, unsigned char* data, DWORD length) { return image_.get() && image_->Read(offset,data,length); }
    // Why Open failed, or what was opened.
    const std::string& Note() const { return note_; }
private:
    std::unique_ptr<DiscImage> image_;
    std::string note_;
};

// For the tests: the disc image of a folder or GOD package, or NULL with why in error.
DiscImage* OpenFolderImage(const std::string& folder, std::string& error);
DiscImage* OpenGodImage(const std::string& header, std::string& error);
}
