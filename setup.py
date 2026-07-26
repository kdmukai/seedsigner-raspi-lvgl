from __future__ import annotations

from pathlib import Path
import glob
import os
import shlex

from setuptools import Extension, setup
from setuptools.command.build_ext import build_ext as _build_ext

ROOT = Path(__file__).resolve().parent

DEFAULT_CMODULES = ROOT / "sources" / "seedsigner-lvgl-screens"
SEEDSIGNER_LVGL_SCREENS_DIR = Path(os.environ.get("SEEDSIGNER_LVGL_SCREENS_DIR", str(DEFAULT_CMODULES))).resolve()

_lvgl_env = os.environ.get("LVGL_ROOT")
if _lvgl_env:
    LVGL_ROOT = Path(_lvgl_env).resolve()
else:
    LVGL_ROOT = (SEEDSIGNER_LVGL_SCREENS_DIR / "third_party" / "lvgl").resolve()

SEEDSIGNER_DIR = SEEDSIGNER_LVGL_SCREENS_DIR / "components" / "seedsigner"
NLOHMANN_JSON_INCLUDE_DIR = SEEDSIGNER_LVGL_SCREENS_DIR / "components" / "nlohmann_json" / "include"

if not (SEEDSIGNER_DIR / "screens").is_dir():
    raise RuntimeError(
        f"Missing screens/ under {SEEDSIGNER_DIR} -- submodule not checked out, "
        f"or pinned before the seedsigner.cpp split reorg."
    )
if not (LVGL_ROOT / "lvgl.h").exists():
    raise RuntimeError(f"Missing lvgl.h under {LVGL_ROOT}")

lvgl_sources_all = glob.glob(str(LVGL_ROOT / "src" / "**" / "*.c"), recursive=True)

# Exclude architecture/accelerator-specific backends that can force higher CPU attrs
# and are not needed for current Pi Zero portability path.
EXCLUDE_SUBSTRINGS = [
    # Hardware-specific draw backends (not needed for Pi Zero SW rendering)
    "/src/draw/dma2d/",
    "/src/draw/espressif/",
    "/src/draw/eve/",
    "/src/draw/nanovg/",
    "/src/draw/nema_gfx/",
    "/src/draw/nxp/",
    "/src/draw/opengles/",
    "/src/draw/renesas/",
    "/src/draw/sdl/",
    "/src/draw/vg_lite/",
    # Architecture-specific SW blend backends (ARMv7+, RISC-V, etc.)
    "/src/draw/sw/blend/neon/",
    "/src/draw/sw/blend/helium/",
    "/src/draw/sw/blend/arm2d/",
    "/src/draw/sw/blend/riscv_v/",
    "/src/draw/sw/arm2d/",
]

lvgl_sources = [
    s for s in lvgl_sources_all
    if not any(ex in s for ex in EXCLUDE_SUBSTRINGS)
]

font_sources = [
    # Baked Western-Latin floor. On feat/font-i18n the five translated text roles
    # (title/button/body/...) are no longer pre-rasterized bitmaps; they are
    # rasterized at runtime via tiny_ttf from one compiled-in OpenSans Western TTF
    # subset (Regular + SemiBold), all sizes from the same blob. See
    # gui_constants.cpp (the post-lv_init() per-role tiny_ttf registration loop).
    "opensans_western_regular.c",
    "opensans_western_semibold.c",
    # Icons (PUA) + fixed-width Inconsolata keyboard/text-entry font stay bitmap
    # (240px profile — matches the host's SUPPORT_DISPLAY_HEIGHT_240 build).
    "seedsigner_icons_24_4bpp.c",
    "seedsigner_icons_26_4bpp.c",  # top_nav contextual title icon (gui_constants 240 profile)
    "seedsigner_icons_36_4bpp.c",
    "seedsigner_icons_48_4bpp.c",
    "inconsolata_semibold_24_4bpp.c",  # keyboard/text-entry = 24px
    # Seed-word candidate font for seed_mnemonic_entry_screen = 22px
    # (button_font_size + 4). Referenced by the 240 profile in gui_constants.cpp;
    # without it the .so fails to dlopen (undefined inconsolata_semibold_22_4bpp).
    "inconsolata_semibold_22_4bpp.c",
]

