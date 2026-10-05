#pragma once

// The bundle of services every panel draws from.
//
// Held by reference: Application owns the lifetimes and outlives every panel.
// Panels must treat the collectors as read-mostly and never block -- anything
// that talks to a device goes through `pool`, with the result posted back
// through `dispatcher`.

#include <filesystem>
#include <string>
#include <vector>

namespace em {

class DeviceManager;
class MetricSampler;
class FrameStatsCollector;
class LogcatReader;
class DeviceInfoCollector;
class SensorReader;
class FileSystemBrowser;
class AppInspector;
class PerfettoCapture;
class TraceProcessor;
class ThreadPool;
class Dispatcher;
class IniFile;

struct AppContext {
    DeviceManager& devices;
    MetricSampler& metrics;
    FrameStatsCollector& frames;
    LogcatReader& logcat;
    DeviceInfoCollector& deviceInfo;
    SensorReader& sensors;
    FileSystemBrowser& files;
    AppInspector& apps;
    PerfettoCapture& perfetto;
    TraceProcessor& traceProcessor;

    ThreadPool& pool;
    Dispatcher& dispatcher;

    // Persisted to <workspace>/emeron.ini on exit and on theme change. Panels
    // keep small conveniences here, e.g. the last download folder.
    IniFile& prefs;

    // Where pulled files and captured traces are written.
    std::filesystem::path workspaceDir;

    // emeron's own UI frame time, so the tool can be held to its own standard.
    double uiFrameMs = 0.0;
    double uiFps = 0.0;

    // Set by Application when the selected serial changes this frame.
    bool deviceChangedThisFrame = false;
    std::string selectedSerial;
    std::string selectedPackage;

    // Files dropped onto any emeron window from the OS this frame. A panel that
    // acts on them sets droppedPathsConsumed; Application logs the drop as
    // ignored otherwise, so it never vanishes silently.
    std::vector<std::filesystem::path> droppedPaths;
    bool droppedPathsConsumed = false;

    [[nodiscard]] bool hasDevice() const noexcept { return !selectedSerial.empty(); }
    [[nodiscard]] bool hasPackage() const noexcept { return !selectedPackage.empty(); }
};

}  // namespace em
