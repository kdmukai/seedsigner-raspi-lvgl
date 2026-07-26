// camera_engine — native libcamera capture engine (Phase 1). See camera_engine.h.
//
// Threading (spec §4.9): three actors, two mutexes, manager thread never blocks on
// the pump.
//   - libcamera manager thread (requestCompleted): copy the display YUV420 buffer
//     out of the uncached dmabuf into cached scratch, bump a generation counter,
//     requeue the Request. Copy/flag/requeue-only — no convert, no LVGL, no Python.
//   - blit worker (engine-owned std::thread): wakes on a new generation, snapshots
//     the scratch, converts YUV420 -> RGB565 with rotation into a private work
//     buffer, then publishes it to the ready buffer under out_mtx.
//   - pump thread (camera_engine_pump_consume, LVGL locus): under out_mtx, copies
//     the ready buffer into the camera_preview sink + invalidates.
//
// in_mtx guards {scratch, in_gen} (manager thread <-> blit worker). out_mtx guards
// {ready_buf, dirty} (blit worker <-> pump). The manager thread touches only in_mtx,
// so a busy pump can never stall 3A.
#include "camera_engine.h"
#include "camera_config.h"
#include "camera_error.h"
#include "camera_preview_sink.h"
#include "scan_coordinator.h"

#include <zbar.h>

#include <libcamera/camera.h>
#include <libcamera/camera_manager.h>
#include <libcamera/control_ids.h>
#include <libcamera/controls.h>
#include <libcamera/formats.h>
#include <libcamera/framebuffer_allocator.h>
#include <libcamera/geometry.h>
#include <libcamera/request.h>
#include <libcamera/stream.h>

#include <sys/mman.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace libcamera;
// zbar.h wraps its entire C API in `namespace zbar { extern "C" {...} }` when compiled
// as C++, so the zbar_* names are zbar::-qualified without this.
using namespace zbar;

namespace {

// Decode stream is 480x480 (Phase 2 will consume its Y plane); display stream is
// the sink resolution. Both YUV420, square, sharing one centered-square ScalerCrop
// FOV — exactly the Phase-0 harness geometry.
constexpr int  kDecodeSide  = 480;
constexpr unsigned kBufferCount = 4;

struct PlaneMap {
    void  *mem = nullptr;
    size_t len = 0;
};

struct Engine {
    // libcamera session
    std::unique_ptr<CameraManager>       manager;
    std::shared_ptr<Camera>              camera;
    std::unique_ptr<CameraConfiguration> config;
    std::unique_ptr<FrameBufferAllocator> allocator;
    Stream *decode_stream  = nullptr;
    Stream *display_stream = nullptr;
    std::vector<std::unique_ptr<Request>> requests;
    std::map<int, PlaneMap>              maps;   // plane-0 fd -> whole-buffer mmap

    // display frame geometry
    int disp_w = 0, disp_h = 0;      // == sink dims
    int disp_ystride = 0;            // Y row stride
    int disp_uvstride = 0;           // U/V row stride
    size_t disp_u_off = 0, disp_v_off = 0;  // U/V byte offset within the copied buffer
    size_t disp_total = 0;           // bytes copied per frame (whole YUV420 buffer)
    int rotate = 90;

    // decode frame geometry (Y plane only)
    int    dec_w = 0, dec_h = 0;     // 480x480
    int    dec_ystride = 0;          // Y row stride (>= dec_w, 64B-aligned)
    size_t dec_ylen = 0;            // bytes copied per frame (Y plane length)

    // manager thread <-> blit worker (display)
    std::mutex               in_mtx;
    std::condition_variable  in_cv;
    std::vector<uint8_t>     scratch;          // latest display YUV420, cached
    uint64_t                 in_gen = 0;       // bumped on each new scratch frame

    // manager thread <-> decode worker (decode Y)
    std::mutex               dec_mtx;
    std::condition_variable  dec_cv;
    std::vector<uint8_t>     dec_scratch;      // latest raw decode Y (with stride), cached
    uint64_t                 dec_gen = 0;

