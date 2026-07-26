#!/usr/bin/env bash
# Local-dev deploy to a RUNNING SeedSigner Pi (rsync over SSH).
# ============================================================================
# Targets the SeedSigner OS *dev* image (>= build #114: libcamera, cpython-312,
# glibc 2.40). That image differs from the old Buster layout this script used to
# assume:
#   - it is ROOT-ONLY (there is no `pi` user) -> SS_DEVICE is root@...
#   - the app runs from a checkout on the writable data partition,
#     /mnt/data/seedsigner/src, which the image's /start.sh prefers over the
#     baked /opt/src app (see /etc/init.d/S02seedsigner -> /start.sh).
#   - the app is launched/stopped via the `seedsigner` init helper
#     (`seedsigner {start|stop|restart|status}` = /etc/init.d/S02seedsigner).
#
# Because the app is `exec python3 main.py` with CWD = /mnt/data/seedsigner/src,
# that dir is sys.path[0]. So the native .so is deployed straight INTO it — a
# bare `import seedsigner_lvgl_screens` then resolves with no PYTHONPATH/.pth.
#
# Pushes the app's deployable payload: the app code (incl. main.py, so a bare
# #114 image is provisioned on the first run), the cross-compiled .so(s), and the
# app's already-bundled language packs ($SS_APP_DIR/src/lang-packs).
#
# It points ONLY at the app. It does NOT know the pack repo, does NOT build packs,
# and does NOT branch on signed-vs-dev — whatever the app bundled is what deploys.
# An absent/empty src/lang-packs is a valid English-only deploy, not an error.
# (Populating the app's src/lang-packs is the pack-repo/app dev flow's job:
#  `build_packs.sh --out-dir $SS_APP_DIR/src/lang-packs` from the live pack checkout.)
#
# Boot behavior (image, not this script): S02seedsigner starts the app at boot
# via start-stop-daemon with NO respawn flag, and inittab only respawns login
# shells -- so the app auto-runs at startup but a killed/crashed app stays down
# until a manual `seedsigner start` or reboot. This script stops the app before
# syncing (frees GPIO/SPI + the in-use .so) and explicitly restarts it at the end.
#
# DEV-ONLY. This is the "rsync onto a live Pi" loop for the maintainer's dev box.
# It is NOT how SeedSigner ships: SeedSigner OS bakes its own buildroot image.
#
# Config precedence (highest first): CLI args, real environment variables, then a
# .env in the repo root (see .env.example). Usage:
#   scripts/deploy-dev.sh [SSH_TARGET] [-d TARGET] [-a APP_DIR]
#   e.g.  scripts/deploy-dev.sh root@192.168.1.50
#         scripts/deploy-dev.sh 192.168.1.50        # bare host/IP -> root@ prepended
#
# Before syncing, it (re)generates the app's src/seedsigner/version.json from the
# checkout's git state the way the release build does (tools/write_versionfile.py
# with SEEDSIGNER_OS_BUILDER=1). SeedSigner OS reads version data ONLY from that
# file (never `git` at runtime) and the app sync strips .git, so without it the
# on-device splash + Settings>Version have nothing to show. Disable with
# SS_SKIP_VERSIONFILE=1.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# --- CLI args (override env AND .env, for a quick one-off target) --------------
# Positional SSH_TARGET (or -d/--device) -> SS_DEVICE; -a/--app-dir -> SS_APP_DIR.
# Exported here so the .env snapshot below carries them over a .env value.
_ss_usage() {
  cat >&2 <<EOF
Usage: $(basename "$0") [SSH_TARGET] [options]
  SSH_TARGET             device ssh target, e.g. root@192.168.1.50. A bare host or
                         IP (no "@") gets "root@" prepended (the dev image is
                         root-only). Overrides SS_DEVICE.
  -d, --device TARGET    same as the positional SSH_TARGET.
  -a, --app-dir DIR      seedsigner app checkout root. Overrides SS_APP_DIR.
  -s, --screens-dir DIR  seedsigner-lvgl-screens checkout to BUILD the .so from
                         (live/uncommitted screens source). Implies --build. Must
                         live under the dev/ tree (WS_ROOT). Overrides SS_SCREENS_DIR.
      --build            (re)build the .so via run_build.sh before deploying.
  -p, --profile PROFILE  target build profile: armv6 (default, Pi Zero/ARM1176) or
                         pi02w (Pi Zero 2 W / A53). Selects the SDK + src/<profile>/.
  -h, --help             show this help.
Anything not given on the CLI falls back to env vars, then .env (see .env.example).
EOF
}
_cli_device=""; _cli_app_dir=""; _cli_screens_dir=""; _cli_build=0; _cli_profile=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    -h|--help)    _ss_usage; exit 0 ;;
    -d|--device)  _cli_device="${2:?--device needs a value}"; shift 2 ;;
    -a|--app-dir) _cli_app_dir="${2:?--app-dir needs a value}"; shift 2 ;;
    -s|--screens-dir) _cli_screens_dir="${2:?--screens-dir needs a value}"; shift 2 ;;
    --build)      _cli_build=1; shift ;;
    -p|--profile) _cli_profile="${2:?--profile needs a value}"; shift 2 ;;
    --)           shift; break ;;
    -*)           echo "Unknown option: $1" >&2; _ss_usage; exit 2 ;;
    *)            if [ -z "$_cli_device" ]; then _cli_device="$1"; shift;
                  else echo "Unexpected extra argument: $1" >&2; _ss_usage; exit 2; fi ;;
  esac
