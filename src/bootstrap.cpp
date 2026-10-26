// ============================================================================
// WF_MAXON — Bootstrap (reflective-loader stub)
// ============================================================================
//
// Called by `WF_MAXONLoader.exe` from a remote thread after copying the
// payload image into the target process. Performs:
//
//   1. Base relocations (delta between preferred image base and actual load)
//   2. Import resolution (LoadLibraryW + GetProcAddress on the loaders's
//      API pointers, walking the IAT)
//   3. TLS callbacks (DLL_PROCESS_ATTACH)
//   4. DllMain(DLL_PROCESS_ATTACH, NULL)
//
// The function lives at a fixed RVA in the payload (exported as "Bootstrap"),
// so the loader can call it after copying. The image's preferred base is
// read from the PE header and the actual load address comes from the loader
// as `image_base`.
// ============================================================================

#include "config.h"
#include <cstring>

#ifdef WF_MAXON_DLL

namespace {

using NativeUnicodeString = struct {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};
using NativeAnsiString = struct {
    USHORT Length;
    USHORT MaximumLength;
    PCHAR Buffer;
};
using LdrLoadDllFn = LONG (NTAPI*)(PWSTR, ULONG, NativeUnicodeString*, PHANDLE);
using LdrGetProcedureAddressFn = LONG (NTAPI*)(HMODULE, NativeAnsiString*, ULONG, PVOID*);

struct BootstrapArgs {
    LPVOID image_base;
    LdrLoadDllFn load_library_w;
    LdrGetProcedureAddressFn get_proc_address;
    LPVOID payload_path;   // stage pointer during manual-map diagnostics
};

DWORD ApplyRelocations(LPVOID image_base, PIMAGE_NT_HEADERS64 nt) {
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (!dir.VirtualAddress || !dir.Size) return 0;

    // The loader passes us the actual load address. We compute the delta
    // from the PE's preferred ImageBase. Most reflective loaders carry the
    // delta through; we recompute it here so Bootstrap is self-contained.
    SIZE_T preferred = nt->OptionalHeader.ImageBase;
    SIZE_T actual    = reinterpret_cast<SIZE_T>(image_base);
    if (preferred == actual) return 0;
    auto* relocs = reinterpret_cast<IMAGE_BASE_RELOCATION*>(
        static_cast<uint8_t*>(image_base) + dir.VirtualAddress);
    SIZE_T processed = 0;
    while (processed < dir.Size) {
        DWORD page_rva = relocs->VirtualAddress;
        DWORD block_size = relocs->SizeOfBlock;
        if (block_size == 0) break;
        DWORD count = (block_size - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
        auto* entries = reinterpret_cast<WORD*>(relocs + 1);
        for (DWORD i = 0; i < count; ++i) {
            int type   = entries[i] >> 12;
            int offset = entries[i] & 0xFFF;
            if (type == IMAGE_REL_BASED_DIR64) {
                auto* p = reinterpret_cast<SIZE_T*>(
                    static_cast<uint8_t*>(image_base) + page_rva + offset);
                *p += actual - preferred;
            } else if (type == IMAGE_REL_BASED_HIGHLOW) {
                auto* p = reinterpret_cast<DWORD*>(
                    static_cast<uint8_t*>(image_base) + page_rva + offset);
                *p += static_cast<DWORD>(actual - preferred);
            } else if (type == IMAGE_REL_BASED_ABSOLUTE) {
                // padding
            }
        }
        relocs = reinterpret_cast<IMAGE_BASE_RELOCATION*>(
            reinterpret_cast<uint8_t*>(relocs) + block_size);
        processed += block_size;
    }
    return 0;
}

void UpdateBootstrapStage(LPVOID stage_pointer, DWORD stage);

HMODULE LoadImportedModule(LdrLoadDllFn load_library_w, const char* name, LPVOID stage_pointer) {
    UpdateBootstrapStage(stage_pointer, 222);
    // Convert the ANSI PE import name to the UNICODE_STRING expected by
    // ntdll!LdrLoadDll. This matches the native-loader path used by OwHelper.
    wchar_t wide_name[MAX_PATH]{};
    size_t i = 0;
    for (; name && name[i] && i + 1 < sizeof(wide_name) / sizeof(wide_name[0]); ++i) {
        wide_name[i] = static_cast<wchar_t>(static_cast<unsigned char>(name[i]));
    }
    if (!name || name[i] != '\0') return nullptr;
    UpdateBootstrapStage(stage_pointer, 223);
    NativeUnicodeString module_name{};
    module_name.Length = static_cast<USHORT>(i * sizeof(wchar_t));
    module_name.MaximumLength = static_cast<USHORT>((i + 1) * sizeof(wchar_t));
    module_name.Buffer = wide_name;
    HANDLE module = nullptr;
    const LONG status = load_library_w
        ? load_library_w(nullptr, 0, &module_name, &module)
        : static_cast<LONG>(-1);
    UpdateBootstrapStage(stage_pointer, 224);
    return status >= 0 ? reinterpret_cast<HMODULE>(module) : nullptr;
}

void UpdateBootstrapStage(LPVOID stage_pointer, DWORD stage) {
    if (stage_pointer) {
        *reinterpret_cast<volatile DWORD*>(stage_pointer) = stage;
    }
}

DWORD ResolveImports(LPVOID image_base, PIMAGE_NT_HEADERS64 nt,
                     LdrLoadDllFn load_library_w, LdrGetProcedureAddressFn get_proc_address,
                     LPVOID stage_pointer) {
    UpdateBootstrapStage(stage_pointer, 21);
    if (!load_library_w || !get_proc_address) return 1;

    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) return 0;

    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        static_cast<uint8_t*>(image_base) + dir.VirtualAddress);
    while (desc->Name) {
        UpdateBootstrapStage(stage_pointer, 22);
        auto* dll_name = reinterpret_cast<const char*>(
            static_cast<uint8_t*>(image_base) + desc->Name);
        UpdateBootstrapStage(stage_pointer, 221);
        HMODULE mod = LoadImportedModule(load_library_w, dll_name, stage_pointer);
        UpdateBootstrapStage(stage_pointer, 224);
        if (!mod) return 2;

        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
            static_cast<uint8_t*>(image_base) + desc->FirstThunk);
        auto* orig = desc->OriginalFirstThunk
            ? reinterpret_cast<IMAGE_THUNK_DATA*>(
                static_cast<uint8_t*>(image_base) + desc->OriginalFirstThunk)
            : thunk;

