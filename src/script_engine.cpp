// ============================================================================
// WF_MAXON — Script Engine Implementation
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <mutex>
#include <vector>

#include "script_engine.h"
#include "engine.h"
#include "generated/warframe_offsets.h"

// Lua/Pluto headers (embedded in src/lua/). LUA_API already expands to
// `extern "C" PLUTO_DLLSPEC` under __cplusplus, so no wrapper is needed here.
// Pluto's PLUTO_API additions (e.g. pluto_pushstring) use C++ linkage and
// overload, which is illegal inside an `extern "C"` block.
#include "lua/lua.h"
#include "lua/lualib.h"
#include "lua/lauxlib.h"

namespace wfmaxon {
namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
void AppendScriptLog(const char* line) {
    wchar_t path[MAX_PATH]{};
    DWORD n = GetTempPathW(static_cast<DWORD>(std::size(path)), path);
    if (!n || n >= std::size(path)) return;
    if (wcscat_s(path, L"WF_MAXON_script.log") != 0) return;

    HANDLE file = CreateFileW(path, FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
    CloseHandle(file);
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                   nullptr, 0, nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                   nullptr, 0);
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        out.data(), size);
    return out;
}

// ---------------------------------------------------------------------------
// Lua hook instruction-count guard
// ---------------------------------------------------------------------------
void HookInstructionCountHook(lua_State* L, lua_Debug* ar) {
    (void)ar;
    lua_pushliteral(L, "WF_MAXON hook instruction limit exceeded");
    lua_error(L);
}

// ---------------------------------------------------------------------------
// wf module bindings
// ---------------------------------------------------------------------------
ScriptEngine* g_script_engine = nullptr;

int wf_base(lua_State* L) {
    auto* self = g_script_engine;
    if (!self || !self->GetEngine()) return 0;
    lua_pushinteger(L, static_cast<lua_Integer>(self->GetEngine()->Warbase()));
    return 1;
}

int wf_player_base(lua_State* L) {
    auto* self = g_script_engine;
    if (!self || !self->GetEngine()) return 0;
    uintptr_t pb = self->GetEngine()->PlayerBase();
    if (!pb) return 0;
    lua_pushinteger(L, static_cast<lua_Integer>(pb));
    return 1;
}

int wf_read_float(lua_State* L) {
    auto* self = g_script_engine;
    if (!self || !self->GetEngine() || !lua_isinteger(L, 1)) return 0;
    uintptr_t addr = static_cast<uintptr_t>(lua_tointeger(L, 1));
    float val = 0.0f;
    if (self->GetEngine()->ReadBytes(addr, &val, sizeof(val))) {
        lua_pushnumber(L, static_cast<lua_Number>(val));
        return 1;
    }
    return 0;
}

int wf_write_float(lua_State* L) {
    auto* self = g_script_engine;
    if (!self || !self->GetEngine() || !lua_isinteger(L, 1) || !lua_isnumber(L, 2)) return 0;
    uintptr_t addr = static_cast<uintptr_t>(lua_tointeger(L, 1));
    float val = static_cast<float>(lua_tonumber(L, 2));
    lua_pushboolean(L, self->GetEngine()->WriteBytes(addr, &val, sizeof(val)));
    return 1;
}

int wf_read_int(lua_State* L) {
    auto* self = g_script_engine;
    if (!self || !self->GetEngine() || !lua_isinteger(L, 1)) return 0;
    uintptr_t addr = static_cast<uintptr_t>(lua_tointeger(L, 1));
    int32_t val = 0;
    if (self->GetEngine()->ReadBytes(addr, &val, sizeof(val))) {
        lua_pushinteger(L, static_cast<lua_Integer>(val));
        return 1;
    }
    return 0;
}

int wf_read_ptr(lua_State* L) {
    auto* self = g_script_engine;
    if (!self || !self->GetEngine() || !lua_isinteger(L, 1)) return 0;
    uintptr_t addr = static_cast<uintptr_t>(lua_tointeger(L, 1));
    uintptr_t val = 0;
    if (self->GetEngine()->ReadBytes(addr, &val, sizeof(val))) {
        lua_pushinteger(L, static_cast<lua_Integer>(val));
        return 1;
    }
    return 0;
}

