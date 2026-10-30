#include "SettingsWindow.h"
#include "SettingsLayout.h"
#include "ModernFormControls.h"
#include "App.h"
#include "LaunchAtLogin.h"
#include "LocalDiagnostics.h"
#include "resource.h"
#include "Settings.h"
#include "Translator.h"
#include "UpdateChecker.h"
#include "Util.h"
#include "UITheme.h"
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <vsstyle.h>
#include <algorithm>
#include <string>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace {

constexpr wchar_t kClassName[] = L"TypeTideSettings";
constexpr UINT kMsgUpdateResult = WM_APP + 101;
constexpr UINT kMsgBackendHealth = WM_APP + 102;
// 工作线程取回云端模型列表：lParam = CloudModels*（接收方释放）
constexpr UINT kMsgCloudModels = WM_APP + 104;

struct CloudModels {
    std::vector<std::wstring> models;
    std::wstring error;
};

enum CtrlId : int {
    kTab = 1000,
    // General
    kEnable, kLogin, kVersionLabel, kCheckUpdate, kFirstRunHint,
    // Backend
    kOaiProviderLabel, kOaiProvider,
    kOaiUrlLabel, kOaiUrl, kOaiKeyLabel, kOaiKey, kOaiModelLabel, kOaiModel, kOaiFetch, kOaiHint, kPrivacyHint,
    kBackendTest, kBackendStatus,
    // Language
    kNativeLabel, kNative, kForeignLabel, kForeign,
    kReadDirLabel, kReadDir, kRewriteDirLabel, kRewriteDir, kLangWarn,
    // Shortcuts
    kHkReadLabel, kHkRead, kReadWin, kHkReadClear,
    kHkRewriteLabel, kHkRewrite, kRewriteWin, kHkRewriteClear,
    kReadActionLabel, kReadAction, kRewriteActionLabel, kRewriteAction,
    kHkRestoreDefaults, kHkVerify, kHkNote, kHkStatus,
    // Behavior
    kTriggerLabel, kTrigger, kPreview,
    kReadStyleLabel, kReadStyle, kRewriteStyleLabel, kRewriteStyle,
    // Skip apps
    kSkipEdit, kSkipAdd, kSkipList, kSkipRemove, kSkipHint,
    // Diagnostics
    kDiagPrivacy, kDiagExport, kDiagClear, kDiagPath,
    // About
    kAboutTitle, kAboutDesc, kAboutLinks,
    // Page headings (must remain contiguous)
    kPageTitle0, kPageTitle1, kPageTitle2, kPageTitle3,
    kPageTitle4, kPageTitle5, kPageTitle6, kPageTitle7,
};

struct State {
    HWND hwnd = nullptr;
    HWND appWindow = nullptr;
    HFONT font = nullptr, fontBold = nullptr, fontTitle = nullptr;
    HIMAGELIST tabImages = nullptr;
    UINT dpi = 96;
    std::vector<std::vector<HWND>> pages;  // 每个 tab 页的控件
    bool loading = false;                  // 初始化填充时不触发保存
    bool saveWarningShown = false;
    bool backendReady = false;
    bool shortcutsReady = false;
    bool dark = false;
    bool highContrast = false;
    HWND hoverButton = nullptr;            // Win11 风格按钮自绘：跟踪悬停
    bool navBackdrop = false;              // Mica 近似背景是否已应用
    RECT backendCard{};                    // Backend 页字段卡片（逻辑坐标，96dpi）
    int currentPage = 0;                   // 当前显示的页（卡片只在 Backend 页绘制）
    std::vector<layout::Page> layouts;      // 每页的声明式布局（与 pages 一一对应）
};

State g;

HWND ctrl(int id);

int px(int v) { return MulDiv(v, (int)g.dpi, 96); }

COLORREF pageBg() { return ui::palette(g.dark, g.highContrast).background; }
COLORREF navBg() { return g.dark ? RGB(0x19, 0x19, 0x1B) : RGB(0xF0, 0xEF, 0xF5); }
COLORREF textColor() { return ui::palette(g.dark, g.highContrast).text; }
COLORREF secondaryColor() { return ui::palette(g.dark, g.highContrast).secondary; }
HBRUSH pageBrush() { static HBRUSH light = CreateSolidBrush(RGB(0xF3, 0xF3, 0xF3)); static HBRUSH dark = CreateSolidBrush(RGB(0x20, 0x20, 0x22)); return g.dark ? dark : light; }
HBRUSH navBrush() { static HBRUSH light = CreateSolidBrush(RGB(0xF0, 0xEF, 0xF5)); static HBRUSH dark = CreateSolidBrush(RGB(0x19, 0x19, 0x1B)); return g.dark ? dark : light; }
HBRUSH editBrush() { static HBRUSH light = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF)); static HBRUSH dark = CreateSolidBrush(RGB(0x2B, 0x2B, 0x2E)); return g.dark ? dark : light; }

bool inBackendCard(HWND control) {
    if (g.currentPage != 1 || g.layouts.size() <= 1) return false;
    const auto& page = g.layouts[1];
    for (int r = page.cardFirst; r >= 0 && r <= page.cardLast; ++r)
        for (const auto& item : page.rows[r].items)
            if (item.hwnd == control) return true;
    return false;
}

HBRUSH controlBackground(HWND control) {
    if (g.highContrast) return GetSysColorBrush(COLOR_WINDOW);
    return inBackendCard(control) ? editBrush() : pageBrush();
}

form::Theme formTheme(HWND control) {
    return {g.dark, g.highContrast,
            inBackendCard(control) ? ui::palette(g.dark, g.highContrast).surface : pageBg()};
}

// ---------- Win11 风格按钮自绘 ----------
// 圆角、Subtle 填充、悬停渐亮、按下变暗、accent 主按钮；
// 明暗两套主题共用一套几何，颜色取自 ui::palette。
//
namespace win11 {

constexpr int kCornerRadius = 6;   // Win11 控件圆角（96dpi 下 4px 边框圆角 + 2px 视觉）
constexpr int kBorderWidth = 1;

COLORREF blend(COLORREF a, COLORREF b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    return RGB(GetRValue(a) + (int)((GetRValue(b) - GetRValue(a)) * t),
               GetGValue(a) + (int)((GetGValue(b) - GetGValue(a)) * t),
               GetBValue(a) + (int)((GetBValue(b) - GetBValue(a)) * t));
}

bool isPrimary(HWND button) {
    wchar_t text[64]{};
    GetWindowTextW(button, text, (int)std::size(text));
    // 主操作按钮（accent 填充）
    return _wcsicmp(text, L"Test connection") == 0;
}

void roundRectPath(HDC dc, const RECT& r, int radius) {
    RECT rc = r;
    rc.right--; rc.bottom--;  // pen 对齐
    HRGN region = CreateRoundRectRgn(rc.left, rc.top, rc.right + 1, rc.bottom + 1, radius * 2, radius * 2);
    // GDI RoundRect 用画笔描边 + 画刷填充，这里直接用两步：填充 + 1px 描边
    DeleteObject(region);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
}

} // namespace win11

