-- Bench harness: run macbench.sfc (our own 68000 ROM — no Apple bytes) and
-- stamp each "P <phase>" mailbox line with the frame count. Frames-per-phase
-- over the host's instructions-per-phase (benchhost) = cycles/instruction,
-- the metric every optimization in docs/PERF.md is judged by.
local MB_MAGIC, MB_STATUS, MB_WINDEX, MB_RING = 0xF000, 0xF002, 0xF004, 0xF010
local RING_SIZE = 3552

local frames, sawMagic, readIndex, line = 0, false, 0, ""
local phases = {}                       -- { {mark=string, frame=number}, ... }

local function word(addr) return emu.readWord(addr, emu.memType.snesWorkRam, false) end

local function handle(l)
  print("OUT " .. l)
  if l:sub(1, 2) == "P " then phases[#phases + 1] = { mark = l:sub(3), frame = frames } end
end

local function drain()
  local wi = word(MB_WINDEX)
  while readIndex ~= wi do
    local b = emu.read(MB_RING + (readIndex % RING_SIZE), emu.memType.snesWorkRam)
    if b == 10 then handle(line); line = "" else line = line .. string.char(b) end
    readIndex = (readIndex + 1) % 65536
  end
end

local function onFrame()
  frames = frames + 1
  if not sawMagic then
    if word(MB_MAGIC) == 0xCAFE then sawMagic = true
    elseif frames > 1200 then print("RESULT FAIL macbench no-magic"); emu.stop(98) end
    return
  end
  drain()
  local status = word(MB_STATUS)
  if status ~= 0 then
    drain()
    if line ~= "" then handle(line); line = "" end
    for i = 2, #phases do
      print(string.format("PHASE %s frames=%d", phases[i - 1].mark, phases[i].frame - phases[i - 1].frame))
    end
    if status == 1 then print("RESULT PASS macbench"); emu.stop(0)
    else print(string.format("RESULT FAIL macbench status=%04X", status)); emu.stop(1) end
    return
  end
  if frames > 40000 then print("RESULT FAIL macbench timeout"); emu.stop(99) end
end

emu.addEventCallback(onFrame, emu.eventType.endFrame)
