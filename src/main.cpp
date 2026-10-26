#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <process.h>
#include <string>
#include <vector>

#include "config.h"
#include "engine.h"
#include "script_engine.h"
#include "imgui_layer.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

using wfmaxon::ConfigStore;
using wfmaxon::FeatureBinding;
using wfmaxon::NumericBinding;

// Global constants
constexpr wchar_t kWindowClass[] = L"WF_MAXON_MainWindow";
constexpr wchar_t kWindowTitle[] = L"WF_MAXON";
constexpr UINT_PTR kStatusTimer = 1;

constexpr COLORREF RGBX(BYTE r, BYTE g, BYTE b) { return RGB(r, g, b); }

const COLORREF C_BG       = RGBX(14, 17, 20);
const COLORREF C_PANEL    = RGBX(19, 23, 27);
const COLORREF C_CARD     = RGBX(24, 29, 34);
const COLORREF C_CARD_HI  = RGBX(29, 36, 41);
const COLORREF C_BORDER   = RGBX(43, 52, 59);
const COLORREF C_TEXT     = RGBX(235, 240, 242);
const COLORREF C_MUTED    = RGBX(139, 151, 158);
const COLORREF C_ACCENT   = RGBX(42, 225, 185);
const COLORREF C_ACCENT_2 = RGBX(17, 117, 100);
const COLORREF C_WARN     = RGBX(244, 178, 75);
const COLORREF C_DANGER   = RGBX(231, 91, 104);

constexpr int CAT_COMBAT  = 0;
constexpr int CAT_ARSENAL = 1;
constexpr int CAT_WORLD   = 2;
constexpr int CAT_VIEW    = 3;
constexpr int CAT_SCRIPTS = 4;

const wchar_t* kCategoryNames[] = {L"COMBAT", L"ARSENAL", L"WORLD", L"VIEW", L"SCRIPTS"};
const wchar_t* kCategoryDescriptions[] = {
    L"Damage, survival, skills",
    L"Weapons, ammo, Incarnon",
    L"Loot and missions",
    L"Camera",
    L"Lua scripts and hooks"
};

struct UiHit {
    enum Kind { Category, Feature, Numeric, Apply, Browse, MirrorRage, ReloadScripts, ScriptItem, HookItem } kind;
    RECT rect{};
    int index = -1;
};

// Variables and functions shared with ImGui layer
HWND g_hwnd = nullptr;
wfmaxon::ScriptEngine g_scripts;
wfmaxon::LiveEngine g_engine;
ConfigStore g_config;
std::vector<FeatureBinding> g_features;
std::vector<NumericBinding> g_numerics;
bool g_mirror_rage = true;
bool g_dirty = false;
std::wstring g_status = L"READY";
COLORREF g_status_color = C_ACCENT;

void SetStatus(const std::wstring& text, COLORREF color);
bool SaveConfig();
bool ApplyToGame();
void BrowseGameDirectory();
void EnforceExclusive(int changed_index);
int CategoryCount(int category);
int CategoryEnabledCount(int category);
bool IsProcessRunning(const wchar_t* process_name);

namespace {
HMODULE g_self_module = nullptr;
HFONT g_font_title = nullptr;
HFONT g_font_heading = nullptr;
HFONT g_font_body = nullptr;
HFONT g_font_small = nullptr;
HFONT g_font_mono = nullptr;
std::vector<UiHit> g_hits;
int g_category = CAT_COMBAT;
int g_scroll = 0;
int g_scroll_max = 0;
int g_drag_numeric = -1;

RECT R(int l, int t, int r, int b) { RECT out{l, t, r, b}; return out; }

void FillRectColor(HDC dc, const RECT& rc, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rc, brush);
    DeleteObject(brush);
}

void RoundFill(HDC dc, const RECT& rc, COLORREF color, int radius) {
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_NULL, 0, color);
    HGDIOBJ old_brush = SelectObject(dc, brush);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void RoundStroke(HDC dc, const RECT& rc, COLORREF color, int radius, int width = 1) {
    HBRUSH brush = static_cast<HBRUSH>(GetStockObject(HOLLOW_BRUSH));
    HPEN pen = CreatePen(PS_SOLID, width, color);
    HGDIOBJ old_brush = SelectObject(dc, brush);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
}

void Text(HDC dc, const std::wstring& value, RECT rc, HFONT font, COLORREF color,
          UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    HGDIOBJ old = SelectObject(dc, font);
    DrawTextW(dc, value.c_str(), static_cast<int>(value.size()), &rc, flags);
    SelectObject(dc, old);
}

void Text(HDC dc, const wchar_t* value, RECT rc, HFONT font, COLORREF color,
          UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX) {
    Text(dc, value ? std::wstring(value) : std::wstring(), rc, font, color, flags);
}

bool PtIn(const RECT& rc, int x, int y) {
    POINT p{x, y};
    return PtInRect(&rc, p) != FALSE;
}

} // namespace

void SetStatus(const std::wstring& text, COLORREF color) {
    g_status = text;
    g_status_color = color;
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

void BrowseGameDirectory() {
    BROWSEINFOW info{};
    info.hwndOwner = g_hwnd;
    info.lpszTitle = L"Select the Warframe game directory";
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&info);
    if (!pidl) return;
    wchar_t path[MAX_PATH]{};
    if (SHGetPathFromIDListW(pidl, path)) {
        g_config.SetGameDirectory(path);
        g_config.Load(g_features, g_numerics);
        SetStatus(g_config.Exists() ? L"CONFIG LOADED" : L"NEW CONFIG TARGET", C_ACCENT);
        g_dirty = false;
        InvalidateRect(g_hwnd, nullptr, FALSE);
    }
    CoTaskMemFree(pidl);
}

