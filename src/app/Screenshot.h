#pragma once

// Writes the main window's framebuffer to a PNG. For development and bug
// reports (`emeron --screenshot out.png`): it reads emeron's own OpenGL back
// buffer, so unlike an OS screen capture it needs no screen-recording
// permission. Panels torn off into their own windows (multi-viewport) are not
// in the main framebuffer; pass --no-viewports for a complete picture.

#include <filesystem>

#include "core/Result.h"

namespace em::screenshot {

// Call after rendering the frame and before swapping buffers.
[[nodiscard]] Status captureBackBuffer(int width, int height, const std::filesystem::path& file);

}  // namespace em::screenshot
