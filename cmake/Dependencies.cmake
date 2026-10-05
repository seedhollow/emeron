# Third-party dependencies.
#
# Default is the committed source under vendor/, so a clone builds offline and
# builds exactly what was tested. -DEMERON_VENDORED=OFF downloads the same
# pinned revisions instead, which is the path to use when bumping a version.
#
# vendor/MANIFEST.txt records the commit behind each tag below; keep them in
# sync.

include(FetchContent)

set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

find_package(Threads REQUIRED)

set(EMERON_VENDOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor")

set(EMERON_IMGUI_TAG  "v1.92.9b-docking")
set(EMERON_IMPLOT_TAG "v1.0")
set(EMERON_GLFW_TAG   "3.4")

# Vendored by default, but only when the tree is actually there -- a sparse or
# partial checkout should fall back to downloading rather than fail to configure.
if(EXISTS "${EMERON_VENDOR_DIR}/imgui/imgui.cpp"
   AND EXISTS "${EMERON_VENDOR_DIR}/implot/implot.cpp"
   AND EXISTS "${EMERON_VENDOR_DIR}/glfw/CMakeLists.txt")
    set(_emeron_vendored_default ON)
else()
    set(_emeron_vendored_default OFF)
endif()

option(EMERON_VENDORED
       "Build third-party dependencies from vendor/ instead of downloading them"
       ${_emeron_vendored_default})

if(NOT EMERON_VENDORED)
    set(FETCHCONTENT_QUIET OFF)
endif()

# ---------------------------------------------------------------------------
# GLFW -- has its own CMake project, so it is added as a subdirectory either way
# ---------------------------------------------------------------------------
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)

if(EMERON_VENDORED)
    # SYSTEM keeps glfw's headers from being warned about by our own flags;
    # EXCLUDE_FROM_ALL keeps its auxiliary targets out of the default build.
    add_subdirectory("${EMERON_VENDOR_DIR}/glfw" "${CMAKE_BINARY_DIR}/vendor/glfw"
                     SYSTEM EXCLUDE_FROM_ALL)
    set(EMERON_GLFW_ORIGIN "vendor/glfw (${EMERON_GLFW_TAG})")
else()
    FetchContent_Declare(glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG        ${EMERON_GLFW_TAG}
        GIT_SHALLOW    TRUE
        SYSTEM)
    FetchContent_MakeAvailable(glfw)
    set(EMERON_GLFW_ORIGIN "downloaded ${EMERON_GLFW_TAG}")
endif()

# ---------------------------------------------------------------------------
# Dear ImGui -- docking branch, for ImGuiConfigFlags_DockingEnable and
# ImGuiConfigFlags_ViewportsEnable. No upstream CMakeLists, so we compile it.
# ---------------------------------------------------------------------------
if(EMERON_VENDORED)
    set(EMERON_IMGUI_DIR "${EMERON_VENDOR_DIR}/imgui")
    set(EMERON_IMGUI_ORIGIN "vendor/imgui (${EMERON_IMGUI_TAG})")
else()
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        ${EMERON_IMGUI_TAG}
        GIT_SHALLOW    TRUE
        SYSTEM)
    FetchContent_MakeAvailable(imgui)
    set(EMERON_IMGUI_DIR "${imgui_SOURCE_DIR}")
    set(EMERON_IMGUI_ORIGIN "downloaded ${EMERON_IMGUI_TAG}")
endif()

