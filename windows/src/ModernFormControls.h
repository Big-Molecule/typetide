#pragma once

#include <windows.h>
#include <string>

// A visual layer over standard Win32 controls. Keep their HWNDs, messages,
// notifications, accessibility providers, IME and editing behavior intact.
namespace form {

constexpr int Height = 36;
constexpr int MenuItemHeight = 34;

struct Theme {
    bool dark = false;
    bool highContrast = false;
    COLORREF backdrop = RGB(243, 243, 243);
};
using ThemeProvider = Theme (*)(HWND);
using TextProvider = std::wstring (*)(HWND);

// Create standalone edits without WS_BORDER / WS_EX_CLIENTEDGE: EDIT caches
// these during creation, so removing them afterwards can leave a second frame.
void attachEdit(HWND edit, ThemeProvider theme);
// Create combos with CBS_OWNERDRAWFIXED | CBS_HASSTRINGS before attaching.
void attachCombo(HWND combo, ThemeProvider theme);
void attachHotkey(HWND hotkey, ThemeProvider theme, TextProvider text);
void refresh(HWND control);
bool drawItem(const DRAWITEMSTRUCT& item);
// Handles editable combo children as well as standalone input fields.
HBRUSH colorControl(HWND control, HDC dc);

} // namespace form
