#include "ui/unicode_font.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <unordered_map>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#ifdef CRYPTVAULT_HAVE_HARFBUZZ
#include <hb.h>
#endif

// Declarations only: the implementation lives in ui_renderer.cpp (same target).
#include "stb_image.h"

namespace fs = std::filesystem;

namespace cryptvault {

namespace {

// Roughly matches the 7px cap height of the bitmap font.
constexpr float kPixelHeight = 9.5f;

uint16_t Be16(const unsigned char* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t Be32(const unsigned char* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

// CBDT/CBLC: Google's colour-bitmap tables (PNG images per glyph). Only the
// largest strike is used; it is scaled down to the label size on demand.
struct ColorTables {
    bool ok = false;
    const unsigned char* cblc = nullptr;
    size_t cblc_len = 0;
    const unsigned char* cbdt = nullptr;
    size_t cbdt_len = 0;
    uint32_t array_offset = 0;   // IndexSubTableArray, relative to CBLC
    uint32_t sub_tables = 0;
};

struct LoadedFont {
    std::string path;
    bool tried = false;
    bool ok = false;
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
    float scale = 1.0f;
    ColorTables color;
    bool stb_ok = true;           // false: font has no outlines (colour-bitmap-only) - stb_truetype can't open it
    const unsigned char* cmap12 = nullptr;   // own code point -> glyph lookup for such fonts
    size_t cmap12_len = 0;
    const unsigned char* cmap4 = nullptr;
    size_t cmap4_len = 0;
    bool emoji_named = false;   // file name says "emoji": preferred for emoji code points
#ifdef CRYPTVAULT_HAVE_HARFBUZZ
    hb_blob_t* blob = nullptr;
    hb_face_t* face = nullptr;
    hb_font_t* hb = nullptr;
#endif
};

// sfnt table directory lookup.
bool FindTable(const std::vector<unsigned char>& data, int sfnt_offset, const char* tag, const unsigned char** table, size_t* length) {
    if (data.size() < static_cast<size_t>(sfnt_offset) + 12) return false;
    const unsigned char* base = data.data() + sfnt_offset;
    const uint16_t count = Be16(base + 4);
    for (uint16_t i = 0; i < count; ++i) {
        const size_t rec = static_cast<size_t>(sfnt_offset) + 12 + 16u * i;
        if (rec + 16 > data.size()) return false;
        const unsigned char* r = data.data() + rec;
        if (std::memcmp(r, tag, 4) != 0) continue;
        const uint32_t off = Be32(r + 8), len = Be32(r + 12);
        if (static_cast<size_t>(off) + len > data.size()) return false;
        *table = data.data() + off;
        *length = len;
        return true;
    }
    return false;
}

void FindColorTables(LoadedFont& font, int sfnt_offset) {
    ColorTables& c = font.color;
    if (!FindTable(font.data, sfnt_offset, "CBLC", &c.cblc, &c.cblc_len)) return;
    if (!FindTable(font.data, sfnt_offset, "CBDT", &c.cbdt, &c.cbdt_len)) return;
    if (c.cblc_len < 8) return;
    const uint32_t num_sizes = Be32(c.cblc + 4);
    int best_ppem = -1;
    for (uint32_t s = 0; s < num_sizes; ++s) {   // BitmapSize records are 48 bytes, starting at +8
        const size_t b = 8 + 48u * s;
        if (b + 48 > c.cblc_len) break;
        const int ppem = c.cblc[b + 44];
        if (ppem > best_ppem) {
            best_ppem = ppem;
            c.array_offset = Be32(c.cblc + b);
            c.sub_tables = Be32(c.cblc + b + 8);
        }
    }
    c.ok = best_ppem > 0 && c.sub_tables > 0;
}

// Finds a format 12 (full Unicode) and a format 4 (BMP) subtable in 'cmap'.
void FindCmap(LoadedFont& font, int sfnt_offset) {
    const unsigned char* cmap = nullptr;
    size_t len = 0;
    if (!FindTable(font.data, sfnt_offset, "cmap", &cmap, &len) || len < 4) return;
    const uint16_t count = Be16(cmap + 2);
    for (uint16_t i = 0; i < count; ++i) {
        const size_t rec = 4 + 8u * i;
        if (rec + 8 > len) return;
        const uint16_t platform = Be16(cmap + rec), encoding = Be16(cmap + rec + 2);
        const uint32_t off = Be32(cmap + rec + 4);
        if (static_cast<size_t>(off) + 4 > len) continue;
        const uint16_t format = Be16(cmap + off);
        const bool unicode = platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10));
        if (!unicode) continue;
        if (format == 12 && !font.cmap12) { font.cmap12 = cmap + off; font.cmap12_len = len - off; }
        if (format == 4 && !font.cmap4) { font.cmap4 = cmap + off; font.cmap4_len = len - off; }
    }
}

int OwnGlyphIndex(const LoadedFont& font, uint32_t cp) {
    if (font.cmap12 && font.cmap12_len >= 16) {
        const uint32_t groups = Be32(font.cmap12 + 12);
        for (uint32_t g = 0; g < groups; ++g) {
            const size_t o = 16 + 12u * g;
            if (o + 12 > font.cmap12_len) break;
            const uint32_t start = Be32(font.cmap12 + o), end = Be32(font.cmap12 + o + 4), glyph = Be32(font.cmap12 + o + 8);
            if (cp >= start && cp <= end) return static_cast<int>(glyph + (cp - start));
        }
        return 0;
    }
    if (font.cmap4 && cp < 0x10000 && font.cmap4_len >= 14) {
        const uint16_t seg_x2 = Be16(font.cmap4 + 6);
        const size_t end_codes = 14, start_codes = 16 + seg_x2, deltas = start_codes + seg_x2, range_offsets = deltas + seg_x2;
        for (size_t k = 0; k < seg_x2 / 2u; ++k) {
            if (range_offsets + 2 * k + 2 > font.cmap4_len) break;
            const uint16_t end = Be16(font.cmap4 + end_codes + 2 * k), start = Be16(font.cmap4 + start_codes + 2 * k);
            if (cp < start || cp > end) continue;
            const uint16_t delta = Be16(font.cmap4 + deltas + 2 * k), range = Be16(font.cmap4 + range_offsets + 2 * k);
            if (range == 0) return static_cast<uint16_t>(cp + delta);
            const size_t gi = range_offsets + 2 * k + range + 2 * (cp - start);
            if (gi + 2 > font.cmap4_len) return 0;
            const uint16_t g = Be16(font.cmap4 + gi);
            return g ? static_cast<uint16_t>(g + delta) : 0;
        }
    }
    return 0;
}

// Glyph index for a code point, whichever way this font is read.
int GlyphIndexOf(LoadedFont& font, uint32_t cp) {
    return font.stb_ok ? stbtt_FindGlyphIndex(&font.info, static_cast<int>(cp)) : OwnGlyphIndex(font, cp);
}

// Colour bitmap for one glyph, scaled to fit an `box` x `box` pixel square
// (alpha-weighted box filter), bottom-aligned just under the baseline.
bool ColorBitmapForGlyph(const ColorTables& c, int glyph_index, int box, UnicodeGlyph& out) {
    if (!c.ok) return false;
    for (uint32_t i = 0; i < c.sub_tables; ++i) {
        const size_t entry = static_cast<size_t>(c.array_offset) + 8u * i;
        if (entry + 8 > c.cblc_len) return false;
        const uint16_t first = Be16(c.cblc + entry), last = Be16(c.cblc + entry + 2);
        if (glyph_index < first || glyph_index > last) continue;
        const size_t table = static_cast<size_t>(c.array_offset) + Be32(c.cblc + entry + 4);
        if (table + 8 > c.cblc_len) return false;
        const uint16_t index_format = Be16(c.cblc + table), image_format = Be16(c.cblc + table + 2);
        const uint32_t data_base = Be32(c.cblc + table + 4);
        const uint32_t k = static_cast<uint32_t>(glyph_index - first);
        size_t pos = 0, avail = 0;
        if (index_format == 1) {
            const size_t o = table + 8 + 4u * k;
            if (o + 8 > c.cblc_len) return false;
            const uint32_t a = Be32(c.cblc + o), b = Be32(c.cblc + o + 4);
            if (b <= a) return false;
            pos = data_base + a;
            avail = b - a;
        } else if (index_format == 3) {
            const size_t o = table + 8 + 2u * k;
            if (o + 4 > c.cblc_len) return false;
            const uint16_t a = Be16(c.cblc + o), b = Be16(c.cblc + o + 2);
            if (b <= a) return false;
            pos = data_base + a;
            avail = b - a;
        } else if (index_format == 2) {
            if (table + 12 > c.cblc_len) return false;
            const uint32_t image_size = Be32(c.cblc + table + 8);
            pos = data_base + static_cast<size_t>(image_size) * k;
            avail = image_size;
        } else {
            return false;
        }
        if (pos + avail > c.cbdt_len || avail < 4) return false;
        const unsigned char* p = c.cbdt + pos;
        size_t skip = 0;
        if (image_format == 17) skip = 5;        // smallGlyphMetrics
        else if (image_format == 18) skip = 8;   // bigGlyphMetrics
        else if (image_format != 19) return false;  // only PNG formats
        if (avail < skip + 4) return false;
        const uint32_t png_len = Be32(p + skip);
        if (skip + 4 + png_len > avail) return false;
        int w = 0, h = 0, comp = 0;
        unsigned char* px = stbi_load_from_memory(p + skip + 4, static_cast<int>(png_len), &w, &h, &comp, 4);
        if (!px) return false;
        const float scale = static_cast<float>(box) / static_cast<float>(std::max(w, h));
        const int tw = std::max(1, static_cast<int>(w * scale + 0.5f)), th = std::max(1, static_cast<int>(h * scale + 0.5f));
        out.color = true;
        out.width = tw;
        out.height = th;
        out.advance = static_cast<float>(box + 1);
        out.xoff = (box + 1 - tw) / 2;
        out.yoff = 1 - th;                      // bottom edge one pixel under the baseline
        out.rgba.assign(static_cast<size_t>(tw) * th * 4, 0);
        for (int y = 0; y < th; ++y) {
            const int sy0 = y * h / th, sy1 = std::max(sy0 + 1, (y + 1) * h / th);
            for (int x = 0; x < tw; ++x) {
                const int sx0 = x * w / tw, sx1 = std::max(sx0 + 1, (x + 1) * w / tw);
                uint64_t r = 0, g = 0, b = 0, a = 0, n = 0;
                for (int sy = sy0; sy < sy1 && sy < h; ++sy) {
                    for (int sx = sx0; sx < sx1 && sx < w; ++sx) {
                        const unsigned char* q = px + (static_cast<size_t>(sy) * w + sx) * 4;
                        r += static_cast<uint64_t>(q[0]) * q[3];
                        g += static_cast<uint64_t>(q[1]) * q[3];
                        b += static_cast<uint64_t>(q[2]) * q[3];
                        a += q[3];
                        ++n;
                    }
                }
                unsigned char* d = &out.rgba[(static_cast<size_t>(y) * tw + x) * 4];
                if (a > 0) {
                    d[0] = static_cast<unsigned char>(r / a);
                    d[1] = static_cast<unsigned char>(g / a);
                    d[2] = static_cast<unsigned char>(b / a);
                    d[3] = static_cast<unsigned char>(a / std::max<uint64_t>(n, 1));
                }
            }
        }
        stbi_image_free(px);
        return true;
    }
    return false;
}

constexpr int kEmojiBox = 11;  // colour emoji are drawn about 11 px tall (cap height is 7)

class FontSet {
public:
    FontSet() { Discover(); }

