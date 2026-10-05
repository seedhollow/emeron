#pragma once

// Decodes the scrcpy server's H.264 stream into RGBA frames.
//
// Implemented with FFmpeg (VideoDecoderFfmpeg.cpp) when the build found it,
// and as an always-unavailable stub (VideoDecoderNone.cpp) otherwise, so the
// rest of the code needs no #ifdefs.

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "collect/ScreenFrame.h"
#include "core/Result.h"

namespace em {

class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;

    // True when this build can decode H.264 at all.
    [[nodiscard]] static bool available();
    // Why not, for the UI, when available() is false.
    [[nodiscard]] static std::string unavailableReason();
    [[nodiscard]] static Result<std::unique_ptr<VideoDecoder>> createH264();

    // One access unit as the server sent it (config data already merged in
    // by the caller). Calls `onFrame` for every picture it completes.
    [[nodiscard]] virtual Status decode(std::string_view packet, std::int64_t pts,
                                        const std::function<void(ScreenFrame&&)>& onFrame) = 0;
};

}  // namespace em
