// toast.cpp — Pi Zero binding for the native LVGL toast overlay.
//
// The portable toast (components/seedsigner/toast_overlay.{h,cpp} + the
// overlay_manager toast queue) is a transient bottom-pinned banner built on the
// display's TOP LAYER (lv_layer_top()), so it composites over whatever screen is
// live and survives screen swaps. It joins no input group (never steals focus) and
// is dismissed natively — auto after duration_ms, or by ANY hardware key/joystick
// press (non-consuming: the press both hides the toast and drives the screen). The
// host does NOT poll input or pump a result for it.
//
// WHY THIS EXISTS ON THE PI: the underlying screen is now drawn by LVGL, but the
// toast used to be a PIL component that blitted a whole framebuffer. With no live PIL
// canvas to composite onto, that PIL toast painted over BLACK — blacking out the LVGL
// UI. Rendering the toast as a native LVGL widget makes it composite over the live
// screen. The rendering + dismissal logic is entirely native; this file is only the
// thin cfg-dict → toast_overlay_spec_t marshaller.
//
// POLICY-FREE: the module renders WHAT it is told (text, icon glyph, colors,
// duration). The severity → (icon, colors) mapping is the HOST's policy (Python's
// InfoToast / SuccessToast / WarningToast / ... subclasses); the app resolves it and
// passes the finished glyph + colors here. Kept name-identical to the MicroPython
// binding (show_toast / dismiss_toast) so the shared app needs no platform branch.
//
// THREADING: toasts are produced from more than one thread on the Pi — the app's SD-
// card detector runs on its own Python thread (hardware/microsd.py) and raises the
// "microSD removed/inserted" toasts, while ordinary UI toasts are raised from the main
// pump thread. show_toast() therefore goes through overlay_manager_show_toast(), the
// thread-safe producer entry point: it DEEP-COPIES the spec's strings under
// overlay_manager_lock() and hands the request to the dispatcher lv_timer, which
// realizes it on the LVGL loop (LVGL itself is not thread-safe). This file provides the
// real std::mutex those weak lock hooks default to no-ops for — see the override below,
// which is the "the Pi .so provides a real mutex" contract stated in overlay_manager.h.
#include "module_internal.h"

#include "toast_overlay.h"     // toast_overlay_spec_t, toast_overlay_dismiss
#include "overlay_manager.h"   // overlay_manager_show_toast (thread-safe producer)

#include <mutex>
#include <stdexcept>
#include <string>

// --- Cross-thread lock: the strong override of overlay_manager's weak hooks --------
// overlay_manager_lock()/unlock() are weak no-ops in overlay_manager.cpp (fine for
// single-threaded hosts: the ESP32 LVGL task, the desktop runner, web). The Pi has
// genuine cross-thread producers, so it must supply a real mutex — as documented in
// overlay_manager.h. It guards the manager's staged-toast request: the producer copies
// the spec under it, the dispatcher (an lv_timer inside the LVGL loop) drains under it.
//
// This mutex is LOAD-BEARING under RASPI-5 Phase 2: the dispatcher now runs on the
// background pump thread (GIL-free), so the GIL no longer serializes producer vs.
// dispatcher. Producers are the SD-card detector thread and any UI thread calling
// show_toast; each takes ONLY this overlay lock (O). The dispatcher runs UNDER the LVGL
// lock (L) and then takes O to drain — order L -> O. No producer ever takes L, so there
// is no L/O cycle (no AB-BA). dismiss_toast deletes widgets directly and stays LVGL-thread
// only (it takes the LVGL lock, not this one).
static std::mutex &overlay_state_mutex() {
    static std::mutex m;   // function-local static: thread-safe first-use init, no
    return m;              // static-init-order dependence on this translation unit.
}

extern "C" void overlay_manager_lock(void)   { overlay_state_mutex().lock(); }
extern "C" void overlay_manager_unlock(void) { overlay_state_mutex().unlock(); }

// --- Helpers ----------------------------------------------------------------
// Read an optional 0xRRGGBB color from the cfg dict, falling back to `dflt` when the
// key is absent. A present-but-non-integer value is a host wiring error (raises).
// Returns true on success; on failure a Python error is set and false is returned.
static bool read_optional_color(PyObject *cfg, const char *key, uint32_t dflt,
                                uint32_t *out) {
    PyObject *v = PyDict_GetItemString(cfg, key);  // borrowed; NULL if absent
    if (!v || v == Py_None) {
        *out = dflt;
        return true;
    }
    unsigned long c = PyLong_AsUnsignedLong(v);
    if (c == (unsigned long)-1 && PyErr_Occurred()) {
        return false;  // TypeError/OverflowError already set by PyLong_AsUnsignedLong
    }
    *out = (uint32_t)(c & 0xFFFFFFu);
    return true;
}

