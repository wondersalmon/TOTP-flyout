#pragma once

#include "dialogs.hpp"
#include "providers.hpp"
#include "appearance.hpp"
#include "clipboard.hpp"
#include "menus.hpp"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>
#include <wtsapi32.h>

#include <chrono>
#include <atomic>
#include <cmath>
#include <cwchar>

namespace twofa {

inline PCWSTR TrayWindowClass(bool standalone) {
    return standalone ? L"TwoFaFlyoutStandalone" : L"TwoFaFlyout";
}

inline constexpr UINT kShowStandaloneFlyout = WM_APP + 2;

// Coordinates are relative to the monitor work area in 96-DPI units.
struct ManualWindowPosition {
    std::array<wchar_t, CCHDEVICENAME> monitor{};
    POINT offset{};
};

inline POINT ClampFlyoutPosition(POINT position, SIZE size, const RECT& work) {
    return {
        std::clamp(position.x, work.left, (std::max)(work.left, work.right - size.cx)),
        std::clamp(position.y, work.top, (std::max)(work.top, work.bottom - size.cy)),
    };
}

inline POINT RestoreManualFlyoutPosition(POINT offset, SIZE size, const RECT& work, UINT dpi) {
    const auto x = std::clamp(MulDiv(offset.x, dpi, 96), 0, static_cast<int>(work.right - work.left));
    const auto y = std::clamp(MulDiv(offset.y, dpi, 96), 0, static_cast<int>(work.bottom - work.top));
    return ClampFlyoutPosition({work.left + x, work.top + y}, size, work);
}

inline POINT CornerFlyoutPosition(WindowPlacementMode mode, SIZE size, const RECT& work, int inset) {
    const bool right = mode == WindowPlacementMode::TopRight || mode == WindowPlacementMode::BottomRight;
    const bool bottom = mode == WindowPlacementMode::BottomLeft || mode == WindowPlacementMode::BottomRight;
    return ClampFlyoutPosition({right ? work.right - size.cx - inset : work.left + inset,
                                bottom ? work.bottom - size.cy - inset : work.top + inset}, size, work);
}

class TrayFlyout {
public:
    TrayFlyout(bool standalone, const std::atomic<FlyoutPreferences>* modPreferences, std::wstring vaultPath,
               void (*checkUpdates)(HWND))
        : standalone_(standalone),
          checkUpdates_(checkUpdates),
          modPreferences_(modPreferences),
          preferences_(modPreferences ? modPreferences->load() : LoadPreferences()),
          vaultPath_(std::move(vaultPath)) {}

    int Run(HANDLE stopEvent, bool showOnLaunch) {
        stopEvent_ = stopEvent;
        russianUi = preferences_.russian;
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const PaintResources painting;
        taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
        const auto instance = GetModuleHandleW(nullptr);
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = TrayWindowClass(standalone_);
        if (!RegisterClassW(&windowClass)) {
            return 1;
        }

        const auto window = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_CONTROLPARENT, windowClass.lpszClassName, L"TOTP flyout",
            WS_POPUP | WS_BORDER | WS_CLIPCHILDREN, 0, 0, 1, 1, nullptr, nullptr, instance, this);
        int result = 1;
        if (window) {
            AddIcon();
            if (showOnLaunch) {
                previewVisible_ = true;
                ShowWindow(window_, SW_HIDE);
                ShowFlyout(false);
            }
            result = PumpMessages(stopEvent);
            if (IsWindow(window)) {
                DestroyWindow(window);
            }
        }
        UnregisterClassW(windowClass.lpszClassName, instance);
        return result;
    }

private:
    static constexpr UINT kTrayMessage = WM_APP + 1;
    static constexpr UINT_PTR kTimer = 1;
    static constexpr UINT_PTR kLockTimer = 2;
    static constexpr UINT_PTR kMotionTimer = 3;
    static constexpr UINT_PTR kClipboardTimer = 4;
    static constexpr UINT_PTR kCopyPulseTimer = 5;
    static constexpr UINT_PTR kCopyNoteTimer = 6;
    static constexpr UINT_PTR kDragScrollTimer = 7;
    static constexpr UINT_PTR kFeedbackTimer = 8;
    static constexpr UINT_PTR kHideAfterCopyTimer = 9;
    static constexpr UINT kHideAfterCopyMs = 500;
    static constexpr ULONGLONG kEnterDurationMs = 167;
    static constexpr ULONGLONG kExitDurationMs = 167;
    static constexpr ULONGLONG kCopyPulseMs = 250;
    static constexpr UINT kCopyConfirmationMs = 1800;
    static constexpr UINT kIconId = 1;
    static constexpr UINT kOpen = 1;
    static constexpr UINT kExit = 2;
    static constexpr UINT kTraySettings = 3;
    static constexpr UINT kTrayCheckUpdates = 4;
    static constexpr UINT kTagFilter = 101;
    static constexpr UINT kAccounts = 102;
    static constexpr UINT kClose = 104;
    static constexpr UINT kSettings = 105;
    static constexpr UINT kSort = 107;
    static constexpr UINT kAppearanceSettings = 1;
    static constexpr UINT kShortcutSettings = 2;
    static constexpr UINT kBackupSettings = 3;
    static constexpr UINT kPositionSettings = 4;
    static constexpr UINT kManageSettings = 5;
    static constexpr UINT kImportButton = 108;
    static constexpr UINT kEditButton = 109;
    static constexpr UINT kAccountScroll = 110;
    static constexpr UINT kEditSelectedTag = 0x8000;
    static constexpr UINT kEditAccountTags = 0x8001;
    static constexpr int kGutter = 16;
    enum class Motion { Idle, Entering, Leaving };
    struct RowDrag {
        WORD source;
        WORD target;
        POINT start;
        POINT pointer;
        LONG rowOffset;
        bool moving = false;
    };
    enum Action : UINT {
        ImportLink = 201, SetTags, Lock, RenameAccount, RemoveAccount, MoveUp, MoveDown, AddAccount, ImportText, ImportHtml, ImportVault, ImportMenu, ImportClipboard, UnlockLocalVault, UseWindowsVault
    };
    static constexpr std::array<PCWSTR, 6> kSortNames{
        L"Manual order", L"Name (A to Z)", L"Name (Z to A)", L"Provider", L"Most used", L"Recently used"
    };
    static constexpr std::array<PCWSTR, 6> kSortButtonNames{
        L"Sort: Manual", L"Sort: A to Z", L"Sort: Z to A", L"Sort: Provider", L"Sort: Most used", L"Sort: Recent"
    };

