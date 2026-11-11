#ifndef REALTIME_OVERLAY_SURFACE_H
#define REALTIME_OVERLAY_SURFACE_H

#include <jni.h>
#include <android/native_window.h>

struct SurfaceContext {
    jobject        surfaceControl = nullptr;
    jobject        surface        = nullptr;
    ANativeWindow *window         = nullptr;
    int width  = 0;
    int height = 0;
};

bool surface_create(JNIEnv *env, SurfaceContext &ctx, int width, int height,
                    const char *name = nullptr, bool setSkipScreenshot = true);
void surface_destroy(JNIEnv *env, SurfaceContext &ctx);

/* Get real display resolution and rotation via DisplayManagerGlobal (no Context needed) */
bool surface_get_display_size(JNIEnv *env, int &width, int &height, int *rotation = nullptr);

#endif  // REALTIME_OVERLAY_SURFACE_H
