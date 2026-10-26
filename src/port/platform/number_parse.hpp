#pragma once
// Port-only (132ps5c, research/PS5.md "Build"): std::from_chars for every arithmetic type. The PS5 SDK's libc++
// (ps5-payload-sdk v0.42) declares the floating-point overloads deleted; on the PS5 the general format is parsed with
// strtof / strtod (correctly rounded, as from_chars) after from_chars' own grammar checks (no leading whitespace or
// '+'). Every other build, and every integer, is std::from_chars itself (the PC and Vita builds are unchanged).
#include <charconv>
#include <system_error>
#include <type_traits>
#if defined(OMEGA_PS5)
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <string>
#endif

namespace omega {

template <class T>
std::from_chars_result from_chars_number(const char* first, const char* last, T& value) {
#if defined(OMEGA_PS5)
    if constexpr (std::is_floating_point_v<T>) {
        if (first == last || *first == '+' || std::isspace(static_cast<unsigned char>(*first)))
            return {first, std::errc::invalid_argument};
        const std::string text(first, last);
        char* end = nullptr;
        errno = 0;
        T parsed{};
        if constexpr (std::is_same_v<T, float>) parsed = std::strtof(text.c_str(), &end);
        else if constexpr (std::is_same_v<T, double>) parsed = std::strtod(text.c_str(), &end);
        else parsed = std::strtold(text.c_str(), &end);
        if (end == text.c_str()) return {first, std::errc::invalid_argument};
        const char* stop = first + (end - text.c_str());
        if (errno == ERANGE) return {stop, std::errc::result_out_of_range};
        value = parsed;
        return {stop, std::errc{}};
    } else
#endif
        return std::from_chars(first, last, value);
}

}  // namespace omega