#!/usr/bin/env python3
"""RASPI-5 Phase 2 — camera-engine lifecycle stress (the g use-after-free check).

Run on the #114 dev Pi WITH A REAL CAMERA, against the rebuilt Phase-2 .so:

    # stop the app first (it owns the display/GPIO/camera), then:
    cd /home/pi/seedsigner-raspi-lvgl && PYTHONPATH=src python3 -u tests/pi_camera_engine_stress.py
    # optional: pass a cycle count -> tests/pi_camera_engine_stress.py 60

GENTLE ON THE FANLESS Pi ZERO
-----------------------------
The Pi Zero has no heatsink/fan, so this is tuned to sample the race, NOT to thermally
hammer the board: a small default cycle count, and an IDLE cooldown between cycles (the
engine is stopped, so the core actually cools). The race is exercised at each start/stop
TRANSITION — variety of stop-timing (the jittered holds) matters, raw cycles/sec does not
— so a few dozen cycles is plenty. Don't crank the count into the hundreds/thousands.

WHY THIS TEST EXISTS
--------------------
Phase 2 moves the LVGL pump into a background native thread that runs
camera_engine_pump_consume() GIL-free every ~5 ms. camera_scanner.start()/stop()
run on THIS (main) thread and create/`delete` the engine handle `g`. Under the old
host-driven pump the GIL serialized consume vs start/stop; under the flip they race —
a use-after-free if consume derefs a `g` that stop() is deleting. The fix is a
per-engine lifecycle std::mutex (camera_engine.cpp / camera_entropy_engine.cpp) held
around the `g` pointer transitions and the whole consume body.

A data race is invisible to the build/ABI/pytest gates — the ONLY way to validate the
fix is to hammer the race on-device. This loop starts + stops the scan engine many
times with a JITTERED hold (so stop() lands at every phase of the pump's 5 ms consume
cadence), while the background pump thread consumes frames the whole time.

The scan engine's `g` is the SAME handle io_test's grab uses, and the entropy engine
got the identical fix — so a clean run here covers the whole fix pattern.

WHAT TO EXPECT
--------------
- The display shows the live camera preview flickering as each cycle builds + tears
  down the preview screen. That churn is the point, not a bug.
- A heartbeat line every 25 cycles: cycles done, frames the pump PUBLISHED in the last
  session, and the running total. `pub` climbing across cycles is the proof the pump
  thread is actively consuming `g` while start/stop churn it — i.e. the race is really
  being exercised, not idled past.
- At the end: "PASS — survived N cycles" and a clean exit.

WHAT TO LOOK FOR  (a regression of the UAF fix manifests as ONE of these)
------------------------------------------------------------------------
- HARD FAIL: the process dies mid-run with SIGSEGV / "Segmentation fault" / bus error /
  a libc "free(): ... / corrupted" / std::terminate abort. That is the use-after-free
  (or heap corruption) firing — the fix regressed. Note the cycle number it died on.
- SOFT SMELL: total `pub` stays 0 for many cycles -> the camera never delivered frames,
  so the race isn't actually being stressed (no consume activity). Check the camera /
  that you're on a camera-equipped Pi; the test is only meaningful with pub > 0.
- SOFT SMELL: repeated "start failed" lines -> the camera isn't being released cleanly
  between cycles (a stop()/bring-up ordering problem), worth a look on its own.
- A clean "PASS" after a few hundred cycles WITH pub climbing = the lifecycle mutex holds.
"""
from __future__ import annotations

import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
SRC_DIR = REPO_ROOT / "src"
if str(SRC_DIR) not in sys.path:
    sys.path.insert(0, str(SRC_DIR))

import seedsigner_lvgl_screens as lv

W = H = 240

# Jittered holds (seconds) between start() and stop(). Deliberately NOT multiples of the
# pump's 5 ms cadence, and spanning "stop immediately after start" (tightest create/delete
# race, maybe before the first frame) through "let several frames flow" (consume actively
# deref'ing g when the delete lands). Rotated per cycle to sample the whole phase space.
# After start(), wait until the pump thread has actually PUBLISHED a frame (frames_pub > 0)
# before stopping — so stop()'s `delete g` races an ACTIVE consume (the strongest form of the
# lifecycle race), not an idle engine that never produced. Capped so a frame-less cycle can't
# stall. Then a tiny jitter varies the exact phase of the ~5 ms consume cadence stop() lands on.
FRAME_WAIT_MAX = 1.0
JITTERS = [0.0, 0.005, 0.011, 0.019, 0.031, 0.05]

# Idle gap between cycles (engine stopped). Lets the fanless Pi Zero core cool between
# libcamera bring-ups, and keeps average CPU well below a pegged loop.
COOLDOWN_S = 0.15

