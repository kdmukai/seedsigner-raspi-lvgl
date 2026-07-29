# Debugging the SeedSigner app on the Pi Zero: getting output, and reading a "freeze"

Techniques for diagnosing the app on a SeedSigner OS device, and the reasoning that
separates the three failure modes that all present identically as "it froze".

## The app produces no output as normally launched

`/start.sh` ends with a bare `exec ${PYTHON} main.py` — no redirection — and the init script
launches it with `start-stop-daemon -b`, so stdout and stderr go nowhere. There is no log
file. (`/start.sh` carries a commented-out `>> /dev/kmsg` variant.)

To capture output, stop the managed app and run it by hand:

```bash
ssh root@seedsigner.local 'seedsigner stop'
ssh root@seedsigner.local 'cd /mnt/data/seedsigner/src && \
  nohup python3 -X faulthandler -u main.py > /mnt/data/seedsigner/app.log 2>&1 &'
```

`-u` is required or logging sits in a buffer and the log looks frozen even when the app is
fine. `-X faulthandler` costs nothing and enables the thread dump below.

While running this way `seedsigner status` reports **not running** — it tracks a pidfile the
manual process never writes. Use `ps | grep main.py`. Restore the managed launch with
`seedsigner start` when finished, or the app will not come back correctly after a reboot.

## Distinguishing deadlock from starvation from a wedged syscall

All three look like a frozen UI. Three cheap probes separate them, cheapest first.

**1. Is it burning CPU?** Sample `utime` from `/proc/<pid>/stat` a few seconds apart:

```bash
read -r _ _ _ _ _ _ _ _ _ _ _ _ _ ut st rest < /proc/<pid>/stat; echo "$ut $st"
```

Rising means it is running and starved (or looping); flat means genuinely blocked. Roughly
400 jiffies per 4s wall on this single-core board is ~100% of the CPU. This one probe
usually decides the question — a deadlock cannot accumulate CPU time.

**2. What is each thread blocked on?** `/proc/<pid>/task/*/wchan` and `.../status` name the
kernel function each thread waits in — `futex_wait_queue_me` (a lock), `hrtimer_nanosleep`
(a sleep), `ppoll` (an event loop). Cheap, no tooling, but says nothing about *which code*.

**3. Python-level stacks.** With `-X faulthandler` enabled, `kill -ABRT <pid>` dumps a
traceback for **every** thread into the log. This is the probe that names the actual line.
It kills the process, so use it once the freeze is confirmed. `py-spy` is not present on the
device; `gdb` and `strace` are.

For the C layer, `gdb -p <pid> -batch -ex "thread apply all bt 12"` symbolises libcamera,
libzbar, libstdc++ and this repo's `.so` well enough to tell an idle worker
(`pthread_cond_wait`) from a busy one, which is usually the question.

## A missing LVGL pump looks exactly like a camera failure

The most misleading failure mode on this platform. On the Pi, LVGL only advances when Python
calls `lvgl_pump()` — there is no native display task (that is RASPI-5). **Three separate
things hang off that pump**, so a drive loop that forgets it breaks all three at once:

1. Rendering and flush — nothing repaints the panel.
2. **Camera preview** — `camera_engine_pump_consume()` is called from `lvgl_runtime_pump`. It
   is the only place an engine frame reaches the preview sink, so without a pump the capture
   and blit workers run perfectly and their output is discarded. The camera looks broken
   while being entirely healthy.
3. Input — LVGL reads the input device in its timer handler, so no button registers and the
   screen cannot be cancelled, which makes the freeze look terminal.

The tell: high CPU, a main thread sleeping in Python rather than blocked on a lock, and
engine workers in normal idle waits. That combination means frames are being produced and
nobody is consuming them — look at the drive loop, not the camera.

## Device access quirks

- **`rsync`, not `scp`.** The image ships no `sftp-server` and modern `scp` defaults to the
  SFTP protocol, so `scp` fails with "Connection closed". `scripts/deploy-dev.sh` uses rsync
  for this reason; `scp -O` also works.
- **Stop the app before running any on-device harness that calls `ensure_lvgl_runtime()`** or
  `native_display_init()`. GPIO lines are exclusive: a second process fails with
  `GPIO_GET_LINEHANDLE_IOCTL(input) failed pin=6 errno=16` (EBUSY). This is easy to misread
  as a bug in the thing being tested.
