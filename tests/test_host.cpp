#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cwchar>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        ::fwprintf(stderr, L"usage: test_host.exe <payload.dll>\n");
        return 2;
    }

    HMODULE payload = LoadLibraryW(argv[1]);
    if (!payload) {
        ::fwprintf(stderr, L"LoadLibrary failed: %lu\n", GetLastError());
        return 3;
    }

    using GetVersionFn = unsigned int (*)();
    using GetClassFn = const wchar_t* (*)();
    auto get_version = reinterpret_cast<GetVersionFn>(GetProcAddress(payload, "WF_MAXON_GetVersion"));
    auto get_class = reinterpret_cast<GetClassFn>(GetProcAddress(payload, "WF_MAXON_GetWindowClass"));
    FARPROC bootstrap = GetProcAddress(payload, "Bootstrap");
    if (!get_version || !get_class || !bootstrap) {
        ::fwprintf(stderr, L"Expected payload exports missing\n");
        FreeLibrary(payload);
        return 4;
    }

    HWND window = nullptr;
    for (int i = 0; i < 100 && !window; ++i) {
        window = FindWindowW(get_class(), nullptr);
        Sleep(50);
    }
    if (!window) {
        ::fwprintf(stderr, L"WF_MAXON window did not appear\n");
        FreeLibrary(payload);
        return 5;
    }

    wchar_t title[128]{};
    GetWindowTextW(window, title, static_cast<int>(ARRAYSIZE(title)));
    ::wprintf(L"payload=loaded version=0x%08X class=%ls title=%ls hwnd=%p\n",
                 get_version(), get_class(), title, window);

    PostMessageW(window, WM_CLOSE, 0, 0);
    Sleep(250);
    // Do not FreeLibrary: the payload created a detached UI thread. The test host
    // process exits immediately after the window closes, matching manual-map usage.
    return 0;
}
