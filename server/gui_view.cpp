// SPDX-License-Identifier: MIT
// The pages of NetDisc 360's window, drawn with Dear ImGui: a sidebar with the
// pages, a dashboard that shows what the server and each console are doing
// (with how fast each one streams), the games as a table, and the settings.
// Everything here runs on the window's thread, once per frame (gui.cpp draws
// only when something changed, and once a second while the numbers move).
#ifdef _WIN32
#include "gui.h"
#include "../third_party/imgui-1.92/imgui.h"
#include "../third_party/imgui-1.92/imgui_internal.h"
#include "../third_party/lucide/lucide_icons.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <algorithm>

namespace gui {
namespace {
enum Page { PageDashboard, PageGames, PageFolders, PageConsoles, PageLog, PageSettings };
const char RepositoryUrl[] = "https://github.com/marcusisdahl/NetDisc-360";
const char InstallUrl[] = "https://github.com/marcusisdahl/NetDisc-360#1-set-up-the-console";
const float SidebarWidth = 208;

struct Palette {
    ImU32 sidebar, content, line, card, cardLine, field, fieldLine, text, muted, faint, accent, onAccent, accentSoft, hover, warning,
        success, danger;
};
Palette C;

int Current = PageDashboard;
char SearchText[128];
std::string Details;  // Path of the game shown in the details window.
bool ShowPassword;
bool FieldsLoaded;
char XboxText[256], UserText[128], PasswordText[128], OptionsText[1024];
bool LogAtEnd = true;
size_t LogSeen;
double CopiedAt = -10;  // When a copy button was last pressed, and which, for "Copied".
std::string CopiedId;

float S(float v) { return floorf(v * Scale + 0.5f); }
ImU32 Rgb(unsigned hex, float alpha = 1) {
    return IM_COL32((hex >> 16) & 255, (hex >> 8) & 255, hex & 255, int(alpha * 255 + 0.5f));
}
ImU32 Fade(ImU32 color, float alpha) {
    return (color & 0x00FFFFFF) | (ImU32(((color >> 24) & 255) * alpha + 0.5f) << 24);
}
ImU32 Mix(ImU32 a, ImU32 b, float t) {
    ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t));
}
ImU32 Wash(ImU32 color) { return Fade(color, Dark ? 0.18f : 0.13f); }  // A light background in a color.
ImVec2 operator+(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }
ImVec2 operator-(ImVec2 a, ImVec2 b) { return ImVec2(a.x - b.x, a.y - b.y); }

// Text and icons ----------------------------------------------------------------

float TextSize() { return S(14); }
ImVec2 Measure(ImFont* font, float size, const char* text) { return font->CalcTextSizeA(size, FLT_MAX, 0, text); }

// text shortened with an ellipsis to fit width.
std::string Elide(ImFont* font, float size, const std::string& text, float width) {
    if (Measure(font, size, text.c_str()).x <= width) return text;
    const char* dots = font->IsGlyphInFont(0x2026) ? "\xe2\x80\xa6" : "...";
    float room = width - Measure(font, size, dots).x;
    size_t n = text.size();
    while (n > 0) {
        do --n; while (n > 0 && (text[n] & 0xC0) == 0x80);  // Whole UTF-8 characters.
        if (Measure(font, size, text.substr(0, n).c_str()).x <= room) break;
    }
    return text.substr(0, n) + dots;
}

void Write(ImDrawList* draw, ImFont* font, float size, ImVec2 at, ImU32 color, const std::string& text) {
    draw->AddText(font, size, ImVec2(floorf(at.x), floorf(at.y)), color, text.c_str());
}

// Text wrapped at width, and how tall that is.
float Wrapped(ImDrawList* draw, ImFont* font, float size, ImVec2 at, ImU32 color, const std::string& text, float width) {
    draw->AddText(font, size, ImVec2(floorf(at.x), floorf(at.y)), color, text.c_str(), NULL, width);
    return font->CalcTextSizeA(size, FLT_MAX, width, text.c_str()).y;
}
float WrappedHeight(ImFont* font, float size, const std::string& text, float width) { return font->CalcTextSizeA(size, FLT_MAX, width, text.c_str()).y; }

// An icon centered on center.
void Icon(ImDrawList* draw, const char* icon, ImVec2 center, float size, ImU32 color) {
    unsigned int point = 0;
    ImTextCharFromUtf8(&point, icon, NULL);
    ImFontGlyph* glyph = FontIcons->GetFontBaked(size)->FindGlyphNoFallback(ImWchar(point));
    if (!glyph) return;
    ImVec2 at(center.x - (glyph->X0 + glyph->X1) / 2, center.y - (glyph->Y0 + glyph->Y1) / 2);
    draw->AddText(FontIcons, size, ImVec2(floorf(at.x + 0.5f), floorf(at.y + 0.5f)), color, icon);
}

// A turning arc, for "loading".
void Spinner(ImDrawList* draw, ImVec2 center, float radius, ImU32 color) {
    float t = float(ImGui::GetTime()) * 5.0f;
    draw->PathArcTo(center, radius, t, t + IM_PI * 1.4f, 24);
    draw->PathStroke(color, 0, S(2.5f));
    WantFrameAfter(16);
}

// Numbers and times ------------------------------------------------------------

std::string Gib(unsigned long long bytes) {
    char text[32];
    _snprintf_s(text, sizeof(text), _TRUNCATE, "%.2f GiB", bytes / 1073741824.0);
    return text;
}

std::string Bytes(unsigned long long bytes) {
    char text[32];
    if (bytes >= 1099511627776ULL) _snprintf_s(text, sizeof(text), _TRUNCATE, "%.2f TiB", bytes / 1099511627776.0);
    else if (bytes >= 1073741824ULL) _snprintf_s(text, sizeof(text), _TRUNCATE, "%.1f GiB", bytes / 1073741824.0);
    else if (bytes >= 1048576ULL) _snprintf_s(text, sizeof(text), _TRUNCATE, "%.0f MiB", bytes / 1048576.0);
    else if (bytes) _snprintf_s(text, sizeof(text), _TRUNCATE, "%.0f KiB", std::max(1.0, bytes / 1024.0));
    else strcpy_s(text, "0 MiB");
    return text;
}

std::string Rate(float bytesPerSecond) {
    char text[32];
    if (bytesPerSecond >= 1048576) _snprintf_s(text, sizeof(text), _TRUNCATE, "%.1f MiB/s", bytesPerSecond / 1048576.0);
    else if (bytesPerSecond >= 1024) _snprintf_s(text, sizeof(text), _TRUNCATE, "%.0f KiB/s", bytesPerSecond / 1024.0);
    else strcpy_s(text, "0 MiB/s");
    return text;
}

std::string Hex(DWORD titleId) {
    char text[12];
    _snprintf_s(text, sizeof(text), _TRUNCATE, "%08X", (unsigned)titleId);
    return text;
}

std::string Plural(long long count, const char* one, const char* many) { return std::to_string(count) + " " + (count == 1 ? one : many); }

// How long ago a time() was.
std::string Ago(long long then) {
    long long seconds = (long long)time(NULL) - then;
    if (seconds < 90) return "just now";
    if (seconds < 3600) return std::to_string(seconds / 60) + " min ago";
    if (seconds < 86400) return Plural(seconds / 3600, "hour", "hours") + " ago";
    if (seconds < 2 * 86400) return "yesterday";
    return std::to_string(seconds / 86400) + " days ago";
}

// A time() as the clock showed it; with the day if that wasn't today.
std::string ClockAt(long long then) {
    time_t at = (time_t)then, now = time(NULL);
    struct tm local, today;
    if (localtime_s(&local, &at) || localtime_s(&today, &now)) return "";
    char text[32];
    strftime(text, sizeof(text), local.tm_yday == today.tm_yday && local.tm_year == today.tm_year ? "%H:%M" : "%d %b %H:%M", &local);
    return text;
}

const char* KindName(nia::DiscKind kind) { return kind == nia::DiscGod ? "GOD" : kind == nia::DiscFolder ? "Folder" : "ISO"; }

std::string FirstAddress() { return ThisPc.substr(0, ThisPc.find(',')); }

// The consoles ----------------------------------------------------------------------

// A console for the pages: what the server says about it now, and when it was
// last connected (kept between runs).
struct ConsoleView {
    std::string address;
    const ConsoleState* state;  // NULL: it hasn't connected since the server started.
    long long seen;             // time(); 0: not known.
    int color;                  // Its color in the charts.
};

std::vector<ConsoleView> ConsoleList() {
    std::vector<ConsoleView> list;
    for (const ConsoleState& c : Server.consoles) {
        ConsoleView view = {c.address, &c, 0, 0};
        list.push_back(view);
    }
    for (const KnownConsole& known : Saved.consoles) {
        bool found = false;
        for (ConsoleView& view : list)
            if (view.address == known.address) {
                view.seen = known.seen;
                found = true;
            }
        if (!found) {
            ConsoleView view = {known.address, NULL, known.seen, 0};
            list.push_back(view);
        }
    }
    for (size_t i = 0; i < list.size(); ++i) list[i].color = int(i);
    return list;
}

bool Connected(const ConsoleState* c) { return Server.running && c && c->connections > 0; }
bool Playing(const ConsoleState* c) { return Connected(c) && !c->mounted.empty(); }

ImU32 ConsoleColor(int index) {
    static const unsigned dark[] = {0, 0xe0a33a, 0xb48ef0, 0x5cbf6a, 0xf07fb0, 0x6fa8ff};
    static const unsigned light[] = {0, 0xb7791f, 0x7b4fc9, 0x1e8a3a, 0xc2185b, 0x2f6fd6};
    index %= 6;
    return index == 0 ? C.accent : Rgb(Dark ? dark[index] : light[index]);
}

int Slot(long long second) { return int(((second % 120) + 120) % 120); }

// A console's rate in one second. A second without a line between two with
// one (the window's clock and the server's don't tick together) gets their
// average, and the current second its previous one until its line comes.
float Sample(const ConsoleState& c, long long second) {
    if (c.rateSecond[Slot(second)] == second) return c.rate[Slot(second)];
    bool before = c.rateSecond[Slot(second - 1)] == second - 1, after = c.rateSecond[Slot(second + 1)] == second + 1;
    if (before && after) return (c.rate[Slot(second - 1)] + c.rate[Slot(second + 1)]) / 2;
    if (before && second == Second()) return c.rate[Slot(second - 1)];
    return 0;
}

float TotalRate() {
    float total = 0;
    long long now = Second();
    for (const ConsoleState& c : Server.consoles) total += Sample(c, now);
    return total;
}

unsigned long long TotalSent() {
    unsigned long long total = Server.sentBefore;
    for (const ConsoleState& c : Server.consoles) total += c.sent;
    return total;
}

// Whether the server keeps this console's game folders up to date (Saved.xbox).
bool KeptUpToDate(const std::string& address) {
    if (Saved.xbox.empty()) return true;
    std::string list = Utf8(Saved.xbox);
    for (char& c : list)
        if (c == ',' || c == ';') c = ' ';
    size_t at = 0;
    while ((at = list.find_first_not_of(' ', at)) != std::string::npos) {
        size_t end = list.find(' ', at);
        if (list.compare(at, end == std::string::npos ? std::string::npos : end - at, address) == 0) return true;
        at = end;
    }
    return false;
}

const Game* GameNamed(const std::string& name) {
    for (const Game& g : Games.games)
        if (!_stricmp(g.name.c_str(), name.c_str())) return &g;
    return NULL;
}

// The consoles playing a game, as "10.0.0.5" or "10.0.0.5, 10.0.0.6".
std::string PlayingOn(const Game& game) {
    std::string on;
    for (const ConsoleState& c : Server.consoles)
        if (Playing(&c) && !_stricmp(c.mounted.c_str(), game.name.c_str())) on += (on.empty() ? "" : ", ") + c.address;
    return on;
}

