#pragma once

#include "backup.hpp"
#include "appearance.hpp"

#include <commdlg.h>
#include <commctrl.h>
#include <cstddef>
#include <functional>
#include <new>

namespace twofa {

using DialogText = SensitiveBuffer<wchar_t, kMaxUriLength + 1>;

inline double DialogTextScale(HWND owner, int width, int height) {
    const WindowsTextSize settings;
    MONITORINFO monitor{sizeof(monitor), {}, {}, 0};
    if (!GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor)) {
        return settings.Factor();
    }
    const auto dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
    const auto units = GetDialogBaseUnits();
    const auto systemDpi = GetDpiForSystem();
    const auto w = MulDiv(MulDiv(width, LOWORD(units), 4), dpi, systemDpi);
    const auto h = MulDiv(MulDiv(height, HIWORD(units), 8), dpi, systemDpi);
    const auto fit = (std::min)(static_cast<double>(monitor.rcWork.right - monitor.rcWork.left - 48) / w,
                                static_cast<double>(monitor.rcWork.bottom - monitor.rcWork.top - 72) / h);
    return (std::min)(settings.Factor(), (std::max)(1.0, fit));
}

inline int ScaleDialogUnits(int units, double scale) {
    return static_cast<int>(std::lround(units * scale));
}

struct alignas(DWORD) ModalTemplate {
    DLGTEMPLATE bounds{};
    WORD menu = 0;
    WORD windowClass = 0;
    WORD title = 0;

    ModalTemplate(short width, short height, double scale) {
        bounds.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER;
        bounds.cx = static_cast<short>(ScaleDialogUnits(width, scale));
        bounds.cy = static_cast<short>(ScaleDialogUnits(height, scale));
    }
};
static_assert(offsetof(ModalTemplate, menu) == 18);

enum class DialogControl : UINT_PTR { Button = 1, CheckBox, ComboBox, Hotkey };
inline constexpr PCWSTR kThemeNames[]{L"Follow Windows", L"Light", L"Dark"};
inline constexpr PCWSTR kLockLabels[]{L"1 minute", L"3 minutes", L"5 minutes", L"10 minutes"};
inline constexpr PCWSTR kClipboardLabels[]{L"5 seconds", L"15 seconds", L"30 seconds"};
inline constexpr int kThemeChoice = 101;
inline constexpr int kShowCodesChoice = 102;
inline constexpr int kSettingsHelp = 103;
inline constexpr int kCountdownChoice = 104;
inline constexpr int kMotionChoice = 105;
inline constexpr int kLockChoice = 106;
inline constexpr int kClipboardChoice = 107;
inline constexpr int kScreenshotChoice = 108;
inline constexpr int kHideAfterCopyChoice = 109;
inline constexpr int kRussianChoice = 110;
inline constexpr int kPlacementChoice = 114;
inline constexpr int kLockDragChoice = 115;
inline constexpr int kRememberPositionChoice = 116;

enum class WindowPlacementMode : unsigned char { Manual, TopLeft, TopRight, BottomLeft, BottomRight };

struct WindowPlacementPreferences {
    WindowPlacementMode mode = WindowPlacementMode::Manual;
    bool lockDrag = false;
    bool remember = false;

    bool operator==(const WindowPlacementPreferences&) const = default;
};

inline constexpr PCWSTR kPlacementNames[]{L"Manual", L"Top left", L"Top right", L"Bottom left", L"Bottom right"};

inline std::span<const PCWSTR> PreferenceLabels(int id) {
    switch (id) {
        case kThemeChoice: return kThemeNames;
        case kLockChoice: return kLockLabels;
        case kClipboardChoice: return kClipboardLabels;
        case kPlacementChoice: return kPlacementNames;
    }
    return {};
}

inline COLORREF AccentText(COLORREF accent) {
    return Luminance(accent) >= 0.179 ? RGB(0, 0, 0) : RGB(255, 255, 255);
}

inline std::wstring HotkeyLabel(WORD chord) {
    std::wstring label;
    const auto append = [&](PCWSTR key) {
        if (!label.empty()) {
            label += L" + ";
        }
        label += key;
    };
    const auto modifiers = HIBYTE(chord);
    if (modifiers & HOTKEYF_CONTROL) {
        append(L"Ctrl");
    }
    if (modifiers & HOTKEYF_ALT) {
        append(L"Alt");
    }
    if (modifiers & HOTKEYF_SHIFT) {
        append(L"Shift");
    }
    if (const auto key = LOBYTE(chord)) {
        const auto scan = MapVirtualKeyW(key, MAPVK_VK_TO_VSC_EX);
        const auto info = static_cast<LONG>((scan & 0xff) << 16) |
                          ((scan >> 8) || (modifiers & HOTKEYF_EXT) ? 1L << 24 : 0);
        wchar_t name[32]{};
        if (GetKeyNameTextW(info, name, ARRAYSIZE(name))) {
            append(name);
        }
    }
    return label.empty() ? L"Press a shortcut" : label;
}

inline void PaintCheckBox(HDC dc, RECT box, int checked, bool enabled, COLORREF unchecked,
                          const Palette& palette) {
    const auto accent = enabled ? palette.accent : palette.disabledText;
    const auto fill = BlendColor(unchecked, accent, checked);
    const auto outline = BlendColor(BlendColor(palette.stroke, palette.muted, 60), accent, checked);
    const auto width = box.right - box.left;
    const auto height = box.bottom - box.top;
    PaintRounded(dc, box, (std::max)(1L, width / 5), fill, outline);
    if (checked > 0) {
        const auto ink = BlendColor(fill, AccentText(accent), checked);
        DrawLine(dc, {box.left + width * 3 / 16, box.top + height / 2},
                 {box.left + width * 7 / 16, box.top + height * 12 / 16}, ink);
        DrawLine(dc, {box.left + width * 7 / 16, box.top + height * 12 / 16},
                 {box.left + width * 13 / 16, box.top + height * 5 / 16}, ink);
    }
}

