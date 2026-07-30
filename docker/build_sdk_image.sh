#!/usr/bin/env bash
# Build a cross-compile SDK image from a SeedSigner OS buildroot output.
# ============================================================================
# The SDK carries the buildroot cross toolchain + the matching target sysroot, so
# the extension is compiled BY the device's toolchain AGAINST the device's libs --
# on x86 at native speed. One image supplies both, so the compiler and the linked
# libraries cannot drift apart or away from the flashed image.
#
# Run this ONLY when the SeedSigner OS pin moves. Per-CI-job builds just pull the
# resulting tag; they never rebuild the SDK.
#
# TARGET_PROFILE selects the board whose buildroot output is being packaged:
#   armv6  Pi Zero / Zero W (ARM1176)     -> sdk-armv6   [default]
#   pi02w  Pi Zero 2 W (Cortex-A53+NEON)  -> sdk-pi02w
# Both profiles share the same OS pin -- their sysroots are harvested from the
# same seedsigner-os commit, built for different boards -- so the tag, the commit
# labels and the toolchain tuple are IDENTICAL between them. The sysroot's
# Tag_CPU_arch is the only thing that tells them apart, which is why this script
# gates on it below rather than trusting the operator's TARGET_PROFILE.
#
# Source of the buildroot output (first match wins):
#   SS_OS_CONTAINER  name of a (stopped is fine) seedsigner-os build container
#                    holding /output/host  [default: seedsigner-os-build-images-1]
#   SS_OS_HOST_DIR   path to a buildroot output/host tree on the host
#
# Provenance: the image is tagged AND labelled with the seedsigner-os commit its
# sysroot came from, so any artifact traces back to an OS release. That claim is
# independently verifiable (check out that commit and rebuild the OS).
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SS_OS_DIR="${SS_OS_DIR:-$(cd "${ROOT_DIR}/../seedsigner-os" 2>/dev/null && pwd || echo "")}"
CONTAINER="${SS_OS_CONTAINER:-seedsigner-os-build-images-1}"
HOST_DIR="${SS_OS_HOST_DIR:-}"

# Profile -> SDK repo + lock, mirroring run_build.sh's mapping (the consumer side).
TARGET_PROFILE="${TARGET_PROFILE:-armv6}"
case "${TARGET_PROFILE}" in
  armv6) _SDK_REPO="sdk-armv6"; _LOCK="${ROOT_DIR}/versions.lock.toml" ;;
  pi02w) _SDK_REPO="sdk-pi02w"; _LOCK="${ROOT_DIR}/versions.lock.pi02w.toml" ;;
  *) echo "[sdk] ERROR: unknown TARGET_PROFILE '${TARGET_PROFILE}' (armv6|pi02w)" >&2; exit 1 ;;
esac
IMAGE_REPO="${IMAGE_REPO:-ghcr.io/kdmukai-bot/seedsigner-raspi-lvgl/${_SDK_REPO}}"

# The expected sysroot arch comes from the SAME lock key build_steps.sh gates the
# built artifact on, with the same default, so the SDK and the extension can never
# disagree about what a profile means.
CPU_ARCH_REGEX="$(python3 - "${_LOCK}" <<'PY'
import sys, tomllib
with open(sys.argv[1], 'rb') as fh:
    print(tomllib.load(fh).get('toolchain', {}).get('cpu_arch_regex', 'v6|v6KZ'))
PY
)"

# --- Provenance from the seedsigner-os checkout ------------------------------
# Probed with rev-parse rather than by testing for a .git DIRECTORY, so a git
# WORKTREE (where .git is a file holding a gitdir pointer) is recognised instead of
# silently stamping the image 'unknown'.
if [[ -n "${SS_OS_DIR}" ]] && git -C "${SS_OS_DIR}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  SS_OS_COMMIT="$(git -C "${SS_OS_DIR}" rev-parse HEAD)"
  SS_OS_DESCRIBE="$(git -C "${SS_OS_DIR}" describe --tags --always)"
  BUILDROOT_COMMIT="$(git -C "${SS_OS_DIR}/opt/buildroot" rev-parse HEAD 2>/dev/null || echo unknown)"
  if [[ -n "$(git -C "${SS_OS_DIR}" status --porcelain)" ]]; then
    echo "[sdk] WARNING: ${SS_OS_DIR} has uncommitted changes -- the provenance" \
         "stamp (${SS_OS_DESCRIBE}) will NOT fully describe this sysroot." >&2
  fi
else
  echo "[sdk] WARNING: no seedsigner-os checkout found (set SS_OS_DIR)." \
       "Provenance will be stamped 'unknown'." >&2
  SS_OS_COMMIT=unknown; SS_OS_DESCRIBE=unknown; BUILDROOT_COMMIT=unknown
fi

