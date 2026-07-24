// LVGL runtime lifecycle: init/shutdown, tick + pump loop, flush dispatch
// (native ST7789 vs Python callback), resolution switching, and the
// save/restore/clear screen helpers. The pump is host-driven: LVGL only
// advances while Python calls lvgl_pump().
#include "module_internal.h"

#include "gui_constants.h"    // set_display / active_profile
#include "input_profile.h"
#include "overlay_manager.h"  // native screensaver idle-watch dispatcher

#ifdef SS_CAMERA_ENGINE
#include "camera_config.h"    // sticky rotation shared by both camera engines
#include "camera_engine.h"    // camera_engine_pump_consume (Phase-1 native capture)
#include "camera_entropy_engine.h"  // camera_entropy_engine_pump_consume (image-entropy)
#endif

#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>

// RASPI-5 LVGL global lock (see module_internal.h). Recursive so a locked mutator
// may re-enter another locked path on the same thread. The background pump thread
// takes ONLY this lock (never the GIL); host bindings hold the GIL then take it.
static std::recursive_mutex s_lvgl_mtx;
void lvgl_lock() { s_lvgl_mtx.lock(); }
void lvgl_unlock() { s_lvgl_mtx.unlock(); }

static bool s_lvgl_inited = false;

// RASPI-5 Phase 2: the background pump thread that unconditionally owns the panel
// (the ESP taskLVGL model) and its run flag. Spawned by lvgl_init, joined by
// lvgl_shutdown. s_pump_running gates the thread loop, flips lvgl_pump() to a no-op
// while the thread owns the panel (host + thread must not double-pump), and bars
// flush_cb's Python path (a GIL acquire under the LVGL lock would deadlock — §3.1).
static std::thread s_pump_thread;
static std::atomic<bool> s_pump_running{false};
static constexpr unsigned int PUMP_THREAD_SLEEP_MS = 5;  // fixed first-cut cadence (design §5)
static std::vector<uint8_t> s_buf1;  // RGB565: 2 bytes/pixel, sized at runtime
static lv_display_t *s_disp = NULL;
static lv_indev_t *s_input_indev = NULL;
static uint64_t s_last_tick_ms = 0;
static uint32_t s_hor_res = 240;
static uint32_t s_ver_res = 240;
static PyObject *s_flush_cb_py = NULL;

static uint64_t now_ms() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

static void lvgl_tick_update() {
    if (!s_lvgl_inited) {
        return;
    }

    uint64_t now = now_ms();
    if (s_last_tick_ms == 0) {
        s_last_tick_ms = now;
        return;
    }

    uint64_t delta = now - s_last_tick_ms;
    if (delta == 0) {
        return;
    }

    if (delta > 100) {
        delta = 100;
    }

    lv_tick_inc(static_cast<uint32_t>(delta));
    s_last_tick_ms = now;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;
    size_t nbytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 2;  // RGB565: 2 bytes/pixel

    if (native_flush_active()) {
        native_flush_blit(area, px_map, nbytes);
    } else if (!s_pump_running.load(std::memory_order_acquire) && s_flush_cb_py != NULL) {
        // Python flush path — legal ONLY while the host drives the pump (no background
        // thread owns the panel). The pump thread runs GIL-free holding the LVGL lock;
        // entering Python here would acquire the GIL under the LVGL lock — the AB-BA
        // deadlock §3.1 forbids. Once the pump thread owns the panel the flush MUST be
        // native (native_display_init sets it); the Python path is then refused, so a
        // stale python-mode setting renders nothing rather than deadlocking the pump.
        PyGILState_STATE gil = PyGILState_Ensure();

        PyObject *payload = PyBytes_FromStringAndSize(reinterpret_cast<const char *>(px_map), static_cast<Py_ssize_t>(nbytes));
        if (payload != NULL) {
            PyObject *ret = PyObject_CallFunction(s_flush_cb_py, "iiiiO", area->x1, area->y1, area->x2, area->y2, payload);
            if (ret == NULL) {
                // Preserve KeyboardInterrupt so the pump loop can exit.
                // Only print/clear non-interrupt exceptions.
                if (!PyErr_ExceptionMatches(PyExc_KeyboardInterrupt)) {
                    PyErr_Print();
                }
            } else {
                Py_DECREF(ret);
            }
            Py_DECREF(payload);
        } else {
            if (!PyErr_ExceptionMatches(PyExc_KeyboardInterrupt)) {
                PyErr_Print();
            }
        }

        PyGILState_Release(gil);
    }

    lv_display_flush_ready(disp);
}

