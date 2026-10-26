// ============================================================================
// WF_MAXON — live in-game engine
// ============================================================================
//
// Reads the user's `WarframeHelper.ini` and applies enabled toggles only after
// the current-build feature offsets have been independently verified. The
// current build has a code-confirmed GameRules -> HumanPlayer -> Avatar
// structural chain and a unique candidate in one populated snapshot, but the
// candidate is dynamic and the historical field table is intentionally not
// writable until each field is remapped against this executable.
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wfmaxon {

void WriteAbsoluteJump(unsigned char* destination, uintptr_t target);

bool InstallFireTelemetryHook();
bool InstallWeaponAmountTelemetryHook();
bool InstallDamageTelemetryHook();
bool InstallDamageContextTelemetryHook();
bool InstallFovOverrideHook(uintptr_t module_base = 0);
bool SetFovOverride(bool enabled, float degrees, uintptr_t module_base = 0);

struct LiveFeature {
    const wchar_t* id;
    const wchar_t* ini_key;
    int   field_offset;   // historical +field_offset from resolved avatar base
    float write_value;     // value written when enabled
    float restore_value;   // value restored when disabled
    bool  needs_max;       // true: read max from +fMaxHealth / +fMaxShields
    int   max_offset;      // historical max field offset when needs_max is true
    bool  enabled;
};

struct LiveNumeric {
    const wchar_t* id;
    const wchar_t* ini_key;
    int field_offset;
    int min;
    int max;
    int value;
    int step;
};

class LiveEngine {
public:
    LiveEngine();
    bool DetectGame();
    void SetGame(const std::wstring& game_dir);

    bool LoadProfile(const std::wstring& ini_path);
    bool ApplyAll();   // writes all enabled fields into the running game
    bool ApplyOne(const LiveFeature& feature);   // single feature
    bool ApplyNumeric(const LiveNumeric& numeric);
    void DisableAll();
    const std::wstring& GameDir() const { return game_dir_; }
    const std::wstring& IniPath() const { return ini_path_; }
    bool ProcessRunning() const { return warframe_pid_ != 0; }
    DWORD Warpid() const { return warframe_pid_; }
    uintptr_t Warbase() const { return warframe_base_; }
    uintptr_t PlayerBase() const { return player_base_; }
    std::wstring Status() const { return status_; }

    const std::vector<LiveFeature>& Features() const { return features_; }
    const std::vector<LiveNumeric>& Numerics() const { return numerics_; }

    // Raw memory access for the script engine's wf.read_* / wf.write_* bindings.
    bool ReadBytes(uintptr_t addr, void* out, size_t n);
    bool WriteBytes(uintptr_t addr, const void* in, size_t n);

private:
    bool ResolvePlayerBase();

    DWORD warframe_pid_ = 0;
    uintptr_t warframe_base_ = 0;
    uintptr_t warframe_size_ = 0;
    // Kept for the existing UI/API name; this is the resolved Avatar address,
    // not the obsolete executable-relative player slot.
    uintptr_t player_base_ = 0;
    bool feature_offsets_verified_ = false;
    std::wstring game_dir_;
    std::wstring ini_path_;
    std::wstring status_;

    std::vector<LiveFeature> features_;
    std::vector<LiveNumeric> numerics_;
};

} // namespace wfmaxon