// SPDX-License-Identifier: MIT
// The pages of netiso-server.exe's window, drawn with Dear ImGui after
// TinyXbox360BackupManager's look: icons down the left, a page per icon,
// rounded cards, and the games as a grid of covers. Everything here runs on
// the window's thread, once per frame (gui.cpp draws only when something
// changed).
#ifdef _WIN32
#include "gui.h"
#include "../third_party/imgui-1.92/imgui.h"
#include "../third_party/imgui-1.92/imgui_internal.h"
#include "../third_party/lucide/lucide_icons.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>

namespace gui {
namespace {
enum Page { PageGames, PageFolders, PageXbox, PageLog, PageSettings, PageAbout };
const char RepositoryUrl[] = "https://github.com/marcusisdahl/NetISO-Aurora";

struct Palette {
    ImU32 sidebar, content, line, card, cardLine, field, fieldLine, text, muted, faint, accent, onAccent, accentSoft, hover, warning,
        success, danger;
};
Palette C;

int Current = PageGames;
char SearchText[128];
std::string Details;  // Path of the game shown in the details window.
bool ShowPassword;
bool FieldsLoaded;
char XboxText[64], UserText[128], PasswordText[128], OptionsText[1024];
bool LogAtEnd = true;
size_t LogSeen;

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

std::string Gib(unsigned long long bytes) {
    char text[32];
    _snprintf_s(text, sizeof(text), _TRUNCATE, "%.2f GiB", bytes / 1073741824.0);
    return text;
}

std::string Hex(DWORD titleId) {
    char text[12];
    _snprintf_s(text, sizeof(text), _TRUNCATE, "%08X", (unsigned)titleId);
    return text;
}

const char* KindName(nia::DiscKind kind) { return kind == nia::DiscGod ? "GOD" : kind == nia::DiscFolder ? "Folder" : "ISO"; }

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

bool NavButton(const char* icon, const char* tip, bool selected) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 at = ImGui::GetCursorScreenPos();
    float size = S(44);
    ImGui::PushID(icon);
    ImGui::InvisibleButton("nav", ImVec2(size, size));
    bool clicked = Clicked();
    bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    ImVec2 center = at + ImVec2(size / 2, size / 2);
    if (selected) draw->AddCircleFilled(center, size / 2, C.accentSoft, 40);
    else if (hovered) draw->AddCircleFilled(center, size / 2, C.hover, 40);
    Icon(draw, icon, center, S(20), selected ? C.text : Mix(C.text, C.muted, 0.35f));
    Tip(tip);
    return clicked;
}

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

float ButtonWidth(const char* icon, const char* label) {
    return Measure(FontText, TextSize(), label).x + S(icon ? 46 : 28);
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
    ImU32 fill = primary ? C.accent : C.card, ink = primary ? C.onAccent : C.text;
    if (hovered) fill = Mix(fill, primary ? (Dark ? 0xFFFFFFFF : 0xFF000000) : C.hover, primary ? 0.12f : 1.0f);
    if (!enabled) ink = C.faint;
    draw->AddRectFilled(at, at + size, fill, S(7));
    if (!primary) draw->AddRect(at, at + size, C.fieldLine, S(7), 0, S(1));
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

// A pill with text over a cover: white on translucent black.
void Badge(ImDrawList* draw, ImVec2 rightBottom, const char* icon, const std::string& text, ImU32 fill = IM_COL32(0, 0, 0, 166)) {
    float size = S(11);
    float w = Measure(FontBold, size, text.c_str()).x + S(14) + (icon ? S(15) : 0), h = S(18);
    ImVec2 a(rightBottom.x - w, rightBottom.y - h);
    draw->AddRectFilled(a, rightBottom, fill, S(6));
    float x = a.x + S(7);
    if (icon) {
        Icon(draw, icon, ImVec2(x + S(5), a.y + h / 2), S(11), IM_COL32_WHITE);
        x += S(15);
    }
    Write(draw, FontBold, size, ImVec2(x, a.y + (h - size) / 2 - S(0.5f)), IM_COL32_WHITE, text);
}

// Cards: a rounded panel with an icon and a title over its content.
struct CardFrame {
    ImVec2 start;
    float width;
    float right;  // Where text in it wraps, in the window's coordinates.
};
CardFrame Card;

float CardInner() { return Card.width - S(28); }

void BeginCard(const char* icon, const char* title, const char* id = NULL) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    Card.start = ImGui::GetCursorScreenPos();
    Card.width = ImGui::GetContentRegionAvail().x;
    draw->ChannelsSplit(2);
    draw->ChannelsSetCurrent(1);
    ImGui::PushID(id ? id : title);
    float pad = S(14);
    ImVec2 head = Card.start + ImVec2(pad, S(13));
    Icon(draw, icon, head + ImVec2(S(8), S(8)), S(15), C.text);
    Write(draw, FontText, TextSize(), head + ImVec2(S(22), S(-1)), C.text, title);
    float lineY = Card.start.y + S(42);
    draw->AddLine(ImVec2(Card.start.x + pad, lineY), ImVec2(Card.start.x + Card.width - pad, lineY), C.cardLine, S(1));
    ImGui::SetCursorScreenPos(ImVec2(Card.start.x + pad, lineY + S(12)));
    ImGui::BeginGroup();
    Card.right = ImGui::GetCursorPosX() + CardInner();
    ImGui::PushTextWrapPos(Card.right);
}

void EndCard() {
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    float bottom = ImGui::GetItemRectMax().y + S(14);
    draw->ChannelsSetCurrent(0);
    ImVec2 end(Card.start.x + Card.width, bottom);
    draw->AddRectFilled(Card.start, end, C.card, S(8));
    draw->AddRect(Card.start, end, C.cardLine, S(8), 0, S(1));
    draw->ChannelsMerge();
    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(Card.start.x, bottom));
    ImGui::Dummy(ImVec2(Card.width, S(10)));
}

