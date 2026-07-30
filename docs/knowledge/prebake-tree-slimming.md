# Slimming the pre-baked Buildroot tree

The pre-baked dev tree published to GHCR carries an **allowlist** of `output/build`,
not the whole thing. `docker/slim_prebake_tree.sh` produces it; the numbers for pi0:

| | full tree | slim tree |
|---|---|---|
| `output/` by `du` | 16 GB | 4.5 GB |
| `output/build` by `du` | 14 GB | 2.8 GB |
| image disk usage | 21.6 GB | 7.44 GB |
| image content (push/pull) | 4.43 GB | 2.12 GB |

## Why it matters: CI peak disk, not image size

The restore step holds the image layers **and** the extracted copy at the same time,
so the tree is doubled at the high-water mark. A full tree needs ~38 GB on a hosted
runner whose free space is unmeasured; the slim tree needs ~12 GB. That is the whole
point — the push getting 2x smaller is a bonus.

`.github/workflows/dev-image.yml` asserts `org.seedsigner.tree-variant == slim` before
extracting, so a full tree fails fast with a clear message instead of an ENOSPC
partway through `docker cp`.

## `output/build` is not inert scratch

This is why it is an allowlist and not a blocklist. Deleting every package build
directory while keeping the `.stamp_*` files (so Buildroot still considers each
package built) yields a 4.3 GB tree that builds for ~20 s and then dies at
`target-finalize` — *after* the expensive part:

```
grep: /output/build/busybox-1.36.1/.config: No such file or directory
python3.12: can't open '/output/build/python3-3.12.10/Lib/compileall.py'
make[1]: *** [target-finalize] Error 2
```

### What is kept, and what opens it

Everything outside `build/` is kept whole (`host/`, `target/`, `images/`, the
top-level `.config` / `.br2-external.*` / `Makefile`, and the `staging` symlink).
Inside `build/`:

| Path | Opened by | Failure if dropped |
|---|---|---|
| `linux-custom/` **entire** | the kernel relink; `post-image-seedsigner.sh` runs `scripts/dtc/dtc` and `cpp` over `arch/arm/boot/dts/overlays` with `-I include` | loud |
| `python3-3.12.10/` | `post-build.sh` runs `Lib/compileall.py` by path with the host interpreter | loud |
| `busybox-*/` **entire** | `make oldconfig`, re-run whenever the board's config fragment is newer than `.stamp_dotconfig` | loud, **but only in CI** |
| `buildroot-fs/` | `cpio/fakeroot` **assembles `rootfs.cpio`**; also the device/user tables | loud |
| `buildroot-config/` | the kconfig binaries Buildroot re-runs to reload `.config` | loud |
| `openssh-*/scp` | `post-build.sh`'s dropbear-collision fix-up | **SILENT** |
| `busybox-*/.config` | busybox `target-finalize` | loud |
| every other `<pkg>/`, dotfiles only | `.stamp_*`, `.files-list*.txt`, `.applied_patches_list` | loud (rebuilds everything) |

`linux-custom/` is 2.5 GB — over half the slim tree, and non-negotiable:
`CONFIG_INITRAMFS_SOURCE` embeds the rootfs in the zImage, so a new payload is a new
kernel, and a kernel cannot be relinked without its full tree (sources plus every
`.o`).

`buildroot-fs/` and `buildroot-config/` are the two easy ones to miss — they carry no
version suffix and no `.stamp_*` files, so a "keep the packages I need" reading of
`build/` skips right over them. Together they are 2.5 MB and the rootfs cannot be
built without them.

Package dirs outside the allowlist are reduced to their **top-level dotfiles**, not
to `.stamp_*` alone. `.files-list*.txt` and `.applied_patches_list` cost megabytes
across all 163 packages, and the failure mode of guessing wrong is a 20-minute build
that dies at the end.

## kconfig packages need their sources, and only CI proves it

Buildroot's kconfig packages — `linux`, `busybox`, `linux-backports`, `swupdate`,
`uclibc`, `xvisor`; only the first two are in this build — are the one class whose
`.stamp_*` file has a **real** prerequisite rather than an order-only one:

```make
$(BUSYBOX_DIR)/.stamp_dotconfig: $(BUSYBOX_KCONFIG_FILE) $(BUSYBOX_KCONFIG_FRAGMENT_FILES)
```

