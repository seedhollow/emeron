cmake_minimum_required(VERSION 3.25)

project(emeron
    VERSION 0.1.0
    DESCRIPTION "Host-side Android inspector: live metrics, jank analysis, file explorer, Perfetto"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(EMERON_BUILD_TESTS   "Build unit tests"                ON)
option(EMERON_ASAN          "Enable AddressSanitizer+UBSan"   OFF)
option(EMERON_TSAN          "Enable ThreadSanitizer"          OFF)
option(EMERON_WERROR        "Treat warnings as errors"        OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)
include(Dependencies)

# ---------------------------------------------------------------------------
# emeron_core : everything that does NOT depend on ImGui or a window.
# Kept separate so the parsers and the adb layer stay unit-testable headlessly.
# ---------------------------------------------------------------------------
add_library(emeron_core STATIC
    src/core/Log.cpp
    src/core/StringUtil.cpp
    src/core/Subprocess.cpp
    src/core/CrashHandler.cpp
    src/core/Hash.cpp
    src/core/Gzip.cpp
    src/core/TaskQueue.cpp
    src/core/HttpClient.cpp
    src/core/TcpSocket.cpp
    src/core/IniFile.cpp
    src/core/PngWriter.cpp

    src/adb/AdbCliTransport.cpp
    src/adb/AdbClient.cpp
    src/adb/DeviceManager.cpp

    src/collect/MetricSampler.cpp
    src/collect/FrameStats.cpp
    src/collect/LogcatReader.cpp
    src/collect/DeviceInfo.cpp
    src/collect/SensorReader.cpp
    src/collect/FileSystemBrowser.cpp
    src/collect/TraceProcessor.cpp
    src/collect/PerfettoCapture.cpp
    src/collect/AppInspector.cpp
    src/collect/ApkAnalysis.cpp
    src/collect/ScreenMirror.cpp
    src/collect/ScrcpyProtocol.cpp
    src/collect/LayoutInspector.cpp
)

target_include_directories(emeron_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(emeron_core PUBLIC Threads::Threads PRIVATE miniz)

# H.264 decoding for the fast screen mirror. Optional: without FFmpeg the
# mirror falls back to `screencap`, and everything else is unaffected.
if(EMERON_FFMPEG_FOUND)
    target_sources(emeron_core PRIVATE src/collect/VideoDecoderFfmpeg.cpp)
    target_link_libraries(emeron_core PRIVATE PkgConfig::EMERON_FFMPEG)
    target_compile_definitions(emeron_core PUBLIC EMERON_HAS_FFMPEG=1)
else()
    target_sources(emeron_core PRIVATE src/collect/VideoDecoderNone.cpp)
endif()
emeron_set_warnings(emeron_core)
emeron_set_sanitizers(emeron_core)

if(WIN32)
    target_link_libraries(emeron_core PUBLIC ws2_32 dbghelp)
    target_compile_definitions(emeron_core PUBLIC
        WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()

# ---------------------------------------------------------------------------
# emeron_assets : binary assets compiled into the executable.
#
# Font Awesome Free Solid (vendor/fontawesome) is embedded at build time so the
# icons work wherever the binary is run from, with no font file to ship beside
# it. IconsFontAwesome6.h supplies the matching ICON_FA_* names.
# ---------------------------------------------------------------------------
set(EMERON_FA_TTF "${CMAKE_CURRENT_SOURCE_DIR}/vendor/fontawesome/fa-solid-900.ttf")
set(EMERON_FA_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/fa_solid_900.cpp")
add_custom_command(
    OUTPUT  "${EMERON_FA_CPP}"
    COMMAND "${CMAKE_COMMAND}" -DINPUT=${EMERON_FA_TTF} -DOUTPUT=${EMERON_FA_CPP}
            -DSYMBOL=kFontAwesomeSolid -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake"
    DEPENDS "${EMERON_FA_TTF}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake"
    COMMENT "Embedding Font Awesome Solid"
    VERBATIM)

# The scrcpy server (vendor/scrcpy, Apache-2.0), pushed to the device by the
# fast screen mirror. Embedded for the same reason as the font: one binary,
# nothing to find at run time.
set(EMERON_SCRCPY_VERSION "3.3.4")
set(EMERON_SCRCPY_JAR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/scrcpy/scrcpy-server-v${EMERON_SCRCPY_VERSION}")
set(EMERON_SCRCPY_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/scrcpy_server.cpp")
add_custom_command(
    OUTPUT  "${EMERON_SCRCPY_CPP}"
    COMMAND "${CMAKE_COMMAND}" -DINPUT=${EMERON_SCRCPY_JAR} -DOUTPUT=${EMERON_SCRCPY_CPP}
            -DSYMBOL=kScrcpyServer -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake"
    DEPENDS "${EMERON_SCRCPY_JAR}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake"
    COMMENT "Embedding the scrcpy server"
    VERBATIM)

add_library(emeron_assets STATIC "${EMERON_FA_CPP}" "${EMERON_SCRCPY_CPP}")
target_compile_definitions(emeron_assets PUBLIC EMERON_SCRCPY_VERSION="${EMERON_SCRCPY_VERSION}")
target_include_directories(emeron_assets SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/IconFontCppHeaders")
target_compile_features(emeron_assets PUBLIC cxx_std_20)

# ---------------------------------------------------------------------------
# emeron : the GUI front-end.
# ---------------------------------------------------------------------------
add_executable(emeron
    src/main.cpp
    src/app/Application.cpp
    src/app/Theme.cpp
    src/app/NativeDialogs.cpp
    src/app/Fonts.cpp
    src/app/Screenshot.cpp
    src/app/InputScript.cpp
    src/app/AboutDialog.cpp
    src/ui/PanelRegistry.cpp
    src/ui/Notifications.cpp
    src/ui/panels/DeviceToolbar.cpp
    src/ui/panels/DashboardPanel.cpp
    src/ui/panels/FrameTimePanel.cpp
    src/ui/panels/FileExplorerPanel.cpp
    src/ui/panels/DeviceInfoPanel.cpp
    src/ui/panels/SensorPanel.cpp
    src/ui/panels/PerfettoPanel.cpp
    src/ui/panels/LogPanel.cpp
    src/ui/panels/LogcatPanel.cpp
    src/ui/panels/CrashLogPanel.cpp
    src/ui/panels/AppInspectorPanel.cpp
    src/ui/panels/ScreenMirrorPanel.cpp
    src/ui/panels/LayoutInspectorPanel.cpp
)

target_compile_definitions(emeron PRIVATE EMERON_VERSION="${PROJECT_VERSION}")

# implot links imgui publicly, so naming both would pass libimgui.a twice.
target_link_libraries(emeron PRIVATE emeron_core emeron_assets implot::implot)
# ImGuiNotify (vendor/ImGuiNotify): header-only toasts, used by src/ui/Notifications.cpp.
target_include_directories(emeron SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vendor/ImGuiNotify")

if(EMERON_NFD_AVAILABLE)
    target_link_libraries(emeron PRIVATE nfd)
    target_compile_definitions(emeron PRIVATE EMERON_HAS_NATIVE_DIALOGS=1)
    if(EMERON_NFD_WAYLAND)
        target_compile_definitions(emeron PRIVATE EMERON_NFD_WAYLAND=1)
    endif()
endif()
emeron_set_warnings(emeron)
emeron_set_sanitizers(emeron)

if(APPLE)
    set_target_properties(emeron PROPERTIES
        MACOSX_BUNDLE FALSE
        INSTALL_RPATH "@executable_path")
endif()

install(TARGETS emeron RUNTIME DESTINATION bin)

if(EMERON_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "")
message(STATUS "emeron ${PROJECT_VERSION}")
message(STATUS "  build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  compiler   : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  tests      : ${EMERON_BUILD_TESTS}")
message(STATUS "  imgui      : ${EMERON_IMGUI_ORIGIN}")
message(STATUS "  implot     : ${EMERON_IMPLOT_ORIGIN}")
message(STATUS "  glfw       : ${EMERON_GLFW_ORIGIN}")
message(STATUS "  dialogs    : ${EMERON_NFD_ORIGIN}")
message(STATUS "  ffmpeg     : ${EMERON_FFMPEG_ORIGIN}")
message(STATUS "  asan/ubsan : ${EMERON_ASAN}")
message(STATUS "")
