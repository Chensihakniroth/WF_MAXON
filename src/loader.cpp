// ============================================================================
// WF_MAXON — suspended replacement/thread-hijack loader
// ============================================================================
//
// This loader follows OwHelperLoader.exe's replacement flow:
//
//   1. Read launch parameters from the launcher-created Warframe process.
//   2. Create a suspended replacement with the same image/command line/env.
//   3. Manually map WF_MAXON.dll into the replacement.
//   4. Hijack its suspended primary thread with Get/SetThreadContext.
//   5. Resume the replacement, verify its payload window, then terminate the
//      original Warframe process.
//
// No CreateRemoteThread is used. The payload uses native Ldr import resolution,
// restores the original context with NtContinue, and remains fail-closed for
// gameplay behavior.
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <winternl.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cwctype>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "shlwapi.lib")

namespace {

constexpr wchar_t kTargetProcess[] = L"Warframe.x64.exe";
constexpr wchar_t kPayloadDll[] = L"WF_MAXON.dll";
constexpr wchar_t kPayloadWindowClass[] = L"WF_MAXON_MainWindow";
constexpr DWORD kCreateSuspended = 0x00000004;
constexpr DWORD kCreateUnicodeEnvironment = 0x00000400;
constexpr DWORD kProcessQueryRead = PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ;

struct LaunchInfo {
    std::wstring image_path;
    std::wstring command_line;
    std::wstring current_directory;
    std::vector<wchar_t> environment;
};

struct RemoteThreadData {
    CONTEXT original_context;
    DWORD bootstrap_status;
    DWORD magic;
    DWORD stage;
    DWORD fault_code;
    uintptr_t fault_address;
    uintptr_t fault_rip;
};

std::wstring g_log_path;

void Log(const char* format, ...) {
    if (g_log_path.empty()) return;
    char line[1024]{};
    va_list args;
    va_start(args, format);
    vsnprintf_s(line, sizeof(line), _TRUNCATE, format, args);
    va_end(args);
    FILE* file = nullptr;
    if (_wfopen_s(&file, g_log_path.c_str(), L"ab") != 0 || !file) return;
    std::fwrite(line, 1, std::strlen(line), file);
    std::fwrite("\r\n", 1, 2, file);
    std::fclose(file);
}

void LogLastError(const char* step) {
    Log("%s failed: winerr=%lu", step, GetLastError());
}

DWORD FindTargetPid(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD pid = 0;
    if (Process32FirstW(snap, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, name) == 0) {
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return pid;
}

bool EnableDebugPrivilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }
    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &luid)) {
        CloseHandle(token);
        return false;
    }
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Luid = luid;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    BOOL ok = AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr);
    DWORD error = GetLastError();
    CloseHandle(token);
    return ok && error == ERROR_SUCCESS;
}

std::wstring ExecutableDirectory() {
    wchar_t path[MAX_PATH * 4]{};
    DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (!length || length >= std::size(path)) return L".";
    PathRemoveFileSpecW(path);
    return path;
}

bool ReadFileAll(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        static_cast<unsigned long long>(size.QuadPart) > SIZE_MAX) {
        CloseHandle(file);
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    BOOL ok = ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
    CloseHandle(file);
    return ok && read == out.size();
}

bool RvaToFileOffset(const std::vector<uint8_t>& image,
                     const IMAGE_NT_HEADERS64* nt,
                     DWORD rva,
                     size_t& offset) {
    if (rva < nt->OptionalHeader.SizeOfHeaders) {
        if (static_cast<size_t>(rva) < image.size()) {
            offset = rva;
            return true;
        }
        return false;
    }
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        const DWORD span = section->Misc.VirtualSize > section->SizeOfRawData
            ? section->Misc.VirtualSize
            : section->SizeOfRawData;
        if (rva < section->VirtualAddress ||
            rva - section->VirtualAddress >= span) {
            continue;
        }
        const uint64_t file_offset = static_cast<uint64_t>(section->PointerToRawData) +
                                     (rva - section->VirtualAddress);
        if (file_offset >= image.size()) return false;
        offset = static_cast<size_t>(file_offset);
        return true;
    }
    return false;
}

