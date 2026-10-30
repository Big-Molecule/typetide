#pragma once

#include <windows.h>
#include <algorithm>
#include <vector>

// Logical (96-DPI) geometry. Rows stack vertically; items flow left to right.
// Native combo drop-down height is separate from its visible row height.
namespace layout {

constexpr int kNavWidth = 184;
constexpr int kPageX = 222;
constexpr int kPageTop = 82;
constexpr int kRightMargin = 38;
constexpr int kBaseHeight = 560;

struct Item {
    HWND hwnd = nullptr;
    int width = 0, height = 0;
    bool stretch = false;
    int dropHeight = 0;
    bool growHeight = false;
};

struct Row {
    std::vector<Item> items;
    int spacingAfter = 16;
    int inset = 0;
    int gap = 12;
};

struct Page {
    std::vector<Row> rows;
    int top = kPageTop;
    int cardFirst = -1, cardLast = -1;
};

inline Row& addRow(Page& page, int spacingAfter = 16, int inset = 0) {
    page.rows.push_back({{}, spacingAfter, inset});
    return page.rows.back();
}

inline int rowHeight(const Row& row, int extraHeight = 0) {
    int height = 0;
    for (const auto& item : row.items)
        height = std::max(height, item.height + (item.growHeight ? extraHeight : 0));
    return height;
}

// The callback receives the visible bounds and requested native window height.
// Keeping this calculation independent of HWND operations makes it testable.
template<class Place>
int arrange(const Page& page, int pageX, int width, int extraHeight, Place place) {
    int y = page.top;
    for (const auto& row : page.rows) {
        int used = 2 * row.inset + std::max(0, (int)row.items.size() - 1) * row.gap;
        int flexible = 0;
        for (const auto& item : row.items) {
            used += item.width;
            if (item.stretch) ++flexible;
        }
        int remaining = std::max(0, width - used);
        const int baseHeight = rowHeight(row);
        const int height = rowHeight(row, extraHeight);
        const bool growingRow = std::any_of(row.items.begin(), row.items.end(),
            [](const Item& item) { return item.growHeight; });
        int x = pageX + row.inset;
        for (const auto& item : row.items) {
            int w = item.width;
            if (item.stretch) {
                const int share = remaining / flexible--;
                w += share;
                remaining -= share;
            }
            const int h = item.height + (item.growHeight ? extraHeight : 0);
            // A non-growing button next to a growing list stays at its top.
            const int top = y + (growingRow ? 0 : std::max(0, baseHeight - item.height) / 2);
            RECT bounds{x, top, x + w, top + h};
            place(item, bounds, item.dropHeight ? item.dropHeight : h);
            x += w + row.gap;
        }
        y += height + row.spacingAfter;
    }
    return y;
}

inline RECT cardRect(const Page& page, int pageX, int width, int padding = 20) {
    if (page.cardFirst < 0 || page.cardLast < page.cardFirst ||
        page.cardLast >= (int)page.rows.size()) return {};
    int y = page.top;
    RECT card{};
    for (int r = 0; r <= page.cardLast; ++r) {
        if (r == page.cardFirst) card = {pageX, y - padding, pageX + width, 0};
        const int bottom = y + rowHeight(page.rows[r]);
        if (r == page.cardLast) card.bottom = bottom + padding;
        y = bottom + page.rows[r].spacingAfter;
    }
    return card;
}

} // namespace layout
