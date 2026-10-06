#include "app/Application.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <system_error>

#include <imgui.h>
#include <imgui_internal.h>  // DockBuilder, for the first-run layout
#include <implot.h>

#include <IconsFontAwesome6.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

// GLFW must come after the OpenGL loader that imgui_impl_opengl3 pulls in.
#include <GLFW/glfw3.h>

#include "adb/AdbCliTransport.h"
#include "core/CrashHandler.h"
#include "adb/DeviceManager.h"
#include "app/AboutDialog.h"
#include "app/AppContext.h"
#include "app/Fonts.h"
#include "app/InputScript.h"
#include "app/Screenshot.h"
#include "app/NativeDialogs.h"
#include "app/Theme.h"
#include "collect/DeviceInfo.h"
#include "collect/AppInspector.h"
#include "collect/ScreenMirror.h"
#include "collect/LayoutInspector.h"
#include "collect/FileSystemBrowser.h"
#include "collect/FrameStats.h"
#include "collect/LogcatReader.h"
#include "collect/MetricSampler.h"
#include "collect/PerfettoCapture.h"
#include "collect/SensorReader.h"
#include "collect/TraceProcessor.h"
#include "core/HttpClient.h"
#include "core/IniFile.h"
#include "core/Log.h"
#include "core/StringUtil.h"
#include "core/Subprocess.h"
#include "core/TaskQueue.h"
#include "ui/Panel.h"
#include "ui/Notifications.h"
#include "ui/PanelRegistry.h"
#include "ui/panels/DashboardPanel.h"
#include "ui/panels/DeviceInfoPanel.h"
#include "ui/panels/DeviceToolbar.h"
#include "ui/panels/FileExplorerPanel.h"
#include "ui/panels/FrameTimePanel.h"
#include "ui/panels/LogPanel.h"
#include "ui/panels/LogcatPanel.h"
#include "ui/panels/CrashLogPanel.h"
#include "ui/panels/AppInspectorPanel.h"
#include "ui/panels/ScreenMirrorPanel.h"
#include "ui/panels/LayoutInspectorPanel.h"
#include "ui/panels/PerfettoPanel.h"
#include "ui/panels/SensorPanel.h"
#include "ui/Widgets.h"

namespace em {
namespace embedded {
// vendor/scrcpy's server, embedded by CMakeLists.txt (cmake/EmbedFile.cmake).
extern const unsigned char kScrcpyServer[];
extern const std::size_t kScrcpyServerSize;
}  // namespace embedded
namespace {

constexpr const char* kCategory = "app";
constexpr const char* kDockspaceName = "EmeronDockspace";

void glfwErrorCallback(int code, const char* description) {
    EM_LOG_ERROR(kCategory, "glfw error " + std::to_string(code) + ": " +
                                (description != nullptr ? description : "?"));
}

// OS drag-and-drop arrives through a plain C callback with no user pointer we
// can safely claim (imgui_impl_glfw notes the window user pointer is shared),
// so drops are queued here and handed to the frame. The callback fires inside
// glfwPollEvents on the main thread; the mutex is for safety, not contention.
std::mutex gDropMutex;
std::vector<std::filesystem::path> gDroppedPaths;

std::filesystem::path pathFromUtf8(const char* utf8) {
    // GLFW hands over UTF-8. Going through u8string keeps non-ASCII names
    // intact on Windows, where a narrow string would be read as ANSI.
    const std::u8string text{reinterpret_cast<const char8_t*>(utf8)};
    return std::filesystem::path{text};
}

void onFilesDropped(GLFWwindow* /*window*/, int count, const char** paths) {
    const std::lock_guard lock{gDropMutex};
    for (int i = 0; i < count; ++i) {
        if (paths[i] != nullptr) gDroppedPaths.push_back(pathFromUtf8(paths[i]));
    }
}

std::vector<std::filesystem::path> takeDroppedPaths() {
    const std::lock_guard lock{gDropMutex};
    return std::exchange(gDroppedPaths, {});
}

// With multi-viewport on, a panel dragged out of the main window lives in its
// own OS window, created by the ImGui backend. Wrapping the backend's create
// hook gives those windows the drop callback too, so dropping onto an undocked
// Device Files panel works the same as onto the main window.
void (*gBackendCreateWindow)(ImGuiViewport*) = nullptr;

void createWindowWithDrop(ImGuiViewport* viewport) {
    gBackendCreateWindow(viewport);
    if (auto* window = static_cast<GLFWwindow*>(viewport->PlatformHandle)) {
        glfwSetDropCallback(window, onFilesDropped);
    }
}

// SIGINT (Ctrl+C in a terminal) and SIGTERM used to kill the process outright,
// skipping shutdown -- so every streaming child (adb logcat, perfetto, the
// trace_processor daemon) was left running, orphaned in its own process group
// and holding the device connection. Now they request a normal quit; a second
// signal forces an exit for the case where shutdown itself is stuck.
volatile std::sig_atomic_t gStopRequested = 0;

extern "C" void onStopSignal(int signal) {
    if (gStopRequested != 0) {
        // Only async-signal-safe calls are allowed here.
        std::_Exit(128 + signal);
    }
    gStopRequested = 1;
}

void installStopSignalHandlers() {
    std::signal(SIGINT, onStopSignal);
#if !defined(_WIN32)
    std::signal(SIGTERM, onStopSignal);
#endif
}

// Brings a docked window's tab to the front of its tab bar. Returns false
// while the window or its tab bar does not exist yet.
bool selectDockTab(const char* windowName) {
    ImGuiWindow* window = ImGui::FindWindowByName(windowName);
    if (window == nullptr || window->DockNode == nullptr || window->DockNode->TabBar == nullptr) {
        return false;
    }
    ImGuiTabBar* tabBar = window->DockNode->TabBar;
    ImGuiTabItem* tab = ImGui::TabBarFindTabByID(tabBar, window->TabId);
    if (tab == nullptr) return false;
    ImGui::TabBarQueueFocus(tabBar, tab);
    return true;
}

std::filesystem::path homeDirectory() {
#if defined(_WIN32)
    if (const char* profile = std::getenv("USERPROFILE")) return std::filesystem::path{profile};
#endif
    if (const char* home = std::getenv("HOME")) return std::filesystem::path{home};
    return std::filesystem::current_path();
}

}  // namespace

struct Application::Impl {
    explicit Impl(Config cfg) : config(std::move(cfg)) {}

