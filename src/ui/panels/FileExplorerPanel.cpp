#include "ui/panels/FileExplorerPanel.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

#include <IconsFontAwesome6.h>
#include <imgui_stdlib.h>

#include "adb/DeviceManager.h"
#include "app/NativeDialogs.h"
#include "app/Theme.h"
#include "core/CrashHandler.h"
#include "core/IniFile.h"
#include "core/StringUtil.h"
#include "core/TaskQueue.h"
#include "ui/FileIcons.h"
#include "ui/Widgets.h"

namespace em {
namespace {

// Payload type for rows dragged within the panel. The payload carries only a
// marker; the dragged items live in dragItems_, captured when the drag starts.
constexpr const char* kRowPayload = "EM_DEVICE_FILES";
constexpr int kPayloadMarker = 1;

constexpr const char* kDeletePopup = "Delete";
constexpr const char* kRenamePopup = "Rename";
constexpr const char* kNewFolderPopup = "New folder";
constexpr const char* kUploadPopup = "Upload";
constexpr const char* kUploadPathPopup = "Upload from path";
constexpr const char* kGotoPopup = "##goto";
constexpr const char* kDownloadToPopup = "Download to";
constexpr const char* kDownloadDirKey = "download_dir";

enum Column : int { kName = 0, kSize, kPermissions, kOwner, kModified };

using widgets::iconButtonWidth;
using widgets::sameLineOrWrap;

// Copy on Alt/Option (Finder) or Ctrl (Explorer); move otherwise.
TransferMode dropModeFromModifiers() {
    const ImGuiIO& io = ImGui::GetIO();
    return (io.KeyAlt || io.KeyCtrl) ? TransferMode::Copy : TransferMode::Move;
}

const char* revealLabel() {
#if defined(__APPLE__)
    return "Show in Finder";
#elif defined(_WIN32)
    return "Show in Explorer";
#else
    return "Open folder";
#endif
}

// Escape closes ImGui modals only when asked to.
void closeOnEscape() {
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
}

// Preferences are stored as UTF-8 so a non-ASCII folder round-trips on
// Windows, where std::filesystem::path::string() would be the ANSI code page.
std::string toUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string{reinterpret_cast<const char*>(text.data()), text.size()};
}

std::filesystem::path fromUtf8(std::string_view text) {
    return std::filesystem::path{
        std::u8string{reinterpret_cast<const char8_t*>(text.data()), text.size()}};
}

std::filesystem::path userDownloadsDir() {
#if defined(_WIN32)
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    return home == nullptr ? std::filesystem::path{}
                           : std::filesystem::path{home} / "Downloads";
}

std::string itemCount(std::size_t n) {
    return std::to_string(n) + (n == 1 ? " item" : " items");
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction and navigation
// ---------------------------------------------------------------------------

FileExplorerPanel::FileExplorerPanel() {
    // Selection is stored by name hash, not row index: indices shift on every
    // sort, filter or reload, and a selection that silently moves to a
    // different file before Delete is exactly the bug to avoid.
    selection_.UserData = this;
    selection_.AdapterIndexToStorageId = [](ImGuiSelectionBasicStorage* self, int idx) {
        const auto* panel = static_cast<const FileExplorerPanel*>(self->UserData);
        const std::size_t entry = panel->visible_[static_cast<std::size_t>(idx)];
        return entryId(panel->listing_.entries[entry].name);
    };
}

void FileExplorerPanel::onDeviceChanged(AppContext& context) {
    (void)context;
    listing_ = DirListing{};
    visible_.clear();
    selection_.Clear();
    back_.clear();
    forward_.clear();
    // Device paths mean nothing on another device.
    clipboard_ = Clipboard{};
    dragItems_.clear();
    pendingUpload_.clear();
    lastStatus_ = TransferStatus{};
    initialised_ = false;
}

std::string FileExplorerPanel::currentDir() const {
    return listing_.path.empty() ? normalizeDevicePath(pathInput_) : listing_.path;
}

void FileExplorerPanel::navigate(AppContext& context, std::string path, bool recordHistory) {
    path = normalizeDevicePath(path);
    crash::setBreadcrumb("Device Files: opening " + path);
    const bool sameDir = path == listing_.path;

    if (recordHistory && !listing_.path.empty() && !sameDir) {
        back_.push_back(listing_.path);
        forward_.clear();
    }
    if (!sameDir) selection_.Clear();

    pathInput_ = path;
    listing_.path = path;
    listing_.loading = true;
    listing_.error.clear();

    context.files.list(context.pool, context.dispatcher, path, [this](DirListing result) {
        // A slow listing can land after the user has already moved on.
        if (result.path != listing_.path) return;
        listing_ = std::move(result);
        listing_.loading = false;
        // visible_ holds indices into listing_.entries, so it has to be
        // rebuilt here, in the same step that replaces the entries. Leaving it
        // to drawEntries() let the action bar -- drawn first -- walk the old
        // folder's indices over the new folder's entries for one frame, and
        // opening an empty or smaller folder read past the end and crashed.
        applySort();
        rebuildVisible();
    });
}

void FileExplorerPanel::reload(AppContext& context) {
    navigate(context, currentDir(), false);
}

void FileExplorerPanel::goBack(AppContext& context) {
    if (back_.empty()) return;
    forward_.push_back(currentDir());
    std::string target = std::move(back_.back());
    back_.pop_back();
    navigate(context, std::move(target), false);
}

void FileExplorerPanel::goForward(AppContext& context) {
    if (forward_.empty()) return;
    back_.push_back(currentDir());
    std::string target = std::move(forward_.back());
    forward_.pop_back();
    navigate(context, std::move(target), false);
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

ImGuiID FileExplorerPanel::entryId(std::string_view name) noexcept {
    // FNV-1a. Not ImGui::GetID(), which depends on the ID stack at call time.
    std::uint32_t hash = 2166136261U;
    for (const char c : name) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= 16777619U;
    }
    return hash == 0 ? 1U : hash;
}

bool FileExplorerPanel::isSelected(const FileEntry& entry) const {
    return selection_.Contains(entryId(entry.name));
}

FileRef FileExplorerPanel::refFor(const FileEntry& entry) const {
    return FileRef{joinDevicePath(currentDir(), entry.name), entry.isDirectory()};
}

std::vector<FileRef> FileExplorerPanel::selectedRefs() const {
    // Visible rows only: acting on something the filter is hiding would be a
    // surprise, and Delete is the last place for one.
    std::vector<FileRef> refs;
    for (const std::size_t index : visible_) {
        if (index >= listing_.entries.size()) continue;
        const FileEntry& entry = listing_.entries[index];
        if (isSelected(entry)) refs.push_back(refFor(entry));
    }
    return refs;
}

const FileEntry* FileExplorerPanel::singleSelection() const {
    const FileEntry* found = nullptr;
    for (const std::size_t index : visible_) {
        if (index >= listing_.entries.size()) continue;
        const FileEntry& entry = listing_.entries[index];
        if (!isSelected(entry)) continue;
        if (found != nullptr) return nullptr;
        found = &entry;
    }
    return found;
}

void FileExplorerPanel::selectOnly(const FileEntry& entry) {
    selection_.Clear();
    selection_.SetItemSelected(entryId(entry.name), true);
}

void FileExplorerPanel::selectAll() {
    selection_.Clear();
    for (const std::size_t index : visible_) {
        if (index >= listing_.entries.size()) continue;
        selection_.SetItemSelected(entryId(listing_.entries[index].name), true);
    }
}

void FileExplorerPanel::rebuildVisible() {
    visible_.clear();
    visible_.reserve(listing_.entries.size());
    for (std::size_t i = 0; i < listing_.entries.size(); ++i) {
        const FileEntry& entry = listing_.entries[i];
        if (!showHidden_ && entry.name.starts_with('.')) continue;
        if (!filter_.empty() && !containsIgnoreCase(entry.name, filter_)) continue;
        visible_.push_back(i);
    }
}

void FileExplorerPanel::applySort() {
    const int column = sortColumn_;
    const bool ascending = sortAscending_;

    std::stable_sort(listing_.entries.begin(), listing_.entries.end(),
                     [column, ascending](const FileEntry& a, const FileEntry& b) {
                         // Folders stay on top whichever way the column sorts.
                         if (a.isDirectory() != b.isDirectory()) return a.isDirectory();

                         int order = 0;
                         switch (column) {
                             case kSize:
                                 order = a.size < b.size ? -1 : (a.size > b.size ? 1 : 0);
                                 break;
                             case kPermissions:
                                 order = a.permissions.compare(b.permissions);
                                 break;
                             case kOwner:
                                 order = a.owner.compare(b.owner);
                                 break;
                             case kModified:
                                 // ISO timestamps sort correctly as text.
                                 order = a.modified.compare(b.modified);
                                 break;
                             case kName:
                             default:
                                 break;
                         }
                         if (order == 0) order = toLower(a.name).compare(toLower(b.name));
                         return ascending ? order < 0 : order > 0;
                     });
    sortDirty_ = false;
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

FileSystemBrowser::Callback FileExplorerPanel::reloadAfter(AppContext& context) {
    return [this, &context](TransferStatus status) {
        onOperationDone(context, std::move(status));
    };
}

void FileExplorerPanel::onOperationDone(AppContext& context, TransferStatus status) {
    lastStatus_ = std::move(status);
    reload(context);
}

void FileExplorerPanel::copySelection(bool cut) {
    auto refs = selectedRefs();
    if (refs.empty()) return;

    clipboard_ = Clipboard{};
    clipboard_.mode = cut ? TransferMode::Move : TransferMode::Copy;
    clipboard_.sourceDir = currentDir();
    for (const auto& ref : refs) clipboard_.names.insert(deviceBaseName(ref.path));
    clipboard_.items = std::move(refs);

    lastStatus_ = TransferStatus{};
    lastStatus_.state = TransferStatus::State::Done;
    lastStatus_.description = cut ? "cut" : "copy";
    lastStatus_.message = itemCount(clipboard_.items.size()) + " on the clipboard";
}

void FileExplorerPanel::paste(AppContext& context, const std::string& destDir) {
    if (clipboard_.items.empty()) return;

    const TransferMode mode = clipboard_.mode;
    context.files.transfer(context.pool, context.dispatcher, clipboard_.items, destDir, mode,
                           reloadAfter(context));

    // A cut is consumed by its paste, as in every desktop file manager; a copy
    // can be pasted again.
    if (mode == TransferMode::Move) clipboard_ = Clipboard{};
}

void FileExplorerPanel::duplicateSelection(AppContext& context) {
    auto refs = selectedRefs();
    if (refs.empty()) return;
    // Copying into the source folder: planTransfer names these "x copy".
    context.files.transfer(context.pool, context.dispatcher, std::move(refs), currentDir(),
                           TransferMode::Copy, reloadAfter(context));
}

std::filesystem::path FileExplorerPanel::downloadStart(AppContext& context) const {
    const std::filesystem::path workspace = context.workspaceDir / "pulled";
    std::error_code ec;
    std::filesystem::create_directories(workspace, ec);

    std::vector<std::filesystem::path> candidates;
    if (const auto last = context.prefs.get(kDownloadDirKey))
        candidates.push_back(fromUtf8(*last));
    candidates.push_back(userDownloadsDir());
    candidates.push_back(workspace);
    return resolveDownloadStart(candidates);
}

void FileExplorerPanel::downloadSelection(AppContext& context) {
    auto refs = selectedRefs();
    if (refs.empty()) return;

    // Not now: this runs mid-frame, and a native dialog blocks. The dispatcher
    // is drained before the next NewFrame, which is a clean place to block.
    context.dispatcher.post([this, &context, refs = std::move(refs)]() mutable {
        chooseAndDownload(context, std::move(refs));
    });
}

void FileExplorerPanel::downloadInto(AppContext& context, std::vector<FileRef> refs,
                                     const std::filesystem::path& folder) {
    context.prefs.set(kDownloadDirKey, toUtf8(folder));
    context.files.pull(context.pool, context.dispatcher, std::move(refs), folder,
                       [this](TransferStatus status) { lastStatus_ = std::move(status); });
}

void FileExplorerPanel::chooseAndDownload(AppContext& context, std::vector<FileRef> refs) {
    const std::filesystem::path start = downloadStart(context);

    dialogs::Choice choice;
    const bool singleFile = refs.size() == 1 && !refs.front().isDirectory;

    if (singleFile) {
        // Save As for one file, so it can be renamed on the way out.
        const std::string name = deviceBaseName(refs.front().path);
        choice = dialogs::saveFile("Download " + name, start, name);
        if (choice.chosen()) {
            context.prefs.set(kDownloadDirKey, toUtf8(choice.path.parent_path()));
            context.files.pullAs(
                context.pool, context.dispatcher, refs.front(), choice.path,
                [this](TransferStatus status) { lastStatus_ = std::move(status); });
        }
    } else {
        // A folder or several items: choose where they go; names are kept and
        // collisions become "name copy".
        choice = dialogs::pickFolder("Download " + itemCount(refs.size()) + " to", start);
        if (choice.chosen()) downloadInto(context, std::move(refs), choice.path);
    }

    // The OS dialog consumed the key and mouse releases; without this ImGui can
    // believe a button is still held when the window regains focus.
    ImGui::GetIO().ClearInputKeys();
    ImGui::GetIO().ClearInputMouse();

    if (choice.outcome == dialogs::Outcome::Unavailable ||
        choice.outcome == dialogs::Outcome::Failed) {
        pendingDownload_ = std::move(refs);
        downloadPathBuffer_ = start.string();
        downloadDialogError_ = choice.error;
        openRequest_ = kDownloadToPopup;
    }
}

void FileExplorerPanel::downloadToWorkspace(AppContext& context) {
    auto refs = selectedRefs();
    if (refs.empty()) return;
    context.files.pull(context.pool, context.dispatcher, std::move(refs),
                       context.workspaceDir / "pulled",
                       [this](TransferStatus status) { lastStatus_ = std::move(status); });
}

void FileExplorerPanel::requestDelete() {
    pendingDelete_ = selectedRefs();
    if (!pendingDelete_.empty()) openRequest_ = kDeletePopup;
}

void FileExplorerPanel::requestRename() {
    const FileEntry* entry = singleSelection();
    if (entry == nullptr) return;
    renameTarget_ = joinDevicePath(currentDir(), entry->name);
    renameBuffer_ = entry->name;
    openRequest_ = kRenamePopup;
}

void FileExplorerPanel::requestNewFolder() {
    std::set<std::string> taken;
    for (const auto& entry : listing_.entries) taken.insert(entry.name);

    newFolderBuffer_ = "New Folder";
    for (int n = 2; taken.contains(newFolderBuffer_); ++n) {
        newFolderBuffer_ = "New Folder " + std::to_string(n);
    }
    openRequest_ = kNewFolderPopup;
}

void FileExplorerPanel::openEntry(AppContext& context, const FileEntry& entry) {
    if (entry.isDirectory()) {
        navigate(context, joinDevicePath(currentDir(), entry.name));
    } else if (entry.isSymlink() && !entry.linkTarget.empty()) {
        navigate(context, joinDevicePath(currentDir(), entry.linkTarget));
    }
}

void FileExplorerPanel::acceptRowDrop(AppContext& context, const std::string& destDir) {
    if (!ImGui::BeginDragDropTarget()) return;
    if (ImGui::AcceptDragDropPayload(kRowPayload) != nullptr && !dragItems_.empty()) {
        context.files.transfer(context.pool, context.dispatcher, dragItems_, destDir,
                               dropModeFromModifiers(), reloadAfter(context));
        dragItems_.clear();
    }
    ImGui::EndDragDropTarget();
}

void FileExplorerPanel::acceptOsDrops(AppContext& context) {
    if (context.droppedPaths.empty() || !context.hasDevice()) return;

    pendingUpload_ = context.droppedPaths;
    context.droppedPathsConsumed = true;
    openRequest_ = kUploadPopup;

    // Brings this panel forward if it is a background dock tab, so the
    // confirmation is not opened inside a window nobody can see.
    ImGui::SetWindowFocus(windowKey(kId).c_str());
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void FileExplorerPanel::drawNavigationBar(AppContext& context) {
    ImGui::BeginDisabled(back_.empty());
    if (ImGui::Button(ICON_FA_ARROW_LEFT)) goBack(context);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Back (Alt+Left)");

    ImGui::SameLine(0.0F, 2.0F);
    ImGui::BeginDisabled(forward_.empty());
    if (ImGui::Button(ICON_FA_ARROW_RIGHT)) goForward(context);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Forward (Alt+Right)");

    ImGui::SameLine();
    const std::string dir = currentDir();
    ImGui::BeginDisabled(dir == "/");
    if (ImGui::Button(ICON_FA_ARROW_UP)) navigate(context, parentDevicePath(dir));
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Enclosing folder (Backspace). Drop items here to move them up.");
    if (dir != "/") acceptRowDrop(context, parentDevicePath(dir));

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ARROWS_ROTATE)) reload(context);
    ImGui::SetItemTooltip("Reload (F5)");

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FOLDER_TREE)) {
        gotoBuffer_ = dir;
        ImGui::OpenPopup(kGotoPopup);
    }
    ImGui::SetItemTooltip("Go to a path or a common location (Ctrl+L)");
    if (ImGui::BeginPopup(kGotoPopup)) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(360.0F);
        if (ImGui::InputText(
                "##gotopath", &gotoBuffer_,
                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
            navigate(context, gotoBuffer_);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SeparatorText("Places");
        for (const auto& bookmark : FileSystemBrowser::bookmarkPaths()) {
            if (ImGui::Selectable(bookmark.c_str())) {
                navigate(context, bookmark);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    // --- breadcrumbs ---------------------------------------------------------
    // Each segment navigates on click and accepts dragged rows, which is how an
    // item gets moved more than one level up in a single drag.
    sameLineOrWrap(80.0F, 12.0F);
    std::string prefix;
    const auto parts = split(dir, '/', false);

    ImGui::PushID("crumbs");
    ImGui::PushID(0);
    if (ImGui::SmallButton("/")) navigate(context, "/");
    acceptRowDrop(context, "/");
    ImGui::PopID();

    for (std::size_t i = 0; i < parts.size(); ++i) {
        prefix += "/";
        prefix += parts[i];
        ImGui::SameLine(0.0F, 2.0F);
        ImGui::TextDisabled(">");
        ImGui::SameLine(0.0F, 2.0F);

        ImGui::PushID(static_cast<int>(i) + 1);
        if (ImGui::SmallButton(std::string{parts[i]}.c_str())) navigate(context, prefix);
        acceptRowDrop(context, prefix);
        ImGui::PopID();
    }
    ImGui::PopID();
}

void FileExplorerPanel::drawActionBar(AppContext& context) {
    const auto selected = selectedRefs();
    const bool any = !selected.empty();
    const bool single = singleSelection() != nullptr;

    // Icon buttons with the name and shortcut in the tooltip: the text labels
    // needed ~560 px and clipped in the default 340 px dock.
    if (ImGui::Button(ICON_FA_FOLDER_PLUS)) requestNewFolder();
    ImGui::SetItemTooltip("New folder (Ctrl+Shift+N)");

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_UPLOAD)) {
        uploadPathBuffer_.clear();
        openRequest_ = kUploadPathPopup;
    }
    ImGui::SetItemTooltip(
        "Upload from a path on this computer.\n"
        "Or drag files from Finder/Explorer onto this window.");

    ImGui::SameLine();
    ImGui::BeginDisabled(!any);
    if (ImGui::Button(ICON_FA_DOWNLOAD)) downloadSelection(context);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Download: choose where to save the selection on this computer");

    sameLineOrWrap(iconButtonWidth(ICON_FA_SCISSORS) * 5.0F, 12.0F);
    ImGui::BeginDisabled(!any);
    if (ImGui::Button(ICON_FA_SCISSORS)) copySelection(true);
    ImGui::SetItemTooltip("Cut (Ctrl+X)");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_COPY)) copySelection(false);
    ImGui::SetItemTooltip("Copy (Ctrl+C)");
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(clipboard_.items.empty());
    const std::string pasteLabel =
        clipboard_.items.empty()
            ? std::string{ICON_FA_PASTE}
            : std::string{ICON_FA_PASTE} + " " + std::to_string(clipboard_.items.size());
    if (ImGui::Button(pasteLabel.c_str())) paste(context, currentDir());
    ImGui::EndDisabled();
    if (clipboard_.items.empty()) {
        ImGui::SetItemTooltip("Paste (Ctrl+V)");
    } else {
        ImGui::SetItemTooltip(
            "Paste (Ctrl+V) -- %s %s from %s",
            clipboard_.mode == TransferMode::Move ? "move" : "copy",
            itemCount(clipboard_.items.size()).c_str(), clipboard_.sourceDir.c_str());
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!single);
    if (ImGui::Button(ICON_FA_PEN)) requestRename();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Rename (F2)");

