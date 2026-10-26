#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

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

extern "C" void DamageContextStub();
extern "C" void XmmProbeInvoke(void* target, void* output);

static uintptr_t g_trampoline = 0;
static volatile LONG g_callback_count = 0;

extern "C" void DamageContextCallback(DamageContextHook* context) {
    if (!context) return;
    context->trampoline = g_trampoline;
    InterlockedIncrement(&g_callback_count);
}

static void WriteAbsoluteJump(unsigned char* destination, uintptr_t target) {
    destination[0] = 0x48;
    destination[1] = 0xB8;
    std::memcpy(destination + 2, &target, sizeof(target));
    destination[10] = 0xFF;
    destination[11] = 0xE0;
}

int main() {
    constexpr size_t code_size = 0x40;
    auto* code = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, code_size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!code) return 10;
    std::memset(code, 0xCC, code_size);

    // cvttss2si ecx,xmm1; cmp ecx,80000000h; je branch_target
    const unsigned char original[12] = {
        0xF3, 0x0F, 0x2C, 0xC9,
        0x81, 0xF9, 0x00, 0x00, 0x00, 0x80,
        0x74, 0x14,
    };
    std::memcpy(code, original, sizeof(original));
    // Fall-through result: 0x1234.
    code[12] = 0xB8;
    code[13] = 0x34;
    code[14] = 0x12;
    code[15] = 0x00;
    code[16] = 0x00;
    code[17] = 0xC3;
    // Taken-branch result: 0x5678.
    code[0x20] = 0xB8;
    code[0x21] = 0x78;
    code[0x22] = 0x56;
    code[0x23] = 0x00;
    code[0x24] = 0x00;
    code[0x25] = 0xC3;

    constexpr size_t trampoline_size = 10 + 14 + 12;
    auto* trampoline = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, trampoline_size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) return 11;

    std::memcpy(trampoline, code, 10);
    // Invert JE to JNE over an absolute jump to the taken target.
    trampoline[10] = 0x75;
    trampoline[11] = 0x0C;
    WriteAbsoluteJump(trampoline + 12, reinterpret_cast<uintptr_t>(code + 0x20));
    WriteAbsoluteJump(trampoline + 24, reinterpret_cast<uintptr_t>(code + 12));

    g_trampoline = reinterpret_cast<uintptr_t>(trampoline);
    WriteAbsoluteJump(code, reinterpret_cast<uintptr_t>(&DamageContextStub));
    FlushInstructionCache(GetCurrentProcess(), code, code_size);

    using SyntheticFn = int (__fastcall *)(float, float);
    auto fn = reinterpret_cast<SyntheticFn>(code);
    const int normal = fn(0.0f, 5.0f);
    const int taken = fn(0.0f, std::numeric_limits<float>::quiet_NaN());

    const std::array<std::array<unsigned char, 16>, 10> expected = {{
        {{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}},
        {{0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF}},
        {{0xF0, 0xE1, 0xD2, 0xC3, 0xB4, 0xA5, 0x96, 0x87, 0x78, 0x69, 0x5A, 0x4B, 0x3C, 0x2D, 0x1E, 0x0F}},
        {{0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE}},
        {{0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00}},
        {{0x05, 0x0A, 0x0F, 0x14, 0x19, 0x1E, 0x23, 0x28, 0x2D, 0x32, 0x37, 0x3C, 0x41, 0x46, 0x4B, 0x50}},
        {{0xA5, 0x5A, 0xC3, 0x3C, 0x96, 0x69, 0xF0, 0x0F, 0x69, 0x96, 0x3C, 0xC3, 0x5A, 0xA5, 0x0F, 0xF0}},
        {{0x03, 0x06, 0x09, 0x0C, 0x0F, 0x12, 0x15, 0x18, 0x1B, 0x1E, 0x21, 0x24, 0x27, 0x2A, 0x2D, 0x30}},
        {{0x0F, 0x1E, 0x2D, 0x3C, 0x4B, 0x5A, 0x69, 0x78, 0x87, 0x96, 0xA5, 0xB4, 0xC3, 0xD2, 0xE1, 0xF0}},
        {{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99}},
    }};
    std::array<unsigned char, 160> observed{};
    XmmProbeInvoke(reinterpret_cast<void*>(code), observed.data());
    bool xmm_preserved = true;
    for (size_t i = 0; i < expected.size(); ++i) {
        if (std::memcmp(observed.data() + i * 16, expected[i].data(), 16) != 0) {
            xmm_preserved = false;
            break;
        }
    }
    const LONG callbacks = InterlockedCompareExchange(&g_callback_count, 0, 0);

    VirtualFree(trampoline, 0, MEM_RELEASE);
    VirtualFree(code, 0, MEM_RELEASE);

    std::printf("normal=0x%X taken=0x%X callbacks=%ld xmm_full=%s\n",
                normal, taken, callbacks, xmm_preserved ? "true" : "false");
    if (normal != 0x1234 || taken != 0x5678 || callbacks != 3 || !xmm_preserved) return 13;
    return 0;
}
