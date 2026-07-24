// Language-pack loading and discovery. The shared loader (ss_load_locale) owns
// all the orchestration: clearing the previous locale, registering each role
// font at the right px, and — for complex scripts — loading runs.bin and
// installing the glyph run table. The ONE per-host piece is acquiring the pack
// bytes. On the Pi Zero that's a plain filesystem read of
// <font_dir>/<locale>/<file>; this is the exact reference provider from the
// screenshot generator. On the real signing device this same seam is where
// pack-signature verification will live (see locale_loader.h).
#include "module_internal.h"

#include "gui_constants.h"   // active_profile (endonym image height)
#include "locale_loader.h"   // ss_load_locale / ss_unload_locale / ss_register_pack_manifest

#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include <dirent.h>
#include <sys/stat.h>

bool fs_pack_provider(const char *locale, const char *file,
                      const uint8_t **bytes, size_t *len, void *user) {
    FsPackCtx *ctx = static_cast<FsPackCtx *>(user);
    std::string path = ctx->font_dir + "/" + locale + "/" + file;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        fprintf(stderr, "missing pack file: %s\n", path.c_str());
        return false;
    }
    ctx->scratch.assign((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    *bytes = ctx->scratch.data();
    *len = ctx->scratch.size();
    return true;
}

// set_locale(locale, font_dir="lang-packs") -> bool
//
// Switch the active locale, loading its font packs from <font_dir>/<locale>/.
// Returns True on success. On any missing/unreadable pack the loader restores
// the baked Western floor and this returns False rather than raising: a missing
// pack is a recoverable "fall back to English" condition, not a programming
// error. (Bad argument types still raise TypeError via PyArg_ParseTuple.)
PyObject *py_set_locale(PyObject *self, PyObject *args) {
    (void)self;

    const char *locale = NULL;
    const char *font_dir = "lang-packs";
    if (!PyArg_ParseTuple(args, "s|s", &locale, &font_dir)) {
        return NULL;
    }

    try {
        // Font registration rasterizes via tiny_ttf, which needs a live LVGL
        // runtime (lv_init + an allocator). Guard it the same way screens do.
        require_lvgl_runtime();
        LvglLockGuard _lvgl_guard;

        FsPackCtx ctx;
        ctx.font_dir = font_dir;

        bool ok = ss_load_locale(locale, fs_pack_provider, &ctx);
        return PyBool_FromLong(ok ? 1 : 0);
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }
}

// unload_locale() -> None
//
// Clear everything set_locale installed (fonts, glyph runs, owned buffers) and
// restore the baked Western floor.
PyObject *py_unload_locale(PyObject *self, PyObject *args) {
    (void)self;
    (void)args;
    try {
        require_lvgl_runtime();
        LvglLockGuard _lvgl_guard;
        ss_unload_locale();
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }
    Py_RETURN_NONE;
}

// --- Language-pack discovery (SD / packs partition) -----------------------
// Packs live at <font_dir>/<locale>/ — the exact layout set_locale reads through
// fs_pack_provider. Each pack ships a self-describing manifest.json. On the Pi the
// packs live on a user-writable, cross-platform FAT/exFAT volume, so discovery
// treats every directory entry as hostile input: desktop-OS metadata is skipped,
// a half-copied or malformed pack is silently omitted, and one bad manifest never
// aborts the scan (ss_register_pack_manifest itself fails closed on bad JSON).

// True for directory names that are never language packs — dotfiles (covers the
// macOS junk: .DS_Store, ._* AppleDouble, .Spotlight-V100, .Trashes, .fseventsd —
// and "."/".."), plus the Windows metadata dirs. A real locale code never starts
// with a dot.
static bool is_junk_pack_dir(const char *name) {
    if (!name || name[0] == '\0') return true;
    if (name[0] == '.') return true;
    if (std::strcmp(name, "System Volume Information") == 0) return true;
    if (std::strcmp(name, "$RECYCLE.BIN") == 0) return true;
    if (std::strcmp(name, "found.000") == 0) return true;  // chkdsk recovery
    return false;
}

// Immediate, non-junk subdirectory names of `dir`. Returns false only if `dir`
// itself can't be opened — an absent packs partition means "no packs", not an
// error. d_type is unreliable on FAT/exFAT (often DT_UNKNOWN), so stat() decides.
static bool list_pack_dirs(const std::string &dir, std::vector<std::string> &out) {
    DIR *d = opendir(dir.c_str());
    if (!d) return false;
    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr) {
        if (is_junk_pack_dir(ent->d_name)) continue;
        std::string sub = dir + "/" + ent->d_name;
        struct stat st;
        if (stat(sub.c_str(), &st) != 0) continue;   // vanished mid-scan / unreadable
        if (!S_ISDIR(st.st_mode)) continue;
        out.push_back(ent->d_name);
    }
    closedir(d);
    return true;
}

