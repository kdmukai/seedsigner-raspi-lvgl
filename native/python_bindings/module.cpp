// seedsigner_lvgl_screens — CPython extension entry point.
//
// This file is ONLY the public API index: the methods[] table below is the
// authoritative list of everything the extension exports, and each docstring is
// the short-form contract. Implementations live in the sibling .cpp files (see
// module_internal.h for the file map and the cross-unit API).
#include "module_internal.h"

static PyMethodDef methods[] = {
    // --- LVGL runtime lifecycle (lvgl_runtime.cpp) --------------------------
    {"lvgl_init", (PyCFunction)py_lvgl_init, METH_VARARGS | METH_KEYWORDS, "Initialize LVGL runtime."},
    {"lvgl_shutdown", py_lvgl_shutdown, METH_NOARGS, "Shutdown LVGL runtime."},
    {"set_resolution", (PyCFunction)py_set_resolution, METH_VARARGS | METH_KEYWORDS, "Switch LVGL display resolution (e.g. 240x240 to 320x240)."},
    {"display_size", py_display_size, METH_NOARGS, "display_size(): (width, height) of the active LVGL display profile."},
    {"lvgl_pump", (PyCFunction)py_lvgl_pump, METH_VARARGS | METH_KEYWORDS, "Pump LVGL timers/input for duration_ms."},
    {"set_flush_callback", py_set_flush_callback, METH_VARARGS, "Set display flush callback(area, bytes)."},
    {"save_screen", py_save_screen, METH_NOARGS, "Save active LVGL screen for later restore."},
    {"restore_screen", py_restore_screen, METH_NOARGS, "Restore previously saved LVGL screen."},
    {"clear_screen", py_clear_screen, METH_NOARGS, "Clear display to black."},
    {"set_screensaver_timeout", py_set_screensaver_timeout, METH_VARARGS, "set_screensaver_timeout(ms): idle ms before the native screensaver activates (0 disables)."},
    {"get_inactive_time_ms", py_get_inactive_time_ms, METH_NOARGS, "get_inactive_time_ms(): milliseconds since the last input activity (any keypad press resets it toward 0). Native replacement for HardwareButtons.has_any_input() polling — a small value means the user just interacted. Requires a running pump for presses to register; raises RuntimeError before lvgl_init()."},
    {"set_camera_rotation", py_set_camera_rotation, METH_VARARGS, "set_camera_rotation(degrees): sticky camera rotation for BOTH camera flows. Pass the raw app setting (0/90/180/270); the sensor-mount base is composed natively. Applies to the next start()."},

    // --- Native ST7789 display backend (display_st7789.cpp) -----------------
    {"native_display_init", (PyCFunction)py_native_display_init, METH_VARARGS | METH_KEYWORDS, "Init native ST7789 backend."},
    {"native_display_shutdown", py_native_display_shutdown, METH_NOARGS, "Shutdown native ST7789 backend."},
    {"native_display_test_pattern", py_native_display_test_pattern, METH_NOARGS, "Render native RGB565 test bands."},
    {"native_debug_config", (PyCFunction)py_native_debug_config, METH_VARARGS | METH_KEYWORDS, "Configure native flush debug logging."},
    {"set_flush_mode", py_set_flush_mode, METH_VARARGS, "Set flush mode: native|python."},

    // --- GPIO input (gpio_input.cpp) -----------------------------------------
    {"native_input_init", py_native_input_init, METH_NOARGS, "Init GPIO input only (no display)."},

    // --- Locale / language packs (locale_packs.cpp) --------------------------
    {"set_locale", py_set_locale, METH_VARARGS, "set_locale(locale, font_dir='lang-packs'): load locale font packs from <font_dir>/<locale>/. Returns True on success, False if a pack is missing (falls back to baked English)."},
    {"unload_locale", py_unload_locale, METH_NOARGS, "Clear loaded locale packs and restore the baked Western floor."},
    {"discover_locale_packs", py_discover_locale_packs, METH_VARARGS, "discover_locale_packs(font_dir='lang-packs'): (re)scan <font_dir>/<locale>/manifest.json and register each pack so set_locale works for not-compiled-in locales. Returns count registered. Skips desktop-OS junk and bad/half-copied packs."},
    {"list_available_locales", py_list_available_locales, METH_VARARGS, "list_available_locales(font_dir='lang-packs'): list of {code, endonym, image, has_image} for each pack present under <font_dir>, for assembling the locale picker cfg."},

    // --- Screen builders (screens.cpp) ---------------------------------------
    // Every screen builds its widget tree and returns immediately; the host
    // pumps (lvgl_pump) and polls (poll_for_result) for the result tuples named
    // in each docstring. "back"/"power" below denote the top-nav affordances,
    // which arrive as button_selected with index 1000/1001 (RET_CODE__BACK/
    // POWER_BUTTON) — not a distinct kind (see result_queue.cpp).
    {"button_list_screen", py_button_list_screen, METH_VARARGS, "Build the button list screen (returns immediately; pump + poll for the result)."},
    {"main_menu_screen", py_main_menu_screen, METH_VARARGS, "Build the main menu screen (2x2 grid; optional cfg localizes title + labels); returns immediately."},
    {"large_icon_status_screen", py_large_icon_status_screen, METH_VARARGS, "Build the large-icon status screen (returns immediately; pump + poll for the result). status_type is 'success'|'warning'|'dire_warning'|'error', or 'custom' with a caller-supplied 'icon' glyph + 'icon_color' (powers PSBTFinalize / microSD notification)."},
    {"keyboard_screen", py_keyboard_screen, METH_VARARGS, "Build the generalized keyboard entry screen (BIP-85 index / derivation path / dice / coin flip); result is text_entered or back."},
    {"seed_add_passphrase_screen", py_seed_add_passphrase_screen, METH_VARARGS, "Build the BIP39 passphrase entry screen; result is text_entered or back."},
    {"seed_mnemonic_entry_screen", py_seed_mnemonic_entry_screen, METH_VARARGS, "Build the BIP39 seed-word entry screen (autocomplete over cfg['wordlist']); result is text_entered (chosen word) or back."},
    {"seed_finalize_screen", py_seed_finalize_screen, METH_VARARGS, "Build the finalize-seed screen (fingerprint readout + bottom button list; cfg requires 'fingerprint'); result is button_selected."},
    {"seed_export_xpub_details_screen", py_seed_export_xpub_details_screen, METH_VARARGS, "Build the xpub-export summary (fingerprint/derivation/truncated-xpub IconTextLines + yellow privacy edge; cfg requires 'fingerprint' and 'xpub'); result is button_selected or back."},
    {"seed_review_passphrase_screen", py_seed_review_passphrase_screen, METH_VARARGS, "Build the review-passphrase screen (orange fixed-width passphrase + changes-fingerprint readout; cfg requires 'passphrase'); result is button_selected or back."},
    {"seed_words_screen", py_seed_words_screen, METH_VARARGS, "Build one host-paginated page of seed words (numbered chips + orange dire-warning edge; cfg requires a non-empty 'words' array); result is button_selected or back."},
    {"seed_transcribe_whole_qr_screen", py_seed_transcribe_whole_qr_screen, METH_VARARGS, "Build the whole-QR SeedQR transcription overview (full QR + title; precursor to the zoomed screen; cfg requires 'qr_data', optional qr_mode/data_encoding/border); result is button_selected or back."},
    {"seed_transcribe_seedqr_format_screen", py_seed_transcribe_seedqr_format_screen, METH_VARARGS, "Build the SeedQR-format chooser ([Standard, Compact] over two caption/value rows; cfg requires 'top_nav.title', non-empty 'button_list', and 'standard_label'/'standard_text'/'compact_label'/'compact_text'); result is button_selected or back."},
    {"seed_transcribe_zoomed_qr_screen", py_seed_transcribe_zoomed_qr_screen, METH_VARARGS, "Build the zoomed, pannable SeedQR transcription screen (one highlighted zone window over the dimmed QR field; pattern mask-matched to python-qrcode; cfg requires 'qr_data', optional qr_mode/data_encoding/exit_text/initial_zone_x/initial_zone_y); static, result is back on exit."},
    {"qr_display_screen", py_qr_display_screen, METH_VARARGS, "Build the native QR display screen (static or animated); result is qr_brightness then back on exit."},
    {"qr_display_set_frame", py_qr_display_set_frame, METH_VARARGS, "Push the next animated-QR frame (bytes or str) into the live qr_display_screen."},
    {"qr_display_is_tip_active", py_qr_display_is_tip_active, METH_NOARGS, "True while the QR brightness tip/panel is up; the animation driver holds while true."},
    {"opening_splash_screen", py_opening_splash_screen, METH_VARARGS, "Build the opening splash (optional cfg localizes version/sponsor + toggles partner logos); emits button_selected(-1, 'splash_complete') on completion."},
    {"loading_spinner_screen", py_loading_spinner_screen, METH_VARARGS, "Build the self-animating loading spinner (optional cfg {'text':...}); pure builder, fire-and-forget — no result, torn down when the next screen loads."},
    {"psbt_overview_screen", py_psbt_overview_screen, METH_VARARGS, "Build the animated PSBT transaction-overview pictogram (inputs->center bar->destinations) + BtcAmount headline; result is button_selected (Review details) or back."},
    {"psbt_address_details_screen", py_psbt_address_details_screen, METH_VARARGS, "Build the per-recipient address-review screen (amount over the full wrapped address; cfg requires 'address'); result is button_selected or back."},
    {"psbt_change_details_screen", py_psbt_change_details_screen, METH_VARARGS, "Build the change/self-receive review screen (amount + address-type label + address + optional 'Address verified!'; cfg requires 'address'); result is button_selected or back."},
    {"psbt_math_screen", py_psbt_math_screen, METH_VARARGS, "Build the fee-math equation screen (input - recipients - fee = change; host passes formatted amount strings); result is button_selected or back."},
    {"psbt_op_return_screen", py_psbt_op_return_screen, METH_VARARGS, "Build the PSBT OP_RETURN review (raw 'hex' and/or decoded 'text' + caption; cfg all-optional, button_list default ['Done']); result is button_selected or back."},
    {"multisig_wallet_descriptor_screen", py_multisig_wallet_descriptor_screen, METH_VARARGS, "Build the multisig wallet-descriptor review (policy + participating fingerprints; all cfg optional: policy/signing_keys|fingerprints/labels/top_nav; button_list default ['OK']); result is button_selected or back."},
    {"seed_address_verification_screen", py_seed_address_verification_screen, METH_VARARGS, "Build the verify-address scan screen (address + live progress; cfg requires 'address' and 'type_network', optional network/progress_text; buttons default ['Skip 10','Cancel']); result is button_selected or back."},
    {"seed_address_verification_set_progress", py_seed_address_verification_set_progress, METH_VARARGS, "Push the live 'Checking address N' progress line (host-localized str) into the running verify-address screen; no result."},
    {"seed_address_verification_success_screen", py_seed_address_verification_success_screen, METH_VARARGS, "Build the address-verified success screen (SUCCESS hero + green headline over abbreviated address + type line + 'index N'; no back; cfg requires 'status_headline','address','address_type_text','index_text', non-empty 'button_list', and 'top_nav.title'); result is button_selected."},
    {"seed_sign_message_confirm_address_screen", py_seed_sign_message_confirm_address_screen, METH_VARARGS, "Build the sign-message confirm-address screen (derivation path + address; cfg requires 'derivation_path' and 'address'; button default ['Sign message']); result is button_selected or back."},
    {"seed_sign_message_confirm_message_screen", py_seed_sign_message_confirm_message_screen, METH_VARARGS, "Build the sign-message confirm-message screen (message text over a Next button; optional 'message' + standard button_list chrome); result is button_selected or back."},
    {"settings_qr_confirmation_screen", py_settings_qr_confirmation_screen, METH_VARARGS, "Build the settings-QR import confirmation (optional config_name/status_message; button default ['Home']); result is button_selected or back."},
    {"settings_locale_picker_screen", py_settings_locale_picker_screen, METH_VARARGS, "Build the language-selection picker (rows carry live-text or pre-rendered endonym images; result is button_selected(index)). cfg may set 'font_dir' (default 'lang-packs')."},
    {"tools_address_explorer_address_type_screen", py_tools_address_explorer_address_type_screen, METH_VARARGS, "Build the address-explorer address-type chooser ([Receive, Change] under a context header; cfg requires 'top_nav.title' + non-empty 'button_list'; optional header is EITHER fingerprint+fingerprint_label+derivation_text+derivation_label OR wallet_descriptor_text+wallet_descriptor_label); result is button_selected or back."},
    {"tools_address_explorer_address_list_screen", py_tools_address_explorer_address_list_screen, METH_VARARGS, "Build the address-explorer address list (scrolling addresses[] + 'Next N' paginate; optional start_index/initial_selected_index/next_label; title default 'Receive Addrs'); result is button_selected(index) or back."},
    {"tools_calc_final_word_screen", py_tools_calc_final_word_screen, METH_VARARGS, "Build the calc-final-word entry screen (input + computed word + checksum-bit breakdown; all fields optional; button default ['Next']); result is button_selected or back."},
    {"tools_calc_final_word_done_screen", py_tools_calc_final_word_done_screen, METH_VARARGS, "Build the calc-final-word result (final word + fingerprint readout; cfg requires 'final_word' and 'fingerprint', optional fingerprint_label/mnemonic_word_length); result is button_selected or back."},
    {"reset_screen", py_reset_screen, METH_VARARGS, "Build the 'Restarting' status screen (centered wrapped message; cfg all-optional: text/top_nav); no back/power affordance."},
    {"power_off_not_required_screen", py_power_off_not_required_screen, METH_VARARGS, "Build the 'Just Unplug It' advisory (centered wrapped message; cfg all-optional: text/top_nav; back shown); result is back."},
    {"power_options_screen", py_power_options_screen, METH_VARARGS, "Build the Reset/Power menu (large-icon tile grid; cfg requires 'top_nav.title' + a 'button_list' of exactly 2 or 4 label+icon items, e.g. Restart/Power off); result is button_selected(index) or back."},
    {"donate_screen", py_donate_screen, METH_VARARGS, "Build the Donate screen (body text + url default 'seedsigner.com'; cfg all-optional: text/url/top_nav; back shown); result is back."},
    {"version_screen", py_version_screen, METH_VARARGS, "Build the Settings>Version screen (version name hard-wrapped to width + optional fork/commit rows + bottom-pinned timestamp; cfg requires top_nav.title/version_name/version_timestamp, optional version_fork/short_commit_hash); result is back."},
    {"io_test_screen", py_io_test_screen, METH_VARARGS, "Build the hardware I/O self-test (capture pictogram + KEY1 camera / KEY2 clear / KEY3 exit labels; cfg requires localized top_nav.title/capturing_text/clear_label/exit_label, optional camera_glyph). Hardware-key driven: KEY1/KEY2/KEY3 emit ('aux_key', 0, 'KEY1'|'KEY2'|'KEY3') results; reflect the async camera grab back with io_test_set_capture_state."},
    {"io_test_set_capture_state", py_io_test_set_capture_state, METH_VARARGS, "io_test_set_capture_state(state): reflect the async camera grab in the live io_test_screen - 0 idle / 1 capturing (band shown) / 2 captured (KEY2 label = Clear). No-op when no io_test_screen is active."},
    {"io_test_get_camera_plane_dims", py_io_test_get_camera_plane_dims, METH_NOARGS, "io_test_get_camera_plane_dims(): side (px) of the centered square camera plane behind the io_test chrome (width==height); 0 when no io_test_screen is active. Center-crop the still to side x side before io_test_blit_camera."},
    {"io_test_blit_camera", py_io_test_blit_camera, METH_VARARGS, "io_test_blit_camera(frame): blit a host-captured RGB565 still (exactly side*side*2 bytes) into the io_test camera plane and reveal it. Size mismatch / no active screen = silent no-op. Cleared by io_test_set_capture_state(0)."},
    {"screensaver_screen", py_screensaver_screen, METH_NOARGS, "Build the screensaver (bouncing logo); returns immediately. Manual-test helper (the overlay manager owns the screensaver at runtime)."},

    // --- Live camera-preview scan surface (camera_preview.cpp) ----------------
    // The Pi owns the pixel plane: a full-screen RGB565 lv_image the host pushes
    // camera frames into, with the portable camera_preview_overlay chrome on top.
    // Drive loop: capture -> set_frame(rgb565 bytes) -> lvgl_pump; decode stays in
    // Python and advances the overlay via set_progress (a few/sec, never per frame).
    {"camera_preview_screen", py_camera_preview_screen, METH_VARARGS, "Build the live camera-preview scan screen (full-screen RGB565 image sink + overlay chrome). Optional cfg {'instructions_text': str} sets the hardware/joystick bottom line. Host then pushes frames with camera_preview_set_frame and progress with camera_preview_set_progress; end with camera_preview_close."},
    {"camera_preview_set_frame", py_camera_preview_set_frame, METH_VARARGS, "Push one LVGL-native RGB565 frame (bytes, exactly width*height*2) into the live camera-preview sink. NEVER pre-swap for the panel — the flush driver owns byte order. No-op when no preview is active."},
    {"camera_preview_set_frame_yuv420", py_camera_preview_set_frame_yuv420, METH_VARARGS, "camera_preview_set_frame_yuv420(buf, src_w, src_h, y_stride, uv_stride, rotate): convert a planar I420/YUV420 frame (Y,U,V contiguous; U/V half-res) to RGB565 into the sink, rotated 0/90/180/270 deg. Rotated src dims must equal the sink dims (no scaling). Phase-0 libcamera path (replaces the numpy convert). No-op when no preview is active."},
    {"camera_preview_set_progress", py_camera_preview_set_progress, METH_VARARGS, "camera_preview_set_progress(percent, frame_status): advance the overlay status bar (0..100) + status dot (0 none/1 added/2 repeated/3 miss). Implies scanning. A few times/sec, never per frame."},
    {"camera_preview_set_scanning", py_camera_preview_set_scanning, METH_VARARGS, "camera_preview_set_scanning(active): toggle between the back-affordance state (instruction text) and the scanning status-bar state."},
    {"camera_preview_close", py_camera_preview_close, METH_NOARGS, "End the camera-preview session: free the overlay handle + sink buffer. Call before loading the next screen. Idempotent."},
    {"io_test_camera_start", py_io_test_camera_start, METH_NOARGS, "io_test_camera_start(): begin feeding the active io_test_screen's camera plane from the native engine (KEY1 grab). The app pumps its capturing hold, then calls io_test_camera_stop() to freeze the last frame. Raises OSError(code,msg) on camera bring-up failure; RuntimeError if no io_test_screen is active."},
    {"io_test_camera_stop", py_io_test_camera_stop, METH_NOARGS, "io_test_camera_stop(): stop the engine (last frame stays frozen in the io_test plane) + clear the grab redirect. Idempotent."},
    {"io_test_camera_frame_ready", py_io_test_camera_frame_ready, METH_NOARGS, "io_test_camera_frame_ready(): True once the background pump has stashed at least one camera frame during the active io_test grab. Lets the host wait for a confirmed first frame (bounded) before io_test_camera_stop() freezes the plane."},

    // --- Native toast overlay (toast.cpp) -------------------------------------
    // A transient bottom banner built on the display's top layer: composites over the
    // live screen, steals no input, and is dismissed natively (auto after duration_ms or
    // by any hardware key/joystick press). No result is emitted; the host does not poll.
    // Policy-free: the app resolves severity -> glyph+colors and passes them.
    {"show_toast", py_show_toast, METH_VARARGS, "show_toast(cfg): raise/replace a toast over the live screen. cfg: 'label_text' (str, required), 'icon' (str glyph or None), 'outline_color' (int 0xRRGGBB, default 0xFFFFFF), 'font_color' (int 0xRRGGBB, default 0xFFFFFF), 'duration_ms' (int, default 3000; 0 = until dismissed/replaced). Thread-safe (deferred to the LVGL loop)."},
    {"dismiss_toast", py_dismiss_toast, METH_NOARGS, "dismiss_toast(): dismiss the current toast immediately (no-op if none). LVGL-thread only — call from the pump thread; for a cross-thread dismiss let duration_ms expire."},

    // --- Result queue (result_queue.cpp) --------------------------------------
    {"poll_for_result", py_poll_for_result, METH_NOARGS, "Poll next result tuple or None."},
    {"clear_result_queue", py_clear_result_queue, METH_NOARGS, "Clear result queue."},

    // --- Debug helpers ---------------------------------------------------------
    {"_debug_last_path", py_debug_last_path, METH_NOARGS, "Debug helper for bridge path."},
    {"_debug_emit_result", py_debug_emit_result, METH_VARARGS, "Debug helper to inject callback-like events."},
    {"_debug_emit_qr_density", py_debug_emit_qr_density, METH_VARARGS, "Debug helper to fire the on_qr_density callback (px_per_module) into the result queue."},
    {"_debug_emit_aux_key", py_debug_emit_aux_key, METH_VARARGS, "Debug helper to fire the on_aux_key callback (key_name) into the result queue."},
    {NULL, NULL, 0, NULL},
};

