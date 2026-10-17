// SPDX-License-Identifier: MIT
// The games page's data: the games in the folders, found as the server finds
// them (discs.cpp) on a thread of their own, and their covers.
//
// Covers come from the XboxUnity-Scraper archive of XboxUnity's covers on
// GitHub (the same covers Aurora downloads), or from XboxUnity itself for a
// title the archive lacks, and are kept in %LOCALAPPDATA%\NetDisc
// 360\Covers. They are only for this window: the Xbox gets its art from
// Aurora's own scan.
#ifdef _WIN32
#include "gui.h"
#include "../third_party/imgui-1.92/imgui.h"
#include <d3d11.h>
#include <shlobj.h>
#include <wincodec.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

namespace gui {
namespace {
const char ArchiveUrl[] = "https://raw.githubusercontent.com/UncreativeXenon/XboxUnity-Scraper/master";
const char UnityUrl[] = "https://xboxunity.net/Resources/Lib";
const float FrontShare = 0.47f;   // Of a full box scan (back, spine, front), the front.
const UINT CoverHeight = 512;     // Covers are kept at most this tall.
const DWORD MissRetryDays = 7;    // A title without a cover is asked about again after this.
enum { Asked, Ready, Missing };

HWND Window;
UINT LibraryMessage, CoverMessage;
std::mutex Lock;
std::condition_variable Wake;
std::vector<std::wstring> Folders;  // What the scan thread scans, under Lock.
bool Stopping;
std::atomic<bool> ScanWanted(false), Busy(false), Downloads(true);
std::deque<DWORD> CoverQueue;      // Under Lock.
std::thread ScanThread, CoverThread;

struct CoverEntry { int state; ID3D11ShaderResourceView* view; float width, height; };
std::map<DWORD, CoverEntry> Covers;  // The window's thread only.
ID3D11SamplerState* Smooth;

struct PostedCover { DWORD titleId; UINT width, height; std::vector<BYTE> pixels; };

// The games -------------------------------------------------------------------

bool Within(const std::string& path, const std::string& folder) {
    return path.size() > folder.size() && !_strnicmp(path.c_str(), folder.c_str(), folder.size()) &&
           (path[folder.size()] == '\\' || folder.back() == '\\');
}

// Where a disc is, as one would remove it: a GOD package by its Title ID folder.
std::string Location(const nia::IsoFile& disc) {
    if (disc.kind != nia::DiscGod) return disc.path;
    std::string path = disc.path;
    for (int i = 0; i < 2; ++i) {
        size_t at = path.find_last_of('\\');
        if (at != std::string::npos) path.erase(at);
    }
    return path;
}

// The bytes in a folder and everything in it.
unsigned long long FolderBytes(const std::wstring& folder, int depth) {
    unsigned long long total = 0;
    WIN32_FIND_DATAW found;
    HANDLE find = FindFirstFileExW((folder + L"\\*").c_str(), FindExInfoBasic, &found, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            total += ((unsigned long long)found.nFileSizeHigh << 32) | found.nFileSizeLow;
        else if (wcscmp(found.cFileName, L".") && wcscmp(found.cFileName, L"..") &&
                 !(found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && depth < 32)
            total += FolderBytes(folder + L"\\" + found.cFileName, depth + 1);
    } while (FindNextFileW(find, &found));
    FindClose(find);
    return total;
}

// What an extracted game or a GOD package takes on the disk (the server only
// works it out when it builds the disc), kept until the game changes.
std::map<std::string, unsigned long long> Sizes;  // The scan thread only.

unsigned long long DiscBytes(const nia::IsoFile& disc) {
    if (disc.kind == nia::DiscIso) return disc.size;
    char stamp[32];
    _snprintf_s(stamp, sizeof(stamp), _TRUNCATE, "|%lld", disc.modified);
    std::string key = disc.path + stamp;
    std::map<std::string, unsigned long long>::iterator known = Sizes.find(key);
    if (known != Sizes.end()) return known->second;
    std::wstring path = Wide(AnsiToUtf8(disc.path));
    unsigned long long bytes = 0;
    if (disc.kind == nia::DiscFolder) {
        bytes = FolderBytes(path, 0);
    } else {
        WIN32_FILE_ATTRIBUTE_DATA header;
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &header))
            bytes = ((unsigned long long)header.nFileSizeHigh << 32) | header.nFileSizeLow;
        bytes += FolderBytes(path + L".data", 0);
    }
    Sizes[key] = bytes;
    return bytes;
}

Library* Scan(const std::vector<std::wstring>& folders, std::string& last) {
    std::vector<std::string> roots;
    std::vector<nia::IsoFile> isos;
    for (const std::wstring& folder : folders) {
        std::string root = Utf8ToAnsi(Utf8(folder));
        if (root.size() > 3 && root.back() == '\\') root.pop_back();
        roots.push_back(root);
        nia::FindIsos(root, isos);
    }
    std::string fingerprint;
    for (const std::string& root : roots) fingerprint += root + "\n";
    for (const nia::IsoFile& iso : isos) {
        char line[64];
        _snprintf_s(line, sizeof(line), _TRUNCATE, "|%llu|%lld\n", (unsigned long long)iso.size, iso.modified);
        fingerprint += iso.path + line;
    }
    if (fingerprint == last) return NULL;
    last = fingerprint;
    nia::ReadTitles(isos);
    std::vector<nia::IsoFile> dropped, kept;
    nia::DropDuplicates(isos, dropped, kept);
    nia::UniqueNames(isos);

    Library* library = new Library;
    library->done = true;
    library->perFolder.assign(roots.size(), 0);
    for (const nia::IsoFile& iso : isos) {
        Game game;
        game.name = AnsiToUtf8(iso.name.size() > 4 ? iso.name.substr(0, iso.name.size() - 4) : iso.name);
        game.path = AnsiToUtf8(Location(iso));
        game.size = DiscBytes(iso);
        game.kind = iso.kind;
        game.titleId = iso.titleId;
        game.disc = iso.disc;
        library->games.push_back(game);
        for (size_t i = 0; i < roots.size(); ++i)
            if (Within(iso.path, roots[i])) { ++library->perFolder[i]; break; }
    }
    for (size_t i = 0; i < dropped.size(); ++i) {
        Duplicate d = {AnsiToUtf8(Location(dropped[i])), AnsiToUtf8(Location(kept[i]))};
        library->duplicates.push_back(d);
    }
    return library;
}

// Looks again every few seconds while the window shows, as the server does
// every second, and at once when asked to.
void ScanLoop() {
    const std::string unknown = "?";  // No scan to compare with.
    std::string last = unknown;
    std::unique_lock<std::mutex> hold(Lock);
    for (;;) {
        bool visible = IsWindowVisible(Window) && !IsIconic(Window);
        Wake.wait_for(hold, std::chrono::seconds(visible ? 4 : 60), [] { return Stopping || ScanWanted; });
        if (Stopping) return;
        if (!visible && !ScanWanted) continue;
        bool asked = ScanWanted;
        if (asked) last = unknown;
        ScanWanted = false;
        std::vector<std::wstring> folders = Folders;
        Busy = true;
        hold.unlock();
        Library* library = Scan(folders, last);
        if (library && !PostMessageW(Window, LibraryMessage, 0, (LPARAM)library)) delete library;
        hold.lock();
        Busy = false;
        if (!library && asked) PostMessageW(Window, LibraryMessage, 0, 0);  // Done, for the page's "scanning".
    }
}

// Covers ------------------------------------------------------------------------

std::wstring CoverFolder() {
    PWSTR local = NULL;
    std::wstring folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &local))) folder = local;
    CoTaskMemFree(local);
    if (folder.empty()) return L"";
    std::wstring old = folder + L"\\NetISO for Aurora\\Covers";  // Kept there before the rename.
    folder += L"\\NetDisc 360";
    CreateDirectoryW(folder.c_str(), NULL);
    folder += L"\\Covers";
    if (GetFileAttributesW(folder.c_str()) == INVALID_FILE_ATTRIBUTES) MoveFileW(old.c_str(), folder.c_str());
    CreateDirectoryW(folder.c_str(), NULL);
    return folder;
}