// One pump iteration: advance the tick, then (under the LVGL lock) publish the newest
// camera frame and run the LVGL timer/render/flush + keypad indev read. Shared by the
// Phase-2 background pump thread and the host-driven fallback loop. The tick update is a
// single-writer lv_tick_inc left OUTSIDE the lock; the lock wraps the LVGL core work.
// Runs NO Python and NO signal check under the lock (§3.1/§3.5) — the native flush path
// is GIL-free, so the pump thread never touches the GIL while holding the LVGL lock.
static void pump_one_iteration() {
    lvgl_tick_update();
    LvglLockGuard _lvgl_guard;
#ifdef SS_CAMERA_ENGINE
    // Publish the newest engine-converted camera frame into the preview sink BEFORE
    // rendering, so this iteration paints it. No-op when no capture session is active.
    // This is the ONLY place an engine frame reaches LVGL. Scan and entropy engines are
    // mutually exclusive, so at most one publishes per iteration.
    camera_engine_pump_consume();
    camera_entropy_engine_pump_consume();
#endif
    lv_timer_handler();
}

// Background pump thread (RASPI-5 Phase 2, the ESP taskLVGL model). It owns the panel and
// drives lv_timer_handler continuously, entirely GIL-free: NO Python, NO PyErr/signal
// check (§3.5 — KeyboardInterrupt is delivered on the app's main thread between
// poll_for_result() calls, never here). The flush is the native ST7789 path (§3.1);
// flush_cb refuses the Python flush path while this thread owns the panel.
static void pump_thread_main() {
    while (s_pump_running.load(std::memory_order_acquire)) {
        pump_one_iteration();
        std::this_thread::sleep_for(std::chrono::milliseconds(PUMP_THREAD_SLEEP_MS));
    }
}

static void ensure_lvgl_runtime() {
    if (s_lvgl_inited) {
        return;
    }

    // lv_init() MUST precede set_display(): the i18n baked floor rasterizes its
    // five translated-text role fonts (OpenSans Western via tiny_ttf) inside
    // set_display(), but only once lv_is_initialized() is true. With the reverse
    // order those role fonts stay null, and the first Fallback-pack load (e.g.
    // ru, which chains under the baseline) dereferences null -> segfault. (The
    // old pre-i18n floor was static bitmap fonts, non-null at init, so order
    // didn't matter then.)
    lv_init();
    set_display(s_hor_res, s_ver_res);

    s_buf1.assign(static_cast<size_t>(s_hor_res) * s_ver_res * 2, 0);

    s_disp = lv_display_create(s_hor_res, s_ver_res);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, s_buf1.data(), NULL, s_buf1.size(),
                           LV_DISPLAY_RENDER_MODE_FULL);

    s_input_indev = lv_indev_create();
    lv_indev_set_type(s_input_indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(s_input_indev, native_input_read_cb);

    input_profile_set_mode(INPUT_MODE_HARDWARE);

    // Start the native overlay manager (screensaver idle-watch) now that the
    // display + input devices exist — its contract requires both. The dispatcher
    // runs inside lv_timer_handler() (pumped by lvgl_pump). The idle timeout is
    // configured later by the Python runtime via set_screensaver_timeout().
    overlay_manager_init();

    s_last_tick_ms = now_ms();
    s_lvgl_inited = true;

    // RASPI-5 Phase 2 flip: spawn the background pump thread that unconditionally owns the
    // panel (mirrors the ESP taskLVGL, created at boot). From here LVGL advances without
    // Python and the host's lvgl_pump() is a no-op (see lvgl_runtime_pump). Force native
    // flush as the mode (§4) — the required panel path (§3.1) — so the pump never takes the
    // Python flush path; flush_cb additionally bars it whenever the thread owns the panel.
    // If the thread fails to spawn, s_pump_running stays false and the host-driven fallback
    // pump takes over (Phase-1 behaviour).
    native_flush_select_native();
    s_pump_running.store(true, std::memory_order_release);
    try {
        s_pump_thread = std::thread(pump_thread_main);
    } catch (...) {
        s_pump_running.store(false, std::memory_order_release);
    }
}

// Stop + join the background pump thread. Idempotent. extern "C" + exposed so module init
// can register it with Py_AtExit: if the app exits WITHOUT calling lvgl_shutdown(), the
// still-joinable std::thread would std::terminate at static destruction (§4). A GIL-free
// join of a Python-free thread, so it is safe to run during interpreter finalization.
extern "C" void lvgl_runtime_join_pump_thread(void) {
    if (s_pump_running.exchange(false, std::memory_order_acq_rel)) {
        if (s_pump_thread.joinable()) {
            s_pump_thread.join();
        }
    }
}