IMAGE_TAG="${IMAGE_TAG:-${IMAGE_REPO}:ss-os-${SS_OS_DESCRIBE}}"

echo "[sdk] target profile  = ${TARGET_PROFILE} (expect Tag_CPU_arch ~ ${CPU_ARCH_REGEX})"
echo "[sdk] ss-os commit    = ${SS_OS_COMMIT}"
echo "[sdk] ss-os describe  = ${SS_OS_DESCRIBE}"
echo "[sdk] buildroot pin   = ${BUILDROOT_COMMIT}"
echo "[sdk] image tag       = ${IMAGE_TAG}"

# --- Assemble the build context ---------------------------------------------
CTX="$(mktemp -d)"
trap 'rm -rf "${CTX}"' EXIT
cp "${ROOT_DIR}/docker/Dockerfile.sdk" "${CTX}/Dockerfile"

if [[ -n "${HOST_DIR}" ]]; then
  echo "[sdk] source: host dir ${HOST_DIR}"
  [[ -x "${HOST_DIR}/bin/arm-Buildroot-linux-gnueabihf-gcc" ]] \
    || { echo "ERROR: no cross gcc under ${HOST_DIR}/bin" >&2; exit 1; }
  cp -a "${HOST_DIR}" "${CTX}/host"
else
  echo "[sdk] source: container ${CONTAINER} (/output/host, ~1GB -- this takes a minute)"
  docker inspect "${CONTAINER}" >/dev/null
  docker cp "${CONTAINER}:/output/host" "${CTX}/host"
fi

# `staging` is a symlink INTO host, so the single host/ tree is self-sufficient.
# Fail loudly here rather than at link time inside a CI job.
for probe in \
    bin/arm-Buildroot-linux-gnueabihf-g++ \
    bin/arm-Buildroot-linux-gnueabihf-readelf \
    bin/python3 \
    arm-Buildroot-linux-gnueabihf/sysroot/usr/include/python3.12/Python.h \
    arm-Buildroot-linux-gnueabihf/sysroot/usr/lib/libcamera.so \
    arm-Buildroot-linux-gnueabihf/sysroot/usr/lib/python3.12/_sysconfigdata__linux_arm-linux-gnueabihf.py; do
  [[ -e "${CTX}/host/${probe}" ]] || { echo "ERROR: extracted tree missing ${probe}" >&2; exit 1; }
done
echo "[sdk] extracted tree verified ($(du -sh "${CTX}/host" | cut -f1))"

# --- Gate the sysroot's arch against the profile ------------------------------
# The container default holds whichever board was built LAST, so asking for pi02w
# right after a pi0 build silently packages an ARMv6 sysroot under the pi02w tag.
# Nothing downstream would catch it: the two profiles share a toolchain tuple, a
# SOABI and an artifact filename, so the mislabelled SDK builds and links cleanly
# and only fails at dlopen on the device, as an opaque ImportError. Read the
# sysroot's own libc with the tree's own readelf -- the one fact that differs.
SYSROOT="${CTX}/host/arm-Buildroot-linux-gnueabihf/sysroot"
GOT_ARCH="$("${CTX}/host/bin/arm-Buildroot-linux-gnueabihf-readelf" -A "${SYSROOT}/lib/libc.so.6" \
  | sed -n 's/.*Tag_CPU_arch: *//p' | head -1)"
[[ -n "${GOT_ARCH}" ]] || { echo "ERROR: could not read Tag_CPU_arch from ${SYSROOT}/lib/libc.so.6" >&2; exit 1; }
if [[ ! "${GOT_ARCH}" =~ ^(${CPU_ARCH_REGEX})$ ]]; then
  echo "ERROR: TARGET_PROFILE=${TARGET_PROFILE} expects Tag_CPU_arch matching '${CPU_ARCH_REGEX}'," \
       "but this sysroot reports '${GOT_ARCH}'." >&2
  echo "       The extracted buildroot output is for a different board -- rebuild" \
       "seedsigner-os for ${TARGET_PROFILE}, or point SS_OS_HOST_DIR at that board's output/host." >&2
  exit 1
fi
echo "[sdk] sysroot Tag_CPU_arch=${GOT_ARCH} OK for ${TARGET_PROFILE}"

# --- Build --------------------------------------------------------------------
docker build \
  --build-arg "SS_OS_COMMIT=${SS_OS_COMMIT}" \
  --build-arg "SS_OS_DESCRIBE=${SS_OS_DESCRIBE}" \
  --build-arg "BUILDROOT_COMMIT=${BUILDROOT_COMMIT}" \
  --build-arg "TARGET_PROFILE=${TARGET_PROFILE}" \
  -t "${IMAGE_TAG}" \
  "${CTX}"

echo "[sdk] OK -> ${IMAGE_TAG}"
echo "[sdk] push it (public) so CI can pull:  docker push ${IMAGE_TAG}"