// Widgets ------------------------------------------------------------------------

void Tip(const char* text) {
    if (!text || !ImGui::IsItemHovered()) return;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", text);
    else WantFrameAfter(100);
}

// A pointing hand over things that can be clicked.
bool Clicked() {
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return ImGui::IsItemClicked(ImGuiMouseButton_Left);
}

void At(float x, float y) { ImGui::SetCursorScreenPos(ImVec2(x, y)); }

bool RoundButton(const char* icon, const char* tip, bool busy = false) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 at = ImGui::GetCursorScreenPos();
    float size = S(34);
    ImGui::PushID(icon);
    ImGui::InvisibleButton("round", ImVec2(size, size));
    bool clicked = Clicked();
    bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    ImGui::PopID();
    ImVec2 center = at + ImVec2(size / 2, size / 2);
    ImU32 fill = held ? Mix(C.accent, Dark ? 0xFF000000 : 0xFFFFFFFF, 0.2f) : hovered ? Mix(C.accent, Dark ? 0xFFFFFFFF : 0xFF000000, 0.12f) : C.accent;
    draw->AddCircleFilled(center, size / 2, fill, 40);
    if (busy) Spinner(draw, center, S(8), C.onAccent);
    else Icon(draw, icon, center, S(17), C.onAccent);
    Tip(tip);
    return clicked;
}

// A small square button with an icon, for rows.
bool IconButton(const char* id, const char* icon, const char* tip, ImU32 color = 0) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 at = ImGui::GetCursorScreenPos();
    float size = S(28);
    ImGui::InvisibleButton(id, ImVec2(size, size));
    bool clicked = Clicked();
    if (ImGui::IsItemHovered()) draw->AddRectFilled(at, at + ImVec2(size, size), C.hover, S(6));
    Icon(draw, icon, at + ImVec2(size / 2, size / 2), S(16), color ? color : C.muted);
    Tip(tip);
    return clicked;
}

bool JustCopied(const char* id) {
    if (CopiedId != id || ImGui::GetTime() - CopiedAt > 1.5) return false;
    WantFrameAfter(200);
    return true;
}

void Copy(const char* id, const std::string& text) {
    ImGui::SetClipboardText(text.c_str());
    CopiedId = id;
    CopiedAt = ImGui::GetTime();
}

// An icon button that copies text, and shows a check for a moment.
void CopyButton(const char* id, const std::string& text, const char* tip) {
    bool copied = JustCopied(id);
    if (IconButton(id, copied ? ICON_Check : ICON_Copy, copied ? "Copied" : tip, copied ? C.success : 0)) Copy(id, text);
}

float ButtonWidth(const char* icon, const char* label) {
    return Measure(FontText, TextSize(), label).x + S(icon ? 46.0f : 28.0f);
}

// A rounded button with a label: filled with the accent color when primary.
bool Button(const char* id, const char* icon, const char* label, bool primary = false, bool enabled = true) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 at = ImGui::GetCursorScreenPos();
    ImVec2 size(ButtonWidth(icon, label), S(32));
    ImGui::PushID(id);
    ImGui::InvisibleButton("button", size);
    bool clicked = enabled && Clicked();
    bool hovered = enabled && ImGui::IsItemHovered();
    ImGui::PopID();
    ImU32 fill = primary && enabled ? C.accent : C.card, ink = primary ? C.onAccent : C.text;
    if (hovered) fill = Mix(fill, primary ? (Dark ? 0xFFFFFFFF : 0xFF000000) : C.hover, primary ? 0.12f : 1.0f);
    if (!enabled) ink = C.faint;
    draw->AddRectFilled(at, at + size, fill, S(7));
    if (!primary || !enabled) draw->AddRect(at, at + size, C.fieldLine, S(7), 0, S(1));
    float x = at.x + S(14);
    if (icon) {
        Icon(draw, icon, ImVec2(x + S(8), at.y + size.y / 2), S(16), ink);
        x += S(22);
    }
    Write(draw, FontText, TextSize(), ImVec2(x, at.y + (size.y - TextSize()) / 2 - S(1)), ink, label);
    return clicked;
}

bool Toggle(const char* id, bool on) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 at = ImGui::GetCursorScreenPos();
    ImVec2 size(S(40), S(22));
    ImGui::InvisibleButton(id, size);
    bool clicked = Clicked();
    draw->AddRectFilled(at, at + size, on ? C.accent : C.field, size.y / 2);
    if (!on) draw->AddRect(at, at + size, C.fieldLine, size.y / 2, 0, S(1));
    float r = size.y / 2 - S(4);
    ImVec2 knob(on ? at.x + size.x - size.y / 2 : at.x + size.y / 2, at.y + size.y / 2);
    draw->AddCircleFilled(knob, r, on ? C.onAccent : C.muted, 24);
    return clicked;
}

// A setting's switch with its label and an explanation under it.
bool Switch(const char* id, const char* label, const char* note, bool on) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 at = ImGui::GetCursorScreenPos();
    bool clicked = Toggle(id, on);
    Write(draw, FontText, TextSize(), ImVec2(at.x + S(52), at.y + S(2)), C.text, label);
    if (note) {
        ImGui::SetCursorScreenPos(ImVec2(at.x + S(52), at.y + S(24)));
        ImGui::PushStyleColor(ImGuiCol_Text, C.muted);
        ImGui::TextWrapped("%s", note);
        ImGui::PopStyleColor();
    }
    return clicked;
}

bool Radio(const char* label, bool on) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 at = ImGui::GetCursorScreenPos();
    float w = Measure(FontText, TextSize(), label).x + S(28);
    ImGui::InvisibleButton(label, ImVec2(w, S(26)));
    bool clicked = Clicked();
    ImVec2 center = at + ImVec2(S(8), S(13));
    draw->AddCircle(center, S(7), on ? C.accent : C.muted, 24, S(1.5f));
    if (on) draw->AddCircleFilled(center, S(3.5f), C.accent, 16);
    Write(draw, FontText, TextSize(), at + ImVec2(S(24), S(4)), C.text, label);
    return clicked;
}

// A text field. Returns true when an edit is finished (Enter, or leaving it).
bool Field(const char* id, char* buffer, size_t capacity, const char* hint, float width, ImGuiInputTextFlags flags = 0,
           ImFont* font = NULL) {
    ImGui::PushStyleColor(ImGuiCol_FrameBg, C.field);
    ImGui::PushStyleColor(ImGuiCol_Border, C.fieldLine);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, C.faint);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, S(1));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(6));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(10), S(7)));
    if (font) ImGui::PushFont(font, 0);
    ImGui::SetNextItemWidth(width);
    ImGui::InputTextWithHint(id, hint, buffer, capacity, flags);
    if (font) ImGui::PopFont();
    bool done = ImGui::IsItemDeactivatedAfterEdit();
    if (ImGui::IsItemActive()) {
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRect(a, b, C.accent, S(6), 0, S(1.5f));
    }
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(3);
    return done;
}

void Muted(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, C.muted);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

void Label(const char* text, float width) {
    ImVec2 at = ImGui::GetCursorScreenPos();
    Write(ImGui::GetWindowDrawList(), FontText, TextSize(), at + ImVec2(0, S(7)), C.text, text);
    ImGui::Dummy(ImVec2(width, S(30)));
    ImGui::SameLine(0, 0);
}

// A small rounded label with a dot, in a color.
float PillWidth(const std::string& text) { return Measure(FontBold, S(12), text.c_str()).x + S(28); }
void Pill(ImDrawList* draw, ImVec2 at, ImU32 color, const std::string& text) {
    float w = PillWidth(text), h = S(22);
    draw->AddRectFilled(at, at + ImVec2(w, h), Wash(color), h / 2);
    draw->AddCircleFilled(at + ImVec2(S(12), h / 2), S(3.5f), color, 12);
    Write(draw, FontBold, S(12), at + ImVec2(S(20), (h - S(12)) / 2 - S(1)), color == C.faint ? C.muted : color, text);
}

// A rounded panel, as cards are drawn.
void Panel(ImDrawList* draw, ImVec2 a, ImVec2 b) {
    draw->AddRectFilled(a, b, C.card, S(10));
    draw->AddRect(a, b, C.cardLine, S(10), 0, S(1));
}

// The icon and title at the top of a panel.
void PanelTitle(ImDrawList* draw, ImVec2 a, const char* icon, const char* title) {
    Icon(draw, icon, a + ImVec2(S(26), S(26)), S(15), C.text);
    Write(draw, FontBold, TextSize(), a + ImVec2(S(42), S(17)), C.text, title);
}

// Cards: a panel with an icon and a title over content laid out by ImGui.
struct CardFrame {
    ImVec2 start;
    float width;
    float right;  // Where text in it wraps, in the window's coordinates.
};
CardFrame Card;

float CardInner() { return Card.width - S(36); }

void BeginCard(const char* icon, const char* title, const char* id = NULL) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    Card.start = ImGui::GetCursorScreenPos();
    Card.width = ImGui::GetContentRegionAvail().x;
    draw->ChannelsSplit(2);
    draw->ChannelsSetCurrent(1);
    ImGui::PushID(id ? id : title);
    float pad = S(18);
    PanelTitle(draw, Card.start, icon, title);
    float lineY = Card.start.y + S(50);
    draw->AddLine(ImVec2(Card.start.x + pad, lineY), ImVec2(Card.start.x + Card.width - pad, lineY), C.cardLine, S(1));
    ImGui::SetCursorScreenPos(ImVec2(Card.start.x + pad, lineY + S(14)));
    ImGui::BeginGroup();
    Card.right = ImGui::GetCursorPosX() + CardInner();
    ImGui::PushTextWrapPos(Card.right);
}

void EndCard() {
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    float bottom = ImGui::GetItemRectMax().y + S(16);
    draw->ChannelsSetCurrent(0);
    Panel(draw, Card.start, ImVec2(Card.start.x + Card.width, bottom));
    draw->ChannelsMerge();
    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(Card.start.x, bottom));
    ImGui::Dummy(ImVec2(Card.width, S(6)));
}

// Page frame -----------------------------------------------------------------------

// Header buttons go right to left from the page's right edge.
struct RightRow {
    float x, y;
    void Place(float width) {
        x -= width;
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        x -= S(8);
    }
};

// The page's title; buttons can go right of it.
RightRow Header(const char* title) {
    ImVec2 at = ImGui::GetCursorScreenPos();
    Write(ImGui::GetWindowDrawList(), FontBold, S(21), at + ImVec2(0, S(4)), C.text, title);
    RightRow row = {at.x + ImGui::GetContentRegionAvail().x, at.y};
    return row;
}

// Where the page goes on under its header.
void Below(const RightRow& row) {
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetStyle().WindowPadding.x, row.y + S(50)));
}

// Check chips in one group: which kinds of games show.
void Chips(const char* const* labels, bool** values, int count, RightRow& row) {
    float total = S(4), h = S(34);
    std::vector<float> widths;
    for (int i = 0; i < count; ++i) {
        widths.push_back(Measure(FontText, TextSize(), labels[i]).x + S(36));
        total += widths.back();
    }
    row.Place(total);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 at = ImGui::GetCursorScreenPos();
    draw->AddRectFilled(at, at + ImVec2(total, h), C.card, S(8));
    draw->AddRect(at, at + ImVec2(total, h), C.cardLine, S(8), 0, S(1));
    float x = at.x + S(2);
    for (int i = 0; i < count; ++i) {
        ImVec2 a(x, at.y + S(2));
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton(labels[i], ImVec2(widths[i], h - S(4)));
        if (Clicked()) {
            *values[i] = !*values[i];
            SaveConfig();
        }
        if (ImGui::IsItemHovered()) draw->AddRectFilled(a, a + ImVec2(widths[i], h - S(4)), C.hover, S(6));
        Icon(draw, *values[i] ? ICON_BoxChecked : ICON_Box, a + ImVec2(S(15), (h - S(4)) / 2), S(16), *values[i] ? C.accent : C.muted);
        Write(draw, FontText, TextSize(), a + ImVec2(S(27), (h - S(4) - TextSize()) / 2 - S(1)), C.text, labels[i]);
        x += widths[i];
    }
}