        while (orig->u1.AddressOfData) {
            UpdateBootstrapStage(stage_pointer, 24);
            FARPROC proc = nullptr;
            PVOID resolved = nullptr;
            LONG status = -1;
            if (orig->u1.Ordinal & IMAGE_ORDINAL_FLAG64) {
                status = get_proc_address(mod, nullptr,
                                          static_cast<ULONG>(orig->u1.Ordinal & 0xFFFF),
                                          &resolved);
            } else {
                auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                    static_cast<uint8_t*>(image_base) + orig->u1.AddressOfData);
                size_t name_length = 0;
                while (import->Name[name_length] != '\0' && name_length < 0xFFFF) {
                    ++name_length;
                }
                NativeAnsiString function_name{};
                function_name.Length = static_cast<USHORT>(name_length);
                function_name.MaximumLength = static_cast<USHORT>(name_length);
                function_name.Buffer = reinterpret_cast<PCHAR>(import->Name);
                status = get_proc_address(mod, &function_name, 0, &resolved);
            }
            if (status < 0 || !resolved) return 3;
            proc = reinterpret_cast<FARPROC>(resolved);
            thunk->u1.Function = reinterpret_cast<ULONGLONG>(proc);
            ++thunk;
            ++orig;
        }
        UpdateBootstrapStage(stage_pointer, 25);
        ++desc;
    }
    return 0;
}

void CallTlsCallbacks(LPVOID image_base, PIMAGE_NT_HEADERS64 nt, DWORD reason) {
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (!dir.VirtualAddress || !dir.Size) return;
    auto* tls = reinterpret_cast<IMAGE_TLS_DIRECTORY64*>(
        static_cast<uint8_t*>(image_base) + dir.VirtualAddress);
    auto* callbacks = reinterpret_cast<PIMAGE_TLS_CALLBACK*>(tls->AddressOfCallBacks);
    if (!callbacks) return;
    while (*callbacks) {
        (*callbacks)(static_cast<PVOID>(image_base), reason, nullptr);
        ++callbacks;
    }
}

__declspec(safebuffers) unsigned long RunBootstrap(BootstrapArgs* args) {
    UpdateBootstrapStage(args->payload_path, 10);
    auto base = static_cast<uint8_t*>(args->image_base);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);

    UpdateBootstrapStage(args->payload_path, 11);
    if (ApplyRelocations(args->image_base, nt) != 0) return 0x100;
    UpdateBootstrapStage(args->payload_path, 20);
    if (ResolveImports(args->image_base, nt, args->load_library_w, args->get_proc_address,
                       args->payload_path) != 0) return 0x101;
    UpdateBootstrapStage(args->payload_path, 30);

    UpdateBootstrapStage(args->payload_path, 40);
    CallTlsCallbacks(args->image_base, nt, DLL_PROCESS_ATTACH);

    auto* op = reinterpret_cast<IMAGE_OPTIONAL_HEADER64*>(&nt->OptionalHeader);
    auto* data_dirs = op->DataDirectory;
    auto& exception_dir = data_dirs[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    (void)exception_dir;

    UpdateBootstrapStage(args->payload_path, 50);
    if (nt->OptionalHeader.AddressOfEntryPoint) {
        auto entry = reinterpret_cast<void(*)()>(base + nt->OptionalHeader.AddressOfEntryPoint);
        // The entry point is the DllMain for DLLs (when subsystem includes IMAGE_SUBSYSTEM_WINDOWS_GUI).
        // DllMain is declared as:
        //   BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID)
        using DllMainFn = BOOL (WINAPI*)(HINSTANCE, DWORD, LPVOID);
        auto dllmain = reinterpret_cast<DllMainFn>(entry);
        dllmain(reinterpret_cast<HINSTANCE>(args->image_base), DLL_PROCESS_ATTACH, nullptr);
        UpdateBootstrapStage(args->payload_path, 60);
    }

    return 0;
}

} // namespace

extern "C" __declspec(dllexport) __declspec(safebuffers) unsigned long __cdecl Bootstrap(
    LPVOID image_base, LPVOID load_library_w, LPVOID get_proc_address, LPVOID payload_path) {
    BootstrapArgs args{};
    args.image_base = image_base;
    args.load_library_w = reinterpret_cast<LdrLoadDllFn>(load_library_w);
    args.get_proc_address = reinterpret_cast<LdrGetProcedureAddressFn>(get_proc_address);
    args.payload_path = payload_path;
    return RunBootstrap(&args);
}

#endif // WF_MAXON_DLL
