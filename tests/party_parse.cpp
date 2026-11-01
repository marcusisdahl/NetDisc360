// SPDX-License-Identifier: MIT
// Reads the party as NetDiscParty.xex does (party/state.cpp), from the text
// on standard input, and prints what it read and where the wheel is at each
// moment given (milliseconds into the spin), for tests/test_server.py.
//   party-parse <ms> ... [tag=<gamertag>=<name> ...]
// A tag= argument prints whether the gamertag is that guest's (GamertagOf).
#include "../party/state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
    static char text[1 << 16];
    unsigned length = unsigned(fread(text, 1, sizeof(text), stdin));
    static party::State s;
    bool ok = party::Parse(text, length, s);
    printf("valid %d\nversion %llu\nconsole %s|%s|%d\nparty %d|%s|%s\nspin %u|%s|%d|%d|%d\nwheel %d\nsteps %d\nteams %d\nseats %d\n",
           ok, s.version, s.console, s.address, s.gamemaster, s.active, s.title, s.line, s.spinId, s.kind, s.leadMs, s.pauseMs, party::SpinMs(s),
           s.wheelCount, s.stepCount, s.teamCount, s.seatCount);
    const party::Seat* mine = party::MySeat(s);
    printf("mine %s|%d|%d\n", mine ? mine->console : "", mine ? mine->count : 0, mine && mine->count ? party::TeamOf(s, mine->names[0]) : -1);
    for (int i = 0; i < s.signedCount; ++i)
        printf("signin %d|%d|%s\n", s.signedIn[i].seat, s.signedIn[i].controller, s.signedIn[i].gamertag);
    for (int i = 1; i < argc; ++i) {
        if (!strncmp(argv[i], "tag=", 4)) {
            static char tag[256];
            snprintf(tag, sizeof(tag), "%s", argv[i] + 4);
            char* name = strchr(tag, '=');
            if (!name) continue;
            *name++ = 0;
            printf("tag %s|%s %d\n", tag, name, party::GamertagOf(tag, name));
            continue;
        }
        int step; bool stopped;
        double ms = atof(argv[i]), angle = party::SpinAngle(s, ms, &step, &stopped);
        printf("at %.0f %.3f %d %d %d\n", ms, angle, step, stopped, party::Stopped(s, ms));
    }
    return 0;
}
