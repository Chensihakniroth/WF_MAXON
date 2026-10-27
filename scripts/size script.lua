-- RESTORE SPEED & SCALE
local avatar = wf.player_base()
if avatar and avatar ~= 0 then
    wf.write_float(avatar + 0x44, 1.0)   -- speed
    wf.write_float(avatar + 0x2FC, 1.0)  -- scale
    print("[RESTORE] Speed and scale reset to 1.0")
end
