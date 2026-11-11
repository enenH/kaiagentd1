#pragma once

struct KTouchProxy;

struct KTouchState {
    int x = 0;
    int y = 0;
    int dx = 0;
    int dy = 0;
    int down = 0;
    int pressed = 0;
    int released = 0;
};

extern "C" KTouchProxy* ktouchproxy_create(int width, int height, int rotation, int grab);
extern "C" void ktouchproxy_poll(KTouchProxy* handle, int timeout_ms);
extern "C" int ktouchproxy_get_touch_state(KTouchProxy* handle, KTouchState* out);
extern "C" int ktouchproxy_aim(
        KTouchProxy* handle,
        int pointer,
        int anchor_x,
        int anchor_y,
        int end_x,
        int end_y);
extern "C" void ktouchproxy_stop_aim(KTouchProxy* handle, int pointer);
extern "C" void ktouchproxy_destroy(KTouchProxy* handle);
