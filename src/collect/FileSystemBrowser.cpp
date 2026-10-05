#include "collect/FileSystemBrowser.h"

#include <algorithm>
#include <chrono>
#include <system_error>
#include <utility>

#include "core/Log.h"
#include "core/StringUtil.h"
#include "core/Subprocess.h"

namespace em {
namespace {

constexpr const char* kCategory = "files";

bool looksLikePermissions(std::string_view token) noexcept {
    if (token.size() < 10) return false;
    static constexpr std::string_view kTypes = "-dlbcps";
    return kTypes.find(token.front()) != std::string_view::npos;
}

}  // namespace

FileKind fileKindFromPermissions(std::string_view permissions) noexcept {
    if (permissions.empty()) return FileKind::Unknown;
    switch (permissions.front()) {
        case '-': return FileKind::Regular;
        case 'd': return FileKind::Directory;
        case 'l': return FileKind::Symlink;
        case 'b': return FileKind::Block;
        case 'c': return FileKind::Character;
        case 'p': return FileKind::Fifo;
        case 's': return FileKind::Socket;
        default:  return FileKind::Unknown;
    }
}

// Parses toybox/coreutils `ls -la` output:
//   drwxrwx--x   4 system system      4096 2024-05-01 09:12 data
//   -rw-r--r--   1 root   root         512 2024-05-01 09:12 build.prop
//   lrwxrwxrwx   1 root   root          21 2024-05-01 09:12 sdcard -> /storage/self/primary
//   crw-rw-rw-   1 root   root     1,   3  2024-05-01 09:12 null
std::vector<FileEntry> parseLsLongOutput(std::string_view text) {
    std::vector<FileEntry> entries;

    for (const auto rawLine : splitLines(text)) {
        const std::string_view line = trimRight(rawLine);
        if (line.empty()) continue;
        if (line.starts_with("total ")) continue;

        const auto tokens = tokenize(line);
        if (tokens.size() < 6 || !looksLikePermissions(tokens[0])) continue;

        FileEntry entry;
        entry.permissions = std::string{tokens[0]};
        entry.kind = fileKindFromPermissions(tokens[0]);
        entry.owner = std::string{tokens[2]};
        entry.group = std::string{tokens[3]};

        // Device nodes report "major, minor" where a size would be, which
        // shifts every later field by one.
        std::size_t cursor = 4;
        const bool isDeviceNode =
            entry.kind == FileKind::Block || entry.kind == FileKind::Character;
        if (isDeviceNode) {
            cursor = 6;
        } else {
            if (const auto size = parseNumber<std::uint64_t>(tokens[4])) entry.size = *size;
            cursor = 5;
        }

        // Date and time: either "2024-05-01 09:12" or "May  1 09:12".
        std::string modified;
        std::size_t dateFields = 0;
        while (cursor < tokens.size() && dateFields < 3) {
            const std::string_view token = tokens[cursor];
            const bool isTime = token.find(':') != std::string_view::npos;
            const bool isDate = token.find('-') != std::string_view::npos &&
                                parseNumber<int>(token.substr(0, 4)).has_value();
            const bool isNumeric = parseNumber<int>(token).has_value();
            const bool isMonthName = token.size() == 3 && !isNumeric;

            if (!isTime && !isDate && !isNumeric && !isMonthName) break;
            if (!modified.empty()) modified.push_back(' ');
            modified.append(token);
            ++cursor;
            ++dateFields;
            if (isTime) break;  // the time is always the last date field
        }
        entry.modified = std::move(modified);

        if (cursor >= tokens.size()) continue;

        // The name is the remainder of the line, so names with spaces survive.
        const auto nameOffset = static_cast<std::size_t>(tokens[cursor].data() - line.data());
        std::string_view name = line.substr(nameOffset);

        if (entry.kind == FileKind::Symlink) {
            const auto arrow = name.find(" -> ");
            if (arrow != std::string_view::npos) {
                entry.linkTarget = std::string{name.substr(arrow + 4)};
                name = name.substr(0, arrow);
            }
        }
        entry.name = std::string{name};
        if (entry.name.empty() || entry.name == "." || entry.name == "..") continue;

        entries.push_back(std::move(entry));
    }

    std::sort(entries.begin(), entries.end(), [](const FileEntry& a, const FileEntry& b) {
        // Directories first, then case-insensitive by name.
        if (a.isDirectory() != b.isDirectory()) return a.isDirectory();
        return toLower(a.name) < toLower(b.name);
    });
    return entries;
}

std::string joinDevicePath(std::string_view base, std::string_view child) {
    if (child.starts_with('/')) return std::string{child};
    if (child == ".") return std::string{base};
    if (child == "..") return parentDevicePath(base);

    std::string out{base};
    if (out.empty()) out = "/";
    if (out.back() != '/') out.push_back('/');
    out.append(child);
    return out;
}

std::string parentDevicePath(std::string_view path) {
    std::string_view trimmed = path;
    while (trimmed.size() > 1 && trimmed.back() == '/') trimmed.remove_suffix(1);
    const auto slash = trimmed.rfind('/');
    if (slash == std::string_view::npos || slash == 0) return "/";
    return std::string{trimmed.substr(0, slash)};
}

std::vector<std::string> FileSystemBrowser::bookmarkPaths() {
    return {
        "/",
        "/sdcard",
        "/sdcard/Download",
        "/sdcard/Android/data",
        "/data/local/tmp",
        "/data/misc/perfetto-traces",
        "/system",
        "/system/bin",
        "/proc",
        "/sys/class/thermal",
        "/vendor",
    };
}

FileSystemBrowser::FileSystemBrowser(DeviceManager& devices) : devices_(devices) {}

void FileSystemBrowser::list(ThreadPool& pool, Dispatcher& dispatcher, std::string path,
                             std::function<void(DirListing)> onDone) {
    const std::string serial = devices_.selectedSerial();
    if (serial.empty()) {
        DirListing listing;
        listing.path = std::move(path);
        listing.error = "no device selected";
        dispatcher.post([cb = std::move(onDone), l = std::move(listing)] { cb(l); });
        return;
    }

    active_.fetch_add(1, std::memory_order_acq_rel);
    pool.submit([this, &dispatcher, serial, path = std::move(path),
                 onDone = std::move(onDone)]() mutable {
        DirListing listing;
        listing.path = path;

        // -A hides . and .. but keeps dotfiles; -L is deliberately omitted so
        // symlinks show as links rather than their targets.
        const std::string cmd = buildListCommand(path);
        auto output = devices_.adb().shellRaw(serial, cmd, kAdbNormalTimeout);

        if (!output) {
            listing.error = output.error().message;
        } else {
            const CommandOutput result = std::move(output).value();
            const std::string combined = result.out + result.err;

            if (containsIgnoreCase(combined, "permission denied")) {
                listing.permissionDenied = true;
                listing.error = "permission denied (try `adb root` on a userdebug build)";
            } else if (containsIgnoreCase(combined, "no such file or directory")) {
                listing.error = "no such file or directory";
            } else {
                listing.entries = parseLsLongOutput(result.out);
                if (listing.entries.empty() && result.exitCode != 0) {
                    listing.error = result.err.empty() ? "ls failed"
                                                       : std::string{result.firstErrLine()};
                }
            }
        }

        active_.fetch_sub(1, std::memory_order_acq_rel);
        dispatcher.post([cb = std::move(onDone), l = std::move(listing)] { cb(l); });
    });
}

// ---------------------------------------------------------------------------
// Planning helpers
// ---------------------------------------------------------------------------

namespace {

constexpr std::string_view kRcMarker = "@@RC ";

// Strips a trailing " copy" or " copy N" so repeated copies count up instead of
// stacking ("a copy copy").
std::string_view stripCopySuffix(std::string_view stem) {
    constexpr std::string_view kCopy = " copy";
    if (stem.ends_with(kCopy)) return stem.substr(0, stem.size() - kCopy.size());

    const auto space = stem.rfind(' ');
    if (space == std::string_view::npos || space + 1 >= stem.size()) return stem;
    const std::string_view digits = stem.substr(space + 1);
    for (const char c : digits) {
        if (c < '0' || c > '9') return stem;
    }
    const std::string_view head = stem.substr(0, space);
    if (head.ends_with(kCopy)) return head.substr(0, head.size() - kCopy.size());
    return stem;
}

std::string hostName(const std::filesystem::path& path) {
    // "dir/" has an empty filename(); fall back to the last real component.
    auto name = path.filename();
    if (name.empty()) name = path.parent_path().filename();
    return name.string();
}

std::set<std::string> hostNames(const std::filesystem::path& dir) {
    std::set<std::string> names;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator{dir, ec}) {
        names.insert(entry.path().filename().string());
    }
    return names;
}

}  // namespace

