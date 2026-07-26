#!/usr/bin/env python3
"""SeedSigner+ 320x240 centered-square scan/entropy render check (RASPI-7 / RASPI-8).

Proves the piece no host/desktop test can: that on a WIDE 320x240 panel the live
camera preview renders as a CENTERED 240x240 square with static black L/R pillars and
the overlay chrome centered over the square — and that the native camera engine feeds
that square sink without the non-square heap overflow RASPI-8 closes.

Runs a raspi-side 320x240 init directly (RASPI-12's native display-resolution API), so
it needs NO app-side resolution setting (APP-26) — it stands alone on the Plus.

    # On the SeedSigner+ (192.168.1.96). Stop the app first (it owns display/GPIO/camera):
    seedsigner stop
    cd /mnt/data/seedsigner-raspi-lvgl && PYTHONPATH=src python3 -u tests/pi_plus_320_scan_test.py
    #   synthetic geometry check (no camera needed) — the default
    # ...or drive the LIVE camera engine (needs the camera):
    PYTHONPATH=src python3 -u tests/pi_plus_320_scan_test.py --scan

WATCH THE DISPLAY AND CONFIRM
-----------------------------
synthetic (default):
  1. A 240x240 block of 8 vertical color bars (R G B Y C M W K, left->right) sits
     HORIZONTALLY CENTERED, with a ~40 px BLACK pillar on the LEFT and RIGHT.
     - Bars left-justified / filling the full width  -> RASPI-7a not applied.
     - red<->blue swapped / byte-garbled             -> frame byte order (unrelated).
     - Right ~80 px column of power-on GARBAGE        -> full-extent init clear missing.
  2. The overlay instruction line + progress bar are centered over the SQUARE (not the
     panel) and the status bar cycles 10%..100% with the green/gray dot.
  3. Joystick LEFT (or Ctrl+C) exits.

--scan (live camera):
  1. The live camera preview is the SAME centered 240x240 square + black pillars.
  2. Hold a QR up: it decodes (a result prints) — the engine->square-sink->panel path
     is correct on a wide panel. A crash / "free(): corrupted" / SIGSEGV here is the
     RASPI-8 overflow (it should NOT happen — the square sink keeps the convert square).
  3. Joystick LEFT (or Ctrl+C) exits.
"""
from __future__ import annotations

import struct
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
SRC_DIR = REPO_ROOT / "src"
if str(SRC_DIR) not in sys.path:
    sys.path.insert(0, str(SRC_DIR))

import seedsigner_lvgl_screens as lv

# SeedSigner+ landscape panel. The sink is the SHORT-dim square; the flanking columns
# are static black pillars the portable overlay paints (RASPI-7a).
DW, DH = 320, 240
SIDE = min(DW, DH)          # 240 — the centered-square sink the frame must match
PUMP_MS = 100

BARS = [
    (0xFF, 0x00, 0x00),  # red
    (0x00, 0xFF, 0x00),  # green
    (0x00, 0x00, 0xFF),  # blue
    (0xFF, 0xFF, 0x00),  # yellow
    (0x00, 0xFF, 0xFF),  # cyan
    (0xFF, 0x00, 0xFF),  # magenta
    (0xFF, 0xFF, 0xFF),  # white
    (0x00, 0x00, 0x00),  # black
]


def _rgb565_le(r: int, g: int, b: int) -> bytes:
    """One pixel as LVGL-native RGB565, little-endian (NOT pre-swapped for the panel)."""
    v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    return struct.pack("<H", v)


