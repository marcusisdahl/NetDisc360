// SPDX-License-Identifier: MIT
#pragma once
// The dashboard of NetDisc 360: a small web server for its page
// (web/index.html, built into web_page.h by tools/make_web.py) and the JSON
// the page reads and sends, the same in NetDisc.exe and in Docker. The API
// itself (what /api/... answers) is in main.cpp.
//
// With a password, every /api/ request but logging in needs the cookie that
// logging in gives. Without one, the dashboard only answers to addresses and
// local names (not to example.com pointed at this PC), and every change must
// come from the page itself (an X-NetDisc header, which another site can't
// send), so other web pages can't use it.
#include "../shared/platform.h"
#include <string>
#include <utility>
#include <vector>

namespace web {

// JSON ------------------------------------------------------------------------

std::string Quote(const std::string& text);  // As a JSON string; text is UTF-8.

// Writes JSON: j.Open('{').Set("games",12).Key("names").Open('[').String("A").Close(']').Close('}').
class Json {
public:
    Json() : first_(true) {}
    Json& Open(char bracket) { Comma(); text_+=bracket; first_=true; return *this; }
    Json& Close(char bracket) { text_+=bracket; first_=false; return *this; }
    Json& Key(const char* key) { Comma(); text_+=Quote(key); text_+=':'; first_=true; return *this; }
    Json& String(const std::string& value) { Comma(); text_+=Quote(value); return *this; }
    Json& Number(double value);
    Json& Int(long long value);
    Json& Bool(bool value) { Comma(); text_+=value ? "true" : "false"; return *this; }
    Json& Null() { Comma(); text_+="null"; return *this; }
    Json& Set(const char* key, const std::string& value) { return Key(key).String(value); }
    Json& Set(const char* key, const char* value) { return Key(key).String(value); }
    Json& Set(const char* key, int value) { return Key(key).Int(value); }
    Json& Set(const char* key, long long value) { return Key(key).Int(value); }
    Json& Set(const char* key, unsigned long long value) { return Key(key).Number(double(value)); }
    Json& Set(const char* key, double value) { return Key(key).Number(value); }
    Json& Set(const char* key, bool value) { return Key(key).Bool(value); }
    const std::string& Text() const { return text_; }
private:
    void Comma() { if(!first_) text_+=','; first_=false; }
    std::string text_;
    bool first_;
};

// What the page sends.
struct Value {
    enum Type { Null, Bool, Number, String, Array, Object };
    Type type;
    bool boolean;
    double number;
    std::string text;
    std::vector<Value> items;
    std::vector<std::pair<std::string,Value> > fields;
    Value() : type(Null), boolean(false), number(0) {}
    const Value* Get(const char* key) const;  // NULL if it isn't there.
};
bool Parse(const std::string& text, Value& out);

// The server --------------------------------------------------------------------

struct Request {
    std::string method, path, query, body;  // path without the query.
    std::string Param(const char* name) const;  // A query parameter, decoded.
};

struct Reply {
    int status;
    std::string type, body;
    std::string download;  // A file name: the browser saves the body as that file.
    Reply() : status(200), type("application/json") {}
    void Error(int code, const std::string& message);  // {"error": message}
};

// Answers /api/... for the page (after the password, if there is one).
typedef void (*Api)(const Request& request, Reply& reply);

// Starts serving on address:port (127.0.0.1: this PC only; 0.0.0.0: the
// network too), or moves there if it was already serving. False with the
// reason in error when it can't listen.
bool Start(const std::string& address, int port, Api api, std::string& error);

// The password: stored only as salt and hash ("" for none).
std::string HashPassword(const std::string& password);
void SetPasswordHash(const std::string& hash);
std::string PasswordHash();
// A secret of this server's, kept out of the settings file and its backups,
// that sessions are signed with too; NewSecret makes one.
void SetSessionSecret(const std::string& secret);
std::string NewSecret();

std::string Sha256Hex(const std::string& data);  // In lower-case hex.
}
