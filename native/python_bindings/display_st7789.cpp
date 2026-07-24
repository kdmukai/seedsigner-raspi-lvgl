// Native ST7789 display backend: SPI panel driver (spidev) + GPIO output lines
// (DC/RST/backlight) + the flush-path flags the LVGL runtime dispatches on.
// Owns /dev/gpiochip0; gpio_input.cpp requests its input lines from the same
// chip fd via the accessors at the bottom.
#include "module_internal.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>

#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <unistd.h>

struct native_display_t {
    int spi_fd;
    int gpio_chip_fd;
    int dc_line_fd;
    int rst_line_fd;
    int bl_line_fd;
    bool gpiochip_ready;
    int dc_pin;
    int rst_pin;
    int bl_pin;
    uint32_t speed_hz;
    uint8_t mode;
    uint8_t bits_per_word;
    uint32_t width;
    uint32_t height;
    bool bgr;
    bool ready;
};

static native_display_t s_native = {
    .spi_fd = -1,
    .gpio_chip_fd = -1,
    .dc_line_fd = -1,
    .rst_line_fd = -1,
    .bl_line_fd = -1,
    .gpiochip_ready = false,
    .dc_pin = 25,
    .rst_pin = 27,
    .bl_pin = 24,
    .speed_hz = 40000000,
    .mode = 0,
    .bits_per_word = 8,
    .width = 240,
    .height = 240,
    // The SeedSigner Pi Zero ST7789 panel is BGR-wired (matches SeedSigner's own
    // display drivers, which use BGR color order). Default to BGR so the MADCTL
    // BGR bit is set and RGB565 isn't red/blue-swapped (orange would otherwise
    // render light blue). Override per call via native_display_init(bgr=...).
    .bgr = true,
    .ready = false,
};

static bool s_use_native_flush = false;
static bool s_native_lvgl_swap_bytes = true;
static bool s_native_debug_log = false;
static uint32_t s_native_flush_log_limit = 20;
static uint32_t s_native_flush_log_count = 0;

static void gpiochip_init_lines() {
    s_native.gpio_chip_fd = open("/dev/gpiochip0", O_RDWR | O_CLOEXEC);
    if (s_native.gpio_chip_fd < 0) {
        throw std::runtime_error("open /dev/gpiochip0 failed errno=" + std::to_string(errno));
    }

    s_native.dc_line_fd = gpiochip_request_line(s_native.gpio_chip_fd, s_native.dc_pin, "sslvgl-dc");
    s_native.rst_line_fd = gpiochip_request_line(s_native.gpio_chip_fd, s_native.rst_pin, "sslvgl-rst");
    s_native.bl_line_fd = gpiochip_request_line(s_native.gpio_chip_fd, s_native.bl_pin, "sslvgl-bl");
    s_native.gpiochip_ready = true;
}

static void gpiochip_shutdown_lines() {
    if (s_native.dc_line_fd >= 0) { close(s_native.dc_line_fd); s_native.dc_line_fd = -1; }
    if (s_native.rst_line_fd >= 0) { close(s_native.rst_line_fd); s_native.rst_line_fd = -1; }
    if (s_native.bl_line_fd >= 0) { close(s_native.bl_line_fd); s_native.bl_line_fd = -1; }
    if (s_native.gpio_chip_fd >= 0) { close(s_native.gpio_chip_fd); s_native.gpio_chip_fd = -1; }
    s_native.gpiochip_ready = false;
}

static int gpio_line_fd_for_pin(int pin) {
    if (pin == s_native.dc_pin) return s_native.dc_line_fd;
    if (pin == s_native.rst_pin) return s_native.rst_line_fd;
    if (pin == s_native.bl_pin) return s_native.bl_line_fd;
    return -1;
}

