// SPDX-License-Identifier: MIT
// Writes the gamer pictures guest profiles get (server/picture.cpp), to look
// at them, or to hand them to the helper by hand for a console test:
//   gamer-picture <out-folder> <gamertag> [<gamertag>...]
// writes guest1.png (64 by 64) and guest1_32.png for the first, and on, the
// names a profile job uses (ProfilePictureFile in server/guests.h).
//   g++ -std=c++11 -o gamer-picture tests/gamer_picture.cpp server/picture.cpp
//   cl /EHsc tests\gamer_picture.cpp server\picture.cpp
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <string>
#include "../server/picture.h"

int main(int argc, char** argv) {
    if(argc < 3) { fprintf(stderr, "gamer-picture <out-folder> <gamertag> [<gamertag>...]\n"); return 2; }
    for(int i = 2; i < argc; ++i)
        for(int size = 64; size >= 32; size -= 32) {
            char name[64];
            sprintf(name, "guest%d%s.png", i - 1, size == 32 ? "_32" : "");
            std::string path = std::string(argv[1]) + "/" + name, png = nia::GamerPicturePng(argv[i], size);
            FILE* f = fopen(path.c_str(), "wb");
            if(!f || fwrite(png.data(), 1, png.size(), f) != png.size()) { fprintf(stderr, "Couldn't write %s\n", path.c_str()); return 1; }
            fclose(f);
            printf("%s: %s (%s, #%06X), %u bytes\n", path.c_str(), argv[i], nia::GamertagInitials(argv[i]).c_str(), nia::GamerPictureColour(argv[i]), unsigned(png.size()));
        }
    return 0;
}