// Win11 风格按钮自绘（明暗两套主题统一走这里）。
// 圆角 + Subtle 填充 + 悬停/按下状态；主操作按钮用 accent 填充。
LRESULT drawStyledButton(NMCUSTOMDRAW* draw) {
    if (draw->dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
    HWND button = draw->hdr.hwndFrom;
    const LONG_PTR style = GetWindowLongPtrW(button, GWL_STYLE);
    const UINT type = (UINT)(style & BS_TYPEMASK);
    const bool checkbox = type == BS_AUTOCHECKBOX || type == BS_CHECKBOX;
    const bool radio = type == BS_AUTORADIOBUTTON || type == BS_RADIOBUTTON;
    if (!checkbox && !radio && type != BS_PUSHBUTTON && type != BS_DEFPUSHBUTTON)
        return CDRF_DODEFAULT;

    const ui::Palette th = ui::palette(g.dark, g.highContrast);
    const bool disabled = !IsWindowEnabled(button);
    const bool pressed = (draw->uItemState & CDIS_SELECTED) != 0;
    const bool hot = (draw->uItemState & CDIS_HOT) != 0;
    const bool isPrimary = !checkbox && !radio &&
        win11::isPrimary(button) && type != BS_DEFPUSHBUTTON;

    RECT textRect = draw->rc;
    FillRect(draw->hdc, &draw->rc, controlBackground(button));

    const int saved = SaveDC(draw->hdc);
    HFONT font = (HFONT)SendMessageW(button, WM_GETFONT, 0, 0);
    if (font) SelectObject(draw->hdc, font);

    wchar_t text[512]{};
    GetWindowTextW(button, text, (int)std::size(text));
    SetBkMode(draw->hdc, TRANSPARENT);

    if (checkbox || radio) {
        // 复选/单选：glyph 交给主题渲染器（DarkMode/Explorer 已足够现代），
        // 只统一文字颜色与背景。
        HTHEME theme = OpenThemeData(button, L"Button");
        if (theme) {
            int state = disabled ? 4 : pressed ? 3 : hot ? 2 : 1;
            const int part = radio ? BP_RADIOBUTTON : BP_CHECKBOX;
            if (SendMessageW(button, BM_GETCHECK, 0, 0) == BST_CHECKED) state += 4;
            SIZE size{px(16), px(16)};
            GetThemePartSize(theme, draw->hdc, part, state, nullptr, TS_TRUE, &size);
            RECT glyph{draw->rc.left, draw->rc.top + (draw->rc.bottom - draw->rc.top - size.cy) / 2,
                       draw->rc.left + size.cx, 0};
            glyph.bottom = glyph.top + size.cy;
            DrawThemeBackground(theme, draw->hdc, part, state, &glyph, nullptr);
            CloseThemeData(theme);
            textRect.left = glyph.right + px(6);
        }
        SetTextColor(draw->hdc, disabled ? secondaryColor() : textColor());
        UINT flags = DT_SINGLELINE | DT_VCENTER | DT_LEFT;
        const LRESULT uiState = SendMessageW(button, WM_QUERYUISTATE, 0, 0);
        if (uiState & UISF_HIDEACCEL) flags |= DT_HIDEPREFIX;
        DrawTextW(draw->hdc, text, -1, &textRect, flags);
        if ((draw->uItemState & CDIS_FOCUS) && !(uiState & UISF_HIDEFOCUS)) {
            RECT focus = textRect;
            InflateRect(&focus, -1, -2);
            DrawFocusRect(draw->hdc, &focus);
        }
        RestoreDC(draw->hdc, saved);
        return CDRF_SKIPDEFAULT;
    }

    // ---- 普通按钮：Win11 圆角样式 ----
    const COLORREF fillBase = isPrimary ? ui::Accent
        : disabled ? th.button
        : th.surface;
    COLORREF fill = fillBase;
    COLORREF border = isPrimary ? fillBase : th.buttonBorder;
    const COLORREF textColor2 = isPrimary ? ui::AccentText
                  : disabled ? secondaryColor()
                  : textColor();
    if (disabled) {
        fill = win11::blend(fillBase, pageBg(), 0.45);
        border = win11::blend(border, pageBg(), 0.45);
    } else if (pressed) {
        fill = win11::blend(fillBase, g.dark ? RGB(0x00, 0x00, 0x00) : RGB(0x00, 0x00, 0x00), 0.12);
        border = fill;
    } else if (hot) {
        fill = win11::blend(fillBase, g.dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x00, 0x00, 0x00), isPrimary ? 0.10 : 0.05);
        border = win11::blend(border, g.dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x00, 0x00, 0x00), 0.18);
    }

    HBRUSH bgBrush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, win11::kBorderWidth, border);
    HGDIOBJ oldBrush = SelectObject(draw->hdc, bgBrush);
    HGDIOBJ oldPen = SelectObject(draw->hdc, pen);
    win11::roundRectPath(draw->hdc, draw->rc, px(win11::kCornerRadius));
    SelectObject(draw->hdc, oldPen);
    SelectObject(draw->hdc, oldBrush);
    DeleteObject(pen);
    DeleteObject(bgBrush);

    UINT flags = DT_SINGLELINE | DT_VCENTER | DT_CENTER;
    const LRESULT uiState = SendMessageW(button, WM_QUERYUISTATE, 0, 0);
    if (uiState & UISF_HIDEACCEL) flags |= DT_HIDEPREFIX;
    InflateRect(&textRect, -px(4), 0);
    SetTextColor(draw->hdc, textColor2);
    DrawTextW(draw->hdc, text, -1, &textRect, flags);
    if ((draw->uItemState & CDIS_FOCUS) && !(uiState & UISF_HIDEFOCUS) && !isPrimary) {
        RECT focus = textRect;
        InflateRect(&focus, -1, -2);
        DrawFocusRect(draw->hdc, &focus);
    }
    RestoreDC(draw->hdc, saved);
    return CDRF_SKIPDEFAULT;
}

std::wstring hotkeyText(HWND hwnd) {
    const WORD encoded = (WORD)SendMessageW(hwnd, HKM_GETHOTKEY, 0, 0);
    Hotkey hotkey{};
    hotkey.vk = LOBYTE(encoded);
    hotkey.modifiers = 0;
    const BYTE flags = HIBYTE(encoded);
    if (flags & HOTKEYF_CONTROL) hotkey.modifiers |= MOD_CONTROL;
    if (flags & HOTKEYF_SHIFT) hotkey.modifiers |= MOD_SHIFT;
    if (flags & HOTKEYF_ALT) hotkey.modifiers |= MOD_ALT;
    return hotkey.vk ? hotkey.displayString() : L"None";
}

bool systemUsesDarkMode() {
    DWORD light = 1, size = sizeof(light);
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, nullptr, (BYTE*)&light, &size);
        RegCloseKey(key);
    }
    return light == 0;
}

void enableCommonControlDarkMode() {
    // Common controls do not opt into their dark renderers merely from the
    // DWM title-bar attribute.  This process-level opt-in must happen before
    // the controls are created (supported on Windows 10 1809 and newer).
    using SetPreferredAppModeFn = int (WINAPI*)(int);
    if (HMODULE ux = GetModuleHandleW(L"uxtheme.dll")) {
        auto setMode = reinterpret_cast<SetPreferredAppModeFn>(
            GetProcAddress(ux, MAKEINTRESOURCEA(135)));
        if (setMode) setMode(1); // PreferredAppMode::AllowDark
    }
}

HIMAGELIST createTabImages();

void createFonts() {
    const HFONT oldFont = g.font, oldBold = g.fontBold, oldTitle = g.fontTitle;
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, g.dpi);
    g.font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW bold = ncm.lfMessageFont;
    bold.lfWeight = FW_SEMIBOLD;
    bold.lfHeight = MulDiv(bold.lfHeight, 5, 4);
    g.fontBold = CreateFontIndirectW(&bold);
    LOGFONTW title = ncm.lfMessageFont;
    title.lfWeight = FW_SEMIBOLD;
    title.lfHeight = -MulDiv(20, (int)g.dpi, 72);
    g.fontTitle = CreateFontIndirectW(&title);
    struct PreviousFonts { HFONT bold, title; } previous{oldBold, oldTitle};
    EnumChildWindows(g.hwnd, [](HWND child, LPARAM param) -> BOOL {
        const auto& old = *reinterpret_cast<PreviousFonts*>(param);
        HFONT current = (HFONT)SendMessageW(child, WM_GETFONT, 0, 0);
        HFONT next = current && current == old.title ? g.fontTitle
                   : current && current == old.bold ? g.fontBold : g.font;
        SendMessageW(child, WM_SETFONT, (WPARAM)next, TRUE);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&previous));
    if (oldFont) DeleteObject(oldFont);
    if (oldBold) DeleteObject(oldBold);
    if (oldTitle) DeleteObject(oldTitle);
}

