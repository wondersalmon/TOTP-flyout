#pragma once

#include "localization.hpp"

#include <windows.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <roapi.h>
#include <uxtheme.h>
#include <winstring.h>
#include <windows.ui.viewmanagement.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string_view>

namespace twofa {

class PaintResources {
public:
    PaintResources() {
        buffered_ = SUCCEEDED(BufferedPaintInit());
        const Gdiplus::GdiplusStartupInput options;
        Gdiplus::GdiplusStartup(&token_, &options, nullptr);
        antialias_ = token_ != 0;
    }
    ~PaintResources() {
        antialias_ = false;
        if (token_) {
            Gdiplus::GdiplusShutdown(token_);
        }
        if (buffered_) {
            BufferedPaintUnInit();
        }
    }
    PaintResources(const PaintResources&) = delete;
    PaintResources& operator=(const PaintResources&) = delete;

    static bool Antialias() { return antialias_; }

private:
    static inline thread_local bool antialias_ = false;
    ULONG_PTR token_ = 0;
    bool buffered_ = false;
};

class BufferedPaint {
public:
    BufferedPaint(HDC target, const RECT& bounds) : dc_(target) {
        HDC bufferDc = nullptr;
        buffer_ = BeginBufferedPaint(target, &bounds, BPBF_COMPATIBLEBITMAP, nullptr, &bufferDc);
        if (buffer_) {
            dc_ = bufferDc;
        }
    }
    ~BufferedPaint() {
        if (buffer_) {
            EndBufferedPaint(buffer_, TRUE);
        }
    }
    BufferedPaint(const BufferedPaint&) = delete;
    BufferedPaint& operator=(const BufferedPaint&) = delete;

    HDC Dc() const { return dc_; }

private:
    HDC dc_;
    HPAINTBUFFER buffer_;
};

class WindowsTextSize {
public:
    WindowsTextSize() : initialized_(SUCCEEDED(RoInitialize(RO_INIT_SINGLETHREADED))) {
        constexpr wchar_t name[] = L"Windows.UI.ViewManagement.UISettings";
        HSTRING_HEADER header{};
        HSTRING runtimeClass = nullptr;
        IActivationFactory* factory = nullptr;
        if (SUCCEEDED(WindowsCreateStringReference(name, ARRAYSIZE(name) - 1, &header, &runtimeClass)) &&
            SUCCEEDED(RoGetActivationFactory(runtimeClass, __uuidof(IActivationFactory),
                                             reinterpret_cast<void**>(&factory)))) {
            IInspectable* object = nullptr;
            if (SUCCEEDED(factory->ActivateInstance(&object))) {
                object->QueryInterface(__uuidof(Settings), reinterpret_cast<void**>(&settings_));
                object->QueryInterface(__uuidof(ColorSettings), reinterpret_cast<void**>(&colors_));
                object->Release();
            }
            factory->Release();
        }
    }
    ~WindowsTextSize() {
        if (colors_) {
            colors_->Release();
        }
        if (settings_) {
            settings_->Release();
        }
        if (initialized_) {
            RoUninitialize();
        }
    }
    WindowsTextSize(const WindowsTextSize&) = delete;
    WindowsTextSize& operator=(const WindowsTextSize&) = delete;

    double Factor() const {
        double factor;
        return settings_ && SUCCEEDED(settings_->get_TextScaleFactor(&factor)) ?
            std::clamp(factor, 1.0, 2.25) : 1.0;
    }

    std::optional<COLORREF> Color(ABI::Windows::UI::ViewManagement::UIColorType role) const {
        ABI::Windows::UI::Color color{};
        if (colors_ && SUCCEEDED(colors_->GetColorValue(role, &color))) {
            return RGB(color.R, color.G, color.B);
        }
        return std::nullopt;
    }

private:
    using Settings = ABI::Windows::UI::ViewManagement::IUISettings2;
    using ColorSettings = ABI::Windows::UI::ViewManagement::IUISettings3;
    bool initialized_;
    Settings* settings_ = nullptr;
    ColorSettings* colors_ = nullptr;
};

inline constexpr wchar_t kPreferencesPath[] = L"Software\\TOTP-flyout";
inline constexpr wchar_t kPreviousPreferencesPath[] = L"Software\\2fa-flyout";
enum class ThemeMode : unsigned char { System, Light, Dark };

struct alignas(8) FlyoutPreferences {
    ThemeMode theme = ThemeMode::System;
    bool showCodes = false;
    bool showCountdown = true;
    bool animateCopy = true;
    unsigned char lockMinutes = 5;
    unsigned char clipboardSeconds = 30;
    bool allowScreenshots : 1 = false;
    bool hideAfterCopy : 1 = false;
    bool russian : 1 = false;

