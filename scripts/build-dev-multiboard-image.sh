#!/bin/bash
# =============================================================================
# build-dev-multiboard-image.sh
#
# Build ONE flashable dev card that boots both the Pi Zero / Zero W (armv6
# bucket, ARM1176) and the Pi Zero 2 W (pi02w bucket, A53), carrying:
#
#   - the SeedSigner OS #114 dev baseline (libcamera, CPython 3.12, glibc 2.40)
#   - this repo's native LVGL + camera extension, per-board .so
#   - the seedsigner app tree from a local checkout
#   - the unique per-board dev hostname (rootfs-overlay-dev/usr/sbin/dev-name)
#
# The two per-board images are compiled separately -- different architectures,
# sharing nothing but the app tree -- then assembled by combine-multiboard.sh
# into a single image whose config.txt routes each Pi model to its own os_prefix
# subdirectory. Only the board's own kernel is ever read, so RAM use matches a
# single-board image; the other bucket costs card space, not memory.
#
# Each bucket gets its OWN .so: the rootfs is embedded in that bucket's zImage
# (CONFIG_INITRAMFS_SOURCE), so the staging step is re-run per board and the
# armv6 and pi02w builds never see each other's binaries.
#
# INPUTS
#   APP_TREE  seedsigner checkout (default: the sibling repo, whatever is checked out)
#   OS_TREE   seedsigner-os worktree carrying the dev-hostname + dev-combine work
#
# The app is staged into the OS tree's rootfs overlay and built with --skip-repo,
# so the image contains exactly the local tree -- no clone, no network. The
# checkout's .git and tools/ are staged too because build.sh's write_version_json
# needs them; it deletes both before the initramfs is packed, exactly as it does
# for a cloned repo.
#
# USAGE
#   scripts/build-dev-multiboard-image.sh                 # stage+build both, combine
#   scripts/build-dev-multiboard-image.sh stage pi0       # refresh one overlay only
#   scripts/build-dev-multiboard-image.sh build pi02w     # one board
#   scripts/build-dev-multiboard-image.sh combine         # assemble existing images
# =============================================================================

set -o errexit -o nounset -o pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

APP_TREE="${APP_TREE:-/home/kdmukai/dev/seedsigner}"
OS_TREE="${OS_TREE:-${REPO_ROOT}/.tmp/ss-os-devimg}"
# Where the shared Buildroot download / ccache directories live (the primary
# seedsigner-os checkout, since a worktree has none of its own).
CACHE_TREE="${CACHE_TREE:-/home/kdmukai/dev/seedsigner-os}"

# Middle field of seedsigner_os.<version>.<board>-dev.img. Passed to build.sh as
# --app-branch, which under --skip-repo only names the output file.
VERSION="${VERSION:-lvgl-dev}"
BOARDS_TO_BUILD="${BOARDS_TO_BUILD:-pi0 pi02w}"

# board -> directory holding that board's .so (see docker/build_steps.sh: the
# armv6 profile builds in place at src/, others land in src/<profile>/).
so_dir_for() {
  case "$1" in
    pi0)   echo "${REPO_ROOT}/src" ;;
    pi02w) echo "${REPO_ROOT}/src/pi02w" ;;
    *)     die "no .so profile mapped for board '$1'" ;;
  esac
}

log() { echo "[dev-image] $*"; }
die() { echo "[dev-image] ERROR: $*" >&2; exit 1; }

WORKDIR="$(mktemp -d)"; trap 'rm -rf "$WORKDIR"' EXIT