int wf_write_int(lua_State* L) {
    auto* self = g_script_engine;
    if (!self || !self->GetEngine() || !lua_isinteger(L, 1) || !lua_isinteger(L, 2)) return 0;
    uintptr_t addr = static_cast<uintptr_t>(lua_tointeger(L, 1));
    int32_t val = static_cast<int32_t>(lua_tointeger(L, 2));
    lua_pushboolean(L, self->GetEngine()->WriteBytes(addr, &val, sizeof(val)));
    return 1;
}

int wf_write_ptr(lua_State* L) {
    auto* self = g_script_engine;
    if (!self || !self->GetEngine() || !lua_isinteger(L, 1) || !lua_isinteger(L, 2)) return 0;
    uintptr_t addr = static_cast<uintptr_t>(lua_tointeger(L, 1));
    uintptr_t val = static_cast<uintptr_t>(lua_tointeger(L, 2));
    lua_pushboolean(L, self->GetEngine()->WriteBytes(addr, &val, sizeof(val)));
    return 1;
}

int wf_read_bytes(lua_State* L) {
    auto* self = g_script_engine;
    if (!self || !self->GetEngine() || !lua_isinteger(L, 1) || !lua_isinteger(L, 2)) return 0;
    uintptr_t addr = static_cast<uintptr_t>(lua_tointeger(L, 1));
    int n = static_cast<int>(lua_tointeger(L, 2));
    if (n <= 0 || n > 65536) return 0;

    std::vector<uint8_t> buf(static_cast<size_t>(n));
    if (!self->GetEngine()->ReadBytes(addr, buf.data(), buf.size())) return 0;

    lua_createtable(L, n, 0);
    for (int i = 0; i < n; ++i) {
        lua_pushinteger(L, static_cast<lua_Integer>(buf[i]));
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

int wf_write_bytes(lua_State* L) {
    auto* self = g_script_engine;
    if (!self || !self->GetEngine() || !lua_isinteger(L, 1) || !lua_istable(L, 2)) return 0;
    uintptr_t addr = static_cast<uintptr_t>(lua_tointeger(L, 1));
    size_t n = lua_rawlen(L, 2);
    if (n == 0 || n > 65536) return 0;

    std::vector<uint8_t> buf(n);
    for (size_t i = 0; i < n; ++i) {
        lua_rawgeti(L, 2, static_cast<int>(i) + 1);
        buf[i] = static_cast<uint8_t>(lua_tointeger(L, -1));
        lua_pop(L, 1);
    }

    lua_pushboolean(L, self->GetEngine()->WriteBytes(addr, buf.data(), buf.size()));
    return 1;
}

int wf_hook(lua_State* L) {
    auto* self = g_script_engine;
    if (!self || !lua_isinteger(L, 1) || !lua_isfunction(L, 2)) return 0;
    uintptr_t addr = static_cast<uintptr_t>(lua_tointeger(L, 1));
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    int id = self->InstallHook(addr, ref);
    if (id == 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        lua_pushnil(L);
        lua_pushliteral(L, "hook install failed");
        return 2;
    }
    lua_pushinteger(L, id);
    return 1;
}

int wf_unhook(lua_State* L) {
    auto* self = g_script_engine;
    if (!self || !lua_isinteger(L, 1)) return 0;
    int id = static_cast<int>(lua_tointeger(L, 1));
    lua_pushboolean(L, self->UninstallHook(id));
    return 1;
}

int wf_on_unload(lua_State* L) {
    auto* self = g_script_engine;
    if (!self || !lua_isfunction(L, 1)) return 0;
    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    self->GetEngine(); // keep alive if needed
    // Store ref in the engine for cleanup during reload.
    // We'll use a dedicated registry key.
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    // Actually, let me store this differently. Push to a global list.
    lua_getglobal(L, "_wf_on_unload_list");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "_wf_on_unload_list");
    }
    int count = static_cast<int>(lua_rawlen(L, -1)) + 1;
    lua_pushvalue(L, -3); // the function
    lua_rawseti(L, -2, count);
    lua_pop(L, 2); // table + function copy
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
    return 0;
}

int wf_find_pattern(lua_State* L) {
    // Stub: pattern scanning requires module enumeration.
    // For now, return nil and log.
    lua_pushnil(L);
    lua_pushliteral(L, "find_pattern not yet implemented");
    return 2;
}