    // blit worker <-> pump (display)
    std::mutex               out_mtx;
    std::vector<uint8_t>     ready_buf;        // latest converted RGB565
    bool                     dirty = false;

    std::thread              blit;
    std::thread              decode;
    zbar_image_scanner_t    *zbar = nullptr;   // owned + used only by the decode worker
    std::atomic<bool>        running{false};
};

Engine *g = nullptr;

// RASPI-5 Phase 2: the g pointer's LIFECYCLE lock. camera_engine_pump_consume() now runs
// on the background pump thread (GIL-free), concurrently with start()/stop()/bringup_failed()
// on the host thread — the GIL used to serialize them. Without this, consume could deref g
// while stop() does `delete g` (use-after-free). This leaf mutex is held around ONLY the g
// pointer transitions (publish / delete) and the whole consume body, never across the
// blocking libcamera teardown — so it never stalls the pump. Ordering: consume takes it
// UNDER the LVGL lock (pump), then g->out_mtx — a strict LVGL -> s_engine_mtx -> out_mtx
// order, and nothing acquires the LVGL lock while holding it, so there is no cycle.
std::mutex s_engine_mtx;

// Lifetime counters, namespace-static so they survive g's deletion on stop() (the
// harness reads them after the session ends). Reset at each start().
std::atomic<uint64_t> s_n_in{0};    // frames captured by the manager thread
std::atomic<uint64_t> s_n_conv{0};  // frames converted by the blit worker
std::atomic<uint64_t> s_n_pub{0};   // frames published to the sink by the pump
std::atomic<uint64_t> s_n_dec{0};   // zbar decode passes run by the decode worker
std::atomic<uint64_t> s_n_hit{0};   // decode passes that found >=1 QR symbol

// BT.601 studio-range YUV420 -> RGB565 with 0/90/180/270 rotation. Mirrors
// camera_preview.cpp's set_frame_yuv420 inner loop (Phase-1 keeps the proven
// convert for parity; the §4.9 tiled linear-read/strided-write refinement is a
// follow-up). Reads Y/U/V from explicit byte offsets so a non-contiguous plane
// layout is handled correctly.
void convert_yuv420_rgb565(const uint8_t *buf, int src_w, int src_h,
                           int y_stride, int uv_stride,
                           size_t u_off, size_t v_off, int rotate,
                           uint16_t *dst, int dst_w) {
    const uint8_t *Yp = buf;
    const uint8_t *Up = buf + u_off;
    const uint8_t *Vp = buf + v_off;

    for (int sy = 0; sy < src_h; ++sy) {
        const uint8_t *yrow = Yp + static_cast<size_t>(sy) * y_stride;
        const uint8_t *urow = Up + static_cast<size_t>(sy >> 1) * uv_stride;
        const uint8_t *vrow = Vp + static_cast<size_t>(sy >> 1) * uv_stride;
        for (int sx = 0; sx < src_w; ++sx) {
            const int c = 298 * (static_cast<int>(yrow[sx]) - 16);
            const int u = static_cast<int>(urow[sx >> 1]) - 128;
            const int v = static_cast<int>(vrow[sx >> 1]) - 128;
            int r = (c + 409 * v + 128) >> 8;
            int gg = (c - 100 * u - 208 * v + 128) >> 8;
            int b = (c + 516 * u + 128) >> 8;
            r  = r  < 0 ? 0 : (r  > 255 ? 255 : r);
            gg = gg < 0 ? 0 : (gg > 255 ? 255 : gg);
            b  = b  < 0 ? 0 : (b  > 255 ? 255 : b);
            const uint16_t px = static_cast<uint16_t>(
                ((r & 0xF8) << 8) | ((gg & 0xFC) << 3) | (b >> 3));

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
}

// --- blit worker ------------------------------------------------------------
void blit_worker() {
    // A quarter-turn (90/270) TRANSPOSES the converted frame: the output is disp_h wide x
    // disp_w tall, so the write stride (dst_w) must be the POST-rotation width, not disp_w.
    // On a non-square sink, passing disp_w strides past the buffer end every frame — a heap
    // overflow on this worker thread. The pixel *product* is rotation-invariant, so on a
    // square sink (every Pi Zero, and the SeedSigner+ centered-square preview) this is byte-
    // identical to before. Mirrors the entropy still path. See nonsquare-panel-preview-rotation-todo.md.
    const bool swap_axes = (g->rotate == 90 || g->rotate == 270);
    const int  out_w = swap_axes ? g->disp_h : g->disp_w;
    const int  out_h = swap_axes ? g->disp_w : g->disp_h;

    std::vector<uint8_t> local_in(g->disp_total);
    std::vector<uint8_t> work(static_cast<size_t>(out_w) * out_h * 2);
    uint64_t last_gen = 0;

    while (g->running.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> lk(g->in_mtx);
            g->in_cv.wait(lk, [&] {
                return !g->running.load(std::memory_order_acquire) || g->in_gen != last_gen;
            });
            if (!g->running.load(std::memory_order_acquire)) {
                break;
            }
            last_gen = g->in_gen;
            std::memcpy(local_in.data(), g->scratch.data(), g->disp_total);
        }

        convert_yuv420_rgb565(local_in.data(), g->disp_w, g->disp_h,
                              g->disp_ystride, g->disp_uvstride,
                              g->disp_u_off, g->disp_v_off, g->rotate,
                              reinterpret_cast<uint16_t *>(work.data()), out_w);

        {
            std::lock_guard<std::mutex> lk(g->out_mtx);
            g->ready_buf.swap(work);
            g->dirty = true;
        }
        s_n_conv.fetch_add(1, std::memory_order_relaxed);
        // `work` now holds the previous ready_buf's storage (right size) — reused.
    }
}

// --- decode worker ----------------------------------------------------------
// Copy-on-dispatch (spec §6 Phase 2): snapshot the newest raw decode-Y from
// dec_scratch, stride-strip it into a contiguous grayscale buffer, and run zbar. The
// worker never holds a libcamera Request, so buffer_count stays sized for display
// cadence. Decoded payloads + a per-frame NONE/NEW/REPEAT classification go to the
// scan_coordinator; the app's scan_consumer.run_scan drains them via the binding.
void decode_worker() {
    std::vector<uint8_t> local(g->dec_ylen);                      // raw Y (strided)
    std::vector<uint8_t> gray(static_cast<size_t>(g->dec_w) * g->dec_h);  // contiguous
    uint64_t last_gen = 0;

    while (g->running.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> lk(g->dec_mtx);
            g->dec_cv.wait(lk, [&] {
                return !g->running.load(std::memory_order_acquire) || g->dec_gen != last_gen;
            });
            if (!g->running.load(std::memory_order_acquire)) {
                break;
            }
            last_gen = g->dec_gen;
            std::memcpy(local.data(), g->dec_scratch.data(), g->dec_ylen);
        }

        // Stride-strip (e.g. 512 -> 480). Row-major contiguous runs — cache-friendly,
        // unlike the column-major rotate read (§4.9), so this is cheap.
        for (int y = 0; y < g->dec_h; ++y) {
            std::memcpy(gray.data() + static_cast<size_t>(y) * g->dec_w,
                        local.data() + static_cast<size_t>(y) * g->dec_ystride, g->dec_w);
        }

        zbar_image_t *img = zbar_image_create();
        zbar_image_set_format(img, zbar_fourcc('Y', '8', '0', '0'));
        zbar_image_set_size(img, g->dec_w, g->dec_h);
        zbar_image_set_data(img, gray.data(),
                            static_cast<unsigned long>(g->dec_w) * g->dec_h, nullptr);
        int n = zbar_scan_image(g->zbar, img);
        s_n_dec.fetch_add(1, std::memory_order_relaxed);
        if (n > 0) {
            s_n_hit.fetch_add(1, std::memory_order_relaxed);
            for (const zbar_symbol_t *sym = zbar_image_first_symbol(img);
                 sym; sym = zbar_symbol_next(sym)) {
                const char *data = zbar_symbol_get_data(sym);
                unsigned len = zbar_symbol_get_data_length(sym);
                scan_coord_on_frame(reinterpret_cast<const uint8_t *>(data), len);
            }
        } else {
            scan_coord_on_frame(nullptr, 0);
        }
        zbar_image_destroy(img);
    }
}

// --- completion handler (libcamera manager thread) --------------------------
void on_request_completed(Request *req) {
    // Skip when stopping: running is cleared before camera->stop(), so a request
    // that completes during teardown is neither copied nor requeued (requeuing into
    // a Stopping camera is an error). In-flight requests are cancelled by stop() and
    // delivered here as RequestCancelled.
    if (!g || !g->running.load(std::memory_order_acquire) ||
        req->status() == Request::RequestCancelled) {
        return;
    }
    const Request::BufferMap &bufs = req->buffers();
    auto it = bufs.find(g->display_stream);
    if (it != bufs.end()) {
        FrameBuffer *fb = it->second;
        const FrameBuffer::Plane &p0 = fb->planes()[0];
        auto mit = g->maps.find(p0.fd.get());
        if (mit != g->maps.end()) {
            const uint8_t *src = static_cast<const uint8_t *>(mit->second.mem) + p0.offset;
            {
                std::lock_guard<std::mutex> lk(g->in_mtx);
                std::memcpy(g->scratch.data(), src, g->disp_total);
                g->in_gen++;
            }
            s_n_in.fetch_add(1, std::memory_order_relaxed);
            g->in_cv.notify_one();
        }
    }
    // Decode Y plane: linear memcpy (with stride) into cached scratch, bump the decode
    // generation. Stride-stripping is left to the decode worker (a strided read from
    // the uncached dmabuf here would stall 3A, §4.9).
    auto dit = bufs.find(g->decode_stream);
    if (dit != bufs.end()) {
        const FrameBuffer::Plane &p0 = dit->second->planes()[0];
        auto mit = g->maps.find(p0.fd.get());
        if (mit != g->maps.end()) {
            const uint8_t *src = static_cast<const uint8_t *>(mit->second.mem) + p0.offset;
            {
                std::lock_guard<std::mutex> lk(g->dec_mtx);
                std::memcpy(g->dec_scratch.data(), src, g->dec_ylen);
                g->dec_gen++;
            }
            g->dec_cv.notify_one();
        }
    }
    // Re-check running immediately before requeuing: stop() clears it before
    // camera->stop(), so this narrows the "requeue into a Stopping camera" race to a
    // few instructions. A mutex shared with stop() would deadlock (camera->stop()
    // joins this manager thread), so this is the deadlock-free best effort; a residual
    // benign "state Running" log from libcamera during teardown is possible.
    if (!g->running.load(std::memory_order_acquire)) {
        return;
    }
    // Requeue (copy/flag/requeue-only): reuse the same buffers.
    req->reuse(Request::ReuseBuffers);
    g->camera->queueRequest(req);
}

int bringup_failed(int code) {
    // Best-effort teardown of a partially-constructed engine, then surface the code. Any
    // worker threads are already joined by the caller (the only bringup_failed after
    // thread spawn is the camera->start failure path), so ~Engine won't terminate().
    if (g) {
        if (g->camera) {
            g->camera->stop();
            g->camera->release();
        }
        if (g->zbar) {
            zbar_image_scanner_destroy(g->zbar);
            g->zbar = nullptr;
        }
        for (auto &kv : g->maps) {
            munmap(kv.second.mem, kv.second.len);
        }
        g->camera.reset();
        if (g->manager) {
            g->manager->stop();
        }
        // Publish g == nullptr under the lifecycle lock so a concurrent pump-thread consume
        // never derefs a freed engine (the blocking teardown above ran lock-free).
        std::lock_guard<std::mutex> lk(s_engine_mtx);
        delete g;
        g = nullptr;
    }
    return code;
}

}  // namespace

