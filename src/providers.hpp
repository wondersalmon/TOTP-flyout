#pragma once

#include "accounts.hpp"

namespace twofa {

inline std::wstring ProviderMonogram(std::wstring_view issuer) {
    issuer = TrimName(issuer);
    constexpr std::pair<std::wstring_view, std::wstring_view> known[] = {
        {L"GitHub", L"GH"}, {L"github.com", L"GH"},
        {L"GitLab", L"GL"}, {L"gitlab.com", L"GL"},
        {L"Discord", L"DC"}, {L"discord.com", L"DC"},
    };
    for (const auto& [name, letters] : known) {
        if (SameName(issuer, name)) {
            return std::wstring(letters);
        }
    }

    std::wstring letters;
    bool nextWord = false;
    for (const auto character : issuer) {
        WORD type{};
        if ((character >= 0xd800 && character <= 0xdfff) || !GetStringTypeW(CT_CTYPE1, &character, 1, &type)) {
            continue;
        }
        if (type & (C1_ALPHA | C1_DIGIT)) {
            if (nextWord) {
                letters.resize(1);
                letters.push_back(character);
                break;
            }
            if (letters.size() < 2) {
                letters.push_back(character);
            }
        } else if (!letters.empty() && (type & (C1_SPACE | C1_PUNCT))) {
            nextWord = true;
        }
    }

    wchar_t upper[2]{};
    if (!letters.empty() && LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, letters.data(),
            static_cast<int>(letters.size()), upper, ARRAYSIZE(upper), nullptr, nullptr, 0) ==
            static_cast<int>(letters.size())) {
        letters.assign(upper, letters.size());
    }
    return letters;
}

}  // namespace twofa