    int PumpMessages(HANDLE stopEvent) {
        const DWORD count = stopEvent ? 1 : 0;
        for (;;) {
            const auto ready = MsgWaitForMultipleObjectsEx(
                count, stopEvent ? &stopEvent : nullptr, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (stopEvent && ready == WAIT_OBJECT_0) {
                return 0;
            }
            if (ready != WAIT_OBJECT_0 + count) {
                return 1;
            }

            MSG message;
            if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    return static_cast<int>(message.wParam);
                }
                if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE &&
                    (message.hwnd == window_ || IsChild(window_, message.hwnd))) {
                    if (drag_) {
                        EndRowDrag();
                    } else {
                        DismissFlyout();
                    }
                    continue;
                }
                if (message.message == WM_KEYDOWN &&
                    (message.wParam == VK_RETURN || message.wParam == VK_SPACE) &&
                    message.hwnd == accountList_ && selected_) {
                    if (motion_ != Motion::Leaving && !(message.lParam & (1L << 30))) {
                        CopyAccountCode(*selected_);
                    }
                    continue;
                }
                if (message.message == WM_SYSKEYDOWN && message.hwnd == accountList_ &&
                    (message.lParam & (1L << 29)) && (message.wParam == VK_UP || message.wParam == VK_DOWN) &&
                    editing_ && sort_ == AccountSort::Manual) {
                    if (motion_ != Motion::Leaving) {
                        MoveSelected(message.wParam == VK_UP ? -1 : 1);
                    }
                    continue;
                }
                if (IsWindowVisible(window_) && IsDialogMessageW(window_, &message)) {
                    continue;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
    }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const auto creation = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            const auto flyout = static_cast<TrayFlyout*>(creation->lpCreateParams);
            flyout->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(flyout));
        }
        const auto flyout = reinterpret_cast<TrayFlyout*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        return flyout ? flyout->HandleMessage(message, wparam, lparam)
                      : DefWindowProcW(window, message, wparam, lparam);
    }

    static LRESULT CALLBACK AccountListProc(HWND list, UINT message, WPARAM wparam, LPARAM lparam,
                                            UINT_PTR id, DWORD_PTR context) {
        const auto flyout = reinterpret_cast<TrayFlyout*>(context);
        if (message == WM_NCDESTROY) {
            RemoveWindowSubclass(list, AccountListProc, id);
        } else if (message == WM_CANCELMODE) {
            flyout->EndRowDrag();
        } else if (message == WM_MOUSEMOVE) {
            const POINT pointer{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (flyout->drag_) {
                flyout->UpdateRowDrag(pointer);
                return 0;
            }
            const auto previous = flyout->hoverRow_;
            flyout->hoverRow_ = flyout->HitRow(pointer);
            if (previous != flyout->hoverRow_) {
                flyout->InvalidateRow(previous);
                flyout->InvalidateRow(flyout->hoverRow_);
            }
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, list, 0};
            TrackMouseEvent(&tracking);
        } else if (message == WM_MOUSEWHEEL) {
            flyout->ScrollWheel(GET_WHEEL_DELTA_WPARAM(wparam));
            return 0;
        } else if (message == WM_MOUSELEAVE || message == WM_VSCROLL) {
            flyout->InvalidateRow(std::exchange(flyout->hoverRow_, std::nullopt));
            if (message != WM_MOUSELEAVE && flyout->drag_ && flyout->drag_->moving) {
                const auto result = DefSubclassProc(list, message, wparam, lparam);
                if (flyout->drag_) {
                    flyout->UpdateRowDrag(flyout->drag_->pointer);
                }
                return result;
            }
        } else if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) {
            const POINT pointer{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            const auto row = flyout->HitRow(pointer);
            if (row) {
                SetFocus(list);
                flyout->SelectRow(*row);
                if (flyout->IsDragGrip(pointer)) {
                    RECT bounds{};
                    SendMessageW(list, LB_GETITEMRECT, *row, reinterpret_cast<LPARAM>(&bounds));
                    flyout->drag_ = RowDrag{*row, *row, pointer, pointer, pointer.y - bounds.top};
                    SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
                } else {
                    flyout->copyPressed_ = flyout->selected_;
                }
                SetCapture(list);
                flyout->InvalidateRow(row);
                return 0;
            }
        } else if (message == WM_LBUTTONUP) {
            if (flyout->drag_) {
                const auto drag = *flyout->drag_;
                const POINT pointer{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                RECT client{};
                GetClientRect(list, &client);
                flyout->EndRowDrag();
                if (drag.moving && PtInRect(&client, pointer)) {
                    flyout->ReorderRows(drag.source, flyout->DragTarget(pointer));
                }
                return 0;
            }
            const auto pressed = std::exchange(flyout->copyPressed_, std::nullopt);
            if (pressed) {
                if (GetCapture() == list) {
                    ReleaseCapture();
                }
                const auto row = flyout->HitRow({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
                InvalidateRect(list, nullptr, FALSE);
                if (row && SendMessageW(list, LB_GETITEMDATA, *row, 0) == static_cast<LRESULT>(*pressed)) {
                    flyout->CopyAccountCode(*pressed);
                }
                return 0;
            }
        } else if (message == WM_CAPTURECHANGED) {
            flyout->copyPressed_.reset();
            flyout->EndRowDrag();
            InvalidateRect(list, nullptr, FALSE);
        } else if (message == WM_CONTEXTMENU) {
            flyout->ShowAccountMenu({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
            return 0;
        } else if (message == WM_SETCURSOR) {
            POINT pointer;
            if (GetCursorPos(&pointer) && ScreenToClient(list, &pointer) && flyout->HitRow(pointer)) {
                SetCursor(LoadCursorW(nullptr, flyout->IsDragGrip(pointer) ? IDC_SIZENS : IDC_HAND));
                return TRUE;
            }
        }
        const auto result = DefSubclassProc(list, message, wparam, lparam);
        if (message == WM_KEYDOWN || message == LB_SETTOPINDEX || message == LB_SETCURSEL || message == WM_VSCROLL) {
            InvalidateRect(flyout->accountScroll_, nullptr, FALSE);
        }
        return result;
    }

    static LRESULT CALLBACK ScrollbarProc(HWND scroll, UINT message, WPARAM wparam, LPARAM lparam,
                                           UINT_PTR id, DWORD_PTR data) {
        const auto flyout = reinterpret_cast<TrayFlyout*>(data);
        if (message == WM_NCDESTROY) {
            RemoveWindowSubclass(scroll, ScrollbarProc, id);
        } else if (message == WM_LBUTTONDOWN) {
            SetCapture(scroll);
            flyout->ScrollToPointer(GET_Y_LPARAM(lparam));
            return 0;
        } else if (message == WM_MOUSEMOVE && GetCapture() == scroll) {
            flyout->ScrollToPointer(GET_Y_LPARAM(lparam));
            return 0;
        } else if (message == WM_LBUTTONUP && GetCapture() == scroll) {
            ReleaseCapture();
            return 0;
        } else if (message == WM_MOUSEWHEEL) {
            flyout->ScrollWheel(GET_WHEEL_DELTA_WPARAM(wparam));
            return 0;
        }
        return DefSubclassProc(scroll, message, wparam, lparam);
    }

    int VisibleRows() const {
        RECT rows{};
        GetClientRect(accountList_, &rows);
        return (std::max)(1, static_cast<int>(rows.bottom) / RowHeight());
    }

    int LastScrollRow() const { return (std::max)(0, accountCount_ - VisibleRows()); }

    void ScrollAccounts(int top) {
        InvalidateRow(std::exchange(hoverRow_, std::nullopt));
        SendMessageW(accountList_, LB_SETTOPINDEX, std::clamp(top, 0, LastScrollRow()), 0);
        if (drag_ && drag_->moving) {
            UpdateRowDrag(drag_->pointer);
        }
    }

    void ScrollWheel(int delta) {
        wheelDelta_ += delta;
        const int steps = wheelDelta_ / WHEEL_DELTA;
        wheelDelta_ %= WHEEL_DELTA;
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        const int amount = lines == WHEEL_PAGESCROLL ? VisibleRows() : static_cast<int>((std::min)(lines, 256u));
        if (steps && amount) {
            const auto top = SendMessageW(accountList_, LB_GETTOPINDEX, 0, 0);
            ScrollAccounts(static_cast<int>(top) - steps * amount);
        }
    }

    int ScrollThumbHeight(int height) const {
        return std::clamp(MulDiv(height, VisibleRows(), (std::max)(1, accountCount_)),
                          (std::min)(Scale(18), height), height);
    }

    void ScrollToPointer(int y) {
        RECT track{};
        GetClientRect(accountScroll_, &track);
        const int thumbHeight = ScrollThumbHeight(track.bottom);
        const int travel = track.bottom - thumbHeight;
        if (travel > 0) {
            ScrollAccounts(MulDiv(std::clamp(y - thumbHeight / 2, 0, travel), LastScrollRow(), travel));
        }
    }

    void DrawAccountScrollbar(HDC dc, RECT track) const {
        FillSurface(dc, track, palette_.surface);
        if (const int last = LastScrollRow(); last > 0) {
            const int height = ScrollThumbHeight(track.bottom);
            const auto row = static_cast<int>(SendMessageW(accountList_, LB_GETTOPINDEX, 0, 0));
            const int top = MulDiv(track.bottom - height, std::clamp(row, 0, last), last);
            const auto color = palette_.highContrast ? palette_.text : BlendColor(palette_.stroke, palette_.muted, 45);
            PaintRounded(dc, {Scale(2), top, track.right - 1, top + height}, Scale(2), color, color);
        }
    }

    void InvalidateRow(std::optional<UINT> row) const {
        RECT bounds{};
        if (row && SendMessageW(accountList_, LB_GETITEMRECT, *row,
                               reinterpret_cast<LPARAM>(&bounds)) != LB_ERR) {
            InvalidateRect(accountList_, &bounds, FALSE);
        }
    }

    bool CanDragRows() const { return editing_ && sort_ == AccountSort::Manual && accountCount_ > 1; }

    bool IsDragGrip(POINT pointer) const { return CanDragRows() && pointer.x < Scale(33); }

    WORD DragTarget(POINT pointer) const {
        RECT client{};
        GetClientRect(accountList_, &client);
        pointer.x = std::clamp(pointer.x, 0L, (std::max)(0L, client.right - 1));
        pointer.y = std::clamp(pointer.y, 0L, (std::max)(0L, client.bottom - 1));
        return LOWORD(SendMessageW(accountList_, LB_ITEMFROMPOINT, 0, MAKELPARAM(pointer.x, pointer.y)));
    }

    void EndRowDrag() {
        if (const auto drag = std::exchange(drag_, std::nullopt)) {
            KillTimer(window_, kDragScrollTimer);
            const auto ghost = DragPreviewBounds(*drag);
            InvalidateRect(accountList_, &ghost, FALSE);
            InvalidateRow(drag->source);
            InvalidateRow(drag->target);
            if (GetCapture() == accountList_) {
                ReleaseCapture();
            }
        }
    }

    void UpdateRowDrag(POINT pointer) {
        const auto previousGhost = DragPreviewBounds(*drag_);
        drag_->pointer = pointer;
        if (!drag_->moving) {
            const auto threshold = GetSystemMetricsForDpi(SM_CYDRAG, dpi_);
            if (std::abs(pointer.y - drag_->start.y) < (std::max)(threshold, 1)) {
                return;
            }
            drag_->moving = true;
            InvalidateRow(drag_->source);
            SetTimer(window_, kDragScrollTimer, 100, nullptr);
        }
        const auto previous = std::exchange(drag_->target, DragTarget(pointer));
        InvalidateRow(previous);
        InvalidateRow(drag_->target);
        const auto ghost = DragPreviewBounds(*drag_);
        InvalidateRect(accountList_, &previousGhost, FALSE);
        InvalidateRect(accountList_, &ghost, FALSE);
    }

    RECT DragPreviewBounds(const RowDrag& drag) const {
        RECT client{};
        GetClientRect(accountList_, &client);
        const auto height = static_cast<LONG>(RowHeight());
        const auto top = std::clamp(drag.pointer.y - drag.rowOffset, 0L,
                                    (std::max)(0L, client.bottom - height));
        return {client.left + Scale(10), top, client.right - Scale(8), top + height};
    }

    void DrawDragPreview(HDC dc, const RECT& paintedRow) const {
        if (!drag_ || !drag_->moving) {
            return;
        }
        const auto ghost = DragPreviewBounds(*drag_);
        const auto width = ghost.right - ghost.left;
        const auto height = ghost.bottom - ghost.top;
        const RECT paintedGhost{ghost.left, ghost.top, ghost.right, ghost.bottom - Scale(4)};
        RECT overlap{};
        if (!IntersectRect(&overlap, &paintedRow, &paintedGhost)) {
            return;
        }
        const auto index = SendMessageW(accountList_, LB_GETITEMDATA, drag_->source, 0);
        if (index < 0 || static_cast<std::size_t>(index) >= accounts_.size()) {
            return;
        }
        const auto memory = CreateCompatibleDC(dc);
        const auto bitmap = CreateCompatibleBitmap(dc, width, height);
        if (memory && bitmap) {
            const auto original = SelectObject(memory, bitmap);
            if (original) {
                DRAWITEMSTRUCT item{};
                item.hDC = memory;
                item.itemData = static_cast<ULONG_PTR>(index);
                item.rcItem = {0, 0, width, height};
                PaintAccountRow(item, memory, true);
                const auto copyWidth = overlap.right - overlap.left;
                const auto copyHeight = overlap.bottom - overlap.top;
                const auto sourceX = overlap.left - ghost.left;
                const auto sourceY = overlap.top - ghost.top;
                const bool blended = !palette_.highContrast &&
                    GdiAlphaBlend(dc, overlap.left, overlap.top, copyWidth, copyHeight,
                                  memory, sourceX, sourceY, copyWidth, copyHeight, {AC_SRC_OVER, 0, 220, 0});
                if (!blended) {
                    BitBlt(dc, overlap.left, overlap.top, copyWidth, copyHeight, memory, sourceX, sourceY, SRCCOPY);
                }
                SelectObject(memory, original);
            }
        }
        DeleteObject(bitmap);
        DeleteDC(memory);
    }

    void ScrollRowDrag() {
        if (!drag_ || !drag_->moving) {
            return;
        }
        RECT client{};
        GetClientRect(accountList_, &client);
        if (drag_->pointer.x < 0 || drag_->pointer.x >= client.right) {
            return;
        }
        const int edge = Scale(12);
        const int direction = drag_->pointer.y < edge ? -1 : drag_->pointer.y >= client.bottom - edge ? 1 : 0;
        if (direction) {
            const auto top = SendMessageW(accountList_, LB_GETTOPINDEX, 0, 0);
            const auto visible = (std::max)(1L, client.bottom / RowHeight());
            const auto next = std::clamp<LRESULT>(top + direction, 0, (std::max)(0L, accountCount_ - visible));
            if (next != top) {
                SendMessageW(accountList_, LB_SETTOPINDEX, next, 0);
                UpdateRowDrag(drag_->pointer);
            }
        }
    }

    static LRESULT CALLBACK ButtonProc(HWND control, UINT message, WPARAM wparam, LPARAM lparam,
                                      UINT_PTR id, DWORD_PTR context) {
        const auto flyout = reinterpret_cast<TrayFlyout*>(context);
        if (message == WM_NCDESTROY) {
            RemoveWindowSubclass(control, ButtonProc, id);
            if (flyout->hotButton_ == control) {
                flyout->hotButton_ = nullptr;
            }
        } else if (message == WM_MOUSEMOVE) {
            RECT bounds{};
            GetClientRect(control, &bounds);
            const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (IsWindowEnabled(control) && PtInRect(&bounds, point)) {
                if (flyout->hotButton_ != control) {
                    flyout->SetHotButton(control);
                }
                TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, control, 0};
                TrackMouseEvent(&tracking);
            } else if (flyout->hotButton_ == control) {
                flyout->SetHotButton(nullptr);
            }
        } else if (message == WM_MOUSELEAVE || message == WM_CANCELMODE ||
                   (message == WM_ENABLE && !wparam)) {
            if (flyout->hotButton_ == control) {
                flyout->SetHotButton(nullptr);
            }
        } else if (message == WM_UPDATEUISTATE) {
            InvalidateRect(control, nullptr, FALSE);
        }
        return DefSubclassProc(control, message, wparam, lparam);
    }

    void SetHotButton(HWND button) {
        const auto previous = std::exchange(hotButton_, button);
        if (previous == button) {
            return;
        }
        const auto now = GetTickCount64();
        const bool animate = IsWindowVisible(window_) && MotionEnabled();
        for (auto& feedback : buttonFeedback_) {
            if (feedback.window == previous || feedback.window == button) {
                feedback.fade.Set(feedback.window == button ? 100 : 0, now, animate);
                feedback.animating = feedback.fade.Running(now);
                InvalidateRect(feedback.window, nullptr, FALSE);
            }
        }
        if (animate) {
            SetTimer(window_, kFeedbackTimer, USER_TIMER_MINIMUM, nullptr);
        }
    }

    void HighlightChangedRow() {
        const auto row = SendMessageW(accountList_, LB_GETCURSEL, 0, 0);
        if (row < 0 || !IsWindowVisible(window_) || !MotionEnabled()) {
            return;
        }
        changedRow_ = static_cast<UINT>(row);
        const auto now = GetTickCount64();
        changedRowFade_.Set(100, now, false);
        changedRowFade_.Set(0, now, true, 167);
        if (SetTimer(window_, kFeedbackTimer, USER_TIMER_MINIMUM, nullptr)) {
            InvalidateRow(changedRow_);
        } else {
            changedRow_.reset();
        }
    }

    void UpdateInteractionFeedback() {
        const auto now = GetTickCount64();
        const bool animate = MotionEnabled() && IsWindowVisible(window_);
        bool active = false;
        for (auto& feedback : buttonFeedback_) {
            if (feedback.animating) {
                if (!animate) {
                    feedback.fade.Set(feedback.window == hotButton_ ? 100 : 0, now, false);
                }
                feedback.animating = feedback.fade.Running(now);
                active |= feedback.animating;
                InvalidateRect(feedback.window, nullptr, FALSE);
            }
        }
        if (changedRow_) {
            InvalidateRow(changedRow_);
            if (animate && changedRowFade_.Running(now)) {
                active = true;
            } else {
                changedRow_.reset();
            }
        }
        if (!active) {
            KillTimer(window_, kFeedbackTimer);
        }
    }

    static LRESULT CALLBACK ProgressProc(HWND control, UINT message, WPARAM wparam, LPARAM lparam,
                                        UINT_PTR id, DWORD_PTR context) {
        if (message == WM_NCDESTROY) {
            RemoveWindowSubclass(control, ProgressProc, id);
        } else if (message == WM_ERASEBKGND) {
            return 1;
        } else if (message == WM_PAINT || message == WM_PRINTCLIENT ||
                   (message == WM_PRINT && (lparam & PRF_CLIENT))) {
            const auto flyout = reinterpret_cast<const TrayFlyout*>(context);
            PAINTSTRUCT paint{};
            const auto dc = message == WM_PAINT ? BeginPaint(control, &paint) : reinterpret_cast<HDC>(wparam);
            flyout->DrawProgress(dc);
            if (message == WM_PAINT) {
                EndPaint(control, &paint);
            }
            return 0;
        }
        return DefSubclassProc(control, message, wparam, lparam);
    }

    std::optional<WORD> HitRow(POINT point) const {
        RECT client{};
        GetClientRect(accountList_, &client);
        if (!PtInRect(&client, point)) {
            return std::nullopt;
        }
        const auto hit = SendMessageW(accountList_, LB_ITEMFROMPOINT, 0, MAKELPARAM(point.x, point.y));
        if (HIWORD(hit)) {
            return std::nullopt;
        }
        RECT row{};
        if (SendMessageW(accountList_, LB_GETITEMRECT, LOWORD(hit), reinterpret_cast<LPARAM>(&row)) == LB_ERR) {
            return std::nullopt;
        }
        row.bottom -= Scale(4);
        return PtInRect(&row, point) ? std::optional<WORD>(LOWORD(hit)) : std::nullopt;
    }

    void SelectRow(WPARAM row) {
        SendMessageW(accountList_, LB_SETCURSEL, row, 0);
        lastActivity_ = GetTickCount64();
        UpdateSelection();
    }

    void ShowAccountMenu(POINT pointer) {
        if (handlingCommand_) {
            return;
        }
        if (pointer.x == -1 && pointer.y == -1) {
            RECT row{};
            const auto selected = SendMessageW(accountList_, LB_GETCURSEL, 0, 0);
            if (selected == LB_ERR || SendMessageW(accountList_, LB_GETITEMRECT, selected,
                                                   reinterpret_cast<LPARAM>(&row)) == LB_ERR) {
                return;
            }
            pointer = {row.left + Scale(12), row.bottom};
            ClientToScreen(accountList_, &pointer);
        } else {
            auto client = pointer;
            ScreenToClient(accountList_, &client);
            const auto row = HitRow(client);
            if (!row) {
                return;
            }
            SelectRow(*row);
        }
        if (!SelectedAccount()) {
            return;
        }
        ThemedMenu menu(palette_, dpi_, textScale_);
        menu.Add(RenameAccount, L"Rename account...", MFS_ENABLED, false, L"\ue70f");
        menu.Add(SetTags, L"Edit tags...", MFS_ENABLED, false, L"\ue8ec");
            if (editing_ && sort_ == AccountSort::Manual) {
            const auto row = SendMessageW(accountList_, LB_GETCURSEL, 0, 0);
            menu.Separator();
            menu.Add(MoveUp, L"Move up", row > 0 ? MFS_ENABLED : MFS_DISABLED, false, L"\ue74a");
            menu.Add(MoveDown, L"Move down", row >= 0 && row + 1 < accountCount_ ? MFS_ENABLED : MFS_DISABLED, false, L"\ue74b");
        }
        menu.Separator();
        menu.Add(RemoveAccount, L"Remove account...", MFS_ENABLED, true, L"\ue74d");
        handlingCommand_ = true;
        lastActivity_ = GetTickCount64();
        const auto generation = lockGeneration_;
        const auto action = TrackMenu(menu, pointer);
        if (generation == lockGeneration_) {
            switch (action) {
                case RenameAccount: EditAccountName(); break;
                case SetTags: AssignTags(); break;
                case MoveUp: MoveSelected(-1); break;
                case MoveDown: MoveSelected(1); break;
                case RemoveAccount: RemoveSelectedAccount(); break;
            }
        }
        handlingCommand_ = false;
    }

    LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
        if (taskbarCreated_ && message == taskbarCreated_) {
            AddIcon();
            if (IsWindowVisible(window_) && !movingWindow_) {
                anchor_ = IconRect();
                PositionFlyout();
            }
            return 0;
        }
        switch (message) {
            case kShowStandaloneFlyout:
                if (standalone_ && !handlingCommand_ && sessionAvailable_) {
                    ShowFlyout();
                }
                return 0;
            case WM_CREATE:
                return CreateControls() ? 0 : -1;
            case WM_COMMAND:
                if (HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) == kTagFilter) {
                    ShowTagMenu();
                } else if (HIWORD(wparam) == LBN_SELCHANGE && LOWORD(wparam) == kAccounts) {
                    lastActivity_ = GetTickCount64();
                    UpdateSelection();
                } else if (HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) == kImportButton) {
                    ShowImportAccounts();
                } else if (HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) == kSettings) {
                    ShowSettings();
                } else if (HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) == kEditButton) {
                    ToggleEditing();
                } else if (HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) == kSort) {
                    ShowSortMenu();
                } else if (HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) == kClose) {
                    DismissFlyout();
                }
                return 0;
            case WM_MOUSEWHEEL:
                ScrollWheel(GET_WHEEL_DELTA_WPARAM(wparam));
                return 0;
            case WM_DRAWITEM: {
                const auto item = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
                if (item->CtlType == ODT_MENU) {
                    return activeMenu_ && activeMenu_->Draw(*item);
                } else if (item->hwndItem == note_) {
                    RECT text = item->rcItem;
                    FillSurface(item->hDC, text, palette_.card);
                    PaintRounded(item->hDC, text, Scale(6), palette_.card, palette_.stroke);
                    InflateRect(&text, -Scale(12), -Scale(6));
                    PaintText(item->hDC, noteText_, text, smallFont_, palette_.text, DT_WORDBREAK);
                } else if (item->hwndItem == accountList_) {
                    DrawAccountRow(*item);
                } else if (item->hwndItem == accountScroll_) {
                    DrawAccountScrollbar(item->hDC, item->rcItem);
                } else {
                    DrawButton(*item);
                }
                return TRUE;
            }
            case WM_MEASUREITEM: {
                const auto item = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
                if (item->CtlType == ODT_MENU) {
                    return activeMenu_ && activeMenu_->Measure(window_, *item);
                }
                if (item->CtlID == kAccounts) {
                    item->itemHeight = RowHeight();
                    return TRUE;
                }
                break;
            }
            case WM_PAINT: {
                PAINTSTRUCT paint;
                const auto dc = BeginPaint(window_, &paint);
                DrawFrame(dc);
                EndPaint(window_, &paint);
                return 0;
            }
            case WM_PRINTCLIENT:
                DrawFrame(reinterpret_cast<HDC>(wparam));
                return 0;
            case WM_PRINT:
                if (!accounts_.empty() && !preferences_.allowScreenshots) {
                    return 0;
                }
                break;
            case WM_ERASEBKGND:
                return 1;
            case WM_CTLCOLORSTATIC: {
                const auto child = reinterpret_cast<HWND>(lparam);
                const auto dc = reinterpret_cast<HDC>(wparam);
                const bool inEmptyCard = child == emptyTitle_ || child == emptyHint_;
                const bool muted = child == emptyHint_ || child == timerLabel_ || child == note_;
                SetBkMode(dc, OPAQUE);
                SetBkColor(dc, inEmptyCard ? palette_.card : palette_.surface);
                SetTextColor(dc, muted ? palette_.muted : palette_.text);
                return reinterpret_cast<LRESULT>(inEmptyCard ? cardBrush_ : frameBrush_);
            }
            case WM_CTLCOLORLISTBOX:
                return reinterpret_cast<LRESULT>(frameBrush_);
            case WM_NCHITTEST: {
                const auto hit = DefWindowProcW(window_, message, wparam, lparam);
                RECT title{};
                if (hit == HTCLIENT && CanMoveWindow() &&
                    GetWindowRect(title_, &title) &&
                    PtInRect(&title, {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)})) {
                    return HTCAPTION;
                }
                return hit;
            }
            case WM_SYSCOMMAND:
                if ((wparam & 0xfff0) == SC_MOVE) {
                    if (!CanMoveWindow()) {
                        return 0;
                    }
                    CompleteMotion();
                }
                break;
            case WM_ENTERSIZEMOVE:
                movingWindow_ = true;
                GetWindowRect(window_, &moveStart_);
                return 0;
            case WM_EXITSIZEMOVE:
                movingWindow_ = false;
                FinishWindowMove();
                return 0;
            case WM_DISPLAYCHANGE:
                EndRowDrag();
                if (IsWindowVisible(window_) && !movingWindow_) {
                    anchor_ = IconRect();
                    PositionFlyout();
                }
                return 0;
            case WM_SETTINGCHANGE:
                if (wparam == SPI_SETWORKAREA && IsWindowVisible(window_) && !movingWindow_) {
                    anchor_ = IconRect();
                    PositionFlyout();
                }
                [[fallthrough]];
            case WM_SYSCOLORCHANGE:
                UpdateTrayIcon();
                RefreshTextSize();
                RefreshAppearance();
                return 0;
            case WM_DWMCOMPOSITIONCHANGED:
            case WM_DWMCOLORIZATIONCOLORCHANGED:
            case WM_THEMECHANGED:
                RefreshAppearance();
                return 0;
            case WM_DPICHANGED: {
                const auto rect = reinterpret_cast<const RECT*>(lparam);
                dpi_ = HIWORD(wparam);
                EndRowDrag();
                if (!positioning_) {
                    CompleteMotion();
                    SIZE size{rect->right - rect->left, rect->bottom - rect->top};
                    if (movingWindow_) {
                        MONITORINFO monitor{sizeof(monitor), {}, {}, 0};
                        if (GetMonitorInfoW(MonitorFromRect(rect, MONITOR_DEFAULTTONEAREST), &monitor)) {
                            size = FlyoutSize(monitor.rcWork);
                        }
                    }
                    SetWindowPos(window_, nullptr, rect->left, rect->top, size.cx, size.cy,
                                 SWP_NOZORDER | SWP_NOACTIVATE);
                    if (IsWindowVisible(window_) && !movingWindow_) {
                        PositionFlyout();
                    }
                }
                return 0;
            }
            case WM_ACTIVATE:
                if (LOWORD(wparam) == WA_INACTIVE) {
                    SetHotButton(nullptr);
                    if (!handlingCommand_ && !previewVisible_ && !movingWindow_) {
                        DismissFlyout();
                    }
                }
                return 0;
            case WM_KEYDOWN:
                if (wparam == VK_ESCAPE) {
                    DismissFlyout();
                    return 0;
                }
                break;
            case WM_HOTKEY:
                if (wparam == activeHotkey_ && activeHotkey_ && !handlingCommand_ && sessionAvailable_) {
                    if (IsWindowVisible(window_) && motion_ != Motion::Leaving) {
                        DismissFlyout();
                    } else {
                        ShowFlyout();
                    }
                }
                return 0;
            case WM_TIMER:
                if (wparam == kTimer) {
                    UpdateCountdown();
                } else if (wparam == kLockTimer) {
                    CheckSession();
                } else if (wparam == kMotionTimer) {
                    UpdateMotion();
                } else if (wparam == kClipboardTimer) {
                    KillTimer(window_, kClipboardTimer);
                    clipboard_.Clear(window_);
                } else if (wparam == kCopyPulseTimer) {
                    UpdateCopyPulse();
                } else if (wparam == kCopyNoteTimer) {
                    ClearNote();
                    StopCopyPulse();
                } else if (wparam == kDragScrollTimer) {
                    ScrollRowDrag();
                } else if (wparam == kFeedbackTimer) {
                    UpdateInteractionFeedback();
                } else if (wparam == kHideAfterCopyTimer) {
                    KillTimer(window_, kHideAfterCopyTimer);
                    if (preferences_.hideAfterCopy && !handlingCommand_) {
                        DismissFlyout();
                    }
                }
                return 0;
            case WM_WTSSESSION_CHANGE:
                if (wparam == WTS_SESSION_LOCK || wparam == WTS_CONSOLE_DISCONNECT ||
                    wparam == WTS_REMOTE_DISCONNECT) {
                    sessionAvailable_ = false;
                    LockSession();
                } else if (wparam == WTS_SESSION_UNLOCK || wparam == WTS_CONSOLE_CONNECT ||
                           wparam == WTS_REMOTE_CONNECT) {
                    sessionAvailable_ = true;
                    if (sessionNotifications_) {
                        EnableWindow(importButton_, TRUE);
                    }
                }
                return 0;
            case WM_POWERBROADCAST:
                if (wparam == PBT_APMSUSPEND) {
                    LockSession();
                }
                return TRUE;
            case WM_CLOSE:
                DestroyWindow(window_);
                return 0;
            case kTrayMessage:
                switch (LOWORD(lparam)) {
                    case NIN_SELECT:
                    case NIN_KEYSELECT:
                    case WM_LBUTTONUP:
                        ShowFlyout();
                        break;
                    case WM_CONTEXTMENU:
                    case WM_RBUTTONUP:
                        if (standalone_) {
                            ShowTrayMenu();
                        } else {
                            ShowFlyout();
                        }
                        break;
                }
                return 0;
            case WM_DESTROY: {
                StopHotkey();
                for (const auto id : hotkeyIds_) {
                    if (id) {
                        GlobalDeleteAtom(id);
                    }
                }
                KillTimer(window_, kTimer);
                KillTimer(window_, kLockTimer);
                KillTimer(window_, kMotionTimer);
                KillTimer(window_, kClipboardTimer);
                KillTimer(window_, kCopyPulseTimer);
                KillTimer(window_, kCopyNoteTimer);
                KillTimer(window_, kDragScrollTimer);
                KillTimer(window_, kFeedbackTimer);
                KillTimer(window_, kHideAfterCopyTimer);
                clipboard_.Clear(window_, true);
                if (sessionNotifications_) {
                    WTSUnRegisterSessionNotification(window_);
                }
                auto icon = IconData();
                Shell_NotifyIconW(NIM_DELETE, &icon);
                PostQuitMessage(0);
                return 0;
            }
            case WM_NCDESTROY:
                ReleaseFonts();
                DeleteObject(frameBrush_);
                DeleteObject(cardBrush_);
                break;
        }
        return DefWindowProcW(window_, message, wparam, lparam);
    }

    HWND CreateChild(PCWSTR kind, PCWSTR text, DWORD style, UINT id = 0) const {
        return CreateWindowExW(0, kind, Ui(text), WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
                               window_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                               GetModuleHandleW(nullptr), nullptr);
    }

    bool CreateControls() {
        const INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS};
        if (!InitCommonControlsEx(&controls)) {
            return false;
        }
        title_ = CreateChild(L"STATIC", L"TOTP flyout", SS_NOPREFIX | SS_CENTERIMAGE);
        settings_ = CreateChild(L"BUTTON", L"Settings", BS_OWNERDRAW | WS_TABSTOP, kSettings);
        edit_ = CreateChild(L"BUTTON", L"Edit accounts", BS_OWNERDRAW | WS_TABSTOP, kEditButton);
        close_ = CreateChild(L"BUTTON", L"Close", BS_OWNERDRAW | WS_TABSTOP, kClose);
        tagFilter_ = CreateChild(L"BUTTON", L"All tags", BS_OWNERDRAW | WS_TABSTOP,
                                   kTagFilter);
        const auto savedSort = ReadPreference(SortPreference(), static_cast<DWORD>(AccountSort::NameAscending));
        if (savedSort < kSortNames.size()) {
            sort_ = static_cast<AccountSort>(savedSort);
        }
        sortButton_ = CreateChild(L"BUTTON", kSortButtonNames[static_cast<std::size_t>(sort_)],
                                  BS_OWNERDRAW | WS_TABSTOP, kSort);
        accountList_ = CreateChild(L"LISTBOX", L"", LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_HASSTRINGS |
                                   LBS_OWNERDRAWFIXED | WS_TABSTOP, kAccounts);
        accountScroll_ = CreateChild(L"STATIC", L"Account scroll", SS_OWNERDRAW | SS_NOTIFY, kAccountScroll);
        ShowWindow(accountScroll_, SW_HIDE);
        emptyTitle_ = CreateChild(L"STATIC", L"", SS_NOPREFIX | SS_CENTER | SS_CENTERIMAGE);
        emptyHint_ = CreateChild(L"STATIC", L"", SS_NOPREFIX | SS_CENTER);
        importButton_ = CreateChild(L"BUTTON", L"Import accounts", BS_OWNERDRAW | WS_TABSTOP, kImportButton);
        countdown_ = CreateChild(L"STATIC", L"", SS_NOPREFIX | SS_CENTERIMAGE | SS_RIGHT);
        timerLabel_ = CreateChild(L"STATIC", L"Next code", SS_NOPREFIX | SS_CENTERIMAGE);
        progress_ = CreateChild(PROGRESS_CLASSW, L"Time until the next code", PBS_SMOOTH);
        note_ = CreateChild(L"STATIC", L"", SS_OWNERDRAW | SS_NOPREFIX);
        ShowWindow(note_, SW_HIDE);
        if (!title_ || !close_ || !tagFilter_ || !sortButton_ || !accountList_ ||
            !emptyTitle_ || !emptyHint_ || !importButton_ || !timerLabel_ || !countdown_ || !progress_ || !note_ || !settings_ || !edit_ ||
            !accountScroll_ || !SetWindowSubclass(accountScroll_, ScrollbarProc, 1, reinterpret_cast<DWORD_PTR>(this)) ||
            !SetWindowSubclass(accountList_, AccountListProc, 1, reinterpret_cast<DWORD_PTR>(this)) ||
            !SetWindowSubclass(progress_, ProgressProc, 1, reinterpret_cast<DWORD_PTR>(this)) ||
            !SetTimer(window_, kLockTimer, 1000, nullptr)) {
            return false;
        }
        const auto progressStyle = GetWindowLongPtrW(progress_, GWL_EXSTYLE);
        SetWindowLongPtrW(progress_, GWL_EXSTYLE, progressStyle & ~(WS_EX_STATICEDGE | WS_EX_CLIENTEDGE));
        SetWindowPos(progress_, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE |
                     SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(progress_, PBM_SETRANGE32, 0, 1000);
        buttonFeedback_[0].window = tagFilter_;
        buttonFeedback_[1].window = sortButton_;
        buttonFeedback_[2].window = settings_;
        buttonFeedback_[3].window = close_;
        buttonFeedback_[4].window = importButton_;
        buttonFeedback_[5].window = edit_;
        for (const auto& button : buttonFeedback_) {
            if (!SetWindowSubclass(button.window, ButtonProc, 1, reinterpret_cast<DWORD_PTR>(this))) {
                return false;
            }
        }
        textScale_ = systemTextSize_.Factor();
        dpi_ = GetDpiForWindow(window_);
        RefreshAppearance();
        sessionNotifications_ = WTSRegisterSessionNotification(window_, NOTIFY_FOR_THIS_SESSION);
        ReadWindowPlacement();
        shortcut_ = LoadHotkeyPreferences(standalone_);
        RegisterShortcut(shortcut_);
        if (!vaultPath_.empty()) {
            const auto found = GetFileAttributesW(vaultPath_.c_str()) != INVALID_FILE_ATTRIBUTES;
            const auto error = found ? ERROR_SUCCESS : GetLastError();
            localVaultExists_ = error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND;
        }
        RefreshTags();
        if (!sessionNotifications_) {
            SetNote(L"Windows session lock detection is unavailable.");
        }
        return true;
    }

    int Scale(int units) const { return MulDiv(units, dpi_, 96); }
    int Text(int units) const { return static_cast<int>(std::lround(units * textScale_)); }
    int RowHeight() const { return Scale(Text(34) + 12); }

    void RefreshTextSize() {
        const auto factor = systemTextSize_.Factor();
        if (factor != textScale_) {
            textScale_ = factor;
            if (IsWindowVisible(window_)) {
                PositionFlyout();
            }
        }
    }

    void UpdateFonts() {
        if (fontDpi_ == dpi_ && fontTextScale_ == textScale_) {
            return;
        }
        const auto old = std::array{labelFont_, smallFont_, titleFont_, codeFont_, iconFont_, strongFont_, badgeFont_};
        labelFont_ = CreateUiFont(Scale(Text(14)));
        smallFont_ = CreateUiFont(Scale(Text(12)));
        titleFont_ = CreateUiFont(Scale(Text(14)), FW_SEMIBOLD, L"Segoe UI Variable Display");
        codeFont_ = CreateUiFont(Scale(Text(14)), FW_SEMIBOLD, L"Consolas");
        iconFont_ = CreateUiFont(Scale(14), FW_NORMAL, L"Segoe Fluent Icons");
        strongFont_ = CreateUiFont(Scale(Text(14)), FW_SEMIBOLD);
        badgeFont_ = CreateUiFont(Scale(11), FW_SEMIBOLD);
        for (const auto child : {tagFilter_, sortButton_, accountList_, importButton_}) {
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(labelFont_), FALSE);
        }
        for (const auto child : {timerLabel_, emptyHint_, note_}) {
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont_), FALSE);
        }
        SendMessageW(countdown_, WM_SETFONT, reinterpret_cast<WPARAM>(strongFont_), FALSE);
        SendMessageW(emptyTitle_, WM_SETFONT, reinterpret_cast<WPARAM>(strongFont_), FALSE);
        SendMessageW(title_, WM_SETFONT, reinterpret_cast<WPARAM>(titleFont_), FALSE);
        for (const auto font : old) {
            DeleteObject(font);
        }
        SendMessageW(accountList_, LB_SETITEMHEIGHT, 0, RowHeight());
        fontDpi_ = dpi_;
        fontTextScale_ = textScale_;
    }

    int MeasureNoteHeight(int width) const {
        RECT bounds{0, 0, (std::max)(1, width), 0};
        if (const auto dc = GetDC(window_)) {
            const auto old = SelectObject(dc, smallFont_);
            DrawTextW(dc, noteText_.c_str(), static_cast<int>(noteText_.size()), &bounds,
                      DT_CALCRECT | DT_NOPREFIX | DT_WORDBREAK);
            SelectObject(dc, old);
            ReleaseDC(window_, dc);
        }
        return (std::max)(Scale(Text(16)), static_cast<int>(bounds.bottom)) + Scale(2);
    }

    SIZE LayoutControls(const RECT& work) {
        UpdateFonts();
        const int gutter = Scale(kGutter);
        const int width = (std::min)(Scale(368 + Text(100) - 100),
                                     static_cast<int>(work.right - work.left) - Scale(20));
        const int content = (std::max)(1, width - 2 * gutter);
        const int iconButton = Scale(30);
        const int header = (std::max)(iconButton, Scale(Text(21)));
        const int controlsY = gutter + header + Scale(9);
        const int command = Scale(Text(18) + 14);
        const bool hasAccounts = !accounts_.empty();
        const bool hasRows = accountCount_ > 0;
        const int listY = controlsY + (editing_ && hasAccounts ? command + Scale(8) : 0);
        const int timerLine = Scale(Text(17));
        const int timerTextWidth = Scale(Text(russianUi ? 106 : 82));
        const int secondsWidth = preferences_.showCountdown ? Scale(Text(48)) : 0;
        const bool stackedTimer = content < timerTextWidth + secondsWidth + Scale(96);
        const int timerHeight = hasRows ? timerLine + Scale(stackedTimer ? 38 : 26) : Scale(12);
        const int footerHeight = timerHeight + gutter;
        const int workHeight = work.bottom - work.top - Scale(20);
        const int rowsThatFit = std::clamp((workHeight - listY - footerHeight) / RowHeight(), 1, 6);
        const int listHeight = hasRows ? (std::min)(accountCount_, rowsThatFit) * RowHeight() :
            Scale(Text(64) + 36) + (hasAccounts ? 0 : command);
        const int listBottom = listY + listHeight;
        const auto place = [](HWND child, int x, int y, int w, int h) {
            MoveWindow(child, x, y, (std::max)(1, w), (std::max)(1, h), FALSE);
        };

        place(title_, gutter, gutter, content - 3 * iconButton - Scale(8), header);
        const int actionsY = gutter + (header - iconButton) / 2;
        place(settings_, gutter + content - 2 * iconButton, actionsY, iconButton, iconButton);
        place(edit_, gutter + content - 3 * iconButton, actionsY, iconButton, iconButton);
        place(close_, gutter + content - iconButton, actionsY, iconButton, iconButton);

        const int sortWidth = (std::min)(Scale(Text(110) + 38), content / 2);
        place(tagFilter_, gutter, controlsY, content - sortWidth - Scale(8), command);
        place(sortButton_, gutter + content - sortWidth, controlsY, sortWidth, command);
        const int scrollWidth = accountCount_ > rowsThatFit ? Scale(9) : 0;
        place(accountList_, gutter, listY, content - scrollWidth, listHeight);
        place(accountScroll_, gutter + content - scrollWidth, listY, scrollWidth, listHeight);
        ShowWindow(accountScroll_, scrollWidth ? SW_SHOWNA : SW_HIDE);

        emptyPanel_ = {gutter, listY, gutter + content, listBottom};
        const int emptyInset = Scale(16);
        const int emptyTitleHeight = Scale(Text(20));
        const int emptyTitleY = listY + Scale(16);
        const int emptyHintY = emptyTitleY + emptyTitleHeight + Scale(4);
        place(emptyTitle_, gutter + emptyInset, emptyTitleY, content - 2 * emptyInset, emptyTitleHeight);
        const int importWidth = (std::min)(content - 2 * emptyInset, Scale(Text(120) + 40));
        const int importY = listBottom - command - Scale(12);
        place(importButton_, gutter + (content - importWidth) / 2, importY, importWidth, command);
        const int hintBottom = hasAccounts ? listBottom : importY;
        place(emptyHint_, gutter + emptyInset, emptyHintY, content - 2 * emptyInset, hintBottom - emptyHintY - Scale(6));

        const int timerY = listBottom + Scale(14);
        footerSeparator_ = {gutter, listBottom + Scale(3), gutter + content, listBottom + Scale(4)};
        place(timerLabel_, gutter, timerY, timerTextWidth, timerLine);
        place(countdown_, gutter + content - secondsWidth, timerY, secondsWidth, timerLine);
        const int progressHeight = Scale(4);
        if (stackedTimer) {
            place(progress_, gutter, timerY + timerLine + Scale(6), content, progressHeight);
        } else {
            place(progress_, gutter + timerTextWidth, timerY + (timerLine - progressHeight) / 2,
                  content - timerTextWidth - secondsWidth - (secondsWidth ? Scale(12) : 0), progressHeight);
        }
        PositionNote(width, listBottom + footerHeight);

        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
        return {width, listBottom + footerHeight};
    }
    void RefreshAppearance() {
        if (accountList_) {
            ApplyPalette(WindowsPalette(preferences_.theme, systemTextSize_));
        }
    }

    void ApplyPalette(Palette palette) {
        palette_ = palette;
        UpdateCaptureProtection();
        ApplyWindowFrame(window_, palette_);
        DeleteObject(frameBrush_);
        DeleteObject(cardBrush_);
        frameBrush_ = CreateSolidBrush(palette_.surface);
        cardBrush_ = CreateSolidBrush(palette_.card);
        if (palette_.highContrast || !UseClientAnimations()) {
            CompleteMotion();
            FinishCopyPulse();
            UpdateInteractionFeedback();
        }
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }

    void ApplyPreferences(FlyoutPreferences updated) {
        const auto old = std::exchange(preferences_, updated);
        if (updated.russian != old.russian) {
            russianUi = updated.russian;
            SetWindowTextW(settings_, Ui(L"Settings"));
            SetWindowTextW(close_, Ui(L"Close"));
            SetWindowTextW(edit_, Ui(editing_ ? L"Finish editing" : L"Edit accounts"));
            SetWindowTextW(sortButton_, Ui(kSortButtonNames[static_cast<std::size_t>(sort_)]));
            SetWindowTextW(timerLabel_, Ui(L"Next code"));
            SetWindowTextW(progress_, Ui(L"Time until the next code"));
            SetWindowTextW(accountScroll_, Ui(L"Account scroll"));
            ClearNote();
            RefreshTags();
        }
        if (!updated.animateCopy && copyPulseRow_) {
            FinishCopyPulse();
        }
        if (updated.clipboardSeconds < old.clipboardSeconds) {
            clipboard_.Clear(window_, true);
        }
        if (!updated.hideAfterCopy) {
            KillTimer(window_, kHideAfterCopyTimer);
        }
        if (updated.showCountdown != old.showCountdown || updated.russian != old.russian) {
            UpdateFooterVisibility();
            displayedSecond_ = -1;
            if (IsWindowVisible(window_)) {
                PositionFlyout();
                UpdateCountdown();
            }
        }
        RefreshAppearance();
    }

    void UpdateCaptureProtection() {
        const bool sensitive = !accounts_.empty();
        const bool exclude = sensitive && !preferences_.allowScreenshots;
        displayProtected_ = SetWindowDisplayAffinity(window_, exclude ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE) && exclude;
    }

    void DrawFrame(HDC dc) const {
        RECT frame{};
        GetClientRect(window_, &frame);
        FillSurface(dc, frame, palette_.surface);
        if (accountCount_) {
            FillSurface(dc, footerSeparator_, palette_.stroke);
        } else {
            PaintRounded(dc, emptyPanel_, Scale(8), palette_.card, palette_.stroke);
        }
    }

    void ReleaseFonts() const {
        for (const auto font : {labelFont_, smallFont_, titleFont_, codeFont_, iconFont_, strongFont_, badgeFont_}) {
            if (font) {
                DeleteObject(font);
            }
        }
    }

    void DrawButton(const DRAWITEMSTRUCT& item) const {
        const auto rect = item.rcItem;
        const BufferedPaint paint(item.hDC, rect);
        const auto dc = paint.Dc();
        const bool pressed = (item.itemState & ODS_SELECTED) != 0;
        const bool focused = (item.itemState & (ODS_FOCUS | ODS_NOFOCUSRECT)) == ODS_FOCUS;
        const bool closing = item.CtlID == kClose;
        const bool editButton = item.CtlID == kEditButton;
        const bool editActive = editButton && editing_;
        const bool headerButton = closing || item.CtlID == kSettings || editButton;
        const bool disabled = (item.itemState & ODS_DISABLED) != 0;
        const auto feedback = std::ranges::find(buttonFeedback_, item.hwndItem, &ButtonFeedback::window);
        const int hover = disabled || feedback == buttonFeedback_.end() ? 0 :
                          palette_.highContrast ? (hotButton_ == item.hwndItem ? 100 : 0) :
                          feedback->fade.Value(GetTickCount64());
        const bool importing = item.CtlID == kImportButton;
        const bool unlock = importing && localVaultExists_ && !localVault_.Unlocked();
        const bool accent = importing && !disabled && !palette_.highContrast;
        const auto base = editActive ? palette_.selected : accent ? palette_.accent : headerButton ? palette_.surface : palette_.button;
        const auto fill = accent ? BlendColor(base, AccentText(base), pressed ? 16 : hover / 10) :
                          pressed ? palette_.pressed : BlendColor(base, palette_.hover, hover);
        const auto ink = disabled ? palette_.disabledText : accent ? AccentText(fill) :
                         palette_.highContrast && (pressed || hover) ? palette_.selectionText : palette_.buttonText;
        FillSurface(dc, rect, importing ? palette_.card : palette_.surface);
        if (!headerButton || editActive || (!disabled && (hover || pressed))) {
            PaintRounded(dc, rect, Scale(4), fill, headerButton && !palette_.highContrast ? fill : palette_.stroke);
        }
        if (focused) {
            PaintFocus(dc, rect, Scale(1), palette_.focusOuter, palette_.focusInner);
        }
        constexpr UINT centered = DT_CENTER | DT_VCENTER | DT_SINGLELINE;
        if (headerButton) {
            PaintText(dc, closing ? L"\ue711" : editButton ? L"\ue70f" : L"\ue713", rect, iconFont_,
                      editActive && palette_.highContrast ? palette_.selectionText : ink, centered);
            return;
        }

        wchar_t label[kMaxNameLength + 32]{};
        const auto length = GetWindowTextW(item.hwndItem, label, ARRAYSIZE(label));
        auto text = rect;
        text.left += Scale(12);
        text.right -= Scale(unlock ? 12 : 32);
        PaintText(dc, {label, static_cast<std::size_t>(length)}, text, labelFont_,
                  ink, (unlock ? DT_CENTER : DT_LEFT) | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (!unlock) {
            const RECT chevron{rect.right - Scale(30), rect.top, rect.right - Scale(6), rect.bottom};
            PaintText(dc, L"\ue70d", chevron, iconFont_, ink, centered);
        }
    }

    void DrawAccountRow(const DRAWITEMSTRUCT& item) const {
        const BufferedPaint paint(item.hDC, item.rcItem);
        PaintAccountRow(item, paint.Dc(), false);
    }

    void PaintAccountRow(const DRAWITEMSTRUCT& item, HDC dc, bool preview) const {
        auto row = item.rcItem;
        FillSurface(dc, row, palette_.surface);
        if (item.itemID == static_cast<UINT>(-1) || item.itemData >= accounts_.size()) {
            return;
        }
        row.bottom -= Scale(4);
        const bool selected = (item.itemState & ODS_SELECTED) != 0;
        const bool focused = (item.itemState & (ODS_FOCUS | ODS_NOFOCUSRECT)) == ODS_FOCUS;
        const bool dragged = !preview && drag_ && drag_->moving && drag_->source == item.itemID;
        const bool hovered = !preview && hoverRow_ == item.itemID;
        const bool pressed = hovered && copyPressed_ == item.itemData;
        const bool copied = !preview && !dragged && copyPulseRow_ == item.itemID;
        auto fill = pressed ? palette_.pressed : selected ? palette_.selected :
                    hovered ? palette_.hover : palette_.card;
        const bool inverted = palette_.highContrast && (selected || hovered);
        if (inverted) {
            fill = palette_.selected;
        }
        const auto ink = inverted ? palette_.selectionText : palette_.text;
        const auto muted = inverted ? ink : palette_.muted;
        if (dragged) {
            fill = palette_.surface;
        }
        const auto border = preview ? palette_.accent : palette_.highContrast ? palette_.stroke : fill;
        if (!preview && changedRow_ == item.itemID && !palette_.highContrast && !dragged) {
            fill = BlendColor(fill, palette_.accent, changedRowFade_.Value(GetTickCount64()) * 22 / 100);
        }
        const auto& account = accounts_[item.itemData];
        const auto& colorName = account.issuer.empty() ? account.label : account.issuer;
        const auto accountAccent = palette_.highContrast ? fill :
            TagMarkerColor(NameColorHash(colorName) >> 24, palette_);
        std::optional<COLORREF> gradientEnd;
        if (!palette_.highContrast && !dragged && !hovered && !selected) {
            gradientEnd = AccountTint(fill, accountAccent, muted, palette_.dark ? 24 : 19);
        }
        PaintRounded(dc, row, Scale(6), fill, border, gradientEnd);
        if (dragged) {
            if (focused) {
                PaintFocus(dc, row, Scale(1), palette_.focusOuter, palette_.focusInner);
            }
            DrawDragPreview(dc, item.rcItem);
            return;
        }
        if (focused) {
            PaintFocus(dc, row, Scale(1), palette_.focusOuter, palette_.focusInner);
        }
        const int center = (row.top + row.bottom) / 2;
        if (!account.tags.empty()) {
            const auto count = (std::min)(account.tags.size(), std::size_t{4});
            const auto height = (std::max<LONG>)(Scale(4), (row.bottom - row.top - Scale(10)) /
                                                          static_cast<LONG>(count) - Scale(2));
            const auto total = height * static_cast<LONG>(count) + Scale(2) * static_cast<LONG>(count - 1);
            for (std::size_t i = 0; i < count; ++i) {
                const auto top = center - total / 2 + static_cast<LONG>(i) * (height + Scale(2));
                auto color = TagMarkerColor(NameColorHash(account.tags[i]), palette_, inverted);
                if (!palette_.highContrast && account.tagColors[i]) {
                    const auto custom = static_cast<COLORREF>(account.tagColors[i] - 1);
                    color = custom;
                    for (int adjustment = 10; adjustment <= 100 && ColorContrast(color, fill) < 3.0; adjustment += 10) {
                        color = BlendColor(custom, ink, adjustment);
                    }
                }
                PaintRounded(dc, {row.left + Scale(4), top, row.left + Scale(8), top + height},
                             Scale(2), color, color);
            }
        } else if (selected && !palette_.highContrast) {
            const RECT mark{row.left + Scale(3), center - Scale(8), row.left + Scale(6), center + Scale(8)};
            PaintRounded(dc, mark, Scale(2), palette_.accent, palette_.accent);
        }

        const auto monogram = ProviderMonogram(account.issuer);
        const bool draggable = CanDragRows();
        if (draggable) {
            const int dot = Scale(2);
            for (int y = -1; y <= 1; ++y) {
                for (int x = 0; x < 2; ++x) {
                    const int left = row.left + Scale(14 + 5 * x);
                    const int top = center + Scale(5 * y) - dot / 2;
                    FillSurface(dc, {left, top, left + dot, top + dot}, muted);
                }
            }
        }
        const int iconLeft = row.left + Scale(draggable ? 32 : 12);
        const int iconTop = center - Scale(11);
        const RECT badge{iconLeft, iconTop, iconLeft + Scale(22), iconTop + Scale(22)};
        constexpr UINT centered = DT_SINGLELINE | DT_VCENTER | DT_CENTER;
        if (!palette_.highContrast) {
            const auto badgeFill = AccountTint(palette_.surface, accountAccent, palette_.text, 32);
            PaintRounded(dc, badge, Scale(6), badgeFill, badgeFill);
        }
        const auto badgeInk = palette_.highContrast ? ink : palette_.text;
        PaintText(dc, monogram.empty() ? std::wstring_view(L"\uea18") : std::wstring_view(monogram),
                  badge, monogram.empty() ? iconFont_ : badgeFont_, badgeInk, centered);

        const RECT copy{row.right - Scale(28), center - Scale(11), row.right - Scale(6), center + Scale(11)};
        if (hovered && !palette_.highContrast) {
            const auto copyFill = pressed ? palette_.surface : palette_.button;
            PaintRounded(dc, copy, Scale(4), copyFill, copyFill);
        }
        PaintText(dc, L"\ue8c8", copy, iconFont_, hovered ? ink : muted, centered);
        const int codeRight = copy.left - Scale(4);
        const int codeHalfHeight = Scale(Text(8) + 3);
        const RECT codeBounds{codeRight - Scale(Text(76) + 6), center - codeHalfHeight,
                              codeRight, center + codeHalfHeight};
        if (copied) {
            const auto badge = palette_.highContrast ? palette_.accent :
                BlendColor(palette_.accent, AccentText(palette_.accent), copyPulseAmount_);
            const auto badgeText = palette_.highContrast ? palette_.selectionText : AccentText(badge);
            PaintRounded(dc, codeBounds, Scale(6), badge, badge);
            const auto groupWidth = Scale(Text(40) + 24);
            const LONG origin = (codeBounds.left + codeBounds.right - groupWidth) / 2;
            RECT tick{origin, codeBounds.top, origin + Scale(18), codeBounds.bottom};
            OffsetRect(&tick, 0, Scale(copyPulseAmount_ / 6));
            PaintText(dc, L"\ue73e", tick, iconFont_, badgeText, centered);
            RECT confirmation{tick.right + Scale(4), codeBounds.top, codeBounds.right, codeBounds.bottom};
            PaintText(dc, Ui(L"Copied"), confirmation, smallFont_, badgeText, DT_SINGLELINE | DT_VCENTER);
        } else {
            const auto code = !preview && preferences_.showCodes &&
                (displayProtected_ || preferences_.allowScreenshots) && displayedSecond_ >= 0 ?
                CalculateAccountCode(account, displayedSecond_) : std::nullopt;
            wchar_t digits[10]{};
            const auto half = account.digits / 2;
            for (unsigned i = 0; i < account.digits; ++i) {
                digits[i + (i >= half)] = code ? code->text[i] : L'\u2022';
            }
            digits[half] = L'\u2002';
            if (!palette_.highContrast) {
                const auto well = BlendColor(fill, palette_.surface, 85);
                PaintRounded(dc, codeBounds, Scale(6), well, well);
            }
            PaintText(dc, digits, codeBounds, codeFont_, code ? ink : muted, centered);
            SecureZeroMemory(digits, sizeof(digits));
        }

        const int textLeft = badge.right + Scale(8);
        const int textRight = codeBounds.left - Scale(6);
        const auto name = AccountDisplayName(account.issuer, account.label);
        constexpr UINT labelStyle = DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
        if (account.issuer.empty()) {
            PaintText(dc, name, {textLeft, row.top, textRight, row.bottom}, strongFont_, ink, labelStyle);
        } else {
            const int firstLineTop = center - Scale(Text(18));
            PaintText(dc, account.issuer, {textLeft, firstLineTop, textRight, center + Scale(1)}, strongFont_, ink,
                      labelStyle);
            PaintText(dc, name, {textLeft, center + Scale(1), textRight, center + Scale(Text(17))},
                      smallFont_, muted, labelStyle);
        }
        if (!preview && drag_ && drag_->moving && drag_->target == item.itemID && drag_->target != drag_->source) {
            const auto y = drag_->target < drag_->source ? row.top : row.bottom - Scale(2);
            FillSurface(dc, {row.left, y, row.right, y + Scale(2)},
                        palette_.highContrast ? palette_.text : palette_.accent);
        }
        if (!preview) {
            DrawDragPreview(dc, item.rcItem);
        }
    }

    void DrawProgress(HDC target) const {
        RECT track{};
        GetClientRect(progress_, &track);
        const BufferedPaint paint(target, track);
        const auto dc = paint.Dc();
        FillSurface(dc, track, palette_.surface);
        const auto trackFill = palette_.highContrast ? palette_.surface : palette_.stroke;
        PaintRounded(dc, track, Scale(2), trackFill, palette_.stroke);
        if (progressTotal_) {
            track.right = track.left + static_cast<LONG>((track.right - track.left) * progressLeft_ / progressTotal_);
            PaintRounded(dc, track, Scale(2), palette_.accent, palette_.accent);
        }
    }
    NOTIFYICONDATAW IconData() const {
        NOTIFYICONDATAW icon{};
        icon.cbSize = sizeof(icon);
        icon.hWnd = window_;
        icon.uID = kIconId;
        return icon;
    }

    void AddIcon() {
        auto icon = IconData();
        icon.uFlags = NIF_MESSAGE | NIF_TIP | NIF_ICON | NIF_SHOWTIP;
        icon.uCallbackMessage = kTrayMessage;
        const auto key = CreateTrayKeyIcon();
        icon.hIcon = key ? key : LoadIconW(nullptr, IDI_SHIELD);
        wcscpy_s(icon.szTip, standalone_ ? L"TOTP flyout" : L"TOTP flyout (mod)");
        trayIconAdded_ = Shell_NotifyIconW(NIM_ADD, &icon);
        if (key) {
            DestroyIcon(key);
        }
        if (trayIconAdded_) {
            icon.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &icon);
        }
    }

    void UpdateTrayIcon() const {
        if (trayIconAdded_) {
            if (const auto key = CreateTrayKeyIcon()) {
                auto icon = IconData();
                icon.uFlags = NIF_ICON;
                icon.hIcon = key;
                Shell_NotifyIconW(NIM_MODIFY, &icon);
                DestroyIcon(key);
            }
        }
    }

    RECT IconRect() const {
        NOTIFYICONIDENTIFIER icon{};
        icon.cbSize = sizeof(icon);
        icon.hWnd = window_;
        icon.uID = kIconId;
        RECT rect{};
        if (FAILED(Shell_NotifyIconGetRect(&icon, &rect))) {
            POINT cursor{};
            GetCursorPos(&cursor);
            rect = {cursor.x, cursor.y, cursor.x + 1, cursor.y + 1};
        }
        return rect;
    }

    PCWSTR PlacementPreference() const { return standalone_ ? L"WindowPlacement" : L"ModWindowPlacement"; }

    PCWSTR PositionPreference() const { return standalone_ ? L"WindowPosition" : L"ModWindowPosition"; }

    void ReadWindowPlacement() {
        const auto stored = ReadPreference(PlacementPreference(), 0);
        const auto mode = LOBYTE(stored);
        if (mode <= static_cast<BYTE>(WindowPlacementMode::BottomRight)) {
            placement_ = {static_cast<WindowPlacementMode>(mode), (stored & 0x100) != 0, (stored & 0x200) != 0};
        }
        if (placement_.remember) {
            ManualWindowPosition position;
            DWORD size = sizeof(position);
            if (ReadPreferenceValue(PositionPreference(), RRF_RT_REG_BINARY, &position, &size) == ERROR_SUCCESS &&
                size == sizeof(position) &&
                position.monitor.front() && !position.monitor.back()) {
                manualPosition_ = position;
            }
        }
    }

    bool SaveManualPosition() {
        if (!manualPosition_) {
            return true;
        }
        return RegSetKeyValueW(HKEY_CURRENT_USER, kPreferencesPath, PositionPreference(), REG_BINARY,
                               &*manualPosition_, sizeof(*manualPosition_)) == ERROR_SUCCESS;
    }

    bool CanMoveWindow() const {
        return placement_.mode == WindowPlacementMode::Manual && !placement_.lockDrag &&
               !handlingCommand_ && motion_ != Motion::Leaving;
    }

    void FinishWindowMove() {
        RECT bounds{};
        if (!IsWindowVisible(window_) || !CanMoveWindow() ||
            !GetWindowRect(window_, &bounds) || EqualRect(&bounds, &moveStart_)) {
            return;
        }
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &info)) {
            return;
        }
        restingBounds_ = bounds;
        ManualWindowPosition position;
        std::ranges::copy(info.szDevice, position.monitor.begin());
        position.offset = {MulDiv(bounds.left - info.rcWork.left, 96, dpi_),
                           MulDiv(bounds.top - info.rcWork.top, 96, dpi_)};
        manualPosition_ = position;
        PositionFlyout();
        if (placement_.remember && !SaveManualPosition()) {
            SetNote(L"Window position applies to this session only.");
        }
    }

    static BOOL CALLBACK FindManualMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM context) {
        auto& lookup = *reinterpret_cast<std::pair<PCWSTR, HMONITOR>*>(context);
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, &info) && wcscmp(info.szDevice, lookup.first) == 0) {
            lookup.second = monitor;
            return FALSE;
        }
        return TRUE;
    }

    SIZE FlyoutSize(const RECT& work) {
        const auto size = LayoutControls(work);
        RECT frame{0, 0, size.cx, size.cy};
        AdjustWindowRectExForDpi(&frame, GetWindowLongW(window_, GWL_STYLE), FALSE,
                                 GetWindowLongW(window_, GWL_EXSTYLE), dpi_);
        return {frame.right - frame.left, frame.bottom - frame.top};
    }

    POINT TrayAnchorPosition(SIZE size, const MONITORINFO& info) const {
        const POINT anchor{std::clamp(anchor_.right, info.rcMonitor.left, info.rcMonitor.right - 1),
                           std::clamp(anchor_.top, info.rcMonitor.top, info.rcMonitor.bottom - 1)};
        auto exclusion = anchor_;
        InflateRect(&exclusion, Scale(8), Scale(8));
        RECT popup{anchor.x - size.cx, anchor.y - size.cy, anchor.x, anchor.y};
        CalculatePopupWindowPosition(&anchor, &size, TPM_RIGHTALIGN | TPM_BOTTOMALIGN | TPM_WORKAREA,
                                     &exclusion, &popup);
        return ClampFlyoutPosition({popup.left, popup.top}, size, info.rcWork);
    }

    void PositionFlyout() {
        if (movingWindow_) {
            return;
        }
        CompleteMotion();
        const bool manual = placement_.mode == WindowPlacementMode::Manual;
        std::pair<PCWSTR, HMONITOR> lookup{manualPosition_ ? manualPosition_->monitor.data() : nullptr, nullptr};
        if (manual && lookup.first) {
            EnumDisplayMonitors(nullptr, nullptr, FindManualMonitor, reinterpret_cast<LPARAM>(&lookup));
        }
        const auto monitor = lookup.second ? lookup.second : MonitorFromRect(&anchor_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{sizeof(info), {}, {}, 0};
        if (!GetMonitorInfoW(monitor, &info)) {
            return;
        }
        positioning_ = true;
        // Moving the HWND onto the target monitor provides its effective DPI via WM_DPICHANGED.
        for (int pass = 0; pass < 2; ++pass) {
            const auto previousDpi = dpi_;
            const auto size = FlyoutSize(info.rcWork);
            POINT point{};
            if (lookup.second) {
                point = RestoreManualFlyoutPosition(manualPosition_->offset, size, info.rcWork, dpi_);
            } else if (manual) {
                point = TrayAnchorPosition(size, info);
            } else {
                point = CornerFlyoutPosition(placement_.mode, size, info.rcWork, Scale(8));
            }
            SetWindowPos(window_, HWND_TOPMOST, point.x, point.y, size.cx, size.cy, SWP_NOACTIVATE);
            GetWindowRect(window_, &restingBounds_);
            if (previousDpi == dpi_) {
                break;
            }
        }
        positioning_ = false;

        const int distance = Scale(14);
        motionOffset_ = {0, distance};
        if (restingBounds_.top >= anchor_.bottom) {
            motionOffset_ = {0, -distance};
        } else if (restingBounds_.bottom > anchor_.top) {
            if (restingBounds_.right <= anchor_.left) {
                motionOffset_ = {distance, 0};
            } else if (restingBounds_.left >= anchor_.right) {
                motionOffset_ = {-distance, 0};
            }
        }
    }

    void ShowFlyout(bool activate = true) {
        const bool wasVisible = IsWindowVisible(window_) && motion_ != Motion::Leaving;
        UpdateCaptureProtection();
        anchor_ = IconRect();
        textScale_ = systemTextSize_.Factor();
        const auto palette = WindowsPalette(preferences_.theme, systemTextSize_);
        if (palette != palette_) {
            ApplyPalette(palette);
        }
        PositionFlyout();
        if (!wasVisible && MotionEnabled() &&
            SetTimer(window_, kMotionTimer, USER_TIMER_MINIMUM, nullptr)) {
            motion_ = Motion::Entering;
            motionStart_ = std::chrono::steady_clock::now();
            MoveForMotion(1.0);
        }
        lastActivity_ = GetTickCount64();
        UpdateCountdown();
        ShowWindow(window_, activate ? SW_SHOW : SW_SHOWNOACTIVATE);
        if (activate) {
            previewVisible_ = false;
            SetForegroundWindow(window_);
            if (!wasVisible || !IsChild(window_, GetFocus())) {
                SetFocus(accounts_.empty() ? importButton_ : editing_ ? tagFilter_ : accountList_);
            }
        }
        SetTimer(window_, kTimer, 250, nullptr);
    }

    bool MotionEnabled() const { return !palette_.highContrast && UseClientAnimations(); }

    void DismissFlyout() {
        if (!IsWindowVisible(window_) || motion_ == Motion::Leaving) {
            return;
        }
        EndRowDrag();
        if (!MotionEnabled() || !SetTimer(window_, kMotionTimer, USER_TIMER_MINIMUM, nullptr)) {
            HideFlyout();
            return;
        }
        RECT current{};
        GetWindowRect(window_, &current);
        const auto remaining = motionOffset_.x ? current.left - restingBounds_.left :
                                                current.top - restingBounds_.top;
        const auto offset = motionOffset_.x ? motionOffset_.x : motionOffset_.y;
        exitStartOffset_ = offset ? std::clamp(static_cast<double>(remaining) / offset, 0.0, 1.0) : 0.0;
        motion_ = Motion::Leaving;
        motionStart_ = std::chrono::steady_clock::now();
    }

    void HideFlyout() {
        ShowWindow(window_, SW_HIDE);
        motion_ = Motion::Idle;
        KillTimer(window_, kTimer);
        KillTimer(window_, kMotionTimer);
        KillTimer(window_, kHideAfterCopyTimer);
        StopCopyPulse();
        ClearNote();
        EndRowDrag();
        SetHotButton(nullptr);
        changedRow_.reset();
        KillTimer(window_, kFeedbackTimer);
        const auto now = GetTickCount64();
        for (auto& button : buttonFeedback_) {
            button.fade.Set(0, now, false);
            button.animating = false;
        }
        hoverRow_.reset();
        if (GetCapture() == accountList_) {
            ReleaseCapture();
        }
        SetWindowTextW(countdown_, L"");
        SendMessageW(progress_, PBM_SETPOS, 0, 0);
        progressLeft_ = 0;
        progressTotal_ = 0;
        previewVisible_ = false;
        displayedSecond_ = -1;
    }

    void MoveForMotion(double offset) const {
        const auto x = restingBounds_.left + static_cast<LONG>(std::lround(motionOffset_.x * offset));
        const auto y = restingBounds_.top + static_cast<LONG>(std::lround(motionOffset_.y * offset));
        SetWindowPos(window_, nullptr, x, y, 0, 0, SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    void CompleteMotion() {
        if (motion_ == Motion::Idle) {
            return;
        }
        KillTimer(window_, kMotionTimer);
        if (std::exchange(motion_, Motion::Idle) == Motion::Leaving) {
            HideFlyout();
        } else {
            MoveForMotion(0.0);
        }
    }

    void UpdateMotion() {
        if (motion_ == Motion::Idle) {
            return;
        }
        const bool entering = motion_ == Motion::Entering;
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                        motionStart_).count();
        const auto progress = elapsed / (entering ? kEnterDurationMs : kExitDurationMs);
        if (progress >= 1.0 || !MotionEnabled()) {
            CompleteMotion();
            return;
        }
        // Fluent direct entrance (0,0,0,1) and gentle exit (1,0,1,1).
        const auto t = entering ? std::cbrt(progress) : 1.0 - std::cbrt(1.0 - progress);
        const auto eased = t * t * (3.0 - 2.0 * t);
        const auto offset = entering ? 1.0 - eased : exitStartOffset_ + (1.0 - exitStartOffset_) * eased;
        MoveForMotion(offset);
    }
    UINT TrackMenu(ThemedMenu& menu, POINT anchor, const RECT* origin = nullptr) {
        activeMenu_ = &menu;
        const auto command = menu.Track(window_, anchor, origin);
        activeMenu_ = nullptr;
        return command;
    }

    void ShowTrayMenu() {
        if (handlingCommand_) {
            return;
        }
        ThemedMenu menu(palette_, dpi_, textScale_);
        menu.Add(kOpen, L"Open TOTP flyout");
        menu.Add(kTraySettings, L"Settings...", MFS_ENABLED, false, L"\ue713");
        if (checkUpdates_) {
            menu.Add(kTrayCheckUpdates, L"Check for updates...");
        }
        menu.Separator();
        menu.Add(kExit, L"Exit", MFS_ENABLED, false, L"\ue711");
        const auto icon = IconRect();
        handlingCommand_ = true;
        SetForegroundWindow(window_);
        const auto command = TrackMenu(menu, {icon.left, icon.top});
        handlingCommand_ = false;
        PostMessageW(window_, WM_NULL, 0, 0);
        if (command == kOpen) {
            ShowFlyout();
        } else if (command == kTraySettings) {
            ShowFlyout();
            ShowSettings();
        } else if (command == kTrayCheckUpdates && checkUpdates_) {
            checkUpdates_(window_);
        }
        if (command == kExit) {
            SendMessageW(window_, WM_CLOSE, 0, 0);
        }
    }

    void RefreshTags() {
        if (filter_ && !filter_->empty()) {
            const auto names = AccountTags(accounts_);
            const auto match = std::ranges::find_if(names, [&](auto name) { return SameName(*filter_, name); });
            if (match == names.end()) {
                filter_.reset();
            } else {
                filter_ = *match;
            }
        }
        const auto label = !filter_ ? Ui(L"All tags") : filter_->empty() ? Ui(L"Untagged") : filter_->c_str();
        SetWindowTextW(tagFilter_, label);
        InvalidateRect(tagFilter_, nullptr, FALSE);
        RefreshList();
    }

    PCWSTR SortPreference() const { return standalone_ ? L"AccountSort" : L"ModAccountSort"; }

    std::optional<std::wstring_view> TagFilter() const {
        return filter_ ? std::optional<std::wstring_view>(*filter_) : std::nullopt;
    }

    std::vector<std::wstring> TagChoices() const {
        const auto names = AccountTags(accounts_);
        return {names.begin(), names.end()};
    }

    void ShowSortMenu() {
        if (handlingCommand_) {
            return;
        }
        const auto generation = lockGeneration_;
        lastActivity_ = GetTickCount64();
        handlingCommand_ = true;
        ThemedMenu menu(palette_, dpi_, textScale_);
        const auto previous = static_cast<std::size_t>(sort_);
        for (std::size_t i = 0; i < kSortNames.size(); ++i) {
            menu.Add(static_cast<UINT>(i + 1), kSortNames[i], i == previous ? MFS_CHECKED : MFS_ENABLED);
        }
        RECT bounds{};
        GetWindowRect(sortButton_, &bounds);
        const auto id = TrackMenu(menu, {bounds.left, bounds.bottom}, &bounds);
        handlingCommand_ = false;
        if (generation != lockGeneration_ || !id || id > kSortNames.size() || id - 1 == previous) {
            return;
        }
        sort_ = static_cast<AccountSort>(id - 1);
        WritePreference(SortPreference(), id - 1);
        SetWindowTextW(sortButton_, Ui(kSortButtonNames[id - 1]));
        InvalidateRect(sortButton_, nullptr, FALSE);
        RefreshList();
    }

    void ShowTagMenu() {
        if (handlingCommand_ || !editing_) {
            return;
        }
        ThemedMenu menu(palette_, dpi_, textScale_);
        lastActivity_ = GetTickCount64();
        const auto generation = lockGeneration_;
        handlingCommand_ = true;
        menu.Add(1, L"All tags", !filter_ ? MFS_CHECKED : MFS_ENABLED);
        menu.Add(2, L"Untagged", filter_ && filter_->empty() ? MFS_CHECKED : MFS_ENABLED);
        const auto names = AccountTags(accounts_);
        if (!names.empty()) {
            menu.Separator();
        }
        for (std::size_t i = 0; i < names.size(); ++i) {
            const bool selected = filter_ && SameName(*filter_, names[i]);
            menu.Add(static_cast<UINT>(i + 3), names[i], selected ? MFS_CHECKED : MFS_ENABLED);
        }
        menu.Separator();
        menu.Add(kEditAccountTags, L"Edit tags...", selected_ ? MFS_ENABLED : MFS_DISABLED, false, L"\ue70f");
        if (!names.empty()) {
            menu.Add(kEditSelectedTag, L"Rename or remove selected tag...",
                     filter_ && !filter_->empty() ? MFS_ENABLED : MFS_DISABLED);
        }
        RECT position{};
        GetWindowRect(tagFilter_, &position);
        const auto id = TrackMenu(menu, {position.left, position.bottom}, &position);
        if (generation == lockGeneration_) {
            if (id == kEditAccountTags) {
                AssignTags();
            } else if (id == kEditSelectedTag && filter_ && !filter_->empty()) {
                RenameTag(*filter_);
            }
        }
        handlingCommand_ = false;
        if (generation != lockGeneration_ || id == 0 || id == kEditSelectedTag || id == kEditAccountTags) {
            return;
        }
        if (id == 1) {
            filter_.reset();
        } else if (id == 2) {
            filter_.emplace();
        } else if (id > 2 && static_cast<std::size_t>(id - 3) < names.size()) {
            filter_ = names[id - 3];
        }
        RefreshTags();
    }

    void RefreshList() {
        EndRowDrag();
        changedRow_.reset();
        StopCopyPulse();
        hoverRow_.reset();
        SendMessageW(accountList_, WM_SETREDRAW, FALSE, 0);
        SendMessageW(accountList_, LB_RESETCONTENT, 0, 0);
        int rowToSelect = 0;
        const auto rows = AccountRows(accounts_, TagFilter(), sort_);
        for (const auto i : rows) {
            const auto& account = accounts_[i];
            const auto label = AccountDisplayName(account.issuer, account.label);
            auto text = account.issuer.empty() ? std::wstring(label) : account.issuer + L" \u2014 " + std::wstring(label);
            if (!account.tags.empty()) {
                text += Ui(L". Tags:");
                for (const auto& tag : account.tags) {
                    text += L" " + tag + L";";
                }
            }
            const auto row = SendMessageW(accountList_, LB_ADDSTRING, 0,
                                           reinterpret_cast<LPARAM>(text.c_str()));
            if (row >= 0) {
                SendMessageW(accountList_, LB_SETITEMDATA, row, i);
                if (selected_ == i) {
                    rowToSelect = static_cast<int>(row);
                }
            }
        }
        SendMessageW(accountList_, LB_SETCURSEL, rowToSelect, 0);
        SendMessageW(accountList_, WM_SETREDRAW, TRUE, 0);
        const auto previousCount = accountCount_;
        const auto count = SendMessageW(accountList_, LB_GETCOUNT, 0, 0);
        accountCount_ = count > 0 ? static_cast<int>(count) : 0;
        const bool hasRows = accountCount_ > 0;
        const bool hasAccounts = !accounts_.empty();
        const bool showToolbar = editing_ && hasAccounts;
        const bool hadToolbar = (GetWindowLongW(tagFilter_, GWL_STYLE) & WS_VISIBLE) != 0;
        const auto focus = GetFocus();
        if ((!hasRows && focus == accountList_) ||
            (!showToolbar && (focus == tagFilter_ || focus == sortButton_))) {
            SetFocus(hasRows ? accountList_ : settings_);
        }
        ShowWindow(tagFilter_, showToolbar ? SW_SHOWNA : SW_HIDE);
        ShowWindow(sortButton_, showToolbar ? SW_SHOWNA : SW_HIDE);
        ShowWindow(accountList_, hasRows ? SW_SHOWNA : SW_HIDE);
        ShowWindow(emptyTitle_, hasRows ? SW_HIDE : SW_SHOWNA);
        ShowWindow(emptyHint_, hasRows ? SW_HIDE : SW_SHOWNA);
        if (hasAccounts && GetFocus() == importButton_) {
            SetFocus(showToolbar ? tagFilter_ : accountList_);
        }
        ShowWindow(importButton_, hasAccounts ? SW_HIDE : SW_SHOWNA);
        const bool vaultLocked = localVaultExists_ && !localVault_.Unlocked();
        SetWindowTextW(importButton_, Ui(vaultLocked ? L"Unlock vault" : L"Import accounts"));
        EnableWindow(importButton_, !vaultLocked || (sessionAvailable_ && sessionNotifications_));
        UpdateFooterVisibility();
        if (!hasRows) {
            SetWindowTextW(emptyTitle_, Ui(hasAccounts ? L"No matching accounts" :
                           vaultLocked ? L"Local vault is locked" : L"No accounts open"));
            SetWindowTextW(emptyHint_, Ui(hasAccounts ? L"Choose All tags to see your accounts." : vaultLocked ?
                (LocalVault::IsWindowsProtected(vaultPath_) ? L"Use your Windows account to unlock the vault." :
                                                             L"Enter your master password to open your accounts.") :
                              L"Import setup links or restore an encrypted backup."));
        }
        if ((accountCount_ != previousCount || showToolbar != hadToolbar) && IsWindowVisible(window_)) {
            PositionFlyout();
        }
        InvalidateRect(accountList_, nullptr, FALSE);
        UpdateSelection();
        displayedSecond_ = -1;
        if (IsWindowVisible(window_)) {
            UpdateCountdown();
        }
    }

    void ToggleEditing() {
        editing_ = !editing_;
        SetWindowTextW(edit_, Ui(editing_ ? L"Finish editing" : L"Edit accounts"));
        if (!editing_) {
            filter_.reset();
        }
        RefreshTags();
        InvalidateRect(edit_, nullptr, FALSE);
    }

    void ReorderRows(LRESULT sourceRow, LRESULT targetRow) {
        if (!editing_ || sort_ != AccountSort::Manual || sourceRow < 0 || targetRow < 0 ||
            sourceRow >= accountCount_ || targetRow >= accountCount_) {
            return;
        }
        const auto source = SendMessageW(accountList_, LB_GETITEMDATA, sourceRow, 0);
        const auto target = SendMessageW(accountList_, LB_GETITEMDATA, targetRow, 0);
        auto proposed = accounts_;
        if (source < 0 || target < 0 || !MoveAccount(proposed, source, target, TagFilter()) ||
            !SaveAccounts(std::move(proposed))) {
            return;
        }
        lastActivity_ = GetTickCount64();
        selected_ = static_cast<std::size_t>(target);
        RefreshList();
        HighlightChangedRow();
        SetNote(L"Manual order updated.");
    }

    void MoveSelected(int direction) {
        const auto row = SendMessageW(accountList_, LB_GETCURSEL, 0, 0);
        if (row >= 0) {
            ReorderRows(row, row + direction);
        }
    }

    Account* SelectedAccount() {
        return selected_ && *selected_ < accounts_.size() ? &accounts_[*selected_] : nullptr;
    }

    void UpdateSelection() {
        const auto row = SendMessageW(accountList_, LB_GETCURSEL, 0, 0);
        const auto index = row >= 0 ? SendMessageW(accountList_, LB_GETITEMDATA, row, 0) : LB_ERR;
        selected_.reset();
        if (index >= 0 && static_cast<std::size_t>(index) < accounts_.size()) {
            selected_ = static_cast<std::size_t>(index);
        }
    }

    void PositionNote(int width, int height) const {
        const int gutter = Scale(kGutter);
        const int toastWidth = (std::max)(1, width - 2 * gutter);
        const int toastHeight = MeasureNoteHeight(toastWidth - Scale(24)) + Scale(12);
        SetWindowPos(note_, HWND_TOP, gutter, (std::max)(0, height - gutter - toastHeight),
                     toastWidth, toastHeight, SWP_NOACTIVATE);
    }

    void UpdateFooterVisibility() const {
        const bool showTimer = accountCount_ > 0 && noteText_.empty();
        ShowWindow(timerLabel_, showTimer ? SW_SHOWNA : SW_HIDE);
        ShowWindow(countdown_, showTimer && preferences_.showCountdown ? SW_SHOWNA : SW_HIDE);
        ShowWindow(progress_, showTimer ? SW_SHOWNA : SW_HIDE);
    }

    void SetNote(PCWSTR text, UINT lifetime = 3600) {
        StopCopyPulse();
        noteText_ = Ui(text);
        SetWindowTextW(note_, noteText_.c_str());
        if (!SetTimer(window_, kCopyNoteTimer, lifetime, nullptr)) {
            ClearNote();
            return;
        }
        RECT client{};
        GetClientRect(window_, &client);
        UpdateFooterVisibility();
        PositionNote(client.right, client.bottom);
        ShowWindow(note_, SW_SHOWNA);
        InvalidateRect(note_, nullptr, FALSE);
        NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, note_, OBJID_CLIENT, CHILDID_SELF);
    }

    void ClearNote() {
        KillTimer(window_, kCopyNoteTimer);
        noteText_.clear();
        SetWindowTextW(note_, L"");
        ShowWindow(note_, SW_HIDE);
        UpdateFooterVisibility();
    }

    void StopCopyPulse() {
        KillTimer(window_, kCopyPulseTimer);
        InvalidateRow(std::exchange(copyPulseRow_, std::nullopt));
    }

    void FinishCopyPulse() {
        KillTimer(window_, kCopyPulseTimer);
        copyPulseAmount_ = 0;
        InvalidateRow(copyPulseRow_);
    }

    void UpdateCopyPulse() {
        const auto elapsed = GetTickCount64() - copyPulseStart_;
        const auto phase = static_cast<double>((std::min)(elapsed, kCopyPulseMs)) / kCopyPulseMs;
        if (elapsed >= kCopyPulseMs || !MotionEnabled()) {
            FinishCopyPulse();
            return;
        }
        const auto remaining = 1 - phase;
        const auto old = std::exchange(copyPulseAmount_, static_cast<int>(24 * remaining * remaining));
        if (old != copyPulseAmount_) {
            InvalidateRow(copyPulseRow_);
        }
    }

    void ShowCopyFeedback() {
        SetNote(L"Code copied.", kCopyConfirmationMs);
        const auto row = SendMessageW(accountList_, LB_GETCURSEL, 0, 0);
        if (row >= 0) {
            copyPulseRow_ = static_cast<UINT>(row);
            copyPulseStart_ = GetTickCount64();
            const bool animate = preferences_.animateCopy && MotionEnabled();
            copyPulseAmount_ = animate && SetTimer(window_, kCopyPulseTimer, 16, nullptr) ? 24 : 0;
            InvalidateRow(copyPulseRow_);
        }
    }

    void StopHotkey() {
        if (const auto id = std::exchange(activeHotkey_, 0)) {
            UnregisterHotKey(window_, id);
        }
    }

    bool RegisterShortcut(HotkeyPreferences shortcut) {
        if (!shortcut.enabled) {
            StopHotkey();
            return true;
        }
        const auto key = LOBYTE(shortcut.chord);
        const auto modifiers = HotkeyModifiers(shortcut.chord);
        if (activeHotkey_ && key == LOBYTE(shortcut_.chord) && modifiers == HotkeyModifiers(shortcut_.chord)) {
            return true;
        }
        const bool alternate = activeHotkey_ && activeHotkey_ == hotkeyIds_[0];
        auto& id = hotkeyIds_[alternate ? 1 : 0];
        if (!id) {
            if (standalone_) {
                id = GlobalAddAtomW(alternate ? L"2FA Flyout standalone shortcut B" : L"2FA Flyout standalone shortcut A");
            } else {
                id = GlobalAddAtomW(alternate ? L"2FA Flyout mod shortcut B" : L"2FA Flyout mod shortcut A");
            }
        }
        if (!id || !RegisterHotKey(window_, id, modifiers | MOD_NOREPEAT, key)) {
            return false;
        }
        StopHotkey();
        activeHotkey_ = id;
        return true;
    }

    void ShowShortcut() {
        const auto generation = lockGeneration_;
        const auto applyShortcut = [&](HotkeyPreferences updated) {
            if (generation != lockGeneration_ || !RegisterShortcut(updated)) {
                return false;
            }
            if (shortcut_ != updated) {
                shortcut_ = updated;
                if (!WritePreference(HotkeyPreferenceName(standalone_), MAKELONG(updated.chord, updated.enabled))) {
                    SetNote(L"The shortcut applies to this session only.");
                }
            }
            return true;
        };
        EditShortcut(window_, preferences_.theme, shortcut_, shortcut_.enabled && !activeHotkey_, applyShortcut);
    }

    void ShowBackup() {
        const auto generation = lockGeneration_;
        const auto action = ChooseBackupAction(window_, preferences_.theme,
                                               sessionAvailable_ && !accounts_.empty(),
                                               sessionAvailable_ && sessionNotifications_);
        if (generation != lockGeneration_) {
            return;
        }
        switch (action) {
            case BackupAction::Export: ExportVault(); break;
            case BackupAction::Restore: OpenVault(); break;
            case BackupAction::ExportAegis: ExportPortable(ExportFormat::Aegis); break;
            case BackupAction::ExportAndOtp: ExportPortable(ExportFormat::AndOtp); break;
            case BackupAction::None: break;
        }
    }

    void ShowPositionSettings() {
        const auto updated = EditWindowPlacement(window_, preferences_.theme, placement_);
        if (!updated || *updated == placement_) {
            return;
        }
        const bool rememberChanged = placement_.remember != updated->remember;
        placement_ = *updated;
        const DWORD stored = static_cast<DWORD>(placement_.mode) |
                             (placement_.lockDrag ? 0x100 : 0) | (placement_.remember ? 0x200 : 0);
        bool saved = WritePreference(PlacementPreference(), stored);
        if (rememberChanged) {
            if (placement_.remember && manualPosition_) {
                saved &= SaveManualPosition();
            } else {
                for (const auto path : {kPreferencesPath, kPreviousPreferencesPath}) {
                    const auto result = RegDeleteKeyValueW(HKEY_CURRENT_USER, path, PositionPreference());
                    saved &= result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
                }
            }
        }
        if (IsWindowVisible(window_)) {
            PositionFlyout();
        }
        if (!saved) {
            SetNote(L"Some window settings apply to this session only.");
        }
    }

    void ShowSettings() {
        if (handlingCommand_) {
            return;
        }
        const auto generation = lockGeneration_;
        lastActivity_ = GetTickCount64();
        handlingCommand_ = true;
        ThemedMenu imports(palette_, dpi_, textScale_);
        ThemedMenu accounts(palette_, dpi_, textScale_);
        ThemedMenu menu(palette_, dpi_, textScale_);
        AddImportCommands(imports);
        AddAccountCommands(accounts, imports);
        menu.Add(kManageSettings, L"Manage accounts", MFS_ENABLED, false, L"\ue77b", &accounts);
        menu.Separator();
        if (standalone_) {
            menu.Add(kAppearanceSettings, L"Appearance and privacy...", MFS_ENABLED, false, L"\ue713");
        }
        menu.Add(kShortcutSettings, L"Shortcut...", MFS_ENABLED, false, L"\ue765");
        menu.Add(kBackupSettings, L"Backup...", MFS_ENABLED, false, L"\ue8b7");
        menu.Add(kPositionSettings, L"Window position...", MFS_ENABLED, false, L"\ue922");
        RECT bounds{};
        GetWindowRect(settings_, &bounds);
        const auto action = TrackMenu(menu, {bounds.left, bounds.bottom}, &bounds);
        if (generation == lockGeneration_) {
            switch (action) {
                case kAppearanceSettings: {
                    const auto updated = EditPreferences(window_, preferences_);
                    if (updated && generation == lockGeneration_ && *updated != preferences_) {
                        const auto saved = SavePreferences(preferences_, *updated);
                        ApplyPreferences(*updated);
                        if (!saved) {
                            SetNote(L"Some settings apply to this session only.");
                        }
                    }
                    break;
                }
                case kShortcutSettings: ShowShortcut(); break;
                case kBackupSettings: ShowBackup(); break;
                case kPositionSettings: ShowPositionSettings(); break;
                default: HandleAccountCommand(action); break;
            }
        }
        handlingCommand_ = false;
    }

    void AddImportCommands(ThemedMenu& menu) const {
        const auto state = sessionNotifications_ && sessionAvailable_ ? MFS_ENABLED : MFS_DISABLED;
        menu.Add(ImportClipboard, L"From clipboard", state, false, L"\ue77f");
        menu.Add(ImportLink, L"Enter TOTP link...", state);
        menu.Add(ImportText, L"From TXT file...", state);
        menu.Add(ImportHtml, L"From HTML file...", state);
        menu.Separator();
        menu.Add(ImportVault, L"Restore encrypted backup...", state);
    }

    void ShowImportAccounts() {
        if (handlingCommand_ || !accounts_.empty()) {
            return;
        }
        handlingCommand_ = true;
        lastActivity_ = GetTickCount64();
        if (localVaultExists_ && !localVault_.Unlocked()) {
            UnlockVault();
            handlingCommand_ = false;
            return;
        }
        const auto generation = lockGeneration_;
        ThemedMenu menu(palette_, dpi_, textScale_);
        const auto adding = sessionNotifications_ && sessionAvailable_ ? MFS_ENABLED : MFS_DISABLED;
        menu.Add(AddAccount, L"Enter setup key manually...", adding);
        menu.Separator();
        AddImportCommands(menu);
        RECT bounds{};
        GetWindowRect(importButton_, &bounds);
        const auto action = TrackMenu(menu, {bounds.left, bounds.bottom}, &bounds);
        if (generation == lockGeneration_) {
            HandleAccountCommand(action);
        }
        handlingCommand_ = false;
    }

    void AddAccountCommands(ThemedMenu& menu, ThemedMenu& imports) const {
        const auto adding = sessionNotifications_ && sessionAvailable_ ? MFS_ENABLED : MFS_DISABLED;
        menu.Add(AddAccount, L"Add account...", adding, false, L"\ue710");
        menu.Add(ImportMenu, L"Import accounts", MFS_ENABLED, false, L"\ue8b7", &imports);
        menu.Separator();
        menu.Add(SetTags, L"Edit tags...", selected_ ? MFS_ENABLED : MFS_DISABLED, false, L"\ue8ec");
        if (localVault_.Unlocked()) {
            menu.Add(Lock, L"Lock accounts", MFS_ENABLED, false, L"\ue72e");
            if (localVault_.HasMasterPassword()) {
                menu.Add(UseWindowsVault, L"Use Windows protection without a password...");
            }
        } else if (localVaultExists_) {
            menu.Add(UnlockLocalVault, L"Unlock local vault...", adding, false, L"\ue72e");
        }
    }

    void HandleAccountCommand(UINT action) {
        switch (action) {
            case AddAccount: AddManualAccount(); break;
            case ImportClipboard: ImportAccountClipboard(); break;
            case ImportLink: ImportAccount(); break;
            case ImportText: ImportAccountFile(false); break;
            case ImportHtml: ImportAccountFile(true); break;
            case ImportVault: OpenVault(); break;
            case SetTags: AssignTags(); break;
            case UnlockLocalVault: UnlockVault(); break;
            case Lock: LockSession(); break;
            case UseWindowsVault: {
                const auto generation = lockGeneration_;
                if (ConfirmAction(window_, preferences_.theme, L"Change local vault protection?",
                    L"Use your Windows user account to protect the vault.",
                    Ui(L"Apps running as this Windows user can also unlock it."), L"Use Windows") &&
                    generation == lockGeneration_) {
                    SetNote(localVault_.UseWindowsProtection(accounts_) ? L"The local vault now uses Windows protection." :
                            L"Could not change vault protection. The previous protection is still in use.");
                }
                break;
            }
        }
    }

    void RemoveSelectedAccount() {
        const auto index = selected_;
        const auto account = SelectedAccount();
        if (!index || !account) {
            return;
        }
        const auto name = account->issuer.empty() ? account->label : account->issuer + L" \u2014 " + account->label;
        const auto generation = lockGeneration_;
        if (!ConfirmAccountRemoval(window_, name.c_str(), preferences_.theme) ||
            generation != lockGeneration_ || *index >= accounts_.size()) {
            return;
        }
        auto proposed = accounts_;
        proposed.erase(proposed.begin() + *index);
        if (!SaveAccounts(std::move(proposed))) {
            return;
        }
        clipboard_.Clear(window_, true);
        if (accounts_.empty()) {
            selected_.reset();
        } else {
            selected_ = (std::min)(*index, accounts_.size() - 1);
        }
        lastActivity_ = GetTickCount64();
        RefreshTags();
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        UpdateCaptureProtection();
        SetNote(L"Account removed from the local vault.");
    }

    void ImportAccount() {
        const auto generation = lockGeneration_;
        auto input = PromptText(window_, L"Import TOTP link", L"Paste the otpauth://totp/ setup link.",
                                kMaxUriLength, true, L"", preferences_.theme);
        if (!input || generation != lockGeneration_) {
            return;
        }
        auto account = ParseOtpAuth(TextView(*input));
        input.reset();
        if (!account) {
            SetNote(L"This TOTP link has an invalid or unsupported format.");
            return;
        }
        std::vector<Account> imported;
        imported.push_back(std::move(*account));
        ApplyImport(std::move(imported));
    }

    void ImportAccountClipboard() {
        if (!sessionNotifications_ || !sessionAvailable_) {
            return;
        }
        const auto generation = lockGeneration_;
        auto contents = ReadClipboardSetupText(window_);
        if (generation != lockGeneration_) {
            return;
        }
        if (!contents) {
            SetNote(L"Could not read clipboard text. Copy one or more TOTP links and try again.");
            return;
        }
        auto imported = ParseLinkFile(contents->bytes, false);
        contents.reset();
        if (!imported) {
            SetNote(L"The clipboard must contain 1 to 256 distinct, valid TOTP links.");
            return;
        }
        ApplyImport(std::move(*imported));
    }

    void ImportAccountFile(bool html) {
        if (!sessionNotifications_ || !sessionAvailable_) {
            return;
        }
        const auto generation = lockGeneration_;
        const auto path = OpenLinkFilePath(window_, html);
        if (path.empty() || generation != lockGeneration_) {
            return;
        }
        auto data = ReadLinkFile(path);
        if (!data) {
            SetNote(L"Could not read this file. Choose a TXT or HTML file up to 1 MB.");
            return;
        }
        auto imported = ParseLinkFile(data->bytes, html);
        data.reset();
        if (!imported) {
            SetNote(L"Import requires 1 to 256 distinct, valid TOTP links.");
            return;
        }
        ApplyImport(std::move(*imported));
    }

    void AddManualAccount() {
        if (!sessionNotifications_ || !sessionAvailable_) {
            return;
        }
        lastActivity_ = GetTickCount64();
        const auto generation = lockGeneration_;
        const auto tags = TagChoices();
        const auto initialTag = filter_.value_or(L"");
        auto account = PromptManualAccount(window_, preferences_.theme, initialTag, tags);
        if (!account || generation != lockGeneration_) {
            return;
        }
        const bool keepFilter = filter_ && MatchesTag(*account, TagFilter());
        auto view = keepFilter ? filter_ : std::nullopt;
        std::vector<Account> imported;
        imported.push_back(std::move(*account));
        ApplyImport(std::move(imported), std::move(view), L"Account saved in the local vault.");
    }

    void OpenVault(HWND owner = nullptr) {
        if (!sessionAvailable_ || !sessionNotifications_) {
            return;
        }
        owner = owner ? owner : window_;
        const auto generation = lockGeneration_;
        const auto path = BackupPath(owner, false);
        if (path.empty() || generation != lockGeneration_) {
            return;
        }
        auto bytes = ReadBackup(path);
        if (!bytes) {
            SetNote(L"Could not read this backup.");
            return;
        }
        auto password = PromptText(owner, L"Open encrypted backup", L"Enter the backup password.",
                                   kMaxPasswordLength, true, L"", preferences_.theme);
        if (!password || generation != lockGeneration_) {
            return;
        }
        auto accounts = LoadBackup(*bytes, TextView(*password));
        password.reset();
        if (!accounts) {
            SetNote(L"Wrong password, unsupported format, or damaged backup.");
            return;
        }
        ApplyImport(std::move(*accounts), std::nullopt, L"Backup restored to the local vault.");
    }

    bool UnlockVault() {
        if (localVault_.Unlocked()) {
            return true;
        }
        if (!localVaultExists_ || !sessionAvailable_ || !sessionNotifications_) {
            return false;
        }
        const auto generation = lockGeneration_;
        const bool windows = LocalVault::IsWindowsProtected(vaultPath_);
        std::optional<DialogText> password;
        if (!windows) {
            password = PromptText(window_, L"Unlock local vault", L"Enter your local vault master password.",
                                  kMaxPasswordLength, true, L"", preferences_.theme);
            if (!password || generation != lockGeneration_) {
                return false;
            }
        }
        auto stored = localVault_.Open(vaultPath_, password ? TextView(*password) : std::wstring_view{});
        if (!stored) {
            SetNote(windows ? L"Could not open this vault under the current Windows account." :
                              L"Could not open the local vault. Check the password and the vault file.");
            return false;
        }
        accounts_ = std::move(*stored);
        lastActivity_ = GetTickCount64();
        UpdateCaptureProtection();
        RefreshTags();
        SetNote(L"Local vault unlocked.");
        return true;
    }

    bool CreateLocalVault(std::span<const Account> accounts) {
        if (vaultPath_.empty()) {
            SetNote(L"Could not find local storage. No accounts were imported.");
            return false;
        }
        const auto generation = lockGeneration_;
        const auto choice = MessageBoxW(window_,
            Ui(L"Yes: create a master password.\r\nNo: use Windows protection without a separate password.\r\n\r\n"
               L"Apps running as your Windows user can also unlock a Windows-protected vault."),
            Ui(L"Protect the local vault"), MB_YESNOCANCEL | MB_ICONQUESTION);
        if ((choice != IDYES && choice != IDNO) || generation != lockGeneration_) {
            return false;
        }
        std::optional<DialogText> password;
        if (choice == IDYES) {
            password = PromptText(window_, L"Create local vault", L"Choose a master password of at least 6 characters.",
                                  kMaxPasswordLength, true, L"", preferences_.theme);
            if (!password || generation != lockGeneration_) {
                return false;
            }
            if (password->size < kMinLocalPasswordLength) {
                SetNote(L"The master password must be at least 6 characters. No accounts were imported.");
                return false;
            }
            const auto confirmation = PromptText(window_, L"Confirm master password", L"Enter the same master password again.",
                                                 kMaxPasswordLength, true, L"", preferences_.theme);
            if (!confirmation || generation != lockGeneration_) {
                return false;
            }
            if (TextView(*password) != TextView(*confirmation)) {
                SetNote(L"Passwords did not match. No accounts were imported.");
                return false;
            }
        }
        if (!localVault_.Create(vaultPath_, accounts, password ? TextView(*password) : std::wstring_view{})) {
            SetNote(L"Could not create the local vault. No accounts were imported.");
            return false;
        }
        localVaultExists_ = true;
        return true;
    }

    bool SaveAccounts(std::vector<Account> proposed) {
        if (!localVault_.Save(proposed)) {
            SetNote(L"Could not save the local vault. Accounts were left unchanged.");
            return false;
        }
        accounts_ = std::move(proposed);
        return true;
    }

    void ApplyImport(std::vector<Account> imported,
                     std::optional<std::wstring> view = std::nullopt, PCWSTR success = nullptr) {
        if (!sessionAvailable_ || !sessionNotifications_ || (localVaultExists_ && !UnlockVault())) {
            return;
        }
        auto proposed = accounts_;
        const auto previous = proposed.size();
        const auto added = MergeAccounts(proposed, std::move(imported));
        if (!added) {
            SetNote(L"This session can hold up to 256 accounts.");
            return;
        }
        if (*added == 0) {
            SetNote(L"These accounts are already open.");
            return;
        }
        ApplyTagColors(proposed, CurrentTagColors(proposed));
        if (localVaultExists_ ? !localVault_.Save(proposed) : !CreateLocalVault(proposed)) {
            if (localVaultExists_) {
                SetNote(L"Could not save the local vault. No accounts were imported.");
            }
            return;
        }
        accounts_ = std::move(proposed);
        UpdateCaptureProtection();
        filter_ = std::move(view);
        selected_ = previous;
        lastActivity_ = GetTickCount64();
        RefreshTags();
        HighlightChangedRow();
        SetNote(success ? success : L"Accounts imported into the local vault.");
    }

    void ExportVault(HWND owner = nullptr) {
        if (accounts_.empty() || !sessionAvailable_) {
            return;
        }
        owner = owner ? owner : window_;
        const auto generation = lockGeneration_;
        const auto path = BackupPath(owner, true);
        if (path.empty() || generation != lockGeneration_) {
            return;
        }
        auto password = PromptText(owner, L"Protect backup",
                                   L"Choose a backup password with at least 12 characters.",
                                   kMaxPasswordLength, true, L"", preferences_.theme);
        if (!password || generation != lockGeneration_) {
            return;
        }
        if (password->size < kMinExportPasswordLength) {
            SetNote(L"The backup password must be at least 12 characters.");
            return;
        }
        auto check = PromptText(owner, L"Confirm backup password", L"Enter the same password again.",
                                kMaxPasswordLength, true, L"", preferences_.theme);
        if (!check || generation != lockGeneration_) {
            return;
        }
        if (TextView(*password) != TextView(*check)) {
            SetNote(L"Passwords did not match.");
            return;
        }
        auto backup = CreateBackup(accounts_, TextView(*password));
        password.reset();
        check.reset();
        if (!backup || !WriteNewBackup(path, *backup)) {
            SetNote(L"Could not save the backup. Choose a new, writable file.");
            return;
        }
        lastActivity_ = GetTickCount64();
        SetNote(L"Encrypted backup saved.");
    }

    void ExportPortable(ExportFormat format) {
        if (accounts_.empty() || !sessionAvailable_) {
            return;
        }
        const auto generation = lockGeneration_;
        if (!ConfirmPlaintextExport(window_, preferences_.theme) || generation != lockGeneration_) {
            return;
        }
        const auto path = BackupPath(window_, true, format);
        if (path.empty() || generation != lockGeneration_) {
            return;
        }
        auto exportData = CreatePortableExport(accounts_, format);
        if (!exportData || !WriteNewPortableExport(path, exportData->bytes)) {
            SetNote(L"Could not save the export. Check for an incomplete unencrypted file before retrying.");
            return;
        }
        exportData.reset();
        lastActivity_ = GetTickCount64();
        SetNote(L"Unencrypted export saved. Anyone with this file can generate your codes.");
    }

    void EditAccountName() {
        const auto account = SelectedAccount();
        if (!account) {
            return;
        }
        const auto generation = lockGeneration_;
        const auto currentName = std::wstring(AccountDisplayName(account->issuer, account->label));
        const auto input = PromptText(window_, L"Rename account", L"Enter an account name.",
                                      kMaxNameLength, false, currentName.c_str(), preferences_.theme);
        if (!input || generation != lockGeneration_) {
            return;
        }
        const auto name = AccountDisplayName(account->issuer, TrimName(TextView(*input)));
        if (name.empty() || !ValidName(name)) {
            SetNote(L"Enter a valid name without control characters.");
            return;
        }
        if (currentName == name) {
            return;
        }
        if (std::ranges::any_of(accounts_, [&](const auto& other) {
                return &other != account && SameName(other.issuer, account->issuer) &&
                    SameName(AccountDisplayName(other.issuer, other.label), name);
            })) {
            SetNote(L"An account with this provider and name already exists.");
            return;
        }
        auto proposed = accounts_;
        proposed[*selected_].label = name;
        if (!SaveAccounts(std::move(proposed))) {
            return;
        }
        RefreshList();
        SetNote(L"Account updated in the local vault.");
    }

    static TagColors CurrentTagColors(std::span<const Account> accounts) {
        TagColors colors;
        for (const auto& account : accounts) {
            for (std::size_t i = 0; i < account.tags.size(); ++i) {
                if (account.tagColors[i] && !FindTagColor(colors, account.tags[i])) {
                    colors.emplace_back(account.tags[i], account.tagColors[i]);
                }
            }
        }
        return colors;
    }

    static void ApplyTagColors(std::span<Account> accounts, const TagColors& colors) {
        for (auto& account : accounts) {
            account.tagColors = {};
            for (std::size_t i = 0; i < account.tags.size(); ++i) {
                account.tagColors[i] = FindTagColor(colors, account.tags[i]);
            }
        }
    }

    void AssignTags() {
        const auto account = SelectedAccount();
        if (!account) {
            return;
        }
        const auto generation = lockGeneration_;
        const auto previousColors = CurrentTagColors(accounts_);
        auto colors = previousColors;
        auto tags = PromptAccountTags(window_, preferences_.theme, account->tags, TagChoices(), &colors);
        if (!tags || generation != lockGeneration_ || (*tags == account->tags && previousColors == colors)) {
            return;
        }
        auto proposed = accounts_;
        proposed[*selected_].tags = std::move(*tags);
        ApplyTagColors(proposed, colors);
        if (!SaveAccounts(std::move(proposed))) {
            return;
        }
        if (!MatchesTag(accounts_[*selected_], TagFilter())) {
            filter_.reset();
        }
        RefreshTags();
        HighlightChangedRow();
        SetNote(L"Tags updated in the local vault.");
    }

    void RenameTag(std::wstring current) {
        const auto generation = lockGeneration_;
        const auto input = PromptText(window_, L"Rename or remove tag",
            L"Enter a new or existing tag name. Clear the name to remove this tag from all accounts.",
            kMaxNameLength, false, current.c_str(), preferences_.theme);
        if (!input || generation != lockGeneration_) {
            return;
        }
        const auto name = TrimName(TextView(*input));
        if (!ValidName(name)) {
            SetNote(L"Enter a tag name without control characters.");
            return;
        }
        if (name == current) {
            return;
        }
        auto colors = CurrentTagColors(accounts_);
        if (!name.empty() && !FindTagColor(colors, name)) {
            if (const auto color = FindTagColor(colors, current)) {
                colors.emplace_back(name, color);
            }
        }
        auto proposed = accounts_;
        for (auto& account : proposed) {
            const auto tag = std::ranges::find_if(account.tags, [&](const auto& value) {
                return SameName(value, current);
            });
            if (tag == account.tags.end()) {
                continue;
            }
            if (name.empty() || (!SameName(name, current) && HasTag(account, name))) {
                account.tags.erase(tag);
            } else {
                *tag = name;
            }
        }
        ApplyTagColors(proposed, colors);
        if (!SaveAccounts(std::move(proposed))) {
            return;
        }
        filter_ = name.empty() ? std::nullopt : std::optional<std::wstring>(name);
        RefreshTags();
        SetNote(L"Tags updated in the local vault.");
    }

    void CancelDialogs() const {
        EndMenu();
        EnumThreadWindows(GetCurrentThreadId(), CloseOwnedDialog, reinterpret_cast<LPARAM>(window_));
    }

    static BOOL CALLBACK CloseOwnedDialog(HWND dialog, LPARAM owner) {
        const auto root = reinterpret_cast<HWND>(owner);
        if (dialog != root && IsWindowVisible(dialog) && GetAncestor(dialog, GA_ROOTOWNER) == root) {
            PostMessageW(dialog, WM_CLOSE, 0, 0);
        }
        return TRUE;
    }

    void LockSession() {
        ++lockGeneration_;
        HideFlyout();
        CancelDialogs();
        clipboard_.Clear(window_, true);
        localVault_.Lock();
        accounts_.clear();
        selected_.reset();
        filter_.reset();
        RefreshTags();
    }

    void CheckSession() {
        clipboard_.Clear(window_);
        if (stopEvent_ && WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0) {
            LockSession();
            return;
        }
        if (!trayIconAdded_) {
            AddIcon();
        }
        if (IsWindowVisible(window_)) {
            RefreshTextSize();
            // UISettings color-change events are unavailable to desktop callers.
            const auto palette = WindowsPalette(preferences_.theme, systemTextSize_);
            if (palette != palette_) {
                ApplyPalette(palette);
            }
        }
        if (modPreferences_) {
            const auto settings = modPreferences_->load();
            if (settings != preferences_) {
                ApplyPreferences(settings);
            }
        }
        if (localVault_.Unlocked() && GetTickCount64() - lastActivity_ >=
            static_cast<ULONGLONG>(preferences_.lockMinutes) * 60000) {
            LockSession();
        }
    }

    static std::int64_t UnixSeconds() {
        using namespace std::chrono;
        return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
    }

    void CopyAccountCode(std::size_t index) {
        using namespace std::chrono;
        const auto timestamp = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
        const auto now = timestamp / 1000;
        if (index >= accounts_.size() || timestamp < 0) {
            return;
        }
        const auto code = CalculateAccountCode(accounts_[index], now);
        const auto lifetime = code ? ClipboardLifetime(code->secondsRemaining, timestamp % 1000,
                                                       preferences_.clipboardSeconds) : 0;
        const bool copied = code && clipboard_.Copy(window_, *code, lifetime);
        if (copied) {
            SetTimer(window_, kClipboardTimer, lifetime, nullptr);
            auto& account = accounts_[index];
            if (account.useCount != UINT32_MAX) {
                ++account.useCount;
            }
            account.lastUsed = timestamp;
            localVault_.Save(accounts_);
            if (sort_ == AccountSort::MostUsed || sort_ == AccountSort::RecentlyUsed) {
                RefreshList();
            }
            ShowCopyFeedback();
            if (preferences_.hideAfterCopy) {
                SetTimer(window_, kHideAfterCopyTimer, kHideAfterCopyMs, nullptr);
            }
        } else {
            SetNote(L"Could not copy the code.");
        }
        lastActivity_ = GetTickCount64();
    }

    void UpdateCountdown() {
        const auto now = UnixSeconds();
        if (now == displayedSecond_) {
            return;
        }
        const auto previousSecond = std::exchange(displayedSecond_, now);
        const auto tag = TagFilter();
        const auto next = now >= 0 ? NextRefresh(accounts_, tag, now) : std::nullopt;
        wchar_t label[32]{};
        const unsigned left = next ? next->seconds : 0;
        const unsigned total = next ? next->period : 0;
        if (progressLeft_ != left || progressTotal_ != total) {
            progressLeft_ = left;
            progressTotal_ = total;
            SendMessageW(progress_, PBM_SETPOS, total ? MulDiv(left, 1000, total) : 0, 0);
            InvalidateRect(progress_, nullptr, FALSE);
        }
        if (next && preferences_.showCountdown) {
            swprintf_s(label, Ui(L"%u s"), next->seconds);
        }
        if (preferences_.showCountdown) {
            SetWindowTextW(countdown_, label);
        }
        if (preferences_.showCodes && (displayProtected_ || preferences_.allowScreenshots)) {
            if (previousSecond < 0 || now < previousSecond) {
                InvalidateRect(accountList_, nullptr, FALSE);
                return;
            }
            const auto first = SendMessageW(accountList_, LB_GETTOPINDEX, 0, 0);
            RECT client{};
            GetClientRect(accountList_, &client);
            const auto afterLast = (std::min<LRESULT>)(accountCount_, first + client.bottom / RowHeight() + 1);
            for (auto row = first; row >= 0 && row < afterLast; ++row) {
                const auto index = SendMessageW(accountList_, LB_GETITEMDATA, row, 0);
                if (index >= 0 && static_cast<std::size_t>(index) < accounts_.size()) {
                    const auto period = accounts_[index].period;
                    if (period && now / period != previousSecond / period) {
                        InvalidateRow(static_cast<UINT>(row));
                    }
                }
            }
        }
    }

    bool standalone_;
    void (*checkUpdates_)(HWND);
    const std::atomic<FlyoutPreferences>* modPreferences_;
    FlyoutPreferences preferences_;
    HotkeyPreferences shortcut_;
    std::array<ATOM, 2> hotkeyIds_{};
    ATOM activeHotkey_ = 0;
    HANDLE stopEvent_ = nullptr;
    UINT taskbarCreated_ = 0;
    HWND window_ = nullptr;
    HWND title_ = nullptr;
    HWND settings_ = nullptr;
    HWND edit_ = nullptr;
    HWND close_ = nullptr;
    HWND tagFilter_ = nullptr;
    HWND sortButton_ = nullptr;
    HWND hotButton_ = nullptr;
    struct ButtonFeedback {
        HWND window = nullptr;
        InteractionFade fade;
        bool animating = false;
    };
    std::array<ButtonFeedback, 6> buttonFeedback_{};
    std::optional<UINT> changedRow_;
    InteractionFade changedRowFade_;
    HWND accountList_ = nullptr;
    HWND accountScroll_ = nullptr;
    HWND emptyTitle_ = nullptr;
    HWND emptyHint_ = nullptr;
    HWND importButton_ = nullptr;
    HWND timerLabel_ = nullptr;
    HWND countdown_ = nullptr;
    HWND progress_ = nullptr;
    HWND note_ = nullptr;
    HFONT labelFont_ = nullptr;
    HFONT smallFont_ = nullptr;
    HFONT titleFont_ = nullptr;
    HFONT codeFont_ = nullptr;
    HFONT iconFont_ = nullptr;
    HFONT strongFont_ = nullptr;
    HFONT badgeFont_ = nullptr;
    RECT emptyPanel_{};
    RECT footerSeparator_{};
    WindowsTextSize systemTextSize_;
    UINT dpi_ = 96;
    UINT fontDpi_ = 0;
    double textScale_ = 1.0;
    double fontTextScale_ = 0.0;
    int accountCount_ = 0;
    int wheelDelta_ = 0;
    AccountSort sort_ = AccountSort::NameAscending;
    bool editing_ = false;
    Palette palette_ = WindowsPalette(preferences_.theme, systemTextSize_);
    bool trayIconAdded_ = false;
    bool previewVisible_ = false;
    bool displayProtected_ = false;
    unsigned progressLeft_ = 0;
    unsigned progressTotal_ = 0;
    HBRUSH frameBrush_ = nullptr;
    HBRUSH cardBrush_ = nullptr;
    RECT anchor_{};
    RECT restingBounds_{};
    POINT motionOffset_{};
    Motion motion_ = Motion::Idle;
    std::chrono::steady_clock::time_point motionStart_{};
    double exitStartOffset_ = 0.0;
    bool positioning_ = false;
    bool movingWindow_ = false;
    RECT moveStart_{};
    WindowPlacementPreferences placement_;
    std::optional<ManualWindowPosition> manualPosition_;
    std::int64_t displayedSecond_ = -1;
    std::vector<Account> accounts_;
    std::optional<std::wstring> filter_;
    std::optional<std::size_t> selected_;
    std::optional<std::size_t> copyPressed_;
    std::optional<RowDrag> drag_;
    std::optional<UINT> hoverRow_;
    std::optional<UINT> copyPulseRow_;
    ULONGLONG copyPulseStart_ = 0;
    int copyPulseAmount_ = 0;
    std::wstring noteText_;
    CodeClipboard clipboard_;
    LocalVault localVault_;
    std::wstring vaultPath_;
    bool localVaultExists_ = false;
    bool handlingCommand_ = false;
    const ThemedMenu* activeMenu_ = nullptr;
    bool sessionNotifications_ = false;
    bool sessionAvailable_ = true;
    unsigned lockGeneration_ = 0;
    ULONGLONG lastActivity_ = 0;
};

inline int RunTray(bool standalone, HANDLE stopEvent = nullptr, bool showOnLaunch = false,
                   const std::atomic<FlyoutPreferences>* modPreferences = nullptr,
                   std::optional<std::wstring> vaultPath = std::nullopt, void (*checkUpdates)(HWND) = nullptr) {
    return TrayFlyout(standalone, modPreferences, vaultPath ? std::move(*vaultPath) : LocalVaultPath(standalone),
                      checkUpdates)
        .Run(stopEvent, showOnLaunch);
}

}  // namespace twofa
