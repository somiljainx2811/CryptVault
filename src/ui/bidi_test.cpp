// Headless checks for the bidi module (no fonts or GPU needed).
#include "ui/bidi.h"

#include <cstdio>

using namespace cryptvault;

static int g_pass = 0, g_fail = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("  [FAIL] %s (line %d)\n", #c, __LINE__); } } while (0)

// Visual rendering of the runs as "[level:text]" in left-to-right layout order, for readable assertions.
static std::string Visual(const std::string& s) {
    auto runs = BidiResolveRuns(s);
    std::string out;
    for (size_t idx : BidiVisualOrder(runs)) {
        out += "[" + std::to_string(runs[idx].level) + ":" + s.substr(runs[idx].begin, runs[idx].end - runs[idx].begin) + "]";
    }
    return out;
}

int main() {
    const std::string shalom = "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D";        // שלום
    const std::string olam = "\xD7\xA2\xD7\x95\xD7\x9C\xD7\x9D";          // עולם
    const std::string taqrir = "\xD8\xAA\xD9\x82\xD8\xB1\xD9\x8A\xD8\xB1"; // تقرير (Arabic letters)

    std::printf("=== needs-bidi detection ===\n");
    CHECK(!TextNeedsBidi("plain ascii 123"));
    CHECK(!TextNeedsBidi("r\xC3\xA9sum\xC3\xA9 \xD0\x9F\xD1\x80\xD0\xB8"));   // accents, Cyrillic: no
    CHECK(TextNeedsBidi(shalom) && TextNeedsBidi(taqrir));

    std::printf("=== run levels and visual order ===\n");
    CHECK(Visual("abc def") == "[0:abc def]");
    CHECK(Visual(shalom) == "[1:" + shalom + "]");
    // Hebrew inside Latin: the spaces take the paragraph direction, order unchanged.
    CHECK(Visual("abc " + shalom + " def") == "[0:abc ][1:" + shalom + "][0: def]");
    // Two Hebrew words around a number: one RTL region, so the WHOLE sequence reverses
    // and the number stays readable left to right.
    {
        auto v = Visual(shalom + " 2024 " + olam);
        // expect: olam first (leftmost), then the number, then shalom (rightmost)
        CHECK(v.find(olam) < v.find("2024") && v.find("2024") < v.find(shalom));
    }
    // Arabic word + year + extension: year gets the Arabic-number level, ".pdf" is LTR again.
    {
        auto runs = BidiResolveRuns(taqrir + " 2024.pdf");
        bool saw_number_level2 = false, saw_ext_level0 = false;
        for (auto& r : runs) {
            std::string t = (taqrir + " 2024.pdf").substr(r.begin, r.end - r.begin);
            if (t == "2024") saw_number_level2 = r.level == 2;
            if (t == ".pdf") saw_ext_level0 = r.level == 0;
        }
        CHECK(saw_number_level2 && saw_ext_level0);
        auto v = Visual(taqrir + " 2024.pdf");
        CHECK(v.find("2024") < v.find(taqrir) && v.find(taqrir) < v.find(".pdf"));   // "2024 <arabic>.pdf" left to right
    }
    // Digits after Latin stay left-to-right text (W7) - nothing reorders.
    CHECK(Visual("file 2024 " + shalom) == "[0:file 2024 ][1:" + shalom + "]");
    // Trailing white space returns to the paragraph level.
    {
        auto runs = BidiResolveRuns(shalom + "  ");
        CHECK(runs.size() == 2 && runs.back().level == 0);
    }
    CHECK(BidiResolveRuns("").empty());

    std::printf("\n=== Summary: %d/%d checks passed ===\n", g_pass, g_pass + g_fail);
    return g_fail == 0 ? 0 : 1;
}
