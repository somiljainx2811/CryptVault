#include "ui/bidi.h"

#include <algorithm>

#include "ui/utf8.h"

namespace cryptvault {

namespace {

enum class T : unsigned char { L, R, AL, EN, AN, ES, ET, CS, NSM, WS, ON };

bool InRange(uint32_t c, uint32_t a, uint32_t b) { return c >= a && c <= b; }

T Classify(uint32_t c) {
    if (c < 0x80) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return T::L;
        if (c >= '0' && c <= '9') return T::EN;
        switch (c) {
            case ' ': case '\t': return T::WS;
            case '+': case '-': return T::ES;
            case '#': case '$': case '%': return T::ET;
            case ',': case '.': case '/': case ':': return T::CS;
            default: return T::ON;
        }
    }
    if (c == 0xA0) return T::CS;
    if (InRange(c, 0xA2, 0xA5) || c == 0xB0 || c == 0xB1 || c == 0x20AC) return T::ET;
    if (InRange(c, 0x0300, 0x036F)) return T::NSM;
    // Hebrew
    if (InRange(c, 0x0591, 0x05BD) || c == 0x05BF || InRange(c, 0x05C1, 0x05C2) || InRange(c, 0x05C4, 0x05C5) || c == 0x05C7) return T::NSM;
    if (InRange(c, 0x0590, 0x05FF) || InRange(c, 0x07C0, 0x085F) || InRange(c, 0xFB1D, 0xFB4F)) return T::R;
    // Arabic
    if (InRange(c, 0x0660, 0x0669) || c == 0x066B || c == 0x066C) return T::AN;
    if (InRange(c, 0x06F0, 0x06F9)) return T::EN;
    if (InRange(c, 0x064B, 0x065F) || c == 0x0670 || InRange(c, 0x06D6, 0x06DC) || InRange(c, 0x06DF, 0x06E4)
        || InRange(c, 0x06E7, 0x06E8) || InRange(c, 0x06EA, 0x06ED)) return T::NSM;
    if (InRange(c, 0x0600, 0x07BF) || InRange(c, 0x0860, 0x08FF) || InRange(c, 0xFB50, 0xFDFF) || InRange(c, 0xFE70, 0xFEFF)) return T::AL;
    if (c == 0x200D || c == 0xFE0F || c == 0x20E3) return T::NSM;      // emoji glue travels with its base
    if (InRange(c, 0x2000, 0x200A) || c == 0x3000) return T::WS;
    if (InRange(c, 0x2010, 0x2027) || InRange(c, 0x2030, 0x205E) || InRange(c, 0x2190, 0x2BFF) || InRange(c, 0x3001, 0x3003)
        || c >= 0x1F000) return T::ON;                                   // punctuation, symbols, arrows, emoji
    return T::L;
}

bool IsNeutral(T t) { return t == T::WS || t == T::ON; }

}  // namespace

bool TextNeedsBidi(const std::string& utf8) {
    bool maybe = false;
    for (unsigned char c : utf8) {
        if (c >= 0xD6) { maybe = true; break; }   // U+0590 and up start with 0xD6 in UTF-8
    }
    if (!maybe) return false;
    for (size_t i = 0; i < utf8.size();) {
        const T t = Classify(DecodeUtf8(utf8, i));
        if (t == T::R || t == T::AL || t == T::AN) return true;
    }
    return false;
}