DWORD GetExportRva(const std::vector<uint8_t>& image,
                   const IMAGE_NT_HEADERS64* nt,
                   const char* name) {
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!directory.VirtualAddress || !directory.Size || !name) return 0;
    size_t export_offset = 0;
    if (!RvaToFileOffset(image, nt, directory.VirtualAddress, export_offset) ||
        export_offset + sizeof(IMAGE_EXPORT_DIRECTORY) > image.size()) {
        return 0;
    }
    const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(image.data() + export_offset);
    if (!exports->NumberOfNames || !exports->NumberOfFunctions) return 0;

    size_t names_offset = 0;
    size_t functions_offset = 0;
    size_t ordinals_offset = 0;
    if (!RvaToFileOffset(image, nt, exports->AddressOfNames, names_offset) ||
        !RvaToFileOffset(image, nt, exports->AddressOfFunctions, functions_offset) ||
        !RvaToFileOffset(image, nt, exports->AddressOfNameOrdinals, ordinals_offset)) {
        return 0;
    }
    const auto* names = reinterpret_cast<const DWORD*>(image.data() + names_offset);
    const auto* functions = reinterpret_cast<const DWORD*>(image.data() + functions_offset);
    const auto* ordinals = reinterpret_cast<const WORD*>(image.data() + ordinals_offset);
    for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
        size_t name_offset = 0;
        if (!RvaToFileOffset(image, nt, names[i], name_offset)) continue;
        const char* export_name = reinterpret_cast<const char*>(image.data() + name_offset);
        if (std::strcmp(export_name, name) == 0 && ordinals[i] < exports->NumberOfFunctions) {
            return functions[ordinals[i]];
        }
    }
    return 0;
}

HMODULE GetRemoteModuleBase(DWORD pid, const wchar_t* module_name) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return nullptr;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    HMODULE result = nullptr;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szModule, module_name) == 0) {
                result = entry.hModule;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

HANDLE OpenEarliestThread(DWORD pid, DWORD* thread_id_out) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return nullptr;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    HANDLE best = nullptr;
    DWORD best_id = 0;
    ULARGE_INTEGER best_creation{};
    bool have_time = false;
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID != pid) continue;
            HANDLE candidate = OpenThread(
                THREAD_QUERY_INFORMATION | THREAD_SUSPEND_RESUME |
                    THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                FALSE, entry.th32ThreadID);
            if (!candidate) continue;
            FILETIME creation{}, exit_time{}, kernel_time{}, user_time{};
            const bool has_time = GetThreadTimes(candidate, &creation, &exit_time,
                                                 &kernel_time, &user_time) != FALSE;
            ULARGE_INTEGER when{};
            when.LowPart = creation.dwLowDateTime;
            when.HighPart = creation.dwHighDateTime;
            if (!best || (has_time && (!have_time || when.QuadPart < best_creation.QuadPart))) {
                if (best) CloseHandle(best);
                best = candidate;
                best_id = entry.th32ThreadID;
                best_creation = when;
                have_time = has_time;
            } else {
                CloseHandle(candidate);
            }
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    if (thread_id_out) *thread_id_out = best_id;
    return best;
}

FARPROC ResolveRemoteExport(HMODULE local_module, HMODULE remote_module, const char* name) {
    if (!local_module || !remote_module) return nullptr;
    const uintptr_t local_base = reinterpret_cast<uintptr_t>(local_module);
    const uintptr_t local_export = reinterpret_cast<uintptr_t>(GetProcAddress(local_module, name));
    if (!local_export || local_export < local_base) return nullptr;
    return reinterpret_cast<FARPROC>(reinterpret_cast<uintptr_t>(remote_module) +
                                     (local_export - local_base));
}

void StripRelaunchArgument(std::wstring& command_line) {
    std::wstring lower = command_line;
    for (wchar_t& ch : lower) ch = static_cast<wchar_t>(std::towlower(ch));
    size_t token = lower.find(L"-relaunch:");
    if (token == std::wstring::npos) token = lower.find(L"-relaunch");
    if (token == std::wstring::npos) return;
    size_t start = token;
    while (start > 0 && std::iswspace(command_line[start - 1])) --start;
    size_t end = token;
    while (end < command_line.size() && !std::iswspace(command_line[end])) ++end;
    command_line.erase(start, end - start);
}

