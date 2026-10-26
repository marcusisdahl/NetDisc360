// SPDX-License-Identifier: MIT
#pragma once
// Compressed games. A game converted once to a CSO file (the CISO format of
// maxcso and the PSP tools: the disc in blocks, each deflated on its own, and
// a table of where each block starts) is served like an ISO: the plugin only
// asks for byte ranges, and only the blocks a read touches are unpacked.
//
// Converting reads an ISO, or an ISO inside a .7z or .zip archive, and writes
// <name>.cso next to it. The space on a retail disc that no file uses (Xbox
// 360 discs fill it with random bytes, which don't compress) is written as
// zeros, as an extracted game or a Games on Demand package also leaves it
// out; a disc whose file table can't be read is kept whole. The original is
// never changed or deleted.
#include "discs.h"
#include <atomic>
#include <string>
#include <vector>

namespace nia {
bool IsCsoName(const std::string& name);
bool IsArchiveName(const std::string& name);  // .7z or .zip

// A CSO file's disc image, or NULL with why in error.
DiscImage* OpenCsoImage(const std::string& path, std::string& error);

// The ISOs in an archive: their names inside it (UTF-8, without folders)
// and sizes. False with why in error if it can't be read.
struct ArchiveDisc { std::string name; U64 size; };
bool ListArchive(const std::string& path, std::vector<ArchiveDisc>& discs, std::string& error);

// The .7z and .zip files in folder and its subfolders that hold an ISO, and
// where each ISO's CSO goes (next to the archive; native paths).
struct ArchiveFile { std::string path; U64 size; long long modified; std::vector<ArchiveDisc> discs; std::vector<std::string> outputs; std::string problem; };
void FindArchives(const std::string& folder, std::vector<ArchiveFile>& out);

// Where the CSO of an ISO goes: the same folder and name, ending in .cso.
// For an archive, discs holds what ListArchive found and index which disc;
// one ISO in it takes the archive's name, several their own.
std::string CsoPathFor(const std::string& iso);
std::string CsoPathFor(const std::string& archive, const std::vector<ArchiveDisc>& discs, size_t index);

enum CompressStep { StepTable, StepCompress, StepCheck };
struct CompressProgress {
    std::atomic<int> step;
    std::atomic<U64> done, total;  // Bytes of the disc image.
    CompressProgress() : step(StepTable), done(0), total(0) {}
};
struct CompressResult {
    U64 imageBytes;   // The disc image, as served.
    U64 keptBytes;    // What its files use (all of it when kept whole).
    U64 outputBytes;  // The CSO file.
    bool whole;       // The file table couldn't be read, so every byte was kept.
    CompressResult() : imageBytes(0), keptBytes(0), outputBytes(0), whole(false) {}
};
// Converts the ISO at source (entry < 0) or the entry'th ISO that
// ListArchive lists in the archive at source into a CSO file at output,
// through output.part, which is read back and checked before it is renamed.
// keepWhole: keep the unused space too. cancel (may be NULL) stops it.
bool CompressDisc(const std::string& source, int entry, const std::string& output, bool keepWhole,
                  CompressProgress& progress, const std::atomic<bool>* cancel, CompressResult& result, std::string& error);

// Free bytes on the drive that holds folder; 0 if not known.
U64 FreeBytes(const std::string& folder);
}