void EmptyState(const char* icon, const char* title, const char* note, const char* action, const char* actionIcon, Page go) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 at = ImGui::GetCursorScreenPos();
    ImVec2 room = ImGui::GetContentRegionAvail();
    float width = std::min(room.x - S(40), S(440));
    float top = at.y + std::max(S(30), room.y * 0.25f);
    float cx = at.x + room.x / 2;
    Icon(draw, icon, ImVec2(cx, top + S(28)), S(52), C.faint);
    ImVec2 t = Measure(FontText, S(18), title);
    Write(draw, FontText, S(18), ImVec2(cx - t.x / 2, top + S(72)), C.text, title);
    ImGui::SetCursorScreenPos(ImVec2(cx - width / 2, top + S(104)));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
    ImGui::PushStyleColor(ImGuiCol_Text, C.muted);
    // Centered lines.
    std::string rest = note;
    while (!rest.empty()) {
        const char* end = FontText->CalcWordWrapPosition(TextSize(), rest.c_str(), rest.c_str() + rest.size(), width);
        std::string line(rest.c_str(), end);
        rest.erase(0, end - rest.c_str());
        while (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
        float w = Measure(FontText, TextSize(), line.c_str()).x;
        ImGui::SetCursorScreenPos(ImVec2(cx - w / 2, ImGui::GetCursorScreenPos().y));
        ImGui::TextUnformatted(line.c_str());
    }
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    if (action) {
        ImGui::Dummy(ImVec2(0, S(10)));
        ImGui::SetCursorScreenPos(ImVec2(cx - ButtonWidth(actionIcon, action) / 2, ImGui::GetCursorScreenPos().y));
        if (Button("empty", actionIcon, action, true)) {
            if (go == PageFolders && Current == PageFolders) AddFolders();
            else if (go == PageGames) AddFolders();
            else Current = go;
        }
    }
}

// The cover in a box, or a placeholder; the front fills the box's width and
// its bottom stays in view.
void CoverImage(ImDrawList* draw, const Game& game, ImVec2 a, ImVec2 b, float rounding) {
    float w = 0, h = 0;
    ImTextureID texture = game.titleId ? Cover(game.titleId, w, h) : 0;
    if (texture && w > 0 && h > 0) {
        float boxW = b.x - a.x, boxH = b.y - a.y;
        float shownH = boxW * h / w;
        ImVec2 uv0(0, 0), uv1(1, 1);
        if (shownH > boxH) uv0.y = 1 - boxH / shownH;          // Taller: the top is cut.
        else {                                                  // Wider: the sides are cut.
            float shownW = boxH * w / h;
            uv0.x = (1 - boxW / shownW) / 2;
            uv1.x = 1 - uv0.x;
        }
        SmoothImages(draw, true);
        draw->AddImageRounded(ImTextureRef(texture), a, b, uv0, uv1, IM_COL32_WHITE, rounding);
        SmoothImages(draw, false);
        return;
    }
    draw->AddRectFilled(a, b, C.hover, rounding);
    ImVec2 center((a.x + b.x) / 2, (a.y + b.y) / 2);
    bool loading = game.titleId && w == 0;
    if (loading && (b.x - a.x) > S(40)) Spinner(draw, center, S(14), C.faint);
    else Icon(draw, (b.x - a.x) > S(40) ? ICON_Image : ICON_Disc, center, std::min(S(44), (b.x - a.x) * 0.6f), C.faint);
}

// Charts ----------------------------------------------------------------------------

// A round top for a chart of rates up to bytesPerSecond: 1, 2, 2.5 or 5 times
// a power of ten, in MiB/s.
float ChartTop(float bytesPerSecond) {
    float mib = std::max(1.0f, bytesPerSecond * 1.1f / 1048576.0f);
    float step = powf(10, floorf(log10f(mib)));
    static const float steps[] = {1, 2, 2.5f, 5, 10};
    for (float m : steps)
        if (m * step >= mib) return m * step * 1048576.0f;
    return 10 * step * 1048576.0f;
}

// values as an area and a line in box a-b, from 0 up to top.
void Plot(ImDrawList* draw, const float* values, int count, ImVec2 a, ImVec2 b, float top, ImU32 color, float thickness, float fill) {
    std::vector<ImVec2> points(count);
    for (int i = 0; i < count; ++i)
        points[i] = ImVec2(a.x + (b.x - a.x) * i / (count - 1), b.y - (b.y - a.y) * std::min(1.0f, values[i] / top));
    if (fill > 0) {
        // Side by side quads; without anti-aliasing their edges don't show.
        ImDrawListFlags flags = draw->Flags;
        draw->Flags &= ~ImDrawListFlags_AntiAliasedFill;
        for (int i = 0; i + 1 < count; ++i)
            if (values[i] > 0 || values[i + 1] > 0)
                draw->AddQuadFilled(ImVec2(points[i].x, b.y), points[i], points[i + 1], ImVec2(points[i + 1].x, b.y), Fade(color, fill));
        draw->Flags = flags;
    }
    if (thickness > 0) draw->AddPolyline(points.data(), count, color, ImDrawFlags_None, thickness);
}

// The dashboard ------------------------------------------------------------------------

// Steps for a new setup, which tick themselves off.
float SetupCard(float x, float y, float width, const std::vector<ConsoleView>& consoles) {
    bool ftp = false, synced = false;
    for (const ConsoleState& c : Server.consoles) {
        ftp = ftp || c.ftp == 2;
        synced = synced || c.synced > 0;
    }
    std::string pc = FirstAddress();
    struct Step {
        const char* title;
        std::string note;
        bool done;
    };
    const Step steps[] = {
        {"Add the folders with your games",
         "ISOs, extracted games and Games on Demand packages, in the folders and their subfolders. You can also drop folders on this window.",
         !Saved.folders.empty()},
        {"Point NetISO at this PC",
         "Load the NetISO plugin in DashLaunch, and put this PC's address" + (pc.empty() ? std::string() : " (" + pc + ")") +
             " in NetISO.xex.txt next to NetISO.xex on the console's USB drive. This step is done once a console connects.",
         !consoles.empty()},
        {"Turn on Aurora's FTP server",
         "In Aurora's settings. The server uses it to put a folder for each game on the Xbox; the login is on the Consoles page.", ftp},
        {"Add Hdd1:\\NetISO to Aurora's scan paths",
         "In Aurora, under Content, Scan paths, with depth 1. The server fills that folder, and your games show up in Aurora with their covers.",
         synced},
    };
    const int count = 4;
    int done = 0, next = -1;
    for (int i = 0; i < count; ++i) {
        if (steps[i].done) ++done;
        else if (next < 0) next = i;
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    float pad = S(18), textX = x + pad + S(40);
    bool narrow = width < S(620);
    float noteW = x + width - pad - (narrow ? 0 : S(170)) - textX;
    float heights[count], total = S(56);
    for (int i = 0; i < count; ++i) {
        bool button = !steps[i].done && i < 2;
        heights[i] = steps[i].done ? S(48) : S(14) + S(20) + WrappedHeight(FontText, S(13), steps[i].note, noteW) + S(14) + (narrow && button ? S(42) : 0);
        total += heights[i];
    }
    ImVec2 a(x, y), b(x + width, y + total + S(4));
    Panel(draw, a, b);
    const char* title = done == count ? "You're all set" : "Get started";
    PanelTitle(draw, a, ICON_ListChecks, title);
    Write(draw, FontText, S(13), a + ImVec2(S(52) + Measure(FontBold, TextSize(), title).x, S(18)), C.muted,
          std::to_string(done) + " of " + std::to_string(count) + " done");
    float rx = b.x - pad;
    if (done == count) {
        rx -= ButtonWidth(ICON_Check, "Done");
        At(rx, a.y + S(11));
        if (Button("setup-done", ICON_Check, "Done", true)) SetupDone();
    } else {
        rx -= S(28);
        At(rx, a.y + S(13));
        if (IconButton("setup-hide", ICON_Close, "Hide these steps (the Consoles page can show them again)")) SetupDone();
    }
    rx -= S(8) + ButtonWidth(ICON_Link, "Install guide");
    At(rx, a.y + S(11));
    if (Button("setup-guide", ICON_Link, "Install guide")) OpenUrl(InstallUrl);
    draw->AddLine(ImVec2(a.x + pad, a.y + S(52)), ImVec2(b.x - pad, a.y + S(52)), C.cardLine, S(1));

    float sy = a.y + S(54);
    for (int i = 0; i < count; ++i) {
        const Step& step = steps[i];
        ImVec2 mark(x + pad + S(12), sy + S(14) + S(10));
        if (step.done) {
            Icon(draw, ICON_Done, mark, S(20), C.success);
        } else {
            ImU32 ring = i == next ? C.accent : C.faint;
            draw->AddCircle(mark, S(10), ring, 32, S(1.5f));
            std::string number = std::to_string(i + 1);
            ImVec2 size = Measure(FontBold, S(12), number.c_str());
            Write(draw, FontBold, S(12), mark - ImVec2(size.x / 2, size.y / 2 + S(0.5f)), i == next ? C.accent : C.muted, number);
        }
        Write(draw, FontBold, TextSize(), ImVec2(textX, sy + S(14)), step.done ? C.muted : C.text, step.title);
        float noteH = step.done ? 0 : Wrapped(draw, FontText, S(13), ImVec2(textX, sy + S(35)), C.muted, step.note, noteW);
        if (!step.done && i < 2) {
            bool copied = JustCopied("setup-copy");
            const char* icon = i == 0 ? ICON_FolderPlus : copied ? ICON_Check : ICON_Copy;
            const char* label = i == 0 ? "Add folder" : copied ? "Copied" : "Copy address";
            if (narrow) At(textX, sy + S(35) + noteH + S(10));
            else At(b.x - pad - ButtonWidth(icon, label), sy + S(16));
            if (Button(i == 0 ? "setup-add" : "setup-copy", icon, label, i == next, i == 0 || !pc.empty())) {
                if (i == 0) AddFolders();
                else Copy("setup-copy", pc);
            }
        }
        sy += heights[i];
        if (i + 1 < count) draw->AddLine(ImVec2(textX, sy), ImVec2(b.x - pad, sy), C.cardLine, S(1));
    }
    return b.y;
}

// Whether the server runs, and where consoles find it.
float ServerCard(float x, float y, float width) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    float h = S(84);
    ImVec2 a(x, y), b(x + width, y + h);
    Panel(draw, a, b);
    bool running = Server.running;
    ImVec2 center = a + ImVec2(S(44), h / 2);
    draw->AddCircleFilled(center, S(25), running ? Wash(C.success) : C.hover, 40);
    Icon(draw, ICON_Server, center, S(21), running ? C.success : C.muted);
    std::string title, note;
    if (running && Server.games >= 0) {
        title = "Server running";
        note = "Serving " + Plural(Server.games, "game", "games") + " to the consoles on your network";
    } else if (running) {
        title = "Starting the server";
        note = "Looking for games in your folders";
    } else {
        title = "Server stopped";
        note = Saved.folders.empty() ? "Add a folder with your games to start it" : "Consoles can't start games from this PC until it runs";
    }
    // The button, then this PC's address left of it.
    const char* label = running ? "Stop" : "Start";
    const char* icon = running ? ICON_Power : ICON_Play;
    float bx = b.x - S(22) - ButtonWidth(icon, label);
    At(bx, a.y + (h - S(32)) / 2);
    if (Button("server", icon, label, !running, running || !Saved.folders.empty())) {
        if (running) StopServer(true);
        else StartServer();
    }
    float textRight = bx - S(16);
    if (width >= S(600) && !ThisPc.empty()) {
        const char* what = "This PC, for NetISO.xex.txt";
        std::string pc = Elide(FontMono, S(15), ThisPc, S(220));
        float pw = Measure(FontMono, S(15), pc.c_str()).x;
        float px = bx - S(30) - std::max(pw + S(32), Measure(FontText, S(12), what).x);
        draw->AddLine(ImVec2(px - S(22), a.y + S(20)), ImVec2(px - S(22), b.y - S(20)), C.cardLine, S(1));
        Write(draw, FontText, S(12), ImVec2(px, a.y + S(22)), C.muted, what);
        Write(draw, FontMono, S(15), ImVec2(px, a.y + S(42)), C.text, pc);
        At(px + pw + S(4), a.y + S(37));
        CopyButton("server-copy", FirstAddress(), "Copy the address");
        textRight = px - S(40);
    }
    float textX = a.x + S(84);
    Write(draw, FontBold, S(17), ImVec2(textX, a.y + S(20)), C.text, Elide(FontBold, S(17), title, textRight - textX));
    Write(draw, FontText, S(13), ImVec2(textX, a.y + S(46)), C.muted, Elide(FontText, S(13), note, textRight - textX));
    return b.y;
}

