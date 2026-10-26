// SPDX-License-Identifier: MIT
#include "wake.h"
#include "../shared/isvr.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#ifdef _WIN32
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
typedef int AddressLength;
#else
#include <ifaddrs.h>
#include <net/if.h>
#ifdef __linux__
#include <netpacket/packet.h>
#else
#include <net/if_dl.h>
#endif
typedef socklen_t AddressLength;
#endif

namespace wake {
namespace {
std::string MacText(const unsigned char* bytes) {
    char text[18];
    snprintf(text,sizeof(text),"%02X:%02X:%02X:%02X:%02X:%02X",bytes[0],bytes[1],bytes[2],bytes[3],bytes[4],bytes[5]);
    return text;
}
bool Blank(const unsigned char* bytes) {
    for(int i=0;i<6;++i) if(bytes[i]) return false;
    return true;
}
// This computer's address that a connection toward goes out from: a UDP
// socket's connect only picks the route, nothing is sent.
DWORD LocalAddress(DWORD toward) {
    SOCKET s=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(s==INVALID_SOCKET) return 0;
    sockaddr_in to; memset(&to,0,sizeof(to));
    to.sin_family=AF_INET; to.sin_port=htons(9); to.sin_addr.s_addr=toward;
    sockaddr_in own; memset(&own,0,sizeof(own));
    AddressLength length=sizeof(own);
    DWORD address=0;
    if(!connect(s,(sockaddr*)&to,sizeof(to)) && !getsockname(s,(sockaddr*)&own,&length)) address=own.sin_addr.s_addr;
    closesocket(s);
    return address;
}
}

bool Adapter(DWORD toward, std::string& mac, DWORD& address, DWORD& broadcast) {
    address=LocalAddress(toward);
    if(!address || (ntohl(address)>>24)==127) return false;
#ifdef _WIN32
    ULONG size=16384;
    std::vector<BYTE> buffer(size);
    IP_ADAPTER_ADDRESSES* list=(IP_ADAPTER_ADDRESSES*)buffer.data();
    ULONG flags=GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER;
    if(GetAdaptersAddresses(AF_INET,flags,NULL,list,&size)==ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        list=(IP_ADAPTER_ADDRESSES*)buffer.data();
    }
    if(GetAdaptersAddresses(AF_INET,flags,NULL,list,&size)!=NO_ERROR) return false;
    for(IP_ADAPTER_ADDRESSES* a=list;a;a=a->Next) {
        if(a->PhysicalAddressLength!=6 || Blank(a->PhysicalAddress)) continue;
        for(IP_ADAPTER_UNICAST_ADDRESS* u=a->FirstUnicastAddress;u;u=u->Next) {
            if(((sockaddr_in*)u->Address.lpSockaddr)->sin_addr.s_addr!=address) continue;
            int bits=u->OnLinkPrefixLength>32 ? 24 : u->OnLinkPrefixLength;
            DWORD mask=bits ? 0xFFFFFFFFu<<(32-bits) : 0;
            broadcast=htonl(ntohl(address)|~mask);
            mac=MacText(a->PhysicalAddress);
            return true;
        }
    }
    return false;
#else
    struct ifaddrs* list;
    if(getifaddrs(&list)) return false;
    std::string name;
    for(struct ifaddrs* a=list;a;a=a->ifa_next) {
        if(!a->ifa_addr || a->ifa_addr->sa_family!=AF_INET || ((sockaddr_in*)a->ifa_addr)->sin_addr.s_addr!=address) continue;
        name=a->ifa_name;
        DWORD mask=a->ifa_netmask ? ((sockaddr_in*)a->ifa_netmask)->sin_addr.s_addr : htonl(0xFFFFFF00u);
        broadcast=address|~mask;
        break;
    }
    bool found=false;
    for(struct ifaddrs* a=list;a && !name.empty() && !found;a=a->ifa_next) {
        if(!a->ifa_addr || name!=a->ifa_name) continue;
#ifdef __linux__
        if(a->ifa_addr->sa_family!=AF_PACKET) continue;
        const sockaddr_ll* link=(const sockaddr_ll*)a->ifa_addr;
        if(link->sll_halen!=6 || Blank(link->sll_addr)) continue;
        mac=MacText(link->sll_addr);
#else
        if(a->ifa_addr->sa_family!=AF_LINK) continue;
        const sockaddr_dl* link=(const sockaddr_dl*)a->ifa_addr;
        if(link->sdl_alen!=6 || Blank((const unsigned char*)LLADDR(link))) continue;
        mac=MacText((const unsigned char*)LLADDR(link));
#endif
        found=true;
    }
    freeifaddrs(list);
    return found;
#endif
}

std::string CleanMac(const std::string& text) {
    unsigned char bytes[6];
    int count=0, digits=0, value=0;
    for(size_t i=0;i<text.size();++i) {
        char c=text[i];
        if(isxdigit((unsigned char)c)) {
            value=value*16+(isdigit((unsigned char)c) ? c-'0' : (toupper((unsigned char)c)-'A'+10));
            if(++digits==2) { if(count==6) return std::string(); bytes[count++]=(unsigned char)value; digits=0; value=0; }
        } else if((c==':' || c=='-' || c=='.' || c==' ') && !digits) continue;
        else return std::string();
    }
    if(count!=6 || digits || Blank(bytes)) return std::string();
    return MacText(bytes);
}

std::string Ini(const std::string& mac, DWORD address, DWORD broadcast) {
    char at[16], all[16];
    nia::AddressText(address,at,sizeof(at));
    nia::AddressText(broadcast,all,sizeof(all));
    return "mac="+mac+"\r\naddress="+at+"\r\nbroadcast="+all+"\r\n";
}
}