So whenever the board's `busybox.config.fragment` is newer than the stamp, make
re-runs the dotconfig step, which calls `make oldconfig` **inside the package build
directory**. With only `.config` kept, that fails:

```
# merged configuration written to /output/build/busybox-1.36.1/.config (needs make)
make[2]: *** No rule to make target 'oldconfig'.  Stop.
make[1]: *** [package/busybox/busybox.mk:476: .../.stamp_dotconfig] Error 2
```

**This never reproduces locally.** The prebake tree is built from the OS tree in
place, so the stamps end up *newer* than the fragments and the step is skipped. In CI
the OS tree is a fresh `actions/checkout`, so every file carries the checkout time and
is newer than every stamp — the step always runs. A slim tree can therefore pass the
full local gate and still fail on the first real CI run, which is exactly what
happened.

Two consequences:

1. Keep kconfig packages **whole**, not pruned to `.config`. busybox is 47 MB.
2. **The validation gate must simulate a fresh checkout** — otherwise it does not
   exercise the path CI takes. Before each side of the comparison:
   ```sh
   find opt -maxdepth 3 -path opt/buildroot -prune -o -print0 | xargs -0 touch
   ```
   Verified harmless to the comparison: busybox rebuilds reproducibly
   (`BR2_REPRODUCIBLE=y`), so the touched baseline is byte-identical to the untouched
   one — 4635 files, no diff. It does mean CI recompiles busybox on every run, which
   is a couple of minutes and the correct behaviour: if the fragment genuinely
   changes, the rebuild picks it up instead of silently shipping a stale config.

## The silent one

`<board>-dev/board/post-build.sh`:

```sh
OSSH_SCP=$(ls -d ${BUILD_DIR}/openssh-*/scp 2>/dev/null | head -1)
if [ -n "${OSSH_SCP}" ] && [ -f "${OSSH_SCP}" ]; then
    rm -f "${TARGET_DIR}/usr/bin/scp"
    cp -a "${OSSH_SCP}" "${TARGET_DIR}/usr/bin/scp"
fi
```

dropbear always installs its own `scp` at `/usr/bin/scp`, and
`BR2_PACKAGE_DROPBEAR_CLIENT` is off, so dropbear's `scp` does not work. The fix-up
is *guarded*: a missing file skips it silently and ships a **green build with broken
scp on the device**. This is why build exit status is never a sufficient gate here,
and why `build_prebake_image.sh` hard-fails on the path even though the OS build
would not.

## The copy must run as root, inside a container

Everything under `output/` is root-owned, and `output/target`'s uid/gid map straight
into `rootfs.cpio`. A host-side `cp -a` as an unprivileged user rewrites every file
to uid 1000, and **the content-hash gate below does not notice** — it hashes bytes,
not ownership. The image would boot with a userspace owned by a nonexistent user.
So `slim_prebake_tree.sh` does the copy as root in a throwaway container with the
tree's parent bind-mounted.

`cp -a` implies `--preserve=all`, which includes `links`, so hardlinks are preserved
*within* the copy and the slim tree's `du` is comparable to the full tree's.

## The validation gate

**Do not byte-compare `rootfs.cpio`, `zImage` or the `.img`.** They are not
reproducible even with a frozen payload — see the entropy section below.

The gate is a **per-file hash manifest of `output/target`, excluding `./etc/shadow`**,
comparing an incremental build on the full tree against one on the slim tree. That is
what CI actually does, and it is strictly better than a cpio diff: file-granular, so
a regression names the file it broke, and it positively proves nothing else changed.

```sh
# From inside the build container (output/ is root-owned).
manifest() {   # $1 = output label
  ( cd "$OS_TREE" && docker compose run --rm -T --entrypoint bash build-images -c \
      "cd /output/target && find . -type f -print0 | sort -z | xargs -0 sha256sum \
       | grep -v ' ./etc/shadow$' > /output/manifest-$1.txt" )
}
```

Protocol — `SKIP_STAGE=1` is mandatory (see entropy, below), and **both** sides must
run with freshly-touched OS-tree mtimes or the gate misses the kconfig path above:

