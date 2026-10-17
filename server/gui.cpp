// SPDX-License-Identifier: MIT
// The window of netiso-server.exe on Windows. Started with no options (from
// Explorer, a shortcut, or at sign-in with --tray), the exe shows a window
// with the games it serves, the game folders, the Xbox's connection and the
// server's log, drawn with Dear ImGui over Direct3D 11 (gui_view.cpp). The
// settings are kept in %APPDATA%\NetISO for Aurora\netiso-server.ini (the
// password encrypted for this Windows user), and the server itself runs as a
// second copy of this exe with those settings as options, its output shown
// in the log. Minimizing or closing the window leaves the server running
// behind an icon in the notification area.
//
// Started with options, the exe is the command-line server as before; this
// file then only connects it to the console it was started from.
#ifdef _WIN32
#include "gui.h"
#include "../third_party/imgui-1.92/imgui.h"
#include "../third_party/imgui-1.92/backends/imgui_impl_dx11.h"
#include "../third_party/imgui-1.92/backends/imgui_impl_win32.h"
#include "../third_party/fonts/roboto_medium.h"
#include "../third_party/lucide/lucide_icons.h"
#include <d3d11.h>
#include <dwmapi.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincrypt.h>
#include <windowsx.h>
#include <stdio.h>
#include <string.h>
#include <functional>
#include <thread>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "imm32.lib")

IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace gui {
Config Saved;
Library Games;
ServerState Server;
std::string ThisPc;
float Scale = 1;
bool Dark;
ImFont *FontText, *FontBold, *FontMono, *FontIcons;
ID3D11Device* Device;
ID3D11DeviceContext* Context;

namespace {
const wchar_t AppName[] = L"NetISO for Aurora";
const wchar_t WindowClass[] = L"NetIsoAuroraServer";
const wchar_t RunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t RunValue[] = L"NetISO server for Aurora";
const wchar_t Section[] = L"Server";
const int IconId = 1;  // server.rc
const size_t LogLines = 5000;
enum { WM_TRAY = WM_APP + 1, WM_LOGLINE, WM_STOPPED, WM_SHOWMAIN, WM_LIBRARY, WM_COVER };
enum { IdMenuOpen = 200, IdMenuStart, IdMenuBoot, IdMenuQuit };

HWND Window;
HICON BigIcon, SmallIcon;
UINT TaskbarCreated;
NOTIFYICONDATAW Tray;
HANDLE Job, Child;
unsigned Generation;
IDXGISwapChain* SwapChain;
ID3D11RenderTargetView* Target;
int FramesWanted;
DWORD WakeAt;  // GetTickCount() of a frame asked for later; 0: none.
bool InFrame, Quitting;
std::vector<std::function<void()>> AfterFrame;

// Runs what would interrupt a frame (a dialog, closing the window) after it.
void Later(const std::function<void()>& work) {
    if (InFrame) AfterFrame.push_back(work);
    else work();
}

// Text --------------------------------------------------------------------------

std::string Narrow(const std::wstring& text, UINT page) {
    int n = WideCharToMultiByte(page, 0, text.data(), int(text.size()), NULL, 0, NULL, NULL);
    std::string out(n, '\0');
    if (n) WideCharToMultiByte(page, 0, text.data(), int(text.size()), &out[0], n, NULL, NULL);
    return out;
}

std::wstring Widen(const std::string& text, UINT page) {
    int n = MultiByteToWideChar(page, 0, text.data(), int(text.size()), NULL, 0);
    std::wstring out(n, L'\0');
    if (n) MultiByteToWideChar(page, 0, text.data(), int(text.size()), &out[0], n);
    return out;
}

// Paths and settings -----------------------------------------------------

std::wstring ExePath() {
    wchar_t path[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(NULL, path, ARRAYSIZE(path));
    return std::wstring(path, n);
}

std::wstring ExeFolder() {
    std::wstring path = ExePath();
    size_t at = path.find_last_of(L"\\/");
    return at == std::wstring::npos ? L"." : path.substr(0, at);
}

// The FTP password is kept encrypted for this Windows user (DPAPI).
std::wstring Protect(const std::wstring& text) {
    DATA_BLOB in = {DWORD(text.size() * sizeof(wchar_t)), (BYTE*)text.data()}, out = {0, NULL};
    if (!CryptProtectData(&in, L"NetISO for Aurora FTP password", NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out)) return L"";
    DWORD n = 0;
    std::wstring result;
    if (CryptBinaryToStringW(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &n)) {
        result.resize(n);
        CryptBinaryToStringW(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &result[0], &n);
        result.resize(n);
    }
    LocalFree(out.pbData);
    return result;
}

bool Unprotect(const std::wstring& text, std::wstring& password) {
    DWORD n = 0;
    if (text.empty() || !CryptStringToBinaryW(text.c_str(), 0, CRYPT_STRING_BASE64, NULL, &n, NULL, NULL)) return false;
    std::vector<BYTE> data(n);
    if (!CryptStringToBinaryW(text.c_str(), 0, CRYPT_STRING_BASE64, data.data(), &n, NULL, NULL)) return false;
    DATA_BLOB in = {n, data.data()}, out = {0, NULL};
    if (!CryptUnprotectData(&in, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    password.assign((const wchar_t*)out.pbData, out.cbData / sizeof(wchar_t));
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}

// Whether a folder looks like it holds games: an ISO, an extracted game's
// default.xex or a GOD package's 00007000 folder, within a few levels.
bool HasGames(const std::wstring& folder, int depth) {
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW((folder + L"\\*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return false;
    bool games = false;
    do {
        std::wstring name = found.cFileName;
        if (name == L"." || name == L"..") continue;
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            games = !_wcsicmp(name.c_str(), L"00007000") || (depth > 0 && HasGames(folder + L"\\" + name, depth - 1));
        else
            games = (name.size() > 4 && !_wcsicmp(name.c_str() + name.size() - 4, L".iso")) || !_wcsicmp(name.c_str(), L"default.xex");
    } while (!games && FindNextFileW(search, &found));
    FindClose(search);
    return games;
}

std::wstring ReadSetting(const std::wstring& ini, const wchar_t* key, const wchar_t* fallback) {
    std::vector<wchar_t> value(32768);
    GetPrivateProfileStringW(Section, key, fallback, value.data(), DWORD(value.size()), ini.c_str());
    return value.data();
}

bool ReadFlag(const std::wstring& ini, const wchar_t* key, bool fallback) {
    return GetPrivateProfileIntW(Section, key, fallback ? 1 : 0, ini.c_str()) != 0;
}

void WriteFlag(const std::wstring& ini, const wchar_t* key, bool on) {
    WritePrivateProfileStringW(Section, key, on ? L"1" : L"0", ini.c_str());
}

void LoadConfig() {
    std::wstring ini = IniPath();
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // First start: serve the folder the exe is in when it has games, so
        // the exe can simply be put in the folder with them. Run from the
        // Desktop or Downloads, it starts with an empty list instead.
        if (HasGames(ExeFolder(), 3)) Saved.folders.push_back(ExeFolder());
        return;
    }
    int count = GetPrivateProfileIntW(Section, L"folders", 0, ini.c_str());
    for (int i = 1; i <= count; ++i) {
        std::wstring folder = ReadSetting(ini, (L"folder" + std::to_wstring(i)).c_str(), L"");
        if (!folder.empty()) Saved.folders.push_back(folder);
    }
    Saved.xbox = ReadSetting(ini, L"xbox", L"");
    Saved.user = ReadSetting(ini, L"ftp_user", L"xboxftp");
    std::wstring password;
    if (Unprotect(ReadSetting(ini, L"ftp_password", L""), password)) Saved.password = password;
    Saved.options = ReadSetting(ini, L"options", L"");
    Saved.theme = GetPrivateProfileIntW(Section, L"theme", ThemeSystem, ini.c_str());
    if (Saved.theme < ThemeSystem || Saved.theme > ThemeDark) Saved.theme = ThemeSystem;
    Saved.covers = ReadFlag(ini, L"covers", true);
    Saved.listView = ReadFlag(ini, L"list_view", false);
    Saved.sortBySize = ReadFlag(ini, L"sort_by_size", false);
    Saved.showIso = ReadFlag(ini, L"show_iso", true);
    Saved.showGod = ReadFlag(ini, L"show_god", true);
    Saved.showFolder = ReadFlag(ini, L"show_folder", true);
    Saved.toldTray = ReadFlag(ini, L"told_tray", false);
}

// Start with Windows: a value under HKCU\...\Run that starts the exe in the tray.
std::wstring RunCommand() { return L"\"" + ExePath() + L"\" --tray"; }

std::wstring StartupEntry() {
    wchar_t value[MAX_PATH * 4];
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, RunKey, RunValue, RRF_RT_REG_SZ, NULL, value, &size) != ERROR_SUCCESS) return L"";
    return value;
}

// The PC's IPv4 addresses on networks with a gateway: what netiso.xex.txt needs.
std::string PcAddresses() {
    ULONG size = 16384;
    std::vector<BYTE> buffer(size);
    IP_ADAPTER_ADDRESSES* list = (IP_ADAPTER_ADDRESSES*)buffer.data();
    ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET, flags, NULL, list, &size) == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        list = (IP_ADAPTER_ADDRESSES*)buffer.data();
    }
    if (GetAdaptersAddresses(AF_INET, flags, NULL, list, &size) != NO_ERROR) return "";
    std::string result;
    for (IP_ADAPTER_ADDRESSES* a = list; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || !a->FirstGatewayAddress) continue;
        for (IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u; u = u->Next) {
            sockaddr_in* at = (sockaddr_in*)u->Address.lpSockaddr;
            const BYTE* b = (const BYTE*)&at->sin_addr;
            char text[20];
            _snprintf_s(text, sizeof(text), _TRUNCATE, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
            if (!result.empty()) result += ", ";
            result += text;
        }
    }
    return result;
}

// The server process ---------------------------------------------------------

// One argument for CommandLineToArgvW / the C runtime's parser.
std::wstring Quote(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') { ++slashes; continue; }
        out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L"\"";
}

std::wstring ServerCommandLine() {
    std::wstring command = Quote(ExePath());
    for (const std::wstring& folder : Saved.folders) {
        std::wstring f = folder;
        // "D:\" would lose its backslash to the option parser and mean the
        // current folder on D:, so a drive is passed as "D:\.".
        if (f.size() <= 3 && f.size() >= 2 && f[1] == L':') f = f.substr(0, 2) + L"\\.";
        command += L" " + Quote(f);
    }
    if (!Saved.xbox.empty()) command += L" --xbox " + Quote(Saved.xbox);
    command += L" --ftp-user " + Quote(Saved.user);  // The password goes in NETISO_FTP_PASSWORD.
    if (!Saved.options.empty()) command += L" " + Saved.options;
    return command;
}

void ReadOutput(HANDLE pipe, unsigned generation) {
    std::string pending;
    char part[4096];
    DWORD got;
    while (ReadFile(pipe, part, sizeof(part), &got, NULL) && got) {
        pending.append(part, got);
        size_t end;
        while ((end = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, end);
            pending.erase(0, end + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::string* text = new std::string(AnsiToUtf8(line));
            if (!PostMessageW(Window, WM_LOGLINE, generation, (LPARAM)text)) delete text;
        }
    }
    CloseHandle(pipe);
    PostMessageW(Window, WM_STOPPED, generation, 0);
}

void SetTrayTip() {
    std::wstring tip = std::wstring(L"NetISO for Aurora: ") + (Server.running ? L"running" : L"stopped");
    if (Server.running && Server.games >= 0) tip += L", " + std::to_wstring(Server.games) + (Server.games == 1 ? L" game" : L" games");
    wcsncpy_s(Tray.szTip, tip.c_str(), _TRUNCATE);
    Tray.uFlags = NIF_TIP | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &Tray);
}

void ServerChanged() {
    ThisPc = PcAddresses();
    SetTrayTip();
    WantFrames(2);
}

bool StartsWith(const std::string& text, const char* start) { return !text.compare(0, strlen(start), start); }

// The server's own lines (after its "hh:mm:ss " time) that say how things are.
void ReadLine(const std::string& line) {
    if (line.size() < 9 || line[2] != ':' || line[5] != ':') return;
    std::string time = line.substr(0, 5), text = line.substr(9);
    int count;
    size_t at;
    if (sscanf_s(text.c_str(), "%d game(s) in", &count) == 1 && text.find(" game(s) in ") != std::string::npos) {
        Server.games = count;
    } else if (StartsWith(text, "The Xbox is at ") && (at = text.find(';')) != std::string::npos) {
        Server.xbox = text.substr(15, at - 15);
    } else if (text.size() > 11 && text.compare(text.size() - 11, 11, " connected.") == 0 && text.find(' ') == text.size() - 11 &&
               !StartsWith(text, "127.")) {
        Server.xbox = text.substr(0, text.size() - 11);
    } else if ((at = text.find(": mounted ")) != std::string::npos && text.find(' ') == at + 1) {
        std::string name = text.substr(at + 10);
        size_t iso = name.rfind(".iso");
        Server.playing = iso == std::string::npos ? name : name.substr(0, iso);
    } else if ((at = text.find(": unmounting ")) != std::string::npos && text.find(' ') == at + 1) {
        Server.playing.clear();
    } else if (StartsWith(text, "Waiting for the Xbox's FTP server: ")) {
        Server.ftp = 1;
        Server.ftpNote = text.substr(35);
    } else if (StartsWith(text, "The Xbox's FTP server at ") && (at = text.find(" answers.")) != std::string::npos) {
        Server.ftp = 2;
        Server.ftpNote = text.substr(25, at - 25);
    } else if (StartsWith(text, "The Xbox is up to date: ")) {
        Server.update = time + "  " + text.substr(24);
        if (!Server.update.empty() && Server.update.back() == '.') Server.update.pop_back();
    } else if (StartsWith(text, "Updating the Xbox's game folders stopped: ") || StartsWith(text, "Couldn't list ")) {
        Server.update = time + "  " + text;
    }
}

void RestartServer() {
    bool was = Server.running;
    StopServer(false);
    if (was) Log("Settings changed; restarting the server.");
    StartServer();
}

// Fonts, style and drawing ------------------------------------------------------

std::wstring FontsFolder() {
    PWSTR fonts = NULL;
    std::wstring folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Fonts, 0, NULL, &fonts))) folder = fonts;
    CoTaskMemFree(fonts);
    return folder;
}

