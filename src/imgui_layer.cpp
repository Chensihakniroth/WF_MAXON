#include "imgui_layer.h"
#include "config.h"
#include "engine.h"
#include "script_engine.h"

#include <d3d11.h>
#include <dxgi.h>
#include <shellapi.h>
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

struct ScriptFileEntry {
    std::wstring path;
    std::string filename;
    bool enabled = false;
    bool loaded = false;
    std::string error;
    uint64_t size = 0;
};

std::vector<ScriptFileEntry> g_scriptFiles;
bool g_scriptsScanned = false;
char g_scriptFilter[64] = "";
std::string g_currentEditingFile = "";
std::wstring g_currentEditingPath = L"";
bool g_autoScrollLog = true;

static std::wstring Utf8ToWideString(const std::string& str) {
    if (str.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), out.data(), len);
    return out;
}

static std::string WideToUtf8String(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), out.data(), len, nullptr, nullptr);
    return out;
}

static void RescanScriptsFolder() {
    std::wstring folder = g_scripts.GetCustomHooksDir();
    while (!folder.empty() && (folder.back() == L'\\' || folder.back() == L'/')) {
        folder.pop_back();
    }
    if (folder.empty() || GetFileAttributesW(folder.c_str()) == INVALID_FILE_ATTRIBUTES) {
        g_scriptFiles.clear();
        g_scriptsScanned = true;
        return;
    }
    std::vector<ScriptFileEntry> new_list;

    const wchar_t* patterns[] = { L"\\*.lua", L"\\*.pluto" };
    for (const wchar_t* pat : patterns) {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((folder + pat).c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                ScriptFileEntry entry;
                entry.path = folder + L"\\" + fd.cFileName;

                char name_buf[MAX_PATH]{};
                WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name_buf, sizeof(name_buf), nullptr, nullptr);
                entry.filename = name_buf;
                entry.size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;

                for (const auto& old : g_scriptFiles) {
                    if (old.path == entry.path) {
                        entry.enabled = old.enabled;
                        entry.loaded = old.loaded;
                        entry.error = old.error;
                        break;
                    }
                }
                new_list.push_back(std::move(entry));
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }

    std::sort(new_list.begin(), new_list.end(), [](const ScriptFileEntry& a, const ScriptFileEntry& b) {
        return a.filename < b.filename;
    });

    g_scriptFiles = std::move(new_list);
    g_scriptsScanned = true;
}

static void HotReloadActiveScripts() {
    g_scripts.ResetEnvironment();
    int loaded_count = 0;
    int error_count = 0;

    for (auto& item : g_scriptFiles) {
        if (item.enabled) {
            std::string err;
            if (g_scripts.ExecuteScriptFile(item.path, &err)) {
                item.loaded = true;
                item.error.clear();
                ++loaded_count;
            } else {
                item.loaded = false;
                item.error = err;
                ++error_count;
            }
        } else {
            item.loaded = false;
            item.error.clear();
        }
    }

    if (error_count > 0) {
        SetStatus(L"Hot reload completed with errors", RGBX(231, 91, 104));
    } else {
        wchar_t status_buf[128]{};
        swprintf_s(status_buf, L"Hot reloaded %d active scripts (%d hooks active)", loaded_count, g_scripts.HookCount());
        SetStatus(status_buf, RGBX(42, 225, 185));
    }
}

static bool LoadScriptFileToEditor(const std::wstring& path, const std::string& filename) {
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(hFile, nullptr);
    if (size >= sizeof(g_scriptEditorBuffer)) size = sizeof(g_scriptEditorBuffer) - 1;
    DWORD read = 0;
    ReadFile(hFile, g_scriptEditorBuffer, size, &read, nullptr);
    g_scriptEditorBuffer[read] = '\0';
    CloseHandle(hFile);
    g_currentEditingFile = filename;
    g_currentEditingPath = path;
    g_showScriptError = false;
    g_lastScriptError.clear();
    return true;
}

