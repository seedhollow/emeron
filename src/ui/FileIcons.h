#pragma once

// Font Awesome icon for a device file, by kind and then by extension.
// Header-only so the mapping can be unit tested without a UI.

#include <array>
#include <string_view>

#include <IconsFontAwesome6.h>

#include "collect/FileSystemBrowser.h"
#include "core/StringUtil.h"

namespace em {

namespace detail {

struct ExtensionIcon {
    std::string_view extension;
    const char* icon;
};

// Grouped by what a developer poking around a device is likely to look for.
inline constexpr std::array kExtensionIcons{
    // images and screenshots
    ExtensionIcon{"png", ICON_FA_FILE_IMAGE}, ExtensionIcon{"jpg", ICON_FA_FILE_IMAGE},
    ExtensionIcon{"jpeg", ICON_FA_FILE_IMAGE}, ExtensionIcon{"webp", ICON_FA_FILE_IMAGE},
    ExtensionIcon{"gif", ICON_FA_FILE_IMAGE}, ExtensionIcon{"heic", ICON_FA_FILE_IMAGE},
    ExtensionIcon{"bmp", ICON_FA_FILE_IMAGE},
    // screen recordings and media
    ExtensionIcon{"mp4", ICON_FA_FILE_VIDEO}, ExtensionIcon{"mkv", ICON_FA_FILE_VIDEO},
    ExtensionIcon{"webm", ICON_FA_FILE_VIDEO}, ExtensionIcon{"3gp", ICON_FA_FILE_VIDEO},
    ExtensionIcon{"mov", ICON_FA_FILE_VIDEO},
    ExtensionIcon{"mp3", ICON_FA_FILE_AUDIO}, ExtensionIcon{"ogg", ICON_FA_FILE_AUDIO},
    ExtensionIcon{"wav", ICON_FA_FILE_AUDIO}, ExtensionIcon{"flac", ICON_FA_FILE_AUDIO},
    ExtensionIcon{"m4a", ICON_FA_FILE_AUDIO}, ExtensionIcon{"opus", ICON_FA_FILE_AUDIO},
    // packages and archives
    ExtensionIcon{"apk", ICON_FA_FILE_ZIPPER}, ExtensionIcon{"aab", ICON_FA_FILE_ZIPPER},
    ExtensionIcon{"apex", ICON_FA_FILE_ZIPPER}, ExtensionIcon{"obb", ICON_FA_FILE_ZIPPER},
    ExtensionIcon{"jar", ICON_FA_FILE_ZIPPER}, ExtensionIcon{"zip", ICON_FA_FILE_ZIPPER},
    ExtensionIcon{"gz", ICON_FA_FILE_ZIPPER}, ExtensionIcon{"tgz", ICON_FA_FILE_ZIPPER},
    ExtensionIcon{"tar", ICON_FA_FILE_ZIPPER}, ExtensionIcon{"xz", ICON_FA_FILE_ZIPPER},
    ExtensionIcon{"7z", ICON_FA_FILE_ZIPPER},
    // text, logs, config
    ExtensionIcon{"txt", ICON_FA_FILE_LINES}, ExtensionIcon{"log", ICON_FA_FILE_LINES},
    ExtensionIcon{"md", ICON_FA_FILE_LINES}, ExtensionIcon{"csv", ICON_FA_FILE_LINES},
    ExtensionIcon{"json", ICON_FA_FILE_LINES}, ExtensionIcon{"xml", ICON_FA_FILE_LINES},
    ExtensionIcon{"prop", ICON_FA_FILE_LINES}, ExtensionIcon{"conf", ICON_FA_FILE_LINES},
    ExtensionIcon{"cfg", ICON_FA_FILE_LINES}, ExtensionIcon{"ini", ICON_FA_FILE_LINES},
    ExtensionIcon{"rc", ICON_FA_FILE_LINES},
    // scripts and source
    ExtensionIcon{"sh", ICON_FA_FILE_CODE}, ExtensionIcon{"py", ICON_FA_FILE_CODE},
    ExtensionIcon{"java", ICON_FA_FILE_CODE}, ExtensionIcon{"kt", ICON_FA_FILE_CODE},
    // compiled code: native libraries, dex and its ART forms
    ExtensionIcon{"so", ICON_FA_GEARS}, ExtensionIcon{"dex", ICON_FA_GEARS},
    ExtensionIcon{"odex", ICON_FA_GEARS}, ExtensionIcon{"vdex", ICON_FA_GEARS},
    ExtensionIcon{"oat", ICON_FA_GEARS}, ExtensionIcon{"art", ICON_FA_GEARS},
    // databases
    ExtensionIcon{"db", ICON_FA_DATABASE}, ExtensionIcon{"sqlite", ICON_FA_DATABASE},
    ExtensionIcon{"db-wal", ICON_FA_DATABASE}, ExtensionIcon{"db-shm", ICON_FA_DATABASE},
    // traces captured by emeron
    ExtensionIcon{"perfetto-trace", ICON_FA_WAVE_SQUARE},
    ExtensionIcon{"pftrace", ICON_FA_WAVE_SQUARE},
};

}  // namespace detail

[[nodiscard]] inline const char* fileIcon(FileKind kind, std::string_view name) {
    switch (kind) {
        case FileKind::Directory: return ICON_FA_FOLDER;
        case FileKind::Symlink:   return ICON_FA_LINK;
        case FileKind::Block:
        case FileKind::Character: return ICON_FA_MICROCHIP;
        case FileKind::Fifo:
        case FileKind::Socket:    return ICON_FA_PLUG;
        case FileKind::Regular:
        case FileKind::Unknown:   break;
    }

    // Last extension, case-insensitively. A leading dot marks a hidden file,
    // not an extension, so ".bashrc" is a plain file.
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= name.size()) return ICON_FA_FILE;

    // "trace.perfetto-trace" and "app.db-wal" have dashes, so match the whole tail.
    const std::string extension = toLower(name.substr(dot + 1));
    for (const auto& entry : detail::kExtensionIcons) {
        if (entry.extension == extension) return entry.icon;
    }
    return ICON_FA_FILE;
}

}  // namespace em
