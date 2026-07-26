// camera_preview.cpp — Pi Zero live camera-preview scan surface.
//
// The portable overlay (components/seedsigner/camera_preview_overlay.{h,cpp}) is
// PASSIVE CHROME ONLY: status bar, progress fill, instruction text, back-affordance,
// status dot. It never touches camera pixels — it draws OVER a pixel plane the
// platform owns. On ESP32 that plane is an lv_image fed by the camera pipeline
// (board_pipeline_display_lvgl.c) with the overlay created above it
// (camera_scanner.cpp). This file is the Pi equivalent: it owns the pixel plane (a
// full-screen RGB565 lv_image the host pushes frames into) and creates the same
// portable overlay on top. The screens submodule needs no changes — the overlay is
// called as a black box.
//
// Execution model: the native camera engine (camera_engine.cpp) captures + converts
// frames on worker threads and publishes them into this sink from the background pump
// thread's consume hook (RASPI-5 Phase 2 — the ESP taskLVGL model). LVGL core is not
// thread-safe, so every host binding here that touches the sink or the overlay holds the
// LVGL lock (LvglLockGuard) to serialize against that pump thread. The overlay-state
// calls (set_progress/report/...) run a few times/sec, never per frame. The legacy Python
// frame-push (camera_preview_set_frame) takes the same lock and retires with the PIL sunset.
//
// FRAME FORMAT CONTRACT: set_frame() takes LVGL-NATIVE RGB565 (w*h*2 bytes), NEVER
// pre-swapped for the panel. Once a frame is lv_image content the active flush driver
// (python flush -> ST7789.py, or native flush -> display_st7789.cpp) applies its
// panel byte-order/BGR handling uniformly to camera pixels AND overlay widgets.
// Pre-swapping would be correct under one flush driver and double-swapped under the
// other; feeding LVGL-native keeps this binding flush-mode-agnostic (cutover-safe).
#include "module_internal.h"

#include "screen_scaffold.h"          // load_screen_and_cleanup_previous
#include "overlay_manager.h"          // SS_OBJ_FLAG_NO_SCREENSAVER (per-screen saver opt-out)
#include "camera_preview_overlay.h"   // camera_preview_overlay_* (portable, called as a black box)
#include "camera_preview_sink.h"      // the Python-free sink bridge the native engine calls
#include "camera_entropy_overlay.h"   // camera_entropy_overlay_* (portable image-entropy chrome)
#include "image_entropy.h"            // image_entropy_process (portable contrast stretch + aspect-fit)
#include "seedsigner.h"               // io_test_get_camera_plane_dims / io_test_blit_camera (io_test grab redirect)
#ifdef SS_CAMERA_ENGINE
#include "camera_engine.h"            // camera_engine_start/stop (io_test single-frame grab)
#include "camera_error.h"             // camera_error_str (OSError text on grab failure)
#endif

#include <cstring>                    // memcpy
#include <stdexcept>
#include <string>
#include <vector>

// --- Live session state -----------------------------------------------------
// One camera, one session. The screen is reaped by the app's next
// load_screen_and_cleanup_previous(); the overlay HANDLE is a separate lv_malloc'd
// struct that widget teardown does NOT free, so close() must always destroy it.
static lv_obj_t                 *s_cam_screen = nullptr;
static lv_obj_t                 *s_cam_img    = nullptr;
static lv_image_dsc_t            s_cam_dsc;
static std::vector<uint8_t>      s_cam_buf;     // dsc.data backing; sized once per session
static uint8_t                  *s_cam_data = nullptr;  // == s_cam_buf.data() while a session is live
static size_t                    s_cam_size = 0;        // == w*h*2
static camera_preview_overlay_t *s_overlay  = nullptr;
// The image-entropy flow reuses the SAME sink (s_cam_*) — scan and entropy are mutually
// exclusive, so only one overlay handle is ever live. Its own handle struct, freed on close.
static camera_entropy_overlay_t *s_entropy_overlay = nullptr;
// Static-QR progress-bar suppression. A single-frame QR completes on its first successful
// read, so driving the bar to 100% only flashes it as the screen tears down. Track whether
// any *partial* (0 < pct < 100) progress was ever reported this session; if not, the 100%
// completion is suppressed so the bar never appears for a static decode. Reset per session
// in camera_preview_build_session(). Mirrors the ESP, which never raises the bar on a static
// scan. Only a multi-part decode ever reports a percent strictly between 0 and 100.
static bool s_scan_saw_partial = false;

