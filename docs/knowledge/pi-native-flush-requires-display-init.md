# Pi native flush requires native_display_init()

On the Pi (CPython), the LVGL flush path (`flush_cb`, `lvgl_runtime.cpp`) writes to the ST7789
through the native backend (`native_flush_blit`, `display_st7789.cpp`) — pure SPI + GPIO ioctls, no
Python. That is the only flush mode compatible with the background pump thread (the Python flush path
would take the GIL under the LVGL lock and deadlock — RASPI-5 design §3.1), so the pump forces it.

`native_flush_blit` gates on `s_native.ready`: **it no-op-paints until `native_display_init()` has
brought the panel up.** `native_display_init()` opens `/dev/spidev0.0`, claims the DC/RST/BL output
lines plus the joystick/key input lines on the gpiochip, sets the SPI mode/speed, drives the hardware
reset, and runs the ST7789 init sequence. It also calls `native_input_init()` itself.

So the Pi CPython runtime init (`ensure_lvgl_runtime` in `lvgl_screen_runner.py`) calls
`lv.native_display_init()` **before** `lv.lvgl_init()`. `lvgl_init` spawns the pump thread, which
immediately starts flushing; if the backend is not up first, every flush is a silent no-op and the
panel shows uninitialized garble while the app otherwise runs normally.

**Why this is easy to miss:**

- On ESP the single `init()` entry brings the display up itself, so the Pi CPython path is the only
  one that must bring up the panel explicitly.
- A dead panel produces no error. The logs show `input init OK`, `LVGL runtime initialized`, and
  `Executing MainMenuView()`; the app reaches its main loop and responds to input. The only symptom
  is visual (blank/garbled panel), so log-only verification passes.

**Coexistence with the Python `Renderer`:** the Python `Renderer` instantiates its ST7789 driver
(`hardware/displays/ST7789.py`, `RPi.GPIO` + `spidev`) for the canvas dimensions. This is benign
alongside the native owner: `RPi.GPIO` uses `/dev/gpiomem` (direct register access), which does not
reserve the gpiochip lines the native backend requests, and only the native side drives the panel.
Retiring the Renderer's hardware-driver instantiation is part of the wider Python-display-pipeline
removal.
