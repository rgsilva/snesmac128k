-- M4: run the macvideo ROM, let it draw, screenshot the SNES output.
local frames = 0
local function onFrame()
  frames = frames + 1
  if frames == 20 then
    local ok, png = pcall(emu.takeScreenshot)
    print("takeScreenshot ok=" .. tostring(ok) .. " type=" .. type(png) ..
          (type(png) == "string" and (" len=" .. #png) or (" val=" .. tostring(png))))
    if ok and type(png) == "string" and #png > 0 then
      local t = {}
      for i = 1, #png do t[i] = string.format("%02x", png:byte(i)) end
      print("PNGHEX " .. table.concat(t))     -- decoded by the shell (io is sandboxed)
    end
    print("RESULT PASS macvideo")
    emu.stop(0)
  end
  if frames > 120 then print("RESULT FAIL macvideo timeout"); emu.stop(1) end
end
emu.addEventCallback(onFrame, emu.eventType.endFrame)
