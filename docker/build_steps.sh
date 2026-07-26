#!/usr/bin/env bash
# Cross-compile the Pi Zero extensions inside the SeedSigner OS SDK image.
# ============================================================================
# Runs on x86 at native speed: the compiler is the SeedSigner OS buildroot cross
# toolchain and the link target is that same buildroot's sysroot, so the artifact
# is built BY the device's toolchain AGAINST the device's libraries. No QEMU.
#
# The SDK image (docker/Dockerfile.sdk) provides everything at fixed paths:
#   /output/host      buildroot cross toolchain + host Python 3.12 (setuptools, pytest)
#   /output/staging   target sysroot (symlink into host/): glibc, libstdc++,
#                     Python 3.12 headers + sysconfigdata, libcamera, zbar
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_TS="${RUN_TS:-$(date -u +%Y%m%d-%H%M%S)}"

echo "[build] run_ts=${RUN_TS}"

ABI_JSON="${ABI_JSON:-${ROOT_DIR}/docs/abi/dev-pi-abi.json}"
LOCK_FILE="${LOCK_FILE:-${ROOT_DIR}/versions.lock.toml}"

SDK_HOST="${SDK_HOST:-/output/host}"
SDK_SYSROOT="${SDK_SYSROOT:-/output/staging}"
CROSS_TUPLE="${CROSS_TUPLE:-arm-Buildroot-linux-gnueabihf}"
PYTHON_BIN="${PYTHON_BIN:-${SDK_HOST}/bin/python3}"

[[ -x "${PYTHON_BIN}" ]] || { echo "ERROR: SDK python missing at ${PYTHON_BIN}" >&2; exit 10; }
[[ -x "${SDK_HOST}/bin/${CROSS_TUPLE}-g++" ]] \
  || { echo "ERROR: cross toolchain missing at ${SDK_HOST}/bin/${CROSS_TUPLE}-g++" >&2; exit 10; }

# Record WHICH SeedSigner OS release this sysroot came from, in every build log.
if [[ -f /output/SS_OS_PROVENANCE ]]; then
  echo "[build] SDK provenance:"
  sed 's/^/[build]   /' /output/SS_OS_PROVENANCE
fi

# --- Cross environment --------------------------------------------------------
# The target sysconfigdata reports DEVICE-absolute paths (LIBDIR=/usr/lib,
# INCLUDEPY=/usr/include/python3.12). Handing those to distutils yields
# -L/usr/lib / -I/usr/include, which on this x86 build host are the HOST dirs --
# the buildroot wrapper rightly rejects them ("unsafe header/library path used in
# cross-compilation"). So emit a patched copy that prefixes the sysroot onto every
# value beginning with /usr. This is what crossenv does internally; done inline so
# the mechanism stays visible and dependency-free.
#
# Values with a NON-leading /usr (e.g. MODULE_*_LDFLAGS, already pointing at
# .../sysroot/usr/lib) begin with -L, not /usr, so they are correctly untouched.
SYSCONFIG_SRC="$(ls "${SDK_SYSROOT}"/usr/lib/python3.12/_sysconfigdata_*.py | head -n1)"
SYSCONFIG_NAME="$(basename "${SYSCONFIG_SRC}" .py)"
XSC_DIR="${XSC_DIR:-/tmp/xsc}"
mkdir -p "${XSC_DIR}"
"${PYTHON_BIN}" - "${SYSCONFIG_SRC}" "${SDK_SYSROOT}" "${XSC_DIR}/${SYSCONFIG_NAME}.py" <<'PY'
import runpy, sys
src, sysroot, dest = sys.argv[1:4]
v = runpy.run_path(src)["build_time_vars"]
patched = {k: (sysroot + val if isinstance(val, str) and val.startswith("/usr") else val)
           for k, val in v.items()}
with open(dest, "w") as f:
    f.write("build_time_vars = %r\n" % (patched,))
print(f"[build] sysconfigdata patched: LIBDIR {v['LIBDIR']} -> {patched['LIBDIR']}")
PY

# With this in place the HOST interpreter reports TARGET ABI facts, so the ABI
# gate below and setuptools' artifact naming both come out target-correct.
export _PYTHON_SYSCONFIGDATA_NAME="${SYSCONFIG_NAME}"
export PYTHONPATH="${XSC_DIR}"

