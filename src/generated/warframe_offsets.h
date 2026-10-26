// ============================================================================
// WF_MAXON current-build offset manifest (AUTO-GENERATED)
// Tool: refresh_offsets.py v1.0.0
// Generated: 2026-10-26T08:39:24
// Binary: D:\SteamLibrary\steamapps\common\Warframe\Warframe.x64.exe
// SHA-256: eec0dbd7078cd6e1f2402fc953ec2741e87a6fb2658ec288be195aac75a1e76d
// PE timestamp: 0x6ada4423
// SizeOfImage: 0x2cd9000
// Do not hand-edit. Re-run WF_MAXON\tools\refresh_offsets.py after a patch.
// An unresolved entry is deliberately fail-closed at runtime.
// ============================================================================
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace wfmaxon_offsets {
inline constexpr std::uintptr_t kExpectedImageSize = 0x2CD9000ULL;
inline constexpr std::uint32_t kExpectedPeTimestamp = 0x6ADA4423U;
inline constexpr char kBinarySha256[] = "eec0dbd7078cd6e1f2402fc953ec2741e87a6fb2658ec288be195aac75a1e76d";
inline constexpr char kBinaryPath[] = "D:/SteamLibrary/steamapps/common/Warframe/Warframe.x64.exe";

// fire_weapon_rmi: resolved_unique (1 hit(s)); Weapon fire telemetry entry. The final object-field displacement is fixed at +0x1F8 to discriminate this entry; the generated header records the exact current prologue.
inline constexpr std::uintptr_t kFireWeaponRmiRva = 0xEA5790ULL;
inline constexpr std::uintptr_t kFireWeaponRmiBranchTargetRva = 0x0ULL;
inline constexpr bool kFireWeaponRmiResolved = true;
inline constexpr std::size_t kFireWeaponRmiPatchLength = 22;
inline constexpr std::array<std::uint8_t, 22> kFireWeaponRmiPrologue = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0x81, 0xF8, 0x00, 0x00, 0x00};

// weapon_amount: resolved_unique (1 hit(s)); WeaponEx amount classifier telemetry entry.
inline constexpr std::uintptr_t kWeaponAmountRva = 0x5E47F0ULL;
inline constexpr std::uintptr_t kWeaponAmountBranchTargetRva = 0x0ULL;
inline constexpr bool kWeaponAmountResolved = true;
inline constexpr std::size_t kWeaponAmountPatchLength = 12;
inline constexpr std::array<std::uint8_t, 12> kWeaponAmountPrologue = {0x8D, 0x42, 0xE8, 0x4C, 0x8B, 0xD1, 0x3C, 0x01, 0x76, 0x3A, 0x8D, 0x42};

// damage_result: resolved_unique (1 hit(s)); Current-build damage result point: the unique clamp sequence is anchored to the RDX-backed damage path; the hook target is cvttss2si ecx,xmm1 immediately before integer accumulation.
inline constexpr std::uintptr_t kDamageResultRva = 0x167E9A2ULL;
inline constexpr std::uintptr_t kDamageResultBranchTargetRva = 0x167E9CCULL;
inline constexpr bool kDamageResultResolved = true;
inline constexpr std::size_t kDamageResultPatchLength = 12;
inline constexpr std::array<std::uint8_t, 12> kDamageResultPrologue = {0xF3, 0x0F, 0x2C, 0xC9, 0x81, 0xF9, 0x00, 0x00, 0x00, 0x80, 0x74, 0x1E};

// damage_accumulate: resolved_unique (1 hit(s)); Damage accumulator telemetry entry.
inline constexpr std::uintptr_t kDamageAccumulateRva = 0x167E8A0ULL;
inline constexpr std::uintptr_t kDamageAccumulateBranchTargetRva = 0x0ULL;
inline constexpr bool kDamageAccumulateResolved = true;
inline constexpr std::size_t kDamageAccumulatePatchLength = 12;
inline constexpr std::array<std::uint8_t, 12> kDamageAccumulatePrologue = {0x0F, 0x10, 0x42, 0x10, 0x44, 0x0F, 0xB6, 0x82, 0x90, 0x00, 0x00, 0x00};

// fov_setter: resolved_unique (1 hit(s)); Camera base-FOV setter. The compare displacement is wildcarded; the store to camera +0x918 is the semantic discriminator.
inline constexpr std::uintptr_t kFovSetterRva = 0x137D170ULL;
inline constexpr std::uintptr_t kFovSetterBranchTargetRva = 0x0ULL;
inline constexpr bool kFovSetterResolved = true;
inline constexpr std::size_t kFovSetterPatchLength = 12;
inline constexpr std::array<std::uint8_t, 12> kFovSetterPrologue = {0x48, 0x83, 0xEC, 0x48, 0x0F, 0x29, 0x74, 0x24, 0x30, 0x0F, 0x28, 0xF1};

// fov_getter: resolved_unique (1 hit(s)); Camera base-FOV getter.
inline constexpr std::uintptr_t kFovGetterRva = 0x1437A00ULL;
inline constexpr std::uintptr_t kFovGetterBranchTargetRva = 0x0ULL;
inline constexpr bool kFovGetterResolved = true;
inline constexpr std::size_t kFovGetterPatchLength = 12;
inline constexpr std::array<std::uint8_t, 12> kFovGetterPrologue = {0xF3, 0x0F, 0x10, 0x81, 0x18, 0x09, 0x00, 0x00, 0xC3, 0xCC, 0xCC, 0xCC};

// fov_view_getter: resolved_unique (1 hit(s)); Renderer-facing view-FOV getter.
inline constexpr std::uintptr_t kFovViewGetterRva = 0x6C7D60ULL;
inline constexpr std::uintptr_t kFovViewGetterBranchTargetRva = 0x0ULL;
inline constexpr bool kFovViewGetterResolved = true;
inline constexpr std::size_t kFovViewGetterPatchLength = 12;
inline constexpr std::array<std::uint8_t, 12> kFovViewGetterPrologue = {0xF3, 0x0F, 0x10, 0x81, 0x58, 0x09, 0x00, 0x00, 0xC3, 0xCC, 0xCC, 0xCC};

} // namespace wfmaxon_offsets
