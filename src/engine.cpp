// ============================================================================
// WF_MAXON — live engine implementation
// ============================================================================
//
// Opens Warframe.x64.exe by name, resolves the player base, then writes each
// toggled field directly with ReadProcessMemory / WriteProcessMemory. The
// toggles mirror the proven OwHelper WarframeHelper.ini schema.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <process.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cstring>

#pragma comment(lib, "psapi.lib")

#include "engine.h"
#include "damage_policy.h"
#include "generated/warframe_offsets.h"

namespace wfmaxon {
namespace {
// Build identity and code anchors come from the exact executable scanned by
// WF_MAXON/tools/refresh_offsets.py. The generated manifest is fail-closed:
// an ambiguous or dead signature gets RVA 0 and its hook refuses to install.
constexpr uintptr_t kExpectedImageSize = wfmaxon_offsets::kExpectedImageSize;
constexpr uint32_t kExpectedPeTimestamp = wfmaxon_offsets::kExpectedPeTimestamp;

// Current-build read-only anchor chain. These identify the object path, not
// writable feature fields; feature_offsets_verified_ remains false until the
// individual feature semantics are validated.
constexpr uintptr_t kRegistryRva = 0x270EF90;
constexpr uint32_t kGameRulesKey = 0xB364FD20;
constexpr uintptr_t kGameRulesVtableRva = 0x21A0A28;
constexpr uintptr_t kHumanPlayersDataOffset = 0x230;
constexpr uintptr_t kHumanPlayersBytesOffset = 0x238;
constexpr uintptr_t kHumanPlayerVtableRva = 0x232E190;
constexpr uintptr_t kHumanPlayerAvatarHolderOffset = 0x148;
constexpr uintptr_t kAvatarFactionOffset = 0x688;
constexpr uintptr_t kAvatarSubobjectOffset = 0x588;
constexpr uintptr_t kAvatarFriendlyVtableSlot = 0x128;
constexpr uintptr_t kAvatarPositionOffset = 0x70;
// The Orbiter uses a LotusMainMenuAvatar that does not expose the gameplay
// friendly-subobject fingerprint, but its current vtable and position field
// are behavior-validated by the read-only movement probe.
constexpr uintptr_t kOrbiterAvatarVtableRva = 0x22F5230;

// Current-build hook anchors. These are generated from unique signatures; the
// payload still performs a byte-for-byte prologue check before any patch.
constexpr uintptr_t kFireWeaponRmiRva = wfmaxon_offsets::kFireWeaponRmiRva;
constexpr bool kFireWeaponRmiResolved = wfmaxon_offsets::kFireWeaponRmiResolved;
constexpr uintptr_t kWeaponExAmmoClipOffset = 0x630;
constexpr size_t kFirePatchLength = wfmaxon_offsets::kFireWeaponRmiPatchLength;
constexpr auto kFirePrologue = wfmaxon_offsets::kFireWeaponRmiPrologue;

constexpr uintptr_t kWeaponAmountRva = wfmaxon_offsets::kWeaponAmountRva;
constexpr bool kWeaponAmountResolved = wfmaxon_offsets::kWeaponAmountResolved;
constexpr size_t kWeaponAmountPatchLength = wfmaxon_offsets::kWeaponAmountPatchLength;
constexpr auto kWeaponAmountPrologue = wfmaxon_offsets::kWeaponAmountPrologue;

// The current-build semantic anchor resolves the final float damage value
// immediately before cvttss2si.  This is the point where the OwHelper-style
// context callback can substitute damage without touching health fields.
constexpr uintptr_t kDamageResultRva = wfmaxon_offsets::kDamageResultRva;
constexpr uintptr_t kDamageResultBranchTargetRva = wfmaxon_offsets::kDamageResultBranchTargetRva;
constexpr bool kDamageResultResolved = wfmaxon_offsets::kDamageResultResolved;
constexpr size_t kDamageResultPatchLength = wfmaxon_offsets::kDamageResultPatchLength;
constexpr auto kDamageResultPrologue = wfmaxon_offsets::kDamageResultPrologue;

constexpr uintptr_t kDamageAccumulateRva = wfmaxon_offsets::kDamageAccumulateRva;
constexpr bool kDamageAccumulateResolved = wfmaxon_offsets::kDamageAccumulateResolved;
constexpr size_t kDamageAccumulatePatchLength = wfmaxon_offsets::kDamageAccumulatePatchLength;
constexpr auto kDamageAccumulatePrologue = wfmaxon_offsets::kDamageAccumulatePrologue;

constexpr uintptr_t kFovSetterRva = wfmaxon_offsets::kFovSetterRva;
constexpr bool kFovSetterResolved = wfmaxon_offsets::kFovSetterResolved;
constexpr size_t kFovPatchLength = wfmaxon_offsets::kFovSetterPatchLength;
constexpr auto kFovSetterPrologue = wfmaxon_offsets::kFovSetterPrologue;

constexpr uintptr_t kFovGetterRva = wfmaxon_offsets::kFovGetterRva;
constexpr bool kFovGetterResolved = wfmaxon_offsets::kFovGetterResolved;
constexpr size_t kFovGetterPatchLength = wfmaxon_offsets::kFovGetterPatchLength;
constexpr auto kFovGetterPrologue = wfmaxon_offsets::kFovGetterPrologue;

constexpr uintptr_t kFovViewGetterRva = wfmaxon_offsets::kFovViewGetterRva;
constexpr bool kFovViewGetterResolved = wfmaxon_offsets::kFovViewGetterResolved;
constexpr size_t kFovViewGetterPatchLength = wfmaxon_offsets::kFovViewGetterPatchLength;
constexpr auto kFovViewGetterPrologue = wfmaxon_offsets::kFovViewGetterPrologue;

bool OffsetResolved(bool signature, uintptr_t rva, uintptr_t secondary = 1) noexcept {
    return signature && rva != 0 && secondary != 0;
}

typedef void (__fastcall *SetBaseFovFn)(uintptr_t, float);
typedef float (__fastcall *GetBaseFovFn)(uintptr_t);
typedef float (__fastcall *GetViewFovFn)(uintptr_t);
SetBaseFovFn g_fov_original = nullptr;
GetBaseFovFn g_fov_get_original = nullptr;
GetViewFovFn g_fov_view_get_original = nullptr;
uintptr_t g_fov_trampoline = 0;
uintptr_t g_fov_get_trampoline = 0;
uintptr_t g_fov_view_get_trampoline = 0;
std::atomic<bool> g_fov_enabled{false};
std::atomic<float> g_fov_degrees{90.0f};
std::atomic<ULONG64> g_fov_call_count{0};
std::atomic<ULONG64> g_fov_get_call_count{0};
std::atomic<ULONG64> g_fov_view_get_call_count{0};
std::atomic<uintptr_t> g_fov_last_camera{0};
std::atomic<bool> g_one_hit_kill_enabled{false};
std::atomic<bool> g_one_hit_kill_alt_only{false};
volatile LONG64 g_one_hit_kill_override_count = 0;
typedef __int64 (__fastcall *FireWeaponRmiFn)(uintptr_t, uintptr_t);
FireWeaponRmiFn g_fire_original = nullptr;
uintptr_t g_fire_trampoline = 0;

typedef float (__fastcall *WeaponAmountFn)(uintptr_t, uint32_t);
WeaponAmountFn g_weapon_amount_original = nullptr;
uintptr_t g_weapon_amount_trampoline = 0;

typedef void (__fastcall *DamageAccumulateFn)(uintptr_t, uintptr_t, uint32_t);
DamageAccumulateFn g_damage_accumulate_original = nullptr;
uintptr_t g_damage_accumulate_trampoline = 0;

volatile LONG64 g_fire_last_log_ms = 0;
volatile LONG64 g_fire_call_count = 0;
volatile LONG64 g_damage_call_count = 0;

struct FireSnapshot {
    uintptr_t self = 0;
    uintptr_t event = 0;
    uintptr_t vtable = 0;
    unsigned char event_type = 0;
    uint32_t amount = 0;
    uint16_t clip = 0;
};

bool CaptureFireSnapshot(uintptr_t self, uintptr_t event, FireSnapshot& out) {
    __try {
        if (!self || !event) return false;
        out.self = self;
        out.event = event;
        out.vtable = *reinterpret_cast<uintptr_t*>(self);
        out.event_type = *reinterpret_cast<unsigned char*>(event + 1);
        out.amount = *reinterpret_cast<uint32_t*>(event + 4);
        out.clip = *reinterpret_cast<uint16_t*>(self + kWeaponExAmmoClipOffset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void AppendFireTelemetry(const FireSnapshot& before, const FireSnapshot& after,
                         __int64 result) {
    wchar_t path[MAX_PATH]{};
    DWORD n = GetTempPathW(static_cast<DWORD>(std::size(path)), path);
    if (!n || n >= std::size(path)) return;
    if (wcscat_s(path, L"WF_MAXON_fire_telemetry.log") != 0) return;

    char line[512]{};
    sprintf_s(line,
        "event=8 self=0x%llX event_ptr=0x%llX vtable=0x%llX amount=%u "
        "clip_before=%u clip_after=%u result=0x%llX\\r\\n",
        static_cast<unsigned long long>(before.self),
        static_cast<unsigned long long>(before.event),
        static_cast<unsigned long long>(before.vtable),
        before.amount,
        static_cast<unsigned>(before.clip),
        static_cast<unsigned>(after.clip),
        static_cast<unsigned long long>(result));

    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
    CloseHandle(file);
}

void AppendFireCallDiagnostic(bool before_ok, const FireSnapshot& before,
                              bool after_ok, const FireSnapshot& after,
                              __int64 result) {
    wchar_t path[MAX_PATH]{};
    DWORD n = GetTempPathW(static_cast<DWORD>(std::size(path)), path);
    if (!n || n >= std::size(path)) return;
    if (wcscat_s(path, L"WF_MAXON_fire_telemetry.log") != 0) return;

    char line[640]{};
    sprintf_s(line,
        "call before_ok=%d after_ok=%d self=0x%llX event_ptr=0x%llX "
        "type=%u amount=%u clip_before=%u clip_after=%u result=0x%llX\\r\\n",
        before_ok ? 1 : 0,
        after_ok ? 1 : 0,
        static_cast<unsigned long long>(before.self),
        static_cast<unsigned long long>(before.event),
        static_cast<unsigned>(before.event_type),
        static_cast<unsigned>(before.amount),
        static_cast<unsigned>(before.clip),
        static_cast<unsigned>(after.clip),
        static_cast<unsigned long long>(result));

    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
    CloseHandle(file);
}

void AppendWeaponAmountDiagnostic(uintptr_t self, uint32_t amount, float result) {
    wchar_t path[MAX_PATH]{};
    DWORD n = GetTempPathW(static_cast<DWORD>(std::size(path)), path);
    if (!n || n >= std::size(path)) return;
    if (wcscat_s(path, L"WF_MAXON_fire_telemetry.log") != 0) return;

    char line[256]{};
    sprintf_s(line, "amount self=0x%llX value=%u result=%.3f\\r\\n",
              static_cast<unsigned long long>(self),
              static_cast<unsigned>(amount),
              static_cast<double>(result));
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
    CloseHandle(file);
}

struct DamageSnapshot {
    uintptr_t accumulator = 0;
    uintptr_t damage = 0;
    uint32_t flags = 0;
    uint8_t type = 0;
    int32_t accumulator_value = 0;
    float d70 = 0.0f;
    float d74 = 0.0f;
    float d78 = 0.0f;
    float d7c = 0.0f;
    float d80 = 0.0f;
    float d84 = 0.0f;
    float d88 = 0.0f;
    float d8c = 0.0f;
};

volatile LONG g_damage_flush_running = 0;
volatile LONG64 g_damage_snapshot_sequence = 0;
volatile LONG64 g_damage_snapshot_call = 0;
DamageSnapshot g_damage_snapshot_before{};
DamageSnapshot g_damage_snapshot_after{};

bool CaptureDamageSnapshot(uintptr_t accumulator, uintptr_t damage,
                           uint32_t flags, DamageSnapshot& out) {
    __try {
        if (!accumulator || !damage) return false;
        out.accumulator = accumulator;
        out.damage = damage;
        out.flags = flags;
        out.type = *reinterpret_cast<unsigned char*>(damage + 0x1BF);
        out.accumulator_value = *reinterpret_cast<int32_t*>(accumulator + 0x5A0);
        out.d70 = *reinterpret_cast<float*>(damage + 0x70);
        out.d74 = *reinterpret_cast<float*>(damage + 0x74);
        out.d78 = *reinterpret_cast<float*>(damage + 0x78);
        out.d7c = *reinterpret_cast<float*>(damage + 0x7C);
        out.d80 = *reinterpret_cast<float*>(damage + 0x80);
        out.d84 = *reinterpret_cast<float*>(damage + 0x84);
        out.d88 = *reinterpret_cast<float*>(damage + 0x88);
        out.d8c = *reinterpret_cast<float*>(damage + 0x8C);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void AppendDamageTelemetry(const DamageSnapshot& before,
                           const DamageSnapshot& after,
                           LONG64 call_count) {
    wchar_t path[MAX_PATH]{};
    DWORD n = GetTempPathW(static_cast<DWORD>(std::size(path)), path);
    if (!n || n >= std::size(path)) return;
    if (wcscat_s(path, L"WF_MAXON_damage_telemetry.log") != 0) return;

    char line[768]{};
    sprintf_s(line,
        "call=%lld accumulator=0x%llX damage=0x%llX flags=0x%08X "
        "type=%u before=%d after=%d d70=%.3f d74=%.3f d78=%.3f "
        "d7c=%.3f d80=%.3f d84=%.3f d88=%.3f d8c=%.3f\\r\\n",
        static_cast<long long>(call_count),
        static_cast<unsigned long long>(before.accumulator),
        static_cast<unsigned long long>(before.damage),
        static_cast<unsigned>(before.flags),
        static_cast<unsigned>(before.type),
        before.accumulator_value,
        after.accumulator_value,
        static_cast<double>(before.d70),
        static_cast<double>(before.d74),
        static_cast<double>(before.d78),
        static_cast<double>(before.d7c),
        static_cast<double>(before.d80),
        static_cast<double>(before.d84),
        static_cast<double>(before.d88),
        static_cast<double>(before.d8c));

    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
    CloseHandle(file);
}

unsigned __stdcall DamageTelemetryFlushThread(void*) {
    LONG64 last_sequence = 0;
    for (;;) {
        Sleep(250);
        if (InterlockedCompareExchange(&g_damage_flush_running, 0, 0) == 0) {
            break;
        }

        const LONG64 begin = InterlockedCompareExchange64(
            &g_damage_snapshot_sequence, 0, 0);
        if (begin == 0 || (begin & 1) != 0 || begin == last_sequence) {
            continue;
        }

        DamageSnapshot before{};
        DamageSnapshot after{};
        MemoryBarrier();
        std::memcpy(&before, &g_damage_snapshot_before, sizeof(before));
        std::memcpy(&after, &g_damage_snapshot_after, sizeof(after));
        const LONG64 call_count = InterlockedCompareExchange64(
            &g_damage_snapshot_call, 0, 0);
        MemoryBarrier();

        const LONG64 end = InterlockedCompareExchange64(
            &g_damage_snapshot_sequence, 0, 0);
        if (begin != end || (end & 1) != 0) {
            continue;
        }

        AppendDamageTelemetry(before, after, call_count);
        last_sequence = end;
    }
    return 0;
}

void AppendFovDiagnostic(const char* message) {
    wchar_t path[MAX_PATH]{};
    DWORD n = GetTempPathW(static_cast<DWORD>(std::size(path)), path);
    if (!n || n >= std::size(path) || wcscat_s(path, L"WF_MAXON_fov.log") != 0) return;
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    char line[512]{};
    sprintf_s(line, "%llu %s\\r\\n",
              static_cast<unsigned long long>(GetTickCount64()), message);
    DWORD written = 0;
    WriteFile(file, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
    CloseHandle(file);
}

HMODULE FindCurrentBuildModule(uintptr_t preferred_base) {
    auto matches_identity = [](HMODULE module) {
        if (!module) return false;
        auto* base = reinterpret_cast<unsigned char*>(module);
        auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS64>(base + dos->e_lfanew);
        return nt->Signature == IMAGE_NT_SIGNATURE &&
               nt->OptionalHeader.SizeOfImage == kExpectedImageSize &&
               nt->FileHeader.TimeDateStamp == kExpectedPeTimestamp;
    };

    if (preferred_base) {
        char message[128]{};
        sprintf_s(message, "preferred_base=0x%llX",
                  static_cast<unsigned long long>(preferred_base));
        AppendFovDiagnostic(message);
        AppendFovDiagnostic("preferred_base_trusted");
        return reinterpret_cast<HMODULE>(preferred_base);
    }

    HMODULE modules[512]{};
    DWORD needed = 0;
    HANDLE process = GetCurrentProcess();
    if (!EnumProcessModules(process, modules, sizeof(modules), &needed)) return nullptr;
    const DWORD count = std::min<DWORD>(needed / sizeof(HMODULE), std::size(modules));
    for (DWORD i = 0; i < count; ++i) {
        MODULEINFO info{};
        if (!GetModuleInformation(process, modules[i], &info, sizeof(info)) ||
            info.SizeOfImage != kExpectedImageSize) {
            continue;
        }
        if (matches_identity(modules[i])) return modules[i];
    }
    return nullptr;
}

__int64 __fastcall FireWeaponRmiTelemetry(uintptr_t self, uintptr_t event) {
    FireSnapshot before{};
    FireSnapshot after{};
    const bool before_ok = CaptureFireSnapshot(self, event, before);
    __int64 result = g_fire_original ? g_fire_original(self, event) : 0;
    const bool after_ok = CaptureFireSnapshot(self, event, after);

    const LONG64 call_count = InterlockedIncrement64(&g_fire_call_count);
    if (call_count <= 20) {
        AppendFireCallDiagnostic(before_ok, before, after_ok, after, result);
    }

    if (before_ok && after_ok && before.event_type == 8 && before.amount != 0) {
        const LONG64 now = static_cast<LONG64>(GetTickCount64());
        const LONG64 previous = g_fire_last_log_ms;
        if (now - previous >= 250 &&
            InterlockedCompareExchange64(&g_fire_last_log_ms, now, previous) == previous) {
            AppendFireTelemetry(before, after, result);
        }
    }
    return result;
}

} // namespace

void WriteAbsoluteJump(unsigned char* destination, uintptr_t target) {
    destination[0] = 0x48;
    destination[1] = 0xB8;
    std::memcpy(destination + 2, &target, sizeof(target));
    destination[10] = 0xFF;
    destination[11] = 0xE0;
}

struct DamageContextHook {
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
static_assert(offsetof(DamageContextHook, rsp) == 0x78);
static_assert(offsetof(DamageContextHook, xmm) == 0x80);
static_assert(offsetof(DamageContextHook, skip) == 0x180);
static_assert(offsetof(DamageContextHook, trampoline) == 0x188);

struct DamageContextSnapshot {
    uintptr_t rcx = 0;
    uintptr_t rdx = 0;
    uintptr_t r8 = 0;
    uintptr_t r9 = 0;
    uintptr_t rsp = 0;
    float computed_damage = 0.0f;
    float applied_damage = 0.0f;
    bool overridden = false;
    LONG64 call_count = 0;
};

extern "C" void DamageContextStub();
extern "C" void DamageContextCallback(DamageContextHook* context);

volatile LONG g_damage_context_flush_running = 0;
volatile LONG64 g_damage_context_snapshot_sequence = 0;
volatile LONG64 g_damage_context_call_count = 0;
DamageContextSnapshot g_damage_context_snapshot{};
uintptr_t g_damage_context_trampoline = 0;

bool CaptureDamageContextSnapshot(const DamageContextHook& context,
                                  LONG64 call_count,
                                  float original_damage,
                                  bool overridden,
                                  DamageContextSnapshot& out) {
    out.rcx = context.rcx;
    out.rdx = context.rdx;
    out.r8 = context.r8;
    out.r9 = context.r9;
    out.rsp = context.rsp;
    out.computed_damage = original_damage;
    out.applied_damage = context.xmm[1][0];
    out.overridden = overridden;
    out.call_count = call_count;
    return true;
}

void AppendDamageContextTelemetry(const DamageContextSnapshot& snapshot) {
    wchar_t path[MAX_PATH]{};
    DWORD n = GetTempPathW(static_cast<DWORD>(std::size(path)), path);
    if (!n || n >= std::size(path)) return;
    if (wcscat_s(path, L"WF_MAXON_damage_context_telemetry.log") != 0) return;

    char line[512]{};
    sprintf_s(line,
        "call=%lld rcx=0x%llX rdx=0x%llX r8=0x%llX r9=0x%llX "
        "rsp=0x%llX xmm1_before=%.3f xmm1_after=%.3f ohk=%d\\r\\n",
        static_cast<long long>(snapshot.call_count),
        static_cast<unsigned long long>(snapshot.rcx),
        static_cast<unsigned long long>(snapshot.rdx),
        static_cast<unsigned long long>(snapshot.r8),
        static_cast<unsigned long long>(snapshot.r9),
        static_cast<unsigned long long>(snapshot.rsp),
        static_cast<double>(snapshot.computed_damage),
        static_cast<double>(snapshot.applied_damage),
        snapshot.overridden ? 1 : 0);

    HANDLE file = CreateFileW(path, FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
    CloseHandle(file);
}

unsigned __stdcall DamageContextFlushThread(void*) {
    LONG64 last_sequence = 0;
    for (;;) {
        Sleep(250);
        if (InterlockedCompareExchange(&g_damage_context_flush_running, 0, 0) == 0) {
            break;
        }

        const LONG64 begin = InterlockedCompareExchange64(
            &g_damage_context_snapshot_sequence, 0, 0);
        if (begin == 0 || (begin & 1) != 0 || begin == last_sequence) {
            continue;
        }

        DamageContextSnapshot snapshot{};
        MemoryBarrier();
        std::memcpy(&snapshot, &g_damage_context_snapshot, sizeof(snapshot));
        MemoryBarrier();
        const LONG64 end = InterlockedCompareExchange64(
            &g_damage_context_snapshot_sequence, 0, 0);
        if (begin != end || (end & 1) != 0) {
            continue;
        }

        AppendDamageContextTelemetry(snapshot);
        last_sequence = end;
    }
    return 0;
}

extern "C" void DamageContextCallback(DamageContextHook* context) {
    if (!context) return;
    context->trampoline = g_damage_context_trampoline;

    const LONG64 call_count = InterlockedIncrement64(&g_damage_context_call_count);
    const float original_damage = context->xmm[1][0];
    bool overridden = false;
    const bool enabled = g_one_hit_kill_enabled.load(std::memory_order_acquire);
    const bool alt_only = g_one_hit_kill_alt_only.load(std::memory_order_relaxed);
    const bool right_alt_held = alt_only &&
        ((GetAsyncKeyState(VK_RMENU) & static_cast<SHORT>(0x8000)) != 0);
    const DamageOverrideDecision decision = EvaluateDamageOverride(
        enabled, alt_only, original_damage, right_alt_held);
    if (decision.apply) {
        context->xmm[1][0] = decision.value;
        overridden = true;
        InterlockedIncrement64(&g_one_hit_kill_override_count);
    }

    if (call_count > 128) return;

    DamageContextSnapshot snapshot{};
    if (!CaptureDamageContextSnapshot(*context, call_count, original_damage,
                                      overridden, snapshot)) return;
    InterlockedIncrement64(&g_damage_context_snapshot_sequence); // writer/odd
    std::memcpy(&g_damage_context_snapshot, &snapshot, sizeof(snapshot));
    MemoryBarrier();
    InterlockedIncrement64(&g_damage_context_snapshot_sequence); // published/even
}

bool InstallFireTelemetryHook() {
    if (!OffsetResolved(kFireWeaponRmiResolved, kFireWeaponRmiRva)) {
        AppendFovDiagnostic("fire_offset_unresolved");
        return false;
    }
    if (g_fire_original) {
        AppendFovDiagnostic("fire_already_installed");
        return true;
    }

    HMODULE module = FindCurrentBuildModule(0);
    if (!module) {
        AppendFovDiagnostic("fire_no_current_build_module");
        return false;
    }
    auto* base = reinterpret_cast<unsigned char*>(module);
    auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS64>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.SizeOfImage != kExpectedImageSize ||
        nt->FileHeader.TimeDateStamp != kExpectedPeTimestamp) {
        return false;
    }

    auto* target = base + kFireWeaponRmiRva;
    if (std::memcmp(target, kFirePrologue.data(), kFirePatchLength) != 0) {
        char message[192]{};
        sprintf_s(message, "fire_prologue_mismatch addr=0x%llX bytes=%02X%02X%02X%02X%02X%02X%02X%02X",
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(target)),
                  target[0], target[1], target[2], target[3], target[4], target[5], target[6], target[7]);
        AppendFovDiagnostic(message);
        return false;
    }

    constexpr size_t trampoline_size = kFirePatchLength + 12;
    auto* trampoline = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, trampoline_size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        AppendFovDiagnostic("fire_trampoline_alloc_failed");
        return false;
    }
    std::memcpy(trampoline, target, kFirePatchLength);
    WriteAbsoluteJump(trampoline + kFirePatchLength,
                      reinterpret_cast<uintptr_t>(target + kFirePatchLength));

    DWORD old_protect = 0;
    if (!VirtualProtect(target, kFirePatchLength, PAGE_EXECUTE_READWRITE, &old_protect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        AppendFovDiagnostic("fire_virtualprotect_failed");
        return false;
    }

    unsigned char patch[kFirePatchLength]{};
    std::memset(patch, 0x90, sizeof(patch));
    WriteAbsoluteJump(patch, reinterpret_cast<uintptr_t>(&FireWeaponRmiTelemetry));
    std::memcpy(target, patch, sizeof(patch));
    FlushInstructionCache(GetCurrentProcess(), target, kFirePatchLength);
    DWORD ignored = 0;
    VirtualProtect(target, kFirePatchLength, old_protect, &ignored);

    g_fire_trampoline = reinterpret_cast<uintptr_t>(trampoline);
    g_fire_original = reinterpret_cast<FireWeaponRmiFn>(trampoline);
    AppendFovDiagnostic("fire_installed");
    return true;
}

void __fastcall FovOverrideThunk(uintptr_t self, float requested_degrees) {
    SetBaseFovFn original = g_fov_original;
    if (!original) return;
    g_fov_last_camera.store(self, std::memory_order_release);
    const float degrees = g_fov_enabled.load(std::memory_order_relaxed)
        ? g_fov_degrees.load(std::memory_order_relaxed)
        : requested_degrees;
    const ULONG64 call_count = g_fov_call_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (call_count <= 5) {
        char message[160]{};
        sprintf_s(message, "setter_call=%llu self=0x%llX requested=%.2f applied=%.2f",
                  static_cast<unsigned long long>(call_count),
                  static_cast<unsigned long long>(self),
                  static_cast<double>(requested_degrees),
                  static_cast<double>(degrees));
        AppendFovDiagnostic(message);
    }
    original(self, degrees);
}

float __fastcall FovGetterThunk(uintptr_t self) {
    GetBaseFovFn original = g_fov_get_original;
    if (!original) return 0.0f;
    g_fov_last_camera.store(self, std::memory_order_release);
    const float current = original(self);
    const bool enabled = g_fov_enabled.load(std::memory_order_relaxed);
    const float applied = enabled ? g_fov_degrees.load(std::memory_order_relaxed) : current;
    const ULONG64 call_count = g_fov_get_call_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (call_count <= 5) {
        char message[192]{};
        sprintf_s(message, "getter_call=%llu self=0x%llX current=%.2f applied=%.2f",
                  static_cast<unsigned long long>(call_count),
                  static_cast<unsigned long long>(self),
                  static_cast<double>(current),
                  static_cast<double>(applied));
        AppendFovDiagnostic(message);
    }
    return applied;
}

float __fastcall WeaponAmountTelemetry(uintptr_t self, uint32_t amount) {
    WeaponAmountFn original = g_weapon_amount_original;
    const float result = original ? original(self, amount) : 0.0f;
    const LONG64 call_count = InterlockedIncrement64(&g_fire_call_count);
    if (call_count <= 20) {
        AppendWeaponAmountDiagnostic(self, amount, result);
    }
    return result;
}

void __fastcall DamageAccumulateTelemetry(uintptr_t accumulator,
                                          uintptr_t damage,
                                          uint32_t flags) {
    DamageSnapshot before{};
    DamageSnapshot after{};
    const bool before_ok = CaptureDamageSnapshot(accumulator, damage, flags, before);
    DamageAccumulateFn original = g_damage_accumulate_original;
    if (original) {
        original(accumulator, damage, flags);
    }
    const bool after_ok = CaptureDamageSnapshot(accumulator, damage, flags, after);
    const LONG64 call_count = InterlockedIncrement64(&g_damage_call_count);

    if (before_ok && after_ok && call_count <= 64) {
        InterlockedIncrement64(&g_damage_snapshot_sequence); // writer/odd
        std::memcpy(&g_damage_snapshot_before, &before, sizeof(before));
        std::memcpy(&g_damage_snapshot_after, &after, sizeof(after));
        InterlockedExchange64(&g_damage_snapshot_call, call_count);
        MemoryBarrier();
        InterlockedIncrement64(&g_damage_snapshot_sequence); // published/even
    }
}

bool InstallDamageContextTelemetryHook() {
    if (!OffsetResolved(kDamageResultResolved, kDamageResultRva,
                        kDamageResultBranchTargetRva)) {
        AppendFovDiagnostic("damage_context_offset_unresolved");
        return false;
    }
    if (g_damage_context_trampoline) {
        AppendFovDiagnostic("damage_context_already_installed");
        return true;
    }

    HMODULE module = FindCurrentBuildModule(0);
    if (!module) {
        AppendFovDiagnostic("damage_context_no_current_build_module");
        return false;
    }

    auto* base = reinterpret_cast<unsigned char*>(module);
    auto* target = base + kDamageResultRva;
    if (std::memcmp(target, kDamageResultPrologue.data(),
                    kDamageResultPatchLength) != 0) {
        char message[192]{};
        sprintf_s(message,
                  "damage_context_prologue_mismatch addr=0x%llX",
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(target)));
        AppendFovDiagnostic(message);
        return false;
    }

    constexpr size_t trampoline_size = 10 + 14 + 12;
    auto* trampoline = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, trampoline_size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        AppendFovDiagnostic("damage_context_trampoline_alloc_failed");
        return false;
    }

    // The 12-byte detour overwrites cvttss2si (4), cmp (6), and a short
    // conditional branch (2). Rebuild the condition with an inverted short
    // branch plus an absolute jump so the trampoline does not depend on being
    // within +/-2GB of the game's code image.
    constexpr size_t copied_prefix = 10;
    constexpr size_t conditional_jump_size = 14;
    constexpr uintptr_t branch_target_rva = kDamageResultBranchTargetRva;
    std::memcpy(trampoline, target, copied_prefix);
    trampoline[copied_prefix + 0] = 0x75; // jne: skip absolute branch
    trampoline[copied_prefix + 1] = 0x0C; // 12-byte mov/jmp sequence
    WriteAbsoluteJump(trampoline + copied_prefix + 2,
                      reinterpret_cast<uintptr_t>(base + branch_target_rva));
    WriteAbsoluteJump(trampoline + copied_prefix + conditional_jump_size,
                      reinterpret_cast<uintptr_t>(target + kDamageResultPatchLength));

    DWORD old_protect = 0;
    if (!VirtualProtect(target, kDamageResultPatchLength,
                        PAGE_EXECUTE_READWRITE, &old_protect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        AppendFovDiagnostic("damage_context_virtualprotect_failed");
        return false;
    }

    g_damage_context_trampoline = reinterpret_cast<uintptr_t>(trampoline);
    unsigned char patch[kDamageResultPatchLength]{};
    std::memset(patch, 0x90, sizeof(patch));
    WriteAbsoluteJump(patch, reinterpret_cast<uintptr_t>(&DamageContextStub));
    std::memcpy(target, patch, sizeof(patch));
    FlushInstructionCache(GetCurrentProcess(), target, kDamageResultPatchLength);
    DWORD ignored = 0;
    VirtualProtect(target, kDamageResultPatchLength, old_protect, &ignored);

    if (InterlockedCompareExchange(&g_damage_context_flush_running, 1, 0) == 0) {
        uintptr_t logger = _beginthreadex(nullptr, 0, DamageContextFlushThread,
                                           nullptr, 0, nullptr);
        if (logger) {
            CloseHandle(reinterpret_cast<HANDLE>(logger));
        } else {
            InterlockedExchange(&g_damage_context_flush_running, 0);
            AppendFovDiagnostic("damage_context_logger_create_failed");
        }
    }

    AppendFovDiagnostic("damage_context_installed");
    return true;
}

bool InstallDamageTelemetryHook() {
    if (!OffsetResolved(kDamageAccumulateResolved, kDamageAccumulateRva)) {
        AppendFovDiagnostic("damage_accumulate_offset_unresolved");
        return false;
    }
    if (g_damage_accumulate_original) {
        AppendFovDiagnostic("damage_accumulate_already_installed");
        return true;
    }

    HMODULE module = FindCurrentBuildModule(0);
    if (!module) {
        AppendFovDiagnostic("damage_accumulate_no_current_build_module");
        return false;
    }
    auto* base = reinterpret_cast<unsigned char*>(module);
    auto* target = base + kDamageAccumulateRva;
    if (std::memcmp(target, kDamageAccumulatePrologue.data(),
                    kDamageAccumulatePatchLength) != 0) {
        AppendFovDiagnostic("damage_accumulate_prologue_mismatch");
        return false;
    }

    constexpr size_t trampoline_size = kDamageAccumulatePatchLength + 12;
    auto* trampoline = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, trampoline_size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        AppendFovDiagnostic("damage_accumulate_trampoline_alloc_failed");
        return false;
    }
    std::memcpy(trampoline, target, kDamageAccumulatePatchLength);
    WriteAbsoluteJump(trampoline + kDamageAccumulatePatchLength,
                      reinterpret_cast<uintptr_t>(target + kDamageAccumulatePatchLength));

    DWORD old_protect = 0;
    if (!VirtualProtect(target, kDamageAccumulatePatchLength,
                        PAGE_EXECUTE_READWRITE, &old_protect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        AppendFovDiagnostic("damage_accumulate_virtualprotect_failed");
        return false;
    }

    g_damage_accumulate_trampoline = reinterpret_cast<uintptr_t>(trampoline);
    g_damage_accumulate_original = reinterpret_cast<DamageAccumulateFn>(trampoline);

    unsigned char patch[kDamageAccumulatePatchLength]{};
    std::memset(patch, 0x90, sizeof(patch));
    WriteAbsoluteJump(patch, reinterpret_cast<uintptr_t>(&DamageAccumulateTelemetry));
    std::memcpy(target, patch, sizeof(patch));
    FlushInstructionCache(GetCurrentProcess(), target, kDamageAccumulatePatchLength);
    DWORD ignored = 0;
    VirtualProtect(target, kDamageAccumulatePatchLength, old_protect, &ignored);

    if (InterlockedCompareExchange(&g_damage_flush_running, 1, 0) == 0) {
        uintptr_t logger = _beginthreadex(nullptr, 0, DamageTelemetryFlushThread,
                                           nullptr, 0, nullptr);
        if (logger) {
            CloseHandle(reinterpret_cast<HANDLE>(logger));
        } else {
            InterlockedExchange(&g_damage_flush_running, 0);
            AppendFovDiagnostic("damage_accumulate_logger_create_failed");
        }
    }
    AppendFovDiagnostic("damage_accumulate_installed");
    return true;
}

bool InstallWeaponAmountTelemetryHook() {
    if (!OffsetResolved(kWeaponAmountResolved, kWeaponAmountRva)) {
        AppendFovDiagnostic("weapon_amount_offset_unresolved");
        return false;
    }
    if (g_weapon_amount_original) {
        AppendFovDiagnostic("weapon_amount_already_installed");
        return true;
    }

    HMODULE module = FindCurrentBuildModule(0);
    if (!module) {
        AppendFovDiagnostic("weapon_amount_no_current_build_module");
        return false;
    }
    auto* base = reinterpret_cast<unsigned char*>(module);
    auto* target = base + kWeaponAmountRva;
    if (std::memcmp(target, kWeaponAmountPrologue.data(), kWeaponAmountPatchLength) != 0) {
        AppendFovDiagnostic("weapon_amount_prologue_mismatch");
        return false;
    }

    constexpr size_t trampoline_size = kWeaponAmountPatchLength + 12;
    auto* trampoline = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, trampoline_size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        AppendFovDiagnostic("weapon_amount_trampoline_alloc_failed");
        return false;
    }
    std::memcpy(trampoline, target, kWeaponAmountPatchLength);
    WriteAbsoluteJump(trampoline + kWeaponAmountPatchLength,
                      reinterpret_cast<uintptr_t>(target + kWeaponAmountPatchLength));

    DWORD old_protect = 0;
    if (!VirtualProtect(target, kWeaponAmountPatchLength, PAGE_EXECUTE_READWRITE, &old_protect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        AppendFovDiagnostic("weapon_amount_virtualprotect_failed");
        return false;
    }

    g_weapon_amount_trampoline = reinterpret_cast<uintptr_t>(trampoline);
    g_weapon_amount_original = reinterpret_cast<WeaponAmountFn>(trampoline);
    unsigned char patch[kWeaponAmountPatchLength]{};
    std::memset(patch, 0x90, sizeof(patch));
    WriteAbsoluteJump(patch, reinterpret_cast<uintptr_t>(&WeaponAmountTelemetry));
    std::memcpy(target, patch, sizeof(patch));
    FlushInstructionCache(GetCurrentProcess(), target, kWeaponAmountPatchLength);
    DWORD ignored = 0;
    VirtualProtect(target, kWeaponAmountPatchLength, old_protect, &ignored);
    AppendFovDiagnostic("weapon_amount_installed");
    return true;
}

bool InstallFovGetterHook(uintptr_t module_base) {
    if (!OffsetResolved(kFovGetterResolved, kFovGetterRva)) {
        AppendFovDiagnostic("fov_getter_offset_unresolved");
        return false;
    }
    if (g_fov_get_original) {
        AppendFovDiagnostic("getter_already_installed");
        return true;
    }

    HMODULE module = FindCurrentBuildModule(module_base);
    if (!module) {
        AppendFovDiagnostic("no_current_build_module_for_getter");
        return false;
    }
    auto* base = reinterpret_cast<unsigned char*>(module);
    auto* target = base + kFovGetterRva;
    if (std::memcmp(target, kFovGetterPrologue.data(), kFovGetterPatchLength) != 0) {
        char message[160]{};
        sprintf_s(message, "getter_prologue_mismatch addr=0x%llX bytes=%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(target)),
                  target[0], target[1], target[2], target[3], target[4], target[5],
                  target[6], target[7], target[8], target[9], target[10], target[11]);
        AppendFovDiagnostic(message);
        return false;
    }

    constexpr size_t trampoline_size = kFovGetterPatchLength + 12;
    auto* trampoline = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, trampoline_size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        AppendFovDiagnostic("getter_trampoline_alloc_failed");
        return false;
    }
    std::memcpy(trampoline, target, kFovGetterPatchLength);
    WriteAbsoluteJump(trampoline + kFovGetterPatchLength,
                      reinterpret_cast<uintptr_t>(target + kFovGetterPatchLength));

    DWORD old_protect = 0;
    if (!VirtualProtect(target, kFovGetterPatchLength, PAGE_EXECUTE_READWRITE, &old_protect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        AppendFovDiagnostic("getter_virtualprotect_failed");
        return false;
    }

    g_fov_get_trampoline = reinterpret_cast<uintptr_t>(trampoline);
    g_fov_get_original = reinterpret_cast<GetBaseFovFn>(trampoline);

    unsigned char patch[kFovGetterPatchLength]{};
    std::memset(patch, 0x90, sizeof(patch));
    WriteAbsoluteJump(patch, reinterpret_cast<uintptr_t>(&FovGetterThunk));
    std::memcpy(target, patch, sizeof(patch));
    FlushInstructionCache(GetCurrentProcess(), target, kFovGetterPatchLength);
    DWORD ignored = 0;
    VirtualProtect(target, kFovGetterPatchLength, old_protect, &ignored);
    AppendFovDiagnostic("getter_installed");
    return true;
}

float __fastcall FovViewGetterThunk(uintptr_t self) {
    GetViewFovFn original = g_fov_view_get_original;
    if (!original) return 0.0f;
    g_fov_last_camera.store(self, std::memory_order_release);
    const float current = original(self);
    const bool enabled = g_fov_enabled.load(std::memory_order_relaxed);
    const float applied = enabled ? g_fov_degrees.load(std::memory_order_relaxed) : current;
    const ULONG64 call_count = g_fov_view_get_call_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (call_count <= 5) {
        char message[192]{};
        sprintf_s(message, "view_getter_call=%llu self=0x%llX current=%.2f applied=%.2f",
                  static_cast<unsigned long long>(call_count),
                  static_cast<unsigned long long>(self),
                  static_cast<double>(current),
                  static_cast<double>(applied));
        AppendFovDiagnostic(message);
    }
    return applied;
}

bool InstallFovViewGetterHook(uintptr_t module_base) {
    if (!OffsetResolved(kFovViewGetterResolved, kFovViewGetterRva)) {
        AppendFovDiagnostic("fov_view_getter_offset_unresolved");
        return false;
    }
    if (g_fov_view_get_original) {
        AppendFovDiagnostic("view_getter_already_installed");
        return true;
    }

    HMODULE module = FindCurrentBuildModule(module_base);
    if (!module) {
        AppendFovDiagnostic("no_current_build_module_for_view_getter");
        return false;
    }
    auto* base = reinterpret_cast<unsigned char*>(module);
    auto* target = base + kFovViewGetterRva;
    if (std::memcmp(target, kFovViewGetterPrologue.data(), kFovViewGetterPatchLength) != 0) {
        char message[160]{};
        sprintf_s(message, "view_getter_prologue_mismatch addr=0x%llX bytes=%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(target)),
                  target[0], target[1], target[2], target[3], target[4], target[5],
                  target[6], target[7], target[8], target[9], target[10], target[11]);
        AppendFovDiagnostic(message);
        return false;
    }

    constexpr size_t trampoline_size = kFovViewGetterPatchLength + 12;
    auto* trampoline = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, trampoline_size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        AppendFovDiagnostic("view_getter_trampoline_alloc_failed");
        return false;
    }
    std::memcpy(trampoline, target, kFovViewGetterPatchLength);
    WriteAbsoluteJump(trampoline + kFovViewGetterPatchLength,
                      reinterpret_cast<uintptr_t>(target + kFovViewGetterPatchLength));

    DWORD old_protect = 0;
    if (!VirtualProtect(target, kFovViewGetterPatchLength, PAGE_EXECUTE_READWRITE, &old_protect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        AppendFovDiagnostic("view_getter_virtualprotect_failed");
        return false;
    }

    g_fov_view_get_trampoline = reinterpret_cast<uintptr_t>(trampoline);
    g_fov_view_get_original = reinterpret_cast<GetViewFovFn>(trampoline);

    unsigned char patch[kFovViewGetterPatchLength]{};
    std::memset(patch, 0x90, sizeof(patch));
    WriteAbsoluteJump(patch, reinterpret_cast<uintptr_t>(&FovViewGetterThunk));
    std::memcpy(target, patch, sizeof(patch));
    FlushInstructionCache(GetCurrentProcess(), target, kFovViewGetterPatchLength);
    DWORD ignored = 0;
    VirtualProtect(target, kFovViewGetterPatchLength, old_protect, &ignored);
    AppendFovDiagnostic("view_getter_installed");
    return true;
}

bool InstallFovOverrideHook(uintptr_t module_base) {
    if (!OffsetResolved(kFovSetterResolved, kFovSetterRva)) {
        AppendFovDiagnostic("fov_setter_offset_unresolved");
        return false;
    }
    if (g_fov_original) {
        AppendFovDiagnostic("already_installed");
        return true;
    }

    HMODULE module = FindCurrentBuildModule(module_base);
    if (!module) {
        AppendFovDiagnostic("no_current_build_module");
        return false;
    }
    auto* base = reinterpret_cast<unsigned char*>(module);
    if (!module_base) {
        auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            AppendFovDiagnostic("bad_dos_header");
            return false;
        }
        auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS64>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.SizeOfImage != kExpectedImageSize ||
            nt->FileHeader.TimeDateStamp != kExpectedPeTimestamp) {
            char message[128]{};
            sprintf_s(message, "build_identity_mismatch size=0x%lX timestamp=0x%08lX",
                      nt->OptionalHeader.SizeOfImage, nt->FileHeader.TimeDateStamp);
            AppendFovDiagnostic(message);
            return false;
        }
    }

    auto* target = base + kFovSetterRva;
    if (std::memcmp(target, kFovSetterPrologue.data(), kFovPatchLength) != 0) {
        char message[128]{};
        sprintf_s(message, "prologue_mismatch addr=0x%llX bytes=%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(target)),
                  target[0], target[1], target[2], target[3], target[4], target[5],
                  target[6], target[7], target[8], target[9], target[10], target[11]);
        AppendFovDiagnostic(message);
        return false;
    }

    constexpr size_t trampoline_size = kFovPatchLength + 12;
    auto* trampoline = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, trampoline_size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        AppendFovDiagnostic("trampoline_alloc_failed");
        return false;
    }
    std::memcpy(trampoline, target, kFovPatchLength);
    WriteAbsoluteJump(trampoline + kFovPatchLength,
                      reinterpret_cast<uintptr_t>(target + kFovPatchLength));

    DWORD old_protect = 0;
    if (!VirtualProtect(target, kFovPatchLength, PAGE_EXECUTE_READWRITE, &old_protect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        AppendFovDiagnostic("virtualprotect_failed");
        return false;
    }

    g_fov_trampoline = reinterpret_cast<uintptr_t>(trampoline);
    g_fov_original = reinterpret_cast<SetBaseFovFn>(trampoline);

    unsigned char patch[kFovPatchLength]{};
    std::memset(patch, 0x90, sizeof(patch));
    WriteAbsoluteJump(patch, reinterpret_cast<uintptr_t>(&FovOverrideThunk));
    std::memcpy(target, patch, sizeof(patch));
    FlushInstructionCache(GetCurrentProcess(), target, kFovPatchLength);
    DWORD ignored = 0;
    VirtualProtect(target, kFovPatchLength, old_protect, &ignored);
    AppendFovDiagnostic("installed");
    return true;
}

bool SetFovOverride(bool enabled, float degrees, uintptr_t module_base) {
    if (!enabled) {
        g_fov_enabled.store(false, std::memory_order_release);
        AppendFovDiagnostic("disabled");
        return true;
    }
    const bool setter_ok = InstallFovOverrideHook(module_base);
    const bool getter_ok = InstallFovGetterHook(module_base);
    const bool view_getter_ok = InstallFovViewGetterHook(module_base);
    if (!setter_ok && !getter_ok && !view_getter_ok) {
        AppendFovDiagnostic("enable_failed");
        return false;
    }
    const float clamped = std::clamp(degrees, 50.0f, 150.0f);
    g_fov_degrees.store(clamped, std::memory_order_release);
    g_fov_enabled.store(true, std::memory_order_release);

    // A camera can stop calling the setter after initialization. Reapply the
    // validated original setter to the last live camera object so changing the
    // UI value updates +0x918 immediately instead of waiting for a camera
    // transition or a future engine setter call.
    const uintptr_t camera = g_fov_last_camera.load(std::memory_order_acquire);
    if (camera && g_fov_original) {
        g_fov_original(camera, clamped);
        char reapply[160]{};
        sprintf_s(reapply, "reapply_setter self=0x%llX degrees=%.2f",
                  static_cast<unsigned long long>(camera),
                  static_cast<double>(clamped));
        AppendFovDiagnostic(reapply);
    } else {
        AppendFovDiagnostic("reapply_setter_no_camera");
    }

    char message[192]{};
    sprintf_s(message, "enabled_degrees=%.2f setter=%d getter=%d view_getter=%d",
              static_cast<double>(clamped), setter_ok ? 1 : 0, getter_ok ? 1 : 0,
              view_getter_ok ? 1 : 0);
    AppendFovDiagnostic(message);
    return true;
}

LiveEngine::LiveEngine() {
    // The feature list mirrors the OwHelper catalog.  OneHitKill is wired to
    // the current-build damage-result context hook above; the remaining field
    // rows stay fail-closed until their current-build write targets are proven.
    features_ = {
        {L"ohk",              L"OneHitKillForMe_Always",       0x44,  9999.f, 0.f,  false, 0x0,   false},
        {L"skill_mult",       L"SkillsModsMultiplier",         0x9C,  9999.f, 0.f,  false, 0x0,   false},
        {L"super_skill",      L"SuperSkillsMode",              0x9C,  9999.f, 0.f,  false, 0x0,   false},
        {L"cooldown",         L"SkillCooldownForMe",           0x64C, 0.f,    0.f,  false, 0x0,   false},
        {L"invulnerable",     L"InvulnerableOnlyForMe",        0x50C, 0.f,    0.f,  true,  0x4D4, false},
        {L"max_energy",       L"GetMaxEnergy",                 0x64C, 0.f,    0.f,  false, 0x0,   false},
        {L"detector",         L"EnemynLootDetector",           0x200, 9999.f, 0.f,  false, 0x0,   false},

        {L"combo",            L"AlwaysMaxCombo",               0x44,  9999.f, 0.f,  false, 0x0,   false},
        {L"infinite_ammo",    L"Ammo1",                       0x44,  9999.f, 0.f,  false, 0x0,   false},
        {L"incarnon_ammo",    L"AmmoIncarnon",                 0x44,  9999.f, 0.f,  false, 0x0,   false},
        {L"regen_on",         L"AmmoRegenOn",                  0x44,  9999.f, 0.f,  false, 0x0,   false},
        {L"regen_off",        L"AmmoRegenOff",                 0x44,  0.f,    0.f,  false, 0x0,   false},
        {L"incarnon_charge",  L"FastIncarnonCharge",           0x44,  9999.f, 0.f,  false, 0x0,   false},
        {L"tauron_fast",      L"FastChargeTauronStrike",       0x44,  9999.f, 0.f,  false, 0x0,   false},
        {L"tauron_faster",    L"FastChargeTauronStrikeFaster", 0x44,  9999.f, 0.f,  false, 0x0,   false},

        {L"koumei",           L"AutoChallengeKoumei",           0x200, 1.f,    0.f,  false, 0x0,   false},
        {L"eximus",           L"AlwaysEximus",                 0x200, 1.f,    0.f,  false, 0x0,   false},
        {L"vacuum_always",    L"Alwaysvacuum",                 0x44,  9999.f, 0.f,  false, 0x0,   false},
        {L"vacuum_new",       L"LootVacuumNew",                0x200, 9999.f, 0.f,  false, 0x0,   false},
        {L"vacuum_old",       L"LootVacuumOld",                0x200, 255.f,  0.f,  false, 0x0,   false},
        {L"loot_hotkey",      L"LootHotkey",                   0x200, 1.f,    0.f,  false, 0x0,   false},
        {L"vacuum_limit",     L"IncreaseLootVacuumLimit",      0x200, 255.f,  0.f,  false, 0x0,   false},
        {L"fov",              L"FovChanger",                  0x44,  1.f,    0.f,  false, 0x0,   false},
    };
    numerics_ = {
        {L"skill_mult_value", L"SkillsMultiplier_Value", 0x9C, 1, 20, 1, 1},
        {L"vacuum_value",     L"LootVacuumOldValue",    0x200, 10, 255, 50, 5},
        {L"fov_value",        L"FovChangerValue",       0x44, 50, 150, 90, 1},
    };
}

DWORD FindWarframePid() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD pid = 0;
    if (Process32FirstW(snap, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"Warframe.x64.exe") == 0) {
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return pid;
}

bool GetModuleInfo(DWORD pid, const wchar_t* module_name,
                   uintptr_t& base_out, uintptr_t& size_out) {
    base_out = 0;
    size_out = 0;
    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!proc) return false;

    HMODULE modules[256]{};
    DWORD needed = 0;
    if (!EnumProcessModules(proc, modules, sizeof(modules), &needed)) {
        CloseHandle(proc);
        return false;
    }

    const DWORD count = std::min<DWORD>(needed / sizeof(HMODULE), static_cast<DWORD>(std::size(modules)));
    for (DWORD i = 0; i < count; ++i) {
        wchar_t name[MAX_PATH]{};
        if (!GetModuleBaseNameW(proc, modules[i], name, MAX_PATH) ||
            _wcsicmp(name, module_name) != 0) {
            continue;
        }
        MODULEINFO info{};
        if (!GetModuleInformation(proc, modules[i], &info, sizeof(info))) {
            CloseHandle(proc);
            return false;
        }
        base_out = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
        size_out = static_cast<uintptr_t>(info.SizeOfImage);
        CloseHandle(proc);
        return base_out != 0 && size_out != 0;
    }

    CloseHandle(proc);
    return false;
}

bool LiveEngine::DetectGame() {
    warframe_pid_ = FindWarframePid();
    if (!warframe_pid_) {
        warframe_base_ = 0;
        warframe_size_ = 0;
        player_base_ = 0;
        status_ = L"Warframe not running";
        return false;
    }
    if (!GetModuleInfo(warframe_pid_, L"Warframe.x64.exe", warframe_base_, warframe_size_)) {
        status_ = L"Cannot read Warframe module information";
        return false;
    }
    uint32_t pe_header_offset = 0;
    uint32_t pe_timestamp = 0;
    if (!ReadBytes(warframe_base_ + 0x3C, &pe_header_offset, sizeof(pe_header_offset)) ||
        !ReadBytes(warframe_base_ + pe_header_offset + 0x08, &pe_timestamp, sizeof(pe_timestamp))) {
        status_ = L"Cannot read Warframe PE identity";
        return false;
    }
    if (warframe_size_ != kExpectedImageSize || pe_timestamp != kExpectedPeTimestamp) {
        status_ = L"Warframe build mismatch; refresh offsets before applying";
        player_base_ = 0;
        return false;
    }
    if (!ResolvePlayerBase()) {
        status_ = L"Player anchor not uniquely resolved (read-only)";
        return false;
    }
    status_ = L"Warframe live; structural player anchor resolved; feature offsets pending";
    return true;
}

void LiveEngine::SetGame(const std::wstring& game_dir) {
    game_dir_ = game_dir;
    while (!game_dir_.empty() && (game_dir_.back() == L'\\' || game_dir_.back() == L'/')) {
        game_dir_.pop_back();
    }
    ini_path_ = game_dir_ + L"\\WarframeHelper.ini";
}

bool LiveEngine::ResolvePlayerBase() {
    player_base_ = 0;
    feature_offsets_verified_ = false;
    if (!warframe_base_ || !warframe_size_ || !warframe_pid_) return false;

    const auto in_module = [this](uintptr_t address) {
        return address >= warframe_base_ && address < warframe_base_ + warframe_size_;
    };
    const auto plausible_heap = [this, &in_module](uintptr_t address) {
        return address >= 0x10000 && address < 0x00007FFFFFFFF000ULL && !in_module(address);
    };

    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                              FALSE, warframe_pid_);
    if (!proc) return false;

    const auto fail = [&]() {
        CloseHandle(proc);
        player_base_ = 0;
        return false;
    };
    const auto read_live = [&](uintptr_t address, void* out, size_t size) {
        SIZE_T got = 0;
        return ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(address),
                                 out, size, &got) && got == size;
    };
    const auto read_ptr = [&](uintptr_t address, uintptr_t& out) {
        return read_live(address, &out, sizeof(out));
    };
    const auto read_u32 = [&](uintptr_t address, uint32_t& out) {
        return read_live(address, &out, sizeof(out));
    };