inline void PaintDialogControl(HWND control, DialogControl kind, const Palette& palette, HDC dc,
                               int hover, int checked) {
    RECT rect{};
    GetClientRect(control, &rect);
    FillSurface(dc, rect, palette.surface);
    const auto dpi = GetDpiForWindow(control);
    const auto units = [dpi](int size) { return MulDiv(size, dpi, 96); };
    const auto font = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
    const bool enabled = IsWindowEnabled(control);
    const bool focused = GetFocus() == control && !(SendMessageW(control, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
    constexpr UINT line = DT_SINGLELINE | DT_VCENTER;
    if (kind == DialogControl::Hotkey) {
        PaintRounded(dc, rect, units(4), palette.card, palette.stroke);
        if (focused) {
            PaintFocus(dc, rect, units(1), palette.focusOuter, palette.focusInner);
        }
        const auto chord = static_cast<WORD>(SendMessageW(control, HKM_GETHOTKEY, 0, 0));
        rect.left += units(10);
        rect.right -= units(10);
        PaintText(dc, HotkeyLabel(chord), rect, font, enabled ? palette.text : palette.disabledText,
                  line | DT_END_ELLIPSIS);
        return;
    }
    const auto state = kind != DialogControl::ComboBox ? SendMessageW(control, BM_GETSTATE, 0, 0) : 0;
    const bool pressed = state & BST_PUSHED;
    hover = enabled ? hover : 0;
    const int centerY = (rect.top + rect.bottom) / 2;

    if (kind == DialogControl::CheckBox) {
        const auto fill = pressed && enabled ? palette.pressed : BlendColor(palette.card, palette.hover, hover);
        const RECT box{0, centerY - units(8), units(16), centerY + units(8)};
        PaintCheckBox(dc, box, checked, enabled, fill, palette);
        if (focused) {
            PaintFocus(dc, rect, units(1), palette.focusOuter, palette.focusInner);
        }
        rect.left += units(24);
        wchar_t caption[64]{};
        GetWindowTextW(control, caption, ARRAYSIZE(caption));
        PaintText(dc, caption, rect, font, enabled ? palette.text : palette.disabledText, line |
                  ((SendMessageW(control, WM_QUERYUISTATE, 0, 0) & UISF_HIDEACCEL) ? DT_HIDEPREFIX : 0), true);
        return;
    }

    const auto defaultButton = SendMessageW(GetParent(control), DM_GETDEFID, 0, 0);
    const bool primary = kind == DialogControl::Button && GetDlgCtrlID(control) == IDOK &&
                         LOWORD(defaultButton) == IDOK;
    const auto base = primary ? palette.accent : kind == DialogControl::ComboBox ? palette.card : palette.button;
    const auto fill = !enabled ? palette.card : primary ? BlendColor(base, AccentText(base), pressed ? 12 : 6 * hover / 100) :
                      pressed ? palette.pressed : BlendColor(base, palette.hover, hover);
    const auto stroke = primary && enabled ? fill : palette.stroke;
    const auto foreground = !enabled ? palette.disabledText : primary ? AccentText(fill) :
                            kind == DialogControl::ComboBox ? palette.text : palette.buttonText;
    PaintRounded(dc, rect, units(4), fill, stroke);
    if (focused) {
        PaintFocus(dc, rect, units(1), palette.focusOuter, palette.focusInner);
    }

    if (kind == DialogControl::ComboBox) {
        const auto chosen = SendMessageW(control, CB_GETCURSEL, 0, 0);
        wchar_t caption[kMaxNameLength + 1]{};
        if (chosen >= 0) {
            const auto length = SendMessageW(control, CB_GETLBTEXTLEN, chosen, 0);
            if (length >= 0 && length < static_cast<LRESULT>(ARRAYSIZE(caption))) {
                SendMessageW(control, CB_GETLBTEXT, chosen, reinterpret_cast<LPARAM>(caption));
            }
        }
        const auto x = rect.right - units(14);
        DrawLine(dc, {x - units(4), centerY - units(2)}, {x, centerY + units(2)}, foreground);
        DrawLine(dc, {x, centerY + units(2)}, {x + units(4) + 1, centerY - units(2) - 1}, foreground);
        rect.left += units(10);
        rect.right -= units(28);
        PaintText(dc, caption, rect, font, foreground, line | DT_END_ELLIPSIS);
        return;
    }
    wchar_t caption[64]{};
    GetWindowTextW(control, caption, ARRAYSIZE(caption));
    PaintText(dc, caption, rect, font, foreground, line | DT_CENTER |
              ((SendMessageW(control, WM_QUERYUISTATE, 0, 0) & UISF_HIDEACCEL) ? DT_HIDEPREFIX : 0), true);
}

inline constexpr wchar_t kDialogMotionProperty[] = L"2FA Flyout dialog control animation";

struct DialogControlMotion {
    const Palette& palette;
    InteractionFade hover;
    InteractionFade check;
    bool hot = false;
};

inline LRESULT CALLBACK StyledDialogControl(HWND control, UINT message, WPARAM wparam, LPARAM lparam,
                                             UINT_PTR id, DWORD_PTR context) {
    constexpr UINT_PTR timer = 1;
    const auto motion = reinterpret_cast<DialogControlMotion*>(context);
    const auto kind = static_cast<DialogControl>(id);
    if (message == WM_NCDESTROY) {
        KillTimer(control, timer);
        RemovePropW(control, kDialogMotionProperty);
        RemoveWindowSubclass(control, StyledDialogControl, id);
        delete motion;
        return DefSubclassProc(control, message, wparam, lparam);
    }
    const auto& palette = motion->palette;
    if (message == WM_TIMER && wparam == timer) {
        const auto now = GetTickCount64();
        if (palette.highContrast || (!motion->hover.Running(now) && !motion->check.Running(now))) {
            KillTimer(control, timer);
        }
        InvalidateRect(control, nullptr, FALSE);
        return 0;
    }
    if (!palette.highContrast) {
        if (message == WM_ERASEBKGND) {
            return 1;
        }
        if (message == WM_PAINT || message == WM_PRINTCLIENT ||
            (kind == DialogControl::Hotkey && message == WM_PRINT && (lparam & PRF_CLIENT))) {
            PAINTSTRUCT paint{};
            const auto dc = message == WM_PAINT ? BeginPaint(control, &paint) : reinterpret_cast<HDC>(wparam);
            RECT bounds{};
            GetClientRect(control, &bounds);
            {
                const BufferedPaint buffer(dc, bounds);
                const auto now = GetTickCount64();
                PaintDialogControl(control, kind, palette, buffer.Dc(), motion->hover.Value(now),
                                   motion->check.Value(now));
            }
            if (message == WM_PAINT) {
                EndPaint(control, &paint);
            }
            return 0;
        }
    }
    const auto result = DefSubclassProc(control, message, wparam, lparam);
    if (GetPropW(control, kDialogMotionProperty) != motion) {
        return result;
    }
    switch (message) {
        case WM_MOUSEMOVE:
            if (!motion->hot) {
                TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, control, 0};
                TrackMouseEvent(&tracking);
                motion->hot = true;
            }
            [[fallthrough]];
        case WM_MOUSELEAVE:
        case WM_CANCELMODE:
        case WM_LBUTTONUP:
        case BM_SETSTATE:
        case BM_SETCHECK:
        case CB_SETCURSEL:
        case CB_SHOWDROPDOWN:
        case HKM_SETHOTKEY:
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
        case WM_ENABLE:
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case WM_UPDATEUISTATE: {
            if (message == WM_MOUSELEAVE || message == WM_CANCELMODE) {
                motion->hot = false;
            }
            const bool animated = !palette.highContrast && UseClientAnimations();
            const auto now = GetTickCount64();
            motion->hover.Set(motion->hot && IsWindowEnabled(control) ? 100 : 0, now, animated);
            if (kind == DialogControl::CheckBox) {
                const bool checked = SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED;
                motion->check.Set(checked ? 100 : 0, now, animated);
            }
            if (motion->hover.Running(now) || motion->check.Running(now)) {
                SetTimer(control, timer, USER_TIMER_MINIMUM, nullptr);
            }
            InvalidateRect(control, nullptr, FALSE);
            break;
        }
    }
    return result;
}

inline void StyleDialogControl(HWND control, DialogControl kind, const Palette& palette) {
    const auto motion = new (std::nothrow) DialogControlMotion{palette, {}, {}};
    if (!motion) {
        return;
    }
    if (kind == DialogControl::CheckBox && SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        motion->check.Set(100, GetTickCount64(), false);
    }
    if (!SetPropW(control, kDialogMotionProperty, motion)) {
        delete motion;
    } else if (!SetWindowSubclass(control, StyledDialogControl, static_cast<UINT_PTR>(kind),
                                  reinterpret_cast<DWORD_PTR>(motion))) {
        RemovePropW(control, kDialogMotionProperty);
        delete motion;
    }
}

struct DialogAppearance {
    DialogAppearance(ThemeMode mode) : theme(mode), palette(WindowsPalette(mode)) {}
    DialogAppearance(const DialogAppearance&) = delete;
    DialogAppearance& operator=(const DialogAppearance&) = delete;

    ~DialogAppearance() {
        DeleteObject(font);
        DeleteObject(surface);
        DeleteObject(field);
    }

    void Initialize(HWND dialog, double textScale) {
        font = CreateUiFont(MulDiv(ScaleDialogUnits(14, textScale), GetDpiForWindow(dialog), 96));
        UpdateColors(dialog);
    }

    bool HandleColorChange(HWND dialog, UINT message) {
        if (message != WM_THEMECHANGED && message != WM_SYSCOLORCHANGE &&
            message != WM_DWMCOLORIZATIONCOLORCHANGED && message != WM_SETTINGCHANGE) {
            return false;
        }
        const auto colors = WindowsPalette(theme);
        if (palette != colors) {
            palette = colors;
            UpdateColors(dialog);
            RedrawWindow(dialog, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
        }
        return true;
    }

    INT_PTR ColorControl(UINT message, WPARAM wparam, LPARAM lparam, bool muted = false) const {
        const auto dc = reinterpret_cast<HDC>(wparam);
        const bool editable = message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX;
        const auto control = reinterpret_cast<HWND>(lparam);
        SetBkColor(dc, editable ? palette.card : palette.surface);
        SetTextColor(dc, !IsWindowEnabled(control) ? palette.disabledText : muted ? palette.muted : palette.text);
        return reinterpret_cast<INT_PTR>(editable ? field : surface);
    }

    ThemeMode theme;
    Palette palette;
    HFONT font = nullptr;

private:
    void UpdateColors(HWND dialog) {
        if (auto brush = CreateSolidBrush(palette.surface)) {
            DeleteObject(std::exchange(surface, brush));
        }
        if (auto brush = CreateSolidBrush(palette.card)) {
            DeleteObject(std::exchange(field, brush));
        }
        ApplyWindowFrame(dialog, palette);
    }

    HBRUSH surface = nullptr;
    HBRUSH field = nullptr;
};

struct TextPrompt : DialogAppearance {
    PCWSTR title;
    PCWSTR label;
    std::size_t limit;
    bool masked;
    PCWSTR initial = L"";
    DialogText result;
    HWND edit = nullptr;
    double textScale = 1.0;
    PCWSTR confirmAction = nullptr;
};

inline HWND AddDialogControl(HWND dialog, PCWSTR kind, PCWSTR label, DWORD style, int id,
                             RECT bounds, HFONT font, double factor, bool localize = true) {
    bounds = {ScaleDialogUnits(bounds.left, factor), ScaleDialogUnits(bounds.top, factor),
              ScaleDialogUnits(bounds.right, factor), ScaleDialogUnits(bounds.bottom, factor)};
    MapDialogRect(dialog, &bounds);
    const auto control = CreateWindowExW(0, kind, localize && wcscmp(kind, L"EDIT") != 0 ? Ui(label) : label,
                                         WS_CHILD | WS_VISIBLE | style,
                                         bounds.left, bounds.top, bounds.right - bounds.left,
                                         bounds.bottom - bounds.top, dialog,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                         GetModuleHandleW(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
    return control;
}

inline INT_PTR CALLBACK TextPromptProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_INITDIALOG) {
        const auto prompt = reinterpret_cast<TextPrompt*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(prompt));
        SetWindowTextW(dialog, Ui(prompt->title));
        if (prompt->masked) {
            SetWindowDisplayAffinity(dialog, WDA_EXCLUDEFROMCAPTURE);
        }
        prompt->Initialize(dialog, prompt->textScale);
        const auto add = [&](PCWSTR kind, PCWSTR text, DWORD style, int id, RECT bounds) {
            return AddDialogControl(dialog, kind, text, style, id, bounds, prompt->font, prompt->textScale);
        };
        add(L"STATIC", prompt->label, SS_NOPREFIX, -1, {10, 9, 278, 27});
        if (prompt->confirmAction) {
            AddDialogControl(dialog, L"STATIC", prompt->initial, SS_ENDELLIPSIS | SS_NOPREFIX, -1,
                             {10, 29, 278, 47}, prompt->font, prompt->textScale, false);
        } else {
            const DWORD flags = WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | (prompt->masked ? ES_PASSWORD : 0);
            prompt->edit = add(L"EDIT", prompt->initial, flags, 101, {10, 29, 278, 47});
        }
        const auto ok = add(L"BUTTON", prompt->confirmAction ? prompt->confirmAction : L"OK",
                            WS_TABSTOP | (prompt->confirmAction ? BS_PUSHBUTTON : BS_DEFPUSHBUTTON),
                            IDOK, {172, 54, 222, 73});
        const auto cancel = add(L"BUTTON", L"Cancel",
                                WS_TABSTOP | (prompt->confirmAction ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON),
                                IDCANCEL, {228, 54, 278, 73});
        if (!ok || !cancel || (!prompt->confirmAction && !prompt->edit)) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        StyleDialogControl(ok, DialogControl::Button, prompt->palette);
        StyleDialogControl(cancel, DialogControl::Button, prompt->palette);
        if (prompt->confirmAction) {
            SendMessageW(dialog, DM_SETDEFID, IDCANCEL, 0);
            SetFocus(cancel);
            return FALSE;
        }
        SendMessageW(prompt->edit, EM_SETLIMITTEXT, prompt->limit, 0);
        SendMessageW(prompt->edit, EM_SETSEL, 0, -1);
        SetFocus(prompt->edit);
        return FALSE;
    }
    const auto prompt = reinterpret_cast<TextPrompt*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (!prompt || prompt->HandleColorChange(dialog, message)) {
        return FALSE;
    }
    if (message == WM_COMMAND && LOWORD(wparam) == 101 && HIWORD(wparam) == EN_CHANGE) {
        if (prompt->masked && prompt->edit) {
            SendMessageW(prompt->edit, EM_EMPTYUNDOBUFFER, 0, 0);
        }
    }
    if (message == WM_CTLCOLORDLG || message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT ||
        message == WM_CTLCOLORLISTBOX) {
        return prompt->ColorControl(message, wparam, lparam);
    }
    if (message == WM_CLOSE ||
        (message == WM_COMMAND && (LOWORD(wparam) == IDOK || LOWORD(wparam) == IDCANCEL))) {
        const auto result = message == WM_CLOSE ? IDCANCEL : LOWORD(wparam);
        if (prompt->edit) {
            if (result == IDOK) {
                prompt->result.size = GetWindowTextW(prompt->edit, prompt->result.data.data(),
                                                      static_cast<int>(prompt->limit + 1));
            }
            if (prompt->masked) {
                SetWindowTextW(prompt->edit, L"");
                SendMessageW(prompt->edit, EM_EMPTYUNDOBUFFER, 0, 0);
            }
        }
        EndDialog(dialog, result);
        return TRUE;
    }
    return FALSE;
}

inline std::optional<DialogText> PromptText(HWND owner, PCWSTR title, PCWSTR label,
                                           std::size_t limit, bool masked, PCWSTR initial, ThemeMode theme) {
    const auto scale = DialogTextScale(owner, 288, 82);
    const ModalTemplate dialog(288, 82, scale);
    TextPrompt prompt{{theme}, title, label, limit, masked, initial, {}, nullptr, scale};
    const auto response = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &dialog.bounds,
                                                   owner, TextPromptProc,
                                                   reinterpret_cast<LPARAM>(&prompt));
    return response == IDOK ? std::optional(std::move(prompt.result)) : std::nullopt;
}

