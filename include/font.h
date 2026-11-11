#ifndef REALTIME_FONT_H
#define REALTIME_FONT_H

#include "overlay_canvas.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

struct FT_LibraryRec_;
struct FT_FaceRec_;

struct GlyphBitmap {
    std::vector<uint8_t> alpha;
    std::vector<uint8_t> outlineAlpha;
    int w = 0;
    int h = 0;
    int xoff = 0;
    int yoff = 0;
    int advance = 0;
};

class Font {
public:
    Font() = default;
    ~Font();

    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;

    bool loadFromFile(const char* ttfPath, float sizePx);
    bool loadFromMemory(const uint8_t* data, size_t len, float sizePx);

    void drawText(Canvas& canvas, int x, int y, const char* utf8, uint32_t color);
    void drawTextBlackOutline(Canvas& canvas, int x, int y, const char* utf8, uint32_t color);
    int measureText(const char* utf8);

    float fontSize() const { return sizePx_; }
    int lineHeight() const { return ascent_ - descent_; }
    int ascent() const { return ascent_; }
    bool isLoaded() const { return loaded_; }

private:
    void reset();
    void drawTextImpl(Canvas& canvas, int x, int y, const char* utf8, uint32_t color,
                      bool blackOutline);
    const GlyphBitmap* getGlyph(uint32_t codepoint);
    static uint32_t decodeUtf8(const char*& p);

    std::vector<uint8_t> fontData_;
    FT_LibraryRec_* ftLib_ = nullptr;
    FT_FaceRec_* face_ = nullptr;
    float sizePx_ = 0.0f;
    int ascent_ = 0;
    int descent_ = 0;
    int lineGap_ = 0;
    bool loaded_ = false;
    std::unordered_map<uint32_t, GlyphBitmap> glyphCache_;
};

#endif  // REALTIME_FONT_H
