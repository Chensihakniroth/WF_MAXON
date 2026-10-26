#include "imgui_layer.h"
#include "config.h"
#include "engine.h"
#include "script_engine.h"

#include <d3d11.h>
#include <dxgi.h>
#include <string>
#include <vector>
#include <algorithm>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

using wfmaxon::ConfigStore;
using wfmaxon::FeatureBinding;
using wfmaxon::NumericBinding;

#ifndef RGBX
#define RGBX(r, g, b) RGB(r, g, b)
#endif

extern wfmaxon::ScriptEngine g_scripts;
extern wfmaxon::LiveEngine g_engine;
extern ConfigStore g_config;
extern std::vector<FeatureBinding> g_features;
extern std::vector<NumericBinding> g_numerics;
extern bool g_mirror_rage;
extern bool g_dirty;
extern std::wstring g_status;
extern COLORREF g_status_color;

extern void SetStatus(const std::wstring& text, COLORREF color);
extern bool SaveConfig();
extern bool ApplyToGame();
extern void BrowseGameDirectory();
extern void EnforceExclusive(int changed_index);
extern int CategoryCount(int category);
extern int CategoryEnabledCount(int category);
extern bool IsProcessRunning(const wchar_t* process_name);

namespace wfmaxon {
namespace {

ID3D11Device*           g_pd3dDevice = nullptr;
ID3D11DeviceContext*    g_pd3dDeviceContext = nullptr;
IDXGISwapChain*         g_pSwapChain = nullptr;
ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
HWND                    g_hCurrentHwnd = nullptr;
bool                    g_imGuiInitialized = false;

char g_scriptEditorBuffer[65536] = 
    "-- ============================================================================\n"
    "-- WF_MAXON Lua Script\n"
    "-- Write Lua / Pluto code and click [Run Script] or press Ctrl+Enter!\n"
    "-- ============================================================================\n"
    "print(\"Hello from WF_MAXON Lua engine!\")\n"
    "local ver = _WF_MAXON_VERSION or \"1.0\"\n"
    "print(\"Engine version: \" .. tostring(ver))\n\n"
    "-- Example: reading player pointer\n"
    "-- local player = wf.get_player()\n"
    "-- if player ~= 0 then\n"
    "--     print(string.format(\"Player base: 0x%X\", player))\n"
    "-- end\n";

std::string g_lastScriptError = "";
bool g_showScriptError = false;

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    if (SUCCEEDED(g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer)))) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                               createDeviceFlags, featureLevelArray, 2,
                                               D3D11_SDK_VERSION, &sd, &g_pSwapChain,
                                               &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK) {
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                           createDeviceFlags, featureLevelArray, 2,
                                           D3D11_SDK_VERSION, &sd, &g_pSwapChain,
                                           &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    }
    if (res != S_OK) return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void SetupImGuiStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 5.0f;
    style.PopupRounding = 6.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 6.0f;

    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.TabBorderSize = 0.0f;

    style.WindowPadding = ImVec2(16, 16);
    style.FramePadding = ImVec2(10, 6);
    style.ItemSpacing = ImVec2(10, 10);
    style.ItemInnerSpacing = ImVec2(8, 6);
    style.ScrollbarSize = 10.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                  = ImVec4(0.94f, 0.96f, 0.98f, 1.00f);
    colors[ImGuiCol_TextDisabled]          = ImVec4(0.50f, 0.55f, 0.60f, 1.00f);
    colors[ImGuiCol_WindowBg]              = ImVec4(0.07f, 0.08f, 0.10f, 1.00f);
    colors[ImGuiCol_ChildBg]               = ImVec4(0.10f, 0.12f, 0.14f, 1.00f);
    colors[ImGuiCol_PopupBg]               = ImVec4(0.12f, 0.14f, 0.17f, 0.98f);
    colors[ImGuiCol_Border]                = ImVec4(0.18f, 0.22f, 0.26f, 0.80f);
    colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]               = ImVec4(0.13f, 0.15f, 0.18f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.17f, 0.20f, 0.24f, 1.00f);
    colors[ImGuiCol_FrameBgActive]         = ImVec4(0.20f, 0.25f, 0.30f, 1.00f);
    colors[ImGuiCol_TitleBg]               = ImVec4(0.09f, 0.10f, 0.12f, 1.00f);
    colors[ImGuiCol_TitleBgActive]         = ImVec4(0.09f, 0.10f, 0.12f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.09f, 0.10f, 0.12f, 0.75f);
    colors[ImGuiCol_MenuBarBg]             = ImVec4(0.09f, 0.10f, 0.12f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.08f, 0.09f, 0.11f, 0.20f);
    colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.24f, 0.28f, 0.33f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.32f, 0.38f, 0.44f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.40f, 0.48f, 0.55f, 1.00f);
    colors[ImGuiCol_CheckMark]             = ImVec4(0.16f, 0.88f, 0.73f, 1.00f);
    colors[ImGuiCol_SliderGrab]            = ImVec4(0.16f, 0.88f, 0.73f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]      = ImVec4(0.25f, 0.96f, 0.80f, 1.00f);
    colors[ImGuiCol_Button]                = ImVec4(0.14f, 0.17f, 0.20f, 1.00f);
    colors[ImGuiCol_ButtonHovered]         = ImVec4(0.16f, 0.88f, 0.73f, 0.30f);
    colors[ImGuiCol_ButtonActive]          = ImVec4(0.16f, 0.88f, 0.73f, 0.60f);
    colors[ImGuiCol_Header]                = ImVec4(0.16f, 0.88f, 0.73f, 0.18f);
    colors[ImGuiCol_HeaderHovered]         = ImVec4(0.16f, 0.88f, 0.73f, 0.30f);
    colors[ImGuiCol_HeaderActive]          = ImVec4(0.16f, 0.88f, 0.73f, 0.50f);
    colors[ImGuiCol_Separator]             = ImVec4(0.18f, 0.22f, 0.26f, 0.70f);
    colors[ImGuiCol_SeparatorHovered]      = ImVec4(0.16f, 0.88f, 0.73f, 0.50f);
    colors[ImGuiCol_SeparatorActive]       = ImVec4(0.16f, 0.88f, 0.73f, 1.00f);
    colors[ImGuiCol_Tab]                   = ImVec4(0.11f, 0.13f, 0.16f, 1.00f);
    colors[ImGuiCol_TabHovered]            = ImVec4(0.16f, 0.88f, 0.73f, 0.35f);
    colors[ImGuiCol_TabActive]             = ImVec4(0.16f, 0.88f, 0.73f, 0.25f);
    colors[ImGuiCol_TabUnfocused]          = ImVec4(0.10f, 0.11f, 0.14f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.13f, 0.15f, 0.18f, 1.00f);
}

} // namespace