inline bool ConfirmAction(HWND owner, ThemeMode theme, PCWSTR title, PCWSTR label, PCWSTR detail,
                          PCWSTR action, bool sensitive = false) {
    const auto scale = DialogTextScale(owner, 288, 82);
    const ModalTemplate dialog(288, 82, scale);
    TextPrompt prompt{{theme}, title, label, 0, sensitive, detail, {}, nullptr, scale, action};
    return DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &dialog.bounds, owner, TextPromptProc,
                                    reinterpret_cast<LPARAM>(&prompt)) == IDOK;
}

inline bool ConfirmAccountRemoval(HWND owner, PCWSTR account, ThemeMode theme) {
    return ConfirmAction(owner, theme, L"Remove account?", L"Remove this account from the local vault?",
                         account, L"Remove", true);
}

inline bool ConfirmPlaintextExport(HWND owner, ThemeMode theme) {
    return ConfirmAction(owner, theme, L"Export unencrypted setup keys?",
                         L"This file will contain unencrypted setup keys.",
                         Ui(L"Anyone with the file can generate your codes."), L"Export");
}

enum TagEditorField : int {
    kTagList = 221, kNewTag, kTagStatus, kTagColor, kAutoTagColor
};

using TagColors = std::vector<std::pair<std::wstring, std::uint32_t>>;

inline std::uint32_t FindTagColor(const TagColors& colors, std::wstring_view name) {
    const auto match = std::ranges::find_if(colors, [&](const auto& entry) { return SameName(entry.first, name); });
    return match == colors.end() ? 0 : match->second;
}

struct TagEditor : DialogAppearance {
    double textScale;
    std::vector<std::wstring> initial;
    std::vector<std::wstring> names;
    std::vector<std::wstring> result;
    HWND list = nullptr;
    std::size_t checked = 0;
    int animatedRow = -1;
    InteractionFade checkFade;
    int listHeight;
    std::optional<TagColors> colors;

    COLORREF DisplayColor(std::size_t index) const {
        const auto color = colors ? FindTagColor(*colors, names[index]) : 0;
        return color ? color - 1 : TagMarkerColor(NameColorHash(names[index]), palette);
    }

    RECT ColorSquare(RECT row) const {
        const auto dpi = GetDpiForWindow(list);
        const LONG inset = MulDiv(8, dpi, 96);
        const LONG size = (std::min<LONG>)(MulDiv(16, dpi, 96), row.bottom - row.top - 2);
        const LONG top = (row.top + row.bottom - size) / 2;
        return {row.right - inset - size, top, row.right - inset, top + size};
    }

    bool IsColorClick(int index, POINT position) const {
        RECT row{};
        if (!colors || palette.highContrast || !ListView_GetItemRect(list, index, &row, LVIR_BOUNDS)) {
            return false;
        }
        auto square = ColorSquare(row);
        InflateRect(&square, MulDiv(4, GetDpiForWindow(list), 96), 0);
        return PtInRect(&square, position);
    }

    void EditColor(HWND dialog, bool reset) {
        const auto index = ListView_GetNextItem(list, -1, LVNI_SELECTED);
        if (!colors || index < 0 || static_cast<std::size_t>(index) >= names.size()) {
            return;
        }
        const auto name = names[index];
        auto match = std::ranges::find_if(*colors, [&](const auto& entry) { return SameName(entry.first, name); });
        std::uint32_t color = 0;
        if (!reset) {
            COLORREF custom[16]{};
            CHOOSECOLORW chooser{};
            chooser.lStructSize = sizeof(chooser);
            chooser.hwndOwner = dialog;
            chooser.lpCustColors = custom;
            chooser.Flags = CC_FULLOPEN | CC_RGBINIT;
            chooser.rgbResult = DisplayColor(static_cast<std::size_t>(index));
            if (!ChooseColorW(&chooser)) {
                return;
            }
            color = chooser.rgbResult + 1;
        }
        if (match == colors->end()) {
            colors->emplace_back(name, color);
        } else {
            match->second = color;
        }
        ListView_RedrawItems(list, index, index);
        SetFocus(list);
    }

    void UpdateStatus(HWND dialog) const {
        const auto status = names.empty() ? Ui(L"No tags yet. Create a tag to assign it.") :
            std::to_wstring(checked) + Ui(L" of ") + std::to_wstring(kMaxTags) + Ui(L" tags selected");
        SetDlgItemTextW(dialog, kTagStatus, status.c_str());
    }

    bool InsertTag(std::wstring& name) const {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = ListView_GetItemCount(list);
        item.pszText = name.data();
        return ListView_InsertItem(list, &item) == item.iItem;
    }

    void CreateTag(HWND dialog) {
        if (checked == kMaxTags) {
            SetDlgItemTextW(dialog, kTagStatus, Ui(L"Clear a checkbox before adding another tag."));
            return;
        }
        const auto input = PromptText(dialog, L"New tag", L"Enter a name for the new tag.",
                                      kMaxNameLength, false, L"", theme);
        if (!input || GetWindowLongPtrW(dialog, DWLP_USER) != reinterpret_cast<LONG_PTR>(this)) {
            return;
        }
        const auto name = TrimName({input->data.data(), input->size});
        if (name.empty() || !ValidName(name)) {
            SetDlgItemTextW(dialog, kTagStatus, Ui(L"Enter a valid tag name without control characters."));
            return;
        }
        const auto found = std::ranges::find_if(names, [&](const auto& value) { return SameName(value, name); });
        if (found != names.end()) {
            const auto index = static_cast<int>(found - names.begin());
            ListView_EnsureVisible(list, index, FALSE);
            ListView_SetItemState(list, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            SetFocus(list);
            SetDlgItemTextW(dialog, kTagStatus, Ui(L"This tag already exists. Use its checkbox to assign it."));
            return;
        }
        auto& created = names.emplace_back(name);
        if (!InsertTag(created)) {
            names.pop_back();
            SetDlgItemTextW(dialog, kTagStatus, Ui(L"Windows could not create the tag."));
            return;
        }
        const auto index = static_cast<int>(names.size() - 1);
        ListView_SetCheckState(list, index, TRUE);
        ListView_SetItemState(list, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list, index, FALSE);
        SetFocus(list);
    }

    void Commit() {
        result.clear();
        // Preserve the order of existing memberships so saving unchanged tags is a no-op.
        for (const auto& tag : initial) {
            auto found = std::ranges::find_if(names, [&](const auto& value) { return SameName(value, tag); });
            if (found != names.end() && ListView_GetCheckState(list, found - names.begin())) {
                result.push_back(tag);
            }
        }
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (ListView_GetCheckState(list, i) && std::ranges::none_of(result, [&](const auto& tag) {
                    return SameName(tag, names[i]);
                })) {
                result.push_back(names[i]);
            }
        }
    }