    bool Available() const { return !fonts_.empty(); }

    const UnicodeGlyph* Get(uint32_t cp) {
        auto it = cache_.find(cp);
        if (it != cache_.end()) return it->second.get();
        std::unique_ptr<UnicodeGlyph> glyph;
        for (LoadedFont& font : fonts_) {
            if (!Load(font)) continue;
            int index = GlyphIndexOf(font, cp);
            if (index == 0) continue;
            glyph = std::make_unique<UnicodeGlyph>();
            if (ColorBitmapForGlyph(font.color, index, kEmojiBox, *glyph)) break;   // colour emoji
            if (!font.stb_ok) { glyph->advance = 0.0f; break; }                    // outline-less font, no bitmap: nothing to draw
            int adv = 0, lsb = 0;
            stbtt_GetGlyphHMetrics(&font.info, index, &adv, &lsb);
            glyph->advance = static_cast<float>(adv) * font.scale;
            int w = 0, h = 0, xoff = 0, yoff = 0;
            unsigned char* bitmap = stbtt_GetGlyphBitmap(&font.info, font.scale, font.scale, index, &w, &h, &xoff, &yoff);
            if (bitmap && w > 0 && h > 0) {
                glyph->width = w;
                glyph->height = h;
                glyph->xoff = xoff;
                glyph->yoff = yoff;
                glyph->alpha.assign(bitmap, bitmap + static_cast<size_t>(w) * h);
            }
            if (bitmap) stbtt_FreeBitmap(bitmap, nullptr);
            break;
        }
        const UnicodeGlyph* raw = glyph.get();
        cache_[cp] = std::move(glyph);  // cache misses too (nullptr), so we don't rescan fonts per frame
        return raw;
    }