    bool operator==(const FlyoutPreferences&) const = default;
};

inline constexpr std::array<unsigned char, 4> kLockMinutes{1, 3, 5, 10};
inline constexpr std::array<unsigned char, 3> kClipboardSeconds{5, 15, 30};

inline unsigned char LockMinutes(int value) {
    return std::ranges::find(kLockMinutes, value) != kLockMinutes.end() ?
        static_cast<unsigned char>(value) : FlyoutPreferences{}.lockMinutes;
}

inline unsigned char ClipboardSeconds(int value) {
    return std::ranges::find(kClipboardSeconds, value) != kClipboardSeconds.end() ?
        static_cast<unsigned char>(value) : FlyoutPreferences{}.clipboardSeconds;
}

inline LSTATUS ReadPreferenceValue(PCWSTR name, DWORD flags, void* value, DWORD* size) {
    const auto capacity = *size;
    const auto result = RegGetValueW(HKEY_CURRENT_USER, kPreferencesPath, name, flags, nullptr, value, size);
    if (result != ERROR_FILE_NOT_FOUND) {
        return result;
    }
    *size = capacity;
    return RegGetValueW(HKEY_CURRENT_USER, kPreviousPreferencesPath, name, flags, nullptr, value, size);
}

inline DWORD ReadPreference(PCWSTR name, DWORD fallback) {
    DWORD value;
    DWORD size = sizeof(value);
    return ReadPreferenceValue(name, RRF_RT_REG_DWORD, &value, &size) == ERROR_SUCCESS ? value : fallback;
}

inline bool WritePreference(PCWSTR name, DWORD value) {
    return RegSetKeyValueW(HKEY_CURRENT_USER, kPreferencesPath, name, REG_DWORD,
                           &value, sizeof(value)) == ERROR_SUCCESS;
}

inline ThemeMode LoadThemeMode() {
    const auto mode = ReadPreference(L"Theme", 0);
    return mode <= static_cast<DWORD>(ThemeMode::Dark) ? static_cast<ThemeMode>(mode) : ThemeMode::System;
}

inline FlyoutPreferences LoadPreferences() {
    return {LoadThemeMode(), ReadPreference(L"ShowCodes", 0) == 1,
            ReadPreference(L"HideCountdown", 0) != 1, ReadPreference(L"ReduceCopyMotion", 0) != 1,
            LockMinutes(ReadPreference(L"AutoLockMinutes", 5)),
            ClipboardSeconds(ReadPreference(L"ClipboardSeconds", 30)),
            ReadPreference(L"AllowScreenshots", 0) == 1, ReadPreference(L"HideAfterCopy", 0) == 1,
            ReadPreference(L"Russian", 0) == 1};
}

inline bool SavePreferences(const FlyoutPreferences& previous, const FlyoutPreferences& updated) {
    const auto save = [](PCWSTR key, DWORD before, DWORD after) {
        return before == after || WritePreference(key, after);
    };
    bool saved = save(L"Theme", static_cast<DWORD>(previous.theme), static_cast<DWORD>(updated.theme));
    saved &= save(L"ShowCodes", previous.showCodes, updated.showCodes);
    saved &= save(L"HideCountdown", !previous.showCountdown, !updated.showCountdown);
    saved &= save(L"ReduceCopyMotion", !previous.animateCopy, !updated.animateCopy);
    saved &= save(L"AutoLockMinutes", previous.lockMinutes, updated.lockMinutes);
    saved &= save(L"ClipboardSeconds", previous.clipboardSeconds, updated.clipboardSeconds);
    saved &= save(L"AllowScreenshots", previous.allowScreenshots, updated.allowScreenshots);
    saved &= save(L"HideAfterCopy", previous.hideAfterCopy, updated.hideAfterCopy);
    saved &= save(L"Russian", previous.russian, updated.russian);
    return saved;
}

struct Palette {
    COLORREF surface;
    COLORREF card;
    COLORREF button;
    COLORREF selected;
    COLORREF hover;
    COLORREF pressed;
    COLORREF stroke;
    COLORREF text;
    COLORREF muted;
    COLORREF accent;
    COLORREF selectionText;
    COLORREF buttonText;
    COLORREF disabledText;
    COLORREF focusOuter;
    COLORREF focusInner;
    bool dark;
    bool highContrast;

