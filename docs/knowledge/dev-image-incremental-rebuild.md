# Swapping the app/.so into a built dev image takes ~70 seconds, not hours

A SeedSigner OS image build is dominated by Buildroot compiling the toolchain and every
package. That work depends only on the seedsigner-os ref, which changes rarely. The parts
that change constantly — this repo's `.so` and the app tree — live in
`rootfs-overlay/opt`, and swapping them needs only the tail of the build.

## Measured

On a Pi Zero 2 W target, with the Buildroot tree already built:

| | wall clock |
|---|---|
| full `--dev` build, pi0 (warm ccache + dl cache) | ~90 min |
| full `--dev` build, pi02w (caches hot from the pi0 run) | ~20 min |
| **`--no-clean --skip-repo` after replacing the overlay** | **~70 s** |

Three consecutive incremental runs measured 1m10s, 1m10s, 1m07s. Only these steps run:

```
Copying overlay ../rootfs-overlay/
Copying overlay ../rootfs-overlay-dev/
Executing post-build script
Generating filesystem image rootfs.cpio
Rebuilding kernel with initramfs      <-- unavoidable
Executing post-image script
```

The kernel relink is unavoidable because `CONFIG_INITRAMFS_SOURCE` embeds the rootfs *in*
the zImage — a new payload is a new kernel. It is still only a relink plus lz4/gzip of the
cpio, not a compile.

Verified it is not a no-op: planting a sentinel file in the overlay changed the zImage
hash and the file appeared in the rebuilt `rootfs.cpio`.

## `output/target` is additive — deletions never propagate

Removing a file from the overlay does **not** remove it from the image. Buildroot builds
`output/target` up incrementally; `target-finalize` copies the overlay *over* it and never
prunes. A sentinel deleted from the overlay was still in the next `rootfs.cpio`.

So a tree reused across payloads accumulates stale files forever. Wipe `output/target/opt`
before the rebuild — and do it **as root inside the container**, because everything under
`output/` is root-owned (`post-build.sh` writes `__pycache__` as root) and a host-side
`rm -rf` dies with `Permission denied` partway through, leaving a half-cleaned tree.

For the same reason a pre-baked tree is best baked with **no app staged at all**, so the
first real payload cannot collide with a leftover one.

## A pre-baked tree cannot be trimmed naively

`output/` is ~16 GB, of which `build/` is ~14 GB. Deleting every package build directory
while keeping the `.stamp_*` files (so Buildroot still considers each package done) shrinks
it to **4.3 GB** — and then the build fails in 20 s, at `target-finalize`, after the
expensive part:

```
grep: /output/build/busybox-1.36.1/.config: No such file or directory
python3.12: can't open '/output/build/python3-3.12.10/Lib/compileall.py'
make[1]: *** [target-finalize] Error 2
```

Two separate reachbacks into build directories that are not inert scratch:

1. Buildroot's own busybox `target-finalize` step reads `build/busybox-*/.config`.
2. **This project's `<board>-dev/board/post-build.sh` runs `compileall.py` out of
   `${BUILD_DIR}/python3-3.12.10/Lib/`** to pre-generate `__pycache__`. That one is
   project-specific and will not be found by reading Buildroot's manual.

An **allowlist** does work, and is what the published trees carry — but the failure
surfaces only at the end of a build (or not at all, for the guarded `scp` fix-up), so it
is only trustworthy once run end to end against a per-file manifest of `output/target`.
The allowlist, the full set of reachbacks and that validation protocol:
`prebake-tree-slimming.md`.

## Why this matters for CI

It makes a per-merge image build affordable: publish the built `output/` tree as an
artifact keyed on *(seedsigner-os ref x buildroot submodule ref x defconfig hash)*, then
each run restores it, wipes `target/opt`, stages the fresh `.so` + app, and runs
`--no-clean --skip-repo`. ~2 min per board instead of hours, which removes both the disk
and the 6-hour-job concerns on a hosted runner.

Key on all three inputs, not just the seedsigner-os SHA — a defconfig edit with an
unchanged SHA would otherwise silently reuse a stale toolchain.

## See also
- `per-profile-so-build-order-and-image-staging.md` — what `--skip-repo` does *not* do
  (trimming, `version.json`), and the per-profile `.so` build-order trap.
- `seedsigner-os-0.8.7-image-variants.md` — why overlay size is a RAM budget.
- `scripts/build-dev-multiboard-image.sh` — the staging/build/combine path.
