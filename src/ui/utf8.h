// Minimal UTF-8 helpers shared by the text code (kept free of fonts/GPU so
// they can be unit-tested on their own).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace cryptvault {

// Decodes the UTF-8 sequence starting at text[i], advancing i. Malformed
// bytes decode to U+FFFD and advance one byte, so it never loops forever.
uint32_t DecodeUtf8(const std::string& text, size_t& i);

// Appends `cp` to `out` encoded as UTF-8.
void AppendUtf8(std::string& out, uint32_t cp);

}  // namespace cryptvault