    // Index of the first font that has a glyph for `cp`, or -1.
    int PickFont(uint32_t cp) {
        auto it = font_for_cp_.find(cp);
        if (it != font_for_cp_.end()) return it->second;
        int pick = -1;
        for (size_t i = 0; i < fonts_.size() && pick < 0; ++i) {
            if (Load(fonts_[i]) && GlyphIndexOf(fonts_[i], cp) != 0) pick = static_cast<int>(i);
        }
        font_for_cp_[cp] = pick;
        return pick;
    }

    // Like PickFont, but prefers the emoji font(s) first - for code points that are
    // meant to be emoji (the pictograph planes, ZWJ/variation selectors, ...).
    int PickEmojiFirst(uint32_t cp) {
        auto it = emoji_pick_.find(cp);
        if (it != emoji_pick_.end()) return it->second;
        int pick = -1;
        for (size_t i = 0; i < fonts_.size() && pick < 0; ++i) {
            if (!fonts_[i].emoji_named || !Load(fonts_[i])) continue;
            if (GlyphIndexOf(fonts_[i], cp) != 0) pick = static_cast<int>(i);
        }
        if (pick < 0) pick = PickFont(cp);
        emoji_pick_[cp] = pick;
        return pick;
    }

    // Rasterised glyph by glyph index (what shaping produces), cached per font.
    const UnicodeGlyph* GlyphByIndex(int font_index, int glyph_index) {
        const uint64_t key = (static_cast<uint64_t>(font_index) << 32) | static_cast<uint32_t>(glyph_index);
        auto it = index_cache_.find(key);
        if (it != index_cache_.end()) return it->second.get();
        LoadedFont& font = fonts_[font_index];
        auto glyph = std::make_unique<UnicodeGlyph>();
        if (ColorBitmapForGlyph(font.color, glyph_index, kEmojiBox, *glyph)) {
            const UnicodeGlyph* raw = glyph.get();
            index_cache_[key] = std::move(glyph);
            return raw;
        }
        if (!font.stb_ok) {   // e.g. the empty ZWJ / variation-selector glyphs of an emoji font
            glyph->advance = 0.0f;
            const UnicodeGlyph* raw = glyph.get();
            index_cache_[key] = std::move(glyph);
            return raw;
        }
        int adv = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(&font.info, glyph_index, &adv, &lsb);
        glyph->advance = static_cast<float>(adv) * font.scale;
        int w = 0, h = 0, xoff = 0, yoff = 0;
        unsigned char* bitmap = stbtt_GetGlyphBitmap(&font.info, font.scale, font.scale, glyph_index, &w, &h, &xoff, &yoff);
        if (bitmap && w > 0 && h > 0) {
            glyph->width = w;
            glyph->height = h;
            glyph->xoff = xoff;
            glyph->yoff = yoff;
            glyph->alpha.assign(bitmap, bitmap + static_cast<size_t>(w) * h);
        }
        if (bitmap) stbtt_FreeBitmap(bitmap, nullptr);
        const UnicodeGlyph* raw = glyph.get();
        index_cache_[key] = std::move(glyph);
        return raw;
    }

