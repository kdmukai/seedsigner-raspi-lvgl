# SeedSigner+ 320×240 scan: the centered-square sink (RASPI-7a) and why it's raspi-only

On a wide 320×240 panel (SeedSigner+ / "Plus") the live camera scan and image-entropy
preview render as a **centered 240×240 square with static black L/R pillars**, not a
full-width image. Two things about how this is wired are non-obvious and load-bearing for
any future work on the wide-panel preview.

## 1. The framing is 100% raspi-internal — the portable overlays already do the pillars

`camera_preview.cpp` builds a centered short-dim square sink (`side = min(w,h)`, positioned
at `((w-side)/2, (h-side)/2)`) and passes that rect to the overlay via
`spec.square_x/square_y/square_w/square_h`. It is tempting to think the wide panel needs a
new "landscape pillarboxed" overlay in the screens submodule — it does **not**:

- `camera_preview_overlay.cpp` and `camera_entropy_overlay.cpp` already **paint the gutter
  pillars** (`add_gutter_fill` over the parent region outside the square, keyed off
  `square_x/square_w` vs the full display) and **center every chrome element** (instruction
  text, status bar, progress fill, percent label, status dot) relative to `SX/SY/SW/SH`.
  When the square == the screen (Pi Zero 240², `square_x == 0`) the gutters are empty and
  nothing is added — identical to before.
- So the SeedSigner+ scan needed **zero screens-repo changes**. The `camera_preview_pillarboxed`
  component (the ESP-style rotated-text variant) is a *different* layout and is **not** used
  on the Pi — don't confuse it with `camera_preview_overlay` (chrome over the square), which
  is what the Pi builds and which handles the centered-square-with-pillars case generically.

The "port the P4 pillar-overlay layout" idea (RASPI-7) is therefore **dropped**: it is
cosmetic (moving the overlay chrome *into* the pillars), carries no perf gain, and the
existing overlay already renders correctly with the chrome centered over the square.

## 2. The square sink sidesteps the RASPI-8 overflow — RASPI-8 is the safety net, not the enabler

Each camera engine configures its **display/preview stream at the sink dims**
(`config->at(1).size = Size(disp_w, disp_h)`, where `disp_w/disp_h` come from
`camera_preview_get_sink_dims()` = the lv_image the sink owns). The blit worker then converts
that frame with the default 90° rotation into a buffer sized from the sink dims.

- With a **full-width** 320×240 sink, the 90° transpose makes the converted image `disp_h`
  wide (240) — but the old code passed `dst_w = disp_w` (320), striding past the buffer end:
  a ~51 KB/frame heap overflow (RASPI-8).
- Because **RASPI-7a keeps the sink square (240² even on the 320-wide Plus)**, the engine
  captures 240×240, converts a square (transpose is a no-op on the bounds), and never
  overflows. The decode stream is separately a 480×480 square, so the **decode path is
  byte-identical on the Pi Zero and the Plus** — only display *placement* differs.

RASPI-8 (pass the post-rotation width as `dst_w`) is still applied as **defense-in-depth**:
it makes the engines correct for *any* sink shape, so an intermediate state or a future
**full-width preview mode** can't silently corrupt the heap. The corollary: **anyone who
later builds a non-square (full-width) preview sink re-exposes the overflow path** — RASPI-8
contains it, but the geometry (a transposed image in a wider sink) is still wrong to display;
that's a framing decision (pillarbox vs. rotate-to-fit), not a bounds fix.

## Verified

SeedSigner+ (Pi Zero W / armv6l, 192.168.1.96), 2026-07-26.

**Standalone** via `tests/pi_plus_320_scan_test.py`, which inits 320×240 directly through the
RASPI-12 native display-resolution API (no app-side resolution setting needed):
- synthetic geometry: centered square, black pillars, chrome centered on the square;
- live camera: centered square + pillars, real 12-word SeedQR decoded (zbar hits climbing,
  3 payloads drained via `camera_scanner.poll_new()`), on-screen green-bar/dot feedback;
- 60s sustained decoding (428 zbar attempts), clean exit — no `free()` corruption / SIGSEGV.

**Full app** with APP-26 (the Pi resolution setting; `display_resolution: "320x240"` in
`/mnt/microsd/settings.json`, read at startup → `native_display_init(320,240)` +
`lvgl_init(320,240)`): app boots at 320×240, full-width UI renders, and **all three camera
surfaces — scan, image-entropy, and io_test — show the centered square** (io_test uses
SCREENS-9's own square plane, not this change, but also renders correctly centered on the
wide panel). Real-flow decode works; `camera_rotation: 180` is applied by the app (the
standalone harness does not read it, which is why its preview looked un-flipped). The
Hardware→resolution setting is restart-applied (APP-26 is restart-only, not live).

## Related
- `docs/nonsquare-panel-preview-rotation-todo.md` — the RASPI-8 overflow arithmetic + fix.
- `docs/_integration/lvgl-scan-preview-partial-flush.md` — the remaining `FULL → DIRECT`
  flush-side win (RASPI-7 "7b"), deferred as a separate global render-mode increment.
- `docs/wide-panel-unused-strip-todo.md` — the Option-A full-extent GRAM clear (a different
  layer: the ST7789 init blank, independent of the scan framing).