std::string normalizeDevicePath(std::string_view path) {
    std::string out{trim(path)};
    while (out.size() > 1 && out.back() == '/') out.pop_back();
    if (out.empty()) out = "/";
    return out;
}

std::string deviceBaseName(std::string_view path) {
    const std::string normalized = normalizeDevicePath(path);
    if (normalized == "/") return {};
    const auto slash = normalized.rfind('/');
    return slash == std::string::npos ? normalized : normalized.substr(slash + 1);
}

bool isSameOrDescendant(std::string_view ancestor, std::string_view path) {
    const std::string a = normalizeDevicePath(ancestor);
    const std::string p = normalizeDevicePath(path);
    if (a == p) return true;
    if (a == "/") return p.starts_with('/');
    return p.size() > a.size() && p.starts_with(a) && p[a.size()] == '/';
}

std::optional<std::string> validateFileName(std::string_view name) {
    if (trim(name).empty()) return std::string{"name is empty"};
    if (name == "." || name == "..") return std::string{"\".\" and \"..\" are reserved"};
    if (name.find('/') != std::string_view::npos) return std::string{"name cannot contain /"};
    if (name.find('\0') != std::string_view::npos) return std::string{"name contains a NUL byte"};
    if (name.size() > 255) return std::string{"name is longer than 255 bytes"};
    return std::nullopt;
}