// Drop the overlay handle + backing buffer. Safe after the screen was reaped
// externally: overlay_destroy() only touches the anim subsystem + frees the handle
// struct (never the already-freed widgets). Does NOT delete the screen object.
static void camera_preview_teardown() {
    if (s_overlay) {
        camera_preview_overlay_destroy(s_overlay);
        s_overlay = nullptr;
    }
    if (s_entropy_overlay) {
        camera_entropy_overlay_destroy(s_entropy_overlay);
        s_entropy_overlay = nullptr;
    }
    s_cam_screen = nullptr;
    s_cam_img    = nullptr;
    s_cam_data   = nullptr;
    s_cam_size   = 0;
    std::vector<uint8_t>().swap(s_cam_buf);  // release capacity
}

// Called from lvgl_runtime_shutdown() BEFORE lv_deinit(), while the widgets + the
// overlay handle are still valid: destroys the handle and nulls every static so a
// subsequent lvgl_init() + set_frame/build can't dereference a freed lv_obj (the
// build-time tests init/shutdown the runtime repeatedly across modules).
void camera_preview_on_lvgl_shutdown() {
    LvglLockGuard _lvgl_guard;
    camera_preview_teardown();
}

// End the live session (shared by py_camera_preview_close and camera_scanner.stop()):
// drop the overlay handle + sink buffer and reset LVGL's idle clock so the successor
// screen gets a full screensaver window. Idempotent.
void camera_preview_close_session() {
    LvglLockGuard _lvgl_guard;
    camera_preview_teardown();
    if (lvgl_runtime_is_inited()) {
        lv_display_trigger_activity(NULL);
    }
}