# --- 1. stage the app tree + this board's .so into the OS rootfs overlay ------
# The rootfs is embedded in the kernel (CONFIG_INITRAMFS_SOURCE), so everything
# left here is resident in RAM on a 512MB board -- the trimming below is not
# cosmetic.
#
# Both the version.json write and the trimming happen HERE rather than in
# build.sh: on this branch both live inside download_app_repo(), which --skip-repo
# skips entirely. A --skip-repo build therefore ships whatever the overlay
# contains, .git and all, and with no version.json. Order matters and mirrors a
# cloned build: version.json first (it needs .git/ and tools/), trim second.
stage_app() {
  local board="$1"
  local so_dir; so_dir="$(so_dir_for "$board")"

  [[ -d "$APP_TREE/src/seedsigner" ]] || die "app tree not found: $APP_TREE"
  [[ -d "$OS_TREE/opt" ]]             || die "os tree not found: $OS_TREE"

  local overlay="${OS_TREE}/opt/rootfs-overlay/opt"
  log "staging ${board}: app ${APP_TREE} -> ${overlay}"

  rm -rf "$overlay"
  mkdir -p "$overlay"

  # Stage the TRACKED files only (submodule contents included), not the working
  # tree: a maintainer's checkout accumulates untracked scratch -- notes, PR
  # extracts, translation working dirs -- and rsyncing the directory would bake
  # all of it into the initramfs. This reproduces what a clone of the branch
  # would contain, which is what the image is supposed to be.
  local filelist="${WORKDIR}/app-files.z"
  ( cd "$APP_TREE" && git ls-files --recurse-submodules -z ) > "$filelist"
  [[ -s "$filelist" ]] || die "git ls-files produced nothing in ${APP_TREE}"
  rsync -a --from0 --files-from="$filelist" \
    --exclude='__pycache__' --exclude='*.pyc' --exclude='.DS_Store' \
    "$APP_TREE"/ "$overlay"/

  # .git is needed only to derive version.json below; the trim step removes it.
  rsync -a "$APP_TREE/.git" "$overlay"/

  # src/lang-packs is a BUILT payload (produced from the seedsigner-language-packs
  # repo by build_packs.sh) and is deliberately untracked, so the tracked-files
  # copy above misses it. Without it the app has no language packs at all, which
  # only shows up at runtime -- so its absence is an error, not a warning.
  [[ -d "$APP_TREE/src/lang-packs" ]] \
    || die "no src/lang-packs in ${APP_TREE}; generate it with the language-packs repo's build_packs.sh --out-dir ${APP_TREE}/src/lang-packs"
  rsync -a "$APP_TREE/src/lang-packs" "${overlay}/src/"
  log "  lang-packs $(ls "$APP_TREE/src/lang-packs" | wc -l) entries ($(du -sh "$APP_TREE/src/lang-packs" | cut -f1))"

  # /start.sh does `cd /opt/src` and execs python from there, so sys.path[0] is
  # /opt/src -- a bare `import seedsigner_lvgl_screens` resolves with no
  # PYTHONPATH or .pth file if the .so sits alongside main.py. Same placement
  # scripts/deploy-dev.sh uses on a live board.
  local main_so uur_so
  main_so="$(ls -1t "$so_dir"/seedsigner_lvgl_screens*.so 2>/dev/null | head -n1 || true)"
  [[ -n "$main_so" ]] || die "no seedsigner_lvgl_screens .so in ${so_dir} (build it: TARGET_PROFILE=... ./run_build.sh)"
  cp -a "$main_so" "${overlay}/src/"
  log "  .so  $(basename "$so_dir")/$(basename "$main_so") ($(du -h "$main_so" | cut -f1))"

  # uUR is optional: the app's helpers/ur2/decoder.py falls back to pure Python.
  uur_so="$(ls -1t "$so_dir"/uUR*.so 2>/dev/null | head -n1 || true)"
  if [[ -n "$uur_so" ]]; then
    cp -a "$uur_so" "${overlay}/src/"
    log "  .so  $(basename "$uur_so")"
  else
    log "  uUR .so absent -- app will use the pure-Python UR decoder"
  fi

  # Fail loudly here rather than at boot: an ARMv6 board loading an A53 .so dies
  # with an opaque ImportError long after the build.
  local want_arch got_arch
  case "$board" in pi0) want_arch="v6" ;; pi02w) want_arch="v8" ;; esac
  got_arch="$(readelf -A "${overlay}/src/$(basename "$main_so")" | sed -n 's/.*Tag_CPU_arch: *//p' | head -1)"
  [[ "$got_arch" == "$want_arch"* ]] \
    || die "${board} expects Tag_CPU_arch ${want_arch}*, staged .so reports '${got_arch}'"
  log "  arch Tag_CPU_arch=${got_arch} OK for ${board}"

  # version.json drives the app's version screens and its SeedSigner-OS checks.
  # Must run while .git/ and tools/ are still present.
  ( cd "$overlay" && SEEDSIGNER_OS_BUILDER=1 PYTHONPATH=src python3 tools/write_versionfile.py >/dev/null ) \
    || die "write_versionfile.py failed in the staged tree"
  [[ -f "${overlay}/src/seedsigner/version.json" ]] || die "version.json was not written"
  log "  version $(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(d["name"], d["short_commit_hash"], d["timestamp"])' "${overlay}/src/seedsigner/version.json")"

  # Same set build.sh strips after a clone.
  local before; before="$(du -sh "$overlay" | cut -f1)"
  rm -rf "${overlay}"/{.github,docker,docs,enclosures,l10n,seedsigner-screenshots,tests,tools}
  rm -rf "${overlay}"/.git*
  rm -f  "${overlay}"/{docker-compose.yml,LICENSE.md,MANIFEST.in,pyproject.toml,README.md} \
         "${overlay}"/{requirements-raspi.txt,requirements.txt,seedsigner_pubkey.gpg}
  rm -f  "${overlay}"/setup.*
  local tr="${overlay}/src/seedsigner/resources/seedsigner-translations"
  if [[ -d "$tr" ]]; then
    rm -rf "$tr"/.git* "$tr"/LICENSE "$tr"/README.md
    # No .mo files are compiled on this path (that is download_app_repo's job), so
    # the .po sources are dead weight in RAM; the bundled src/lang-packs is what
    # the app actually reads.
    find "$tr" -name '*.po' -delete
  fi
  log "  trimmed ${before} -> $(du -sh "$overlay" | cut -f1)"

  [[ -f "${overlay}/src/main.py" ]] || die "staged overlay lost src/main.py"
}