int camera_engine_start() {
    if (g) {
        return CAMERA_ERR_ALREADY_RUNNING;
    }
    if (!camera_preview_session_active()) {
        return CAMERA_ERR_NO_SINK;
    }

    // Publish the new engine under the lifecycle lock so the pump-thread consume observes a
    // fully-constructed g (never a torn pointer). g is written exactly once here; the rest
    // of bring-up only reads it and mutates members disjoint from consume's (out_mtx/dirty/
    // ready_buf), and the blit worker that fills those isn't spawned until below.
    {
        std::lock_guard<std::mutex> lk(s_engine_mtx);
        g = new Engine();
    }
    // Sticky device setting, sampled once per session: the user's delta composed
    // with the sensor-mount base (camera_config.h).
    g->rotate = camera_config_effective_rotation();
    s_n_in.store(0); s_n_conv.store(0); s_n_pub.store(0);
    s_n_dec.store(0); s_n_hit.store(0);
    camera_preview_get_sink_dims(&g->disp_w, &g->disp_h);

    g->manager = std::make_unique<CameraManager>();
    if (g->manager->start()) {
        return bringup_failed(CAMERA_ERR_MANAGER);
    }
    if (g->manager->cameras().empty()) {
        return bringup_failed(CAMERA_ERR_NO_CAMERA);
    }
    g->camera = g->manager->cameras()[0];
    if (g->camera->acquire()) {
        return bringup_failed(CAMERA_ERR_ACQUIRE);
    }

    g->config = g->camera->generateConfiguration(
        { StreamRole::VideoRecording, StreamRole::Viewfinder });
    if (!g->config || g->config->size() < 2) {
        return bringup_failed(CAMERA_ERR_CONFIG_GENERATE);
    }
    g->config->at(0).pixelFormat = formats::YUV420;
    g->config->at(0).size = Size(kDecodeSide, kDecodeSide);
    g->config->at(0).bufferCount = kBufferCount;
    g->config->at(1).pixelFormat = formats::YUV420;
    g->config->at(1).size = Size(g->disp_w, g->disp_h);
    g->config->at(1).bufferCount = kBufferCount;
    if (g->config->validate() == CameraConfiguration::Invalid) {
        return bringup_failed(CAMERA_ERR_CONFIG_INVALID);
    }
    if (g->camera->configure(g->config.get())) {
        return bringup_failed(CAMERA_ERR_CONFIG_APPLY);
    }
    g->decode_stream  = g->config->at(0).stream();
    g->display_stream = g->config->at(1).stream();
    g->disp_ystride   = g->config->at(1).stride;
    g->dec_w = kDecodeSide;
    g->dec_h = kDecodeSide;
    g->dec_ystride = g->config->at(0).stride;

    g->allocator = std::make_unique<FrameBufferAllocator>(g->camera);
    if (g->allocator->allocate(g->decode_stream) < 0 ||
        g->allocator->allocate(g->display_stream) < 0) {
        return bringup_failed(CAMERA_ERR_ALLOC);
    }

    // mmap the whole buffer of every stream (plane-0 fd is the map key). Derive the
    // display frame's U/V offsets + strides from the actual plane layout.
    const auto &dbufs = g->allocator->buffers(g->display_stream);
    if (dbufs.empty()) {
        return bringup_failed(CAMERA_ERR_NO_BUFFERS);
    }
    {
        const auto &planes = dbufs[0]->planes();
        const size_t base = planes[0].offset;
        g->disp_total  = planes.back().offset + planes.back().length - base;
        g->disp_u_off  = planes[1].offset - base;
        g->disp_v_off  = planes[2].offset - base;
        g->disp_uvstride = static_cast<int>(planes[1].length / (g->disp_h / 2));
        g->scratch.assign(g->disp_total, 0);
        // Swap-partner of blit_worker's `work`; sized to the post-rotation extent, which is
        // disp_w*disp_h (the pixel product is rotation-invariant) — the swap keeps sizes equal.
        g->ready_buf.assign(static_cast<size_t>(g->disp_w) * g->disp_h * 2, 0);
    }
    {
        const auto &decbufs0 = g->allocator->buffers(g->decode_stream);
        if (decbufs0.empty()) {
            return bringup_failed(CAMERA_ERR_NO_BUFFERS);
        }
        g->dec_ylen = decbufs0[0]->planes()[0].length;  // Y plane bytes (stride*h)
        g->dec_scratch.assign(g->dec_ylen, 0);
    }
    for (Stream *s : { g->decode_stream, g->display_stream }) {
        for (const std::unique_ptr<FrameBuffer> &buf : g->allocator->buffers(s)) {
            const FrameBuffer::Plane &p0 = buf->planes()[0];
            int fd = p0.fd.get();
            if (g->maps.count(fd)) {
                continue;
            }
            size_t len = buf->planes().back().offset + buf->planes().back().length;
            void *mem = mmap(nullptr, len, PROT_READ, MAP_SHARED, fd, 0);
            if (mem == MAP_FAILED) {
                return bringup_failed(CAMERA_ERR_MMAP);
            }
            g->maps[fd] = PlaneMap{ mem, len };
        }
    }

    // One request per buffer index, each carrying both streams' buffers.
    const auto &decbufs = g->allocator->buffers(g->decode_stream);
    unsigned nreq = std::min(decbufs.size(), dbufs.size());
    for (unsigned i = 0; i < nreq; ++i) {
        std::unique_ptr<Request> req = g->camera->createRequest(i);
        if (!req ||
            req->addBuffer(g->decode_stream,  decbufs[i].get()) < 0 ||
            req->addBuffer(g->display_stream, dbufs[i].get()) < 0) {
            return bringup_failed(CAMERA_ERR_REQUEST);
        }
        g->requests.push_back(std::move(req));
    }

    g->camera->requestCompleted.connect(&on_request_completed);

    // Centered-square ScalerCrop -> undistorted, decode+display share one FOV.
    ControlList controls(g->camera->controls());
    const ControlInfoMap &cinfo = g->camera->controls();
    auto sc = cinfo.find(&controls::ScalerCrop);
    if (sc != cinfo.end()) {
        Rectangle max = sc->second.max().get<Rectangle>();
        unsigned side = std::min(max.width, max.height);
        controls.set(controls::ScalerCrop,
                     Rectangle(max.x + static_cast<int>((max.width - side) / 2),
                               max.y + static_cast<int>((max.height - side) / 2),
                               side, side));
    }
    {
        int64_t fd = 1000000 / CAMERA_TARGET_FPS;
        controls.set(controls::FrameDurationLimits, Span<const int64_t, 2>({ fd, fd }));
    }

    // zbar decoder, QR-only, cache disabled (the coordinator dedups; the cache would
    // suppress re-reads of a held/animated QR that we need decoded every frame).
    g->zbar = zbar_image_scanner_create();
    if (!g->zbar) {
        return bringup_failed(CAMERA_ERR_DECODER);
    }
    zbar_image_scanner_set_config(g->zbar, ZBAR_NONE, ZBAR_CFG_ENABLE, 0);
    zbar_image_scanner_set_config(g->zbar, ZBAR_QRCODE, ZBAR_CFG_ENABLE, 1);
    // Return byte-mode QR payloads as RAW bytes (ZBAR_CFG_BINARY = "don't convert
    // binary data to text"). Without it, zbar applies an ECI/charset conversion to
    // byte-mode content, corrupting binary QRs — notably CompactSeedQR (16/32 raw
    // entropy bytes, which routinely contain non-ASCII/NUL bytes). Numeric SeedQR
    // (ASCII digits) is unaffected either way. Mirrors the app's upstream pyzbar fork,
    // whose `binary=True` sets this same zbar config.
    zbar_image_scanner_set_config(g->zbar, ZBAR_QRCODE, ZBAR_CFG_BINARY, 1);
    zbar_image_scanner_enable_cache(g->zbar, 0);
    scan_coord_reset();

    g->running.store(true, std::memory_order_release);
    g->blit = std::thread(blit_worker);
    g->decode = std::thread(decode_worker);

    if (g->camera->start(&controls)) {
        g->running.store(false, std::memory_order_release);
        g->in_cv.notify_all();
        g->dec_cv.notify_all();
        if (g->blit.joinable()) {
            g->blit.join();
        }
        if (g->decode.joinable()) {
            g->decode.join();
        }
        g->camera->requestCompleted.disconnect(&on_request_completed);
        return bringup_failed(CAMERA_ERR_START);
    }
    for (auto &req : g->requests) {
        g->camera->queueRequest(req.get());
    }
    return CAMERA_OK;
}

