#pragma once

// Minimal PNG encoder: 8-bit RGBA, no filtering, deflate "stored" blocks (i.e.
// uncompressed). Larger files than a real encoder makes, but it is ~100 lines
// with no dependency, and its only job is screenshots for development and bug
// reports. Any PNG reader decodes it.

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include "core/Result.h"

namespace em {

// `rgba` is width*height*4 bytes, top row first.
[[nodiscard]] std::vector<std::uint8_t> encodePng(std::span<const std::uint8_t> rgba,
                                                  std::uint32_t width, std::uint32_t height);

[[nodiscard]] Status writePng(const std::filesystem::path& file,
                              std::span<const std::uint8_t> rgba, std::uint32_t width,
                              std::uint32_t height);

// Exposed for tests.
[[nodiscard]] std::uint32_t crc32(std::span<const std::uint8_t> bytes,
                                  std::uint32_t seed = 0) noexcept;

}  // namespace em
