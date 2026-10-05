#include "core/IniFile.h"

#include <fstream>
#include <sstream>

#include "core/StringUtil.h"

namespace em {

IniFile IniFile::parse(std::string_view text) {
    IniFile file;
    for (const auto rawLine : splitLines(text)) {
        const std::string_view line = trim(rawLine);
        if (line.empty() || line.front() == '#' || line.front() == ';') continue;

        const auto separator = line.find('=');
        if (separator == std::string_view::npos) continue;

        const std::string_view key = trim(line.substr(0, separator));
        if (key.empty()) continue;

        file.values_.insert_or_assign(std::string{key},
                                      std::string{trim(line.substr(separator + 1))});
    }
    return file;
}

std::string IniFile::serialize() const {
    std::string out;
    out += "# emeron preferences. Window layout lives in imgui.ini instead.\n";
    for (const auto& [key, value] : values_) {
        out += key;
        out += " = ";
        out += value;
        out += '\n';
    }
    return out;
}

Result<IniFile> IniFile::load(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return IniFile{};  // first run

    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        return makeError(ErrorKind::Io, "cannot read " + path.string());
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return parse(buffer.str());
}

Status IniFile::save(const std::filesystem::path& path) const {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    if (!stream) {
        return makeError(ErrorKind::Io, "cannot write " + path.string());
    }
    stream << serialize();
    if (!stream) {
        return makeError(ErrorKind::Io, "write failed for " + path.string());
    }
    return {};
}

void IniFile::set(std::string_view key, std::string_view value) {
    values_.insert_or_assign(std::string{key}, std::string{value});
}

void IniFile::setBool(std::string_view key, bool value) {
    set(key, value ? "true" : "false");
}

void IniFile::setInt(std::string_view key, long long value) {
    set(key, std::to_string(value));
}

std::optional<std::string_view> IniFile::get(std::string_view key) const {
    const auto it = values_.find(key);
    if (it == values_.end()) return std::nullopt;
    return std::string_view{it->second};
}

std::string IniFile::getOr(std::string_view key, std::string_view fallback) const {
    const auto value = get(key);
    return std::string{value.value_or(fallback)};
}

bool IniFile::getBool(std::string_view key, bool fallback) const {
    const auto value = get(key);
    if (!value) return fallback;

    const std::string lowered = toLower(*value);
    if (lowered == "true" || lowered == "1" || lowered == "yes" || lowered == "on") {
        return true;
    }
    if (lowered == "false" || lowered == "0" || lowered == "no" || lowered == "off") {
        return false;
    }
    return fallback;
}

long long IniFile::getInt(std::string_view key, long long fallback) const {
    const auto value = get(key);
    if (!value) return fallback;
    return parseNumber<long long>(*value).value_or(fallback);
}

bool IniFile::contains(std::string_view key) const {
    return values_.find(key) != values_.end();
}

void IniFile::remove(std::string_view key) {
    const auto it = values_.find(key);
    if (it != values_.end()) values_.erase(it);
}

}  // namespace em