    // Resolve the current gGameRules object through the client's hashed
    // registry. The registry table stores sorted 16-byte {key, shared_ptr}
    // entries; the pointer target is the live heap object, not a static RVA.
    uintptr_t table = 0;
    uint32_t table_bytes = 0;
    if (!read_ptr(warframe_base_ + kRegistryRva + 0x138, table) ||
        !read_u32(warframe_base_ + kRegistryRva + 0x140, table_bytes) ||
        !plausible_heap(table) || table_bytes == 0 || table_bytes > 0x10000 ||
        (table_bytes % 16) != 0) {
        return fail();
    }

    uintptr_t game_rules = 0;
    for (uint32_t offset = 0; offset < table_bytes; offset += 16) {
        uint32_t key = 0;
        uintptr_t holder = 0;
        uintptr_t target = 0;
        if (!read_u32(table + offset, key) ||
            !read_ptr(table + offset + 8, holder)) {
            return fail();
        }
        if (key != kGameRulesKey || !plausible_heap(holder) ||
            !read_ptr(holder, target) || !plausible_heap(target)) {
            continue;
        }
        game_rules = target;
        break;
    }
    if (!game_rules) return fail();

    uintptr_t rules_vtable = 0;
    if (!read_ptr(game_rules, rules_vtable) ||
        rules_vtable != warframe_base_ + kGameRulesVtableRva) {
        return fail();
    }