static void gpio_write_value(int pin, bool high) {
    if (s_native.gpiochip_ready) {
        int fd = gpio_line_fd_for_pin(pin);
        if (fd < 0) {
            throw std::runtime_error("gpio line fd missing for pin " + std::to_string(pin));
        }
        struct gpiohandle_data data;
        memset(&data, 0, sizeof(data));
        data.values[0] = high ? 1 : 0;
        if (ioctl(fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data) < 0) {
            throw std::runtime_error("GPIOHANDLE_SET_LINE_VALUES_IOCTL failed pin=" + std::to_string(pin) + " errno=" + std::to_string(errno));
        }
        return;
    }

    write_text_file("/sys/class/gpio/gpio" + std::to_string(pin) + "/value", high ? "1" : "0");
}

static void native_spi_write(const uint8_t *data, size_t len) {
    if (s_native.spi_fd < 0) {
        throw std::runtime_error("native SPI not initialized");
    }

    const size_t kChunk = 4096;
    size_t off = 0;
    while (off < len) {
        size_t nreq = (len - off > kChunk) ? kChunk : (len - off);

        struct spi_ioc_transfer tr;
        memset(&tr, 0, sizeof(tr));
        tr.tx_buf = reinterpret_cast<unsigned long>(data + off);
        tr.rx_buf = 0;
        tr.len = static_cast<__u32>(nreq);
        tr.speed_hz = s_native.speed_hz;
        tr.bits_per_word = s_native.bits_per_word;
        tr.delay_usecs = 0;
        tr.cs_change = 0;

        int rc = ioctl(s_native.spi_fd, SPI_IOC_MESSAGE(1), &tr);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error("SPI_IOC_MESSAGE failed errno=" + std::to_string(errno));
        }
        off += nreq;
    }
}

static void native_cmd(uint8_t cmd) {
    gpio_write_value(s_native.dc_pin, false);
    native_spi_write(&cmd, 1);
}

static void native_data_byte(uint8_t b) {
    gpio_write_value(s_native.dc_pin, true);
    native_spi_write(&b, 1);
}

static void native_data_buf(const uint8_t *buf, size_t len) {
    gpio_write_value(s_native.dc_pin, true);
    native_spi_write(buf, len);
}

static void native_set_window(int x1, int y1, int x2, int y2) {
    native_cmd(0x2A);
    uint8_t col[] = {static_cast<uint8_t>((x1 >> 8) & 0xFF), static_cast<uint8_t>(x1 & 0xFF), static_cast<uint8_t>((x2 >> 8) & 0xFF), static_cast<uint8_t>(x2 & 0xFF)};
    native_data_buf(col, sizeof(col));

    native_cmd(0x2B);
    uint8_t row[] = {static_cast<uint8_t>((y1 >> 8) & 0xFF), static_cast<uint8_t>(y1 & 0xFF), static_cast<uint8_t>((y2 >> 8) & 0xFF), static_cast<uint8_t>(y2 & 0xFF)};
    native_data_buf(row, sizeof(row));

    native_cmd(0x2C);
}

