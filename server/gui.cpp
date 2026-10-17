// SPDX-License-Identifier: MIT
// The window of netiso-server.exe on Windows. Started with no options (from
// Explorer, a shortcut, or at sign-in with --tray), the exe shows a small
// window: the game folders, the Xbox's address and Aurora's FTP login, a
// "Start with Windows" box and the server's log. The settings are kept in
// %APPDATA%\NetISO for Aurora\netiso-server.ini (the password encrypted for
// this Windows user), and the server itself runs as a second copy of this exe
// with those settings as options, its output shown in the log. Minimizing or
// closing the window leaves the server running behind an icon in the
// notification area.
//
// Started with options, the exe is the command-line server as before; this
// file then only connects it to the console it was started from.
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincrypt.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {
const wchar_t AppName[] = L"NetISO server for Aurora";
const wchar_t WindowClass[] = L"NetIsoAuroraServer";
const wchar_t RunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t RunValue[] = L"NetISO server for Aurora";
const wchar_t Section[] = L"Server";
const int IconId = 1;  // server.rc
enum { WM_TRAY = WM_APP + 1, WM_LOGLINE, WM_STOPPED, WM_SHOWMAIN };
enum { IdFolders = 100, IdAdd, IdRemove, IdXbox, IdUser, IdPassword, IdBoot, IdStart, IdQuit, IdLog,
       IdMenuOpen = 200, IdMenuStart, IdMenuBoot, IdMenuQuit };

struct Config {
    std::vector<std::wstring> folders;
    std::wstring xbox, user, password, options;
    bool toldTray;
    Config() : user(L"xboxftp"), password(L"xboxftp"), toldTray(false) {}
};
Config Saved;

HWND Window, FolderList, AddButton, RemoveButton, XboxEdit, UserEdit, PasswordEdit, BootBox, StartButton, QuitButton, LogEdit;
HWND FoldersLabel, FoldersHint, XboxLabel, XboxIpLabel, XboxHint, LoginLabel, StatusText, PcText, LogLabel;
HFONT Font, Bold, Mono;
HBRUSH Background;
HICON BigIcon, SmallIcon;
UINT Dpi = 96, TaskbarCreated;
NOTIFYICONDATAW Tray;
HANDLE Job, Child;
bool Running, Quitting;
unsigned Generation;
int Games = -1;
std::wstring LastLine;

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
    Saved.toldTray = GetPrivateProfileIntW(Section, L"told_tray", 0, ini.c_str()) != 0;
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
    WritePrivateProfileStringW(Section, L"told_tray", Saved.toldTray ? L"1" : L"0", ini.c_str());
}

// Start with Windows: a value under HKCU\...\Run that starts the exe in the tray.
std::wstring RunCommand() { return L"\"" + ExePath() + L"\" --tray"; }

std::wstring StartupEntry() {
    wchar_t value[MAX_PATH * 4];
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, RunKey, RunValue, RRF_RT_REG_SZ, NULL, value, &size) != ERROR_SUCCESS) return L"";
    return value;
}

void SetStartWithWindows(bool on) {
    if (on) {
        std::wstring command = RunCommand();
        RegSetKeyValueW(HKEY_CURRENT_USER, RunKey, RunValue, REG_SZ, command.c_str(), DWORD((command.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, RunKey, RunValue);
    }
}

// The PC's IPv4 addresses on networks with a gateway: what netiso.xex.txt needs.
std::wstring PcAddresses() {
    ULONG size = 16384;
    std::vector<BYTE> buffer(size);
    IP_ADAPTER_ADDRESSES* list = (IP_ADAPTER_ADDRESSES*)buffer.data();
    ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET, flags, NULL, list, &size) == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        list = (IP_ADAPTER_ADDRESSES*)buffer.data();
    }
    if (GetAdaptersAddresses(AF_INET, flags, NULL, list, &size) != NO_ERROR) return L"";
    std::wstring result;
    for (IP_ADAPTER_ADDRESSES* a = list; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || !a->FirstGatewayAddress) continue;
        for (IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u; u = u->Next) {
            sockaddr_in* at = (sockaddr_in*)u->Address.lpSockaddr;
            const BYTE* b = (const BYTE*)&at->sin_addr;
            wchar_t text[20];
            swprintf(text, 20, L"%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
            if (!result.empty()) result += L", ";
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
            int n = MultiByteToWideChar(CP_ACP, 0, line.data(), int(line.size()), NULL, 0);
            std::wstring* text = new std::wstring(n, L'\0');
            if (n) MultiByteToWideChar(CP_ACP, 0, line.data(), int(line.size()), &(*text)[0], n);
            if (!PostMessageW(Window, WM_LOGLINE, generation, (LPARAM)text)) delete text;
        }
    }
    CloseHandle(pipe);
    PostMessageW(Window, WM_STOPPED, generation, 0);
}