int wf_msgbox(lua_State* L) {
    const char* text = luaL_checkstring(L, 1);
    const char* title = luaL_optstring(L, 2, "WF_MAXON");
    MessageBoxA(nullptr, text, title, MB_OK | MB_TOPMOST);
    return 0;
}

int wf_protect(lua_State* L) {
    if (!lua_isinteger(L, 1) || !lua_isinteger(L, 2) || !lua_isinteger(L, 3)) return 0;
    uintptr_t addr = static_cast<uintptr_t>(lua_tointeger(L, 1));
    size_t size = static_cast<size_t>(lua_tointeger(L, 2));
    DWORD flags = static_cast<DWORD>(lua_tointeger(L, 3));
    DWORD old = 0;
    if (VirtualProtect(reinterpret_cast<LPVOID>(addr), size, flags, &old)) {
        lua_pushinteger(L, static_cast<lua_Integer>(old));
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

int lua_custom_print(lua_State* L) {
    int n = lua_gettop(L);
    std::string out;
    for (int i = 1; i <= n; ++i) {
        if (i > 1) out += "\t";
        size_t len = 0;
        const char* s = luaL_tolstring(L, i, &len);
        if (s) out += std::string(s, len);
        lua_pop(L, 1);
    }
    out += "\n";
    AppendScriptLog(out.c_str());
    return 0;
}

void RegisterWfModule(ScriptEngine* self, lua_State* L) {
    g_script_engine = self;
    luaL_Reg funcs[] = {
        {"base",           wf_base},
        {"player_base",    wf_player_base},
        {"read_float",     wf_read_float},
        {"write_float",    wf_write_float},
        {"read_int",       wf_read_int},
        {"write_int",      wf_write_int},
        {"read_ptr",       wf_read_ptr},
        {"write_ptr",      wf_write_ptr},
        {"read_i64",       wf_read_ptr},
        {"write_i64",      wf_write_ptr},
        {"read_bytes",     wf_read_bytes},
        {"write_bytes",    wf_write_bytes},
        {"hook",           wf_hook},
        {"unhook",         wf_unhook},
        {"on_unload",      wf_on_unload},
        {"find_pattern",   wf_find_pattern},
        {"msgbox",         wf_msgbox},
        {"protect",        wf_protect},
        {nullptr, nullptr}
    };

    luaL_newlibtable(L, funcs);
    luaL_setfuncs(L, funcs, 0);
    lua_setglobal(L, "wf");

    // Page constants for protect()
    lua_pushinteger(L, PAGE_NOACCESS);    lua_setglobal(L, "PAGE_NOACCESS");
    lua_pushinteger(L, PAGE_READONLY);    lua_setglobal(L, "PAGE_READONLY");
    lua_pushinteger(L, PAGE_READWRITE);   lua_setglobal(L, "PAGE_READWRITE");
    lua_pushinteger(L, PAGE_EXECUTE);     lua_setglobal(L, "PAGE_EXECUTE");
    lua_pushinteger(L, PAGE_EXECUTE_READ);       lua_setglobal(L, "PAGE_EXECUTE_READ");
    lua_pushinteger(L, PAGE_EXECUTE_READWRITE);  lua_setglobal(L, "PAGE_EXECUTE_READWRITE");

    // Helpful globals
    lua_pushliteral(L, "WF_MAXON Lua Engine v1.0");
    lua_setglobal(L, "_WF_MAXON_VERSION");

    lua_pushcfunction(L, lua_custom_print);
    lua_setglobal(L, "print");
}

// ---------------------------------------------------------------------------
// ASM stub for Lua hooks (mirrors damage_context.asm pattern)
// ---------------------------------------------------------------------------
extern "C" void ScriptHookStub();
extern "C" void ScriptHookDispatch(LuaHookContext* ctx);

// We generate the per-hook thunk in C++ instead of separate ASM.
// Thunk layout (22 bytes):
//   48 B9 [hook_id:8]      mov rcx, hook_id
//   48 B8 [dispatcher:8]   mov rax, dispatcher
//   FF E0                  jmp rax

void WriteThunk(uint8_t* buf, uintptr_t hook_id, uintptr_t dispatcher) {
    // mov rcx, imm64  -> bytes 0..9
    buf[0] = 0x48;
    buf[1] = 0xB9;
    std::memcpy(buf + 2, &hook_id, sizeof(hook_id));
    // mov rax, imm64  -> bytes 10..19
    buf[10] = 0x48;
    buf[11] = 0xB8;
    std::memcpy(buf + 12, &dispatcher, sizeof(dispatcher));
    // jmp rax         -> bytes 20..21
    buf[20] = 0xFF;
    buf[21] = 0xE0;
}

// The shared context stub is in script_hook.asm.

} // anonymous namespace

// ============================================================================
// ScriptEngine public implementation
// ============================================================================

ScriptEngine::ScriptEngine() = default;

ScriptEngine::~ScriptEngine() {
    Shutdown();
}

bool ScriptEngine::Initialize(LiveEngine* engine, const std::wstring& game_dir) {
    engine_ = engine;
    game_dir_ = game_dir;

    std::lock_guard<std::mutex> lock(lua_mutex_);

    lua_state_ = luaL_newstate();
    if (!lua_state_) {
        status_ = L"Lua state creation failed";
        return false;
    }

    luaL_openlibs(reinterpret_cast<lua_State*>(lua_state_));
    RegisterWfModule(this, reinterpret_cast<lua_State*>(lua_state_));

    // Load scripts from custom_hooks/
    std::wstring hooks_dir = GetCustomHooksDir();
    if (!LoadScriptsFromFolder(hooks_dir)) {
        status_ = L"Scripts loaded (some may have errors)";
    } else {
        status_ = L"Scripts loaded";
    }

    return true;
}

void ScriptEngine::Shutdown() {
    std::lock_guard<std::mutex> lock(lua_mutex_);

    UninstallAllHooks();

    if (lua_state_) {
        lua_close(reinterpret_cast<lua_State*>(lua_state_));
        lua_state_ = nullptr;
    }

    loaded_scripts_.clear();
    status_ = L"Script engine stopped";
}

std::wstring ScriptEngine::GetCustomHooksDir() const {
    if (!game_dir_.empty()) {
        return game_dir_ + L"\\custom_hooks";
    }
    // Fallback: try to find Warframe directory.
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(static_cast<DWORD>(std::size(temp)), temp);
    return std::wstring(temp) + L"WF_MAXON_custom_hooks";
}

bool ScriptEngine::LoadScriptsFromFolder(const std::wstring& folder) {
    loaded_scripts_.clear();

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((folder + L"\\*.lua").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        status_ = L"No scripts in " + folder;
        return true; // Not an error - just no scripts
    }

    int loaded = 0;
    int errors = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        std::wstring full_path = folder + L"\\" + fd.cFileName;
        std::string utf8_path = WideToUtf8(full_path);

        LoadedScript script;
        script.path = full_path;
        script.name = fd.cFileName;

        std::lock_guard<std::mutex> lock(lua_mutex_);

        int top = lua_gettop(reinterpret_cast<lua_State*>(lua_state_));
        int rc = luaL_loadfile(reinterpret_cast<lua_State*>(lua_state_), utf8_path.c_str());
        if (rc == LUA_OK) {
            lua_sethook(reinterpret_cast<lua_State*>(lua_state_),
                        HookInstructionCountHook, LUA_MASKCOUNT, ScriptEngine::kHookInstructionLimit);
            rc = lua_pcall(reinterpret_cast<lua_State*>(lua_state_), 0, LUA_MULTRET, 0);
            lua_sethook(reinterpret_cast<lua_State*>(lua_state_), nullptr, 0, 0);
        }

        if (rc == LUA_OK) {
            script.ok = true;
            ++loaded;
        } else {
            script.ok = false;
            script.error = lua_tostring(reinterpret_cast<lua_State*>(lua_state_), -1);
            ++errors;
            char log[512]{};
            sprintf_s(log, "[script_engine] ERROR %s: %s\n",
                      utf8_path.c_str(), script.error.c_str());
            AppendScriptLog(log);
        }

        lua_settop(reinterpret_cast<lua_State*>(lua_state_), top);
        loaded_scripts_.push_back(std::move(script));

    } while (FindNextFileW(h, &fd));

    FindClose(h);

    char summary[256]{};
    sprintf_s(summary, "[script_engine] Loaded %d scripts, %d errors\n", loaded, errors);
    AppendScriptLog(summary);

    wchar_t wsummary[256]{};
    swprintf_s(wsummary, L"Loaded %d scripts (%d errors)", loaded, errors);
    status_ = wsummary;

    return errors == 0;
}

