# Developer Guide

## Local setup

```bash
git submodule update --init --recursive   # pulls seedsigner-lvgl-screens + LVGL
```

## Build

```bash
./run_build.sh
```

This produces the ARMv6 CPython extensions (`seedsigner_lvgl_screens` and `uUR`)
targeting the Pi Zero. The build cross-compiles **natively on x86** inside the SDK
container — no QEMU. Expect ~3 minutes cold; subsequent builds with a warm ccache
complete in seconds.

## Pi hardware testing

See `docs/pi-hardware-test.md`.

---

## Build details

The build uses a pre-built **cross-compile SDK image** carrying the SeedSigner OS
buildroot toolchain and the matching target sysroot, so the extension is compiled
by the device's own compiler against the device's own libraries — ABI skew with
the flashed image is structurally impossible. The image tag names the OS release
its sysroot came from. See `docs/knowledge/armv6-cross-compile-sdk.md` for the
mechanism and its constraints.

**GHCR** (`ghcr.io/kdmukai-bot/...`) is the primary registry — pulled by GitHub
Actions, GitLab CI, Forgejo CI, and local `run_build.sh` alike; the GitLab and
Codeberg registries hold secondary mirror copies. The image is built and
published **locally** (see "Rebuilding and publishing the cross-compile SDK image" below).

### Build caching

Local builds use Docker named volumes to persist caches across runs:

- **ccache** (`seedsigner-raspi-lvgl-ccache`) — compiled object cache, avoids
  recompiling unchanged translation units. Load-bearing: a cold build is ~3
  minutes, a fully cached one ~4 seconds. Every build log prints the resolved
  ccache binary, its `cache_dir`, and the hit rate — a persistent 0% hit rate
  means the cache is writing somewhere unmounted.

No venv volume: the SDK image already carries setuptools and pytest.

To reset the cache:
```bash
docker volume rm seedsigner-raspi-lvgl-ccache
```

CI builds use `actions/cache` for ccache persistence across workflow runs.

### Build logs

All builds write timestamped logs to `logs/`.

### Rebuilding and publishing the cross-compile SDK image

The SDK image (definition: `docker/Dockerfile.sdk`, producer:
`docker/build_sdk_image.sh`) carries the SeedSigner OS buildroot cross toolchain
and the matching target sysroot. It is built and published locally rather than by
an automated CI job: publishing requires a registry token with write access, and
keeping that token off CI runners avoids exposing it to a compromised build-time
dependency that could exfiltrate it. The image changes rarely — only when the
SeedSigner OS pin moves — so a manual local publish is a worthwhile trade.

The tag names the OS release whose sysroot it carries. **Bumping SeedSigner OS
means rebuilding the SDK, pushing the new tag, and updating `IMAGE_TAG` in
`run_build.sh` plus all three CI configs in one commit.** Publish to all three
registries so every consumer (and mirror) stays in sync:

