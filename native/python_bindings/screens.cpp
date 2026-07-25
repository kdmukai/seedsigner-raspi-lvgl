// Python wrappers for the portable LVGL screens (components/seedsigner/ in the
// screens submodule). Every screen is a PURE BUILDER: it constructs the LVGL
// widget tree and returns immediately; the Python host drives the event loop
// with lvgl_pump() and drains results with poll_for_result(). Navigation/focus
// wiring is owned by the C screens themselves (nav_bind in the scaffold) — no
// binding-layer group is attached here.
//
// Adding a screen (all four touch points, in order):
//   1. The C entry point lands in the submodule: declared in seedsigner.h,
//      defined in components/seedsigner/screens/<name>.cpp (picked up by the
//      setup.py glob — no build change needed).
//   2. Add a SCREEN_BINDING(...) line below, with a contract comment stating
//      the cfg keys (required vs optional) and the results it emits.
//   3. Declare py_<name> in module_internal.h and add its methods[] row (with
//      docstring) in module.cpp.
//   4. Add the screen's minimal cfg to SCREEN_MIN_CFGS in
//      tests/test_native_smoke.py so the build's pytest gate exercises it.
//
// Two cfg policies cover every screen:
//   SCREEN_BINDING(name)              — cfg dict REQUIRED (may still be lenient
//                                       about which keys are present).
//   SCREEN_BINDING_OPTIONAL_CFG(name) — cfg dict optional; None/absent passes
//                                       NULL and the C side applies its
//                                       English defaults (RFC 7396 merge-patch).
// Screens needing extra Python-side work (strict validation, providers, no
// args) are hand-written at the bottom.
#include "module_internal.h"

#include "seedsigner.h"
#include "locale_picker.h"  // locale_picker_set_image_provider (endonym-image rows)

#include <stdexcept>
#include <string>

// Debug breadcrumb reported by _debug_last_path(): "none" until the first
// screen build succeeds, "compiled" afterwards.
static const char *s_last_path = "none";

PyObject *py_debug_last_path(PyObject *self, PyObject *args) {
    (void)self;
    (void)args;
    return PyUnicode_FromString(s_last_path);
}

// Mark a successful build for peer subsystems that don't live in this file (e.g.
// camera_preview.cpp) so the shared _debug_last_path() test gate covers them too.
void mark_last_path_compiled() {
    s_last_path = "compiled";
}

// Strict cfg validation for button_list_screen only. The other screens are
// lenient by design: they forward the cfg JSON to the shared C parser, which
// applies defaults and raises on genuinely missing required fields.
static void validate_button_list_cfg(PyObject *cfg) {
    if (!PyDict_Check(cfg)) {
        throw std::runtime_error("button_list_screen expects cfg_dict as dict");
    }

    PyObject *top_nav = PyDict_GetItemString(cfg, "top_nav");
    if (!top_nav || !PyDict_Check(top_nav)) {
        throw std::runtime_error("top_nav object is required");
    }

    PyObject *title = PyDict_GetItemString(top_nav, "title");
    if (!title || !PyUnicode_Check(title)) {
        throw std::runtime_error("top_nav.title is required and must be a string");
    }

    PyObject *button_list = PyDict_GetItemString(cfg, "button_list");
    if (!button_list || !PyList_Check(button_list)) {
        throw std::runtime_error("button_list is required and must be an array/list");
    }

    Py_ssize_t n = PyList_Size(button_list);
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject *it = PyList_GetItem(button_list, i);  // borrowed
        if (!it) {
            throw std::runtime_error("button_list contains invalid item");
        }
        if (PyUnicode_Check(it)) {
            continue;
        }
        if (PyList_Check(it) || PyTuple_Check(it)) {
            if (PySequence_Size(it) <= 0) {
                throw std::runtime_error("button_list entries as array/tuple must not be empty");
            }
            PyObject *label0 = PySequence_GetItem(it, 0);
            if (!label0) {
                throw std::runtime_error("button_list array/tuple label missing");
            }
            bool ok = PyUnicode_Check(label0);
            Py_DECREF(label0);
            if (!ok) {
                throw std::runtime_error("button_list array/tuple index 0 must be string");
            }
            continue;
        }
        // Object form: { "label": str, "icon"?: str, "icon_color"?: str,
        // "right_icon"?: str, "label_color"?: str, ... }. The native
        // read_button_list_items() parser (lvgl-screens) accepts this shape for
        // per-button icons / colors / checkbox state; only the structural
        // requirement (a string "label") is enforced here — the optional keys
        // ride through json.dumps to the shared parser.
        if (PyDict_Check(it)) {
            PyObject *label = PyDict_GetItemString(it, "label");
            if (!label || !PyUnicode_Check(label)) {
                throw std::runtime_error("button_list object entries require a string \"label\"");
            }
            continue;
        }
        throw std::runtime_error("button_list entries must be string, array/tuple with string label at index 0, or object with string \"label\"");
    }

    // Optional per-screen screensaver policy (view-owned). When present it must be
    // a bool; it rides in the cfg JSON to the shared parser, which stamps the
    // screen's root object so the overlay dispatcher honors it. Absent = default
    // (screensaver allowed), applied by the shared parser.
    PyObject *allow_screensaver = PyDict_GetItemString(cfg, "allow_screensaver");
    if (allow_screensaver && !PyBool_Check(allow_screensaver)) {
        throw std::runtime_error("allow_screensaver must be a bool");
    }
}