std::string uniqueCopyName(std::string_view name, const std::set<std::string>& taken,
                           bool isDirectory) {
    if (!taken.contains(std::string{name})) return std::string{name};

    // Keep the extension on files so "photo.jpg" becomes "photo copy.jpg" and
    // still opens. A leading dot is a hidden-file marker, not an extension.
    std::string_view stem = name;
    std::string_view extension;
    if (!isDirectory) {
        const auto dot = name.rfind('.');
        if (dot != std::string_view::npos && dot > 0 && dot + 1 < name.size()) {
            stem = name.substr(0, dot);
            extension = name.substr(dot);
        }
    }
    const std::string base{stripCopySuffix(stem)};

    for (int n = 1; n < 100'000; ++n) {
        std::string candidate = base + (n == 1 ? " copy" : " copy " + std::to_string(n));
        candidate += extension;
        if (!taken.contains(candidate)) return candidate;
    }
    // Unreachable in practice; fall back to something still non-destructive.
    return base + " copy " + std::to_string(std::chrono::steady_clock::now()
                                                .time_since_epoch()
                                                .count()) +
           std::string{extension};
}

TransferPlan planTransfer(const std::vector<FileRef>& sources, std::string_view destDir,
                          std::set<std::string> taken, TransferMode mode) {
    TransferPlan plan;
    const std::string dest = normalizeDevicePath(destDir);

    for (const auto& source : sources) {
        const std::string path = normalizeDevicePath(source.path);
        const std::string name = deviceBaseName(path);

        if (name.empty()) {
            plan.skipped.push_back("/: the root cannot be copied or moved");
            continue;
        }
        // Covers dropping a folder onto itself as well as into a subfolder of
        // itself. cp -r would recurse forever; mv would fail half-way.
        if (isSameOrDescendant(path, dest)) {
            plan.skipped.push_back(name + ": cannot put a folder inside itself");
            continue;
        }
        if (mode == TransferMode::Move && parentDevicePath(path) == dest) {
            plan.skipped.push_back(name + ": already in " + dest);
            continue;
        }

        const std::string target = uniqueCopyName(name, taken, source.isDirectory);
        taken.insert(target);
        plan.ops.push_back(PlannedOp{mode, path, joinDevicePath(dest, target)});
    }
    return plan;
}

bool isDeletionAllowed(std::string_view path) {
    const std::string normalized = normalizeDevicePath(path);
    return normalized != "/" && normalized.starts_with('/');
}

std::filesystem::path resolveDownloadStart(const std::vector<std::filesystem::path>& candidates) {
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (!candidate.empty() && std::filesystem::is_directory(candidate, ec)) return candidate;
    }
    return candidates.empty() ? std::filesystem::path{} : candidates.back();
}

