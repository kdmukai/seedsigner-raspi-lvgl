# Camera pipeline: measured performance, and how to measure it again

What the Pi camera pipeline actually costs, on real hardware, for two builds:

- **v0.8.7** — picamera + PIL + the Python `DecodeQR` loop.
- **The current pipeline** — SeedSigner OS on libcamera (`seedsigner-os` PR #114) plus the native
  C camera and LVGL render path (`seedsigner-raspi-lvgl` PR #18).

The numbers are kept here because they are quoted elsewhere and are otherwise unreproducible: the
raw run logs are not committed, and the v0.8.7 arm needs a purpose-built image to regenerate. The
measurement *boundaries* matter as much as the values — several of these numbers mean something
different from what their names suggest, and that is the part most easily got wrong.

Boards: **Pi Zero** (240×240, ARM11 single core), **SeedSigner+** (320×240, ARM11 single core),
**Pi Zero 2 W** (240×240, A53 quad core). "Pi Zero" covers the Zero and Zero W — same BCM2835,
differing only in radios that are disabled here. The bench boards were Zero W Rev 1.1.

## Results

30 s windows, 3–4 runs per configuration, animated-UR workload, mean values.

### v0.8.7

| board | preview fps | capture fps | decode fps | ratio | decode ms | prep ms |
|---|---|---|---|---|---|---|
| Pi Zero 240² | 4.86 | 5.95 | 2.35 | 0.891 | 272.2 | 32.2 |
| SeedSigner+ 320×240 | 4.37 | 5.95 | 2.44 | 0.906 | 261.8 | 31.2 |
| Pi Zero 2 W 240² | 10.44 | 5.89 | 2.87 | 0.862 | 245.9 | 21.3 |

### Current pipeline

| board | preview fps | capture fps | decode fps | ratio | decode ms | prep ms |
|---|---|---|---|---|---|---|
| Pi Zero 240² | 15.63 | 15.63 | 5.55 | 0.919 | 157.6 | 1.41 |
| SeedSigner+ 320×240 | 15.62 | 15.63 | 5.21 | 0.923 | 169.3 | 1.40 |
| Pi Zero 2 W 240² | 15.62 | 15.62 | 9.73 | 0.853 | 84.5 | 0.25 |

Same board, across builds: **preview 1.5–3.6×**, **useful decodes/sec 2.1–3.4×**, **per-frame
pre-processing 23–85×**.

BC-UR reassembly through the native cUR (`uUR`) module costs **296 µs per part on the Pi Zero —
0.13% of a 30 s window**. It is not a bottleneck, and it is not part of the camera pipeline (see
`[[project_cur_uur_native_integration]]`); its role is to stop reassembly clawing back what the
camera pipeline gained.

## What each number actually times

- **decode ms** — one QR scan call on a frame that decoded. On the native side, `zbar_scan_image`.
  On v0.8.7, `pyzbar.decode()`. **Both builds hand zbar a 480×480 8-bit image**, so this is not
  resolution-dominated. The residual asymmetry is the channel: v0.8.7 feeds the R channel of an RGB
  frame, the native path feeds luma. That moves the *success ratio*, not the CPU cost.
- **prep ms** — turning a delivered frame into the buffer zbar consumes: a strided-Y → contiguous-Y
  copy natively, a first-channel extraction from a `(480, 480, 3)` array on v0.8.7. Timed as its own
  segment on both sides — see `pyzbar-decode-prep-split.md` for how that split is taken without
  duplicating work.
- **preview fps** — frames actually pushed to the panel. Independent of aim.
- **decode fps** — QR frames decoded per second, counted before any dedup or reassembly.
- **ratio** — the readable fraction. Aim- and focus-dependent; context, never a headline.

## Traps when reading these numbers

**v0.8.7's decode ms is a pipeline number, not a silicon number.** `pyzbar.decode()` builds and
configures a *fresh* zbar scanner on every call — roughly ten `set_config` calls to disable the
non-QR symbologies, plus image construction and teardown — much of it Python and ctypes contending
for the GIL with the preview thread. Consequence: **A53-vs-ARM11 reads 1.11× on v0.8.7 against
1.88× on the native pipeline for the same two boards.** Take the silicon ratio from the native
build. A corollary worth keeping: on v0.8.7 more cores partly cancel themselves, because the extra
capacity goes into running the preview thread more often, which raises GIL pressure on decoding.

**Whether the wide panel costs anything depends on which side the preview is bound on.**

| | preview | capture | |
|---|---|---|---|
| v0.8.7, ARM11 | 4.86 | 5.95 | render-bound — cannot keep up |
| v0.8.7, A53 | 10.44 | 5.89 | has headroom, re-renders frames |
| current pipeline, any board | 15.6 | 15.6 | capture-bound, headroom to spare |

The current pipeline absorbs 33% more pixels in spare capacity, so the SeedSigner+ panel is free
(320×240 measures identical to 240² on every metric). v0.8.7's single-core preview has no slack, so
the same pixels come off the frame rate: −10% raw, ~13% after correcting for the fact that the two
Zero boards used were not the same silicon. The native preview also stays *square* regardless of
panel width, so only the flush widens.

**Two v0.8.7 per-board settings ship asymmetrically** and ride along in its numbers:
`force_turbo` and `spidev.bufsiz` are set for the pi0 bucket and not for pi02w. That is upstream's
own configuration, so cross-board v0.8.7 comparisons are fair as *shipped-configuration*
comparisons — not as silicon ones.

## The measurement rule that decides what is worth running

Runs taken back-to-back without touching the rig repeat to **±0.3–0.6% on decode ms** and
**±2.5–3% on decode fps**. Returning to a board later is a *different physical setup*: the focus
point cannot be reproduced, and zbar's time scales with the QR's apparent module size, so decode ms
moves too. **Matching success ratios across two sittings does not demonstrate matched focus** — the
same ratio is reachable from different distance/angle combinations.

So: **an effect is only readable if it exceeds the between-setup confound.** Headline-scale results
(1.5–3.6×) clear it by an order of magnitude. Anything in the few-percent range does not, however
tight the within-group spread looks — a small gap with "no overlap" across two sittings is
measuring precision, not accuracy.

Any A/B of a flag, a cadence or a build option must therefore be **interleaved in one sitting**
(A, B, A, B) without touching the rig, so drift is shared. A control run collected in a later
session answers nothing.

Focus is the dominant per-camera confound and it moves `ratio` and decode fps while leaving decode
ms alone. Control it before a measured run, not after.

## Reproducing it

**Current pipeline** — `tests/pi_scan_bench.py` on a camera-equipped dev Pi against an instrumented
`.so`. It drives the native scan engine, samples the debug counters over fixed windows, and runs
payloads through `uUR` in-loop. Two things about it are not free choices:

- The consumer round runs at the app's own cadence — `scan_consumer.run_scan`'s
  `poll_interval_ms=20`, which the Pi runner does not override. The shipping loop is
  drain-to-empty → `read_status` → sleep; there is no pump in it, because the background pump
  thread owns rendering. The cadence matters because that round is where Python-side work competes
  for the GIL with the native decode worker.
- BC-UR reassembly belongs in the loop, because v0.8.7 does its Python equivalent inside
  `DecodeQR.add_image`. Leaving it out makes the two builds' decode *rates* measure different
  amounts of work.

**v0.8.7** — needs a purpose-built image: the app instrumented at the `0.8.7` tag, built into a
single multiboard (pi0 + pi02w) SeedSigner OS image that boots into a benchmark harness instead of
the app. The release variant is the right one to build — `--dev` at that tag keeps pigpio, syslogd
and klogd, which the release post-build strips, and that background CPU would make the baseline
pessimistic on a single core. Results are written to the card's FAT partition. The platform
constraints that shape all of this are in `seedsigner-os-0.8.7-image-variants.md`.
