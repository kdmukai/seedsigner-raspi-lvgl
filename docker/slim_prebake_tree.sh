#!/usr/bin/env bash
# Materialise an allowlisted ("slim") copy of a built Buildroot output tree.
# ============================================================================
# A full dev tree is ~16G by du (~21.6G as Docker layers, which count Buildroot's
# hardlinks once per link). Nearly all of it is per-package build directories that
# nothing reads again once the package is installed. Dropping them takes the tree
# to ~4.4G, which is what makes the CI restore step fit: that step holds the image
# layers AND the extracted copy at the same time, so tree size is doubled there.
#
# output/build is NOT inert scratch, which is why this is an ALLOWLIST and not a
# blocklist. Deleting every build dir and keeping only the .stamp_* files yields a
# 4.3G tree that builds for ~20 minutes and then dies at target-finalize -- after
# the expensive part. Worse, one reachback (post-build.sh's OpenSSH scp fix-up) is
# *guarded*, so dropping it does not fail the build at all: it ships a green image
# with a broken scp. Every entry below is a path some hook or Buildroot step opens
# by name during the incremental tail. See docs/knowledge/prebake-tree-slimming.md.
#
# The copy runs as ROOT INSIDE A CONTAINER, not on the host. Everything under
# output/ is root-owned, and output/target's uid/gid map straight into rootfs.cpio.
# A host-side `cp -a` as an unprivileged user would silently rewrite every file to
# uid 1000 -- and the validation gate hashes file CONTENTS, so it would not notice.
# The image would boot with a userspace owned by a nonexistent user.
#
# Usage:
#   ./docker/slim_prebake_tree.sh --dry-run          # measure, copy nothing
#   ./docker/slim_prebake_tree.sh                    # write <parent>/output.slim
#   SS_OS_DIR=... DEST=/path/to/output.slim ./docker/slim_prebake_tree.sh
#
# Then validate before baking (docs/knowledge/prebake-tree-slimming.md): swap the slim tree
# in as output/, rebuild with INCREMENTAL=1 SKIP_STAGE=1, and diff the per-file
# manifest of output/target against the full-tree baseline. It must be empty.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SS_OS_DIR="${SS_OS_DIR:-$(cd "${ROOT_DIR}/../seedsigner-os" 2>/dev/null && pwd || echo "")}"

DRY_RUN=0
for arg in "$@"; do
  case "$arg" in
    --dry-run) DRY_RUN=1 ;;
    *) echo "[slim] ERROR: unknown argument '$arg'" >&2; exit 1 ;;
  esac
done

OUTPUT_DIR="${SS_OS_OUTPUT_DIR:-${SS_OS_DIR:+${SS_OS_DIR}/output}}"
[[ -n "${OUTPUT_DIR}" ]] || { echo "[slim] ERROR: set SS_OS_OUTPUT_DIR or SS_OS_DIR" >&2; exit 1; }
[[ -d "${OUTPUT_DIR}" ]] || { echo "[slim] ERROR: no output tree at ${OUTPUT_DIR}" >&2; exit 1; }
OUTPUT_DIR="$(cd "${OUTPUT_DIR}" && pwd)"
TREE_PARENT="$(cd "${OUTPUT_DIR}/.." && pwd)"
DEST="${DEST:-${TREE_PARENT}/output.slim}"

for d in host target build images; do
  [[ -d "${OUTPUT_DIR}/${d}" ]] || { echo "[slim] ERROR: ${OUTPUT_DIR} is missing ${d}/ -- not a built tree" >&2; exit 1; }
done

