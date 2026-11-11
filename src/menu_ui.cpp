#include "menu_ui.h"

#include "font.h"
#include "overlay_draw.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

uint32_t fnv1a(const char* s) {
    uint32_t h = 2166136261u;
    for (; s && *s; ++s) {
        h ^= static_cast<uint8_t>(*s);
        h *= 16777619u;
    }
    return h;
}

struct TabDrawItem {
    std::string label;
    int index = 0;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    int pad = 0;
    bool pressed = false;
};

struct UiCtx {
    Canvas* canvas = nullptr;
    Font* font = nullptr;
    MenuTouchState touch {};

    uint32_t hot = 0;
    uint32_t active = 0;
    uint32_t prevActive = 0;
    uint32_t dragWinId = 0;
    int dragOffX = 0;
    int dragOffY = 0;

    uint32_t winId = 0;
    int wx = 0;
    int wy = 0;
    int ww = 0;
    int wh = 0;
    int cx = 0;
    int cy = 0;
    int prevCy = 0;
    int nextX = 0;
    bool inWindow = false;
    bool contentVisible = false;
    bool captured = false;

    int padding = 10;
    int spacing = 6;
    int lineH = 30;
    int titleH = 38;
    int cornerR = 6;
    int textScale = 2;
    int fontH = 16;
    uint32_t tabBarId = 0;
    int tabX = 0;
    int tabIndex = 0;
    std::vector<TabDrawItem> tabItems;

    uint32_t colors[MENU_UI_COL_COUNT] {};
    bool colorsInit = false;
};

UiCtx g;

std::unordered_map<uint32_t, int> s_winH;
std::unordered_map<uint32_t, std::pair<int, int>> s_winPos;
std::unordered_map<uint32_t, bool> s_winCollapsed;
std::unordered_map<uint32_t, int> s_tabs;

void initColors() {
    g.colors[MENU_UI_COL_WINDOW_BG] = rgba(18, 22, 27, 232);
    g.colors[MENU_UI_COL_TITLE_BG] = rgba(34, 41, 50, 246);
    g.colors[MENU_UI_COL_BORDER] = rgba(83, 95, 112, 230);
    g.colors[MENU_UI_COL_TEXT] = rgba(236, 241, 247, 255);
    g.colors[MENU_UI_COL_TEXT_DIM] = rgba(164, 174, 188, 255);
    g.colors[MENU_UI_COL_BUTTON] = rgba(45, 105, 165, 245);
    g.colors[MENU_UI_COL_BUTTON_HOVER] = rgba(60, 125, 190, 250);
    g.colors[MENU_UI_COL_BUTTON_ACTIVE] = rgba(31, 84, 137, 255);
    g.colors[MENU_UI_COL_SLIDER_TRACK] = rgba(54, 61, 70, 245);
    g.colors[MENU_UI_COL_SLIDER_GRAB] = rgba(86, 177, 132, 255);
    g.colors[MENU_UI_COL_SEPARATOR] = rgba(95, 108, 124, 180);
    g.colorsInit = true;
}

bool hitTest(int x, int y, int w, int h) {
    return g.touch.x >= x && g.touch.x < x + w &&
           g.touch.y >= y && g.touch.y < y + h;
}

int textH() {
    if (g.font && g.font->isLoaded()) {
        return g.font->lineHeight();
    }
    return 8 * g.textScale;
}

int textW(const char* s) {
    if (g.font && g.font->isLoaded()) {
        return g.font->measureText(s ? s : "");
    }
    return static_cast<int>(strlen(s ? s : "")) * 8 * g.textScale;
}

void drawLabel(int x, int y, const char* s, uint32_t color) {
    if (g.font && g.font->isLoaded()) {
        g.font->drawText(*g.canvas, x, y, s ? s : "", color);
    } else {
        draw::text(*g.canvas, x, y, s ? s : "", color, g.textScale);
    }
}

int contentW() {
    return std::max(1, g.wx + g.ww - g.padding - g.cx);
}

void advance(int extra = 0) {
    g.prevCy = g.cy;
    g.cy += g.lineH + g.spacing + extra;
    g.cx = g.wx + g.padding;
}

bool canDrawContent() {
    return g.inWindow && g.contentVisible && g.canvas;
}

uint32_t widgetId(const char* label) {
    return fnv1a(label) ^ (g.winId * 16777619u);
}

