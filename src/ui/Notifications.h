#pragma once

// Toast notifications (vendor/ImGuiNotify), bottom-right of the main window.
//
// For things worth knowing about while looking at something else: a transfer
// finished, a phone was plugged in, an action on the device failed. Not a
// replacement for the inline status text panels already show -- a toast is
// gone after a few seconds.
//
// post() is safe from any thread; the toast appears on the next UI frame.

#include <functional>
#include <string>

namespace em::notify {

enum class Kind { Info, Success, Warning, Error };

void post(Kind kind, std::string title, std::string text = {});
// A toast with a button, e.g. "Show in Finder". `onPress` runs on the UI thread.
void postWithAction(Kind kind, std::string title, std::string text, std::string buttonLabel,
                    std::function<void()> onPress);

inline void info(std::string title, std::string text = {}) { post(Kind::Info, std::move(title), std::move(text)); }
inline void success(std::string title, std::string text = {}) { post(Kind::Success, std::move(title), std::move(text)); }
inline void warning(std::string title, std::string text = {}) { post(Kind::Warning, std::move(title), std::move(text)); }
inline void error(std::string title, std::string text = {}) { post(Kind::Error, std::move(title), std::move(text)); }

// Off: post() is dropped. Saved in emeron.ini by Application.
void setEnabled(bool enabled);
[[nodiscard]] bool enabled();

// UI thread, once per frame after the panels: moves queued toasts into
// ImGuiNotify and draws them.
void render();

}  // namespace em::notify