    Config config;
    GLFWwindow* window = nullptr;
    std::string iniPath;
    std::string glslVersion = "#version 150";

    // --- services ---------------------------------------------------------
    std::shared_ptr<AdbCliTransport> transport;
    std::shared_ptr<AdbClient> adb;
    std::unique_ptr<DeviceManager> devices;
    std::unique_ptr<MetricSampler> metrics;
    std::unique_ptr<FrameStatsCollector> frames;
    std::unique_ptr<LogcatReader> logcat;
    std::unique_ptr<DeviceInfoCollector> deviceInfo;
    std::unique_ptr<SensorReader> sensors;
    std::unique_ptr<FileSystemBrowser> files;
    std::unique_ptr<AppInspector> apps;
    std::unique_ptr<ScreenMirror> mirror;
    std::unique_ptr<LayoutInspector> layout;
    std::unique_ptr<PerfettoCapture> perfetto;
    std::unique_ptr<TraceProcessor> traceProcessor;
    std::unique_ptr<ThreadPool> pool;
    std::unique_ptr<Dispatcher> dispatcher;
    std::unique_ptr<PanelRegistry> panels;
    DeviceToolbar toolbar;

    IniFile prefs;
    std::string prefsPath;
    theme::Mode themeMode = theme::Mode::Dark;

    std::string previousSerial;
    // Devices as last announced, serial -> state, for the connect/disconnect
    // toasts. The first list after start-up is recorded silently.
    std::map<std::string, DeviceState> announcedDevices;
    std::uint64_t announcedRevision = 0;
    bool devicesSeeded = false;
    void announceDeviceChanges();
    void announceNewCrashReport();
    bool layoutBuilt = false;
    int frontTabsCountdown = 0;
    // True only for a freshly built layout, whose own tab choice gets
    // corrected; a saved layout keeps the tabs the user left open.
    bool selectDefaultTabs = false;
    bool showImGuiDemo = false;
    bool showImPlotDemo = false;
    bool showAbout = false;
    bool quitRequested = false;

    // --script: the steps, where we are, and what the current step waits for.
    std::vector<ScriptStep> script;
    std::size_t scriptAt = 0;
    double scriptResumeAt = 0.0;
    ImVec2 scriptMouse{-FLT_MAX, -FLT_MAX};
    std::optional<std::filesystem::path> scriptShot;  // taken after this frame renders
    std::string scriptPanel;                          // brought to the front this frame

    [[nodiscard]] Status initWindow();
    [[nodiscard]] Status loadScript();
    void runScript();
    void loadPreferences();
    void savePreferences();
    void setTheme(theme::Mode next);
    void initImGui();
    void initServices();
    void registerPanels();

