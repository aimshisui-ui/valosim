// VLR Manager — ImGui + DX11 + Win32 frontend.
//
// One TU for everything UI so the engine TUs stay headless.
// Sections (jump):
//   1. D3D11 / Win32 plumbing
//   2. Theme, fonts, helpers
//   3. App state
//   4. Live Match playback engine state
//   5. Modal: Player Profile (with God Mode editor)
//   6. Modal: Coach Editor
//   7. Modal: Match History detail
//   8. Modal: Live Match Viewer
//   9. Screens (Dashboard, Roster, Standings, Brackets, Market, SoloQ, Coach,
//      HoF, EventLog, Watch)
//  10. Sidebar + entrypoint

#include "Coach.h"
#include "Country.h"
#include "FlagBitmaps.h"
#include "GameManager.h"
#include "Goat.h"
#include "LogoArt.h"
#include "Match.h"
#include "MatchExport.h"
#include "Names.h"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <tchar.h>
#include <windows.h>
#include <commdlg.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

// ============================================================
// 1. D3D11 / Win32 plumbing
// ============================================================
namespace {

ID3D11Device*           g_pd3dDevice           = nullptr;
ID3D11DeviceContext*    g_pd3dDeviceContext    = nullptr;
IDXGISwapChain*         g_pSwapChain           = nullptr;
ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
UINT                    g_ResizeWidth = 0, g_ResizeHeight = 0;

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (!pBackBuffer) return;
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}
void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}
bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags          = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage    = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow   = hWnd;
    sd.SampleDesc.Count   = 1;
    sd.Windowed         = TRUE;
    sd.SwapEffect       = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL fls[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
        fls, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
        &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED) {
        res = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags,
            fls, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
            &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    }
    if (res != S_OK) return false;
    CreateRenderTarget();
    return true;
}
void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain)        { g_pSwapChain->Release();        g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice)        { g_pd3dDevice->Release();        g_pd3dDevice = nullptr; }
}
}  // namespace

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) return 0;
            g_ResizeWidth  = (UINT)LOWORD(lParam);
            g_ResizeHeight = (UINT)HIWORD(lParam);
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY: ::PostQuitMessage(0); return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ============================================================
// 2. Theme, fonts, helpers
// ============================================================
namespace {

// === Premium 2026 dark esports palette ==================================
// Redesigned 2026-05-15. Deep charcoal base, electric-blue primary accent,
// purple secondary, gold for prestige. Existing constant NAMES are kept so
// the whole UI inherits the new look with zero per-screen churn; values are
// retuned. New accent/surface tokens added below for the card design system.
constexpr ImU32 kVlrRed    = IM_COL32(0xFF, 0x46, 0x55, 0xFF); // VLR brand red (kept)
constexpr ImU32 kVlrRedDim = IM_COL32(0xC4, 0x39, 0x45, 0xFF);
constexpr ImU32 kVlrNavy   = IM_COL32(0x0F, 0x11, 0x17, 0xFF); // deep charcoal base bg
constexpr ImU32 kVlrPanel  = IM_COL32(0x17, 0x1A, 0x24, 0xFF); // surface
constexpr ImU32 kVlrPanel2 = IM_COL32(0x22, 0x27, 0x35, 0xFF); // raised surface
constexpr ImU32 kVlrText   = IM_COL32(0xE9, 0xEC, 0xF1, 0xFF); // near-white
constexpr ImU32 kVlrSub    = IM_COL32(0x8B, 0x93, 0xA3, 0xFF); // muted secondary text
constexpr ImU32 kVlrGold   = IM_COL32(0xFF, 0xD7, 0x00, 0xFF); // prestige gold
constexpr ImU32 kVlrSilver = IM_COL32(0xC6, 0xCD, 0xD8, 0xFF);
constexpr ImU32 kVlrBronze = IM_COL32(0xCD, 0x7F, 0x32, 0xFF);
constexpr ImU32 kVlrGreen  = IM_COL32(0x3F, 0xD1, 0x7A, 0xFF); // success green
constexpr ImU32 kVlrBlue   = IM_COL32(0x00, 0xD4, 0xFF, 0xFF); // electric-blue accent

// New design-system tokens (use these in new card-based layouts).
// 2026-06-11 premium redesign: primary accent pulled from electric cyan to
// a refined warm gold-amber so the UI loses the "stock AI tech" fingerprint
// and reads as a broadcast graphics package. Cyan is retained as
// `kInfo` for explicit informational chips / data-line plotting where the
// gold would clash with the prestige color. `kVlrGold` (#FFD700, brighter)
// stays the prestige / trophy color — the contrast between bright trophy
// gold and the muted interaction amber gives clear hierarchy.
constexpr ImU32 kAccent       = IM_COL32(0xD4, 0xA1, 0x4C, 0xFF); // warm amber — primary
constexpr ImU32 kAccentDim    = IM_COL32(0x9D, 0x76, 0x35, 0xFF); // darker amber (press / focus-ring depth)
constexpr ImU32 kAccentSoft   = IM_COL32(0xD4, 0xA1, 0x4C, 0x22); // 13% amber for hover overlays / inner glow
constexpr ImU32 kAccent2      = IM_COL32(0x7B, 0x5E, 0xFF, 0xFF); // purple — secondary (archetype chips / plot histo)
constexpr ImU32 kAccent2Dim   = IM_COL32(0x5C, 0x47, 0xC0, 0xFF);
constexpr ImU32 kInfo         = IM_COL32(0x5B, 0xA8, 0xB0, 0xFF); // muted teal — info / data-line chips
constexpr ImU32 kInfoDim      = IM_COL32(0x3F, 0x78, 0x7F, 0xFF);
constexpr ImU32 kBgDeep       = IM_COL32(0x0B, 0x0D, 0x12, 0xFF); // deepest (gutters)
constexpr ImU32 kBgBase       = IM_COL32(0x0F, 0x11, 0x17, 0xFF); // window bg
constexpr ImU32 kSurface      = IM_COL32(0x17, 0x1A, 0x24, 0xFF); // card bg
constexpr ImU32 kSurfaceAlt   = IM_COL32(0x1C, 0x20, 0x2C, 0xFF); // alt row / hover
constexpr ImU32 kSurfaceHi    = IM_COL32(0x24, 0x29, 0x38, 0xFF); // raised / selected
constexpr ImU32 kBorder       = IM_COL32(0x2A, 0x2F, 0x3D, 0xFF); // hairline border
constexpr ImU32 kBorderStrong = IM_COL32(0x3A, 0x41, 0x52, 0xFF);
constexpr ImU32 kTextFaint    = IM_COL32(0x5E, 0x66, 0x74, 0xFF); // labels / captions
// Tinted drop-shadow color — surface-hued black so shadows match the bg
// instead of reading as a generic black halo. Used by BeginCard and the
// LogoArt backplate stack.
constexpr ImU32 kShadow       = IM_COL32(0x05, 0x07, 0x0B, 0x90);
constexpr ImU32 kShadowSoft   = IM_COL32(0x05, 0x07, 0x0B, 0x50);

// === Role / Position palette (muted, esport-clean) =======================
// These are the SOLE source of role colour. Earlier rounds used the bright
// kVlrRed / kVlrGold / saturated purple-cyan for role tinting which read as
// neon against the dark-mode background. The muted palette below shifts
// each role toward an earth tone while keeping them distinguishable.
// Use these constants in place of kVlrRed/kVlrGold/etc. for any colour
// that semantically represents a player's role or IGL status. Non-role
// uses of kVlrGold (signature agent strip, gold accents) stay as-is.
constexpr ImU32 kRoleDuelist    = IM_COL32(0xC4, 0x5A, 0x5A, 0xFF); // muted brick red
constexpr ImU32 kRoleController = IM_COL32(0x8A, 0x6D, 0xB8, 0xFF); // muted indigo
constexpr ImU32 kRoleSentinel   = IM_COL32(0x5C, 0x9D, 0x76, 0xFF); // muted forest green
constexpr ImU32 kRoleInitiator  = IM_COL32(0x6E, 0x97, 0xB8, 0xFF); // muted steel blue
constexpr ImU32 kRoleFlex       = IM_COL32(0xC4, 0x9A, 0x52, 0xFF); // muted amber
constexpr ImU32 kIglBadge       = IM_COL32(0xE5, 0xC5, 0x5C, 0xFF); // muted gold (distinct from Flex amber)

ImVec4 toV4(ImU32 c) {
    return ImVec4(((c >>  0) & 0xff) / 255.0f,
                  ((c >>  8) & 0xff) / 255.0f,
                  ((c >> 16) & 0xff) / 255.0f,
                  ((c >> 24) & 0xff) / 255.0f);
}

// Font globals — populated in main() after ImGui context is created. Each
// call site picks the font that matches what it's drawing instead of using
// SetWindowFontScale (which produces blurry / inconsistent text).
ImFont* g_font_body    = nullptr;  // 16 Segoe UI
ImFont* g_font_small   = nullptr;  // 13 Segoe UI
ImFont* g_font_h2      = nullptr;  // 22 Segoe UI Semibold
ImFont* g_font_h1      = nullptr;  // 30 Segoe UI Black
ImFont* g_font_kpi     = nullptr;  // 38 Segoe UI Black (big numbers on cards)
ImFont* g_font_mono    = nullptr;  // 15 Consolas (scoreboards)

void LoadFonts(ImGuiIO& io) {
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    cfg.RasterizerMultiply = 1.05f;
    cfg.PixelSnapH = false;

    // Common Latin range; cheap to bake.
    static const ImWchar ranges[] = { 0x0020, 0x00FF, 0x2014, 0x2014, 0x2022, 0x2022, 0 };

    auto add = [&](const char* path, float px, ImFontConfig& c) -> ImFont* {
        ImFont* f = io.Fonts->AddFontFromFileTTF(path, px, &c, ranges);
        return f;
    };

    // Bake all the sizes we need from local Windows fonts.
    g_font_body  = add("C:\\Windows\\Fonts\\segoeui.ttf",  16.0f, cfg);
    g_font_small = add("C:\\Windows\\Fonts\\segoeui.ttf",  13.0f, cfg);
    g_font_h2    = add("C:\\Windows\\Fonts\\seguisb.ttf",  22.0f, cfg);
    g_font_h1    = add("C:\\Windows\\Fonts\\seguibl.ttf",  30.0f, cfg);
    g_font_kpi   = add("C:\\Windows\\Fonts\\seguibl.ttf",  38.0f, cfg);
    g_font_mono  = add("C:\\Windows\\Fonts\\consola.ttf",  15.0f, cfg);

    // If any failed to load, fall back to default.
    if (!g_font_body) g_font_body = io.FontDefault ? io.FontDefault : io.Fonts->Fonts[0];
    if (!g_font_small) g_font_small = g_font_body;
    if (!g_font_h2) g_font_h2 = g_font_body;
    if (!g_font_h1) g_font_h1 = g_font_h2;
    if (!g_font_kpi) g_font_kpi = g_font_h1;
    if (!g_font_mono) g_font_mono = g_font_body;

    io.FontDefault = g_font_body;
}

void ApplyVlrTheme(ImGuiStyle& s) {
    // Premium 2026 dark esports look: generous rounding, more breathing
    // room, electric-blue interaction accents, hairline borders.
    // Varied radius scale — uniform rounding reads as a generic AI default.
    // Outer surfaces are softest, instrumented frames are tightest, so the
    // eye reads a clear hierarchy from chrome -> panel -> control.
    s.WindowRounding    = 14.0f; // softest outer chrome — broadcast-card feel
    s.ChildRounding     = 10.0f; // panel / card containers — mid-tier softness
    s.FrameRounding     = 6.0f;  // inputs, sliders, combos — tight "instrumented" feel
    s.TabRounding       = 8.0f;  // tabs sit between frame and child
    s.ScrollbarRounding = 12.0f; // matches the rounded outer chrome
    s.GrabRounding      = 6.0f;  // matches frame so grabs fit their tracks
    s.PopupRounding     = 12.0f; // popups echo the outer scrollbar/window radius
    s.TabBarBorderSize  = 0.0f;
    s.WindowPadding     = ImVec2(24, 22); // extra breathing room around content
    s.FramePadding      = ImVec2(13, 8);
    s.ItemSpacing       = ImVec2(12, 10); // slightly looser vertical rhythm
    s.ItemInnerSpacing  = ImVec2(8, 6);
    s.IndentSpacing     = 22.0f;
    s.ScrollbarSize     = 13.0f;
    s.GrabMinSize       = 12.0f;
    s.WindowBorderSize  = 0.0f;
    s.FrameBorderSize   = 0.0f;
    s.ChildBorderSize   = 1.0f;
    s.PopupBorderSize   = 1.0f;
    s.CellPadding       = ImVec2(12, 7);
    s.WindowTitleAlign  = ImVec2(0.0f, 0.5f);
    s.SeparatorTextBorderSize = 2.0f;
    s.SeparatorTextPadding    = ImVec2(20, 6);

    auto& c = s.Colors;
    c[ImGuiCol_Text]              = toV4(kVlrText);
    c[ImGuiCol_TextDisabled]      = toV4(kTextFaint);
    c[ImGuiCol_WindowBg]          = toV4(kBgBase);
    c[ImGuiCol_ChildBg]           = toV4(IM_COL32(0x00, 0x00, 0x00, 0x00));
    c[ImGuiCol_PopupBg]           = toV4(IM_COL32(0x14, 0x17, 0x20, 0xFC));
    c[ImGuiCol_Border]            = toV4(kBorder);
    c[ImGuiCol_BorderShadow]      = toV4(IM_COL32(0x00, 0x00, 0x00, 0x00));
    c[ImGuiCol_FrameBg]           = toV4(IM_COL32(0x1F, 0x23, 0x30, 0xFF));
    c[ImGuiCol_FrameBgHovered]    = toV4(kSurfaceHi);
    c[ImGuiCol_FrameBgActive]     = toV4(IM_COL32(0x2C, 0x33, 0x44, 0xFF));
    c[ImGuiCol_TitleBg]           = toV4(kBgDeep);
    c[ImGuiCol_TitleBgActive]     = toV4(kSurface);
    c[ImGuiCol_MenuBarBg]         = toV4(kBgDeep);
    c[ImGuiCol_ScrollbarBg]       = toV4(IM_COL32(0x00, 0x00, 0x00, 0x00));
    c[ImGuiCol_ScrollbarGrab]     = toV4(IM_COL32(0x2C, 0x32, 0x42, 0xFF));
    c[ImGuiCol_ScrollbarGrabHovered] = toV4(IM_COL32(0x3A, 0x42, 0x56, 0xFF));
    c[ImGuiCol_ScrollbarGrabActive]  = toV4(kAccentDim);
    c[ImGuiCol_CheckMark]         = toV4(kAccent);
    c[ImGuiCol_SliderGrab]        = toV4(kAccent);
    c[ImGuiCol_SliderGrabActive]  = toV4(kAccentDim);
    c[ImGuiCol_Button]            = toV4(IM_COL32(0x22, 0x27, 0x35, 0xFF));
    c[ImGuiCol_ButtonHovered]     = toV4(IM_COL32(0x2E, 0x35, 0x47, 0xFF));
    c[ImGuiCol_ButtonActive]      = toV4(kAccentDim);
    c[ImGuiCol_Header]            = toV4(IM_COL32(0x22, 0x27, 0x35, 0xFF));
    c[ImGuiCol_HeaderHovered]     = toV4(kSurfaceHi);
    c[ImGuiCol_HeaderActive]      = toV4(IM_COL32(0x2C, 0x33, 0x44, 0xFF));
    c[ImGuiCol_Separator]         = toV4(kBorder);
    c[ImGuiCol_SeparatorHovered]  = toV4(kAccent);
    c[ImGuiCol_SeparatorActive]   = toV4(kAccent);
    c[ImGuiCol_ResizeGrip]        = toV4(IM_COL32(0x2C, 0x33, 0x44, 0x60));
    c[ImGuiCol_ResizeGripHovered] = toV4(kAccentDim);
    c[ImGuiCol_ResizeGripActive]  = toV4(kAccent);
    c[ImGuiCol_Tab]               = toV4(IM_COL32(0x00, 0x00, 0x00, 0x00));
    c[ImGuiCol_TabHovered]        = toV4(kSurfaceHi);
    c[ImGuiCol_TabActive]         = toV4(kSurface);
    c[ImGuiCol_TabUnfocused]      = toV4(IM_COL32(0x00, 0x00, 0x00, 0x00));
    c[ImGuiCol_TabUnfocusedActive]= toV4(kSurface);
    c[ImGuiCol_PlotLines]         = toV4(kInfo); // muted teal — gold is reserved for user-agency, not data
    c[ImGuiCol_PlotLinesHovered]  = toV4(kVlrGold);
    c[ImGuiCol_PlotHistogram]     = toV4(kAccent2);
    c[ImGuiCol_PlotHistogramHovered] = toV4(kVlrGold);
    c[ImGuiCol_TableHeaderBg]     = toV4(IM_COL32(0x16, 0x19, 0x22, 0xFF));
    c[ImGuiCol_TableBorderStrong] = toV4(kBorder);
    c[ImGuiCol_TableBorderLight]  = toV4(IM_COL32(0x1B, 0x1F, 0x29, 0xFF));
    c[ImGuiCol_TableRowBg]        = toV4(IM_COL32(0x00, 0x00, 0x00, 0x00));
    c[ImGuiCol_TableRowBgAlt]     = toV4(IM_COL32(0xFF, 0xFF, 0xFF, 0x06));
    c[ImGuiCol_TextSelectedBg]    = toV4(IM_COL32(0xD4, 0xA1, 0x4C, 0x40)); // 25% warm amber — matches new kAccent
    c[ImGuiCol_NavHighlight]      = toV4(kAccent);
    c[ImGuiCol_DragDropTarget]    = toV4(kVlrGold);
}

// === Small UI helpers ===

ImU32 placement_color(int rank) {
    if (rank == 1) return kVlrGold;
    if (rank == 2) return kVlrSilver;
    if (rank == 3) return kVlrBronze;
    return kVlrText;
}

const char* role_short(vlr::Role r) {
    switch (r) {
        case vlr::Role::Duelist:    return "DUE";
        case vlr::Role::Initiator:  return "INI";
        case vlr::Role::Controller: return "CTR";
        case vlr::Role::Sentinel:   return "SEN";
        default:                    return "?";
    }
}
ImU32 role_color(vlr::Role r) {
    switch (r) {
        case vlr::Role::Duelist:    return kRoleDuelist;
        case vlr::Role::Initiator:  return kRoleInitiator;
        case vlr::Role::Controller: return kRoleController;
        case vlr::Role::Sentinel:   return kRoleSentinel;
        default:                    return kVlrText;
    }
}
void RoleBadge(vlr::Role r) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = 44.0f;
    float h = ImGui::GetFontSize() + 6.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), role_color(r), 4.0f);
    dl->AddText(ImVec2(pos.x + 8, pos.y + 3), kVlrText, role_short(r));
    ImGui::Dummy(ImVec2(w + 6, h));
}

// === Position helpers (4-slot canonical roster position) ================
// Position represents the player's CANONICAL gameplay slot derived purely
// from primary_role. The four positions are:
//   Duelist / Controller / Sentinel / Initiator.
// Flex is NOT a position — it's an orthogonal SUB-ROLE overlay flag
// (`Player::is_flex`) rendered as a separate amber chip via FlexBadge().
// IGL is likewise NOT a position — it's an orthogonal LEADERSHIP overlay
// flag (`Player::is_igl`) rendered as a separate muted-gold chip via
// IglBadge(). All three are independent: a player can be e.g.
// "Initiator • Flex • IGL".
const char* position_short(vlr::Position p) {
    switch (p) {
        case vlr::Position::Duelist:    return "DUE";
        case vlr::Position::Controller: return "CTR";
        case vlr::Position::Sentinel:   return "SEN";
        case vlr::Position::Initiator:  return "INI";
        default:                        return "?";
    }
}
ImU32 position_color(vlr::Position p) {
    switch (p) {
        case vlr::Position::Duelist:    return kRoleDuelist;
        case vlr::Position::Controller: return kRoleController;
        case vlr::Position::Sentinel:   return kRoleSentinel;
        case vlr::Position::Initiator:  return kRoleInitiator;
        default:                        return kVlrText;
    }
}
void PositionBadge(vlr::Position pos) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = 44.0f;
    float h = ImGui::GetFontSize() + 6.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), position_color(pos), 4.0f);
    dl->AddText(ImVec2(p.x + 8, p.y + 3), kVlrText, position_short(pos));
    ImGui::Dummy(ImVec2(w + 6, h));
}
// Secondary IGL pill — small muted-gold tag rendered AFTER the position
// pill (via ImGui::SameLine()) whenever a player is the team's IGL. The
// IGL flag is independent of position; e.g. "Controller • IGL",
// "Initiator • IGL". Text is rendered in near-black for contrast against
// the gold background.
void IglBadge() {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = 32.0f;
    float h = ImGui::GetFontSize() + 6.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), kIglBadge, 4.0f);
    dl->AddText(ImVec2(p.x + 6, p.y + 3),
                IM_COL32(0x14, 0x14, 0x14, 0xFF), "IGL");
    ImGui::Dummy(ImVec2(w + 6, h));
}
// Secondary Flex pill — small muted-amber tag rendered AFTER the position
// pill (and after IglBadge() if both apply) via ImGui::SameLine() whenever
// `Player::is_flex` is true. Flex is independent of position AND of IGL;
// a "Flex IGL" renders as e.g. "Initiator • Flex • IGL". Mirrors the
// IglBadge() implementation but uses kRoleFlex (the muted amber from
// PROJECT_GUIDE §5.2) as the background.
void FlexBadge() {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = 36.0f;
    float h = ImGui::GetFontSize() + 6.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), kRoleFlex, 4.0f);
    dl->AddText(ImVec2(p.x + 6, p.y + 3),
                IM_COL32(0x14, 0x14, 0x14, 0xFF), "Flex");
    ImGui::Dummy(ImVec2(w + 6, h));
}


// ========================================================================
// Design system (2026-05-15) — reusable card/tile/pill/header primitives.
// Every screen restyle should compose UI from THESE instead of bare ImGui
// widgets so the whole product stays visually consistent. All helpers are
// self-contained: no global state, safe to nest, no draw-order surprises.
// ========================================================================

inline bool brightish(ImU32 c) {
    int r = (c >> IM_COL32_R_SHIFT) & 0xFF;
    int g = (c >> IM_COL32_G_SHIFT) & 0xFF;
    int b = (c >> IM_COL32_B_SHIFT) & 0xFF;
    return (r * 299 + g * 587 + b * 114) / 1000 > 140;
}

// --- Card container -----------------------------------------------------
// Usage:
//   if (BeginCard("##id")) { ... content ... } EndCard();
// Auto-resizes vertically to its content; rounded; hairline border; inner
// padding. `bg`/`pad` optional. Width fills the available content region
// unless you wrap it in a fixed-width column / SameLine layout.
//
// 2026-06-15 depth upgrade: cards now paint a surface-hued stacked drop
// shadow (kShadow / kShadowSoft) BEHIND the child via parent-draw-list
// channel-splitting, plus a 1px top inner highlight for an "edge-lit"
// raised feel. Pass `hover_lift = true` to opt into a 13% kAccentSoft
// tint overlay when the card is hovered (default false keeps existing
// callers identical). Recommended `rounding` range: 8-10 for inner /
// dense cards, 12 (default) for standard surfaces, 14-16 for hero panels.

// Per-card state threaded from Begin* to End* so the shadow can be
// back-painted with the final rect known. Stack supports nesting.
//
// CRITICAL: `splitter` must be held by unique_ptr — ImDrawListSplitter contains
// an ImVector<ImDrawChannel> whose operator= uses shallow `memcpy`, so copying
// a CardFrame_ aliases the channel buffers between source and destination and
// double-frees them when both destructors run. That manifested as
// STATUS_HEAP_CORRUPTION (0xC0000374) the first time the dashboard tried to
// draw a card after the New Game Wizard handed control over. Holding the
// splitter by unique_ptr deletes the implicit copy ctor, forces move semantics
// in std::vector, and keeps a single owner of the channel buffers.
struct CardFrame_ {
    ImDrawList*                          parent_dl  = nullptr;
    std::unique_ptr<ImDrawListSplitter>  splitter;
    ImVec2                               start_pos  = ImVec2(0, 0);
    float                                rounding   = 0.f;
    bool                                 hover_lift = false;
    bool                                 opened     = false;
};
inline std::vector<CardFrame_>& CardStack_() {
    static std::vector<CardFrame_> s;
    return s;
}

// Push the shared style state used by every card variant. Kept in one
// place so any future token tweaks (border colour, padding rules) only
// have to change once.
inline void PushCardStyle_(ImU32 bg, float pad, float rounding) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, toV4(bg));
    ImGui::PushStyleColor(ImGuiCol_Border,  toV4(kBorder));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, rounding);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
}
inline void PopCardStyle_() {
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

// Begin a card frame: capture parent draw list + cursor, split into two
// channels (0 = shadow / underlay, 1 = card + content). All card rendering
// occurs on channel 1; the End* call paints the shadow into channel 0 then
// merges so the shadow lands BEHIND the card without depending on the
// auto-resized height being known up-front.
inline void CardBegin_(float rounding, bool hover_lift) {
    CardFrame_ f;
    f.parent_dl  = ImGui::GetWindowDrawList();
    f.start_pos  = ImGui::GetCursorScreenPos();
    f.rounding   = rounding;
    f.hover_lift = hover_lift;
    f.opened     = false;
    f.splitter   = std::make_unique<ImDrawListSplitter>();
    f.splitter->Split(f.parent_dl, 2);
    f.splitter->SetCurrentChannel(f.parent_dl, 1);
    CardStack_().push_back(std::move(f));
}

// Paint the top inner highlight + optional hover tint on the CHILD draw
// list. Must be called AFTER BeginChild returns true (inside the child).
inline void CardPaintInner_(float rounding, bool hover_lift) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 mn = ImGui::GetWindowPos();
    ImVec2 sz = ImGui::GetWindowSize();
    ImVec2 mx = ImVec2(mn.x + sz.x, mn.y + sz.y);
    // 1px top inner highlight, inset by rounding so it follows the corner.
    float inset = rounding;
    dl->AddLine(ImVec2(mn.x + inset, mn.y + 1.0f),
                ImVec2(mx.x - inset, mn.y + 1.0f),
                IM_COL32(255, 255, 255, 18), 1.0f);
    // Hover lift overlay — drawn on the child so it clips to the rounded
    // rect. IsWindowHovered() inside the child checks pointer-over on this
    // very window without depending on Item ordering quirks.
    if (hover_lift && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
        dl->AddRectFilled(mn, mx, kAccentSoft, rounding);
    }
}

// End a card frame: read the child's final rect, paint the stacked tinted
// shadow into channel 0 of the parent draw list, then merge. Safe to call
// even when BeginChild reported "not visible" — we still balanced the
// child stack and need to merge the splitter to leave the draw list sane.
inline void CardEnd_() {
    // Child rect captured BEFORE EndChild so we have the actual size that
    // ImGui resolved (covers both AutoResizeY and fixed-size variants).
    ImVec2 cmin = ImGui::GetWindowPos();
    ImVec2 csz  = ImGui::GetWindowSize();
    ImVec2 cmax = ImVec2(cmin.x + csz.x, cmin.y + csz.y);
    ImGui::EndChild();
    PopCardStyle_();

    // Operate on the stack entry IN PLACE — copying the frame would clone
    // the splitter's channel-buffer pointers and double-free at scope exit
    // (see CardFrame_ comment). Snapshot the scalar fields, then pop only
    // after Merge has flushed channel 1 back into the parent draw list.
    CardFrame_& back = CardStack_().back();
    ImDrawList* parent_dl = back.parent_dl;
    float       rounding  = back.rounding;
    ImDrawListSplitter* sp = back.splitter.get();
    // Back-paint stacked shadow on the underlay channel. kShadow is the
    // deeper drop (offset 6, surface-hued near-black), kShadowSoft is the
    // softer halo (offset 3) painted on top of it — together they read as
    // a single tinted glow rather than a hard black bar.
    sp->SetCurrentChannel(parent_dl, 0);
    parent_dl->AddRectFilled(ImVec2(cmin.x, cmin.y + 6.0f),
                             ImVec2(cmax.x, cmax.y + 6.0f),
                             kShadow, rounding);
    parent_dl->AddRectFilled(ImVec2(cmin.x, cmin.y + 3.0f),
                             ImVec2(cmax.x, cmax.y + 3.0f),
                             kShadowSoft, rounding);
    sp->Merge(parent_dl);
    CardStack_().pop_back();
}

bool BeginCard(const char* id, ImU32 bg = kSurface, float pad = 18.0f,
               float rounding = 12.0f, bool hover_lift = false) {
    CardBegin_(rounding, hover_lift);
    PushCardStyle_(bg, pad, rounding);
    bool open = ImGui::BeginChild(id, ImVec2(0, 0),
                    ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders,
                    ImGuiWindowFlags_NoScrollbar);
    if (open) {
        CardStack_().back().opened = true;
        CardPaintInner_(rounding, hover_lift);
    }
    return open;
}
void EndCard() {
    CardEnd_();
}

// Fixed-size card variant (for grid/row layouts where you need exact dims).
bool BeginCardSized(const char* id, ImVec2 size, ImU32 bg = kSurface,
                    float pad = 16.0f, float rounding = 12.0f,
                    bool hover_lift = false) {
    CardBegin_(rounding, hover_lift);
    PushCardStyle_(bg, pad, rounding);
    bool open = ImGui::BeginChild(id, size, ImGuiChildFlags_Borders,
                                  ImGuiWindowFlags_NoScrollbar);
    if (open) {
        CardStack_().back().opened = true;
        CardPaintInner_(rounding, hover_lift);
    }
    return open;
}
void EndCardSized() {
    CardEnd_();
}

// --- Rounded pill / chip ------------------------------------------------
// Inline chip with auto-measured width. Advances the cursor; use SameLine()
// before/after to chain. fg auto-picks black/white for contrast if 0.
void Pill(const char* text, ImU32 bg, ImU32 fg = 0, float extra_pad = 0.0f) {
    if (fg == 0) fg = brightish(bg) ? IM_COL32(0x12,0x12,0x14,0xFF) : kVlrText;
    ImVec2 ts = ImGui::CalcTextSize(text);
    float padx = 10.0f + extra_pad;
    float h = ImGui::GetFontSize() + 8.0f;
    float w = ts.x + padx * 2.0f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, h * 0.5f);
    dl->AddText(ImVec2(p.x + padx, p.y + 4.0f), fg, text);
    ImGui::Dummy(ImVec2(w, h));
}

// Outlined pill (subtle — for secondary/neutral tags).
void PillOutline(const char* text, ImU32 line, ImU32 fg = 0) {
    if (fg == 0) fg = line;
    ImVec2 ts = ImGui::CalcTextSize(text);
    float padx = 10.0f;
    float h = ImGui::GetFontSize() + 8.0f;
    float w = ts.x + padx * 2.0f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), line, h * 0.5f, 0, 1.4f);
    dl->AddText(ImVec2(p.x + padx, p.y + 4.0f), fg, text);
    ImGui::Dummy(ImVec2(w, h));
}

// Signature agent chip — small muted-gold inline pill rendered AFTER the
// gamertag via ImGui::SameLine() to show which agent the player is most
// identified with. Empty signature_agent() => no-op (skips render entirely).
//
// Before this helper, signature agents were displayed inconsistently across
// 4+ screens (gold `[*] X` prefix in news; dim `[X]` text in scoreboard;
// SIG column in MVP race; full hero strip in player profile). This helper
// is the single canonical inline form so the UI surface is consistent.
//
// Defined here (after Pill) so it can call Pill — that's why it's not in
// the badge cluster up above with IglBadge / FlexBadge.
void SigBadge(const vlr::Player& p) {
    std::string sig = p.signature_agent();
    if (sig.empty()) return;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s", sig.c_str());
    ImU32 bg = IM_COL32(0x55, 0x42, 0x14, 0xFF);  // muted gold-brown
    Pill(buf, bg, kVlrGold);
}

// --- Section header -----------------------------------------------------
// Accent rule + bold title + optional dim subtitle. Use at the top of a
// content block instead of bare H2() for stronger hierarchy.
void SectionHeader(const char* title, const char* subtitle = nullptr,
                   ImU32 accent = kAccent) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float barH = (g_font_h2 ? g_font_h2->FontSize : 22.0f);
    dl->AddRectFilled(p, ImVec2(p.x + 4.0f, p.y + barH), accent, 2.0f);
    ImGui::Indent(14.0f);
    if (g_font_h2) ImGui::PushFont(g_font_h2);
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrText));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    if (g_font_h2) ImGui::PopFont();
    if (subtitle && subtitle[0]) {
        if (g_font_small) ImGui::PushFont(g_font_small);
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
        ImGui::TextUnformatted(subtitle);
        ImGui::PopStyleColor();
        if (g_font_small) ImGui::PopFont();
    }
    ImGui::Unindent(14.0f);
    ImGui::Dummy(ImVec2(0, 6));
}

// --- TableRowSelectable -------------------------------------------------
// Drop-in wrapper around ImGui::Selectable() for clickable table rows. On
// hover, paints a warm-gold "lift":
//   (a) a 3px kAccent vertical bar along the LEFT edge of the row, and
//   (b) a faint kAccentSoft (13% gold) overlay across the full row width.
//
// IsItemHovered() is queried AFTER the Selectable renders, so the overlay
// paints over the cell contents. kAccentSoft sits at 13% alpha â€” light
// enough that the row text underneath stays readable, but strong enough
// to cue "this row is the active target" without relying on the bland
// default HeaderHovered tint.
//
// Use this for player rows, team rows, FA rows, tournament participant
// rows, leaderboards, HoF inductees, etc. DO NOT use for combo/dropdown
// items, sidebar nav buttons, or bracket / matchup cards (which carry
// their own bespoke hover paint).
bool TableRowSelectable(const char* label, bool selected = false,
                        ImGuiSelectableFlags flags = 0,
                        const ImVec2& size = ImVec2(0, 0)) {
    bool clicked = ImGui::Selectable(label, selected, flags, size);
    if (ImGui::IsItemHovered()) {
        ImVec2 row_min = ImGui::GetItemRectMin();
        ImVec2 row_max = ImGui::GetItemRectMax();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(row_min, row_max, kAccentSoft);
        dl->AddRectFilled(row_min, ImVec2(row_min.x + 3.0f, row_max.y),
                          kAccent);
    }
    return clicked;
}

// --- Stat tile ----------------------------------------------------------
// Compact KPI tile: small caps label, big number, optional accent + delta.
// Draw several with SameLine() between for a KPI strip.
void StatTile(const char* label, const std::string& value, ImU32 accent,
              ImVec2 size = ImVec2(0, 0), const char* sub = nullptr) {
    if (size.x <= 0) size.x = 168.0f;
    if (size.y <= 0) size.y = 92.0f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), kSurface, 12.0f);
    dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), kBorder, 12.0f, 0, 1.0f);
    // accent edge
    dl->AddRectFilled(p, ImVec2(p.x + 4.0f, p.y + size.y), accent,
                      12.0f, ImDrawFlags_RoundCornersLeft);
    float padx = 16.0f;
    if (g_font_small) ImGui::PushFont(g_font_small);
    dl->AddText(ImVec2(p.x + padx, p.y + 14.0f), kVlrSub, label);
    if (g_font_small) ImGui::PopFont();
    ImFont* bigf = g_font_kpi ? g_font_kpi : g_font_h1;
    if (bigf) {
        dl->AddText(bigf, bigf->FontSize, ImVec2(p.x + padx, p.y + 32.0f),
                    kVlrText, value.c_str());
    } else {
        dl->AddText(ImVec2(p.x + padx, p.y + 34.0f), kVlrText, value.c_str());
    }
    if (sub && sub[0]) {
        if (g_font_small) ImGui::PushFont(g_font_small);
        dl->AddText(ImVec2(p.x + padx, p.y + size.y - 22.0f), accent, sub);
        if (g_font_small) ImGui::PopFont();
    }
    ImGui::Dummy(size);
}

// --- Misc spacing helpers ----------------------------------------------
inline void VSpace(float h = 10.0f) { ImGui::Dummy(ImVec2(0, h)); }
void MutedText(const char* fmt, ...) {
    char buf[512];
    va_list a; va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);
    if (g_font_small) ImGui::PushFont(g_font_small);
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
    ImGui::TextUnformatted(buf);
    ImGui::PopStyleColor();
    if (g_font_small) ImGui::PopFont();
}
// Thin full-width divider with the hairline border colour.
void ThinDivider(float pad_y = 8.0f) {
    ImGui::Dummy(ImVec2(0, pad_y));
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddLine(p, ImVec2(p.x + w, p.y), kBorder, 1.0f);
    ImGui::Dummy(ImVec2(0, pad_y));
}

// === Procedural flag drawing ============================================
//
// Each country gets its real flag rendered into a 28x16 (or similar) box
// using ImGui's draw list primitives. No bitmaps shipped; flags are drawn
// from primitives so they look crisp at any zoom and ship with the exe.
//
// Layouts handled:
//   * Horizontal 3-stripe (DE, RU, NL, etc.)
//   * Vertical 3-stripe   (FR, IT, IE, MX, etc.)
//   * Horizontal 2-stripe (UA, ID, PL)
//   * Nordic cross        (SE, DK, FI, NO)
//   * Disc center         (JP)
//   * Star canton + stripes (US)
//   * Saltire             (SCT)
//   * Crescent + star     (TR — simplified)
//   * Disc swirl          (KR — yin-yang simplified)
//   * Yellow star on red  (VN, CN — simplified)
//   * Triangular hoist    (PH)
//   * Maple leaf          (CA — simplified diamond)
//
// Anything not handled falls back to the ISO-code stub.

struct RGB { unsigned int v; };
inline ImU32 hex(unsigned int rgb) {
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 0xFF);
}

namespace flagdraw {

void horiz3(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c1, ImU32 c2, ImU32 c3) {
    float h = (b.y - a.y) / 3.0f;
    dl->AddRectFilled(a, ImVec2(b.x, a.y + h), c1);
    dl->AddRectFilled(ImVec2(a.x, a.y + h), ImVec2(b.x, a.y + 2*h), c2);
    dl->AddRectFilled(ImVec2(a.x, a.y + 2*h), b, c3);
}
void vert3(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c1, ImU32 c2, ImU32 c3) {
    float w = (b.x - a.x) / 3.0f;
    dl->AddRectFilled(a, ImVec2(a.x + w, b.y), c1);
    dl->AddRectFilled(ImVec2(a.x + w, a.y), ImVec2(a.x + 2*w, b.y), c2);
    dl->AddRectFilled(ImVec2(a.x + 2*w, a.y), b, c3);
}
void horiz2(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c1, ImU32 c2) {
    float h = (b.y - a.y) / 2.0f;
    dl->AddRectFilled(a, ImVec2(b.x, a.y + h), c1);
    dl->AddRectFilled(ImVec2(a.x, a.y + h), b, c2);
}
void nordic_cross(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 bg, ImU32 cross) {
    dl->AddRectFilled(a, b, bg);
    float w = b.x - a.x, h = b.y - a.y;
    float thick = h * 0.20f;
    float hoist = w * 0.30f;  // vertical bar offset from the hoist (left)
    // horizontal bar
    dl->AddRectFilled(ImVec2(a.x, a.y + h*0.5f - thick*0.5f),
                      ImVec2(b.x, a.y + h*0.5f + thick*0.5f), cross);
    // vertical bar (offset toward hoist)
    dl->AddRectFilled(ImVec2(a.x + hoist - thick*0.5f, a.y),
                      ImVec2(a.x + hoist + thick*0.5f, b.y), cross);
}
void nordic_cross_bordered(ImDrawList* dl, ImVec2 a, ImVec2 b,
                           ImU32 bg, ImU32 border, ImU32 cross) {
    nordic_cross(dl, a, b, bg, border);
    // Inner cross slightly thinner.
    float w = b.x - a.x, h = b.y - a.y;
    float thick = h * 0.10f;
    float hoist = w * 0.30f;
    dl->AddRectFilled(ImVec2(a.x, a.y + h*0.5f - thick*0.5f),
                      ImVec2(b.x, a.y + h*0.5f + thick*0.5f), cross);
    dl->AddRectFilled(ImVec2(a.x + hoist - thick*0.5f, a.y),
                      ImVec2(a.x + hoist + thick*0.5f, b.y), cross);
}
void saltire(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 bg, ImU32 cross) {
    dl->AddRectFilled(a, b, bg);
    // Diagonal X. Approximated as 2 thick lines.
    float t = (b.y - a.y) * 0.18f;
    dl->AddLine(ImVec2(a.x, a.y), b, cross, t);
    dl->AddLine(ImVec2(b.x, a.y), ImVec2(a.x, b.y), cross, t);
}
void disc_center(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 bg, ImU32 disc) {
    dl->AddRectFilled(a, b, bg);
    float w = b.x - a.x, h = b.y - a.y;
    ImVec2 c = ImVec2(a.x + w*0.5f, a.y + h*0.5f);
    float r = std::min(w, h) * 0.30f;
    dl->AddCircleFilled(c, r, disc, 16);
}
// Filled 5-point star at center/origin, given outer radius r.
void draw_star(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
    ImVec2 pts[10];
    for (int i = 0; i < 10; ++i) {
        float ang = -3.14159265f * 0.5f + i * (3.14159265f / 5.0f);
        float rad = (i % 2 == 0) ? r : r * 0.45f;
        pts[i] = ImVec2(c.x + std::cos(ang) * rad, c.y + std::sin(ang) * rad);
    }
    dl->AddConvexPolyFilled(pts, 10, col);
}
void us_stars_stripes(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    float w = b.x - a.x, h = b.y - a.y;
    ImU32 red = hex(0xB22234), white = hex(0xFFFFFF), blue = hex(0x3C3B6E);
    // 13 alternating horizontal stripes.
    for (int i = 0; i < 13; ++i) {
        float y0 = a.y + i * h / 13.0f;
        float y1 = a.y + (i + 1) * h / 13.0f;
        dl->AddRectFilled(ImVec2(a.x, y0), ImVec2(b.x, y1),
                          (i % 2 == 0) ? red : white);
    }
    // Blue canton: 7/13 of height, 0.4 of width.
    dl->AddRectFilled(a, ImVec2(a.x + w * 0.40f, a.y + h * 7.0f / 13.0f), blue);
    // Star dots in canton (4 rows x 5 cols, suggestive of stars).
    float cw = w * 0.40f, ch = h * 7.0f / 13.0f;
    for (int sy = 0; sy < 4; ++sy) {
        for (int sx = 0; sx < 5; ++sx) {
            float dx = a.x + (sx + 1) * cw / 6.0f;
            float dy = a.y + (sy + 1) * ch / 5.0f;
            dl->AddCircleFilled(ImVec2(dx, dy), std::max(0.6f, cw * 0.020f), white, 6);
        }
    }
}
void union_jack(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    float w = b.x - a.x, h = b.y - a.y;
    ImU32 blue = hex(0x012169), white = hex(0xFFFFFF), red = hex(0xC8102E);
    dl->AddRectFilled(a, b, blue);
    // White diagonal saltires.
    float diag = h * 0.34f;
    dl->AddLine(ImVec2(a.x, a.y), b, white, diag);
    dl->AddLine(ImVec2(b.x, a.y), ImVec2(a.x, b.y), white, diag);
    // Red diagonal saltires (thinner).
    float diag_red = h * 0.17f;
    dl->AddLine(ImVec2(a.x, a.y), b, red, diag_red);
    dl->AddLine(ImVec2(b.x, a.y), ImVec2(a.x, b.y), red, diag_red);
    // White cross.
    float t = h * 0.30f;
    dl->AddRectFilled(ImVec2(a.x + w*0.5f - t*0.5f, a.y),
                      ImVec2(a.x + w*0.5f + t*0.5f, b.y), white);
    dl->AddRectFilled(ImVec2(a.x, a.y + h*0.5f - t*0.5f),
                      ImVec2(b.x, a.y + h*0.5f + t*0.5f), white);
    // Red cross (thinner).
    float tr = h * 0.18f;
    dl->AddRectFilled(ImVec2(a.x + w*0.5f - tr*0.5f, a.y),
                      ImVec2(a.x + w*0.5f + tr*0.5f, b.y), red);
    dl->AddRectFilled(ImVec2(a.x, a.y + h*0.5f - tr*0.5f),
                      ImVec2(b.x, a.y + h*0.5f + tr*0.5f), red);
}
void canada(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    float w = b.x - a.x, h = b.y - a.y;
    ImU32 red = hex(0xD52B1E), white = hex(0xFFFFFF);
    dl->AddRectFilled(a, ImVec2(a.x + w*0.25f, b.y), red);
    dl->AddRectFilled(ImVec2(a.x + w*0.25f, a.y), ImVec2(a.x + w*0.75f, b.y), white);
    dl->AddRectFilled(ImVec2(a.x + w*0.75f, a.y), b, red);
    // Maple-leaf approximation: a small red diamond cluster in the middle.
    float cx = a.x + w*0.5f, cy = a.y + h*0.5f;
    float r = h * 0.30f;
    ImVec2 d[4] = {
        ImVec2(cx, cy - r), ImVec2(cx + r*0.7f, cy),
        ImVec2(cx, cy + r), ImVec2(cx - r*0.7f, cy)
    };
    dl->AddConvexPolyFilled(d, 4, red);
}
void brazil(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    ImU32 green = hex(0x009C3B), yellow = hex(0xFFDF00), blue = hex(0x002776);
    dl->AddRectFilled(a, b, green);
    float w = b.x - a.x, h = b.y - a.y;
    // Yellow rhombus.
    ImVec2 cx = ImVec2(a.x + w*0.5f, a.y + h*0.5f);
    ImVec2 d[4] = {
        ImVec2(cx.x, a.y + h*0.10f),
        ImVec2(b.x - w*0.10f, cx.y),
        ImVec2(cx.x, b.y - h*0.10f),
        ImVec2(a.x + w*0.10f, cx.y)
    };
    dl->AddConvexPolyFilled(d, 4, yellow);
    // Blue disc center.
    dl->AddCircleFilled(cx, h * 0.22f, blue, 18);
}
void korea(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    ImU32 white = hex(0xFFFFFF), red = hex(0xC60C30), blue = hex(0x003478), black = hex(0x000000);
    dl->AddRectFilled(a, b, white);
    float w = b.x - a.x, h = b.y - a.y;
    ImVec2 c = ImVec2(a.x + w*0.5f, a.y + h*0.5f);
    float r = h * 0.32f;
    // Yin-yang: red top half (curve), blue bottom (curve). Approximate with
    // 2 half-circles + 2 small nibs.
    dl->AddCircleFilled(c, r, red, 24);
    // Mask bottom-half blue.
    dl->PathArcTo(c, r, 0, 3.14159265f, 24);
    dl->PathLineTo(ImVec2(c.x - r, c.y));
    dl->PathFillConvex(blue);
    // Trigram dots (4 corners) — just three short bars per corner suggestion.
    auto bar = [&](ImVec2 p, float bw, float bh) {
        dl->AddRectFilled(ImVec2(p.x - bw*0.5f, p.y - bh*0.5f),
                          ImVec2(p.x + bw*0.5f, p.y + bh*0.5f), black);
    };
    float bw = w * 0.10f, bh = h * 0.04f;
    bar(ImVec2(a.x + w*0.13f, a.y + h*0.20f), bw, bh);
    bar(ImVec2(b.x - w*0.13f, a.y + h*0.20f), bw, bh);
    bar(ImVec2(a.x + w*0.13f, b.y - h*0.20f), bw, bh);
    bar(ImVec2(b.x - w*0.13f, b.y - h*0.20f), bw, bh);
}
void turkey(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    ImU32 red = hex(0xE30A17), white = hex(0xFFFFFF);
    dl->AddRectFilled(a, b, red);
    float w = b.x - a.x, h = b.y - a.y;
    ImVec2 c = ImVec2(a.x + w*0.40f, a.y + h*0.5f);
    float r = h * 0.30f;
    dl->AddCircleFilled(c, r, white, 18);
    // Inner red disc offset right.
    dl->AddCircleFilled(ImVec2(c.x + r*0.30f, c.y), r * 0.85f, red, 18);
    // Star.
    draw_star(dl, ImVec2(c.x + r*1.05f, c.y), r * 0.45f, white);
}
void vietnam(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    ImU32 red = hex(0xDA251D), yellow = hex(0xFFFF00);
    dl->AddRectFilled(a, b, red);
    float w = b.x - a.x, h = b.y - a.y;
    draw_star(dl, ImVec2(a.x + w*0.5f, a.y + h*0.5f), h * 0.32f, yellow);
}
void china(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    ImU32 red = hex(0xDE2910), yellow = hex(0xFFDE00);
    dl->AddRectFilled(a, b, red);
    float w = b.x - a.x, h = b.y - a.y;
    draw_star(dl, ImVec2(a.x + w*0.18f, a.y + h*0.30f), h * 0.20f, yellow);
    // 4 small surrounding stars.
    draw_star(dl, ImVec2(a.x + w*0.32f, a.y + h*0.14f), h * 0.07f, yellow);
    draw_star(dl, ImVec2(a.x + w*0.38f, a.y + h*0.28f), h * 0.07f, yellow);
    draw_star(dl, ImVec2(a.x + w*0.38f, a.y + h*0.45f), h * 0.07f, yellow);
    draw_star(dl, ImVec2(a.x + w*0.32f, a.y + h*0.58f), h * 0.07f, yellow);
}
void thailand(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    ImU32 red = hex(0xED1C24), white = hex(0xFFFFFF), blue = hex(0x241D4F);
    float w = b.x - a.x, h = b.y - a.y;
    // 5 horizontal stripes: red(1), white(1), blue(2), white(1), red(1) -> 6 units
    // Use proportions 1-1-2-1-1 = 6.
    auto stripe = [&](float y0, float y1, ImU32 c) {
        dl->AddRectFilled(ImVec2(a.x, a.y + y0 * h), ImVec2(b.x, a.y + y1 * h), c);
        (void)w;
    };
    stripe(0.0f,    1.0f/6,  red);
    stripe(1.0f/6,  2.0f/6,  white);
    stripe(2.0f/6,  4.0f/6,  blue);
    stripe(4.0f/6,  5.0f/6,  white);
    stripe(5.0f/6,  1.0f,    red);
}
void philippines(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    ImU32 blue = hex(0x0038A8), red = hex(0xCE1126), white = hex(0xFFFFFF), yellow = hex(0xFCD116);
    float w = b.x - a.x, h = b.y - a.y;
    horiz2(dl, a, b, blue, red);
    // White triangle on the hoist (left).
    ImVec2 tri[3] = {
        ImVec2(a.x, a.y), ImVec2(a.x, b.y), ImVec2(a.x + w*0.42f, a.y + h*0.5f)
    };
    dl->AddConvexPolyFilled(tri, 3, white);
    draw_star(dl, ImVec2(a.x + w*0.10f, a.y + h*0.5f), h * 0.10f, yellow);
}
void argentina(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    ImU32 light = hex(0x74ACDF), white = hex(0xFFFFFF), gold = hex(0xF6B40E);
    horiz3(dl, a, b, light, white, light);
    float w = b.x - a.x, h = b.y - a.y;
    dl->AddCircleFilled(ImVec2(a.x + w*0.5f, a.y + h*0.5f), h * 0.10f, gold, 12);
}
void chile(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    ImU32 white = hex(0xFFFFFF), red = hex(0xD52B1E), blue = hex(0x0039A6);
    float w = b.x - a.x, h = b.y - a.y;
    horiz2(dl, a, b, white, red);
    // Blue square with white star (hoist top corner).
    dl->AddRectFilled(a, ImVec2(a.x + w*0.5f * (h/(h*1.0f)) * 0.5f, a.y + h*0.5f), blue);
    // simpler: square = h x h/2 vertical, w/4 wide-ish
    dl->AddRectFilled(a, ImVec2(a.x + w*0.33f, a.y + h*0.5f), blue);
    draw_star(dl, ImVec2(a.x + w*0.16f, a.y + h*0.25f), h * 0.16f, white);
}
}  // namespace flagdraw

// Public entry point — draws the country flag at the current cursor.
//
// Phase 2 (Agent C): now prefers Agent B's embedded ARGB bitmap from
// FlagBitmaps.h when the ISO is known. Each 24x16 source pixel is rendered
// as a (w/24, h/16) AddRectFilled rect (~384 rects per flag — acceptable
// at the sizes / quantities the UI renders). Unknown ISOs fall back to the
// existing per-country procedural draw, which in turn falls through to the
// ISO-stub placeholder if even that doesn't recognise the code.
void CountryFlag(std::string_view iso) {
    if (iso.empty()) {
        ImGui::Dummy(ImVec2(30, ImGui::GetFontSize() + 4));
        return;
    }
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = 28.0f;
    float h = ImGui::GetFontSize() + 4.0f;
    ImVec2 a = pos, b = ImVec2(pos.x + w, pos.y + h);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // === Phase 2: try real bitmap first =====================================
    {
        vlr::FlagBitmap fb = vlr::get_flag_bitmap(iso);
        if (fb.pixels && fb.width > 0 && fb.height > 0) {
            float px = w / static_cast<float>(fb.width);
            float py = h / static_cast<float>(fb.height);
            for (int y = 0; y < fb.height; ++y) {
                for (int x = 0; x < fb.width; ++x) {
                    std::uint32_t argb = fb.pixels[y * fb.width + x];
                    // Pack ARGB -> ImU32 (IM_COL32 expects R,G,B,A).
                    std::uint8_t A = (argb >> 24) & 0xFF;
                    std::uint8_t R = (argb >> 16) & 0xFF;
                    std::uint8_t G = (argb >>  8) & 0xFF;
                    std::uint8_t B =  argb        & 0xFF;
                    if (A == 0) A = 0xFF;  // flags are opaque
                    ImU32 col = IM_COL32(R, G, B, A);
                    ImVec2 p0(pos.x + x * px,         pos.y + y * py);
                    ImVec2 p1(pos.x + (x + 1) * px,   pos.y + (y + 1) * py);
                    dl->AddRectFilled(p0, p1, col);
                }
            }
            // Outline.
            dl->AddRect(a, b, IM_COL32(0, 0, 0, 0xC0), 1.0f, 0, 1.0f);
            ImGui::Dummy(ImVec2(w + 4, h));
            return;
        }
    }

    // === Procedural fallback (preserved for ISOs not in the bitmap pool) ====
    // Per-country dispatch.
    bool drew = true;
    if      (iso == "us")  flagdraw::us_stars_stripes(dl, a, b);
    else if (iso == "gb")  flagdraw::union_jack(dl, a, b);
    else if (iso == "au") {
        // Australia: blue field + Union Jack canton + white stars.
        dl->AddRectFilled(a, b, hex(0x012169));
        ImVec2 cb = ImVec2(a.x + w*0.5f, a.y + h*0.5f);
        flagdraw::union_jack(dl, a, cb);
        flagdraw::draw_star(dl, ImVec2(b.x - w*0.18f, b.y - h*0.20f), h * 0.10f, hex(0xFFFFFF));
        flagdraw::draw_star(dl, ImVec2(b.x - w*0.34f, a.y + h*0.40f), h * 0.07f, hex(0xFFFFFF));
        flagdraw::draw_star(dl, ImVec2(b.x - w*0.10f, a.y + h*0.55f), h * 0.07f, hex(0xFFFFFF));
    }
    else if (iso == "ca")  flagdraw::canada(dl, a, b);
    else if (iso == "br")  flagdraw::brazil(dl, a, b);
    else if (iso == "kr")  flagdraw::korea(dl, a, b);
    else if (iso == "jp")  flagdraw::disc_center(dl, a, b, hex(0xFFFFFF), hex(0xBC002D));
    else if (iso == "tr")  flagdraw::turkey(dl, a, b);
    else if (iso == "vn")  flagdraw::vietnam(dl, a, b);
    else if (iso == "cn")  flagdraw::china(dl, a, b);
    else if (iso == "th")  flagdraw::thailand(dl, a, b);
    else if (iso == "ph")  flagdraw::philippines(dl, a, b);
    else if (iso == "ar")  flagdraw::argentina(dl, a, b);
    else if (iso == "cl")  flagdraw::chile(dl, a, b);
    else if (iso == "fr")  flagdraw::vert3(dl, a, b, hex(0x002654), hex(0xFFFFFF), hex(0xCE1126));
    else if (iso == "it")  flagdraw::vert3(dl, a, b, hex(0x008C45), hex(0xF4F9FF), hex(0xCD212A));
    else if (iso == "ie")  flagdraw::vert3(dl, a, b, hex(0x009A44), hex(0xFFFFFF), hex(0xFF7900));
    else if (iso == "mx")  flagdraw::vert3(dl, a, b, hex(0x006847), hex(0xFFFFFF), hex(0xCE1126));
    else if (iso == "es")  flagdraw::horiz3(dl, a, b, hex(0xC60B1E), hex(0xFFC400), hex(0xC60B1E));
    else if (iso == "de")  flagdraw::horiz3(dl, a, b, hex(0x000000), hex(0xDD0000), hex(0xFFCE00));
    else if (iso == "ru")  flagdraw::horiz3(dl, a, b, hex(0xFFFFFF), hex(0x0039A6), hex(0xD52B1E));
    else if (iso == "nl")  flagdraw::horiz3(dl, a, b, hex(0xAE1C28), hex(0xFFFFFF), hex(0x21468B));
    else if (iso == "ua")  flagdraw::horiz2(dl, a, b, hex(0x005BBB), hex(0xFFD500));
    else if (iso == "id")  flagdraw::horiz2(dl, a, b, hex(0xFF0000), hex(0xFFFFFF));
    else if (iso == "pl")  flagdraw::horiz2(dl, a, b, hex(0xFFFFFF), hex(0xDC143C));
    else if (iso == "se")  flagdraw::nordic_cross(dl, a, b, hex(0x006AA7), hex(0xFECC00));
    else if (iso == "dk")  flagdraw::nordic_cross(dl, a, b, hex(0xC8102E), hex(0xFFFFFF));
    else if (iso == "fi")  flagdraw::nordic_cross(dl, a, b, hex(0xFFFFFF), hex(0x002F6C));
    else if (iso == "no")  flagdraw::nordic_cross_bordered(dl, a, b, hex(0xEF2B2D), hex(0xFFFFFF), hex(0x002868));
    else if (iso == "sct") flagdraw::saltire(dl, a, b, hex(0x0065BD), hex(0xFFFFFF));
    else if (iso == "sg") {
        flagdraw::horiz2(dl, a, b, hex(0xED2939), hex(0xFFFFFF));
        ImVec2 cc(a.x + w*0.30f, a.y + h*0.25f);
        dl->AddCircleFilled(cc, h*0.18f, hex(0xFFFFFF), 16);
        dl->AddCircleFilled(ImVec2(cc.x + w*0.05f, cc.y), h*0.16f, hex(0xED2939), 16);
        flagdraw::draw_star(dl, ImVec2(a.x + w*0.50f, a.y + h*0.25f), h*0.06f, hex(0xFFFFFF));
    }
    else drew = false;

    if (!drew) {
        // Fallback: 3 dark bands + ISO code.
        dl->AddRectFilled(a, b, kVlrPanel);
    }

    // Outline.
    dl->AddRect(a, b, IM_COL32(0, 0, 0, 0xC0), 1.0f, 0, 1.0f);

    if (!drew) {
        std::string label(iso);
        for (auto& c : label) c = (char)std::toupper((unsigned char)c);
        ImVec2 ts = ImGui::CalcTextSize(label.c_str());
        ImGui::PushFont(g_font_small);
        dl->AddText(ImVec2(pos.x + (w - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f),
                    IM_COL32(0xFF, 0xFF, 0xFF, 0xC0), label.c_str());
        ImGui::PopFont();
    }
    ImGui::Dummy(ImVec2(w + 4, h));
}

void H1(const char* s, ImU32 col = kVlrText) {
    ImGui::PushFont(g_font_h1);
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(col));
    ImGui::TextUnformatted(s);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}
void H2(const char* s, ImU32 col = kVlrGold) {
    ImGui::PushFont(g_font_h2);
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(col));
    ImGui::TextUnformatted(s);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}
void Sub(const char* fmt, ...) {
    ImGui::PushFont(g_font_small);
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
    va_list args;
    va_start(args, fmt);
    ImGui::TextV(fmt, args);
    va_end(args);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

// Display label for a player: gamertag with an "[I] " prefix when the
// player is the team's IGL. Used by every UI surface that shows a player
// name so the IGL flag is visible at a glance — table rows, kill feed,
// MVP banner, etc.
std::string player_label(const vlr::Player& p) {
    return p.is_igl ? std::string("[I] ") + p.name : p.name;
}

// === Scouting fog-of-war display (Group C) ==============================
// Known = the user's org has scouted this player OR owns them (you know your
// own talent). Gates the exact-potential reveal across the market + profile.
inline bool potential_known(const vlr::GameManager& gm, const vlr::Player& p) {
    return p.potential_scouted ||
           (gm.user_team && p.team_name == gm.user_team->name);
}
// Deterministic uncertainty band around the true potential. WS-A INC-3: the
// ENGINE owns the id-seeded hash + the scout-quality tightening (GameManager::
// scouted_band), so this is a thin presenter wrapper — Market and Profile and
// every frame agree, and a sharper head scout reads the band tighter.
inline void potential_band(const vlr::GameManager& gm, const vlr::Player& p, int& lo, int& hi) {
    gm.scouted_band(p, lo, hi);
}
// The potential value the user is ALLOWED to filter/sort the market by: the true
// value when known, else the band MIDPOINT (already derivable from the shown
// band, so it leaks nothing new — unlike comparing the hidden exact value, which
// a slider sweep could binary-search).
inline int effective_potential(const vlr::GameManager& gm, const vlr::Player& p) {
    if (potential_known(gm, p)) return p.potential;
    int lo, hi; potential_band(gm, p, lo, hi); return (lo + hi) / 2;
}

// Map the engine-stamped trajectory class (item 8) to a display label + color.
// The engine computes the CLASS from real recent OVR movement + form at each
// monthly progression tick; the GUI only presents it (so it's stable between
// ticks and never recomputed live). Color constants live here, not the engine.
std::pair<const char*, ImU32> traj_display(vlr::Player::TrajClass tc) {
    using TC = vlr::Player::TrajClass;
    switch (tc) {
        case TC::FutureStar: return {"Future Star \xE2\x96\xB2", kVlrGreen};
        case TC::Rising:     return {"Rising \xE2\x96\xB2",      kVlrGreen};
        case TC::Developing: return {"Developing \xE2\x96\xB2",  kAccent};
        case TC::Slump:      return {"Slump \xE2\x96\xBC",       kRoleFlex};   // amber warning
        case TC::Declining:  return {"Declining \xE2\x96\xBC",   kVlrRedDim};
        case TC::Twilight:   return {"Twilight \xE2\x80\x94",    kVlrSub};
        case TC::Established:
        default:             return {"Established \xE2\x80\x94", kVlrSub};
    }
}

std::string fmt_money(long long dollars) {
    char buf[40];
    // Scale on MAGNITUDE so negative values still get K/M units (a negative
    // budget used to fall through both branches and render raw, e.g.
    // "$-2500000"). Sign is prepended: "-$2.50M".
    const char* sign = dollars < 0 ? "-" : "";
    long long a = dollars < 0 ? -dollars : dollars;
    if (a >= 1000000)   std::snprintf(buf, sizeof(buf), "%s$%.2fM", sign, a / 1000000.0);
    else if (a >= 1000) std::snprintf(buf, sizeof(buf), "%s$%lldK", sign, a / 1000);
    else                std::snprintf(buf, sizeof(buf), "%s$%lld",  sign, a);
    return buf;
}

// Format a value already expressed in THOUSANDS of dollars (the *_k fields:
// budgets-in-K, payroll, revenue). Routes through fmt_money so the whole app
// shares ONE $ / $K / $M convention and the same sign handling — no more
// "$1.50M" sitting next to "$1500K" in the same row.
std::string fmt_money_k(long long thousands) {
    return fmt_money(thousands * 1000LL);
}

// KPI card with big-number font; 220x90px.
void KpiCard(const char* label, const std::string& value, ImU32 color) {
    ImGui::BeginChild(label, ImVec2(220, 90), ImGuiChildFlags_Border);
    ImGui::PushFont(g_font_kpi);
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(color));
    ImGui::TextUnformatted(value.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::PushFont(g_font_small);
    ImGui::TextDisabled("%s", label);
    ImGui::PopFont();
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Team color helpers + TeamLogo widget (Phase 2)
// ---------------------------------------------------------------------------
// Team stores primary/accent colors as 3 ints (_r/_g/_b). Pack them into the
// ImU32 form LogoArt::draw_team_logo expects.
inline ImU32 team_primary_color(const vlr::Team& t) {
    return IM_COL32(t.color_primary_r, t.color_primary_g, t.color_primary_b, 0xFF);
}
inline ImU32 team_accent_color(const vlr::Team& t) {
    return IM_COL32(t.color_accent_r,  t.color_accent_g,  t.color_accent_b,  0xFF);
}

// Renders the team's procedural logo at the current cursor as a `size` x
// `size` block. Reserves layout via Dummy(). Threads the team's TAG into
// the badge renderer so the monogram wordmark is the team's actual brand
// identifier (e.g. "NRG", "FNX") rather than a generic placeholder.
void TeamLogo(const vlr::Team& team, float size = 32.f) {
    auto dl = ImGui::GetWindowDrawList();
    auto pos = ImGui::GetCursorScreenPos();
    ImVec2 center{pos.x + size * 0.5f, pos.y + size * 0.5f};
    ImU32 primary = team_primary_color(team);
    ImU32 accent  = team_accent_color(team);
    vlr::draw_team_logo(dl, center, size * 0.45f,
                        static_cast<std::uint8_t>(team.logo_shape),
                        primary, accent,
                        team.tag.c_str());
    ImGui::Dummy(ImVec2(size, size));
}

// Convenience: render logo + tag text on a single line, both clickable
// (the caller wraps with its own Selectable / PushID as needed).
void TeamLogoInline(const vlr::Team& team, float size = 24.f) {
    TeamLogo(team, size);
    ImGui::SameLine();
}

}  // namespace

// ============================================================
// 3. App state
// ============================================================
namespace {

enum class Screen {
    Dashboard, Roster, Standings, Brackets, Market, SoloQ,
    EventLog, Watch, Calendar, Favorites,
    TeamProfile, LeagueLeaders, Manager, News,
    // Phase 1 additions (Agent C):
    PowerRankings,  // Power + Community rankings (Community is a sub-tab)
    Compare,        // Side-by-side player comparison
    // Pack C C7 — six grouped sidebar entries that route via an internal
    // tab bar to the existing leaf Draw* functions. Old leaf values remain
    // valid (used by deep-links like OpenTeamProfile / OpenLiveMatch
    // restore paths).
    GroupHome,         // tabs: Dashboard | News
    GroupTeam,         // tabs: Roster | Manager | Calendar
    GroupCompetition,  // tabs: Standings | Tournaments | Power Rankings
    GroupPeople,       // tabs: Market | Solo Q | League Leaders | Compare
    GroupHistory,      // tabs: Favorites | Event Log
    GroupLive,         // tabs: Watch
    GroupAwards,       // tabs: MVP Race | League Leaders | Season Awards | History
    GroupFinance,      // FM-depth finance dashboard (your club + league + transfers)
    GroupMail          // per-club inbox: list + reading pane + deep-link actions
};

// Live Match viewer can operate on either:
//  - a freshly-simulated match held by `match` (for the Watch screen), or
//  - a previously-recorded match held by `recording` (for replays from a
//    player profile).
// Both paths route through `RecordedMatch` for the actual draw code.
struct LiveMatchState {
    std::shared_ptr<vlr::Match>     match;       // optional, holds new sim alive
    vlr::RecordedMatchPtr           recording;   // the CURRENT map being viewed
    // Series support — for BO3/BO5 the user can step through each map.
    // series[0] is map 1, series[1] is map 2, etc. current_map_idx is the
    // index of `recording` within `series`. Empty means single-map viewing.
    std::vector<vlr::RecordedMatchPtr> series;
    int  current_map_idx = 0;
    int  round_cursor   = 0;
    // Within-round event cursor for play-by-play mode. When auto_play is
    // on, kills tick out one at a time (event_cursor++) until the round
    // is exhausted, then we advance round_cursor and reset event_cursor.
    int   event_cursor   = 0;
    // Speed control: 0=Slow (1s/kill), 1=Normal (0.5s), 2=Fast (0.25s),
    // 3=Skip-rounds (legacy whole-round-at-a-time).
    int   playback_speed = 1;
    bool auto_play      = false;
    float auto_timer    = 0.0f;
    bool open           = false;
};

struct AppState {
    vlr::GameManager gm;
    Screen           screen = Screen::GroupHome;
    std::vector<std::string> log;

    std::string ladder_region = "Americas";
    int  soloq_min_mmr = 1100;
    char search[64] = {};

    // === Transfer-market filters (Group B) ===============================
    // -1 / 0 mean "no filter". role indexes vlr::Position; region indexes the
    // kRegions list (+ -1 = All). min_ovr/min_pot/max_ask are inclusive bounds.
    int  market_role   = -1;
    int  market_region = -1;
    int  market_min_ovr = 0;
    int  market_min_pot = 0;
    int  market_max_ask = 0;   // 0 = no cap

    vlr::PlayerPtr selected_player;
    bool show_player_modal = false;
    bool show_match_history_modal = false;
    int  history_filter = 0;  // 0 = pro, 1 = solo

    // === Agent C: Re-sign negotiation modal ============================
    // Opened from a roster row's "Re-sign" SmallButton when a player's
    // contract is in its expiring/final-year window. Driven by Agent A's
    // `Player::propose_resign_offer` + `Player::accepts_resign_offer`
    // contract and Agent B's `Team::resign_player` mutator.
    bool          show_resign_modal   = false;
    vlr::PlayerPtr resign_target;
    int           resign_years_choice = 2;
    int           resign_amount_choice = 0;   // 0 = use player's ask
    // -1 sentinel = "use player's primary_role" (no role change). When the
    // user picks a non-natural role from the combo, this stores the role
    // index 0..3 (Duelist/Initiator/Controller/Sentinel).
    int           resign_role_choice  = -1;
    // Cached at the moment the modal opens. `propose_resign_offer` calls
    // `gen_contract` with randomize_amount=true, so recomputing every frame
    // produces ±10-20% jitter on the slider's max bound — the slider
    // visibly oscillates left/right every frame as ImGui re-clamps. Cache
    // once when `resign_target` changes; reuse for the modal's lifetime.
    vlr::Player::ResignOffer resign_cached_offer{};
    int           resign_cached_mkt_k = 0;
    vlr::Player*  resign_cached_for   = nullptr;
    // Multi-year structure + reject-cost session state (reset when the modal
    // opens a new target). bonus = one-time signing bonus $K; promise = a
    // guaranteed starter slot; offers_left = lowball retries before the player
    // walks from talks this session.
    int           resign_bonus_choice    = 0;
    bool          resign_promise_starter = false;
    int           resign_offers_left     = 3;

    // === FA Sign negotiation modal (2026-05-28) ========================
    // Replaces the old "direct sign" flow in DrawMarket. Mirrors the
    // re-sign modal layout (player's ask, willingness, role offer combo,
    // breakdown, accept/walk).
    bool          show_fa_modal      = false;
    vlr::PlayerPtr fa_target;
    int           fa_years_choice    = 2;
    int           fa_amount_choice   = 0;
    int           fa_role_choice     = -1;
    vlr::Player::ResignOffer fa_cached_offer{};
    int           fa_cached_mkt_k    = 0;
    vlr::Player*  fa_cached_for      = nullptr;
    int           fa_bonus_choice    = 0;
    bool          fa_promise_starter = false;
    int           fa_offers_left     = 3;

    // === Make-Offer for a contracted player (Group D) ==================
    // Inline panel inside the player-profile modal. offer_for pins which player
    // the fee + result belong to (reset when the profile switches players).
    bool          offer_panel_open  = false;
    vlr::Player*  offer_for         = nullptr;
    int           offer_fee_choice  = 0;
    int           offer_result_code = -1;   // -1 = none; else TransferBidOutcome::Code
    std::string   offer_result_msg;

    bool show_history_detail_modal = false;
    vlr::RecordedMatchPtr history_detail;

    bool god_mode = false;

    LiveMatchState live;

    // Selected team for the Team Profile screen. Set by OpenTeamProfile()
    // from any clickable team row (Standings, Brackets, Calendar, etc.).
    vlr::TeamPtr selected_team;
    // Back-navigation stack for Team Profile drill-downs. Each entry is the
    // (screen, selected_team) we left to open a profile, so Back returns
    // step-by-step through A->B->C instead of jumping to the origin. TeamPtrs
    // stay valid for the world's life (never-free invariant).
    std::vector<std::pair<Screen, vlr::TeamPtr>> nav_stack;

    // League Leaders filters.
    int  ll_region_filter = 0;     // 0 = all, 1..3 = Americas/EMEA/Pacific
    int  ll_min_matches   = 10;
    int  ll_category      = 0;     // current leaderboard tab

    // GOAT formula weights — user-tweakable in the Favorites screen's
    // "Formula Lab" tab.
    vlr::GoatWeights goat_weights{};
    int goat_tab = 0;  // 0 favorited, 1 career goat, 2 season goat, 3 lab

    // Watch-screen team selection
    int watch_t1 = 0;
    int watch_t2 = 1;
    std::string watch_region = "Americas";

    // === Phase 2: Calendar grid cursor + per-day modal ====================
    int cal_cursor_day  = -1;  // -1 sentinel: snap to today on first draw
    int cal_year_offset = 0;   // +/- N if the user paged across year boundary
    int cal_modal_day   = -1;  // -1 closed; else day_in_year being shown
    int cal_modal_year  = 0;
    std::vector<vlr::UpcomingFixture> cal_modal_fixtures;

    // === Phase 2: New-Game Wizard (full-screen modal pre-init) ============
    // When true, wWinMain skips the normal sidebar/screen pipeline and draws
    // DrawNewGameWizard instead. Default true so a fresh launch shows the
    // wizard. Cleared once the user clicks "Start New Career".
    bool show_new_game_wizard = true;
    vlr::NewGameConfig cfg;
    bool show_new_career_warning = false;   // confirmation modal when user
                                            // clicks the sidebar's "New Career"
                                            // button mid-game.
    vlr::PlayerPtr release_confirm_target;  // QW: pending player-release confirm
    // Tracks whether the user manually picked colors yet — auto-hash from
    // org name updates them only while still false.
    bool cfg_colors_user_picked = false;

    // === Mail / Inbox screen state ========================================
    // selected_mail_id is resolved against gm.mailbox by ID every frame (never
    // a cached pointer/index) so a deleted or cap-dropped item simply clears
    // the reading pane instead of dangling. mail_filter is -1 for "All" or an
    // (int)vlr::GameManager::MailCategory 0..6.
    int  selected_mail_id     = -1;
    int  mail_filter          = -1;
    bool mail_unread_only     = false;
    bool mail_important_only  = false;

    // === Async simulation worker ==========================================
    // advance_day / simulate_full_season / skip helpers can take >16 ms on
    // matchdays (solo Q ticks, pro match sim, news emitters). Running them
    // on the UI thread freezes the frame. We run them on a worker thread
    // and show a "Simulating..." overlay during the wait. The overlay also
    // SUPPRESSES normal page rendering so we don't race against the worker
    // mutating engine state — only the overlay draws while sim_running.
    std::atomic<bool> sim_running{false};
    std::thread sim_thread;
    std::string sim_status;            // "Continuing...", "Skipping to next match...", etc.
    int sim_progress_total = 0;        // for skip helpers — informational only
    int sim_progress_done  = 0;
    std::vector<std::string> sim_log;  // worker writes here; UI drains after join
    // DayResult(s) the worker produced. UI thread drains via process_day_result
    // (opens live viewer modal, pushes log lines) once the worker is joined.
    std::vector<vlr::DayResult> pending_results;

    ~AppState() {
        // Defensive — if window closes mid-sim, wait for worker to finish so
        // we don't destroy engine state from under it.
        if (sim_thread.joinable()) sim_thread.join();
    }
};

// Forward decls (definitions later in the file).
void OpenReplay(AppState& s, vlr::RecordedMatchPtr rec);
void OpenLiveMatch(AppState& s, vlr::TeamPtr a, vlr::TeamPtr b, const std::string& event);
void OpenLiveMatchSeries(AppState& s, vlr::TeamPtr a, vlr::TeamPtr b,
                         const std::string& event, int best_of);

// Phase 1 (Agent C) additions — see end of file for definitions.
void DrawPowerRankings(AppState& s);
void DrawCompare(AppState& s);
vlr::TeamPtr find_team_by_name(AppState& s, const std::string& name);  // defined far below
void DrawTrophyRoomTab(AppState& s);
void DrawHallOfRecordsTab(AppState& s);
void DrawGreatestSeasonsTab(AppState& s);

// Phase 2 (Agent C) additions — see end of file for definitions.
void DrawLeagueLeaders(AppState& s);
void DrawMvpRaceBody(AppState& s);
void DrawAwardsRecapBody(AppState& s);
void DrawAwardsHistoryBody(AppState& s);
void DrawAwards(AppState& s);
void DrawSeasonStats(AppState& s);
void DrawFinanceDashboard(AppState& s);
void DrawInbox(AppState& s);
void DrawNewGameWizard(AppState& s);
void DrawNewCareerWarningModal(AppState& s);

// Frivolities + history pack — defined later in the file.
std::vector<vlr::PlayerPtr> all_players_world(AppState& s);

void OpenPlayerModal(AppState& s, vlr::PlayerPtr p) {
    s.selected_player = p;
    s.show_player_modal = true;
    // Reset the Make-Offer panel for the newly opened player (Group D).
    s.offer_panel_open = false;
    s.offer_for = nullptr;
    s.offer_result_code = -1;
    s.offer_result_msg.clear();
}

// Open the Team Profile screen for the given team. Records the screen the
// user came from so we can offer a Back button.
void OpenTeamProfile(AppState& s, vlr::TeamPtr t) {
    if (!t) return;
    if (s.screen != Screen::TeamProfile) {
        // Fresh drill-in from a non-profile screen: start a new Back chain
        // rooted at the screen we came from. Clearing here also discards any
        // stale chain left over from top-nav navigation.
        s.nav_stack.clear();
        s.nav_stack.push_back({s.screen, nullptr});
    } else if (s.selected_team != t) {
        // Profile -> profile: remember the team we're leaving so Back returns
        // to it rather than skipping straight past it.
        s.nav_stack.push_back({Screen::TeamProfile, s.selected_team});
    }
    s.selected_team = t;
    s.screen = Screen::TeamProfile;
}

}  // namespace

// ============================================================
// 5. Modal: Player Profile (incl. God Mode editor)
// ============================================================
namespace {

void DrawAttrSlider(const char* label, int& v) {
    ImGui::PushID(label);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(170);
    ImGui::SetNextItemWidth(280);
    ImGui::SliderInt("##s", &v, 1, 99);
    ImGui::PopID();
}

#include "gui_player_modal.inc"

void DrawHistoryDetailModal(AppState& s) {
    if (!s.show_history_detail_modal || !s.history_detail) return;
    ImGui::OpenPopup("Match Detail##modal");
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(640, 360), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Match Detail##modal", &s.show_history_detail_modal,
                               ImGuiWindowFlags_NoSavedSettings)) {
        auto& m = *s.history_detail;
        {
            char msub[128];
            std::snprintf(msub, sizeof(msub), "Map: %s%s",
                          m.map_name.c_str(), m.is_solo_q ? "  (Solo Q)" : "");
            SectionHeader(m.event.c_str(), msub, kAccent);
        }
        VSpace(6);
        if (BeginCard("##hd_score", kBgDeep, 16.0f, 12.0f)) {
            ImGui::PushFont(g_font_h2);
            ImGui::Text("%-20s   %s   %-20s",
                        m.blue_name.c_str(), m.score.c_str(), m.red_name.c_str());
            ImGui::PopFont();
            ThinDivider(6);
            MutedText("Match MVP: %s", m.mvp_name.c_str());
            MutedText("Rounds played: %d   |   Events recorded: %zu",
                      m.blue_score + m.red_score,
                      m.round_history ? m.round_history->size() : 0);
        } EndCard();
        VSpace(8);

        ImGui::PushStyleColor(ImGuiCol_Button, toV4(kVlrRed));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(kVlrRedDim));
        if (ImGui::Button("WATCH ROUND-BY-ROUND", ImVec2(260, 36))) {
            s.show_history_detail_modal = false;
            ImGui::CloseCurrentPopup();
            OpenReplay(s, s.history_detail);
        }
        ImGui::PopStyleColor(2);

        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(120, 36))) {
            s.show_history_detail_modal = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

}  // namespace

// ============================================================
// 6/8. Live Match Viewer
// ============================================================
namespace {
#include "gui_match.inc"
}  // namespace

// ============================================================
// 9. Screens
// ============================================================
namespace {

// === Shared negotiation modal — Re-sign + FA Sign =========================
// Both flows are ~250 lines of near-identical UI: cached offer, sliders,
// role offer combo, transparent breakdown, action buttons. Only the
// header pills, slider amt_hi multiplier (1.5x for re-sign, 1.6x for FA),
// and the commit path differ. Extracted here so future UX tweaks land in
// ONE place — and so any divergence between the two flows is a single
// `if (mode == ...)` branch instead of a hunt across the file.
//
// The caller owns:
//   - which `target` player to negotiate over
//   - which `team` the offer is for
//   - the persisted state (years_choice, amount_choice, role_choice,
//     cached_offer, cached_mkt_k, cached_for, show_modal)
//   - the popup id string (so multiple modals can't collide)
//
// The helper only writes through the references; it never owns state.
enum class NegotiationMode { Resign, FreeAgentSign };

void DrawNegotiationModal(
    AppState& s,
    NegotiationMode mode,
    const char* popup_id,
    vlr::PlayerPtr& target,
    vlr::TeamPtr team,
    int& years_choice,
    int& amount_choice,
    int& role_choice,
    vlr::Player::ResignOffer& cached_offer,
    int& cached_mkt_k,
    vlr::Player*& cached_for,
    bool& show_modal,
    int& bonus_choice,
    bool& promise_starter,
    int& offers_left)
{
    if (!show_modal || !target || !team) return;
    auto& gm = s.gm;
    auto p = target;

    ImGui::OpenPopup(popup_id);
    ImVec2 rc = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(rc, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(860, 560), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(popup_id, &show_modal,
                               ImGuiWindowFlags_NoSavedSettings)) {
        // === Cache offer + market value on first frame for this target =====
        // `propose_resign_offer` -> `gen_contract` uses randomize_amount,
        // so recomputing each frame jitters slider bounds. Snapshot once.
        if (cached_for != p.get()) {
            cached_offer = p->propose_resign_offer(*team, gm.year);
            cached_mkt_k = team->market_value_estimate(*p);
            cached_for   = p.get();
            if (years_choice  <= 0) years_choice  = cached_offer.years;
            if (amount_choice <= 0) amount_choice = cached_offer.amount_k;
            bonus_choice    = 0;        // reset multi-year + retry state per target
            promise_starter = false;
            offers_left     = 3;
        }
        const auto& offer = cached_offer;
        int mkt_k = cached_mkt_k;

        // Slider bounds. Re-sign caps at 1.5x ask; FA at 1.6x (FA flows
        // were tuned slightly higher to give more overpay headroom).
        int years_lo = 1;
        int years_hi = std::max(1, offer.max_acceptable_years);
        int amt_lo   = std::max((int)vlr::kSalaryFloorK,
                                offer.min_acceptable_k);
        double overpay_mult = (mode == NegotiationMode::Resign) ? 1.5 : 1.6;
        int amt_hi   = std::max(amt_lo + 1,
                                (int)std::min<long long>(
                                    vlr::kSalaryCapK,
                                    (long long)std::lround(
                                        offer.amount_k * overpay_mult)));
        years_choice  = vlr::clamp_v(years_choice,  years_lo, years_hi);
        amount_choice = vlr::clamp_v(amount_choice, amt_lo,   amt_hi);

        // === Header ========================================================
        {
            const char* verb = (mode == NegotiationMode::Resign)
                                ? "RE-SIGN " : "SIGN ";
            std::string title = std::string(verb) + p->name;
            H2(title.c_str());
        }
        // Mode-specific status pills.
        if (mode == NegotiationMode::Resign) {
            std::string sig = p->signature_agent();
            if (!sig.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                ImGui::Text("[*] Signature %s", sig.c_str());
                ImGui::PopStyleColor();
            }
            char exp_buf[32];
            std::snprintf(exp_buf, sizeof(exp_buf),
                          "Expires %d", p->contract.exp_year);
            Pill(exp_buf, kVlrSub);
            ImGui::SameLine();
            ImU32 wcol = kAccent;
            const char* wname = "Open";
            switch (team->window) {
                case vlr::TeamWindow::Opening:
                    wcol = kVlrGreen;  wname = "Opening"; break;
                case vlr::TeamWindow::Open:
                    wcol = kAccent;    wname = "Open";    break;
                case vlr::TeamWindow::Closing:
                    wcol = kVlrGold;   wname = "Closing"; break;
                case vlr::TeamWindow::Closed:
                    wcol = kVlrRedDim; wname = "Closed";  break;
            }
            Pill(wname, wcol);
        } else {
            char tag[80];
            std::snprintf(tag, sizeof(tag),
                "%d | %s | %s | OVR %.0f",
                p->age, vlr::role_name(p->primary_role),
                p->region.c_str(), p->ovr());
            Pill(tag, kVlrSub);
            if (p->region != team->region) {
                ImGui::SameLine();
                Pill("IMPORT", kVlrGold);
            }
        }
        ThinDivider(6);

        // === Two-column body ===============================================
        ImGui::Columns(2, "##neg_cols", false);

        // LEFT: player's ask
        SectionHeader("PLAYER IS ASKING", nullptr, kVlrGold);
        ImGui::Text("Years requested: %d", offer.years);
        ImGui::Text("Amount requested: $%dK/yr", offer.amount_k);
        {
            float w01 = (float)vlr::clamp_v(offer.willingness, 0.0, 1.0);
            ImU32 wcol = (w01 >= 0.70f) ? kVlrGreen
                       : (w01 >= 0.40f) ? kVlrGold : kVlrRed;
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, toV4(wcol));
            char wbuf[32];
            std::snprintf(wbuf, sizeof(wbuf), "%.0f%% willing",
                          w01 * 100.0f);
            ImGui::ProgressBar(w01, ImVec2(-FLT_MIN, 0), wbuf);
            ImGui::PopStyleColor();
        }
        if (!offer.explainer.empty()) MutedText("%s", offer.explainer.c_str());
        VSpace(6);
        ImGui::Text("Min acceptable: $%dK/yr", offer.min_acceptable_k);
        ImGui::Text("Max acceptable years: %d", offer.max_acceptable_years);
        if (mode == NegotiationMode::FreeAgentSign) {
            VSpace(4);
            MutedText("Preferred role: %s", vlr::role_name(p->primary_role));
            // Competing suitors — rival orgs that could realistically land this
            // FA (positional need + budget). You're not the only bidder; the
            // offseason market (B8) can poach them if you wait too long.
            int suitors = 0;
            int est = team->market_value_estimate(*p);
            for (auto& kv : gm.leagues) {
                if (!kv.second) continue;
                for (auto& rt : kv.second->teams()) {
                    if (!rt || rt == team || rt->roster.size() >= 7) continue;
                    if (rt->budget < static_cast<long long>(est) * 1000LL) continue;
                    int same_role = 0;
                    for (auto& rp : rt->roster)
                        if (rp && rp->primary_role == p->primary_role) ++same_role;
                    if (same_role <= 1) ++suitors;
                }
            }
            suitors = std::min(suitors, 12);
            if (suitors > 0) {
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                ImGui::Text("Other interest: %d team%s", suitors, suitors == 1 ? "" : "s");
                ImGui::PopStyleColor();
            }
        }
        // Personality readout — who you're dealing with. High greed/ego = pricey
        // and touchy; high loyalty = easier to keep; high ambition = wants a winner.
        VSpace(6);
        if (g_font_small) ImGui::PushFont(g_font_small);
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
        ImGui::TextUnformatted("PERSONALITY");
        ImGui::PopStyleColor();
        auto trait_line = [&](const char* nm, int v, ImU32 hi) {
            ImU32 c = (v >= 68) ? hi : (v <= 32) ? kVlrSub : kVlrText;
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(c));
            ImGui::Text("  %-9s %d", nm, v);
            ImGui::PopStyleColor();
        };
        trait_line("Ambition", p->ambition, kAccent);
        trait_line("Loyalty",  p->loyalty,  kVlrGreen);
        trait_line("Greed",    p->greed,    kVlrGold);
        trait_line("Ego",      p->ego,      kVlrRed);
        if (g_font_small) ImGui::PopFont();

        // RIGHT: market + your offer
        ImGui::NextColumn();
        SectionHeader("MARKET & YOUR OFFER", nullptr, kAccent);
        ImGui::Text("Market value: $%dK/yr", mkt_k);
        ImGui::Text("Team budget: %s", fmt_money(team->budget).c_str());
        VSpace(4);

        ImGui::SetNextItemWidth(220);
        ImGui::SliderInt("Years", &years_choice, years_lo, years_hi);
        ImGui::SetNextItemWidth(220);
        ImGui::SliderInt("Salary $K/yr", &amount_choice, amt_lo, amt_hi);
        // Multi-year structure: a one-time signing bonus (paid up-front from
        // budget on signing) and a guaranteed starter slot. Both sweeten the
        // deal; the bonus is amortized in the player's valuation. Cap the bonus
        // at ~1 year of the offered salary.
        ImGui::SetNextItemWidth(220);
        int bonus_hi = std::max(0, amount_choice);
        ImGui::SliderInt("Signing bonus $K", &bonus_choice, 0, bonus_hi);
        bonus_choice = vlr::clamp_v(bonus_choice, 0, bonus_hi);
        ImGui::Checkbox("Guarantee starter slot", &promise_starter);

        // Role offer combo — natural-role default; off-role offers take a
        // role_fit_score-derived penalty in evaluate_resign_offer.
        const vlr::Role role_opts[4] = {
            vlr::Role::Duelist, vlr::Role::Initiator,
            vlr::Role::Controller, vlr::Role::Sentinel
        };
        int natural_idx = 0;
        for (int i = 0; i < 4; ++i)
            if (role_opts[i] == p->primary_role) natural_idx = i;
        int cur_idx = (role_choice < 0) ? natural_idx : role_choice;
        if (cur_idx < 0 || cur_idx > 3) cur_idx = natural_idx;
        ImGui::SetNextItemWidth(220);
        char preview[80];
        std::snprintf(preview, sizeof(preview), "%s%s",
            vlr::role_name(role_opts[cur_idx]),
            (cur_idx == natural_idx) ? " (Natural)" : "");
        if (ImGui::BeginCombo("Offered Role", preview)) {
            for (int i = 0; i < 4; ++i) {
                char item[96];
                std::snprintf(item, sizeof(item), "%s  -  %s",
                    vlr::role_name(role_opts[i]),
                    p->role_fit_verdict(role_opts[i]));
                bool selected = (i == cur_idx);
                if (ImGui::Selectable(item, selected)) {
                    role_choice = i;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        vlr::Role offered_role =
            (role_choice >= 0 && role_choice < 4)
                ? role_opts[role_choice]
                : p->primary_role;

        // Transparent breakdown — same evaluator the engine uses to decide
        // (6-arg: includes the signing bonus + starter promise sweeteners).
        auto bd = p->evaluate_resign_offer(amount_choice, years_choice,
                                           *team, offered_role,
                                           bonus_choice, promise_starter);
        // Offer-vs-ask quick read — are you over/under the player's price?
        if (bd.ask_k > 0) {
            int dpct = (int)std::lround((amount_choice - bd.ask_k) * 100.0 / bd.ask_k);
            ImU32 oc = (dpct >= 0) ? kVlrGreen : (dpct >= -10) ? kVlrGold : kVlrRed;
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(oc));
            ImGui::Text("Your offer vs ask ($%dK): %+d%%", bd.ask_k, dpct);
            ImGui::PopStyleColor();
        }

        ImU32 verdict_col = kVlrSub;
        if      (bd.total >= 85) verdict_col = kVlrGold;
        else if (bd.total >= 70) verdict_col = kVlrGreen;
        else if (bd.total >= 55) verdict_col = kAccent;
        else if (bd.total >= 35) verdict_col = kRoleFlex;
        else                     verdict_col = kVlrRed;
        char vbuf[80];
        std::snprintf(vbuf, sizeof(vbuf), "%s  (%d/100)", bd.verdict, bd.total);
        Pill(vbuf, verdict_col);
        ImGui::SameLine();
        if (bd.will_accept) Pill("WILL ACCEPT", kVlrGreen);
        else                Pill("WILL REJECT", kVlrRed);

        VSpace(4);
        if (g_font_small) ImGui::PushFont(g_font_small);
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
        ImGui::Text("Base interest: %d", bd.base_score);
        ImGui::PopStyleColor();
        for (auto& kv : bd.labels) {
            ImU32 col = (kv.second > 0) ? kVlrGreen
                      : (kv.second < 0) ? kVlrRed : kVlrSub;
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(col));
            ImGui::Text("  %s: %+d", kv.first.c_str(), kv.second);
            ImGui::PopStyleColor();
        }
        if (g_font_small) ImGui::PopFont();
        if (!bd.will_accept && !bd.reject_reason.empty()) {
            VSpace(4);
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRedDim));
            ImGui::TextWrapped("Reason: %s", bd.reject_reason.c_str());
            ImGui::PopStyleColor();
        }
        // Counter-offer card — a near-miss returns the minimum terms the player
        // WOULD sign. One click snaps the sliders to those terms.
        if (!bd.will_accept && bd.has_counter) {
            VSpace(4);
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
            ImGui::Text("Player counters: $%dK/yr for %dY",
                        bd.counter_amount_k, bd.counter_years);
            ImGui::PopStyleColor();
            if (ImGui::SmallButton("Accept Counter")) {
                amount_choice = bd.counter_amount_k;
                years_choice  = bd.counter_years;
            }
        }

        ImGui::Columns(1);
        ThinDivider(8);

        // === Action buttons ================================================
        // Reject-cost: too many lowball offers and the player walks from talks
        // this session (Send locks). You can still let the offseason AI bidding
        // take the FA, or re-open the modal next session once their mood decays.
        bool walked = (offers_left <= 0);
        if (walked) {
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
            ImGui::TextUnformatted("Player has walked away from talks (too many lowball offers).");
            ImGui::PopStyleColor();
        } else if (offers_left <= 2) {
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
            ImGui::Text("Patience: %d offer%s left before they walk.",
                        offers_left, offers_left == 1 ? "" : "s");
            ImGui::PopStyleColor();
        }
        ImGui::PushStyleColor(ImGuiCol_Button,        toV4(kAccent));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(kAccent2));
        ImGui::BeginDisabled(walked);
        if (ImGui::Button("Send Offer", ImVec2(150, 32))) {
            int yrs = years_choice;
            int amt = amount_choice;
            bool ok = bd.will_accept;
            if (mode == NegotiationMode::Resign) {
                if (ok) {
                    // A new deal's first season is the next NOT-yet-started
                    // season: in OFFSEASON gm.year is already the upcoming
                    // season, so the deal starts there; in-season the current
                    // season is in progress, so the deal starts gm.year+1.
                    // This matches the AI re-sign convention (year+1 at the
                    // year-end boundary) and stops a re-sign from landing one
                    // year short.
                    int base_yr = (gm.current_phase() == "OFFSEASON")
                                      ? gm.year : gm.year + 1;
                    bool committed = team->resign_player(p, yrs, amt,
                                                         base_yr, offered_role);
                    if (committed) {
                        p->contract.signing_bonus_k  = bonus_choice;
                        p->contract.promised_starter = promise_starter;
                        p->contract.promise_active   = promise_starter;
                        if (bonus_choice > 0)
                            team->budget -= static_cast<long long>(bonus_choice) * 1000LL;
                        char lb[176];
                        std::snprintf(lb, sizeof(lb),
                            "Re-signed %s for %dY at $%dK/yr%s.",
                            p->name.c_str(), yrs, amt,
                            bonus_choice > 0 ? " (+bonus)" : "");
                        s.log.emplace_back(lb);
                        show_modal = false;
                        target.reset();
                    } else {
                        char lb[160];
                        std::snprintf(lb, sizeof(lb),
                            "Re-sign of %s failed (budget / cap).",
                            p->name.c_str());
                        s.log.emplace_back(lb);
                    }
                } else {
                    // Genuine player refusal -> reject cost (mood + offers-left).
                    int tier = p->register_rejected_offer(amt, yrs, *team);
                    offers_left -= 1;
                    char lb[176];
                    std::snprintf(lb, sizeof(lb),
                        "%s refused $%dK/y for %dY%s.",
                        p->name.c_str(), amt, yrs,
                        tier == 2 ? " — insulted!" : tier == 1 ? " (lowball)" : "");
                    s.log.emplace_back(lb);
                }
            } else {
                // FreeAgentSign path — pre-flight + sign_player.
                long long cost = static_cast<long long>(amt) * 1000LL;
                std::string reject;
                if (ok && !s.gm.transfers_open()) {   // WS-A INC-4: window gate (FA only, not re-signs)
                    ok = false; reject = "The transfer window is closed.";
                }
                if (ok && team->budget < cost) {
                    ok = false;
                    reject = "Insufficient budget ($" +
                             std::to_string(team->budget / 1000) +
                             "K available, $" + std::to_string(amt) + "K offered).";
                }
                if (ok && p->region != team->region) {
                    int imports = 0;
                    for (auto& rp : team->roster)
                        if (rp && rp->region != team->region) ++imports;
                    if (imports >= vlr::config().max_imports) {
                        ok = false;
                        reject = "Import cap reached (" +
                                 std::to_string(vlr::config().max_imports) + ").";
                    }
                }
                if (ok && team->roster.size() >= 7) {
                    ok = false; reject = "Roster full.";
                }
                if (!ok) {
                    if (!bd.will_accept) {   // player refusal (not a budget/cap block) -> reject cost
                        int tier = p->register_rejected_offer(amt, yrs, *team);
                        offers_left -= 1;
                        (void)tier;
                    }
                    s.log.emplace_back(p->name + ": " +
                        (reject.empty() ? std::string("Offer rejected.") : reject));
                } else {
                    std::size_t before_size = team->roster.size();
                    team->budget -= cost;
                    p->contract.amount_k = amt;
                    int base_yr = (gm.current_phase() == "OFFSEASON")
                                      ? gm.year : gm.year + 1;
                    team->sign_player(p, yrs, base_yr, offered_role);
                    bool added = (team->roster.size() == before_size + 1)
                              && (p->team_name == team->name);
                    if (!added) {
                        team->budget += cost;
                        s.log.emplace_back("Engine rejected sign of " + p->name + ".");
                    } else {
                        p->contract.signing_bonus_k  = bonus_choice;
                        p->contract.promised_starter = promise_starter;
                        p->contract.promise_active   = promise_starter;
                        if (bonus_choice > 0)
                            team->budget -= static_cast<long long>(bonus_choice) * 1000LL;
                        gm.sync_player_ranked_region(p, team->region);
                        char msg[256];
                        std::snprintf(msg, sizeof(msg),
                            "Signed %s as %s (%dy / $%dK/yr%s).",
                            p->name.c_str(),
                            vlr::role_name(offered_role), yrs, amt,
                            bonus_choice > 0 ? " +bonus" : "");
                        s.log.emplace_back(msg);
                        show_modal = false;
                        target.reset();
                    }
                }
            }
        }
        ImGui::EndDisabled();
        ImGui::PopStyleColor(2);
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button,        toV4(kVlrGold));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(kVlrGold));
        if (ImGui::Button("Match Their Ask", ImVec2(170, 32))) {
            years_choice  = vlr::clamp_v(offer.years, years_lo, years_hi);
            amount_choice = vlr::clamp_v(offer.amount_k, amt_lo, amt_hi);
        }
        ImGui::PopStyleColor(2);
        ImGui::SameLine();

        if (ImGui::Button("Walk Away", ImVec2(120, 32))) {
            show_modal = false;
            target.reset();
        }

        ImGui::EndPopup();
    }
    // Defensive cleanup if user dismissed via [x]
    if (!show_modal) {
        target.reset();
        cached_for = nullptr;
    }
}

// === Strategy tab — full per-map agent composition =========================
// Lets the user build the exact 5-agent comp their team runs on each map.
// Stored in Team::agent_override (5 agent names per map) and honoured by
// Team::build_round_selection for the user team's live-sim matches. A map with
// no override stays on "Auto" (the coaching staff pick).
void DrawStrategy(AppState& s) {
    auto& t = s.gm.user_team;
    if (!t) { ImGui::Text("No user team."); return; }

    ImGui::PushFont(g_font_h1);
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
    ImGui::TextUnformatted("MAP STRATEGY");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    MutedText("Build the exact agent composition your team runs on each map. Pick 5 agents "
              "per map and your starters are assigned to them by best fit. \"Auto\" lets the "
              "coaching staff choose. Applies to your team's live-sim matches. "
              "(Not saved between runs yet.)");
    VSpace(10);

    // === WS-B: Club Philosophy chooser (B1) ==============================
    if (BeginCard("##club_philosophy")) {
        SectionHeader("CLUB PHILOSOPHY",
            "Your club's identity \xE2\x80\x94 biases your AI auto-decisions + a small "
            "bounded match tilt. 'Emergent' derives it from your roster + coach.",
            kAccent);
        const char* phils[] = {
            "Emergent (derive from my club)",
            "Aggressive Academy",
            "Tactical / Methodical",
            "Win-Now Spender",
            "Defensive / Structured",
            "Balanced & Flexible"
        };
        int cur = static_cast<int>(s.gm.user_philosophy);
        ImGui::SetNextItemWidth(320);
        if (ImGui::Combo("##club_phil", &cur, phils, IM_ARRAYSIZE(phils))) {
            s.gm.user_philosophy = static_cast<vlr::GameManager::ClubPhilosophy>(cur);
            t->identity = (s.gm.user_philosophy != vlr::GameManager::ClubPhilosophy::Emergent)
                        ? vlr::GameManager::philosophy_to_identity(s.gm.user_philosophy, *t)
                        : vlr::compute_team_identity(*t);
        }
        ImGui::SameLine();
        Pill(vlr::brand_tag_name(t->identity.brand), kVlrGold);
        MutedText("Now: %s \xE2\x80\x94 %s",
                  vlr::brand_tag_name(t->identity.brand),
                  t->identity.user_chosen ? "chosen" : "emergent");
    } EndCard();
    VSpace(10);

    // === NEXT OPPONENT — analyst-scaled opposition report (Match-Prep B) =======
    {
        vlr::TeamPtr opp; std::string flabel; int days_until = -1;
        for (auto& f : s.gm.upcoming_fixtures(14)) {
            if (f.a == t || f.b == t) {
                opp = (f.a == t) ? f.b : f.a;
                flabel = f.label;
                days_until = f.day_in_year - s.gm.day_in_year;
                break;
            }
        }
        if (opp) {
            auto rep = s.gm.scout_opposition(*opp);
            if (BeginCard("##next_opp")) {
                SectionHeader("NEXT OPPONENT", flabel.empty() ? nullptr : flabel.c_str(), kVlrGold);
                ImGui::PushFont(g_font_h2);
                ImGui::TextUnformatted(rep.opp_name.c_str());
                ImGui::PopFont();
                ImGui::SameLine();
                if (days_until >= 0) MutedText("  %s  \xC2\xB7  in %dd", rep.opp_region.c_str(), days_until);
                else                 MutedText("  %s", rep.opp_region.c_str());

                if (!rep.has_analyst) {
                    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
                    ImGui::TextWrapped("No analyst on staff \xE2\x80\x94 basic report only. Hire one "
                        "(Manager > Head Analyst) to reveal key players, tendencies + a counter.");
                    ImGui::PopStyleColor();
                } else {
                    ImGui::PushFont(g_font_small);
                    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                    ImGui::Text("Analyst report \xE2\x80\x94 %d/5 sections  \xC2\xB7  +%.0f%% read sharpness",
                                rep.detail_level, rep.accuracy * 100.0);
                    ImGui::PopStyleColor();
                    ImGui::PopFont();
                }
                VSpace(4);
                // Section 1 (always)
                ImGui::Text("Est. roster OVR:  ~%d  (\xC2\xB1%d)", rep.opp_ovr_est, rep.ovr_band);
                ImGui::Text("Recent form:      %s", rep.form.c_str());
                // Section 2
                if (!rep.key_players.empty()) {
                    VSpace(2);
                    Pill("KEY PLAYERS", kAccent2);
                    for (auto& kp : rep.key_players) ImGui::BulletText("%s", kp.c_str());
                }
                // Section 3
                if (!rep.comp_tendency.empty())
                    ImGui::Text("Comp tendency:    %s", rep.comp_tendency.c_str());
                // Section 4
                if (!rep.role_read.empty())
                    ImGui::Text("Role read:        %s", rep.role_read.c_str());
                // Section 5
                if (!rep.recommendation.empty()) {
                    VSpace(2);
                    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                    ImGui::TextWrapped("Coach's note: %s", rep.recommendation.c_str());
                    ImGui::PopStyleColor();
                }
            }
            EndCard();
            VSpace(10);
        }
    }

    if (t->roster.size() < 5) {
        ImGui::TextDisabled("You need a full starting 5 before setting a map strategy.");
        return;
    }

    const auto& M = vlr::maps();
    static int sel_map = 0;
    if (sel_map < 0 || sel_map >= (int)M.size()) sel_map = 0;

    ImGui::Columns(2, "##strat_cols", false);
    ImGui::SetColumnWidth(0, 230);

    // ---- Left pane: map list (green name = customised) ----
    if (BeginCard("##strat_maps")) {
        SectionHeader("MAPS", nullptr, kAccent);
        for (int i = 0; i < (int)M.size(); ++i) {
            bool custom = t->agent_override.count(M[i].name) > 0;
            ImGui::PushID(i);
            if (custom) ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGreen));
            std::string label = M[i].name + (custom ? "  *" : "");
            if (ImGui::Selectable(label.c_str(), i == sel_map)) sel_map = i;
            if (custom) ImGui::PopStyleColor();
            ImGui::PopID();
        }
        VSpace(6);
        ImGui::TextDisabled("* = custom comp");
        VSpace(4);
        if (ImGui::SmallButton("All maps -> Auto")) t->agent_override.clear();
    } EndCard();

    ImGui::NextColumn();

    // ---- Right pane: agent comp editor for the selected map ----
    const auto& gmap = M[sel_map];
    if (BeginCard("##strat_editor")) {
        char hdr[96];
        std::snprintf(hdr, sizeof(hdr), "COMPOSITION  \xC2\xB7  %s", gmap.name.c_str());
        SectionHeader(hdr, nullptr, kVlrGold);

        // === MATCH PREP (increment E) — bounded, pure-upside per-map edge ===
        {
            int lvl = 0;
            auto pit = t->map_prep.find(gmap.name);
            if (pit != t->map_prep.end()) lvl = pit->second.level;
            if (lvl < 0 || lvl > 2) lvl = 0;
            const char* levels[] = { "None", "Standard", "Heavy" };
            ImGui::TextUnformatted("Match prep:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(150);
            if (ImGui::Combo("##map_prep", &lvl, levels, 3)) {
                if (lvl <= 0) t->map_prep.erase(gmap.name);
                else          t->map_prep[gmap.name].level = lvl;
            }
            double aq = t->head_analyst ? t->head_analyst->quality01() : 0.0;
            double edge = (lvl > 0) ? 0.015 * lvl * (0.5 + 0.5 * aq) : 0.0;
            if (edge > 0.03) edge = 0.03;
            ImGui::PushFont(g_font_small);
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(lvl > 0 ? kVlrGold : kVlrSub));
            if (lvl > 0)
                ImGui::Text("  prepped \xE2\x80\x94 ~+%.1f%% edge on this map%s",
                            edge * 100.0,
                            t->head_analyst ? " (scaled by your analyst)"
                                            : " \xE2\x80\x94 hire an analyst to sharpen it");
            else
                ImGui::TextUnformatted("  no prep edge on this map");
            ImGui::PopStyleColor();
            ImGui::PopFont();
            VSpace(8);
        }

        auto oit = t->agent_override.find(gmap.name);
        bool manual = (oit != t->agent_override.end() && oit->second.size() == 5);

        if (!manual) {
            MutedText("This map is on AUTO — the coaching staff pick the comp based on the "
                      "map, current form, and the opponent.");
            VSpace(10);
            if (ImGui::Button("Customize this map", ImVec2(190, 32))) {
                // Seed from the engine's current auto pick so the user edits
                // from a sensible baseline, padded to 5 distinct agents.
                auto rt = t->build_round_selection(gmap, true, nullptr, false);
                std::vector<std::string> seed;
                std::unordered_set<std::string> seen;
                for (int i = 0; i < 5 && i < (int)t->roster.size(); ++i) {
                    auto* p = t->roster[i].get();
                    auto ait = rt.chosen_agents.find(p);
                    if (ait != rt.chosen_agents.end() && ait->second &&
                        !seen.count(ait->second->name)) {
                        seed.push_back(ait->second->name);
                        seen.insert(ait->second->name);
                    }
                }
                for (auto& a : vlr::agents()) {
                    if (seed.size() >= 5) break;
                    if (!seen.count(a.name)) { seed.push_back(a.name); seen.insert(a.name); }
                }
                seed.resize(5);
                t->agent_override[gmap.name] = seed;
            }
        } else {
            auto& ov = oit->second;
            // Role-grouped agent combo that hides agents used in other slots.
            auto agent_combo = [&](const char* id, std::string& cur,
                                   const std::unordered_set<std::string>& taken) {
                const vlr::Agent* curA = vlr::find_agent_by_name(cur);
                std::string preview = curA
                    ? (cur + "   " + vlr::role_name(curA->role))
                    : std::string("(pick agent)");
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo(id, preview.c_str())) {
                    for (int r = 0; r < (int)vlr::Role::Count; ++r) {
                        ImGui::TextDisabled("%s", vlr::role_name(static_cast<vlr::Role>(r)));
                        for (auto& a : vlr::agents()) {
                            if (static_cast<int>(a.role) != r) continue;
                            bool issel = (a.name == cur);
                            if (taken.count(a.name) && !issel) continue;
                            if (ImGui::Selectable(("   " + a.name).c_str(), issel))
                                cur = a.name;
                        }
                    }
                    ImGui::EndCombo();
                }
            };

            for (int slot = 0; slot < 5; ++slot) {
                std::unordered_set<std::string> others;
                for (int j = 0; j < 5; ++j) if (j != slot) others.insert(ov[j]);
                ImGui::PushID(slot);
                ImGui::AlignTextToFramePadding();
                ImGui::Text("Agent %d", slot + 1);
                ImGui::SameLine(82);
                agent_combo("##ag", ov[slot], others);
                ImGui::PopID();
            }

            VSpace(8);
            // Comp summary + legality badge.
            int rc[4] = {0, 0, 0, 0};
            for (auto& nm : ov) {
                const vlr::Agent* a = vlr::find_agent_by_name(nm);
                if (a) { int ri = (int)a->role; if (ri >= 0 && ri < 4) ++rc[ri]; }
            }
            int doubled = -1, ones = 0; bool legal = true;
            for (int i = 0; i < 4; ++i) {
                if (rc[i] == 2) { if (doubled >= 0) legal = false; doubled = i; }
                else if (rc[i] == 1) ++ones;
                else legal = false;
            }
            legal = legal && (doubled >= 0) && (ones == 3);
            const char* dn[4] = {"Double Duelist", "Double Initiator",
                                 "Double Controller", "Double Sentinel"};
            char sum[80];
            std::snprintf(sum, sizeof(sum), "Comp:  %dD %dI %dC %dS",
                          rc[0], rc[1], rc[2], rc[3]);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(sum);
            ImGui::SameLine();
            if (legal && doubled >= 0) Pill(dn[doubled], kVlrGreen);
            else                       Pill("Off-meta", kAccent2);

            VSpace(10);
            SectionHeader("BEST-FIT ASSIGNMENT", nullptr, kAccent);
            auto rt = t->build_round_selection(gmap, true, nullptr, false);
            for (int i = 0; i < 5 && i < (int)t->roster.size(); ++i) {
                auto& p = t->roster[i];
                if (!p) continue;
                auto ait = rt.chosen_agents.find(p.get());
                const vlr::Agent* a = (ait != rt.chosen_agents.end()) ? ait->second : nullptr;
                bool offrole = a && (a->role != p->primary_role);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(offrole ? kVlrRedDim : kVlrText));
                ImGui::Text("%-16s ->  %s%s", p->name.c_str(),
                            a ? a->name.c_str() : "?",
                            offrole ? "   (off-role)" : "");
                ImGui::PopStyleColor();
            }

            VSpace(10);
            if (ImGui::SmallButton("Reset this map to Auto"))
                t->agent_override.erase(gmap.name);
        }
    } EndCard();

    ImGui::Columns(1);
}

void DrawDashboard(AppState& s) {
    auto& t = s.gm.user_team;
    if (!t) { ImGui::Text("No user team."); return; }

    int next_match = s.gm.next_user_match_day_in_year();
    int days_until = (next_match >= 0) ? (next_match - s.gm.day_in_year) : -1;
    // Keep the value SHORT so it fits the fixed-width StatTile (big KPI
    // font, no wrapping inside the tile). Long phrasing overflowed the card.
    std::string match_blurb;
    if (days_until == 0) match_blurb = "TODAY";
    else if (days_until > 0) match_blurb = "in " + std::to_string(days_until) + "d";
    else match_blurb = "None";

    // === Title block =======================================================
    ImGui::PushFont(g_font_h1);
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
    ImGui::TextUnformatted(t->name.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    // Use the team's ACTUAL division (tier-aware) so a relegated user team
    // shows the right label + ranks against the right league.
    auto league = s.gm.league_for(t);
    if (!league) {
        auto _lit = s.gm.leagues.find(t->region);
        if (_lit != s.gm.leagues.end()) league = _lit->second;
    }
    const char* div = (league && !league->division_name().empty())
                      ? league->division_name().c_str() : "VCT";
    MutedText("%s %s   |   Year %d   |   Day %d/%d   |   %s",
        div, t->region.c_str(), s.gm.year, s.gm.day_in_year + 1,
        vlr::GameManager::kDaysPerYear, s.gm.current_phase().c_str());
    VSpace(6);

    // === Board objectives (Group E) — the season's goals, live progress ====
    {
        const auto& objs = s.gm.board_objectives();
        if (!objs.empty()) {
            if (BeginCard("##dash_objectives")) {
                int met = 0;
                for (auto& o : objs) if (s.gm.evaluate_objective(o).met) ++met;
                char hdr[64];
                std::snprintf(hdr, sizeof(hdr), "%d of %d met", met, (int)objs.size());
                SectionHeader("BOARD OBJECTIVES", hdr, kAccent);
                for (auto& o : objs) {
                    auto st = s.gm.evaluate_objective(o);
                    ImU32 col = st.met ? kVlrGreen : kVlrGold;
                    ImGui::PushStyleColor(ImGuiCol_Text, toV4(col));
                    ImGui::TextUnformatted(st.met ? "[x]" : "[ ]");
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    if (o.mandatory) { Pill("PRIMARY", kVlrRedDim); ImGui::SameLine(); }
                    ImGui::TextUnformatted(o.text.c_str());
                    ImGui::SameLine();
                    MutedText("\xE2\x80\x94 %s", st.progress.c_str());
                    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, toV4(col));
                    ImGui::ProgressBar(static_cast<float>(st.pct), ImVec2(-FLT_MIN, 6.0f), "");
                    ImGui::PopStyleColor();
                }
            } EndCard();
            VSpace(6);
        }
    }
    // === Preseason banner ==================================================
    // Prominent call-to-action while the engine sits in its preseason buffer:
    // points the user at the board expectations (Mail), sponsor pick, and
    // finalizing the starting five before opening day.
    if (s.gm.in_preseason_buffer_) {
        if (BeginCard("##dash_preseason")) {
            char hdr[96];
            std::snprintf(hdr, sizeof(hdr),
                          "PRESEASON \xE2\x80\x94 %d day%s to opening",
                          s.gm.preseason_days_left_,
                          s.gm.preseason_days_left_ == 1 ? "" : "s");
            SectionHeader(hdr,
                "Review board expectations (Mail), choose a sponsor, finalize "
                "your starting 5.",
                kAccent);
        }
        EndCard();
        VSpace(10);
    }
    // Auto-manage (simulate) toggle — when ON the AI GM runs the user's
    // year-end offseason exactly like every CPU team.
    ImGui::Checkbox("Auto-manage my team (simulate offseasons)",
                    &s.gm.user_auto_manage);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("When ON, the AI front office re-signs / cuts / signs for your\n"
                          "team at year-end like every CPU team — play out a whole\n"
                          "career without hands-on roster management.");
    VSpace(10);

    std::vector<vlr::TeamPtr> sorted_t;
    if (league) for (auto& tt : league->teams()) if (tt) sorted_t.push_back(tt);
    std::sort(sorted_t.begin(), sorted_t.end(),
              [](const vlr::TeamPtr& a, const vlr::TeamPtr& b) { return a->phase_wins > b->phase_wins; });
    int rank = 1;
    for (auto& tt : sorted_t) { if (tt == t) break; ++rank; }

    // === KPI strip =========================================================
    {
        char rec[32]; std::snprintf(rec, sizeof(rec), "%dW-%dL", t->phase_wins, t->phase_losses);
        char rk[16];  std::snprintf(rk, sizeof(rk), "#%d", rank);
        char ov[16];  std::snprintf(ov, sizeof(ov), "%.0f", t->ovr());
        char pay[24]; std::snprintf(pay, sizeof(pay), "$%dK", t->total_payroll_k());
        StatTile("BUDGET",        fmt_money(t->budget), kVlrGreen); ImGui::SameLine();
        StatTile("PHASE RECORD",  rec, kAccent);                    ImGui::SameLine();
        StatTile("REGIONAL RANK", rk,  placement_color(rank));      ImGui::SameLine();
        StatTile("ROSTER OVR",    ov,  kAccent);                    ImGui::SameLine();
        StatTile("NEXT MATCH",    match_blurb, days_until == 0 ? kVlrRed : kAccent2);
        // === C4: Org Memory glance — top metric tile =======================
        // Always shows the team's most-pronounced metric, even when all six
        // are small. Previously gated to |value| >= 25 which left new teams
        // staring at "building..." for an entire first season; the engine
        // metrics ARE moving day-by-day, the gate was just hiding them.
        ImGui::SameLine();
        {
            struct MemEntry { const char* label; int value; };
            MemEntry me[6] = {
                { "ROOKIE SUCCESS",       t->memory.rookie_success       },
                { "IMPORT SUCCESS",       t->memory.import_success       },
                { "VETERAN SUCCESS",      t->memory.veteran_success      },
                { "FIN. DISCIPLINE",      t->memory.financial_discipline },
                { "STABILITY CULTURE",    t->memory.stability_culture    },
                { "STAR DEPENDENCY",      t->memory.star_dependency      },
            };
            int best = 0, best_abs = std::abs(me[0].value);
            for (int i = 1; i < 6; ++i) {
                int a = std::abs(me[i].value);
                if (a > best_abs) { best_abs = a; best = i; }
            }
            int v = me[best].value;
            ImU32 col = (v >=  30) ? kVlrGreen
                      : (v >=  10) ? kAccent
                      : (v <= -30) ? kVlrRed
                      : (v <= -10) ? kRoleFlex
                      :              kVlrSub;
            char vb[16];
            std::snprintf(vb, sizeof(vb), "%+d", v);
            StatTile(me[best].label, vb, col);
        }
        // === Phase B (C3): Championship Window tile ========================
        // User-team window indicator — colour matches the Bio-tab pill
        // mapping (Opening=green, Open=blue, Closing=gold, Closed=red-dim).
        // Subline gives a one-word phase descriptor.
        ImGui::SameLine();
        {
            ImU32 wcol = kAccent;
            const char* wsub = "Contending";
            switch (t->window) {
                case vlr::TeamWindow::Opening:
                    wcol = kVlrGreen;  wsub = "Building";   break;
                case vlr::TeamWindow::Open:
                    wcol = kAccent;    wsub = "Contending"; break;
                case vlr::TeamWindow::Closing:
                    wcol = kVlrGold;   wsub = "Urgency";    break;
                case vlr::TeamWindow::Closed:
                    wcol = kVlrRedDim; wsub = "Rebuild";    break;
            }
            std::string wval = vlr::team_window_name(t->window);
            // Force upper-case display per spec ("OPENING" / "OPEN" / ...).
            for (auto& ch : wval) ch = (char)std::toupper((unsigned char)ch);
            StatTile("WINDOW", wval, wcol, ImVec2(0, 0), wsub);
        }
    }
    // C5: Re-sign hint chip — count user-roster players in their final
    // contract year and nudge the user toward the Re-sign flow if any
    // exist. Drawn under the KPI strip so it sits above the wrapped
    // next-match line.
    {
        int expiring = 0;
        for (auto& p : t->roster) {
            if (p && p->years_left(s.gm.year) == 1) ++expiring;
        }
        if (expiring > 0) {
            char ebuf[48];
            std::snprintf(ebuf, sizeof(ebuf),
                          "%d player%s expiring", expiring,
                          expiring == 1 ? "" : "s");
            Pill(ebuf, kVlrGold);
            ImGui::SameLine();
            MutedText("\xE2\x80\x94 use Re-sign on My Roster.");
        }
    }
    // Full next-match description as a wrapped, width-responsive line so the
    // long phrasing is preserved without overflowing any fixed tile.
    {
        std::string full = (days_until == 0)
            ? "Match today \xE2\x80\x94 good luck."
            : (days_until > 0
                   ? "Next match in " + std::to_string(days_until) + " day"
                         + (days_until == 1 ? "" : "s") + "."
                   : "No match currently upcoming on the schedule.");
        ImGui::PushFont(g_font_small);
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX()
                               + ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted(full.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
    VSpace(10);

    // === Org Memory strip — all 6 metrics, always ==========================
    // Compact horizontal row showing every memory metric the engine tracks,
    // not just the strongest one. Color-coded so users can see at a glance
    // which way the org's institutional learning is leaning. Full bar-grid
    // version with explanations lives in Team Profile -> Bio (§7946+).
    if (BeginCard("##dash_orgmem", kBgDeep)) {
    SectionHeader("ORG MEMORY",
                  "How the front office is learning year-over-year",
                  kAccent2);
    {
        struct MemPair { const char* label; int value; };
        MemPair pairs[6] = {
            { "Rookie",       t->memory.rookie_success       },
            { "Imports",      t->memory.import_success       },
            { "Veterans",     t->memory.veteran_success      },
            { "Discipline",   t->memory.financial_discipline },
            { "Stability",    t->memory.stability_culture    },
            { "Star Dep.",    t->memory.star_dependency      },
        };
        for (int i = 0; i < 6; ++i) {
            if (i > 0) ImGui::SameLine();
            int v = pairs[i].value;
            ImU32 col = (v >=  30) ? kVlrGreen
                      : (v >=  10) ? kAccent
                      : (v <= -30) ? kVlrRed
                      : (v <= -10) ? kRoleFlex
                      :              kVlrSub;
            char chip[48];
            std::snprintf(chip, sizeof(chip), "%s %+d",
                          pairs[i].label, v);
            PillOutline(chip, col, col);
        }
    }
    } EndCard();
    VSpace(14);

    // === Roster snapshot card ==============================================
    if (BeginCard("##dash_roster")) {
    SectionHeader("STARTERS", "Your active competitive five", kAccent);
    if (ImGui::BeginTable("##roster", 9,
            ImGuiTableFlags_RowBg |
            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY,
            ImVec2(-FLT_MIN, 240))) {
        ImGui::TableSetupColumn("FLAG");
        ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("ROLE");
        ImGui::TableSetupColumn("AGE");
        ImGui::TableSetupColumn("OVR");
        ImGui::TableSetupColumn("AVG RAT");
        ImGui::TableSetupColumn("KAST");
        ImGui::TableSetupColumn("CONTRACT");
        ImGui::TableSetupColumn("SALARY");
        ImGui::TableHeadersRow();
        int idx = 0;
        for (auto& p : t->roster) {
            if (!p) continue;
            ImGui::TableNextRow();
            ImGui::PushID(idx++);
            ImGui::TableNextColumn(); CountryFlag(p->country_iso);
            ImGui::TableNextColumn();
            if (TableRowSelectable(player_label(*p).c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
                OpenPlayerModal(s, p);
            }
            ImGui::PopID();
            ImGui::TableNextColumn();
            PositionBadge(vlr::position_of(*p));
            if (p->is_igl) { ImGui::SameLine(); IglBadge(); }
            ImGui::TableNextColumn(); ImGui::Text("%d", p->age);
            ImGui::TableNextColumn(); ImGui::Text("%.0f", p->ovr());
            ImGui::TableNextColumn(); ImGui::Text("%.2f", p->avg_match_rating());
            ImGui::TableNextColumn(); ImGui::Text("%.1f", p->kast());
            ImGui::TableNextColumn(); ImGui::Text("exp %d", p->contract.exp_year);
            ImGui::TableNextColumn(); ImGui::Text("$%dK", p->contract.amount_k);
        }
        ImGui::EndTable();
    }
    } EndCard();

    VSpace(14);

    // === Squad development card ===========================================
    // Long-term planning view: OVR vs POT, contract runway, and a
    // colour-coded development trajectory per player so the user can spot
    // future stars / developing rookies / aging vets / risky prospects at
    // a glance. POT shown directly (explicit user request) regardless of
    // God Mode. No internal scroll — the table flows inside the auto-
    // resizing card; the screen owns the page scroll.
    if (BeginCard("##dash_development")) {
    SectionHeader("SQUAD DEVELOPMENT",
                  "OVR / potential, contract runway & trajectory", kAccent2);
    // Reads the engine-stamped trajectory class (item 8 fix) — form-aware and
    // updated each progression tick, not a stale every-frame gap/age guess.
    auto traj_of = [](const vlr::PlayerPtr& pl)
        -> std::pair<const char*, ImU32> {
        return traj_display(pl->trajectory_class);
    };
    if (ImGui::BeginTable("##devtbl", 8,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
            ImGuiTableFlags_SizingFixedFit, ImVec2(-FLT_MIN, 0))) {
        ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("ROLE");
        ImGui::TableSetupColumn("AGE");
        ImGui::TableSetupColumn("OVR");
        ImGui::TableSetupColumn("POT");
        ImGui::TableSetupColumn("FORM");
        ImGui::TableSetupColumn("CONTRACT");
        ImGui::TableSetupColumn("TRAJECTORY");
        ImGui::TableHeadersRow();
        int didx = 0;
        for (auto& p : t->roster) {
            if (!p) continue;
            ImGui::TableNextRow();
            ImGui::PushID(10000 + didx++);
            ImGui::TableNextColumn();
            if (TableRowSelectable(player_label(*p).c_str(), false,
                                   ImGuiSelectableFlags_SpanAllColumns)) {
                OpenPlayerModal(s, p);
            }
            ImGui::TableNextColumn();
            PositionBadge(vlr::position_of(*p));
            ImGui::TableNextColumn(); ImGui::Text("%d", p->age);
            int ovr_i = (int)std::lround(p->ovr());
            ImGui::TableNextColumn(); ImGui::Text("%d", ovr_i);
            ImGui::TableNextColumn();
            ImU32 pc = (p->potential > ovr_i) ? kVlrGreen
                     : (p->potential < ovr_i ? kVlrRedDim : kVlrSub);
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(pc));
            ImGui::Text("%d", p->potential);
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            {
                double r = p->avg_match_rating();
                const char* fl = r >= 1.10 ? "Hot" :
                                 r >= 0.95 ? "Steady" : "Cold";
                ImU32 fc = r >= 1.10 ? kVlrGreen :
                           r >= 0.95 ? kVlrText  : kVlrRedDim;
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(fc));
                ImGui::Text("%s", fl);
                ImGui::PopStyleColor();
            }
            ImGui::TableNextColumn();
            ImGui::Text("%dY (exp %d)", p->years_left(s.gm.year),
                        p->contract.exp_year);
            ImGui::TableNextColumn();
            auto tr = traj_of(p);
            Pill(tr.first, tr.second);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    } EndCard();

    VSpace(14);
    if (t->head_coach) {
        if (BeginCard("##coach_card")) {
        SectionHeader("HEAD COACH", nullptr, kAccent2);
        ImGui::PushFont(g_font_h2);
        ImGui::TextUnformatted(t->head_coach->name.c_str());
        ImGui::PopFont();
        // Personality first — it's the most identity-defining signal.
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
        ImGui::Text("%s", vlr::coach_personality_name(t->head_coach->personality));
        ImGui::PopStyleColor();
        MutedText("Age %d   |   Experience %d   |   $%dK/yr",
                  t->head_coach->age, t->head_coach->experience,
                  t->head_coach->salary_k);
        VSpace(8);
        char tb[24]; std::snprintf(tb, sizeof(tb), "TAC %d", t->head_coach->tactical);
        char db[24]; std::snprintf(db, sizeof(db), "DEV %d", t->head_coach->development);
        char lb[24]; std::snprintf(lb, sizeof(lb), "LEAD %d", t->head_coach->leadership);
        Pill(tb, kSurfaceHi, kVlrText); ImGui::SameLine();
        Pill(db, kSurfaceHi, kVlrText); ImGui::SameLine();
        Pill(lb, kSurfaceHi, kVlrText);
        VSpace(6);
        MutedText("Match synergy boost: +%.1f%%    Dev mult: %.2fx",
            (t->head_coach->match_synergy_mult() - 1.0) * 100.0,
            t->head_coach->dev_chance_mult());
        } EndCard();
    }
}

#include "gui_roster.inc"

void DrawStandings(AppState& s) {
    ImGui::PushFont(g_font_h1);
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
    ImGui::TextUnformatted("LEAGUE STANDINGS");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    VSpace(12);

    // Division selector — switch between tier-1 (VCT) and the lower divisions
    // (Challengers, ...). Only shown when lower tiers exist.
    static int standings_tier = 1;  // 1-based
    int max_tier = 1;
    for (auto& kv : s.gm.tier_leagues_)
        max_tier = std::max(max_tier, static_cast<int>(kv.second.size()));
    if (max_tier > 1) {
        const char* tier_names[] = {"Tier 1 - VCT", "Tier 2 - Challengers", "Tier 3 - Open"};
        int sel = standings_tier - 1;
        if (sel < 0) sel = 0;
        if (sel >= max_tier) sel = max_tier - 1;
        ImGui::SetNextItemWidth(240);
        if (ImGui::Combo("Division", &sel, tier_names, std::min(max_tier, 3)))
            standings_tier = sel + 1;
        VSpace(8);
    }

    // Gather the leagues to show for the selected tier (one per region).
    std::vector<vlr::League*> shown;
    if (s.gm.tier_leagues_.empty()) {
        for (auto& kv : s.gm.leagues) if (kv.second) shown.push_back(kv.second.get());
    } else {
        for (auto* region_str : vlr::kRegions) {
            auto lg = s.gm.league_at(region_str, standings_tier);
            if (lg) shown.push_back(lg.get());
        }
    }
    if (shown.empty()) { ImGui::TextDisabled("No leagues in this division."); return; }

    ImGui::Columns(static_cast<int>(shown.size()), nullptr, false);
    for (auto* lg : shown) {
        SectionHeader(lg->name().c_str(), nullptr, kAccent);
        // WS-B: region meta style label (each league is one region).
        if (!lg->teams().empty() && lg->teams().front()) {
            ImGui::PushFont(g_font_small);
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kAccent2));
            ImGui::Text("Meta: %s", vlr::region_meta(lg->teams().front()->region).brand);
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }
        std::vector<vlr::TeamPtr> sorted_t;
        for (auto& t : lg->teams()) if (t) sorted_t.push_back(t);
        std::sort(sorted_t.begin(), sorted_t.end(),
                  [](const vlr::TeamPtr& a, const vlr::TeamPtr& b) { return a->phase_wins > b->phase_wins; });
        if (ImGui::BeginTable(lg->name().c_str(), 6,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_Sortable |
                ImGuiTableFlags_SortMulti)) {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_NoSort);
            ImGui::TableSetupColumn("TEAM", ImGuiTableColumnFlags_WidthStretch, 0, 1);
            ImGui::TableSetupColumn("OVR", ImGuiTableColumnFlags_None, 0, 2);
            ImGui::TableSetupColumn("W",
                ImGuiTableColumnFlags_DefaultSort |
                ImGuiTableColumnFlags_PreferSortDescending, 0, 3);
            ImGui::TableSetupColumn("L", ImGuiTableColumnFlags_None, 0, 4);
            ImGui::TableSetupColumn("FORM", ImGuiTableColumnFlags_NoSort, 0, 5);
            ImGui::TableHeadersRow();
            if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs()) {
                if (sp->SpecsDirty && sp->SpecsCount > 0) {
                    const ImGuiTableColumnSortSpecs& c = sp->Specs[0];
                    bool asc = (c.SortDirection == ImGuiSortDirection_Ascending);
                    std::sort(sorted_t.begin(), sorted_t.end(),
                        [&](const vlr::TeamPtr& a, const vlr::TeamPtr& b) {
                            switch (c.ColumnUserID) {
                            case 1: return asc ? (a->name < b->name)
                                               : (a->name > b->name);
                            case 2: return asc ? (a->ovr() < b->ovr())
                                               : (a->ovr() > b->ovr());
                            case 4: return asc ? (a->phase_losses < b->phase_losses)
                                               : (a->phase_losses > b->phase_losses);
                            case 3:
                            default: return asc ? (a->phase_wins < b->phase_wins)
                                                : (a->phase_wins > b->phase_wins);
                            }
                        });
                    sp->SpecsDirty = false;
                }
            }
            int rank = 1;
            for (auto& t : sorted_t) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(placement_color(rank)));
                ImGui::PushFont(g_font_mono);
                ImGui::Text("#%d", rank);
                ImGui::PopFont();
                ImGui::PopStyleColor();
                ImGui::TableNextColumn();
                {
                    ImGui::PushID(rank);
                    // Phase 2: 24px team logo to the left of the tag/name.
                    TeamLogo(*t, 24.f);
                    ImGui::SameLine();
                    bool is_user = (t == s.gm.user_team);
                    if (is_user) ImGui::PushStyleColor(ImGuiCol_Text, toV4(kAccent));
                    char buf[96];
                    std::snprintf(buf, sizeof(buf), "%s%-4s %s",
                                  is_user ? "> " : "  ",
                                  t->tag.empty() ? "???" : t->tag.c_str(),
                                  t->name.c_str());
                    if (TableRowSelectable(buf, false, ImGuiSelectableFlags_SpanAllColumns)) {
                        OpenTeamProfile(s, t);
                    }
                    if (is_user) ImGui::PopStyleColor();
                    ImGui::PopID();
                }
                ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%.0f", t->ovr()); ImGui::PopFont();
                ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%d", t->phase_wins); ImGui::PopFont();
                ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%d", t->phase_losses); ImGui::PopFont();
                // FORM sparkline — last-5 league results as green/red squares.
                ImGui::TableNextColumn();
                {
                    const auto& rr = t->recent_results;
                    int n = static_cast<int>(rr.size());
                    if (n == 0) {
                        ImGui::TextDisabled("--");
                    } else {
                        int start = std::max(0, n - 5);
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        ImVec2 cp = ImGui::GetCursorScreenPos();
                        float sq = 9.0f, gap = 3.0f, y = cp.y + 3.0f, x = cp.x;
                        for (int i = start; i < n; ++i) {
                            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + sq, y + sq),
                                              rr[i] ? kVlrGreen : kVlrRed, 2.0f);
                            x += sq + gap;
                        }
                        ImGui::Dummy(ImVec2((sq + gap) * (n - start), sq + 4.0f));
                        if (ImGui::IsItemHovered()) {
                            std::string f;
                            for (int i = start; i < n; ++i) f += rr[i] ? 'W' : 'L';
                            ImGui::SetTooltip("Recent form: %s (newest at right)", f.c_str());
                        }
                    }
                }
                ++rank;
            }
            ImGui::EndTable();
        }
        ImGui::NextColumn();
    }
    ImGui::Columns(1);
}

// =========================================================================
// Bracket renderer (VCT / NBA playoff style — full redesign)
// =========================================================================
// Layout philosophy:
//   - Two visually separated panels: UPPER BRACKET on top, LOWER BRACKET on
//     bottom, with a horizontal divider between them. The Grand Final lives
//     in its own column to the right of the upper panel.
//   - Each round is a vertical column of fixed-size cards (~150x60).
//     Between-card spacing in column N is `base_pitch * 2^N` so that round
//     N+1's cards naturally align to the MIDPOINT of their two feeders.
//   - Connector lines are H-V-H stepped polylines (NOT diagonals) drawn
//     with ImDrawList::AddLine. Played-path connectors are bright; unplayed
//     are muted gray.
//   - BO5 matches get a small gold pill in the top-right corner.
//   - Click semantics preserved: played cards open the winner's team
//     profile; unplayed cards open the live match viewer with the right
//     `best_of` taken from BracketMatch::best_of.
//
// Round labels come from two sources:
//   - The latest scheduled / in-progress round's label is provided by
//     `Tournament::current_round_label()` (Agent A contract).
//   - Historical rounds derive their label from index/team-count fallback:
//     UB R1 / UB Quarterfinals / UB Semifinals / UB Final
//     LB R1 / LB R2 / ... / LB Semifinal / LB Final (BO5)
namespace bracket_ui {
constexpr float kCardW       = 152.0f;   // VCT-style narrower card
constexpr float kCardH       = 60.0f;
constexpr float kColGap      = 56.0f;    // horizontal gap between rounds
constexpr float kBasePitch   = 78.0f;    // vertical pitch in round 0 (card + gap)
constexpr float kRoundLabelH = 26.0f;
constexpr float kStripeW     = 4.0f;
constexpr float kPanelPad    = 16.0f;

// Color helpers — keep ImU32 literals out of the call sites.
constexpr ImU32 kCardBg          = IM_COL32(0x16, 0x1E, 0x2A, 0xFF);
constexpr ImU32 kCardBgUnplayed  = IM_COL32(0x12, 0x18, 0x22, 0xFF);
constexpr ImU32 kCardBorder      = IM_COL32(0x2C, 0x3A, 0x4C, 0xFF);
constexpr ImU32 kCardBorderLive  = IM_COL32(0xFF, 0xD7, 0x00, 0xFF);  // gold
constexpr ImU32 kCardDivider     = IM_COL32(0x22, 0x2C, 0x3A, 0xFF);
constexpr ImU32 kRowWin          = IM_COL32(0x22, 0x2D, 0x3E, 0xFF);
constexpr ImU32 kTextWhite       = IM_COL32(0xEC, 0xE8, 0xE1, 0xFF);
constexpr ImU32 kTextDim         = IM_COL32(0x70, 0x7B, 0x88, 0xFF);
constexpr ImU32 kTextMid         = IM_COL32(0xB4, 0xBE, 0xC8, 0xFF);
constexpr ImU32 kLineUnplayed    = IM_COL32(0x3A, 0x46, 0x58, 0xFF);
constexpr ImU32 kLinePlayed      = IM_COL32(0xC4, 0xCC, 0xD6, 0xFF);
constexpr ImU32 kPanelBg         = IM_COL32(0x0E, 0x16, 0x20, 0xFF);
constexpr ImU32 kPanelBorder     = IM_COL32(0x24, 0x30, 0x40, 0xFF);
}  // namespace bracket_ui

#include "gui_brackets.inc"

#include "gui_market.inc"

void DrawSoloQ(AppState& s) {
    ImGui::PushFont(g_font_h1);
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
    ImGui::TextUnformatted("SOLO Q LADDER");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    VSpace(10);

    const char* regions[] = {"Americas", "EMEA", "Pacific"};
    int rc = 0;
    for (int i = 0; i < 3; ++i) if (s.ladder_region == regions[i]) rc = i;
    ImGui::SetNextItemWidth(180);
    if (ImGui::Combo("Region", &rc, regions, IM_ARRAYSIZE(regions))) s.ladder_region = regions[rc];
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180);
    ImGui::SliderInt("Min MMR", &s.soloq_min_mmr, 1100, 4000);

    auto it = s.gm.solo_qs.find(s.ladder_region);
    if (it == s.gm.solo_qs.end()) return;
    auto board = it->second->get_leaderboard();

    VSpace(10);
    if (BeginCard("##soloq_card")) {
    SectionHeader("RANKED LEADERBOARD", "Top ranked players by MMR", kAccent);
    if (ImGui::BeginTable("##ladder", 10,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Sortable,
            ImVec2(-FLT_MIN, 560))) {
        ImGuiTableColumnFlags desc = ImGuiTableColumnFlags_PreferSortDescending;
        ImGui::TableSetupColumn("#",    ImGuiTableColumnFlags_NoSort);
        ImGui::TableSetupColumn("FLAG", ImGuiTableColumnFlags_NoSort);
        ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("RANK", desc);
        ImGui::TableSetupColumn("MMR",  desc | ImGuiTableColumnFlags_DefaultSort);
        ImGui::TableSetupColumn("OVR",  desc);
        ImGui::TableSetupColumn("ROLE");
        ImGui::TableSetupColumn("W",    desc);
        ImGui::TableSetupColumn("L",    desc);
        ImGui::TableSetupColumn("K/D",  desc);
        ImGui::TableHeadersRow();

        // Apply the Min-MMR filter (and the 250-row cap) BEFORE sorting so the
        // sort always acts on the displayed set. board is already MMR-desc.
        std::vector<vlr::PlayerPtr> ladder;
        ladder.reserve(256);
        for (auto& p : board) {
            if (!p) continue;
            if (p->solo_mmr < s.soloq_min_mmr) continue;
            ladder.push_back(p);
            if (ladder.size() >= 250) break;
        }
        // Click-to-sort: single-key (Specs[0]), same idiom as DrawSeasonStats.
        // RANK sorts by solo_mmr (rank is derived from it); ROLE by position
        // enum; K/D uses solo_kd() which guards deaths==0 internally.
        if (ImGuiTableSortSpecs* sd = ImGui::TableGetSortSpecs()) {
            if (sd->SpecsCount > 0) {
                const ImGuiTableColumnSortSpecs& sp = sd->Specs[0];
                bool asc = sp.SortDirection == ImGuiSortDirection_Ascending;
                std::sort(ladder.begin(), ladder.end(),
                    [&](const vlr::PlayerPtr& a, const vlr::PlayerPtr& b) {
                        double va = 0, vb = 0;
                        switch (sp.ColumnIndex) {
                            case 2: return asc ? (a->name < b->name) : (a->name > b->name);
                            case 3: va = a->solo_mmr; vb = b->solo_mmr; break;
                            case 4: va = a->solo_mmr; vb = b->solo_mmr; break;
                            case 5: va = a->ovr();    vb = b->ovr();    break;
                            case 6: va = (double)(int)vlr::position_of(*a);
                                    vb = (double)(int)vlr::position_of(*b); break;
                            case 7: va = a->solo_wins;   vb = b->solo_wins;   break;
                            case 8: va = a->solo_losses; vb = b->solo_losses; break;
                            case 9: va = a->solo_kd();   vb = b->solo_kd();   break;
                            default: va = a->solo_mmr; vb = b->solo_mmr; break;
                        }
                        return asc ? (va < vb) : (va > vb);
                    });
            }
        }

        int rank = 1;
        for (auto& p : ladder) {
            ImGui::PushID(rank);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(placement_color(rank)));
            ImGui::PushFont(g_font_mono);
            ImGui::Text("#%d", rank);
            ImGui::PopFont();
            ImGui::PopStyleColor();
            ImGui::TableNextColumn(); CountryFlag(p->country_iso);
            ImGui::TableNextColumn();
            if (ImGui::Selectable(player_label(*p).c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
                OpenPlayerModal(s, p);
            }
            ImGui::TableNextColumn(); ImGui::Text("%s", p->rank_name().c_str());
            ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%d", p->solo_mmr); ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%.0f", p->ovr()); ImGui::PopFont();
            ImGui::TableNextColumn();
            PositionBadge(vlr::position_of(*p));
            if (p->is_igl) { ImGui::SameLine(); IglBadge(); }
            ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%d", p->solo_wins); ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%d", p->solo_losses); ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%.2f", p->solo_kd()); ImGui::PopFont();
            ImGui::PopID();
            ++rank;
        }
        ImGui::EndTable();
    }
    } EndCard();
}

void DrawCoach(AppState& s) {
    ImGui::PushFont(g_font_h1);
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
    ImGui::TextUnformatted("HEAD COACH");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    VSpace(10);
    auto& t = s.gm.user_team;
    if (!t || !t->head_coach) {
        if (BeginCard("##coach_empty")) {
            ImGui::TextDisabled("No coach signed.");
        } EndCard();
        return;
    }
    auto& c = t->head_coach;

    if (BeginCard("##coach_card")) {
    SectionHeader("PROFILE", nullptr, kAccent2);
    if (s.god_mode) {
        char nm[128];
        std::snprintf(nm, sizeof(nm), "%s", c->name.c_str());
        ImGui::SetNextItemWidth(360);
        if (ImGui::InputText("##coach_name", nm, sizeof(nm))) c->name = nm;
    } else {
        ImGui::PushFont(g_font_h2);
        ImGui::TextUnformatted(c->name.c_str());
        ImGui::PopFont();
    }
    MutedText("Region: %s   Age: %d   Salary: $%dK/yr",
        c->region.c_str(), c->age, c->salary_k);
    // Coach personality archetype — drives mid-season replacement
    // aggressiveness, play-calling philosophy, and development chance.
    // Engine has it (CoachPersonality, 16 archetypes) but it was never
    // surfaced in the UI; users only saw the 4 numeric stats.
    {
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
        ImGui::Text("Personality: %s",
                    vlr::coach_personality_name(c->personality));
        ImGui::PopStyleColor();
        ImGui::PushFont(g_font_small);
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX()
                               + ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted(vlr::coach_personality_blurb(c->personality));
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
    VSpace(10);

    auto stat = [&](const char* lbl, int& v) {
        if (s.god_mode) {
            DrawAttrSlider(lbl, v);
        } else {
            ImGui::Text("%-15s", lbl);
            ImGui::SameLine(170);
            ImGui::ProgressBar(v / 99.0f, ImVec2(280, 0), "");
            ImGui::SameLine();
            ImGui::Text("%d", v);
        }
    };
    stat("Tactical",    c->tactical);
    stat("Development", c->development);
    stat("Leadership",  c->leadership);
    stat("Experience",  c->experience);

    if (s.god_mode) {
        ImGui::SetNextItemWidth(120); ImGui::DragInt("Age", &c->age, 1.0f, 25, 70);
        ImGui::SameLine(260);
        ImGui::SetNextItemWidth(120); ImGui::DragInt("Salary $K", &c->salary_k, 1.0f, 15, vlr::kSalaryCapK, "%d", ImGuiSliderFlags_AlwaysClamp);
    }
    } EndCard();

    VSpace(14);
    if (BeginCard("##coach_derived")) {
    SectionHeader("WHAT THIS COACH DOES", nullptr, kAccent);

    // 1) Match-day tactical edge — a SMALL, bounded squad-power tilt.
    ImGui::Text("%-22s", "Match tactics");
    ImGui::SameLine(200);
    ImGui::TextColored(toV4(kVlrGold), "+%.1f%% squad synergy",
                       (c->match_synergy_mult() - 1.0) * 100.0);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Tactical + leadership feed a small, hard-capped match-power\n"
                          "bonus. A great coach helps win close maps but the duel\n"
                          "ceiling still binds \xE2\x80\x94 it can never break game balance.");

    // 2) Player development — the headline value of a dev coach.
    {
        double base = c->dev_growth_factor();
        double df   = t->coach_lean().dev_focus;
        double ch   = vlr::clamp_v(base * (1.0 + 0.4 * df), 0.0, 0.45);
        double exp_pts = vlr::clamp_v(ch * 3.0, 0.0, 3.0);  // expected attr/young player/yr
        ImGui::Text("%-22s", "Player development");
        ImGui::SameLine(200);
        ImGui::TextColored(toV4(kAccent2), "~%.1f attr pts / youngster / yr", exp_pts);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Each off-season this coach pushes young players toward their\n"
                              "potential \xE2\x80\x94 never past it \xE2\x80\x94 capped at 3 attribute points\n"
                              "per player per season. Higher Development + a dev-focused\n"
                              "personality raise this.");
    }

    // 3) Mid-season ruthlessness (existing engine behaviour, now surfaced).
    ImGui::Text("%-22s", "Bench trigger");
    ImGui::SameLine(200);
    ImGui::Text("%.0f%% \xE2\x80\x94 drops underperformers",
                vlr::personality_replacement_aggressiveness(c->personality) * 100.0);

    VSpace(8);
    ImGui::Separator();
    VSpace(4);
    ImGui::Text("Career:  %d title(s)   |   %d Coach of the Year   |   reputation %d",
                c->career_titles, c->career_coty, c->reputation);
    ImGui::Text("Asking salary now:  $%dK/yr", c->requested_salary_k());
    } EndCard();
}

void DrawHallOfFame(AppState& s) {
    SectionHeader("HALL OF FAME", "The all-time greats", kVlrGold);
    VSpace(6);
    if (s.gm.hall_of_fame.empty()) {
        if (BeginCard("##hof_empty")) {
            ImGui::TextDisabled("No inductees yet. Sim a few seasons!");
        } EndCard();
        return;
    }
    BeginCard("##hof_card");
    if (ImGui::BeginTable("##hof", 6,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY,
            ImVec2(-FLT_MIN, 520))) {
        ImGui::TableSetupColumn("FLAG");
        ImGui::TableSetupColumn("NAME");
        ImGui::TableSetupColumn("REGION");
        ImGui::TableSetupColumn("CAREER MATCHES");
        ImGui::TableSetupColumn("AVG RAT");
        ImGui::TableSetupColumn("AWARDS");
        ImGui::TableHeadersRow();
        int idx = 0;
        for (auto& p : s.gm.hall_of_fame) {
            ImGui::PushID(idx++);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); CountryFlag(p->country_iso);
            ImGui::TableNextColumn();
            if (TableRowSelectable(p->name.c_str(), false,
                    ImGuiSelectableFlags_SpanAllColumns)) {
                OpenPlayerModal(s, p);
            }
            ImGui::TableNextColumn(); ImGui::Text("%s", p->region.c_str());
            ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%d", p->career_matches); ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%.2f", p->avg_match_rating()); ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%zu", p->awards.size()); ImGui::PopFont();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    EndCard();
}

void DrawEventLog(AppState& s) {
    ImGui::PushFont(g_font_h1);
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
    ImGui::TextUnformatted("EVENT LOG");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    VSpace(10);
    if (BeginCard("##log_card", kBgDeep)) {
        SectionHeader("ACTIVITY FEED", "Most recent first", kAccent);
        ImGui::BeginChild("##log", ImVec2(0, -FLT_MIN));
        ImGui::PushFont(g_font_mono);
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
        for (auto it = s.log.rbegin(); it != s.log.rend(); ++it) {
            ImGui::TextWrapped("%s", it->c_str());
        }
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::EndChild();
    } EndCard();
}

void DrawWatch(AppState& s) {
    H1("WATCH MATCH", kVlrRed);
    ImGui::Separator();
    Sub("Pick two teams and watch the round-by-round simulation. Useful for "
        "studying how attribute changes propagate through round outcomes.");
    ImGui::Spacing();

    const char* regions[] = {"Americas", "EMEA", "Pacific"};
    int rc = 0;
    for (int i = 0; i < 3; ++i) if (s.watch_region == regions[i]) rc = i;
    ImGui::SetNextItemWidth(200);
    if (ImGui::Combo("Region", &rc, regions, IM_ARRAYSIZE(regions))) {
        s.watch_region = regions[rc];
        s.watch_t1 = 0; s.watch_t2 = 1;
    }

    auto& league = s.gm.leagues.at(s.watch_region);
    std::vector<std::string> names;
    for (auto& t : league->teams()) if (t) names.push_back(t->name);
    std::vector<const char*> name_ptrs;
    for (auto& n : names) name_ptrs.push_back(n.c_str());

    s.watch_t1 = std::min<int>(s.watch_t1, (int)names.size() - 1);
    s.watch_t2 = std::min<int>(s.watch_t2, (int)names.size() - 1);

    ImGui::SetNextItemWidth(260); ImGui::Combo("Team A", &s.watch_t1, name_ptrs.data(), (int)name_ptrs.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(260); ImGui::Combo("Team B", &s.watch_t2, name_ptrs.data(), (int)name_ptrs.size());

    ImGui::Spacing();

    // === Matchup preview card: logos + tags for the two picked teams ===
    {
        vlr::TeamPtr pa, pb;
        if (s.watch_t1 >= 0 && s.watch_t1 < (int)league->teams().size())
            pa = league->teams()[s.watch_t1];
        if (s.watch_t2 >= 0 && s.watch_t2 < (int)league->teams().size())
            pb = league->teams()[s.watch_t2];
        auto side = [&](const vlr::TeamPtr& t, bool right) {
            if (!t) { ImGui::TextDisabled("(no team)"); return; }
            ImU32 tc = IM_COL32(t->color_primary_r, t->color_primary_g,
                                t->color_primary_b, 0xFF);
            ImGui::BeginGroup();
            TeamLogo(*t, 44.f);
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::PushFont(g_font_h2);
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(tc));
            ImGui::TextUnformatted(t->tag.empty() ? t->name.c_str()
                                                  : t->tag.c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::PushFont(g_font_small);
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
            ImGui::Text("%s  \xE2\x80\xA2  OVR %.0f",
                        t->region.c_str(), t->ovr());
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::EndGroup();
            ImGui::EndGroup();
            (void)right;
        };
        BeginCardSized("##watch_preview", ImVec2(560, 76), kSurface, 14.0f, 12.0f);
        side(pa, false);
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(16, 0));
        ImGui::SameLine();
        ImGui::PushFont(g_font_h1);
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kTextFaint));
        ImGui::TextUnformatted("VS");
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(16, 0));
        ImGui::SameLine();
        side(pb, true);
        EndCardSized();
    }

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, toV4(kVlrRed));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(kVlrRedDim));
    if (ImGui::Button("PLAY MATCH (LIVE)", ImVec2(220, 44)) &&
        s.watch_t1 != s.watch_t2 &&
        s.watch_t1 >= 0 && s.watch_t1 < (int)league->teams().size() &&
        s.watch_t2 >= 0 && s.watch_t2 < (int)league->teams().size()) {
        OpenLiveMatch(s, league->teams()[s.watch_t1], league->teams()[s.watch_t2], "Friendly");
    }
    ImGui::PopStyleColor(2);
}

// ===== Team Profile screen ===============================================
//
// Shown when the user clicks any team row anywhere in the app. Big top
// banner with the org's home flag/name; underneath: roster, stats, recent
// & upcoming matches, map performance, and ranking within their league.
#include "gui_team.inc"

// Modal for click-through: lists matchups on a given day. Populated when
// `s.cal_modal_day != -1`.
void DrawCalendarDayModal(AppState& s, const std::vector<vlr::UpcomingFixture>& fx);

}  // namespace

void DrawCalendar(AppState& s) {
    auto& gm = s.gm;

    // Header line + phase progress.
    auto pacing = gm.current_phase_pacing();
    int phase_total = pacing.second;
    int phase_day   = gm.day_in_phase + 1;
    char hbuf[128];
    std::snprintf(hbuf, sizeof(hbuf), "%d  -  %s, Day %d of %d",
                  gm.year, gm.current_phase().c_str(),
                  phase_day, std::max(phase_total, 1));
    ImGui::PushFont(g_font_h1);
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
    ImGui::TextUnformatted(hbuf);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    int remain = std::max(0, phase_total - phase_day);
    MutedText("Phase: %d day%s remaining   |   Season day %d of %d",
        remain, remain == 1 ? "" : "s",
        gm.day_in_year + 1, vlr::GameManager::kDaysPerYear);
    VSpace(10);

    // === Cursor control ====================================================
    int cursor = s.cal_cursor_day;
    if (cursor < 0) cursor = gm.day_in_year;   // initial: today
    int today = gm.day_in_year;
    int years_offset = s.cal_year_offset;       // +1 if user paged past Dec

    ImGui::Spacing();
    if (ImGui::Button("<< -30 days")) {
        cursor -= 30;
        while (cursor < 0) { cursor += vlr::GameManager::kDaysPerYear; years_offset--; }
    }
    ImGui::SameLine();
    if (ImGui::Button("Today")) { cursor = today; years_offset = 0; }
    ImGui::SameLine();
    if (ImGui::Button("+30 days >>")) {
        cursor += 30;
        while (cursor >= vlr::GameManager::kDaysPerYear) {
            cursor -= vlr::GameManager::kDaysPerYear; years_offset++;
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("   |   Cursor day %d of %d (year %d)",
                        cursor + 1, vlr::GameManager::kDaysPerYear,
                        gm.year + years_offset);
    s.cal_cursor_day = cursor;
    s.cal_year_offset = years_offset;

    // === Pre-fetch matchup data for the visible window ====================
    // 5 rows x 7 cols = 35 days. Start ~17 days before cursor for centering.
    constexpr int kGridRows = 5;
    constexpr int kGridCols = 7;
    constexpr int kCells    = kGridRows * kGridCols;
    int start_day = cursor - 17;
    int look_ahead_max = std::max(2, kCells + std::max(0, cursor - today) + 4);
    auto fixtures = gm.upcoming_fixtures(look_ahead_max);

    auto phase_label_for_day_offset = [&](int days_from_today) -> std::string {
        // Best-effort phase lookup: we know today's phase. Future days fall
        // back to "(?)". Tournament/awards/offseason still tint correctly
        // for "today" and same-phase neighbours via current_phase().
        if (days_from_today == 0) return gm.current_phase();
        // We don't have a per-day phase oracle outside engine internals;
        // returning empty avoids confidently mislabeling future days.
        return std::string();
    };

    std::vector<CalDayInfo> cells(kCells);
    for (int i = 0; i < kCells; ++i) {
        int rel_day = start_day + i;       // may be negative or > kDaysPerYear
        // Wrap into a usable representation that still flags today/match.
        int abs_offset = rel_day - today;
        int yr = gm.year + years_offset;
        int doy = rel_day;
        while (doy < 0) { doy += vlr::GameManager::kDaysPerYear; yr--; }
        while (doy >= vlr::GameManager::kDaysPerYear) {
            doy -= vlr::GameManager::kDaysPerYear; yr++;
        }
        cells[i].day = doy;
        cells[i].year = yr;
        cells[i].is_today = (rel_day == today && years_offset == 0);
        cells[i].phase = (abs_offset == 0) ? gm.current_phase()
                                           : phase_label_for_day_offset(abs_offset);
    }
    // Map fixtures into cells whose absolute day matches.
    for (auto& f : fixtures) {
        int rel_day = f.day_in_year;       // returned absolute day_in_year
        int idx = rel_day - start_day;
        if (idx < 0 || idx >= kCells) continue;
        if (cells[idx].year != gm.year)  continue;  // ignore wrap mismatches
        cells[idx].is_matchday = true;
        cells[idx].fixtures.push_back(f);
        if (gm.user_team && (f.a == gm.user_team || f.b == gm.user_team)) {
            cells[idx].is_user_match = true;
            auto opp = (f.a == gm.user_team) ? f.b : f.a;
            cells[idx].short_tag = opp ? (std::string("vs ") +
                                          (opp->tag.empty() ? opp->name : opp->tag))
                                       : std::string("MATCH");
        }
    }
    // Tournament day flag: mark cells where any active tournament has
    // its current round scheduled for "today" — engine doesn't expose
    // per-day tour matchups for future days here, so we tint the cells
    // whose phase begins with REGIONALS/MASTERS/CHAMPIONS (today only).
    for (auto& c : cells) {
        if (c.is_today) {
            if (c.phase.find("REGIONALS") == 0 ||
                c.phase.find("MASTERS")   == 0 ||
                c.phase.find("CHAMPIONS") == 0) {
                c.is_tournament = true;
                if (c.short_tag.empty() && !gm.active_tournaments.empty()) {
                    c.short_tag = "TOUR";
                }
            }
        }
    }

    // === Draw the grid ====================================================
    constexpr float kCellW = 80.f;
    constexpr float kCellH = 56.f;
    constexpr float kPad   = 4.f;

    if (ImGui::BeginTable("##cal_grid", kGridCols,
            ImGuiTableFlags_None,
            ImVec2((kCellW + kPad) * kGridCols, (kCellH + kPad) * kGridRows + 28))) {
        for (int c = 0; c < kGridCols; ++c) {
            ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthFixed, kCellW);
        }
        for (int r = 0; r < kGridRows; ++r) {
            ImGui::TableNextRow(0, kCellH);
            for (int c = 0; c < kGridCols; ++c) {
                int idx = r * kGridCols + c;
                ImGui::TableNextColumn();
                auto& cell = cells[idx];
                ImGui::PushID(idx);

                ImVec2 origin = ImGui::GetCursorScreenPos();
                ImVec2 cmax(origin.x + kCellW, origin.y + kCellH);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                constexpr float kCR = 7.0f;  // cell rounding

                // Base surface so empty cells still read as tiles.
                dl->AddRectFilled(origin, cmax,
                                  cell.day < 0 ? kBgDeep : kSurface, kCR);

                // Phase tint overlay (kept) then user-match accent fill.
                ImU32 bg = phase_tint(cell.phase);
                if (bg) dl->AddRectFilled(origin, cmax, bg, kCR);
                if (cell.is_user_match) {
                    dl->AddRectFilled(origin, cmax,
                                      IM_COL32(0x10, 0x3A, 0x52, 0xFF), kCR);
                }

                // Hover raise.
                bool hovered = ImGui::IsMouseHoveringRect(origin, cmax);
                if (hovered && cell.day >= 0) {
                    dl->AddRectFilled(origin, cmax, kSurfaceHi, kCR);
                    if (cell.is_user_match)
                        dl->AddRectFilled(origin, cmax,
                                          IM_COL32(0x16, 0x46, 0x66, 0xFF), kCR);
                }

                // Hairline border on every cell.
                dl->AddRect(origin, cmax, kBorder, kCR, 0, 1.0f);

                // User-match accent edge.
                if (cell.is_user_match) {
                    dl->AddRect(origin, cmax, kAccent, kCR, 0, 1.6f);
                }
                // Tournament border (red).
                if (cell.is_tournament) {
                    dl->AddRect(origin, cmax, kVlrRed, kCR, 0, 2.0f);
                }
                // Today: accent border + soft glow.
                if (cell.is_today) {
                    dl->AddRect(ImVec2(origin.x - 1, origin.y - 1),
                                ImVec2(cmax.x + 1, cmax.y + 1),
                                IM_COL32(0x00, 0xD4, 0xFF, 0x40), kCR, 0, 4.0f);
                    dl->AddRect(origin, cmax, kAccent, kCR, 0, 2.5f);
                }

                // Day-in-year top-left.
                char dbuf[16];
                std::snprintf(dbuf, sizeof(dbuf), "%d", cell.day + 1);
                ImGui::PushFont(g_font_small);
                dl->AddText(ImVec2(origin.x + 6, origin.y + 4),
                            cell.is_today ? kAccent : kTextFaint, dbuf);
                ImGui::PopFont();

                // Centered short tag.
                if (!cell.short_tag.empty()) {
                    ImVec2 ts = ImGui::CalcTextSize(cell.short_tag.c_str());
                    ImU32 tagc = cell.is_user_match ? kAccent : kVlrText;
                    dl->AddText(ImVec2(origin.x + (kCellW - ts.x) * 0.5f,
                                       origin.y + (kCellH - ts.y) * 0.5f + 4),
                                tagc, cell.short_tag.c_str());
                }

                // Invisible button for hover + click.
                ImGui::SetCursorScreenPos(origin);
                if (ImGui::InvisibleButton("##cl", ImVec2(kCellW, kCellH))) {
                    if (!cell.fixtures.empty()) {
                        s.cal_modal_day = cell.day;
                        s.cal_modal_year = cell.year;
                        s.cal_modal_fixtures = cell.fixtures;
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::Text("Day %d  -  Year %d", cell.day + 1, cell.year);
                    if (!cell.phase.empty()) ImGui::TextDisabled("%s", cell.phase.c_str());
                    if (cell.fixtures.empty()) {
                        ImGui::TextDisabled("(no scheduled matchups)");
                    } else {
                        ImGui::Separator();
                        for (auto& f : cell.fixtures) {
                            ImGui::Text("[%s] %s vs %s",
                                        f.region.c_str(),
                                        f.a ? f.a->name.c_str() : "?",
                                        f.b ? f.b->name.c_str() : "?");
                        }
                    }
                    ImGui::EndTooltip();
                }

                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();

    // === Legend bar =======================================================
    Pill("YOUR MATCH", kAccent);                              ImGui::SameLine();
    Pill("PLAYOFFS",   IM_COL32(0x8A, 0x3A, 0x3A, 0xFF));     ImGui::SameLine();
    Pill("MASTERS",    kAccent2);                             ImGui::SameLine();
    Pill("CHAMPIONS",  kVlrGold);                             ImGui::SameLine();
    Pill("OFFSEASON",  kVlrGreen);                            ImGui::SameLine();
    Pill("AWARDS",     IM_COL32(0xC4, 0x9A, 0x52, 0xFF));
    ImGui::NewLine();

    // === Day modal ========================================================
    if (s.cal_modal_day != -1) {
        DrawCalendarDayModal(s, s.cal_modal_fixtures);
    }
}

namespace {

void DrawCalendarDayModal(AppState& s, const std::vector<vlr::UpcomingFixture>& fx) {
    ImGui::OpenPopup("Day Matchups##calmodal");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(640, 480), ImGuiCond_Appearing);
    bool open = true;
    if (ImGui::BeginPopupModal("Day Matchups##calmodal", &open,
                               ImGuiWindowFlags_NoSavedSettings)) {
        char hb[64];
        std::snprintf(hb, sizeof(hb), "Year %d  -  Day %d",
                      s.cal_modal_year, s.cal_modal_day + 1);
        SectionHeader(hb, "Scheduled matchups", kAccent);
        if (fx.empty()) {
            ImGui::TextDisabled("(no matchup data available)");
        } else {
            if (ImGui::BeginTable("##cm", 4,
                    ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("REGION");
                ImGui::TableSetupColumn("EVENT");
                ImGui::TableSetupColumn("MATCHUP", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("WATCH");
                ImGui::TableHeadersRow();
                int row = 0;
                for (auto& f : fx) {
                    ImGui::TableNextRow();
                    ImGui::PushID(row++);
                    ImGui::TableNextColumn(); ImGui::Text("%s", f.region.c_str());
                    ImGui::TableNextColumn(); ImGui::Text("%s", f.label.c_str());
                    ImGui::TableNextColumn();
                    if (f.a && f.b) {
                        char mb[128];
                        std::snprintf(mb, sizeof(mb), "%s  vs  %s",
                                      f.a->name.c_str(), f.b->name.c_str());
                        ImGui::TextUnformatted(mb);
                    } else {
                        ImGui::TextDisabled("?");
                    }
                    ImGui::TableNextColumn();
                    if (f.a && f.b) {
                        if (ImGui::SmallButton("WATCH")) {
                            // Capture before clearing: this loop iterates a
                            // const-ref bound to s.cal_modal_fixtures, so the
                            // clear() below invalidates `f`/`fx`. Snapshot the
                            // fields, clear, pop our ID, then break out.
                            vlr::TeamPtr wa = f.a, wb = f.b;
                            std::string wlabel = f.label;
                            s.cal_modal_day = -1;
                            s.cal_modal_fixtures.clear();
                            ImGui::CloseCurrentPopup();
                            ImGui::PopID();
                            OpenLiveMatchSeries(s, wa, wb, wlabel, 3);
                            break;
                        }
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }
        VSpace(8);
        ImGui::PushStyleColor(ImGuiCol_Button,        toV4(kAccent));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(kAccentDim));
        ImGui::PushStyleColor(ImGuiCol_Text,          toV4(IM_COL32(0x08,0x12,0x16,0xFF)));
        if (ImGui::Button("Close", ImVec2(120, 32))) {
            s.cal_modal_day = -1;
            s.cal_modal_fixtures.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(3);
        ImGui::EndPopup();
    }
    if (!open) {
        s.cal_modal_day = -1;
        s.cal_modal_fixtures.clear();
    }
}

}  // namespace

// ===== Progression screen: last monthly tick's deltas =====================
void DrawProgression(AppState& s) {
    ImGui::PushFont(g_font_h1);
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
    ImGui::TextUnformatted("PLAYER DEVELOPMENT");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    MutedText("Every 30 in-game days each player runs a development pass. "
        "Pros grow based on match performance + coaching; free agents grow "
        "from ranked performance and natural age-curve.");
    VSpace(12);

    auto& list = s.gm.recent_progression_reports;
    if (list.empty()) {
        if (BeginCard("##prog_empty")) {
            ImGui::TextDisabled("(no recent monthly progression — advance ~30 days)");
        } EndCard();
        return;
    }

    if (BeginCard("##prog_card")) {
    SectionHeader("LAST MONTHLY TICK", "Attribute deltas from the most recent development pass", kAccent);
    if (ImGui::BeginTable("##prog", 6,
            ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, 560))) {
        ImGui::TableSetupColumn("PLAYER");
        ImGui::TableSetupColumn("TYPE");
        ImGui::TableSetupColumn("MATCHES");
        ImGui::TableSetupColumn("AVG RAT");
        ImGui::TableSetupColumn("CHANGES");
        ImGui::TableSetupColumn("EXPLANATION", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (auto& r : list) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%s", r.player_name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(toV4(r.was_pro ? kVlrGreen : kInfo),
                               "%s", r.was_pro ? "PRO" : "FA");
            ImGui::TableNextColumn();
            if (r.was_pro) ImGui::Text("%d", r.matches_in_window);
            else           ImGui::Text("%d ranked", r.ranked_played);
            ImGui::TableNextColumn();
            if (r.matches_in_window > 0) ImGui::Text("%.2f", r.avg_rating);
            else                          ImGui::Text("—");
            ImGui::TableNextColumn();
            if (r.changes.empty()) {
                ImGui::TextDisabled("—");
            } else {
                for (auto& c : r.changes) {
                    ImU32 col = c.second > 0 ? kVlrGreen : kVlrRed;
                    ImGui::PushStyleColor(ImGuiCol_Text, toV4(col));
                    ImGui::Text("%s%+d ", vlr::attr_name(c.first), c.second);
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                }
                ImGui::NewLine();
            }
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", r.explanation.c_str());
        }
        ImGui::EndTable();
    }
    } EndCard();
}

// ===== Favorites + GOAT Lab ===============================================
void DrawMvpRaceBody(AppState& s) {
        H2("SEASON-LONG AWARD RACE");
        Sub("Updated every 30 in-game days. Hybrid score: season rating + "
            "team success + international weight. Climb / slip indicators "
            "track movement vs the previous tick.");
        ImGui::Spacing();

        if (s.gm.mvp_race.empty()) {
            ImGui::TextDisabled("(race builds after a few months of play)");
        } else {
            // Stacked-cards layout — one card per race, role-colored header,
            // leader pill, sortable SCORE table. Mirrors DrawHallOfRecordsTab.
            int lbi = 0;
            for (auto& lb : s.gm.mvp_race) {
                ImGui::PushID(lbi++);
                // Role color from the category label.
                ImU32 race_col = kVlrGold;
                if      (lb.category == "Duelist Race")    race_col = kRoleDuelist;
                else if (lb.category == "Initiator Race")  race_col = kRoleInitiator;
                else if (lb.category == "Controller Race") race_col = kRoleController;
                else if (lb.category == "Sentinel Race")   race_col = kRoleSentinel;
                else if (lb.category == "IGL Race")        race_col = kRoleFlex;
                else                                       race_col = kVlrGold; // MVP Race

                if (BeginCard("##race_card")) {
                    SectionHeader(lb.category.c_str(), nullptr, race_col);
                    // Leader pill — top candidate by score (candidates already
                    // ranked by the engine; front() is the leader).
                    {
                        const vlr::GameManager::RaceCandidate* leader = nullptr;
                        for (auto& c : lb.candidates) {
                            if (c.player) { leader = &c; break; }
                        }
                        if (leader) {
                            char lead[160];
                            std::snprintf(lead, sizeof(lead), "LEADER  %s  %.2f",
                                          leader->player->name.c_str(), leader->score);
                            Pill(lead, race_col);
                        }
                    }
                    VSpace(4);

                    // Local copy so the SCORE sort never mutates s.gm.mvp_race.
                    std::vector<vlr::GameManager::RaceCandidate> cands;
                    cands.reserve(lb.candidates.size());
                    for (auto& c : lb.candidates) if (c.player) cands.push_back(c);

                    enum RCol { RC_RANK, RC_FLAG, RC_PLAYER, RC_SIG, RC_SCORE,
                                RC_WHY, RC_TREND, RC_DELTA, RC_N };
                    if (ImGui::BeginTable("##race_tbl", RC_N,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable,
                            ImVec2(-FLT_MIN, 360))) {
                        ImGuiTableColumnFlags nosort = ImGuiTableColumnFlags_NoSort;
                        ImGui::TableSetupColumn("#", nosort);
                        ImGui::TableSetupColumn("FLAG", nosort);
                        ImGui::TableSetupColumn("PLAYER",
                            ImGuiTableColumnFlags_WidthStretch | nosort);
                        ImGui::TableSetupColumn("SIG", nosort);
                        ImGui::TableSetupColumn("SCORE",
                            ImGuiTableColumnFlags_PreferSortDescending |
                            ImGuiTableColumnFlags_DefaultSort);
                        ImGui::TableSetupColumn("WHY", nosort);
                        ImGui::TableSetupColumn("TREND", nosort);
                        ImGui::TableSetupColumn("DELTA", nosort);
                        ImGui::TableHeadersRow();

                        // Sortable SCORE column — same idiom as DrawSeasonStats.
                        if (ImGuiTableSortSpecs* sd = ImGui::TableGetSortSpecs()) {
                            if (sd->SpecsCount > 0) {
                                const ImGuiTableColumnSortSpecs& sp = sd->Specs[0];
                                bool asc = sp.SortDirection == ImGuiSortDirection_Ascending;
                                if (sp.ColumnIndex == RC_SCORE) {
                                    std::sort(cands.begin(), cands.end(),
                                        [&](const auto& a, const auto& b) {
                                            return asc ? (a.score < b.score)
                                                       : (a.score > b.score);
                                        });
                                }
                            }
                        }

                        int rk = 1, ri = 0;
                        for (auto& c : cands) {
                            if (!c.player) continue;
                            ImGui::PushID(ri++);
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::PushStyleColor(ImGuiCol_Text, toV4(placement_color(rk)));
                            ImGui::PushFont(g_font_mono);
                            ImGui::Text("#%d", rk);
                            ImGui::PopFont();
                            ImGui::PopStyleColor();
                            ImGui::TableNextColumn(); CountryFlag(c.player->country_iso);
                            ImGui::TableNextColumn();
                            if (TableRowSelectable(player_label(*c.player).c_str(), false,
                                    ImGuiSelectableFlags_SpanAllColumns)) {
                                OpenPlayerModal(s, c.player);
                            }
                            // SIG column — small dim text.
                            ImGui::TableNextColumn();
                            {
                                std::string sigA = c.player->signature_agent();
                                if (!sigA.empty()) {
                                    ImGui::PushFont(g_font_small);
                                    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
                                    ImGui::TextUnformatted(sigA.c_str());
                                    ImGui::PopStyleColor();
                                    ImGui::PopFont();
                                } else {
                                    ImGui::TextDisabled("-");
                                }
                            }
                            ImGui::TableNextColumn();
                            ImGui::PushFont(g_font_mono);
                            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                            ImGui::Text("%.2f", c.score);
                            ImGui::PopStyleColor();
                            ImGui::PopFont();
                            // Hover the score to see EXACTLY why this player ranks
                            // here — the full multiplicative MVP factor breakdown.
                            if (ImGui::IsItemHovered() && !c.breakdown.factors.empty()) {
                                ImGui::BeginTooltip();
                                ImGui::TextColored(toV4(kVlrGold), "Why this MVP score");
                                for (std::size_t fi = 0; fi < c.breakdown.factors.size(); ++fi) {
                                    const auto& f = c.breakdown.factors[fi];
                                    if (fi == 0) ImGui::Text("   %s", f.first.c_str());   // base rating
                                    else         ImGui::Text("   x %.2f   %s", f.second, f.first.c_str());
                                }
                                if (c.breakdown.team_wins > 0)
                                    ImGui::TextDisabled("   (%d team wins this season)", c.breakdown.team_wins);
                                ImGui::Separator();
                                ImGui::Text("   = %.2f", c.breakdown.total);
                                ImGui::EndTooltip();
                            }

                            // WHY column — dominant trait surfaced from the
                            // multi-factor MVP score.
                            ImGui::TableNextColumn();
                            const char* why = "Frag leader";
                            ImU32 why_col = kVlrText;
                            if (c.player->is_igl && c.player->igl_impact_season >= 5.0) {
                                why = "Elite IGL";
                                why_col = kVlrGold;
                            } else if (c.player->is_igl && c.player->igl_impact_season >= 3.0) {
                                why = "Tactical IGL";
                                why_col = kInfo;
                            } else if (c.player->season_pressure_matches >= 3) {
                                why = "Pressure closer";
                                why_col = kVlrGold;
                            } else if (c.player->career_max_match_kd_x100 >= 250
                                    && c.player->season_matches > 0) {
                                why = "Carry games";
                                why_col = kVlrGreen;
                            } else if (c.player->is_igl) {
                                why = "IGL season";
                                why_col = kInfo;
                            }
                            ImGui::PushStyleColor(ImGuiCol_Text, toV4(why_col));
                            ImGui::Text("%s", why);
                            ImGui::PopStyleColor();

                            ImGui::TableNextColumn();
                            ImU32 tcol = c.blurb == "Climbing" ? kVlrGreen
                                       : c.blurb == "Slipping" ? kVlrRed
                                                                : kVlrSub;
                            ImGui::PushStyleColor(ImGuiCol_Text, toV4(tcol));
                            ImGui::Text("%s", c.blurb.c_str());
                            ImGui::PopStyleColor();
                            ImGui::TableNextColumn();
                            if (c.delta > 0) {
                                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGreen));
                                ImGui::Text("+%d", c.delta);
                                ImGui::PopStyleColor();
                            } else if (c.delta < 0) {
                                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
                                ImGui::Text("%d", c.delta);
                                ImGui::PopStyleColor();
                            } else {
                                ImGui::TextDisabled("-");
                            }
                            ImGui::PopID();
                            ++rk;
                        }
                        ImGui::EndTable();
                    }
                }
                EndCard();
                ImGui::PopID();
                VSpace(10);
            }
        }
}

void DrawAwardsRecapBody(AppState& s) {
        H2("LAST SEASON AWARDS");
        if (s.gm.last_season_awards.empty()) {
            ImGui::TextDisabled("(no awards yet — finish a season first)");
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
            ImGui::Text("Year %d Awards", s.gm.last_season_awards.front().year);
            ImGui::PopStyleColor();
            ImGui::Spacing();

            int ai = 0;
            for (auto& aw : s.gm.last_season_awards) {
                ImGui::PushID(ai++);
                ImGui::BeginChild("##award_card", ImVec2(0, 0),
                                  ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY);
                ImGui::PushFont(g_font_h2);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                ImGui::Text("%s", aw.category.c_str());
                ImGui::PopStyleColor();
                ImGui::PopFont();

                if (aw.winner) {
                    ImGui::PushFont(g_font_h1);
                    CountryFlag(aw.winner->country_iso); ImGui::SameLine();
                    if (ImGui::Selectable(player_label(*aw.winner).c_str(), false,
                            0, ImVec2(380, 0))) {
                        OpenPlayerModal(s, aw.winner);
                    }
                    ImGui::PopFont();
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                    if (!aw.scores.empty()) ImGui::Text("  Score: %.2f", aw.scores.front());
                    ImGui::PopStyleColor();
                } else if (aw.coach_winner) {
                    // WS-B coach award (e.g. Coach of the Year). No player modal
                    // for coaches; render name + club inline.
                    ImGui::PushFont(g_font_h1);
                    CountryFlag(aw.coach_winner->country_iso); ImGui::SameLine();
                    ImGui::TextUnformatted(aw.coach_winner->name.c_str());
                    ImGui::PopFont();
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                    ImGui::Text("  (%s)", aw.coach_winner->team_name.c_str());
                    ImGui::PopStyleColor();
                }

                ImGui::PushFont(g_font_small);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
                ImGui::TextWrapped("%s", aw.explanation.c_str());
                ImGui::PopStyleColor();
                ImGui::PopFont();

                if (aw.finalists.size() > 1) {
                    ImGui::Text("Finalists:");
                    for (std::size_t i = 1; i < aw.finalists.size(); ++i) {
                        if (!aw.finalists[i]) continue;
                        ImGui::SameLine();
                        ImGui::PushID((int)i);
                        if (ImGui::SmallButton(aw.finalists[i]->name.c_str())) {
                            OpenPlayerModal(s, aw.finalists[i]);
                        }
                        ImGui::PopID();
                        if (i < aw.scores.size()) {
                            ImGui::SameLine();
                            ImGui::TextDisabled("(%.2f)", aw.scores[i]);
                        }
                    }
                }
                if (aw.coach_finalists.size() > 1) {
                    ImGui::Text("Finalists:");
                    for (std::size_t i = 1; i < aw.coach_finalists.size(); ++i) {
                        if (!aw.coach_finalists[i]) continue;
                        ImGui::SameLine();
                        ImGui::Text("%s", aw.coach_finalists[i]->name.c_str());
                        if (i < aw.scores.size()) {
                            ImGui::SameLine();
                            ImGui::TextDisabled("(%.1f)", aw.scores[i]);
                        }
                    }
                }
                ImGui::EndChild();
                ImGui::PopID();
                ImGui::Spacing();
            }

            // === Flex of the Year placeholder ===========================
            // The award is added by Agent A's compute_season_awards changes
            // (out of scope for this UI work). If the engine has populated
            // it, the per-award loop above already rendered it. If not, we
            // surface a placeholder card so the user can see the slot is
            // ready and just empty for this season.
            bool flex_award_present = false;
            for (auto& aw : s.gm.last_season_awards) {
                if (aw.category == "Flex of the Year") {
                    flex_award_present = true;
                    break;
                }
            }
            if (!flex_award_present) {
                ImGui::PushID("##flex_aw_placeholder");
                ImGui::BeginChild("##flex_aw_card", ImVec2(0, 80),
                                  ImGuiChildFlags_Border);
                ImGui::PushFont(g_font_h2);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kRoleFlex));
                ImGui::Text("Flex of the Year");
                ImGui::PopStyleColor();
                ImGui::PopFont();
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
                ImGui::TextWrapped("Not awarded this season.");
                ImGui::PopStyleColor();
                ImGui::EndChild();
                ImGui::PopID();
                ImGui::Spacing();
            }
        }
}

void DrawAwardsHistoryBody(AppState& s) {
        if (s.gm.awards_history.empty()) {
            ImGui::TextDisabled("(no awards yet)");
        } else if (ImGui::BeginTable("##aw_hist", 4,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, 540))) {
            ImGui::TableSetupColumn("YEAR");
            ImGui::TableSetupColumn("CATEGORY", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("WINNER", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("SCORE");
            ImGui::TableHeadersRow();
            int hi = 0;
            for (auto it = s.gm.awards_history.rbegin();
                 it != s.gm.awards_history.rend(); ++it) {
                ImGui::PushID(hi++);
                auto& aw = *it;
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%d", aw.year); ImGui::PopFont();
                ImGui::TableNextColumn(); ImGui::Text("%s", aw.category.c_str());
                ImGui::TableNextColumn();
                if (aw.winner) {
                    CountryFlag(aw.winner->country_iso); ImGui::SameLine();
                    if (ImGui::Selectable(aw.winner->name.c_str(), false)) {
                        OpenPlayerModal(s, aw.winner);
                    }
                } else if (aw.coach_winner) {
                    CountryFlag(aw.coach_winner->country_iso); ImGui::SameLine();
                    ImGui::Text("%s (coach)", aw.coach_winner->name.c_str());
                }
                ImGui::TableNextColumn();
                if (!aw.scores.empty()) { ImGui::PushFont(g_font_mono); ImGui::Text("%.2f", aw.scores.front()); ImGui::PopFont(); }
                else ImGui::TextDisabled("-");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
}

void DrawSeasonStats(AppState& s) {
    auto& gm = s.gm;
    SectionHeader("PLAYER STATS \xE2\x80\x94 SEASON",
        "Every tier-1 player's cumulative stats this season \xE2\x80\x94 the same "
        "board tournaments show, but live through the regular season. Resets "
        "each new year. Click a column to sort.",
        kAccent);
    {
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
        ImGui::Text("[*] %d \xE2\x80\x94 %s", gm.year, gm.current_phase().c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        MutedText("\xE2\x80\x94 stats accumulate across stage + playoff matches.");
    }
    VSpace(6);
    ImGui::SetNextItemWidth(260);
    ImGui::InputTextWithHint("##seasonstatsearch", "Search name...",
                             s.search, sizeof(s.search));
    VSpace(6);

    // === Tournament filter ================================================
    // Index 0 = "Season (all events)" (the season_* aggregate). Each further
    // entry is a unique tournament label present for the CURRENT year across
    // rostered tier-1 players' tourn_stats (keyed "<label>|<year>").
    std::string year_suffix = "|" + std::to_string(gm.year);
    std::vector<std::string> tourn_labels;   // labels only (no "Season")
    {
        std::set<std::string> seen;
        for (auto& kv : gm.leagues) {
            if (!kv.second) continue;
            for (auto& t : kv.second->teams()) {
                if (!t) continue;
                for (auto& p : t->roster) {
                    if (!p) continue;
                    for (auto& ts : p->tourn_stats) {
                        const std::string& key = ts.first;
                        if (key.size() <= year_suffix.size()) continue;
                        if (key.compare(key.size() - year_suffix.size(),
                                        year_suffix.size(), year_suffix) != 0)
                            continue;
                        seen.insert(key.substr(0, key.size() - year_suffix.size()));
                    }
                }
            }
        }
        tourn_labels.assign(seen.begin(), seen.end());
    }
    // Combo entries: [0]="Season (all events)", [1..]=tourn_labels.
    static int s_tourn_filter = 0;
    if (s_tourn_filter > (int)tourn_labels.size()) s_tourn_filter = 0;
    {
        std::string preview = (s_tourn_filter == 0)
            ? std::string("Season (all events)")
            : tourn_labels[s_tourn_filter - 1];
        ImGui::SetNextItemWidth(280);
        if (ImGui::BeginCombo("##tournfilter", preview.c_str())) {
            if (ImGui::Selectable("Season (all events)", s_tourn_filter == 0))
                s_tourn_filter = 0;
            for (int i = 0; i < (int)tourn_labels.size(); ++i) {
                ImGui::PushID(i);
                if (ImGui::Selectable(tourn_labels[i].c_str(),
                                      s_tourn_filter == i + 1))
                    s_tourn_filter = i + 1;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    }
    bool season_mode = (s_tourn_filter == 0);
    std::string sel_key;
    if (!season_mode)
        sel_key = tourn_labels[s_tourn_filter - 1] + year_suffix;
    VSpace(6);

    struct Row {
        vlr::PlayerPtr p; std::string team, agent;
        int m, k, d, a, fb, fd, rnd;
        double kd, rat, adr, hs, kast;
    };
    std::vector<Row> rows;
    rows.reserve(256);
    std::string q = s.search;
    for (auto& c : q) c = static_cast<char>(std::tolower((unsigned char)c));
    for (auto& kv : gm.leagues) {
        if (!kv.second) continue;
        for (auto& t : kv.second->teams()) {
            if (!t) continue;
            for (auto& p : t->roster) {
                if (!p) continue;
                if (season_mode) {
                    if (p->season_matches <= 0) continue;
                } else {
                    auto it = p->tourn_stats.find(sel_key);
                    if (it == p->tourn_stats.end() || it->second.maps <= 0)
                        continue;
                }
                if (!q.empty()) {
                    std::string nm = p->name;
                    for (auto& c : nm) c = static_cast<char>(std::tolower((unsigned char)c));
                    if (nm.find(q) == std::string::npos) continue;
                }
                Row r;
                r.p = p; r.team = t->name; r.agent = p->signature_agent();
                if (season_mode) {
                    r.m = p->season_matches;
                    r.k = p->season_kills; r.d = p->season_deaths; r.a = p->season_assists;
                    r.fb = p->season_fb; r.fd = p->season_fd; r.rnd = p->season_rounds;
                    r.kd  = p->season_deaths > 0 ? (double)p->season_kills / p->season_deaths
                                                 : (double)p->season_kills;
                    r.rat = p->season_rating_total / p->season_matches;
                    r.adr = p->season_rounds > 0 ? (double)p->season_damage / p->season_rounds : 0.0;
                    r.hs  = p->season_kills  > 0 ? 100.0 * p->season_hs_hits / p->season_kills : 0.0;
                    r.kast= p->season_rounds > 0 ? 100.0 * p->season_rounds_with_kast / p->season_rounds : 0.0;
                } else {
                    const vlr::Player::TournStatLine& ts = p->tourn_stats[sel_key];
                    r.m = ts.matches;
                    r.k = ts.kills; r.d = ts.deaths; r.a = ts.assists;
                    r.fb = ts.fb; r.fd = ts.fd; r.rnd = ts.rounds;
                    r.kd  = ts.deaths > 0 ? (double)ts.kills / ts.deaths
                                          : (double)ts.kills;
                    r.rat = ts.maps   > 0 ? ts.rating_total / ts.maps : 0.0;
                    r.adr = ts.rounds > 0 ? (double)ts.damage / ts.rounds : 0.0;
                    r.hs  = ts.kills  > 0 ? 100.0 * ts.hs_hits / ts.kills : 0.0;
                    r.kast= ts.rounds > 0 ? 100.0 * ts.rounds_with_kast / ts.rounds : 0.0;
                }
                rows.push_back(std::move(r));
            }
        }
    }

    if (rows.empty()) {
        ImGui::TextDisabled("(no matches played yet this season \xE2\x80\x94 check back "
                            "after the stage opens)");
        return;
    }

    enum Col { C_NAME, C_TEAM, C_AGENT, C_M, C_K, C_D, C_A, C_KD, C_RAT,
               C_ADR, C_HS, C_KAST, C_FK, C_FD, C_RND, C_N };
    if (ImGui::BeginTable("##seasonstats", C_N,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable |
            ImGuiTableFlags_SortMulti, ImVec2(-FLT_MIN, 560))) {
        ImGuiTableColumnFlags desc = ImGuiTableColumnFlags_PreferSortDescending;
        ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort);
        ImGui::TableSetupColumn("Team",   ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort);
        ImGui::TableSetupColumn("Agent",  ImGuiTableColumnFlags_NoSort);
        ImGui::TableSetupColumn("M",   desc);
        ImGui::TableSetupColumn("K",   desc);
        ImGui::TableSetupColumn("D",   desc);
        ImGui::TableSetupColumn("A",   desc);
        ImGui::TableSetupColumn("K/D", desc);
        ImGui::TableSetupColumn("Rating", desc | ImGuiTableColumnFlags_DefaultSort);
        ImGui::TableSetupColumn("ADR", desc);
        ImGui::TableSetupColumn("HS%", desc);
        ImGui::TableSetupColumn("KAST",desc);
        ImGui::TableSetupColumn("FK",  desc);
        ImGui::TableSetupColumn("FD",  desc);
        ImGui::TableSetupColumn("RND", desc);
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableHeadersRow();

        if (ImGuiTableSortSpecs* sd = ImGui::TableGetSortSpecs()) {
            if (sd->SpecsCount > 0) {
                const ImGuiTableColumnSortSpecs& sp = sd->Specs[0];
                bool asc = sp.SortDirection == ImGuiSortDirection_Ascending;
                auto cmp = [&](const Row& a, const Row& b) {
                    double va = 0, vb = 0;
                    switch (sp.ColumnIndex) {
                        case C_M:   va=a.m;   vb=b.m;   break;
                        case C_K:   va=a.k;   vb=b.k;   break;
                        case C_D:   va=a.d;   vb=b.d;   break;
                        case C_A:   va=a.a;   vb=b.a;   break;
                        case C_KD:  va=a.kd;  vb=b.kd;  break;
                        case C_RAT: va=a.rat; vb=b.rat; break;
                        case C_ADR: va=a.adr; vb=b.adr; break;
                        case C_HS:  va=a.hs;  vb=b.hs;  break;
                        case C_KAST:va=a.kast;vb=b.kast;break;
                        case C_FK:  va=a.fb;  vb=b.fb;  break;
                        case C_FD:  va=a.fd;  vb=b.fd;  break;
                        case C_RND: va=a.rnd; vb=b.rnd; break;
                        default:    va=a.rat; vb=b.rat; break;
                    }
                    return asc ? (va < vb) : (va > vb);
                };
                std::sort(rows.begin(), rows.end(), cmp);
            }
        }

        int ri = 0;
        for (auto& r : rows) {
            ImGui::PushID(ri++);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            CountryFlag(r.p->country_iso); ImGui::SameLine();
            if (ImGui::Selectable(player_label(*r.p).c_str(), false,
                    ImGuiSelectableFlags_SpanAllColumns)) {
                OpenPlayerModal(s, r.p);
            }
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.team.c_str());
            ImGui::TableNextColumn();
            if (!r.agent.empty()) {
                ImGui::PushFont(g_font_small);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
                ImGui::TextUnformatted(r.agent.c_str());
                ImGui::PopStyleColor(); ImGui::PopFont();
            } else ImGui::TextDisabled("-");
            ImGui::PushFont(g_font_mono);
            ImGui::TableNextColumn(); ImGui::Text("%d", r.m);
            ImGui::TableNextColumn(); ImGui::Text("%d", r.k);
            ImGui::TableNextColumn(); ImGui::Text("%d", r.d);
            ImGui::TableNextColumn(); ImGui::Text("%d", r.a);
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(
                r.kd >= 1.1 ? kVlrGreen : (r.kd < 0.9 ? kVlrRed : kVlrText)));
            ImGui::Text("%.2f", r.kd); ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(
                r.rat >= 1.1 ? kVlrGreen : (r.rat < 0.9 ? kVlrRed : kVlrText)));
            ImGui::Text("%.2f", r.rat); ImGui::PopStyleColor();
            ImGui::TableNextColumn(); ImGui::Text("%.0f", r.adr);
            ImGui::TableNextColumn(); ImGui::Text("%.0f%%", r.hs);
            ImGui::TableNextColumn(); ImGui::Text("%.0f%%", r.kast);
            ImGui::TableNextColumn(); ImGui::Text("%d", r.fb);
            ImGui::TableNextColumn(); ImGui::Text("%d", r.fd);
            ImGui::TableNextColumn(); ImGui::Text("%d", r.rnd);
            ImGui::PopFont();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void WealthTierBadge(vlr::WealthTier w) {
    ImU32 col;
    switch (w) {
        case vlr::WealthTier::SuperRich: col = IM_COL32(0x55, 0x42, 0x14, 0xFF); break;
        case vlr::WealthTier::Rich:      col = IM_COL32(0x14, 0x4A, 0x32, 0xFF); break;
        case vlr::WealthTier::Stable:    col = IM_COL32(0x33, 0x33, 0x33, 0xFF); break;
        case vlr::WealthTier::Modest:    col = IM_COL32(0x42, 0x38, 0x18, 0xFF); break;
        case vlr::WealthTier::Poor:      col = IM_COL32(0x55, 0x22, 0x22, 0xFF); break;
        default:                         col = kSurface; break;
    }
    Pill(vlr::wealth_tier_name(w), col, kVlrText);
}

void DrawFinanceDashboard(AppState& s) {
    auto& gm = s.gm;
    SectionHeader("FINANCE",
        "Every club's economy at a glance \xE2\x80\x94 income, payroll vs wage budget, "
        "projected balance, wealth tier, and the live transfer market.",
        kAccent);
    VSpace(6);

    // ---- Your club: the deepest breakdown ----
    if (gm.user_team) {
        auto t = gm.user_team;
        H2("YOUR CLUB"); ImGui::SameLine(); WealthTierBadge(t->wealth_tier);
        VSpace(4);
        StatTile("BUDGET",        fmt_money(t->budget),                kVlrGreen); ImGui::SameLine();
        StatTile("SPONSORSHIP",   fmt_money_k(t->sponsorship_k),       kVlrGold);  ImGui::SameLine();
        StatTile("PROJ. INCOME",  fmt_money_k(t->projected_income_k),  kInfo);     ImGui::SameLine();
        StatTile("COMMITTED",     fmt_money_k(t->committed_payroll_k), kAccent);
        StatTile("WAGE BUDGET",   fmt_money_k(t->wage_envelope_k),     kVlrGreen); ImGui::SameLine();
        StatTile("PAYROLL",       fmt_money_k(t->total_payroll_k()),   kAccent);   ImGui::SameLine();
        int net = t->net_transfer_k;
        StatTile("NET TRANSFERS", fmt_money_k(net), net >= 0 ? kVlrGreen : kVlrRed); ImGui::SameLine();
        int bal = t->projected_income_k - t->committed_payroll_k;
        StatTile("PROJ. BALANCE", fmt_money_k(bal), bal >= 0 ? kVlrGreen : kVlrRed);
        VSpace(10);

        // ---- Sponsor row ----
        // Surfaces the chosen preseason sponsor, its requirement, live
        // progress toward it, the reward, and whether it has been paid.
        if (t->sponsor_active) {
            if (BeginCard("##fin_sponsor")) {
                SectionHeader("ACTIVE SPONSOR", nullptr, kVlrGold);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                ImGui::TextUnformatted(t->sponsor_name.c_str());
                ImGui::PopStyleColor();

                // Requirement + live progress (current / target).
                int cur = 0, tgt = t->sponsor_req_value;
                const char* req_label = "Requirement";
                switch (t->sponsor_req_type) {
                    case vlr::SponsorReqType::Placement:
                    case vlr::SponsorReqType::TitleBerth: {
                        auto pl = gm.user_league_placement();
                        cur = pl.first;   // current league rank
                        req_label = (t->sponsor_req_type
                                     == vlr::SponsorReqType::Placement)
                                    ? "Finish <= "
                                    : "Reach playoff berth (<= ";
                        break;
                    }
                    case vlr::SponsorReqType::WinCount:
                        cur = t->wins;
                        req_label = "Wins >= ";
                        break;
                    case vlr::SponsorReqType::IndividualMilestone: {
                        // Best rostered season rating (rating_total/matches).
                        double best = 0.0;
                        for (auto& p : t->roster) {
                            if (!p || p->season_matches <= 0) continue;
                            double r = p->season_rating_total / p->season_matches;
                            if (r > best) best = r;
                        }
                        cur = static_cast<int>(best * 100.0 + 0.5);
                        req_label = "A player ends season rating >= ";
                        break;
                    }
                }
                if (t->sponsor_req_type == vlr::SponsorReqType::IndividualMilestone) {
                    MutedText("%s%.2f   \xE2\x80\x94   best so far: %.2f",
                              req_label, tgt / 100.0, cur / 100.0);
                } else if (t->sponsor_req_type == vlr::SponsorReqType::Placement) {
                    MutedText("%s%d   \xE2\x80\x94   currently #%d",
                              req_label, tgt, cur);
                } else if (t->sponsor_req_type == vlr::SponsorReqType::TitleBerth) {
                    MutedText("%s%d)   \xE2\x80\x94   currently #%d",
                              req_label, tgt, cur);
                } else {
                    MutedText("%s%d   \xE2\x80\x94   currently %d",
                              req_label, tgt, cur);
                }

                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGreen));
                ImGui::Text("REWARD: +$%dK", t->sponsor_reward_k);
                ImGui::PopStyleColor();
                ImGui::SameLine();
                if (t->sponsor_credited) {
                    Pill("PAID", kVlrGreen);
                } else {
                    Pill("pending", kAccent);
                }
            }
            EndCard();
        } else {
            MutedText("No active sponsor \xE2\x80\x94 pick one next preseason.");
        }
        VSpace(10);
    }

    // ---- League finance table (sorted by budget) ----
    H2("LEAGUE FINANCES");
    std::vector<vlr::TeamPtr> clubs;
    for (auto& kv : gm.leagues)
        for (auto& tm : kv.second->teams()) if (tm) clubs.push_back(tm);
    std::sort(clubs.begin(), clubs.end(),
              [](const vlr::TeamPtr& a, const vlr::TeamPtr& b) { return a->budget > b->budget; });
    if (ImGui::BeginTable("##fin_league", 8,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY,
            ImVec2(-FLT_MIN, 300))) {
        ImGui::TableSetupColumn("CLUB", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("WEALTH");
        ImGui::TableSetupColumn("BUDGET");
        ImGui::TableSetupColumn("SPONSOR");
        ImGui::TableSetupColumn("PAYROLL");
        ImGui::TableSetupColumn("WAGE BUD");
        ImGui::TableSetupColumn("NET XFER");
        ImGui::TableSetupColumn("PRES");
        ImGui::TableHeadersRow();
        int ri = 0;
        for (auto& tm : clubs) {
            ImGui::PushID(ri++);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool is_user = (tm == gm.user_team);
            if (is_user) ImGui::PushStyleColor(ImGuiCol_Text, toV4(kAccent));
            ImGui::TextUnformatted(tm->name.c_str());
            if (is_user) ImGui::PopStyleColor();
            ImGui::TableNextColumn(); WealthTierBadge(tm->wealth_tier);
            ImGui::PushFont(g_font_mono);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(fmt_money(tm->budget).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(fmt_money_k(tm->sponsorship_k).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(fmt_money_k(tm->total_payroll_k()).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(fmt_money_k(tm->wage_envelope_k).c_str());
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(tm->net_transfer_k >= 0 ? kVlrGreen : kVlrRed));
            ImGui::TextUnformatted(fmt_money_k(tm->net_transfer_k).c_str());
            ImGui::PopStyleColor();
            ImGui::TableNextColumn(); ImGui::Text("%d", tm->prestige);
            ImGui::PopFont();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // ---- Transfer market log (buyer-side records, most recent first) ----
    VSpace(10);
    H2("TRANSFER MARKET");
    Sub("Recent club-to-club moves across the league. Free signings shown too.");
    struct Xfer { int year; std::string player, from, to; int fee; };
    std::vector<Xfer> xfers;
    for (auto& kv : gm.leagues)
        for (auto& tm : kv.second->teams()) {
            if (!tm) continue;
            for (auto& r : tm->transfer_log_) {
                if (r.to_team != tm->name) continue;   // dedup: take the buyer's copy only
                xfers.push_back({ r.year, r.player, r.from_team, r.to_team, r.fee_k });
            }
        }
    std::sort(xfers.begin(), xfers.end(), [](const Xfer& a, const Xfer& b) { return a.year > b.year; });
    if (xfers.empty()) {
        ImGui::TextDisabled("(no transfers yet \xE2\x80\x94 play a season)");
    } else if (ImGui::BeginTable("##fin_xfer", 5,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY,
            ImVec2(-FLT_MIN, 260))) {
        ImGui::TableSetupColumn("YEAR");
        ImGui::TableSetupColumn("PLAYER", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("FROM", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("TO", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("FEE");
        ImGui::TableHeadersRow();
        int ri = 0;
        for (auto& x : xfers) {
            if (ri > 200) break;
            ImGui::PushID(ri++);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%d", x.year); ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(x.player.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(x.from.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(x.to.c_str());
            ImGui::TableNextColumn();
            if (x.fee > 0) { ImGui::PushFont(g_font_mono); ImGui::TextUnformatted(fmt_money_k(x.fee).c_str()); ImGui::PopFont(); }
            else ImGui::TextDisabled("free");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void DrawAwards(AppState& s) {
    SectionHeader("AWARDS",
        "Live MVP race + individual award races, league leaders, the season "
        "awards recap, and all-time history \xE2\x80\x94 every honor in one place.",
        kAccent);
    VSpace(6);
    if (ImGui::BeginTabBar("##awardstabs")) {
        if (ImGui::BeginTabItem("MVP Race"))       { DrawMvpRaceBody(s);       ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("League Leaders")) { DrawLeagueLeaders(s);     ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Season Awards"))  { DrawAwardsRecapBody(s);   ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Awards History")) { DrawAwardsHistoryBody(s); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
}

#include "gui_favorites.inc"
#include "gui_watchlist.inc"

// ===== League Leaders ====================================================
//
// Sortable leaderboards for the most common pro Valorant rate stats.
// Filters: region, min matches threshold. The active leaderboard category
// is selected via a tab bar.
void DrawLeagueLeaders(AppState& s) {
    ImGui::PushFont(g_font_h1);
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
    ImGui::TextUnformatted("LEAGUE LEADERS");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    MutedText("Career rate stats. Filter by region and min match count.");
    VSpace(12);

    // Filter row.
    const char* regions[] = {"All Regions", "Americas", "EMEA", "Pacific"};
    ImGui::SetNextItemWidth(180);
    ImGui::Combo("Region", &s.ll_region_filter, regions, IM_ARRAYSIZE(regions));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180);
    ImGui::SliderInt("Min matches", &s.ll_min_matches, 1, 100);

    // Collect candidate players.
    std::string region_str;
    if (s.ll_region_filter > 0 && s.ll_region_filter < (int)IM_ARRAYSIZE(regions))
        region_str = regions[s.ll_region_filter];

    std::vector<vlr::PlayerPtr> all;
    for (auto& kv : s.gm.solo_qs) {
        if (!region_str.empty() && kv.first != region_str) continue;
        for (auto& p : kv.second->global_ladder()) {
            if (!p) continue;
            if (p->career_matches < s.ll_min_matches) continue;
            all.push_back(p);
        }
    }

    // Category tabs — each defines a sort key + display formatter.
    struct Category {
        const char* label;
        const char* unit;
        std::function<double(const vlr::Player&)> value;
        std::function<std::string(double)> format;
    };
    auto fmt2 = [](double v) {
        char b[32]; std::snprintf(b, sizeof(b), "%.2f", v); return std::string(b);
    };
    auto fmt1 = [](double v) {
        char b[32]; std::snprintf(b, sizeof(b), "%.1f", v); return std::string(b);
    };
    auto fmtPct = [](double v) {
        char b[32]; std::snprintf(b, sizeof(b), "%.1f%%", v); return std::string(b);
    };
    auto fmtInt = [](double v) {
        char b[32]; std::snprintf(b, sizeof(b), "%d", (int)v); return std::string(b);
    };

    std::vector<Category> cats = {
        {"Rating",   "RAT",  [](const vlr::Player& p) { return p.avg_match_rating(); }, fmt2},
        {"K/D",      "K/D",  [](const vlr::Player& p) { return p.kd_ratio();       }, fmt2},
        {"ADR",      "ADR",  [](const vlr::Player& p) { return p.career_adr();      }, fmt1},
        {"KAST",     "%",    [](const vlr::Player& p) { return p.career_kast_pct(); }, fmtPct},
        {"Kills",    "K",    [](const vlr::Player& p) { return (double)p.career_kills; }, fmtInt},
        {"HS%",      "%",    [](const vlr::Player& p) { return p.career_hs_pct();   }, fmtPct},
        {"Clutch",   "PTS",  [](const vlr::Player& p) {
            return (double)(p.career_kills - p.career_deaths) * (p.kd_ratio() > 1.0 ? 1.0 : 0.5);
        }, fmtInt},
        {"MVPs",     "MVP",  [](const vlr::Player& p) { return (double)p.career_mvps; }, fmtInt},
        {"Entry %",  "%",    [](const vlr::Player& p) { return p.career_entry_pct();}, fmtPct},
        {"Assists",  "APR",  [](const vlr::Player& p) { return p.career_apr();      }, fmt2},
        // IGL Impact — only IGL-flagged players score here. Sorted by current
        // season impact; non-IGLs naturally drop to the bottom (value=0).
        {"IGL Impact", "IMP", [](const vlr::Player& p) {
            return p.is_igl ? p.igl_impact_season : 0.0;
        }, fmt2},
        // Flex Rating — only is_flex players score here. Sorted by current
        // season rating; non-Flex players drop to the bottom (value=0).
        // Matches the IGL Impact pattern so the Position filter is uniform.
        {"Flex Rating", "RAT", [](const vlr::Player& p) {
            if (!p.is_flex || p.season_matches <= 0) return 0.0;
            return p.season_rating_total / (double)p.season_matches;
        }, fmt2},
    };

    if (ImGui::BeginTabBar("##ll_cats")) {
        for (int ci = 0; ci < (int)cats.size(); ++ci) {
            if (!ImGui::BeginTabItem(cats[ci].label)) continue;
            s.ll_category = ci;

            // Sort by category value descending.
            auto& cat = cats[ci];
            auto rows = all;
            std::sort(rows.begin(), rows.end(),
                      [&](const vlr::PlayerPtr& a, const vlr::PlayerPtr& b) {
                          return cat.value(*a) > cat.value(*b);
                      });
            if ((int)rows.size() > 100) rows.resize(100);

            ImGui::Spacing();
            if (ImGui::BeginTable("##ll_table", 7,
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_Sortable |
                    ImGuiTableFlags_SortMulti |
                    ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, 540))) {
                ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_NoSort);
                ImGui::TableSetupColumn("FLAG", ImGuiTableColumnFlags_NoSort);
                ImGui::TableSetupColumn("PLAYER",
                    ImGuiTableColumnFlags_WidthStretch, 0, 2);
                ImGui::TableSetupColumn("ROLE", ImGuiTableColumnFlags_NoSort);
                ImGui::TableSetupColumn("TEAM", ImGuiTableColumnFlags_None, 0, 4);
                ImGui::TableSetupColumn("MATCHES",
                    ImGuiTableColumnFlags_PreferSortDescending, 0, 5);
                ImGui::TableSetupColumn(cat.unit,
                    ImGuiTableColumnFlags_DefaultSort |
                    ImGuiTableColumnFlags_PreferSortDescending, 0, 6);
                ImGui::TableHeadersRow();
                if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs()) {
                    if (sp->SpecsDirty && sp->SpecsCount > 0) {
                        const ImGuiTableColumnSortSpecs& c = sp->Specs[0];
                        bool asc =
                            (c.SortDirection == ImGuiSortDirection_Ascending);
                        std::sort(rows.begin(), rows.end(),
                            [&](const vlr::PlayerPtr& a,
                                const vlr::PlayerPtr& b) {
                                switch (c.ColumnUserID) {
                                case 2: return asc ? (a->name < b->name)
                                                   : (a->name > b->name);
                                case 4: return asc
                                    ? (a->team_name < b->team_name)
                                    : (a->team_name > b->team_name);
                                case 5: return asc
                                    ? (a->career_matches < b->career_matches)
                                    : (a->career_matches > b->career_matches);
                                case 6:
                                default: return asc
                                    ? (cat.value(*a) < cat.value(*b))
                                    : (cat.value(*a) > cat.value(*b));
                                }
                            });
                        sp->SpecsDirty = false;
                    }
                }
                int rank = 1;
                for (auto& p : rows) {
                    ImGui::PushID(rank);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::PushStyleColor(ImGuiCol_Text, toV4(placement_color(rank)));
                    ImGui::PushFont(g_font_mono);
                    ImGui::Text("#%d", rank);
                    ImGui::PopFont();
                    ImGui::PopStyleColor();
                    ImGui::TableNextColumn(); CountryFlag(p->country_iso);
                    ImGui::TableNextColumn();
                    if (TableRowSelectable(player_label(*p).c_str(), false,
                            ImGuiSelectableFlags_SpanAllColumns)) {
                        OpenPlayerModal(s, p);
                    }
                    // Signature agent inline chip (same helper as Roster /
                    // Market / Team Profile). Empty = no-op.
                    ImGui::SameLine();
                    SigBadge(*p);
                    ImGui::TableNextColumn();
                    PositionBadge(vlr::position_of(*p));
                    if (p->is_igl) { ImGui::SameLine(); IglBadge(); }
                    ImGui::TableNextColumn(); ImGui::Text("%s", p->team_name.c_str());
                    ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%d", p->career_matches); ImGui::PopFont();
                    ImGui::TableNextColumn();
                    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                    ImGui::PushFont(g_font_mono);
                    ImGui::Text("%s", cat.format(cat.value(*p)).c_str());
                    ImGui::PopFont();
                    ImGui::PopStyleColor();
                    ImGui::PopID();
                    ++rank;
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

// ============================================================
// 9b. MANAGER screen — Player Dev / Coach Mgmt / Finance subtabs
// ============================================================
#include "gui_manager.inc"

// ============================================================
// 9c. NEWS feed — Breaking news cards (roster moves, awards, MVP race)
// ============================================================
void DrawNews(AppState& s) {
    ImGui::PushFont(g_font_h1);
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
    ImGui::TextUnformatted("BREAKING NEWS");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    MutedText("Roster moves, coach decisions, awards, and the MVP race — as they "
        "happen across the league. Most recent first.");
    VSpace(12);

    // Category filter — chips derived LIVE from the categories actually present
    // in the feed, so the dropdown always matches reality: no dead options, and
    // any new engine category (Breakout, Upset, Dynasty, ...) shows up on its
    // own. Stored as a string because the available set grows as news accrues.
    auto has_cat = [](const std::vector<std::string>& v, const std::string& x) {
        for (auto& e : v) if (e == x) return true;
        return false;
    };
    static std::string news_filter = "All";
    std::vector<std::string> cats;
    cats.reserve(18);
    cats.push_back("All");
    for (auto& item : s.gm.news_feed)
        if (!has_cat(cats, item.category)) cats.push_back(item.category);
    // If the previously-selected category has aged out of the feed, reset to All.
    if (!has_cat(cats, news_filter)) news_filter = "All";
    ImGui::SetNextItemWidth(180);
    if (ImGui::BeginCombo("Filter", news_filter.c_str())) {
        for (auto& c : cats) {
            bool sel = (c == news_filter);
            if (ImGui::Selectable(c.c_str(), sel)) news_filter = c;
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::Spacing();

    if (s.gm.news_feed.empty()) {
        ImGui::TextDisabled("(no news yet — advance time and watch the league react)");
        return;
    }

    // Per-frame signature-agent lookup: build ONCE here instead of walking
    // all solo Q ladders for every news item (was ~540k string compares per
    // frame at full feed × 2700 players). Trades 1× walk for N× walks.
    std::unordered_map<std::string, std::string> sig_lookup;
    for (auto& kv : s.gm.solo_qs) {
        if (!kv.second) continue;
        for (auto& pp : kv.second->global_ladder()) {
            if (!pp) continue;
            std::string sig = pp->signature_agent();
            if (!sig.empty()) sig_lookup.emplace(pp->name, std::move(sig));
        }
    }

    int ni = 0;
    for (auto& item : s.gm.news_feed) {
        if (news_filter != "All" && item.category != news_filter) continue;
        ImGui::PushID(ni++);

        // Color-coded category pill. Covers every category the engine emits:
        // gold = prestige/achievement, green = positive form, red = negative/
        // attrition, purple = transfers/gossip, accent = events/milestones.
        ImU32 cat_col = kVlrSub;
        if      (item.category == "Roster Move")      cat_col = kAccent2;
        else if (item.category == "Rumor")            cat_col = kAccent2;
        else if (item.category == "Awards")           cat_col = kVlrGold;
        else if (item.category == "Coach of the Year") cat_col = kVlrGold;
        else if (item.category == "Hot Streak")       cat_col = kVlrGold;
        else if (item.category == "Upset")            cat_col = kVlrGold;
        else if (item.category == "Historic")         cat_col = kVlrGold;
        else if (item.category == "Dynasty")          cat_col = kVlrGold;
        else if (item.category == "MVP Race")         cat_col = kVlrGreen;
        else if (item.category == "Breakout")         cat_col = kVlrGreen;
        else if (item.category == "Prospect")         cat_col = kVlrGreen;
        else if (item.category == "Tournament")       cat_col = kAccent;
        else if (item.category == "Milestone")        cat_col = kAccent;
        else if (item.category == "Result")           cat_col = kAccent;
        else if (item.category == "Slump")            cat_col = kVlrRed;
        else if (item.category == "Retirement Watch") cat_col = kVlrRed;
        else if (item.category == "Retirement")       cat_col = kVlrGold;
        else if (item.category == "Rivalry")          cat_col = kVlrRed;

        if (BeginCard("##news_card")) {
        // Header row: category pill (left) + faint date (right).
        Pill(item.category.c_str(), cat_col);
        {
            char db[48];
            std::snprintf(db, sizeof(db), "Year %d  -  Day %d",
                          item.year, item.day_in_year);
            float tw = ImGui::CalcTextSize(db).x;
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - tw);
            ImGui::AlignTextToFramePadding();
            if (g_font_small) ImGui::PushFont(g_font_small);
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kTextFaint));
            ImGui::TextUnformatted(db);
            ImGui::PopStyleColor();
            if (g_font_small) ImGui::PopFont();
        }
        VSpace(4);
        ImGui::PushFont(g_font_h2);
        // Phase 1 (Agent C) — prepend a small gold [*] chip when the
        // referenced player has a signature agent. Look up by player_name.
        std::string sig_news;
        if (!item.player_name.empty()) {
            auto it = sig_lookup.find(item.player_name);
            if (it != sig_lookup.end()) sig_news = it->second;
        }
        if (!sig_news.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
            ImGui::TextUnformatted("[*]");
            ImGui::PopStyleColor();
            ImGui::SameLine();
        }
        ImGui::TextWrapped("%s", item.headline.c_str());
        ImGui::PopFont();
        if (!sig_news.empty()) {
            ImGui::PushFont(g_font_small);
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
            ImGui::Text("(Signature %s main)", sig_news.c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }

        VSpace(4);
        if (g_font_small) ImGui::PushFont(g_font_small);
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
        ImGui::TextWrapped("%s", item.body.c_str());
        ImGui::PopStyleColor();
        if (g_font_small) ImGui::PopFont();

        } EndCard();
        ImGui::PopID();
        VSpace(10);
    }
}

// === Mail deep-link resolution (never-free safe) =======================
// Mail stores player_name STRINGS, not pointers. At click time we resolve the
// name against the LIVE world; if the player retired or left every container
// the resolver returns nullptr and the action no-ops — the mailbox can outlive
// any entity it references without ever dangling.
//
// EXACT resolution by stable Player::id. Gamertags are NOT unique (namesakes
// exist, and retired players persist forever), so name-only resolution opened
// the WRONG player whenever an active namesake shadowed the mail's true (often
// retired) subject — exactly the case for HoF / retirement mail. Resolve by id
// first; works for retired players too (scans the whole world, no is_retired
// preference). Returns nullptr for id 0 (legacy/non-player mail).
vlr::PlayerPtr resolve_player_by_id(AppState& s, std::uint64_t id) {
    if (id == 0) return nullptr;
    for (auto& p : all_players_world(s)) {
        if (p && p->id == id) return p;
    }
    return nullptr;
}
vlr::PlayerPtr resolve_player_by_name(AppState& s, const std::string& name) {
    if (name.empty()) return nullptr;
    vlr::PlayerPtr fallback;
    for (auto& p : all_players_world(s)) {
        if (!p || p->name != name) continue;
        if (!p->is_retired) return p;   // prefer an active match
        if (!fallback) fallback = p;    // else remember a retired namesake
    }
    return fallback;
}
// Prefer the stable id; fall back to name only for legacy mail (player_id==0).
vlr::PlayerPtr resolve_mail_player(AppState& s, const vlr::GameManager::MailItem& m) {
    if (auto p = resolve_player_by_id(s, m.player_id)) return p;
    return resolve_player_by_name(s, m.player_name);
}
bool player_on_user_roster(AppState& s, const vlr::PlayerPtr& p) {
    auto t = s.gm.user_team;
    if (!t || !p) return false;
    for (auto& rp : t->roster) if (rp == p) return true;
    return false;
}

void DrawInbox(AppState& s) {
    using Cat = vlr::GameManager::MailCategory;
    using Lnk = vlr::GameManager::MailLink;
    using Mail = vlr::GameManager::MailItem;
    auto& gm = s.gm;

    // Category metadata (label + accent), indexed by (int)MailCategory.
    struct CatMeta { const char* label; ImU32 col; };
    static const CatMeta kCat[] = {
        {"Board",    kAccent },   // objectives / board verdicts
        {"Transfer", kAccent2},   // market moves
        {"Contract", kVlrBlue},   // re-sign / expiry / walk
        {"Award",    kVlrGold},   // your players' trophies
        {"Squad",    kVlrGreen},  // morale / breakout / retirement
        {"Result",   kVlrRed },   // tournament results / upsets
        {"Media",    kVlrSub },   // milestones / dynasty / sponsor press
    };
    static_assert(IM_ARRAYSIZE(kCat) == static_cast<int>(Cat::COUNT),
                  "kCat must have exactly one entry per MailCategory");
    const int kCatCount = static_cast<int>(Cat::COUNT);  // 7

    SectionHeader("Inbox",
        "Club mail — board directives, transfers, contracts, awards, and results "
        "for your organization. Most recent first.");

    int unread = gm.unread_mail_count();

    // Header action row.
    {
        char hb[64];
        std::snprintf(hb, sizeof(hb), "%d unread of %d", unread,
                      static_cast<int>(gm.mailbox.size()));
        ImGui::AlignTextToFramePadding();
        Sub("%s", hb);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Mark all read")) {
        for (auto& m : gm.mailbox) m.read = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear read")) {
        // Drop read && !important items; the reading pane re-validates its
        // selection by id below, so a cleared selection just empties the pane.
        gm.mailbox.erase(std::remove_if(gm.mailbox.begin(), gm.mailbox.end(),
            [](const Mail& m) { return m.read && !m.important; }),
            gm.mailbox.end());
    }

    // Filter chips: All + per-category.
    VSpace(6);
    auto chip = [&](const char* label, int cat_value, ImU32 col) {
        bool sel = (s.mail_filter == cat_value);
        ImGui::PushStyleColor(ImGuiCol_Button,
                              sel ? toV4(col) : toV4(kSurfaceAlt));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(col));
        ImGui::PushStyleColor(ImGuiCol_Text,
                              sel ? toV4(IM_COL32(0x12,0x12,0x14,0xFF)) : toV4(kVlrSub));
        if (ImGui::SmallButton(label)) s.mail_filter = cat_value;
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
    };
    chip("All", -1, kAccent);
    for (int c = 0; c < kCatCount; ++c) chip(kCat[c].label, c, kCat[c].col);
    ImGui::NewLine();

    ImGui::Checkbox("Unread only", &s.mail_unread_only);
    ImGui::SameLine();
    ImGui::Checkbox("Important only", &s.mail_important_only);
    VSpace(8);

    auto passes = [&](const Mail& m) {
        if (s.mail_filter >= 0 && static_cast<int>(m.category) != s.mail_filter) return false;
        if (s.mail_unread_only && m.read) return false;
        if (s.mail_important_only && !m.important) return false;
        return true;
    };

    // === Split layout: LEFT list (fixed) | RIGHT reading pane ===
    const float listW = 360.0f;
    ImGui::BeginChild("##mail_list", ImVec2(listW, 0), ImGuiChildFlags_Border);
    if (gm.mailbox.empty()) {
        MutedText("No mail yet. As your season unfolds — board objectives, "
                  "transfers, awards, and results land here.");
    } else {
        int shown = 0;
        for (auto& m : gm.mailbox) {
            if (!passes(m)) continue;
            ++shown;
            ImGui::PushID(m.id);
            const CatMeta& cm = kCat[static_cast<int>(m.category)];
            bool selected = (s.selected_mail_id == m.id);

            ImVec2 rp = ImGui::GetCursorScreenPos();
            const float rowH = 46.0f;
            if (ImGui::Selectable("##row", selected, ImGuiSelectableFlags_None,
                                  ImVec2(0, rowH))) {
                s.selected_mail_id = m.id;
                m.read = true;   // opening marks read
            }
            ImDrawList* dl = ImGui::GetWindowDrawList();
            // Category color bar (left edge).
            dl->AddRectFilled(ImVec2(rp.x, rp.y + 3), ImVec2(rp.x + 3, rp.y + rowH - 3),
                              cm.col, 1.5f);
            float tx = rp.x + 12.0f;
            if (!m.read) {
                dl->AddCircleFilled(ImVec2(tx + 3.0f, rp.y + 13.0f), 3.5f, kVlrRed);
            }
            float subx = tx + (m.read ? 0.0f : 12.0f);
            std::string subj = (m.important ? std::string("\xE2\x98\x85 ") : std::string()) + m.subject;
            if (subj.size() > 40) {
                size_t cut = 39;
                // Back off so we never split a multi-byte UTF-8 codepoint
                // (accented player names) into a broken glyph before the ellipsis.
                while (cut > 0 && (static_cast<unsigned char>(subj[cut]) & 0xC0) == 0x80) --cut;
                subj = subj.substr(0, cut) + "\xE2\x80\xA6";
            }
            dl->AddText(ImVec2(subx, rp.y + 6.0f),
                        m.read ? kVlrSub : kVlrText, subj.c_str());
            char meta[96];
            std::snprintf(meta, sizeof(meta), "%s  -  Yr %d, Day %d",
                          cm.label, m.year, m.day);
            if (g_font_small) {
                dl->AddText(g_font_small, g_font_small->FontSize,
                            ImVec2(tx, rp.y + 26.0f), kTextFaint, meta);
            } else {
                dl->AddText(ImVec2(tx, rp.y + 26.0f), kTextFaint, meta);
            }
            ImGui::PopID();
        }
        if (shown == 0) MutedText("No mail matches this filter.");
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // === RIGHT: reading pane ===
    ImGui::BeginChild("##mail_read", ImVec2(0, 0), ImGuiChildFlags_Border);
    Mail* sel = nullptr;
    for (auto& m : gm.mailbox) if (m.id == s.selected_mail_id) { sel = &m; break; }
    if (!sel) {
        s.selected_mail_id = -1;  // selection was deleted / cap-dropped
        VSpace(20);
        MutedText("Select a message from the list to read it.");
    } else {
        const CatMeta& cm = kCat[static_cast<int>(sel->category)];
        Pill(cm.label, cm.col);
        ImGui::SameLine();
        if (sel->important) { Pill("Important", kVlrRed); ImGui::SameLine(); }
        {
            char db[48];
            std::snprintf(db, sizeof(db), "Season %d, Day %d", sel->year, sel->day);
            float tw = ImGui::CalcTextSize(db).x;
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - tw);
            if (g_font_small) ImGui::PushFont(g_font_small);
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kTextFaint));
            ImGui::TextUnformatted(db);
            ImGui::PopStyleColor();
            if (g_font_small) ImGui::PopFont();
        }
        VSpace(8);
        H2(sel->subject.c_str(), kVlrText);
        VSpace(8);
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
        ImGui::TextWrapped("%s", sel->body.c_str());
        ImGui::PopStyleColor();

        if (sel->amount_k != 0) {
            VSpace(10);
            StatTile("Amount", fmt_money_k(sel->amount_k), cm.col);
        }

        VSpace(14);
        ThinDivider(6);
        VSpace(6);

        // === Deep-link action(s): resolve by stable ID at click time ===
        // (id-first, name only as a legacy fallback — see resolve_mail_player)
        vlr::PlayerPtr who = resolve_mail_player(s, *sel);
        bool drew_action = false;
        switch (sel->link) {
            case Lnk::Player:
                if (!sel->player_name.empty()) {
                    if (who) {
                        if (ImGui::Button("View Player")) OpenPlayerModal(s, who);
                    } else {
                        MutedText("%s is no longer active.", sel->player_name.c_str());
                    }
                    drew_action = true;
                }
                break;
            case Lnk::Negotiation:
                if (player_on_user_roster(s, who)) {
                    if (ImGui::Button("Open Negotiation")) {
                        s.screen = Screen::Roster;     // leaf that hosts the modal
                        s.show_resign_modal = true;
                        s.resign_target = who;
                    }
                } else {
                    // Player already left talks / the roster -> market fallback.
                    if (ImGui::Button("Go to Market")) s.screen = Screen::Market;
                    ImGui::SameLine();
                    MutedText(who ? "no longer on your roster"
                                  : "player no longer active");
                }
                drew_action = true;
                break;
            case Lnk::Market:
                if (ImGui::Button("Go to Market")) s.screen = Screen::Market;
                drew_action = true;
                break;
            case Lnk::Manager:
                if (ImGui::Button("Open Manager")) s.screen = Screen::Manager;
                drew_action = true;
                break;
            case Lnk::Finance:
                if (ImGui::Button("Open Finance")) s.screen = Screen::GroupFinance;
                drew_action = true;
                break;
            case Lnk::Roster:
                if (ImGui::Button("Open Roster")) s.screen = Screen::Roster;
                drew_action = true;
                break;
            case Lnk::None:
            default:
                break;
        }
        // If the primary link wasn't a player action but the mail still names a
        // (live) player, offer a secondary View Player.
        if (sel->link != Lnk::Player && sel->link != Lnk::Negotiation && who) {
            if (drew_action) ImGui::SameLine();
            if (ImGui::Button("View Player")) OpenPlayerModal(s, who);
        }

        // Mail-management row.
        VSpace(10);
        if (ImGui::SmallButton(sel->read ? "Mark unread" : "Mark read"))
            sel->read = !sel->read;
        ImGui::SameLine();
        if (ImGui::SmallButton(sel->important ? "Unstar" : "Star"))
            sel->important = !sel->important;
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete")) {
            int del_id = sel->id;
            gm.mailbox.erase(std::remove_if(gm.mailbox.begin(), gm.mailbox.end(),
                [del_id](const Mail& m) { return m.id == del_id; }),
                gm.mailbox.end());
            s.selected_mail_id = -1;
            sel = nullptr;   // must not be used after erase
        }
    }
    ImGui::EndChild();
}

void DrawCurrentScreen(AppState& s) {
    ImGui::BeginChild("##content", ImVec2(0, 0), ImGuiChildFlags_Border);
    switch (s.screen) {
        case Screen::Dashboard:   DrawDashboard(s); break;
        case Screen::Roster:      DrawRoster(s); break;
        case Screen::Standings:   DrawStandings(s); break;
        case Screen::Brackets:    DrawBrackets(s); break;
        case Screen::Market:      DrawMarket(s); break;
        case Screen::SoloQ:       DrawSoloQ(s); break;
        case Screen::EventLog:    DrawEventLog(s); break;
        case Screen::Watch:       DrawWatch(s); break;
        case Screen::Calendar:    DrawCalendar(s); break;
        case Screen::Favorites:   DrawFavorites(s); break;
        case Screen::TeamProfile: DrawTeamProfile(s); break;
        case Screen::LeagueLeaders:DrawLeagueLeaders(s); break;
        case Screen::Manager:     DrawManager(s); break;
        case Screen::News:        DrawNews(s); break;
        case Screen::PowerRankings: DrawPowerRankings(s); break;
        case Screen::Compare:     DrawCompare(s); break;

        // === Pack C C7 grouped routes ============================
        // Each group renders a top-level tab bar and dispatches the
        // active tab to the existing leaf Draw* function. Routing
        // only — no rewrites. Deep-link routes (Watch, TeamProfile,
        // Brackets) still set leaf Screen values via OpenLiveMatch /
        // OpenTeamProfile and continue to land on those cases above.
        case Screen::GroupHome: {
            if (ImGui::BeginTabBar("##grp_home_tabs")) {
                if (ImGui::BeginTabItem("Dashboard")) {
                    DrawDashboard(s); ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("News")) {
                    DrawNews(s); ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            break;
        }
        case Screen::GroupTeam: {
            if (ImGui::BeginTabBar("##grp_team_tabs")) {
                if (ImGui::BeginTabItem("Roster")) {
                    DrawRoster(s); ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Manager")) {
                    DrawManager(s); ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Strategy")) {
                    DrawStrategy(s); ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Calendar")) {
                    DrawCalendar(s); ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            break;
        }
        case Screen::GroupCompetition: {
            if (ImGui::BeginTabBar("##grp_comp_tabs")) {
                if (ImGui::BeginTabItem("Standings")) {
                    DrawStandings(s); ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Tournaments")) {
                    DrawBrackets(s); ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Player Stats")) {
                    DrawSeasonStats(s); ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Power Rankings")) {
                    DrawPowerRankings(s); ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            break;
        }
        case Screen::GroupPeople: {
            if (ImGui::BeginTabBar("##grp_people_tabs")) {
                if (ImGui::BeginTabItem("Transfer Market")) {
                    DrawMarket(s); ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Solo Q")) {
                    DrawSoloQ(s); ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Compare")) {
                    DrawCompare(s); ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Watchlist")) {
                    DrawWatchlist(s); ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            break;
        }
        case Screen::GroupHistory: {
            if (ImGui::BeginTabBar("##grp_history_tabs")) {
                if (ImGui::BeginTabItem("Favorites")) {
                    DrawFavorites(s); ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Event Log")) {
                    DrawEventLog(s); ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            break;
        }
        case Screen::GroupLive: {
            if (ImGui::BeginTabBar("##grp_live_tabs")) {
                if (ImGui::BeginTabItem("Watch")) {
                    DrawWatch(s); ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            break;
        }
        case Screen::GroupAwards: {
            // DrawAwards builds its own SectionHeader + tab bar (MVP Race,
            // League Leaders, Season Awards, Awards History).
            DrawAwards(s);
            break;
        }
        case Screen::GroupFinance: {
            // DrawFinanceDashboard builds its own SectionHeader + panels (your
            // club, league finances, transfer market).
            DrawFinanceDashboard(s);
            break;
        }
        case Screen::GroupMail: {
            // DrawInbox builds its own split list / reading-pane layout.
            DrawInbox(s);
            break;
        }
    }
    ImGui::EndChild();
}

}  // namespace

// ============================================================
// 10. Sidebar + entrypoint
// ============================================================
namespace {

// Process a DayResult on the UI thread (touches ImGui state via the live
// viewer open helpers). Used by both the direct-call path (legacy) and the
// async-worker poll path (main loop in wWinMain).
void process_day_result(AppState& s, const vlr::DayResult& r) {
    if (!r.user_match_series.empty()) {
        OpenReplaySeries(s, r.user_match_series);
        char b[180];
        std::snprintf(b, sizeof(b), "[Day %d] Match day vs %s — opening live viewer.",
                      r.day_in_year + 1, r.user_match_opp_name.c_str());
        s.log.emplace_back(b);
    } else if (r.user_match_recording) {
        OpenReplay(s, r.user_match_recording);
        char b[160];
        std::snprintf(b, sizeof(b), "[Day %d] Match day vs %s — opening live viewer.",
                      r.day_in_year + 1, r.user_match_opp_name.c_str());
        s.log.emplace_back(b);
    }
    if (r.progression_ran) {
        char b[128];
        std::snprintf(b, sizeof(b), "[Day %d] Monthly player development pass.", r.day_in_year + 1);
        s.log.emplace_back(b);
    }
    if (r.year_rolled) {
        s.log.emplace_back("== New season begins ==");
    }
}

// Spawn the engine tick on a worker thread so the UI thread keeps presenting
// frames at 60fps. Caller passes a body that runs the simulation. The body
// MUST NOT touch any ImGui state (single-thread invariant) — it should only
// mutate engine state via s.gm. UI thread joins + drains pending_results in
// the main wWinMain loop. Returns immediately if a sim is already running.
template <class F>
void launch_async_sim(AppState& s, const char* status, F&& body) {
    if (s.sim_running.load()) return;
    if (s.sim_thread.joinable()) s.sim_thread.join();  // belt-and-braces
    s.sim_status = status;
    s.pending_results.clear();
    s.sim_progress_total = 0;
    s.sim_progress_done  = 0;
    s.sim_running.store(true);
    s.sim_thread = std::thread([&s, body = std::forward<F>(body)]() mutable {
        body();
        s.sim_running.store(false);
    });
}

// Called from wWinMain every frame. If the worker has finished, join the
// thread and process any DayResult(s) it produced (opens live viewer modal,
// pushes log lines, etc. — all UI-thread work).
void poll_async_sim(AppState& s) {
    if (s.sim_thread.joinable() && !s.sim_running.load()) {
        s.sim_thread.join();
        for (const auto& r : s.pending_results) process_day_result(s, r);
        s.pending_results.clear();
        s.sim_status.clear();
    }
}

// Full-screen "Simulating..." overlay shown while a worker is in flight.
// Renders only ImGui primitives — touches NO engine state — so it can run
// while the worker mutates s.gm safely.
void DrawSimOverlay(AppState& s) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0,0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.03f, 0.04f, 0.06f, 0.92f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##sim_overlay", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoInputs);
    // Centered card.
    float card_w = 420.0f, card_h = 180.0f;
    ImVec2 c{(io.DisplaySize.x - card_w) * 0.5f, (io.DisplaySize.y - card_h) * 0.5f};
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(c, ImVec2(c.x + card_w, c.y + card_h), kSurface, 14.0f);
    dl->AddRect(c, ImVec2(c.x + card_w, c.y + card_h), kAccent, 14.0f, 0, 1.5f);
    // Animated spinner (small rotating arc).
    float t = (float)ImGui::GetTime();
    ImVec2 sc{c.x + card_w * 0.5f, c.y + 58.0f};
    for (int i = 0; i < 12; ++i) {
        float a = t * 6.0f - i * 0.52f;
        float r1 = 18.0f, r2 = 26.0f;
        ImVec2 p1{sc.x + r1 * std::cos(a), sc.y + r1 * std::sin(a)};
        ImVec2 p2{sc.x + r2 * std::cos(a), sc.y + r2 * std::sin(a)};
        int alpha = std::max(40, 220 - i * 14);
        // 2026-06-15 — broadcast spinner moves from electric-cyan to warm
        // amber so it matches the new primary interaction tonality (kAccent).
        // The overlay's card border already paints in kAccent; switching the
        // spinner unifies the loading affordance around one color family.
        ImU32 col = IM_COL32(0xD4, 0xA1, 0x4C, alpha);
        dl->AddLine(p1, p2, col, 3.0f);
    }
    // Status text.
    const char* status = s.sim_status.empty() ? "Simulating..." : s.sim_status.c_str();
    if (g_font_h2) {
        ImVec2 ts = g_font_h2->CalcTextSizeA(g_font_h2->FontSize, FLT_MAX, 0.0f, status);
        dl->AddText(g_font_h2, g_font_h2->FontSize,
                    ImVec2(c.x + (card_w - ts.x) * 0.5f, c.y + 100.0f),
                    kVlrText, status);
    } else {
        dl->AddText(ImVec2(c.x + 30.0f, c.y + 100.0f), kVlrText, status);
    }
    if (g_font_small) {
        const char* sub = "Game state is being updated. UI will resume in a moment.";
        ImVec2 ts = g_font_small->CalcTextSizeA(g_font_small->FontSize, FLT_MAX, 0.0f, sub);
        dl->AddText(g_font_small, g_font_small->FontSize,
                    ImVec2(c.x + (card_w - ts.x) * 0.5f, c.y + 138.0f),
                    kVlrSub, sub);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void DrawSidebar(AppState& s) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, toV4(kBgDeep));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 18));
    ImGui::BeginChild("##sidebar", ImVec2(228, 0), ImGuiChildFlags_Border);

    // === Slim brand header =================================================
    ImGui::PushFont(g_font_h1);
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
    ImGui::TextUnformatted("VLR");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::SameLine(0, 8);
    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(0, 8));
    if (g_font_small) ImGui::PushFont(g_font_small);
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
    ImGui::TextUnformatted("MANAGER");
    ImGui::PopStyleColor();
    if (g_font_small) ImGui::PopFont();
    ImGui::EndGroup();

    MutedText("Year %d  |  %s", s.gm.year, s.gm.current_phase().c_str());
    ThinDivider(6);

    // === User org chip (Phase 2): logo + tag + name ========================
    // Clickable 1-click "My Team" shortcut (2026-05-28). Renders the
    // existing logo + tag + name strip inside an InvisibleButton so the
    // whole row routes to OpenTeamProfile(user_team). Hover paints a
    // subtle raised background + "Open team profile" tooltip so users
    // discover the shortcut without instruction.
    if (s.gm.user_team) {
        auto& ut = *s.gm.user_team;
        ImVec2 chip_origin = ImGui::GetCursorScreenPos();
        float  chip_w      = ImGui::GetContentRegionAvail().x;
        float  chip_h      = 40.0f;

        // Invisible hit-rect spanning the whole row.
        ImGui::PushID("##my_team_chip");
        ImGui::InvisibleButton("##my_team_btn", ImVec2(chip_w, chip_h));
        bool chip_hovered = ImGui::IsItemHovered();
        bool chip_clicked = ImGui::IsItemClicked();
        if (chip_hovered) {
            ImGui::SetTooltip("Open My Team profile");
            ImDrawList* cdl = ImGui::GetWindowDrawList();
            cdl->AddRectFilled(
                chip_origin,
                ImVec2(chip_origin.x + chip_w, chip_origin.y + chip_h),
                kSurfaceAlt, 7.0f);
            // 2px gold accent stripe along the top edge — mirrors the
            // "you are here" indicator on active nav rows so the chip
            // reads as part of the same interaction language.
            cdl->AddRectFilled(
                chip_origin,
                ImVec2(chip_origin.x + chip_w, chip_origin.y + 2.0f),
                kAccent, 2.0f, ImDrawFlags_RoundCornersTop);
        }
        if (chip_clicked && s.gm.user_team) {
            OpenTeamProfile(s, s.gm.user_team);
        }

        // Re-draw the logo + tag + name overlay on top of the hit-rect.
        ImGui::SetCursorScreenPos(ImVec2(chip_origin.x + 4.0f,
                                         chip_origin.y + 6.0f));
        TeamLogo(ut, 28.f);
        ImGui::SameLine();
        ImGui::BeginGroup();
        // Tag uses kAccent (warm amber) to match the new primary interaction
        // accent. Bright kVlrGold remains reserved for trophy/prestige.
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kAccent));
        ImGui::PushFont(g_font_h2);
        ImGui::TextUnformatted(ut.tag.empty() ? "???" : ut.tag.c_str());
        ImGui::PopFont();
        ImGui::PopStyleColor();
        ImGui::PushFont(g_font_small);
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
        std::string nm = ut.name;
        if (nm.size() > 18) nm = nm.substr(0, 16) + "..";
        ImGui::TextUnformatted(nm.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::EndGroup();
        ImGui::PopID();
        // Restore cursor to just below the hit-rect for subsequent items.
        ImGui::SetCursorScreenPos(ImVec2(chip_origin.x,
                                         chip_origin.y + chip_h + 2.0f));
        ThinDivider(6);
    }
    ImGui::Spacing();

    // === Grouped nav: active item gets accent left-bar + raised bg =========
    // Active = 4px gold "you are here" bar + kSurfaceHi raised bg with a
    // 6% kAccent tint overlay so the row glows warmly without screaming.
    // Hover (inactive) = 2px half-accent left bar painted post-Button on
    // top of the framework hover bg, so inactive rows have a clear
    // affordance instead of relying only on the bg color shift.
    auto navbtn = [&](const char* label, Screen target) {
        bool selected = (s.screen == target);
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        float h = 32.0f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (selected) {
            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), kSurfaceHi, 7.0f);
            // 6% amber tint overlay (warm glow on the raised bg).
            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                              IM_COL32(0xD4, 0xA1, 0x4C, 0x0F), 7.0f);
            // 4px broadcast "you are here" bar, full row height.
            dl->AddRectFilled(p, ImVec2(p.x + 4.0f, p.y + h), kAccent,
                              2.0f, ImDrawFlags_RoundCornersLeft);
        }
        // Brighter amber for active text — max contrast against kSurfaceHi.
        const ImU32 active_text = IM_COL32(0xE8, 0xB8, 0x60, 0xFF);
        ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              selected ? kSurfaceHi : kSurfaceAlt);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kSurfaceHi);
        ImGui::PushStyleColor(ImGuiCol_Text,
                              toV4(selected ? active_text : kVlrSub));
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
        if (ImGui::Button(label, ImVec2(-FLT_MIN, h))) s.screen = target;
        // Hover affordance for inactive rows — 2px half-accent left bar
        // painted after the Button so it sits on top of the hover bg.
        if (!selected && ImGui::IsItemHovered()) {
            dl->AddRectFilled(p, ImVec2(p.x + 2.0f, p.y + h),
                              IM_COL32(0xD4, 0xA1, 0x4C, 0x80),
                              1.0f, ImDrawFlags_RoundCornersLeft);
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
    };
    auto navgroup = [&](const char* title) {
        ImGui::Dummy(ImVec2(0, 4));
        if (g_font_small) ImGui::PushFont(g_font_small);
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kTextFaint));
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();
        if (g_font_small) ImGui::PopFont();
        // 1px kBorder hairline beneath the group title so the six buckets
        // read as distinct sections rather than one continuous strip.
        ImVec2 hp = ImGui::GetCursorScreenPos();
        float  hw = ImGui::GetContentRegionAvail().x;
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(hp.x,       hp.y + 1.0f),
            ImVec2(hp.x + hw,  hp.y + 1.0f), kBorder, 1.0f);
        ImGui::Dummy(ImVec2(0, 4));
    };

    // Pack C C7 — collapsed 16-entry flat sidebar into 6 grouped routes.
    // Each route dispatches to a top-tab bar (see DrawCurrentScreen). The
    // navbtn handler also treats leaf-screen siblings of a group as
    // "selected" so deep-link landings (e.g., OpenTeamProfile sets
    // Screen::TeamProfile) keep the closest group highlighted.
    auto navbtn_group = [&](const char* label, Screen target,
                            std::initializer_list<Screen> siblings) {
        bool selected = (s.screen == target);
        for (auto sib : siblings) if (s.screen == sib) { selected = true; break; }
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        float h = 32.0f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (selected) {
            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), kSurfaceHi, 7.0f);
            // 6% amber tint overlay — warm glow on the raised bg.
            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                              IM_COL32(0xD4, 0xA1, 0x4C, 0x0F), 7.0f);
            // 4px broadcast "you are here" bar, full row height.
            dl->AddRectFilled(p, ImVec2(p.x + 4.0f, p.y + h), kAccent,
                              2.0f, ImDrawFlags_RoundCornersLeft);
        }
        // Brighter amber for active text — max contrast against kSurfaceHi.
        const ImU32 active_text = IM_COL32(0xE8, 0xB8, 0x60, 0xFF);
        ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              selected ? kSurfaceHi : kSurfaceAlt);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kSurfaceHi);
        ImGui::PushStyleColor(ImGuiCol_Text,
                              toV4(selected ? active_text : kVlrSub));
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
        if (ImGui::Button(label, ImVec2(-FLT_MIN, h))) s.screen = target;
        // Hover affordance for inactive rows — 2px half-accent left bar
        // painted after the Button so it sits on top of the hover bg.
        if (!selected && ImGui::IsItemHovered()) {
            dl->AddRectFilled(p, ImVec2(p.x + 2.0f, p.y + h),
                              IM_COL32(0xD4, 0xA1, 0x4C, 0x80),
                              1.0f, ImDrawFlags_RoundCornersLeft);
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
    };

    navgroup("NAVIGATION");
    navbtn_group("Home",        Screen::GroupHome,
                 {Screen::Dashboard, Screen::News});
    // Mail — same grouped nav row, but we capture the row rect first so we can
    // paint an unread-count badge over its right edge after the button draws.
    {
        ImVec2 mp = ImGui::GetCursorScreenPos();
        float  mw = ImGui::GetContentRegionAvail().x;
        navbtn_group("Mail", Screen::GroupMail, {});
        int unread = s.gm.unread_mail_count();
        if (unread > 0) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            float cx = mp.x + mw - 16.0f;   // near the right edge
            float cy = mp.y + 16.0f;        // row height is 32
            dl->AddCircleFilled(ImVec2(cx, cy), 9.0f, kVlrRed);
            char cbuf[8];
            std::snprintf(cbuf, sizeof(cbuf), "%d", unread > 99 ? 99 : unread);
            // Render the count in the small font so a 2-digit "99" stays inside
            // the 18px circle.
            ImFont* bf  = g_font_small ? g_font_small : ImGui::GetFont();
            float   bfs = bf->FontSize;
            ImVec2  ts  = bf->CalcTextSizeA(bfs, FLT_MAX, 0.0f, cbuf);
            dl->AddText(bf, bfs, ImVec2(cx - ts.x * 0.5f, cy - ts.y * 0.5f),
                        kVlrText, cbuf);
        }
    }
    navbtn_group("Team",        Screen::GroupTeam,
                 {Screen::Roster, Screen::Manager, Screen::Calendar});
    navbtn_group("Competition", Screen::GroupCompetition,
                 {Screen::Standings, Screen::Brackets,
                  Screen::PowerRankings, Screen::TeamProfile});
    navbtn_group("Awards",      Screen::GroupAwards,
                 {Screen::LeagueLeaders});
    navbtn_group("Finance",     Screen::GroupFinance, {});
    navbtn_group("People",      Screen::GroupPeople,
                 {Screen::Market, Screen::SoloQ, Screen::Compare});
    navbtn_group("History",     Screen::GroupHistory,
                 {Screen::Favorites, Screen::EventLog});
    navbtn_group("Live",        Screen::GroupLive,
                 {Screen::Watch});
    // Original navbtn lambda kept defined above for compatibility with any
    // future direct-link addition; unused-lambda generally doesn't warn
    // (lambdas are objects, not functions), so no explicit reference
    // needed here.

    ImGui::Spacing();
    ThinDivider(6);
    ImGui::Spacing();

    // === Day-by-day advance controls ===
    // Each click on Continue advances ONE in-game day on a worker thread.
    // While the sim is running, every button is disabled (visually dimmed)
    // and a full-screen "Simulating..." overlay appears. The UI keeps
    // presenting frames at 60fps because the engine work is OFF this thread.
    // When the worker finishes, the main loop joins it and calls
    // process_day_result (which is the only thing that touches ImGui state
    // post-sim — e.g. opening the live viewer modal).
    bool sim_busy = s.sim_running.load();
    ImGui::BeginDisabled(sim_busy);

    // Primary action — full-width accent-filled.
    ImGui::PushStyleColor(ImGuiCol_Button,        toV4(kAccent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(kAccentDim));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  toV4(kAccentDim));
    ImGui::PushStyleColor(ImGuiCol_Text,          toV4(IM_COL32(0x08,0x12,0x16,0xFF)));
    if (ImGui::Button("CONTINUE (1 day)", ImVec2(-FLT_MIN, 40))) {
        launch_async_sim(s, "Continuing...", [&s]() {
            auto r = s.gm.advance_day(s.log);
            s.pending_results.push_back(std::move(r));
        });
    }
    ImGui::PopStyleColor(4);

    // Secondary actions — subtle.
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kSurfaceAlt);
    ImGui::PushStyleColor(ImGuiCol_Text,          toV4(kVlrSub));
    if (ImGui::Button("Skip to next match", ImVec2(-FLT_MIN, 28))) {
        launch_async_sim(s, "Skipping to next match...", [&s]() {
            auto r = s.gm.advance_to_next_user_match(s.log);
            s.pending_results.push_back(std::move(r));
        });
    }
    if (ImGui::Button("Skip to Playoffs", ImVec2(-FLT_MIN, 28))) {
        launch_async_sim(s, "Skipping to playoffs...", [&s]() {
            auto r = s.gm.advance_to_playoffs(s.log);
            s.pending_results.push_back(std::move(r));
        });
    }
    if (ImGui::Button("Sim Full Season", ImVec2(-FLT_MIN, 28))) {
        launch_async_sim(s, "Simulating full season...", [&s]() {
            s.gm.simulate_full_season(s.log);
        });
    }
    if (ImGui::Button("Sim Solo Q Day",  ImVec2(-FLT_MIN, 28))) {
        launch_async_sim(s, "Simulating Solo Q day...", [&s]() {
            s.gm.simulate_solo_q_day();
        });
    }
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();

    ImGui::Spacing();
    ThinDivider(6);
    ImGui::Spacing();

    // Phase 2: NEW CAREER — gold-styled. Pops a confirmation modal that, on
    // accept, will reset_world() + bounce back to the wizard with cfg reset
    // to defaults. The actual reset happens inside the modal handler so we
    // don't blow away state mid-frame.
    ImGui::PushStyleColor(ImGuiCol_Button,        toV4(kVlrGold));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(IM_COL32(0xFF, 0xE0, 0x40, 0xFF)));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  toV4(IM_COL32(0xCC, 0xA0, 0x00, 0xFF)));
    ImGui::PushStyleColor(ImGuiCol_Text,          toV4(IM_COL32(0x20, 0x18, 0x00, 0xFF)));
    if (ImGui::Button("NEW CAREER", ImVec2(-FLT_MIN, 30))) {
        s.show_new_career_warning = true;
    }
    ImGui::PopStyleColor(4);

    ImGui::Spacing();

    bool old_god = s.god_mode;
    if (s.god_mode) {
        ImGui::PushStyleColor(ImGuiCol_Button,        toV4(kVlrGold));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(IM_COL32(0xFF,0xE0,0x40,0xFF)));
        ImGui::PushStyleColor(ImGuiCol_Text,          toV4(IM_COL32(0x20,0x18,0x00,0xFF)));
    } else {
        // OFF state reads as a real toggle row (kSurfaceAlt fill) rather
        // than transparent text — clear it is a clickable mode switch.
        ImGui::PushStyleColor(ImGuiCol_Button,        kSurfaceAlt);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kSurfaceHi);
        ImGui::PushStyleColor(ImGuiCol_Text,          toV4(kVlrSub));
    }
    if (ImGui::Button(s.god_mode ? "[*] GOD MODE: ON" : "GOD MODE: OFF", ImVec2(-FLT_MIN, 32))) {
        s.god_mode = !s.god_mode;
    }
    ImGui::PopStyleColor(3);
    if (g_font_small) ImGui::PushFont(g_font_small);
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kTextFaint));
    ImGui::TextWrapped("Toggle to edit player/coach attributes directly.");
    ImGui::PopStyleColor();
    if (g_font_small) ImGui::PopFont();

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

}  // namespace (sidebar)

// ============================================================
// Phase 1 (Agent C) — new pages and Favorites tabs
// ============================================================
//
// Layout:
//   - DrawTrophyRoomTab        — favorited player's trophy + timeline
//   - DrawHallOfRecordsTab     — career stat leader grid (2x3)
//   - DrawGreatestSeasonsTab   — top 25 single-season performances
//   - DrawPowerRankings        — power + community rankings (region tabs)
//   - DrawCompare              — side-by-side player comparison
//
// All routines defensively handle empty vectors / null accessors. Engine
// accessors (trophy_summary, career_timeline, best_season, top_n_seasons,
// head_to_head, trophy_case, dynasty_tier, power_rankings_for,
// community_rankings_for, next_power_tick_in_days) are implemented by
// Agents A/B in parallel — until those land, the UI surfaces "No data yet"
// placeholders rather than crashing.

namespace {

// --------- small helpers -------------------------------------------------

// Tier badge color for power rankings (S / A / B / C / Bubble).
ImU32 tier_color(const std::string& tier) {
    if (tier == "S")      return kIglBadge;                        // gold
    if (tier == "A")      return IM_COL32(0xB8, 0xB8, 0xB8, 0xFF); // silver
    if (tier == "B")      return IM_COL32(0xA0, 0x70, 0x50, 0xFF); // bronze
    if (tier == "C")      return IM_COL32(0x80, 0x80, 0x80, 0xFF); // gray
    return IM_COL32(0x50, 0x50, 0x50, 0xFF);                       // dim
}

// Event-category badge color used by Career Timeline rows.
ImU32 timeline_category_color(const std::string& cat) {
    if (cat == "trophy")     return kVlrGold;
    if (cat == "award")      return kInfo;
    if (cat == "transfer")   return IM_COL32(0x80, 0x80, 0x80, 0xFF);
    if (cat == "milestone")  return kVlrGreen;
    if (cat == "debut")      return IM_COL32(0x9A, 0x6D, 0xC2, 0xFF);
    if (cat == "retirement") return IM_COL32(0x9A, 0x6D, 0xC2, 0xFF);
    return kVlrSub;
}

// Collect every player from every region's solo Q global ladder.
std::vector<vlr::PlayerPtr> all_players_world(AppState& s) {
    std::vector<vlr::PlayerPtr> out;
    out.reserve(2048);
    for (auto& kv : s.gm.solo_qs) {
        if (!kv.second) continue;
        for (auto& p : kv.second->global_ladder()) {
            if (p) out.push_back(p);
        }
    }
    return out;
}

// Best-effort lookup of a Team* by name across every region's league.
vlr::TeamPtr find_team_by_name(AppState& s, const std::string& name) {
    if (name.empty()) return nullptr;
    for (auto& kv : s.gm.leagues) {
        if (!kv.second) continue;
        for (auto& t : kv.second->teams()) {
            if (t && t->name == name) return t;
        }
    }
    return nullptr;
}

// --------- Trophy Room (Favorites tab) -----------------------------------

void DrawTrophyRoomTab(AppState& s) {
    // Pick the most recently selected favorited player, falling back to the
    // first favorite. If neither is set, show the placeholder.
    vlr::PlayerPtr p = s.selected_player;
    if (!p || (s.gm.favorite_players.empty())) {
        // Try first favorite.
        if (!s.gm.favorite_players.empty()) p = s.gm.favorite_players.front();
    }
    bool is_favorited = false;
    if (p) {
        for (auto& fp : s.gm.favorite_players) {
            if (fp.get() == p.get()) { is_favorited = true; break; }
        }
    }
    if (!p || !is_favorited) {
        ImGui::TextDisabled("Favorite a player and re-open this tab to see their trophy room.");
        return;
    }

    char title[256];
    std::snprintf(title, sizeof(title), "%s — Trophy Case", p->name.c_str());
    SectionHeader(title, "Silverware, awards, and career milestones", kVlrGold);
    VSpace(6);

    // Pull the trophy summary. If the accessor isn't wired up yet this will
    // come back zeroed and we display zeros (still useful as a layout proof).
    auto ts = p->trophy_summary();

    // KPI row.
    StatTile("REGIONAL", std::to_string(ts.regional), kVlrGold);
    ImGui::SameLine();
    StatTile("MASTERS",  std::to_string(ts.masters),  kVlrGold);
    ImGui::SameLine();
    StatTile("WORLDS",   std::to_string(ts.worlds),   kVlrGold);
    ImGui::SameLine();
    StatTile("MVPs",     std::to_string(ts.mvps),     kAccent);
    ImGui::SameLine();
    StatTile("ROLE OTY", std::to_string(ts.role_awards), kAccent);
    if (p->is_igl) {
        ImGui::SameLine();
        StatTile("IGL OTY", std::to_string(ts.igl_oty), kIglBadge);
    }

    VSpace(12);
    if (BeginCard("##trophy_card", kSurface, 18.0f, 12.0f)) {
    SectionHeader("ALL TROPHIES", nullptr, kVlrGold);
    if (ts.all_titles.empty()) {
        ImGui::TextDisabled("No trophies yet — keep grinding.");
    } else {
        ImGui::BeginChild("##trophy_list", ImVec2(0, 180), ImGuiChildFlags_None);
        for (auto& t : ts.all_titles) {
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
            ImGui::Bullet();
            ImGui::SameLine();
            ImGui::TextUnformatted(t.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();
    }
    } EndCard();

    VSpace(12);
    H2("CAREER TIMELINE");
    auto timeline = p->career_timeline();
    if (timeline.empty()) {
        ImGui::TextDisabled("No notable career events recorded yet.");
        return;
    }
    // Sortable table over a local copy of the timeline events.
    auto tl_rows = timeline;
    if (ImGui::BeginTable("##timeline_list", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Sortable |
            ImGuiTableFlags_SortMulti | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_SizingStretchProp, ImVec2(0, 260))) {
        ImGui::TableSetupColumn("YEAR",
            ImGuiTableColumnFlags_DefaultSort |
            ImGuiTableColumnFlags_PreferSortDescending |
            ImGuiTableColumnFlags_WidthFixed, 56, 0);
        ImGui::TableSetupColumn("CATEGORY",
            ImGuiTableColumnFlags_WidthFixed, 110, 1);
        ImGui::TableSetupColumn("EVENT",
            ImGuiTableColumnFlags_WidthStretch, 0, 2);
        ImGui::TableHeadersRow();
        if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs()) {
            if (sp->SpecsDirty && sp->SpecsCount > 0) {
                const ImGuiTableColumnSortSpecs& c = sp->Specs[0];
                bool asc = (c.SortDirection == ImGuiSortDirection_Ascending);
                std::sort(tl_rows.begin(), tl_rows.end(),
                    [&](const auto& a, const auto& b) {
                        switch (c.ColumnUserID) {
                        case 1: return asc ? (a.category < b.category)
                                           : (a.category > b.category);
                        case 2: return asc ? (a.label < b.label)
                                           : (a.label > b.label);
                        case 0:
                        default: return asc ? (a.year < b.year)
                                            : (a.year > b.year);
                        }
                    });
                sp->SpecsDirty = false;
            }
        }
        int ei = 0;
        for (auto& ev : tl_rows) {
            ImGui::PushID(ei++);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushFont(g_font_mono); ImGui::Text("%d", ev.year); ImGui::PopFont();
            ImGui::TableNextColumn();
            std::string cat_upper = ev.category;
            for (auto& c : cat_upper) c = (char)std::toupper((unsigned char)c);
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  toV4(timeline_category_color(ev.category)));
            ImGui::Text("[%s]", cat_upper.c_str());
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(ev.label.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// --------- Hall of Records (Favorites tab) -------------------------------

void DrawHallOfRecordsTab(AppState& s) {
    SectionHeader("HALL OF RECORDS",
        "All-time career stat leaders across every region. Top 10 per category.",
        kVlrGold);
    VSpace(6);

    auto all = all_players_world(s);
    if (all.empty()) {
        ImGui::TextDisabled("No data yet.");
        return;
    }

    // Helper: render a top-10 leaderboard for a given scoring function.
    auto render_board = [&](const char* title,
                            std::function<double(const vlr::Player&)> score_fn,
                            const char* unit) {
        ImGui::PushID(title);
        ImGui::BeginChild(title, ImVec2(0, 280), ImGuiChildFlags_Border);
        H2(title);
        // Build scored rows.
        std::vector<std::pair<double, vlr::PlayerPtr>> rows;
        rows.reserve(all.size());
        for (auto& pp : all) {
            if (!pp) continue;
            double v = score_fn(*pp);
            if (v <= 0.0) continue;
            rows.emplace_back(v, pp);
        }
        if (rows.empty()) {
            ImGui::TextDisabled("No data yet.");
            ImGui::EndChild();
            ImGui::PopID();
            return;
        }
        std::sort(rows.begin(), rows.end(),
                  [](auto& a, auto& b) { return a.first > b.first; });
        if (rows.size() > 10) rows.resize(10);
        if (ImGui::BeginTable("##hor_tbl", 5,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti)) {
            ImGui::TableSetupColumn("#",
                ImGuiTableColumnFlags_WidthFixed |
                ImGuiTableColumnFlags_NoSort, 28);
            ImGui::TableSetupColumn("FLAG",
                ImGuiTableColumnFlags_WidthFixed |
                ImGuiTableColumnFlags_NoSort, 26);
            ImGui::TableSetupColumn("NAME",
                ImGuiTableColumnFlags_WidthStretch, 0, 2);
            ImGui::TableSetupColumn("TEAM",
                ImGuiTableColumnFlags_WidthFixed, 110, 3);
            ImGui::TableSetupColumn(unit,
                ImGuiTableColumnFlags_WidthFixed |
                ImGuiTableColumnFlags_DefaultSort |
                ImGuiTableColumnFlags_PreferSortDescending, 70, 4);
            ImGui::TableHeadersRow();
            if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs()) {
                if (sp->SpecsDirty && sp->SpecsCount > 0) {
                    const ImGuiTableColumnSortSpecs& c = sp->Specs[0];
                    bool asc =
                        (c.SortDirection == ImGuiSortDirection_Ascending);
                    std::sort(rows.begin(), rows.end(),
                        [&](const std::pair<double, vlr::PlayerPtr>& a,
                            const std::pair<double, vlr::PlayerPtr>& b) {
                            switch (c.ColumnUserID) {
                            case 2: return asc
                                ? (a.second->name < b.second->name)
                                : (a.second->name > b.second->name);
                            case 3: return asc
                                ? (a.second->team_name < b.second->team_name)
                                : (a.second->team_name > b.second->team_name);
                            case 4:
                            default: return asc ? (a.first < b.first)
                                                : (a.first > b.first);
                            }
                        });
                    sp->SpecsDirty = false;
                }
            }
            int rk = 1, idx = 0;
            for (auto& r : rows) {
                ImGui::PushID(idx++);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(placement_color(rk)));
                ImGui::PushFont(g_font_mono);
                ImGui::Text("#%d", rk);
                ImGui::PopFont();
                ImGui::PopStyleColor();
                ImGui::TableNextColumn(); CountryFlag(r.second->country_iso);
                ImGui::TableNextColumn();
                if (TableRowSelectable(player_label(*r.second).c_str(), false,
                        ImGuiSelectableFlags_SpanAllColumns)) {
                    OpenPlayerModal(s, r.second);
                }
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
                ImGui::TextUnformatted(r.second->team_name.c_str());
                ImGui::PopStyleColor();
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                ImGui::PushFont(g_font_mono);
                if (r.first >= 100.0) ImGui::Text("%.0f", r.first);
                else                  ImGui::Text("%.2f", r.first);
                ImGui::PopFont();
                ImGui::PopStyleColor();
                ImGui::PopID();
                ++rk;
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
        ImGui::PopID();
    };

    // 2x3 grid. Each cell is roughly half-width.
    float avail_w = ImGui::GetContentRegionAvail().x;
    float col_w   = (avail_w - 16.0f) * 0.5f;

    // Row 1.
    ImGui::BeginGroup();
    ImGui::PushItemWidth(col_w);
    ImGui::BeginChild("##hor_r1c1", ImVec2(col_w, 280));
    render_board("Most Career Kills",
        [](const vlr::Player& p) { return (double)p.career_kills; }, "K");
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##hor_r1c2", ImVec2(col_w, 280));
    render_board("Most Career MVPs",
        [](const vlr::Player& p) {
            auto ts = p.trophy_summary();
            return (double)ts.mvps;
        }, "MVP");
    ImGui::EndChild();
    ImGui::PopItemWidth();
    ImGui::EndGroup();

    // Row 2.
    ImGui::BeginGroup();
    ImGui::PushItemWidth(col_w);
    ImGui::BeginChild("##hor_r2c1", ImVec2(col_w, 280));
    render_board("Most Career Titles",
        [](const vlr::Player& p) {
            auto ts = p.trophy_summary();
            return (double)ts.total_trophies();
        }, "T");
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##hor_r2c2", ImVec2(col_w, 280));
    render_board("Highest Career Rating",
        [](const vlr::Player& p) {
            if (p.career_matches < 10) return 0.0;
            return p.avg_match_rating();
        }, "RAT");
    ImGui::EndChild();
    ImGui::PopItemWidth();
    ImGui::EndGroup();

    // Row 3.
    ImGui::BeginGroup();
    ImGui::PushItemWidth(col_w);
    ImGui::BeginChild("##hor_r3c1", ImVec2(col_w, 280));
    render_board("Most Grand-Final Clutches",
        [](const vlr::Player& p) {
            return (double)p.career_grand_final_clutches;
        }, "CLU");
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##hor_r3c2", ImVec2(col_w, 280));
    render_board("Most Pro Matches Played",
        [](const vlr::Player& p) { return (double)p.career_matches; }, "M");
    ImGui::EndChild();
    ImGui::PopItemWidth();
    ImGui::EndGroup();
}

// --------- Greatest Seasons (Favorites tab) ------------------------------

void DrawGreatestSeasonsTab(AppState& s) {
    SectionHeader("GREATEST SEASONS",
        "Top 25 single-season performances across history — sorted by "
        "season rating, then matches played.",
        kVlrGold);
    VSpace(6);

    auto all = all_players_world(s);
    struct Row {
        vlr::PlayerPtr player;
        vlr::Player::SeasonHighlight hi;
    };
    std::vector<Row> rows;
    rows.reserve(all.size() * 3);
    for (auto& p : all) {
        if (!p) continue;
        auto top = p->top_n_seasons(3);
        for (auto& h : top) {
            rows.push_back({p, h});
        }
    }
    if (rows.empty()) {
        ImGui::TextDisabled("No data yet — finish a season or two first.");
        return;
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.hi.rating != b.hi.rating) return a.hi.rating > b.hi.rating;
        return a.hi.matches > b.hi.matches;
    });
    if (rows.size() > 25) rows.resize(25);

    if (!ImGui::BeginTable("##greatest_seasons", 8,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
            ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti |
            ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, 560))) return;
    ImGui::TableSetupColumn("#",
        ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 30);
    ImGui::TableSetupColumn("PLAYER",
        ImGuiTableColumnFlags_WidthStretch, 0, 1);
    ImGui::TableSetupColumn("YEAR",
        ImGuiTableColumnFlags_WidthFixed |
        ImGuiTableColumnFlags_PreferSortDescending, 56, 2);
    ImGui::TableSetupColumn("TEAM",
        ImGuiTableColumnFlags_WidthFixed, 130, 3);
    ImGui::TableSetupColumn("RAT",
        ImGuiTableColumnFlags_WidthFixed |
        ImGuiTableColumnFlags_DefaultSort |
        ImGuiTableColumnFlags_PreferSortDescending, 60, 4);
    ImGui::TableSetupColumn("M",
        ImGuiTableColumnFlags_WidthFixed |
        ImGuiTableColumnFlags_PreferSortDescending, 50, 5);
    ImGui::TableSetupColumn("K-D-A",
        ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 110);
    ImGui::TableSetupColumn("AWARDS",
        ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort);
    ImGui::TableHeadersRow();
    if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs()) {
        if (sp->SpecsDirty && sp->SpecsCount > 0) {
            const ImGuiTableColumnSortSpecs& c = sp->Specs[0];
            bool asc = (c.SortDirection == ImGuiSortDirection_Ascending);
            std::sort(rows.begin(), rows.end(),
                [&](const Row& a, const Row& b) {
                    switch (c.ColumnUserID) {
                    case 1: return asc
                        ? (a.player->name < b.player->name)
                        : (a.player->name > b.player->name);
                    case 2: return asc ? (a.hi.year < b.hi.year)
                                       : (a.hi.year > b.hi.year);
                    case 3: return asc
                        ? (a.hi.team_name < b.hi.team_name)
                        : (a.hi.team_name > b.hi.team_name);
                    case 5: return asc ? (a.hi.matches < b.hi.matches)
                                       : (a.hi.matches > b.hi.matches);
                    case 4:
                    default: return asc ? (a.hi.rating < b.hi.rating)
                                        : (a.hi.rating > b.hi.rating);
                    }
                });
            sp->SpecsDirty = false;
        }
    }
    int rk = 1, idx = 0;
    for (auto& r : rows) {
        ImGui::PushID(idx++);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(placement_color(rk)));
        ImGui::PushFont(g_font_mono);
        ImGui::Text("#%d", rk);
        ImGui::PopFont();
        ImGui::PopStyleColor();
        ImGui::TableNextColumn();
        if (ImGui::Selectable(player_label(*r.player).c_str(), false,
                ImGuiSelectableFlags_SpanAllColumns)) {
            OpenPlayerModal(s, r.player);
        }
        ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%d", r.hi.year); ImGui::PopFont();
        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
        ImGui::TextUnformatted(r.hi.team_name.c_str());
        ImGui::PopStyleColor();
        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
        ImGui::PushFont(g_font_mono);
        ImGui::Text("%.2f", r.hi.rating);
        ImGui::PopFont();
        ImGui::PopStyleColor();
        ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%d", r.hi.matches); ImGui::PopFont();
        ImGui::TableNextColumn();
        ImGui::PushFont(g_font_mono);
        ImGui::Text("%d-%d-%d", r.hi.kills, r.hi.deaths, r.hi.assists);
        ImGui::PopFont();
        ImGui::TableNextColumn();
        // Comma-joined awards.
        std::string joined;
        for (std::size_t i = 0; i < r.hi.awards_that_year.size(); ++i) {
            if (i) joined += ", ";
            joined += r.hi.awards_that_year[i];
        }
        if (joined.empty()) {
            ImGui::TextDisabled("-");
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kInfo));
            ImGui::TextWrapped("%s", joined.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
        ++rk;
    }
    ImGui::EndTable();
}

// --------- Power / Community Rankings page -------------------------------
//
// Single screen with a top-level Power / Community tab, then nested
// region tabs. The same row renderer drives both modes; community mode
// adds a POP chip + DIVERGENCE badge when rank differs by >=2.

// Render a single ranking row. Generic over PowerRanking / CommunityRanking
// via a tiny shim: we pass extracted fields. This keeps us decoupled from
// whichever exact type alias Agent A lands on.
struct RankRowView {
    vlr::TeamPtr team;
    int  rank;
    int  prev_rank;
    double score;
    std::string tier;
    std::string analyst_note;
    bool   is_community = false;
    double popularity_score = 0.0;  // community only
    int    power_rank_other = -1;   // community only (for divergence)
};

void DrawRankRow(AppState& s, const RankRowView& v) {
    ImGui::PushID(v.rank);

    // Fixed-height row laid out with explicit pixel columns. NO nested
    // scroll child — the page owns the single vertical scroll. Width is
    // constrained to content-region so nothing produces a horizontal bar.
    const float kRowH  = 56.0f;
    const float full_w = ImGui::GetContentRegionAvail().x;
    ImVec2 cp = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 stripe = tier_color(v.tier);

    // Card background + border + tier stripe (manual draw, no child).
    dl->AddRectFilled(cp, ImVec2(cp.x + full_w, cp.y + kRowH),
                      kSurface, 10.0f);
    dl->AddRect(cp, ImVec2(cp.x + full_w, cp.y + kRowH),
                kBorder, 10.0f, 0, 1.0f);
    dl->AddRectFilled(cp, ImVec2(cp.x + 5, cp.y + kRowH), stripe, 10.0f,
                      ImDrawFlags_RoundCornersLeft);

    // Vertically-centred helper: place cursor at column x with a given
    // content height so it sits mid-row.
    auto col_at = [&](float x, float content_h) {
        ImGui::SetCursorScreenPos(
            ImVec2(cp.x + x, cp.y + (kRowH - content_h) * 0.5f));
    };

    // [col 14] big rank number
    col_at(14.0f, 30.0f);
    ImGui::PushFont(g_font_h1 ? g_font_h1 : g_font_h2);
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrText));
    ImGui::Text("%d", v.rank);
    ImGui::PopStyleColor();
    ImGui::PopFont();

    // [col 64] movement indicator
    col_at(64.0f, 18.0f);
    ImGui::PushFont(g_font_small);
    if (v.prev_rank < 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
        ImGui::TextUnformatted("NEW");
        ImGui::PopStyleColor();
    } else if (v.prev_rank > v.rank) {
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGreen));
        ImGui::Text("\xE2\x96\xB2%d", v.prev_rank - v.rank);
        ImGui::PopStyleColor();
    } else if (v.prev_rank < v.rank) {
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
        ImGui::Text("\xE2\x96\xBC%d", v.rank - v.prev_rank);
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kTextFaint));
        ImGui::TextUnformatted("\xE2\x80\x94");
        ImGui::PopStyleColor();
    }
    ImGui::PopFont();

    // [col 116] tier pill
    col_at(116.0f, 22.0f);
    {
        char tb[8];
        std::snprintf(tb, sizeof(tb), "%s", v.tier.empty() ? "?" : v.tier.c_str());
        Pill(tb, stripe);
    }

    // [col 168] tag + clickable name
    col_at(168.0f, 22.0f);
    if (v.team && !v.team->tag.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
        ImGui::Text("%s", v.team->tag.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
    }
    if (v.team) {
        if (ImGui::Selectable(v.team->name.c_str(), false, 0, ImVec2(210, 0))) {
            OpenTeamProfile(s, v.team);
        }
    } else {
        ImGui::TextDisabled("(unknown team)");
    }

    // [col 400] score
    col_at(400.0f, 18.0f);
    ImGui::PushFont(g_font_small);
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
    ImGui::Text("%.1f", v.score);
    ImGui::PopStyleColor();
    ImGui::PopFont();

    // Right region: community chips (anchored right) OR analyst note
    // (clipped/ellipsised so it never overflows the card).
    if (v.is_community) {
        float rx = full_w - 16.0f;
        // DIVERGENCE badge (rightmost)
        if (v.power_rank_other > 0 &&
            std::abs(v.power_rank_other - v.rank) >= 2) {
            char db[32];
            std::snprintf(db, sizeof(db), "DIVERGENCE %+d",
                          v.rank - v.power_rank_other);
            ImVec2 sz = ImGui::CalcTextSize(db);
            rx -= sz.x;
            ImGui::SetCursorScreenPos(
                ImVec2(cp.x + rx, cp.y + (kRowH - sz.y) * 0.5f));
            ImGui::PushFont(g_font_small);
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
            ImGui::TextUnformatted(db);
            ImGui::PopStyleColor();
            ImGui::PopFont();
            rx -= 14.0f;
        }
        // POP chip
        {
            char popbuf[32];
            std::snprintf(popbuf, sizeof(popbuf), "POP %d",
                          (int)std::round(v.popularity_score));
            int br = 0x60 + (int)(v.popularity_score * 0.012 * (0xFF - 0x60));
            if (br < 0x60) br = 0x60;
            if (br > 0xFF) br = 0xFF;
            ImU32 pop_col = IM_COL32(br, (int)(br * 0.6), 0x40, 0xFF);
            ImVec2 sz = ImGui::CalcTextSize(popbuf);
            rx -= sz.x;
            ImGui::SetCursorScreenPos(
                ImVec2(cp.x + rx, cp.y + (kRowH - sz.y) * 0.5f));
            ImGui::PushFont(g_font_small);
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(pop_col));
            ImGui::TextUnformatted(popbuf);
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }
    } else if (!v.analyst_note.empty()) {
        // Clip the note into [460 .. full_w-16], ellipsise if too long.
        float note_x0 = 460.0f;
        float note_w  = full_w - 16.0f - note_x0;
        if (note_w > 40.0f) {
            std::string note = v.analyst_note;
            ImGui::PushFont(g_font_small);
            float ell = ImGui::CalcTextSize("...").x;
            while (!note.empty() &&
                   ImGui::CalcTextSize(note.c_str()).x > note_w - ell) {
                note.pop_back();
            }
            if (note.size() != v.analyst_note.size() && !note.empty()) {
                note += "...";
            }
            ImVec2 sz = ImGui::CalcTextSize(note.c_str());
            ImGui::SetCursorScreenPos(
                ImVec2(cp.x + note_x0, cp.y + (kRowH - sz.y) * 0.5f));
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
            ImGui::TextUnformatted(note.c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }
    }

    // Advance layout cursor past this fixed-height row.
    ImGui::SetCursorScreenPos(ImVec2(cp.x, cp.y));
    ImGui::Dummy(ImVec2(full_w, kRowH));
    ImGui::Spacing();
    ImGui::PopID();
}

void DrawPowerRankings(AppState& s) {
    {
        char buf[160];
        int next_in = 0;
        // Defensive — accessor may not exist yet (returns 0 → "?").
        next_in = s.gm.next_power_tick_in_days();
        std::snprintf(buf, sizeof(buf),
                      "Last updated Day %d  -  Next update in %d days",
                      s.gm.day_total, next_in);
        SectionHeader("RANKINGS", buf, kAccent);
    }
    VSpace(6);

    static int top_mode = 0;  // 0 = Power, 1 = Community
    if (!ImGui::BeginTabBar("##rk_top_tabs")) return;

    const char* region_names[] = {"Americas", "EMEA", "Pacific", "International"};

    auto draw_region_for_mode = [&](const std::string& region, bool community_mode) {
        if (!community_mode) {
            const auto& list = s.gm.power_rankings_for(region);
            if (list.empty()) {
                ImGui::TextDisabled("No data yet for %s.", region.c_str());
                return;
            }
            // No nested scroll child — rows laid directly so the page owns
            // the single vertical scroll. No horizontal scrollbar.
            for (auto& pr : list) {
                RankRowView v;
                // Some engines may store the TeamPtr; handle either raw* or
                // shared_ptr (whichever Agent A lands on) via best-effort.
                if (pr.team) {
                    v.team = find_team_by_name(s, pr.team->name);
                    if (!v.team) {
                        // Engine returned a raw* not visible to us; fall back
                        // to displaying name from the raw struct.
                    }
                }
                v.rank = pr.rank;
                v.prev_rank = pr.prev_rank;
                v.score = pr.score;
                v.tier = pr.tier;
                v.analyst_note = pr.analyst_note;
                v.is_community = false;
                DrawRankRow(s, v);
            }
        } else {
            const auto& list = s.gm.community_rankings_for(region);
            if (list.empty()) {
                ImGui::TextDisabled("No community data yet for %s.", region.c_str());
                return;
            }
            for (auto& cr : list) {
                RankRowView v;
                if (cr.team) {
                    v.team = find_team_by_name(s, cr.team->name);
                }
                v.rank = cr.rank;
                v.prev_rank = cr.prev_rank;
                v.score = cr.score;
                v.tier = cr.tier;
                v.analyst_note = cr.analyst_note;
                v.is_community = true;
                v.popularity_score = cr.popularity_score;
                // Compute divergence gap using power_rank_of if available.
                if (cr.team) {
                    v.power_rank_other = s.gm.power_rank_of(cr.team);
                }
                DrawRankRow(s, v);
            }
        }
    };

    if (ImGui::BeginTabItem("Power")) {
        top_mode = 0;
        if (ImGui::BeginTabBar("##rk_pwr_regions")) {
            for (int i = 0; i < 4; ++i) {
                if (ImGui::BeginTabItem(region_names[i])) {
                    draw_region_for_mode(region_names[i], false);
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Community")) {
        top_mode = 1;
        if (ImGui::BeginTabBar("##rk_com_regions")) {
            for (int i = 0; i < 4; ++i) {
                if (ImGui::BeginTabItem(region_names[i])) {
                    draw_region_for_mode(region_names[i], true);
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
    (void)top_mode;
}

// --------- Compare page --------------------------------------------------

// Render a single-column comparison cell. Two-arg overload-style helper to
// keep the table loop compact.
void cmp_row_label(const char* label) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
    ImGui::PushFont(g_font_small);
    ImGui::TextUnformatted(label);
    ImGui::PopFont();
    ImGui::PopStyleColor();
}

// Pick-a-player combo: search filter + listbox.
vlr::PlayerPtr DrawComparePicker(AppState& s, const char* id,
                                  const std::vector<vlr::PlayerPtr>& all,
                                  vlr::PlayerPtr current) {
    ImGui::PushID(id);
    static char filter_a[64] = {};
    static char filter_b[64] = {};
    char* filter = (std::strcmp(id, "A") == 0) ? filter_a : filter_b;
    ImGui::SetNextItemWidth(280);
    ImGui::InputTextWithHint("##filter", "search player...", filter, 64);
    std::string ftext = filter;
    for (auto& c : ftext) c = (char)std::tolower((unsigned char)c);

    vlr::PlayerPtr chosen = current;
    if (ImGui::BeginListBox("##plist", ImVec2(280, 200))) {
        int shown = 0;
        for (auto& p : all) {
            if (!p) continue;
            if (!ftext.empty()) {
                std::string lname = p->name;
                for (auto& c : lname) c = (char)std::tolower((unsigned char)c);
                if (lname.find(ftext) == std::string::npos) continue;
            }
            ++shown;
            if (shown > 200) break;
            bool selected = current && current.get() == p.get();
            std::string lbl = p->name + " - " + p->team_name;
            if (TableRowSelectable(lbl.c_str(), selected)) {
                chosen = p;
            }
        }
        ImGui::EndListBox();
    }
    if (chosen) {
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
        ImGui::Text("Selected: %s", chosen->name.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::PopID();
    return chosen;
}

void DrawCompare(AppState& s) {
    SectionHeader("PLAYER COMPARISON",
        "Pick two players to compare side-by-side. Selection persists "
        "between page switches.",
        kAccent);
    VSpace(6);

    static vlr::PlayerPtr a;
    static vlr::PlayerPtr b;
    auto all = all_players_world(s);

    ImGui::BeginGroup();
    if (BeginCardSized("##cmp_a", ImVec2(330, 290))) {
        SectionHeader("PLAYER A", nullptr, kAccent);
        a = DrawComparePicker(s, "A", all, a);
    } EndCardSized();
    ImGui::EndGroup();

    ImGui::SameLine();

    ImGui::BeginGroup();
    if (BeginCardSized("##cmp_b", ImVec2(330, 290))) {
        SectionHeader("PLAYER B", nullptr, kAccent2);
        b = DrawComparePicker(s, "B", all, b);
    } EndCardSized();
    ImGui::EndGroup();

    if (!a || !b) {
        ImGui::Spacing();
        ImGui::TextDisabled("Select two players to see comparison.");
        return;
    }

    VSpace(10);
    SectionHeader("COMPARISON", nullptr, kAccent);

    if (!ImGui::BeginTable("##cmp_tbl", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
            ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("METRIC", ImGuiTableColumnFlags_WidthFixed, 160);
    ImGui::TableSetupColumn("A",      ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("B",      ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    auto ts_a = a->trophy_summary();
    auto ts_b = b->trophy_summary();
    auto bs_a = a->best_season();
    auto bs_b = b->best_season();

    auto cell_text = [](const std::string& v) {
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(v.c_str());
    };

    // Country.
    cmp_row_label("Country");
    ImGui::TableNextColumn(); CountryFlag(a->country_iso); ImGui::SameLine();
    ImGui::TextUnformatted(a->country_iso.c_str());
    ImGui::TableNextColumn(); CountryFlag(b->country_iso); ImGui::SameLine();
    ImGui::TextUnformatted(b->country_iso.c_str());

    cmp_row_label("Age");
    ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%d", a->age); ImGui::PopFont();
    ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%d", b->age); ImGui::PopFont();

    cmp_row_label("Team");
    cell_text(a->team_name);
    cell_text(b->team_name);

    cmp_row_label("Position");
    // Canonical chip order: PositionBadge -> FlexBadge -> IglBadge.
    ImGui::TableNextColumn();
    PositionBadge(vlr::position_of(*a));
    if (a->is_flex) { ImGui::SameLine(); FlexBadge(); }
    if (a->is_igl)  { ImGui::SameLine(); IglBadge();  }
    ImGui::TableNextColumn();
    PositionBadge(vlr::position_of(*b));
    if (b->is_flex) { ImGui::SameLine(); FlexBadge(); }
    if (b->is_igl)  { ImGui::SameLine(); IglBadge();  }

    cmp_row_label("Career Rating");
    ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%.2f", a->avg_match_rating()); ImGui::PopFont();
    ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%.2f", b->avg_match_rating()); ImGui::PopFont();

    cmp_row_label("Career K-D-A");
    ImGui::TableNextColumn();
    ImGui::PushFont(g_font_mono);
    ImGui::Text("%d-%d-%d", a->career_kills, a->career_deaths, a->career_assists);
    ImGui::PopFont();
    ImGui::TableNextColumn();
    ImGui::PushFont(g_font_mono);
    ImGui::Text("%d-%d-%d", b->career_kills, b->career_deaths, b->career_assists);
    ImGui::PopFont();

    cmp_row_label("Career Matches");
    ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%d", a->career_matches); ImGui::PopFont();
    ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%d", b->career_matches); ImGui::PopFont();

    cmp_row_label("Peak Season Rating");
    ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%.2f", bs_a.rating); ImGui::PopFont();
    ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%.2f", bs_b.rating); ImGui::PopFont();

    cmp_row_label("Total Trophies");
    ImGui::TableNextColumn();
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
    ImGui::PushFont(g_font_mono);
    ImGui::Text("%d", ts_a.total_trophies());
    ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::TableNextColumn();
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
    ImGui::PushFont(g_font_mono);
    ImGui::Text("%d", ts_b.total_trophies());
    ImGui::PopFont();
    ImGui::PopStyleColor();

    cmp_row_label("MVPs");
    ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%d", ts_a.mvps); ImGui::PopFont();
    ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%d", ts_b.mvps); ImGui::PopFont();

    cmp_row_label("Role Awards");
    ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%d", ts_a.role_awards); ImGui::PopFont();
    ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%d", ts_b.role_awards); ImGui::PopFont();

    cmp_row_label("Signature Agent");
    {
        std::string sa = a->signature_agent();
        std::string sb = b->signature_agent();
        ImGui::TableNextColumn();
        if (sa.empty()) ImGui::TextDisabled("-");
        else {
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
            ImGui::Text("[*] %s", sa.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::TableNextColumn();
        if (sb.empty()) ImGui::TextDisabled("-");
        else {
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
            ImGui::Text("[*] %s", sb.c_str());
            ImGui::PopStyleColor();
        }
    }

    cmp_row_label("Career Earnings");
    long long ea = 0, eb = 0;
    for (auto& sal : a->salary_log) ea += (long long)sal.second * 1000;
    for (auto& sal : b->salary_log) eb += (long long)sal.second * 1000;
    cell_text(fmt_money(ea));
    cell_text(fmt_money(eb));

    ImGui::EndTable();

    // Head-to-head section.
    VSpace(10);
    SectionHeader("HEAD-TO-HEAD", nullptr, kAccent2);
    auto h_ab = a->head_to_head(*b);
    auto h_ba = b->head_to_head(*a);
    if (h_ab.matches == 0 && h_ba.matches == 0) {
        ImGui::TextDisabled("These two have never faced off in a pro match.");
        return;
    }
    if (ImGui::BeginTable("##h2h_tbl", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("STAT",    ImGuiTableColumnFlags_WidthFixed, 200);
        ImGui::TableSetupColumn(a->name.c_str(), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(b->name.c_str(), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        cmp_row_label("Matches faced");
        ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%d", h_ab.matches); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%d", h_ba.matches); ImGui::PopFont();

        cmp_row_label("Wins / Losses");
        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGreen));
        ImGui::PushFont(g_font_mono);
        ImGui::Text("%d", h_ab.wins);
        ImGui::PopFont();
        ImGui::PopStyleColor();
        ImGui::SameLine(); ImGui::TextUnformatted(" / ");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
        ImGui::PushFont(g_font_mono);
        ImGui::Text("%d", h_ab.losses);
        ImGui::PopFont();
        ImGui::PopStyleColor();

        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGreen));
        ImGui::PushFont(g_font_mono);
        ImGui::Text("%d", h_ba.wins);
        ImGui::PopFont();
        ImGui::PopStyleColor();
        ImGui::SameLine(); ImGui::TextUnformatted(" / ");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
        ImGui::PushFont(g_font_mono);
        ImGui::Text("%d", h_ba.losses);
        ImGui::PopFont();
        ImGui::PopStyleColor();

        cmp_row_label("Kills for / against");
        ImGui::TableNextColumn();
        ImGui::PushFont(g_font_mono);
        ImGui::Text("%d / %d", h_ab.kills_for, h_ab.kills_against);
        ImGui::PopFont();
        ImGui::TableNextColumn();
        ImGui::PushFont(g_font_mono);
        ImGui::Text("%d / %d", h_ba.kills_for, h_ba.kills_against);
        ImGui::PopFont();

        cmp_row_label("Avg rating when facing");
        ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%.2f", h_ab.avg_rating_when_facing); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%.2f", h_ba.avg_rating_when_facing); ImGui::PopFont();

        ImGui::EndTable();
    }

    // === Team-level head-to-head split (C10 — Pack B accessors) ===========
    // Resolves each player's current team and surfaces reg/playoff split
    // + any captured finals between them this season. Silent when either
    // player is a Free Agent or retired (no current team to query).
    auto ta = find_team_by_name(s, a->team_name);
    auto tb = find_team_by_name(s, b->team_name);
    if (ta && tb && ta.get() != tb.get()) {
        int tot = s.gm.h2h_total(ta.get(), tb.get());
        if (tot > 0) {
            VSpace(8);
            SectionHeader("TEAM RIVALRY (this season)", nullptr, kAccent2);
            int reg = s.gm.h2h_regular(ta.get(), tb.get());
            int po  = s.gm.h2h_playoff(ta.get(), tb.get());
            MutedText("%s vs %s: %d total  -  %d regular  -  %d playoff",
                ta->name.c_str(), tb->name.c_str(), tot, reg, po);
            auto finals = s.gm.h2h_finals_between(ta.get(), tb.get());
            if (!finals.empty()) {
                ImGui::Spacing();
                if (ImGui::BeginTable("##cmp_finals", 4,
                        ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_BordersInnerV)) {
                    ImGui::TableSetupColumn("EVENT");
                    ImGui::TableSetupColumn("YEAR");
                    ImGui::TableSetupColumn("DAY");
                    ImGui::TableSetupColumn("WINNER");
                    ImGui::TableHeadersRow();
                    for (auto& f : finals) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(f.event_name.c_str());
                        ImGui::TableNextColumn();
                        ImGui::PushFont(g_font_mono); ImGui::Text("%d", f.year); ImGui::PopFont();
                        ImGui::TableNextColumn();
                        ImGui::PushFont(g_font_mono); ImGui::Text("%d", f.day_in_year + 1); ImGui::PopFont();   // 1-based, matches calendar/fixtures
                        ImGui::TableNextColumn();
                        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                        ImGui::TextUnformatted(
                            f.winner ? f.winner->name.c_str() : "?");
                        ImGui::PopStyleColor();
                    }
                    ImGui::EndTable();
                }
            }
        }
    }
}

// =========================================================================
// Phase 2: New Game Wizard
// =========================================================================
// Full-screen modal drawn BEFORE the rest of the UI on launch (and again
// when the user clicks the sidebar's "New Career" button). On "Start", we
// call gm.reset_world() (in case any state leaked from a prior session)
// then gm.initialize_world_with_config(s.cfg, s.log), then clear the
// show_new_game_wizard flag so the main UI takes over.
//
// All fields write into s.cfg (vlr::NewGameConfig). Colors derive from the
// org-name hash by default; we override that auto-fill the moment the user
// touches either ColorEdit3 (s.cfg_colors_user_picked).

// (NB: no inner anonymous namespace here — we're already inside one from
// the start of the Phase 1 additions block above. Wrapping again would
// leave an unclosed open and break linkage.)

// Cheap deterministic hash → ImU32 color, used to derive starter org colors
// from the org name string. Two hashes (rotated) give primary + accent so
// they differ. Same algorithm Team.cpp uses for procedural team colors.
inline std::uint32_t wizard_color_from_hash(std::uint32_t h) {
    // HSV-ish: pick hue from h, fixed saturation+value so colors look bright.
    double hue = (h % 360) / 360.0;
    double sat = 0.65, val = 0.85;
    double c = val * sat;
    double hp = hue * 6.0;
    double x = c * (1.0 - std::fabs(std::fmod(hp, 2.0) - 1.0));
    double r = 0, g = 0, b = 0;
    if      (hp < 1) { r = c; g = x; }
    else if (hp < 2) { r = x; g = c; }
    else if (hp < 3) { g = c; b = x; }
    else if (hp < 4) { g = x; b = c; }
    else if (hp < 5) { r = x; b = c; }
    else             { r = c; b = x; }
    double m = val - c;
    auto to8 = [](double v) {
        return static_cast<std::uint8_t>(std::min(255.0, std::max(0.0, (v + 0.0) * 255.0 + 0.5)));
    };
    std::uint8_t R = to8(r + m), G = to8(g + m), B = to8(b + m);
    // Pack as 0xAARRGGBB (alpha 0xFF). NewGameConfig stores std::uint32_t.
    return (0xFFu << 24) | (static_cast<std::uint32_t>(R) << 16)
         | (static_cast<std::uint32_t>(G) << 8) |  static_cast<std::uint32_t>(B);
}
inline std::uint32_t wizard_hash_string(const std::string& s) {
    std::uint32_t h = 2166136261u;
    for (char c : s) { h ^= static_cast<std::uint8_t>(c); h *= 16777619u; }
    return h;
}
inline void wizard_recompute_colors_from_name(AppState& s) {
    if (s.cfg_colors_user_picked) return;
    auto h = wizard_hash_string(s.cfg.user_team_name);
    s.cfg.user_color_primary = wizard_color_from_hash(h);
    s.cfg.user_color_accent  = wizard_color_from_hash(h * 2654435761u + 0x9E3779B9u);
}
// Unpack ImU32 ARGB into an ImVec3 for ColorEdit3.
inline ImVec4 wizard_u32_to_v4(std::uint32_t c) {
    ImVec4 v;
    v.x = ((c >> 16) & 0xFF) / 255.f;
    v.y = ((c >>  8) & 0xFF) / 255.f;
    v.z = ( c        & 0xFF) / 255.f;
    v.w = 1.f;
    return v;
}
inline std::uint32_t wizard_v4_to_u32(const ImVec4& v) {
    auto clamp01 = [](float f) { return f < 0.f ? 0.f : (f > 1.f ? 1.f : f); };
    std::uint8_t R = static_cast<std::uint8_t>(clamp01(v.x) * 255.f + 0.5f);
    std::uint8_t G = static_cast<std::uint8_t>(clamp01(v.y) * 255.f + 0.5f);
    std::uint8_t B = static_cast<std::uint8_t>(clamp01(v.z) * 255.f + 0.5f);
    return (0xFFu << 24) | (static_cast<std::uint32_t>(R) << 16)
         | (static_cast<std::uint32_t>(G) << 8) |  static_cast<std::uint32_t>(B);
}

// Reset the wizard's config to defaults (called when the user clicks "Use
// Defaults" or when restarting a career mid-game).
void wizard_reset_cfg(AppState& s) {
    s.cfg = vlr::NewGameConfig{};
    s.cfg_colors_user_picked = false;
    wizard_recompute_colors_from_name(s);
}

void DrawNewGameWizard(AppState& s) {
    // Ensure colors are populated on first draw (name hash → procedural).
    if (s.cfg.user_color_primary == 0 && s.cfg.user_color_accent == 0
        && !s.cfg_colors_user_picked) {
        wizard_recompute_colors_from_name(s);
    }

    // Render INTO the existing ##root window. We add some interior padding
    // by indenting via dummies — no nested Begin needed, which keeps the
    // single-window layout pipeline simple and avoids z-order surprises.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::Indent(28.0f);
    ImGui::Dummy(ImVec2(0, 16));

    // === Title block ====================================================
    {
        ImGui::PushFont(g_font_h1);
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
        ImVec2 tsz = ImGui::CalcTextSize("VLR MANAGER");
        ImGui::SetCursorPosX((vp->Size.x - tsz.x) * 0.5f);
        ImGui::TextUnformatted("VLR MANAGER");
        ImGui::PopStyleColor();
        ImGui::PopFont();

        ImGui::PushFont(g_font_h2);
        ImVec2 ssz = ImGui::CalcTextSize("Start a new career");
        ImGui::SetCursorPosX((vp->Size.x - ssz.x) * 0.5f);
        ImGui::TextDisabled("Start a new career");
        ImGui::PopFont();
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // === Two-column layout: form (left) | preview (right) ===============
    ImGui::Columns(2, "##wizcols", false);
    ImGui::SetColumnWidth(0, std::max(420.f, vp->Size.x * 0.62f));

    // ---- Left: form -----------------------------------------------------
    ImGui::BeginChild("##wizform", ImVec2(0, vp->Size.y - 180), ImGuiChildFlags_None);

    SectionHeader("ORGANIZATION", "Name your org and pick a home city", kAccent);
    {
        char name_buf[64];
        std::snprintf(name_buf, sizeof(name_buf), "%s", s.cfg.user_team_name.c_str());
        ImGui::SetNextItemWidth(360);
        if (ImGui::InputText("Org name", name_buf, sizeof(name_buf))) {
            s.cfg.user_team_name = name_buf;
            wizard_recompute_colors_from_name(s);
        }

        char city_buf[64];
        std::snprintf(city_buf, sizeof(city_buf), "%s", s.cfg.user_org_city.c_str());
        ImGui::SetNextItemWidth(360);
        if (ImGui::InputText("Org city", city_buf, sizeof(city_buf))) {
            s.cfg.user_org_city = city_buf;
        }
    }
    ImGui::Spacing();

    SectionHeader("REGION", "Which VCT league you compete in", kAccent);
    {
        const char* regions[3] = {"Americas", "EMEA", "Pacific"};
        int region_idx = 0;
        for (int i = 0; i < 3; ++i) {
            if (s.cfg.user_region == regions[i]) { region_idx = i; break; }
        }
        for (int i = 0; i < 3; ++i) {
            if (ImGui::RadioButton(regions[i], region_idx == i)) {
                s.cfg.user_region = regions[i];
            }
            if (i < 2) ImGui::SameLine();
        }
    }
    ImGui::Spacing();

    SectionHeader("COUNTRY OF ORIGIN", "Your org's home flag", kAccent);
    {
        const auto& clist = vlr::countries();
        // Find current index in list.
        int sel = 0;
        for (int i = 0; i < (int)clist.size(); ++i) {
            if (clist[i].iso == s.cfg.user_org_country_iso) { sel = i; break; }
        }
        // Build label of currently-selected.
        std::string preview = sel < (int)clist.size()
                              ? (clist[sel].name + " (" + clist[sel].iso + ")")
                              : std::string("(unknown)");
        ImGui::SetNextItemWidth(360);
        if (ImGui::BeginCombo("Country", preview.c_str())) {
            for (int i = 0; i < (int)clist.size(); ++i) {
                bool is_sel = (i == sel);
                std::string label = clist[i].name + " (" + clist[i].iso + ")";
                if (ImGui::Selectable(label.c_str(), is_sel)) {
                    s.cfg.user_org_country_iso = clist[i].iso;
                }
                if (is_sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        CountryFlag(s.cfg.user_org_country_iso);
    }
    ImGui::Spacing();

    SectionHeader("STARTING TIER", "Budget and expectations you begin with",
                  kAccent);
    {
        using OrgTier = vlr::NewGameConfig::OrgTier;
        struct TierOpt { OrgTier v; const char* label; const char* tip; };
        static const TierOpt tiers[] = {
            {OrgTier::Rich,       "Rich",       "$4M+ budget, win-now culture. Steeper expectations."},
            {OrgTier::Contender,  "Contender",  "$2.5M budget. Aiming to crack the top 3."},
            {OrgTier::Mid,        "Mid",        "$1.5M budget. Balanced rebuild + win mix. (Recommended)"},
            {OrgTier::Budget,     "Budget",     "$0.8M budget. Develop cheap talent, beat the odds."},
            {OrgTier::Expansion,  "Expansion",  "$0.5M, low prestige. Pure underdog start."},
        };
        for (auto& t : tiers) {
            bool sel = (s.cfg.user_tier == t.v);
            if (ImGui::RadioButton(t.label, sel)) {
                s.cfg.user_tier = t.v;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", t.tip);
            ImGui::SameLine();
        }
        ImGui::NewLine();
    }
    ImGui::Spacing();

    SectionHeader("BRAND COLORS", "Primary + accent — drives your logo",
                  kAccent2);
    {
        ImVec4 prim = wizard_u32_to_v4(s.cfg.user_color_primary);
        ImVec4 acc  = wizard_u32_to_v4(s.cfg.user_color_accent);
        ImGui::SetNextItemWidth(280);
        if (ImGui::ColorEdit3("Primary", &prim.x)) {
            s.cfg.user_color_primary = wizard_v4_to_u32(prim);
            s.cfg_colors_user_picked = true;
        }
        ImGui::SetNextItemWidth(280);
        if (ImGui::ColorEdit3("Accent",  &acc.x)) {
            s.cfg.user_color_accent  = wizard_v4_to_u32(acc);
            s.cfg_colors_user_picked = true;
        }
        if (ImGui::SmallButton("Re-derive from name")) {
            s.cfg_colors_user_picked = false;
            wizard_recompute_colors_from_name(s);
        }
    }
    ImGui::Spacing();

    SectionHeader("DIFFICULTY", "Scales AI match skill + AI manager sharpness + your economy", kAccent);
    {
        float dif = static_cast<float>(s.cfg.difficulty);
        ImGui::SetNextItemWidth(360);
        if (ImGui::SliderFloat("Difficulty", &dif, 0.7f, 1.3f, "%.2f")) {
            s.cfg.difficulty = static_cast<double>(dif);
        }
        ImGui::SameLine();
        if (dif < 0.95f)      ImGui::TextDisabled("(easier)");
        else if (dif > 1.05f) ImGui::TextDisabled("(harder)");
        else                  ImGui::TextDisabled("(default)");
        // P1.1: named anchor presets onto the same continuous 0.7..1.3 slider.
        if (ImGui::SmallButton("Easy"))   s.cfg.difficulty = 0.85;
        ImGui::SameLine();
        if (ImGui::SmallButton("Normal")) s.cfg.difficulty = 1.00;
        ImGui::SameLine();
        if (ImGui::SmallButton("Hard"))   s.cfg.difficulty = 1.15;
        ImGui::SameLine();
        ImGui::TextDisabled("Easy = weaker AI + economy tailwind | Hard = sharper AI + headwind");
    }
    ImGui::Spacing();

    SectionHeader("STARTING YEAR", "Which season the world begins in", kAccent);
    {
        int yr = s.cfg.starting_year;
        ImGui::SetNextItemWidth(140);
        if (ImGui::InputInt("Year", &yr)) {
            yr = std::max(2020, std::min(2099, yr));
            s.cfg.starting_year = yr;
        }
    }

    ImGui::EndChild();

    // ---- Right: preview -------------------------------------------------
    ImGui::NextColumn();
    ImGui::BeginChild("##wizpreview", ImVec2(0, vp->Size.y - 180), ImGuiChildFlags_Border);
    SectionHeader("PREVIEW", "Live look at your org identity", kVlrGold);
    ImGui::Spacing();

    // Build a synthetic preview team to feed TeamLogo without needing a
    // real vlr::Team (no world yet). We reuse the per-team draw_team_logo
    // contract directly so the preview always matches the live render.
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 pos = ImGui::GetCursorScreenPos();
        float size = 120.f;
        ImVec2 center{pos.x + size * 0.5f, pos.y + size * 0.5f};
        // Default to shape 0 (Shield) for preview. Once the engine assigns
        // a real logo_shape during initialize_world_with_config we'll show
        // that for real on the dashboard.
        std::uint8_t shape = 0;
        // ARGB → ImU32 conversion (NewGameConfig stores 0xAARRGGBB).
        auto u32_to_imu32 = [](std::uint32_t c) -> ImU32 {
            std::uint8_t A = (c >> 24) & 0xFF;
            std::uint8_t R = (c >> 16) & 0xFF;
            std::uint8_t G = (c >>  8) & 0xFF;
            std::uint8_t B =  c        & 0xFF;
            if (A == 0) A = 0xFF;
            return IM_COL32(R, G, B, A);
        };
        // Derive a 3-char monogram from the in-progress org name so the
        // preview badge shows the user's actual brand identity rather than
        // a generic per-shape default.
        std::string preview_tag = s.cfg.user_team_name.empty()
            ? std::string("ORG")
            : s.cfg.user_team_name.substr(0,
                std::min<std::size_t>(3, s.cfg.user_team_name.size()));
        for (auto& ch : preview_tag) ch = (char)std::toupper((unsigned char)ch);
        vlr::draw_team_logo(dl, center, size * 0.45f, shape,
                            u32_to_imu32(s.cfg.user_color_primary),
                            u32_to_imu32(s.cfg.user_color_accent),
                            preview_tag.c_str());
        ImGui::Dummy(ImVec2(size, size));
    }

    ImGui::Spacing();
    ImGui::PushFont(g_font_h2);
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrText));
    ImGui::TextUnformatted(s.cfg.user_team_name.empty() ? "(org name)"
                                                        : s.cfg.user_team_name.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
    if (!s.cfg.user_org_city.empty()) ImGui::Text("Based in %s", s.cfg.user_org_city.c_str());
    ImGui::Text("Region: %s", s.cfg.user_region.c_str());
    ImGui::Text("ISO: %s", s.cfg.user_org_country_iso.c_str());
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Mock standings row.
    H2("MOCK STANDINGS ROW", kVlrSub);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        float row_w = ImGui::GetContentRegionAvail().x;
        float row_h = 38.f;
        dl->AddRectFilled(p0, ImVec2(p0.x + row_w, p0.y + row_h),
                          IM_COL32(0x1E, 0x28, 0x36, 0xFF));
        // Logo + tag-ish name preview using the same pipeline.
        auto u32_to_imu32 = [](std::uint32_t c) -> ImU32 {
            std::uint8_t A = (c >> 24) & 0xFF;
            std::uint8_t R = (c >> 16) & 0xFF;
            std::uint8_t G = (c >>  8) & 0xFF;
            std::uint8_t B =  c        & 0xFF;
            if (A == 0) A = 0xFF;
            return IM_COL32(R, G, B, A);
        };
        std::string tagprev = s.cfg.user_team_name.empty()
            ? std::string("ORG")
            : s.cfg.user_team_name.substr(0, std::min<size_t>(3, s.cfg.user_team_name.size()));
        for (auto& c : tagprev) c = (char)std::toupper((unsigned char)c);
        vlr::draw_team_logo(dl,
                            ImVec2(p0.x + 22, p0.y + row_h * 0.5f),
                            14.f, 0,
                            u32_to_imu32(s.cfg.user_color_primary),
                            u32_to_imu32(s.cfg.user_color_accent),
                            tagprev.c_str());
        dl->AddText(ImVec2(p0.x + 44, p0.y + 8), kVlrGold, tagprev.c_str());
        dl->AddText(ImVec2(p0.x + 86, p0.y + 8), kVlrText,
                    s.cfg.user_team_name.empty() ? "(your org)" : s.cfg.user_team_name.c_str());
        dl->AddText(ImVec2(p0.x + row_w - 60, p0.y + 8), kVlrText, "0 - 0");
        ImGui::Dummy(ImVec2(row_w, row_h));
    }

    ImGui::EndChild();
    ImGui::Columns(1);

    // === Bottom buttons =================================================
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Use Defaults", ImVec2(160, 40))) {
        wizard_reset_cfg(s);
    }
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Button,        toV4(kVlrRed));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(IM_COL32(0xFF, 0x70, 0x80, 0xFF)));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  toV4(IM_COL32(0xC0, 0x30, 0x40, 0xFF)));
    if (ImGui::Button("START NEW CAREER", ImVec2(280, 40))) {
        // Belt-and-braces: reset world state first, then init with cfg.
        s.gm.reset_world();
        s.log.clear();
        bool ok = s.gm.initialize_world_with_config(s.cfg, s.log);
        if (!ok) {
            s.log.emplace_back("[Wizard] initialize_world_with_config failed; check log.");
        } else {
            s.log.emplace_back("[Wizard] World initialized with custom configuration.");
        }
        s.show_new_game_wizard = false;
        s.screen = Screen::GroupHome;
        // Reset Calendar cursor so it snaps to the new world's "today".
        s.cal_cursor_day  = -1;
        s.cal_year_offset = 0;
    }
    ImGui::PopStyleColor(3);

    ImGui::Unindent(28.0f);
}

// Confirmation popup for the sidebar's "NEW CAREER" button — destructive
// op, so we make the user confirm before bouncing back into the wizard.
void DrawNewCareerWarningModal(AppState& s) {
    if (!s.show_new_career_warning) return;
    ImGui::OpenPopup("Start a new career?##nc_warn");
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520, 200), ImGuiCond_Appearing);
    bool open = true;
    if (ImGui::BeginPopupModal("Start a new career?##nc_warn", &open,
                               ImGuiWindowFlags_NoSavedSettings)) {
        if (g_font_h1) ImGui::PushFont(g_font_h1);
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
        ImGui::TextUnformatted("This will erase your current career.");
        ImGui::PopStyleColor();
        if (g_font_h1) ImGui::PopFont();
        VSpace(8);
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
        ImGui::TextWrapped("All teams, players, awards, and rankings will be regenerated. "
                           "Are you sure you want to continue?");
        ImGui::PopStyleColor();
        ThinDivider(8);
        if (ImGui::Button("Cancel", ImVec2(140, 36))) {
            s.show_new_career_warning = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button,        toV4(kVlrRed));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(IM_COL32(0xFF, 0x70, 0x80, 0xFF)));
        if (ImGui::Button("Yes, restart career", ImVec2(220, 36))) {
            s.gm.reset_world();
            wizard_reset_cfg(s);
            s.show_new_game_wizard = true;
            s.show_new_career_warning = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(2);
        ImGui::EndPopup();
    }
    if (!open) s.show_new_career_warning = false;
}

// === Sponsor-choice modal ==================================================
// Surfaces the preseason sponsor system. When the engine flags
// sponsor_choice_pending_, the user picks one of three SponsorOffers (or
// declines). choose_sponsor() stamps the pick on user_team AND clears the
// pending flag (an empty offer = decline / no sponsor). We only OpenPopup
// when the flag is set, offers exist, and there is a user team to stamp.
void DrawSponsorChoiceModal(AppState& s) {
    auto& gm = s.gm;
    if (!gm.sponsor_choice_pending_ || gm.pending_sponsor_offers_.empty()
            || !gm.user_team) {
        return;
    }
    ImGui::OpenPopup("Choose a Sponsor##sponsor_choice");
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(760, 420), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Choose a Sponsor##sponsor_choice", nullptr,
                               ImGuiWindowFlags_NoSavedSettings)) {
        if (g_font_h1) ImGui::PushFont(g_font_h1);
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
        ImGui::TextUnformatted("Preseason Sponsorship");
        ImGui::PopStyleColor();
        if (g_font_h1) ImGui::PopFont();
        MutedText("Pick a backer for the coming season. Hit their target by "
                  "year-end and the club banks a one-time bonus.");
        ThinDivider(8);

        int oi = 0;
        for (const auto& offer : gm.pending_sponsor_offers_) {
            ImGui::PushID(oi++);
            if (BeginCard("##sponsor_offer_card")) {
                if (g_font_h2) ImGui::PushFont(g_font_h2);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kAccent));
                ImGui::TextUnformatted(offer.name.c_str());
                ImGui::PopStyleColor();
                if (g_font_h2) ImGui::PopFont();
                MutedText("%s", offer.requirement_label.c_str());
                VSpace(4);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGreen));
                ImGui::Text("Reward: +$%dK bonus", offer.reward_k);
                ImGui::PopStyleColor();
                VSpace(4);
                if (ImGui::Button("Select", ImVec2(160, 32))) {
                    gm.choose_sponsor(offer);
                    ImGui::CloseCurrentPopup();
                }
            }
            EndCard();
            ImGui::PopID();
            VSpace(6);
        }

        ThinDivider(6);
        if (ImGui::Button("Decline (no sponsor)", ImVec2(220, 36))) {
            gm.choose_sponsor(vlr::SponsorOffer{});
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

}  // namespace (Phase 1 — Agent C additions)

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L,
                       hInstance, nullptr, nullptr, nullptr, nullptr,
                       L"VlrManager", nullptr };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"VLR Manager :: C++ Edition",
                                WS_OVERLAPPEDWINDOW, 60, 30, 1520, 940,
                                nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    LoadFonts(io);
    ApplyVlrTheme(ImGui::GetStyle());

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    auto state = std::make_unique<AppState>();
    // Phase 2: defer world init until the user finishes the New Game Wizard.
    // The wizard's "Start New Career" button calls reset_world() +
    // initialize_world_with_config(state->cfg, state->log).
    state->show_new_game_wizard = true;

    bool done = false;
    LARGE_INTEGER freq, last;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);

    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (g_ResizeWidth && g_ResizeHeight) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float dt = static_cast<float>(now.QuadPart - last.QuadPart) / static_cast<float>(freq.QuadPart);
        last = now;
        if (dt > 0.5f) dt = 0.5f;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::Begin("##root", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::PopStyleVar(2);

        // Drain a completed async sim BEFORE rendering this frame. If the
        // worker has finished, this joins the thread and runs any DayResult
        // processing (e.g. opens live viewer modal) on the UI thread.
        poll_async_sim(*state);

        if (state->show_new_game_wizard) {
            // Phase 2: full-screen wizard takes over before world exists.
            // No sidebar, no DrawCurrentScreen, no Player modal (no players).
            DrawNewGameWizard(*state);
        } else if (state->sim_running.load()) {
            // Async sim in flight — render ONLY the overlay. We deliberately
            // do NOT render the sidebar/pages because they read engine state
            // (rosters, recordings, etc.) that the worker is mutating; even
            // a single-byte race could crash on shared_ptr edges. The
            // overlay touches no engine state — pure ImGui primitives.
            DrawSimOverlay(*state);
        } else {
            DrawSidebar(*state);
            ImGui::SameLine();
            DrawCurrentScreen(*state);
            DrawPlayerProfileModal(*state);
            DrawLiveMatchModal(*state, dt);
            if (state->show_new_career_warning) {
                DrawNewCareerWarningModal(*state);
            }
            DrawSponsorChoiceModal(*state);
        }

        ImGui::End();

        ImGui::Render();
        const float clear[4] = {0.06f, 0.08f, 0.11f, 1.0f};
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    state.reset();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
