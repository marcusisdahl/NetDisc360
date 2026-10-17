// SPDX-License-Identifier: MIT
#pragma once
// HTTP GET for the game art netiso-server gives Aurora (server/art.cpp).
// WinHTTP on Windows, so https, redirects and the system proxy work; plain
// http over sockets elsewhere, which is all the host tests need.
#include <string>
#include <vector>

namespace nia {
// The body of url into body (at most limit bytes); false with error set if
// the request failed or the status wasn't 200. status is 0 when nothing answered.
bool HttpGet(const std::string& url, std::vector<unsigned char>& body, size_t limit, int& status, std::string& error);
}