    void drawFrame(AppContext& context);
    void drawMenuBar(AppContext& context);
    void drawTopToolbar(AppContext& context);
    void drawStatusBar(AppContext& context);
    void buildDefaultLayout(ImGuiID dockspaceId);
    void shutdown(AppContext& context);
};

// ---------------------------------------------------------------------------

std::filesystem::path Application::defaultWorkspaceDir() {
    return homeDirectory() / ".emeron";
}

Application::Application(Config config) : impl_(std::make_unique<Impl>(std::move(config))) {}

Application::~Application() = default;

Status Application::initialize() {
    initProcessSubsystem();
    installStopSignalHandlers();
    initSocketSubsystem();

    if (impl_->config.workspaceDir.empty()) {
        impl_->config.workspaceDir = defaultWorkspaceDir();
    }
    std::error_code ec;
    std::filesystem::create_directories(impl_->config.workspaceDir, ec);
    std::filesystem::create_directories(impl_->config.workspaceDir / "traces", ec);
    std::filesystem::create_directories(impl_->config.workspaceDir / "pulled", ec);
    if (ec) {
        return makeError(ErrorKind::Io,
                         "cannot create workspace at " + impl_->config.workspaceDir.string());
    }
    crash::install(impl_->config.workspaceDir);

    impl_->loadPreferences();
    EM_TRY_VOID(impl_->loadScript());
    EM_TRY_VOID(impl_->initWindow());
    impl_->initImGui();
    impl_->initServices();
    impl_->registerPanels();
    impl_->announceNewCrashReport();
    return {};
}

Status Application::Impl::loadScript() {
    if (!config.scriptPath) return {};
    std::ifstream in{*config.scriptPath, std::ios::binary};
    if (!in) return makeError(ErrorKind::Io, "cannot read script " + config.scriptPath->string());
    std::ostringstream text;
    text << in.rdbuf();
    EM_TRY(steps, parseInputScript(text.str()));
    script = std::move(steps);
    // Shots land next to the script unless their path is absolute.
    for (auto& step : script) {
        if (step.kind == ScriptStep::Kind::Shot && std::filesystem::path{step.text}.is_relative()) {
            step.text = (config.scriptPath->parent_path() / step.text).string();
        }
    }
    return {};
}

// Feeds the script's input to ImGui. Called between the platform backend's
// NewFrame and ImGui::NewFrame, so the script's pointer wins over the real one.
void Application::Impl::runScript() {
    if (script.empty() || scriptShot) return;
    ImGuiIO& io = ImGui::GetIO();
    if (scriptMouse.x != -FLT_MAX) io.AddMousePosEvent(scriptMouse.x, scriptMouse.y);
    const double now = ImGui::GetTime();
    if (now < scriptResumeAt) return;
    while (scriptAt < script.size()) {
        const ScriptStep& step = script[scriptAt++];
        using Kind = ScriptStep::Kind;
        switch (step.kind) {
            case Kind::Wait: scriptResumeAt = now + step.seconds; return;
            case Kind::Frame: return;
            case Kind::MouseMove:
                scriptMouse = {step.x, step.y};
                io.AddMousePosEvent(step.x, step.y);
                break;
            case Kind::MouseButton: io.AddMouseButtonEvent(step.button, step.down); break;
            case Kind::Wheel: io.AddMouseWheelEvent(0.0F, step.y); break;
            case Kind::Key: io.AddKeyEvent(step.key, step.down); break;
            case Kind::Text: io.AddInputCharactersUTF8(step.text.c_str()); break;
            case Kind::Panel: scriptPanel = step.text; break;
            case Kind::Package: devices->selectPackage(step.text); break;
            case Kind::Shot: scriptShot = step.text; return;
            case Kind::Quit: quitRequested = true; return;
        }
    }
    quitRequested = true;
}

void Application::Impl::loadPreferences() {
    prefsPath = (config.workspaceDir / "emeron.ini").string();

    auto loaded = IniFile::load(prefsPath);
    if (!loaded) {
        EM_LOG_WARN(kCategory, "could not read preferences: " + loaded.describe());
    } else {
        prefs = std::move(loaded).value();
    }

    // An unrecognised or absent value falls back to dark rather than failing.
    const std::string stored = prefs.getOr("theme", "");
    themeMode = theme::parseMode(stored).value_or(theme::Mode::Dark);
    notify::setEnabled(prefs.getOr("notifications", "on") != "off");

    // Write the file back when it was missing or held something we did not
    // recognise. That makes the preferences discoverable and hand-editable on
    // a first run instead of only appearing after the first theme switch, and
    // it normalises a stale or mistyped value rather than silently ignoring it
    // on every launch.
    if (stored != theme::toString(themeMode)) savePreferences();
}

void Application::Impl::savePreferences() {
    prefs.set("theme", theme::toString(themeMode));
    prefs.set("notifications", notify::enabled() ? "on" : "off");
    if (auto saved = prefs.save(prefsPath); !saved) {
        EM_LOG_WARN(kCategory, "could not write preferences: " + saved.describe());
    }
}

namespace {
std::string deviceLabel(const DeviceRef& d) {
    std::string name = d.model.empty() ? d.serial : d.model;
    std::replace(name.begin(), name.end(), '_', ' ');  // adb reports "TECNO_LH8n"
    return name;
}
}  // namespace

void Application::Impl::announceDeviceChanges() {
    if (devices->devicesRevision() == announcedRevision) return;
    announcedRevision = devices->devicesRevision();
    const DeviceList list = devices->devices();
    // No adb server answer is not "every phone was unplugged".
    if (!list.serverReachable) return;

    std::map<std::string, DeviceState> now;
    for (const auto& d : list.devices) now[d.serial] = d.state;
    if (!devicesSeeded) {
        devicesSeeded = true;
        announcedDevices = std::move(now);
        return;
    }
    for (const auto& d : list.devices) {
        const auto before = announcedDevices.find(d.serial);
        const bool isNew = before == announcedDevices.end();
        if (!isNew && before->second == d.state) continue;
        switch (d.state) {
            case DeviceState::Online:
                notify::success(ICON_FA_MOBILE_SCREEN "  " + deviceLabel(d) + " connected", d.serial);
                break;
            case DeviceState::Unauthorized:
                notify::warning(deviceLabel(d) + " is waiting for permission",
                                "Unlock the phone and tap Allow on the USB debugging prompt.");
                break;
            case DeviceState::NoPermissions:
                notify::error(deviceLabel(d) + ": no permission",
                              "Linux needs a udev rule for this device before adb can use it.");
                break;
            case DeviceState::Offline:
                if (!isNew) notify::warning(deviceLabel(d) + " went offline", d.serial);
                break;
            default:
                break;
        }
    }
    for (const auto& [serial, state] : announcedDevices) {
        if (!now.contains(serial)) notify::info(ICON_FA_PLUG "  Device disconnected", serial);
    }
    announcedDevices = std::move(now);
}

void Application::Impl::announceNewCrashReport() {
    const auto reports = crash::listReports();
    if (reports.empty()) return;
    // Once per report: the newest one already announced is remembered.
    const std::string newest = reports.front().fileName;
    if (prefs.getOr("crash_seen", "") == newest) return;
    prefs.set("crash_seen", newest);
    savePreferences();
    notify::postWithAction(notify::Kind::Warning, "emeron crashed last time",
                           "A report was saved: " + newest, ICON_FA_BOMB "  Open Crash Logs", [this] {
                               if (Panel* panel = panels->find(CrashLogPanel::kId)) {
                                   panel->setVisible(true);
                                   ImGui::SetWindowFocus(Panel::windowKey(CrashLogPanel::kId).c_str());
                               }
                           });
}

void Application::Impl::setTheme(theme::Mode next) {
    if (next == themeMode) return;
    themeMode = next;
    theme::apply(themeMode);
    savePreferences();
    EM_LOG_INFO(kCategory, std::string{"theme: "} + std::string{theme::toString(themeMode)});
}

Status Application::Impl::initWindow() {
    glfwSetErrorCallback(glfwErrorCallback);
    if (glfwInit() == GLFW_FALSE) {
        return makeError(ErrorKind::Io,
                         "glfwInit failed. On Linux make sure a display is available "
                         "and libgl/libx11 (or wayland) development packages are installed.");
    }

#if defined(__APPLE__)
    // macOS caps core-profile OpenGL at 4.1 and requires forward compatibility.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glslVersion = "#version 150";
#else
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glslVersion = "#version 130";
#endif
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);