void UpdateStatus();
void AppendLog(const std::wstring& line);

void StartServer() {
    if (Running) return;
    Games = -1;
    LastLine.clear();
    if (Saved.folders.empty()) {
        AppendLog(L"Add a folder with your games to start the server.");
        UpdateStatus();
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
        AppendLog(L"Couldn't start the server (error " + std::to_wstring(GetLastError()) + L").");
        UpdateStatus();
        return;
    }
    // In a job, so the server ends with this window, even if it crashes.
    AssignProcessToJobObject(Job, process.hProcess);
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    Child = process.hProcess;
    Running = true;
    std::thread(ReadOutput, readEnd, ++Generation).detach();
    UpdateStatus();
}

void StopServer(bool say) {
    if (!Running) return;
    ++Generation;  // Its last lines and its end are no longer news.
    TerminateProcess(Child, 0);
    WaitForSingleObject(Child, 5000);
    CloseHandle(Child);
    Child = NULL;
    Running = false;
    if (say) AppendLog(L"Server stopped.");
    UpdateStatus();
}

void RestartServer() {
    bool was = Running;
    StopServer(false);
    if (was) AppendLog(L"Settings changed; restarting the server.");
    StartServer();
}

// The window -------------------------------------------------------------------

int Px(int dips) { return MulDiv(dips, Dpi, 96); }