// A line with a colored dot: a state and what it means.
void StatusRow(const char* what, ImU32 dot, const std::string& text, float labelWidth) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 at = ImGui::GetCursorScreenPos();
    Write(draw, FontText, TextSize(), at, C.muted, what);
    draw->AddCircleFilled(at + ImVec2(labelWidth + S(5), S(9)), S(4.5f), dot, 16);
    ImGui::SetCursorScreenPos(at + ImVec2(labelWidth + S(18), 0));
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::Dummy(ImVec2(0, S(4)));
}

// Page frame -----------------------------------------------------------------------

// The page title; returns the x where the header's buttons may end.
void Header(const char* title) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 at = ImGui::GetCursorScreenPos();
    Icon(draw, ICON_ChevronRight, at + ImVec2(S(8), S(17)), S(16), C.muted);
    Write(draw, FontText, S(18), at + ImVec2(S(22), S(6)), C.text, title);
}

// Header buttons go right to left from the page's right edge.
struct RightRow {
    float x, y;
    void Place(float width) {
        x -= width;
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        x -= S(8);
    }
};

RightRow HeaderRight() {
    ImVec2 at = ImGui::GetCursorScreenPos();
    RightRow row = {at.x + ImGui::GetContentRegionAvail().x, at.y};
    return row;
}

// A segmented group of icon toggles.
int Segments(const char* id, const char* const* icons, const char* const* tips, int count, int selected, RightRow& row) {
    float item = S(32), h = S(34);
    row.Place(item * count + S(4));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 at = ImGui::GetCursorScreenPos();
    draw->AddRectFilled(at, at + ImVec2(item * count + S(4), h), C.card, S(8));
    draw->AddRect(at, at + ImVec2(item * count + S(4), h), C.cardLine, S(8), 0, S(1));
    int clicked = -1;
    ImGui::PushID(id);
    for (int i = 0; i < count; ++i) {
        ImVec2 a = at + ImVec2(S(2) + item * i, S(2));
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        ImGui::InvisibleButton("segment", ImVec2(item, h - S(4)));
        if (Clicked()) clicked = i;
        bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        if (i == selected) draw->AddRectFilled(a, a + ImVec2(item, h - S(4)), C.accentSoft, S(6));
        else if (hovered) draw->AddRectFilled(a, a + ImVec2(item, h - S(4)), C.hover, S(6));
        Icon(draw, icons[i], a + ImVec2(item / 2, (h - S(4)) / 2), S(16), i == selected ? C.text : C.muted);
        Tip(tips[i]);
    }
    ImGui::PopID();
    return clicked;
}

