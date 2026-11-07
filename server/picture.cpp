// SPDX-License-Identifier: MIT
#include "picture.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#ifdef _MSC_VER
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "../third_party/imgui/imstb_truetype.h"
#ifdef _MSC_VER
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include "../third_party/fonts/roboto_medium.h"

namespace nia {
namespace {
// Colours white text reads well on (contrast 4.5:1 or better), far enough
// apart that two guests side by side are told apart.
const unsigned Colours[] = {
    0xC62828, 0xAD1457, 0x6A1B9A, 0x4527A0, 0x283593, 0x1565C0,
    0x00695C, 0x2E7D32, 0xD84315, 0x4E342E, 0x37474F, 0x00838F,
};

unsigned Crc(const unsigned char* data, size_t size, unsigned crc = 0) {
    static unsigned table[256];
    if(!table[1])
        for(unsigned n = 0; n < 256; ++n) {
            unsigned c = n;
            for(int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
    crc = ~crc;
    for(size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void Put32(std::string& out, unsigned v) {
    out += char(v >> 24); out += char(v >> 16); out += char(v >> 8); out += char(v);
}

void Chunk(std::string& out, const char* type, const std::string& data) {
    Put32(out, unsigned(data.size()));
    std::string body = std::string(type, 4) + data;
    out += body;
    Put32(out, Crc((const unsigned char*)body.data(), body.size()));
}

// rgb (size*size*3 bytes, rows top down) as a PNG: 8-bit RGB, the image data
// in stored (not compressed) deflate blocks, which every PNG reader takes
// and which keep a 64-pixel picture under the 15 KB a package's thumbnail
// may be.
std::string Png(const std::vector<unsigned char>& rgb, int size) {
    std::string raw;
    for(int y = 0; y < size; ++y) {
        raw += char(0);  // No filter.
        raw.append((const char*)&rgb[size_t(y) * size * 3], size_t(size) * 3);
    }
    std::string z;
    z += char(0x78); z += char(0x01);
    for(size_t at = 0;;) {
        size_t n = raw.size() - at < 65535 ? raw.size() - at : 65535;
        bool last = at + n == raw.size();
        z += char(last ? 1 : 0);
        z += char(n & 0xFF); z += char(n >> 8);
        z += char(~n & 0xFF); z += char((~n >> 8) & 0xFF);
        z.append(raw, at, n);
        at += n;
        if(last) break;
    }
    unsigned a = 1, b = 0;
    for(size_t i = 0; i < raw.size(); ++i) { a = (a + (unsigned char)raw[i]) % 65521; b = (b + a) % 65521; }
    Put32(z, (b << 16) | a);
    std::string header;
    Put32(header, unsigned(size)); Put32(header, unsigned(size));
    header += char(8); header += char(2); header += char(0); header += char(0); header += char(0);
    std::string out("\x89PNG\r\n\x1a\n", 8);
    Chunk(out, "IHDR", header);
    Chunk(out, "IDAT", z);
    Chunk(out, "IEND", "");
    return out;
}

bool Letter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
bool Digit(char c) { return c >= '0' && c <= '9'; }
char Upper(char c) { return c >= 'a' && c <= 'z' ? char(c - 'a' + 'A') : c; }
}

std::string GamertagInitials(const std::string& gamertag) {
    std::vector<std::string> words;
    std::string word;
    for(size_t i = 0; i <= gamertag.size(); ++i) {
        char c = i < gamertag.size() ? gamertag[i] : ' ';
        if(Letter(c) || Digit(c)) word += c;
        else if(!word.empty()) { words.push_back(word); word.clear(); }
    }
    if(words.empty()) return "?";
    std::string initials(1, Upper(words.front()[0]));
    if(words.size() > 1) initials += Upper(words.back()[0]);
    return initials;
}

unsigned GamerPictureColour(const std::string& gamertag) {
    unsigned hash = 2166136261u;
    for(size_t i = 0; i < gamertag.size(); ++i) { hash ^= (unsigned char)(gamertag[i] >= 'A' && gamertag[i] <= 'Z' ? gamertag[i] - 'A' + 'a' : gamertag[i]); hash *= 16777619u; }
    return Colours[hash % (sizeof(Colours) / sizeof(Colours[0]))];
}

std::string GamerPicturePng(const std::string& gamertag, int size) {
    if(size < 8) size = 8;
    if(size > 256) size = 256;
    unsigned colour = GamerPictureColour(gamertag);
    std::vector<unsigned char> rgb(size_t(size) * size * 3);
    for(int y = 0; y < size; ++y)
        for(int x = 0; x < size; ++x) {
            // A little lighter at the top, as the dashboard's own pictures.
            double light = 1.0 + 0.10 * (1.0 - 2.0 * (y + 0.5) / size);
            for(int k = 0; k < 3; ++k) {
                double v = ((colour >> (16 - 8 * k)) & 0xFF) * light;
                rgb[(size_t(y) * size + x) * 3 + k] = (unsigned char)(v > 255 ? 255 : v);
            }
        }
    std::string text = GamertagInitials(gamertag);
    stbtt_fontinfo font;
    if(!stbtt_InitFont(&font, RobotoMedium, stbtt_GetFontOffsetForIndex(RobotoMedium, 0))) return Png(rgb, size);
    // The letters' height (a capital's), as a share of the picture.
    double capShare = text.size() > 1 ? 0.36 : 0.44;
    int x0, y0, x1, y1;
    stbtt_GetCodepointBox(&font, 'H', &x0, &y0, &x1, &y1);
    float scale = float(capShare * size / (y1 > 0 ? y1 : 1));
    // Where each letter goes, and the box they make together.
    struct Placed { int code, left, top, width, height; };
    std::vector<Placed> placed;
    double pen = 0;
    int left = 1 << 30, right = -(1 << 30), top = 1 << 30, bottom = -(1 << 30);
    for(size_t i = 0; i < text.size(); ++i) {
        int code = (unsigned char)text[i];
        int advance, bearing;
        stbtt_GetCodepointHMetrics(&font, code, &advance, &bearing);
        int bx0, by0, bx1, by1;
        stbtt_GetCodepointBitmapBox(&font, code, scale, scale, &bx0, &by0, &bx1, &by1);
        Placed p = { code, int(floor(pen + 0.5)) + bx0, by0, bx1 - bx0, by1 - by0 };
        placed.push_back(p);
        if(p.width > 0) {
            if(p.left < left) left = p.left;
            if(p.left + p.width > right) right = p.left + p.width;
            if(p.top < top) top = p.top;
            if(p.top + p.height > bottom) bottom = p.top + p.height;
        }
        pen += advance * scale;
        if(i + 1 < text.size()) pen += stbtt_GetCodepointKernAdvance(&font, code, (unsigned char)text[i + 1]) * scale;
    }
    if(right <= left) return Png(rgb, size);
    int dx = (size - (right - left)) / 2 - left, dy = (size - (bottom - top)) / 2 - top;
    for(size_t i = 0; i < placed.size(); ++i) {
        const Placed& p = placed[i];
        if(p.width <= 0 || p.height <= 0) continue;
        std::vector<unsigned char> mask(size_t(p.width) * p.height);
        stbtt_MakeCodepointBitmap(&font, &mask[0], p.width, p.height, p.width, scale, scale, p.code);
        for(int y = 0; y < p.height; ++y)
            for(int x = 0; x < p.width; ++x) {
                int px = p.left + dx + x, py = p.top + dy + y;
                if(px < 0 || py < 0 || px >= size || py >= size) continue;
                unsigned alpha = mask[size_t(y) * p.width + x];
                unsigned char* at = &rgb[(size_t(py) * size + px) * 3];
                for(int k = 0; k < 3; ++k) at[k] = (unsigned char)((at[k] * (255 - alpha) + 255 * alpha + 127) / 255);
            }
    }
    return Png(rgb, size);
}
}
