#include "FlagBitmaps.h"

#include <algorithm>
#include <cstring>
#include <cstdint>

namespace vlr {

namespace {

// ---------------------------------------------------------------------------
// Color palette. ARGB packed: 0xAARRGGBB.
// Chosen to be vivid-but-not-saturated, matching the rest of the UI's
// muted-clean aesthetic (PROJECT_GUIDE §5.2). Adding per-flag accents below
// where the canonical national hue diverges from the base palette.
// ---------------------------------------------------------------------------
constexpr std::uint32_t kRed        = 0xFFD32F2F;  // generic flag red
constexpr std::uint32_t kBlue       = 0xFF1A4E8E;  // generic flag blue
constexpr std::uint32_t kWhite      = 0xFFFFFFFF;
constexpr std::uint32_t kBlack      = 0xFF111111;
constexpr std::uint32_t kYellow     = 0xFFF4C20D;
constexpr std::uint32_t kGreen      = 0xFF2E7D32;
constexpr std::uint32_t kOrange     = 0xFFE65100;

// Per-flag accent shades — flagged where the country's canonical color
// differs noticeably from the generics.
constexpr std::uint32_t kBR_Green    = 0xFF009C3B;  // Brazil deep green
constexpr std::uint32_t kBR_Yellow   = 0xFFFFDF00;  // Brazil saturated yellow
constexpr std::uint32_t kBR_Blue     = 0xFF002776;  // Brazil dark blue
constexpr std::uint32_t kAR_SkyBlue  = 0xFF74ACDF;  // Argentina sky-blue
constexpr std::uint32_t kSE_Blue     = 0xFF005293;  // Sweden Nordic blue
constexpr std::uint32_t kSE_Yellow   = 0xFFFECC00;  // Sweden cross yellow
constexpr std::uint32_t kFI_Blue     = 0xFF003580;  // Finland cross blue
constexpr std::uint32_t kDK_Red      = 0xFFC60C30;  // Denmark Dannebrog red
constexpr std::uint32_t kNO_Red      = 0xFFEF2B2D;  // Norway red
constexpr std::uint32_t kNO_Blue     = 0xFF002868;  // Norway blue
constexpr std::uint32_t kIE_Green    = 0xFF169B62;  // Ireland green
constexpr std::uint32_t kIE_Orange   = 0xFFFF883E;  // Ireland orange
constexpr std::uint32_t kIT_Green    = 0xFF008C45;  // Italy green
constexpr std::uint32_t kIT_Red      = 0xFFCD212A;  // Italy red
constexpr std::uint32_t kPL_Red      = 0xFFDC143C;  // Poland crimson
constexpr std::uint32_t kNL_Red      = 0xFFAE1C28;  // Netherlands red
constexpr std::uint32_t kNL_Blue     = 0xFF21468B;  // Netherlands blue
constexpr std::uint32_t kES_Red      = 0xFFAA151B;  // Spain red
constexpr std::uint32_t kES_Yellow   = 0xFFF1BF00;  // Spain yellow
constexpr std::uint32_t kUA_Blue     = 0xFF0057B7;  // Ukraine blue
constexpr std::uint32_t kUA_Yellow   = 0xFFFFD700;  // Ukraine yellow
constexpr std::uint32_t kRU_Blue     = 0xFF0039A6;  // Russia blue
constexpr std::uint32_t kRU_Red      = 0xFFD52B1E;  // Russia red
constexpr std::uint32_t kSCT_Blue    = 0xFF0065BD;  // Scotland St Andrew blue
constexpr std::uint32_t kJP_Red      = 0xFFBC002D;  // Japan red
constexpr std::uint32_t kKR_Red      = 0xFFCD2E3A;  // Korea red
constexpr std::uint32_t kKR_Blue     = 0xFF0047A0;  // Korea blue
constexpr std::uint32_t kVN_Red      = 0xFFDA251D;  // Vietnam red
constexpr std::uint32_t kPH_Blue     = 0xFF0038A8;  // Philippines blue
constexpr std::uint32_t kPH_Red      = 0xFFCE1126;  // Philippines red
constexpr std::uint32_t kPH_Yellow   = 0xFFFCD116;  // Philippines yellow
constexpr std::uint32_t kID_Red      = 0xFFCE1126;  // Indonesia red
constexpr std::uint32_t kCN_Red      = 0xFFEE1C25;  // China red
constexpr std::uint32_t kCN_Yellow   = 0xFFFFFF00;  // China yellow
constexpr std::uint32_t kSG_Red      = 0xFFEF3340;  // Singapore red
constexpr std::uint32_t kTH_Blue     = 0xFF2D2A4A;  // Thailand navy
constexpr std::uint32_t kTH_Red      = 0xFFA51931;  // Thailand red
constexpr std::uint32_t kDE_Red      = 0xFFDD0000;  // Germany red
constexpr std::uint32_t kDE_Gold     = 0xFFFFCE00;  // Germany gold
constexpr std::uint32_t kFR_Blue     = 0xFF0055A4;  // France blue
constexpr std::uint32_t kFR_Red      = 0xFFEF4135;  // France red
constexpr std::uint32_t kMX_Green    = 0xFF006847;  // Mexico green
constexpr std::uint32_t kMX_Red      = 0xFFCE1126;  // Mexico red
constexpr std::uint32_t kCL_Red      = 0xFFD52B1E;  // Chile red
constexpr std::uint32_t kCL_Blue     = 0xFF0039A6;  // Chile blue
constexpr std::uint32_t kAU_Blue     = 0xFF012169;  // Australia blue
constexpr std::uint32_t kAU_Red      = 0xFFE4002B;  // Australia red
constexpr std::uint32_t kTR_Red      = 0xFFE30A17;  // Turkey red

constexpr int W = kFlagWidth;
constexpr int H = kFlagHeight;
constexpr int N = W * H;

// ---------------------------------------------------------------------------
// Tiny composition helpers — these let each flag be defined in roughly 5-15
// short calls rather than hand-typing 384-element arrays.
// ---------------------------------------------------------------------------

void fill_all(std::uint32_t* px, std::uint32_t color) {
    for (int i = 0; i < N; ++i) px[i] = color;
}

void fill_rect(std::uint32_t* px, int x, int y, int w, int h, std::uint32_t color) {
    int x0 = std::max(0, x);
    int y0 = std::max(0, y);
    int x1 = std::min(W, x + w);
    int y1 = std::min(H, y + h);
    for (int yy = y0; yy < y1; ++yy) {
        for (int xx = x0; xx < x1; ++xx) {
            px[yy * W + xx] = color;
        }
    }
}

void fill_row(std::uint32_t* px, int y, std::uint32_t color) {
    fill_rect(px, 0, y, W, 1, color);
}

void fill_col(std::uint32_t* px, int x, std::uint32_t color) {
    fill_rect(px, x, 0, 1, H, color);
}

void set_px(std::uint32_t* px, int x, int y, std::uint32_t color) {
    if (x >= 0 && x < W && y >= 0 && y < H) px[y * W + x] = color;
}

// Three equal horizontal bands. Top fills rows 0..h1, middle h1..h2, bottom h2..H.
void horizontal_tri(std::uint32_t* px, std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    // 16 / 3 doesn't divide evenly — split as 5 / 6 / 5 (close enough at this size).
    fill_rect(px, 0, 0, W, 5, a);
    fill_rect(px, 0, 5, W, 6, b);
    fill_rect(px, 0, 11, W, 5, c);
}

// Three equal vertical bands. 24 / 3 = 8 px each. Clean.
void vertical_tri(std::uint32_t* px, std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    fill_rect(px, 0,  0,  8, H, a);
    fill_rect(px, 8,  0,  8, H, b);
    fill_rect(px, 16, 0,  8, H, c);
}

// Nordic cross. Vertical line at column 8 (offset left of center per
// Nordic convention — 8/24 = exact 1/3). Horizontal line at row 7-8.
// `field` is the background, `cross` is the cross color.
void nordic_cross(std::uint32_t* px, std::uint32_t field, std::uint32_t cross) {
    fill_all(px, field);
    fill_rect(px, 0, 7, W, 2, cross);   // horizontal bar
    fill_rect(px, 8, 0, 2, H, cross);   // vertical bar (2px wide)
}

// Filled axis-aligned diamond shape inside [cx-r..cx+r, cy-r..cy+r].
// Used for Japan disc approximation and small accent marks.
void draw_diamond(std::uint32_t* px, int cx, int cy, int r, std::uint32_t color) {
    for (int yy = -r; yy <= r; ++yy) {
        int span = r - std::abs(yy);
        for (int xx = -span; xx <= span; ++xx) {
            set_px(px, cx + xx, cy + yy, color);
        }
    }
}

// Approximate a filled disc / circle at (cx, cy) with radius r.
// At 4-6 px radii this looks like a hand-pixel-art blob, which is fine.
void draw_disc(std::uint32_t* px, int cx, int cy, int r, std::uint32_t color) {
    int r2 = r * r;
    for (int yy = -r; yy <= r; ++yy) {
        for (int xx = -r; xx <= r; ++xx) {
            if (xx * xx + yy * yy <= r2) {
                set_px(px, cx + xx, cy + yy, color);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Per-flag bitmaps. One static buffer per ISO. Initialized lazily on first
// call to get_flag_bitmap. ImGui is single-threaded (PROJECT_GUIDE §12), so
// no synchronization needed.
// ---------------------------------------------------------------------------

std::uint32_t s_flag_us[N];
std::uint32_t s_flag_ca[N];
std::uint32_t s_flag_br[N];
std::uint32_t s_flag_mx[N];
std::uint32_t s_flag_cl[N];
std::uint32_t s_flag_ar[N];
std::uint32_t s_flag_de[N];
std::uint32_t s_flag_se[N];
std::uint32_t s_flag_gb[N];
std::uint32_t s_flag_ua[N];
std::uint32_t s_flag_fr[N];
std::uint32_t s_flag_tr[N];
std::uint32_t s_flag_fi[N];
std::uint32_t s_flag_sct[N];
std::uint32_t s_flag_ie[N];
std::uint32_t s_flag_es[N];
std::uint32_t s_flag_ru[N];
std::uint32_t s_flag_pl[N];
std::uint32_t s_flag_nl[N];
std::uint32_t s_flag_it[N];
std::uint32_t s_flag_dk[N];
std::uint32_t s_flag_no[N];
std::uint32_t s_flag_au[N];
std::uint32_t s_flag_jp[N];
std::uint32_t s_flag_kr[N];
std::uint32_t s_flag_th[N];
std::uint32_t s_flag_vn[N];
std::uint32_t s_flag_ph[N];
std::uint32_t s_flag_id[N];
std::uint32_t s_flag_sg[N];
std::uint32_t s_flag_cn[N];

bool s_initialized = false;

void init_us(std::uint32_t* p) {
    // 13 stripes won't fit at 16 rows cleanly — approximate with 7 alternating
    // bands of red/white (taller, more readable at small sizes), plus a blue
    // canton with one large "star spot" cluster.
    // Stripe heights ~ 2 px each = 14 rows + 2 padding.
    fill_all(p, kWhite);
    for (int s = 0; s < 7; ++s) {
        if (s % 2 == 0) {
            fill_rect(p, 0, s * 2, W, 2, kRed);
        }
    }
    // tail row (row 14-15) — red band as well so we end on red
    fill_rect(p, 0, 14, W, 2, kRed);
    // Blue canton — top-left ~ 40% wide, ~50% tall (proportions match real).
    fill_rect(p, 0, 0, 10, 8, kBlue);
    // A few white "star" pixels scattered in the canton.
    for (int yy = 1; yy < 8; yy += 2) {
        for (int xx = 1; xx < 10; xx += 2) {
            set_px(p, xx, yy, kWhite);
        }
    }
}

void init_ca(std::uint32_t* p) {
    // Canada: red bars left+right (~6 px each), white center (~12 px),
    // red maple-leaf approximation in the center (small red blob).
    fill_all(p, kWhite);
    fill_rect(p, 0,  0, 6,  H, kRed);
    fill_rect(p, 18, 0, 6,  H, kRed);
    // Approximate maple leaf: a diamond + 2 small lobes on the sides + stem.
    draw_diamond(p, 12, 8, 4, kRed);
    set_px(p, 12, 13, kRed); set_px(p, 12, 14, kRed);  // stem
    set_px(p, 8, 8, kRed);  set_px(p, 16, 8, kRed);    // side lobes
    set_px(p, 9, 6, kRed);  set_px(p, 15, 6, kRed);    // upper lobes
}

void init_br(std::uint32_t* p) {
    // Brazil: green field, yellow diamond, blue disc in center.
    fill_all(p, kBR_Green);
    // Yellow diamond — use the helper at a larger radius.
    draw_diamond(p, 12, 8, 7, kBR_Yellow);
    // Blue disc inside the diamond.
    draw_disc(p, 12, 8, 3, kBR_Blue);
}

void init_mx(std::uint32_t* p) {
    // Mexico: vertical tricolor green/white/red, plus a small dark mark in
    // the center white panel for the eagle/coat-of-arms.
    vertical_tri(p, kMX_Green, kWhite, kMX_Red);
    // Suggest an emblem with a few dark pixels in the center.
    draw_disc(p, 12, 8, 2, kGreen);
    set_px(p, 12, 6, kRed); set_px(p, 12, 10, kRed);
}

void init_cl(std::uint32_t* p) {
    // Chile: top-half split white(right) blue-canton(top-left), bottom-half red.
    fill_rect(p, 0, 0, W, 8, kWhite);
    fill_rect(p, 0, 8, W, 8, kCL_Red);
    fill_rect(p, 0, 0, 8, 8, kCL_Blue);
    // White star in canton.
    set_px(p, 4, 4, kWhite);
    set_px(p, 3, 4, kWhite); set_px(p, 5, 4, kWhite);
    set_px(p, 4, 3, kWhite); set_px(p, 4, 5, kWhite);
}

void init_ar(std::uint32_t* p) {
    // Argentina: 3 horizontal stripes — sky-blue / white / sky-blue, with
    // a small yellow "sun of May" disc in the middle.
    fill_rect(p, 0, 0,  W, 5, kAR_SkyBlue);
    fill_rect(p, 0, 5,  W, 6, kWhite);
    fill_rect(p, 0, 11, W, 5, kAR_SkyBlue);
    draw_disc(p, 12, 8, 2, kYellow);
}

void init_de(std::uint32_t* p) {
    // Germany: horizontal tricolor black/red/gold.
    horizontal_tri(p, kBlack, kDE_Red, kDE_Gold);
}

void init_se(std::uint32_t* p) {
    nordic_cross(p, kSE_Blue, kSE_Yellow);
}

void init_gb(std::uint32_t* p) {
    // Union Jack approximation — blue field with a simplified red+white
    // cross of St George overlaid on white diagonals of St Andrew.
    fill_all(p, kBlue);
    // White diagonals (rough lines).
    for (int i = 0; i < W && i < H * (W / (float)H); ++i) {
        int yy = (i * H) / W;
        set_px(p, i,   yy, kWhite);
        set_px(p, i, yy + 1, kWhite);
        set_px(p, W - 1 - i,   yy, kWhite);
        set_px(p, W - 1 - i, yy + 1, kWhite);
    }
    // Red diagonals on top of the white (thinner, off-center per real flag).
    for (int i = 1; i < W - 1; ++i) {
        int yy = (i * H) / W;
        set_px(p, i, yy, kRed);
        set_px(p, W - 1 - i, yy, kRed);
    }
    // White cross (St George) — 4 px thick.
    fill_rect(p, 0, 6, W, 4, kWhite);
    fill_rect(p, 10, 0, 4, H, kWhite);
    // Red cross inside the white.
    fill_rect(p, 0, 7, W, 2, kRed);
    fill_rect(p, 11, 0, 2, H, kRed);
}

void init_ua(std::uint32_t* p) {
    // Ukraine: horizontal bicolor blue (top) / yellow (bottom).
    fill_rect(p, 0, 0, W, 8, kUA_Blue);
    fill_rect(p, 0, 8, W, 8, kUA_Yellow);
}

void init_fr(std::uint32_t* p) {
    vertical_tri(p, kFR_Blue, kWhite, kFR_Red);
}

void init_tr(std::uint32_t* p) {
    // Turkey: red field with a white crescent + star (approximated).
    fill_all(p, kTR_Red);
    // Crescent: white disc with a slightly-offset red disc carved into it.
    draw_disc(p, 9, 8, 4, kWhite);
    draw_disc(p, 10, 8, 3, kTR_Red);
    // Star: a small white pixel cluster to the right.
    set_px(p, 15, 8, kWhite);
    set_px(p, 14, 7, kWhite); set_px(p, 16, 7, kWhite);
    set_px(p, 14, 9, kWhite); set_px(p, 16, 9, kWhite);
    set_px(p, 15, 7, kWhite); set_px(p, 15, 9, kWhite);
}

void init_fi(std::uint32_t* p) {
    nordic_cross(p, kWhite, kFI_Blue);
}

void init_sct(std::uint32_t* p) {
    // Scotland: St Andrew's saltire — blue field, white diagonals.
    fill_all(p, kSCT_Blue);
    // Two diagonals (thick).
    for (int i = 0; i < W; ++i) {
        int yy = (i * H) / W;
        set_px(p, i, yy, kWhite);
        set_px(p, i, yy + 1, kWhite);
        if (yy > 0) set_px(p, i, yy - 1, kWhite);
        set_px(p, W - 1 - i, yy, kWhite);
        set_px(p, W - 1 - i, yy + 1, kWhite);
        if (yy > 0) set_px(p, W - 1 - i, yy - 1, kWhite);
    }
}

void init_ie(std::uint32_t* p) {
    vertical_tri(p, kIE_Green, kWhite, kIE_Orange);
}

void init_es(std::uint32_t* p) {
    // Spain: red / yellow (double-height) / red horizontal bands with a
    // small dark dot for the coat-of-arms placement on the left.
    fill_rect(p, 0,  0,  W, 4,  kES_Red);
    fill_rect(p, 0,  4,  W, 8,  kES_Yellow);
    fill_rect(p, 0,  12, W, 4,  kES_Red);
    // Coat of arms approximation — a small red pixel cluster on the left third.
    set_px(p, 8, 8, kES_Red);
    set_px(p, 7, 7, kES_Red); set_px(p, 9, 7, kES_Red);
    set_px(p, 7, 9, kES_Red); set_px(p, 9, 9, kES_Red);
}

void init_ru(std::uint32_t* p) {
    horizontal_tri(p, kWhite, kRU_Blue, kRU_Red);
}

void init_pl(std::uint32_t* p) {
    // Poland: horizontal bicolor white (top) / crimson red (bottom).
    fill_rect(p, 0, 0, W, 8, kWhite);
    fill_rect(p, 0, 8, W, 8, kPL_Red);
}

void init_nl(std::uint32_t* p) {
    horizontal_tri(p, kNL_Red, kWhite, kNL_Blue);
}

void init_it(std::uint32_t* p) {
    vertical_tri(p, kIT_Green, kWhite, kIT_Red);
}

void init_dk(std::uint32_t* p) {
    nordic_cross(p, kDK_Red, kWhite);
}

void init_no(std::uint32_t* p) {
    // Norway: red field, white Nordic cross, with a blue cross inside the
    // white. We approximate by drawing white-cross then a thinner blue cross.
    fill_all(p, kNO_Red);
    fill_rect(p, 0, 6, W, 4, kWhite);      // horizontal white bar
    fill_rect(p, 7, 0, 4, H, kWhite);      // vertical white bar at col 7-10
    fill_rect(p, 0, 7, W, 2, kNO_Blue);    // blue horizontal inside white
    fill_rect(p, 8, 0, 2, H, kNO_Blue);    // blue vertical inside white
}

void init_au(std::uint32_t* p) {
    // Australia: blue field, Union Jack canton in top-left (highly
    // simplified), and a few white "stars" on the right (Commonwealth +
    // Southern Cross approximation).
    fill_all(p, kAU_Blue);
    // Simplified Union canton in top-left ~ 12x8.
    fill_rect(p, 0, 0, 12, 8, kAU_Blue);
    fill_rect(p, 0, 3, 12, 2, kWhite);   // horizontal cross
    fill_rect(p, 5, 0, 2, 8, kWhite);    // vertical cross
    fill_rect(p, 0, 3, 12, 2, kAU_Red);  // red inside the white horizontal
    fill_rect(p, 5, 0, 2, 8, kAU_Red);   // red inside the white vertical
    // Commonwealth star below the canton.
    set_px(p, 6, 11, kWhite);
    set_px(p, 5, 11, kWhite); set_px(p, 7, 11, kWhite);
    set_px(p, 6, 10, kWhite); set_px(p, 6, 12, kWhite);
    // Southern Cross — 4 small white dots on right half.
    set_px(p, 17, 4,  kWhite);
    set_px(p, 21, 6,  kWhite);
    set_px(p, 19, 9,  kWhite);
    set_px(p, 16, 11, kWhite);
    set_px(p, 21, 12, kWhite);
}

void init_jp(std::uint32_t* p) {
    // Japan: white field, red disc in center.
    fill_all(p, kWhite);
    draw_disc(p, 12, 8, 5, kJP_Red);
}

void init_kr(std::uint32_t* p) {
    // Korea: white field + blue/red yin-yang taegeuk approximated as
    // two stacked half-discs of opposite color in the center.
    fill_all(p, kWhite);
    // Top-half: red semicircle. Bottom-half: blue semicircle.
    int cx = 12, cy = 8, r = 5;
    int r2 = r * r;
    for (int yy = -r; yy <= r; ++yy) {
        for (int xx = -r; xx <= r; ++xx) {
            if (xx * xx + yy * yy <= r2) {
                set_px(p, cx + xx, cy + yy, (yy < 0) ? kKR_Red : kKR_Blue);
            }
        }
    }
    // Black trigram marker dots (corners) — heavily abstracted.
    set_px(p, 3, 3,  kBlack); set_px(p, 4, 3,  kBlack);
    set_px(p, 19, 3, kBlack); set_px(p, 20, 3, kBlack);
    set_px(p, 3, 12, kBlack); set_px(p, 4, 12, kBlack);
    set_px(p, 19, 12,kBlack); set_px(p, 20, 12,kBlack);
}

void init_th(std::uint32_t* p) {
    // Thailand: 5 horizontal stripes — red / white / blue (double) / white / red.
    fill_rect(p, 0, 0,  W, 3, kTH_Red);
    fill_rect(p, 0, 3,  W, 2, kWhite);
    fill_rect(p, 0, 5,  W, 6, kTH_Blue);
    fill_rect(p, 0, 11, W, 2, kWhite);
    fill_rect(p, 0, 13, W, 3, kTH_Red);
}

void init_vn(std::uint32_t* p) {
    // Vietnam: red field, yellow 5-point star in the middle.
    fill_all(p, kVN_Red);
    // Star approximation — diamond + 4 horizontal arms.
    draw_diamond(p, 12, 8, 3, kYellow);
    set_px(p, 9, 8,  kYellow); set_px(p, 15, 8,  kYellow);
}

void init_ph(std::uint32_t* p) {
    // Philippines: triangle (white) on the hoist side with sun motif, blue
    // top half and red bottom half on the fly side.
    fill_rect(p, 0, 0, W, 8, kPH_Blue);
    fill_rect(p, 0, 8, W, 8, kPH_Red);
    // White hoist triangle.
    for (int yy = 0; yy < H; ++yy) {
        int span = (yy < H / 2) ? (yy + 1) : (H - yy);
        // Spread it wider — multiply span.
        int w = std::min(W / 2, span + 2);
        fill_rect(p, 0, yy, w, 1, kWhite);
    }
    // Yellow sun in the triangle.
    set_px(p, 4, 8, kPH_Yellow);
    set_px(p, 3, 8, kPH_Yellow); set_px(p, 5, 8, kPH_Yellow);
    set_px(p, 4, 7, kPH_Yellow); set_px(p, 4, 9, kPH_Yellow);
}

void init_id(std::uint32_t* p) {
    // Indonesia: horizontal bicolor red (top) / white (bottom).
    fill_rect(p, 0, 0, W, 8, kID_Red);
    fill_rect(p, 0, 8, W, 8, kWhite);
}

void init_sg(std::uint32_t* p) {
    // Singapore: red (top) over white (bottom) with crescent + 5 stars
    // approximated as a white crescent in the top-left.
    fill_rect(p, 0, 0, W, 8, kSG_Red);
    fill_rect(p, 0, 8, W, 8, kWhite);
    // Crescent approximation.
    draw_disc(p, 6, 4, 3, kWhite);
    draw_disc(p, 7, 4, 2, kSG_Red);
    // Star dots.
    set_px(p, 11, 3, kWhite); set_px(p, 13, 5, kWhite); set_px(p, 15, 3, kWhite);
}

void init_cn(std::uint32_t* p) {
    // China: red field, large yellow star top-left, 4 smaller yellow stars
    // arcing to its right.
    fill_all(p, kCN_Red);
    // Large star (approximated as small diamond cluster).
    draw_diamond(p, 5, 4, 2, kCN_Yellow);
    // Four small stars to the right.
    set_px(p, 10, 2, kCN_Yellow);
    set_px(p, 12, 4, kCN_Yellow);
    set_px(p, 12, 7, kCN_Yellow);
    set_px(p, 10, 9, kCN_Yellow);
}

// Lazy-init all flag buffers on first call. Cheap (~12k pixel writes) and
// happens once per program lifetime.
void init_all_flags() {
    if (s_initialized) return;
    init_us(s_flag_us);
    init_ca(s_flag_ca);
    init_br(s_flag_br);
    init_mx(s_flag_mx);
    init_cl(s_flag_cl);
    init_ar(s_flag_ar);
    init_de(s_flag_de);
    init_se(s_flag_se);
    init_gb(s_flag_gb);
    init_ua(s_flag_ua);
    init_fr(s_flag_fr);
    init_tr(s_flag_tr);
    init_fi(s_flag_fi);
    init_sct(s_flag_sct);
    init_ie(s_flag_ie);
    init_es(s_flag_es);
    init_ru(s_flag_ru);
    init_pl(s_flag_pl);
    init_nl(s_flag_nl);
    init_it(s_flag_it);
    init_dk(s_flag_dk);
    init_no(s_flag_no);
    init_au(s_flag_au);
    init_jp(s_flag_jp);
    init_kr(s_flag_kr);
    init_th(s_flag_th);
    init_vn(s_flag_vn);
    init_ph(s_flag_ph);
    init_id(s_flag_id);
    init_sg(s_flag_sg);
    init_cn(s_flag_cn);
    s_initialized = true;
}

}  // namespace

FlagBitmap get_flag_bitmap(std::string_view iso) {
    init_all_flags();
    // Linear scan over 31 entries — trivially fast vs the cost of building
    // a hash map for this tiny set. Keep ordered roughly by region for
    // readability.
    if (iso == "us")  return {W, H, s_flag_us};
    if (iso == "ca")  return {W, H, s_flag_ca};
    if (iso == "br")  return {W, H, s_flag_br};
    if (iso == "mx")  return {W, H, s_flag_mx};
    if (iso == "cl")  return {W, H, s_flag_cl};
    if (iso == "ar")  return {W, H, s_flag_ar};
    if (iso == "de")  return {W, H, s_flag_de};
    if (iso == "se")  return {W, H, s_flag_se};
    if (iso == "gb")  return {W, H, s_flag_gb};
    if (iso == "ua")  return {W, H, s_flag_ua};
    if (iso == "fr")  return {W, H, s_flag_fr};
    if (iso == "tr")  return {W, H, s_flag_tr};
    if (iso == "fi")  return {W, H, s_flag_fi};
    if (iso == "sct") return {W, H, s_flag_sct};
    if (iso == "ie")  return {W, H, s_flag_ie};
    if (iso == "es")  return {W, H, s_flag_es};
    if (iso == "ru")  return {W, H, s_flag_ru};
    if (iso == "pl")  return {W, H, s_flag_pl};
    if (iso == "nl")  return {W, H, s_flag_nl};
    if (iso == "it")  return {W, H, s_flag_it};
    if (iso == "dk")  return {W, H, s_flag_dk};
    if (iso == "no")  return {W, H, s_flag_no};
    if (iso == "au")  return {W, H, s_flag_au};
    if (iso == "jp")  return {W, H, s_flag_jp};
    if (iso == "kr")  return {W, H, s_flag_kr};
    if (iso == "th")  return {W, H, s_flag_th};
    if (iso == "vn")  return {W, H, s_flag_vn};
    if (iso == "ph")  return {W, H, s_flag_ph};
    if (iso == "id")  return {W, H, s_flag_id};
    if (iso == "sg")  return {W, H, s_flag_sg};
    if (iso == "cn")  return {W, H, s_flag_cn};
    return {0, 0, nullptr};
}

}  // namespace vlr
