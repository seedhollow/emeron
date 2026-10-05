#pragma once

#include <string>
#include <string_view>

#include "core/Result.h"

namespace em {

[[nodiscard]] bool looksLikeGzip(std::string_view data) noexcept;

// Decompresses a single-member gzip stream (RFC 1952), checking the CRC-32 and
// the length in the trailer, so a truncated transfer is an error rather than a
// silently short result.
[[nodiscard]] Result<std::string> gunzip(std::string_view data);

}  // namespace em
