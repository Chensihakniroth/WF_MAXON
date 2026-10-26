// Minimal stubs for Lua/Pluto standard libraries whose full implementations
// are not yet vendored into WF_MAXON. Each returns an empty table so that
// luaL_requiref() and the Pluto startup boot sequence can complete without
// crashing. Replace with real implementations as they become available.
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

// Pluto's parser expects both luaopen_* AND Pluto::preloaded_* constants.
// The PLUTO_NEWLIB macro creates both, but needs a funcs_* array. Since we're
// stubbing, we provide an empty array + empty init for each.

static const luaL_Reg empty_funcs[] = {{nullptr, nullptr}};

// Stub that creates both luaopen_name and Pluto::preloaded_name.
#define STUB_LIB(name) \
    LUAMOD_API int luaopen_##name(lua_State* L) { lua_newtable(L); return 1; } \
    const Pluto::PreloadedLibrary Pluto::preloaded_##name { #name, empty_funcs, &luaopen_##name };

// Standard Lua libs that use LUAMOD_API (extern "C") only — no PreloadedLibrary.
extern "C" {
    LUAMOD_API int luaopen_package(lua_State* L) { lua_newtable(L); return 1; }
    LUAMOD_API int luaopen_coroutine(lua_State* L) { lua_newtable(L); return 1; }
    LUAMOD_API int luaopen_debug(lua_State* L) { lua_newtable(L); return 1; }
    LUAMOD_API int luaopen_utf8(lua_State* L) { lua_newtable(L); return 1; }
}

// Pluto extension libs — need both luaopen_* and Pluto::preloaded_*.
STUB_LIB(crypto)
STUB_LIB(json)
STUB_LIB(base32)
STUB_LIB(base64)
STUB_LIB(assert)
STUB_LIB(vector3)
STUB_LIB(url)
STUB_LIB(star)
STUB_LIB(cat)
STUB_LIB(http)
STUB_LIB(scheduler)
STUB_LIB(bigint)
STUB_LIB(xml)
STUB_LIB(regex)
STUB_LIB(ffi)
STUB_LIB(canvas)
STUB_LIB(buffer)
STUB_LIB(wasm)
STUB_LIB(socket)

#undef STUB_LIB