    LoadedFont& FontAt(int index) { return fonts_[index]; }
    std::unordered_map<std::string, std::shared_ptr<const ShapedRun>>& ShapeCache() { return shape_cache_; }

private:
    bool Load(LoadedFont& font) {
        if (font.tried) return font.ok;
        font.tried = true;
        std::ifstream f(font.path, std::ios::binary);
        if (!f) return false;
        font.data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        int offset = stbtt_GetFontOffsetForIndex(font.data.data(), 0);
        if (offset < 0) {
            font.data.clear();
            return false;
        }
        FindColorTables(font, offset);
        if (stbtt_InitFont(&font.info, font.data.data(), offset)) {
            font.scale = stbtt_ScaleForPixelHeight(&font.info, kPixelHeight);
        } else if (font.color.ok) {
            // Colour-bitmap-only font (Noto Color Emoji has no outlines, so stb_truetype
            // can't open it): read its character map ourselves; glyphs come from CBDT.
            font.stb_ok = false;
            FindCmap(font, offset);
            if (!font.cmap12 && !font.cmap4) {
                font.data.clear();
                return false;
            }
        } else {
            font.data.clear();
            return false;
        }
#ifdef CRYPTVAULT_HAVE_HARFBUZZ
        // HarfBuzz reads the same bytes; positions come back in font units, which
        // multiply by font.scale to give the pixel values stb_truetype rasterises at.
        font.blob = hb_blob_create(reinterpret_cast<const char*>(font.data.data()),
                                   static_cast<unsigned>(font.data.size()), HB_MEMORY_MODE_READONLY, nullptr, nullptr);
        font.face = hb_face_create(font.blob, 0);
        font.hb = hb_font_create(font.face);
        unsigned upem = hb_face_get_upem(font.face);
        hb_font_set_scale(font.hb, static_cast<int>(upem), static_cast<int>(upem));
        if (!font.stb_ok) font.scale = kPixelHeight / static_cast<float>(upem ? upem : 1000);
#endif
        font.ok = true;
        return true;
    }