bool QueryLaunchInfo(DWORD pid, LaunchInfo& out) {
    HANDLE process = OpenProcess(kProcessQueryRead, FALSE, pid);
    if (!process) return false;

    wchar_t image_path[MAX_PATH * 4]{};
    DWORD image_length = static_cast<DWORD>(std::size(image_path));
    if (!QueryFullProcessImageNameW(process, 0, image_path, &image_length)) {
        CloseHandle(process);
        return false;
    }
    out.image_path.assign(image_path, image_length);
    const size_t slash = out.image_path.find_last_of(L"\\/");
    out.current_directory = slash == std::wstring::npos
        ? L"."
        : out.image_path.substr(0, slash);

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    using NtQueryInformationProcessFn = NTSTATUS (NTAPI*)(
        HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
    auto query = reinterpret_cast<NtQueryInformationProcessFn>(
        ntdll ? GetProcAddress(ntdll, "NtQueryInformationProcess") : nullptr);
    if (!query) {
        CloseHandle(process);
        return false;
    }

    PROCESS_BASIC_INFORMATION basic{};
    ULONG returned = 0;
    if (query(process, ProcessBasicInformation, &basic, sizeof(basic), &returned) < 0 ||
        !basic.PebBaseAddress) {
        CloseHandle(process);
        return false;
    }

    uintptr_t process_parameters = 0;
    SIZE_T got = 0;
    if (!ReadProcessMemory(process,
                           static_cast<const char*>(static_cast<const void*>(basic.PebBaseAddress)) + 0x20,
                           &process_parameters, sizeof(process_parameters), &got) ||
        got != sizeof(process_parameters)) {
        CloseHandle(process);
        return false;
    }

    struct RemoteUnicodeString {
        USHORT length;
        USHORT maximum_length;
        ULONG padding;
        uintptr_t buffer;
    } command_line{};
    if (!ReadProcessMemory(process,
                           reinterpret_cast<const char*>(process_parameters) + 0x70,
                           &command_line, sizeof(command_line), &got) ||
        got != sizeof(command_line) || !command_line.buffer ||
        command_line.length > 0x10000) {
        CloseHandle(process);
        return false;
    }

    std::vector<wchar_t> command(command_line.length / sizeof(wchar_t) + 1, L'\0');
    if (command_line.length &&
        (!ReadProcessMemory(process, reinterpret_cast<const void*>(command_line.buffer),
                            command.data(), command_line.length, &got) ||
         got != command_line.length)) {
        CloseHandle(process);
        return false;
    }
    out.command_line.assign(command.data(), command_line.length / sizeof(wchar_t));
    if (out.command_line.empty()) {
        out.command_line = L"\"" + out.image_path + L"\"";
    }
    const std::wstring original_command_line = out.command_line;
    StripRelaunchArgument(out.command_line);
    if (out.command_line != original_command_line) {
        Log("removed one-shot -relaunch argument");
    }

    uintptr_t environment = 0;
    SIZE_T environment_size = 0;
    ReadProcessMemory(process, reinterpret_cast<const char*>(process_parameters) + 0x80,
                      &environment, sizeof(environment), &got);
    ReadProcessMemory(process, reinterpret_cast<const char*>(process_parameters) + 0x3F0,
                      &environment_size, sizeof(environment_size), &got);
    if (environment && environment_size && environment_size <= 0x100000 &&
        (environment_size % sizeof(wchar_t)) == 0) {
        out.environment.resize(environment_size / sizeof(wchar_t) + 2, L'\0');
        if (!ReadProcessMemory(process, reinterpret_cast<const void*>(environment),
                               out.environment.data(), environment_size, &got) ||
            got != environment_size) {
            out.environment.clear();
        } else {
            out.environment[environment_size / sizeof(wchar_t)] = L'\0';
            out.environment[environment_size / sizeof(wchar_t) + 1] = L'\0';
        }
    }
    CloseHandle(process);
    return true;
}

bool CreateSuspendedClone(const LaunchInfo& launch, PROCESS_INFORMATION& pi) {
    std::vector<wchar_t> command(launch.command_line.begin(), launch.command_line.end());
    command.push_back(L'\0');
    LPVOID environment = launch.environment.empty()
        ? nullptr
        : static_cast<LPVOID>(const_cast<wchar_t*>(launch.environment.data()));
    DWORD flags = kCreateSuspended;
    if (environment) flags |= kCreateUnicodeEnvironment;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    std::memset(&pi, 0, sizeof(pi));
    return CreateProcessW(launch.image_path.c_str(), command.data(), nullptr, nullptr, FALSE,
                          flags, environment, launch.current_directory.c_str(), &startup, &pi) != FALSE;
}

bool MapImage(HANDLE process, const std::vector<uint8_t>& image,
              const IMAGE_NT_HEADERS64* nt, LPVOID& remote_base) {
    remote_base = VirtualAllocEx(process, nullptr, nt->OptionalHeader.SizeOfImage,
                                 MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!remote_base) return false;
    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remote_base, image.data(), nt->OptionalHeader.SizeOfHeaders,
                             &written) || written != nt->OptionalHeader.SizeOfHeaders) {
        VirtualFreeEx(process, remote_base, 0, MEM_RELEASE);
        remote_base = nullptr;
        return false;
    }
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (!section->SizeOfRawData) continue;
        LPVOID destination = reinterpret_cast<LPVOID>(reinterpret_cast<uintptr_t>(remote_base) +
                                                       section->VirtualAddress);
        if (!WriteProcessMemory(process, destination,
                                 image.data() + section->PointerToRawData,
                                 section->SizeOfRawData, &written) ||
            written != section->SizeOfRawData) {
            VirtualFreeEx(process, remote_base, 0, MEM_RELEASE);
            remote_base = nullptr;
            return false;
        }
    }
    return true;
}