    // sub_14040C4F0 (the current GameRules virtual at +0x3B0/944) copies
    // this weak-pointer vector from GameRules +0x230 and its byte count at
    // +0x238. This is the code-confirmed humanPlayers collection.
    uintptr_t human_data = 0;
    uint32_t human_bytes = 0;
    if (!read_ptr(game_rules + kHumanPlayersDataOffset, human_data) ||
        !read_u32(game_rules + kHumanPlayersBytesOffset, human_bytes) ||
        !plausible_heap(human_data) || human_bytes == 0 ||
        human_bytes > 0x10000 || (human_bytes % sizeof(uintptr_t)) != 0) {
        return fail();
    }

    std::vector<uintptr_t> holders(human_bytes / sizeof(uintptr_t));
    if (!read_live(human_data, holders.data(), human_bytes)) return fail();

    uintptr_t candidate = 0;
    size_t candidate_count = 0;
    for (const uintptr_t holder : holders) {
        uintptr_t human = 0;
        uintptr_t human_vtable = 0;
        if (!plausible_heap(holder) || !read_ptr(holder, human) ||
            !plausible_heap(human) || !read_ptr(human, human_vtable) ||
            human_vtable != warframe_base_ + kHumanPlayerVtableRva) {
            continue;
        }

        // LotusHumanPlayer::GetAvatar / sub_140123E90 returns
        // **(QWORD **)(human + 0x148) in this build.
        uintptr_t avatar_holder = 0;
        uintptr_t avatar = 0;
        uintptr_t avatar_vtable = 0;
        uint32_t faction = 0;
        uintptr_t sub_holder = 0;
        uintptr_t inner = 0;
        uintptr_t sub_vtable = 0;
        uintptr_t friendly_virtual = 0;
        float position[4]{};
        if (!read_ptr(human + kHumanPlayerAvatarHolderOffset, avatar_holder) ||
            !plausible_heap(avatar_holder) || !read_ptr(avatar_holder, avatar) ||
            !plausible_heap(avatar) || !read_ptr(avatar, avatar_vtable) ||
            !in_module(avatar_vtable) ||
            !read_u32(avatar + kAvatarFactionOffset, faction) || faction == 0) {
            continue;
        }

        // Orbiter/MainMenuAvatar has a valid movement position but may not
        // expose the gameplay Avatar +0x588 friendly-subobject chain.
        bool orbiter_candidate =
            avatar_vtable == warframe_base_ + kOrbiterAvatarVtableRva &&
            read_live(avatar + kAvatarPositionOffset, position, sizeof(position));
        if (orbiter_candidate) {
            for (const float value : position) {
                if (!std::isfinite(value) || std::fabs(value) > 10000000.0f) {
                    orbiter_candidate = false;
                    break;
                }
            }
        }

        bool gameplay_candidate = false;
        if (read_ptr(avatar + kAvatarSubobjectOffset, sub_holder) &&
            plausible_heap(sub_holder) && read_ptr(sub_holder, inner) &&
            plausible_heap(inner) && read_ptr(inner, sub_vtable) &&
            in_module(sub_vtable) &&
            read_ptr(sub_vtable + kAvatarFriendlyVtableSlot, friendly_virtual) &&
            in_module(friendly_virtual)) {
            gameplay_candidate = true;
        }
        if (!orbiter_candidate && !gameplay_candidate) continue;

        ++candidate_count;
        candidate = avatar;
    }