std::wstring TitleText(DWORD titleId) {
    wchar_t text[12];
    swprintf(text, 12, L"%08X", (unsigned)titleId);
    return text;
}

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// The body of a GET, if it answered 200.
bool Get(const std::string& url, std::string& body) {
    body.clear();
    std::wstring wide = Wide(url);
    URL_COMPONENTS parts = {sizeof(parts)};
    wchar_t host[256], path[2048];
    parts.lpszHostName = host;
    parts.dwHostNameLength = ARRAYSIZE(host);
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = ARRAYSIZE(path);
    parts.dwExtraInfoLength = DWORD(-1);  // Points into wide.
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts)) return false;
    std::wstring target = std::wstring(path, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo) target.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    // Automatic proxy needs Windows 8.1; before, the proxy set with netsh.
    HINTERNET session = WinHttpOpen(L"NetISO-for-Aurora", 4 /* WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY */, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) session = WinHttpOpen(L"NetISO-for-Aurora", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 10000, 10000, 15000, 30000);
    bool ok = false;
    HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", target.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                        parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                                   : NULL;
    if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, NULL)) {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                            WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            char part[16384];
            DWORD got = 0;
            ok = true;
            while (WinHttpReadData(request, part, sizeof(part), &got) && got) {
                body.append(part, got);
                if (body.size() > 16 * 1024 * 1024) { ok = false; break; }
            }
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ok && !body.empty();
}

