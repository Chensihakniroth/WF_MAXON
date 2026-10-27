-- find_energy_write3.lua
local base   = wf.base()
local player = wf.player_base()
if not player or player == 0 then print("[ERR] no player"); return end
print(string.format("[EW3] BASE=0x%X PLAYER=0x%X", base, player))

local RANGE = 0x4000
local baseline = nil
local frame = 0
local reported = {}
local fired = false

local function sample()
    local t = {}
    for i = 0, RANGE, 4 do
        local ok, v = pcall(function() return wf.read_float(player + i) end)
        if ok and v then t[i] = v end
    end
    return t
end

local function on_frame(ctx)
    local ok, err = pcall(function()
        frame = frame + 1
        if not fired then fired = true; print("[EW3] hook fired, frame="..frame) end
        if frame == 3 then
            baseline = sample()
            print("[EW3] baseline captured - cast an ability now")
            return
        end
        if frame < 8 then return end
        if frame % 60 ~= 0 then return end
        if not baseline then return end
        local cur = sample()
        local cnt = 0
        for i = 0, RANGE, 4 do
            local b = baseline[i]
            local c = cur[i]
            if b and c and b ~= c and not reported[i] then
                print(string.format("[EW3] CHANGED +0x%04X: %.6f -> %.6f", i, b, c))
                reported[i] = true
                cnt = cnt + 1
            end
        end
        if cnt == 0 then
            print(string.format("[EW3] frame %d: no changes yet", frame))
        end
    end)
    if not ok then print("[EW3] on_frame error: "..tostring(err)) end
end

local HOOK_TARGETS = {
    wf.base() + 0x1B66B00,
    wf.base() + 0x1B66B10,
    wf.base() + 0x1B66A00,
    wf.base() + 0x10B91E0,
    wf.base() + 0x10B9200,
}
for _, addr in ipairs(HOOK_TARGETS) do
    if addr and addr > 0 then
        local ok, id = pcall(function() return wf.hook(addr, on_frame) end)
        if ok and id and id ~= 0 then
            print(string.format("[EW3] hooked 0x%X id=%d", addr, id))
            break
        end
    end
end
wf.on_unload(function() print("[EW3] unloaded") end)
