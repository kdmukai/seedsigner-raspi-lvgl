// Internal contract between the translation units of the seedsigner_lvgl_screens
// CPython extension. Every .cpp in this directory includes this header FIRST —
// it owns the PY_SSIZE_T_CLEAN-before-Python.h ordering requirement.
//
// Extension layout (one subsystem per file):
//   module.cpp          — PyMethodDef table + module init (the public API index)
//   lvgl_runtime.cpp    — LVGL lifecycle: init/shutdown/pump/tick, flush dispatch,
//                         resolution switch, save/restore/clear screen
//   display_st7789.cpp  — ST7789 SPI panel driver + GPIO output lines + flush flags
//   gpio_input.cpp      — joystick/key GPIO input lines + LVGL keypad indev callback
//   gpio_lines.cpp      — generic gpiochip-ioctl / sysfs GPIO line helpers
//   result_queue.cpp    — host result queue + seedsigner_lvgl_on_* callbacks
//   screens.cpp         — Python wrappers for the portable LVGL screens
//   camera_preview.cpp  — Pi live camera-preview sink (lv_image) + portable overlay
//   toast.cpp           — native toast overlay binding (show_toast / dismiss_toast)
//   locale_packs.cpp    — language-pack discovery/loading (set_locale & co.)
#ifndef SS_LVGL_MODULE_INTERNAL_H
#define SS_LVGL_MODULE_INTERNAL_H

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "lvgl.h"

#include <cstdint>
#include <string>
#include <vector>

// --- lvgl_runtime.cpp -------------------------------------------------------

// Throws std::runtime_error if lvgl_init() has not been called.
void require_lvgl_runtime();
bool lvgl_runtime_is_inited();

// LVGL global lock (RASPI-5). LVGL core is not thread-safe; every host->widget-tree
// call and the pump loop's lv_timer_handler holds this recursive lock so the
// background pump thread (Phase 2) and the host binding thread serialize. Recursive
// so a locked mutator may call a helper that re-locks (e.g. clear_screen ->
// lvgl_clear_to_black -> lvgl_runtime_pump). Strict lock ordering: the pump thread
// takes ONLY this lock; host bindings hold the GIL then take it — so NEVER acquire
// the GIL while holding this lock (that is the GIL<->LVGL deadlock the design avoids).
// Uncontended (a no-op) until Phase 2 spawns the pump thread.
void lvgl_lock();
void lvgl_unlock();
// RAII: lock for the enclosing scope, unlock on every exit path (including throws).
struct LvglLockGuard {
    LvglLockGuard() { lvgl_lock(); }
    ~LvglLockGuard() { lvgl_unlock(); }
    LvglLockGuard(const LvglLockGuard &) = delete;
    LvglLockGuard &operator=(const LvglLockGuard &) = delete;
};
// Drive lv_timer_handler for duration_ms (sleep_ms between iterations).
// Returns 0 normally, -1 if a Python exception/signal is pending.
int lvgl_runtime_pump(unsigned int duration_ms, unsigned int sleep_ms);
// Stop + join the RASPI-5 Phase-2 background pump thread. Idempotent. Registered with
// Py_AtExit at module init so the thread is joined even if the app never calls
// lvgl_shutdown() (a joinable std::thread would std::terminate at static destruction).
extern "C" void lvgl_runtime_join_pump_thread(void);
// Load a fresh all-black screen and pump briefly so it reaches the panel.
// clean_sys_layer additionally clears lv_layer_sys() overlays first.
void lvgl_clear_to_black(bool clean_sys_layer);

// --- display_st7789.cpp -----------------------------------------------------

// True when the native ST7789 flush path is initialized and selected.
bool native_flush_active();
// Select native flush as the flush mode (RASPI-5 Phase 2 — armed at lvgl_init so the
// background pump never takes the Python flush path). Gated by s_native.ready, so it only
// paints once native_display_init has brought up the panel.
void native_flush_select_native();
// Native-path flush body: debug logging, optional RGB565 byte swap, SPI blit.
// Catches and logs its own errors (a failed flush must not kill the pump loop).
void native_flush_blit(const lv_area_t *area, const uint8_t *px_map, size_t nbytes);
// Push the panel MADCTL/geometry for a new resolution (no-op unless the native
// flush path is active).
void display_update_resolution(uint32_t width, uint32_t height);
// Tear down SPI + GPIO output lines (clears the panel to black first when both
// the panel and the LVGL runtime are up). Idempotent.
void native_display_shutdown_internal();
bool native_gpiochip_ready();
int native_gpio_chip_fd();
// Open /dev/gpiochip0 for input-only use: display lines are NOT claimed, but the
// chip is marked ready so input line requests can proceed. Throws on failure.
int native_gpio_chip_open_for_input();

