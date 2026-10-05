#pragma once

// Device file explorer over `adb shell ls` plus `adb pull` / `adb push`.
//
// Listing uses the device's own ls rather than adb's sync protocol so that
// permissions, owners, symlink targets and SELinux-denied directories are all
// visible -- the information a developer actually wants when poking at
// /data/data or /sdcard. File transfer still goes through adb pull/push.
//
// Copy, move, delete and rename run on the device with cp/mv/rm, batched into
// one `adb shell` per operation however many items are involved. Every batch
// is planned on the host first (planTransfer), which is where the decisions
// that can lose data are made -- name collisions, moving a folder into itself
// -- so those are pure functions with tests rather than shell logic.
//
// Nothing here overwrites. A name collision on paste, move, upload or download
// produces "name copy", "name copy 2", ... exactly as a desktop file manager
// does. Rename is the one exception in intent -- the user typed that exact
// name -- so it refuses instead of renaming the rename.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "adb/DeviceManager.h"
#include "core/TaskQueue.h"

namespace em {

enum class FileKind : std::uint8_t { Regular, Directory, Symlink, Block, Character, Fifo, Socket, Unknown };

struct FileEntry {
    std::string name;
    FileKind kind = FileKind::Unknown;
    std::uint64_t size = 0;
    std::string permissions;
    std::string owner;
    std::string group;
    std::string modified;
    std::string linkTarget;

    [[nodiscard]] bool isDirectory() const noexcept { return kind == FileKind::Directory; }
    [[nodiscard]] bool isSymlink() const noexcept { return kind == FileKind::Symlink; }
    [[nodiscard]] bool executable() const noexcept {
        return permissions.size() >= 10 &&
               (permissions[3] == 'x' || permissions[6] == 'x' || permissions[9] == 'x');
    }
};

struct DirListing {
    std::string path;
    std::vector<FileEntry> entries;
    std::string error;       // empty on success
    bool permissionDenied = false;
    bool loading = false;

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

struct TransferStatus {
    enum class State : std::uint8_t { Idle, Running, Done, Failed };
    State state = State::Idle;
    std::string description;
    std::string message;
    std::filesystem::path localPath;

