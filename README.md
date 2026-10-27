# WF_MAXON
## Architecture

```text
WF_MAXON folder\
  WF_MAXONLoader.exe   own reflective loader (UAC elevated)
  WF_MAXON.dll         the entire payload: UI + controller + game hooks
  WarframeHelper.ini   profile written by APPLY
  custom_hooks\        optional Lua hooks
  custom_localization\   optional localization
```

Flow:

```text
WF_MAXONLoader.exe
  -> find Warframe.x64.exe process
  -> enable SeDebugPrivilege
  -> read WF_MAXON.dll from disk and reflective-map into Warframe
  -> call the payload's Bootstrap export (relocations, imports, TLS, DllMain)
  -> WF_MAXON.dll's UI thread opens
  -> user toggles, APPLY -> writes WarframeHelper.ini
  -> LOAD ENGINE -> EnableAllFeatures() applies the saved INI to the game
```

## Files in the WF_MAXON folder

| File | What |
|------|------|
| `WF_MAXON.exe` | optional standalone editor (config-only, never injected) |
| `WF_MAXONLoader.exe` | UAC elevated launcher; reflective-maps `WF_MAXON.dll` into Warframe |
| `WF_MAXON.dll` | the entire payload: UI + controller + Apply-cheats to the game |
| `WarframeHelper.ini` | profile (written by APPLY) |
| `test_host.exe` | native DLL smoke host used by the build |
| `wf_maxon_selftest.log` | self-test report |


- The payload filename is `WF_MAXON.dll`, not `OwHelper.dll`.
- The loader is open-source and rebuildable.
- It only knows the two file names it needs: `WF_MAXONLoader.exe` and `WF_MAXON.dll`.

The loader's approach — `OpenProcess` + `VirtualAllocEx` + section copy + remote `Bootstrap` thread — is the same shape as the proven loader. It is not a byte-for-byte copy.

## Included controls (23 + 3 sliders)

### Combat
- One-Hit Kill
- Skill Mods Multiplier + value slider
- Super Skills Mode
- No Skill Cooldown
- Invulnerable
- Max Energy
- Enemy & Loot Detector

### Arsenal
- Always Max Combo
- Infinite Primary & Secondary Ammo
- Infinite Incarnon Ammo
- Fast Ammo Regen / Disable Ammo Regen (exclusive)
- Fast Incarnon Charge
- Fast Tauron Strike

### World
- Auto Koumei Challenge
- Always Eximus
- Vacuum Always
- New / Legacy Loot Vacuum (exclusive)
- Loot Hotkey
- Increase Vacuum Limit + range slider

### View
- FOV Changer + slider

## Build

```bat
cd D:\game\warframe_menu\WF_MAXON
build.bat
```

Output:

```text
build\WF_MAXON.exe          optional standalone editor
build\WF_MAXON.dll          the entire payload
build\WF_MAXONLoader.exe    own reflective loader
build\test_host.exe         native DLL smoke-test host
build\wf_maxon_selftest.log round-trip log for all keys/sliders
build\WF_MAXON_wizard.png   first-run wizard preview
build\WF_MAXON_main.png     main UI preview
```

The build runs both:

1. **Config round-trip**: writes a temp INI with every toggle + slider, reloads it, asserts zero mismatches, asserts exclusive groups (regen/vacuum) hold, asserts the mirrored `rage_Cheats` profile was written, asserts `Ammo1` and `Ammo2` aliases were paired.
2. **Native DLL smoke test**: `test_host.exe build\WF_MAXON.dll` loads the payload, calls `WF_MAXON_GetVersion` and `WF_MAXON_GetWindowClass`, polls for the `WF_MAXON_MainWindow`, then closes it.

Expected:

```text
[WF_MAXON] BUILD SUCCEEDED
[WF_MAXON] CONTROLLER ... bytes
[WF_MAXON] PAYLOAD    ... bytes
[WF_MAXON] LOADER     ... bytes
WF_MAXON SELFTEST
result=PASS
payload=loaded version=0x00010000 class=WF_MAXON_MainWindow title=WF_MAXON hwnd=...
[WF_MAXON] ALL TESTS PASSED
```

## Run

1. Start Warframe normally.
2. Open the WF_MAXON folder.
3. Run **`WF_MAXONLoader.exe`** as Administrator. It finds Warframe.x64.exe and reflective-maps `WF_MAXON.dll`.
4. `WF_MAXON.dll` opens its UI inside the Warframe process.
5. Toggle features. Click **APPLY** to write `[enable_Cheats]` (and the mirrored `[rage_Cheats]`).
6. Click **LOAD ENGINE** to apply the saved profile to the running game.

## Configuration path

Auto-detected:

```text
D:\SteamLibrary\steamapps\common\Warframe
D:\Program Files (x86)\Steam\steamapps\common\Warframe
D:\Program Files\Steam\steamapps\common\Warframe
D:\Games\Warframe
```

Or set:

```text
WF_MAXON_GAME_DIR=D:\Your\Warframe\Directory
```

## Refreshing offsets after a Warframe update

Do not edit an RVA inside `OwHelper.dll` by hand. The payload now keeps the current-build manifest in `src\generated\warframe_offsets.h` and refreshes it from unique byte signatures:

```bat
cd /d D:\game\warframe_menu\WF_MAXON
refresh_offsets.bat
build.bat
```

`refresh_offsets.bat` scans the installed `Warframe.x64.exe`, records its SHA-256/PE identity, writes a timestamped report under `WF_MAXON\offsets\`, and atomically replaces the generated header only when every required signature has exactly one hit. Zero-hit or multi-hit signatures leave the previous header untouched instead of guessing. `build.bat` runs the same refresh automatically before linking; use `WF_MAXON_WARFRAME_EXE` when the game is installed elsewhere.

The current installed build was scanned successfully: FireWeaponRmi `0xEA5790`, WeaponAmount `0x5E47F0`, DamageResult `0x167E9A2`, DamageAccumulate `0x167E8A0`, FOV setter `0x137D170`, FOV getter `0x1437A00`, and view-FOV getter `0x6C7D60` (RVAs, not file offsets). `DamageResult` is anchored by the unique RDX-backed clamp sequence and derives its short-branch destination from the generated patch bytes, so the OneHitKill context hook no longer depends on the old 471-hit arithmetic signature.

For an offline build that deliberately reuses the last generated manifest:

```bat
set WF_MAXON_SKIP_OFFSET_REFRESH=1
build.bat
```

Do not use that override after a game update unless the generated header's binary hash still matches the executable you are testing.


```bat
python D:\game\warframe_menu\_dump_offsets\_warframe_offset_dumper.py ^
  --binary "D:\SteamLibrary\steamapps\common\Warframe\Warframe.x64.exe" ^
  --live-pid <PID> ^
  --out-dir D:\game\warframe_menu\_dump_offsets\live_current
```

The live engine rejects a candidate that points into the executable image or fails the avatar fingerprint (`team +0x688`, sub-object `+0x588`, module-backed vtable). A readable pointer alone is not enough; an offset mismatch must fail closed instead of writing memory.
