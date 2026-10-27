-- ============================================================
-- WF_MAXON: DIAGNOSTIC — trace player_base resolver
-- Run this in a solo mission and paste me the output
-- ============================================================

local base = wf.base()
print("========================================")
print(string.format("BASE = 0x%X", base))

-- Test 1: What does player_base() actually return?
local player = wf.player_base()
print(string.format("player_base() = 0x%X", player or 0))

-- Test 2: If it's garbage, let's check what's at common offsets
-- from a known-good avatar candidate we can derive

-- We know from earlier static analysis:
-- Avatar vtable in gameplay build is around RVA 0x23695C0
-- Let's check if that vtable pointer exists anywhere in memory

-- Read the first qword at player_base (should be vtable if valid)
if player and player > 0 then
    local vt = wf.read_ptr(player)
    print(string.format("  [player + 0x0] = 0x%X", vt or 0))
    
    -- Check if vt is module-backed
    if vt and vt > base and vt < base + 0x2CD9000 then
        print(string.format("  vtable RVA = 0x%X (VALID MODULE)", vt - base))
    else
        print("  vtable NOT module-backed -> INVALID AVATAR")
    end
    
    -- Scan first 0x40 bytes for debugging
    print("  First 0x40 qwords:")
    for i = 0, 0x40, 8 do
        local v = wf.read_ptr(player + i)
        if v and v ~= 0 then
            local tag = ""
            if v > base and v < base + 0x2CD9000 then
                tag = string.format("  <== RVA 0x%X", v - base)
            end
            print(string.format("    +0x%02X = 0x%X%s", i, v, tag))
        end
    end
else
    print("player_base() returned NULL or 0")
end

print("========================================")
