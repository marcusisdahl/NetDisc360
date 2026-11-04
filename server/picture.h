// SPDX-License-Identifier: MIT
#pragma once
// The gamer picture a LAN party guest's profile gets: the guest's initials,
// in white, on a colour of their own (the same colour for the same name
// every time), as the PNG the console keeps in the profile (tile_64.png and
// tile_32.png, see ProfileJob in shared/helper.h). On its own, with no
// other part of the server, so a test can make one too.
#include <string>

namespace nia {
// The initials shown for a gamertag: the first letter of its first word and
// of its last ("Kristoffer H": "KH"; "Guest 2": "G2"), or of its only word
// ("Marcus": "M"), in upper case. "?" for a gamertag with no letters.
std::string GamertagInitials(const std::string& gamertag);
// The picture for gamertag, size by size pixels (64 or 32 on the console):
// a PNG file's bytes.
std::string GamerPicturePng(const std::string& gamertag, int size);
// Its background colour, as 0xRRGGBB (the dashboard shows it beside the name).
unsigned GamerPictureColour(const std::string& gamertag);
}
