#!/usr/bin/env bash
# Package a built SeedSigner OS Buildroot tree as a pre-baked CI image.
# ============================================================================
# A Buildroot build is dominated by compiling the toolchain and every package,
# which depends only on the seedsigner-os ref. That work is done ONCE here and
# published; CI then restores the tree and re-runs only the payload tail (overlay
# copy, cpio, kernel relink) -- ~70s/board instead of hours.
# See docs/knowledge/dev-image-incremental-rebuild.md.
#
# Run this ONLY when the seedsigner-os pin moves. It is published locally and
# manually, exactly like the cross-compile SDK: no CI job ever needs a registry
# write token, and the multi-hour build stays off GitHub's runners.
#
# BOARD selects which board's tree is being packaged (its dev variant):
#   pi0    Pi Zero / Zero W (ARM1176)     -> prebake-pi0
#   pi02w  Pi Zero 2 W (Cortex-A53+NEON)  -> prebake-pi02w
#
# Source of the output tree (first match wins):
#   SS_OS_OUTPUT_DIR  path to a buildroot output/ tree
#   SS_OS_DIR/output  the seedsigner-os checkout's own tree  [default]
#
# Usage:
#   BOARD=pi0 ./docker/build_prebake_image.sh
#   BOARD=pi02w SS_OS_OUTPUT_DIR=/path/to/output ./docker/build_prebake_image.sh
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SS_OS_DIR="${SS_OS_DIR:-$(cd "${ROOT_DIR}/../seedsigner-os" 2>/dev/null && pwd || echo "")}"

SLIM=0
for arg in "$@"; do
  case "$arg" in
    # An ASSERTION, not an action: slimming is a separate step
    # (docker/slim_prebake_tree.sh) because the slim tree must be validated by an
    # incremental build BEFORE it is baked, and the tree that gets validated has
    # to be the exact tree that gets shipped. --slim refuses to bake a full tree,
    # for the operator who means to publish the small one.
    --slim) SLIM=1 ;;
    --board=*) BOARD="${arg#--board=}" ;;
    *) echo "[prebake] ERROR: unknown argument '$arg'" >&2; exit 1 ;;
  esac
done

BOARD="${BOARD:-}"
case "${BOARD}" in
  pi0|pi02w) : ;;
  "") echo "[prebake] ERROR: BOARD is required (pi0|pi02w)" >&2; exit 1 ;;
  *)  echo "[prebake] ERROR: unknown BOARD '${BOARD}' (pi0|pi02w)" >&2; exit 1 ;;
esac
# Every prebake tree is a --dev tree; the release path does not use this mechanism.
BOARD_DIR="${BOARD}-dev"
IMAGE_REPO="${IMAGE_REPO:-ghcr.io/kdmukai-bot/seedsigner-raspi-lvgl/prebake-${BOARD}}"

OUTPUT_DIR="${SS_OS_OUTPUT_DIR:-${SS_OS_DIR:+${SS_OS_DIR}/output}}"
[[ -n "${OUTPUT_DIR}" ]] || { echo "[prebake] ERROR: set SS_OS_OUTPUT_DIR or SS_OS_DIR" >&2; exit 1; }
[[ -d "${OUTPUT_DIR}" ]] || { echo "[prebake] ERROR: no output tree at ${OUTPUT_DIR}" >&2; exit 1; }

# --- Provenance from the seedsigner-os checkout ------------------------------
# Probed with rev-parse rather than by testing for a .git DIRECTORY: the tree these
# images are baked from is normally a git WORKTREE, where .git is a file holding a
# gitdir pointer. A -d test silently misses that and stamps the image 'unknown'.
if [[ -n "${SS_OS_DIR}" ]] && git -C "${SS_OS_DIR}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  SS_OS_COMMIT="$(git -C "${SS_OS_DIR}" rev-parse HEAD)"
  SS_OS_DESCRIBE="$(git -C "${SS_OS_DIR}" describe --tags --always)"
  BUILDROOT_COMMIT="$(git -C "${SS_OS_DIR}/opt/buildroot" rev-parse HEAD 2>/dev/null || echo unknown)"
  if [[ -n "$(git -C "${SS_OS_DIR}" status --porcelain)" ]]; then
    echo "[prebake] WARNING: ${SS_OS_DIR} has uncommitted changes -- the provenance" \
         "stamp (${SS_OS_DESCRIBE}) will NOT fully describe this tree." >&2
  fi
