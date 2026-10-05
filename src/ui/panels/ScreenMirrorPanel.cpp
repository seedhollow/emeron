#include "ui/panels/ScreenMirrorPanel.h"

#include <algorithm>
#include <cmath>
#include <ctime>

#include <imgui_stdlib.h>

#include <GLFW/glfw3.h>  // the platform GL 1.x declarations, as in Screenshot.cpp

#include "adb/DeviceManager.h"
#include "app/Theme.h"
#include "collect/FileSystemBrowser.h"
#include "collect/LayoutInspector.h"
#include "core/CrashHandler.h"
#include "core/PngWriter.h"
#include "core/StringUtil.h"
#include "ui/Notifications.h"
#include "ui/Widgets.h"
#include "ui/panels/LayoutInspectorPanel.h"

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F  // GL 1.2; Windows' gl.h stops at 1.1
#endif

namespace em {
namespace {

constexpr double kRippleSeconds = 0.45;
constexpr double kTypingFlushSeconds = 0.3;
constexpr double kWheelSendSeconds = 0.25;

std::string timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm parts{};
#if defined(_WIN32)
    localtime_s(&parts, &now);
#else
    localtime_r(&now, &parts);
#endif
    return format("%04d%02d%02d-%02d%02d%02d", parts.tm_year + 1900, parts.tm_mon + 1,
                  parts.tm_mday, parts.tm_hour, parts.tm_min, parts.tm_sec);
}

}  // namespace

void ScreenMirrorPanel::onDeviceChanged(AppContext& context) {
    context.mirror.stop();
    shown_.reset();
    ripples_.clear();
    pressing_ = false;
    pendingTyping_.clear();
    status_.clear();
}

void ScreenMirrorPanel::onShutdown(AppContext& context) {
    context.mirror.stop();
    releaseTexture();  // the GL context is still alive here
}

void ScreenMirrorPanel::releaseTexture() {
    if (texture_ != 0) {
        const auto id = static_cast<GLuint>(texture_);
        glDeleteTextures(1, &id);
        texture_ = 0;
    }
    textureWidth_ = textureHeight_ = 0;
}

void ScreenMirrorPanel::uploadFrame() {
    if (!shown_ || shown_->rgba.empty()) return;
    GLuint id = static_cast<GLuint>(texture_);
    if (id == 0) {
        glGenTextures(1, &id);
        texture_ = id;
    }
    glBindTexture(GL_TEXTURE_2D, id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    const auto w = static_cast<GLsizei>(shown_->width);
    const auto h = static_cast<GLsizei>(shown_->height);
    if (shown_->width != textureWidth_ || shown_->height != textureHeight_) {
        // New size (first frame, or the device rotated): reallocate.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     shown_->rgba.data());
        textureWidth_ = shown_->width;
        textureHeight_ = shown_->height;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE,
                        shown_->rgba.data());
    }
}

void ScreenMirrorPanel::saveScreenshot(AppContext& context) {
    if (!shown_) return;
    std::string serial = context.selectedSerial;
    for (char& c : serial) {
        if (c == ':' || c == '/' || c == '\\') c = '_';  // network serials are host:port
    }
    const auto file =
        context.workspaceDir / "screenshots" / (serial + "-" + timestamp() + ".png");
    const auto status = writePng(file, shown_->rgba, shown_->width, shown_->height);
    statusOk_ = static_cast<bool>(status);
    if (status) {
        lastScreenshot_ = file;
        status_ = "saved " + file.filename().string();
        notify::postWithAction(
            notify::Kind::Success, "Screenshot saved", file.filename().string(),
            ICON_FA_FOLDER_OPEN "  Show",
            [&context, file] { FileSystemBrowser::revealOnHost(context.pool, file); });
    } else {
        status_ = status.error().message;
        notify::error("Screenshot failed", status_);
    }
}

