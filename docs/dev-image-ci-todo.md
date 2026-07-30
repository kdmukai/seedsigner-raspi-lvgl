# TODO: dev-image CI

Per-merge CI that emits a flashable dev card per board. Local-dev only, pi0 + pi02w.

## Current state

Both boards' pre-baked trees are published and public. **pi0 is green on a hosted
runner** (run `30578590356`: 17 min, 94 MB `.img` artifact). The pi02w leg is enabled
but has not yet run in CI — its tree passed the local validation gate.

| Piece | State |
|---|---|
| `sdk-armv6`, `sdk-pi02w` on GHCR | public, labels verified at the registry |
| `prebake-pi0`, `prebake-pi02w` on GHCR | public, slim, `tree-variant` labelled |
| `.github/workflows/dev-image.yml` | pi0 leg proven green; pi02w leg unexercised |
| `docker/build_prebake_image.sh` + `Dockerfile.prebake` | exercised on both boards |
| `docker/slim_prebake_tree.sh` | exercised and gated on both boards |
| `INCREMENTAL=1` in `scripts/build-dev-multiboard-image.sh` | exercised, locally and in CI |
| lang-packs in CI | two workflow steps, no code change |

## Shape, and why

Two jobs, one per board, each restoring a pre-baked Buildroot tree and re-running only
the payload tail (overlay copy, cpio, kernel relink) instead of a ~90 min Buildroot
build. The expensive build happens **locally, once per OS pin**, and is published as a
GHCR image.

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

That gate needs the SDK's commit to be *present*, and `actions/checkout` is shallow by
default, so the step fetches that single commit first. Deepening the whole clone would
drag buildroot's full history in for one object lookup.

## Measured on the runner

Peak disk is the restore step, which holds the image layers and the extracted copy at
once: **~6 GB net, against 113 GB free** after the reclaim step. The runner is 145 GB,
considerably larger than the ~30 GB this was originally designed against — so peak disk
turned out not to be the binding constraint it was treated as. Slimming still earns its
place on pull size (4.43 → 2.12 GB) and restore time.

## Open

- **pi02w has never run in CI.** Its tree passed the local gate, but the pi0 leg is the
  only one proven end to end on a runner.
- **`build-dev-multiboard-image.sh` patches `opt/build.sh` at run time** (the bind-mount
  clean step) via an inline Python heredoc. CI inherits this because it calls the same
  script, so a CI run mutates its seedsigner-os checkout — worth replacing with an
  upstream-friendly fix if the script ever grows a real CI mode.
- **Re-baking when the OS pin moves** means both boards, and the trees cannot coexist:
  `build.sh` clears the build dir per board. Validate each before moving on, since the
  baseline is destroyed by the next board's build.