    ImGui::SameLine();
    ImGui::BeginDisabled(!any);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::palette().bad);
    if (ImGui::Button(ICON_FA_TRASH)) requestDelete();
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Delete (Delete or Ctrl+Backspace)");

    // --- view ---------------------------------------------------------------
    const char* hiddenIcon = showHidden_ ? ICON_FA_EYE : ICON_FA_EYE_SLASH;
    constexpr float kMinFilterWidth = 110.0F;
    sameLineOrWrap(kMinFilterWidth + iconButtonWidth(hiddenIcon), 12.0F);
    ImGui::SetNextItemWidth(
        std::max(kMinFilterWidth, ImGui::GetContentRegionAvail().x - iconButtonWidth(hiddenIcon) -
                                      ImGui::GetStyle().ItemSpacing.x));
    ImGui::InputTextWithHint("##filter", ICON_FA_FILTER " filter", &filter_);
    ImGui::SameLine();
    if (ImGui::Button(hiddenIcon)) showHidden_ = !showHidden_;
    ImGui::SetItemTooltip(showHidden_ ? "Showing hidden files (dot files)"
                                      : "Hiding hidden files (dot files)");
}

void FileExplorerPanel::drawStatus(AppContext& context) {
    const auto& palette = theme::palette();

    const std::size_t selectedCount = static_cast<std::size_t>(selection_.Size);
    ImGui::TextDisabled("%zu of %zu shown", visible_.size(), listing_.entries.size());
    if (selectedCount > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("| %zu selected", selectedCount);
    }
    if (listing_.loading) {
        ImGui::SameLine();
        ImGui::TextDisabled("| loading...");
    }
    if (const std::size_t busy = context.files.activeOperations(); busy > 0) {
        ImGui::SameLine();
        ImGui::TextColored(palette.accent, "| working (%zu)...", busy);
    }

    if (lastStatus_.state != TransferStatus::State::Idle) {
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        const bool failed = lastStatus_.state == TransferStatus::State::Failed;
        ImGui::TextColored(failed ? palette.bad : palette.good, "%s: %s",
                           lastStatus_.description.c_str(), lastStatus_.message.c_str());
        if (!lastStatus_.details.empty() && ImGui::BeginItemTooltip()) {
            for (const auto& line : lastStatus_.details) ImGui::TextUnformatted(line.c_str());
            ImGui::EndTooltip();
        }
        if (!lastStatus_.localPath.empty() && !failed) {
            ImGui::SameLine();
            if (ImGui::SmallButton(revealLabel())) {
                FileSystemBrowser::revealOnHost(context.pool, lastStatus_.localPath);
            }
        }
    }

    if (!listing_.error.empty()) {
        ImGui::TextColored(listing_.permissionDenied ? palette.warning : palette.bad, "%s",
                           listing_.error.c_str());
        if (listing_.permissionDenied) {
            ImGui::TextWrapped(
                "App-private directories under /data/data are unreadable by the shell user on "
                "a production build. On a debuggable app you can reach them with "
                "`adb shell run-as <package> ls <path>`; on a userdebug build, `adb root` "
                "lifts the restriction entirely.");
        }
    }
}

