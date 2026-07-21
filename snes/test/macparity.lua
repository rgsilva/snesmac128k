-- M4b parity harness: run the live Mac machine model on the SNES and drain the
-- WRAM mailbox ($7EF000). Same protocol as parity.lua, but a much longer frame
-- budget: booting the ROM for MACP_N steps at -O0 takes many emulated frames.
local MB_MAGIC, MB_STATUS, MB_WINDEX, MB_RING = 0xF000, 0xF002, 0xF004, 0xF010
local RING_SIZE = 3552

local frames, sawMagic, readIndex, line = 0, false, 0, ""

local function word(addr) return emu.readWord(addr, emu.memType.snesWorkRam, false) end

local function drain()
  local wi = word(MB_WINDEX)
  while readIndex ~= wi do
    local b = emu.read(MB_RING + (readIndex % RING_SIZE), emu.memType.snesWorkRam)
    if b == 10 then print("OUT " .. line); line = "" else line = line .. string.char(b) end
    readIndex = (readIndex + 1) % 65536
  end
end

local function onFrame()
  frames = frames + 1
  if not sawMagic then
    if word(MB_MAGIC) == 0xCAFE then sawMagic = true
    elseif frames > 1200 then print("RESULT FAIL macparity no-magic"); emu.stop(98) end
    return
  end
  drain()
  local status = word(MB_STATUS)
  if status ~= 0 then
    drain()
    if line ~= "" then print("OUT " .. line) end
    if status == 1 then print("RESULT PASS macparity"); emu.stop(0)
    else print(string.format("RESULT FAIL macparity status=%04X", status)); emu.stop(1) end
    return
  end
  if frames > 216000 then print("RESULT FAIL macparity timeout"); emu.stop(99) end
end

emu.addEventCallback(onFrame, emu.eventType.endFrame)
