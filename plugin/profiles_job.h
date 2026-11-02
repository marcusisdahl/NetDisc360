// SPDX-License-Identifier: MIT
#pragma once
// The LAN party's guest profiles made by this plugin, in the background
// while Aurora stays on screen (launcher/profiles.h does the making). Without
// it the server has Aurora start NetDisc360's helper to make them, so Aurora
// goes away for a few seconds on the console that makes them.
//
// The server leaves the job as NetDisc360.job on the hard drive's root
// (Hdd1:\, as ProfileJob in shared/helper.h describes its lines, with
// folder= naming the folder under Hdd1: that the gamer pictures are in). A
// thread here looks for it every couple of seconds, deletes it first (so a
// crash doesn't start it again), makes the profiles and writes
// NetDisc360.done next to it, as the helper writes ProfileDone, with by=plugin.
// The server waits a few seconds for the job to be taken; if it isn't (no
// plugin that does this), it starts the helper as before.
namespace profilejob {
typedef void (*NoteCall)(const char* format, ...);
typedef void (*ShowCall)(const char* text);
// Starts the watching thread. note logs (netiso.log), show puts a notice on screen.
void Start(NoteCall note, ShowCall show);
void Stop();
// The job's work alone, for NetDiscParty.xex too (which gets the job and its
// pictures from the server instead of files): makes the guestN= profiles
// (and gives profileN= ones their pictures), each picture read with read
// (its file name as the job names it; bytes read, or -1); writes
// ProfileDone's lines into done (by=by); made: how many it made. note logs.
typedef int (*ReadCall)(void* with, const char* name, void* into, unsigned long capacity);
size_t MakeFromJob(const char* job, ReadCall read, void* with, char* done, size_t capacity, const char* by, int* made);
void SetNote(NoteCall note);
}
