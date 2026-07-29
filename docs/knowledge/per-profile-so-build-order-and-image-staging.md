# Building a two-architecture dev image: `.so` build order, and what `--skip-repo` does not do

Two traps that only appear when you build both target profiles *and* bake the result into a
SeedSigner OS image. Both fail silently — the build reports success and the damage shows up
either at `dlopen` on the wrong board or as a broken app at boot.

## 1. Build `armv6` LAST, or its `.so` is deleted

`setup.py` builds with `build_ext --inplace`, whose output lands in `src/` no matter which
profile is being built. `docker/build_steps.sh` therefore clears that staging location before
every build — **including the armv6 output, unconditionally**:

```sh
if [[ "${TARGET_PROFILE:-armv6}" == "armv6" ]]; then OUT_DIR="${ROOT_DIR}/src"; else OUT_DIR="${ROOT_DIR}/src/${TARGET_PROFILE}"; fi
rm -f "${OUT_DIR}"/seedsigner_lvgl_screens*.so "${OUT_DIR}"/uUR*.so
rm -f "${ROOT_DIR}"/src/seedsigner_lvgl_screens*.so "${ROOT_DIR}"/src/uUR*.so   # <-- always
```

A non-armv6 profile then *moves* `src/*.so` into `src/<profile>/`. So:

```sh
TARGET_PROFILE=armv6 ./run_build.sh    # writes src/*.so
TARGET_PROFILE=pi02w ./run_build.sh    # second rm -f wipes src/*.so, then fills src/pi02w/
# -> src/ is now EMPTY. Both builds reported success.
```

Reverse the order (`pi02w` then `armv6`) and both survive: the armv6 run clears only `src/`,
which `src/pi02w/` is not under. The rule is **armv6 last**, or rebuild it after any other
profile. Nothing warns you — the armv6 `.so` is simply gone, and a stage step that globs for
"the newest `.so`" either finds nothing or, worse, an older one left from a previous session.

Both profiles emit the **same filename** (`seedsigner_lvgl_screens.cpython-312-arm-linux-gnueabihf.so`)
because they share the SOABI — the 32-bit armhf tuple and Python 3.12 are identical, only
codegen differs. Directory is the only thing keeping them apart, so a filename check can never
tell you which one you have. Check the ELF instead:

```sh
readelf -A <so> | sed -n 's/.*Tag_CPU_arch: *//p' | head -1
# v6KZ -> ARM1176 (pi0 / Zero / Zero W)      v8 -> Cortex-A53 (pi02w / Zero 2 W)
```

Staging the wrong one produces an `ImportError` on the device long after the build, with
nothing pointing back at the mix-up. Assert the arch at stage time.

## 2. `--skip-repo` skips the trimming and `version.json` too

On the seedsigner-os branch carrying the dev images (`feat/unified-multiboard-image`, i.e.
the #114 lineage), `opt/build.sh` keeps the space-trimming *and* the version-file write inside
`download_app_repo()` — the function `--skip-repo` exists to skip. So a `--skip-repo` build
ships **whatever the overlay contains**, untrimmed and with no `version.json`.

That matters more than it sounds, because the rootfs is embedded in the kernel
(`CONFIG_INITRAMFS_SOURCE`) and is resident in RAM on a 512 MB board. Staging a maintainer's
checkout by copying the directory put **319 MB** in the overlay where a clone-equivalent tree
is **30 MB**: 112 MB of `.git`, plus `tests/`, `docs/`, `tools/`, and ~13 MB of untracked
local scratch (translation working dirs, PR extracts, scratch notes) that a clone would never
have contained.

Note this is a **branch-dependent** hazard. Upstream `main` (and the multiboard-release
lineage) has since split these into `write_version_json()` / `delete_unnecessary_files()`,
which run unconditionally *precisely* so `--skip-repo` builds get the same treatment. Reading
`build.sh` from the wrong checkout tells you the opposite of what your build will do — read it
from the tree you are actually building.

What a `--skip-repo` stage has to do itself, in this order:

1. Copy **tracked files only** — `git ls-files --recurse-submodules -z` piped to
   `rsync --from0 --files-from=-`. Copying the working tree drags in untracked scratch.
2. Copy `.git` anyway, temporarily: `write_versionfile.py` derives the branch/commit/timestamp
   from it. A missing or wrong `version.json` breaks the app's version screens and its
   SeedSigner-OS checks.
3. Copy `src/lang-packs` **explicitly**. It is a *built* payload (generated from the
   `seedsigner-language-packs` repo) and is deliberately untracked, so step 1 misses it
   entirely — and its absence only shows up at runtime as an app with no language packs.
4. Write `version.json` (needs `.git/` and `tools/`), *then* trim — `.git`, `tools/`, `tests/`,
   `docs/`, `.github/`, `enclosures/`, `l10n/`, `seedsigner-screenshots/`, the root packaging
   files, and the translation `.po` sources.

The unsubsetted Noto CJK fonts under `resources/seedsigner-translations/fonts` (~22 MB) are
what `compile_translations_and_fonts()` would shrink, and `--skip-repo` does not run that
either. They stay, because `gui/constants.py` maps them per locale for the PIL render path;
dropping them would silently break non-Latin locales. Budget the 22 MB rather than delete it.

## See also
- `armv6-cross-compile-sdk.md` — the per-profile SDK images and how the OS pin is shared.
- `scripts/build-dev-multiboard-image.sh` — the staging/build/combine path these notes describe.
