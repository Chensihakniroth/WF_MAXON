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

#include <tlhelp32.h>

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
    luaL_error(L, "WF_MAXON hook instruction limit exceeded");
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
    // wf.find_pattern(module_base, pattern) -> address or nil
    // pattern: hex string like "48 89 5C 24 ?? 48 89 6C 24"
    //   '??' = wildcard byte, ' ' = separator
    auto* self = g_script_engine;
    if (!self || !lua_isinteger(L, 1) || !lua_isstring(L, 2)) {
        lua_pushnil(L);
        lua_pushliteral(L, "find_pattern: expected (integer module_base, string pattern)");
        return 2;
    }

    uintptr_t module_base = static_cast<uintptr_t>(lua_tointeger(L, 1));
    const char* pat_str = lua_tostring(L, 2);

    if (!module_base) {
        lua_pushnil(L);
        lua_pushliteral(L, "find_pattern: module_base is 0");
        return 2;
    }

    // Parse pattern into bytes + wildcard flags
    std::vector<uint8_t> pat_bytes;
    std::vector<bool> pat_wild;
    const char* p = pat_str;
    while (*p) {
        if (*p == ' ') { ++p; continue; }
        if (*p == '?' && p[1] == '?') {
            pat_bytes.push_back(0);
            pat_wild.push_back(true);
            p += 2;
            continue;
        }
        auto hex_val = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
            if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
            if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
            return 255;
        };
        uint8_t hi = hex_val(p[0]);
        uint8_t lo = hex_val(p[1]);
        if (hi == 255 || lo == 255) {
            lua_pushnil(L);
            lua_pushliteral(L, "find_pattern: invalid pattern hex");
            return 2;
        }
        pat_bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
        pat_wild.push_back(false);
        p += 2;
    }
    size_t plen = pat_bytes.size();
    if (plen == 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "find_pattern: empty pattern");
        return 2;
    }

    // Determine module size from PE headers
    uintptr_t module_size = 0;
    {
        // Read DOS header
        uint16_t dos_magic = 0;
        if (!self->GetEngine()->ReadBytes(module_base, &dos_magic, sizeof(dos_magic)) || dos_magic != 0x5A4D) {
            lua_pushnil(L);
            lua_pushliteral(L, "find_pattern: not a valid PE (no MZ)");
            return 2;
        }
        uint32_t e_lfanew = 0;
        if (!self->GetEngine()->ReadBytes(module_base + 0x3C, &e_lfanew, sizeof(e_lfanew)) || e_lfanew < 0x80) {
            lua_pushnil(L);
            lua_pushliteral(L, "find_pattern: invalid e_lfanew");
            return 2;
        }
        uint32_t pe_sig = 0;
        if (!self->GetEngine()->ReadBytes(module_base + e_lfanew, &pe_sig, sizeof(pe_sig)) || pe_sig != 0x00004550) {
            lua_pushnil(L);
            lua_pushliteral(L, "find_pattern: not a valid PE (no PE signature)");
            return 2;
        }
        // Optional header starts at e_lfanew + 24 (PE sig 4 + COFF 20)
        uintptr_t opt_hdr = module_base + e_lfanew + 24;
        uint16_t magic = 0;
        if (!self->GetEngine()->ReadBytes(opt_hdr, &magic, sizeof(magic))) {
            lua_pushnil(L);
            lua_pushliteral(L, "find_pattern: cannot read optional header magic");
            return 2;
        }
        if (magic == 0x20B) {  // PE32+
            // SizeOfImage is at offset 80 in PE32+ optional header
            if (!self->GetEngine()->ReadBytes(opt_hdr + 80, &module_size, sizeof(module_size))) {
                module_size = 0;
            }
        } else if (magic == 0x10B) {  // PE32
            uint32_t size32 = 0;
            // SizeOfImage is at offset 60 in PE32 optional header
            if (self->GetEngine()->ReadBytes(opt_hdr + 60, &size32, sizeof(size32))) {
                module_size = size32;
            }
        }
    }

    if (module_size == 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "find_pattern: could not determine module size");
        return 2;
    }

    // Scan in chunks
    constexpr size_t kChunk = 0x4000;  // 16KB chunks
    std::vector<uint8_t> buf(kChunk + plen);
    uintptr_t found = 0;

    for (uintptr_t off = 0; off + plen <= module_size && !found; off += kChunk) {
        size_t chunk_size = std::min<size_t>(kChunk, module_size - off);
        size_t read_size = chunk_size + plen;
        if (read_size > buf.size()) buf.resize(read_size);
        if (!self->GetEngine()->ReadBytes(module_base + off, buf.data(), read_size)) continue;

        for (size_t i = 0; i + plen <= chunk_size && !found; ++i) {
            bool match = true;
            for (size_t j = 0; j < plen; ++j) {
                if (!pat_wild[j] && buf[i + j] != pat_bytes[j]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                found = module_base + off + i;
            }
        }
    }

    if (found) {
        lua_pushinteger(L, static_cast<lua_Integer>(found));
        return 1;
    }
    lua_pushnil(L);
    lua_pushliteral(L, "pattern not found");
    return 2;
}

