-- ============================================================================
-- infinite_energy_and_cost.lua — Zero Energy Cost & Infinite Energy Hook
-- Discovered from IDA Pro in live Warframe build:
--   1) SpendEnergy / ConsumeEnergy: sub_140B7D410 (RVA 0xB7D410)
--      Args: rcx = this (PowerSuit/Avatar), xmm1 = float amount
--   2) GetAbilityEnergyCost:        sub_140DB9690 (RVA 0xDB9690)
--      Returns: xmm0 = float cost
-- ============================================================================

local base = wf.base()

-- 1. Function that subtracts/spends energy on ability cast:
local spend_energy_rva = 0xB7D410
local spend_energy_addr = base + spend_energy_rva

-- 2. Function that calculates the energy cost of any ability:
local calc_cost_rva = 0xDB9690
local calc_cost_addr = base + calc_cost_rva

print("========================================")
print(string.format("BASE                = 0x%X", base))
print(string.format("SpendEnergy (Func)  = 0x%X (RVA 0x%X)", spend_energy_addr, spend_energy_rva))
print(string.format("GetEnergyCost(Func) = 0x%X (RVA 0x%X)", calc_cost_addr, calc_cost_rva))
print("========================================")

local spend_hook_id = nil
local cost_hook_id = nil
local spend_count = 0
local cost_count = 0

-- Hook 1: Whenever energy is spent, set amount (xmm1) to 0.0
-- In x64 calling convention:
-- rcx = avatar / energy system, xmm1 = amount to spend (float)
-- Setting ctx.xmm[1] to 0.0 prevents energy deduction
-- while allowing the original function to update any internal state cleanly!
local function on_spend_energy(ctx)
    spend_count = spend_count + 1
    ctx.xmm1 = 0.0

    if spend_count <= 10 then
        print(string.format("[ENERGY] SpendEnergy amount set to 0.0! count=%d", spend_count))
    end
end

-- Hook 2: Whenever ability cost is calculated, return 0.0
local function on_get_energy_cost(ctx)
    cost_count = cost_count + 1
    ctx.xmm0 = 0.0
    ctx.skip = 1

    if cost_count <= 10 then
        print(string.format("[ENERGY] Ability energy cost set to 0.0! count=%d", cost_count))
    end
end

-- Install SpendEnergy hook
local ok1, id1 = pcall(wf.hook, spend_energy_addr, on_spend_energy)
if ok1 and id1 and id1 ~= 0 then
    spend_hook_id = id1
    print(string.format("[SUCCESS] Hooked SpendEnergy! ID=%d at 0x%X", spend_hook_id, spend_energy_addr))
else
    print("[FAIL] Failed to hook SpendEnergy")
end

-- Install GetEnergyCost hook
local ok2, id2 = pcall(wf.hook, calc_cost_addr, on_get_energy_cost)
if ok2 and id2 and id2 ~= 0 then
    cost_hook_id = id2
    print(string.format("[SUCCESS] Hooked GetEnergyCost! ID=%d at 0x%X", cost_hook_id, calc_cost_addr))
else
    print("[FAIL] Failed to hook GetEnergyCost")
end

wf.on_unload(function()
    if spend_hook_id then
        wf.unhook(spend_hook_id)
        print("[UNLOAD] Removed SpendEnergy hook")
    end
    if cost_hook_id then
        wf.unhook(cost_hook_id)
        print("[UNLOAD] Removed GetEnergyCost hook")
    end
end)