ImFont* SystemFont(const wchar_t* const* files, float size) {
    std::wstring folder = FontsFolder();
    for (; *files; ++files) {
        std::string path = Narrow(folder + L"\\" + *files, CP_UTF8);
        if (GetFileAttributesW((folder + L"\\" + *files).c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        if (ImFont* font = ImGui::GetIO().Fonts->AddFontFromFileTTF(path.c_str(), size)) return font;
    }
    return NULL;
}

void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    static const wchar_t* text[] = {L"segoeui.ttf", L"tahoma.ttf", NULL};
    static const wchar_t* bold[] = {L"seguisb.ttf", L"segoeuib.ttf", L"tahomabd.ttf", NULL};
    static const wchar_t* mono[] = {L"consola.ttf", L"cour.ttf", NULL};
    ImFontConfig embedded;
    embedded.FontDataOwnedByAtlas = false;
    FontText = SystemFont(text, 14);
    if (!FontText) FontText = io.Fonts->AddFontFromMemoryTTF((void*)RobotoMedium, sizeof(RobotoMedium), 14, &embedded);
    FontBold = SystemFont(bold, 14);
    if (!FontBold) FontBold = FontText;
    FontMono = SystemFont(mono, 13);
    if (!FontMono) FontMono = FontText;
    FontIcons = io.Fonts->AddFontFromMemoryTTF((void*)LucideIcons, sizeof(LucideIcons), 16, &embedded);
    io.FontDefault = FontText;
}

bool SystemIsDark() {
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                 RRF_RT_REG_DWORD, NULL, &light, &size);
    return light == 0;
}