done
if [ -n "$_cli_device" ]; then
  case "$_cli_device" in *@*) : ;; *) _cli_device="root@$_cli_device" ;; esac
  export SS_DEVICE="$_cli_device"
fi
if [ -n "$_cli_app_dir" ]; then export SS_APP_DIR="$_cli_app_dir"; fi
if [ -n "$_cli_screens_dir" ]; then export SS_SCREENS_DIR="$_cli_screens_dir"; fi
if [ "$_cli_build" = 1 ]; then export SS_BUILD=1; fi
if [ -n "$_cli_profile" ]; then export SS_PROFILE="$_cli_profile"; fi
unset _cli_device _cli_app_dir _cli_screens_dir _cli_build _cli_profile

# Load .env (repo root) if present; process env still overrides it.
#
# Sourcing alone does NOT give that precedence: a plain `VAR=value` line in .env
# overwrites an already-exported VAR, so `SS_APP_DIR=... ./deploy-dev.sh` would
# silently deploy whatever .env names instead — the wrong tree, with a successful
# -looking run. Snapshot the caller's SS_* vars and re-apply them after sourcing.
if [ -f "$REPO_ROOT/.env" ]; then
  declare -A _ss_override=()
  while IFS= read -r _k; do _ss_override["$_k"]="${!_k}"; done \
    < <(env | sed -n 's/^\(SS_[A-Za-z0-9_]*\)=.*/\1/p')
  set -a; . "$REPO_ROOT/.env"; set +a
  for _k in "${!_ss_override[@]}"; do printf -v "$_k" '%s' "${_ss_override[$_k]}"; done
  unset _ss_override _k
fi

# --- Config (env-driven) -----------------------------------------------------
: "${SS_DEVICE:?set SS_DEVICE (e.g. root@seedsigner.local) — see .env.example}"
: "${SS_APP_DIR:?set SS_APP_DIR — the seedsigner app checkout root — see .env.example}"

# --- Optional (re)build before deploy ----------------------------------------
SS_PROFILE="${SS_PROFILE:-armv6}"    # armv6 (Pi Zero/ARM1176) | pi02w (Zero 2 W/A53)
# A screens source dir (SS_SCREENS_DIR / --screens-dir) or SS_BUILD=1 / --build
# triggers a fresh build via run_build.sh, so a test deploy can pick up LIVE,
# uncommitted seedsigner-lvgl-screens changes without touching the pinned submodule.
# run_build.sh maps a HOST screens dir (must live under the dev/ tree) to the mounted
# container path. Without either, deploy uses the newest existing .so (unchanged).
if [ -n "${SS_SCREENS_DIR:-}" ] || [ "${SS_BUILD:-0}" = 1 ]; then
  echo "==> [build] run_build.sh (profile: $SS_PROFILE)${SS_SCREENS_DIR:+  (screens source: $SS_SCREENS_DIR)}"
  if [ -n "${SS_SCREENS_DIR:-}" ]; then
    TARGET_PROFILE="$SS_PROFILE" SS_SCREENS_SRC="$SS_SCREENS_DIR" "$REPO_ROOT/run_build.sh"
  else
    TARGET_PROFILE="$SS_PROFILE" "$REPO_ROOT/run_build.sh"
  fi