std::string buildListCommand(std::string_view path) {
    // The trailing slash matters: on a real device /sdcard is a symlink, and
    // `ls -l /sdcard` describes the link itself -- the explorer showed a single
    // "/sdcard" entry instead of its contents. With the slash ls follows it.
    std::string dir{path};
    if (dir.empty() || dir.back() != '/') dir.push_back('/');
    // -A hides . and .. but keeps dotfiles; no -L, so links inside the folder
    // still show as links rather than as their targets.
    return "ls -lA -- " + shellQuote(dir);
}

std::vector<std::string> parseLsNames(std::string_view text) {
    std::vector<std::string> names;
    for (const auto line : splitLines(text)) {
        const std::string_view name = trimRight(line);
        if (name.empty() || name == "." || name == "..") continue;
        names.emplace_back(name);
    }
    return names;
}

std::string buildOpCommand(const PlannedOp& op) {
    // `--` so a name starting with '-' is not read as an option; 2>&1 so the
    // error text survives AdbClient::shellBatch discarding stderr.
    const std::string verb = op.mode == TransferMode::Copy ? "cp -r -- " : "mv -- ";
    return verb + shellQuote(op.source) + " " + shellQuote(op.destination) +
           " 2>&1; echo \"" + std::string{kRcMarker} + "$?\"";
}

std::string buildRemoveCommand(const FileRef& target) {
    const std::string verb = target.isDirectory ? "rm -rf -- " : "rm -f -- ";
    return verb + shellQuote(target.path) + " 2>&1; echo \"" + std::string{kRcMarker} +
           "$?\"";
}

OpOutcome parseOpOutput(std::string_view text) {
    OpOutcome outcome;
    const auto marker = text.rfind(kRcMarker);
    if (marker == std::string_view::npos) {
        outcome.exitCode = -1;
        const std::string_view body = trim(text);
        outcome.message = body.empty() ? "no response from the device" : std::string{body};
        return outcome;
    }

    const auto code = parseNumber<int>(text.substr(marker + kRcMarker.size()));
    outcome.exitCode = code.value_or(-1);

    // Keep only the first line of the tool's message: toybox prefixes it with
    // "cp: " / "mv: " and that line says everything.
    const std::string_view body = trim(text.substr(0, marker));
    const auto newline = body.find('\n');
    outcome.message = std::string{newline == std::string_view::npos ? body
                                                                    : trim(body.substr(0, newline))};
    if (!outcome.ok() && outcome.message.empty()) {
        outcome.message = "exit code " + std::to_string(outcome.exitCode);
    }
    return outcome;
}

TransferStatus summarizeOutcomes(std::string_view verbPast,
                                 const std::vector<std::string>& labels,
                                 const std::vector<OpOutcome>& outcomes,
                                 const std::vector<std::string>& skipped) {
    TransferStatus status;
    std::size_t succeeded = 0;

    for (std::size_t i = 0; i < outcomes.size(); ++i) {
        if (outcomes[i].ok()) {
            ++succeeded;
            continue;
        }
        const std::string label = i < labels.size() ? labels[i] : "item " + std::to_string(i);
        status.details.push_back(label + ": " + outcomes[i].message);
    }
    for (const auto& reason : skipped) status.details.push_back("skipped " + reason);

    const std::size_t failed = outcomes.size() - succeeded;
    const auto plural = [](std::size_t n) { return n == 1 ? std::string{" item"} : std::string{" items"}; };

    if (outcomes.empty() && !skipped.empty()) {
        status.state = TransferStatus::State::Failed;
        status.message = "nothing to do: " + skipped.front();
    } else if (failed == 0) {
        status.state = TransferStatus::State::Done;
        status.message = std::string{verbPast} + " " + std::to_string(succeeded) + plural(succeeded);
        if (!skipped.empty()) status.message += ", " + std::to_string(skipped.size()) + " skipped";
    } else {
        status.state = TransferStatus::State::Failed;
        status.message = std::string{verbPast} + " " + std::to_string(succeeded) + " of " +
                         std::to_string(outcomes.size()) + ", " + std::to_string(failed) +
                         " failed";
        // Surface the first reason inline; the rest are in `details`.
        status.message += " (" + status.details.front() + ")";
    }
    return status;
}

// ---------------------------------------------------------------------------
// Operations
// ---------------------------------------------------------------------------

