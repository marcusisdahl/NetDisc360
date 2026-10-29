// SPDX-License-Identifier: MIT
#include "system.h"
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <netlistmgr.h>
#include <shellapi.h>
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
#else
void KeepAwake(bool) {}
std::vector<std::string> NetworkCategories() { return std::vector<std::string>(); }
bool MakeNetworksPrivate(std::string& error) {
    error = "Only Windows has network categories.";
    return false;
}
#endif
}