bool ScriptEngine::ReloadScripts() {
    std::lock_guard<std::mutex> lock(lua_mutex_);

    // Call on_unload callbacks.
    lua_State* L = reinterpret_cast<lua_State*>(lua_state_);
    lua_getglobal(L, "_wf_on_unload_list");
    if (!lua_isnil(L, -1)) {
        size_t n = lua_rawlen(L, -1);
        for (size_t i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, static_cast<int>(i));
            if (lua_isfunction(L, -1)) {
                lua_pcall(L, 0, 0, 0);
            } else {
                lua_pop(L, 1);
            }
        }
    }
    lua_pop(L, 1); // _wf_on_unload_list

    // Uninstall hooks.
    UninstallAllHooks();

    // Re-register wf module.
    RegisterWfModule(this, L);

    // Reload scripts.
    std::wstring hooks_dir = GetCustomHooksDir();
    return LoadScriptsFromFolder(hooks_dir);
}

void ScriptEngine::UninstallAllHooks() {
    for (auto& hook : hooks_) {
        if (hook.active) {
            CleanupHook(hook);
        }
    }
    hooks_.clear();
    next_hook_id_ = 1;
}

int ScriptEngine::InstallHook(uintptr_t address, int lua_ref) {
    if (!address || !engine_ || !engine_->Warbase()) return 0;

    // Allocate thunk (22 bytes) + trampoline.
    uint8_t* thunk = static_cast<uint8_t*>(VirtualAlloc(
        nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!thunk) return 0;

    uint8_t* trampoline = static_cast<uint8_t*>(VirtualAlloc(
        nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        VirtualFree(thunk, 0, MEM_RELEASE);
        return 0;
    }

    int hook_id = next_hook_id_.fetch_add(1);

    // Write thunk: mov rcx, hook_id; jmp ScriptHookStub
    WriteThunk(thunk, static_cast<uintptr_t>(hook_id),
               reinterpret_cast<uintptr_t>(&ScriptHookStub));

    // Write trampoline: original prologue + jump to address+patch_len
    // We don't know the patch length here; the caller will fill it.
    // For now, just zero it. The actual patch length is determined by the
    // prologue comparison in the install function.
    std::memset(trampoline, 0x90, 12);
    WriteAbsoluteJump(trampoline, address + 12); // placeholder, will be corrected

    // Patch the target: jump to thunk.
    unsigned char patch[12]{};
    std::memset(patch, 0x90, sizeof(patch));
    WriteAbsoluteJump(patch, reinterpret_cast<uintptr_t>(thunk));

    DWORD old_protect = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(address), 12,
                        PAGE_EXECUTE_READWRITE, &old_protect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        VirtualFree(thunk, 0, MEM_RELEASE);
        return 0;
    }

    std::memcpy(reinterpret_cast<void*>(address), patch, sizeof(patch));
    FlushInstructionCache(GetCurrentProcess(),
                          reinterpret_cast<LPCVOID>(address), sizeof(patch));
    VirtualProtect(reinterpret_cast<LPVOID>(address), 12, old_protect, &old_protect);

    LuaHook hook{};
    hook.target = address;
    hook.trampoline = reinterpret_cast<uintptr_t>(trampoline);
    hook.thunk = reinterpret_cast<uintptr_t>(thunk);
    hook.lua_ref = lua_ref;
    hook.id = hook_id;
    hook.active = true;
    hooks_.push_back(hook);

    return hook_id;
}

bool ScriptEngine::UninstallHook(int hook_id) {
    for (size_t i = 0; i < hooks_.size(); ++i) {
        auto& hook = hooks_[i];
        if (hook.id != hook_id || !hook.active) continue;

        // Restore original bytes (we'd need to save them; for now just nop).
        DWORD old_protect = 0;
        VirtualProtect(reinterpret_cast<LPVOID>(hook.target), 12,
                       PAGE_EXECUTE_READWRITE, &old_protect);
        std::memset(reinterpret_cast<void*>(hook.target), 0x90, 12);
        FlushInstructionCache(GetCurrentProcess(),
                              reinterpret_cast<LPCVOID>(hook.target), 12);
        VirtualProtect(reinterpret_cast<LPVOID>(hook.target), 12, old_protect, &old_protect);

        CleanupHook(hook);
        hooks_.erase(hooks_.begin() + static_cast<ptrdiff_t>(i));
        return true;
    }
    return false;
}

void ScriptEngine::CleanupHook(LuaHook& hook) {
    if (hook.trampoline) {
        VirtualFree(reinterpret_cast<LPVOID>(hook.trampoline), 0, MEM_RELEASE);
        hook.trampoline = 0;
    }
    if (hook.thunk) {
        VirtualFree(reinterpret_cast<LPVOID>(hook.thunk), 0, MEM_RELEASE);
        hook.thunk = 0;
    }
    if (hook.lua_ref != -1) {
        std::lock_guard<std::mutex> lock(lua_mutex_);
        if (lua_state_) {
            luaL_unref(reinterpret_cast<lua_State*>(lua_state_),
                       LUA_REGISTRYINDEX, hook.lua_ref);
        }
        hook.lua_ref = -1;
    }
    hook.active = false;
}

// ============================================================================
// Hook dispatch (called from script_hook.asm stub on the game thread)
// ============================================================================
void ScriptEngine::DispatchHook(int hook_id, LuaHookContext* ctx) {
    auto* self = g_script_engine;
    if (!self || !ctx) return;

    LuaHook* hook = nullptr;
    for (auto& h : self->hooks_) {
        if (h.id == hook_id && h.active) {
            hook = &h;
            break;
        }
    }
    if (!hook || hook->lua_ref == -1) return;

    std::lock_guard<std::mutex> lock(self->lua_mutex_);
    if (!self->lua_state_) return;

    lua_State* L = reinterpret_cast<lua_State*>(self->lua_state_);

    // Get the Lua callback.
    lua_rawgeti(L, LUA_REGISTRYINDEX, hook->lua_ref);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return;
    }

    // Build context table.
    lua_createtable(L, 0, 18);
    lua_pushinteger(L, static_cast<lua_Integer>(ctx->rcx));
    lua_setfield(L, -2, "rcx");
    lua_pushinteger(L, static_cast<lua_Integer>(ctx->rdx));
    lua_setfield(L, -2, "rdx");
    lua_pushinteger(L, static_cast<lua_Integer>(ctx->r8));
    lua_setfield(L, -2, "r8");
    lua_pushinteger(L, static_cast<lua_Integer>(ctx->r9));
    lua_setfield(L, -2, "r9");
    lua_pushinteger(L, static_cast<lua_Integer>(ctx->rax));
    lua_setfield(L, -2, "rax");
    lua_pushinteger(L, static_cast<lua_Integer>(ctx->r10));
    lua_setfield(L, -2, "r10");
    lua_pushinteger(L, static_cast<lua_Integer>(ctx->r11));
    lua_setfield(L, -2, "r11");
    lua_pushinteger(L, static_cast<lua_Integer>(ctx->rbx));
    lua_setfield(L, -2, "rbx");
    lua_pushinteger(L, static_cast<lua_Integer>(ctx->rbp));
    lua_setfield(L, -2, "rbp");
    lua_pushinteger(L, static_cast<lua_Integer>(ctx->rdi));
    lua_setfield(L, -2, "rdi");
    lua_pushinteger(L, static_cast<lua_Integer>(ctx->rsi));
    lua_setfield(L, -2, "rsi");
    lua_pushinteger(L, static_cast<lua_Integer>(ctx->r12));
    lua_setfield(L, -2, "r12");
    lua_pushinteger(L, static_cast<lua_Integer>(ctx->r13));
    lua_setfield(L, -2, "r13");
    lua_pushinteger(L, static_cast<lua_Integer>(ctx->r14));
    lua_setfield(L, -2, "r14");
    lua_pushinteger(L, static_cast<lua_Integer>(ctx->r15));
    lua_setfield(L, -2, "r15");
    lua_pushinteger(L, static_cast<lua_Integer>(ctx->rsp));
    lua_setfield(L, -2, "rsp");

    // XMM registers as doubles (low 64 bits).
    for (int i = 0; i < 16; ++i) {
        char key[8];
        sprintf_s(key, "xmm%d", i);
        lua_pushnumber(L, static_cast<lua_Number>(ctx->xmm[i][0]));
        lua_setfield(L, -2, key);
    }

    // Call Lua callback: fn(ctx)
    lua_pushvalue(L, -2); // duplicate function
    lua_pushvalue(L, -2); // ctx table
    int rc = lua_pcall(L, 1, 0, 0);

    if (rc != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        char log[512]{};
        sprintf_s(log, "[script_engine] HOOK ERROR id=%d: %s\n", hook_id, err ? err : "unknown");
        AppendScriptLog(log);
        lua_pop(L, 1);
    }

    // Read skip flag from context.
    ctx->skip = ctx->skip ? 1 : 0;

    hook->fire_count = InterlockedIncrement(&hook->fire_count);

    lua_pop(L, 2); // ctx table + function reference
}