bool ApplyToGame() {
    g_engine.SetGame(g_config.GameDirectory());
    if (!g_engine.DetectGame()) {
        SetStatus(g_engine.Status(), C_WARN);
        return false;
    }
    g_engine.LoadProfile(g_config.IniPath());
    bool ok = g_engine.ApplyAll();
    if (!ok) {
        SetStatus(g_engine.Status(), C_WARN);
        return false;
    }
    SetStatus(g_engine.Status(), C_ACCENT);
    return true;
}

int CategoryCount(int category) {
    int count = 0;
    for (const auto& f : g_features) if (f.category == category) ++count;
    for (const auto& n : g_numerics) if (n.category == category) ++count;
    return count;
}

int CategoryEnabledCount(int category) {
    int count = 0;
    for (const auto& f : g_features) if (f.category == category && f.enabled) ++count;
    return count;
}

void EnforceExclusive(int changed_index) {
    if (changed_index < 0 || changed_index >= static_cast<int>(g_features.size())) return;
    const int group = g_features[changed_index].exclusive_group;
    if (!group || !g_features[changed_index].enabled) return;
    for (int i = 0; i < static_cast<int>(g_features.size()); ++i) {
        if (i != changed_index && g_features[i].exclusive_group == group) {
            g_features[i].enabled = false;
        }
    }
}

bool SaveConfig() {
    if (g_config.GameDirectory().empty()) {
        SetStatus(L"WARFRAME DIRECTORY NOT FOUND", C_DANGER);
        return false;
    }
    bool ok = g_config.Save(g_features, g_numerics, g_mirror_rage);
    if (!ok) {
        SetStatus(L"CONFIG SAVE FAILED", C_DANGER);
        return false;
    }
    g_dirty = false;
    SetStatus(L"SAVED TO PROFILE", C_ACCENT);
    return true;
}

bool IsProcessRunning(const wchar_t* process_name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snap, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, process_name) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return found;
}