    void Discover() {
        std::vector<std::string> wanted_in_order;
        std::vector<LoadedFont> emoji_candidates;
        std::map<std::string, std::string> found;  // lowercase basename -> path

        if (const char* env = std::getenv("CRYPTVAULT_FONT")) {
            if (*env) fonts_.push_back(LoadedFont{env});
        }

        std::vector<fs::path> roots = {"/usr/share/fonts", "/usr/local/share/fonts", "/usr/share/texmf/fonts"};
        if (const char* home = std::getenv("HOME")) {
            roots.push_back(fs::path(home) / ".local/share/fonts");
            roots.push_back(fs::path(home) / ".fonts");
        }
#ifdef _WIN32
        roots.push_back("C:\\Windows\\Fonts");
#endif
        size_t visited = 0;
        for (const fs::path& root : roots) {
            std::error_code ec;
            if (!fs::is_directory(root, ec)) continue;
            for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end && visited < 20000; it.increment(ec), ++visited) {
                std::string ext = it->path().extension().string();
                for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (ext != ".ttf" && ext != ".ttc" && ext != ".otf") continue;
                std::string base = it->path().filename().string();
                for (char& c : base) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                found.emplace(base, it->path().string());
            }
        }

        // Latin/Cyrillic/Greek first, then wide-coverage CJK fallbacks, then
        // symbol-ish fonts. Plain TrueType is preferred: stb_truetype handles
        // TrueType outlines best (CFF-flavoured .otf support is basic).
        static const char* kPreferred[] = {
            "notosans-regular.ttf",     "dejavusans.ttf",          "liberationsans-regular.ttf",
            "arial.ttf",                "segoeui.ttf",             "ubuntu-r.ttf",
            "freesans.ttf",             "opensans-regular.ttf",    "roboto-regular.ttf",
            // CJK and wide-coverage fallbacks
            "notosanscjk-regular.ttc",  "notosanscjksc-regular.otf", "sourcehansans-regular.ttc",
            "wqy-microhei.ttc",         "wqy-zenhei.ttc",          "droidsansfallbackfull.ttf",
            "msyh.ttc",                 "msgothic.ttc",            "unifont.ttf",
            "notosanssymbols-regular.ttf", "notosanssymbols2-regular.ttf", "dejavusansmono.ttf",
        };
        for (const char* name : kPreferred) {
            auto it = found.find(name);
            if (it != found.end()) fonts_.push_back(LoadedFont{it->second});
        }
        // Emoji last: the colour font also holds glyphs for (c), (R), TM, arrows, ...
        // that should stay plain text, so it is only chosen first for code points
        // that are really emoji (see PickEmojiFirst).
        static const char* kEmojiFonts[] = {"notocoloremoji.ttf", "notocoloremoji-regular.ttf", "notoemoji-regular.ttf",
                                            "notoemoji[wght].ttf", "twemoji.ttf", "twemojimozilla.ttf"};
        for (const char* name : kEmojiFonts) {
            auto it = found.find(name);
            if (it != found.end()) { LoadedFont f{it->second}; f.emoji_named = true; emoji_candidates.push_back(f); }
        }
        // Script-specific fonts (what shaping needs for Indic, Thai, Arabic, ...).
        // Matched by file-name prefix so "NotoSansDevanagari-Regular.ttf" and
        // "NotoSansDevanagari[wdth,wght].ttf" both work; regular weight preferred.
        static const char* kScriptPrefixes[] = {
            "notosansarabic",   "notosanshebrew",   "notosansdevanagari", "notosansbengali", "notosanstamil",
            "notosanstelugu",   "notosanskannada",  "notosansmalayalam",  "notosansgujarati", "notosansgurmukhi",
            "notosansthai",     "notosanslao",      "notosanskhmer",      "notosansmyanmar",  "notosanssinhala",
            "notosansarmenian", "notosansgeorgian", "notosansethiopic",   "notosanstibetan",
        };
        static const char* kSkip[] = {"bold", "italic", "light", "thin", "black", "medium", "semi", "extra", "condensed", "ui-"};
        for (const char* prefix : kScriptPrefixes) {
            std::string best_name, best_path;
            for (const auto& entry : found) {
                const std::string& base = entry.first;
                if (base.rfind(prefix, 0) != 0) continue;
                bool skip = false;
                for (const char* bad : kSkip) skip = skip || base.find(bad) != std::string::npos;
                if (skip || base.find("ui") == std::string(prefix).size()) continue;  // "...ui-regular" variants
                if (best_name.empty() || base.size() < best_name.size()) { best_name = base; best_path = entry.second; }
            }
            if (!best_path.empty()) fonts_.push_back(LoadedFont{best_path});
        }
        for (LoadedFont& f : emoji_candidates) fonts_.push_back(std::move(f));
    }