    if (candidate_count != 1) return fail();
    player_base_ = candidate;
    CloseHandle(proc);
    return true;
}

bool LiveEngine::ReadBytes(uintptr_t addr, void* out_buf, size_t n) {
    if (!warframe_pid_ || !addr || !out_buf || n == 0) return false;
    HANDLE proc = (warframe_pid_ == GetCurrentProcessId())
        ? GetCurrentProcess()
        : OpenProcess(PROCESS_VM_READ, FALSE, warframe_pid_);
    if (!proc) return false;
    SIZE_T got = 0;
    BOOL ok = ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(addr), out_buf, n, &got);
    if (proc != GetCurrentProcess()) CloseHandle(proc);
    return ok && got == n;
}

bool LiveEngine::WriteBytes(uintptr_t addr, const void* in_buf, size_t n) {
    if (!warframe_pid_ || !addr || !in_buf || n == 0) return false;
    HANDLE proc = (warframe_pid_ == GetCurrentProcessId())
        ? GetCurrentProcess()
        : OpenProcess(PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, warframe_pid_);
    if (!proc) return false;
    SIZE_T written = 0;
    BOOL ok = WriteProcessMemory(proc, reinterpret_cast<LPVOID>(addr), in_buf, n, &written);
    if (proc != GetCurrentProcess()) CloseHandle(proc);
    return ok && written == n;
}