else
  echo "[prebake] WARNING: no seedsigner-os checkout found (set SS_OS_DIR)." \
       "Provenance will be stamped 'unknown'." >&2
  SS_OS_COMMIT=unknown; SS_OS_DESCRIBE=unknown; BUILDROOT_COMMIT=unknown
fi

IMAGE_TAG="${IMAGE_TAG:-${IMAGE_REPO}:ss-os-${SS_OS_DESCRIBE}}"

echo "[prebake] board           = ${BOARD} (defconfig dir ${BOARD_DIR})"
echo "[prebake] output tree     = ${OUTPUT_DIR}"
echo "[prebake] ss-os commit    = ${SS_OS_COMMIT}"
echo "[prebake] ss-os describe  = ${SS_OS_DESCRIBE}"
echo "[prebake] buildroot pin   = ${BUILDROOT_COMMIT}"
echo "[prebake] image tag       = ${IMAGE_TAG}"

# --- Gate 1: the tree is for THIS board, and is a dev tree -------------------
# A Buildroot output tree carries no obvious sign of which board produced it, and
# the build directory is reused across boards -- so packaging the wrong tree is
# easy and silent. BR2_ROOTFS_POST_BUILD_SCRIPT names the board dir literally,
# which proves the board AND the dev variant in one read.
CONFIG="${OUTPUT_DIR}/.config"
[[ -f "${CONFIG}" ]] || { echo "[prebake] ERROR: no ${CONFIG} -- not a configured Buildroot tree" >&2; exit 1; }
GOT_HOOK="$(sed -n 's/^BR2_ROOTFS_POST_BUILD_SCRIPT="\(.*\)"$/\1/p' "${CONFIG}" | head -1)"
WANT_HOOK="../${BOARD_DIR}/board/post-build.sh"
if [[ "${GOT_HOOK}" != "${WANT_HOOK}" ]]; then
  echo "[prebake] ERROR: this tree was built for a different target." >&2
  echo "          BR2_ROOTFS_POST_BUILD_SCRIPT = '${GOT_HOOK}'" >&2
  echo "          expected for BOARD=${BOARD}   = '${WANT_HOOK}'" >&2
  echo "          Rebuild seedsigner-os for ${BOARD} --dev, or point SS_OS_OUTPUT_DIR at that tree." >&2
  exit 1
fi
echo "[prebake] board identity OK (${GOT_HOOK})"

# --- Gate 2: the tree is complete --------------------------------------------
for d in host target build images; do
  [[ -d "${OUTPUT_DIR}/${d}" ]] || { echo "[prebake] ERROR: tree missing output/${d}" >&2; exit 1; }
done

# --- Gate 3: the build-directory reachbacks are present ----------------------
# output/build is NOT inert scratch. The kernel is relinked on every incremental
# build (CONFIG_INITRAMFS_SOURCE embeds the rootfs in the zImage), and the board
# hooks compile DT overlays and bytecode out of build/ directly. A tree missing
# any of these builds for ~20 minutes and then fails at target-finalize, or --
# worse, for the guarded scp fix-up -- succeeds while shipping a defect.
#
# Checked here rather than trusted, because this is the last point where the tree
# is inspectable before it becomes an opaque published artifact.
check_reachback() {
  local desc="$1"; shift
  local found=0 p
  for p in "$@"; do [[ -e "$p" ]] && { found=1; break; }; done
  if [[ "$found" == "0" ]]; then
    echo "[prebake] ERROR: ${desc} is missing from the tree:" >&2
    printf '          %s\n' "$@" >&2
    exit 1
  fi
}
B="${OUTPUT_DIR}/build"
# The kernel relink needs the whole tree; dtc + overlay sources + headers are the
# parts post-image-seedsigner.sh invokes by path.
check_reachback "kernel build tree (relink + DT overlays)" "${B}/linux-custom/scripts/dtc/dtc"
check_reachback "kernel DT overlay sources"                "${B}/linux-custom/arch/arm/boot/dts/overlays"
check_reachback "kernel headers for the overlay cpp pass"  "${B}/linux-custom/include"
# busybox target-finalize re-reads its generated Kconfig.
check_reachback "busybox .config (target-finalize)"         ${B}/busybox-*/.config
# post-build.sh runs this by path with the HOST interpreter.
check_reachback "python3 compileall.py (__pycache__ gen)"   ${B}/python3-*/Lib/compileall.py
# Guarded in post-build.sh, so its absence is SILENT: dropbear's scp would stay at
# /usr/bin/scp with its client disabled, leaving scp broken on the device.
check_reachback "OpenSSH scp (silent dropbear-collision fix-up)" ${B}/openssh-*/scp
echo "[prebake] build-dir reachbacks OK"

