#include "app/Screenshot.h"

#include <cstdint>
#include <vector>

#include <GLFW/glfw3.h>  // brings in the platform GL 1.x declarations

#include "core/PngWriter.h"

namespace em::screenshot {

Status captureBackBuffer(int width, int height, const std::filesystem::path& file) {
    if (width <= 0 || height <= 0) return makeError(ErrorKind::Unsupported, "empty framebuffer");

    const auto w = static_cast<std::uint32_t>(width);
    const auto h = static_cast<std::uint32_t>(height);
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(w) * h * 4);

    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    // OpenGL's origin is bottom-left; PNG rows run top to bottom. The alpha
    // channel is forced opaque: the backbuffer's alpha is not meaningful.
    const std::size_t stride = static_cast<std::size_t>(w) * 4;
    std::vector<std::uint8_t> flipped(pixels.size());
    for (std::uint32_t y = 0; y < h; ++y) {
        const auto* src = pixels.data() + (h - 1 - y) * stride;
        auto* dst = flipped.data() + y * stride;
        for (std::size_t x = 0; x < stride; x += 4) {
            dst[x] = src[x];
            dst[x + 1] = src[x + 1];
            dst[x + 2] = src[x + 2];
            dst[x + 3] = 0xFF;
        }
    }
    return writePng(file, flipped, w, h);
}

}  // namespace em::screenshot