    window = glfwCreateWindow(config.windowWidth, config.windowHeight, config.title.c_str(),
                              nullptr, nullptr);
    if (window == nullptr) {
        glfwTerminate();
        return makeError(ErrorKind::Io, "could not create an OpenGL 3.2+ window");
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(config.enableVsync ? 1 : 0);

    dialogs::initialize(window);
    return {};
}

void Application::Impl::initImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    if (config.enableViewports) {
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    }
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.ConfigDockingWithShift = false;

    // Layout and window positions persist across runs next to the workspace.
    iniPath = (config.workspaceDir / "imgui.ini").string();
    io.IniFilename = iniPath.c_str();

    theme::apply(themeMode);
    fonts::loadUiFonts();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glslVersion.c_str());

    // After backend init: the backend installs its own callbacks, but not a
    // drop callback, so this does not displace anything it relies on.
    glfwSetDropCallback(window, onFilesDropped);
    if ((io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0) {
        ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
        gBackendCreateWindow = platformIo.Platform_CreateWindow;
        platformIo.Platform_CreateWindow = createWindowWithDrop;
    }

    // If no ini exists this is a first run, so the default layout is applied.
    std::error_code ec;
    layoutBuilt = std::filesystem::exists(iniPath, ec);
    // --show-panel has to work with a saved layout too, not only on a first
    // run -- it used to be silently ignored once imgui.ini existed.
    if (layoutBuilt && !config.showPanels.empty()) frontTabsCountdown = 10;
}