void ScreenMirrorPanel::drawToolbar(AppContext& context) {
    const auto& palette = theme::palette();
    ScreenMirror& mirror = context.mirror;

    if (ImGui::Button(wantRunning_ ? ICON_FA_PAUSE "###live" : ICON_FA_PLAY "###live")) {
        wantRunning_ = !wantRunning_;
        if (wantRunning_) crash::setBreadcrumb("Screen: mirroring " + context.selectedSerial);
    }
    widgets::termTooltip(
        wantRunning_ ? "Pause" : "Show the screen  (screencap)",
        "Copies the phone's screen over adb a few times a second. It pauses by "
        "itself while this tab is hidden.");

    const auto key = [&mirror](const char* icon, const char* keycode, const char* label) {
        widgets::sameLineOrWrap(widgets::iconButtonWidth(icon));
        if (ImGui::Button(icon)) mirror.key(keycode);
        const std::string term = std::string{"input keyevent "} + keycode;
        widgets::termTooltip(term.c_str(), label);
    };
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    key(ICON_FA_CHEVRON_LEFT, "KEYCODE_BACK", "Back");
    key(ICON_FA_HOUSE, "KEYCODE_HOME", "Home");
    key(ICON_FA_SQUARE, "KEYCODE_APP_SWITCH", "Recent apps");
    key(ICON_FA_POWER_OFF, "KEYCODE_POWER", "Power button: screen off / on");
    key(ICON_FA_VOLUME_LOW, "KEYCODE_VOLUME_DOWN", "Volume down");
    key(ICON_FA_VOLUME_HIGH, "KEYCODE_VOLUME_UP", "Volume up");

    widgets::sameLineOrWrap(widgets::iconButtonWidth(ICON_FA_CAMERA) * 2.0F);
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::BeginDisabled(!shown_);
    if (ImGui::Button(ICON_FA_CAMERA)) saveScreenshot(context);
    ImGui::EndDisabled();
    widgets::termTooltip("Save screenshot",
                         "Save the frame on screen as a PNG in <workspace>/screenshots.");

    LayoutInspector::Focus& focus = context.layout.focus();
    widgets::sameLineOrWrap(widgets::iconButtonWidth(ICON_FA_CROSSHAIRS));
    ImGui::BeginDisabled(!context.hasDevice());
    if (widgets::toggleButton(
            ICON_FA_CROSSHAIRS, &focus.pickOnScreen,
            "Inspect: point at the screen to see its views, click to select one "
            "in the Layout panel. Clicks do not reach the phone meanwhile.") &&
        focus.pickOnScreen && !focus.snapshot) {
        LayoutInspectorPanel::requestCapture(context, LayoutSource::Merged);
    }
    ImGui::EndDisabled();
    if (focus.pickOnScreen) {
        ImGui::SameLine();
        ImGui::BeginDisabled(context.layout.busy());
        if (ImGui::Button(context.layout.busy() ? ICON_FA_HOURGLASS_HALF "###relayout"
                                                : ICON_FA_ARROWS_ROTATE "###relayout")) {
            LayoutInspectorPanel::requestCapture(context, LayoutSource::Merged);
        }
        ImGui::EndDisabled();
        widgets::termTooltip(
            "Capture layout again",
            "Read the views again after the screen changed (the Layout panel's "
            "Capture).");
    }

    widgets::sameLineOrWrap(widgets::iconButtonWidth(ICON_FA_KEYBOARD));
    widgets::toggleButton(
        ICON_FA_KEYBOARD, &typeIntoDevice_,
        "Type into the phone: while the pointer is over the screen, what you "
        "type goes to the phone (Enter, Backspace, Tab and arrows too). ASCII "
        "only -- a limit of `input text`.");

    const auto restartIfRunning = [&mirror] {
        if (mirror.running()) {
            mirror.stop();
            mirror.start();
        }
    };

    // Engine: Fast streams the hardware encoder through scrcpy's server;
    // Compatible is plain screencap and needs nothing but adb.
    widgets::sameLineOrWrap(150.0F);
    {
        const bool fastAvailable = mirror.scrcpyAvailable();
        int engine = mirror.engine() == MirrorEngine::Scrcpy && fastAvailable ? 0 : 1;
        static constexpr const char* kEngines[] = {"Fast", "Compatible"};
        ImGui::SetNextItemWidth(120.0F);
        if (ImGui::BeginCombo("##engine", kEngines[engine])) {
            ImGui::BeginDisabled(!fastAvailable);
            if (ImGui::Selectable("Fast  (scrcpy)", engine == 0)) {
                mirror.setEngine(MirrorEngine::Scrcpy);
                restartIfRunning();
            }
            ImGui::EndDisabled();
            if (!fastAvailable)
                ImGui::SetItemTooltip("%s", mirror.scrcpyUnavailableReason().c_str());
            if (ImGui::Selectable("Compatible  (screencap)", engine == 1)) {
                mirror.setEngine(MirrorEngine::Screencap);
                restartIfRunning();
            }
            ImGui::EndCombo();
        }
        widgets::termTooltip(
            "Capture engine",
            "Fast: runs scrcpy's server on the phone for the length of the session (it deletes "
            "itself on start), which streams the phone's hardware video encoder -- 30-60 fps, "
            "with real touch: a drag is a drag. Decoded here with FFmpeg.\n\n"
            "Compatible: `screencap` loops over plain adb. A few frames per second and "
            "approximated gestures, but it needs nothing on the phone. Fast falls back to it "
            "by "
            "itself if the server cannot start.");
    }

    // Compress and Streams only apply to the Compatible engine.
    if (!(mirror.engine() == MirrorEngine::Scrcpy && mirror.scrcpyAvailable())) {
        widgets::sameLineOrWrap(170.0F);
        bool compress = mirror.compression();
        if (ImGui::Checkbox("Compress", &compress)) {
            mirror.setCompression(compress);
            restartIfRunning();
        }
        widgets::termTooltip(
            "gzip on the phone",
            "Compress each frame on the phone before sending it. Usually far less "
            "data over USB. Turn off for an emulator, or if the phone's own CPU -- "
            "not the cable -- turns out to be the bottleneck.");

        widgets::sameLineOrWrap(190.0F);
        int workers = mirror.workerCount();
        ImGui::SetNextItemWidth(110.0F);
        if (ImGui::SliderInt("Streams", &workers, 1, ScreenMirror::kMaxWorkers)) {
            mirror.setWorkerCount(workers);
            restartIfRunning();
        }
        widgets::termTooltip(
            "Concurrent screencap loops",
            "`screencap` itself -- not adb, not USB -- is the slow part: ~170 ms on a "
            "mid-range "
            "phone for one capture. It is not serialized, though, so running several loops at "
            "once genuinely helps: measured on a real phone, 1 stream ~4 fps, 3 ~12 fps, 6 ~16 "
            "fps -- real gains, but each extra stream buys less and spends more of the phone's "
            "own CPU on the mirror instead of whatever you are profiling.");
    }

    // Text field: for longer text than typing passthrough is comfortable for.
    ImGui::SetNextItemWidth(std::max(140.0F, ImGui::GetContentRegionAvail().x -
                                                 widgets::iconButtonWidth(ICON_FA_PAPER_PLANE) -
                                                 8.0F));
    const bool enter =
        ImGui::InputTextWithHint("##sendtext", ICON_FA_KEYBOARD " text to type on the phone",
                                 &textField_, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::BeginDisabled(textField_.empty());
    if ((ImGui::Button(ICON_FA_PAPER_PLANE) || enter) && !textField_.empty()) {
        if (inputTextArgument(textField_).empty()) {
            status_ = "only printable ASCII can be typed through adb";
            statusOk_ = false;
        } else {
            mirror.text(textField_);
            textField_.clear();
        }
    }
    ImGui::EndDisabled();
    widgets::termTooltip("input text", "Type this into whatever field has focus on the phone.");

    // Status line.
    const MirrorStats stats = mirror.stats();
    if (!stats.error.empty() && stats.running) {
        ImGui::TextColored(palette.bad, "%s", stats.error.c_str());
    } else if (shown_) {
        if (stats.engine == MirrorEngine::Scrcpy) {
            ImGui::TextDisabled("%u x %u  |  %.0f fps  |  %s/frame H.264  |  fast%s",
                                shown_->width, shown_->height, stats.fps,
                                humanBytes(static_cast<double>(stats.lastFrameBytes)).c_str(),
                                stats.running ? "" : "  |  paused");
            widgets::termTooltip(
                "Fast engine  (scrcpy)",
                "The phone's hardware encoder streams H.264, decoded here. It "
                "only sends a frame when the screen changes, so a still screen "
                "shows a low frame rate -- that is not lag.");
        } else {
            ImGui::TextDisabled("%u x %u  |  %.1f fps  |  %s/frame  |  %d stream%s%s",
                                shown_->width, shown_->height, stats.fps,
                                humanBytes(static_cast<double>(stats.lastFrameBytes)).c_str(),
                                stats.workers, stats.workers == 1 ? "" : "s",
                                stats.running ? "" : "  |  paused");
        }
    }
    if (!stats.notice.empty() && stats.notice != announcedNotice_) {
        announcedNotice_ = stats.notice;
        notify::warning("Screen: using the compatible engine", stats.notice);
    }
    if (!stats.notice.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, palette.warning);
        ImGui::TextWrapped("%s", stats.notice.c_str());
        ImGui::PopStyleColor();
    }
    if (!status_.empty()) {
        if (shown_ || !stats.error.empty()) ImGui::SameLine();
        ImGui::TextColored(statusOk_ ? palette.good : palette.bad, "%s", status_.c_str());
        if (statusOk_ && !lastScreenshot_.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_FOLDER_OPEN)) {
                FileSystemBrowser::revealOnHost(context.pool, lastScreenshot_);
            }
            ImGui::SetItemTooltip("Show the screenshot");
        }
    }
}