font_paths = [str(SEEDSIGNER_DIR / "fonts" / f) for f in font_sources]

# --- Display-height profile -------------------------------------------------
# The native screens bake one logo asset per display-height profile (100x=240px,
# 133x=320px, 200x=480px) and pick the variant at runtime by px_multiplier.
# gui_constants.{h,cpp} #ifdef-gate the LV_IMAGE_DECLAREs and the get_logo()
# selectors on SUPPORT_DISPLAY_HEIGHT_<N>, so a build references only its own
# profile's logo symbols. The image .c files themselves are NOT guarded — each
# unconditionally defines its symbol — so the source list is what gates them.
# Unlike the firmware (CMake lists every variant and drops the unreferenced ones
# at link via --gc-sections), this extension does not link with --gc-sections, so
# it must compile in ONLY the active profile's image .c files. Derive both the
# SUPPORT_DISPLAY_HEIGHT_<N> macro and the image source set from one height value.
# The Pi panel is 240px tall (240x240; a 320x240 width switch keeps height 240).
DISPLAY_HEIGHT = int(os.environ.get("DISPLAY_HEIGHT", "240"))

# height -> image-asset filename suffix (matches the px_multiplier: 100/133/200).
_LOGO_SUFFIX_BY_HEIGHT = {240: "", 320: "_133x", 480: "_200x"}
if DISPLAY_HEIGHT not in _LOGO_SUFFIX_BY_HEIGHT:
    raise RuntimeError(
        f"Unsupported DISPLAY_HEIGHT={DISPLAY_HEIGHT}; expected one of "
        f"{sorted(_LOGO_SUFFIX_BY_HEIGHT)}"
    )
_logo_suffix = _LOGO_SUFFIX_BY_HEIGHT[DISPLAY_HEIGHT]

# Base SeedSigner wordmark (screensaver + splash) + HRF partner logo (splash) +
# Bitcoin logo (loading_spinner_screen spinner), for the active height only. Each is
# #ifdef-gated in gui_constants.cpp on SUPPORT_DISPLAY_HEIGHT_<N>, so only the
# active profile's .c must be compiled in (missing symbols fail at dlopen).
logo_sources = [
    str(SEEDSIGNER_DIR / "images" / f"seedsigner_logo_img{_logo_suffix}.c"),
    str(SEEDSIGNER_DIR / "images" / f"hrf_logo_img{_logo_suffix}.c"),
    str(SEEDSIGNER_DIR / "images" / f"btc_logo_img{_logo_suffix}.c"),
]

cross_build = os.environ.get("CROSS_BUILD", "0") == "1"
armv6_force = os.environ.get("ARMV6_FORCE", "0") == "1"
python_target_include = os.environ.get("PYTHON_TARGET_INCLUDE", "").strip()
python_target_libdir = os.environ.get("PYTHON_TARGET_LIBDIR", "").strip()
python_target_ldlibrary = os.environ.get("PYTHON_TARGET_LDLIBRARY", "").strip()

# --- Native camera engine (default) -----------------------------------------
# The native libcamera C++ capture engine (native/camera/) is the DEFAULT build:
# SeedSigner OS #114 is the target, and the native camera pipeline (scan +
# image-entropy) is the main approach. It links the engine into the
# seedsigner_lvgl_screens extension, which shares libcamera's C++ objects across
# the library boundary, so the whole extension MUST link SHARED libstdc++ (a
# static copy would split the ABI) — the #114 device provides libstdc++.so.6.0.32.
# libcamera + its headers come from the target sysroot the build supplies via
# CAMERA_SYSROOT -- the SeedSigner OS SDK image points it at /output/staging (see
# docker/build_steps.sh and docs/knowledge/armv6-cross-compile-sdk.md).
# CAMERA_ENGINE=0 opts out for a no-camera diagnostic build (static libstdc++, no
# libcamera, no camera_scanner/camera_entropy submodules).
camera_engine = os.environ.get("CAMERA_ENGINE", "1") == "1"
CAMERA_SYSROOT = Path(
    os.environ.get("CAMERA_SYSROOT", str(ROOT / "sysroot" / "pi0-dev"))
).resolve()

