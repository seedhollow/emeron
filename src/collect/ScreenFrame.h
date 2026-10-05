#pragma once

#include <cstdint>
#include <vector>

namespace em {

// One picture of the device screen, ready to upload as a texture.
struct ScreenFrame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;  // width * height * 4, top row first, opaque
    std::uint64_t sequence = 0;
};

}  // namespace em