| Registry | Role |
|----------|------|
| `ghcr.io/kdmukai-bot/seedsigner-raspi-lvgl/sdk-<profile>:ss-os-<describe>` | primary — pulled by all CI configs and local `run_build.sh` |
| `registry.gitlab.com/kdmukai-bot/seedsigner-raspi-lvgl/sdk-<profile>:ss-os-<describe>` | mirror (fallback via `IMAGE_TAG=` override) |
| `codeberg.org/kdmukai-bot/seedsigner-raspi-lvgl/sdk-<profile>:ss-os-<describe>` | mirror (kept in sync across the bot's forges) |

**There is one image per target profile**, selected by `TARGET_PROFILE`
(`armv6` → `sdk-armv6`, `pi02w` → `sdk-pi02w`), and a multi-board image build
needs **both** published. The two are near-indistinguishable from the outside —
same `arm-Buildroot-linux-gnueabihf` tuple, same SOABI, same artifact filename,
same OS pin and therefore the same tag — so the only reliable discriminators are
the `org.seedsigner.target-profile` label and the sysroot's `Tag_CPU_arch`
(`v6KZ` for armv6, `v8` for pi02w). `build_sdk_image.sh` reads that arch out of
the extracted sysroot and refuses to build if it disagrees with the requested
profile, which is what stops the common mistake: `SS_OS_CONTAINER` holds
whichever board was built **last**, so asking for `pi02w` right after a pi0 OS
build would otherwise package an ARMv6 sysroot under the pi02w tag and fail only
at `dlopen` on the device.

Verify a built or pulled image before trusting it:

```bash
IMG=ghcr.io/kdmukai-bot/seedsigner-raspi-lvgl/sdk-pi02w:ss-os-0.8.0-81-gbfbd791
docker inspect "$IMG" --format '{{index .Config.Labels "org.seedsigner.target-profile"}}'
docker run --rm --entrypoint sh "$IMG" -c \
  '/output/host/bin/arm-Buildroot-linux-gnueabihf-readelf -A \
     /output/host/arm-Buildroot-linux-gnueabihf/sysroot/lib/libc.so.6 \
   | sed -n "s/.*Tag_CPU_arch: *//p" | head -1'
```

**Creating the publish tokens (one-time).** You need one token per registry,
each scoped to *registry write only* and with a **short expiry** — they're used
rarely, so regenerate on demand rather than keeping a long-lived token around.
Store them in your password manager, never on disk. Create both as the
`kdmukAI-bot` account.

*GHCR — classic PAT:*
1. GitHub → **Settings → Developer settings → Personal access tokens → Tokens
   (classic) → Generate new token (classic)**.
2. Name it (e.g. `ghcr-base-image-publish`); set a short **Expiration** (e.g. 7 days).
3. Under **Select scopes**, check **`write:packages`** (this implies
   `read:packages`). Nothing else is needed to push to a package the account owns.
4. **Generate token** → copy the `ghp_…` value into your password manager.
   - Use a **classic** PAT — fine-grained PATs are unreliable for GHCR, and the
     token must belong to `kdmukAI-bot` (the package owner).

*GitLab — Personal Access Token:*
1. GitLab → avatar → **Edit profile → Access Tokens**
   (`gitlab.com/-/user_settings/personal_access_tokens`).
2. Name it; set an **Expiration date**.
3. Scopes: check **`write_registry`** and **`read_registry`** only.
4. **Create** → copy the `glpat-…` value into your password manager.
   - More-scoped alternative: a **project deploy token** (project → **Settings →
     Repository → Deploy tokens**) with `read_registry` + `write_registry`; it
     issues its own username + token to use at the `docker login` prompt.

*Codeberg — access token (Forgejo permission-scoped):*
1. Codeberg → **Settings → Applications → Access Tokens**
   (`codeberg.org/user/settings/applications`).
2. Name it.
3. Set **Repository and organization access** to **"Public only"** — this field is
   required (it cannot be empty). Choose "Public only", not "All (public, private
   and limited)"; public-only is least-privilege and does not gate the registry push.
4. Set the **`package`** permission to **Read and write** (this is what authorizes
   the push); leave the other permission scopes at **No Access**.
5. **Generate Token** → copy the value into your password manager.
6. Codeberg tokens have no expiry field, so **delete this token right after
   publishing** (same page) to keep it short-lived.

**1. Build locally.** Requires a SeedSigner OS buildroot output to extract from —
either a (stopped is fine) build container, or a host tree:
```bash
./docker/build_sdk_image.sh
# or point it explicitly:
SS_OS_CONTAINER=seedsigner-os-build-images-1 ./docker/build_sdk_image.sh
SS_OS_HOST_DIR=/path/to/seedsigner-os/output/host ./docker/build_sdk_image.sh
```
It reads the seedsigner-os checkout (`SS_OS_DIR`, default `../seedsigner-os`) to
stamp the image with that commit, and prints the resulting tag. It warns if the
checkout is dirty, since the provenance stamp would then not fully describe the
sysroot.

**2. Tag for all registries** (substitute the tag the producer printed):
```bash
SDK=ghcr.io/kdmukai-bot/seedsigner-raspi-lvgl/sdk-armv6:ss-os-0.8.0-81-gbfbd791
TAG="${SDK##*:}"
docker tag "$SDK" "registry.gitlab.com/kdmukai-bot/seedsigner-raspi-lvgl/sdk-armv6:${TAG}"
docker tag "$SDK" "codeberg.org/kdmukai-bot/seedsigner-raspi-lvgl/sdk-armv6:${TAG}"
```