std::set<std::string> FileSystemBrowser::deviceNames(std::string_view serial,
                                                     std::string_view dir) {
    auto output = devices_.adb().shell(serial, "ls -A1 -- " + shellQuote(dir), kAdbNormalTimeout);
    if (!output) return {};
    const auto names = parseLsNames(output.value());
    return {names.begin(), names.end()};
}

void FileSystemBrowser::finish(Dispatcher& dispatcher, Callback onDone, TransferStatus status) {
    active_.fetch_sub(1, std::memory_order_acq_rel);
    if (status.state == TransferStatus::State::Failed) {
        EM_LOG_WARN(kCategory, status.description + ": " + status.message);
    } else {
        EM_LOG_INFO(kCategory, status.description + ": " + status.message);
    }
    dispatcher.post([cb = std::move(onDone), s = std::move(status)] { cb(s); });
}

void FileSystemBrowser::transfer(ThreadPool& pool, Dispatcher& dispatcher,
                                 std::vector<FileRef> sources, std::string destDir,
                                 TransferMode mode, Callback onDone) {
    const std::string serial = devices_.selectedSerial();
    active_.fetch_add(1, std::memory_order_acq_rel);

    pool.submit([this, &dispatcher, serial, sources = std::move(sources),
                 destDir = std::move(destDir), mode, onDone = std::move(onDone)]() mutable {
        const char* verb = mode == TransferMode::Copy ? "copied" : "moved";
        TransferStatus status;

        if (serial.empty()) {
            status.state = TransferStatus::State::Failed;
            status.message = "no device selected";
        } else {
            const TransferPlan plan =
                planTransfer(sources, destDir, deviceNames(serial, destDir), mode);

            std::vector<std::string> commands;
            std::vector<std::string> labels;
            for (const auto& op : plan.ops) {
                commands.push_back(buildOpCommand(op));
                labels.push_back(deviceBaseName(op.source));
            }

            std::vector<OpOutcome> outcomes;
            if (!commands.empty()) {
                auto batch = devices_.adb().shellBatch(serial, commands, kAdbTransferTimeout);
                for (std::size_t i = 0; i < commands.size(); ++i) {
                    outcomes.push_back(batch ? parseOpOutput(batch.value()[i])
                                             : OpOutcome{-1, batch.error().message});
                }
            }
            status = summarizeOutcomes(verb, labels, outcomes, plan.skipped);
        }

        status.description = std::string{mode == TransferMode::Copy ? "copy" : "move"} +
                             " to " + normalizeDevicePath(destDir);
        finish(dispatcher, std::move(onDone), std::move(status));
    });
}

void FileSystemBrowser::rename(ThreadPool& pool, Dispatcher& dispatcher, std::string path,
                               std::string newName, Callback onDone) {
    const std::string serial = devices_.selectedSerial();
    active_.fetch_add(1, std::memory_order_acq_rel);

    pool.submit([this, &dispatcher, serial, path = std::move(path),
                 newName = std::move(newName), onDone = std::move(onDone)]() mutable {
        TransferStatus status;
        status.description = "rename " + deviceBaseName(path);

        if (serial.empty()) {
            status.state = TransferStatus::State::Failed;
            status.message = "no device selected";
        } else if (const auto problem = validateFileName(newName)) {
            status.state = TransferStatus::State::Failed;
            status.message = *problem;
        } else {
            const std::string target = joinDevicePath(parentDevicePath(path), newName);
            // The existence test and the mv run in one shell so nothing can
            // appear at the target in between. Exit 17 is EEXIST.
            const std::string cmd = "if [ -e " + shellQuote(target) + " ] || [ -L " +
                                    shellQuote(target) + " ]; then echo \"" + newName +
                                    " already exists\"; echo \"" + std::string{kRcMarker} +
                                    "17\"; else mv -- " + shellQuote(path) + " " +
                                    shellQuote(target) + " 2>&1; echo \"" +
                                    std::string{kRcMarker} + "$?\"; fi";
            auto output = devices_.adb().shellRaw(serial, cmd, kAdbNormalTimeout);
            const OpOutcome outcome = output ? parseOpOutput(output.value().out)
                                             : OpOutcome{-1, output.error().message};
            status.state = outcome.ok() ? TransferStatus::State::Done
                                        : TransferStatus::State::Failed;
            status.message = outcome.ok() ? "renamed to " + newName : outcome.message;
        }
        finish(dispatcher, std::move(onDone), std::move(status));
    });
}

