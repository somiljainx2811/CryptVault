// unicode_font: glyphs for everything the 5x7 bitmap font (font5x7.h) can't
// draw - accented Latin, Greek, Cyrillic, CJK, symbols - so file names in
// any language show up correctly in the explorer.
//
// Colour emoji (including family/skin-tone/flag sequences) are drawn from
// the system's Noto Color Emoji font when installed (its CBDT bitmap
// format; the newer COLRv1 vector variant is not supported - without a
// usable colour font, emoji fall back to monochrome symbol glyphs or a gap).
//
// ASCII keeps using the crisp pixel font (so the app's look doesn't change);
// everything else comes from a TrueType font found on the system, rasterised
// with the single-header stb_truetype. When built with HarfBuzz
// (CRYPTVAULT_HAVE_HARFBUZZ, the default when it is available) runs of
// non-ASCII text are SHAPED: Arabic letters join, Devanagari/Bengali/Tamil/
// Thai... conjuncts and vowel signs are placed, Hebrew/Arabic run right to
// left. Without HarfBuzz it falls back to drawing one code point at a time
// (fine for Latin/Greek/Cyrillic/CJK; Hebrew/Arabic reordered but unjoined;
// Indic scripts not laid out). Shaping needs a font that covers the script:
// install e.g. Noto Sans Devanagari / Arabic / Thai (the usual
// google-noto-sans-*-fonts packages) - CryptVault looks for them by name.
//
// Fonts are discovered, never bundled: $CRYPTVAULT_FONT first, then common
// distro fonts under /usr/share/fonts, /usr/local/share/fonts and the
// user's ~/.local/share/fonts. Each font file is only read the first time a
// character it is needed for turns up.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ui/utf8.h"

namespace cryptvault {

struct UnicodeGlyph {
    int width = 0, height = 0;  // bitmap size in pixels (can be 0 for spaces)
    int xoff = 0, yoff = 0;     // top-left of the bitmap relative to the pen position on the baseline
    float advance = 6.0f;       // pen advance in pixels
    std::vector<uint8_t> alpha; // width*height coverage 0..255 (text glyphs: drawn in the label's colour)
    bool color = false;         // colour emoji: `rgba` holds the pixels and `alpha` is unused
    std::vector<uint8_t> rgba;  // width*height*4, straight (non-premultiplied) alpha
};



// Upper-cases Latin-1/Latin Extended-A/Greek/Cyrillic so non-ASCII text
// matches the bitmap font's all-caps look. Other code points unchanged.
uint32_t UpperCodepoint(uint32_t cp);

// One shaped run of text: positioned glyphs ready to draw. Coordinates are in
// the same base pixels as UnicodeGlyph (callers multiply by the label scale).
struct ShapedGlyph {
    const UnicodeGlyph* glyph = nullptr;  // null = nothing to draw (spaces, missing glyph)
    float x = 0.0f;                       // pen position of this glyph from the run start
    float y = 0.0f;                       // vertical shift from the baseline (down is positive)
};
struct ShapedRun {
    std::vector<ShapedGlyph> glyphs;
    float advance = 0.0f;                 // total width
};

// Shapes a run of text that contains non-ASCII characters (the renderer
// slices labels into ASCII, which the pixel font draws, and these runs).
// Upper-casing to match the pixel font's caps is applied here. The result is
// cached and shared, so the result stays valid however many runs are shaped after it.
//   direction: 0 = guess from the text, 1 = force left-to-right, 2 = force
//   right-to-left (used by the bidi layout, which has already decided).
std::shared_ptr<const ShapedRun> ShapeRun(const std::string& utf8_run, int direction = 0);

// True when HarfBuzz shaping is compiled in (diagnostics).
bool UnicodeShapingEnabled();

// Re-orders right-to-left runs (Hebrew, Arabic) from logical to visual order
// so they read correctly when drawn left to right. Deliberately minimal - not
// the full Unicode bidi algorithm: only runs of RTL letters (plus the spaces
// between them) are reversed, digits and Latin stay put. Hebrew then looks
// right; Arabic gets its letters in the right order but not their joined
// forms (that needs real shaping). Text with no RTL characters is returned
// unchanged.
std::string ReorderRtlRuns(const std::string& text);

// Glyph for `cp` at the app's base text size (cap height ~7px), or nullptr
// when no installed font has it (callers draw nothing / a gap). The pointer
// stays valid for the life of the process.
const UnicodeGlyph* GetUnicodeGlyph(uint32_t cp);

// Pen advance for `cp`, in the same base pixels as GetUnicodeGlyph().
float UnicodeAdvance(uint32_t cp);

// True if at least one usable font was found (diagnostics).
bool UnicodeFontAvailable();

}  // namespace cryptvault