// Check chips in one group, like 360 / XBLA / OG in TinyXbox360BackupManager.
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

bool IsPlaying(const Game& game) { return Server.running && !Server.playing.empty() && !_stricmp(game.name.c_str(), Server.playing.c_str()); }

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
    draw->AddRectFilled(a, b, Dark ? Rgb(0x2c2c2c) : Rgb(0xeeeeee), rounding);
    ImVec2 center((a.x + b.x) / 2, (a.y + b.y) / 2);
    bool loading = game.titleId && w == 0;
    if (loading && (b.x - a.x) > S(40)) Spinner(draw, center, S(14), C.faint);
    else Icon(draw, (b.x - a.x) > S(40) ? ICON_Image : ICON_Disc, center, std::min(S(44), (b.y - a.y) * 0.6f), C.faint);
}

void GameCard(const Game& game) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 at = ImGui::GetCursorScreenPos();
    ImVec2 size(S(172), S(268));
    ImGui::PushID(game.path.c_str());
    ImGui::InvisibleButton("card", size);
    bool hovered = ImGui::IsItemHovered(), visible = ImGui::IsItemVisible();
    if (Clicked()) Details = game.path;
    GameMenu(game);
    ImGui::PopID();
    if (!visible) return;
    bool playing = IsPlaying(game);
    ImVec2 end = at + size;
    float r = S(10);
    if (hovered || playing) {
        // A soft glow in the accent color.
        for (int i = 1; i <= 6; ++i)
            draw->AddRect(at - ImVec2(S(float(i)), S(float(i))), end + ImVec2(S(float(i)), S(float(i))),
                          Fade(C.accent, (playing && !hovered ? 0.10f : 0.16f) * (7 - i) / 6.0f), r + S(float(i)), 0, S(2));
    }
    draw->AddRectFilled(at, end, C.card, r);
    draw->AddRect(at, end, hovered || playing ? C.accent : C.cardLine, r, 0, hovered || playing ? S(2) : S(1));
    std::string title = Elide(FontBold, TextSize(), game.name, size.x - S(16));
    float tw = Measure(FontBold, TextSize(), title.c_str()).x;
    Write(draw, FontBold, TextSize(), ImVec2(at.x + (size.x - tw) / 2, at.y + S(8)), C.text, title);
    ImVec2 a = at + ImVec2(S(6), S(32)), b = end - ImVec2(S(6), S(6));
    CoverImage(draw, game, a, b, S(6));
    Badge(draw, b - ImVec2(S(6), S(6)), ICON_Tag, game.titleId ? Hex(game.titleId) : std::string(KindName(game.kind)));
    Badge(draw, b - ImVec2(S(6), S(27)), NULL, Gib(game.size));
    if (game.kind != nia::DiscIso) Badge(draw, ImVec2(b.x - S(6), a.y + S(24)), NULL, KindName(game.kind));
    if (playing) {
        float w = Measure(FontBold, S(11), "Playing").x + S(30);
        ImVec2 pa = a + ImVec2(S(6), S(6));
        draw->AddRectFilled(pa, pa + ImVec2(w, S(20)), C.accent, S(6));
        Icon(draw, ICON_Play, pa + ImVec2(S(11), S(10)), S(11), C.onAccent);
        Write(draw, FontBold, S(11), pa + ImVec2(S(20), S(3.5f)), C.onAccent, "Playing");
    }
}

