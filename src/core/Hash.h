#pragma once

// Small, dependency-free digests and encodings for certificate fingerprints.
// Not for anything security-sensitive: emeron only displays these.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace em {

[[nodiscard]] std::array<std::uint8_t, 20> sha1(std::string_view data);
[[nodiscard]] std::array<std::uint8_t, 32> sha256(std::string_view data);

// Lower-case hex, optionally with a separator between bytes ("ab:cd:...").
template <std::size_t N>
[[nodiscard]] std::string toHex(const std::array<std::uint8_t, N>& bytes, char separator = '\0');
[[nodiscard]] std::string toHex(std::string_view bytes, char separator = '\0');

// Standard alphabet; whitespace is skipped (adb may add CR/LF). nullopt on any
// other non-alphabet character.
[[nodiscard]] std::optional<std::string> base64Decode(std::string_view text);

template <std::size_t N>
std::string toHex(const std::array<std::uint8_t, N>& bytes, char separator) {
    return toHex(std::string_view{reinterpret_cast<const char*>(bytes.data()), N}, separator);
}

}  // namespace em