def _make_color_bar_frame() -> bytes:
    """A SIDE x SIDE square frame — the exact size the centered-square sink expects."""
    bar_w = SIDE // len(BARS)
    row = bytearray()
    for x in range(SIDE):
        idx = min(x // bar_w, len(BARS) - 1)
        row += _rgb565_le(*BARS[idx])
    return bytes(row) * SIDE


def _init_320() -> None:
    print(f"[plus320] init LVGL + native ST7789 at {DW}x{DH} (MADCTL landscape)...")
    lv.lvgl_init(hor_res=DW, ver_res=DH)
    lv.native_display_init(width=DW, height=DH)
    lv.set_flush_mode("native")
    lv.set_screensaver_timeout(0)
    w, h = lv.display_size()
    print(f"[plus320] display_size() -> {w}x{h} (expect {DW}x{DH})")
    lv.clear_result_queue()


def _run_synthetic() -> int:
    frame = _make_color_bar_frame()
    assert len(frame) == SIDE * SIDE * 2, len(frame)

    lv.camera_preview_screen({"instructions_text": "< back  |  Scan a QR code"})
    lv.camera_preview_set_frame(frame)
    lv.lvgl_pump(duration_ms=PUMP_MS)
    print(f"[plus320] pushed a {SIDE}x{SIDE} color-bar frame.")
    print("[plus320] >>> EXPECT: centered square, BLACK pillars L/R, chrome centered on "
          "the square. Joystick LEFT or Ctrl+C to exit.")

    pct, ticks = 0, 0
    try:
        while True:
            lv.camera_preview_set_frame(frame)
            lv.lvgl_pump(duration_ms=PUMP_MS)
            ticks += 1
            if ticks % 10 == 0:
                pct = (pct + 10) % 110
                status = 1 if (pct // 10) % 2 else 2
                lv.camera_preview_set_progress(pct, status)
            ev = lv.poll_for_result()
            if ev is not None:
                print(f"[plus320]   result: {ev}")
                kind, index, _label = ev
                if kind == "button_selected" and index == 1000:  # RET_CODE__BACK_BUTTON
                    print("[plus320] back received — PASS.")
                    return 0
    finally:
        lv.camera_preview_close()
    return 0


def _run_scan(run_s: float = 90.0) -> int:
    scanner = lv.camera_scanner
    try:
        scanner.start(instructions_text="< back  |  Scan a QR code")
    except OSError as e:
        print(f"[plus320] camera bring-up failed: {e} — is a camera attached?")
        return 1
    print(f"[plus320] live scan up. >>> Hold a QR to the camera. On a decode you should see a "
          f"GREEN bar + dot on screen AND a 'DECODED' line here. Running ~{run_s:.0f}s; "
          f"joystick LEFT or Ctrl+C to exit.")
    t0 = last_hb = time.monotonic()
    payloads = 0
    try:
        while time.monotonic() - t0 < run_s:
            lv.lvgl_pump(duration_ms=PUMP_MS)

            # Drain decoded payloads exactly like the app's scan_consumer.run_scan loop.
            # A payload here is a real, complete QR read off the 480x480 decode stream
            # (unchanged by RASPI-7/8 — decode is independent of the display sink).
            while True:
                p = scanner.poll_new()
                if p is None:
                    break
                payloads += 1
                print(f"[plus320]   DECODED #{payloads}: {len(p)} bytes  {p[:56]!r}")

            # Per-frame QR classification -> drive the overlay so there is ON-SCREEN
            # feedback. The lower-level set_progress bypasses the single-QR suppression the
            # app uses (a static QR would otherwise show nothing), which is what we want for
            # a visible test signal. 1=NEW(green) 2=REPEAT(gray) 3=MISS 0=none.
            latest = lv.camera_scanner.read_status().latest
            if latest in (1, 2):
                lv.camera_preview_set_progress(100, latest)  # green/gray bar + dot

            # Heartbeat: zbar telemetry every ~3s. hits>0 is hard proof of a successful QR
            # read even if a payload was deduped; attempts>0 proves the decode worker ran.
            if time.monotonic() - last_hb >= 3.0:
                last_hb = time.monotonic()
                att, hits = scanner._debug_decode_stats()
                print(f"[plus320]   ..zbar attempts={att} hits={hits} drained_payloads={payloads}")

            ev = lv.poll_for_result()
            if ev is not None:
                kind, index, _label = ev
                if kind == "button_selected" and index == 1000:
                    print("[plus320] back received — exiting.")
                    break
        else:
            print("[plus320] time budget elapsed.")
    finally:
        att = hits = 0
        try:
            att, hits = scanner._debug_decode_stats()
        except Exception:
            pass
        scanner.stop()
    ok = payloads > 0 or hits > 0
    verdict = "PASS — QR decode confirmed" if ok else "NO DECODE (held a QR? in focus?)"
    print(f"[plus320] {verdict}: drained_payloads={payloads}, zbar hits={hits}, "
          f"attempts={att}. Clean exit (no crash) = RASPI-8 square-sink path held.")
    return 0 if ok else 2


def main() -> int:
    scan = "--scan" in sys.argv[1:]
    _init_320()
    return _run_scan() if scan else _run_synthetic()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\n[plus320] interrupted (a clean interrupt is fine; a crash is the failure).")
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
