#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app/Application.h"
#include "core/Log.h"
#include "core/StringUtil.h"

namespace {

void printUsage() {
    std::puts(
        "emeron - host-side Android inspector\n"
        "\n"
        "Usage: emeron [options]\n"
        "\n"
        "Options:\n"
        "  --adb <path>              Use this adb binary instead of searching\n"
        "  --trace-processor <path>   Use this trace_processor_shell binary\n"
        "  --workspace <dir>          Where traces, pulled files and the layout live\n"
        "                             (default: ~/.emeron)\n"
        "  --log-level <level>        trace | debug | info | warn | error (default: info)\n"
        "  --no-viewports             Keep all panels inside the main window\n"
        "  --no-vsync                 Do not cap the UI frame rate\n"
        "  --screenshot <file.png>    Save the main window to a PNG and quit\n"
        "  --screenshot-delay <sec>   Seconds to wait before the screenshot (default 4)\n"
        "  --package <name>           Target this app from the start\n"
        "  --show-panel <name>        Bring a panel's tab to the front (repeatable),\n"
        "                             e.g. --show-panel Sensors\n"
        "  -h, --help                 Show this message\n"
        "\n"
        "Environment:\n"
        "  EMERON_ADB                 Same as --adb\n"
        "  EMERON_TRACE_PROCESSOR     Same as --trace-processor\n"
        "  ANDROID_HOME               Searched for platform-tools/adb\n");
}

std::optional<em::LogLevel> parseLogLevel(std::string_view text) {
    const std::string lowered = em::toLower(text);
    if (lowered == "trace") return em::LogLevel::Trace;
    if (lowered == "debug") return em::LogLevel::Debug;
    if (lowered == "info") return em::LogLevel::Info;
    if (lowered == "warn" || lowered == "warning") return em::LogLevel::Warn;
    if (lowered == "error") return em::LogLevel::Error;
    return std::nullopt;
}

}  // namespace

int main(int argc, char** argv) {
    em::Application::Config config;

    const std::vector<std::string_view> args{argv + 1, argv + argc};
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        const auto next = [&]() -> std::string_view {
            return i + 1 < args.size() ? args[++i] : std::string_view{};
        };

        if (arg == "-h" || arg == "--help") {
            printUsage();
            return 0;
        }
        if (arg == "--adb") {
            if (const auto value = next(); !value.empty()) config.adbPath = std::string{value};
            continue;
        }
        if (arg == "--trace-processor") {
            if (const auto value = next(); !value.empty()) {
                config.traceProcessorPath = std::string{value};
            }
            continue;
        }
        if (arg == "--workspace") {
            if (const auto value = next(); !value.empty()) {
                config.workspaceDir = std::string{value};
            }
            continue;
        }
        if (arg == "--log-level") {
            const auto value = next();
            if (const auto level = parseLogLevel(value)) {
                em::Logger::instance().setMinLevel(*level);
            } else {
                std::fprintf(stderr, "emeron: unknown log level '%.*s'\n",
                             static_cast<int>(value.size()), value.data());
                return 2;
            }
            continue;
        }
        if (arg == "--no-viewports") {
            config.enableViewports = false;
            continue;
        }
        if (arg == "--screenshot") {
            if (const auto value = next(); !value.empty()) {
                config.screenshotPath = std::string{value};
            }
            continue;
        }
        if (arg == "--screenshot-delay") {
            const auto seconds = em::parseNumber<double>(next());
            if (!seconds || *seconds < 0.0) {
                std::fprintf(stderr, "emeron: --screenshot-delay needs a number of seconds\n");
                return 2;
            }
            config.screenshotDelaySeconds = *seconds;
            continue;
        }
        if (arg == "--package") {
            if (const auto value = next(); !value.empty()) {
                config.initialPackage = std::string{value};
            }
            continue;
        }
        if (arg == "--show-panel") {
            if (const auto value = next(); !value.empty()) {
                config.showPanels.emplace_back(value);
            }
            continue;
        }
        if (arg == "--no-vsync") {
            config.enableVsync = false;
            continue;
        }

        std::fprintf(stderr, "emeron: unknown option '%.*s'\n",
                     static_cast<int>(arg.size()), arg.data());
        printUsage();
        return 2;
    }

    em::Application app{std::move(config)};
    if (auto status = app.initialize(); !status) {
        std::fprintf(stderr, "emeron: startup failed: %s\n", status.describe().c_str());
        return 1;
    }
    return app.run();
}
