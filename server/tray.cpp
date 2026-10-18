// SPDX-License-Identifier: MIT
// NetDisc.exe (NetDisc 360) on Windows. Started with no options (from
// Explorer, a shortcut, or at sign-in with --tray), it runs the server behind
// an icon in the notification area and opens the dashboard, the web page the
// server serves (web.cpp), in the browser. Clicking the icon opens it again;
// its menu has "Start with Windows" and Quit. The settings are kept in
// %APPDATA%\NetDisc 360\netdisc.ini (the FTP password encrypted for this
// Windows user).
//
// Started with options, the exe is the command-line server; this file then
// only connects it to the console it was started from.
#ifdef _WIN32
#include "app.h"
#include "../shared/platform.h"
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")

namespace {
const wchar_t AppName[] = L"NetDisc 360";
const wchar_t WindowClass[] = L"NetDisc360Tray";
// The names before NetDisc 360: its settings and "Start with Windows" carry over.
const wchar_t OldName[] = L"NetISO for Aurora";
const wchar_t OldRunValue[] = L"NetISO server for Aurora";
const wchar_t RunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t RunValue[] = L"NetDisc 360";
const int IconId = 1;  // server.rc
enum { WM_TRAY = WM_APP + 1, WM_READY, WM_STOPPED, WM_SHOWDASHBOARD, WM_NOTE };
enum { IdOpen = 200, IdBoot, IdQuit, TipTimer = 1 };

HWND Window;
NOTIFYICONDATAW Tray;
UINT TaskbarCreated;
std::wstring Url;     // The dashboard, once it listens.
bool OpenWhenReady;   // Open it as soon as it does.
std::wstring BalloonPage;  // Where clicking the last balloon goes: "" the dashboard, "#games" a page.

struct Ready { std::string url, problem; };
struct Note { std::string title, text; };

std::wstring Wide(const std::string& text, UINT page) {
    int n = MultiByteToWideChar(page, 0, text.data(), int(text.size()), NULL, 0);
    std::wstring out(size_t(n), L'\0');
    if (n) MultiByteToWideChar(page, 0, text.data(), int(text.size()), &out[0], n);
    return out;
}

std::string Narrow(const std::wstring& text, UINT page) {
    int n = WideCharToMultiByte(page, 0, text.data(), int(text.size()), NULL, 0, NULL, NULL);
    std::string out(size_t(n), '\0');
    if (n) WideCharToMultiByte(page, 0, text.data(), int(text.size()), &out[0], n, NULL, NULL);
    return out;
}

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

// %APPDATA%\NetDisc 360\netdisc.ini, copied from the old name's folder the first time.
std::wstring IniPath() {
    PWSTR roaming = NULL;
    std::wstring folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, NULL, &roaming))) folder = roaming;
    CoTaskMemFree(roaming);
    if (folder.empty()) folder = ExeFolder();
    std::wstring old = folder + L"\\" + OldName + L"\\netiso-server.ini";
    folder += L"\\";
    folder += AppName;
    CreateDirectoryW(folder.c_str(), NULL);
    std::wstring ini = folder + L"\\netdisc.ini";
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) CopyFileW(old.c_str(), ini.c_str(), TRUE);
    return ini;
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

// Start with Windows: a value under HKCU\...\Run that starts the exe in the tray.
std::wstring RunCommand() { return L"\"" + ExePath() + L"\" --tray"; }

std::wstring StartupEntry() {
    wchar_t value[MAX_PATH * 4];
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, RunKey, RunValue, RRF_RT_REG_SZ, NULL, value, &size) != ERROR_SUCCESS) return L"";
    return value;
}

void AddTray() {
    Tray = NOTIFYICONDATAW();
    Tray.cbSize = sizeof(Tray);
    Tray.hWnd = Window;
    Tray.uID = 1;
    Tray.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    Tray.uCallbackMessage = WM_TRAY;
    Tray.hIcon = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IconId), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    if (!Tray.hIcon) Tray.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    wcsncpy_s(Tray.szTip, AppName, _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &Tray);
    Tray.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &Tray);
}

void SetTip() {
    std::wstring tip = std::wstring(AppName) + L": " + Wide(ServerSummary(), CP_UTF8);
    if (!wcscmp(tip.c_str(), Tray.szTip)) return;
    wcsncpy_s(Tray.szTip, tip.c_str(), _TRUNCATE);
    Tray.uFlags = NIF_TIP | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &Tray);
}