void GameRow(const Game& game, float width, int index) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 at = ImGui::GetCursorScreenPos();
    ImVec2 size(width, S(44));
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
    CoverImage(draw, game, at + ImVec2(S(8), S(5)), at + ImVec2(S(32), S(39)), S(3));
    float y = at.y + (size.y - TextSize()) / 2 - S(1);
    float sizeX = end.x - S(12), idX = end.x - S(200), kindX = end.x - S(270);
    std::string name = game.name;
    if (IsPlaying(game)) name += "  (playing)";
    Write(draw, FontBold, TextSize(), ImVec2(at.x + S(44), y), IsPlaying(game) ? C.accent : C.text,
          Elide(FontBold, TextSize(), name, kindX - at.x - S(56)));
    Write(draw, FontText, TextSize(), ImVec2(kindX, y), C.muted, KindName(game.kind));
    Write(draw, FontMono, S(13), ImVec2(idX, y + S(1)), C.muted, game.titleId ? Hex(game.titleId) : "-");
    std::string gib = Gib(game.size);
    Write(draw, FontText, TextSize(), ImVec2(sizeX - Measure(FontText, TextSize(), gib.c_str()).x, y), C.text, gib);
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
            row("Size", Gib(game->size));
            row("Where", game->path);
            if (IsPlaying(*game)) row("Now", "Mounted on the Xbox");
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
    Header("Games");
    RightRow row = HeaderRight();
    row.Place(S(34));
    if (RoundButton(ICON_FolderPlus, "Add a folder with games")) AddFolders();
    row.Place(S(34));
    if (RoundButton(ICON_Refresh, "Look for new games now", Scanning())) Rescan();
    if (!Games.games.empty()) {  // With games: how to show them.
        static const char* viewIcons[] = {ICON_Grid, ICON_List};
        static const char* viewTips[] = {"Covers", "List"};
        int view = Segments("view", viewIcons, viewTips, 2, Saved.listView ? 1 : 0, row);
        if (view >= 0) {
            Saved.listView = view == 1;
            SaveConfig();
        }
        static const char* sortIcons[] = {ICON_SortName, ICON_SortSize};
        static const char* sortTips[] = {"By name", "Biggest first"};
        int sort = Segments("sort", sortIcons, sortTips, 2, Saved.sortBySize ? 1 : 0, row);
        if (sort >= 0) {
            Saved.sortBySize = sort == 1;
            SaveConfig();
        }
        static const char* kinds[] = {"ISO", "GOD", "Folder"};
        bool* shows[] = {&Saved.showIso, &Saved.showGod, &Saved.showFolder};
        Chips(kinds, shows, 3, row);
    }
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetStyle().WindowPadding.x, row.y + S(46)));

    std::vector<const Game*> shown = Shown();
    if (!Games.games.empty()) {
        // Search, and how many.
        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(at);
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
        char count[96];
        _snprintf_s(count, sizeof(count), _TRUNCATE, "%d %s   ~%s", int(shown.size()), shown.size() == 1 ? "game" : "games", Gib(total).c_str());
        float w = Measure(FontText, TextSize(), count).x;
        Write(draw, FontText, TextSize(), ImVec2(at.x + ImGui::GetContentRegionAvail().x - w - S(4), at.y + S(8)), C.muted, count);
        ImGui::SetCursorScreenPos(at + ImVec2(0, S(44)));
    }

    ImGui::BeginChild("games", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
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
        ImGui::Dummy(ImVec2(0, S(30)));
        Muted("No games match. Change the search or the ISO / GOD / Folder boxes.");
    } else if (Saved.listView) {
        float width = ImGui::GetContentRegionAvail().x;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, S(2)));
        for (size_t i = 0; i < shown.size(); ++i) GameRow(*shown[i], width, int(i));
        ImGui::PopStyleVar();
    } else {
        // Columns of cards, spread over the width; the margin leaves room for the glow.
        float cell = S(172), height = S(268), margin = S(6), gap = S(12);
        ImVec2 start = ImGui::GetCursorPos() + ImVec2(margin, margin);
        float width = ImGui::GetContentRegionAvail().x - 2 * margin;
        int columns = std::max(1, int((width + gap) / (cell + gap)));
        if (columns > 1 && int(shown.size()) >= columns) gap = std::min(S(28), (width - columns * cell) / (columns - 1));
        for (size_t i = 0; i < shown.size(); ++i) {
            ImGui::SetCursorPos(start + ImVec2((cell + gap) * float(i % columns), (height + S(12)) * float(i / columns)));
            GameCard(*shown[i]);
        }
        int rows = int((shown.size() + columns - 1) / columns);
        ImGui::SetCursorPos(start + ImVec2(0, (height + S(12)) * rows));
        ImGui::Dummy(ImVec2(0, S(4)));
    }
    if (!Games.duplicates.empty() && Games.done) {
        ImGui::Dummy(ImVec2(0, S(6)));
        char text[160];
        _snprintf_s(text, sizeof(text), _TRUNCATE, "%d %s more than once in your folders; one copy of each is served. The Folders page lists them.",
                    int(Games.duplicates.size()), Games.duplicates.size() == 1 ? "game is" : "games are");
        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImVec2 at = ImGui::GetCursorScreenPos();
        Icon(draw, ICON_Warning, at + ImVec2(S(8), S(9)), S(14), C.warning);
        ImGui::SetCursorScreenPos(at + ImVec2(S(22), 0));
        ImGui::PushStyleColor(ImGuiCol_Text, C.warning);
        ImGui::TextWrapped("%s", text);
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, S(8)));
    }
    ImGui::EndChild();
    DetailsWindow();
}

