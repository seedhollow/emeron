#pragma once

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include <IconsFontAwesome6.h>
#include <imgui.h>

#include "collect/LayoutInspector.h"
#include "ui/Panel.h"

namespace em {

// The View hierarchy of whatever is on screen, from any app: a component tree,
// the screenshot with every view outlined (2D, or exploded in 3D), redlines
// between views, properties in px and dp, test selectors to copy, and an audit
// of touch targets, labels and layout cost.
class LayoutInspectorPanel final : public Panel {
public:
    static constexpr const char* kId = "Layout";

    [[nodiscard]] std::string_view id() const noexcept override { return kId; }
    [[nodiscard]] const char* icon() const noexcept override { return ICON_FA_SITEMAP; }
    void draw(AppContext& context) override;
    void onDeviceChanged(AppContext& context) override;
    void onShutdown(AppContext& context) override;

    // Starts a capture whose result becomes the shared focus snapshot; used by
    // the Screen panel's Inspect mode too. False while one is running.
    static bool requestCapture(AppContext& context, LayoutSource source);

private:
    void drawToolbar(AppContext& context);
    void drawTree(AppContext& context);
    void drawCanvas(AppContext& context);
    void drawCanvas2D(AppContext& context, ImVec2 origin, ImVec2 size);
    void drawCanvas3D(AppContext& context, ImVec2 origin, ImVec2 size);
    void drawProperties(AppContext& context);
    void drawAudit(AppContext& context);
    void exportSnapshot(AppContext& context);
    void tapOnDevice(AppContext& context, int node);

    void syncSnapshot();  // picks up a new focus snapshot
    void rebuildRows();
    void uploadScreenshot();
    void releaseTexture();
    void select(int node, bool reveal);
    [[nodiscard]] bool visibleInTree(int node) const;

    LayoutSource source_ = LayoutSource::Merged;
    bool live_ = false;
    double nextLive_ = 0.0;
    bool prefsLoaded_ = false;
    bool autoCaptured_ = false;

    std::shared_ptr<const LayoutSnapshot> snapshot_;  // what rows_ etc. were built for
    int window_ = -2;
    std::vector<int> roots_;

    // The tree, flattened to what is expanded and matches the filter.
    struct Row {
        int node;
        int depth;
        bool match;  // matches the filter itself, not just an ancestor of a match
    };
    std::vector<Row> rows_;
    std::unordered_set<std::string> collapsed_;  // by nodeKey(), so it survives re-captures
    bool rowsDirty_ = true;
    std::string filter_;
    bool showHidden_ = false;
    bool focusFilter_ = false;

    // Canvas.
    bool mode3D_ = false;
    bool wireframe_ = true;
    bool showScreenshot_ = true;
    float zoom_ = 1.0F;  // on top of fit-to-panel
    ImVec2 pan_{0.0F, 0.0F};
    float yaw_ = -0.55F;  // 3D, radians
    float pitch_ = 0.25F;
    float spacing_ = 28.0F;  // 3D layer distance, in screen points at zoom 1

    unsigned int texture_ = 0;
    std::shared_ptr<const ScreenFrame> textureFrame_;

    std::vector<LayoutIssue> issues_;
};

}  // namespace em