fi

# The .so built by this repo (default: newest in src/). The `|| true` keeps a
# missing .so from killing the script here via pipefail+set -e — the preflight
# below owns that error and prints an actionable message.
# Per-profile .so dir: armv6 at src/, other profiles at src/<profile>/ (same SOABI
# -> same filename, kept apart by directory; see docker/build_steps.sh).
if [ "$SS_PROFILE" = armv6 ]; then _SO_DIR="$REPO_ROOT/src"; else _SO_DIR="$REPO_ROOT/src/$SS_PROFILE"; fi
SS_SO_PATH="${SS_SO_PATH:-$(ls -1t "$_SO_DIR"/seedsigner_lvgl_screens*.so 2>/dev/null | head -n1 || true)}"
# Native cUR (BC-UR) `uUR` extension, if this build produced one. Optional: an
# absent uUR.so is a valid deploy — the app's helpers/ur2/decoder.py falls back
# to the pure-Python decoder when `import uUR` fails.
SS_UUR_SO_PATH="${SS_UUR_SO_PATH:-$(ls -1t "$_SO_DIR"/uUR*.so 2>/dev/null | head -n1 || true)}"
# Device-side layout (#114 dev image; override only for an odd device).
# The app dir is /start.sh's DEV_SRC and the CWD at launch, so the .so co-locates
# there by default (sys.path[0] import — no PYTHONPATH needed).
SS_DEVICE_APP_DIR="${SS_DEVICE_APP_DIR:-/mnt/data/seedsigner/src}"
SS_DEVICE_SO_DIR="${SS_DEVICE_SO_DIR:-$SS_DEVICE_APP_DIR}"
# The `seedsigner` init helper on the dev image (stop/start around the sync).
SS_CTL="${SS_CTL:-seedsigner}"

# --- Preflight ---------------------------------------------------------------
[ -n "$SS_SO_PATH" ] && [ -f "$SS_SO_PATH" ] \
  || { echo "ERROR: .so not found. Build it (./run_build.sh) or set SS_SO_PATH." >&2; exit 1; }
[ -d "$SS_APP_DIR/src/seedsigner" ] \
  || { echo "ERROR: no src/seedsigner under SS_APP_DIR=$SS_APP_DIR" >&2; exit 1; }

RSYNC=(rsync -az --exclude='__pycache__' --exclude='*.pyc' --exclude='.DS_Store')

# --- Generate version.json (release-image approach) --------------------------
# SeedSigner OS reads version data ONLY from src/seedsigner/version.json (never
# `git` at runtime), and the [1/4] app sync strips .git — so without this file the
# on-device splash + Settings>Version have nothing to show. Regenerate it from the
# checkout's git state exactly as the release build does (the app's own
# tools/write_versionfile.py; SEEDSIGNER_OS_BUILDER=1 makes version_timestamp the
# git commit time rather than a file mtime). It writes
# $SS_APP_DIR/src/seedsigner/version.json, which the [1/4] sync then carries over
# (version.json is not excluded). Best-effort — a failure here never aborts deploy.
if [ "${SS_SKIP_VERSIONFILE:-0}" = 1 ]; then
  echo "==> [prep] version.json: skipped (SS_SKIP_VERSIONFILE=1)"
