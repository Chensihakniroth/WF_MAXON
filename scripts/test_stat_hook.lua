-- ============================================================================
-- test_stat_hook.lua — Global Upgrade & Ability Stat Hook
-- Targets sub_14123DF60 (RVA 0x123DF60):
-- The universal stat calculator for Warframe (UpgradeValue::GetValue / ComputeStat)
-- Every mod, ability strength, efficiency, duration, range calculation runs here!
-- ============================================================================

local base = wf.base()
local target_rva = 0x123DF60
local target_addr = base + target_rva

print("========================================")
print(string.format("BASE               = 0x%X", base))
print(string.format("UpgradedValue Eval = 0x%X (RVA 0x%X)", target_addr, target_rva))
print("========================================")

local hook_id = nil
local tick_count = 0

-- Callback fires whenever any stat/modifier is evaluated
local function on_stat_calc(ctx)
    tick_count = tick_count + 1
    
    -- Print first 10 calls to inspect the struct rcx
    if tick_count <= 10 then
        local p = ctx.rcx
        local flags = wf.read_int(p + 0x30) or 0
        local f18 = wf.read_float(p + 0x18) or 0
        local f1C = wf.read_float(p + 0x1C) or 0
        local f20 = wf.read_float(p + 0x20) or 0
        local f24 = wf.read_float(p + 0x24) or 0
        print(string.format("[STAT] #%d rcx=0x%X f18=%.2f f1C=%.2f f20=%.2f f24=%.2f flags=0x%X",
            tick_count, p, f18, f1C, f20, f24, flags))
    end
end

local ok, id = pcall(wf.hook, target_addr, on_stat_calc)
if ok and id and id ~= 0 then
    hook_id = id
    print(string.format("[SUCCESS] Hooked UpgradedValue::GetValue! id=%d", hook_id))
    print("Watch the Active Memory Hooks table in WF_MAXON UI!")
else
    print("[FAIL] Failed to install hook at 0x" .. string.format("%X", target_addr))
end

wf.on_unload(function()
    if hook_id then
        wf.unhook(hook_id)
        print("[UNLOAD] Removed stat calculation hook")
    end
end)
