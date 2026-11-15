#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace mxb::guid {
// beta21 player-info payload, built by 0x14005FF60 / 0x140095D70.
inline constexpr size_t kPayloadBytes = 684, kGuidOffset = 648, kGuidBytes = 9;
using Value = std::array<uint8_t, kGuidBytes>;
using Payload = std::array<std::byte, kPayloadBytes>;

inline bool Valid(const Value& value) {
    bool zero = true, sentinel = true;
    for (auto b : value) { zero &= b == 0; sentinel &= b == 100; }
    return !zero && !sentinel;
}
// The profile prints the most significant byte first; the wire stores it first
// at offset +8 (little endian). Short hex strings have implicit leading zeroes.
inline bool Parse(std::string_view text, Value& result) {
    if (text.empty() || text.size() > 18) return false;
    Value parsed{};
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[text.size() - 1 - i];
        const int n = c >= '0' && c <= '9' ? c - '0' :
                      c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                      c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (n < 0) return false;
        parsed[i / 2] |= static_cast<uint8_t>(n << (4 * (i % 2)));
    }
    if (!Valid(parsed)) return false;
    result = parsed;
    return true;
}
inline std::array<char, 19> Format(const Value& value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::array<char, 19> text{};
    for (size_t i = 0; i < kGuidBytes; ++i) {
        const auto b = value[kGuidBytes - 1 - i];
        text[2 * i] = hex[b >> 4]; text[2 * i + 1] = hex[b & 15];
    }
    return text;
}
// Caller verifies source readability. Never edits the original buffer.
inline bool Project(const void* source, size_t bytes, const Value& value, Payload& output) {
    if (!source || bytes != kPayloadBytes || !Valid(value)) return false;
    const auto* input = static_cast<const std::byte*>(source);
    if (std::memcmp(input + kGuidOffset, value.data(), kGuidBytes) == 0) return false;
    std::memcpy(output.data(), input, kPayloadBytes);
    std::memcpy(output.data() + kGuidOffset, value.data(), kGuidBytes);
    return true;
}
}
