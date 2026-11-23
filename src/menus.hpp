#pragma once

#include "appearance.hpp"

#include <oleacc.h>

#include <deque>
#include <string>

namespace twofa {

class ThemedMenu {
public:
    ThemedMenu(Palette palette, UINT dpi, double textScale)
        : palette_(palette), dpi_(dpi ? dpi : 96), textScale_(textScale), menu_(CreatePopupMenu()),
          background_(CreateSolidBrush(palette.card)), font_(CreateUiFont(Scale(Text(14)))),
          iconFont_(CreateUiFont(Scale(Text(15)), FW_NORMAL, L"Segoe Fluent Icons")) {
        const MENUINFO info{sizeof(info), MIM_BACKGROUND | MIM_STYLE, MNS_NOCHECK, 0, background_, 0, 0};
        if (menu_) {
            SetMenuInfo(menu_, &info);
        }
    }
    ThemedMenu(const ThemedMenu&) = delete;
    ThemedMenu& operator=(const ThemedMenu&) = delete;
    ~ThemedMenu() {
        if (menu_ && !attached_) {
            DestroyMenu(menu_);
        }
        DeleteObject(background_);
        DeleteObject(font_);
        DeleteObject(iconFont_);
    }

    void Add(UINT command, std::wstring_view text, UINT state = MFS_ENABLED, bool destructive = false,
             PCWSTR icon = nullptr, ThemedMenu* submenu = nullptr) {
        if (!menu_ || (submenu && (!submenu->menu_ || submenu->attached_))) {
            return;
        }
        auto& entry = entries_.emplace_back();
        entry.text = text;
        entry.destructive = destructive;
        entry.icon = icon;
        entry.submenu = submenu;
        entry.accessible = {MSAA_MENU_SIG, static_cast<DWORD>(entry.text.size()), entry.text.data()};
        MENUITEMINFOW item{};
        item.cbSize = sizeof(item);
        item.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_DATA;
        item.fType = MFT_OWNERDRAW;
        item.fState = command ? state : MFS_DISABLED;
        item.wID = command;
        item.dwItemData = reinterpret_cast<ULONG_PTR>(&entry);
        if (submenu) {
            item.fMask |= MIIM_SUBMENU;
            item.hSubMenu = submenu->menu_;
        }
        if (!InsertMenuItemW(menu_, static_cast<UINT>(entries_.size() - 1), TRUE, &item)) {
            entries_.pop_back();
        } else if (submenu) {
            // DestroyMenu on the parent now owns the submenu handle.
            submenu->attached_ = true;
        }
    }

    void Add(UINT command, PCWSTR text, UINT state = MFS_ENABLED, bool destructive = false,
             PCWSTR icon = nullptr, ThemedMenu* submenu = nullptr) {
        Add(command, UiText(text), state, destructive, icon, submenu);
    }

    void Separator() { Add(0, std::wstring_view{}); }

