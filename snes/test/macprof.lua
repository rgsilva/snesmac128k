-- Sampling profiler: run macbench and sample the 65816 PC twice per frame.
-- Prints PROF <addr> <count> lines at the end; symbolicate against
-- build/macbench.map (tools: see docs/PERF.md).
local MB_MAGIC, MB_STATUS, MB_WINDEX, MB_RING = 0x24000, 0x24002, 0x24004, 0x24010
local RING_SIZE = 3552
local frames, sawMagic, readIndex, line = 0, 0 == 1, 0, ""
local samples = {}
local shown = false

local function word(addr) return emu.readWord(addr, emu.memType.snesSaveRam, false) end

local function sample()
  local s = emu.getState()
  if not shown then
    for k, _ in pairs(s) do if k:find("pc") or k:find("k") == 5 then print("KEY " .. k) end end
    shown = true
  end
  local pc = s["cpu.pc"] or 0
  local k = s["cpu.k"] or 0
  local a = k * 65536 + pc
  samples[a] = (samples[a] or 0) + 1
end

local function drain()
  local wi = word(MB_WINDEX)
  while readIndex ~= wi do
    local b = emu.read(MB_RING + (readIndex % RING_SIZE), emu.memType.snesSaveRam)
    if b == 10 then print("OUT " .. line); line = "" else line = line .. string.char(b) end
    readIndex = (readIndex + 1) % 65536
  end
end

local function onFrame()
  frames = frames + 1
  sample()
  if not sawMagic then
    if word(MB_MAGIC) == 0xCAFE then sawMagic = true; readIndex = word(MB_WINDEX)
    elseif frames > 1200 then print("RESULT FAIL no-magic"); emu.stop(98) end
    return
  end
  drain()
  local status = word(MB_STATUS)
  if status ~= 0 then
    for a, c in pairs(samples) do print(string.format("PROF %06X %d", a, c)) end
    print("RESULT PASS macprof")
    emu.stop(0)
    return
  end
  if frames > 40000 then print("RESULT FAIL timeout"); emu.stop(99) end
end

emu.addEventCallback(onFrame, emu.eventType.endFrame)
emu.addEventCallback(sample, emu.eventType.startFrame)