# setup.py's cross hooks: resolve Python.h / libs inside the sysroot. No
# PYTHON_TARGET_LDLIBRARY -- extension modules leave Python symbols undefined at
# link (the interpreter resolves them at dlopen), so libpython is never linked.
export CROSS_BUILD=1
export PYTHON_TARGET_INCLUDE="${SDK_SYSROOT}/usr/include/python3.12"
export PYTHON_TARGET_LIBDIR="${SDK_SYSROOT}/usr/lib"
# Native camera engine links libcamera/zbar from this same sysroot.
export CAMERA_SYSROOT="${CAMERA_SYSROOT:-${SDK_SYSROOT}}"

# --- ABI gate -----------------------------------------------------------------
# Asserts the ABI the SDK will actually emit matches versions.lock.toml and the
# device capture. Unchanged from the emulated build: the sysconfigdata swap above
# makes the running interpreter report the TARGET SOABI/EXT_SUFFIX.
"${PYTHON_BIN}" - <<'PY' "${ABI_JSON}" "${LOCK_FILE}"
import json, platform, sysconfig, sys, tomllib
abi_json, lock_file = sys.argv[1:3]
with open(abi_json, 'r', encoding='utf-8') as f:
    target = json.load(f)
with open(lock_file, 'rb') as f:
    lock = tomllib.load(f)

soabi = sysconfig.get_config_var('SOABI')
ext_suffix = sysconfig.get_config_var('EXT_SUFFIX')

lock_soabi = lock.get('target_abi', {}).get('python_soabi')
lock_ext = lock.get('target_abi', {}).get('python_ext_suffix')
tc = lock.get('toolchain', {})

print('[build] build host machine (x86 cross host)=', platform.machine())
print('[build] emitted soabi=', soabi)
print('[build] emitted ext_suffix=', ext_suffix)
print('[build] target_soabi(json)=', target.get('soabi'))
print('[build] lock_soabi=', lock_soabi)

if target.get('soabi') != lock_soabi:
    raise SystemExit(f"ABI JSON vs lock mismatch for SOABI: {target.get('soabi')} != {lock_soabi}")
if target.get('ext_suffix') != lock_ext:
    raise SystemExit(f"ABI JSON vs lock mismatch for EXT_SUFFIX: {target.get('ext_suffix')} != {lock_ext}")
if soabi != lock_soabi:
    raise SystemExit(f"SOABI mismatch: SDK emits {soabi}, lock expects {lock_soabi}")
if ext_suffix != lock_ext:
    raise SystemExit(f"EXT_SUFFIX mismatch: SDK emits {ext_suffix}, lock expects {lock_ext}")

if not all(tc.get(k) for k in ('arch', 'tune', 'fpu', 'float_abi')):
    raise SystemExit('Lock file missing required toolchain fields')
print('[build] ABI gate OK')
PY

# --- Toolchain ----------------------------------------------------------------
# Codegen from the lock (the buildroot toolchain already defaults to this target;
# passing it keeps versions.lock.<profile>.toml the single source of truth).
# [toolchain].cflags (an explicit list, e.g. pi02w's A53+NEON) is the general path,
# applied verbatim by setup.py via TARGET_CFLAGS; when it is absent the armv6
# ARMV6_* env supplies the flags (byte-identical to before). CPU_ARCH_REGEX is the
# ELF Tag_CPU_arch the artifact gate then requires this build to emit.
eval "$("${PYTHON_BIN}" - <<'PY' "${LOCK_FILE}"
import sys, tomllib, shlex
lock = tomllib.load(open(sys.argv[1], 'rb'))
tc = lock['toolchain']; abi = lock.get('target_abi', {})
def q(k, v): print(f"{k}={shlex.quote(str(v))}")
q('ARCH', tc['arch']); q('TUNE', tc['tune']); q('FPU', tc['fpu']); q('FLOAT_ABI', tc['float_abi'])
q('LOCK_MAX_GLIBC', abi.get('max_glibc', 'GLIBC_2.28'))
q('LOCK_MAX_GLIBCXX', abi.get('max_glibcxx', 'GLIBCXX_3.4.21'))
q('TARGET_CFLAGS', ' '.join(tc['cflags']) if tc.get('cflags') else '')
q('CPU_ARCH_REGEX', tc.get('cpu_arch_regex', 'v6|v6KZ'))
PY
)"

if [[ -n "${TARGET_CFLAGS}" ]]; then
  export TARGET_CFLAGS                 # setup.py applies these verbatim
