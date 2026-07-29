# TODO: slim the pre-baked Buildroot tree with a validated allowlist

**Status:** designed, not attempted. `docker/build_prebake_image.sh --slim` refuses on
purpose until the validation below has actually been run.

**DO THIS BEFORE PUSHING ANYTHING, AND BEFORE BUILDING pi02w.** Sequencing reversed
from the original plan, for one reason: the byte-comparison baseline is **perishable**.
It requires a payload built on the full pi0 tree, that tree exists right now, and
`build.sh` clears the build dir per board — so building pi02w destroys it and
recreating the baseline costs another ~90 min.

## State when this was written (2026-07-29)

- Full pi0 tree present at `.tmp/ss-os-devimg/output` — 16 G by `du`, 169 build dirs,
  all four reachbacks verified present.
- `prebake-pi0:ss-os-v0.8.7-19-gae9288f` baked locally and content-verified
  (labels, provenance, `target/opt` stripped, symlinks resolve). **NOT pushed.**
- **Measured image size: 21.6 GB**, not the ~16 GB originally assumed (Docker's layer
  representation vs `du`, which counts Buildroot's many hardlinks once). This is what
  moved CI peak disk from ~32 GB to ~38 GB and made slimming urgent rather than nice.
- Host disk at bake time: 42 G free of 279 G. The pi0 image must be removed before
  the pi02w leg fits.

### Baseline hashes — full (non-incremental) pi0 build

```
zImage          3df7bfe1cbfdccc6c96a09b692e0edbd1457eb60c8da71c2c8901f7966a5bd31
rootfs.cpio     8d82d69f034bd9e1629a70d8c05cdd13eb720e595a4aae0af9745655c33e923c
rootfs.cpio.gz  14d94cfa4c2702c2e336caf04c2e025f2625f1a708bf0ba3f857301a5a7ca0b5
pi0-dev.img     4269ff3a1b4736553c7029a003cfb68ccab43ff623ecadab294b3af02f12778e
```

The authoritative comparison is **incremental-on-full vs incremental-on-slim**, since
that is what CI does. The hashes above are the full-build reference; an
`INCREMENTAL=1` run on the full tree supersedes them as the baseline (and comparing
the two is itself a useful check that incremental reproduces a full build, given
`BR2_REPRODUCIBLE=y`).

## Why bother

Not image size for its own sake. The binding constraint is **peak disk in CI**: the
restore step holds the image layers *and* the extracted copy at the same time. At
21.6 GB + ~16 GB that is ~38 GB on a hosted runner whose free space is unmeasured —
the most likely failure in the whole workflow. Halving the tree turns the tightest
constraint into a non-issue, and cuts the push from 21.6 GB to a few GB.

**Fallback if slimming stalls but CI still runs out of disk:** run the build *inside* a
container derived from the prebake image, so `/output` is the image's own
copy-on-write layer and the ~16 GB `docker cp` disappears entirely (peak ≈ image size
alone). Needs the compose override to stop binding `./output`, so it is a script
change — cheaper than an allowlist, but it forgoes the smaller push.

## Why a naive trim fails

Deleting every package build directory while keeping the `.stamp_*` files (so
Buildroot still considers each package built) shrinks `output/` from ~16 GB to
**4.3 GB** — and the build then fails in ~20 s at `target-finalize`, *after* the
expensive part:

```
grep: /output/build/busybox-1.36.1/.config: No such file or directory
python3.12: can't open '/output/build/python3-3.12.10/Lib/compileall.py'
make[1]: *** [target-finalize] Error 2
```

`output/build` is not inert scratch. Details:
`docs/knowledge/dev-image-incremental-rebuild.md`.

## The allowlist

Keep all `.stamp_*` files, plus:

| Path | Needed by | Failure mode if dropped |
|---|---|---|
| `build/linux-custom/` **entire** | the kernel relink itself; `post-image-seedsigner.sh` runs `scripts/dtc/dtc` and `cpp` over `arch/arm/boot/dts/overlays` with `-I include` | loud |
| `build/python3-*/Lib/` (dir must exist — `post-build.sh` symlinks `python3-3.10.10` → it) | `compileall.py`, run by the **host** interpreter by path | loud |
| `build/openssh-*/scp` | `post-build.sh` scp fix-up | **SILENT** |
| `build/busybox-*/.config` | busybox `target-finalize` | loud |

`build/linux-custom/` is the non-negotiable one and almost certainly the largest
single item: the incremental path *relinks the kernel* because
`CONFIG_INITRAMFS_SOURCE` embeds the rootfs in the zImage, and a kernel cannot be
relinked without its full build tree (sources plus every `.o`).

Everything else in `build/` — boost, libcamera, gdb, gnupg2, mc, tmux, … — looks
droppable. Expect somewhere around 6–7 GB versus 16 GB, extrapolating from the
4.3 GB "all build dirs deleted" datapoint plus a kernel tree. **Not measured.**

## The silent one, in detail

`<board>-dev/board/post-build.sh`:

```sh
OSSH_SCP=$(ls -d ${BUILD_DIR}/openssh-*/scp 2>/dev/null | head -1)
if [ -n "${OSSH_SCP}" ] && [ -f "${OSSH_SCP}" ]; then
    rm -f "${TARGET_DIR}/usr/bin/scp"
    cp -a "${OSSH_SCP}" "${TARGET_DIR}/usr/bin/scp"
fi
```

It is *guarded*, so a missing file does not fail the build — it skips the fix-up.
dropbear's `scp` then stays at `/usr/bin/scp`, and because
`BR2_PACKAGE_DROPBEAR_CLIENT` is off, **scp is broken on the device with a green
build log**. This is why exit status alone is not a valid gate, and why
`build_prebake_image.sh` hard-fails on this path even though the OS build would not.

## Validation gate — MEASURED, not proposed

**Do NOT byte-compare `rootfs.cpio`, `zImage` or the `.img`.** They are not
reproducible even with a frozen payload; see "build entropy" below. Two `SKIP_STAGE=1`
runs on the *same* tree with the *same* overlay produced different cpio and zImage
hashes.

The working gate is a **per-file hash manifest of `output/target`, excluding
`./etc/shadow`**. Measured on this tree: two identical-input builds hash **4636 files**,
the file set is identical, and **exactly one file differs — `./etc/shadow`**. So this
comparison is exact, and it is strictly better than a cpio diff: it is file-granular,
so a slimming regression names the file it broke, and it positively proves nothing else
changed.

```sh
# Generate a manifest from inside the build container (output/ is root-owned).
manifest() {   # $1 = output label
  ( cd "$OS_TREE" && docker compose run --rm -T --entrypoint bash build-images -c \
      "cd /output/target && find . -type f -print0 | sort -z | xargs -0 sha256sum \
       | grep -v ' ./etc/shadow$' > /output/manifest-$1.txt" )
}
```

Full protocol:

1. Stage once on the **full** tree and build → `manifest full`.
2. Swap in the **slim** tree, rebuild with `SKIP_STAGE=1` → `manifest slim`.
3. `diff` the two manifests. **Must be empty.**
4. Plus the two positive checks, since a guarded reachback fails silently:
   - `/usr/bin/scp` in `output/target` is OpenSSH's, not dropbear's
   - `__pycache__` present under `target/opt/src` (proves `compileall` ran)

Gates 4a/4b are belt-and-braces — a manifest match already implies them — but they are
the two failures that would otherwise be invisible, so assert them explicitly.

Remember to delete the `manifest-*.txt` files from `output/` before baking, or they
ride along into the image.

## Build entropy: two independent per-run sources

Found by running it, not by reading. Both would silently defeat a naive gate:

1. **`version.json` carries wall-clock time.** `write_versionfile.py` stamps
   `"timestamp": "2026-07-29T19:44:49"` on every `stage_app`, so the payload changes
   every run and cascades into cpio → zImage → `.img`. Fixed by `SKIP_STAGE=1`.
2. **`/etc/shadow` is re-salted every build.** `post-build.sh` runs
   `mkpasswd -m sha-256 "$ROOT_PASSWD"`, which picks a **random salt** per invocation
   (verified: two calls, same password, different hash). This runs inside *every* build,
   so `SKIP_STAGE` cannot help — it must be excluded from the comparison.

### `SKIP_STAGE=1` is mandatory for the gate

`write_versionfile.py` stamps `version.json` with **wall-clock time**:

```json
{ "name": "integration/lvgl-mpy", "short_commit_hash": "266874b",
  "timestamp": "2026-07-29T19:44:49" }
```

So re-staging changes the payload on every run, which cascades into
`rootfs.cpio` → `zImage` → `.img`. A byte-diff between two builds that each re-staged
compares clocks, not trees, and **can never pass**. (`BR2_REPRODUCIBLE=y` is set, but
the app's own timestamp defeats it above Buildroot's layer.)

`scripts/build-dev-multiboard-image.sh` therefore takes `SKIP_STAGE=1`, which reuses
the overlay already in place. Protocol:

```sh
# 1. stage ONCE, on the full tree, and build -> baseline
OS_TREE=… BOARDS_TO_BUILD=pi0 INCREMENTAL=1 \
  ./scripts/build-dev-multiboard-image.sh build pi0
sha256sum "$OS_TREE"/output/images/{rootfs.cpio,zImage}   # record

# 2. swap in the SLIM tree, do NOT re-stage, rebuild -> compare
OS_TREE=… BOARDS_TO_BUILD=pi0 INCREMENTAL=1 SKIP_STAGE=1 \
  ./scripts/build-dev-multiboard-image.sh build pi0
sha256sum "$OS_TREE"/output/images/{rootfs.cpio,zImage}   # must match step 1
```

The overlay lives in the OS tree (`opt/rootfs-overlay/opt`), not under `output/`, so
replacing `output/` while keeping the overlay is exactly the isolation this needs.

Corollary worth knowing: **CI images are not byte-reproducible run to run**, by design —
each run re-stages and gets a fresh timestamp. That is fine (provenance is recorded as
refs, not hashes) but it means image hashes are not a valid CI change-detection signal.

## How to do it non-destructively

A multi-stage Dockerfile: stage A carries the full tree, stage B `COPY --from=A`
only the allowlisted paths. Image B is genuinely smaller (a `RUN rm` would not
shrink it — the bytes stay in the lower layer), and the full image is untouched, so
both are available for the byte-comparison.

Iterate **locally**, not in CI: the allowlist will need a few passes, and burning
runner time on it is both slow and the resource profile we are deliberately avoiding.

## Per-board caveat

The reachbacks above were read from **`pi0-dev`**. `pi02w-dev` has its own
`post-build.sh` / `post-image-seedsigner.sh`, and pi0-dev's header says it was
"derived from `../pi02w/board/post-build.sh`" — so they may have drifted. Re-read
both hooks per board and validate the allowlist per board; do not assume symmetry.

## Wiring already in place

- `docker/build_prebake_image.sh` takes `--slim` and currently refuses, pointing here.
- Its `check_reachback` gates already encode every path in the table above, so a
  slim tree missing one is caught at bake time rather than mid-build.