static std::string py_cfg_to_json(PyObject *cfg) {
    PyObject *json_mod = PyImport_ImportModule("json");
    if (!json_mod) {
        throw std::runtime_error("failed to import json module");
    }

    PyObject *dumps = PyObject_GetAttrString(json_mod, "dumps");
    Py_DECREF(json_mod);
    if (!dumps) {
        throw std::runtime_error("failed to get json.dumps");
    }

    PyObject *args = PyTuple_Pack(1, cfg);
    PyObject *kwargs = PyDict_New();
    PyObject *seps = Py_BuildValue("(ss)", ",", ":");
    if (!args || !kwargs || !seps) {
        Py_XDECREF(seps);
        Py_XDECREF(kwargs);
        Py_XDECREF(args);
        Py_DECREF(dumps);
        throw std::runtime_error("failed to build json.dumps arguments");
    }
    PyDict_SetItemString(kwargs, "separators", seps);
    Py_DECREF(seps);

    PyObject *json_str = PyObject_Call(dumps, args, kwargs);
    Py_DECREF(dumps);
    Py_DECREF(args);
    Py_DECREF(kwargs);

    if (!json_str) {
        throw std::runtime_error("json.dumps failed");
    }

    const char *utf8 = PyUnicode_AsUTF8(json_str);
    if (!utf8) {
        Py_DECREF(json_str);
        throw std::runtime_error("failed to encode cfg json");
    }

    std::string out(utf8);
    Py_DECREF(json_str);
    return out;
}

typedef void (*screen_entry_fn)(void *ctx_json);

// Shared body for screens whose cfg dict is REQUIRED.
static PyObject *build_screen_required_cfg(PyObject *args, const char *name, screen_entry_fn fn) {
    PyObject *cfg = NULL;
    if (!PyArg_ParseTuple(args, "O", &cfg)) {
        return NULL;
    }
    if (!PyDict_Check(cfg)) {
        PyErr_Format(PyExc_RuntimeError, "%s expects cfg_dict as dict", name);
        return NULL;
    }

    try {
        require_lvgl_runtime();
        LvglLockGuard _lvgl_guard;
        std::string cfg_json = py_cfg_to_json(cfg);
        fn((void *)cfg_json.c_str());
        s_last_path = "compiled";
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }

    Py_RETURN_NONE;
}

// Shared body for screens whose cfg dict is OPTIONAL: absent/None passes NULL
// and the C side applies its defaults.
static PyObject *build_screen_optional_cfg(PyObject *args, const char *name, screen_entry_fn fn) {
    PyObject *cfg = NULL;
    if (!PyArg_ParseTuple(args, "|O", &cfg)) {
        return NULL;
    }
    if (cfg && cfg != Py_None && !PyDict_Check(cfg)) {
        PyErr_Format(PyExc_RuntimeError, "%s expects cfg_dict as dict", name);
        return NULL;
    }

    try {
        require_lvgl_runtime();
        LvglLockGuard _lvgl_guard;
        if (cfg && cfg != Py_None) {
            std::string cfg_json = py_cfg_to_json(cfg);
            fn((void *)cfg_json.c_str());
        } else {
            fn(NULL);
        }
        s_last_path = "compiled";
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }

    Py_RETURN_NONE;
}

#define SCREEN_BINDING(NAME) \
    PyObject *py_##NAME(PyObject *self, PyObject *args) { \
        (void)self; \
        return build_screen_required_cfg(args, #NAME, NAME); \
    }

#define SCREEN_BINDING_OPTIONAL_CFG(NAME) \
    PyObject *py_##NAME(PyObject *self, PyObject *args) { \
        (void)self; \
        return build_screen_optional_cfg(args, #NAME, NAME); \
    }

