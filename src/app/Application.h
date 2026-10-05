#pragma once

// Window, ImGui context, dockspace and the frame loop.
//
// Owns every collector; AppContext hands panels references to them. Nothing in
// here talks to a device directly: the collectors have their own threads and
// the panels queue work on the pool.

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/Result.h"

struct GLFWwindow;

namespace em {

class PanelRegistry;

class Application {
public:
    struct Config {
        int windowWidth = 1680;
        int windowHeight = 1020;
        std::string title = "emeron - Android Inspector";
        std::filesystem::path workspaceDir;  // empty -> ~/.emeron
        std::optional<std::filesystem::path> adbPath;
        std::optional<std::filesystem::path> traceProcessorPath;
        bool enableViewports = true;
        bool enableVsync = true;

        // Development aid: save the main window to a PNG after this many
        // seconds, then quit. See app/Screenshot.h.
        std::optional<std::filesystem::path> screenshotPath;
        double screenshotDelaySeconds = 4.0;

        // Development aid: drive the UI with synthetic input and save
        // screenshots along the way. See app/InputScript.h.
        std::optional<std::filesystem::path> scriptPath;

        // Package to target from the start, as if picked in the toolbar.
        std::optional<std::string> initialPackage;

        // Panels (by name) to bring to the front of their dock at startup.
        std::vector<std::string> showPanels;
    };

    explicit Application(Config config);
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    Application(Application&&) = delete;
    Application& operator=(Application&&) = delete;

    [[nodiscard]] Status initialize();
    int run();

    [[nodiscard]] static std::filesystem::path defaultWorkspaceDir();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace em