void UpdateTheme() {
    Dark = Saved.theme == ThemeDark || (Saved.theme == ThemeSystem && SystemIsDark());
    BOOL dark = Dark;
    DwmSetWindowAttribute(Window, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    ApplyStyle();
    WantFrames(2);
}

void MakeTarget() {
    ID3D11Texture2D* back = NULL;
    if (SUCCEEDED(SwapChain->GetBuffer(0, IID_PPV_ARGS(&back)))) {
        Device->CreateRenderTargetView(back, NULL, &Target);
        back->Release();
    }
}

bool MakeDevice() {
    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferCount = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = Window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got;
    HRESULT result = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, levels, 2, D3D11_SDK_VERSION, &desc, &SwapChain,
                                                   &Device, &got, &Context);
    if (result == DXGI_ERROR_UNSUPPORTED)  // No usable graphics card: drawn by the processor.
        result = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, levels, 2, D3D11_SDK_VERSION, &desc, &SwapChain,
                                               &Device, &got, &Context);
    if (FAILED(result)) return false;
    MakeTarget();
    return true;
}

void Resize(UINT width, UINT height) {
    if (!SwapChain || !width || !height) return;
    if (Target) Target->Release();
    Target = NULL;
    SwapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    MakeTarget();
}

void Frame() {
    if (InFrame || !Device || !IsWindowVisible(Window) || IsIconic(Window)) return;
    InFrame = true;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    Draw();
    ImGui::Render();
    ImVec4 back = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    const float clear[4] = {back.x, back.y, back.z, 1};
    Context->OMSetRenderTargets(1, &Target, NULL);
    Context->ClearRenderTargetView(Target, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    SwapChain->Present(1, 0);
    InFrame = false;
    std::vector<std::function<void()>> work;
    work.swap(AfterFrame);
    for (const auto& w : work) w();
}

void ApplyScale(UINT dpi) {
    Scale = dpi / 96.0f;
    ApplyStyle();
}

// The notification area -------------------------------------------------------

void AddTray() {
    Tray = NOTIFYICONDATAW();
    Tray.cbSize = sizeof(Tray);
    Tray.hWnd = Window;
    Tray.uID = 1;
    Tray.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    Tray.uCallbackMessage = WM_TRAY;
    Tray.hIcon = SmallIcon;
    wcsncpy_s(Tray.szTip, AppName, _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &Tray);
    Tray.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &Tray);
}

