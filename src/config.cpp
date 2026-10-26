#define WIN32_LEAN_AND_MEAN
#include "config.h"

#include <shlwapi.h>
#include <algorithm>
#include <cwchar>

#pragma comment(lib, "shlwapi.lib")

namespace wfmaxon {
namespace {
constexpr wchar_t kProfile[] = L"enable_Cheats";
constexpr wchar_t kRageProfile[] = L"rage_Cheats";

std::wstring Join(const std::wstring& left, const wchar_t* right) {
    if (left.empty()) return right ? std::wstring(right) : std::wstring();
    std::wstring out = left;
    if (out.back() != L'\\' && out.back() != L'/') out.push_back(L'\\');
    if (right) out += right;
    return out;
}

bool CopyOne(const std::wstring& source, const std::wstring& target, std::wstring* error) {
    if (!FileExists(source)) {
        if (error) *error = L"Missing runtime file: " + source;
        return false;
    }
    if (!CopyFileW(source.c_str(), target.c_str(), FALSE)) {
        if (error) {
            wchar_t buf[128]{};
            swprintf_s(buf, L"Copy failed (%lu): ", GetLastError());
            *error = std::wstring(buf) + source + L" -> " + target;
        }
        return false;
    }
    return true;
}
} // namespace

std::wstring ExecutableDirectory() {
    wchar_t path[MAX_PATH * 4]{};
    DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (!length || length >= std::size(path)) return L".";
    PathRemoveFileSpecW(path);
    return path;
}

bool FileExists(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirectoryExists(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

void ConfigStore::SetGameDirectory(const std::wstring& path) {
    game_dir_ = path;
    while (!game_dir_.empty() && (game_dir_.back() == L'\\' || game_dir_.back() == L'/')) {
        game_dir_.pop_back();
    }
    ini_path_ = Join(game_dir_, L"WarframeHelper.ini");
}

bool ConfigStore::DetectGameDirectory() {
    wchar_t env[MAX_PATH * 4]{};
    DWORD env_len = GetEnvironmentVariableW(L"WF_MAXON_GAME_DIR", env, static_cast<DWORD>(std::size(env)));
    if (env_len && env_len < std::size(env) && DirectoryExists(env)) {
        SetGameDirectory(env);
        return true;
    }

    const wchar_t* candidates[] = {
        L"D:\\SteamLibrary\\steamapps\\common\\Warframe",
        L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Warframe",
        L"C:\\Program Files\\Steam\\steamapps\\common\\Warframe",
        L"D:\\Games\\Warframe"
    };
    for (const wchar_t* candidate : candidates) {
        if (DirectoryExists(candidate)) {
            SetGameDirectory(candidate);
            return true;
        }
    }

    std::wstring exe_dir = ExecutableDirectory();
    if (FileExists(Join(exe_dir, L"Warframe.x64.exe"))) {
        SetGameDirectory(exe_dir);
        return true;
    }
    return false;
}

bool ConfigStore::Exists() const {
    return !ini_path_.empty() && FileExists(ini_path_);
}

bool ConfigStore::ReadBool(const wchar_t* section, const wchar_t* key, bool fallback) const {
    wchar_t value[32]{};
    GetPrivateProfileStringW(section, key, fallback ? L"true" : L"false",
                             value, static_cast<DWORD>(std::size(value)), ini_path_.c_str());
    std::wstring normalized(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::towlower);
    return normalized == L"true" || normalized == L"1" || normalized == L"yes" || normalized == L"on";
}

int ConfigStore::ReadInt(const wchar_t* section, const wchar_t* key, int fallback) const {
    return static_cast<int>(GetPrivateProfileIntW(section, key, fallback, ini_path_.c_str()));
}

bool ConfigStore::WriteBool(const wchar_t* section, const wchar_t* key, bool value) const {
    return WritePrivateProfileStringW(section, key, value ? L"true" : L"false", ini_path_.c_str()) != FALSE;
}

bool ConfigStore::WriteInt(const wchar_t* section, const wchar_t* key, int value) const {
    wchar_t text[32]{};
    swprintf_s(text, L"%d", value);
    return WritePrivateProfileStringW(section, key, text, ini_path_.c_str()) != FALSE;
}

bool ConfigStore::Flush() const {
    return WritePrivateProfileStringW(nullptr, nullptr, nullptr, ini_path_.c_str()) != FALSE;
}

bool ConfigStore::Load(std::vector<FeatureBinding>& features,
                       std::vector<NumericBinding>& numerics) const {
    if (ini_path_.empty()) return false;
    for (auto& feature : features) {
        feature.enabled = ReadBool(kProfile, feature.ini_key, feature.enabled);
    }
    for (auto& numeric : numerics) {
        numeric.value = std::clamp(ReadInt(kProfile, numeric.ini_key, numeric.value),
                                   numeric.minimum, numeric.maximum);
    }
    return true;
}

bool ConfigStore::Save(const std::vector<FeatureBinding>& features,
                       const std::vector<NumericBinding>& numerics,
                       bool mirror_rage_profile) const {
    if (ini_path_.empty()) return false;
    bool ok = true;
    for (const auto& feature : features) {
        if (!feature.persisted) continue;
        ok = WriteBool(kProfile, feature.ini_key, feature.enabled) && ok;
        if (feature.alias_key && *feature.alias_key) {
            ok = WriteBool(kProfile, feature.alias_key, feature.enabled) && ok;
        }
        if (mirror_rage_profile) {
            ok = WriteBool(kRageProfile, feature.ini_key, feature.enabled) && ok;
            if (feature.alias_key && *feature.alias_key) {
                ok = WriteBool(kRageProfile, feature.alias_key, feature.enabled) && ok;
            }
        }
    }
    for (const auto& numeric : numerics) {
        ok = WriteInt(kProfile, numeric.ini_key, numeric.value) && ok;
        if (mirror_rage_profile) {
            ok = WriteInt(kRageProfile, numeric.ini_key, numeric.value) && ok;
        }
    }
    Flush();
    return ok;
}

bool CopyRuntimeToGame(const std::wstring& source_dir,
                       const std::wstring& game_dir,
                       std::wstring* error) {
    if (!DirectoryExists(game_dir)) {
        if (error) *error = L"Warframe directory not found: " + game_dir;
        return false;
    }
    const std::wstring src_loader = Join(source_dir, L"WF_MAXONLoader.exe");
    const std::wstring src_dll = Join(source_dir, L"WF_MAXON.dll");
    const std::wstring dst_loader = Join(game_dir, L"WF_MAXONLoader.exe");
    const std::wstring dst_dll = Join(game_dir, L"WF_MAXON.dll");
    if (!CopyOne(src_loader, dst_loader, error)) return false;
    if (!CopyOne(src_dll, dst_dll, error)) return false;
    return true;
}

} // namespace wfmaxon