void Balloon(const wchar_t* title, const wchar_t* text, DWORD icon = NIIF_INFO, const wchar_t* page = L"") {
    Tray.uFlags = NIF_INFO;
    wcsncpy_s(Tray.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(Tray.szInfo, text, _TRUNCATE);
    Tray.dwInfoFlags = icon;
    BalloonPage = page;
    Shell_NotifyIconW(NIM_MODIFY, &Tray);
}

void OpenDashboard(const std::wstring& page = L"") {
    if (Url.empty()) OpenWhenReady = true;
    else ShellExecuteW(NULL, L"open", (Url + page).c_str(), NULL, NULL, SW_SHOWNORMAL);
}

void Quit() {
    Shell_NotifyIconW(NIM_DELETE, &Tray);
    // The server's threads never end by themselves; its settings are saved as they change.
    ExitProcess(0);
}

void Menu(int x, int y) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IdOpen, L"Open the dashboard");
    AppendMenuW(menu, MF_STRING | (StartsWithWindows() ? MF_CHECKED : 0), IdBoot, L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IdQuit, L"Quit NetDisc 360");
    SetMenuDefaultItem(menu, IdOpen, FALSE);
    SetForegroundWindow(Window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, x, y, 0, Window, NULL);
    PostMessageW(Window, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

LRESULT CALLBACK Procedure(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == TaskbarCreated && TaskbarCreated) {  // Explorer restarted.
        AddTray();
        SetTip();
        return 0;
    }
    switch (message) {
    case WM_TRAY:
        switch (LOWORD(lParam)) {
        case WM_LBUTTONUP: case NIN_SELECT: case NIN_KEYSELECT: OpenDashboard(); break;
        case NIN_BALLOONUSERCLICK: OpenDashboard(BalloonPage); break;
        case WM_CONTEXTMENU: Menu(short(LOWORD(wParam)), short(HIWORD(wParam))); break;
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IdOpen: OpenDashboard(); break;
        case IdBoot: SetStartWithWindows(!StartsWithWindows()); break;
        case IdQuit: Quit(); break;
        }
        return 0;
    case WM_SHOWDASHBOARD:  // Started again.
        OpenDashboard();
        return 0;
    case WM_READY: {
        Ready* ready = (Ready*)lParam;
        if (!ready->url.empty()) {
            Url = Wide(ready->url, CP_UTF8);
            if (OpenWhenReady) OpenDashboard();
            OpenWhenReady = false;
            if (FirstTrayStart())
                Balloon(L"NetDisc 360 is serving your games",
                        L"It runs here in the notification area. Click the icon to open the dashboard, or right-click it to quit.");
        } else {
            std::wstring text = L"The dashboard couldn't start: " + Wide(ready->problem, CP_ACP) + L"\n\nThe server itself runs.";
            MessageBoxW(NULL, text.c_str(), AppName, MB_ICONWARNING);
        }
        delete ready;
        return 0;
    }
    case WM_NOTE: {
        Note* note = (Note*)lParam;
        Balloon(Wide(note->title, CP_UTF8).c_str(), Wide(note->text, CP_UTF8).c_str(), NIIF_WARNING, L"#games");
        delete note;
        return 0;
    }
    case WM_STOPPED: {
        std::wstring text = L"NetDisc 360 couldn't start.\n\n" + Wide(ServerProblem(), CP_ACP);
        MessageBoxW(NULL, text.c_str(), AppName, MB_ICONERROR);
        Quit();
        return 0;
    }
    case WM_TIMER:
        SetTip();
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

// With options: the command-line server, printing to the console it was
// started from.
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

void DashboardReady(const std::string& url, const std::string& problem) {
    Ready* ready = new Ready;
    ready->url = url;
    ready->problem = problem;
    if (!PostMessageW(Window, WM_READY, 0, (LPARAM)ready)) delete ready;
}

void Notify(const std::string& title, const std::string& text) {
    Note* note = new Note;
    note->title = title;
    note->text = text;
    if (!PostMessageW(Window, WM_NOTE, 0, (LPARAM)note)) delete note;
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

int TrayMain(int argc, char** argv) {
    bool tray = argc == 2 && !strcmp(argv[1], "--tray");
    if (argc > 1 && !tray) {
        UseConsole();
        return -1;
    }
    HANDLE once = CreateMutexW(NULL, FALSE, L"Local\\NetDisc360Window");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Already running: open its dashboard instead.
        HWND other = FindWindowW(WindowClass, NULL);
        if (other && !tray) PostMessageW(other, WM_SHOWDASHBOARD, 0, 0);
        return 0;
    }
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    std::wstring ini = IniPath();
    std::vector<std::string> args;
    // First start: serve the folder the exe is in when it has games, so the
    // exe can simply be put in the folder with them. Run from the Desktop or
    // Downloads, it starts with no folders, which the dashboard asks for.
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES && HasGames(ExeFolder(), 3)) args.push_back(Narrow(ExeFolder(), CP_ACP));
    wchar_t old[MAX_PATH * 4];
    DWORD oldSize = sizeof(old);
    if (RegGetValueW(HKEY_CURRENT_USER, RunKey, OldRunValue, RRF_RT_REG_SZ, NULL, old, &oldSize) == ERROR_SUCCESS) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, RunKey, OldRunValue);
        SetStartWithWindows(true);
    }
    // Keep "Start with Windows" pointing at this exe if it was moved.
    std::wstring entry = StartupEntry();
    if (!entry.empty() && entry != RunCommand()) SetStartWithWindows(true);

    WNDCLASSEXW kind = {sizeof(kind)};
    kind.lpfnWndProc = Procedure;
    kind.hInstance = GetModuleHandleW(NULL);
    kind.lpszClassName = WindowClass;
    RegisterClassExW(&kind);
    TaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    // Never shown: it gets the icon's clicks and Explorer's restarts.
    Window = CreateWindowExW(0, WindowClass, AppName, WS_OVERLAPPED, 0, 0, 0, 0, NULL, NULL, kind.hInstance, NULL);
    if (!Window) return 1;
    AddTray();
    OpenWhenReady = !tray;
    std::string config = Narrow(ini, CP_UTF8);
    std::thread([args, config]() {
        int code = RunServer(args, config, true);
        PostMessageW(Window, WM_STOPPED, WPARAM(code), 0);
    }).detach();
    SetTimer(Window, TipTimer, 3000, NULL);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    CloseHandle(once);
    return 0;
}
#endif
