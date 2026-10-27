-- find_powersuit_vtable.lua
local base   = wf.base()
local player = wf.player_base()
print(string.format("[PS] BASE=0x%X PLAYER=0x%X", base, player))

-- scan avatar for pointers whose first qword is a code address
local found = {}
for i = 0, 0x2000, 8 do
    local ok, p = pcall(function() return wf.read_ptr(player + i) end)
    if ok and p and p ~= 0 and p > 0x10000 and p < 0x7FFFFFFFFFFF then
        -- check if p's first qword is in module code range
        local ok2, v0 = pcall(function() return wf.read_ptr(p) end)
        if ok2 and v0 and v0 > base and v0 < base + 0x3000000 then
            -- count slots that are code (up to 32)
            local cnt = 0
            for s = 0, 31 do
                local ok3, vs = pcall(function() return wf.read_ptr(p + s*8) end)
                if ok3 and vs and vs > base and vs < base + 0x3000000 then
                    cnt = cnt + 1
                else
                    break
                end
            end
            if cnt >= 8 then
                print(string.format("  +0x%04X -> 0x%X  vtable_slots=%d  first=0x%X RVA=0x%X", i, p, cnt, v0, v0-base))
                table.insert(found, {off=i, ptr=p, slots=cnt, first=v0})
            end
        end
    end
end
print(string.format("[PS] found %d candidate vtables", #found))
-- print slot 474 for each candidate with enough slots
for _, f in ipairs(found) do
    if f.slots > 474 then
        local p = wf.read_ptr(f.ptr + 474*8)
        print(string.format("  +0x%04X slot474 = 0x%X RVA=0x%X", f.off, p, p-base))
    end
end