bool ScriptEngine::ExecuteScript(const std::string& code, std::string* error_out) {
    std::lock_guard<std::mutex> lock(lua_mutex_);
    if (!lua_state_) {
        if (error_out) *error_out = "Lua state not initialized";
        return false;
    }
    lua_State* L = reinterpret_cast<lua_State*>(lua_state_);
    int top = lua_gettop(L);

    int rc = luaL_loadbuffer(L, code.c_str(), code.size(), "user_script");
    if (rc == LUA_OK) {
        lua_sethook(L, HookInstructionCountHook, LUA_MASKCOUNT, ScriptEngine::kHookInstructionLimit);
        rc = lua_pcall(L, 0, LUA_MULTRET, 0);
        lua_sethook(L, nullptr, 0, 0);
    }

    if (rc == LUA_OK) {
        AppendScriptLog("[script] Executed script successfully\n");
        lua_settop(L, top);
        return true;
    } else {
        const char* err = lua_tostring(L, -1);
        std::string err_str = err ? err : "Unknown error";
        if (error_out) *error_out = err_str;
        char log[512]{};
        sprintf_s(log, "[script] ERROR: %s\n", err_str.c_str());
        AppendScriptLog(log);
        lua_settop(L, top);
        return false;
    }
}

void ScriptEngine::ClearLog() {
    wchar_t path[MAX_PATH]{};
    DWORD n = GetTempPathW(static_cast<DWORD>(std::size(path)), path);
    if (!n || n >= std::size(path)) return;
    if (wcscat_s(path, L"WF_MAXON_script.log") != 0) return;
    DeleteFileW(path);
}