// The folders page --------------------------------------------------------------------

void FoldersPage() {
    Header("Game folders");
    RightRow row = HeaderRight();
    row.Place(S(34));
    if (RoundButton(ICON_FolderPlus, "Add a folder with games")) AddFolders();
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetStyle().WindowPadding.x, row.y + S(46)));
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
        if (Games.done && i < Games.perFolder.size())
            count = std::to_string(Games.perFolder[i]) + (Games.perFolder[i] == 1 ? " game" : " games");
        else
            count = "Looking...";
        if (GetFileAttributesW(Saved.folders[i].c_str()) == INVALID_FILE_ATTRIBUTES) count = "Not found";
        float countW = Measure(FontText, TextSize(), count.c_str()).x;
        Write(draw, FontText, TextSize(), at + ImVec2(S(28), S(6)), C.text, Elide(FontText, TextSize(), path, inner - S(28) - countW - S(92)));
        Write(draw, FontText, TextSize(), ImVec2(at.x + inner - S(68) - countW, at.y + S(6)), count == "Not found" ? C.warning : C.muted, count);
        ImGui::SetCursorScreenPos(ImVec2(at.x + inner - S(60), at.y));
        if (IconButton("open", ICON_FolderOpen, "Open in Explorer")) ShowInExplorer(path);
        ImGui::SameLine(0, S(4));
        if (IconButton("remove", ICON_Close, "Stop serving this folder (nothing is deleted)")) RemoveFolder(i);
        ImGui::SetCursorScreenPos(at + ImVec2(0, S(30)));
        if (i + 1 < Saved.folders.size())
            draw->AddLine(at + ImVec2(0, S(33)), at + ImVec2(inner, S(33)), C.cardLine, S(1));
        ImGui::Dummy(ImVec2(inner, S(6)));
        ImGui::PopID();
    }
    EndCard();

    BeginCard(ICON_Info, "What's served");
    Muted("ISO files, extracted games (a folder with default.xex) and Games on Demand packages, in these folders and their "
          "subfolders. New games are served and added to Aurora without a restart. You can also drop folders on this window. "
          "Removing a folder here only stops serving it; nothing is deleted.");
    EndCard();

    if (!Games.duplicates.empty()) {
        BeginCard(ICON_Warning, "More than once");
        Muted("These are the same game as another copy, so only that one is served. You can remove the extra copies:");
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
}

// The Xbox page -----------------------------------------------------------------------

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

