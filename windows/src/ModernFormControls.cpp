#include "ModernFormControls.h"
#include "UITheme.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <algorithm>
#include <string>

namespace form {
namespace {

enum class Kind { Edit, Combo, Editor, Hotkey };
struct State {
    Kind kind;
    HWND root;
    ThemeProvider theme;
    bool hover = false;
    TextProvider text = nullptr;
};
constexpr UINT_PTR kSubclass = 0x5454464D;
LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

State* state(HWND hwnd) {
    DWORD_PTR value = 0;
    return GetWindowSubclass(hwnd, proc, kSubclass, &value)
        ? reinterpret_cast<State*>(value) : nullptr;
}
int scale(HWND hwnd, int value) { return MulDiv(value, (int)GetDpiForWindow(hwnd), 96); }

struct Colors {
    COLORREF fill, border, hotBorder, text, muted, accent, selected;
};
Colors colors(const Theme& theme, bool enabled) {
    if (theme.highContrast) {
        return {GetSysColor(COLOR_WINDOW), GetSysColor(COLOR_WINDOWTEXT),
                GetSysColor(COLOR_HIGHLIGHT),
                GetSysColor(enabled ? COLOR_WINDOWTEXT : COLOR_GRAYTEXT),
                GetSysColor(COLOR_GRAYTEXT), GetSysColor(COLOR_HIGHLIGHT),
                GetSysColor(COLOR_HIGHLIGHT)};
    }
    const auto palette = ui::palette(theme.dark);
    return {enabled ? (theme.dark ? RGB(0x32,0x32,0x36) : RGB(255,255,255))
                    : (theme.dark ? RGB(0x29,0x29,0x2C) : RGB(0xEA,0xEA,0xED)),
            theme.dark ? RGB(0x53,0x53,0x5B) : RGB(0xCF,0xCF,0xD7),
            theme.dark ? RGB(0x83,0x83,0x91) : RGB(0x9B,0x9B,0xAB),
            enabled ? palette.text : palette.secondary, palette.secondary,
            theme.dark ? RGB(0xB1,0x9D,0xFF) : ui::Accent,
            theme.dark ? RGB(0x49,0x40,0x65) : RGB(0xED,0xE9,0xFF)};
}

HBRUSH menuBrush(const Theme& t) {
    static HBRUSH dark = CreateSolidBrush(ui::palette(true).surface);
    static HBRUSH light = CreateSolidBrush(ui::palette(false).surface);
    return t.highContrast ? GetSysColorBrush(COLOR_WINDOW) : t.dark ? dark : light;
}

bool focused(HWND root) {
    const HWND focus = GetFocus();
    return focus == root || (focus && IsChild(root, focus));
}
void invalidate(HWND root) {
    RedrawWindow(root, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
}
void fill(HDC dc, const RECT& r, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &r, brush);
    DeleteObject(brush);
}
void rounded(HDC dc, const RECT& r, int radius, COLORREF background, COLORREF border) {
    HBRUSH brush = CreateSolidBrush(background);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    const auto oldBrush = SelectObject(dc, brush);
    const auto oldPen = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void frame(HWND hwnd, HDC dc, const RECT& r, const State& s) {
    const Theme theme = s.theme(s.root);
    const bool enabled = IsWindowEnabled(s.root) != FALSE;
    const auto c = colors(theme, enabled);
    fill(dc, r, theme.backdrop);
    const bool focus = enabled && focused(s.root);
    const bool open = s.kind == Kind::Combo && SendMessageW(hwnd, CB_GETDROPPEDSTATE, 0, 0);
    const COLORREF border = (focus || open) ? c.accent : s.hover && enabled ? c.hotBorder : c.border;
    if (theme.highContrast) {
        fill(dc, r, c.fill);
        HBRUSH brush = CreateSolidBrush(border);
        FrameRect(dc, &r, brush);
        DeleteObject(brush);
    } else {
        rounded(dc, r, scale(hwnd, 7), c.fill, border);
        if (focus || open) {
            RECT underline{r.left + scale(hwnd, 7), r.bottom - scale(hwnd, 2),
                           r.right - scale(hwnd, 7), r.bottom};
            fill(dc, underline, c.accent);
        }
    }
}

int textHeight(HWND hwnd) {
    HDC dc = GetDC(hwnd);
    HFONT font = (HFONT)SendMessageW(hwnd, WM_GETFONT, 0, 0);
    const auto old = font ? SelectObject(dc, font) : nullptr;
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    if (old) SelectObject(dc, old);
    ReleaseDC(hwnd, dc);
    return tm.tmHeight;
}

void positionEditor(HWND hwnd) {
    COMBOBOXINFO info{sizeof(info)};
    if (!GetComboBoxInfo(hwnd, &info) || !info.hwndItem || info.hwndItem == hwnd) return;
    // DROPDOWNLIST exposes a static item HWND on some Windows versions.
    wchar_t cls[32]{};
    GetClassNameW(info.hwndItem, cls, 32);
    if (_wcsicmp(cls, L"Edit") != 0) return;
    RECT r{};
    GetClientRect(hwnd, &r);
    const int h = textHeight(info.hwndItem);
    SetWindowPos(info.hwndItem, nullptr, scale(hwnd, 12), (r.bottom - h) / 2,
                 std::max(1, (int)r.right - scale(hwnd, 48)), h,
                 SWP_NOACTIVATE | SWP_NOZORDER);
    SendMessageW(info.hwndItem, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
}

std::wstring itemText(HWND combo, int index) {
    if (index < 0) return {};
    const int length = (int)SendMessageW(combo, CB_GETLBTEXTLEN, index, 0);
    if (length < 0) return {};
    std::wstring text(length + 1, L'\0');
    SendMessageW(combo, CB_GETLBTEXT, index, (LPARAM)text.data());
    text.resize(length);
    return text;
}

void paintField(HWND hwnd, HDC target, const State& s) {
    RECT r{};
    GetClientRect(hwnd, &r);
    if (r.right <= 0 || r.bottom <= 0) return;
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, r.right, r.bottom);
    const auto oldBitmap = SelectObject(dc, bitmap);
    frame(hwnd, dc, r, s);
    const auto c = colors(s.theme(hwnd), IsWindowEnabled(hwnd) != FALSE);
    const auto font = (HFONT)SendMessageW(hwnd, WM_GETFONT, 0, 0);
    const auto oldFont = font ? SelectObject(dc, font) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c.text);
    const bool hotkey = s.kind == Kind::Hotkey;
    if (hotkey || (GetWindowLongPtrW(hwnd, GWL_STYLE) & CBS_DROPDOWNLIST) == CBS_DROPDOWNLIST) {
        const auto text = hotkey ? s.text(hwnd)
            : itemText(hwnd, (int)SendMessageW(hwnd, CB_GETCURSEL, 0, 0));
        RECT tr{scale(hwnd, 12), 0, r.right - scale(hwnd, hotkey ? 12 : 38), r.bottom};
        DrawTextW(dc, text.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    if (!hotkey) {
        const int x = r.right - scale(hwnd, 20), y = r.bottom / 2;
        const bool open = SendMessageW(hwnd, CB_GETDROPPEDSTATE, 0, 0) != 0;
        const int d = scale(hwnd, 4), sign = open ? -1 : 1;
        HPEN pen = CreatePen(PS_SOLID, std::max(2, scale(hwnd, 1)),
                             IsWindowEnabled(hwnd) && (focused(hwnd) || open) ? c.accent : c.muted);
        const auto oldPen = SelectObject(dc, pen);
        MoveToEx(dc, x - d, y - sign * d / 2, nullptr);
        LineTo(dc, x, y + sign * d / 2);
        LineTo(dc, x + d, y - sign * d / 2);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    }
    if (oldFont) SelectObject(dc, oldFont);
    BitBlt(target, 0, 0, r.right, r.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

void paintEditFrame(HWND hwnd, const State& s) {
    HDC dc = GetWindowDC(hwnd);
    RECT r{};
    GetWindowRect(hwnd, &r);
    OffsetRect(&r, -r.left, -r.top);
    RECT client{};
    GetClientRect(hwnd, &client);
    POINT origin{};
    ClientToScreen(hwnd, &origin);
    RECT window{};
    GetWindowRect(hwnd, &window);
    OffsetRect(&client, origin.x - window.left, origin.y - window.top);
    ExcludeClipRect(dc, client.left, client.top, client.right, client.bottom);
    frame(hwnd, dc, r, s);
    ReleaseDC(hwnd, dc);
}

void track(HWND hwnd, State& s) {
    if (s.hover) return;
    s.hover = true;
    TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
    TrackMouseEvent(&track);
    invalidate(s.root);
}

LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    auto& s = *reinterpret_cast<State*>(ref);
    if (msg == WM_NCDESTROY) {
        const LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);
        RemoveWindowSubclass(hwnd, proc, kSubclass);
        delete &s;
        return result;
    }
    if (msg == WM_MOUSEMOVE) track(hwnd, s);
    if (msg == WM_MOUSELEAVE) { s.hover = false; invalidate(s.root); }
    // Padding belongs to the input hit target, not to a draggable/non-client border.
    if (s.kind == Kind::Edit && msg == WM_NCHITTEST) return HTCLIENT;
    if (s.kind == Kind::Edit && msg == WM_NCCALCSIZE) {
        auto& r = wp ? reinterpret_cast<NCCALCSIZE_PARAMS*>(lp)->rgrc[0]
                     : *reinterpret_cast<RECT*>(lp);
        const int pad = std::max(scale(hwnd, 3), ((int)(r.bottom - r.top) - textHeight(hwnd)) / 2);
        r.left += scale(hwnd, 12); r.right -= scale(hwnd, 12);
        r.top += pad; r.bottom -= pad;
        return 0;
    }
    if (s.kind == Kind::Edit && msg == WM_NCPAINT) { paintEditFrame(hwnd, s); return 0; }
    if (s.kind == Kind::Hotkey && msg == WM_NCCALCSIZE) return 0;
    if (s.kind == Kind::Hotkey || s.kind == Kind::Combo) {
        if (msg == WM_ERASEBKGND || msg == WM_NCPAINT) return 1;
        if (msg == WM_PAINT) {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            paintField(hwnd, dc, s);
            EndPaint(hwnd, &ps);
            return 0;
        }
        if (msg == WM_PRINTCLIENT) { paintField(hwnd, (HDC)wp, s); return 0; }
    }
    if (s.kind == Kind::Combo) {
        if (msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORSTATIC)
            if (HBRUSH brush = colorControl((HWND)lp, (HDC)wp)) return (LRESULT)brush;
        if (msg == WM_CTLCOLORLISTBOX) {
            const auto theme = s.theme(s.root);
            SetBkColor((HDC)wp, ui::palette(theme.dark, theme.highContrast).surface);
            SetTextColor((HDC)wp, ui::palette(theme.dark, theme.highContrast).text);
            return (LRESULT)menuBrush(theme);
        }
    }
    const LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);
    if (s.kind == Kind::Combo && (msg == WM_SIZE || msg == WM_WINDOWPOSCHANGED || msg == WM_SETFONT))
        positionEditor(hwnd);
    if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS || msg == WM_ENABLE || msg == WM_SETTEXT ||
        msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR || msg == WM_SYSKEYDOWN || msg == WM_MOUSEWHEEL ||
        msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP || msg == CB_RESETCONTENT ||
        msg == CB_DELETESTRING || msg == CB_SETCURSEL || msg == CB_SHOWDROPDOWN ||
        msg == HKM_SETHOTKEY || msg == WM_THEMECHANGED) invalidate(s.root);
    if (s.kind == Kind::Edit && msg == WM_SETFONT)
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOACTIVATE | SWP_FRAMECHANGED);
    return result;
}

void attach(HWND hwnd, Kind kind, HWND root, ThemeProvider theme) {
    if (!hwnd || state(hwnd)) return;
    auto* s = new State{kind, root, theme};
    if (!SetWindowSubclass(hwnd, proc, kSubclass, (DWORD_PTR)s)) delete s;
}

} // namespace