void FileSystemBrowser::makeDirectory(ThreadPool& pool, Dispatcher& dispatcher,
                                      std::string parentDir, std::string name,
                                      Callback onDone) {
    const std::string serial = devices_.selectedSerial();
    active_.fetch_add(1, std::memory_order_acq_rel);

    pool.submit([this, &dispatcher, serial, parentDir = std::move(parentDir),
                 name = std::move(name), onDone = std::move(onDone)]() mutable {
        TransferStatus status;
        status.description = "new folder " + name;

        if (serial.empty()) {
            status.state = TransferStatus::State::Failed;
            status.message = "no device selected";
        } else if (const auto problem = validateFileName(name)) {
            status.state = TransferStatus::State::Failed;
            status.message = *problem;
        } else {
            // No -p: an existing name should be reported, not silently reused.
            const std::string cmd = "mkdir -- " + shellQuote(joinDevicePath(parentDir, name)) +
                                    " 2>&1; echo \"" + std::string{kRcMarker} + "$?\"";
            auto output = devices_.adb().shellRaw(serial, cmd, kAdbNormalTimeout);
            const OpOutcome outcome = output ? parseOpOutput(output.value().out)
                                             : OpOutcome{-1, output.error().message};
            status.state = outcome.ok() ? TransferStatus::State::Done
                                        : TransferStatus::State::Failed;
            status.message = outcome.ok() ? "created" : outcome.message;
        }
        finish(dispatcher, std::move(onDone), std::move(status));
    });
}

void FileSystemBrowser::remove(ThreadPool& pool, Dispatcher& dispatcher,
                               std::vector<FileRef> targets, Callback onDone) {
    const std::string serial = devices_.selectedSerial();
    active_.fetch_add(1, std::memory_order_acq_rel);

    pool.submit([this, &dispatcher, serial, targets = std::move(targets),
                 onDone = std::move(onDone)]() mutable {
        TransferStatus status;

        if (serial.empty()) {
            status.state = TransferStatus::State::Failed;
            status.message = "no device selected";
        } else {
            std::vector<std::string> commands;
            std::vector<std::string> labels;
            std::vector<std::string> skipped;
            for (const auto& target : targets) {
                if (!isDeletionAllowed(target.path)) {
                    skipped.push_back(target.path + ": refusing to delete this path");
                    continue;
                }
                commands.push_back(buildRemoveCommand(target));
                labels.push_back(deviceBaseName(target.path));
            }

            std::vector<OpOutcome> outcomes;
            if (!commands.empty()) {
                auto batch = devices_.adb().shellBatch(serial, commands, kAdbTransferTimeout);
                for (std::size_t i = 0; i < commands.size(); ++i) {
                    outcomes.push_back(batch ? parseOpOutput(batch.value()[i])
                                             : OpOutcome{-1, batch.error().message});
                }
            }
            status = summarizeOutcomes("deleted", labels, outcomes, skipped);
        }
        status.description = "delete";
        finish(dispatcher, std::move(onDone), std::move(status));
    });
}

void FileSystemBrowser::push(ThreadPool& pool, Dispatcher& dispatcher,
                             std::vector<std::filesystem::path> localPaths, std::string destDir,
                             Callback onDone) {
    const std::string serial = devices_.selectedSerial();
    active_.fetch_add(1, std::memory_order_acq_rel);

    pool.submit([this, &dispatcher, serial, localPaths = std::move(localPaths),
                 destDir = std::move(destDir), onDone = std::move(onDone)]() mutable {
        TransferStatus status;

        if (serial.empty()) {
            status.state = TransferStatus::State::Failed;
            status.message = "no device selected";
        } else {
            std::set<std::string> taken = deviceNames(serial, destDir);
            std::vector<std::string> labels;
            std::vector<OpOutcome> outcomes;

            for (const auto& local : localPaths) {
                const std::string name = hostName(local);
                labels.push_back(name);

                std::error_code ec;
                if (!std::filesystem::exists(local, ec)) {
                    outcomes.push_back(OpOutcome{-1, "not found on this computer"});
                    continue;
                }
                const bool isDir = std::filesystem::is_directory(local, ec);
                const std::string target = uniqueCopyName(name, taken, isDir);
                taken.insert(target);

                auto result = devices_.adb().push(serial, local, joinDevicePath(destDir, target),
                                                  kAdbTransferTimeout);
                outcomes.push_back(result ? OpOutcome{0, {}}
                                          : OpOutcome{-1, result.error().message});
            }
            status = summarizeOutcomes("uploaded", labels, outcomes, {});
        }
        status.description = "upload to " + normalizeDevicePath(destDir);
        finish(dispatcher, std::move(onDone), std::move(status));
    });
}