void clampWindowToCanvas(int w, int h, int* x, int* y) {
    if (!g.canvas || !x || !y) {
        return;
    }
    const int maxX = std::max(0, g.canvas->width() - w);
    const int maxY = std::max(0, g.canvas->height() - std::min(h, g.titleH));
    *x = std::clamp(*x, 0, maxX);
    *y = std::clamp(*y, 0, maxY);
}

}  // namespace

namespace menu_ui {

void newFrame(Canvas& canvas, const MenuTouchState& touch, Font* font) {
    g.canvas = &canvas;
    g.font = font;
    g.touch = touch;
    g.inWindow = false;
    g.contentVisible = false;
    g.hot = 0;
    g.captured = false;
    g.tabItems.clear();

    if (!g.colorsInit) {
        initColors();
    }

    const int fh = textH();
    g.fontH = fh;
    g.lineH = std::max(30, fh * 3 / 2);
    g.titleH = std::max(38, g.lineH + fh / 3);
    g.padding = std::max(10, fh / 2);
    g.spacing = std::max(6, fh / 4);
    g.cornerR = std::max(6, fh / 4);

    g.prevActive = g.active;
    if (!touch.down) {
        g.active = 0;
        g.dragWinId = 0;
        g.dragOffX = 0;
        g.dragOffY = 0;
    }
}

void render() {
}

bool beginWindow(const char* title, int x, int y, int w, int h, bool* open) {
    if (open && !*open) {
        return false;
    }
    const uint32_t id = fnv1a(title);
    const uint32_t collapseId = id ^ 0x7f4a7c15u;
    const uint32_t closeId = id ^ 0x59d3b937u;

    auto posIt = s_winPos.find(id);
    if (posIt != s_winPos.end()) {
        x = posIt->second.first;
        y = posIt->second.second;
    } else {
        s_winPos[id] = {x, y};
    }

    bool collapsed = false;
    auto collapsedIt = s_winCollapsed.find(id);
    if (collapsedIt != s_winCollapsed.end()) {
        collapsed = collapsedIt->second;
    } else {
        s_winCollapsed[id] = false;
    }

    const int titleBarH = collapsed ? (g.titleH + g.spacing) : g.titleH;
    const int storedH = s_winH.count(id) ? s_winH[id] : 980;
    const int resolvedH = collapsed ? (g.titleH + g.spacing + 2) : (h > 0 ? h : storedH);
    clampWindowToCanvas(w, resolvedH, &x, &y);

    const bool insideWindow = hitTest(x, y, w, resolvedH);
    if (insideWindow && (g.touch.down || g.touch.pressed || g.touch.released)) {
        g.captured = true;
    }

    int arrowBox = std::max(g.fontH, g.titleH - g.spacing * 2);
    int arrowX = x + g.padding;
    int arrowY = y + (titleBarH - arrowBox) / 2;
    const bool arrowHovered = hitTest(arrowX, arrowY, arrowBox, arrowBox);
    int closeBox = arrowBox;
    int closeX = x + w - g.padding - closeBox;
    int closeY = y + (titleBarH - closeBox) / 2;
    const bool closeHovered = open && hitTest(closeX, closeY, closeBox, closeBox);
    bool arrowPressed = false;
    bool closePressed = false;
    if (arrowHovered) {
        g.hot = collapseId;
        if (g.touch.pressed) {
            g.active = collapseId;
        }
        if (g.touch.down && g.active == collapseId) {
            arrowPressed = true;
        }
        if (g.touch.released && (g.prevActive == collapseId || g.active == collapseId)) {
            collapsed = !collapsed;
            s_winCollapsed[id] = collapsed;
            g.captured = true;
        }
    }
    if (closeHovered) {
        g.hot = closeId;
        if (g.touch.pressed) {
            g.active = closeId;
        }
        if (g.touch.down && g.active == closeId) {
            closePressed = true;
        }
        if (g.touch.released && (g.prevActive == closeId || g.active == closeId)) {
            *open = false;
            g.captured = true;
        }
    }

    if (hitTest(x + 1, y + 1, w - 2, titleBarH) && !arrowHovered && !closeHovered) {
        g.captured = g.captured || g.touch.down || g.touch.pressed || g.touch.released;
        if (g.touch.pressed && g.active == 0 && g.dragWinId == 0) {
            g.dragWinId = id;
            g.dragOffX = g.touch.x - x;
            g.dragOffY = g.touch.y - y;
        }
    }
    if (g.dragWinId == id && g.touch.down) {
        x = g.touch.x - g.dragOffX;
        y = g.touch.y - g.dragOffY;
        clampWindowToCanvas(w, resolvedH, &x, &y);
        s_winPos[id] = {x, y};
        arrowX = x + g.padding;
        arrowY = y + (titleBarH - arrowBox) / 2;
        closeX = x + w - g.padding - closeBox;
        closeY = y + (titleBarH - closeBox) / 2;
        g.captured = true;
    }

    g.winId = id;
    g.wx = x;
    g.wy = y;
    g.ww = w;
    g.wh = resolvedH;
    g.inWindow = true;
    g.contentVisible = !collapsed;

    draw::fillRoundRect(*g.canvas, x, y, w, resolvedH, g.cornerR,
                        g.colors[MENU_UI_COL_WINDOW_BG]);
    draw::roundRect(*g.canvas, x, y, w, resolvedH, g.cornerR,
                    g.colors[MENU_UI_COL_BORDER]);
    draw::fillRoundRect(*g.canvas, x + 1, y + 1, w - 2, titleBarH,
                        g.cornerR, g.colors[MENU_UI_COL_TITLE_BG]);

    const int th = textH();
    const int tw = textW(title);
    const int titleMinX = arrowX + arrowBox + g.spacing;
    const int titleRight = open ? closeX - g.spacing : x + w - g.padding;
    const int titleMaxX = titleRight - tw;
    int titleX = x + (w - tw) / 2;
    if (titleMaxX < titleMinX) {
        titleX = titleMinX;
    } else {
        titleX = std::clamp(titleX, titleMinX, titleMaxX);
    }
    drawLabel(titleX, y + (titleBarH - th) / 2, title, g.colors[MENU_UI_COL_TEXT]);

    if (arrowHovered || arrowPressed) {
        draw::fillRoundRect(*g.canvas, arrowX, arrowY, arrowBox, arrowBox,
                            std::max(1, g.cornerR / 2),
                            arrowPressed ? g.colors[MENU_UI_COL_BUTTON_ACTIVE]
                                         : g.colors[MENU_UI_COL_BUTTON_HOVER]);
    }
    if (open && (closeHovered || closePressed)) {
        draw::fillRoundRect(*g.canvas, closeX, closeY, closeBox, closeBox,
                            std::max(1, g.cornerR / 2),
                            closePressed ? g.colors[MENU_UI_COL_BUTTON_ACTIVE]
                                         : g.colors[MENU_UI_COL_BUTTON_HOVER]);
    }

    const int tri = std::max(4, arrowBox / 3);
    const int tcx = arrowX + arrowBox / 2;
    const int tcy = arrowY + arrowBox / 2;
    if (collapsed) {
        draw::fillTriangle(*g.canvas, tcx - tri / 2, tcy - tri,
                           tcx - tri / 2, tcy + tri,
                           tcx + tri, tcy, g.colors[MENU_UI_COL_TEXT]);
    } else {
        draw::fillTriangle(*g.canvas, tcx - tri, tcy - tri / 2,
                           tcx + tri, tcy - tri / 2,
                           tcx, tcy + tri, g.colors[MENU_UI_COL_TEXT]);
    }
    if (open) {
        const int inset = std::max(6, closeBox / 4);
        draw::line(*g.canvas, closeX + inset, closeY + inset,
                   closeX + closeBox - inset, closeY + closeBox - inset,
                   g.colors[MENU_UI_COL_TEXT]);
        draw::line(*g.canvas, closeX + closeBox - inset, closeY + inset,
                   closeX + inset, closeY + closeBox - inset,
                   g.colors[MENU_UI_COL_TEXT]);
    }

    g.cx = x + g.padding;
    g.cy = y + g.titleH + g.spacing;
    return g.contentVisible;
}

void endWindow() {
    if (!g.inWindow) {
        return;
    }
    if (g.contentVisible) {
        s_winH[g.winId] = g.cy - g.wy + g.padding;
    }
    g.inWindow = false;
    g.contentVisible = false;
}

bool button(const char* label) {
    if (!canDrawContent()) {
        return false;
    }

    const uint32_t id = widgetId(label);
    const int x = g.cx;
    const int y = g.cy;
    const int w = contentW();
    const int h = g.lineH;
    bool clicked = false;
    uint32_t bg = g.colors[MENU_UI_COL_BUTTON];

    if (hitTest(x, y, w, h)) {
        g.hot = id;
        g.captured = g.captured || g.touch.down || g.touch.pressed || g.touch.released;
        bg = g.colors[MENU_UI_COL_BUTTON_HOVER];
        if (g.touch.pressed) {
            g.active = id;
        }
        if (g.touch.down && g.active == id) {
            bg = g.colors[MENU_UI_COL_BUTTON_ACTIVE];
        }
        if (g.touch.released && (g.prevActive == id || g.active == id)) {
            clicked = true;
        }
    }

    draw::fillRoundRect(*g.canvas, x, y, w, h, g.cornerR, bg);
    const int tw = textW(label);
    const int th = textH();
    drawLabel(x + std::max(0, (w - tw) / 2), y + (h - th) / 2,
              label, g.colors[MENU_UI_COL_TEXT]);

    g.nextX = x + w;
    advance();
    return clicked;
}

bool sliderCore(const char* label, const char* valueText, float* norm, uint32_t id) {
    if (!canDrawContent() || !norm) {
        return false;
    }
    const int x = g.cx;
    const int y = g.cy;
    const int w = contentW();
    const int labelH = g.lineH / 2 + 2;
    const int trackY = y + labelH + 8;
    const int trackH = 5;
    const int grabR = 9;
    const int totalH = labelH + 24;
    bool changed = false;

    if (hitTest(x - grabR, y, w + grabR * 2, totalH)) {
        g.hot = id;
        g.captured = g.captured || g.touch.down || g.touch.pressed || g.touch.released;
        if (g.touch.pressed) {
            g.active = id;
        }
    }

    if (g.active == id && g.touch.down) {
        float t = static_cast<float>(g.touch.x - x) / static_cast<float>(std::max(1, w));
        t = std::clamp(t, 0.0f, 1.0f);
        if (t != *norm) {
            *norm = t;
            changed = true;
        }
        g.captured = true;
    }

    const int th = textH();
    drawLabel(x, y + (labelH - th) / 2, label, g.colors[MENU_UI_COL_TEXT]);
    const int vw = textW(valueText);
    drawLabel(x + w - vw, y + (labelH - th) / 2,
              valueText, g.colors[MENU_UI_COL_TEXT_DIM]);

    draw::fillRoundRect(*g.canvas, x, trackY, w, trackH, trackH / 2,
                        g.colors[MENU_UI_COL_SLIDER_TRACK]);
    const int fillW = static_cast<int>(*norm * w);
    if (fillW > 0) {
        draw::fillRoundRect(*g.canvas, x, trackY, fillW, trackH, trackH / 2,
                            g.colors[MENU_UI_COL_SLIDER_GRAB]);
    }
    const int gx = x + fillW;
    const int gy = trackY + trackH / 2;
    draw::fillCircle(*g.canvas, gx, gy, grabR, g.colors[MENU_UI_COL_SLIDER_GRAB]);
    draw::circle(*g.canvas, gx, gy, grabR, g.colors[MENU_UI_COL_TEXT]);

    g.prevCy = g.cy;
    g.nextX = x + w;
    g.cy += totalH + g.spacing;
    g.cx = g.wx + g.padding;
    return changed;
}

bool sliderFloat(const char* label, float* value, float min, float max, int decimals) {
    if (!canDrawContent() || !value) {
        return false;
    }
    if (max < min) {
        std::swap(min, max);
    }
    *value = std::clamp(*value, min, max);
    float norm = max > min ? (*value - min) / (max - min) : 0.0f;
    char buf[48];
    snprintf(buf, sizeof(buf), "%.*f", std::clamp(decimals, 0, 4), *value);
    if (sliderCore(label, buf, &norm, widgetId(label))) {
        *value = min + norm * (max - min);
        return true;
    }
    return false;
}

bool sliderInt(const char* label, int* value, int min, int max) {
    if (!canDrawContent() || !value) {
        return false;
    }
    if (max < min) {
        std::swap(min, max);
    }
    *value = std::clamp(*value, min, max);
    float norm = max > min
            ? static_cast<float>(*value - min) / static_cast<float>(max - min)
            : 0.0f;
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", *value);
    if (sliderCore(label, buf, &norm, widgetId(label))) {
        const int next = min + static_cast<int>(norm * static_cast<float>(max - min) + 0.5f);
        *value = std::clamp(next, min, max);
        return true;
    }
    return false;
}

void text(const char* str) {
    textColored(g.colors[MENU_UI_COL_TEXT], str);
}

void textColored(uint32_t color, const char* str) {
    if (!canDrawContent()) {
        return;
    }
    const int th = textH();
    drawLabel(g.cx, g.cy + (g.lineH - th) / 2, str, color);
    g.nextX = g.cx + textW(str);
    advance();
}

void separator() {
    if (!canDrawContent()) {
        return;
    }
    const int y = g.cy + g.spacing;
    draw::line(*g.canvas, g.cx, y, g.wx + g.ww - g.padding, y,
               g.colors[MENU_UI_COL_SEPARATOR]);
    g.prevCy = g.cy;
    g.cy += g.spacing * 3;
    g.cx = g.wx + g.padding;
}

void sameLine() {
    if (!canDrawContent()) {
        return;
    }
    g.cy = g.prevCy;
    g.cx = g.nextX + g.spacing;
}

bool beginTabBar(const char* id) {
    if (!canDrawContent()) {
        return false;
    }
    g.tabBarId = widgetId(id);
    g.tabX = g.cx;
    g.tabIndex = 0;
    g.tabItems.clear();
    if (s_tabs.find(g.tabBarId) == s_tabs.end()) {
        s_tabs[g.tabBarId] = 0;
    }
    return true;
}

bool tabItem(const char* label) {
    if (!canDrawContent() || g.tabBarId == 0) {
        return false;
    }
    const int idx = g.tabIndex++;
    const int tabPad = std::max(12, g.fontH * 3 / 4);
    const int tabH = std::max(1, g.lineH - g.fontH / 7);
    const int tabW = textW(label) + tabPad;
    const int x = g.tabX;
    const int y = g.cy;
    const uint32_t id = widgetId(label) ^ g.tabBarId;
    bool pressed = false;
    if (hitTest(x, y, tabW, tabH)) {
        g.hot = id;
        g.captured = g.captured || g.touch.down || g.touch.pressed || g.touch.released;
        if (g.touch.pressed) {
            g.active = id;
        }
        if (g.touch.down && g.active == id) {
            pressed = true;
        }
        if (g.touch.released && (g.prevActive == id || g.active == id)) {
            s_tabs[g.tabBarId] = idx;
        }
    }

    g.tabItems.push_back(TabDrawItem {
            label ? label : "",
            idx,
            x,
            y,
            tabW,
            tabH,
            tabPad,
            pressed
    });
    g.tabX += tabW + std::max(1, g.fontH / 7);
    return s_tabs[g.tabBarId] == idx;
}

void endTabBar() {
    if (!canDrawContent() || g.tabBarId == 0) {
        return;
    }
    for (const TabDrawItem& item : g.tabItems) {
        const bool selected = s_tabs[g.tabBarId] == item.index;
        const uint32_t bg = selected ? g.colors[MENU_UI_COL_BUTTON]
                : item.pressed ? g.colors[MENU_UI_COL_BUTTON_ACTIVE]
                               : g.colors[MENU_UI_COL_SLIDER_TRACK];
        draw::fillRoundRect(*g.canvas, item.x, item.y, item.w, item.h, g.cornerR, bg);
        draw::roundRect(*g.canvas, item.x, item.y, item.w, item.h, g.cornerR,
                        selected ? g.colors[MENU_UI_COL_SLIDER_GRAB]
                                 : g.colors[MENU_UI_COL_BORDER]);
        const int th = textH();
        drawLabel(item.x + item.pad / 2, item.y + (item.h - th) / 2,
                  item.label.c_str(),
                  selected ? g.colors[MENU_UI_COL_TEXT] : g.colors[MENU_UI_COL_TEXT_DIM]);
    }
    g.tabBarId = 0;
    g.tabItems.clear();
    g.prevCy = g.cy;
    g.cy += g.lineH + g.spacing;
    g.cx = g.wx + g.padding;
    g.nextX = g.cx;
}

bool isTouchCaptured() {
    return g.captured || g.active != 0 || g.prevActive != 0 || g.dragWinId != 0;
}

void setColor(int idx, uint32_t color) {
    if (idx >= 0 && idx < MENU_UI_COL_COUNT) {
        g.colors[idx] = color;
    }
}

}  // namespace menu_ui