// --- Builder ----------------------------------------------------------------
// Build the live preview screen + portable overlay (the body shared by the Python
// binding and camera_scanner.start(), which owns the screen on the Pi like the ESP
// camera_scanner does). Throws std::runtime_error on failure; the callers translate
// that to a Python exception. `instructions` is the optional bottom-line text.
void camera_preview_build_session(const std::string &instructions) {
    {
        require_lvgl_runtime();
        LvglLockGuard _lvgl_guard;

        // A rebuild without close() first must not leak the prior handle.
        if (s_overlay) {
            camera_preview_overlay_destroy(s_overlay);
            s_overlay = nullptr;
        }

        // Fresh session: no partial progress seen yet, so a static decode stays suppressed.
        s_scan_saw_partial = false;

        const int32_t w = lv_display_get_horizontal_resolution(NULL);
        const int32_t h = lv_display_get_vertical_resolution(NULL);
        // The live preview is a CENTERED SQUARE of the short display dimension. On a square
        // panel (Pi Zero 240x240) the square == the whole screen and sq_x/sq_y are 0 —
        // byte-identical to before. On a wide panel (SeedSigner+ 320x240) the square is
        // centered with static black L/R pillars that the portable overlay paints via its
        // gutter-blanking (keyed off square_x/w). Keeping the sink SQUARE also keeps the
        // engine's convert square (no non-square transpose), so a scan costs the same
        // per-frame on both panels — the square-crop is the SeedSigner+ perf lever (RASPI-7).
        const int32_t side = w < h ? w : h;
        const int32_t sq_x = (w - side) / 2;
        const int32_t sq_y = (h - side) / 2;
        const size_t  buf_size = static_cast<size_t>(side) * static_cast<size_t>(side) * 2;

        // Bare black screen (chrome-free: no top-nav scaffold). Its black background is what
        // shows through the pillars on a wide panel (the overlay's gutter fills sit on top).
        lv_obj_t *scr = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
        lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
        lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

        // Pixel sink: one centered-square RGB565 lv_image the host memcpy's frames into.
        // The dsc.data pointer stays STABLE for the whole session (buffer sized once,
        // never resized on set_frame) so LVGL's cached decode aliases our buffer and
        // a memcpy + invalidate updates what's drawn — the ESP image-widget contract.
        s_cam_buf.assign(buf_size, 0);
        s_cam_data = s_cam_buf.data();
        s_cam_size = buf_size;

        lv_memzero(&s_cam_dsc, sizeof(s_cam_dsc));
        s_cam_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
        s_cam_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
        s_cam_dsc.header.w      = side;
        s_cam_dsc.header.h      = side;
        s_cam_dsc.header.stride = static_cast<uint32_t>(side) * 2;
        s_cam_dsc.data_size     = static_cast<uint32_t>(buf_size);
        s_cam_dsc.data          = s_cam_data;

        s_cam_img = lv_image_create(scr);
        lv_obj_set_size(s_cam_img, side, side);
        lv_obj_set_pos(s_cam_img, sq_x, sq_y);
        lv_image_set_src(s_cam_img, &s_cam_dsc);
        lv_obj_remove_flag(s_cam_img,
                           (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

        // Overlay chrome ON TOP (created after the image so it draws above it). It reads the
        // square rect below to paint the gutter pillars (wide panel only) and center all
        // chrome over the square. Start on the back-affordance state (instruction text
        // shown); the first set_progress() raises the bar (mirrors Python ScanScreen:
        // instructions first, progress bar once decoding).
        camera_preview_overlay_spec_t spec;
        lv_memzero(&spec, sizeof(spec));
        spec.instructions_text = instructions.empty() ? nullptr : instructions.c_str();
        spec.square_x = sq_x;
        spec.square_y = sq_y;
        spec.square_w = side;
        spec.square_h = side;
        spec.scanning_active  = false;
        spec.progress_percent = 0;
        spec.frame_status     = CAMERA_OVERLAY_FRAME_NONE;
        s_overlay = camera_preview_overlay_create(scr, &spec);
        if (!s_overlay) {
            lv_obj_delete(scr);
            camera_preview_teardown();
            throw std::runtime_error("camera_preview overlay create failed");
        }

        s_cam_screen = scr;

        // A scan runs with NO user input while the user lines up the QR. Opt this
        // screen out of the idle screensaver (the overlay-manager dispatcher reads
        // this flag off the active screen and skips activation) so the saver never
        // covers the live preview mid-scan. The flag rides on the screen object, so
        // it auto-clears on the next screen swap. camera_preview_close() then resets
        // LVGL's idle clock so the successor screen still gets a full saver window.
        lv_obj_add_flag(scr, SS_OBJ_FLAG_NO_SCREENSAVER);

        load_screen_and_cleanup_previous(scr);
        mark_last_path_compiled();
    }
}

// --- Image-entropy session --------------------------------------------------
// Parallel to camera_preview_build_session but builds the camera_entropy_overlay
// (PREVIEW/CAPTURING/CONFIRM) instead of the scan overlay. Reuses the SAME RGB565 sink
// statics (s_cam_*) — scan and entropy never run at once. The screen + sink block is
// intentionally kept a copy of the scan builder's so the device-validated scan path is
// untouched. Strings are host-provided + already localized (from camera_entropy.set_labels).
void camera_entropy_build_session(const std::string &preview_instructions,
                                  const std::string &confirm_instructions,
                                  const std::string &capturing_text,
                                  const std::string &accept_label) {
    require_lvgl_runtime();
    LvglLockGuard _lvgl_guard;

    // A rebuild without close() first must not leak a prior handle (either overlay).
    if (s_overlay) {
        camera_preview_overlay_destroy(s_overlay);
        s_overlay = nullptr;
    }
    if (s_entropy_overlay) {
        camera_entropy_overlay_destroy(s_entropy_overlay);
        s_entropy_overlay = nullptr;
    }

    const int32_t w = lv_display_get_horizontal_resolution(NULL);
    const int32_t h = lv_display_get_vertical_resolution(NULL);
    // Centered short-dim square sink; on a wide panel the flanking columns are static black
    // pillars the overlay paints. Mirrors camera_preview_build_session — see the rationale
    // there (square sink keeps the engine convert square, no non-square transpose). The
    // CONFIRM review image is separately display-sized (fills the whole panel by design).
    const int32_t side = w < h ? w : h;
    const int32_t sq_x = (w - side) / 2;
    const int32_t sq_y = (h - side) / 2;
    const size_t  buf_size = static_cast<size_t>(side) * static_cast<size_t>(side) * 2;

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_cam_buf.assign(buf_size, 0);
    s_cam_data = s_cam_buf.data();
    s_cam_size = buf_size;

    lv_memzero(&s_cam_dsc, sizeof(s_cam_dsc));
    s_cam_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_cam_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    s_cam_dsc.header.w      = side;
    s_cam_dsc.header.h      = side;
    s_cam_dsc.header.stride = static_cast<uint32_t>(side) * 2;
    s_cam_dsc.data_size     = static_cast<uint32_t>(buf_size);
    s_cam_dsc.data          = s_cam_data;

    s_cam_img = lv_image_create(scr);
    lv_obj_set_size(s_cam_img, side, side);
    lv_obj_set_pos(s_cam_img, sq_x, sq_y);
    lv_image_set_src(s_cam_img, &s_cam_dsc);
    lv_obj_remove_flag(s_cam_img,
                       (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    // Entropy overlay ON TOP (created after the image so it draws above it). It reads the
    // square rect below to paint the gutter pillars (wide panel) and center chrome over the
    // square. PREVIEW phase to start.
    camera_entropy_overlay_spec_t spec;
    lv_memzero(&spec, sizeof(spec));
    spec.square_x = sq_x;
    spec.square_y = sq_y;
    spec.square_w = side;
    spec.square_h = side;
    spec.preview_instructions = preview_instructions.empty() ? nullptr : preview_instructions.c_str();
    spec.confirm_instructions = confirm_instructions.empty() ? nullptr : confirm_instructions.c_str();
    spec.capturing_text       = capturing_text.empty() ? nullptr : capturing_text.c_str();
    spec.capture_style        = CAMERA_ENTROPY_CAPTURE_RING;  // TOUCH-mode only; ignored in HARDWARE mode
    spec.capture_icon         = nullptr;
    spec.capture_label        = nullptr;
    spec.accept_label         = accept_label.empty() ? nullptr : accept_label.c_str();
    spec.phase                = CAMERA_ENTROPY_PHASE_PREVIEW;
    s_entropy_overlay = camera_entropy_overlay_create(scr, &spec);
    if (!s_entropy_overlay) {
        lv_obj_delete(scr);
        camera_preview_teardown();
        throw std::runtime_error("camera_entropy overlay create failed");
    }

    s_cam_screen = scr;
    lv_obj_add_flag(scr, SS_OBJ_FLAG_NO_SCREENSAVER);
    load_screen_and_cleanup_previous(scr);
    mark_last_path_compiled();
}

// Flip the entropy overlay's phase (camera_entropy.capture()→CAPTURING, get_result()'s
// first latch→CONFIRM, resume()→PREVIEW). No-op when no entropy overlay is active.
void camera_entropy_set_phase(int phase) {
    LvglLockGuard _lvgl_guard;
    if (s_entropy_overlay) {
        camera_entropy_overlay_set_phase(s_entropy_overlay, (camera_entropy_phase_t)phase);
    }
}

// Build the CONFIRM review image from the latched RAW frame: aspect-fit with a capped
// letterbox + color-preserving luminance contrast stretch (portable image_entropy_process)
// into a display-sized RGB565 buffer, then hand it to the overlay (which deep-copies it).
// DISPLAY-ONLY — never fed back into the entropy chain.
//
// `raw_rgb565` is the engine's wide, high-resolution still (src_w x src_h in DISPLAY
// orientation) — NOT a sink-sized frame, and not necessarily square. Callers must pass the
// latched frame's own dimensions (entropy_coord_get_result reports them); image_entropy_process
// box-filters the downscale, which is what makes the extra capture resolution worth having.
void camera_entropy_build_confirm_image(const uint8_t *raw_rgb565, int src_w, int src_h) {
    if (!s_entropy_overlay || !raw_rgb565 || src_w <= 0 || src_h <= 0) {
        return;
    }
    if (!lvgl_runtime_is_inited()) {
        return;
    }
    LvglLockGuard _lvgl_guard;
    const int32_t dw = lv_display_get_horizontal_resolution(NULL);
    const int32_t dh = lv_display_get_vertical_resolution(NULL);
    std::vector<uint16_t> disp(static_cast<size_t>(dw) * static_cast<size_t>(dh));
    image_entropy_process(raw_rgb565, src_w, src_h, IMAGE_ENTROPY_PIXFMT_RGB565,
                          disp.data(), dw, dh);
    camera_entropy_overlay_set_confirm_image(s_entropy_overlay, disp.data(), dw, dh);
}

PyObject *py_camera_preview_screen(PyObject *self, PyObject *args) {
    (void)self;

    PyObject *cfg = nullptr;
    if (!PyArg_ParseTuple(args, "|O", &cfg)) {
        return nullptr;
    }
    if (cfg && cfg != Py_None && !PyDict_Check(cfg)) {
        PyErr_SetString(PyExc_RuntimeError, "camera_preview_screen expects cfg_dict as dict");
        return nullptr;
    }

    // Optional hardware/joystick bottom-line text (already localized + composed by the
    // host, e.g. "< back  |  Scan a QR code"). Borrowed ref; copied into std::string so
    // it outlives the overlay build (the overlay copies it into an lv_label).
    std::string instructions;
    if (cfg && cfg != Py_None) {
        PyObject *it = PyDict_GetItemString(cfg, "instructions_text");  // borrowed
        if (it && PyUnicode_Check(it)) {
            const char *s = PyUnicode_AsUTF8(it);
            if (!s) {
                return nullptr;  // encoding error already set
            }
            instructions = s;
        }
    }

    try {
        LvglLockGuard _lvgl_guard;
        camera_preview_build_session(instructions);
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return nullptr;
    }
    Py_RETURN_NONE;
}

// --- Native-engine sink bridge (camera_preview_sink.h) ----------------------
// Python-free entry points the native camera engine calls (camera_engine.cpp). The
// thread model is NOT uniform, so the locking differs per function (RASPI-5 Phase 2):
//   * blit_rgb565 runs pump-thread-ONLY: the engine's consume hook fires inside
//     pump_one_iteration, already under the LVGL lock — so its lv_obj_invalidate stays on
//     the LVGL locus and it needs no lock of its own.
//   * session_active / get_sink_dims are called on the HOST thread (from camera_scanner /
//     camera_entropy start() and from camera_engine_start()/entropy_engine_start()), never
//     from the pump. In the io_test-grab case they read LVGL widget state via
//     io_test_get_camera_plane_dims, which would race the concurrent pump thread — so each
//     takes the LVGL lock itself. (The engine start() callers hold no lock, so this is a
//     fresh, brief acquisition released before the blocking bring-up — never held across it.)
//
// io_test grab redirect: io_test_screen owns its OWN square pixel plane (SCREENS-9),
// not the camera_preview sink. While an io_test single-frame grab is active, this bridge
// reports the io_test plane's dims to the UNCHANGED engine and STASHES each converted
// frame (see blit_rgb565) instead of displaying it — the live stream is suppressed so the
// plane stays dark during the "Capturing…" window. io_test_camera_stop() then reveals ONLY
// the last stashed frame, so io_test shows a single captured still, not video. Scan and
// io_test use different screens (never both live), so the redirect never collides with a sink.
static bool s_io_test_grab = false;
static std::vector<uint8_t> s_io_test_last_frame;  // last converted frame during a grab

bool camera_preview_session_active() {
    // Host-thread entry: serialize the io_test_get_camera_plane_dims LVGL read (and the
    // sink-pointer reads) against the background pump thread. Recursive lock — harmless if
    // a future caller already holds it.
    LvglLockGuard _lvgl_guard;
    if (s_io_test_grab) {
        int w = 0, h = 0;
        io_test_get_camera_plane_dims(&w, &h);
        return w > 0 && h > 0;
    }
    return s_cam_img != nullptr && s_cam_data != nullptr;
}

void camera_preview_get_sink_dims(int *w, int *h) {
    // Host-thread entry: serialize the LVGL read against the background pump thread.
    LvglLockGuard _lvgl_guard;
    if (s_io_test_grab) {
        io_test_get_camera_plane_dims(w, h);
        return;
    }
    if (w) {
        *w = static_cast<int>(s_cam_dsc.header.w);
    }
    if (h) {
        *h = static_cast<int>(s_cam_dsc.header.h);
    }
}

void camera_preview_blit_rgb565(const uint8_t *rgb565, size_t nbytes) {
    if (s_io_test_grab) {
        // Suppress the live stream: stash the latest converted frame. io_test_camera_stop()
        // reveals ONLY this last one, so io_test shows a single captured still (not video).
        s_io_test_last_frame.assign(rgb565, rgb565 + nbytes);
        return;
    }
    if (!s_cam_img || !s_cam_data || nbytes != s_cam_size) {
        return;  // no session / size mismatch — silent no-op
    }
    memcpy(s_cam_data, rgb565, s_cam_size);
    lv_obj_invalidate(s_cam_img);
}

// --- io_test single-frame grab (KEY1 in run_io_test_screen) -----------------
// The app's io_test loop calls io_test_camera_start() on KEY1, pumps its "Capturing…"
// hold (each lvgl_pump runs camera_engine_pump_consume, so frames flow into the io_test
// plane via the redirect above), then io_test_camera_stop() — which freezes the last
// frame in the plane. SS_CAMERA_ENGINE-only (needs the native libcamera engine).
PyObject *py_io_test_camera_start(PyObject *self, PyObject *args) {
    (void)self;
    (void)args;
#ifdef SS_CAMERA_ENGINE
    // Requires an active io_test_screen — its plane is the redirect target. Read the plane
    // dims AND arm the redirect under the LVGL lock, then RELEASE before
    // camera_engine_start(): the engine call spawns worker threads and does (possibly
    // blocking) libcamera bring-up, and must NEVER run while we hold the LVGL lock (it
    // would stall the background pump thread — §follow-up-1). Setting s_io_test_grab under
    // the lock also publishes it (release/acquire via the LVGL lock) so the pump thread's
    // blit_rgb565 reliably observes the armed redirect. camera_engine_start() re-reads the
    // plane dims via the self-locking sink bridge, so that read stays serialized without us
    // holding the lock across the engine call.
    int w = 0, h = 0;
    {
        LvglLockGuard _lvgl_guard;
        io_test_get_camera_plane_dims(&w, &h);
        if (w > 0 && h > 0) {
            s_io_test_last_frame.clear();  // fresh grab: discard any prior still
            s_io_test_grab = true;
        }
    }
    if (w <= 0 || h <= 0) {
        PyErr_SetString(PyExc_RuntimeError, "io_test_camera_start: no active io_test_screen");
        return nullptr;
    }
    int err = camera_engine_start();
    if (err != CAMERA_OK) {
        // Bring-up failed: the engine is torn down (g == nullptr), so the pump's consume
        // no longer calls blit_rgb565 — disarm the redirect under the lock all the same.
        LvglLockGuard _lvgl_guard;
        s_io_test_grab = false;
        PyErr_SetObject(PyExc_OSError, Py_BuildValue("(is)", err, camera_error_str(err)));
        return nullptr;
    }
    Py_RETURN_NONE;
#else
    PyErr_SetString(PyExc_RuntimeError, "io_test_camera_start: camera engine not built");
    return nullptr;
#endif
}

// io_test_camera_stop() -> None. Stop the engine (the last frame stays frozen in the
// io_test plane) + clear the redirect. Idempotent — safe if start() failed or was skipped.
PyObject *py_io_test_camera_stop(PyObject *self, PyObject *args) {
    (void)self;
    (void)args;
#ifdef SS_CAMERA_ENGINE
    // Stop OUTSIDE the LVGL lock (it joins the engine worker threads — must not stall the
    // pump). After it returns g == nullptr, so the pump's consume no longer calls
    // blit_rgb565: disarming the redirect + revealing the still below is then race-free.
    camera_engine_stop();
#endif
    LvglLockGuard _lvgl_guard;
    s_io_test_grab = false;
    // Reveal ONLY the final captured frame — a single still, no live video. Empty if the
    // engine delivered nothing (camera failed to start/produce a frame); then leave the
    // plane dark. Sizing/no-active-screen are guarded inside io_test_blit_camera.
    if (!s_io_test_last_frame.empty()) {
        io_test_blit_camera(s_io_test_last_frame.data(), s_io_test_last_frame.size());
        s_io_test_last_frame.clear();
    }
    Py_RETURN_NONE;
}

// io_test_camera_frame_ready() -> bool
// True once the background pump has stashed at least one frame during the active grab
// (blit_rgb565 above). The pump feeds frames asynchronously and the libcamera cold-start
// (engine start -> first delivered frame) can exceed a second on the Pi Zero, so the host
// polls this to wait for a confirmed first frame before io_test_camera_stop() freezes the
// plane. Read under the LVGL lock so it serializes against the pump thread's stash write
// (blit_rgb565 runs under that lock).
PyObject *py_io_test_camera_frame_ready(PyObject *self, PyObject *args) {
    (void)self;
    (void)args;
    bool ready;
    {
        LvglLockGuard _lvgl_guard;
        ready = !s_io_test_last_frame.empty();
    }
    if (ready) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

// --- Frame push -------------------------------------------------------------
// camera_preview_set_frame(frame: bytes) -> None
// frame is LVGL-native RGB565, exactly w*h*2 bytes (see the FRAME FORMAT CONTRACT
// at the top). Zero-copy read of the buffer, memcpy into the sink, invalidate.
// Safe no-op when no session is active.
PyObject *py_camera_preview_set_frame(PyObject *self, PyObject *args) {
    (void)self;

    Py_buffer view;
    if (!PyArg_ParseTuple(args, "y*", &view)) {  // y* == read-only bytes-like, zero-copy
        return nullptr;
    }

    if (!s_cam_img || !s_cam_data) {
        PyBuffer_Release(&view);
        Py_RETURN_NONE;  // no active session — no-op
    }

    LvglLockGuard _lvgl_guard;

    if (static_cast<size_t>(view.len) != s_cam_size) {
        PyErr_Format(PyExc_ValueError,
                     "camera_preview_set_frame: expected %zu RGB565 bytes, got %zd",
                     s_cam_size, (Py_ssize_t)view.len);
        PyBuffer_Release(&view);
        return nullptr;
    }

    memcpy(s_cam_data, view.buf, s_cam_size);
    PyBuffer_Release(&view);
    lv_obj_invalidate(s_cam_img);  // mark dirty; the next pump re-reads the sink buffer

    Py_RETURN_NONE;
}

// camera_preview_set_frame_yuv420(buf, src_w, src_h, y_stride, uv_stride, rotate) -> None
// Convert a planar I420/YUV420 frame (Y plane, then U, then V; U/V at half resolution)
// straight into the LVGL-native RGB565 sink, applying a 0/90/180/270-degree rotation.
// The three planes are contiguous in buf at Y=0, U=y_stride*src_h, V=U+uv_stride*(src_h/2)
// (the RPi vc4 YUV420 layout). No scaling: the ROTATED source dims must equal the sink
// dims. This is the Phase-0 replacement for the old per-frame numpy RGB->RGB565 convert
// (numpy is gone on the libcamera dev image); the platform owns the pixel plane, so the
// host converts here rather than shipping pre-made RGB565. BT.601 studio-range coeffs.
PyObject *py_camera_preview_set_frame_yuv420(PyObject *self, PyObject *args) {
    (void)self;

    Py_buffer view;
    int src_w, src_h, y_stride, uv_stride, rotate;
    if (!PyArg_ParseTuple(args, "y*iiiii", &view, &src_w, &src_h, &y_stride, &uv_stride, &rotate)) {
        return nullptr;
    }

    if (!s_cam_img || !s_cam_data) {
        PyBuffer_Release(&view);
        Py_RETURN_NONE;  // no active session — no-op
    }

    LvglLockGuard _lvgl_guard;

    if (src_w <= 0 || src_h <= 0 || (src_w & 1) || (src_h & 1) ||
        y_stride < src_w || uv_stride < (src_w + 1) / 2) {
        PyErr_SetString(PyExc_ValueError, "set_frame_yuv420: bad src dims/strides");
        PyBuffer_Release(&view);
        return nullptr;
    }

    const int  dst_w = static_cast<int>(s_cam_dsc.header.w);
    const int  dst_h = static_cast<int>(s_cam_dsc.header.h);
    const bool swap  = (rotate == 90 || rotate == 270);
    const int  rot_w = swap ? src_h : src_w;
    const int  rot_h = swap ? src_w : src_h;
    if (rot_w != dst_w || rot_h != dst_h) {
        PyErr_Format(PyExc_ValueError,
                     "set_frame_yuv420: rotated src %dx%d != sink %dx%d (no scaling)",
                     rot_w, rot_h, dst_w, dst_h);
        PyBuffer_Release(&view);
        return nullptr;
    }

    const size_t y_size  = static_cast<size_t>(y_stride) * src_h;
    const size_t uv_size = static_cast<size_t>(uv_stride) * (src_h / 2);
    const size_t need    = y_size + 2 * uv_size;
    if (static_cast<size_t>(view.len) < need) {
        PyErr_Format(PyExc_ValueError,
                     "set_frame_yuv420: buffer %zd < needed %zu", (Py_ssize_t)view.len, need);
        PyBuffer_Release(&view);
        return nullptr;
    }

    const uint8_t *base = static_cast<const uint8_t *>(view.buf);
    const uint8_t *Yp = base;
    const uint8_t *Up = base + y_size;
    const uint8_t *Vp = Up + uv_size;
    uint16_t *dst = reinterpret_cast<uint16_t *>(s_cam_data);

    for (int sy = 0; sy < src_h; ++sy) {
        const uint8_t *yrow = Yp + static_cast<size_t>(sy) * y_stride;
        const uint8_t *urow = Up + static_cast<size_t>(sy >> 1) * uv_stride;
        const uint8_t *vrow = Vp + static_cast<size_t>(sy >> 1) * uv_stride;
        for (int sx = 0; sx < src_w; ++sx) {
            const int c = 298 * (static_cast<int>(yrow[sx]) - 16);
            const int u = static_cast<int>(urow[sx >> 1]) - 128;
            const int v = static_cast<int>(vrow[sx >> 1]) - 128;
            int r = (c + 409 * v + 128) >> 8;
            int g = (c - 100 * u - 208 * v + 128) >> 8;
            int b = (c + 516 * u + 128) >> 8;
            r = r < 0 ? 0 : (r > 255 ? 255 : r);
            g = g < 0 ? 0 : (g > 255 ? 255 : g);
            b = b < 0 ? 0 : (b > 255 ? 255 : b);
            const uint16_t px = static_cast<uint16_t>(
                ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));

            int dx, dy;
            switch (rotate) {
                case 90:  dx = src_h - 1 - sy; dy = sx;             break;
                case 180: dx = src_w - 1 - sx; dy = src_h - 1 - sy; break;
                case 270: dx = sy;             dy = src_w - 1 - sx; break;
                default:  dx = sx;             dy = sy;             break;  // 0
            }
            dst[static_cast<size_t>(dy) * dst_w + dx] = px;
        }
    }

    PyBuffer_Release(&view);
    lv_obj_invalidate(s_cam_img);
    Py_RETURN_NONE;
}

// --- Overlay state (a few updates/sec, never per frame) ---------------------
// camera_preview_set_progress(percent: int, frame_status: int) -> None
// frame_status: 0 none / 1 added(green) / 2 repeated(gray) / 3 miss(hidden),
// matching Python ScanScreen FRAME__*. Implies scanning (raises the bar).
PyObject *py_camera_preview_set_progress(PyObject *self, PyObject *args) {
    (void)self;

    int percent = 0;
    int frame_status = 0;
    if (!PyArg_ParseTuple(args, "ii", &percent, &frame_status)) {
        return nullptr;
    }
    LvglLockGuard _lvgl_guard;
    if (s_overlay) {
        camera_preview_overlay_set_progress(
            s_overlay, percent, (camera_overlay_frame_status_t)frame_status);
    }
    Py_RETURN_NONE;
}

// C++ helper shared by the Python binding and camera_scanner.start(): toggle the
// overlay between the back-affordance state and the scanning status-bar state.
void camera_preview_set_scanning_active(bool active) {
    LvglLockGuard _lvgl_guard;
    if (s_overlay) {
        camera_preview_overlay_set_scanning(s_overlay, active);
    }
}

// Drive the overlay status bar + dot from camera_scanner.report()/report_complete().
// Arg order (frame_status, percent) matches the ESP camera_scanner.report() contract
// — the REVERSE of set_progress(percent, frame_status), which retires with the rest of
// the Phase-0 surface. No-op when no overlay is active.
void camera_preview_report(int frame_status, int percent) {
    if (!s_overlay) return;
    LvglLockGuard _lvgl_guard;
    // Only a multi-part decode ever reports a percent strictly between 0 and 100; an idle/held
    // frame reports pct == 0 (FRAME_REPEAT/FRAME_NONE) and does not count as partial progress.
    if (percent > 0 && percent < 100) s_scan_saw_partial = true;
    // Until a multi-part frame is actually decoded, stay in the back-affordance state (the
    // "< back | Scan a QR code" instruction, hardware mode). set_progress() implies scanning
    // (it raises the bar and hides the instruction), so gate every report until the first real
    // partial: idle/held frames (pct == 0) and a static QR's lone completion (pct >= 100 with no
    // partial ever seen) both stay suppressed -> the bar never appears for a static scan, and the
    // instruction line stays up until an animated QR starts decoding. Matches the ESP.
    if (!s_scan_saw_partial) return;
    camera_preview_overlay_set_progress(
        s_overlay, percent, (camera_overlay_frame_status_t)frame_status);
}

// Segmented (indexed-cycle) progress for BBQR/Specter, driven by
// camera_scanner.begin_segments()/segment_event(). The SCREEN owns the decoded set: the
// host announces the cycle size once, then streams one event per decode frame and the
// overlay derives the percent from its own lit count. UR/fountain + single-frame QRs stay
// on camera_preview_report() (the continuous bar). No-op when no overlay is active.
void camera_preview_begin_segments(int total_segments) {
    LvglLockGuard _lvgl_guard;
    if (s_overlay) {
        camera_preview_overlay_begin_segments(s_overlay, total_segments);
    }
}

void camera_preview_segment_event(int frame_status, int piece_index) {
    LvglLockGuard _lvgl_guard;
    if (s_overlay) {
        camera_preview_overlay_segment_event(
            s_overlay, (camera_overlay_frame_status_t)frame_status, piece_index);
    }
}

// camera_preview_set_scanning(active: bool) -> None
// Toggle between the back-affordance state and the status-bar state.
PyObject *py_camera_preview_set_scanning(PyObject *self, PyObject *args) {
    (void)self;

    int active = 0;
    if (!PyArg_ParseTuple(args, "p", &active)) {
        return nullptr;
    }
    LvglLockGuard _lvgl_guard;
    camera_preview_set_scanning_active(active != 0);
    Py_RETURN_NONE;
}

// --- Teardown ---------------------------------------------------------------
// camera_preview_close() -> None
// End the session: destroy the overlay handle + free the sink buffer. Call this when
// the scan ends, BEFORE loading the next screen (the next build's
// load_screen_and_cleanup_previous reaps our screen object). Idempotent.
PyObject *py_camera_preview_close(PyObject *self, PyObject *args) {
    (void)self;
    (void)args;
    LvglLockGuard _lvgl_guard;
    // Reset LVGL's idle clock so the successor screen gets a full screensaver window:
    // a no-input scan leaves inactive-time large, which would otherwise immediately
    // fire the saver over the next (flag-free) screen.
    camera_preview_close_session();
    Py_RETURN_NONE;
}