// --- Core screens -----------------------------------------------------------

// main_menu_screen: the 2x2 home grid. Optional cfg localizes the title + 4
// button labels (top_nav + button_list); with no cfg the C side uses its
// English defaults.
SCREEN_BINDING_OPTIONAL_CFG(main_menu_screen)

// large_icon_status_screen: a hero icon + headline + body over an optional bottom
// button list. cfg["status_type"] selects the preset icon+color ("success", "warning",
// "dire_warning", "error") OR "custom" — where the CALLER supplies the hero glyph and
// color via cfg["icon"] (a raw glyph string, same convention as button/top-nav icons)
// and cfg["icon_color"] (hex). The custom mode is what powers PSBTFinalize's SIGN prompt
// and the microSD notification without a bespoke entry point. On the Pi the hero glyph
// renders in the baked 48px seedsigner icon font (PUA 0xE900-0xE923), which includes
// SIGN (0xe921) and MICROSD (0xe91f); a glyph outside that range renders as tofu (see
// docs/knowledge/custom-large-icon-glyph-range.md).
SCREEN_BINDING(large_icon_status_screen)

// keyboard_screen: generalized keyboard entry. One native function backs several
// SeedSigner keyboard flows via cfg: BIP-85 child index, custom derivation path,
// dice-roll and coin-flip entropy (see the keys/keys_to_values/return_after_n_chars/
// title_keystroke_template contract in the screens repo). The completed string is
// delivered as a text_entered result; a top-nav back emits button_selected(1000).
SCREEN_BINDING(keyboard_screen)

// seed_add_passphrase_screen: BIP39 passphrase entry. Optional JSON ctx (top_nav,
// initial_text, max_length, input mode override); with no cfg the C side applies
// its defaults ("Enter Passphrase"). Result is text_entered or button_selected(1000).
SCREEN_BINDING_OPTIONAL_CFG(seed_add_passphrase_screen)

// --- Seed flow ----------------------------------------------------------------

// seed_mnemonic_entry_screen: BIP39 seed-word entry (autocomplete keyboard over a
// wordlist). cfg requires a "wordlist" (array of candidate words); optional
// "initial_letters" / "initial_selected_word" prefill state. The accepted word is
// delivered as a text_entered result; a top-nav back emits button_selected(1000).
SCREEN_BINDING(seed_mnemonic_entry_screen)

// seed_finalize_screen: shown after a seed loads — a fingerprint readout above a
// bottom-pinned button list (Done / BIP-39 Passphrase, no back button). cfg requires
// a "fingerprint" string; optional "fingerprint_label", "top_nav", and "button_list"
// (defaults to ["Done"]). Buttons emit button_selected (Done=0, ...); an optional
// power button emits button_selected(1001).
SCREEN_BINDING(seed_finalize_screen)

// seed_export_xpub_details_screen: the xpub-export summary — fingerprint, derivation
// path, and the (host-truncated) xpub as IconTextLines under a pulsing yellow
// privacy-warning edge, above a bottom button list. cfg requires "fingerprint" and
// "xpub" strings; optional "derivation_path" (default m/84'/0'/0'), the three field
// labels (fingerprint_label/derivation_label/xpub_label), "top_nav", and "button_list"
// (defaults ["Export xpub"]). Buttons emit button_selected; back emits button_selected(1000).
SCREEN_BINDING(seed_export_xpub_details_screen)

// seed_review_passphrase_screen: the entered BIP-39 passphrase (orange, fixed-width,
// centered, up to 3 lines) above a fingerprint IconTextLine spelling out how it changes
// the seed's fingerprint (without >> with). cfg requires a "passphrase" string; optional
// "fingerprint_without"/"fingerprint_with", "changes_fingerprint_label", "top_nav", and
// "button_list" (defaults ["Done"]). Buttons emit button_selected; back emits button_selected(1000).
SCREEN_BINDING(seed_review_passphrase_screen)

// seed_words_screen: one host-paginated page of a seed's words, each numbered in a
// rounded chip, under a pulsing orange dire-warning edge (the seed is on screen), above
// a bottom button list. cfg requires a non-empty "words" array; optional "page_index"
// (default 0), "num_pages" (default 1) for the default "Seed Words: n/N" title,
// "start_number" (1-based number of this page's first word), "top_nav", and "button_list"
// (defaults ["Done"]). Buttons emit button_selected; back emits button_selected(1000).
SCREEN_BINDING(seed_words_screen)

// --- QR display / SeedQR transcription ---------------------------------------