// --- gpio_input.cpp ---------------------------------------------------------

// Claim the joystick/key input lines from the already-open gpiochip. Logs and
// returns (no throw) when the gpiochip is not ready.
void native_input_init();
void native_input_shutdown();
// Open the gpiochip if needed (display lines unclaimed), then claim input lines.
// For use when an external driver owns the display. Throws on gpiochip failure.
void native_input_only_init();
// LVGL keypad indev read callback (registered by the runtime).
void native_input_read_cb(lv_indev_t *indev, lv_indev_data_t *data);

// --- gpio_lines.cpp ---------------------------------------------------------

void write_text_file(const std::string &path, const std::string &value);
void gpio_export_pin(int pin);
void gpio_unexport_pin(int pin);   // best-effort, never throws
void gpio_set_dir_out(int pin);
int gpiochip_request_line(int chip_fd, int pin, const char *consumer);
int gpiochip_request_input_line(int chip_fd, int pin, const char *consumer);
int gpio_line_read(int fd);

// --- locale_packs.cpp -------------------------------------------------------

// Filesystem pack-byte provider shared by set_locale and the locale picker's
// endonym-image rows: serves <font_dir>/<locale>/<file> verbatim. The loader
// copies what it keeps, so the scratch buffer only lives across one call.
struct FsPackCtx {
    std::string font_dir;
    std::vector<uint8_t> scratch;
};
bool fs_pack_provider(const char *locale, const char *file,
                      const uint8_t **bytes, size_t *len, void *user);

// --- Python entry points (the methods[] table in module.cpp) ----------------

// lvgl_runtime.cpp
PyObject *py_lvgl_init(PyObject *self, PyObject *args, PyObject *kwargs);
PyObject *py_lvgl_shutdown(PyObject *self, PyObject *args);
PyObject *py_set_resolution(PyObject *self, PyObject *args, PyObject *kwargs);
PyObject *py_display_size(PyObject *self, PyObject *args);
PyObject *py_lvgl_pump(PyObject *self, PyObject *args, PyObject *kwargs);
PyObject *py_set_flush_callback(PyObject *self, PyObject *args);
PyObject *py_save_screen(PyObject *self, PyObject *args);
PyObject *py_restore_screen(PyObject *self, PyObject *args);
PyObject *py_clear_screen(PyObject *self, PyObject *args);
PyObject *py_set_screensaver_timeout(PyObject *self, PyObject *args);
PyObject *py_get_inactive_time_ms(PyObject *self, PyObject *args);
PyObject *py_set_camera_rotation(PyObject *self, PyObject *args);

// display_st7789.cpp
PyObject *py_native_display_init(PyObject *self, PyObject *args, PyObject *kwargs);
PyObject *py_native_display_shutdown(PyObject *self, PyObject *args);
PyObject *py_native_display_test_pattern(PyObject *self, PyObject *args);
PyObject *py_native_debug_config(PyObject *self, PyObject *args, PyObject *kwargs);
PyObject *py_set_flush_mode(PyObject *self, PyObject *args);

// gpio_input.cpp
PyObject *py_native_input_init(PyObject *self, PyObject *args);

// result_queue.cpp
PyObject *py_poll_for_result(PyObject *self, PyObject *args);
PyObject *py_clear_result_queue(PyObject *self, PyObject *args);
PyObject *py_debug_emit_result(PyObject *self, PyObject *args);
PyObject *py_debug_emit_qr_density(PyObject *self, PyObject *args);
PyObject *py_debug_emit_aux_key(PyObject *self, PyObject *args);

// locale_packs.cpp
PyObject *py_set_locale(PyObject *self, PyObject *args);
PyObject *py_unload_locale(PyObject *self, PyObject *args);
PyObject *py_discover_locale_packs(PyObject *self, PyObject *args);
PyObject *py_list_available_locales(PyObject *self, PyObject *args);