    std::vector<LoadedFont> fonts_;
    std::unordered_map<uint32_t, std::unique_ptr<UnicodeGlyph>> cache_;
    std::unordered_map<uint32_t, int> font_for_cp_;
    std::unordered_map<uint32_t, int> emoji_pick_;
    std::unordered_map<uint64_t, std::unique_ptr<UnicodeGlyph>> index_cache_;
    std::unordered_map<std::string, std::shared_ptr<const ShapedRun>> shape_cache_;
};

FontSet& Fonts() {
    static FontSet set;  // discovery (a directory scan) happens on first use, i.e. the first non-ASCII character
    return set;
}

}  // namespace


uint32_t UpperCodepoint(uint32_t cp) {
    if (cp < 0x80) return cp;
    if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7) return cp - 0x20;          // Latin-1
    if (cp == 0xFF) return 0x178;
    if (cp >= 0x100 && cp <= 0x137) return (cp & 1) ? cp - 1 : cp;         // Latin Extended-A
    if (cp >= 0x139 && cp <= 0x148) return (cp & 1) ? cp : cp - 1;
    if (cp >= 0x14A && cp <= 0x177) return (cp & 1) ? cp - 1 : cp;
    if (cp == 0x17A || cp == 0x17C || cp == 0x17E) return cp - 1;
    if (cp == 0x3C2) return 0x3A3;                                          // Greek final sigma
    if (cp >= 0x3B1 && cp <= 0x3C9) return cp - 0x20;                       // Greek
    if (cp >= 0x430 && cp <= 0x44F) return cp - 0x20;                       // Cyrillic
    if (cp >= 0x450 && cp <= 0x45F) return cp - 0x50;
    return cp;
}

