// SPDX-License-Identifier: MIT
// The Tasks page's list (see tasks.h).
#include "tasks.h"
#include "config.h"
#include "web.h"
#include <deque>
#include <map>
#include <mutex>
#include <time.h>

namespace tasks {
namespace {

struct Task {
    long long id, ref;
    std::string kind, name, where, state, detail, error;
    double done, total;
    long long started, ended;
    Live live;
};
struct Job {
    std::string name, where, off, error;
    double seconds;
    long long last, next;  // time(); 0: not yet / not known.
    bool ok;
    int runs, failures;
};

const size_t HistoryKeep = 200;
std::mutex Lock;
std::map<long long, Task> Running;
std::deque<Task> History;  // Newest last.
std::map<std::string, Job> Jobs;
long long NextId = 1;

long long Seconds() { return (long long)time(NULL); }

void Put(web::Json& j, const Task& t, long long now) {
    j.Open('{').Set("id", t.id).Set("kind", t.kind).Set("name", config::ToUtf8(t.name)).Set("where", config::ToUtf8(t.where));
    j.Set("ref", t.ref).Set("state", t.state).Set("detail", config::ToUtf8(t.detail)).Set("done", t.done).Set("total", t.total);
    j.Set("started", t.started).Set("ended", t.ended).Set("seconds", (t.ended ? t.ended : now) - t.started);
    j.Set("error", config::ToUtf8(t.error)).Close('}');
}

}

long long Begin(const std::string& kind, const std::string& name, const std::string& where, long long ref) {
    std::lock_guard<std::mutex> hold(Lock);
    Task t;
    t.id = NextId++; t.ref = ref; t.kind = kind; t.name = name; t.where = where; t.state = "running";
    t.done = 0; t.total = 0; t.started = Seconds(); t.ended = 0;
    Running[t.id] = t;
    return t.id;
}

void Step(long long id, const std::string& detail, double done, double total) {
    std::lock_guard<std::mutex> hold(Lock);
    std::map<long long, Task>::iterator t = Running.find(id);
    if(t == Running.end()) return;
    t->second.detail = detail; t->second.done = done; t->second.total = total;
}

void Follow(long long id, const Live& live) {
    std::lock_guard<std::mutex> hold(Lock);
    std::map<long long, Task>::iterator t = Running.find(id);
    if(t != Running.end()) t->second.live = live;
}

void End(long long id, bool ok, const std::string& error, bool stopped) {
    std::lock_guard<std::mutex> hold(Lock);
    std::map<long long, Task>::iterator t = Running.find(id);
    if(t == Running.end()) return;
    Task done = t->second;
    Running.erase(t);
    done.live = Live();
    done.state = stopped ? "stopped" : ok ? "done" : "failed";
    done.error = ok ? std::string() : error;
    done.ended = Seconds();
    History.push_back(done);
    while(History.size() > HistoryKeep) History.pop_front();
}

void Every(const std::string& key, const std::string& name, double seconds, const std::string& where) {
    std::lock_guard<std::mutex> hold(Lock);
    std::map<std::string, Job>::iterator found = Jobs.find(key);
    if(found != Jobs.end()) { found->second.name = name; found->second.seconds = seconds; found->second.where = where; return; }
    Job job;
    job.name = name; job.where = where; job.seconds = seconds; job.last = 0; job.next = 0; job.ok = true; job.runs = 0; job.failures = 0;
    Jobs[key] = job;
}

void Ran(const std::string& key, bool ok, const std::string& error, long long next) {
    std::lock_guard<std::mutex> hold(Lock);
    std::map<std::string, Job>::iterator found = Jobs.find(key);
    if(found == Jobs.end()) return;
    Job& job = found->second;
    job.last = Seconds(); job.ok = ok; job.error = ok ? std::string() : error; ++job.runs;
    if(!ok) ++job.failures;
    job.next = next ? next : job.seconds > 0 ? job.last + (long long)job.seconds : 0;
}

void Off(const std::string& key, const std::string& why) {
    std::lock_guard<std::mutex> hold(Lock);
    std::map<std::string, Job>::iterator found = Jobs.find(key);
    if(found != Jobs.end()) found->second.off = why;
}

void Counts(int& running, long long& lastFailed) {
    std::lock_guard<std::mutex> hold(Lock);
    running = int(Running.size());
    lastFailed = 0;
    for(size_t i = History.size(); i-- > 0 && !lastFailed;) if(History[i].state == "failed") lastFailed = History[i].ended;
}

void Write(web::Json& j, long long now) {
    std::lock_guard<std::mutex> hold(Lock);
    j.Key("running").Open('[');
    for(std::map<long long, Task>::iterator t = Running.begin(); t != Running.end(); ++t) {
        if(t->second.live) t->second.live(t->second.detail, t->second.done, t->second.total);
        Put(j, t->second, now);
    }
    j.Close(']').Key("history").Open('[');
    for(size_t i = History.size(); i-- > 0;) Put(j, History[i], now);
    j.Close(']').Key("scheduled").Open('[');
    for(std::map<std::string, Job>::const_iterator k = Jobs.begin(); k != Jobs.end(); ++k) {
        const Job& job = k->second;
        j.Open('{').Set("key", k->first).Set("name", config::ToUtf8(job.name)).Set("where", config::ToUtf8(job.where)).Set("every", job.seconds);
        j.Set("last", job.last).Set("next", job.next).Set("ok", job.ok).Set("error", config::ToUtf8(job.error));
        j.Set("runs", job.runs).Set("failures", job.failures).Set("off", config::ToUtf8(job.off)).Close('}');
    }
    j.Close(']');
}

}