void applyTheme(HWND hwnd) {
    if (!hwnd) return;
    if (g.tabImages) {
        HIMAGELIST images = createTabImages();
        if (images) {
            ImageList_Destroy(g.tabImages);
            g.tabImages = images;
        }
    }
    BOOL dark = g.dark;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    SetWindowTheme(hwnd, g.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    EnumChildWindows(hwnd, [](HWND child, LPARAM darkParam) -> BOOL {
        const bool darkMode = darkParam != 0;
        wchar_t cls[32]{};
        GetClassNameW(child, cls, (int)std::size(cls));
        const bool isHotkey = _wcsicmp(cls, HOTKEY_CLASSW) == 0;
        const bool needsCfd = _wcsicmp(cls, L"ComboBox") == 0 ||
                              _wcsicmp(cls, L"Edit") == 0;
        // The legacy hotkey control has no dark themed renderer.  Disable its
        // theme so WM_CTLCOLOREDIT can supply our dark background and text.
        if (darkMode && isHotkey)
            SetWindowTheme(child, L"", L"");
        else
            SetWindowTheme(child, darkMode ? (needsCfd ? L"DarkMode_CFD" : L"DarkMode_Explorer")
                                           : L"Explorer", nullptr);
        form::refresh(child);
        return TRUE;
    }, (LPARAM)g.dark);
    RedrawWindow(hwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

void updateFirstRunCompletion() {
    Settings& s = Settings::shared();
    if (!s.hasCompletedFirstLaunch && g.backendReady && g.shortcutsReady) {
        s.hasCompletedFirstLaunch = true;
        s.save();
    }
    if (ctrl(kFirstRunHint)) {
        SetWindowTextW(ctrl(kFirstRunHint), s.hasCompletedFirstLaunch
            ? L"Setup checks complete. TypeTide is ready."
            : L"First run: test the backend and verify both shortcuts before relying on TypeTide.");
    }
}

HWND ctrl(int id) { return GetDlgItem(g.hwnd, id); }

HIMAGELIST createTabImages() {
    const int size = px(16);
    HIMAGELIST images = ImageList_Create(size, size, ILC_COLOR32, 8, 0);
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    DWORD* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS,
                                      reinterpret_cast<void**>(&pixels), nullptr, 0);
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    HBRUSH maskBrush = CreateSolidBrush(RGB(255, 0, 255));
    HPEN pen = CreatePen(PS_SOLID, std::max(1, px(1)), textColor());
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    auto p = [](int value) { return px(value); };
    for (int icon = 0; icon < 8; ++icon) {
        RECT rc{0, 0, size, size};
        FillRect(dc, &rc, maskBrush);
        switch (icon) {
        case 0: // General: gear
            Ellipse(dc, p(4), p(4), p(12), p(12));
            Ellipse(dc, p(7), p(7), p(9), p(9));
            MoveToEx(dc, p(8), p(1), nullptr); LineTo(dc, p(8), p(4));
            MoveToEx(dc, p(8), p(12), nullptr); LineTo(dc, p(8), p(15));
            MoveToEx(dc, p(1), p(8), nullptr); LineTo(dc, p(4), p(8));
            MoveToEx(dc, p(12), p(8), nullptr); LineTo(dc, p(15), p(8));
            break;
        case 1: // Backend: server stack
            Rectangle(dc, p(2), p(2), p(14), p(7));
            Rectangle(dc, p(2), p(9), p(14), p(14));
            Ellipse(dc, p(4), p(4), p(6), p(6));
            Ellipse(dc, p(4), p(11), p(6), p(13));
            break;
        case 2: // Language: globe
            Ellipse(dc, p(2), p(2), p(14), p(14));
            MoveToEx(dc, p(2), p(8), nullptr); LineTo(dc, p(14), p(8));
            Ellipse(dc, p(5), p(2), p(11), p(14));
            break;
        case 3: // Shortcuts: keyboard
            Rectangle(dc, p(1), p(3), p(15), p(13));
            for (int x = 4; x <= 12; x += 4) {
                MoveToEx(dc, p(x), p(5), nullptr); LineTo(dc, p(x + 1), p(5));
                MoveToEx(dc, p(x), p(8), nullptr); LineTo(dc, p(x + 1), p(8));
            }
            MoveToEx(dc, p(4), p(11), nullptr); LineTo(dc, p(12), p(11));
            break;
        case 4: // Behavior: sliders
            MoveToEx(dc, p(2), p(4), nullptr); LineTo(dc, p(14), p(4));
            MoveToEx(dc, p(2), p(8), nullptr); LineTo(dc, p(14), p(8));
            MoveToEx(dc, p(2), p(12), nullptr); LineTo(dc, p(14), p(12));
            Ellipse(dc, p(5), p(2), p(8), p(6));
            Ellipse(dc, p(10), p(6), p(13), p(10));
            Ellipse(dc, p(3), p(10), p(6), p(14));
            break;
        case 5: // Skip apps: stacked windows
            Rectangle(dc, p(2), p(4), p(12), p(13));
            MoveToEx(dc, p(5), p(2), nullptr); LineTo(dc, p(14), p(2));
            LineTo(dc, p(14), p(11));
            break;
        case 6: // Diagnostics: pulse
            MoveToEx(dc, p(1), p(8), nullptr); LineTo(dc, p(4), p(8));
            LineTo(dc, p(6), p(3)); LineTo(dc, p(9), p(13));
            LineTo(dc, p(11), p(8)); LineTo(dc, p(15), p(8));
            break;
        case 7: // About: info
            Ellipse(dc, p(2), p(2), p(14), p(14));
            Ellipse(dc, p(7), p(4), p(9), p(6));
            MoveToEx(dc, p(8), p(7), nullptr); LineTo(dc, p(8), p(12));
            break;
        }
        // GDI draws RGB with no alpha. Convert the drawing's background to
        // transparent pixels and its theme-colored strokes to opaque pixels.
        // Avoid color-key masks: they leaked magenta tiles on dark navigation.
        GdiFlush();
        for (int pixel = 0; pixel < size * size; ++pixel) {
            DWORD rgb = pixels[pixel] & 0x00FFFFFF;
            pixels[pixel] = rgb == 0x00FF00FF ? 0 : (rgb | 0xFF000000);
        }
        SelectObject(dc, oldBitmap);
        ImageList_Add(images, bitmap, nullptr);
        SelectObject(dc, bitmap);
    }
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBitmap);
    DeleteObject(pen);
    DeleteObject(maskBrush);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return images;
}

void applyToApp() {
    if (g.appWindow) {
        // 直接调用（同线程）重装配热键 / 划词触发
        App::shared().applyEnabledState();
    }
}

// ---------- 控件创建辅助 ----------

HWND make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h,
          int id, DWORD exStyle = 0) {
    if (_wcsicmp(cls, L"COMBOBOX") == 0) style |= CBS_OWNERDRAWFIXED | CBS_HASSTRINGS;
    // EDIT caches its border during WM_NCCREATE; remove it before creation,
    // not just when attaching the rounded non-client frame.
    if (_wcsicmp(cls, L"EDIT") == 0) style &= ~WS_BORDER;
    HWND c = CreateWindowExW(exStyle, cls, text, WS_CHILD | style, px(x), px(y), px(w), px(h),
                             g.hwnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g.font, TRUE);
    if (_wcsicmp(cls, L"EDIT") == 0) form::attachEdit(c, formTheme);
    if (_wcsicmp(cls, L"COMBOBOX") == 0) form::attachCombo(c, formTheme);
    if (_wcsicmp(cls, HOTKEY_CLASSW) == 0) form::attachHotkey(c, formTheme, hotkeyText);
    return c;
}

HWND makeLabel(const wchar_t* text, int x, int y, int w, int id, bool bold = false, int h = 18) {
    HWND c = make(L"STATIC", text, SS_LEFT, x, y, w, h, id);
    if (bold) SendMessageW(c, WM_SETFONT, (WPARAM)g.fontBold, TRUE);
    return c;
}

// 创建时立即登记页面归属，位置只由布局声明计算；不再传手写 x/y。
struct RowBuilder {
    layout::Row& row;
    std::vector<HWND>& controls;

    RowBuilder& add(HWND hwnd, int width, int height, bool stretch = false,
                    int dropHeight = 0, bool growHeight = false) {
        row.items.push_back({hwnd, width, height, stretch, dropHeight, growHeight});
        controls.push_back(hwnd);
        return *this;
    }
    RowBuilder& label(const wchar_t* text, int width = 0, int id = -1,
                      int height = 20, bool bold = false) {
        return add(makeLabel(text, 0, 0, width, id, bold, height), width, height, width == 0);
    }
    RowBuilder& control(const wchar_t* cls, const wchar_t* text, DWORD style,
                        int width, int height, int id, bool stretch = false,
                        int dropHeight = 0, bool growHeight = false) {
        return add(make(cls, text, style, 0, 0, width, dropHeight ? dropHeight : height, id),
                   width, height, stretch || width == 0, dropHeight, growHeight);
    }
    RowBuilder& combo(int id, int width = 0, bool editable = false) {
        return control(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL |
                       (editable ? CBS_DROPDOWN : CBS_DROPDOWNLIST), width, form::Height, id,
                       width == 0, 240);
    }
    RowBuilder& button(const wchar_t* text, int width, int id) {
        return control(L"BUTTON", text, WS_TABSTOP | BS_PUSHBUTTON, width, 32, id);
    }
    RowBuilder& checkbox(const wchar_t* text, int width, int id) {
        return control(L"BUTTON", text, WS_TABSTOP | BS_AUTOCHECKBOX, width, 24, id);
    }
};

struct PageBuilder {
    int index;
    RowBuilder row(int spacingAfter = 16, int inset = 0) {
        return {layout::addRow(g.layouts[index], spacingAfter, inset), g.pages[index]};
    }
};

void fillLanguageCombo(HWND combo, Language selected) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (Language l : lang::All) {
        int idx = (int)SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)lang::DisplayName(l));
        if (l == selected) SendMessageW(combo, CB_SETCURSEL, idx, 0);
    }
}

Language comboLanguage(HWND combo) {
    int idx = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (idx < 0 || idx >= (int)std::size(lang::All)) return Language::English;
    return lang::All[idx];
}

void fillDirectionCombo(HWND combo, TranslationDirection selected) {
    const Settings& s = Settings::shared();
    std::wstring native = lang::ShortName(s.nativeLanguage);
    std::wstring foreign = lang::ShortName(s.foreignLanguage);
    std::wstring items[] = {
        L"Auto · bidirectional",
        native + L" → " + foreign,
        foreign + L" → " + native,
    };
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (auto& it : items) SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)it.c_str());
    SendMessageW(combo, CB_SETCURSEL, (int)selected, 0);
}

void fillStyleCombo(HWND combo, RewriteStyle selected) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    const RewriteStyle all[] = {RewriteStyle::Faithful, RewriteStyle::Formal,
                                RewriteStyle::Casual, RewriteStyle::Polished};
    for (RewriteStyle st : all) {
        int idx = (int)SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)lang::StyleDisplayName(st));
        if (st == selected) SendMessageW(combo, CB_SETCURSEL, idx, 0);
    }
}

RewriteStyle comboStyle(HWND combo) {
    const RewriteStyle all[] = {RewriteStyle::Faithful, RewriteStyle::Formal,
                                RewriteStyle::Casual, RewriteStyle::Polished};
    int idx = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
    return (idx >= 0 && idx < 4) ? all[idx] : RewriteStyle::Faithful;
}

void fillShortcutActionCombo(HWND combo, ShortcutAction selected) {
    const wchar_t* names[] = {
        L"Smart translate · popup", L"Translate to native language · popup",
        L"Translate to foreign language · popup", L"Smart translate · replace",
        L"Translate to native language · replace", L"Translate to foreign language · replace",
    };
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (const auto* name : names) SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)name);
    SendMessageW(combo, CB_SETCURSEL, (int)selected, 0);
}