void Application::Impl::initServices() {
    pool = std::make_unique<ThreadPool>();
    dispatcher = std::make_unique<Dispatcher>();

    transport = std::make_shared<AdbCliTransport>(config.adbPath);
    adb = std::make_shared<AdbClient>(transport);

    devices = std::make_unique<DeviceManager>(adb);
    metrics = std::make_unique<MetricSampler>(*devices);
    frames = std::make_unique<FrameStatsCollector>(*devices);
    logcat = std::make_unique<LogcatReader>(*devices);
    deviceInfo = std::make_unique<DeviceInfoCollector>(*devices);
    sensors = std::make_unique<SensorReader>(*devices);
    files = std::make_unique<FileSystemBrowser>(*devices);
    apps = std::make_unique<AppInspector>(*devices);
    mirror = std::make_unique<ScreenMirror>(*devices);
    mirror->setScrcpyServer(std::string{reinterpret_cast<const char*>(embedded::kScrcpyServer),
                                        embedded::kScrcpyServerSize},
                            EMERON_SCRCPY_VERSION);
    layout = std::make_unique<LayoutInspector>(*devices);
    perfetto = std::make_unique<PerfettoCapture>(*devices);
    traceProcessor = std::make_unique<TraceProcessor>(config.traceProcessorPath);

    if (transport->available()) {
        // Starting the server here means the first device poll is not the one
        // that pays the daemon launch cost.
        pool->submit([this] {
            if (auto started = adb->startServer(); !started) {
                EM_LOG_WARN(kCategory, "adb start-server: " + started.describe());
            }
        });
    }

    // Set before the first device poll: auto-selecting the first device keeps
    // the package, whereas switching devices later clears it.
    if (config.initialPackage) devices->selectPackage(*config.initialPackage);

    devices->start();
    metrics->start();
    frames->start();
}

void Application::Impl::registerPanels() {
    panels = std::make_unique<PanelRegistry>();
    panels->add<DashboardPanel>();
    panels->add<FrameTimePanel>();
    panels->add<FileExplorerPanel>();
    panels->add<AppInspectorPanel>();
    panels->add<ScreenMirrorPanel>();
    panels->add<LayoutInspectorPanel>();
    panels->add<DeviceInfoPanel>();
    panels->add<SensorPanel>();
    panels->add<PerfettoPanel>();
    panels->add<LogcatPanel>();
    panels->add<LogPanel>();
    panels->add<CrashLogPanel>();
}

void Application::Impl::buildDefaultLayout(ImGuiID dockspaceId) {
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

    ImGuiID center = dockspaceId;
    const ImGuiID bottom =
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.30F, nullptr, &center);
    const ImGuiID left =
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.25F, nullptr, &center);
    const ImGuiID right =
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.30F, nullptr, &center);
    const ImGuiID centerLower =
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.45F, nullptr, &center);

    ImGui::DockBuilderDockWindow(Panel::windowKey(DashboardPanel::kId).c_str(), center);
    ImGui::DockBuilderDockWindow(Panel::windowKey(PerfettoPanel::kId).c_str(), center);
    ImGui::DockBuilderDockWindow(Panel::windowKey(AppInspectorPanel::kId).c_str(), center);
    ImGui::DockBuilderDockWindow(Panel::windowKey(LayoutInspectorPanel::kId).c_str(), center);
    ImGui::DockBuilderDockWindow(Panel::windowKey(FrameTimePanel::kId).c_str(), centerLower);
    ImGui::DockBuilderDockWindow(Panel::windowKey(FileExplorerPanel::kId).c_str(), left);
    ImGui::DockBuilderDockWindow(Panel::windowKey(DeviceInfoPanel::kId).c_str(), right);
    ImGui::DockBuilderDockWindow(Panel::windowKey(SensorPanel::kId).c_str(), right);
    ImGui::DockBuilderDockWindow(Panel::windowKey(ScreenMirrorPanel::kId).c_str(), right);
    ImGui::DockBuilderDockWindow(Panel::windowKey(LogcatPanel::kId).c_str(), bottom);
    ImGui::DockBuilderDockWindow(Panel::windowKey(LogPanel::kId).c_str(), bottom);
    ImGui::DockBuilderDockWindow(Panel::windowKey(CrashLogPanel::kId).c_str(), bottom);

    ImGui::DockBuilderFinish(dockspaceId);

    // Which tab ends up in front is not controlled by docking order. ImGui
    // keeps the *focused* window's tab in front, and each new window takes
    // focus as it is first created -- so the last panel submitted (Log) won its
    // tab bar. Traced frame by frame: selecting a tab alone was undone the next
    // frame. So the tabs are selected on the tab bar once it exists, and focus
    // is moved off Log in the same frame; see the countdown in drawFrame().
    frontTabsCountdown = 10;
    selectDefaultTabs = true;
    EM_LOG_INFO(kCategory, "applied the default dock layout");
}

