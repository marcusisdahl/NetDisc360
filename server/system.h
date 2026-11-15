// SPDX-License-Identifier: MIT
#pragma once
// What NetDisc360 asks of Windows itself: staying awake while a console
// plays, and how Windows sees the network the consoles are on. Elsewhere
// (Linux, Docker) there's nothing to do, and nothing to report.
#include <string>
#include <utility>
#include <vector>

namespace nia {
// Keeps the computer from sleeping while on (a game would lose its disc).
// The state belongs to the calling thread, so it's always the same one.
void KeepAwake(bool on);

// The connected networks' Windows categories: "public", "private" or
// "domain". A Public network's firewall rules keep the consoles out.
std::vector<std::string> NetworkCategories();
// Asks Windows, with its administrator prompt on this computer, to make the
// connected networks that are Public Private; returns once the prompt is
// answered. False with why if it was turned down or couldn't be shown.
bool MakeNetworksPrivate(std::string& error);

// This computer, for the log and the About page: name and value pairs such as
// ("Windows", "Windows 11 Pro 24H2, build 26100"), ("Memory", "7.9 GB free of 31.9 GB").
std::vector<std::pair<std::string, std::string> > SystemFacts();
// Why a folder (a native path) can't be opened, in a few words, like "no such
// folder" or "access denied for Windows user marcu"; empty when it opens.
std::string FolderProblem(const std::string& native);
}
