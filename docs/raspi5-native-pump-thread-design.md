# RASPI-5 — Native background display-pump thread (design / prep)

**Status:** design prep, not scheduled. RASPI-5 is endgame — its only consumer is **APP-10**
(PIL display-pipeline teardown), which is itself gated on the wider PIL-GUI sunset. This doc
exists so the *hard part* (the concurrency model) is worked out before implementation pressure;
it is not an approval to build. Cross-repo status lives in `/home/kdmukai/dev/docs/cross-repo-ledger.md`
(row RASPI-5).

## 1. What "own the panel" means, and why the blended pump exists today

On the Pi there is **no background display task**. LVGL only advances when Python calls
`lvgl_pump()`, and LVGL pixels only reach the ST7789 when a flush runs. Today both are driven
from Python: the app's poll loop calls `_lv.lvgl_pump(5, 1)` under `renderer.lock`, and each
pump iteration ([`lvgl_runtime.cpp:165-198`](../native/python_bindings/lvgl_runtime.cpp#L165))
drives **three** things together — render+flush, `camera_engine_pump_consume()`, and the LVGL
keypad-indev read. The GIL is held for the entire loop (there is no `Py_BEGIN_ALLOW_THREADS`
anywhere in `native/`), so the GIL is the de-facto global lock that makes everything below safe.

RASPI-5 moves that loop into a native thread that owns the panel — mirroring the ESP32 firmware,
where `taskLVGL` runs `lv_timer_handler` continuously and the host bindings just build screens and
poll for results. When done, **every `if IS_MICROPYTHON:` branch in the app's
`lvgl_screen_runner.py` becomes the sole path** (build + poll only; no flush callback, no
`lvgl_pump`, no `renderer.lock`), which is what unblocks APP-10.

## 2. Target model — mirror the ESP32 `taskLVGL`

The ESP32 firmware does not hand-roll its task; it uses Espressif's `esp_lvgl_port` component and
layers a `run_screen()` lock-bracket + an SPSC result ring on top. The pattern to replicate:

1. **One background pump thread** = `taskLVGL`. Loop:
   `while (running) { wait/sleep; lock; lv_timer_handler(); unlock; yield; }`
   (input read happens *inside* `lv_timer_handler`; keypad read is already a pure syscall path —
   [`gpio_input.cpp:122-190`](../native/python_bindings/gpio_input.cpp#L122) — with no Python).
2. **A recursive LVGL mutex.** The lock is held around `lv_timer_handler`, and the flush callback
   plus widget event handlers run *inside* that call while the lock is already held — recursion is
   load-bearing on ESP for exactly this. On Pi it is defensive (our event-handler callbacks only
   push to the result queue, they don't re-enter LVGL), but use a recursive mutex to match the
   proven model and stay safe against future re-entrant callbacks.
3. **Tick from a delta source, not a fixed in-loop increment.** Keep the existing
   `lvgl_tick_update()` ([`lvgl_runtime.cpp:35-57`](../native/python_bindings/lvgl_runtime.cpp#L35)) —
   it reads `steady_clock` and self-corrects for actual elapsed time, so it stays accurate whether
   the pump spins fast (animation/camera) or sleeps at idle. It mutates one static (`s_last_tick_ms`),
   single-writer on the pump thread → safe. (ESP uses an independent `esp_timer` ISR because its
   task can block; our pump thread is the sole driver and ticks each iteration, so no separate
   timer thread is needed.)
4. **Every host→widget-tree call wraps its whole body in lock/unlock** — the ESP `run_screen()`
   bracket. See §4 for the full list.
5. **Flush stays inline on the pump thread under the lock** (native flush mode — see §3.1).
6. **Results cross back via an SPSC ring** the pump-thread callbacks fill and the host thread polls
   — but with real synchronization added (§3.3), which the ESP version omits.

## 3. The concurrency model (the hard part)

### 3.1 Native flush is REQUIRED, not merely preferred — the core deadlock argument

Two locks are in play across the thread boundary: the **GIL** and the **LVGL mutex**. The safety of
the whole design rests on a strict lock ordering:

- **Host binding thread** (Python): holds the GIL (it is a Python call) → acquires the LVGL lock to
  mutate widgets → releases LVGL → returns still holding the GIL. Order: **GIL → LVGL**.
- **Pump thread** (native `std::thread`): acquires the LVGL lock → runs `lv_timer_handler` →
  releases LVGL. Order: **LVGL only; it must NEVER acquire the GIL.**

Because the pump thread never takes the GIL, there is no `GIL↔LVGL` cycle, and no deadlock. That
invariant holds **only in native flush mode.** The flush dispatch
([`flush_cb`, `lvgl_runtime.cpp:59-92`](../native/python_bindings/lvgl_runtime.cpp#L59)) has two paths:

- **Native** (`native_flush_active()` → `native_flush_blit`,
  [`display_st7789.cpp:286-311`](../native/python_bindings/display_st7789.cpp#L286)): pure SPI +
  GPIO ioctls. **Zero Python, zero GIL.** ✅ compatible with the pump thread.
- **Python** (`s_flush_cb_py` → `PyGILState_Ensure`): the pump thread would, *while holding the LVGL
  lock*, acquire the GIL. Now pump = **LVGL → GIL** while host = **GIL → LVGL**: a textbook AB-BA
  deadlock. ❌

**Therefore the Python flush path is incompatible with the background pump thread and must be
disabled once the thread owns the panel.** This is the correctness reason APP-10 deletes
`_make_flush_callback` — it is not merely cleanup. Native flush is the enabler that makes a
Python-free pump body possible.

### 3.2 What must take the LVGL lock (the collision map)

Everything the GIL implicitly serialized today needs the LVGL lock once `lv_timer_handler` can run
concurrently. LVGL core is not thread-safe. Categories, all in `module.cpp`'s table:

- **~40 screen builders** — all `py_*_screen` in `screens.cpp` (`lv_obj_create` / `lv_scr_load`).
- **Runtime screen lifecycle** — `py_save_screen`, `py_restore_screen`, `py_clear_screen`, and
  especially **`py_set_resolution`** ([`lvgl_runtime.cpp:244-295`](../native/python_bindings/lvgl_runtime.cpp#L244)):
  it deletes and recreates the display and every screen — the single sharpest call to serialize.
- **Camera preview** — build/close sessions, `set_frame`/`set_frame_yuv420`, `set_progress` /
  `set_scanning` / segment forwarders, `io_test_camera_start/stop`.
- **Per-screen push companions** — `qr_display_set_frame`, `seed_address_verification_set_progress`,
  `io_test_set_capture_state`, `io_test_blit_camera`.
- **Locale** — `set_locale` / `unload_locale` swap the global role fonts (LVGL global state).
  (`discover_locale_packs` / `list_available_locales` are filesystem-only → LVGL-safe.)
- **Read-only-but-still-concurrent** — `py_get_inactive_time_ms`, `py_display_size`: they read LVGL
  state, so they must also hold the lock (a read torn by a concurrent `lv_timer_handler` is still a bug).

### 3.3 Result queue — add real synchronization

[`result_queue.cpp:52-74,148-170`](../native/python_bindings/result_queue.cpp#L52) is four plain
statics with **no mutex and no atomics**. Push runs on the **pump thread** (the
`seedsigner_lvgl_on_*` strong overrides fire from widget event handlers inside `lv_timer_handler`);
poll runs on the **app thread**. Under the background pump those are different threads mutating
`s_head`/`s_tail`/`s_count` concurrently → torn reads, lost events, count underflow, ring
corruption. The header comment states the current assumption outright ("single-threaded by design").

Fix: wrap `queue_push` / `poll_for_result` / `clear_result_queue` in a `std::mutex`. It is a leaf
lock — held only for the few statement body, never while acquiring another lock — so it introduces
no ordering constraint. (An SPSC-atomic head/tail is possible but not worth it at 16 entries and low
rates; the ESP side omits synchronization entirely and relies on SPSC discipline — the Pi should
*not* copy that gap.)

### 3.4 Reuse the two correct patterns the repo already contains — don't invent

- **Native camera engine** (`camera_engine.cpp`, `camera_entropy_engine.cpp`): already runs
  libcamera-manager-thread producers → a blit-worker `std::thread` → publish under `out_mtx` →
  `camera_engine_pump_consume()` (called on the pump thread) is the *only* step that calls
  `lv_obj_invalidate`. This already assumes the exact model RASPI-5 introduces — the template for
  "worker threads produce, only the pump-thread consume touches LVGL." **One gap the GIL used to
  cover, though:** the engine *handle* `g` is created by `start()` and `delete`d by `stop()` /
  `bringup_failed()` on the host thread, while `pump_consume()` derefs it on the pump thread. Under
  the flip those overlap (they were GIL-serialized before) → a use-after-free. Each engine now owns a
  Python-free lifecycle `std::mutex` held around ONLY the `g` pointer transitions and the whole
  `pump_consume()` body (never across the blocking libcamera teardown), ordered LVGL → engine-mutex →
  `out_mtx`. The sink-bridge (`camera_preview.cpp`) is split by locus too: `blit_rgb565` is
  pump-thread-only (under the LVGL lock via consume); `session_active` / `get_sink_dims` are
  host-thread entries that self-take the LVGL lock (they read LVGL state in the io_test-grab path).
- **Toast / overlay manager**: the portable screens library defines `overlay_manager_lock()` /
  `unlock()` as **weak no-op seams**
  ([`overlay_manager.cpp:49`](../sources/seedsigner-lvgl-screens/components/seedsigner/overlay_manager.cpp#L49))
  precisely so each platform backend supplies its own locking; `overlay_manager_show_toast`
  deep-copies the spec under that lock and defers the widget build to a dispatcher `lv_timer` on the
  LVGL loop. `py_show_toast` is already called cross-thread today (the SD-card detector runs on a
  separate Python thread). RASPI-5's job here is small: provide a **strong override** for
  `overlay_manager_lock()`/`unlock()` backed by a real mutex (the same shape by which
  `result_queue.cpp` strong-overrides the weak result callbacks). `dismiss_toast` deletes a widget
  directly and stays pump-thread-only.

The **legacy Python frame-push** path is the one camera exception that breaks: `py_camera_preview_set_frame`
memcpys into the sink and calls `lv_obj_invalidate` **on the calling Python thread**. Once the pump
runs concurrently, that invalidate races the render. It must either be retired in favour of the
native engine (the real path) or take the LVGL lock like every other mutator (§3.2). Recommend:
lock it for now, retire with the PIL sunset.

### 3.5 Signal handling moves off the pump thread

The pump loop's `PyErr_Occurred() || PyErr_CheckSignals()` check
([`lvgl_runtime.cpp:186`](../native/python_bindings/lvgl_runtime.cpp#L186)) is main-thread-only —
`PyErr_CheckSignals()` is a no-op on any non-main thread, and it needs the GIL. On the background
pump thread it must be **dropped entirely**. This is not a loss: the app's poll loop runs on the
CPython main thread, so `KeyboardInterrupt` is delivered there naturally between `poll_for_result()`
calls. The native pump thread simply never touches Python or signals.

## 4. Thread lifecycle

- **Start:** spawn the pump thread inside `lvgl_init` (analogous to ESP creating `taskLVGL` at boot).
  Set native flush mode as part of init (`native_display_init` already sets `s_use_native_flush`).
- **Stop / join:** on module teardown / interpreter shutdown, set `running = false`, wake the
  thread, and `join()` it cleanly before tearing down LVGL. (ESP never tears down in normal
  operation — its main task idles forever — but a Pi CPython extension should join to avoid a
  thread touching a half-freed display at exit.)
- **Idempotent init:** the app's `init()` becomes a cheap idempotent re-entry, matching the ESP
  binding's `initialized`-guarded `init()`.

## 5. Cadence

Governed by the pump loop's inter-iteration sleep. Simplest first cut: a fixed small sleep (~5 ms)
always — slightly more idle CPU but trivially correct. If idle CPU matters, mirror ESP's scheme:
let `lv_timer_handler`'s returned delay drive the sleep, capped at a `task_max_sleep_ms` (~500 ms)
when idle, and force a fast spin during animation/camera. Defer that optimization; ship the fixed
sleep first.

## 6. Sequencing — de-risk by adding the locks *before* the thread flip

The invasive part (the LVGL lock on ~40 bindings, the result-queue mutex, the overlay strong
override) can land and be validated **while still single-threaded**, because the locks are
uncontended no-ops under the host-driven pump. Suggested phases:

1. **Phase 1 (safe under today's model):** add the recursive LVGL mutex + wrap every mutator in §3.2;
   add the result-queue mutex; provide the `overlay_manager_lock` strong override; lock the legacy
   `set_frame` path. Ship and soak — behaviour is identical, locks are uncontended.
2. **Phase 2 (the actual flip):** spawn the background pump thread, force native flush, drop the
   signal check from the loop, stop pumping from Python. This is now a *small* change on top of a
   fully-locked base.

This staging means the risky concurrency edits are proven correct one at a time under the GIL before
concurrency is ever introduced.

## 7. App-side contract (APP-10 — cross-repo, for reference only)

Once the native thread owns the panel, the app deletes: `_make_flush_callback`, all `renderer.lock`
pumping, `_LoadingPumpThread` + `stop_loading_pump`, and the `view.py` stop-seam call; every native
flow collapses to its `IS_MICROPYTHON` branch (build + poll). The self-animating loading spinner then
advances on the native pump with no Python thread, so `run_loading_screen` collapses to a bare
builder call. **Caveat:** `renderer.lock` itself only fully disappears once **PIL is also gone** —
its whole purpose is PIL↔LVGL panel arbitration — so RASPI-5 *enables* APP-10, but the lock's final
removal stays coupled to the wider PIL sunset. (App anchor is `lvgl_screen_runner.py:904-909` and the
master TODO, **not** the adjacent `896-898` screensaver-flag TODO, which is a separate screens-repo item.)

## 8. Open decisions (for whoever picks this up)

1. **Result-queue synchronization:** plain `std::mutex` (recommended — simplest, correct) vs.
   SPSC atomics (faster, unnecessary at this scale).
2. **Recursive vs plain LVGL mutex:** recommend recursive to match ESP and stay safe against future
   re-entrant event callbacks, though today's Pi callbacks don't re-enter LVGL.
3. **Legacy `py_camera_preview_set_frame`:** lock it now vs. retire it in favour of the native engine
   (recommend lock now, retire with PIL sunset).
4. **Cadence:** fixed ~5 ms sleep (recommended first cut) vs. ESP-style adaptive delay + idle cap.

## Sources
- ESP32 model: `esp_lvgl_port.c` (recursive `lvgl_mux`, `esp_timer` tick, `taskLVGL`),
  `display_manager.cpp` (`run_screen()` bracket), `modseedsigner_bindings.c` (SPSC result ring) in
  `seedsigner-micropython-builder`.
- Pi surface: `native/python_bindings/` — `lvgl_runtime.cpp`, `display_st7789.cpp`,
  `result_queue.cpp`, `gpio_input.cpp`, `camera_engine.cpp`, `camera_preview.cpp`.
- Overlay seam: `sources/seedsigner-lvgl-screens/components/seedsigner/overlay_manager.*`.
- App-side: `seedsigner/src/seedsigner/gui/lvgl_screen_runner.py` +
  `docs/knowledge/pi-blended-display-what-a-native-flow-must-own.md`.
