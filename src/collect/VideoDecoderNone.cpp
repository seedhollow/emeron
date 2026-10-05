#include "collect/VideoDecoder.h"

namespace em {

bool VideoDecoder::available() { return false; }

std::string VideoDecoder::unavailableReason() {
    return "this build has no H.264 decoder: install FFmpeg (macOS: brew install ffmpeg; "
           "Debian/Ubuntu: apt install libavcodec-dev libswscale-dev) and re-run CMake";
}

Result<std::unique_ptr<VideoDecoder>> VideoDecoder::createH264() {
    return makeError(ErrorKind::Unsupported, unavailableReason());
}

}  // namespace em