void camera_engine_stop() {
    if (!g) {
        return;
    }
    g->running.store(false, std::memory_order_release);
    if (g->camera) {
        g->camera->stop();  // cancels in-flight requests; no more completions
        g->camera->requestCompleted.disconnect(&on_request_completed);
    }
    g->in_cv.notify_all();
    g->dec_cv.notify_all();
    if (g->blit.joinable()) {
        g->blit.join();
    }
    if (g->decode.joinable()) {
        g->decode.join();
    }
    if (g->zbar) {
        zbar_image_scanner_destroy(g->zbar);
        g->zbar = nullptr;
    }
    for (auto &kv : g->maps) {
        munmap(kv.second.mem, kv.second.len);
    }
    g->maps.clear();
    g->requests.clear();
    g->allocator.reset();
    if (g->camera) {
        g->camera->release();
        g->camera.reset();
    }
    g->config.reset();
    if (g->manager) {
        g->manager->stop();
        g->manager.reset();
    }
    // Publish g == nullptr under the lifecycle lock, AFTER the blocking teardown above ran
    // lock-free — so a concurrent pump-thread consume never derefs a freed engine (§3.4).
    std::lock_guard<std::mutex> lk(s_engine_mtx);
    delete g;
    g = nullptr;
}

bool camera_engine_is_running() {
    return g != nullptr && g->running.load(std::memory_order_acquire);
}

