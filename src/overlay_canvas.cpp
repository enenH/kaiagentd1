#include "overlay_canvas.h"
#include "native_log.h"

#include <cstring>
#include <android/hardware_buffer.h>

bool Canvas::lock(ANativeWindow *window)
{
    if (locked_) unlockAndPost();

    window_ = window;

    ANativeWindow_Buffer buf;
    if (ANativeWindow_lock(window_, &buf, nullptr) != 0) {
        LOGE("ANativeWindow_lock failed");
        return false;
    }

    buffer_ = buf;
    locked_ = true;
    return true;
}

void Canvas::unlockAndPost()
{
    if (!locked_) return;
    ANativeWindow_unlockAndPost(window_);
    locked_ = false;
}

uint32_t *Canvas::pixels() const
{
    return locked_ ? (uint32_t *)buffer_.bits : nullptr;
}

int Canvas::width() const  { return buffer_.width; }
int Canvas::height() const { return buffer_.height; }
int Canvas::stride() const { return buffer_.stride; }
bool Canvas::isLocked() const { return locked_; }

void Canvas::clear(uint32_t color)
{
    if (!locked_) return;
    auto *px = (uint32_t *)buffer_.bits;
    int total = buffer_.stride * buffer_.height;
    if (color == 0) {
        memset(px, 0, total * sizeof(uint32_t));
    } else {
        for (int i = 0; i < total; ++i)
            px[i] = color;
    }
}

void Canvas::setPixel(int x, int y, uint32_t color)
{
    if (!locked_) return;
    if (x < 0 || x >= buffer_.width || y < 0 || y >= buffer_.height) return;
    auto *px = (uint32_t *)buffer_.bits;
    px[y * buffer_.stride + x] = color;
}

uint32_t Canvas::getPixel(int x, int y) const
{
    if (!locked_) return 0;
    if (x < 0 || x >= buffer_.width || y < 0 || y >= buffer_.height) return 0;
    auto *px = (uint32_t *)buffer_.bits;
    return px[y * buffer_.stride + x];
}

void Canvas::blendPixel(int x, int y, uint32_t color)
{
    if (!locked_) return;
    if ((unsigned)x >= (unsigned)buffer_.width ||
        (unsigned)y >= (unsigned)buffer_.height) return;

    uint8_t sa = colorA(color);
    if (sa == 0) return;

    uint32_t *px = (uint32_t *)buffer_.bits + y * buffer_.stride + x;
    if (sa == 255) { *px = color; return; }

    uint32_t dst = *px;
    uint16_t inv = 255 - sa;
    *px = rgba((uint8_t)((colorR(color) * sa + colorR(dst) * inv) / 255),
               (uint8_t)((colorG(color) * sa + colorG(dst) * inv) / 255),
               (uint8_t)((colorB(color) * sa + colorB(dst) * inv) / 255),
               (uint8_t)(sa + (colorA(dst) * inv) / 255));
}

void Canvas::hline(int x0, int x1, int y, uint32_t color)
{
    if (!locked_ || y < 0 || y >= buffer_.height) return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (x0 >= buffer_.width || x1 < 0) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= buffer_.width) x1 = buffer_.width - 1;

    uint32_t *row = (uint32_t *)buffer_.bits + y * buffer_.stride;
    uint8_t sa = colorA(color);
    if (sa == 0) return;

    if (sa == 255) {
        for (int x = x0; x <= x1; ++x) row[x] = color;
        return;
    }

    uint16_t sr = colorR(color) * sa;
    uint16_t sg = colorG(color) * sa;
    uint16_t sb = colorB(color) * sa;
    uint16_t inv = 255 - sa;
    for (int x = x0; x <= x1; ++x) {
        uint32_t d = row[x];
        row[x] = rgba((uint8_t)((sr + colorR(d) * inv) / 255),
                       (uint8_t)((sg + colorG(d) * inv) / 255),
                       (uint8_t)((sb + colorB(d) * inv) / 255),
                       (uint8_t)(sa + (colorA(d) * inv) / 255));
    }
}
