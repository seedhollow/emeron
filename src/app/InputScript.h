#pragma once

// Development aid: `emeron --script steps.txt` drives the UI with synthetic
// input and saves screenshots along the way -- how docs/showcase is made, and
// reproducible. One step per line, `#` starts a comment; coordinates are in
// window points (as ImGui sees them), not framebuffer pixels:
//
//   wait 2.5                  seconds
//   panel Layout              bring a panel's tab to the front
//   package com.example.app   target an app, as if picked in the toolbar
//   move 400 300
//   click 400 300 [right|middle] [double]
//   drag 400 300 600 320 [right|middle]
//   scroll 400 300 -2         wheel notches; negative scrolls down
//   key ctrl+f                ctrl, shift, alt, super; letters, digits, f1-f12,
//                             enter, escape, tab, space, backspace, delete,
//                             up, down, left, right, home, end, pageup, pagedown
//   hold alt / release alt    keep a modifier down across steps
//   type some text
//   shot out/screen.png       save the window to a PNG
//   quit                      (also implied at the end of the script)

#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>

#include "core/Result.h"

namespace em {

// The primitives the commands above expand to, one ImGui input event (or one
// pause) each; a click is move, down, a frame, up, a frame.
struct ScriptStep {
    enum class Kind {
        Wait,
        Frame,
        MouseMove,
        MouseButton,
        Wheel,
        Key,
        Text,
        Panel,
        Package,
        Shot,
        Quit
    };
    Kind kind = Kind::Frame;
    float x = 0.0F;
    float y = 0.0F;
    int button = 0;  // ImGuiMouseButton
    bool down = false;
    ImGuiKey key = ImGuiKey_None;
    double seconds = 0.0;
    std::string text;  // Text, Panel, Package, Shot
};

[[nodiscard]] Result<std::vector<ScriptStep>> parseInputScript(std::string_view script);

}  // namespace em