HEARTBEAT_EVERY = 5


def main() -> int:
    cycles = 40
    if len(sys.argv) > 1:
        try:
            cycles = int(sys.argv[1])
        except ValueError:
            print(f"[stress] bad cycle count {sys.argv[1]!r}; using {cycles}")
    if cycles > 200:
        print(f"[stress] NOTE: {cycles} cycles is a lot for a fanless Pi Zero; "
              f"consider keeping it under ~100.")

    print("[stress] init LVGL + native ST7789 (spawns the Phase-2 background pump thread)...")
    lv.lvgl_init(hor_res=W, ver_res=H)
    lv.native_display_init()
    lv.set_flush_mode("native")
    lv.set_screensaver_timeout(0)  # keep the saver out of the way during the churn
    lv.clear_result_queue()

    scanner = lv.camera_scanner

    print(f"[stress] hammering camera_scanner.start()/stop() x{cycles} "
          f"against the live pump thread. Ctrl+C to stop early.")

    started = 0            # cycles where start() actually brought the engine up
    start_failures = 0
    flowed = 0             # cycles where the pump published >=1 frame (race truly stressed)
    total_pub = 0          # frames the pump published across all sessions
    last_pub = 0
    t0 = time.monotonic()

    for i in range(1, cycles + 1):
        try:
            scanner.start(instructions_text="stress")
        except OSError as e:
            # Camera busy / bring-up failure. stop() is idempotent; back off and move on.
            start_failures += 1
            if start_failures <= 5 or start_failures % 20 == 0:
                print(f"[stress]   cycle {i}: start failed ({e}); "
                      f"{start_failures} so far")
            scanner.stop()
            time.sleep(0.05)
            continue

        started += 1
        # Wait until the pump thread has actually PUBLISHED a frame this session, so stop()
        # races an ACTIVE consume — not an engine that was torn down before it produced. Poll
        # the per-session publish counter (resets at the next start) with a small sleep so the
        # wait is not a busy-spin. Capped by FRAME_WAIT_MAX.
        t_wait = time.monotonic()
        sess_pub = 0
        while sess_pub == 0 and (time.monotonic() - t_wait) < FRAME_WAIT_MAX:
            time.sleep(0.01)
            try:
                _fin, _fconv, sess_pub = scanner._debug_stats()
            except Exception:
                break
        if sess_pub > 0:
            flowed += 1

        # Tiny jitter so stop()'s delete lands at a varied phase of the ~5 ms consume cadence.
        time.sleep(JITTERS[i % len(JITTERS)])

        # Re-read the publish count right before stop() (frames keep flowing during the jitter).
        try:
            _fin, _fconv, sess_pub = scanner._debug_stats()
        except Exception:
            pass
        last_pub = sess_pub
        total_pub += sess_pub

        # Drain any results the engine/nav pushed so the queue never saturates.
        while lv.poll_for_result() is not None:
            pass

        scanner.stop()

        if scanner.is_running():
            print(f"[stress]   !! cycle {i}: is_running() still True after stop() "
                  f"— engine not fully torn down")

        if i % HEARTBEAT_EVERY == 0:
            rate = i / max(time.monotonic() - t0, 1e-6)
            print(f"[stress]   {i:>5}/{cycles}  started={started} flowed={flowed} "
                  f"start_fail={start_failures}  last_pub={last_pub} total_pub={total_pub} "
                  f"({rate:.1f} cyc/s)")

        # Idle cooldown between cycles (engine off) — keeps the fanless Pi Zero cool.
        time.sleep(COOLDOWN_S)

    dt = time.monotonic() - t0
    print(f"[stress] done: {cycles} cycles in {dt:.1f}s (started={started}, "
          f"flowed={flowed}, start_failures={start_failures}, total_pub={total_pub})")
    if flowed == 0:
        print("[stress] WARNING: no cycle ever published a frame — the DELETE-vs-active-consume "
              "path was not exercised. Camera not delivering? (bring-up churn alone still "
              "exercised the pointer-lifecycle race, and it survived.)")
    else:
        print(f"[stress] PASS — survived {cycles} start/stop cycles; {flowed} of them tore the "
              f"engine down WHILE the pump was actively consuming (total_pub={total_pub}). "
              f"No crash/hang → the lifecycle mutex holds.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\n[stress] interrupted — (a clean interrupt is fine; a crash is the failure).")
    finally:
        try:
            lv.camera_scanner.stop()
        except Exception:
            pass
        try:
            lv.native_display_shutdown()
            lv.lvgl_shutdown()
        except Exception:
            pass