    LRESULT DrawTag(const NMLVCUSTOMDRAW& item) const {
        if (palette.highContrast) {
            return CDRF_DODEFAULT;
        }
        if (item.nmcd.dwDrawStage == CDDS_PREPAINT) {
            return CDRF_NOTIFYITEMDRAW;
        }
        const auto index = static_cast<std::size_t>(item.nmcd.dwItemSpec);
        if (item.nmcd.dwDrawStage != CDDS_ITEMPREPAINT || index >= names.size()) {
            return CDRF_DODEFAULT;
        }
        RECT row{};
        if (!ListView_GetItemRect(list, static_cast<int>(index), &row, LVIR_BOUNDS)) {
            return CDRF_DODEFAULT;
        }
        const auto dpi = GetDpiForWindow(list);
        const auto px = [dpi](int value) { return MulDiv(value, dpi, 96); };
        const auto dc = item.nmcd.hdc;
        const auto state = ListView_GetItemState(list, index, LVIS_SELECTED | LVIS_FOCUSED);
        FillSurface(dc, row, palette.card);
        auto surface = row;
        InflateRect(&surface, -px(2), 0);
        if (state & LVIS_SELECTED) {
            PaintRounded(dc, surface, px(4), palette.selected, palette.selected);
        }
        const auto center = (row.top + row.bottom) / 2;
        const auto size = (std::min)(px(16), static_cast<int>(row.bottom - row.top) - px(2));
        const RECT box{row.left + px(4), center - size / 2, row.left + px(4) + size, center - size / 2 + size};
        const int check = static_cast<int>(index) == animatedRow ? checkFade.Value(GetTickCount64()) :
                          ListView_GetCheckState(list, index) ? 100 : 0;
        PaintCheckBox(dc, box, check, true, palette.card, palette);
        RECT label{row.left + px(28), row.top, row.right - px(8), row.bottom};
        auto textColor = palette.text;
        if (colors) {
            const auto swatch = ColorSquare(row);
            const auto color = DisplayColor(index);
            PaintRounded(dc, swatch, px(2), color, palette.muted);

            const auto previous = SelectObject(dc, font);
            SIZE textSize{};
            GetTextExtentPoint32W(dc, names[index].data(), static_cast<int>(names[index].size()), &textSize);
            if (previous) {
                SelectObject(dc, previous);
            }
            RECT background{label.left - px(4), row.top + 1, swatch.left - px(8), row.bottom - 1};
            background.right = (std::min)(background.right, background.left + textSize.cx + px(12));
            PaintRounded(dc, background, px(4), color, color);
            label.right = background.right - px(4);
            textColor = AccentText(color);
        }
        if ((state & LVIS_FOCUSED) && GetFocus() == list &&
            !(SendMessageW(list, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
            PaintFocus(dc, surface, px(1), palette.focusOuter, palette.focusInner);
        }
        PaintText(dc, names[index], label, font, textColor, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        return CDRF_SKIPDEFAULT;
    }
};

inline INT_PTR CALLBACK TagEditorProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_INITDIALOG) {
        const auto editor = reinterpret_cast<TagEditor*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(editor));
        SetWindowTextW(dialog, Ui(L"Edit tags"));
        editor->Initialize(dialog, editor->textScale);
        const auto add = [&](PCWSTR type, PCWSTR text, DWORD style, int id, RECT bounds) {
            return AddDialogControl(dialog, type, text, style, id, bounds, editor->font, editor->textScale);
        };
        add(L"STATIC", L"Select tags for this account.", SS_NOPREFIX, -1, {12, 9, 232, 24});
        const int listBottom = 26 + editor->listHeight;
        editor->list = add(WC_LISTVIEWW, L"Available tags",
                           LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL | WS_BORDER | WS_TABSTOP,
                           kTagList, {12, 26, 232, listBottom});
        const auto create = add(L"BUTTON", L"New tag...", WS_TABSTOP, kNewTag, {12, listBottom + 6, 78, listBottom + 25});
        if (editor->colors) {
            const auto pick = add(L"BUTTON", L"Color...", WS_TABSTOP, kTagColor,
                                  {84, listBottom + 6, 147, listBottom + 25});
            const auto reset = add(L"BUTTON", L"Reset color", WS_TABSTOP, kAutoTagColor,
                                   {153, listBottom + 6, 232, listBottom + 25});
            if (pick && reset) {
                StyleDialogControl(pick, DialogControl::Button, editor->palette);
                StyleDialogControl(reset, DialogControl::Button, editor->palette);
                EnableWindow(pick, !editor->names.empty());
                EnableWindow(reset, !editor->names.empty());
            }
        }
        add(L"STATIC", L"", SS_NOPREFIX, kTagStatus, {12, listBottom + 30, 232, listBottom + 45});
        const auto ok = add(L"BUTTON", L"Save", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK,
                            {124, listBottom + 48, 175, listBottom + 67});
        const auto cancel = add(L"BUTTON", L"Cancel", WS_TABSTOP, IDCANCEL,
                                {181, listBottom + 48, 232, listBottom + 67});
        if (!editor->list || !create || !ok || !cancel) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        for (const auto button : {create, ok, cancel}) {
            StyleDialogControl(button, DialogControl::Button, editor->palette);
        }
        const auto list = editor->list;
        ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT |
                                               LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        RECT bounds{};
        GetClientRect(list, &bounds);
        LVCOLUMNW column{};
        column.mask = LVCF_WIDTH;
        column.cx = bounds.right - GetSystemMetricsForDpi(SM_CXVSCROLL, GetDpiForWindow(list));
        if (ListView_InsertColumn(list, 0, &column) < 0) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        ListView_SetBkColor(list, editor->palette.card);
        ListView_SetTextColor(list, editor->palette.text);
        ListView_SetTextBkColor(list, CLR_NONE);
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        for (std::size_t i = 0; i < editor->names.size(); ++i) {
            auto& name = editor->names[i];
            if (!editor->InsertTag(name)) {
                EndDialog(dialog, IDCANCEL);
                return TRUE;
            }
            if (std::ranges::any_of(editor->initial, [&](const auto& tag) { return SameName(tag, name); })) {
                ListView_SetCheckState(list, i, TRUE);
            }
        }
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        editor->UpdateStatus(dialog);
        if (!editor->names.empty()) {
            ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        }
        SetFocus(editor->names.empty() ? create : list);
        return FALSE;
    }
    const auto editor = reinterpret_cast<TagEditor*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (!editor) {
        return FALSE;
    }
    if (editor->HandleColorChange(dialog, message)) {
        if (editor->list) {
            ListView_SetBkColor(editor->list, editor->palette.card);
            ListView_SetTextColor(editor->list, editor->palette.text);
        }
        return FALSE;
    }
    constexpr UINT_PTR checkTimer = 1;
    switch (message) {
        case WM_TIMER:
            if (wparam == checkTimer) {
                if (editor->animatedRow >= 0) {
                    ListView_RedrawItems(editor->list, editor->animatedRow, editor->animatedRow);
                }
                if (editor->palette.highContrast || !editor->checkFade.Running(GetTickCount64())) {
                    KillTimer(dialog, checkTimer);
                    editor->animatedRow = -1;
                }
                return TRUE;
            }
            break;
        case WM_NOTIFY: {
            const auto notify = reinterpret_cast<const NMHDR*>(lparam);
            if (notify->idFrom != kTagList) {
                break;
            }
            if (notify->code == NM_CUSTOMDRAW) {
                const auto result = editor->DrawTag(*reinterpret_cast<const NMLVCUSTOMDRAW*>(notify));
                SetWindowLongPtrW(dialog, DWLP_MSGRESULT, result);
                return TRUE;
            }
            if (notify->code == LVN_ITEMCHANGING || notify->code == LVN_ITEMCHANGED) {
                const auto& change = *reinterpret_cast<const NMLISTVIEW*>(notify);
                const auto checked = INDEXTOSTATEIMAGEMASK(2);
                const bool wasChecked = (change.uOldState & LVIS_STATEIMAGEMASK) == checked;
                const bool willCheck = (change.uNewState & LVIS_STATEIMAGEMASK) == checked;
                if (!(change.uChanged & LVIF_STATE) || wasChecked == willCheck) {
                    break;
                }
                if (notify->code == LVN_ITEMCHANGING && willCheck && editor->checked == kMaxTags) {
                    SetDlgItemTextW(dialog, kTagStatus, Ui(L"Clear a checkbox before adding another tag."));
                    SetWindowLongPtrW(dialog, DWLP_MSGRESULT, TRUE);
                    return TRUE;
                }
                if (notify->code == LVN_ITEMCHANGED) {
                    editor->checked = willCheck ? editor->checked + 1 : editor->checked - 1;
                    editor->UpdateStatus(dialog);
                    if (change.iItem >= 0 && IsWindowVisible(dialog) && !editor->palette.highContrast &&
                        UseClientAnimations() && SetTimer(dialog, checkTimer, USER_TIMER_MINIMUM, nullptr)) {
                        const auto now = GetTickCount64();
                        if (editor->animatedRow != change.iItem) {
                            if (editor->animatedRow >= 0) {
                                ListView_RedrawItems(editor->list, editor->animatedRow, editor->animatedRow);
                            }
                            editor->animatedRow = change.iItem;
                            editor->checkFade.Set(wasChecked ? 100 : 0, now, false);
                        }
                        editor->checkFade.Set(willCheck ? 100 : 0, now, true);
                    }
                }
            }
            if (notify->code == NM_CLICK) {
                LVHITTESTINFO hit{};
                hit.pt = reinterpret_cast<const NMITEMACTIVATE*>(notify)->ptAction;
                const auto index = ListView_SubItemHitTest(editor->list, &hit);
                if (index >= 0 && editor->IsColorClick(index, hit.pt)) {
                    ListView_SetItemState(editor->list, index, LVIS_SELECTED | LVIS_FOCUSED,
                                          LVIS_SELECTED | LVIS_FOCUSED);
                    PostMessageW(dialog, WM_COMMAND, kTagColor, 0);
                } else if (index >= 0 && !(hit.flags & LVHT_ONITEMSTATEICON)) {
                    ListView_SetCheckState(editor->list, index, !ListView_GetCheckState(editor->list, index));
                }
            }
            break;
        }
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case kNewTag:
                    editor->CreateTag(dialog);
                    if (!editor->names.empty()) {
                        EnableWindow(GetDlgItem(dialog, kTagColor), TRUE);
                        EnableWindow(GetDlgItem(dialog, kAutoTagColor), TRUE);
                    }
                    return TRUE;
                case kTagColor:
                case kAutoTagColor:
                    editor->EditColor(dialog, LOWORD(wparam) == kAutoTagColor);
                    return TRUE;
                case IDOK:
                    editor->Commit();
                    if (ValidTags(editor->result)) {
                        EndDialog(dialog, IDOK);
                    } else {
                        SetDlgItemTextW(dialog, kTagStatus, Ui(L"Clear a checkbox before saving these tags."));
                    }
                    return TRUE;
                case IDCANCEL:
                    EndDialog(dialog, IDCANCEL);
                    return TRUE;
            }
            break;
        case WM_CLOSE:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC:
            return editor->ColorControl(message, wparam, lparam);
    }
    return FALSE;
}