void ScreenMirrorPanel::handlePointer(AppContext& context, ImVec2 imageMin, float scale) {
    const ImGuiIO& io = ImGui::GetIO();
    const auto toDevice = [&](ImVec2 screen) {
        const DevicePoint p = screenToDevice(screen.x, screen.y, imageMin.x, imageMin.y, scale,
                                             shown_->width, shown_->height);
        return ImVec2{p.x, p.y};
    };
    const bool hovered = ImGui::IsItemHovered();

    // With scrcpy's control channel the finger follows the mouse for real:
    // down on press, a move whenever the pointer moves, up on release. Long
    // presses, drags and flings then behave exactly as on the phone.
    if (context.mirror.liveTouch()) {
        const ImVec2 now = toDevice(io.MousePos);
        if (ImGui::IsItemActivated()) {
            pressing_ = true;
            pressDevice_ = now;
            lastTouch_ = now;
            context.mirror.touch(ScreenMirror::Touch::Down, static_cast<int>(now.x),
                                 static_cast<int>(now.y));
        } else if (pressing_ && ImGui::IsItemActive() &&
                   (static_cast<int>(now.x) != static_cast<int>(lastTouch_.x) ||
                    static_cast<int>(now.y) != static_cast<int>(lastTouch_.y))) {
            lastTouch_ = now;
            context.mirror.touch(ScreenMirror::Touch::Move, static_cast<int>(now.x),
                                 static_cast<int>(now.y));
        }
        if (pressing_ && ImGui::IsItemDeactivated()) {
            pressing_ = false;
            context.mirror.touch(ScreenMirror::Touch::Up, static_cast<int>(now.x),
                                 static_cast<int>(now.y));
            ripples_.push_back({now, ImGui::GetTime()});
        }
        // The wheel is a real scroll event at the pointer, as with a mouse
        // plugged into the phone.
        if (hovered && io.MouseWheel != 0.0F) {
            context.mirror.scroll(static_cast<int>(now.x), static_cast<int>(now.y),
                                  io.MouseWheel);
        }
        return;
    }

    if (ImGui::IsItemActivated()) {
        pressing_ = true;
        pressScreen_ = io.MousePos;
        pressDevice_ = toDevice(io.MousePos);
        pressTime_ = ImGui::GetTime();
    }
    if (pressing_ && ImGui::IsItemDeactivated()) {
        pressing_ = false;
        const ImVec2 releaseDevice = toDevice(io.MousePos);
        const Gesture g = classifyGesture(
            DevicePoint{pressDevice_.x, pressDevice_.y},
            DevicePoint{releaseDevice.x, releaseDevice.y},
            std::hypot(io.MousePos.x - pressScreen_.x, io.MousePos.y - pressScreen_.y),
            ImGui::GetTime() - pressTime_);
        switch (g.kind) {
            case Gesture::Kind::Tap:
                context.mirror.tap(g.x1, g.y1);
                break;
            case Gesture::Kind::LongPress:
                context.mirror.longPress(g.x1, g.y1);
                break;
            case Gesture::Kind::Swipe:
                context.mirror.swipe(g.x1, g.y1, g.x2, g.y2, g.durationMs);
                break;
        }
        ripples_.push_back(
            {g.kind == Gesture::Kind::Swipe ? releaseDevice : pressDevice_, ImGui::GetTime()});
    }

    // Wheel: gathered for a moment, then sent as one swipe from the middle.
    if (hovered && io.MouseWheel != 0.0F) wheelAccumulated_ += io.MouseWheel;
    if (wheelAccumulated_ != 0.0F && ImGui::GetTime() - lastWheelSend_ > kWheelSendSeconds) {
        const float h = static_cast<float>(shown_->height);
        const float cx = static_cast<float>(shown_->width) * 0.5F;
        const float cy = h * 0.5F;
        const float distance = std::clamp(std::abs(wheelAccumulated_), 1.0F, 3.0F) * h * 0.12F;
        // Wheel up shows what is above: the finger moves down.
        const float direction = wheelAccumulated_ > 0.0F ? 1.0F : -1.0F;
        context.mirror.swipe(
            static_cast<int>(cx), static_cast<int>(cy - direction * distance * 0.5F),
            static_cast<int>(cx), static_cast<int>(cy + direction * distance * 0.5F), 200);
        wheelAccumulated_ = 0.0F;
        lastWheelSend_ = ImGui::GetTime();
    }
}