// screens.cpp — non-screen companions
PyObject *py_qr_display_set_frame(PyObject *self, PyObject *args);
PyObject *py_qr_display_is_tip_active(PyObject *self, PyObject *args);
// io_test_screen host-push companion: reflect the async camera grab (IDLE/CAPTURING/CAPTURED).
PyObject *py_io_test_set_capture_state(PyObject *self, PyObject *args);
// io_test_screen camera plane (SCREENS-9): report the square side + blit a host still.
PyObject *py_io_test_get_camera_plane_dims(PyObject *self, PyObject *args);
PyObject *py_io_test_blit_camera(PyObject *self, PyObject *args);
PyObject *py_debug_last_path(PyObject *self, PyObject *args);
// Mark a successful build in the shared _debug_last_path breadcrumb (for peer
// subsystems whose builders live outside screens.cpp, e.g. camera_preview.cpp).
void mark_last_path_compiled();

// camera_preview.cpp — live camera-preview scan surface (pixel sink + overlay)
PyObject *py_camera_preview_screen(PyObject *self, PyObject *args);
PyObject *py_camera_preview_set_frame(PyObject *self, PyObject *args);
PyObject *py_camera_preview_set_frame_yuv420(PyObject *self, PyObject *args);
PyObject *py_camera_preview_set_progress(PyObject *self, PyObject *args);
PyObject *py_camera_preview_set_scanning(PyObject *self, PyObject *args);
PyObject *py_camera_preview_close(PyObject *self, PyObject *args);
// io_test single-frame grab (camera_preview.cpp): start/stop the engine feeding the io_test plane.
PyObject *py_io_test_camera_start(PyObject *self, PyObject *args);
PyObject *py_io_test_camera_stop(PyObject *self, PyObject *args);
// Build the preview screen + overlay (shared by py_camera_preview_screen and the
// native camera_scanner.start()). Throws std::runtime_error on failure.
void camera_preview_build_session(const std::string &instructions);
// End the live session: drop the overlay + sink and reset the idle clock. Shared by
// py_camera_preview_close and camera_scanner.stop(). Idempotent.
void camera_preview_close_session();
// Toggle the overlay's scanning state (shared by the binding + camera_scanner.start()).
void camera_preview_set_scanning_active(bool active);
// Drive the overlay bar/dot from camera_scanner.report() — arg order (frame_status,
// percent) per the ESP contract (reverse of set_progress).
void camera_preview_report(int frame_status, int percent);
// Segmented-progress forwarders (camera_scanner.begin_segments()/segment_event()):
// BBQR/Specter indexed cycles; the screen owns the decoded set. No-op without an overlay.
void camera_preview_begin_segments(int total_segments);
void camera_preview_segment_event(int frame_status, int piece_index);
// Tear the live session down before lv_deinit() so its statics can't dangle into
// the next lvgl_init(); called from lvgl_runtime_shutdown().
void camera_preview_on_lvgl_shutdown();

// camera_scanner.cpp — nested Python module implementing the ESP camera_scanner
// poll contract (Phase 1: start/stop/is_running). Attaches the submodule object to
// the parent extension at PyInit time; returns 0 on success, -1 with a Python error
// set. Only present under the SS_CAMERA_ENGINE build.
int camera_scanner_attach(PyObject *parent);

// camera_preview.cpp — image-entropy session (reuses the scan sink; builds the portable
// camera_entropy_overlay instead of the scan overlay). Shared by the camera_entropy
// binding. build_session throws std::runtime_error on failure.
void camera_entropy_build_session(const std::string &preview_instructions,
                                  const std::string &confirm_instructions,
                                  const std::string &capturing_text,
                                  const std::string &accept_label);
// Flip the entropy overlay phase (0 PREVIEW / 1 CAPTURING / 2 CONFIRM). No-op if none active.
void camera_entropy_set_phase(int phase);
// Build the CONFIRM review image (aspect-fit with a capped letterbox + color-preserving
// contrast stretch via the portable image_entropy_process) from the latched RAW frame and
// hand it to the overlay. DISPLAY-ONLY — never fed into the entropy chain. src_w/src_h are
// the latched still's own display-orientation dims (from entropy_coord_get_result), NOT the
// sink dims — the still is wider and higher-resolution than the preview.
void camera_entropy_build_confirm_image(const uint8_t *raw_rgb565, int src_w, int src_h);