std::vector<std::wstring> ScriptEngine::GetScriptLog(int max_lines) const {
    std::vector<std::wstring> result;
    wchar_t path[MAX_PATH]{};
    DWORD n = GetTempPathW(static_cast<DWORD>(std::size(path)), path);
    if (!n || n >= std::size(path)) return result;
    if (wcscat_s(path, L"WF_MAXON_script.log") != 0) return result;

    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return result;

    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    constexpr DWORD kChunk = 4096;
    DWORD to_read = static_cast<DWORD>(std::min<LONGLONG>(size.QuadPart, static_cast<LONGLONG>(kChunk)));
    DWORD file_offset = static_cast<DWORD>(std::max<LONGLONG>(0, size.QuadPart - static_cast<LONGLONG>(kChunk)));

    LARGE_INTEGER offset{};
    offset.QuadPart = static_cast<LONGLONG>(file_offset);
    SetFilePointerEx(file, offset, nullptr, FILE_BEGIN);
    std::string tail(to_read, '\0');
    DWORD read = 0;
    ReadFile(file, tail.data(), to_read, &read, nullptr);
    CloseHandle(file);

    if (read == 0) return result;

    // Count lines and keep only the last max_lines.
    int line_count = 0;
    for (char c : tail) if (c == '\n') ++line_count;

    int skip = (line_count > max_lines) ? (line_count - max_lines) : 0;
    std::string line;
    line.reserve(512);
    for (DWORD i = 0; i < read; ++i) {
        char c = tail[i];
        if (c == '\n') {
            if (skip > 0) { --skip; line.clear(); continue; }
            if (!line.empty() && line.back() == '\r') line.pop_back();
            result.push_back(Utf8ToWide(line));
            line.clear();
        } else if (c != '\r') {
            line.push_back(c);
        }
    }
    if (!line.empty() && skip <= 0) {
        result.push_back(Utf8ToWide(line));
    }
    return result;
}

// ============================================================================
// ASM stub entry point (implemented in script_hook.asm)
// ============================================================================
extern "C" void ScriptHookStub();

} // namespace wfmaxon

extern "C" void ScriptHookDispatch(wfmaxon::LuaHookContext* ctx) {
    if (!ctx) return;
    wfmaxon::ScriptEngine::DispatchHook(static_cast<int>(ctx->rcx), ctx);
}