void Application::Impl::drawMenuBar(AppContext& context) {
    if (!ImGui::BeginMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        // This used to log the path and nothing else.
        if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Open workspace folder")) {
            FileSystemBrowser::revealOnHost(*pool, context.workspaceDir);
        }
        ImGui::SetItemTooltip("%s", context.workspaceDir.string().c_str());
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_POWER_OFF "  Quit", "Ctrl+Q")) quitRequested = true;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Device")) {
        if (ImGui::MenuItem(ICON_FA_ARROWS_ROTATE "  Refresh device list", "Ctrl+R")) devices->requestRefresh();
        if (ImGui::MenuItem(ICON_FA_POWER_OFF "  Restart adb server")) {
            pool->submit([this] {
                (void)adb->killServer();
                if (auto started = adb->startServer(); !started) {
                    EM_LOG_ERROR(kCategory, "restart failed: " + started.describe());
                } else {
                    EM_LOG_INFO(kCategory, "adb server restarted");
                }
                devices->requestRefresh();
            });
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_MAGNIFYING_GLASS "  Re-detect adb binary")) {
            transport->relocate(config.adbPath);
            devices->requestRefresh();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        panels->drawViewMenu();
        ImGui::Separator();

        if (ImGui::BeginMenu(ICON_FA_PALETTE "  Theme")) {
            if (ImGui::MenuItem(ICON_FA_MOON "  Dark", nullptr, themeMode == theme::Mode::Dark)) {
                setTheme(theme::Mode::Dark);
            }
            if (ImGui::MenuItem(ICON_FA_SUN "  Light", nullptr, themeMode == theme::Mode::Light)) {
                setTheme(theme::Mode::Light);
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(ICON_FA_CIRCLE_HALF_STROKE "  Toggle light/dark", "Ctrl+T")) {
            setTheme(theme::other(themeMode));
        }

        if (bool on = notify::enabled();
            ImGui::MenuItem(ICON_FA_BELL "  Notifications", nullptr, &on)) {
            notify::setEnabled(on);
            savePreferences();
        }
        ImGui::SetItemTooltip("Pop-up messages in the corner: transfers finished, a phone "
                              "connected, an action failed.");

        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_TABLE_COLUMNS "  Reset layout")) layoutBuilt = false;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Sampling")) {
        auto intervalMs = static_cast<int>(metrics->interval().count());
        if (ImGui::SliderInt("Dashboard update every", &intervalMs, 100, 5000, "%d ms")) {
            metrics->setInterval(std::chrono::milliseconds{intervalMs});
        }
        widgets::termTooltip("Metrics interval",
                             "How often CPU, memory, battery and temperature are read. "
                             "Faster costs a little more device CPU.");
        auto historySec = static_cast<float>(metrics->historySeconds());
        if (ImGui::SliderFloat("Keep history for", &historySec, 30.0F, 1800.0F, "%.0f s")) {
            metrics->setHistorySeconds(static_cast<double>(historySec));
        }
        widgets::termTooltip("History length",
                             "How far back the dashboard charts can scroll.");
        auto frameIntervalMs = static_cast<int>(frames->interval().count());
        if (ImGui::SliderInt("Frame timing update every", &frameIntervalMs, 250, 5000,
                             "%d ms")) {
            frames->setInterval(std::chrono::milliseconds{frameIntervalMs});
        }
        widgets::termTooltip("Framestats interval  (dumpsys gfxinfo <pkg> framestats)",
                             "How often frame timings are fetched. Android only keeps the "
                             "last ~120 frames, so slow updates can miss frames.");
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_ROTATE_LEFT "  Reset metric history")) metrics->reset();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        ImGui::MenuItem(ICON_FA_WINDOW_MAXIMIZE "  Dear ImGui demo", nullptr, &showImGuiDemo);
        ImGui::MenuItem(ICON_FA_CHART_AREA "  ImPlot demo", nullptr, &showImPlotDemo);
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_CIRCLE_INFO "  About emeron")) showAbout = true;
        ImGui::Separator();
        ImGui::TextDisabled("adb: %s", transport->describe().c_str());
        ImGui::TextDisabled("workspace: %s", context.workspaceDir.string().c_str());
        ImGui::EndMenu();
    }

    // Right-aligned live readout of emeron's own frame cost.
    const std::string fps = format("%.1f FPS  %.2f ms", context.uiFps, context.uiFrameMs);
    const float textWidth = ImGui::CalcTextSize(fps.c_str()).x;
    ImGui::SameLine(ImGui::GetWindowWidth() - textWidth - 16.0F);
    ImGui::TextDisabled("%s", fps.c_str());

    ImGui::EndMenuBar();
}