else
  export ARMV6_FORCE=1
  export ARMV6_ARCH="${ARCH}" ARMV6_TUNE="${TUNE}" ARMV6_FPU="${FPU}" ARMV6_FLOAT_ABI="${FLOAT_ABI}"
fi

# ccache wraps the CROSS compiler (not host gcc) so repeat CI runs reuse objects.
# Assign unconditionally -- a "${CC:-...}" default would silently lose to an
# inherited CC and run the build uncached.
if command -v ccache >/dev/null 2>&1; then
  export CC="ccache ${CROSS_TUPLE}-gcc"
  export CXX="ccache ${CROSS_TUPLE}-g++"
  # Pin the cache dir to the mounted volume. The SDK image sets CCACHE_DIR for
  # this reason (buildroot's own ccache is first on PATH and would otherwise
  # cache into an unmounted, container-local dir); re-assert it here so the
  # build is correct even under a hand-rolled `docker run`.
  export CCACHE_DIR="${CCACHE_DIR:-/root/.cache/ccache}"
  ccache --max-size=2G >/dev/null 2>&1 || true
  ccache --zero-stats >/dev/null 2>&1 || true
  # Log which binary and dir are in play: a cache writing to the wrong place is
  # invisible except as a permanently 0% hit rate, which is what this catches.
  echo "[build] ccache enabled: $(command -v ccache) -> cache_dir=$(ccache -p 2>/dev/null | sed -n 's/.*cache_dir = //p' | head -1)"
else
  export CC="${CROSS_TUPLE}-gcc"
  export CXX="${CROSS_TUPLE}-g++"
fi
echo "[build] compiler CC=${CC} CXX=${CXX}"

# --- Build --------------------------------------------------------------------
# The package dir (pyproject: package-dir {"" = "src"}) holds only the built,
# git-ignored .so, so a fresh checkout has no src/ at all. build_ext --inplace
# writes into src/; a non-armv6 profile's .so has the SAME SOABI/filename, so it
# is moved into src/<profile>/ after the gates (below) to coexist with armv6's.
if [[ "${TARGET_PROFILE:-armv6}" == "armv6" ]]; then OUT_DIR="${ROOT_DIR}/src"; else OUT_DIR="${ROOT_DIR}/src/${TARGET_PROFILE}"; fi
mkdir -p "${ROOT_DIR}/src" "${OUT_DIR}"
rm -f "${OUT_DIR}"/seedsigner_lvgl_screens*.so "${OUT_DIR}"/uUR*.so
rm -f "${ROOT_DIR}"/src/seedsigner_lvgl_screens*.so "${ROOT_DIR}"/src/uUR*.so

BUILD_DIR="${BUILD_DIR:-/tmp/build}"
rm -rf "${BUILD_DIR}"
"${PYTHON_BIN}" "${ROOT_DIR}/setup.py" build_ext --inplace --force \
  --parallel "${JOBS:-$(nproc)}" \
  --build-temp "${BUILD_DIR}/temp" --build-lib "${BUILD_DIR}/lib"

# Cache effectiveness is load-bearing for CI wall-clock: surface hit/miss stats in
# every log so a silently-dead cache (empty dir, defeated CC) is visible.
if command -v ccache >/dev/null 2>&1; then
  echo "[build] ccache stats after build:"
  ccache --show-stats || true
fi

# Native cases self-skip: an ARMv6 .so cannot be imported on this x86 host (the
# guard is `except ImportError`, since a built-but-unloadable extension raises
# plain ImportError). Functional validation happens on-device.
#
# dist-packages carries Debian's pure-Python pytest, imported by the SDK's
# buildroot Python 3.12 (it has no ssl, so pip/PyPI are unavailable). Added for
# THIS command only -- leaving it on PYTHONPATH during the build would let
# Debian's 3.11-targeted setuptools shadow the buildroot interpreter's own.
PYTHONPATH="${ROOT_DIR}/src:${PYTHONPATH}:/usr/lib/python3/dist-packages" \
  "${PYTHON_BIN}" -m pytest -q -p no:cacheprovider "${ROOT_DIR}/tests"

# --- Artifact gates -----------------------------------------------------------
MAX_GLIBCXX="${MAX_GLIBCXX:-${LOCK_MAX_GLIBCXX}}"
MAX_GLIBC="${MAX_GLIBC:-${LOCK_MAX_GLIBC}}"