void Tile(ImDrawList* draw, ImVec2 a, ImVec2 b, const char* icon, const char* label, const std::string& value, ImU32 valueColor,
          const std::string& note) {
    Panel(draw, a, b);
    float room = b.x - a.x - S(36);
    Icon(draw, icon, a + ImVec2(S(26), S(25)), S(15), C.muted);
    Write(draw, FontText, S(13), a + ImVec2(S(42), S(17)), C.muted, Elide(FontText, S(13), label, room - S(24)));
    Write(draw, FontBold, S(24), a + ImVec2(S(18), S(40)), valueColor, Elide(FontBold, S(24), value, room));
    Write(draw, FontText, S(12), a + ImVec2(S(18), S(74)), C.muted, Elide(FontText, S(12), note, room));
}

// The numbers: consoles, streaming, sent and games.
float Tiles(float x, float y, float width, const std::vector<ConsoleView>& consoles) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    int columns = width >= S(640) ? 4 : 2;
    float gap = S(14), h = S(100);
    float w = floorf((width - gap * (columns - 1)) / columns);
    int connected = 0;
    for (const ConsoleView& c : consoles) connected += Connected(c.state);
    float rate = TotalRate();
    unsigned long long size = 0;
    for (const Game& g : Games.games) size += g.size;
    int games = Server.running && Server.games >= 0 ? Server.games : int(Games.games.size());
    for (int i = 0; i < 4; ++i) {
        ImVec2 a(x + (w + gap) * (i % columns), y + (h + gap) * (i / columns)), b = a + ImVec2(w, h);
        switch (i) {
        case 0:
            Tile(draw, a, b, ICON_Xbox, "Consoles connected", std::to_string(connected), C.text,
                 consoles.empty() ? std::string("None has connected yet")
                                  : Plural(int(consoles.size()), "console has", "consoles have") + " used this server");
            break;
        case 1:
            Tile(draw, a, b, ICON_Activity, "Streaming now", Rate(rate), rate > 0 ? C.accent : C.text,
                 Server.peak > 0 ? "Peak " + Rate(Server.peak) : std::string("No game is loading data"));
            break;
        case 2: Tile(draw, a, b, ICON_Upload, "Sent this session", Bytes(TotalSent()), C.text, "Game data to the consoles"); break;
        case 3:
            Tile(draw, a, b, ICON_Games, "Games served", std::to_string(games), C.text,
                 Bytes(size) + " in " + Plural(int(Saved.folders.size()), "folder", "folders"));
            break;
        }
    }
    int rows = (4 + columns - 1) / columns;
    return y + h * rows + gap * (rows - 1);
}

// How fast the games stream, over the last two minutes, per console.
void StreamingCard(float x, float y, float w, float h, const std::vector<ConsoleView>& consoles) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 a(x, y), b(x + w, y + h);
    Panel(draw, a, b);
    PanelTitle(draw, a, ICON_Activity, "Streaming");
    const int count = 120;
    long long now = Second();
    std::vector<float> total(count, 0.0f);
    std::vector<std::vector<float>> lines;
    std::vector<int> owners;
    for (size_t k = 0; k < consoles.size(); ++k) {
        if (!consoles[k].state) continue;
        std::vector<float> line(count);
        bool any = false;
        for (int i = 0; i < count; ++i) {
            line[i] = Sample(*consoles[k].state, now - (count - 1) + i);
            total[i] += line[i];
            any = any || line[i] > 0;
        }
        if (!any) continue;
        lines.push_back(line);
        owners.push_back(int(k));
    }
    float top = 0;
    for (float v : total) top = std::max(top, v);
    // Right of the title: each console's color when several stream, or the rate.
    float lx = b.x - S(18);
    if (lines.size() > 1) {
        for (size_t k = lines.size(); k-- > 0;) {
            const std::string& name = consoles[owners[k]].address;
            float tw = Measure(FontText, S(12), name.c_str()).x;
            lx -= tw;
            Write(draw, FontText, S(12), ImVec2(lx, a.y + S(19)), C.muted, name);
            draw->AddCircleFilled(ImVec2(lx - S(9), a.y + S(26)), S(4), ConsoleColor(consoles[owners[k]].color), 12);
            lx -= S(26);
        }
    } else {
        std::string rate = Rate(TotalRate());
        Write(draw, FontBold, TextSize(), ImVec2(lx - Measure(FontBold, TextSize(), rate.c_str()).x, a.y + S(17)), top > 0 ? C.accent : C.muted, rate);
    }
    ImVec2 pa(a.x + S(18), a.y + S(64)), pb(b.x - S(18), b.y - S(36));
    float scale = ChartTop(top);
    for (int g = 0; g <= 2; ++g) {
        float gy = floorf(pb.y - (pb.y - pa.y) * g / 2);
        draw->AddLine(ImVec2(pa.x, gy), ImVec2(pb.x, gy), C.cardLine, S(1));
        if (!g) continue;
        char text[32];
        _snprintf_s(text, sizeof(text), _TRUNCATE, "%g MiB/s", scale * g / 2 / 1048576.0f);
        Write(draw, FontText, S(11), ImVec2(pa.x, gy - S(16)), C.faint, text);
    }
    const char* times[] = {"2 min ago", "1 min ago", "now"};
    for (int t = 0; t < 3; ++t) {
        float tw = Measure(FontText, S(11), times[t]).x;
        float tx = t == 0 ? pa.x : t == 1 ? (pa.x + pb.x - tw) / 2 : pb.x - tw;
        Write(draw, FontText, S(11), ImVec2(tx, pb.y + S(9)), C.faint, times[t]);
    }
    if (top <= 0) {
        const char* idle = Server.running ? "No game is loading data right now" : "The server is stopped";
        ImVec2 size = Measure(FontText, S(13), idle);
        ImVec2 at((pa.x + pb.x - size.x) / 2, (pa.y + pb.y - size.y) / 2);
        draw->AddRectFilled(at - ImVec2(S(10), S(4)), at + size + ImVec2(S(10), S(4)), C.card);
        Write(draw, FontText, S(13), at, C.muted, idle);
        return;
    }
    if (lines.size() > 1) {
        Plot(draw, total.data(), count, pa, pb, scale, C.muted, 0, 0.08f);
        for (size_t k = 0; k < lines.size(); ++k) Plot(draw, lines[k].data(), count, pa, pb, scale, ConsoleColor(consoles[owners[k]].color), S(1.75f), 0);
    } else {
        Plot(draw, total.data(), count, pa, pb, scale, ConsoleColor(consoles[owners[0]].color), S(2), 0.2f);
    }
}

void EventLook(const Event& event, const char*& icon, ImU32& color) {
    switch (event.kind) {
    case EventGood:
        icon = event.text.compare(0, 10, "New game: ") ? ICON_Done : ICON_Sparkles;
        color = C.success;
        break;
    case EventPlay: icon = ICON_Play; color = C.accent; break;
    case EventXbox: icon = ICON_Xbox; color = C.accent; break;
    case EventWarning: icon = ICON_Warning; color = C.warning; break;
    default: icon = ICON_Info; color = C.muted; break;
    }
}

// What happened lately, newest first.
void ActivityCard(float x, float y, float w, float h) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 a(x, y), b(x + w, y + h);
    Panel(draw, a, b);
    PanelTitle(draw, a, ICON_History, "Recent activity");
    At(a.x + S(4), a.y + S(52));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(4)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, S(10)));
    ImGui::BeginChild("activity", ImVec2(w - S(8), h - S(60)), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
    ImDrawList* list = ImGui::GetWindowDrawList();
    float inner = ImGui::GetContentRegionAvail().x;
    if (Server.events.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);
        Muted("Nothing yet. Consoles connecting, games starting and new games in your folders show up here.");
        ImGui::PopTextWrapPos();
    }
    for (size_t i = Server.events.size(); i-- > 0;) {
        const Event& event = Server.events[i];
        ImVec2 p = ImGui::GetCursorScreenPos();
        const char* icon;
        ImU32 color;
        EventLook(event, icon, color);
        list->AddCircleFilled(p + ImVec2(S(12), S(11)), S(12), Wash(color), 24);
        Icon(list, icon, p + ImVec2(S(12), S(11)), S(12), color);
        float timeW = Measure(FontText, S(12), event.time.c_str()).x;
        Write(list, FontText, S(12), ImVec2(p.x + inner - timeW, p.y + S(3)), C.faint, event.time);
        float textH = Wrapped(list, FontText, S(13), p + ImVec2(S(34), S(3)), C.text, event.text, inner - S(34) - timeW - S(12));
        ImGui::Dummy(ImVec2(inner, std::max(S(22), textH + S(4))));
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
}

float StreamingAndActivity(float x, float y, float width, const std::vector<ConsoleView>& consoles) {
    float h = S(290), gap = S(14);
    if (width < S(760)) {
        StreamingCard(x, y, width, h, consoles);
        ActivityCard(x, y + h + gap, width, h);
        return y + h * 2 + gap;
    }
    float chart = floorf((width - gap) * 0.6f);
    StreamingCard(x, y, chart, h, consoles);
    ActivityCard(x + chart + gap, y, width - chart - gap, h);
    return y + h;
}

// A small chart of one console's last minute.
void Sparkline(ImDrawList* draw, const ConsoleState* c, ImVec2 a, ImVec2 b, ImU32 color) {
    const int count = 60;
    float values[count], top = 0;
    long long now = Second();
    for (int i = 0; i < count; ++i) {
        values[i] = c ? Sample(*c, now - (count - 1) + i) : 0;
        top = std::max(top, values[i]);
    }
    draw->AddLine(ImVec2(a.x, b.y), b, C.cardLine, S(1));
    if (top > 0) Plot(draw, values, count, a, b, ChartTop(top), color, S(1.5f), 0.22f);
}

void Stat(ImDrawList* draw, ImVec2 at, float width, const char* label, ImU32 dot, const std::string& value) {
    Write(draw, FontText, S(12), at, C.muted, label);
    float x = at.x;
    if (dot) {
        draw->AddCircleFilled(ImVec2(x + S(4), at.y + S(27)), S(4), dot, 12);
        x += S(13);
    }
    Write(draw, FontText, S(13), ImVec2(x, at.y + S(19)), C.text, Elide(FontText, S(13), value, width - (x - at.x) - S(8)));
}

