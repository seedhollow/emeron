#pragma once

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include <imgui.h>

#include "collect/FileSystemBrowser.h"
#include <IconsFontAwesome6.h>

#include "ui/Panel.h"

namespace em {

// Device file manager.
//
// Selection uses ImGui's multi-select (click, Ctrl/Cmd-click, Shift-click,
// box-select, Ctrl+A, Escape). Every action works on the selection, so the
// toolbar, the context menus, the shortcuts and drag-and-drop all funnel into
// the same few methods.
//
// Drag-and-drop:
//   * within the panel  -- drag rows onto a folder row, the Up button or a
//                          breadcrumb to move them; hold Alt/Option to copy.
//   * from the computer -- drop files or folders from Finder/Explorer onto any
//                          emeron window to upload into the current folder.
// Dragging *out* of emeron onto the desktop is not possible: GLFW can receive
// OS drops but cannot originate one. "Download" is the way out.
class FileExplorerPanel final : public Panel {
public:
    static constexpr const char* kId = "Device Files";

    FileExplorerPanel();

    [[nodiscard]] std::string_view id() const noexcept override { return kId; }
    [[nodiscard]] const char* icon() const noexcept override { return ICON_FA_FOLDER_OPEN; }
    void draw(AppContext& context) override;
    void onDeviceChanged(AppContext& context) override;

private:
    // --- navigation ---------------------------------------------------------
    void navigate(AppContext& context, std::string path, bool recordHistory = true);
    void reload(AppContext& context);
    void goBack(AppContext& context);
    void goForward(AppContext& context);
    [[nodiscard]] std::string currentDir() const;

    // --- drawing ------------------------------------------------------------
    void drawNavigationBar(AppContext& context);
    void drawActionBar(AppContext& context);
    void drawStatus(AppContext& context);
    void drawEntries(AppContext& context);
    void drawRowContextMenu(AppContext& context, const FileEntry& entry);
    void drawBackgroundContextMenu(AppContext& context);
    void drawModals(AppContext& context);
    void handleShortcuts(AppContext& context);
    void acceptOsDrops(AppContext& context);

    // A folder location that accepts dragged rows, e.g. a breadcrumb.
    void acceptRowDrop(AppContext& context, const std::string& destDir);

    // --- selection ----------------------------------------------------------
    void rebuildVisible();
    void applySort();
    [[nodiscard]] static ImGuiID entryId(std::string_view name) noexcept;
    [[nodiscard]] bool isSelected(const FileEntry& entry) const;
    [[nodiscard]] std::vector<FileRef> selectedRefs() const;
    [[nodiscard]] const FileEntry* singleSelection() const;
    [[nodiscard]] FileRef refFor(const FileEntry& entry) const;
    void selectOnly(const FileEntry& entry);
    void selectAll();

    // --- actions ------------------------------------------------------------
    void copySelection(bool cut);
    void paste(AppContext& context, const std::string& destDir);
    void duplicateSelection(AppContext& context);
    // Opens the OS Save / Choose Folder dialog. Deferred through the
    // Dispatcher so the blocking dialog runs between frames, not inside one.
    void downloadSelection(AppContext& context);
    void chooseAndDownload(AppContext& context, std::vector<FileRef> refs);
    void downloadToWorkspace(AppContext& context);
    void downloadInto(AppContext& context, std::vector<FileRef> refs,
                      const std::filesystem::path& folder);
    [[nodiscard]] std::filesystem::path downloadStart(AppContext& context) const;
    void requestDelete();
    void requestRename();
    void requestNewFolder();
    void openEntry(AppContext& context, const FileEntry& entry);
    void onOperationDone(AppContext& context, TransferStatus status);
    // Records the status and, once it is finished, posts a toast.
    void setStatus(AppContext& context, TransferStatus status);
    [[nodiscard]] FileSystemBrowser::Callback reloadAfter(AppContext& context);

    DirListing listing_;
    std::vector<std::size_t> visible_;  // indices into listing_.entries, filtered
    ImGuiSelectionBasicStorage selection_;

    std::string pathInput_ = "/sdcard";
    std::string filter_;
    std::vector<std::string> back_;
    std::vector<std::string> forward_;

    struct Clipboard {
        std::vector<FileRef> items;
        TransferMode mode = TransferMode::Copy;
        std::string sourceDir;
        std::set<std::string> names;  // for dimming cut rows in place
    };
    Clipboard clipboard_;

    std::vector<FileRef> dragItems_;

    // Modal state. `openRequest_` is consumed by drawModals(), which keeps every
    // OpenPopup on the same ID stack as its BeginPopupModal.
    const char* openRequest_ = nullptr;
    std::vector<FileRef> pendingDelete_;
    std::string renameTarget_;
    std::string renameBuffer_;
    std::string newFolderBuffer_;
    std::vector<std::filesystem::path> pendingUpload_;
    std::string uploadPathBuffer_;
    std::string gotoBuffer_;

    // In-app fallback when no native dialog is available.
    std::vector<FileRef> pendingDownload_;
    std::string downloadPathBuffer_;
    std::string downloadDialogError_;

    TransferStatus lastStatus_;

    int sortColumn_ = 0;
    bool sortAscending_ = true;
    bool sortDirty_ = true;
    bool initialised_ = false;
    bool showHidden_ = true;
};

}  // namespace em