// --- show_toast(cfg) -> None ------------------------------------------------
// Raise (or replace) a toast over the live screen. cfg keys:
//   label_text    (str, required)   message; may contain '\n', soft-wraps to width.
//   icon          (str | None)      leading seedsigner-icon PUA glyph, or None/absent
//                                    for a text-only toast.
//   outline_color (int, default 0xFFFFFF)  banner outline + icon color (0xRRGGBB).
//   font_color    (int, default 0xFFFFFF)  message text color (0xRRGGBB).
//   duration_ms   (int, default 3000)      auto-dismiss delay; 0 = stay until
//                                          dismiss_toast() or a newer toast replaces it.
// Thread-safe (SD-card detector thread + pump thread both call it): routes through
// overlay_manager_show_toast(), which deep-copies under the lock and defers the build
// to the LVGL loop. The local std::strings need only outlive this call (the copy is
// taken inside overlay_manager_show_toast).
PyObject *py_show_toast(PyObject *self, PyObject *args) {
    (void)self;

    PyObject *cfg = nullptr;
    if (!PyArg_ParseTuple(args, "O", &cfg)) {
        return nullptr;
    }
    if (!PyDict_Check(cfg)) {
        PyErr_SetString(PyExc_RuntimeError, "show_toast expects cfg_dict as dict");
        return nullptr;
    }

    // label_text is the message the toast exists to show — required and localized by
    // the host (a baked literal would be English-only). Borrowed refs; copied into
    // std::string so they outlive the deep-copy inside overlay_manager_show_toast.
    PyObject *label = PyDict_GetItemString(cfg, "label_text");  // borrowed
    if (!label || !PyUnicode_Check(label)) {
        PyErr_SetString(PyExc_RuntimeError,
                        "show_toast: label_text is required and must be a string");
        return nullptr;
    }
    const char *label_utf8 = PyUnicode_AsUTF8(label);
    if (!label_utf8) {
        return nullptr;  // encoding error already set
    }
    std::string label_text(label_utf8);

    std::string icon_glyph;
    bool have_icon = false;
    PyObject *icon = PyDict_GetItemString(cfg, "icon");  // borrowed; NULL/None -> text-only
    if (icon && icon != Py_None) {
        if (!PyUnicode_Check(icon)) {
            PyErr_SetString(PyExc_RuntimeError,
                            "show_toast: icon must be a string glyph or None");
            return nullptr;
        }
        const char *icon_utf8 = PyUnicode_AsUTF8(icon);
        if (!icon_utf8) {
            return nullptr;  // encoding error already set
        }
        icon_glyph = icon_utf8;
        have_icon = true;
    }

    uint32_t outline_color = 0xFFFFFF;
    uint32_t font_color = 0xFFFFFF;
    if (!read_optional_color(cfg, "outline_color", 0xFFFFFF, &outline_color) ||
        !read_optional_color(cfg, "font_color", 0xFFFFFF, &font_color)) {
        return nullptr;
    }

    uint32_t duration_ms = 3000;
    PyObject *dur = PyDict_GetItemString(cfg, "duration_ms");  // borrowed
    if (dur && dur != Py_None) {
        unsigned long d = PyLong_AsUnsignedLong(dur);
        if (d == (unsigned long)-1 && PyErr_Occurred()) {
            return nullptr;  // TypeError/OverflowError already set
        }
        duration_ms = (uint32_t)d;
    }

    try {
        require_lvgl_runtime();

        toast_overlay_spec_t spec;
        spec.label_text    = label_text.c_str();
        spec.icon_glyph    = have_icon ? icon_glyph.c_str() : nullptr;
        spec.outline_color = outline_color;
        spec.font_color    = font_color;
        spec.duration_ms   = duration_ms;
        overlay_manager_show_toast(&spec);
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return nullptr;
    }

    Py_RETURN_NONE;
}

// --- dismiss_toast() -> None ------------------------------------------------
// Immediately dismiss the current toast (no-op if none is showing). toast_overlay_dismiss()
// deletes the banner widget + its timer, so it is LVGL-thread only: call it from the pump
// thread. For a cross-thread dismiss, prefer letting duration_ms expire (or replace the
// toast with a new one) rather than reaching in from a producer thread.
PyObject *py_dismiss_toast(PyObject *self, PyObject *args) {
    (void)self;
    (void)args;
    try {
        require_lvgl_runtime();
        // Deletes the banner widget + its timer — serialize vs the pump thread's render.
        LvglLockGuard _lvgl_guard;
        toast_overlay_dismiss();
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return nullptr;
    }
    Py_RETURN_NONE;
}