void ShowMain() {
    ShowWindow(Window, IsIconic(Window) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(Window);
    Rescan();
    WantFrames(3);
}

void HideToTray() {
    ShowWindow(Window, SW_HIDE);
    if (!Saved.toldTray) {
        Saved.toldTray = true;
        SaveConfig();
        Tray.uFlags = NIF_INFO;
        wcsncpy_s(Tray.szInfoTitle, L"NetISO for Aurora keeps serving your games", _TRUNCATE);
        wcsncpy_s(Tray.szInfo, L"It's here in the notification area. Click the icon to open it, or right-click it to quit.", _TRUNCATE);
        Tray.dwInfoFlags = NIIF_INFO;
        Shell_NotifyIconW(NIM_MODIFY, &Tray);
    }
}

void TrayMenu(int x, int y) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IdMenuOpen, L"Open");
    AppendMenuW(menu, MF_STRING, IdMenuStart, Server.running ? L"Stop the server" : L"Start the server");
    AppendMenuW(menu, MF_STRING | (StartsWithWindows() ? MF_CHECKED : 0), IdMenuBoot, L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IdMenuQuit, L"Quit");
    SetMenuDefaultItem(menu, IdMenuOpen, FALSE);
    SetForegroundWindow(Window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, x, y, 0, Window, NULL);
    PostMessageW(Window, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void PickFolders() {
    IFileOpenDialog* dialog = NULL;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_ALLOWMULTISELECT | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"Folders with your Xbox 360 games");
    if (SUCCEEDED(dialog->Show(Window))) {
        IShellItemArray* items = NULL;
        DWORD count = 0;
        if (SUCCEEDED(dialog->GetResults(&items)) && SUCCEEDED(items->GetCount(&count))) {
            for (DWORD i = 0; i < count; ++i) {
                IShellItem* item = NULL;
                PWSTR path = NULL;
                if (SUCCEEDED(items->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) AddFolder(path);
                CoTaskMemFree(path);
                if (item) item->Release();
            }
        }
        if (items) items->Release();
    }
    dialog->Release();
}

void DropFolders(HDROP drop) {
    UINT count = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
    std::vector<std::wstring> folders;
    for (UINT i = 0; i < count; ++i) {
        std::wstring path(DragQueryFileW(drop, i, NULL, 0) + 1, L'\0');
        path.resize(DragQueryFileW(drop, i, &path[0], UINT(path.size())));
        DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) folders.push_back(path);
    }
    DragFinish(drop);
    for (const std::wstring& f : folders) AddFolder(f);
}

void FoldersChanged() {
    SaveConfig();
    Rescan();
    if (Server.running || !Saved.folders.empty()) {
        StopServer(false);
        StartServer();
    }
}

bool InputMessage(UINT message) {
    return (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) || (message >= WM_KEYFIRST && message <= WM_KEYLAST) ||
           message == WM_MOUSELEAVE || message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_SETCURSOR ||
           message == WM_IME_COMPOSITION || message == WM_ACTIVATE || message == WM_ACTIVATEAPP;
}

LRESULT CALLBACK Procedure(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == TaskbarCreated && TaskbarCreated) {  // Explorer restarted.
        AddTray();
        SetTrayTip();
        return 0;
    }
    if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, message, wParam, lParam)) {
        WantFrames(3);
        return 1;
    }
    if (InputMessage(message)) WantFrames(3);
    switch (message) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) {
            HideToTray();
            return 0;
        }
        Resize(LOWORD(lParam), HIWORD(lParam));
        Frame();  // Also while the edge is being dragged.
        return 0;
    case WM_PAINT:
        ValidateRect(hwnd, NULL);
        WantFrames(1);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* info = (MINMAXINFO*)lParam;
        RECT size = {0, 0, LONG(720 * Scale), LONG(520 * Scale)};
        AdjustWindowRectEx(&size, WS_OVERLAPPEDWINDOW, FALSE, 0);
        info->ptMinTrackSize.x = size.right - size.left;
        info->ptMinTrackSize.y = size.bottom - size.top;
        return 0;
    }
    case WM_DPICHANGED: {
        ApplyScale(HIWORD(wParam));
        RECT* suggested = (RECT*)lParam;
        SetWindowPos(hwnd, NULL, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (lParam && !wcscmp((const wchar_t*)lParam, L"ImmersiveColorSet")) UpdateTheme();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IdMenuOpen: ShowMain(); break;
        case IdMenuStart:
            if (Server.running) StopServer(true);
            else StartServer();
            break;
        case IdMenuBoot: SetStartWithWindows(!StartsWithWindows()); break;
        case IdMenuQuit: Quit(); break;
        }
        return 0;
    case WM_DROPFILES:
        DropFolders((HDROP)wParam);
        return 0;
    case WM_TRAY:
        switch (LOWORD(lParam)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case WM_LBUTTONDBLCLK: ShowMain(); break;
        case WM_CONTEXTMENU: TrayMenu(GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam)); break;
        }
        return 0;
    case WM_SHOWMAIN:
        ShowMain();
        return 0;
    case WM_LOGLINE: {
        std::string* line = (std::string*)lParam;
        Log(*line);
        if (wParam == Generation) ReadLine(*line);
        delete line;
        ServerChanged();
        return 0;
    }
    case WM_STOPPED:
        if (wParam == Generation && Server.running) {
            DWORD code = 0;
            WaitForSingleObject(Child, 2000);
            GetExitCodeProcess(Child, &code);
            CloseHandle(Child);
            Child = NULL;
            Server.running = false;
            Log("The server stopped" + (code ? " (exit code " + std::to_string(code) + ")." : std::string(".")));
            ServerChanged();
        }
        return 0;
    case WM_LIBRARY:
        TakeLibrary((void*)lParam);
        return 0;
    case WM_COVER:
        TakeCover((void*)lParam);
        return 0;
    case WM_CLOSE:
        // Closing keeps the server running in the notification area; Quit ends it.
        HideToTray();
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wParam) Quit();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void HighDpi() {
    typedef BOOL(WINAPI * SetContext)(HANDLE);
    HMODULE user = GetModuleHandleW(L"user32.dll");
    SetContext set = user ? (SetContext)(void*)GetProcAddress(user, "SetProcessDpiAwarenessContext") : NULL;
    if (!set || !set((HANDLE)-4 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */)) SetProcessDPIAware();
}

