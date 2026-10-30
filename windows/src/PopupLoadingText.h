#pragma once

#include "UITheme.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace popup {

// A skeleton-like shimmer confined to the source glyphs. Cache the glyph mask
// while waiting: animation frames only recolor pixels, never reflow/move text.
class LoadingText {
public:
    LoadingText() = default;
    LoadingText(const LoadingText&) = delete;
    LoadingText& operator=(const LoadingText&) = delete;
    ~LoadingText() { clear(); }

    void clear() {
        if (dc_) {
            SelectObject(dc_, oldBitmap_);
            DeleteObject(bitmap_);
            DeleteDC(dc_);
        }
        dc_ = nullptr;
        bitmap_ = nullptr;
        pixels_ = nullptr;
        mask_.clear();
        text_.clear();
    }

    // The soft wave has zero intensity at either end of its cycle, avoiding a
    // visible jump when it wraps. Coordinates are logical/DPI-independent.
    static double intensity(double x, double y, double width, ULONGLONG elapsed) {
        constexpr double pi = 3.14159265358979323846;
        const double band = std::clamp(width * 0.24, 64.0, 120.0);
        const double phase = static_cast<double>(elapsed % 2400) / 2400.0;
        const double center = -band * 2 + (width + band * 4) * phase;
        const double ripple = std::sin(y / 40.0 - phase * 2 * pi) * 12.0;
        const double distance = std::abs(x - center - ripple) / band;
        return distance < 1.0 ? (1.0 + std::cos(distance * pi)) * 0.5 : 0.0;
    }

    void draw(HDC target, const std::wstring& text, HFONT font, const RECT& bounds,
              const RECT& viewport, UINT dpi, const ui::Palette& palette,
              bool animated, ULONGLONG elapsed) {
        if (!animated) {
            SelectObject(target, font);
            SetBkMode(target, TRANSPARENT);
            SetTextColor(target, palette.secondary);
            RECT r = bounds;
            DrawTextW(target, text.c_str(), -1, &r, DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
            return;
        }
        const int width = viewport.right - viewport.left;
        const int height = viewport.bottom - viewport.top;
        if (width <= 0 || height <= 0) return;
        RECT local{bounds.left - viewport.left, bounds.top - viewport.top,
                   bounds.right - viewport.left, bounds.bottom - viewport.top};
        if (!dc_ || width != width_ || height != height_ || font != font_ ||
            text != text_ || !EqualRect(&local, &bounds_)) {
            clear();
            dc_ = CreateCompatibleDC(target);
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = width;
            info.bmiHeader.biHeight = -height;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            bitmap_ = CreateDIBSection(target, &info, DIB_RGB_COLORS,
                                      reinterpret_cast<void**>(&pixels_), nullptr, 0);
            if (!dc_ || !bitmap_) {
                if (bitmap_) DeleteObject(bitmap_);
                if (dc_) DeleteDC(dc_);
                dc_ = nullptr;
                bitmap_ = nullptr;
                draw(target, text, font, bounds, viewport, dpi, palette, false, elapsed);
                return;
            }
            oldBitmap_ = SelectObject(dc_, bitmap_);
            width_ = width; height_ = height; font_ = font; text_ = text; bounds_ = local;
            PatBlt(dc_, 0, 0, width, height, BLACKNESS);
            SelectObject(dc_, font);
            SetBkMode(dc_, TRANSPARENT);
            SetTextColor(dc_, RGB(255, 255, 255));
            DrawTextW(dc_, text.c_str(), -1, &local, DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
            GdiFlush();
            mask_.resize(static_cast<size_t>(width) * height);
            for (size_t i = 0; i < mask_.size(); ++i) {
                const auto p = pixels_[i];
                // Retain antialiased coverage without colored ClearType edges.
                mask_[i] = static_cast<uint8_t>(((p & 255) + ((p >> 8) & 255) + ((p >> 16) & 255)) / 3);
            }
        }
        const auto mix = [](COLORREF a, COLORREF b, double t) {
            const auto channel = [t](int x, int y) { return static_cast<int>(x + (y - x) * t + 0.5); };
            return RGB(channel(GetRValue(a), GetRValue(b)), channel(GetGValue(a), GetGValue(b)),
                       channel(GetBValue(a), GetBValue(b)));
        };
        const COLORREF base = mix(palette.surface, palette.secondary, 0.72);
        const COLORREF crest = mix(palette.secondary, ui::Accent, 0.72);
        const double scale = 96.0 / std::max(1u, dpi);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const size_t i = static_cast<size_t>(y) * width + x;
                COLORREF color = palette.surface;
                if (mask_[i]) {
                    const COLORREF ink = mix(base, crest, intensity(x * scale, y * scale, width * scale, elapsed));
                    color = mix(palette.surface, ink, mask_[i] / 255.0);
                }
                pixels_[i] = (static_cast<uint32_t>(GetRValue(color)) << 16) |
                             (static_cast<uint32_t>(GetGValue(color)) << 8) | GetBValue(color);
            }
        }
        BitBlt(target, viewport.left, viewport.top, width, height, dc_, 0, 0, SRCCOPY);
        // Ensure GDI has consumed the DIB before the next CPU recoloring pass.
        GdiFlush();
    }

private:
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ oldBitmap_ = nullptr;
    uint32_t* pixels_ = nullptr;
    int width_ = 0, height_ = 0;
    HFONT font_ = nullptr;
    RECT bounds_{};
    std::wstring text_;
    std::vector<uint8_t> mask_;
};

} // namespace popup
