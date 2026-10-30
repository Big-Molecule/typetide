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
    if (!preview.empty()) {
        InvalidateRect(hwnd, nullptr, FALSE);
        capture(hwnd, preview / "complete.bmp");
    }
    popup.resetForRetranslate();
    check(popup.translation().empty(), "retry clears old translation");
    ValidateRect(hwnd, nullptr);
    SendMessageW(hwnd, WM_TIMER, 2, 0);
    check((GetUpdateRect(hwnd, nullptr, FALSE) != FALSE) == (animations && !ui::highContrastEnabled()),
          "retry restarts waiting animation");
    popup.setError(L"Mock network error");
    ValidateRect(hwnd, nullptr);
    SendMessageW(hwnd, WM_TIMER, 2, 0);
    check(!GetUpdateRect(hwnd, nullptr, FALSE), "errors stop waiting animation");
    popup.close();
    check(!IsWindow(hwnd), "closing destroys waiting popup safely");
    const DWORD resources = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    for (int i = 0; i < 12; ++i) {
        popup.show(source, Language::Chinese, anchor, false);
        UpdateWindow(FindWindowW(L"TypeTidePopup", nullptr));
        popup.close();
    }
    check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= resources,
          "repeated waiting/close cycles release cached GDI resources");
}
}

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    renderTests();
    lifecycleTests(argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path{});
    std::puts(passed ? "All popup loading tests passed" : "Popup loading tests failed");
    return passed ? 0 : 1;
}
