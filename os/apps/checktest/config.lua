-- Check test: the knobs. The case itself comes from case.lua, which the device test pushes.
return {
  -- How often "CT tick <n>" is logged from on_update.
  tick_ms = 1000,

  -- What a flush_draw case fills the screen with on every frame (red, green, blue; 0..255).
  -- test/device/t_sign.py looks for this colour in its screenshots (0xF81F in RGB565).
  flush_color = {255, 0, 255},

  -- What a storage_probe case tries to read through badge.storage. Both must fail: the history
  -- file is outside the app's directory.
  probe_paths = {"../../vk/history.bin", "/vk/history.bin"},
}
