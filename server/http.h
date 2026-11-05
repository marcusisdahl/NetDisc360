// SPDX-License-Identifier: MIT
#pragma once
// An HTTP client for NetDisc360's services on the internet (the sync on
// Cloudflare, sync.h): https on Windows through WinHTTP and, in Docker,
// through curl; plain http everywhere (a service on the LAN, and the tests).
#include <string>

namespace http {
// Whether url is an address this can reach: http(s)://host[:port][/path],
// without spaces, quotes or a login; why not in error. what names the
// service in the error ("The sync's address ...").
bool GoodUrl(const std::string& url, const char* what, std::string& error);

struct Call {
    std::string method;       // GET, PUT, POST or DELETE.
    std::string url;          // As GoodUrl takes it, with any query.
    std::string key;          // "Authorization: Bearer <key>"; letters, digits, - and _ only.
    std::string type;         // Content-Type of body ("" for none).
    std::string body;
    std::string sha256;       // Sent as X-Sha256 when not "".
    double seconds;           // How long to wait for a silent server.
    size_t most;              // The largest answer to take.
    Call() : seconds(30), most(1 << 20) {}
};

// Makes the call; the answer's status and body. false with why in error when
// there was no whole answer (any status is an answer).
bool Send(const Call& call, int& status, std::string& body, std::string& error);
}