void FileSystemBrowser::pull(ThreadPool& pool, Dispatcher& dispatcher,
                             std::vector<FileRef> remotes, std::filesystem::path localDir,
                             Callback onDone) {
    const std::string serial = devices_.selectedSerial();
    active_.fetch_add(1, std::memory_order_acq_rel);

    pool.submit([this, &dispatcher, serial, remotes = std::move(remotes),
                 localDir = std::move(localDir), onDone = std::move(onDone)]() mutable {
        TransferStatus status;

        if (serial.empty()) {
            status.state = TransferStatus::State::Failed;
            status.message = "no device selected";
        } else {
            std::error_code ec;
            std::filesystem::create_directories(localDir, ec);

            std::set<std::string> taken = hostNames(localDir);
            std::vector<std::string> labels;
            std::vector<OpOutcome> outcomes;

            for (const auto& remote : remotes) {
                const std::string name = deviceBaseName(remote.path);
                labels.push_back(name);

                const std::string target = uniqueCopyName(name, taken, remote.isDirectory);
                taken.insert(target);

                auto result = devices_.adb().pull(serial, remote.path, localDir / target,
                                                  kAdbTransferTimeout);
                outcomes.push_back(result ? OpOutcome{0, {}}
                                          : OpOutcome{-1, result.error().message});
            }
            status = summarizeOutcomes("downloaded", labels, outcomes, {});
        }
        status.description = "download";
        status.localPath = localDir;
        finish(dispatcher, std::move(onDone), std::move(status));
    });
}

void FileSystemBrowser::pullAs(ThreadPool& pool, Dispatcher& dispatcher, FileRef remote,
                               std::filesystem::path localFile, Callback onDone) {
    const std::string serial = devices_.selectedSerial();
    active_.fetch_add(1, std::memory_order_acq_rel);

    pool.submit([this, &dispatcher, serial, remote = std::move(remote),
                 localFile = std::move(localFile), onDone = std::move(onDone)]() mutable {
        TransferStatus status;
        status.description = "download";

        if (serial.empty()) {
            status.state = TransferStatus::State::Failed;
            status.message = "no device selected";
        } else {
            std::error_code ec;
            std::filesystem::create_directories(localFile.parent_path(), ec);
            auto result = devices_.adb().pull(serial, remote.path, localFile, kAdbTransferTimeout);
            if (result) {
                status.state = TransferStatus::State::Done;
                status.message = "saved " + localFile.filename().string();
                status.localPath = localFile;
            } else {
                status.state = TransferStatus::State::Failed;
                status.message = result.error().message;
            }
        }
        finish(dispatcher, std::move(onDone), std::move(status));
    });
}

void FileSystemBrowser::revealOnHost(ThreadPool& pool, std::filesystem::path path) {
    pool.submit([path = std::move(path)] {
        std::error_code ec;
        const bool isFile = std::filesystem::is_regular_file(path, ec);
#if defined(__APPLE__)
        std::vector<std::string> argv = isFile ? std::vector<std::string>{"open", "-R", path.string()}
                                               : std::vector<std::string>{"open", path.string()};
#elif defined(_WIN32)
        std::vector<std::string> argv =
            isFile ? std::vector<std::string>{"explorer", "/select," + path.string()}
                   : std::vector<std::string>{"explorer", path.string()};
#else
        // No portable "select this file"; open the folder that holds it.
        std::vector<std::string> argv{"xdg-open",
                                      (isFile ? path.parent_path() : path).string()};
#endif
        // Explorer exits 1 even on success, so the exit code is not checked.
        if (auto result = runCommand(std::move(argv), std::chrono::seconds{10}); !result) {
            EM_LOG_WARN(kCategory, "could not open " + path.string() + ": " + result.describe());
        }
    });
}

}  // namespace em
