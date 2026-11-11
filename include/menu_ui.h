#pragma once

#include <cstdint>

#include "overlay_canvas.h"

class Font;

struct MenuTouchState {
    int x = 0;
    int y = 0;
    int dx = 0;
    int dy = 0;
    bool down = false;
    bool pressed = false;
    bool released = false;
};

enum MenuUiColor {
    MENU_UI_COL_WINDOW_BG = 0,
    MENU_UI_COL_TITLE_BG,
    MENU_UI_COL_BORDER,
    MENU_UI_COL_TEXT,
    MENU_UI_COL_TEXT_DIM,
    MENU_UI_COL_BUTTON,
    MENU_UI_COL_BUTTON_HOVER,
    MENU_UI_COL_BUTTON_ACTIVE,
    MENU_UI_COL_SLIDER_TRACK,
    MENU_UI_COL_SLIDER_GRAB,
    MENU_UI_COL_SEPARATOR,
    MENU_UI_COL_COUNT
};

namespace menu_ui {

void newFrame(Canvas& canvas, const MenuTouchState& touch, Font* font = nullptr);
void render();

bool beginWindow(const char* title, int x, int y, int w, int h = 0, bool* open = nullptr);
void endWindow();

bool button(const char* label);
bool sliderFloat(const char* label, float* value, float min, float max, int decimals = 2);
bool sliderInt(const char* label, int* value, int min, int max);
void text(const char* str);
void textColored(uint32_t color, const char* str);
void separator();
void sameLine();
bool beginTabBar(const char* id);
bool tabItem(const char* label);
void endTabBar();

bool isTouchCaptured();
void setColor(int idx, uint32_t color);

}  // namespace menu_ui