**3. Log in and push — without leaving a token on disk or in shell history.**
Pull each token from your password manager (GHCR: a classic PAT with
`write:packages`; GitLab: a PAT or deploy token with registry write; Codeberg: an
access token with `write:package`). Paste it at
a hidden prompt and use a throwaway Docker config so nothing persists in
`~/.docker/config.json`:
```bash
export DOCKER_CONFIG="$(mktemp -d)"          # throwaway, not ~/.docker
TAG=ss-os-0.8.0-81-gbfbd791                   # the tag the producer printed

# --- GHCR ---
read -rs TOKEN && echo                        # paste GHCR PAT; hidden, not in history
printf '%s' "$TOKEN" | docker login ghcr.io -u kdmukAI-bot --password-stdin
docker push "ghcr.io/kdmukai-bot/seedsigner-raspi-lvgl/sdk-armv6:${TAG}"
docker logout ghcr.io

# --- GitLab ---
read -rs TOKEN && echo                        # paste GitLab token
printf '%s' "$TOKEN" | docker login registry.gitlab.com -u kdmukAI-bot --password-stdin
docker push "registry.gitlab.com/kdmukai-bot/seedsigner-raspi-lvgl/sdk-armv6:${TAG}"
docker logout registry.gitlab.com

# --- Codeberg ---
read -rs TOKEN && echo                        # paste Codeberg token
printf '%s' "$TOKEN" | docker login codeberg.org -u kdmukAI-bot --password-stdin
docker push "codeberg.org/kdmukai-bot/seedsigner-raspi-lvgl/sdk-armv6:${TAG}"
docker logout codeberg.org

rm -rf "$DOCKER_CONFIG"; unset DOCKER_CONFIG TOKEN TAG
```

**4. Make the GHCR package public (first publish only).** A new GHCR package
defaults to **private**, and fork PRs / unauthenticated CI runners cannot pull it —
the build then fails at `docker pull` with an auth error rather than a clear 404.
As `kdmukAI-bot`: **github.com/users/kdmukAI-bot/packages** → `sdk-<profile>` →
**Package settings** → **Danger Zone → Change visibility → Public**. This is
per-package, so `sdk-armv6` being public says nothing about `sdk-pi02w`. Verify
each with a logged-out pull:
```bash
for p in armv6 pi02w; do
  DOCKER_CONFIG="$(mktemp -d)" docker pull \
    "ghcr.io/kdmukai-bot/seedsigner-raspi-lvgl/sdk-${p}:ss-os-0.8.0-81-gbfbd791"
done
```

> **Never** `echo "<token>" | docker login …` — that writes the literal token
> into `~/.bash_history`. Always read it into a variable via `read -rs` (hidden,
> not recorded) and pipe with `--password-stdin`.

### Rebuilding and publishing the pre-baked Buildroot tree

Distinct from the SDK, and easy to confuse with it. Both are slices of the *same*
Buildroot build, published the same way, for different jobs:

| | SDK (`sdk-<profile>`) | Prebake (`prebake-<board>`) |
|---|---|---|
| Carries | `output/host` — cross toolchain + target sysroot | `output/` incl. an allowlisted `build/` |
| Size | ~1.5 GB | ~7.5 GB (~2 GB to pull) |
| Answers | "compile this C++ for ARM" | "don't rebuild 200 OS packages" |
| Produces | the `.so` | the flashable `.img` |
| Keyed per | target **profile** (arch) | **board** (board + dev defconfig) |
| Consumed by | `build.yml` **and** `dev-image.yml` | `dev-image.yml` only |

The SDK is a *subset* of the prebake, kept separate because every PR pulls the SDK
and nobody wants a multi-GB pull for a `.so` build.

