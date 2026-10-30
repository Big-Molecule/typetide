#include "PopupStreamText.h"
#include <limits>

namespace popup {

void StreamText::clear() {
    layout_.Reset();
    for (auto& brush : brushes_) brush.Reset();
    target_.Reset();
    clusters_.clear(); text_.clear(); font_ = nullptr; width_ = height_ = 0;
}

bool StreamText::prepare(const std::wstring& text, HFONT font, int width) {
    if (layout_ && text == text_ && font == font_ && width == width_) return true;
    if (width <= 0 || text.size() > std::numeric_limits<UINT32>::max()) return false;
    if (!write_ && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                         reinterpret_cast<IUnknown**>(write_.GetAddressOf())))) return false;
    LOGFONTW lf{};
    if (!GetObjectW(font, sizeof(lf), &lf)) return false;
    Ptr<IDWriteTextFormat> format;
    if (FAILED(write_->CreateTextFormat(lf.lfFaceName, nullptr,
            static_cast<DWRITE_FONT_WEIGHT>(lf.lfWeight),
            lf.lfItalic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, static_cast<float>(std::max(1L, std::abs(lf.lfHeight))),
            L"", format.GetAddressOf()))) return false;
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    Ptr<IDWriteTextLayout> next;
    if (FAILED(write_->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format.Get(),
            static_cast<float>(width), 1000000.0f, next.GetAddressOf()))) return false;
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(next->GetMetrics(&metrics))) return false;
    UINT32 count = 0;
    next->GetClusterMetrics(nullptr, 0, &count);
    std::vector<DWRITE_CLUSTER_METRICS> clusters(count);
    if (count && FAILED(next->GetClusterMetrics(clusters.data(), count, &count))) return false;
    layout_ = next; clusters_ = std::move(clusters);
    text_ = text; font_ = font; width_ = width;
    height_ = std::max(1, static_cast<int>(std::ceil(metrics.height)));
    return true;
}

int StreamText::measure(const std::wstring& text, HFONT font, int width) {
    return prepare(text, font, width) ? height_ : 0;
}

bool StreamText::createTarget() {
    if (target_) return true;
    if (!factory_ && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                             factory_.GetAddressOf()))) return false;
    const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
    if (FAILED(factory_->CreateDCRenderTarget(&properties, target_.GetAddressOf()))) return false;
    for (auto& brush : brushes_) {
        if (FAILED(target_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), brush.GetAddressOf()))) {
            for (auto& created : brushes_) created.Reset();
            target_.Reset(); return false;
        }
    }
    target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    return true;
}

bool StreamText::draw(HDC dc, const std::wstring& text, HFONT font, const RECT& bounds,
                      const RECT& viewport, COLORREF foreground, COLORREF background,
                      const StreamFade& fade, ULONGLONG now, bool animated) {
    if (!prepare(text, font, bounds.right - bounds.left) || !createTarget()) return false;
    if (FAILED(target_->BindDC(dc, &viewport))) return false;
    const auto color = [](COLORREF c) {
        return D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f);
    };
    const auto bg = color(background), fg = color(foreground);
    for (int i = 0; i < 16; ++i) {
        const float alpha = i / 15.0f;
        brushes_[i]->SetColor(D2D1::ColorF(bg.r + (fg.r - bg.r) * alpha,
            bg.g + (fg.g - bg.g) * alpha, bg.b + (fg.b - bg.b) * alpha));
    }
    layout_->SetDrawingEffect(nullptr, {0, static_cast<UINT32>(text.size())});
    if (animated && fade.active(now)) {
        UINT32 at = 0, groupStart = 0, groupLength = 0;
        int previous = 15;
        const auto apply = [&] {
            if (groupLength && previous < 15)
                layout_->SetDrawingEffect(brushes_[previous].Get(), {groupStart, groupLength});
        };
        for (const auto& cluster : clusters_) {
            const int bucket = std::clamp(static_cast<int>(std::lround(fade.opacity(at, cluster.length, now) * 15)), 0, 15);
            if (bucket != previous) { apply(); groupStart = at; groupLength = 0; previous = bucket; }
            groupLength += cluster.length; at += cluster.length;
        }
        apply();
    }
    target_->BeginDraw();
    target_->SetTransform(D2D1::Matrix3x2F::Identity());
    target_->Clear(bg);
    target_->DrawTextLayout(D2D1::Point2F(static_cast<float>(bounds.left - viewport.left),
        static_cast<float>(bounds.top - viewport.top)), layout_.Get(), brushes_[15].Get());
    const HRESULT result = target_->EndDraw();
    if (FAILED(result)) {
        layout_->SetDrawingEffect(nullptr, {0, static_cast<UINT32>(text.size())});
        for (auto& brush : brushes_) brush.Reset();
        target_.Reset();
        return false; // GDI fallback keeps translation readable on renderer failure.
    }
    return true;
}

} // namespace popup