# --- 2. per-board Buildroot builds -------------------------------------------
compose_run() { ( cd "$OS_TREE" && docker compose run --rm -T "$@" ); }

build_container() {
  log "building the Buildroot container image"
  ( cd "$OS_TREE" && docker compose build )
}

write_compose_override() {
  # /output is where build.sh puts the Buildroot tree. The stock compose file
  # leaves it inside the container, which buries a ~20GB tree in Docker's
  # storage; binding it to the worktree keeps it inspectable and deletable
  # between boards (this host has limited free space).
  #
  # The cache mounts are re-pointed at CACHE_TREE. A git worktree starts with only
  # tracked files, so the stock `./.buildroot_dl` / `./.ccache` / `./.buildroot-ccache`
  # would be empty new directories -- every source re-downloaded and every object
  # recompiled. Sharing the primary checkout's caches is the difference between a
  # warm build and a cold one; Buildroot's dl cache is content-addressed and ccache
  # is keyed on compiler+flags, so sharing them across trees is safe.
  [[ -d "$CACHE_TREE" ]] || die "cache tree not found: ${CACHE_TREE}"
  mkdir -p "${CACHE_TREE}/.buildroot_dl" "${CACHE_TREE}/.ccache" "${CACHE_TREE}/.buildroot-ccache"
  cat > "${OS_TREE}/docker-compose.override.yml" <<EOF
services:
  build-images:
    volumes:
      - ./output:/output
      - ${CACHE_TREE}/.buildroot_dl:/buildroot_dl
      - ${CACHE_TREE}/.ccache:/root/.cache/ccache
      - ${CACHE_TREE}/.buildroot-ccache:/root/.buildroot-ccache
EOF
  mkdir -p "${OS_TREE}/output" "${OS_TREE}/images"

  # build.sh clears the build directory by removing it, which fails on a bind
  # mount: the contents go but the mount point itself cannot be unlinked, and
  # the script runs under `set -e`. Clearing the contents instead is equivalent
  # for Buildroot and leaves the mount intact. Applied to the worktree copy at
  # run time so it never lands in a commit.
  python3 - "$OS_TREE" <<'PY'
import sys, pathlib
p = pathlib.Path(sys.argv[1]) / "opt" / "build.sh"
src = p.read_text()
old = '    # remove previous build dir\n    rm -rf "${build_dir}"\n    mkdir -p "${build_dir}"\n'
new = ('    # clear previous build dir contents; the dir itself may be a bind mount\n'
       '    mkdir -p "${build_dir}"\n'
       '    find "${build_dir}" -mindepth 1 -maxdepth 1 -exec rm -rf {} +\n')
if new in src:
    print("[dev-image] build.sh already patched for bind-mounted output")
elif old in src:
    p.write_text(src.replace(old, new))
    print("[dev-image] patched build.sh clean step for bind-mounted output")
else:
    sys.exit("[dev-image] ERROR: build.sh clean step not found; refusing to guess")
PY
}