include_dirs = [
    str(LVGL_ROOT),
    str(SEEDSIGNER_DIR),
    str(NLOHMANN_JSON_INCLUDE_DIR),
    # camera_preview.cpp always includes camera_preview_sink.h (Python-free bridge);
    # the header is harmless in the default build (its impls are just never called).
    str(ROOT / "native" / "camera"),
]

extra_link_args: list[str] = []
# uUR is a SEPARATE pure-C extension — it must NEVER inherit the C++ extension's
# camera link args (-lcamera/-lzbar/...), which would give uUR.so a libcamera
# DT_NEEDED and make `import uUR` fail anywhere off the device. It carries only its
# own portability + cross-build link args.
uur_link_args: list[str] = []
extra_compile_args: list[str] = ["-std=c++17"]
# Per-target codegen. TARGET_CFLAGS (set by docker/build_steps.sh from
# versions.lock.<profile>.toml's [toolchain].cflags) is the general path, applied
# verbatim -- e.g. the pi02w A53+NEON profile. When it is unset, the ARMV6_FORCE
# path supplies the armv6 flags (unchanged), so the default build is byte-identical.
target_cflags = os.environ.get("TARGET_CFLAGS", "").strip()
if target_cflags or armv6_force:
    if target_cflags:
        extra_compile_args.extend(shlex.split(target_cflags))
    else:
        # Codegen flags come from versions.lock.toml via docker/build_steps.sh
        # (ARMV6_* env); the fallbacks only serve a bare ARMV6_FORCE=1 invocation
        # outside the locked build.
        extra_compile_args.extend([
            f"-march={os.environ.get('ARMV6_ARCH', 'armv6zk')}",
            f"-mtune={os.environ.get('ARMV6_TUNE', 'arm1176jzf-s')}",
            "-marm",
            f"-mfpu={os.environ.get('ARMV6_FPU', 'vfp')}",
            f"-mfloat-abi={os.environ.get('ARMV6_FLOAT_ABI', 'hard')}",
        ])
    # uUR is pure C (no libstdc++); static libgcc keeps it self-contained on the
    # device regardless of the main extension's camera/no-camera linkage.
    uur_link_args.append("-static-libgcc")
    # Avoid runtime dependency on host libstdc++/libgcc symbol versions.
    # This is critical for Pi Zero targets that often have older system toolchains.
    # EXCEPTION: the default (camera) build must link libstdc++ SHARED (see above),
    # so the static flip is skipped there — the device provides libstdc++.so.6.0.32.
    if not camera_engine:
        extra_link_args.extend([
            "-static-libstdc++",
            "-static-libgcc",
        ])

# libcamera link recipe (validated end-to-end by scripts/build-camera-probe.sh):
# sysroot include + libs, --allow-shlib-undefined because the sysroot libs reference
# GLIBC/GLIBCXX version nodes the build container's runtime lacks but the device has.
camera_sources: list[str] = []
if camera_engine:
    cam_lib = str(CAMERA_SYSROOT / "usr" / "lib")
    # libcamera headers live under usr/include/libcamera (code uses <libcamera/...>);
    # zbar.h is directly under usr/include.
    include_dirs.append(str(CAMERA_SYSROOT / "usr" / "include" / "libcamera"))
    include_dirs.append(str(CAMERA_SYSROOT / "usr" / "include"))
    # Top-level native/camera/*.cpp only (engine + scan_coordinator); the probe/
    # subdir is a standalone tool, not part of the extension.
    camera_sources = sorted(glob.glob(str(ROOT / "native" / "camera" / "*.cpp")))
    extra_link_args.extend([
        f"-L{cam_lib}",
        "-lcamera",
        "-lcamera-base",
        "-lzbar",
        "-Wl,--allow-shlib-undefined",
        f"-Wl,-rpath-link,{cam_lib}",
    ])
