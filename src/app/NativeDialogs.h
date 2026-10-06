#pragma once

// The operating system's own file dialogs, via nativefiledialog-extended:
// NSSavePanel/NSOpenPanel on macOS, IFileDialog on Windows, GTK or the XDG
// desktop portal on Linux.
//
// Built only when EMERON_HAS_NATIVE_DIALOGS is defined (see
// cmake/Dependencies.cmake -- it is optional on Linux). Without it every call
// reports Outcome::Unavailable and callers fall back to an in-app prompt.
//
// THREADING: main thread only, and outside an ImGui frame. The dialogs are
// blocking and Cocoa requires the main thread. Callers post the request to the
// Dispatcher, which Application drains before NewFrame -- so the dialog never
// opens half way through drawing a window.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

struct GLFWwindow;

namespace em::dialogs {

enum class Outcome : std::uint8_t {
    Chosen,
    Cancelled,
    Unavailable,  // not built in, or failed to initialise at startup
    Failed,       // the dialog itself reported an error
};

struct Choice {
    Outcome outcome = Outcome::Unavailable;
    std::filesystem::path path;
    std::string error;

    [[nodiscard]] bool chosen() const noexcept { return outcome == Outcome::Chosen; }
};

// `parent` makes the dialogs modal to the main window where the platform
// supports it. Safe to call when native dialogs are not built in.
void initialize(GLFWwindow* parent);
void shutdown();

[[nodiscard]] bool available() noexcept;

[[nodiscard]] Choice pickFolder(std::string_view title, const std::filesystem::path& startIn);

// Every backend asks before replacing an existing file, so a Chosen path that
// already exists is one the user agreed to overwrite.
// `extensions` is a comma-separated list without dots, e.g. "apk,dex"; empty
// for any file. `filterName` labels it in the dialog.
[[nodiscard]] Choice openFile(std::string_view title, const std::filesystem::path& startIn,
                              std::string_view filterName, std::string_view extensions);

[[nodiscard]] Choice saveFile(std::string_view title, const std::filesystem::path& startIn,
                              std::string_view suggestedName);

}  // namespace em::dialogs