std::vector<BidiRun> BidiResolveRuns(const std::string& utf8) {
    std::vector<size_t> starts;
    std::vector<T> type;
    for (size_t i = 0; i < utf8.size();) {
        starts.push_back(i);
        type.push_back(Classify(DecodeUtf8(utf8, i)));
    }
    starts.push_back(utf8.size());
    const size_t n = type.size();
    std::vector<BidiRun> runs;
    if (n == 0) return runs;

    // W1: non-spacing marks take the type of the previous character (sos = L).
    for (size_t i = 0; i < n; ++i) {
        if (type[i] == T::NSM) type[i] = i == 0 ? T::L : type[i - 1];
    }
    // W2: a European number after Arabic-letter context becomes an Arabic number.
    // W3: AL -> R.
    {
        T last_strong = T::L;
        for (size_t i = 0; i < n; ++i) {
            if (type[i] == T::L || type[i] == T::R || type[i] == T::AL) last_strong = type[i];
            else if (type[i] == T::EN && last_strong == T::AL) type[i] = T::AN;
        }
        for (T& t : type) {
            if (t == T::AL) t = T::R;
        }
    }
    // W4: a single separator between two numbers of the same type joins them.
    for (size_t i = 1; i + 1 < n; ++i) {
        if (type[i] == T::ES && type[i - 1] == T::EN && type[i + 1] == T::EN) type[i] = T::EN;
        else if (type[i] == T::CS && type[i - 1] == type[i + 1] && (type[i - 1] == T::EN || type[i - 1] == T::AN)) type[i] = type[i - 1];
    }
    // W5: terminators next to a European number become part of it.
    for (size_t i = 0; i < n; ++i) {
        if (type[i] != T::ET) continue;
        size_t j = i;
        while (j < n && type[j] == T::ET) ++j;
        const bool touches_en = (i > 0 && type[i - 1] == T::EN) || (j < n && type[j] == T::EN);
        for (size_t k = i; k < j; ++k) type[k] = touches_en ? T::EN : type[k];
        i = j - 1;
    }
    // W6: leftover separators/terminators are neutral.
    for (T& t : type) {
        if (t == T::ES || t == T::ET || t == T::CS) t = T::ON;
    }
    // W7: a European number after left-to-right context is just left-to-right text.
    {
        T last_strong = T::L;
        for (size_t i = 0; i < n; ++i) {
            if (type[i] == T::L || type[i] == T::R) last_strong = type[i];
            else if (type[i] == T::EN && last_strong == T::L) type[i] = T::L;
        }
    }
    // N1/N2: a run of neutrals takes the direction of its neighbours if they agree
    // (numbers count as right-to-left), otherwise the paragraph direction (L).
    for (size_t i = 0; i < n; ++i) {
        if (!IsNeutral(type[i])) continue;
        size_t j = i;
        while (j < n && IsNeutral(type[j])) ++j;
        T before = i == 0 ? T::L : type[i - 1], after = j == n ? T::L : type[j];
        if (before == T::EN || before == T::AN) before = T::R;
        if (after == T::EN || after == T::AN) after = T::R;
        const T resolved = before == after ? before : T::L;
        for (size_t k = i; k < j; ++k) type[k] = resolved;
        i = j - 1;
    }
    // I1: levels for a left-to-right paragraph.
    std::vector<int> level(n);
    for (size_t i = 0; i < n; ++i) {
        level[i] = type[i] == T::R ? 1 : (type[i] == T::EN || type[i] == T::AN) ? 2 : 0;
    }
    // L1: trailing white space goes back to the paragraph level.
    for (size_t i = n; i > 0; --i) {
        const size_t k = i - 1;
        const uint32_t c = static_cast<unsigned char>(utf8[starts[k]]);
        if (c == ' ' || c == '\t') level[k] = 0; else break;
    }
    // Runs of equal level.
    size_t begin = 0;
    for (size_t i = 1; i <= n; ++i) {
        if (i == n || level[i] != level[begin]) {
            runs.push_back({starts[begin], starts[i], level[begin]});
            begin = i;
        }
    }
    return runs;
}

std::vector<size_t> BidiVisualOrder(const std::vector<BidiRun>& runs) {
    std::vector<size_t> order(runs.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    int highest = 0, lowest_odd = 1000;
    for (const BidiRun& r : runs) {
        highest = std::max(highest, r.level);
        if (r.level & 1) lowest_odd = std::min(lowest_odd, r.level);
    }
    // L2: from the highest level down to the lowest odd one, reverse every
    // maximal sequence of runs at that level or above.
    for (int level = highest; level >= lowest_odd && level >= 1; --level) {
        for (size_t i = 0; i < order.size();) {
            if (runs[order[i]].level < level) { ++i; continue; }
            size_t j = i;
            while (j < order.size() && runs[order[j]].level >= level) ++j;
            std::reverse(order.begin() + static_cast<long>(i), order.begin() + static_cast<long>(j));
            i = j;
        }
    }
    return order;
}

}  // namespace cryptvault