void XboxPage() {
    LoadFields();
    Header("Xbox 360");
    ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(at + ImVec2(0, S(46)));
    ImGui::BeginChild("xbox", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    float label = S(150);

    BeginCard(ICON_Network, "Connection");
    ImU32 off = C.faint;
    if (!Server.running) {
        StatusRow("Server", off, "Stopped. Start it on the Settings page or from the icon in the notification area.", label);
    } else {
        StatusRow("NetISO plugin", Server.xbox.empty() ? off : C.success,
                  Server.xbox.empty() ? "Hasn't connected yet. It connects when the Xbox starts Aurora." : "Connected from " + Server.xbox, label);
        StatusRow("Aurora's FTP server", Server.ftp == 2 ? C.success : Server.ftp == 1 ? C.warning : off,
                  Server.ftp == 2   ? "Answers at " + Server.ftpNote
                  : Server.ftp == 1 ? "Waiting for it: " + Server.ftpNote
                                    : std::string("Not tried yet; the server waits for the Xbox"),
                  label);
        StatusRow("Game folders", Server.update.empty() ? off : C.success, Server.update.empty() ? "Not updated yet" : Server.update, label);
        StatusRow("Playing", Server.playing.empty() ? off : C.accent, Server.playing.empty() ? "Nothing mounted" : Server.playing, label);
    }
    EndCard();

    BeginCard(ICON_Cable, "This PC");
    Muted("The console's NetISO.xex.txt (next to NetISO.xex on the USB drive) needs this PC's address:");
    ImGui::Dummy(ImVec2(0, S(4)));
    std::string addresses = ThisPc.empty() ? "No network connection" : ThisPc;
    ImVec2 a = ImGui::GetCursorScreenPos();
    Write(ImGui::GetWindowDrawList(), FontMono, S(17), a + ImVec2(0, S(6)), C.text, addresses);
    ImGui::Dummy(ImVec2(Measure(FontMono, S(17), addresses.c_str()).x, S(32)));
    ImGui::SameLine(0, S(14));
    if (!ThisPc.empty() && Button("copy", ICON_Copy, "Copy")) {
        std::string first = ThisPc.substr(0, ThisPc.find(','));
        ImGui::SetClipboardText(first.c_str());
    }
    EndCard();

    BeginCard(ICON_Wifi, "Xbox address");
    Label("IP address", label);
    if (Field("##xbox", XboxText, sizeof(XboxText), "automatic", S(200))) SetConnection(Trimmed(XboxText), Trimmed(UserText), Trimmed(PasswordText));
    ImGui::Dummy(ImVec2(0, S(2)));
    Muted("Leave it empty and the server writes the game folders to the Xbox whose NetISO plugin connects. "
          "Fill it in if more than one Xbox uses this server.");
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
    Muted("The login of Aurora's FTP server (Settings > Network in Aurora); its default is xboxftp / xboxftp. "
          "The password is kept encrypted for your Windows account. Changes apply when you press Enter or leave the field.");
    EndCard();
    ImGui::EndChild();
}

// The log page --------------------------------------------------------------------------

ImU32 LineColor(const std::string& line) {
    if (line.find("Couldn't") != std::string::npos || line.find("failed") != std::string::npos ||
        line.find("isn't") != std::string::npos || line.find("stopped") != std::string::npos || line.find("Can't") != std::string::npos)
        return C.warning;
    if (line.find(": mounted ") != std::string::npos || line.find("The Xbox is up to date") != std::string::npos) return C.accent;
    return C.text;
}

void LogPage() {
    Header("Log");
    RightRow row = HeaderRight();
    row.Place(S(34));
    if (RoundButton(ICON_Copy, "Copy the log")) {
        std::string all;
        for (const std::string& line : Server.log) all += line + "\r\n";
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetStyle().WindowPadding.x, row.y + S(46)));
    ImDrawList* outer = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetCursorScreenPos(), room = ImGui::GetContentRegionAvail();
    outer->AddRectFilled(a, a + room, C.card, S(8));
    outer->AddRect(a, a + room, C.cardLine, S(8), 0, S(1));
    ImGui::SetCursorScreenPos(a + ImVec2(S(1), S(6)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(4)));
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

void SettingsPage() {
    LoadFields();
    Header("Settings");
    ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(at + ImVec2(0, S(46)));
    ImGui::BeginChild("settings", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);

    BeginCard(ICON_Power, "Server");
    if (Server.running) {
        char text[96];
        if (Server.games >= 0) _snprintf_s(text, sizeof(text), _TRUNCATE, "Running, serving %d %s.", Server.games, Server.games == 1 ? "game" : "games");
        else strcpy_s(text, "Running.");
        ImGui::TextUnformatted(text);
    } else {
        ImGui::TextUnformatted("Stopped.");
    }
    Muted("Closing or minimizing this window keeps the server running behind its icon in the notification area. Quit stops it.");
    ImGui::Dummy(ImVec2(0, S(4)));
    if (Server.running ? Button("stop", ICON_Power, "Stop the server") : Button("start", ICON_Play, "Start the server", true)) {
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
    Muted("Added to the server's command line; netiso-server.exe -h in a command prompt lists them. Applies when you press Enter.");
    ImGui::Dummy(ImVec2(0, S(6)));
    std::string ini = Utf8(IniPath());
    ImGui::PushStyleColor(ImGuiCol_Text, C.muted);
    ImGui::TextWrapped("Settings file: %s", ini.c_str());
    ImGui::PopStyleColor();
    if (Button("settingsfile", ICON_FolderOpen, "Show the settings file")) ShowInExplorer(ini);
    EndCard();
    ImGui::EndChild();
}

// The about page -------------------------------------------------------------------------

void Link(const char* text, const char* url) {
    ImGui::PushStyleColor(ImGuiCol_Text, C.accent);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    if (ImGui::IsItemHovered()) ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, b.y), b, C.accent, S(1));
    if (Clicked()) OpenUrl(url);
}