if cross_build and python_target_include:
    include_dirs.insert(0, python_target_include)
if cross_build and python_target_libdir:
    extra_link_args.append(f"-L{python_target_libdir}")
    uur_link_args.append(f"-L{python_target_libdir}")
if cross_build and python_target_ldlibrary:
    if python_target_ldlibrary.startswith("lib") and python_target_ldlibrary.endswith((".a", ".so")):
        libname = python_target_ldlibrary[3:].split(".")[0]
        extra_link_args.append(f"-l{libname}")
        uur_link_args.append(f"-l{libname}")

# Portable seedsigner component sources. The screens repo replaced its monolithic
# seedsigner.cpp with one file per screen under screens/ plus shared helpers
# (qr_core / screen_helpers / screen_scaffold). Every top-level .cpp/.c and every
# screens/*_screen.cpp under components/seedsigner/ is portable and compiled in --
# mirroring components/seedsigner/screen_sources.cmake (screens glob) so new screens
# are picked up mechanically, with no ESP32-only translation units in this dir to
# exclude. Camera *overlay* / *overlay_screen sources are UI-only (no camera driver)
# and link in even though the camera screens are not bound to Python. Sorted for a
# deterministic link order.
seedsigner_sources = sorted(
    glob.glob(str(SEEDSIGNER_DIR / "*.cpp"))
    + glob.glob(str(SEEDSIGNER_DIR / "*.c"))
    + glob.glob(str(SEEDSIGNER_DIR / "screens" / "*_screen.cpp"))
)

# Pi platform backend + Python bindings, one subsystem per file (module.cpp is
# the method table; see native/python_bindings/module_internal.h for the map).
# camera_scanner.cpp / camera_entropy.cpp reference the libcamera engines
# unconditionally, so they are only compiled in the CAMERA_ENGINE build (their engine
# sources are in camera_sources); exclude them from the default glob so the default
# extension has no undefined engine symbols.
_engine_only_bindings = {"camera_scanner.cpp", "camera_entropy.cpp"}
binding_sources = sorted(
    s for s in glob.glob(str(ROOT / "native" / "python_bindings" / "*.cpp"))
    if camera_engine or os.path.basename(s) not in _engine_only_bindings
)

# --- Native cUR (BC-UR) -> the `uUR` extension ------------------------------
# Compiled from the sources/cUR submodule into a SEPARATE CPython module named
# `uUR`, mirroring the ESP32 firmware's native uUR module so the app's
# __import__("uUR") gets native BC-UR encode/decode on the Pi. Pure C (no C++,
# no libstdc++); bundled SHA-256 (the host path — UR_USE_MBEDTLS_SHA256 is NOT
# defined, so it uses src/sha256/sha256.c instead of mbedTLS).
CUR_DIR = Path(os.environ.get("CUR_DIR", str(ROOT / "sources" / "cUR"))).resolve()
if not (CUR_DIR / "python" / "uUR.c").exists():
    raise RuntimeError(
        f"Missing cUR CPython binding under {CUR_DIR} -- submodule not checked out "
        f"(run: git submodule update --init sources/cUR)."
    )
# cUR root ('.') satisfies the binding's `src/...` includes; 'src' satisfies the
# core's sibling includes (e.g. "sha256/sha256.h", "ur_decoder.h").
cur_sources = [str(CUR_DIR / "python" / "uUR.c")] + sorted(
    glob.glob(str(CUR_DIR / "src" / "*.c"))
    + glob.glob(str(CUR_DIR / "src" / "types" / "*.c"))
    + glob.glob(str(CUR_DIR / "src" / "sha256" / "sha256.c"))
)

# C++-only flags to strip when compiling C translation units — used by both the
# custom build_ext (per-.c) and the pure-C uUR extension's args.
CXX_ONLY_FLAGS = {"-std=c++17"}

