#include "collect/VideoDecoder.h"

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace em {
namespace {

std::string avError(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

class FfmpegH264Decoder final : public VideoDecoder {
public:
    ~FfmpegH264Decoder() override {
        sws_freeContext(sws_);
        av_frame_free(&frame_);
        av_packet_free(&packet_);
        avcodec_free_context(&context_);
    }

    Status open() {
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (codec == nullptr) return makeError(ErrorKind::Unsupported, "FFmpeg has no H.264 decoder");
        context_ = avcodec_alloc_context3(codec);
        packet_ = av_packet_alloc();
        frame_ = av_frame_alloc();
        if (context_ == nullptr || packet_ == nullptr || frame_ == nullptr) {
            return makeError(ErrorKind::Io, "out of memory");
        }
        // A live view, not a file: show each picture as soon as it is
        // decoded. Frame threading would hold pictures back to decode several
        // at once; slice threading does not.
        context_->flags |= AV_CODEC_FLAG_LOW_DELAY;
        context_->thread_type = FF_THREAD_SLICE;
        context_->thread_count = 0;  // auto
        const int rc = avcodec_open2(context_, codec, nullptr);
        if (rc < 0) return makeError(ErrorKind::Io, "cannot open the H.264 decoder: " + avError(rc));
        return {};
    }

    Status decode(std::string_view data, std::int64_t pts,
                  const std::function<void(ScreenFrame&&)>& onFrame) override {
        // av_new_packet adds the zeroed padding FFmpeg's bitstream readers
        // are allowed to over-read.
        int rc = av_new_packet(packet_, static_cast<int>(data.size()));
        if (rc < 0) return makeError(ErrorKind::Io, "av_new_packet: " + avError(rc));
        std::memcpy(packet_->data, data.data(), data.size());
        packet_->pts = pts;
        rc = avcodec_send_packet(context_, packet_);
        av_packet_unref(packet_);
        if (rc < 0 && rc != AVERROR(EAGAIN)) {
            return makeError(ErrorKind::ParseFailure, "decode: " + avError(rc));
        }
        for (;;) {
            rc = avcodec_receive_frame(context_, frame_);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
            if (rc < 0) return makeError(ErrorKind::ParseFailure, "decode: " + avError(rc));
            convertAndEmit(onFrame);
            av_frame_unref(frame_);
        }
        return {};
    }

private:
    void convertAndEmit(const std::function<void(ScreenFrame&&)>& onFrame) {
        const int w = frame_->width;
        const int h = frame_->height;
        if (w <= 0 || h <= 0) return;
        // The size can change mid-stream (the device rotated): the cached
        // context is rebuilt when it does.
        sws_ = sws_getCachedContext(sws_, w, h, static_cast<AVPixelFormat>(frame_->format), w, h,
                                    AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (sws_ == nullptr) return;

        ScreenFrame out;
        out.width = static_cast<std::uint32_t>(w);
        out.height = static_cast<std::uint32_t>(h);
        out.rgba.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
        std::uint8_t* dst[4] = {out.rgba.data(), nullptr, nullptr, nullptr};
        int dstStride[4] = {w * 4, 0, 0, 0};
        sws_scale(sws_, frame_->data, frame_->linesize, 0, h, dst, dstStride);
        onFrame(std::move(out));
    }

    AVCodecContext* context_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;
    SwsContext* sws_ = nullptr;
};

}  // namespace

bool VideoDecoder::available() { return avcodec_find_decoder(AV_CODEC_ID_H264) != nullptr; }

std::string VideoDecoder::unavailableReason() {
    return available() ? std::string{} : "the linked FFmpeg was built without an H.264 decoder";
}

Result<std::unique_ptr<VideoDecoder>> VideoDecoder::createH264() {
    auto decoder = std::make_unique<FfmpegH264Decoder>();
    EM_TRY_VOID(decoder->open());
    return std::unique_ptr<VideoDecoder>{std::move(decoder)};
}

}  // namespace em