UINT WindowDpi(HWND hwnd) {
    typedef UINT(WINAPI * GetDpi)(HWND);
    HMODULE user = GetModuleHandleW(L"user32.dll");
    GetDpi get = user ? (GetDpi)(void*)GetProcAddress(user, "GetDpiForWindow") : NULL;
    if (get) return get(hwnd);
    HDC dc = GetDC(hwnd);
    UINT dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(hwnd, dc);
    return dpi;
}

// Waits for messages, drawing only when something changed: the window
// costs nothing while it sits there, or hides in the notification area.
void Loop() {
    for (;;) {
        bool visible = IsWindowVisible(Window) && !IsIconic(Window);
        if (!visible || FramesWanted <= 0) {
            DWORD wait = INFINITE;
            if (visible && WakeAt) wait = LONG(WakeAt - GetTickCount()) > 0 ? WakeAt - GetTickCount() : 0;
            if (visible && ImGui::GetIO().WantTextInput && wait > 500) wait = 500;  // The caret blinks.
            if (MsgWaitForMultipleObjects(0, NULL, FALSE, wait, QS_ALLINPUT) == WAIT_TIMEOUT) {
                WakeAt = 0;
                WantFrames(1);
            }
        }
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (WakeAt && LONG(WakeAt - GetTickCount()) <= 0) {
            WakeAt = 0;
            WantFrames(1);
        }
        if (FramesWanted > 0 && IsWindowVisible(Window) && !IsIconic(Window)) {
            --FramesWanted;
            Frame();
        } else if (!IsWindowVisible(Window)) {
            FramesWanted = 0;
        }
    }
}

int RunWindow(bool tray) {
    HANDLE once = CreateMutexW(NULL, FALSE, L"Local\\NetIsoAuroraServerWindow");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Already running: show that one instead.
        HWND other = FindWindowW(WindowClass, NULL);
        if (other && !tray) PostMessageW(other, WM_SHOWMAIN, 0, 0);
        return 0;
    }
    HighDpi();
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    LoadConfig();
    // Keep "Start with Windows" pointing at this exe if it was moved.
    std::wstring entry = StartupEntry();
    if (!entry.empty() && entry != RunCommand()) SetStartWithWindows(true);

    Job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(Job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

    HINSTANCE self = GetModuleHandleW(NULL);
    BigIcon = (HICON)LoadImageW(self, MAKEINTRESOURCEW(IconId), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    SmallIcon = (HICON)LoadImageW(self, MAKEINTRESOURCEW(IconId), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!BigIcon) BigIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    if (!SmallIcon) SmallIcon = BigIcon;
    WNDCLASSEXW kind = {sizeof(kind)};
    kind.style = CS_HREDRAW | CS_VREDRAW;
    kind.lpfnWndProc = Procedure;
    kind.hInstance = self;
    kind.hIcon = BigIcon;
    kind.hIconSm = SmallIcon;
    kind.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    kind.lpszClassName = WindowClass;
    RegisterClassExW(&kind);
    TaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    // Created at the primary monitor's DPI, then sized for the one it's on.
    Window = CreateWindowExW(0, WindowClass, AppName, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                             NULL, NULL, self, NULL);
    if (!Window) return 1;
    Scale = WindowDpi(Window) / 96.0f;
    RECT size = {0, 0, LONG(1060 * Scale), LONG(720 * Scale)};
    AdjustWindowRectEx(&size, WS_OVERLAPPEDWINDOW, FALSE, 0);
    SetWindowPos(Window, NULL, 0, 0, size.right - size.left, size.bottom - size.top, SWP_NOMOVE | SWP_NOZORDER);
    if (!MakeDevice()) {
        MessageBoxW(NULL, L"Direct3D 11 isn't available, so the window can't be shown. The server also runs from a command prompt: netiso-server.exe -h",
                    AppName, MB_ICONERROR);
        return 1;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = NULL;
    ImGui_ImplWin32_Init(Window);
    ImGui_ImplDX11_Init(Device, Context);
    LoadFonts();
    UpdateTheme();
    DragAcceptFiles(Window, TRUE);
    AddTray();
    StartLibrary(Window, WM_LIBRARY, WM_COVER);
    Log("Settings: " + Utf8(IniPath()));
    StartServer();
    if (!tray) {
        ShowWindow(Window, SW_SHOWNORMAL);
        UpdateWindow(Window);
    }
    WantFrames(3);
    Loop();

    StopLibrary();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (Target) Target->Release();
    if (SwapChain) SwapChain->Release();
    if (Context) Context->Release();
    if (Device) Device->Release();
    CloseHandle(Job);
    CloseHandle(once);
    CoUninitialize();
    return 0;
}

// With options: the command-line server, printing to the console it was
// started from (or to the pipe of the window above).
void UseConsole() {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN) return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
    FILE* f;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    freopen_s(&f, "CONIN$", "r", stdin);
    printf("\n");
}
}  // namespace