// The value of "key":"value" (or null) in one JSON object's text.
std::string Field(const std::string& object, const char* key) {
    std::string name = std::string("\"") + key + "\"";
    size_t at = object.find(name);
    if (at == std::string::npos) return "";
    at = object.find(':', at + name.size());
    if (at == std::string::npos) return "";
    at = object.find_first_not_of(" \t\r\n", at + 1);
    if (at == std::string::npos || object[at] != '"') return "";
    size_t end = object.find('"', at + 1);
    return end == std::string::npos ? "" : object.substr(at + 1, end - at - 1);
}

// The covers listed in CoverInfo.php's (or the archive's metadata.json's)
// answer, best first: official ones, then by rating.
std::vector<std::string> CoverIds(const std::string& json) {
    struct Entry { std::string id; bool official; int rating; };
    std::vector<Entry> entries;
    size_t at = json.find("\"Covers\"");
    while (at != std::string::npos && (at = json.find('{', at)) != std::string::npos) {
        size_t end = json.find('}', at);
        if (end == std::string::npos) break;
        std::string object = json.substr(at, end - at + 1);
        Entry e = {Field(object, "CoverID"), Field(object, "Official") == "1", atoi(Field(object, "Rating").c_str())};
        if (!e.id.empty() && e.id.find_first_not_of("0123456789") == std::string::npos) entries.push_back(e);
        at = end + 1;
    }
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return a.official != b.official ? a.official : a.rating > b.rating;
    });
    std::vector<std::string> ids;
    for (const Entry& e : entries) ids.push_back(e.id);
    return ids;
}

bool IsImage(const std::string& data) {
    return data.size() > 8 && ((BYTE(data[0]) == 0xFF && BYTE(data[1]) == 0xD8) || !data.compare(0, 4, "\x89PNG"));
}

bool Download(DWORD titleId, std::string& image) {
    char id[12];
    _snprintf_s(id, sizeof(id), _TRUNCATE, "%08X", (unsigned)titleId);
    const char* custom = getenv("NETDISC_COVERS_URL");  // For the tests: an archive elsewhere.
    std::string archive = custom && *custom ? custom : ArchiveUrl, json;
    if (Get(archive + "/Covers/" + id + "/metadata.json", json))
        for (const std::string& cover : CoverIds(json))
            if (Get(archive + "/Covers/" + id + "/Large/" + cover + ".png", image) && IsImage(image)) return true;
    if (custom && *custom) return false;
    if (Get(std::string(UnityUrl) + "/CoverInfo.php?titleid=" + id, json))
        for (const std::string& cover : CoverIds(json))
            if (Get(std::string(UnityUrl) + "/Cover.php?cid=" + cover + "&size=large", image) && IsImage(image)) return true;
    return false;
}

