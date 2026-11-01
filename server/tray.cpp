// SPDX-License-Identifier: MIT
// NetDisc360.exe on Windows. Started with no options (from
// Explorer, a shortcut, or at sign-in with --tray), it runs the server behind
// an icon in the notification area and opens the dashboard, the web page the
// server serves (web.cpp), in the browser. Clicking the icon opens it again;
// its menu has "Start with Windows" and Quit. The settings are kept in
// %APPDATA%\NetDisc360\netdisc.ini (the FTP password encrypted for this
// Windows user).
//
// Started with options, the exe is the command-line server; this file then
// only connects it to the console it was started from.
//
// Restarting (after an update, or from the dashboard) starts the exe again
// with --tray --restarted <this process's id>; the new one waits for this
// one to end before it starts serving.
#ifdef _WIN32
#include "app.h"
#include "../shared/platform.h"
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")

namespace {
const wchar_t AppName[] = L"NetDisc360";
const wchar_t WindowClass[] = L"NetDisc360Tray";
// The names before NetDisc360: its settings and "Start with Windows" carry over.
const wchar_t OldFolder[] = L"NetDisc 360";
const wchar_t OldestFolder[] = L"NetISO for Aurora";
const wchar_t* const OldRunValues[] = {L"NetDisc 360", L"NetISO server for Aurora"};
const wchar_t RunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t RunValue[] = L"NetDisc360";
const int IconId = 1;  // server.rc
enum { WM_TRAY = WM_APP + 1, WM_READY, WM_STOPPED, WM_SHOWDASHBOARD, WM_NOTE, WM_RESTARTING };
enum { IdOpen = 200, IdBoot, IdQuit, TipTimer = 1, QuitTimer = 2, OldTimer = 3 };
// How long a new version gets after an update to have its dashboard up
// before the old one is put back (see WatchUpdate).
const DWORD UpdateGraceMs = 120000;

HWND Window;
NOTIFYICONDATAW Tray;
UINT TaskbarCreated;
std::wstring Url;     // The dashboard, once it listens.
bool OpenWhenReady;   // Open it as soon as it does.
std::wstring BalloonPage;  // Where clicking the last balloon goes: "" the dashboard, "#games" a page.
LONG Restarting;      // A new process was started to take over; this one quits.
int OldTries;         // Tries left at deleting the exe an update replaced (OldTimer).

struct Ready { std::string url, problem; };
struct Note { std::string title, text, page; bool warning; };

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

// Copies a folder with everything in it, leaving files already there alone.
void CopyFolder(const std::wstring& from, const std::wstring& to) {
    CreateDirectoryW(to.c_str(), NULL);
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW((from + L"\\*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = found.cFileName;
        if (name == L"." || name == L"..") continue;
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) CopyFolder(from + L"\\" + name, to + L"\\" + name);
        else CopyFileW((from + L"\\" + name).c_str(), (to + L"\\" + name).c_str(), TRUE);
    } while (FindNextFileW(search, &found));
    FindClose(search);
}

// %APPDATA%\NetDisc360\netdisc.ini. The first time, the folder of the name
// before is copied over whole (the disc map, backups and login key too), or
// else the oldest name's settings file. The old folder stays as it was.
std::wstring IniPath() {
    PWSTR roaming = NULL;
    std::wstring base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, NULL, &roaming))) base = roaming;
    CoTaskMemFree(roaming);
    if (base.empty()) base = ExeFolder();
    base += L"\\";
    std::wstring folder = base + AppName, ini = folder + L"\\netdisc.ini";
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW((base + OldFolder + L"\\netdisc.ini").c_str()) != INVALID_FILE_ATTRIBUTES)
        CopyFolder(base + OldFolder, folder);
    CreateDirectoryW(folder.c_str(), NULL);
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES)
        CopyFileW((base + OldestFolder + L"\\netiso-server.ini").c_str(), ini.c_str(), TRUE);
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

// Quit from the menu, or Windows shutting down: unlike a restart, nothing
// takes over, so the consoles hear first (ServerStopping).
void Stop() {
    Shell_NotifyIconW(NIM_DELETE, &Tray);
    ServerStopping();
    Quit();
}