std::wstring editText(HWND edit) {
    int n = GetWindowTextLengthW(edit);
    std::wstring s(n, 0);
    if (n) GetWindowTextW(edit, s.data(), n + 1);
    return s;
}

// HOTKEY 控件 ⇄ RegisterHotKey 修饰键转换
WORD hotkeyToControl(const Hotkey& hk) {
    BYTE flags = 0;
    if (hk.modifiers & MOD_SHIFT) flags |= HOTKEYF_SHIFT;
    if (hk.modifiers & MOD_CONTROL) flags |= HOTKEYF_CONTROL;
    if (hk.modifiers & MOD_ALT) flags |= HOTKEYF_ALT;
    return MAKEWORD(hk.vk, flags);
}

Hotkey hotkeyFromControl(WORD w) {
    Hotkey hk;
    hk.vk = LOBYTE(w);
    BYTE flags = HIBYTE(w);
    hk.modifiers = 0;
    if (flags & HOTKEYF_SHIFT) hk.modifiers |= MOD_SHIFT;
    if (flags & HOTKEYF_CONTROL) hk.modifiers |= MOD_CONTROL;
    if (flags & HOTKEYF_ALT) hk.modifiers |= MOD_ALT;
    return hk;
}

void updateShortcutControls() {
    const Settings& s = Settings::shared();
    EnableWindow(ctrl(kReadWin), s.readShortcut.isConfigured());
    EnableWindow(ctrl(kRewriteWin), s.rewriteShortcut.isConfigured());
    EnableWindow(ctrl(kHkReadClear), s.readShortcut.isConfigured());
    EnableWindow(ctrl(kHkRewriteClear), s.rewriteShortcut.isConfigured());
}

// ---------- 页构建 ----------

void updateBackendEnabled() {
    EnableWindow(ctrl(kOaiProvider), TRUE);
    EnableWindow(ctrl(kOaiUrl), TRUE);
    EnableWindow(ctrl(kOaiKey), TRUE);
    EnableWindow(ctrl(kOaiModel), TRUE);
    EnableWindow(ctrl(kOaiFetch), TRUE);
}

int cloudProviderIndex(const std::string& baseUrl) {
    if (baseUrl.find("api.openai.com") != std::string::npos) return 0;
    if (baseUrl.find("openrouter.ai") != std::string::npos) return 1;
    if (baseUrl.find("api.deepseek.com") != std::string::npos) return 2;
    return 3;
}

void applyCloudProviderPreset(int index) {
    Settings& s = Settings::shared();
    switch (index) {
    case 0:
        s.openAIBaseURL = "https://api.openai.com/v1";
        s.openAIModel = "gpt-5-mini";
        break;
    case 1:
        s.openAIBaseURL = "https://openrouter.ai/api/v1";
        s.openAIModel = "openai/gpt-5-mini";
        break;
    case 2:
        s.openAIBaseURL = "https://api.deepseek.com";
        s.openAIModel = "deepseek-v4-flash";
        break;
    default:
        return;
    }
    SetWindowTextW(ctrl(kOaiUrl), util::Widen(s.openAIBaseURL).c_str());
    SetWindowTextW(ctrl(kOaiModel), util::Widen(s.openAIModel).c_str());
}

void updateLangWarn() {
    const Settings& s = Settings::shared();
    SetWindowTextW(ctrl(kLangWarn),
                   s.nativeLanguage == s.foreignLanguage
                       ? L"⚠ Native and foreign are the same language — translation won't do anything useful."
                       : L"TypeTide translates between your native and foreign language. Auto direction detects the text and translates the other way.");
}

void layoutForClientSize(int width, int height);

void buildPages() {
    const Settings& s = Settings::shared();
    g.pages.assign(8, {});
    g.layouts.assign(8, {});
    const int x = layout::kPageX, w = 560;
    const wchar_t* pageTitles[] = {
        L"General", L"Translation backend", L"Languages", L"Keyboard shortcuts",
        L"Behavior", L"Excluded apps", L"Diagnostics", L"About TypeTide"
    };
    for (int page = 0; page < 8; ++page) {
        HWND title = makeLabel(pageTitles[page], x, 26, w, kPageTitle0 + page);
        SendMessageW(title, WM_SETFONT, (WPARAM)g.fontTitle, TRUE);
        SetWindowPos(title, nullptr, px(x), px(26), px(w), px(36), SWP_NOZORDER);
        g.pages[page].push_back(title);
    }

    // --- 0 General ---
    {
        PageBuilder page{0};
        page.row(8).label(L"APP STATUS", 0, -1, 22, true);
        page.row(24).checkbox(L"Enable TypeTide", 0, kEnable);
        page.row(8).label(L"STARTUP", 0, -1, 22, true);
        page.row(24).checkbox(L"Launch at login", 0, kLogin);
        page.row(8).label(L"UPDATES", 0, -1, 22, true);
        page.row(12).label(L"Version " TYPETIDE_VERSION_STRING, 0, kVersionLabel);
        page.row(20).button(L"Check for updates", 150, kCheckUpdate);
        page.row().label(L"", 0, kFirstRunHint, 40);
    }
    CheckDlgButton(g.hwnd, kEnable, s.enabled ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g.hwnd, kLogin, launchatlogin::IsEnabled() ? BST_CHECKED : BST_UNCHECKED);
    updateFirstRunCompletion();

    // --- 1 Backend ---
    {
        PageBuilder page{1};
        // 卡片内边距 20；标题底边与卡片顶边之间保留 18px。
        g.layouts[1].top = 100;
        g.layouts[1].cardFirst = 0;
        g.layouts[1].cardLast = 3;
        page.row(16, 20).label(L"Provider", 100, kOaiProviderLabel).combo(kOaiProvider, 260);
        page.row(16, 20).label(L"Base URL", 100, kOaiUrlLabel)
            .control(L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 0, form::Height, kOaiUrl);
        page.row(16, 20).label(L"API key", 100, kOaiKeyLabel)
            .control(L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | ES_PASSWORD,
                     0, form::Height, kOaiKey);
        page.row(40, 20).label(L"API model", 100, kOaiModelLabel).combo(kOaiModel, 0, true)
            .button(L"Fetch models", 110, kOaiFetch);
        page.row(8).label(L"Type a model name, or fetch the list from the endpoint. "
                         L"The API key is stored in Windows Credential Manager.", 0, kOaiHint, 40);
        page.row(16).label(L"Privacy — triggered selections are sent to the configured endpoint. "
                          L"Exclude sensitive apps on the Excluded apps page.", 0, kPrivacyHint, 40);
        page.row().button(L"Test connection", 140, kBackendTest)
            .label(L"Uses fixed synthetic text, never your clipboard.", 0, kBackendStatus, 40);
    }
    SetWindowTextW(ctrl(kOaiUrl), util::Widen(s.openAIBaseURL).c_str());
    SetWindowTextW(ctrl(kOaiKey), util::Widen(s.openAIKey).c_str());
    SetWindowTextW(ctrl(kOaiModel), util::Widen(s.openAIModel).c_str());
    {
        HWND provider = ctrl(kOaiProvider);
        const wchar_t* providers[] = {L"OpenAI", L"OpenRouter", L"DeepSeek", L"Custom endpoint"};
        for (auto* name : providers) SendMessageW(provider, CB_ADDSTRING, 0, (LPARAM)name);
        SendMessageW(provider, CB_SETCURSEL, cloudProviderIndex(s.openAIBaseURL), 0);
    }
    updateBackendEnabled();
    // --- 2 Language ---
    {
        PageBuilder page{2};
        page.row().label(L"Native language:", 130, kNativeLabel).combo(kNative, 260);
        page.row(24).label(L"Foreign language:", 130, kForeignLabel).combo(kForeign, 260);
        page.row().label(L"Read direction:", 130, kReadDirLabel).combo(kReadDir, 260);
        page.row(20).label(L"Rewrite direction:", 130, kRewriteDirLabel).combo(kRewriteDir, 260);
        page.row().label(L"", 0, kLangWarn, 40);
    }
    fillLanguageCombo(ctrl(kNative), s.nativeLanguage);
    fillLanguageCombo(ctrl(kForeign), s.foreignLanguage);
    fillDirectionCombo(ctrl(kReadDir), s.readDirection);
    fillDirectionCombo(ctrl(kRewriteDir), s.rewriteDirection);
    updateLangWarn();
    // --- 3 Shortcuts ---
    {
        PageBuilder page{3};
        page.row().label(L"Translate selection (read):", 180, kHkReadLabel)
            .control(HOTKEY_CLASSW, L"", WS_TABSTOP, 140, form::Height, kHkRead, true)
            .checkbox(L"Win key", 80, kReadWin).button(L"Clear", 64, kHkReadClear);
        page.row(24).label(L"Rewrite && replace (write):", 180, kHkRewriteLabel)
            .control(HOTKEY_CLASSW, L"", WS_TABSTOP, 140, form::Height, kHkRewrite, true)
            .checkbox(L"Win key", 80, kRewriteWin).button(L"Clear", 64, kHkRewriteClear);
        page.row().label(L"Translate action:", 180, kReadActionLabel).combo(kReadAction);
        page.row(24).label(L"Rewrite action:", 180, kRewriteActionLabel).combo(kRewriteAction);
        page.row(20).button(L"Restore defaults", 130, kHkRestoreDefaults)
            .button(L"Verify shortcuts", 130, kHkVerify);
        page.row(12).label(L"Changes apply immediately. Keep at least one shortcut; "
                          L"configured shortcuts must be different.", 0, kHkNote, 40);
        page.row().label(L"", 0, kHkStatus, 90);
    }
    SendMessageW(ctrl(kHkRead), HKM_SETHOTKEY, hotkeyToControl(s.readShortcut), 0);
    SendMessageW(ctrl(kHkRewrite), HKM_SETHOTKEY, hotkeyToControl(s.rewriteShortcut), 0);
    CheckDlgButton(g.hwnd, kReadWin, s.readShortcut.modifiers & MOD_WIN ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g.hwnd, kRewriteWin, s.rewriteShortcut.modifiers & MOD_WIN ? BST_CHECKED : BST_UNCHECKED);
    updateShortcutControls();
    fillShortcutActionCombo(ctrl(kReadAction), s.readShortcutAction);
    fillShortcutActionCombo(ctrl(kRewriteAction), s.rewriteShortcutAction);

    // --- 4 Behavior ---
    {
        PageBuilder page{4};
        page.row(20).label(L"On text selection:", 140, kTriggerLabel).combo(kTrigger, 260);
        page.row(24).checkbox(L"Preview before replacing (rewrite)", 0, kPreview);
        page.row().label(L"Read style:", 140, kReadStyleLabel).combo(kReadStyle, 260);
        page.row().label(L"Rewrite style:", 140, kRewriteStyleLabel).combo(kRewriteStyle, 260);
    }
    {
        HWND trig = ctrl(kTrigger);
        const wchar_t* items[] = {L"Off (shortcut only)", L"Show floating icon", L"Auto-translate"};
        for (auto* it : items) SendMessageW(trig, CB_ADDSTRING, 0, (LPARAM)it);
        SendMessageW(trig, CB_SETCURSEL, (int)s.selectionTrigger, 0);
    }
    CheckDlgButton(g.hwnd, kPreview, s.rewritePreview ? BST_CHECKED : BST_UNCHECKED);
    fillStyleCombo(ctrl(kReadStyle), s.readStyle);
    fillStyleCombo(ctrl(kRewriteStyle), s.rewriteStyle);

    // --- 5 Skip Apps ---
    {
        PageBuilder page{5};
        page.row(12).control(L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
                             0, form::Height, kSkipEdit).button(L"Add", 80, kSkipAdd);
        page.row(16).control(L"LISTBOX", L"", WS_TABSTOP | WS_BORDER | WS_VSCROLL |
                             LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, 0, 240, kSkipList, true, 0, true)
            .button(L"Remove", 80, kSkipRemove);
        page.row().label(L"TypeTide stays inactive in these apps (executable name, "
                         L"e.g. notepad.exe or code.exe).", 0, kSkipHint, 40);
    }
    for (auto& app : s.skipApps)
        SendMessageW(ctrl(kSkipList), LB_ADDSTRING, 0, (LPARAM)app.c_str());

    // --- 6 Diagnostics ---
    {
        PageBuilder page{6};
        page.row(20).label(L"Stored only on this PC. Never includes selected text, translations, "
                          L"clipboard contents, app names, URLs, or credentials.", 0, kDiagPrivacy, 40);
        page.row(20).label(L"Report file:", 80)
            .control(L"STATIC", diagnostics::Path().c_str(), SS_LEFT | SS_PATHELLIPSIS,
                     0, 20, kDiagPath);
        page.row().button(L"Export report…", 130, kDiagExport)
            .button(L"Clear diagnostics", 140, kDiagClear);
    }

    // --- 7 About ---
    {
        PageBuilder page{7};
        page.row(12).label(L"TypeTide " TYPETIDE_VERSION_STRING, 0, kAboutTitle, 24, true);
        page.row(20).label(L"Clean, lightweight selection translation for Windows.\n"
                          L"Connect to any OpenAI-compatible API endpoint.", 0, kAboutDesc, 40);
        page.row().control(L"SysLink", L"<a href=\"https://github.com/Big-Molecule/typetide\">GitHub</a>",
                           WS_TABSTOP, 0, 24, kAboutLinks);
    }
    RECT client{};
    GetClientRect(g.hwnd, &client);
    layoutForClientSize(client.right, client.bottom);
}

