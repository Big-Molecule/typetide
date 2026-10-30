#pragma once

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

namespace popup {

// Arrival-paced, bounded fades, not a second typewriter queue. All received
// text is available immediately to layout, accessibility, Copy and Replace.
class StreamFade {
public:
    void clear() { spans_.clear(); lastArrival_ = 0; hasArrival_ = false; unitMs_ = 25.0; }
    void append(size_t first, size_t end, ULONGLONG now) {
        if (end <= first) return;
        prune(now);
        const double interval = hasArrival_
            ? static_cast<double>(std::clamp<ULONGLONG>(now - lastArrival_, 1, 500)) : 33.0;
        const double sample = interval / static_cast<double>(end - first);
        unitMs_ = hasArrival_ ? unitMs_ * 0.65 + sample * 0.35 : sample;
        const double duration = std::clamp(80.0 + unitMs_ * 3.5, 90.0, 210.0);
        const double spread = std::clamp(interval * 0.55, 12.0, 55.0);
        spans_.push_back({first, end, now, duration, spread});
        lastArrival_ = now; hasArrival_ = true;
    }
    void prune(ULONGLONG now) {
        spans_.erase(std::remove_if(spans_.begin(), spans_.end(), [now](const Span& s) {
            return static_cast<double>(now - s.arrival) >= s.duration + s.spread;
        }), spans_.end());
    }
    bool active(ULONGLONG now) const {
        return std::any_of(spans_.begin(), spans_.end(), [now](const Span& s) {
            return static_cast<double>(now - s.arrival) < s.duration + s.spread;
        });
    }
    double opacity(size_t first, size_t length, ULONGLONG now) const {
        double result = 1.0;
        for (const auto& s : spans_) {
            if (first + length <= s.first || first >= s.end) continue;
            const size_t at = std::max(first, s.first);
            const double offset = static_cast<double>(at - s.first) /
                                  static_cast<double>(std::max<size_t>(1, s.end - s.first - 1));
            const double elapsed = static_cast<double>(now - s.arrival) - offset * s.spread;
            const double t = std::clamp(elapsed / s.duration, 0.0, 1.0);
            result = std::min(result, 0.18 + 0.82 * t * t * (3.0 - 2.0 * t));
        }
        return result;
    }
    size_t pendingBatches() const { return spans_.size(); }

private:
    struct Span { size_t first, end; ULONGLONG arrival; double duration, spread; };
    std::vector<Span> spans_;
    ULONGLONG lastArrival_ = 0;
    bool hasArrival_ = false;
    double unitMs_ = 25.0;
};

// DirectWrite shapes the complete paragraph once per received batch; animation
// colors whole shaping clusters, never slices UTF-16, ligatures or bidi runs.
// Direct2D's DC target fits the existing double-buffered Win32 popup.
class StreamText {
public:
    void clear();
    int measure(const std::wstring& text, HFONT font, int width);
    bool draw(HDC dc, const std::wstring& text, HFONT font, const RECT& bounds,
              const RECT& viewport, COLORREF foreground, COLORREF background,
              const StreamFade& fade, ULONGLONG now, bool animated);
private:
    bool prepare(const std::wstring& text, HFONT font, int width);
    bool createTarget();
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    Ptr<IDWriteFactory> write_;
    Ptr<ID2D1Factory> factory_;
    Ptr<IDWriteTextLayout> layout_;
    Ptr<ID2D1DCRenderTarget> target_;
    Ptr<ID2D1SolidColorBrush> brushes_[16];
    std::vector<DWRITE_CLUSTER_METRICS> clusters_;
    std::wstring text_;
    HFONT font_ = nullptr;
    int width_ = 0, height_ = 0;
};

} // namespace popup