inline std::optional<std::vector<std::wstring>> PromptAccountTags(HWND owner, ThemeMode theme,
                                                                const std::vector<std::wstring>& initial,
                                                                std::span<const std::wstring> known,
                                                                TagColors* colors = nullptr) {
    const INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
    if (!InitCommonControlsEx(&controls)) {
        return std::nullopt;
    }
    std::vector<std::wstring> names(initial);
    names.insert(names.end(), known.begin(), known.end());
    std::stable_sort(names.begin(), names.end(), TagNameLess);
    names.erase(std::unique(names.begin(), names.end(), SameName), names.end());
    const int listHeight = 5 + 13 * static_cast<int>(std::clamp<std::size_t>(names.size() + 1, 4, 9));
    const auto height = static_cast<short>(listHeight + 103);
    const auto scale = DialogTextScale(owner, 244, height);
    const ModalTemplate dialog(244, height, scale);
    TagEditor editor{{theme}, scale, initial, std::move(names), {}, nullptr, 0, -1, {},
                     listHeight, colors ? std::optional(*colors) : std::nullopt};
    const auto result = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &dialog.bounds, owner,
                                                 TagEditorProc, reinterpret_cast<LPARAM>(&editor));
    if (result != IDOK) {
        return std::nullopt;
    }
    if (colors) {
        *colors = std::move(*editor.colors);
    }
    return std::move(editor.result);
}

enum ManualAccountField : int {
    kManualLabel = 201, kManualIssuer, kManualKey, kManualTags, kManualAlgorithm,
    kManualDigits, kManualPeriod, kManualError, kManualTagCount
};

inline constexpr int kSetupKeyLimit = 512;
inline constexpr std::array kManualAlgorithms{Algorithm::Sha1, Algorithm::Sha256, Algorithm::Sha512};

struct ManualAccountDialog : DialogAppearance {
    double textScale;
    std::vector<std::wstring> tags;
    std::span<const std::wstring> knownTags;
    std::optional<Account> account;
};

inline void UpdateManualTagCount(HWND dialog, std::size_t count) {
    const auto status = count ? std::to_wstring(count) + Ui(L" selected") : Ui(L"No tags selected");
    SetDlgItemTextW(dialog, kManualTagCount, status.c_str());
    NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, GetDlgItem(dialog, kManualTagCount), OBJID_CLIENT, CHILDID_SELF);
}

inline bool InitializeManualAccountDialog(HWND dialog, ManualAccountDialog& state) {
    SetWindowTextW(dialog, Ui(L"Add account"));
    SetWindowDisplayAffinity(dialog, WDA_EXCLUDEFROMCAPTURE);
    const auto dpi = GetDpiForWindow(dialog);
    state.Initialize(dialog, state.textScale);
    const auto add = [&](PCWSTR type, PCWSTR text, DWORD style, int id, RECT rect) {
        return AddDialogControl(dialog, type, text, style, id, rect, state.font, state.textScale);
    };
    const auto label = [&](PCWSTR text, int left, int top, int right) {
        add(L"STATIC", text, SS_NOPREFIX, -1, {left, top, right, top + 12});
    };
    const auto edit = [&](PCWSTR text, int id, int left, int top, int right, DWORD style, int limit) {
        const auto control = add(L"EDIT", text, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | style,
                                 id, {left, top, right, top + 17});
        SendMessageW(control, EM_SETLIMITTEXT, limit, 0);
        const auto margin = MulDiv(5, dpi, 96);
        SendMessageW(control, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(margin, margin));
        return control;
    };
    const auto combo = [&](int id, int left, int right, std::initializer_list<PCWSTR> values) {
        const auto control = add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, id, {left, 170, right, 238});
        if (control) {
            for (const auto value : values) {
                SendMessageW(control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value));
            }
            SendMessageW(control, CB_SETCURSEL, 0, 0);
        }
        return control;
    };
    label(L"Account name", 12, 10, 276);
    const auto name = edit(L"", kManualLabel, 12, 22, 276, 0, kMaxNameLength);
    label(L"Provider (optional)", 12, 46, 276);
    const auto issuer = edit(L"", kManualIssuer, 12, 58, 276, 0, kMaxNameLength);
    label(L"Setup key (Base32)", 12, 82, 276);
    const auto key = edit(L"", kManualKey, 12, 94, 276, ES_PASSWORD, kSetupKeyLimit);
    const auto tags = add(L"BUTTON", L"Edit tags...", WS_TABSTOP, kManualTags, {12, 122, 110, 142});
    const auto tagCount = add(L"STATIC", L"", SS_NOPREFIX | SS_CENTERIMAGE, kManualTagCount, {118, 122, 276, 142});
    // The lists extend below the form when expanded; each collapsed control occupies one row.
    label(L"Algorithm", 12, 158, 110);
    const auto algorithm = combo(kManualAlgorithm, 12, 110, {L"SHA-1", L"SHA-256", L"SHA-512"});
    label(L"Digits", 118, 158, 184);
    const auto digits = combo(kManualDigits, 118, 184, {L"6", L"8"});
    label(L"Period (seconds)", 192, 158, 276);
    const auto period = edit(L"30", kManualPeriod, 192, 170, 276, ES_NUMBER, 4);
    const auto error = add(L"STATIC", L"", SS_NOPREFIX, kManualError, {12, 193, 276, 221});
    const auto ok = add(L"BUTTON", L"Add account", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK, {140, 225, 207, 244});
    const auto cancel = add(L"BUTTON", L"Cancel", WS_TABSTOP, IDCANCEL, {214, 225, 276, 244});
    if (!name || !issuer || !key || !tags || !tagCount || !algorithm || !digits || !period || !error || !ok || !cancel) {
        return false;
    }
    UpdateManualTagCount(dialog, state.tags.size());
    StyleDialogControl(tags, DialogControl::Button, state.palette);
    StyleDialogControl(algorithm, DialogControl::ComboBox, state.palette);
    StyleDialogControl(digits, DialogControl::ComboBox, state.palette);
    StyleDialogControl(ok, DialogControl::Button, state.palette);
    StyleDialogControl(cancel, DialogControl::Button, state.palette);
    SetFocus(name);
    return true;
}

inline bool ReadManualAccount(HWND dialog, ManualAccountDialog& state) {
    const auto reject = [&](int id, PCWSTR reason) {
        SetDlgItemTextW(dialog, kManualError, Ui(reason));
        const auto error = GetDlgItem(dialog, kManualError);
        NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, error, OBJID_CLIENT, CHILDID_SELF);
        SetFocus(GetDlgItem(dialog, id));
        return false;
    };
    const auto readName = [&](int id) {
        wchar_t text[kMaxNameLength + 1]{};
        GetDlgItemTextW(dialog, id, text, ARRAYSIZE(text));
        return std::wstring(TrimName(text));
    };
    Account account;
    account.label = readName(kManualLabel);
    if (account.label.empty() || !ValidName(account.label)) {
        return reject(kManualLabel, L"Enter an account name without control characters.");
    }
    account.issuer = readName(kManualIssuer);
    if (!ValidName(account.issuer)) {
        return reject(kManualIssuer, L"Use a provider name without control characters.");
    }
    account.tags = state.tags;
    wchar_t period[5]{};
    GetDlgItemTextW(dialog, kManualPeriod, period, ARRAYSIZE(period));
    account.period = ParseNumber(period).value_or(0);
    if (account.period < 1 || account.period > 3600) {
        return reject(kManualPeriod, L"Enter a period from 1 to 3600 seconds.");
    }
    const auto algorithm = SendDlgItemMessageW(dialog, kManualAlgorithm, CB_GETCURSEL, 0, 0);
    const auto digits = SendDlgItemMessageW(dialog, kManualDigits, CB_GETCURSEL, 0, 0);
    if (algorithm < 0 || algorithm >= std::ssize(kManualAlgorithms) || digits < 0 || digits > 1) {
        return reject(kManualAlgorithm, L"Select an algorithm and code length.");
    }
    account.algorithm = kManualAlgorithms[algorithm];
    account.digits = digits == 0 ? 6 : 8;
    SensitiveBuffer<wchar_t, kSetupKeyLimit + 1> key;
    key.size = GetDlgItemTextW(dialog, kManualKey, key.data.data(), static_cast<int>(key.data.size()));
    SecretBytes decoded;
    if (!ParseBase32({key.data.data(), key.size}, decoded, SetupKeyFormat::Manual) || decoded.size < 10) {
        return reject(kManualKey, L"Enter a Base32 setup key with at least 16 letters or digits.");
    }
    if (!account.secret.Store(std::move(decoded))) {
        return reject(kManualKey, L"Windows could not protect the setup key. Try again.");
    }
    state.account = std::move(account);
    return true;
}

