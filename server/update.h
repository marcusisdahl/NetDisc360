// SPDX-License-Identifier: MIT
#pragma once
// Updates of NetDisc360 from its GitHub releases. The server asks GitHub's
// API for the newest release. On Windows it can then download that
// release's NetDisc360.exe, check it against the release, and put it in place of
// the running exe, after which tray.cpp starts the new one. The Docker image
// is updated by pulling it, so there the dashboard only says that a new one
// is out (it asks GitHub itself, as the Linux server has no HTTPS).
#include <string>

namespace update {
struct Release {
    std::string tag, version, name, notes, page, published;  // version: the tag without its v.
    std::string exeUrl, exeDigest;  // Its NetDisc360.exe; digest "sha256:<hex>" when GitHub gives one.
    long long exeSize;
    bool prerelease;
    Release() : exeSize(0), prerelease(false) {}
};

// <0, 0 or >0 as version a is older than, the same as or newer than b:
// "1.2.0", "v1.10.0", "1.2.0-beta.2" (a pre-release is older than its release).
int Compare(const std::string& a, const std::string& b);
// The newest release in GitHub's list of releases (its JSON), leaving out
// drafts, and pre-releases unless asked for; false with why in error.
bool Newest(const std::string& json, bool prereleases, Release& out, std::string& error);
// GitHub's list of releases, as Newest reads it.
std::string ReleasesUrl();

// Whether this build can fetch from GitHub itself (Windows).
bool CanFetch();
// GET over HTTPS, following redirects, of at most limit bytes. progress, if
// not NULL, is told the bytes so far and the total (0 if not known).
typedef void (*Progress)(long long got, long long total);
bool Fetch(const std::string& url, std::string& body, long long limit, Progress progress, std::string& error);
// Checks a downloaded NetDisc360.exe against its release: its size, its
// SHA-256, and that it is a 64-bit Windows program.
bool CheckExe(const std::string& data, const Release& r, std::string& error);
// Whether url is a download from this project's releases on GitHub.
bool FromRepo(const std::string& url);
}