// seed_transcribe_whole_qr_screen: the "whole QR" overview step of SeedQR
// transcription — the full QR rendered small under a title, the precursor to
// seed_transcribe_zoomed_qr_screen. cfg requires a non-empty "qr_data" string;
// optional "qr_mode" (numeric|alphanumeric|byte|auto, default auto),
// "data_encoding" (utf8|hex, default utf8), "border" (default 1), "top_nav.title"
// (default "Transcribe SeedQR"), "button_list". Needs LV_USE_QRCODE (enabled).
SCREEN_BINDING(seed_transcribe_whole_qr_screen)

// seed_transcribe_seedqr_format_screen: the "which SeedQR format?" chooser in the
// hand-transcription flow — a bottom-pinned [Standard, Compact] button list under two
// left-aligned caption/value rows explaining each format (the paired rows are why the
// generic button_list_screen's single intro block won't do). cfg requires "top_nav.title",
// a non-empty "button_list" (the per-seed-length "Standard: NxN" / "Compact: MxM"
// choices), and the four localized row strings "standard_label"/"standard_text"/
// "compact_label"/"compact_text". Result button_selected (back = index 1000).
SCREEN_BINDING(seed_transcribe_seedqr_format_screen)

// seed_transcribe_zoomed_qr_screen: a full-bleed, pannable zoomed view of a SeedQR /
// CompactSeedQR for hand transcription — one highlighted zone window over the dimmed QR
// field, the encoded pattern mask-matched to the Pi's python-qrcode so a hand copy is
// pixel-identical. cfg requires a non-empty "qr_data" string; optional "qr_mode"
// (numeric|alphanumeric|byte|auto, default numeric), "data_encoding" (utf8|hex|base64,
// default utf8), "exit_text" (pre-translated bottom hint), and "initial_zone_x"/
// "initial_zone_y" (default 0). Purely static — no host frame push. Exit (joystick click /
// any non-arrow key / close X) emits button_selected(1000).
SCREEN_BINDING(seed_transcribe_zoomed_qr_screen)

// qr_display_screen: native QR display (static and host-driven animated). cfg requires
// "qr_data" plus qr_mode/data_encoding/border/brightness options (see the screens repo).
// For a STATIC QR the host builds once and does nothing further. For an ANIMATED QR the
// host pushes each frame with qr_display_set_frame() and honors
// qr_display_is_tip_active() (hold while true). On exit the screen emits a
// qr_brightness result (final brightness) followed by button_selected(1000).
SCREEN_BINDING(qr_display_screen)

// --- Splash / loading ---------------------------------------------------------

// opening_splash_screen (parity with Python OpeningSplashScreen). Optional cfg dict
// localizes version/sponsor text and toggles partner logos / boot-logo handoff. The
// timed logo reveal is driven by lv_anim + lv_timer; on completion (or dismissal) it
// emits a button_selected(-1, "splash_complete") result the host loop watches for.
SCREEN_BINDING_OPTIONAL_CFG(opening_splash_screen)

// loading_spinner_screen (LVGL port of Python's LoadingScreenThread), shown while the
// host runs a long blocking task (seed gen, PSBT signing). Optional cfg: {"text": "..."}.
// Fire-and-forget: it takes no input and returns no result — the comet self-animates on
// an lv_timer and is torn down when the next screen loads. There is deliberately no
// stop/hide call (matching the ESP32 binding).
//
// Pi Zero caveat (blended display): unlike the ESP32 — where a dedicated display task
// keeps ticking lv_timer_handler so the comet spins with zero host involvement — the Pi
// has no background pump; LVGL only advances when the host calls lvgl_pump. If the host
// builds this and then blocks in its task, nothing pumps and the comet freezes. So on
// CPython/Pi the host must keep pumping (a background pump thread) for the duration, or
// stay on the PIL LoadingScreenThread until the display cutover. See the host-side note
// seedsigner/docs/architecture/loading-screen-native-integration.md.
SCREEN_BINDING_OPTIONAL_CFG(loading_spinner_screen)

// --- PSBT transaction review ----------------------------------------------
// The native screens SeedSigner's PSBT-review flow walks through, each emitting
// button_selected (back = index 1000) like the other button-list screens. They follow
// the host-formats / C-renders split: the host owns all i18n + number/address
// formatting (btc_amount strings, digit grouping, address derivation) and passes
// the finished pieces; these screens only lay them out, so the Pi and ESP32 can
// never disagree on how a value rounds or an address truncates.