void Put8(std::vector<uint8_t>& code, uint8_t value) { code.push_back(value); }
void Put32(std::vector<uint8_t>& code, uint32_t value) {
    const size_t offset = code.size();
    code.resize(offset + sizeof(value));
    std::memcpy(code.data() + offset, &value, sizeof(value));
}
void Put64(std::vector<uint8_t>& code, uint64_t value) {
    const size_t offset = code.size();
    code.resize(offset + sizeof(value));
    std::memcpy(code.data() + offset, &value, sizeof(value));
}
void MovRax(std::vector<uint8_t>& code, uint64_t value) {
    Put8(code, 0x48); Put8(code, 0xB8); Put64(code, value);
}
void MovRcx(std::vector<uint8_t>& code, uint64_t value) {
    Put8(code, 0x48); Put8(code, 0xB9); Put64(code, value);
}
void MovRdx(std::vector<uint8_t>& code, uint64_t value) {
    Put8(code, 0x48); Put8(code, 0xBA); Put64(code, value);
}
void MovR8(std::vector<uint8_t>& code, uint64_t value) {
    Put8(code, 0x49); Put8(code, 0xB8); Put64(code, value);
}
void MovR9(std::vector<uint8_t>& code, uint64_t value) {
    Put8(code, 0x49); Put8(code, 0xB9); Put64(code, value);
}
void MovR10(std::vector<uint8_t>& code, uint64_t value) {
    Put8(code, 0x49); Put8(code, 0xBA); Put64(code, value);
}