1. Touch the OS tree, build on the **full** tree, `INCREMENTAL=1` → `manifest full`.
2. Touch again, swap in the slim tree, rebuild with `INCREMENTAL=1 SKIP_STAGE=1`
   → `manifest slim`.
3. `diff` them. Must be empty.
4. Then the checks a content manifest cannot make:
   - `target/usr/bin/scp` hash-matches `build/openssh-*/scp` (the guarded fix-up ran)
   - `__pycache__` present under `target/opt/src` (`compileall` ran)
   - `find target -printf '%y %U:%G %m %P\n' | sort` matches the full-tree baseline
     (ownership/mode, which the hash manifest is blind to)

Delete the `manifest-*.txt` files from `output/` before baking or they ride into the
image.

### Measured result, pi0 (`v0.8.7-19-gae9288f`)

Both sides built with freshly-touched OS-tree mtimes: 4635 files hashed, manifest diff
**empty**; scp hash-matched OpenSSH's; 12 `__pycache__` dirs; 5878 `target/` entries
identical in type, uid:gid and mode. The slim tree is byte-for-byte equivalent to the
full one for image-building purposes.

Tree 16 GB → 4.6 GB; image 21.6 GB → 7.49 GB; pull 4.43 GB → 2.12 GB.

## Build entropy: two independent per-run sources

Both would silently defeat a naive gate:

1. **`version.json` carries wall-clock time.** `write_versionfile.py` stamps a
   `"timestamp"` on every `stage_app`, so the payload changes every run and cascades
   into cpio → zImage → `.img`. `BR2_REPRODUCIBLE=y` is set but this sits *above*
   Buildroot's layer. Fixed by `SKIP_STAGE=1`, which reuses the overlay in place.
2. **`/etc/shadow` is re-salted every build.** `post-build.sh` runs
   `mkpasswd -m sha-256`, which picks a random salt per invocation. This runs inside
   *every* build, so `SKIP_STAGE` cannot help — it must be excluded from the
   comparison.

Corollary: **CI images are not byte-reproducible run to run**, by design — each run
re-stages and gets a fresh timestamp. Provenance is recorded as refs, not hashes, so
image hashes are not a valid change-detection signal.

## Per-board

The reachback paths are **identical** in `pi0-dev` and `pi02w-dev`
(`post-build.sh` lines 53/87/96-97, `post-image-seedsigner.sh` lines 8/17), so the
allowlist is board-independent as written. The *validation* is still per-board: only
pi0 has been run through the gate above.

## The variant is marked, not inferred

`slim_prebake_tree.sh` writes `output/SS_OS_SLIM_ALLOWLIST` listing the paths it kept.
`build_prebake_image.sh` reads that to stamp `org.seedsigner.tree-variant`, and
`--slim` refuses to bake a tree without it.

Inferring the variant from the tree's shape **does not work**, and fails in the
dangerous direction. The obvious signature — "a package build dir holding its
`.stamp_*` files and nothing else" — is also true of Buildroot's virtual and
script-only packages, of which a full pi0 tree has 16:

```
host-fakedate  host-gettext  host-openssl  host-skeleton  host-zlib
ifupdown-scripts  initscripts  jpeg  openssl  skeleton  skeleton-init-common
skeleton-init-sysv  toolchain  toolchain-buildroot  urandom-scripts  zlib
```

`jpeg` and `openssl` are virtual packages resolving to `libjpeg`/`libopenssl`;
`toolchain` and `skeleton` are meta-packages; the `*-scripts` ones only install
files. None has a source tree to begin with. So the shape check reports **every**
tree as slim, and a guard built on it never fires — a full tree would sail into a CI
job that cannot fit it.

A hand-slimmed tree with no marker reads as `full` and `--slim` refuses it. That is
the safe direction: refuse rather than mislabel.

## Why slimming is a separate script from baking

`docker/slim_prebake_tree.sh` materialises the tree; `docker/build_prebake_image.sh`
bakes whatever tree it is pointed at. Keeping them separate is what makes **the tree
that gets validated the exact tree that gets shipped** — a Dockerfile that filtered
during `COPY` would ship a tree filtered by different code than the one the gate ran
against. `--slim` on the producer is therefore an *assertion* (refuse to bake a full
tree), and the variant is recorded as the `org.seedsigner.tree-variant` label plus a
line in `/output/SS_OS_PREBAKE_PROVENANCE`.