void Application::Impl::drawTopToolbar(AppContext& context) {
    const float height = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2.0F;
    if (ImGui::BeginViewportSideBar(
            "##EmeronToolbar", ImGui::GetMainViewport(), ImGuiDir_Up, height,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollbar)) {
        toolbar.draw(context);
    }
    ImGui::End();
}

void Application::Impl::drawStatusBar(AppContext& context) {
    const float height = ImGui::GetFrameHeight();
    if (ImGui::BeginViewportSideBar(
            "##EmeronStatusBar", ImGui::GetMainViewport(), ImGuiDir_Down, height,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollbar)) {
        const DeviceList list = devices->devices();
        const auto& palette = theme::palette();

        if (!transport->available()) {
            ImGui::TextColored(palette.critical, ICON_FA_CIRCLE_XMARK " adb not found");
        } else if (!list.serverReachable) {
            ImGui::TextColored(palette.bad, ICON_FA_TRIANGLE_EXCLAMATION " adb server unreachable");
        } else {
            ImGui::TextColored(palette.good, ICON_FA_CIRCLE_CHECK " adb");
        }

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::Text(ICON_FA_MOBILE_SCREEN " %zu device%s", list.devices.size(),
                    list.devices.size() == 1 ? "" : "s");

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        if (context.hasDevice()) {
            ImGui::Text("%s", context.selectedSerial.c_str());
        } else {
            ImGui::TextDisabled("no device selected");
        }

        if (context.hasPackage()) {
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            ImGui::Text(ICON_FA_CUBE " %s", context.selectedPackage.c_str());
        }

        const std::size_t pending = pool->pendingCount();
        if (pending > 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            ImGui::TextColored(palette.warning, ICON_FA_HOURGLASS_HALF " %zu task%s queued", pending,
                               pending == 1 ? "" : "s");
        }

        if (!list.lastError.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            ImGui::TextColored(palette.bad, "%s", list.lastError.c_str());
        }
    }
    ImGui::End();
}

void Application::Impl::drawFrame(AppContext& context) {
    drawTopToolbar(context);
    drawStatusBar(context);

    // The host window is invisible; it exists only to own the dockspace so
    // panels can be docked against the viewport edges.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.0F, 0.0F});

    constexpr ImGuiWindowFlags kHostFlags =
        ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGui::Begin("##EmeronHost", nullptr, kHostFlags);
    ImGui::PopStyleVar(3);

    const ImGuiID dockspaceId = ImGui::GetID(kDockspaceName);
    ImGui::DockSpace(dockspaceId, ImVec2{0.0F, 0.0F}, ImGuiDockNodeFlags_PassthruCentralNode);

    if (!layoutBuilt) {
        buildDefaultLayout(dockspaceId);
        layoutBuilt = true;
    }

    drawMenuBar(context);
    ImGui::End();

    if (frontTabsCountdown > 0) {
        // Retried for a few frames: the tab bars are created a frame or two
        // after the layout is built. Device logcat over emeron's own log.
        bool done = !selectDefaultTabs ||
                    (selectDockTab(Panel::windowKey(LogcatPanel::kId).c_str()) &&
                     selectDockTab(Panel::windowKey(DashboardPanel::kId).c_str()));
        for (const auto& name : config.showPanels) {
            done = selectDockTab(Panel::windowKey(name).c_str()) && done;
        }
        // Focus decides the front tab of its own dock, so it goes to the last
        // panel asked for on the command line -- or Dashboard by default.
        if (done && (selectDefaultTabs || !config.showPanels.empty())) {
            const std::string focus =
                config.showPanels.empty() ? std::string{DashboardPanel::kId} : config.showPanels.back();
            ImGui::SetWindowFocus(Panel::windowKey(focus).c_str());
        }
        frontTabsCountdown = done ? 0 : frontTabsCountdown - 1;
        if (frontTabsCountdown == 0) selectDefaultTabs = false;
    }

    if (!scriptPanel.empty() && selectDockTab(Panel::windowKey(scriptPanel).c_str())) {
        ImGui::SetWindowFocus(Panel::windowKey(scriptPanel).c_str());
        scriptPanel.clear();
    }

    panels->drawAll(context);
    notify::render();

    if (showImGuiDemo) ImGui::ShowDemoWindow(&showImGuiDemo);
    if (showImPlotDemo) ImPlot::ShowDemoWindow(&showImPlotDemo);
    drawAboutDialog(showAbout);
}

