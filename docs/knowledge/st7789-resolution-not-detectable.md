# The ST7789 panel resolution cannot be auto-detected — it must be declared

There is no way to read a panel's visible resolution back from an ST7789 over SPI. Resolution
must come from configuration (a setting, or a per-image/per-board default) — never a probe. This
is why the display-resolution selection is a declared setting (APP-26) rather than
auto-detection, and why the native driver takes `width`/`height` as parameters.

## Why detection is impossible (three independent reasons)

1. **The controller frame memory is a fixed 240×320, independent of the glass.** A 240×240,
   320×240, 240×320, or 240×135 ST7789 is the *same silicon* with different glass bonded on.
   The controller has no register that reports the bonded glass dimensions — it doesn't know
   its own panel size.
2. **The read commands don't carry dimensions.** `RDDID (0x04)` / `RDDST (0x09)` return the
   controller ID + status, which are **identical** across a 240×240 and a 320×240 ST7789.
3. **The SeedSigner SPI wiring is write-only.** The Waveshare hat exposes DC/MOSI/SCLK/CS with
   **no MISO**, so no register can be read back at all. (Same for the ILI9341/ILI9486 — same
   controller class, same limitation.)

## Implications in this codebase

- `native_display_init(width, height, …)` and `lvgl_init(hor_res, ver_res)` take the resolution
  as parameters (default 240×240). `set_resolution(w, h)` switches at runtime; `display_size()`
  reads back the *configured* profile. MADCTL is chosen from the configured dims —
  `0x60` (landscape) for 320×240, else `0x70` (`display_st7789.cpp:180-184`) — not from any
  panel probe.
- Because the resolution is declared, a **wrong** declaration renders wrong: e.g. driving a
  320×240 panel at the default logical 240×240 paints a correct left square and leaves the
  extra columns as uninitialized power-on GRAM ("garbage"). That extra region can still be
  blanked *without* knowing the true resolution — clear the controller's **fixed 240×320
  maximum**, which is a superset of every ST7789 glass (see `docs/wide-panel-unused-strip-todo.md`).

## Related

- Cross-repo ledger: **RASPI-12** (native resolution API) + **APP-26** (Pi-only resolution
  selection setting — replaces the obsolete `SETTING__DISPLAY_CONFIGURATION` → `renderer.py`
  Python-driver path that the native pump made vestigial).
- `docs/wide-panel-unused-strip-todo.md` — the uninitialized-strip blanking + sideways-instruction
  ideas (interim, independent of the resolution setting).