void Menu(int x, int y) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IdOpen, L"Open the dashboard");
    AppendMenuW(menu, MF_STRING | (StartsWithWindows() ? MF_CHECKED : 0), IdBoot, L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IdQuit, L"Quit NetDisc360");
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
        case IdQuit: Stop(); break;
        }
        return 0;
    case WM_ENDSESSION:
        if (wParam) Stop();
        return 0;
    case WM_SHOWDASHBOARD:  // Started again.
        OpenDashboard();
        return 0;
    case WM_READY: {
        Ready* ready = (Ready*)lParam;
        // Serving: should an update have started this one, it worked
        // (WatchUpdate, running from the exe it replaced, stops watching and
        // quits), so that exe isn't needed now.
        wchar_t name[64];
        _snwprintf_s(name, ARRAYSIZE(name), _TRUNCATE, L"Local\\NetDisc360Ready%lu", GetCurrentProcessId());
        HANDLE signal = CreateEventW(NULL, TRUE, FALSE, name);  // Kept open while this one runs.
        if (signal) SetEvent(signal);
        OldTries = 15;
        SetTimer(Window, OldTimer, 1000, NULL);
        if (!ready->url.empty()) {
            Url = Wide(ready->url, CP_UTF8);
            if (OpenWhenReady) OpenDashboard();
            OpenWhenReady = false;
            if (FirstTrayStart())
                Balloon(L"NetDisc360 is serving your games",
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
        Balloon(Wide(note->title, CP_UTF8).c_str(), Wide(note->text, CP_UTF8).c_str(), note->warning ? NIIF_WARNING : NIIF_INFO,
                Wide(note->page, CP_UTF8).c_str());
        delete note;
        return 0;
    }
    case WM_RESTARTING:  // The new one waits; a moment for the dashboard's answer to go out first.
        SetTimer(Window, QuitTimer, 1500, NULL);
        return 0;
    case WM_STOPPED: {
        std::wstring text = L"NetDisc360 couldn't start.\n\n" + Wide(ServerProblem(), CP_ACP);
        MessageBoxW(NULL, text.c_str(), AppName, MB_ICONERROR);
        Quit();
        return 0;
    }
    case WM_TIMER:
        if (wParam == QuitTimer) Quit();
        if (wParam == OldTimer) {
            std::wstring old = ExePath() + L".old";
            if (DeleteFileW(old.c_str()) || GetFileAttributesW(old.c_str()) == INVALID_FILE_ATTRIBUTES || --OldTries <= 0) KillTimer(Window, OldTimer);
            return 0;
        }
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

void Notify(const std::string& title, const std::string& text, const std::string& page, bool warning) {
    Note* note = new Note;
    note->title = title;
    note->text = text;
    note->page = page;
    note->warning = warning;
    if (!PostMessageW(Window, WM_NOTE, 0, (LPARAM)note)) delete note;
}

namespace {
// Starts exe to take over from this process, which quits; with watcher, also
// starts that (the exe an update replaced) to put itself back should the new
// one not get its dashboard up (see WatchUpdate).
bool StartAgain(const std::wstring& exe, const std::wstring& watcher, std::string& error) {
    if (InterlockedCompareExchange(&Restarting, 1, 0)) { error = "NetDisc360 is restarting already."; return false; }
    wchar_t pid[16];
    _snwprintf_s(pid, ARRAYSIZE(pid), _TRUNCATE, L"%lu", GetCurrentProcessId());
    std::wstring command = L"\"" + exe + L"\" --tray --restarted " + pid;
    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION process;
    // With a watcher, the new one waits until that has hold of it, so it can't end unseen.
    if (!CreateProcessW(exe.c_str(), &command[0], NULL, NULL, FALSE, watcher.empty() ? 0 : CREATE_SUSPENDED, NULL, NULL, &startup, &process)) {
        char text[96];
        _snprintf_s(text, sizeof(text), _TRUNCATE, "NetDisc360 couldn't start again (error %lu).", GetLastError());
        error = text;
        InterlockedExchange(&Restarting, 0);
        return false;
    }
    if (!watcher.empty()) {
        wchar_t child[16];
        _snwprintf_s(child, ARRAYSIZE(child), _TRUNCATE, L"%lu", process.dwProcessId);
        std::wstring watch = L"\"" + watcher + L"\" --watch-update " + child;
        HANDLE held = CreateEventW(NULL, TRUE, FALSE, (L"Local\\NetDisc360Watching" + std::wstring(child)).c_str());
        STARTUPINFOW watchStartup = {sizeof(watchStartup)};
        PROCESS_INFORMATION watching;
        if (held && CreateProcessW(watcher.c_str(), &watch[0], NULL, NULL, FALSE, 0, NULL, NULL, &watchStartup, &watching)) {
            HANDLE either[2] = {held, watching.hProcess};
            WaitForMultipleObjects(2, either, FALSE, 10000);
            CloseHandle(watching.hThread);
            CloseHandle(watching.hProcess);
        }
        if (held) CloseHandle(held);
        ResumeThread(process.hThread);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    PostMessageW(Window, WM_RESTARTING, 0, 0);
    return true;
}

// Run as "<exe>.old --watch-update <pid>" by the version an update replaced:
// waits for the new one (pid) to have its dashboard up, which it signals.
// Should it end or hang first, it is stopped and this version goes back in
// its place and starts; it then says the update didn't take (RunServer).
int WatchUpdate(DWORD pid) {
    HANDLE child = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
    if (!child) return 0;
    wchar_t name[64];
    _snwprintf_s(name, ARRAYSIZE(name), _TRUNCATE, L"Local\\NetDisc360Watching%lu", pid);
    HANDLE held = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);  // The one that started it lets it go now.
    if (held) {
        SetEvent(held);
        CloseHandle(held);
    }
    _snwprintf_s(name, ARRAYSIZE(name), _TRUNCATE, L"Local\\NetDisc360Ready%lu", pid);
    HANDLE ready = CreateEventW(NULL, TRUE, FALSE, name);
    HANDLE both[2] = {ready, child};
    DWORD waited = ready ? WaitForMultipleObjects(2, both, FALSE, UpdateGraceMs) : WAIT_FAILED;
    if (ready) CloseHandle(ready);
    if (waited == WAIT_OBJECT_0 || waited == WAIT_FAILED) { CloseHandle(child); return 0; }
    TerminateProcess(child, 1);
    WaitForSingleObject(child, 10000);
    CloseHandle(child);
    std::wstring self = ExePath();
    if (self.size() < 5 || _wcsicmp(self.c_str() + self.size() - 4, L".old")) return 1;
    std::wstring exe = self.substr(0, self.size() - 4), broken = exe + L".new";
    MoveFileExW(exe.c_str(), broken.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!MoveFileExW(self.c_str(), exe.c_str(), MOVEFILE_WRITE_THROUGH)) return 1;
    DeleteFileW(broken.c_str());
    std::string error;
    return StartAgain(exe, L"", error) ? 0 : 1;
}
}  // namespace

bool Restart(std::string& error) { return StartAgain(ExePath(), L"", error); }

bool InstallExe(const std::string& data, std::string& error) {
    // Windows lets a running exe be renamed, not replaced: this one moves
    // aside to .old, and the new one takes its name.
    std::wstring exe = ExePath(), fresh = exe + L".new", old = exe + L".old";
    std::string folder = Narrow(ExeFolder(), CP_UTF8);
    HANDLE file = CreateFileW(fresh.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written = 0;
    bool ok = file != INVALID_HANDLE_VALUE && WriteFile(file, data.data(), DWORD(data.size()), &written, NULL) && written == data.size() &&
              FlushFileBuffers(file);  // On the disk before the renames, should the power go.
    if (file != INVALID_HANDLE_VALUE) ok = CloseHandle(file) && ok;
    if (!ok) {
        DeleteFileW(fresh.c_str());
        error = "NetDisc360 can't write in " + folder + ". Download the new version from GitHub and put it there yourself.";
        return false;
    }
    if (!MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(fresh.c_str());
        error = "NetDisc360 couldn't move itself aside in " + folder + ". Download the new version from GitHub and put it there yourself.";
        return false;
    }
    if (!MoveFileExW(fresh.c_str(), exe.c_str(), MOVEFILE_WRITE_THROUGH)) {
        bool back = MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
        DeleteFileW(fresh.c_str());
        error = "The new version couldn't take NetDisc360's place in " + folder + ".";
        std::string name = Narrow(exe.substr(exe.find_last_of(L"\\/") + 1), CP_UTF8);
        if (!back) error += " This version is there as " + name + ".old; rename it back to " + name + ".";
        return false;
    }
    if (StartAgain(exe, old, error)) return true;
    // The old one back, as the new one didn't start.
    MoveFileExW(exe.c_str(), fresh.c_str(), MOVEFILE_REPLACE_EXISTING);
    MoveFileExW(old.c_str(), exe.c_str(), 0);
    DeleteFileW(fresh.c_str());
    return false;
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
    if (argc == 3 && !strcmp(argv[1], "--watch-update")) return WatchUpdate(DWORD(strtoul(argv[2], NULL, 10)));
    bool tray = argc > 1 && !strcmp(argv[1], "--tray");
    bool restarted = tray && argc == 4 && !strcmp(argv[2], "--restarted");
    if (argc > 1 && !(tray && (argc == 2 || restarted))) {
        UseConsole();
        return -1;
    }
    if (restarted) {
        // The one that started this one quits once it has; then this one serves.
        HANDLE before = OpenProcess(SYNCHRONIZE, FALSE, DWORD(strtoul(argv[3], NULL, 10)));
        if (before) {
            WaitForSingleObject(before, 30000);
            CloseHandle(before);
        }
    }
    HANDLE once = CreateMutexW(NULL, FALSE, L"Local\\NetDisc360Window");
    // Started again: the one before may take a moment longer to go.
    for (int tries = 0; restarted && GetLastError() == ERROR_ALREADY_EXISTS && tries < 30; ++tries) {
        CloseHandle(once);
        Sleep(1000);
        once = CreateMutexW(NULL, FALSE, L"Local\\NetDisc360Window");
    }
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
    for (size_t i = 0; i < ARRAYSIZE(OldRunValues); ++i) {
        wchar_t old[MAX_PATH * 4];
        DWORD oldSize = sizeof(old);
        if (RegGetValueW(HKEY_CURRENT_USER, RunKey, OldRunValues[i], RRF_RT_REG_SZ, NULL, old, &oldSize) == ERROR_SUCCESS) {
            RegDeleteKeyValueW(HKEY_CURRENT_USER, RunKey, OldRunValues[i]);
            SetStartWithWindows(true);
        }
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