    bool operator==(const Palette&) const = default;
};

inline HFONT CreateUiFont(int height, int weight = FW_NORMAL, PCWSTR family = L"Segoe UI Variable Text") {
    return CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                       DEFAULT_PITCH, family);
}

inline COLORREF BlendColor(COLORREF from, COLORREF to, int percent) {
    const auto mix = [percent](BYTE first, BYTE second) {
        return (first * (100 - percent) + second * percent) / 100;
    };
    return RGB(mix(GetRValue(from), GetRValue(to)), mix(GetGValue(from), GetGValue(to)),
               mix(GetBValue(from), GetBValue(to)));
}

inline double Luminance(COLORREF color) {
    const auto linear = [](BYTE component) {
        const auto srgb = component / 255.0;
        return srgb <= 0.04045 ? srgb / 12.92 : std::pow((srgb + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(GetRValue(color)) + 0.7152 * linear(GetGValue(color)) +
           0.0722 * linear(GetBValue(color));
}

inline double ColorContrast(COLORREF first, COLORREF second) {
    const auto [dark, light] = (std::minmax)({Luminance(first), Luminance(second)});
    return (light + 0.05) / (dark + 0.05);
}

inline COLORREF AccountTint(COLORREF background, COLORREF accent, COLORREF text, int strength) {
    const auto tinted = BlendColor(background, accent, strength);
    return ColorContrast(tinted, text) >= 4.5 ? tinted : background;
}

inline bool WindowsHighContrast() {
    HIGHCONTRASTW contrast{sizeof(contrast), 0, nullptr};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
           (contrast.dwFlags & HCF_HIGHCONTRASTON);
}

inline bool ReadWindowsLightMode(PCWSTR name, bool fallback) {
    DWORD light = fallback;
    DWORD size = sizeof(light);
    return RegGetValueW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                        name, RRF_RT_REG_DWORD, nullptr, &light, &size) == ERROR_SUCCESS ?
           light != 0 : fallback;
}

inline Palette WindowsPalette(ThemeMode mode, const WindowsTextSize& settings) {
    using namespace ABI::Windows::UI::ViewManagement;
    if (WindowsHighContrast()) {
        const auto surface = GetSysColor(COLOR_WINDOW);
        const auto text = GetSysColor(COLOR_WINDOWTEXT);
        const auto highlight = GetSysColor(COLOR_HIGHLIGHT);
        return {surface, surface, GetSysColor(COLOR_BTNFACE), highlight, highlight, highlight,
                text, text, text, highlight, GetSysColor(COLOR_HIGHLIGHTTEXT),
                GetSysColor(COLOR_BTNTEXT), GetSysColor(COLOR_GRAYTEXT), text, surface,
                false, true};
    }

    constexpr auto white = RGB(255, 255, 255);
    constexpr auto black = RGB(0, 0, 0);
    const auto systemText = settings.Color(UIColorType_Foreground);
    const auto systemBackground = settings.Color(UIColorType_Background);
    const bool systemDark = systemText ? Luminance(*systemText) > 0.5 :
                                       !ReadWindowsLightMode(L"AppsUseLightTheme", true);
    const bool dark = mode == ThemeMode::Dark || (mode == ThemeMode::System && systemDark);
    auto background = dark ? black : white;
    auto foreground = dark ? white : black;
    if (dark == systemDark && systemText && systemBackground &&
        ColorContrast(*systemText, *systemBackground) >= 12) {
        background = *systemBackground;
        foreground = *systemText;
    }

    // Opaque Win32 surfaces use the Windows foreground and background as their neutral endpoints.
    const auto neutral = [=](int strength) { return BlendColor(background, foreground, strength); };
    const auto surface = neutral(dark ? 12 : 5);
    const auto card = neutral(dark ? 17 : 2);
    const auto button = neutral(dark ? 19 : 1);
    const auto hover = neutral(dark ? 21 : 6);
    const auto pressed = neutral(dark ? 24 : 9);
    const auto text = neutral(dark ? 98 : 90);
    const auto muted = neutral(dark ? 76 : 68);
    const auto accentFallback = dark ? RGB(96, 205, 255) : RGB(0, 103, 192);
    auto accent = accentFallback;
    const auto accentBase = settings.Color(UIColorType_Accent);
    for (const auto shade : {dark ? UIColorType_AccentLight2 : UIColorType_AccentDark1,
                             dark ? UIColorType_AccentLight3 : UIColorType_AccentDark2}) {
        const auto candidate = settings.Color(shade).value_or(accentBase.value_or(accentFallback));
        const auto selected = BlendColor(card, candidate, 10);
        if (ColorContrast(candidate, pressed) >= 3 && ColorContrast(candidate, selected) >= 3) {
            accent = candidate;
            break;
        }
    }
    return {surface, card, button, BlendColor(card, accent, 10), hover, pressed,
            neutral(dark ? 26 : 18), text, muted, accent, text, text,
            neutral(dark ? 44 : 43), white, black, dark, false};
}

inline Palette WindowsPalette(ThemeMode mode) {
    const WindowsTextSize settings;
    return WindowsPalette(mode, settings);
}

inline COLORREF TagMarkerColor(std::size_t index, const Palette& palette, bool highlighted = false) {
    constexpr std::array light{RGB(0, 101, 180), RGB(157, 54, 105), RGB(122, 72, 157), RGB(0, 114, 104),
                               RGB(141, 82, 14), RGB(60, 111, 37), RGB(43, 91, 148), RGB(164, 62, 49)};
    constexpr std::array dark{RGB(112, 200, 255), RGB(246, 153, 205), RGB(203, 169, 250), RGB(98, 211, 194),
                              RGB(248, 188, 108), RGB(160, 216, 123), RGB(152, 194, 247), RGB(253, 155, 142)};
    if (palette.highContrast) {
        return highlighted ? palette.selectionText : palette.text;
    }
    return (palette.dark ? dark : light)[index % light.size()];
}

inline void FillSurface(HDC dc, const RECT& rect, COLORREF color) {
    if (rect.right <= rect.left || rect.bottom <= rect.top) {
        return;
    }
    const auto previous = SelectObject(dc, GetStockObject(DC_BRUSH));
    SetDCBrushColor(dc, color);
    PatBlt(dc, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, PATCOPY);
    SelectObject(dc, previous);
}

inline void PaintRounded(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border,
                         std::optional<COLORREF> gradientEnd = std::nullopt) {
    const auto width = rect.right - rect.left;
    const auto height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) {
        return;
    }
    if (PaintResources::Antialias() && width > 1 && height > 1 && radius > 0) {
        const auto color = [](COLORREF value) {
            return Gdiplus::Color(GetRValue(value), GetGValue(value), GetBValue(value));
        };
        Gdiplus::Graphics graphics(dc);
        if (graphics.GetLastStatus() == Gdiplus::Ok) {
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            const bool outlined = fill != border;
            const float inset = outlined ? 0.5f : 0.0f;
            const float x = rect.left + inset;
            const float y = rect.top + inset;
            const float right = rect.right - inset;
            const float bottom = rect.bottom - inset;
            const float diameter = (std::min)({2.0f * radius, right - x, bottom - y});
            Gdiplus::GraphicsPath path;
            path.AddArc(x, y, diameter, diameter, 180.0f, 90.0f);
            path.AddArc(right - diameter, y, diameter, diameter, 270.0f, 90.0f);
            path.AddArc(right - diameter, bottom - diameter, diameter, diameter, 0.0f, 90.0f);
            path.AddArc(x, bottom - diameter, diameter, diameter, 90.0f, 90.0f);
            path.CloseFigure();
            bool filled = false;
            if (gradientEnd && *gradientEnd != fill) {
                const Gdiplus::RectF bounds(x, y, right - x, bottom - y);
                const Gdiplus::LinearGradientBrush brush(bounds, color(fill), color(*gradientEnd),
                                                         Gdiplus::LinearGradientModeHorizontal);
                filled = graphics.FillPath(&brush, &path) == Gdiplus::Ok;
            }
            if (!filled) {
                const Gdiplus::SolidBrush brush(color(fill));
                graphics.FillPath(&brush, &path);
            }
            if (outlined) {
                const Gdiplus::Pen pen(color(border));
                graphics.DrawPath(&pen, &path);
            }
            return;
        }
    }
    const auto pen = SelectObject(dc, GetStockObject(DC_PEN));
    const auto brush = SelectObject(dc, GetStockObject(DC_BRUSH));
    SetDCPenColor(dc, border);
    SetDCBrushColor(dc, fill);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
    SelectObject(dc, brush);
    SelectObject(dc, pen);
}

inline void DrawLine(HDC dc, POINT from, POINT to, COLORREF color) {
    const auto previous = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCPenColor(dc, color);
    MoveToEx(dc, from.x, from.y, nullptr);
    LineTo(dc, to.x, to.y);
    SelectObject(dc, previous);
}

inline void PaintText(HDC dc, std::wstring_view text, RECT rect, HFONT font,
                      COLORREF color, UINT style, bool mnemonic = false) {
    const auto previous = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text.data(), static_cast<int>(text.size()), &rect, style | (mnemonic ? 0 : DT_NOPREFIX));
    SelectObject(dc, previous);
}

