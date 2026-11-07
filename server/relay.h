// SPDX-License-Identifier: MIT
#pragma once
// Join from a phone: NetDisc360 posts the LAN party to its relay on
// Cloudflare (relay/, a Worker) and takes back what phones asked for there,
// as phones on the internet can't reach this PC on the LAN. This is the
// HTTP client for that: https on Windows (WinHTTP) and, in Docker, through
// curl; plain http everywhere (a relay on the LAN, and the tests).
#include <string>

namespace relay {
// Whether url is an address this can post to: http(s)://host[:port][/path],
// without spaces, quotes or a login; why not in error.
bool GoodUrl(const std::string& url, std::string& error);
// POSTs json to url with "Authorization: Bearer <key>"; the answer's status
// and body. false with why in error when there was no answer.
bool Post(const std::string& url, const std::string& key, const std::string& json, double seconds,
          int& status, std::string& body, std::string& error);
}
