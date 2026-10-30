#include "PopupWindow.h"
#include "Hooks.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

// Exercise the actual popup without installing global hooks, touching the
// clipboard, starting HTTP requests, or loading the user's settings.
namespace hooks {
int AddMouse(MouseFn) { return 1; }
int AddKey(KeyFn) { return 2; }
void RemoveMouse(int) {}
void RemoveKey(int) {}
void SetSwallowKey(DWORD, int, const std::atomic<bool>*) {}
}
namespace clipboard { bool SetText(const std::wstring&) { return true; } }
namespace util { void Log(const char*, ...) {} }

namespace {
bool passed = true;
void check(bool ok, const char* message) {
    if (!ok) { std::printf("FAIL: %s\n", message); passed = false; }
}

struct Canvas {
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = nullptr;
    HGDIOBJ old = nullptr;
    uint32_t* pixels = nullptr;
    int width, height;
    Canvas(int w, int h) : width(w), height(h) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w; info.bmiHeader.biHeight = -h;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels), nullptr, 0);
        old = SelectObject(dc, bitmap);
    }
    ~Canvas() { SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc); }
    std::vector<uint32_t> snapshot() {
        GdiFlush();
        return {pixels, pixels + static_cast<size_t>(width) * height};
    }
    void fill(COLORREF color) {
        RECT r{0, 0, width, height};
        auto brush = CreateSolidBrush(color);
        FillRect(dc, &r, brush); DeleteObject(brush);
    }
    void save(const std::filesystem::path& path) {
        GdiFlush();
        BITMAPFILEHEADER file{};
        file.bfType = 0x4D42;
        file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
        file.bfSize = file.bfOffBits + width * height * 4;
        BITMAPINFOHEADER info{};
        info.biSize = sizeof(info); info.biWidth = width; info.biHeight = -height;
        info.biPlanes = 1; info.biBitCount = 32;
        std::ofstream stream(path, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(&file), sizeof(file));
        stream.write(reinterpret_cast<const char*>(&info), sizeof(info));
        stream.write(reinterpret_cast<const char*>(pixels), width * height * 4);
    }
};

