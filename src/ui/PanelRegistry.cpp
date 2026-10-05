#include "ui/PanelRegistry.h"

#include <imgui.h>

namespace em {

void PanelRegistry::drawAll(AppContext& context) {
    if (context.deviceChangedThisFrame) {
        for (auto& panel : panels_) panel->onDeviceChanged(context);
    }
    for (auto& panel : panels_) {
        if (panel->visible()) panel->draw(context);
    }
}

void PanelRegistry::drawViewMenu() {
    for (auto& panel : panels_) {
        const std::string label = std::string{panel->icon()} + "  " + std::string{panel->menuLabel()};
        ImGui::MenuItem(label.c_str(), nullptr, panel->visibleFlag());
    }
}

Panel* PanelRegistry::find(std::string_view id) {
    for (auto& panel : panels_) {
        if (panel->id() == id) return panel.get();
    }
    return nullptr;
}

void PanelRegistry::setAllVisible(bool value) {
    for (auto& panel : panels_) panel->setVisible(value);
}

void PanelRegistry::shutdown(AppContext& context) {
    for (auto& panel : panels_) panel->onShutdown(context);
}

}  // namespace em