void camera_engine_pump_consume() {
    // Runs on the background pump thread (under the LVGL lock). Hold the lifecycle lock for
    // the whole body so start()/stop()/bringup_failed() on the host thread can't free g
    // mid-deref (§3.4). Leaf order: LVGL(held) -> s_engine_mtx -> out_mtx.
    std::lock_guard<std::mutex> lk_g(s_engine_mtx);
    if (!g) {
        return;
    }
    std::lock_guard<std::mutex> lk(g->out_mtx);
    if (g->dirty) {
        camera_preview_blit_rgb565(g->ready_buf.data(), g->ready_buf.size());
        g->dirty = false;
        s_n_pub.fetch_add(1, std::memory_order_relaxed);
    }
}

void camera_engine_stats(uint64_t *frames_in, uint64_t *frames_conv, uint64_t *frames_pub) {
    if (frames_in)   *frames_in   = s_n_in.load(std::memory_order_relaxed);
    if (frames_conv) *frames_conv = s_n_conv.load(std::memory_order_relaxed);
    if (frames_pub)  *frames_pub  = s_n_pub.load(std::memory_order_relaxed);
}

void camera_engine_decode_stats(uint64_t *attempts, uint64_t *hits) {
    if (attempts) *attempts = s_n_dec.load(std::memory_order_relaxed);
    if (hits)     *hits     = s_n_hit.load(std::memory_order_relaxed);
}