namespace {

bool IsRtlLetter(uint32_t cp) {
    return (cp >= 0x590 && cp <= 0x8FF) || (cp >= 0xFB1D && cp <= 0xFDFF) || (cp >= 0xFE70 && cp <= 0xFEFF);
}


}  // namespace

std::string ReorderRtlRuns(const std::string& text) {
    // Fast exit: the first RTL code point (U+0590) starts with byte 0xD6 in UTF-8.
    bool maybe = false;
    for (unsigned char c : text) {
        if (c >= 0xD6) { maybe = true; break; }
    }
    if (!maybe) return text;

    std::vector<uint32_t> cps;
    for (size_t i = 0; i < text.size();) cps.push_back(DecodeUtf8(text, i));
    bool any = false;
    for (uint32_t cp : cps) any = any || IsRtlLetter(cp);
    if (!any) return text;

    std::string out;
    size_t i = 0;
    while (i < cps.size()) {
        if (!IsRtlLetter(cps[i])) {
            AppendUtf8(out, cps[i++]);
            continue;
        }
        // A run: RTL letters, allowing single spaces that are followed by more RTL letters.
        size_t end = i;
        while (end < cps.size()) {
            if (IsRtlLetter(cps[end])) { ++end; continue; }
            if (cps[end] == ' ' && end + 1 < cps.size() && IsRtlLetter(cps[end + 1])) { ++end; continue; }
            break;
        }
        for (size_t k = end; k > i; --k) AppendUtf8(out, cps[k - 1]);
        i = end;
    }
    return out;
}

namespace {

std::string UpperRun(const std::string& run) {
    std::string out;
    for (size_t i = 0; i < run.size();) AppendUtf8(out, UpperCodepoint(DecodeUtf8(run, i)));
    return out;
}

}  // namespace

bool UnicodeShapingEnabled() {
#ifdef CRYPTVAULT_HAVE_HARFBUZZ
    return true;
#else
    return false;
#endif
}