void FileExplorerPanel::drawRowContextMenu(AppContext& context, const FileEntry& entry) {
    if (!ImGui::BeginPopupContextItem("##row")) return;

    const std::size_t count = selectedRefs().size();
    const std::string many = count > 1 ? " " + itemCount(count) : std::string{};

    if (entry.isDirectory() && count == 1 && ImGui::MenuItem("Open", "Enter")) {
        openEntry(context, entry);
    }
    if (ImGui::MenuItem(("Download" + many + "...").c_str())) downloadSelection(context);
    if (ImGui::MenuItem("Download to workspace")) downloadToWorkspace(context);
    ImGui::SetItemTooltip("Straight into %s, no dialog",
                          (context.workspaceDir / "pulled").string().c_str());
    ImGui::Separator();

    if (ImGui::MenuItem(("Cut" + many).c_str(), "Ctrl+X")) copySelection(true);
    if (ImGui::MenuItem(("Copy" + many).c_str(), "Ctrl+C")) copySelection(false);
    if (entry.isDirectory() && !clipboard_.items.empty() &&
        ImGui::MenuItem("Paste into folder")) {
        paste(context, joinDevicePath(currentDir(), entry.name));
    }
    if (ImGui::MenuItem(("Duplicate" + many).c_str())) duplicateSelection(context);
    if (count == 1 && ImGui::MenuItem("Rename...", "F2")) requestRename();
    ImGui::Separator();

    if (ImGui::MenuItem(count > 1 ? "Copy paths" : "Copy path")) {
        std::string text;
        for (const auto& ref : selectedRefs()) {
            if (!text.empty()) text += '\n';
            text += ref.path;
        }
        ImGui::SetClipboardText(text.c_str());
    }
    ImGui::Separator();

    ImGui::PushStyleColor(ImGuiCol_Text, theme::palette().bad);
    if (ImGui::MenuItem(("Delete" + many + "...").c_str(), "Del")) requestDelete();
    ImGui::PopStyleColor();

    ImGui::EndPopup();
}