static bool SaveEditorToScriptFile(const std::wstring& path) {
    if (path.empty()) return false;
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD len = static_cast<DWORD>(strlen(g_scriptEditorBuffer));
    DWORD written = 0;
    WriteFile(hFile, g_scriptEditorBuffer, len, &written, nullptr);
    CloseHandle(hFile);
    return true;
}

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
        if (ImGui::BeginTabItem("  LUA SCRIPTS & MANAGER  ")) {
            if (!g_scriptsScanned) {
                RescanScriptsFolder();
            }

            // Keyboard shortcut F5 for hot reload
            if (ImGui::IsKeyPressed(ImGuiKey_F5)) {
                HotReloadActiveScripts();
            }

            ImGui::Spacing();

            // =================================================================
            // ACTION TOOLBAR
            // =================================================================
            if (ImGui::Button("REFRESH (F5)", ImVec2(120, 28))) {
                RescanScriptsFolder();
                SetStatus(L"Scripts directory scanned", RGBX(42, 225, 185));
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rescan scripts directory for new or modified files (F5)");

            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.45f, 0.38f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.16f, 0.60f, 0.50f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.20f, 0.75f, 0.62f, 1.0f));
            if (ImGui::Button("HOT RELOAD ACTIVE", ImVec2(165, 28))) {
                HotReloadActiveScripts();
            }
            ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Cleanly unload and re-execute all currently enabled/ticked scripts");

            ImGui::SameLine();
            if (ImGui::Button("LOAD ALL", ImVec2(90, 28))) {
                for (auto& item : g_scriptFiles) item.enabled = true;
                HotReloadActiveScripts();
            }

            ImGui::SameLine();
            if (ImGui::Button("UNLOAD ALL", ImVec2(100, 28))) {
                for (auto& item : g_scriptFiles) item.enabled = false;
                g_scripts.ResetEnvironment();
                for (auto& item : g_scriptFiles) { item.loaded = false; item.error.clear(); }
                SetStatus(L"All scripts unloaded, hooks removed", RGBX(224, 175, 104));
            }

            ImGui::SameLine();
            if (ImGui::Button("OPEN FOLDER", ImVec2(110, 28))) {
                std::wstring dir = g_scripts.GetCustomHooksDir();
                ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }

            ImGui::SameLine();
            if (ImGui::Button("CLEAR LOG", ImVec2(95, 28))) {
                g_scripts.ClearLog();
            }

            // Folder info on the right
            std::string folderUtf8 = WideToUtf8String(g_scripts.GetCustomHooksDir());
            int activeCount = 0;
            for (const auto& f : g_scriptFiles) if (f.enabled) ++activeCount;
            ImGui::SameLine();
            ImGui::SetCursorPosX(width - 380.0f);
            ImGui::TextDisabled("%d / %d active | %s", activeCount, static_cast<int>(g_scriptFiles.size()), folderUtf8.c_str());

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Two-column layout:
            float leftWidth = 380.0f;
            if (width < 900.0f) leftWidth = width * 0.40f;
            float rightWidth = width - leftWidth - 42.0f;
            float panelHeight = contentHeight - 48.0f;

            // =================================================================
            // LEFT PANEL: SCRIPT BROWSER & TICK MANAGER
            // =================================================================
            ImGui::BeginChild("##ScriptBrowserChild", ImVec2(leftWidth, panelHeight), true);
            {
                ImGui::TextColored(ImVec4(0.16f, 0.88f, 0.73f, 1.0f), "SCRIPTS LIST");
                ImGui::SameLine();
                ImGui::TextDisabled("(tick to load/unload)");

                // Filter box
                ImGui::SetNextItemWidth(-1);
                ImGui::InputTextWithHint("##ScriptFilter", "Filter scripts...", g_scriptFilter, sizeof(g_scriptFilter));

                ImGui::Separator();

                // Scripts Table
                float tableHeight = panelHeight - 75.0f;
                if (ImGui::BeginTable("##ScriptTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, tableHeight))) {
                    ImGui::TableSetupColumn("Active", ImGuiTableColumnFlags_WidthFixed, 42.0f);
                    ImGui::TableSetupColumn("Script Name", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 65.0f);
                    ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 48.0f);
                    ImGui::TableHeadersRow();

                    std::string filterLower = g_scriptFilter;
                    std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(), [](unsigned char c) { return (char)::tolower(c); });

                    for (size_t i = 0; i < g_scriptFiles.size(); ++i) {
                        auto& item = g_scriptFiles[i];

                        // Filter matching
                        if (!filterLower.empty()) {
                            std::string nameLower = item.filename;
                            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), [](unsigned char c) { return (char)::tolower(c); });
                            if (nameLower.find(filterLower) == std::string::npos) {
                                continue;
                            }
                        }

                        ImGui::PushID(static_cast<int>(i));
                        ImGui::TableNextRow();

                        // Col 0: Checkbox
                        ImGui::TableSetColumnIndex(0);
                        if (ImGui::Checkbox("##toggle", &item.enabled)) {
                            if (item.enabled) {
                                std::string err;
                                if (g_scripts.ExecuteScriptFile(item.path, &err)) {
                                    item.loaded = true;
                                    item.error.clear();
                                    SetStatus(L"Loaded: " + Utf8ToWideString(item.filename), RGBX(42, 225, 185));
                                } else {
                                    item.loaded = false;
                                    item.error = err;
                                    SetStatus(L"Error: " + Utf8ToWideString(item.filename), RGBX(231, 91, 104));
                                }
                            } else {
                                HotReloadActiveScripts();
                                SetStatus(L"Unloaded: " + Utf8ToWideString(item.filename), RGBX(224, 175, 104));
                            }
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tick to load and run; untick to unload");

                        // Col 1: Name (clickable to open in editor)
                        ImGui::TableSetColumnIndex(1);
                        bool isCurrent = (g_currentEditingFile == item.filename);
                        if (isCurrent) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.16f, 0.88f, 0.73f, 1.0f));
                        }
                        if (ImGui::Selectable(item.filename.c_str(), isCurrent, 0)) {
                            LoadScriptFileToEditor(item.path, item.filename);
                        }
                        if (isCurrent) {
                            ImGui::PopStyleColor();
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("Click to inspect and edit %s in the editor", item.filename.c_str());
                        }

                        // Col 2: Status Badge
                        ImGui::TableSetColumnIndex(2);
                        if (item.enabled && item.loaded) {
                            ImGui::TextColored(ImVec4(0.16f, 0.88f, 0.73f, 1.0f), "[ACTIVE]");
                        } else if (item.enabled && !item.loaded) {
                            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.40f, 1.0f), "[ERROR]");
                            if (ImGui::IsItemHovered() && !item.error.empty()) {
                                ImGui::SetTooltip("Error: %s", item.error.c_str());
                            }
                        } else {
                            ImGui::TextDisabled("[IDLE]");
                        }

                        // Col 3: Edit Button
                        ImGui::TableSetColumnIndex(3);
                        if (ImGui::SmallButton("Edit")) {
                            LoadScriptFileToEditor(item.path, item.filename);
                        }

                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }

                // Summary footer
                ImGui::Separator();
                ImGui::TextDisabled("Active: %d / %d | Hooks: %d", activeCount, static_cast<int>(g_scriptFiles.size()), g_scripts.HookCount());
            }
            ImGui::EndChild();

            ImGui::SameLine();

            // =================================================================
            // RIGHT PANEL: EDITOR (TOP) + HOOKS & LOGS (BOTTOM)
            // =================================================================
            ImGui::BeginChild("##ScriptWorkChild", ImVec2(rightWidth, panelHeight), false);
            {
                float editorHeight = (panelHeight - 12.0f) * 0.55f;
                float bottomHeight = panelHeight - editorHeight - 12.0f;

                // --- Top: Editor ---
                ImGui::BeginChild("##EditorChild", ImVec2(0, editorHeight), true);
                {
                    // Editor header & actions
                    if (!g_currentEditingFile.empty()) {
                        ImGui::TextColored(ImVec4(0.16f, 0.88f, 0.73f, 1.0f), "EDITING: %s", g_currentEditingFile.c_str());
                    } else {
                        ImGui::TextColored(ImVec4(0.85f, 0.88f, 0.92f, 1.0f), "SCRATCHPAD / USER BUFFER");
                    }

                    ImGui::SameLine();
                    float btnPos = rightWidth - 360.0f;
                    if (btnPos > 200.0f) ImGui::SetCursorPosX(btnPos);

                    if (ImGui::Button("RUN BUFFER (Ctrl+Enter)", ImVec2(170, 22))) {
                        std::string err;
                        if (g_scripts.ExecuteScript(g_scriptEditorBuffer, &err)) {
                            g_showScriptError = false;
                            g_lastScriptError.clear();
                            SetStatus(L"Script executed successfully", RGBX(42, 225, 185));
                        } else {
                            g_showScriptError = true;
                            g_lastScriptError = err;
                            SetStatus(L"Script runtime error", RGBX(231, 91, 104));
                        }
                    }

                    ImGui::SameLine();
                    if (!g_currentEditingPath.empty()) {
                        if (ImGui::Button("SAVE (Ctrl+S)", ImVec2(100, 22))) {
                            if (SaveEditorToScriptFile(g_currentEditingPath)) {
                                SetStatus(L"Saved " + Utf8ToWideString(g_currentEditingFile), RGBX(42, 225, 185));
                                // If enabled, reload it
                                for (auto& f : g_scriptFiles) {
                                    if (f.path == g_currentEditingPath && f.enabled) {
                                        HotReloadActiveScripts();
                                        break;
                                    }
                                }
                            } else {
                                SetStatus(L"Save failed!", RGBX(231, 91, 104));
                            }
                        }
                    } else {
                        ImGui::BeginDisabled();
                        ImGui::Button("SAVE (Ctrl+S)", ImVec2(100, 22));
                        ImGui::EndDisabled();
                    }

                    ImGui::SameLine();
                    if (ImGui::Button("NEW", ImVec2(50, 22))) {
                        g_scriptEditorBuffer[0] = '\0';
                        g_currentEditingFile.clear();
                        g_currentEditingPath.clear();
                        g_showScriptError = false;
                        g_lastScriptError.clear();
                    }

                    ImGui::Separator();

                    if (g_showScriptError && !g_lastScriptError.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.35f, 0.40f, 1.0f));
                        ImGui::TextWrapped("Error: %s", g_lastScriptError.c_str());
                        ImGui::PopStyleColor();
                        ImGui::Separator();
                    }

                    // Keyboard shortcuts for Ctrl+Enter and Ctrl+S
                    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S) && !g_currentEditingPath.empty()) {
                        SaveEditorToScriptFile(g_currentEditingPath);
                        HotReloadActiveScripts();
                    }

                    ImGuiInputTextFlags flags = ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CtrlEnterForNewLine;
                    ImGui::InputTextMultiline("##ScriptCode", g_scriptEditorBuffer, sizeof(g_scriptEditorBuffer),
                                             ImVec2(-1, -1), flags);
                }
                ImGui::EndChild();

                // --- Bottom: Tabs for Hooks and Logs ---
                ImGui::BeginChild("##BottomTabsChild", ImVec2(0, bottomHeight), false);
                {
                    if (ImGui::BeginTabBar("##DiagnosticsTabs", ImGuiTabBarFlags_None)) {
                        // Sub-Tab 1: Active Memory Hooks
                        char hooksTabTitle[64]{};
                        sprintf_s(hooksTabTitle, "  ACTIVE MEMORY HOOKS (%d)  ", g_scripts.HookCount());
                        if (ImGui::BeginTabItem(hooksTabTitle)) {
                            const auto& hooks = g_scripts.GetHooks();
                            if (hooks.empty()) {
                                ImGui::TextDisabled("No active memory detour hooks registered.");
                            } else {
                                if (ImGui::BeginTable("##HooksTbl", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
                                    ImGui::TableSetupColumn("Hook ID", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                                    ImGui::TableSetupColumn("Target Address", ImGuiTableColumnFlags_WidthStretch);
                                    ImGui::TableSetupColumn("Trampoline", ImGuiTableColumnFlags_WidthStretch);
                                    ImGui::TableSetupColumn("Fire Count", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                                    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                                    ImGui::TableHeadersRow();

                                    for (const auto& h : hooks) {
                                        ImGui::TableNextRow();
                                        ImGui::TableSetColumnIndex(0);
                                        ImGui::Text("%d", h.id);

                                        ImGui::TableSetColumnIndex(1);
                                        ImGui::TextColored(ImVec4(0.16f, 0.88f, 0.73f, 1.0f), "0x%llX", h.target);

                                        ImGui::TableSetColumnIndex(2);
                                        ImGui::Text("0x%llX", h.trampoline);

                                        ImGui::TableSetColumnIndex(3);
                                        ImGui::Text("%ld", h.fire_count);

                                        ImGui::TableSetColumnIndex(4);
                                        if (h.active) {
                                            ImGui::TextColored(ImVec4(0.16f, 0.88f, 0.73f, 1.0f), "ACTIVE");
                                        } else {
                                            ImGui::TextDisabled("INACTIVE");
                                        }
                                    }
                                    ImGui::EndTable();
                                }
                            }
                            ImGui::EndTabItem();
                        }

                        // Sub-Tab 2: Script Console Log
                        if (ImGui::BeginTabItem("  SCRIPT CONSOLE LOG  ")) {
                            if (ImGui::Button("CLEAR", ImVec2(70, 20))) {
                                g_scripts.ClearLog();
                            }
                            ImGui::SameLine();
                            if (ImGui::Button("COPY TO CLIPBOARD", ImVec2(140, 20))) {
                                auto logs = g_scripts.GetScriptLog(300);
                                std::string all;
                                for (const auto& l : logs) {
                                    char buf[512]{};
                                    WideCharToMultiByte(CP_UTF8, 0, l.c_str(), -1, buf, sizeof(buf), nullptr, nullptr);
                                    all += buf;
                                }
                                if (!all.empty()) {
                                    ImGui::SetClipboardText(all.c_str());
                                    SetStatus(L"Log copied to clipboard", RGBX(42, 225, 185));
                                }
                            }
                            ImGui::SameLine();
                            ImGui::Checkbox("Auto-Scroll", &g_autoScrollLog);

                            ImGui::Separator();

                            ImGui::BeginChild("##LogScrollRegion", ImVec2(0, 0), false);
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
                                if (g_autoScrollLog && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
                                    ImGui::SetScrollHereY(1.0f);
                                }
                            }
                            ImGui::EndChild();

                            ImGui::EndTabItem();
                        }

                        ImGui::EndTabBar();
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