int wf_get_module(lua_State* L) {
    // wf.get_module(module_name) -> base_address or nil
    // e.g. wf.get_module("dxgi.dll")
    auto* self = g_script_engine;
    if (!self || !lua_isstring(L, 1)) {
        lua_pushnil(L);
        lua_pushliteral(L, "get_module: expected string module_name");
        return 2;
    }

    std::string name = lua_tostring(L, 1);

    HANDLE process = GetCurrentProcess();
    HMODULE modules[1024] = {};
    DWORD needed = 0;
    if (!EnumProcessModules(process, modules, sizeof(modules), &needed)) {
        lua_pushnil(L);
        lua_pushliteral(L, "get_module: EnumProcessModules failed");
        return 2;
    }
    DWORD count = std::min<DWORD>(needed / sizeof(HMODULE), std::size(modules));
    for (DWORD i = 0; i < count; ++i) {
        wchar_t mod_name[MAX_PATH] = {};
        if (GetModuleBaseNameW(process, modules[i], mod_name, std::size(mod_name)) == 0) continue;
        std::wstring wname(mod_name);
        // Compare case-insensitively
        std::wstring wtarget = Utf8ToWide(name);
        bool match = false;
        if (wname.size() >= wtarget.size()) {
            std::wstring suffix = wname.substr(wname.size() - wtarget.size());
            // case-insensitive compare
            bool eq = true;
            for (size_t k = 0; k < wtarget.size(); ++k) {
                wchar_t a = towlower(suffix[k]);
                wchar_t b = towlower(wtarget[k]);
                if (a != b) { eq = false; break; }
            }
            match = eq;
        }
        if (match) {
            lua_pushinteger(L, static_cast<lua_Integer>(reinterpret_cast<uintptr_t>(modules[i])));
            return 1;
        }
    }

    lua_pushnil(L);
    lua_pushliteral(L, "module not found");
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
        {"get_module",     wf_get_module},
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

// Thunk layout (24 bytes):
//   mov [rsp-8], rax       (save original game RAX)
//   mov rax, hook_id       (store hook ID in RAX temporarily)
//   xchg [rsp-8], rax      (swap: RAX restored to original game RAX! [rsp-8] has hook_id)
//   push hook_id / jump    
// Better & standard:
//   mov [rsp-18h], r10     (save scratch register r10)
//   mov r10, hook_id       (put hook_id in r10)
//   jmp dispatcher         (in dispatcher, save rcx, rdx, etc. and read hook_id from r10!)

void WriteThunk(uint8_t* buf, uintptr_t hook_id, uintptr_t dispatcher) {
    // 49 BA [hook_id:8]      mov r10, imm64
    buf[0] = 0x49;
    buf[1] = 0xBA;
    std::memcpy(buf + 2, &hook_id, sizeof(hook_id));
    // 48 B8 [dispatcher:8]   mov rax, imm64
    buf[10] = 0x48;
    buf[11] = 0xB8;
    std::memcpy(buf + 12, &dispatcher, sizeof(dispatcher));
    // FF E0                  jmp rax
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

    // Do NOT automatically run scripts on startup — let the user choose
    // which scripts to activate via the Script Manager UI checkboxes.
    status_ = L"Script engine ready";

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
    if (!custom_scripts_dir_.empty()) {
        return custom_scripts_dir_;
    }
    if (GetFileAttributesW(L"D:\\game\\warframe_menu\\WF_MAXON\\scripts") != INVALID_FILE_ATTRIBUTES) {
        return L"D:\\game\\warframe_menu\\WF_MAXON\\scripts";
    }
    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    wchar_t* last_slash = wcsrchr(exe_path, L'\\');
    if (last_slash) {
        *last_slash = L'\0';
        std::wstring cand1 = std::wstring(exe_path) + L"\\scripts";
        if (GetFileAttributesW(cand1.c_str()) != INVALID_FILE_ATTRIBUTES) return cand1;
        std::wstring cand2 = std::wstring(exe_path) + L"\\..\\scripts";
        if (GetFileAttributesW(cand2.c_str()) != INVALID_FILE_ATTRIBUTES) return cand2;
    }
    if (!game_dir_.empty()) {
        return game_dir_ + L"\\custom_hooks";
    }
    // Fallback: try to find Warframe directory.
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(static_cast<DWORD>(std::size(temp)), temp);
    return std::wstring(temp) + L"WF_MAXON_custom_hooks";
}

void ScriptEngine::SetCustomHooksDir(const std::wstring& dir) {
    custom_scripts_dir_ = dir;
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

    // Direct entry-point hook:
    // When hooking a function from Lua (e.g. wf.hook(target_addr, cb)),
    // we MUST hook at the requested entry address (offset 0), NOT at a random guessed offset!
    constexpr size_t kPatchLen = 14;

    uintptr_t hook_addr = address;

    // Allocate thunk (24 bytes) + trampoline.
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

    // Save the original kPatchLen bytes of the target.
    uint8_t original_bytes[kPatchLen]{};
    if (!engine_->ReadBytes(hook_addr, original_bytes, kPatchLen)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        VirtualFree(thunk, 0, MEM_RELEASE);
        return 0;
    }

    // Write thunk: mov r10, hook_id; jmp ScriptHookStub
    WriteThunk(thunk, static_cast<uintptr_t>(hook_id),
               reinterpret_cast<uintptr_t>(&ScriptHookStub));

    // Write trampoline: copy original bytes, then jump back to hook_addr + kPatchLen.
    std::memcpy(trampoline, original_bytes, kPatchLen);
    WriteAbsoluteJump(trampoline + kPatchLen, hook_addr + kPatchLen);
    FlushInstructionCache(GetCurrentProcess(), trampoline, kPatchLen + 12);

    // Patch the target: jump to thunk.
    unsigned char patch[kPatchLen]{};
    std::memset(patch, 0x90, kPatchLen);
    WriteAbsoluteJump(patch, reinterpret_cast<uintptr_t>(thunk));

    DWORD old_protect = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(hook_addr), kPatchLen,
                        PAGE_EXECUTE_READWRITE, &old_protect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        VirtualFree(thunk, 0, MEM_RELEASE);
        return 0;
    }

    __try {
        std::memcpy(reinterpret_cast<void*>(hook_addr), patch, sizeof(patch));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        VirtualProtect(reinterpret_cast<LPVOID>(hook_addr), kPatchLen, old_protect, &old_protect);
        VirtualFree(trampoline, 0, MEM_RELEASE);
        VirtualFree(thunk, 0, MEM_RELEASE);
        return 0;
    }
    FlushInstructionCache(GetCurrentProcess(),
                          reinterpret_cast<LPCVOID>(hook_addr), sizeof(patch));
    VirtualProtect(reinterpret_cast<LPVOID>(hook_addr), kPatchLen, old_protect, &old_protect);

    LuaHook hook{};
    hook.target = hook_addr;
    hook.trampoline = reinterpret_cast<uintptr_t>(trampoline);
    hook.thunk = reinterpret_cast<uintptr_t>(thunk);
    std::memcpy(hook.original_bytes, original_bytes, kPatchLen);
    hook.patch_len = kPatchLen;
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

        // Restore real original bytes, not NOPs!
        DWORD old_protect = 0;
        if (VirtualProtect(reinterpret_cast<LPVOID>(hook.target), hook.patch_len,
                           PAGE_EXECUTE_READWRITE, &old_protect)) {
            __try {
                std::memcpy(reinterpret_cast<void*>(hook.target), hook.original_bytes, hook.patch_len);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                // Fall through - page may not be writable
            }
            FlushInstructionCache(GetCurrentProcess(),
                                  reinterpret_cast<LPCVOID>(hook.target), hook.patch_len);
            VirtualProtect(reinterpret_cast<LPVOID>(hook.target), hook.patch_len, old_protect, &old_protect);
        }

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

    // Set the trampoline pointer into ctx so ScriptHookStub can jump through it!
    ctx->trampoline = hook->trampoline;

    InterlockedIncrement(&hook->fire_count);

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
    lua_pushinteger(L, static_cast<lua_Integer>(ctx->skip));
    lua_setfield(L, -2, "skip");

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

    // Read skip flag from Lua context (table at -2 before cleanup).
    int skip_read = 0;
    lua_getfield(L, -2, "skip");  // push skip value from ctx table
    if (lua_isnumber(L, -1)) skip_read = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);                 // pop skip value back
    ctx->skip = skip_read ? 1 : 0;

    hook->fire_count = InterlockedIncrement(&hook->fire_count);

    lua_pop(L, 2); // ctx table + function reference
}

