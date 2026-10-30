#include "SettingsLayout.h"
#include <iostream>

int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        if (!ok) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
    };
    for (int width : {560, 760, 1100}) {
        layout::Page page;
        page.top = 100;
        page.cardFirst = 0;
        page.cardLast = 1;
        layout::addRow(page, 16, 20).items = {
            {nullptr, 100, 20}, {nullptr, 0, 32, true}};
        layout::addRow(page, 40, 20).items = {
            {nullptr, 100, 20}, {nullptr, 0, 26, true, 240}, {nullptr, 110, 32}};
        layout::addRow(page).items = {{nullptr, 0, 40, true}};
        std::vector<RECT> bounds;
        layout::arrange(page, 222, width, 0,
            [&](const layout::Item& item, const RECT& r, int nativeHeight) {
                check(r.left >= 222 && r.right <= 222 + width, "control stays inside page");
                check(r.right > r.left, "positive control width");
                check(nativeHeight == (item.dropHeight ? 240 : item.height), "combo drop height preserved");
                bounds.push_back(r);
            });
        check(bounds[1].left - bounds[0].right == 12, "label/input gap");
        check(bounds[1].right == 222 + width - 20, "URL right inset");
        check(bounds[4].left - bounds[3].right == 12, "model/button gap");
        check(bounds[4].right == bounds[1].right, "field right edges align");
        check(bounds[3].top + 26 <= bounds[5].top, "combo popup height does not push later rows");
        const RECT card = layout::cardRect(page, 222, width);
        check(card.top == 80 && card.bottom == 200, "card symmetric vertical padding");
        check(bounds[5].top - card.bottom == 20, "card/hint gap");
        check(card.top - 62 == 18, "heading/card gap");
        std::vector<RECT> again;
        layout::arrange(page, 222, width, 0,
            [&](const layout::Item&, const RECT& r, int) { again.push_back(r); });
        for (size_t i = 0; i < bounds.size(); ++i)
            check(EqualRect(&bounds[i], &again[i]), "repeated layout has no resize drift");
    }
    layout::Page listPage;
    layout::addRow(listPage).items = {
        {nullptr, 0, 240, true, 0, true}, {nullptr, 80, 32}};
    layout::addRow(listPage).items = {{nullptr, 0, 40, true}};
    std::vector<RECT> list;
    layout::arrange(listPage, 222, 560, 140,
        [&](const layout::Item&, const RECT& r, int) { list.push_back(r); });
    check(list[0].bottom - list[0].top == 380, "excluded-app list grows with window height");
    check(list[1].top == list[0].top, "remove button remains top aligned");
    check(list[2].top - list[0].bottom == 16, "hint follows growing list");
    layout::Page split;
    layout::addRow(split).items = {{nullptr, 100, 30, true}, {nullptr, 120, 30, true}};
    int total = 0;
    layout::arrange(split, 0, 561, 0,
        [&](const layout::Item&, const RECT& r, int) { total += r.right - r.left; });
    check(total + 12 == 561, "multiple flexible columns share all remaining space");
    std::cout << (failures ? "Layout tests failed\n" : "All layout tests passed\n");
    return failures ? 1 : 0;
}