void MakeFonts() {
    if (Font) DeleteObject(Font);
    if (Bold) DeleteObject(Bold);
    if (Mono) DeleteObject(Mono);
    NONCLIENTMETRICSW metrics = {sizeof(metrics)};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
    LOGFONTW face = metrics.lfMessageFont;
    face.lfHeight = -MulDiv(9, Dpi, 72);
    Font = CreateFontIndirectW(&face);
    face.lfWeight = FW_SEMIBOLD;
    face.lfHeight = -MulDiv(10, Dpi, 72);
    Bold = CreateFontIndirectW(&face);
    Mono = CreateFontW(-MulDiv(9, Dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
}

void LoadIcons() {
    HINSTANCE self = GetModuleHandleW(NULL);
    BigIcon = (HICON)LoadImageW(self, MAKEINTRESOURCEW(IconId), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    SmallIcon = (HICON)LoadImageW(self, MAKEINTRESOURCEW(IconId), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!BigIcon) BigIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    if (!SmallIcon) SmallIcon = BigIcon;
}

void ApplyFonts() {
    HWND normal[] = {FolderList, AddButton, RemoveButton, XboxEdit, UserEdit, PasswordEdit, BootBox, StartButton, QuitButton,
                     FoldersHint, XboxIpLabel, XboxHint, LoginLabel, PcText};
    for (HWND h : normal) SendMessageW(h, WM_SETFONT, (WPARAM)Font, TRUE);
    HWND bold[] = {FoldersLabel, XboxLabel, LogLabel, StatusText};
    for (HWND h : bold) SendMessageW(h, WM_SETFONT, (WPARAM)Bold, TRUE);
    SendMessageW(LogEdit, WM_SETFONT, (WPARAM)Mono, TRUE);
}

void Layout() {
    RECT client;
    GetClientRect(Window, &client);
    int w = client.right, h = client.bottom, m = Px(16), button = Px(120), row = Px(26), gap = Px(8);
    int y = m;
    MoveWindow(FoldersLabel, m, y, w - 2 * m, Px(20), TRUE);
    y += Px(24);
    int listW = w - 2 * m - button - gap;
    MoveWindow(FolderList, m, y, listW, Px(92), TRUE);
    MoveWindow(AddButton, w - m - button, y, button, Px(28), TRUE);
    MoveWindow(RemoveButton, w - m - button, y + Px(34), button, Px(28), TRUE);
    y += Px(96);
    MoveWindow(FoldersHint, m, y, w - 2 * m, Px(18), TRUE);
    y += Px(30);
    MoveWindow(XboxLabel, m, y, w - 2 * m, Px(20), TRUE);
    y += Px(26);
    int labelW = Px(130), editW = Px(150);
    MoveWindow(XboxIpLabel, m, y + Px(4), labelW, Px(18), TRUE);
    MoveWindow(XboxEdit, m + labelW, y, editW, Px(24), TRUE);
    MoveWindow(XboxHint, m + labelW + editW + gap, y + Px(4), w - m - (m + labelW + editW + gap), Px(18), TRUE);
    y += Px(32);
    MoveWindow(LoginLabel, m, y + Px(4), labelW, Px(18), TRUE);
    MoveWindow(UserEdit, m + labelW, y, editW, Px(24), TRUE);
    MoveWindow(PasswordEdit, m + labelW + editW + gap, y, editW, Px(24), TRUE);
    y += Px(34);
    MoveWindow(BootBox, m, y, w - 2 * m, Px(22), TRUE);
    y += Px(36);
    MoveWindow(StatusText, m, y + Px(2), w - 2 * m - 2 * button - gap, Px(22), TRUE);
    MoveWindow(StartButton, w - m - 2 * button - gap, y, button, Px(28), TRUE);
    MoveWindow(QuitButton, w - m - button, y, button, Px(28), TRUE);
    y += Px(30);
    MoveWindow(PcText, m, y, w - 2 * m, Px(18), TRUE);
    y += Px(30);
    MoveWindow(LogLabel, m, y, w - 2 * m, Px(20), TRUE);
    y += Px(24);
    MoveWindow(LogEdit, m, y, w - 2 * m, h - y - m > row ? h - y - m : row, TRUE);
}

HWND Make(const wchar_t* kind, const wchar_t* text, DWORD style, int id, DWORD exStyle = 0) {
    return CreateWindowExW(exStyle, kind, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, Window, (HMENU)(INT_PTR)id,
                           GetModuleHandleW(NULL), NULL);
}

void FillFolders() {
    SendMessageW(FolderList, LB_RESETCONTENT, 0, 0);
    for (const std::wstring& folder : Saved.folders) SendMessageW(FolderList, LB_ADDSTRING, 0, (LPARAM)folder.c_str());
    EnableWindow(RemoveButton, FALSE);
}

void CreateControls() {
    FoldersLabel = Make(L"STATIC", L"Game folders", SS_LEFT, -1);
    FolderList = Make(L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, IdFolders);
    AddButton = Make(L"BUTTON", L"Add folder...", BS_PUSHBUTTON | WS_TABSTOP, IdAdd);
    RemoveButton = Make(L"BUTTON", L"Remove", BS_PUSHBUTTON | WS_TABSTOP, IdRemove);
    FoldersHint = Make(L"STATIC", L"ISOs, extracted games and Games on Demand, in these folders and their subfolders. You can also drop folders here.",
                       SS_LEFT | SS_ENDELLIPSIS, -1);
    XboxLabel = Make(L"STATIC", L"Xbox", SS_LEFT, -1);
    XboxIpLabel = Make(L"STATIC", L"Xbox IP address", SS_LEFT, -1);
    XboxEdit = Make(L"EDIT", Saved.xbox.c_str(), WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, IdXbox);
    XboxHint = Make(L"STATIC", L"Empty: the Xbox that connects", SS_LEFT | SS_ENDELLIPSIS, -1);
    LoginLabel = Make(L"STATIC", L"Aurora FTP login", SS_LEFT, -1);
    UserEdit = Make(L"EDIT", Saved.user.c_str(), WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, IdUser);
    PasswordEdit = Make(L"EDIT", Saved.password.c_str(), WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD, IdPassword);
    SendMessageW(XboxEdit, EM_SETCUEBANNER, TRUE, (LPARAM)L"automatic");
    SendMessageW(UserEdit, EM_SETCUEBANNER, TRUE, (LPARAM)L"user name");
    SendMessageW(PasswordEdit, EM_SETCUEBANNER, TRUE, (LPARAM)L"password");
    BootBox = Make(L"BUTTON", L"Start with Windows (in the notification area)", BS_AUTOCHECKBOX | WS_TABSTOP, IdBoot);
    Button_SetCheck(BootBox, StartupEntry().empty() ? BST_UNCHECKED : BST_CHECKED);
    StatusText = Make(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, -1);
    StartButton = Make(L"BUTTON", L"Stop server", BS_PUSHBUTTON | WS_TABSTOP, IdStart);
    QuitButton = Make(L"BUTTON", L"Quit", BS_PUSHBUTTON | WS_TABSTOP, IdQuit);
    PcText = Make(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, -1);
    LogLabel = Make(L"STATIC", L"Log", SS_LEFT, -1);
    LogEdit = Make(L"EDIT", L"", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_TABSTOP, IdLog);
    SendMessageW(LogEdit, EM_SETLIMITTEXT, 0, 0);
    FillFolders();
    ApplyFonts();
}

void AppendLog(const std::wstring& line) {
    int length = GetWindowTextLengthW(LogEdit);
    if (length > 500000) {
        // Drop the older half, at a line break.
        int first = int(SendMessageW(LogEdit, EM_LINEFROMCHAR, length / 2, 0));
        int cut = int(SendMessageW(LogEdit, EM_LINEINDEX, first + 1, 0));
        SendMessageW(LogEdit, EM_SETSEL, 0, cut);
        SendMessageW(LogEdit, EM_REPLACESEL, FALSE, (LPARAM)L"");
        length = GetWindowTextLengthW(LogEdit);
    }
    std::wstring text = line + L"\r\n";
    SendMessageW(LogEdit, EM_SETSEL, length, length);
    SendMessageW(LogEdit, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
}

void SetTrayTip(const std::wstring& tip) {
    wcsncpy_s(Tray.szTip, tip.c_str(), _TRUNCATE);
    Tray.uFlags = NIF_TIP | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &Tray);
}

void UpdateStatus() {
    std::wstring status;
    if (Running) {
        status = L"\x25CF  Running";
        if (Games >= 0) status += L", serving " + std::to_wstring(Games) + (Games == 1 ? L" game" : L" games");
    } else {
        status = L"\x25CB  Stopped";
    }
    SetWindowTextW(StatusText, status.c_str());
    SetWindowTextW(StartButton, Running ? L"Stop server" : L"Start server");
    std::wstring pc = PcAddresses();
    SetWindowTextW(PcText, pc.empty() ? L"This PC has no network connection."
                                      : (L"This PC: " + pc + L"  (the address for netiso.xex.txt on the console)").c_str());
    SetTrayTip(std::wstring(L"NetISO server: ") + (Running ? L"running" : L"stopped") +
               (Running && Games >= 0 ? L", " + std::to_wstring(Games) + L" games" : L""));
    InvalidateRect(StatusText, NULL, TRUE);
}

// The server's own lines (after its "hh:mm:ss " time) that change the status.
void ReadLine(const std::wstring& line) {
    std::wstring text = line.size() > 9 ? line.substr(9) : line;
    int count;
    if (swscanf_s(text.c_str(), L"%d game(s) in", &count) == 1 && text.find(L" game(s) in ") != std::wstring::npos) Games = count;
    LastLine = text;
    UpdateStatus();
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
}

void HideToTray() {
    ShowWindow(Window, SW_HIDE);
    if (!Saved.toldTray) {
        Saved.toldTray = true;
        SaveConfig();
        Tray.uFlags = NIF_INFO;
        wcsncpy_s(Tray.szInfoTitle, L"NetISO server keeps running", _TRUNCATE);
        wcsncpy_s(Tray.szInfo, L"It's here in the notification area. Click the icon to open it, or right-click it to quit.", _TRUNCATE);
        Tray.dwInfoFlags = NIIF_INFO;
        Shell_NotifyIconW(NIM_MODIFY, &Tray);
    }
}

void TrayMenu(int x, int y) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IdMenuOpen, L"Open");
    AppendMenuW(menu, MF_STRING, IdMenuStart, Running ? L"Stop server" : L"Start server");
    AppendMenuW(menu, MF_STRING | (StartupEntry().empty() ? 0 : MF_CHECKED), IdMenuBoot, L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IdMenuQuit, L"Quit");
    SetMenuDefaultItem(menu, IdMenuOpen, FALSE);
    SetForegroundWindow(Window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, x, y, 0, Window, NULL);
    PostMessageW(Window, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

// Settings changes -------------------------------------------------------------

std::wstring Text(HWND edit) {
    std::wstring text(GetWindowTextLengthW(edit) + 1, L'\0');
    text.resize(GetWindowTextW(edit, &text[0], int(text.size())));
    size_t a = text.find_first_not_of(L" \t"), b = text.find_last_not_of(L" \t");
    return a == std::wstring::npos ? L"" : text.substr(a, b - a + 1);
}

void ReadEdits() {
    std::wstring xbox = Text(XboxEdit), user = Text(UserEdit), password = Text(PasswordEdit);
    if (xbox == Saved.xbox && user == Saved.user && password == Saved.password) return;
    Saved.xbox = xbox;
    Saved.user = user;
    Saved.password = password;
    SaveConfig();
    RestartServer();
}

void AddFolder(const std::wstring& folder) {
    for (const std::wstring& f : Saved.folders)
        if (!_wcsicmp(f.c_str(), folder.c_str())) return;
    Saved.folders.push_back(folder);
}

void FoldersChanged() {
    FillFolders();
    SaveConfig();
    if (Running || !Saved.folders.empty()) {
        StopServer(false);
        StartServer();
    }
}

void PickFolders() {
    IFileOpenDialog* dialog = NULL;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_ALLOWMULTISELECT | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"Folders with your Xbox 360 games");
    bool added = false;
    if (SUCCEEDED(dialog->Show(Window))) {
        IShellItemArray* items = NULL;
        DWORD count = 0;
        if (SUCCEEDED(dialog->GetResults(&items)) && SUCCEEDED(items->GetCount(&count))) {
            for (DWORD i = 0; i < count; ++i) {
                IShellItem* item = NULL;
                PWSTR path = NULL;
                if (SUCCEEDED(items->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    AddFolder(path);
                    added = true;
                }
                CoTaskMemFree(path);
                if (item) item->Release();
            }
        }
        if (items) items->Release();
    }
    dialog->Release();
    if (added) FoldersChanged();
}

void DropFolders(HDROP drop) {
    UINT count = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
    bool added = false;
    for (UINT i = 0; i < count; ++i) {
        std::wstring path(DragQueryFileW(drop, i, NULL, 0) + 1, L'\0');
        path.resize(DragQueryFileW(drop, i, &path[0], UINT(path.size())));
        DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            AddFolder(path);
            added = true;
        }
    }
    DragFinish(drop);
    if (added) FoldersChanged();
}

void Quit() {
    Quitting = true;
    ReadEdits();
    StopServer(false);
    Shell_NotifyIconW(NIM_DELETE, &Tray);
    DestroyWindow(Window);
}

LRESULT CALLBACK Procedure(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == TaskbarCreated && TaskbarCreated) {  // Explorer restarted.
        AddTray();
        UpdateStatus();
        return 0;
    }
    switch (message) {
    case WM_CREATE:
        Window = hwnd;
        CreateControls();
        DragAcceptFiles(hwnd, TRUE);
        return 0;
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) {
            HideToTray();
            return 0;
        }
        Layout();
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* info = (MINMAXINFO*)lParam;
        info->ptMinTrackSize.x = Px(560);
        info->ptMinTrackSize.y = Px(520);
        return 0;
    }
    case WM_DPICHANGED: {
        Dpi = HIWORD(wParam);
        MakeFonts();
        ApplyFonts();
        RECT* suggested = (RECT*)lParam;
        SetWindowPos(hwnd, NULL, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wParam;
        HWND control = (HWND)lParam;
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        if (control == StatusText) SetTextColor(dc, Running ? RGB(0x1B, 0x7F, 0x3B) : RGB(0x9A, 0x3B, 0x26));
        else if (control == FoldersHint || control == XboxHint || control == PcText) SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        else SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        return (LRESULT)Background;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam), code = HIWORD(wParam);
        if (id == IdFolders && code == LBN_SELCHANGE)
            EnableWindow(RemoveButton, SendMessageW(FolderList, LB_GETCURSEL, 0, 0) != LB_ERR);
        else if (id == IdAdd) PickFolders();
        else if (id == IdRemove) {
            LRESULT at = SendMessageW(FolderList, LB_GETCURSEL, 0, 0);
            if (at != LB_ERR && size_t(at) < Saved.folders.size()) {
                Saved.folders.erase(Saved.folders.begin() + at);
                FoldersChanged();
            }
        } else if (((id == IdXbox || id == IdUser || id == IdPassword) && code == EN_KILLFOCUS) || id == IDOK) ReadEdits();
        else if (id == IdBoot || id == IdMenuBoot) {
            bool on = StartupEntry().empty();
            SetStartWithWindows(on);
            Button_SetCheck(BootBox, on ? BST_CHECKED : BST_UNCHECKED);
        } else if (id == IdStart || id == IdMenuStart) {
            ReadEdits();
            if (Running) StopServer(true);
            else StartServer();
        } else if (id == IdQuit || id == IdMenuQuit) Quit();
        else if (id == IdMenuOpen) ShowMain();
        return 0;
    }
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
        std::wstring* line = (std::wstring*)lParam;
        AppendLog(*line);
        if (wParam == Generation) ReadLine(*line);
        delete line;
        return 0;
    }
    case WM_STOPPED:
        if (wParam == Generation && Running) {
            DWORD code = 0;
            WaitForSingleObject(Child, 2000);
            GetExitCodeProcess(Child, &code);
            CloseHandle(Child);
            Child = NULL;
            Running = false;
            AppendLog(L"The server stopped" + (code ? L" (exit code " + std::to_wstring(code) + L")." : std::wstring(L".")));
            UpdateStatus();
        }
        return 0;
    case WM_CLOSE:
        // Closing keeps the server running in the notification area; Quit ends it.
        ReadEdits();
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
    INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    LoadConfig();
    // Keep "Start with Windows" pointing at this exe if it was moved.
    std::wstring entry = StartupEntry();
    if (!entry.empty() && entry != RunCommand()) SetStartWithWindows(true);

    Job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(Job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

    LoadIcons();
    Background = GetSysColorBrush(COLOR_WINDOW);
    WNDCLASSEXW kind = {sizeof(kind)};
    kind.lpfnWndProc = Procedure;
    kind.hInstance = GetModuleHandleW(NULL);
    kind.hIcon = BigIcon;
    kind.hIconSm = SmallIcon;
    kind.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    kind.hbrBackground = Background;
    kind.lpszClassName = WindowClass;
    RegisterClassExW(&kind);
    TaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    // Created at the primary monitor's DPI, then sized for the one it's on.
    HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, WindowClass, AppName, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                CW_USEDEFAULT, CW_USEDEFAULT, NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!hwnd) return 1;
    Dpi = WindowDpi(hwnd);
    MakeFonts();
    ApplyFonts();
    RECT size = {0, 0, Px(680), Px(640)};
    AdjustWindowRectEx(&size, WS_OVERLAPPEDWINDOW, FALSE, 0);
    SetWindowPos(hwnd, NULL, 0, 0, size.right - size.left, size.bottom - size.top, SWP_NOMOVE | SWP_NOZORDER);
    Layout();
    AddTray();
    AppendLog(L"Settings: " + IniPath());
    StartServer();
    if (!tray) {
        ShowWindow(hwnd, SW_SHOWNORMAL);
        UpdateWindow(hwnd);
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
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

// Runs the window and returns its exit code, or returns -1 when the exe was
// started with options and should run as the command-line server.
int GuiMain(int argc, char** argv) {
    bool tray = argc == 2 && !strcmp(argv[1], "--tray");
    if (argc == 1 || tray) return RunWindow(tray);
    UseConsole();
    return -1;
}
#endif