static void lvgl_runtime_shutdown() {
    if (!s_lvgl_inited) {
        return;
    }

    // Stop + join the background pump thread BEFORE any LVGL teardown (§4): a thread still
    // inside lv_timer_handler must never touch a half-freed display. After the join the
    // pump can no longer run, so the teardown below is single-threaded again.
    lvgl_runtime_join_pump_thread();

#if LV_USE_LOG
    LV_LOG_USER("lvgl runtime shutdown");
#endif
    // Tear down any live camera-preview session while its widgets + overlay handle
    // are still valid — after lv_deinit() the statics would dangle into a re-init.
    camera_preview_on_lvgl_shutdown();
#if defined(LVGL_VERSION_MAJOR) && (LVGL_VERSION_MAJOR >= 8)
    lv_deinit();
#endif
    s_disp = NULL;
    s_buf1.clear();
    s_last_tick_ms = 0;
    s_lvgl_inited = false;
}

// --- Cross-unit API (see module_internal.h) --------------------------------

void require_lvgl_runtime() {
    if (!s_lvgl_inited) {
        throw std::runtime_error("LVGL runtime not initialized: call lvgl_init(hor_res=..., ver_res=...) first");
    }
}

bool lvgl_runtime_is_inited() {
    return s_lvgl_inited;
}

int lvgl_runtime_pump(unsigned int duration_ms, unsigned int sleep_ms) {
    if (!s_lvgl_inited) {
        return 0;
    }

    // RASPI-5 Phase 2: the background pump thread owns the panel and drives
    // lv_timer_handler continuously. Host-driven pumping would run lv_timer_handler on
    // two threads at once, so while the thread runs this is a no-op — the app's poll loop
    // still calls lvgl_pump(); it simply returns immediately. The loop below is the
    // single-threaded FALLBACK, reached only if the pump thread failed to spawn: the host
    // then drives the pump as in Phase 1, signal check included (valid because it runs on
    // the app's main thread, unlike the pump thread — §3.5).
    if (s_pump_running.load(std::memory_order_acquire)) {
        return 0;
    }

    auto start = std::chrono::steady_clock::now();
    while (true) {
        pump_one_iteration();

        // Check for exceptions raised inside flush callbacks (e.g.
        // KeyboardInterrupt) or new pending signals (Ctrl+C).
        if (PyErr_Occurred() || PyErr_CheckSignals() != 0) {
            return -1;
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
        if (elapsed >= static_cast<long long>(duration_ms)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    }
    return 0;
}

void lvgl_clear_to_black(bool clean_sys_layer) {
    {
        // Serialize the widget-tree build against the pump thread; release before
        // pumping (lvgl_runtime_pump takes the lock itself, per iteration). This
        // guard also covers py_clear_screen, whose only LVGL access is via here.
        LvglLockGuard _lvgl_guard;
        if (clean_sys_layer) {
            lv_obj_clean(lv_layer_sys());
        }
        lv_obj_t *scr = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
        lv_scr_load(scr);
    }
    lvgl_runtime_pump(50, 5);
}

// --- Python entry points ----------------------------------------------------

PyObject *py_lvgl_init(PyObject *self, PyObject *args, PyObject *kwargs) {
    (void)self;
    static const char *kwlist[] = {"hor_res", "ver_res", NULL};
    unsigned int hor_res = 240;
    unsigned int ver_res = 240;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|II", const_cast<char **>(kwlist), &hor_res, &ver_res)) {
        return NULL;
    }

    if (s_lvgl_inited) {
        Py_RETURN_NONE;
    }

    if (hor_res == 0 || ver_res == 0) {
        PyErr_SetString(PyExc_ValueError, "hor_res and ver_res must be > 0");
        return NULL;
    }

    s_hor_res = hor_res;
    s_ver_res = ver_res;
    ensure_lvgl_runtime();
    Py_RETURN_NONE;
}

PyObject *py_lvgl_shutdown(PyObject *self, PyObject *args) {
    (void)self;
    (void)args;
    lvgl_runtime_shutdown();
    Py_RETURN_NONE;
}

PyObject *py_set_resolution(PyObject *self, PyObject *args, PyObject *kwargs) {
    (void)self;
    static const char *kwlist[] = {"width", "height", NULL};
    unsigned int width = 0;
    unsigned int height = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "II", const_cast<char **>(kwlist), &width, &height)) {
        return NULL;
    }

    if (!s_lvgl_inited) {
        PyErr_SetString(PyExc_RuntimeError, "LVGL runtime not initialized: call lvgl_init() first");
        return NULL;
    }

    if (width == s_hor_res && height == s_ver_res) {
        Py_RETURN_NONE;  // already at requested resolution
    }

    // Serialize the whole display teardown+recreate against the pump thread — this
    // deletes and rebuilds the display and every screen (the sharpest call to lock).
    LvglLockGuard _lvgl_guard;

    // Update the active display profile (aborts if no profile matches).
    set_display(width, height);

    s_hor_res = width;
    s_ver_res = height;

    // Delete the current LVGL display (also deletes all screens).
    if (s_disp) {
        lv_display_delete(s_disp);
        s_disp = NULL;
    }

    // Resize the draw buffer for the new resolution.
    s_buf1.assign(static_cast<size_t>(width) * height * 2, 0);

    // Create a new LVGL display at the new resolution.
    s_disp = lv_display_create(width, height);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, s_buf1.data(), NULL, s_buf1.size(),
                           LV_DISPLAY_RENDER_MODE_FULL);

    // lv_display_delete sets all associated indevs' display to NULL, which
    // silently disables them.  Reassign every indev to the new display.
    lv_indev_t *indev = NULL;
    while ((indev = lv_indev_get_next(indev)) != NULL) {
        lv_indev_set_display(indev, s_disp);
    }

    // If native display is active, send the new MADCTL for the target rotation.
    display_update_resolution(width, height);

    Py_RETURN_NONE;
}