bool LiveEngine::LoadProfile(const std::wstring& ini_path) {
    ini_path_ = ini_path;
    for (auto& f : features_) {
        wchar_t buf[32]{};
        GetPrivateProfileStringW(L"Enable_Cheats", f.ini_key, L"false", buf,
                                 static_cast<DWORD>(std::size(buf)), ini_path.c_str());
        std::wstring v(buf);
        std::transform(v.begin(), v.end(), v.begin(), ::towlower);
        f.enabled = (v == L"true" || v == L"1");
    }
    wchar_t alt_buf[32]{};
    GetPrivateProfileStringW(L"Enable_Cheats", L"OneHitKillForMe(RAlt)",
                             L"false", alt_buf,
                             static_cast<DWORD>(std::size(alt_buf)), ini_path.c_str());
    std::wstring alt_value(alt_buf);
    std::transform(alt_value.begin(), alt_value.end(), alt_value.begin(), ::towlower);
    g_one_hit_kill_alt_only.store(alt_value == L"true" || alt_value == L"1",
                                  std::memory_order_release);
    for (auto& n : numerics_) {
        n.value = std::clamp<int>(
            static_cast<int>(GetPrivateProfileIntW(L"Enable_Cheats", n.ini_key,
                                                    static_cast<UINT>(n.min), ini_path.c_str())),
            n.min, n.max);
    }
    return true;
}