- **The SSH host key regenerates on EVERY boot, not just on reflash**, so every power-cycle trips
  `REMOTE HOST IDENTIFICATION HAS CHANGED` under strict checking (it is not a reflash and not a
  MITM). Verified by fingerprint across a deliberate reboot, on two different boards.

  The dev image *tries* to prevent this and does not succeed: the rootfs is a RAM-resident
  initramfs, and `S30devdata` symlinks `/etc/dropbear` -> `/mnt/data/etc/dropbear` so
  `dropbear -R`'s generated keys land on the persistent data partition. In practice `/etc/dropbear`
  is still a real directory at runtime and `/mnt/data/etc` never appears. The `/root` bind-mount in
  the *same* `if` block does work, so the block is running — the dropbear branch specifically is not
  achieving persistence. Cause unresolved; `ls -ld /etc/dropbear` on a booted board says immediately
  whether the symlink took.

  Don't chase individual keys; exempt the dev boards once in `~/.ssh/config`. Each board advertises
  its own name (see the naming bullet below), so the pattern covers the family:
  ```conf
  Host seedsigner-*.local 192.168.1.9*
      User root
      StrictHostKeyChecking no
      UserKnownHostsFile /dev/null
      LogLevel ERROR
  ```
  `UserKnownHostsFile /dev/null` keeps the churning keys out of the real `known_hosts` so they
  never collide again. (Tradeoff: this drops MITM protection — fine for LAN dev boards that
  regenerate keys by design, not for production hosts.)

  `authorized_keys` is unaffected and does persist: put it on the boot (FAT) partition and
  `S30devdata` reinstalls it into `/root/.ssh/` on every boot, independent of the data partition.
- **Each dev board names itself; the system hostname is the same everywhere.** The dev image derives
  `seedsigner-<last 6 hex of the board serial>` at boot (`/usr/sbin/dev-name`, published at
  `/etc/seedsigner-devname`) and uses it for both the mDNS `.local` record and the DHCP hostname
  option, so any number of boards coexist on one LAN without a name fight. The *system* hostname
  stays `seedsigner-os` on every image, because the app keys OS-specific behavior off
  `os.uname()[1]` — so `hostname` is useless for telling two boards apart. Use the shell prompt,
  `network-info`, or from another machine:
  ```bash
  avahi-browse -rt _ssh._tcp     # every SeedSigner on the link, name -> IP
  ```
  A one-line `hostname.txt` on the boot partition overrides the serial suffix
  (`plus` -> `seedsigner-plus.local`), which is worth doing since serial hashes are unmemorable.
- No `timeout` or `pgrep` on the device (busybox); use `ps | grep '[m]ain.py'`, and bound a
  manual run with a background launch + `sleep N` + `kill` instead of `timeout`.
- No `pytest` in the device Python env, so the repo's pytest suites cannot run on-device. Run a
  native-module check as a **standalone assert script** (import, exercise, `print`/`SystemExit`)
  from the dir holding the `.so`. For a binding whose behavior only manifests with real input
  (e.g. `get_inactive_time_ms()` resetting on a keypress), verify it **through the app**: the
  app logs the observable decision (the toast `showing toast` vs `canceled before showing`),
  which is more reliable than trying to time a physical press against a headless probe window.
- **A warm `reboot` can hang this dev image — prefer `seedsigner stop`/`start` or a power-cycle to
  pick up a new `.so`.** Observed 2026-07-26: an `ssh <host> reboot` issued while the app was
  still running (GPIO/SPI/camera held; not `stop`-ped first) left **both** a Pi Zero and a
  SeedSigner+ off the network (`No route to host`) with the panel frozen on its last frame — the
  app never restarted, so nothing re-ran `native_display_init`. Both needed a physical power-cycle.
  `scripts/deploy-dev.sh` deliberately stops/starts the app and never reboots the device — mirror
  that. Root cause unconfirmed (candidates: the app holding exclusive GPIO/SPI/camera fds at reboot,
  a busy `/mnt/data` unmount stalling busybox shutdown, no watchdog to force the reset); confirming
  it needs a serial console, since a hung warm-reboot leaves nothing over the network and a
  power-cycle wipes the RAM logs. Note a power-cycle is also the only way to reproduce the
  uninitialized-GRAM garbage strip on a wide panel — a warm reboot retains GRAM.

## Capturing what the camera sees, without a screen

The entropy engine latches a raw RGB565 frame retrievable via
`camera_entropy.capture()` / `get_result()`. Pull it back and convert to PNG on the host to
inspect orientation or exposure — this is how camera rotation direction was verified
(a light source tracked across 0°/90°/180° captures proved the rotation is clockwise).

Comparing two captures is more reliable than judging one: separate captures differ by scene
and exposure noise, so an exact-match test will not score zero, but the correct rotation
scores roughly half the error of a wrong one.

## See also
- `docs/knowledge/armv6-cross-compile-sdk.md` — how the `.so` under test is built.
- `docs/interface-contract.md` — the binding surface these harnesses drive.