inline INT_PTR CALLBACK ManualAccountProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_INITDIALOG) {
        const auto state = reinterpret_cast<ManualAccountDialog*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        if (!InitializeManualAccountDialog(dialog, *state)) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        return FALSE;
    }
    const auto state = reinterpret_cast<ManualAccountDialog*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (!state || state->HandleColorChange(dialog, message)) {
        return FALSE;
    }
    switch (message) {
        case WM_COMMAND:
            if (LOWORD(wparam) == kManualTags && HIWORD(wparam) == BN_CLICKED) {
                if (auto tags = PromptAccountTags(dialog, state->theme, state->tags, state->knownTags)) {
                    state->tags = std::move(*tags);
                    UpdateManualTagCount(dialog, state->tags.size());
                }
                return TRUE;
            }
            if (LOWORD(wparam) == kManualKey && HIWORD(wparam) == EN_CHANGE) {
                SendDlgItemMessageW(dialog, kManualKey, EM_EMPTYUNDOBUFFER, 0, 0);
                return TRUE;
            }
            if (LOWORD(wparam) == IDOK && !ReadManualAccount(dialog, *state)) {
                return TRUE;
            }
            if (LOWORD(wparam) != IDOK && LOWORD(wparam) != IDCANCEL) {
                break;
            }
            [[fallthrough]];
        case WM_CLOSE:
            SetDlgItemTextW(dialog, kManualKey, L"");
            SendDlgItemMessageW(dialog, kManualKey, EM_EMPTYUNDOBUFFER, 0, 0);
            EndDialog(dialog, message == WM_CLOSE ? IDCANCEL : LOWORD(wparam));
            return TRUE;
        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            return state->ColorControl(message, wparam, lparam);
    }
    return FALSE;
}

inline std::optional<Account> PromptManualAccount(HWND owner, ThemeMode theme, const std::wstring& initialTag,
                                                   std::span<const std::wstring> knownTags) {
    const auto scale = DialogTextScale(owner, 288, 254);
    const ModalTemplate dialog(288, 254, scale);
    auto tags = initialTag.empty() ? std::vector<std::wstring>{} : std::vector<std::wstring>{initialTag};
    ManualAccountDialog state{{theme}, scale, std::move(tags), knownTags, std::nullopt};
    const auto result = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &dialog.bounds, owner,
                                                 ManualAccountProc, reinterpret_cast<LPARAM>(&state));
    return result == IDOK ? std::move(state.account) : std::nullopt;
}

struct HotkeyPreferences {
    WORD chord = MAKEWORD('O', HOTKEYF_ALT);
    bool enabled = false;

    bool operator==(const HotkeyPreferences&) const = default;
};

inline UINT HotkeyModifiers(WORD chord) {
    const auto flags = HIBYTE(chord);
    return (flags & HOTKEYF_CONTROL ? MOD_CONTROL : 0) |
           (flags & HOTKEYF_SHIFT ? MOD_SHIFT : 0) |
           (flags & HOTKEYF_ALT ? MOD_ALT : 0);
}

inline bool ValidHotkey(WORD chord) {
    const auto key = LOBYTE(chord);
    const auto modifiers = HotkeyModifiers(chord);
    return key && key != VK_F12 && (modifiers & (MOD_CONTROL | MOD_ALT)) != 0 &&
           !(HIBYTE(chord) & ~(HOTKEYF_CONTROL | HOTKEYF_SHIFT | HOTKEYF_ALT | HOTKEYF_EXT));
}

inline PCWSTR HotkeyPreferenceName(bool standalone) {
    return standalone ? L"GlobalShortcut" : L"ModGlobalShortcut";
}

inline HotkeyPreferences LoadHotkeyPreferences(bool standalone) {
    const auto value = ReadPreference(HotkeyPreferenceName(standalone), HotkeyPreferences{}.chord);
    return ValidHotkey(LOWORD(value)) ? HotkeyPreferences{LOWORD(value), HIWORD(value) == 1} : HotkeyPreferences{};
}

enum class BackupAction { None, Export, Restore, ExportAegis, ExportAndOtp };

inline constexpr int kShortcutEnabled = 109;
inline constexpr int kShortcutKeys = 110;
inline constexpr int kShortcutStatus = 111;
inline constexpr int kBackupExport = 112;
inline constexpr int kBackupRestore = 113;
inline constexpr int kBackupExportAegis = 117;
inline constexpr int kBackupExportAndOtp = 118;

struct ShortcutPrompt : DialogAppearance {
    double textScale;
    HotkeyPreferences shortcut;
    bool conflict;
    const std::function<bool(HotkeyPreferences)>& apply;
};

inline INT_PTR CALLBACK ShortcutPromptProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_INITDIALOG) {
        const auto state = reinterpret_cast<ShortcutPrompt*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        SetWindowTextW(dialog, Ui(L"Shortcut"));
        state->Initialize(dialog, state->textScale);
        const auto add = [&](PCWSTR type, PCWSTR text, DWORD style, int id, RECT bounds) {
            return AddDialogControl(dialog, type, text, style, id, bounds, state->font, state->textScale);
        };
        const auto enabled = add(L"BUTTON", L"Enable global shortcut", BS_AUTOCHECKBOX | WS_TABSTOP,
                                 kShortcutEnabled, {12, 10, 236, 26});
        CheckDlgButton(dialog, kShortcutEnabled, state->shortcut.enabled ? BST_CHECKED : BST_UNCHECKED);
        add(L"STATIC", L"Shortcut", SS_NOPREFIX, -1, {12, 33, 236, 45});
        const auto chord = add(HOTKEY_CLASSW, L"", WS_TABSTOP | (state->palette.highContrast ? WS_BORDER : 0),
                               kShortcutKeys, {12, 46, 236, 64});
        SendMessageW(chord, HKM_SETHOTKEY, state->shortcut.chord, 0);
        EnableWindow(chord, state->shortcut.enabled);
        const auto status = add(L"STATIC", state->conflict ? L"Shortcut unavailable. Choose another combination." :
                          L"Use Ctrl or Alt plus a key. Shift is optional.",
                          SS_NOPREFIX, kShortcutStatus, {12, 70, 236, 99});
        const auto ok = add(L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK, {130, 103, 181, 122});
        const auto cancel = add(L"BUTTON", L"Cancel", WS_TABSTOP, IDCANCEL, {185, 103, 236, 122});
        if (!enabled || !chord || !status || !ok || !cancel) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        StyleDialogControl(enabled, DialogControl::CheckBox, state->palette);
        StyleDialogControl(chord, DialogControl::Hotkey, state->palette);
        StyleDialogControl(ok, DialogControl::Button, state->palette);
        StyleDialogControl(cancel, DialogControl::Button, state->palette);
        SetFocus(enabled);
        return FALSE;
    }

    const auto state = reinterpret_cast<ShortcutPrompt*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (!state || state->HandleColorChange(dialog, message)) {
        return FALSE;
    }
    switch (message) {
        case WM_COMMAND: {
            const auto id = LOWORD(wparam);
            const bool enabled = IsDlgButtonChecked(dialog, kShortcutEnabled) == BST_CHECKED;
            if (id == kShortcutEnabled && HIWORD(wparam) == BN_CLICKED) {
                EnableWindow(GetDlgItem(dialog, kShortcutKeys), enabled);
                return TRUE;
            }
            if (id == IDOK) {
                const auto input = static_cast<WORD>(SendDlgItemMessageW(dialog, kShortcutKeys, HKM_GETHOTKEY, 0, 0));
                const auto valid = ValidHotkey(input);
                const auto chord = valid ? input : state->shortcut.chord;
                if ((enabled && !valid) || !state->apply({chord, enabled})) {
                    SetDlgItemTextW(dialog, kShortcutStatus, Ui(enabled && !valid ?
                        L"Use Ctrl or Alt plus a key. F12 is reserved by Windows." :
                        L"Shortcut unavailable. Choose another combination."));
                    NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, GetDlgItem(dialog, kShortcutStatus), OBJID_CLIENT, CHILDID_SELF);
                    SetFocus(GetDlgItem(dialog, kShortcutKeys));
                    return TRUE;
                }
            }
            if (id == IDOK || id == IDCANCEL) {
                EndDialog(dialog, id);
                return TRUE;
            }
            break;
        }
        case WM_CLOSE:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
            return state->ColorControl(message, wparam, lparam);
    }
    return FALSE;
}