build_board() {
  local board="$1"
  log "building ${board}-dev (hours on a cold tree)"
  # Args after the service name replace the compose `command`, and the image's
  # ENTRYPOINT is build.sh, so these are build.sh's own flags.
  compose_run build-images "--${board}" --dev --skip-repo "--app-branch=${VERSION}"
  local img="${OS_TREE}/images/seedsigner_os.${VERSION}.${board}-dev.img"
  [[ -f "$img" ]] || die "build finished but ${img} is missing"
  log "built $(basename "$img") ($(du -h "$img" | cut -f1))"
}


# --- 3. assemble both buckets onto one card ----------------------------------
# Runs inside the build container: the assembly needs mtools, fdisk and
# dosfstools, which the Dockerfile installs and a typical host does not have.
combine() {
  log "combining [${BOARDS_TO_BUILD}] into one dev card"
  compose_run --entrypoint bash build-images -c \
    "cd /opt && BRANCH='${VERSION}' MULTIBOARD_DEV=1 MULTIBOARD_BOARDS='${BOARDS_TO_BUILD}' ./combine-multiboard.sh"

  local out="${OS_TREE}/images/seedsigner_os.${VERSION}.multiboard-dev.img"
  [[ -f "$out" ]] || die "combine finished but ${out} is missing"
  log "MULTIBOARD DEV IMAGE: ${out} ($(du -h "$out" | cut -f1))"
  sha256sum "$out"
}


# --- main --------------------------------------------------------------------
cmd="${1:-all}"
case "$cmd" in
  stage)
    shift || true
    stage_app "${1:?usage: stage <board>}"
    ;;
  build)
    write_compose_override
    build_container
    shift || true
    for b in ${*:-$BOARDS_TO_BUILD}; do stage_app "$b"; build_board "$b"; done
    ;;
  combine)
    combine
    ;;
  all)
    write_compose_override
    build_container
    # No explicit tree cleanup between boards: build.sh already clears the build
    # directory at the start of every board (its "clean" argument), and it does so
    # from inside the container as root -- which is the only thing that CAN, since
    # every file in the bind-mounted tree is root-owned. Clearing it from the host
    # fails with EPERM on the first .dtbo it reaches. Peak disk is one tree either
    # way, because the clear happens before the next board's build begins.
    for b in $BOARDS_TO_BUILD; do
      stage_app "$b"
      build_board "$b"
    done
    combine
    ;;
  *)
    die "unknown command: ${cmd} (stage <board> | build [board...] | combine | all)"
    ;;
esac