inline HICON CreateTrayKeyIcon() {
    constexpr int size = 32;
    const auto ink = WindowsHighContrast() ? GetSysColor(COLOR_BTNTEXT) :
        ReadWindowsLightMode(L"SystemUsesLightTheme", false) ? RGB(25, 25, 25) : RGB(246, 246, 246);

    BITMAPINFO bitmap{};
    bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap.bmiHeader.biWidth = size;
    bitmap.bmiHeader.biHeight = -size;
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    bitmap.bmiHeader.biCompression = BI_RGB;
    void* data = nullptr;
    const auto color = CreateDIBSection(nullptr, &bitmap, DIB_RGB_COLORS, &data, nullptr, 0);
    const BYTE maskBits[size * size / 8]{};
    const auto mask = CreateBitmap(size, size, 1, 1, maskBits);
    const auto dc = CreateCompatibleDC(nullptr);
    const auto font = CreateFontW(-26, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  0, 0, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe Fluent Icons");
    HICON icon = nullptr;
    if (color && mask && dc && font && data) {
        const auto previous = SelectObject(dc, color);
        FillSurface(dc, {0, 0, size, size}, RGB(0, 0, 0));
        PaintText(dc, L"\ue8d7", {0, 0, size, size}, font, RGB(255, 255, 255),
                  DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        GdiFlush();
        auto pixels = static_cast<DWORD*>(data);
        for (int i = 0; i < size * size; ++i) {
            const DWORD alpha = pixels[i] & 0xff;
            pixels[i] = (alpha << 24) | ((GetRValue(ink) * alpha / 255) << 16) |
                        ((GetGValue(ink) * alpha / 255) << 8) | (GetBValue(ink) * alpha / 255);
        }
        SelectObject(dc, previous);
        ICONINFO info{TRUE, 0, 0, mask, color};
        icon = CreateIconIndirect(&info);
    }
    DeleteObject(font);
    DeleteDC(dc);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

inline void ApplyWindowFrame(HWND window, const Palette& palette) {
    const BOOL dark = palette.dark;
    const auto preference = palette.highContrast ? DWMWCP_DONOTROUND : DWMWCP_ROUND;
    const COLORREF border = palette.highContrast ? DWMWA_COLOR_DEFAULT : palette.stroke;
    DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DwmSetWindowAttribute(window, DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof(preference));
    DwmSetWindowAttribute(window, DWMWA_BORDER_COLOR, &border, sizeof(border));
}

inline void PaintFocus(HDC dc, RECT bounds, int thickness, COLORREF outside, COLORREF inside) {
    for (const auto color : {outside, inside}) {
        const RECT top{bounds.left, bounds.top, bounds.right, bounds.top + thickness};
        const RECT bottom{bounds.left, bounds.bottom - thickness, bounds.right, bounds.bottom};
        const RECT left{bounds.left, top.bottom, bounds.left + thickness, bottom.top};
        const RECT right{bounds.right - thickness, top.bottom, bounds.right, bottom.top};
        for (const auto& edge : {top, bottom, left, right}) {
            FillSurface(dc, edge, color);
        }
        InflateRect(&bounds, -thickness, -thickness);
    }
}

inline bool UseClientAnimations() {
    BOOL enabled = FALSE;
    return SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0) && enabled;
}

class InteractionFade {
public:
    int Value(ULONGLONG now) const {
        if (!Running(now)) {
            return to_;
        }
        const auto remaining = 1.0 - static_cast<double>(now - started_) / duration_;
        return to_ + static_cast<int>(std::lround((from_ - to_) * remaining * remaining));
    }

    void Set(int target, ULONGLONG now, bool animate, unsigned duration = 83) {
        if (target == to_ && animate) {
            return;
        }
        from_ = animate ? Value(now) : target;
        to_ = target;
        started_ = now;
        duration_ = duration;
    }

    bool Running(ULONGLONG now) const { return from_ != to_ && now - started_ < duration_; }

private:
    int from_ = 0;
    int to_ = 0;
    ULONGLONG started_ = 0;
    unsigned duration_ = 83;
};

}  // namespace twofa