void ScreenMirrorPanel::handleTyping(AppContext& context) {
    ScreenMirror& mirror = context.mirror;
    const auto flush = [&] {
        if (!pendingTyping_.empty()) {
            mirror.text(pendingTyping_);
            pendingTyping_.clear();
        }
    };
    const ImGuiIO& io = ImGui::GetIO();
    if (typeIntoDevice_ && ImGui::IsWindowHovered() && !io.WantTextInput) {
        for (const ImWchar c : io.InputQueueCharacters) {
            if (c >= 0x20 && c <= 0x7E) {
                pendingTyping_.push_back(static_cast<char>(c));
                lastTypedTime_ = ImGui::GetTime();
            }
        }
        static constexpr std::pair<ImGuiKey, const char*> kKeys[] = {
            {ImGuiKey_Enter, "KEYCODE_ENTER"},
            {ImGuiKey_KeypadEnter, "KEYCODE_ENTER"},
            {ImGuiKey_Backspace, "KEYCODE_DEL"},
            {ImGuiKey_Delete, "KEYCODE_FORWARD_DEL"},
            {ImGuiKey_Tab, "KEYCODE_TAB"},
            {ImGuiKey_Escape, "KEYCODE_BACK"},
            {ImGuiKey_LeftArrow, "KEYCODE_DPAD_LEFT"},
            {ImGuiKey_RightArrow, "KEYCODE_DPAD_RIGHT"},
            {ImGuiKey_UpArrow, "KEYCODE_DPAD_UP"},
            {ImGuiKey_DownArrow, "KEYCODE_DPAD_DOWN"},
        };
        for (const auto& [key, keycode] : kKeys) {
            if (ImGui::IsKeyPressed(key)) {
                flush();  // keep keys in order with the text before them
                mirror.key(keycode);
            }
        }
    }
    // Typed text goes in short bursts: one `input text` per burst, not per key.
    if (!pendingTyping_.empty() && ImGui::GetTime() - lastTypedTime_ > kTypingFlushSeconds)
        flush();
}