namespace {

void InitBindings() {
    g_features = {
        {L"ohk", L"One-Hit Kill", L"Player damage detour. Always-on variant.", L"OneHitKillForMe_Always", CAT_COMBAT, false, true, nullptr, 0},
        {L"skill_mult", L"Skill Mods Multiplier", L"Amplifies ability mod effects using the value below.", L"SkillsModsMultiplier", CAT_COMBAT, false, true, nullptr, 0},
        {L"super_skill", L"Super Skills Mode", L"Maximum ability output preset.", L"SuperSkillsMode", CAT_COMBAT, false, true, nullptr, 0},
        {L"cooldown", L"No Skill Cooldown", L"Removes cooldowns for the local player.", L"SkillCooldownForMe", CAT_COMBAT, false, true, nullptr, 0},
        {L"invulnerable", L"Invulnerable", L"Prevents the local player's health from dropping.", L"InvulnerableOnlyForMe", CAT_COMBAT, false, true, nullptr, 0},
        {L"max_energy", L"Get Max Energy", L"Restores the local energy pool to maximum.", L"GetMaxEnergy", CAT_COMBAT, false, true, nullptr, 0},
        {L"detector", L"Enemy + Loot Detector", L"Extends minimap detection for enemies and loot.", L"EnemynLootDetector", CAT_COMBAT, false, true, nullptr, 0},

        {L"combo", L"Always Max Combo", L"Keeps the melee combo counter at maximum.", L"AlwaysMaxCombo", CAT_ARSENAL, false, true, nullptr, 0},
        {L"infinite_ammo", L"Infinite Primary + Secondary", L"Enables both primary and secondary infinite ammo paths.", L"Ammo1", CAT_ARSENAL, false, true, L"Ammo2", 0},
        {L"incarnon_ammo", L"Infinite Incarnon Ammo", L"Locks Incarnon ammunition to a usable value.", L"AmmoIncarnon", CAT_ARSENAL, false, true, nullptr, 0},
        {L"regen_on", L"Fast Ammo Regen", L"Enables accelerated passive ammo regeneration.", L"AmmoRegenOn", CAT_ARSENAL, false, true, nullptr, 1},
        {L"regen_off", L"Disable Ammo Regen", L"Forces passive ammo regeneration off.", L"AmmoRegenOff", CAT_ARSENAL, false, true, nullptr, 1},
        {L"incarnon_charge", L"Fast Incarnon Charge", L"Accelerates Incarnon transformation charge.", L"FastIncarnonCharge", CAT_ARSENAL, false, true, nullptr, 0},
        {L"tauron_fast", L"Fast Tauron Strike", L"Accelerates Tauron Strike charge.", L"FastChargeTauronStrike", CAT_ARSENAL, false, true, nullptr, 0},
        {L"tauron_faster", L"Tauron Strike: Maximum", L"Uses the faster Tauron charge path.", L"FastChargeTauronStrikeFaster", CAT_ARSENAL, false, true, nullptr, 0},

        {L"koumei", L"Auto Koumei Challenge", L"Automatically completes Koumei dice challenges.", L"AutoChallengeKoumei", CAT_WORLD, false, true, nullptr, 0},
        {L"eximus", L"Always Eximus", L"Forces the Eximus spawn path.", L"AlwaysEximus", CAT_WORLD, false, true, nullptr, 0},
        {L"vacuum_always", L"Vacuum Always", L"Keeps the general loot vacuum enabled.", L"Alwaysvacuum", CAT_WORLD, false, true, nullptr, 0},
        {L"vacuum_new", L"Loot Vacuum: New", L"Recommended pickup teleport implementation.", L"LootVacuumNew", CAT_WORLD, false, true, nullptr, 2},
        {L"vacuum_old", L"Loot Vacuum: Legacy", L"Legacy vacuum implementation for fallback use.", L"LootVacuumOld", CAT_WORLD, false, true, nullptr, 2},
        {L"loot_hotkey", L"Loot Hotkey", L"Enables manual vacuum activation.", L"LootHotkey", CAT_WORLD, false, true, nullptr, 0},
        {L"vacuum_limit", L"Increase Vacuum Limit", L"Raises the legacy vacuum distance cap.", L"IncreaseLootVacuumLimit", CAT_WORLD, false, true, nullptr, 0},

        {L"fov", L"FOV Changer", L"Overrides the in-game field of view.", L"FovChanger", CAT_VIEW, false, true, nullptr, 0},
    };

    g_numerics = {
        {L"skill_value", L"Skill Multiplier", L"OwHelper SkillsMultiplier_Value", L"SkillsMultiplier_Value", CAT_COMBAT, 1, 1, 20, 1},
        {L"vacuum_value", L"Legacy Vacuum Range", L"Distance used by LootVacuumOld", L"LootVacuumOldValue", CAT_WORLD, 50, 10, 255, 5},
        {L"fov_value", L"Field of View", L"Camera angle in degrees", L"FovChangerValue", CAT_VIEW, 110, 50, 150, 1},
    };
}

void DrawToggle(HDC dc, const RECT& rc, bool enabled) {
    RoundFill(dc, rc, enabled ? C_ACCENT_2 : RGBX(53, 62, 68), 24);
    int diameter = (rc.bottom - rc.top) - 6;
    int x = enabled ? rc.right - diameter - 3 : rc.left + 3;
    RECT knob{x, rc.top + 3, x + diameter, rc.bottom - 3};
    RoundFill(dc, knob, enabled ? C_ACCENT : RGBX(177, 185, 190), diameter);
}

void DrawButton(HDC dc, const RECT& rc, const wchar_t* label, bool primary) {
    RoundFill(dc, rc, primary ? C_ACCENT : C_CARD_HI, 8);
    if (!primary) RoundStroke(dc, rc, C_BORDER, 8);
    Text(dc, label, rc, g_font_small, primary ? C_BG : C_TEXT,
         DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

void PaintScriptsTab(HDC dc, const RECT& client);

void PaintUi(HDC dc, const RECT& client) {
    g_hits.clear();
    FillRectColor(dc, client, C_BG);

    const int sidebar_w = 236;
    const int top_h = 90;
    const int bottom_h = 54;
    RECT sidebar{0, 0, sidebar_w, client.bottom};
    RECT topbar{sidebar_w, 0, client.right, top_h};
    RECT statusbar{sidebar_w, client.bottom - bottom_h, client.right, client.bottom};
    FillRectColor(dc, sidebar, C_PANEL);
    FillRectColor(dc, topbar, C_BG);
    FillRectColor(dc, statusbar, C_PANEL);

    // Brand block.
    Text(dc, L"WF_MAXON", R(24, 24, 210, 52), g_font_title, C_TEXT);
    Text(dc, L"WARFRAME LITE CONTROL", R(25, 54, 210, 74), g_font_mono, C_ACCENT);
    RECT brand_line{24, 82, sidebar_w - 24, 84};
    FillRectColor(dc, brand_line, C_ACCENT_2);

    // Navigation.
    int nav_y = 112;
    for (int i = 0; i < 5; ++i) {
        RECT item{16, nav_y, sidebar_w - 16, nav_y + 54};
        bool active = i == g_category;
        if (active) {
            RoundFill(dc, item, C_CARD_HI, 9);
            RECT accent{16, nav_y + 8, 19, nav_y + 46};
            FillRectColor(dc, accent, C_ACCENT);
        }
        Text(dc, kCategoryNames[i], R(32, nav_y + 5, 155, nav_y + 31),
             g_font_heading, active ? C_TEXT : C_MUTED);
        wchar_t count[32]{};
        swprintf_s(count, L"%d / %d", CategoryEnabledCount(i), CategoryCount(i));
        Text(dc, count, R(150, nav_y + 6, sidebar_w - 28, nav_y + 31),
             g_font_mono, active ? C_ACCENT : C_MUTED, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        Text(dc, kCategoryDescriptions[i], R(32, nav_y + 29, sidebar_w - 24, nav_y + 49),
             g_font_small, C_MUTED);
        g_hits.push_back({UiHit::Category, item, i});
        nav_y += 64;
    }

    // Game state block.
    bool game_running = IsProcessRunning(L"Warframe.x64.exe");
    RECT engine_box{16, client.bottom - 154, sidebar_w - 16, client.bottom - 72};
    RoundFill(dc, engine_box, C_CARD, 9);
    RoundStroke(dc, engine_box, C_BORDER, 9);
    Text(dc, L"GAME", R(30, engine_box.top + 8, 120, engine_box.top + 30), g_font_mono, C_MUTED);
    Text(dc, game_running ? L"RUNNING" : L"OFFLINE",
         R(30, engine_box.top + 31, engine_box.right - 26, engine_box.top + 56),
         g_font_heading, game_running ? C_ACCENT : C_MUTED);
    Text(dc, L"reads WarframeHelper.ini",
         R(30, engine_box.top + 56, engine_box.right - 26, engine_box.bottom - 6),
         g_font_small, C_MUTED);

    // Top heading and actions.
    Text(dc, kCategoryNames[g_category], R(sidebar_w + 28, 15, sidebar_w + 330, 49),
         g_font_title, C_TEXT);
    Text(dc, kCategoryDescriptions[g_category], R(sidebar_w + 30, 49, sidebar_w + 520, 74),
         g_font_body, C_MUTED);

    int action_right = client.right - 24;
    RECT apply{action_right - 168, 25, action_right, 65};
    DrawButton(dc, apply, L"APPLY", true);
    g_hits.push_back({UiHit::Apply, apply, -1});

    // Content cards (skip for SCRIPTS tab, which has its own renderer).
    if (g_category != CAT_SCRIPTS) {
    int content_left = sidebar_w + 28;
    int content_right = client.right - 24;
    int content_top = top_h + 4;
    int content_bottom = client.bottom - bottom_h - 8;
    int gap = 14;
    int columns = (content_right - content_left > 760) ? 2 : 1;
    int card_w = (content_right - content_left - (columns - 1) * gap) / columns;
    int card_h = 82;
    int row = 0;
    int col = 0;
    int logical_y = content_top - g_scroll;

    for (int i = 0; i < static_cast<int>(g_features.size()); ++i) {
        const auto& feature = g_features[i];
        if (feature.category != g_category) continue;
        int x = content_left + col * (card_w + gap);
        int y = logical_y + row * (card_h + gap);
        RECT card{x, y, x + card_w, y + card_h};
        if (card.bottom >= content_top && card.top <= content_bottom) {
            RoundFill(dc, card, C_CARD, 10);
            RoundStroke(dc, card, feature.enabled ? C_ACCENT_2 : C_BORDER, 10);
            if (feature.enabled) {
                RECT signal{card.left, card.top + 14, card.left + 3, card.bottom - 14};
                FillRectColor(dc, signal, C_ACCENT);
            }
            Text(dc, feature.title, R(card.left + 18, card.top + 11, card.right - 76, card.top + 37),
                 g_font_heading, C_TEXT);
            Text(dc, feature.description, R(card.left + 18, card.top + 38, card.right - 22, card.bottom - 10),
                 g_font_small, C_MUTED, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
            RECT toggle{card.right - 60, card.top + 20, card.right - 18, card.top + 42};
            DrawToggle(dc, toggle, feature.enabled);
            g_hits.push_back({UiHit::Feature, card, i});
        }
        ++col;
        if (col >= columns) { col = 0; ++row; }
    }
    if (col != 0) ++row;

    for (int i = 0; i < static_cast<int>(g_numerics.size()); ++i) {
        const auto& numeric = g_numerics[i];
        if (numeric.category != g_category) continue;
        int x = content_left + col * (card_w + gap);
        int y = logical_y + row * (card_h + gap);
        RECT card{x, y, x + card_w, y + card_h};
        if (card.bottom >= content_top && card.top <= content_bottom) {
            RoundFill(dc, card, C_CARD, 10);
            RoundStroke(dc, card, C_BORDER, 10);
            Text(dc, numeric.title, R(card.left + 18, card.top + 9, card.right - 80, card.top + 34),
                 g_font_heading, C_TEXT);
            wchar_t value[32]{};
            swprintf_s(value, L"%d", numeric.value);
            Text(dc, value, R(card.right - 74, card.top + 8, card.right - 18, card.top + 34),
                 g_font_mono, C_ACCENT, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            Text(dc, numeric.description, R(card.left + 18, card.top + 31, card.right - 18, card.top + 51),
                 g_font_small, C_MUTED);
            RECT track{card.left + 18, card.bottom - 22, card.right - 18, card.bottom - 16};
            RoundFill(dc, track, RGBX(51, 61, 67), 6);
            double t = double(numeric.value - numeric.minimum) / double(numeric.maximum - numeric.minimum);
            RECT fill = track;
            fill.right = fill.left + static_cast<int>((fill.right - fill.left) * t);
            RoundFill(dc, fill, C_ACCENT_2, 6);
            int knob_x = fill.right;
            RECT knob{knob_x - 7, track.top - 5, knob_x + 7, track.bottom + 5};
            RoundFill(dc, knob, C_ACCENT, 14);
            g_hits.push_back({UiHit::Numeric, card, i});
        }
        ++col;
        if (col >= columns) { col = 0; ++row; }
    }
    if (col != 0) ++row;

    int total_h = row * (card_h + gap);
    int viewport_h = content_bottom - content_top;
    g_scroll_max = std::max(0, total_h - viewport_h + 6);
    g_scroll = std::clamp(g_scroll, 0, g_scroll_max);

    // Status bar.
    RECT status_dot{sidebar_w + 30, statusbar.top + 21, sidebar_w + 38, statusbar.top + 29};
    RoundFill(dc, status_dot, g_status_color, 8);
    Text(dc, g_status, R(sidebar_w + 48, statusbar.top + 10, client.right - 430, statusbar.bottom - 8),
         g_font_mono, g_status_color);
    std::wstring path = g_config.IniPath().empty() ? L"Warframe directory not detected" : g_config.IniPath();
    Text(dc, path, R(client.right - 580, statusbar.top + 10, client.right - 190, statusbar.bottom - 8),
         g_font_small, C_MUTED, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT browse{client.right - 176, statusbar.top + 10, client.right - 94, statusbar.bottom - 9};
    RECT mirror{client.right - 86, statusbar.top + 10, client.right - 18, statusbar.bottom - 9};
    RECT reload_scripts{client.right - 420, statusbar.top + 10, client.right - 188, statusbar.bottom - 9};
    DrawButton(dc, browse, L"PATH", false);
    DrawButton(dc, mirror, g_mirror_rage ? L"RAGE: ON" : L"RAGE: OFF", false);
    DrawButton(dc, reload_scripts, L"RELOAD SCRIPTS", false);
    g_hits.push_back({UiHit::Browse, browse, -1});
    g_hits.push_back({UiHit::MirrorRage, mirror, -1});
    g_hits.push_back({UiHit::ReloadScripts, reload_scripts, -1});
    } // end if (g_category != CAT_SCRIPTS)

    if (g_category == CAT_SCRIPTS) {
        PaintScriptsTab(dc, client);
    }
}

// ---------------------------------------------------------------------------
// Scripts tab rendering
// ---------------------------------------------------------------------------
void PaintScriptsTab(HDC dc, const RECT& client) {
    g_hits.clear();

    const int sidebar_w = 236;
    const int top_h = 90;
    const int bottom_h = 54;
    const int content_left = sidebar_w + 28;
    const int content_right = client.right - 24;
    const int content_top = top_h + 4;
    const int content_bottom = client.bottom - bottom_h - 8;

    // Section: Scripts list
    const int script_header_h = 32;
    const int script_row_h = 44;
    const int hooks_header_h = 28;
    const int hook_row_h = 26;
    const int log_h = 160;
    const int gap = 8;

    // Available space for scripts + hooks (log is fixed at bottom)
    int available_h = content_bottom - content_top - log_h - gap * 2;
    // Split: scripts get 55%, hooks get 45%
    int scripts_h = available_h * 55 / 100;
    int hooks_h = available_h - scripts_h;

    RECT scripts_box{content_left, content_top, content_right, content_top + scripts_h};
    RECT hooks_box{content_left, scripts_box.bottom + gap, content_right, scripts_box.bottom + gap + hooks_h};
    RECT log_box{content_left, hooks_box.bottom + gap, content_right, hooks_box.bottom + gap + log_h};

    // ---- Scripts list ----
    RoundFill(dc, scripts_box, C_PANEL, 10);
    RoundStroke(dc, scripts_box, C_BORDER, 10);
    Text(dc, L"SCRIPTS", R(scripts_box.left + 16, scripts_box.top + 8, scripts_box.right - 120, scripts_box.top + 26),
         g_font_heading, C_ACCENT);

    const auto& scripts = g_scripts.GetLoadedScripts();
    int script_scroll = g_scroll;
    int script_max_scroll = std::max(0, static_cast<int>(scripts.size()) * script_row_h - (scripts_h - script_header_h - 8));
    script_scroll = std::clamp(script_scroll, 0, script_max_scroll);

    int sy = scripts_box.top + script_header_h - script_scroll;
    for (int i = 0; i < static_cast<int>(scripts.size()); ++i) {
        const auto& s = scripts[i];
        RECT row{scripts_box.left + 8, sy, scripts_box.right - 8, sy + script_row_h};
        if (row.bottom >= scripts_box.top + script_header_h && row.top <= scripts_box.bottom) {
            RoundFill(dc, row, C_CARD, 8);
            RoundStroke(dc, row, s.ok ? C_BORDER : C_DANGER, 8);

            // Status dot
            RECT dot{row.left + 10, row.top + 14, row.left + 18, row.top + 22};
            RoundFill(dc, dot, s.ok ? C_ACCENT : C_DANGER, 8);

            // Script name
            Text(dc, s.name, R(row.left + 26, row.top + 4, row.right - 140, row.top + 22),
                 g_font_heading, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

            // Status text
            Text(dc, s.ok ? L"loaded" : L"error", R(row.left + 26, row.top + 22, row.right - 140, row.top + 40),
                 g_font_small, s.ok ? C_ACCENT : C_DANGER);

            // Reload button
            RECT reload_btn{row.right - 110, row.top + 8, row.right - 60, row.top + 34};
            DrawButton(dc, reload_btn, L"Reload", false);
            g_hits.push_back({UiHit::ScriptItem, reload_btn, i});

            // Hook count badge
            wchar_t badge[32]{};
            swprintf_s(badge, L"%d hooks", g_scripts.HookCount()); // total hooks for now
            Text(dc, badge, R(row.right - 55, row.top + 10, row.right - 10, row.top + 32),
                 g_font_small, C_MUTED, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
        sy += script_row_h;
    }

    // ---- Hook registry ----
    RoundFill(dc, hooks_box, C_PANEL, 10);
    RoundStroke(dc, hooks_box, C_BORDER, 10);
    Text(dc, L"ACTIVE HOOKS", R(hooks_box.left + 16, hooks_box.top + 6, hooks_box.right - 16, hooks_box.top + 22),
         g_font_heading, C_MUTED);

    const auto& hooks = g_scripts.GetHooks();
    int hook_scroll = std::max(0, g_scroll - script_max_scroll - 16);
    int hook_max_scroll = std::max(0, static_cast<int>(hooks.size()) * hook_row_h - (hooks_h - hooks_header_h - 8));
    hook_scroll = std::clamp(hook_scroll, 0, hook_max_scroll);

    int hy = hooks_box.top + hooks_header_h - hook_scroll;
    for (int i = 0; i < static_cast<int>(hooks.size()); ++i) {
        const auto& h = hooks[i];
        RECT row{hooks_box.left + 8, hy, hooks_box.right - 8, hy + hook_row_h};
        if (row.bottom >= hooks_box.top + hooks_header_h && row.top <= hooks_box.bottom) {
            RoundFill(dc, row, C_CARD, 6);
            wchar_t addr[32]{};
            swprintf_s(addr, L"0x%llX", static_cast<unsigned long long>(h.target));
            Text(dc, addr, R(row.left + 10, row.top + 2, row.left + 120, row.bottom - 2),
                 g_font_mono, C_MUTED, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            Text(dc, h.active ? L"active" : L"inactive", R(row.left + 130, row.top + 2, row.left + 220, row.bottom - 2),
                 g_font_small, h.active ? C_ACCENT : C_DANGER);
            wchar_t fires[32]{};
            swprintf_s(fires, L"%d fires", static_cast<int>(h.fire_count));
            Text(dc, fires, R(row.right - 100, row.top + 2, row.right - 10, row.bottom - 2),
                 g_font_small, C_MUTED, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            g_hits.push_back({UiHit::HookItem, row, i});
        }
        hy += hook_row_h;
    }

    // ---- Script log ----
    RoundFill(dc, log_box, C_PANEL, 10);
    RoundStroke(dc, log_box, C_BORDER, 10);
    Text(dc, L"SCRIPT LOG", R(log_box.left + 16, log_box.top + 6, log_box.right - 16, log_box.top + 22),
         g_font_heading, C_MUTED);

    auto log_lines = g_scripts.GetScriptLog(64);
    int line_h = 16;
    int max_lines = (log_h - 30) / line_h;
    int log_scroll = std::max(0, static_cast<int>(log_lines.size()) - max_lines);
    log_scroll = std::clamp(log_scroll, 0, log_scroll);
    int ly = log_box.top + 26;
    for (int i = log_scroll; i < static_cast<int>(log_lines.size()) && i < log_scroll + max_lines; ++i) {
        Text(dc, log_lines[i], R(log_box.left + 12, ly, log_box.right - 12, ly + line_h),
             g_font_mono, C_MUTED, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        ly += line_h;
    }
    if (log_lines.empty()) {
        Text(dc, L"No log entries yet", R(log_box.left + 12, log_box.top + 30, log_box.right - 12, log_box.bottom - 8),
             g_font_small, C_MUTED);
    }
}

void SetNumericFromPoint(int index, int x) {
    if (index < 0 || index >= static_cast<int>(g_numerics.size())) return;
    // Recompute the matching card track from the current paint layout.
    RECT client{};
    GetClientRect(g_hwnd, &client);
    int sidebar_w = 236;
    int content_left = sidebar_w + 28;
    int content_right = client.right - 24;
    int gap = 14;
    int columns = (content_right - content_left > 760) ? 2 : 1;
    int card_w = (content_right - content_left - (columns - 1) * gap) / columns;

    for (const auto& hit : g_hits) {
        if (hit.kind == UiHit::Numeric && hit.index == index) {
            int left = hit.rect.left + 18;
            int right = hit.rect.right - 18;
            double t = double(std::clamp(x, left, right) - left) / double(std::max(1, right - left));
            auto& n = g_numerics[index];
            int raw = n.minimum + static_cast<int>(std::round(t * (n.maximum - n.minimum)));
            raw = ((raw - n.minimum + n.step / 2) / n.step) * n.step + n.minimum;
            n.value = std::clamp(raw, n.minimum, n.maximum);
            g_dirty = true;
            SetStatus(L"UNSAVED CHANGES", C_WARN);
            InvalidateRect(g_hwnd, nullptr, FALSE);
            return;
        }
    }
    (void)card_w;
    (void)columns;
}

void OnClick(int x, int y) {
    for (const auto& hit : g_hits) {
        if (!PtIn(hit.rect, x, y)) continue;
        switch (hit.kind) {
        case UiHit::Category:
            g_category = hit.index;
            g_scroll = 0;
            InvalidateRect(g_hwnd, nullptr, FALSE);
            return;
        case UiHit::Feature:
            if (hit.index >= 0 && hit.index < static_cast<int>(g_features.size())) {
                g_features[hit.index].enabled = !g_features[hit.index].enabled;
                EnforceExclusive(hit.index);
                g_dirty = true;
                SetStatus(L"UNSAVED CHANGES", C_WARN);
                InvalidateRect(g_hwnd, nullptr, FALSE);
            }
            return;
        case UiHit::Numeric:
            g_drag_numeric = hit.index;
            SetCapture(g_hwnd);
            SetNumericFromPoint(hit.index, x);
            return;
        case UiHit::Apply:
            SaveConfig();
            return;
        case UiHit::Browse:
            BrowseGameDirectory();
            return;
        case UiHit::MirrorRage:
            g_mirror_rage = !g_mirror_rage;
            SetStatus(g_mirror_rage ? L"RAGE PROFILE MIRROR ENABLED" : L"RAGE PROFILE MIRROR DISABLED", C_ACCENT);
            InvalidateRect(g_hwnd, nullptr, FALSE);
            return;
        case UiHit::ReloadScripts: {
            std::wstring script_status = g_scripts.ReloadScripts() ? L"Scripts reloaded" : L"Script reload failed";
            int hook_count = g_scripts.HookCount();
            int script_count = g_scripts.ScriptCount();
            wchar_t detail[256]{};
            swprintf_s(detail, L"%s (%d scripts, %d hooks)", script_status.c_str(), script_count, hook_count);
            SetStatus(detail, C_ACCENT);
            InvalidateRect(g_hwnd, nullptr, FALSE);
            return;
        }
        case UiHit::ScriptItem: {
            // Per-script reload: reload all scripts (full reload for now).
            std::wstring script_status = g_scripts.ReloadScripts() ? L"Scripts reloaded" : L"Script reload failed";
            int hook_count = g_scripts.HookCount();
            int script_count = g_scripts.ScriptCount();
            wchar_t detail[256]{};
            swprintf_s(detail, L"%s (%d scripts, %d hooks)", script_status.c_str(), script_count, hook_count);
            SetStatus(detail, C_ACCENT);
            InvalidateRect(g_hwnd, nullptr, FALSE);
            return;
        }
        case UiHit::HookItem: {
            // Hook detail could be expanded; for now just refresh.
            InvalidateRect(g_hwnd, nullptr, FALSE);
            return;
        }
        }
    }
}

void CreateFonts() {
    g_font_title = CreateFontW(-27, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Display");
    g_font_heading = CreateFontW(-17, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Text");
    g_font_body = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Text");
    g_font_small = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Text");
    g_font_mono = CreateFontW(-12, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, FIXED_PITCH, L"Cascadia Mono");
}

void DestroyFonts() {
    for (HFONT font : {g_font_title, g_font_heading, g_font_body, g_font_small, g_font_mono}) {
        if (font) DeleteObject(font);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (wfmaxon::HandleImGuiMessage(hwnd, message, wparam, lparam)) {
        return 0;
    }

    switch (message) {
    case WM_CREATE:
        SetTimer(hwnd, kStatusTimer, 16, nullptr); // ~60fps refresh for smooth ImGui rendering
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        if (wfmaxon::IsImGuiInitialized()) {
            wfmaxon::RenderImGuiFrame();
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        OnClick(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        return 0;
    case WM_MOUSEMOVE:
        if (g_drag_numeric >= 0 && (wparam & MK_LBUTTON)) {
            SetNumericFromPoint(g_drag_numeric, GET_X_LPARAM(lparam));
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_drag_numeric >= 0) {
            SetNumericFromPoint(g_drag_numeric, GET_X_LPARAM(lparam));
            g_drag_numeric = -1;
            ReleaseCapture();
        }
        return 0;
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wparam);
        g_scroll = std::clamp(g_scroll - delta / WHEEL_DELTA * 58, 0, g_scroll_max);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_TIMER:
        if (wparam == kStatusTimer) {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_SIZE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize = {900, 620};
        return 0;
    }
    case WM_CLOSE:
        if (g_dirty) SaveConfig();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kStatusTimer);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

bool WriteSelfTestLog(const std::wstring& path, const std::wstring& body) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    int size = WideCharToMultiByte(CP_UTF8, 0, body.c_str(), static_cast<int>(body.size()),
                                   nullptr, 0, nullptr, nullptr);
    std::string utf8(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, body.c_str(), static_cast<int>(body.size()),
                        utf8.data(), size, nullptr, nullptr);
    DWORD written = 0;
    BOOL ok = WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(file);
    return ok && written == utf8.size();
}

int RunSelfTest(const std::wstring& log_path) {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(static_cast<DWORD>(std::size(temp)), temp);
    std::wstring test_dir = std::wstring(temp) + L"WF_MAXON_selftest";
    CreateDirectoryW(test_dir.c_str(), nullptr);

    InitBindings();
    ConfigStore store;
    store.SetGameDirectory(test_dir);
    for (auto& feature : g_features) feature.enabled = true;
    // Enforce the same exclusivity rules used by the UI.
    for (int i = 0; i < static_cast<int>(g_features.size()); ++i) EnforceExclusive(i);
    g_numerics[0].value = 7;
    g_numerics[1].value = 180;
    g_numerics[2].value = 120;

    bool saved = store.Save(g_features, g_numerics, true);
    std::vector<FeatureBinding> loaded = g_features;
    std::vector<NumericBinding> numbers = g_numerics;
    for (auto& f : loaded) f.enabled = false;
    for (auto& n : numbers) n.value = n.minimum;
    bool read = store.Load(loaded, numbers);

    int mismatches = 0;
    for (size_t i = 0; i < g_features.size(); ++i) {
        if (loaded[i].enabled != g_features[i].enabled) ++mismatches;
    }
    for (size_t i = 0; i < g_numerics.size(); ++i) {
        if (numbers[i].value != g_numerics[i].value) ++mismatches;
    }
    int regen_enabled = 0;
    int vacuum_enabled = 0;
    for (const auto& feature : g_features) {
        if (feature.enabled && feature.exclusive_group == 1) ++regen_enabled;
        if (feature.enabled && feature.exclusive_group == 2) ++vacuum_enabled;
    }
    bool regen_exclusive = regen_enabled <= 1;
    bool vacuum_exclusive = vacuum_enabled <= 1;
    bool mirror_ok = store.ReadBool(L"rage_Cheats", L"AlwaysMaxCombo", false);
    bool ammo_alias_ok = store.ReadBool(L"enable_Cheats", L"Ammo1", false) &&
                         store.ReadBool(L"enable_Cheats", L"Ammo2", false);

    bool engine_feature_parity = g_engine.Features().size() == g_features.size();
    for (const auto& ui_feature : g_features) {
        bool found = false;
        for (const auto& live_feature : g_engine.Features()) {
            if (wcscmp(ui_feature.ini_key, live_feature.ini_key) == 0) {
                found = true;
                break;
            }
        }
        if (!found) engine_feature_parity = false;
    }
    bool engine_numeric_parity = g_engine.Numerics().size() == g_numerics.size();
    for (const auto& ui_numeric : g_numerics) {
        bool found = false;
        for (const auto& live_numeric : g_engine.Numerics()) {
            if (wcscmp(ui_numeric.ini_key, live_numeric.ini_key) == 0) {
                found = true;
                break;
            }
        }
        if (!found) engine_numeric_parity = false;
    }

    wchar_t report[2048]{};
    swprintf_s(report,
        L"WF_MAXON SELFTEST\r\n"
        L"saved=%s\r\n"
        L"loaded=%s\r\n"
        L"feature_count=%zu\r\n"
        L"numeric_count=%zu\r\n"
        L"mismatches=%d\r\n"
        L"ammo_regen_exclusive=%s\r\n"
        L"loot_vacuum_exclusive=%s\r\n"
        L"rage_profile_mirror=%s\r\n"
        L"ammo_alias_pair=%s\r\n"
        L"engine_feature_parity=%s\r\n"
        L"engine_numeric_parity=%s\r\n"
        L"result=%s\r\n",
        saved ? L"true" : L"false",
        read ? L"true" : L"false",
        g_features.size(), g_numerics.size(), mismatches,
        regen_exclusive ? L"true" : L"false",
        vacuum_exclusive ? L"true" : L"false",
        mirror_ok ? L"true" : L"false",
        ammo_alias_ok ? L"true" : L"false",
        engine_feature_parity ? L"true" : L"false",
        engine_numeric_parity ? L"true" : L"false",
        (saved && read && mismatches == 0 && regen_exclusive && vacuum_exclusive && mirror_ok && ammo_alias_ok &&
         engine_feature_parity && engine_numeric_parity)
            ? L"PASS" : L"FAIL");
    WriteSelfTestLog(log_path, report);
    return (saved && read && mismatches == 0 && regen_exclusive && vacuum_exclusive && mirror_ok && ammo_alias_ok &&
            engine_feature_parity && engine_numeric_parity) ? 0 : 1;
}

} // namespace

static int RunApplication(HINSTANCE instance, int show, bool parse_command_line) {
#ifndef WF_MAXON_DLL
    if (parse_command_line) {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argc >= 2 && wcscmp(argv[1], L"--selftest") == 0) {
            std::wstring log = argc >= 3 ? argv[2] : (wfmaxon::ExecutableDirectory() + L"\\wf_maxon_selftest.log");
            int result = RunSelfTest(log);
            LocalFree(argv);
            return result;
        }
        if (argv) LocalFree(argv);
    }
#else
    (void)parse_command_line;
#endif

    g_self_module = instance;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    InitBindings();
    CreateFonts();

    wchar_t disable_live_apply[8]{};
    const DWORD disable_live_apply_len = GetEnvironmentVariableW(
        L"WF_MAXON_DISABLE_LIVE_APPLY", disable_live_apply,
        static_cast<DWORD>(std::size(disable_live_apply)));
    const bool live_apply_disabled = disable_live_apply_len != 0 &&
                                     _wcsicmp(disable_live_apply, L"1") == 0;

    if (g_config.DetectGameDirectory()) {
        g_config.Load(g_features, g_numerics);
        SetStatus(g_config.Exists() ? L"CONFIG LOADED" : L"CONFIG TARGET READY", C_ACCENT);
        if (!live_apply_disabled && g_config.Exists() && IsProcessRunning(L"Warframe.x64.exe")) {
#ifdef WF_MAXON_DLL
            // Replacement-process startup can reach the payload before the
            // launcher has finished publishing GameRules. Retry the read-only
            // resolver for a bounded period instead of silently skipping the
            // first automatic FOV application.
            for (int attempt = 0; attempt < 30; ++attempt) {
                if (ApplyToGame()) break;
                Sleep(1000);
            }
#else
            ApplyToGame();
#endif
        }
        // Initialize Lua script engine after config is loaded.
        g_scripts.Initialize(&g_engine, g_config.GameDirectory());
        int sc = g_scripts.ScriptCount();
        int hc = g_scripts.HookCount();
        if (sc > 0 || hc > 0) {
            wchar_t script_msg[128]{};
            swprintf_s(script_msg, L"Scripts: %d loaded, %d hooks", sc, hc);
            SetStatus(script_msg, C_ACCENT);
        }
    } else {
        SetStatus(L"SELECT WARFRAME PATH", C_WARN);
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWindowClass;
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, kWindowClass, kWindowTitle,
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1180, 760,
                             nullptr, nullptr, instance, nullptr);
    if (!g_hwnd) {
        DestroyFonts();
        CoUninitialize();
        return 2;
    }

    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_hwnd, 20, &dark, sizeof(dark));
    COLORREF caption = C_PANEL;
    COLORREF text = C_TEXT;
    DwmSetWindowAttribute(g_hwnd, 35, &caption, sizeof(caption));
    DwmSetWindowAttribute(g_hwnd, 36, &text, sizeof(text));

    if (!wfmaxon::InitImGui(g_hwnd)) {
        DestroyFonts();
        CoUninitialize();
        return 3;
    }

    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    wfmaxon::ShutdownImGui();
    DestroyFonts();
    CoUninitialize();
    return static_cast<int>(message.wParam);
}

#ifndef WF_MAXON_DLL
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    return RunApplication(instance, show, true);
}
#else
static unsigned __stdcall PayloadUiThread(void* module) {
    return static_cast<unsigned>(RunApplication(static_cast<HINSTANCE>(module), SW_SHOW, false));
}

extern "C" __declspec(dllexport) unsigned int WF_MAXON_GetVersion() {
    return 0x00010000;
}

extern "C" __declspec(dllexport) const wchar_t* WF_MAXON_GetWindowClass() {
    return kWindowClass;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        uintptr_t thread = _beginthreadex(nullptr, 0, PayloadUiThread, module, 0, nullptr);
        if (!thread) return FALSE;
        CloseHandle(reinterpret_cast<HANDLE>(thread));
    }
    return TRUE;
}
#endif