void showPage(int index) {
    g.currentPage = index;
    // 页切换时让整个内容区重绘（清除上一页的自绘卡片等痕迹）
    if (g.hwnd) {
        RECT rc{};
        GetClientRect(g.hwnd, &rc);
        InvalidateRect(g.hwnd, &rc, TRUE);
    }
    for (int p = 0; p < (int)g.pages.size(); ++p)
        for (HWND c : g.pages[p])
            ShowWindow(c, p == index ? SW_SHOW : SW_HIDE);
}

void layoutForClientSize(int width, int height) {
    if (!g.hwnd || width <= 0 || height <= 0) return;
    const int logicalWidth = MulDiv(width, 96, (int)g.dpi);
    const int logicalHeight = MulDiv(height, 96, (int)g.dpi);
    const int pageWidth = std::max(0, logicalWidth - layout::kPageX - layout::kRightMargin);
    const int extraHeight = std::max(0, logicalHeight - layout::kBaseHeight);
    if (HWND nav = ctrl(kTab)) {
        SetWindowPos(nav, nullptr, 0, px(14), px(layout::kNavWidth), height - px(28),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    for (int p = 0; p < (int)g.layouts.size(); ++p) {
        SetWindowPos(ctrl(kPageTitle0 + p), nullptr, px(layout::kPageX), px(26),
                     px(pageWidth), px(36), SWP_NOZORDER | SWP_NOACTIVATE);
        layout::arrange(g.layouts[p], layout::kPageX, pageWidth, extraHeight,
            [](const layout::Item& item, const RECT& r, int nativeHeight) {
                SetWindowPos(item.hwnd, nullptr, px(r.left), px(r.top), px(r.right - r.left),
                             px(nativeHeight), SWP_NOZORDER | SWP_NOACTIVATE);
            });
    }
    if (g.layouts.size() > 1)
        g.backendCard = layout::cardRect(g.layouts[1], layout::kPageX, pageWidth);
    InvalidateRect(g.hwnd, nullptr, TRUE);
}

// ---------- 变更处理 ----------

void onCommand(int id, int code) {
    if (g.loading) return;
    Settings& s = Settings::shared();
    bool save = true, reapply = false;

    switch (id) {
    case kTab:
        if (code == LBN_SELCHANGE)
            showPage((int)SendMessageW(ctrl(kTab), LB_GETCURSEL, 0, 0));
        return;
    case kEnable:
        s.enabled = IsDlgButtonChecked(g.hwnd, kEnable) == BST_CHECKED;
        reapply = true;
        break;
    case kLogin:
        launchatlogin::SetEnabled(IsDlgButtonChecked(g.hwnd, kLogin) == BST_CHECKED);
        save = false;
        break;
    case kCheckUpdate:
        EnableWindow(ctrl(kCheckUpdate), FALSE);
        SetWindowTextW(ctrl(kCheckUpdate), L"Checking…");
        updatechecker::CheckNowInteractive(g.hwnd, kMsgUpdateResult);
        save = false;
        break;

    case kOaiProvider:
        if (code != CBN_SELCHANGE) return;
        applyCloudProviderPreset((int)SendMessageW(ctrl(kOaiProvider), CB_GETCURSEL, 0, 0));
        break;
    case kOaiUrl:
        if (code != EN_CHANGE) return;
        s.openAIBaseURL = util::Narrow(editText(ctrl(kOaiUrl)));
        break;
    case kOaiKey:
        if (code != EN_KILLFOCUS) return;
        s.openAIKey = util::Narrow(editText(ctrl(kOaiKey)));
        break;
    case kOaiModel:
        if (code == CBN_SELCHANGE) {
            int sel = (int)SendMessageW(ctrl(kOaiModel), CB_GETCURSEL, 0, 0);
            if (sel < 0) return;
            int len = (int)SendMessageW(ctrl(kOaiModel), CB_GETLBTEXTLEN, sel, 0);
            std::wstring m(len, 0);
            SendMessageW(ctrl(kOaiModel), CB_GETLBTEXT, sel, (LPARAM)m.data());
            s.openAIModel = util::Narrow(m);
        } else if (code == CBN_EDITCHANGE) {
            s.openAIModel = util::Narrow(editText(ctrl(kOaiModel)));
        } else {
            return;
        }
        break;
    case kOaiFetch: {
        // 用界面上当前的 URL/Key 去查询，用户不必先失焦保存
        const std::string base = util::Narrow(editText(ctrl(kOaiUrl)));
        const std::string key = util::Narrow(editText(ctrl(kOaiKey)));
        EnableWindow(ctrl(kOaiFetch), FALSE);
        SetWindowTextW(ctrl(kOaiHint), L"Fetching the model list from the endpoint…");
        HWND target = g.hwnd;
        translator::FetchCloudModelsAsync(base, key,
            [target](std::vector<std::wstring> models, std::wstring error) {
                auto* payload = new CloudModels{std::move(models), std::move(error)};
                if (!PostMessageW(target, kMsgCloudModels, 0, (LPARAM)payload)) delete payload;
            });
        save = false;
        break;
    }
    case kBackendTest:
        EnableWindow(ctrl(kBackendTest), FALSE);
        SetWindowTextW(ctrl(kBackendTest), L"Testing…");
        SetWindowTextW(ctrl(kBackendStatus), L"Connecting and translating synthetic text…");
        {
        HWND target = g.hwnd;
        translator::CheckHealthAsync([target](translator::HealthCheckResult result) {
            auto* payload = new translator::HealthCheckResult(std::move(result));
            if (!PostMessageW(target, kMsgBackendHealth, 0, (LPARAM)payload)) delete payload;
        });
        }
        save = false;
        break;

    case kNative:
        if (code != CBN_SELCHANGE) return;
        s.nativeLanguage = comboLanguage(ctrl(kNative));
        fillDirectionCombo(ctrl(kReadDir), s.readDirection);
        fillDirectionCombo(ctrl(kRewriteDir), s.rewriteDirection);
        updateLangWarn();
        break;
    case kForeign:
        if (code != CBN_SELCHANGE) return;
        s.foreignLanguage = comboLanguage(ctrl(kForeign));
        fillDirectionCombo(ctrl(kReadDir), s.readDirection);
        fillDirectionCombo(ctrl(kRewriteDir), s.rewriteDirection);
        updateLangWarn();
        break;
    case kReadDir:
        if (code != CBN_SELCHANGE) return;
        s.readDirection = (TranslationDirection)SendMessageW(ctrl(kReadDir), CB_GETCURSEL, 0, 0);
        break;
    case kRewriteDir:
        if (code != CBN_SELCHANGE) return;
        s.rewriteDirection = (TranslationDirection)SendMessageW(ctrl(kRewriteDir), CB_GETCURSEL, 0, 0);
        break;

    case kHkRead:
        if (code != EN_CHANGE) return;
        {
            Hotkey hk = hotkeyFromControl((WORD)SendMessageW(ctrl(kHkRead), HKM_GETHOTKEY, 0, 0));
            if (hk.vk == 0) {
                s.readShortcut = {0, 0};
                CheckDlgButton(g.hwnd, kReadWin, BST_UNCHECKED);
                updateShortcutControls();
                reapply = true;
                break;
            }
            if (IsDlgButtonChecked(g.hwnd, kReadWin) == BST_CHECKED) hk.modifiers |= MOD_WIN;
            if (s.rewriteShortcut.isConfigured() && hk == s.rewriteShortcut) {
                MessageBoxW(g.hwnd, L"Translate and rewrite must use different shortcuts.",
                            L"TypeTide — duplicate shortcut", MB_OK | MB_ICONWARNING);
                SendMessageW(ctrl(kHkRead), HKM_SETHOTKEY, hotkeyToControl(s.readShortcut), 0);
                return;
            }
            s.readShortcut = hk;
            updateShortcutControls();
            reapply = true;
        }
        break;
    case kHkRewrite:
        if (code != EN_CHANGE) return;
        {
            Hotkey hk = hotkeyFromControl((WORD)SendMessageW(ctrl(kHkRewrite), HKM_GETHOTKEY, 0, 0));
            if (hk.vk == 0) {
                s.rewriteShortcut = {0, 0};
                CheckDlgButton(g.hwnd, kRewriteWin, BST_UNCHECKED);
                updateShortcutControls();
                reapply = true;
                break;
            }
            if (IsDlgButtonChecked(g.hwnd, kRewriteWin) == BST_CHECKED) hk.modifiers |= MOD_WIN;
            if (s.readShortcut.isConfigured() && hk == s.readShortcut) {
                MessageBoxW(g.hwnd, L"Translate and rewrite must use different shortcuts.",
                            L"TypeTide — duplicate shortcut", MB_OK | MB_ICONWARNING);
                SendMessageW(ctrl(kHkRewrite), HKM_SETHOTKEY, hotkeyToControl(s.rewriteShortcut), 0);
                return;
            }
            s.rewriteShortcut = hk;
            updateShortcutControls();
            reapply = true;
        }
        break;
    case kReadWin:
        if (!s.readShortcut.isConfigured()) {
            CheckDlgButton(g.hwnd, kReadWin, BST_UNCHECKED);
            return;
        }
        if (IsDlgButtonChecked(g.hwnd, kReadWin) == BST_CHECKED) s.readShortcut.modifiers |= MOD_WIN;
        else s.readShortcut.modifiers &= ~MOD_WIN;
        if (s.rewriteShortcut.isConfigured() && s.readShortcut == s.rewriteShortcut) {
            MessageBoxW(g.hwnd, L"Translate and rewrite must use different shortcuts.",
                        L"TypeTide — duplicate shortcut", MB_OK | MB_ICONWARNING);
            s.readShortcut.modifiers ^= MOD_WIN;
            CheckDlgButton(g.hwnd, kReadWin,
                           s.readShortcut.modifiers & MOD_WIN ? BST_CHECKED : BST_UNCHECKED);
            return;
        }
        reapply = true;
        break;
    case kRewriteWin:
        if (!s.rewriteShortcut.isConfigured()) {
            CheckDlgButton(g.hwnd, kRewriteWin, BST_UNCHECKED);
            return;
        }
        if (IsDlgButtonChecked(g.hwnd, kRewriteWin) == BST_CHECKED) s.rewriteShortcut.modifiers |= MOD_WIN;
        else s.rewriteShortcut.modifiers &= ~MOD_WIN;
        if (s.readShortcut.isConfigured() && s.readShortcut == s.rewriteShortcut) {
            MessageBoxW(g.hwnd, L"Translate and rewrite must use different shortcuts.",
                        L"TypeTide — duplicate shortcut", MB_OK | MB_ICONWARNING);
            s.rewriteShortcut.modifiers ^= MOD_WIN;
            CheckDlgButton(g.hwnd, kRewriteWin,
                           s.rewriteShortcut.modifiers & MOD_WIN ? BST_CHECKED : BST_UNCHECKED);
            return;
        }
        reapply = true;
        break;
    case kHkReadClear:
        s.readShortcut = {0, 0};
        SendMessageW(ctrl(kHkRead), HKM_SETHOTKEY, 0, 0);
        CheckDlgButton(g.hwnd, kReadWin, BST_UNCHECKED);
        updateShortcutControls();
        reapply = true;
        break;
    case kHkRewriteClear:
        s.rewriteShortcut = {0, 0};
        SendMessageW(ctrl(kHkRewrite), HKM_SETHOTKEY, 0, 0);
        CheckDlgButton(g.hwnd, kRewriteWin, BST_UNCHECKED);
        updateShortcutControls();
        reapply = true;
        break;
    case kReadAction:
        if (code != CBN_SELCHANGE) return;
        s.readShortcutAction = (ShortcutAction)SendMessageW(ctrl(kReadAction), CB_GETCURSEL, 0, 0);
        break;
    case kRewriteAction:
        if (code != CBN_SELCHANGE) return;
        s.rewriteShortcutAction = (ShortcutAction)SendMessageW(ctrl(kRewriteAction), CB_GETCURSEL, 0, 0);
        break;
    case kHkRestoreDefaults:
        s.readShortcut = kDefaultReadShortcut;
        s.rewriteShortcut = kDefaultRewriteShortcut;
        s.readShortcutAction = ShortcutAction::SmartPopup;
        s.rewriteShortcutAction = ShortcutAction::SmartReplace;
        SendMessageW(ctrl(kHkRead), HKM_SETHOTKEY, hotkeyToControl(s.readShortcut), 0);
        SendMessageW(ctrl(kHkRewrite), HKM_SETHOTKEY, hotkeyToControl(s.rewriteShortcut), 0);
        CheckDlgButton(g.hwnd, kReadWin, BST_UNCHECKED);
        CheckDlgButton(g.hwnd, kRewriteWin, BST_UNCHECKED);
        fillShortcutActionCombo(ctrl(kReadAction), s.readShortcutAction);
        fillShortcutActionCombo(ctrl(kRewriteAction), s.rewriteShortcutAction);
        updateShortcutControls();
        reapply = true;
        break;
    case kHkVerify:
        applyToApp();
        g.shortcutsReady = App::shared().shortcutsRegistered();
        updateFirstRunCompletion();
        SettingsWindow::refreshShortcutStatus();
        save = false;
        break;

    case kTrigger:
        if (code != CBN_SELCHANGE) return;
        s.selectionTrigger = (SelectionTrigger)SendMessageW(ctrl(kTrigger), CB_GETCURSEL, 0, 0);
        reapply = true;
        break;
    case kPreview:
        s.rewritePreview = IsDlgButtonChecked(g.hwnd, kPreview) == BST_CHECKED;
        break;
    case kReadStyle:
        if (code != CBN_SELCHANGE) return;
        s.readStyle = comboStyle(ctrl(kReadStyle));
        break;
    case kRewriteStyle:
        if (code != CBN_SELCHANGE) return;
        s.rewriteStyle = comboStyle(ctrl(kRewriteStyle));
        break;

    case kSkipAdd: {
        std::wstring app = util::Trim(editText(ctrl(kSkipEdit)));
        if (app.empty()) { save = false; break; }
        for (auto& e : s.skipApps)
            if (util::ToLower(e) == util::ToLower(app)) { save = false; break; }
        if (!save) break;
        s.skipApps.push_back(app);
        SendMessageW(ctrl(kSkipList), LB_ADDSTRING, 0, (LPARAM)app.c_str());
        SetWindowTextW(ctrl(kSkipEdit), L"");
        break;
    }
    case kSkipRemove: {
        int sel = (int)SendMessageW(ctrl(kSkipList), LB_GETCURSEL, 0, 0);
        if (sel < 0 || sel >= (int)s.skipApps.size()) { save = false; break; }
        s.skipApps.erase(s.skipApps.begin() + sel);
        SendMessageW(ctrl(kSkipList), LB_DELETESTRING, sel, 0);
        break;
    }
    case kDiagExport: {
        wchar_t path[MAX_PATH] = L"TypeTide-Diagnostics.json";
        OPENFILENAMEW dialog{sizeof(dialog)};
        dialog.hwndOwner = g.hwnd;
        dialog.lpstrFilter = L"JSON report\0*.json\0All files\0*.*\0";
        dialog.lpstrFile = path;
        dialog.nMaxFile = MAX_PATH;
        dialog.lpstrDefExt = L"json";
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        if (GetSaveFileNameW(&dialog)) {
            if (!CopyFileW(diagnostics::Path().c_str(), path, FALSE))
                MessageBoxW(g.hwnd, L"No diagnostics have been recorded yet.",
                            L"TypeTide — export", MB_OK | MB_ICONINFORMATION);
        }
        save = false;
        break;
    }
    case kDiagClear:
        diagnostics::Clear();
        MessageBoxW(g.hwnd, L"Local diagnostics were cleared.", L"TypeTide — diagnostics",
                    MB_OK | MB_ICONINFORMATION);
        save = false;
        break;

    default:
        return;
    }

    if (save) {
        if (s.save()) {
            g.saveWarningShown = false;
        } else if (!g.saveWarningShown) {
            g.saveWarningShown = true;
            MessageBoxW(g.hwnd,
                        L"TypeTide couldn't save your settings. Your existing settings file was "
                        L"left unchanged.\n\nCheck that the settings folder is writable, then try again.",
                        L"TypeTide — settings not saved", MB_OK | MB_ICONWARNING);
        }
    }
    if (reapply) applyToApp();
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lp);
        RECT minimum{0, 0, px(820), px(layout::kBaseHeight)};
        AdjustWindowRectExForDpi(&minimum, WS_OVERLAPPEDWINDOW, FALSE, 0, g.dpi);
        info->ptMinTrackSize = {minimum.right - minimum.left, minimum.bottom - minimum.top};
        return 0;
    }
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED)
            layoutForClientSize(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_DPICHANGED: {
        g.dpi = HIWORD(wp);
        createFonts();
        SendMessageW(ctrl(kTab), LB_SETITEMHEIGHT, 0, px(42));
        const RECT& suggested = *reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd, nullptr, suggested.left, suggested.top,
                     suggested.right - suggested.left, suggested.bottom - suggested.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        RECT client{};
        GetClientRect(hwnd, &client);
        layoutForClientSize(client.right, client.bottom);
        applyTheme(hwnd);
        return 0;
    }
    case WM_COMMAND:
        onCommand(LOWORD(wp), HIWORD(wp));
        return 0;
    case kMsgCloudModels: {
        auto* payload = (CloudModels*)lp;
        EnableWindow(ctrl(kOaiFetch), TRUE);
        HWND combo = ctrl(kOaiModel);
        const std::wstring current = util::Widen(Settings::shared().openAIModel);
        if (!payload->models.empty()) {
            SendMessageW(combo, CB_RESETCONTENT, 0, 0);
            for (auto& m : payload->models)
                SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)m.c_str());
            // 保留用户当前输入；他可以从下拉里换一个
            SetWindowTextW(combo, current.c_str());
            SetWindowTextW(ctrl(kOaiHint),
                           (L"Loaded " + std::to_wstring(payload->models.size()) +
                            L" models from the endpoint. Pick one or keep typing a name.")
                               .c_str());
        } else {
            const std::wstring message = payload->error.empty()
                ? std::wstring(L"The endpoint returned no models.")
                : std::wstring(L"⚠ ") + payload->error;
            SetWindowTextW(ctrl(kOaiHint), message.c_str());
        }
        delete payload;
        return 0;
    }
    case kMsgUpdateResult:
        EnableWindow(ctrl(kCheckUpdate), TRUE);
        SetWindowTextW(ctrl(kCheckUpdate), L"Check for updates");
        if (wp == 1) {
            if (MessageBoxW(hwnd, L"A new TypeTide version is available. Open the download page?",
                            L"TypeTide update", MB_YESNO | MB_ICONINFORMATION) == IDYES)
                updatechecker::OpenReleasesPage();
        } else if (wp == 0) {
            MessageBoxW(hwnd, L"You're using the latest version.", L"TypeTide update",
                        MB_OK | MB_ICONINFORMATION);
        } else {
            MessageBoxW(hwnd,
                        L"TypeTide couldn't check for updates. Check your internet connection and try again.",
                        L"TypeTide update", MB_OK | MB_ICONWARNING);
        }
        return 0;
    case kMsgBackendHealth: {
        auto* result = (translator::HealthCheckResult*)lp;
        EnableWindow(ctrl(kBackendTest), TRUE);
        SetWindowTextW(ctrl(kBackendTest), L"Test connection");
        std::wstring status = result->message;
        if (result->ok) {
            status += L"  ";
            if (result->firstTokenMs >= 0)
                status += L"First token " + std::to_wstring(result->firstTokenMs) + L" ms · ";
            status += L"Total " + std::to_wstring(result->totalMs) + L" ms";
        }
        SetWindowTextW(ctrl(kBackendStatus), status.c_str());
        g.backendReady = result->ok;
        updateFirstRunCompletion();
        delete result;
        return 0;
    }
    case WM_NOTIFY: {
        auto* hdr = (NMHDR*)lp;
        if (!g.highContrast && hdr->code == NM_CUSTOMDRAW) {
            wchar_t cls[32]{};
            GetClassNameW(hdr->hwndFrom, cls, (int)std::size(cls));
            if (_wcsicmp(cls, L"Button") == 0)
                return drawStyledButton(reinterpret_cast<NMCUSTOMDRAW*>(lp));
        }
        if (hdr->idFrom == kAboutLinks && (hdr->code == NM_CLICK || hdr->code == NM_RETURN)) {
            auto* link = (NMLINK*)lp;
            ShellExecuteW(nullptr, L"open", link->item.szUrl, nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        break;
    }
    case WM_MEASUREITEM: {
        auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
        if (item->CtlType == ODT_COMBOBOX) {
            item->itemHeight = px(form::MenuItemHeight);
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM: {
        auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (form::drawItem(*item)) return TRUE;
        if (item->CtlID != kTab || item->itemID == (UINT)-1) break;
        const bool selected = (item->itemState & ODS_SELECTED) != 0;
        HBRUSH bg = CreateSolidBrush(selected ? (g.dark ? RGB(0x3A, 0x31, 0x59) : RGB(0xE4, 0xDF, 0xFF)) : navBg());
        FillRect(item->hDC, &item->rcItem, bg);
        DeleteObject(bg);
        if (selected) {
            RECT accent{item->rcItem.left, item->rcItem.top + px(7),
                        item->rcItem.left + px(3), item->rcItem.bottom - px(7)};
            HBRUSH ab = CreateSolidBrush(ui::Accent);
            FillRect(item->hDC, &accent, ab);
            DeleteObject(ab);
        }
        wchar_t text[64]{};
        SendMessageW(item->hwndItem, LB_GETTEXT, item->itemID, (LPARAM)text);
        SetBkMode(item->hDC, TRANSPARENT);
        SetTextColor(item->hDC, textColor());
        SelectObject(item->hDC, g.font);
        RECT tr = item->rcItem;
        if (g.tabImages && item->itemID < 8) {
            ImageList_Draw(g.tabImages, static_cast<int>(item->itemID), item->hDC,
                           item->rcItem.left + px(16), item->rcItem.top + px(13), ILD_TRANSPARENT);
        }
        tr.left += px(42);
        DrawTextW(item->hDC, text, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        return TRUE;
    }
    case WM_CTLCOLORSTATIC: {
        if (HBRUSH brush = form::colorControl((HWND)lp, (HDC)wp)) return (LRESULT)brush;
        wchar_t cls[32]{};
        GetClassNameW((HWND)lp, cls, (int)std::size(cls));
        if (_wcsicmp(cls, L"Edit") == 0) {
            SetBkColor((HDC)wp, g.dark ? RGB(0x25, 0x25, 0x28) : RGB(0xEE, 0xEE, 0xF1));
            SetTextColor((HDC)wp, secondaryColor());
            return (LRESULT)pageBrush();
        }
        SetBkMode((HDC)wp, TRANSPARENT);
        SetTextColor((HDC)wp, textColor());
        return (LRESULT)controlBackground((HWND)lp);
    }
    case WM_CTLCOLOREDIT: {
        if (HBRUSH brush = form::colorControl((HWND)lp, (HDC)wp)) return (LRESULT)brush;
        SetBkColor((HDC)wp, g.dark ? RGB(0x2B, 0x2B, 0x2E) : RGB(0xFF, 0xFF, 0xFF));
        SetTextColor((HDC)wp, textColor());
        return (LRESULT)editBrush();
    }
    case WM_CTLCOLORBTN:
        SetBkMode((HDC)wp, TRANSPARENT);
        SetTextColor((HDC)wp, textColor());
        return (LRESULT)pageBrush();
    case WM_CTLCOLORLISTBOX:
        SetBkColor((HDC)wp, navBg());
        SetTextColor((HDC)wp, textColor());
        return (LRESULT)navBrush();
    case WM_ERASEBKGND: {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, pageBrush());
        RECT nav{0, 0, px(184), rc.bottom};
        FillRect((HDC)wp, &nav, navBrush());
        // Backend 页的字段卡片：浅一档的圆角面板，把四个字段在视觉上分组
        if (g.currentPage == 1 && g.backendCard.right > g.backendCard.left) {
            ui::Palette th = ui::palette(g.dark, g.highContrast);
            RECT card{px(g.backendCard.left), px(g.backendCard.top),
                      px(g.backendCard.right), px(g.backendCard.bottom)};
            HBRUSH surface = CreateSolidBrush(th.surface);
            HGDIOBJ oldBrush = SelectObject((HDC)wp, surface);
            HGDIOBJ oldPen = SelectObject((HDC)wp, GetStockObject(NULL_PEN));
            RoundRect((HDC)wp, card.left, card.top, card.right + 1, card.bottom + 1, px(10), px(10));
            SelectObject((HDC)wp, oldPen);
            SelectObject((HDC)wp, oldBrush);
            // 1px 描边（内缩画，避免圆角画刷溢出）
            HPEN outline = CreatePen(PS_SOLID, 1, th.divider);
            HGDIOBJ oldOutline = SelectObject((HDC)wp, outline);
            HGDIOBJ oldFill = SelectObject((HDC)wp, GetStockObject(NULL_BRUSH));
            RoundRect((HDC)wp, card.left, card.top, card.right, card.bottom, px(10), px(10));
            SelectObject((HDC)wp, oldOutline);
            SelectObject((HDC)wp, oldFill);
            DeleteObject(outline);
            DeleteObject(surface);
        }
        return 1;
    }
    case WM_SETTINGCHANGE:
        g.dark = systemUsesDarkMode();
        g.highContrast = ui::highContrastEnabled();
        applyTheme(hwnd);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g.font) { DeleteObject(g.font); g.font = nullptr; }
        if (g.fontBold) { DeleteObject(g.fontBold); g.fontBold = nullptr; }
        if (g.fontTitle) { DeleteObject(g.fontTitle); g.fontTitle = nullptr; }
        if (g.tabImages) { ImageList_Destroy(g.tabImages); g.tabImages = nullptr; }
        g.hwnd = nullptr;
        g.pages.clear();
        g.layouts.clear();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

namespace SettingsWindow {

bool testNavigationIconColors(bool dark, UINT dpi) {
    const bool previousDark = g.dark;
    const bool previousHighContrast = g.highContrast;
    const UINT previousDpi = g.dpi;
    g.dark = dark;
    g.highContrast = false;
    g.dpi = dpi;
    const COLORREF expected = textColor();
    HIMAGELIST images = createTabImages();
    const int size = px(16);
    g.dark = previousDark;
    g.highContrast = previousHighContrast;
    g.dpi = previousDpi;
    if (!images) return false;
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!dc || !bitmap) {
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        ImageList_Destroy(images);
        return false;
    }
    HGDIOBJ old = SelectObject(dc, bitmap);
    bool ok = ImageList_GetImageCount(images) == 8;
    // Include normal and selected backgrounds in both themes. The former
    // magenta-only test could not distinguish transparency from leaked tiles.
    const COLORREF backgrounds[] = {RGB(0x19, 0x19, 0x1B), RGB(0x3A, 0x31, 0x59),
                                   RGB(0xF0, 0xEF, 0xF5), RGB(0xE4, 0xDF, 0xFF)};
    for (COLORREF backgroundColor : backgrounds) {
        HBRUSH background = CreateSolidBrush(backgroundColor);
        for (int i = 0; i < 8 && ok; ++i) {
            RECT rc{0, 0, size, size};
            FillRect(dc, &rc, background);
            ok = ImageList_Draw(images, i, dc, 0, 0, ILD_TRANSPARENT) != FALSE;
            int strokes = 0;
            for (int y = 0; y < size; ++y) {
                for (int x = 0; x < size; ++x) {
                    const COLORREF color = GetPixel(dc, x, y);
                    if (color == backgroundColor) continue;
                    ++strokes;
                    if (color != expected) ok = false;
                }
            }
            if (!strokes || GetPixel(dc, 0, 0) != backgroundColor) ok = false;
        }
        DeleteObject(background);
    }
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ImageList_Destroy(images);
    return ok;
}

void refreshShortcutStatus() {
    if (!g.hwnd || !ctrl(kHkStatus)) return;
    const auto& app = App::shared();
    g.shortcutsReady = app.shortcutsRegistered();
    std::wstring status;
    if (!Settings::shared().enabled) status = L"TypeTide is disabled. Enable it to register shortcuts.";
    else if (!app.shortcutIssue().empty())
        status = app.shortcutIssue() + L"\nChoose another combination, then click Verify shortcuts to retry.";
    else if (g.shortcutsReady) status = L"Every configured shortcut is registered and ready.";
    else status = L"Set at least one shortcut to use TypeTide from the keyboard.";
    SetWindowTextW(ctrl(kHkStatus), status.c_str());
}

void open(HWND appWindow, bool showShortcuts) {
    if (g.hwnd) {
        refreshShortcutStatus();
        if (showShortcuts) {
            SendMessageW(ctrl(kTab), LB_SETCURSEL, 3, 0);
            showPage(3);
        }
        ShowWindow(g.hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(g.hwnd);
        return;
    }
    g.appWindow = appWindow;
    enableCommonControlDarkMode();

    static bool registered = [] {
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_TAB_CLASSES | ICC_HOTKEY_CLASS | ICC_LINK_CLASS |
                                                  ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&icc);
        WNDCLASSW wc{};
        wc.lpfnWndProc = wndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClassName;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDI_APPICON));
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        RegisterClassW(&wc);
        return true;
    }();
    (void)registered;

    g.dpi = GetDpiForSystem();
    int w = px(820), h = px(560);
    RECT wr{0, 0, w, h};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);

    g.hwnd = CreateWindowExW(0, kClassName, L"TypeTide Settings",
                             WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             wr.right - wr.left, wr.bottom - wr.top,
                             nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g.hwnd) return;
    g.dpi = GetDpiForWindow(g.hwnd);
    // Win11 圆角窗口（Win10 上静默忽略）
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(g.hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    g.dark = systemUsesDarkMode();
    g.highContrast = ui::highContrastEnabled();
    // Mica 背景材质（Win11 22H2+；旧系统调用失败时保持现有自绘背景，
    // 不影响 Win10）。Mica 生效时客户区必须延伸进标题栏才会透出材质，
    // 这里同时启用 DWMWCP_ROUND + ExtendFrame；WM_ERASEBKGND 的自绘
    // 背景会在 Mica 失效的系统上照常工作。
    if (!g.highContrast) {
        int backdrop = 2;  // DWMSBT_MICA
        HRESULT hr = DwmSetWindowAttribute(g.hwnd, DWMWA_SYSTEMBACKDROP_TYPE,
                                           &backdrop, sizeof(backdrop));
        g.navBackdrop = SUCCEEDED(hr);
        if (g.navBackdrop) {
            // 把一像素边框融进背景（Win11 默认边框是亮色，Mica 下显得突兀）
            COLORREF border = g.dark ? 0 : RGB(0xFF, 0xFF, 0xFF);
            DwmSetWindowAttribute(g.hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
        }
    }

    HICON smallIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON),
                                        IMAGE_ICON, px(16), px(16), LR_DEFAULTCOLOR | LR_SHARED);
    SendMessageW(g.hwnd, WM_SETICON, ICON_BIG,
                 (LPARAM)LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON)));
    SendMessageW(g.hwnd, WM_SETICON, ICON_SMALL, (LPARAM)smallIcon);

    createFonts();

    g.loading = true;

    // Windows 11-style left navigation.
    HWND tab = CreateWindowExW(0, L"LISTBOX", L"",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY |
                                   LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                               0, px(14), px(184), h - px(28), g.hwnd,
                               (HMENU)kTab, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(tab, WM_SETFONT, (WPARAM)g.font, TRUE);
    SendMessageW(tab, LB_SETITEMHEIGHT, 0, px(42));
    g.tabImages = createTabImages();
    const wchar_t* names[] = {L"General", L"Translation", L"Languages", L"Shortcuts",
                              L"Behavior", L"Excluded apps", L"Diagnostics", L"About"};
    for (int i = 0; i < 8; ++i) {
        SendMessageW(tab, LB_ADDSTRING, 0, (LPARAM)names[i]);
    }
    SendMessageW(tab, LB_SETCURSEL, 0, 0);

    buildPages();
    applyTheme(g.hwnd);
    const int initialPage = showShortcuts ? 3 : 0;
    SendMessageW(ctrl(kTab), LB_SETCURSEL, initialPage, 0);
    showPage(initialPage);
    g.loading = false;
    refreshShortcutStatus();

    SetForegroundWindow(g.hwnd);
}

} // namespace SettingsWindow
