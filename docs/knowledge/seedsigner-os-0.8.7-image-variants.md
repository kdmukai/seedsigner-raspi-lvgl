# SeedSigner OS at v0.8.7: what `--dev` actually changes, and where a program can write

Facts about the v0.8.7-era SeedSigner OS image that are load-bearing for anything that has to run
on it and report results — a benchmark build, a field diagnostic, any on-device harness. Several of
them contradict what the later (#114 / libcamera-line) dev images do, so carrying assumptions
backwards from those images is the trap.

## `--dev` at 0.8.7 is a serial console, not SSH

The later dev images grew wifi + dropbear + a USB gadget, and on-device work there is done over
SSH. **None of that exists at v0.8.7.** Diffing `opt/pi0/` against `opt/pi0-dev/` at the tag, the
entire difference is:

- **cmdline** gains `console=tty1 console=ttyAMA0,115200` (the release image has only
  `spidev.bufsiz=131072`)
- **post-build** adds `console::respawn:-/bin/sh` and `tty1::respawn:-/bin/sh` to inittab
- `BR2_PACKAGE_PYTHON3_PY_ONLY` appears, commented out

No networking package, no ssh daemon. So reaching a 0.8.7 dev image means a **USB-TTL adapter on the
UART pins**, and `tty1` is useless because the ST7789 is not a framebuffer console.

## The release image is the *quieter* one, which matters for measurement

The release `post-build.sh` deletes init scripts the dev variant keeps:

```
S01syslogd  S02klogd  S02sysctl  S02mdev  S20seedrng  S40network  S50pigpio
```

A dev image therefore runs **pigpio, syslogd and klogd** in the background. On a single-core Pi
Zero that is real CPU competing with whatever is being measured, and it makes the dev variant a
*pessimistic* stand-in for shipped behaviour. For performance work, build the **release** variant
and give up the console — it is both the faithful configuration and the quiet one.

## `/mnt/microsd` is the running card's own boot partition

This is the part that is easy to get wrong: it is not a second card in a second slot. The rootfs
overlay ships an mdev rule matching exactly one device:

```
# etc/mdev.conf
mmcblk0p1 root:root 777 */etc/mdev/mdev.sh
```

and `mdev.sh` mounts it:

```sh
mkdir -p /mnt/microsd
mount -o sync $DEVNAME /mnt/microsd
```

`mmcblk0p1` is the **FAT partition the system just booted from**. That is how `settings.json`
persists (`Settings.SETTINGS_FILENAME = "/mnt/microsd/settings.json"` when the hostname is
`seedsigner-os`), and it is the only writable, power-cycle-surviving location on the device — the
rootfs is an initramfs embedded in the kernel and lives in RAM.

Two consequences:

- **Anything written to `/mnt/microsd` comes off the card in a normal card reader.** It is the exfil
  path for an image with no network and no console.
- **It is mounted `-o sync`**, so writes hit the card as they are made; a `flush()` + `fsync()` is
  still worth doing, but the card can be pulled between operations without a clean unmount.
- **One card moved between boards shares that partition.** Anything device-specific written there
  (a panel size, say) will be read by the *next* board unless it is keyed by something per-device.
  `Serial` from `/proc/cpuinfo` is burned into the SoC and is the stable key;
  `/proc/device-tree/model` gives the human-readable board name.

## The card is mounted AFTER the app starts, and an unmounted path still accepts writes

Two facts that combine into silent data loss for anything that persists to the card.

**The app starts first.** Busybox init runs the overlay's scripts in lexical order, so
`S02seedsigner` backgrounds `/start.sh` *before* `S10mdev` starts the daemon that mounts
`mmcblk0p1`. The coldplug block in `S10mdev` is commented out, so the mount does not even happen
at daemon start — it waits for the MMC probe's uevent. A process that reads or writes
`/mnt/microsd` during its own start-up can therefore find nothing there, and how long "nothing"
lasts is not fixed.

**And `/mnt/microsd` is writable when it is not a mount.** `mdev.sh` does `mkdir -p /mnt/microsd`
before mounting, but until that runs the path is just a directory in the RAM rootfs — and the
rootfs is an initramfs, so it is fully writable. `os.makedirs()` succeeds, every write succeeds,
`os.access(W_OK)` returns true, and the data evaporates at power-off. Nothing reports an error at
any point.

The test that actually distinguishes the two cases is `os.path.ismount("/mnt/microsd")` —
existence and writability both return true in the failure case. Anything persisting to the card
should check that the path is a mount point, and wait for it if it starts early:

```python
deadline = time.monotonic() + 20.0
while time.monotonic() < deadline and not os.path.ismount("/mnt/microsd"):
    time.sleep(0.25)
```

This is why the app's own `Settings` reads `os.path.exists(SETTINGS_FILENAME)` lazily rather than
once at import: a settings file that is not there yet is normal, not missing.

## The rootfs is in the kernel, so overlay size is RAM

`CONFIG_INITRAMFS_SOURCE` embeds the whole rootfs in the zImage. A ~30MB kernel on the card unpacks
to a few hundred MB of RAM on a 512MB board. This is why `build.sh`'s `download_app_repo()` deletes
`docs/`, `tests/`, `tools/`, `enclosures/`, `.git`, the `.po` files and so on after cloning: the
trimming is not cosmetic tidiness, it is the RAM budget. Any custom overlay has to trim the same
way.

Relatedly, `--skip-repo` skips **both** the clone and `compile_translations_and_fonts()`. A build
that stages its own app tree gets no compiled `.mo` catalogs — which is harmless if the app only
needs to fall back to English, because `gettext.bindtextdomain()` against a missing directory does
not raise and `_()` then returns the msgid. Default fonts (`OpenSans-Regular`,
`Inconsolata-Regular`) live in `src/seedsigner/resources/fonts/` in the app repo itself, **not** in
the translations submodule, so text still renders without it.

## There is no real-time clock

No RTC, and on a release image no network to learn the time from. `time.time()` returns whatever the
kernel started at, identically on every boot. Anything that needs to order events across runs or
across boards must carry its own counter — deriving it from records already stored on the card is
what survives the card being moved and the device being power-cycled.

## `spidev.bufsiz` is set for pi0 and not for pi02w

`opt/pi0/board/boot_cmdline.txt` is `spidev.bufsiz=131072`; `opt/pi02w/board/boot_cmdline.txt` is
**empty**. The Python ST7789 driver at this tag builds the whole framebuffer and hands it over in
one call:

```python
arr = array.array("H", Image.convert("BGR;16").tobytes())
arr.byteswap()
pix = arr.tobytes()          # 240*240*2 = 115,200 bytes
self._spi.writebytes2(pix)
```

`writebytes2` is the spidev entry point that accepts a buffer larger than the kernel's per-transfer
limit and splits it, so **the panel works on either board** — this is not a broken configuration.
What changes is the number of ioctls per frame: with a 131072-byte limit the frame goes in one
transfer, with the 4096-byte default it takes roughly 29.

That is a per-board difference in display cost that has nothing to do with the SoC, which matters
for anything comparing frame rates across boards on this tag. It is invisible in the code — the
only place it is expressed is the board's `boot_cmdline.txt`.

Note this is specific to the **Python** driver. The later native display path buffers its own SPI
writes, which is why the dev-line pi02w image can ship an empty cmdline without consequence.

## Board buckets already exist at 0.8.7

`opt/` at the tag already contains `pi0`, `pi02w`, `pi2`, `pi4` (and a `-dev` variant of each), and
`build.sh --pi02w` is documented as "Build for pi02w and pi3". A Pi Zero 2 W baseline at this tag
needs no new board support. The images are **single-FAT-partition** in both the release variant
(`post-image-seedsigner.sh` builds one `type=c` partition by hand) and the dev variant
(`genimage-rpi-seedsigner.cfg` declares one `partition boot`), which is what makes them eligible for
`opt/combine-multiboard.sh` — that script assembles per-board images into one `os_prefix`-routed
card and accepts a subset via `MULTIBOARD_BOARDS="pi0 pi02w"`.
