-- find_energy_write.lua — detect avatar fields that change when casting
local base   = wf.base()
local player = wf.player_base()
if not player or player == 0 then print("[ERR] no player"); return end

print(string.format("[EW] BASE=0x%X PLAYER=0x%X", base, player))

local RANGE = 0x2000
local baseline = {}
local captured = false
local reported = {}
local frame = 0

local function sample()
    local t = {}
    for i = 0, RANGE, 4 do
        local ok, v = pcall(function() return wf.read_float(player + i) end)
        if ok and v then t[i] = v end
    end
    return t
end

local function on_frame(ctx)
    frame = frame + 1
    if frame == 5 then
        baseline = sample()
        print("[EW] baseline captured at frame 5 - cast an ability now")
        return
    end
    if frame < 10 then return end
    if not captured then
        local cur = sample()
        for i = 0, RANGE, 4 do
            local b = baseline[i]
            local c = cur[i]
            if b and c and b ~= c and not reported[i] then
                -- only report if decreased (energy consumption) or changed significantly
                if c < b then
                    print(string.format("[EW] CHANGED (decreased) +0x%04X: %.4f -> %.4f", i, b, c))
                    reported[i] = true
                end
            end
        end
    end
end

-- install hook on OnFrame thunk
local HOOK_TARGETS = { wf.base() + 0x1B66B00 }
for _, addr in ipairs(HOOK_TARGETS) do
    local ok, id = pcall(function() return wf.hook(addr, on_frame) end)
    if ok and id and id ~= 0 then
        print(string.format("[EW] hooked 0x%X id=%d", addr, id))
        break
    end
end

wf.on_unload(function() print("[EW] unloaded") end)
