#!/usr/bin/env bash
# Build the Pi Zero extensions via the SeedSigner OS cross-compile SDK.
# ============================================================================
# Runs NATIVELY on x86 -- no QEMU, no --platform. The SDK image carries the
# SeedSigner OS buildroot cross toolchain and the matching target sysroot, so the
# artifacts are built by the device's own toolchain against the device's own libs.
#
# This is the SAME entry point CI uses (.github / .forgejo), so local and CI run
# one build path. Rebuild the SDK image only when the OS pin moves:
#   ./docker/build_sdk_image.sh
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WS_ROOT="${WS_ROOT:-$(cd "${ROOT_DIR}/.." && pwd)}"
# Per-target SDK image + versions lock. Both profiles share the SAME OS pin (their
# sysroots are harvested from the same SeedSigner OS #114 buildroot); only the arch
# differs. TARGET_PROFILE=armv6 (default, Pi Zero/ARM1176) or pi02w (Pi Zero 2 W/A53).
TARGET_PROFILE="${TARGET_PROFILE:-armv6}"
case "${TARGET_PROFILE}" in
  armv6) _SDK_REPO="sdk-armv6"; _LOCK="versions.lock.toml" ;;
  pi02w) _SDK_REPO="sdk-pi02w"; _LOCK="versions.lock.pi02w.toml" ;;
  *) echo "[run-build] ERROR: unknown TARGET_PROFILE '${TARGET_PROFILE}'" >&2; exit 1 ;;
esac
# Pinned to the SeedSigner OS release whose sysroot it carries; bumping the OS
# means rebuilding the SDK and moving this tag in one commit.
IMAGE_TAG="${IMAGE_TAG:-ghcr.io/kdmukai-bot/seedsigner-raspi-lvgl/${_SDK_REPO}:ss-os-0.8.0-81-gbfbd791}"

# Map host repo path into mounted container workspace deterministically.
REL_REPO_PATH="$(realpath --relative-to="${WS_ROOT}" "${ROOT_DIR}")"
CONTAINER_REPO_DIR="${CONTAINER_REPO_DIR:-/workspace/${REL_REPO_PATH}}"

# --- Optional: build from a LIVE screens checkout (SS_SCREENS_SRC) ------------
# Point SS_SCREENS_SRC at a seedsigner-lvgl-screens checkout on the HOST to compile
# its (possibly uncommitted) source instead of the pinned sources/ submodule -- for
# testing live screens changes without a submodule bump. It must live UNDER WS_ROOT
# (the dev/ tree mounted at /workspace) so the container can see it; both the screens
# source and its nested LVGL are taken from there, kept consistent.
if [ -n "${SS_SCREENS_SRC:-}" ]; then
  _screens_abs="$(cd "${SS_SCREENS_SRC}" 2>/dev/null && pwd)" \
    || { echo "[run-build] ERROR: SS_SCREENS_SRC not found: ${SS_SCREENS_SRC}" >&2; exit 1; }
  _screens_rel="$(realpath --relative-to="${WS_ROOT}" "${_screens_abs}")"
  case "${_screens_rel}" in
    ../*|/*) echo "[run-build] ERROR: SS_SCREENS_SRC must live under WS_ROOT (${WS_ROOT}) so the build container can mount it: ${_screens_abs}" >&2; exit 1 ;;
  esac
  [ -d "${_screens_abs}/components/seedsigner/screens" ] \
    || { echo "[run-build] ERROR: no components/seedsigner/screens under SS_SCREENS_SRC: ${_screens_abs}" >&2; exit 1; }
  [ -f "${_screens_abs}/third_party/lvgl/lvgl.h" ] \
    || { echo "[run-build] ERROR: no third_party/lvgl/lvgl.h under SS_SCREENS_SRC (nested LVGL submodule not checked out?): ${_screens_abs}" >&2; exit 1; }
  export SEEDSIGNER_LVGL_SCREENS_DIR="/workspace/${_screens_rel}"
  export LVGL_ROOT="/workspace/${_screens_rel}/third_party/lvgl"
  echo "[run-build] screens source OVERRIDE (SS_SCREENS_SRC): ${_screens_abs}"
  echo "[run-build]   container SEEDSIGNER_LVGL_SCREENS_DIR=${SEEDSIGNER_LVGL_SCREENS_DIR}"
fi

RUN_TS="${RUN_TS:-$(date -u +%Y%m%d-%H%M%S)}"
LOG_DIR="${ROOT_DIR}/logs"
mkdir -p "${LOG_DIR}"
LOG_FILE="${LOG_DIR}/${RUN_TS}_build.log"
exec > >(tee -a "${LOG_FILE}")
exec 2>&1

echo "[run-build] run_ts=${RUN_TS}"
echo "[run-build] target_profile=${TARGET_PROFILE} lock=${_LOCK}"
echo "[run-build] image=${IMAGE_TAG} (native x86 cross-compile)"
echo "[run-build] container_repo_dir=${CONTAINER_REPO_DIR}"
echo "[run-build] log_file=${LOG_FILE}"

docker image inspect "${IMAGE_TAG}" >/dev/null

# ccache volume: host path if set (CI), else a Docker named volume (local dev).
# No venv volume -- the SDK image already carries setuptools and pytest.
CCACHE_VOLUME="${CCACHE_HOST_DIR:-seedsigner-raspi-lvgl-ccache}"

docker run --rm \
  -v "${WS_ROOT}:/workspace" \
  -v "${CCACHE_VOLUME}:/root/.cache/ccache" \
  -w "${CONTAINER_REPO_DIR}" \
  -e RUN_TS="${RUN_TS}" \
  -e JOBS="${JOBS:-}" \
  -e TARGET_PROFILE="${TARGET_PROFILE}" \
  -e ABI_JSON="${ABI_JSON:-${CONTAINER_REPO_DIR}/docs/abi/dev-pi-abi.json}" \
  -e LOCK_FILE="${LOCK_FILE:-${CONTAINER_REPO_DIR}/${_LOCK}}" \
  -e SEEDSIGNER_LVGL_SCREENS_DIR="${SEEDSIGNER_LVGL_SCREENS_DIR:-${CONTAINER_REPO_DIR}/sources/seedsigner-lvgl-screens}" \
  -e LVGL_ROOT="${LVGL_ROOT:-${CONTAINER_REPO_DIR}/sources/seedsigner-lvgl-screens/third_party/lvgl}" \
  -e LVGL_PERF_MONITOR="${LVGL_PERF_MONITOR:-0}" \
  "${IMAGE_TAG}" \
  bash ./docker/build_steps.sh

echo "[run-build] OK"