std::vector<uint8_t> BuildExceptionHandler(uintptr_t remote_data) {
    std::vector<uint8_t> handler;
    Put8(handler, 0x48); Put8(handler, 0x8B); Put8(handler, 0x01); // rax = ExceptionRecord
    Put8(handler, 0x8B); Put8(handler, 0x10);                   // edx = ExceptionCode
    Put8(handler, 0x4C); Put8(handler, 0x8B); Put8(handler, 0x40); Put8(handler, 0x10); // r8 = ExceptionAddress
    Put8(handler, 0x48); Put8(handler, 0x8B); Put8(handler, 0x49); Put8(handler, 0x08); // rcx = ContextRecord
    Put8(handler, 0x4C); Put8(handler, 0x8B); Put8(handler, 0x89); Put32(handler, 0xF8); // r9 = Context.Rip
    MovR10(handler, remote_data);
    Put8(handler, 0x41); Put8(handler, 0x89); Put8(handler, 0x92);
    Put32(handler, static_cast<uint32_t>(offsetof(RemoteThreadData, fault_code)));
    Put8(handler, 0x4D); Put8(handler, 0x89); Put8(handler, 0x82);
    Put32(handler, static_cast<uint32_t>(offsetof(RemoteThreadData, fault_address)));
    Put8(handler, 0x4D); Put8(handler, 0x89); Put8(handler, 0x8A);
    Put32(handler, static_cast<uint32_t>(offsetof(RemoteThreadData, fault_rip)));
    Put8(handler, 0x31); Put8(handler, 0xC0); // EXCEPTION_CONTINUE_SEARCH
    Put8(handler, 0xC3);
    return handler;
}