void renderTests() {
    for (UINT dpi : {96u, 144u, 192u}) {
        auto px = [dpi](int v) { return MulDiv(v, dpi, 96); };
        Canvas canvas(px(340), px(120));
        HFONT font = CreateFontW(-MulDiv(11, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        RECT bounds{px(12), px(12), px(328), px(110)};
        const std::wstring text = L"Waiting for a response & keeping text still.\n正在等待翻译响应，文字水波纹。 العربية";
        for (bool dark : {false, true}) {
            const auto palette = ui::palette(dark);
            const uint32_t bg = (GetRValue(palette.surface) << 16) |
                                (GetGValue(palette.surface) << 8) | GetBValue(palette.surface);
            popup::LoadingText effect;
            canvas.fill(palette.surface);
            effect.draw(canvas.dc, text, font, bounds, bounds, dpi, palette, true, 0);
            const auto start = canvas.snapshot();
            effect.draw(canvas.dc, text, font, bounds, bounds, dpi, palette, true, 1100);
            const auto middle = canvas.snapshot();
            int changed = 0, ink = 0, stray = 0;
            for (size_t i = 0; i < start.size(); ++i) {
                if ((start[i] & 0xFFFFFF) != bg) ++ink;
                if (start[i] != middle[i]) ++changed;
                if ((start[i] & 0xFFFFFF) == bg && (middle[i] & 0xFFFFFF) != bg) ++stray;
            }
            check(ink > 100, "waiting source glyphs are visible at every DPI/theme");
            check(changed > 100, "wave changes glyph brightness over time");
            check(stray == 0, "no moving dots, glyph displacement, or ink outside source text");
            effect.draw(canvas.dc, text, font, bounds, bounds, dpi, palette, true, 2399);
            check(start == canvas.snapshot(), "wave wraps without a visible discontinuity");
            canvas.fill(palette.surface);
            effect.draw(canvas.dc, text, font, bounds, bounds, dpi, palette, false, 0);
            const auto reduced = canvas.snapshot();
            canvas.fill(palette.surface);
            effect.draw(canvas.dc, text, font, bounds, bounds, dpi, palette, false, 1100);
            check(reduced == canvas.snapshot(), "reduced motion leaves text completely static");
            // Rebuild cached glyphs after scrolling/resizing or changing text.
            RECT scrolled = bounds; scrolled.top -= px(24);
            effect.draw(canvas.dc, text + L" updated", font, scrolled, bounds, dpi, palette, true, 1000);
            effect.clear();
            effect.clear();
        }
        SelectObject(canvas.dc, GetStockObject(SYSTEM_FONT));
        DeleteObject(font);
    }
    for (int t = 0; t < 2400; t += 17) {
        const double level = popup::LoadingText::intensity(150, 60, 320, t);
        check(std::isfinite(level) && level >= 0 && level <= 1, "wave intensity remains bounded");
    }
}

void streamFadeTests() {
    popup::StreamFade fade;
    fade.append(0, 6, 1000);
    check(fade.active(1000), "received text starts a bounded fade");
    check(fade.opacity(0, 1, 1000) >= 0.18, "new text is visible immediately without a typewriter queue");
    double previous = fade.opacity(3, 1, 1000);
    for (ULONGLONG now = 1001; now < 1300; ++now) {
        const double next = fade.opacity(3, 1, now);
        check(next >= previous && next <= 1.0, "glyph opacity increases monotonically to full contrast");
        previous = next;
    }
    check(!fade.active(1300) && fade.opacity(0, 6, 1300) == 1.0,
          "a stalled stream leaves no animation or invisible backlog after 300ms");
    fade.prune(1300);
    fade.append(6, 12, 1500);
    check(fade.opacity(0, 6, 1500) == 1.0, "old text never fades again when a new batch arrives");
    check(fade.opacity(5, 2, 1500) < 1.0, "a cluster spanning received batches fades as one unit");
    fade.clear();
    check(!fade.active(1500) && fade.pendingBatches() == 0, "retry/close discards all fade history");

    popup::StreamFade fast, slow;
    for (size_t i = 0; i < 40; ++i) {
        fast.append(i * 20, (i + 1) * 20, 1000 + i * 33);
        slow.append(i, i + 1, 1000 + i * 300);
    }
    check(fast.opacity(780, 1, 1000 + 39 * 33 + 70) > slow.opacity(39, 1, 1000 + 39 * 300 + 70),
          "high throughput uses shorter fades than slow token output");
    check(fast.pendingBatches() <= 9, "fast output prunes settled batches instead of accumulating a playback queue");
    check(!fast.active(1000 + 39 * 33 + 300) && !slow.active(1000 + 39 * 300 + 300),
          "fast and slow output both catch up within the same bounded tail");
    popup::StreamFade burst;
    burst.append(0, 100000, 1000);
    check(burst.opacity(99999, 1, 1000) >= 0.18 && !burst.active(1300),
          "a huge cached/buffered response does not create per-character delays");
}

void streamRenderTests() {
    for (UINT dpi : {96u, 144u, 192u}) {
        auto px = [dpi](int v) { return MulDiv(v, dpi, 96); };
        Canvas canvas(px(340), px(170));
        HFONT font = CreateFontW(-MulDiv(11, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        const std::wstring stable = L"Already visible and fully settled.\n";
        const std::wstring text = stable + L"新文字逐渐显现。 office e\u0301 \U0001F469\u200D\U0001F4BB العربية हिंदी";
        RECT bounds{px(12), px(12), px(328), px(160)};
        for (bool dark : {false, true}) {
            const auto palette = ui::palette(dark);
            popup::StreamText renderer;
            popup::StreamFade fade;
            fade.append(stable.size(), text.size(), 1000);
            const int measured = renderer.measure(text, font, bounds.right - bounds.left);
            check(measured > px(20), "full paragraph is measured before fade begins at every DPI");
            canvas.fill(palette.surface);
            check(renderer.draw(canvas.dc, text, font, bounds, bounds, palette.text, palette.surface, fade, 1000, true),
                  "DirectWrite renders mixed scripts and grapheme clusters");
            const auto start = canvas.snapshot();
            check(renderer.draw(canvas.dc, text, font, bounds, bounds, palette.text, palette.surface, fade, 1050, true),
                  "stream fades render an intermediate frame");
            const auto middle = canvas.snapshot();
            check(renderer.draw(canvas.dc, text, font, bounds, bounds, palette.text, palette.surface, fade, 1300, true),
                  "stream fades reach a fully settled frame");
            const auto end = canvas.snapshot();
            check(start != middle && middle != end, "only new text progressively gains contrast");
            bool stableLine = true;
            for (int y = bounds.top; y < bounds.top + px(16); ++y) {
                for (int x = bounds.left; x < bounds.right; ++x) {
                    const size_t i = static_cast<size_t>(y) * canvas.width + x;
                    if (start[i] != end[i]) stableLine = false;
                }
            }
            check(stableLine, "previously settled line does not flicker as new glyphs fade");
            renderer.draw(canvas.dc, text, font, bounds, bounds, palette.text, palette.surface, fade, 1000, false);
            check(end == canvas.snapshot(), "reduced-motion rendering immediately matches the final shaped text");
            check(measured == renderer.measure(text, font, bounds.right - bounds.left),
                  "fade does not change wrapping or measured geometry");
            RECT scrolled = bounds; scrolled.top -= px(24);
            check(renderer.draw(canvas.dc, text, font, scrolled, bounds, palette.text, palette.surface, fade, 1060, true),
                  "scrolling during fade preserves full paragraph layout");
            renderer.clear();
        }
        DeleteObject(font);
    }
}

void pump(unsigned duration) {
    const auto end = GetTickCount64() + duration;
    do {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        if (GetTickCount64() >= end) break;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    } while (true);
}

void capture(HWND hwnd, const std::filesystem::path& path) {
    UpdateWindow(hwnd);
    RECT r{}; GetClientRect(hwnd, &r);
    Canvas image(r.right, r.bottom);
    HDC dc = GetDC(hwnd);
    BitBlt(image.dc, 0, 0, r.right, r.bottom, dc, 0, 0, SRCCOPY);
    ReleaseDC(hwnd, dc);
    image.save(path);
}

void lifecycleTests(const std::filesystem::path& preview) {
    auto& popup = PopupWindow::shared();
    RECT anchor{120, 100, 480, 124};
    const std::wstring source = L"Good design keeps the interface calm while waiting for a response.\n"
                                L"让等待更安静，让文字泛起轻柔的水波纹。";
    popup.show(source, Language::Chinese, anchor, true);
    HWND hwnd = FindWindowW(L"TypeTidePopup", nullptr);
    check(hwnd != nullptr, "real translation popup opens in waiting state");
    if (!hwnd) return;
    UpdateWindow(hwnd);
    BOOL animations = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    ValidateRect(hwnd, nullptr);
    SendMessageW(hwnd, WM_TIMER, 2, 0);
    check((GetUpdateRect(hwnd, nullptr, FALSE) != FALSE) == (animations && !ui::highContrastEnabled()),
          "only animated waiting state invalidates on loading timer");
    popup.appendDelta(L"");
    check(popup.translation().empty(), "empty delta does not end the waiting state");
    if (!preview.empty()) {
        std::filesystem::create_directories(preview);
        for (int i = 0; i < 30; ++i) {
            pump(80);
            capture(hwnd, preview / ("waiting-" + std::to_string(i) + ".bmp"));
        }
    }
    popup.appendDelta(L"好的设计让等待响应的界面保持宁静。");
    ValidateRect(hwnd, nullptr);
    SendMessageW(hwnd, WM_TIMER, 2, 0);
    check(!GetUpdateRect(hwnd, nullptr, FALSE), "first translated delta stops waiting animation immediately");
    popup.appendDelta(L"文字以流式正常呈现。");
    check(popup.translation() == L"好的设计让等待响应的界面保持宁静。文字以流式正常呈现。",
          "streamed translation assembly is unchanged");
    popup.finishTranslation();
    ValidateRect(hwnd, nullptr);
    SendMessageW(hwnd, WM_TIMER, 2, 0);
    check(!GetUpdateRect(hwnd, nullptr, FALSE), "completion has no trailing particle/settling animation");
    ValidateRect(hwnd, nullptr);
    SendMessageW(hwnd, WM_TIMER, 3, 0);
    check((GetUpdateRect(hwnd, nullptr, FALSE) != FALSE) == (animations && !ui::highContrastEnabled()),
          "completion lets the last received text finish its short fade");
    pump(320);
    ValidateRect(hwnd, nullptr);
    SendMessageW(hwnd, WM_TIMER, 3, 0);
    check(!GetUpdateRect(hwnd, nullptr, FALSE), "settled translation stops the fade timer completely");
    if (!preview.empty()) {
        InvalidateRect(hwnd, nullptr, FALSE);
        capture(hwnd, preview / "complete.bmp");
    }
    popup.resetForRetranslate();
    check(popup.translation().empty(), "retry clears old translation");
    ValidateRect(hwnd, nullptr);
    SendMessageW(hwnd, WM_TIMER, 3, 0);
    check(!GetUpdateRect(hwnd, nullptr, FALSE), "retry cancels stale stream-fade ticks");
    ValidateRect(hwnd, nullptr);
    SendMessageW(hwnd, WM_TIMER, 2, 0);
    check((GetUpdateRect(hwnd, nullptr, FALSE) != FALSE) == (animations && !ui::highContrastEnabled()),
          "retry restarts waiting animation");
    popup.setError(L"Mock network error");
    ValidateRect(hwnd, nullptr);
    SendMessageW(hwnd, WM_TIMER, 2, 0);
    check(!GetUpdateRect(hwnd, nullptr, FALSE), "errors stop waiting animation");
    SendMessageW(hwnd, WM_TIMER, 3, 0);
    check(!GetUpdateRect(hwnd, nullptr, FALSE), "errors stop stream-fade animation");
    popup.close();
    check(!IsWindow(hwnd), "closing destroys waiting popup safely");
    const DWORD resources = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    for (int i = 0; i < 12; ++i) {
        popup.show(source, Language::Chinese, anchor, false);
        popup.appendDelta(L"Resource test 新增译文 \U0001F600");
        UpdateWindow(FindWindowW(L"TypeTidePopup", nullptr));
        popup.close();
    }
    check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= resources,
          "repeated waiting/close cycles release cached GDI resources");
}

void captureStreamPreview(const std::filesystem::path& preview) {
    if (preview.empty()) return;
    auto& popup = PopupWindow::shared();
    RECT anchor{120, 100, 480, 124};
    popup.show(L"Streamed text fades in at the pace of the actual response.", Language::Chinese, anchor, true);
    HWND hwnd = FindWindowW(L"TypeTidePopup", nullptr);
    UpdateWindow(hwnd);
    std::wstring result;
    const std::vector<std::pair<int, std::wstring>> arrivals{
        {4, L"文字"}, {12, L"随"}, {20, L"响应"}, {28, L"速度"}, {36, L"自然"}, {44, L"淡入。\n"},
        {52, L"输出加快时，"}, {55, L"动画同步提速，"}, {58, L"不积压内容，"}, {61, L"也不拖慢译文。"},
        {78, L"\n停顿后继续输出，"}, {84, L"已显示的文字保持清晰。"}};
    size_t next = 0;
    for (int frame = 0; frame < 108; ++frame) {
        if (next < arrivals.size() && frame == arrivals[next].first) {
            const auto& delta = arrivals[next++].second;
            result += delta; popup.appendDelta(delta);
            check(popup.translation() == result, "all response text is available immediately during visual fade");
        }
        if (frame == 85) popup.finishTranslation();
        pump(32);
        capture(hwnd, preview / ("stream-" + std::to_string(frame) + ".bmp"));
    }
    popup.close();
}
}

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    renderTests();
    streamFadeTests();
    streamRenderTests();
    lifecycleTests(argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path{});
    captureStreamPreview(argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path{});
    std::puts(passed ? "All popup loading tests passed" : "Popup loading tests failed");
    return passed ? 0 : 1;
}
