#pragma once

#include <stdexcept>
#include <string>

namespace omega {

inline std::string asset_basename(const std::wstring& input) {
    if (input.empty() || input.size() > 255 || input == L"." || input == L"..")
        throw std::runtime_error("Invalid asset name");
    std::string result;
    for (const auto character : input) {
        if (character < 32 || character >= 127 || character == L'/' || character == L'\\' || character == L':')
            throw std::runtime_error("Asset names must be printable ASCII basenames");
        result.push_back(static_cast<char>(character));
    }
    return result;
}

inline std::string uppercase_asset(std::string name) {
    for (auto& character : name) {
        if (character >= 'a' && character <= 'z') character = static_cast<char>(character - 'a' + 'A');
    }
    return name;
}

}