void ConsoleCard(const ConsoleView& view, float x, float y, float w, float h) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 a(x, y), b(x + w, y + h);
    Panel(draw, a, b);
    const ConsoleState* c = view.state;
    bool connected = Connected(c), playing = Playing(c);
    ImU32 color = ConsoleColor(view.color);
    float pad = S(18);
    // The console, and its state.
    ImVec2 box = a + ImVec2(pad, S(16));
    draw->AddRectFilled(box, box + ImVec2(S(38), S(38)), connected ? Wash(color) : C.hover, S(9));
    Icon(draw, ICON_Xbox, box + ImVec2(S(19), S(19)), S(18), connected ? color : C.muted);
    std::string state = playing ? "Playing" : connected ? "Connected" : "Offline";
    ImU32 stateColor = playing ? C.accent : connected ? C.success : C.faint;
    float pillX = b.x - pad - PillWidth(state);
    Pill(draw, ImVec2(pillX, a.y + S(24)), stateColor, state);
    float textX = a.x + pad + S(52);
    Write(draw, FontBold, S(15), ImVec2(textX, a.y + S(16)), C.text, Elide(FontBold, S(15), view.address, pillX - textX - S(8)));
    std::string seen = connected ? "Connected now" : view.seen ? "Last seen " + Ago(view.seen) : "Not connected since the server started";
    Write(draw, FontText, S(12), ImVec2(textX, a.y + S(38)), C.muted, Elide(FontText, S(12), seen, pillX - textX - S(8)));

    // Its disc, and how fast it streams.
    float my = a.y + S(70);
    bool disc = c && !c->mounted.empty();
    const Game* game = disc ? GameNamed(c->mounted) : NULL;
    ImVec2 ca(a.x + pad, my), cb(ca.x + S(38), my + S(52));
    if (game) CoverImage(draw, *game, ca, cb, S(4));
    else {
        draw->AddRectFilled(ca, cb, C.hover, S(4));
        Icon(draw, ICON_Disc, ImVec2((ca.x + cb.x) / 2, (ca.y + cb.y) / 2), S(18), C.faint);
    }
    float rate = c ? Sample(*c, Second()) : 0;
    std::string rateText = Rate(rate);
    float sparkW = std::min(S(170), floorf(w * 0.36f));
    float sx = b.x - pad - sparkW;
    Write(draw, FontBold, S(13), ImVec2(b.x - pad - Measure(FontBold, S(13), rateText.c_str()).x, my), rate > 0 ? color : C.muted, rateText);
    Sparkline(draw, c, ImVec2(sx, my + S(22)), ImVec2(b.x - pad, my + S(52)), color);
    float discX = cb.x + S(12), discW = sx - discX - S(14);
    Write(draw, FontText, S(12), ImVec2(discX, my + S(4)), C.muted, playing ? "Disc" : disc ? "Last disc" : "Disc");
    Write(draw, FontBold, TextSize(), ImVec2(discX, my + S(22)), disc ? C.text : C.muted,
          Elide(FontBold, TextSize(), disc ? c->mounted : std::string("None mounted"), discW));

    // Aurora, its game folders, and what it was sent.
    float fy = a.y + S(138);
    draw->AddLine(ImVec2(a.x + pad, fy - S(10)), ImVec2(b.x - pad, fy - S(10)), C.cardLine, S(1));
    float column = (w - pad * 2) / 3;
    bool kept = KeptUpToDate(view.address);
    ImU32 auroraDot = C.faint;
    std::string aurora = "Not checked yet";
    if (!kept) aurora = "Not used";
    else if (c && c->ftp == 2) {
        auroraDot = C.success;
        aurora = "Answers";
    } else if (c && c->ftp == 1) {
        auroraDot = C.warning;
        aurora = "Not answering";
    }
    Stat(draw, ImVec2(a.x + pad, fy), column, "Aurora FTP", auroraDot, aurora);
    std::string folders = !kept ? "Not in your list" : c && c->synced ? "Updated " + ClockAt(c->synced) : "Not yet";
    Stat(draw, ImVec2(a.x + pad + column, fy), column, "Game folders", kept && c && c->synced ? C.success : C.faint, folders);
    Stat(draw, ImVec2(a.x + pad + column * 2, fy), column, "Sent", 0, Bytes(c ? c->sent : 0));
}

float ConsoleCards(float x, float y, float width, const std::vector<ConsoleView>& consoles) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    Write(draw, FontBold, S(16), ImVec2(x, y + S(6)), C.text, "Consoles");
    y += S(38);
    if (consoles.empty()) {
        float h = S(78);
        ImVec2 a(x, y), b(x + width, y + h);
        Panel(draw, a, b);
        draw->AddRectFilled(a + ImVec2(S(18), S(20)), a + ImVec2(S(56), S(58)), C.hover, S(9));
        Icon(draw, ICON_Xbox, a + ImVec2(S(37), S(39)), S(18), C.faint);
        Write(draw, FontBold, TextSize(), a + ImVec2(S(72), S(20)), C.text, "No console has connected yet");
        Write(draw, FontText, S(13), a + ImVec2(S(72), S(42)), C.muted,
              Elide(FontText, S(13), "A console shows up here when its NetISO plugin connects to this PC.", width - S(90)));
        return b.y;
    }
    int columns = width >= S(640) ? 2 : 1;
    float gap = S(14), h = S(186);
    float w = floorf((width - gap * (columns - 1)) / columns);
    for (size_t i = 0; i < consoles.size(); ++i)
        ConsoleCard(consoles[i], x + (w + gap) * float(i % columns), y + (h + gap) * float(i / columns), w, h);
    int rows = int((consoles.size() + columns - 1) / columns);
    return y + h * rows + gap * (rows - 1);
}

void DashboardPage() {
    RightRow row = Header("Dashboard");
    Below(row);
    ImGui::BeginChild("dashboard", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    std::vector<ConsoleView> consoles = ConsoleList();
    float width = ImGui::GetContentRegionAvail().x - S(2);
    ImVec2 at = ImGui::GetCursorScreenPos();
    float y = at.y, gap = S(14);
    if (!Saved.setupDone) y = SetupCard(at.x, y, width, consoles) + gap;
    y = ServerCard(at.x, y, width) + gap;
    y = Tiles(at.x, y, width, consoles) + gap;
    y = StreamingAndActivity(at.x, y, width, consoles) + gap;
    y = ConsoleCards(at.x, y, width, consoles);
    At(at.x, y);
    ImGui::Dummy(ImVec2(width, S(8)));
    ImGui::EndChild();
    if (Server.running) WantFrameAfter(1000);  // The charts move on.
}

// The games page -------------------------------------------------------------------

bool Matches(const Game& game) {
    if (game.kind == nia::DiscIso && !Saved.showIso) return false;
    if (game.kind == nia::DiscGod && !Saved.showGod) return false;
    if (game.kind == nia::DiscFolder && !Saved.showFolder) return false;
    if (!SearchText[0]) return true;
    std::string name = game.name, id = Hex(game.titleId), search = SearchText;
    for (char& c : name) c = char(tolower((unsigned char)c));
    for (char& c : search) c = char(tolower((unsigned char)c));
    for (char& c : id) c = char(tolower((unsigned char)c));
    return name.find(search) != std::string::npos || (game.titleId && id.find(search) != std::string::npos);
}

std::vector<const Game*> Shown() {
    std::vector<const Game*> shown;
    for (const Game& g : Games.games)
        if (Matches(g)) shown.push_back(&g);
    std::stable_sort(shown.begin(), shown.end(), [](const Game* a, const Game* b) {
        if (Saved.sortBySize && a->size != b->size) return a->size > b->size;
        return _stricmp(a->name.c_str(), b->name.c_str()) < 0;
    });
    return shown;
}

void GameMenu(const Game& game) {
    if (ImGui::BeginPopupContextItem("menu")) {
        if (ImGui::MenuItem("Details")) Details = game.path;
        if (ImGui::MenuItem("Show in Explorer")) ShowInExplorer(game.path);
        ImGui::Separator();
        if (ImGui::MenuItem("Copy name")) ImGui::SetClipboardText(game.name.c_str());
        if (game.titleId && ImGui::MenuItem("Copy Title ID")) ImGui::SetClipboardText(Hex(game.titleId).c_str());
        ImGui::EndPopup();
    }
}

// Where the table's columns start (Size ends at end).
struct Columns {
    float name, kind, id, size, end;
};
Columns TableColumns(float left, float width) {
    Columns c;
    c.end = left + width - S(14);
    c.size = c.end - S(84);
    c.id = c.size - S(104);
    c.kind = c.id - S(76);
    c.name = left + S(56);
    return c;
}

void GameRow(const Game& game, const Columns& col, float width, int index) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 at = ImGui::GetCursorScreenPos();
    ImVec2 size(width, S(50));
    ImGui::PushID(game.path.c_str());
    ImGui::InvisibleButton("row", size);
    bool hovered = ImGui::IsItemHovered(), visible = ImGui::IsItemVisible();
    if (Clicked()) Details = game.path;
    GameMenu(game);
    ImGui::PopID();
    if (!visible) return;
    ImVec2 end = at + size;
    if (hovered) draw->AddRectFilled(at, end, C.hover, S(6));
    else if (index % 2) draw->AddRectFilled(at, end, Fade(C.hover, 0.45f), S(6));
    CoverImage(draw, game, at + ImVec2(S(14), S(6)), at + ImVec2(S(44), S(44)), S(3));
    float y = at.y + (size.y - TextSize()) / 2 - S(1);
    std::string on = PlayingOn(game);
    float nameRoom = col.kind - col.name - S(16);
    if (!on.empty()) {
        std::string label = "Playing on " + on;
        float pw = std::min(PillWidth(label), nameRoom * 0.6f);
        label = Elide(FontBold, S(12), label, pw - S(28));
        pw = PillWidth(label);
        std::string name = Elide(FontBold, TextSize(), game.name, nameRoom - pw - S(10));
        Write(draw, FontBold, TextSize(), ImVec2(col.name, y), C.text, name);
        Pill(draw, ImVec2(col.name + Measure(FontBold, TextSize(), name.c_str()).x + S(10), at.y + (size.y - S(22)) / 2), C.accent, label);
    } else {
        Write(draw, FontBold, TextSize(), ImVec2(col.name, y), C.text, Elide(FontBold, TextSize(), game.name, nameRoom));
    }
    Write(draw, FontText, TextSize(), ImVec2(col.kind, y), C.muted, KindName(game.kind));
    Write(draw, FontMono, S(13), ImVec2(col.id, y + S(1)), C.muted, game.titleId ? Hex(game.titleId) : "-");
    std::string gib = game.size ? Gib(game.size) : "-";
    Write(draw, FontText, TextSize(), ImVec2(col.end - Measure(FontText, TextSize(), gib.c_str()).x, y), C.text, gib);
}

// The table's head: Name and Size sort.
void TableHead(ImVec2 at, float width, const Columns& col) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    float h = S(34), y = at.y + (h - S(13)) / 2 - S(1);
    draw->AddLine(ImVec2(at.x, at.y + h - S(1)), ImVec2(at.x + width, at.y + h - S(1)), C.cardLine, S(1));
    struct Head {
        const char* label;
        float x;
        bool right;
        int sort;  // -1: doesn't sort; 0: by name; 1: by size.
    };
    Head heads[] = {{"Name", col.name, false, 0}, {"Format", col.kind, false, -1}, {"Title ID", col.id, false, -1}, {"Size", col.end, true, 1}};
    for (const Head& head : heads) {
        float tw = Measure(FontBold, S(13), head.label).x;
        bool sorted = head.sort == (Saved.sortBySize ? 1 : 0);
        float x = head.right ? head.x - tw - (sorted ? S(18) : 0) : head.x;
        if (head.sort >= 0) {
            At(x - S(6), at.y);
            ImGui::InvisibleButton(head.label, ImVec2(tw + S(30), h));
            if (Clicked() && !sorted) {
                Saved.sortBySize = head.sort == 1;
                SaveConfig();
            }
            if (ImGui::IsItemHovered()) draw->AddRectFilled(ImVec2(x - S(6), at.y + S(4)), ImVec2(x + tw + S(24), at.y + h - S(4)), C.hover, S(6));
            Tip(head.sort ? "Biggest first" : "By name");
        }
        Write(draw, FontBold, S(13), ImVec2(x, y), sorted ? C.text : C.muted, head.label);
        if (sorted) Icon(draw, head.sort ? ICON_ChevronDown : ICON_ChevronUp, ImVec2(x + tw + S(10), at.y + h / 2), S(13), C.text);
    }
}

