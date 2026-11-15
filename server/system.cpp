// SPDX-License-Identifier: MIT
#include "system.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <netlistmgr.h>
#include <shellapi.h>
#else
#include <errno.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <dirent.h>
#include <fstream>
#endif

namespace nia {
#ifdef _WIN32
void KeepAwake(bool on) {
    SetThreadExecutionState(on ? ES_CONTINUOUS | ES_SYSTEM_REQUIRED : ES_CONTINUOUS);
}

std::vector<std::string> NetworkCategories() {
    std::vector<std::string> out;
    HRESULT init = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    INetworkListManager* manager = NULL;
    if (SUCCEEDED(CoCreateInstance(CLSID_NetworkListManager, NULL, CLSCTX_ALL, IID_INetworkListManager, (void**)&manager))) {
        IEnumNetworks* networks = NULL;
        if (SUCCEEDED(manager->GetNetworks(NLM_ENUM_NETWORK_CONNECTED, &networks))) {
            INetwork* network = NULL;
            while (networks->Next(1, &network, NULL) == S_OK) {
                NLM_NETWORK_CATEGORY category;
                if (SUCCEEDED(network->GetCategory(&category)))
                    out.push_back(category == NLM_NETWORK_CATEGORY_PUBLIC ? "public" : category == NLM_NETWORK_CATEGORY_PRIVATE ? "private" : "domain");
                network->Release();
            }
            networks->Release();
        }
        manager->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return out;
}

bool MakeNetworksPrivate(std::string& error) {
    HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SHELLEXECUTEINFOW run = {};
    run.cbSize = sizeof(run);
    run.lpVerb = L"runas";
    run.lpFile = L"powershell.exe";
    run.lpParameters = L"-NoProfile -NonInteractive -WindowStyle Hidden -Command \"Get-NetConnectionProfile | "
                       L"Where-Object NetworkCategory -eq 'Public' | Set-NetConnectionProfile -NetworkCategory Private\"";
    run.nShow = SW_HIDE;
    bool asked = ShellExecuteExW(&run) != FALSE;
    if (!asked) error = GetLastError() == ERROR_CANCELLED ? "Windows' administrator prompt was turned down, so nothing changed."
                                                           : "Windows couldn't start PowerShell to change the network.";
    if (SUCCEEDED(init)) CoUninitialize();
    return asked;
}

namespace {
std::string Gb(unsigned long long n) {
    char t[32]; snprintf(t, sizeof(t), "%.1f GB", n / 1073741824.0);
    return t;
}
std::string RegText(const char* key, const char* name) {
    char value[256] = {};
    DWORD size = sizeof(value);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, key, name, RRF_RT_REG_SZ, NULL, value, &size) != ERROR_SUCCESS) return "";
    return value;
}
DWORD RegNumber(const char* key, const char* name) {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, key, name, RRF_RT_REG_DWORD, NULL, &value, &size) != ERROR_SUCCESS) return 0;
    return value;
}
std::string Trim(std::string s) {
    while (!s.empty() && (s[0] == ' ' || s[0] == '\t')) s.erase(0, 1);
    while (!s.empty() && (s[s.size() - 1] == ' ' || s[s.size() - 1] == '\t' || s[s.size() - 1] == '\n' || s[s.size() - 1] == '\r')) s.erase(s.size() - 1);
    return s;
}
std::string UserName() {
    char name[256] = {};
    DWORD size = sizeof(name);
    return GetUserNameA(name, &size) ? name : "";
}
}

std::vector<std::pair<std::string, std::string> > SystemFacts() {
    std::vector<std::pair<std::string, std::string> > out;
    const char* nt = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    // ProductName says Windows 10 on Windows 11 too; the build number tells them apart.
    std::string product = RegText(nt, "ProductName"), release = RegText(nt, "DisplayVersion");
    DWORD build = 0;
    typedef LONG(WINAPI * Version)(OSVERSIONINFOW*);
    if (Version get = (Version)(void*)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion")) {
        OSVERSIONINFOW v = {};
        v.dwOSVersionInfoSize = sizeof(v);
        if (!get(&v)) build = v.dwBuildNumber;
    }
    if (build >= 22000 && product.compare(0, 10, "Windows 10") == 0) product = "Windows 11" + product.substr(10);
    char text[256];
    snprintf(text, sizeof(text), "%s%s%s, build %lu.%lu", product.empty() ? "Windows" : product.c_str(), release.empty() ? "" : " ", release.c_str(),
             (unsigned long)build, (unsigned long)RegNumber(nt, "UBR"));
    out.push_back(std::make_pair(std::string("System"), std::string(text)));
    char computer[256] = {};
    DWORD size = sizeof(computer);
    if (GetComputerNameA(computer, &size)) out.push_back(std::make_pair(std::string("Computer"), std::string(computer)));
    std::string user = UserName();
    bool admin = false;
    HANDLE token;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION e;
        DWORD got;
        if (GetTokenInformation(token, TokenElevation, &e, sizeof(e), &got)) admin = e.TokenIsElevated != 0;
        CloseHandle(token);
    }
    out.push_back(std::make_pair(std::string("Runs as"), (user.empty() ? std::string("unknown user") : user) + (admin ? " (as administrator)" : "")));
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    std::string cpu = Trim(RegText("HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString"));
    snprintf(text, sizeof(text), "%s%s%lu threads", cpu.c_str(), cpu.empty() ? "" : ", ", (unsigned long)si.dwNumberOfProcessors);
    out.push_back(std::make_pair(std::string("Processor"), std::string(text)));
    MEMORYSTATUSEX m = {};
    m.dwLength = sizeof(m);
    if (GlobalMemoryStatusEx(&m)) out.push_back(std::make_pair(std::string("Memory"), Gb(m.ullAvailPhys) + " free of " + Gb(m.ullTotalPhys)));
    char exe[MAX_PATH] = {};
    if (GetModuleFileNameA(NULL, exe, MAX_PATH)) out.push_back(std::make_pair(std::string("Program"), std::string(exe)));
    char here[MAX_PATH] = {};
    if (GetCurrentDirectoryA(MAX_PATH, here)) out.push_back(std::make_pair(std::string("Started in"), std::string(here)));
    unsigned long long up = GetTickCount64() / 60000;
    snprintf(text, sizeof(text), "%llu d %llu h %llu min", up / 1440, up / 60 % 24, up % 60);
    out.push_back(std::make_pair(std::string("Up since start"), std::string(text)));
    return out;
}