bool HijackPrimaryThread(HANDLE process, HANDLE thread, LPVOID image_base,
                        FARPROC load_library_w, FARPROC get_proc_address,
                        FARPROC bootstrap, FARPROC nt_continue, FARPROC veh_add,
                        DWORD& bootstrap_status) {
    Log("hijack begin process=%p thread=%p image=%p bootstrap=%p ntcontinue=%p",
        process, thread, image_base, bootstrap, nt_continue);
    if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
        LogLastError("SuspendThread");
        return false;
    }

    CONTEXT original{};
    original.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &original)) {
        LogLastError("GetThreadContext");
        return false;
    }
    Log("original context rip=0x%llX rsp=0x%llX flags=0x%lX",
        static_cast<unsigned long long>(original.Rip),
        static_cast<unsigned long long>(original.Rsp),
        static_cast<unsigned long>(original.ContextFlags));

    RemoteThreadData data{};
    data.original_context = original;
    data.bootstrap_status = 0xFFFFFFFF;
    data.magic = 0x57464D58;
    data.stage = 0;

    LPVOID remote_data = VirtualAllocEx(process, nullptr, 0x1000,
                                        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    LPVOID remote_code = VirtualAllocEx(process, nullptr, 0x1000,
                                        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    Log("alloc data=%p code=%p last_error=%lu",
        remote_data, remote_code, GetLastError());
    if (!remote_data || !remote_code) return false;

    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remote_data, &data, sizeof(data), &written) ||
        written != sizeof(data)) {
        Log("WriteProcessMemory(data) failed winerr=%lu written=%llu expected=%zu",
            GetLastError(), static_cast<unsigned long long>(written), sizeof(data));
        return false;
    }

    const uintptr_t status_address = reinterpret_cast<uintptr_t>(remote_data) + offsetof(RemoteThreadData, bootstrap_status);
    const uintptr_t stage_address = reinterpret_cast<uintptr_t>(remote_data) + offsetof(RemoteThreadData, stage);
    const uintptr_t context_address = reinterpret_cast<uintptr_t>(remote_data) + offsetof(RemoteThreadData, original_context);
    // Keep the API calls on the thread's real TEB-backed stack. A separately
    // allocated heap page can fault inside LdrLoadDll/Rtl routines because it
    // is outside the thread stack limits.
    const uintptr_t stack_top = static_cast<uintptr_t>(original.Rsp);
    const uintptr_t handler_address = reinterpret_cast<uintptr_t>(remote_code) + 0x200u;
    const std::vector<uint8_t> exception_handler = BuildExceptionHandler(reinterpret_cast<uintptr_t>(remote_data));
    if (!WriteProcessMemory(process, reinterpret_cast<LPVOID>(handler_address),
                             exception_handler.data(), exception_handler.size(), &written) ||
        written != exception_handler.size()) {
        Log("WriteProcessMemory(exception_handler) failed winerr=%lu", GetLastError());
        return false;
    }
    std::vector<uint8_t> stub;
    MovRax(stub, stack_top);
    Put8(stub, 0x48); Put8(stub, 0x89); Put8(stub, 0xC4);
    Put8(stub, 0x48); Put8(stub, 0x83); Put8(stub, 0xEC); Put8(stub, 0x28);
    // Install a temporary VEH before entering the payload so an AV records
    // its exact address in RemoteThreadData.
    MovRcx(stub, 1);
    MovRdx(stub, handler_address);
    MovRax(stub, reinterpret_cast<uintptr_t>(veh_add));
    Put8(stub, 0xFF); Put8(stub, 0xD0);
    MovR10(stub, stage_address);
    Put8(stub, 0x41); Put8(stub, 0xC7); Put8(stub, 0x02);
    Put8(stub, 0x01); Put8(stub, 0x00); Put8(stub, 0x00); Put8(stub, 0x00);
    MovRcx(stub, reinterpret_cast<uintptr_t>(image_base));
    MovRdx(stub, reinterpret_cast<uintptr_t>(load_library_w));
    MovR8(stub, reinterpret_cast<uintptr_t>(get_proc_address));
    MovR9(stub, stage_address);
    MovRax(stub, reinterpret_cast<uintptr_t>(bootstrap));
    Put8(stub, 0xFF); Put8(stub, 0xD0);
    MovR10(stub, stage_address);
    Put8(stub, 0x41); Put8(stub, 0xC7); Put8(stub, 0x02);
    Put8(stub, 0x02); Put8(stub, 0x00); Put8(stub, 0x00); Put8(stub, 0x00);
    MovR10(stub, status_address);
    Put8(stub, 0x41); Put8(stub, 0x89); Put8(stub, 0x02);
    MovRcx(stub, context_address);
    Put8(stub, 0x31); Put8(stub, 0xD2);
    MovRax(stub, reinterpret_cast<uintptr_t>(nt_continue));
    Put8(stub, 0xFF); Put8(stub, 0xD0);
    Put8(stub, 0xCC);

    if (!WriteProcessMemory(process, remote_code, stub.data(), stub.size(), &written) ||
        written != stub.size()) {
        Log("WriteProcessMemory(stub) failed winerr=%lu written=%llu expected=%zu",
            GetLastError(), static_cast<unsigned long long>(written), stub.size());
        return false;
    }

    CONTEXT hijack = original;
    hijack.Rip = reinterpret_cast<DWORD64>(remote_code);
    hijack.ContextFlags = CONTEXT_FULL;
    if (!SetThreadContext(thread, &hijack)) {
        LogLastError("SetThreadContext");
        return false;
    }
    const DWORD resume_one = ResumeThread(thread);
    const DWORD resume_two = ResumeThread(thread);
    Log("context set rip=%p resume_one=%lu resume_two=%lu",
        remote_code, resume_one, resume_two);
    if (resume_one == static_cast<DWORD>(-1) || resume_two == static_cast<DWORD>(-1)) return false;

    const ULONGLONG start = GetTickCount64();
    DWORD status = 0xFFFFFFFF;
    DWORD stage = 0;
    while (GetTickCount64() - start < 15000) {
        DWORD process_exit = STILL_ACTIVE;
        if (GetExitCodeProcess(process, &process_exit) && process_exit != STILL_ACTIVE) {
            DWORD fault_code = 0;
            uintptr_t fault_address = 0;
            uintptr_t fault_rip = 0;
            SIZE_T fault_read = 0;
            ReadProcessMemory(process,
                              reinterpret_cast<const char*>(remote_data) + offsetof(RemoteThreadData, fault_code),
                              &fault_code, sizeof(fault_code), &fault_read);
            ReadProcessMemory(process,
                              reinterpret_cast<const char*>(remote_data) + offsetof(RemoteThreadData, fault_address),
                              &fault_address, sizeof(fault_address), &fault_read);
            ReadProcessMemory(process,
                              reinterpret_cast<const char*>(remote_data) + offsetof(RemoteThreadData, fault_rip),
                              &fault_rip, sizeof(fault_rip), &fault_read);
            Log("clone exited during hijack exit=0x%08lX stage=0x%08lX status=0x%08lX fault_code=0x%08lX fault_address=0x%llX fault_rip=0x%llX",
                process_exit, stage, status, fault_code,
                static_cast<unsigned long long>(fault_address),
                static_cast<unsigned long long>(fault_rip));
            return false;
        }
        SIZE_T read = 0;
        ReadProcessMemory(process,
                          reinterpret_cast<const char*>(remote_data) + offsetof(RemoteThreadData, stage),
                          &stage, sizeof(stage), &read);
        if (ReadProcessMemory(process,
                               reinterpret_cast<const char*>(remote_data) + offsetof(RemoteThreadData, bootstrap_status),
                               &status, sizeof(status), &read) && read == sizeof(status) &&
            status != 0xFFFFFFFF) {
            bootstrap_status = status;
            Log("bootstrap status=0x%08lX stage=0x%08lX after %llums", status, stage,
                static_cast<unsigned long long>(GetTickCount64() - start));
            return status == 0;
        }
        Sleep(25);
    }
    Log("bootstrap status timeout stage=0x%08lX last_status=0x%08lX winerr=%lu",
        stage, status, GetLastError());
    return false;
}