`dev-image.yml` restores the prebaked tree and re-runs only the payload tail —
overlay copy, cpio, kernel relink — which is ~70 s per board instead of hours. That
is what keeps per-run CI compute small; the expensive build happens here, locally,
only when the seedsigner-os pin moves.

Baking is two steps, because the tree that gets published is an allowlisted **slim**
copy and it has to be validated by a real build before it ships:

```bash
# 1. materialise the slim tree alongside the full one (~16 GB -> ~4.5 GB).
#    Runs the copy as root in a container: output/ is root-owned and target/'s
#    uid/gid map into rootfs.cpio.
SS_OS_OUTPUT_DIR=/path/to/seedsigner-os/output ./docker/slim_prebake_tree.sh

# 2. validate it, then bake from the tree you validated. --slim refuses a full tree.
#    Validation protocol: docs/knowledge/prebake-tree-slimming.md
BOARD=pi0 SS_OS_OUTPUT_DIR=/path/to/seedsigner-os/output.slim \
  ./docker/build_prebake_image.sh --slim
```

Slimming cuts CI peak disk from ~38 GB to ~12 GB, which is the point — the restore
step holds the layers *and* the extracted copy at once. The variant is recorded as the
`org.seedsigner.tree-variant` label, and `dev-image.yml` refuses anything but `slim`.

The producer refuses rather than publishing a subtly wrong tree. It gates on:

- **board + dev identity** — `BR2_ROOTFS_POST_BUILD_SCRIPT` in `output/.config` names
  the board dir literally (`../pi0-dev/board/post-build.sh`), proving both at once.
  Needed because a Buildroot tree carries no other sign of which board built it and
  the build dir is reused across boards.
- **the `build/` reachbacks** — the kernel tree (`linux-custom`, for the relink *and*
  the DT-overlay `dtc`/`cpp` pass), `busybox-*/.config`, `python3-*/Lib/compileall.py`,
  and `openssh-*/scp`. The last is **guarded** in `post-build.sh`, so its absence is
  silent: dropbear's `scp` would ship with its client disabled, breaking `scp` on the
  device with a green build. The producer hard-fails on it anyway.
- **the tree variant** — full vs slim, detected structurally and recorded on the
  image, so a consumer can tell which it is without measuring.

It also strips `output/target/opt` at bake time, so no leftover app payload can
collide with the first real one (`output/target` is additive — overlays are copied
over it and never pruned).

Push and make public exactly as for the SDK above, then set `PREBAKE_KEY` in
`.github/workflows/dev-image.yml` to the new tag.

### Submodules

The repo includes `seedsigner-lvgl-screens` as a git submodule under
`sources/seedsigner-lvgl-screens`, which itself contains LVGL as a nested
submodule (`third_party/lvgl`). The `--recursive` flag is required.

### Environment variables

| Variable | Purpose | Default |
|----------|---------|---------|
| `IMAGE_TAG` | Cross-compile SDK image | `sdk-armv6:ss-os-<describe>` tag |
| `LVGL_PERF_MONITOR` | Enable LVGL FPS/CPU overlay | `0` |
| `SEEDSIGNER_LVGL_SCREENS_DIR` | Path to lvgl-screens source | `sources/seedsigner-lvgl-screens` |
| `LVGL_ROOT` | Path to LVGL source | `sources/seedsigner-lvgl-screens/third_party/lvgl` |
| `WS_ROOT` | Workspace root for Docker mounts | auto-detected |
| `LOCK_FILE` | Version lock file | `versions.lock.toml` |
| `ABI_JSON` | ABI reference file | `docs/abi/dev-pi-abi.json` |
| `CCACHE_HOST_DIR` | Host path for ccache (CI use) | Docker named volume |

#### Examples

Enable LVGL performance monitor overlay (displays FPS/CPU on screen):
```bash
LVGL_PERF_MONITOR=1 ./run_build.sh
```

Use a locally-built SDK image instead of the GHCR one (e.g. after re-running
`./docker/build_sdk_image.sh` against a newer SeedSigner OS output):
```bash
IMAGE_TAG=ghcr.io/kdmukai-bot/seedsigner-raspi-lvgl/sdk-armv6:ss-os-<describe> ./run_build.sh
```
