#pragma once

// Flat key/value preferences file.
//
// emeron's window layout is already persisted by ImGui into imgui.ini. This
// holds the handful of settings ImGui knows nothing about -- currently the
// theme -- in <workspace>/emeron.ini.
//
// Deliberately not a real INI parser: no sections, no quoting, no escapes.
// Lines are `key = value`, `#` and `;` start a comment, and anything
// unparseable is skipped rather than being an error, so a hand-edited or
// partly-written file still loads.

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "core/Result.h"

namespace em {

class IniFile {
public:
    // A missing file is not an error: it yields an empty IniFile, which is the
    // correct state for a first run.
    [[nodiscard]] static Result<IniFile> load(const std::filesystem::path& path);

    [[nodiscard]] Status save(const std::filesystem::path& path) const;

    void set(std::string_view key, std::string_view value);
    void setBool(std::string_view key, bool value);
    void setInt(std::string_view key, long long value);

    [[nodiscard]] std::optional<std::string_view> get(std::string_view key) const;
    [[nodiscard]] std::string getOr(std::string_view key, std::string_view fallback) const;
    [[nodiscard]] bool getBool(std::string_view key, bool fallback) const;
    [[nodiscard]] long long getInt(std::string_view key, long long fallback) const;

    [[nodiscard]] bool contains(std::string_view key) const;
    void remove(std::string_view key);

    [[nodiscard]] bool empty() const noexcept { return values_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return values_.size(); }

    // Exposed so the format can be unit tested without touching the disk.
    [[nodiscard]] static IniFile parse(std::string_view text);
    [[nodiscard]] std::string serialize() const;

private:
    // Ordered, so a written file has a stable key order and diffs cleanly.
    std::map<std::string, std::string, std::less<>> values_;
};

}  // namespace em