void Application::Impl::shutdown(AppContext& context) {
    panels->shutdown(context);
    savePreferences();

    // Stop producers before consumers: the collector threads touch the adb
    // client, and the pool's tasks capture the collectors.
    perfetto->cancel();
    sensors->stopPolling();
    mirror->stop();
    logcat->stop();
    frames->stop();
    metrics->stop();
    devices->stop();
    traceProcessor->stopHttpd();
    // Not pool.reset(): AppContext holds a reference to it. The pool is
    // declared after the collectors in Impl, so its destructor joins the
    // worker threads before anything a queued task could have captured goes
    // away. Dropping queued work here is what matters.
    pool->drainPending();
    dispatcher->clear();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    dialogs::shutdown();
    if (window != nullptr) glfwDestroyWindow(window);
    glfwTerminate();
}

int Application::run() {
    Impl& impl = *impl_;

    AppContext context{
        .devices = *impl.devices,
        .metrics = *impl.metrics,
        .frames = *impl.frames,
        .logcat = *impl.logcat,
        .deviceInfo = *impl.deviceInfo,
        .sensors = *impl.sensors,
        .files = *impl.files,
        .apps = *impl.apps,
        .mirror = *impl.mirror,
        .layout = *impl.layout,
        .perfetto = *impl.perfetto,
        .traceProcessor = *impl.traceProcessor,
        .pool = *impl.pool,
        .dispatcher = *impl.dispatcher,
        .prefs = impl.prefs,
        .workspaceDir = impl.config.workspaceDir,
    };

    const auto startedAt = std::chrono::steady_clock::now();

    while (glfwWindowShouldClose(impl.window) == GLFW_FALSE && !impl.quitRequested) {
        glfwPollEvents();
        if (gStopRequested != 0) {
            EM_LOG_INFO(kCategory, "stop signal received, shutting down");
            break;
        }

        // A minimised window still runs the loop; sleeping keeps it off the CPU.
        if (glfwGetWindowAttrib(impl.window, GLFW_ICONIFIED) != 0) {
            glfwWaitEventsTimeout(0.25);
            continue;
        }

        // Results from worker threads land here, on the UI thread, before any
        // panel reads the state they mutate.
        impl.dispatcher->drain();
        impl.announceDeviceChanges();

        const std::string serial = impl.devices->selectedSerial();
        context.deviceChangedThisFrame = serial != impl.previousSerial;
        if (context.deviceChangedThisFrame) impl.toolbar.onDeviceChanged();
        impl.previousSerial = serial;
        context.selectedSerial = serial;
        context.selectedPackage = impl.devices->selectedPackage();
        context.droppedPaths = takeDroppedPaths();
        context.droppedPathsConsumed = false;

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        impl.runScript();
        ImGui::NewFrame();

        const ImGuiIO& io = ImGui::GetIO();
        context.uiFrameMs = static_cast<double>(io.DeltaTime) * 1000.0;
        context.uiFps = static_cast<double>(io.Framerate);

        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_R)) {
            impl.devices->requestRefresh();
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Q)) {
            impl.quitRequested = true;
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_T)) {
            impl.setTheme(theme::other(impl.themeMode));
        }

        impl.drawFrame(context);

        if (!context.droppedPaths.empty() && !context.droppedPathsConsumed) {
            EM_LOG_WARN(kCategory,
                        std::to_string(context.droppedPaths.size()) +
                            " dropped file(s) ignored: open View > Device Files and select "
                            "a device to upload");
        }
        context.droppedPaths.clear();

        ImGui::Render();

        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(impl.window, &width, &height);
        glViewport(0, 0, width, height);
        const ImVec4 clear = theme::palette().windowClear;
        glClearColor(clear.x, clear.y, clear.z, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (impl.config.screenshotPath) {
            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - startedAt).count();
            if (elapsed >= impl.config.screenshotDelaySeconds) {
                if (auto saved = screenshot::captureBackBuffer(width, height,
                                                               *impl.config.screenshotPath);
                    !saved) {
                    EM_LOG_ERROR(kCategory, "screenshot failed: " + saved.describe());
                } else {
                    EM_LOG_INFO(kCategory, "screenshot saved to " +
                                               impl.config.screenshotPath->string());
                }
                impl.quitRequested = true;
            }
        }

        if (impl.scriptShot) {
            std::filesystem::create_directories(impl.scriptShot->parent_path());
            if (auto saved = screenshot::captureBackBuffer(width, height, *impl.scriptShot); !saved) {
                EM_LOG_ERROR(kCategory, "screenshot failed: " + saved.describe());
            } else {
                EM_LOG_INFO(kCategory, "screenshot saved to " + impl.scriptShot->string());
            }
            impl.scriptShot.reset();
        }

        if ((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0) {
            GLFWwindow* backup = glfwGetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            glfwMakeContextCurrent(backup);
        }

        glfwSwapBuffers(impl.window);
    }

    impl.shutdown(context);
    return 0;
}

}  // namespace em
