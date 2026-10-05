#include "core/PngWriter.h"

#include <algorithm>
#include <array>
#include <fstream>

namespace em {
namespace {

constexpr std::array<std::uint32_t, 256> makeCrcTable() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t n = 0; n < 256; ++n) {
        std::uint32_t c = n;
        for (int k = 0; k < 8; ++k) c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1) : c >> 1;
        table[n] = c;
    }
    return table;
}

constexpr auto kCrcTable = makeCrcTable();

void putU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

void putChunk(std::vector<std::uint8_t>& out, const char (&type)[5],
              std::span<const std::uint8_t> data) {
    putU32(out, static_cast<std::uint32_t>(data.size()));
    const std::size_t typeStart = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    // The CRC covers the type and the data, not the length.
    putU32(out, crc32(std::span{out}.subspan(typeStart)));
}

}  // namespace

std::uint32_t crc32(std::span<const std::uint8_t> bytes, std::uint32_t seed) noexcept {
    std::uint32_t c = seed ^ 0xFFFFFFFFU;
    for (const std::uint8_t b : bytes) c = kCrcTable[(c ^ b) & 0xFFU] ^ (c >> 8);
    return c ^ 0xFFFFFFFFU;
}

std::vector<std::uint8_t> encodePng(std::span<const std::uint8_t> rgba, std::uint32_t width,
                                    std::uint32_t height) {
    const std::size_t stride = static_cast<std::size_t>(width) * 4;

    // Raw scanlines, each prefixed with filter type 0 (none).
    std::vector<std::uint8_t> raw;
    raw.reserve((stride + 1) * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        const auto row = rgba.subspan(y * stride, stride);
        raw.insert(raw.end(), row.begin(), row.end());
    }

    // zlib stream of deflate stored blocks (max 65535 bytes each).
    std::vector<std::uint8_t> zlib{0x78, 0x01};
    std::uint32_t adlerA = 1;
    std::uint32_t adlerB = 0;
    for (std::size_t offset = 0; offset < raw.size() || offset == 0;) {
        const std::size_t length = std::min<std::size_t>(65535, raw.size() - offset);
        const bool last = offset + length >= raw.size();
        zlib.push_back(last ? 1 : 0);
        const auto len16 = static_cast<std::uint16_t>(length);
        const auto nlen16 = static_cast<std::uint16_t>(~len16);
        zlib.push_back(static_cast<std::uint8_t>(len16 & 0xFFU));
        zlib.push_back(static_cast<std::uint8_t>(len16 >> 8));
        zlib.push_back(static_cast<std::uint8_t>(nlen16 & 0xFFU));
        zlib.push_back(static_cast<std::uint8_t>(nlen16 >> 8));
        for (std::size_t i = offset; i < offset + length; ++i) {
            zlib.push_back(raw[i]);
            adlerA = (adlerA + raw[i]) % 65521U;
            adlerB = (adlerB + adlerA) % 65521U;
        }
        offset += length;
        if (last) break;
    }
    putU32(zlib, (adlerB << 16) | adlerA);

    std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

    std::vector<std::uint8_t> header;
    putU32(header, width);
    putU32(header, height);
    header.insert(header.end(), {8, 6, 0, 0, 0});  // 8-bit, RGBA, deflate, no filter, no interlace

    putChunk(png, "IHDR", header);
    putChunk(png, "IDAT", zlib);
    putChunk(png, "IEND", {});
    return png;
}

Status writePng(const std::filesystem::path& file, std::span<const std::uint8_t> rgba,
                std::uint32_t width, std::uint32_t height) {
    if (rgba.size() < static_cast<std::size_t>(width) * height * 4) {
        return makeError(ErrorKind::Unsupported, "pixel buffer is smaller than the image");
    }
    const auto png = encodePng(rgba, width, height);

    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream stream{file, std::ios::binary | std::ios::trunc};
    if (!stream) return makeError(ErrorKind::Io, "cannot write " + file.string());
    stream.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    if (!stream) return makeError(ErrorKind::Io, "write failed for " + file.string());
    return {};
}

}  // namespace em