struct WindowSearch {
    DWORD pid;
    HWND window;
};

BOOL CALLBACK FindPayloadWindow(HWND hwnd, LPARAM parameter) {
    auto* search = reinterpret_cast<WindowSearch*>(parameter);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != search->pid) return TRUE;
    wchar_t class_name[128]{};
    GetClassNameW(hwnd, class_name, static_cast<int>(std::size(class_name)));
    if (_wcsicmp(class_name, kPayloadWindowClass) == 0) {
        search->window = hwnd;
        return FALSE;
    }
    return TRUE;
}

bool WaitForPayloadWindow(DWORD pid, DWORD timeout_ms) {
    const ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < timeout_ms) {
        WindowSearch search{pid, nullptr};
        EnumWindows(FindPayloadWindow, reinterpret_cast<LPARAM>(&search));
        if (search.window) return true;
        Sleep(100);
    }
    return false;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const std::wstring folder = ExecutableDirectory();
    g_log_path = folder + L"\\WF_MAXONLoader.log";
    DeleteFileW(g_log_path.c_str());
    Log("loader start");
    if (!EnableDebugPrivilege()) {
        LogLastError("EnableDebugPrivilege");
        return 2;
    }
    const std::wstring payload_path = folder + L"\\" + kPayloadDll;
    Log("payload=%ls", payload_path.c_str());

    std::vector<uint8_t> image;
    if (!ReadFileAll(payload_path, image)) {
        LogLastError("ReadFileAll");
        return 3;
    }
    Log("payload bytes=%zu", image.size());
    if (image.size() < sizeof(IMAGE_DOS_HEADER)) return 4;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 5;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return 6;
    const DWORD bootstrap_rva = GetExportRva(image, nt, "Bootstrap");
    if (!bootstrap_rva) return 7;

    const DWORD source_pid = FindTargetPid(kTargetProcess);
    Log("source_pid=%lu", source_pid);
    if (!source_pid) return 8;
    HANDLE source_process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE,
                                         FALSE, source_pid);
    if (!source_process) {
        LogLastError("OpenProcess(source)");
        return 9;
    }
    HMODULE source_kernel32 = GetRemoteModuleBase(source_pid, L"kernel32.dll");
    HMODULE source_ntdll = GetRemoteModuleBase(source_pid, L"ntdll.dll");
    Log("source modules kernel32=%p ntdll=%p", source_kernel32, source_ntdll);
    LaunchInfo launch{};
    if (!QueryLaunchInfo(source_pid, launch)) {
        LogLastError("QueryLaunchInfo");
        CloseHandle(source_process);
        return 10;
    }
    Log("image=%ls command_line=%ls env_chars=%zu",
        launch.image_path.c_str(), launch.command_line.c_str(), launch.environment.size());

    // Warframe rejects a second live copy. OwHelper terminates the launcher
    // instance before creating the suspended replacement.
    if (!TerminateProcess(source_process, 0)) {
        LogLastError("TerminateProcess(source before respawn)");
        CloseHandle(source_process);
        return 11;
    }
    const DWORD source_wait = WaitForSingleObject(source_process, 60000);
    Log("source terminated before respawn pid=%lu wait=0x%08lX", source_pid, source_wait);
    if (source_wait != WAIT_OBJECT_0) {
        Log("source did not reach terminated state; refusing respawn");
        CloseHandle(source_process);
        return 11;
    }
    CloseHandle(source_process);
    source_process = nullptr;

    PROCESS_INFORMATION pi{};
    if (!CreateSuspendedClone(launch, pi)) {
        LogLastError("CreateSuspendedClone");
        return 12;
    }
    Log("replacement_pid=%lu replacement_thread=%p", pi.dwProcessId, pi.hThread);

    HMODULE local_ntdll = GetModuleHandleW(L"ntdll.dll");
    // OwHelper uses ntdll's native loader exports rather than kernel32
    // LoadLibraryW/GetProcAddress while running from the hijacked thread.
    FARPROC load_library_w = source_ntdll
        ? ResolveRemoteExport(local_ntdll, source_ntdll, "LdrLoadDll")
        : (local_ntdll ? GetProcAddress(local_ntdll, "LdrLoadDll") : nullptr);
    FARPROC get_proc_address = source_ntdll
        ? ResolveRemoteExport(local_ntdll, source_ntdll, "LdrGetProcedureAddress")
        : (local_ntdll ? GetProcAddress(local_ntdll, "LdrGetProcedureAddress") : nullptr);
    FARPROC nt_continue = source_ntdll
        ? ResolveRemoteExport(local_ntdll, source_ntdll, "NtContinue")
        : (local_ntdll ? GetProcAddress(local_ntdll, "NtContinue") : nullptr);
    FARPROC veh_add = source_ntdll
        ? ResolveRemoteExport(local_ntdll, source_ntdll, "RtlAddVectoredExceptionHandler")
        : (local_ntdll ? GetProcAddress(local_ntdll, "RtlAddVectoredExceptionHandler") : nullptr);
    Log("apis loader_ntdll=%p source_ntdll=%p LdrLoadDll=%p LdrGetProcedureAddress=%p NtContinue=%p RtlAddVEH=%p",
        local_ntdll, source_ntdll, load_library_w, get_proc_address, nt_continue, veh_add);
    if (!load_library_w || !get_proc_address || !nt_continue || !veh_add) {
        Log("system API resolution failed");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 12;
    }

    LPVOID remote_base = nullptr;
    if (!MapImage(pi.hProcess, image, nt, remote_base)) {
        LogLastError("MapImage");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 13;
    }
    Log("mapped image=%p size=0x%lX bootstrap_rva=0x%lX", remote_base,
        nt->OptionalHeader.SizeOfImage, bootstrap_rva);
    FARPROC bootstrap = reinterpret_cast<FARPROC>(reinterpret_cast<uintptr_t>(remote_base) + bootstrap_rva);
    DWORD bootstrap_status = 0xFFFFFFFF;
    if (!HijackPrimaryThread(pi.hProcess, pi.hThread, remote_base,
                             load_library_w, get_proc_address, bootstrap,
                             nt_continue, veh_add, bootstrap_status)) {
        Log("HijackPrimaryThread failed bootstrap_status=0x%08lX", bootstrap_status);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 14;
    }
    CloseHandle(pi.hThread);

    if (!WaitForPayloadWindow(pi.dwProcessId, 30000)) {
        Log("payload window timeout replacement_pid=%lu", pi.dwProcessId);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        return 15;
    }

    const ULONGLONG replacement_start = GetTickCount64();
    while (GetTickCount64() - replacement_start < 5000) {
        DWORD replacement_exit = STILL_ACTIVE;
        if (GetExitCodeProcess(pi.hProcess, &replacement_exit) &&
            replacement_exit != STILL_ACTIVE) {
            Log("replacement exited after source kill exit=0x%08lX", replacement_exit);
            break;
        }
        WindowSearch replacement_window{pi.dwProcessId, nullptr};
        EnumWindows(FindPayloadWindow, reinterpret_cast<LPARAM>(&replacement_window));
        Log("replacement alive pid=%lu window=%p", pi.dwProcessId, replacement_window.window);
        Sleep(250);
    }
    Log("success replacement_pid=%lu", pi.dwProcessId);
    CloseHandle(pi.hProcess);
    return 0;
}