ext_modules = [
    Extension(
        "seedsigner_lvgl_screens",
        sources=[
            *binding_sources,
            # Native libcamera capture engine (empty unless CAMERA_ENGINE=1).
            *camera_sources,
            # Every portable seedsigner component + per-screen source (see the
            # seedsigner_sources glob above).
            *seedsigner_sources,
            *logo_sources,
            *font_paths,
            *lvgl_sources,
        ],
        include_dirs=include_dirs,
        define_macros=[
            ("LV_CONF_SKIP", "1"),
            ("LV_USE_DRAW_SW_ASM", "LV_DRAW_SW_ASM_NONE"),
            # Use glibc malloc, not LVGL's 64KB builtin fixed pool (the LV_CONF_SKIP
            # default). The shared font code enables the tiny_ttf glyph cache
            # (SEEDSIGNER_TTF_CACHE_SIZE), which retains rasterized bitmaps; a 64KB
            # pool OOMs and LVGL's default assert handler spins. The Pi Zero has
            # 512MB, but LVGL only sees it through CLIB malloc. See
            # docs/knowledge/tiny-ttf-cache-needs-clib-malloc.md.
            ("LV_USE_STDLIB_MALLOC", "LV_STDLIB_CLIB"),
            # Runtime font loading: per-locale subset TTFs (and the baked OpenSans
            # Western floor) are rasterized from in-memory buffers via
            # lv_tiny_ttf_create_data_ex() — the registration seam the loader drives.
            ("LV_USE_TINY_TTF", "1"),
            # Farsi/Arabic i18n: BIDI = right-to-left reordering of mixed RTL/LTR
            # text; ARABIC_PERSIAN_CHARS = cursive base-letter -> presentation-form
            # shaping. Both must match the font packs (subset to the shaper's forms).
            ("LV_USE_BIDI", "1"),
            ("LV_USE_ARABIC_PERSIAN_CHARS", "1"),
            # Native QR rendering (qr_display_screen). seedsigner.cpp calls the
            # LVGL-bundled Nayuki qrcodegen directly (not the lv_qrcode widget) to
            # control ECC/mode/quiet-zone; both qrcodegen.c and lv_qrcode.c are
            # already picked up by the LVGL src glob, so only the flag is needed.
            # Without it the screen compiles to a blank stub (#if !LV_USE_QRCODE).
            ("LV_USE_QRCODE", "1"),
            (f"SUPPORT_DISPLAY_HEIGHT_{DISPLAY_HEIGHT}", "1"),
            *(
                [("LV_USE_SYSMON", "1"), ("LV_USE_PERF_MONITOR", "1")]
                if os.environ.get("LVGL_PERF_MONITOR", "0") == "1"
                else []
            ),
            # Gate the pump-path consume hook + the camera_scanner submodule attach.
            *([("SS_CAMERA_ENGINE", "1")] if camera_engine else []),
        ],
        extra_compile_args=extra_compile_args,
        extra_link_args=extra_link_args,
        language="c++",
    ),
    # Native cUR -> the `uUR` module. Pure C: strip the C++-only std flag (the
    # custom build_ext also drops it per-.c, but keep this extension's own args
    # clean). Uses uur_link_args (ARMv6 static-libgcc + cross-build) — NOT the main
    # extension's camera link args, so uUR.so has no libcamera DT_NEEDED.
    Extension(
        "uUR",
        sources=cur_sources,
        include_dirs=[str(CUR_DIR), str(CUR_DIR / "src")],
        extra_compile_args=[a for a in extra_compile_args if a not in CXX_ONLY_FLAGS],
        extra_link_args=uur_link_args,
        language="c",
    ),
]


class build_ext(_build_ext):
    """Strip C++-only flags when compiling C source files."""

    def build_extensions(self):
        _original_compile = self.compiler._compile

        def _compile(obj, src, ext, cc_args, extra_postargs, pp_opts):
            if src.endswith(".c"):
                extra_postargs = [a for a in extra_postargs if a not in CXX_ONLY_FLAGS]
            return _original_compile(obj, src, ext, cc_args, extra_postargs, pp_opts)

        self.compiler._compile = _compile
        super().build_extensions()


setup(ext_modules=ext_modules, cmdclass={"build_ext": build_ext})