void FileExplorerPanel::drawBackgroundContextMenu(AppContext& context) {
    if (!ImGui::BeginPopupContextWindow("##background", ImGuiPopupFlags_MouseButtonRight |
                                                            ImGuiPopupFlags_NoOpenOverItems)) {
        return;
    }
    ImGui::BeginDisabled(clipboard_.items.empty());
    if (ImGui::MenuItem("Paste", "Ctrl+V")) paste(context, currentDir());
    ImGui::EndDisabled();
    ImGui::Separator();
    if (ImGui::MenuItem("New folder...", "Ctrl+Shift+N")) requestNewFolder();
    if (ImGui::MenuItem("Upload from path...")) {
        uploadPathBuffer_.clear();
        openRequest_ = kUploadPathPopup;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Select all", "Ctrl+A")) selectAll();
    if (ImGui::MenuItem("Reload", "F5")) reload(context);
    if (ImGui::MenuItem("Copy folder path")) ImGui::SetClipboardText(currentDir().c_str());
    ImGui::EndPopup();
}

void FileExplorerPanel::drawEntries(AppContext& context) {
    const auto& palette = theme::palette();

    constexpr ImGuiTableFlags kFlags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_Hideable |
        ImGuiTableFlags_SizingStretchProp;

    if (!ImGui::BeginTable("##files", 5, kFlags)) return;

    // The fixed columns used to total 430 px, more than the default dock is
    // wide, which squeezed Name -- the one column that matters -- to nothing.
    // Permissions and Owner now start hidden; right-click a header to show them.
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch |
                                        ImGuiTableColumnFlags_DefaultSort |
                                        ImGuiTableColumnFlags_NoHide);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 64.0F);
    ImGui::TableSetupColumn("Permissions",
                            ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide,
                            90.0F);
    ImGui::TableSetupColumn("Owner",
                            ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide,
                            100.0F);
    ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed, 112.0F);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    // The table was flagged Sortable before, but nothing read the specs, so the
    // header arrows changed and the rows did not.
    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs != nullptr) {
        if (specs->SpecsDirty && specs->SpecsCount > 0) {
            sortColumn_ = specs->Specs[0].ColumnIndex;
            sortAscending_ = specs->Specs[0].SortDirection != ImGuiSortDirection_Descending;
            sortDirty_ = true;
            specs->SpecsDirty = false;
        }
    }
    if (sortDirty_) applySort();
    rebuildVisible();

    const bool cutHere = clipboard_.mode == TransferMode::Move && !clipboard_.items.empty() &&
                         clipboard_.sourceDir == currentDir();

    constexpr ImGuiMultiSelectFlags kSelectFlags = ImGuiMultiSelectFlags_ClearOnEscape |
                                                   ImGuiMultiSelectFlags_ClearOnClickVoid |
                                                   ImGuiMultiSelectFlags_BoxSelect1d;
    const int itemCountInt = static_cast<int>(visible_.size());
    ImGuiMultiSelectIO* io =
        ImGui::BeginMultiSelect(kSelectFlags, selection_.Size, itemCountInt);
    selection_.ApplyRequests(io);

    ImGuiListClipper clipper;
    clipper.Begin(itemCountInt);
    // The shift-click anchor must be submitted even when scrolled out of view.
    if (io->RangeSrcItem != -1) clipper.IncludeItemByIndex(static_cast<int>(io->RangeSrcItem));

    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const FileEntry& entry = listing_.entries[visible_[static_cast<std::size_t>(row)]];
            const bool selected = isSelected(entry);
            const bool dimmed = cutHere && clipboard_.names.contains(entry.name);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(entry.name.c_str());

            // The selectable carries no label so the icon can have its own
            // colour; icon and name are drawn over it once every query about
            // the selectable (drag, drop, context menu) has been made.
            ImGui::SetNextItemSelectionUserData(row);
            const ImVec2 cellStart = ImGui::GetCursorPos();
            ImGui::Selectable(
                "##row", selected,
                ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick);

            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                openEntry(context, entry);
            }
            // Right-clicking an unselected row acts on that row, not on
            // whatever happened to be selected before.
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !selected) selectOnly(entry);

            if (!entry.linkTarget.empty())
                ImGui::SetItemTooltip("-> %s", entry.linkTarget.c_str());

            // --- drag source ---------------------------------------------
            if (ImGui::BeginDragDropSource()) {
                if (ImGui::GetDragDropPayload() == nullptr) {
                    // First frame of the drag: dragging a selected row carries
                    // the whole selection, an unselected one only itself.
                    dragItems_ =
                        selected ? selectedRefs() : std::vector<FileRef>{refFor(entry)};
                }
                ImGui::SetDragDropPayload(kRowPayload, &kPayloadMarker, sizeof(kPayloadMarker),
                                          ImGuiCond_Once);
                const bool copying = dropModeFromModifiers() == TransferMode::Copy;
                ImGui::Text("%s %s", copying ? "Copy" : "Move",
                            dragItems_.size() == 1
                                ? deviceBaseName(dragItems_.front().path).c_str()
                                : itemCount(dragItems_.size()).c_str());
                ImGui::TextDisabled(
                    "%s", copying ? "release Alt/Option to move" : "hold Alt/Option to copy");
                ImGui::EndDragDropSource();
            }

            // --- drop target: real folders that are not being dragged -----
            const bool beingDragged =
                std::any_of(dragItems_.begin(), dragItems_.end(), [&](const FileRef& ref) {
                    return deviceBaseName(ref.path) == entry.name &&
                           parentDevicePath(ref.path) == currentDir();
                });
            if (entry.isDirectory() && !beingDragged) {
                acceptRowDrop(context, joinDevicePath(currentDir(), entry.name));
            }

            drawRowContextMenu(context, entry);

            const bool folderLike = entry.isDirectory() || entry.isSymlink();
            ImGui::SetCursorPos(cellStart);
            ImGui::TextColored(dimmed       ? palette.muted
                               : folderLike ? palette.accent
                                            : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
                               "%s", fileIcon(entry.kind, entry.name));
            ImGui::SameLine();
            if (dimmed) {
                ImGui::TextColored(palette.muted, "%s", entry.name.c_str());
            } else {
                ImGui::TextUnformatted(entry.name.c_str());
            }

            ImGui::TableNextColumn();
            if (entry.isDirectory()) {
                ImGui::TextDisabled("--");
            } else {
                ImGui::TextUnformatted(humanBytes(static_cast<double>(entry.size)).c_str());
            }
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", entry.permissions.c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s:%s", entry.owner.c_str(), entry.group.c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", entry.modified.c_str());

            ImGui::PopID();
        }
    }

    io = ImGui::EndMultiSelect();
    selection_.ApplyRequests(io);

    drawBackgroundContextMenu(context);
    ImGui::EndTable();

    // A drag that ended anywhere other than a target leaves nothing behind.
    if (ImGui::GetDragDropPayload() == nullptr &&
        !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        dragItems_.clear();
    }
}