bool LiveEngine::ApplyOne(const LiveFeature& feature) {
    if (!player_base_ || !feature_offsets_verified_) {
        status_ = L"Current-build feature offsets are not verified; no write performed";
        return false;
    }
    if (feature.enabled) {
        float value = feature.write_value;
        if (feature.needs_max) {
            uint8_t buf[4]{};
            if (ReadBytes(player_base_ + feature.max_offset, buf, 4)) {
                memcpy(&value, buf, 4);
                if (value <= 0.f) value = feature.write_value;
            }
        }
        return WriteBytes(player_base_ + feature.field_offset, &value, 4);
    } else {
        if (feature.needs_max) {
            uint8_t buf[4]{};
            if (ReadBytes(player_base_ + feature.max_offset, buf, 4)) {
                float max_v = 0.f;
                memcpy(&max_v, buf, 4);
                return WriteBytes(player_base_ + feature.field_offset, &max_v, 4);
            }
        }
        float restore = feature.restore_value;
        return WriteBytes(player_base_ + feature.field_offset, &restore, 4);
    }
}

bool LiveEngine::ApplyNumeric(const LiveNumeric& numeric) {
    if (!player_base_ || !feature_offsets_verified_) {
        status_ = L"Current-build feature offsets are not verified; no write performed";
        return false;
    }
    float value = static_cast<float>(numeric.value);
    return WriteBytes(player_base_ + numeric.field_offset, &value, 4);
}

