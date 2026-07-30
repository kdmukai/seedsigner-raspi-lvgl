# TODO: dev-image CI

Per-merge CI that emits a flashable dev card per board. Local-dev only, pi0 + pi02w.

## Current state

**IN PROGRESS: baking the pre-baked Buildroot trees locally.** Everything else is
written and awaiting those images.

| Piece | State |
|---|---|
| `sdk-armv6`, `sdk-pi02w` on GHCR | done — public, labels verified at the registry |
| `.github/workflows/dev-image.yml` | written; YAML + read-only permissions verified |
| `docker/build_prebake_image.sh` + `Dockerfile.prebake` | written; syntax verified, gates unexercised |
| `INCREMENTAL=1` in `scripts/build-dev-multiboard-image.sh` | written, unexercised |
| lang-packs in CI | resolved — no code change needed, two workflow steps |
| `prebake-pi0`, `prebake-pi02w` on GHCR | **← in progress (local bake + push)** |
| First green run | blocked on the above |
| Tree slimming | done for pi0 — validated, 21.6 GB → 7.44 GB image |

## Shape, and why

Two jobs, one per board, each restoring a pre-baked Buildroot tree and re-running only
the payload tail (overlay copy, cpio, kernel relink) — ~70 s of OS build instead of
hours. The expensive Buildroot build happens **locally, once per OS pin**, and is
published as a GHCR image.

That split is doing two jobs at once:

- **Per-run CI compute stays small.** A repeated multi-hour job is a resource profile
  that risks the account, so the expensive build never runs on GitHub's runners.
- **No workflow needs a write scope.** Publishing is the only thing wanting
  `packages: write`, and it is off-CI, so `dev-image.yml` is `contents: read` +
  `packages: read` — matching `build.yml` and the language-packs repo's posture.

**No multiboard combine in CI.** Each job emits its own per-board `.img`, which is what
actually gets flashed. Combining stays a local convenience in
`scripts/build-dev-multiboard-image.sh`.

**One profile per job**, which sidesteps the armv6-last trap: a pi02w build in the same
workspace clears the armv6 `.so` from `src/`.

## Refs it builds from

All bot forks, all public, so `GITHUB_TOKEN` suffices:

| Repo | Ref |
|---|---|
| `kdmukAI-bot/seedsigner-os` | `integration/lvgl-dev` |
| `kdmukAI-bot/seedsigner` | `integration/lvgl-mpy` |
| `kdmukAI-bot/seedsigner-language-packs` | `main` |

Checked out as **siblings** under `$GITHUB_WORKSPACE`, mirroring the local dev layout, so
`build-dev-multiboard-image.sh` resolves `APP_TREE`/`CACHE_TREE` from its own sibling
defaults with no path overrides.

## The two keys differ on purpose

`SDK_KEY=ss-os-0.8.0-81-gbfbd791` (sysroot harvested at `bfbd791`) vs
`PREBAKE_KEY=ss-os-v0.8.7-19-gae9288f` (tree compiled from the integration tip). Each
names its own provenance. Compatibility is proven by the workflow's **staleness gate** —
it fails if any `*_defconfig` or the buildroot submodule changed between the SDK's
`ss-os-commit` label and the OS ref — not by the tags matching.

## Known risks for the first run

1. **Peak disk during restore.** The step holds the image layers (~7.5 GB) *and* the
   extracted copy (~4.9 GB) — ~12 GB, against ~38 GB before the tree was slimmed.
   Runner free space after the reclaim step is still unmeasured, but this is no longer
   the tightest constraint. `docs/knowledge/prebake-tree-slimming.md`.
2. **Restore may dominate runtime.** Pulling + extracting ~2 GB of layers could exceed
   the ~70 s build it exists to enable.
3. **Unexercised paths**: the producer's gates, the `.config` board-identity `sed`, the
   `.dockerignore` build-context handling, and `INCREMENTAL=1`.

## Open

- **pi02w's slim tree is unvalidated.** The allowlist is board-independent as written
  (both boards' hooks reach back to the same paths), but only pi0 has been through the
  manifest gate. Run it when the pi02w tree is next built — the baseline is perishable,
  so validate *before* moving on, per `docs/knowledge/prebake-tree-slimming.md`.

## Not carried over from the local script

`build-dev-multiboard-image.sh` patches `opt/build.sh` at run time (the bind-mount clean
step) via an inline Python heredoc. CI inherits this because it calls the same script. It
works, but it means a CI run mutates its seedsigner-os checkout — worth replacing with an
upstream-friendly fix if the script ever grows a real CI mode.