# --- Gate 4: which variant of the tree is this? -------------------------------
# Recorded rather than left to be inferred later, because the two variants are
# indistinguishable once published except by size, and size is exactly what a
# consumer is trying to predict.
#
# Read from a MARKER slim_prebake_tree.sh writes, not inferred from the tree's
# shape. The tempting structural signature -- a package dir holding its .stamp_*
# files and nothing else -- is wrong: Buildroot's virtual and script-only packages
# (jpeg, openssl, toolchain, skeleton, urandom-scripts, ... 16 of them here) look
# exactly like that in a FULL tree, so the check reports every tree as slim and the
# guard below silently never fires.
SLIM_MARKER="${OUTPUT_DIR}/SS_OS_SLIM_ALLOWLIST"
if [[ -f "${SLIM_MARKER}" ]]; then TREE_VARIANT=slim; else TREE_VARIANT=full; fi

if [[ "${SLIM}" == "1" && "${TREE_VARIANT}" != "slim" ]]; then
  echo "[prebake] ERROR: --slim was given but ${OUTPUT_DIR} is a FULL tree." >&2
  echo "          Slim it first:  SS_OS_OUTPUT_DIR=${OUTPUT_DIR} ./docker/slim_prebake_tree.sh" >&2
  echo "          then validate the result per docs/knowledge/prebake-tree-slimming.md" >&2
  echo "          and point this script at the slim tree." >&2
  exit 1
fi
echo "[prebake] tree variant   = ${TREE_VARIANT}"
echo "[prebake] tree size $(du -sh "${OUTPUT_DIR}" 2>/dev/null | cut -f1 || echo unknown)"

# --- Build --------------------------------------------------------------------
# The context is the tree's PARENT, so `COPY output/` in the Dockerfile resolves to
# the tree. Docker will not follow a symlink out of its context, so the tree cannot
# be aliased in from elsewhere -- it has to sit in the context as `output`.
OUTPUT_PARENT="$(cd "${OUTPUT_DIR}/.." && pwd)"
if [[ "$(basename "${OUTPUT_DIR}")" != "output" ]]; then
  echo "[prebake] ERROR: the tree directory must be named 'output' (Docker cannot" >&2
  echo "          follow a symlink out of the build context), but got" >&2
  echo "          '$(basename "${OUTPUT_DIR}")' at ${OUTPUT_DIR}." >&2
  exit 1
fi

# The context is ~16GB, so the tar stream to the daemon is the slow part -- minutes,
# for an operation run only when the OS pin moves. Everything in the parent that is
# not the tree is excluded so a full seedsigner-os checkout does not ride along.
DOCKERIGNORE="${OUTPUT_PARENT}/.dockerignore"
DOCKERIGNORE_PREEXISTING=0
[[ -e "${DOCKERIGNORE}" ]] && DOCKERIGNORE_PREEXISTING=1
cleanup() { [[ "${DOCKERIGNORE_PREEXISTING}" == "0" ]] && rm -f "${DOCKERIGNORE}"; }
trap cleanup EXIT
if [[ "${DOCKERIGNORE_PREEXISTING}" == "0" ]]; then
  printf '*\n!output\n' > "${DOCKERIGNORE}"
else
  echo "[prebake] NOTE: ${DOCKERIGNORE} already exists; leaving it alone." \
       "The context may include more than output/." >&2
fi

echo "[prebake] building the image (large context; this takes a while)"
docker build \
  --build-arg "SS_OS_COMMIT=${SS_OS_COMMIT}" \
  --build-arg "SS_OS_DESCRIBE=${SS_OS_DESCRIBE}" \
  --build-arg "BUILDROOT_COMMIT=${BUILDROOT_COMMIT}" \
  --build-arg "BOARD=${BOARD}" \
  --build-arg "TREE_VARIANT=${TREE_VARIANT}" \
  -t "${IMAGE_TAG}" \
  -f "${ROOT_DIR}/docker/Dockerfile.prebake" \
  "${OUTPUT_PARENT}"

echo "[prebake] OK -> ${IMAGE_TAG}"
echo "[prebake] push it (public) so CI can pull:  docker push ${IMAGE_TAG}"
echo "[prebake] then set PREBAKE_KEY=ss-os-${SS_OS_DESCRIBE} in .github/workflows/dev-image.yml"