bool LiveEngine::ApplyAll() {
    // Always re-resolve: PID and base stay stable across Orbiter->Mission,
    // but the avatar object changes. Without this the anchor freezes at
    // whatever state DetectGame() first saw it in.
    DetectGame();

    bool fov_enabled = false;
    float fov_degrees = 90.0f;
    for (const auto& f : features_) {
        if (wcscmp(f.ini_key, L"FovChanger") == 0) {
            fov_enabled = f.enabled;
            break;
        }
    }
    for (const auto& n : numerics_) {
        if (wcscmp(n.ini_key, L"FovChangerValue") == 0) {
            fov_degrees = static_cast<float>(n.value);
            break;
        }
    }

    // FOV uses the current-build camera setter hook and does not depend on the
    // unresolved Avatar write table used by the other features.
    char fov_request[96]{};
    sprintf_s(fov_request, "apply_request enabled=%d degrees=%.2f",
              fov_enabled ? 1 : 0, static_cast<double>(fov_degrees));
    AppendFovDiagnostic(fov_request);
    const bool fov_applied = fov_enabled &&
        SetFovOverride(true, fov_degrees, warframe_base_);
    if (fov_enabled && !fov_applied) {
        status_ = L"FOV hook unavailable: current-build setter signature mismatch";
        return false;
    }
    if (!fov_enabled) SetFovOverride(false, 0.0f, warframe_base_);

    bool ohk_enabled = false;
    for (const auto& f : features_) {
        if (wcscmp(f.ini_key, L"OneHitKillForMe_Always") == 0) {
            ohk_enabled = f.enabled;
            break;
        }
    }
    const bool damage_entry_hook_ok = !ohk_enabled || InstallDamageTelemetryHook();
    const bool ohk_hook_ok = !ohk_enabled || InstallDamageContextTelemetryHook();
    g_one_hit_kill_enabled.store(ohk_enabled && damage_entry_hook_ok && ohk_hook_ok,
                                  std::memory_order_release);
    char ohk_request[160]{};
    sprintf_s(ohk_request, "ohk enabled=%d entry_hook=%d hook=%d alt_only=%d overrides=%lld",
              ohk_enabled ? 1 : 0,
              damage_entry_hook_ok ? 1 : 0,
              ohk_hook_ok ? 1 : 0,
              g_one_hit_kill_alt_only.load(std::memory_order_relaxed) ? 1 : 0,
              static_cast<long long>(InterlockedCompareExchange64(
                  &g_one_hit_kill_override_count, 0, 0)));
    AppendFovDiagnostic(ohk_request);
    if (ohk_enabled && (!damage_entry_hook_ok || !ohk_hook_ok)) {
        status_ = L"One-Hit Kill hook unavailable: current-build damage anchor mismatch";
        return false;
    }

    if (!player_base_) {
        if (fov_applied) {
            status_ = L"Applied FOV override; other feature offsets pending";
            return true;
        }
        status_ = L"Engine: player base missing";
        return false;
    }
    if (!feature_offsets_verified_) {
        if (fov_applied) {
            status_ = L"Applied FOV override; other feature offsets pending";
            return true;
        }
        status_ = L"Current-build feature offsets pending; no writes performed";
        return false;
    }
    int applied = fov_applied ? 1 : 0;
    for (const auto& f : features_) {
        if (wcscmp(f.ini_key, L"FovChanger") == 0) continue;
        if (f.enabled && ApplyOne(f)) ++applied;
    }
    for (const auto& n : numerics_) {
        if (wcscmp(n.ini_key, L"FovChangerValue") == 0) continue;
        if (ApplyNumeric(n)) ++applied;
    }
    status_ = L"Applied " + std::to_wstring(applied) + L" fields live";
    return applied > 0;
}

void LiveEngine::DisableAll() {
    if (!player_base_ || !feature_offsets_verified_) return;
    for (const auto& f : features_) {
        // Force off and write restore value.
        float value = f.restore_value;
        if (f.needs_max) {
            uint8_t buf[4]{};
            if (ReadBytes(player_base_ + f.max_offset, buf, 4)) {
                memcpy(&value, buf, 4);
            }
        }
        WriteBytes(player_base_ + f.field_offset, &value, 4);
    }
}

} // namespace wfmaxon