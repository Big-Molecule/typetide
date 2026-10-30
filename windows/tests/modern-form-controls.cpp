#include "ModernFormControls.h"
#include "UITheme.h"
#include <commctrl.h>
#include <iostream>
#include <string>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {
form::Theme theme;
int changes = 0;
form::Theme getTheme(HWND) { return theme; }
std::wstring hotkeyText(HWND hwnd) {
    return SendMessageW(hwnd, HKM_GETHOTKEY, 0, 0) ? L"Ctrl+K" : L"None";
}
LRESULT CALLBACK parentProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DRAWITEM && form::drawItem(*reinterpret_cast<DRAWITEMSTRUCT*>(lp))) return TRUE;
    if (msg == WM_MEASUREITEM) {
        auto& item = *reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
        if (item.CtlType == ODT_COMBOBOX) { item.itemHeight = form::MenuItemHeight; return TRUE; }
    }
    if (msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORSTATIC)
        if (const auto brush = form::colorControl((HWND)lp, (HDC)wp)) return (LRESULT)brush;
    if (msg == WM_COMMAND && HIWORD(wp) == EN_CHANGE) ++changes;
    return DefWindowProcW(hwnd, msg, wp, lp);
}
std::wstring text(HWND hwnd) {
    const int n = GetWindowTextLengthW(hwnd);
    std::wstring value(n + 1, L'\0');
    GetWindowTextW(hwnd, value.data(), n + 1);
    value.resize(n);
    return value;
}
}

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_HOTKEY_CLASS};
    InitCommonControlsEx(&icc);
    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        if (!ok) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
    };
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TypeTideFormTest";
    wc.lpfnWndProc = parentProc;
    RegisterClassW(&wc);
    HWND parent = CreateWindowW(wc.lpszClassName, L"Form tests", WS_OVERLAPPEDWINDOW,
                                 0, 0, 700, 400, nullptr, nullptr, wc.hInstance, nullptr);
    check(parent != nullptr, "test host created");
    const UINT dpi = GetDpiForWindow(parent);
    const auto px = [dpi](int n) { return MulDiv(n, (int)dpi, 96); };
    HFONT font = CreateFontW(-px(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HWND edit = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                              10, 10, px(280), px(form::Height), parent, (HMENU)101, wc.hInstance, nullptr);
    SendMessageW(edit, WM_SETFONT, (WPARAM)font, TRUE);
    form::attachEdit(edit, getTheme);
    HWND password = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_PASSWORD,
                                  10, 60, px(280), px(form::Height), parent, (HMENU)102, wc.hInstance, nullptr);
    SendMessageW(password, WM_SETFONT, (WPARAM)font, TRUE);
    form::attachEdit(password, getTheme);
    const auto combo = [&](bool editable, int id) {
        HWND c = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | (editable ? CBS_DROPDOWN : CBS_DROPDOWNLIST),
                310, id == 103 ? 10 : 70, px(260), px(240), parent, (HMENU)(INT_PTR)id, wc.hInstance, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
        form::attachCombo(c, getTheme);
        for (const auto* item : {L"中文 (Chinese)", L"English", L"Custom & local"})
            SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)item);
        SendMessageW(c, CB_SETCURSEL, 0, 0);
        return c;
    };
    HWND select = combo(false, 103), model = combo(true, 104);
    HWND hotkey = CreateWindowW(HOTKEY_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                10, 120, px(280), px(form::Height), parent, (HMENU)105, wc.hInstance, nullptr);
    SendMessageW(hotkey, WM_SETFONT, (WPARAM)font, TRUE);
    form::attachHotkey(hotkey, getTheme, hotkeyText);
    SendMessageW(hotkey, HKM_SETHOTKEY, MAKEWORD('K', HOTKEYF_CONTROL), 0);
    check(SendMessageW(hotkey, HKM_GETHOTKEY, 0, 0) == MAKEWORD('K', HOTKEYF_CONTROL), "native hotkey recording retained");
    check(!(GetWindowLongPtrW(hotkey, GWL_EXSTYLE) & WS_EX_CLIENTEDGE), "hotkey legacy 3D edge removed");
    SetWindowTextW(edit, L"https://example.test/v1/中文");
    check(text(edit) == L"https://example.test/v1/中文", "Unicode input roundtrip");
    check(changes > 0, "native EN_CHANGE notification retained");
    check(!(GetWindowLongPtrW(edit, GWL_STYLE) & WS_BORDER), "legacy border removed");
    check(SendMessageW(edit, WM_NCHITTEST, 0, 0) == HTCLIENT, "inset padding remains clickable");
    SendMessageW(edit, EM_SETSEL, 0, -1);
    SendMessageW(edit, EM_REPLACESEL, TRUE, (LPARAM)L"replacement");
    check(text(edit) == L"replacement", "native selection and replacement retained");
    check(SendMessageW(edit, EM_CANUNDO, 0, 0) != 0, "native undo available");
    SendMessageW(edit, EM_UNDO, 0, 0);
    check(text(edit) == L"https://example.test/v1/中文", "native undo restores Unicode text");
    SetWindowTextW(password, L"test-secret");
    check(SendMessageW(password, EM_GETPASSWORDCHAR, 0, 0) != 0, "password masking retained");
    COMBOBOXINFO info{sizeof(info)};
    GetComboBoxInfo(model, &info);
    SetWindowTextW(model, L"manually-typed-model");
    check(text(model) == L"manually-typed-model", "editable model name retained");
    check(SendMessageW(select, CB_GETCOUNT, 0, 0) == 3, "owner-drawn combo retains string storage");
    SendMessageW(select, CB_SETCURSEL, 1, 0);
    check(SendMessageW(select, CB_GETCURSEL, 0, 0) == 1, "selection messages retained");
    SendMessageW(select, WM_KEYDOWN, VK_DOWN, 0);
    check(SendMessageW(select, CB_GETCURSEL, 0, 0) == 2, "native arrow-key navigation retained");
    check(SendMessageW(select, WM_GETDLGCODE, VK_TAB, 0) != DLGC_WANTALLKEYS, "Tab is not trapped");
    for (bool dark : {false, true}) {
        theme = {dark, false, ui::palette(dark).background};
        for (HWND c : {edit, password, select, model, hotkey}) form::refresh(c);
        RECT bounds{};
        GetWindowRect(select, &bounds);
        check(bounds.bottom - bounds.top == px(form::Height), "combo height matches declared layout");
        GetClientRect(edit, &bounds);
        check(bounds.right == px(280) - 2 * px(12), "input horizontal padding");
        HDC dc = GetDC(parent);
        for (bool enabled : {true, false}) {
            EnableWindow(edit, enabled);
            const auto brush = form::colorControl(edit, dc);
            LOGBRUSH lb{};
            GetObjectW(brush, sizeof(lb), &lb);
            check(lb.lbColor == GetBkColor(dc), "edit brush and text background agree");
            const auto expected = dark ? (enabled ? RGB(0x32,0x32,0x36) : RGB(0x29,0x29,0x2C))
                                      : (enabled ? RGB(255,255,255) : RGB(0xEA,0xEA,0xED));
            check(lb.lbColor == expected, "light/dark/disabled input colors");
        }
        EnableWindow(edit, TRUE);
        form::colorControl(info.hwndItem, dc);
        check(GetBkColor(dc) == (dark ? RGB(0x32,0x32,0x36) : RGB(255,255,255)), "model child matches field fill");
        HDC memory = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, px(260), px(form::Height));
        const auto old = SelectObject(memory, bitmap);
        SendMessageW(select, WM_PRINTCLIENT, (WPARAM)memory, PRF_CLIENT);
        check(GetPixel(memory, px(100), px(5)) == (dark ? RGB(0x32,0x32,0x36) : RGB(255,255,255)), "combo custom surface rendered");
        SelectObject(memory, old);
        DeleteObject(bitmap);
        DeleteDC(memory);
        ReleaseDC(parent, dc);
        for (int i = 0; i < 10; ++i) form::refresh(select);
        GetWindowRect(select, &bounds);
        check(bounds.bottom - bounds.top == px(form::Height), "theme refresh has no height drift");
    }
    theme.highContrast = true;
    HDC dc = GetDC(parent);
    check(form::colorControl(edit, dc) == GetSysColorBrush(COLOR_WINDOW), "high contrast uses system brush");
    check(GetTextColor(dc) == GetSysColor(COLOR_WINDOWTEXT), "high contrast uses system text");
    ReleaseDC(parent, dc);
    const HWND oldEdit = edit;
    DestroyWindow(parent);
    check(!IsWindow(oldEdit), "subclass cleanup preserves native control lifetime");
    DeleteObject(font);
    std::cout << (failures ? "Form control tests failed\n" : "All form control tests passed\n");
    return failures ? 1 : 0;
}