inline void EditShortcut(HWND owner, ThemeMode theme, HotkeyPreferences shortcut, bool conflict,
                         const std::function<bool(HotkeyPreferences)>& apply) {
    const INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_HOTKEY_CLASS};
    if (!InitCommonControlsEx(&controls)) {
        return;
    }
    const auto scale = DialogTextScale(owner, 248, 133);
    const ModalTemplate dialog(248, 133, scale);
    ShortcutPrompt prompt{{theme}, scale, shortcut, conflict, apply};
    DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &dialog.bounds, owner,
                            ShortcutPromptProc, reinterpret_cast<LPARAM>(&prompt));
}

struct BackupPrompt : DialogAppearance {
    double textScale;
    bool canExport;
    bool canRestore;
};

inline INT_PTR CALLBACK BackupPromptProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_INITDIALOG) {
        const auto state = reinterpret_cast<BackupPrompt*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        SetWindowTextW(dialog, Ui(L"Backup"));
        state->Initialize(dialog, state->textScale);
        const auto add = [&](PCWSTR type, PCWSTR text, DWORD style, int id, RECT bounds) {
            return AddDialogControl(dialog, type, text, style, id, bounds, state->font, state->textScale);
        };
        const auto button = [&](int id, PCWSTR text, int top, bool enabled) {
            const auto control = add(L"BUTTON", text, WS_TABSTOP, id, {12, top, 260, top + 19});
            if (control) {
                EnableWindow(control, enabled);
                StyleDialogControl(control, DialogControl::Button, state->palette);
            }
            return control;
        };
        const auto save = button(kBackupExport, L"Export encrypted backup...", 10, state->canExport);
        const auto restore = button(kBackupRestore, L"Restore from encrypted backup...", 33, state->canRestore);
        add(L"STATIC", L"Portable exports contain unencrypted setup keys.", SS_NOPREFIX, -1, {12, 63, 260, 80});
        const auto aegis = button(kBackupExportAegis, L"Export Aegis JSON...", 83, state->canExport);
        const auto andOtp = button(kBackupExportAndOtp, L"Export andOTP JSON...", 106, state->canExport);
        const auto close = add(L"BUTTON", L"Close", BS_DEFPUSHBUTTON | WS_TABSTOP, IDCANCEL, {205, 137, 260, 156});
        if (!save || !restore || !aegis || !andOtp || !close) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        StyleDialogControl(close, DialogControl::Button, state->palette);
        SendMessageW(dialog, DM_SETDEFID, IDCANCEL, 0);
        SetFocus(close);
        return FALSE;
    }
    const auto state = reinterpret_cast<BackupPrompt*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (!state || state->HandleColorChange(dialog, message)) {
        return FALSE;
    }
    switch (message) {
        case WM_COMMAND: {
            const auto id = LOWORD(wparam);
            if (id == IDCANCEL || (id == kBackupRestore && state->canRestore) ||
                (state->canExport && (id == kBackupExport || id == kBackupExportAegis || id == kBackupExportAndOtp))) {
                EndDialog(dialog, id);
                return TRUE;
            }
            break;
        }
        case WM_CLOSE:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            return state->ColorControl(message, wparam, lparam);
    }
    return FALSE;
}

inline BackupAction ChooseBackupAction(HWND owner, ThemeMode theme, bool canExport, bool canRestore) {
    const auto scale = DialogTextScale(owner, 272, 168);
    const ModalTemplate dialog(272, 168, scale);
    BackupPrompt prompt{{theme}, scale, canExport, canRestore};
    const auto result = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &dialog.bounds, owner,
                                                BackupPromptProc, reinterpret_cast<LPARAM>(&prompt));
    switch (result) {
        case kBackupExport: return BackupAction::Export;
        case kBackupRestore: return BackupAction::Restore;
        case kBackupExportAegis: return BackupAction::ExportAegis;
        case kBackupExportAndOtp: return BackupAction::ExportAndOtp;
    }
    return BackupAction::None;
}

inline HWND AddPreferenceCombo(HWND dialog, const DialogAppearance& appearance, double scale, int id, PCWSTR label,
                               int y, int right, WPARAM selected) {
    AddDialogControl(dialog, L"STATIC", label, 0, -1, {12, y, right, y + 13}, appearance.font, scale);
    const auto control = AddDialogControl(dialog, L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED |
                                          CBS_HASSTRINGS | WS_TABSTOP | WS_VSCROLL, id, {12, y + 13, right, y + 136},
                                          appearance.font, scale);
    if (!control) {
        return nullptr;
    }
    StyleDialogControl(control, DialogControl::ComboBox, appearance.palette);
    const auto rowHeight = MulDiv(ScaleDialogUnits(28, scale), GetDpiForWindow(dialog), 96);
    SendMessageW(control, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), rowHeight);
    SendMessageW(control, CB_SETITEMHEIGHT, 0, rowHeight);
    for (const auto name : PreferenceLabels(id)) {
        if (SendMessageW(control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Ui(name))) < 0) {
            return nullptr;
        }
    }
    SendMessageW(control, CB_SETCURSEL, selected, 0);
    return control;
}

inline bool PaintPreferenceItem(HWND dialog, const DialogAppearance& appearance, const DRAWITEMSTRUCT& item) {
    const auto labels = PreferenceLabels(item.CtlID);
    if (item.itemID >= labels.size()) {
        return false;
    }
    const auto& colors = appearance.palette;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool selected = (item.itemState & ODS_SELECTED) != 0 && !disabled;
    FillSurface(item.hDC, item.rcItem, selected ? colors.selected : colors.card);
    auto bounds = item.rcItem;
    bounds.left += MulDiv(10, GetDpiForWindow(dialog), 96);
    PaintText(item.hDC, Ui(labels[item.itemID]), bounds, appearance.font,
              disabled ? colors.disabledText : selected ? colors.selectionText : colors.text,
              DT_SINGLELINE | DT_VCENTER);
    if ((item.itemState & (ODS_FOCUS | ODS_NOFOCUSRECT)) == ODS_FOCUS) {
        PaintFocus(item.hDC, item.rcItem, MulDiv(1, GetDpiForWindow(dialog), 96), colors.focusOuter, colors.focusInner);
    }
    return true;
}

struct SettingsPrompt : DialogAppearance {
    FlyoutPreferences preferences;
    double textScale;
};

inline INT_PTR CALLBACK SettingsPromptProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_INITDIALOG) {
        const auto settings = reinterpret_cast<SettingsPrompt*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(settings));
        SetWindowTextW(dialog, Ui(L"TOTP flyout settings"));
        settings->Initialize(dialog, settings->textScale);
        const auto add = [&](PCWSTR kind, PCWSTR text, DWORD style, int id, RECT bounds) {
            return AddDialogControl(dialog, kind, text, style, id, bounds, settings->font, settings->textScale);
        };
        const auto combo = [&](int id, PCWSTR label, int y, WPARAM selected) {
            return AddPreferenceCombo(dialog, *settings, settings->textScale, id, label, y, 206, selected);
        };
        const auto check = [&](int id, PCWSTR label, int y, bool checked) {
            const auto control = add(L"BUTTON", label, BS_AUTOCHECKBOX | WS_TABSTOP, id,
                                     {12, y, 206, y + 15});
            if (control) {
                StyleDialogControl(control, DialogControl::CheckBox, settings->palette);
                CheckDlgButton(dialog, id, checked ? BST_CHECKED : BST_UNCHECKED);
            }
            return control;
        };
        const auto& prefs = settings->preferences;
        const auto theme = combo(kThemeChoice, L"&Theme", 8, static_cast<WPARAM>(prefs.theme));
        const auto show = check(kShowCodesChoice, L"&Show one-time codes", 44, prefs.showCodes);
        const auto countdown = check(kCountdownChoice, L"Show timer seconds", 61, prefs.showCountdown);
        const auto motion = check(kMotionChoice, L"Animate copy confirmation", 78, prefs.animateCopy);
        const auto screenshot = check(kScreenshotChoice, L"Allow screenshots of the flyout", 95, prefs.allowScreenshots);
        const auto hideAfterCopy = check(kHideAfterCopyChoice, L"Hide the flyout after copying", 112, prefs.hideAfterCopy);
        const auto russian = check(kRussianChoice, L"Русский интерфейс (Russian)", 129, prefs.russian);
        const auto lock = combo(kLockChoice, L"Auto-lock after inactivity", 150,
                                std::ranges::find(kLockMinutes, prefs.lockMinutes) - kLockMinutes.begin());
        const auto clipboard = combo(kClipboardChoice, L"Clear copied codes after", 187,
                                     std::ranges::find(kClipboardSeconds, prefs.clipboardSeconds) -
                                     kClipboardSeconds.begin());
        add(L"STATIC", L"Codes clear sooner if they expire.", 0, kSettingsHelp, {12, 225, 206, 238});
        constexpr int footer = 243;
        const auto ok = add(L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK, {102, footer, 152, footer + 17});
        const auto cancel = add(L"BUTTON", L"Cancel", WS_TABSTOP, IDCANCEL, {156, footer, 206, footer + 17});
        if (!theme || !show || !countdown || !motion || !screenshot || !hideAfterCopy || !russian ||
            !lock || !clipboard || !ok || !cancel) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        StyleDialogControl(ok, DialogControl::Button, settings->palette);
        StyleDialogControl(cancel, DialogControl::Button, settings->palette);
        SetFocus(theme);
        return FALSE;
    }
    const auto settings = reinterpret_cast<SettingsPrompt*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (!settings || settings->HandleColorChange(dialog, message)) {
        return FALSE;
    }
    switch (message) {
        case WM_DRAWITEM:
            return PaintPreferenceItem(dialog, *settings, *reinterpret_cast<const DRAWITEMSTRUCT*>(lparam));
        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLORLISTBOX: {
            const bool hint = message == WM_CTLCOLORSTATIC &&
                GetDlgCtrlID(reinterpret_cast<HWND>(lparam)) == kSettingsHelp;
            return settings->ColorControl(message, wparam, lparam, hint);
        }
        case WM_COMMAND:
            if (HIWORD(wparam) == CBN_SELCHANGE && !PreferenceLabels(LOWORD(wparam)).empty()) {
                InvalidateRect(GetDlgItem(dialog, LOWORD(wparam)), nullptr, FALSE);
                return TRUE;
            }
            if (LOWORD(wparam) == IDOK) {
                const auto selected = SendDlgItemMessageW(dialog, kThemeChoice, CB_GETCURSEL, 0, 0);
                const auto lock = SendDlgItemMessageW(dialog, kLockChoice, CB_GETCURSEL, 0, 0);
                const auto clipboard = SendDlgItemMessageW(dialog, kClipboardChoice, CB_GETCURSEL, 0, 0);
                if (selected >= 0 && selected < std::ssize(kThemeNames) &&
                    lock >= 0 && lock < std::ssize(kLockMinutes) &&
                    clipboard >= 0 && clipboard < std::ssize(kClipboardSeconds)) {
                    settings->preferences = {static_cast<ThemeMode>(selected),
                        IsDlgButtonChecked(dialog, kShowCodesChoice) == BST_CHECKED,
                        IsDlgButtonChecked(dialog, kCountdownChoice) == BST_CHECKED,
                        IsDlgButtonChecked(dialog, kMotionChoice) == BST_CHECKED,
                        kLockMinutes[lock], kClipboardSeconds[clipboard],
                        IsDlgButtonChecked(dialog, kScreenshotChoice) == BST_CHECKED,
                        IsDlgButtonChecked(dialog, kHideAfterCopyChoice) == BST_CHECKED,
                        IsDlgButtonChecked(dialog, kRussianChoice) == BST_CHECKED};
                    EndDialog(dialog, IDOK);
                }
                return TRUE;
            }
            if (LOWORD(wparam) != IDCANCEL) {
                break;
            }
            [[fallthrough]];
        case WM_CLOSE:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
    }
    return FALSE;
}