# The container sees the tree's parent, so a DEST alongside it is reachable in the
# same mount. A DEST elsewhere would need a second mount and is not worth it.
case "${DEST}" in
  "${TREE_PARENT}"/*) : ;;
  *) echo "[slim] ERROR: DEST must sit alongside the tree, under ${TREE_PARENT}" >&2; exit 1 ;;
esac
DEST_NAME="$(basename "${DEST}")"
SRC_NAME="$(basename "${OUTPUT_DIR}")"

echo "[slim] source = ${OUTPUT_DIR}"
echo "[slim] dest   = ${DEST}$( ((DRY_RUN)) && echo '  (dry run: nothing written)')"

# --- the allowlist -----------------------------------------------------------
# Package build dirs kept IN FULL. Globs, resolved inside the container.
#
#   linux-custom          The incremental tail RELINKS the kernel, because
#                         CONFIG_INITRAMFS_SOURCE embeds the rootfs in the zImage
#                         -- a new payload is a new kernel. A kernel cannot be
#                         relinked without its full tree (sources plus every .o).
#                         post-image-seedsigner.sh also runs scripts/dtc/dtc and
#                         cpp over arch/arm/boot/dts/overlays with -I include, to
#                         rebuild the camera overlays against our own .dtb.
#                         The largest single item kept, and non-negotiable.
#   python3-3.12.10       post-build.sh runs Lib/compileall.py BY PATH with the
#                         host interpreter to generate __pycache__ under
#                         target/opt/src. Python puts the script's own directory
#                         first on sys.path, so Lib/ resolves compileall's imports
#                         too and has to be intact. The whole dir is only ~150M,
#                         so it is kept entire rather than pruned to Lib/.
#   busybox-*             A kconfig package, like linux-custom. Its .stamp_dotconfig
#                         has a REAL prerequisite on the board's
#                         busybox.config.fragment, so when that file is newer than
#                         the stamp -- which it always is in CI, where the OS tree is
#                         a fresh checkout stamped with the checkout time -- make
#                         re-runs the dotconfig step and calls `make oldconfig` in
#                         this directory. Without the sources that fails with
#                         "No rule to make target 'oldconfig'". Keeping only
#                         busybox-*/.config is enough for target-finalize but NOT for
#                         this, and the difference does not show up in a local
#                         validation run, where the stamp is newer than the fragment.
#                         Buildroot's kconfig packages are linux, busybox,
#                         linux-backports, swupdate, uclibc and xvisor; only the
#                         first two are in this build.
KEEP_FULL_GLOBS=(
  'linux-custom'
  'python3-3.12.10'
  'busybox-*'
)

# Buildroot's own infrastructure directories -- NOT packages, and easy to miss
# because they carry no version suffix and no .stamp_* files.
#
#   buildroot-fs          cpio/fakeroot is the script that ASSEMBLES rootfs.cpio,
#                         plus the device and user tables it applies. 20K, and the
#                         rootfs cannot be built without it.
#   buildroot-config      The kconfig binaries (conf, auto.conf, autoconf.h)
#                         Buildroot re-runs on every invocation to reload .config.
KEEP_FULL_GLOBS+=(
  'buildroot-fs'
  'buildroot-config'
)

# Single files rescued out of otherwise-dropped package dirs.
#
#   openssh-*/scp         dropbear installs its own scp at the same path, and its
#                         client is disabled, so post-build.sh overwrites it with
#                         OpenSSH's. That fix-up is GUARDED -- absence skips it
#                         silently and ships a broken scp. THE SILENT ONE.
# (busybox-*/.config is covered by keeping busybox-* whole, above.)
KEEP_FILE_GLOBS=(
  'openssh-*/scp'
)

# Every other package dir is reduced to its top-level DOTFILES (see the copy loop
# below). Those are what Buildroot itself reads: .stamp_* (so each package still
# counts as built, configured and installed -- without them the tail rebuilds
# everything), .files-list*.txt (per-package install manifests, used for collision
# detection and uninstall) and .applied_patches_list. All small; kept for every
# package rather than stamps-only because the cost is megabytes and the failure mode
# of guessing wrong is a 20-minute build that dies at the end.