    // One line per item that failed or was skipped, for the status tooltip.
    std::vector<std::string> details;
};

// A device path plus whether it is a directory. The kind decides how a copy is
// renamed on collision ("a copy.txt" vs "folder copy") and whether rm needs -r.
struct FileRef {
    std::string path;
    bool isDirectory = false;
};

enum class TransferMode : std::uint8_t { Copy, Move };

struct PlannedOp {
    TransferMode mode = TransferMode::Copy;
    std::string source;
    std::string destination;
};

struct TransferPlan {
    std::vector<PlannedOp> ops;
    std::vector<std::string> skipped;  // human-readable, one per refused source
};

// Result of one cp/mv/rm, parsed from its output and trailing exit code.
struct OpOutcome {
    int exitCode = -1;
    std::string message;
    [[nodiscard]] bool ok() const noexcept { return exitCode == 0; }
};

// Device-side copies and uploads of a large tree can take minutes.
inline constexpr std::chrono::milliseconds kAdbTransferTimeout{600'000};

class FileSystemBrowser {
public:
    explicit FileSystemBrowser(DeviceManager& devices);

    // Async directory listing. `onDone` runs on the UI thread via `dispatcher`.
    void list(ThreadPool& pool, Dispatcher& dispatcher, std::string path,
              std::function<void(DirListing)> onDone);

    using Callback = std::function<void(TransferStatus)>;

    // Copy or move on the device, into `destDir`. Collisions are renamed, never
    // overwritten; a folder dropped into itself is skipped with a reason.
    void transfer(ThreadPool& pool, Dispatcher& dispatcher, std::vector<FileRef> sources,
                  std::string destDir, TransferMode mode, Callback onDone);

    // Renames in place. Refuses if the target name already exists.
    void rename(ThreadPool& pool, Dispatcher& dispatcher, std::string path,
                std::string newName, Callback onDone);

    void makeDirectory(ThreadPool& pool, Dispatcher& dispatcher, std::string parentDir,
                       std::string name, Callback onDone);

    // Destructive, so the panel always confirms first. Directories are removed
    // with their contents; "/" is refused outright.
    void remove(ThreadPool& pool, Dispatcher& dispatcher, std::vector<FileRef> targets,
                Callback onDone);

    // Host -> device. Folders are pushed recursively.
    void push(ThreadPool& pool, Dispatcher& dispatcher,
              std::vector<std::filesystem::path> localPaths, std::string destDir,
              Callback onDone);

    // Device -> host, into `localDir`, which is created if needed.
    void pull(ThreadPool& pool, Dispatcher& dispatcher, std::vector<FileRef> remotes,
              std::filesystem::path localDir, Callback onDone);

    // Device -> one exact host file, as chosen in a Save dialog. An existing
    // file there is replaced: every native Save dialog has already asked.
    void pullAs(ThreadPool& pool, Dispatcher& dispatcher, FileRef remote,
                std::filesystem::path localFile, Callback onDone);

    // Opens a host folder in Finder / Explorer / the desktop file manager. For
    // a file, opens its folder with the file selected where the OS supports it.
    static void revealOnHost(ThreadPool& pool, std::filesystem::path path);

    [[nodiscard]] std::size_t activeOperations() const noexcept {
        return active_.load(std::memory_order_acquire);
    }

    // Directories worth offering as shortcuts on a stock device.
    [[nodiscard]] static std::vector<std::string> bookmarkPaths();

private:
    // Lists the names in a device directory, for collision planning. Returns an
    // empty set for an unreadable directory; the ops themselves then report
    // the real error.
    [[nodiscard]] std::set<std::string> deviceNames(std::string_view serial,
                                                    std::string_view dir);

    void finish(Dispatcher& dispatcher, Callback onDone, TransferStatus status);

    DeviceManager& devices_;
    std::atomic<std::size_t> active_{0};
};

// --- parsers, exposed for unit tests ---------------------------------------
[[nodiscard]] std::vector<FileEntry> parseLsLongOutput(std::string_view text);
[[nodiscard]] FileKind fileKindFromPermissions(std::string_view permissions) noexcept;

// Joins a directory and a child name, collapsing ".." and keeping POSIX form.
[[nodiscard]] std::string joinDevicePath(std::string_view base, std::string_view child);
[[nodiscard]] std::string parentDevicePath(std::string_view path);

// --- planning, exposed for unit tests ---------------------------------------

// Strips trailing slashes; "/" stays "/".
[[nodiscard]] std::string normalizeDevicePath(std::string_view path);

// Last path component; empty for "/".
[[nodiscard]] std::string deviceBaseName(std::string_view path);

// True when `path` is `ancestor` or lies anywhere beneath it.
[[nodiscard]] bool isSameOrDescendant(std::string_view ancestor, std::string_view path);

// nullopt when `name` is usable as a single path component, else the reason.
[[nodiscard]] std::optional<std::string> validateFileName(std::string_view name);

// `name` if free, otherwise "stem copy.ext", "stem copy 2.ext", ... The
// extension is kept for files only, and an existing " copy"/" copy N" suffix
// is folded in, so copying "a copy" yields "a copy 2", not "a copy copy".
[[nodiscard]] std::string uniqueCopyName(std::string_view name,
                                         const std::set<std::string>& taken,
                                         bool isDirectory);

// Decides every destination name up front. `taken` is what already exists in
// `destDir`; names assigned within the batch are added to it as it goes.
[[nodiscard]] TransferPlan planTransfer(const std::vector<FileRef>& sources,
                                        std::string_view destDir,
                                        std::set<std::string> taken, TransferMode mode);

// Device paths rm must never be pointed at, however the selection arrived.
[[nodiscard]] bool isDeletionAllowed(std::string_view path);

// Where a download dialog should open: the first candidate that is an existing
// directory, else the last candidate (which the caller makes sure exists).
// Typically: the last folder used, ~/Downloads, then <workspace>/pulled.
[[nodiscard]] std::filesystem::path resolveDownloadStart(
    const std::vector<std::filesystem::path>& candidates);

[[nodiscard]] std::vector<std::string> parseLsNames(std::string_view text);

// The `ls` used to list a directory; follows a symlinked directory itself.
[[nodiscard]] std::string buildListCommand(std::string_view path);

// Each command ends by echoing "@@RC <exit code>" so a batch reports per item.
[[nodiscard]] std::string buildOpCommand(const PlannedOp& op);
[[nodiscard]] std::string buildRemoveCommand(const FileRef& target);
[[nodiscard]] OpOutcome parseOpOutput(std::string_view text);

// Collapses per-item outcomes into one status line plus per-item details.
[[nodiscard]] TransferStatus summarizeOutcomes(std::string_view verbPast,
                                               const std::vector<std::string>& labels,
                                               const std::vector<OpOutcome>& outcomes,
                                               const std::vector<std::string>& skipped);

}  // namespace em