// display_size() -> (width, height) of the active display profile.
//
// Gate on the runtime: active_profile() aborts() the process if no profile is set
// (a profile is only installed by lvgl_init()/set_resolution()), so raise a
// catchable RuntimeError for a premature call — same guard as locale_packs.cpp.
// Lets the app read vertical_resolution the same way on Pi and ESP32 (it just
// calls display_size() regardless of target).
PyObject *py_display_size(PyObject *self, PyObject *args) {
    (void)self;
    (void)args;
    try {
        require_lvgl_runtime();
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }
    LvglLockGuard _lvgl_guard;
    const DisplayProfile &p = active_profile();
    return Py_BuildValue("(ii)", p.width, p.height);
}

PyObject *py_lvgl_pump(PyObject *self, PyObject *args, PyObject *kwargs) {
    (void)self;
    static const char *kwlist[] = {"duration_ms", "sleep_ms", NULL};
    unsigned int duration_ms = 10;
    unsigned int sleep_ms = 1;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|II", const_cast<char **>(kwlist), &duration_ms, &sleep_ms)) {
        return NULL;
    }

    require_lvgl_runtime();
    if (lvgl_runtime_pump(duration_ms, sleep_ms) < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}

PyObject *py_set_flush_callback(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *cb = Py_None;
    if (!PyArg_ParseTuple(args, "|O", &cb)) {
        return NULL;
    }

    if (cb != Py_None && !PyCallable_Check(cb)) {
        PyErr_SetString(PyExc_TypeError, "flush callback must be callable or None");
        return NULL;
    }

    Py_XINCREF(cb == Py_None ? NULL : cb);
    Py_XDECREF(s_flush_cb_py);
    s_flush_cb_py = (cb == Py_None) ? NULL : cb;
    Py_RETURN_NONE;
}

// --- Screen save / restore ------------------------------------------------
// General-purpose mechanism for preserving the active LVGL screen across
// an overlay (e.g. screensaver, modal dialog). The Python side decides
// when to save and restore; the C side just holds the pointer.

static lv_obj_t  *s_saved_screen = NULL;
static lv_group_t *s_saved_group = NULL;

PyObject *py_save_screen(PyObject *self, PyObject *args) {
    (void)self; (void)args;
    try {
        require_lvgl_runtime();
        LvglLockGuard _lvgl_guard;
        s_saved_screen = lv_scr_act();

        // Save the current indev group.
        s_saved_group = NULL;
        lv_indev_t *indev = NULL;
        while ((indev = lv_indev_get_next(indev)) != NULL) {
            if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD ||
                lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
                s_saved_group = lv_indev_get_group(indev);
                break;
            }
        }
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }
    Py_RETURN_NONE;
}

PyObject *py_restore_screen(PyObject *self, PyObject *args) {
    (void)self; (void)args;
    try {
        require_lvgl_runtime();
        LvglLockGuard _lvgl_guard;
        if (!s_saved_screen) {
            Py_RETURN_NONE;  // nothing saved — no-op
        }

        lv_obj_t *cur_scr = lv_scr_act();

        // Restore the saved indev group BEFORE deleting the overlay screen,
        // since deletion frees the overlay's group.
        if (s_saved_group) {
            lv_indev_t *indev = NULL;
            while ((indev = lv_indev_get_next(indev)) != NULL) {
                if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD ||
                    lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
                    lv_indev_set_group(indev, s_saved_group);
                }
            }
        }

        if (cur_scr != s_saved_screen) {
            lv_scr_load(s_saved_screen);
            // Synchronously delete the overlay screen now that the saved
            // screen is active and the indev group is restored.
            lv_obj_delete(cur_scr);
        }

        s_saved_screen = NULL;
        s_saved_group = NULL;
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }
    Py_RETURN_NONE;
}