static void native_hw_reset() {
    gpio_write_value(s_native.rst_pin, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    gpio_write_value(s_native.rst_pin, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    gpio_write_value(s_native.rst_pin, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

static uint8_t native_madctl_for_resolution() {
    // MADCTL register controls memory access direction (rotation).
    // 240x240: 0x70 (MX+MV+ML) — proven on Waveshare 1.3" hat
    // 320x240: 0x60 (MX+MV) — landscape rotation (from st7789_mpy rotation 1)
    uint8_t madctl = (s_native.width == 320 && s_native.height == 240) ? 0x60 : 0x70;
    if (s_native.bgr) {
        madctl |= 0x08;  // BGR bit
    }
    return madctl;
}

static void native_display_init_sequence() {
    // Full ST7789 initialization sequence (matches SeedSigner st7789_mpy.py).
    native_cmd(0x11);                                           // SLPOUT: exit sleep
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    native_cmd(0x13);                                           // NORON: normal display mode
    native_cmd(0xB6);                                           // Display function control
    uint8_t b6[] = {0x0A, 0x82};
    native_data_buf(b6, sizeof(b6));
    native_cmd(0x3A);                                           // COLMOD: 16-bit RGB565
    native_data_byte(0x55);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    native_cmd(0xB2);                                           // Porch control
    uint8_t b2[] = {0x0C, 0x0C, 0x00, 0x33, 0x33};
    native_data_buf(b2, sizeof(b2));
    native_cmd(0xB7);                                           // Gate control
    native_data_byte(0x35);
    native_cmd(0xBB);                                           // VCOMS setting
    native_data_byte(0x28);
    native_cmd(0xC0);                                           // Power control 1
    native_data_byte(0x0C);
    native_cmd(0xC2);                                           // Power control 2
    uint8_t c2[] = {0x01, 0xFF};
    native_data_buf(c2, sizeof(c2));
    native_cmd(0xC3);                                           // Power control 3
    native_data_byte(0x10);
    native_cmd(0xC4);                                           // Power control 4
    native_data_byte(0x20);
    native_cmd(0xC6);                                           // VCOM control 1
    native_data_byte(0x0F);
    native_cmd(0xD0);                                           // Power control A
    uint8_t d0[] = {0xA4, 0xA1};
    native_data_buf(d0, sizeof(d0));
    native_cmd(0xE0);                                           // Gamma positive
    uint8_t e0[] = {0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x32, 0x44, 0x42, 0x06, 0x0E, 0x12, 0x14, 0x17};
    native_data_buf(e0, sizeof(e0));
    native_cmd(0xE1);                                           // Gamma negative
    uint8_t e1[] = {0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x31, 0x54, 0x47, 0x0E, 0x1C, 0x17, 0x1B, 0x1E};
    native_data_buf(e1, sizeof(e1));
    native_cmd(0x21);                                           // INVON: display inversion
    native_cmd(0x36);                                           // MADCTL: rotation/color order
    native_data_byte(native_madctl_for_resolution());
    native_cmd(0x29);                                           // DISPON: display on
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
}

static void native_display_blit(int x1, int y1, int x2, int y2, const uint8_t *buf, size_t len) {
    native_set_window(x1, y1, x2, y2);
    native_data_buf(buf, len);
}

static void native_display_test_pattern() {
    if (!s_native.ready) {
        throw std::runtime_error("native display not initialized");
    }

    const uint16_t colors[] = {
        0xF800, // red
        0x07E0, // green
        0x001F, // blue
        0xFFFF, // white
        0x0000, // black
    };
    const int bands = static_cast<int>(sizeof(colors) / sizeof(colors[0]));
    int band_h = static_cast<int>(s_native.height) / bands;
    if (band_h <= 0) {
        band_h = 1;
    }

    std::vector<uint8_t> row(static_cast<size_t>(s_native.width) * 2);
    for (int b = 0; b < bands; ++b) {
        uint16_t c = colors[b];
        for (uint32_t x = 0; x < s_native.width; ++x) {
            row[2 * x] = static_cast<uint8_t>((c >> 8) & 0xFF);
            row[2 * x + 1] = static_cast<uint8_t>(c & 0xFF);
        }

        int y_start = b * band_h;
        int y_end = (b == bands - 1) ? static_cast<int>(s_native.height) - 1 : (y_start + band_h - 1);
        if (y_end < y_start) {
            continue;
        }

        for (int y = y_start; y <= y_end; ++y) {
            native_set_window(0, y, static_cast<int>(s_native.width) - 1, y);
            native_data_buf(row.data(), row.size());
        }
    }
}

// --- Cross-unit API (see module_internal.h) --------------------------------

bool native_flush_active() {
    return s_use_native_flush && s_native.ready;
}

// RASPI-5 Phase 2: arm native flush as the selected mode (design §4). native_flush_active()
// still gates on s_native.ready, so this is a no-op paint until native_display_init brings up
// the panel — at which point the flush is native without the app calling set_flush_mode.
// The background pump thread requires this: the Python flush path would deadlock it (§3.1).
void native_flush_select_native() {
    s_use_native_flush = true;
}

void native_flush_blit(const lv_area_t *area, const uint8_t *px_map, size_t nbytes) {
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;

    if (s_native_debug_log && s_native_flush_log_count < s_native_flush_log_limit) {
        fprintf(stderr, "[seedsigner_lvgl_screens] flush #%u area=(%d,%d)-(%d,%d) w=%d h=%d nbytes=%zu\n",
                s_native_flush_log_count + 1, area->x1, area->y1, area->x2, area->y2, w, h, nbytes);
        s_native_flush_log_count++;
    }

    try {
        const uint8_t *src = px_map;
        std::vector<uint8_t> swapped;
        if (s_native_lvgl_swap_bytes) {
            swapped.resize(nbytes);
            for (size_t i = 0; i + 1 < nbytes; i += 2) {
                swapped[i] = px_map[i + 1];
                swapped[i + 1] = px_map[i];
            }
            src = swapped.data();
        }
        native_display_blit(area->x1, area->y1, area->x2, area->y2, src, nbytes);
    } catch (const std::exception &e) {
        fprintf(stderr, "[seedsigner_lvgl_screens] native flush failed: %s\n", e.what());
    }
}

void display_update_resolution(uint32_t width, uint32_t height) {
    if (!native_flush_active()) {
        return;
    }
    s_native.width = width;
    s_native.height = height;
    native_cmd(0x36);
    native_data_byte(native_madctl_for_resolution());
}

void native_display_shutdown_internal() {
    // Clear display to black before tearing down hardware.
    if (s_native.ready && lvgl_runtime_is_inited()) {
        lvgl_clear_to_black(true);
    }

    native_input_shutdown();

    if (s_native.spi_fd >= 0) {
        close(s_native.spi_fd);
        s_native.spi_fd = -1;
    }

    if (s_native.gpiochip_ready) {
        gpiochip_shutdown_lines();
    } else if (s_native.ready) {
        try { gpio_unexport_pin(s_native.dc_pin); } catch (...) {}
        try { gpio_unexport_pin(s_native.rst_pin); } catch (...) {}
        try { gpio_unexport_pin(s_native.bl_pin); } catch (...) {}
    }

    s_native.ready = false;
}

bool native_gpiochip_ready() {
    return s_native.gpiochip_ready;
}

int native_gpio_chip_fd() {
    return s_native.gpio_chip_fd;
}

int native_gpio_chip_open_for_input() {
    if (s_native.gpio_chip_fd < 0) {
        s_native.gpio_chip_fd = open("/dev/gpiochip0", O_RDWR | O_CLOEXEC);
        if (s_native.gpio_chip_fd < 0) {
            throw std::runtime_error("open /dev/gpiochip0 failed errno=" + std::to_string(errno));
        }
        // Mark gpiochip as ready so native_input_init can use it,
        // but display lines are NOT claimed.
        s_native.gpiochip_ready = true;
    }
    return s_native.gpio_chip_fd;
}

// --- Python entry points ----------------------------------------------------

PyObject *py_native_display_init(PyObject *self, PyObject *args, PyObject *kwargs) {
    (void)self;
    static const char *kwlist[] = {"width", "height", "dc_pin", "rst_pin", "bl_pin", "spi_path", "spi_speed_hz", "bgr", "lvgl_swap_bytes", NULL};
    unsigned int width = 240;
    unsigned int height = 240;
    int dc_pin = 25;
    int rst_pin = 27;
    int bl_pin = 24;
    const char *spi_path = "/dev/spidev0.0";
    unsigned int spi_speed_hz = 62500000;
    int bgr = 1;  // BGR by default — the Pi 0 ST7789 panel is BGR-wired (see s_native)
    int lvgl_swap_bytes = 1;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|IIiiisIpp", const_cast<char **>(kwlist),
                                     &width, &height, &dc_pin, &rst_pin, &bl_pin, &spi_path, &spi_speed_hz, &bgr, &lvgl_swap_bytes)) {
        return NULL;
    }

    try {
        native_display_shutdown_internal();

        s_native.width = width;
        s_native.height = height;
        s_native.dc_pin = dc_pin;
        s_native.rst_pin = rst_pin;
        s_native.bl_pin = bl_pin;
        s_native.speed_hz = spi_speed_hz;
        s_native.bgr = (bgr != 0);
        s_native_lvgl_swap_bytes = (lvgl_swap_bytes != 0);

        try {
            gpiochip_init_lines();
#if LV_USE_LOG
            LV_LOG_USER("native gpio backend: gpiochip");
#endif
        } catch (const std::exception &) {
            gpiochip_shutdown_lines();
            // Fallback for environments where gpiochip access is unavailable.
            gpio_export_pin(s_native.dc_pin);
            gpio_export_pin(s_native.rst_pin);
            gpio_export_pin(s_native.bl_pin);
            gpio_set_dir_out(s_native.dc_pin);
            gpio_set_dir_out(s_native.rst_pin);
            gpio_set_dir_out(s_native.bl_pin);
#if LV_USE_LOG
            LV_LOG_USER("native gpio backend: sysfs fallback");
#endif
        }

        s_native.spi_fd = open(spi_path, O_RDWR | O_CLOEXEC);
        if (s_native.spi_fd < 0) {
            throw std::runtime_error("failed to open spi path");
        }

        if (ioctl(s_native.spi_fd, SPI_IOC_WR_MODE, &s_native.mode) < 0) {
            throw std::runtime_error("failed to set SPI mode");
        }
        if (ioctl(s_native.spi_fd, SPI_IOC_WR_BITS_PER_WORD, &s_native.bits_per_word) < 0) {
            throw std::runtime_error("failed to set SPI bits-per-word");
        }
        if (ioctl(s_native.spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &s_native.speed_hz) < 0) {
            throw std::runtime_error("failed to set SPI speed");
        }

        gpio_write_value(s_native.bl_pin, true);
        native_hw_reset();
        native_display_init_sequence();

        // Input (joystick/buttons) is best-effort for now; display can run without it.
        try {
            native_input_init();
        } catch (const std::exception &e) {
            fprintf(stderr, "[seedsigner_lvgl_screens] native input init skipped: %s\n", e.what());
            native_input_shutdown();
        }

        s_native.ready = true;
        s_use_native_flush = true;
    } catch (const std::exception &e) {
        native_display_shutdown_internal();
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }

    Py_RETURN_NONE;
}

PyObject *py_native_display_shutdown(PyObject *self, PyObject *args) {
    (void)self;
    (void)args;
    native_display_shutdown_internal();
    s_use_native_flush = false;
    Py_RETURN_NONE;
}

PyObject *py_native_display_test_pattern(PyObject *self, PyObject *args) {
    (void)self;
    (void)args;
    try {
        native_display_test_pattern();
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }
    Py_RETURN_NONE;
}

PyObject *py_native_debug_config(PyObject *self, PyObject *args, PyObject *kwargs) {
    (void)self;
    static const char *kwlist[] = {"enabled", "flush_log_limit", NULL};
    int enabled = 1;
    unsigned int flush_log_limit = 20;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|pI", const_cast<char **>(kwlist), &enabled, &flush_log_limit)) {
        return NULL;
    }

    s_native_debug_log = (enabled != 0);
    s_native_flush_log_limit = flush_log_limit;
    s_native_flush_log_count = 0;
    Py_RETURN_NONE;
}

PyObject *py_set_flush_mode(PyObject *self, PyObject *args) {
    (void)self;
    const char *mode = NULL;
    if (!PyArg_ParseTuple(args, "s", &mode)) {
        return NULL;
    }

    if (std::strcmp(mode, "native") == 0) {
        s_use_native_flush = true;
    } else if (std::strcmp(mode, "python") == 0) {
        s_use_native_flush = false;
    } else {
        PyErr_SetString(PyExc_ValueError, "mode must be 'native' or 'python'");
        return NULL;
    }

    Py_RETURN_NONE;
}
