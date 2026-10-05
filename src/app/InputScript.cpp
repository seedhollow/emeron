#include "app/InputScript.h"

#include <array>
#include <utility>

#include "core/StringUtil.h"

namespace em {
namespace {

ImGuiKey keyNamed(std::string_view name) {
    const std::string n = toLower(name);
    if (n.size() == 1 && n[0] >= 'a' && n[0] <= 'z')
        return static_cast<ImGuiKey>(ImGuiKey_A + (n[0] - 'a'));
    if (n.size() == 1 && n[0] >= '0' && n[0] <= '9')
        return static_cast<ImGuiKey>(ImGuiKey_0 + (n[0] - '0'));
    if (n.size() >= 2 && n[0] == 'f') {
        if (const auto f = parseNumber<int>(std::string_view{n}.substr(1));
            f && *f >= 1 && *f <= 12) {
            return static_cast<ImGuiKey>(ImGuiKey_F1 + (*f - 1));
        }
    }
    static constexpr std::array<std::pair<std::string_view, ImGuiKey>, 15> kNamed{{
        {"enter", ImGuiKey_Enter},
        {"escape", ImGuiKey_Escape},
        {"tab", ImGuiKey_Tab},
        {"space", ImGuiKey_Space},
        {"backspace", ImGuiKey_Backspace},
        {"delete", ImGuiKey_Delete},
        {"up", ImGuiKey_UpArrow},
        {"down", ImGuiKey_DownArrow},
        {"left", ImGuiKey_LeftArrow},
        {"right", ImGuiKey_RightArrow},
        {"home", ImGuiKey_Home},
        {"end", ImGuiKey_End},
        {"pageup", ImGuiKey_PageUp},
        {"pagedown", ImGuiKey_PageDown},
        {"esc", ImGuiKey_Escape},
    }};
    for (const auto& [label, key] : kNamed) {
        if (n == label) return key;
    }
    return ImGuiKey_None;
}

ImGuiKey modifierNamed(std::string_view name) {
    const std::string n = toLower(name);
    if (n == "ctrl" || n == "control") return ImGuiMod_Ctrl;
    if (n == "shift") return ImGuiMod_Shift;
    if (n == "alt" || n == "option") return ImGuiMod_Alt;
    if (n == "super" || n == "cmd") return ImGuiMod_Super;
    return ImGuiKey_None;
}

int buttonNamed(std::string_view name) {
    if (name == "right") return ImGuiMouseButton_Right;
    if (name == "middle") return ImGuiMouseButton_Middle;
    return name == "left" ? ImGuiMouseButton_Left : -1;
}

}  // namespace

Result<std::vector<ScriptStep>> parseInputScript(std::string_view script) {
    std::vector<ScriptStep> steps;
    using Kind = ScriptStep::Kind;
    const auto frame = [&] { steps.push_back({.kind = Kind::Frame}); };
    const auto move = [&](float x, float y) {
        steps.push_back({.kind = Kind::MouseMove, .x = x, .y = y});
    };
    const auto button = [&](int b, bool down) {
        steps.push_back({.kind = Kind::MouseButton, .button = b, .down = down});
    };

    int lineNumber = 0;
    for (std::string_view line : split(script, '\n', true)) {
        ++lineNumber;
        if (const auto hash = line.find('#'); hash != std::string_view::npos)
            line = line.substr(0, hash);
        line = trim(line);
        if (line.empty()) continue;
        const auto space = line.find(' ');
        const std::string command = toLower(line.substr(0, space));
        const std::string_view rest =
            space == std::string_view::npos ? std::string_view{} : trim(line.substr(space));
        std::vector<std::string_view> words;
        for (const auto w : split(rest, ' ', false)) {
            if (!w.empty()) words.push_back(w);
        }
        const auto bad = [&](std::string_view why) {
            return makeError(
                ErrorKind::ParseFailure,
                format("script line %d: %s: %.*s", lineNumber, std::string{why}.c_str(),
                       static_cast<int>(line.size()), line.data()));
        };
        // The first `count` words as numbers.
        std::vector<float> numbers;
        const auto numeric = [&](std::size_t count) {
            numbers.clear();
            if (words.size() < count) return false;
            for (std::size_t i = 0; i < count; ++i) {
                const auto v = parseNumber<double>(words[i]);
                if (!v) return false;
                numbers.push_back(static_cast<float>(*v));
            }
            return true;
        };

        if (command == "wait") {
            if (!numeric(1) || numbers[0] < 0.0F) return bad("wait needs seconds");
            steps.push_back({.kind = Kind::Wait, .seconds = static_cast<double>(numbers[0])});
        } else if (command == "move") {
            if (!numeric(2)) return bad("move needs x y");
            move(numbers[0], numbers[1]);
            frame();
        } else if (command == "click") {
            if (!numeric(2)) return bad("click needs x y");
            int b = ImGuiMouseButton_Left;
            int clicks = 1;
            for (std::size_t i = 2; i < words.size(); ++i) {
                if (words[i] == "double") {
                    clicks = 2;
                } else if ((b = buttonNamed(words[i])) < 0) {
                    return bad("unknown button");
                }
            }
            move(numbers[0], numbers[1]);
            frame();
            for (int c = 0; c < clicks; ++c) {
                button(b, true);
                frame();
                button(b, false);
                frame();
            }
        } else if (command == "drag") {
            if (!numeric(4)) return bad("drag needs x1 y1 x2 y2");
            const int b = words.size() > 4 ? buttonNamed(words[4]) : ImGuiMouseButton_Left;
            if (b < 0) return bad("unknown button");
            const float x1 = numbers[0];
            const float y1 = numbers[1];
            move(x1, y1);
            frame();
            button(b, true);
            frame();
            constexpr int kSteps = 12;
            for (int i = 1; i <= kSteps; ++i) {
                const float t = static_cast<float>(i) / kSteps;
                move(x1 + (numbers[2] - x1) * t, y1 + (numbers[3] - y1) * t);
                frame();
            }
            button(b, false);
            frame();
        } else if (command == "scroll") {
            if (!numeric(3)) return bad("scroll needs x y notches");
            move(numbers[0], numbers[1]);
            frame();
            steps.push_back({.kind = Kind::Wheel, .y = numbers[2]});
            frame();
        } else if (command == "key") {
            if (words.size() != 1) return bad("key needs one chord, e.g. ctrl+f");
            std::vector<ImGuiKey> mods;
            ImGuiKey key = ImGuiKey_None;
            for (const auto part : split(words[0], '+', false)) {
                if (const ImGuiKey m = modifierNamed(part); m != ImGuiKey_None) {
                    mods.push_back(m);
                } else if ((key = keyNamed(part)) == ImGuiKey_None) {
                    return bad("unknown key");
                }
            }
            if (key == ImGuiKey_None) return bad("key needs a key besides modifiers");
            for (const ImGuiKey m : mods)
                steps.push_back({.kind = Kind::Key, .down = true, .key = m});
            steps.push_back({.kind = Kind::Key, .down = true, .key = key});
            frame();
            steps.push_back({.kind = Kind::Key, .down = false, .key = key});
            for (const ImGuiKey m : mods)
                steps.push_back({.kind = Kind::Key, .down = false, .key = m});
            frame();
        } else if (command == "hold" || command == "release") {
            const ImGuiKey m = words.size() == 1 ? modifierNamed(words[0]) : ImGuiKey_None;
            if (m == ImGuiKey_None) return bad("hold/release needs ctrl, shift, alt or super");
            steps.push_back({.kind = Kind::Key, .down = command == "hold", .key = m});
            frame();
        } else if (command == "type") {
            if (rest.empty()) return bad("type needs text");
            steps.push_back({.kind = Kind::Text, .text = std::string{rest}});
            frame();
        } else if (command == "panel" || command == "package" || command == "shot") {
            if (rest.empty()) return bad(command + " needs an argument");
            const Kind kind = command == "panel"     ? Kind::Panel
                              : command == "package" ? Kind::Package
                                                     : Kind::Shot;
            steps.push_back({.kind = kind, .text = std::string{rest}});
            frame();
        } else if (command == "quit") {
            steps.push_back({.kind = Kind::Quit});
        } else {
            return bad("unknown command");
        }
    }
    return steps;
}

}  // namespace em
