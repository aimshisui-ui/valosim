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

// New design-system tokens (use these in new card-based layouts):
constexpr ImU32 kAccent       = IM_COL32(0x00, 0xD4, 0xFF, 0xFF); // electric blue — primary
constexpr ImU32 kAccentDim    = IM_COL32(0x0A, 0x9F, 0xBE, 0xFF);
constexpr ImU32 kAccent2      = IM_COL32(0x7B, 0x5E, 0xFF, 0xFF); // purple — secondary
constexpr ImU32 kAccent2Dim   = IM_COL32(0x5C, 0x47, 0xC0, 0xFF);
constexpr ImU32 kBgDeep       = IM_COL32(0x0B, 0x0D, 0x12, 0xFF); // deepest (gutters)
constexpr ImU32 kBgBase       = IM_COL32(0x0F, 0x11, 0x17, 0xFF); // window bg
constexpr ImU32 kSurface      = IM_COL32(0x17, 0x1A, 0x24, 0xFF); // card bg
constexpr ImU32 kSurfaceAlt   = IM_COL32(0x1C, 0x20, 0x2C, 0xFF); // alt row / hover
constexpr ImU32 kSurfaceHi    = IM_COL32(0x24, 0x29, 0x38, 0xFF); // raised / selected
constexpr ImU32 kBorder       = IM_COL32(0x2A, 0x2F, 0x3D, 0xFF); // hairline border
constexpr ImU32 kBorderStrong = IM_COL32(0x3A, 0x41, 0x52, 0xFF);
constexpr ImU32 kTextFaint    = IM_COL32(0x5E, 0x66, 0x74, 0xFF); // labels / captions

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
    s.WindowRounding    = 10.0f;
    s.FrameRounding     = 8.0f;
    s.ChildRounding     = 10.0f;
    s.GrabRounding      = 8.0f;
    s.PopupRounding     = 10.0f;
    s.ScrollbarRounding = 10.0f;
    s.TabRounding       = 8.0f;
    s.TabBarBorderSize  = 0.0f;
    s.WindowPadding     = ImVec2(22, 20);
    s.FramePadding      = ImVec2(13, 8);
    s.ItemSpacing       = ImVec2(11, 9);
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
    c[ImGuiCol_PlotLines]         = toV4(kAccent);
    c[ImGuiCol_PlotLinesHovered]  = toV4(kVlrGold);
    c[ImGuiCol_PlotHistogram]     = toV4(kAccent2);
    c[ImGuiCol_PlotHistogramHovered] = toV4(kVlrGold);
    c[ImGuiCol_TableHeaderBg]     = toV4(IM_COL32(0x16, 0x19, 0x22, 0xFF));
    c[ImGuiCol_TableBorderStrong] = toV4(kBorder);
    c[ImGuiCol_TableBorderLight]  = toV4(IM_COL32(0x1B, 0x1F, 0x29, 0xFF));
    c[ImGuiCol_TableRowBg]        = toV4(IM_COL32(0x00, 0x00, 0x00, 0x00));
    c[ImGuiCol_TableRowBgAlt]     = toV4(IM_COL32(0xFF, 0xFF, 0xFF, 0x06));
    c[ImGuiCol_TextSelectedBg]    = toV4(IM_COL32(0x00, 0xD4, 0xFF, 0x40));
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
bool BeginCard(const char* id, ImU32 bg = kSurface, float pad = 18.0f,
               float rounding = 12.0f) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, toV4(bg));
    ImGui::PushStyleColor(ImGuiCol_Border,  toV4(kBorder));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, rounding);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    bool open = ImGui::BeginChild(id, ImVec2(0, 0),
                    ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders,
                    ImGuiWindowFlags_NoScrollbar);
    return open;
}
void EndCard() {
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

// Fixed-size card variant (for grid/row layouts where you need exact dims).
bool BeginCardSized(const char* id, ImVec2 size, ImU32 bg = kSurface,
                    float pad = 16.0f, float rounding = 12.0f) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, toV4(bg));
    ImGui::PushStyleColor(ImGuiCol_Border,  toV4(kBorder));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, rounding);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    bool open = ImGui::BeginChild(id, size, ImGuiChildFlags_Borders,
                                  ImGuiWindowFlags_NoScrollbar);
    return open;
}
void EndCardSized() {
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
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

std::string fmt_money(long long dollars) {
    char buf[32];
    if (dollars >= 1000000)   std::snprintf(buf, sizeof(buf), "$%.2fM", dollars / 1000000.0);
    else if (dollars >= 1000) std::snprintf(buf, sizeof(buf), "$%lldK",  dollars / 1000);
    else                       std::snprintf(buf, sizeof(buf), "$%lld",  dollars);
    return buf;
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

// Renders the team's procedural logo (shape + primary + accent) at the
// current cursor as a `size` x `size` block. Reserves layout via Dummy().
// Falls back to a colored disc if logo_shape is somehow out of range.
void TeamLogo(const vlr::Team& team, float size = 32.f) {
    auto dl = ImGui::GetWindowDrawList();
    auto pos = ImGui::GetCursorScreenPos();
    ImVec2 center{pos.x + size * 0.5f, pos.y + size * 0.5f};
    ImU32 primary = team_primary_color(team);
    ImU32 accent  = team_accent_color(team);
    vlr::draw_team_logo(dl, center, size * 0.45f,
                        static_cast<std::uint8_t>(team.logo_shape),
                        primary, accent);
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
    GroupLive          // tabs: Watch
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

    bool show_history_detail_modal = false;
    vlr::RecordedMatchPtr history_detail;

    bool god_mode = false;

    LiveMatchState live;

    // Selected team for the Team Profile screen. Set by OpenTeamProfile()
    // from any clickable team row (Standings, Brackets, Calendar, etc.).
    vlr::TeamPtr selected_team;
    Screen       prev_screen_before_team_profile = Screen::Dashboard;

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
    // Tracks whether the user manually picked colors yet — auto-hash from
    // org name updates them only while still false.
    bool cfg_colors_user_picked = false;

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
void DrawTrophyRoomTab(AppState& s);
void DrawHallOfRecordsTab(AppState& s);
void DrawGreatestSeasonsTab(AppState& s);

// Phase 2 (Agent C) additions — see end of file for definitions.
void DrawNewGameWizard(AppState& s);
void DrawNewCareerWarningModal(AppState& s);

// Frivolities + history pack — defined later in the file.
std::vector<vlr::PlayerPtr> all_players_world(AppState& s);

void OpenPlayerModal(AppState& s, vlr::PlayerPtr p) {
    s.selected_player = p;
    s.show_player_modal = true;
}

// Open the Team Profile screen for the given team. Records the screen the
// user came from so we can offer a Back button.
void OpenTeamProfile(AppState& s, vlr::TeamPtr t) {
    if (!t) return;
    if (s.screen != Screen::TeamProfile) s.prev_screen_before_team_profile = s.screen;
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

void DrawPlayerProfileModal(AppState& s) {
    if (!s.show_player_modal || !s.selected_player) return;
    ImGui::OpenPopup("Player Profile##modal");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(820, 720), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("Player Profile##modal", &s.show_player_modal,
                               ImGuiWindowFlags_NoSavedSettings)) {
        auto p = s.selected_player;

        // Header strip
        // === Header strip: handle, flag, role, identity, action buttons ===
        // Hero header card — premium framing around the existing handle /
        // flag / badges / identity rows. Pure visual wrapper; every widget,
        // label and ImGui ID inside is unchanged.
        BeginCard("##pp_hero", kBgDeep, 16.0f, 12.0f);
        CountryFlag(p->country_iso);
        ImGui::SameLine();
        if (s.god_mode) {
            char name_buf[128];
            std::snprintf(name_buf, sizeof(name_buf), "%s", p->name.c_str());
            ImGui::SetNextItemWidth(280);
            if (ImGui::InputText("##nameedit", name_buf, sizeof(name_buf))) {
                p->name = name_buf;
            }
        } else {
            H1(player_label(*p).c_str(), kVlrRed);
        }
        ImGui::SameLine(); ImGui::Dummy(ImVec2(10, 0)); ImGui::SameLine();
        PositionBadge(vlr::position_of(*p));
        if (p->is_flex) { ImGui::SameLine(); FlexBadge(); }
        if (p->is_igl)  { ImGui::SameLine(); IglBadge();  }

        // Favorite + queue-ranked actions on the right side of the header.
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(20, 0));
        ImGui::SameLine();
        bool favorited = s.gm.is_favorited(p);
        if (favorited) ImGui::PushStyleColor(ImGuiCol_Button, toV4(kVlrGold));
        if (ImGui::Button(favorited ? "[*] Favorited" : "[ ] Favorite", ImVec2(140, 28))) {
            if (favorited) s.gm.unfavorite_player(p);
            else            s.gm.favorite_player(p);
        }
        if (favorited) ImGui::PopStyleColor();
        ImGui::SameLine();
        // Force solo Q match: pop the live viewer with this player as host.
        ImGui::PushStyleColor(ImGuiCol_Button,        toV4(kVlrRed));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(kVlrRedDim));
        bool clicked_queue = ImGui::Button("Queue Ranked Match", ImVec2(180, 28));
        ImGui::PopStyleColor(2);

        // Identity strip.
        if (!p->first.empty() || !p->last.empty() || !p->country.empty()) {
            Sub("%s %s  |  %s  |  Age %d  |  %s  |  $%dK/yr (exp %d)  |  %dY left",
                p->first.c_str(), p->last.c_str(),
                p->country.empty() ? "?" : p->country.c_str(),
                p->age,
                p->team_name.c_str(),
                p->contract.amount_k, p->contract.exp_year,
                p->years_left(s.gm.year));
        } else {
            Sub("%s  |  Age %d  |  Team: %s  |  %dY left  |  $%dK/yr (exp %d)",
                p->region.c_str(), p->age, p->team_name.c_str(),
                p->years_left(s.gm.year), p->contract.amount_k, p->contract.exp_year);
        }

        // Signature agent strip — gold accent identifying this player's
        // mastery main. Empty until the player has 5+ pro matches on a
        // single agent, so most rookies won't see this line.
        {
            std::string sig = p->signature_agent();
            if (!sig.empty()) {
                auto sit = p->agent_mastery.find(sig);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                if (sit != p->agent_mastery.end() && sit->second.matches > 0) {
                    ImGui::Text("[*] Signature %s  -  %d matches  -  %.2f avg  -  peak %.2f",
                        sig.c_str(), sit->second.matches,
                        sit->second.avg_rating, sit->second.peak_rating);
                } else {
                    ImGui::Text("[*] Signature %s", sig.c_str());
                }
                ImGui::PopStyleColor();

                // Phase 1 (Agent C) — regional rank flavor line. Compute
                // LOCALLY by scanning every player in this region for the
                // same signature agent, ranked by agent_mastery score.
                double my_score = 0.0;
                if (sit != p->agent_mastery.end()) {
                    // Use matches * avg_rating as a robust mastery proxy
                    // (works whether the engine exposes a method or not).
                    my_score = (double)sit->second.matches * sit->second.avg_rating;
                }
                int rank = 1;     // 1-indexed
                int tied_above = 0;
                auto it_eng = s.gm.solo_qs.find(p->region);
                if (it_eng != s.gm.solo_qs.end() && it_eng->second) {
                    for (auto& other : it_eng->second->global_ladder()) {
                        if (!other || other.get() == p.get()) continue;
                        if (other->signature_agent() != sig) continue;
                        auto oit = other->agent_mastery.find(sig);
                        if (oit == other->agent_mastery.end()) continue;
                        double os = (double)oit->second.matches * oit->second.avg_rating;
                        if (os > my_score) ++tied_above;
                    }
                }
                rank = tied_above + 1;
                const char* flavor = nullptr;
                if      (rank == 1)  flavor = "Best %s player in %s";
                else if (rank <= 3)  flavor = "Top %s player in %s";
                else if (rank <= 10) flavor = "Notable %s main";
                else                 flavor = "Strong %s player";
                ImGui::PushFont(g_font_small);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                if (rank <= 3) {
                    ImGui::Text(flavor, sig.c_str(), p->region.c_str());
                } else {
                    // "Notable %s main" / "Strong %s player" — no region token.
                    ImGui::Text(flavor, sig.c_str());
                }
                ImGui::PopStyleColor();
                ImGui::PopFont();
            }
        }

        // Playstyle = the deep 32-archetype (Player::archetype). This is the
        // stable, simulation-driving identity assigned at spawn — one of 32,
        // never a generic "Generalist". Rendered prominently in the archetype
        // accent (purple). The OLD emergent playstyle_identity() is kept only
        // as a small secondary "tendency" note, and ONLY when it surfaces
        // something meaningful — the generic "Generalist" fallback is
        // suppressed entirely so it never reads as bland.
        {
            ImGui::PushFont(g_font_h2 ? g_font_h2 : g_font_body);
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kAccent2));
            ImGui::Text("PLAYSTYLE: %s", vlr::archetype_name(p->archetype));
            ImGui::PopStyleColor();
            ImGui::PopFont();

            std::string ident = p->playstyle_identity();
            if (!ident.empty() && ident != "Generalist") {
                bool volatile_ident = (ident == "LAN Choker"
                                    || ident == "Momentum Player");
                ImU32 col = volatile_ident ? kVlrRedDim : kVlrSub;
                ImGui::PushFont(g_font_small ? g_font_small : g_font_body);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(col));
                ImGui::Text("Tendency: %s", ident.c_str());
                ImGui::PopStyleColor();
                ImGui::PopFont();
            }
        }

        // Position chip — canonical 4-value gameplay slot (Duelist /
        // Controller / Sentinel / Initiator), derived purely from
        // primary_role via position_of(). Flex and IGL are INDEPENDENT
        // overlay flags appended as suffix chips when set, in canonical
        // order: position first, then • Flex, then • IGL. A "Flex IGL"
        // renders as e.g. "POSITION: Initiator • Flex • IGL".
        {
            vlr::Position pos = vlr::position_of(*p);
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(position_color(pos)));
            ImGui::Text("POSITION: %s", vlr::position_name(pos));
            ImGui::PopStyleColor();
            if (p->is_flex) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kRoleFlex));
                ImGui::TextUnformatted("\xE2\x80\xA2 Flex");
                ImGui::PopStyleColor();
            }
            if (p->is_igl) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kIglBadge));
                ImGui::TextUnformatted("\xE2\x80\xA2 IGL");
                ImGui::PopStyleColor();
            }
        }

        // === Archetype + development row ===============================
        // Deep simulation-driving playstyle (distinct from the emergent
        // PLAYSTYLE label above) shown as a prominent purple Pill, plus an
        // OVR -> POT line and a development-trajectory Pill so the user can
        // instantly read whether this is a rising star, an aging vet, etc.
        // POT is shown directly here (user explicitly requested visibility
        // for squad planning) regardless of God Mode.
        //
        // Trajectory rule: derive from age + (potential - ovr) gap +
        // consistency (inconsistency => high-risk prospect).
        auto dev_trajectory = [](const vlr::Player& pl)
            -> std::pair<const char*, ImU32> {
            int ovr = (int)std::lround(pl.ovr());
            int gap = pl.potential - ovr;
            bool young = (pl.age <= 22);
            bool inconsistent = (pl.consistency > 0 && pl.consistency < 45);
            if (young && gap >= 8 && pl.potential >= 86)
                return {"Future Star \xE2\x96\xB2", kVlrGreen};
            if (young && gap >= 8 && inconsistent)
                return {"High-Risk Prospect", kRoleFlex};
            if (gap >= 8 && young)
                return {"Rising \xE2\x96\xB2", kVlrGreen};
            if (ovr > pl.potential || pl.age >= 29)
                return {"Declining \xE2\x96\xBC", kVlrRedDim};
            if (gap >= 4)
                return {"Developing \xE2\x96\xB2", kAccent};
            return {"Established \xE2\x80\x94", kVlrSub};
        };
        {
            int ovr_i = (int)std::lround(p->ovr());
            // (Archetype is shown as the prominent PLAYSTYLE line above —
            // no duplicate pill here.) OVR -> POT pill (gold = prestige).
            char ovrpot[48];
            std::snprintf(ovrpot, sizeof(ovrpot), "OVR %d \xE2\x86\x92 POT %d",
                          ovr_i, p->potential);
            Pill(ovrpot, kSurfaceHi, kVlrText);
            ImGui::SameLine();
            // Development-trajectory pill (color-coded).
            auto traj = dev_trajectory(*p);
            Pill(traj.first, traj.second);
            ImGui::SameLine();
            // Desire pill — Agent B's NEGOTIATION-driving preference system.
            // Colour mapping per Phase A spec: Greedy=gold (money), Loyal=blue
            // (team), RingChaser=red (trophy), Mercenary=amber (hired-gun),
            // StabilitySeeker=green (steady), Competitor=electric (fire).
            {
                ImU32 dcol = kAccent;
                switch (p->desire) {
                    case vlr::Desire::Greedy:          dcol = kVlrGold;   break;
                    case vlr::Desire::Loyal:           dcol = kVlrBlue;   break;
                    case vlr::Desire::RingChaser:      dcol = kVlrRed;    break;
                    case vlr::Desire::Mercenary:       dcol = kRoleFlex;  break;
                    case vlr::Desire::StabilitySeeker: dcol = kVlrGreen;  break;
                    case vlr::Desire::Competitor:      dcol = kAccent;    break;
                    default:                           dcol = kAccent;    break;
                }
                Pill(vlr::desire_name(p->desire), dcol);
            }
        }
        // One-line desire blurb — gives the user a single-sentence read on
        // WHY this player negotiates the way they do. Suppressed when empty.
        {
            const char* db = vlr::desire_blurb(p->desire);
            if (db && db[0]) MutedText("Desire: %s", db);
        }

        if (clicked_queue) {
            // Find the SoloQEngine for this player's region (defensive: fall
            // back to any region if their region isn't found).
            std::shared_ptr<vlr::SoloQEngine> engine;
            auto it = s.gm.solo_qs.find(p->region);
            if (it != s.gm.solo_qs.end()) engine = it->second;
            if (!engine && !s.gm.solo_qs.empty()) engine = s.gm.solo_qs.begin()->second;
            if (engine) {
                auto rec = engine->force_solo_match(p);
                if (rec) {
                    s.show_player_modal = false;
                    ImGui::CloseCurrentPopup();
                    OpenReplay(s, rec);
                    s.log.emplace_back("Queued ranked match for " + p->name + " — opening live viewer.");
                } else {
                    s.log.emplace_back("Could not queue match (not enough nearby-MMR players).");
                }
            }
        }

        EndCard();
        VSpace(8);

        if (ImGui::BeginTabBar("##playertabs")) {
            // === Overview ===
            if (ImGui::BeginTabItem("Overview")) {
                // === Phase B (C4): Best on-team chemistry partner =========
                // For active roster players, walk current team and ask
                // Agent A's O(1) `chemistry_between` for each teammate.
                // Render a single MutedText if a strong duo OR friction
                // exists; suppress entirely for FA / Retired / no team.
                if (!p->is_retired
                    && p->team_name != "Free Agent"
                    && !p->team_name.empty()
                    && p->team_name != "Retired") {
                    vlr::TeamPtr cur_team;
                    for (auto& kv : s.gm.leagues) {
                        if (!kv.second) continue;
                        for (auto& tt : kv.second->teams()) {
                            if (tt && tt->name == p->team_name) {
                                cur_team = tt; break;
                            }
                        }
                        if (cur_team) break;
                    }
                    if (cur_team) {
                        const vlr::Player* best_mate = nullptr;
                        double best_val = 0.0;
                        for (auto& mate : cur_team->roster) {
                            if (!mate || mate.get() == p.get()) continue;
                            double v = cur_team->chemistry_between(
                                *p, *mate);
                            if (best_mate == nullptr
                                || std::abs(v) > std::abs(best_val)) {
                                best_mate = mate.get();
                                best_val  = v;
                            }
                        }
                        if (best_mate && best_val >= 0.5) {
                            MutedText("Strong duo: \xE2\x86\x94 %s (%+.2f)",
                                      best_mate->name.c_str(),
                                      best_val);
                        } else if (best_mate && best_val <= -0.5) {
                            ImGui::PushStyleColor(ImGuiCol_Text,
                                                  toV4(kVlrRedDim));
                            if (g_font_small) ImGui::PushFont(g_font_small);
                            ImGui::Text("Friction: \xE2\x86\x94 %s (%+.2f)",
                                        best_mate->name.c_str(),
                                        best_val);
                            if (g_font_small) ImGui::PopFont();
                            ImGui::PopStyleColor();
                        }
                    }
                }

                ImGui::Columns(2, nullptr, false);

                H2("OVERALL");
                // OVR / POT side-by-side tiles so long-term ceiling reads
                // at a glance next to current ability.
                {
                    char ovv[12], potv[12];
                    std::snprintf(ovv,  sizeof(ovv),  "%d",
                                  (int)std::lround(p->ovr()));
                    std::snprintf(potv, sizeof(potv), "%d", p->potential);
                    bool grows = (p->potential > (int)std::lround(p->ovr()));
                    StatTile("OVR", ovv, kAccent, ImVec2(120, 78));
                    ImGui::SameLine();
                    StatTile("POT", potv, grows ? kVlrGreen : kVlrSub,
                             ImVec2(120, 78));
                }
                // Form proxy: recent-vs-career rating delta (no dedicated
                // morale/form field on Player — avg_match_rating is the
                // honest signal available).
                {
                    double r = p->avg_match_rating();
                    const char* fl = r >= 1.10 ? "Hot" :
                                     r >= 0.95 ? "Steady" : "Cold";
                    ImGui::Text("Form: %s (%.2f avg)", fl, r);
                }
                ImGui::Text("Age: %d   Work Ethic: %d   Consistency: %d",
                            p->age, p->work_ethic, p->consistency);
                ImGui::Text("Contract: %dY left  -  $%dK/yr (exp %d)",
                            p->years_left(s.gm.year), p->contract.amount_k,
                            p->contract.exp_year);
                ImGui::Spacing();

                H2("RANKED");
                ImGui::Text("MMR %d  (%s)  Peak %d", p->solo_mmr, p->rank_name().c_str(), p->peak_mmr);
                ImGui::Text("K/D %.2f  W %d  L %d", p->solo_kd(), p->solo_wins, p->solo_losses);
                ImGui::Spacing();

                H2("CAREER");
                ImGui::Text("Matches: %d", p->career_matches);
                ImGui::Text("K/D: %.2f", p->kd_ratio());
                ImGui::Text("Avg Rating: %.2f", p->avg_match_rating());
                ImGui::Text("KAST: %.1f%%", p->kast());

                ImGui::NextColumn();

                H2("ATTRIBUTES");
                for (std::size_t i = 0; i < vlr::kAttrCount; ++i) {
                    const char* nm = vlr::attr_name(static_cast<vlr::Attr>(i));
                    if (s.god_mode) {
                        DrawAttrSlider(nm, p->attributes[i]);
                    } else {
                        ImGui::Text("%-15s", nm);
                        ImGui::SameLine(170);
                        ImGui::ProgressBar(p->attributes[i] / 99.0f, ImVec2(180, 0), "");
                        ImGui::SameLine();
                        ImGui::Text("%d", p->attributes[i]);
                    }
                }

                ImGui::Columns(1);

                if (s.god_mode) {
                    // === C5: Negotiation dev-tools reveal ====================
                    // God-mode-only block surfacing Agent B's Desire enum +
                    // blurb so the user can see WHY a sign was refused or a
                    // re-sign succeeded. Read-only — Desire is generation-
                    // assigned and stable for the career.
                    ThinDivider(8);
                    SectionHeader("DEV TOOLS \xE2\x80\x94 NEGOTIATION",
                        "Drives gen_contract, refuses_to_negotiate, "
                        "and re-sign acceptance.", kVlrGold);
                    {
                        ImU32 dcol = kAccent;
                        switch (p->desire) {
                            case vlr::Desire::Greedy:          dcol = kVlrGold;   break;
                            case vlr::Desire::Loyal:           dcol = kVlrBlue;   break;
                            case vlr::Desire::RingChaser:      dcol = kVlrRed;    break;
                            case vlr::Desire::Mercenary:       dcol = kRoleFlex;  break;
                            case vlr::Desire::StabilitySeeker: dcol = kVlrGreen;  break;
                            case vlr::Desire::Competitor:      dcol = kAccent;    break;
                            default:                           dcol = kAccent;    break;
                        }
                        Pill(vlr::desire_name(p->desire), dcol);
                        const char* db = vlr::desire_blurb(p->desire);
                        if (db && db[0]) MutedText("%s", db);
                    }
                    ThinDivider(8);
                    SectionHeader("DEV TOOLS",
                        "God Mode editor — direct attribute overrides", kVlrGold);
                    ImGui::SetNextItemWidth(120); ImGui::DragInt("Age", &p->age, 1.0f, 14, 50);
                    ImGui::SameLine(260);
                    ImGui::SetNextItemWidth(120); ImGui::DragInt("Potential", &p->potential, 1.0f, 1, 99);
                    ImGui::SameLine(500);
                    ImGui::SetNextItemWidth(120); ImGui::DragInt("Work Ethic", &p->work_ethic, 1.0f, 1, 99);

                    ImGui::SetNextItemWidth(120); ImGui::DragInt("Consistency", &p->consistency, 1.0f, 1, 99);
                    ImGui::SameLine(260);
                    ImGui::SetNextItemWidth(120); ImGui::DragInt("MMR", &p->solo_mmr, 5.0f, 1100, 5000);
                    ImGui::SameLine(500);
                    // Bounds tied to the engine-side salary range
                    // (vlr::kSalaryFloorK .. vlr::kSalaryCapK) — Agent A
                    // dropped the cap from 999 to 180, so the old magic
                    // numbers would let god mode push contracts above the
                    // legal ceiling.
                    ImGui::SetNextItemWidth(120); ImGui::DragInt("Salary $K", &p->contract.amount_k, 1.0f, vlr::kSalaryFloorK, vlr::kSalaryCapK);

                    ImGui::SetNextItemWidth(120); ImGui::DragInt("Signed Yrs (snapshot)", &p->contract_years, 1.0f, 0, 6);
                    ImGui::SameLine(260);
                    ImGui::SetNextItemWidth(120); ImGui::DragInt("Exp Year", &p->contract.exp_year, 1.0f, 2020, 2100);
                    ImGui::SameLine(500);
                    ImGui::SetNextItemWidth(120); ImGui::DragInt("Peak Age", &p->peak_age, 1.0f, 18, 35);

                    int role_idx = static_cast<int>(p->primary_role);
                    const char* roles[] = {"Duelist", "Initiator", "Controller", "Sentinel"};
                    ImGui::SetNextItemWidth(140);
                    if (ImGui::Combo("Primary Role", &role_idx, roles, 4)) {
                        p->primary_role = static_cast<vlr::Role>(role_idx);
                    }
                    ImGui::SameLine(260);
                    int arch_idx = static_cast<int>(p->growth_archetype);
                    const char* archs[] = {"Standard", "Early Peaker", "Late Bloomer"};
                    ImGui::SetNextItemWidth(140);
                    if (ImGui::Combo("Growth Curve", &arch_idx, archs, 3)) {
                        p->growth_archetype = static_cast<vlr::GrowthArchetype>(arch_idx);
                    }
                    // Rookie archetype combo (god-mode reveal/edit). 17 entries
                    // (None + 16). Combo's labels generated from helper.
                    ImGui::SameLine(420);
                    int rk_idx = static_cast<int>(p->rookie_archetype);
                    static std::vector<const char*> rk_labels;
                    if (rk_labels.empty()) {
                        for (int i = 0; i <= 16; ++i) {
                            rk_labels.push_back(vlr::rookie_archetype_name(
                                static_cast<vlr::RookieArchetype>(i)));
                        }
                    }
                    ImGui::SetNextItemWidth(180);
                    if (ImGui::Combo("Rookie Archetype", &rk_idx,
                                     rk_labels.data(), (int)rk_labels.size())) {
                        p->rookie_archetype = static_cast<vlr::RookieArchetype>(rk_idx);
                    }
                    if (p->rookie_archetype != vlr::RookieArchetype::None) {
                        ImGui::PushFont(g_font_small);
                        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
                        ImGui::TextWrapped("Style: %s",
                            vlr::rookie_archetype_blurb(p->rookie_archetype));
                        ImGui::PopStyleColor();
                        ImGui::PopFont();
                    }
                    ImGui::Spacing();
                    if (ImGui::Button("Refresh agent pool")) p->update_agent_pool();

                    // Morale per team display + edit
                    ImGui::Spacing();
                    H2("MORALE / MOOD");
                    Sub("Per-team mood: 0 = happy, 1 = will refuse to negotiate.");
                    if (p->mood.per_team.empty()) {
                        ImGui::TextDisabled("(no team-specific mood yet)");
                    } else {
                        int mi = 0;
                        for (auto& kv : p->mood.per_team) {
                            ImGui::PushID(mi++);
                            float mv = (float)kv.second;
                            ImGui::SetNextItemWidth(180);
                            if (ImGui::SliderFloat(kv.first.c_str(), &mv, 0.0f, 1.0f)) {
                                kv.second = vlr::clamp_v((double)mv, 0.0, 1.0);
                            }
                            ImGui::PopID();
                        }
                    }

                    // IGL toggle with proper stat shift
                    ImGui::Spacing();
                    bool was_igl = p->is_igl;
                    if (ImGui::Checkbox("Is IGL", &p->is_igl)) {
                        if (p->is_igl && !was_igl) {
                            // Apply IGL shift
                            p->apply_attribute_delta(vlr::Attr::Aim, -8);
                            p->apply_attribute_delta(vlr::Attr::Headshot, -8);
                            p->apply_attribute_delta(vlr::Attr::Entry, -5);
                            p->apply_attribute_delta(vlr::Attr::Aggressiveness, -4);
                            p->apply_attribute_delta(vlr::Attr::Leadership, +14);
                            p->apply_attribute_delta(vlr::Attr::Intelligence, +10);
                            p->apply_attribute_delta(vlr::Attr::Communication, +10);
                            p->apply_attribute_delta(vlr::Attr::GameSense, +8);
                            p->apply_attribute_delta(vlr::Attr::DecisionMaking, +8);
                            p->apply_attribute_delta(vlr::Attr::MidRoundCalling, +12);
                            p->apply_attribute_delta(vlr::Attr::EconomyMgmt, +12);
                            p->apply_attribute_delta(vlr::Attr::AntiStrat, +10);
                        } else if (!p->is_igl && was_igl) {
                            // Revert IGL shift
                            p->apply_attribute_delta(vlr::Attr::Aim, +8);
                            p->apply_attribute_delta(vlr::Attr::Headshot, +8);
                            p->apply_attribute_delta(vlr::Attr::Entry, +5);
                            p->apply_attribute_delta(vlr::Attr::Aggressiveness, +4);
                            p->apply_attribute_delta(vlr::Attr::Leadership, -14);
                            p->apply_attribute_delta(vlr::Attr::Intelligence, -10);
                            p->apply_attribute_delta(vlr::Attr::Communication, -10);
                            p->apply_attribute_delta(vlr::Attr::GameSense, -8);
                            p->apply_attribute_delta(vlr::Attr::DecisionMaking, -8);
                            p->apply_attribute_delta(vlr::Attr::MidRoundCalling, -12);
                            p->apply_attribute_delta(vlr::Attr::EconomyMgmt, -12);
                            p->apply_attribute_delta(vlr::Attr::AntiStrat, -10);
                        }
                        p->update_agent_pool();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Force retire")) {
                        p->is_retired = true;
                        p->team_name = "Retired";
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Reset MMR to 1100")) p->solo_mmr = 1100;
                }

                ImGui::EndTabItem();
            }

            // === Detail (badges, awards, agent pool, history) ===
            if (ImGui::BeginTabItem("Detail")) {
                // === Personal info block ===
                H2("PERSONAL");
                ImGui::Columns(2, nullptr, false);
                ImGui::SetColumnWidth(0, 320);
                ImGui::Text("Real name:"); ImGui::SameLine(140);
                ImGui::Text("%s %s", p->first.c_str(), p->last.c_str());
                ImGui::Text("Born:"); ImGui::SameLine(140);
                if (p->birth_year > 0) {
                    ImGui::Text("%d  (age %d)", p->birth_year, p->age);
                } else {
                    ImGui::Text("age %d", p->age);
                }
                ImGui::Text("Hometown:"); ImGui::SameLine(140);
                ImGui::Text("%s, %s",
                            p->birth_city.empty() ? "?" : p->birth_city.c_str(),
                            p->country.empty() ? "?" : p->country.c_str());
                ImGui::Text("Country:"); ImGui::SameLine(140);
                CountryFlag(p->country_iso); ImGui::SameLine();
                ImGui::Text("%s", p->country.empty() ? "?" : p->country.c_str());
                ImGui::Text("Region:"); ImGui::SameLine(140);
                ImGui::Text("%s", p->region.c_str());
                ImGui::NextColumn();
                ImGui::Text("Status:"); ImGui::SameLine(140);
                if (p->is_igl) {
                    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                    ImGui::Text("[I] In-Game Leader");
                    ImGui::PopStyleColor();
                } else {
                    ImGui::Text("Player");
                }
                ImGui::Text("Role:"); ImGui::SameLine(140);
                ImGui::TextColored(toV4(role_color(p->primary_role)),
                                   "%s", vlr::role_name(p->primary_role));
                ImGui::Text("Team:"); ImGui::SameLine(140);
                ImGui::Text("%s", p->team_name.c_str());
                ImGui::Text("Contract:"); ImGui::SameLine(140);
                ImGui::Text("%dY left  $%dK/yr (exp %d)",
                            p->years_left(s.gm.year), p->contract.amount_k, p->contract.exp_year);
                ImGui::Columns(1);
                ImGui::Spacing();

                if (!p->agent_pool.empty()) {
                    H2("PRIMARY AGENT POOL");
                    for (auto* a : p->agent_pool) {
                        ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Button, toV4(role_color(a->role)));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(role_color(a->role)));
                        ImGui::SmallButton(a->name.c_str());
                        ImGui::PopStyleColor(2);
                    }
                    ImGui::NewLine();
                }
                if (!p->badges.empty() || s.god_mode) {
                    H2("BADGES");
                    int bi = 0;
                    for (auto it = p->badges.begin(); it != p->badges.end(); ) {
                        ImGui::SameLine();
                        ImGui::PushID(bi++);
                        ImGui::PushStyleColor(ImGuiCol_Button, toV4(kVlrPanel2));
                        ImGui::SmallButton(it->c_str());
                        ImGui::PopStyleColor();
                        // God mode: right-click to remove the badge
                        bool removed = false;
                        if (s.god_mode && ImGui::BeginPopupContextItem("##badge_ctx")) {
                            if (ImGui::MenuItem("Remove badge")) {
                                it = p->badges.erase(it);
                                removed = true;
                            }
                            ImGui::EndPopup();
                        }
                        ImGui::PopID();
                        if (!removed) ++it;
                    }
                    if (s.god_mode) {
                        ImGui::NewLine();
                        ImGui::PushFont(g_font_small);
                        ImGui::TextDisabled("Right-click a badge to remove. Add new:");
                        ImGui::PopFont();
                        // Combo of all known badges; "Add" button appends.
                        static int s_badge_pick = 0;
                        const auto& BAD = vlr::badges();
                        std::vector<const char*> labels;
                        labels.reserve(BAD.size());
                        for (auto& b : BAD) labels.push_back(b.name.c_str());
                        if (s_badge_pick >= (int)labels.size()) s_badge_pick = 0;
                        ImGui::SetNextItemWidth(220);
                        ImGui::Combo("##add_badge_pick", &s_badge_pick,
                                     labels.data(), (int)labels.size());
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Add")) {
                            const auto& bd = BAD[(std::size_t)s_badge_pick];
                            // Don't double-add the same badge
                            bool exists = false;
                            for (auto& b : p->badges) if (b == bd.name) { exists = true; break; }
                            if (!exists) {
                                p->badges.push_back(bd.name);
                                for (auto& m : bd.mods)
                                    p->apply_attribute_delta(m.stat, m.delta);
                            }
                        }
                    }
                    ImGui::NewLine();
                }
                if (!p->awards.empty()) {
                    H2("AWARDS");
                    // === Trophy audit indicator =================================
                    // Sums [T]+[M]+[W] championship wins using the helper
                    // award_count_by_prefix (NOT raw awards.size(), which would
                    // also include [A] season awards). Surfaces total trophies
                    // at a glance so the user can spot anomalies (phantom
                    // championships, double-pinned awards, etc.).
                    int n_regional = p->award_count_by_prefix("[T] ");
                    int n_masters  = p->award_count_by_prefix("[M] ");
                    int n_world    = p->award_count_by_prefix("[W] ");
                    int n_trophies = n_regional + n_masters + n_world;

                    // Anomaly flag: >10 championships for a player under 25
                    // is unrealistic. Tint the totals line red so it screams.
                    bool age_anomaly = (n_trophies > 10 && p->age < 25);
                    ImU32 total_col = age_anomaly ? kVlrRed : kVlrSub;
                    ImGui::PushStyleColor(ImGuiCol_Text, toV4(total_col));
                    ImGui::Text("Total trophies: %d  (T:%d  M:%d  W:%d)",
                                n_trophies, n_regional, n_masters, n_world);
                    ImGui::PopStyleColor();

                    // === Optional integrity cross-check =========================
                    // Count how many currently-active tournaments list this
                    // player in their winning_roster_snapshot(). Compare against
                    // this season's [T]/[M]/[W] count for the player. Mismatch
                    // = either a phantom pin or a missing one — surface a small
                    // warning. By year-end active_tournaments may be empty, in
                    // which case skip (nothing to verify against).
                    int cur_year = s.gm.year;
                    int snap_hits = 0;
                    int snapshots_seen = 0;
                    for (auto& tour : s.gm.active_tournaments) {
                        if (!tour || !tour->finished()) continue;
                        const auto& snap = tour->winning_roster_snapshot();
                        if (snap.empty()) continue;
                        ++snapshots_seen;
                        for (vlr::Player* pp : snap) {
                            if (pp == p.get()) { ++snap_hits; break; }
                        }
                    }
                    if (snapshots_seen > 0) {
                        // Count this player's trophies stamped with the current
                        // year suffix — those are the ones an active tournament
                        // would still hold a snapshot for.
                        std::string ystr = std::to_string(cur_year);
                        int year_trophies = 0;
                        for (auto& a : p->awards) {
                            if (a.size() < 4) continue;
                            if (a[0] != '[' || a[2] != ']') continue;
                            char k = a[1];
                            if (k != 'T' && k != 'M' && k != 'W') continue;
                            if (a.size() >= ystr.size()
                                && a.compare(a.size() - ystr.size(),
                                             ystr.size(), ystr) == 0) {
                                ++year_trophies;
                            }
                        }
                        if (snap_hits != year_trophies) {
                            ImGui::SameLine();
                            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
                            ImGui::PushFont(g_font_small);
                            ImGui::Text("  (integrity check failed: snap=%d vs year=%d)",
                                        snap_hits, year_trophies);
                            ImGui::PopFont();
                            ImGui::PopStyleColor();
                        }
                    }

                    // Color-coded by category prefix:
                    //   [W] World/Champions  = gold
                    //   [M] Masters          = silver-grey
                    //   [T] Regional         = bronze (orange)
                    //   [A] Season Award     = green
                    //   default              = subtext
                    for (auto& a : p->awards) {
                        ImU32 col = kVlrSub;
                        const char* cat = "";
                        if (a.rfind("[W]", 0) == 0) {
                            col = kVlrGold; cat = "WORLD ";
                        } else if (a.rfind("[M]", 0) == 0) {
                            col = IM_COL32(0xC0, 0xC0, 0xC8, 0xFF); cat = "MASTERS ";
                        } else if (a.rfind("[T]", 0) == 0) {
                            col = IM_COL32(0xCD, 0x7F, 0x32, 0xFF); cat = "REGIONAL ";
                        } else if (a.rfind("[A]", 0) == 0) {
                            col = kVlrGreen; cat = "AWARD ";
                        }
                        ImGui::Bullet();
                        ImGui::PushStyleColor(ImGuiCol_Text, toV4(col));
                        ImGui::PushFont(g_font_small);
                        ImGui::TextUnformatted(cat);
                        ImGui::PopFont();
                        ImGui::PopStyleColor();
                        ImGui::SameLine();
                        // Strip the [X] prefix in the displayed text
                        std::string display = a;
                        if (display.size() > 3 && display[0] == '[' && display[2] == ']')
                            display = display.substr(4);
                        ImGui::Text("%s", display.c_str());
                    }
                }
                if (!p->history.empty()) {
                    H2("SEASON HISTORY");
                    int total_earnings_k = 0;
                    if (ImGui::BeginTable("##hist", 7,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
                        ImGui::TableSetupColumn("YEAR");
                        ImGui::TableSetupColumn("AGE");
                        ImGui::TableSetupColumn("TEAM");
                        ImGui::TableSetupColumn("RAT");
                        ImGui::TableSetupColumn("KD");
                        ImGui::TableSetupColumn("KAST");
                        ImGui::TableSetupColumn("EARNED");
                        ImGui::TableHeadersRow();
                        for (auto& e : p->history) {
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn(); ImGui::Text("%d", e.year);
                            ImGui::TableNextColumn(); ImGui::Text("%d", e.age);
                            ImGui::TableNextColumn(); ImGui::Text("%s", e.team.c_str());
                            ImGui::TableNextColumn(); ImGui::Text("%.2f", e.rating);
                            ImGui::TableNextColumn(); ImGui::Text("%.2f", e.kd);
                            ImGui::TableNextColumn(); ImGui::Text("%.1f", e.kast);
                            ImGui::TableNextColumn();
                            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGreen));
                            ImGui::Text("$%dK", e.salary_k);
                            ImGui::PopStyleColor();
                            total_earnings_k += e.salary_k;
                        }
                        // Career total footer row
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                        ImGui::Text("TOTAL");
                        ImGui::PopStyleColor();
                        ImGui::TableNextColumn(); ImGui::TextDisabled("-");
                        ImGui::TableNextColumn(); ImGui::TextDisabled("Career");
                        ImGui::TableNextColumn(); ImGui::TextDisabled("-");
                        ImGui::TableNextColumn(); ImGui::TextDisabled("-");
                        ImGui::TableNextColumn(); ImGui::TextDisabled("-");
                        ImGui::TableNextColumn();
                        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                        if (total_earnings_k >= 1000) {
                            ImGui::Text("$%.2fM", total_earnings_k / 1000.0);
                        } else {
                            ImGui::Text("$%dK", total_earnings_k);
                        }
                        ImGui::PopStyleColor();
                        ImGui::EndTable();
                    }
                }
                ImGui::EndTabItem();
            }

            // === Match History ===
            if (ImGui::BeginTabItem("Match History")) {
                ImGui::RadioButton("Pro Matches",  &s.history_filter, 0); ImGui::SameLine();
                ImGui::RadioButton("Solo Q",       &s.history_filter, 1);
                ImGui::Separator();

                auto& list = (s.history_filter == 0) ? p->pro_match_history : p->solo_match_history;
                ImGui::TextDisabled("Click any row to watch it round-by-round.");
                if (list.empty()) {
                    ImGui::TextDisabled("(no matches yet)");
                } else if (ImGui::BeginTable("##matches", 9,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                        ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, 440))) {
                    ImGui::TableSetupColumn("EVENT", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("MAP");
                    ImGui::TableSetupColumn("MATCHUP");
                    ImGui::TableSetupColumn("SCORE");
                    ImGui::TableSetupColumn("W/L");
                    ImGui::TableSetupColumn("K-D-A");
                    ImGui::TableSetupColumn("HS%");
                    ImGui::TableSetupColumn("RATING");
                    ImGui::TableSetupColumn("MVP");
                    ImGui::TableHeadersRow();

                    // Capture which row was clicked this frame; act on it
                    // AFTER the table closes so we can safely close the
                    // outer popup before opening the live-match popup.
                    vlr::RecordedMatchPtr clicked_rec;
                    int i = 0;
                    for (auto& rec : list) {
                        if (!rec) continue;
                        ImGui::PushID(i++);
                        int my_k = 0, my_d = 0, my_a = 0;
                        double my_rating = 1.0;
                        if (rec->match_stats) {
                            auto sit = rec->match_stats->find(p.get());
                            if (sit != rec->match_stats->end()) {
                                my_k = sit->second.k;
                                my_d = sit->second.d;
                                my_a = sit->second.a;
                                my_rating = sit->second.rating;
                            }
                        }
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        bool clicked = ImGui::Selectable(rec->event.c_str(), false,
                                ImGuiSelectableFlags_SpanAllColumns);
                        ImGui::TableNextColumn(); ImGui::Text("%s", rec->map_name.c_str());
                        ImGui::TableNextColumn();
                        {
                            char buf[80];
                            std::snprintf(buf, sizeof(buf), "%s vs %s",
                                          rec->blue_name.c_str(), rec->red_name.c_str());
                            ImGui::Text("%s", buf);
                        }
                        ImGui::TableNextColumn(); ImGui::Text("%s", rec->score.c_str());
                        // W/L from THE PLAYER'S perspective at the time —
                        // use the recording's snapshotted blue/red rosters
                        // (history_record->blue_team / red_team) to figure
                        // out which side the player was actually on for
                        // this specific map. Rosters change over time, so
                        // checking by team_name string would be wrong.
                        ImGui::TableNextColumn();
                        {
                            bool was_blue = false, was_red = false;
                            if (rec->history_record) {
                                // blue_team / red_team are vector<Player*>
                                // (raw pointers), not shared_ptr.
                                for (auto* bp : rec->history_record->blue_team)
                                    if (bp == p.get()) { was_blue = true; break; }
                                if (!was_blue) {
                                    for (auto* rp : rec->history_record->red_team)
                                        if (rp == p.get()) { was_red = true; break; }
                                }
                            }
                            const char* result = "—";
                            ImU32 col = kVlrSub;
                            if (was_blue || was_red) {
                                bool won = was_blue
                                    ? (rec->blue_score > rec->red_score)
                                    : (rec->red_score  > rec->blue_score);
                                result = won ? "W" : "L";
                                col = won ? kVlrGreen : kVlrRed;
                            }
                            ImGui::PushStyleColor(ImGuiCol_Text, toV4(col));
                            ImGui::Text("%s", result);
                            ImGui::PopStyleColor();
                        }
                        ImGui::TableNextColumn(); ImGui::Text("%d / %d / %d", my_k, my_d, my_a);
                        ImGui::TableNextColumn();
                        // Per-match HS% from PlayerLine.hs (stored in history_record).
                        // Note: NOT career_hs_pct() — that would show the cumulative
                        // value on every row, which defeats the point of a per-match
                        // breakdown.
                        {
                            int my_hs = -1;
                            if (rec->history_record) {
                                auto hit = rec->history_record->stats.find(p.get());
                                if (hit != rec->history_record->stats.end()) {
                                    my_hs = hit->second.hs;
                                }
                            }
                            if (my_hs >= 0) ImGui::Text("%d%%", my_hs);
                            else            ImGui::TextDisabled("—");
                        }
                        ImGui::TableNextColumn();
                        {
                            ImU32 col = my_rating >= 1.10 ? kVlrGreen : (my_rating >= 0.95 ? kVlrText : kVlrRed);
                            ImGui::PushStyleColor(ImGuiCol_Text, toV4(col));
                            ImGui::Text("%.2f", my_rating);
                            ImGui::PopStyleColor();
                        }
                        ImGui::TableNextColumn(); ImGui::Text("%s", rec->mvp_name.c_str());
                        if (clicked) clicked_rec = rec;
                        ImGui::PopID();
                    }
                    ImGui::EndTable();

                    // Close the player profile modal now (after EndTable)
                    // so the live-match popup that we open next frame
                    // doesn't get stacked under us.
                    if (clicked_rec) {
                        s.show_player_modal = false;
                        ImGui::CloseCurrentPopup();
                        OpenReplay(s, clicked_rec);
                    }
                }
                ImGui::EndTabItem();
            }

            // === Map Mastery ===
            // Mirrors the agent-mastery flow but on the per-map dimension.
            // Comfort map emerges from sustained pro play; map_mastery_bonus
            // (±12) feeds per-map agent selection in Team::build_round_selection.
            if (ImGui::BeginTabItem("Map Mastery")) {
                ImGui::PushID("##mapmastery_tab");
                Sub("Per-map career stats. Comfort maps emerge from sustained "
                    "pro play. Decays on disuse.");
                ImGui::Spacing();

                // Comfort-map strip — gold accent if established, otherwise
                // "not yet established" hint.
                {
                    std::string comfort = p->comfort_map();
                    if (!comfort.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                        ImGui::Text("[*] Comfort Map: %s", comfort.c_str());
                        ImGui::PopStyleColor();
                    } else {
                        Sub("Comfort map: not yet established (need 5+ matches "
                            "on a single map).");
                    }
                }
                ImGui::Spacing();

                if (p->map_mastery.empty()) {
                    ImGui::TextDisabled("No pro matches played yet.");
                } else {
                    struct MapRow {
                        std::string name;
                        int    matches = 0;
                        double avg_rating = 0.0;
                        double peak_rating = 0.0;
                        int    last_year = 0;
                        double bonus = 0.0;
                        bool   is_comfort = false;
                    };
                    std::string comfort = p->comfort_map();
                    std::vector<MapRow> mrows;
                    mrows.reserve(vlr::maps().size());
                    // Iterate ALL canonical maps so the user sees every map in
                    // the pool — even the ones with 0 matches show as "—".
                    for (auto& gm : vlr::maps()) {
                        MapRow r;
                        r.name = gm.name;
                        auto it = p->map_mastery.find(gm.name);
                        if (it != p->map_mastery.end()) {
                            r.matches = it->second.matches;
                            r.avg_rating = it->second.avg_rating;
                            r.peak_rating = it->second.peak_rating;
                            r.last_year = it->second.last_played_year;
                        }
                        r.bonus = p->map_mastery_bonus(gm.name);
                        r.is_comfort = !comfort.empty() && comfort == gm.name;
                        mrows.push_back(r);
                    }
                    // Sort by matches desc, then peak_rating desc as tiebreak.
                    std::sort(mrows.begin(), mrows.end(),
                        [](const MapRow& a, const MapRow& b) {
                            if (a.matches != b.matches) return a.matches > b.matches;
                            return a.peak_rating > b.peak_rating;
                        });

                    if (ImGui::BeginTable("##map_mastery", 7,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                            ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, 320))) {
                        ImGui::TableSetupColumn("#",       ImGuiTableColumnFlags_WidthFixed, 32);
                        ImGui::TableSetupColumn("MAP",     ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("MATCHES", ImGuiTableColumnFlags_WidthFixed, 80);
                        ImGui::TableSetupColumn("AVG RATING", ImGuiTableColumnFlags_WidthFixed, 90);
                        ImGui::TableSetupColumn("PEAK",    ImGuiTableColumnFlags_WidthFixed, 70);
                        ImGui::TableSetupColumn("LAST PLAYED", ImGuiTableColumnFlags_WidthFixed, 100);
                        ImGui::TableSetupColumn("BONUS",   ImGuiTableColumnFlags_WidthFixed, 70);
                        ImGui::TableHeadersRow();
                        int rank = 1;
                        for (auto& r : mrows) {
                            ImGui::TableNextRow();
                            if (r.is_comfort) ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));

                            ImGui::TableNextColumn();
                            if (!r.is_comfort) ImGui::PushStyleColor(ImGuiCol_Text, toV4(placement_color(rank)));
                            ImGui::Text("%d", rank);
                            if (!r.is_comfort) ImGui::PopStyleColor();
                            ++rank;

                            ImGui::TableNextColumn();
                            ImGui::Text("%s", r.name.c_str());

                            ImGui::TableNextColumn();
                            if (r.matches > 0) ImGui::Text("%d", r.matches);
                            else               ImGui::TextDisabled("—");

                            ImGui::TableNextColumn();
                            if (r.matches > 0) ImGui::Text("%.2f", r.avg_rating);
                            else               ImGui::TextDisabled("—");

                            ImGui::TableNextColumn();
                            if (r.peak_rating > 0.0) ImGui::Text("%.2f", r.peak_rating);
                            else                     ImGui::TextDisabled("—");

                            ImGui::TableNextColumn();
                            if (r.last_year > 0) ImGui::Text("%d", r.last_year);
                            else                 ImGui::TextDisabled("—");

                            ImGui::TableNextColumn();
                            if (r.bonus > 0.05 || r.bonus < -0.05)
                                ImGui::Text("%+.1f", r.bonus);
                            else
                                ImGui::TextDisabled("—");

                            if (r.is_comfort) ImGui::PopStyleColor();
                        }
                        ImGui::EndTable();
                    }
                    Sub("BONUS feeds per-map agent selection (+12 cap). Mastery "
                        "decays on disuse.");
                }

                // === God-mode map-mastery editor ===
                // Mirrors the agent-mastery editor — direct backfill of the
                // map_mastery table. Same gating, same buttons.
                if (s.god_mode) {
                    ImGui::Spacing();
                    H2("EDIT MAP MASTERY (God Mode)");
                    Sub("Backfill map mastery directly. Matches > 0 marks the "
                        "player as having played that map; avg_rating and "
                        "peak_rating shape the map_mastery_bonus contribution.");

                    if (ImGui::BeginTable("##gm_map_mastery", 4,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                            ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, 320))) {
                        ImGui::TableSetupColumn("MAP", ImGuiTableColumnFlags_WidthFixed, 130);
                        ImGui::TableSetupColumn("MATCHES",    ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("AVG RATING", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("PEAK",       ImGuiTableColumnFlags_WidthFixed, 90);
                        ImGui::TableHeadersRow();
                        int gmi = 0;
                        for (auto& gm : vlr::maps()) {
                            ImGui::PushID(gmi++);
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::Text("%s", gm.name.c_str());

                            auto& m = p->map_mastery[gm.name];
                            int    matches = m.matches;
                            float  avg     = static_cast<float>(m.avg_rating);
                            float  peak    = static_cast<float>(m.peak_rating);

                            ImGui::TableNextColumn();
                            ImGui::SetNextItemWidth(180);
                            if (ImGui::SliderInt("##mm_matches", &matches, 0, 300)) {
                                m.matches = matches;
                                if (matches == 0 && m.avg_rating == 0.0 && m.peak_rating == 0.0) {
                                    p->map_mastery.erase(gm.name);
                                } else if (m.last_played_year == 0) {
                                    m.last_played_year = vlr::current_world_year();
                                }
                            }

                            ImGui::TableNextColumn();
                            ImGui::SetNextItemWidth(180);
                            if (ImGui::SliderFloat("##mm_avg", &avg, 0.0f, 2.0f, "%.2f")) {
                                m.avg_rating = avg;
                                if (m.peak_rating < m.avg_rating) m.peak_rating = m.avg_rating;
                            }

                            ImGui::TableNextColumn();
                            ImGui::SetNextItemWidth(80);
                            if (ImGui::SliderFloat("##mm_peak", &peak, 0.0f, 2.5f, "%.2f")) {
                                m.peak_rating = peak;
                            }

                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                    if (ImGui::Button("Refresh agent pool from attrs", ImVec2(260, 28))) {
                        p->update_agent_pool();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Clear all map mastery", ImVec2(200, 28))) {
                        p->map_mastery.clear();
                    }
                }
                ImGui::PopID();
                ImGui::EndTabItem();
            }

            // === Agents & Mastery ===
            // The new scoring system: weighted attribute fit (a1 0.45, a2 0.32,
            // a3 0.23) + role match + mastery experience. Pool is auto-derived
            // from this score; manual override removed since it desynced from
            // the simulation contract.
            if (ImGui::BeginTabItem("Agents & Mastery")) {
                Sub("Each agent has 3 signature attributes — the player's "
                    "values for those attributes drive FIT. MASTERY is built "
                    "by playing pro matches on that agent (capped, decays on "
                    "disuse). SCORE = weighted FIT + role match + mastery, "
                    "and the top scorers fill the player's agent pool.");
                ImGui::Spacing();

                std::string sig = p->signature_agent();

                // Pool summary strip + signature
                {
                    std::string pool_str = "Pool: ";
                    if (p->agent_pool.empty()) {
                        pool_str += "(empty)";
                    } else {
                        for (std::size_t i = 0; i < p->agent_pool.size(); ++i) {
                            if (i) pool_str += ", ";
                            pool_str += p->agent_pool[i]->name;
                        }
                    }
                    ImGui::TextUnformatted(pool_str.c_str());
                    if (!sig.empty()) {
                        auto sit = p->agent_mastery.find(sig);
                        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                        if (sit != p->agent_mastery.end()) {
                            ImGui::Text("[*] Signature: %s  (%d matches, %.2f avg, peak %.2f)",
                                sig.c_str(), sit->second.matches,
                                sit->second.avg_rating, sit->second.peak_rating);
                        } else {
                            ImGui::Text("[*] Signature: %s", sig.c_str());
                        }
                        ImGui::PopStyleColor();
                    } else {
                        Sub("Signature agent: not yet established (need 5+ pro "
                            "matches on the same agent).");
                    }
                }
                ImGui::Spacing();

                struct Row {
                    const vlr::Agent* agent;
                    double fit;        // weighted attribute fit, 0..99
                    double mastery;    // mastery_bonus_for (0..12)
                    double score;      // full update_agent_pool score
                    int    matches;
                    double avg_rating;
                    double peak_rating;
                    int    last_year;
                    bool   in_pool;
                };
                std::vector<Row> rows;
                rows.reserve(vlr::agents().size());
                for (auto& a : vlr::agents()) {
                    Row r;
                    r.agent = &a;
                    int v1 = vlr::at(p->attributes, a.a1);
                    int v2 = vlr::at(p->attributes, a.a2);
                    int v3 = vlr::at(p->attributes, a.a3);
                    r.fit = v1 * 0.45 + v2 * 0.32 + v3 * 0.23;
                    r.mastery = p->mastery_bonus_for(a.name);
                    r.score = r.fit + (a.role == p->primary_role ? 6.0 : 0.0) + r.mastery;
                    auto mit = p->agent_mastery.find(a.name);
                    if (mit != p->agent_mastery.end()) {
                        r.matches = mit->second.matches;
                        r.avg_rating = mit->second.avg_rating;
                        r.peak_rating = mit->second.peak_rating;
                        r.last_year = mit->second.last_played_year;
                    } else {
                        r.matches = 0; r.avg_rating = 0.0; r.peak_rating = 0.0; r.last_year = 0;
                    }
                    r.in_pool = false;
                    for (auto* ap : p->agent_pool) if (ap == &a) { r.in_pool = true; break; }
                    rows.push_back(r);
                }
                std::sort(rows.begin(), rows.end(),
                          [](const Row& x, const Row& y) { return x.score > y.score; });

                if (ImGui::BeginTable("##agents_mastery", 9,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                        ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, 380))) {
                    ImGui::TableSetupColumn("#",       ImGuiTableColumnFlags_WidthFixed, 32);
                    ImGui::TableSetupColumn("AGENT",   ImGuiTableColumnFlags_WidthFixed, 96);
                    ImGui::TableSetupColumn("ROLE",    ImGuiTableColumnFlags_WidthFixed, 80);
                    ImGui::TableSetupColumn("KEY ATTRS", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("FIT",     ImGuiTableColumnFlags_WidthFixed, 50);
                    ImGui::TableSetupColumn("MAT",     ImGuiTableColumnFlags_WidthFixed, 50);
                    ImGui::TableSetupColumn("AVG",     ImGuiTableColumnFlags_WidthFixed, 60);
                    ImGui::TableSetupColumn("LAST",    ImGuiTableColumnFlags_WidthFixed, 60);
                    ImGui::TableSetupColumn("SCORE",   ImGuiTableColumnFlags_WidthFixed, 80);
                    ImGui::TableHeadersRow();
                    int rank = 1;
                    for (auto& r : rows) {
                        ImGui::TableNextRow();
                        bool is_sig = !sig.empty() && sig == r.agent->name;
                        if (is_sig) ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));

                        ImGui::TableNextColumn();
                        if (!is_sig) ImGui::PushStyleColor(ImGuiCol_Text, toV4(placement_color(rank)));
                        if (r.in_pool) ImGui::Text("[%d]", rank);
                        else           ImGui::Text(" %d ", rank);
                        if (!is_sig) ImGui::PopStyleColor();
                        ++rank;

                        ImGui::TableNextColumn();
                        if (!is_sig) ImGui::PushStyleColor(ImGuiCol_Text, toV4(role_color(r.agent->role)));
                        ImGui::Text("%s", r.agent->name.c_str());
                        if (!is_sig) ImGui::PopStyleColor();

                        ImGui::TableNextColumn();
                        ImGui::Text("%s", vlr::role_name(r.agent->role));

                        ImGui::TableNextColumn();
                        // Compact "Aim:88 HS:81 React:78" — abbreviates the
                        // most common long names so the column doesn't wrap.
                        auto abbrev_attr = [](vlr::Attr a) -> const char* {
                            const char* nm = vlr::attr_name(a);
                            // Common shortenings.
                            if (std::string_view(nm) == "Headshot")           return "HS";
                            if (std::string_view(nm) == "DecisionMaking")     return "Decision";
                            if (std::string_view(nm) == "Aggressiveness")     return "Aggro";
                            if (std::string_view(nm) == "Communication")      return "Comm";
                            if (std::string_view(nm) == "Adaptability")       return "Adapt";
                            if (std::string_view(nm) == "Positioning")        return "Pos";
                            if (std::string_view(nm) == "GameSense")          return "GS";
                            if (std::string_view(nm) == "Intelligence")       return "Intel";
                            if (std::string_view(nm) == "MidRoundCalling")    return "MidCall";
                            if (std::string_view(nm) == "CrosshairPlacement") return "Cross";
                            if (std::string_view(nm) == "EconomyMgmt")        return "Econ";
                            if (std::string_view(nm) == "AntiStrat")          return "AntiS";
                            if (std::string_view(nm) == "SpikeHandle")        return "Spike";
                            if (std::string_view(nm) == "Reaction")           return "React";
                            if (std::string_view(nm) == "Movement")           return "Move";
                            if (std::string_view(nm) == "Leadership")         return "Lead";
                            return nm;
                        };
                        char attrs_buf[128];
                        std::snprintf(attrs_buf, sizeof(attrs_buf),
                            "%s:%d  %s:%d  %s:%d",
                            abbrev_attr(r.agent->a1), vlr::at(p->attributes, r.agent->a1),
                            abbrev_attr(r.agent->a2), vlr::at(p->attributes, r.agent->a2),
                            abbrev_attr(r.agent->a3), vlr::at(p->attributes, r.agent->a3));
                        ImGui::TextUnformatted(attrs_buf);

                        ImGui::TableNextColumn();
                        ImGui::Text("%.0f", r.fit);

                        ImGui::TableNextColumn();
                        if (r.matches > 0) ImGui::Text("%d", r.matches);
                        else               ImGui::TextDisabled("—");

                        ImGui::TableNextColumn();
                        if (r.matches > 0) ImGui::Text("%.2f", r.avg_rating);
                        else               ImGui::TextDisabled("—");

                        ImGui::TableNextColumn();
                        if (r.last_year > 0) ImGui::Text("%d", r.last_year);
                        else                 ImGui::TextDisabled("—");

                        ImGui::TableNextColumn();
                        ImGui::Text("%.1f", r.score);

                        if (is_sig) ImGui::PopStyleColor();
                    }
                    ImGui::EndTable();
                }

                Sub("[N] in the # column = currently in agent pool. Mastery "
                    "decays 15%%/yr on disuse and prunes after 4 unused years.");

                // === God-mode mastery editor + pool refresh ===
                if (s.god_mode) {
                    ImGui::Spacing();
                    H2("EDIT MASTERY (God Mode)");
                    Sub("Backfill agent mastery directly. Matches > 0 marks the "
                        "player as having played that agent; avg_rating and "
                        "peak_rating shape the mastery bonus contribution.");

                    if (ImGui::BeginTable("##gm_mastery", 5,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                            ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, 320))) {
                        ImGui::TableSetupColumn("AGENT", ImGuiTableColumnFlags_WidthFixed, 110);
                        ImGui::TableSetupColumn("ROLE",  ImGuiTableColumnFlags_WidthFixed, 80);
                        ImGui::TableSetupColumn("MATCHES", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("AVG RATING", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("PEAK", ImGuiTableColumnFlags_WidthFixed, 90);
                        ImGui::TableHeadersRow();
                        int gi = 0;
                        for (auto& a : vlr::agents()) {
                            ImGui::PushID(gi++);
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::PushStyleColor(ImGuiCol_Text, toV4(role_color(a.role)));
                            ImGui::Text("%s", a.name.c_str());
                            ImGui::PopStyleColor();

                            ImGui::TableNextColumn();
                            ImGui::Text("%s", vlr::role_name(a.role));

                            auto& m = p->agent_mastery[a.name];
                            int matches = m.matches;
                            float avg = static_cast<float>(m.avg_rating);
                            float peak = static_cast<float>(m.peak_rating);

                            ImGui::TableNextColumn();
                            ImGui::SetNextItemWidth(180);
                            if (ImGui::SliderInt("##matches", &matches, 0, 300)) {
                                m.matches = matches;
                                if (matches == 0 && m.avg_rating == 0.0 && m.peak_rating == 0.0) {
                                    p->agent_mastery.erase(a.name);
                                } else if (m.last_played_year == 0) {
                                    m.last_played_year = vlr::current_world_year();
                                }
                            }

                            ImGui::TableNextColumn();
                            ImGui::SetNextItemWidth(180);
                            if (ImGui::SliderFloat("##avg", &avg, 0.0f, 2.0f, "%.2f")) {
                                m.avg_rating = avg;
                                if (m.peak_rating < m.avg_rating) m.peak_rating = m.avg_rating;
                            }

                            ImGui::TableNextColumn();
                            ImGui::SetNextItemWidth(80);
                            if (ImGui::SliderFloat("##peak", &peak, 0.0f, 2.5f, "%.2f")) {
                                m.peak_rating = peak;
                            }

                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                    if (ImGui::Button("Refresh agent pool from attrs", ImVec2(260, 28))) {
                        p->update_agent_pool();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Clear all mastery", ImVec2(180, 28))) {
                        p->agent_mastery.clear();
                        p->update_agent_pool();
                    }
                }
                ImGui::EndTabItem();
            }

            // === IGL tab (only meaningful for IGL-flagged players) ===
            if (p->is_igl && ImGui::BeginTabItem("IGL Profile")) {
                ImGui::PushFont(g_font_h2);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                ImGui::TextUnformatted("[I]  In-Game Leader");
                ImGui::PopStyleColor();
                ImGui::PopFont();
                Sub("IGLs trade fragging power for mental ceiling. They call "
                    "the shots mid-round, manage the team economy, and read "
                    "opponent tendencies. The hidden tendencies below shape "
                    "how this IGL likes to run a system.");
                ImGui::Spacing();

                bool god = s.god_mode;

                ImGui::Columns(2, nullptr, false);
                H2("LEADERSHIP STATS");
                auto stat_bar = [&](const char* lbl, vlr::Attr a) {
                    if (god) {
                        int& vr = vlr::at(p->attributes, a);
                        ImGui::PushID(lbl);
                        ImGui::AlignTextToFramePadding();
                        ImGui::Text("%-20s", lbl);
                        ImGui::SameLine(220);
                        ImGui::SetNextItemWidth(240);
                        ImGui::SliderInt("##gs", &vr, 1, 99);
                        ImGui::PopID();
                    } else {
                        int v = vlr::at(p->attributes, a);
                        ImGui::Text("%-20s", lbl);
                        ImGui::SameLine(220);
                        ImGui::ProgressBar(v / 99.0f, ImVec2(220, 0), "");
                        ImGui::SameLine();
                        ImGui::Text("%d", v);
                    }
                };
                stat_bar("Mid-round Calling",  vlr::Attr::MidRoundCalling);
                stat_bar("Economy Management", vlr::Attr::EconomyMgmt);
                stat_bar("Anti-Strat",         vlr::Attr::AntiStrat);
                stat_bar("Leadership",         vlr::Attr::Leadership);
                stat_bar("Communication",      vlr::Attr::Communication);
                stat_bar("Game Sense",         vlr::Attr::GameSense);
                stat_bar("Decision Making",    vlr::Attr::DecisionMaking);
                stat_bar("Intelligence",       vlr::Attr::Intelligence);
                if (god) {
                    stat_bar("Adaptability",   vlr::Attr::Adaptability);
                }

                ImGui::NextColumn();
                H2("TENDENCIES");
                Sub("Sliders are bidirectional — show preferred style.");
                ImGui::Spacing();

                auto tendency = [&](const char* low, const char* high, int& vr) {
                    ImGui::PushFont(g_font_small);
                    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
                    ImGui::Text("%-15s", low);
                    ImGui::PopStyleColor();
                    ImGui::SameLine(160);
                    ImGui::Text("%s", high);
                    ImGui::PopFont();
                    if (god) {
                        ImGui::PushID(low);
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        ImGui::SliderInt("##gt", &vr, 0, 100);
                        ImGui::PopID();
                    } else {
                        ImGui::ProgressBar(vr / 100.0f, ImVec2(-FLT_MIN, 0), "");
                    }
                };
                tendency("Passive",       "Aggressive system",   p->tend_play_aggressive);
                tendency("Lurk-heavy",    "Default-execute",     p->tend_lurk_vs_execute);
                tendency("Quiet leader",  "Vocal leader",        p->tend_vocal);
                tendency("Structured",    "Adaptive",            p->tend_adaptive);

                ImGui::Columns(1);

                // === IGL IMPACT — strategic contribution counters =========
                // These are populated by GameManager's MVP/IGL-impact overhaul
                // (parallel agent B). Wrapped in PushID so the section's own
                // child widgets never collide with surrounding tab IDs.
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::PushID("igl_impact");
                H2("IGL IMPACT");
                if (p->igl_match_count == 0) {
                    ImGui::TextDisabled("No pro matches played yet.");
                } else {
                    ImGui::Text("SEASON IMPACT: %.2f  (peak %.2f)",
                                p->igl_impact_season, p->igl_impact_season_peak);
                    ImGui::Text("CAREER IMPACT: %.2f across %d matches",
                                p->igl_impact_total, p->igl_match_count);
                    double avg_per_match = p->igl_impact_total
                        / static_cast<double>(p->igl_match_count > 0
                                              ? p->igl_match_count : 1);
                    ImGui::Text("Average impact per match: %.3f", avg_per_match);
                    ImGui::Text("PRESSURE MATCHES THIS SEASON: %d",
                                p->season_pressure_matches);
                }
                Sub("IGL Impact tracks strategic contribution (mid-round calls, "
                    "anti-strat, adaptation, comeback creation) independent of "
                    "frag stats. Drives IGL of the Year, MVP race, and contract "
                    "value.");

                if (god) {
                    ImGui::Spacing();
                    SectionHeader("DEV TOOLS \xE2\x80\x94 IGL", nullptr, kVlrGold);
                    ImGui::Spacing();
                    {
                        float impt = static_cast<float>(p->igl_impact_total);
                        ImGui::SetNextItemWidth(220);
                        if (ImGui::DragFloat("Career Impact (total)", &impt,
                                             0.05f, 0.0f, 0.0f, "%.2f")) {
                            p->igl_impact_total = impt;
                        }
                        float imps = static_cast<float>(p->igl_impact_season);
                        ImGui::SetNextItemWidth(220);
                        if (ImGui::DragFloat("Season Impact", &imps,
                                             0.05f, 0.0f, 0.0f, "%.2f")) {
                            p->igl_impact_season = imps;
                        }
                        float impp = static_cast<float>(p->igl_impact_season_peak);
                        ImGui::SetNextItemWidth(220);
                        if (ImGui::DragFloat("Season Peak Impact", &impp,
                                             0.05f, 0.0f, 0.0f, "%.2f")) {
                            p->igl_impact_season_peak = impp;
                        }
                        ImGui::SetNextItemWidth(220);
                        ImGui::InputInt("IGL Match Count", &p->igl_match_count);
                        if (p->igl_match_count < 0) p->igl_match_count = 0;
                        ImGui::SetNextItemWidth(220);
                        ImGui::InputInt("Pressure Matches (season)",
                                        &p->season_pressure_matches);
                        if (p->season_pressure_matches < 0)
                            p->season_pressure_matches = 0;
                        // NOTE: Is-IGL toggle intentionally LIVES on the
                        // Overview tab (god-mode block). Putting it here
                        // produced a catch-22 — IGL Profile only opens when
                        // is_igl is already true, so promoting a non-IGL
                        // required first being an IGL. Toggle from Overview
                        // instead; that path runs the proper attribute
                        // shift (pitfall 4), this raw assignment did not.
                        MutedText("God Mode: raw edits, no clamping beyond valid "
                                  "ranges. Attribute & tendency sliders above are "
                                  "live-editable. (Promote/demote IGL on the "
                                  "Overview tab — it applies the correct "
                                  "attribute shift.)");
                    }
                }
                ImGui::PopID();

                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::Separator();
        if (ImGui::Button("Close", ImVec2(120, 32))) {
            s.show_player_modal = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

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

// Spin up a fresh simulated match from the Watch screen / brackets and
// hand it to the Live Match viewer.
// Run a full BO3/BO5 series (friendly — no career pollution) and open the
// live viewer with every map navigable. Used for "Watch" buttons on the
// tournament bracket so users actually see the series, not just one map.
void OpenLiveMatchSeries(AppState& s, vlr::TeamPtr a, vlr::TeamPtr b,
                          const std::string& event, int best_of) {
    if (!a || !b) return;
    auto fa1 = s.gm.solo_qs[a->region]->global_ladder();
    auto fa2 = s.gm.solo_qs[b->region]->global_ladder();
    a->auto_fill_roster(fa1);
    b->auto_fill_roster(fa2);
    if (a->roster.size() < 5 || b->roster.size() < 5) return;

    auto series = std::make_shared<vlr::Series>(a, b, best_of, event);
    std::vector<vlr::RecordedMatchPtr> recordings;
    while (!series->is_over()) {
        const auto& M = vlr::maps()[static_cast<std::size_t>(
            vlr::rng().irange(0, static_cast<int>(vlr::maps().size()) - 1))];
        vlr::Match m(a, b, M, /*is_solo_q=*/false, event, /*friendly=*/true);
        m.play();
        series->add_match_data(m);
        if (auto rec = series->last_recording()) recordings.push_back(rec);
    }
    if (recordings.empty()) return;
    s.live.match.reset();
    s.live.series = recordings;
    s.live.current_map_idx = 0;
    s.live.recording = recordings.front();
    s.live.round_cursor = 0;
    s.live.event_cursor = 0;
    s.live.auto_play = false;
    s.live.auto_timer = 0.0f;
    s.live.open = true;
}

void OpenLiveMatch(AppState& s, vlr::TeamPtr a, vlr::TeamPtr b, const std::string& event) {
    if (!a || !b) return;
    auto fa1 = s.gm.solo_qs[a->region]->global_ladder();
    auto fa2 = s.gm.solo_qs[b->region]->global_ladder();
    a->auto_fill_roster(fa1);
    b->auto_fill_roster(fa2);
    if (a->roster.size() < 5 || b->roster.size() < 5) return;

    const auto& M = vlr::maps()[static_cast<std::size_t>(
        vlr::rng().irange(0, static_cast<int>(vlr::maps().size()) - 1))];
    // friendly=true: this is a demo/Watch match; stats won't pollute careers.
    auto m = std::make_shared<vlr::Match>(a, b, M, false, event, /*friendly=*/true);
    m->play();
    s.live.match     = m;
    s.live.recording = vlr::make_recorded_match(*m);
    // CRITICAL: clear any leftover series state from a previous viewer
    // open. Without this, OpenLiveMatch (single map) inherits the series
    // vector + current_map_idx from a prior BO3, which causes "Map 2/2"
    // to display and crashes when the user switches maps to a stale ptr.
    s.live.series.clear();
    s.live.current_map_idx = 0;
    // Start at round 0 — viewers want to watch the match unfold from the
    // beginning. They can hit Auto Play or Skip to End if they prefer the
    // result first.
    s.live.round_cursor = 0;
    s.live.event_cursor = 0;
    s.live.auto_play = false;
    s.live.auto_timer = 0.0f;
    s.live.open = true;
}

// Replay an already-recorded match (e.g. from match history). No new
// simulation; we walk the saved round_history.
void OpenReplay(AppState& s, vlr::RecordedMatchPtr rec) {
    if (!rec) return;
    s.live.match.reset();
    s.live.recording = rec;
    s.live.series.clear();
    s.live.current_map_idx = 0;
    // Start at round 0 so the user can watch the whole match play out.
    s.live.round_cursor = 0;
    s.live.event_cursor = 0;
    s.live.auto_play = false;
    s.live.auto_timer = 0.0f;
    s.live.open = true;
}

// Open a multi-map series (BO3/BO5). Starts on Map 1. The Live Match
// modal exposes Next/Prev Map buttons to navigate.
void OpenReplaySeries(AppState& s, const std::vector<vlr::RecordedMatchPtr>& maps) {
    if (maps.empty()) return;
    s.live.match.reset();
    s.live.series = maps;
    s.live.current_map_idx = 0;
    s.live.recording = maps.front();
    s.live.round_cursor = 0;
    s.live.event_cursor = 0;
    s.live.auto_play = false;
    s.live.auto_timer = 0.0f;
    s.live.open = true;
}

// Score banner: a clean pro-broadcast header. Big team tags with team-color
// accent strips, a large centered scoreline, the current map + map-in-series
// (NEVER the series total), and a LIVE/FINAL status pill. Leading team is
// emphasised, trailing dimmed. Every element is clamped to the modal width
// so nothing can bleed off-screen.
void DrawScoreBanner(const vlr::RecordedMatch& rec, int t1_score_now, int t2_score_now,
                     int round_idx, std::size_t total_rounds) {
    ImGui::BeginChild("##banner", ImVec2(0, 122), ImGuiChildFlags_Border,
                      ImGuiWindowFlags_NoScrollbar |
                      ImGuiWindowFlags_NoScrollWithMouse);
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 sz = ImGui::GetContentRegionAvail();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Use team-specific colors when available; fall back to blue/red.
    auto team_col = [](const vlr::TeamPtr& t, ImU32 fallback) {
        if (!t) return fallback;
        return IM_COL32(t->color_primary_r, t->color_primary_g, t->color_primary_b, 0xFF);
    };
    vlr::TeamPtr t1 = rec.team1.lock();
    vlr::TeamPtr t2 = rec.team2.lock();
    ImU32 c1 = team_col(t1, kVlrBlue);
    ImU32 c2 = team_col(t2, kVlrRed);
    bool t1_leading = (t1_score_now > t2_score_now);
    bool t2_leading = (t2_score_now > t1_score_now);

    auto dim = [](ImU32 c, float a) {
        ImColor col(c);
        col.Value.x *= a; col.Value.y *= a; col.Value.z *= a;
        return (ImU32)col;
    };

    float x0 = origin.x, x1 = origin.x + sz.x;
    float y0 = origin.y, y1 = origin.y + sz.y;
    float half = sz.x * 0.5f;
    float center_w = std::min(150.0f, sz.x * 0.22f);
    float left_end  = origin.x + half - center_w * 0.5f;
    float right_beg = origin.x + half + center_w * 0.5f;

    // Deep base + side washes. Leading side gets a stronger team tint.
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), kBgDeep, 8.0f);
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(left_end, y1),
                      dim(c1, t1_leading ? 0.34f : 0.14f), 8.0f);
    dl->AddRectFilled(ImVec2(right_beg, y0), ImVec2(x1, y1),
                      dim(c2, t2_leading ? 0.34f : 0.14f), 8.0f);
    // Team-color accent strips along the outer top+bottom edges.
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(left_end, y0 + 4), c1);
    dl->AddRectFilled(ImVec2(x0, y1 - 4), ImVec2(left_end, y1), c1);
    dl->AddRectFilled(ImVec2(right_beg, y0), ImVec2(x1, y0 + 4), c2);
    dl->AddRectFilled(ImVec2(right_beg, y1 - 4), ImVec2(x1, y1), c2);

    // Center scoreline panel.
    dl->AddRectFilled(ImVec2(left_end, y0), ImVec2(right_beg, y1),
                      IM_COL32(0x06, 0x09, 0x10, 0xFF), 6.0f);
    dl->AddRect(ImVec2(left_end, y0), ImVec2(right_beg, y1),
                kBorderStrong, 6.0f, 0, 1.0f);

    // Big centered score.
    char sb[16];
    ImGui::PushFont(g_font_kpi);
    std::snprintf(sb, sizeof(sb), "%d", t1_score_now);
    ImVec2 ssz1 = ImGui::CalcTextSize(sb);
    dl->AddText(ImVec2(origin.x + half - 22.0f - ssz1.x, y0 + 34),
                t1_leading ? c1 : kVlrText, sb);
    std::snprintf(sb, sizeof(sb), "%d", t2_score_now);
    dl->AddText(ImVec2(origin.x + half + 22.0f, y0 + 34),
                t2_leading ? c2 : kVlrText, sb);
    ImGui::PopFont();
    ImGui::PushFont(g_font_h2);
    ImVec2 dsz = ImGui::CalcTextSize("-");
    dl->AddText(ImVec2(origin.x + half - dsz.x * 0.5f, y0 + 48), kVlrSub, "-");
    ImGui::PopFont();

    // Helper: draw a string clamped to a max width with ellipsis.
    auto clamped = [&](ImFont* f, float fs, ImVec2 at, ImU32 col,
                       const std::string& txt, float max_w, bool right_align) {
        ImGui::PushFont(f);
        std::string t = txt;
        ImVec2 m = ImGui::CalcTextSize(t.c_str());
        if (m.x > max_w && t.size() > 1) {
            while (t.size() > 1 &&
                   ImGui::CalcTextSize((t + "\xE2\x80\xA6").c_str()).x > max_w)
                t.pop_back();
            t += "\xE2\x80\xA6";
            m = ImGui::CalcTextSize(t.c_str());
        }
        float px = right_align ? (at.x - m.x) : at.x;
        dl->AddText(f, fs, ImVec2(px, at.y), col, t.c_str());
        ImGui::PopFont();
        return m.x;
    };
    float h1fs = g_font_h1 ? g_font_h1->FontSize : 30.0f;
    float h2fs = g_font_h2 ? g_font_h2->FontSize : 22.0f;
    float smfs = g_font_small ? g_font_small->FontSize : 13.0f;
    float side_max = (left_end - x0) - 36.0f;
    if (side_max < 60.0f) side_max = 60.0f;

    // === Team 1 (left) ===
    {
        std::string tag = (t1 && !t1->tag.empty()) ? t1->tag : rec.blue_name;
        clamped(g_font_h1, h1fs, ImVec2(x0 + 18, y0 + 16),
                t1_leading ? c1 : kVlrText, tag, side_max, false);
        clamped(g_font_h2, h2fs, ImVec2(x0 + 18, y0 + 16 + h1fs + 2),
                t1_leading ? kVlrText : kVlrSub, rec.blue_name, side_max, false);
        std::string sub = (t1 ? t1->region : std::string()) + "  \xE2\x80\xA2  "
                          + rec.map_name;
        clamped(g_font_small, smfs, ImVec2(x0 + 18, y1 - smfs - 10),
                kVlrSub, sub, side_max, false);
    }

    // === Team 2 (right, right-aligned) ===
    {
        std::string tag = (t2 && !t2->tag.empty()) ? t2->tag : rec.red_name;
        clamped(g_font_h1, h1fs, ImVec2(x1 - 18, y0 + 16),
                t2_leading ? c2 : kVlrText, tag, side_max, true);
        clamped(g_font_h2, h2fs, ImVec2(x1 - 18, y0 + 16 + h1fs + 2),
                t2_leading ? kVlrText : kVlrSub, rec.red_name, side_max, true);
        // Spoiler-free: show "Round N" only, never the total.
        char rb[48];
        std::snprintf(rb, sizeof(rb), "%s  \xE2\x80\xA2  Round %d",
                      t2 ? t2->region.c_str() : "", round_idx);
        clamped(g_font_small, smfs, ImVec2(x1 - 18, y1 - smfs - 10),
                kVlrSub, rb, side_max, true);
    }

    // Event name centered under the scoreline (clamped to the center panel).
    clamped(g_font_small, smfs,
            ImVec2(origin.x + half, y1 - smfs - 10), kTextFaint,
            rec.event, sz.x - side_max * 2.0f - 24.0f, false);

    // LIVE / FINAL status pill in the center, just above the scoreline base.
    bool is_final = (round_idx >= (int)total_rounds);
    const char* status = is_final ? "FINAL" : "LIVE";
    ImU32 status_col = is_final ? kVlrSub : kVlrGreen;
    ImGui::PushFont(g_font_small);
    ImVec2 stsz = ImGui::CalcTextSize(status);
    float pw = stsz.x + 18.0f;
    ImVec2 ptl(origin.x + half - pw * 0.5f, y1 - 24);
    ImVec2 pbr(origin.x + half + pw * 0.5f, y1 - 6);
    dl->AddRectFilled(ptl, pbr,
                      is_final ? kSurface : IM_COL32(0x12, 0x2A, 0x18, 0xFF), 9.0f);
    dl->AddRect(ptl, pbr, status_col, 9.0f, 0, 1.0f);
    dl->AddText(ImVec2(origin.x + half - stsz.x * 0.5f, y1 - 22),
                status_col, status);
    ImGui::PopFont();

    ImGui::EndChild();
}

// Round timeline strip: split-half presentation with momentum streaks,
// half-divider, OT shading, and click-to-jump. Each round square is colored
// by the team that won; consecutive wins for the same side show a top
// stripe of growing intensity to indicate momentum.
void DrawRoundTimeline(LiveMatchState& live,
                       const std::vector<vlr::RoundLog>& rounds,
                       const vlr::RecordedMatch& rec) {
    if (rounds.empty()) {
        ImGui::Dummy(ImVec2(0, 30));
        return;
    }
    // Outer frame is FIXED to the modal width; the round strip itself gets
    // its own narrow horizontally-scrolling child so a long match (OT) can
    // never push the rest of the modal off-screen — only this strip scrolls.
    ImGui::BeginChild("##timeline", ImVec2(0, 60), ImGuiChildFlags_Border,
                      ImGuiWindowFlags_HorizontalScrollbar |
                      ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Use team colors for visual identity rather than hard-coded blue/red.
    vlr::TeamPtr t1 = rec.team1.lock();
    vlr::TeamPtr t2 = rec.team2.lock();
    ImU32 c1 = t1
        ? IM_COL32(t1->color_primary_r, t1->color_primary_g, t1->color_primary_b, 0xFF)
        : kVlrBlue;
    ImU32 c2 = t2
        ? IM_COL32(t2->color_primary_r, t2->color_primary_g, t2->color_primary_b, 0xFF)
        : kVlrRed;

    int n = (int)rounds.size();
    // SPOILER GUARD: render only rounds that have been "played" up to
    // the cursor. Pre-rendering all `n` slots (even dimmed) leaks the
    // total round count — the user can count squares to figure out
    // whether the match went to OT before they've watched it. Keep
    // slot width FIXED so adding squares can't be inferred from layout
    // shifts either.
    int n_visible = std::clamp(live.round_cursor, 0, n);
    // Allow a single un-revealed slot for "current round" while in the
    // middle of play-by-play (so the gold ring has somewhere to land).
    // This is the same round_cursor that already drives the score banner,
    // so we're not leaking new info.
    if (n_visible < n) ++n_visible;
    if (n_visible == 0) {
        // No rounds played yet — just leave an empty strip.
        ImGui::EndChild();
        return;
    }
    float pad = 8.0f;
    float gap = 4.0f;
    // Fixed slot width — independent of total round count. Keep it FIXED;
    // scaling by (avail / n) would re-leak total round count via square
    // geometry. When the strip is wider than the visible region the child's
    // own horizontal scrollbar handles overflow (the rest of the modal is
    // unaffected — only this narrow strip scrolls).
    float w = 24.0f;
    float h = 30.0f;
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float strip_w = pad * 2.0f + n_visible * (w + gap);
    // Reserve the strip's full content width so the child can scroll to it.
    ImGui::Dummy(ImVec2(strip_w, 44.0f));
    float y = origin.y + (44.0f - h) * 0.5f;

    auto winner_is_t1 = [&](int i) -> bool {
        if (i == 0) return rounds[i].t1_score > 0;
        return rounds[i].t1_score > rounds[i - 1].t1_score;
    };

    // Half labels at the top — only show DEF / OT once we've reached
    // those rounds. Showing them ahead of time spoils that the match has
    // 12+ / 24+ rounds.
    ImGui::PushFont(g_font_small);
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
    dl->AddText(ImVec2(origin.x + pad, origin.y + 3), kTextFaint, "ATK");
    if (n_visible > 12) {
        float x12 = origin.x + pad + 12 * (w + gap);
        dl->AddText(ImVec2(x12 + 4, origin.y + 3), kTextFaint, "DEF");
    }
    if (n_visible > 24) {
        float x24 = origin.x + pad + 24 * (w + gap);
        dl->AddText(ImVec2(x24 + 4, origin.y + 3), kTextFaint, "OT");
    }
    ImGui::PopStyleColor();
    ImGui::PopFont();

    int streak = 0;
    bool last_was_t1 = false;
    for (int i = 0; i < n_visible; ++i) {
        float x = origin.x + pad + i * (w + gap);
        bool t1_wins = winner_is_t1(i);
        ImU32 col = t1_wins ? c1 : c2;
        bool revealed = (i < live.round_cursor);

        // Track momentum streak (only across revealed rounds)
        if (revealed) {
            if (i == 0) { streak = 1; last_was_t1 = t1_wins; }
            else if (t1_wins == last_was_t1) ++streak;
            else { streak = 1; last_was_t1 = t1_wins; }
        }

        if (!revealed) {
            // Empty slot — hairline-framed placeholder, no color leak.
            dl->AddRectFilled(ImVec2(x, y + 6), ImVec2(x + w, y + h),
                              kBgDeep, 4.0f);
            dl->AddRect(ImVec2(x, y + 6), ImVec2(x + w, y + h),
                        kBorder, 4.0f, 0, 1.0f);
        } else {
            // Won round: deep base then the winning side's team color.
            dl->AddRectFilled(ImVec2(x, y + 6), ImVec2(x + w, y + h),
                              kBgDeep, 4.0f);
            dl->AddRectFilled(ImVec2(x, y + 6), ImVec2(x + w, y + h),
                              col, 4.0f);
            // Momentum streak indicator: brighter top stripe with streak.
            if (streak >= 2) {
                float intensity = std::min(1.0f, 0.45f + 0.16f * streak);
                ImColor streak_col = ImColor(col);
                streak_col.Value.x = std::min(1.0f, streak_col.Value.x * 1.35f);
                streak_col.Value.y = std::min(1.0f, streak_col.Value.y * 1.35f);
                streak_col.Value.z = std::min(1.0f, streak_col.Value.z * 1.35f);
                streak_col.Value.w = intensity;
                dl->AddRectFilled(ImVec2(x, y + 6), ImVec2(x + w, y + 10),
                                  (ImU32)streak_col, 2.0f);
            }
        }

        // Half / OT divider lines — only draw the divider when the
        // boundary round has been reached (otherwise we'd telegraph
        // "OT is coming" before round 24 is even played).
        if ((i == 12 || i == 24) && revealed) {
            dl->AddLine(ImVec2(x - gap * 0.5f, y - 2),
                        ImVec2(x - gap * 0.5f, y + h + 4),
                        kVlrGold, 1.5f);
        }

        // Pistol round indicator (rounds 0 and 12) — gold hairline frame.
        if (revealed && (i == 0 || i == 12)) {
            dl->AddRect(ImVec2(x, y + 6), ImVec2(x + w, y + h),
                        kVlrGold, 4.0f, 0, 1.4f);
        }

        // Current round highlight — electric accent ring.
        if (i == live.round_cursor - 1) {
            dl->AddRect(ImVec2(x - 2, y + 4), ImVec2(x + w + 2, y + h + 2),
                        kAccent, 5.0f, 0, 2.5f);
        }

        // Click hit-area to jump
        ImGui::SetCursorScreenPos(ImVec2(x, y + 6));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##rj", ImVec2(w, h - 6))) {
            live.round_cursor = i + 1;
            live.auto_play = false;
        }
        // Tooltip only on REVEALED slots — never spoil future round
        // outcomes by hovering an un-played slot. (We still let users
        // click the un-played slot to skip ahead, matching the existing
        // Skip-to-End / per-round-step semantics.)
        if (revealed && ImGui::IsItemHovered()) {
            vlr::TeamPtr tt1 = rec.team1.lock();
            vlr::TeamPtr tt2 = rec.team2.lock();
            const char* w1 = tt1 ? tt1->tag.c_str() : "T1";
            const char* w2 = tt2 ? tt2->tag.c_str() : "T2";
            ImGui::SetTooltip("Round %d  |  %s %d - %d %s\nWinner: %s",
                              rounds[i].round,
                              w1, rounds[i].t1_score, rounds[i].t2_score, w2,
                              t1_wins ? (tt1 ? tt1->name.c_str() : "T1")
                                      : (tt2 ? tt2->name.c_str() : "T2"));
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

// Compute a partial scoreboard from the round-by-round event log up to the
// given cursor (exclusive). Used by the Live Match viewer so the on-screen
// scoreboard never reveals stats from rounds that haven't been "played" yet.
//
// Accurate fields (derived directly from the event log):
//     k, d, fb, fd, survivals, rounds_with_kast
// Approximated (event log doesn't store per-event values):
//     a, damage, hs_hits         — scaled as final * (cursor / total_rounds)
//     rating                      — rough K/D-based live estimate while
//                                   running, true VLR rating once complete
std::unordered_map<vlr::Player*, vlr::PlayerMatchStats>
compute_partial_stats(const vlr::RecordedMatch& rec, int cursor) {
    std::unordered_map<vlr::Player*, vlr::PlayerMatchStats> out;
    if (!rec.round_history) return out;

    auto init_from_team = [&](const vlr::TeamPtr& t) {
        if (!t) return;
        std::size_t n = std::min<std::size_t>(5, t->roster.size());
        for (std::size_t i = 0; i < n; ++i) out[t->roster[i].get()] = vlr::PlayerMatchStats{};
    };
    auto init_from_history = [&](const std::vector<vlr::Player*>& roster) {
        std::size_t n = std::min<std::size_t>(5, roster.size());
        for (std::size_t i = 0; i < n; ++i) {
            if (roster[i]) out[roster[i]] = vlr::PlayerMatchStats{};
        }
    };
    // Try the live TeamPtr first (pro matches); fall back to the
    // recording's history_record team rosters when the TeamPtr has died
    // (solo Q matches — synthetic Blue/Red Teams don't outlive
    // force_solo_match). Without the fallback the partial-stats map is
    // empty for solo Q replays and the live scoreboard reads zeros even
    // when stats exist in match_stats.
    if (auto t1 = rec.team1.lock()) init_from_team(t1);
    else if (rec.history_record) init_from_history(rec.history_record->blue_team);
    if (auto t2 = rec.team2.lock()) init_from_team(t2);
    else if (rec.history_record) init_from_history(rec.history_record->red_team);

    int total = static_cast<int>(rec.round_history->size());
    int n     = std::min(cursor, total);

    for (int r = 0; r < n; ++r) {
        const auto& round = (*rec.round_history)[r];
        std::set<vlr::Player*> died_this_round;
        std::set<vlr::Player*> kast_this_round;
        for (std::size_t j = 0; j < round.events.size(); ++j) {
            const auto& ev = round.events[j];
            if (!ev.killer || !ev.victim) continue;
            out[ev.killer].k++;
            out[ev.victim].d++;
            if (j == 0) {
                out[ev.killer].fb++;
                out[ev.victim].fd++;
            }
            died_this_round.insert(ev.victim);
            kast_this_round.insert(ev.killer);  // got a kill -> KAST
            // Trade: previous kill went the other way + trader gets KAST,
            // and the now-traded victim also keeps their KAST credit.
            if (j > 0) {
                const auto& prev = round.events[j - 1];
                if (prev.team_won != ev.team_won) {
                    kast_this_round.insert(prev.victim);  // traded -> KAST
                }
            }
        }
        // Survivors: any participant who didn't die this round.
        for (auto& kv : out) {
            if (!died_this_round.count(kv.first)) {
                kast_this_round.insert(kv.first);
                kv.second.survivals++;
            }
        }
        for (auto* p : kast_this_round) out[p].rounds_with_kast++;
    }

    // Damage / assists / HS aren't per-event in the log — scale the final
    // values by the fraction of rounds revealed. When the cursor reaches
    // the end of the match we expose the actual final figures.
    if (rec.match_stats && total > 0) {
        double frac = (n <= 0) ? 0.0 : static_cast<double>(n) / total;
        bool live = (n < total);
        for (auto& kv : out) {
            auto fit = rec.match_stats->find(kv.first);
            if (fit == rec.match_stats->end()) continue;
            kv.second.damage  = static_cast<int>(fit->second.damage  * frac);
            kv.second.a       = static_cast<int>(fit->second.a       * frac);
            kv.second.hs_hits = static_cast<int>(fit->second.hs_hits * frac);
            if (!live) {
                // Match's actually finished — expose the true VLR rating.
                kv.second.rating = fit->second.rating;
            } else if (n > 0) {
                // Live mode estimate: simple K/D-driven rating that updates
                // every round without spoiling the final figure.
                double kpr  = kv.second.k / static_cast<double>(n);
                double dpr  = kv.second.d / static_cast<double>(n);
                double rough = 0.70 + (kpr - dpr) * 0.40 + (kv.second.fb * 0.05);
                kv.second.rating = vlr::clamp_v(rough, 0.10, 2.50);
            } else {
                kv.second.rating = 1.00;
            }
        }
    }
    return out;
}

// One side of the scoreboard. Header strip uses the team's primary color
// for instant visual identification, then a clean stats table with
// right-aligned numeric columns and team-color-coded rating cells.
//
// Refactored to accept the roster as a raw `vector<Player*>` so solo Q
// matches (whose synthetic Blue/Red TeamPtrs die when force_solo_match
// returns) can still render their scoreboard from the recording's
// history_record. For pro/league matches the caller passes
// team->roster's player pointers + the team's display data; for solo Q
// matches the caller passes history_record.blue_team / red_team + the
// recording's blue_name / red_name as fallbacks.
//
// roster_players: ordered list of starters (any size; iterated as-is).
// display_name / display_tag: header strip text. Tag may be empty.
// brand_col: primary color for the strip + accent (already resolved).
void DrawTeamScoreboard(const std::vector<vlr::Player*>& roster_players,
                        const std::string& display_name,
                        const std::string& display_tag,
                        ImU32 brand,
                        const std::unordered_map<vlr::Player*, vlr::PlayerMatchStats>& stats,
                        const vlr::HistoryRecord& history,
                        const char* table_id) {
    if (roster_players.empty()) {
        // Defensive: empty roster shouldn't happen post-fix, but if it
        // does (a recording with no history_record AND no team1.lock())
        // we want the user to see something rather than a silent gap.
        ImGui::TextDisabled("(no scoreboard data for %s)", display_name.c_str());
        return;
    }

    int total_k = 0, total_fb = 0, total_dmg = 0;
    int total_hs = 0;
    for (auto* p : roster_players) {
        if (!p) continue;
        auto it = stats.find(p);
        if (it == stats.end()) continue;
        total_k  += it->second.k;
        total_fb += it->second.fb;
        total_dmg += it->second.damage;
        total_hs += it->second.hs_hits;
    }
    int hs_pct = total_k > 0 ? (int)(100.0 * total_hs / total_k) : 0;

    // === Team header strip (colored bar with team name + tag + KPIs) ===
    ImVec2 hdr_pos = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float hdr_h = 40.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Deep base + a faint brand wash so the strip reads as a broadcast
    // lower-third rather than a flat block.
    dl->AddRectFilled(hdr_pos, ImVec2(hdr_pos.x + avail.x, hdr_pos.y + hdr_h),
                      kBgDeep, 4.0f);
    ImColor dim((ImU32)brand);
    dim.Value.x *= 0.30f; dim.Value.y *= 0.30f; dim.Value.z *= 0.30f; dim.Value.w = 1.0f;
    dl->AddRectFilled(hdr_pos, ImVec2(hdr_pos.x + avail.x, hdr_pos.y + hdr_h),
                      (ImU32)dim, 4.0f);
    dl->AddRect(hdr_pos, ImVec2(hdr_pos.x + avail.x, hdr_pos.y + hdr_h),
                kBorder, 4.0f, 0, 1.0f);
    // Left brand stripe (5px, full team color).
    dl->AddRectFilled(hdr_pos, ImVec2(hdr_pos.x + 5, hdr_pos.y + hdr_h), brand);
    ImGui::SetCursorScreenPos(ImVec2(hdr_pos.x + 14, hdr_pos.y + 6));
    ImGui::PushFont(g_font_h2);
    if (!display_tag.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(brand));
        ImGui::Text("%s", display_tag.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(8, 0)); ImGui::SameLine();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrText));
    ImGui::TextUnformatted(display_name.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();

    // KPI row right-aligned in the header strip
    char kpi[160];
    int adr = roster_players.empty() ? 0
            : (int)((double)total_dmg / std::max<int>(1, (int)roster_players.size() * 12));
    std::snprintf(kpi, sizeof(kpi), "K %d   FB %d   HS %d%%   ADR %d",
                  total_k, total_fb, hs_pct, adr);
    ImGui::PushFont(g_font_small);
    ImVec2 ksz = ImGui::CalcTextSize(kpi);
    ImGui::SetCursorScreenPos(ImVec2(hdr_pos.x + avail.x - 14 - ksz.x, hdr_pos.y + 12));
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
    ImGui::TextUnformatted(kpi);
    ImGui::PopStyleColor();
    ImGui::PopFont();

    // Reserve the strip's vertical space for the layout
    ImGui::SetCursorScreenPos(ImVec2(hdr_pos.x, hdr_pos.y + hdr_h + 6));

    // Identify the top-rated player so their row gets a faint accent
    // left-edge (the broadcast "team MVP" highlight).
    vlr::Player* top_rated = nullptr;
    {
        double best = -1.0;
        for (auto* p : roster_players) {
            if (!p) continue;
            auto it = stats.find(p);
            if (it == stats.end()) continue;
            if (it->second.rating > best) { best = it->second.rating; top_rated = p; }
        }
    }

    // === Stats table (sortable) ===
    // Columns map to a sort key; clicking a numeric header sorts that side's
    // players by it (DESC default, toggle), name ASC. Role-name tinting,
    // MVP edge and mono numerics preserved. We sort a local copy of the
    // roster pointers so the caller's vector is untouched.
    enum SbCol { SB_FLAG, SB_PLAYER, SB_AGENT, SB_K, SB_D, SB_A,
                 SB_FB, SB_HS, SB_RAT, SB_NCOL };
    if (!ImGui::BeginTable(table_id, SB_NCOL,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
            ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti |
            ImGuiTableFlags_SizingFixedFit, ImVec2(-FLT_MIN, 0))) return;
    ImGui::TableSetupColumn(" ", ImGuiTableColumnFlags_WidthFixed |
                            ImGuiTableColumnFlags_NoSort, 24.0f);
    ImGui::TableSetupColumn("PLAYER", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("AGENT", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("K", ImGuiTableColumnFlags_WidthFixed |
        ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending, 36.0f);
    ImGui::TableSetupColumn("D", ImGuiTableColumnFlags_WidthFixed |
        ImGuiTableColumnFlags_PreferSortDescending, 36.0f);
    ImGui::TableSetupColumn("A", ImGuiTableColumnFlags_WidthFixed |
        ImGuiTableColumnFlags_PreferSortDescending, 36.0f);
    ImGui::TableSetupColumn("FB", ImGuiTableColumnFlags_WidthFixed |
        ImGuiTableColumnFlags_PreferSortDescending, 36.0f);
    ImGui::TableSetupColumn("HS%", ImGuiTableColumnFlags_WidthFixed |
        ImGuiTableColumnFlags_PreferSortDescending, 44.0f);
    ImGui::TableSetupColumn("RAT", ImGuiTableColumnFlags_WidthFixed |
        ImGuiTableColumnFlags_PreferSortDescending, 56.0f);
    ImGui::TableHeadersRow();

    std::vector<vlr::Player*> ordered;
    ordered.reserve(roster_players.size());
    for (auto* p : roster_players) if (p && stats.count(p)) ordered.push_back(p);
    if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs()) {
        if (sp->SpecsCount > 0) {
            auto stat_of = [&](vlr::Player* p) -> const vlr::PlayerMatchStats& {
                return stats.find(p)->second;
            };
            std::stable_sort(ordered.begin(), ordered.end(),
                [&](vlr::Player* a, vlr::Player* b) {
                    for (int si = 0; si < sp->SpecsCount; ++si) {
                        const auto& s2 = sp->Specs[si];
                        bool asc = (s2.SortDirection == ImGuiSortDirection_Ascending);
                        const auto& sa = stat_of(a);
                        const auto& sb = stat_of(b);
                        double va = 0, vb = 0; int cmp = 0;
                        switch (s2.ColumnIndex) {
                        case SB_PLAYER:
                            cmp = player_label(*a).compare(player_label(*b)); break;
                        case SB_AGENT: {
                            auto ia = history.stats.find(a);
                            auto ib = history.stats.find(b);
                            std::string aa = ia != history.stats.end() ? ia->second.agent : "";
                            std::string ab = ib != history.stats.end() ? ib->second.agent : "";
                            cmp = aa.compare(ab); break; }
                        case SB_K:   va = sa.k; vb = sb.k; break;
                        case SB_D:   va = sa.d; vb = sb.d; break;
                        case SB_A:   va = sa.a; vb = sb.a; break;
                        case SB_FB:  va = sa.fb; vb = sb.fb; break;
                        case SB_HS:  va = sa.hs_hits; vb = sb.hs_hits; break;
                        case SB_RAT: va = sa.rating; vb = sb.rating; break;
                        default: break;
                        }
                        if (cmp == 0 && va != vb) cmp = (va < vb) ? -1 : 1;
                        if (cmp != 0) return asc ? (cmp < 0) : (cmp > 0);
                    }
                    return player_label(*a) < player_label(*b);
                });
        }
    }
    for (auto* p : ordered) {
        if (!p) continue;
        auto it = stats.find(p);
        if (it == stats.end()) continue;
        auto& ss = it->second;
        bool is_top = (p == top_rated);
        ImGui::TableNextRow();
        // Faint accent left-edge on the team's top-rated row.
        if (is_top) {
            ImVec2 rmin = ImGui::GetCursorScreenPos();
            float rh = ImGui::GetTextLineHeightWithSpacing() + 4.0f;
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(rmin.x, rmin.y),
                ImVec2(rmin.x + 3.0f, rmin.y + rh), kAccent);
        }
        ImGui::TableNextColumn(); CountryFlag(p->country_iso);
        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text,
                              toV4(is_top ? kVlrText : kVlrSub));
        ImGui::Text("%s", player_label(*p).c_str());
        ImGui::PopStyleColor();
        // Phase 1 (Agent C) — subtle signature agent chip beside the
        // gamertag. Dimmer, smaller font; skipped if empty.
        {
            std::string sig_chip = p->signature_agent();
            if (!sig_chip.empty()) {
                ImGui::SameLine();
                ImGui::PushFont(g_font_small);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
                ImGui::Text("[%s]", sig_chip.c_str());
                ImGui::PopStyleColor();
                ImGui::PopFont();
            }
        }
        ImGui::TableNextColumn();
        auto agentit = history.stats.find(p);
        std::string agname = agentit != history.stats.end() ? agentit->second.agent : "?";
        // Color the agent name by the AGENT's role (the role of the agent
        // they're piloting this map), NOT by the player's primary_role
        // (career default). A Controller main flexed onto Killjoy must
        // render in the Sentinel color, not the Controller color.
        vlr::Role agent_role = p->primary_role;  // safe fallback
        if (const auto* ag = vlr::find_agent_by_name(agname)) {
            agent_role = ag->role;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(role_color(agent_role)));
        ImGui::Text("%s", agname.c_str());
        ImGui::PopStyleColor();
        // Monospaced numeric columns for tight pro-scoreboard alignment.
        ImGui::PushFont(g_font_mono);
        ImGui::TableNextColumn(); ImGui::Text("%d", ss.k);
        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
        ImGui::Text("%d", ss.d);
        ImGui::PopStyleColor();
        ImGui::TableNextColumn(); ImGui::Text("%d", ss.a);
        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(ss.fb > 0 ? kVlrGold : kVlrSub));
        ImGui::Text("%d", ss.fb);
        ImGui::PopStyleColor();
        ImGui::TableNextColumn();
        // Per-match HS% — read from PlayerLine (history_record.stats[p].hs)
        // which is the per-map computed value. Falls back to '—' if the
        // player has no kills yet (HS% over zero kills is undefined).
        if (agentit != history.stats.end() && ss.k > 0) {
            ImGui::Text("%d%%", agentit->second.hs);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kTextFaint));
            ImGui::TextUnformatted("\xE2\x80\x94");
            ImGui::PopStyleColor();
        }
        ImGui::TableNextColumn();
        ImU32 rcol = ss.rating >= 1.10 ? kVlrGreen : (ss.rating >= 0.95 ? kVlrText : kVlrRed);
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(rcol));
        ImGui::Text("%.2f", ss.rating);
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
    ImGui::EndTable();
}

void DrawLiveMatchModal(AppState& s, float dt) {
    if (!s.live.open) return;
    ImGui::OpenPopup("Live Match##modal");
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 c = vp->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    // Large broadcast modal, but clamped to the viewport so it never
    // exceeds the screen. The modal window itself owns the ONE vertical
    // scroll for the whole live view — every inner panel is non-scrolling.
    ImVec2 lm_size(std::min(1180.0f, vp->WorkSize.x - 40.0f),
                   std::min(820.0f,  vp->WorkSize.y - 40.0f));
    ImGui::SetNextWindowSize(lm_size, ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("Live Match##modal", &s.live.open,
                               ImGuiWindowFlags_NoSavedSettings)) {
        if (!s.live.recording || !s.live.recording->round_history) {
            ImGui::TextDisabled("No live match.");
            ImGui::EndPopup();
            return;
        }
        const auto& rec = *s.live.recording;
        const auto& rounds = *rec.round_history;
        const auto& match_stats = *rec.match_stats;
        const auto& history_record = *rec.history_record;

        auto& live = s.live;

        // === Export-to-JSON helpers ===
        // Sanitize a filename component: replace anything outside
        // [A-Za-z0-9_-] with '_', strip leading/trailing whitespace,
        // collapse the result to <= cap chars.
        auto sanitize_filename_part = [](std::string in, std::size_t cap = 40) {
            // Strip leading/trailing whitespace.
            std::size_t l = 0, r = in.size();
            while (l < r && std::isspace((unsigned char)in[l])) ++l;
            while (r > l && std::isspace((unsigned char)in[r - 1])) --r;
            std::string s2 = in.substr(l, r - l);
            for (auto& c : s2) {
                unsigned char uc = (unsigned char)c;
                bool ok = std::isalnum(uc) || c == '_' || c == '-';
                if (!ok) c = '_';
            }
            if (s2.size() > cap) s2.resize(cap);
            if (s2.empty()) s2 = "match";
            return s2;
        };

        // Native Win32 Save File Dialog. Returns "" if user cancels.
        auto prompt_save_path = [](const std::string& default_filename) -> std::string {
            char buf[MAX_PATH];
            std::strncpy(buf, default_filename.c_str(), sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';

            OPENFILENAMEA ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner   = nullptr;
            ofn.lpstrFilter = "JSON files (*.json)\0*.json\0All files (*.*)\0*.*\0";
            ofn.lpstrFile   = buf;
            ofn.nMaxFile    = sizeof(buf);
            ofn.lpstrDefExt = "json";
            ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
            if (GetSaveFileNameA(&ofn)) return std::string(buf);
            return std::string();
        };

        // Build a default filename "{event}_{team1}_vs_{team2}_{year}-{day}.json"
        // from the current series. Capped at ~80 chars total.
        auto build_default_filename = [&]() {
            std::string ev   = sanitize_filename_part(rec.event,      24);
            std::string t1n  = sanitize_filename_part(rec.blue_name,  16);
            std::string t2n  = sanitize_filename_part(rec.red_name,   16);
            char tail[32];
            std::snprintf(tail, sizeof(tail), "_%d-%d.json",
                          s.gm.year, s.gm.day_in_year);
            std::string fn = ev + "_" + t1n + "_vs_" + t2n + tail;
            if (fn.size() > 80) {
                // Trim event part first, keep matchup + tail intact.
                std::size_t excess = fn.size() - 80;
                if (excess < ev.size()) {
                    ev.resize(ev.size() - excess);
                    fn = ev + "_" + t1n + "_vs_" + t2n + tail;
                }
            }
            return fn;
        };

        // Run the export. Pulls live.series, derives event/best_of/year/day,
        // calls export_series_to_json, prompts for save path, writes file,
        // pushes status to s.log.
        auto run_export = [&]() {
            if (live.series.empty()) {
                s.log.emplace_back("Export failed: no series data.");
                return;
            }
            // Derive event_name from the first recording's event field
            // (works for both fresh and replayed series).
            std::string event_name = live.series.front()
                ? live.series.front()->event
                : rec.event;
            // best_of: the actual played map count is the most reliable
            // signal we have here. Series::is_over() guarantees this matches
            // the BO bound for completed series (3 maps -> bo3, 5 -> bo5).
            int bo = (int)live.series.size();
            std::string json = vlr::export_series_to_json(
                live.series, event_name, bo, s.gm.year, s.gm.day_in_year);
            if (json.empty()) {
                s.log.emplace_back("Export failed: serializer returned empty.");
                return;
            }
            std::string path = prompt_save_path(build_default_filename());
            if (path.empty()) return;  // user cancelled — no log noise
            std::ofstream out(path, std::ios::binary);
            if (!out.is_open()) {
                s.log.emplace_back("Export failed: could not open " + path);
                return;
            }
            out << json;
            out.close();
            if (!out) {
                s.log.emplace_back("Export failed: write error on " + path);
                return;
            }
            s.log.emplace_back("Exported series to: " + path);
        };

        // Gate: at least one map in series + currently viewing the LAST map.
        // Recordings are by definition finished, so no further match-state
        // check is needed.
        bool can_export_series =
            !live.series.empty()
            && live.current_map_idx == (int)live.series.size() - 1;

        // === Auto-play: per-kill pacing inside a round ===
        // We advance event_cursor through the current round's events one
        // at a time (showing kills sequentially in the feed). When we run
        // out of events for the current round we tick round_cursor and
        // reset event_cursor to start the next round. Speed selector:
        //   0=Slow (1.2s/kill), 1=Normal (0.6s), 2=Fast (0.25s),
        //   3=Round-skip (legacy: round_cursor++ per tick).
        const float kSpeedDelays[4] = { 1.20f, 0.60f, 0.25f, 0.30f };
        float tick_delay = kSpeedDelays[std::clamp(live.playback_speed, 0, 3)];
        if (live.auto_play && live.round_cursor < (int)rounds.size()) {
            live.auto_timer += dt;
            if (live.auto_timer >= tick_delay) {
                live.auto_timer = 0.0f;
                if (live.playback_speed == 3) {
                    // Legacy round-skip mode — whole round per tick.
                    live.round_cursor += 1;
                    live.event_cursor = 0;
                } else {
                    // Per-kill mode: tick events; advance round when
                    // current round exhausted.
                    auto& cur_round = rounds[(std::size_t)live.round_cursor];
                    int n_events = static_cast<int>(cur_round.events.size());
                    if (live.event_cursor < n_events) {
                        live.event_cursor += 1;
                    } else {
                        // End-of-round: small pause already absorbed by
                        // tick_delay; advance to next round.
                        live.round_cursor += 1;
                        live.event_cursor = 0;
                    }
                }
            }
        }
        int shown_idx = std::min<int>(live.round_cursor, (int)rounds.size());
        int t1_score_now = 0, t2_score_now = 0;
        if (shown_idx > 0) {
            t1_score_now = rounds[shown_idx - 1].t1_score;
            t2_score_now = rounds[shown_idx - 1].t2_score;
        }

        // Helper: derive (blue_tag, red_tag) comp tags for a recording by
        // counting roles of the agent each player ran. Returns {nullptr,
        // nullptr} when stats / team rosters are missing.
        auto comp_tags_for = [](const vlr::RecordedMatch& r)
            -> std::pair<const char*, const char*> {
            if (!r.history_record || !r.history_record->blue_team.size()
                || !r.history_record->red_team.size()) {
                return {nullptr, nullptr};
            }
            const auto& hr = *r.history_record;
            auto build_tag = [&](const std::vector<vlr::Player*>& roster)
                -> const char* {
                vlr::CompPlan plan{};
                int counted = 0;
                for (auto* pl : roster) {
                    if (!pl) continue;
                    auto it = hr.stats.find(pl);
                    if (it == hr.stats.end()) continue;
                    if (auto* ag = vlr::find_agent_by_name(it->second.agent)) {
                        plan.need[(int)ag->role] += 1;
                        ++counted;
                    }
                }
                if (counted == 0) return nullptr;
                return vlr::comp_tag_name(vlr::comp_tag_of(plan));
            };
            return {build_tag(hr.blue_team), build_tag(hr.red_team)};
        };

        // === Series Comp Evolution panel ===
        // Multi-map only — single-map opens hide it. Shows comp tag each
        // team ran on each map of the series, with the active row gold-
        // accented. Sits ABOVE the score banner so the user reads the
        // series narrative before the current-map detail.
        //
        // SPOILER GUARD: render rows only up to the currently-viewed map.
        // Showing all rows reveals series length (3 rows = BO3, 5 = BO5)
        // and pre-spoils future-map comp picks. Rows for not-yet-reached
        // maps stay hidden until the user navigates onto them.
        if (live.series.size() > 1) {
            ImGui::PushID("##series_comp_evolution");
            H2("SERIES COMP EVOLUTION");
            if (ImGui::BeginTable("##series_evo_tbl", 4,
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableSetupColumn("MAP",  ImGuiTableColumnFlags_WidthFixed, 60);
                ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("BLUE COMP", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("RED COMP",  ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                std::size_t evo_max = std::min<std::size_t>(
                    live.series.size(),
                    static_cast<std::size_t>(live.current_map_idx) + 1);
                for (std::size_t i = 0; i < evo_max; ++i) {
                    const auto& sm = live.series[i];
                    bool is_active = ((int)i == live.current_map_idx);
                    ImGui::TableNextRow();
                    if (is_active) ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));

                    ImGui::TableNextColumn();
                    ImGui::Text("Map %zu", i + 1);

                    ImGui::TableNextColumn();
                    if (sm) ImGui::Text("%s", sm->map_name.c_str());
                    else    ImGui::TextDisabled("—");

                    const char* btag = nullptr;
                    const char* rtag = nullptr;
                    if (sm) {
                        auto pair = comp_tags_for(*sm);
                        btag = pair.first;
                        rtag = pair.second;
                    }
                    ImGui::TableNextColumn();
                    if (btag) ImGui::Text("%s", btag);
                    else      ImGui::TextDisabled("—");

                    ImGui::TableNextColumn();
                    if (rtag) ImGui::Text("%s", rtag);
                    else      ImGui::TextDisabled("—");

                    if (is_active) ImGui::PopStyleColor();
                }
                ImGui::EndTable();
            }
            ImGui::PopID();
            ImGui::Spacing();
        }

        DrawScoreBanner(rec, t1_score_now, t2_score_now, shown_idx, rounds.size());
        // Per-team comp tag subtitle row — sits directly below the score
        // banner, mirroring its left/right team layout. Only shown when
        // we can derive both comps from the recording's history_record.
        {
            auto cur_tags = comp_tags_for(rec);
            if (cur_tags.first || cur_tags.second) {
                ImGui::PushID("##live_comp_tag_row");
                ImGui::PushFont(g_font_small);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                if (cur_tags.first) {
                    ImGui::Text("%s comp: %s", rec.blue_name.c_str(), cur_tags.first);
                } else {
                    ImGui::Text("%s comp: —", rec.blue_name.c_str());
                }
                // Right-align the red team's comp tag on the same line.
                char red_buf[160];
                if (cur_tags.second) {
                    std::snprintf(red_buf, sizeof(red_buf), "%s comp: %s",
                                  rec.red_name.c_str(), cur_tags.second);
                } else {
                    std::snprintf(red_buf, sizeof(red_buf), "%s comp: —",
                                  rec.red_name.c_str());
                }
                ImVec2 sz = ImGui::CalcTextSize(red_buf);
                float right_x = ImGui::GetContentRegionAvail().x - sz.x;
                if (right_x < 0) right_x = 0;
                ImGui::SameLine(right_x);
                ImGui::TextUnformatted(red_buf);
                ImGui::PopStyleColor();
                ImGui::PopFont();
                ImGui::PopID();
            }
        }
        ImGui::Spacing();
        DrawRoundTimeline(live, rounds, rec);
        ImGui::Spacing();

        // Controls row — broadcast transport bar.
        ImGui::PushStyleColor(ImGuiCol_Button, toV4(live.auto_play ? kVlrRed : kVlrGreen));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(kVlrRedDim));
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kBgDeep));
        if (ImGui::Button(live.auto_play ? "Pause" : "Auto Play", ImVec2(120, 34))) {
            live.auto_play = !live.auto_play;
        }
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        if (ImGui::Button("Next Round", ImVec2(120, 34))) {
            if (live.round_cursor < (int)rounds.size()) live.round_cursor++;
            live.event_cursor = 0;
        }
        ImGui::SameLine();
        if (ImGui::Button("Skip 5", ImVec2(80, 34))) {
            live.round_cursor = std::min<int>(live.round_cursor + 5, (int)rounds.size());
            live.event_cursor = 0;
        }
        ImGui::SameLine();
        if (ImGui::Button("Skip to End", ImVec2(120, 34))) {
            live.round_cursor = (int)rounds.size();
            live.event_cursor = 0;
        }
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(14, 0)); ImGui::SameLine();
        // Segmented speed selector pill row — same playback_speed values
        // (0=Slow 1=Normal 2=Fast 3=Round-skip), just a cleaner control
        // than the dropdown. Selected segment uses the accent fill.
        {
            const char* speed_labels[] = {"Slow", "Normal", "Fast", "Round-skip"};
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 0));
            for (int sp = 0; sp < 4; ++sp) {
                bool sel = (live.playback_speed == sp);
                ImGui::PushStyleColor(ImGuiCol_Button,
                    toV4(sel ? kAccent : kSurfaceAlt));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                    toV4(sel ? kAccent : kSurfaceHi));
                ImGui::PushStyleColor(ImGuiCol_Text,
                    toV4(sel ? kBgDeep : kVlrSub));
                if (ImGui::Button(speed_labels[sp], ImVec2(0, 34))) {
                    live.playback_speed = sp;
                }
                ImGui::PopStyleColor(3);
                if (sp < 3) ImGui::SameLine();
            }
            ImGui::PopStyleVar();
        }
        // Series navigation: only shown when this is a multi-map series.
        // Safety: only honor the series if every entry is non-null AND
        // current_map_idx is in-range AND the current recording matches
        // the series slot. Otherwise treat as single-map (clear series).
        bool series_ok = live.series.size() > 1
                      && live.current_map_idx >= 0
                      && live.current_map_idx < (int)live.series.size();
        if (series_ok) {
            for (auto& m : live.series) {
                if (!m || !m->round_history) { series_ok = false; break; }
            }
        }
        if (!series_ok) {
            // Stale series state — drop it so map-switch buttons can't
            // touch dangling/mismatched recordings (the historic crash
            // when switching back to Map 1 after a tournament watch).
            live.series.clear();
            live.current_map_idx = 0;
        }
        if (live.series.size() > 1) {
            ImGui::SameLine();
            ImGui::Dummy(ImVec2(20, 0));
            ImGui::SameLine();
            bool can_prev = live.current_map_idx > 0;
            bool can_next = live.current_map_idx + 1 < (int)live.series.size();
            if (!can_prev) ImGui::BeginDisabled();
            if (ImGui::Button("< Prev Map", ImVec2(110, 32))) {
                int new_idx = live.current_map_idx - 1;
                if (new_idx >= 0 && new_idx < (int)live.series.size()
                    && live.series[new_idx] && live.series[new_idx]->round_history) {
                    live.current_map_idx = new_idx;
                    live.recording = live.series[new_idx];
                    live.round_cursor = 0;
                    live.auto_play = false;
                }
            }
            if (!can_prev) ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
            // Series length intentionally HIDDEN — showing "Map 1/3" tells
            // the user it's a BO3 (or BO5), spoiling whether further maps
            // remain. We just show the current map number; the Next/Prev
            // buttons grey out at series boundaries.
            ImGui::Text("Map %d", live.current_map_idx + 1);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (!can_next) ImGui::BeginDisabled();
            if (ImGui::Button("Next Map >", ImVec2(110, 32))) {
                int new_idx = live.current_map_idx + 1;
                if (new_idx >= 0 && new_idx < (int)live.series.size()
                    && live.series[new_idx] && live.series[new_idx]->round_history) {
                    live.current_map_idx = new_idx;
                    live.recording = live.series[new_idx];
                    live.round_cursor = 0;
                    live.auto_play = false;
                }
            }
            if (!can_next) ImGui::EndDisabled();
        }
        // === Slot 1: Export to JSON button (map-nav row) ===
        // Only visible when we're on the final map of a recorded series.
        // Sits in the controls row alongside Next Map / Prev Map.
        if (can_export_series) {
            ImGui::SameLine();
            ImGui::Dummy(ImVec2(20, 0));
            ImGui::SameLine();
            ImGui::PushID("export_json_navrow");
            ImGui::PushStyleColor(ImGuiCol_Button,        toV4(kVlrGold));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(kVlrGold));
            ImGui::PushStyleColor(ImGuiCol_Text,          IM_COL32(0x10, 0x14, 0x1A, 0xFF));
            if (ImGui::Button("Export JSON", ImVec2(130, 32))) {
                run_export();
            }
            ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Export full series to JSON file\n"
                    "(every map, every player, full stats).");
            }
            ImGui::PopID();
        }
        ImGui::SameLine();
        // Spoiler-free round counter: total rounds hidden so the user
        // can't pre-read whether the match went to overtime. " — FINAL"
        // (below) signals completion once the cursor reaches the end.
        ImGui::TextDisabled("Round %d", shown_idx);
        if (live.round_cursor >= (int)rounds.size()) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
            ImGui::Text(" — FINAL");
            ImGui::PopStyleColor();
            // MVP highlight.
            if (history_record.mvp) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                ImGui::Text("  |  MVP: %s", player_label(*history_record.mvp).c_str());
                ImGui::PopStyleColor();
            }
        }

        ImGui::Spacing();

        // Responsive split: scoreboards | round feed. Side-by-side only
        // when the modal content region is wide enough for both panels;
        // otherwise stack them vertically so nothing overflows / drifts
        // off-screen on a narrow window. The modal window owns the single
        // vertical scroll — neither panel scrolls internally.
        float live_avail_w = ImGui::GetContentRegionAvail().x;
        bool live_two_col = (live_avail_w >= 1000.0f);
        // Replaced deprecated ImGui::Columns(2) with two side-by-side
        // BeginChild panels + SameLine. Preserves the prior 60/40 split
        // and the responsive stacked fallback when the modal is narrow.
        // BeginChild is safe to host arbitrary nested widgets (tables,
        // selectables, draw-list painting) — unlike inner BeginTable.
        float live_left_w  = live_two_col ? (live_avail_w * 0.60f - 6.0f) : 0.0f;
        float live_right_w = live_two_col ? (live_avail_w - live_left_w - 14.0f) : 0.0f;
        if (live_two_col) {
            ImGui::BeginChild("##live_left", ImVec2(live_left_w, 0),
                              ImGuiChildFlags_AutoResizeY);
        }

        // Live partial stats: only show what's been revealed up to the
        // current cursor. Final K/D/FB/KAST come from the event log; A/dmg
        // are scaled approximations until the match ends.
        auto live_stats = compute_partial_stats(rec, live.round_cursor);

        // Header label so the user knows whether they're seeing live data
        // or the finalized scoreboard.
        bool is_finalized = (live.round_cursor >= (int)rounds.size());
        ImGui::PushFont(g_font_small);
        if (is_finalized) {
            ImGui::TextColored(toV4(kVlrGold), "FINAL — actual match totals");
        } else {
            // Spoiler-free LIVE counter: drop "/ total".
            ImGui::TextColored(toV4(kVlrSub),
                               "LIVE — stats accurate as of round %d",
                               shown_idx);
        }
        ImGui::PopFont();

        // Left: side-by-side scoreboards stacked vertically.
        //
        // Build a (roster, name, tag, brand_color) bundle for each side.
        // For pro/league matches the TeamPtr survives (it's owned by the
        // world's `teams` vector) and we use its roster + identity. For
        // solo Q matches the synthetic Blue/Red Teams die when
        // force_solo_match() returns, so team1.lock() comes back null —
        // we fall back to the recording's history_record.blue_team /
        // red_team (raw vector<Player*>) plus rec.blue_name / red_name
        // and a default blue/red brand color. Without this fallback the
        // solo Q live viewer's scoreboard renders empty.
        vlr::TeamPtr rec_t1 = rec.team1.lock();
        vlr::TeamPtr rec_t2 = rec.team2.lock();

        auto resolve_side = [&](const vlr::TeamPtr& tp,
                                const std::string& fallback_name,
                                const std::vector<vlr::Player*>& fallback_roster,
                                ImU32 fallback_brand)
            -> std::tuple<std::vector<vlr::Player*>, std::string, std::string, ImU32> {
            if (tp) {
                std::vector<vlr::Player*> roster;
                roster.reserve(tp->roster.size());
                for (auto& sp : tp->roster) roster.push_back(sp.get());
                ImU32 brand = (tp->color_primary_r || tp->color_primary_g ||
                               tp->color_primary_b)
                    ? IM_COL32(tp->color_primary_r, tp->color_primary_g,
                               tp->color_primary_b, 0xFF)
                    : fallback_brand;
                return {std::move(roster), tp->name, tp->tag, brand};
            }
            // Solo Q / orphaned recording path. No tag — we never minted
            // one for the synthetic Team. Brand falls back to blue/red.
            return {fallback_roster, fallback_name, std::string{}, fallback_brand};
        };

        auto [t1_roster, t1_name, t1_tag, t1_brand] = resolve_side(
            rec_t1, rec.blue_name,
            history_record.blue_team, kVlrBlue);
        auto [t2_roster, t2_name, t2_tag, t2_brand] = resolve_side(
            rec_t2, rec.red_name,
            history_record.red_team, kVlrRed);

        DrawTeamScoreboard(t1_roster, t1_name, t1_tag, t1_brand,
                           live_stats, history_record, "##sb_blue");
        ImGui::Spacing();
        DrawTeamScoreboard(t2_roster, t2_name, t2_tag, t2_brand,
                           live_stats, history_record, "##sb_red");

        // === Slot 2: Export to JSON button (scoreboard panel footer) ===
        // Mirror of Slot 1, placed below both scoreboards so users at the
        // post-match results screen don't have to reach for the controls
        // row above. Same gating, same handler.
        if (can_export_series) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::PushID("export_json_scoreboard");
            ImGui::PushStyleColor(ImGuiCol_Button,        toV4(kVlrGold));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(kVlrGold));
            ImGui::PushStyleColor(ImGuiCol_Text,          IM_COL32(0x10, 0x14, 0x1A, 0xFF));
            if (ImGui::Button("Export Series to JSON", ImVec2(220, 32))) {
                run_export();
            }
            ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Export full series to JSON file\n"
                    "(every map, every player, full stats).");
            }
            ImGui::PopID();
        }

        if (live_two_col) {
            ImGui::EndChild();          // close ##live_left
            ImGui::SameLine();
            ImGui::BeginChild("##live_right", ImVec2(live_right_w, 0),
                              ImGuiChildFlags_AutoResizeY);
        } else {
            // Stacked layout — visual breather between the scoreboards
            // and the round feed instead of a column gutter.
            ImGui::Spacing();
            ThinDivider(6);
        }

        // === ROUND DETAIL: result card + economy + kill feed ===
        SectionHeader("ROUND FEED", nullptr, kAccent);
        // Round to display events for: the IN-PROGRESS round if we're
        // partway through it (event_cursor > 0), else the most recently
        // completed round. This produces real play-by-play in auto_play
        // mode — kills tick out one at a time as event_cursor advances.
        int events_round_idx = -1;
        bool round_in_progress = false;
        if (live.event_cursor > 0 && live.round_cursor < (int)rounds.size()) {
            events_round_idx = live.round_cursor;
            round_in_progress = true;
        } else if (shown_idx > 0) {
            events_round_idx = shown_idx - 1;
        }
        if (events_round_idx >= 0) {
            auto& r = rounds[events_round_idx];
            int events_to_show = round_in_progress
                ? std::min(live.event_cursor, (int)r.events.size())
                : (int)r.events.size();
            ImU32 c1 = rec_t1
                ? IM_COL32(rec_t1->color_primary_r, rec_t1->color_primary_g, rec_t1->color_primary_b, 0xFF)
                : kVlrBlue;
            ImU32 c2 = rec_t2
                ? IM_COL32(rec_t2->color_primary_r, rec_t2->color_primary_g, rec_t2->color_primary_b, 0xFF)
                : kVlrRed;
            bool t1_won = (r.winner_name == rec.blue_name);

            // --- Round result card -------------------------------------
            // round # | winning-side color chip | outcome | score-after.
            // We don't have an explicit outcome enum on RoundLog, so infer
            // a tasteful label from the alive counts of the final event:
            // a side wiped => "elimination", otherwise "round won".
            {
                ImGui::BeginChild("##rd_hdr", ImVec2(0, 48),
                                  ImGuiChildFlags_Border,
                                  ImGuiWindowFlags_NoScrollbar |
                                  ImGuiWindowFlags_NoScrollWithMouse);
                ImVec2 hp = ImGui::GetCursorScreenPos();
                ImVec2 hs = ImGui::GetContentRegionAvail();
                ImDrawList* hdl = ImGui::GetWindowDrawList();
                ImColor dim_w(t1_won ? c1 : c2);
                dim_w.Value.x *= 0.30f; dim_w.Value.y *= 0.30f;
                dim_w.Value.z *= 0.30f; dim_w.Value.w = 1.0f;
                hdl->AddRectFilled(hp, ImVec2(hp.x + hs.x, hp.y + hs.y),
                                   (ImU32)dim_w, 4.0f);
                // Winning-side color chip on the left.
                hdl->AddRectFilled(hp, ImVec2(hp.x + 6, hp.y + hs.y),
                                   t1_won ? c1 : c2);
                ImGui::SetCursorScreenPos(ImVec2(hp.x + 16, hp.y + 11));
                ImGui::PushFont(g_font_h2);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                ImGui::Text("ROUND %d", r.round);
                ImGui::PopStyleColor();
                ImGui::PopFont();
                ImGui::SameLine();
                ImGui::PushFont(g_font_small);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrText));
                if (round_in_progress) {
                    // Spoiler-safe: never reveal winner or total kills mid-round.
                    ImGui::Text("   in progress \xE2\x80\x94 %d kills so far",
                                events_to_show);
                } else {
                    const char* how = "round won";
                    if (!r.events.empty()) {
                        const auto& last = r.events.back();
                        if (last.t1_alive == 0 || last.t2_alive == 0)
                            how = "team eliminated";
                    }
                    ImGui::Text("   %s \xE2\x80\x94 %s   (%d - %d)",
                                r.winner_name.c_str(), how,
                                r.t1_score, r.t2_score);
                }
                ImGui::PopStyleColor();
                ImGui::PopFont();
                ImGui::EndChild();
            }

            // --- Economy panel -----------------------------------------
            // Compact per-team loadout card derived from round invest.
            {
                auto loadout_label = [](int invest) -> const char* {
                    if (invest >= 18000) return "OP BUY";
                    if (invest >= 14000) return "FULL BUY";
                    if (invest >= 8000)  return "HALF BUY";
                    if (invest >= 4500)  return "FORCE";
                    return "ECO";
                };
                bool have_econ = (r.t1_invest > 0 || r.t2_invest > 0);
                ImGui::Spacing();
                ImGui::BeginChild("##econ", ImVec2(0, 66),
                                  ImGuiChildFlags_Border,
                                  ImGuiWindowFlags_NoScrollbar |
                                  ImGuiWindowFlags_NoScrollWithMouse);
                ImVec2 ep = ImGui::GetCursorScreenPos();
                ImVec2 es = ImGui::GetContentRegionAvail();
                ImDrawList* edl = ImGui::GetWindowDrawList();
                if (!have_econ) {
                    ImGui::PushFont(g_font_small);
                    ImGui::SetCursorScreenPos(ImVec2(ep.x + 12, ep.y + 22));
                    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kTextFaint));
                    ImGui::TextUnformatted(
                        "Economy data not recorded for this round.");
                    ImGui::PopStyleColor();
                    ImGui::PopFont();
                } else {
                    float bar_w = es.x - 24.0f;
                    float bar_h = 16.0f;
                    auto frac = [](int v){ return std::min(1.0f,
                        (float)v / 22000.0f); };
                    auto draw_row = [&](float yo, ImU32 col, int invest,
                                        const char* tag) {
                        edl->AddRectFilled(ImVec2(ep.x + 12, ep.y + yo),
                            ImVec2(ep.x + 12 + bar_w, ep.y + yo + bar_h),
                            IM_COL32(0x14, 0x1B, 0x26, 0xFF), 3.0f);
                        edl->AddRectFilled(ImVec2(ep.x + 12, ep.y + yo),
                            ImVec2(ep.x + 12 + bar_w * frac(invest),
                                   ep.y + yo + bar_h), col, 3.0f);
                        char b[96];
                        std::snprintf(b, sizeof(b), "%s  \xE2\x80\xA2  %s  ($%d)",
                            tag, loadout_label(invest), invest);
                        edl->AddText(g_font_small ? g_font_small : nullptr,
                            g_font_small ? g_font_small->FontSize : 13.0f,
                            ImVec2(ep.x + 18, ep.y + yo + 1), kVlrText, b);
                    };
                    draw_row(10.0f, c1,
                        r.t1_invest,
                        rec_t1 && !rec_t1->tag.empty() ? rec_t1->tag.c_str()
                            : rec.blue_name.c_str());
                    draw_row(36.0f, c2,
                        r.t2_invest,
                        rec_t2 && !rec_t2->tag.empty() ? rec_t2->tag.c_str()
                            : rec.red_name.c_str());
                }
                ImGui::EndChild();
            }

            // --- Kill feed ---------------------------------------------
            // Each kill is a self-contained, fixed-height card. Layout is
            // computed ONCE per row (no overlapping absolute cursor math —
            // the old draw-list text bleed bug) and laid out via the
            // window draw list inside a measured InvisibleButton slot so
            // ImGui owns the row advance and nothing overlaps.
            ImGui::Spacing();
            ImGui::BeginChild("##events", ImVec2(0, 0),
                              ImGuiChildFlags_Border |
                              ImGuiChildFlags_AutoResizeY,
                              ImGuiWindowFlags_NoScrollbar |
                              ImGuiWindowFlags_NoScrollWithMouse);

            std::unordered_map<vlr::Player*, int> kill_count;
            for (int k_i = 0; k_i < events_to_show && k_i < (int)r.events.size(); ++k_i) {
                auto& ev = r.events[k_i];
                if (ev.killer) kill_count[ev.killer]++;
            }

            const float kRowH = 30.0f;
            const float kRowGap = 4.0f;
            ImFont* fs = g_font_small;
            float fsz = fs ? fs->FontSize : 13.0f;
            int ev_i = 0;
            int last_clutch_kills = 0;          // for the callout banner
            std::string clutch_name, ace_name;
            // Running per-killer counter — replaces the inner O(N) re-scan
            // that made the kill-feed loop O(N^2). Increment as we iterate.
            std::unordered_map<vlr::Player*, int> kills_running;
            for (int e_idx = 0; e_idx < events_to_show && e_idx < (int)r.events.size(); ++e_idx) {
                auto& ev = r.events[e_idx];
                if (!ev.killer || !ev.victim) continue;
                bool killer_t1 = (ev.team_won == 1);
                ImU32 kcol = killer_t1 ? c1 : c2;
                ImU32 vcol = killer_t1 ? c2 : c1;
                // Trade: previous kill went the other way (mirrors the
                // partial-stats heuristic — RoundEvent has no trade flag).
                bool traded = (e_idx > 0 &&
                    r.events[e_idx - 1].team_won != ev.team_won);
                int this_kill_idx = ++kills_running[ev.killer];
                int total_kills = kill_count[ev.killer];
                int adv = killer_t1 ? (ev.t1_alive - ev.t2_alive)
                                    : (ev.t2_alive - ev.t1_alive);
                bool clutch = (adv < 0 && this_kill_idx >= 2);
                if (clutch && this_kill_idx > last_clutch_kills) {
                    last_clutch_kills = this_kill_idx;
                    clutch_name = player_label(*ev.killer);
                }
                if (total_kills >= 5) ace_name = player_label(*ev.killer);

                ImGui::PushID(ev_i++);
                // Reserve the row with a real ImGui item so the next row
                // advances correctly — this is the core overlap fix.
                ImVec2 rp = ImGui::GetCursorScreenPos();
                float rw = ImGui::GetContentRegionAvail().x;
                ImGui::InvisibleButton("##krow", ImVec2(rw, kRowH + kRowGap));
                ImDrawList* rdl = ImGui::GetWindowDrawList();
                ImVec2 a(rp.x, rp.y), b(rp.x + rw - 2, rp.y + kRowH);

                rdl->AddRectFilled(a, b, kBgDeep, 5.0f);
                float bgi = 0.18f;
                if (total_kills >= 3) bgi = 0.30f;
                if (total_kills >= 4) bgi = 0.42f;
                ImColor bg(kcol);
                bg.Value.x *= bgi; bg.Value.y *= bgi; bg.Value.z *= bgi;
                bg.Value.w = 1.0f;
                rdl->AddRectFilled(a, b, (ImU32)bg, 5.0f);
                if (total_kills >= 3)
                    rdl->AddRect(a, b, kVlrGold, 5.0f, 0, 1.0f);
                rdl->AddRectFilled(a, ImVec2(a.x + 4, b.y), kcol, 5.0f,
                                   ImDrawFlags_RoundCornersLeft);

                float ty = rp.y + (kRowH - fsz) * 0.5f;
                // alive badge (fixed slot)
                char ab[12];
                std::snprintf(ab, sizeof(ab), "%dv%d",
                              ev.t1_alive, ev.t2_alive);
                rdl->AddText(fs, fsz, ImVec2(rp.x + 12, ty), kVlrSub, ab);
                // killer (bold team color)
                float cx = rp.x + 64.0f;
                std::string kn = player_label(*ev.killer);
                rdl->AddText(fs, fsz, ImVec2(cx, ty), kcol, kn.c_str());
                cx += ImGui::CalcTextSize(kn.c_str()).x + 6.0f;
                if (total_kills >= 2) {
                    const char* mk = total_kills == 2 ? "[2K]"
                                  : total_kills == 3 ? "[3K]"
                                  : total_kills == 4 ? "[4K!]"
                                  : "[ACE]";
                    rdl->AddText(fs, fsz, ImVec2(cx, ty), kVlrGold, mk);
                    cx += ImGui::CalcTextSize(mk).x + 6.0f;
                }
                // Kill separator. The recording stores no weapon string
                // or per-kill headshot flag, so we use a clean directional
                // marker rather than fabricating data.
                rdl->AddText(fs, fsz, ImVec2(cx, ty), kTextFaint,
                             "\xE2\x96\xB8");
                cx += 18.0f;
                // victim (dim)
                std::string vn = player_label(*ev.victim);
                rdl->AddText(fs, fsz, ImVec2(cx, ty), vcol, vn.c_str());
                cx += ImGui::CalcTextSize(vn.c_str()).x + 8.0f;
                if (traded) {
                    rdl->AddText(fs, fsz, ImVec2(cx, ty), kVlrSub, "[trade]");
                    cx += ImGui::CalcTextSize("[trade]").x + 6.0f;
                }
                if (clutch) {
                    rdl->AddText(fs, fsz, ImVec2(cx, ty), kVlrRed, "CLUTCH!");
                }
                ImGui::PopID();
            }
            ImGui::EndChild();

            // --- Clutch / multi-kill callout banner --------------------
            if (!ace_name.empty() || !clutch_name.empty()) {
                ImGui::Spacing();
                ImGui::BeginChild("##callout", ImVec2(0, 36),
                                  ImGuiChildFlags_Border,
                                  ImGuiWindowFlags_NoScrollbar |
                                  ImGuiWindowFlags_NoScrollWithMouse);
                ImVec2 cp = ImGui::GetCursorScreenPos();
                ImVec2 cs = ImGui::GetContentRegionAvail();
                ImDrawList* cdl = ImGui::GetWindowDrawList();
                ImU32 cc = !ace_name.empty() ? kVlrGold : kVlrRed;
                ImColor dimc(cc);
                dimc.Value.x *= 0.22f; dimc.Value.y *= 0.22f;
                dimc.Value.z *= 0.22f; dimc.Value.w = 1.0f;
                cdl->AddRectFilled(cp, ImVec2(cp.x + cs.x, cp.y + cs.y),
                                   (ImU32)dimc, 4.0f);
                cdl->AddRectFilled(cp, ImVec2(cp.x + 5, cp.y + cs.y), cc);
                char cb[120];
                if (!ace_name.empty())
                    std::snprintf(cb, sizeof(cb), "ACE  \xE2\x80\x94  %s",
                                  ace_name.c_str());
                else
                    std::snprintf(cb, sizeof(cb), "CLUTCH  \xE2\x80\x94  %s",
                                  clutch_name.c_str());
                ImGui::SetCursorScreenPos(ImVec2(cp.x + 16, cp.y + 8));
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(cc));
                ImGui::TextUnformatted(cb);
                ImGui::PopStyleColor();
                ImGui::EndChild();
            }
        } else {
            ImGui::TextDisabled("Click Next Round / Auto Play to begin.");
        }

        if (live_two_col) ImGui::EndChild();  // close ##live_right

        ImGui::Separator();
        if (ImGui::Button("Close", ImVec2(120, 32))) {
            s.live.open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

}  // namespace

// ============================================================
// 9. Screens
// ============================================================
namespace {

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
    MutedText("VCT %s   |   Year %d   |   Day %d/%d   |   %s",
        t->region.c_str(), s.gm.year, s.gm.day_in_year + 1,
        vlr::GameManager::kDaysPerYear, s.gm.current_phase().c_str());
    VSpace(14);

    auto& league = s.gm.leagues.at(t->region);
    std::vector<vlr::TeamPtr> sorted_t;
    for (auto& tt : league->teams()) if (tt) sorted_t.push_back(tt);
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
        // === C4: Org Memory glance =========================================
        // Picks the most-pronounced (highest abs) memory metric for the user
        // team. If nothing crosses |25| we show a "building..." placeholder.
        // Frame-cheap: 6 ints — no walks over rosters.
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
            int best = -1, best_abs = 0;
            for (int i = 0; i < 6; ++i) {
                int a = std::abs(me[i].value);
                if (a > best_abs) { best_abs = a; best = i; }
            }
            if (best >= 0 && best_abs >= 25) {
                int v = me[best].value;
                ImU32 col = (v >=  30) ? kVlrGreen
                          : (v <= -30) ? kVlrRed
                          :              kVlrText;
                char vb[16];
                std::snprintf(vb, sizeof(vb), "%+d", v);
                StatTile(me[best].label, vb, col);
            } else {
                StatTile("ORG MEMORY", std::string("building..."), kVlrSub);
            }
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
            ImGui::TableNextRow();
            ImGui::PushID(idx++);
            ImGui::TableNextColumn(); CountryFlag(p->country_iso);
            ImGui::TableNextColumn();
            if (ImGui::Selectable(player_label(*p).c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
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
    auto traj_of = [](const vlr::PlayerPtr& pl)
        -> std::pair<const char*, ImU32> {
        int ovr = (int)std::lround(pl->ovr());
        int gap = pl->potential - ovr;
        bool young = (pl->age <= 22);
        bool inconsistent = (pl->consistency > 0 && pl->consistency < 45);
        if (young && gap >= 8 && pl->potential >= 86)
            return {"Future Star", kVlrGreen};
        if (young && gap >= 8 && inconsistent)
            return {"High-Risk", kRoleFlex};
        if (gap >= 8 && young)   return {"Rising", kVlrGreen};
        if (ovr > pl->potential || pl->age >= 29)
            return {"Declining", kVlrRedDim};
        if (gap >= 4)            return {"Developing", kAccent};
        return {"Established", kVlrSub};
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
            if (ImGui::Selectable(player_label(*p).c_str(), false,
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

void DrawRoster(AppState& s) {
    auto t = s.gm.user_team;
    if (!t) return;
    ImGui::PushFont(g_font_h1);
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
    ImGui::TextUnformatted("MY ROSTER");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    // Comp shape — every starting 5 is 4 canonical gameplay slots plus a
    // flexible 5th. Flex and IGL are independent overlay flags (any
    // starter can carry zero, one, or both).
    MutedText("Comp shape: 1D + 1C + 1S + 1I + 1 flexible 5th "
        "(Flex/secondary I/secondary C). 1 IGL flag overlaid on any starter.   "
        "(Tends toward %s comps)",
        vlr::comp_tag_name(vlr::comp_tag_of(t->target_comp)));
    VSpace(14);

    // === Bucket the roster into the canonical 4 + flexible 5th ==========
    // Layout: 4 named slots (Duelist / Controller / Sentinel / Initiator)
    // each filled by the first roster member matching that position, plus
    // a 5th "5th / Flex" slot for whoever's left over from the starting 5.
    // Bucketing logic: walk roster[0..4]. Track which Position has been
    // filled. First occupant per canonical position locks that slot; any
    // subsequent starter goes into the 5th slot. Everyone past roster[4]
    // (or any extras displaced by the bucketing pass) drops to bench,
    // sorted by OVR descending.
    std::array<vlr::PlayerPtr, (size_t)vlr::Position::Count> canonical{};
    vlr::PlayerPtr fifth;       // the flexible 5th occupant
    std::vector<vlr::PlayerPtr> bench;
    {
        // First pass: only starters (roster[0..4]) compete for the 5 slots.
        std::size_t starter_count = std::min<std::size_t>(t->roster.size(), 5);
        std::vector<vlr::PlayerPtr> displaced;
        for (std::size_t i = 0; i < starter_count; ++i) {
            auto& p = t->roster[i];
            if (!p) continue;
            std::size_t idx = (std::size_t)vlr::position_of(*p);
            if (!canonical[idx]) {
                canonical[idx] = p;
            } else if (!fifth) {
                fifth = p;
            } else {
                // Two starters share a canonical position AND the 5th
                // slot is already taken (e.g. 2D + 2C in the same 5).
                // The duplicate falls to the bench so the user can see
                // the invariant violation.
                displaced.push_back(p);
            }
        }
        // Bench = everyone past roster[4] + any displaced duplicates.
        for (std::size_t i = starter_count; i < t->roster.size(); ++i) {
            if (t->roster[i]) bench.push_back(t->roster[i]);
        }
        for (auto& d : displaced) bench.push_back(d);
        std::sort(bench.begin(), bench.end(),
                  [](const vlr::PlayerPtr& a, const vlr::PlayerPtr& b) {
                      return a->ovr() > b->ovr();
                  });
    }

    std::vector<vlr::PlayerPtr> to_release;

    // Lambda: render one player row. nullptr = "[empty]" placeholder.
    // `dup_warn` = mark the position column with a duplicate warning.
    // `slot_label` overrides the empty-slot label; pass non-empty to render
    // e.g. "[empty 5th slot]" when slot_pos is meaningless (5th / Flex /
    // bench rows).
    auto render_row = [&](vlr::Position slot_pos,
                          const vlr::PlayerPtr& p,
                          bool dup_warn,
                          int row_id,
                          const char* slot_label = nullptr) {
        ImGui::PushID(row_id);
        ImGui::TableNextRow();

        if (!p) {
            // Empty slot — surface the gap loudly so the user can see
            // they're short a starter for this position (or 5th slot).
            ImGui::TableNextColumn(); ImGui::TextDisabled("--");
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
            if (slot_label && slot_label[0]) {
                ImGui::Text("[empty %s]", slot_label);
            } else {
                ImGui::Text("[empty %s]", vlr::position_name(slot_pos));
            }
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            if (slot_label && slot_label[0]) {
                ImGui::TextDisabled("--");      // no PositionBadge for 5th
            } else {
                PositionBadge(slot_pos);
            }
            ImGui::TableNextColumn(); ImGui::TextDisabled("--");
            ImGui::TableNextColumn(); ImGui::TextDisabled("--");
            ImGui::TableNextColumn(); ImGui::TextDisabled("--");
            ImGui::TableNextColumn(); ImGui::TextDisabled("--");
            ImGui::TableNextColumn(); ImGui::TextDisabled("--");
            ImGui::TableNextColumn(); ImGui::TextDisabled("--");
            ImGui::TableNextColumn(); // ACTIONS column blank
            ImGui::PopID();
            return;
        }

        ImGui::TableNextColumn(); CountryFlag(p->country_iso);
        ImGui::TableNextColumn();
        // NOTE 1: do NOT use SpanAllColumns — it covers the Release
        //         button in the last column, swallowing its clicks.
        // NOTE 2: Do NOT add an inline "[I]" prefix for imports here —
        //         it collides visually with player_label()'s "[I]"
        //         IGL prefix, making imports look like duplicate IGLs.
        //         Import status is shown as " (Import)" in the REGION
        //         column instead.
        if (ImGui::Selectable(player_label(*p).c_str(), false)) {
            OpenPlayerModal(s, p);
        }
        ImGui::TableNextColumn();
        // Canonical order: PositionBadge → FlexBadge (if is_flex) →
        // IglBadge (if is_igl). Both overlay flags are independent and a
        // single player can carry zero, one, or both.
        PositionBadge(vlr::position_of(*p));
        if (p->is_flex) { ImGui::SameLine(); FlexBadge(); }
        if (p->is_igl)  { ImGui::SameLine(); IglBadge();  }
        if (dup_warn) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
            ImGui::PushFont(g_font_small);
            ImGui::Text("[duplicate]");
            ImGui::PopFont();
            ImGui::PopStyleColor();
        }
        ImGui::TableNextColumn();
        if (p->region != t->region) {
            ImGui::Text("%s", p->region.c_str());
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
            ImGui::PushFont(g_font_small);
            ImGui::Text("(Import)");
            ImGui::PopFont();
            ImGui::PopStyleColor();
        } else {
            ImGui::Text("%s", p->region.c_str());
        }
        ImGui::TableNextColumn(); ImGui::Text("%d", p->age);
        ImGui::TableNextColumn(); ImGui::Text("%.0f", p->ovr());
        ImGui::TableNextColumn(); ImGui::Text("%d", p->potential);
        ImGui::TableNextColumn();
        ImGui::Text("exp %d", p->contract.exp_year);
        // C3: Contract status pill — surfaces expiring deals so the user
        // can spot re-sign candidates at a glance. Long-term deals
        // (>=3y) render no pill to keep the row uncluttered.
        {
            int yl = p->years_left(s.gm.year);
            if (yl <= 0) {
                ImGui::SameLine(); Pill("EXPIRED", kVlrRedDim);
            } else if (yl == 1) {
                ImGui::SameLine(); Pill("FINAL YEAR", kVlrGold);
            } else if (yl == 2) {
                ImGui::SameLine(); Pill("Expiring soon", kVlrSub);
            }
        }
        ImGui::TableNextColumn(); ImGui::Text("$%dK", p->contract.amount_k);
        ImGui::TableNextColumn();
        // C1: Re-sign / Extend button — any active rostered player can
        // open the negotiation modal at any time. Final-year deals use
        // "Re-sign" wording; mid-deal extensions read "Extend". Was
        // previously gated to `years_left <= 1` which silently locked
        // bench + mid-contract players out of negotiations — user spec
        // 2026-05-28: "all rostered players can be re-signed regardless
        // of starting/bench status". Defensive guard still hides this on
        // any row that somehow shows a FA team_name.
        {
            int yl = p->years_left(s.gm.year);
            bool can_resign = (yl >= 1)
                              && (p->team_name == t->name)
                              && p->team_name != "Free Agent"
                              && p->team_name != "Retired";
            if (can_resign) {
                ImGui::PushStyleColor(ImGuiCol_Button,        toV4(kAccent));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(kAccent2));
                const char* label = (yl <= 1) ? "Re-sign" : "Extend";
                if (ImGui::SmallButton(label)) {
                    s.show_resign_modal    = true;
                    s.resign_target        = p;
                    // Sentinel 0 -> modal fills both from cached offer on
                    // first frame (years = offer.years, amount = offer.amount_k).
                    s.resign_years_choice  = 0;
                    s.resign_amount_choice = 0;
                    s.resign_role_choice   = -1;       // default to natural
                    s.resign_cached_for    = nullptr;  // force recompute
                }
                ImGui::PopStyleColor(2);
                ImGui::SameLine();
            }
        }
        ImGui::PushStyleColor(ImGuiCol_Button,        toV4(kVlrRed));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(kVlrRedDim));
        if (ImGui::SmallButton("Release")) to_release.push_back(p);
        ImGui::PopStyleColor(2);
        ImGui::PopID();
    };

    // === Starters table — fixed 5-row order by Position =================
    if (BeginCard("##roster_starters")) {
    SectionHeader("STARTERS", "Four canonical slots + a flexible fifth", kAccent);
    if (ImGui::BeginTable("##fullroster", 10,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
            ImVec2(-FLT_MIN, 220))) {
        ImGui::TableSetupColumn("FLAG");
        ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("POSITION");
        ImGui::TableSetupColumn("REGION");
        ImGui::TableSetupColumn("AGE");
        ImGui::TableSetupColumn("OVR");
        ImGui::TableSetupColumn("POT");
        ImGui::TableSetupColumn("CONTRACT");
        ImGui::TableSetupColumn("SALARY");
        ImGui::TableSetupColumn("ACTIONS");
        ImGui::TableHeadersRow();
        int row_id = 0;
        // 4 canonical rows in enum order (D / C / S / I).
        for (size_t i = 0; i < (size_t)vlr::Position::Count; ++i) {
            auto slot_pos = (vlr::Position)i;
            const auto& occupant = canonical[i];
            render_row(slot_pos, occupant, false, row_id++);
        }
        // 5th / Flex row — whichever starter didn't fill a canonical
        // slot. Empty label is "[empty 5th slot]" (no PositionBadge
        // since the 5th can be any position).
        render_row(vlr::Position::Initiator, fifth, false, row_id++,
                   "5th slot");
        ImGui::EndTable();
    }
    } EndCard();

    // === Bench (overflow / duplicates) ==================================
    if (!bench.empty()) {
        VSpace(14);
        if (BeginCard("##roster_bench", kBgDeep)) {
        SectionHeader("BENCH", "Reserves, overflow & duplicate-position players", kVlrSub);
        if (ImGui::BeginTable("##fullroster_bench", 10,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                ImVec2(-FLT_MIN, 180))) {
            ImGui::TableSetupColumn("FLAG");
            ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("POSITION");
            ImGui::TableSetupColumn("REGION");
            ImGui::TableSetupColumn("AGE");
            ImGui::TableSetupColumn("OVR");
            ImGui::TableSetupColumn("POT");
            ImGui::TableSetupColumn("CONTRACT");
            ImGui::TableSetupColumn("SALARY");
            ImGui::TableSetupColumn("ACTIONS");
            ImGui::TableHeadersRow();
            int row_id = 1000;
            for (auto& p : bench) {
                render_row(vlr::position_of(*p), p, false, row_id++);
            }
            ImGui::EndTable();
        }
        } EndCard();
    }

    for (auto& r : to_release) {
        t->release_player(r);
        s.log.emplace_back("Released " + r->name + " from " + t->name + ".");
    }

    // === C2: Re-sign negotiation modal ==================================
    // Inlined here (per hard-rule #2: no new top-level functions). Opens
    // when a roster row's Re-sign SmallButton was clicked this/last
    // frame. Reads Agent A's `propose_resign_offer` + Agent B's
    // `market_value_estimate` and writes via `Team::resign_player`.
    if (s.show_resign_modal && s.resign_target) {
        ImGui::OpenPopup("Re-sign Player##resign_modal");
        ImVec2 rc = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(rc, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(860, 540), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("Re-sign Player##resign_modal",
                                   &s.show_resign_modal,
                                   ImGuiWindowFlags_NoSavedSettings)) {
            auto p = s.resign_target;

            // Cache the offer + market value on FIRST frame for this target.
            // `propose_resign_offer` -> `gen_contract` uses randomize_amount,
            // so recomputing each frame jitters the slider's max bound and
            // the value gets re-clamped, causing visible oscillation. Stable
            // snapshot per modal session is the correct UX anyway — once the
            // player has stated their ask, the user negotiates against THAT.
            if (s.resign_cached_for != p.get()) {
                s.resign_cached_offer = p->propose_resign_offer(*t, s.gm.year);
                s.resign_cached_mkt_k = t->market_value_estimate(*p);
                s.resign_cached_for   = p.get();
                if (s.resign_years_choice <= 0)
                    s.resign_years_choice = s.resign_cached_offer.years;
                if (s.resign_amount_choice <= 0)
                    s.resign_amount_choice = s.resign_cached_offer.amount_k;
            }
            const vlr::Player::ResignOffer& offer = s.resign_cached_offer;
            int mkt_k = s.resign_cached_mkt_k;

            // Clamp slider defaults to the offer's tolerance window on
            // first open. resign_amount_choice == 0 = "use ask".
            if (s.resign_amount_choice <= 0) {
                s.resign_amount_choice = offer.amount_k;
            }
            int years_lo = 1;
            int years_hi = std::max(1, offer.max_acceptable_years);
            int amt_lo   = std::max((int)vlr::kSalaryFloorK,
                                    offer.min_acceptable_k);
            int amt_hi   = std::max(amt_lo + 1,
                                    (int)std::min<long long>(
                                        vlr::kSalaryCapK,
                                        (long long)std::lround(
                                            offer.amount_k * 1.5)));
            s.resign_years_choice  = vlr::clamp_v(s.resign_years_choice,
                                                  years_lo, years_hi);
            s.resign_amount_choice = vlr::clamp_v(s.resign_amount_choice,
                                                  amt_lo, amt_hi);

            // === Header ================================================
            {
                std::string title = std::string("RE-SIGN ") + p->name;
                H2(title.c_str());
            }
            {
                std::string sig = p->signature_agent();
                if (!sig.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                    ImGui::Text("[*] Signature %s", sig.c_str());
                    ImGui::PopStyleColor();
                }
            }
            // Status pills row.
            {
                char exp_buf[32];
                std::snprintf(exp_buf, sizeof(exp_buf),
                              "Expires %d", p->contract.exp_year);
                Pill(exp_buf, kVlrSub);
                ImGui::SameLine();
                ImU32 wcol = kAccent;
                const char* wname = "Open";
                switch (t->window) {
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
            }
            ThinDivider(6);

            // === Two-column body ======================================
            ImGui::Columns(2, "##resign_cols", false);

            // ---- LEFT: player's ask -----------------------------------
            SectionHeader("PLAYER IS ASKING", nullptr, kVlrGold);
            ImGui::Text("Years requested: %d", offer.years);
            ImGui::Text("Amount requested: $%dK/yr", offer.amount_k);
            // Willingness bar — 0..100% color-tiered.
            {
                float w01 = (float)vlr::clamp_v(offer.willingness,
                                                0.0, 1.0);
                ImU32 wcol = (w01 >= 0.70f) ? kVlrGreen
                           : (w01 >= 0.40f) ? kVlrGold
                           :                  kVlrRed;
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, toV4(wcol));
                char wbuf[32];
                std::snprintf(wbuf, sizeof(wbuf), "%.0f%% willing",
                              w01 * 100.0f);
                ImGui::ProgressBar(w01, ImVec2(-FLT_MIN, 0), wbuf);
                ImGui::PopStyleColor();
            }
            if (!offer.explainer.empty()) {
                MutedText("%s", offer.explainer.c_str());
            }
            VSpace(6);
            ImGui::Text("Min acceptable: $%dK/yr", offer.min_acceptable_k);
            ImGui::Text("Max acceptable years: %d",
                        offer.max_acceptable_years);

            // ---- RIGHT: market + your offer ---------------------------
            ImGui::NextColumn();
            SectionHeader("MARKET & YOUR OFFER", nullptr, kAccent);
            ImGui::Text("Market value: $%dK/yr", mkt_k);
            ImGui::Text("Team budget: %s", fmt_money(t->budget).c_str());
            VSpace(4);

            ImGui::SetNextItemWidth(220);
            ImGui::SliderInt("Years", &s.resign_years_choice,
                             years_lo, years_hi);
            ImGui::SetNextItemWidth(220);
            ImGui::SliderInt("Salary $K/yr",
                             &s.resign_amount_choice,
                             amt_lo, amt_hi);

            // Role offer combo — the role the team is offering for the
            // new deal. Natural role is the default (no fit penalty); off-
            // role offers take a role_fit_score-derived score modifier.
            // Each entry surfaces a fit verdict so the user sees up-front
            // whether the offered role is plausible for this player.
            {
                const vlr::Role role_opts[4] = {
                    vlr::Role::Duelist, vlr::Role::Initiator,
                    vlr::Role::Controller, vlr::Role::Sentinel
                };
                int natural_idx = 0;
                for (int i = 0; i < 4; ++i)
                    if (role_opts[i] == p->primary_role) natural_idx = i;
                int cur_idx = (s.resign_role_choice < 0)
                                ? natural_idx : s.resign_role_choice;
                if (cur_idx < 0 || cur_idx > 3) cur_idx = natural_idx;
                ImGui::SetNextItemWidth(220);
                char combo_preview[80];
                std::snprintf(combo_preview, sizeof(combo_preview),
                    "%s%s",
                    vlr::role_name(role_opts[cur_idx]),
                    (cur_idx == natural_idx) ? " (Natural)" : "");
                if (ImGui::BeginCombo("Offered Role", combo_preview)) {
                    for (int i = 0; i < 4; ++i) {
                        char item[96];
                        std::snprintf(item, sizeof(item),
                            "%s  -  %s",
                            vlr::role_name(role_opts[i]),
                            p->role_fit_verdict(role_opts[i]));
                        bool selected = (i == cur_idx);
                        if (ImGui::Selectable(item, selected)) {
                            s.resign_role_choice = i;
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }
            vlr::Role offered_role = p->primary_role;
            if (s.resign_role_choice >= 0 && s.resign_role_choice < 4) {
                const vlr::Role role_opts[4] = {
                    vlr::Role::Duelist, vlr::Role::Initiator,
                    vlr::Role::Controller, vlr::Role::Sentinel
                };
                offered_role = role_opts[s.resign_role_choice];
            }

            // Transparent breakdown — single source of truth with the engine.
            // evaluate_resign_offer is the same function the AI uses to
            // decide acceptance, so the displayed pill never disagrees with
            // what "Send Offer" actually does.
            auto bd = p->evaluate_resign_offer(s.resign_amount_choice,
                                                s.resign_years_choice,
                                                *t, offered_role);

            // Verdict pill — colored by total score band.
            ImU32 verdict_col = kVlrSub;
            if      (bd.total >= 85) verdict_col = kVlrGold;       // OVERPAY
            else if (bd.total >= 70) verdict_col = kVlrGreen;      // STRONG
            else if (bd.total >= 55) verdict_col = kAccent;        // FAIR
            else if (bd.total >= 35) verdict_col = kRoleFlex;      // WEAK (amber)
            else                     verdict_col = kVlrRed;        // INSULTING
            char vbuf[80];
            std::snprintf(vbuf, sizeof(vbuf), "%s  (%d/100)", bd.verdict, bd.total);
            Pill(vbuf, verdict_col);
            ImGui::SameLine();
            if (bd.will_accept) Pill("WILL ACCEPT", kVlrGreen);
            else                Pill("WILL REJECT", kVlrRed);

            // Factor breakdown — small mono list.
            VSpace(4);
            if (g_font_small) ImGui::PushFont(g_font_small);
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
            ImGui::Text("Base interest: %d", bd.base_score);
            ImGui::PopStyleColor();
            for (auto& kv : bd.labels) {
                ImU32 col = (kv.second > 0) ? kVlrGreen
                          : (kv.second < 0) ? kVlrRed
                                            : kVlrSub;
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(col));
                ImGui::Text("  %s: %+d", kv.first.c_str(), kv.second);
                ImGui::PopStyleColor();
            }
            if (g_font_small) ImGui::PopFont();

            // Reject reason (only when relevant).
            if (!bd.will_accept && !bd.reject_reason.empty()) {
                VSpace(4);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRedDim));
                ImGui::TextWrapped("Reason: %s", bd.reject_reason.c_str());
                ImGui::PopStyleColor();
            }

            ImGui::Columns(1);
            ThinDivider(8);

            // === Action buttons =======================================
            // "Send Offer" — primary; commits via Team::resign_player.
            ImGui::PushStyleColor(ImGuiCol_Button,        toV4(kAccent));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(kAccent2));
            if (ImGui::Button("Send Offer", ImVec2(150, 32))) {
                int yrs = s.resign_years_choice;
                int amt = s.resign_amount_choice;
                // Use the role-aware acceptance check via the breakdown
                // (same source of truth — the user-visible verdict is
                // exactly what gets committed).
                bool ok = bd.will_accept;
                if (ok) {
                    bool committed = t->resign_player(p, yrs, amt,
                                                     s.gm.year,
                                                     offered_role);
                    if (committed) {
                        char lb[160];
                        std::snprintf(lb, sizeof(lb),
                            "Re-signed %s for %dY at $%dK/yr.",
                            p->name.c_str(), yrs, amt);
                        s.log.emplace_back(lb);
                        s.show_resign_modal = false;
                        s.resign_target.reset();
                    } else {
                        char lb[160];
                        std::snprintf(lb, sizeof(lb),
                            "Re-sign of %s failed (budget / cap).",
                            p->name.c_str());
                        s.log.emplace_back(lb);
                    }
                } else {
                    char lb[160];
                    std::snprintf(lb, sizeof(lb),
                        "%s refused at $%dK/y for %dY.",
                        p->name.c_str(), amt, yrs);
                    s.log.emplace_back(lb);
                }
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine();

            // "Match Their Ask" — gold quick-fill.
            ImGui::PushStyleColor(ImGuiCol_Button,        toV4(kVlrGold));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(kVlrGold));
            if (ImGui::Button("Match Their Ask", ImVec2(170, 32))) {
                s.resign_years_choice =
                    vlr::clamp_v(offer.years, years_lo, years_hi);
                s.resign_amount_choice =
                    vlr::clamp_v(offer.amount_k, amt_lo, amt_hi);
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine();

            // "Walk Away" — close without action.
            if (ImGui::Button("Walk Away", ImVec2(120, 32))) {
                s.show_resign_modal = false;
                s.resign_target.reset();
            }

            ImGui::EndPopup();
        }
        // Defensive: if user closed via the [x] window button, clear the
        // target pointer + cache so a stale ptr/offer can't survive into
        // next frame OR leak into a fresh open of a different player.
        if (!s.show_resign_modal) {
            s.resign_target.reset();
            s.resign_cached_for = nullptr;
        }
    }
}

void DrawStandings(AppState& s) {
    ImGui::PushFont(g_font_h1);
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
    ImGui::TextUnformatted("LEAGUE STANDINGS");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    VSpace(12);
    ImGui::Columns(static_cast<int>(s.gm.leagues.size()), nullptr, false);
    for (auto& kv : s.gm.leagues) {
        auto& lg = kv.second;
        SectionHeader(lg->name().c_str(), nullptr, kAccent);
        std::vector<vlr::TeamPtr> sorted_t;
        for (auto& t : lg->teams()) if (t) sorted_t.push_back(t);
        std::sort(sorted_t.begin(), sorted_t.end(),
                  [](const vlr::TeamPtr& a, const vlr::TeamPtr& b) { return a->phase_wins > b->phase_wins; });
        if (ImGui::BeginTable(lg->name().c_str(), 5,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_Sortable |
                ImGuiTableFlags_SortMulti)) {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_NoSort);
            ImGui::TableSetupColumn("TEAM", ImGuiTableColumnFlags_WidthStretch, 0, 1);
            ImGui::TableSetupColumn("OVR", ImGuiTableColumnFlags_None, 0, 2);
            ImGui::TableSetupColumn("W",
                ImGuiTableColumnFlags_DefaultSort |
                ImGuiTableColumnFlags_PreferSortDescending, 0, 3);
            ImGui::TableSetupColumn("L", ImGuiTableColumnFlags_None, 0, 4);
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
                ImGui::Text("#%d", rank);
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
                    if (ImGui::Selectable(buf, false, ImGuiSelectableFlags_SpanAllColumns)) {
                        OpenTeamProfile(s, t);
                    }
                    if (is_user) ImGui::PopStyleColor();
                    ImGui::PopID();
                }
                ImGui::TableNextColumn(); ImGui::Text("%.0f", t->ovr());
                ImGui::TableNextColumn(); ImGui::Text("%d", t->phase_wins);
                ImGui::TableNextColumn(); ImGui::Text("%d", t->phase_losses);
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

// Screen-space rect produced by drawing a card. Used by connector lines.
struct BracketCardLayout {
    ImVec2 tl;
    ImVec2 br;
    bool   is_winner_a = false;
    bool   is_winner_b = false;
    bool   played      = false;
    vlr::TeamPtr winner;
};

// Draw a single bracket card.
//   - 2-row layout: team A (top) / team B (bottom)
//   - Winner row tinted with team primary color + bold white tag/name + gold score
//   - Loser row dimmed
//   - Unplayed match: both rows at mid-tier opacity, no scores
//   - BO5 chip drawn in top-right corner when best_of >= 5
BracketCardLayout DrawBracketCardAt(AppState& s, ImVec2 origin,
                                     const vlr::TeamPtr& a, const vlr::TeamPtr& b,
                                     const vlr::TeamPtr& winner,
                                     int a_score, int b_score, bool played,
                                     int best_of,
                                     bool is_lower, bool highlight,
                                     const std::string& event_name,
                                     int unique_id) {
    using namespace bracket_ui;
    BracketCardLayout out;
    out.tl = origin;
    out.br = ImVec2(origin.x + kCardW, origin.y + kCardH);
    out.played = played;
    out.winner = winner;
    out.is_winner_a = (winner && winner == a);
    out.is_winner_b = (winner && winner == b);

    auto* dl = ImGui::GetWindowDrawList();
    ImU32 bg     = played ? kCardBg : kCardBgUnplayed;
    ImU32 border = highlight ? kCardBorderLive : kCardBorder;
    float border_thick = highlight ? 2.0f : 1.0f;
    dl->AddRectFilled(out.tl, out.br, bg, 4.0f);
    dl->AddRect(out.tl, out.br, border, 4.0f, 0, border_thick);

    auto draw_team_row = [&](const vlr::TeamPtr& t, float row_y, bool is_winner,
                              bool is_loser, int score) {
        ImVec2 row_br(out.br.x, row_y + kCardH * 0.5f);
        if (!t) {
            dl->AddText(ImVec2(out.tl.x + 12, row_y + 6),
                        kTextDim, "(TBD)");
            return;
        }
        ImU32 tc = IM_COL32(t->color_primary_r, t->color_primary_g, t->color_primary_b, 0xFF);
        // Team color stripe down the left side of this row.
        dl->AddRectFilled(ImVec2(out.tl.x + 1, row_y + 1),
                          ImVec2(out.tl.x + 1 + kStripeW, row_br.y - 1), tc);
        if (is_winner) {
            // Subtle tinted bg behind winner row.
            dl->AddRectFilled(ImVec2(out.tl.x + 1 + kStripeW, row_y + 1),
                              ImVec2(out.br.x - 1, row_br.y - 1), kRowWin);
        }
        std::string tag = t->tag.empty() ? std::string("???") : t->tag;
        ImU32 row_text_col;
        if (!played)          row_text_col = kTextMid;
        else if (is_winner)   row_text_col = kTextWhite;
        else                  row_text_col = kTextDim;
        dl->AddText(ImVec2(out.tl.x + 12, row_y + 8), row_text_col, tag.c_str());
        // Truncated team name (after tag).
        std::string nm = t->name;
        if (nm.size() > 13) nm = nm.substr(0, 11) + "..";
        dl->AddText(ImVec2(out.tl.x + 50, row_y + 8), row_text_col, nm.c_str());
        // Right-aligned score for played matches.
        if (played) {
            char sb[8]; std::snprintf(sb, sizeof(sb), "%d", score);
            ImVec2 sz = ImGui::CalcTextSize(sb);
            ImU32 score_col = is_winner ? kVlrGold : kTextDim;
            dl->AddText(ImVec2(out.br.x - sz.x - 10, row_y + 8), score_col, sb);
        }
    };

    bool a_loser = played && winner && winner != a;
    bool b_loser = played && winner && winner != b;
    draw_team_row(a, out.tl.y,                   out.is_winner_a, a_loser, a_score);
    draw_team_row(b, out.tl.y + kCardH * 0.5f,   out.is_winner_b, b_loser, b_score);
    // Center divider line between the two team rows.
    dl->AddLine(ImVec2(out.tl.x + 1 + kStripeW, out.tl.y + kCardH * 0.5f),
                ImVec2(out.br.x - 1,            out.tl.y + kCardH * 0.5f),
                kCardDivider, 1.0f);

    // BO5 chip — drawn last so it floats on top. Structural info, not a
    // live-match spoiler (per §8 pitfall 31 — bracket cards may surface
    // best_of since it's match metadata, not in-progress round state).
    if (best_of >= 5) {
        ImVec2 chip_br(out.br.x - 4, out.tl.y + 14);
        ImVec2 chip_tl(chip_br.x - 28, chip_br.y - 12);
        dl->AddRectFilled(chip_tl, chip_br, kVlrGold, 2.0f);
        dl->AddText(ImVec2(chip_tl.x + 4, chip_tl.y - 1),
                    IM_COL32(0x10, 0x10, 0x10, 0xFF), "BO5");
    }
    (void)is_lower;  // borders are uniform now; LB is distinguished by panel header

    // Invisible click target overlay.
    ImGui::PushID(unique_id);
    ImGui::SetCursorScreenPos(ImVec2(out.tl.x, out.tl.y));
    if (ImGui::InvisibleButton("##bcardclk", ImVec2(kCardW, kCardH))) {
        if (a && b && !played) {
            // Use the per-match best_of stamped by the Tournament at
            // scheduling time (Agent A contract). Fallback to 3 if it's 0.
            int bo = (best_of > 0) ? best_of : 3;
            OpenLiveMatchSeries(s, a, b, event_name, bo);
        } else if (winner) {
            OpenTeamProfile(s, winner);
        } else if (a) {
            OpenTeamProfile(s, a);
        }
    }
    if (ImGui::IsItemHovered()) {
        if (a && b) {
            if (played && winner) {
                ImGui::SetTooltip("%s %d - %d %s\nWinner: %s\n(click for team profile)",
                                  a->name.c_str(), a_score, b_score, b->name.c_str(),
                                  winner->name.c_str());
            } else {
                ImGui::SetTooltip("%s vs %s%s\n(click to Watch live)",
                                  a->name.c_str(), b->name.c_str(),
                                  best_of >= 5 ? "  [BO5]" : "");
            }
        }
    }
    ImGui::PopID();
    return out;
}

// Stepped H-V-H connector line between two cards. Routes from the right
// edge of `from` to the left edge of `to`. Colored by whether the source
// match was played (white-ish) or not (muted gray).
void DrawBracketConnector(ImDrawList* dl, const BracketCardLayout& from,
                           const BracketCardLayout& to) {
    float fx = from.br.x;
    float fy = (from.tl.y + from.br.y) * 0.5f;
    float tx = to.tl.x;
    float ty = (to.tl.y + to.br.y) * 0.5f;
    float midx = fx + (tx - fx) * 0.5f;
    // Played connectors that resolved a winner subtly carry the accent so
    // the eye can trace the advancing path; unplayed stays muted.
    bool advanced = from.played && (bool)from.winner;
    float thick = advanced ? 2.2f : 1.6f;
    ImU32 col = !from.played ? bracket_ui::kLineUnplayed
              : advanced     ? kAccent
                             : bracket_ui::kLinePlayed;
    // Smooth H-V-H route: three segments with a small filled dot at each
    // bend so the corners read as rounded joints rather than hard kinks.
    dl->AddLine(ImVec2(fx, fy),   ImVec2(midx, fy), col, thick);
    dl->AddLine(ImVec2(midx, fy), ImVec2(midx, ty), col, thick);
    dl->AddLine(ImVec2(midx, ty), ImVec2(tx, ty),   col, thick);
    dl->AddCircleFilled(ImVec2(midx, fy), thick * 0.9f, col);
    dl->AddCircleFilled(ImVec2(midx, ty), thick * 0.9f, col);
}

// Slot record for one match in the rendered grid. Carries enough info to
// draw the card and route the click handler — best_of is stamped at
// scheduling time by the Tournament (Agent A contract on BracketMatch).
struct RoundSlot {
    vlr::TeamPtr a, b, winner;
    int  a_score   = 0;
    int  b_score   = 0;
    int  best_of   = 3;
    bool played    = false;
    bool placeholder = false;       // future round, no teams known yet
};

// Resolve a per-column label using a passed-in label list (one per
// round). Empty entry → fall back to a count-based generic.
static std::string round_label_at(const std::vector<std::string>& labels,
                                   std::size_t round_idx,
                                   std::size_t teams_in_round,
                                   bool is_lower) {
    if (round_idx < labels.size() && !labels[round_idx].empty()) {
        return labels[round_idx];
    }
    int remaining = static_cast<int>(teams_in_round) * 2;
    if (is_lower) {
        if (teams_in_round <= 1) return "LB FINAL";
        if (teams_in_round == 2) return "LB Semifinal";
        return "LB R" + std::to_string(round_idx + 1);
    }
    if (remaining == 16) return "UB R1";
    if (remaining == 8)  return "UB Quarterfinals";
    if (remaining == 4)  return "UB Semifinals";
    if (remaining == 2)  return "UB Final";
    return "Round " + std::to_string(round_idx + 1);
}

// Render the UB or LB rounds as a horizontal sequence of columns. Returns
// the bottom-Y of the lowest card drawn so the caller can place the next
// section beneath it.
float DrawBracketRounds(AppState& s, const std::vector<std::vector<RoundSlot>>& rounds,
                        const std::vector<std::string>& round_labels,
                        ImVec2 origin, bool is_lower,
                        const std::string& event_name, int id_base,
                        std::vector<BracketCardLayout>* out_finals = nullptr) {
    using namespace bracket_ui;
    auto* dl = ImGui::GetWindowDrawList();
    float content_y0 = origin.y + kRoundLabelH + 4.0f;

    if (rounds.empty()) return content_y0;

    std::vector<std::vector<BracketCardLayout>> rendered;
    rendered.resize(rounds.size());

    for (std::size_t r = 0; r < rounds.size(); ++r) {
        auto& slots = rounds[r];
        float col_x = origin.x + static_cast<float>(r) * (kCardW + kColGap);

        // Column header — H2 font, muted gold for UB, red for LB Final,
        // soft slate for everything else.
        std::string lbl = round_label_at(round_labels, r, slots.size(), is_lower);
        bool is_lb_final = is_lower && (r + 1 == rounds.size()) && slots.size() == 1;
        ImU32 lbl_col = is_lb_final ? kVlrGold
                       : (is_lower ? IM_COL32(0xE0, 0x9C, 0x9C, 0xFF)
                                   : IM_COL32(0xE3, 0xC5, 0x6D, 0xFF));
        if (is_lb_final) {
            std::string upper = lbl;
            for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            // Append "(BO5)" suffix per design spec
            if (upper.find("BO5") == std::string::npos) upper += " (BO5)";
            lbl = upper;
        }
        // SectionHeader-style: a short accent tick precedes the label so
        // each round column reads as its own titled block.
        {
            float lblfs = g_font_h2 ? g_font_h2->FontSize * 0.78f : 16.0f;
            dl->AddRectFilled(ImVec2(col_x, origin.y + 2.0f),
                              ImVec2(col_x + 3.0f, origin.y + lblfs),
                              is_lb_final ? kVlrGold : kAccent, 1.5f);
            if (g_font_h2) {
                dl->AddText(g_font_h2, lblfs,
                            ImVec2(col_x + 10.0f, origin.y), lbl_col, lbl.c_str());
            } else {
                dl->AddText(ImVec2(col_x + 10.0f, origin.y), lbl_col, lbl.c_str());
            }
        }

        // Vertical layout: in round 0, stack at base pitch. In subsequent
        // rounds, place each card at the midpoint of its two feeders. This
        // gives the "branching outward" VCT/NBA shape automatically.
        std::vector<float> card_ys;
        card_ys.reserve(slots.size());
        if (r == 0) {
            for (std::size_t i = 0; i < slots.size(); ++i) {
                card_ys.push_back(content_y0 + i * kBasePitch);
            }
        } else {
            auto& prev = rendered[r - 1];
            for (std::size_t i = 0; i < slots.size(); ++i) {
                std::size_t f1 = std::min(prev.size() - 1, i * 2);
                std::size_t f2 = std::min(prev.size() - 1, i * 2 + 1);
                float y1 = (prev[f1].tl.y + prev[f1].br.y) * 0.5f;
                float y2 = (prev[f2].tl.y + prev[f2].br.y) * 0.5f;
                float mid = (y1 + y2) * 0.5f;
                card_ys.push_back(mid - kCardH * 0.5f);
            }
        }

        for (std::size_t i = 0; i < slots.size(); ++i) {
            auto& sl = slots[i];
            BracketCardLayout layout;
            if (sl.placeholder) {
                ImVec2 tl(col_x, card_ys[i]);
                ImVec2 br(col_x + kCardW, card_ys[i] + kCardH);
                // Crisp empty card — deep fill, hairline border, centered
                // muted "TBD" so future rounds still read as real slots.
                dl->AddRectFilled(tl, br, bracket_ui::kCardBgUnplayed, 5.0f);
                dl->AddRect(tl, br, kCardBorder, 5.0f, 0, 1.0f);
                const char* tbd = "TBD";
                ImVec2 tsz = ImGui::CalcTextSize(tbd);
                dl->AddText(ImVec2((tl.x + br.x) * 0.5f - tsz.x * 0.5f,
                                   (tl.y + br.y) * 0.5f - tsz.y * 0.5f),
                            kTextDim, tbd);
                layout.tl = tl; layout.br = br;
                layout.played = false;
            } else {
                layout = DrawBracketCardAt(
                    s, ImVec2(col_x, card_ys[i]),
                    sl.a, sl.b, sl.winner, sl.a_score, sl.b_score, sl.played,
                    sl.best_of,
                    is_lower, /*highlight=*/(!sl.played && sl.a && sl.b),
                    event_name, id_base + static_cast<int>(r * 100 + i));
            }
            rendered[r].push_back(layout);

            // Connectors from this round back into prev round.
            if (r > 0 && !rendered[r - 1].empty()) {
                std::size_t f1 = std::min(rendered[r - 1].size() - 1, i * 2);
                std::size_t f2 = std::min(rendered[r - 1].size() - 1, i * 2 + 1);
                DrawBracketConnector(dl, rendered[r - 1][f1], layout);
                if (f1 != f2) DrawBracketConnector(dl, rendered[r - 1][f2], layout);
            }
        }
    }

    if (out_finals && !rendered.empty()) {
        *out_finals = rendered.back();
    }

    float bottom_y = content_y0;
    for (auto& rrr : rendered)
        for (auto& c : rrr) bottom_y = std::max(bottom_y, c.br.y);
    return bottom_y;
}

// Convert a Tournament's history + scheduled matchups into a slot grid.
// Future UB rounds (after the latest scheduled) get TBD placeholders so
// the visual bracket extends all the way to the final. LB structure is
// irregular (drop-ins from UB merge unpredictably with LB winners) so we
// do NOT pad LB placeholders — that was a major source of the "glitchy
// lower bracket" complaints.
std::vector<std::vector<RoundSlot>>
build_round_grid(const std::vector<std::vector<vlr::BracketMatch>>& history,
                 const std::vector<std::pair<vlr::TeamPtr, vlr::TeamPtr>>& scheduled,
                 int scheduled_best_of,
                 bool is_lower,
                 int target_total_rounds) {
    std::vector<std::vector<RoundSlot>> out;
    // 1. Played history rounds.
    for (auto& round : history) {
        std::vector<RoundSlot> r;
        for (auto& m : round) {
            RoundSlot s;
            s.a = m.a; s.b = m.b; s.winner = m.winner;
            s.a_score = m.a_score; s.b_score = m.b_score; s.played = m.played;
            s.best_of = m.best_of > 0 ? m.best_of : 3;
            r.push_back(s);
        }
        out.push_back(std::move(r));
    }
    // 2. Currently-scheduled (not-yet-played) round.
    if (!scheduled.empty()) {
        std::vector<RoundSlot> r;
        for (auto& mu : scheduled) {
            RoundSlot s;
            s.a = mu.first; s.b = mu.second; s.played = false;
            s.best_of = scheduled_best_of > 0 ? scheduled_best_of : 3;
            r.push_back(s);
        }
        out.push_back(std::move(r));
    }
    // 3. UB-only: pad with TBD placeholders down to the final.
    if (!is_lower && !out.empty()) {
        std::size_t last = out.back().size();
        int safety = 0;
        while (last > 1 && safety++ < 6) {
            std::size_t next = (last + 1) / 2;
            std::vector<RoundSlot> r;
            for (std::size_t i = 0; i < next; ++i) {
                RoundSlot s; s.placeholder = true; s.best_of = 3;
                r.push_back(s);
            }
            out.push_back(std::move(r));
            last = next;
        }
    }
    // 4. LB: if Agent A's lower_bracket_round_count() is known, pad with
    //    TBD placeholders so the column count matches across renders. We
    //    just append empty rounds carrying a single placeholder each — the
    //    midpoint-of-feeders layout will pick a vertical position even if
    //    the math is approximate. Caps growth at 6 placeholders.
    if (is_lower && target_total_rounds > 0 &&
        static_cast<int>(out.size()) < target_total_rounds) {
        int pad = target_total_rounds - static_cast<int>(out.size());
        if (pad > 6) pad = 6;
        for (int p = 0; p < pad; ++p) {
            std::vector<RoundSlot> r;
            RoundSlot s; s.placeholder = true; s.best_of = 3;
            r.push_back(s);
            out.push_back(std::move(r));
        }
    }
    return out;
}

// Backwards-compat shim: keeps DrawMatchupCard signature for the live-match
// tile rendering on the sidebar / dashboard. Returns true if Watch clicked.
bool DrawMatchupCard(AppState& s, const std::pair<vlr::TeamPtr, vlr::TeamPtr>& mu,
                     const std::string& event_name,
                     bool is_lower_bracket,
                     const std::unordered_set<vlr::Team*>& eliminated_set) {
    bool watch_clicked = false;
    auto draw_team_row = [&](const vlr::TeamPtr& t) {
        if (!t) {
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
            ImGui::Text("  BYE");
            ImGui::PopStyleColor();
            return;
        }
        ImVec2 cur = ImGui::GetCursorScreenPos();
        ImU32 tc = IM_COL32(t->color_primary_r, t->color_primary_g, t->color_primary_b, 0xFF);
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(cur.x, cur.y), ImVec2(cur.x + 4, cur.y + 22), tc, 1.5f);
        ImGui::SetCursorScreenPos(ImVec2(cur.x + 10, cur.y));
        TeamLogo(*t, 20.f);
        ImGui::SameLine();
        CountryFlag(t->home_country_iso);
        ImGui::SameLine();
        if (!t->tag.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(tc));
            ImGui::Text("%s", t->tag.c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine();
        }
        if (ImGui::Selectable(t->name.c_str(), false, 0, ImVec2(130, 0))) {
            OpenTeamProfile(s, t);
        }
    };
    BeginCardSized("##muc", ImVec2(280, 70), kSurface, 8.0f, 8.0f);
    draw_team_row(mu.first);
    draw_team_row(mu.second);
    if (mu.first && mu.second) {
        ImGui::PushFont(g_font_small);
        ImGui::PushStyleColor(ImGuiCol_Button, toV4(kAccent));
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kBgDeep));
        if (ImGui::SmallButton("Watch")) {
            OpenLiveMatch(s, mu.first, mu.second, event_name);
            watch_clicked = true;
        }
        ImGui::PopStyleColor(2);
        ImGui::PopFont();
    }
    EndCardSized();
    (void)eliminated_set;
    (void)is_lower_bracket;
    return watch_clicked;
}

void DrawBrackets(AppState& s) {
    H1("TOURNAMENT BRACKETS", kVlrRed);
    ImGui::Separator();
    if (s.gm.active_tournaments.empty()) {
        ImGui::TextDisabled("No active tournaments. Advance the phase to enter playoffs.");
        return;
    }

    // Group tournaments into tabs. Regional events go in their region tab;
    // international events (MASTERS / CHAMPIONS) go in an "International" tab.
    // This keeps the page navigable instead of a single endless scroll.
    auto tour_tab_for = [](const std::shared_ptr<vlr::Tournament>& t) -> std::string {
        const std::string& n = t->name();
        // International events go in their own tab regardless of word order.
        if (n.find("MASTERS")   != std::string::npos) return "International";
        if (n.find("CHAMPIONS") != std::string::npos) return "International";
        // Match known region names explicitly (rather than splitting on the
        // first space) so multi-word region tags don't get truncated and
        // unknown tournament names don't pollute the tab bar.
        for (auto* known : {"Americas", "EMEA", "Pacific"}) {
            if (n.find(known) != std::string::npos) return known;
        }
        // Unknown / future events land in the International bucket so the tab
        // bar stays compact.
        return "International";
    };

    // Build deterministic tab order: known regions first, then International,
    // then any leftover/unknown.
    std::vector<std::string> tab_order;
    auto push_tab = [&](const std::string& nm) {
        if (std::find(tab_order.begin(), tab_order.end(), nm) == tab_order.end())
            tab_order.push_back(nm);
    };
    for (auto* known : {"Americas", "EMEA", "Pacific"}) push_tab(known);
    push_tab("International");
    for (auto& t : s.gm.active_tournaments) push_tab(tour_tab_for(t));

    if (!ImGui::BeginTabBar("##tour_tabs", ImGuiTabBarFlags_FittingPolicyScroll)) return;
    for (auto& tab_name : tab_order) {
        // Skip tabs with no active tournament (avoids empty tabs).
        bool has_any = false;
        for (auto& t : s.gm.active_tournaments) {
            if (tour_tab_for(t) == tab_name) { has_any = true; break; }
        }
        if (!has_any) continue;
        if (!ImGui::BeginTabItem(tab_name.c_str())) continue;

        int tour_index = 0;
        for (auto& tour : s.gm.active_tournaments) {
            if (tour_tab_for(tour) != tab_name) continue;
        // Scope ImGui IDs to this tournament so the bracket card buttons
        // don't collide with cards in another tournament's bracket
        // (which all share the same id_base offsets internally).
        ImGui::PushID(("##tour_" + std::to_string(tour_index++)).c_str());
        // Tournament title strip
        ImGui::PushFont(g_font_h2);
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
        ImGui::Text("%s", tour->name().c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::SameLine();
        Sub("  |  Round %d  |  %s", tour->round_num(),
            tour->current_round_label().c_str());

        // Per-tournament sub-tabs: "Bracket" (existing view) and
        // "Player Stats" (event-wide aggregate). Lives INSIDE the
        // per-tournament PushID scope so widget IDs stay namespaced.
        if (ImGui::BeginTabBar("##tour_subtabs")) {
        if (ImGui::BeginTabItem("Bracket")) {
        // The existing bracket/group/champion rendering. Wrapped in a
        // lambda so its several early-exit paths can `return` cleanly
        // instead of `continue`-ing past the Player Stats tab below.
        auto render_bracket = [&]() {
        // Champion banner
        if (tour->finished()) {
            ImGui::Spacing();
            {
                ImVec2 cb_p = ImGui::GetCursorScreenPos();
                ImVec2 cb_a = ImGui::GetContentRegionAvail();
                float cb_h = 68.0f;
                ImDrawList* cdl = ImGui::GetWindowDrawList();
                cdl->AddRectFilled(cb_p, ImVec2(cb_p.x + cb_a.x, cb_p.y + cb_h),
                                   kBgDeep, 8.0f);
                cdl->AddRectFilled(cb_p, ImVec2(cb_p.x + cb_a.x, cb_p.y + cb_h),
                                   IM_COL32(0x2A, 0x22, 0x08, 0xFF), 8.0f);
                cdl->AddRect(cb_p, ImVec2(cb_p.x + cb_a.x, cb_p.y + cb_h),
                             kVlrGold, 8.0f, 0, 1.5f);
                cdl->AddRectFilled(cb_p, ImVec2(cb_p.x + 5, cb_p.y + cb_h),
                                   kVlrGold);
            }
            ImGui::BeginChild("##champ_banner", ImVec2(0, 68), ImGuiChildFlags_None);
            ImGui::Dummy(ImVec2(0, 14));
            ImGui::Indent(18.0f);
            ImGui::PushFont(g_font_h1);
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
            ImGui::Text("CHAMPION: %s", tour->champion()->name.c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::Unindent(18.0f);
            ImGui::EndChild();
            ImGui::Spacing();

            // Final placement strip — runner_up_/semifinalists_ are only
            // populated when the tournament reaches Done, so this is
            // final-state only and shown alongside the champion banner.
            if (!tour->eliminated().empty() || !tour->semifinalists().empty()
                || tour->runner_up()) {
                ImGui::PushFont(g_font_small);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
                ImGui::Text("FINAL PLACEMENT:");
                ImGui::PopStyleColor();
                ImGui::PopFont();
                auto pill = [&](const vlr::TeamPtr& t, const char* tag, ImU32 col) {
                    if (!t) return;
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text, toV4(col));
                    ImGui::PushFont(g_font_small);
                    std::string label = std::string(tag) + " " +
                                        (t->tag.empty() ? t->name : t->tag);
                    if (ImGui::SmallButton(label.c_str())) OpenTeamProfile(s, t);
                    ImGui::PopFont();
                    ImGui::PopStyleColor();
                };
                if (tour->champion())
                    pill(tour->champion(), "CHAMP", kVlrGold);
                if (tour->runner_up())
                    pill(tour->runner_up(), "FINAL", IM_COL32(0xC0, 0xC0, 0xC0, 0xFF));
                for (auto& sf : tour->semifinalists())
                    pill(sf, "SF", IM_COL32(0xCD, 0x7F, 0x32, 0xFF));
                std::unordered_set<vlr::Team*> shown;
                if (tour->champion())  shown.insert(tour->champion().get());
                if (tour->runner_up()) shown.insert(tour->runner_up().get());
                for (auto& sf : tour->semifinalists()) if (sf) shown.insert(sf.get());
                for (auto& t : tour->eliminated()) {
                    if (!t || shown.count(t.get())) continue;
                    pill(t, "OUT", kVlrRed);
                }
                ImGui::Spacing();
            }

            return;   // finished tournament — no bracket grid to draw
        }

        // Build elimination set for visual indicators
        std::unordered_set<vlr::Team*> elim_set;
        for (auto& t : tour->eliminated()) if (t) elim_set.insert(t.get());

        // Group stage view
        if (!tour->groups().empty() && !tour->group_stage_complete()) {
            ImGui::Spacing();
            ImGui::PushFont(g_font_h2);
            ImGui::Text("GROUP STAGE");
            ImGui::PopFont();
            char gname[2] = {'A', 0};
            for (auto& g : tour->groups()) {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                ImGui::Text("Group %s", gname);
                ImGui::PopStyleColor();
                ++gname[0];
                if (ImGui::BeginTable(("##grp" + std::to_string(gname[0])).c_str(), 5,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
                    ImGui::TableSetupColumn("TEAM", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("W");
                    ImGui::TableSetupColumn("L");
                    ImGui::TableSetupColumn("MAP DIFF");
                    ImGui::TableSetupColumn("STATUS");
                    ImGui::TableHeadersRow();
                    for (auto& gs : g) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        if (gs.team) {
                            CountryFlag(gs.team->home_country_iso); ImGui::SameLine();
                            if (ImGui::Selectable(gs.team->name.c_str(), false)) {
                                OpenTeamProfile(s, gs.team);
                            }
                        }
                        ImGui::TableNextColumn(); ImGui::Text("%d", gs.wins);
                        ImGui::TableNextColumn(); ImGui::Text("%d", gs.losses);
                        ImGui::TableNextColumn(); ImGui::Text("%+d", gs.map_diff);
                        ImGui::TableNextColumn();
                        if (gs.eliminated) {
                            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
                            ImGui::Text("ELIMINATED");
                            ImGui::PopStyleColor();
                        } else if (gs.wins >= 2) {
                            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGreen));
                            ImGui::Text("ADVANCING");
                            ImGui::PopStyleColor();
                        } else {
                            ImGui::TextDisabled("alive");
                        }
                    }
                    ImGui::EndTable();
                }
            }
            ImGui::Spacing();
            ImGui::Spacing();
            return;  // group stage doesn't show bracket yet
        }

        // === Bracket view (VCT / NBA playoff style — clean redesign) ===
        // Two visually separated panels (UB top, LB bottom) inside a single
        // scrollable canvas. The Grand Final lives in its own column to the
        // right of the UB panel, vertically centered as a single BO5 card
        // (modern VCT format — no bracket reset).
        //
        // Lower bracket is rendered as its own dedicated horizontal strip
        // below the UB panel. We do NOT try to interleave UB drops into the
        // LB column flow — that was the previous design's biggest source of
        // visual glitches (rounds misaligning after every UB elimination).
        ImGui::Spacing();

        // === Empty-bracket guard ===
        // If we somehow landed here with no UB history, no scheduled UB,
        // and no GF (e.g. partial state mid-transition from groups),
        // render a friendly placeholder instead of an empty canvas.
        bool ub_has_data = !tour->ub_history().empty()
            || !tour->current_matchups().empty()
            || !tour->gf_history().empty()
            || tour->in_grand_final();
        if (!ub_has_data) {
            ImGui::TextDisabled("Bracket starts after group stage.");
            return;
        }

        // === Round-label sourcing ===
        // For the latest scheduled round we use Tournament::current_round_label()
        // (Agent A contract — returns labels like "Upper Bracket R1",
        // "Lower Bracket Final", "Grand Final").
        // Historical / placeholder rounds get a count-based fallback inside
        // build_round_grid → round_label_at.
        std::string live_label = tour->current_round_label();
        bool live_in_gf = tour->in_grand_final();

        // Per-match BO for the currently-scheduled UB/LB rounds. The played
        // history's per-match best_of comes off BracketMatch::best_of, but
        // the SCHEDULED round's pairs are raw (TeamPtr, TeamPtr) — Agent A
        // stamps BO3 everywhere except LB Final / GF (BO5).
        // Mirror that here so the live card can render its BO5 chip
        // correctly even before play_round runs.
        auto sched_bo_for = [&](bool is_lower, std::size_t scheduled_count) -> int {
            if (live_in_gf) return 5;  // GF is BO5
            // Lower Bracket Final: scheduled LB count == 1
            if (is_lower && scheduled_count == 1
                && live_label.find("Final") != std::string::npos) return 5;
            return 3;
        };

        // Build UB grid (use scheduled current_matchups ONLY if not in GF
        // phase — in GF the current_matchups are the GF series, which we
        // render in the dedicated GF column).
        std::vector<std::pair<vlr::TeamPtr, vlr::TeamPtr>> ub_sched;
        if (!live_in_gf) ub_sched = tour->current_matchups();
        int ub_sched_bo = sched_bo_for(/*is_lower=*/false, ub_sched.size());

        std::vector<std::pair<vlr::TeamPtr, vlr::TeamPtr>> lb_sched;
        if (!live_in_gf) lb_sched = tour->lower_matchups();
        int lb_sched_bo = sched_bo_for(/*is_lower=*/true, lb_sched.size());

        // Target round counts come from Agent A's new accessors when
        // available; pass them so build_round_grid can extend the visual
        // bracket out to the full depth instead of cutting off at the
        // last played round.
        int ub_target = tour->upper_bracket_round_count();
        int lb_target = tour->lower_bracket_round_count();

        auto ub_grid = build_round_grid(tour->ub_history(), ub_sched,
                                         ub_sched_bo, /*is_lower=*/false,
                                         ub_target);
        auto lb_grid = build_round_grid(tour->lb_history(), lb_sched,
                                         lb_sched_bo, /*is_lower=*/true,
                                         lb_target);

        // === Defensive layout audit (C9) ===
        // UB rounds MUST shrink monotonically (single-elim invariant). If we
        // ever see a wider next round it means history was mis-ordered or
        // double-fired (see pitfall 8: schedule_next_bracket_round
        // snapshot+clear). Surface a small muted warning so user knows the
        // visual layout may misrepresent the actual standings — engine fix
        // belongs in Pack B (Tournament::validate_bracket_state).
        bool bracket_warn = false;
        for (std::size_t i = 1; i < ub_grid.size(); ++i) {
            if (ub_grid[i].size() > ub_grid[i - 1].size()) {
                bracket_warn = true; break;
            }
        }
        if (bracket_warn) {
            MutedText("[bracket layout warning] UB round sizes are not "
                      "monotonically shrinking. Display may misrepresent "
                      "standings.");
        }

        // === Pre-compute per-round labels for both panels ===
        // Played UB rounds: pull from history[i][0].label if present, else
        // empty (count-based fallback inside the column renderer kicks in).
        // The single LATEST scheduled round gets `live_label` patched in.
        auto labels_for = [&](const std::vector<std::vector<vlr::BracketMatch>>& hist,
                              std::size_t scheduled_present,
                              std::size_t total_columns,
                              bool is_lower) -> std::vector<std::string> {
            std::vector<std::string> out_lbl;
            out_lbl.reserve(total_columns);
            for (auto& round : hist) {
                if (!round.empty() && !round.front().label.empty()) {
                    out_lbl.push_back(round.front().label);
                } else {
                    out_lbl.emplace_back();
                }
            }
            if (scheduled_present) {
                // The latest scheduled column gets Tournament's current label,
                // but only if it matches this panel's bracket side. Otherwise
                // leave empty so the count-based fallback names it.
                bool live_is_lower = live_label.find("Lower") != std::string::npos;
                bool live_is_upper = live_label.find("Upper") != std::string::npos;
                if ((is_lower && live_is_lower) || (!is_lower && live_is_upper)) {
                    out_lbl.push_back(live_label);
                } else {
                    out_lbl.emplace_back();
                }
            }
            // Pad to total_columns with empties.
            while (out_lbl.size() < total_columns) out_lbl.emplace_back();
            return out_lbl;
        };

        std::vector<std::string> ub_labels = labels_for(
            tour->ub_history(),
            ub_sched.empty() ? 0u : 1u,
            ub_grid.size(),
            /*is_lower=*/false);
        std::vector<std::string> lb_labels = labels_for(
            tour->lb_history(),
            lb_sched.empty() ? 0u : 1u,
            lb_grid.size(),
            /*is_lower=*/true);

        // === Canvas sizing ===
        // Width = max(UB cols, LB cols) * column + GF column on the right.
        // Height: estimate generously — connectors must not clip even when
        // round-0 has 8 cards (Champions UB R1).
        std::size_t ub_cols = ub_grid.size();
        std::size_t lb_cols = lb_grid.size();
        std::size_t ub_r0   = ub_grid.empty() ? 0u : ub_grid.front().size();
        std::size_t lb_r0   = lb_grid.empty() ? 0u : lb_grid.front().size();
        float ub_height = std::max<float>(200.0f,
            static_cast<float>(ub_r0) * bracket_ui::kBasePitch + 80.0f);
        float lb_height = lb_grid.empty() ? 60.0f
            : std::max<float>(160.0f,
                  static_cast<float>(lb_r0) * bracket_ui::kBasePitch + 80.0f);
        float canvas_h = ub_height + lb_height + 100.0f;
        std::size_t widest_cols = std::max(ub_cols, lb_cols) + 1u;  // +1 for GF
        float canvas_w = std::max<float>(900.0f,
            static_cast<float>(widest_cols)
                * (bracket_ui::kCardW + bracket_ui::kColGap) + 80.0f);

        ImGui::BeginChild("##bracket_canvas", ImVec2(0, canvas_h),
                          ImGuiChildFlags_Border,
                          ImGuiWindowFlags_HorizontalScrollbar);
        ImVec2 cv_origin = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(canvas_w, canvas_h - 16.0f));
        ImGui::SetCursorScreenPos(cv_origin);
        auto* dl = ImGui::GetWindowDrawList();

        // === Panel A: UPPER BRACKET ===
        // Panel-level header (gold, H2) painted with full canvas width so it
        // anchors the visual hierarchy.
        ImVec2 ub_panel_tl(cv_origin.x + 6.0f, cv_origin.y + 4.0f);
        ImVec2 ub_panel_br(cv_origin.x + canvas_w - 6.0f, cv_origin.y + ub_height);
        dl->AddRectFilled(ub_panel_tl, ub_panel_br, bracket_ui::kPanelBg, 6.0f);
        dl->AddRect(ub_panel_tl, ub_panel_br, bracket_ui::kPanelBorder, 6.0f, 0, 1.0f);
        {
            float hfs = g_font_h2 ? g_font_h2->FontSize : 22.0f;
            dl->AddRectFilled(ImVec2(ub_panel_tl.x + 14.0f, ub_panel_tl.y + 7.0f),
                              ImVec2(ub_panel_tl.x + 18.0f, ub_panel_tl.y + 6.0f + hfs),
                              kVlrGold, 2.0f);
            if (g_font_h2) {
                dl->AddText(g_font_h2, hfs,
                            ImVec2(ub_panel_tl.x + 28.0f, ub_panel_tl.y + 6.0f),
                            kVlrGold, "UPPER BRACKET");
            } else {
                dl->AddText(ImVec2(ub_panel_tl.x + 28.0f, ub_panel_tl.y + 6.0f),
                            kVlrGold, "UPPER BRACKET");
            }
        }

        std::vector<BracketCardLayout> ub_finals;
        float ub_rounds_top = ub_panel_tl.y + 40.0f;
        DrawBracketRounds(s, ub_grid, ub_labels,
                          ImVec2(ub_panel_tl.x + bracket_ui::kPanelPad, ub_rounds_top),
                          /*is_lower=*/false,
                          tour->name() + " - UB",
                          /*id_base=*/1000,
                          &ub_finals);

        // === GF column (to the right of UB) ===
        ImVec2 gf_origin(
            ub_panel_tl.x + bracket_ui::kPanelPad
                + static_cast<float>(ub_cols)
                    * (bracket_ui::kCardW + bracket_ui::kColGap),
            ub_rounds_top);

        // Vertically center GF against UB final card (or fall back to mid-panel).
        float gf_y = ub_rounds_top + ub_height * 0.25f;
        if (!ub_finals.empty()) {
            gf_y = (ub_finals.back().tl.y + ub_finals.back().br.y) * 0.5f
                 - bracket_ui::kCardH * 0.5f;
        }

        // GF column header — wider gold treatment so it reads as the apex.
        if (g_font_h2) {
            dl->AddText(g_font_h2, g_font_h2->FontSize * 0.78f,
                        ImVec2(gf_origin.x, gf_y - 28.0f),
                        kVlrGold, "GRAND FINAL");
        } else {
            dl->AddText(ImVec2(gf_origin.x, gf_y - 28.0f),
                        kVlrGold, "GRAND FINAL");
        }

        std::vector<BracketCardLayout> gf_layouts;
        // Modern VCT format: exactly one Grand Final card (BO5, no bracket
        // reset). Render the played GF if it's in history, otherwise render
        // the live scheduled GF card.
        if (!tour->gf_history().empty()) {
            auto& g = tour->gf_history().front();
            BracketCardLayout layout = DrawBracketCardAt(
                s, ImVec2(gf_origin.x, gf_y),
                g.a, g.b, g.winner, g.a_score, g.b_score, g.played,
                g.best_of > 0 ? g.best_of : 5,
                /*is_lower=*/false, /*highlight=*/false,
                tour->name() + " - GF",
                /*unique_id=*/9000);
            gf_layouts.push_back(layout);
            // Mini-label above the card — prefer the stamped BracketMatch.label.
            std::string mini = g.label.empty() ? std::string("Grand Final") : g.label;
            dl->AddText(ImVec2(layout.tl.x, layout.tl.y - 16.0f),
                        bracket_ui::kTextMid, mini.c_str());
        } else if (live_in_gf && !tour->current_matchups().empty()) {
            // Live GF card (when GF is scheduled but not yet played).
            auto& mu = tour->current_matchups().front();
            BracketCardLayout layout = DrawBracketCardAt(
                s, ImVec2(gf_origin.x, gf_y),
                mu.first, mu.second, vlr::TeamPtr{}, 0, 0, /*played=*/false,
                /*best_of=*/5,
                /*is_lower=*/false, /*highlight=*/true,
                tour->name() + " - GF", /*unique_id=*/9100);
            gf_layouts.push_back(layout);
            dl->AddText(ImVec2(layout.tl.x, layout.tl.y - 16.0f),
                        kVlrGold, live_label.c_str());
        }

        // Connector: UB final → GF card. (LB → GF connector drawn after LB.)
        if (!gf_layouts.empty() && !ub_finals.empty()) {
            DrawBracketConnector(dl, ub_finals.back(), gf_layouts.front());
        }

        // === Visual divider between UB and LB panels ===
        float divider_y = ub_panel_br.y + 14.0f;
        dl->AddLine(ImVec2(ub_panel_tl.x + 8.0f, divider_y),
                    ImVec2(ub_panel_br.x - 8.0f, divider_y),
                    bracket_ui::kPanelBorder, 1.0f);

        // === Panel B: LOWER BRACKET ===
        if (!lb_grid.empty()) {
            ImVec2 lb_panel_tl(cv_origin.x + 6.0f, divider_y + 14.0f);
            ImVec2 lb_panel_br(cv_origin.x + canvas_w - 6.0f,
                               lb_panel_tl.y + lb_height);
            dl->AddRectFilled(lb_panel_tl, lb_panel_br, bracket_ui::kPanelBg, 6.0f);
            dl->AddRect(lb_panel_tl, lb_panel_br, bracket_ui::kPanelBorder, 6.0f, 0, 1.0f);
            {
                ImU32 lbc = IM_COL32(0xE0, 0x9C, 0x9C, 0xFF);
                float hfs = g_font_h2 ? g_font_h2->FontSize : 22.0f;
                dl->AddRectFilled(ImVec2(lb_panel_tl.x + 14.0f, lb_panel_tl.y + 7.0f),
                                  ImVec2(lb_panel_tl.x + 18.0f, lb_panel_tl.y + 6.0f + hfs),
                                  kVlrRed, 2.0f);
                if (g_font_h2) {
                    dl->AddText(g_font_h2, hfs,
                                ImVec2(lb_panel_tl.x + 28.0f, lb_panel_tl.y + 6.0f),
                                lbc, "LOWER BRACKET");
                } else {
                    dl->AddText(ImVec2(lb_panel_tl.x + 28.0f, lb_panel_tl.y + 6.0f),
                                lbc, "LOWER BRACKET");
                }
            }
            // Caption explaining UB-loser inflow.
            if (g_font_small) {
                dl->AddText(g_font_small, g_font_small->FontSize,
                            ImVec2(lb_panel_tl.x + 220.0f, lb_panel_tl.y + 11.0f),
                            bracket_ui::kTextDim, "(UB losers drop in here)");
            }

            std::vector<BracketCardLayout> lb_finals;
            DrawBracketRounds(s, lb_grid, lb_labels,
                              ImVec2(lb_panel_tl.x + bracket_ui::kPanelPad,
                                     lb_panel_tl.y + 40.0f),
                              /*is_lower=*/true,
                              tour->name() + " - LB",
                              /*id_base=*/2000,
                              &lb_finals);

            // LB final → GF connector. Draw it long-form across the panel
            // gap (the connector helper handles any vertical delta).
            if (!lb_finals.empty() && !gf_layouts.empty()) {
                DrawBracketConnector(dl, lb_finals.back(), gf_layouts.front());
            }
        }
        ImGui::EndChild();

        // (Final-placement strip is rendered above in the finished/champion
        // branch — runner_up_/semifinalists_ are populated only at Done, so
        // there is nothing meaningful to show during active brackets.)
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        };   // end render_bracket lambda
        render_bracket();
        ImGui::EndTabItem();
        }   // end "Bracket" tab item

        // === Player Stats sub-tab ==================================
        // Event-wide aggregate (groups + playoffs accumulated by the
        // engine accessor). Sortable table; default sort = Rating desc.
        if (ImGui::BeginTabItem("Player Stats")) {
            // Persistent per-tournament copy so sort order survives
            // frames. Previously the cache was overwritten unconditionally
            // every frame and re-sorted ONLY when SpecsDirty fired — so
            // Pull fresh aggregated stats EVERY frame so per-map
            // accumulation (more kills/maps/rating) propagates immediately
            // as the tournament progresses. The previous version-keyed
            // cache (row_count + last player ptr) caught "roster changed"
            // but NOT "existing players played more maps", so stats froze
            // after the first map. aggregate_player_stats() is O(players)
            // (~16 per bracket) — trivial cost at 60fps. Sort spec is
            // re-applied unconditionally below so the user's chosen
            // column order survives across the per-frame rebuild.
            auto rows = tour->aggregate_player_stats();
            if (rows.empty()) {
                ImGui::Spacing();
                float availw = ImGui::GetContentRegionAvail().x;
                const char* msg = "Stats available once the event begins.";
                ImGui::SetCursorPosX(ImGui::GetCursorPosX()
                    + std::max(0.0f,
                        (availw - ImGui::CalcTextSize(msg).x) * 0.5f));
                MutedText("%s", msg);
            } else {
                enum PsCol { P_NAME, P_TEAM, P_AGENT, P_MAPS, P_K, P_D,
                             P_A, P_KD, P_RAT, P_FK, P_FD, P_RND, P_HS,
                             P_ADR, P_KAST, P_CLU, P_NCOL };
                if (ImGui::BeginTable("##tour_pstats", P_NCOL,
                        ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti |
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                        ImGuiTableFlags_BordersInnerV |
                        ImGuiTableFlags_SizingStretchProp,
                        ImVec2(0, 460))) {
                    auto desc = ImGuiTableColumnFlags_PreferSortDescending;
                    ImGui::TableSetupScrollFreeze(1, 1);
                    ImGui::TableSetupColumn("Player",
                        ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Team",
                        ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Agent",
                        ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Maps", desc);
                    ImGui::TableSetupColumn("K", desc);
                    ImGui::TableSetupColumn("D", desc);
                    ImGui::TableSetupColumn("A", desc);
                    ImGui::TableSetupColumn("KD", desc);
                    ImGui::TableSetupColumn("Rating",
                        desc | ImGuiTableColumnFlags_DefaultSort);
                    ImGui::TableSetupColumn("FK", desc);
                    ImGui::TableSetupColumn("FD", desc);
                    ImGui::TableSetupColumn("Rounds", desc);
                    ImGui::TableSetupColumn("HS%", desc);
                    ImGui::TableSetupColumn("ADR", desc);
                    ImGui::TableSetupColumn("KAST", desc);
                    ImGui::TableSetupColumn("Clutches", desc);
                    ImGui::TableHeadersRow();

                    auto kd_of = [](const vlr::TournamentPlayerStat& r) {
                        return (double)r.kills /
                               (double)std::max(1, r.deaths);
                    };
                    if (ImGuiTableSortSpecs* sp =
                            ImGui::TableGetSortSpecs()) {
                        // rows is rebuilt every frame, so we must re-apply
                        // the active sort every frame too. Drop the dirty
                        // gate.
                        if (sp->SpecsCount > 0) {
                            std::stable_sort(rows.begin(), rows.end(),
                              [&](const vlr::TournamentPlayerStat& a,
                                  const vlr::TournamentPlayerStat& b) {
                                for (int i = 0; i < sp->SpecsCount; ++i) {
                                    const auto& sc = sp->Specs[i];
                                    bool asc = (sc.SortDirection ==
                                        ImGuiSortDirection_Ascending);
                                    double va = 0, vb = 0; int cmp = 0;
                                    switch (sc.ColumnIndex) {
                                    case P_NAME:
                                        cmp = a.player->name.compare(
                                              b.player->name); break;
                                    case P_TEAM:
                                        cmp = a.team_name.compare(
                                              b.team_name); break;
                                    case P_AGENT:
                                        cmp = a.top_agent.compare(
                                              b.top_agent); break;
                                    case P_MAPS: va=a.maps; vb=b.maps; break;
                                    case P_K: va=a.kills; vb=b.kills; break;
                                    case P_D: va=a.deaths; vb=b.deaths; break;
                                    case P_A: va=a.assists; vb=b.assists; break;
                                    case P_KD: va=kd_of(a); vb=kd_of(b); break;
                                    case P_RAT: va=a.rating; vb=b.rating; break;
                                    case P_FK: va=a.first_kills;
                                               vb=b.first_kills; break;
                                    case P_FD: va=a.first_deaths;
                                               vb=b.first_deaths; break;
                                    case P_RND: va=a.rounds;
                                                vb=b.rounds; break;
                                    case P_HS: va=a.hs_pct;
                                               vb=b.hs_pct; break;
                                    case P_ADR: va=a.adr; vb=b.adr; break;
                                    case P_KAST: va=a.kast;
                                                 vb=b.kast; break;
                                    case P_CLU: va=a.clutches;
                                                vb=b.clutches; break;
                                    default: break;
                                    }
                                    if (cmp == 0 && va != vb)
                                        cmp = (va < vb) ? -1 : 1;
                                    if (cmp != 0)
                                        return asc ? (cmp<0) : (cmp>0);
                                }
                                return a.player->name < b.player->name;
                              });
                            sp->SpecsDirty = false;
                        }
                    }

                    for (auto& r : rows) {
                        if (!r.player) continue;
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        if (ImGui::Selectable(
                                player_label(*r.player).c_str(), false,
                                ImGuiSelectableFlags_SpanAllColumns)) {
                            // Resolve a PlayerPtr for the modal by scanning
                            // league rosters (OpenPlayerModal needs a
                            // shared_ptr; the accessor only gives Player*).
                            vlr::PlayerPtr found;
                            for (auto& kv : s.gm.leagues) {
                                if (!kv.second) continue;
                                for (auto& tt : kv.second->teams()) {
                                    if (!tt) continue;
                                    for (auto& pp : tt->roster)
                                        if (pp.get() == r.player) {
                                            found = pp; break;
                                        }
                                    if (found) break;
                                }
                                if (found) break;
                            }
                            if (found) OpenPlayerModal(s, found);
                        }
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(r.team_name.c_str());
                        ImGui::TableNextColumn();
                        if (r.top_agent.empty())
                            ImGui::TextDisabled("\xE2\x80\x94");
                        else
                            ImGui::TextUnformatted(r.top_agent.c_str());
                        ImGui::PushFont(g_font_mono);
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", r.maps);
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", r.kills);
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", r.deaths);
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", r.assists);
                        double kd = kd_of(r);
                        ImGui::TableNextColumn();
                        ImGui::PushStyleColor(ImGuiCol_Text, toV4(
                            kd >= 1.1 ? kVlrGreen
                            : (kd < 0.9 ? kVlrRed : kVlrText)));
                        ImGui::Text("%.2f", kd);
                        ImGui::PopStyleColor();
                        ImGui::TableNextColumn();
                        ImGui::PushStyleColor(ImGuiCol_Text, toV4(
                            r.rating >= 1.1 ? kVlrGreen
                            : (r.rating < 0.9 ? kVlrRed : kVlrText)));
                        ImGui::Text("%.2f", r.rating);
                        ImGui::PopStyleColor();
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", r.first_kills);
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", r.first_deaths);
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", r.rounds);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.0f%%", r.hs_pct);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.0f", r.adr);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.0f%%", r.kast);
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", r.clutches);
                        ImGui::PopFont();
                    }
                    ImGui::EndTable();
                }
            }
            ImGui::EndTabItem();
        }   // end "Player Stats" tab item
        ImGui::EndTabBar();
        }   // end per-tournament sub-tab bar

        ImGui::PopID();   // close per-tournament ID scope
        }   // end inner per-tournament for-loop
        ImGui::EndTabItem();
    }   // end tab-order for-loop
    ImGui::EndTabBar();
}

void DrawMarket(AppState& s) {
    auto& gm = s.gm;
    SectionHeader("TRANSFER MARKET",
        "Free agents' asking-prices reflect mood; offer too low and they refuse.",
        kAccent);
    VSpace(6);

    // === OFFSEASON banner (C10) ===
    // FA price-decay runs internally during OFFSEASON ticks (Pack B). Surface
    // a small header banner so users understand WHY ask-prices drift down
    // each day they hold. No engine state needed — just phase-gated text.
    bool offseason_now = (gm.current_phase() == "OFFSEASON");
    if (offseason_now) {
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
        ImGui::TextUnformatted("[*] OFFSEASON");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        MutedText("\xE2\x80\x94 Free Agent prices decay daily during this "
                  "window. Hold out, save money.");
        VSpace(4);
    }

    ImGui::SetNextItemWidth(260);
    ImGui::InputTextWithHint("##search", "Search name...", s.search, sizeof(s.search));
    VSpace(6);

    // FA listing — per PROJECT_GUIDE §5.11, recomputed every frame from
    // every regional solo Q ladder, filtered ONLY by `team_name == "Free
    // Agent"` (and !is_retired). No role / position filter is applied
    // here: Flex is an INDEPENDENT overlay flag (Player::is_flex) and
    // every FA — including Flex players — has a canonical primary_role
    // that resolves to one of the 4 Position values via position_of().
    //
    // Earlier round of code bucketed Flex players into their own
    // `Position::Flex` group; when Position::Flex was removed and
    // position_of() became pure-primary_role, any code path that filtered
    // FAs by Position would have dropped Flex players entirely. This
    // unfiltered loop is intentional — DO NOT re-add a Position-keyed
    // filter unless you also accept Flex players via the is_flex flag.
    std::vector<vlr::PlayerPtr> fas;
    for (auto& kv : gm.solo_qs) {
        for (auto& p : kv.second->global_ladder()) {
            if (p->team_name == "Free Agent" && !p->is_retired) fas.push_back(p);
        }
    }
    std::sort(fas.begin(), fas.end(),
              [](const vlr::PlayerPtr& a, const vlr::PlayerPtr& b) { return a->ovr() > b->ovr(); });

    BeginCard("##market_card");
    if (ImGui::BeginTable("##market", 10,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY,
            ImVec2(-FLT_MIN, 540))) {
        ImGui::TableSetupColumn("FLAG");
        ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("ROLE");
        ImGui::TableSetupColumn("REGION");
        ImGui::TableSetupColumn("AGE");
        ImGui::TableSetupColumn("OVR");
        ImGui::TableSetupColumn("POT");
        ImGui::TableSetupColumn("ASK $K/Y");
        ImGui::TableSetupColumn("MOOD");
        ImGui::TableSetupColumn("SIGN");
        ImGui::TableHeadersRow();

        std::string q = s.search;
        for (auto& cc : q) cc = (char)std::tolower(cc);

        int shown = 0; int idx = 0;
        for (auto& p : fas) {
            if (!q.empty()) {
                std::string nm = p->name;
                for (auto& cc : nm) cc = (char)std::tolower(cc);
                if (nm.find(q) == std::string::npos) continue;
            }
            if (++shown > 200) break;
            ImGui::PushID(idx++);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); CountryFlag(p->country_iso);
            ImGui::TableNextColumn();
            if (ImGui::Selectable(player_label(*p).c_str(), false,
                    ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                OpenPlayerModal(s, p);
            }
            // Small Flex chip on the gamertag column — surfaces is_flex
            // FAs at a glance so the user can spot rotation pieces in the
            // listing without opening the profile modal.
            if (p->is_flex) { ImGui::SameLine(); FlexBadge(); }
            ImGui::TableNextColumn();
            PositionBadge(vlr::position_of(*p));
            if (p->is_flex) { ImGui::SameLine(); FlexBadge(); }
            if (p->is_igl)  { ImGui::SameLine(); IglBadge();  }
            ImGui::TableNextColumn(); ImGui::Text("%s", p->region.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%d", p->age);
            ImGui::TableNextColumn(); ImGui::Text("%.0f", p->ovr());
            ImGui::TableNextColumn(); ImGui::Text("%d", p->potential);
            int demand = gm.user_team
                ? p->amount_with_mood(p->contract.amount_k, gm.user_team->name)
                : p->contract.amount_k;
            ImGui::TableNextColumn();
            ImGui::Text("$%dK", demand);
            // FA decay chip (C10): in OFFSEASON, if the current raw ask is
            // appreciably below the player's value baseline (0.55*ovr +
            // 0.45*potential rounded into $K), show a small accent chip so
            // the user can spot bargains as they appear. Heuristic — exact
            // baseline isn't exposed by Pack B (decay happens internally).
            if (offseason_now) {
                int baseline = static_cast<int>(
                    0.55 * p->ovr() + 0.45 * p->potential);
                if (baseline > 0
                    && p->contract.amount_k < (baseline - 5)) {
                    ImGui::SameLine();
                    Pill("decayed", kAccent);
                }
            }
            double m = gm.user_team ? p->mood_for(gm.user_team->name) : 0.0;
            ImGui::TableNextColumn();
            {
                ImU32 c = m > 0.6 ? kVlrRed : (m > 0.3 ? kVlrGold : kVlrGreen);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(c));
                ImGui::Text("%.0f%%", m * 100.0);
                ImGui::PopStyleColor();
            }
            ImGui::TableNextColumn();
            // Surface roster-full state explicitly so the user understands
            // why the Sign button is missing rather than the row simply
            // looking different.
            if (!gm.user_team) {
                ImGui::TextDisabled("—");
            } else if (gm.user_team->roster.size() >= 7) {
                ImGui::TextDisabled("ROSTER FULL");
            } else if (ImGui::SetNextItemAllowOverlap(), ImGui::SmallButton("Negotiate")) {
                // 2026-05-28: replaced direct sign with the negotiation
                // modal so FA signings have the same interactive depth as
                // re-signings (salary, length, role offer, breakdown).
                // Pre-flight validation now lives inside the modal's
                // Send Offer commit path.
                s.show_fa_modal      = true;
                s.fa_target          = p;
                s.fa_years_choice    = 0;   // 0 sentinel -> fill from cached offer
                s.fa_amount_choice   = 0;
                s.fa_role_choice     = -1;  // -1 -> natural
                s.fa_cached_for      = nullptr;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    EndCard();

    // === FA Sign negotiation modal (2026-05-28) =========================
    // Mirrors the Re-sign modal layout: player's ask + willingness on the
    // left, market + sliders + role combo + breakdown on the right.
    // Uses the same evaluate_resign_offer machinery (Player.cpp) but
    // calls Team::sign_player(p, years, year, role) on commit.
    if (s.show_fa_modal && s.fa_target && gm.user_team) {
        auto p = s.fa_target;
        auto ut = gm.user_team;
        ImGui::OpenPopup("Sign Free Agent##fa_modal");
        ImVec2 rc = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(rc, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(860, 560), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("Sign Free Agent##fa_modal",
                                   &s.show_fa_modal,
                                   ImGuiWindowFlags_NoSavedSettings)) {
            if (s.fa_cached_for != p.get()) {
                s.fa_cached_offer = p->propose_resign_offer(*ut, gm.year);
                s.fa_cached_mkt_k = ut->market_value_estimate(*p);
                s.fa_cached_for   = p.get();
                if (s.fa_years_choice  <= 0) s.fa_years_choice  = s.fa_cached_offer.years;
                if (s.fa_amount_choice <= 0) s.fa_amount_choice = s.fa_cached_offer.amount_k;
            }
            const auto& offer = s.fa_cached_offer;
            int mkt_k = s.fa_cached_mkt_k;

            int years_lo = 1;
            int years_hi = std::max(1, offer.max_acceptable_years);
            int amt_lo   = std::max((int)vlr::kSalaryFloorK,
                                    offer.min_acceptable_k);
            int amt_hi   = std::max(amt_lo + 1,
                                    (int)std::min<long long>(
                                        vlr::kSalaryCapK,
                                        (long long)std::lround(offer.amount_k * 1.6)));
            s.fa_years_choice  = vlr::clamp_v(s.fa_years_choice,  years_lo, years_hi);
            s.fa_amount_choice = vlr::clamp_v(s.fa_amount_choice, amt_lo, amt_hi);

            {
                std::string title = std::string("SIGN ") + p->name;
                H2(title.c_str());
            }
            {
                char tag[80];
                std::snprintf(tag, sizeof(tag),
                    "%d | %s | %s | OVR %.0f",
                    p->age, vlr::role_name(p->primary_role),
                    p->region.c_str(), p->ovr());
                Pill(tag, kVlrSub);
                if (p->region != ut->region) {
                    ImGui::SameLine();
                    Pill("IMPORT", kVlrGold);
                }
            }
            ThinDivider(6);

            ImGui::Columns(2, "##fa_cols", false);

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
            VSpace(4);
            MutedText("Preferred role: %s", vlr::role_name(p->primary_role));

            // RIGHT: market + your offer
            ImGui::NextColumn();
            SectionHeader("MARKET & YOUR OFFER", nullptr, kAccent);
            ImGui::Text("Market value: $%dK/yr", mkt_k);
            ImGui::Text("Team budget: %s", fmt_money(ut->budget).c_str());
            VSpace(4);

            ImGui::SetNextItemWidth(220);
            ImGui::SliderInt("Years", &s.fa_years_choice, years_lo, years_hi);
            ImGui::SetNextItemWidth(220);
            ImGui::SliderInt("Salary $K/yr", &s.fa_amount_choice, amt_lo, amt_hi);

            // Role offer combo — same UX as Re-sign modal.
            {
                const vlr::Role role_opts[4] = {
                    vlr::Role::Duelist, vlr::Role::Initiator,
                    vlr::Role::Controller, vlr::Role::Sentinel
                };
                int natural_idx = 0;
                for (int i = 0; i < 4; ++i)
                    if (role_opts[i] == p->primary_role) natural_idx = i;
                int cur_idx = (s.fa_role_choice < 0) ? natural_idx : s.fa_role_choice;
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
                            s.fa_role_choice = i;
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }
            vlr::Role fa_offered_role = p->primary_role;
            if (s.fa_role_choice >= 0 && s.fa_role_choice < 4) {
                const vlr::Role role_opts[4] = {
                    vlr::Role::Duelist, vlr::Role::Initiator,
                    vlr::Role::Controller, vlr::Role::Sentinel
                };
                fa_offered_role = role_opts[s.fa_role_choice];
            }

            // Breakdown — same evaluator as Re-sign for parity.
            auto bd = p->evaluate_resign_offer(s.fa_amount_choice,
                                               s.fa_years_choice,
                                               *ut, fa_offered_role);
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

            ImGui::Columns(1);
            ThinDivider(8);

            // === Action buttons =======================================
            ImGui::PushStyleColor(ImGuiCol_Button,        toV4(kAccent));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(kAccent2));
            if (ImGui::Button("Send Offer", ImVec2(150, 32))) {
                int yrs  = s.fa_years_choice;
                int amt  = s.fa_amount_choice;
                long long cost = static_cast<long long>(amt) * 1000LL;
                // Pre-flight gates (unchanged from legacy direct sign).
                bool ok = bd.will_accept;
                std::string reject;
                if (ok && ut->budget < cost) {
                    ok = false;
                    reject = "Insufficient budget ($" +
                             std::to_string(ut->budget / 1000) +
                             "K available, $" + std::to_string(amt) + "K offered).";
                }
                if (ok && p->region != ut->region) {
                    int imports = 0;
                    for (auto& rp : ut->roster)
                        if (rp && rp->region != ut->region) ++imports;
                    if (imports >= vlr::config().max_imports) {
                        ok = false;
                        reject = "Import cap reached (" +
                                 std::to_string(vlr::config().max_imports) + ").";
                    }
                }
                if (ok && ut->roster.size() >= 7) {
                    ok = false; reject = "Roster full.";
                }
                if (!ok) {
                    s.log.emplace_back(p->name + ": " +
                        (reject.empty() ? std::string("Offer rejected.") : reject));
                } else {
                    std::size_t before_size = ut->roster.size();
                    ut->budget -= cost;
                    p->contract.amount_k = amt;
                    ut->sign_player(p, yrs, gm.year, fa_offered_role);
                    bool added = (ut->roster.size() == before_size + 1)
                              && (p->team_name == ut->name);
                    if (!added) {
                        ut->budget += cost;
                        s.log.emplace_back("Engine rejected sign of " + p->name + ".");
                    } else {
                        gm.sync_player_ranked_region(p, ut->region);
                        char msg[256];
                        std::snprintf(msg, sizeof(msg),
                            "Signed %s as %s (%dy / $%dK/yr).",
                            p->name.c_str(),
                            vlr::role_name(fa_offered_role), yrs, amt);
                        s.log.emplace_back(msg);
                        s.show_fa_modal = false;
                        s.fa_target.reset();
                    }
                }
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button,        toV4(kVlrGold));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(kVlrGold));
            if (ImGui::Button("Match Their Ask", ImVec2(170, 32))) {
                s.fa_years_choice  = vlr::clamp_v(offer.years, years_lo, years_hi);
                s.fa_amount_choice = vlr::clamp_v(offer.amount_k, amt_lo, amt_hi);
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine();

            if (ImGui::Button("Walk Away", ImVec2(120, 32))) {
                s.show_fa_modal = false;
                s.fa_target.reset();
            }

            ImGui::EndPopup();
        }
        if (!s.show_fa_modal) {
            s.fa_target.reset();
            s.fa_cached_for = nullptr;
        }
    }
}

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
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
            ImVec2(-FLT_MIN, 560))) {
        ImGui::TableSetupColumn("#");
        ImGui::TableSetupColumn("FLAG");
        ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("RANK");
        ImGui::TableSetupColumn("MMR");
        ImGui::TableSetupColumn("OVR");
        ImGui::TableSetupColumn("ROLE");
        ImGui::TableSetupColumn("W");
        ImGui::TableSetupColumn("L");
        ImGui::TableSetupColumn("K/D");
        ImGui::TableHeadersRow();
        int rank = 1, idx = 0;
        for (auto& p : board) {
            if (p->solo_mmr < s.soloq_min_mmr) { ++rank; continue; }
            if (++idx > 250) break;
            ImGui::PushID(rank);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(placement_color(rank)));
            ImGui::Text("#%d", rank);
            ImGui::PopStyleColor();
            ImGui::TableNextColumn(); CountryFlag(p->country_iso);
            ImGui::TableNextColumn();
            if (ImGui::Selectable(player_label(*p).c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
                OpenPlayerModal(s, p);
            }
            ImGui::TableNextColumn(); ImGui::Text("%s", p->rank_name().c_str());
            ImGui::TableNextColumn(); ImGui::Text("%d", p->solo_mmr);
            ImGui::TableNextColumn(); ImGui::Text("%.0f", p->ovr());
            ImGui::TableNextColumn();
            PositionBadge(vlr::position_of(*p));
            if (p->is_igl) { ImGui::SameLine(); IglBadge(); }
            ImGui::TableNextColumn(); ImGui::Text("%d", p->solo_wins);
            ImGui::TableNextColumn(); ImGui::Text("%d", p->solo_losses);
            ImGui::TableNextColumn(); ImGui::Text("%.2f", p->solo_kd());
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
        ImGui::SetNextItemWidth(120); ImGui::DragInt("Salary $K", &c->salary_k, 1.0f, 15, 999);
    }
    } EndCard();

    VSpace(14);
    if (BeginCard("##coach_derived")) {
    SectionHeader("DERIVED EFFECTS", nullptr, kAccent);
    ImGui::Text("Match synergy boost: +%.1f%%", (c->match_synergy_mult() - 1.0) * 100.0);
    ImGui::Text("Annual dev chance:   %.2fx",   c->dev_chance_mult());
    ImGui::Text("Asking salary now:   $%dK/yr", c->requested_salary_k());
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
            if (ImGui::Selectable(p->name.c_str(), false,
                    ImGuiSelectableFlags_SpanAllColumns)) {
                OpenPlayerModal(s, p);
            }
            ImGui::TableNextColumn(); ImGui::Text("%s", p->region.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%d", p->career_matches);
            ImGui::TableNextColumn(); ImGui::Text("%.2f", p->avg_match_rating());
            ImGui::TableNextColumn(); ImGui::Text("%zu", p->awards.size());
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
void DrawTeamProfile(AppState& s) {
    if (!s.selected_team) {
        ImGui::TextDisabled("(no team selected)");
        return;
    }
    auto t = s.selected_team;

    // Back button so the user can return to where they came from.
    if (ImGui::Button("< Back", ImVec2(80, 28))) {
        s.screen = s.prev_screen_before_team_profile;
        s.selected_team.reset();
        return;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|  Team profile");

    ImGui::Spacing();

    // === Banner =========================================================
    // Phase 2: large 96px TeamLogo on the left, replacing the prior
    // tag-square. CountryFlag now sits between the logo and the name.
    ImGui::BeginChild("##team_banner", ImVec2(0, 140), ImGuiChildFlags_Border);
    ImGui::SetCursorPos(ImVec2(16, 12));
    TeamLogo(*t, 96.f);
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(12, 0));
    ImGui::SameLine();
    ImGui::BeginGroup();
    CountryFlag(t->home_country_iso);
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(6, 0));
    ImGui::SameLine();
    ImGui::PushFont(g_font_h1);
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
    ImGui::TextUnformatted(t->name.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::PushFont(g_font_small);
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
    ImGui::Text("VCT %s   |   Based in %s, %s   |   Payroll $%dK/yr   |   Roster OVR %.0f",
                t->region.c_str(),
                t->home_city.empty() ? "?" : t->home_city.c_str(),
                t->home_country.empty() ? "?" : t->home_country.c_str(),
                t->total_payroll_k(),
                t->ovr());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 4));
    Pill(vlr::identity_name(t->comp_identity), kAccent2);
    ImGui::SameLine();
    {
        const char* pers = "Balanced";
        switch (t->personality) {
            case vlr::Personality::Aggressive: pers = "Aggressive"; break;
            case vlr::Personality::Tactical:   pers = "Tactical";   break;
            case vlr::Personality::Budget:     pers = "Budget";     break;
            case vlr::Personality::Balanced:   pers = "Balanced";   break;
        }
        PillOutline(pers, kBorderStrong, kVlrSub);
    }
    ImGui::EndGroup();
    ImGui::EndChild();
    ImGui::Spacing();

    // === KPI row =========================================================
    int rank = 1;
    auto lit = s.gm.leagues.find(t->region);
    if (lit != s.gm.leagues.end()) {
        std::vector<vlr::TeamPtr> sorted_t;
        for (auto& tt : lit->second->teams()) if (tt) sorted_t.push_back(tt);
        std::sort(sorted_t.begin(), sorted_t.end(),
                  [](const vlr::TeamPtr& a, const vlr::TeamPtr& b) { return a->phase_wins > b->phase_wins; });
        for (auto& tt : sorted_t) { if (tt == t) break; ++rank; }
    }

    {
        char b[32];
        std::snprintf(b, sizeof(b), "%dW-%dL", t->phase_wins, t->phase_losses);
        StatTile("PHASE RECORD", b, kAccent);
    }
    ImGui::SameLine();
    {
        char b[32]; std::snprintf(b, sizeof(b), "#%d", rank);
        StatTile("REGIONAL RANK", b, kVlrGold);
    }
    ImGui::SameLine();
    {
        char b[32]; std::snprintf(b, sizeof(b), "%d-%d", t->wins, t->losses);
        StatTile("SEASON RECORD", b, kAccent);
    }
    ImGui::SameLine();
    {
        char b[32]; std::snprintf(b, sizeof(b), "%.0f", t->ovr());
        StatTile("ROSTER OVR", b, kAccent2);
    }
    ImGui::SameLine();
    StatTile("BUDGET", fmt_money(t->budget), kVlrGreen);

    ImGui::Spacing();
    ImGui::Spacing();

    // === Tabs: Overview / Recent / Upcoming / Maps / Bio =================
    if (ImGui::BeginTabBar("##teamtabs")) {
        if (ImGui::BeginTabItem("Roster")) {
            // === 4 canonical + 1 flexible-5th display layout ============
            // Mirror DrawRoster: walk roster[0..4], first occupant per
            // canonical position locks that slot, the leftover starter
            // takes the "5th / Flex" slot. Everything past roster[4] (or
            // displaced duplicates) drops to the bench.
            std::array<vlr::PlayerPtr,
                       (size_t)vlr::Position::Count> tp_canonical{};
            vlr::PlayerPtr tp_fifth;
            std::vector<vlr::PlayerPtr> tp_bench;
            {
                std::size_t starter_count =
                    std::min<std::size_t>(t->roster.size(), 5);
                std::vector<vlr::PlayerPtr> displaced;
                for (std::size_t i = 0; i < starter_count; ++i) {
                    auto& pp = t->roster[i];
                    if (!pp) continue;
                    std::size_t idx = (std::size_t)vlr::position_of(*pp);
                    if (!tp_canonical[idx])      tp_canonical[idx] = pp;
                    else if (!tp_fifth)          tp_fifth = pp;
                    else                         displaced.push_back(pp);
                }
                for (std::size_t i = starter_count; i < t->roster.size(); ++i)
                    if (t->roster[i]) tp_bench.push_back(t->roster[i]);
                for (auto& d : displaced) tp_bench.push_back(d);
                std::sort(tp_bench.begin(), tp_bench.end(),
                          [](const vlr::PlayerPtr& a, const vlr::PlayerPtr& b) {
                              return a->ovr() > b->ovr();
                          });
            }

            auto tp_row = [&](vlr::Position slot,
                              const vlr::PlayerPtr& p,
                              bool dup_warn,
                              int row_id,
                              const char* slot_label = nullptr) {
                ImGui::PushID(row_id);
                ImGui::TableNextRow();
                if (!p) {
                    ImGui::TableNextColumn(); ImGui::TextDisabled("--");
                    ImGui::TableNextColumn();
                    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
                    if (slot_label && slot_label[0])
                        ImGui::Text("[empty %s]", slot_label);
                    else
                        ImGui::Text("[empty %s]", vlr::position_name(slot));
                    ImGui::PopStyleColor();
                    ImGui::TableNextColumn();
                    if (slot_label && slot_label[0])
                        ImGui::TextDisabled("--");
                    else
                        PositionBadge(slot);
                    ImGui::TableNextColumn(); ImGui::TextDisabled("--");
                    ImGui::TableNextColumn(); ImGui::TextDisabled("--");
                    ImGui::TableNextColumn(); ImGui::TextDisabled("--");
                    ImGui::TableNextColumn(); ImGui::TextDisabled("--");
                    ImGui::TableNextColumn(); ImGui::TextDisabled("--");
                    ImGui::PopID();
                    return;
                }
                ImGui::TableNextColumn(); CountryFlag(p->country_iso);
                ImGui::TableNextColumn();
                if (ImGui::Selectable(player_label(*p).c_str(), false,
                        ImGuiSelectableFlags_SpanAllColumns)) {
                    OpenPlayerModal(s, p);
                }
                ImGui::TableNextColumn();
                // Canonical chip order: PositionBadge → FlexBadge → IglBadge.
                PositionBadge(vlr::position_of(*p));
                if (p->is_flex) { ImGui::SameLine(); FlexBadge(); }
                if (p->is_igl)  { ImGui::SameLine(); IglBadge();  }
                if (dup_warn) {
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
                    ImGui::PushFont(g_font_small);
                    ImGui::Text("[duplicate]");
                    ImGui::PopFont();
                    ImGui::PopStyleColor();
                }
                ImGui::TableNextColumn(); ImGui::Text("%d", p->age);
                ImGui::TableNextColumn(); ImGui::Text("%.0f", p->ovr());
                ImGui::TableNextColumn(); ImGui::Text("%.2f", p->avg_match_rating());
                ImGui::TableNextColumn(); ImGui::Text("%.1f", p->kast());
                ImGui::TableNextColumn(); ImGui::Text("$%dK", p->contract.amount_k);
                ImGui::PopID();
            };

            H2("STARTERS");
            if (ImGui::BeginTable("##tp_roster", 8,
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                    ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, 220))) {
                ImGui::TableSetupColumn("FLAG");
                ImGui::TableSetupColumn("PLAYER", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("POSITION");
                ImGui::TableSetupColumn("AGE");
                ImGui::TableSetupColumn("OVR");
                ImGui::TableSetupColumn("AVG RAT");
                ImGui::TableSetupColumn("KAST");
                ImGui::TableSetupColumn("SALARY");
                ImGui::TableHeadersRow();
                int row_id = 0;
                for (size_t i = 0; i < (size_t)vlr::Position::Count; ++i) {
                    auto slot = (vlr::Position)i;
                    tp_row(slot, tp_canonical[i], false, row_id++);
                }
                // 5th / Flex row (no PositionBadge in the empty-state).
                tp_row(vlr::Position::Initiator, tp_fifth, false, row_id++,
                       "5th slot");
                ImGui::EndTable();
            }
            if (!tp_bench.empty()) {
                ImGui::Spacing();
                H2("BENCH");
                if (ImGui::BeginTable("##tp_roster_bench", 8,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                        ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, 150))) {
                    ImGui::TableSetupColumn("FLAG");
                    ImGui::TableSetupColumn("PLAYER", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("POSITION");
                    ImGui::TableSetupColumn("AGE");
                    ImGui::TableSetupColumn("OVR");
                    ImGui::TableSetupColumn("AVG RAT");
                    ImGui::TableSetupColumn("KAST");
                    ImGui::TableSetupColumn("SALARY");
                    ImGui::TableHeadersRow();
                    int row_id = 2000;
                    for (auto& bp : tp_bench)
                        tp_row(vlr::position_of(*bp), bp, false, row_id++);
                    ImGui::EndTable();
                }
            }
            if (t->head_coach) {
                ImGui::Spacing();
                ImGui::BeginChild("##tp_coach", ImVec2(0, 80), ImGuiChildFlags_Border);
                H2("HEAD COACH");
                ImGui::Text("%s   age %d   |   Tactical %d  Dev %d  Leadership %d  |  $%dK/yr",
                            t->head_coach->name.c_str(), t->head_coach->age,
                            t->head_coach->tactical, t->head_coach->development,
                            t->head_coach->leadership, t->head_coach->salary_k);
                ImGui::EndChild();
            }
            ImGui::EndTabItem();
        }

        // === Recent matches: pull from any roster member's pro history ===
        if (ImGui::BeginTabItem("Recent Matches")) {
            std::vector<vlr::RecordedMatchPtr> recent;
            if (!t->roster.empty()) {
                for (auto& r : t->roster.front()->pro_match_history) recent.push_back(r);
            }
            if (recent.empty()) {
                ImGui::TextDisabled("(no recorded matches yet)");
            } else if (ImGui::BeginTable("##tp_recent", 6,
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                    ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, 380))) {
                ImGui::TableSetupColumn("EVENT", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("MAP");
                ImGui::TableSetupColumn("OPPONENT");
                ImGui::TableSetupColumn("SCORE");
                ImGui::TableSetupColumn("RESULT");
                ImGui::TableSetupColumn("WATCH");
                ImGui::TableHeadersRow();
                int idx = 0;
                for (auto& rec : recent) {
                    if (!rec) continue;
                    ImGui::PushID(idx++);
                    bool is_blue = (rec->blue_name == t->name);
                    std::string opp = is_blue ? rec->red_name : rec->blue_name;
                    bool won = is_blue
                        ? (rec->blue_score > rec->red_score)
                        : (rec->red_score  > rec->blue_score);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::Text("%s", rec->event.c_str());
                    ImGui::TableNextColumn(); ImGui::Text("%s", rec->map_name.c_str());
                    ImGui::TableNextColumn(); ImGui::Text("%s", opp.c_str());
                    ImGui::TableNextColumn(); ImGui::Text("%s", rec->score.c_str());
                    ImGui::TableNextColumn();
                    ImGui::PushStyleColor(ImGuiCol_Text, toV4(won ? kVlrGreen : kVlrRed));
                    ImGui::Text("%s", won ? "W" : "L");
                    ImGui::PopStyleColor();
                    ImGui::TableNextColumn();
                    if (ImGui::SmallButton("Watch")) OpenReplay(s, rec);
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        // === Upcoming matches: filter the calendar to this team =========
        if (ImGui::BeginTabItem("Upcoming")) {
            auto fixtures = s.gm.upcoming_fixtures(30);
            std::vector<vlr::UpcomingFixture> mine;
            for (auto& f : fixtures) if (f.a == t || f.b == t) mine.push_back(f);
            if (mine.empty()) {
                ImGui::TextDisabled("(no fixtures scheduled in the next 30 days)");
            } else if (ImGui::BeginTable("##tp_upc", 4,
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableSetupColumn("DAY");
                ImGui::TableSetupColumn("EVENT");
                ImGui::TableSetupColumn("OPPONENT", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("WHEN");
                ImGui::TableHeadersRow();
                int upc_idx = 0;
                for (auto& f : mine) {
                    ImGui::PushID(upc_idx++);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    int delta = f.day_in_year - s.gm.day_in_year;
                    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                    if (delta == 0) ImGui::Text("TODAY");
                    else            ImGui::Text("+%dd", delta);
                    ImGui::PopStyleColor();
                    ImGui::TableNextColumn(); ImGui::Text("%s", f.label.c_str());
                    ImGui::TableNextColumn();
                    {
                        vlr::TeamPtr opp = (f.a == t) ? f.b : f.a;
                        if (opp) {
                            CountryFlag(opp->home_country_iso); ImGui::SameLine();
                            if (ImGui::Selectable(opp->name.c_str(), false,
                                    ImGuiSelectableFlags_SpanAllColumns)) {
                                OpenTeamProfile(s, opp);
                            }
                        }
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text("Day %d/%d", f.day_in_year + 1,
                                vlr::GameManager::kDaysPerYear);
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        // === Maps: aggregate roster's per-map ratings ===================
        if (ImGui::BeginTabItem("Maps")) {
            if (ImGui::BeginTable("##tp_maps", 4,
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableSetupColumn("MAP", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("MATCHES PLAYED");
                ImGui::TableSetupColumn("AVG TEAM RATING");
                ImGui::TableSetupColumn("FAVORED ROLE");
                ImGui::TableHeadersRow();
                for (auto& m : vlr::maps()) {
                    int total_count = 0;
                    double total_rating = 0.0;
                    for (auto& p : t->roster) {
                        auto it = p->map_stats.find(m.name);
                        if (it == p->map_stats.end()) continue;
                        total_count += it->second.count;
                        total_rating += it->second.rating_total;
                    }
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::Text("%s", m.name.c_str());
                    ImGui::TableNextColumn(); ImGui::Text("%d", total_count);
                    ImGui::TableNextColumn();
                    if (total_count > 0) {
                        double avg = total_rating / total_count;
                        ImU32 col = avg >= 1.10 ? kVlrGreen : (avg >= 0.95 ? kVlrText : kVlrRed);
                        ImGui::PushStyleColor(ImGuiCol_Text, toV4(col));
                        ImGui::Text("%.2f", avg);
                        ImGui::PopStyleColor();
                    } else {
                        ImGui::TextDisabled("—");
                    }
                    ImGui::TableNextColumn();
                    ImGui::PushStyleColor(ImGuiCol_Text, toV4(role_color(m.favored_role)));
                    ImGui::Text("%s", vlr::role_name(m.favored_role));
                    ImGui::PopStyleColor();
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        // === Bio / org info =============================================
        if (ImGui::BeginTabItem("Bio")) {
            H2("ORGANIZATION");
            ImGui::Text("Name:");        ImGui::SameLine(180); ImGui::Text("%s", t->name.c_str());
            ImGui::Text("Region:");      ImGui::SameLine(180); ImGui::Text("VCT %s", t->region.c_str());
            ImGui::Text("Home country:"); ImGui::SameLine(180);
            CountryFlag(t->home_country_iso); ImGui::SameLine();
            ImGui::Text("%s", t->home_country.empty() ? "?" : t->home_country.c_str());
            ImGui::Text("Headquarters:"); ImGui::SameLine(180);
            ImGui::Text("%s", t->home_city.empty() ? "?" : t->home_city.c_str());
            ImGui::Text("Personality:"); ImGui::SameLine(180);
            switch (t->personality) {
                case vlr::Personality::Aggressive: ImGui::Text("Aggressive"); break;
                case vlr::Personality::Tactical:   ImGui::Text("Tactical"); break;
                case vlr::Personality::Budget:     ImGui::Text("Budget"); break;
                case vlr::Personality::Balanced:   ImGui::Text("Balanced"); break;
            }
            // Comp shape — every team is 4 canonical gameplay slots plus
            // a flexible 5th. Flex and IGL are independent OVERLAY flags
            // that any starter may carry (Team::enforce_one_flex ensures
            // >=1 is_flex player exists in the starting 5; multiples are
            // allowed). Flex tendency below derives the concrete rotation
            // list from the is_flex player's agent_pool roles.
            ImGui::Text("Comp shape:"); ImGui::SameLine(180);
            ImGui::TextWrapped(
                "1D + 1C + 1S + 1I + 1 flexible 5th "
                "(Flex/secondary I/secondary C). "
                "1 IGL flag overlaid on any starter.");

            // === Flex tendency — concrete rotation list =================
            // Algorithm:
            //   1. Collect every roster member with is_flex == true. Per
            //      the new model multiple Flex starters are allowed
            //      (Team::enforce_one_flex only ensures >=1 exists).
            //   2. Pull each Flex player's agent_pool, collect distinct
            //      roles (primary_role first, drop primary_role if it
            //      never appears in the pool).
            //   3. Format:
            //        - 0 Flex players in roster -> "No designated Flex
            //                                      player (rare)."
            //        - 1 Flex player, 0 agents  -> "not yet set"
            //        - 1 Flex player, 1 role    -> "locked to {Role}"
            //                                      + "(low rotation player)"
            //        - 1 Flex player, 2-3 roles -> "{Name} rotates between
            //                                      {Role A / Role B [/ C]}"
            //        - 2+ Flex players          -> "Flex players: {names},
            //                                      rotating between
            //                                      {Role A / Role B [/ C]}"
            ImGui::Text("Flex tendency:"); ImGui::SameLine(180);
            {
                std::vector<vlr::PlayerPtr> flex_players;
                for (auto& pp : t->roster) {
                    if (pp && pp->is_flex) flex_players.push_back(pp);
                }
                if (flex_players.empty()) {
                    ImGui::TextDisabled("No designated Flex player (rare).");
                } else if (flex_players.size() == 1) {
                    auto& flex_p = flex_players.front();
                    if (flex_p->agent_pool.empty()) {
                        ImGui::TextDisabled("not yet set");
                    } else {
                        std::vector<vlr::Role> roles_seen;
                        auto push_unique = [&](vlr::Role r) {
                            for (auto x : roles_seen) if (x == r) return;
                            roles_seen.push_back(r);
                        };
                        push_unique(flex_p->primary_role);
                        for (auto* a : flex_p->agent_pool) {
                            if (a) push_unique(a->role);
                        }
                        bool primary_in_pool = false;
                        for (auto* a : flex_p->agent_pool) {
                            if (a && a->role == flex_p->primary_role) {
                                primary_in_pool = true; break;
                            }
                        }
                        if (!primary_in_pool && !roles_seen.empty()
                            && roles_seen.front() == flex_p->primary_role) {
                            roles_seen.erase(roles_seen.begin());
                        }
                        if (roles_seen.empty()) {
                            ImGui::TextDisabled("not yet set");
                        } else if (roles_seen.size() == 1) {
                            ImGui::Text("locked to %s",
                                        vlr::role_name(roles_seen[0]));
                            ImGui::PushFont(g_font_small);
                            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
                            ImGui::SameLine();
                            ImGui::Text("(low rotation player)");
                            ImGui::PopStyleColor();
                            ImGui::PopFont();
                        } else {
                            if (roles_seen.size() > 3) roles_seen.resize(3);
                            std::string list;
                            for (size_t i = 0; i < roles_seen.size(); ++i) {
                                if (i) list += " / ";
                                list += vlr::role_name(roles_seen[i]);
                            }
                            ImGui::Text("%s rotates between %s",
                                        flex_p->name.c_str(), list.c_str());
                        }
                    }
                } else {
                    // Multiple Flex players — show names + union of roles.
                    std::vector<vlr::Role> roles_seen;
                    auto push_unique = [&](vlr::Role r) {
                        for (auto x : roles_seen) if (x == r) return;
                        roles_seen.push_back(r);
                    };
                    std::string names;
                    for (size_t i = 0; i < flex_players.size(); ++i) {
                        if (i) names += ", ";
                        names += flex_players[i]->name;
                        push_unique(flex_players[i]->primary_role);
                        for (auto* a : flex_players[i]->agent_pool) {
                            if (a) push_unique(a->role);
                        }
                    }
                    if (roles_seen.size() > 3) roles_seen.resize(3);
                    std::string list;
                    for (size_t i = 0; i < roles_seen.size(); ++i) {
                        if (i) list += " / ";
                        list += vlr::role_name(roles_seen[i]);
                    }
                    if (list.empty()) {
                        ImGui::Text("Flex players: %s", names.c_str());
                    } else {
                        ImGui::Text("Flex players: %s, rotating between %s",
                                    names.c_str(), list.c_str());
                    }
                }
            }

            // Comp Identity — distinct from "personality" and "strategy".
            // Surfaces the new CompIdentity classification on Team. Display
            // only; value is engine-driven.
            ImGui::Text("Comp Identity:"); ImGui::SameLine(180);
            ImGui::Text("%s", vlr::identity_name(t->comp_identity));

            // === C3: Strategy + inertia ====================================
            // Surfaces Agent A's `t->strategy` plus a from-previous shift
            // indicator. If `strategy == previous_strategy` (stable) the
            // trailing label is suppressed. A local lambda maps the enum
            // to a display name — kept inside the function body per the
            // "no new top-level functions" rule.
            auto strategy_name = [](vlr::Team::Strategy s) -> const char* {
                switch (s) {
                    case vlr::Team::Strategy::Contender:        return "Contender";
                    case vlr::Team::Strategy::Rebuilding:       return "Rebuilding";
                    case vlr::Team::Strategy::Bridge:           return "Bridge";
                    case vlr::Team::Strategy::BudgetRoster:     return "Budget Roster";
                    case vlr::Team::Strategy::DevelopmentFocus: return "Development Focus";
                    case vlr::Team::Strategy::WinNow:           return "Win Now";
                    case vlr::Team::Strategy::TalentFarm:       return "Talent Farm";
                }
                return "?";
            };
            ImGui::Text("Strategy:"); ImGui::SameLine(180);
            PillOutline(strategy_name(t->strategy), kAccent, kVlrText);
            if (t->previous_strategy != t->strategy) {
                ImGui::SameLine();
                ImGui::PushFont(g_font_small);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
                ImGui::Text("\xE2\x86\x90 from %s",
                            strategy_name(t->previous_strategy));
                ImGui::PopStyleColor();
                ImGui::PopFont();
            }

            // === Phase B (C1): Championship Window indicator ===============
            // Surfaces Agent A's `t->window` lifecycle classifier. Pill is
            // colour-coded by window state and gets a directional glyph
            // (rising / peak / hourglass / falling). A one-line MutedText
            // below explains the behavioural implication for AI decisions.
            {
                ImU32 wcol = kAccent;
                const char* glyph = "\xE2\x97\x8F"; // ●
                const char* blurb = "";
                switch (t->window) {
                    case vlr::TeamWindow::Opening:
                        wcol = kVlrGreen;
                        glyph = "\xE2\x96\xB2"; // ▲
                        blurb = "Building a young core "
                                "\xE2\x80\x94 patient development, "
                                "regional focus.";
                        break;
                    case vlr::TeamWindow::Open:
                        wcol = kAccent;
                        glyph = "\xE2\x97\x8F"; // ●
                        blurb = "Prime contention years "
                                "\xE2\x80\x94 re-sign cores, push for "
                                "titles.";
                        break;
                    case vlr::TeamWindow::Closing:
                        wcol = kVlrGold;
                        glyph = "\xE2\x8F\xB3"; // ⏳
                        blurb = "Window closing "
                                "\xE2\x80\x94 aggressive imports, "
                                "win-now spending.";
                        break;
                    case vlr::TeamWindow::Closed:
                        wcol = kVlrRedDim;
                        glyph = "\xE2\x96\xBC"; // ▼
                        blurb = "Rebuild phase "
                                "\xE2\x80\x94 cycling out vets, "
                                "betting on potential.";
                        break;
                }
                ImGui::Text("Window:"); ImGui::SameLine(180);
                char wlabel[64];
                std::snprintf(wlabel, sizeof(wlabel), "%s WINDOW: %s",
                              glyph, vlr::team_window_name(t->window));
                Pill(wlabel, wcol);
                MutedText("%s", blurb);
            }

            // === C2: Org Memory grid =======================================
            // Surfaces Agent A's six rolling memory metrics. Each tile shows
            // a small-caps label, signed value (green/red/text by sign +
            // magnitude) and a thin centered horizontal bar that extends
            // left/right from the midpoint to encode the [-100,+100] value.
            // 3 columns x 2 rows; tile ~ 200x60.
            ImGui::Spacing();
            SectionHeader("ORG MEMORY",
                          "Hidden rolling metrics that steer long-term AI",
                          kAccent2);
            {
                struct MemMetric { const char* label; int value; };
                MemMetric mm[6] = {
                    { "ROOKIE SUCCESS",      t->memory.rookie_success       },
                    { "IMPORT SUCCESS",      t->memory.import_success       },
                    { "VETERAN SUCCESS",     t->memory.veteran_success      },
                    { "FINANCIAL DISCIPLINE",t->memory.financial_discipline },
                    { "STABILITY CULTURE",   t->memory.stability_culture    },
                    { "STAR DEPENDENCY",     t->memory.star_dependency      },
                };
                const ImVec2 tile_size(200.0f, 60.0f);
                for (int i = 0; i < 6; ++i) {
                    if (i % 3 != 0) ImGui::SameLine();
                    ImGui::PushID(900 + i);
                    if (BeginCardSized("##memtile", tile_size, kSurface,
                                       8.0f, 10.0f)) {
                        int v = mm[i].value;
                        if (v < -100) v = -100; else if (v > 100) v = 100;
                        ImU32 vcol = (v >=  30) ? kVlrGreen
                                   : (v <= -30) ? kVlrRed
                                   :              kVlrText;
                        // Label
                        ImGui::PushFont(g_font_small);
                        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
                        ImGui::TextUnformatted(mm[i].label);
                        ImGui::PopStyleColor();
                        ImGui::PopFont();
                        // Signed value
                        ImGui::PushStyleColor(ImGuiCol_Text, toV4(vcol));
                        ImGui::Text("%+d", v);
                        ImGui::PopStyleColor();
                        // Inline bar — centered on midpoint, extends per sign.
                        {
                            ImVec2 cp = ImGui::GetCursorScreenPos();
                            float avail = ImGui::GetContentRegionAvail().x;
                            if (avail < 40.0f) avail = 40.0f;
                            float bar_h = 5.0f;
                            float mid_x = cp.x + avail * 0.5f;
                            ImDrawList* dl = ImGui::GetWindowDrawList();
                            // Track
                            dl->AddRectFilled(
                                ImVec2(cp.x, cp.y),
                                ImVec2(cp.x + avail, cp.y + bar_h),
                                kBorder, bar_h * 0.5f);
                            // Filled signed bar
                            float half = avail * 0.5f;
                            float ext  = (std::abs(v) / 100.0f) * half;
                            if (ext > 0.0f) {
                                ImVec2 a = (v >= 0)
                                    ? ImVec2(mid_x, cp.y)
                                    : ImVec2(mid_x - ext, cp.y);
                                ImVec2 b = (v >= 0)
                                    ? ImVec2(mid_x + ext, cp.y + bar_h)
                                    : ImVec2(mid_x, cp.y + bar_h);
                                dl->AddRectFilled(a, b, vcol, bar_h * 0.5f);
                            }
                            // Midpoint tick
                            dl->AddLine(ImVec2(mid_x, cp.y - 1.0f),
                                        ImVec2(mid_x, cp.y + bar_h + 1.0f),
                                        kBorderStrong, 1.0f);
                            ImGui::Dummy(ImVec2(avail, bar_h + 2.0f));
                        }
                    }
                    EndCardSized();
                    ImGui::PopID();
                }
                MutedText("Org memory drives long-term AI decisions. "
                          "High Rookie Success -> more youth signings. "
                          "Negative Import Success -> AI shies away from "
                          "foreign FAs.");
            }

            // === Phase B (C2): Roster chemistry panel ======================
            // Surfaces Agent A's `top_chemistry_pairs(n)` — co-play synergy
            // values learned from match outcomes. Each pair gets a coloured
            // value (green = cohesive, red = toxic, otherwise muted) and a
            // small inline horizontal bar in the [-2, +2] range so the user
            // can visually compare magnitudes at a glance.
            ImGui::Spacing();
            SectionHeader("ROSTER CHEMISTRY",
                          "Co-play synergy built from match outcomes",
                          kAccent2);
            {
                auto pairs = t->top_chemistry_pairs(5);
                if (pairs.empty()) {
                    MutedText("No co-play history yet "
                              "\xE2\x80\x94 chemistry develops over "
                              "the season.");
                } else {
                    int ci = 0;
                    for (auto& tup : pairs) {
                        auto* pa = std::get<0>(tup);
                        auto* pb = std::get<1>(tup);
                        double v = std::get<2>(tup);
                        if (!pa || !pb) continue;
                        ImGui::PushID(8200 + ci++);
                        if (BeginCard("##chempair", kSurface, 10.0f, 8.0f)) {
                            ImU32 vcol = (v >=  1.0) ? kVlrGreen
                                       : (v <= -0.5) ? kVlrRed
                                       :               kVlrSub;
                            // Row label: "A <-> B"
                            ImGui::TextUnformatted(pa->name.c_str());
                            ImGui::SameLine();
                            ImGui::TextDisabled("\xE2\x86\x94");
                            ImGui::SameLine();
                            ImGui::TextUnformatted(pb->name.c_str());
                            // Signed value, coloured.
                            ImGui::SameLine();
                            ImGui::Dummy(ImVec2(10, 0));
                            ImGui::SameLine();
                            ImGui::PushStyleColor(ImGuiCol_Text, toV4(vcol));
                            ImGui::Text("%+.2f", v);
                            ImGui::PopStyleColor();
                            // Inline bar — [-2, +2] range, centered on mid.
                            {
                                ImVec2 cp = ImGui::GetCursorScreenPos();
                                float avail =
                                    ImGui::GetContentRegionAvail().x;
                                if (avail < 60.0f) avail = 60.0f;
                                float bar_h = 5.0f;
                                float mid_x = cp.x + avail * 0.5f;
                                ImDrawList* dl =
                                    ImGui::GetWindowDrawList();
                                dl->AddRectFilled(
                                    ImVec2(cp.x, cp.y),
                                    ImVec2(cp.x + avail, cp.y + bar_h),
                                    kBorder, bar_h * 0.5f);
                                double vc = v;
                                if (vc < -2.0) vc = -2.0;
                                if (vc >  2.0) vc =  2.0;
                                float half = avail * 0.5f;
                                float ext  =
                                    (float)(std::abs(vc) / 2.0) * half;
                                if (ext > 0.0f) {
                                    ImVec2 a = (vc >= 0)
                                        ? ImVec2(mid_x, cp.y)
                                        : ImVec2(mid_x - ext, cp.y);
                                    ImVec2 b = (vc >= 0)
                                        ? ImVec2(mid_x + ext,
                                                 cp.y + bar_h)
                                        : ImVec2(mid_x, cp.y + bar_h);
                                    dl->AddRectFilled(
                                        a, b, vcol, bar_h * 0.5f);
                                }
                                dl->AddLine(
                                    ImVec2(mid_x, cp.y - 1.0f),
                                    ImVec2(mid_x, cp.y + bar_h + 1.0f),
                                    kBorderStrong, 1.0f);
                                ImGui::Dummy(ImVec2(avail, bar_h + 2.0f));
                            }
                        }
                        EndCard();
                        ImGui::PopID();
                    }
                }
            }

            ImGui::Spacing();
            H2("HISTORICAL RECORD");
            if (t->history.empty()) {
                ImGui::TextDisabled("(no completed seasons yet)");
            } else if (ImGui::BeginTable("##tp_hist", 4,
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableSetupColumn("YEAR");
                ImGui::TableSetupColumn("WINS");
                ImGui::TableSetupColumn("LOSSES");
                ImGui::TableSetupColumn("WIN RATE");
                ImGui::TableHeadersRow();
                for (auto& h : t->history) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::Text("%d", h.year);
                    ImGui::TableNextColumn(); ImGui::Text("%d", h.wins);
                    ImGui::TableNextColumn(); ImGui::Text("%d", h.losses);
                    ImGui::TableNextColumn();
                    int total = h.wins + h.losses;
                    if (total > 0) ImGui::Text("%.0f%%", 100.0 * h.wins / total);
                    else           ImGui::TextDisabled("—");
                }
                ImGui::EndTable();
            }

            // === Rivalries (C10 — uses Pack B h2h split accessors) ====
            // Lists every frequent opponent (h2h_total >= 3 this season)
            // with reg/playoff split + finals-title count between teams.
            ImGui::Spacing();
            H2("RIVALRIES (this season)");
            {
                struct RivRow {
                    vlr::Team* opp = nullptr;
                    int total = 0;
                    int reg = 0;
                    int po = 0;
                    int finals_titles_vs = 0;
                };
                std::vector<RivRow> rivs;
                for (auto& kv : s.gm.leagues) {
                    if (!kv.second) continue;
                    for (auto& tt : kv.second->teams()) {
                        if (!tt || tt.get() == t.get()) continue;
                        int total = s.gm.h2h_total(t.get(), tt.get());
                        if (total < 3) continue;
                        RivRow r;
                        r.opp = tt.get();
                        r.total = total;
                        r.reg = s.gm.h2h_regular(t.get(), tt.get());
                        r.po  = s.gm.h2h_playoff(t.get(), tt.get());
                        auto finals = s.gm.h2h_finals_between(
                            t.get(), tt.get());
                        for (auto& fF : finals) {
                            if (fF.winner == t.get()) r.finals_titles_vs++;
                        }
                        rivs.push_back(r);
                    }
                }
                std::sort(rivs.begin(), rivs.end(),
                    [](const RivRow& a, const RivRow& b) {
                        return a.total > b.total;
                    });
                if (rivs.empty()) {
                    MutedText("No frequent rivals this season "
                              "(need >= 3 meetings to qualify).");
                } else if (ImGui::BeginTable("##tp_rivs", 5,
                        ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_BordersInnerV)) {
                    ImGui::TableSetupColumn("VS");
                    ImGui::TableSetupColumn("Total");
                    ImGui::TableSetupColumn("Regular");
                    ImGui::TableSetupColumn("Playoff");
                    ImGui::TableSetupColumn("Finals (won by this team)");
                    ImGui::TableHeadersRow();
                    int ri = 0;
                    for (auto& r : rivs) {
                        if (!r.opp) continue;
                        ImGui::PushID(ri++);
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        if (!r.opp->tag.empty()) {
                            ImU32 tc = IM_COL32(r.opp->color_primary_r,
                                r.opp->color_primary_g,
                                r.opp->color_primary_b, 0xFF);
                            ImGui::PushStyleColor(ImGuiCol_Text, toV4(tc));
                            ImGui::TextUnformatted(r.opp->tag.c_str());
                            ImGui::PopStyleColor();
                            ImGui::SameLine();
                        }
                        ImGui::TextUnformatted(r.opp->name.c_str());
                        ImGui::TableNextColumn(); ImGui::Text("%d", r.total);
                        ImGui::TableNextColumn(); ImGui::Text("%d", r.reg);
                        ImGui::TableNextColumn(); ImGui::Text("%d", r.po);
                        ImGui::TableNextColumn();
                        if (r.finals_titles_vs > 0) {
                            ImGui::PushStyleColor(ImGuiCol_Text,
                                toV4(kVlrGold));
                            ImGui::Text("%d", r.finals_titles_vs);
                            ImGui::PopStyleColor();
                        } else {
                            ImGui::TextDisabled("0");
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
            }
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}

// ===== Calendar screen: upcoming fixtures grouped by day =================
// =========================================================================
// Calendar (Phase 2): month-grid layout
// =========================================================================
// Replaces the legacy table view. Renders a 5x7 grid of 80x56 day cells
// centred on `today`, navigable with +/- 30 day buttons. Cells are tinted
// by phase / matchday status; hover shows a tooltip with all matchups for
// that day across regions; click opens a modal listing the day's matchups
// with click-through into the live viewer.
//
// Schedule data: we drain `gm.upcoming_fixtures(max_days_ahead)` which returns
// already-paired stage round-robin fixtures across all regions for the
// requested look-ahead window. That's the canonical accessor and works
// for any positive look-ahead. Tournament matchdays are tinted via the
// phase background (REGIONALS/MASTERS/CHAMPIONS), but per-matchup data
// for those isn't returned by `upcoming_fixtures` — the bracket UI is the
// dedicated surface for that.
namespace {

struct CalDayInfo {
    int  day = -1;        // day_in_year, or -1 for empty trailing cells
    int  year = 0;
    bool is_today = false;
    bool is_user_match = false;
    bool is_matchday = false;
    bool is_tournament = false;
    std::string phase;          // e.g. "STAGE 1", "REGIONALS 1", "OFFSEASON"
    std::string short_tag;      // small per-day note ("vs FNX" / "MASTERS R1")
    std::vector<vlr::UpcomingFixture> fixtures;
};

// Background tint per phase. Returns 0 if no tint (keeps default cell bg).
ImU32 phase_tint(const std::string& phase) {
    if (phase.find("STAGE") == 0)        return IM_COL32(0x16, 0x22, 0x30, 0xFF);
    if (phase.find("REGIONALS") == 0)    return IM_COL32(0x40, 0x18, 0x18, 0x60);
    if (phase.find("MASTERS") == 0)      return IM_COL32(0x4A, 0x2A, 0x60, 0x60);
    if (phase.find("CHAMPIONS") == 0)    return IM_COL32(0x5C, 0x46, 0x16, 0x80);
    if (phase == "AWARDS")               return IM_COL32(0x60, 0x4A, 0x16, 0x90);
    if (phase == "OFFSEASON")            return IM_COL32(0x1A, 0x2B, 0x1A, 0xFF);
    return 0;
}

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
                            OpenLiveMatchSeries(s, f.a, f.b, f.label, 3);
                            s.cal_modal_day = -1;
                            s.cal_modal_fixtures.clear();
                            ImGui::CloseCurrentPopup();
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
            ImGui::TextColored(toV4(r.was_pro ? kVlrGreen : kVlrBlue),
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
void DrawFavorites(AppState& s) {
    SectionHeader("FAVORITES",
        "Favorited players, Hall of Fame, all-time / single-season GOAT "
        "rankings, and the formula lab — all in one place.",
        kAccent);
    VSpace(6);

    ImGui::BeginTabBar("##favtabs");

    // ---- MVP Race (live, season-long buildup) ----
    if (ImGui::BeginTabItem("MVP Race")) {
        H2("SEASON-LONG AWARD RACE");
        Sub("Updated every 30 in-game days. Hybrid score: season rating + "
            "team success + international weight. Climb / slip indicators "
            "track movement vs the previous tick.");
        ImGui::Spacing();

        if (s.gm.mvp_race.empty()) {
            ImGui::TextDisabled("(race builds after a few months of play)");
        } else {
            ImGui::BeginTabBar("##race_cats");
            for (auto& lb : s.gm.mvp_race) {
                if (!ImGui::BeginTabItem(lb.category.c_str())) continue;
                if (ImGui::BeginTable("##race_tbl", 8,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                        ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, 460))) {
                    ImGui::TableSetupColumn("#");
                    ImGui::TableSetupColumn("FLAG");
                    ImGui::TableSetupColumn("PLAYER", ImGuiTableColumnFlags_WidthStretch);
                    // Phase 1 (Agent C): signature agent column. Dim/small text.
                    ImGui::TableSetupColumn("SIG");
                    ImGui::TableSetupColumn("SCORE");
                    ImGui::TableSetupColumn("WHY");
                    ImGui::TableSetupColumn("TREND");
                    ImGui::TableSetupColumn("DELTA");
                    ImGui::TableHeadersRow();
                    int rk = 1, ri = 0;
                    for (auto& c : lb.candidates) {
                        if (!c.player) continue;
                        ImGui::PushID(ri++);
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::PushStyleColor(ImGuiCol_Text, toV4(placement_color(rk)));
                        ImGui::Text("#%d", rk);
                        ImGui::PopStyleColor();
                        ImGui::TableNextColumn(); CountryFlag(c.player->country_iso);
                        ImGui::TableNextColumn();
                        if (ImGui::Selectable(player_label(*c.player).c_str(), false,
                                ImGuiSelectableFlags_SpanAllColumns)) {
                            OpenPlayerModal(s, c.player);
                        }
                        // SIG column (Phase 1, Agent C) — small dim text.
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
                        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                        ImGui::Text("%.2f", c.score);
                        ImGui::PopStyleColor();

                        // WHY column — dominant trait surfaced from the
                        // multi-factor MVP score. Priority: tactical IGL
                        // impact > pressure-match closer > carry-game frag
                        // leader > general frag leader.
                        ImGui::TableNextColumn();
                        const char* why = "Frag leader";
                        ImU32 why_col = kVlrText;
                        if (c.player->is_igl && c.player->igl_impact_season >= 5.0) {
                            why = "Tactical IGL";
                            why_col = kVlrBlue;
                        } else if (c.player->season_pressure_matches >= 3) {
                            why = "Pressure closer";
                            why_col = kVlrGold;
                        } else if (c.player->career_max_match_kd_x100 >= 250
                                && c.player->season_matches > 0) {
                            why = "Carry games";
                            why_col = kVlrGreen;
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
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndTabItem();
    }

    // ---- Last Season Awards Recap ----
    if (ImGui::BeginTabItem("Awards Recap")) {
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
                ImGui::BeginChild("##award_card", ImVec2(0, 130), ImGuiChildFlags_Border);
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
                }

                ImGui::PushFont(g_font_small);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
                ImGui::TextWrapped("%s", aw.explanation.c_str());
                ImGui::PopStyleColor();
                ImGui::PopFont();

                if (aw.finalists.size() > 1) {
                    ImGui::Text("Finalists:");
                    for (std::size_t i = 1; i < aw.finalists.size(); ++i) {
                        ImGui::SameLine();
                        ImGui::PushID((int)i);
                        if (ImGui::SmallButton(aw.finalists[i]->name.c_str())) {
                            OpenPlayerModal(s, aw.finalists[i]);
                        }
                        ImGui::PopID();
                        ImGui::SameLine();
                        ImGui::TextDisabled("(%.2f)", aw.scores[i]);
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
        ImGui::EndTabItem();
    }

    // ---- All-time Awards History ----
    if (ImGui::BeginTabItem("Awards History")) {
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
                ImGui::TableNextColumn(); ImGui::Text("%d", aw.year);
                ImGui::TableNextColumn(); ImGui::Text("%s", aw.category.c_str());
                ImGui::TableNextColumn();
                if (aw.winner) {
                    CountryFlag(aw.winner->country_iso); ImGui::SameLine();
                    if (ImGui::Selectable(aw.winner->name.c_str(), false)) {
                        OpenPlayerModal(s, aw.winner);
                    }
                }
                ImGui::TableNextColumn();
                if (!aw.scores.empty()) ImGui::Text("%.2f", aw.scores.front());
                else ImGui::TextDisabled("-");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }

    // ---- Hall of Fame ----
    if (ImGui::BeginTabItem("Hall of Fame")) {
        // Filter out anyone not currently retired — strictly retirees only.
        std::vector<vlr::PlayerPtr> hof;
        for (auto& p : s.gm.hall_of_fame) {
            if (p && p->is_retired) hof.push_back(p);
        }
        if (hof.empty()) {
            ImGui::TextDisabled("No inductees yet. Players must retire first, "
                                "have 3+ seasons played, and meet 4 of the 9 "
                                "Hall of Fame criteria (international/role/MVP "
                                "title, 1.20+ season rating, top-20 solo, "
                                "$1M earnings, 30+ kill match, 2.0 KD match, "
                                "or Grand Final clutch).");
        } else if (ImGui::BeginTable("##hof_table", 6,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, 540))) {
            ImGui::TableSetupColumn("FLAG");
            ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("REGION");
            ImGui::TableSetupColumn("CAREER M");
            ImGui::TableSetupColumn("AVG RAT");
            ImGui::TableSetupColumn("AWARDS");
            ImGui::TableHeadersRow();
            int idx = 0;
            // Iterate the filtered `hof` (retired-only); never raw hall_of_fame.
            for (auto& p : hof) {
                if (!p) continue;
                ImGui::PushID(idx++);
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); CountryFlag(p->country_iso);
                ImGui::TableNextColumn();
                if (ImGui::Selectable(player_label(*p).c_str(), false,
                        ImGuiSelectableFlags_SpanAllColumns)) {
                    OpenPlayerModal(s, p);
                }
                ImGui::TableNextColumn(); ImGui::Text("%s", p->region.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%d", p->career_matches);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", p->avg_match_rating());
                ImGui::TableNextColumn(); ImGui::Text("%zu", p->awards.size());
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }

    // ---- Favorited list ----
    if (ImGui::BeginTabItem("Favorited")) {
        if (s.gm.favorite_players.empty()) {
            ImGui::TextDisabled("No favorited players yet. Open a player "
                                "profile and click 'Favorite' to add them here.");
        } else if (ImGui::BeginTable("##favs", 8,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, 540))) {
            ImGui::TableSetupColumn("FLAG");
            ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("ROLE");
            ImGui::TableSetupColumn("AGE");
            ImGui::TableSetupColumn("OVR");
            ImGui::TableSetupColumn("CAREER M");
            ImGui::TableSetupColumn("AVG RAT");
            ImGui::TableSetupColumn("MMR");
            ImGui::TableHeadersRow();
            int idx = 0;
            for (auto& p : s.gm.favorite_players) {
                if (!p) continue;
                ImGui::PushID(idx++);
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); CountryFlag(p->country_iso);
                ImGui::TableNextColumn();
                if (ImGui::Selectable(p->name.c_str(), false,
                        ImGuiSelectableFlags_SpanAllColumns)) {
                    OpenPlayerModal(s, p);
                }
                ImGui::TableNextColumn();
                PositionBadge(vlr::position_of(*p));
                if (p->is_igl) { ImGui::SameLine(); IglBadge(); }
                ImGui::TableNextColumn(); ImGui::Text("%d", p->age);
                ImGui::TableNextColumn(); ImGui::Text("%.0f", p->ovr());
                ImGui::TableNextColumn(); ImGui::Text("%d", p->career_matches);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", p->avg_match_rating());
                ImGui::TableNextColumn(); ImGui::Text("%d", p->solo_mmr);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }

    // Helper: gather all players once for the GOAT computations.
    auto collect_players = [&]() {
        std::vector<vlr::PlayerPtr> all;
        for (auto& kv : s.gm.solo_qs) {
            for (auto& p : kv.second->global_ladder()) all.push_back(p);
        }
        return all;
    };

    // ---- Career GOAT ----
    if (ImGui::BeginTabItem("GOAT Career")) {
        Sub("All-time greats ranked by your formula. Min 10 career matches.");
        auto all = collect_players();
        auto rows = vlr::compute_goat_career(all, s.goat_weights, 100);
        if (rows.empty()) {
            ImGui::TextDisabled("(No qualifying careers yet — sim a few seasons.)");
        } else if (ImGui::BeginTable("##gcareer", 9,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, 520))) {
            ImGui::TableSetupColumn("#");
            ImGui::TableSetupColumn("FLAG");
            ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("ROLE");
            ImGui::TableSetupColumn("CAREER M");
            ImGui::TableSetupColumn("AVG RAT");
            ImGui::TableSetupColumn("AWARDS");
            ImGui::TableSetupColumn("PEAK MMR");
            ImGui::TableSetupColumn("GOAT SCORE");
            ImGui::TableHeadersRow();
            int rank = 1, idx = 0;
            for (auto& r : rows) {
                if (!r.player) continue;
                ImGui::PushID(idx++);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(placement_color(rank)));
                ImGui::Text("#%d", rank);
                ImGui::PopStyleColor();
                ImGui::TableNextColumn(); CountryFlag(r.player->country_iso);
                ImGui::TableNextColumn();
                if (ImGui::Selectable(player_label(*r.player).c_str(), false,
                        ImGuiSelectableFlags_SpanAllColumns)) {
                    OpenPlayerModal(s, r.player);
                }
                ImGui::TableNextColumn();
                PositionBadge(vlr::position_of(*r.player));
                if (r.player->is_igl) { ImGui::SameLine(); IglBadge(); }
                ImGui::TableNextColumn(); ImGui::Text("%d", r.player->career_matches);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", r.player->avg_match_rating());
                ImGui::TableNextColumn(); ImGui::Text("%zu", r.player->awards.size());
                ImGui::TableNextColumn(); ImGui::Text("%d", r.player->peak_mmr);
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                ImGui::Text("%.1f", r.score);
                ImGui::PopStyleColor();
                ImGui::PopID();
                ++rank;
            }
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }

    // ---- Season GOAT ----
    if (ImGui::BeginTabItem("GOAT Season")) {
        Sub("All-time best individual seasons (every season ever played by anyone, "
            "including retirees). Old legendary years stay on the board forever — "
            "never reset between seasons.");
        auto all = collect_players();
        // Generous cap so historic seasons aren't pushed off by recent ones.
        auto rows = vlr::compute_goat_season(all, s.goat_weights, 500);
        if (rows.empty()) {
            ImGui::TextDisabled("(No season history yet — sim a few seasons.)");
        } else if (ImGui::BeginTable("##gseason", 9,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, 520))) {
            ImGui::TableSetupColumn("#");
            ImGui::TableSetupColumn("FLAG");
            ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("YEAR");
            ImGui::TableSetupColumn("TEAM");
            ImGui::TableSetupColumn("RAT");
            ImGui::TableSetupColumn("KD");
            ImGui::TableSetupColumn("ROLE");
            ImGui::TableSetupColumn("GOAT SCORE");
            ImGui::TableHeadersRow();
            int rank = 1, idx = 0;
            for (auto& r : rows) {
                if (!r.player) continue;
                ImGui::PushID(idx++);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(placement_color(rank)));
                ImGui::Text("#%d", rank);
                ImGui::PopStyleColor();
                ImGui::TableNextColumn(); CountryFlag(r.player->country_iso);
                ImGui::TableNextColumn();
                if (ImGui::Selectable(player_label(*r.player).c_str(), false,
                        ImGuiSelectableFlags_SpanAllColumns)) {
                    OpenPlayerModal(s, r.player);
                }
                ImGui::TableNextColumn(); ImGui::Text("%d", r.season);
                ImGui::TableNextColumn(); ImGui::Text("%s", r.season_team.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%.2f", r.season_rating);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", r.season_kd);
                ImGui::TableNextColumn();
                PositionBadge(vlr::position_of(*r.player));
                if (r.player->is_igl) { ImGui::SameLine(); IglBadge(); }
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                ImGui::Text("%.1f", r.score);
                ImGui::PopStyleColor();
                ImGui::PopID();
                ++rank;
            }
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }

    // ---- Formula Lab: edit the weights live ----
    if (ImGui::BeginTabItem("Formula Lab")) {
        H2("EDIT GOAT WEIGHTS");
        Sub("Tweak the coefficients that go into the GOAT score. Bigger value "
            "= bigger contribution to the ranking. Other tabs update live.");
        ImGui::Spacing();

        auto& w = s.goat_weights;
        auto slider = [&](const char* lbl, double& v, double lo, double hi, const char* fmt = "%.3f") {
            float fv = (float)v;
            ImGui::SetNextItemWidth(360);
            if (ImGui::SliderFloat(lbl, &fv, (float)lo, (float)hi, fmt)) v = fv;
        };

        H2("CHAMPIONSHIPS");
        slider("World Champion (per title)",   w.champ_world,    0.0, 100.0, "%.1f");
        slider("Masters Champion (per title)", w.champ_masters,  0.0, 60.0,  "%.1f");
        slider("Regional Champion (per title)",w.champ_regional, 0.0, 30.0,  "%.1f");

        ImGui::Spacing();
        H2("INDIVIDUAL AWARDS");
        slider("MVP award",                    w.mvp_award,      0.0, 60.0,  "%.1f");
        slider("Role of the Year",             w.role_award,     0.0, 40.0,  "%.1f");
        slider("IGL of the Year",              w.igl_award,      0.0, 40.0,  "%.1f");

        ImGui::Spacing();
        H2("PRO PLAY");
        slider("Per career match",             w.per_match,        0.0, 0.5,  "%.3f");
        slider("Avg rating bonus (rating-1.0)*x", w.avg_rating_bonus, 0.0, 200.0, "%.1f");
        slider("Peak rating bonus",            w.peak_rating_bonus,0.0, 120.0, "%.1f");
        slider("Per kill",                     w.per_kill,         0.0, 0.05, "%.4f");
        slider("Per assist",                   w.per_assist,       0.0, 0.05, "%.4f");
        slider("Per first blood",              w.per_first_blood,  0.0, 0.10, "%.4f");
        slider("Per match-MVP",                w.per_mvp_match,    0.0, 1.0,  "%.3f");

        ImGui::Spacing();
        H2("HOF MILESTONES");
        slider("Bonus: 30+ kill match",        w.bonus_30_kill_match, 0.0, 30.0, "%.1f");
        slider("Bonus: 2.0+ KD match",         w.bonus_2kd_match,     0.0, 30.0, "%.1f");
        slider("Bonus: Grand Final clutch",    w.bonus_gf_clutch,     0.0, 50.0, "%.1f");
        slider("Bonus: Top 20 solo Q ever",    w.bonus_top20_solo,    0.0, 30.0, "%.1f");

        ImGui::Spacing();
        H2("RANKED");
        slider("Per peak-MMR pt above 2000",   w.peak_mmr_above_2k,0.0, 0.05, "%.4f");
        slider("Per solo Q win",               w.per_solo_win,     0.0, 0.05, "%.4f");

        ImGui::Spacing();
        if (ImGui::Button("Reset to defaults", ImVec2(180, 32))) {
            s.goat_weights = vlr::GoatWeights{};
        }
        ImGui::EndTabItem();
    }

    // ---- Phase 1 (Agent C) additions: three new tabs --------------------
    if (ImGui::BeginTabItem("Trophy Room")) {
        DrawTrophyRoomTab(s);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Hall of Records")) {
        DrawHallOfRecordsTab(s);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Greatest Seasons")) {
        DrawGreatestSeasonsTab(s);
        ImGui::EndTabItem();
    }

    // === C8: Frivolities (5 "fun stats" sub-views) ========================
    // Each sub-view is a sortable table backed by existing accessors.
    // Empty-state strings cover early saves where the data doesn't qualify
    // yet. NO engine edits — everything derived locally from
    // all_players_world(s) + Player::trophy_summary / top_n_seasons /
    // career_max_ovr / potential / is_retired.
    if (ImGui::BeginTabItem("Frivolities")) {
        SectionHeader("FRIVOLITIES",
            "Fun-stat oddities — players who orbit the spotlight without "
            "the obvious trophy haul.",
            kAccent2);
        VSpace(6);

        if (ImGui::BeginTabBar("##friv_tabs")) {
            auto all_friv = all_players_world(s);

            // ----- 1. Best Without a LAN ----------------------------------
            // Top players by career rating who NEVER won [M] Masters or [W]
            // Worlds. Sortable: Name / Team / Career Rating / Trophies /
            // Seasons.
            if (ImGui::BeginTabItem("Best Without a LAN")) {
                struct R {
                    vlr::PlayerPtr p;
                    double rating = 0.0;
                    int trophies = 0;
                    int seasons = 0;
                };
                std::vector<R> rows;
                for (auto& p : all_friv) {
                    if (!p) continue;
                    auto ts = p->trophy_summary();
                    if (ts.masters > 0 || ts.worlds > 0) continue;
                    if (p->career_matches < 30) continue;
                    R r;
                    r.p = p;
                    r.rating = p->avg_match_rating();
                    r.trophies = ts.total_trophies();
                    r.seasons = static_cast<int>(p->top_n_seasons(99).size());
                    rows.push_back(r);
                }
                std::sort(rows.begin(), rows.end(),
                    [](const R& a, const R& b) { return a.rating > b.rating; });
                if (rows.size() > 100) rows.resize(100);
                if (rows.empty()) {
                    MutedText("No qualifying players yet.");
                } else if (ImGui::BeginTable("##fr_bwl", 5,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                        ImGuiTableFlags_BordersInnerV,
                        ImVec2(0, 460))) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("Name");
                    ImGui::TableSetupColumn("Team");
                    ImGui::TableSetupColumn("Career Rating");
                    ImGui::TableSetupColumn("Trophies");
                    ImGui::TableSetupColumn("Seasons");
                    ImGui::TableHeadersRow();
                    int ri = 0;
                    for (auto& r : rows) {
                        ImGui::PushID(ri++);
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        if (ImGui::Selectable(player_label(*r.p).c_str(),
                                false, ImGuiSelectableFlags_SpanAllColumns)) {
                            OpenPlayerModal(s, r.p);
                        }
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(r.p->team_name.c_str());
                        ImGui::TableNextColumn();
                        ImGui::Text("%.2f", r.rating);
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", r.trophies);
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", r.seasons);
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }

            // ----- 2. Youngest MVPs ---------------------------------------
            // Players who won [A] MVP at the youngest ages. We parse the
            // award strings — every MVP line is "[A] MVP YYYY".
            // age_at_win = (YYYY - birth_year) when birth_year known, else
            // skip. Sort: age_at_win ASC, year ASC.
            if (ImGui::BeginTabItem("Youngest MVPs")) {
                struct R {
                    vlr::PlayerPtr p;
                    int age = 0;
                    int year = 0;
                };
                std::vector<R> rows;
                for (auto& p : all_friv) {
                    if (!p || p->birth_year <= 0) continue;
                    for (auto& aw : p->awards) {
                        if (aw.find("[A] MVP ") != 0) continue;
                        // Extract trailing 4-digit year.
                        if (aw.size() < 4) continue;
                        std::string yr_s = aw.substr(aw.size() - 4);
                        bool digits = true;
                        for (char c : yr_s) if (!std::isdigit((unsigned char)c)) {
                            digits = false; break;
                        }
                        if (!digits) continue;
                        int yr = std::atoi(yr_s.c_str());
                        if (yr <= 0) continue;
                        R r;
                        r.p = p;
                        r.year = yr;
                        r.age = yr - p->birth_year;
                        if (r.age < 14 || r.age > 50) continue;
                        rows.push_back(r);
                    }
                }
                std::sort(rows.begin(), rows.end(),
                    [](const R& a, const R& b) {
                        if (a.age != b.age) return a.age < b.age;
                        return a.year < b.year;
                    });
                if (rows.size() > 100) rows.resize(100);
                if (rows.empty()) {
                    MutedText("No MVP winners on file yet.");
                } else if (ImGui::BeginTable("##fr_ym", 4,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                        ImGuiTableFlags_BordersInnerV,
                        ImVec2(0, 460))) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("Name");
                    ImGui::TableSetupColumn("Age at MVP");
                    ImGui::TableSetupColumn("Year");
                    ImGui::TableSetupColumn("Team");
                    ImGui::TableHeadersRow();
                    int ri = 0;
                    for (auto& r : rows) {
                        ImGui::PushID(ri++);
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        if (ImGui::Selectable(player_label(*r.p).c_str(),
                                false, ImGuiSelectableFlags_SpanAllColumns)) {
                            OpenPlayerModal(s, r.p);
                        }
                        ImGui::TableNextColumn();
                        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                        ImGui::Text("%d", r.age);
                        ImGui::PopStyleColor();
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", r.year);
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(r.p->team_name.c_str());
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }

            // ----- 3. Hall of Good ----------------------------------------
            // Retired players who hit 2-3 HoF criteria but NOT 4+. We don't
            // have a public criteria-met accessor, so derive a local "score"
            // from common HoF-style thresholds (see §7 cheat sheet, "HoF
            // criteria"). 4+ -> HoF; 2-3 -> Hall of Good.
            if (ImGui::BeginTabItem("Hall of Good")) {
                struct R {
                    vlr::PlayerPtr p;
                    int score = 0;
                    int retire_year = 0;
                };
                auto score_player = [](const vlr::Player& p,
                                       int& trophies_out,
                                       double& rating_out) -> int {
                    int n = 0;
                    auto ts = p.trophy_summary();
                    trophies_out = ts.total_trophies();
                    rating_out = p.avg_match_rating();
                    if (ts.total_trophies() >= 1)        ++n;
                    if (ts.worlds      >= 1)             ++n;
                    if (ts.mvps        >= 1)             ++n;
                    if (ts.role_awards >= 1)             ++n;
                    if (p.career_matches >= 200)         ++n;
                    if (p.career_kills   >= 5000)        ++n;
                    if (rating_out >= 1.15)              ++n;
                    if (p.career_max_match_kills >= 30)  ++n;
                    if (p.igl_impact_total >= 50.0)      ++n;
                    return n;
                };
                std::vector<R> rows;
                for (auto& p : all_friv) {
                    if (!p || !p->is_retired) continue;
                    // 2026-05-28 audit: require a meaningful career body of
                    // work — short-career flukes (e.g. one Champions run
                    // followed by retirement) were polluting the list. Real
                    // "Hall of Good" should mean a sustained pro tenure that
                    // fell just short of full HoF, not random 2-3 season
                    // careers with one trophy.
                    if (p->career_seasons_played < 5) continue;
                    if (p->career_matches < 150)      continue;
                    int trophies = 0; double rating = 0.0;
                    int n = score_player(*p, trophies, rating);
                    // Exactly 3 criteria (4+ is real HoF; <3 is just "had a
                    // pro career"). Was 2-3; the 2-band let single-criterion
                    // accidents (one MVP + one role award) qualify too easily.
                    if (n != 3) continue;
                    // Career rating floor — Hall of Good should imply at
                    // least a competent pro, not a journeyman who happened
                    // to ride a winning team.
                    if (rating < 1.00) continue;
                    R r;
                    r.p = p;
                    r.score = n;
                    r.retire_year = 0;
                    // Last career_timeline entry of category "retirement"
                    // gives the retire year (best-effort).
                    for (auto& ev : p->career_timeline()) {
                        if (ev.category == "retirement") r.retire_year = ev.year;
                    }
                    rows.push_back(r);
                }
                std::sort(rows.begin(), rows.end(),
                    [](const R& a, const R& b) {
                        if (a.score != b.score) return a.score > b.score;
                        return a.p->avg_match_rating() > b.p->avg_match_rating();
                    });
                if (rows.size() > 100) rows.resize(100);
                if (rows.empty()) {
                    MutedText("No retirees on the border-of-greatness list yet.");
                } else if (ImGui::BeginTable("##fr_hog", 5,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                        ImGuiTableFlags_BordersInnerV,
                        ImVec2(0, 460))) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("Name");
                    ImGui::TableSetupColumn("Criteria Met");
                    ImGui::TableSetupColumn("Career Rating");
                    ImGui::TableSetupColumn("Trophies");
                    ImGui::TableSetupColumn("Retired Year");
                    ImGui::TableHeadersRow();
                    int ri = 0;
                    for (auto& r : rows) {
                        ImGui::PushID(ri++);
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        if (ImGui::Selectable(player_label(*r.p).c_str(),
                                false, ImGuiSelectableFlags_SpanAllColumns)) {
                            OpenPlayerModal(s, r.p);
                        }
                        ImGui::TableNextColumn();
                        ImGui::Text("%d / 9", r.score);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.2f", r.p->avg_match_rating());
                        ImGui::TableNextColumn();
                        ImGui::Text("%d",
                            r.p->trophy_summary().total_trophies());
                        ImGui::TableNextColumn();
                        if (r.retire_year > 0) ImGui::Text("%d", r.retire_year);
                        else ImGui::TextDisabled("\xE2\x80\x94");
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }

            // ----- 4. Biggest Busts ---------------------------------------
            // career_max_ovr < potential - 15 AND (is_retired OR age >= 28).
            // Reason chip: derived from consistency / work_ethic / age
            // heuristics. Sort by gap DESC.
            if (ImGui::BeginTabItem("Biggest Busts")) {
                struct R {
                    vlr::PlayerPtr p;
                    int gap = 0;
                    const char* reason = "Inconsistent";
                };
                std::vector<R> rows;
                for (auto& p : all_friv) {
                    if (!p) continue;
                    if (p->potential <= 0 || p->career_max_ovr <= 0) continue;
                    int gap = p->potential - p->career_max_ovr;
                    if (gap <= 15) continue;
                    if (!p->is_retired && p->age < 28) continue;
                    R r;
                    r.p = p;
                    r.gap = gap;
                    if (p->work_ethic < 50)      r.reason = "Off-meta";
                    else if (p->consistency < 50) r.reason = "Inconsistent";
                    else                          r.reason = "Injured";
                    rows.push_back(r);
                }
                std::sort(rows.begin(), rows.end(),
                    [](const R& a, const R& b) { return a.gap > b.gap; });
                if (rows.size() > 100) rows.resize(100);
                if (rows.empty()) {
                    MutedText("No qualifying players yet (need POT - peak >= 16).");
                } else if (ImGui::BeginTable("##fr_bb", 5,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                        ImGuiTableFlags_BordersInnerV,
                        ImVec2(0, 460))) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("Name");
                    ImGui::TableSetupColumn("POT");
                    ImGui::TableSetupColumn("Peak OVR");
                    ImGui::TableSetupColumn("Gap");
                    ImGui::TableSetupColumn("Reason");
                    ImGui::TableHeadersRow();
                    int ri = 0;
                    for (auto& r : rows) {
                        ImGui::PushID(ri++);
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        if (ImGui::Selectable(player_label(*r.p).c_str(),
                                false, ImGuiSelectableFlags_SpanAllColumns)) {
                            OpenPlayerModal(s, r.p);
                        }
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", r.p->potential);
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", r.p->career_max_ovr);
                        ImGui::TableNextColumn();
                        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
                        ImGui::Text("%d", r.gap);
                        ImGui::PopStyleColor();
                        ImGui::TableNextColumn();
                        Pill(r.reason, kAccent2);
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }

            // ----- 5. Best Single-Season Performances Without a Title -----
            // Top single-season ratings whose awards_that_year contains no
            // [W]/[M]/[T] trophy line — the team didn't win a championship
            // that year (proxy via the player's awards snapshot).
            if (ImGui::BeginTabItem("Seasons Without a Title")) {
                struct R {
                    vlr::PlayerPtr p;
                    int year = 0;
                    double rating = 0.0;
                    std::string team_name;
                    std::string finish;
                };
                std::vector<R> rows;
                for (auto& p : all_friv) {
                    if (!p) continue;
                    for (auto& h : p->top_n_seasons(3)) {
                        bool has_title = false;
                        for (auto& aw : h.awards_that_year) {
                            if (aw.find("[T] ") == 0
                                || aw.find("[M] ") == 0
                                || aw.find("[W] ") == 0) {
                                has_title = true; break;
                            }
                        }
                        if (has_title) continue;
                        R r;
                        r.p = p;
                        r.year = h.year;
                        r.rating = h.rating;
                        r.team_name = h.team_name;
                        r.finish = "(no title)";
                        rows.push_back(r);
                    }
                }
                std::sort(rows.begin(), rows.end(),
                    [](const R& a, const R& b) {
                        return a.rating > b.rating;
                    });
                if (rows.size() > 100) rows.resize(100);
                if (rows.empty()) {
                    MutedText("No qualifying seasons yet.");
                } else if (ImGui::BeginTable("##fr_swt", 5,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                        ImGuiTableFlags_BordersInnerV,
                        ImVec2(0, 460))) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("Name");
                    ImGui::TableSetupColumn("Season");
                    ImGui::TableSetupColumn("Rating");
                    ImGui::TableSetupColumn("Team");
                    ImGui::TableSetupColumn("Finish");
                    ImGui::TableHeadersRow();
                    int ri = 0;
                    for (auto& r : rows) {
                        ImGui::PushID(ri++);
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        if (ImGui::Selectable(player_label(*r.p).c_str(),
                                false, ImGuiSelectableFlags_SpanAllColumns)) {
                            OpenPlayerModal(s, r.p);
                        }
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", r.year);
                        ImGui::TableNextColumn();
                        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGreen));
                        ImGui::Text("%.2f", r.rating);
                        ImGui::PopStyleColor();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(r.team_name.c_str());
                        ImGui::TableNextColumn();
                        MutedText("%s", r.finish.c_str());
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
}

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
                    ImGui::Text("#%d", rank);
                    ImGui::PopStyleColor();
                    ImGui::TableNextColumn(); CountryFlag(p->country_iso);
                    ImGui::TableNextColumn();
                    if (ImGui::Selectable(player_label(*p).c_str(), false,
                            ImGuiSelectableFlags_SpanAllColumns)) {
                        OpenPlayerModal(s, p);
                    }
                    ImGui::TableNextColumn();
                    PositionBadge(vlr::position_of(*p));
                    if (p->is_igl) { ImGui::SameLine(); IglBadge(); }
                    ImGui::TableNextColumn(); ImGui::Text("%s", p->team_name.c_str());
                    ImGui::TableNextColumn(); ImGui::Text("%d", p->career_matches);
                    ImGui::TableNextColumn();
                    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                    ImGui::Text("%s", cat.format(cat.value(*p)).c_str());
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
void DrawManager(AppState& s) {
    SectionHeader("MANAGER",
        "Run your organization — player development, head coach, and "
        "finances all in one place.",
        kAccent);
    VSpace(6);

    auto& t = s.gm.user_team;
    if (!t) { ImGui::TextDisabled("(no user team selected)"); return; }

    ImGui::BeginTabBar("##manager_tabs");

    // ===== PLAYER DEVELOPMENT =====
    if (ImGui::BeginTabItem("Player Development")) {
        H2("MONTHLY DEVELOPMENT REPORTS");
        Sub("Every 30 in-game days each player runs a development pass. "
            "Pros grow based on match performance + coaching; FAs grow from "
            "ranked performance + natural age curve.");
        ImGui::Spacing();

        auto& list = s.gm.recent_progression_reports;
        if (list.empty()) {
            ImGui::TextDisabled("(no recent monthly progression — advance ~30 days)");
        } else if (ImGui::BeginTable("##mgr_prog", 6,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, 280))) {
            ImGui::TableSetupColumn("PLAYER");
            ImGui::TableSetupColumn("TYPE");
            ImGui::TableSetupColumn("MATCHES");
            ImGui::TableSetupColumn("AVG RAT");
            ImGui::TableSetupColumn("CHANGES");
            ImGui::TableSetupColumn("EXPLANATION", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            int idx = 0;
            for (auto& r : list) {
                ImGui::PushID(idx++);
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%s", r.player_name.c_str());
                ImGui::TableNextColumn();
                ImGui::TextColored(toV4(r.was_pro ? kVlrGreen : kVlrBlue),
                                   "%s", r.was_pro ? "PRO" : "FA");
                ImGui::TableNextColumn();
                if (r.was_pro) ImGui::Text("%d", r.matches_in_window);
                else           ImGui::Text("%d ranked", r.ranked_played);
                ImGui::TableNextColumn();
                if (r.matches_in_window > 0) ImGui::Text("%.2f", r.avg_rating);
                else                          ImGui::Text("-");
                ImGui::TableNextColumn();
                if (r.changes.empty()) {
                    ImGui::TextDisabled("-");
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
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        ImGui::Spacing();
        H2("ROSTER FORM TRACKER");
        Sub("Snapshot of each starter's form, age, and trajectory. Players "
            "below 0.95 rating may attract mid-season replacement attention "
            "from strict coaches.");

        if (ImGui::BeginTable("##mgr_form", 8,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV,
                ImVec2(-FLT_MIN, 220))) {
            ImGui::TableSetupColumn("PLAYER", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("AGE");
            ImGui::TableSetupColumn("OVR");
            ImGui::TableSetupColumn("POT");
            ImGui::TableSetupColumn("AVG RAT");
            ImGui::TableSetupColumn("SEASON RAT");
            ImGui::TableSetupColumn("FORM");
            ImGui::TableSetupColumn("AT RISK");
            ImGui::TableHeadersRow();
            int ri = 0;
            for (auto& p : t->roster) {
                if (!p) continue;
                ImGui::PushID(ri++);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::Selectable(player_label(*p).c_str(), false,
                        ImGuiSelectableFlags_SpanAllColumns)) {
                    OpenPlayerModal(s, p);
                }
                ImGui::TableNextColumn(); ImGui::Text("%d", p->age);
                ImGui::TableNextColumn(); ImGui::Text("%.0f", p->ovr());
                ImGui::TableNextColumn(); ImGui::Text("%d", p->potential);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", p->avg_match_rating());
                double srat = p->season_matches > 0
                    ? p->season_rating_total / p->season_matches : 0.0;
                ImGui::TableNextColumn();
                if (p->season_matches > 0) ImGui::Text("%.2f", srat);
                else                       ImGui::TextDisabled("-");
                ImGui::TableNextColumn();
                {
                    const char* form = "STEADY";
                    ImU32 col = kVlrText;
                    if (srat >= 1.20)      { form = "HOT";    col = kVlrGreen; }
                    else if (srat >= 1.05) { form = "GOOD";   col = kVlrText;  }
                    else if (srat >= 0.95) { form = "STEADY"; col = kVlrText;  }
                    else if (srat > 0.0)   { form = "COLD";   col = kVlrRed;   }
                    else                   { form = "-";      col = kVlrSub;   }
                    ImGui::PushStyleColor(ImGuiCol_Text, toV4(col));
                    ImGui::Text("%s", form);
                    ImGui::PopStyleColor();
                }
                ImGui::TableNextColumn();
                bool at_risk = (p->season_matches >= 4 && srat < 0.95);
                if (at_risk) {
                    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
                    ImGui::TextUnformatted("YES");
                    ImGui::PopStyleColor();
                } else { ImGui::TextDisabled("no"); }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        if (s.god_mode) {
            ImGui::Spacing();
            H2("HIDDEN: PEAK ARCHETYPES (god mode)");
            Sub("Internal aging curve assignments. Players don't know which "
                "archetype they have; coaches infer it over time.");
            if (ImGui::BeginTable("##mgr_arch", 4, ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("PLAYER", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("ARCHETYPE");
                ImGui::TableSetupColumn("PEAK AGE");
                ImGui::TableSetupColumn("WORK ETHIC");
                ImGui::TableHeadersRow();
                for (auto& p : t->roster) {
                    if (!p) continue;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::Text("%s", p->name.c_str());
                    ImGui::TableNextColumn();
                    const char* a = "Standard";
                    switch (p->growth_archetype) {
                        case vlr::GrowthArchetype::EarlyPeaker: a = "Early Peaker"; break;
                        case vlr::GrowthArchetype::LateBloomer: a = "Late Bloomer"; break;
                        case vlr::GrowthArchetype::Standard:    a = "Standard";     break;
                    }
                    ImGui::Text("%s", a);
                    ImGui::TableNextColumn(); ImGui::Text("%d", p->peak_age);
                    ImGui::TableNextColumn(); ImGui::Text("%d", p->work_ethic);
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndTabItem();
    }

    // ===== HEAD COACH MANAGEMENT =====
    if (ImGui::BeginTabItem("Head Coach")) {
        if (!t->head_coach) {
            ImGui::TextDisabled("No coach signed.");
        } else {
            auto& c = t->head_coach;
            if (s.god_mode) {
                char nm[128];
                std::snprintf(nm, sizeof(nm), "%s", c->name.c_str());
                ImGui::SetNextItemWidth(360);
                if (ImGui::InputText("##mgr_coach_name", nm, sizeof(nm))) c->name = nm;
            } else {
                H2(c->name.c_str());
            }
            CountryFlag(c->country_iso); ImGui::SameLine();
            Sub("%s   |   age %d   |   $%dK/yr   |   %s",
                c->country.empty() ? c->region.c_str() : c->country.c_str(),
                c->age, c->salary_k,
                vlr::coach_personality_name(c->personality));
            ImGui::PushFont(g_font_small);
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
            ImGui::Text("Style: %s", vlr::coach_personality_blurb(c->personality));
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::Spacing();

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

            ImGui::Spacing();
            H2("CONTRACT");
            int years_left = c->contract_exp_year > 0
                ? std::max(0, c->contract_exp_year - s.gm.year + 1)
                : c->contract_years;
            ImGui::Text("Years remaining: %d   |   Expires after season %d",
                        years_left,
                        c->contract_exp_year > 0 ? c->contract_exp_year : (s.gm.year + years_left - 1));
            if (s.god_mode) {
                ImGui::Text("Edit contract length:");
                int yrs_in = years_left;
                ImGui::SetNextItemWidth(160);
                if (ImGui::SliderInt("##coach_contract_years", &yrs_in, 0, 8, "%d yrs")) {
                    c->contract_years = std::max(0, yrs_in);
                    c->contract_exp_year = s.gm.year + c->contract_years - 1;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("+1 yr")) {
                    c->contract_years = std::max(1, years_left + 1);
                    c->contract_exp_year = s.gm.year + c->contract_years - 1;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("-1 yr")) {
                    c->contract_years = std::max(0, years_left - 1);
                    c->contract_exp_year = s.gm.year + c->contract_years - 1;
                }
            }
            ImGui::Spacing();
            H2("DERIVED EFFECTS");
            ImGui::Text("Match synergy boost: +%.1f%%", (c->match_synergy_mult() - 1.0) * 100.0);
            ImGui::Text("Annual dev chance:   %.2fx",   c->dev_chance_mult());
            ImGui::Text("Asking salary now:   $%dK/yr", c->requested_salary_k());
            ImGui::Text("Replacement aggression: %.0f%%",
                vlr::personality_replacement_aggressiveness(c->personality) * 100.0);

            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Button, toV4(kVlrRed));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(kVlrRedDim));
            if (ImGui::Button("Fire Coach", ImVec2(140, 32))) {
                t->head_coach.reset();
                s.log.emplace_back("Fired head coach. New coach hired from market at year-end.");
            }
            ImGui::PopStyleColor(2);
        }

        ImGui::Spacing();
        H2("COACH MARKET");
        Sub("Available free-agent coaches. Hire one to replace your current "
            "head coach immediately.");
        // Cached market in AppState (regenerate on demand). Use a static
        // local vector tied to the year so it refreshes once a year.
        static std::vector<vlr::CoachPtr> market;
        static int last_market_year = -1;
        if (last_market_year != s.gm.year) {
            market = vlr::generate_coach_market(6);
            last_market_year = s.gm.year;
        }
        if (ImGui::Button("Refresh Market", ImVec2(140, 28))) {
            market = vlr::generate_coach_market(6);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Refreshes annually + on demand");
        if (ImGui::BeginTable("##mgr_market", 8,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, 280))) {
            ImGui::TableSetupColumn("FLAG");
            ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("REGION");
            ImGui::TableSetupColumn("AGE");
            ImGui::TableSetupColumn("PERSONALITY");
            ImGui::TableSetupColumn("OVR");
            ImGui::TableSetupColumn("ASK $K/yr");
            ImGui::TableSetupColumn("HIRE");
            ImGui::TableHeadersRow();
            int mi = 0;
            for (auto& c : market) {
                if (!c) continue;
                ImGui::PushID(mi++);
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); CountryFlag(c->country_iso);
                ImGui::TableNextColumn(); ImGui::Text("%s", c->name.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%s", c->region.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%d", c->age);
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                ImGui::Text("%s", vlr::coach_personality_name(c->personality));
                ImGui::PopStyleColor();
                int ovr = (c->tactical + c->development + c->leadership + c->experience) / 4;
                ImGui::TableNextColumn(); ImGui::Text("%d", ovr);
                ImGui::TableNextColumn(); ImGui::Text("$%d", c->requested_salary_k());
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("Hire")) {
                    long long cost = static_cast<long long>(c->requested_salary_k()) * 1000LL;
                    if (t->budget < cost) {
                        s.log.emplace_back("Cannot afford coach " + c->name + ".");
                    } else {
                        c->salary_k = c->requested_salary_k();
                        c->team_name = t->name;
                        if (c->contract_years <= 0) c->contract_years = 2;
                        c->contract_exp_year = s.gm.year + c->contract_years - 1;
                        t->head_coach = c;
                        s.log.emplace_back("Hired " + c->name + " as head coach.");
                    }
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }

    // ===== FINANCE =====
    if (ImGui::BeginTabItem("Finance")) {
        H2("ORG FINANCES");
        Sub("Budget, salary commitments, and revenue snapshot. Year-end "
            "payroll subtracts total salaries from your budget; sponsorships "
            "and prize winnings refill it.");
        ImGui::Spacing();

        long long budget = t->budget;
        int payroll = t->total_payroll_k();
        int player_salaries = 0;
        for (auto& p : t->roster) if (p) player_salaries += p->contract.amount_k;
        int coach_salary = t->head_coach ? t->head_coach->salary_k : 0;

        // Estimated annual revenue from sponsorships scales with regional
        // standing (better standing -> better deals). Prize winnings are
        // tournament titles already accounted for during championship plays.
        int est_sponsorship = 800 + std::max(0, t->wins) * 25;
        int est_revenue_k = est_sponsorship + 100;  // base merch/streaming

        StatTile("BUDGET", fmt_money(budget), kVlrGreen);
        ImGui::SameLine();
        {
            char b[32]; std::snprintf(b, sizeof(b), "$%dK", payroll);
            StatTile("ANNUAL PAYROLL", b, kAccent);
        }
        ImGui::SameLine();
        {
            char b[32]; std::snprintf(b, sizeof(b), "$%dK", est_revenue_k);
            StatTile("EST. REVENUE", b, kVlrGold);
        }
        ImGui::SameLine();
        {
            char b[32]; int net = est_revenue_k - payroll;
            std::snprintf(b, sizeof(b), "$%dK", net);
            StatTile("NET", b, net >= 0 ? kVlrGreen : kVlrRed);
        }

        ImGui::Spacing();
        ImGui::Spacing();

        H2("SALARY BREAKDOWN");
        if (ImGui::BeginTable("##mgr_salary", 6,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV,
                ImVec2(-FLT_MIN, 240))) {
            ImGui::TableSetupColumn("PLAYER", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("ROLE");
            ImGui::TableSetupColumn("AGE");
            ImGui::TableSetupColumn("OVR");
            ImGui::TableSetupColumn("SALARY $K");
            ImGui::TableSetupColumn("EXP YEAR");
            ImGui::TableHeadersRow();
            int fi = 0;
            for (auto& p : t->roster) {
                if (!p) continue;
                ImGui::PushID(fi++);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::Selectable(player_label(*p).c_str(), false,
                        ImGuiSelectableFlags_SpanAllColumns)) {
                    OpenPlayerModal(s, p);
                }
                ImGui::TableNextColumn();
                PositionBadge(vlr::position_of(*p));
                if (p->is_igl) { ImGui::SameLine(); IglBadge(); }
                ImGui::TableNextColumn(); ImGui::Text("%d", p->age);
                ImGui::TableNextColumn(); ImGui::Text("%.0f", p->ovr());
                ImGui::TableNextColumn(); ImGui::Text("$%d", p->contract.amount_k);
                ImGui::TableNextColumn(); ImGui::Text("%d", p->contract.exp_year);
                ImGui::PopID();
            }
            if (t->head_coach) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextColored(toV4(kVlrGold), "%s (Coach)", t->head_coach->name.c_str());
                ImGui::TableNextColumn(); ImGui::TextDisabled("-");
                ImGui::TableNextColumn(); ImGui::Text("%d", t->head_coach->age);
                ImGui::TableNextColumn(); ImGui::TextDisabled("-");
                ImGui::TableNextColumn(); ImGui::Text("$%d", t->head_coach->salary_k);
                ImGui::TableNextColumn(); ImGui::TextDisabled("-");
            }
            ImGui::EndTable();
        }

        ImGui::Spacing();
        H2("INVESTMENTS");
        Sub("(Future: facility upgrades, scouting network, analyst staff. "
            "Currently each costs $K from budget for a small recurring "
            "performance bonus.)");
        ImGui::TextDisabled("Player salaries:   $%dK/yr", player_salaries);
        ImGui::TextDisabled("Coach salary:      $%dK/yr", coach_salary);
        ImGui::TextDisabled("Total commitments: $%dK/yr", payroll);
        ImGui::EndTabItem();
    }

    // ===== COMMISSIONER (god-mode only) =====
    if (s.god_mode && ImGui::BeginTabItem("Commissioner")) {
        H2("LEAGUE-WIDE EDITOR");
        Sub("God-mode commissioner panel: edit any team's finances, "
            "prestige, sponsorship, strategy, and roster. Changes take "
            "effect immediately.");
        ImGui::Spacing();

        // Team selection by region
        const char* regions[] = {"Americas", "EMEA", "Pacific"};
        static int reg_idx = 0;
        ImGui::SetNextItemWidth(160);
        ImGui::Combo("Region##commreg", &reg_idx, regions, 3);

        auto rit = s.gm.leagues.find(regions[reg_idx]);
        if (rit == s.gm.leagues.end()) { ImGui::EndTabItem(); ImGui::EndTabBar(); return; }
        auto& league = rit->second;

        if (ImGui::BeginTable("##comm_teams", 9,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, 540))) {
            ImGui::TableSetupColumn("FLAG");
            ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("TAG");
            ImGui::TableSetupColumn("BUDGET ($K)");
            ImGui::TableSetupColumn("PRESTIGE");
            ImGui::TableSetupColumn("SPONSOR ($K)");
            ImGui::TableSetupColumn("STRATEGY");
            ImGui::TableSetupColumn("OVR");
            ImGui::TableSetupColumn("PAYROLL");
            ImGui::TableHeadersRow();
            int ti = 0;
            for (auto& t : league->teams()) {
                if (!t) continue;
                ImGui::PushID(ti++);
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); CountryFlag(t->home_country_iso);
                ImGui::TableNextColumn();
                if (ImGui::Selectable(t->name.c_str(), false,
                        ImGuiSelectableFlags_SpanAllColumns)) {
                    OpenTeamProfile(s, t);
                }
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                ImGui::Text("%s", t->tag.c_str());
                ImGui::PopStyleColor();
                ImGui::TableNextColumn();
                int bud_k = (int)(t->budget / 1000LL);
                ImGui::SetNextItemWidth(110);
                if (ImGui::DragInt("##bud", &bud_k, 25.0f, -10000, 100000)) {
                    t->budget = (long long)bud_k * 1000LL;
                }
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(80);
                ImGui::DragInt("##prst", &t->prestige, 1.0f, 0, 99);
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(110);
                ImGui::DragInt("##spon", &t->sponsorship_k, 25.0f, 0, 9999);
                ImGui::TableNextColumn();
                int strat_idx = static_cast<int>(t->strategy);
                const char* strats[] = {"Contender", "Rebuilding", "Bridge",
                                         "BudgetRoster", "DevelopmentFocus",
                                         "WinNow", "TalentFarm"};
                ImGui::SetNextItemWidth(140);
                if (ImGui::Combo("##strat", &strat_idx, strats, 7)) {
                    t->strategy = static_cast<vlr::Team::Strategy>(strat_idx);
                }
                ImGui::TableNextColumn(); ImGui::Text("%.0f", t->ovr());
                ImGui::TableNextColumn(); ImGui::Text("$%dK", t->total_payroll_k());
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        ImGui::Spacing();
        H2("BULK ACTIONS");
        if (ImGui::Button("Reset all budgets to $1500K", ImVec2(220, 28))) {
            for (auto& kv : s.gm.leagues) {
                for (auto& t : kv.second->teams()) {
                    if (t) t->budget = 1'500'000LL;
                }
            }
            s.log.emplace_back("[CMSR] All team budgets reset to $1.5M");
        }
        ImGui::SameLine();
        if (ImGui::Button("Re-classify all strategies", ImVec2(220, 28))) {
            int n = 0;
            for (auto& kv : s.gm.leagues) {
                for (auto& t : kv.second->teams()) {
                    if (!t) continue;
                    t->strategy = vlr::classify_team_strategy(*t);
                    ++n;
                }
            }
            char buf[64]; std::snprintf(buf, sizeof(buf), "[CMSR] Re-classified %d teams.", n);
            s.log.emplace_back(buf);
        }
        ImGui::SameLine();
        if (ImGui::Button("+$1M budget to user team", ImVec2(220, 28))) {
            if (s.gm.user_team) s.gm.user_team->budget += 1'000'000LL;
        }
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
}

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

    // Category filter
    static int filter = 0;
    const char* filters[] = {"All", "Roster Move", "Awards", "MVP Race", "Coaching", "Retirement"};
    ImGui::SetNextItemWidth(180);
    ImGui::Combo("Filter", &filter, filters, IM_ARRAYSIZE(filters));
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
        if (filter > 0 && item.category != filters[filter]) continue;
        ImGui::PushID(ni++);

        // Color-coded category: Awards=gold, Tournament=accent,
        // Roster Move=purple, Retirement=red, MVP Race=green.
        ImU32 cat_col = kVlrSub;
        if      (item.category == "Roster Move") cat_col = kAccent2;
        else if (item.category == "Awards")      cat_col = kVlrGold;
        else if (item.category == "MVP Race")    cat_col = kVlrGreen;
        else if (item.category == "Coaching")    cat_col = kAccent;
        else if (item.category == "Tournament")  cat_col = kAccent;
        else if (item.category == "Retirement")  cat_col = kVlrRed;

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
                if (ImGui::BeginTabItem("League Leaders")) {
                    DrawLeagueLeaders(s); ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Compare")) {
                    DrawCompare(s); ImGui::EndTabItem();
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
                      r.day_in_year, r.user_match_opp_name.c_str());
        s.log.emplace_back(b);
    } else if (r.user_match_recording) {
        OpenReplay(s, r.user_match_recording);
        char b[160];
        std::snprintf(b, sizeof(b), "[Day %d] Match day vs %s — opening live viewer.",
                      r.day_in_year, r.user_match_opp_name.c_str());
        s.log.emplace_back(b);
    }
    if (r.progression_ran) {
        char b[128];
        std::snprintf(b, sizeof(b), "[Day %d] Monthly player development pass.", r.day_in_year);
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
        ImU32 col = IM_COL32(0, 0xD4, 0xFF, alpha);
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
            ImGui::GetWindowDrawList()->AddRectFilled(
                chip_origin,
                ImVec2(chip_origin.x + chip_w, chip_origin.y + chip_h),
                kSurfaceAlt, 7.0f);
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
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
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
    auto navbtn = [&](const char* label, Screen target) {
        bool selected = (s.screen == target);
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        float h = 32.0f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (selected) {
            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), kSurfaceHi, 7.0f);
            dl->AddRectFilled(p, ImVec2(p.x + 3.0f, p.y + h), kAccent,
                              2.0f, ImDrawFlags_RoundCornersLeft);
        }
        ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              selected ? kSurfaceHi : kSurfaceAlt);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kSurfaceHi);
        ImGui::PushStyleColor(ImGuiCol_Text,
                              toV4(selected ? kAccent : kVlrSub));
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
        if (ImGui::Button(label, ImVec2(-FLT_MIN, h))) s.screen = target;
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
        ImGui::Dummy(ImVec2(0, 2));
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
            dl->AddRectFilled(p, ImVec2(p.x + 3.0f, p.y + h), kAccent,
                              2.0f, ImDrawFlags_RoundCornersLeft);
        }
        ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              selected ? kSurfaceHi : kSurfaceAlt);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kSurfaceHi);
        ImGui::PushStyleColor(ImGuiCol_Text,
                              toV4(selected ? kAccent : kVlrSub));
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
        if (ImGui::Button(label, ImVec2(-FLT_MIN, h))) s.screen = target;
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
    };

    navgroup("NAVIGATION");
    navbtn_group("Home",        Screen::GroupHome,
                 {Screen::Dashboard, Screen::News});
    navbtn_group("Team",        Screen::GroupTeam,
                 {Screen::Roster, Screen::Manager, Screen::Calendar});
    navbtn_group("Competition", Screen::GroupCompetition,
                 {Screen::Standings, Screen::Brackets,
                  Screen::PowerRankings, Screen::TeamProfile});
    navbtn_group("People",      Screen::GroupPeople,
                 {Screen::Market, Screen::SoloQ,
                  Screen::LeagueLeaders, Screen::Compare});
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
        ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kSurfaceAlt);
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
    if (cat == "award")      return kVlrBlue;
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
            ImGui::Text("%d", ev.year);
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
                ImGui::Text("#%d", rk);
                ImGui::PopStyleColor();
                ImGui::TableNextColumn(); CountryFlag(r.second->country_iso);
                ImGui::TableNextColumn();
                if (ImGui::Selectable(player_label(*r.second).c_str(), false,
                        ImGuiSelectableFlags_SpanAllColumns)) {
                    OpenPlayerModal(s, r.second);
                }
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
                ImGui::TextUnformatted(r.second->team_name.c_str());
                ImGui::PopStyleColor();
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                if (r.first >= 100.0) ImGui::Text("%.0f", r.first);
                else                  ImGui::Text("%.2f", r.first);
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
        ImGui::Text("#%d", rk);
        ImGui::PopStyleColor();
        ImGui::TableNextColumn();
        if (ImGui::Selectable(player_label(*r.player).c_str(), false,
                ImGuiSelectableFlags_SpanAllColumns)) {
            OpenPlayerModal(s, r.player);
        }
        ImGui::TableNextColumn(); ImGui::Text("%d", r.hi.year);
        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
        ImGui::TextUnformatted(r.hi.team_name.c_str());
        ImGui::PopStyleColor();
        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
        ImGui::Text("%.2f", r.hi.rating);
        ImGui::PopStyleColor();
        ImGui::TableNextColumn(); ImGui::Text("%d", r.hi.matches);
        ImGui::TableNextColumn();
        ImGui::Text("%d-%d-%d", r.hi.kills, r.hi.deaths, r.hi.assists);
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
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrBlue));
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
            if (ImGui::Selectable(lbl.c_str(), selected)) {
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
    ImGui::TableNextColumn(); ImGui::Text("%d", a->age);
    ImGui::TableNextColumn(); ImGui::Text("%d", b->age);

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
    ImGui::TableNextColumn(); ImGui::Text("%.2f", a->avg_match_rating());
    ImGui::TableNextColumn(); ImGui::Text("%.2f", b->avg_match_rating());

    cmp_row_label("Career K-D-A");
    ImGui::TableNextColumn();
    ImGui::Text("%d-%d-%d", a->career_kills, a->career_deaths, a->career_assists);
    ImGui::TableNextColumn();
    ImGui::Text("%d-%d-%d", b->career_kills, b->career_deaths, b->career_assists);

    cmp_row_label("Career Matches");
    ImGui::TableNextColumn(); ImGui::Text("%d", a->career_matches);
    ImGui::TableNextColumn(); ImGui::Text("%d", b->career_matches);

    cmp_row_label("Peak Season Rating");
    ImGui::TableNextColumn(); ImGui::Text("%.2f", bs_a.rating);
    ImGui::TableNextColumn(); ImGui::Text("%.2f", bs_b.rating);

    cmp_row_label("Total Trophies");
    ImGui::TableNextColumn();
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
    ImGui::Text("%d", ts_a.total_trophies());
    ImGui::PopStyleColor();
    ImGui::TableNextColumn();
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
    ImGui::Text("%d", ts_b.total_trophies());
    ImGui::PopStyleColor();

    cmp_row_label("MVPs");
    ImGui::TableNextColumn(); ImGui::Text("%d", ts_a.mvps);
    ImGui::TableNextColumn(); ImGui::Text("%d", ts_b.mvps);

    cmp_row_label("Role Awards");
    ImGui::TableNextColumn(); ImGui::Text("%d", ts_a.role_awards);
    ImGui::TableNextColumn(); ImGui::Text("%d", ts_b.role_awards);

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
        ImGui::TableNextColumn(); ImGui::Text("%d", h_ab.matches);
        ImGui::TableNextColumn(); ImGui::Text("%d", h_ba.matches);

        cmp_row_label("Wins / Losses");
        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGreen));
        ImGui::Text("%d", h_ab.wins);
        ImGui::PopStyleColor();
        ImGui::SameLine(); ImGui::TextUnformatted(" / ");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
        ImGui::Text("%d", h_ab.losses);
        ImGui::PopStyleColor();

        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGreen));
        ImGui::Text("%d", h_ba.wins);
        ImGui::PopStyleColor();
        ImGui::SameLine(); ImGui::TextUnformatted(" / ");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
        ImGui::Text("%d", h_ba.losses);
        ImGui::PopStyleColor();

        cmp_row_label("Kills for / against");
        ImGui::TableNextColumn();
        ImGui::Text("%d / %d", h_ab.kills_for, h_ab.kills_against);
        ImGui::TableNextColumn();
        ImGui::Text("%d / %d", h_ba.kills_for, h_ba.kills_against);

        cmp_row_label("Avg rating when facing");
        ImGui::TableNextColumn(); ImGui::Text("%.2f", h_ab.avg_rating_when_facing);
        ImGui::TableNextColumn(); ImGui::Text("%.2f", h_ba.avg_rating_when_facing);

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
                        ImGui::Text("%d", f.year);
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", f.day_in_year);
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

    SectionHeader("DIFFICULTY", "Global skill multiplier for the AI", kAccent);
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
        vlr::draw_team_logo(dl, center, size * 0.45f, shape,
                            u32_to_imu32(s.cfg.user_color_primary),
                            u32_to_imu32(s.cfg.user_color_accent));
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
        vlr::draw_team_logo(dl,
                            ImVec2(p0.x + 22, p0.y + row_h * 0.5f),
                            14.f, 0,
                            u32_to_imu32(s.cfg.user_color_primary),
                            u32_to_imu32(s.cfg.user_color_accent));
        std::string tagprev = s.cfg.user_team_name.empty()
            ? std::string("ORG")
            : s.cfg.user_team_name.substr(0, std::min<size_t>(3, s.cfg.user_team_name.size()));
        for (auto& c : tagprev) c = (char)std::toupper((unsigned char)c);
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