inline std::optional<FlyoutPreferences> EditPreferences(HWND owner, FlyoutPreferences preferences) {
    const auto scale = DialogTextScale(owner, 218, 270);
    const ModalTemplate dialog(218, 270, scale);
    SettingsPrompt prompt{{preferences.theme}, preferences, scale};
    const auto result = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &dialog.bounds, owner,
                                                 SettingsPromptProc, reinterpret_cast<LPARAM>(&prompt));
    return result == IDOK ? std::optional(prompt.preferences) : std::nullopt;
}

struct PlacementPrompt : DialogAppearance {
    double textScale;
    WindowPlacementPreferences preferences;
};

inline void UpdatePlacementChecks(HWND dialog) {
    const bool manual = SendDlgItemMessageW(dialog, kPlacementChoice, CB_GETCURSEL, 0, 0) ==
                        static_cast<LRESULT>(WindowPlacementMode::Manual);
    EnableWindow(GetDlgItem(dialog, kLockDragChoice), manual);
    EnableWindow(GetDlgItem(dialog, kRememberPositionChoice), manual);
}

inline INT_PTR CALLBACK PlacementPromptProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_INITDIALOG) {
        const auto state = reinterpret_cast<PlacementPrompt*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        SetWindowTextW(dialog, Ui(L"Window position"));
        state->Initialize(dialog, state->textScale);
        const auto add = [&](PCWSTR type, PCWSTR text, DWORD style, int id, RECT bounds) {
            return AddDialogControl(dialog, type, text, style, id, bounds, state->font, state->textScale);
        };
        const auto mode = AddPreferenceCombo(dialog, *state, state->textScale, kPlacementChoice, L"&Position",
                                             10, 248, static_cast<WPARAM>(state->preferences.mode));
        const auto lockDrag = add(L"BUTTON", L"&Lock window dragging", BS_AUTOCHECKBOX | WS_TABSTOP,
                                  kLockDragChoice, {12, 49, 248, 66});
        const auto remember = add(L"BUTTON", L"&Remember manual position after restart", BS_AUTOCHECKBOX | WS_TABSTOP,
                                   kRememberPositionChoice, {12, 69, 248, 86});
        add(L"STATIC", L"Drag the title to move the window in Manual mode. A corner follows the taskbar monitor.",
            SS_NOPREFIX, -1, {12, 94, 248, 122});
        const auto ok = add(L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK, {142, 128, 193, 147});
        const auto cancel = add(L"BUTTON", L"Cancel", WS_TABSTOP, IDCANCEL, {197, 128, 248, 147});
        if (!mode || !lockDrag || !remember || !ok || !cancel) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        for (const auto check : {lockDrag, remember}) {
            StyleDialogControl(check, DialogControl::CheckBox, state->palette);
        }
        for (const auto button : {ok, cancel}) {
            StyleDialogControl(button, DialogControl::Button, state->palette);
        }
        CheckDlgButton(dialog, kLockDragChoice, state->preferences.lockDrag ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, kRememberPositionChoice, state->preferences.remember ? BST_CHECKED : BST_UNCHECKED);
        UpdatePlacementChecks(dialog);
        SetFocus(mode);
        return FALSE;
    }
    const auto state = reinterpret_cast<PlacementPrompt*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (!state || state->HandleColorChange(dialog, message)) {
        return FALSE;
    }
    switch (message) {
        case WM_DRAWITEM:
            return PaintPreferenceItem(dialog, *state, *reinterpret_cast<const DRAWITEMSTRUCT*>(lparam));
        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLORLISTBOX:
            return state->ColorControl(message, wparam, lparam);
        case WM_COMMAND:
            if (LOWORD(wparam) == kPlacementChoice && HIWORD(wparam) == CBN_SELCHANGE) {
                InvalidateRect(GetDlgItem(dialog, kPlacementChoice), nullptr, FALSE);
                UpdatePlacementChecks(dialog);
                return TRUE;
            }
            if (LOWORD(wparam) == IDOK) {
                const auto mode = SendDlgItemMessageW(dialog, kPlacementChoice, CB_GETCURSEL, 0, 0);
                if (mode >= 0 && mode < std::ssize(kPlacementNames)) {
                    state->preferences = {static_cast<WindowPlacementMode>(mode),
                                          IsDlgButtonChecked(dialog, kLockDragChoice) == BST_CHECKED,
                                          IsDlgButtonChecked(dialog, kRememberPositionChoice) == BST_CHECKED};
                    EndDialog(dialog, IDOK);
                }
                return TRUE;
            }
            if (LOWORD(wparam) != IDCANCEL) {
                break;
            }
            [[fallthrough]];
        case WM_CLOSE:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
    }
    return FALSE;
}

inline std::optional<WindowPlacementPreferences> EditWindowPlacement(HWND owner, ThemeMode theme,
                                                                    WindowPlacementPreferences preferences) {
    const auto scale = DialogTextScale(owner, 260, 158);
    const ModalTemplate dialog(260, 158, scale);
    PlacementPrompt prompt{{theme}, scale, preferences};
    const auto result = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &dialog.bounds, owner,
                                                PlacementPromptProc, reinterpret_cast<LPARAM>(&prompt));
    return result == IDOK ? std::optional(prompt.preferences) : std::nullopt;
}

inline std::wstring OpenLinkFilePath(HWND owner, bool html) {
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    if (russianUi) {
        dialog.lpstrFilter = html ? L"Файлы HTML (*.html;*.htm)\0*.html;*.htm\0\0" :
                                    L"Текстовые файлы (*.txt)\0*.txt\0\0";
    } else {
        dialog.lpstrFilter = html ? L"HTML files (*.html;*.htm)\0*.html;*.htm\0\0" :
                                    L"Text files (*.txt)\0*.txt\0\0";
    }
    dialog.lpstrFile = path;
    dialog.nMaxFile = ARRAYSIZE(path);
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
    return GetOpenFileNameW(&dialog) ? std::wstring(path) : std::wstring{};
}

inline std::wstring BackupPath(HWND owner, bool forExport, std::optional<ExportFormat> portable = {}) {
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    if (russianUi) {
        dialog.lpstrFilter = portable ? L"Незашифрованный экспорт (*.json)\0*.json\0\0" :
                                        L"Зашифрованные копии (*.2fa-vault)\0*.2fa-vault\0\0";
    } else {
        dialog.lpstrFilter = portable ? L"Unencrypted MFA export (*.json)\0*.json\0\0" :
                                        L"Encrypted 2FA backups (*.2fa-vault)\0*.2fa-vault\0\0";
    }
    dialog.lpstrFile = path;
    dialog.nMaxFile = ARRAYSIZE(path);
    dialog.lpstrDefExt = portable ? L"json" : L"2fa-vault";
    dialog.lpstrTitle = Ui(portable ? L"Save unencrypted export to a new file" :
                         forExport ? L"Save encrypted backup to a new file" : L"Open encrypted backup");
    dialog.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST |
                   OFN_DONTADDTORECENT | OFN_NOTESTFILECREATE;
    if (!forExport) {
        dialog.Flags |= OFN_FILEMUSTEXIST;
    }
    const auto accepted = forExport ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
    return accepted ? path : L"";
}

inline std::wstring_view TextView(const DialogText& value) {
    return {value.data.data(), value.size};
}

}  // namespace twofa
