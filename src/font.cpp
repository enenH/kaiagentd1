#include "font.h"

#include "native_log.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <cstdio>
#include <cstring>
#include <utility>

namespace {

void buildOutlineAlpha(GlyphBitmap& glyph) {
    if (glyph.w <= 0 || glyph.h <= 0 || glyph.alpha.empty()) {
        return;
    }

    const int ow = glyph.w + 2;
    const int oh = glyph.h + 2;
    glyph.outlineAlpha.assign(ow * oh, 0);

    for (int row = 0; row < glyph.h; ++row) {
        const uint8_t* src = glyph.alpha.data() + row * glyph.w;
        for (int col = 0; col < glyph.w; ++col) {
            const uint8_t a = src[col];
            if (a == 0) {
                continue;
            }

            for (int dy = 0; dy < 3; ++dy) {
                uint8_t* dst = glyph.outlineAlpha.data() + (row + dy) * ow + col;
                if (dst[0] < a) dst[0] = a;
                if (dst[1] < a) dst[1] = a;
                if (dst[2] < a) dst[2] = a;
            }
        }
    }
}

void blendAlphaBitmap(uint32_t* pixels, int bstride, int cw, int ch,
                      int x, int y, int w, int h, const uint8_t* alpha,
                      uint8_t sr, uint8_t sg, uint8_t sb, uint8_t ca) {
    if (!pixels || !alpha || w <= 0 || h <= 0 || ca == 0) {
        return;
    }

    for (int row = 0; row < h; ++row) {
        const int py = y + row;
        if (py < 0 || py >= ch) {
            continue;
        }

        uint32_t* dst = pixels + py * bstride;
        const uint8_t* src = alpha + row * w;

        for (int col = 0; col < w; ++col) {
            const int px = x + col;
            if (static_cast<unsigned>(px) >= static_cast<unsigned>(cw)) {
                continue;
            }

            const uint8_t a = src[col];
            if (a == 0) {
                continue;
            }

            const uint8_t fa = static_cast<uint8_t>((a * ca) / 255);
            if (fa == 0) {
                continue;
            }

            if (fa == 255) {
                dst[px] = rgba(sr, sg, sb, 255);
            } else {
                const uint32_t d = dst[px];
                const uint16_t inv = 255 - fa;
                dst[px] = rgba(
                        static_cast<uint8_t>((sr * fa + colorR(d) * inv) / 255),
                        static_cast<uint8_t>((sg * fa + colorG(d) * inv) / 255),
                        static_cast<uint8_t>((sb * fa + colorB(d) * inv) / 255),
                        static_cast<uint8_t>(fa + (colorA(d) * inv) / 255));
            }
        }
    }
}

}  // namespace

Font::~Font() {
    reset();
}

void Font::reset() {
    if (face_) {
        FT_Done_Face(reinterpret_cast<FT_Face>(face_));
        face_ = nullptr;
    }
    if (ftLib_) {
        FT_Done_FreeType(reinterpret_cast<FT_Library>(ftLib_));
        ftLib_ = nullptr;
    }
    fontData_.clear();
    glyphCache_.clear();
    sizePx_ = 0.0f;
    ascent_ = 0;
    descent_ = 0;
    lineGap_ = 0;
    loaded_ = false;
}

bool Font::loadFromFile(const char* ttfPath, float sizePx) {
    FILE* fp = fopen(ttfPath, "rb");
    if (!fp) {
        LOGE("Font: cannot open %s", ttfPath);
        return false;
    }

    fseek(fp, 0, SEEK_END);
    const long fileSize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (fileSize <= 0) {
        fclose(fp);
        return false;
    }

    std::vector<uint8_t> data(static_cast<size_t>(fileSize));
    const size_t readSize = fread(data.data(), 1, data.size(), fp);
    fclose(fp);

    if (readSize != data.size()) {
        LOGE("Font: cannot read %s", ttfPath);
        return false;
    }

    return loadFromMemory(data.data(), data.size(), sizePx);
}

bool Font::loadFromMemory(const uint8_t* data, size_t len, float sizePx) {
    if (!data || len == 0 || sizePx <= 0.0f) {
        return false;
    }

    reset();
    fontData_.assign(data, data + len);
    sizePx_ = sizePx;

    FT_Library lib = nullptr;
    if (FT_Init_FreeType(&lib)) {
        LOGE("Font: FT_Init_FreeType failed");
        reset();
        return false;
    }
    ftLib_ = reinterpret_cast<FT_LibraryRec_*>(lib);

    FT_Face face = nullptr;
    if (FT_New_Memory_Face(lib, fontData_.data(), static_cast<FT_Long>(fontData_.size()),
                           0, &face)) {
        LOGE("Font: FT_New_Memory_Face failed");
        reset();
        return false;
    }
    face_ = reinterpret_cast<FT_FaceRec_*>(face);

    if (FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(sizePx))) {
        LOGE("Font: FT_Set_Pixel_Sizes failed");
        reset();
        return false;
    }

    ascent_ = static_cast<int>(face->size->metrics.ascender >> 6);
    descent_ = static_cast<int>(face->size->metrics.descender >> 6);
    lineGap_ = static_cast<int>((face->size->metrics.height >> 6) - ascent_ + descent_);
    loaded_ = true;
    LOGD("Font loaded: size=%.1f", sizePx);
    return true;
}

