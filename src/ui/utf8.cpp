#include "ui/utf8.h"

namespace cryptvault {

uint32_t DecodeUtf8(const std::string& text, size_t& i) {
    const auto byte = [&](size_t k) { return static_cast<unsigned char>(text[k]); };
    unsigned char c = byte(i);
    if (c < 0x80) { ++i; return c; }
    int extra = (c >= 0xF0 && c < 0xF8) ? 3 : (c >= 0xE0 && c < 0xF0) ? 2 : (c >= 0xC2 && c < 0xE0) ? 1 : -1;
    if (extra < 0 || i + extra >= text.size()) {
        ++i;
        return 0xFFFD;
    }
    uint32_t cp = c & (0x3F >> extra);
    for (int k = 1; k <= extra; ++k) {
        unsigned char cc = byte(i + k);
        if ((cc & 0xC0) != 0x80) { ++i; return 0xFFFD; }
        cp = (cp << 6) | (cc & 0x3F);
    }
    i += extra + 1;
    return cp;
}

void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

}  // namespace cryptvault