// psbt_overview_screen: the animated transaction pictogram (inputs -> center bar ->
// destinations) under a BtcAmount headline, over a bottom-pinned action button. cfg
// describes the structure (num_inputs, destination_addresses, num_change_outputs,
// has_op_return, ...) plus optional btc_amount + translated labels; defaults title
// and the single "Review details" button.
SCREEN_BINDING(psbt_overview_screen)

// psbt_address_details_screen: one recipient's amount over its full (wrapped) address,
// centered above the action button. cfg requires an "address" string; optional
// btc_amount, top_nav, and button_list (defaults ["Next"]).
SCREEN_BINDING(psbt_address_details_screen)

// psbt_change_details_screen: the change / self-receive output — amount, an
// address-type label ("change address #N"), the single-line address, and an optional
// "Address verified!" line. cfg requires an "address" string; optional
// address_type_label, is_verified, verified_text, btc_amount, button_list.
SCREEN_BINDING(psbt_change_details_screen)

// psbt_math_screen: the fee "math" — a right-aligned fixed-width equation of the input
// total minus recipients minus fee, ruled off, equalling the change. The host passes
// each amount as an already-formatted (unpadded) number string; cfg carries
// amounts{input,spend,fee,change}, denomination ("btc"|"sats"), num_recipients, and
// translated labels. All fields optional (the native screen fills sensible defaults).
SCREEN_BINDING(psbt_math_screen)

// psbt_op_return_screen: the PSBT OP_RETURN data review — raw "hex" and/or decoded
// "text" under a caption, above a bottom button list. cfg all-optional: "hex", "text",
// "hex_label" (default "raw hex data"), "button_list" (default ["Done"]), "top_nav"
// (title default "OP_RETURN"). Result button_selected (back = index 1000).
SCREEN_BINDING(psbt_op_return_screen)

// --- Remaining SeedSigner flows ---------------------------------------------

// multisig_wallet_descriptor_screen: the "Descriptor Loaded" review — a policy
// line over the participating fingerprints (monospace), above a bottom button
// list. cfg all-optional: "policy" (str), "signing_keys" (str) OR "fingerprints"
// (str array), "policy_label"/"signing_keys_label", "top_nav.title" (default
// "Descriptor Loaded"), "button_list" (default ["OK"]).
SCREEN_BINDING(multisig_wallet_descriptor_screen)

// seed_address_verification_screen: the live "Verify Address" scan — the address
// (type/network-colored) with a progress readout while derivation indexes are
// scanned. cfg requires "address" and "type_network" strings; optional "network"
// (default "mainnet"), "progress_text", "top_nav.title" (default "Verify Address",
// back hidden), "button_list" (default ["Skip 10","Cancel"]).
SCREEN_BINDING(seed_address_verification_screen)

// seed_address_verification_success_screen: the confirmation after the Address
// Explorer's brute-force worker matches the unverified address to an index — the
// SUCCESS variant of large_icon_status_screen (green check hero + green headline) but
// with a bespoke three-line read-out (abbreviated FormattedAddress, address-type line,
// "index N" line) the generic status screen can't express. No back button (forced). cfg
// requires "status_headline", "address", "address_type_text", "index_text" strings and a
// non-empty "button_list" (the "OK" ack), plus "top_nav.title". Result button_selected.
SCREEN_BINDING(seed_address_verification_success_screen)

// seed_sign_message_confirm_address_screen: sign-message step 2 — the deriving
// path over the derived address, above a bottom button list. cfg requires
// "derivation_path" and "address" strings; optional "derivation_path_label"
// (default "derivation path"), "top_nav.title" (default "Confirm Address"),
// "button_list" (default ["Sign message"]).
SCREEN_BINDING(seed_sign_message_confirm_address_screen)

// seed_sign_message_confirm_message_screen: sign-message step 1 — the message
// text to be signed. Re-enters the public button_list_screen and overlays the
// message. cfg optional "message" (str) plus the standard "top_nav" /
// "button_list" (default ["Next"]) / "is_bottom_list" chrome keys.
SCREEN_BINDING(seed_sign_message_confirm_message_screen)

// settings_qr_confirmation_screen: post-SettingsQR-import confirmation — an
// optional status line naming the applied config, above a single action.
// Re-enters button_list_screen with empty intro text. cfg optional "config_name"
// and "status_message" strings; "top_nav.title" (default "Settings QR", back
// hidden), "button_list" (default ["Home"]).
SCREEN_BINDING(settings_qr_confirmation_screen)