void FileExplorerPanel::handleShortcuts(AppContext& context) {
    // Typing into the filter or a path field owns the keyboard.
    if (ImGui::GetIO().WantTextInput) return;

    // RouteFocused (the default) fires only while this panel, or the table
    // inside it, has focus, and never behind an open modal.
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C)) copySelection(false);
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_X)) copySelection(true);
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_V)) paste(context, currentDir());
    if (ImGui::Shortcut(ImGuiKey_Delete) ||
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Backspace)) {
        requestDelete();
    }
    if (ImGui::Shortcut(ImGuiKey_F2)) requestRename();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_N)) requestNewFolder();
    if (ImGui::Shortcut(ImGuiKey_F5)) reload(context);
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_L)) {
        gotoBuffer_ = currentDir();
        ImGui::OpenPopup(kGotoPopup);
    }
    if (ImGui::Shortcut(ImGuiMod_Alt | ImGuiKey_LeftArrow)) goBack(context);
    if (ImGui::Shortcut(ImGuiMod_Alt | ImGuiKey_RightArrow)) goForward(context);
    if (ImGui::Shortcut(ImGuiKey_Backspace) ||
        ImGui::Shortcut(ImGuiMod_Alt | ImGuiKey_UpArrow)) {
        if (currentDir() != "/") navigate(context, parentDevicePath(currentDir()));
    }
    if (ImGui::Shortcut(ImGuiKey_Enter) || ImGui::Shortcut(ImGuiKey_KeypadEnter)) {
        if (const FileEntry* entry = singleSelection()) openEntry(context, *entry);
    }
}

