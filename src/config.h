#pragma once

#include <windows.h>
#include <string>
#include <vector>

// Exported from WF_MAXON.dll when built as a payload. The reflective loader
// calls this from a remote thread after copying the image into the target.
//
// Signature: DWORD Bootstrap(LPVOID image_base, LPVOID load_library_w,
//                            LPVOID get_proc_address, LPVOID payload_path);
//
// The implementation must:
//   1. Apply base relocations using `image_base` as the preferred image base.
//   2. Walk the import table using `load_library_w` and `get_proc_address`.
//   3. Call TLS callbacks (DLL_PROCESS_ATTACH).
//   4. Invoke the payload's DllMain(DLL_PROCESS_ATTACH, NULL).
//   5. Return 0 on success, non-zero on failure.
#ifdef WF_MAXON_DLL
extern "C" __declspec(dllexport) unsigned long __cdecl Bootstrap(
    LPVOID image_base, LPVOID load_library_w, LPVOID get_proc_address, LPVOID payload_path);
#endif

namespace wfmaxon {

struct FeatureBinding {
    const wchar_t* id;
    const wchar_t* title;
    const wchar_t* description;
    const wchar_t* ini_key;
    int category;
    bool enabled;
    bool persisted;
    const wchar_t* alias_key;
    int exclusive_group;
};

struct NumericBinding {
    const wchar_t* id;
    const wchar_t* title;
    const wchar_t* description;
    const wchar_t* ini_key;
    int category;
    int value;
    int minimum;
    int maximum;
    int step;
};

class ConfigStore {
public:
    bool DetectGameDirectory();
    void SetGameDirectory(const std::wstring& path);
    const std::wstring& GameDirectory() const { return game_dir_; }
    const std::wstring& IniPath() const { return ini_path_; }
    bool Exists() const;

    bool ReadBool(const wchar_t* section, const wchar_t* key, bool fallback) const;
    int ReadInt(const wchar_t* section, const wchar_t* key, int fallback) const;
    bool WriteBool(const wchar_t* section, const wchar_t* key, bool value) const;
    bool WriteInt(const wchar_t* section, const wchar_t* key, int value) const;
    bool Flush() const;

    bool Load(std::vector<FeatureBinding>& features,
              std::vector<NumericBinding>& numerics) const;
    bool Save(const std::vector<FeatureBinding>& features,
              const std::vector<NumericBinding>& numerics,
              bool mirror_rage_profile) const;

private:
    std::wstring game_dir_;
    std::wstring ini_path_;
};

std::wstring ExecutableDirectory();
bool FileExists(const std::wstring& path);
bool DirectoryExists(const std::wstring& path);
bool CopyRuntimeToGame(const std::wstring& source_dir,
                       const std::wstring& game_dir,
                       std::wstring* error);

} // namespace wfmaxon