static struct PyModuleDef module_def = {
    PyModuleDef_HEAD_INIT,
    "seedsigner_lvgl_screens",
    "SeedSigner LVGL native screen bindings for the Raspberry Pi Zero.",
    -1,
    methods,
};

PyMODINIT_FUNC PyInit_seedsigner_lvgl_screens(void) {
    PyObject *m = PyModule_Create(&module_def);
    if (!m) {
        return NULL;
    }
    // RASPI-5 Phase 2: the background LVGL pump thread (spawned by lvgl_init) is a joinable
    // std::thread; if the app exits without calling lvgl_shutdown() it would std::terminate
    // at static destruction. Join it at interpreter finalization as a safety net (idempotent
    // with an explicit lvgl_shutdown()).
    Py_AtExit(lvgl_runtime_join_pump_thread);
#ifdef SS_CAMERA_ENGINE
    // Attach the nested `camera_scanner` (QR) + `camera_entropy` (image-entropy) submodules
    // (native libcamera capture engines; ESP camera_scanner / camera_entropy contracts).
    // Only present in the CAMERA_ENGINE build.
    if (camera_scanner_attach(m) < 0) {
        Py_DECREF(m);
        return NULL;
    }
    if (camera_entropy_attach(m) < 0) {
        Py_DECREF(m);
        return NULL;
    }
#endif
    return m;
}
