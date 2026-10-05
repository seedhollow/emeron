#include "app/NativeDialogs.h"

#include "core/Log.h"

#if defined(EMERON_HAS_NATIVE_DIALOGS)
// glfw3native.h needs to know which native handles to declare, and
// nfd_glfw3.h uses them to turn the GLFW window into a dialog parent.
#  define GLFW_INCLUDE_NONE
#  if defined(_WIN32)
#    define GLFW_EXPOSE_NATIVE_WIN32
#  elif defined(__APPLE__)
#    define GLFW_EXPOSE_NATIVE_COCOA
#  else
#    define GLFW_EXPOSE_NATIVE_X11
#    if defined(EMERON_NFD_WAYLAND)
#      define GLFW_EXPOSE_NATIVE_WAYLAND
#    endif
#  endif
#  include <nfd.h>
#  include <nfd_glfw3.h>
#endif

namespace em::dialogs {
namespace {

constexpr const char* kCategory = "dialogs";

#if defined(EMERON_HAS_NATIVE_DIALOGS)

bool gReady = false;
GLFWwindow* gParent = nullptr;

// UTF-8 in both directions, so non-ASCII folder names survive on Windows.
std::string toUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string{reinterpret_cast<const char*>(text.data()), text.size()};
}

std::filesystem::path fromUtf8(const nfdu8char_t* text) {
    return std::filesystem::path{std::u8string{reinterpret_cast<const char8_t*>(text)}};
}

nfdwindowhandle_t parentHandle() {
    nfdwindowhandle_t handle{};
    if (gParent != nullptr) NFD_GetNativeWindowFromGLFWWindow(gParent, &handle);
    return handle;
}

Choice finish(nfdresult_t result, nfdu8char_t* outPath) {
    Choice choice;
    switch (result) {
        case NFD_OKAY:
            choice.outcome = Outcome::Chosen;
            choice.path = fromUtf8(outPath);
            NFD_FreePathU8(outPath);
            break;
        case NFD_CANCEL:
            choice.outcome = Outcome::Cancelled;
            break;
        case NFD_ERROR:
        default: {
            choice.outcome = Outcome::Failed;
            const char* error = NFD_GetError();
            choice.error = error != nullptr ? error : "the file dialog failed";
            EM_LOG_WARN(kCategory, choice.error);
            NFD_ClearError();
            break;
        }
    }
    return choice;
}

#endif

}  // namespace

void initialize(GLFWwindow* parent) {
#if defined(EMERON_HAS_NATIVE_DIALOGS)
    gParent = parent;
    // Can fail on Linux with no usable GTK display or portal; the app then
    // runs on with the in-app fallback rather than refusing to start.
    if (NFD_Init() == NFD_OKAY) {
        gReady = true;
    } else {
        const char* error = NFD_GetError();
        EM_LOG_WARN(kCategory, std::string{"native file dialogs unavailable: "} +
                                   (error != nullptr ? error : "unknown error"));
        NFD_ClearError();
    }
#else
    (void)parent;
    EM_LOG_INFO(kCategory, "built without native file dialogs; using the in-app prompt");
#endif
}

void shutdown() {
#if defined(EMERON_HAS_NATIVE_DIALOGS)
    if (gReady) NFD_Quit();
    gReady = false;
    gParent = nullptr;
#endif
}

bool available() noexcept {
#if defined(EMERON_HAS_NATIVE_DIALOGS)
    return gReady;
#else
    return false;
#endif
}

Choice pickFolder(std::string_view title, const std::filesystem::path& startIn) {
#if defined(EMERON_HAS_NATIVE_DIALOGS)
    if (!gReady) return Choice{};

    const std::string titleText{title};
    const std::string start = toUtf8(startIn);

    nfdpickfolderu8args_t args{};
    args.defaultPath = start.empty() ? nullptr : start.c_str();
    args.title = titleText.c_str();
    args.acceptLabel = "Download Here";
    args.parentWindow = parentHandle();

    nfdu8char_t* outPath = nullptr;
    return finish(NFD_PickFolderU8_With(&outPath, &args), outPath);
#else
    (void)title;
    (void)startIn;
    return Choice{};
#endif
}

Choice saveFile(std::string_view title, const std::filesystem::path& startIn,
                std::string_view suggestedName) {
#if defined(EMERON_HAS_NATIVE_DIALOGS)
    if (!gReady) return Choice{};

    const std::string titleText{title};
    const std::string start = toUtf8(startIn);
    const std::string name{suggestedName};

    nfdsavedialogu8args_t args{};
    args.defaultPath = start.empty() ? nullptr : start.c_str();
    args.defaultName = name.empty() ? nullptr : name.c_str();
    args.title = titleText.c_str();
    args.acceptLabel = "Download";
    args.parentWindow = parentHandle();

    nfdu8char_t* outPath = nullptr;
    return finish(NFD_SaveDialogU8_With(&outPath, &args), outPath);
#else
    (void)title;
    (void)startIn;
    (void)suggestedName;
    return Choice{};
#endif
}

}  // namespace em::dialogs