void FileExplorerPanel::drawModals(AppContext& context) {
    const auto& palette = theme::palette();

    if (openRequest_ != nullptr) {
        ImGui::OpenPopup(openRequest_);
        openRequest_ = nullptr;
    }

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    const auto centerNext = [&center] {
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2{0.5F, 0.5F});
    };

    // --- delete --------------------------------------------------------------
    centerNext();
    if (ImGui::BeginPopupModal(kDeletePopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const bool hasFolder = std::any_of(pendingDelete_.begin(), pendingDelete_.end(),
                                           [](const FileRef& ref) { return ref.isDirectory; });

        ImGui::Text("Delete %s from the device?", itemCount(pendingDelete_.size()).c_str());
        ImGui::TextColored(palette.bad, "This cannot be undone.");
        if (hasFolder) {
            ImGui::TextColored(palette.warning,
                               "Folders are deleted with everything inside them.");
        }
        ImGui::Spacing();

        constexpr std::size_t kShown = 10;
        for (std::size_t i = 0; i < pendingDelete_.size() && i < kShown; ++i) {
            ImGui::BulletText("%s%s", pendingDelete_[i].path.c_str(),
                              pendingDelete_[i].isDirectory ? "/" : "");
        }
        if (pendingDelete_.size() > kShown) {
            ImGui::TextDisabled("...and %zu more", pendingDelete_.size() - kShown);
        }
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Button, palette.bad);
        const bool confirmed = ImGui::Button("Delete", ImVec2{110.0F, 0.0F});
        ImGui::PopStyleColor();
        ImGui::SameLine();
        // Cancel holds the default focus, so a stray Enter cannot delete.
        const bool cancelled = ImGui::Button("Cancel", ImVec2{110.0F, 0.0F});
        ImGui::SetItemDefaultFocus();

        if (confirmed) {
            context.files.remove(context.pool, context.dispatcher, std::move(pendingDelete_),
                                 reloadAfter(context));
            pendingDelete_.clear();
            selection_.Clear();
            ImGui::CloseCurrentPopup();
        } else if (cancelled) {
            pendingDelete_.clear();
            ImGui::CloseCurrentPopup();
        }
        closeOnEscape();
        ImGui::EndPopup();
    }

    // --- rename --------------------------------------------------------------
    centerNext();
    if (ImGui::BeginPopupModal(kRenamePopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const std::string original = deviceBaseName(renameTarget_);
        ImGui::Text("Rename %s", original.c_str());

        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(360.0F);
        const bool entered = ImGui::InputText(
            "##rename", &renameBuffer_,
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);

        const auto problem = validateFileName(renameBuffer_);
        const bool unchanged = renameBuffer_ == original;
        if (problem) ImGui::TextColored(palette.bad, "%s", problem->c_str());

        ImGui::BeginDisabled(problem.has_value() || unchanged);
        const bool confirmed = ImGui::Button("Rename", ImVec2{110.0F, 0.0F}) ||
                               (entered && !problem && !unchanged);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2{110.0F, 0.0F})) ImGui::CloseCurrentPopup();

        if (confirmed) {
            context.files.rename(context.pool, context.dispatcher, renameTarget_, renameBuffer_,
                                 reloadAfter(context));
            selection_.Clear();
            selection_.SetItemSelected(entryId(renameBuffer_), true);
            ImGui::CloseCurrentPopup();
        }
        closeOnEscape();
        ImGui::EndPopup();
    }

    // --- new folder ----------------------------------------------------------
    centerNext();
    if (ImGui::BeginPopupModal(kNewFolderPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("New folder in %s", currentDir().c_str());

        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(360.0F);
        const bool entered = ImGui::InputText(
            "##newfolder", &newFolderBuffer_,
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        const auto problem = validateFileName(newFolderBuffer_);
        if (problem) ImGui::TextColored(palette.bad, "%s", problem->c_str());

        ImGui::BeginDisabled(problem.has_value());
        const bool confirmed =
            ImGui::Button("Create", ImVec2{110.0F, 0.0F}) || (entered && !problem);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2{110.0F, 0.0F})) ImGui::CloseCurrentPopup();

        if (confirmed) {
            context.files.makeDirectory(context.pool, context.dispatcher, currentDir(),
                                        newFolderBuffer_, reloadAfter(context));
            selection_.Clear();
            selection_.SetItemSelected(entryId(newFolderBuffer_), true);
            ImGui::CloseCurrentPopup();
        }
        closeOnEscape();
        ImGui::EndPopup();
    }

    // --- download destination (no native dialog) ----------------------------
    centerNext();
    if (ImGui::BeginPopupModal(kDownloadToPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Download %s to a folder on this computer",
                    itemCount(pendingDownload_.size()).c_str());
        if (!downloadDialogError_.empty()) {
            ImGui::TextColored(palette.warning, "The system file dialog failed: %s",
                               downloadDialogError_.c_str());
        }

        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(460.0F);
        const bool entered = ImGui::InputText("##downloadto", &downloadPathBuffer_,
                                              ImGuiInputTextFlags_EnterReturnsTrue);

        const std::filesystem::path folder{std::string{trim(downloadPathBuffer_)}};
        std::error_code ec;
        const bool empty = trim(downloadPathBuffer_).empty();
        const bool isFile = !empty && std::filesystem::is_regular_file(folder, ec);
        if (isFile) {
            ImGui::TextColored(palette.bad, "that is a file, not a folder");
        } else if (!empty && !std::filesystem::exists(folder, ec)) {
            ImGui::TextDisabled("will be created");
        }

        const bool valid = !empty && !isFile;
        ImGui::BeginDisabled(!valid);
        const bool confirmed =
            ImGui::Button("Download", ImVec2{110.0F, 0.0F}) || (entered && valid);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Workspace folder")) {
            downloadPathBuffer_ = (context.workspaceDir / "pulled").string();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2{110.0F, 0.0F})) {
            pendingDownload_.clear();
            ImGui::CloseCurrentPopup();
        }

        if (confirmed) {
            downloadInto(context, std::move(pendingDownload_), folder);
            pendingDownload_.clear();
            ImGui::CloseCurrentPopup();
        }
        closeOnEscape();
        ImGui::EndPopup();
    }

    // --- upload confirmation (OS drop) ---------------------------------------
    // Confirmed rather than immediate: a drop is easy to make by accident, and
    // the folder it lands in is whatever happened to be open.
    centerNext();
    if (ImGui::BeginPopupModal(kUploadPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Upload %s to", itemCount(pendingUpload_.size()).c_str());
        ImGui::TextColored(palette.accent, "%s", currentDir().c_str());
        ImGui::Spacing();

        constexpr std::size_t kShown = 8;
        for (std::size_t i = 0; i < pendingUpload_.size() && i < kShown; ++i) {
            std::error_code ec;
            const bool isDir = std::filesystem::is_directory(pendingUpload_[i], ec);
            ImGui::BulletText("%s%s", pendingUpload_[i].filename().string().c_str(),
                              isDir ? "/  (whole folder)" : "");
        }
        if (pendingUpload_.size() > kShown) {
            ImGui::TextDisabled("...and %zu more", pendingUpload_.size() - kShown);
        }
        ImGui::TextDisabled("Existing names are kept; uploads are renamed \"name copy\".");
        ImGui::Spacing();

        if (ImGui::Button("Upload", ImVec2{110.0F, 0.0F})) {
            context.files.push(context.pool, context.dispatcher, std::move(pendingUpload_),
                               currentDir(), reloadAfter(context));
            pendingUpload_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2{110.0F, 0.0F})) {
            pendingUpload_.clear();
            ImGui::CloseCurrentPopup();
        }
        closeOnEscape();
        ImGui::EndPopup();
    }

    // --- upload from a typed path --------------------------------------------
    centerNext();
    if (ImGui::BeginPopupModal(kUploadPathPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Upload into %s", currentDir().c_str());
        ImGui::TextDisabled(
            "A file or folder on this computer. Dragging it onto the window "
            "works too.");

        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(460.0F);
        const bool entered =
            ImGui::InputTextWithHint("##localpath", "/path/on/this/computer",
                                     &uploadPathBuffer_, ImGuiInputTextFlags_EnterReturnsTrue);

        std::error_code ec;
        const std::filesystem::path local{std::string{trim(uploadPathBuffer_)}};
        const bool exists = !uploadPathBuffer_.empty() && std::filesystem::exists(local, ec);
        if (!uploadPathBuffer_.empty() && !exists) {
            ImGui::TextColored(palette.bad, "not found on this computer");
        }

        ImGui::BeginDisabled(!exists);
        const bool confirmed =
            ImGui::Button("Upload", ImVec2{110.0F, 0.0F}) || (entered && exists);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2{110.0F, 0.0F})) ImGui::CloseCurrentPopup();

        if (confirmed) {
            context.files.push(context.pool, context.dispatcher, {local}, currentDir(),
                               reloadAfter(context));
            ImGui::CloseCurrentPopup();
        }
        closeOnEscape();
        ImGui::EndPopup();
    }
}

void FileExplorerPanel::draw(AppContext& context) {
    // Before Begin(): a drop has to be claimed even while this panel is a
    // background tab, or it would be logged as ignored.
    acceptOsDrops(context);

    if (!ImGui::Begin(windowTitle(), visibleFlag())) {
        ImGui::End();
        return;
    }

    if (!context.hasDevice()) {
        ImGui::TextDisabled("No device selected.");
        ImGui::End();
        return;
    }

    if (!initialised_) {
        initialised_ = true;
        navigate(context, pathInput_, false);
    }

    drawNavigationBar(context);
    drawActionBar(context);
    drawStatus(context);
    ImGui::Separator();
    drawEntries(context);

    handleShortcuts(context);
    drawModals(context);

    ImGui::End();
}

}  // namespace em