// tools_address_explorer_address_type_screen: the Address Explorer's "which
// addresses?" chooser — a bottom-pinned [Receive, Change] button list under a context
// header identifying the source. cfg requires "top_nav.title" and a non-empty
// "button_list"; the header is one of two OPTIONAL shapes (a header-less call is
// tolerated): single-sig seed -> "fingerprint" + "fingerprint_label" +
// "derivation_text" + "derivation_label" (all required together); loaded descriptor ->
// "wallet_descriptor_text" + "wallet_descriptor_label". Result button_selected (back = index 1000).
SCREEN_BINDING(tools_address_explorer_address_type_screen)

// tools_address_explorer_address_list_screen: the Address Explorer list — a
// scrolling column of derived addresses (each a button) plus a "Next N" paginate
// action, in monospace (is_bottom_list=false). cfg optional "addresses" (str
// array), "start_index" (default 0), "initial_selected_index" (default 0),
// "next_label", "top_nav.title" (default "Receive Addrs"; host also passes
// "Change Addrs"). Result button_selected(index) — address row or paginate.
SCREEN_BINDING(tools_address_explorer_address_list_screen)

// tools_calc_final_word_screen: BIP-39 calc-final-word entry — the running input,
// the computed final word, and the checksum-bit breakdown (discarded bits dimmed),
// above a "Next" button. Re-enters button_list_screen and overlays the breakdown.
// cfg optional "your_input_text", "final_word_text", "checksum_label" (default
// "Checksum"), "selected_final_bits", "checksum_bits", "has_selected_word"
// (default true), "top_nav.title" (default "Final Word Calc"), "button_list"
// (default ["Next"]).
SCREEN_BINDING(tools_calc_final_word_screen)

// tools_calc_final_word_done_screen: calc-final-word result — the computed final
// word with a centered master-fingerprint readout, above a bottom button list.
// cfg requires "final_word" and "fingerprint" strings; optional "fingerprint_label"
// (default "fingerprint"), "mnemonic_word_length" (12 -> title "12th Word", else
// "24th Word"), "button_list".
SCREEN_BINDING(tools_calc_final_word_done_screen)

// --- Simple info / final screens ---------------------------------------------

// reset_screen: the "Restarting" status screen — a centered wrapped message
// under a title, no back/power affordance (host tears it down). cfg all-optional:
// "text", "top_nav" (title default "Restarting").
SCREEN_BINDING(reset_screen)

// power_off_not_required_screen: the "Just Unplug It" advisory — one centered
// wrapped message; back button shown. cfg all-optional: "text", "top_nav" (title
// default "Just Unplug It"). Result button_selected(1000).
SCREEN_BINDING(power_off_not_required_screen)

// power_options_screen: the "Reset / Power" menu — Python's LargeButtonScreen with a
// non-home button count (large icon tiles, not a text list), so it needs its own entry
// point (the shared large_button_grid geometry main_menu also uses). cfg requires
// "top_nav.title" and a "button_list" of EXACTLY 2 or 4 items (each a label + icon glyph,
// the same flat shape button_list_screen takes; e.g. "Restart"/"Power off"). Pressing a
// tile emits button_selected(index); back emits button_selected(1000).
SCREEN_BINDING(power_options_screen)

// donate_screen: body text over a URL (default "seedsigner.com") in a scroll
// container; back shown. cfg all-optional: "text", "url", "top_nav" (title
// default "Donate"). Result button_selected(1000).
SCREEN_BINDING(donate_screen)

// version_screen: Settings > Version — informational build readout. cfg (required
// dict): "top_nav.title", "version_name" (the screen hard-wraps it to the panel
// width), and "version_timestamp" (host pre-formatted "%Y-%m-%d %H:%M:%S UTC",
// pinned bottom-center). Optional "version_fork" / "short_commit_hash" (absent or
// "" -> that row hidden; the app nulls both for release images). No body
// focusables; result is back (SEEDSIGNER_RET_BACK_BUTTON).
SCREEN_BINDING(version_screen)

// io_test_screen: the hardware I/O self-test — a capture pictogram with per-key
// labels (KEY1 camera glyph / KEY2 clear / KEY3 exit); no back/power. Driven by
// hardware key events, not a button_list. cfg requires the localized strings
// "top_nav.title", "capturing_text", "clear_label" and "exit_label" (the C screen
// throws if any is missing); optional "camera_glyph" (icon codepoint default).
//
// KEY1/2/3 are forwarded to the host via seedsigner_lvgl_on_aux_key(); this
// extension's strong override (result_queue.cpp) surfaces each as an
// ("aux_key", 0, "KEY1"|"KEY2"|"KEY3") result. The host reflects its async
// single-frame camera grab back via io_test_set_capture_state() (companion below).
SCREEN_BINDING(io_test_screen)