// What the pages can do --------------------------------------------------------

std::string Utf8(const std::wstring& text) { return Narrow(text, CP_UTF8); }
std::wstring Wide(const std::string& utf8) { return Widen(utf8, CP_UTF8); }
std::string AnsiToUtf8(const std::string& text) { return Narrow(Widen(text, CP_ACP), CP_UTF8); }
std::string Utf8ToAnsi(const std::string& text) { return Narrow(Widen(text, CP_UTF8), CP_ACP); }

std::wstring IniPath() {
    PWSTR roaming = NULL;
    std::wstring folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, NULL, &roaming))) folder = roaming;
    CoTaskMemFree(roaming);
    if (folder.empty()) folder = ExeFolder();
    folder += L"\\NetISO for Aurora";
    CreateDirectoryW(folder.c_str(), NULL);
    return folder + L"\\netiso-server.ini";
}

void SaveConfig() {
    std::wstring ini = IniPath();
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // A UTF-16 file, so folder names keep every character.
        HANDLE f = CreateFileW(ini.c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            static const unsigned char bom[2] = {0xFF, 0xFE};
            DWORD written;
            WriteFile(f, bom, 2, &written, NULL);
            CloseHandle(f);
        }
    }
    WritePrivateProfileStringW(Section, NULL, NULL, ini.c_str());
    WritePrivateProfileStringW(Section, L"folders", std::to_wstring(Saved.folders.size()).c_str(), ini.c_str());
    for (size_t i = 0; i < Saved.folders.size(); ++i)
        WritePrivateProfileStringW(Section, (L"folder" + std::to_wstring(i + 1)).c_str(), Saved.folders[i].c_str(), ini.c_str());
    WritePrivateProfileStringW(Section, L"xbox", Saved.xbox.c_str(), ini.c_str());
    WritePrivateProfileStringW(Section, L"ftp_user", Saved.user.c_str(), ini.c_str());
    WritePrivateProfileStringW(Section, L"ftp_password", Protect(Saved.password).c_str(), ini.c_str());
    WritePrivateProfileStringW(Section, L"options", Saved.options.c_str(), ini.c_str());
    WritePrivateProfileStringW(Section, L"theme", std::to_wstring(Saved.theme).c_str(), ini.c_str());
    WriteFlag(ini, L"covers", Saved.covers);
    WriteFlag(ini, L"list_view", Saved.listView);
    WriteFlag(ini, L"sort_by_size", Saved.sortBySize);
    WriteFlag(ini, L"show_iso", Saved.showIso);
    WriteFlag(ini, L"show_god", Saved.showGod);
    WriteFlag(ini, L"show_folder", Saved.showFolder);
    WriteFlag(ini, L"told_tray", Saved.toldTray);
}

void WantFrames(int count) {
    if (FramesWanted < count) FramesWanted = count;
}

void WantFrameAfter(int milliseconds) {
    DWORD at = GetTickCount() + DWORD(milliseconds);
    if (!at) at = 1;
    if (!WakeAt || LONG(at - WakeAt) < 0) WakeAt = at;
}

void Log(const std::string& line) {
    Server.log.push_back(line);
    if (Server.log.size() > LogLines) Server.log.erase(Server.log.begin(), Server.log.begin() + LogLines / 5);
    WantFrames(1);
}