elif [ -e "$SS_APP_DIR/.git" ] && [ -f "$SS_APP_DIR/tools/write_versionfile.py" ]; then
  echo "==> [prep] version.json <- git state of $SS_APP_DIR"
  if ( cd "$SS_APP_DIR" && SEEDSIGNER_OS_BUILDER=1 PYTHONPATH=src "${SS_PYTHON:-python3}" \
         tools/write_versionfile.py >/dev/null ); then
    grep -o '"name"[^,]*' "$SS_APP_DIR/src/seedsigner/version.json" 2>/dev/null | sed 's/^/    /' || true
  else
    echo "    WARN: write_versionfile.py failed — deploying without a refreshed version.json" >&2
  fi
else
  echo "==> [prep] version.json: skipped — $SS_APP_DIR is not a git checkout" >&2
  echo "           (SeedSigner OS reads version data only from version.json; on-device version may be blank)" >&2
fi

# --- Stop the app so the sync can replace the in-use .so and free GPIO/SPI ----
echo "==> [0/4] stop app  ($SS_DEVICE: $SS_CTL stop)"
ssh "$SS_DEVICE" "$SS_CTL stop" || true

# --- [1/4] app code (whole src/ -> provisions main.py on a bare image) -------
# Excludes: settings.json (preserve device-side state), VCS/build cruft, and
# lang-packs (handled explicitly in [3/4] to also clear a stale symlink).
echo "==> [1/4] app   -> $SS_DEVICE:$SS_DEVICE_APP_DIR/"
ssh "$SS_DEVICE" "mkdir -p '$SS_DEVICE_APP_DIR'"
"${RSYNC[@]}" --exclude='settings.json' --exclude='.git' --exclude='*.egg-info' \
  --exclude='lang-packs' \
  "$SS_APP_DIR/src/" "$SS_DEVICE:$SS_DEVICE_APP_DIR/"

# --- [2/4] native .so(s) co-located into the app dir (CWD import) ------------
echo "==> [2/4] .so   -> $SS_DEVICE:$SS_DEVICE_SO_DIR/  ($(basename "$SS_SO_PATH"))"
ssh "$SS_DEVICE" "mkdir -p '$SS_DEVICE_SO_DIR'"
"${RSYNC[@]}" "$SS_SO_PATH" "$SS_DEVICE:$SS_DEVICE_SO_DIR/"
if [ -n "$SS_UUR_SO_PATH" ] && [ -f "$SS_UUR_SO_PATH" ]; then
  echo "    + uUR -> $(basename "$SS_UUR_SO_PATH")"
  "${RSYNC[@]}" "$SS_UUR_SO_PATH" "$SS_DEVICE:$SS_DEVICE_SO_DIR/"
else
  echo "    (no uUR .so found — app uses the pure-Python ur2 decoder fallback)"
fi

# --- [3/4] language packs: the app reads packs at CWD-relative "lang-packs" ---
PACKS="$SS_APP_DIR/src/lang-packs"
if [ -d "$PACKS" ] && [ -n "$(ls -A "$PACKS" 2>/dev/null)" ]; then
  echo "==> [3/4] language packs <- $PACKS"
  # Deploy STRAIGHT there — no symlink. If an old deploy left a symlink, remove it
  # first (rsync would otherwise write THROUGH it into the old location).
  ssh "$SS_DEVICE" "[ -L '$SS_DEVICE_APP_DIR/lang-packs' ] && rm -f '$SS_DEVICE_APP_DIR/lang-packs' || true"
  "${RSYNC[@]}" "$PACKS"/ "$SS_DEVICE:$SS_DEVICE_APP_DIR/lang-packs/"
else
  echo "==> [3/4] no packs at $PACKS — English-only deploy (nothing to copy)"
fi

# --- [4/4] start the freshly-deployed app ------------------------------------
echo "==> [4/4] start app ($SS_DEVICE: $SS_CTL start)"
ssh "$SS_DEVICE" "$SS_CTL start"

echo ""
echo "==> Deployed and started. Handy on-device commands:"
echo "    ssh $SS_DEVICE '$SS_CTL status'   # running? (pid)"
echo "    ssh $SS_DEVICE '$SS_CTL restart'  # after a manual edit"
echo "    ssh $SS_DEVICE '$SS_CTL stop'     # stays down (no respawn) until start/reboot"