# Every runtime-compat gate on one built .so: target EXT_SUFFIX, ARMv6 CPU arch,
# and the GLIBCXX/GLIBC symbol-version ceilings (a too-new ref only ever fails at
# dlopen on the device). Applied to BOTH extensions this build emits.
verify_artifact() {
  local ART="$1" label="$2"
  echo "[build] --- verifying ${label}: $(basename "${ART}") ---"

  "${PYTHON_BIN}" - <<'PY' "${ABI_JSON}" "${ART}"
import json,os,sys
abi_json,art=sys.argv[1:3]
with open(abi_json,'r',encoding='utf-8') as f:
    target=json.load(f)
ext=target.get('ext_suffix')
if ext and not art.endswith(ext):
    raise SystemExit(f"artifact suffix mismatch: {os.path.basename(art)} != *{ext}")
print('[build] artifact suffix OK')
PY

  file "${ART}" || true

  if command -v readelf >/dev/null 2>&1; then
    local READELF_OUT
    READELF_OUT="$(readelf -A "${ART}")"
    echo "${READELF_OUT}"
    if ! echo "${READELF_OUT}" | grep -Eq "Tag_CPU_arch: (${CPU_ARCH_REGEX})"; then
      echo "ERROR: ${label} artifact CPU arch (Tag_CPU_arch) does not match required '${CPU_ARCH_REGEX}'" >&2
      exit 7
    fi
  fi

  local FOUND_GLIBCXX
  FOUND_GLIBCXX="$(strings "${ART}" | grep -o 'GLIBCXX_[0-9.]*' | sort -V | tail -n1 || true)"
  if [[ -n "${FOUND_GLIBCXX}" ]]; then
    echo "[build] ${label} max_glibcxx_required=${FOUND_GLIBCXX} allowed=${MAX_GLIBCXX}"
    if [[ "$(printf '%s\n%s\n' "${MAX_GLIBCXX}" "${FOUND_GLIBCXX}" | sort -V | tail -n1)" != "${MAX_GLIBCXX}" ]]; then
      echo "ERROR: ${label} GLIBCXX requirement too new for target runtime: ${FOUND_GLIBCXX} > ${MAX_GLIBCXX}" >&2
      exit 8
    fi
  else
    echo "[build] ${label} no GLIBCXX symbols found (pure C / static libstdc++)"
  fi

  local FOUND_GLIBC
  FOUND_GLIBC="$(strings "${ART}" | grep -o 'GLIBC_[0-9.]*' | sort -V | tail -n1 || true)"
  if [[ -n "${FOUND_GLIBC}" ]]; then
    echo "[build] ${label} max_glibc_required=${FOUND_GLIBC} allowed=${MAX_GLIBC}"
    if [[ "$(printf '%s\n%s\n' "${MAX_GLIBC}" "${FOUND_GLIBC}" | sort -V | tail -n1)" != "${MAX_GLIBC}" ]]; then
      echo "ERROR: ${label} GLIBC requirement too new for target runtime: ${FOUND_GLIBC} > ${MAX_GLIBC}" >&2
      exit 9
    fi
  else
    echo "[build] ${label} no GLIBC versioned symbols found"
  fi
}

# Move the freshly built .so(s) from src/ (build_ext --inplace target) into the
# profile output dir (no-op for armv6), then verify them there.
if [[ "${OUT_DIR}" != "${ROOT_DIR}/src" ]]; then
  mv "${ROOT_DIR}"/src/seedsigner_lvgl_screens*.so "${OUT_DIR}/"
  mv "${ROOT_DIR}"/src/uUR*.so "${OUT_DIR}/"
fi

MAIN_ART="$(find "${OUT_DIR}" -maxdepth 1 -type f -name 'seedsigner_lvgl_screens*.so' | sort | tail -n1 || true)"
if [[ -z "${MAIN_ART}" ]]; then
  echo "ERROR: no built seedsigner_lvgl_screens extension artifact found" >&2
  exit 6
fi
verify_artifact "${MAIN_ART}" "seedsigner_lvgl_screens"

UUR_ART="$(find "${OUT_DIR}" -maxdepth 1 -type f -name 'uUR*.so' | sort | tail -n1 || true)"
if [[ -z "${UUR_ART}" ]]; then
  echo "ERROR: no built uUR extension artifact found" >&2
  exit 6
fi
verify_artifact "${UUR_ART}" "uUR"

echo "[build] OK"