// --- Hand-written screens ------------------------------------------------------

// button_list_screen: the workhorse list screen. The ONE strictly-validated cfg
// (top_nav.title + button_list required; see validate_button_list_cfg). Buttons
// emit button_selected(index, label); back/power emit button_selected with index
// 1000/1001 (RET_CODE__BACK/POWER_BUTTON).
PyObject *py_button_list_screen(PyObject *self, PyObject *args) {
    (void)self;

    PyObject *cfg = NULL;
    if (!PyArg_ParseTuple(args, "O", &cfg)) {
        return NULL;
    }

    try {
        validate_button_list_cfg(cfg);
        require_lvgl_runtime();
        LvglLockGuard _lvgl_guard;

        std::string cfg_json = py_cfg_to_json(cfg);
        button_list_screen((void *)cfg_json.c_str());
        s_last_path = "compiled";
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }

    Py_RETURN_NONE;
}

// settings_locale_picker_screen(cfg) -> None
//
// The language-selection screen. Each row is "English | native"; the native name
// is live text (Latin, baked floor) or a pre-rendered endonym image for non-Latin
// scripts (a row whose cfg carries "image"). Those image bytes are fetched through
// the SAME filesystem pack provider set_locale uses — it serves
// <font_dir>/<locale>/endonym_<h>.bin verbatim. font_dir comes from the optional
// top-level cfg "font_dir" key (default "lang-packs", matching set_locale).
// Selection fires the standard button_selected(row_index) result — the host maps
// the index back to the locale it placed there.
PyObject *py_settings_locale_picker_screen(PyObject *self, PyObject *args) {
    (void)self;

    PyObject *cfg = NULL;
    if (!PyArg_ParseTuple(args, "O", &cfg)) {
        return NULL;
    }
    if (!PyDict_Check(cfg)) {
        PyErr_SetString(PyExc_RuntimeError, "settings_locale_picker_screen expects cfg_dict as dict");
        return NULL;
    }

    try {
        require_lvgl_runtime();
        LvglLockGuard _lvgl_guard;

        // Point the picker's endonym-image provider at the filesystem pack store —
        // the exact seam ss_load_locale uses. A function-local static ctx outlives
        // the build call; the picker parses + copies each image during the build, so
        // the provider bytes need only be valid across this call (like ss_load_locale).
        static FsPackCtx picker_ctx;
        const char *font_dir = "lang-packs";
        PyObject *fd = PyDict_GetItemString(cfg, "font_dir");  // borrowed
        if (fd && PyUnicode_Check(fd)) {
            const char *s = PyUnicode_AsUTF8(fd);
            if (!s) {
                return NULL;  // encoding error already set
            }
            font_dir = s;
        }
        picker_ctx.font_dir = font_dir;
        locale_picker_set_image_provider(fs_pack_provider, &picker_ctx);

        std::string cfg_json = py_cfg_to_json(cfg);
        settings_locale_picker_screen((void *)cfg_json.c_str());
        s_last_path = "compiled";
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }

    Py_RETURN_NONE;
}

// screensaver_screen: bouncing-logo builder, no cfg. The overlay manager owns the
// runtime screensaver (idle-watch); this export remains as a manual-test builder.
PyObject *py_screensaver_screen(PyObject *self, PyObject *args) {
    (void)self;
    (void)args;
    try {
        require_lvgl_runtime();
        LvglLockGuard _lvgl_guard;
        screensaver_screen(NULL);
        s_last_path = "compiled";
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }

    Py_RETURN_NONE;
}

// --- qr_display_screen companions ---------------------------------------------

// Push the next animated-QR frame into the live qr_display_screen. Accepts bytes
// (raw payload, e.g. a CompactSeedQR chunk) or str (UTF-8 encoded, e.g. a BBQr /
// UR fragment). Re-encodes + repaints in place using the screen's qr_mode. Safe
// no-op on the native side when no QR screen is active.
PyObject *py_qr_display_set_frame(PyObject *self, PyObject *args) {
    (void)self;

    PyObject *obj = NULL;
    if (!PyArg_ParseTuple(args, "O", &obj)) {
        return NULL;
    }

    char *data = NULL;
    Py_ssize_t len = 0;
    if (PyBytes_Check(obj)) {
        if (PyBytes_AsStringAndSize(obj, &data, &len) < 0) {
            return NULL;
        }
    } else if (PyUnicode_Check(obj)) {
        data = const_cast<char *>(PyUnicode_AsUTF8AndSize(obj, &len));
        if (!data) {
            return NULL;
        }
    } else {
        PyErr_SetString(PyExc_TypeError, "qr_display_set_frame expects bytes or str");
        return NULL;
    }

    try {
        require_lvgl_runtime();
        LvglLockGuard _lvgl_guard;
        qr_display_set_frame((const void *)data, static_cast<size_t>(len));
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }

    Py_RETURN_NONE;
}