add_library(imgui STATIC
    ${EMERON_IMGUI_DIR}/imgui.cpp
    ${EMERON_IMGUI_DIR}/imgui_draw.cpp
    ${EMERON_IMGUI_DIR}/imgui_tables.cpp
    ${EMERON_IMGUI_DIR}/imgui_widgets.cpp
    ${EMERON_IMGUI_DIR}/imgui_demo.cpp
    ${EMERON_IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp
    ${EMERON_IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${EMERON_IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui SYSTEM PUBLIC
    ${EMERON_IMGUI_DIR}
    ${EMERON_IMGUI_DIR}/backends
    ${EMERON_IMGUI_DIR}/misc/cpp)
target_link_libraries(imgui PUBLIC glfw)
target_compile_features(imgui PUBLIC cxx_std_20)

# imgui_impl_opengl3 ships its own loader (imgl3w), so no glad/gl3w needed.
if(APPLE)
    target_compile_definitions(imgui PUBLIC GL_SILENCE_DEPRECATION)
    find_library(COCOA_LIB Cocoa REQUIRED)
    find_library(IOKIT_LIB IOKit REQUIRED)
    find_library(COREVIDEO_LIB CoreVideo REQUIRED)
    find_library(OPENGL_LIB OpenGL REQUIRED)
    target_link_libraries(imgui PUBLIC
        ${COCOA_LIB} ${IOKIT_LIB} ${COREVIDEO_LIB} ${OPENGL_LIB})
elseif(WIN32)
    target_link_libraries(imgui PUBLIC opengl32)
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(imgui PUBLIC OpenGL::GL ${CMAKE_DL_LIBS})
endif()

add_library(imgui::imgui ALIAS imgui)

# ---------------------------------------------------------------------------
# ImPlot -- the plotting widgets used by the dashboard, jank analyser and
# sensor charts. Also has no upstream CMakeLists.
# ---------------------------------------------------------------------------
if(EMERON_VENDORED)
    set(EMERON_IMPLOT_DIR "${EMERON_VENDOR_DIR}/implot")
    set(EMERON_IMPLOT_ORIGIN "vendor/implot (${EMERON_IMPLOT_TAG})")
else()
    FetchContent_Declare(implot
        GIT_REPOSITORY https://github.com/epezent/implot.git
        GIT_TAG        ${EMERON_IMPLOT_TAG}
        GIT_SHALLOW    TRUE
        SYSTEM)
    FetchContent_MakeAvailable(implot)
    set(EMERON_IMPLOT_DIR "${implot_SOURCE_DIR}")
    set(EMERON_IMPLOT_ORIGIN "downloaded ${EMERON_IMPLOT_TAG}")
endif()

add_library(implot STATIC
    ${EMERON_IMPLOT_DIR}/implot.cpp
    ${EMERON_IMPLOT_DIR}/implot_items.cpp
    ${EMERON_IMPLOT_DIR}/implot_demo.cpp
)
target_include_directories(implot SYSTEM PUBLIC ${EMERON_IMPLOT_DIR})
target_link_libraries(implot PUBLIC imgui)
add_library(implot::implot ALIAS implot)

# ---------------------------------------------------------------------------
# nativefiledialog-extended -- the OS's own Save / Choose Folder dialogs.
#
# Optional by design. On macOS and Windows it needs nothing extra. On Linux
# its CMake hard-requires GTK3 (or dbus-1 for the XDG portal backend), which
# would otherwise make GTK a build requirement for everyone; instead we probe
# first and, if neither is present, build without it. emeron then falls back to
# an in-app folder prompt, so the feature degrades rather than the build.
# ---------------------------------------------------------------------------
set(EMERON_NFD_TAG "v1.4.1")
option(EMERON_NATIVE_DIALOGS "Use the OS file dialogs (nativefiledialog-extended)" ON)

set(EMERON_NFD_AVAILABLE OFF)
set(EMERON_NFD_WAYLAND OFF)
set(EMERON_NFD_ORIGIN "disabled")

if(EMERON_NATIVE_DIALOGS)
    set(_emeron_nfd_ok ON)

    if(UNIX AND NOT APPLE)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(EMERON_GTK3 QUIET gtk+-3.0)
            pkg_check_modules(EMERON_DBUS QUIET dbus-1)
            pkg_check_modules(EMERON_WAYLAND_CLIENT QUIET wayland-client)
        endif()

        if(EMERON_GTK3_FOUND)
            set(NFD_PORTAL OFF CACHE BOOL "" FORCE)
        elseif(EMERON_DBUS_FOUND)
            set(NFD_PORTAL ON CACHE BOOL "" FORCE)
        else()
            set(_emeron_nfd_ok OFF)
            set(EMERON_NFD_ORIGIN "unavailable: install gtk+-3.0 or dbus-1 dev packages")
        endif()

        if(EMERON_WAYLAND_CLIENT_FOUND)
            set(EMERON_NFD_WAYLAND ON)
        else()
            set(NFD_WAYLAND OFF CACHE BOOL "" FORCE)
        endif()
    endif()

    if(_emeron_nfd_ok)
        set(NFD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(NFD_INSTALL     OFF CACHE BOOL "" FORCE)

        if(EMERON_VENDORED AND EXISTS "${EMERON_VENDOR_DIR}/nfd/CMakeLists.txt")
            add_subdirectory("${EMERON_VENDOR_DIR}/nfd" "${CMAKE_BINARY_DIR}/vendor/nfd"
                             SYSTEM EXCLUDE_FROM_ALL)
            set(EMERON_NFD_ORIGIN "vendor/nfd (${EMERON_NFD_TAG})")
        else()
            FetchContent_Declare(nfd
                GIT_REPOSITORY https://github.com/btzy/nativefiledialog-extended.git
                GIT_TAG        ${EMERON_NFD_TAG}
                GIT_SHALLOW    TRUE
                SYSTEM)
            FetchContent_MakeAvailable(nfd)
            set(EMERON_NFD_ORIGIN "downloaded ${EMERON_NFD_TAG}")
        endif()
        set(EMERON_NFD_AVAILABLE ON)
    endif()
endif()