static int SafeLuaPCall(lua_State* L) {
    __try {
        return lua_pcall(L, 0, LUA_MULTRET, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -999;
    }
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
        // High instruction limit (5,000,000) for user scripts so loops/scans don't trip
        lua_sethook(L, HookInstructionCountHook, LUA_MASKCOUNT, 5000000);
        rc = SafeLuaPCall(L);
        lua_sethook(L, nullptr, 0, 0);
    }

    if (rc == -999) {
        std::string err_str = "Native hardware exception caught during execution";
        if (error_out) *error_out = err_str;
        AppendScriptLog("[script] ERROR: Native hardware exception\n");
        lua_settop(L, top);
        return false;
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

bool ScriptEngine::ExecuteScriptFile(const std::wstring& path, std::string* error_out) {
    std::lock_guard<std::mutex> lock(lua_mutex_);
    if (!lua_state_) {
        if (error_out) *error_out = "Lua state not initialized";
        return false;
    }
    lua_State* L = reinterpret_cast<lua_State*>(lua_state_);
    int top = lua_gettop(L);

    std::string utf8_path = WideToUtf8(path);
    int rc = luaL_loadfile(L, utf8_path.c_str());
    if (rc == LUA_OK) {
        lua_sethook(L, HookInstructionCountHook, LUA_MASKCOUNT, 5000000);
        rc = SafeLuaPCall(L);
        lua_sethook(L, nullptr, 0, 0);
    }

    if (rc == -999) {
        std::string err_str = "Native hardware exception caught during execution";
        if (error_out) *error_out = err_str;
        char log[512]{};
        sprintf_s(log, "[script] ERROR in %s: %s\n", utf8_path.c_str(), err_str.c_str());
        AppendScriptLog(log);
        lua_settop(L, top);
        return false;
    }

    if (rc == LUA_OK) {
        char log[512]{};
        sprintf_s(log, "[script] Loaded %s successfully\n", utf8_path.c_str());
        AppendScriptLog(log);
        lua_settop(L, top);
        return true;
    } else {
        const char* err = lua_tostring(L, -1);
        std::string err_str = err ? err : "Unknown error";
        if (error_out) *error_out = err_str;
        char log[512]{};
        sprintf_s(log, "[script] ERROR in %s: %s\n", utf8_path.c_str(), err_str.c_str());
        AppendScriptLog(log);
        lua_settop(L, top);
        return false;
    }
}

void ScriptEngine::ResetEnvironment() {
    std::lock_guard<std::mutex> lock(lua_mutex_);
    if (!lua_state_) return;
    lua_State* L = reinterpret_cast<lua_State*>(lua_state_);

    // 1. Call on_unload callbacks.
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
    lua_pop(L, 1);

    // Clear on_unload table
    lua_pushnil(L);
    lua_setglobal(L, "_wf_on_unload_list");

    // 2. Uninstall all memory hooks.
    UninstallAllHooks();

    // 3. Re-register wf module.
    RegisterWfModule(this, L);

    AppendScriptLog("[script] Environment reset: hooks cleared and on_unload completed\n");
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
    wfmaxon::ScriptEngine::DispatchHook(static_cast<int>(ctx->r10), ctx);
}