void AboutPage() {
    Header("About");
    ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(at + ImVec2(0, S(46)));
    ImGui::BeginChild("about", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetCursorScreenPos();
    draw->AddCircleFilled(a + ImVec2(S(40), S(40)), S(40), C.accentSoft, 48);
    Icon(draw, ICON_Disc, a + ImVec2(S(40), S(40)), S(44), C.accent);
    Write(draw, FontBold, S(22), a + ImVec2(S(100), S(12)), C.text, "NetISO for Aurora");
    Write(draw, FontText, TextSize(), a + ImVec2(S(100), S(46)), C.muted, "Built " __DATE__);
    ImGui::Dummy(ImVec2(0, S(96)));
    BeginCard(ICON_Info, "What it does");
    ImGui::TextWrapped("Serves the Xbox 360 games on this PC to a modded Xbox 360 over the network, and puts each one in Aurora like an "
                       "installed game, with its cover and the Guide working.");
    ImGui::Dummy(ImVec2(0, S(4)));
    Link("github.com/marcusisdahl/NetISO-Aurora", RepositoryUrl);
    EndCard();
    BeginCard(ICON_Heart, "Thanks to");
    ImGui::TextWrapped("cOz for NetISO and its plugin; tuxuser for netiso-srv; Team Phoenix for Aurora; XboxUnity for the covers.");
    ImGui::Dummy(ImVec2(0, S(4)));
    ImGui::TextWrapped("This window uses Dear ImGui (MIT) and Lucide icons (ISC), and takes its look from TinyXbox360BackupManager.");
    ImGui::Dummy(ImVec2(0, S(4)));
    Muted("Xbox and Xbox 360 are trademarks of Microsoft. This project is not affiliated with Microsoft, cOz or Team Phoenix. "
          "Use it only with games you own. MIT License.");
    EndCard();
    ImGui::EndChild();
}

// The window ---------------------------------------------------------------------------------

void Sidebar(float height) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 at = ImGui::GetCursorScreenPos();
    float width = S(62);
    draw->AddRectFilled(at, at + ImVec2(width, height), C.sidebar);
    draw->AddLine(at + ImVec2(width - S(1), 0), at + ImVec2(width - S(1), height), C.line, S(1));
    struct Item { const char* icon; const char* tip; int page; };
    static const Item top[] = {{ICON_Games, "Games", PageGames},
                               {ICON_Folder, "Game folders", PageFolders},
                               {ICON_Xbox, "Xbox 360", PageXbox},
                               {ICON_Log, "Log", PageLog}};
    static const Item bottom[] = {{ICON_Settings, "Settings", PageSettings}, {ICON_Info, "About", PageAbout}};
    float y = at.y + S(10);
    for (const Item& item : top) {
        ImGui::SetCursorScreenPos(ImVec2(at.x + S(9), y));
        if (NavButton(item.icon, item.tip, Current == item.page)) Current = item.page;
        y += S(52);
    }
    y = at.y + height - S(10) - S(52) * 2 + S(8);
    for (const Item& item : bottom) {
        ImGui::SetCursorScreenPos(ImVec2(at.x + S(9), y));
        if (NavButton(item.icon, item.tip, Current == item.page)) Current = item.page;
        y += S(52);
    }
}