std::string FolderProblem(const std::string& native) {
    DWORD a = GetFileAttributesA(native.c_str());
    DWORD error = a == INVALID_FILE_ATTRIBUTES ? GetLastError() : 0;
    if (!error && !(a & FILE_ATTRIBUTE_DIRECTORY)) return "that's a file, not a folder";
    if (!error) {
        // It's there; can it be listed?
        WIN32_FIND_DATAA f;
        HANDLE h = FindFirstFileA((native + (native.empty() || native[native.size() - 1] == '\\' ? "*" : "\\*")).c_str(), &f);
        if (h != INVALID_HANDLE_VALUE) { FindClose(h); return ""; }
        error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) return "";
    }
    switch (error) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: return "Windows finds no such folder";
    case ERROR_INVALID_DRIVE: return "there's no such drive";
    case ERROR_NOT_READY: return "the drive isn't ready (not connected?)";
    case ERROR_ACCESS_DENIED: return "access denied for Windows user " + UserName();
    case ERROR_BAD_NETPATH: case ERROR_BAD_NET_NAME: return "the network share isn't there";
    case ERROR_LOGON_FAILURE: return "the network share turned down the login";
    case ERROR_INVALID_NAME: return "that isn't a valid folder name";
    }
    char t[48]; snprintf(t, sizeof(t), "Windows error %lu", (unsigned long)error);
    return t;
}
#else
void KeepAwake(bool) {}
std::vector<std::string> NetworkCategories() { return std::vector<std::string>(); }
bool MakeNetworksPrivate(std::string& error) {
    error = "Only Windows has network categories.";
    return false;
}

namespace {
std::string Gb(unsigned long long n) {
    char t[32]; snprintf(t, sizeof(t), "%.1f GB", n / 1073741824.0);
    return t;
}
// The value after "key:" or "key=" on the first line of a file that starts with key.
std::string Field(const char* path, const std::string& key) {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.compare(0, key.size(), key) != 0) continue;
        std::string v = line.substr(key.size());
        while (!v.empty() && (v[0] == ' ' || v[0] == '\t' || v[0] == ':' || v[0] == '=' || v[0] == '"')) v.erase(0, 1);
        while (!v.empty() && (v[v.size() - 1] == '"' || v[v.size() - 1] == ' ')) v.erase(v.size() - 1);
        return v;
    }
    return "";
}
std::string UserName() {
    struct passwd* p = getpwuid(geteuid());
    char t[64]; snprintf(t, sizeof(t), "uid %d", int(geteuid()));
    return p && p->pw_name ? p->pw_name : t;
}
}

std::vector<std::pair<std::string, std::string> > SystemFacts() {
    std::vector<std::pair<std::string, std::string> > out;
    struct utsname u;
    std::string os = Field("/etc/os-release", "PRETTY_NAME");
    if (!uname(&u)) os += (os.empty() ? std::string() : std::string(", ")) + u.sysname + " " + u.release + " " + u.machine;
    out.push_back(std::make_pair(std::string("System"), os));
    char host[256] = {};
    if (!gethostname(host, sizeof(host) - 1)) out.push_back(std::make_pair(std::string("Computer"), std::string(host)));
    out.push_back(std::make_pair(std::string("Runs as"), UserName()));
    char text[256];
    std::string cpu = Field("/proc/cpuinfo", "model name");
    snprintf(text, sizeof(text), "%s%s%ld threads", cpu.c_str(), cpu.empty() ? "" : ", ", sysconf(_SC_NPROCESSORS_ONLN));
    out.push_back(std::make_pair(std::string("Processor"), std::string(text)));
    long long total = atoll(Field("/proc/meminfo", "MemTotal").c_str()), free = atoll(Field("/proc/meminfo", "MemAvailable").c_str());
    if (total) out.push_back(std::make_pair(std::string("Memory"), Gb((unsigned long long)free * 1024) + " free of " + Gb((unsigned long long)total * 1024)));
    char exe[4096] = {};
    if (readlink("/proc/self/exe", exe, sizeof(exe) - 1) > 0) out.push_back(std::make_pair(std::string("Program"), std::string(exe)));
    char here[4096] = {};
    if (getcwd(here, sizeof(here))) out.push_back(std::make_pair(std::string("Started in"), std::string(here)));
    return out;
}

std::string FolderProblem(const std::string& native) {
    struct stat st;
    if (stat(native.c_str(), &st)) {
        if (errno == ENOENT || errno == ENOTDIR) return "no such folder";
        if (errno == EACCES) return "access denied for user " + UserName();
        return strerror(errno);
    }
    if (!S_ISDIR(st.st_mode)) return "that's a file, not a folder";
    DIR* d = opendir(native.c_str());
    if (!d) return errno == EACCES ? "access denied for user " + UserName() : std::string(strerror(errno));
    closedir(d);
    return "";
}
#endif
}
