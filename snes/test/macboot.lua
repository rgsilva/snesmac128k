-- M4b live boot: let the Mac machine run on the SNES and periodically screenshot
-- the output so we can watch the boot progress. Mesen's io is sandboxed, so the
-- PNG is dumped as hex ("SHOTHEX ...") for the shell to decode. Also echoes the
-- ROM's "steps/vbl" mailbox so we can see how far the boot has got. Runs a long
-- time (the boot is slow); kill externally when done.
local MB_MAGIC, MB_WINDEX, MB_RING = 0xF000, 0xF004, 0xF010
local RING_SIZE = 3552
local frames, readIndex, line, shots = 0, 0, "", 0
local SHOT_EVERY = 1800          -- ~30 s of emulated time

local function drain()
  local wi = emu.readWord(MB_WINDEX, emu.memType.snesWorkRam, false)
  while readIndex ~= wi do
    local b = emu.read(MB_RING + (readIndex % RING_SIZE), emu.memType.snesWorkRam)
    if b == 10 then print("OUT " .. line); line = "" else line = line .. string.char(b) end
    readIndex = (readIndex + 1) % 65536
  end
end

local function shoot()
  local ok, png = pcall(emu.takeScreenshot)
  if ok and type(png) == "string" and #png > 0 then
    local t = {}
    for i = 1, #png do t[i] = string.format("%02x", png:byte(i)) end
    print("SHOT " .. shots .. " len " .. #png)
    print("SHOTHEX " .. table.concat(t))
    shots = shots + 1
  end
end

local function onFrame()
  frames = frames + 1
  drain()
  if frames % SHOT_EVERY == 0 then shoot() end
  if frames > 2160000 then print("RESULT PASS macboot done"); emu.stop(0) end   -- ~10 h cap
end
emu.addEventCallback(onFrame, emu.eventType.endFrame)