void StartServer() {
    if (Server.running) return;
    Server.games = -1;
    Server.ftp = 0;
    Server.ftpNote.clear();
    Server.playing.clear();
    if (Saved.folders.empty()) {
        Log("Add a folder with your games to start the server.");
        ServerChanged();
        return;
    }
    SECURITY_ATTRIBUTES inherit = {sizeof(inherit), NULL, TRUE};
    HANDLE readEnd, writeEnd;
    if (!CreatePipe(&readEnd, &writeEnd, &inherit, 0)) return;
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, NULL);
    STARTUPINFOW start = {sizeof(start)};
    start.dwFlags = STARTF_USESTDHANDLES;
    start.hStdInput = nul;
    start.hStdOutput = start.hStdError = writeEnd;
    PROCESS_INFORMATION process;
    std::wstring command = ServerCommandLine();
    std::wstring folder = ExeFolder();
    // The password travels in the server's environment, not on its command line.
    SetEnvironmentVariableW(L"NETISO_FTP_PASSWORD", Saved.password.c_str());
    BOOL started = CreateProcessW(ExePath().c_str(), &command[0], NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                                  NULL, folder.c_str(), &start, &process);
    SetEnvironmentVariableW(L"NETISO_FTP_PASSWORD", NULL);
    CloseHandle(writeEnd);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!started) {
        CloseHandle(readEnd);
        Log("Couldn't start the server (error " + std::to_string(GetLastError()) + ").");
        ServerChanged();
        return;
    }
    // In a job, so the server ends with this window, even if it crashes.
    AssignProcessToJobObject(Job, process.hProcess);
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    Child = process.hProcess;
    Server.running = true;
    std::thread(ReadOutput, readEnd, ++Generation).detach();
    ServerChanged();
}

void StopServer(bool say) {
    if (!Server.running) return;
    ++Generation;  // Its last lines and its end are no longer news.
    TerminateProcess(Child, 0);
    WaitForSingleObject(Child, 5000);
    CloseHandle(Child);
    Child = NULL;
    Server.running = false;
    Server.playing.clear();
    if (say) Log("Server stopped.");
    ServerChanged();
}

void Quit() {
    Later([] {
        if (Quitting) return;
        Quitting = true;
        StopServer(false);
        Shell_NotifyIconW(NIM_DELETE, &Tray);
        DestroyWindow(Window);
    });
}

void AddFolders() {
    Later(PickFolders);
}

void AddFolder(const std::wstring& path) {
    std::wstring folder = path;
    while (folder.size() > 3 && (folder.back() == L'\\' || folder.back() == L'/')) folder.pop_back();
    for (const std::wstring& f : Saved.folders)
        if (!_wcsicmp(f.c_str(), folder.c_str())) return;
    Saved.folders.push_back(folder);
    Later(FoldersChanged);
}

void RemoveFolder(size_t index) {
    if (index >= Saved.folders.size()) return;
    Saved.folders.erase(Saved.folders.begin() + index);
    Later(FoldersChanged);
}

void ShowInExplorer(const std::string& path) {
    std::wstring wide = Wide(path);
    DWORD attributes = GetFileAttributesW(wide.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return;
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
        ShellExecuteW(Window, L"open", wide.c_str(), NULL, NULL, SW_SHOWNORMAL);
        return;
    }
    PIDLIST_ABSOLUTE item = NULL;
    if (SUCCEEDED(SHParseDisplayName(wide.c_str(), NULL, &item, 0, NULL))) {
        SHOpenFolderAndSelectItems(item, 0, NULL, 0);
        CoTaskMemFree(item);
    }
}

void OpenUrl(const char* url) { ShellExecuteW(Window, L"open", Wide(url).c_str(), NULL, NULL, SW_SHOWNORMAL); }

void SetConnection(const std::wstring& xbox, const std::wstring& user, const std::wstring& password) {
    if (xbox == Saved.xbox && user == Saved.user && password == Saved.password) return;
    Saved.xbox = xbox;
    Saved.user = user;
    Saved.password = password;
    SaveConfig();
    Later(RestartServer);
}

void SetOptions(const std::wstring& options) {
    if (options == Saved.options) return;
    Saved.options = options;
    SaveConfig();
    Later(RestartServer);
}

bool StartsWithWindows() { return !StartupEntry().empty(); }

void SetStartWithWindows(bool on) {
    if (on) {
        std::wstring command = RunCommand();
        RegSetKeyValueW(HKEY_CURRENT_USER, RunKey, RunValue, REG_SZ, command.c_str(), DWORD((command.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, RunKey, RunValue);
    }
}

void SetTheme(int theme) {
    Saved.theme = theme;
    SaveConfig();
    UpdateTheme();
}
}  // namespace gui

// Runs the window and returns its exit code, or returns -1 when the exe was
// started with options and should run as the command-line server.
int GuiMain(int argc, char** argv) {
    bool tray = argc == 2 && !strcmp(argv[1], "--tray");
    if (argc == 1 || tray) return gui::RunWindow(tray);
    gui::UseConsole();
    return -1;
}
#endif
