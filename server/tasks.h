// SPDX-License-Identifier: MIT
#pragma once
// The Tasks page: what the server does by itself, in one list, as Sonarr and
// Radarr show theirs. Each piece of background work (a compression, a copy to
// a console, game folders written to a console, a save backup...) is a task
// while it runs and stays in the history after, with how long it took and,
// if it failed, why. Work done on a schedule (title update look-ups, backups,
// sync...) says when it last ran, how that went, and when it runs next.
//
// Text is native (as Log takes it); the page gets it as UTF-8.
#include "../shared/platform.h"
#include <functional>
#include <string>

namespace web { class Json; }

namespace tasks {

// A task that runs from Begin to End. kind groups them on the page
// ("compress", "console", "push", "saves", "sync"...); where: the console or
// folder it works on, or empty. ref: the id the kind's own list knows it by
// (a copy's id), 0 if none. Returns its id.
long long Begin(const std::string& kind, const std::string& name, const std::string& where = std::string(), long long ref = 0);
// What it does now ("Sending default.xex"), and how far (bytes, files...; 0 of 0: not known).
void Step(long long id, const std::string& detail, double done = 0, double total = 0);
// Where its progress is kept elsewhere (a copy's own fields): called as the
// page reads the list, to fill in detail, done and total. It must not call
// back into tasks.
typedef std::function<void(std::string& detail, double& done, double& total)> Live;
void Follow(long long id, const Live& live);
// ok false: it failed, with why in error. stopped: the user stopped it.
void End(long long id, bool ok, const std::string& error = std::string(), bool stopped = false);

// Ends as done when it goes out of scope, unless Fail or Stop said otherwise.
class Run {
public:
    Run(const std::string& kind, const std::string& name, const std::string& where = std::string(), long long ref = 0)
        : id_(Begin(kind, name, where, ref)), ok_(true), stopped_(false) {}
    ~Run() { End(id_, ok_, error_, stopped_); }
    void Step(const std::string& detail, double done = 0, double total = 0) { tasks::Step(id_, detail, done, total); }
    void Follow(const Live& live) { tasks::Follow(id_, live); }
    void Fail(const std::string& error) { ok_ = false; error_ = error; }
    void Done() { ok_ = true; error_.clear(); }
    void Stop() { stopped_ = true; }
    long long Id() const { return id_; }
private:
    Run(const Run&);
    Run& operator=(const Run&);
    long long id_;
    bool ok_, stopped_;
    std::string error_;
};

// Work on a schedule, by key ("tu-lookup", "installed:10.10.1.42"). Every
// declares it (seconds apart; 0: when something asks for it), Ran says it ran
// now (next: when it runs again, time(); 0: seconds from now), Off that it is
// turned off in the settings (why, for the page) or on again (empty).
void Every(const std::string& key, const std::string& name, double seconds, const std::string& where = std::string());
void Ran(const std::string& key, bool ok, const std::string& error = std::string(), long long next = 0);
void Off(const std::string& key, const std::string& why);

// For the sidebar: how many run now, and when the latest one failed (time(); 0: none).
void Counts(int& running, long long& lastFailed);

// {"running":[...],"history":[...],"scheduled":[...]} for /api/tasks; each
// task: id, kind, name, where, ref, state (running, done, failed, stopped),
// detail, done, total, started, ended (time()), seconds, error.
void Write(web::Json& j, long long now);

}