// True while the QR screen's brightness tip/panel is showing. The host's animation
// frame driver polls this and holds (does not advance frames) while true, then
// restarts the sequence from frame 0 when it clears. False when no QR screen active.
PyObject *py_qr_display_is_tip_active(PyObject *self, PyObject *args) {
    (void)self;
    (void)args;
    LvglLockGuard _lvgl_guard;
    if (qr_display_is_tip_active()) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

// --- io_test_screen companion -------------------------------------------------

// io_test_set_capture_state(state: int) -> None
// Reflect the host's async single-frame camera grab in the running io_test_screen:
// 0 IDLE (band hidden, KEY2 label blank), 1 CAPTURING ("Capturing image…" band up),
// 2 CAPTURED (KEY2 label = "Clear"). Same host-push shape as qr_display_set_frame.
// Safe no-op when no io_test_screen is active (the C setter guards on its live ctx).
PyObject *py_io_test_set_capture_state(PyObject *self, PyObject *args) {
    (void)self;

    int state = 0;
    if (!PyArg_ParseTuple(args, "i", &state)) {
        return NULL;
    }
    if (state < IO_TEST_CAPTURE_IDLE || state > IO_TEST_CAPTURE_CAPTURED) {
        PyErr_Format(PyExc_ValueError,
                     "io_test_set_capture_state: state must be 0..2, got %d", state);
        return NULL;
    }

    try {
        require_lvgl_runtime();
        LvglLockGuard _lvgl_guard;
        io_test_set_capture_state((io_test_capture_state_t)state);
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }

    Py_RETURN_NONE;
}

// io_test_get_camera_plane_dims() -> int
// The centered-square camera plane's side (width == height) behind the io_test chrome;
// the host center-crops its still to side x side before io_test_blit_camera(). Returns 0
// when no io_test_screen is active (SCREENS-9 owned-plane contract).
PyObject *py_io_test_get_camera_plane_dims(PyObject *self, PyObject *args) {
    (void)self;
    (void)args;

    int width = 0, height = 0;
    try {
        require_lvgl_runtime();
        LvglLockGuard _lvgl_guard;
        io_test_get_camera_plane_dims(&width, &height);
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }
    // The plane is square (width == height); return the side (0 when inactive).
    return PyLong_FromLong(width);
}

// io_test_blit_camera(frame: bytes) -> None
// Blit a host-captured RGB565 still (exactly side*side*2 bytes, per
// io_test_get_camera_plane_dims) into the io_test camera plane and reveal it. A size
// mismatch or the no-active-screen case is a silent no-op (the screen guards both).
// Cleared by io_test_set_capture_state(IO_TEST_CAPTURE_IDLE). Zero-copy read; mirrors
// camera_preview_set_frame.
PyObject *py_io_test_blit_camera(PyObject *self, PyObject *args) {
    (void)self;

    Py_buffer view;
    if (!PyArg_ParseTuple(args, "y*", &view)) {  // y* == read-only bytes-like, zero-copy
        return NULL;
    }

    try {
        require_lvgl_runtime();
        LvglLockGuard _lvgl_guard;
        io_test_blit_camera(reinterpret_cast<const uint8_t *>(view.buf),
                            static_cast<size_t>(view.len));
    } catch (const std::exception &e) {
        PyBuffer_Release(&view);
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }

    PyBuffer_Release(&view);
    Py_RETURN_NONE;
}

// --- seed_address_verification_screen companion -------------------------------

// Push the live "Checking address N" progress line into the running verify-address
// screen. The host owns the brute-force worker and builds the localized string;
// native just re-labels the live line (holds no strings). Mirrors qr_display_set_frame,
// but the payload is a plain UTF-8 string, not a pixel buffer. Safe no-op on the
// native side when no verify-address screen is active.
PyObject *py_seed_address_verification_set_progress(PyObject *self, PyObject *args) {
    (void)self;

    const char *text = NULL;
    if (!PyArg_ParseTuple(args, "s", &text)) {  // 's' = UTF-8; rejects bytes/None
        return NULL;
    }

    try {
        require_lvgl_runtime();
        LvglLockGuard _lvgl_guard;
        seed_address_verification_set_progress(text);
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }

    Py_RETURN_NONE;
}
