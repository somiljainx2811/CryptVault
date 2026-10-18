// bidi: Unicode bidirectional layout (a compact implementation of the core of
// UAX #9) for labels that mix right-to-left text (Hebrew, Arabic) with digits,
// Latin and punctuation - e.g. an Arabic word followed by a year and ".pdf".
//
// The paragraph direction is always left-to-right (the UI is LTR), so a label
// reads left to right overall and right-to-left runs sit inside it. Resolved:
// strong types (L, R, AL), numbers (EN/AN and their W1-W7 rules), neutrals
// (N1/N2), implicit levels (I1/I2) and the visual reordering (L2). Not
// handled: explicit embeddings/overrides/isolates (LRE, RLO, ...), which file
// names practically never contain, and bracket pairing (N0).
//
// Shaping is separate: callers shape each level run with the matching
// direction (see unicode_font.h) and place the runs in the order returned
// here.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace cryptvault {

struct BidiRun {
    size_t begin = 0, end = 0;  // byte range in the UTF-8 text
    int level = 0;              // 0 = left-to-right, 1 = right-to-left, 2 = number/Latin inside RTL (LTR)
};

// True if the text contains any right-to-left character or Arabic digit, i.e.
// if bidi processing can change anything. Cheap; use it to skip the rest.
bool TextNeedsBidi(const std::string& utf8);

// Splits `utf8` into runs of equal embedding level, in LOGICAL order.
std::vector<BidiRun> BidiResolveRuns(const std::string& utf8);

// Order in which to lay the runs out left to right (indices into `runs`).
std::vector<size_t> BidiVisualOrder(const std::vector<BidiRun>& runs);

}  // namespace cryptvault