bool IsImGuiInitialized() {
    return g_imGuiInitialized && (g_pd3dDeviceContext != nullptr) && (g_mainRenderTargetView != nullptr);
}

bool InitImGui(HWND hwnd) {
    if (g_imGuiInitialized) return true;
    g_hCurrentHwnd = hwnd;
    if (!CreateDeviceD3D(hwnd)) return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    SetupImGuiStyle();

    if (!ImGui_ImplWin32_Init(hwnd)) {
        CleanupDeviceD3D();
        ImGui::DestroyContext();
        return false;
    }
    if (!ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext)) {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        CleanupDeviceD3D();
        return false;
    }

    g_imGuiInitialized = true;
    return true;
}

void ShutdownImGui() {
    if (!g_imGuiInitialized) return;
    g_imGuiInitialized = false;

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
}

bool HandleImGuiMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (!g_imGuiInitialized) return false;
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) return true;

    if (msg == WM_SIZE && g_pd3dDevice != nullptr && wparam != SIZE_MINIMIZED) {
        CleanupRenderTarget();
        g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lparam), (UINT)HIWORD(lparam), DXGI_FORMAT_UNKNOWN, 0);
        CreateRenderTarget();
        return false;
    }
    return false;
}

void RenderImGuiFrame() {
    if (!IsImGuiInitialized()) return;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    RECT rc;
    GetClientRect(g_hCurrentHwnd, &rc);
    float width = static_cast<float>(rc.right - rc.left);
    float height = static_cast<float>(rc.bottom - rc.top);

    // Root full-viewport window (no borders, clean dark canvas)
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    ImGuiWindowFlags rootFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
                                ImGuiWindowFlags_NoScrollWithMouse;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 16));
    ImGui::Begin("##WF_MAXON_ROOT", nullptr, rootFlags);
    ImGui::PopStyleVar();

    // =========================================================================
    // 1. TOP HEADER BAR
    // =========================================================================
    {
        // Brand & Title
        ImGui::TextColored(ImVec4(0.16f, 0.88f, 0.73f, 1.0f), "WF_MAXON");
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.85f, 0.88f, 0.92f, 1.0f), "Warframe Lite Control & Lua Environment");

        // Game status indicator
        bool gameRunning = IsProcessRunning(L"Warframe.x64.exe");
        ImGui::SameLine(width - 340.0f);
        if (gameRunning) {
            ImGui::TextColored(ImVec4(0.16f, 0.88f, 0.73f, 1.0f), "[● WARFRAME RUNNING]");
        } else {
            ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.25f, 1.0f), "[○ WARFRAME NOT DETECTED]");
        }

        // Apply configuration button
        ImGui::SameLine(width - 130.0f);
        if (ImGui::Button("SAVE & APPLY", ImVec2(114, 24))) {
            SaveConfig();
            ApplyToGame();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }

    // =========================================================================
    // 2. MAIN HORIZONTAL TABS (Top-level clean navigation)
    // =========================================================================
    float footerHeight = 44.0f;
    float contentHeight = height - ImGui::GetCursorPosY() - footerHeight - 16.0f;

    if (ImGui::BeginTabBar("##TopTabs", ImGuiTabBarFlags_None)) {

        // ---------------------------------------------------------------------
        // TAB 1: LUA SCRIPT ENGINE & HOOK MONITOR
        // ---------------------------------------------------------------------
        if (ImGui::BeginTabItem("  LUA SCRIPT EDITOR  ")) {
            ImGui::Spacing();

            // Action toolbar
            if (ImGui::Button("RUN SCRIPT (Ctrl+Enter)", ImVec2(180, 28))) {
                std::string err;
                if (g_scripts.ExecuteScript(g_scriptEditorBuffer, &err)) {
                    g_showScriptError = false;
                    g_lastScriptError = "";
                    SetStatus(L"Script executed successfully", RGBX(42, 225, 185));
                } else {
                    g_showScriptError = true;
                    g_lastScriptError = err;
                    SetStatus(L"Script runtime error", RGBX(231, 91, 104));
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("RELOAD HOOKS FOLDER", ImVec2(170, 28))) {
                g_scripts.ReloadScripts();
                SetStatus(L"Scripts and hooks reloaded", RGBX(42, 225, 185));
            }
            ImGui::SameLine();
            if (ImGui::Button("CLEAR CONSOLE", ImVec2(120, 28))) {
                g_scripts.ClearLog();
            }

            // Error notification banner if any
            if (g_showScriptError && !g_lastScriptError.empty()) {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.35f, 0.40f, 1.0f));
                ImGui::TextWrapped("Error: %s", g_lastScriptError.c_str());
                ImGui::PopStyleColor();
            }

            ImGui::Spacing();

            // Two-column layout: Left = Code Editor, Right = Hooks Table + Log Output
            float panelWidth = (width - 42.0f) * 0.5f;
            float panelHeight = contentHeight - 48.0f;

            // --- Left: Code Editor ---
            ImGui::BeginChild("##EditorChild", ImVec2(panelWidth, panelHeight), true);
            {
                ImGui::TextColored(ImVec4(0.16f, 0.88f, 0.73f, 1.0f), "SCRIPT BUFFER");
                ImGui::Separator();
                ImGuiInputTextFlags flags = ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CtrlEnterForNewLine;
                ImGui::InputTextMultiline("##ScriptCode", g_scriptEditorBuffer, sizeof(g_scriptEditorBuffer),
                                         ImVec2(-1, -1), flags);
            }
            ImGui::EndChild();

            ImGui::SameLine();

            // --- Right: Diagnostics (Hooks Top, Log Bottom) ---
            ImGui::BeginChild("##DiagnosticsChild", ImVec2(panelWidth, panelHeight), false);
            {
                float halfBoxH = (panelHeight - 12.0f) * 0.5f;

                // Active Hooks
                ImGui::BeginChild("##HooksPanel", ImVec2(0, halfBoxH), true);
                {
                    ImGui::TextColored(ImVec4(0.16f, 0.88f, 0.73f, 1.0f), "ACTIVE MEMORY HOOKS");
                    ImGui::Separator();

                    const auto& hooks = g_scripts.GetHooks();
                    if (hooks.empty()) {
                        ImGui::TextDisabled("No active memory detour hooks registered.");
                    } else {
                        if (ImGui::BeginTable("##HooksTbl", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
                            ImGui::TableSetupColumn("Target RVA/Addr", ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                            ImGui::TableSetupColumn("Fires", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                            ImGui::TableHeadersRow();

                            for (const auto& h : hooks) {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                ImGui::Text("0x%llX", static_cast<unsigned long long>(h.target));
                                ImGui::TableSetColumnIndex(1);
                                if (h.active) ImGui::TextColored(ImVec4(0.16f, 0.88f, 0.73f, 1.0f), "ACTIVE");
                                else ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "INACTIVE");
                                ImGui::TableSetColumnIndex(2);
                                ImGui::Text("%ld", h.fire_count);
                            }
                            ImGui::EndTable();
                        }
                    }
                }
                ImGui::EndChild();

                ImGui::Spacing();

                // Live Log
                ImGui::BeginChild("##LogPanel", ImVec2(0, halfBoxH), true);
                {
                    ImGui::TextColored(ImVec4(0.16f, 0.88f, 0.73f, 1.0f), "SCRIPT EXECUTION LOG");
                    ImGui::Separator();

                    auto logs = g_scripts.GetScriptLog(150);
                    if (logs.empty()) {
                        ImGui::TextDisabled("Console log is clear.");
                    } else {
                        for (const auto& line : logs) {
                            char buf[512]{};
                            WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, buf, sizeof(buf), nullptr, nullptr);
                            if (strstr(buf, "ERROR") || strstr(buf, "error")) {
                                ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.40f, 1.0f), "%s", buf);
                            } else {
                                ImGui::TextUnformatted(buf);
                            }
                        }
                        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
                            ImGui::SetScrollHereY(1.0f);
                        }
                    }
                }
                ImGui::EndChild();
            }
            ImGui::EndChild();

            ImGui::EndTabItem();
        }

        // ---------------------------------------------------------------------
        // TAB 2: COMBAT & SURVIVAL FEATURES
        // ---------------------------------------------------------------------
        if (ImGui::BeginTabItem("  COMBAT & SURVIVAL  ")) {
            ImGui::Spacing();
            ImGui::BeginChild("##CombatList", ImVec2(0, contentHeight), true);
            {
                ImGui::TextColored(ImVec4(0.16f, 0.88f, 0.73f, 1.0f), "COMBAT CHEATS & MULTIPLIERS");
                ImGui::Separator();
                ImGui::Spacing();

                // Toggles
                for (size_t i = 0; i < g_features.size(); ++i) {
                    if (g_features[i].category != 0) continue;
                    char title[128]{};
                    char desc[256]{};
                    WideCharToMultiByte(CP_UTF8, 0, g_features[i].title, -1, title, sizeof(title), nullptr, nullptr);
                    WideCharToMultiByte(CP_UTF8, 0, g_features[i].description, -1, desc, sizeof(desc), nullptr, nullptr);

                    if (ImGui::Checkbox(title, &g_features[i].enabled)) {
                        EnforceExclusive(static_cast<int>(i));
                        g_dirty = true;
                    }
                    ImGui::SameLine(320);
                    ImGui::TextDisabled("%s", desc);
                    ImGui::Spacing();
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Sliders
                for (size_t i = 0; i < g_numerics.size(); ++i) {
                    if (g_numerics[i].category != 0) continue;
                    char title[128]{};
                    WideCharToMultiByte(CP_UTF8, 0, g_numerics[i].title, -1, title, sizeof(title), nullptr, nullptr);
                    ImGui::SetNextItemWidth(300.0f);
                    if (ImGui::SliderInt(title, &g_numerics[i].value, g_numerics[i].minimum, g_numerics[i].maximum)) {
                        g_dirty = true;
                    }
                    ImGui::Spacing();
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        // ---------------------------------------------------------------------
        // TAB 3: ARSENAL & AMMUNITION
        // ---------------------------------------------------------------------
        if (ImGui::BeginTabItem("  ARSENAL & AMMO  ")) {
            ImGui::Spacing();
            ImGui::BeginChild("##ArsenalList", ImVec2(0, contentHeight), true);
            {
                ImGui::TextColored(ImVec4(0.16f, 0.88f, 0.73f, 1.0f), "WEAPON & AMMUNITION MODIFIERS");
                ImGui::Separator();
                ImGui::Spacing();

                for (size_t i = 0; i < g_features.size(); ++i) {
                    if (g_features[i].category != 1) continue;
                    char title[128]{};
                    char desc[256]{};
                    WideCharToMultiByte(CP_UTF8, 0, g_features[i].title, -1, title, sizeof(title), nullptr, nullptr);
                    WideCharToMultiByte(CP_UTF8, 0, g_features[i].description, -1, desc, sizeof(desc), nullptr, nullptr);

                    if (ImGui::Checkbox(title, &g_features[i].enabled)) {
                        EnforceExclusive(static_cast<int>(i));
                        g_dirty = true;
                    }
                    ImGui::SameLine(320);
                    ImGui::TextDisabled("%s", desc);
                    ImGui::Spacing();
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        // ---------------------------------------------------------------------
        // TAB 4: WORLD & LOOT AUTOMATION
        // ---------------------------------------------------------------------
        if (ImGui::BeginTabItem("  WORLD & LOOT  ")) {
            ImGui::Spacing();
            ImGui::BeginChild("##WorldList", ImVec2(0, contentHeight), true);
            {
                ImGui::TextColored(ImVec4(0.16f, 0.88f, 0.73f, 1.0f), "LOOT TELEPORT & VACUUM");
                ImGui::Separator();
                ImGui::Spacing();

                for (size_t i = 0; i < g_features.size(); ++i) {
                    if (g_features[i].category != 2) continue;
                    char title[128]{};
                    char desc[256]{};
                    WideCharToMultiByte(CP_UTF8, 0, g_features[i].title, -1, title, sizeof(title), nullptr, nullptr);
                    WideCharToMultiByte(CP_UTF8, 0, g_features[i].description, -1, desc, sizeof(desc), nullptr, nullptr);

                    if (ImGui::Checkbox(title, &g_features[i].enabled)) {
                        EnforceExclusive(static_cast<int>(i));
                        g_dirty = true;
                    }
                    ImGui::SameLine(320);
                    ImGui::TextDisabled("%s", desc);
                    ImGui::Spacing();
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                for (size_t i = 0; i < g_numerics.size(); ++i) {
                    if (g_numerics[i].category != 2) continue;
                    char title[128]{};
                    WideCharToMultiByte(CP_UTF8, 0, g_numerics[i].title, -1, title, sizeof(title), nullptr, nullptr);
                    ImGui::SetNextItemWidth(300.0f);
                    if (ImGui::SliderInt(title, &g_numerics[i].value, g_numerics[i].minimum, g_numerics[i].maximum)) {
                        g_dirty = true;
                    }
                    ImGui::Spacing();
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        // ---------------------------------------------------------------------
        // TAB 5: VIEW & CAMERA
        // ---------------------------------------------------------------------
        if (ImGui::BeginTabItem("  VIEW & CAMERA  ")) {
            ImGui::Spacing();
            ImGui::BeginChild("##ViewList", ImVec2(0, contentHeight), true);
            {
                ImGui::TextColored(ImVec4(0.16f, 0.88f, 0.73f, 1.0f), "CAMERA & FIELD OF VIEW");
                ImGui::Separator();
                ImGui::Spacing();

                for (size_t i = 0; i < g_features.size(); ++i) {
                    if (g_features[i].category != 3) continue;
                    char title[128]{};
                    char desc[256]{};
                    WideCharToMultiByte(CP_UTF8, 0, g_features[i].title, -1, title, sizeof(title), nullptr, nullptr);
                    WideCharToMultiByte(CP_UTF8, 0, g_features[i].description, -1, desc, sizeof(desc), nullptr, nullptr);

                    if (ImGui::Checkbox(title, &g_features[i].enabled)) {
                        EnforceExclusive(static_cast<int>(i));
                        g_dirty = true;
                    }
                    ImGui::SameLine(320);
                    ImGui::TextDisabled("%s", desc);
                    ImGui::Spacing();
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                for (size_t i = 0; i < g_numerics.size(); ++i) {
                    if (g_numerics[i].category != 3) continue;
                    char title[128]{};
                    WideCharToMultiByte(CP_UTF8, 0, g_numerics[i].title, -1, title, sizeof(title), nullptr, nullptr);
                    ImGui::SetNextItemWidth(300.0f);
                    if (ImGui::SliderInt(title, &g_numerics[i].value, g_numerics[i].minimum, g_numerics[i].maximum)) {
                        g_dirty = true;
                    }
                    ImGui::Spacing();
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    // =========================================================================
    // 3. BOTTOM STATUS & PATH BAR
    // =========================================================================
    {
        ImGui::SetCursorPosY(height - footerHeight);
        ImGui::Separator();
        ImGui::Spacing();

        // Status text
        char statusUtf8[256]{};
        WideCharToMultiByte(CP_UTF8, 0, g_status.c_str(), -1, statusUtf8, sizeof(statusUtf8), nullptr, nullptr);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(0.16f, 0.88f, 0.73f, 1.0f), "STATUS: ");
        ImGui::SameLine();
        ImGui::TextUnformatted(statusUtf8);

        // Path folder browse button
        ImGui::SameLine(width - 240.0f);
        if (ImGui::Button("WARFRAME PATH...", ImVec2(130, 24))) {
            BrowseGameDirectory();
        }

        // Quick profile mirror toggle
        ImGui::SameLine(width - 96.0f);
        if (ImGui::Checkbox("RAGE", &g_mirror_rage)) {
            g_dirty = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Mirror settings to OwHelper / rage_Cheats profile");
        }
    }

    ImGui::End();

    // Render frame via Direct3D 11
    ImGui::Render();
    const float clearColor[4] = { 0.07f, 0.08f, 0.10f, 1.0f };
    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
    g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColor);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_pSwapChain->Present(1, 0); // VSync
}

} // namespace wfmaxon
