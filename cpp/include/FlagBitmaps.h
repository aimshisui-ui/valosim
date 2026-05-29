#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace vlr {

// ---------------------------------------------------------------------------
// Embedded pixel data for ~30 country flags.
//
// Each flag is a small hand-coded bitmap at kFlagWidth x kFlagHeight pixels
// (24x16 = 384 px, roughly 3:2 aspect close to the real 1.5:1 flag standard).
// At ~31 flags x 384 px x 4 bytes = ~48 KB of static data total, which is an
// acceptable binary cost for clean, recognizable flags at small UI sizes
// (16-32 px tall) without depending on an external image-loading pipeline.
//
// Storage format: ARGB, row-major. `pixels[y*width + x]` for top-down access.
// Each pixel is 0xAARRGGBB packed into a uint32 (alpha is always 0xFF for
// flag data — we don't need transparency for opaque flag rectangles).
// ---------------------------------------------------------------------------

struct FlagBitmap {
    int width  = 0;
    int height = 0;
    const std::uint32_t* pixels = nullptr;  // ARGB, row-major, len = w*h
};

// Returns a bitmap with `pixels == nullptr` and zero dimensions if `iso` is
// not in the known country list. Caller should treat that as "no flag" and
// fall back to a procedural placeholder.
FlagBitmap get_flag_bitmap(std::string_view iso);

constexpr int kFlagWidth  = 24;
constexpr int kFlagHeight = 16;

}  // namespace vlr