uint32_t Font::decodeUtf8(const char*& p) {
    const auto c = static_cast<uint8_t>(*p++);
    if (c < 0x80) {
        return c;
    }

    uint32_t cp = '?';
    if ((c & 0xE0) == 0xC0) {
        cp = c & 0x1F;
        if ((*p & 0xC0) == 0x80) cp = (cp << 6) | (*p++ & 0x3F);
    } else if ((c & 0xF0) == 0xE0) {
        cp = c & 0x0F;
        for (int i = 0; i < 2 && (*p & 0xC0) == 0x80; ++i) {
            cp = (cp << 6) | (*p++ & 0x3F);
        }
    } else if ((c & 0xF8) == 0xF0) {
        cp = c & 0x07;
        for (int i = 0; i < 3 && (*p & 0xC0) == 0x80; ++i) {
            cp = (cp << 6) | (*p++ & 0x3F);
        }
    }
    return cp;
}

const GlyphBitmap* Font::getGlyph(uint32_t codepoint) {
    auto it = glyphCache_.find(codepoint);
    if (it != glyphCache_.end()) {
        return &it->second;
    }

    GlyphBitmap glyph;
    FT_Face face = reinterpret_cast<FT_Face>(face_);
    if (!face || FT_Load_Char(face, codepoint, FT_LOAD_RENDER)) {
        auto [ins, ignored] = glyphCache_.emplace(codepoint, std::move(glyph));
        (void)ignored;
        return &ins->second;
    }

    FT_GlyphSlot slot = face->glyph;
    FT_Bitmap& bmp = slot->bitmap;

    glyph.advance = static_cast<int>(slot->advance.x >> 6);
    glyph.w = static_cast<int>(bmp.width);
    glyph.h = static_cast<int>(bmp.rows);
    glyph.xoff = slot->bitmap_left;
    glyph.yoff = -slot->bitmap_top;

    if (glyph.w > 0 && glyph.h > 0) {
        glyph.alpha.resize(static_cast<size_t>(glyph.w * glyph.h));
        for (int row = 0; row < glyph.h; ++row) {
            memcpy(glyph.alpha.data() + row * glyph.w,
                   bmp.buffer + row * bmp.pitch,
                   static_cast<size_t>(glyph.w));
        }
        buildOutlineAlpha(glyph);
    }

    auto [ins, ignored] = glyphCache_.emplace(codepoint, std::move(glyph));
    (void)ignored;
    return &ins->second;
}

void Font::drawText(Canvas& canvas, int x, int y, const char* utf8, uint32_t color) {
    drawTextImpl(canvas, x, y, utf8, color, false);
}

void Font::drawTextBlackOutline(Canvas& canvas, int x, int y, const char* utf8,
                                uint32_t color) {
    drawTextImpl(canvas, x, y, utf8, color, true);
}

void Font::drawTextImpl(Canvas& canvas, int x, int y, const char* utf8, uint32_t color,
                        bool blackOutline) {
    if (!loaded_ || !utf8 || !canvas.isLocked()) {
        return;
    }

    uint32_t* pixels = canvas.pixels();
    const int bstride = canvas.stride();
    const int cw = canvas.width();
    const int ch = canvas.height();
    int baseline = y + ascent_;
    int penX = x;

    const uint8_t sr = colorR(color);
    const uint8_t sg = colorG(color);
    const uint8_t sb = colorB(color);
    const uint8_t ca = colorA(color);

    const char* p = utf8;
    while (*p) {
        const uint32_t cp = decodeUtf8(p);
        if (cp == '\n') {
            penX = x;
            baseline += ascent_ - descent_ + lineGap_;
            continue;
        }

        const GlyphBitmap* glyph = getGlyph(cp);
        if (!glyph) {
            continue;
        }
        if (glyph->w == 0 || glyph->h == 0) {
            penX += glyph->advance;
            continue;
        }

        const int gx0 = penX + glyph->xoff;
        const int gy0 = baseline + glyph->yoff;

        if (blackOutline && !glyph->outlineAlpha.empty()) {
            blendAlphaBitmap(pixels, bstride, cw, ch, gx0 - 1, gy0 - 1,
                             glyph->w + 2, glyph->h + 2, glyph->outlineAlpha.data(),
                             0, 0, 0, ca);
        }
        blendAlphaBitmap(pixels, bstride, cw, ch, gx0, gy0,
                         glyph->w, glyph->h, glyph->alpha.data(), sr, sg, sb, ca);
        penX += glyph->advance;
    }
}

int Font::measureText(const char* utf8) {
    if (!loaded_ || !utf8) {
        return 0;
    }

    int maxW = 0;
    int penX = 0;
    const char* p = utf8;
    while (*p) {
        const uint32_t cp = decodeUtf8(p);
        if (cp == '\n') {
            if (penX > maxW) {
                maxW = penX;
            }
            penX = 0;
            continue;
        }

        const GlyphBitmap* glyph = getGlyph(cp);
        if (glyph) {
            penX += glyph->advance;
        }
    }
    return penX > maxW ? penX : maxW;
}