void ScreenMirrorPanel::drawScreen(AppContext& context) {
    const auto& palette = theme::palette();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (!shown_) {
        const MirrorStats stats = context.mirror.stats();
        ImGui::Spacing();
        if (wantRunning_ && stats.error.empty()) {
            ImGui::TextDisabled(ICON_FA_HOURGLASS_HALF "  Waiting for the first frame...");
        } else if (!wantRunning_) {
            ImGui::TextDisabled(ICON_FA_MOBILE_SCREEN "  Press " ICON_FA_PLAY
                                                      " to show the phone's screen.");
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, palette.muted);
            ImGui::TextWrapped(
                "Click to tap, hold to long-press, drag to swipe, and use the wheel to scroll. "
                "Expect a few frames per second over USB: enough to follow along and drive the "
                "app, not video. Apps that block screenshots (banking, DRM video) show black.");
            ImGui::PopStyleColor();
        }
        return;
    }
    if (avail.x < 10.0F || avail.y < 10.0F) return;

    const float scale = std::min(avail.x / static_cast<float>(shown_->width),
                                 avail.y / static_cast<float>(shown_->height));
    const ImVec2 size{static_cast<float>(shown_->width) * scale,
                      static_cast<float>(shown_->height) * scale};
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const ImVec2 imageMin{cursor.x + std::floor((avail.x - size.x) * 0.5F), cursor.y};
    const ImVec2 imageMax{imageMin.x + size.x, imageMin.y + size.y};

    ImGui::SetCursorScreenPos(imageMin);
    ImGui::InvisibleButton("##screen", size);
    LayoutInspector::Focus& focus = context.layout.focus();
    // Layout coordinates are the device's; the frame may be scaled down.
    const LayoutSnapshot* layout = focus.snapshot.get();
    if (layout != nullptr &&
        (layout->screenWidth <= 0 || layout->screenHeight <= 0 ||
         (layout->screenWidth > layout->screenHeight) != (shown_->width > shown_->height))) {
        layout = nullptr;  // rotated since the capture
    }
    const float toLayout = layout != nullptr ? static_cast<float>(layout->screenWidth) /
                                                   static_cast<float>(shown_->width)
                                             : 1.0F;
    if (focus.pickOnScreen) {
        const bool over = ImGui::IsItemHovered();
        if (over && layout != nullptr) {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const int x = static_cast<int>((mouse.x - imageMin.x) / scale * toLayout);
            const int y = static_cast<int>((mouse.y - imageMin.y) / scale * toLayout);
            const int hit = hitTest(layout->nodes, layout->rootsFor(focus.window), x, y, false);
            focus.hovered = hit;
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                focus.selected = hit;
                focus.revealSelected = hit >= 0;
            }
            ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
        } else if (pickHovering_) {
            focus.hovered = -1;
        }
        pickHovering_ = over;
    } else {
        handlePointer(context, imageMin, scale);
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddImage(ImTextureRef{static_cast<ImTextureID>(texture_)}, imageMin, imageMax);
    draw->AddRect(imageMin, imageMax, ImGui::GetColorU32(ImGuiCol_Border));

    // The Layout panel's hovered and selected views, on the live screen.
    if (layout != nullptr) {
        const float k = scale / toLayout;
        const auto viewRect = [&](int node, ImU32 fill, ImU32 line, float thickness) {
            if (node < 0 || node >= static_cast<int>(layout->nodes.size())) return;
            const LayoutRect& b = layout->nodes[static_cast<std::size_t>(node)].bounds;
            const ImVec2 a{imageMin.x + static_cast<float>(b.left) * k,
                           imageMin.y + static_cast<float>(b.top) * k};
            const ImVec2 c{imageMin.x + static_cast<float>(b.right) * k,
                           imageMin.y + static_cast<float>(b.bottom) * k};
            draw->PushClipRect(imageMin, imageMax, true);
            draw->AddRectFilled(a, c, fill);
            draw->AddRect(a, c, line, 0.0F, 0, thickness);
            draw->PopClipRect();
        };
        ImVec4 tint = palette.accent;
        tint.w = 0.15F;
        const ImU32 fill = ImGui::ColorConvertFloat4ToU32(tint);
        const ImU32 line = ImGui::ColorConvertFloat4ToU32(palette.accent);
        if (focus.pickOnScreen) viewRect(focus.selected, fill, line, 2.5F);
        if (focus.hovered != focus.selected || !focus.pickOnScreen)
            viewRect(focus.hovered, fill, line, 1.5F);
        if (focus.pickOnScreen && ImGui::IsItemHovered() && focus.hovered >= 0) {
            const LayoutNode& n = layout->nodes[static_cast<std::size_t>(focus.hovered)];
            std::string tip{n.simpleName()};
            if (!n.resourceId.empty()) tip += "  #" + std::string{n.idName()};
            tip += format("\n%.0f x %.0f dp", static_cast<double>(layout->dp(n.bounds.width())),
                          static_cast<double>(layout->dp(n.bounds.height())));
            if (!n.text.empty()) tip += "\n\"" + n.text.substr(0, 60) + "\"";
            ImGui::SetTooltip("%s", tip.c_str());
        }
    }

    const auto toScreen = [&](ImVec2 device) {
        return ImVec2{imageMin.x + device.x * scale, imageMin.y + device.y * scale};
    };
    const ImU32 accent = ImGui::ColorConvertFloat4ToU32(palette.accent);
    if (pressing_) {
        const ImVec2 io = ImGui::GetIO().MousePos;
        draw->AddLine(toScreen(pressDevice_), io, accent, 3.0F);
        draw->AddCircleFilled(toScreen(pressDevice_), 7.0F, accent);
    }
    const double now = ImGui::GetTime();
    std::erase_if(ripples_, [now](const Ripple& r) { return now - r.time > kRippleSeconds; });
    for (const Ripple& r : ripples_) {
        const float t = static_cast<float>((now - r.time) / kRippleSeconds);
        ImVec4 color = palette.accent;
        color.w = 1.0F - t;
        draw->AddCircle(toScreen(r.device), 8.0F + 22.0F * t,
                        ImGui::ColorConvertFloat4ToU32(color), 0, 3.0F);
    }
}

void ScreenMirrorPanel::draw(AppContext& context) {
    const bool visible = ImGui::Begin(windowTitle(), visibleFlag());

    // Capture runs only while asked for, on screen, and with a device.
    const bool shouldRun = wantRunning_ && visible && this->visible() && context.hasDevice();
    if (shouldRun && !context.mirror.running()) context.mirror.start();
    if (shouldRun) context.mirror.keepAlive();
    if (!shouldRun && context.mirror.running()) context.mirror.stop();

    if (!visible) {
        ImGui::End();
        return;
    }
    if (!context.hasDevice()) {
        ImGui::TextDisabled(ICON_FA_PLUG "  No device selected.");
        ImGui::End();
        return;
    }

    if (auto frame = context.mirror.latestFrame(); frame && frame != shown_) {
        shown_ = std::move(frame);
        uploadFrame();
    }

    drawToolbar(context);
    ImGui::Separator();
    drawScreen(context);
    handleTyping(context);
    ImGui::End();
}

}  // namespace em