// camera_entropy.cpp — nested Python module implementing the ESP camera_entropy contract
// (start/stop/set_labels/is_running/frames_chained/capture/get_result/resume). Attaches the
// submodule at PyInit time; returns 0 on success, -1 with a Python error set. CAMERA_ENGINE only.
int camera_entropy_attach(PyObject *parent);

// toast.cpp — native LVGL toast overlay (transient bottom banner over the live screen)
PyObject *py_show_toast(PyObject *self, PyObject *args);
PyObject *py_dismiss_toast(PyObject *self, PyObject *args);

// screens.cpp — screen builders (all take cfg_dict except screensaver_screen)
PyObject *py_button_list_screen(PyObject *self, PyObject *args);
PyObject *py_main_menu_screen(PyObject *self, PyObject *args);
PyObject *py_large_icon_status_screen(PyObject *self, PyObject *args);
PyObject *py_keyboard_screen(PyObject *self, PyObject *args);
PyObject *py_seed_add_passphrase_screen(PyObject *self, PyObject *args);
PyObject *py_seed_mnemonic_entry_screen(PyObject *self, PyObject *args);
PyObject *py_seed_finalize_screen(PyObject *self, PyObject *args);
PyObject *py_seed_export_xpub_details_screen(PyObject *self, PyObject *args);
PyObject *py_seed_review_passphrase_screen(PyObject *self, PyObject *args);
PyObject *py_seed_words_screen(PyObject *self, PyObject *args);
PyObject *py_seed_transcribe_zoomed_qr_screen(PyObject *self, PyObject *args);
PyObject *py_seed_transcribe_whole_qr_screen(PyObject *self, PyObject *args);
PyObject *py_seed_transcribe_seedqr_format_screen(PyObject *self, PyObject *args);
PyObject *py_qr_display_screen(PyObject *self, PyObject *args);
PyObject *py_opening_splash_screen(PyObject *self, PyObject *args);
PyObject *py_loading_spinner_screen(PyObject *self, PyObject *args);
PyObject *py_psbt_overview_screen(PyObject *self, PyObject *args);
PyObject *py_psbt_address_details_screen(PyObject *self, PyObject *args);
PyObject *py_psbt_change_details_screen(PyObject *self, PyObject *args);
PyObject *py_psbt_math_screen(PyObject *self, PyObject *args);
PyObject *py_psbt_op_return_screen(PyObject *self, PyObject *args);
PyObject *py_multisig_wallet_descriptor_screen(PyObject *self, PyObject *args);
PyObject *py_seed_address_verification_screen(PyObject *self, PyObject *args);
PyObject *py_seed_address_verification_set_progress(PyObject *self, PyObject *args);
PyObject *py_seed_address_verification_success_screen(PyObject *self, PyObject *args);
PyObject *py_seed_sign_message_confirm_address_screen(PyObject *self, PyObject *args);
PyObject *py_seed_sign_message_confirm_message_screen(PyObject *self, PyObject *args);
PyObject *py_settings_qr_confirmation_screen(PyObject *self, PyObject *args);
PyObject *py_settings_locale_picker_screen(PyObject *self, PyObject *args);
PyObject *py_tools_address_explorer_address_type_screen(PyObject *self, PyObject *args);
PyObject *py_tools_address_explorer_address_list_screen(PyObject *self, PyObject *args);
PyObject *py_tools_calc_final_word_screen(PyObject *self, PyObject *args);
PyObject *py_tools_calc_final_word_done_screen(PyObject *self, PyObject *args);
PyObject *py_reset_screen(PyObject *self, PyObject *args);
PyObject *py_power_off_not_required_screen(PyObject *self, PyObject *args);
PyObject *py_power_options_screen(PyObject *self, PyObject *args);
PyObject *py_donate_screen(PyObject *self, PyObject *args);
PyObject *py_version_screen(PyObject *self, PyObject *args);
PyObject *py_io_test_screen(PyObject *self, PyObject *args);
PyObject *py_screensaver_screen(PyObject *self, PyObject *args);

#endif  // SS_LVGL_MODULE_INTERNAL_H