void DetailsWindow() {
    const Game* game = NULL;
    for (const Game& g : Games.games)
        if (g.path == Details) game = &g;
    if (!Details.empty() && !ImGui::IsPopupOpen("game")) ImGui::OpenPopup("game");
    ImGui::SetNextWindowSize(ImVec2(S(600), 0));
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(18), S(18)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(10));
    if (ImGui::BeginPopupModal("game", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize)) {
        if (!game || Details.empty()) {
            Details.clear();
            ImGui::CloseCurrentPopup();
        } else {
            ImDrawList* draw = ImGui::GetWindowDrawList();
            ImVec2 at = ImGui::GetCursorScreenPos();
            ImVec2 coverSize(S(190), S(268));
            CoverImage(draw, *game, at, at + coverSize, S(8));
            ImGui::Dummy(coverSize);
            ImGui::SameLine(0, S(20));
            ImGui::BeginGroup();
            float right = ImGui::GetContentRegionAvail().x;
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + right);
            ImGui::PushFont(FontBold, 18);
            ImGui::TextWrapped("%s", game->name.c_str());
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(0, S(6)));
            auto row = [&](const char* what, const std::string& value) {
                ImGui::PushStyleColor(ImGuiCol_Text, C.muted);
                ImGui::TextUnformatted(what);
                ImGui::PopStyleColor();
                ImGui::SameLine(S(70));  // From the group's left.
                ImGui::TextWrapped("%s", value.c_str());
            };
            row("Title ID", game->titleId ? Hex(game->titleId) : std::string("Not known"));
            int discs = 0;
            for (const Game& g : Games.games) discs += game->titleId && g.titleId == game->titleId;
            if (game->disc && discs > 1) row("Disc", std::to_string(game->disc));
            row("Kind", game->kind == nia::DiscGod ? "Games on Demand package" : game->kind == nia::DiscFolder ? "Extracted game folder" : "ISO file");
            if (game->size) row("Size", Gib(game->size));
            row("Where", game->path);
            std::string on = PlayingOn(*game);
            if (!on.empty()) row("Now", "Playing on " + on);
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();
            ImGui::Dummy(ImVec2(0, S(8)));
            float buttons = ButtonWidth(ICON_FolderOpen, "Show in Explorer") + ButtonWidth(NULL, "Close") + S(8);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - buttons);
            if (Button("explorer", ICON_FolderOpen, "Show in Explorer")) ShowInExplorer(game->path);
            ImGui::SameLine(0, S(8));
            if (Button("close", NULL, "Close", true) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                Details.clear();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}

void GamesPage() {
    RightRow row = Header("Games");
    row.Place(S(34));
    if (RoundButton(ICON_FolderPlus, "Add a folder with games")) AddFolders();
    row.Place(S(34));
    if (RoundButton(ICON_Refresh, "Look for new games now", Scanning())) Rescan();
    if (!Games.games.empty()) {
        static const char* kinds[] = {"ISO", "GOD", "Folder"};
        bool* shows[] = {&Saved.showIso, &Saved.showGod, &Saved.showFolder};
        Chips(kinds, shows, 3, row);
    }
    Below(row);

    std::vector<const Game*> shown = Shown();
    bool table = Games.done && !shown.empty();
    if (!Games.games.empty()) {
        // Search, and how many.
        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(34), S(7)));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, C.field);
        ImGui::PushStyleColor(ImGuiCol_Border, C.fieldLine);
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, C.faint);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, S(1));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(8));
        ImGui::SetNextItemWidth(S(260));
        ImGui::InputTextWithHint("##search", "Search by title or Title ID", SearchText, sizeof(SearchText));
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(3);
        Icon(draw, ICON_Search, at + ImVec2(S(17), ImGui::GetItemRectSize().y / 2), S(15), C.muted);
        if (SearchText[0]) {
            ImGui::SameLine(0, S(-30));
            if (IconButton("clear", ICON_Close, "Clear")) SearchText[0] = 0;
        }
        unsigned long long total = 0;
        for (const Game* g : shown) total += g->size;
        std::string count = Plural(int(shown.size()), "game", "games") + "   " + Bytes(total);
        float w = Measure(FontText, TextSize(), count.c_str()).x;
        Write(draw, FontText, TextSize(), ImVec2(at.x + ImGui::GetContentRegionAvail().x - w - S(4), at.y + S(8)), C.muted, count);
        ImGui::SetCursorScreenPos(at + ImVec2(0, S(46)));
    }
    // The table's head stays put above the rows; it's drawn after them, when
    // their width (with or without a scroll bar) is known.
    ImVec2 head = ImGui::GetCursorScreenPos();
    if (table) ImGui::SetCursorScreenPos(head + ImVec2(0, S(38)));
    ImGui::BeginChild("games", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    float width = ImGui::GetContentRegionAvail().x;
    Columns col = TableColumns(ImGui::GetCursorScreenPos().x, width);
    if (Saved.folders.empty()) {
        EmptyState(ICON_FolderPlus, "Add the folders with your games",
                   "ISOs, extracted games and Games on Demand packages, in the folders you add and their subfolders. "
                   "You can also drop folders on this window.",
                   "Add folder", ICON_FolderPlus, PageGames);
    } else if (!Games.done) {
        ImVec2 room = ImGui::GetContentRegionAvail();
        ImVec2 at = ImGui::GetCursorScreenPos();
        Spinner(ImGui::GetWindowDrawList(), at + ImVec2(room.x / 2, room.y * 0.35f), S(16), C.accent);
    } else if (Games.games.empty()) {
        EmptyState(ICON_Disc, "No games in your folders yet",
                   "Put ISOs, extracted games (a folder with default.xex) or Games on Demand packages in one of your folders, "
                   "or add another folder. New games show up here by themselves.",
                   "Folders", ICON_Folder, PageFolders);
    } else if (shown.empty()) {
        ImGui::Dummy(ImVec2(0, S(20)));
        Muted("No games match. Change the search or the ISO / GOD / Folder boxes.");
    } else {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, S(2)));
        for (size_t i = 0; i < shown.size(); ++i) GameRow(*shown[i], col, width, int(i));
        ImGui::PopStyleVar();
    }
    if (!Games.duplicates.empty() && Games.done) {
        ImGui::Dummy(ImVec2(0, S(10)));
        BeginCard(ICON_Warning, "Not served: in your folders more than once");
        Muted("Each of these is the same game as another copy, which is served instead (an ISO before a Games on Demand package "
              "before an extracted folder). You can remove the extra copies.");
        ImGui::Dummy(ImVec2(0, S(4)));
        for (const Duplicate& d : Games.duplicates) {
            ImGui::TextWrapped("%s", d.copy.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, C.muted);
            ImGui::TextWrapped("    same game as %s", d.kept.c_str());
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, S(2)));
        }
        EndCard();
    }
    ImGui::EndChild();
    if (table) TableHead(head, width, col);
    DetailsWindow();
}

// The folders page --------------------------------------------------------------------