void attachEdit(HWND edit, ThemeProvider theme) {
    SetWindowLongPtrW(edit, GWL_STYLE, GetWindowLongPtrW(edit, GWL_STYLE) & ~WS_BORDER);
    SetWindowLongPtrW(edit, GWL_EXSTYLE, GetWindowLongPtrW(edit, GWL_EXSTYLE) & ~WS_EX_CLIENTEDGE);
    attach(edit, Kind::Edit, edit, theme);
    SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    refresh(edit);
}

void attachCombo(HWND combo, ThemeProvider theme) {
    SetWindowLongPtrW(combo, GWL_STYLE, GetWindowLongPtrW(combo, GWL_STYLE) | WS_CLIPCHILDREN);
    attach(combo, Kind::Combo, combo, theme);
    COMBOBOXINFO info{sizeof(info)};
    if (GetComboBoxInfo(combo, &info)) {
        wchar_t cls[32]{};
        GetClassNameW(info.hwndItem, cls, 32);
        if (_wcsicmp(cls, L"Edit") == 0) attach(info.hwndItem, Kind::Editor, combo, theme);
        if (info.hwndList) {
            const Theme t = theme(combo);
            SetWindowTheme(info.hwndList, t.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
            const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUNDSMALL;
            DwmSetWindowAttribute(info.hwndList, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
        }
    }
    refresh(combo);
}

void attachHotkey(HWND hotkey, ThemeProvider theme, TextProvider text) {
    SetWindowLongPtrW(hotkey, GWL_EXSTYLE, GetWindowLongPtrW(hotkey, GWL_EXSTYLE) & ~WS_EX_CLIENTEDGE);
    SetWindowLongPtrW(hotkey, GWL_STYLE, GetWindowLongPtrW(hotkey, GWL_STYLE) & ~WS_BORDER);
    attach(hotkey, Kind::Hotkey, hotkey, theme);
    if (auto* s = state(hotkey)) s->text = text;
    refresh(hotkey);
}

void refresh(HWND control) {
    State* s = state(control);
    if (!s || s->kind == Kind::Editor) return;
    if (s->kind == Kind::Combo) {
        // The native combo adds its own 6-pixel selection-frame allowance.
        SendMessageW(control, CB_SETITEMHEIGHT, (WPARAM)-1, scale(control, Height) - 6);
        SendMessageW(control, CB_SETITEMHEIGHT, 0, scale(control, MenuItemHeight));
        SendMessageW(control, CB_SETMINVISIBLE, 7, 0);
        COMBOBOXINFO info{sizeof(info)};
        if (GetComboBoxInfo(control, &info)) {
            const auto t = s->theme(control);
            SetWindowTheme(info.hwndList, t.dark && !t.highContrast ? L"DarkMode_Explorer" : L"Explorer", nullptr);
            if (state(info.hwndItem)) SetWindowTheme(info.hwndItem, L"", L"");
        }
        positionEditor(control);
    } else SetWindowTheme(control, L"", L""); // no second themed border inside the inset client
    SetWindowPos(control, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);
    invalidate(control);
}

HBRUSH colorControl(HWND control, HDC dc) {
    const State* s = state(control);
    if (!s) return nullptr;
    const Theme t = s->theme(s->root);
    const bool enabled = IsWindowEnabled(s->root) != FALSE;
    const auto c = colors(t, enabled);
    SetBkColor(dc, c.fill);
    SetTextColor(dc, c.text);
    if (t.highContrast) return GetSysColorBrush(COLOR_WINDOW);
    // Process-lifetime shared brushes, not one allocation per keystroke.
    static HBRUSH light = CreateSolidBrush(RGB(255,255,255));
    static HBRUSH dark = CreateSolidBrush(RGB(0x32,0x32,0x36));
    static HBRUSH disabledLight = CreateSolidBrush(RGB(0xEA,0xEA,0xED));
    static HBRUSH disabledDark = CreateSolidBrush(RGB(0x29,0x29,0x2C));
    return enabled ? (t.dark ? dark : light) : (t.dark ? disabledDark : disabledLight);
}

bool drawItem(const DRAWITEMSTRUCT& item) {
    const State* s = state(item.hwndItem);
    if (!s || s->kind != Kind::Combo || item.CtlType != ODT_COMBOBOX) return false;
    const Theme t = s->theme(s->root);
    const auto c = colors(t, IsWindowEnabled(item.hwndItem) != FALSE);
    const auto palette = ui::palette(t.dark, t.highContrast);
    const int saved = SaveDC(item.hDC);
    // COMBOBOX sometimes requests the selection field synchronously while
    // handling keyboard input. Do not briefly paint a menu tile/check there.
    const bool field = (item.itemState & ODS_COMBOBOXEDIT) != 0;
    fill(item.hDC, item.rcItem, field ? c.fill : palette.surface);
    const bool selected = !field && (item.itemState & ODS_SELECTED) != 0;
    RECT tile = item.rcItem;
    InflateRect(&tile, -scale(item.hwndItem, 4), -scale(item.hwndItem, 2));
    if (selected) rounded(item.hDC, tile, scale(item.hwndItem, 5), c.selected, c.selected);
    if (item.itemID != (UINT)-1) {
        const auto text = itemText(item.hwndItem, (int)item.itemID);
        const HFONT font = (HFONT)SendMessageW(item.hwndItem, WM_GETFONT, 0, 0);
        if (font) SelectObject(item.hDC, font);
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, selected && t.highContrast ? GetSysColor(COLOR_HIGHLIGHTTEXT) : c.text);
        RECT tr = item.rcItem;
        tr.left += scale(item.hwndItem, field ? 8 : 32); tr.right -= scale(item.hwndItem, 12);
        DrawTextW(item.hDC, text.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        if (!field && (int)item.itemID == (int)SendMessageW(item.hwndItem, CB_GETCURSEL, 0, 0)) {
            HPEN pen = CreatePen(PS_SOLID, std::max(1, scale(item.hwndItem, 2)),
                selected && t.highContrast ? GetSysColor(COLOR_HIGHLIGHTTEXT) : c.accent);
            SelectObject(item.hDC, pen);
            const int x = item.rcItem.left + scale(item.hwndItem, 12);
            const int y = (item.rcItem.top + item.rcItem.bottom) / 2;
            MoveToEx(item.hDC, x, y, nullptr);
            LineTo(item.hDC, x + scale(item.hwndItem, 3), y + scale(item.hwndItem, 3));
            LineTo(item.hDC, x + scale(item.hwndItem, 8), y - scale(item.hwndItem, 3));
            SelectObject(item.hDC, GetStockObject(BLACK_PEN));
            DeleteObject(pen);
        }
        if ((item.itemState & ODS_FOCUS) && t.highContrast) DrawFocusRect(item.hDC, &tile);
    }
    RestoreDC(item.hDC, saved);
    return true;
}

} // namespace form