PyObject *py_clear_screen(PyObject *self, PyObject *args) {
    (void)self;
    (void)args;
    try {
        require_lvgl_runtime();
        lvgl_clear_to_black(false);
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }
    Py_RETURN_NONE;
}

// set_screensaver_timeout(ms) -> None
//
// Idle time (ms) before the native overlay manager activates the screensaver;
// 0 disables it. The Python runtime calls this once at init with the configured
// screensaver_activation_ms. Per-screen opt-out is declarative (the cfg's
// allow_screensaver bool), so there is no suspend/resume export.
PyObject *py_set_screensaver_timeout(PyObject *self, PyObject *args) {
    (void)self;
    unsigned int ms = 0;
    if (!PyArg_ParseTuple(args, "I", &ms)) {
        return NULL;
    }
    // A 0 timeout dismisses an active screensaver (loads/deletes screens), so this can
    // touch LVGL — serialize against the pump thread.
    LvglLockGuard _lvgl_guard;
    overlay_manager_set_screensaver_timeout(static_cast<uint32_t>(ms));
    Py_RETURN_NONE;
}

// get_inactive_time_ms() -> int
//
// Milliseconds since the last input activity on the LVGL display. Any keypad press
// updates the display's activity clock (lv_indev sets last_activity_time = lv_tick_get()
// on a PRESSED read; lv_display_get_inactive_time returns lv_tick_elaps of it), so this
// is the native stand-in for the app's HardwareButtons.has_any_input() poll — a small
// value means the user just interacted. The toast pre-show activation delay
// (gui/toast.py) polls it to cancel while the user is still pressing keys, which retires
// the last non-screen HardwareButtons consumer (APP-7).
//
// The activity clock only advances while LVGL is pumped — the keypad indev is read
// inside lv_timer_handler() (lvgl_pump). A caller polling this must have a pump running
// for presses to register; with none the value only ever grows. Reads the same single
// display the runtime owns (s_disp); lv_display_get_inactive_time tolerates a NULL disp
// (returns the min over all displays). Requires the runtime — raises RuntimeError
// before lvgl_init().
PyObject *py_get_inactive_time_ms(PyObject *self, PyObject *args) {
    (void)self;
    (void)args;
    try {
        require_lvgl_runtime();
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }
    LvglLockGuard _lvgl_guard;
    uint32_t ms = lv_display_get_inactive_time(s_disp);
    return PyLong_FromUnsignedLong(static_cast<unsigned long>(ms));
}

// set_camera_rotation(degrees) -> None
//
// Sticky device setting applied to BOTH camera flows (scan and entropy). Takes the
// app's raw SETTING__CAMERA_ROTATION value (0/90/180/270) UNMODIFIED — this layer
// composes it with the sensor's mount base (camera_config.h), so the app must not
// pre-add that base.
//
// Modelling rotation as a sticky setter rather than a start() argument is what keeps
// camera_scanner.start() / camera_entropy.start() argument-identical to the ESP
// bindings, so one Python call shape drives both platforms. Each engine samples the
// value at start(), so a change takes effect on the next camera session, not mid-
// stream. The runtime calls this at init and again whenever the setting changes.
//
// Pi-only: the MicroPython build deliberately does not implement it (the app reports
// the setting as unsupported on that hardware), so there is no ESP counterpart.
PyObject *py_set_camera_rotation(PyObject *self, PyObject *args) {
    (void)self;
    int degrees = 0;
    if (!PyArg_ParseTuple(args, "i", &degrees)) {
        return NULL;
    }
    // The frame-convert path implements quarter turns only; anything else would
    // silently fall through its switch and render unrotated.
    if (degrees % 90 != 0) {
        PyErr_Format(PyExc_ValueError,
                     "camera rotation must be a multiple of 90 (got %d)", degrees);
        return NULL;
    }
#ifdef SS_CAMERA_ENGINE
    camera_config_set_rotation(degrees);
#else
    // No-camera diagnostic build: keep the API surface stable for the app, but there
    // is no engine to configure.
    (void)degrees;
#endif
    Py_RETURN_NONE;
}
