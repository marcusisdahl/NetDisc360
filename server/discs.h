// SPDX-License-Identifier: MIT
#pragma once
// What the server lists as discs: ISO files, Xbox 360 games extracted to a
// folder (default.xex at the top), and Games on Demand packages. The NetISO
// plugin reads a disc only as sectors, so an extracted folder or a GOD
// package is shown as the disc image it came from, put together while it is
// read. Nothing is written next to the games.
#include "../shared/platform.h"
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace nia {
enum DiscKind { DiscIso, DiscGod, DiscFolder };
// A content package on an install disc: its path under Content
// (0000000000000000\<Title ID>\<type>\<name>, as on the disc) and size.
struct Package { std::string path; U64 size; };
// A file on a disc image: its path from the disc's top (folders split by \),
// first byte in the image, and size.
struct DiscFile { std::string path; U64 at, size; };
// name is what the server lists. For folders and GOD packages it is the
// game's name with ".iso" added, so NetISO Sync names the console's game
// folder from it as for an ISO.
// titleId, mediaId and disc come from the game's default.xex (0: not known yet, or not a game).
// packages are those the disc carries for its own game, under content at its
// top; installDisc is the listed name of the disc whose packages this one's
// game needs installed (see LinkInstallDiscs). role and roleLabel are what
// the disc map says the disc is (see discmap.h; the label in UTF-8), empty
// if it has nothing.
struct IsoFile { std::string path, name; U64 size; long long modified; DiscKind kind; DWORD titleId, mediaId; int disc;
                 std::vector<Package> packages; std::string installDisc, role, roleLabel; };

// The discs in folder and its subfolders, sorted by name, added to out;
// false if folder itself couldn't be read (a drive or share that isn't there).
bool FindIsos(const std::string& folder, std::vector<IsoFile>& out);
// Makes each listed name one the console can take (see ServedName), and
// gives a disc whose name is taken (ignoring case) a number: an ISO keeps its
// name over a folder or GOD package, otherwise the first one does.
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
    friend class GodPackage;
public:
    bool Open(const IsoFile& disc);
    bool IsOpen() const { return image_.get()!=NULL; }
    void Close() { image_.reset(); }
    U64 Size() const { return image_.get() ? image_->Size() : 0; }
    bool Read(U64 offset, unsigned char* data, DWORD length) { return image_.get() && image_->Read(offset,data,length); }
    // Where the file at path (from the disc's top, folders split by \ or /)
    // is in the image: its first byte and size; false if it isn't there.
    bool Locate(const std::string& path, U64& at, U64& size);
    // Every file on the disc, a folder's files after it; folders (each a
    // path ending in \, so an empty one is kept) are in it too, size 0.
    // False if its tables can't be read.
    bool Files(std::vector<DiscFile>& out);
    // Why Open failed, or what was opened.
    const std::string& Note() const { return note_; }
private:
    std::unique_ptr<DiscImage> image_;
    std::string note_;
};

// A Games on Demand package of a disc, made as iso2god-rs makes one (MIT,
// third_party/iso2god): the disc's game partition, from its start to the end
// of its last file, in Data0000, Data0001, ... of 0xA1C4 4 KB blocks each,
// every file a master hash block first, then groups of a hash block and 0xCC
// data blocks; each hash block lists the SHA-1 of the blocks after it, the
// master one those of the file's hash blocks and then of the next file's
// master block, and the header (the empty LIVE package iso2god starts from,
// filled in) that of Data0000's master block. So the first bytes depend on
// the whole disc: Make reads it all once for the hashes, and then Part gives
// each file's bytes from the disc as they are sent, without writing any here.
class GodPackage {
public:
    // false, with why in error, if reader's disc isn't an Xbox 360 game
    // (default.xex at its top) or can't be read; progress(done, total) hears
    // as it reads, and stops it when it returns false (error is then empty).
    bool Make(IsoReader& reader, const std::string& title, const std::function<bool(U64, U64)>& progress, std::string& error);
    DWORD TitleId() const { return titleId_; }
    DWORD MediaId() const { return mediaId_; }
    // <Title ID>\00007000, where the package goes under Content\0000000000000000.
    std::string Folder() const;
    // The header's name (the Media ID), and the folder of the Data files is it with .data.
    std::string Name() const;
    int Parts() const { return int(masters_.size()); }
    U64 PartSize(int part) const;
    U64 Bytes() const;  // The Data files' and the header's.
    // length bytes of Data<part> from offset; after Make, with the reader it was made from still open.
    bool Part(int part, U64 offset, unsigned char* data, DWORD length);
    const std::vector<unsigned char>& Header() const { return header_; }
private:
    DWORD PartBlocks(int part) const;
    bool Group(int part, U64 group, unsigned char* hashes);
    IsoReader* reader_ = NULL;
    DWORD titleId_ = 0, mediaId_ = 0;
    U64 start_ = 0, blocks_ = 0;  // The partition's first byte in the image, and its blocks.
    std::vector<std::vector<unsigned char> > masters_;
    std::vector<unsigned char> header_, group_;
    long long groupAt_ = -1;  // Which group's hash block group_ is (part * 0x10000 + group).
};

// For the tests: the disc image of a folder or GOD package, or NULL with why in error.
DiscImage* OpenFolderImage(const std::string& folder, std::string& error);
DiscImage* OpenGodImage(const std::string& header, std::string& error);
}