std::shared_ptr<const ShapedRun> ShapeRun(const std::string& run_in, int direction) {
    FontSet& set = Fonts();
    auto& cache = set.ShapeCache();
    const std::string key = std::string(1, static_cast<char>('0' + direction)) + run_in;
    auto cached = cache.find(key);
    if (cached != cache.end()) return cached->second;
    if (cache.size() > 4096) cache.clear();   // bounded; labels are re-shaped on demand (callers keep their own shared_ptr)
    auto result = std::make_shared<ShapedRun>();
    ShapedRun& out = *result;

    const std::string run = UpperRun(run_in);
    std::vector<uint32_t> cps;
    for (size_t i = 0; i < run.size();) cps.push_back(DecodeUtf8(run, i));

#ifdef CRYPTVAULT_HAVE_HARFBUZZ
    // Which font draws each code point. Emoji (pictograph planes, ZWJ/variation
    // selectors, anything glued to them) prefer the emoji font so whole sequences
    // such as family or skin-tone emoji end up in ONE shaping run and combine.
    auto is_emoji_cp = [](uint32_t cp) {
        return cp >= 0x1F000 || cp == 0x200D || cp == 0xFE0F || cp == 0x20E3 || (cp >= 0xE0020 && cp <= 0xE007F);
    };
    std::vector<int> pick(cps.size());
    for (size_t k = 0; k < cps.size(); ++k) {
        const bool glued_to_emoji = (k + 1 < cps.size() && cps[k + 1] == 0xFE0F) || (k > 0 && cps[k - 1] == 0x200D);
        pick[k] = (is_emoji_cp(cps[k]) || glued_to_emoji) ? set.PickEmojiFirst(cps[k]) : set.PickFont(cps[k]);
    }
    // Split into stretches that one font covers, shape each with HarfBuzz.
    size_t i = 0;
    float pen = 0.0f;
    while (i < cps.size()) {
        const int font_index = pick[i];
        size_t j = i + 1;
        while (j < cps.size() && pick[j] == font_index) ++j;
        if (font_index < 0) {                       // no installed font has these: leave a gap
            for (size_t k = i; k < j; ++k) {
                const uint32_t cp = cps[k];
                const bool invisible = cp == 0x200D || cp == 0xFE0F || cp == 0xFE0E || (cp >= 0x200B && cp <= 0x200F);
                if (!invisible) pen += 6.0f;
            }
            i = j;
            continue;
        }
        LoadedFont& font = set.FontAt(font_index);
        hb_buffer_t* buffer = hb_buffer_create();
        hb_buffer_add_codepoints(buffer, cps.data(), static_cast<int>(cps.size()), static_cast<unsigned>(i),
                                 static_cast<int>(j - i));
        if (direction == 1) hb_buffer_set_direction(buffer, HB_DIRECTION_LTR);       // the bidi layout already decided
        else if (direction == 2) hb_buffer_set_direction(buffer, HB_DIRECTION_RTL);
        hb_buffer_guess_segment_properties(buffer);  // script and language (and direction, unless set above)
        hb_shape(font.hb, buffer, nullptr, 0);
        unsigned count = 0;
        hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buffer, &count);
        hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buffer, &count);
        for (unsigned k = 0; k < count; ++k) {
            ShapedGlyph g;
            g.glyph = set.GlyphByIndex(font_index, static_cast<int>(info[k].codepoint));
            g.x = pen + static_cast<float>(pos[k].x_offset) * font.scale;
            g.y = -static_cast<float>(pos[k].y_offset) * font.scale;   // HarfBuzz y is up, the screen's is down
            out.glyphs.push_back(g);
            // Bitmap fonts carry their own advance (hb's would be in outline units); empty glyphs add none.
            pen += font.stb_ok ? static_cast<float>(pos[k].x_advance) * font.scale : (g.glyph ? g.glyph->advance : 0.0f);
        }
        hb_buffer_destroy(buffer);
        i = j;
    }
    out.advance = pen;
#else
    // No shaping: visual order for RTL runs, then one glyph per code point.
    float pen = 0.0f;
    std::string visual = run;
    if (direction == 2) {            // forced right-to-left: reverse the whole run
        std::string reversed;
        for (size_t k = cps.size(); k > 0; --k) AppendUtf8(reversed, cps[k - 1]);
        visual = reversed;
    } else if (direction == 0) {
        visual = ReorderRtlRuns(run);
    }
    for (size_t i = 0; i < visual.size();) {
        uint32_t cp = DecodeUtf8(visual, i);
        ShapedGlyph g;
        g.glyph = GetUnicodeGlyph(cp);
        g.x = pen;
        out.glyphs.push_back(g);
        pen += UnicodeAdvance(cp);
    }
    out.advance = pen;
#endif
    cache[key] = result;
    return result;
}

const UnicodeGlyph* GetUnicodeGlyph(uint32_t cp) { return Fonts().Get(cp); }

float UnicodeAdvance(uint32_t cp) {
    const UnicodeGlyph* glyph = Fonts().Get(cp);
    if (glyph) return std::max(1.0f, glyph->advance);
    return 6.0f;
}

bool UnicodeFontAvailable() { return Fonts().Available(); }

}  // namespace cryptvault