void StatusBar(ImVec2 at, float width) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    float h = S(34);
    draw->AddRectFilled(at, at + ImVec2(width, h), C.sidebar);
    draw->AddLine(at, at + ImVec2(width, 0), C.line, S(1));
    float y = at.y + (h - TextSize()) / 2 - S(1);
    float x = at.x + S(14);
    draw->AddCircleFilled(ImVec2(x + S(4), at.y + h / 2), S(4.5f), Server.running ? C.success : C.faint, 16);
    x += S(16);
    std::string state;
    if (!Server.running) state = Saved.folders.empty() ? "Stopped: add a folder with games" : "Stopped";
    else if (Server.games >= 0) state = "Serving " + std::to_string(Server.games) + (Server.games == 1 ? " game" : " games");
    else state = "Starting";
    Write(draw, FontText, TextSize(), ImVec2(x, y), C.text, state);
    x += Measure(FontText, TextSize(), state.c_str()).x + S(18);
    if (Server.running && !Server.playing.empty()) {
        Icon(draw, ICON_Play, ImVec2(x + S(5), at.y + h / 2), S(12), C.accent);
        std::string playing = "Playing " + Server.playing;
        Write(draw, FontText, TextSize(), ImVec2(x + S(16), y), C.accent, Elide(FontText, TextSize(), playing, width * 0.45f));
    }
    std::string pc = ThisPc.empty() ? "No network" : "This PC  " + ThisPc;
    float pw = Measure(FontText, TextSize(), pc.c_str()).x;
    Write(draw, FontText, TextSize(), ImVec2(at.x + width - pw - S(14), y), C.muted, pc);
}

void Colors() {
    if (Dark) {
        C.sidebar = Rgb(0x1e1e1e);
        C.content = Rgb(0x282828);
        C.line = Rgb(0x393939);
        C.card = Rgb(0x1e1e1e);
        C.cardLine = Rgb(0x333333);
        C.field = Rgb(0x2f2f2f);
        C.fieldLine = Rgb(0x444444);
        C.text = Rgb(0xe8e8e8);
        C.muted = Rgb(0xa0a0a0);
        C.faint = Rgb(0x6a6a6a);
        C.accent = Rgb(0x65d2e1);
        C.onAccent = Rgb(0x0f1e21);
        C.accentSoft = Rgb(0x335458);
        C.hover = Rgb(0x333333);
        C.warning = Rgb(0xe0a33a);
        C.success = Rgb(0x5cbf6a);
        C.danger = Rgb(0xe5534b);
    } else {
        C.sidebar = Rgb(0xffffff);
        C.content = Rgb(0xf7f7f7);
        C.line = Rgb(0xe3e3e3);
        C.card = Rgb(0xffffff);
        C.cardLine = Rgb(0xdddddd);
        C.field = Rgb(0xffffff);
        C.fieldLine = Rgb(0xcfcfcf);
        C.text = Rgb(0x1f1f1f);
        C.muted = Rgb(0x666666);
        C.faint = Rgb(0xa8a8a8);
        C.accent = Rgb(0x466288);
        C.onAccent = Rgb(0xffffff);
        C.accentSoft = Rgb(0xc8d0dc);
        C.hover = Rgb(0xeceff3);
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
    ImGui::Begin("NetISO for Aurora", NULL,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(3);
    float sidebar = S(62), bar = S(34);
    ImVec2 at = ImGui::GetCursorScreenPos();
    Sidebar(view->Size.y);
    ImGui::SetCursorScreenPos(at + ImVec2(sidebar, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(12)));
    ImGui::BeginChild("page", ImVec2(view->Size.x - sidebar, view->Size.y - bar), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    switch (Current) {
    case PageGames: GamesPage(); break;
    case PageFolders: FoldersPage(); break;
    case PageXbox: XboxPage(); break;
    case PageLog: LogPage(); break;
    case PageSettings: SettingsPage(); break;
    case PageAbout: AboutPage(); break;
    }
    if (Current != PageXbox && Current != PageSettings) FieldsLoaded = false;
    ImGui::EndChild();
    StatusBar(at + ImVec2(sidebar, view->Size.y - bar), view->Size.x - sidebar);
    ImGui::End();
}
}
#endif