bool WriteWhole(const std::wstring& path, const void* data, size_t size) {
    std::wstring part = path + L".part";
    HANDLE f = CreateFileW(part.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(f, data, DWORD(size), &written, NULL) && written == size;
    CloseHandle(f);
    return ok && MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

bool RecentlyMissed(const std::wstring& marker) {
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExW(marker.c_str(), GetFileExInfoStandard, &info)) return false;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULONGLONG a = (ULONGLONG(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
    ULONGLONG b = (ULONGLONG(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    return b < a + ULONGLONG(MissRetryDays) * 24 * 3600 * 10000000ULL;
}

// The front of the cover, at most CoverHeight tall, as RGBA.
bool Decode(IWICImagingFactory* wic, const std::wstring& file, PostedCover& out) {
    IWICBitmapDecoder* decoder = NULL;
    IWICBitmapFrameDecode* frame = NULL;
    IWICBitmapClipper* clipper = NULL;
    IWICBitmapScaler* scaler = NULL;
    IWICFormatConverter* converter = NULL;
    bool ok = false;
    UINT w = 0, h = 0;
    if (SUCCEEDED(wic->CreateDecoderFromFilename(file.c_str(), NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&w, &h)) && w && h) {
        IWICBitmapSource* source = frame;
        if (w > h && SUCCEEDED(wic->CreateBitmapClipper(&clipper))) {
            WICRect front = {INT(w - UINT(w * FrontShare + 0.5f)), 0, INT(w * FrontShare + 0.5f), INT(h)};
            if (SUCCEEDED(clipper->Initialize(source, &front))) {
                source = clipper;
                w = front.Width;
            }
        }
        if (h > CoverHeight && SUCCEEDED(wic->CreateBitmapScaler(&scaler))) {
            UINT sw = std::max(1u, UINT(double(w) * CoverHeight / h + 0.5)), sh = CoverHeight;
            if (SUCCEEDED(scaler->Initialize(source, sw, sh, WICBitmapInterpolationModeFant))) {
                source = scaler;
                w = sw;
                h = sh;
            }
        }
        if (SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
            SUCCEEDED(converter->Initialize(source, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom))) {
            out.width = w;
            out.height = h;
            out.pixels.resize(size_t(w) * h * 4);
            ok = SUCCEEDED(converter->CopyPixels(NULL, w * 4, UINT(out.pixels.size()), out.pixels.data()));
        }
    }
    if (converter) converter->Release();
    if (scaler) scaler->Release();
    if (clipper) clipper->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    return ok;
}

void CoverLoop() {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IWICImagingFactory* wic = NULL;
    CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    std::wstring folder = CoverFolder();
    for (;;) {
        DWORD titleId;
        {
            std::unique_lock<std::mutex> hold(Lock);
            Wake.wait(hold, [] { return Stopping || !CoverQueue.empty(); });
            if (Stopping) break;
            titleId = CoverQueue.front();
            CoverQueue.pop_front();
        }
        PostedCover* cover = new PostedCover;
        cover->titleId = titleId;
        cover->width = cover->height = 0;
        std::wstring base = folder + L"\\" + TitleText(titleId), file;
        if (Exists(base + L".jpg")) file = base + L".jpg";
        else if (Exists(base + L".png")) file = base + L".png";
        else if (!folder.empty() && Downloads && !RecentlyMissed(base + L".none")) {
            std::string image;
            if (Download(titleId, image)) {
                file = base + (BYTE(image[0]) == 0xFF ? L".jpg" : L".png");
                if (!WriteWhole(file, image.data(), image.size())) file.clear();
                DeleteFileW((base + L".none").c_str());
            } else {
                WriteWhole(base + L".none", "", 0);
            }
        }
        if (!file.empty() && wic && !Decode(wic, file, *cover)) {
            DeleteFileW(file.c_str());  // A broken download: fetched again next time.
            cover->pixels.clear();
        }
        if (!PostMessageW(Window, CoverMessage, 0, (LPARAM)cover)) delete cover;
    }
    if (wic) wic->Release();
    CoUninitialize();
}

void UseSmooth(const ImDrawList*, const ImDrawCmd*) {
    if (Smooth) Context->PSSetSamplers(0, 1, &Smooth);
}
}  // namespace

void StartLibrary(HWND window, UINT libraryMessage, UINT coverMessage) {
    Window = window;
    LibraryMessage = libraryMessage;
    CoverMessage = coverMessage;
    Folders = Saved.folders;
    Downloads = Saved.covers;
    ScanWanted = true;
    ScanThread = std::thread(ScanLoop);
    CoverThread = std::thread(CoverLoop);
}

void StopLibrary() {
    {
        std::lock_guard<std::mutex> hold(Lock);
        Stopping = true;
    }
    Wake.notify_all();
    if (ScanThread.joinable()) ScanThread.join();
    if (CoverThread.joinable()) CoverThread.join();
    ForgetCovers(false);
    if (Smooth) Smooth->Release();
    Smooth = NULL;
}

void Rescan() {
    {
        std::lock_guard<std::mutex> hold(Lock);
        Folders = Saved.folders;
        ScanWanted = true;
    }
    Downloads = Saved.covers;
    Wake.notify_all();
}

bool Scanning() { return Busy || ScanWanted; }

void TakeLibrary(void* posted) {
    Library* library = (Library*)posted;
    WantFrames(2);
    if (!library) return;
    Games = *library;
    delete library;
    WantFrames(2);
}

unsigned long long Cover(DWORD titleId, float& width, float& height) {
    if (!titleId) return 0;
    std::map<DWORD, CoverEntry>::iterator found = Covers.find(titleId);
    if (found == Covers.end()) {
        CoverEntry entry = {Asked, NULL, 0, 0};
        Covers[titleId] = entry;
        {
            std::lock_guard<std::mutex> hold(Lock);
            CoverQueue.push_back(titleId);
        }
        Wake.notify_all();
        return 0;
    }
    if (found->second.state == Missing) width = -1;
    if (found->second.state != Ready) return 0;
    width = found->second.width;
    height = found->second.height;
    return (unsigned long long)(size_t)found->second.view;
}

void TakeCover(void* posted) {
    PostedCover* cover = (PostedCover*)posted;
    std::map<DWORD, CoverEntry>::iterator found = Covers.find(cover->titleId);
    if (found != Covers.end() && found->second.state == Asked) {
        CoverEntry& entry = found->second;
        entry.state = Missing;
        if (!cover->pixels.empty() && Device) {
            // With mipmaps, so it stays smooth drawn small.
            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = cover->width;
            desc.Height = cover->height;
            desc.MipLevels = 0;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            ID3D11Texture2D* texture = NULL;
            if (SUCCEEDED(Device->CreateTexture2D(&desc, NULL, &texture))) {
                Context->UpdateSubresource(texture, 0, NULL, cover->pixels.data(), cover->width * 4, 0);
                if (SUCCEEDED(Device->CreateShaderResourceView(texture, NULL, &entry.view))) {
                    Context->GenerateMips(entry.view);
                    entry.state = Ready;
                    entry.width = float(cover->width);
                    entry.height = float(cover->height);
                }
                texture->Release();
            }
        }
    }
    delete cover;
    WantFrames(2);
}

void ForgetCovers(bool deleteFiles) {
    for (auto& c : Covers)
        if (c.second.view) c.second.view->Release();
    Covers.clear();
    Downloads = Saved.covers;
    if (!deleteFiles) return;
    std::wstring folder = CoverFolder();
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW((folder + L"\\*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((folder + L"\\" + found.cFileName).c_str());
    } while (FindNextFileW(search, &found));
    FindClose(search);
}

// Covers drawn between these two use a sampler with mipmaps.
void SmoothImages(ImDrawList* draw, bool on) {
    if (!Smooth && Device) {
        D3D11_SAMPLER_DESC desc = {};
        desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
        desc.MaxLOD = D3D11_FLOAT32_MAX;
        Device->CreateSamplerState(&desc, &Smooth);
    }
    draw->AddCallback(on ? UseSmooth : ImDrawCallback_ResetRenderState, NULL);
}
}
#endif