// Read a whole (small) file into `out`. False on any open failure — a missing or
// half-copied manifest.json is a skip, not a crash.
static bool read_file_string(const std::string &path, std::string &out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return true;
}

// discover_locale_packs(font_dir="lang-packs") -> int
//
// Enumerate <font_dir>/<locale>/manifest.json and register each pack with the
// shared loader so ss_load_locale() / set_locale() work for a locale NOT compiled
// in (the "drop a pack on the card, no rebuild" path). Clears any prior runtime
// registrations first, so it doubles as a rescan on card insert. Returns the count
// registered. Never raises on bad packs (they are skipped); registration is pure
// data, so no live LVGL runtime is required.
PyObject *py_discover_locale_packs(PyObject *self, PyObject *args) {
    (void)self;
    const char *font_dir = "lang-packs";
    if (!PyArg_ParseTuple(args, "|s", &font_dir)) {
        return NULL;
    }

    ss_clear_pack_manifests();

    std::string base = font_dir;
    std::vector<std::string> dirs;
    list_pack_dirs(base, dirs);

    long count = 0;
    for (const std::string &name : dirs) {
        std::string bytes;
        if (!read_file_string(base + "/" + name + "/manifest.json", bytes)) {
            continue;  // no manifest (not a pack) / half-copied
        }
        if (ss_register_pack_manifest(bytes.data(), bytes.size())) {
            count++;
        }
    }
    return PyLong_FromLong(count);
}

static void dict_set_str(PyObject *d, const char *key, const std::string &val) {
    PyObject *v = PyUnicode_FromString(val.c_str());
    PyDict_SetItemString(d, key, v);
    Py_DECREF(v);
}

static void dict_set_str_or_none(PyObject *d, const char *key, const std::string &val) {
    if (val.empty()) {
        PyDict_SetItemString(d, key, Py_None);  // borrows; SetItemString increfs
        return;
    }
    dict_set_str(d, key, val);
}

// list_available_locales(font_dir="lang-packs") -> list[dict]
//
// One dict per pack present under <font_dir>, for the seedsigner app to assemble
// the locale-picker cfg from (unioned with the baked-Latin locales it knows from
// its own .mo catalogs). Each dict:
//   {"code": "<locale>",              # what you pass to set_locale()
//    "endonym": "<native name>"|None, # from the manifest
//    "image": "endonym_<h>.bin"|None, # pre-rendered native-script image for the
//                                     # ACTIVE display height, if the pack ships one
//    "has_image": bool}               # True => render the native name as that image
//                                     # (non-Latin script); False => live text.
// Pure read: it parses manifests but does NOT register them — call
// discover_locale_packs() for that. Malformed/half-copied packs are skipped.
PyObject *py_list_available_locales(PyObject *self, PyObject *args) {
    (void)self;
    const char *font_dir = "lang-packs";
    if (!PyArg_ParseTuple(args, "|s", &font_dir)) {
        return NULL;
    }

    // active_profile() aborts if no display profile is set, and a profile is
    // only installed by lvgl_init()/set_resolution(). Gate on the runtime so a
    // premature call raises a catchable error instead of abort()ing the process.
    try {
        require_lvgl_runtime();
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }

    // Endonym images are pre-rendered per display height; report the one matching
    // the active profile.
    const int height = active_profile().height;

    std::string base = font_dir;
    std::vector<std::string> dirs;
    list_pack_dirs(base, dirs);

    PyObject *result = PyList_New(0);
    if (!result) return NULL;

    for (const std::string &name : dirs) {
        std::string bytes;
        if (!read_file_string(base + "/" + name + "/manifest.json", bytes)) {
            continue;
        }

        nlohmann::json m;
        try {
            m = nlohmann::json::parse(bytes);
        } catch (...) {
            continue;  // malformed manifest -> skip (fail closed)
        }
        if (!m.is_object()) continue;

        const std::string code = m.value("locale", std::string());
        if (code.empty()) continue;  // a pack with no locale is unusable
        const std::string endonym = m.value("endonym", std::string());

        std::string image_file;
        if (height > 0 && m.contains("endonym_images") && m["endonym_images"].is_object()) {
            const nlohmann::json &imgs = m["endonym_images"];
            auto it = imgs.find(std::to_string(height));
            if (it != imgs.end() && it->is_object()) {
                image_file = it->value("file", std::string());
            }
        }

        PyObject *entry = PyDict_New();
        if (!entry) { Py_DECREF(result); return NULL; }
        dict_set_str(entry, "code", code);
        dict_set_str_or_none(entry, "endonym", endonym);
        dict_set_str_or_none(entry, "image", image_file);
        PyObject *has = PyBool_FromLong(image_file.empty() ? 0 : 1);
        PyDict_SetItemString(entry, "has_image", has);
        Py_DECREF(has);

        PyList_Append(result, entry);
        Py_DECREF(entry);
    }
    return result;
}