void FoldersPage() {
    RightRow row = Header("Game folders");
    row.Place(S(34));
    if (RoundButton(ICON_FolderPlus, "Add a folder with games")) AddFolders();
    Below(row);
    ImGui::BeginChild("folders", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    if (Saved.folders.empty()) {
        EmptyState(ICON_FolderPlus, "Add the folders with your games",
                   "ISOs, extracted games and Games on Demand packages, in the folders you add and their subfolders. "
                   "You can also drop folders on this window.",
                   "Add folder", ICON_FolderPlus, PageFolders);
        ImGui::EndChild();
        return;
    }
    BeginCard(ICON_Folder, "Served folders");
    ImDrawList* draw = ImGui::GetWindowDrawList();
    for (size_t i = 0; i < Saved.folders.size(); ++i) {
        ImGui::PushID(int(i));
        ImVec2 at = ImGui::GetCursorScreenPos();
        float inner = CardInner();
        Icon(draw, ICON_Folder, at + ImVec2(S(10), S(15)), S(16), C.accent);
        std::string path = Utf8(Saved.folders[i]);
        std::string count;
        if (Games.done && i < Games.perFolder.size()) count = Plural(Games.perFolder[i], "game", "games");
        else count = "Looking...";
        if (GetFileAttributesW(Saved.folders[i].c_str()) == INVALID_FILE_ATTRIBUTES) count = "Not found";
        float countW = Measure(FontText, TextSize(), count.c_str()).x;
        Write(draw, FontText, TextSize(), at + ImVec2(S(28), S(6)), C.text, Elide(FontText, TextSize(), path, inner - S(28) - countW - S(92)));
        Write(draw, FontText, TextSize(), ImVec2(at.x + inner - S(68) - countW, at.y + S(6)), count == "Not found" ? C.warning : C.muted, count);
        ImGui::SetCursorScreenPos(ImVec2(at.x + inner - S(60), at.y));
        if (IconButton("open", ICON_FolderOpen, "Open in Explorer")) ShowInExplorer(path);
        ImGui::SameLine(0, S(4));
        if (IconButton("remove", ICON_Close, "Stop serving this folder (nothing is deleted)")) RemoveFolder(i);
        ImGui::SetCursorScreenPos(at + ImVec2(0, S(30)));
        if (i + 1 < Saved.folders.size()) draw->AddLine(at + ImVec2(0, S(33)), at + ImVec2(inner, S(33)), C.cardLine, S(1));
        ImGui::Dummy(ImVec2(inner, S(6)));
        ImGui::PopID();
    }
    EndCard();

    BeginCard(ICON_Info, "What's served");
    Muted("ISO files, extracted games (a folder with default.xex) and Games on Demand packages, in these folders and their "
          "subfolders. New games are served and added to Aurora on every console without a restart. You can also drop folders "
          "on this window. Removing a folder here only stops serving it; nothing is deleted.");
    if (!Games.duplicates.empty()) {
        ImGui::Dummy(ImVec2(0, S(2)));
        std::string note = Plural(int(Games.duplicates.size()), "game is", "games are") +
                           " in your folders more than once; the Games page lists the copies that aren't served.";
        ImGui::PushStyleColor(ImGuiCol_Text, C.warning);
        ImGui::TextWrapped("%s", note.c_str());
        ImGui::PopStyleColor();
    }
    EndCard();
    ImGui::EndChild();
}

// The consoles page -----------------------------------------------------------------------

void LoadFields() {
    if (FieldsLoaded) return;
    FieldsLoaded = true;
    strncpy_s(XboxText, Utf8(Saved.xbox).c_str(), _TRUNCATE);
    strncpy_s(UserText, Utf8(Saved.user).c_str(), _TRUNCATE);
    strncpy_s(PasswordText, Utf8(Saved.password).c_str(), _TRUNCATE);
    strncpy_s(OptionsText, Utf8(Saved.options).c_str(), _TRUNCATE);
}

std::wstring Trimmed(const char* text) {
    std::string s = text;
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? L"" : Wide(s.substr(a, b - a + 1));
}

void ConsolesPage() {
    LoadFields();
    RightRow row = Header("Consoles");
    Below(row);
    ImGui::BeginChild("consoles", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    float label = S(160);
    std::vector<ConsoleView> consoles = ConsoleList();
    std::string forget;

    BeginCard(ICON_Xbox, "Consoles that use this server");
    if (consoles.empty())
        Muted("None yet. A console shows up here when its NetISO plugin connects: when the Xbox starts with NetISO.xex loaded and "
              "NetISO.xex.txt pointing at this PC. Any number of consoles can use the server at once.");
    ImDrawList* draw = ImGui::GetWindowDrawList();
    for (size_t i = 0; i < consoles.size(); ++i) {
        const ConsoleView& view = consoles[i];
        const ConsoleState* c = view.state;
        ImGui::PushID(view.address.c_str());
        ImVec2 at = ImGui::GetCursorScreenPos();
        float inner = CardInner();
        bool connected = Connected(c), playing = Playing(c);
        draw->AddCircleFilled(at + ImVec2(S(6), S(14)), S(5), connected ? ConsoleColor(view.color) : C.faint, 16);
        Write(draw, FontBold, TextSize(), at + ImVec2(S(22), S(5)), C.text, view.address);
        std::string state = playing ? "Playing " + c->mounted : connected ? "Connected" : view.seen ? "Last seen " + Ago(view.seen) : "Not connected";
        if (!KeptUpToDate(view.address)) state += " (game folders not kept up to date)";
        std::string sent = c && c->sent ? Bytes(c->sent) + " sent" : "";
        float sentW = Measure(FontText, S(13), sent.c_str()).x;
        float stateX = at.x + S(150);
        Write(draw, FontText, S(13), ImVec2(stateX, at.y + S(6)), playing ? C.accent : C.muted,
              Elide(FontText, S(13), state, at.x + inner - S(44) - sentW - S(16) - stateX));
        Write(draw, FontText, S(13), ImVec2(at.x + inner - S(40) - sentW, at.y + S(6)), C.muted, sent);
        ImGui::SetCursorScreenPos(ImVec2(at.x + inner - S(28), at.y));
        if (!connected && IconButton("forget", ICON_Remove, "Forget this console (it comes back when it connects again)")) forget = view.address;
        ImGui::SetCursorScreenPos(at + ImVec2(0, S(30)));
        if (i + 1 < consoles.size()) draw->AddLine(at + ImVec2(0, S(33)), at + ImVec2(inner, S(33)), C.cardLine, S(1));
        ImGui::Dummy(ImVec2(inner, S(6)));
        ImGui::PopID();
    }
    EndCard();

    BeginCard(ICON_Cable, "This PC");
    Muted("Each console's NetISO.xex.txt (next to NetISO.xex on its USB drive) needs this PC's address:");
    ImGui::Dummy(ImVec2(0, S(4)));
    std::string addresses = ThisPc.empty() ? "No network connection" : ThisPc;
    ImVec2 a = ImGui::GetCursorScreenPos();
    Write(ImGui::GetWindowDrawList(), FontMono, S(17), a + ImVec2(0, S(6)), C.text, addresses);
    ImGui::Dummy(ImVec2(Measure(FontMono, S(17), addresses.c_str()).x, S(32)));
    if (!ThisPc.empty()) {
        ImGui::SameLine(0, S(14));
        bool copied = JustCopied("pc-copy");
        if (Button("pc-copy", copied ? ICON_Check : ICON_Copy, copied ? "Copied" : "Copy")) Copy("pc-copy", FirstAddress());
    }
    EndCard();

    BeginCard(ICON_Sync, "Game folders in Aurora");
    Label("Only these consoles", label);
    if (Field("##xbox", XboxText, sizeof(XboxText), "every console", std::min(S(320), CardInner() - label)))
        SetConnection(Trimmed(XboxText), Trimmed(UserText), Trimmed(PasswordText));
    ImGui::Dummy(ImVec2(0, S(2)));
    Muted("Leave it empty and the server keeps the game folders up to date on every Xbox that connects. Or give the addresses of "
          "the ones to keep up to date, separated by commas; the others can still play, but get no folders in Aurora.");
    EndCard();

    BeginCard(ICON_Key, "Aurora FTP login");
    Label("User name", label);
    if (Field("##user", UserText, sizeof(UserText), "xboxftp", S(200))) SetConnection(Trimmed(XboxText), Trimmed(UserText), Trimmed(PasswordText));
    Label("Password", label);
    if (Field("##password", PasswordText, sizeof(PasswordText), "xboxftp", S(200), ShowPassword ? 0 : ImGuiInputTextFlags_Password))
        SetConnection(Trimmed(XboxText), Trimmed(UserText), Trimmed(PasswordText));
    ImGui::SameLine(0, S(6));
    if (IconButton("show", ShowPassword ? ICON_EyeOff : ICON_Eye, ShowPassword ? "Hide the password" : "Show the password")) ShowPassword = !ShowPassword;
    ImGui::Dummy(ImVec2(0, S(2)));
    Muted("The login of Aurora's FTP server (Settings > Network in Aurora), the same on every console; its default is xboxftp / "
          "xboxftp. The password is kept encrypted for your Windows account. Changes apply when you press Enter or leave the field.");
    EndCard();

    if (Saved.setupDone) {
        BeginCard(ICON_ListChecks, "Setting up a console");
        Muted("The dashboard's setup steps walk through it: the plugins on the USB drive, NetISO.xex.txt, Aurora's FTP server and "
              "its scan path.");
        ImGui::Dummy(ImVec2(0, S(4)));
        if (Button("setup", ICON_ListChecks, "Show the setup steps")) {
            Saved.setupDone = false;
            SaveConfig();
            Current = PageDashboard;
        }
        EndCard();
    }
    ImGui::EndChild();
    if (!forget.empty()) ForgetConsole(forget);
}

// The log page --------------------------------------------------------------------------

ImU32 LineColor(const std::string& line) {
    static const char* const bad[] = {"couldn't", "Couldn't", "failed", "isn't", "stopped", "Can't", "can't"};
    for (const char* word : bad)
        if (line.find(word) != std::string::npos) return C.warning;
    if (line.find(": mounted ") != std::string::npos || line.find("game folders up to date") != std::string::npos) return C.accent;
    return C.text;
}

void LogPage() {
    RightRow row = Header("Log");
    row.Place(S(34));
    if (RoundButton(ICON_Copy, "Copy the log")) {
        std::string all;
        for (const std::string& line : Server.log) all += line + "\r\n";
        ImGui::SetClipboardText(all.c_str());
    }
    Below(row);
    ImDrawList* outer = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetCursorScreenPos(), room = ImGui::GetContentRegionAvail();
    Panel(outer, a, a + room);
    ImGui::SetCursorScreenPos(a + ImVec2(S(1), S(6)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(6)));
    ImGui::BeginChild("log", room - ImVec2(S(2), S(12)), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushFont(FontMono, 13);
    ImGuiListClipper clipper;
    clipper.Begin(int(Server.log.size()));
    while (clipper.Step())
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            ImGui::PushStyleColor(ImGuiCol_Text, LineColor(Server.log[i]));
            ImGui::TextUnformatted(Server.log[i].c_str());
            ImGui::PopStyleColor();
        }
    ImGui::PopFont();
    // Follows new lines while it's scrolled to the end.
    if (Server.log.size() != LogSeen && LogAtEnd) ImGui::SetScrollHereY(1.0f);
    LogSeen = Server.log.size();
    LogAtEnd = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - S(4);
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

// The settings page --------------------------------------------------------------------

void Link(const char* text, const char* url) {
    ImGui::PushStyleColor(ImGuiCol_Text, C.accent);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    if (ImGui::IsItemHovered()) ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, b.y), b, C.accent, S(1));
    if (Clicked()) OpenUrl(url);
}

void SettingsPage() {
    LoadFields();
    RightRow row = Header("Settings");
    Below(row);
    ImGui::BeginChild("settings", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);

    BeginCard(ICON_Power, "Server");
    if (Server.running) {
        std::string text = Server.games >= 0 ? "Running, serving " + Plural(Server.games, "game.", "games.") : std::string("Running.");
        ImGui::TextUnformatted(text.c_str());
    } else {
        ImGui::TextUnformatted("Stopped.");
    }
    Muted("Closing or minimizing this window keeps the server running behind its icon in the notification area. Quit stops it.");
    ImGui::Dummy(ImVec2(0, S(4)));
    if (Server.running ? Button("stop", ICON_Power, "Stop the server") : Button("start", ICON_Play, "Start the server", true, !Saved.folders.empty())) {
        if (Server.running) StopServer(true);
        else StartServer();
    }
    ImGui::SameLine(0, S(8));
    if (Button("quit", ICON_Quit, "Quit")) Quit();
    EndCard();

    BeginCard(ICON_Windows, "Windows");
    bool boot = StartsWithWindows();
    if (Switch("boot", "Start with Windows", "Starts hidden in the notification area when you sign in, so your games are always served.", boot))
        SetStartWithWindows(!boot);
    EndCard();

    BeginCard(ICON_Palette, "Theme");
    if (Radio("System", Saved.theme == ThemeSystem)) SetTheme(ThemeSystem);
    if (Radio("Light", Saved.theme == ThemeLight)) SetTheme(ThemeLight);
    if (Radio("Dark", Saved.theme == ThemeDark)) SetTheme(ThemeDark);
    EndCard();

    BeginCard(ICON_Image, "Covers");
    if (Switch("covers", "Download covers", "From XboxUnity, as Aurora does (first from an archive of XboxUnity's covers on GitHub). "
                                             "They're only for this window; the Xbox gets its art from Aurora.",
               Saved.covers)) {
        Saved.covers = !Saved.covers;
        SaveConfig();
        ForgetCovers(false);
    }
    ImGui::Dummy(ImVec2(0, S(4)));
    if (Button("again", ICON_Refresh, "Download the covers again")) ForgetCovers(true);
    EndCard();

    BeginCard(ICON_Sliders, "Advanced");
    ImGui::TextUnformatted("Extra server options");
    if (Field("##options", OptionsText, sizeof(OptionsText), "for example --verbose", CardInner(), 0, FontMono)) SetOptions(Trimmed(OptionsText));
    Muted("Added to the server's command line; NetDisc.exe -h in a command prompt lists them. Applies when you press Enter.");
    ImGui::Dummy(ImVec2(0, S(6)));
    std::string ini = Utf8(IniPath());
    ImGui::PushStyleColor(ImGuiCol_Text, C.muted);
    ImGui::TextWrapped("Settings file: %s", ini.c_str());
    ImGui::PopStyleColor();
    if (Button("settingsfile", ICON_FolderOpen, "Show the settings file")) ShowInExplorer(ini);
    EndCard();

    BeginCard(ICON_Info, "About");
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetCursorScreenPos();
    draw->AddRectFilled(a, a + ImVec2(S(44), S(44)), C.accent, S(11));
    Icon(draw, ICON_Disc, a + ImVec2(S(22), S(22)), S(26), C.onAccent);
    Write(draw, FontBold, S(17), a + ImVec2(S(58), S(2)), C.text, "NetDisc 360");
    Write(draw, FontText, S(13), a + ImVec2(S(58), S(26)), C.muted, "Built " __DATE__);
    ImGui::Dummy(ImVec2(0, S(52)));
    ImGui::TextWrapped("Serves the Xbox 360 games on this PC to modded Xbox 360 consoles over the network, and puts each one in Aurora "
                       "like an installed game, with its cover and the Guide working.");
    ImGui::Dummy(ImVec2(0, S(2)));
    Link("github.com/marcusisdahl/NetDisc-360", RepositoryUrl);
    ImGui::Dummy(ImVec2(0, S(4)));
    Muted("Thanks to cOz for NetISO and its plugin, tuxuser for netiso-srv, Team Phoenix for Aurora and XboxUnity for the covers. "
          "This window uses Dear ImGui (MIT) and Lucide icons (ISC).");
    ImGui::Dummy(ImVec2(0, S(2)));
    Muted("Xbox and Xbox 360 are trademarks of Microsoft. This project is not affiliated with Microsoft, cOz or Team Phoenix. "
          "Use it only with games you own. MIT License.");
    EndCard();
    ImGui::EndChild();
}

// The window ---------------------------------------------------------------------------------

bool NavItem(const char* icon, const char* label, bool selected, const std::string& badge, bool strong, float width) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 at = ImGui::GetCursorScreenPos();
    ImVec2 size(width, S(38));
    ImGui::PushID(label);
    ImGui::InvisibleButton("nav", size);
    bool clicked = Clicked();
    bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    if (selected) draw->AddRectFilled(at, at + size, C.accentSoft, S(8));
    else if (hovered) draw->AddRectFilled(at, at + size, C.hover, S(8));
    Icon(draw, icon, at + ImVec2(S(20), size.y / 2), S(17), selected ? C.text : Mix(C.text, C.muted, 0.45f));
    Write(draw, selected ? FontBold : FontText, TextSize(), at + ImVec2(S(40), (size.y - TextSize()) / 2 - S(1)), C.text, label);
    if (!badge.empty()) {
        float w = Measure(FontBold, S(12), badge.c_str()).x;
        if (strong) {
            float pw = std::max(S(22), w + S(14));
            ImVec2 p(at.x + size.x - S(10) - pw, at.y + (size.y - S(20)) / 2);
            draw->AddRectFilled(p, p + ImVec2(pw, S(20)), C.accent, S(10));
            Write(draw, FontBold, S(12), p + ImVec2((pw - w) / 2, S(3)), C.onAccent, badge);
        } else {
            Write(draw, FontText, S(12), ImVec2(at.x + size.x - S(14) - w, at.y + (size.y - S(12)) / 2 - S(1)), C.muted, badge);
        }
    }
    return clicked;
}

