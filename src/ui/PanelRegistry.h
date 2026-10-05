#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ui/Panel.h"

namespace em {

class PanelRegistry {
public:
    template <typename T, typename... Args>
    T& add(Args&&... args) {
        auto panel = std::make_unique<T>(std::forward<Args>(args)...);
        T& reference = *panel;
        panels_.push_back(std::move(panel));
        return reference;
    }

    // Draws every visible panel, and dispatches device-change notifications.
    void drawAll(AppContext& context);

    // Renders the View menu entries that toggle each panel.
    void drawViewMenu();

    [[nodiscard]] Panel* find(std::string_view id);
    void setAllVisible(bool value);

    void shutdown(AppContext& context);

    [[nodiscard]] const std::vector<std::unique_ptr<Panel>>& panels() const noexcept {
        return panels_;
    }

private:
    std::vector<std::unique_ptr<Panel>> panels_;
};

}  // namespace em
