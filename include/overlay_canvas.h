#ifndef REALTIME_OVERLAY_CANVAS_H
#define REALTIME_OVERLAY_CANVAS_H

#include <cstdint>
#include <android/native_window.h>

class Canvas {
public:
    bool lock(ANativeWindow *window);
    void unlockAndPost();

    uint32_t *pixels() const;
    int width() const;
    int height() const;
    int stride() const;
    bool isLocked() const;

    void clear(uint32_t color = 0);
    void setPixel(int x, int y, uint32_t color);
    uint32_t getPixel(int x, int y) const;
    void blendPixel(int x, int y, uint32_t color);
    void hline(int x0, int x1, int y, uint32_t color);

private:
    ANativeWindow        *window_ = nullptr;
    ANativeWindow_Buffer  buffer_ = {};
    bool                  locked_ = false;
};

/* Color helpers */
constexpr uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return (uint32_t)a << 24 | (uint32_t)b << 16 | (uint32_t)g << 8 | r;
}

constexpr uint8_t colorR(uint32_t c) { return (uint8_t)(c); }
constexpr uint8_t colorG(uint32_t c) { return (uint8_t)(c >> 8); }
constexpr uint8_t colorB(uint32_t c) { return (uint8_t)(c >> 16); }
constexpr uint8_t colorA(uint32_t c) { return (uint8_t)(c >> 24); }

#endif  // REALTIME_OVERLAY_CANVAS_H