void Sidebar(float height) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 at = ImGui::GetCursorScreenPos();
    float width = S(SidebarWidth);
    draw->AddRectFilled(at, at + ImVec2(width, height), C.sidebar);
    draw->AddLine(at + ImVec2(width - S(1), 0), at + ImVec2(width - S(1), height), C.line, S(1));
    ImVec2 logo = at + ImVec2(S(18), S(18));
    draw->AddRectFilled(logo, logo + ImVec2(S(36), S(36)), C.accent, S(9));
    Icon(draw, ICON_Disc, logo + ImVec2(S(18), S(18)), S(21), C.onAccent);
    Write(draw, FontBold, S(15), logo + ImVec2(S(48), S(1)), C.text, "NetDisc 360");
    Write(draw, FontText, S(12), logo + ImVec2(S(48), S(20)), C.muted, "Game server");
    int connected = 0;
    for (const ConsoleState& c : Server.consoles) connected += Connected(&c);
    struct Item {
        const char* icon;
        const char* label;
        int page;
    };
    static const Item items[] = {{ICON_Dashboard, "Dashboard", PageDashboard},
                                 {ICON_Games, "Games", PageGames},
                                 {ICON_Folder, "Game folders", PageFolders},
                                 {ICON_Xbox, "Consoles", PageConsoles},
                                 {ICON_Log, "Log", PageLog}};
    float y = at.y + S(78);
    for (const Item& item : items) {
        std::string badge;
        bool strong = false;
        if (item.page == PageGames && Games.done && !Games.games.empty()) badge = std::to_string(Games.games.size());
        if (item.page == PageConsoles && connected) {
            badge = std::to_string(connected);
            strong = true;
        }
        At(at.x + S(10), y);
        if (NavItem(item.icon, item.label, Current == item.page, badge, strong, width - S(20))) Current = item.page;
        y += S(42);
    }
    At(at.x + S(10), at.y + height - S(48));
    if (NavItem(ICON_Settings, "Settings", Current == PageSettings, "", false, width - S(20))) Current = PageSettings;
}

void StatusBar(ImVec2 at, float width) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    float h = S(34);
    draw->AddRectFilled(at, at + ImVec2(width, h), C.sidebar);
    draw->AddLine(at, at + ImVec2(width, 0), C.line, S(1));
    float y = at.y + (h - S(13)) / 2 - S(1);
    float x = at.x + S(16);
    draw->AddCircleFilled(ImVec2(x + S(4), at.y + h / 2), S(4.5f), Server.running ? C.success : C.faint, 16);
    x += S(16);
    std::vector<std::string> parts;
    if (!Server.running) parts.push_back(Saved.folders.empty() ? "Stopped: add a folder with games" : "Stopped");
    else if (Server.games >= 0) parts.push_back("Serving " + Plural(Server.games, "game", "games"));
    else parts.push_back("Starting");
    int connected = 0, playing = 0;
    std::string game, on;
    for (const ConsoleState& c : Server.consoles) {
        connected += Connected(&c);
        if (Playing(&c)) {
            ++playing;
            game = c.mounted;
            on = c.address;
        }
    }
    if (connected) parts.push_back(Plural(connected, "console connected", "consoles connected"));
    float rate = TotalRate();
    size_t rateAt = rate > 0 ? parts.size() : std::string::npos;
    if (rate > 0) parts.push_back(Rate(rate));
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) {
            draw->AddCircleFilled(ImVec2(x + S(8), at.y + h / 2), S(1.5f), C.faint, 8);
            x += S(16);
        }
        Write(draw, FontText, S(13), ImVec2(x, y), i == rateAt ? C.accent : C.text, parts[i]);
        x += Measure(FontText, S(13), parts[i].c_str()).x;
    }
    std::string pc = ThisPc.empty() ? "No network" : "This PC  " + ThisPc;
    float pw = Measure(FontText, S(13), pc.c_str()).x;
    float right = at.x + width - pw - S(16);
    Write(draw, FontText, S(13), ImVec2(right, y), C.muted, pc);
    if (playing) {
        x += S(22);
        Icon(draw, ICON_Play, ImVec2(x + S(5), at.y + h / 2), S(12), C.accent);
        std::string text = playing == 1 ? game + " on " + on : Plural(playing, "console playing", "consoles playing");
        Write(draw, FontText, S(13), ImVec2(x + S(16), y), C.accent, Elide(FontText, S(13), text, right - x - S(36)));
    }
    if (rate > 0) WantFrameAfter(1000);
}

void Colors() {
    if (Dark) {
        C.sidebar = Rgb(0x1e1e1e);
        C.content = Rgb(0x262626);
        C.line = Rgb(0x363636);
        C.card = Rgb(0x1e1e1e);
        C.cardLine = Rgb(0x333333);
        C.field = Rgb(0x2f2f2f);
        C.fieldLine = Rgb(0x444444);
        C.text = Rgb(0xe8e8e8);
        C.muted = Rgb(0xa0a0a0);
        C.faint = Rgb(0x6a6a6a);
        C.accent = Rgb(0x65d2e1);
        C.onAccent = Rgb(0x0f1e21);
        C.accentSoft = Rgb(0x2c4a4e);
        C.hover = Rgb(0x2e2e2e);
        C.warning = Rgb(0xe0a33a);
        C.success = Rgb(0x5cbf6a);
        C.danger = Rgb(0xe5534b);
    } else {
        C.sidebar = Rgb(0xffffff);
        C.content = Rgb(0xf5f6f8);
        C.line = Rgb(0xe3e3e3);
        C.card = Rgb(0xffffff);
        C.cardLine = Rgb(0xe0e0e0);
        C.field = Rgb(0xffffff);
        C.fieldLine = Rgb(0xcfcfcf);
        C.text = Rgb(0x1f1f1f);
        C.muted = Rgb(0x666666);
        C.faint = Rgb(0xa8a8a8);
        C.accent = Rgb(0x466288);
        C.onAccent = Rgb(0xffffff);
        C.accentSoft = Rgb(0xdbe2ec);
        C.hover = Rgb(0xeef0f3);
        C.warning = Rgb(0xa86a00);
        C.success = Rgb(0x1e8a3a);
        C.danger = Rgb(0xc42b1c);
    }
}
}  // namespace

void ApplyStyle() {
    Colors();
    ImGuiStyle style;
    style.WindowPadding = ImVec2(10, 10);
    style.FramePadding = ImVec2(10, 6);
    style.ItemSpacing = ImVec2(8, 8);
    style.ItemInnerSpacing = ImVec2(6, 6);
    style.ScrollbarSize = 10;
    style.ScrollbarRounding = 5;
    style.GrabRounding = 5;
    style.WindowRounding = 10;
    style.ChildRounding = 8;
    style.FrameRounding = 6;
    style.PopupRounding = 8;
    style.WindowBorderSize = 1;
    style.PopupBorderSize = 1;
    style.FontSizeBase = 14;
    style.ScaleAllSizes(Scale);
    style.FontScaleDpi = Scale;
    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = ImGui::ColorConvertU32ToFloat4(C.text);
    c[ImGuiCol_TextDisabled] = ImGui::ColorConvertU32ToFloat4(C.faint);
    c[ImGuiCol_WindowBg] = ImGui::ColorConvertU32ToFloat4(C.content);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImGui::ColorConvertU32ToFloat4(C.card);
    c[ImGuiCol_Border] = ImGui::ColorConvertU32ToFloat4(C.cardLine);
    c[ImGuiCol_FrameBg] = ImGui::ColorConvertU32ToFloat4(C.field);
    c[ImGuiCol_FrameBgHovered] = ImGui::ColorConvertU32ToFloat4(C.field);
    c[ImGuiCol_FrameBgActive] = ImGui::ColorConvertU32ToFloat4(C.field);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImGui::ColorConvertU32ToFloat4(Fade(C.muted, 0.35f));
    c[ImGuiCol_ScrollbarGrabHovered] = ImGui::ColorConvertU32ToFloat4(Fade(C.muted, 0.55f));
    c[ImGuiCol_ScrollbarGrabActive] = ImGui::ColorConvertU32ToFloat4(Fade(C.muted, 0.7f));
    c[ImGuiCol_Header] = ImGui::ColorConvertU32ToFloat4(C.hover);
    c[ImGuiCol_HeaderHovered] = ImGui::ColorConvertU32ToFloat4(C.hover);
    c[ImGuiCol_HeaderActive] = ImGui::ColorConvertU32ToFloat4(C.accentSoft);
    c[ImGuiCol_Separator] = ImGui::ColorConvertU32ToFloat4(C.cardLine);
    c[ImGuiCol_TextSelectedBg] = ImGui::ColorConvertU32ToFloat4(Fade(C.accent, 0.35f));
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, Dark ? 0.55f : 0.35f);
    c[ImGuiCol_NavCursor] = ImGui::ColorConvertU32ToFloat4(C.accent);
    ImGui::GetStyle() = style;
}

void Draw() {
    ImGuiViewport* view = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(view->Pos);
    ImGui::SetNextWindowSize(view->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::Begin("NetDisc 360", NULL,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(3);
    float sidebar = S(SidebarWidth), bar = S(34);
    ImVec2 at = ImGui::GetCursorScreenPos();
    Sidebar(view->Size.y);
    ImGui::SetCursorScreenPos(at + ImVec2(sidebar, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(22), S(16)));
    ImGui::BeginChild("page", ImVec2(view->Size.x - sidebar, view->Size.y - bar), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    switch (Current) {
    case PageDashboard: DashboardPage(); break;
    case PageGames: GamesPage(); break;
    case PageFolders: FoldersPage(); break;
    case PageConsoles: ConsolesPage(); break;
    case PageLog: LogPage(); break;
    case PageSettings: SettingsPage(); break;
    }
    if (Current != PageConsoles && Current != PageSettings) FieldsLoaded = false;
    ImGui::EndChild();
    StatusBar(at + ImVec2(sidebar, view->Size.y - bar), view->Size.x - sidebar);
    ImGui::End();
}
}
#endif
