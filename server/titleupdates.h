// SPDX-License-Identifier: MIT
#pragma once
// Title updates from XboxUnity, the site Aurora downloads them from.
//
// XboxUnity's TitleUpdateInfo.php?titleid=<Title ID> lists a game's updates,
// grouped by the Media ID of the disc each one is for. Most games give
// {"Type":1,"MediaIDS":[{"MediaID":"...","Updates":[...]}]}; some give
// {"Type":2,"Updates":[...]} with a MediaID on each update. Each update has
// TitleUpdateID, Version, Name, Size, hash (SHA-1 of the file), UploadDate
// and BaseVersion, mostly as strings. TitleUpdate.php?tuid=<TitleUpdateID>
// is the file, named by its Content-Disposition header. That name says where
// it goes on the console, as Aurora puts it there: a lowercase tu... name
// under Content\0000000000000000\<Title ID>\000B0000, an uppercase TU_...
// name in Hdd1:\Cache. The console uses an update whose Title ID and Media
// ID match the disc.
//
// The server asks XboxUnity itself (the browser can't: XboxUnity doesn't
// allow other sites' pages to read its answers), over HTTPS with Windows'
// HTTP client, or curl on Linux; a base address of http://... (the tests')
// is asked over a plain socket.
#include "../shared/platform.h"
#include <string>
#include <vector>

namespace tu {

struct Offer {
    std::string id;         // TitleUpdateID, for TitleUpdate.php.
    std::string mediaId;    // 8 hex digits, upper case.
    std::string name;       // "Title Update 5", as XboxUnity names it.
    std::string hash;       // SHA-1 of the file, 40 hex digits in lower case (empty if not given).
    std::string uploaded;   // UploadDate, as given.
    std::string baseVersion;
    int version;            // XboxUnity's number (5 for Title Update 5).
    U64 size;               // In bytes, from XboxUnity's KB; 0 if not given.
    Offer() : version(0), size(0) {}
};

// XboxUnity's answer for a Title ID, every Media ID's updates; false with
// why in error if it isn't one (an empty list is fine: no updates).
bool ParseInfo(const std::string& json, std::vector<Offer>& out, std::string& error);
// Those for one disc (its Media ID), newest first.
std::vector<Offer> ForMedia(const std::vector<Offer>& all, DWORD mediaId);

// The addresses, from base (https://xboxunity.net/Resources/Lib).
std::string InfoUrl(const std::string& base, DWORD titleId);
std::string FileUrl(const std::string& base, const std::string& id);

// What went wrong with a Get, so what the user is told fits: Offline (the
// name didn't resolve, or nothing took the connection: this computer's
// network, most likely), Slow (no answer in time: the site may be down),
// ServerError (it answered with an error such as 500: it's down), NotThere
// (it answered 404 or 410: it has no such thing), Broken (an answer cut
// short, too large or not understood).
enum Failure { Fine, Offline, Slow, ServerError, NotThere, Broken };
// How long a look-up (a list, a name, an icon) may take before it counts as
// Slow: 30 s (--lookup-seconds for the tests); a download of an update file 10 min.
extern int LookupSeconds;
enum { DownloadSeconds = 600 };

// GET of at most limit bytes: the body, and the file name its
// Content-Disposition gives (empty if none). False with why in error, which
// calls the server site; failure, if given, says what kind of failure it was
// (Fine when it worked). seconds: the most it may take, 0 for LookupSeconds.
bool Get(const std::string& url, std::string& body, size_t limit, std::string& fileName, std::string& error,
         int seconds = 0, const std::string& site = "XboxUnity", Failure* failure = NULL);

// Where a title update file goes on the console (an FTP path), from its
// name as XboxUnity gives it; name is first made one FATX takes (see FatxName).
std::string ConsolePath(DWORD titleId, const std::string& name);
// The folders an update of titleId can be in: Content's 000B0000 and the Cache.
std::string ContentFolder(DWORD titleId);
extern const char CacheFolder[];
// name as a FATX file name: at most 42 characters, none of those FATX refuses.
std::string FatxName(const std::string& name);

// What an Xbox 360 package's header says (CON, LIVE or PIRS: a title
// update, DLC, a Games on Demand header, an install package...), from its
// first 0x971 bytes or more.
struct PackageHeader {
    char magic[5];
    DWORD contentType, titleId, mediaId, version, baseVersion;
    std::string displayName;  // UTF-8.
    std::string contentId;    // The header's SHA-1 at 0x32C, in lower-case hex: XboxUnity's "hash".
};
enum { PackageHeaderBytes = 0x971 };
bool ReadPackageHeader(const unsigned char* data, size_t size, PackageHeader& out);
// Content types (the header's at 0x344).
enum : DWORD { TypeSavedGame = 0x1, TypeMarketplace = 0x2, TypePublisher = 0x3, TypeXbox360Title = 0x1000,
               TypeIptv = 0x2000, TypeInstalledGame = 0x4000, TypeXboxOriginal = 0x5000, TypeGamesOnDemand = 0x7000,
               TypeAvatarItem = 0x9000, TypeProfile = 0x10000, TypeGamerPicture = 0x20000, TypeTheme = 0x30000,
               TypeCache = 0x40000, TypeStorageDownload = 0x50000, TypeXboxSavedGame = 0x60000,
               TypeXboxDownload = 0x70000, TypeGameDemo = 0x80000, TypeVideo = 0x90000, TypeGameTitle = 0xA0000,
               TypeInstaller = 0xB0000, TypeGameTrailer = 0xC0000, TypeArcadeTitle = 0xD0000, TypeXna = 0xE0000,
               TypeLicense = 0xF0000, TypeMovie = 0x100000, TypeTv = 0x200000, TypeMusicVideo = 0x300000,
               TypeGameVideo = 0x400000, TypePodcast = 0x500000, TypeViralVideo = 0x600000,
               TypeCommunityGame = 0x2000000 };

// SHA-1 of data, as 40 lower-case hex digits.
std::string Sha1Hex(const void* data, size_t size);
// A file of bytes is the size XboxUnity lists (whole KB, rounded up).
inline bool SameSize(U64 bytes, U64 listed) { return (bytes+1023)/1024*1024==listed; }
}
