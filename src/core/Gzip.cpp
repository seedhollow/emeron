#include "core/Gzip.h"

#include <cstdint>

#include <miniz.h>

namespace em {
namespace {

std::uint32_t le32(std::string_view b, std::size_t at) noexcept {
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at])) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at + 1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at + 2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at + 3])) << 24);
}

}  // namespace

bool looksLikeGzip(std::string_view data) noexcept {
    return data.size() >= 18 && static_cast<std::uint8_t>(data[0]) == 0x1f &&
           static_cast<std::uint8_t>(data[1]) == 0x8b && data[2] == 8;
}

Result<std::string> gunzip(std::string_view data) {
    if (!looksLikeGzip(data)) return makeError(ErrorKind::ParseFailure, "not gzip data");

    // Header: ID1 ID2 CM FLG MTIME(4) XFL OS, then the optional fields FLG
    // announces.
    constexpr std::uint8_t kFhcrc = 0x02;
    constexpr std::uint8_t kFextra = 0x04;
    constexpr std::uint8_t kFname = 0x08;
    constexpr std::uint8_t kFcomment = 0x10;
    const auto flags = static_cast<std::uint8_t>(data[3]);
    std::size_t at = 10;
    if (flags & kFextra) {
        if (at + 2 > data.size()) return makeError(ErrorKind::ParseFailure, "truncated gzip header");
        at += 2 + (static_cast<std::size_t>(static_cast<std::uint8_t>(data[at])) |
                   (static_cast<std::size_t>(static_cast<std::uint8_t>(data[at + 1])) << 8));
    }
    for (const std::uint8_t field : {kFname, kFcomment}) {
        if (!(flags & field)) continue;
        while (at < data.size() && data[at] != '\0') ++at;
        ++at;  // the terminating NUL
    }
    if (flags & kFhcrc) at += 2;
    if (at + 8 > data.size()) return makeError(ErrorKind::ParseFailure, "truncated gzip stream");

    const std::uint32_t expectedCrc = le32(data, data.size() - 8);
    const std::uint32_t expectedSize = le32(data, data.size() - 4);  // modulo 2^32

    std::string out(expectedSize, '\0');
    const std::string_view deflate = data.substr(at, data.size() - 8 - at);
    const std::size_t written = tinfl_decompress_mem_to_mem(
        out.data(), out.size(), deflate.data(), deflate.size(),
        TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (written == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
        return makeError(ErrorKind::ParseFailure, "corrupt or truncated gzip data");
    }
    if (written != expectedSize) {
        return makeError(ErrorKind::ParseFailure, "gzip length mismatch");
    }
    const auto crc = static_cast<std::uint32_t>(
        mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(out.data()), out.size()));
    if (crc != expectedCrc) return makeError(ErrorKind::ParseFailure, "gzip CRC mismatch");
    return out;
}

}  // namespace em