# --- the copy ----------------------------------------------------------------
# Rendered as a script run inside the container so it executes as root in one
# pass. Paths are container-side: the tree parent is bound at /work.
copy_script=$(cat <<'INNER'
set -euo pipefail
SRC="/work/${SRC_NAME}"
DST="/work/${DEST_NAME}"

if [ -e "${DST}" ]; then
  echo "[slim] ERROR: ${DST} already exists -- refusing to overwrite." >&2
  echo "       Remove it (as root: docker run --rm -v <parent>:/work debian:12-slim rm -rf /work/${DEST_NAME})" >&2
  exit 1
fi

# Wholesale: everything outside build/. host/ is the toolchain and sysroot the
# relink and the .so build use; target/ is the rootfs under construction; images/
# holds the boot blobs post-image writes into. The top-level files (.config,
# .br2-external.*, Makefile) are what Buildroot reads to know what this tree is,
# and `staging` is a SYMLINK into host/ that must stay a symlink -- cp -a keeps it.
mkdir -p "${DST}"
find "${SRC}" -maxdepth 1 -mindepth 1 \( -type f -o -type l \) -exec cp -a {} "${DST}/" \;
for d in host target images; do
  echo "[slim]   copying ${d}/"
  cp -a "${SRC}/${d}" "${DST}/"
done

# build/: top-level files and symlinks first. packages-file-list*.txt are global
# install manifests; the python3 / python3-3.10.10 symlinks are created by
# post-build.sh but preserved here so the tree is self-consistent before a build.
echo "[slim]   copying build/ (allowlisted)"
mkdir -p "${DST}/build"
find "${SRC}/build" -maxdepth 1 -mindepth 1 \( -type f -o -type l \) -exec cp -a {} "${DST}/build/" \;

# Full-copy dirs.
for glob in ${KEEP_FULL}; do
  for p in ${SRC}/build/${glob}; do
    [ -d "${p}" ] || continue
    echo "[slim]     full: $(basename "${p}")"
    cp -a "${p}" "${DST}/build/"
  done
done

# Every remaining package dir: recreate it (preserving mode/ownership/timestamp
# via cp -a of the dir itself with --attributes-only) and copy its dotfiles.
for p in ${SRC}/build/*/; do
  name="$(basename "${p}")"
  [ -e "${DST}/build/${name}" ] && continue          # already full-copied
  [ -L "${SRC}/build/${name}" ] && continue          # symlink, handled above
  cp -a --attributes-only "${SRC}/build/${name}" "${DST}/build/${name}"
  find "${SRC}/build/${name}" -maxdepth 1 -mindepth 1 -name '.*' -type f \
    -exec cp -a {} "${DST}/build/${name}/" \;
done

# Single-file rescues, after the dirs exist.
for glob in ${KEEP_FILES}; do
  for p in ${SRC}/build/${glob}; do
    [ -f "${p}" ] || continue
    rel="${p#${SRC}/build/}"
    echo "[slim]     file: ${rel}"
    mkdir -p "${DST}/build/$(dirname "${rel}")"
    cp -a "${p}" "${DST}/build/${rel}"
  done
done

# Mark the tree. build_prebake_image.sh reads this to stamp the image's
# org.seedsigner.tree-variant label, and refuses to bake a full tree under --slim.
# A marker rather than a shape check, because Buildroot's virtual and script-only
# packages (jpeg, openssl, toolchain, skeleton, ...) hold nothing but .stamp_* files
# in a FULL tree too -- so "a package dir with only dotfiles" identifies every tree
# as slim. Recording the allowlist itself makes the tree self-describing: which
# paths were kept is the one thing a reader of a published tree cannot reconstruct.
{
  echo "# Allowlisted (slim) Buildroot tree -- docs/knowledge/prebake-tree-slimming.md"
  echo "# Everything outside build/ is kept whole. Within build/:"
  for g in ${KEEP_FULL}; do echo "full_dir=${g}"; done
  for g in ${KEEP_FILES}; do echo "file=${g}"; done
  echo "other_package_dirs=top-level dotfiles only (.stamp_*, .files-list*, .applied_patches_list)"
} > "${DST}/SS_OS_SLIM_ALLOWLIST"

echo "[slim] sizes:"
du -sh "${SRC}" "${DST}"
INNER
)

# Report what a slim tree would weigh, without writing one. Sums du of the
# allowlisted paths -- du counts hardlinks once, matching how the source is
# measured, so the two numbers are comparable.
if ((DRY_RUN)); then
  echo "[slim] measuring the allowlist (nothing is written)"
  {
    printf '%s\n' "${OUTPUT_DIR}/host" "${OUTPUT_DIR}/target" "${OUTPUT_DIR}/images"
    for g in "${KEEP_FULL_GLOBS[@]}"; do
      # shellcheck disable=SC2086
      ls -d ${OUTPUT_DIR}/build/${g} 2>/dev/null || true
    done
  } | xargs du -sh
  echo "[slim] allowlisted total:"
  {
    printf '%s\n' "${OUTPUT_DIR}/host" "${OUTPUT_DIR}/target" "${OUTPUT_DIR}/images"
    for g in "${KEEP_FULL_GLOBS[@]}"; do
      # shellcheck disable=SC2086
      ls -d ${OUTPUT_DIR}/build/${g} 2>/dev/null || true
    done
  } | xargs du -shc | tail -1
  echo "[slim] full tree:"
  du -sh "${OUTPUT_DIR}"
  echo "[slim] (dotfile-only package stubs add a few MB on top of the total above)"
  exit 0
fi

echo "[slim] copying as root inside a container (preserves uid/gid/mode)"
docker run --rm \
  -v "${TREE_PARENT}:/work" \
  -e "SRC_NAME=${SRC_NAME}" \
  -e "DEST_NAME=${DEST_NAME}" \
  -e "KEEP_FULL=${KEEP_FULL_GLOBS[*]}" \
  -e "KEEP_FILES=${KEEP_FILE_GLOBS[*]}" \
  debian:12-slim \
  bash -c "${copy_script}"

echo "[slim] OK -> ${DEST}"
echo "[slim] validate it before baking: docs/knowledge/prebake-tree-slimming.md"
