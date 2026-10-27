-- present_hook.lua — Present-triggered energy offset discovery (defensive, multi-pattern)
local base   = wf.base()
local player = wf.player_base()
print(string.format("[PH] base=0x%X player=0x%X", base or 0, player or 0))
if not player or player == 0 then print("[PH] ERROR: no player base"); return end

local RANGE   = 0x4000
local dxgi    = wf.get_module("dxgi.dll")
local PATTERNS = {
    "48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 48 8B 05 ?? ?? ?? ?? 48 8B F9",
    "48 89 5C 24 ?? 57 48 83 EC ?? 48 8B 05 ?? ?? ?? ?? 48 8B F9",
    "48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57",
    "48 8B 05 ?? ?? ?? ?? 48 8B F9",
    "40 53 48 83 EC ?? 48 8B 05 ?? ?? ?? ??",
    "48 89 5C 24 ?? 48 89 6C 24 ?? 57 48 83 EC ??",
}

local baseline = nil
local reported = {}
local frame    = 0
local hook_id  = 0

local function sample_block(p)
    local ok, data = pcall(function() return wf.read_bytes(p, RANGE) end)
    if not ok or not data or #data < 4 then return nil end
    local t = {}
    local n = #data - 3
    for i = 0, n, 4 do
        local b0 = data:byte(i+1); local b1 = data:byte(i+2)
        local b2 = data:byte(i+3); local b3 = data:byte(i+4)
        if b0 and b1 and b2 and b3 then
            t[i] = string.unpack("<f", string.char(b0,b1,b2,b3))
        end
    end
    return t
end

local function player_sane(p)
    local ok, e = pcall(function() return wf.read_float(p + 0x3A78) end)
    return ok and e and e >= 0 and e <= 5000
end

local function on_present(ctx)
    local ok, err = pcall(function()
        frame = frame + 1
        local p = wf.player_base()
        if not p or p == 0 then return end
        if not player_sane(p) then return end

        if frame % 600 == 1 then
            print(string.format("[PH] frame=%d player=0x%X", frame, p))
        end

        if not baseline then
            baseline = sample_block(p)
            if baseline then print(string.format("[PH] baseline captured at frame %d - cast now", frame)) end
            return
        end

        if frame % 30 ~= 0 then return end
        local cur = sample_block(p)
        if not cur then return end

        local cnt = 0
        for i = 0, RANGE - 4, 4 do
            local b = baseline[i]; local c = cur[i]
            if b and c and b ~= c and not reported[i] then
                if b >= 0 and b <= 1000 and c < b then
                    local d = b - c
                    if d > 0 then
                        print(string.format("[PH] +0x%04X before=%.4f after=%.4f delta=%.4f", i, b, c, d))
                        reported[i] = true
                        cnt = cnt + 1
                    end
                end
            end
        end
        if cnt > 0 then print(string.format("[PH] frame %d: %d matching offsets", frame, cnt)) end
        baseline = cur
    end)
    if not ok then
        print(string.format("[PH] callback error: %s", tostring(err)))
    end
end

local function install()
    if not dxgi then print("[PH] ERROR: dxgi.dll not found"); return false end
    for _, pat in ipairs(PATTERNS) do
        local pa = wf.find_pattern(dxgi, pat)
        if pa then
            print(string.format("[PH] pattern matched: %s -> 0x%X", pat:sub(1,40), pa))
            local ok2, id = pcall(function() return wf.hook(pa, on_present) end)
            if ok2 and id and id ~= 0 then
                hook_id = id
                print(string.format("[PH] Present hook id=%d installed at 0x%X", hook_id, pa))
                return true
            else
                print(string.format("[PH] wf.hook failed for 0x%X", pa))
            end
        end
    end
    print("[PH] ERROR: no Present pattern matched in dxgi.dll")
    return false
end

install()

wf.on_unload(function()
    if hook_id ~= 0 then
        local ok = pcall(function() wf.unhook(hook_id) end)
        print(string.format("[PH] hook removed id=%d ok=%s", hook_id, tostring(ok)))
    end
    print("[PH] unloaded")
end)