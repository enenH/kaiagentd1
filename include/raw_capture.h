#ifndef REALTIME_RAW_CAPTURE_H
#define REALTIME_RAW_CAPTURE_H

#include <cstddef>
#include <cstdint>

struct ScreenrecordRawCapture;

enum ScreenrecordRawCaptureBackend {
    SCREENRECORD_RAW_CAPTURE_BACKEND_AIMAGE_READER = 0,
    SCREENRECORD_RAW_CAPTURE_BACKEND_CPU_CONSUMER = 1,
};

extern "C" int screenrecord_raw_capture_create(
        int width, int height, ScreenrecordRawCapture** out);
extern "C" int screenrecord_raw_capture_create_crop(
        int width,
        int height,
        int cropLeft,
        int cropTop,
        int cropWidth,
        int cropHeight,
        ScreenrecordRawCapture** out);
extern "C" int screenrecord_raw_capture_create_crop_backend(
        int width,
        int height,
        int cropLeft,
        int cropTop,
        int cropWidth,
        int cropHeight,
        ScreenrecordRawCaptureBackend backend,
        ScreenrecordRawCapture** out);
extern "C" int screenrecord_raw_capture_read(
        ScreenrecordRawCapture* capture,
        const uint8_t** data,
        size_t* size,
        int* width,
        int* height);
extern "C" void screenrecord_raw_capture_destroy(ScreenrecordRawCapture* capture);

#endif  // REALTIME_RAW_CAPTURE_H
