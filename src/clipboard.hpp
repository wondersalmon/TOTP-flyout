#pragma once

#include "backup.hpp"

#include <algorithm>
#include <cstring>
#include <cwchar>

namespace twofa {

inline DWORD ClipboardLifetime(unsigned secondsRemaining, unsigned millisecondsIntoSecond,
                               unsigned maximumSeconds = 30) {
    const auto remaining = (std::min)(secondsRemaining, 31u) * 1000;
    const auto elapsed = (std::min)(millisecondsIntoSecond, 999u);
    return remaining ? (std::min)(remaining - elapsed, (std::min)(maximumSeconds, 30u) * 1000) : 0;
}

inline std::optional<backup::Plaintext> EncodeClipboardSetupText(std::span<const wchar_t> buffer) {
    const auto bound = buffer.first((std::min)(buffer.size(), kMaxLinkFileSize + 1));
    const auto end = std::ranges::find(bound, L'\0');
    if (end == bound.end() || end == bound.begin()) {
        return std::nullopt;
    }
    const auto characters = static_cast<int>(end - bound.begin());
    const auto bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, bound.data(), characters,
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0 || static_cast<std::size_t>(bytes) > kMaxLinkFileSize) {
        return std::nullopt;
    }
    backup::Plaintext result;
    result.bytes.resize(bytes);
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, bound.data(), characters,
                           reinterpret_cast<char*>(result.bytes.data()), bytes, nullptr, nullptr) != bytes) {
        return std::nullopt;
    }
    return result;
}

inline std::optional<backup::Plaintext> ReadClipboardSetupText(HWND owner) {
    if (!OpenClipboard(owner)) {
        return std::nullopt;
    }
    struct ClipboardScope {
        ~ClipboardScope() { CloseClipboard(); }
    } clipboard;
    const auto handle = GetClipboardData(CF_UNICODETEXT);
    const auto text = handle ? static_cast<const wchar_t*>(GlobalLock(handle)) : nullptr;
    if (!text) {
        return std::nullopt;
    }
    struct ClipboardTextLock {
        HGLOBAL handle;
        ~ClipboardTextLock() { GlobalUnlock(handle); }
    } lock{handle};
    return EncodeClipboardSetupText({text, GlobalSize(handle) / sizeof(wchar_t)});
}

class ClipboardBlock {
public:
    ClipboardBlock(const void* data, std::size_t count) : handle_(GlobalAlloc(GMEM_MOVEABLE, count)) {
        if (handle_) {
            if (const auto bytes = GlobalLock(handle_)) {
                std::memcpy(bytes, data, count);
                GlobalUnlock(handle_);
            } else {
                GlobalFree(handle_);
                handle_ = nullptr;
            }
        }
    }
    ClipboardBlock(const ClipboardBlock&) = delete;
    ClipboardBlock& operator=(const ClipboardBlock&) = delete;
    ~ClipboardBlock() {
        if (handle_) {
            if (const auto bytes = GlobalLock(handle_)) {
                SecureZeroMemory(bytes, GlobalSize(handle_));
                GlobalUnlock(handle_);
            }
            GlobalFree(handle_);
        }
    }
    explicit operator bool() const { return handle_ != nullptr; }
    bool Publish(UINT format) {
        if (!SetClipboardData(format, handle_)) {
            return false;
        }
        handle_ = nullptr;
        return true;
    }

private:
    HGLOBAL handle_;
};

class CodeClipboard {
public:
    bool Copy(HWND window, const TotpCode& code, DWORD lifetime) {
        if (!lifetime) {
            return false;
        }
        const auto exclusion = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
        constexpr DWORD marker = 0;
        ClipboardBlock privacy(&marker, sizeof(marker));
        ClipboardBlock text(code.text.data(), (wcslen(code.text.data()) + 1) * sizeof(wchar_t));
        if (!exclusion || !privacy || !text || !OpenClipboard(window)) {
            return false;
        }
        // Publish the exclusion before the code so a failed exclusion cannot leak it to history.
        const bool copied = EmptyClipboard() && privacy.Publish(exclusion) && text.Publish(CF_UNICODETEXT);
        CloseClipboard();
        if (copied) {
            // Windows may finalize formats and advance the sequence number when the clipboard is closed.
            sequence_ = GetClipboardSequenceNumber();
            expires_ = GetTickCount64() + lifetime;
        }
        return copied;
    }

    void Clear(HWND window, bool now = false) {
        if (!sequence_) {
            return;
        }
        if (now) {
            expires_ = 0;
        }
        if (GetTickCount64() < expires_) {
            return;
        }
        if (GetClipboardOwner() != window || GetClipboardSequenceNumber() != *sequence_) {
            sequence_.reset();
            return;
        }
        if (OpenClipboard(window)) {
            const bool changed = GetClipboardOwner() != window || GetClipboardSequenceNumber() != *sequence_;
            const bool done = changed || EmptyClipboard();
            CloseClipboard();
            if (done) {
                sequence_.reset();
            }
        }
    }

private:
    std::optional<DWORD> sequence_;
    ULONGLONG expires_ = 0;
};

}  // namespace twofa
