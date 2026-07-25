// camera_preview_sink — the Python-free bridge between the native camera engine
// and the camera_preview.cpp pixel sink (a full-screen RGB565 lv_image + the
// portable overlay chrome). camera_preview.cpp implements these; camera_engine.cpp
// calls them so it never has to include Python.h or touch PyObject.
//
// The thread model differs per function (RASPI-5 Phase 2 — see camera_preview.cpp):
// blit_rgb565 runs on the pump/LVGL thread ONLY (the engine's consume hook fires under the
// LVGL lock), so it memcpies into the sink and invalidates on the LVGL locus — no LVGL
// mutation on an engine worker thread (spec §4.9). session_active / get_sink_dims are
// called on the HOST thread (from the scanner/entropy start paths + camera_engine_start),
// so their implementations take the LVGL lock themselves to serialize against the pump.
#ifndef SS_CAMERA_PREVIEW_SINK_H
#define SS_CAMERA_PREVIEW_SINK_H

#include <cstddef>
#include <cstdint>

// True when a camera_preview session is live (screen + sink buffer + overlay exist).
bool camera_preview_session_active();

// Sink dimensions (== the active LVGL display resolution). Undefined values when no
// session is active; call only after camera_preview_session_active() is true.
void camera_preview_get_sink_dims(int *w, int *h);

// Copy one LVGL-native RGB565 frame (exactly sink w*h*2 bytes) into the sink and
// invalidate the image so the next pump redraws it. Size mismatch or no active
// session is a silent no-op. MUST be called on the pump/LVGL thread.
void camera_preview_blit_rgb565(const uint8_t *rgb565, size_t nbytes);

#endif  // SS_CAMERA_PREVIEW_SINK_H