    UINT Track(HWND owner, POINT point, const RECT* origin = nullptr) {
        if (!menu_) {
            return 0;
        }
        MONITORINFO monitor{sizeof(monitor), {}, {}, 0};
        const bool hasWorkArea = GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), &monitor);
        if (hasWorkArea) {
            maxWidth_ = (std::max<LONG>)(Scale(128), monitor.rcWork.right - monitor.rcWork.left - Scale(40));
        }
        const bool rightAligned = GetSystemMetrics(SM_MENUDROPALIGNMENT) != 0;
        UINT placement = rightAligned ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
        bool above = false;
        TPMPARAMS params{sizeof(params), {}};
        if (origin) {
            params.rcExclude = *origin;
            const auto gap = Scale(4);
            int height = 2 * gap;
            for (const auto& entry : entries_) {
                height += ItemHeight(entry);
            }
            if (hasWorkArea) {
                const auto below = monitor.rcWork.bottom - origin->bottom;
                above = below < height && origin->top - monitor.rcWork.top > below;
            }
            point.x = rightAligned ? origin->right : origin->left;
            point.y = above ? origin->top - gap : origin->bottom + gap;
            placement |= TPM_VERTICAL | (above ? TPM_BOTTOMALIGN : TPM_TOPALIGN);
        }
        const UINT motion = palette_.highContrast || !UseClientAnimations() ? TPM_NOANIMATION :
                            origin ? (above ? TPM_VERNEGANIMATION : TPM_VERPOSANIMATION) : 0;
        return TrackPopupMenuEx(menu_, TPM_RETURNCMD | TPM_RIGHTBUTTON | placement | motion,
                                point.x, point.y, owner, origin ? &params : nullptr);
    }

    bool Measure(HWND owner, MEASUREITEMSTRUCT& item) const {
        const auto entry = Find(item.itemData);
        if (!entry) {
            return false;
        }
        SIZE text{};
        if (const auto dc = GetDC(owner)) {
            const auto previous = SelectObject(dc, font_);
            GetTextExtentPoint32W(dc, entry->text.c_str(), static_cast<int>(entry->text.size()), &text);
            SelectObject(dc, previous);
            ReleaseDC(owner, dc);
        }
        item.itemWidth = std::clamp(text.cx + Scale(52), static_cast<LONG>(Scale(128)), maxWidth_);
        item.itemHeight = ItemHeight(*entry);
        return true;
    }

    bool Draw(const DRAWITEMSTRUCT& item) const {
        const auto entry = Find(item.itemData);
        if (!entry) {
            return false;
        }
        auto bounds = item.rcItem;
        const auto dc = item.hDC;
        FillSurface(dc, bounds, palette_.card);
        const int center = (bounds.top + bounds.bottom) / 2;
        if (entry->text.empty()) {
            DrawLine(dc, {bounds.left + Scale(11), center}, {bounds.right - Scale(11), center}, palette_.stroke);
            return true;
        }
        const bool selected = (item.itemState & ODS_SELECTED) != 0;
        const bool disabled = (item.itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
        auto ink = selected ? palette_.selectionText : palette_.text;
        if (selected) {
            auto highlight = bounds;
            InflateRect(&highlight, -Scale(4), -Scale(2));
            PaintRounded(dc, highlight, Scale(5), palette_.selected,
                         palette_.highContrast ? palette_.selectionText : palette_.selected);
        }
        if (disabled) {
            ink = palette_.disabledText;
        } else if (entry->destructive && !palette_.highContrast) {
            ink = palette_.dark ? RGB(255, 154, 166) : RGB(164, 36, 51);
        }
        const auto icon = item.itemState & ODS_CHECKED ? L"\ue73e" : entry->icon;
        if (icon) {
            const RECT glyph{bounds.left + Scale(8), bounds.top, bounds.left + Scale(30), bounds.bottom};
            PaintText(dc, icon, glyph, iconFont_, ink, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        }
        bounds.left += Scale(34);
        bounds.right -= Scale(12);
        PaintText(dc, entry->text, bounds, font_, ink, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        return true;
    }

private:
    struct Entry {
        MSAAMENUINFO accessible{};
        std::wstring text;
        bool destructive = false;
        PCWSTR icon = nullptr;
        ThemedMenu* submenu = nullptr;
    };

    const Entry* Find(ULONG_PTR data) const {
        const auto entry = reinterpret_cast<const Entry*>(data);
        for (const auto& candidate : entries_) {
            if (&candidate == entry) {
                return &candidate;
            }
            if (candidate.submenu) {
                if (const auto nested = candidate.submenu->Find(data)) {
                    return nested;
                }
            }
        }
        return nullptr;
    }

    int ItemHeight(const Entry& entry) const { return Scale(entry.text.empty() ? 8 : Text(18) + 14); }

    int Scale(int units) const { return MulDiv(units, dpi_, 96); }
    int Text(int units) const { return static_cast<int>(std::lround(units * textScale_)); }

    Palette palette_;
    UINT dpi_;
    double textScale_;
    HMENU menu_;
    HBRUSH background_;
    HFONT font_;
    HFONT iconFont_;
    LONG maxWidth_ = LONG_MAX;
    bool attached_ = false;
    std::deque<Entry> entries_;
};

}  // namespace twofa
