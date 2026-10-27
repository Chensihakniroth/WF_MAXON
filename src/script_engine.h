// ============================================================================
// WF_MAXON — Script Engine (Lua + wf bindings)
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>

namespace wfmaxon {

// Forward-declare the engine so bindings can reach it.
class LiveEngine;

struct LuaHookContext {
    uintptr_t rcx = 0;
    uintptr_t rdx = 0;
    uintptr_t r8 = 0;
    uintptr_t r9 = 0;
    uintptr_t rax = 0;
    uintptr_t r10 = 0;
    uintptr_t r11 = 0;
    uintptr_t rbx = 0;
    uintptr_t rbp = 0;
    uintptr_t rdi = 0;
    uintptr_t rsi = 0;
    uintptr_t r12 = 0;
    uintptr_t r13 = 0;
    uintptr_t r14 = 0;
    uintptr_t r15 = 0;
    uintptr_t rsp = 0;
    float xmm[16][4]{};
    uint8_t skip = 0;
    uint8_t reserved[7]{};
    uintptr_t trampoline = 0;
};
static_assert(offsetof(LuaHookContext, rsp) == 0x78);
static_assert(offsetof(LuaHookContext, xmm) == 0x80);
static_assert(offsetof(LuaHookContext, skip) == 0x180);
static_assert(offsetof(LuaHookContext, trampoline) == 0x188);

class ScriptEngine {
public:
    ScriptEngine();
    ~ScriptEngine();

    bool Initialize(LiveEngine* engine, const std::wstring& game_dir);
    void Shutdown();

    bool ReloadScripts();
    bool LoadScriptsFromFolder(const std::wstring& folder);
    bool ExecuteScript(const std::string& code, std::string* error_out = nullptr);
    bool ExecuteScriptFile(const std::wstring& path, std::string* error_out = nullptr);
    void ResetEnvironment();
    void ClearLog();
    void UninstallAllHooks();

    std::wstring GetCustomHooksDir() const;
    void SetCustomHooksDir(const std::wstring& dir);

    const std::wstring& Status() const { return status_; }
    int HookCount() const { return static_cast<int>(hooks_.size()); }
    int ScriptCount() const { return static_cast<int>(loaded_scripts_.size()); }

    // Called from the detour stub on the game thread.
    static void DispatchHook(int hook_id, LuaHookContext* ctx);

    // Hook management (public so main.cpp can drive reload).
    int InstallHook(uintptr_t address, int lua_ref);
    bool UninstallHook(int hook_id);

    // Access to the live engine for bindings.
    LiveEngine* GetEngine() const { return engine_; }

private:
    friend class LuaBindingGuard;

    // Hook registry entry. Declared before use: MSVC requires the nested type
    // to be named before a member declaration takes it as a parameter.
    struct LuaHook {
        uintptr_t target = 0;
        uintptr_t trampoline = 0;
        uintptr_t thunk = 0;
        uint8_t original_bytes[32]{};
        size_t patch_len = 0;
        int lua_ref = -1;       // registry reference to the callback
        int id = -1;
        bool active = false;
        LONG fire_count = 0;    // updated via InterlockedIncrement
    };

    // Hook management. RegisterWfModule is an internal helper defined in the
    // implementation; the Lua state is only handled as an opaque pointer here.
    void CleanupHook(LuaHook& hook);

    LiveEngine* engine_ = nullptr;
    std::wstring game_dir_;
    std::wstring custom_scripts_dir_;
    std::wstring status_;

    // Lua state (protected by mutex).
    mutable std::mutex lua_mutex_;
    // Opaque Lua state pointer (lua_State*).
    void* lua_state_ = nullptr;

    std::vector<LuaHook> hooks_;
    std::atomic<int> next_hook_id_{1};

    // Loaded script metadata.
    struct LoadedScript {
        std::wstring path;
        std::wstring name;
        bool ok = false;
        std::string error;
    };
    std::vector<LoadedScript> loaded_scripts_;

    // on_unload callbacks (registry references).
    std::vector<int> unload_refs_;

    // Hook instruction limit (prevents game freezes from runaway Lua).
    static constexpr int kHookInstructionLimit = 8000;

public:
    // Read-only access for the UI.
    const std::vector<LoadedScript>& GetLoadedScripts() const { return loaded_scripts_; }
    const std::vector<LuaHook>& GetHooks() const { return hooks_; }

    // Last N lines from the script log file.
    std::vector<std::wstring> GetScriptLog(int max_lines = 200) const;
};

} // namespace wfmaxon
